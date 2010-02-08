/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdclass/linux/linux-module.c
 *
 * Object Devices Class Driver
 * These are the only exported functions, they provide some generic
 * infrastructure for managing object devices
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#ifdef __KERNEL__
#ifndef AUTOCONF_INCLUDED
#include <linux/config.h> /* for CONFIG_PROC_FS */
#endif
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/lp.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <asm/io.h>
#include <asm/ioctls.h>
#include <asm/system.h>
#include <asm/poll.h>
#include <asm/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/smp_lock.h>
#include <linux/seq_file.h>
#else
# include <liblustre.h>
#endif

#include <libcfs/libcfs.h>
#include <obd_support.h>
#include <obd_class.h>
#include <lprocfs_status.h>
#include <lustre_ver.h>
#include <lustre/lustre_build_version.h>
#ifdef __KERNEL__
#include <linux/lustre_version.h>

int proc_version;

/* buffer MUST be at least the size of obd_ioctl_hdr */
int obd_ioctl_getdata(char **buf, int *len, void *arg)
{
        struct obd_ioctl_hdr hdr;
        struct obd_ioctl_data *data;
        int err;
        int offset = 0;
        ENTRY;

        err = copy_from_user(&hdr, (void *)arg, sizeof(hdr));
        if ( err )
                RETURN(err);

        if (hdr.ioc_version != OBD_IOCTL_VERSION) {
                CERROR("Version mismatch kernel (%x) vs application (%x)\n",
                       OBD_IOCTL_VERSION, hdr.ioc_version);
                RETURN(-EINVAL);
        }

        if (hdr.ioc_len > OBD_MAX_IOCTL_BUFFER) {
                CERROR("User buffer len %d exceeds %d max buffer\n",
                       hdr.ioc_len, OBD_MAX_IOCTL_BUFFER);
                RETURN(-EINVAL);
        }

        if (hdr.ioc_len < sizeof(struct obd_ioctl_data)) {
                CERROR("User buffer too small for ioctl (%d)\n", hdr.ioc_len);
                RETURN(-EINVAL);
        }

        /* XXX allocate this more intelligently, using kmalloc when
         * appropriate */
        OBD_VMALLOC(*buf, hdr.ioc_len);
        if (*buf == NULL) {
                CERROR("Cannot allocate control buffer of len %d\n",
                       hdr.ioc_len);
                RETURN(-EINVAL);
        }
        *len = hdr.ioc_len;
        data = (struct obd_ioctl_data *)*buf;

        err = copy_from_user(*buf, (void *)arg, hdr.ioc_len);
        if ( err ) {
                OBD_VFREE(*buf, hdr.ioc_len);
                RETURN(err);
        }

        if (obd_ioctl_is_invalid(data)) {
                CERROR("ioctl not correctly formatted\n");
                OBD_VFREE(*buf, hdr.ioc_len);
                RETURN(-EINVAL);
        }

        if (data->ioc_inllen1) {
                data->ioc_inlbuf1 = &data->ioc_bulk[0];
                offset += size_round(data->ioc_inllen1);
        }

        if (data->ioc_inllen2) {
                data->ioc_inlbuf2 = &data->ioc_bulk[0] + offset;
                offset += size_round(data->ioc_inllen2);
        }

        if (data->ioc_inllen3) {
                data->ioc_inlbuf3 = &data->ioc_bulk[0] + offset;
                offset += size_round(data->ioc_inllen3);
        }

        if (data->ioc_inllen4) {
                data->ioc_inlbuf4 = &data->ioc_bulk[0] + offset;
        }

        EXIT;
        return 0;
}

int obd_ioctl_popdata(void *arg, void *data, int len)
{
        int err;

        err = copy_to_user(arg, data, len);
        if (err)
                err = -EFAULT;
        return err;
}

EXPORT_SYMBOL(obd_ioctl_getdata);
EXPORT_SYMBOL(obd_ioctl_popdata);

#define OBD_MINOR 241
extern struct cfs_psdev_ops          obd_psdev_ops;

/*  opening /dev/obd */
static int obd_class_open(struct inode * inode, struct file * file)
{
        if (obd_psdev_ops.p_open != NULL)
                return obd_psdev_ops.p_open(0, NULL);
        return -EPERM;
}

/*  closing /dev/obd */
static int obd_class_release(struct inode * inode, struct file * file)
{
        if (obd_psdev_ops.p_close != NULL)
                return obd_psdev_ops.p_close(0, NULL);
        return -EPERM;
}

/* to control /dev/obd */
static int obd_class_ioctl(struct inode *inode, struct file *filp,
                           unsigned int cmd, unsigned long arg)
{
        int err = 0;
        ENTRY;

        /* Allow non-root access for OBD_IOC_PING_TARGET - used by lfs check */
        if (!cfs_capable(CFS_CAP_SYS_ADMIN) && (cmd != OBD_IOC_PING_TARGET))
                RETURN(err = -EACCES);
        if ((cmd & 0xffffff00) == ((int)'T') << 8) /* ignore all tty ioctls */
                RETURN(err = -ENOTTY);

        if (obd_psdev_ops.p_ioctl != NULL)
                err = obd_psdev_ops.p_ioctl(NULL, cmd, (void *)arg);
        else
                err = -EPERM;

        RETURN(err);
}

/* declare character device */
static struct file_operations obd_psdev_fops = {
        .owner   = THIS_MODULE,
        .ioctl   = obd_class_ioctl,     /* ioctl */
        .open    = obd_class_open,      /* open */
        .release = obd_class_release,   /* release */
};

/* modules setup */
cfs_psdev_t obd_psdev = {
        .minor = OBD_MINOR,
        .name  = "obd_psdev",
        .fops  = &obd_psdev_fops,
};

#endif

#ifdef LPROCFS
int obd_proc_read_version(char *page, char **start, off_t off, int count,
                          int *eof, void *data)
{
        *eof = 1;
#ifdef HAVE_VFS_INTENT_PATCHES
        return snprintf(page, count, "lustre: %s\nkernel: %u\nbuild:  %s\n",
                        LUSTRE_VERSION_STRING, LUSTRE_KERNEL_VERSION,
                        BUILD_VERSION);
#else
        return snprintf(page, count, "lustre: %s\nkernel: %s\nbuild:  %s\n",
                        LUSTRE_VERSION_STRING, "patchless_client",
                        BUILD_VERSION);
#endif
}

int obd_proc_read_pinger(char *page, char **start, off_t off, int count,
                         int *eof, void *data)
{
        *eof = 1;
        return snprintf(page, count, "%s\n",
#ifdef ENABLE_PINGER
                        "on"
#else
                        "off"
#endif
                       );
}

/**
 * Check all obd devices health
 *
 * \param page
 * \param start
 * \param off
 * \param count
 * \param eof
 * \param data
 *                  proc read function parameters, please refer to kernel
 *                  code fs/proc/generic.c proc_file_read()
 * \param data [in] unused
 *
 * \retval number of characters printed
 */
static int obd_proc_read_health(char *page, char **start, off_t off,
                                int count, int *eof, void *data)
{
        int rc = 0, i;
        *eof = 1;

        if (libcfs_catastrophe)
                rc += snprintf(page + rc, count - rc, "LBUG\n");

        spin_lock(&obd_dev_lock);
        for (i = 0; i < class_devno_max(); i++) {
                struct obd_device *obd;

                obd = class_num2obd(i);
                if (obd == NULL || !obd->obd_attached || !obd->obd_set_up)
                        continue;

                LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);
                if (obd->obd_stopping)
                        continue;

                class_incref(obd);
                spin_unlock(&obd_dev_lock);

                if (obd_health_check(obd)) {
                        rc += snprintf(page + rc, count - rc,
                                       "device %s reported unhealthy\n",
                                       obd->obd_name);
                }
                class_decref(obd);
                spin_lock(&obd_dev_lock);
        }
        spin_unlock(&obd_dev_lock);

        if (rc == 0)
                return snprintf(page, count, "healthy\n");

        rc += snprintf(page + rc, count - rc, "NOT HEALTHY\n");
        return rc;
}

/* Root for /proc/fs/lustre */
struct proc_dir_entry *proc_lustre_root = NULL;

struct lprocfs_vars lprocfs_base[] = {
        { "version", obd_proc_read_version, NULL, NULL },
        { "pinger", obd_proc_read_pinger, NULL, NULL },
        { "health_check", obd_proc_read_health, NULL, NULL },
        { 0 }
};
#else
#define lprocfs_base NULL
#endif /* LPROCFS */

#ifdef __KERNEL__
static void *obd_device_list_seq_start(struct seq_file *p, loff_t *pos)
{
        if (*pos >= class_devno_max())
                return NULL;

        return pos;
}

static void obd_device_list_seq_stop(struct seq_file *p, void *v)
{
}

static void *obd_device_list_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
        ++*pos;
        if (*pos >= class_devno_max())
                return NULL;

        return pos;
}

static int obd_device_list_seq_show(struct seq_file *p, void *v)
{
        loff_t index = *(loff_t *)v;
        struct obd_device *obd = class_num2obd((int)index);
        char *status;

        if (obd == NULL)
                return 0;

        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);
        if (obd->obd_stopping)
                status = "ST";
        else if (obd->obd_inactive)
                status = "IN";
        else if (obd->obd_set_up)
                status = "UP";
        else if (obd->obd_attached)
                status = "AT";
        else
                status = "--";

        return seq_printf(p, "%3d %s %s %s %s %d\n",
                          (int)index, status, obd->obd_type->typ_name,
                          obd->obd_name, obd->obd_uuid.uuid,
                          atomic_read(&obd->obd_refcount));
}

struct seq_operations obd_device_list_sops = {
        .start = obd_device_list_seq_start,
        .stop = obd_device_list_seq_stop,
        .next = obd_device_list_seq_next,
        .show = obd_device_list_seq_show,
};

static int obd_device_list_open(struct inode *inode, struct file *file)
{
        struct proc_dir_entry *dp = PDE(inode);
        struct seq_file *seq;
        int rc = seq_open(file, &obd_device_list_sops);

        if (rc)
                return rc;

        seq = file->private_data;
        seq->private = dp->data;

        return 0;
}

struct file_operations obd_device_list_fops = {
        .owner   = THIS_MODULE,
        .open    = obd_device_list_open,
        .read    = seq_read,
        .llseek  = seq_lseek,
        .release = seq_release,
};
#endif

int class_procfs_init(void)
{
#ifdef __KERNEL__
        struct proc_dir_entry *entry;
        ENTRY;

        obd_sysctl_init();
        proc_lustre_root = lprocfs_register("fs/lustre", NULL,
                                              lprocfs_base, NULL);
        if (!proc_lustre_root) {
                printk(KERN_ERR
                       "LustreError: error registering /proc/fs/lustre\n");
                RETURN(-ENOMEM);
        }

        entry = create_proc_entry("devices", 0444, proc_lustre_root);
        if (entry == NULL) {
                CERROR("error registering /proc/fs/lustre/devices\n");
                lprocfs_remove(&proc_lustre_root);
                RETURN(-ENOMEM);
        }
        entry->proc_fops = &obd_device_list_fops;
#else
        ENTRY;
#endif
        RETURN(0);
}

#ifdef __KERNEL__
int class_procfs_clean(void)
{
        ENTRY;
        if (proc_lustre_root)
                lprocfs_remove(&proc_lustre_root);
        RETURN(0);
}


/* Check that we're building against the appropriate version of the Lustre
 * kernel patch */
#include <linux/lustre_version.h>
#ifdef LUSTRE_KERNEL_VERSION
#define LUSTRE_MIN_VERSION 45
#define LUSTRE_MAX_VERSION 47
#if (LUSTRE_KERNEL_VERSION < LUSTRE_MIN_VERSION)
# error Cannot continue: Your Lustre kernel patch is older than the sources
#elif (LUSTRE_KERNEL_VERSION > LUSTRE_MAX_VERSION)
# error Cannot continue: Your Lustre sources are older than the kernel patch
#endif
#endif
#endif
