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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lvfs/lvfs_linux.c
 *
 * Author: Andreas Dilger <adilger@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#define DEBUG_SUBSYSTEM S_FILTER

#include <linux/version.h>
#include <linux/fs.h>
#include <asm/unistd.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/version.h>
#include <libcfs/libcfs.h>
#include <lustre_fsfilt.h>
#include <obd.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/lustre_compat25.h>
#include <lvfs.h>

#include <obd.h>
#include <lustre_lib.h>
#include <lustre_quota.h>

__u64 obd_max_pages = 0;
__u64 obd_max_alloc = 0;
struct lprocfs_stats *obd_memory = NULL;
cfs_spinlock_t obd_updatemax_lock = CFS_SPIN_LOCK_UNLOCKED;
/* refine later and change to seqlock or simlar from libcfs */

/* Debugging check only needed during development */
#ifdef OBD_CTXT_DEBUG
# define ASSERT_CTXT_MAGIC(magic) LASSERT((magic) == OBD_RUN_CTXT_MAGIC)
# define ASSERT_NOT_KERNEL_CTXT(msg) LASSERTF(!segment_eq(get_fs(), get_ds()),\
                                              msg)
# define ASSERT_KERNEL_CTXT(msg) LASSERTF(segment_eq(get_fs(), get_ds()), msg)
#else
# define ASSERT_CTXT_MAGIC(magic) do {} while(0)
# define ASSERT_NOT_KERNEL_CTXT(msg) do {} while(0)
# define ASSERT_KERNEL_CTXT(msg) do {} while(0)
#endif

static void push_group_info(struct lvfs_run_ctxt *save,
                            struct group_info *ginfo)
{
        if (!ginfo) {
                save->ngroups = current_ngroups;
                current_ngroups = 0;
        } else {
                struct cred *cred;
                task_lock(current);
                save->group_info = current_cred()->group_info;
                if ((cred = prepare_creds())) {
                        cred->group_info = ginfo;
                        commit_creds(cred);
                }
                task_unlock(current);
        }
}

static void pop_group_info(struct lvfs_run_ctxt *save,
                           struct group_info *ginfo)
{
        if (!ginfo) {
                current_ngroups = save->ngroups;
        } else {
                struct cred *cred;
                task_lock(current);
                if ((cred = prepare_creds())) {
                        cred->group_info = save->group_info;
                        commit_creds(cred);
                }
                task_unlock(current);
        }
}

/* push / pop to root of obd store */
void push_ctxt(struct lvfs_run_ctxt *save, struct lvfs_run_ctxt *new_ctx,
               struct lvfs_ucred *uc)
{
        //ASSERT_NOT_KERNEL_CTXT("already in kernel context!\n");
        ASSERT_CTXT_MAGIC(new_ctx->magic);
        OBD_SET_CTXT_MAGIC(save);

        save->fs = get_fs();
        LASSERT(cfs_atomic_read(&cfs_fs_pwd(current->fs)->d_count));
        LASSERT(cfs_atomic_read(&new_ctx->pwd->d_count));
        save->pwd = dget(cfs_fs_pwd(current->fs));
        save->pwdmnt = mntget(cfs_fs_mnt(current->fs));
        save->luc.luc_umask = cfs_curproc_umask();
        save->ngroups = current_cred()->group_info->ngroups;

        LASSERT(save->pwd);
        LASSERT(save->pwdmnt);
        LASSERT(new_ctx->pwd);
        LASSERT(new_ctx->pwdmnt);

        if (uc) {
                struct cred *cred;
                save->luc.luc_uid = current_uid();
                save->luc.luc_gid = current_gid();
                save->luc.luc_fsuid = current_fsuid();
                save->luc.luc_fsgid = current_fsgid();
                save->luc.luc_cap = current_cap();

                if ((cred = prepare_creds())) {
                        cred->uid = uc->luc_uid;
                        cred->gid = uc->luc_gid;
                        cred->fsuid = uc->luc_fsuid;
                        cred->fsgid = uc->luc_fsgid;
                        cred->cap_effective = uc->luc_cap;
                        commit_creds(cred);
                }

                push_group_info(save,
                                uc->luc_ginfo ?:
                                uc->luc_identity ? uc->luc_identity->mi_ginfo :
                                                   NULL);
        }
        current->fs->umask = 0; /* umask already applied on client */
        set_fs(new_ctx->fs);
        ll_set_fs_pwd(current->fs, new_ctx->pwdmnt, new_ctx->pwd);
}
EXPORT_SYMBOL(push_ctxt);

void pop_ctxt(struct lvfs_run_ctxt *saved, struct lvfs_run_ctxt *new_ctx,
              struct lvfs_ucred *uc)
{
        ASSERT_CTXT_MAGIC(saved->magic);
        ASSERT_KERNEL_CTXT("popping non-kernel context!\n");

        LASSERTF(cfs_fs_pwd(current->fs) == new_ctx->pwd, "%p != %p\n",
                 cfs_fs_pwd(current->fs), new_ctx->pwd);
        LASSERTF(cfs_fs_mnt(current->fs) == new_ctx->pwdmnt, "%p != %p\n",
                 cfs_fs_mnt(current->fs), new_ctx->pwdmnt);

        set_fs(saved->fs);
        ll_set_fs_pwd(current->fs, saved->pwdmnt, saved->pwd);

        dput(saved->pwd);
        mntput(saved->pwdmnt);
        current->fs->umask = saved->luc.luc_umask;
        if (uc) {
                struct cred *cred;
                if ((cred = prepare_creds())) {
                        cred->uid = saved->luc.luc_uid;
                        cred->gid = saved->luc.luc_gid;
                        cred->fsuid = saved->luc.luc_fsuid;
                        cred->fsgid = saved->luc.luc_fsgid;
                        cred->cap_effective = saved->luc.luc_cap;
                        commit_creds(cred);
                }

                pop_group_info(saved,
                               uc->luc_ginfo ?:
                               uc->luc_identity ? uc->luc_identity->mi_ginfo :
                                                  NULL);
        }
}
EXPORT_SYMBOL(pop_ctxt);

/* utility to make a directory */
struct dentry *simple_mkdir(struct dentry *dir, struct vfsmount *mnt, 
                            const char *name, int mode, int fix)
{
        struct dentry *dchild;
        int err = 0;
        ENTRY;

        // ASSERT_KERNEL_CTXT("kernel doing mkdir outside kernel context\n");
        CDEBUG(D_INODE, "creating directory %.*s\n", (int)strlen(name), name);
        dchild = ll_lookup_one_len(name, dir, strlen(name));
        if (IS_ERR(dchild))
                GOTO(out_up, dchild);

        if (dchild->d_inode) {
                int old_mode = dchild->d_inode->i_mode;
                if (!S_ISDIR(old_mode)) {
                        CERROR("found %s (%lu/%u) is mode %o\n", name,
                               dchild->d_inode->i_ino,
                               dchild->d_inode->i_generation, old_mode);
                        GOTO(out_err, err = -ENOTDIR);
                }

                /* Fixup directory permissions if necessary */
                if (fix && (old_mode & S_IALLUGO) != (mode & S_IALLUGO)) {
                        CDEBUG(D_CONFIG,
                               "fixing permissions on %s from %o to %o\n",
                               name, old_mode, mode);
                        dchild->d_inode->i_mode = (mode & S_IALLUGO) |
                                                  (old_mode & ~S_IALLUGO);
                        mark_inode_dirty(dchild->d_inode);
                }
                GOTO(out_up, dchild);
        }

        err = ll_vfs_mkdir(dir->d_inode, dchild, mnt, mode);
        if (err)
                GOTO(out_err, err);

        RETURN(dchild);

out_err:
        dput(dchild);
        dchild = ERR_PTR(err);
out_up:
        return dchild;
}
EXPORT_SYMBOL(simple_mkdir);

/* utility to rename a file */
int lustre_rename(struct dentry *dir, struct vfsmount *mnt,
                  char *oldname, char *newname)
{
        struct dentry *dchild_old, *dchild_new;
        int err = 0;
        ENTRY;

        ASSERT_KERNEL_CTXT("kernel doing rename outside kernel context\n");
        CDEBUG(D_INODE, "renaming file %.*s to %.*s\n",
               (int)strlen(oldname), oldname, (int)strlen(newname), newname);

        dchild_old = ll_lookup_one_len(oldname, dir, strlen(oldname));
        if (IS_ERR(dchild_old))
                RETURN(PTR_ERR(dchild_old));

        if (!dchild_old->d_inode)
                GOTO(put_old, err = -ENOENT);

        dchild_new = ll_lookup_one_len(newname, dir, strlen(newname));
        if (IS_ERR(dchild_new))
                GOTO(put_old, err = PTR_ERR(dchild_new));

        err = ll_vfs_rename(dir->d_inode, dchild_old, mnt,
                            dir->d_inode, dchild_new, mnt);

        dput(dchild_new);
put_old:
        dput(dchild_old);
        RETURN(err);
}
EXPORT_SYMBOL(lustre_rename);

int __lvfs_set_rdonly(lvfs_sbdev_type dev, lvfs_sbdev_type jdev)
{
#ifdef HAVE_DEV_SET_RDONLY
        if (jdev && (jdev != dev)) {
                CDEBUG(D_IOCTL | D_HA, "set journal dev %lx rdonly\n",
                       (long)jdev);
                dev_set_rdonly(jdev);
        }
        CDEBUG(D_IOCTL | D_HA, "set dev %lx rdonly\n", (long)dev);
        dev_set_rdonly(dev);

        return 0;
#else
        CERROR("DEV %lx CANNOT BE SET READONLY\n", (long)dev);

        return -EOPNOTSUPP;
#endif
}
EXPORT_SYMBOL(__lvfs_set_rdonly);

int lvfs_check_rdonly(lvfs_sbdev_type dev)
{
#ifdef HAVE_DEV_SET_RDONLY
        return dev_check_rdonly(dev);
#else
        return 0;
#endif
}
EXPORT_SYMBOL(lvfs_check_rdonly);

void obd_update_maxusage()
{
        __u64 max1, max2;

        max1 = obd_pages_sum();
        max2 = obd_memory_sum();

        cfs_spin_lock(&obd_updatemax_lock);
        if (max1 > obd_max_pages)
                obd_max_pages = max1;
        if (max2 > obd_max_alloc)
                obd_max_alloc = max2;
        cfs_spin_unlock(&obd_updatemax_lock);

}

__u64 obd_memory_max(void)
{
        __u64 ret;

        cfs_spin_lock(&obd_updatemax_lock);
        ret = obd_max_alloc;
        cfs_spin_unlock(&obd_updatemax_lock);

        return ret;
}

__u64 obd_pages_max(void)
{
        __u64 ret;

        cfs_spin_lock(&obd_updatemax_lock);
        ret = obd_max_pages;
        cfs_spin_unlock(&obd_updatemax_lock);

        return ret;
}

EXPORT_SYMBOL(obd_update_maxusage);
EXPORT_SYMBOL(obd_pages_max);
EXPORT_SYMBOL(obd_memory_max);
EXPORT_SYMBOL(obd_memory);

#ifdef LPROCFS
__s64 lprocfs_read_helper(struct lprocfs_counter *lc,
                          enum lprocfs_fields_flags field)
{
        __s64 ret = 0;
        int centry;

        if (!lc)
                RETURN(0);
        do {
                centry = cfs_atomic_read(&lc->lc_cntl.la_entry);

                switch (field) {
                        case LPROCFS_FIELDS_FLAGS_CONFIG:
                                ret = lc->lc_config;
                                break;
                        case LPROCFS_FIELDS_FLAGS_SUM:
                                ret = lc->lc_sum + lc->lc_sum_irq;
                                break;
                        case LPROCFS_FIELDS_FLAGS_MIN:
                                ret = lc->lc_min;
                                break;
                        case LPROCFS_FIELDS_FLAGS_MAX:
                                ret = lc->lc_max;
                                break;
                        case LPROCFS_FIELDS_FLAGS_AVG:
                                ret = (lc->lc_max - lc->lc_min)/2;
                                break;
                        case LPROCFS_FIELDS_FLAGS_SUMSQUARE:
                                ret = lc->lc_sumsquare;
                                break;
                        case LPROCFS_FIELDS_FLAGS_COUNT:
                                ret = lc->lc_count;
                                break;
                        default:
                                break;
                };
        } while (centry != cfs_atomic_read(&lc->lc_cntl.la_entry) &&
                 centry != cfs_atomic_read(&lc->lc_cntl.la_exit));

        RETURN(ret);
}
EXPORT_SYMBOL(lprocfs_read_helper);
#endif /* LPROCFS */

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre VFS Filesystem Helper v0.1");
MODULE_LICENSE("GPL");
