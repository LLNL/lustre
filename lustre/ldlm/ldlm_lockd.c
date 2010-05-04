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
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
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

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
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

#ifdef __KERNEL__
static int ldlm_num_threads;
CFS_MODULE_PARM(ldlm_num_threads, "i", int, 0444,
                "number of DLM service threads to start");
#endif

extern cfs_mem_cache_t *ldlm_resource_slab;
extern cfs_mem_cache_t *ldlm_lock_slab;
extern struct lustre_lock ldlm_handle_lock;

static struct semaphore ldlm_ref_sem;
static int ldlm_refcount;

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

#ifdef __KERNEL__
/* w_l_spinlock protects both waiting_locks_list and expired_lock_thread */
static spinlock_t waiting_locks_spinlock;   /* BH lock (timer) */
static struct list_head waiting_locks_list;
static cfs_timer_t waiting_locks_timer;

static struct expired_lock_thread {
        cfs_waitq_t               elt_waitq;
        int                       elt_state;
        int                       elt_dump;
        struct list_head          elt_expired_locks;
} expired_lock_thread;
#endif

#define ELT_STOPPED   0
#define ELT_READY     1
#define ELT_TERMINATE 2

struct ldlm_bl_pool {
        spinlock_t              blp_lock;

        /*
         * blp_prio_list is used for callbacks that should be handled
         * as a priority. It is used for LDLM_FL_DISCARD_DATA requests.
         * see bug 13843
         */
        struct list_head        blp_prio_list;

        /*
         * blp_list is used for all other callbacks which are likely
         * to take longer to process.
         */
        struct list_head        blp_list;

        cfs_waitq_t             blp_waitq;
        struct completion       blp_comp;
        atomic_t                blp_num_threads;
        atomic_t                blp_busy_threads;
        int                     blp_min_threads;
        int                     blp_max_threads;
};

struct ldlm_bl_work_item {
        struct list_head        blwi_entry;
        struct ldlm_namespace   *blwi_ns;
        struct ldlm_lock_desc   blwi_ld;
        struct ldlm_lock        *blwi_lock;
        struct list_head        blwi_head;
        int                     blwi_count;
};

#ifdef __KERNEL__

static inline int have_expired_locks(void)
{
        int need_to_run;
        ENTRY;

        spin_lock_bh(&waiting_locks_spinlock);
        need_to_run = !list_empty(&expired_lock_thread.elt_expired_locks);
        spin_unlock_bh(&waiting_locks_spinlock);

        RETURN(need_to_run);
}

static int expired_lock_main(void *arg)
{
        struct list_head *expired = &expired_lock_thread.elt_expired_locks;
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
                        spin_unlock_bh(&waiting_locks_spinlock);

                        /* from waiting_locks_callback, but not in timer */
                        libcfs_debug_dumplog();
                        libcfs_run_lbug_upcall(__FILE__,
                                                "waiting_locks_callback",
                                                expired_lock_thread.elt_dump);

                        spin_lock_bh(&waiting_locks_spinlock);
                        expired_lock_thread.elt_dump = 0;
                }

                do_dump = 0;

                while (!list_empty(expired)) {
                        struct obd_export *export;
                        struct ldlm_lock *lock;

                        lock = list_entry(expired->next, struct ldlm_lock,
                                          l_pending_chain);
                        if ((void *)lock < LP_POISON + CFS_PAGE_SIZE &&
                            (void *)lock >= LP_POISON) {
                                spin_unlock_bh(&waiting_locks_spinlock);
                                CERROR("free lock on elt list %p\n", lock);
                                LBUG();
                        }
                        list_del_init(&lock->l_pending_chain);
                        if ((void *)lock->l_export < LP_POISON + CFS_PAGE_SIZE &&
                            (void *)lock->l_export >= LP_POISON) {
                                CERROR("lock with free export on elt list %p\n",
                                       lock->l_export);
                                lock->l_export = NULL;
                                LDLM_ERROR(lock, "free export");
                                /* release extra ref grabbed by
                                 * ldlm_add_waiting_lock() or
                                 * ldlm_failed_ast() */
                                LDLM_LOCK_PUT(lock);
                                continue;
                        }
                        export = class_export_get(lock->l_export);
                        spin_unlock_bh(&waiting_locks_spinlock);

                        /* release extra ref grabbed by ldlm_add_waiting_lock()
                         * or ldlm_failed_ast() */
                        LDLM_LOCK_PUT(lock);

                        do_dump++;
                        class_fail_export(export);
                        class_export_put(export);
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

        spin_lock(&lock->l_export->exp_lock);
        list_for_each_entry(req, &lock->l_export->exp_queued_rpc, rq_exp_list) {
                if (req->rq_ops->hpreq_lock_match) {
                        match = req->rq_ops->hpreq_lock_match(req, lock);
                        if (match)
                                break;
                }
        }
        spin_unlock(&lock->l_export->exp_lock);
        RETURN(match);
}

/* This is called from within a timer interrupt and cannot schedule */
static void waiting_locks_callback(unsigned long unused)
{
        struct ldlm_lock *lock, *last = NULL;

        spin_lock_bh(&waiting_locks_spinlock);
        while (!list_empty(&waiting_locks_list)) {
                lock = list_entry(waiting_locks_list.next, struct ldlm_lock,
                                  l_pending_chain);
                if (cfs_time_after(lock->l_callback_timeout, cfs_time_current())
                    || (lock->l_req_mode == LCK_GROUP))
                        break;

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
                                LDLM_LOCK_PUT(lock);
                                break;
                        }

                        LDLM_LOCK_PUT(lock);
                        continue;
                }
                lock->l_resource->lr_namespace->ns_timeouts++;
                LDLM_ERROR(lock, "lock callback timer expired after %lds: "
                           "evicting client at %s ",
                           cfs_time_current_sec()- lock->l_last_activity,
                           libcfs_nid2str(
                                   lock->l_export->exp_connection->c_peer.nid));
                if (lock == last) {
                        LDLM_ERROR(lock, "waiting on lock multiple times");
                        CERROR("wll %p n/p %p/%p, l_pending %p n/p %p/%p\n",
                               &waiting_locks_list,
                               waiting_locks_list.next, waiting_locks_list.prev,
                               &lock->l_pending_chain,
                               lock->l_pending_chain.next,
                               lock->l_pending_chain.prev);

                        CFS_INIT_LIST_HEAD(&waiting_locks_list);    /* HACK */
                        expired_lock_thread.elt_dump = __LINE__;

                        /* LBUG(); */
                        CEMERG("would be an LBUG, but isn't (bug 5653)\n");
                        libcfs_debug_dumpstack(NULL);
                        /*blocks* libcfs_debug_dumplog(); */
                        /*blocks* libcfs_run_lbug_upcall(file, func, line); */
                        break;
                }
                last = lock;

                /* no needs to take an extra ref on the lock since it was in
                 * the waiting_locks_list and ldlm_add_waiting_lock()
                 * already grabbed a ref */
                list_del(&lock->l_pending_chain);
                list_add(&lock->l_pending_chain,
                         &expired_lock_thread.elt_expired_locks);
        }

        if (!list_empty(&expired_lock_thread.elt_expired_locks)) {
                if (obd_dump_on_timeout)
                        expired_lock_thread.elt_dump = __LINE__;

                cfs_waitq_signal(&expired_lock_thread.elt_waitq);
        }

        /*
         * Make sure the timer will fire again if we have any locks
         * left.
         */
        if (!list_empty(&waiting_locks_list)) {
                cfs_time_t timeout_rounded;
                lock = list_entry(waiting_locks_list.next, struct ldlm_lock,
                                  l_pending_chain);
                timeout_rounded = (cfs_time_t)round_timeout(lock->l_callback_timeout);
                cfs_timer_arm(&waiting_locks_timer, timeout_rounded);
        }
        spin_unlock_bh(&waiting_locks_spinlock);
}

/*
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

        if (!list_empty(&lock->l_pending_chain))
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
        list_add_tail(&lock->l_pending_chain, &waiting_locks_list); /* FIFO */
        return 1;
}

static int ldlm_add_waiting_lock(struct ldlm_lock *lock)
{
        int ret;
        int timeout = ldlm_get_enq_timeout(lock);

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
        if (ret)
                /* grab ref on the lock if it has been added to the
                 * waiting list */
                LDLM_LOCK_GET(lock);
        spin_unlock_bh(&waiting_locks_spinlock);

        LDLM_DEBUG(lock, "%sadding to wait list(timeout: %d, AT: %s)",
                   ret == 0 ? "not re-" : "", timeout,
                   AT_OFF ? "off" : "on");
        return ret;
}

/*
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
        struct list_head *list_next;

        if (list_empty(&lock->l_pending_chain))
                return 0;

        list_next = lock->l_pending_chain.next;
        if (lock->l_pending_chain.prev == &waiting_locks_list) {
                /* Removing the head of the list, adjust timer. */
                if (list_next == &waiting_locks_list) {
                        /* No more, just cancel. */
                        cfs_timer_disarm(&waiting_locks_timer);
                } else {
                        struct ldlm_lock *next;
                        next = list_entry(list_next, struct ldlm_lock,
                                          l_pending_chain);
                        cfs_timer_arm(&waiting_locks_timer,
                                      round_timeout(next->l_callback_timeout));
                }
        }
        list_del_init(&lock->l_pending_chain);

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
        if (ret)
                /* release lock ref if it has indeed been removed
                 * from a list */
                LDLM_LOCK_PUT(lock);

        return ret;
}

/*
 * Prolong the lock
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

        if (list_empty(&lock->l_pending_chain)) {
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
#else /* !__KERNEL__ */

static int ldlm_add_waiting_lock(struct ldlm_lock *lock)
{
        LASSERT(!(lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK));
        RETURN(1);
}

int ldlm_del_waiting_lock(struct ldlm_lock *lock)
{
        RETURN(0);
}

int ldlm_refresh_waiting_lock(struct ldlm_lock *lock, int timeout)
{
        RETURN(0);
}
#endif /* __KERNEL__ */

static void ldlm_failed_ast(struct ldlm_lock *lock, int rc,
                            const char *ast_type)
{
        struct ptlrpc_connection *conn = lock->l_export->exp_connection;
        char                     *str = libcfs_nid2str(conn->c_peer.nid);

        LCONSOLE_ERROR_MSG(0x138, "%s: A client on nid %s was evicted due "
                             "to a lock %s callback to %s timed out: rc %d\n",
                             lock->l_export->exp_obd->obd_name, str,
                             ast_type, obd_export_nid2str(lock->l_export), rc);

        if (obd_dump_on_timeout)
                libcfs_debug_dumplog();
#ifdef __KERNEL__
        spin_lock_bh(&waiting_locks_spinlock);
        if (__ldlm_del_waiting_lock(lock) == 0)
                /* the lock was not in any list, grab an extra ref before adding
                 * the lock to the expired list */
                LDLM_LOCK_GET(lock);
        list_add(&lock->l_pending_chain, &expired_lock_thread.elt_expired_locks);
        cfs_waitq_signal(&expired_lock_thread.elt_waitq);
        spin_unlock_bh(&waiting_locks_spinlock);
#else
        class_fail_export(lock->l_export);
#endif
}

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
                if (rc == -EINVAL)
                        LDLM_DEBUG(lock, "client (nid %s) returned %d"
                                   " from %s AST - normal race",
                                   libcfs_nid2str(peer.nid),
                                   lustre_msg_get_status(req->rq_repmsg),
                                   ast_type);
                else
                        LDLM_ERROR(lock, "client (nid %s) returned %d "
                                   "from %s AST", libcfs_nid2str(peer.nid),
                                   (req->rq_repmsg != NULL) ?
                                   lustre_msg_get_status(req->rq_repmsg) : 0,
                                   ast_type);
                ldlm_lock_cancel(lock);
                /* Server-side AST functions are called from ldlm_reprocess_all,
                 * which needs to be told to please restart its reprocessing. */
                rc = -ERESTART;
        }

        return rc;
}

static int ldlm_cb_interpret(struct ptlrpc_request *req, void *data, int rc)
{
        struct ldlm_cb_set_arg *arg;
        struct ldlm_lock *lock;
        ENTRY;

        LASSERT(data != NULL);

        arg = req->rq_async_args.pointer_arg[0];
        lock = req->rq_async_args.pointer_arg[1];
        LASSERT(lock != NULL);
        if (rc != 0) {
                /* If client canceled the lock but the cancel has not
                 * been recieved yet, we need to update lvbo to have the
                 * proper attributes cached. */
                if (rc == -EINVAL && arg->type == LDLM_BL_CALLBACK)
                        ldlm_res_lvbo_update(lock->l_resource, NULL,
                                             0, 1);
                rc = ldlm_handle_ast_error(lock, req, rc,
                                           arg->type == LDLM_BL_CALLBACK
                                           ? "blocking" : "completion");
        }

        LDLM_LOCK_PUT(lock);

        if (rc == -ERESTART)
                atomic_set(&arg->restart, 1);

        RETURN(0);
}

static inline int ldlm_bl_and_cp_ast_fini(struct ptlrpc_request *req,
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
                        /* If we cancelled the lock, we need to restart
                         * ldlm_reprocess_queue */
                        atomic_set(&arg->restart, 1);
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

        spin_lock(&lock->l_export->exp_lock);
        list_for_each_entry(req, &lock->l_export->exp_queued_rpc, rq_exp_list) {
                if (!req->rq_hp && req->rq_ops->hpreq_lock_match &&
                    req->rq_ops->hpreq_lock_match(req, lock))
                        ptlrpc_hpreq_reorder(req);
        }
        spin_unlock(&lock->l_export->exp_lock);
        EXIT;
}

/*
 * ->l_blocking_ast() method for server-side locks. This is invoked when newly
 * enqueued server lock conflicts with given one.
 *
 * Sends blocking ast rpc to the client owning that lock; arms timeout timer
 * to wait for client response.
 */
int ldlm_server_blocking_ast(struct ldlm_lock *lock,
                             struct ldlm_lock_desc *desc,
                             void *data, int flag)
{
        struct ldlm_cb_set_arg *arg = data;
        struct ldlm_request *body;
        struct ptlrpc_request *req;
        __u32 size[] = { [MSG_PTLRPC_BODY_OFF] = sizeof(struct ptlrpc_body),
                       [DLM_LOCKREQ_OFF]     = sizeof(*body) };
        int instant_cancel = 0, rc;
        ENTRY;

        if (flag == LDLM_CB_CANCELING) {
                /* Don't need to do anything here. */
                RETURN(0);
        }

        LASSERT(lock);
        LASSERT(data != NULL);

        ldlm_lock_reorder_req(lock);

        req = ptlrpc_prep_req(lock->l_export->exp_imp_reverse,
                              LUSTRE_DLM_VERSION, LDLM_BL_CALLBACK, 2, size,
                              NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        req->rq_async_args.pointer_arg[0] = arg;
        req->rq_async_args.pointer_arg[1] = lock;
        req->rq_interpret_reply = ldlm_cb_interpret;
        req->rq_no_resend = 1;

        lock_res(lock->l_resource);
        if (lock->l_granted_mode != lock->l_req_mode) {
                /* this blocking AST will be communicated as part of the
                 * completion AST instead */
                unlock_res(lock->l_resource);
                ptlrpc_req_finished(req);
                LDLM_DEBUG(lock, "lock not granted, not sending blocking AST");
                RETURN(0);
        }

        if (lock->l_destroyed) {
                /* What's the point? */
                unlock_res(lock->l_resource);
                ptlrpc_req_finished(req);
                RETURN(0);
        }

#if 0
        if (CURRENT_SECONDS - lock->l_export->exp_last_request_time > 30){
                unlock_res(lock->l_resource);
                ptlrpc_req_finished(req);
                ldlm_failed_ast(lock, -ETIMEDOUT, "Not-attempted blocking");
                RETURN(-ETIMEDOUT);
        }
#endif

        if (lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK)
                instant_cancel = 1;

        body = lustre_msg_buf(req->rq_reqmsg, DLM_LOCKREQ_OFF, sizeof(*body));
        body->lock_handle[0] = lock->l_remote_handle;
        body->lock_desc = *desc;
        body->lock_flags |= (lock->l_flags & LDLM_AST_FLAGS);

        LDLM_DEBUG(lock, "server preparing blocking AST");

        lock->l_last_activity = cfs_time_current_sec();

        ptlrpc_req_set_repsize(req, 1, NULL);
        if (instant_cancel) {
                unlock_res(lock->l_resource);
                ldlm_lock_cancel(lock);
        } else {
                LASSERT(lock->l_granted_mode == lock->l_req_mode);
                ldlm_add_waiting_lock(lock);
                unlock_res(lock->l_resource);
        }

        req->rq_send_state = LUSTRE_IMP_FULL;
        /* ptlrpc_prep_req already set timeout */
        if (AT_OFF)
                req->rq_timeout = ldlm_get_rq_timeout();

        if (lock->l_export && lock->l_export->exp_nid_stats &&
            lock->l_export->exp_nid_stats->nid_ldlm_stats) {
                lprocfs_counter_incr(lock->l_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_BL_CALLBACK - LDLM_FIRST_OPC);
        }

        rc = ldlm_bl_and_cp_ast_fini(req, arg, lock, instant_cancel);

        RETURN(rc);
}

int ldlm_server_completion_ast(struct ldlm_lock *lock, int flags, void *data)
{
        struct ldlm_cb_set_arg *arg = data;
        struct ldlm_request *body;
        struct ptlrpc_request *req;
        long total_enqueue_wait;
        __u32 size[3] = { [MSG_PTLRPC_BODY_OFF] = sizeof(struct ptlrpc_body),
                        [DLM_LOCKREQ_OFF]     = sizeof(*body) };
        int rc, buffers = 2, instant_cancel = 0;
        ENTRY;

        LASSERT(lock != NULL);
        LASSERT(data != NULL);

        total_enqueue_wait = cfs_time_sub(cfs_time_current_sec(),
                                          lock->l_last_activity);

        lock_res_and_lock(lock);
        if (lock->l_resource->lr_lvb_len) {
                size[DLM_REQ_REC_OFF] = lock->l_resource->lr_lvb_len;
                buffers = 3;
        }
        unlock_res_and_lock(lock);

        req = ptlrpc_prep_req(lock->l_export->exp_imp_reverse,
                              LUSTRE_DLM_VERSION, LDLM_CP_CALLBACK, buffers,
                              size, NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        req->rq_async_args.pointer_arg[0] = arg;
        req->rq_async_args.pointer_arg[1] = lock;
        req->rq_interpret_reply = ldlm_cb_interpret;
        req->rq_no_resend = 1;

        body = lustre_msg_buf(req->rq_reqmsg, DLM_LOCKREQ_OFF, sizeof(*body));
        body->lock_handle[0] = lock->l_remote_handle;
        body->lock_flags = flags;
        ldlm_lock2desc(lock, &body->lock_desc);

        if (buffers == 3) {
                void *lvb;

                lvb = lustre_msg_buf(req->rq_reqmsg, DLM_REQ_REC_OFF,
                                     lock->l_resource->lr_lvb_len);
                lock_res_and_lock(lock);
                memcpy(lvb, lock->l_resource->lr_lvb_data,
                       lock->l_resource->lr_lvb_len);
                unlock_res_and_lock(lock);
        }

        LDLM_DEBUG(lock, "server preparing completion AST (after %lds wait)",
                   total_enqueue_wait);

        /* Server-side enqueue wait time estimate, used in
            __ldlm_add_waiting_lock to set future enqueue timers */
        if (total_enqueue_wait < ldlm_get_enq_timeout(lock))
                at_measured(&lock->l_resource->lr_namespace->ns_at_estimate,
                            total_enqueue_wait);
        else
                /* bz18618. Don't add lock enqueue time we spend waiting for a
                   previous callback to fail. Locks waiting legitimately will
                   get extended by ldlm_refresh_waiting_lock regardless of the
                   estimate, so it's okay to underestimate here. */
                LDLM_DEBUG(lock, "lock completed after %lus; estimate was %ds. "
                       "It is likely that a previous callback timed out.",
                       total_enqueue_wait,
                       at_get(&lock->l_resource->lr_namespace->ns_at_estimate));

        ptlrpc_req_set_repsize(req, 1, NULL);

        req->rq_send_state = LUSTRE_IMP_FULL;
        /* ptlrpc_prep_req already set timeout */
        if (AT_OFF)
                req->rq_timeout = ldlm_get_rq_timeout();

        /* We only send real blocking ASTs after the lock is granted */
        lock_res_and_lock(lock);
        if (lock->l_flags & LDLM_FL_AST_SENT) {
                body->lock_flags |= LDLM_FL_AST_SENT;
                /* copy ast flags like LDLM_FL_DISCARD_DATA */
                body->lock_flags |= (lock->l_flags & LDLM_AST_FLAGS);

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
                        ldlm_add_waiting_lock(lock); /* start the lock-timeout
                                                         clock */
                }
        }
        unlock_res_and_lock(lock);

        if (lock->l_export && lock->l_export->exp_nid_stats &&
            lock->l_export->exp_nid_stats->nid_ldlm_stats) {
                lprocfs_counter_incr(lock->l_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_CP_CALLBACK - LDLM_FIRST_OPC);
        }

        rc = ldlm_bl_and_cp_ast_fini(req, arg, lock, instant_cancel);

        RETURN(rc);
}

int ldlm_server_glimpse_ast(struct ldlm_lock *lock, void *data)
{
        struct ldlm_resource *res = lock->l_resource;
        struct ldlm_request *body;
        struct ptlrpc_request *req;
        __u32 size[] = { [MSG_PTLRPC_BODY_OFF] = sizeof(struct ptlrpc_body),
                       [DLM_LOCKREQ_OFF]     = sizeof(*body) };
        int rc = 0;
        ENTRY;

        LASSERT(lock != NULL && lock->l_export != NULL);

        req = ptlrpc_prep_req(lock->l_export->exp_imp_reverse,
                              LUSTRE_DLM_VERSION, LDLM_GL_CALLBACK, 2, size,
                              NULL);
        if (req == NULL)
                RETURN(-ENOMEM);

        body = lustre_msg_buf(req->rq_reqmsg, DLM_LOCKREQ_OFF, sizeof(*body));
        body->lock_handle[0] = lock->l_remote_handle;
        ldlm_lock2desc(lock, &body->lock_desc);

        lock_res_and_lock(lock);
        size[REPLY_REC_OFF] = lock->l_resource->lr_lvb_len;
        unlock_res_and_lock(lock);
        res = lock->l_resource;
        ptlrpc_req_set_repsize(req, 2, size);

        req->rq_send_state = LUSTRE_IMP_FULL;
        /* ptlrpc_prep_req already set timeout */
        if (AT_OFF)
                req->rq_timeout = ldlm_get_rq_timeout();

        if (lock->l_export && lock->l_export->exp_nid_stats &&
            lock->l_export->exp_nid_stats->nid_ldlm_stats) {
                lprocfs_counter_incr(lock->l_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_GL_CALLBACK - LDLM_FIRST_OPC);
        }

        rc = ptlrpc_queue_wait(req);
        if (rc == -ELDLM_NO_LOCK_DATA)
                LDLM_DEBUG(lock, "lost race - client has a lock but no inode");
        else if (rc != 0)
                rc = ldlm_handle_ast_error(lock, req, rc, "glimpse");
        else
                rc = ldlm_res_lvbo_update(res, req,
                                          REPLY_REC_OFF, 1);
        ptlrpc_req_finished(req);
        if (rc == -ERESTART)
                ldlm_reprocess_all(res);

        RETURN(rc);
}

static void ldlm_svc_get_eopc(struct ldlm_request *dlm_req,
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

        return ;
}

/*
 * Main server-side entry point into LDLM. This is called by ptlrpc service
 * threads to carry out client lock enqueueing requests.
 */
int ldlm_handle_enqueue(struct ptlrpc_request *req,
                        ldlm_completion_callback completion_callback,
                        ldlm_blocking_callback blocking_callback,
                        ldlm_glimpse_callback glimpse_callback)
{
        struct obd_device *obddev = req->rq_export->exp_obd;
        struct ldlm_reply *dlm_rep;
        struct ldlm_request *dlm_req;
        __u32 size[3] = { [MSG_PTLRPC_BODY_OFF] = sizeof(struct ptlrpc_body),
                        [DLM_LOCKREPLY_OFF]   = sizeof(*dlm_rep) };
        int rc = 0;
        __u32 flags;
        ldlm_error_t err = ELDLM_OK;
        struct ldlm_lock *lock = NULL;
        void *cookie = NULL;
        ENTRY;

        LDLM_DEBUG_NOLOCK("server-side enqueue handler START");

        dlm_req = lustre_swab_reqbuf(req, DLM_LOCKREQ_OFF, sizeof(*dlm_req),
                                     lustre_swab_ldlm_request);
        if (dlm_req == NULL) {
                CERROR ("Can't unpack dlm_req\n");
                GOTO(out, rc = -EFAULT);
        }

        ldlm_request_cancel(req, dlm_req, LDLM_ENQUEUE_CANCEL_OFF);
        flags = dlm_req->lock_flags;

        LASSERT(req->rq_export);

        if (req->rq_rqbd->rqbd_service->srv_stats)
                ldlm_svc_get_eopc(dlm_req,
                                  req->rq_rqbd->rqbd_service->srv_stats);

        if (req->rq_export && req->rq_export->exp_nid_stats &&
            req->rq_export->exp_nid_stats->nid_ldlm_stats) {
                lprocfs_counter_incr(req->rq_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_ENQUEUE - LDLM_FIRST_OPC);
        }

        if (dlm_req->lock_desc.l_resource.lr_type < LDLM_MIN_TYPE ||
            dlm_req->lock_desc.l_resource.lr_type >= LDLM_MAX_TYPE) {
                DEBUG_REQ(D_ERROR, req, "invalid lock request type %d",
                          dlm_req->lock_desc.l_resource.lr_type);
                GOTO(out, rc = -EFAULT);
        }

        if (dlm_req->lock_desc.l_req_mode <= LCK_MINMODE ||
            dlm_req->lock_desc.l_req_mode >= LCK_MAXMODE ||
            dlm_req->lock_desc.l_req_mode & (dlm_req->lock_desc.l_req_mode-1)) {
                DEBUG_REQ(D_ERROR, req, "invalid lock request mode %d",
                          dlm_req->lock_desc.l_req_mode);
                GOTO(out, rc = -EFAULT);
        }

        if (req->rq_export->exp_connect_flags & OBD_CONNECT_IBITS) {
                if (dlm_req->lock_desc.l_resource.lr_type == LDLM_PLAIN) {
                        DEBUG_REQ(D_ERROR, req,
                                  "PLAIN lock request from IBITS client?");
                        GOTO(out, rc = -EPROTO);
                }
        } else if (dlm_req->lock_desc.l_resource.lr_type == LDLM_IBITS) {
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
        if (!(req->rq_export->exp_connect_flags & OBD_CONNECT_IBITS) &&
            (dlm_req->lock_desc.l_resource.lr_type == LDLM_PLAIN)) {
                dlm_req->lock_desc.l_resource.lr_type = LDLM_IBITS;
                dlm_req->lock_desc.l_policy_data.l_inodebits.bits =
                        MDS_INODELOCK_LOOKUP | MDS_INODELOCK_UPDATE;
                if (dlm_req->lock_desc.l_req_mode == LCK_PR)
                        dlm_req->lock_desc.l_req_mode = LCK_CR;
        }
#endif

        if (flags & LDLM_FL_REPLAY) {
                /* Find an existing lock in the per-export lock hash */
                lock = lustre_hash_lookup(req->rq_export->exp_lock_hash,
                                          (void *)&dlm_req->lock_handle[0]);
                if (lock != NULL) {
                        DEBUG_REQ(D_DLMTRACE, req, "found existing lock cookie "
                                  LPX64, lock->l_handle.h_cookie);
                        GOTO(existing_lock, rc = 0);
                }
        }

        /* The lock's callback data might be set in the policy function */
        lock = ldlm_lock_create(obddev->obd_namespace,
                                dlm_req->lock_desc.l_resource.lr_name,
                                dlm_req->lock_desc.l_resource.lr_type,
                                dlm_req->lock_desc.l_req_mode,
                                blocking_callback, completion_callback,
                                glimpse_callback, NULL, 0);
        if (!lock)
                GOTO(out, rc = -ENOMEM);

        lock->l_last_activity = cfs_time_current_sec();
        lock->l_remote_handle = dlm_req->lock_handle[0];
        LDLM_DEBUG(lock, "server-side enqueue handler, new lock created");

        OBD_FAIL_TIMEOUT(OBD_FAIL_LDLM_ENQUEUE_BLOCKED, obd_timeout * 2);
        /* Don't enqueue a lock onto the export if it has already
         * been evicted.  Cancel it now instead. (bug 3822) */
        if (req->rq_export->exp_failed) {
                LDLM_ERROR(lock, "lock on destroyed export %p", req->rq_export);
                GOTO(out, rc = -ENOTCONN);
        }
        lock->l_export = class_export_get(req->rq_export);

        if (lock->l_export->exp_lock_hash)
                lustre_hash_add(lock->l_export->exp_lock_hash,
                                &lock->l_remote_handle, &lock->l_exp_hash);

existing_lock:

        if (flags & LDLM_FL_HAS_INTENT) {
                /* In this case, the reply buffer is allocated deep in
                 * local_lock_enqueue by the policy function. */
                cookie = req;
        } else {
                int buffers = 2;

                lock_res_and_lock(lock);
                if (lock->l_resource->lr_lvb_len) {
                        size[DLM_REPLY_REC_OFF] = lock->l_resource->lr_lvb_len;
                        buffers = 3;
                }
                unlock_res_and_lock(lock);

                if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR))
                        GOTO(out, rc = -ENOMEM);

                rc = lustre_pack_reply(req, buffers, size, NULL);
                if (rc)
                        GOTO(out, rc);
        }

        if (dlm_req->lock_desc.l_resource.lr_type != LDLM_PLAIN)
                lock->l_policy_data = dlm_req->lock_desc.l_policy_data;
        if (dlm_req->lock_desc.l_resource.lr_type == LDLM_EXTENT)
                lock->l_req_extent = lock->l_policy_data.l_extent;

        err = ldlm_lock_enqueue(obddev->obd_namespace, &lock, cookie, (int *)&flags);
        if (err)
                GOTO(out, err);

        dlm_rep = lustre_msg_buf(req->rq_repmsg, DLM_LOCKREPLY_OFF,
                                 sizeof(*dlm_rep));
        dlm_rep->lock_flags = flags;

        ldlm_lock2desc(lock, &dlm_rep->lock_desc);
        ldlm_lock2handle(lock, &dlm_rep->lock_handle);

        /* We never send a blocking AST until the lock is granted, but
         * we can tell it right now */
        lock_res_and_lock(lock);

        /* Now take into account flags to be inherited from original lock
           request both in reply to client and in our own lock flags. */
        dlm_rep->lock_flags |= dlm_req->lock_flags & LDLM_INHERIT_FLAGS;
        lock->l_flags |= dlm_req->lock_flags & LDLM_INHERIT_FLAGS;

        /* Don't move a pending lock onto the export if it has already
         * been evicted.  Cancel it now instead. (bug 5683) */
        if (req->rq_export->exp_failed ||
            OBD_FAIL_CHECK_ONCE(OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT)) {
                LDLM_ERROR(lock, "lock on destroyed export %p", req->rq_export);
                rc = -ENOTCONN;
        } else if (lock->l_flags & LDLM_FL_AST_SENT) {
                dlm_rep->lock_flags |= LDLM_FL_AST_SENT;
                if (lock->l_granted_mode == lock->l_req_mode) {
                        /* Only cancel lock if it was granted, because it
                         * would be destroyed immediatelly and would never
                         * be granted in the future, causing timeouts on client.
                         * Not granted lock will be cancelled immediatelly after
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
                if (!(lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK) ||
                    !(dlm_rep->lock_flags & LDLM_FL_CANCEL_ON_BLOCK)) {
                        CERROR("Granting sync lock to libclient. "
                               "req fl %d, rep fl %d, lock fl "LPX64"\n",
                               dlm_req->lock_flags, dlm_rep->lock_flags,
                               lock->l_flags);
                        LDLM_ERROR(lock, "sync lock");
                        if (dlm_req->lock_flags & LDLM_FL_HAS_INTENT) {
                                struct ldlm_intent *it;
                                it = lustre_msg_buf(req->rq_reqmsg,
                                                    DLM_INTENT_IT_OFF,
                                                    sizeof(*it));
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
        req->rq_status = rc ?: err;  /* return either error - bug 11190 */
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

                if (rc == 0 && obddev->obd_fail)
                        rc = -ENOTCONN;

                if (rc == 0) {
                        lock_res_and_lock(lock);
                        size[DLM_REPLY_REC_OFF] = lock->l_resource->lr_lvb_len;
                        if (size[DLM_REPLY_REC_OFF] > 0) {
                                void *lvb = lustre_msg_buf(req->rq_repmsg,
                                                       DLM_REPLY_REC_OFF,
                                                       size[DLM_REPLY_REC_OFF]);
                                LASSERTF(lvb != NULL, "req %p, lock %p\n",
                                         req, lock);

                                memcpy(lvb, lock->l_resource->lr_lvb_data,
                                       size[DLM_REPLY_REC_OFF]);
                        }
                        unlock_res_and_lock(lock);
                } else {
                        lock_res_and_lock(lock);
                        ldlm_resource_unlink_lock(lock);
                        ldlm_lock_destroy_nolock(lock);
                        unlock_res_and_lock(lock);
                }

                if (!err && dlm_req->lock_desc.l_resource.lr_type != LDLM_FLOCK)
                        ldlm_reprocess_all(lock->l_resource);

                LDLM_LOCK_PUT(lock);
        }

        LDLM_DEBUG_NOLOCK("server-side enqueue handler END (lock %p, rc %d)",
                          lock, rc);

        return rc;
}

int ldlm_handle_convert(struct ptlrpc_request *req)
{
        struct ldlm_request *dlm_req;
        struct ldlm_reply *dlm_rep;
        struct ldlm_lock *lock;
        int rc;
        __u32 size[2] = { [MSG_PTLRPC_BODY_OFF] = sizeof(struct ptlrpc_body),
                        [DLM_LOCKREPLY_OFF]   = sizeof(*dlm_rep) };
        ENTRY;

        dlm_req = lustre_swab_reqbuf(req, DLM_LOCKREQ_OFF, sizeof(*dlm_req),
                                     lustre_swab_ldlm_request);
        if (dlm_req == NULL) {
                CERROR ("Can't unpack dlm_req\n");
                RETURN (-EFAULT);
        }

        if (req->rq_export && req->rq_export->exp_nid_stats &&
            req->rq_export->exp_nid_stats->nid_ldlm_stats) {
                lprocfs_counter_incr(req->rq_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_CONVERT - LDLM_FIRST_OPC);
        }

        rc = lustre_pack_reply(req, 2, size, NULL);
        if (rc)
                RETURN(rc);

        dlm_rep = lustre_msg_buf(req->rq_repmsg, DLM_LOCKREPLY_OFF,
                                 sizeof(*dlm_rep));
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

/* Cancel all the locks whos handles are packed into ldlm_request */
int ldlm_request_cancel(struct ptlrpc_request *req,
                        struct ldlm_request *dlm_req, int first)
{
        struct ldlm_resource *res, *pres = NULL;
        struct ldlm_lock *lock;
        int i, count, done = 0;
        ENTRY;

        count = dlm_req->lock_count ? dlm_req->lock_count : 1;
        if (first >= count)
                RETURN(0);

        /* There is no lock on the server at the replay time,
         * skip lock cancelling to make replay tests to pass. */
        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY)
                RETURN(0);

        for (i = first; i < count; i++) {
                lock = ldlm_handle2lock(&dlm_req->lock_handle[i]);
                if (!lock) {
                        LDLM_DEBUG_NOLOCK("server-side cancel handler stale "
                                          "lock (cookie "LPU64")",
                                          dlm_req->lock_handle[i].cookie);
                        continue;
                }

                done++;
                res = lock->l_resource;
                if (res != pres) {
                        if (pres != NULL) {
                                ldlm_reprocess_all(pres);
                                ldlm_resource_putref(pres);
                        }
                        if (res != NULL) {
                                ldlm_resource_getref(res);
                                ldlm_res_lvbo_update(res, NULL, 0, 1);
                        }
                        pres = res;
                }
                ldlm_lock_cancel(lock);
                LDLM_LOCK_PUT(lock);
        }
        if (pres != NULL) {
                ldlm_reprocess_all(pres);
                ldlm_resource_putref(pres);
        }
        RETURN(done);
}

int ldlm_handle_cancel(struct ptlrpc_request *req)
{
        struct ldlm_request *dlm_req;
        int rc;
        ENTRY;

        dlm_req = lustre_swab_reqbuf(req, DLM_LOCKREQ_OFF, sizeof(*dlm_req),
                                     lustre_swab_ldlm_request);
        if (dlm_req == NULL) {
                CERROR("bad request buffer for cancel\n");
                RETURN(-EFAULT);
        }

        if (req->rq_export && req->rq_export->exp_nid_stats &&
            req->rq_export->exp_nid_stats->nid_ldlm_stats) {
                lprocfs_counter_incr(req->rq_export->exp_nid_stats->nid_ldlm_stats,
                                     LDLM_CANCEL - LDLM_FIRST_OPC);
        }

        rc = lustre_pack_reply(req, 1, NULL, NULL);
        if (rc)
                RETURN(rc);

        if (!ldlm_request_cancel(req, dlm_req, 0))
                req->rq_status = ESTALE;

        if (ptlrpc_reply(req) != 0)
                LBUG();

        RETURN(0);
}

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
                CDEBUG(D_DLMTRACE, "Lock %p is already unused, calling callback (%p)\n",
                       lock, lock->l_blocking_ast);
                if (lock->l_blocking_ast != NULL)
                        lock->l_blocking_ast(lock, ld, lock->l_ast_data,
                                             LDLM_CB_BLOCKING);
        } else {
                CDEBUG(D_DLMTRACE, "Lock %p is referenced, will be cancelled later\n",
                       lock);
        }

        LDLM_LOCK_PUT(lock);
        EXIT;
}

static void ldlm_handle_cp_callback(struct ptlrpc_request *req,
                                    struct ldlm_namespace *ns,
                                    struct ldlm_request *dlm_req,
                                    struct ldlm_lock *lock)
{
        CFS_LIST_HEAD(ast_list);
        ENTRY;

        LDLM_DEBUG(lock, "client completion callback handler START");

        if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_CANCEL_BL_CB_RACE)) {
                int to = cfs_time_seconds(1);
                while (to > 0) {
                        to = schedule_timeout(to);
                        if (lock->l_granted_mode == lock->l_req_mode ||
                            lock->l_destroyed)
                                break;
                }
        }

        lock_res_and_lock(lock);
        if (lock->l_destroyed ||
            lock->l_granted_mode == lock->l_req_mode) {
                /* bug 11300: the lock has already been granted */
                unlock_res_and_lock(lock);
                LDLM_DEBUG(lock, "Double grant race happened");
                LDLM_LOCK_PUT(lock);
                EXIT;
                return;
        }

        /* If we receive the completion AST before the actual enqueue returned,
         * then we might need to switch lock modes, resources, or extents. */
        if (dlm_req->lock_desc.l_granted_mode != lock->l_req_mode) {
                lock->l_req_mode = dlm_req->lock_desc.l_granted_mode;
                LDLM_DEBUG(lock, "completion AST, new lock mode");
        }

        if (lock->l_resource->lr_type != LDLM_PLAIN) {
                lock->l_policy_data = dlm_req->lock_desc.l_policy_data;
                LDLM_DEBUG(lock, "completion AST, new policy data");
        }

        ldlm_resource_unlink_lock(lock);
        if (memcmp(&dlm_req->lock_desc.l_resource.lr_name,
                   &lock->l_resource->lr_name,
                   sizeof(lock->l_resource->lr_name)) != 0) {
                unlock_res_and_lock(lock);
                if (ldlm_lock_change_resource(ns, lock,
                                dlm_req->lock_desc.l_resource.lr_name)) {
                        LDLM_ERROR(lock, "Failed to allocate resource");
                        LDLM_LOCK_PUT(lock);
                        EXIT;
                        return;
                }
                LDLM_DEBUG(lock, "completion AST, new resource");
                CERROR("change resource!\n");
                lock_res_and_lock(lock);
        }

        if (dlm_req->lock_flags & LDLM_FL_AST_SENT) {
                /* BL_AST locks are not needed in lru.
                 * let ldlm_cancel_lru() be fast. */
                ldlm_lock_remove_from_lru(lock);
                lock->l_flags |= LDLM_FL_CBPENDING | LDLM_FL_BL_AST;
                LDLM_DEBUG(lock, "completion AST includes blocking AST");
        }

        if (lock->l_lvb_len) {
                void *lvb;
                lvb = lustre_swab_reqbuf(req, DLM_REQ_REC_OFF, lock->l_lvb_len,
                                         lock->l_lvb_swabber);
                if (lvb == NULL) {
                        LDLM_ERROR(lock, "completion AST did not contain "
                                   "expected LVB!");
                } else {
                        memcpy(lock->l_lvb_data, lvb, lock->l_lvb_len);
                }
        }

        ldlm_grant_lock(lock, &ast_list);
        unlock_res_and_lock(lock);

        LDLM_DEBUG(lock, "callback handler finished, about to run_ast_work");

        ldlm_run_cp_ast_work(&ast_list);

        LDLM_DEBUG_NOLOCK("client completion callback handler END (lock %p)",
                          lock);
        LDLM_LOCK_PUT(lock);
        EXIT;
}

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
        LDLM_LOCK_PUT(lock);
        EXIT;
}

static int ldlm_callback_reply(struct ptlrpc_request *req, int rc)
{
        req->rq_status = rc;
        if (!req->rq_packed_final) {
                rc = lustre_pack_reply(req, 1, NULL, NULL);
                if (rc)
                        return rc;
        }
        return ptlrpc_reply(req);
}

#ifdef __KERNEL__
static int ldlm_bl_to_thread(struct ldlm_namespace *ns,
                             struct ldlm_lock_desc *ld, struct ldlm_lock *lock,
                             struct list_head *cancels, int count)
{
        struct ldlm_bl_pool *blp = ldlm_state->ldlm_bl_pool;
        struct ldlm_bl_work_item *blwi;
        ENTRY;

        if (cancels && count == 0)
                RETURN(0);

        OBD_ALLOC(blwi, sizeof(*blwi));
        if (blwi == NULL)
                RETURN(-ENOMEM);

        blwi->blwi_ns = ns;
        if (ld != NULL)
                blwi->blwi_ld = *ld;
        if (count) {
                list_add(&blwi->blwi_head, cancels);
                list_del_init(cancels);
                blwi->blwi_count = count;
        } else {
                blwi->blwi_lock = lock;
        }
        spin_lock(&blp->blp_lock);
        if (lock && lock->l_flags & LDLM_FL_DISCARD_DATA) {
                /* add LDLM_FL_DISCARD_DATA requests to the priority list */
                list_add_tail(&blwi->blwi_entry, &blp->blp_prio_list);
        } else {
                /* other blocking callbacks are added to the regular list */
                list_add_tail(&blwi->blwi_entry, &blp->blp_list);
        }
        cfs_waitq_signal(&blp->blp_waitq);
        spin_unlock(&blp->blp_lock);

        RETURN(0);
}
#endif

int ldlm_bl_to_thread_lock(struct ldlm_namespace *ns, struct ldlm_lock_desc *ld,
                           struct ldlm_lock *lock)
{
#ifdef __KERNEL__
        RETURN(ldlm_bl_to_thread(ns, ld, lock, NULL, 0));
#else
        RETURN(-ENOSYS);
#endif
}

int ldlm_bl_to_thread_list(struct ldlm_namespace *ns, struct ldlm_lock_desc *ld,
                           struct list_head *cancels, int count)
{
#ifdef __KERNEL__
        RETURN(ldlm_bl_to_thread(ns, ld, NULL, cancels, count));
#else
        RETURN(-ENOSYS);
#endif
}

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

        if (req->rq_export == NULL) {
                ldlm_callback_reply(req, -ENOTCONN);
                RETURN(0);
        }

        LASSERT(req->rq_export != NULL);
        LASSERT(req->rq_export->exp_obd != NULL);

        switch (lustre_msg_get_opc(req->rq_reqmsg)) {
        case LDLM_BL_CALLBACK:
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_BL_CALLBACK, 0);
                break;
        case LDLM_CP_CALLBACK:
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_CP_CALLBACK, 0);
                break;
        case LDLM_GL_CALLBACK:
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_GL_CALLBACK, 0);
                break;
        case OBD_LOG_CANCEL: /* remove this eventually - for 1.4.0 compat */
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOG_CANCEL_NET, 0);
                rc = llog_origin_handle_cancel(req);
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOG_CANCEL_REP, 0);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case OBD_QC_CALLBACK:
                OBD_FAIL_RETURN(OBD_FAIL_OBD_QC_CALLBACK_NET, 0);
                rc = target_handle_qc_callback(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case QUOTA_DQACQ:
        case QUOTA_DQREL:
                /* reply in handler */
                rc = target_handle_dqacq_callback(req);
                RETURN(0);
        case LLOG_ORIGIN_HANDLE_CREATE:
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_create(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case LLOG_ORIGIN_HANDLE_NEXT_BLOCK:
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_next_block(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case LLOG_ORIGIN_HANDLE_READ_HEADER:
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_read_header(req);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        case LLOG_ORIGIN_HANDLE_CLOSE:
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOGD_NET, 0);
                rc = llog_origin_handle_close(req);
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

        dlm_req = lustre_swab_reqbuf(req, DLM_LOCKREQ_OFF, sizeof(*dlm_req),
                                     lustre_swab_ldlm_request);
        if (dlm_req == NULL) {
                CERROR ("can't unpack dlm_req\n");
                ldlm_callback_reply(req, -EPROTO);
                RETURN (0);
        }

        /* Force a known safe race, send a cancel to the server for a lock
         * which the server has already started a blocking callback on. */
        if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_CANCEL_BL_CB_RACE) &&
            lustre_msg_get_opc(req->rq_reqmsg) == LDLM_BL_CALLBACK) {
                rc = ldlm_cli_cancel(&dlm_req->lock_handle[0]);
                if (rc < 0)
                        CERROR("ldlm_cli_cancel: %d\n", rc);
        }

        lock = ldlm_handle2lock_ns(ns, &dlm_req->lock_handle[0]);
        if (!lock) {
                CDEBUG(D_DLMTRACE, "callback on lock "LPX64" - lock "
                       "disappeared\n", dlm_req->lock_handle[0].cookie);
                ldlm_callback_reply(req, -EINVAL);
                RETURN(0);
        }

        if ((lock->l_flags & LDLM_FL_FAIL_LOC) &&
            lustre_msg_get_opc(req->rq_reqmsg) == LDLM_BL_CALLBACK)
                OBD_RACE(OBD_FAIL_LDLM_CP_BL_RACE);

        /* Copy hints/flags (e.g. LDLM_FL_DISCARD_DATA) from AST. */
        lock_res_and_lock(lock);
        lock->l_flags |= (dlm_req->lock_flags & LDLM_AST_FLAGS);
        if (lustre_msg_get_opc(req->rq_reqmsg) == LDLM_BL_CALLBACK) {
                /* If somebody cancels lock and cache is already droped,
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
                        LDLM_LOCK_PUT(lock);
                        ldlm_callback_reply(req, -EINVAL);
                        RETURN(0);
                }
                /* BL_AST locks are not needed in lru.
                 * let ldlm_cancel_lru() be fast. */
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
                if (!(lock->l_flags & LDLM_FL_CANCEL_ON_BLOCK))
                        ldlm_callback_reply(req, 0);
                if (ldlm_bl_to_thread_lock(ns, &dlm_req->lock_desc, lock))
                        ldlm_handle_bl_callback(ns, &dlm_req->lock_desc, lock);
                break;
        case LDLM_CP_CALLBACK:
                CDEBUG(D_INODE, "completion ast\n");
                ldlm_callback_reply(req, 0);
                ldlm_handle_cp_callback(req, ns, dlm_req, lock);
                break;
        case LDLM_GL_CALLBACK:
                CDEBUG(D_INODE, "glimpse ast\n");
                ldlm_handle_gl_callback(req, ns, dlm_req, lock);
                break;
        default:
                LBUG();                         /* checked above */
        }

        RETURN(0);
}

static int ldlm_cancel_handler(struct ptlrpc_request *req)
{
        int rc;
        ENTRY;

        /* Requests arrive in sender's byte order.  The ptlrpc service
         * handler has already checked and, if necessary, byte-swapped the
         * incoming request message body, but I am responsible for the
         * message buffers. */

        if (req->rq_export == NULL) {
                struct ldlm_request *dlm_req;

                CERROR("operation %d from %s with bad export cookie "LPU64"\n",
                       lustre_msg_get_opc(req->rq_reqmsg),
                       libcfs_id2str(req->rq_peer),
                       lustre_msg_get_handle(req->rq_reqmsg)->cookie);

                if (lustre_msg_get_opc(req->rq_reqmsg) == LDLM_CANCEL) {
                        dlm_req = lustre_swab_reqbuf(req, DLM_LOCKREQ_OFF,
                                                     sizeof(*dlm_req),
                                                     lustre_swab_ldlm_request);
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
                CDEBUG(D_INODE, "cancel\n");
                OBD_FAIL_RETURN(OBD_FAIL_LDLM_CANCEL, 0);
                rc = ldlm_handle_cancel(req);
                if (rc)
                        break;
                RETURN(0);
        case OBD_LOG_CANCEL:
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOG_CANCEL_NET, 0);
                rc = llog_origin_handle_cancel(req);
                OBD_FAIL_RETURN(OBD_FAIL_OBD_LOG_CANCEL_REP, 0);
                ldlm_callback_reply(req, rc);
                RETURN(0);
        default:
                CERROR("invalid opcode %d\n",
                       lustre_msg_get_opc(req->rq_reqmsg));
                ldlm_callback_reply(req, -EINVAL);
        }

        RETURN(0);
}

#ifdef __KERNEL__
static struct ldlm_bl_work_item *ldlm_bl_get_work(struct ldlm_bl_pool *blp)
{
        struct ldlm_bl_work_item *blwi = NULL;
        static unsigned int num_bl = 0;

        spin_lock(&blp->blp_lock);
        /* process a request from the blp_list at least every blp_num_threads */
        if (!list_empty(&blp->blp_list) &&
            (list_empty(&blp->blp_prio_list) || num_bl == 0))
                blwi = list_entry(blp->blp_list.next,
                                  struct ldlm_bl_work_item, blwi_entry);
        else
                if (!list_empty(&blp->blp_prio_list))
                        blwi = list_entry(blp->blp_prio_list.next,
                                          struct ldlm_bl_work_item, blwi_entry);

        if (blwi) {
                if (++num_bl >= atomic_read(&blp->blp_num_threads))
                        num_bl = 0;
                list_del(&blwi->blwi_entry);
        }
        spin_unlock(&blp->blp_lock);

        return blwi;
}

/* This only contains temporary data until the thread starts */
struct ldlm_bl_thread_data {
        char                    bltd_name[CFS_CURPROC_COMM_MAX];
        struct ldlm_bl_pool     *bltd_blp;
        struct completion       bltd_comp;
        int                     bltd_num;
};

static int ldlm_bl_thread_main(void *arg);

static int ldlm_bl_thread_start(struct ldlm_bl_pool *blp)
{
        struct ldlm_bl_thread_data bltd = { .bltd_blp = blp };
        int rc;

        init_completion(&bltd.bltd_comp);
        rc = cfs_kernel_thread(ldlm_bl_thread_main, &bltd, 0);
        if (rc < 0) {
                CERROR("cannot start LDLM thread ldlm_bl_%02d: rc %d\n",
                       atomic_read(&blp->blp_num_threads), rc);
                return rc;
        }
        wait_for_completion(&bltd.bltd_comp);

        return 0;
}

static int ldlm_bl_thread_main(void *arg)
{
        struct ldlm_bl_pool *blp;
        ENTRY;

        {
                struct ldlm_bl_thread_data *bltd = arg;

                blp = bltd->bltd_blp;

                bltd->bltd_num = atomic_inc_return(&blp->blp_num_threads) - 1;
                atomic_inc(&blp->blp_busy_threads);

                snprintf(bltd->bltd_name, sizeof(bltd->bltd_name) - 1,
                        "ldlm_bl_%02d", bltd->bltd_num);
                cfs_daemonize(bltd->bltd_name);

                complete(&bltd->bltd_comp);
                /* cannot use bltd after this, it is only on caller's stack */
        }

        while (1) {
                struct l_wait_info lwi = { 0 };
                struct ldlm_bl_work_item *blwi = NULL;

                blwi = ldlm_bl_get_work(blp);

                if (blwi == NULL) {
                        int busy;

                        atomic_dec(&blp->blp_busy_threads);
                        l_wait_event_exclusive(blp->blp_waitq,
                                         (blwi = ldlm_bl_get_work(blp)) != NULL,
                                         &lwi);
                        busy = atomic_inc_return(&blp->blp_busy_threads);

                        if (blwi->blwi_ns == NULL)
                                /* added by ldlm_cleanup() */
                                break;

                        /* Not fatal if racy and have a few too many threads */
                        if (unlikely(busy < blp->blp_max_threads &&
                                    busy >= atomic_read(&blp->blp_num_threads)))
                                /* discard the return value, we tried */
                                ldlm_bl_thread_start(blp);
                } else {
                        if (blwi->blwi_ns == NULL)
                                /* added by ldlm_cleanup() */
                                break;
                }

                if (blwi->blwi_count) {
                        /* The special case when we cancel locks in lru
                         * asynchronously, we pass the list of locks here.
                         * Thus lock is marked LDLM_FL_CANCELING, and already
                         * canceled locally. */
                        ldlm_cli_cancel_list(&blwi->blwi_head,
                                             blwi->blwi_count, NULL, 0);
                } else {
                        ldlm_handle_bl_callback(blwi->blwi_ns, &blwi->blwi_ld,
                                                blwi->blwi_lock);
                }
                OBD_FREE(blwi, sizeof(*blwi));
        }

        atomic_dec(&blp->blp_busy_threads);
        atomic_dec(&blp->blp_num_threads);
        complete(&blp->blp_comp);
        RETURN(0);
}

#endif

/*
 * Export handle<->lock hash operations.
 */
static unsigned
ldlm_export_lock_hash(lustre_hash_t *lh, void *key, unsigned mask)
{
        return lh_u64_hash(((struct lustre_handle *)key)->cookie, mask);
}

static void *
ldlm_export_lock_key(struct hlist_node *hnode)
{
        struct ldlm_lock *lock;
        ENTRY;

        lock = hlist_entry(hnode, struct ldlm_lock, l_exp_hash);
        RETURN(&lock->l_remote_handle);
}

static int
ldlm_export_lock_compare(void *key, struct hlist_node *hnode)
{
        ENTRY;
        RETURN(lustre_handle_equal(ldlm_export_lock_key(hnode), key));
}

static void *
ldlm_export_lock_get(struct hlist_node *hnode)
{
        struct ldlm_lock *lock;
        ENTRY;

        lock = hlist_entry(hnode, struct ldlm_lock, l_exp_hash);
        LDLM_LOCK_GET(lock);

        RETURN(lock);
}

static void *
ldlm_export_lock_put(struct hlist_node *hnode)
{
        struct ldlm_lock *lock;
        ENTRY;

        lock = hlist_entry(hnode, struct ldlm_lock, l_exp_hash);
        LDLM_LOCK_PUT(lock);

        RETURN(lock);
}

static lustre_hash_ops_t ldlm_export_lock_ops = {
        .lh_hash    = ldlm_export_lock_hash,
        .lh_key     = ldlm_export_lock_key,
        .lh_compare = ldlm_export_lock_compare,
        .lh_get     = ldlm_export_lock_get,
        .lh_put     = ldlm_export_lock_put
};

int ldlm_init_export(struct obd_export *exp)
{
        ENTRY;

        exp->exp_lock_hash =
                lustre_hash_init(obd_uuid2str(&exp->exp_client_uuid),
                                 7, 16, &ldlm_export_lock_ops, LH_REHASH);

        if (!exp->exp_lock_hash)
                RETURN(-ENOMEM);

        RETURN(0);
}
EXPORT_SYMBOL(ldlm_init_export);

void ldlm_destroy_export(struct obd_export *exp)
{
        ENTRY;
        lustre_hash_exit(exp->exp_lock_hash);
        exp->exp_lock_hash = NULL;
        EXIT;
}
EXPORT_SYMBOL(ldlm_destroy_export);

static int ldlm_setup(void);
static int ldlm_cleanup(void);

int ldlm_get_ref(void)
{
        int rc = 0;
        ENTRY;
        mutex_down(&ldlm_ref_sem);
        if (++ldlm_refcount == 1) {
                rc = ldlm_setup();
                if (rc)
                        ldlm_refcount--;
        }
        mutex_up(&ldlm_ref_sem);

        RETURN(rc);
}

void ldlm_put_ref(void)
{
        ENTRY;
        mutex_down(&ldlm_ref_sem);
        if (ldlm_refcount == 1) {
                int rc = ldlm_cleanup();
                if (rc)
                        CERROR("ldlm_cleanup failed: %d\n", rc);
                else
                        ldlm_refcount--;
        } else {
                ldlm_refcount--;
        }
        mutex_up(&ldlm_ref_sem);

        EXIT;
}

static int ldlm_cancel_hpreq_lock_match(struct ptlrpc_request *req,
                                        struct ldlm_lock *lock)
{
        struct ldlm_request *dlm_req;
        int i, count;
        ENTRY;

        dlm_req = lustre_swab_reqbuf(req, DLM_LOCKREQ_OFF, sizeof(*dlm_req),
                                     lustre_swab_ldlm_request);
        LASSERT(dlm_req != NULL);
        count = dlm_req->lock_count ? dlm_req->lock_count : 1;

        for (i = 0; i < count; i++)
                if (dlm_req->lock_handle[i].cookie == lock->l_handle.h_cookie)
                        RETURN(1);

        RETURN(0);
}

static int ldlm_cancel_hpreq_check(struct ptlrpc_request *req)
{
        RETURN(0);
}

struct ptlrpc_hpreq_ops ldlm_hpreq_cancel = {
        .hpreq_lock_match  = ldlm_cancel_hpreq_lock_match,
        .hpreq_check       = ldlm_cancel_hpreq_check,
};

static int ldlm_hpreq_handler(struct ptlrpc_request *req)
{
        ENTRY;

        if (req->rq_export) {
                int opc = lustre_msg_get_opc(req->rq_reqmsg);
                struct ldlm_request *dlm_req;

                if (opc == LDLM_CANCEL) {
                        dlm_req = lustre_swab_reqbuf(req, DLM_LOCKREQ_OFF,
                                                     sizeof(*dlm_req),
                                                     lustre_swab_ldlm_request);
                        if (!dlm_req) {
                                CERROR("Missing/short ldlm_request\n");
                                RETURN(-EFAULT);
                        }

                        req->rq_ops = &ldlm_hpreq_cancel;
                }
        }

        RETURN(0);
}

static int ldlm_setup(void)
{
        struct ldlm_bl_pool *blp;
        int rc = 0;
        int ldlm_min_threads = LDLM_THREADS_AUTO_MIN;
        int ldlm_max_threads = LDLM_THREADS_AUTO_MAX;
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
                GOTO(out_free, rc);
#endif

#ifdef __KERNEL__
        if (ldlm_num_threads) {
                /* If ldlm_num_threads is set, it is the min and the max. */
                if (ldlm_num_threads > LDLM_THREADS_AUTO_MAX)
                        ldlm_num_threads = LDLM_THREADS_AUTO_MAX;
                if (ldlm_num_threads < LDLM_THREADS_AUTO_MIN)
                        ldlm_num_threads = LDLM_THREADS_AUTO_MIN;
                ldlm_min_threads = ldlm_max_threads = ldlm_num_threads;
        }
#endif

        ldlm_state->ldlm_cb_service =
                ptlrpc_init_svc(LDLM_NBUFS, LDLM_BUFSIZE, LDLM_MAXREQSIZE,
                                LDLM_MAXREPSIZE, LDLM_CB_REQUEST_PORTAL,
                                LDLM_CB_REPLY_PORTAL, 6,
                                ldlm_callback_handler, "ldlm_cbd",
                                ldlm_svc_proc_dir, NULL,
                                ldlm_min_threads, ldlm_max_threads,
                                "ldlm_cb", NULL);

        if (!ldlm_state->ldlm_cb_service) {
                CERROR("failed to start service\n");
                GOTO(out_proc, rc = -ENOMEM);
        }

        ldlm_state->ldlm_cancel_service =
                ptlrpc_init_svc(LDLM_NBUFS, LDLM_BUFSIZE, LDLM_MAXREQSIZE,
                                LDLM_MAXREPSIZE, LDLM_CANCEL_REQUEST_PORTAL,
                                LDLM_CANCEL_REPLY_PORTAL, 18,
                                ldlm_cancel_handler, "ldlm_canceld",
                                ldlm_svc_proc_dir, NULL,
                                ldlm_min_threads, ldlm_max_threads,
                                "ldlm_cn", ldlm_hpreq_handler);

        if (!ldlm_state->ldlm_cancel_service) {
                CERROR("failed to start service\n");
                GOTO(out_proc, rc = -ENOMEM);
        }

        OBD_ALLOC(blp, sizeof(*blp));
        if (blp == NULL)
                GOTO(out_proc, rc = -ENOMEM);
        ldlm_state->ldlm_bl_pool = blp;

        spin_lock_init(&blp->blp_lock);
        CFS_INIT_LIST_HEAD(&blp->blp_list);
        CFS_INIT_LIST_HEAD(&blp->blp_prio_list);
        cfs_waitq_init(&blp->blp_waitq);
        atomic_set(&blp->blp_num_threads, 0);
        atomic_set(&blp->blp_busy_threads, 0);
        blp->blp_min_threads = ldlm_min_threads;
        blp->blp_max_threads = ldlm_max_threads;

#ifdef __KERNEL__
        for (i = 0; i < blp->blp_min_threads; i++) {
                rc = ldlm_bl_thread_start(blp);
                if (rc < 0)
                        GOTO(out_thread, rc);
        }

        rc = ptlrpc_start_threads(NULL, ldlm_state->ldlm_cancel_service);
        if (rc)
                GOTO(out_thread, rc);

        rc = ptlrpc_start_threads(NULL, ldlm_state->ldlm_cb_service);
        if (rc)
                GOTO(out_thread, rc);

        CFS_INIT_LIST_HEAD(&expired_lock_thread.elt_expired_locks);
        expired_lock_thread.elt_state = ELT_STOPPED;
        cfs_waitq_init(&expired_lock_thread.elt_waitq);

        CFS_INIT_LIST_HEAD(&waiting_locks_list);
        spin_lock_init(&waiting_locks_spinlock);
        cfs_timer_init(&waiting_locks_timer, waiting_locks_callback, 0);

        rc = cfs_kernel_thread(expired_lock_main, NULL, CLONE_VM | CLONE_FILES);
        if (rc < 0) {
                CERROR("Cannot start ldlm expired-lock thread: %d\n", rc);
                GOTO(out_thread, rc);
        }

        wait_event(expired_lock_thread.elt_waitq,
                   expired_lock_thread.elt_state == ELT_READY);
#endif

#ifdef __KERNEL__
        rc = ldlm_pools_init();
        if (rc)
                GOTO(out_thread, rc);
#endif

        RETURN(0);

#ifdef __KERNEL__
 out_thread:
        ptlrpc_unregister_service(ldlm_state->ldlm_cancel_service);
        ptlrpc_unregister_service(ldlm_state->ldlm_cb_service);
#endif

 out_proc:
#ifdef LPROCFS
        ldlm_proc_cleanup();
 out_free:
#endif
        OBD_FREE(ldlm_state, sizeof(*ldlm_state));
        ldlm_state = NULL;
        return rc;
}

static int ldlm_cleanup(void)
{
#ifdef __KERNEL__
        struct ldlm_bl_pool *blp = ldlm_state->ldlm_bl_pool;
#endif
        ENTRY;

        if (!list_empty(ldlm_namespace_list(LDLM_NAMESPACE_SERVER)) ||
            !list_empty(ldlm_namespace_list(LDLM_NAMESPACE_CLIENT))) {
                CERROR("ldlm still has namespaces; clean these up first.\n");
                ldlm_dump_all_namespaces(LDLM_NAMESPACE_SERVER, D_DLMTRACE);
                ldlm_dump_all_namespaces(LDLM_NAMESPACE_CLIENT, D_DLMTRACE);
                RETURN(-EBUSY);
        }

#ifdef __KERNEL__
        ldlm_pools_fini();
#endif

#ifdef __KERNEL__
        while (atomic_read(&blp->blp_num_threads) > 0) {
                struct ldlm_bl_work_item blwi = { .blwi_ns = NULL };

                init_completion(&blp->blp_comp);

                spin_lock(&blp->blp_lock);
                list_add_tail(&blwi.blwi_entry, &blp->blp_list);
                cfs_waitq_signal(&blp->blp_waitq);
                spin_unlock(&blp->blp_lock);

                wait_for_completion(&blp->blp_comp);
        }
        OBD_FREE(blp, sizeof(*blp));

        ptlrpc_unregister_service(ldlm_state->ldlm_cb_service);
        ptlrpc_unregister_service(ldlm_state->ldlm_cancel_service);
        ldlm_proc_cleanup();

        expired_lock_thread.elt_state = ELT_TERMINATE;
        cfs_waitq_signal(&expired_lock_thread.elt_waitq);
        wait_event(expired_lock_thread.elt_waitq,
                   expired_lock_thread.elt_state == ELT_STOPPED);
#else
        ptlrpc_unregister_service(ldlm_state->ldlm_cb_service);
        ptlrpc_unregister_service(ldlm_state->ldlm_cancel_service);
#endif

        OBD_FREE(ldlm_state, sizeof(*ldlm_state));
        ldlm_state = NULL;

        RETURN(0);
}

int __init ldlm_init(void)
{
        init_mutex(&ldlm_ref_sem);
        init_mutex(ldlm_namespace_lock(LDLM_NAMESPACE_SERVER));
        init_mutex(ldlm_namespace_lock(LDLM_NAMESPACE_CLIENT));
        ldlm_resource_slab = cfs_mem_cache_create("ldlm_resources",
                                               sizeof(struct ldlm_resource), 0,
                                               SLAB_HWCACHE_ALIGN);
        if (ldlm_resource_slab == NULL)
                return -ENOMEM;

        ldlm_lock_slab = cfs_mem_cache_create("ldlm_locks",
                                      sizeof(struct ldlm_lock), 0,
                                      SLAB_HWCACHE_ALIGN | SLAB_DESTROY_BY_RCU);
        if (ldlm_lock_slab == NULL) {
                cfs_mem_cache_destroy(ldlm_resource_slab);
                return -ENOMEM;
        }

        ldlm_interval_slab = cfs_mem_cache_create("interval_node",
                                        sizeof(struct ldlm_interval),
                                        0, SLAB_HWCACHE_ALIGN);
        if (ldlm_interval_slab == NULL) {
                cfs_mem_cache_destroy(ldlm_resource_slab);
                cfs_mem_cache_destroy(ldlm_lock_slab);
                return -ENOMEM;
        }

        return 0;
}

void __exit ldlm_exit(void)
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

/* ldlm_extent.c */
EXPORT_SYMBOL(ldlm_extent_shift_kms);

/* ldlm_lock.c */
EXPORT_SYMBOL(ldlm_get_processing_policy);
EXPORT_SYMBOL(ldlm_lock2desc);
EXPORT_SYMBOL(ldlm_register_intent);
EXPORT_SYMBOL(ldlm_lockname);
EXPORT_SYMBOL(ldlm_typename);
EXPORT_SYMBOL(ldlm_lock2handle);
EXPORT_SYMBOL(__ldlm_handle2lock);
EXPORT_SYMBOL(ldlm_lock_get);
EXPORT_SYMBOL(ldlm_lock_put);
EXPORT_SYMBOL(ldlm_lock_fast_match);
EXPORT_SYMBOL(ldlm_lock_match);
EXPORT_SYMBOL(ldlm_lock_cancel);
EXPORT_SYMBOL(ldlm_lock_addref);
EXPORT_SYMBOL(ldlm_lock_decref);
EXPORT_SYMBOL(ldlm_lock_decref_and_cancel);
EXPORT_SYMBOL(ldlm_lock_change_resource);
EXPORT_SYMBOL(ldlm_lock_set_data);
EXPORT_SYMBOL(ldlm_it2str);
EXPORT_SYMBOL(ldlm_lock_dump);
EXPORT_SYMBOL(ldlm_lock_dump_handle);
EXPORT_SYMBOL(ldlm_reprocess_all_ns);
EXPORT_SYMBOL(ldlm_lock_allow_match);

/* ldlm_request.c */
EXPORT_SYMBOL(ldlm_completion_ast);
EXPORT_SYMBOL(ldlm_blocking_ast);
EXPORT_SYMBOL(ldlm_glimpse_ast);
EXPORT_SYMBOL(ldlm_expired_completion_wait);
EXPORT_SYMBOL(ldlm_prep_enqueue_req);
EXPORT_SYMBOL(ldlm_prep_elc_req);
EXPORT_SYMBOL(ldlm_cli_convert);
EXPORT_SYMBOL(ldlm_cli_enqueue);
EXPORT_SYMBOL(ldlm_cli_enqueue_fini);
EXPORT_SYMBOL(ldlm_cli_enqueue_local);
EXPORT_SYMBOL(ldlm_cli_cancel);
EXPORT_SYMBOL(ldlm_cli_cancel_unused);
EXPORT_SYMBOL(ldlm_cli_cancel_req);
EXPORT_SYMBOL(ldlm_cli_join_lru);
EXPORT_SYMBOL(ldlm_replay_locks);
EXPORT_SYMBOL(ldlm_resource_foreach);
EXPORT_SYMBOL(ldlm_namespace_foreach);
EXPORT_SYMBOL(ldlm_namespace_foreach_res);
EXPORT_SYMBOL(ldlm_resource_iterate);
EXPORT_SYMBOL(ldlm_cancel_resource_local);
EXPORT_SYMBOL(ldlm_cli_cancel_list);

/* ldlm_lockd.c */
EXPORT_SYMBOL(ldlm_server_blocking_ast);
EXPORT_SYMBOL(ldlm_server_completion_ast);
EXPORT_SYMBOL(ldlm_server_glimpse_ast);
EXPORT_SYMBOL(ldlm_handle_enqueue);
EXPORT_SYMBOL(ldlm_handle_cancel);
EXPORT_SYMBOL(ldlm_request_cancel);
EXPORT_SYMBOL(ldlm_handle_convert);
EXPORT_SYMBOL(ldlm_del_waiting_lock);
EXPORT_SYMBOL(ldlm_get_ref);
EXPORT_SYMBOL(ldlm_put_ref);
EXPORT_SYMBOL(ldlm_refresh_waiting_lock);

/* ldlm_resource.c */
EXPORT_SYMBOL(ldlm_namespace_new);
EXPORT_SYMBOL(ldlm_namespace_cleanup);
EXPORT_SYMBOL(ldlm_namespace_free);
EXPORT_SYMBOL(ldlm_namespace_dump);
EXPORT_SYMBOL(ldlm_dump_all_namespaces);
EXPORT_SYMBOL(ldlm_resource_get);
EXPORT_SYMBOL(ldlm_resource_putref);
EXPORT_SYMBOL(ldlm_resource_unlink_lock);

/* ldlm_lib.c */
EXPORT_SYMBOL(client_import_add_conn);
EXPORT_SYMBOL(client_import_del_conn);
EXPORT_SYMBOL(client_obd_setup);
EXPORT_SYMBOL(client_obd_cleanup);
EXPORT_SYMBOL(client_connect_import);
EXPORT_SYMBOL(client_disconnect_export);
EXPORT_SYMBOL(server_disconnect_export);
EXPORT_SYMBOL(target_abort_recovery);
EXPORT_SYMBOL(target_cleanup_recovery);
EXPORT_SYMBOL(target_handle_connect);
EXPORT_SYMBOL(target_destroy_export);
EXPORT_SYMBOL(target_cancel_recovery_timer);
EXPORT_SYMBOL(target_send_reply);
EXPORT_SYMBOL(target_queue_recovery_request);
EXPORT_SYMBOL(target_handle_ping);
EXPORT_SYMBOL(target_pack_pool_reply);
EXPORT_SYMBOL(target_handle_disconnect);
EXPORT_SYMBOL(target_handle_reply);

/* l_lock.c */
EXPORT_SYMBOL(lock_res_and_lock);
EXPORT_SYMBOL(unlock_res_and_lock);
