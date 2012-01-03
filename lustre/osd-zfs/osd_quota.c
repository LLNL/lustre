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
 */

#include <lu_quota.h>
#include <obd.h>
#include "udmu.h"
#include "osd_internal.h"

/**
 * Helper function to retrieve DMU object id from fid for accounting object
 */
uint64_t osd_quota_fid2dmu(const struct lu_fid *fid)
{
        LASSERT(fid_is_acct(fid));
        if (fid_oid(fid) == ACCT_GROUP_OID)
                return DMU_GROUPUSED_OBJECT;
        return DMU_USERUSED_OBJECT;
}

/**
 * Space Accounting Management
 */

/**
 * Return space usage consumed by a given uid or gid.
 * Block usage is accurrate since it is maintained by DMU itself.
 * However, DMU does not provide inode accounting, so the #inodes in use
 * is estimated from the block usage and statfs information.
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
        char                   *buf  = info->oti_buf;
        struct acct_rec        *rec  = (struct acct_rec *)dtrec;
        struct lu_object       *lo   = &dtobj->do_lu;
        struct osd_device      *osd  = osd_dev(lo->lo_dev);
        int                     rc;
        uint64_t                oid;
        ENTRY;

        /* convert the 64-bit uid/gid into a string */
        sprintf(buf, "%llx", *((__u64 *)dtkey));
        /* fetch DMU object ID (DMU_USERUSED_OBJECT/DMU_GROUPUSED_OBJECT) to be
         * used */
        oid = osd_quota_fid2dmu(lu_object_fid(lo));

        /* disk usage (in bytes) is maintained by DMU.
         * DMU_USERUSED_OBJECT/DMU_GROUPUSED_OBJECT are special objects which
         * not associated with any dmu_but_t (see dnode_special_open()).
         * As a consequence, we cannot use udmu_zap_lookup() here since it
         * requires a valid oo_db. */
        rc = zap_lookup(osd->od_objset.os, oid, buf, sizeof(uint64_t), 1,
                        &rec->bspace);
        if (rc == ENOENT) {
                /* user/group has not created anything yet */
                CDEBUG(D_QUOTA, "%s: id %s not found, assuming 0\n",
                       osd->od_dt_dev.dd_lu_dev.ld_obd->obd_name, buf);
                rec->ispace = rec->bspace = 0;
                RETURN(+1);
        }
        if (rc)
                RETURN(-rc);

        /* as for inode accounting, it is not maintained by DMU, so we just
         * estimate inode usage from the block usage & statfs information */
        rec->ispace = udmu_objset_user_iused(&osd->od_objset, rec->bspace);
        RETURN(+1);
}

/**
 * Initialize osd Iterator for given osd index object.
 *
 * \param  dt    - osd index object
 * \param  attr  - not used
 * \param  capa  - BYPASS_CAPA
 */
static struct dt_it *osd_it_acct_init(const struct lu_env *env,
                                      struct dt_object *dt,
                                      __u32 attr,
                                      struct lustre_capa *capa)
{
        struct osd_thread_info *info = osd_oti_get(env);
        struct osd_it_quota    *it;
        struct lu_object       *lo   = &dt->do_lu;
        struct osd_device      *osd  = osd_dev(lo->lo_dev);
        int                     rc;
        ENTRY;

        LASSERT(lu_object_exists(lo));

        if (info == NULL)
                RETURN(ERR_PTR(-ENOMEM));

        it = &info->oti_it_quota;
        memset(it, 0, sizeof(*it));
        it->oiq_oid = osd_quota_fid2dmu(lu_object_fid(lo));

        /* initialize zap cursor */
        rc = udmu_zap_cursor_init(&it->oiq_zc, &osd->od_objset, it->oiq_oid, 0);
        if (rc)
                RETURN(ERR_PTR(-rc));

        /* take object reference */
        lu_object_get(lo);
        it->oiq_obj   = osd_dt_obj(dt);
        it->oiq_reset = 1;

        RETURN((struct dt_it *)it);
}

/**
 * Free given iterator.
 *
 * \param  di   - osd iterator
 */
static void osd_it_acct_fini(const struct lu_env *env, struct dt_it *di)
{
        struct osd_it_quota *it = (struct osd_it_quota *)di;
        ENTRY;
        udmu_zap_cursor_fini(it->oiq_zc);
        lu_object_put(env, &it->oiq_obj->oo_dt.do_lu);
        EXIT;
}

/**
 * Move Iterator to record specified by \a key, if the \a key isn't found,
 * move to the first valid record.
 *
 * \param  di   - osd iterator
 * \param  key  - uid or gid
 *
 * \retval +ve  - di points to the first valid record
 * \retval  0   - di points to exact matched key
 * \retval -ve  - failure
 */
static int osd_it_acct_get(const struct lu_env *env, struct dt_it *di,
                           const struct dt_key *key)
{
        struct osd_it_quota    *it  = (struct osd_it_quota *)di;
        struct osd_device      *osd = osd_obj2dev(it->oiq_obj);
        zap_cursor_t           *zc;
        int                     rc;
        ENTRY;

        /* XXX: like osd_zap_it_get(), API is currently broken */
        LASSERT(*((__u64 *)key) == 0);

        /* create new cursor pointing to the new key */
        rc = udmu_zap_cursor_init(&zc, &osd->od_objset, it->oiq_oid, 0);
        if (rc)
                RETURN(-rc);

        /* kill old zap cursor and replace with the new one */
        udmu_zap_cursor_fini(it->oiq_zc);
        it->oiq_zc    = zc;
        it->oiq_reset = 1;

        /* always return 1 due to broken API */
        RETURN(+1);
}

/**
 * Release Iterator
 *
 * \param  di   - osd iterator
 */
static void osd_it_acct_put(const struct lu_env *env, struct dt_it *di)
{
}

/**
 * Move on to the next valid entry.
 *
 * \param  di   - osd iterator
 *
 * \retval +ve  - iterator reached the end
 * \retval   0  - iterator has not reached the end yet
 * \retval -ve  - unexpected failure
 */
static int osd_it_acct_next(const struct lu_env *env, struct dt_it *di)
{
        struct osd_it_quota *it = (struct osd_it_quota *)di;
        int                  rc;
        ENTRY;

        if (it->oiq_reset == 0)
                zap_cursor_advance(it->oiq_zc);
        it->oiq_reset = 0;
        rc = udmu_zap_cursor_retrieve_key(env, it->oiq_zc, NULL, 32);
        if (rc == ENOENT) /* reached the end */
                RETURN(+1);
        RETURN(-rc);
}

/**
 * Return pointer to the key under iterator.
 *
 * \param  di   - osd iterator
 */
static struct dt_key *osd_it_acct_key(const struct lu_env *env,
                                      const struct dt_it *di)
{
        struct osd_it_quota    *it = (struct osd_it_quota *)di;
        struct osd_thread_info *info = osd_oti_get(env);
        char                   *buf  = info->oti_buf;
        char                   *p;
        int                     rc;
        ENTRY;

        it->oiq_reset = 0;
        rc = udmu_zap_cursor_retrieve_key(env, it->oiq_zc, buf, 32);
        if (rc)
                RETURN(ERR_PTR(-rc));
        it->oiq_id = simple_strtoull(buf, &p, 10);
        RETURN((struct dt_key *) &it->oiq_id);
}

/**
 * Return size of key under iterator (in bytes)
 *
 * \param  di   - osd iterator
 */
static int osd_it_acct_key_size(const struct lu_env *env,
                                const struct dt_it *di)
{
        ENTRY;
        RETURN((int)sizeof(uint64_t));
}

/**
 * Return pointer to the record under iterator.
 *
 * \param  di    - osd iterator
 * \param  attr  - not used
 */
static int osd_it_acct_rec(const struct lu_env *env,
                           const struct dt_it *di,
                           struct dt_rec *dtrec, __u32 attr)
{
        struct osd_it_quota *it = (struct osd_it_quota *)di;
        struct acct_rec     *rec  = (struct acct_rec *)dtrec;
        struct osd_device   *osd = osd_obj2dev(it->oiq_obj);
        int                  bytes_read;
        int                  rc;
        ENTRY;

        it->oiq_reset = 0;
        rc = udmu_zap_cursor_retrieve_value(env, it->oiq_zc, (char *)&rec->bspace,
                                            sizeof(uint64_t), &bytes_read);
        if (rc)
                RETURN(-rc);

        /* estimate #inodes in use */
        rec->ispace = udmu_objset_user_iused(&osd->od_objset, rec->bspace);
        RETURN(0);
}

/**
 * Returns cookie for current Iterator position.
 *
 * \param  di    - osd iterator
 */
static __u64 osd_it_acct_store(const struct lu_env *env,
                               const struct dt_it *di)
{
        struct osd_it_quota *it = (struct osd_it_quota *)di;
        ENTRY;
        it->oiq_reset = 0;
        RETURN(udmu_zap_cursor_serialize(it->oiq_zc));
}

/**
 * Restore iterator from cookie. if the \a hash isn't found,
 * restore the first valid record.
 *
 * \param  di    - osd iterator
 * \param  hash  - iterator location cookie
 *
 * \retval +ve   - di points to the first valid record
 * \retval  0    - di points to exact matched hash
 * \retval -ve   - failure
 */
static int osd_it_acct_load(const struct lu_env *env,
                            const struct dt_it *di, __u64 hash)
{
        struct osd_it_quota *it  = (struct osd_it_quota *)di;
        struct osd_device   *osd = osd_obj2dev(it->oiq_obj);
        zap_cursor_t        *zc;
        int                  rc;
        ENTRY;

        /* create new cursor pointing to the new hash */
        rc = udmu_zap_cursor_init(&zc, &osd->od_objset, it->oiq_oid, hash);
        if (rc)
                RETURN(-rc);
        udmu_zap_cursor_fini(it->oiq_zc);
        it->oiq_zc = zc;
        it->oiq_reset = 0;

        rc = udmu_zap_cursor_retrieve_key(env, it->oiq_zc, NULL, 32);
        if (rc > 0)
                RETURN(-rc);
        /* compare hash and return 0 for exact match */
        if (hash == udmu_zap_cursor_serialize(it->oiq_zc))
                RETURN(0);
        else
                RETURN(+1);
}

/**
 * Index and Iterator operations for accounting objects
 */
const struct dt_index_operations osd_acct_index_ops = {
        .dio_lookup = osd_acct_index_lookup,
        .dio_it     = {
                .init     = osd_it_acct_init,
                .fini     = osd_it_acct_fini,
                .get      = osd_it_acct_get,
                .put      = osd_it_acct_put,
                .next     = osd_it_acct_next,
                .key      = osd_it_acct_key,
                .key_size = osd_it_acct_key_size,
                .rec      = osd_it_acct_rec,
                .store    = osd_it_acct_store,
                .load     = osd_it_acct_load
        }
};

/**
 * Quota Enforcement Management
 * TODO
 */
