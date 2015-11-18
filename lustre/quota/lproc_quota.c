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
 * version 2 along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2011, 2013, Intel Corporation.
 * Use is subject to license terms.
 *
 * Author: Johann Lombardi <johann.lombardi@intel.com>
 * Author: Niu    Yawei    <yawei.niu@intel.com>
 */

#define DEBUG_SUBSYSTEM S_LQUOTA

#include <linux/version.h>
#include <lprocfs_status.h>
#include <obd.h>
#include <linux/seq_file.h>
#include "lquota_internal.h"

#ifdef LPROCFS
/* structure allocated at seq_open time and release when seq_release is called.
 * It is passed to seq_start/stop/next/show which can thus use the same lu_env
 * to be used with the iterator API */
struct lquota_procfs {
	struct dt_object	*lqp_obj;
	struct lu_env		 lqp_env;
};

/* global shared environment */
static void *lprocfs_quota_seq_start(struct seq_file *p, loff_t *pos)
{
	struct lquota_procfs	*lqp = p->private;
	const struct dt_it_ops	*iops;
	struct dt_it		*it;
	loff_t			 offset = *pos;
	int			 rc;

	LASSERT(lqp);

	if (offset == 0)
		return SEQ_START_TOKEN;
	offset--;

	if (lqp->lqp_obj == NULL)
		/* accounting not enabled. */
		return NULL;

	/* initialize iterator */
	iops = &lqp->lqp_obj->do_index_ops->dio_it;
	it = iops->init(&lqp->lqp_env, lqp->lqp_obj, 0, BYPASS_CAPA);
	if (IS_ERR(it)) {
		CERROR("%s: failed to initialize iterator: rc = %ld\n",
		       lqp->lqp_obj->do_lu.lo_dev->ld_obd->obd_name,
		       PTR_ERR(it));
		return NULL;
	}

	/* move on to the first valid record */
	rc = iops->load(&lqp->lqp_env, it, 0);
	if (rc < 0) { /* Error */
		goto not_found;
	} else if (rc == 0) {
		/*
		 * Iterator didn't find record with exactly the key requested.
		 *
		 * It is currently either
		 *
		 *     - positioned above record with key less than
		 *       requested - skip it.
		 *     - or not positioned at all (is in IAM_IT_SKEWED
		 *       state) - position it on the next item.
		 */
		rc = iops->next(&lqp->lqp_env, it);
		if (rc != 0)
			goto not_found;
	}
	while (offset--) {
		rc = iops->next(&lqp->lqp_env, it);
		if (rc != 0) /* Error or reach the end */
			goto not_found;
	}
	return it;

not_found:
	iops->put(&lqp->lqp_env, it);
	iops->fini(&lqp->lqp_env, it);
	return NULL;
}

static void lprocfs_quota_seq_stop(struct seq_file *p, void *v)
{
	struct lquota_procfs	*lqp = p->private;
	const struct dt_it_ops	*iops;
	struct dt_it		*it;

	if (lqp->lqp_obj == NULL || v == NULL || v == SEQ_START_TOKEN)
		return;

	iops = &lqp->lqp_obj->do_index_ops->dio_it;
	it = (struct dt_it *)v;
	/* if something wrong happened during ->seq_show, we need to release
	 * the iterator here */
	iops->put(&lqp->lqp_env, it);
	iops->fini(&lqp->lqp_env, it);
}

static void *lprocfs_quota_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
	struct lquota_procfs	*lqp = p->private;
	const struct dt_it_ops	*iops;
	struct dt_it		*it;
	int			 rc;

	LASSERT(lqp);

	++*pos;
	if (lqp->lqp_obj == NULL)
		return NULL;

	if (v == SEQ_START_TOKEN)
		return lprocfs_quota_seq_start(p, pos);

	iops = &lqp->lqp_obj->do_index_ops->dio_it;
	it = (struct dt_it *)v;

	rc = iops->next(&lqp->lqp_env, it);
	if (rc == 0)
		return it;

	if (rc < 0)
		CERROR("%s: seq_next failed: rc = %d\n",
		       lqp->lqp_obj->do_lu.lo_dev->ld_obd->obd_name, rc);

	/* Reach the end or error */
	iops->put(&lqp->lqp_env, it);
	iops->fini(&lqp->lqp_env, it);
	return NULL;
}

/*
 * Output example:
 *
 * usr_accounting:
 * - id:      0
 *   usage:   { inodes:                  209, kbytes:             2616 }
 * - id:      840000038
 *   usage:   { inodes:                    1, kbytes:             1048 }
 */
static int lprocfs_quota_seq_show(struct seq_file *p, void *v)
{
	struct lquota_procfs		*lqp = p->private;
	struct lquota_thread_info	*qti = lquota_info(&lqp->lqp_env);
	const struct dt_it_ops		*iops;
	struct dt_it			*it;
	struct dt_key			*key;
	struct dt_rec			*rec = (struct dt_rec *)&qti->qti_rec;
	const struct lu_fid		*fid;
	int				 rc;

	LASSERT(lqp);
	if (lqp->lqp_obj == NULL) {
		seq_printf(p, "not supported\n");
		return 0;
	}

	fid = lu_object_fid(&lqp->lqp_obj->do_lu);

	if (v == SEQ_START_TOKEN) {
		if (fid_is_acct(fid)) {
			if (fid_oid(fid) == ACCT_USER_OID)
				seq_printf(p, "usr_accounting:\n");
			else
				seq_printf(p, "grp_accounting:\n");
		} else if (fid_seq(fid) == FID_SEQ_QUOTA_GLB) {
			int	poolid, rtype, qtype;

			rc = lquota_extract_fid(fid, &poolid, &rtype, &qtype);
			if (rc)
				return rc;

			seq_printf(p, "global_pool%d_%s_%s\n", poolid,
				   RES_NAME(rtype), QTYPE_NAME(qtype));
		} else if (fid_seq(fid) == FID_SEQ_LOCAL_NAME) {
			/* global index copy object */
			seq_printf(p, "global_index_copy:\n");
		} else {
			return -ENOTSUPP;
		}
		return 0;
	}

	iops = &lqp->lqp_obj->do_index_ops->dio_it;
	it = (struct dt_it *)v;

	key = iops->key(&lqp->lqp_env, it);
	if (IS_ERR(key)) {
		CERROR("%s: failed to get key: rc = %ld\n",
		       lqp->lqp_obj->do_lu.lo_dev->ld_obd->obd_name,
		       PTR_ERR(key));
		return PTR_ERR(key);
	}

	rc = iops->rec(&lqp->lqp_env, it, rec, 0);
	if (rc) {
		CERROR("%s: failed to get rec: rc = %d\n",
		       lqp->lqp_obj->do_lu.lo_dev->ld_obd->obd_name, rc);
		return rc;
	}

	seq_printf(p, "- %-8s %llu\n", "id:", *((__u64 *)key));
	if (fid_is_acct(fid))
		seq_printf(p, "  %-8s { inodes: %20"LPF64"u, kbytes: %20"LPF64
			   "u }\n", "usage:",
			   ((struct lquota_acct_rec *)rec)->ispace,
			   toqb(((struct lquota_acct_rec *)rec)->bspace));
	else if (fid_seq(fid) == FID_SEQ_QUOTA_GLB ||
		 fid_seq(fid) == FID_SEQ_LOCAL_NAME)
		seq_printf(p, "  %-8s { hard: %20"LPF64"u, soft: %20"LPF64
			   "u, granted: %20"LPF64"u, time: %20"LPF64"u }\n",
			   "limits:",
			   ((struct lquota_glb_rec *)rec)->qbr_hardlimit,
			   ((struct lquota_glb_rec *)rec)->qbr_softlimit,
			   ((struct lquota_glb_rec *)rec)->qbr_granted,
			   ((struct lquota_glb_rec *)rec)->qbr_time);
	return 0;
}

struct seq_operations lprocfs_quota_seq_sops = {
	.start	= lprocfs_quota_seq_start,
	.stop	= lprocfs_quota_seq_stop,
	.next	= lprocfs_quota_seq_next,
	.show	= lprocfs_quota_seq_show,
};

static int lprocfs_quota_seq_open(struct inode *inode, struct file *file)
{
	struct proc_dir_entry	*dp = PDE(inode);
	struct seq_file		*seq;
	int			 rc;
	struct lquota_procfs	*lqp;

	/* Allocate quota procfs data. This structure will be passed to
	 * seq_start/stop/next/show via seq->private */
	OBD_ALLOC_PTR(lqp);
	if (lqp == NULL)
		return -ENOMEM;

	/* store pointer to object we would like to iterate over */
	lqp->lqp_obj = (struct dt_object *)dp->data;

	/* Initialize the common environment to be used in the seq operations */
	rc = lu_env_init(&lqp->lqp_env, LCT_LOCAL);
	if (rc) {
		char *obd_name = "quota";

		if (lqp->lqp_obj != NULL)
			obd_name = lqp->lqp_obj->do_lu.lo_dev->ld_obd->obd_name;

		CERROR("%s: error initializing procfs quota env: rc = %d\n",
		       obd_name, rc);
		goto out_lqp;
	}

	rc = LPROCFS_ENTRY_CHECK(inode);
	if (rc < 0)
		goto out_env;

	rc = seq_open(file, &lprocfs_quota_seq_sops);
	if (rc)
		goto out_env;

	seq = file->private_data;
	seq->private = lqp;
	return 0;

out_env:
	lu_env_fini(&lqp->lqp_env);
out_lqp:
	OBD_FREE_PTR(lqp);
	return rc;
}

static int lprocfs_quota_seq_release(struct inode *inode, struct file *file)
{
	struct seq_file		*seq = file->private_data;
	struct lquota_procfs	*lqp = seq->private;

	LASSERT(lqp);
	lu_env_fini(&lqp->lqp_env);
	OBD_FREE_PTR(lqp);

	return seq_release(inode, file);
}

struct file_operations lprocfs_quota_seq_fops = {
	.owner		= THIS_MODULE,
	.open		= lprocfs_quota_seq_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= lprocfs_quota_seq_release,
};
#endif  /* LPROCFS */
