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
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef __LINUX_CLASS_OBD_H
#define __LINUX_CLASS_OBD_H

#ifndef __CLASS_OBD_H
#error Do not #include this file directly. #include <obd_class.h> instead
#endif

#ifndef __KERNEL__
#include <sys/types.h>
#include <libcfs/list.h>
#else
#include <asm/uaccess.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/timer.h>
#endif

#ifdef __KERNEL__
# ifndef HAVE_SERVER_SUPPORT

/* hash info structure used by the directory hash */
#  define LDISKFS_DX_HASH_LEGACY        0
#  define LDISKFS_DX_HASH_HALF_MD4      1
#  define LDISKFS_DX_HASH_TEA           2
#  define LDISKFS_DX_HASH_R5            3
#  define LDISKFS_DX_HASH_SAME          4
#  define LDISKFS_DX_HASH_MAX           4

/* hash info structure used by the directory hash */
struct ldiskfs_dx_hash_info
{
        u32     hash;
        u32     minor_hash;
        int     hash_version;
        u32     *seed;
};

#  define LDISKFS_HTREE_EOF     0x7fffffff

int ldiskfsfs_dirhash(const char *name, int len, struct ldiskfs_dx_hash_info *hinfo);

# endif /* HAVE_SERVER_SUPPORT */
#endif /* __KERNEL__ */

/* obdo.c */
#ifdef __KERNEL__
void obdo_from_la(struct obdo *dst, struct lu_attr *la, __u64 valid);
void la_from_obdo(struct lu_attr *la, struct obdo *dst, obd_flag valid);
void obdo_refresh_inode(struct inode *dst, struct obdo *src, obd_flag valid);
void obdo_to_inode(struct inode *dst, struct obdo *src, obd_flag valid);
#define ll_inode_flags(inode)         (inode->i_flags)
#endif


#if !defined(__KERNEL__)
#define to_kdev_t(dev) dev
#define kdev_t_to_nr(dev) dev
#endif

#endif /* __LINUX_OBD_CLASS_H */
