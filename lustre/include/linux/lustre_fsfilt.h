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
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/include/linux/lustre_fsfilt.h
 *
 * Filesystem interface helper.
 */

#ifndef _LINUX_LUSTRE_FSFILT_H
#define _LINUX_LUSTRE_FSFILT_H

#ifndef _LUSTRE_FSFILT_H
#error Do not #include this file directly. #include <lustre_fsfilt.h> instead
#endif

#ifdef __KERNEL__

#include <obd.h>
#include <obd_class.h>

typedef void (*fsfilt_cb_t)(struct obd_device *obd, __u64 last_rcvd,
                            void *data, int error);

struct fsfilt_operations {
        cfs_list_t fs_list;
        cfs_module_t *fs_owner;
        char   *fs_type;
        int     (* fs_map_inode_pages)(struct inode *inode, struct page **page,
                                       int pages, unsigned long *blocks,
                                       int *created, int create,
                                       cfs_mutex_t *sem);
};

extern int fsfilt_register_ops(struct fsfilt_operations *fs_ops);
extern void fsfilt_unregister_ops(struct fsfilt_operations *fs_ops);
extern struct fsfilt_operations *fsfilt_get_ops(const char *type);
extern void fsfilt_put_ops(struct fsfilt_operations *fs_ops);

static inline int fsfilt_map_inode_pages(struct obd_device *obd,
                                         struct inode *inode,
                                         struct page **page, int pages,
                                         unsigned long *blocks, int *created,
                                         int create, cfs_mutex_t *mutex)
{
        return obd->obd_fsops->fs_map_inode_pages(inode, page, pages, blocks,
                                                  created, create, mutex);
}

#endif /* __KERNEL__ */

#endif
