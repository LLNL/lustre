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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_LQUOTA

#include <linux/version.h>
#include <lprocfs_status.h>
#include <obd.h>
#include <linux/seq_file.h>
#include <lustre_fsfilt.h>

#include "lquota_internal.h"
#include "quota_internal.h"

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
 * user_accounting:
 * - id:      0
 *   usage:   { inodes:                  209, bytes:             26161152 }
 * - id:      840000038
 *   usage:   { inodes:                    1, bytes:             10485760 }
 */
static int lprocfs_quota_seq_show(struct seq_file *p, void *v)
{
	struct lquota_procfs *lqp = p->private;
	const struct dt_it_ops *iops;
	struct dt_it           *it;
	struct dt_key          *key;
	struct acct_rec         rec;
	int                     rc;

	LASSERT(lqp);
	if (lqp->lqp_obj == NULL) {
		seq_printf(p, "not supported\n");
		return 0;
	}

	if (v == SEQ_START_TOKEN) {
		const struct lu_fid *fid = lu_object_fid(&lqp->lqp_obj->do_lu);

		LASSERT(fid_is_acct(fid));
		if (fid_oid(fid) == ACCT_USER_OID)
			seq_printf(p, "user_accounting:\n");
		else
			seq_printf(p, "group_accounting:\n");
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

	rc = iops->rec(&lqp->lqp_env, it, (struct dt_rec *)&rec, 0);
	if (rc) {
		CERROR("%s: failed to get rec: rc = %d\n",
		       lqp->lqp_obj->do_lu.lo_dev->ld_obd->obd_name, rc);
		return rc;
	}

	seq_printf(p, "- %-8s %llu\n", "id:", *((__u64 *)key));
	seq_printf(p, "  %-8s { inodes: %20"LPF64"u, bytes: %20"LPF64"u }\n",
		   "usage:", rec.ispace, rec.bspace);
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
		CERROR("%s: error initializing procfs quota env: rc = %d\n",
		       lqp->lqp_obj->do_lu.lo_dev->ld_obd->obd_name, rc);
		goto out_lqp;
	}

	if (LPROCFS_ENTRY_AND_CHECK(dp)) {
		rc = -ENOENT;
		goto out_env;
	}

	rc = seq_open(file, &lprocfs_quota_seq_sops);
	if (rc)
		goto out_lprocfs;

	seq = file->private_data;
	seq->private = lqp;
	return 0;

out_lprocfs:
	LPROCFS_EXIT();
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

	LPROCFS_EXIT();

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

int lprocfs_quota_rd_type_dumb(char *page, char **start, off_t off, int count,
			       int *eof, void *data)
{
	return 0;
}
EXPORT_SYMBOL(lprocfs_quota_rd_type_dumb);

int lprocfs_quota_wr_type_dumb(struct file *file, const char *buffer,
			       unsigned long count, void *data)
{
	return count;
}
EXPORT_SYMBOL(lprocfs_quota_wr_type_dumb);
#endif  /* LPROCFS */
