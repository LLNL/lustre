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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/osd/osd_oi.c
 *
 * Object Index.
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 */

/*
 * oi uses two mechanisms to implement fid->cookie mapping:
 *
 *     - persistent index, where cookie is a record and fid is a key, and
 *
 *     - algorithmic mapping for "igif" fids.
 *
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>

/* LUSTRE_VERSION_CODE */
#include <lustre_ver.h>
/*
 * struct OBD_{ALLOC,FREE}*()
 * OBD_FAIL_CHECK
 */
#include <obd.h>
#include <obd_support.h>

/* fid_cpu_to_be() */
#include <lustre_fid.h>

#include "osd_oi.h"
/* osd_lookup(), struct osd_thread_info */
#include "osd_internal.h"
#include "osd_igif.h"
#include "dt_object.h"

struct oi_descr {
        int   fid_size;
        char *name;
        __u32 oid;
};

/** to serialize concurrent OI index initialization */
static cfs_mutex_t oi_init_lock;

static struct dt_index_features oi_feat = {
        .dif_flags       = DT_IND_UPDATE,
        .dif_recsize_min = sizeof(struct osd_inode_id),
        .dif_recsize_max = sizeof(struct osd_inode_id),
        .dif_ptrsize     = 4
};

static const struct oi_descr oi_descr[OSD_OI_FID_NR] = {
        [OSD_OI_FID_16] = {
                .fid_size = sizeof(struct lu_fid),
                .name     = "oi.16",
                .oid      = OSD_OI_FID_16_OID
        }
};

struct dentry * osd_child_dentry_by_inode(const struct lu_env *env,
                                                 struct inode *inode,
                                                 const char *name,
                                                 const int namelen);

static int osd_oi_index_create_one(struct osd_thread_info *info,
                                   struct osd_device *osd, char *name,
                                   struct dt_index_features *feat)
{
        const struct lu_env *env = info->oti_env;
        struct osd_inode_id    *id     = &info->oti_id;
        struct buffer_head *bh;
        struct inode *inode;
        struct ldiskfs_dir_entry_2 *de;
        struct dentry *dentry;
        struct inode *dir;
        handle_t *jh;
        int rc;

        dentry = osd_child_dentry_by_inode(env, osd_sb(osd)->s_root->d_inode,
                                           name, strlen(name));
        dir = osd_sb(osd)->s_root->d_inode;
        bh = ll_ldiskfs_find_entry(dir, dentry, &de);
        if (bh) {
                brelse(bh);

                id->oii_ino = le32_to_cpu(de->inode);
                id->oii_gen = OSD_OII_NOGEN;

                inode = osd_iget(info, osd, id);
                if (!IS_ERR(inode)) {
                        iput(inode);
                        RETURN(-EEXIST);
                }
                RETURN(PTR_ERR(inode));
        }

        jh = ldiskfs_journal_start_sb(osd_sb(osd), 100);
        LASSERT(!IS_ERR(jh));

        inode = ldiskfs_create_inode(jh, osd_sb(osd)->s_root->d_inode,
                                    (S_IFREG | S_IXUGO)); 
        LASSERT(!IS_ERR(inode));

        if (feat->dif_flags & DT_IND_VARKEY)
                rc = iam_lvar_create(inode, feat->dif_keysize_max,
                                     feat->dif_ptrsize, feat->dif_recsize_max, jh);
        else
                rc = iam_lfix_create(inode, feat->dif_keysize_max,
                                     feat->dif_ptrsize, feat->dif_recsize_max, jh);

        dentry = osd_child_dentry_by_inode(env, osd_sb(osd)->s_root->d_inode,
                                           name, strlen(name));
        rc = ldiskfs_add_entry(jh, dentry, inode);
        LASSERT(rc == 0);

        ldiskfs_journal_stop(jh);
        iput(inode);

        return rc;
}

static int osd_oi_index_create(struct osd_thread_info *info,
                               struct osd_device *osd)
{
        const struct lu_env *env;
        struct lu_fid *oi_fid = &info->oti_fid;
        int i;
        int rc;

        env = info->oti_env;

        for (i = rc = 0; i < OSD_OI_FID_NR; ++i) {
                char *name;
                name = oi_descr[i].name;
                lu_local_obj_fid(oi_fid, oi_descr[i].oid);
                oi_feat.dif_keysize_min = oi_descr[i].fid_size;
                oi_feat.dif_keysize_max = oi_descr[i].fid_size;

                rc = osd_oi_index_create_one(info, osd, name, &oi_feat);
                
                if (rc == -ESTALE || rc != -EEXIST)
                        return(rc);
        }
        return 0;
}

int osd_oi_init(struct osd_thread_info *info,
                struct osd_oi *oi,
                struct osd_device *osd)
{
        struct dt_device *dev = &osd->od_dt_dev;
        const struct lu_env *env;
        int rc;
        int i;

        env = info->oti_env;
        cfs_mutex_lock(&oi_init_lock);
        memset(oi, 0, sizeof *oi);
retry:
        memset(oi, 0, sizeof *oi);
        for (i = rc = 0; i < OSD_OI_FID_NR && rc == 0; ++i) {
                const char       *name;
                struct dt_object *obj;

                name = oi_descr[i].name;
                oi_feat.dif_keysize_min = oi_descr[i].fid_size,
                oi_feat.dif_keysize_max = oi_descr[i].fid_size,

                obj = dt_store_open(env, dev, "", name, &info->oti_fid);
                if (!IS_ERR(obj)) {
                        rc = obj->do_ops->do_index_try(env, obj, &oi_feat);
                        if (rc == 0) {
                                LASSERT(obj->do_index_ops != NULL);
                                oi->oi_dir = obj;
                        } else {
                                CERROR("Wrong index \"%s\": %d\n", name, rc);
                                lu_object_put(env, &obj->do_lu);
                        }
                } else {
                        rc = PTR_ERR(obj);
                        if (rc == -ENOENT) {
                                rc = osd_oi_index_create(info, osd);
                                if (!rc)
                                        goto retry;
                        }
                        CERROR("Cannot open \"%s\": %d\n", name, rc);
                }
        }
        if (rc != 0)
                osd_oi_fini(info, oi);

        cfs_mutex_unlock(&oi_init_lock);
        return rc;
}

void osd_oi_fini(struct osd_thread_info *info, struct osd_oi *oi)
{
        if (oi->oi_dir != NULL) {
                lu_object_put(info->oti_env, &oi->oi_dir->do_lu);
                oi->oi_dir = NULL;
        }
}

static inline int fid_is_oi_fid(const struct lu_fid *fid)
{
        /* We need to filter-out oi obj's fid. As we can not store it, while
         * oi-index create operation.
         */
        return (unlikely(fid_seq(fid) == FID_SEQ_LOCAL_FILE &&
                fid_oid(fid) == OSD_OI_FID_16_OID));
}

int osd_oi_lookup(struct osd_thread_info *info, struct osd_device *osd,
                  const struct lu_fid *fid, struct osd_inode_id *id)
{
        struct osd_oi *oi     = &osd->od_oi;
        struct lu_fid *oi_fid = &info->oti_fid;
        int rc;

        if (fid_is_idif(fid)) {
                /* old OSD obj id */
                rc = osd_compat_objid_lookup(info, osd, fid, id);
        } else if (osd_fid_is_igif(fid)) {
                lu_igif_to_id(fid, id);
                rc = 0;
        } else {
                struct dt_object    *idx;
                const struct dt_key *key;

                if (fid_is_oi_fid(fid))
                        return -ENOENT;

                idx = oi->oi_dir;
                fid_cpu_to_be(oi_fid, fid);
                key = (struct dt_key *) oi_fid;

                LASSERT(idx);
                LASSERT(idx->do_index_ops);
                LASSERT(idx->do_index_ops->dio_lookup);

                rc = idx->do_index_ops->dio_lookup(info->oti_env, idx,
                                                   (struct dt_rec *)id, key,
                                                   BYPASS_CAPA);
                if (rc > 0) {
                        id->oii_ino = be32_to_cpu(id->oii_ino);
                        id->oii_gen = be32_to_cpu(id->oii_gen);
                        rc = 0;
                } else if (rc == 0)
                        rc = -ENOENT;
        }
        return rc;
}

int osd_oi_insert(struct osd_thread_info *info, struct osd_device *osd,
                  const struct lu_fid *fid, const struct osd_inode_id *id0,
                  struct thandle *th, int ignore_quota)
{
        struct lu_fid *oi_fid = &info->oti_fid;
        struct dt_object    *idx;
        struct osd_inode_id *id;
        const struct dt_key *key;

        if (osd_fid_is_igif(fid))
                return 0;

        if (fid_is_oi_fid(fid))
                return 0;

        if (fid_is_idif(fid))
                return osd_compat_objid_insert(info, osd, fid, id0, th);

        /* notice we don't return immediately, but continue to get into OI */
        if (unlikely(fid_seq(fid) == FID_SEQ_LOCAL_FILE))
                osd_compat_spec_insert(info, osd, fid, id0, th);

        idx = osd->od_oi.oi_dir;
        fid_cpu_to_be(oi_fid, fid);
        key = (struct dt_key *) oi_fid;

        id  = &info->oti_id;
        id->oii_ino = cpu_to_be32(id0->oii_ino);
        id->oii_gen = cpu_to_be32(id0->oii_gen);

        return idx->do_index_ops->dio_insert(info->oti_env, idx,
                                             (struct dt_rec *)id,
                                             key, th, BYPASS_CAPA,
                                             ignore_quota);
}

int osd_oi_delete(struct osd_thread_info *info,
                  struct osd_device *osd, const struct lu_fid *fid,
                  struct thandle *th)
{
        struct lu_fid *oi_fid = &info->oti_fid;
        struct dt_object    *idx;
        const struct dt_key *key;

        if (osd_fid_is_igif(fid))
                return 0;

        LASSERT(fid_seq(fid) != FID_SEQ_LOCAL_FILE);

        if (fid_is_idif(fid))
                return osd_compat_objid_delete(info, osd, fid, th);

        idx = osd->od_oi.oi_dir;
        fid_cpu_to_be(oi_fid, fid);
        key = (struct dt_key *) oi_fid;

        return idx->do_index_ops->dio_delete(info->oti_env, idx,
                                             key, th, BYPASS_CAPA);
}

int osd_oi_mod_init()
{
        cfs_mutex_init(&oi_init_lock);
        return 0;
}
