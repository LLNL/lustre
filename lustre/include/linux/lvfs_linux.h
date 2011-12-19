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
 */

#ifndef __LVFS_LINUX_H__
#define __LVFS_LINUX_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/sched.h>

#include <lvfs.h>

#define l_file file
#define l_dentry dentry
#define l_inode inode

#define l_filp_open filp_open

struct lvfs_run_ctxt;
struct l_file *l_dentry_open(struct lvfs_run_ctxt *, struct l_dentry *,
                             int flags);

struct l_linux_dirent {
        cfs_list_t      lld_list;
        ino_t           lld_ino;
        unsigned long   lld_off;
        char            lld_name[LL_FID_NAMELEN];
};
struct l_readdir_callback {
        struct l_linux_dirent *lrc_dirent;
        cfs_list_t            *lrc_list;
};

#define LVFS_DENTRY_PARAM_MAGIC         20070216UL
struct lvfs_dentry_params
{
        unsigned long    ldp_inum;
        void            *ldp_ptr;
        __u32            ldp_magic;
};
#define LVFS_DENTRY_PARAMS_INIT         { .ldp_magic = LVFS_DENTRY_PARAM_MAGIC }

#define BDEVNAME_DECLARE_STORAGE(foo) char foo[BDEVNAME_SIZE]
#define ll_bdevname(SB, STORAGE) __bdevname(kdev_t_to_nr(SB->s_dev), STORAGE)
#define lvfs_sbdev(SB)       ((SB)->s_bdev)
#define lvfs_sbdev_type      struct block_device *
#define lvfs_sbdev_sync      fsync_bdev

int fsync_bdev(struct block_device *);

/* Instead of calling within lvfs (a layering violation) */
#define lvfs_set_rdonly(obd, sb) \
        __lvfs_set_rdonly(lvfs_sbdev(sb), fsfilt_journal_sbdev(obd, sb))

int __lvfs_set_rdonly(lvfs_sbdev_type dev, lvfs_sbdev_type jdev);

int lvfs_check_rdonly(lvfs_sbdev_type dev);
void lvfs_clear_rdonly(lvfs_sbdev_type dev);

#endif /*  __LVFS_LINUX_H__ */
