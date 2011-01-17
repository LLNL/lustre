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
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */

#define DEBUG_SUBSYSTEM S_CLASS
#define D_MOUNT D_SUPER|D_CONFIG /*|D_WARNING */
#define PRINT_CMD CDEBUG
#define PRINT_MASK D_SUPER|D_CONFIG

#include <obd.h>
#include <lvfs.h>
#include <lustre_fsfilt.h>
#include <obd_class.h>
#include <lustre/lustre_user.h>
#include <linux/version.h>
#include <lustre_log.h>
#include <lustre_disk.h>
#include <lustre_param.h>

int (*client_fill_super)(struct super_block *sb, struct vfsmount *mnt) = NULL;
static void (*kill_super_cb)(struct super_block *sb) = NULL;

void lustre_osvfs_update(void *osvfs, struct lustre_sb_info *lsi)
{
        struct super_block *sb = (struct super_block *)osvfs;

        LASSERT(sb != NULL);
        sb->s_fs_info = lsi;
}

static void server_put_super(struct super_block *sb)
{
        lustre_server_umount(s2lsi(sb));
}

#ifdef HAVE_UMOUNTBEGIN_VFSMOUNT
static void server_umount_begin(struct vfsmount *vfsmnt, int flags)
{
        struct super_block *sb = vfsmnt->mnt_sb;
#else
static void server_umount_begin(struct super_block *sb)
{
#endif
        ENTRY;

#ifdef HAVE_UMOUNTBEGIN_VFSMOUNT
        if (!(flags & MNT_FORCE)) {
                EXIT;
                return;
        }
#endif

        lustre_umount_server_force_flag_set(s2lsi(sb));

        EXIT;
}

#ifndef HAVE_STATFS_DENTRY_PARAM
static int server_statfs (struct super_block *sb, cfs_kstatfs_t *buf)
{
#else
static int server_statfs (struct dentry *dentry, cfs_kstatfs_t *buf)
{
        struct super_block *sb = dentry->d_sb;
#endif
        ENTRY;

        /* XXX Call lustre_server_statfs(s2lsi(sb), buf) here */

        /* just return 0 */
        buf->f_type = sb->s_magic;
        buf->f_bsize = sb->s_blocksize;
        buf->f_blocks = 1;
        buf->f_bfree = 0;
        buf->f_bavail = 0;
        buf->f_files = 1;
        buf->f_ffree = 0;
        buf->f_namelen = NAME_MAX;
        RETURN(0);
}

/** The operations we support directly on the superblock:
 * mount, umount, and df.
 */
static struct super_operations server_ops =
{
        .put_super      = server_put_super,
        .umount_begin   = server_umount_begin, /* umount -f */
        .statfs         = server_statfs,
};

#define log2(n) cfs_ffz(~(n))
#define LUSTRE_SUPER_MAGIC 0x0BD00BD1

int lustre_osvfs_mount(void *osvfs)
{
        struct super_block *sb = (struct super_block *)osvfs;
        struct inode *root = 0;
        ENTRY;

        CDEBUG(D_MOUNT, "Server sb, dev=%d\n", (int)sb->s_dev);

        sb->s_blocksize = 4096;
        sb->s_blocksize_bits = log2(sb->s_blocksize);
        sb->s_magic = LUSTRE_SUPER_MAGIC;
        sb->s_maxbytes = 0; //PAGE_CACHE_MAXBYTES;
        sb->s_flags |= MS_RDONLY;
        sb->s_op = &server_ops;

        root = new_inode(sb);
        if (!root) {
                CERROR("Can't make root inode\n");
                RETURN(-EIO);
        }

        /* returns -EIO for every operation */
        /* make_bad_inode(root); -- badness - can't umount */
        /* apparently we need to be a directory for the mount to finish */
        root->i_mode = S_IFDIR;

        sb->s_root = d_alloc_root(root);
        if (!sb->s_root) {
                CERROR("Can't make root dentry\n");
                iput(root);
                RETURN(-EIO);
        }

        RETURN(0);
}


/* We can't call ll_fill_super by name because it lives in a module that
   must be loaded after this one. */
void lustre_register_client_fill_super(int (*cfs)(struct super_block *sb,
                                                  struct vfsmount *mnt))
{
        client_fill_super = cfs;
}

void lustre_register_kill_super_cb(void (*cfs)(struct super_block *sb))
{
        kill_super_cb = cfs;
}

struct lustre_mount_data2 {
        void *lmd2_data;
        struct vfsmount *lmd2_mnt;
};

static int lustre_fill_super(struct super_block *sb, void *data, int silent)
{
        struct lustre_mount_data2 *lmd2 = data;
        return lustre_mount(sb, lmd2->lmd2_mnt, lmd2->lmd2_data, sb->s_flags);
}

/***************** FS registration ******************/

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18))
static struct super_block * lustre_get_sb(struct file_system_type *fs_type,
                               int flags, const char *devname, void * data)
{
        return get_sb_nodev(fs_type, flags, data, lustre_fill_super);
}
#else
static int lustre_get_sb(struct file_system_type *fs_type,
                               int flags, const char *devname, void * data,
                               struct vfsmount *mnt)
{
        struct lustre_mount_data2 lmd2;

        lmd2.lmd2_data = data;
        lmd2.lmd2_mnt = mnt;
        return get_sb_nodev(fs_type, flags, &lmd2, lustre_fill_super, mnt);
}
#endif

static void lustre_kill_super(struct super_block *sb)
{
        struct lustre_sb_info *lsi = s2lsi(sb);

        if (kill_super_cb && lsi && !IS_SERVER(lsi))
                (*kill_super_cb)(sb);

        kill_anon_super(sb);
}

/** Register the "lustre" fs type
 */
static struct file_system_type lustre_fs_type = {
        .owner        = THIS_MODULE,
        .name         = "lustre",
        .get_sb       = lustre_get_sb,
        .kill_sb      = lustre_kill_super,
        .fs_flags     = FS_BINARY_MOUNTDATA | FS_REQUIRES_DEV |
#ifdef FS_HAS_FIEMAP
                        FS_HAS_FIEMAP |
#endif
                        LL_RENAME_DOES_D_MOVE,
};

int lustre_register_fs(void)
{
        return register_filesystem(&lustre_fs_type);
}

int lustre_unregister_fs(void)
{
        return unregister_filesystem(&lustre_fs_type);
}

EXPORT_SYMBOL(lustre_register_client_fill_super);
EXPORT_SYMBOL(lustre_register_kill_super_cb);
