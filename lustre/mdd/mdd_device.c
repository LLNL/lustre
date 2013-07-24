/*
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
 *
 * Copyright (c) 2011, 2013, Intel Corporation.
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

#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <obd_class.h>
#include <lustre_mds.h>
#include <obd_support.h>
#include <lu_object.h>
#include <lustre_param.h>
#include <lustre_fid.h>

#include "mdd_internal.h"

const struct md_device_operations mdd_ops;
static struct lu_device_type mdd_device_type;

static const char mdd_root_dir_name[] = "ROOT";
static const char mdd_obf_dir_name[] = "fid";

/* Slab for MDD object allocation */
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
	struct lu_device	*lud = mdd2lu_dev(m);
	struct obd_device       *obd;
	int			 rc;
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

	lud->ld_site = m->mdd_child_exp->exp_obd->obd_lu_dev->ld_site;
	LASSERT(lud->ld_site);
	m->mdd_child = lu2dt_dev(m->mdd_child_exp->exp_obd->obd_lu_dev);
	m->mdd_bottom = lu2dt_dev(lud->ld_site->ls_bottom_dev);
	lu_dev_add_linkage(lud->ld_site, lud);

out:
	if (data)
		OBD_FREE(data, sizeof(*data));
	RETURN(rc);
}

static int mdd_init0(const struct lu_env *env, struct mdd_device *mdd,
		struct lu_device_type *t, struct lustre_cfg *lcfg)
{
	int rc;
	ENTRY;

	mdd->mdd_md_dev.md_lu_dev.ld_ops = &mdd_lu_ops;
	mdd->mdd_md_dev.md_ops = &mdd_ops;

	rc = mdd_connect_to_next(env, mdd, lustre_cfg_string(lcfg, 3));
	if (rc)
		RETURN(rc);

	mdd->mdd_atime_diff = MAX_ATIME_DIFF;
        /* sync permission changes */
        mdd->mdd_sync_permission = 1;

	dt_conf_get(env, mdd->mdd_child, &mdd->mdd_dt_conf);

	/* we are using service name but not mdd obd name
	 * for compatibility reasons.
	 * It is passed from MDT in lustre_cfg[2] buffer */
	rc = mdd_procfs_init(mdd, lustre_cfg_string(lcfg, 2));
	if (rc < 0)
		obd_disconnect(mdd->mdd_child_exp);

        RETURN(rc);
}

static struct lu_device *mdd_device_fini(const struct lu_env *env,
                                         struct lu_device *d)
{
        struct mdd_device *mdd = lu2mdd_dev(d);
        int rc;

	if (d->ld_site)
		lu_dev_del_linkage(d->ld_site, d);

        rc = mdd_procfs_fini(mdd);
        if (rc) {
                CERROR("proc fini error %d \n", rc);
                return ERR_PTR(rc);
        }
	return NULL;
}

static int changelog_init_cb(const struct lu_env *env, struct llog_handle *llh,
			     struct llog_rec_hdr *hdr, void *data)
{
	struct mdd_device *mdd = (struct mdd_device *)data;
	struct llog_changelog_rec *rec = (struct llog_changelog_rec *)hdr;

	LASSERT(llh->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN);
	LASSERT(rec->cr_hdr.lrh_type == CHANGELOG_REC);

	CDEBUG(D_INFO,
	       "seeing record at index %d/%d/"LPU64" t=%x %.*s in log"
	       DOSTID"\n", hdr->lrh_index, rec->cr_hdr.lrh_index,
	       rec->cr.cr_index, rec->cr.cr_type, rec->cr.cr_namelen,
	       rec->cr.cr_name, POSTID(&llh->lgh_id.lgl_oi));

	mdd->mdd_cl.mc_index = rec->cr.cr_index;
	return LLOG_PROC_BREAK;
}

static int changelog_user_init_cb(const struct lu_env *env,
				  struct llog_handle *llh,
				  struct llog_rec_hdr *hdr, void *data)
{
        struct mdd_device *mdd = (struct mdd_device *)data;
        struct llog_changelog_user_rec *rec =
                (struct llog_changelog_user_rec *)hdr;

        LASSERT(llh->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN);
        LASSERT(rec->cur_hdr.lrh_type == CHANGELOG_USER_REC);

        CDEBUG(D_INFO, "seeing user at index %d/%d id=%d endrec="LPU64
               " in log "DOSTID"\n", hdr->lrh_index, rec->cur_hdr.lrh_index,
               rec->cur_id, rec->cur_endrec, POSTID(&llh->lgh_id.lgl_oi));

	spin_lock(&mdd->mdd_cl.mc_user_lock);
	mdd->mdd_cl.mc_lastuser = rec->cur_id;
	if (rec->cur_endrec > mdd->mdd_cl.mc_index)
		mdd->mdd_cl.mc_index = rec->cur_endrec;
	spin_unlock(&mdd->mdd_cl.mc_user_lock);

	return LLOG_PROC_BREAK;
}

static int llog_changelog_cancel_cb(const struct lu_env *env,
				    struct llog_handle *llh,
				    struct llog_rec_hdr *hdr, void *data)
{
	struct llog_changelog_rec *rec = (struct llog_changelog_rec *)hdr;
	struct llog_cookie	 cookie;
	long long		 endrec = *(long long *)data;
	int			 rc;

	ENTRY;

	/* This is always a (sub)log, not the catalog */
	LASSERT(llh->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN);

	if (rec->cr.cr_index > endrec)
		/* records are in order, so we're done */
		RETURN(LLOG_PROC_BREAK);

	cookie.lgc_lgl = llh->lgh_id;
	cookie.lgc_index = hdr->lrh_index;

	/* cancel them one at a time.  I suppose we could store up the cookies
	 * and cancel them all at once; probably more efficient, but this is
	 * done as a user call, so who cares... */
	rc = llog_cat_cancel_records(env, llh->u.phd.phd_cat_handle, 1,
				     &cookie);
	RETURN(rc < 0 ? rc : 0);
}

static int llog_changelog_cancel(const struct lu_env *env,
				 struct llog_ctxt *ctxt,
				 struct lov_stripe_md *lsm, int count,
				 struct llog_cookie *cookies, int flags)
{
	struct llog_handle	*cathandle = ctxt->loc_handle;
	int			 rc;

	ENTRY;

	/* This should only be called with the catalog handle */
	LASSERT(cathandle->lgh_hdr->llh_flags & LLOG_F_IS_CAT);

	rc = llog_cat_process(env, cathandle, llog_changelog_cancel_cb,
			      (void *)cookies, 0, 0);
	if (rc >= 0)
		/* 0 or 1 means we're done */
		rc = 0;
	else
		CERROR("%s: cancel idx %u of catalog "DOSTID" rc=%d\n",
		       ctxt->loc_obd->obd_name, cathandle->lgh_last_idx,
		       POSTID(&cathandle->lgh_id.lgl_oi), rc);

	RETURN(rc);
}

static struct llog_operations changelog_orig_logops;

int mdd_changelog_on(const struct lu_env *env, struct mdd_device *mdd, int on);

static int mdd_changelog_llog_init(const struct lu_env *env,
				   struct mdd_device *mdd)
{
	struct obd_device	*obd = mdd2obd_dev(mdd);
	struct llog_ctxt	*ctxt = NULL, *uctxt = NULL;
	int			 rc;

	ENTRY;

	/* LU-2844 mdd setup failure should not cause umount oops */
	if (OBD_FAIL_CHECK(OBD_FAIL_MDS_CHANGELOG_INIT))
		RETURN(-EIO);

	OBD_SET_CTXT_MAGIC(&obd->obd_lvfs_ctxt);
	obd->obd_lvfs_ctxt.dt = mdd->mdd_bottom;
	rc = llog_setup(env, obd, &obd->obd_olg, LLOG_CHANGELOG_ORIG_CTXT,
			obd, &changelog_orig_logops);
	if (rc) {
		CERROR("%s: changelog llog setup failed: rc = %d\n",
		       obd->obd_name, rc);
		RETURN(rc);
	}

	ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
	LASSERT(ctxt);

	rc = llog_open_create(env, ctxt, &ctxt->loc_handle, NULL,
			      CHANGELOG_CATALOG);
	if (rc)
		GOTO(out_cleanup, rc);

	rc = llog_cat_init_and_process(env, ctxt->loc_handle);
	if (rc)
		GOTO(out_close, rc);

	rc = llog_cat_reverse_process(env, ctxt->loc_handle,
				      changelog_init_cb, mdd);

	if (rc < 0) {
		CERROR("%s: changelog init failed: rc = %d\n", obd->obd_name,
		       rc);
		GOTO(out_close, rc);
	}

	CDEBUG(D_IOCTL, "changelog starting index="LPU64"\n",
	       mdd->mdd_cl.mc_index);

	/* setup user changelog */
	rc = llog_setup(env, obd, &obd->obd_olg, LLOG_CHANGELOG_USER_ORIG_CTXT,
			obd, &changelog_orig_logops);
	if (rc) {
		CERROR("%s: changelog users llog setup failed: rc = %d\n",
		       obd->obd_name, rc);
		GOTO(out_close, rc);
	}

	uctxt = llog_get_context(obd, LLOG_CHANGELOG_USER_ORIG_CTXT);
	LASSERT(ctxt);

	rc = llog_open_create(env, uctxt, &uctxt->loc_handle, NULL,
			      CHANGELOG_USERS);
	if (rc)
		GOTO(out_ucleanup, rc);

	uctxt->loc_handle->lgh_logops->lop_add = llog_cat_add_rec;
	uctxt->loc_handle->lgh_logops->lop_declare_add = llog_cat_declare_add_rec;

	rc = llog_cat_init_and_process(env, uctxt->loc_handle);
	if (rc)
		GOTO(out_uclose, rc);

	rc = llog_cat_reverse_process(env, uctxt->loc_handle,
				      changelog_user_init_cb, mdd);
	if (rc < 0) {
		CERROR("%s: changelog user init failed: rc = %d\n",
		       obd->obd_name, rc);
		GOTO(out_uclose, rc);
	}

	/* If we have registered users, assume we want changelogs on */
	if (mdd->mdd_cl.mc_lastuser > 0) {
		rc = mdd_changelog_on(env, mdd, 1);
		if (rc < 0)
			GOTO(out_uclose, rc);
	}
	llog_ctxt_put(ctxt);
	llog_ctxt_put(uctxt);
	RETURN(0);
out_uclose:
	llog_cat_close(env, uctxt->loc_handle);
out_ucleanup:
	llog_cleanup(env, uctxt);
out_close:
	llog_cat_close(env, ctxt->loc_handle);
out_cleanup:
	llog_cleanup(env, ctxt);
	return rc;
}

static int mdd_changelog_init(const struct lu_env *env, struct mdd_device *mdd)
{
	struct obd_device	*obd = mdd2obd_dev(mdd);
	int			 rc;

	mdd->mdd_cl.mc_index = 0;
	spin_lock_init(&mdd->mdd_cl.mc_lock);
	mdd->mdd_cl.mc_starttime = cfs_time_current_64();
	mdd->mdd_cl.mc_flags = 0; /* off by default */
	mdd->mdd_cl.mc_mask = CHANGELOG_DEFMASK;
	spin_lock_init(&mdd->mdd_cl.mc_user_lock);
	mdd->mdd_cl.mc_lastuser = 0;

	rc = mdd_changelog_llog_init(env, mdd);
	if (rc) {
		CERROR("%s: changelog setup during init failed: rc = %d\n",
		       obd->obd_name, rc);
		mdd->mdd_cl.mc_flags |= CLM_ERR;
	}

	return rc;
}

static void mdd_changelog_fini(const struct lu_env *env,
			       struct mdd_device *mdd)
{
	struct obd_device	*obd = mdd2obd_dev(mdd);
	struct llog_ctxt	*ctxt;

	mdd->mdd_cl.mc_flags = 0;

	ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
	if (ctxt) {
		llog_cat_close(env, ctxt->loc_handle);
		llog_cleanup(env, ctxt);
	}
	ctxt = llog_get_context(obd, LLOG_CHANGELOG_USER_ORIG_CTXT);
	if (ctxt) {
		llog_cat_close(env, ctxt->loc_handle);
		llog_cleanup(env, ctxt);
	}
}

int mdd_changelog_write_header(const struct lu_env *env,
			       struct mdd_device *mdd, int markerflags);

/* Start / stop recording */
int mdd_changelog_on(const struct lu_env *env, struct mdd_device *mdd, int on)
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
			spin_lock(&mdd->mdd_cl.mc_lock);
			mdd->mdd_cl.mc_flags |= CLM_ON;
			spin_unlock(&mdd->mdd_cl.mc_lock);
			rc = mdd_changelog_write_header(env, mdd, CLM_START);
		}
	} else if ((on == 0) && ((mdd->mdd_cl.mc_flags & CLM_ON) == CLM_ON)) {
		LCONSOLE_INFO("%s: changelog off\n",mdd2obd_dev(mdd)->obd_name);
		rc = mdd_changelog_write_header(env, mdd, CLM_FINI);
		spin_lock(&mdd->mdd_cl.mc_lock);
		mdd->mdd_cl.mc_flags &= ~CLM_ON;
		spin_unlock(&mdd->mdd_cl.mc_lock);
	}
	return rc;
}

/** Remove entries with indicies up to and including \a endrec from the
 *  changelog
 * \param mdd
 * \param endrec
 * \retval 0 ok
 */
int mdd_changelog_llog_cancel(const struct lu_env *env,
			      struct mdd_device *mdd, long long endrec)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_ctxt *ctxt;
        long long unsigned cur;
        int rc;

        ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
        if (ctxt == NULL)
                return -ENXIO;

	spin_lock(&mdd->mdd_cl.mc_lock);
	cur = (long long)mdd->mdd_cl.mc_index;
	spin_unlock(&mdd->mdd_cl.mc_lock);
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
                rc = mdd_changelog_write_header(env, mdd, CLM_PURGE);
                if (rc)
                      goto out;
        }

        /* Some records were purged, so reset repeat-access time (so we
           record new mtime update records, so users can see a file has been
           changed since the last purge) */
        mdd->mdd_cl.mc_starttime = cfs_time_current_64();

	rc = llog_cancel(env, ctxt, NULL, 1, (struct llog_cookie *)&endrec, 0);
out:
        llog_ctxt_put(ctxt);
        return rc;
}

/** Add a CL_MARK record to the changelog
 * \param mdd
 * \param markerflags - CLM_*
 * \retval 0 ok
 */
int mdd_changelog_write_header(const struct lu_env *env,
			       struct mdd_device *mdd, int markerflags)
{
	struct obd_device		*obd = mdd2obd_dev(mdd);
	struct llog_changelog_rec	*rec;
	struct lu_buf			*buf;
	struct llog_ctxt		*ctxt;
	int				 reclen;
	int				 len = strlen(obd->obd_name);
	int				 rc;

	ENTRY;

	if (mdd->mdd_cl.mc_mask & (1 << CL_MARK)) {
		mdd->mdd_cl.mc_starttime = cfs_time_current_64();
		RETURN(0);
	}

	reclen = llog_data_len(sizeof(*rec) + len);
	buf = lu_buf_check_and_alloc(&mdd_env_info(env)->mti_big_buf, reclen);
	if (buf->lb_buf == NULL)
		RETURN(-ENOMEM);
	rec = buf->lb_buf;

        rec->cr.cr_flags = CLF_VERSION;
        rec->cr.cr_type = CL_MARK;
        rec->cr.cr_namelen = len;
        memcpy(rec->cr.cr_name, obd->obd_name, rec->cr.cr_namelen);
        /* Status and action flags */
	rec->cr.cr_markerflags = mdd->mdd_cl.mc_flags | markerflags;
	rec->cr_hdr.lrh_len = llog_data_len(sizeof(*rec) + rec->cr.cr_namelen);
	rec->cr_hdr.lrh_type = CHANGELOG_REC;
	rec->cr.cr_time = cl_time();
	spin_lock(&mdd->mdd_cl.mc_lock);
	rec->cr.cr_index = ++mdd->mdd_cl.mc_index;
	spin_unlock(&mdd->mdd_cl.mc_lock);

	ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
	LASSERT(ctxt);

	rc = llog_cat_add(env, ctxt->loc_handle, &rec->cr_hdr, NULL, NULL);
	if (rc > 0)
		rc = 0;
	llog_ctxt_put(ctxt);

	/* assume on or off event; reset repeat-access time */
	mdd->mdd_cl.mc_starttime = cfs_time_current_64();
	RETURN(rc);
}

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
		CWARN("%s: bad FID format [%s], should be "DFID"\n",
		      mdd2obd_dev(mdd)->obd_name, lname->ln_name,
		      (__u64)FID_SEQ_NORMAL, 1, 0);
                GOTO(out, rc = -EINVAL);
        }

	if (!fid_is_norm(f) && !fid_is_igif(f) && !fid_is_root(f)) {
		CWARN("%s: "DFID" is invalid, sequence should be "
		      ">= "LPX64" or within ["LPX64","LPX64"].\n",
		      mdd2obd_dev(mdd)->obd_name, PFID(f),
		      (__u64)FID_SEQ_NORMAL, (__u64)FID_SEQ_IGIF,
		      (__u64)FID_SEQ_IGIF_MAX);
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
		      struct md_attr *ma, int no_name)
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
		mdo = lu2md(obj);
	} else {
		mdo = ERR_PTR(PTR_ERR(obj));
	}
	return mdo;
}

/**
 * Create special in-memory "fid" object for open-by-fid.
 */
static int mdd_obf_setup(const struct lu_env *env, struct mdd_device *m)
{
	struct md_object	*mdo;
	struct mdd_object	*mdd_obf;
	struct lu_fid		 fid = LU_OBF_FID;
	int			 rc;

	rc = mdd_local_file_create(env, m, mdd_object_fid(m->mdd_dot_lustre),
				   mdd_obf_dir_name, S_IFDIR | S_IXUSR, &fid);
	if (rc < 0)
		RETURN(rc);

	mdo = mdo_locate(env, &m->mdd_md_dev, &fid);
	if (IS_ERR(mdo))
		RETURN(PTR_ERR(mdo));

	LASSERT(lu_object_exists(&mdo->mo_lu));

	mdd_obf = md2mdd_obj(mdo);
	mdd_obf->mod_obj.mo_dir_ops = &mdd_obf_dir_ops;
	m->mdd_dot_lustre_objs.mdd_obf = mdd_obf;

	return 0;
}

/** Setup ".lustre" directory object */
static int mdd_dot_lustre_setup(const struct lu_env *env, struct mdd_device *m)
{
	struct md_object	*mdo;
	struct lu_fid		 fid;
	int			 rc;

	ENTRY;
	/* Create ".lustre" directory in ROOT. */
	fid = LU_DOT_LUSTRE_FID;
	rc = mdd_local_file_create(env, m, &m->mdd_root_fid,
				   dot_lustre_name,
				   S_IFDIR | S_IRUGO | S_IWUSR | S_IXUGO,
				   &fid);
	if (rc < 0)
		RETURN(rc);
	mdo = mdo_locate(env, &m->mdd_md_dev, &fid);
	if (IS_ERR(mdo))
		RETURN(PTR_ERR(mdo));
	LASSERT(lu_object_exists(&mdo->mo_lu));

	m->mdd_dot_lustre = md2mdd_obj(mdo);

	rc = mdd_obf_setup(env, m);
	if (rc) {
		CERROR("%s: error initializing \"fid\" object: rc = %d.\n",
		       mdd2obd_dev(m)->obd_name, rc);
		GOTO(out, rc);
	}
	RETURN(0);
out:
	mdd_object_put(env, m->mdd_dot_lustre);
	m->mdd_dot_lustre = NULL;
	return rc;
}

static void mdd_device_shutdown(const struct lu_env *env, struct mdd_device *m,
				struct lustre_cfg *cfg)
{
	mdd_lfsck_cleanup(env, m);
	mdd_changelog_fini(env, m);
	orph_index_fini(env, m);
	if (m->mdd_dot_lustre_objs.mdd_obf)
		mdd_object_put(env, m->mdd_dot_lustre_objs.mdd_obf);
	if (m->mdd_dot_lustre)
		mdd_object_put(env, m->mdd_dot_lustre);
	if (m->mdd_los != NULL)
		local_oid_storage_fini(env, m->mdd_los);
	lu_site_purge(env, mdd2lu_dev(m)->ld_site, ~0);

	if (m->mdd_child_exp)
		obd_disconnect(m->mdd_child_exp);
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
		dt_conf_get(env, dt, &m->mdd_dt_conf);
                break;
        case LCFG_CLEANUP:
		rc = next->ld_ops->ldo_process_config(env, next, cfg);
		lu_dev_del_linkage(d->ld_site, d);
		mdd_device_shutdown(env, m, cfg);
		break;
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
	struct lu_device *next;
        int rc;
        ENTRY;

        LASSERT(mdd != NULL);
	next = &mdd->mdd_child->dd_lu_dev;

        /* XXX: orphans handling. */
        __mdd_orphan_cleanup(env, mdd);
        rc = next->ld_ops->ldo_recovery_complete(env, next);

        RETURN(rc);
}

int mdd_local_file_create(const struct lu_env *env, struct mdd_device *mdd,
			  const struct lu_fid *pfid, const char *name, __u32 mode,
			  struct lu_fid *fid)
{
	struct dt_object	*parent, *dto;
	int			 rc;

	ENTRY;

	LASSERT(!fid_is_zero(pfid));
	parent = dt_locate(env, mdd->mdd_bottom, pfid);
	if (unlikely(IS_ERR(parent)))
		RETURN(PTR_ERR(parent));

	/* create local file/dir, if @fid is passed then try to use it */
	if (fid_is_zero(fid))
		dto = local_file_find_or_create(env, mdd->mdd_los, parent,
						name, mode);
	else
		dto = local_file_find_or_create_with_fid(env, mdd->mdd_bottom,
							 fid, parent, name,
							 mode);
	if (IS_ERR(dto))
		GOTO(out_put, rc = PTR_ERR(dto));
	*fid = *lu_object_fid(&dto->do_lu);
	/* since stack is not fully set up the local_storage uses own stack
	 * and we should drop its object from cache */
	lu_object_put_nocache(env, &dto->do_lu);
	EXIT;
out_put:
	lu_object_put(env, &parent->do_lu);
	return 0;
}

static int mdd_prepare(const struct lu_env *env,
                       struct lu_device *pdev,
                       struct lu_device *cdev)
{
	struct mdd_device	*mdd = lu2mdd_dev(cdev);
	struct lu_device	*next = &mdd->mdd_child->dd_lu_dev;
	struct lu_fid		 fid;
	int			 rc;

	ENTRY;

	rc = next->ld_ops->ldo_prepare(env, cdev, next);
	if (rc)
		RETURN(rc);

	/* Setup local dirs */
	fid.f_seq = FID_SEQ_LOCAL_NAME;
	fid.f_oid = 1;
	fid.f_ver = 0;
	rc = local_oid_storage_init(env, mdd->mdd_bottom, &fid,
				    &mdd->mdd_los);
	if (rc)
		RETURN(rc);

	rc = dt_root_get(env, mdd->mdd_child, &mdd->mdd_local_root_fid);
	if (rc < 0)
		GOTO(out_los, rc);

	if (mdd_seq_site(mdd)->ss_node_id == 0) {
		lu_root_fid(&fid);
		rc = mdd_local_file_create(env, mdd, &mdd->mdd_local_root_fid,
					   mdd_root_dir_name, S_IFDIR |
					   S_IRUGO | S_IWUSR | S_IXUGO, &fid);
		if (rc != 0) {
			CERROR("%s: create root fid failed: rc = %d\n",
			       mdd2obd_dev(mdd)->obd_name, rc);
			GOTO(out_los, rc);
		}
		mdd->mdd_root_fid = fid;

		rc = mdd_dot_lustre_setup(env, mdd);
		if (rc != 0) {
			CERROR("%s: initializing .lustre failed: rc = %d\n",
			       mdd2obd_dev(mdd)->obd_name, rc);
			GOTO(out_los, rc);
		}

		rc = mdd_compat_fixes(env, mdd);
		if (rc)
			GOTO(out_los, rc);

	}

	rc = orph_index_init(env, mdd);
	if (rc < 0)
		GOTO(out_dot, rc);

	rc = mdd_changelog_init(env, mdd);
	if (rc != 0) {
		CERROR("%s: failed to initialize changelog: rc = %d\n",
		       mdd2obd_dev(mdd)->obd_name, rc);
		GOTO(out_orph, rc);
	}

	rc = mdd_lfsck_setup(env, mdd);
	if (rc != 0) {
		CERROR("%s: failed to initialize lfsck: rc = %d\n",
		       mdd2obd_dev(mdd)->obd_name, rc);
		GOTO(out_changelog, rc);
	}
	RETURN(0);
out_changelog:
	mdd_changelog_fini(env, mdd);
out_orph:
	orph_index_fini(env, mdd);
out_dot:
	if (mdd_seq_site(mdd)->ss_node_id == 0) {
		mdd_object_put(env, mdd->mdd_dot_lustre);
		mdd->mdd_dot_lustre = NULL;
		mdd_object_put(env, mdd->mdd_dot_lustre_objs.mdd_obf);
		mdd->mdd_dot_lustre_objs.mdd_obf = NULL;
	}
out_los:
	local_oid_storage_fini(env, mdd->mdd_los);
	mdd->mdd_los = NULL;
	return rc;
}

const struct lu_device_operations mdd_lu_ops = {
        .ldo_object_alloc      = mdd_object_alloc,
        .ldo_process_config    = mdd_process_config,
        .ldo_recovery_complete = mdd_recovery_complete,
        .ldo_prepare           = mdd_prepare,
};

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
                      struct obd_statfs *sfs)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        int rc;

        ENTRY;

        rc = mdd_child_ops(mdd)->dt_statfs(env, mdd->mdd_child, sfs);

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

        /* need barrier for mds_capa_keys access. */

        rc = mdd_child_ops(mdd)->dt_init_capa_ctxt(env, mdd->mdd_child, mode,
                                                   timeout, alg, keys);
        RETURN(rc);
}

static int mdd_update_capa_key(const struct lu_env *env,
                               struct md_device *m,
                               struct lustre_capa_key *key)
{
	/* we do not support capabilities ... */
	return -EINVAL;
}

static int mdd_llog_ctxt_get(const struct lu_env *env, struct md_device *m,
                             int idx, void **h)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);

        *h = llog_group_get_ctxt(&mdd2obd_dev(mdd)->obd_olg, idx);
        return (*h == NULL ? -ENOENT : 0);
}

static struct lu_device *mdd_device_free(const struct lu_env *env,
					 struct lu_device *lu)
{
	struct mdd_device *m = lu2mdd_dev(lu);
	ENTRY;

	LASSERT(cfs_atomic_read(&lu->ld_ref) == 0);
	md_device_fini(&m->mdd_md_dev);
	OBD_FREE_PTR(m);
	RETURN(NULL);
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
		int rc;

                l = mdd2lu_dev(m);
		md_device_init(&m->mdd_md_dev, t);
		rc = mdd_init0(env, m, t, lcfg);
		if (rc != 0) {
			mdd_device_free(env, l);
			l = ERR_PTR(rc);
		}
        }

        return l;
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
	LASSERT(mdd->mdd_connects == 0);
	mdd->mdd_connects++;

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

	mdd->mdd_connects--;
	if (mdd->mdd_connects == 0)
		release = 1;

	rc = class_disconnect(exp);

	if (rc == 0 && release)
		class_manual_cleanup(obd);
	RETURN(rc);
}

static int mdd_obd_health_check(const struct lu_env *env,
				struct obd_device *obd)
{
	struct mdd_device *mdd = lu2mdd_dev(obd->obd_lu_dev);
	int		   rc;
	ENTRY;

	LASSERT(mdd);
	rc = obd_health_check(env, mdd->mdd_child_exp->exp_obd);
	RETURN(rc);
}

static struct obd_ops mdd_obd_device_ops = {
	.o_owner	= THIS_MODULE,
	.o_connect	= mdd_obd_connect,
	.o_disconnect	= mdd_obd_disconnect,
	.o_health_check	= mdd_obd_health_check
};

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

static int mdd_changelog_user_register(const struct lu_env *env,
				       struct mdd_device *mdd, int *id)
{
        struct llog_ctxt *ctxt;
        struct llog_changelog_user_rec *rec;
        int rc;
        ENTRY;

        ctxt = llog_get_context(mdd2obd_dev(mdd),
				LLOG_CHANGELOG_USER_ORIG_CTXT);
        if (ctxt == NULL)
                RETURN(-ENXIO);

        OBD_ALLOC_PTR(rec);
        if (rec == NULL) {
                llog_ctxt_put(ctxt);
                RETURN(-ENOMEM);
        }

        /* Assume we want it on since somebody registered */
        rc = mdd_changelog_on(env, mdd, 1);
        if (rc)
                GOTO(out, rc);

        rec->cur_hdr.lrh_len = sizeof(*rec);
        rec->cur_hdr.lrh_type = CHANGELOG_USER_REC;
	spin_lock(&mdd->mdd_cl.mc_user_lock);
	if (mdd->mdd_cl.mc_lastuser == (unsigned int)(-1)) {
		spin_unlock(&mdd->mdd_cl.mc_user_lock);
		CERROR("Maximum number of changelog users exceeded!\n");
		GOTO(out, rc = -EOVERFLOW);
	}
	*id = rec->cur_id = ++mdd->mdd_cl.mc_lastuser;
	rec->cur_endrec = mdd->mdd_cl.mc_index;
	spin_unlock(&mdd->mdd_cl.mc_user_lock);

	rc = llog_cat_add(env, ctxt->loc_handle, &rec->cur_hdr, NULL, NULL);

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
	unsigned int mcud_found:1;
};
#define MCUD_UNREGISTER -1LL

/** Two things:
 * 1. Find the smallest record everyone is willing to purge
 * 2. Update the last purgeable record for this user
 */
static int mdd_changelog_user_purge_cb(const struct lu_env *env,
				       struct llog_handle *llh,
				       struct llog_rec_hdr *hdr, void *data)
{
	struct llog_changelog_user_rec	*rec;
	struct mdd_changelog_user_data	*mcud = data;
	int				 rc;

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

                cookie.lgc_lgl = llh->lgh_id;
                cookie.lgc_index = hdr->lrh_index;

		rc = llog_cat_cancel_records(env, llh->u.phd.phd_cat_handle,
					     1, &cookie);
                if (rc == 0)
                        mcud->mcud_usercount--;

                RETURN(rc);
        }

        /* Update the endrec */
        CDEBUG(D_IOCTL, "Rewriting changelog user %d endrec to "LPU64"\n",
               mcud->mcud_id, rec->cur_endrec);

        /* hdr+1 is loc of data */
        hdr->lrh_len -= sizeof(*hdr) + sizeof(struct llog_rec_tail);
	rc = llog_write(env, llh, hdr, NULL, 0, (void *)(hdr + 1),
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
	spin_lock(&mdd->mdd_cl.mc_lock);
	endrec = mdd->mdd_cl.mc_index;
	spin_unlock(&mdd->mdd_cl.mc_lock);
        if ((data.mcud_endrec == 0) ||
            ((data.mcud_endrec > endrec) &&
             (data.mcud_endrec != MCUD_UNREGISTER)))
                data.mcud_endrec = endrec;

        ctxt = llog_get_context(mdd2obd_dev(mdd),
				LLOG_CHANGELOG_USER_ORIG_CTXT);
        if (ctxt == NULL)
                return -ENXIO;

        LASSERT(ctxt->loc_handle->lgh_hdr->llh_flags & LLOG_F_IS_CAT);

	rc = llog_cat_process(env, ctxt->loc_handle,
			      mdd_changelog_user_purge_cb, (void *)&data,
			      0, 0);
        if ((rc >= 0) && (data.mcud_minrec > 0)) {
                CDEBUG(D_IOCTL, "Purging changelog entries up to "LPD64
                       ", referenced by "CHANGELOG_USER_PREFIX"%d\n",
                       data.mcud_minrec, data.mcud_minid);
		rc = mdd_changelog_llog_cancel(env, mdd, data.mcud_minrec);
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
                rc = mdd_changelog_on(env, mdd, 0);

        RETURN (rc);
}

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
        struct obd_ioctl_data *data = karg;
        int rc;
        ENTRY;

        mdd = lu2mdd_dev(&m->md_lu_dev);

        /* Doesn't use obd_ioctl_data */
        switch (cmd) {
        case OBD_IOC_CHANGELOG_CLEAR: {
                struct changelog_setinfo *cs = karg;
                rc = mdd_changelog_user_purge(env, mdd, cs->cs_id,
                                              cs->cs_recno);
                RETURN(rc);
        }
        case OBD_IOC_GET_MNTOPT: {
                mntopt_t *mntopts = (mntopt_t *)karg;
                *mntopts = mdd->mdd_dt_conf.ddp_mntopts;
                RETURN(0);
        }
	case OBD_IOC_START_LFSCK: {
		rc = mdd_lfsck_start(env, &mdd->mdd_lfsck,
				     (struct lfsck_start *)karg);
		RETURN(rc);
	}
	case OBD_IOC_STOP_LFSCK: {
		rc = mdd_lfsck_stop(env, &mdd->mdd_lfsck, false);
		RETURN(rc);
	}
	case OBD_IOC_PAUSE_LFSCK: {
		rc = mdd_lfsck_stop(env, &mdd->mdd_lfsck, true);
		RETURN(rc);
	}
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
                rc = mdd_changelog_user_register(env, mdd, &data->ioc_u32_1);
                break;
        case OBD_IOC_CHANGELOG_DEREG:
                rc = mdd_changelog_user_purge(env, mdd, data->ioc_u32_1,
                                              MCUD_UNREGISTER);
                break;
        default:
                rc = -ENOTTY;
        }

        RETURN (rc);
}

/* type constructor/destructor: mdd_type_init, mdd_type_fini */
LU_TYPE_INIT_FINI(mdd, &mdd_thread_key, &mdd_capainfo_key);

const struct md_device_operations mdd_ops = {
	.mdo_statfs         = mdd_statfs,
	.mdo_root_get	    = mdd_root_get,
	.mdo_init_capa_ctxt = mdd_init_capa_ctxt,
	.mdo_update_capa_key= mdd_update_capa_key,
	.mdo_llog_ctxt_get  = mdd_llog_ctxt_get,
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

	lu_buf_free(&info->mti_big_buf);
	lu_buf_free(&info->mti_link_buf);

        OBD_FREE_PTR(info);
}

/* context key: mdd_thread_key */
LU_CONTEXT_KEY_DEFINE(mdd, LCT_MD_THREAD);

static int __init mdd_mod_init(void)
{
	struct lprocfs_static_vars lvars;
	int rc;

	lprocfs_mdd_init_vars(&lvars);

	rc = lu_kmem_init(mdd_caches);
	if (rc)
		return rc;

	changelog_orig_logops = llog_osd_ops;
	changelog_orig_logops.lop_cancel = llog_changelog_cancel;
	changelog_orig_logops.lop_add = llog_cat_add_rec;
	changelog_orig_logops.lop_declare_add = llog_cat_declare_add_rec;

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
