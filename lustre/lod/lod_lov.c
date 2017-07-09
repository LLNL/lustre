/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright  2009 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2015, Intel Corporation.
 */
/*
 * lustre/lod/lod_lov.c
 *
 * A set of helpers to maintain Logical Object Volume (LOV)
 * Extended Attribute (EA) and known OST targets
 *
 * Author: Alex Zhuravlev <alexey.zhuravlev@intel.com>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <obd_class.h>
#include <lustre_lfsck.h>
#include <lustre_lmv.h>
#include <lustre_swab.h>

#include "lod_internal.h"

/**
 * Increase reference count on the target table.
 *
 * Increase reference count on the target table usage to prevent racing with
 * addition/deletion. Any function that expects the table to remain
 * stationary must take a ref.
 *
 * \param[in] ltd	target table (lod_ost_descs or lod_mdt_descs)
 */
void lod_getref(struct lod_tgt_descs *ltd)
{
	down_read(&ltd->ltd_rw_sem);
	mutex_lock(&ltd->ltd_mutex);
	ltd->ltd_refcount++;
	mutex_unlock(&ltd->ltd_mutex);
}

/**
 * Decrease reference count on the target table.
 *
 * Companion of lod_getref() to release a reference on the target table.
 * If this is the last reference and the OST entry was scheduled for deletion,
 * the descriptor is removed from the table.
 *
 * \param[in] lod	LOD device from which we release a reference
 * \param[in] ltd	target table (lod_ost_descs or lod_mdt_descs)
 */
void lod_putref(struct lod_device *lod, struct lod_tgt_descs *ltd)
{
	mutex_lock(&ltd->ltd_mutex);
	ltd->ltd_refcount--;
	if (ltd->ltd_refcount == 0 && ltd->ltd_death_row) {
		struct lod_tgt_desc *tgt_desc, *tmp;
		struct list_head kill;
		unsigned int idx;

		CDEBUG(D_CONFIG, "destroying %d ltd desc\n",
		       ltd->ltd_death_row);

		INIT_LIST_HEAD(&kill);

		cfs_foreach_bit(ltd->ltd_tgt_bitmap, idx) {
			tgt_desc = LTD_TGT(ltd, idx);
			LASSERT(tgt_desc);

			if (!tgt_desc->ltd_reap)
				continue;

			list_add(&tgt_desc->ltd_kill, &kill);
			LTD_TGT(ltd, idx) = NULL;
			/*FIXME: only support ost pool for now */
			if (ltd == &lod->lod_ost_descs) {
				lod_ost_pool_remove(&lod->lod_pool_info, idx);
				if (tgt_desc->ltd_active)
					lod->lod_desc.ld_active_tgt_count--;
			}
			ltd->ltd_tgtnr--;
			cfs_bitmap_clear(ltd->ltd_tgt_bitmap, idx);
			ltd->ltd_death_row--;
		}
		mutex_unlock(&ltd->ltd_mutex);
		up_read(&ltd->ltd_rw_sem);

		list_for_each_entry_safe(tgt_desc, tmp, &kill, ltd_kill) {
			int rc;
			list_del(&tgt_desc->ltd_kill);
			if (ltd == &lod->lod_ost_descs) {
				/* remove from QoS structures */
				rc = qos_del_tgt(lod, tgt_desc);
				if (rc)
					CERROR("%s: qos_del_tgt(%s) failed:"
					       "rc = %d\n",
					       lod2obd(lod)->obd_name,
					      obd_uuid2str(&tgt_desc->ltd_uuid),
					       rc);
			}
			rc = obd_disconnect(tgt_desc->ltd_exp);
			if (rc)
				CERROR("%s: failed to disconnect %s: rc = %d\n",
				       lod2obd(lod)->obd_name,
				       obd_uuid2str(&tgt_desc->ltd_uuid), rc);
			OBD_FREE_PTR(tgt_desc);
		}
	} else {
		mutex_unlock(&ltd->ltd_mutex);
		up_read(&ltd->ltd_rw_sem);
	}
}

/**
 * Expand size of target table.
 *
 * When the target table is full, we have to extend the table. To do so,
 * we allocate new memory with some reserve, move data from the old table
 * to the new one and release memory consumed by the old table.
 * Notice we take ltd_rw_sem exclusively to ensure atomic switch.
 *
 * \param[in] ltd		target table
 * \param[in] newsize		new size of the table
 *
 * \retval			0 on success
 * \retval			-ENOMEM if reallocation failed
 */
static int ltd_bitmap_resize(struct lod_tgt_descs *ltd, __u32 newsize)
{
	cfs_bitmap_t *new_bitmap, *old_bitmap = NULL;
	int	      rc = 0;
	ENTRY;

	/* grab write reference on the lod. Relocating the array requires
	 * exclusive access */

	down_write(&ltd->ltd_rw_sem);
	if (newsize <= ltd->ltd_tgts_size)
		/* someone else has already resize the array */
		GOTO(out, rc = 0);

	/* allocate new bitmap */
	new_bitmap = CFS_ALLOCATE_BITMAP(newsize);
	if (!new_bitmap)
		GOTO(out, rc = -ENOMEM);

	if (ltd->ltd_tgts_size > 0) {
		/* the bitmap already exists, we need
		 * to copy data from old one */
		cfs_bitmap_copy(new_bitmap, ltd->ltd_tgt_bitmap);
		old_bitmap = ltd->ltd_tgt_bitmap;
	}

	ltd->ltd_tgts_size  = newsize;
	ltd->ltd_tgt_bitmap = new_bitmap;

	if (old_bitmap)
		CFS_FREE_BITMAP(old_bitmap);

	CDEBUG(D_CONFIG, "tgt size: %d\n", ltd->ltd_tgts_size);

	EXIT;
out:
	up_write(&ltd->ltd_rw_sem);
	return rc;
}

/**
 * Connect LOD to a new OSP and add it to the target table.
 *
 * Connect to the OSP device passed, initialize all the internal
 * structures related to the device and add it to the target table.
 *
 * \param[in] env		execution environment for this thread
 * \param[in] lod		LOD device to be connected to the new OSP
 * \param[in] osp		name of OSP device name to be added
 * \param[in] index		index of the new target
 * \param[in] gen		target's generation number
 * \param[in] tgt_index		OSP's group
 * \param[in] type		type of device (mdc or osc)
 * \param[in] active		state of OSP: 0 - inactive, 1 - active
 *
 * \retval			0 if added successfully
 * \retval			negative error number on failure
 */
int lod_add_device(const struct lu_env *env, struct lod_device *lod,
		   char *osp, unsigned index, unsigned gen, int tgt_index,
		   char *type, int active)
{
	struct obd_connect_data *data = NULL;
	struct obd_export	*exp = NULL;
	struct obd_device	*obd;
	struct lu_device	*lu_dev;
	struct dt_device	*dt_dev;
	int			 rc;
	struct lod_tgt_desc     *tgt_desc;
	struct lod_tgt_descs    *ltd;
	struct lustre_cfg	*lcfg;
	struct obd_uuid		obd_uuid;
	bool			for_ost;
	bool lock = false;
	ENTRY;

	CDEBUG(D_CONFIG, "osp:%s idx:%d gen:%d\n", osp, index, gen);

	if (gen <= 0) {
		CERROR("request to add OBD %s with invalid generation: %d\n",
		       osp, gen);
		RETURN(-EINVAL);
	}

	obd_str2uuid(&obd_uuid, osp);

	obd = class_find_client_obd(&obd_uuid, LUSTRE_OSP_NAME,
				&lod->lod_dt_dev.dd_lu_dev.ld_obd->obd_uuid);
	if (obd == NULL) {
		CERROR("can't find %s device\n", osp);
		RETURN(-EINVAL);
	}

	LASSERT(obd->obd_lu_dev != NULL);
	LASSERT(obd->obd_lu_dev->ld_site == lod->lod_dt_dev.dd_lu_dev.ld_site);

	lu_dev = obd->obd_lu_dev;
	dt_dev = lu2dt_dev(lu_dev);

	OBD_ALLOC_PTR(data);
	if (data == NULL)
		GOTO(out_cleanup, rc = -ENOMEM);

	data->ocd_connect_flags = OBD_CONNECT_INDEX | OBD_CONNECT_VERSION;
	data->ocd_version = LUSTRE_VERSION_CODE;
	data->ocd_index = index;

	if (strcmp(LUSTRE_OSC_NAME, type) == 0) {
		for_ost = true;
		data->ocd_connect_flags |= OBD_CONNECT_AT |
					   OBD_CONNECT_FULL20 |
					   OBD_CONNECT_INDEX |
#ifdef HAVE_LRU_RESIZE_SUPPORT
					   OBD_CONNECT_LRU_RESIZE |
#endif
					   OBD_CONNECT_MDS |
					   OBD_CONNECT_REQPORTAL |
					   OBD_CONNECT_SKIP_ORPHAN |
					   OBD_CONNECT_FID |
					   OBD_CONNECT_LVB_TYPE |
					   OBD_CONNECT_VERSION |
					   OBD_CONNECT_PINGLESS |
					   OBD_CONNECT_LFSCK |
					   OBD_CONNECT_BULK_MBITS;

		data->ocd_group = tgt_index;
		ltd = &lod->lod_ost_descs;
	} else {
		struct obd_import *imp = obd->u.cli.cl_import;

		for_ost = false;
		data->ocd_ibits_known = MDS_INODELOCK_UPDATE;
		data->ocd_connect_flags |= OBD_CONNECT_ACL |
					   OBD_CONNECT_IBITS |
					   OBD_CONNECT_MDS_MDS |
					   OBD_CONNECT_FID |
					   OBD_CONNECT_AT |
					   OBD_CONNECT_FULL20 |
					   OBD_CONNECT_LFSCK |
					   OBD_CONNECT_BULK_MBITS;
		spin_lock(&imp->imp_lock);
		imp->imp_server_timeout = 1;
		spin_unlock(&imp->imp_lock);
		imp->imp_client->cli_request_portal = OUT_PORTAL;
		CDEBUG(D_OTHER, "%s: Set 'mds' portal and timeout\n",
		      obd->obd_name);
		ltd = &lod->lod_mdt_descs;
	}

	rc = obd_connect(env, &exp, obd, &obd->obd_uuid, data, NULL);
	OBD_FREE_PTR(data);
	if (rc) {
		CERROR("%s: cannot connect to next dev %s (%d)\n",
		       obd->obd_name, osp, rc);
		GOTO(out_cleanup, rc);
	}

	/* Allocate ost descriptor and fill it */
	OBD_ALLOC_PTR(tgt_desc);
	if (!tgt_desc)
		GOTO(out_conn, rc = -ENOMEM);

	tgt_desc->ltd_tgt    = dt_dev;
	tgt_desc->ltd_exp    = exp;
	tgt_desc->ltd_uuid   = obd->u.cli.cl_target_uuid;
	tgt_desc->ltd_gen    = gen;
	tgt_desc->ltd_index  = index;
	tgt_desc->ltd_active = active;

	lod_getref(ltd);
	if (index >= ltd->ltd_tgts_size) {
		/* we have to increase the size of the lod_osts array */
		__u32  newsize;

		newsize = max(ltd->ltd_tgts_size, (__u32)2);
		while (newsize < index + 1)
			newsize = newsize << 1;

		/* lod_bitmap_resize() needs lod_rw_sem
		 * which we hold with th reference */
		lod_putref(lod, ltd);

		rc = ltd_bitmap_resize(ltd, newsize);
		if (rc)
			GOTO(out_desc, rc);

		lod_getref(ltd);
	}

	mutex_lock(&ltd->ltd_mutex);
	lock = true;
	if (cfs_bitmap_check(ltd->ltd_tgt_bitmap, index)) {
		CERROR("%s: device %d is registered already\n", obd->obd_name,
		       index);
		GOTO(out_mutex, rc = -EEXIST);
	}

	if (ltd->ltd_tgt_idx[index / TGT_PTRS_PER_BLOCK] == NULL) {
		OBD_ALLOC_PTR(ltd->ltd_tgt_idx[index / TGT_PTRS_PER_BLOCK]);
		if (ltd->ltd_tgt_idx[index / TGT_PTRS_PER_BLOCK] == NULL) {
			CERROR("can't allocate index to add %s\n",
			       obd->obd_name);
			GOTO(out_mutex, rc = -ENOMEM);
		}
	}

	if (for_ost) {
		/* pool and qos are not supported for MDS stack yet */
		rc = lod_ost_pool_add(&lod->lod_pool_info, index,
				      lod->lod_osts_size);
		if (rc) {
			CERROR("%s: can't set up pool, failed with %d\n",
			       obd->obd_name, rc);
			GOTO(out_mutex, rc);
		}

		rc = qos_add_tgt(lod, tgt_desc);
		if (rc) {
			CERROR("%s: qos_add_tgt failed with %d\n",
				obd->obd_name, rc);
			GOTO(out_pool, rc);
		}

		/* The new OST is now a full citizen */
		if (index >= lod->lod_desc.ld_tgt_count)
			lod->lod_desc.ld_tgt_count = index + 1;
		if (active)
			lod->lod_desc.ld_active_tgt_count++;
	}

	LTD_TGT(ltd, index) = tgt_desc;
	cfs_bitmap_set(ltd->ltd_tgt_bitmap, index);
	ltd->ltd_tgtnr++;
	mutex_unlock(&ltd->ltd_mutex);
	lod_putref(lod, ltd);
	lock = false;
	if (lod->lod_recovery_completed)
		lu_dev->ld_ops->ldo_recovery_complete(env, lu_dev);

	if (!for_ost && lod->lod_initialized) {
		rc = lod_sub_init_llog(env, lod, tgt_desc->ltd_tgt);
		if (rc != 0) {
			CERROR("%s: cannot start llog on %s:rc = %d\n",
			       lod2obd(lod)->obd_name, osp, rc);
			GOTO(out_ltd, rc);
		}
	}

	rc = lfsck_add_target(env, lod->lod_child, dt_dev, exp, index, for_ost);
	if (rc != 0) {
		CERROR("Fail to add LFSCK target: name = %s, type = %s, "
		       "index = %u, rc = %d\n", osp, type, index, rc);
		GOTO(out_fini_llog, rc);
	}
	RETURN(rc);
out_fini_llog:
	lod_sub_fini_llog(env, tgt_desc->ltd_tgt,
			  tgt_desc->ltd_recovery_thread);
out_ltd:
	lod_getref(ltd);
	mutex_lock(&ltd->ltd_mutex);
	lock = true;
	if (!for_ost && LTD_TGT(ltd, index)->ltd_recovery_thread != NULL) {
		struct ptlrpc_thread *thread;

		thread = LTD_TGT(ltd, index)->ltd_recovery_thread;
		OBD_FREE_PTR(thread);
	}
	ltd->ltd_tgtnr--;
	cfs_bitmap_clear(ltd->ltd_tgt_bitmap, index);
	LTD_TGT(ltd, index) = NULL;
out_pool:
	lod_ost_pool_remove(&lod->lod_pool_info, index);
out_mutex:
	if (lock) {
		mutex_unlock(&ltd->ltd_mutex);
		lod_putref(lod, ltd);
	}
out_desc:
	OBD_FREE_PTR(tgt_desc);
out_conn:
	obd_disconnect(exp);
out_cleanup:
	/* XXX OSP needs us to send down LCFG_CLEANUP because it uses
	 * objects from the MDT stack. See LU-7184. */
	lcfg = &lod_env_info(env)->lti_lustre_cfg;
	memset(lcfg, 0, sizeof(*lcfg));
	lcfg->lcfg_version = LUSTRE_CFG_VERSION;
	lcfg->lcfg_command = LCFG_CLEANUP;
	lu_dev->ld_ops->ldo_process_config(env, lu_dev, lcfg);

	return rc;
}

/**
 * Schedule target removal from the target table.
 *
 * Mark the device as dead. The device is not removed here because it may
 * still be in use. The device will be removed in lod_putref() when the
 * last reference is released.
 *
 * \param[in] env		execution environment for this thread
 * \param[in] lod		LOD device the target table belongs to
 * \param[in] ltd		target table
 * \param[in] idx		index of the target
 * \param[in] for_ost		type of the target: 0 - MDT, 1 - OST
 */
static void __lod_del_device(const struct lu_env *env, struct lod_device *lod,
			     struct lod_tgt_descs *ltd, unsigned idx,
			     bool for_ost)
{
	LASSERT(LTD_TGT(ltd, idx));

	lfsck_del_target(env, lod->lod_child, LTD_TGT(ltd, idx)->ltd_tgt,
			 idx, for_ost);

	if (!for_ost && LTD_TGT(ltd, idx)->ltd_recovery_thread != NULL) {
		struct ptlrpc_thread *thread;

		thread = LTD_TGT(ltd, idx)->ltd_recovery_thread;
		OBD_FREE_PTR(thread);
	}

	if (LTD_TGT(ltd, idx)->ltd_reap == 0) {
		LTD_TGT(ltd, idx)->ltd_reap = 1;
		ltd->ltd_death_row++;
	}
}

/**
 * Schedule removal of all the targets from the given target table.
 *
 * See more details in the description for __lod_del_device()
 *
 * \param[in] env		execution environment for this thread
 * \param[in] lod		LOD device the target table belongs to
 * \param[in] ltd		target table
 * \param[in] for_ost		type of the target: MDT or OST
 *
 * \retval			0 always
 */
int lod_fini_tgt(const struct lu_env *env, struct lod_device *lod,
		 struct lod_tgt_descs *ltd, bool for_ost)
{
	unsigned int idx;

	if (ltd->ltd_tgts_size <= 0)
		return 0;
	lod_getref(ltd);
	mutex_lock(&ltd->ltd_mutex);
	cfs_foreach_bit(ltd->ltd_tgt_bitmap, idx)
		__lod_del_device(env, lod, ltd, idx, for_ost);
	mutex_unlock(&ltd->ltd_mutex);
	lod_putref(lod, ltd);
	CFS_FREE_BITMAP(ltd->ltd_tgt_bitmap);
	for (idx = 0; idx < TGT_PTRS; idx++) {
		if (ltd->ltd_tgt_idx[idx])
			OBD_FREE_PTR(ltd->ltd_tgt_idx[idx]);
	}
	ltd->ltd_tgts_size = 0;
	return 0;
}

/**
 * Remove device by name.
 *
 * Remove a device identified by \a osp from the target table. Given
 * the device can be in use, the real deletion happens in lod_putref().
 *
 * \param[in] env		execution environment for this thread
 * \param[in] lod		LOD device to be connected to the new OSP
 * \param[in] ltd		target table
 * \param[in] osp		name of OSP device to be removed
 * \param[in] idx		index of the target
 * \param[in] gen		generation number, not used currently
 * \param[in] for_ost		type of the target: 0 - MDT, 1 - OST
 *
 * \retval			0 if the device was scheduled for removal
 * \retval			-EINVAL if no device was found
 */
int lod_del_device(const struct lu_env *env, struct lod_device *lod,
		   struct lod_tgt_descs *ltd, char *osp, unsigned idx,
		   unsigned gen, bool for_ost)
{
	struct obd_device *obd;
	int                rc = 0;
	struct obd_uuid    uuid;
	ENTRY;

	CDEBUG(D_CONFIG, "osp:%s idx:%d gen:%d\n", osp, idx, gen);

	obd_str2uuid(&uuid, osp);

	obd = class_find_client_obd(&uuid, LUSTRE_OSP_NAME,
				   &lod->lod_dt_dev.dd_lu_dev.ld_obd->obd_uuid);
	if (obd == NULL) {
		CERROR("can't find %s device\n", osp);
		RETURN(-EINVAL);
	}

	if (gen <= 0) {
		CERROR("%s: request to remove OBD %s with invalid generation %d"
		       "\n", obd->obd_name, osp, gen);
		RETURN(-EINVAL);
	}

	obd_str2uuid(&uuid,  osp);

	lod_getref(ltd);
	mutex_lock(&ltd->ltd_mutex);
	/* check that the index is allocated in the bitmap */
	if (!cfs_bitmap_check(ltd->ltd_tgt_bitmap, idx) ||
	    !LTD_TGT(ltd, idx)) {
		CERROR("%s: device %d is not set up\n", obd->obd_name, idx);
		GOTO(out, rc = -EINVAL);
	}

	/* check that the UUID matches */
	if (!obd_uuid_equals(&uuid, &LTD_TGT(ltd, idx)->ltd_uuid)) {
		CERROR("%s: LOD target UUID %s at index %d does not match %s\n",
		       obd->obd_name, obd_uuid2str(&LTD_TGT(ltd,idx)->ltd_uuid),
		       idx, osp);
		GOTO(out, rc = -EINVAL);
	}

	__lod_del_device(env, lod, ltd, idx, for_ost);
	EXIT;
out:
	mutex_unlock(&ltd->ltd_mutex);
	lod_putref(lod, ltd);
	return(rc);
}

/**
 * Resize per-thread storage to hold specified size.
 *
 * A helper function to resize per-thread temporary storage. This storage
 * is used to process LOV/LVM EAs and may be quite large. We do not want to
 * allocate/release it every time, so instead we put it into the env and
 * reallocate on demand. The memory is released when the correspondent thread
 * is finished.
 *
 * \param[in] info		LOD-specific storage in the environment
 * \param[in] size		new size to grow the buffer to

 * \retval			0 on success, -ENOMEM if reallocation failed
 */
int lod_ea_store_resize(struct lod_thread_info *info, size_t size)
{
	__u32 round = size_roundup_power2(size);

	LASSERT(round <=
		lov_mds_md_size(LOV_MAX_STRIPE_COUNT, LOV_MAGIC_V3));
	if (info->lti_ea_store) {
		LASSERT(info->lti_ea_store_size);
		LASSERT(info->lti_ea_store_size < round);
		CDEBUG(D_INFO, "EA store size %d is not enough, need %d\n",
		       info->lti_ea_store_size, round);
		OBD_FREE_LARGE(info->lti_ea_store, info->lti_ea_store_size);
		info->lti_ea_store = NULL;
		info->lti_ea_store_size = 0;
	}

	OBD_ALLOC_LARGE(info->lti_ea_store, round);
	if (info->lti_ea_store == NULL)
		RETURN(-ENOMEM);
	info->lti_ea_store_size = round;

	RETURN(0);
}

/**
 * Make LOV EA for striped object.
 *
 * Generate striping information and store it in the LOV EA of the given
 * object. The caller must ensure nobody else is calling the function
 * against the object concurrently. The transaction must be started.
 * FLDB service must be running as well; it's used to map FID to the target,
 * which is stored in LOV EA.
 *
 * \param[in] env		execution environment for this thread
 * \param[in] lo		LOD object
 * \param[in] th		transaction handle
 *
 * \retval			0 if LOV EA is stored successfully
 * \retval			negative error number on failure
 */
int lod_generate_and_set_lovea(const struct lu_env *env,
			       struct lod_object *lo, struct thandle *th)
{
	struct lod_thread_info	*info = lod_env_info(env);
	struct dt_object	*next = dt_object_child(&lo->ldo_obj);
	const struct lu_fid	*fid  = lu_object_fid(&lo->ldo_obj.do_lu);
	struct lov_mds_md_v1	*lmm;
	struct lov_ost_data_v1	*objs;
	__u32			 magic;
	int			 i, rc;
	size_t			 lmm_size;
	ENTRY;

	LASSERT(lo);

	magic = lo->ldo_pool != NULL ? LOV_MAGIC_V3 : LOV_MAGIC_V1;
	lmm_size = lov_mds_md_size(lo->ldo_stripenr, magic);
	if (info->lti_ea_store_size < lmm_size) {
		rc = lod_ea_store_resize(info, lmm_size);
		if (rc)
			RETURN(rc);
	}

	if (lo->ldo_pattern == 0) /* default striping */
		lo->ldo_pattern = LOV_PATTERN_RAID0;

	lmm = info->lti_ea_store;

	lmm->lmm_magic = cpu_to_le32(magic);
	lmm->lmm_pattern = cpu_to_le32(lo->ldo_pattern);
	fid_to_lmm_oi(fid, &lmm->lmm_oi);
	if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_BAD_LMMOI))
		lmm->lmm_oi.oi.oi_id++;
	lmm_oi_cpu_to_le(&lmm->lmm_oi, &lmm->lmm_oi);
	lmm->lmm_stripe_size = cpu_to_le32(lo->ldo_stripe_size);
	lmm->lmm_stripe_count = cpu_to_le16(lo->ldo_stripenr);
	if (lo->ldo_pattern & LOV_PATTERN_F_RELEASED)
		lmm->lmm_stripe_count = cpu_to_le16(lo->ldo_released_stripenr);
	lmm->lmm_layout_gen = 0;
	if (magic == LOV_MAGIC_V1) {
		objs = &lmm->lmm_objects[0];
	} else {
		struct lov_mds_md_v3 *v3 = (struct lov_mds_md_v3 *) lmm;
		size_t cplen = strlcpy(v3->lmm_pool_name, lo->ldo_pool,
				sizeof(v3->lmm_pool_name));
		if (cplen >= sizeof(v3->lmm_pool_name))
			RETURN(-E2BIG);
		objs = &v3->lmm_objects[0];
	}

	for (i = 0; i < lo->ldo_stripenr; i++) {
		struct lu_fid		*fid	= &info->lti_fid;
		struct lod_device	*lod;
		__u32			index;
		int			type	= LU_SEQ_RANGE_OST;

		lod = lu2lod_dev(lo->ldo_obj.do_lu.lo_dev);
		LASSERT(lo->ldo_stripe[i]);

		*fid = *lu_object_fid(&lo->ldo_stripe[i]->do_lu);
		if (OBD_FAIL_CHECK(OBD_FAIL_LFSCK_MULTIPLE_REF)) {
			if (cfs_fail_val == 0)
				cfs_fail_val = fid->f_oid;
			else
				fid->f_oid = cfs_fail_val;
		}

		rc = fid_to_ostid(fid, &info->lti_ostid);
		LASSERT(rc == 0);

		ostid_cpu_to_le(&info->lti_ostid, &objs[i].l_ost_oi);
		objs[i].l_ost_gen    = cpu_to_le32(0);
		if (OBD_FAIL_CHECK(OBD_FAIL_MDS_FLD_LOOKUP))
			rc = -ENOENT;
		else
			rc = lod_fld_lookup(env, lod, fid,
					    &index, &type);
		if (rc < 0) {
			CERROR("%s: Can not locate "DFID": rc = %d\n",
			       lod2obd(lod)->obd_name, PFID(fid), rc);
			lod_object_free_striping(env, lo);
			RETURN(rc);
		}
		objs[i].l_ost_idx = cpu_to_le32(index);
	}

	info->lti_buf.lb_buf = lmm;
	info->lti_buf.lb_len = lmm_size;
	rc = lod_sub_object_xattr_set(env, next, &info->lti_buf, XATTR_NAME_LOV,
				      0, th);
	if (rc < 0) {
		lod_object_free_striping(env, lo);
		RETURN(rc);
	}

	RETURN(rc);
}

/**
 * Get LOV EA.
 *
 * Fill lti_ea_store buffer in the environment with a value for the given
 * EA. The buffer is reallocated if the value doesn't fit.
 *
 * \param[in,out] env		execution environment for this thread
 *				.lti_ea_store buffer is filled with EA's value
 * \param[in] lo		LOD object
 * \param[in] name		name of the EA
 *
 * \retval			0 if EA is fetched successfully
 * \retval			negative error number on failure
 */
int lod_get_ea(const struct lu_env *env, struct lod_object *lo,
	       const char *name)
{
	struct lod_thread_info	*info = lod_env_info(env);
	struct dt_object	*next = dt_object_child(&lo->ldo_obj);
	int			rc;
	ENTRY;

	LASSERT(info);

	if (unlikely(info->lti_ea_store == NULL)) {
		/* just to enter in allocation block below */
		rc = -ERANGE;
	} else {
repeat:
		info->lti_buf.lb_buf = info->lti_ea_store;
		info->lti_buf.lb_len = info->lti_ea_store_size;
		rc = dt_xattr_get(env, next, &info->lti_buf, name);
	}

	/* if object is not striped or inaccessible */
	if (rc == -ENODATA || rc == -ENOENT)
		RETURN(0);

	if (rc == -ERANGE) {
		/* EA doesn't fit, reallocate new buffer */
		rc = dt_xattr_get(env, next, &LU_BUF_NULL, name);
		if (rc == -ENODATA || rc == -ENOENT)
			RETURN(0);
		else if (rc < 0)
			RETURN(rc);

		LASSERT(rc > 0);
		rc = lod_ea_store_resize(info, rc);
		if (rc)
			RETURN(rc);
		goto repeat;
	}

	RETURN(rc);
}

/**
 * Verify the target index is present in the current configuration.
 *
 * \param[in] md		LOD device where the target table is stored
 * \param[in] idx		target's index
 *
 * \retval			0 if the index is present
 * \retval			-EINVAL if not
 */
static int validate_lod_and_idx(struct lod_device *md, __u32 idx)
{
	if (unlikely(idx >= md->lod_ost_descs.ltd_tgts_size ||
		     !cfs_bitmap_check(md->lod_ost_bitmap, idx))) {
		CERROR("%s: bad idx: %d of %d\n", lod2obd(md)->obd_name, idx,
		       md->lod_ost_descs.ltd_tgts_size);
		return -EINVAL;
	}

	if (unlikely(OST_TGT(md, idx) == NULL)) {
		CERROR("%s: bad lod_tgt_desc for idx: %d\n",
		       lod2obd(md)->obd_name, idx);
		return -EINVAL;
	}

	if (unlikely(OST_TGT(md, idx)->ltd_ost == NULL)) {
		CERROR("%s: invalid lod device, for idx: %d\n",
		       lod2obd(md)->obd_name , idx);
		return -EINVAL;
	}

	return 0;
}

/**
 * Instantiate objects for stripes.
 *
 * Allocate and initialize LU-objects representing the stripes. The number
 * of the stripes (ldo_stripenr) must be initialized already. The caller
 * must ensure nobody else is calling the function on the object at the same
 * time. FLDB service must be running to be able to map a FID to the targets
 * and find appropriate device representing that target.
 *
 * \param[in] env		execution environment for this thread
 * \param[in,out] lo		LOD object
 * \param[in] objs		an array of IDs to creates the objects from
 *
 * \retval			0 if the objects are instantiated successfully
 * \retval			negative error number on failure
 */
int lod_initialize_objects(const struct lu_env *env, struct lod_object *lo,
			   struct lov_ost_data_v1 *objs)
{
	struct lod_thread_info	*info = lod_env_info(env);
	struct lod_device	*md;
	struct lu_object	*o, *n;
	struct lu_device	*nd;
	struct dt_object       **stripe;
	int			 stripe_len;
	int			 i, rc = 0;
	__u32			idx;
	ENTRY;

	LASSERT(lo != NULL);
	md = lu2lod_dev(lo->ldo_obj.do_lu.lo_dev);
	LASSERT(lo->ldo_stripe == NULL);
	LASSERT(lo->ldo_stripenr > 0);
	LASSERT(lo->ldo_stripe_size > 0);

	stripe_len = lo->ldo_stripenr;
	OBD_ALLOC(stripe, sizeof(stripe[0]) * stripe_len);
	if (stripe == NULL)
		RETURN(-ENOMEM);

	for (i = 0; i < lo->ldo_stripenr; i++) {
		if (unlikely(lovea_slot_is_dummy(&objs[i])))
			continue;

		ostid_le_to_cpu(&objs[i].l_ost_oi, &info->lti_ostid);
		idx = le32_to_cpu(objs[i].l_ost_idx);
		rc = ostid_to_fid(&info->lti_fid, &info->lti_ostid, idx);
		if (rc != 0)
			GOTO(out, rc);
		LASSERTF(fid_is_sane(&info->lti_fid), ""DFID" insane!\n",
			 PFID(&info->lti_fid));
		lod_getref(&md->lod_ost_descs);

		rc = validate_lod_and_idx(md, idx);
		if (unlikely(rc != 0)) {
			lod_putref(md, &md->lod_ost_descs);
			GOTO(out, rc);
		}

		nd = &OST_TGT(md,idx)->ltd_ost->dd_lu_dev;
		lod_putref(md, &md->lod_ost_descs);

		/* In the function below, .hs_keycmp resolves to
		 * u_obj_hop_keycmp() */
		/* coverity[overrun-buffer-val] */
		o = lu_object_find_at(env, nd, &info->lti_fid, NULL);
		if (IS_ERR(o))
			GOTO(out, rc = PTR_ERR(o));

		n = lu_object_locate(o->lo_header, nd->ld_type);
		LASSERT(n);

		stripe[i] = container_of(n, struct dt_object, do_lu);
	}

out:
	if (rc != 0) {
		for (i = 0; i < stripe_len; i++)
			if (stripe[i] != NULL)
				lu_object_put(env, &stripe[i]->do_lu);

		OBD_FREE(stripe, sizeof(stripe[0]) * stripe_len);
		lo->ldo_stripenr = 0;
	} else {
		lo->ldo_stripe = stripe;
		lo->ldo_stripes_allocated = stripe_len;
	}

	RETURN(rc);
}

/**
 * Instantiate objects for striping.
 *
 * Parse striping information in \a buf and instantiate the objects
 * representing the stripes.
 *
 * \param[in] env		execution environment for this thread
 * \param[in] lo		LOD object
 * \param[in] buf		buffer storing LOV EA to parse
 *
 * \retval			0 if parsing and objects creation succeed
 * \retval			negative error number on failure
 */
int lod_parse_striping(const struct lu_env *env, struct lod_object *lo,
		       const struct lu_buf *buf)
{
	struct lov_mds_md_v1	*lmm;
	struct lov_ost_data_v1	*objs;
	__u32			 magic;
	__u32			 pattern;
	int			 rc = 0;
	ENTRY;

	LASSERT(buf);
	LASSERT(buf->lb_buf);
	LASSERT(buf->lb_len);

	lmm = (struct lov_mds_md_v1 *) buf->lb_buf;
	magic = le32_to_cpu(lmm->lmm_magic);
	pattern = le32_to_cpu(lmm->lmm_pattern);

	if (magic != LOV_MAGIC_V1 && magic != LOV_MAGIC_V3)
		GOTO(out, rc = -EINVAL);
	if (lov_pattern(pattern) != LOV_PATTERN_RAID0)
		GOTO(out, rc = -EINVAL);

	lo->ldo_pattern = pattern;
	lo->ldo_stripe_size = le32_to_cpu(lmm->lmm_stripe_size);
	lo->ldo_layout_gen = le16_to_cpu(lmm->lmm_layout_gen);
	lo->ldo_stripenr = le16_to_cpu(lmm->lmm_stripe_count);
	/* released file stripenr fixup. */
	if (pattern & LOV_PATTERN_F_RELEASED)
		lo->ldo_stripenr = 0;

	LASSERT(buf->lb_len >= lov_mds_md_size(lo->ldo_stripenr, magic));

	if (magic == LOV_MAGIC_V3) {
		struct lov_mds_md_v3 *v3 = (struct lov_mds_md_v3 *) lmm;
		objs = &v3->lmm_objects[0];
		/* no need to set pool, which is used in create only */
	} else {
		objs = &lmm->lmm_objects[0];
	}

	if (lo->ldo_stripenr > 0)
		rc = lod_initialize_objects(env, lo, objs);

out:
	RETURN(rc);
}

/**
 * Initialize the object representing the stripes.
 *
 * Unless the stripes are initialized already, fetch LOV (for regular
 * objects) or LMV (for directory objects) EA and call lod_parse_striping()
 * to instantiate the objects representing the stripes.
 *
 * \param[in] env		execution environment for this thread
 * \param[in,out] lo		LOD object
 *
 * \retval			0 if parsing and object creation succeed
 * \retval			negative error number on failure
 */
int lod_load_striping_locked(const struct lu_env *env, struct lod_object *lo)
{
	struct lod_thread_info	*info = lod_env_info(env);
	struct lu_buf		*buf  = &info->lti_buf;
	struct dt_object	*next = dt_object_child(&lo->ldo_obj);
	int			 rc = 0;
	ENTRY;

	/* already initialized? */
	if (lo->ldo_stripe != NULL)
		GOTO(out, rc = 0);

	if (!dt_object_exists(next))
		GOTO(out, rc = 0);

	/* Do not load stripe for slaves of striped dir */
	if (lo->ldo_dir_slave_stripe)
		GOTO(out, rc = 0);

	if (S_ISREG(lu_object_attr(lod2lu_obj(lo)))) {
		rc = lod_get_lov_ea(env, lo);
		if (rc <= 0)
			GOTO(out, rc);
		/*
		 * there is LOV EA (striping information) in this object
		 * let's parse it and create in-core objects for the stripes
		 */
		buf->lb_buf = info->lti_ea_store;
		buf->lb_len = info->lti_ea_store_size;
		rc = lod_parse_striping(env, lo, buf);
	} else if (S_ISDIR(lu_object_attr(lod2lu_obj(lo)))) {
		rc = lod_get_lmv_ea(env, lo);
		if (rc < (typeof(rc))sizeof(struct lmv_mds_md_v1))
			GOTO(out, rc = rc > 0 ? -EINVAL : rc);

		buf->lb_buf = info->lti_ea_store;
		buf->lb_len = info->lti_ea_store_size;
		if (rc == sizeof(struct lmv_mds_md_v1)) {
			rc = lod_load_lmv_shards(env, lo, buf, true);
			if (buf->lb_buf != info->lti_ea_store) {
				OBD_FREE_LARGE(info->lti_ea_store,
					       info->lti_ea_store_size);
				info->lti_ea_store = buf->lb_buf;
				info->lti_ea_store_size = buf->lb_len;
			}

			if (rc < 0)
				GOTO(out, rc);
		}

		/*
		 * there is LOV EA (striping information) in this object
		 * let's parse it and create in-core objects for the stripes
		 */
		rc = lod_parse_dir_striping(env, lo, buf);
	}
out:
	RETURN(rc);
}

/**
 * A generic function to initialize the stripe objects.
 *
 * A protected version of lod_load_striping_locked() - load the striping
 * information from storage, parse that and instantiate LU objects to
 * represent the stripes.  The LOD object \a lo supplies a pointer to the
 * next sub-object in the LU stack so we can lock it. Also use \a lo to
 * return an array of references to the newly instantiated objects.
 *
 * \param[in] env		execution environment for this thread
 * \param[in,out] lo		LOD object, where striping is stored and
 *				which gets an array of references
 *
 * \retval			0 if parsing and object creation succeed
 * \retval			negative error number on failure
 **/
int lod_load_striping(const struct lu_env *env, struct lod_object *lo)
{
	struct dt_object	*next = dt_object_child(&lo->ldo_obj);
	int			rc = 0;

	/* currently this code is supposed to be called from declaration
	 * phase only, thus the object is not expected to be locked by caller */
	dt_write_lock(env, next, 0);
	rc = lod_load_striping_locked(env, lo);
	dt_write_unlock(env, next);
	return rc;
}

/**
 * Verify striping.
 *
 * Check the validity of all fields including the magic, stripe size,
 * stripe count, stripe offset and that the pool is present.  Also check
 * that each target index points to an existing target. The additional
 * \a is_from_disk turns additional checks. In some cases zero fields
 * are allowed (like pattern=0).
 *
 * \param[in] d			LOD device
 * \param[in] buf		buffer with LOV EA to verify
 * \param[in] is_from_disk	0 - from user, allow some fields to be 0
 *				1 - from disk, do not allow
 *
 * \retval			0 if the striping is valid
 * \retval			-EINVAL if striping is invalid
 */
int lod_verify_striping(struct lod_device *d, const struct lu_buf *buf,
			bool is_from_disk)
{
	struct lov_user_md_v1	*lum;
	struct lov_user_md_v3	*lum3;
	struct pool_desc	*pool = NULL;
	__u32			 magic;
	__u32			 stripe_size;
	__u16			 stripe_count;
	__u16			 stripe_offset;
	size_t			 lum_size;
	int			 rc = 0;
	ENTRY;

	lum = buf->lb_buf;

	LASSERT(sizeof(*lum) < sizeof(*lum3));

	if (buf->lb_len < sizeof(*lum)) {
		CDEBUG(D_IOCTL, "buf len %zu too small for lov_user_md\n",
		       buf->lb_len);
		GOTO(out, rc = -EINVAL);
	}

	magic = le32_to_cpu(lum->lmm_magic);
	if (magic != LOV_USER_MAGIC_V1 &&
	    magic != LOV_USER_MAGIC_V3 &&
	    magic != LOV_MAGIC_V1_DEF &&
	    magic != LOV_MAGIC_V3_DEF) {
		CDEBUG(D_IOCTL, "bad userland LOV MAGIC: %#x\n", magic);
		GOTO(out, rc = -EINVAL);
	}

	/* the user uses "0" for default stripe pattern normally. */
	if (!is_from_disk && lum->lmm_pattern == 0)
		lum->lmm_pattern = cpu_to_le32(LOV_PATTERN_RAID0);

	if (le32_to_cpu(lum->lmm_pattern) != LOV_PATTERN_RAID0) {
		CDEBUG(D_IOCTL, "bad userland stripe pattern: %#x\n",
		       le32_to_cpu(lum->lmm_pattern));
		GOTO(out, rc = -EINVAL);
	}

	/* 64kB is the largest common page size we see (ia64), and matches the
	 * check in lfs */
	stripe_size = le32_to_cpu(lum->lmm_stripe_size);
	if (stripe_size & (LOV_MIN_STRIPE_SIZE - 1)) {
		CDEBUG(D_IOCTL, "stripe size %u not a multiple of %u\n",
		       stripe_size, LOV_MIN_STRIPE_SIZE);
		GOTO(out, rc = -EINVAL);
	}

	stripe_offset = le16_to_cpu(lum->lmm_stripe_offset);
	if (stripe_offset != LOV_OFFSET_DEFAULT) {
		/* if offset is not within valid range [0, osts_size) */
		if (stripe_offset >= d->lod_osts_size) {
			CDEBUG(D_IOCTL, "stripe offset %u >= bitmap size %u\n",
			       stripe_offset, d->lod_osts_size);
			GOTO(out, rc = -EINVAL);
		}

		/* if lmm_stripe_offset is *not* in bitmap */
		if (!cfs_bitmap_check(d->lod_ost_bitmap, stripe_offset)) {
			CDEBUG(D_IOCTL, "stripe offset %u not in bitmap\n",
			       stripe_offset);
			GOTO(out, rc = -EINVAL);
		}
	}

	if (magic == LOV_USER_MAGIC_V1 || magic == LOV_MAGIC_V1_DEF)
		lum_size = offsetof(struct lov_user_md_v1,
				    lmm_objects[0]);
	else if (magic == LOV_USER_MAGIC_V3 || magic == LOV_MAGIC_V3_DEF)
		lum_size = offsetof(struct lov_user_md_v3,
				    lmm_objects[0]);
	else
		GOTO(out, rc = -EINVAL);

	stripe_count = le16_to_cpu(lum->lmm_stripe_count);
	if (buf->lb_len != lum_size) {
		CDEBUG(D_IOCTL, "invalid buf len %zu for lov_user_md with "
		       "magic %#x and stripe_count %u\n",
		       buf->lb_len, magic, stripe_count);
		GOTO(out, rc = -EINVAL);
	}

	if (!(magic == LOV_USER_MAGIC_V3 || magic == LOV_MAGIC_V3_DEF))
		goto out;

	lum3 = buf->lb_buf;
	if (buf->lb_len < sizeof(*lum3)) {
		CDEBUG(D_IOCTL, "buf len %zu too small for lov_user_md_v3\n",
		       buf->lb_len);
		GOTO(out, rc = -EINVAL);
	}

	/* In the function below, .hs_keycmp resolves to
	 * pool_hashkey_keycmp() */
	/* coverity[overrun-buffer-val] */
	pool = lod_find_pool(d, lum3->lmm_pool_name);
	if (pool == NULL)
		goto out;

	if (stripe_offset != LOV_OFFSET_DEFAULT) {
		rc = lod_check_index_in_pool(stripe_offset, pool);
		if (rc < 0)
			GOTO(out, rc = -EINVAL);
	}

	if (is_from_disk && stripe_count > pool_tgt_count(pool)) {
		CDEBUG(D_IOCTL,
		       "stripe count %u > # OSTs %u in the pool\n",
		       stripe_count, pool_tgt_count(pool));
		GOTO(out, rc = -EINVAL);
	}

out:
	if (pool != NULL)
		lod_pool_putref(pool);

	RETURN(rc);
}

void lod_fix_desc_stripe_size(__u64 *val)
{
	if (*val < LOV_MIN_STRIPE_SIZE) {
		if (*val != 0)
			LCONSOLE_INFO("Increasing default stripe size to "
				      "minimum value %u\n",
				      LOV_DESC_STRIPE_SIZE_DEFAULT);
		*val = LOV_DESC_STRIPE_SIZE_DEFAULT;
	} else if (*val & (LOV_MIN_STRIPE_SIZE - 1)) {
		*val &= ~(LOV_MIN_STRIPE_SIZE - 1);
		LCONSOLE_WARN("Changing default stripe size to "LPU64" (a "
			      "multiple of %u)\n",
			      *val, LOV_MIN_STRIPE_SIZE);
	}
}

void lod_fix_desc_stripe_count(__u32 *val)
{
	if (*val == 0)
		*val = 1;
}

void lod_fix_desc_pattern(__u32 *val)
{
	/* from lov_setstripe */
	if ((*val != 0) && (*val != LOV_PATTERN_RAID0)) {
		LCONSOLE_WARN("Unknown stripe pattern: %#x\n", *val);
		*val = 0;
	}
}

void lod_fix_desc_qos_maxage(__u32 *val)
{
	/* fix qos_maxage */
	if (*val == 0)
		*val = LOV_DESC_QOS_MAXAGE_DEFAULT;
}

/**
 * Used to fix insane default striping.
 *
 * \param[in] desc	striping description
 */
void lod_fix_desc(struct lov_desc *desc)
{
	lod_fix_desc_stripe_size(&desc->ld_default_stripe_size);
	lod_fix_desc_stripe_count(&desc->ld_default_stripe_count);
	lod_fix_desc_pattern(&desc->ld_pattern);
	lod_fix_desc_qos_maxage(&desc->ld_qos_maxage);
}

/**
 * Initialize the structures used to store pools and default striping.
 *
 * \param[in] lod	LOD device
 * \param[in] lcfg	configuration structure storing default striping.
 *
 * \retval		0 if initialization succeeds
 * \retval		negative error number on failure
 */
int lod_pools_init(struct lod_device *lod, struct lustre_cfg *lcfg)
{
	struct obd_device	   *obd;
	struct lov_desc		   *desc;
	int			    rc;
	ENTRY;

	obd = class_name2obd(lustre_cfg_string(lcfg, 0));
	LASSERT(obd != NULL);
	obd->obd_lu_dev = &lod->lod_dt_dev.dd_lu_dev;

	if (LUSTRE_CFG_BUFLEN(lcfg, 1) < 1) {
		CERROR("LOD setup requires a descriptor\n");
		RETURN(-EINVAL);
	}

	desc = (struct lov_desc *)lustre_cfg_buf(lcfg, 1);

	if (sizeof(*desc) > LUSTRE_CFG_BUFLEN(lcfg, 1)) {
		CERROR("descriptor size wrong: %d > %d\n",
		       (int)sizeof(*desc), LUSTRE_CFG_BUFLEN(lcfg, 1));
		RETURN(-EINVAL);
	}

	if (desc->ld_magic != LOV_DESC_MAGIC) {
		if (desc->ld_magic == __swab32(LOV_DESC_MAGIC)) {
			CDEBUG(D_OTHER, "%s: Swabbing lov desc %p\n",
			       obd->obd_name, desc);
			lustre_swab_lov_desc(desc);
		} else {
			CERROR("%s: Bad lov desc magic: %#x\n",
			       obd->obd_name, desc->ld_magic);
			RETURN(-EINVAL);
		}
	}

	lod_fix_desc(desc);

	desc->ld_active_tgt_count = 0;
	lod->lod_desc = *desc;

	lod->lod_sp_me = LUSTRE_SP_CLI;

	/* Set up allocation policy (QoS and RR) */
	INIT_LIST_HEAD(&lod->lod_qos.lq_oss_list);
	init_rwsem(&lod->lod_qos.lq_rw_sem);
	lod->lod_qos.lq_dirty = 1;
	lod->lod_qos.lq_rr.lqr_dirty = 1;
	lod->lod_qos.lq_reset = 1;
	/* Default priority is toward free space balance */
	lod->lod_qos.lq_prio_free = 232;
	/* Default threshold for rr (roughly 17%) */
	lod->lod_qos.lq_threshold_rr = 43;

	/* Set up OST pool environment */
	lod->lod_pools_hash_body = cfs_hash_create("POOLS", HASH_POOLS_CUR_BITS,
						   HASH_POOLS_MAX_BITS,
						   HASH_POOLS_BKT_BITS, 0,
						   CFS_HASH_MIN_THETA,
						   CFS_HASH_MAX_THETA,
						   &pool_hash_operations,
						   CFS_HASH_DEFAULT);
	if (lod->lod_pools_hash_body == NULL)
		RETURN(-ENOMEM);

	INIT_LIST_HEAD(&lod->lod_pool_list);
	lod->lod_pool_count = 0;
	rc = lod_ost_pool_init(&lod->lod_pool_info, 0);
	if (rc)
		GOTO(out_hash, rc);
	lod_qos_rr_init(&lod->lod_qos.lq_rr);
	rc = lod_ost_pool_init(&lod->lod_qos.lq_rr.lqr_pool, 0);
	if (rc)
		GOTO(out_pool_info, rc);

	RETURN(0);

out_pool_info:
	lod_ost_pool_free(&lod->lod_pool_info);
out_hash:
	cfs_hash_putref(lod->lod_pools_hash_body);

	return rc;
}

/**
 * Release the structures describing the pools.
 *
 * \param[in] lod	LOD device from which we release the structures
 *
 * \retval		0 always
 */
int lod_pools_fini(struct lod_device *lod)
{
	struct obd_device   *obd = lod2obd(lod);
	struct pool_desc    *pool, *tmp;
	ENTRY;

	list_for_each_entry_safe(pool, tmp, &lod->lod_pool_list, pool_list) {
		/* free pool structs */
		CDEBUG(D_INFO, "delete pool %p\n", pool);
		/* In the function below, .hs_keycmp resolves to
		 * pool_hashkey_keycmp() */
		/* coverity[overrun-buffer-val] */
		lod_pool_del(obd, pool->pool_name);
	}

	cfs_hash_putref(lod->lod_pools_hash_body);
	lod_ost_pool_free(&(lod->lod_qos.lq_rr.lqr_pool));
	lod_ost_pool_free(&lod->lod_pool_info);

	RETURN(0);
}
