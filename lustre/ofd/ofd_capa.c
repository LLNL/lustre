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
 * Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ofd/ofd_capa.c
 *
 * Author: Lai Siyao <lsy@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#ifdef __KERNEL__
#include <linux/fs.h>
#include <linux/version.h>
#include <asm/uaccess.h>
#include <linux/file.h>
#include <linux/kmod.h>
#endif

#include <lustre_capa.h>
#include "ofd_internal.h"

static inline __u32 ofd_ck_keyid(struct filter_capa_key *key)
{
        return key->k_key.lk_keyid;
}

int ofd_update_capa_key(struct ofd_device *ofd, struct lustre_capa_key *new)
{
        struct filter_capa_key *k, *keys[2] = { NULL, NULL };
        int i;

        cfs_spin_lock(&capa_lock);
        cfs_list_for_each_entry(k, &ofd->ofd_capa_keys, k_list) {
                if (k->k_key.lk_seq != new->lk_seq)
                        continue;

                if (keys[0]) {
                        keys[1] = k;
                        if (ofd_ck_keyid(keys[1]) > ofd_ck_keyid(keys[0]))
                                keys[1] = keys[0], keys[0] = k;
                } else {
                        keys[0] = k;
                }
        }
        cfs_spin_unlock(&capa_lock);

        for (i = 0; i < 2; i++) {
                if (!keys[i])
                        continue;
                if (ofd_ck_keyid(keys[i]) != new->lk_keyid)
                        continue;
                /* maybe because of recovery or other reasons, MDS sent the
                 * the old capability key again.
                 */
                cfs_spin_lock(&capa_lock);
                keys[i]->k_key = *new;
                cfs_spin_unlock(&capa_lock);

                RETURN(0);
        }

        if (keys[1]) {
                /* if OSS already have two keys, update the old one */
                k = keys[1];
        } else {
                OBD_ALLOC_PTR(k);
                if (!k)
                        RETURN(-ENOMEM);
                CFS_INIT_LIST_HEAD(&k->k_list);
        }

        cfs_spin_lock(&capa_lock);
        k->k_key = *new;
        if (cfs_list_empty(&k->k_list))
                cfs_list_add(&k->k_list, &ofd->ofd_capa_keys);
        cfs_spin_unlock(&capa_lock);

        DEBUG_CAPA_KEY(D_SEC, new, "new");
        RETURN(0);
}

int ofd_auth_capa(struct ofd_device *ofd, struct lu_fid *fid,
                     __u64 mdsid, struct lustre_capa *capa, __u64 opc)
{
        RETURN(0);
}

void ofd_free_capa_keys(struct ofd_device *ofd)
{
        struct filter_capa_key *key, *n;

        cfs_spin_lock(&capa_lock);
        cfs_list_for_each_entry_safe(key, n, &ofd->ofd_capa_keys, k_list) {
                cfs_list_del_init(&key->k_list);
                OBD_FREE(key, sizeof(*key));
        }
        cfs_spin_unlock(&capa_lock);
}
