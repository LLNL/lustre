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
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#ifndef __KERNEL__
# include <liblustre.h>
#else
# include <asm/atomic.h>
#endif

#include <obd_support.h>
#include <obd_class.h>
#include <lnet/lnetctl.h>
#include <lustre_debug.h>
#include <lprocfs_status.h>
#include <lustre/lustre_build_version.h>
#include <libcfs/list.h>
#include "llog_internal.h"

#ifndef __KERNEL__
/* liblustre workaround */
cfs_atomic_t libcfs_kmemory = {0};
#endif

struct obd_device *obd_devs[MAX_OBD_DEVICES];
cfs_list_t obd_types;
cfs_spinlock_t obd_dev_lock = CFS_SPIN_LOCK_UNLOCKED;

#ifndef __KERNEL__
__u64 obd_max_pages = 0;
__u64 obd_max_alloc = 0;
__u64 obd_alloc;
__u64 obd_pages;
#endif

/* The following are visible and mutable through /proc/sys/lustre/. */
unsigned int obd_debug_peer_on_timeout;
unsigned int obd_dump_on_timeout;
unsigned int obd_dump_on_eviction;
unsigned int obd_max_dirty_pages = 256;
unsigned int obd_timeout = OBD_TIMEOUT_DEFAULT;   /* seconds */
unsigned int ldlm_timeout = LDLM_TIMEOUT_DEFAULT; /* seconds */
/* Adaptive timeout defs here instead of ptlrpc module for /proc/sys/ access */
unsigned int at_min = 15;
unsigned int at_max = 600;
unsigned int at_history = 600;
int at_early_margin = 10;
int at_extra = 30;

cfs_atomic_t obd_dirty_pages;
cfs_atomic_t obd_dirty_transit_pages;

static inline void obd_data2conn(struct lustre_handle *conn,
                                 struct obd_ioctl_data *data)
{
        memset(conn, 0, sizeof *conn);
        conn->cookie = data->ioc_cookie;
}

static inline void obd_conn2data(struct obd_ioctl_data *data,
                                 struct lustre_handle *conn)
{
        data->ioc_cookie = conn->cookie;
}

int class_resolve_dev_name(__u32 len, const char *name)
{
        int rc;
        int dev;

        ENTRY;
        if (!len || !name) {
                CERROR("No name passed,!\n");
                GOTO(out, rc = -EINVAL);
        }
        if (name[len - 1] != 0) {
                CERROR("Name not nul terminated!\n");
                GOTO(out, rc = -EINVAL);
        }

        CDEBUG(D_IOCTL, "device name %s\n", name);
        dev = class_name2dev(name);
        if (dev == -1) {
                CDEBUG(D_IOCTL, "No device for name %s!\n", name);
                GOTO(out, rc = -EINVAL);
        }

        CDEBUG(D_IOCTL, "device name %s, dev %d\n", name, dev);
        rc = dev;

out:
        RETURN(rc);
}

int class_handle_ioctl(unsigned int cmd, unsigned long arg)
{
        char *buf = NULL;
        struct obd_ioctl_data *data;
        struct libcfs_debug_ioctl_data *debug_data;
        struct obd_device *obd = NULL;
        int err = 0, len = 0;
        ENTRY;

        /* only for debugging */
        if (cmd == LIBCFS_IOC_DEBUG_MASK) {
                debug_data = (struct libcfs_debug_ioctl_data*)arg;
                libcfs_subsystem_debug = debug_data->subs;
                libcfs_debug = debug_data->debug;
                return 0;
        }

        CDEBUG(D_IOCTL, "cmd = %x\n", cmd);
        if (obd_ioctl_getdata(&buf, &len, (void *)arg)) {
                CERROR("OBD ioctl: data error\n");
                RETURN(-EINVAL);
        }
        data = (struct obd_ioctl_data *)buf;

        switch (cmd) {
        case OBD_IOC_PROCESS_CFG: {
                struct lustre_cfg *lcfg;

                if (!data->ioc_plen1 || !data->ioc_pbuf1) {
                        CERROR("No config buffer passed!\n");
                        GOTO(out, err = -EINVAL);
                }
                OBD_ALLOC(lcfg, data->ioc_plen1);
                if (lcfg == NULL)
                        GOTO(out, err = -ENOMEM);
                err = cfs_copy_from_user(lcfg, data->ioc_pbuf1,
                                         data->ioc_plen1);
                if (!err)
                        err = lustre_cfg_sanity_check(lcfg, data->ioc_plen1);
                if (!err)
                        err = class_process_config(lcfg);

                OBD_FREE(lcfg, data->ioc_plen1);
                GOTO(out, err);
        }

        case OBD_GET_VERSION:
                if (!data->ioc_inlbuf1) {
                        CERROR("No buffer passed in ioctl\n");
                        GOTO(out, err = -EINVAL);
                }

                if (strlen(BUILD_VERSION) + 1 > data->ioc_inllen1) {
                        CERROR("ioctl buffer too small to hold version\n");
                        GOTO(out, err = -EINVAL);
                }

                memcpy(data->ioc_bulk, BUILD_VERSION,
                       strlen(BUILD_VERSION) + 1);

                err = obd_ioctl_popdata((void *)arg, data, len);
                if (err)
                        err = -EFAULT;
                GOTO(out, err);

        case OBD_IOC_NAME2DEV: {
                /* Resolve a device name.  This does not change the
                 * currently selected device.
                 */
                int dev;

                dev = class_resolve_dev_name(data->ioc_inllen1,
                                             data->ioc_inlbuf1);
                data->ioc_dev = dev;
                if (dev < 0)
                        GOTO(out, err = -EINVAL);

                err = obd_ioctl_popdata((void *)arg, data, sizeof(*data));
                if (err)
                        err = -EFAULT;
                GOTO(out, err);
        }

        case OBD_IOC_UUID2DEV: {
                /* Resolve a device uuid.  This does not change the
                 * currently selected device.
                 */
                int dev;
                struct obd_uuid uuid;

                if (!data->ioc_inllen1 || !data->ioc_inlbuf1) {
                        CERROR("No UUID passed!\n");
                        GOTO(out, err = -EINVAL);
                }
                if (data->ioc_inlbuf1[data->ioc_inllen1 - 1] != 0) {
                        CERROR("UUID not NUL terminated!\n");
                        GOTO(out, err = -EINVAL);
                }

                CDEBUG(D_IOCTL, "device name %s\n", data->ioc_inlbuf1);
                obd_str2uuid(&uuid, data->ioc_inlbuf1);
                dev = class_uuid2dev(&uuid);
                data->ioc_dev = dev;
                if (dev == -1) {
                        CDEBUG(D_IOCTL, "No device for UUID %s!\n",
                               data->ioc_inlbuf1);
                        GOTO(out, err = -EINVAL);
                }

                CDEBUG(D_IOCTL, "device name %s, dev %d\n", data->ioc_inlbuf1,
                       dev);
                err = obd_ioctl_popdata((void *)arg, data, sizeof(*data));
                if (err)
                        err = -EFAULT;
                GOTO(out, err);
        }

        case OBD_IOC_CLOSE_UUID: {
                CDEBUG(D_IOCTL, "closing all connections to uuid %s (NOOP)\n",
                       data->ioc_inlbuf1);
                GOTO(out, err = 0);
        }

        case OBD_IOC_GETDEVICE: {
                int     index = data->ioc_count;
                char    *status, *str;

                if (!data->ioc_inlbuf1) {
                        CERROR("No buffer passed in ioctl\n");
                        GOTO(out, err = -EINVAL);
                }
                if (data->ioc_inllen1 < 128) {
                        CERROR("ioctl buffer too small to hold version\n");
                        GOTO(out, err = -EINVAL);
                }

                obd = class_num2obd(index);
                if (!obd)
                        GOTO(out, err = -ENOENT);

                if (obd->obd_stopping)
                        status = "ST";
                else if (obd->obd_set_up)
                        status = "UP";
                else if (obd->obd_attached)
                        status = "AT";
                else
                        status = "--";
                str = (char *)data->ioc_bulk;
                snprintf(str, len - sizeof(*data), "%3d %s %s %s %s %d",
                         (int)index, status, obd->obd_type->typ_name,
                         obd->obd_name, obd->obd_uuid.uuid,
                         cfs_atomic_read(&obd->obd_refcount));
                err = obd_ioctl_popdata((void *)arg, data, len);

                GOTO(out, err = 0);
        }

        }

        if (data->ioc_dev == OBD_DEV_BY_DEVNAME) {
                if (data->ioc_inllen4 <= 0 || data->ioc_inlbuf4 == NULL)
                        GOTO(out, err = -EINVAL);
                if (strnlen(data->ioc_inlbuf4, MAX_OBD_NAME) >= MAX_OBD_NAME)
                        GOTO(out, err = -EINVAL);
                obd = class_name2obd(data->ioc_inlbuf4);
        } else if (data->ioc_dev < class_devno_max()) {
                obd = class_num2obd(data->ioc_dev);
        } else {
                CERROR("OBD ioctl: No device\n");
                GOTO(out, err = -EINVAL);
        }

        if (obd == NULL) {
                CERROR("OBD ioctl : No Device %d\n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }
        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);

        if (!obd->obd_set_up || obd->obd_stopping) {
                CERROR("OBD ioctl: device not setup %d \n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }

        switch(cmd) {
        case OBD_IOC_NO_TRANSNO: {
                if (!obd->obd_attached) {
                        CERROR("Device %d not attached\n", obd->obd_minor);
                        GOTO(out, err = -ENODEV);
                }
                CDEBUG(D_HA, "%s: disabling committed-transno notification\n",
                       obd->obd_name);
                obd->obd_no_transno = 1;
                GOTO(out, err = 0);
        }

        default: {
                err = obd_iocontrol(cmd, obd->obd_self_export, len, data, NULL);
                if (err)
                        GOTO(out, err);

                err = obd_ioctl_popdata((void *)arg, data, len);
                if (err)
                        err = -EFAULT;
                GOTO(out, err);
        }
        }

 out:
        if (buf)
                obd_ioctl_freedata(buf, len);
        RETURN(err);
} /* class_handle_ioctl */



#ifdef __KERNEL__
extern cfs_psdev_t obd_psdev;
#else
void *obd_psdev = NULL;
#endif

EXPORT_SYMBOL(obd_devs);
EXPORT_SYMBOL(obd_debug_peer_on_timeout);
EXPORT_SYMBOL(obd_dump_on_timeout);
EXPORT_SYMBOL(obd_dump_on_eviction);
EXPORT_SYMBOL(obd_timeout);
EXPORT_SYMBOL(ldlm_timeout);
EXPORT_SYMBOL(obd_max_dirty_pages);
EXPORT_SYMBOL(obd_dirty_pages);
EXPORT_SYMBOL(obd_dirty_transit_pages);
EXPORT_SYMBOL(at_min);
EXPORT_SYMBOL(at_max);
EXPORT_SYMBOL(at_extra);
EXPORT_SYMBOL(at_early_margin);
EXPORT_SYMBOL(at_history);
EXPORT_SYMBOL(ptlrpc_put_connection_superhack);

EXPORT_SYMBOL(proc_lustre_root);

EXPORT_SYMBOL(class_register_type);
EXPORT_SYMBOL(class_unregister_type);
EXPORT_SYMBOL(class_get_type);
EXPORT_SYMBOL(class_put_type);
EXPORT_SYMBOL(class_name2dev);
EXPORT_SYMBOL(class_name2obd);
EXPORT_SYMBOL(class_uuid2dev);
EXPORT_SYMBOL(class_uuid2obd);
EXPORT_SYMBOL(class_find_client_obd);
EXPORT_SYMBOL(class_devices_in_group);
EXPORT_SYMBOL(class_conn2export);
EXPORT_SYMBOL(class_exp2obd);
EXPORT_SYMBOL(class_conn2obd);
EXPORT_SYMBOL(class_exp2cliimp);
EXPORT_SYMBOL(class_conn2cliimp);
EXPORT_SYMBOL(class_disconnect);
EXPORT_SYMBOL(class_num2obd);

/* uuid.c */
EXPORT_SYMBOL(class_uuid_unparse);
EXPORT_SYMBOL(lustre_uuid_to_peer);

EXPORT_SYMBOL(class_handle_hash);
EXPORT_SYMBOL(class_handle_unhash);
EXPORT_SYMBOL(class_handle_hash_back);
EXPORT_SYMBOL(class_handle2object);
EXPORT_SYMBOL(class_handle_free_cb);

/* obd_config.c */
EXPORT_SYMBOL(class_incref);
EXPORT_SYMBOL(class_decref);
EXPORT_SYMBOL(class_get_profile);
EXPORT_SYMBOL(class_del_profile);
EXPORT_SYMBOL(class_del_profiles);
EXPORT_SYMBOL(class_process_config);
EXPORT_SYMBOL(class_process_proc_param);
EXPORT_SYMBOL(class_config_parse_llog);
EXPORT_SYMBOL(class_config_dump_llog);
EXPORT_SYMBOL(class_attach);
EXPORT_SYMBOL(class_setup);
EXPORT_SYMBOL(class_cleanup);
EXPORT_SYMBOL(class_detach);
EXPORT_SYMBOL(class_manual_cleanup);

/* mea.c */
EXPORT_SYMBOL(mea_name2idx);
EXPORT_SYMBOL(raw_name2idx);

#define OBD_INIT_CHECK
#ifdef OBD_INIT_CHECK
int obd_init_checks(void)
{
        __u64 u64val, div64val;
        char buf[64];
        int len, ret = 0;

        CDEBUG(D_INFO, "LPU64=%s, LPD64=%s, LPX64=%s\n", LPU64, LPD64, LPX64);

        CDEBUG(D_INFO, "OBD_OBJECT_EOF = "LPX64"\n", (__u64)OBD_OBJECT_EOF);

        u64val = OBD_OBJECT_EOF;
        CDEBUG(D_INFO, "u64val OBD_OBJECT_EOF = "LPX64"\n", u64val);
        if (u64val != OBD_OBJECT_EOF) {
                CERROR("__u64 "LPX64"(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                ret = -EINVAL;
        }
        len = snprintf(buf, sizeof(buf), LPX64, u64val);
        if (len != 18) {
                CWARN("LPX64 wrong length! strlen(%s)=%d != 18\n", buf, len);
                ret = -EINVAL;
        }

        div64val = OBD_OBJECT_EOF;
        CDEBUG(D_INFO, "u64val OBD_OBJECT_EOF = "LPX64"\n", u64val);
        if (u64val != OBD_OBJECT_EOF) {
                CERROR("__u64 "LPX64"(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                ret = -EOVERFLOW;
        }
        if (u64val >> 8 != OBD_OBJECT_EOF >> 8) {
                CERROR("__u64 "LPX64"(%d) != 0xffffffffffffffff\n",
                       u64val, (int)sizeof(u64val));
                return -EOVERFLOW;
        }
        if (do_div(div64val, 256) != (u64val & 255)) {
                CERROR("do_div("LPX64",256) != "LPU64"\n", u64val, u64val &255);
                return -EOVERFLOW;
        }
        if (u64val >> 8 != div64val) {
                CERROR("do_div("LPX64",256) "LPU64" != "LPU64"\n",
                       u64val, div64val, u64val >> 8);
                return -EOVERFLOW;
        }
        len = snprintf(buf, sizeof(buf), LPX64, u64val);
        if (len != 18) {
                CWARN("LPX64 wrong length! strlen(%s)=%d != 18\n", buf, len);
                ret = -EINVAL;
        }
        len = snprintf(buf, sizeof(buf), LPU64, u64val);
        if (len != 20) {
                CWARN("LPU64 wrong length! strlen(%s)=%d != 20\n", buf, len);
                ret = -EINVAL;
        }
        len = snprintf(buf, sizeof(buf), LPD64, u64val);
        if (len != 2) {
                CWARN("LPD64 wrong length! strlen(%s)=%d != 2\n", buf, len);
                ret = -EINVAL;
        }
        if ((u64val & ~CFS_PAGE_MASK) >= CFS_PAGE_SIZE) {
                CWARN("mask failed: u64val "LPU64" >= "LPU64"\n", u64val,
                      (__u64)CFS_PAGE_SIZE);
                ret = -EINVAL;
        }

        return ret;
}
#else
#define obd_init_checks() do {} while(0)
#endif

extern cfs_spinlock_t obd_types_lock;
extern int class_procfs_init(void);
extern int class_procfs_clean(void);

#ifdef __KERNEL__
static int __init init_obdclass(void)
#else
int init_obdclass(void)
#endif
{
        int i, err;
#ifdef __KERNEL__
        int lustre_register_fs(void);

        for (i = CAPA_SITE_CLIENT; i < CAPA_SITE_MAX; i++)
                CFS_INIT_LIST_HEAD(&capa_list[i]);
#endif

        LCONSOLE_INFO("Lustre: Build Version: "BUILD_VERSION"\n");

        cfs_spin_lock_init(&obd_types_lock);
        obd_zombie_impexp_init();
#ifdef LPROCFS
        obd_memory = lprocfs_alloc_stats(OBD_STATS_NUM,
					 LPROCFS_STATS_FLAG_NONE |
					 LPROCFS_STATS_FLAG_IRQ_SAFE);
        if (obd_memory == NULL) {
                CERROR("kmalloc of 'obd_memory' failed\n");
                RETURN(-ENOMEM);
        }

        lprocfs_counter_init(obd_memory, OBD_MEMORY_STAT,
                             LPROCFS_CNTR_AVGMINMAX,
                             "memused", "bytes");
        lprocfs_counter_init(obd_memory, OBD_MEMORY_PAGES_STAT,
                             LPROCFS_CNTR_AVGMINMAX,
                             "pagesused", "pages");
#endif
        err = obd_init_checks();
        if (err == -EOVERFLOW)
                return err;

        class_init_uuidlist();
        err = class_handle_init();
        if (err)
                return err;

        cfs_spin_lock_init(&obd_dev_lock);
        CFS_INIT_LIST_HEAD(&obd_types);

        err = cfs_psdev_register(&obd_psdev);
        if (err) {
                CERROR("cannot register %d err %d\n", OBD_DEV_MINOR, err);
                return err;
        }

        /* This struct is already zeroed for us (static global) */
        for (i = 0; i < class_devno_max(); i++)
                obd_devs[i] = NULL;

        /* Default the dirty page cache cap to 1/2 of system memory.
         * For clients with less memory, a larger fraction is needed
         * for other purposes (mostly for BGL). */
        if (cfs_num_physpages <= 512 << (20 - CFS_PAGE_SHIFT))
                obd_max_dirty_pages = cfs_num_physpages / 4;
        else
                obd_max_dirty_pages = cfs_num_physpages / 2;

        err = obd_init_caches();
        if (err)
                return err;
#ifdef __KERNEL__
        err = class_procfs_init();
        if (err)
                return err;
#endif

        err = lu_global_init();
        if (err)
                return err;

#ifdef __KERNEL__
        err = lustre_register_fs();
#endif

        return err;
}

/* liblustre doesn't call cleanup_obdclass, apparently.  we carry on in this
 * ifdef to the end of the file to cover module and versioning goo.*/
#ifdef __KERNEL__
static void cleanup_obdclass(void)
{
        int i;
        int lustre_unregister_fs(void);
        __u64 memory_leaked, pages_leaked;
        __u64 memory_max, pages_max;
        ENTRY;

        lustre_unregister_fs();

        cfs_psdev_deregister(&obd_psdev);
        for (i = 0; i < class_devno_max(); i++) {
                struct obd_device *obd = class_num2obd(i);
                if (obd && obd->obd_set_up &&
                    OBT(obd) && OBP(obd, detach)) {
                        /* XXX should this call generic detach otherwise? */
                        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);
                        OBP(obd, detach)(obd);
                }
        }
        lu_global_fini();

        obd_cleanup_caches();
        obd_sysctl_clean();

        class_procfs_clean();

        class_handle_cleanup();
        class_exit_uuidlist();
        obd_zombie_impexp_stop();

        memory_leaked = obd_memory_sum();
        pages_leaked = obd_pages_sum();

        memory_max = obd_memory_max();
        pages_max = obd_pages_max();

        lprocfs_free_stats(&obd_memory);
        CDEBUG((memory_leaked) ? D_ERROR : D_INFO,
               "obd_memory max: "LPU64", leaked: "LPU64"\n",
               memory_max, memory_leaked);
        CDEBUG((pages_leaked) ? D_ERROR : D_INFO,
               "obd_memory_pages max: "LPU64", leaked: "LPU64"\n",
               pages_max, pages_leaked);

        EXIT;
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Class Driver Build Version: " BUILD_VERSION);
MODULE_LICENSE("GPL");

cfs_module(obdclass, LUSTRE_VERSION_STRING, init_obdclass, cleanup_obdclass);
#endif
