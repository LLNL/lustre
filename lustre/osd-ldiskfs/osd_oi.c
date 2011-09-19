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
                                   struct osd_device *osd, const char *name,
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
                                     (S_IFREG | S_IRUGO | S_IWUSR));
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

static struct inode *osd_oi_index_open(struct osd_thread_info *info,
                                       struct osd_device *osd,
                                       const char *name,
                                       struct dt_index_features *f)
{
        struct dentry *dentry;
        struct inode  *inode;
        int            rc;

        dentry = ll_lookup_one_len(name, osd_sb(osd)->s_root, strlen(name));
        if (IS_ERR(dentry))
                return (void *) dentry;

        if (dentry->d_inode) {
                LASSERT(!is_bad_inode(dentry->d_inode));
                inode = dentry->d_inode;
                atomic_inc(&inode->i_count);
                dput(dentry);
                return inode;
        }

        /* create */
        dput(dentry);
        shrink_dcache_parent(osd_sb(osd)->s_root);

        rc = osd_oi_index_create_one(info, osd, name, f);
        if (rc)
                RETURN(ERR_PTR(rc));

        dentry = ll_lookup_one_len(name, osd_sb(osd)->s_root, strlen(name));
        if (IS_ERR(dentry))
                return (void *) dentry;

        if (dentry->d_inode) {
                LASSERT(!is_bad_inode(dentry->d_inode));
                inode = dentry->d_inode;
        	atomic_inc(&inode->i_count);
                dput(dentry);
                return inode;
        }

        return ERR_PTR(-ENOENT);
}

int osd_oi_init(struct osd_thread_info *info,
                struct osd_oi *oi,
                struct osd_device *osd)
{
        const struct lu_env *env;
        struct inode *inode;
        int rc;
        int i;

        env = info->oti_env;
        cfs_mutex_lock(&oi_init_lock);
        memset(oi, 0, sizeof *oi);
        for (i = rc = 0; i < OSD_OI_FID_NR && rc == 0; ++i) {
                const char       *name;
                struct osd_directory *dir;
                struct iam_container *bag;

                name = oi_descr[i].name;
                oi_feat.dif_keysize_min = oi_descr[i].fid_size,
                oi_feat.dif_keysize_max = oi_descr[i].fid_size,

                inode = osd_oi_index_open(info, osd, name, &oi_feat);
                if (IS_ERR(inode)) {
                        rc = PTR_ERR(inode);
                        break;
                }

                oi->oi_inode = inode;
                dir = &oi->oi_dir;

                bag = &dir->od_container;
                rc = iam_container_init(bag, &dir->od_descr, inode);
                if (rc)
                        break;
                rc = iam_container_setup(bag);
        }
        if (rc != 0)
                osd_oi_fini(info, oi);

        cfs_mutex_unlock(&oi_init_lock);
        return rc;
}

void osd_oi_fini(struct osd_thread_info *info, struct osd_oi *oi)
{
        struct iam_container *bag;

        if (oi->oi_inode != NULL) {
                bag = &oi->oi_dir.od_container;
                if (bag->ic_object == oi->oi_inode)
                        iam_container_fini(bag);
                iput(oi->oi_inode);
                oi->oi_inode = NULL;
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

static inline int fid_is_fs_root(const struct lu_fid *fid)
{
        /* Map root inode to special local object FID */
        return (unlikely(fid_seq(fid) == FID_SEQ_LOCAL_FILE &&
                fid_oid(fid) == OSD_FS_ROOT_OID));
}

int osd_fid_unpack(struct lu_fid *fid, const struct osd_fid_pack *pack);

static int osd_oi_iam_lookup(struct osd_thread_info *oti,
                             struct osd_device *osd,
                             struct dt_rec *rec,
                             const struct dt_key *key)
{
        struct iam_container  *bag = &osd->od_oi.oi_dir.od_container;
        struct iam_iterator   *it = &oti->oti_idx_it;
        struct inode          *inode = osd->od_oi.oi_inode;
        struct iam_rec        *iam_rec;
        struct iam_path_descr *ipd;
        int rc;
        ENTRY;

        ipd = osd_idx_ipd_get(oti->oti_env, bag);
        if (IS_ERR(ipd))
                RETURN(-ENOMEM);

        /* got ipd now we can start iterator. */
        iam_it_init(it, bag, 0, ipd);

        rc = iam_it_get(it, (struct iam_key *)key);
        if (rc >= 0) {
                if (S_ISDIR(inode->i_mode))
                        iam_rec = (struct iam_rec *)oti->oti_ldp;
                else
                        iam_rec = (struct iam_rec *) rec;

                iam_reccpy(&it->ii_path.ip_leaf, (struct iam_rec *)iam_rec);
                if (S_ISDIR(inode->i_mode))
                        osd_fid_unpack((struct lu_fid *) rec,
                                       (struct osd_fid_pack *)iam_rec);
        }
        iam_it_put(it);
        iam_it_fini(it);
        osd_ipd_put(oti->oti_env, bag, ipd);

        LINVRNT(osd_invariant(obj));

        RETURN(rc);
}

int osd_oi_lookup(struct osd_thread_info *info, struct osd_device *osd,
                  const struct lu_fid *fid, struct osd_inode_id *id)
{
        struct lu_fid *oi_fid = &info->oti_fid;
        const struct   dt_key *key;
        int            rc = 0;

        if (fid_is_idif(fid) || fid_seq(fid) == FID_SEQ_LLOG) {
                /* old OSD obj id */
                rc = osd_compat_objid_lookup(info, osd, fid, id);
        } else if (fid_is_igif(fid)) {
                lu_igif_to_id(fid, id);
                rc = 0;
        } else if (fid_is_fs_root(fid)) {
                struct inode *inode = osd_sb(osd)->s_root->d_inode;
                id->oii_ino = inode->i_ino;
                id->oii_gen = inode->i_generation;
        } else {

                if (fid_is_oi_fid(fid))
                        return -ENOENT;

                if (unlikely(fid_is_acct(fid)))
                        return osd_acct_obj_lookup(info, osd, fid, id);

                if (unlikely(fid_seq(fid) == FID_SEQ_LOCAL_FILE)) {
                        rc = osd_compat_spec_lookup(info, osd, fid, id);
                        if (rc == 0 || rc != -ERESTART)
                                goto out;
                }

                fid_cpu_to_be(oi_fid, fid);
                key = (struct dt_key *) oi_fid;

                rc = osd_oi_iam_lookup(info, osd, (struct dt_rec *) id, key);

                if (rc > 0) {
                        id->oii_ino = be32_to_cpu(id->oii_ino);
                        id->oii_gen = be32_to_cpu(id->oii_gen);
                        rc = 0;
                } else if (rc == 0)
                        rc = -ENOENT;
        }

out:
        return rc;
}

void osd_fid_pack(struct osd_fid_pack *pack, const struct dt_rec *fid,
                  struct lu_fid *befider);

static int osd_oi_iam_insert(struct osd_thread_info *oti, struct osd_device *osd,
                             const struct dt_rec *rec, const struct dt_key *key,
                             struct thandle *th)
{
        struct iam_container  *bag = &osd->od_oi.oi_dir.od_container;
        struct iam_rec        *iam_rec = (struct iam_rec *)oti->oti_ldp;
        struct inode          *inode = osd->od_oi.oi_inode;
        struct iam_path_descr *ipd;
        struct osd_thandle    *oh;
        int                    rc;
        ENTRY;

        ipd = osd_idx_ipd_get(oti->oti_env, bag);
        if (unlikely(ipd == NULL))
                RETURN(-ENOMEM);

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle != NULL);
        LASSERT(oh->ot_handle->h_transaction != NULL);
#ifdef HAVE_QUOTA_SUPPORT
        if (ignore_quota)
                current->cap_effective |= CFS_CAP_SYS_RESOURCE_MASK;
        else
                current->cap_effective &= ~CFS_CAP_SYS_RESOURCE_MASK;
#endif
        if (S_ISDIR(inode->i_mode))
                osd_fid_pack((struct osd_fid_pack *)iam_rec, rec, &oti->oti_fid);
        else
                iam_rec = (struct iam_rec *) rec;
        rc = iam_insert(oh->ot_handle, bag, (const struct iam_key *)key,
                        iam_rec, ipd);
#ifdef HAVE_QUOTA_SUPPORT
        current->cap_effective = save;
#endif
        osd_ipd_put(oti->oti_env, bag, ipd);
        LINVRNT(osd_invariant(obj));
        RETURN(rc);
}

int osd_oi_insert(struct osd_thread_info *info, struct osd_device *osd,
                  const struct lu_fid *fid, const struct osd_inode_id *id0,
                  struct thandle *th, int ignore_quota)
{
        struct lu_fid *oi_fid = &info->oti_fid;
        struct osd_inode_id *id;
        const struct dt_key *key;

        if (fid_is_igif(fid))
                return 0;

        if (fid_is_oi_fid(fid))
                return 0;

        if (fid_is_idif(fid) || fid_seq(fid) == FID_SEQ_LLOG)
                return osd_compat_objid_insert(info, osd, fid, id0, th);

        /* notice we don't return immediately, but continue to get into OI */
        if (unlikely(fid_seq(fid) == FID_SEQ_LOCAL_FILE))
                osd_compat_spec_insert(info, osd, fid, id0, th);

        fid_cpu_to_be(oi_fid, fid);
        key = (struct dt_key *) oi_fid;

        id  = &info->oti_id;
        id->oii_ino = cpu_to_be32(id0->oii_ino);
        id->oii_gen = cpu_to_be32(id0->oii_gen);

        return osd_oi_iam_insert(info, osd, (struct dt_rec *) id, key, th);
}

static int osd_oi_iam_delete(struct osd_thread_info *oti,
                             struct osd_device *osd,
                             const struct dt_key *key,
                             struct thandle *handle)
{
        struct iam_container  *bag = &osd->od_oi.oi_dir.od_container;
        struct iam_path_descr *ipd;
        struct osd_thandle    *oh;
        int                    rc;
        ENTRY;

        ipd = osd_idx_ipd_get(oti->oti_env, bag);
        if (unlikely(ipd == NULL))
                RETURN(-ENOMEM);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle != NULL);
        LASSERT(oh->ot_handle->h_transaction != NULL);

        rc = iam_delete(oh->ot_handle, bag, (const struct iam_key *)key, ipd);
        osd_ipd_put(oti->oti_env, bag, ipd);
        LINVRNT(osd_invariant(obj));
        RETURN(rc);
}

int osd_oi_delete(struct osd_thread_info *info,
                  struct osd_device *osd, const struct lu_fid *fid,
                  struct thandle *th)
{
        struct lu_fid *oi_fid = &info->oti_fid;
        const struct dt_key *key;

        if (fid_is_igif(fid))
                return 0;

        LASSERT(fid_seq(fid) != FID_SEQ_LOCAL_FILE);

        if (fid_is_idif(fid) || fid_seq(fid) == FID_SEQ_LLOG)
                return osd_compat_objid_delete(info, osd, fid, th);

        fid_cpu_to_be(oi_fid, fid);
        key = (struct dt_key *) oi_fid;

        return osd_oi_iam_delete(info, osd, key, th);
}

int osd_oi_mod_init()
{
        cfs_mutex_init(&oi_init_lock);
        return 0;
}
