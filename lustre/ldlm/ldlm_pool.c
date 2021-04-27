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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2010, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ldlm/ldlm_pool.c
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 */

/*
 * Idea of this code is rather simple. Each second, for each server namespace
 * we have SLV - server lock volume which is calculated on current number of
 * granted locks, grant speed for past period, etc - that is, locking load.
 * This SLV number may be thought as a flow definition for simplicity. It is
 * sent to clients with each occasion to let them know what is current load
 * situation on the server. By default, at the beginning, SLV on server is
 * set max value which is calculated as the following: allow to one client
 * have all locks of limit ->pl_limit for 10h.
 *
 * Next, on clients, number of cached locks is not limited artificially in any
 * way as it was before. Instead, client calculates CLV, that is, client lock
 * volume for each lock and compares it with last SLV from the server. CLV is
 * calculated as the number of locks in LRU * lock live time in seconds. If
 * CLV > SLV - lock is canceled.
 *
 * Client has LVF, that is, lock volume factor which regulates how much sensitive
 * client should be about last SLV from server. The higher LVF is the more locks
 * will be canceled on client. Default value for it is 1. Setting LVF to 2 means
 * that client will cancel locks 2 times faster.
 *
 * Locks on a client will be canceled more intensively in these cases:
 * (1) if SLV is smaller, that is, load is higher on the server;
 * (2) client has a lot of locks (the more locks are held by client, the bigger
 *     chances that some of them should be canceled);
 * (3) client has old locks (taken some time ago);
 *
 * Thus, according to flow paradigm that we use for better understanding SLV,
 * CLV is the volume of particle in flow described by SLV. According to this,
 * if flow is getting thinner, more and more particles become outside of it and
 * as particles are locks, they should be canceled.
 *
 * General idea of this belongs to Vitaly Fertman (vitaly@clusterfs.com). Andreas
 * Dilger (adilger@clusterfs.com) proposed few nice ideas like using LVF and many
 * cleanups. Flow definition to allow more easy understanding of the logic belongs
 * to Nikita Danilov (nikita@clusterfs.com) as well as many cleanups and fixes.
 * And design and implementation are done by Yury Umanets (umka@clusterfs.com).
 *
 * Glossary for terms used:
 *
 * pl_limit - Number of allowed locks in pool. Applies to server and client
 * side (tunable);
 *
 * pl_granted - Number of granted locks (calculated);
 * pl_grant_rate - Number of granted locks for last T (calculated);
 * pl_cancel_rate - Number of canceled locks for last T (calculated);
 * pl_grant_speed - Grant speed (GR - CR) for last T (calculated);
 * pl_grant_plan - Planned number of granted locks for next T (calculated);
 * pl_server_lock_volume - Current server lock volume (calculated);
 *
 * As it may be seen from list above, we have few possible tunables which may
 * affect behavior much. They all may be modified via sysfs. However, they also
 * give a possibility for constructing few pre-defined behavior policies. If
 * none of predefines is suitable for a working pattern being used, new one may
 * be "constructed" via sysfs tunables.
 */

#define DEBUG_SUBSYSTEM S_LDLM

#include <linux/workqueue.h>
#include <libcfs/linux/linux-mem.h>
#include <lustre_dlm.h>
#include <cl_object.h>
#include <obd_class.h>
#include <obd_support.h>
#include "ldlm_internal.h"

#ifdef HAVE_LRU_RESIZE_SUPPORT

/*
 * 50 ldlm locks for 1MB of RAM.
 */
#define LDLM_POOL_HOST_L ((NUM_CACHEPAGES >> (20 - PAGE_SHIFT)) * 50)

/*
 * Maximal possible grant step plan in %.
 */
#define LDLM_POOL_MAX_GSP (30)

/*
 * Minimal possible grant step plan in %.
 */
#define LDLM_POOL_MIN_GSP (1)

/*
 * This controls the speed of reaching LDLM_POOL_MAX_GSP
 * with increasing thread period.
 */
#define LDLM_POOL_GSP_STEP_SHIFT (2)

/*
 * LDLM_POOL_GSP% of all locks is default GP.
 */
#define LDLM_POOL_GP(L)   (((L) * LDLM_POOL_MAX_GSP) / 100)

/*
 * Max age for locks on clients.
 */
#define LDLM_POOL_MAX_AGE (36000)

/*
 * The granularity of SLV calculation.
 */
#define LDLM_POOL_SLV_SHIFT (10)

extern struct proc_dir_entry *ldlm_ns_proc_dir;

static inline __u64 dru(__u64 val, __u32 shift, int round_up)
{
        return (val + (round_up ? (1 << shift) - 1 : 0)) >> shift;
}

static inline __u64 ldlm_pool_slv_max(__u32 L)
{
        /*
         * Allow to have all locks for 1 client for 10 hrs.
         * Formula is the following: limit * 10h / 1 client.
         */
        __u64 lim = (__u64)L *  LDLM_POOL_MAX_AGE / 1;
        return lim;
}

static inline __u64 ldlm_pool_slv_min(__u32 L)
{
        return 1;
}

enum {
        LDLM_POOL_FIRST_STAT = 0,
        LDLM_POOL_GRANTED_STAT = LDLM_POOL_FIRST_STAT,
        LDLM_POOL_GRANT_STAT,
        LDLM_POOL_CANCEL_STAT,
        LDLM_POOL_GRANT_RATE_STAT,
        LDLM_POOL_CANCEL_RATE_STAT,
        LDLM_POOL_GRANT_PLAN_STAT,
        LDLM_POOL_SLV_STAT,
        LDLM_POOL_SHRINK_REQTD_STAT,
        LDLM_POOL_SHRINK_FREED_STAT,
        LDLM_POOL_RECALC_STAT,
        LDLM_POOL_TIMING_STAT,
        LDLM_POOL_LAST_STAT
};

static inline struct ldlm_namespace *ldlm_pl2ns(struct ldlm_pool *pl)
{
        return container_of(pl, struct ldlm_namespace, ns_pool);
}

/**
 * Calculates suggested grant_step in % of available locks for passed
 * \a period. This is later used in grant_plan calculations.
 */
static inline int ldlm_pool_t2gsp(unsigned int t)
{
        /*
         * This yields 1% grant step for anything below LDLM_POOL_GSP_STEP
         * and up to 30% for anything higher than LDLM_POOL_GSP_STEP.
         *
         * How this will affect execution is the following:
         *
         * - for thread period 1s we will have grant_step 1% which good from
         * pov of taking some load off from server and push it out to clients.
         * This is like that because 1% for grant_step means that server will
         * not allow clients to get lots of locks in short period of time and
         * keep all old locks in their caches. Clients will always have to
         * get some locks back if they want to take some new;
         *
         * - for thread period 10s (which is default) we will have 23% which
         * means that clients will have enough of room to take some new locks
         * without getting some back. All locks from this 23% which were not
         * taken by clients in current period will contribute in SLV growing.
         * SLV growing means more locks cached on clients until limit or grant
         * plan is reached.
         */
        return LDLM_POOL_MAX_GSP -
                ((LDLM_POOL_MAX_GSP - LDLM_POOL_MIN_GSP) >>
                 (t >> LDLM_POOL_GSP_STEP_SHIFT));
}

static inline int ldlm_pool_granted(struct ldlm_pool *pl)
{
	return atomic_read(&pl->pl_granted);
}

/**
 * Recalculates next grant limit on passed \a pl.
 *
 * \pre ->pl_lock is locked.
 */
static void ldlm_pool_recalc_grant_plan(struct ldlm_pool *pl)
{
	int granted, grant_step, limit;

	limit = ldlm_pool_get_limit(pl);
	granted = ldlm_pool_granted(pl);

	grant_step = ldlm_pool_t2gsp(pl->pl_recalc_period);
	grant_step = ((limit - granted) * grant_step) / 100;
	pl->pl_grant_plan = granted + grant_step;
	limit = (limit * 5) >> 2;
	if (pl->pl_grant_plan > limit)
		pl->pl_grant_plan = limit;
}

/**
 * Recalculates next SLV on passed \a pl.
 *
 * \pre ->pl_lock is locked.
 */
static void ldlm_pool_recalc_slv(struct ldlm_pool *pl)
{
        int granted;
        int grant_plan;
        int round_up;
        __u64 slv;
        __u64 slv_factor;
        __u64 grant_usage;
        __u32 limit;

	slv = pl->pl_server_lock_volume;
	grant_plan = pl->pl_grant_plan;
	limit = ldlm_pool_get_limit(pl);
	granted = ldlm_pool_granted(pl);
	round_up = granted < limit;

        grant_usage = max_t(int, limit - (granted - grant_plan), 1);

        /*
         * Find out SLV change factor which is the ratio of grant usage
         * from limit. SLV changes as fast as the ratio of grant plan
         * consumption. The more locks from grant plan are not consumed
         * by clients in last interval (idle time), the faster grows
         * SLV. And the opposite, the more grant plan is over-consumed
         * (load time) the faster drops SLV.
         */
        slv_factor = (grant_usage << LDLM_POOL_SLV_SHIFT);
        do_div(slv_factor, limit);
        slv = slv * slv_factor;
        slv = dru(slv, LDLM_POOL_SLV_SHIFT, round_up);

        if (slv > ldlm_pool_slv_max(limit)) {
                slv = ldlm_pool_slv_max(limit);
        } else if (slv < ldlm_pool_slv_min(limit)) {
                slv = ldlm_pool_slv_min(limit);
        }

        pl->pl_server_lock_volume = slv;
}

/**
 * Recalculates next stats on passed \a pl.
 *
 * \pre ->pl_lock is locked.
 */
static void ldlm_pool_recalc_stats(struct ldlm_pool *pl)
{
	int grant_plan = pl->pl_grant_plan;
	__u64 slv = pl->pl_server_lock_volume;
	int granted = ldlm_pool_granted(pl);
	int grant_rate = atomic_read(&pl->pl_grant_rate);
	int cancel_rate = atomic_read(&pl->pl_cancel_rate);

	lprocfs_counter_add(pl->pl_stats, LDLM_POOL_SLV_STAT,
			    slv);
	lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANTED_STAT,
			    granted);
	lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANT_RATE_STAT,
			    grant_rate);
	lprocfs_counter_add(pl->pl_stats, LDLM_POOL_GRANT_PLAN_STAT,
			    grant_plan);
	lprocfs_counter_add(pl->pl_stats, LDLM_POOL_CANCEL_RATE_STAT,
			    cancel_rate);
}

/**
 * Sets current SLV into obd accessible via ldlm_pl2ns(pl)->ns_obd.
 */
static void ldlm_srv_pool_push_slv(struct ldlm_pool *pl)
{
        struct obd_device *obd;

        /*
         * Set new SLV in obd field for using it later without accessing the
         * pool. This is required to avoid race between sending reply to client
         * with new SLV and cleanup server stack in which we can't guarantee
         * that namespace is still alive. We know only that obd is alive as
         * long as valid export is alive.
         */
        obd = ldlm_pl2ns(pl)->ns_obd;
        LASSERT(obd != NULL);
	write_lock(&obd->obd_pool_lock);
        obd->obd_pool_slv = pl->pl_server_lock_volume;
	write_unlock(&obd->obd_pool_lock);
}

/**
 * Recalculates all pool fields on passed \a pl.
 *
 * \pre ->pl_lock is not locked.
 */
static int ldlm_srv_pool_recalc(struct ldlm_pool *pl)
{
	time64_t recalc_interval_sec;
        ENTRY;

	recalc_interval_sec = ktime_get_real_seconds() - pl->pl_recalc_time;
        if (recalc_interval_sec < pl->pl_recalc_period)
                RETURN(0);

	spin_lock(&pl->pl_lock);
	recalc_interval_sec = ktime_get_real_seconds() - pl->pl_recalc_time;
	if (recalc_interval_sec < pl->pl_recalc_period) {
		spin_unlock(&pl->pl_lock);
		RETURN(0);
	}
        /*
         * Recalc SLV after last period. This should be done
         * _before_ recalculating new grant plan.
         */
        ldlm_pool_recalc_slv(pl);

        /*
         * Make sure that pool informed obd of last SLV changes.
         */
        ldlm_srv_pool_push_slv(pl);

        /*
         * Update grant_plan for new period.
         */
        ldlm_pool_recalc_grant_plan(pl);

	pl->pl_recalc_time = ktime_get_real_seconds();
        lprocfs_counter_add(pl->pl_stats, LDLM_POOL_TIMING_STAT,
                            recalc_interval_sec);
	spin_unlock(&pl->pl_lock);
	RETURN(0);
}

/**
 * This function is used on server side as main entry point for memory
 * pressure handling. It decreases SLV on \a pl according to passed
 * \a nr and \a gfp_mask.
 *
 * Our goal here is to decrease SLV such a way that clients hold \a nr
 * locks smaller in next 10h.
 */
static int ldlm_srv_pool_shrink(struct ldlm_pool *pl,
				int nr,  gfp_t gfp_mask)
{
	__u32 limit;

	/*
	 * VM is asking how many entries may be potentially freed.
	 */
	if (nr == 0)
		return ldlm_pool_granted(pl);

	/*
	 * Client already canceled locks but server is already in shrinker
	 * and can't cancel anything. Let's catch this race.
	 */
	if (ldlm_pool_granted(pl) == 0)
		RETURN(0);

	spin_lock(&pl->pl_lock);

        /*
         * We want shrinker to possibly cause cancellation of @nr locks from
         * clients or grant approximately @nr locks smaller next intervals.
         *
         * This is why we decreased SLV by @nr. This effect will only be as
         * long as one re-calc interval (1s these days) and this should be
         * enough to pass this decreased SLV to all clients. On next recalc
         * interval pool will either increase SLV if locks load is not high
         * or will keep on same level or even decrease again, thus, shrinker
         * decreased SLV will affect next recalc intervals and this way will
         * make locking load lower.
         */
        if (nr < pl->pl_server_lock_volume) {
                pl->pl_server_lock_volume = pl->pl_server_lock_volume - nr;
        } else {
                limit = ldlm_pool_get_limit(pl);
                pl->pl_server_lock_volume = ldlm_pool_slv_min(limit);
        }

        /*
         * Make sure that pool informed obd of last SLV changes.
         */
        ldlm_srv_pool_push_slv(pl);
	spin_unlock(&pl->pl_lock);

	/*
	 * We did not really free any memory here so far, it only will be
	 * freed later may be, so that we return 0 to not confuse VM.
	 */
	return 0;
}

/**
 * Setup server side pool \a pl with passed \a limit.
 */
static int ldlm_srv_pool_setup(struct ldlm_pool *pl, int limit)
{
        struct obd_device *obd;

        obd = ldlm_pl2ns(pl)->ns_obd;
        LASSERT(obd != NULL && obd != LP_POISON);
        LASSERT(obd->obd_type != LP_POISON);
	write_lock(&obd->obd_pool_lock);
        obd->obd_pool_limit = limit;
	write_unlock(&obd->obd_pool_lock);

        ldlm_pool_set_limit(pl, limit);
        return 0;
}

/**
 * Sets SLV and Limit from ldlm_pl2ns(pl)->ns_obd tp passed \a pl.
 */
static void ldlm_cli_pool_pop_slv(struct ldlm_pool *pl)
{
        struct obd_device *obd;

        /*
         * Get new SLV and Limit from obd which is updated with coming
         * RPCs.
         */
        obd = ldlm_pl2ns(pl)->ns_obd;
        LASSERT(obd != NULL);
	read_lock(&obd->obd_pool_lock);
        pl->pl_server_lock_volume = obd->obd_pool_slv;
        ldlm_pool_set_limit(pl, obd->obd_pool_limit);
	read_unlock(&obd->obd_pool_lock);
}

/**
 * Recalculates client size pool \a pl according to current SLV and Limit.
 */
static int ldlm_cli_pool_recalc(struct ldlm_pool *pl)
{
	time64_t recalc_interval_sec;
	int ret;
        ENTRY;

	recalc_interval_sec = ktime_get_real_seconds() - pl->pl_recalc_time;
        if (recalc_interval_sec < pl->pl_recalc_period)
                RETURN(0);

	spin_lock(&pl->pl_lock);
	/*
	 * Check if we need to recalc lists now.
	 */
	recalc_interval_sec = ktime_get_real_seconds() - pl->pl_recalc_time;
	if (recalc_interval_sec < pl->pl_recalc_period) {
		spin_unlock(&pl->pl_lock);
                RETURN(0);
        }

        /*
         * Make sure that pool knows last SLV and Limit from obd.
         */
        ldlm_cli_pool_pop_slv(pl);
	spin_unlock(&pl->pl_lock);

        /*
         * In the time of canceling locks on client we do not need to maintain
         * sharp timing, we only want to cancel locks asap according to new SLV.
         * It may be called when SLV has changed much, this is why we do not
         * take into account pl->pl_recalc_time here.
         */
	ret = ldlm_cancel_lru(ldlm_pl2ns(pl), 0, LCF_ASYNC, 0);

	spin_lock(&pl->pl_lock);
	/*
	 * Time of LRU resizing might be longer than period,
	 * so update after LRU resizing rather than before it.
	 */
	pl->pl_recalc_time = ktime_get_real_seconds();
	lprocfs_counter_add(pl->pl_stats, LDLM_POOL_TIMING_STAT,
			    recalc_interval_sec);
	spin_unlock(&pl->pl_lock);
	RETURN(ret);
}

/**
 * This function is main entry point for memory pressure handling on client
 * side.  Main goal of this function is to cancel some number of locks on
 * passed \a pl according to \a nr and \a gfp_mask.
 */
static int ldlm_cli_pool_shrink(struct ldlm_pool *pl,
				int nr, gfp_t gfp_mask)
{
        struct ldlm_namespace *ns;
	int unused;

        ns = ldlm_pl2ns(pl);

        /*
         * Do not cancel locks in case lru resize is disabled for this ns.
         */
        if (!ns_connect_lru_resize(ns))
                RETURN(0);

        /*
         * Make sure that pool knows last SLV and Limit from obd.
         */
        ldlm_cli_pool_pop_slv(pl);

	spin_lock(&ns->ns_lock);
	unused = ns->ns_nr_unused;
	spin_unlock(&ns->ns_lock);

	if (nr == 0)
		return (unused / 100) * sysctl_vfs_cache_pressure;
	else
		return ldlm_cancel_lru(ns, nr, LCF_ASYNC, 0);
}

static struct ldlm_pool_ops ldlm_srv_pool_ops = {
        .po_recalc = ldlm_srv_pool_recalc,
        .po_shrink = ldlm_srv_pool_shrink,
        .po_setup  = ldlm_srv_pool_setup
};

static struct ldlm_pool_ops ldlm_cli_pool_ops = {
        .po_recalc = ldlm_cli_pool_recalc,
        .po_shrink = ldlm_cli_pool_shrink
};

/**
 * Pool recalc wrapper. Will call either client or server pool recalc callback
 * depending what pool \a pl is used.
 */
time64_t ldlm_pool_recalc(struct ldlm_pool *pl)
{
	time64_t recalc_interval_sec;
	int count;

	recalc_interval_sec = ktime_get_real_seconds() - pl->pl_recalc_time;
	if (recalc_interval_sec > 0) {
		spin_lock(&pl->pl_lock);
		recalc_interval_sec = ktime_get_real_seconds() - pl->pl_recalc_time;

		if (recalc_interval_sec > 0) {
			/*
			 * Update pool statistics every 1s.
			 */
			ldlm_pool_recalc_stats(pl);

			/*
			 * Zero out all rates and speed for the last period.
			 */
			atomic_set(&pl->pl_grant_rate, 0);
			atomic_set(&pl->pl_cancel_rate, 0);
		}
		spin_unlock(&pl->pl_lock);
	}

	if (pl->pl_ops->po_recalc != NULL) {
		count = pl->pl_ops->po_recalc(pl);
		lprocfs_counter_add(pl->pl_stats, LDLM_POOL_RECALC_STAT,
				    count);
	}

	recalc_interval_sec = pl->pl_recalc_time - ktime_get_real_seconds() +
			      pl->pl_recalc_period;
	if (recalc_interval_sec <= 0) {
		/* DEBUG: should be re-removed after LU-4536 is fixed */
		CDEBUG(D_DLMTRACE, "%s: Negative interval(%lld), too short period(%lld)\n",
		       pl->pl_name, recalc_interval_sec,
		       (s64)pl->pl_recalc_period);

		/* Prevent too frequent recalculation. */
		recalc_interval_sec = 1;
	}

	return recalc_interval_sec;
}

/**
 * Pool shrink wrapper. Will call either client or server pool recalc callback
 * depending what pool \a pl is used.
 */
int ldlm_pool_shrink(struct ldlm_pool *pl, int nr, gfp_t gfp_mask)
{
        int cancel = 0;

        if (pl->pl_ops->po_shrink != NULL) {
                cancel = pl->pl_ops->po_shrink(pl, nr, gfp_mask);
                if (nr > 0) {
                        lprocfs_counter_add(pl->pl_stats,
                                            LDLM_POOL_SHRINK_REQTD_STAT,
                                            nr);
                        lprocfs_counter_add(pl->pl_stats,
                                            LDLM_POOL_SHRINK_FREED_STAT,
                                            cancel);
                        CDEBUG(D_DLMTRACE, "%s: request to shrink %d locks, "
                               "shrunk %d\n", pl->pl_name, nr, cancel);
                }
        }
        return cancel;
}

/**
 * Pool setup wrapper. Will call either client or server pool recalc callback
 * depending what pool \a pl is used.
 *
 * Sets passed \a limit into pool \a pl.
 */
int ldlm_pool_setup(struct ldlm_pool *pl, int limit)
{
        if (pl->pl_ops->po_setup != NULL)
                return(pl->pl_ops->po_setup(pl, limit));
        return 0;
}

static int lprocfs_pool_state_seq_show(struct seq_file *m, void *unused)
{
	int granted, grant_rate, cancel_rate, grant_step;
	int grant_speed, grant_plan, lvf;
	struct ldlm_pool *pl = m->private;
	__u64 slv, clv;
	__u32 limit;

	spin_lock(&pl->pl_lock);
	slv = pl->pl_server_lock_volume;
	clv = pl->pl_client_lock_volume;
	limit = ldlm_pool_get_limit(pl);
	grant_plan = pl->pl_grant_plan;
	granted = ldlm_pool_granted(pl);
	grant_rate = atomic_read(&pl->pl_grant_rate);
	cancel_rate = atomic_read(&pl->pl_cancel_rate);
	grant_speed = grant_rate - cancel_rate;
	lvf = atomic_read(&pl->pl_lock_volume_factor);
	grant_step = ldlm_pool_t2gsp(pl->pl_recalc_period);
	spin_unlock(&pl->pl_lock);

	seq_printf(m, "LDLM pool state (%s):\n"
		   "  SLV: %llu\n"
		   "  CLV: %llu\n"
		   "  LVF: %d\n",
		   pl->pl_name, slv, clv, lvf);

	if (ns_is_server(ldlm_pl2ns(pl))) {
		seq_printf(m, "  GSP: %d%%\n", grant_step);
		seq_printf(m, "  GP:  %d\n", grant_plan);
	}

	seq_printf(m, "  GR:  %d\n  CR:  %d\n  GS:  %d\n  G:   %d\n  L:   %d\n",
		   grant_rate, cancel_rate, grant_speed,
		   granted, limit);
	return 0;
}

LDEBUGFS_SEQ_FOPS_RO(lprocfs_pool_state);

static ssize_t grant_speed_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	struct ldlm_pool *pl = container_of(kobj, struct ldlm_pool,
					    pl_kobj);
	int grant_speed;

	spin_lock(&pl->pl_lock);
	/* serialize with ldlm_pool_recalc */
	grant_speed = atomic_read(&pl->pl_grant_rate) -
			atomic_read(&pl->pl_cancel_rate);
	spin_unlock(&pl->pl_lock);
	return sprintf(buf, "%d\n", grant_speed);
}
LUSTRE_RO_ATTR(grant_speed);

LDLM_POOL_SYSFS_READER_SHOW(grant_plan, int);
LUSTRE_RO_ATTR(grant_plan);

LDLM_POOL_SYSFS_READER_SHOW(recalc_period, int);
LDLM_POOL_SYSFS_WRITER_STORE(recalc_period, int);
LUSTRE_RW_ATTR(recalc_period);

LDLM_POOL_SYSFS_READER_NOLOCK_SHOW(server_lock_volume, u64);
LUSTRE_RO_ATTR(server_lock_volume);

LDLM_POOL_SYSFS_READER_NOLOCK_SHOW(limit, atomic);
LDLM_POOL_SYSFS_WRITER_NOLOCK_STORE(limit, atomic);
LUSTRE_RW_ATTR(limit);

LDLM_POOL_SYSFS_READER_NOLOCK_SHOW(granted, atomic);
LUSTRE_RO_ATTR(granted);

LDLM_POOL_SYSFS_READER_NOLOCK_SHOW(cancel_rate, atomic);
LUSTRE_RO_ATTR(cancel_rate);

LDLM_POOL_SYSFS_READER_NOLOCK_SHOW(grant_rate, atomic);
LUSTRE_RO_ATTR(grant_rate);

LDLM_POOL_SYSFS_READER_NOLOCK_SHOW(lock_volume_factor, atomic);
LDLM_POOL_SYSFS_WRITER_NOLOCK_STORE(lock_volume_factor, atomic);
LUSTRE_RW_ATTR(lock_volume_factor);

/* These are for pools in /sys/fs/lustre/ldlm/namespaces/.../pool */
static struct attribute *ldlm_pl_attrs[] = {
	&lustre_attr_grant_speed.attr,
	&lustre_attr_grant_plan.attr,
	&lustre_attr_recalc_period.attr,
	&lustre_attr_server_lock_volume.attr,
	&lustre_attr_limit.attr,
	&lustre_attr_granted.attr,
	&lustre_attr_cancel_rate.attr,
	&lustre_attr_grant_rate.attr,
	&lustre_attr_lock_volume_factor.attr,
	NULL,
};

static void ldlm_pl_release(struct kobject *kobj)
{
	struct ldlm_pool *pl = container_of(kobj, struct ldlm_pool,
					    pl_kobj);
	complete(&pl->pl_kobj_unregister);
}

static struct kobj_type ldlm_pl_ktype = {
	.default_attrs	= ldlm_pl_attrs,
	.sysfs_ops	= &lustre_sysfs_ops,
	.release	= ldlm_pl_release,
};

static int ldlm_pool_sysfs_init(struct ldlm_pool *pl)
{
	struct ldlm_namespace *ns = ldlm_pl2ns(pl);
	int err;

	init_completion(&pl->pl_kobj_unregister);
	err = kobject_init_and_add(&pl->pl_kobj, &ldlm_pl_ktype, &ns->ns_kobj,
				   "pool");

	return err;
}

static int ldlm_pool_debugfs_init(struct ldlm_pool *pl)
{
	struct ldlm_namespace *ns = ldlm_pl2ns(pl);
	struct dentry *debugfs_ns_parent;
	struct lprocfs_vars pool_vars[2];
	char *var_name = NULL;
	int rc = 0;
	ENTRY;

	OBD_ALLOC(var_name, MAX_STRING_SIZE + 1);
	if (!var_name)
		RETURN(-ENOMEM);

	debugfs_ns_parent = ns->ns_debugfs_entry;
	if (IS_ERR_OR_NULL(debugfs_ns_parent)) {
		CERROR("%s: debugfs entry is not initialized\n",
		       ldlm_ns_name(ns));
		GOTO(out_free_name, rc = -EINVAL);
	}
	pl->pl_debugfs_entry = ldebugfs_register("pool", debugfs_ns_parent,
						 NULL, NULL);
	if (IS_ERR(pl->pl_debugfs_entry)) {
		rc = PTR_ERR(pl->pl_debugfs_entry);
		pl->pl_debugfs_entry = NULL;
		CERROR("%s: cannot create 'pool' debugfs entry: rc = %d\n",
		       ldlm_ns_name(ns), rc);
		GOTO(out_free_name, rc);
	}

	var_name[MAX_STRING_SIZE] = '\0';
	memset(pool_vars, 0, sizeof(pool_vars));
	pool_vars[0].name = var_name;

	ldlm_add_var(&pool_vars[0], pl->pl_debugfs_entry, "state", pl,
		     &lprocfs_pool_state_fops);

        pl->pl_stats = lprocfs_alloc_stats(LDLM_POOL_LAST_STAT -
                                           LDLM_POOL_FIRST_STAT, 0);
        if (!pl->pl_stats)
                GOTO(out_free_name, rc = -ENOMEM);

        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANTED_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "granted", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANT_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "grant", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_CANCEL_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "cancel", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANT_RATE_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "grant_rate", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_CANCEL_RATE_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "cancel_rate", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_GRANT_PLAN_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "grant_plan", "locks/s");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_SLV_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "slv", "slv");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_SHRINK_REQTD_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "shrink_request", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_SHRINK_FREED_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "shrink_freed", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_RECALC_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "recalc_freed", "locks");
        lprocfs_counter_init(pl->pl_stats, LDLM_POOL_TIMING_STAT,
                             LPROCFS_CNTR_AVGMINMAX | LPROCFS_CNTR_STDDEV,
                             "recalc_timing", "sec");
	rc = ldebugfs_register_stats(pl->pl_debugfs_entry, "stats",
				     pl->pl_stats);

        EXIT;
out_free_name:
        OBD_FREE(var_name, MAX_STRING_SIZE + 1);
        return rc;
}

static void ldlm_pool_sysfs_fini(struct ldlm_pool *pl)
{
	kobject_put(&pl->pl_kobj);
	wait_for_completion(&pl->pl_kobj_unregister);
}

static void ldlm_pool_debugfs_fini(struct ldlm_pool *pl)
{
        if (pl->pl_stats != NULL) {
                lprocfs_free_stats(&pl->pl_stats);
                pl->pl_stats = NULL;
        }
	if (pl->pl_debugfs_entry != NULL) {
		ldebugfs_remove(&pl->pl_debugfs_entry);
		pl->pl_debugfs_entry = NULL;
        }
}

int ldlm_pool_init(struct ldlm_pool *pl, struct ldlm_namespace *ns,
		   int idx, enum ldlm_side client)
{
	int rc;
	ENTRY;

	spin_lock_init(&pl->pl_lock);
	atomic_set(&pl->pl_granted, 0);
	pl->pl_recalc_time = ktime_get_real_seconds();
	atomic_set(&pl->pl_lock_volume_factor, 1);

	atomic_set(&pl->pl_grant_rate, 0);
	atomic_set(&pl->pl_cancel_rate, 0);
	pl->pl_grant_plan = LDLM_POOL_GP(LDLM_POOL_HOST_L);

	snprintf(pl->pl_name, sizeof(pl->pl_name), "ldlm-pool-%s-%d",
		 ldlm_ns_name(ns), idx);

        if (client == LDLM_NAMESPACE_SERVER) {
                pl->pl_ops = &ldlm_srv_pool_ops;
                ldlm_pool_set_limit(pl, LDLM_POOL_HOST_L);
                pl->pl_recalc_period = LDLM_POOL_SRV_DEF_RECALC_PERIOD;
                pl->pl_server_lock_volume = ldlm_pool_slv_max(LDLM_POOL_HOST_L);
        } else {
                ldlm_pool_set_limit(pl, 1);
                pl->pl_server_lock_volume = 0;
                pl->pl_ops = &ldlm_cli_pool_ops;
                pl->pl_recalc_period = LDLM_POOL_CLI_DEF_RECALC_PERIOD;
        }
        pl->pl_client_lock_volume = 0;
	rc = ldlm_pool_debugfs_init(pl);
        if (rc)
                RETURN(rc);

	rc = ldlm_pool_sysfs_init(pl);
	if (rc)
		RETURN(rc);

	CDEBUG(D_DLMTRACE, "Lock pool %s is initialized\n", pl->pl_name);

	RETURN(rc);
}

void ldlm_pool_fini(struct ldlm_pool *pl)
{
	ENTRY;
	ldlm_pool_sysfs_fini(pl);
	ldlm_pool_debugfs_fini(pl);

        /*
         * Pool should not be used after this point. We can't free it here as
         * it lives in struct ldlm_namespace, but still interested in catching
         * any abnormal using cases.
         */
        POISON(pl, 0x5a, sizeof(*pl));
        EXIT;
}

/**
 * Add new taken ldlm lock \a lock into pool \a pl accounting.
 */
void ldlm_pool_add(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
	/*
	 * FLOCK locks are special in a sense that they are almost never
	 * cancelled, instead special kind of lock is used to drop them.
	 * also there is no LRU for flock locks, so no point in tracking
	 * them anyway.
	 *
	 * PLAIN locks are used by config and quota, the quantity is small
	 * and usually they are not in LRU.
	 */
	if (lock->l_resource->lr_type == LDLM_FLOCK ||
	    lock->l_resource->lr_type == LDLM_PLAIN)
		return;

	ldlm_reclaim_add(lock);

	atomic_inc(&pl->pl_granted);
	atomic_inc(&pl->pl_grant_rate);
	lprocfs_counter_incr(pl->pl_stats, LDLM_POOL_GRANT_STAT);
	/*
	 * Do not do pool recalc for client side as all locks which
	 * potentially may be canceled has already been packed into
	 * enqueue/cancel rpc. Also we do not want to run out of stack
	 * with too long call paths.
	 */
	if (ns_is_server(ldlm_pl2ns(pl)))
		ldlm_pool_recalc(pl);
}

/**
 * Remove ldlm lock \a lock from pool \a pl accounting.
 */
void ldlm_pool_del(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
	/*
	 * Filter out FLOCK & PLAIN locks. Read above comment in
	 * ldlm_pool_add().
	 */
	if (lock->l_resource->lr_type == LDLM_FLOCK ||
	    lock->l_resource->lr_type == LDLM_PLAIN)
		return;

	ldlm_reclaim_del(lock);

	LASSERT(atomic_read(&pl->pl_granted) > 0);
	atomic_dec(&pl->pl_granted);
	atomic_inc(&pl->pl_cancel_rate);

	lprocfs_counter_incr(pl->pl_stats, LDLM_POOL_CANCEL_STAT);

	if (ns_is_server(ldlm_pl2ns(pl)))
		ldlm_pool_recalc(pl);
}

/**
 * Returns current \a pl SLV.
 *
 * \pre ->pl_lock is not locked.
 */
__u64 ldlm_pool_get_slv(struct ldlm_pool *pl)
{
	__u64 slv;
	spin_lock(&pl->pl_lock);
	slv = pl->pl_server_lock_volume;
	spin_unlock(&pl->pl_lock);
	return slv;
}

/**
 * Sets passed \a slv to \a pl.
 *
 * \pre ->pl_lock is not locked.
 */
void ldlm_pool_set_slv(struct ldlm_pool *pl, __u64 slv)
{
	spin_lock(&pl->pl_lock);
	pl->pl_server_lock_volume = slv;
	spin_unlock(&pl->pl_lock);
}

/**
 * Returns current \a pl CLV.
 *
 * \pre ->pl_lock is not locked.
 */
__u64 ldlm_pool_get_clv(struct ldlm_pool *pl)
{
	__u64 slv;
	spin_lock(&pl->pl_lock);
	slv = pl->pl_client_lock_volume;
	spin_unlock(&pl->pl_lock);
	return slv;
}

/**
 * Sets passed \a clv to \a pl.
 *
 * \pre ->pl_lock is not locked.
 */
void ldlm_pool_set_clv(struct ldlm_pool *pl, __u64 clv)
{
	spin_lock(&pl->pl_lock);
	pl->pl_client_lock_volume = clv;
	spin_unlock(&pl->pl_lock);
}

/**
 * Returns current \a pl limit.
 */
__u32 ldlm_pool_get_limit(struct ldlm_pool *pl)
{
	return atomic_read(&pl->pl_limit);
}

/**
 * Sets passed \a limit to \a pl.
 */
void ldlm_pool_set_limit(struct ldlm_pool *pl, __u32 limit)
{
	atomic_set(&pl->pl_limit, limit);
}

/**
 * Returns current LVF from \a pl.
 */
__u32 ldlm_pool_get_lvf(struct ldlm_pool *pl)
{
	return atomic_read(&pl->pl_lock_volume_factor);
}

static struct shrinker *ldlm_pools_srv_shrinker;
static struct shrinker *ldlm_pools_cli_shrinker;

/*
* count locks from all namespaces (if possible). Returns number of
* cached locks.
*/
static unsigned long ldlm_pools_count(enum ldlm_side client, gfp_t gfp_mask)
{
	unsigned long total = 0;
	int nr_ns;
	struct ldlm_namespace *ns;
	struct ldlm_namespace *ns_old = NULL; /* loop detection */

	if (client == LDLM_NAMESPACE_CLIENT && !(gfp_mask & __GFP_FS))
		return 0;

	CDEBUG(D_DLMTRACE, "Request to count %s locks from all pools\n",
	       client == LDLM_NAMESPACE_CLIENT ? "client" : "server");

	/*
	 * Find out how many resources we may release.
	 */
	for (nr_ns = ldlm_namespace_nr_read(client);
	     nr_ns > 0; nr_ns--) {
		mutex_lock(ldlm_namespace_lock(client));
		if (list_empty(ldlm_namespace_list(client))) {
			mutex_unlock(ldlm_namespace_lock(client));
			return 0;
		}
		ns = ldlm_namespace_first_locked(client);

		if (ns == ns_old) {
			mutex_unlock(ldlm_namespace_lock(client));
			break;
		}

		if (ldlm_ns_empty(ns)) {
			ldlm_namespace_move_to_inactive_locked(ns, client);
			mutex_unlock(ldlm_namespace_lock(client));
			continue;
		}

		if (ns_old == NULL)
			ns_old = ns;

		ldlm_namespace_get(ns);
		ldlm_namespace_move_to_active_locked(ns, client);
		mutex_unlock(ldlm_namespace_lock(client));
		total += ldlm_pool_shrink(&ns->ns_pool, 0, gfp_mask);
		ldlm_namespace_put(ns);
	}

	return total;
}

static unsigned long ldlm_pools_scan(enum ldlm_side client, int nr,
				     gfp_t gfp_mask)
{
	unsigned long freed = 0;
	int tmp, nr_ns;
	struct ldlm_namespace *ns;

	if (client == LDLM_NAMESPACE_CLIENT && !(gfp_mask & __GFP_FS))
		return -1;

	/*
	 * Shrink at least ldlm_namespace_nr_read(client) namespaces.
	 */
	for (tmp = nr_ns = ldlm_namespace_nr_read(client);
	     tmp > 0; tmp--) {
		int cancel, nr_locks;

		/*
		 * Do not call shrink under ldlm_namespace_lock(client)
		*/
		mutex_lock(ldlm_namespace_lock(client));
		if (list_empty(ldlm_namespace_list(client))) {
			mutex_unlock(ldlm_namespace_lock(client));
			break;
		}
		ns = ldlm_namespace_first_locked(client);
		ldlm_namespace_get(ns);
		ldlm_namespace_move_to_active_locked(ns, client);
		mutex_unlock(ldlm_namespace_lock(client));

		nr_locks = ldlm_pool_granted(&ns->ns_pool);
		/*
		 * We use to shrink propotionally but with new shrinker API,
		 * we lost the total number of freeable locks.
		 */
		cancel = 1 + min_t(int, nr_locks, nr / nr_ns);
		freed += ldlm_pool_shrink(&ns->ns_pool, cancel, gfp_mask);
		ldlm_namespace_put(ns);
	}
	/*
	 * we only decrease the SLV in server pools shrinker, return
	 * SHRINK_STOP to kernel to avoid needless loop. LU-1128
	 */
	return (client == LDLM_NAMESPACE_SERVER) ? SHRINK_STOP : freed;
}

#ifdef HAVE_SHRINKER_COUNT
static unsigned long ldlm_pools_srv_count(struct shrinker *s,
					  struct shrink_control *sc)
{
	return ldlm_pools_count(LDLM_NAMESPACE_SERVER, sc->gfp_mask);
}

static unsigned long ldlm_pools_srv_scan(struct shrinker *s,
					 struct shrink_control *sc)
{
	return ldlm_pools_scan(LDLM_NAMESPACE_SERVER, sc->nr_to_scan,
			       sc->gfp_mask);
}

static unsigned long ldlm_pools_cli_count(struct shrinker *s, struct shrink_control *sc)
{
	return ldlm_pools_count(LDLM_NAMESPACE_CLIENT, sc->gfp_mask);
}

static unsigned long ldlm_pools_cli_scan(struct shrinker *s,
					 struct shrink_control *sc)
{
	return ldlm_pools_scan(LDLM_NAMESPACE_CLIENT, sc->nr_to_scan,
			       sc->gfp_mask);
}

#else
/*
 * Cancel \a nr locks from all namespaces (if possible). Returns number of
 * cached locks after shrink is finished. All namespaces are asked to
 * cancel approximately equal amount of locks to keep balancing.
 */
static int ldlm_pools_shrink(enum ldlm_side client, int nr, gfp_t gfp_mask)
{
	unsigned long total = 0;

	if (client == LDLM_NAMESPACE_CLIENT && nr != 0 &&
	    !(gfp_mask & __GFP_FS))
		return -1;

	CDEBUG(D_DLMTRACE, "Request to shrink %d %s locks from all pools\n",
	       nr, client == LDLM_NAMESPACE_CLIENT ? "client" : "server");

	total = ldlm_pools_count(client, gfp_mask);

	if (nr == 0 || total == 0)
		return total;

	return ldlm_pools_scan(client, nr, gfp_mask);
}

static int ldlm_pools_srv_shrink(SHRINKER_ARGS(sc, nr_to_scan, gfp_mask))
{
        return ldlm_pools_shrink(LDLM_NAMESPACE_SERVER,
                                 shrink_param(sc, nr_to_scan),
                                 shrink_param(sc, gfp_mask));
}

static int ldlm_pools_cli_shrink(SHRINKER_ARGS(sc, nr_to_scan, gfp_mask))
{
        return ldlm_pools_shrink(LDLM_NAMESPACE_CLIENT,
                                 shrink_param(sc, nr_to_scan),
                                 shrink_param(sc, gfp_mask));
}

#endif /* HAVE_SHRINKER_COUNT */

static time64_t ldlm_pools_recalc_delay(enum ldlm_side side)
{
	struct ldlm_namespace *ns;
	struct ldlm_namespace *ns_old = NULL;
	/* seconds of sleep if no active namespaces */
	time64_t delay = side == LDLM_NAMESPACE_SERVER ?
				 LDLM_POOL_SRV_DEF_RECALC_PERIOD :
				 LDLM_POOL_CLI_DEF_RECALC_PERIOD;
	int nr;

	/* Recalc at least ldlm_namespace_nr(side) namespaces. */
	for (nr = ldlm_namespace_nr_read(side); nr > 0; nr--) {
		int skip;
		/*
		 * Lock the list, get first @ns in the list, getref, move it
		 * to the tail, unlock and call pool recalc. This way we avoid
		 * calling recalc under @ns lock, which is really good as we
		 * get rid of potential deadlock on side nodes when canceling
		 * locks synchronously.
		 */
		mutex_lock(ldlm_namespace_lock(side));
		if (list_empty(ldlm_namespace_list(side))) {
			mutex_unlock(ldlm_namespace_lock(side));
			break;
		}
		ns = ldlm_namespace_first_locked(side);

		if (ns_old == ns) { /* Full pass complete */
			mutex_unlock(ldlm_namespace_lock(side));
			break;
		}

		/* We got an empty namespace, need to move it back to inactive
		 * list.
		 * The race with parallel resource creation is fine:
		 * - If they do namespace_get before our check, we fail the
		 *   check and they move this item to the end of the list anyway
		 * - If we do the check and then they do namespace_get, then
		 *   we move the namespace to inactive and they will move
		 *   it back to active (synchronised by the lock, so no clash
		 *   there).
		 */
		if (ldlm_ns_empty(ns)) {
			ldlm_namespace_move_to_inactive_locked(ns, side);
			mutex_unlock(ldlm_namespace_lock(side));
			continue;
		}

		if (ns_old == NULL)
			ns_old = ns;

		spin_lock(&ns->ns_lock);
		/*
		 * skip ns which is being freed, and we don't want to increase
		 * its refcount again, not even temporarily. bz21519 & LU-499.
		 */
		if (ns->ns_stopping) {
			skip = 1;
		} else {
			skip = 0;
			ldlm_namespace_get(ns);
		}
		spin_unlock(&ns->ns_lock);

		ldlm_namespace_move_to_active_locked(ns, side);
		mutex_unlock(ldlm_namespace_lock(side));

		/*
		 * After setup is done - recalc the pool.
		 */
		if (!skip) {
			delay = min(delay, ldlm_pool_recalc(&ns->ns_pool));
			ldlm_namespace_put(ns);
		}
	}

	return delay;
}

static void ldlm_pools_recalc_task(struct work_struct *ws);
static DECLARE_DELAYED_WORK(ldlm_pools_recalc_work, ldlm_pools_recalc_task);

static void ldlm_pools_recalc_task(struct work_struct *ws)
{
	/* seconds of sleep if no active namespaces */
	time64_t delay;
#ifdef HAVE_SERVER_SUPPORT
	struct ldlm_namespace *ns;
	unsigned long nr_l = 0, nr_p = 0, l;
	int equal = 0;

	/* Check all modest namespaces first. */
	mutex_lock(ldlm_namespace_lock(LDLM_NAMESPACE_SERVER));
	list_for_each_entry(ns, ldlm_namespace_list(LDLM_NAMESPACE_SERVER),
			    ns_list_chain) {
		if (ns->ns_appetite != LDLM_NAMESPACE_MODEST)
			continue;

		l = ldlm_pool_granted(&ns->ns_pool);
		if (l == 0)
			l = 1;

		/*
		 * Set the modest pools limit equal to their avg granted
		 * locks + ~6%.
		 */
		l += dru(l, LDLM_POOLS_MODEST_MARGIN_SHIFT, 0);
		ldlm_pool_setup(&ns->ns_pool, l);
		nr_l += l;
		nr_p++;
	}

	/*
	 * Make sure than modest namespaces did not eat more that 2/3
	 * of limit.
	 */
	if (nr_l >= 2 * (LDLM_POOL_HOST_L / 3)) {
		CWARN("'Modest' pools eat out 2/3 of server locks "
		      "limit (%lu of %lu). This means that you have too "
		      "many clients for this amount of server RAM. "
		      "Upgrade server!\n", nr_l, LDLM_POOL_HOST_L);
		equal = 1;
	}

	/* The rest is given to greedy namespaces. */
	list_for_each_entry(ns, ldlm_namespace_list(LDLM_NAMESPACE_SERVER),
			    ns_list_chain) {
		if (!equal && ns->ns_appetite != LDLM_NAMESPACE_GREEDY)
			continue;

		if (equal) {
			/*
			 * In the case 2/3 locks are eaten out by
			 * modest pools, we re-setup equal limit
			 * for _all_ pools.
			 */
			l = LDLM_POOL_HOST_L /
				ldlm_namespace_nr_read(LDLM_NAMESPACE_SERVER);
		} else {
			/*
			 * All the rest of greedy pools will have
			 * all locks in equal parts.
			 */
			l = (LDLM_POOL_HOST_L - nr_l) /
				(ldlm_namespace_nr_read(LDLM_NAMESPACE_SERVER) -
				 nr_p);
		}
		ldlm_pool_setup(&ns->ns_pool, l);
	}
	mutex_unlock(ldlm_namespace_lock(LDLM_NAMESPACE_SERVER));

	delay = min(ldlm_pools_recalc_delay(LDLM_NAMESPACE_SERVER),
		    ldlm_pools_recalc_delay(LDLM_NAMESPACE_CLIENT));
#else  /* !HAVE_SERVER_SUPPORT */
	delay = ldlm_pools_recalc_delay(LDLM_NAMESPACE_CLIENT);
#endif /* HAVE_SERVER_SUPPORT */

	/* Wake up the blocking threads from time to time. */
	ldlm_bl_thread_wakeup();

	schedule_delayed_work(&ldlm_pools_recalc_work, cfs_time_seconds(delay));
}

int ldlm_pools_init(void)
{
	DEF_SHRINKER_VAR(shsvar, ldlm_pools_srv_shrink,
			 ldlm_pools_srv_count, ldlm_pools_srv_scan);
	DEF_SHRINKER_VAR(shcvar, ldlm_pools_cli_shrink,
			 ldlm_pools_cli_count, ldlm_pools_cli_scan);

	schedule_delayed_work(&ldlm_pools_recalc_work,
			      LDLM_POOL_CLI_DEF_RECALC_PERIOD);
	ldlm_pools_srv_shrinker = set_shrinker(DEFAULT_SEEKS, &shsvar);
	ldlm_pools_cli_shrinker = set_shrinker(DEFAULT_SEEKS, &shcvar);

	return 0;
}

void ldlm_pools_fini(void)
{
	if (ldlm_pools_srv_shrinker != NULL) {
		remove_shrinker(ldlm_pools_srv_shrinker);
		ldlm_pools_srv_shrinker = NULL;
	}
	if (ldlm_pools_cli_shrinker != NULL) {
		remove_shrinker(ldlm_pools_cli_shrinker);
		ldlm_pools_cli_shrinker = NULL;
	}
	cancel_delayed_work_sync(&ldlm_pools_recalc_work);
}

#else /* !HAVE_LRU_RESIZE_SUPPORT */
int ldlm_pool_setup(struct ldlm_pool *pl, int limit)
{
        return 0;
}

time64_t ldlm_pool_recalc(struct ldlm_pool *pl)
{
        return 0;
}

int ldlm_pool_shrink(struct ldlm_pool *pl,
		     int nr, gfp_t gfp_mask)
{
	return 0;
}

int ldlm_pool_init(struct ldlm_pool *pl, struct ldlm_namespace *ns,
		   int idx, enum ldlm_side client)
{
	return 0;
}

void ldlm_pool_fini(struct ldlm_pool *pl)
{
        return;
}

void ldlm_pool_add(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        return;
}

void ldlm_pool_del(struct ldlm_pool *pl, struct ldlm_lock *lock)
{
        return;
}

__u64 ldlm_pool_get_slv(struct ldlm_pool *pl)
{
        return 1;
}

void ldlm_pool_set_slv(struct ldlm_pool *pl, __u64 slv)
{
        return;
}

__u64 ldlm_pool_get_clv(struct ldlm_pool *pl)
{
        return 1;
}

void ldlm_pool_set_clv(struct ldlm_pool *pl, __u64 clv)
{
        return;
}

__u32 ldlm_pool_get_limit(struct ldlm_pool *pl)
{
        return 0;
}

void ldlm_pool_set_limit(struct ldlm_pool *pl, __u32 limit)
{
        return;
}

__u32 ldlm_pool_get_lvf(struct ldlm_pool *pl)
{
        return 0;
}

int ldlm_pools_init(void)
{
        return 0;
}

void ldlm_pools_fini(void)
{
	return;
}

#endif /* HAVE_LRU_RESIZE_SUPPORT */
