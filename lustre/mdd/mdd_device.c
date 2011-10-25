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
 * Copyright (c) 2011 Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mdd/mdd_device.c
 *
 * Lustre Metadata Server (mdd) routines
 *
 * Author: Wang Di <wangdi@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lprocfs_status.h>

#include <lustre_disk.h>
#include <lustre_fid.h>
#include <lustre_mds.h>
#include <lustre/lustre_idl.h>
#include <lustre_disk.h>      /* for changelogs */
#include <lustre_param.h>
#include <lustre_fid.h>

#include "mdd_internal.h"

const struct md_device_operations mdd_ops;
static struct lu_device_type mdd_device_type;

static const char mdd_root_dir_name[] = "ROOT";
static const char mdd_obf_dir_name[] = "fid";

/* Slab for OSD object allocation */
cfs_mem_cache_t *mdd_object_kmem;

static struct lu_kmem_descr mdd_caches[] = {
        {
                .ckd_cache = &mdd_object_kmem,
                .ckd_name  = "mdd_obj",
                .ckd_size  = sizeof(struct mdd_object)
        },
        {
                .ckd_cache = NULL
        }
};

static int mdd_connect_to_next(const struct lu_env *env, struct mdd_device *m,
                               const char *nextdev)
{
        struct obd_connect_data *data = NULL;
        struct obd_device       *obd;
        int                      rc;
        ENTRY;

        LASSERT(m->mdd_child_exp == NULL);

        OBD_ALLOC(data, sizeof(*data));
        if (data == NULL)
                GOTO(out, rc = -ENOMEM);

        obd = class_name2obd(nextdev);
        if (obd == NULL) {
                CERROR("can't locate next device: %s\n", nextdev);
                GOTO(out, rc = -ENOTCONN);
        }

        data->ocd_connect_flags = OBD_CONNECT_VERSION;
        data->ocd_version = LUSTRE_VERSION_CODE;

        rc = obd_connect(NULL, &m->mdd_child_exp, obd, &obd->obd_uuid, data, NULL);
        if (rc) {
                CERROR("cannot connect to next dev %s (%d)\n", nextdev, rc);
                GOTO(out, rc);
        }

        m->mdd_md_dev.md_lu_dev.ld_site =
                m->mdd_child_exp->exp_obd->obd_lu_dev->ld_site;
        LASSERT(m->mdd_md_dev.md_lu_dev.ld_site);
        m->mdd_child = lu2dt_dev(m->mdd_child_exp->exp_obd->obd_lu_dev);

out:
        if (data)
                OBD_FREE(data, sizeof(*data));
        RETURN(rc);
}

static int mdd_init0(const struct lu_env *env, struct mdd_device *mdd,
                     struct lu_device_type *t, struct lustre_cfg *lcfg)
{
        struct obd_type           *obdtype = NULL;
        int rc;
        ENTRY;

        md_device_init(&mdd->mdd_md_dev, t);

        rc = mdd_connect_to_next(env, mdd, lustre_cfg_string(lcfg, 3));
        if (rc)
                RETURN(rc);

        mdd->mdd_atime_diff = MAX_ATIME_DIFF;
        /* sync permission changes */
        mdd->mdd_sync_permission = 1;

        mdd->mdd_child->dd_ops->dt_conf_get(env, mdd->mdd_child, &mdd->mdd_dt_conf);
#ifdef XXX_MDD_CHANGELOG
        if (rc == 0)
                mdd_changelog_init(env, mdd);
#endif
        if (rc && obdtype)
                class_put_type(obdtype);

        RETURN(rc);
}

static void mdd_device_shutdown(const struct lu_env *env,
                                struct mdd_device *m, struct lustre_cfg *cfg)
{
        ENTRY;
#ifdef XXX_MDD_CHANGELOG
        mdd_changelog_fini(env, m);
#endif
        orph_index_fini(env, m);
        if (m->mdd_dot_lustre_objs.mdd_obf)
                mdd_object_put(env, m->mdd_dot_lustre_objs.mdd_obf);
        if (m->mdd_dot_lustre)
                mdd_object_put(env, m->mdd_dot_lustre);
        lu_site_purge(env, mdd2lu_dev(m)->ld_site, ~0);
        EXIT;
}

static struct lu_device *mdd_device_fini(const struct lu_env *env,
                                         struct lu_device *d)
{
        struct mdd_device *mdd = lu2mdd_dev(d);
        struct lu_device *next = &mdd->mdd_child->dd_lu_dev;
        int rc;

        rc = mdd_procfs_fini(mdd);
        if (rc) {
                CERROR("proc fini error %d \n", rc);
                return ERR_PTR(rc);
        }

        obd_disconnect(mdd->mdd_child_exp);

        return next;
}

#ifdef XXX_MDD_CHANGELOG
static int changelog_init_cb(struct llog_handle *llh, struct llog_rec_hdr *hdr,
                             void *data)
{
        struct mdd_device *mdd = (struct mdd_device *)data;
        struct llog_changelog_rec *rec = (struct llog_changelog_rec *)hdr;
        ENTRY;

        LASSERT(llh->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN);
        LASSERT(rec->cr_hdr.lrh_type == CHANGELOG_REC);

        CDEBUG(D_INFO,
               "seeing record at index %d/%d/"LPU64" t=%x %.*s in log "LPX64"\n",
               hdr->lrh_index, rec->cr_hdr.lrh_index, rec->cr.cr_index,
               rec->cr.cr_type, rec->cr.cr_namelen, rec->cr.cr_name,
               llh->lgh_id.lgl_oid);

        mdd->mdd_cl.mc_index = rec->cr.cr_index;
        RETURN(LLOG_PROC_BREAK);
}

static int changelog_user_init_cb(struct llog_handle *llh,
                                  struct llog_rec_hdr *hdr, void *data)
{
        struct mdd_device *mdd = (struct mdd_device *)data;
        struct llog_changelog_user_rec *rec =
                (struct llog_changelog_user_rec *)hdr;
        ENTRY;

        LASSERT(llh->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN);
        LASSERT(rec->cur_hdr.lrh_type == CHANGELOG_USER_REC);

        CDEBUG(D_INFO, "seeing user at index %d/%d id=%d endrec="LPU64
               " in log "LPX64"\n", hdr->lrh_index, rec->cur_hdr.lrh_index,
               rec->cur_id, rec->cur_endrec, llh->lgh_id.lgl_oid);

        cfs_spin_lock(&mdd->mdd_cl.mc_user_lock);
        mdd->mdd_cl.mc_lastuser = rec->cur_id;
        cfs_spin_unlock(&mdd->mdd_cl.mc_user_lock);

        RETURN(LLOG_PROC_BREAK);
}

static int mdd_changelog_llog_init(struct mdd_device *mdd)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_ctxt *ctxt;
        int rc;

        if (obd == NULL)
                RETURN(-ENODEV);

        /* Find last changelog entry number */
        ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
        if (ctxt == NULL) {
                return -EINVAL;
        }
        if (!ctxt->loc_handle) {
                llog_ctxt_put(ctxt);
                return -EINVAL;
        }

        rc = llog_cat_reverse_process(ctxt->loc_handle, changelog_init_cb, mdd);
        llog_ctxt_put(ctxt);

        if (rc < 0) {
                CERROR("changelog init failed: %d\n", rc);
                return rc;
        }
        CDEBUG(D_IOCTL, "changelog starting index="LPU64"\n",
               mdd->mdd_cl.mc_index);

        /* Find last changelog user id */
        ctxt = llog_get_context(obd, LLOG_CHANGELOG_USER_ORIG_CTXT);
        if (ctxt == NULL) {
                CERROR("no changelog user context\n");
                return -EINVAL;
        }
        if (!ctxt->loc_handle) {
                llog_ctxt_put(ctxt);
                return -EINVAL;
        }

        rc = llog_cat_reverse_process(ctxt->loc_handle, changelog_user_init_cb,
                                      mdd);
        llog_ctxt_put(ctxt);

        if (rc < 0) {
                CERROR("changelog user init failed: %d\n", rc);
                return rc;
        }

        /* If we have registered users, assume we want changelogs on */
        if (mdd->mdd_cl.mc_lastuser > 0)
                rc = mdd_changelog_on(mdd, 1);

        return rc;
}

static int mdd_changelog_init(const struct lu_env *env, struct mdd_device *mdd)
{
        int rc;

        mdd->mdd_cl.mc_index = 0;
        cfs_spin_lock_init(&mdd->mdd_cl.mc_lock);
        cfs_waitq_init(&mdd->mdd_cl.mc_waitq);
        mdd->mdd_cl.mc_starttime = cfs_time_current_64();
        mdd->mdd_cl.mc_flags = 0; /* off by default */
        mdd->mdd_cl.mc_mask = CHANGELOG_DEFMASK;
        cfs_spin_lock_init(&mdd->mdd_cl.mc_user_lock);
        mdd->mdd_cl.mc_lastuser = 0;

        rc = mdd_changelog_llog_init(mdd);
        if (rc) {
                mdd->mdd_cl.mc_flags |= CLM_ERR;
        }
        return rc;
}

static void mdd_changelog_fini(const struct lu_env *env, struct mdd_device *mdd)
{
        mdd->mdd_cl.mc_flags = 0;
}

/* Start / stop recording */
int mdd_changelog_on(struct mdd_device *mdd, int on)
{
        int rc = 0;

        if ((on == 1) && ((mdd->mdd_cl.mc_flags & CLM_ON) == 0)) {
                LCONSOLE_INFO("%s: changelog on\n", mdd2obd_dev(mdd)->obd_name);
                if (mdd->mdd_cl.mc_flags & CLM_ERR) {
                        CERROR("Changelogs cannot be enabled due to error "
                               "condition (see %s log).\n",
                               mdd2obd_dev(mdd)->obd_name);
                        rc = -ESRCH;
                } else {
                        cfs_spin_lock(&mdd->mdd_cl.mc_lock);
                        mdd->mdd_cl.mc_flags |= CLM_ON;
                        cfs_spin_unlock(&mdd->mdd_cl.mc_lock);
                        rc = mdd_changelog_write_header(mdd, CLM_START);
                }
        } else if ((on == 0) && ((mdd->mdd_cl.mc_flags & CLM_ON) == CLM_ON)) {
                LCONSOLE_INFO("%s: changelog off\n",mdd2obd_dev(mdd)->obd_name);
                rc = mdd_changelog_write_header(mdd, CLM_FINI);
                cfs_spin_lock(&mdd->mdd_cl.mc_lock);
                mdd->mdd_cl.mc_flags &= ~CLM_ON;
                cfs_spin_unlock(&mdd->mdd_cl.mc_lock);
        }
        return rc;
}

static __u64 cl_time(void) {
        cfs_fs_time_t time;

        cfs_fs_time_current(&time);
        return (((__u64)time.tv_sec) << 30) + time.tv_nsec;
}

/** Add a changelog entry \a rec to the changelog llog
 * \param mdd
 * \param rec
 * \param handle - currently ignored since llogs start their own transaction;
 *                 this will hopefully be fixed in llog rewrite
 * \retval 0 ok
 */
int mdd_changelog_llog_write(struct mdd_device         *mdd,
                             struct llog_changelog_rec *rec,
                             struct thandle            *handle)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_ctxt *ctxt;
        int rc;

        rec->cr_hdr.lrh_len = llog_data_len(sizeof(*rec) + rec->cr.cr_namelen);
        /* llog_lvfs_write_rec sets the llog tail len */
        rec->cr_hdr.lrh_type = CHANGELOG_REC;
        rec->cr.cr_time = cl_time();
        cfs_spin_lock(&mdd->mdd_cl.mc_lock);
        /* NB: I suppose it's possible llog_add adds out of order wrt cr_index,
           but as long as the MDD transactions are ordered correctly for e.g.
           rename conflicts, I don't think this should matter. */
        rec->cr.cr_index = ++mdd->mdd_cl.mc_index;
        cfs_spin_unlock(&mdd->mdd_cl.mc_lock);
        ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
        if (ctxt == NULL)
                return -ENXIO;

        /* nested journal transaction */
        rc = llog_add_2(ctxt, &rec->cr_hdr, NULL, NULL, 0, handle);
        llog_ctxt_put(ctxt);

        cfs_waitq_signal(&mdd->mdd_cl.mc_waitq);

        return rc;
}

/** Remove entries with indicies up to and including \a endrec from the
 *  changelog
 * \param mdd
 * \param endrec
 * \retval 0 ok
 */
int mdd_changelog_llog_cancel(struct mdd_device *mdd, long long endrec)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_ctxt *ctxt;
        long long unsigned cur;
        int rc;

        ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
        if (ctxt == NULL)
                return -ENXIO;

        cfs_spin_lock(&mdd->mdd_cl.mc_lock);
        cur = (long long)mdd->mdd_cl.mc_index;
        cfs_spin_unlock(&mdd->mdd_cl.mc_lock);
        if (endrec > cur)
                endrec = cur;

        /* purge to "0" is shorthand for everything */
        if (endrec == 0)
                endrec = cur;

        /* If purging all records, write a header entry so we don't have an
           empty catalog and we're sure to have a valid starting index next
           time.  In case of crash, we just restart with old log so we're
           allright. */
        if (endrec == cur) {
                /* XXX: transaction is started by llog itself */
                rc = mdd_changelog_write_header(mdd, CLM_PURGE);
                if (rc)
                      goto out;
        }

        /* Some records were purged, so reset repeat-access time (so we
           record new mtime update records, so users can see a file has been
           changed since the last purge) */
        mdd->mdd_cl.mc_starttime = cfs_time_current_64();

        /* XXX: transaction is started by llog itself */
        rc = llog_cancel(ctxt, NULL, 1, (struct llog_cookie *)&endrec, 0);
out:
        llog_ctxt_put(ctxt);
        return rc;
}

/** Add a CL_MARK record to the changelog
 * \param mdd
 * \param markerflags - CLM_*
 * \retval 0 ok
 */
int mdd_changelog_write_header(struct mdd_device *mdd, int markerflags)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_changelog_rec *rec;
        int reclen;
        int len = strlen(obd->obd_name);
        int rc;
        ENTRY;

        reclen = llog_data_len(sizeof(*rec) + len);
        OBD_ALLOC(rec, reclen);
        if (rec == NULL)
                RETURN(-ENOMEM);

        rec->cr.cr_flags = CLF_VERSION;
        rec->cr.cr_type = CL_MARK;
        rec->cr.cr_namelen = len;
        memcpy(rec->cr.cr_name, obd->obd_name, rec->cr.cr_namelen);
        /* Status and action flags */
        rec->cr.cr_markerflags = mdd->mdd_cl.mc_flags | markerflags;

        /* XXX: transaction is started by llog itself */
        rc = (mdd->mdd_cl.mc_mask & (1 << CL_MARK)) ?
                mdd_changelog_llog_write(mdd, rec, NULL) : 0;

        /* assume on or off event; reset repeat-access time */
        mdd->mdd_cl.mc_starttime = cfs_time_current_64();

        OBD_FREE(rec, reclen);
        RETURN(rc);
}
#endif
/**
 * Create ".lustre" directory.
 */
static int create_dot_lustre_dir(const struct lu_env *env, struct mdd_device *m)
{
        struct lu_fid *fid = &mdd_env_info(env)->mti_fid;
        struct mdd_object *mdo;
        int rc;

        memcpy(fid, &LU_DOT_LUSTRE_FID, sizeof(struct lu_fid));
        mdo = mdd_open_index_internal(env, m, &m->mdd_root_fid,
                                      dot_lustre_name, fid);
        /* .lustre dir may be already present */
        if (IS_ERR(mdo)) {
                rc = PTR_ERR(mdo);
                CERROR("creating obj [%s] fid = "DFID" rc = %d\n",
                        dot_lustre_name, PFID(fid), rc);
                RETURN(rc);
        }

        mdd_object_put(env, mdo);

        return 0;
}


static int dot_lustre_mdd_permission(const struct lu_env *env,
                                     struct md_object *pobj,
                                     struct md_object *cobj,
                                     struct md_attr *attr, int mask)
{
        if (mask & ~(MAY_READ | MAY_EXEC))
                return -EPERM;
        else
                return 0;
}

static int dot_lustre_mdd_attr_get(const struct lu_env *env,
                                   struct md_object *obj, struct md_attr *ma)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);

        LASSERT((ma->ma_need & MA_LOV) == 0);
        LASSERT((ma->ma_need & MA_LMV) == 0);
        LASSERT((ma->ma_need & MA_ACL_DEF) == 0);
        LASSERT((ma->ma_need & MA_HSM) == 0);
        LASSERT((ma->ma_need & MA_SOM) == 0);
        LASSERT((ma->ma_need & MA_PFID) == 0);

        return mdd_la_get(env, mdd_obj, &ma->ma_attr,
                          mdd_object_capa(env, mdd_obj));
}

static int dot_lustre_mdd_attr_set(const struct lu_env *env,
                                   struct md_object *obj,
                                   const struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_xattr_get(const struct lu_env *env,
                                    struct md_object *obj, struct lu_buf *buf,
                                    const char *name)
{
        return 0;
}

static int dot_lustre_mdd_xattr_list(const struct lu_env *env,
                                     struct md_object *obj, struct lu_buf *buf)
{
        return 0;
}

static int dot_lustre_mdd_xattr_set(const struct lu_env *env,
                                    struct md_object *obj,
                                    const struct lu_buf *buf, const char *name,
                                    int fl)
{
        return -EPERM;
}

static int dot_lustre_mdd_xattr_del(const struct lu_env *env,
                                    struct md_object *obj,
                                    const char *name)
{
        return -EPERM;
}

static int dot_lustre_mdd_readlink(const struct lu_env *env,
                                   struct md_object *obj, struct lu_buf *buf)
{
        return 0;
}

static int dot_lustre_mdd_object_create(const struct lu_env *env,
                                        struct md_object *obj,
                                        const struct md_op_spec *spec,
                                        struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_ref_add(const struct lu_env *env,
                                  struct md_object *obj,
                                  const struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_ref_del(const struct lu_env *env,
                                  struct md_object *obj,
                                  struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_open(const struct lu_env *env, struct md_object *obj,
                               int flags)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);

        mdd_write_lock(env, mdd_obj, MOR_TGT_CHILD);
        mdd_obj->mod_count++;
        mdd_write_unlock(env, mdd_obj);

        return 0;
}

static int dot_lustre_mdd_close(const struct lu_env *env, struct md_object *obj,
                                struct md_attr *ma, int mode)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);

        mdd_write_lock(env, mdd_obj, MOR_TGT_CHILD);
        mdd_obj->mod_count--;
        mdd_write_unlock(env, mdd_obj);

        return 0;
}

static int dot_lustre_mdd_object_sync(const struct lu_env *env,
                                      struct md_object *obj)
{
        return -ENOSYS;
}

static int dot_lustre_mdd_path(const struct lu_env *env, struct md_object *obj,
                           char *path, int pathlen, __u64 *recno, int *linkno)
{
        return -ENOSYS;
}

static int dot_file_lock(const struct lu_env *env, struct md_object *obj,
                         struct lov_mds_md *lmm, struct ldlm_extent *extent,
                         struct lustre_handle *lockh)
{
        return -ENOSYS;
}

static int dot_file_unlock(const struct lu_env *env, struct md_object *obj,
                           struct lov_mds_md *lmm, struct lustre_handle *lockh)
{
        return -ENOSYS;
}

static struct md_object_operations mdd_dot_lustre_obj_ops = {
        .moo_permission    = dot_lustre_mdd_permission,
        .moo_attr_get      = dot_lustre_mdd_attr_get,
        .moo_attr_set      = dot_lustre_mdd_attr_set,
        .moo_xattr_get     = dot_lustre_mdd_xattr_get,
        .moo_xattr_list    = dot_lustre_mdd_xattr_list,
        .moo_xattr_set     = dot_lustre_mdd_xattr_set,
        .moo_xattr_del     = dot_lustre_mdd_xattr_del,
        .moo_readpage      = mdd_readpage,
        .moo_readlink      = dot_lustre_mdd_readlink,
        .moo_object_create = dot_lustre_mdd_object_create,
        .moo_ref_add       = dot_lustre_mdd_ref_add,
        .moo_ref_del       = dot_lustre_mdd_ref_del,
        .moo_open          = dot_lustre_mdd_open,
        .moo_close         = dot_lustre_mdd_close,
        .moo_capa_get      = mdd_capa_get,
        .moo_object_sync   = dot_lustre_mdd_object_sync,
        .moo_path          = dot_lustre_mdd_path,
        .moo_file_lock     = dot_file_lock,
        .moo_file_unlock   = dot_file_unlock,
};


static int dot_lustre_mdd_lookup(const struct lu_env *env, struct md_object *p,
                                 const struct lu_name *lname, struct lu_fid *f,
                                 struct md_op_spec *spec)
{
        if (strcmp(lname->ln_name, mdd_obf_dir_name) == 0)
                *f = LU_OBF_FID;
        else
                return -ENOENT;

        return 0;
}

static mdl_mode_t dot_lustre_mdd_lock_mode(const struct lu_env *env,
                                           struct md_object *obj,
                                           mdl_mode_t mode)
{
        return MDL_MINMODE;
}

static int dot_lustre_mdd_create(const struct lu_env *env,
                                 struct md_object *pobj,
                                 const struct lu_name *lname,
                                 struct md_object *child,
                                 struct md_op_spec *spec,
                                 struct md_attr* ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_create_data(const struct lu_env *env,
                                      struct md_object *p,
                                      struct md_object *o,
                                      const struct md_op_spec *spec,
                                      struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_rename(const struct lu_env *env,
                                 struct md_object *src_pobj,
                                 struct md_object *tgt_pobj,
                                 const struct lu_fid *lf,
                                 const struct lu_name *lsname,
                                 struct md_object *tobj,
                                 const struct lu_name *ltname,
                                 struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_link(const struct lu_env *env,
                               struct md_object *tgt_obj,
                               struct md_object *src_obj,
                               const struct lu_name *lname,
                               struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_unlink(const struct lu_env *env,
                                 struct md_object *pobj,
                                 struct md_object *cobj,
                                 const struct lu_name *lname,
                                 struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_name_insert(const struct lu_env *env,
                                      struct md_object *obj,
                                      const struct lu_name *lname,
                                      const struct lu_fid *fid,
                                      const struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_name_remove(const struct lu_env *env,
                                      struct md_object *obj,
                                      const struct lu_name *lname,
                                      const struct md_attr *ma)
{
        return -EPERM;
}

static int dot_lustre_mdd_rename_tgt(const struct lu_env *env,
                                     struct md_object *pobj,
                                     struct md_object *tobj,
                                     const struct lu_fid *fid,
                                     const struct lu_name *lname,
                                     struct md_attr *ma)
{
        return -EPERM;
}


static struct md_dir_operations mdd_dot_lustre_dir_ops = {
        .mdo_is_subdir   = mdd_is_subdir,
        .mdo_lookup      = dot_lustre_mdd_lookup,
        .mdo_lock_mode   = dot_lustre_mdd_lock_mode,
        .mdo_create      = dot_lustre_mdd_create,
        .mdo_create_data = dot_lustre_mdd_create_data,
        .mdo_rename      = dot_lustre_mdd_rename,
        .mdo_link        = dot_lustre_mdd_link,
        .mdo_unlink      = dot_lustre_mdd_unlink,
        .mdo_name_insert = dot_lustre_mdd_name_insert,
        .mdo_name_remove = dot_lustre_mdd_name_remove,
        .mdo_rename_tgt  = dot_lustre_mdd_rename_tgt,
};

static int obf_attr_get(const struct lu_env *env, struct md_object *obj,
                        struct md_attr *ma)
{
        struct mdd_device *mdd = mdo2mdd(obj);

        /* "fid" is a virtual object and hence does not have any "real"
         * attributes. So we reuse attributes of .lustre for "fid" dir */
        return mdd_la_get(env, mdd->mdd_dot_lustre, &ma->ma_attr,
                          mdd_object_capa(env, mdd->mdd_dot_lustre));
}

static int obf_attr_set(const struct lu_env *env, struct md_object *obj,
                        const struct md_attr *ma)
{
        return -EPERM;
}

static int obf_xattr_get(const struct lu_env *env,
                         struct md_object *obj, struct lu_buf *buf,
                         const char *name)
{
        return 0;
}

static int obf_mdd_open(const struct lu_env *env, struct md_object *obj,
                        int flags)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);

        mdd_write_lock(env, mdd_obj, MOR_TGT_CHILD);
        mdd_obj->mod_count++;
        mdd_write_unlock(env, mdd_obj);

        return 0;
}

static int obf_mdd_close(const struct lu_env *env, struct md_object *obj,
                         struct md_attr *ma, int mode)
{
        struct mdd_object *mdd_obj = md2mdd_obj(obj);

        mdd_write_lock(env, mdd_obj, MOR_TGT_CHILD);
        mdd_obj->mod_count--;
        mdd_write_unlock(env, mdd_obj);

        return 0;
}

/** Nothing to list in "fid" directory */
static int obf_mdd_readpage(const struct lu_env *env, struct md_object *obj,
                            const struct lu_rdpg *rdpg)
{
        return -EPERM;
}

static int obf_path(const struct lu_env *env, struct md_object *obj,
                    char *path, int pathlen, __u64 *recno, int *linkno)
{
        return -ENOSYS;
}

static struct md_object_operations mdd_obf_obj_ops = {
        .moo_attr_get   = obf_attr_get,
        .moo_attr_set   = obf_attr_set,
        .moo_xattr_get  = obf_xattr_get,
        .moo_open       = obf_mdd_open,
        .moo_close      = obf_mdd_close,
        .moo_readpage   = obf_mdd_readpage,
        .moo_path       = obf_path
};

/**
 * Lookup method for "fid" object. Only filenames with correct SEQ:OID format
 * are valid. We also check if object with passed fid exists or not.
 */
static int obf_lookup(const struct lu_env *env, struct md_object *p,
                      const struct lu_name *lname, struct lu_fid *f,
                      struct md_op_spec *spec)
{
        char *name = (char *)lname->ln_name;
        struct mdd_device *mdd = mdo2mdd(p);
        struct mdd_object *child;
        int rc = 0;

        while (*name == '[')
                name++;

        sscanf(name, SFID, RFID(f));
        if (!fid_is_sane(f)) {
                CWARN("bad FID format [%s], should be "DFID"\n", lname->ln_name,
                      (__u64)1, 2, 0);
                GOTO(out, rc = -EINVAL);
        }

        /* Check if object with this fid exists */
        child = mdd_object_find(env, mdd, f);
        if (child == NULL)
                GOTO(out, rc = 0);
        if (IS_ERR(child))
                GOTO(out, rc = PTR_ERR(child));

        if (mdd_object_exists(child) == 0)
                rc = -ENOENT;

        mdd_object_put(env, child);

out:
        return rc;
}

static int obf_create(const struct lu_env *env, struct md_object *pobj,
                      const struct lu_name *lname, struct md_object *child,
                      struct md_op_spec *spec, struct md_attr* ma)
{
        return -EPERM;
}

static int obf_rename(const struct lu_env *env,
                      struct md_object *src_pobj, struct md_object *tgt_pobj,
                      const struct lu_fid *lf, const struct lu_name *lsname,
                      struct md_object *tobj, const struct lu_name *ltname,
                      struct md_attr *ma)
{
        return -EPERM;
}

static int obf_link(const struct lu_env *env, struct md_object *tgt_obj,
                    struct md_object *src_obj, const struct lu_name *lname,
                    struct md_attr *ma)
{
        return -EPERM;
}

static int obf_unlink(const struct lu_env *env, struct md_object *pobj,
                      struct md_object *cobj, const struct lu_name *lname,
                      struct md_attr *ma)
{
        return -EPERM;
}

static struct md_dir_operations mdd_obf_dir_ops = {
        .mdo_lookup = obf_lookup,
        .mdo_create = obf_create,
        .mdo_rename = obf_rename,
        .mdo_link   = obf_link,
        .mdo_unlink = obf_unlink
};

/**
 * Create special in-memory "fid" object for open-by-fid.
 */
static int mdd_obf_setup(const struct lu_env *env, struct mdd_device *m)
{
        struct mdd_object *mdd_obf;
        struct lu_object *obf_lu_obj;
        int rc = 0;

        m->mdd_dot_lustre_objs.mdd_obf = mdd_object_find(env, m,
                                                         &LU_OBF_FID);
        if (m->mdd_dot_lustre_objs.mdd_obf == NULL ||
            IS_ERR(m->mdd_dot_lustre_objs.mdd_obf))
                GOTO(out, rc = -ENOENT);

        mdd_obf = m->mdd_dot_lustre_objs.mdd_obf;
        mdd_obf->mod_obj.mo_dir_ops = &mdd_obf_dir_ops;
        mdd_obf->mod_obj.mo_ops = &mdd_obf_obj_ops;
        /* Don't allow objects to be created in "fid" dir */
        mdd_obf->mod_flags |= IMMUTE_OBJ;

        obf_lu_obj = mdd2lu_obj(mdd_obf);
        obf_lu_obj->lo_header->loh_attr |= (LOHA_EXISTS | S_IFDIR);

out:
        return rc;
}

/** Setup ".lustre" directory object */
static int mdd_dot_lustre_setup(const struct lu_env *env, struct mdd_device *m)
{
        struct dt_object *dt_dot_lustre;
        struct lu_fid *fid = &mdd_env_info(env)->mti_fid;
        int rc;

        rc = create_dot_lustre_dir(env, m);
        if (rc)
                return rc;

        dt_dot_lustre = dt_store_open(env, m->mdd_child, mdd_root_dir_name,
                                      dot_lustre_name, fid);
        if (IS_ERR(dt_dot_lustre)) {
                rc = PTR_ERR(dt_dot_lustre);
                GOTO(out, rc);
        }

        /* references are released in mdd_device_shutdown() */
        m->mdd_dot_lustre = lu2mdd_obj(lu_object_locate(dt_dot_lustre->do_lu.lo_header,
                                                        &mdd_device_type));

        m->mdd_dot_lustre->mod_obj.mo_dir_ops = &mdd_dot_lustre_dir_ops;
        m->mdd_dot_lustre->mod_obj.mo_ops = &mdd_dot_lustre_obj_ops;

        rc = mdd_obf_setup(env, m);
        if (rc)
                CERROR("Error initializing \"fid\" object - %d.\n", rc);

out:
        RETURN(rc);
}

static int mdd_process_config(const struct lu_env *env,
                              struct lu_device *d, struct lustre_cfg *cfg)
{
        struct mdd_device *m    = lu2mdd_dev(d);
        struct dt_device  *dt   = m->mdd_child;
        struct lu_device  *next = &dt->dd_lu_dev;
        int rc;
        ENTRY;

        switch (cfg->lcfg_command) {
        case LCFG_PARAM: {
                struct lprocfs_static_vars lvars;

                lprocfs_mdd_init_vars(&lvars);
                rc = class_process_proc_param(PARAM_MDD, lvars.obd_vars, cfg,m);
                if (rc > 0 || rc == -ENOSYS)
                        /* we don't understand; pass it on */
                        rc = next->ld_ops->ldo_process_config(env, next, cfg);
                break;
        }
        case LCFG_SETUP:
                rc = next->ld_ops->ldo_process_config(env, next, cfg);
                if (rc)
                        GOTO(out, rc);
                dt->dd_ops->dt_conf_get(env, dt, &m->mdd_dt_conf);

#ifdef XXX_MDD_CHANGELOG
                mdd_changelog_init(env, m);
#endif
                break;
        case LCFG_CLEANUP:
                mdd_device_shutdown(env, m, cfg);
        default:
                rc = next->ld_ops->ldo_process_config(env, next, cfg);
                break;
        }
out:
        RETURN(rc);
}

static int mdd_recovery_complete(const struct lu_env *env,
                                 struct lu_device *d)
{
        struct mdd_device *mdd = lu2mdd_dev(d);
        struct lu_device *next = &mdd->mdd_child->dd_lu_dev;
        int rc;
        ENTRY;

        /* XXX: orphans handling. */
        __mdd_orphan_cleanup(env, mdd);
        rc = next->ld_ops->ldo_recovery_complete(env, next);

        RETURN(rc);
}

static struct md_object *mdo_locate(const struct lu_env *env,
                                    struct md_device *md,
                                    const struct lu_fid *fid)
{
        struct lu_object *obj;
        struct md_object *mdo;

        obj = lu_object_find(env, &md->md_lu_dev, fid, NULL);
        if (!IS_ERR(obj)) {
                obj = lu_object_locate(obj->lo_header, md->md_lu_dev.ld_type);
                LASSERT(obj != NULL);
                mdo = (struct md_object *) obj;
        } else
                mdo = (struct md_object *)obj;
        return mdo;
}

struct mdd_object *mdd_open_index_internal(const struct lu_env *env,
                                           struct mdd_device *mdd,
                                           const struct lu_fid *pfid,
                                           const char *name,
                                           const struct lu_fid *fid)
{
        struct md_device    *md = &mdd->mdd_md_dev;
        struct md_attr       ma;
        struct lu_attr      *la = &ma.ma_attr;
        struct md_object    *mdo, *dir;
        struct md_op_spec    spec;
        struct mdd_object   *mddo;
        struct lu_name       lname;
        int rc;

        dir = mdo_locate(env, md, pfid);
        if (IS_ERR(dir))
                return ((struct mdd_object *) dir);
        if (!lu_object_exists(&dir->mo_lu)) {
                lu_object_put(env, &dir->mo_lu);
                return ((struct mdd_object *) ERR_PTR(-ENOTDIR));
        }

        mdo = mdo_locate(env, md, fid);
        if (IS_ERR(mdo)) {
                mddo = (struct mdd_object *) mdo;
                goto out;

        }
        if (lu_object_exists(&mdo->mo_lu)) {
                mddo = md2mdd_obj(mdo);
                goto out;
        }

        lname.ln_name = name;
        lname.ln_namelen = strlen(name);

        memset(&spec, 0, sizeof(spec));
        spec.sp_feat = &dt_directory_features;
        spec.sp_cr_flags = 0;
        spec.sp_cr_lookup = 1;
        spec.sp_cr_mode = 0;
        spec.sp_ck_split = 0;

        memset(&ma, 0, sizeof(ma));
        ma.ma_valid = 0;
        ma.ma_need = 0;

        la->la_mode = S_IFDIR | S_IXUGO;
        la->la_mode |= S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
        la->la_uid = la->la_gid = 0;
        la->la_valid = LA_MODE | LA_UID | LA_GID;

        rc = mdo_create(env, dir, &lname, mdo, &spec, &ma);

        if (rc) {
                lu_object_put(env, &mdo->mo_lu);
                mddo = ERR_PTR(rc);
        } else
                mddo = md2mdd_obj(mdo);

out:
        lu_object_put(env, &dir->mo_lu);

        return mddo;
}

static int mdd_start(const struct lu_env *env,
                       struct lu_device *cdev)
{
        struct mdd_device *mdd = lu2mdd_dev(cdev);
        struct lu_device *next = &mdd->mdd_child->dd_lu_dev;
        struct mdd_object *mdo;
        struct lu_fid     fid, pfid;
        int rc;

        ENTRY;
        rc = next->ld_ops->ldo_start(env, next);
        if (rc)
                GOTO(out, rc);

        rc = mdd->mdd_child->dd_ops->dt_root_get(env, mdd->mdd_child, &pfid);
        LASSERT(rc == 0);
        lu_local_obj_fid(&fid, MDD_ROOT_INDEX_OID);

        mdo = mdd_open_index_internal(env, mdd, &pfid, mdd_root_dir_name, &fid);
        if (IS_ERR(mdo))
                RETURN(PTR_ERR(mdo));
        mdd_object_put(env, mdo);

        mdd->mdd_root_fid = fid;

        rc = orph_index_init(env, mdd);
        if (rc)
                GOTO(out, rc);

        rc = mdd_dot_lustre_setup(env, mdd);
        if (rc) {
                CERROR("Error(%d) initializing .lustre objects\n", rc);
                GOTO(out, rc);
        }

        LASSERT(cdev->ld_obd);
        rc = mdd_procfs_init(mdd, cdev->ld_obd->obd_name);
        LASSERT(rc == 0);

out:
        RETURN(rc);
}

const struct lu_device_operations mdd_lu_ops = {
        .ldo_object_alloc      = mdd_object_alloc,
        .ldo_process_config    = mdd_process_config,
        .ldo_recovery_complete = mdd_recovery_complete,
        .ldo_start             = mdd_start,
};

/*
 * No permission check is needed.
 */
static int mdd_root_get(const struct lu_env *env,
                        struct md_device *m, struct lu_fid *f)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);

        ENTRY;
        *f = mdd->mdd_root_fid;
        RETURN(0);
}

/*
 * No permission check is needed.
 */
static int mdd_statfs(const struct lu_env *env, struct md_device *m,
                      struct obd_statfs *osfs)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        int rc;

        ENTRY;

        rc = mdd_child_ops(mdd)->dt_statfs(env, mdd->mdd_child, osfs);

        RETURN(rc);
}

/*
 * No permission check is needed.
 */
static int mdd_init_capa_ctxt(const struct lu_env *env, struct md_device *m,
                              int mode, unsigned long timeout, __u32 alg,
                              struct lustre_capa_key *keys)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        int rc;
        ENTRY;

        rc = mdd_child_ops(mdd)->dt_init_capa_ctxt(env, mdd->mdd_child, mode,
                                                   timeout, alg, keys);
        RETURN(rc);
}

static int mdd_update_capa_key(const struct lu_env *env,
                               struct md_device *m,
                               struct lustre_capa_key *key)
{
        /* XXX: update capa keys in lod/osp? */
        return 0;
}

static struct lu_device *mdd_device_alloc(const struct lu_env *env,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *lcfg)
{
        struct lu_device  *l;
        struct mdd_device *m;

        OBD_ALLOC_PTR(m);
        if (m == NULL) {
                l = ERR_PTR(-ENOMEM);
        } else {
                mdd_init0(env, m, t, lcfg);
                l = mdd2lu_dev(m);
                l->ld_ops = &mdd_lu_ops;
                m->mdd_md_dev.md_ops = &mdd_ops;
        }

        return l;
}

static struct lu_device *mdd_device_free(const struct lu_env *env,
                                         struct lu_device *lu)
{
        struct mdd_device *m = lu2mdd_dev(lu);
        struct lu_device  *next = &m->mdd_child->dd_lu_dev;
        ENTRY;

        LASSERT(cfs_atomic_read(&lu->ld_ref) == 0);
        md_device_fini(&m->mdd_md_dev);
        OBD_FREE_PTR(m);
        RETURN(next);
}

/*
 * we use exports to track all mdd users
 */
static int mdd_obd_connect(const struct lu_env *env, struct obd_export **exp,
                           struct obd_device *obd, struct obd_uuid *cluuid,
                           struct obd_connect_data *data, void *localdata)
{
        struct mdd_device    *mdd = lu2mdd_dev(obd->obd_lu_dev);
        struct lustre_handle  conn;
        int                   rc;
        ENTRY;

        CDEBUG(D_CONFIG, "connect #%d\n", mdd->mdd_connects);

        rc = class_connect(&conn, obd, cluuid);
        if (rc)
                RETURN(rc);

        *exp = class_conn2export(&conn);

        /* Why should there ever be more than 1 connect? */
        /* XXX: locking ? */
        mdd->mdd_connects++;
        LASSERT(mdd->mdd_connects == 1);

        RETURN(0);
}

/*
 * once last export (we don't count self-export) disappeared
 * mdd can be released
 */
static int mdd_obd_disconnect(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        struct mdd_device *mdd = lu2mdd_dev(obd->obd_lu_dev);
        int                rc, release = 0;
        ENTRY;

        /* Only disconnect the underlying layers on the final disconnect. */
        /* XXX: locking ? */
        mdd->mdd_connects--;
        if (mdd->mdd_connects != 0) {
                /* why should there be more than 1 connect? */
                CERROR("disconnect #%d\n", mdd->mdd_connects);
                goto out;
        }

        /* XXX: the last user of lod has gone, let's release the device */
        release = 1;

out:
        rc = class_disconnect(exp); /* bz 9811 */

        if (rc == 0 && release)
                class_manual_cleanup(obd);
        RETURN(rc);
}

static int mdd_obd_health_check(struct obd_device *obd)
{
        struct mdd_device *mdd = lu2mdd_dev(obd->obd_lu_dev);
        int                rc;
        ENTRY;

        LASSERT(mdd);
        rc = obd_health_check(mdd->mdd_child_exp->exp_obd);
        RETURN(rc);
}

static struct obd_ops mdd_obd_device_ops = {
        .o_owner        = THIS_MODULE,
        .o_connect      = mdd_obd_connect,
        .o_disconnect   = mdd_obd_disconnect,
        .o_health_check = mdd_obd_health_check
};

/* context key constructor/destructor: mdd_ucred_key_init, mdd_ucred_key_fini */
LU_KEY_INIT_FINI(mdd_ucred, struct md_ucred);

static struct lu_context_key mdd_ucred_key = {
        .lct_tags = LCT_SESSION,
        .lct_init = mdd_ucred_key_init,
        .lct_fini = mdd_ucred_key_fini
};

struct md_ucred *md_ucred(const struct lu_env *env)
{
        LASSERT(env->le_ses != NULL);
        return lu_context_key_get(env->le_ses, &mdd_ucred_key);
}
EXPORT_SYMBOL(md_ucred);

/*
 * context key constructor/destructor:
 * mdd_capainfo_key_init, mdd_capainfo_key_fini
 */
LU_KEY_INIT_FINI(mdd_capainfo, struct md_capainfo);

struct lu_context_key mdd_capainfo_key = {
        .lct_tags = LCT_SESSION,
        .lct_init = mdd_capainfo_key_init,
        .lct_fini = mdd_capainfo_key_fini
};

struct md_capainfo *md_capainfo(const struct lu_env *env)
{
        /* NB, in mdt_init0 */
        if (env->le_ses == NULL)
                return NULL;
        return lu_context_key_get(env->le_ses, &mdd_capainfo_key);
}
EXPORT_SYMBOL(md_capainfo);

/*
 * context key constructor/destructor:
 * mdd_quota_key_init, mdd_quota_key_fini
 */
LU_KEY_INIT_FINI(mdd_quota, struct md_quota);

struct lu_context_key mdd_quota_key = {
        .lct_tags = LCT_SESSION,
        .lct_init = mdd_quota_key_init,
        .lct_fini = mdd_quota_key_fini
};

struct md_quota *md_quota(const struct lu_env *env)
{
        LASSERT(env->le_ses != NULL);
        return lu_context_key_get(env->le_ses, &mdd_quota_key);
}
EXPORT_SYMBOL(md_quota);

#ifdef XXX_MDD_CHANGELOG
static int mdd_changelog_user_register(struct mdd_device *mdd, int *id)
{
        struct llog_ctxt *ctxt;
        struct llog_changelog_user_rec *rec;
        int rc;
        ENTRY;

        ctxt = llog_get_context(mdd2obd_dev(mdd),LLOG_CHANGELOG_USER_ORIG_CTXT);
        if (ctxt == NULL)
                RETURN(-ENXIO);

        OBD_ALLOC_PTR(rec);
        if (rec == NULL) {
                llog_ctxt_put(ctxt);
                RETURN(-ENOMEM);
        }

        /* Assume we want it on since somebody registered */
        rc = mdd_changelog_on(mdd, 1);
        if (rc)
                GOTO(out, rc);

        rec->cur_hdr.lrh_len = sizeof(*rec);
        rec->cur_hdr.lrh_type = CHANGELOG_USER_REC;
        cfs_spin_lock(&mdd->mdd_cl.mc_user_lock);
        if (mdd->mdd_cl.mc_lastuser == (unsigned int)(-1)) {
                cfs_spin_unlock(&mdd->mdd_cl.mc_user_lock);
                CERROR("Maximum number of changelog users exceeded!\n");
                GOTO(out, rc = -EOVERFLOW);
        }
        *id = rec->cur_id = ++mdd->mdd_cl.mc_lastuser;
        rec->cur_endrec = mdd->mdd_cl.mc_index;
        cfs_spin_unlock(&mdd->mdd_cl.mc_user_lock);

        rc = llog_add_2(ctxt, &rec->cur_hdr, NULL, NULL, 0, NULL);

        CDEBUG(D_IOCTL, "Registered changelog user %d\n", *id);
out:
        OBD_FREE_PTR(rec);
        llog_ctxt_put(ctxt);
        RETURN(rc);
}

struct mdd_changelog_user_data {
        __u64 mcud_endrec; /**< purge record for this user */
        __u64 mcud_minrec; /**< lowest changelog recno still referenced */
        __u32 mcud_id;
        __u32 mcud_minid;  /**< user id with lowest rec reference */
        __u32 mcud_usercount;
        int   mcud_found:1;
        struct mdd_device   *mcud_mdd;
        const struct lu_env *mcud_env;
};
#define MCUD_UNREGISTER -1LL

/** Two things:
 * 1. Find the smallest record everyone is willing to purge
 * 2. Update the last purgeable record for this user
 */
static int mdd_changelog_user_purge_cb(struct llog_handle *llh,
                                       struct llog_rec_hdr *hdr, void *data)
{
        struct llog_changelog_user_rec *rec;
        struct mdd_changelog_user_data *mcud =
                (struct mdd_changelog_user_data *)data;
        int rc;
        ENTRY;

        LASSERT(llh->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN);

        rec = (struct llog_changelog_user_rec *)hdr;

        mcud->mcud_usercount++;

        /* If we have a new endrec for this id, use it for the following
           min check instead of its old value */
        if (rec->cur_id == mcud->mcud_id)
                rec->cur_endrec = max(rec->cur_endrec, mcud->mcud_endrec);

        /* Track the minimum referenced record */
        if (mcud->mcud_minid == 0 || mcud->mcud_minrec > rec->cur_endrec) {
                mcud->mcud_minid = rec->cur_id;
                mcud->mcud_minrec = rec->cur_endrec;
        }

        if (rec->cur_id != mcud->mcud_id)
                RETURN(0);

        /* Update this user's record */
        mcud->mcud_found = 1;

        /* Special case: unregister this user */
        if (mcud->mcud_endrec == MCUD_UNREGISTER) {
                struct llog_cookie cookie;
                void *th;
                struct mdd_device *mdd = mcud->mcud_mdd;

                cookie.lgc_lgl = llh->lgh_id;
                cookie.lgc_index = hdr->lrh_index;

                /* XXX This is a workaround for the deadlock of changelog
                 * adding vs. changelog cancelling. LU-81. */
                th = mdd_trans_create(mcud->mcud_env, mdd);
                if (IS_ERR(th)) {
                        CERROR("Cannot get thandle\n");
                        RETURN(-ENOMEM);
                }

                rc = mdd_declare_llog_cancel(mcud->mcud_env, mdd, th); 
                if (rc)
                        GOTO(stop, rc);

                rc = mdd_trans_start(mcud->mcud_env, mdd, th);
                if (rc)
                        GOTO(stop, rc);

                rc = llog_cat_cancel_records(llh->u.phd.phd_cat_handle,
                                             1, &cookie);
                if (rc == 0)
                        mcud->mcud_usercount--;

stop:
                mdd_trans_stop(mcud->mcud_env, mdd, 0, th);
                RETURN(rc);
        }

        /* Update the endrec */
        CDEBUG(D_IOCTL, "Rewriting changelog user %d endrec to "LPU64"\n",
               mcud->mcud_id, rec->cur_endrec);

        /* hdr+1 is loc of data */
        hdr->lrh_len -= sizeof(*hdr) + sizeof(struct llog_rec_tail);
        rc = llog_write_rec(llh, hdr, NULL, 0, (void *)(hdr + 1),
                            hdr->lrh_index);

        RETURN(rc);
}

static int mdd_changelog_user_purge(const struct lu_env *env,
                                    struct mdd_device *mdd, int id,
                                    long long endrec)
{
        struct mdd_changelog_user_data data;
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;

        CDEBUG(D_IOCTL, "Purge request: id=%d, endrec=%lld\n", id, endrec);

        data.mcud_id = id;
        data.mcud_minid = 0;
        data.mcud_minrec = 0;
        data.mcud_usercount = 0;
        data.mcud_endrec = endrec;
        data.mcud_mdd = mdd;
        data.mcud_env = env;
        cfs_spin_lock(&mdd->mdd_cl.mc_lock);
        endrec = mdd->mdd_cl.mc_index;
        cfs_spin_unlock(&mdd->mdd_cl.mc_lock);
        if ((data.mcud_endrec == 0) ||
            ((data.mcud_endrec > endrec) &&
             (data.mcud_endrec != MCUD_UNREGISTER)))
                data.mcud_endrec = endrec;

        ctxt = llog_get_context(mdd2obd_dev(mdd),LLOG_CHANGELOG_USER_ORIG_CTXT);
        if (ctxt == NULL)
                return -ENXIO;
        LASSERT(ctxt->loc_handle->lgh_hdr->llh_flags & LLOG_F_IS_CAT);

        rc = llog_cat_process(ctxt->loc_handle, mdd_changelog_user_purge_cb,
                              (void *)&data, 0, 0);
        if ((rc >= 0) && (data.mcud_minrec > 0)) {
                CDEBUG(D_IOCTL, "Purging changelog entries up to "LPD64
                       ", referenced by "CHANGELOG_USER_PREFIX"%d\n",
                       data.mcud_minrec, data.mcud_minid);
                rc = mdd_changelog_llog_cancel(mdd, data.mcud_minrec);
        } else {
                CWARN("Could not determine changelog records to purge; rc=%d\n",
                      rc);
        }

        llog_ctxt_put(ctxt);

        if (!data.mcud_found) {
                CWARN("No entry for user %d.  Last changelog reference is "
                      LPD64" by changelog user %d\n", data.mcud_id,
                      data.mcud_minrec, data.mcud_minid);
               rc = -ENOENT;
        }

        if (!rc && data.mcud_usercount == 0)
                /* No more users; turn changelogs off */
                rc = mdd_changelog_on(mdd, 0);

        RETURN (rc);
}
#endif
/** mdd_iocontrol
 * May be called remotely from mdt_iocontrol_handle or locally from
 * mdt_iocontrol. Data may be freeform - remote handling doesn't enforce
 * an obd_ioctl_data format (but local ioctl handler does).
 * \param cmd - ioc
 * \param len - data len
 * \param karg - ioctl data, in kernel space
 */
static int mdd_iocontrol(const struct lu_env *env, struct md_device *m,
                         unsigned int cmd, int len, void *karg)
{
        struct mdd_device *mdd;
        /* struct obd_ioctl_data *data = karg; */
        int rc = 0;
        ENTRY;

        mdd = lu2mdd_dev(&m->md_lu_dev);

        /* Doesn't use obd_ioctl_data */
#ifdef XXX_MDD_CHANGELOG
        if (cmd == OBD_IOC_CHANGELOG_CLEAR) {
                struct changelog_setinfo *cs = karg;
                rc = mdd_changelog_user_purge(env, mdd, cs->cs_id,
                                              cs->cs_recno);
                RETURN(rc);
        }

        /* Below ioctls use obd_ioctl_data */
        if (len != sizeof(*data)) {
                CERROR("Bad ioctl size %d\n", len);
                RETURN(-EINVAL);
        }
        if (data->ioc_version != OBD_IOCTL_VERSION) {
                CERROR("Bad magic %x != %x\n", data->ioc_version,
                       OBD_IOCTL_VERSION);
                RETURN(-EINVAL);
        }

        switch (cmd) {
        case OBD_IOC_CHANGELOG_REG:
                rc = mdd_changelog_user_register(mdd, &data->ioc_u32_1);
                break;
        case OBD_IOC_CHANGELOG_DEREG:
                rc = mdd_changelog_user_purge(env, mdd, data->ioc_u32_1,
                                              MCUD_UNREGISTER);
                break;
        default:
                rc = -ENOTTY;
        }
#endif
        RETURN (rc);
}

/* type constructor/destructor: mdd_type_init, mdd_type_fini */
LU_TYPE_INIT_FINI(mdd, &mdd_thread_key, &mdd_ucred_key, &mdd_capainfo_key,
                  &mdd_quota_key);

const struct md_device_operations mdd_ops = {
        .mdo_statfs         = mdd_statfs,
        .mdo_root_get       = mdd_root_get,
        .mdo_init_capa_ctxt = mdd_init_capa_ctxt,
        .mdo_update_capa_key= mdd_update_capa_key,
#if 0
        .mdo_llog_ctxt_get  = mdd_llog_ctxt_get,
#endif
        .mdo_iocontrol      = mdd_iocontrol,
};

static struct lu_device_type_operations mdd_device_type_ops = {
        .ldto_init = mdd_type_init,
        .ldto_fini = mdd_type_fini,

        .ldto_start = mdd_type_start,
        .ldto_stop  = mdd_type_stop,

        .ldto_device_alloc = mdd_device_alloc,
        .ldto_device_free  = mdd_device_free,

        .ldto_device_fini    = mdd_device_fini
};

static struct lu_device_type mdd_device_type = {
        .ldt_tags     = LU_DEVICE_MD,
        .ldt_name     = LUSTRE_MDD_NAME,
        .ldt_ops      = &mdd_device_type_ops,
        .ldt_ctx_tags = LCT_MD_THREAD
};

/* context key constructor: mdd_key_init */
LU_KEY_INIT(mdd, struct mdd_thread_info);

static void mdd_key_fini(const struct lu_context *ctx,
                         struct lu_context_key *key, void *data)
{
        struct mdd_thread_info *info = data;
        mdd_buf_put(&info->mti_big_buf);

        OBD_FREE_PTR(info);
}

/* context key: mdd_thread_key */
LU_CONTEXT_KEY_DEFINE(mdd, LCT_MD_THREAD);

static int __init mdd_mod_init(void)
{
        struct lprocfs_static_vars lvars;
        int                        rc;

        rc = lu_kmem_init(mdd_caches);
        if (rc)
                return rc;

        lprocfs_mdd_init_vars(&lvars);

        rc = class_register_type(&mdd_obd_device_ops, NULL, lvars.module_vars,
                                 LUSTRE_MDD_NAME, &mdd_device_type);
        if (rc)
                lu_kmem_fini(mdd_caches);
        return rc;
}

static void __exit mdd_mod_exit(void)
{
        class_unregister_type(LUSTRE_MDD_NAME);
        lu_kmem_fini(mdd_caches);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Meta-data Device Prototype ("LUSTRE_MDD_NAME")");
MODULE_LICENSE("GPL");

cfs_module(mdd, "0.1.0", mdd_mod_init, mdd_mod_exit);
