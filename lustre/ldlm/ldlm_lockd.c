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
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2010, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ldlm/ldlm_lockd.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_LDLM

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
#else
# include <liblustre.h>
#endif

#include <lustre_dlm.h>
#include <obd_class.h>
#include <libcfs/list.h>
#include "ldlm_internal.h"

static int ldlm_num_threads;
CFS_MODULE_PARM(ldlm_num_threads, "i", int, 0444,
                "number of DLM service threads to start");

static char *ldlm_cpts;
CFS_MODULE_PARM(ldlm_cpts, "s", charp, 0444,
		"CPU partitions ldlm threads should run on");

extern cfs_mem_cache_t *ldlm_resource_slab;
extern cfs_mem_cache_t *ldlm_lock_slab;
static struct mutex	ldlm_ref_mutex;
static int ldlm_refcount;

struct ldlm_cb_async_args {
        struct ldlm_cb_set_arg *ca_set_arg;
        struct ldlm_lock       *ca_lock;
};

/* LDLM state */

static struct ldlm_state *ldlm_state;

inline cfs_time_t round_timeout(cfs_time_t timeout)
{
        return cfs_time_seconds((int)cfs_duration_sec(cfs_time_sub(timeout, 0)) + 1);
}

/* timeout for initial callback (AST) reply (bz10399) */
static inline unsigned int ldlm_get_rq_timeout(void)
{
        /* Non-AT value */
        unsigned int timeout = min(ldlm_timeout, obd_timeout / 3);

        return timeout < 1 ? 1 : timeout;
}

#define ELT_STOPPED   0
#define ELT_READY     1
#define ELT_TERMINATE 2

struct ldlm_bl_pool {
	spinlock_t		blp_lock;

        /*
         * blp_prio_list is used for callbacks that should be handled
         * as a priority. It is used for LDLM_FL_DISCARD_DATA requests.
         * see bug 13843
         */
        cfs_list_t              blp_prio_list;

        /*
         * blp_list is used for all other callbacks which are likely
         * to take longer to process.
         */
        cfs_list_t              blp_list;

        cfs_waitq_t             blp_waitq;
	struct completion        blp_comp;
        cfs_atomic_t            blp_num_threads;
        cfs_atomic_t            blp_busy_threads;
        int                     blp_min_threads;
        int                     blp_max_threads;
};

struct ldlm_bl_work_item {
        cfs_list_t              blwi_entry;
        struct ldlm_namespace  *blwi_ns;
        struct ldlm_lock_desc   blwi_ld;
        struct ldlm_lock       *blwi_lock;
        cfs_list_t              blwi_head;
        int                     blwi_count;
	struct completion        blwi_comp;
	ldlm_cancel_flags_t     blwi_flags;
        int                     blwi_mem_pressure;
};

#if defined(HAVE_SERVER_SUPPORT) && defined(__KERNEL__)

/**
 * Protects both waiting_locks_list and expired_lock_thread.
 */
static spinlock_t waiting_locks_spinlock;   /* BH lock (timer) */

/**
 * List for contended locks.
 *
 * As soon as a lock is contended, it gets placed on this list and
 * expected time to get a response is filled in the lock. A special
 * thread walks the list looking for locks that should be released and
 * schedules client evictions for those that have not been released in
 * time.
 *
 * All access to it should be under waiting_locks_spinlock.
 */
static cfs_list_t waiting_locks_list;
static cfs_timer_t waiting_locks_timer;

static struct expired_lock_thread {
	cfs_waitq_t		elt_waitq;
	int			elt_state;
	int			elt_dump;
	cfs_list_t		elt_expired_locks;
} expired_lock_thread;

static inline int have_expired_locks(void)
{
	int need_to_run;

	ENTRY;
	spin_lock_bh(&waiting_locks_spinlock);
	need_to_run = !cfs_list_empty(&expired_lock_thread.elt_expired_locks);
	spin_unlock_bh(&waiting_locks_spinlock);

	RETURN(need_to_run);
}

/**
 * Check expired lock list for expired locks and time them out.
 */
static int expired_lock_main(void *arg)
{
        cfs_list_t *expired = &expired_lock_thread.elt_expired_locks;
        struct l_wait_info lwi = { 0 };
        int do_dump;

        ENTRY;
        cfs_daemonize("ldlm_elt");

        expired_lock_thread.elt_state = ELT_READY;
        cfs_waitq_signal(&expired_lock_thread.elt_waitq);

        while (1) {
                l_wait_event(expired_lock_thread.elt_waitq,
                             have_expired_locks() ||
                             expired_lock_thread.elt_state == ELT_TERMINATE,
                             &lwi);

		spin_lock_bh(&waiting_locks_spinlock);
		if (expired_lock_thread.elt_dump) {
			struct libcfs_debug_msg_data msgdata = {
				.msg_file = __FILE__,
				.msg_fn = "waiting_locks_callback",
				.msg_line = expired_lock_thread.elt_dump };
			spin_unlock_bh(&waiting_locks_spinlock);

			/* from waiting_locks_callback, but not in timer */
			libcfs_debug_dumplog();
			libcfs_run_lbug_upcall(&msgdata);

			spin_lock_bh(&waiting_locks_spinlock);
                        expired_lock_thread.elt_dump = 0;
                }

                do_dump = 0;

                while (!cfs_list_empty(expired)) {
                        struct obd_export *export;
                        struct ldlm_lock *lock;

                        lock = cfs_list_entry(expired->next, struct ldlm_lock,
                                          l_pending_chain);
                        if ((void *)lock < LP_POISON + CFS_PAGE_SIZE &&
                            (void *)lock >= LP_POISON) {
				spin_unlock_bh(&waiting_locks_spinlock);
                                CERROR("free lock on elt list %p\n", lock);
                                LBUG();
                        }
                        cfs_list_del_init(&lock->l_pending_chain);
                        if ((void *)lock->l_export < LP_POISON + CFS_PAGE_SIZE &&
                            (void *)lock->l_export >= LP_POISON) {
                                CERROR("lock with free export on elt list %p\n",
                                       lock->l_export);
                                lock->l_export = NULL;
                                LDLM_ERROR(lock, "free export");
                                /* release extra ref grabbed by
                                 * ldlm_add_waiting_lock() or
                                 * ldlm_failed_ast() */
                                LDLM_LOCK_RELEASE(lock);
                                continue;
                        }

			if (lock->l_destroyed) {
				/* release the lock refcount where
				 * waiting_locks_callback() founds */
				LDLM_LOCK_RELEASE(lock);
				continue;
			}
			export = class_export_lock_get(lock->l_export, lock);
			spin_unlock_bh(&waiting_locks_spinlock);

			do_dump++;
			class_fail_export(export);
			class_export_lock_put(export, lock);

			/* release extra ref grabbed by ldlm_add_waiting_lock()
			 * or ldlm_failed_ast() */
			LDLM_LOCK_RELEASE(lock);

			spin_lock_bh(&waiting_locks_spinlock);
		}
		spin_unlock_bh(&waiting_locks_spinlock);

                if (do_dump && obd_dump_on_eviction) {
                        CERROR("dump the log upon eviction\n");
                        libcfs_debug_dumplog();
                }

                if (expired_lock_thread.elt_state == ELT_TERMINATE)
                        break;
        }

        expired_lock_thread.elt_state = ELT_STOPPED;
        cfs_waitq_signal(&expired_lock_thread.elt_waitq);
        RETURN(0);
}

static int ldlm_add_waiting_lock(struct ldlm_lock *lock);
static int __ldlm_add_waiting_lock(struct ldlm_lock *lock, int seconds);

/**
 * Check if there is a request in the export request list
 * which prevents the lock canceling.
 */
static int ldlm_lock_busy(struct ldlm_lock *lock)
{
	struct ptlrpc_request *req;
	int match = 0;
	ENTRY;

	if (lock->l_export == NULL)
		return 0;

	spin_lock_bh(&lock->l_export->exp_rpc_lock);
	cfs_list_for_each_entry(req, &lock->l_export->exp_hp_rpcs,
				rq_exp_list) {
		if (req->rq_ops->hpreq_lock_match) {
			match = req->rq_ops->hpreq_lock_match(req, lock);
			if (match)
				break;
		}
	}
	spin_unlock_bh(&lock->l_export->exp_rpc_lock);
	RETURN(match);
}

/* This is called from within a timer interrupt and cannot schedule */
static void waiting_locks_callback(unsigned long unused)
{
	struct ldlm_lock	*lock;
	int			need_dump = 0;

	spin_lock_bh(&waiting_locks_spinlock);
        while (!cfs_list_empty(&waiting_locks_list)) {
                lock = cfs_list_entry(waiting_locks_list.next, struct ldlm_lock,
                                      l_pending_chain);
                if (cfs_time_after(lock->l_callback_timeout,
                                   cfs_time_current()) ||
                    (lock->l_req_mode == LCK_GROUP))
                        break;

                if (ptlrpc_check_suspend()) {
                        /* there is a case when we talk to one mds, holding
                         * lock from another mds. this way we easily can get
                         * here, if second mds is being recovered. so, we
                         * suspend timeouts. bug 6019 */

                        LDLM_ERROR(lock, "recharge timeout: %s@%s nid %s ",
                                   lock->l_export->exp_client_uuid.uuid,
                                   lock->l_export->exp_connection->c_remote_uuid.uuid,
                                   libcfs_nid2str(lock->l_export->exp_connection->c_peer.nid));

                        cfs_list_del_init(&lock->l_pending_chain);
			if (lock->l_destroyed) {
				/* relay the lock refcount decrease to
				 * expired lock thread */
				cfs_list_add(&lock->l_pending_chain,
					&expired_lock_thread.elt_expired_locks);
			} else {
				__ldlm_add_waiting_lock(lock,
						ldlm_get_enq_timeout(lock));
			}
			continue;
                }

                /* if timeout overlaps the activation time of suspended timeouts
                 * then extend it to give a chance for client to reconnect */
                if (cfs_time_before(cfs_time_sub(lock->l_callback_timeout,
                                                 cfs_time_seconds(obd_timeout)/2),
                                    ptlrpc_suspend_wakeup_time())) {
                        LDLM_ERROR(lock, "extend timeout due to recovery: %s@%s nid %s ",
                                   lock->l_export->exp_client_uuid.uuid,
                                   lock->l_export->exp_connection->c_remote_uuid.uuid,
                                   libcfs_nid2str(lock->l_export->exp_connection->c_peer.nid));

                        cfs_list_del_init(&lock->l_pending_chain);
			if (lock->l_destroyed) {
				/* relay the lock refcount decrease to
				 * expired lock thread */
				cfs_list_add(&lock->l_pending_chain,
					&expired_lock_thread.elt_expired_locks);
			} else {
				__ldlm_add_waiting_lock(lock,
						ldlm_get_enq_timeout(lock));
			}
			continue;
                }

                /* Check if we need to prolong timeout */
                if (!OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_HPREQ_TIMEOUT) &&
                    ldlm_lock_busy(lock)) {
                        int cont = 1;

                        if (lock->l_pending_chain.next == &waiting_locks_list)
                                cont = 0;

                        LDLM_LOCK_GET(lock);

			spin_unlock_bh(&waiting_locks_spinlock);
			LDLM_DEBUG(lock, "prolong the busy lock");
			ldlm_refresh_waiting_lock(lock,
						  ldlm_get_enq_timeout(lock));
			spin_lock_bh(&waiting_locks_spinlock);

                        if (!cont) {
                                LDLM_LOCK_RELEASE(lock);
                                break;
                        }

                        LDLM_LOCK_RELEASE(lock);
                        continue;
                }
                ldlm_lock_to_ns(lock)->ns_timeouts++;
                LDLM_ERROR(lock, "lock callback timer expired after %lds: "
                           "evicting client at %s ",
                           cfs_time_current_sec()- lock->l_last_activity,
                           libcfs_nid2str(
                                   lock->l_export->exp_connection->c_peer.nid));

                /* no needs to take an extra ref on the lock since it was in
                 * the waiting_locks_list and ldlm_add_waiting_lock()
                 * already grabbed a ref */
                cfs_list_del(&lock->l_pending_chain);
                cfs_list_add(&lock->l_pending_chain,
                             &expired_lock_thread.elt_expired_locks);
		need_dump = 1;
	}

	if (!cfs_list_empty(&expired_lock_thread.elt_expired_locks)) {
		if (obd_dump_on_timeout && need_dump)
			expired_lock_thread.elt_dump = __LINE__;

		cfs_waitq_signal(&expired_lock_thread.elt_waitq);
	}

        /*
         * Make sure the timer will fire again if we have any locks
         * left.
         */
        if (!cfs_list_empty(&waiting_locks_list)) {
                cfs_time_t timeout_rounded;
                lock = cfs_list_entry(waiting_locks_list.next, struct ldlm_lock,
                                      l_pending_chain);
                timeout_rounded = (cfs_time_t)round_timeout(lock->l_callback_timeout);
                cfs_timer_arm(&waiting_locks_timer, timeout_rounded);
        }
	spin_unlock_bh(&waiting_locks_spinlock);
}

/**
 * Add lock to the list of contended locks.
 *
 * Indicate that we're waiting for a client to call us back cancelling a given
 * lock.  We add it to the pending-callback chain, and schedule the lock-timeout
 * timer to fire appropriately.  (We round up to the next second, to avoid
 * floods of timer firings during periods of high lock contention and traffic).
 * As done by ldlm_add_waiting_lock(), the caller must grab a lock reference
 * if it has been added to the waiting list (1 is returned).
 *
 * Called with the namespace lock held.
 */
static int __ldlm_add_waiting_lock(struct ldlm_lock *lock, int seconds)
{
        cfs_time_t timeout;
        cfs_time_t timeout_rounded;

        if (!cfs_list_empty(&lock->l_pending_chain))
                return 0;

        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_HPREQ_NOTIMEOUT) ||
            OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_HPREQ_TIMEOUT))
                seconds = 1;

        timeout = cfs_time_shift(seconds);
        if (likely(cfs_time_after(timeout, lock->l_callback_timeout)))
                lock->l_callback_timeout = timeout;

        timeout_rounded = round_timeout(lock->l_callback_timeout);

        if (cfs_time_before(timeout_rounded,
                            cfs_timer_deadline(&waiting_locks_timer)) ||
            !cfs_timer_is_armed(&waiting_locks_timer)) {
                cfs_timer_arm(&waiting_locks_timer, timeout_rounded);
        }
        /* if the new lock has a shorter timeout than something earlier on
           the list, we'll wait the longer amount of time; no big deal. */
        /* FIFO */
        cfs_list_add_tail(&lock->l_pending_chain, &waiting_locks_list);
        return 1;
}

static int ldlm_add_waiting_lock(struct ldlm_lock *lock)
{
	int ret;
	int timeout = ldlm_get_enq_timeout(lock);

	/* NB: must be called with hold of lock_res_and_lock() */
	LASSERT(lock->l_res_locked);
	lock->l_waited = 1;

	LASSERT(!(lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK));

	spin_lock_bh(&waiting_locks_spinlock);
	if (lock->l_destroyed) {
		static cfs_time_t next;
		spin_unlock_bh(&waiting_locks_spinlock);
                LDLM_ERROR(lock, "not waiting on destroyed lock (bug 5653)");
                if (cfs_time_after(cfs_time_current(), next)) {
                        next = cfs_time_shift(14400);
                        libcfs_debug_dumpstack(NULL);
                }
                return 0;
        }

        ret = __ldlm_add_waiting_lock(lock, timeout);
        if (ret) {
                /* grab ref on the lock if it has been added to the
                 * waiting list */
                LDLM_LOCK_GET(lock);
        }
	spin_unlock_bh(&waiting_locks_spinlock);

	if (ret) {
		spin_lock_bh(&lock->l_export->exp_bl_list_lock);
		if (cfs_list_empty(&lock->l_exp_list))
			cfs_list_add(&lock->l_exp_list,
				     &lock->l_export->exp_bl_list);
		spin_unlock_bh(&lock->l_export->exp_bl_list_lock);
	}

	LDLM_DEBUG(lock, "%sadding to wait list(timeout: %d, AT: %s)",
		   ret == 0 ? "not re-" : "", timeout,
		   AT_OFF ? "off" : "on");
	return ret;
}

/**
 * Remove a lock from the pending list, likely because it had its cancellation
 * callback arrive without incident.  This adjusts the lock-timeout timer if
 * needed.  Returns 0 if the lock wasn't pending after all, 1 if it was.
 * As done by ldlm_del_waiting_lock(), the caller must release the lock
 * reference when the lock is removed from any list (1 is returned).
 *
 * Called with namespace lock held.
 */
static int __ldlm_del_waiting_lock(struct ldlm_lock *lock)
{
        cfs_list_t *list_next;

        if (cfs_list_empty(&lock->l_pending_chain))
                return 0;

        list_next = lock->l_pending_chain.next;
        if (lock->l_pending_chain.prev == &waiting_locks_list) {
                /* Removing the head of the list, adjust timer. */
                if (list_next == &waiting_locks_list) {
                        /* No more, just cancel. */
                        cfs_timer_disarm(&waiting_locks_timer);
                } else {
                        struct ldlm_lock *next;
                        next = cfs_list_entry(list_next, struct ldlm_lock,
                                              l_pending_chain);
                        cfs_timer_arm(&waiting_locks_timer,
                                      round_timeout(next->l_callback_timeout));
                }
        }
        cfs_list_del_init(&lock->l_pending_chain);

        return 1;
}

int ldlm_del_waiting_lock(struct ldlm_lock *lock)
{
        int ret;

        if (lock->l_export == NULL) {
                /* We don't have a "waiting locks list" on clients. */
                CDEBUG(D_DLMTRACE, "Client lock %p : no-op\n", lock);
                return 0;
        }

	spin_lock_bh(&waiting_locks_spinlock);
	ret = __ldlm_del_waiting_lock(lock);
	spin_unlock_bh(&waiting_locks_spinlock);

	/* remove the lock out of export blocking list */
	spin_lock_bh(&lock->l_export->exp_bl_list_lock);
	cfs_list_del_init(&lock->l_exp_list);
	spin_unlock_bh(&lock->l_export->exp_bl_list_lock);

        if (ret) {
                /* release lock ref if it has indeed been removed
                 * from a list */
                LDLM_LOCK_RELEASE(lock);
        }

        LDLM_DEBUG(lock, "%s", ret == 0 ? "wasn't waiting" : "removed");
        return ret;
}
EXPORT_SYMBOL(ldlm_del_waiting_lock);

/**
 * Prolong the contended lock waiting time.
 *
 * Called with namespace lock held.
 */
int ldlm_refresh_waiting_lock(struct ldlm_lock *lock, int timeout)
{
	if (lock->l_export == NULL) {
		/* We don't have a "waiting locks list" on clients. */
		LDLM_DEBUG(lock, "client lock: no-op");
		return 0;
	}

	spin_lock_bh(&waiting_locks_spinlock);

	if (cfs_list_empty(&lock->l_pending_chain)) {
		spin_unlock_bh(&waiting_locks_spinlock);
		LDLM_DEBUG(lock, "wasn't waiting");
		return 0;
	}

	/* we remove/add the lock to the waiting list, so no needs to
	 * release/take a lock reference */
	__ldlm_del_waiting_lock(lock);
	__ldlm_add_waiting_lock(lock, timeout);
	spin_unlock_bh(&waiting_locks_spinlock);

	LDLM_DEBUG(lock, "refreshed");
	return 1;
}
EXPORT_SYMBOL(ldlm_refresh_waiting_lock);

#else /* !HAVE_SERVER_SUPPORT ||  !__KERNEL__ */

int ldlm_del_waiting_lock(struct ldlm_lock *lock)
{
        RETURN(0);
}

int ldlm_refresh_waiting_lock(struct ldlm_lock *lock, int timeout)
{
        RETURN(0);
}

# ifdef HAVE_SERVER_SUPPORT
static int ldlm_add_waiting_lock(struct ldlm_lock *lock)
{
	LASSERT(lock->l_res_locked);
	LASSERT(!(lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK));
	RETURN(1);
}

# endif
#endif /* HAVE_SERVER_SUPPORT && __KERNEL__ */

#ifdef HAVE_SERVER_SUPPORT

/**
 * Perform lock cleanup if AST sending failed.
 */
static void ldlm_failed_ast(struct ldlm_lock *lock, int rc,
                            const char *ast_type)
{
        LCONSOLE_ERROR_MSG(0x138, "%s: A client on nid %s was evicted due "
                           "to a lock %s callback time out: rc %d\n",
                           lock->l_export->exp_obd->obd_name,
                           obd_export_nid2str(lock->l_export), ast_type, rc);

        if (obd_dump_on_timeout)
                libcfs_debug_dumplog();
#ifdef __KERNEL__
	spin_lock_bh(&waiting_locks_spinlock);
	if (__ldlm_del_waiting_lock(lock) == 0)
		/* the lock was not in any list, grab an extra ref before adding
		 * the lock to the expired list */
		LDLM_LOCK_GET(lock);
	cfs_list_add(&lock->l_pending_chain,
		     &expired_lock_thread.elt_expired_locks);
	cfs_waitq_signal(&expired_lock_thread.elt_waitq);
	spin_unlock_bh(&waiting_locks_spinlock);
#else
	class_fail_export(lock->l_export);
#endif
}

/**
 * Perform lock cleanup if AST reply came with error.
 */
static int ldlm_handle_ast_error(struct ldlm_lock *lock,
                                 struct ptlrpc_request *req, int rc,
                                 const char *ast_type)
{
        lnet_process_id_t peer = req->rq_import->imp_connection->c_peer;

        if (rc == -ETIMEDOUT || rc == -EINTR || rc == -ENOTCONN) {
                LASSERT(lock->l_export);
                if (lock->l_export->exp_libclient) {
                        LDLM_DEBUG(lock, "%s AST to liblustre client (nid %s)"
                                   " timeout, just cancelling lock", ast_type,
                                   libcfs_nid2str(peer.nid));
                        ldlm_lock_cancel(lock);
                        rc = -ERESTART;
                } else if (lock->l_flags & LDLM_FL_CANCEL) {
                        LDLM_DEBUG(lock, "%s AST timeout from nid %s, but "
                                   "cancel was received (AST reply lost?)",
                                   ast_type, libcfs_nid2str(peer.nid));
                        ldlm_lock_cancel(lock);
                        rc = -ERESTART;
                } else {
                        ldlm_del_waiting_lock(lock);
                        ldlm_failed_ast(lock, rc, ast_type);
                }
        } else if (rc) {
                if (rc == -EINVAL) {
                        struct ldlm_resource *res = lock->l_resource;
                        LDLM_DEBUG(lock, "client (nid %s) returned %d"
                               " from %s AST - normal race",
                               libcfs_nid2str(peer.nid),
                               req->rq_repmsg ?
                               lustre_msg_get_status(req->rq_repmsg) : -1,
                               ast_type);
                        if (res) {
                                /* update lvbo to return proper attributes.
                                 * see bug 23174 */
                                ldlm_resource_getref(res);
                                ldlm_res_lvbo_update(res, NULL, 1);
                                ldlm_resource_putref(res);
                        }

                } else {
                        LDLM_ERROR(lock, "client (nid %s) returned %d "
                                   "from %s AST", libcfs_nid2str(peer.nid),
                                   (req->rq_repmsg != NULL) ?
                                   lustre_msg_get_status(req->rq_repmsg) : 0,
                                   ast_type);
                }
                ldlm_lock_cancel(lock);
                /* Server-side AST functions are called from ldlm_reprocess_all,
                 * which needs to be told to please restart its reprocessing. */
                rc = -ERESTART;
        }

        return rc;
}

static int ldlm_cb_interpret(const struct lu_env *env,
                             struct ptlrpc_request *req, void *data, int rc)
{
        struct ldlm_cb_async_args *ca   = data;
        struct ldlm_lock          *lock = ca->ca_lock;
        struct ldlm_cb_set_arg    *arg  = ca->ca_set_arg;
        ENTRY;

        LASSERT(lock != NULL);

	switch (arg->type) {
	case LDLM_GL_CALLBACK:
		/* Update the LVB from disk if the AST failed
		 * (this is a legal race)
		 *
		 * - Glimpse callback of local lock just returns
		 *   -ELDLM_NO_LOCK_DATA.
		 * - Glimpse callback of remote lock might return
		 *   -ELDLM_NO_LOCK_DATA when inode is cleared. LU-274
		 */
		if (rc == -ELDLM_NO_LOCK_DATA) {
			LDLM_DEBUG(lock, "lost race - client has a lock but no "
				   "inode");
			ldlm_res_lvbo_update(lock->l_resource, NULL, 1);
		} else if (rc != 0) {
			rc = ldlm_handle_ast_error(lock, req, rc, "glimpse");
		} else {
			rc = ldlm_res_lvbo_update(lock->l_resource, req, 1);
		}
		break;
	case LDLM_BL_CALLBACK:
		if (rc != 0)
			rc = ldlm_handle_ast_error(lock, req, rc, "blocking");
		break;
	case LDLM_CP_CALLBACK:
		if (rc != 0)
			rc = ldlm_handle_ast_error(lock, req, rc, "completion");
		break;
	default:
		LDLM_ERROR(lock, "invalid opcode for lock callback %d",
			   arg->type);
		LBUG();
	}

	/* release extra reference taken in ldlm_ast_fini() */
        LDLM_LOCK_RELEASE(lock);

	if (rc == -ERESTART)
		cfs_atomic_inc(&arg->restart);

        RETURN(0);
}

static inline int ldlm_ast_fini(struct ptlrpc_request *req,
				struct ldlm_cb_set_arg *arg,
				struct ldlm_lock *lock,
				int instant_cancel)
{
	int rc = 0;
	ENTRY;

	if (unlikely(instant_cancel)) {
		rc = ptl_send_rpc(req, 1);
		ptlrpc_req_finished(req);
		if (rc == 0)
			cfs_atomic_inc(&arg->restart);
	} else {
		LDLM_LOCK_GET(lock);
		ptlrpc_set_add_req(arg->set, req);
	}

	RETURN(rc);
}

/**
 * Check if there are requests in the export request list which prevent
 * the lock canceling and make these requests high priority ones.
 */
static void ldlm_lock_reorder_req(struct ldlm_lock *lock)
{
	struct ptlrpc_request *req;
	ENTRY;

	if (lock->l_export == NULL) {
		LDLM_DEBUG(lock, "client lock: no-op");
		RETURN_EXIT;
	}

	spin_lock_bh(&lock->l_export->exp_rpc_lock);
	cfs_list_for_each_entry(req, &lock->l_export->exp_hp_rpcs,
				rq_exp_list) {
		/* Do not process requests that were not yet added to there
		 * incoming queue or were already removed from there for
		 * processing. We evaluate ptlrpc_nrs_req_can_move() without
		 * holding svcpt->scp_req_lock, and then redo the check with
		 * the lock held once we need to obtain a reliable result.
		 */
		if (ptlrpc_nrs_req_can_move(req) &&
		    req->rq_ops->hpreq_lock_match &&
		    req->rq_ops->hpreq_lock_match(req, lock))
			ptlrpc_nrs_req_hp_move(req);
	}
	spin_unlock_bh(&lock->l_export->exp_rpc_lock);
	EXIT;
}

/**
 * ->l_blocking_ast() method for server-side locks. This is invoked when newly
 * enqueued server lock conflicts with given one.
 *
 * Sends blocking AST RPC to the client owning that lock; arms timeout timer
 * to wait for client response.
 */
int ldlm_server_blocking_ast(struct ldlm_lock *lock,
                             struct ldlm_lock_desc *desc,
                             void *data, int flag)
{
        struct ldlm_cb_async_args *ca;
        struct ldlm_cb_set_arg *arg = data;
        struct ldlm_request    *body;
        struct ptlrpc_request  *req;
        int                     instant_cancel = 0;
        int                     rc = 0;
        ENTRY;

        if (flag == LDLM_CB_CANCELING)
                /* Don't need to do anything here. */
                RETURN(0);

        LASSERT(lock);
        LASSERT(data != NULL);
        if (lock->l_export->exp_obd->obd_recovering != 0)
                LDLM_ERROR(lock, "BUG 6063: lock collide during recovery");

        ldlm_lock_reorder_req(lock);

        req = ptlrpc_request_alloc_pack(lock->l_export->exp_imp_reverse,
                                        &RQF_LDLM_BL_CALLBACK,
                                        LUSTRE_DLM_VERSION, LDLM_BL_CALLBACK);
        if (req == NULL)
                RETURN(-ENOMEM);

        CLASSERT(sizeof(*ca) <= sizeof(req->rq_async_args));
        ca = ptlrpc_req_async_args(req);
        ca->ca_set_arg = arg;
        ca->ca_lock = lock;

        req->rq_interpret_reply = ldlm_cb_interpret;
        req->rq_no_resend = 1;

	lock_res_and_lock(lock);
	if (lock->l_granted_mode != lock->l_req_mode) {
		/* this blocking AST will be communicated as part of the
		 * completion AST instead */
		unlock_res_and_lock(lock);

		ptlrpc_req_finished(req);
		LDLM_DEBUG(lock, "lock not granted, not sending blocking AST");
		RETURN(0);
	}

	if (lock->l_destroyed) {
		/* What's the point? */
		unlock_res_and_lock(lock);
		ptlrpc_req_finished(req);
		RETURN(0);
	}

        if (lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK)
                instant_cancel = 1;

        body = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
        body->lock_handle[0] = lock->l_remote_handle;
        body->lock_desc = *desc;
	body->lock_flags |= ldlm_flags_to_wire(lock->l_flags & LDLM_AST_FLAGS);

        LDLM_DEBUG(lock, "server preparing blocking AST");

        ptlrpc_request_set_replen(req);
	if (instant_cancel) {
		unlock_res_and_lock(lock);
		ldlm_lock_cancel(lock);
	} else {
		LASSERT(lock->l_granted_mode == lock->l_req_mode);
		ldlm_add_waiting_lock(lock);
		unlock_res_and_lock(lock);
	}

        req->rq_send_state = LUSTRE_IMP_FULL;
        /* ptlrpc_request_alloc_pack already set timeout */
        if (AT_OFF)
                req->rq_timeout = ldlm_get_rq_timeout();

        if (lock->l_export && lock->l_export->exp_nid_stats &&
            lock->l_export->exp_nid_stats->nid_ldlm_stats)
                lprocfs_counter_incr(lock->l_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_BL_CALLBACK - LDLM_FIRST_OPC);

	rc = ldlm_ast_fini(req, arg, lock, instant_cancel);

        RETURN(rc);
}
EXPORT_SYMBOL(ldlm_server_blocking_ast);

/**
 * ->l_completion_ast callback for a remote lock in server namespace.
 *
 *  Sends AST to the client notifying it of lock granting.  If initial
 *  lock response was not sent yet, instead of sending another RPC, just
 *  mark the lock as granted and client will understand
 */
int ldlm_server_completion_ast(struct ldlm_lock *lock, __u64 flags, void *data)
{
        struct ldlm_cb_set_arg *arg = data;
        struct ldlm_request    *body;
        struct ptlrpc_request  *req;
        struct ldlm_cb_async_args *ca;
        long                    total_enqueue_wait;
        int                     instant_cancel = 0;
        int                     rc = 0;
	int			lvb_len;
        ENTRY;

        LASSERT(lock != NULL);
        LASSERT(data != NULL);

        total_enqueue_wait = cfs_time_sub(cfs_time_current_sec(),
                                          lock->l_last_activity);

        req = ptlrpc_request_alloc(lock->l_export->exp_imp_reverse,
                                    &RQF_LDLM_CP_CALLBACK);
        if (req == NULL)
                RETURN(-ENOMEM);

	/* server namespace, doesn't need lock */
	lvb_len = ldlm_lvbo_size(lock);
	/* LU-3124 & LU-2187: to not return layout in completion AST because
	 * it may deadlock for LU-2187, or client may not have enough space
	 * for large layout. The layout will be returned to client with an
	 * extra RPC to fetch xattr.lov */
	if (ldlm_has_layout(lock))
		lvb_len = 0;

	req_capsule_set_size(&req->rq_pill, &RMF_DLM_LVB, RCL_CLIENT, lvb_len);
        rc = ptlrpc_request_pack(req, LUSTRE_DLM_VERSION, LDLM_CP_CALLBACK);
        if (rc) {
                ptlrpc_request_free(req);
                RETURN(rc);
        }

        CLASSERT(sizeof(*ca) <= sizeof(req->rq_async_args));
        ca = ptlrpc_req_async_args(req);
        ca->ca_set_arg = arg;
        ca->ca_lock = lock;

        req->rq_interpret_reply = ldlm_cb_interpret;
        req->rq_no_resend = 1;
        body = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);

        body->lock_handle[0] = lock->l_remote_handle;
	body->lock_flags = ldlm_flags_to_wire(flags);
        ldlm_lock2desc(lock, &body->lock_desc);
	if (lvb_len > 0) {
		void *lvb = req_capsule_client_get(&req->rq_pill, &RMF_DLM_LVB);

		lvb_len = ldlm_lvbo_fill(lock, lvb, lvb_len);
		if (lvb_len < 0) {
			/* We still need to send the RPC to wake up the blocked
			 * enqueue thread on the client.
			 *
			 * Consider old client, there is no better way to notify
			 * the failure, just zero-sized the LVB, then the client
			 * will fail out as "-EPROTO". */
			req_capsule_shrink(&req->rq_pill, &RMF_DLM_LVB, 0,
					   RCL_CLIENT);
			instant_cancel = 1;
		} else {
			req_capsule_shrink(&req->rq_pill, &RMF_DLM_LVB, lvb_len,
					   RCL_CLIENT);
		}
        }

        LDLM_DEBUG(lock, "server preparing completion AST (after %lds wait)",
                   total_enqueue_wait);

        /* Server-side enqueue wait time estimate, used in
            __ldlm_add_waiting_lock to set future enqueue timers */
        if (total_enqueue_wait < ldlm_get_enq_timeout(lock))
                at_measured(ldlm_lock_to_ns_at(lock),
                            total_enqueue_wait);
        else
                /* bz18618. Don't add lock enqueue time we spend waiting for a
                   previous callback to fail. Locks waiting legitimately will
                   get extended by ldlm_refresh_waiting_lock regardless of the
                   estimate, so it's okay to underestimate here. */
                LDLM_DEBUG(lock, "lock completed after %lus; estimate was %ds. "
                       "It is likely that a previous callback timed out.",
                       total_enqueue_wait,
                       at_get(ldlm_lock_to_ns_at(lock)));

        ptlrpc_request_set_replen(req);

        req->rq_send_state = LUSTRE_IMP_FULL;
        /* ptlrpc_request_pack already set timeout */
        if (AT_OFF)
                req->rq_timeout = ldlm_get_rq_timeout();

        /* We only send real blocking ASTs after the lock is granted */
        lock_res_and_lock(lock);
        if (lock->l_flags & LDLM_FL_AST_SENT) {
		body->lock_flags |= ldlm_flags_to_wire(LDLM_FL_AST_SENT);
		/* Copy AST flags like LDLM_FL_DISCARD_DATA. */
		body->lock_flags |= ldlm_flags_to_wire(lock->l_flags &
						       LDLM_AST_FLAGS);

                /* We might get here prior to ldlm_handle_enqueue setting
                 * LDLM_FL_CANCEL_ON_BLOCK flag. Then we will put this lock
                 * into waiting list, but this is safe and similar code in
                 * ldlm_handle_enqueue will call ldlm_lock_cancel() still,
                 * that would not only cancel the lock, but will also remove
                 * it from waiting list */
                if (lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK) {
                        unlock_res_and_lock(lock);
                        ldlm_lock_cancel(lock);
                        instant_cancel = 1;
                        lock_res_and_lock(lock);
                } else {
                        /* start the lock-timeout clock */
                        ldlm_add_waiting_lock(lock);
                }
        }
        unlock_res_and_lock(lock);

        if (lock->l_export && lock->l_export->exp_nid_stats &&
            lock->l_export->exp_nid_stats->nid_ldlm_stats)
                lprocfs_counter_incr(lock->l_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_CP_CALLBACK - LDLM_FIRST_OPC);

	rc = ldlm_ast_fini(req, arg, lock, instant_cancel);

	RETURN(lvb_len < 0 ? lvb_len : rc);
}
EXPORT_SYMBOL(ldlm_server_completion_ast);

/**
 * Server side ->l_glimpse_ast handler for client locks.
 *
 * Sends glimpse AST to the client and waits for reply. Then updates
 * lvbo with the result.
 */
int ldlm_server_glimpse_ast(struct ldlm_lock *lock, void *data)
{
	struct ldlm_cb_set_arg		*arg = data;
	struct ldlm_request		*body;
	struct ptlrpc_request		*req;
	struct ldlm_cb_async_args	*ca;
	int				 rc;
	struct req_format		*req_fmt;
        ENTRY;

        LASSERT(lock != NULL);

	if (arg->gl_desc != NULL)
		/* There is a glimpse descriptor to pack */
		req_fmt = &RQF_LDLM_GL_DESC_CALLBACK;
	else
		req_fmt = &RQF_LDLM_GL_CALLBACK;

        req = ptlrpc_request_alloc_pack(lock->l_export->exp_imp_reverse,
					req_fmt, LUSTRE_DLM_VERSION,
					LDLM_GL_CALLBACK);

        if (req == NULL)
                RETURN(-ENOMEM);

	if (arg->gl_desc != NULL) {
		/* copy the GL descriptor */
		union ldlm_gl_desc	*desc;
		desc = req_capsule_client_get(&req->rq_pill, &RMF_DLM_GL_DESC);
		*desc = *arg->gl_desc;
	}

        body = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
        body->lock_handle[0] = lock->l_remote_handle;
        ldlm_lock2desc(lock, &body->lock_desc);

	CLASSERT(sizeof(*ca) <= sizeof(req->rq_async_args));
	ca = ptlrpc_req_async_args(req);
	ca->ca_set_arg = arg;
	ca->ca_lock = lock;

        /* server namespace, doesn't need lock */
        req_capsule_set_size(&req->rq_pill, &RMF_DLM_LVB, RCL_SERVER,
                             ldlm_lvbo_size(lock));
        ptlrpc_request_set_replen(req);

        req->rq_send_state = LUSTRE_IMP_FULL;
        /* ptlrpc_request_alloc_pack already set timeout */
        if (AT_OFF)
                req->rq_timeout = ldlm_get_rq_timeout();

	req->rq_interpret_reply = ldlm_cb_interpret;

        if (lock->l_export && lock->l_export->exp_nid_stats &&
            lock->l_export->exp_nid_stats->nid_ldlm_stats)
                lprocfs_counter_incr(lock->l_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_GL_CALLBACK - LDLM_FIRST_OPC);

	rc = ldlm_ast_fini(req, arg, lock, 0);

	RETURN(rc);
}
EXPORT_SYMBOL(ldlm_server_glimpse_ast);

int ldlm_glimpse_locks(struct ldlm_resource *res, cfs_list_t *gl_work_list)
{
	int	rc;
	ENTRY;

	rc = ldlm_run_ast_work(ldlm_res_to_ns(res), gl_work_list,
			       LDLM_WORK_GL_AST);
	if (rc == -ERESTART)
		ldlm_reprocess_all(res);

	RETURN(rc);
}
EXPORT_SYMBOL(ldlm_glimpse_locks);

/* return LDLM lock associated with a lock callback request */
struct ldlm_lock *ldlm_request_lock(struct ptlrpc_request *req)
{
	struct ldlm_cb_async_args	*ca;
	struct ldlm_lock		*lock;
	ENTRY;

	ca = ptlrpc_req_async_args(req);
	lock = ca->ca_lock;
	if (lock == NULL)
		RETURN(ERR_PTR(-EFAULT));

	RETURN(lock);
}
EXPORT_SYMBOL(ldlm_request_lock);

static void ldlm_svc_get_eopc(const struct ldlm_request *dlm_req,
                       struct lprocfs_stats *srv_stats)
{
        int lock_type = 0, op = 0;

        lock_type = dlm_req->lock_desc.l_resource.lr_type;

        switch (lock_type) {
        case LDLM_PLAIN:
                op = PTLRPC_LAST_CNTR + LDLM_PLAIN_ENQUEUE;
                break;
        case LDLM_EXTENT:
                if (dlm_req->lock_flags & LDLM_FL_HAS_INTENT)
                        op = PTLRPC_LAST_CNTR + LDLM_GLIMPSE_ENQUEUE;
                else
                        op = PTLRPC_LAST_CNTR + LDLM_EXTENT_ENQUEUE;
                break;
        case LDLM_FLOCK:
                op = PTLRPC_LAST_CNTR + LDLM_FLOCK_ENQUEUE;
                break;
        case LDLM_IBITS:
                op = PTLRPC_LAST_CNTR + LDLM_IBITS_ENQUEUE;
                break;
        default:
                op = 0;
                break;
        }

        if (op)
                lprocfs_counter_incr(srv_stats, op);

        return;
}

/**
 * Main server-side entry point into LDLM for enqueue. This is called by ptlrpc
 * service threads to carry out client lock enqueueing requests.
 */
int ldlm_handle_enqueue0(struct ldlm_namespace *ns,
                         struct ptlrpc_request *req,
                         const struct ldlm_request *dlm_req,
                         const struct ldlm_callback_suite *cbs)
{
        struct ldlm_reply *dlm_rep;
	__u64 flags;
        ldlm_error_t err = ELDLM_OK;
        struct ldlm_lock *lock = NULL;
        void *cookie = NULL;
        int rc = 0;
        ENTRY;

        LDLM_DEBUG_NOLOCK("server-side enqueue handler START");

        ldlm_request_cancel(req, dlm_req, LDLM_ENQUEUE_CANCEL_OFF);
	flags = ldlm_flags_from_wire(dlm_req->lock_flags);

        LASSERT(req->rq_export);

	if (ptlrpc_req2svc(req)->srv_stats != NULL)
		ldlm_svc_get_eopc(dlm_req, ptlrpc_req2svc(req)->srv_stats);

        if (req->rq_export && req->rq_export->exp_nid_stats &&
            req->rq_export->exp_nid_stats->nid_ldlm_stats)
                lprocfs_counter_incr(req->rq_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_ENQUEUE - LDLM_FIRST_OPC);

        if (unlikely(dlm_req->lock_desc.l_resource.lr_type < LDLM_MIN_TYPE ||
                     dlm_req->lock_desc.l_resource.lr_type >= LDLM_MAX_TYPE)) {
                DEBUG_REQ(D_ERROR, req, "invalid lock request type %d",
                          dlm_req->lock_desc.l_resource.lr_type);
                GOTO(out, rc = -EFAULT);
        }

        if (unlikely(dlm_req->lock_desc.l_req_mode <= LCK_MINMODE ||
                     dlm_req->lock_desc.l_req_mode >= LCK_MAXMODE ||
                     dlm_req->lock_desc.l_req_mode &
                     (dlm_req->lock_desc.l_req_mode-1))) {
                DEBUG_REQ(D_ERROR, req, "invalid lock request mode %d",
                          dlm_req->lock_desc.l_req_mode);
                GOTO(out, rc = -EFAULT);
        }

	if (exp_connect_flags(req->rq_export) & OBD_CONNECT_IBITS) {
                if (unlikely(dlm_req->lock_desc.l_resource.lr_type ==
                             LDLM_PLAIN)) {
                        DEBUG_REQ(D_ERROR, req,
                                  "PLAIN lock request from IBITS client?");
                        GOTO(out, rc = -EPROTO);
                }
        } else if (unlikely(dlm_req->lock_desc.l_resource.lr_type ==
                            LDLM_IBITS)) {
                DEBUG_REQ(D_ERROR, req,
                          "IBITS lock request from unaware client?");
                GOTO(out, rc = -EPROTO);
        }

#if 0
        /* FIXME this makes it impossible to use LDLM_PLAIN locks -- check
           against server's _CONNECT_SUPPORTED flags? (I don't want to use
           ibits for mgc/mgs) */

        /* INODEBITS_INTEROP: Perform conversion from plain lock to
         * inodebits lock if client does not support them. */
	if (!(exp_connect_flags(req->rq_export) & OBD_CONNECT_IBITS) &&
            (dlm_req->lock_desc.l_resource.lr_type == LDLM_PLAIN)) {
                dlm_req->lock_desc.l_resource.lr_type = LDLM_IBITS;
                dlm_req->lock_desc.l_policy_data.l_inodebits.bits =
                        MDS_INODELOCK_LOOKUP | MDS_INODELOCK_UPDATE;
                if (dlm_req->lock_desc.l_req_mode == LCK_PR)
                        dlm_req->lock_desc.l_req_mode = LCK_CR;
        }
#endif

        if (unlikely(flags & LDLM_FL_REPLAY)) {
                /* Find an existing lock in the per-export lock hash */
		/* In the function below, .hs_keycmp resolves to
		 * ldlm_export_lock_keycmp() */
		/* coverity[overrun-buffer-val] */
                lock = cfs_hash_lookup(req->rq_export->exp_lock_hash,
                                       (void *)&dlm_req->lock_handle[0]);
                if (lock != NULL) {
                        DEBUG_REQ(D_DLMTRACE, req, "found existing lock cookie "
                                  LPX64, lock->l_handle.h_cookie);
                        GOTO(existing_lock, rc = 0);
                }
        }

        /* The lock's callback data might be set in the policy function */
        lock = ldlm_lock_create(ns, &dlm_req->lock_desc.l_resource.lr_name,
                                dlm_req->lock_desc.l_resource.lr_type,
                                dlm_req->lock_desc.l_req_mode,
				cbs, NULL, 0, LVB_T_NONE);
        if (!lock)
                GOTO(out, rc = -ENOMEM);

        lock->l_last_activity = cfs_time_current_sec();
        lock->l_remote_handle = dlm_req->lock_handle[0];
        LDLM_DEBUG(lock, "server-side enqueue handler, new lock created");

        OBD_FAIL_TIMEOUT(OBD_FAIL_LDLM_ENQUEUE_BLOCKED, obd_timeout * 2);
        /* Don't enqueue a lock onto the export if it is been disonnected
         * due to eviction (bug 3822) or server umount (bug 24324).
         * Cancel it now instead. */
        if (req->rq_export->exp_disconnected) {
                LDLM_ERROR(lock, "lock on disconnected export %p",
                           req->rq_export);
                GOTO(out, rc = -ENOTCONN);
        }

        lock->l_export = class_export_lock_get(req->rq_export, lock);
        if (lock->l_export->exp_lock_hash)
                cfs_hash_add(lock->l_export->exp_lock_hash,
                             &lock->l_remote_handle,
                             &lock->l_exp_hash);

existing_lock:

        if (flags & LDLM_FL_HAS_INTENT) {
                /* In this case, the reply buffer is allocated deep in
                 * local_lock_enqueue by the policy function. */
                cookie = req;
        } else {
                /* based on the assumption that lvb size never changes during
                 * resource life time otherwise it need resource->lr_lock's
                 * protection */
		req_capsule_set_size(&req->rq_pill, &RMF_DLM_LVB,
				     RCL_SERVER, ldlm_lvbo_size(lock));

                if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR))
                        GOTO(out, rc = -ENOMEM);

                rc = req_capsule_server_pack(&req->rq_pill);
                if (rc)
                        GOTO(out, rc);
        }

        if (dlm_req->lock_desc.l_resource.lr_type != LDLM_PLAIN)
                ldlm_convert_policy_to_local(req->rq_export,
                                          dlm_req->lock_desc.l_resource.lr_type,
                                          &dlm_req->lock_desc.l_policy_data,
                                          &lock->l_policy_data);
        if (dlm_req->lock_desc.l_resource.lr_type == LDLM_EXTENT)
                lock->l_req_extent = lock->l_policy_data.l_extent;

	err = ldlm_lock_enqueue(ns, &lock, cookie, &flags);
	if (err) {
		if ((int)err < 0)
			rc = (int)err;
		GOTO(out, err);
	}

        dlm_rep = req_capsule_server_get(&req->rq_pill, &RMF_DLM_REP);
	dlm_rep->lock_flags = ldlm_flags_to_wire(flags);

        ldlm_lock2desc(lock, &dlm_rep->lock_desc);
        ldlm_lock2handle(lock, &dlm_rep->lock_handle);

        /* We never send a blocking AST until the lock is granted, but
         * we can tell it right now */
        lock_res_and_lock(lock);

        /* Now take into account flags to be inherited from original lock
           request both in reply to client and in our own lock flags. */
        dlm_rep->lock_flags |= dlm_req->lock_flags & LDLM_INHERIT_FLAGS;
	lock->l_flags |= ldlm_flags_from_wire(dlm_req->lock_flags &
					      LDLM_INHERIT_FLAGS);

        /* Don't move a pending lock onto the export if it has already been
         * disconnected due to eviction (bug 5683) or server umount (bug 24324).
         * Cancel it now instead. */
        if (unlikely(req->rq_export->exp_disconnected ||
                     OBD_FAIL_CHECK(OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT))) {
                LDLM_ERROR(lock, "lock on destroyed export %p", req->rq_export);
                rc = -ENOTCONN;
        } else if (lock->l_flags & LDLM_FL_AST_SENT) {
		dlm_rep->lock_flags |= ldlm_flags_to_wire(LDLM_FL_AST_SENT);
                if (lock->l_granted_mode == lock->l_req_mode) {
                        /*
                         * Only cancel lock if it was granted, because it would
                         * be destroyed immediately and would never be granted
                         * in the future, causing timeouts on client.  Not
                         * granted lock will be cancelled immediately after
                         * sending completion AST.
                         */
                        if (dlm_rep->lock_flags & LDLM_FL_CANCEL_ON_BLOCK) {
                                unlock_res_and_lock(lock);
                                ldlm_lock_cancel(lock);
                                lock_res_and_lock(lock);
                        } else
                                ldlm_add_waiting_lock(lock);
                }
        }
        /* Make sure we never ever grant usual metadata locks to liblustre
           clients */
        if ((dlm_req->lock_desc.l_resource.lr_type == LDLM_PLAIN ||
            dlm_req->lock_desc.l_resource.lr_type == LDLM_IBITS) &&
             req->rq_export->exp_libclient) {
                if (unlikely(!(lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK) ||
                             !(dlm_rep->lock_flags & LDLM_FL_CANCEL_ON_BLOCK))){
                        CERROR("Granting sync lock to libclient. "
                               "req fl %d, rep fl %d, lock fl "LPX64"\n",
                               dlm_req->lock_flags, dlm_rep->lock_flags,
                               lock->l_flags);
                        LDLM_ERROR(lock, "sync lock");
                        if (dlm_req->lock_flags & LDLM_FL_HAS_INTENT) {
                                struct ldlm_intent *it;

                                it = req_capsule_client_get(&req->rq_pill,
                                                            &RMF_LDLM_INTENT);
                                if (it != NULL) {
                                        CERROR("This is intent %s ("LPU64")\n",
                                               ldlm_it2str(it->opc), it->opc);
                                }
                        }
                }
        }

        unlock_res_and_lock(lock);

        EXIT;
 out:
        req->rq_status = rc ?: err; /* return either error - bug 11190 */
        if (!req->rq_packed_final) {
                err = lustre_pack_reply(req, 1, NULL, NULL);
                if (rc == 0)
                        rc = err;
        }

        /* The LOCK_CHANGED code in ldlm_lock_enqueue depends on this
         * ldlm_reprocess_all.  If this moves, revisit that code. -phil */
        if (lock) {
                LDLM_DEBUG(lock, "server-side enqueue handler, sending reply"
                           "(err=%d, rc=%d)", err, rc);

                if (rc == 0) {
			if (req_capsule_has_field(&req->rq_pill, &RMF_DLM_LVB,
						  RCL_SERVER) &&
			    ldlm_lvbo_size(lock) > 0) {
				void *buf;
				int buflen;

				buf = req_capsule_server_get(&req->rq_pill,
							     &RMF_DLM_LVB);
				LASSERTF(buf != NULL, "req %p, lock %p\n",
					 req, lock);
				buflen = req_capsule_get_size(&req->rq_pill,
						&RMF_DLM_LVB, RCL_SERVER);
				buflen = ldlm_lvbo_fill(lock, buf, buflen);
				if (buflen >= 0)
					req_capsule_shrink(&req->rq_pill,
							   &RMF_DLM_LVB,
							   buflen, RCL_SERVER);
				else
					rc = buflen;
			}
                } else {
                        lock_res_and_lock(lock);
                        ldlm_resource_unlink_lock(lock);
                        ldlm_lock_destroy_nolock(lock);
                        unlock_res_and_lock(lock);
                }

                if (!err && dlm_req->lock_desc.l_resource.lr_type != LDLM_FLOCK)
                        ldlm_reprocess_all(lock->l_resource);

                LDLM_LOCK_RELEASE(lock);
        }

        LDLM_DEBUG_NOLOCK("server-side enqueue handler END (lock %p, rc %d)",
                          lock, rc);

        return rc;
}
EXPORT_SYMBOL(ldlm_handle_enqueue0);

/**
 * Old-style LDLM main entry point for server code enqueue.
 */
int ldlm_handle_enqueue(struct ptlrpc_request *req,
                        ldlm_completion_callback completion_callback,
                        ldlm_blocking_callback blocking_callback,
                        ldlm_glimpse_callback glimpse_callback)
{
        struct ldlm_request *dlm_req;
        struct ldlm_callback_suite cbs = {
                .lcs_completion = completion_callback,
                .lcs_blocking   = blocking_callback,
                .lcs_glimpse    = glimpse_callback
        };
        int rc;

        dlm_req = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
        if (dlm_req != NULL) {
                rc = ldlm_handle_enqueue0(req->rq_export->exp_obd->obd_namespace,
                                          req, dlm_req, &cbs);
        } else {
                rc = -EFAULT;
        }
        return rc;
}
EXPORT_SYMBOL(ldlm_handle_enqueue);

/**
 * Main LDLM entry point for server code to process lock conversion requests.
 */
int ldlm_handle_convert0(struct ptlrpc_request *req,
                         const struct ldlm_request *dlm_req)
{
        struct ldlm_reply *dlm_rep;
        struct ldlm_lock *lock;
        int rc;
        ENTRY;

        if (req->rq_export && req->rq_export->exp_nid_stats &&
            req->rq_export->exp_nid_stats->nid_ldlm_stats)
                lprocfs_counter_incr(req->rq_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_CONVERT - LDLM_FIRST_OPC);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                RETURN(rc);

        dlm_rep = req_capsule_server_get(&req->rq_pill, &RMF_DLM_REP);
        dlm_rep->lock_flags = dlm_req->lock_flags;

        lock = ldlm_handle2lock(&dlm_req->lock_handle[0]);
        if (!lock) {
                req->rq_status = EINVAL;
        } else {
                void *res = NULL;

                LDLM_DEBUG(lock, "server-side convert handler START");

                lock->l_last_activity = cfs_time_current_sec();
                res = ldlm_lock_convert(lock, dlm_req->lock_desc.l_req_mode,
                                        &dlm_rep->lock_flags);
                if (res) {
                        if (ldlm_del_waiting_lock(lock))
                                LDLM_DEBUG(lock, "converted waiting lock");
                        req->rq_status = 0;
                } else {
                        req->rq_status = EDEADLOCK;
                }
        }

        if (lock) {
                if (!req->rq_status)
                        ldlm_reprocess_all(lock->l_resource);
                LDLM_DEBUG(lock, "server-side convert handler END");
                LDLM_LOCK_PUT(lock);
        } else
                LDLM_DEBUG_NOLOCK("server-side convert handler END");

        RETURN(0);
}
EXPORT_SYMBOL(ldlm_handle_convert0);

/**
 * Old-style main LDLM entry point for server code to process lock conversion
 * requests.
 */
int ldlm_handle_convert(struct ptlrpc_request *req)
{
        int rc;
        struct ldlm_request *dlm_req;

        dlm_req = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
        if (dlm_req != NULL) {
                rc = ldlm_handle_convert0(req, dlm_req);
        } else {
                CERROR ("Can't unpack dlm_req\n");
                rc = -EFAULT;
        }
        return rc;
}
EXPORT_SYMBOL(ldlm_handle_convert);

/**
 * Cancel all the locks whose handles are packed into ldlm_request
 *
 * Called by server code expecting such combined cancel activity
 * requests.
 */
int ldlm_request_cancel(struct ptlrpc_request *req,
                        const struct ldlm_request *dlm_req, int first)
{
        struct ldlm_resource *res, *pres = NULL;
        struct ldlm_lock *lock;
        int i, count, done = 0;
        ENTRY;

        count = dlm_req->lock_count ? dlm_req->lock_count : 1;
        if (first >= count)
                RETURN(0);

	if (count == 1 && dlm_req->lock_handle[0].cookie == 0)
		RETURN(0);

        /* There is no lock on the server at the replay time,
         * skip lock cancelling to make replay tests to pass. */
        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY)
                RETURN(0);

        LDLM_DEBUG_NOLOCK("server-side cancel handler START: %d locks, "
                          "starting at %d", count, first);

        for (i = first; i < count; i++) {
                lock = ldlm_handle2lock(&dlm_req->lock_handle[i]);
                if (!lock) {
                        LDLM_DEBUG_NOLOCK("server-side cancel handler stale "
                                          "lock (cookie "LPU64")",
                                          dlm_req->lock_handle[i].cookie);
                        continue;
                }

                res = lock->l_resource;
                done++;

		/* This code is an optimization to only attempt lock
		 * granting on the resource (that could be CPU-expensive)
		 * after we are done cancelling lock in that resource. */
                if (res != pres) {
                        if (pres != NULL) {
                                ldlm_reprocess_all(pres);
                                LDLM_RESOURCE_DELREF(pres);
                                ldlm_resource_putref(pres);
                        }
                        if (res != NULL) {
                                ldlm_resource_getref(res);
                                LDLM_RESOURCE_ADDREF(res);
                                ldlm_res_lvbo_update(res, NULL, 1);
                        }
                        pres = res;
                }
                ldlm_lock_cancel(lock);
                LDLM_LOCK_PUT(lock);
        }
        if (pres != NULL) {
                ldlm_reprocess_all(pres);
                LDLM_RESOURCE_DELREF(pres);
                ldlm_resource_putref(pres);
        }
        LDLM_DEBUG_NOLOCK("server-side cancel handler END");
        RETURN(done);
}
EXPORT_SYMBOL(ldlm_request_cancel);

/**
 * Main LDLM entry point for server code to cancel locks.
 *
 * Typically gets called from service handler on LDLM_CANCEL opc.
 */
int ldlm_handle_cancel(struct ptlrpc_request *req)
{
        struct ldlm_request *dlm_req;
        int rc;
        ENTRY;

        dlm_req = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
        if (dlm_req == NULL) {
                CDEBUG(D_INFO, "bad request buffer for cancel\n");
                RETURN(-EFAULT);
        }

        if (req->rq_export && req->rq_export->exp_nid_stats &&
            req->rq_export->exp_nid_stats->nid_ldlm_stats)
                lprocfs_counter_incr(req->rq_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_CANCEL - LDLM_FIRST_OPC);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                RETURN(rc);

        if (!ldlm_request_cancel(req, dlm_req, 0))
                req->rq_status = ESTALE;

        RETURN(ptlrpc_reply(req));
}
EXPORT_SYMBOL(ldlm_handle_cancel);
#endif /* HAVE_SERVER_SUPPORT */

/**
 * Callback handler for receiving incoming blocking ASTs.
 *
 * This can only happen on client side.
 */
void ldlm_handle_bl_callback(struct ldlm_namespace *ns,
                             struct ldlm_lock_desc *ld, struct ldlm_lock *lock)
{
        int do_ast;
        ENTRY;

        LDLM_DEBUG(lock, "client blocking AST callback handler");

        lock_res_and_lock(lock);
        lock->l_flags |= LDLM_FL_CBPENDING;

        if (lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK)
                lock->l_flags |= LDLM_FL_CANCEL;

        do_ast = (!lock->l_readers && !lock->l_writers);
        unlock_res_and_lock(lock);

        if (do_ast) {
                CDEBUG(D_DLMTRACE, "Lock %p already unused, calling callback (%p)\n",
                       lock, lock->l_blocking_ast);
                if (lock->l_blocking_ast != NULL)
                        lock->l_blocking_ast(lock, ld, lock->l_ast_data,
                                             LDLM_CB_BLOCKING);
        } else {
                CDEBUG(D_DLMTRACE, "Lock %p is referenced, will be cancelled later\n",
                       lock);
        }

        LDLM_DEBUG(lock, "client blocking callback handler END");
        LDLM_LOCK_RELEASE(lock);
        EXIT;
}

/**
 * Callback handler for receiving incoming completion ASTs.
 *
 * This only can happen on client side.
 */
static void ldlm_handle_cp_callback(struct ptlrpc_request *req,
                                    struct ldlm_namespace *ns,
                                    struct ldlm_request *dlm_req,
                                    struct ldlm_lock *lock)
{
	int lvb_len;
        CFS_LIST_HEAD(ast_list);
	int rc = 0;
        ENTRY;

        LDLM_DEBUG(lock, "client completion callback handler START");

        if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_CANCEL_BL_CB_RACE)) {
                int to = cfs_time_seconds(1);
                while (to > 0) {
                        cfs_schedule_timeout_and_set_state(
                                CFS_TASK_INTERRUPTIBLE, to);
                        if (lock->l_granted_mode == lock->l_req_mode ||
                            lock->l_destroyed)
                                break;
                }
        }

	lvb_len = req_capsule_get_size(&req->rq_pill, &RMF_DLM_LVB, RCL_CLIENT);
	if (lvb_len < 0) {
		LDLM_ERROR(lock, "Fail to get lvb_len, rc = %d", lvb_len);
		GOTO(out, rc = lvb_len);
	} else if (lvb_len > 0) {
		if (lock->l_lvb_len > 0) {
			/* for extent lock, lvb contains ost_lvb{}. */
			LASSERT(lock->l_lvb_data != NULL);

			if (unlikely(lock->l_lvb_len < lvb_len)) {
				LDLM_ERROR(lock, "Replied LVB is larger than "
					   "expectation, expected = %d, "
					   "replied = %d",
					   lock->l_lvb_len, lvb_len);
				GOTO(out, rc = -EINVAL);
			}
		} else if (ldlm_has_layout(lock)) { /* for layout lock, lvb has
						     * variable length */
			void *lvb_data;

			OBD_ALLOC(lvb_data, lvb_len);
			if (lvb_data == NULL) {
				LDLM_ERROR(lock, "No memory: %d.\n", lvb_len);
				GOTO(out, rc = -ENOMEM);
			}

			lock_res_and_lock(lock);
			LASSERT(lock->l_lvb_data == NULL);
			lock->l_lvb_data = lvb_data;
			lock->l_lvb_len = lvb_len;
			unlock_res_and_lock(lock);
		}
	}

        lock_res_and_lock(lock);
        if (lock->l_destroyed ||
            lock->l_granted_mode == lock->l_req_mode) {
                /* bug 11300: the lock has already been granted */
                unlock_res_and_lock(lock);
                LDLM_DEBUG(lock, "Double grant race happened");
		GOTO(out, rc = 0);
        }

        /* If we receive the completion AST before the actual enqueue returned,
         * then we might need to switch lock modes, resources, or extents. */
        if (dlm_req->lock_desc.l_granted_mode != lock->l_req_mode) {
                lock->l_req_mode = dlm_req->lock_desc.l_granted_mode;
                LDLM_DEBUG(lock, "completion AST, new lock mode");
        }

        if (lock->l_resource->lr_type != LDLM_PLAIN) {
                ldlm_convert_policy_to_local(req->rq_export,
                                          dlm_req->lock_desc.l_resource.lr_type,
                                          &dlm_req->lock_desc.l_policy_data,
                                          &lock->l_policy_data);
                LDLM_DEBUG(lock, "completion AST, new policy data");
        }

        ldlm_resource_unlink_lock(lock);
        if (memcmp(&dlm_req->lock_desc.l_resource.lr_name,
                   &lock->l_resource->lr_name,
                   sizeof(lock->l_resource->lr_name)) != 0) {
                unlock_res_and_lock(lock);
		rc = ldlm_lock_change_resource(ns, lock,
				&dlm_req->lock_desc.l_resource.lr_name);
		if (rc < 0) {
			LDLM_ERROR(lock, "Failed to allocate resource");
			GOTO(out, rc);
		}
                LDLM_DEBUG(lock, "completion AST, new resource");
                CERROR("change resource!\n");
                lock_res_and_lock(lock);
        }

        if (dlm_req->lock_flags & LDLM_FL_AST_SENT) {
		/* BL_AST locks are not needed in LRU.
		 * Let ldlm_cancel_lru() be fast. */
                ldlm_lock_remove_from_lru(lock);
                lock->l_flags |= LDLM_FL_CBPENDING | LDLM_FL_BL_AST;
                LDLM_DEBUG(lock, "completion AST includes blocking AST");
        }

	if (lock->l_lvb_len > 0) {
		rc = ldlm_fill_lvb(lock, &req->rq_pill, RCL_CLIENT,
				   lock->l_lvb_data, lvb_len);
		if (rc < 0) {
			unlock_res_and_lock(lock);
			GOTO(out, rc);
		}
	}

        ldlm_grant_lock(lock, &ast_list);
        unlock_res_and_lock(lock);

        LDLM_DEBUG(lock, "callback handler finished, about to run_ast_work");

        /* Let Enqueue to call osc_lock_upcall() and initialize
         * l_ast_data */
        OBD_FAIL_TIMEOUT(OBD_FAIL_OSC_CP_ENQ_RACE, 2);

        ldlm_run_ast_work(ns, &ast_list, LDLM_WORK_CP_AST);

        LDLM_DEBUG_NOLOCK("client completion callback handler END (lock %p)",
                          lock);
	GOTO(out, rc);

out:
	if (rc < 0) {
		lock_res_and_lock(lock);
		lock->l_flags |= LDLM_FL_FAILED;
		unlock_res_and_lock(lock);
		cfs_waitq_signal(&lock->l_waitq);
	}
	LDLM_LOCK_RELEASE(lock);
}

/**
 * Callback handler for receiving incoming glimpse ASTs.
 *
 * This only can happen on client side.  After handling the glimpse AST
 * we also consider dropping the lock here if it is unused locally for a
 * long time.
 */
static void ldlm_handle_gl_callback(struct ptlrpc_request *req,
                                    struct ldlm_namespace *ns,
                                    struct ldlm_request *dlm_req,
                                    struct ldlm_lock *lock)
{
        int rc = -ENOSYS;
        ENTRY;

        LDLM_DEBUG(lock, "client glimpse AST callback handler");

        if (lock->l_glimpse_ast != NULL)
                rc = lock->l_glimpse_ast(lock, req);

        if (req->rq_repmsg != NULL) {
                ptlrpc_reply(req);
        } else {
                req->rq_status = rc;
                ptlrpc_error(req);
        }

        lock_res_and_lock(lock);
        if (lock->l_granted_mode == LCK_PW &&
            !lock->l_readers && !lock->l_writers &&
            cfs_time_after(cfs_time_current(),
                           cfs_time_add(lock->l_last_used,
                                        cfs_time_seconds(10)))) {
                unlock_res_and_lock(lock);
                if (ldlm_bl_to_thread_lock(ns, NULL, lock))
                        ldlm_handle_bl_callback(ns, NULL, lock);

                EXIT;
                return;
        }
        unlock_res_and_lock(lock);
        LDLM_LOCK_RELEASE(lock);
        EXIT;
}

static int ldlm_callback_reply(struct ptlrpc_request *req, int rc)
{
        if (req->rq_no_reply)
                return 0;

        req->rq_status = rc;
        if (!req->rq_packed_final) {
                rc = lustre_pack_reply(req, 1, NULL, NULL);
                if (rc)
                        return rc;
        }
        return ptlrpc_reply(req);
}

#ifdef __KERNEL__
static int __ldlm_bl_to_thread(struct ldlm_bl_work_item *blwi,
			       ldlm_cancel_flags_t cancel_flags)
{
	struct ldlm_bl_pool *blp = ldlm_state->ldlm_bl_pool;
	ENTRY;

	spin_lock(&blp->blp_lock);
	if (blwi->blwi_lock &&
	    blwi->blwi_lock->l_flags & LDLM_FL_DISCARD_DATA) {
		/* add LDLM_FL_DISCARD_DATA requests to the priority list */
		cfs_list_add_tail(&blwi->blwi_entry, &blp->blp_prio_list);
	} else {
		/* other blocking callbacks are added to the regular list */
		cfs_list_add_tail(&blwi->blwi_entry, &blp->blp_list);
	}
	spin_unlock(&blp->blp_lock);

	cfs_waitq_signal(&blp->blp_waitq);

	/* can not check blwi->blwi_flags as blwi could be already freed in
	   LCF_ASYNC mode */
	if (!(cancel_flags & LCF_ASYNC))
		wait_for_completion(&blwi->blwi_comp);

	RETURN(0);
}

static inline void init_blwi(struct ldlm_bl_work_item *blwi,
			     struct ldlm_namespace *ns,
			     struct ldlm_lock_desc *ld,
			     cfs_list_t *cancels, int count,
			     struct ldlm_lock *lock,
			     ldlm_cancel_flags_t cancel_flags)
{
	init_completion(&blwi->blwi_comp);
        CFS_INIT_LIST_HEAD(&blwi->blwi_head);

        if (cfs_memory_pressure_get())
                blwi->blwi_mem_pressure = 1;

        blwi->blwi_ns = ns;
	blwi->blwi_flags = cancel_flags;
        if (ld != NULL)
                blwi->blwi_ld = *ld;
        if (count) {
                cfs_list_add(&blwi->blwi_head, cancels);
                cfs_list_del_init(cancels);
                blwi->blwi_count = count;
        } else {
                blwi->blwi_lock = lock;
        }
}

/**
 * Queues a list of locks \a cancels containing \a count locks
 * for later processing by a blocking thread.  If \a count is zero,
 * then the lock referenced as \a lock is queued instead.
 *
 * The blocking thread would then call ->l_blocking_ast callback in the lock.
 * If list addition fails an error is returned and caller is supposed to
 * call ->l_blocking_ast itself.
 */
static int ldlm_bl_to_thread(struct ldlm_namespace *ns,
			     struct ldlm_lock_desc *ld,
			     struct ldlm_lock *lock,
			     cfs_list_t *cancels, int count,
			     ldlm_cancel_flags_t cancel_flags)
{
	ENTRY;

	if (cancels && count == 0)
		RETURN(0);

	if (cancel_flags & LCF_ASYNC) {
		struct ldlm_bl_work_item *blwi;

		OBD_ALLOC(blwi, sizeof(*blwi));
		if (blwi == NULL)
			RETURN(-ENOMEM);
		init_blwi(blwi, ns, ld, cancels, count, lock, cancel_flags);

		RETURN(__ldlm_bl_to_thread(blwi, cancel_flags));
	} else {
		/* if it is synchronous call do minimum mem alloc, as it could
		 * be triggered from kernel shrinker
		 */
		struct ldlm_bl_work_item blwi;

		memset(&blwi, 0, sizeof(blwi));
		init_blwi(&blwi, ns, ld, cancels, count, lock, cancel_flags);
		RETURN(__ldlm_bl_to_thread(&blwi, cancel_flags));
	}
}

#endif

int ldlm_bl_to_thread_lock(struct ldlm_namespace *ns, struct ldlm_lock_desc *ld,
			   struct ldlm_lock *lock)
{
#ifdef __KERNEL__
	return ldlm_bl_to_thread(ns, ld, lock, NULL, 0, LCF_ASYNC);
#else
	return -ENOSYS;
#endif
}

int ldlm_bl_to_thread_list(struct ldlm_namespace *ns, struct ldlm_lock_desc *ld,
			   cfs_list_t *cancels, int count,
			   ldlm_cancel_flags_t cancel_flags)
{
#ifdef __KERNEL__
	return ldlm_bl_to_thread(ns, ld, NULL, cancels, count, cancel_flags);
#else
	return -ENOSYS;
#endif
}

/* Setinfo coming from Server (eg MDT) to Client (eg MDC)! */
static int ldlm_handle_setinfo(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        char *key;
        void *val;
        int keylen, vallen;
        int rc = -ENOSYS;
        ENTRY;

        DEBUG_REQ(D_HSM, req, "%s: handle setinfo\n", obd->obd_name);

        req_capsule_set(&req->rq_pill, &RQF_OBD_SET_INFO);

        key = req_capsule_client_get(&req->rq_pill, &RMF_SETINFO_KEY);
        if (key == NULL) {
                DEBUG_REQ(D_IOCTL, req, "no set_info key");
                RETURN(-EFAULT);
        }
        keylen = req_capsule_get_size(&req->rq_pill, &RMF_SETINFO_KEY,
                                      RCL_CLIENT);
        val = req_capsule_client_get(&req->rq_pill, &RMF_SETINFO_VAL);
        if (val == NULL) {
                DEBUG_REQ(D_IOCTL, req, "no set_info val");
                RETURN(-EFAULT);
        }
        vallen = req_capsule_get_size(&req->rq_pill, &RMF_SETINFO_VAL,
                                      RCL_CLIENT);

        /* We are responsible for swabbing contents of val */

        if (KEY_IS(KEY_HSM_COPYTOOL_SEND))
                /* Pass it on to mdc (the "export" in this case) */
                rc = obd_set_info_async(req->rq_svc_thread->t_env,
                                        req->rq_export,
                                        sizeof(KEY_HSM_COPYTOOL_SEND),
                                        KEY_HSM_COPYTOOL_SEND,
                                        vallen, val, NULL);
        else
                DEBUG_REQ(D_WARNING, req, "ignoring unknown key %s", key);

        return rc;
}

static inline void ldlm_callback_errmsg(struct ptlrpc_request *req,
                                        const char *msg, int rc,
                                        struct lustre_handle *handle)
{
        DEBUG_REQ((req->rq_no_reply || rc) ? D_WARNING : D_DLMTRACE, req,
                  "%s: [nid %s] [rc %d] [lock "LPX64"]",
                  msg, libcfs_id2str(req->rq_peer), rc,
                  handle ? handle->cookie : 0);
        if (req->rq_no_reply)
                CWARN("No reply was sent, maybe cause bug 21636.\n");
        else if (rc)
                CWARN("Send reply failed, maybe cause bug 21636.\n");
}

static int ldlm_handle_qc_callback(struct ptlrpc_request *req)
{
	struct obd_quotactl *oqctl;
	struct client_obd *cli = &req->rq_export->exp_obd->u.cli;

	oqctl = req_capsule_client_get(&req->rq_pill, &RMF_OBD_QUOTACTL);
	if (oqctl == NULL) {
		CERROR("Can't unpack obd_quotactl\n");
		RETURN(-EPROTO);
	}

	cli->cl_qchk_stat = oqctl->qc_stat;
	return 0;
}

/* TODO: handle requests in a similar way as MDT: see mdt_handle_common() */
static int ldlm_callback_handler(struct ptlrpc_request *req)
{
        struct ldlm_namespace *ns;
        struct ldlm_request *dlm_req;
        struct ldlm_lock *lock;
        int rc;
        ENTRY;

        /* Requests arrive in sender's byte order.  The ptlrpc service
         * handler has already checked and, if necessary, byte-swapped the
         * incoming request message body, but I am responsible for the
         * message buffers. */

        /* do nothing for sec context finalize */
        if (lustre_msg_get_opc(req->rq_reqmsg) == SEC_CTX_FINI)
                RETURN(0);

        req_capsule_init(&req->rq_pill, req, RCL_SERVER);

        if (req->rq_export == NULL) {
                rc = ldlm_callback_reply(req, -ENOTCONN);
                ldlm_callback_errmsg(req, "Operate on unconnected server",
                                     rc, NULL);
                RETURN(0);
        }

        LASSERT(req->rq_export != NULL);
        LASSERT(req->rq_export->exp_obd != NULL);

	switch (lustre_msg_get_opc(req->rq_reqmsg)) {
	case LDLM_BL_CALLBACK:
		if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_BL_CALLBACK_NET))
			RETURN(0);
		break;
	case LDLM_CP_CALLBACK:
		if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_CP_CALLBACK_NET))
			RETURN(0);
		break;
	case LDLM_GL_CALLBACK:
		if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_GL_CALLBACK_NET))
			RETURN(0);
		break;
        case LDLM_SET_INFO:
                rc = ldlm_handle_setinfo(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case OBD_LOG_CANCEL: /* remove this eventually - for 1.4.0 compat */
                CERROR("shouldn't be handling OBD_LOG_CANCEL on DLM thread\n");
                req_capsule_set(&req->rq_pill, &RQF_LOG_CANCEL);
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LOG_CANCEL_NET))
                        RETURN(0);
                rc = llog_origin_handle_cancel(req);
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LOG_CANCEL_REP))
                        RETURN(0);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case LLOG_ORIGIN_HANDLE_CREATE:
                req_capsule_set(&req->rq_pill, &RQF_LLOG_ORIGIN_HANDLE_CREATE);
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LOGD_NET))
                        RETURN(0);
		rc = llog_origin_handle_open(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case LLOG_ORIGIN_HANDLE_NEXT_BLOCK:
                req_capsule_set(&req->rq_pill,
                                &RQF_LLOG_ORIGIN_HANDLE_NEXT_BLOCK);
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LOGD_NET))
                        RETURN(0);
                rc = llog_origin_handle_next_block(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case LLOG_ORIGIN_HANDLE_READ_HEADER:
                req_capsule_set(&req->rq_pill,
                                &RQF_LLOG_ORIGIN_HANDLE_READ_HEADER);
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LOGD_NET))
                        RETURN(0);
                rc = llog_origin_handle_read_header(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case LLOG_ORIGIN_HANDLE_CLOSE:
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LOGD_NET))
                        RETURN(0);
                rc = llog_origin_handle_close(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
	case OBD_QC_CALLBACK:
		req_capsule_set(&req->rq_pill, &RQF_QC_CALLBACK);
		if (OBD_FAIL_CHECK(OBD_FAIL_OBD_QC_CALLBACK_NET))
			RETURN(0);
		rc = ldlm_handle_qc_callback(req);
		ldlm_callback_reply(req, rc);
		RETURN(0);
        default:
                CERROR("unknown opcode %u\n",
                       lustre_msg_get_opc(req->rq_reqmsg));
                ldlm_callback_reply(req, -EPROTO);
                RETURN(0);
        }

        ns = req->rq_export->exp_obd->obd_namespace;
        LASSERT(ns != NULL);

        req_capsule_set(&req->rq_pill, &RQF_LDLM_CALLBACK);

        dlm_req = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
        if (dlm_req == NULL) {
                rc = ldlm_callback_reply(req, -EPROTO);
                ldlm_callback_errmsg(req, "Operate without parameter", rc,
                                     NULL);
                RETURN(0);
        }

        /* Force a known safe race, send a cancel to the server for a lock
         * which the server has already started a blocking callback on. */
        if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_CANCEL_BL_CB_RACE) &&
            lustre_msg_get_opc(req->rq_reqmsg) == LDLM_BL_CALLBACK) {
		rc = ldlm_cli_cancel(&dlm_req->lock_handle[0], 0);
                if (rc < 0)
                        CERROR("ldlm_cli_cancel: %d\n", rc);
        }

        lock = ldlm_handle2lock_long(&dlm_req->lock_handle[0], 0);
        if (!lock) {
                CDEBUG(D_DLMTRACE, "callback on lock "LPX64" - lock "
                       "disappeared\n", dlm_req->lock_handle[0].cookie);
                rc = ldlm_callback_reply(req, -EINVAL);
                ldlm_callback_errmsg(req, "Operate with invalid parameter", rc,
                                     &dlm_req->lock_handle[0]);
                RETURN(0);
        }

        if ((lock->l_flags & LDLM_FL_FAIL_LOC) &&
            lustre_msg_get_opc(req->rq_reqmsg) == LDLM_BL_CALLBACK)
                OBD_RACE(OBD_FAIL_LDLM_CP_BL_RACE);

        /* Copy hints/flags (e.g. LDLM_FL_DISCARD_DATA) from AST. */
        lock_res_and_lock(lock);
	lock->l_flags |= ldlm_flags_from_wire(dlm_req->lock_flags &
					      LDLM_AST_FLAGS);
        if (lustre_msg_get_opc(req->rq_reqmsg) == LDLM_BL_CALLBACK) {
                /* If somebody cancels lock and cache is already dropped,
                 * or lock is failed before cp_ast received on client,
                 * we can tell the server we have no lock. Otherwise, we
                 * should send cancel after dropping the cache. */
                if (((lock->l_flags & LDLM_FL_CANCELING) &&
                    (lock->l_flags & LDLM_FL_BL_DONE)) ||
                    (lock->l_flags & LDLM_FL_FAILED)) {
                        LDLM_DEBUG(lock, "callback on lock "
                                   LPX64" - lock disappeared\n",
                                   dlm_req->lock_handle[0].cookie);
                        unlock_res_and_lock(lock);
                        LDLM_LOCK_RELEASE(lock);
                        rc = ldlm_callback_reply(req, -EINVAL);
                        ldlm_callback_errmsg(req, "Operate on stale lock", rc,
                                             &dlm_req->lock_handle[0]);
                        RETURN(0);
                }
		/* BL_AST locks are not needed in LRU.
		 * Let ldlm_cancel_lru() be fast. */
                ldlm_lock_remove_from_lru(lock);
                lock->l_flags |= LDLM_FL_BL_AST;
        }
        unlock_res_and_lock(lock);

        /* We want the ost thread to get this reply so that it can respond
         * to ost requests (write cache writeback) that might be triggered
         * in the callback.
         *
         * But we'd also like to be able to indicate in the reply that we're
         * cancelling right now, because it's unused, or have an intent result
         * in the reply, so we might have to push the responsibility for sending
         * the reply down into the AST handlers, alas. */

        switch (lustre_msg_get_opc(req->rq_reqmsg)) {
        case LDLM_BL_CALLBACK:
                CDEBUG(D_INODE, "blocking ast\n");
                req_capsule_extend(&req->rq_pill, &RQF_LDLM_BL_CALLBACK);
                if (!(lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK)) {
                        rc = ldlm_callback_reply(req, 0);
                        if (req->rq_no_reply || rc)
                                ldlm_callback_errmsg(req, "Normal process", rc,
                                                     &dlm_req->lock_handle[0]);
                }
                if (ldlm_bl_to_thread_lock(ns, &dlm_req->lock_desc, lock))
                        ldlm_handle_bl_callback(ns, &dlm_req->lock_desc, lock);
                break;
        case LDLM_CP_CALLBACK:
                CDEBUG(D_INODE, "completion ast\n");
                req_capsule_extend(&req->rq_pill, &RQF_LDLM_CP_CALLBACK);
                ldlm_callback_reply(req, 0);
                ldlm_handle_cp_callback(req, ns, dlm_req, lock);
                break;
        case LDLM_GL_CALLBACK:
                CDEBUG(D_INODE, "glimpse ast\n");
                req_capsule_extend(&req->rq_pill, &RQF_LDLM_GL_CALLBACK);
                ldlm_handle_gl_callback(req, ns, dlm_req, lock);
                break;
        default:
                LBUG();                         /* checked above */
        }

        RETURN(0);
}

#ifdef HAVE_SERVER_SUPPORT
/**
 * Main handler for canceld thread.
 *
 * Separated into its own thread to avoid deadlocks.
 */
static int ldlm_cancel_handler(struct ptlrpc_request *req)
{
        int rc;
        ENTRY;

        /* Requests arrive in sender's byte order.  The ptlrpc service
         * handler has already checked and, if necessary, byte-swapped the
         * incoming request message body, but I am responsible for the
         * message buffers. */

        req_capsule_init(&req->rq_pill, req, RCL_SERVER);

        if (req->rq_export == NULL) {
                struct ldlm_request *dlm_req;

                CERROR("%s from %s arrived at %lu with bad export cookie "
                       LPU64"\n",
                       ll_opcode2str(lustre_msg_get_opc(req->rq_reqmsg)),
                       libcfs_nid2str(req->rq_peer.nid),
                       req->rq_arrival_time.tv_sec,
                       lustre_msg_get_handle(req->rq_reqmsg)->cookie);

                if (lustre_msg_get_opc(req->rq_reqmsg) == LDLM_CANCEL) {
                        req_capsule_set(&req->rq_pill, &RQF_LDLM_CALLBACK);
                        dlm_req = req_capsule_client_get(&req->rq_pill,
                                                         &RMF_DLM_REQ);
                        if (dlm_req != NULL)
                                ldlm_lock_dump_handle(D_ERROR,
                                                      &dlm_req->lock_handle[0]);
                }
                ldlm_callback_reply(req, -ENOTCONN);
                RETURN(0);
        }

        switch (lustre_msg_get_opc(req->rq_reqmsg)) {

        /* XXX FIXME move this back to mds/handler.c, bug 249 */
        case LDLM_CANCEL:
                req_capsule_set(&req->rq_pill, &RQF_LDLM_CANCEL);
                CDEBUG(D_INODE, "cancel\n");
		if (CFS_FAIL_CHECK(OBD_FAIL_LDLM_CANCEL_NET) ||
		    CFS_FAIL_CHECK(OBD_FAIL_PTLRPC_CANCEL_RESEND))
			RETURN(0);
                rc = ldlm_handle_cancel(req);
                if (rc)
                        break;
                RETURN(0);
        case OBD_LOG_CANCEL:
                req_capsule_set(&req->rq_pill, &RQF_LOG_CANCEL);
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LOG_CANCEL_NET))
                        RETURN(0);
                rc = llog_origin_handle_cancel(req);
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LOG_CANCEL_REP))
                        RETURN(0);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        default:
                CERROR("invalid opcode %d\n",
                       lustre_msg_get_opc(req->rq_reqmsg));
                req_capsule_set(&req->rq_pill, &RQF_LDLM_CALLBACK);
                ldlm_callback_reply(req, -EINVAL);
        }

        RETURN(0);
}

static int ldlm_cancel_hpreq_lock_match(struct ptlrpc_request *req,
                                        struct ldlm_lock *lock)
{
        struct ldlm_request *dlm_req;
        struct lustre_handle lockh;
        int rc = 0;
        int i;
        ENTRY;

        dlm_req = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
        if (dlm_req == NULL)
                RETURN(0);

        ldlm_lock2handle(lock, &lockh);
        for (i = 0; i < dlm_req->lock_count; i++) {
                if (lustre_handle_equal(&dlm_req->lock_handle[i],
                                        &lockh)) {
                        DEBUG_REQ(D_RPCTRACE, req,
                                  "Prio raised by lock "LPX64".", lockh.cookie);

                        rc = 1;
                        break;
                }
        }

        RETURN(rc);

}

static int ldlm_cancel_hpreq_check(struct ptlrpc_request *req)
{
        struct ldlm_request *dlm_req;
        int rc = 0;
        int i;
        ENTRY;

        /* no prolong in recovery */
        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY)
                RETURN(0);

        dlm_req = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
        if (dlm_req == NULL)
                RETURN(-EFAULT);

        for (i = 0; i < dlm_req->lock_count; i++) {
                struct ldlm_lock *lock;

                lock = ldlm_handle2lock(&dlm_req->lock_handle[i]);
                if (lock == NULL)
                        continue;

                rc = !!(lock->l_flags & LDLM_FL_AST_SENT);
                if (rc)
                        LDLM_DEBUG(lock, "hpreq cancel lock");
                LDLM_LOCK_PUT(lock);

                if (rc)
                        break;
        }

        RETURN(rc);
}

static struct ptlrpc_hpreq_ops ldlm_cancel_hpreq_ops = {
        .hpreq_lock_match = ldlm_cancel_hpreq_lock_match,
	.hpreq_check      = ldlm_cancel_hpreq_check,
	.hpreq_fini       = NULL,
};

static int ldlm_hpreq_handler(struct ptlrpc_request *req)
{
        ENTRY;

        req_capsule_init(&req->rq_pill, req, RCL_SERVER);

        if (req->rq_export == NULL)
                RETURN(0);

        if (LDLM_CANCEL == lustre_msg_get_opc(req->rq_reqmsg)) {
                req_capsule_set(&req->rq_pill, &RQF_LDLM_CANCEL);
                req->rq_ops = &ldlm_cancel_hpreq_ops;
        }
        RETURN(0);
}

int ldlm_revoke_lock_cb(cfs_hash_t *hs, cfs_hash_bd_t *bd,
                        cfs_hlist_node_t *hnode, void *data)

{
        cfs_list_t         *rpc_list = data;
        struct ldlm_lock   *lock = cfs_hash_object(hs, hnode);

        lock_res_and_lock(lock);

        if (lock->l_req_mode != lock->l_granted_mode) {
                unlock_res_and_lock(lock);
                return 0;
        }

        LASSERT(lock->l_resource);
        if (lock->l_resource->lr_type != LDLM_IBITS &&
            lock->l_resource->lr_type != LDLM_PLAIN) {
                unlock_res_and_lock(lock);
                return 0;
        }

        if (lock->l_flags & LDLM_FL_AST_SENT) {
                unlock_res_and_lock(lock);
                return 0;
        }

        LASSERT(lock->l_blocking_ast);
        LASSERT(!lock->l_blocking_lock);

        lock->l_flags |= LDLM_FL_AST_SENT;
        if (lock->l_export && lock->l_export->exp_lock_hash) {
		/* NB: it's safe to call cfs_hash_del() even lock isn't
		 * in exp_lock_hash. */
		/* In the function below, .hs_keycmp resolves to
		 * ldlm_export_lock_keycmp() */
		/* coverity[overrun-buffer-val] */
		cfs_hash_del(lock->l_export->exp_lock_hash,
			     &lock->l_remote_handle, &lock->l_exp_hash);
	}

        cfs_list_add_tail(&lock->l_rk_ast, rpc_list);
        LDLM_LOCK_GET(lock);

        unlock_res_and_lock(lock);
        return 0;
}

void ldlm_revoke_export_locks(struct obd_export *exp)
{
        cfs_list_t  rpc_list;
        ENTRY;

        CFS_INIT_LIST_HEAD(&rpc_list);
        cfs_hash_for_each_empty(exp->exp_lock_hash,
                                ldlm_revoke_lock_cb, &rpc_list);
        ldlm_run_ast_work(exp->exp_obd->obd_namespace, &rpc_list,
                          LDLM_WORK_REVOKE_AST);

        EXIT;
}
EXPORT_SYMBOL(ldlm_revoke_export_locks);
#endif /* HAVE_SERVER_SUPPORT */

#ifdef __KERNEL__
static struct ldlm_bl_work_item *ldlm_bl_get_work(struct ldlm_bl_pool *blp)
{
	struct ldlm_bl_work_item *blwi = NULL;
	static unsigned int num_bl = 0;

	spin_lock(&blp->blp_lock);
        /* process a request from the blp_list at least every blp_num_threads */
        if (!cfs_list_empty(&blp->blp_list) &&
            (cfs_list_empty(&blp->blp_prio_list) || num_bl == 0))
                blwi = cfs_list_entry(blp->blp_list.next,
                                      struct ldlm_bl_work_item, blwi_entry);
        else
                if (!cfs_list_empty(&blp->blp_prio_list))
                        blwi = cfs_list_entry(blp->blp_prio_list.next,
                                              struct ldlm_bl_work_item,
                                              blwi_entry);

        if (blwi) {
                if (++num_bl >= cfs_atomic_read(&blp->blp_num_threads))
                        num_bl = 0;
                cfs_list_del(&blwi->blwi_entry);
        }
	spin_unlock(&blp->blp_lock);

	return blwi;
}

/* This only contains temporary data until the thread starts */
struct ldlm_bl_thread_data {
	char			bltd_name[CFS_CURPROC_COMM_MAX];
	struct ldlm_bl_pool	*bltd_blp;
	struct completion	bltd_comp;
	int			bltd_num;
};

static int ldlm_bl_thread_main(void *arg);

static int ldlm_bl_thread_start(struct ldlm_bl_pool *blp)
{
	struct ldlm_bl_thread_data bltd = { .bltd_blp = blp };
	int rc;

	init_completion(&bltd.bltd_comp);
	rc = cfs_create_thread(ldlm_bl_thread_main, &bltd, 0);
	if (rc < 0) {
		CERROR("cannot start LDLM thread ldlm_bl_%02d: rc %d\n",
		       cfs_atomic_read(&blp->blp_num_threads), rc);
		return rc;
	}
	wait_for_completion(&bltd.bltd_comp);

	return 0;
}

/**
 * Main blocking requests processing thread.
 *
 * Callers put locks into its queue by calling ldlm_bl_to_thread.
 * This thread in the end ends up doing actual call to ->l_blocking_ast
 * for queued locks.
 */
static int ldlm_bl_thread_main(void *arg)
{
        struct ldlm_bl_pool *blp;
        ENTRY;

        {
                struct ldlm_bl_thread_data *bltd = arg;

                blp = bltd->bltd_blp;

                bltd->bltd_num =
                        cfs_atomic_inc_return(&blp->blp_num_threads) - 1;
                cfs_atomic_inc(&blp->blp_busy_threads);

                snprintf(bltd->bltd_name, sizeof(bltd->bltd_name) - 1,
                        "ldlm_bl_%02d", bltd->bltd_num);
                cfs_daemonize(bltd->bltd_name);

		complete(&bltd->bltd_comp);
                /* cannot use bltd after this, it is only on caller's stack */
        }

        while (1) {
                struct l_wait_info lwi = { 0 };
                struct ldlm_bl_work_item *blwi = NULL;
                int busy;

                blwi = ldlm_bl_get_work(blp);

                if (blwi == NULL) {
                        cfs_atomic_dec(&blp->blp_busy_threads);
                        l_wait_event_exclusive(blp->blp_waitq,
                                         (blwi = ldlm_bl_get_work(blp)) != NULL,
                                         &lwi);
                        busy = cfs_atomic_inc_return(&blp->blp_busy_threads);
                } else {
                        busy = cfs_atomic_read(&blp->blp_busy_threads);
                }

                if (blwi->blwi_ns == NULL)
                        /* added by ldlm_cleanup() */
                        break;

                /* Not fatal if racy and have a few too many threads */
                if (unlikely(busy < blp->blp_max_threads &&
                             busy >= cfs_atomic_read(&blp->blp_num_threads) &&
                             !blwi->blwi_mem_pressure))
                        /* discard the return value, we tried */
                        ldlm_bl_thread_start(blp);

                if (blwi->blwi_mem_pressure)
                        cfs_memory_pressure_set();

                if (blwi->blwi_count) {
                        int count;
			/* The special case when we cancel locks in LRU
                         * asynchronously, we pass the list of locks here.
                         * Thus locks are marked LDLM_FL_CANCELING, but NOT
                         * canceled locally yet. */
                        count = ldlm_cli_cancel_list_local(&blwi->blwi_head,
                                                           blwi->blwi_count,
                                                           LCF_BL_AST);
			ldlm_cli_cancel_list(&blwi->blwi_head, count, NULL,
					     blwi->blwi_flags);
                } else {
                        ldlm_handle_bl_callback(blwi->blwi_ns, &blwi->blwi_ld,
                                                blwi->blwi_lock);
                }
                if (blwi->blwi_mem_pressure)
                        cfs_memory_pressure_clr();

		if (blwi->blwi_flags & LCF_ASYNC)
			OBD_FREE(blwi, sizeof(*blwi));
		else
			complete(&blwi->blwi_comp);
        }

        cfs_atomic_dec(&blp->blp_busy_threads);
        cfs_atomic_dec(&blp->blp_num_threads);
	complete(&blp->blp_comp);
        RETURN(0);
}

#endif

static int ldlm_setup(void);
static int ldlm_cleanup(void);

int ldlm_get_ref(void)
{
        int rc = 0;
        ENTRY;
	mutex_lock(&ldlm_ref_mutex);
        if (++ldlm_refcount == 1) {
                rc = ldlm_setup();
                if (rc)
                        ldlm_refcount--;
        }
	mutex_unlock(&ldlm_ref_mutex);

        RETURN(rc);
}
EXPORT_SYMBOL(ldlm_get_ref);

void ldlm_put_ref(void)
{
        ENTRY;
	mutex_lock(&ldlm_ref_mutex);
        if (ldlm_refcount == 1) {
                int rc = ldlm_cleanup();
                if (rc)
                        CERROR("ldlm_cleanup failed: %d\n", rc);
                else
                        ldlm_refcount--;
        } else {
                ldlm_refcount--;
        }
	mutex_unlock(&ldlm_ref_mutex);

        EXIT;
}
EXPORT_SYMBOL(ldlm_put_ref);

/*
 * Export handle<->lock hash operations.
 */
static unsigned
ldlm_export_lock_hash(cfs_hash_t *hs, const void *key, unsigned mask)
{
        return cfs_hash_u64_hash(((struct lustre_handle *)key)->cookie, mask);
}

static void *
ldlm_export_lock_key(cfs_hlist_node_t *hnode)
{
        struct ldlm_lock *lock;

        lock = cfs_hlist_entry(hnode, struct ldlm_lock, l_exp_hash);
        return &lock->l_remote_handle;
}

static void
ldlm_export_lock_keycpy(cfs_hlist_node_t *hnode, void *key)
{
        struct ldlm_lock     *lock;

        lock = cfs_hlist_entry(hnode, struct ldlm_lock, l_exp_hash);
        lock->l_remote_handle = *(struct lustre_handle *)key;
}

static int
ldlm_export_lock_keycmp(const void *key, cfs_hlist_node_t *hnode)
{
        return lustre_handle_equal(ldlm_export_lock_key(hnode), key);
}

static void *
ldlm_export_lock_object(cfs_hlist_node_t *hnode)
{
        return cfs_hlist_entry(hnode, struct ldlm_lock, l_exp_hash);
}

static void
ldlm_export_lock_get(cfs_hash_t *hs, cfs_hlist_node_t *hnode)
{
        struct ldlm_lock *lock;

        lock = cfs_hlist_entry(hnode, struct ldlm_lock, l_exp_hash);
        LDLM_LOCK_GET(lock);
}

static void
ldlm_export_lock_put(cfs_hash_t *hs, cfs_hlist_node_t *hnode)
{
        struct ldlm_lock *lock;

        lock = cfs_hlist_entry(hnode, struct ldlm_lock, l_exp_hash);
        LDLM_LOCK_RELEASE(lock);
}

static cfs_hash_ops_t ldlm_export_lock_ops = {
        .hs_hash        = ldlm_export_lock_hash,
        .hs_key         = ldlm_export_lock_key,
        .hs_keycmp      = ldlm_export_lock_keycmp,
        .hs_keycpy      = ldlm_export_lock_keycpy,
        .hs_object      = ldlm_export_lock_object,
        .hs_get         = ldlm_export_lock_get,
        .hs_put         = ldlm_export_lock_put,
        .hs_put_locked  = ldlm_export_lock_put,
};

int ldlm_init_export(struct obd_export *exp)
{
	int rc;
        ENTRY;

        exp->exp_lock_hash =
                cfs_hash_create(obd_uuid2str(&exp->exp_client_uuid),
                                HASH_EXP_LOCK_CUR_BITS,
                                HASH_EXP_LOCK_MAX_BITS,
                                HASH_EXP_LOCK_BKT_BITS, 0,
                                CFS_HASH_MIN_THETA, CFS_HASH_MAX_THETA,
                                &ldlm_export_lock_ops,
                                CFS_HASH_DEFAULT | CFS_HASH_REHASH_KEY |
                                CFS_HASH_NBLK_CHANGE);

        if (!exp->exp_lock_hash)
                RETURN(-ENOMEM);

	rc = ldlm_init_flock_export(exp);
	if (rc)
		GOTO(err, rc);

        RETURN(0);
err:
	ldlm_destroy_export(exp);
	RETURN(rc);
}
EXPORT_SYMBOL(ldlm_init_export);

void ldlm_destroy_export(struct obd_export *exp)
{
        ENTRY;
        cfs_hash_putref(exp->exp_lock_hash);
        exp->exp_lock_hash = NULL;

	ldlm_destroy_flock_export(exp);
        EXIT;
}
EXPORT_SYMBOL(ldlm_destroy_export);

static int ldlm_setup(void)
{
	static struct ptlrpc_service_conf	conf;
	struct ldlm_bl_pool			*blp = NULL;
        int rc = 0;
#ifdef __KERNEL__
        int i;
#endif
        ENTRY;

        if (ldlm_state != NULL)
                RETURN(-EALREADY);

        OBD_ALLOC(ldlm_state, sizeof(*ldlm_state));
        if (ldlm_state == NULL)
                RETURN(-ENOMEM);

#ifdef LPROCFS
        rc = ldlm_proc_setup();
        if (rc != 0)
		GOTO(out, rc);
#endif

	memset(&conf, 0, sizeof(conf));
	conf = (typeof(conf)) {
		.psc_name		= "ldlm_cbd",
		.psc_watchdog_factor	= 2,
		.psc_buf		= {
			.bc_nbufs		= LDLM_CLIENT_NBUFS,
			.bc_buf_size		= LDLM_BUFSIZE,
			.bc_req_max_size	= LDLM_MAXREQSIZE,
			.bc_rep_max_size	= LDLM_MAXREPSIZE,
			.bc_req_portal		= LDLM_CB_REQUEST_PORTAL,
			.bc_rep_portal		= LDLM_CB_REPLY_PORTAL,
		},
		.psc_thr		= {
			.tc_thr_name		= "ldlm_cb",
			.tc_thr_factor		= LDLM_THR_FACTOR,
			.tc_nthrs_init		= LDLM_NTHRS_INIT,
			.tc_nthrs_base		= LDLM_NTHRS_BASE,
			.tc_nthrs_max		= LDLM_NTHRS_MAX,
			.tc_nthrs_user		= ldlm_num_threads,
			.tc_cpu_affinity	= 1,
			.tc_ctx_tags		= LCT_MD_THREAD | LCT_DT_THREAD,
		},
		.psc_cpt		= {
			.cc_pattern		= ldlm_cpts,
		},
		.psc_ops		= {
			.so_req_handler		= ldlm_callback_handler,
		},
	};
	ldlm_state->ldlm_cb_service = \
			ptlrpc_register_service(&conf, ldlm_svc_proc_dir);
	if (IS_ERR(ldlm_state->ldlm_cb_service)) {
		CERROR("failed to start service\n");
		rc = PTR_ERR(ldlm_state->ldlm_cb_service);
		ldlm_state->ldlm_cb_service = NULL;
		GOTO(out, rc);
	}

#ifdef HAVE_SERVER_SUPPORT
	memset(&conf, 0, sizeof(conf));
	conf = (typeof(conf)) {
		.psc_name		= "ldlm_canceld",
		.psc_watchdog_factor	= 6,
		.psc_buf		= {
			.bc_nbufs		= LDLM_SERVER_NBUFS,
			.bc_buf_size		= LDLM_BUFSIZE,
			.bc_req_max_size	= LDLM_MAXREQSIZE,
			.bc_rep_max_size	= LDLM_MAXREPSIZE,
			.bc_req_portal		= LDLM_CANCEL_REQUEST_PORTAL,
			.bc_rep_portal		= LDLM_CANCEL_REPLY_PORTAL,

		},
		.psc_thr		= {
			.tc_thr_name		= "ldlm_cn",
			.tc_thr_factor		= LDLM_THR_FACTOR,
			.tc_nthrs_init		= LDLM_NTHRS_INIT,
			.tc_nthrs_base		= LDLM_NTHRS_BASE,
			.tc_nthrs_max		= LDLM_NTHRS_MAX,
			.tc_nthrs_user		= ldlm_num_threads,
			.tc_cpu_affinity	= 1,
			.tc_ctx_tags		= LCT_MD_THREAD | \
						  LCT_DT_THREAD | \
						  LCT_CL_THREAD,
		},
		.psc_cpt		= {
			.cc_pattern		= ldlm_cpts,
		},
		.psc_ops		= {
			.so_req_handler		= ldlm_cancel_handler,
			.so_hpreq_handler	= ldlm_hpreq_handler,
		},
	};
	ldlm_state->ldlm_cancel_service = \
			ptlrpc_register_service(&conf, ldlm_svc_proc_dir);
	if (IS_ERR(ldlm_state->ldlm_cancel_service)) {
		CERROR("failed to start service\n");
		rc = PTR_ERR(ldlm_state->ldlm_cancel_service);
		ldlm_state->ldlm_cancel_service = NULL;
		GOTO(out, rc);
	}
#endif

	OBD_ALLOC(blp, sizeof(*blp));
	if (blp == NULL)
		GOTO(out, rc = -ENOMEM);
	ldlm_state->ldlm_bl_pool = blp;

	spin_lock_init(&blp->blp_lock);
        CFS_INIT_LIST_HEAD(&blp->blp_list);
        CFS_INIT_LIST_HEAD(&blp->blp_prio_list);
        cfs_waitq_init(&blp->blp_waitq);
        cfs_atomic_set(&blp->blp_num_threads, 0);
        cfs_atomic_set(&blp->blp_busy_threads, 0);

#ifdef __KERNEL__
	if (ldlm_num_threads == 0) {
		blp->blp_min_threads = LDLM_NTHRS_INIT;
		blp->blp_max_threads = LDLM_NTHRS_MAX;
	} else {
		blp->blp_min_threads = blp->blp_max_threads = \
			min_t(int, LDLM_NTHRS_MAX, max_t(int, LDLM_NTHRS_INIT,
							 ldlm_num_threads));
	}

	for (i = 0; i < blp->blp_min_threads; i++) {
		rc = ldlm_bl_thread_start(blp);
		if (rc < 0)
			GOTO(out, rc);
	}

# ifdef HAVE_SERVER_SUPPORT
        CFS_INIT_LIST_HEAD(&expired_lock_thread.elt_expired_locks);
        expired_lock_thread.elt_state = ELT_STOPPED;
        cfs_waitq_init(&expired_lock_thread.elt_waitq);

        CFS_INIT_LIST_HEAD(&waiting_locks_list);
	spin_lock_init(&waiting_locks_spinlock);
        cfs_timer_init(&waiting_locks_timer, waiting_locks_callback, 0);

        rc = cfs_create_thread(expired_lock_main, NULL, CFS_DAEMON_FLAGS);
	if (rc < 0) {
		CERROR("Cannot start ldlm expired-lock thread: %d\n", rc);
		GOTO(out, rc);
	}

        cfs_wait_event(expired_lock_thread.elt_waitq,
                       expired_lock_thread.elt_state == ELT_READY);
# endif /* HAVE_SERVER_SUPPORT */

	rc = ldlm_pools_init();
	if (rc) {
		CERROR("Failed to initialize LDLM pools: %d\n", rc);
		GOTO(out, rc);
	}
#endif
	RETURN(0);

 out:
	ldlm_cleanup();
	RETURN(rc);
}

static int ldlm_cleanup(void)
{
        ENTRY;

        if (!cfs_list_empty(ldlm_namespace_list(LDLM_NAMESPACE_SERVER)) ||
            !cfs_list_empty(ldlm_namespace_list(LDLM_NAMESPACE_CLIENT))) {
                CERROR("ldlm still has namespaces; clean these up first.\n");
                ldlm_dump_all_namespaces(LDLM_NAMESPACE_SERVER, D_DLMTRACE);
                ldlm_dump_all_namespaces(LDLM_NAMESPACE_CLIENT, D_DLMTRACE);
                RETURN(-EBUSY);
        }

#ifdef __KERNEL__
        ldlm_pools_fini();

	if (ldlm_state->ldlm_bl_pool != NULL) {
		struct ldlm_bl_pool *blp = ldlm_state->ldlm_bl_pool;

		while (cfs_atomic_read(&blp->blp_num_threads) > 0) {
			struct ldlm_bl_work_item blwi = { .blwi_ns = NULL };

			init_completion(&blp->blp_comp);

			spin_lock(&blp->blp_lock);
			cfs_list_add_tail(&blwi.blwi_entry, &blp->blp_list);
			cfs_waitq_signal(&blp->blp_waitq);
			spin_unlock(&blp->blp_lock);

			wait_for_completion(&blp->blp_comp);
		}

		OBD_FREE(blp, sizeof(*blp));
	}
#endif /* __KERNEL__ */

	if (ldlm_state->ldlm_cb_service != NULL)
		ptlrpc_unregister_service(ldlm_state->ldlm_cb_service);
# ifdef HAVE_SERVER_SUPPORT
	if (ldlm_state->ldlm_cancel_service != NULL)
		ptlrpc_unregister_service(ldlm_state->ldlm_cancel_service);
# endif

#ifdef __KERNEL__
	ldlm_proc_cleanup();

# ifdef HAVE_SERVER_SUPPORT
	if (expired_lock_thread.elt_state != ELT_STOPPED) {
		expired_lock_thread.elt_state = ELT_TERMINATE;
		cfs_waitq_signal(&expired_lock_thread.elt_waitq);
		cfs_wait_event(expired_lock_thread.elt_waitq,
			       expired_lock_thread.elt_state == ELT_STOPPED);
	}
# endif
#endif /* __KERNEL__ */

        OBD_FREE(ldlm_state, sizeof(*ldlm_state));
        ldlm_state = NULL;

        RETURN(0);
}

int ldlm_init(void)
{
	mutex_init(&ldlm_ref_mutex);
	mutex_init(ldlm_namespace_lock(LDLM_NAMESPACE_SERVER));
	mutex_init(ldlm_namespace_lock(LDLM_NAMESPACE_CLIENT));
        ldlm_resource_slab = cfs_mem_cache_create("ldlm_resources",
                                               sizeof(struct ldlm_resource), 0,
                                               CFS_SLAB_HWCACHE_ALIGN);
        if (ldlm_resource_slab == NULL)
                return -ENOMEM;

	ldlm_lock_slab = cfs_mem_cache_create("ldlm_locks",
			      sizeof(struct ldlm_lock), 0,
			      CFS_SLAB_HWCACHE_ALIGN | SLAB_DESTROY_BY_RCU);
	if (ldlm_lock_slab == NULL) {
		cfs_mem_cache_destroy(ldlm_resource_slab);
		return -ENOMEM;
	}

        ldlm_interval_slab = cfs_mem_cache_create("interval_node",
                                        sizeof(struct ldlm_interval),
                                        0, CFS_SLAB_HWCACHE_ALIGN);
        if (ldlm_interval_slab == NULL) {
                cfs_mem_cache_destroy(ldlm_resource_slab);
                cfs_mem_cache_destroy(ldlm_lock_slab);
                return -ENOMEM;
        }
#if LUSTRE_TRACKS_LOCK_EXP_REFS
        class_export_dump_hook = ldlm_dump_export_locks;
#endif
        return 0;
}

void ldlm_exit(void)
{
        int rc;
        if (ldlm_refcount)
                CERROR("ldlm_refcount is %d in ldlm_exit!\n", ldlm_refcount);
        rc = cfs_mem_cache_destroy(ldlm_resource_slab);
        LASSERTF(rc == 0, "couldn't free ldlm resource slab\n");
#ifdef __KERNEL__
        /* ldlm_lock_put() use RCU to call ldlm_lock_free, so need call
         * synchronize_rcu() to wait a grace period elapsed, so that
         * ldlm_lock_free() get a chance to be called. */
        synchronize_rcu();
#endif
        rc = cfs_mem_cache_destroy(ldlm_lock_slab);
        LASSERTF(rc == 0, "couldn't free ldlm lock slab\n");
        rc = cfs_mem_cache_destroy(ldlm_interval_slab);
        LASSERTF(rc == 0, "couldn't free interval node slab\n");
}
