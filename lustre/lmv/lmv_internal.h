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
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef _LMV_INTERNAL_H_
#define _LMV_INTERNAL_H_

#include <obd.h>
#include <lustre_lmv.h>

#define LMV_MAX_TGT_COUNT 128

#define LL_IT2STR(it)				        \
	((it) ? ldlm_it2str((it)->it_op) : "0")

int lmv_intent_lock(struct obd_export *exp, struct md_op_data *op_data,
		    struct lookup_intent *it, struct ptlrpc_request **reqp,
		    ldlm_blocking_callback cb_blocking,
		    __u64 extra_lock_flags);

int lmv_blocking_ast(struct ldlm_lock *, struct ldlm_lock_desc *,
		     void *, int);
int lmv_fld_lookup(struct lmv_obd *lmv, const struct lu_fid *fid, u32 *mds);
int __lmv_fid_alloc(struct lmv_obd *lmv, struct lu_fid *fid, u32 mds);
int lmv_fid_alloc(const struct lu_env *env, struct obd_export *exp,
		  struct lu_fid *fid, struct md_op_data *op_data);

int lmv_revalidate_slaves(struct obd_export *exp,
			  const struct lu_fid *pfid,
			  const struct lmv_stripe_md *lsm,
			  ldlm_blocking_callback cb_blocking,
			  int extra_lock_flags);

int lmv_getattr_name(struct obd_export *exp, struct md_op_data *op_data,
		     struct ptlrpc_request **preq);

static inline struct obd_device *lmv2obd_dev(struct lmv_obd *lmv)
{
	return container_of0(lmv, struct obd_device, u.lmv);
}

static inline struct lmv_tgt_desc *
lmv_get_target(struct lmv_obd *lmv, u32 mdt_idx, int *index)
{
	int i;

	for (i = 0; i < lmv->desc.ld_tgt_count; i++) {
		if (lmv->tgts[i] == NULL)
			continue;

		if (lmv->tgts[i]->ltd_idx == mdt_idx) {
			if (index != NULL)
				*index = i;
			return lmv->tgts[i];
		}
	}

	return ERR_PTR(-ENODEV);
}

static inline int
lmv_find_target_index(struct lmv_obd *lmv, const struct lu_fid *fid)
{
	struct lmv_tgt_desc	*ltd;
	u32			mdt_idx = 0;
	int			index = 0;

	if (lmv->desc.ld_tgt_count > 1) {
		int rc;
		rc = lmv_fld_lookup(lmv, fid, &mdt_idx);
		if (rc < 0)
			return rc;
	}

	ltd = lmv_get_target(lmv, mdt_idx, &index);
	if (IS_ERR(ltd))
		return PTR_ERR(ltd);

	return index;
}

static inline struct lmv_tgt_desc *
lmv_find_target(struct lmv_obd *lmv, const struct lu_fid *fid)
{
	int index;

	index = lmv_find_target_index(lmv, fid);
	if (index < 0)
		return ERR_PTR(index);

	return lmv->tgts[index];
}

static inline int lmv_stripe_md_size(int stripe_count)
{
	struct lmv_stripe_md *lsm;

	return sizeof(*lsm) + stripe_count * sizeof(lsm->lsm_md_oinfo[0]);
}

/* for file under migrating directory, return the target stripe info */
static inline const struct lmv_oinfo *
lsm_name_to_stripe_info(const struct lmv_stripe_md *lsm, const char *name,
			int namelen, bool post_migrate)
{
	__u32 hash_type = lsm->lsm_md_hash_type;
	__u32 stripe_count = lsm->lsm_md_stripe_count;
	int stripe_index;

	if (hash_type & LMV_HASH_FLAG_MIGRATION) {
		if (post_migrate) {
			hash_type &= ~LMV_HASH_FLAG_MIGRATION;
			stripe_count = lsm->lsm_md_migrate_offset;
		} else {
			hash_type = lsm->lsm_md_migrate_hash;
			stripe_count -= lsm->lsm_md_migrate_offset;
		}
	}

	stripe_index = lmv_name_to_stripe_index(hash_type, stripe_count,
						name, namelen);
	if (stripe_index < 0)
		return ERR_PTR(stripe_index);

	if ((lsm->lsm_md_hash_type & LMV_HASH_FLAG_MIGRATION) && !post_migrate)
		stripe_index += lsm->lsm_md_migrate_offset;

	if (stripe_index >= lsm->lsm_md_stripe_count) {
		CERROR("stripe_index %d stripe_count %d hash_type %#x "
			"migrate_offset %d migrate_hash %#x name %.*s\n",
			stripe_index, lsm->lsm_md_stripe_count,
			lsm->lsm_md_hash_type, lsm->lsm_md_migrate_offset,
			lsm->lsm_md_migrate_hash, namelen, name);
		return ERR_PTR(-EBADF);
	}

	return &lsm->lsm_md_oinfo[stripe_index];
}

static inline bool lmv_is_dir_migrating(const struct lmv_stripe_md *lsm)
{
	return lsm ? lsm->lsm_md_hash_type & LMV_HASH_FLAG_MIGRATION : false;
}

static inline bool lmv_is_dir_bad_hash(const struct lmv_stripe_md *lsm)
{
	if (!lsm)
		return false;

	if (lmv_is_dir_migrating(lsm)) {
		if (lsm->lsm_md_stripe_count - lsm->lsm_md_migrate_offset > 1)
			return !lmv_is_known_hash_type(
					lsm->lsm_md_migrate_hash);
		return false;
	}

	return !lmv_is_known_hash_type(lsm->lsm_md_hash_type);
}

static inline bool lmv_dir_retry_check_update(struct md_op_data *op_data)
{
	const struct lmv_stripe_md *lsm = op_data->op_mea1;

	if (!lsm)
		return false;

	if (lmv_is_dir_migrating(lsm) && !op_data->op_post_migrate) {
		op_data->op_post_migrate = true;
		return true;
	}

	if (lmv_is_dir_bad_hash(lsm) &&
	    op_data->op_stripe_index < lsm->lsm_md_stripe_count - 1) {
		op_data->op_stripe_index++;
		return true;
	}

	return false;
}

struct lmv_tgt_desc *lmv_locate_tgt(struct lmv_obd *lmv,
				    struct md_op_data *op_data,
				    struct lu_fid *fid);
/* lproc_lmv.c */
int lmv_tunables_init(struct obd_device *obd);

#endif
