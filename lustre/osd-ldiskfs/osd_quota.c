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
 *
 * Author: Johann Lombardi <johann@whamcloud.com>
 * Author: Niu    Yawei    <niu@whamcloud.com>
 */

#include <lu_quota.h>
#include "osd_internal.h"

/**
 * Helpers function to find out the quota type (USRQUOTA/GRPQUOTA) of a
 * given object
 */
static inline int fid2type(const struct lu_fid *fid)
{
        LASSERT(fid_is_acct(fid));
        if (fid_oid(fid) == ACCT_GROUP_OID)
                return GRPQUOTA;
        return USRQUOTA;
}
static inline int obj2type(struct dt_object *obj)
{
        return fid2type(lu_object_fid(&obj->do_lu));
}

/**
 * Space Accounting Management
 */

/**
 * Look up an accounting object based on its fid.
 *
 * \param info - is the osd thread info passed by the caller
 * \param osd  - is the osd device
 * \param fid  - is the fid of the accounting object we want to look up
 * \param id   - is the osd_inode_id struct to fill with the inode number of
 *               the quota file if the lookup is successful
 */
int osd_acct_obj_lookup(struct osd_thread_info *info, struct osd_device *osd,
                        const struct lu_fid *fid, struct osd_inode_id *id)
{
        struct super_block *sb = osd_sb(osd);
        ENTRY;
        LASSERT(fid_is_acct(fid));

        if (!LDISKFS_HAS_RO_COMPAT_FEATURE(sb, LDISKFS_FEATURE_RO_COMPAT_QUOTA))
                RETURN(-ENOENT);

        id->oii_gen = OSD_OII_NOGEN;
        id->oii_ino = LDISKFS_SB(sb)->s_qf_inums[fid2type(fid)];
        if (!ldiskfs_valid_inum(sb, id->oii_ino))
                RETURN(-ENOENT);
        RETURN(0);
}

/**
 * Return space usage (#blocks & #inodes) consumed by a given uid or gid.
 *
 * \param env   - is the environment passed by the caller
 * \param dtobj - is the accounting object
 * \param dtrec - is the record to fill with space usage information
 * \param dtkey - is the id the of the user or group for which we would
 *                like to access disk usage.
 * \param capa - is the capability, not used.
 *
 * \retval +ve - success : exact match
 * \retval -ve - failure
 */
static int osd_acct_index_lookup(const struct lu_env *env,
                                 struct dt_object *dtobj,
                                 struct dt_rec *dtrec,
                                 const struct dt_key *dtkey,
                                 struct lustre_capa *capa)
{
        struct osd_thread_info *info = osd_oti_get(env);
        struct if_dqblk        *dqblk = &info->oti_dqblk;
        struct super_block     *sb = osd_sb(osd_obj2dev(osd_dt_obj(dtobj)));
        struct acct_rec        *rec = (struct acct_rec *)dtrec;
        __u64                   id = *((__u64 *)dtkey);
        int                     rc;
        ENTRY;

        memset((void *)dqblk, 0, sizeof(struct obd_dqblk));
        rc = sb->s_qcop->get_dqblk(sb, obj2type(dtobj), (qid_t) id, dqblk);
        if (rc)
                RETURN(rc);
        rec->bspace = dqblk->dqb_curspace;
        rec->ispace = dqblk->dqb_curinodes;
        RETURN(+1);
}

/**
 * Index and Iterator operations for accounting objects
 */
const struct dt_index_operations osd_acct_index_ops = {
        .dio_lookup         = osd_acct_index_lookup,
#if 0
        /* TODO Iterator support to be implemented in ORI-314 */
        .dio_it     = {
                .init     = ,
                .fini     = ,
                .get      = ,
                .put      = ,
                .next     = ,
                .key      = ,
                .key_size = ,
                .rec      = ,
                .load     = 
        }
#endif
};

/**
 * Quota Enforcement Management
 * TODO
 */
