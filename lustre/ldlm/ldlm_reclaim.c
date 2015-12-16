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
 * Copyright (c) 2015, Intel Corporation.
 * Use is subject to license terms.
 *
 * Author: Niu    Yawei    <yawei.niu@intel.com>
 */

#define DEBUG_SUBSYSTEM S_LDLM

#ifndef __KERNEL__
# include <liblustre.h>
#else
# include <linux/kthread.h>
#endif

#include <lustre_dlm.h>
#include <obd_class.h>
#include "ldlm_internal.h"

/*
 * To avoid ldlm lock exhausting server memory, two global parameters:
 * ldlm_reclaim_threshold & ldlm_lock_limit are used for reclaiming
 * granted locks and rejecting incoming enqueue requests defensively.
 *
 * ldlm_reclaim_threshold: When the amount of granted locks reaching this
 * threshold, server start to revoke locks gradually.
 *
 * ldlm_lock_limit: When the amount of granted locks reaching this
 * threshold, server will return -EINPROGRESS to any incoming enqueue
 * request until the lock count is shrunk below the threshold again.
 *
 * ldlm_reclaim_threshold & ldlm_lock_limit is set to 20% & 30% of the
 * total memory by default. It is tunable via proc entry, when it's set
 * to 0, the feature is disabled.
 */

#ifdef HAVE_SERVER_SUPPORT

/* Lock count is stored in ldlm_reclaim_threshold & ldlm_lock_limit */
__u64 ldlm_reclaim_threshold;
__u64 ldlm_lock_limit;

/* Represents ldlm_reclaim_threshold & ldlm_lock_limit in MB, used for
 * proc interface. */
__u64 ldlm_reclaim_threshold_mb;
__u64 ldlm_lock_limit_mb;

#ifdef __KERNEL__
struct percpu_counter		ldlm_granted_total;
#endif
static atomic_t			ldlm_nr_reclaimer;
static cfs_duration_t		ldlm_last_reclaim_age;
static cfs_time_t		ldlm_last_reclaim_time;

struct ldlm_reclaim_cb_data {
	cfs_list_t		 rcd_rpc_list;
	int			 rcd_added;
	int			 rcd_total;
	int			 rcd_cursor;
	int			 rcd_start;
	bool			 rcd_skip;
	cfs_duration_t		 rcd_age;
	struct cfs_hash_bd	*rcd_prev_bd;
};

static inline bool ldlm_lock_reclaimable(struct ldlm_lock *lock)
{
	struct ldlm_namespace *ns = ldlm_lock_to_ns(lock);

	/* FLOCK & PLAIN lock are not reclaimable. FLOCK is
	 * explicitly controlled by application, PLAIN lock
	 * is used by quota global lock and config lock.
	 */
	if (ns->ns_client == LDLM_NAMESPACE_SERVER &&
	    (lock->l_resource->lr_type == LDLM_IBITS ||
	     lock->l_resource->lr_type == LDLM_EXTENT))
		return true;
	return false;
}

/**
 * Callback function for revoking locks from certain resource.
 *
 * \param [in] hs	ns_rs_hash
 * \param [in] bd	current bucket of ns_rsh_hash
 * \param [in] hnode	hnode of the resource
 * \param [in] arg	opaque data
 *
 * \retval 0		continue the scan
 * \retval 1		stop the iteration
 */
static int ldlm_reclaim_lock_cb(struct cfs_hash *hs, struct cfs_hash_bd *bd,
				cfs_hlist_node_t *hnode, void *arg)

{
	struct ldlm_resource		*res;
	struct ldlm_reclaim_cb_data	*data;
	struct ldlm_lock		*lock;
	struct ldlm_ns_bucket		*nsb;
	int				 rc = 0;

	data = (struct ldlm_reclaim_cb_data *)arg;

	LASSERTF(data->rcd_added < data->rcd_total, "added:%d >= total:%d\n",
		 data->rcd_added, data->rcd_total);

	nsb = cfs_hash_bd_extra_get(hs, bd);
	res = cfs_hash_object(hs, hnode);

	if (data->rcd_prev_bd != bd) {
		if (data->rcd_prev_bd != NULL)
			ldlm_res_to_ns(res)->ns_reclaim_start++;
		data->rcd_prev_bd = bd;
		data->rcd_cursor = 0;
		data->rcd_start = nsb->nsb_reclaim_start %
				  cfs_hash_bd_count_get(bd);
	}

	if (data->rcd_skip && data->rcd_cursor < data->rcd_start) {
		data->rcd_cursor++;
		return 0;
	}

	nsb->nsb_reclaim_start++;

	lock_res(res);
	cfs_list_for_each_entry(lock, &res->lr_granted, l_res_link) {
		if (!ldlm_lock_reclaimable(lock))
			continue;

		if (!OBD_FAIL_CHECK(OBD_FAIL_LDLM_WATERMARK_LOW) &&
		    cfs_time_before(cfs_time_current(),
				    cfs_time_add(lock->l_last_used,
						 data->rcd_age)))
			continue;

		if (!ldlm_is_ast_sent(lock)) {
			ldlm_set_ast_sent(lock);
			LASSERT(cfs_list_empty(&lock->l_rk_ast));
			cfs_list_add(&lock->l_rk_ast, &data->rcd_rpc_list);
			LDLM_LOCK_GET(lock);
			if (++data->rcd_added == data->rcd_total) {
				rc = 1; /* stop the iteration */
				break;
			}
		}
	}
	unlock_res(res);

	return rc;
}

/**
 * Revoke locks from the resources of a namespace in a roundrobin
 * manner.
 *
 * \param[in] ns	namespace to do the lock revoke on
 * \param[in] count	count of lock to be revoked
 * \param[in] age	only revoke locks older than the 'age'
 * \param[in] skip	scan from the first lock on resource if the
 *			'skip' is false, otherwise, continue scan
 *			from the last scanned position
 * \param[out] count	count of lock still to be revoked
 */
static void ldlm_reclaim_res(struct ldlm_namespace *ns, int *count,
			     cfs_duration_t age, bool skip)
{
	struct ldlm_reclaim_cb_data	data;
	int				start;
	ENTRY;

	LASSERT(*count != 0);

#ifdef __KERNEL__
	if (ns->ns_obd) {
		int idx, type;
		type = server_name2index(ns->ns_obd->obd_name, &idx, NULL);
		if (type != LDD_F_SV_TYPE_MDT && type != LDD_F_SV_TYPE_OST) {
			EXIT;
			return;
		}
	}
#endif

	if (atomic_read(&ns->ns_bref) == 0) {
		EXIT;
		return;
	}

	CFS_INIT_LIST_HEAD(&data.rcd_rpc_list);
	data.rcd_added = 0;
	data.rcd_total = *count;
	data.rcd_age = age;
	data.rcd_skip = skip;
	data.rcd_prev_bd = NULL;
	start = ns->ns_reclaim_start % CFS_HASH_NBKT(ns->ns_rs_hash);

	cfs_hash_for_each_nolock(ns->ns_rs_hash, ldlm_reclaim_lock_cb, &data,
				 start);

	CDEBUG(D_DLMTRACE, "NS(%s): %d locks to be reclaimed, found %d/%d "
	       "locks.\n", ldlm_ns_name(ns), *count, data.rcd_added,
	       data.rcd_total);

	LASSERTF(*count >= data.rcd_added, "count:%d, added:%d\n", *count,
		 data.rcd_added);

	ldlm_run_ast_work(ns, &data.rcd_rpc_list, LDLM_WORK_REVOKE_AST);
	*count -= data.rcd_added;
	EXIT;
}

#define LDLM_RECLAIM_BATCH	512
#define LDLM_RECLAIM_AGE_MIN	cfs_time_seconds(300)
#define LDLM_RECLAIM_AGE_MAX	(LDLM_DEFAULT_MAX_ALIVE * 3 / 4)

static inline cfs_duration_t ldlm_reclaim_age(void)
{
	cfs_duration_t	age;

	age = ldlm_last_reclaim_age +
		cfs_time_sub(cfs_time_current(), ldlm_last_reclaim_time);
	if (age > LDLM_RECLAIM_AGE_MAX)
		age = LDLM_RECLAIM_AGE_MAX;
	else if (age < (LDLM_RECLAIM_AGE_MIN * 2))
		age = LDLM_RECLAIM_AGE_MIN;
	return age;
}

/**
 * Revoke certain amount of locks from all the server namespaces
 * in a roundrobin manner. Lock age is used to avoid reclaim on
 * the non-aged locks.
 */
static void ldlm_reclaim_ns(void)
{
	struct ldlm_namespace	*ns;
	int			 count = LDLM_RECLAIM_BATCH;
	int			 ns_nr, nr_processed;
	ldlm_side_t		 ns_cli = LDLM_NAMESPACE_SERVER;
	cfs_duration_t		 age;
	bool			 skip = true;
	ENTRY;

	if (!atomic_add_unless(&ldlm_nr_reclaimer, 1, 1)) {
		EXIT;
		return;
	}

	age = ldlm_reclaim_age();
again:
	nr_processed = 0;
	ns_nr = ldlm_namespace_nr_read(ns_cli);
	while (count > 0 && nr_processed < ns_nr) {
		mutex_lock(ldlm_namespace_lock(ns_cli));

		if (cfs_list_empty(ldlm_namespace_list(ns_cli))) {
			mutex_unlock(ldlm_namespace_lock(ns_cli));
			goto out;
		}

		ns = ldlm_namespace_first_locked(ns_cli);
		ldlm_namespace_move_to_active_locked(ns, ns_cli);
		mutex_unlock(ldlm_namespace_lock(ns_cli));

		ldlm_reclaim_res(ns, &count, age, skip);
		ldlm_namespace_put(ns);
		nr_processed++;
	}

	if (count > 0 && age > LDLM_RECLAIM_AGE_MIN) {
		age >>= 1;
		if (age < (LDLM_RECLAIM_AGE_MIN * 2))
			age = LDLM_RECLAIM_AGE_MIN;
		skip = false;
		goto again;
	}

	ldlm_last_reclaim_age = age;
	ldlm_last_reclaim_time = cfs_time_current();
out:
	atomic_add_unless(&ldlm_nr_reclaimer, -1, 0);
	EXIT;
}

void ldlm_reclaim_add(struct ldlm_lock *lock)
{
	if (!ldlm_lock_reclaimable(lock))
		return;
#ifdef __KERNEL__
	percpu_counter_add(&ldlm_granted_total, 1);
#endif
	lock->l_last_used = cfs_time_current();
}

void ldlm_reclaim_del(struct ldlm_lock *lock)
{
	if (!ldlm_lock_reclaimable(lock))
		return;
#ifdef __KERNEL__
	percpu_counter_sub(&ldlm_granted_total, 1);
#endif
}

/**
 * Check on the total granted locks: return true if it reaches the
 * high watermark (ldlm_lock_limit), otherwise return false; It also
 * triggers lock reclaim if the low watermark (ldlm_reclaim_threshold)
 * is reached.
 *
 * \retval true		high watermark reached.
 * \retval false	high watermark not reached.
 */
bool ldlm_reclaim_full(void)
{
	__u64 high = ldlm_lock_limit;
	__u64 low = ldlm_reclaim_threshold;

	if (low != 0 && OBD_FAIL_CHECK(OBD_FAIL_LDLM_WATERMARK_LOW))
		low = cfs_fail_val;
#ifdef __KERNEL__
	if (low != 0 &&
	    percpu_counter_sum_positive(&ldlm_granted_total) > low)
#else
	if (0)
#endif
		ldlm_reclaim_ns();

	if (high != 0 && OBD_FAIL_CHECK(OBD_FAIL_LDLM_WATERMARK_HIGH))
		high = cfs_fail_val;
#ifdef __KERNEL__
	if (high != 0 &&
	    percpu_counter_sum_positive(&ldlm_granted_total) > high)
#else
	if (0)
#endif
		return true;

	return false;
}

static inline __u64 ldlm_ratio2locknr(int ratio)
{
	__u64 locknr;

	locknr = ((__u64)NUM_CACHEPAGES << PAGE_CACHE_SHIFT) * ratio;
	do_div(locknr, 100 * sizeof(struct ldlm_lock));

	return locknr;
}

static inline __u64 ldlm_locknr2mb(__u64 locknr)
{
	return (locknr * sizeof(struct ldlm_lock) + 512 * 1024) >> 20;
}

#define LDLM_WM_RATIO_LOW_DEFAULT	20
#define LDLM_WM_RATIO_HIGH_DEFAULT	30

int ldlm_reclaim_setup(void)
{
	atomic_set(&ldlm_nr_reclaimer, 0);

	ldlm_reclaim_threshold = ldlm_ratio2locknr(LDLM_WM_RATIO_LOW_DEFAULT);
	ldlm_reclaim_threshold_mb = ldlm_locknr2mb(ldlm_reclaim_threshold);
	ldlm_lock_limit = ldlm_ratio2locknr(LDLM_WM_RATIO_HIGH_DEFAULT);
	ldlm_lock_limit_mb = ldlm_locknr2mb(ldlm_lock_limit);

	ldlm_last_reclaim_age = LDLM_RECLAIM_AGE_MAX;
	ldlm_last_reclaim_time = cfs_time_current();

#ifdef __KERNEL__
	return percpu_counter_init(&ldlm_granted_total, 0);
#else
	return 0;
#endif
}

void ldlm_reclaim_cleanup(void)
{
#ifdef __KERNEL__
	percpu_counter_destroy(&ldlm_granted_total);
#endif
}

#else /* HAVE_SERVER_SUPPORT */

bool ldlm_reclaim_full(void)
{
	return false;
}

void ldlm_reclaim_add(struct ldlm_lock *lock)
{
}

void ldlm_reclaim_del(struct ldlm_lock *lock)
{
}

int ldlm_reclaim_setup(void)
{
	return 0;
}

void ldlm_reclaim_cleanup(void)
{
}

#endif /* HAVE_SERVER_SUPPORT */
