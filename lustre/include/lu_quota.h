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
 * version 2 along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2011 Whamcloud, Inc.
 * Use is subject to license terms.
 */

#include <lustre_fid.h>

#ifndef _LUSTRE_LU_QUOTA_H
#define _LUSTRE_LU_QUOTA_H

/* generic structure allocated by each quota user (i.e. MDD and OFD) used to
 * to access per-user/group usage/quota information from underlying storage */
struct lu_quota {
        struct dt_object *acct_user_obj;
        struct dt_object *acct_group_obj;
        /* TODO: slave and master objects to be added here */
};

/* format of an accounting record, providing disk usage information for a given
 * user or group */
struct acct_rec { /* 32 bytes */
        __u64 bspace;  /* current space in use */
        __u64 ispace;  /* current # inodes in use */
};

/* index features supported by the accounting objects */
extern const struct dt_index_features dt_acct_features;

void lu_quota_init(const struct lu_env *env, struct dt_device *dev,
                   struct lu_quota *lu_quota,
                   cfs_proc_dir_entry_t *proc_entry);
void lu_quota_fini(const struct lu_env *env, struct dt_device *dev,
                   struct lu_quota *lu_quota);
int lu_quotactl(const struct lu_env *env, struct lu_quota *lu_quota,
                struct obd_quotactl *oqctl);
#endif /* _LUSTRE_LU_QUOTA_H */
