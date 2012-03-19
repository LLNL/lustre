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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * Copyright (c) 2011 Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ptlrpc/pinger.c
 *
 * Portal-RPC reconnection and replay operations, for use in recovery.
 */

#ifndef __KERNEL__
#include <liblustre.h>
#else
#define DEBUG_SUBSYSTEM S_RPC
#endif

#include <obd_support.h>
#include <obd_class.h>
#include "ptlrpc_internal.h"

cfs_semaphore_t pinger_sem;
static CFS_LIST_HEAD(pinger_imports);
static cfs_list_t timeout_list = CFS_LIST_HEAD_INIT(timeout_list);
struct ptlrpc_request *
ptlrpc_prep_ping(struct obd_import *imp)
{
        struct ptlrpc_request *req;

        req = ptlrpc_request_alloc_pack(imp, &RQF_OBD_PING,
                                        LUSTRE_OBD_VERSION, OBD_PING);
        if (req) {
                ptlrpc_request_set_replen(req);
                req->rq_no_resend = req->rq_no_delay = 1;
        }
        return req;
}

int ptlrpc_obd_ping(struct obd_device *obd)
{
        int rc;
        struct ptlrpc_request *req;
        ENTRY;

        req = ptlrpc_prep_ping(obd->u.cli.cl_import);
        if (req == NULL)
                RETURN(-ENOMEM);

        req->rq_send_state = LUSTRE_IMP_FULL;

        rc = ptlrpc_queue_wait(req);

        ptlrpc_req_finished(req);

        RETURN(rc);
}
EXPORT_SYMBOL(ptlrpc_obd_ping);

int ptlrpc_ping(struct obd_import *imp)
{
        struct ptlrpc_request *req;
        ENTRY;

        req = ptlrpc_prep_ping(imp);
        if (req == NULL) {
                CERROR("OOM trying to ping %s->%s\n",
                       imp->imp_obd->obd_uuid.uuid,
                       obd2cli_tgt(imp->imp_obd));
                RETURN(-ENOMEM);
        }

        DEBUG_REQ(D_INFO, req, "pinging %s->%s",
                  imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        ptlrpcd_add_req(req, PDL_POLICY_ROUND, -1);

        RETURN(0);
}

void ptlrpc_update_next_ping(struct obd_import *imp, int soon)
{
#ifdef ENABLE_PINGER
        int time = soon ? PING_INTERVAL_SHORT : PING_INTERVAL;
        if (imp->imp_state == LUSTRE_IMP_DISCON) {
                int dtime = max_t(int, CONNECTION_SWITCH_MIN,
                                  AT_OFF ? 0 :
                                  at_get(&imp->imp_at.iat_net_latency));
                time = min(time, dtime);
        }
        imp->imp_next_ping = cfs_time_shift(time);
#endif /* ENABLE_PINGER */
}

void ptlrpc_ping_import_soon(struct obd_import *imp)
{
        imp->imp_next_ping = cfs_time_current();
}

static inline int imp_is_deactive(struct obd_import *imp)
{
        return (imp->imp_deactive ||
                OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_IMP_DEACTIVE));
}

static inline int ptlrpc_next_reconnect(struct obd_import *imp)
{
        if (imp->imp_server_timeout)
                return cfs_time_shift(obd_timeout / 2);
        else
                return cfs_time_shift(obd_timeout);
}

static cfs_atomic_t suspend_timeouts = CFS_ATOMIC_INIT(0);
static cfs_time_t suspend_wakeup_time = 0;

cfs_duration_t pinger_check_timeout(cfs_time_t time)
{
        struct timeout_item *item;
        cfs_time_t timeout = PING_INTERVAL;

        /* The timeout list is a increase order sorted list */
        cfs_mutex_down(&pinger_sem);
        cfs_list_for_each_entry(item, &timeout_list, ti_chain) {
                int ti_timeout = item->ti_timeout;
                if (timeout > ti_timeout)
                        timeout = ti_timeout;
                break;
        }
        cfs_mutex_up(&pinger_sem);

        return cfs_time_sub(cfs_time_add(time, cfs_time_seconds(timeout)),
                                         cfs_time_current());
}

static cfs_waitq_t suspend_timeouts_waitq;

cfs_time_t ptlrpc_suspend_wakeup_time(void)
{
        return suspend_wakeup_time;
}

void ptlrpc_deactivate_timeouts(struct obd_import *imp)
{
        /*XXX: disabled for now, will be replaced by adaptive timeouts */
#if 0
        if (imp->imp_no_timeout)
                return;
        imp->imp_no_timeout = 1;
        cfs_atomic_inc(&suspend_timeouts);
        CDEBUG(D_HA|D_WARNING, "deactivate timeouts %u\n",
               cfs_atomic_read(&suspend_timeouts));
#endif
}

void ptlrpc_activate_timeouts(struct obd_import *imp)
{
        /*XXX: disabled for now, will be replaced by adaptive timeouts */
#if 0
        if (!imp->imp_no_timeout)
                return;
        imp->imp_no_timeout = 0;
        LASSERT(cfs_atomic_read(&suspend_timeouts) > 0);
        if (cfs_atomic_dec_and_test(&suspend_timeouts)) {
                suspend_wakeup_time = cfs_time_current();
                cfs_waitq_signal(&suspend_timeouts_waitq);
        }
        CDEBUG(D_HA|D_WARNING, "activate timeouts %u\n",
               cfs_atomic_read(&suspend_timeouts));
#endif
}

int ptlrpc_check_suspend(void)
{
        if (cfs_atomic_read(&suspend_timeouts))
                return 1;
        return 0;
}

int ptlrpc_check_and_wait_suspend(struct ptlrpc_request *req)
{
        struct l_wait_info lwi;

        if (cfs_atomic_read(&suspend_timeouts)) {
                DEBUG_REQ(D_NET, req, "-- suspend %d regular timeout",
                          cfs_atomic_read(&suspend_timeouts));
                lwi = LWI_INTR(NULL, NULL);
                l_wait_event(suspend_timeouts_waitq,
                             cfs_atomic_read(&suspend_timeouts) == 0, &lwi);
                DEBUG_REQ(D_NET, req, "-- recharge regular timeout");
                return 1;
        }
        return 0;
}

#ifdef __KERNEL__

static void ptlrpc_pinger_process_import(struct obd_import *imp,
                                         unsigned long this_ping)
{
        int force, level;

        cfs_spin_lock(&imp->imp_lock);
        level = imp->imp_state;
        force = imp->imp_force_verify;
        if (force)
                imp->imp_force_verify = 0;
        cfs_spin_unlock(&imp->imp_lock);

        CDEBUG(level == LUSTRE_IMP_FULL ? D_INFO : D_HA,
               "level %s/%u force %u deactive %u pingable %u\n",
               ptlrpc_import_state_name(level), level,
               force, imp->imp_deactive, imp->imp_pingable);

        if (cfs_time_aftereq(imp->imp_next_ping - 5 * CFS_TICK,
                             this_ping) && force == 0)
                return;

        if (level == LUSTRE_IMP_DISCON && !imp_is_deactive(imp)) {
                /* wait for a while before trying recovery again */
                imp->imp_next_ping = ptlrpc_next_reconnect(imp);
                if (!imp->imp_no_pinger_recover)
                        ptlrpc_initiate_recovery(imp);
        } else if (level != LUSTRE_IMP_FULL ||
                   imp->imp_obd->obd_no_recov ||
                   imp_is_deactive(imp)) {
                CDEBUG(D_HA, "not pinging %s (in recovery "
                       " or recovery disabled: %s)\n",
                       obd2cli_tgt(imp->imp_obd),
                       ptlrpc_import_state_name(level));
        } else if (imp->imp_pingable || force) {
                ptlrpc_ping(imp);
        }
}

static int ptlrpc_pinger_main(void *arg)
{
        struct ptlrpc_svc_data *data = (struct ptlrpc_svc_data *)arg;
        struct ptlrpc_thread *thread = data->thread;
        ENTRY;

        cfs_daemonize(data->name);

        /* Record that the thread is running */
        thread_set_flags(thread, SVC_RUNNING);
        cfs_waitq_signal(&thread->t_ctl_waitq);

        /* And now, loop forever, pinging as needed. */
        while (1) {
                cfs_time_t this_ping = cfs_time_current();
                struct l_wait_info lwi;
                cfs_duration_t time_to_next_wake;
                struct timeout_item *item;
                cfs_list_t *iter;

                cfs_mutex_down(&pinger_sem);
                cfs_list_for_each_entry(item, &timeout_list, ti_chain) {
                        item->ti_cb(item, item->ti_cb_data);
                }
                cfs_list_for_each(iter, &pinger_imports) {
                        struct obd_import *imp =
                                cfs_list_entry(iter, struct obd_import,
                                               imp_pinger_chain);

                        ptlrpc_pinger_process_import(imp, this_ping);
                        /* obd_timeout might have changed */
                        if (imp->imp_pingable && imp->imp_next_ping &&
                            cfs_time_after(imp->imp_next_ping,
                                           cfs_time_add(this_ping,
                                                        cfs_time_seconds(PING_INTERVAL))))
                                ptlrpc_update_next_ping(imp, 0);
                }
                cfs_mutex_up(&pinger_sem);
                /* update memory usage info */
                obd_update_maxusage();

                /* Wait until the next ping time, or until we're stopped. */
                time_to_next_wake = pinger_check_timeout(this_ping);
                /* The ping sent by ptlrpc_send_rpc may get sent out
                   say .01 second after this.
                   ptlrpc_pinger_sending_on_import will then set the
                   next ping time to next_ping + .01 sec, which means
                   we will SKIP the next ping at next_ping, and the
                   ping will get sent 2 timeouts from now!  Beware. */
                CDEBUG(D_INFO, "next wakeup in "CFS_DURATION_T" ("
                       CFS_TIME_T")\n", time_to_next_wake,
                       cfs_time_add(this_ping,cfs_time_seconds(PING_INTERVAL)));
                if (time_to_next_wake > 0) {
                        lwi = LWI_TIMEOUT(max_t(cfs_duration_t,
                                                time_to_next_wake,
                                                cfs_time_seconds(1)),
                                          NULL, NULL);
                        l_wait_event(thread->t_ctl_waitq,
                                     thread_is_stopping(thread) ||
                                     thread_is_event(thread),
                                     &lwi);
                        if (thread_test_and_clear_flags(thread, SVC_STOPPING)) {
                                EXIT;
                                break;
                        } else {
                                /* woken after adding import to reset timer */
                                thread_test_and_clear_flags(thread, SVC_EVENT);
                        }
                }
        }

        thread_set_flags(thread, SVC_STOPPED);
        cfs_waitq_signal(&thread->t_ctl_waitq);

        CDEBUG(D_NET, "pinger thread exiting, process %d\n", cfs_curproc_pid());
        return 0;
}

static struct ptlrpc_thread *pinger_thread = NULL;

int ptlrpc_start_pinger(void)
{
        struct l_wait_info lwi = { 0 };
        struct ptlrpc_svc_data d;
        int rc;
#ifndef ENABLE_PINGER
        return 0;
#endif
        ENTRY;

        if (pinger_thread != NULL)
                RETURN(-EALREADY);

        OBD_ALLOC_PTR(pinger_thread);
        if (pinger_thread == NULL)
                RETURN(-ENOMEM);
        cfs_waitq_init(&pinger_thread->t_ctl_waitq);
        cfs_waitq_init(&suspend_timeouts_waitq);

        d.name = "ll_ping";
        d.thread = pinger_thread;

        /* CLONE_VM and CLONE_FILES just avoid a needless copy, because we
         * just drop the VM and FILES in cfs_daemonize_ctxt() right away. */
        rc = cfs_create_thread(ptlrpc_pinger_main, &d, CFS_DAEMON_FLAGS);
        if (rc < 0) {
                CERROR("cannot start thread: %d\n", rc);
                OBD_FREE(pinger_thread, sizeof(*pinger_thread));
                pinger_thread = NULL;
                RETURN(rc);
        }
        l_wait_event(pinger_thread->t_ctl_waitq,
                     thread_is_running(pinger_thread), &lwi);

        RETURN(0);
}

int ptlrpc_pinger_remove_timeouts(void);

int ptlrpc_stop_pinger(void)
{
        struct l_wait_info lwi = { 0 };
        int rc = 0;
#ifndef ENABLE_PINGER
        return 0;
#endif
        ENTRY;

        if (pinger_thread == NULL)
                RETURN(-EALREADY);

        ptlrpc_pinger_remove_timeouts();
        cfs_mutex_down(&pinger_sem);
        thread_set_flags(pinger_thread, SVC_STOPPING);
        cfs_waitq_signal(&pinger_thread->t_ctl_waitq);
        cfs_mutex_up(&pinger_sem);

        l_wait_event(pinger_thread->t_ctl_waitq,
                     thread_is_stopped(pinger_thread), &lwi);

        OBD_FREE_PTR(pinger_thread);
        pinger_thread = NULL;
        RETURN(rc);
}

void ptlrpc_pinger_sending_on_import(struct obd_import *imp)
{
        ptlrpc_update_next_ping(imp, 0);
}

void ptlrpc_pinger_commit_expected(struct obd_import *imp)
{
        ptlrpc_update_next_ping(imp, 1);
}

int ptlrpc_pinger_add_import(struct obd_import *imp)
{
        ENTRY;
        if (!cfs_list_empty(&imp->imp_pinger_chain))
                RETURN(-EALREADY);

        cfs_mutex_down(&pinger_sem);
        CDEBUG(D_HA, "adding pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        /* if we add to pinger we want recovery on this import */
        imp->imp_obd->obd_no_recov = 0;
        ptlrpc_update_next_ping(imp, 0);
        /* XXX sort, blah blah */
        cfs_list_add_tail(&imp->imp_pinger_chain, &pinger_imports);
        class_import_get(imp);

        ptlrpc_pinger_wake_up();
        cfs_mutex_up(&pinger_sem);

        RETURN(0);
}

int ptlrpc_pinger_del_import(struct obd_import *imp)
{
        ENTRY;
        if (cfs_list_empty(&imp->imp_pinger_chain))
                RETURN(-ENOENT);

        cfs_mutex_down(&pinger_sem);
        cfs_list_del_init(&imp->imp_pinger_chain);
        CDEBUG(D_HA, "removing pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        /* if we remove from pinger we don't want recovery on this import */
        imp->imp_obd->obd_no_recov = 1;
        class_import_put(imp);
        cfs_mutex_up(&pinger_sem);
        RETURN(0);
}

/**
 * Register a timeout callback to the pinger list, and the callback will
 * be called when timeout happens.
 */
struct timeout_item* ptlrpc_new_timeout(int time, enum timeout_event event,
                                        timeout_cb_t cb, void *data)
{
        struct timeout_item *ti;

        OBD_ALLOC_PTR(ti);
        if (!ti)
                return(NULL);

        CFS_INIT_LIST_HEAD(&ti->ti_obd_list);
        CFS_INIT_LIST_HEAD(&ti->ti_chain);
        ti->ti_timeout = time;
        ti->ti_event = event;
        ti->ti_cb = cb;
        ti->ti_cb_data = data;

        return ti;
}

/**
 * Register timeout event on the the pinger thread.
 * Note: the timeout list is an sorted list with increased timeout value.
 */
static struct timeout_item*
ptlrpc_pinger_register_timeout(int time, enum timeout_event event,
                               timeout_cb_t cb, void *data)
{
        struct timeout_item *item, *tmp;

        LASSERT_SEM_LOCKED(&pinger_sem);

        cfs_list_for_each_entry(item, &timeout_list, ti_chain)
                if (item->ti_event == event)
                        goto out;

        item = ptlrpc_new_timeout(time, event, cb, data);
        if (item) {
                cfs_list_for_each_entry_reverse(tmp, &timeout_list, ti_chain) {
                        if (tmp->ti_timeout < time) {
                                cfs_list_add(&item->ti_chain, &tmp->ti_chain);
                                goto out;
                        }
                }
                cfs_list_add(&item->ti_chain, &timeout_list);
        }
out:
        return item;
}

/* Add a client_obd to the timeout event list, when timeout(@time)
 * happens, the callback(@cb) will be called.
 */
int ptlrpc_add_timeout_client(int time, enum timeout_event event,
                              timeout_cb_t cb, void *data,
                              cfs_list_t *obd_list)
{
        struct timeout_item *ti;

        cfs_mutex_down(&pinger_sem);
        ti = ptlrpc_pinger_register_timeout(time, event, cb, data);
        if (!ti) {
                cfs_mutex_up(&pinger_sem);
                return (-EINVAL);
        }
        cfs_list_add(obd_list, &ti->ti_obd_list);
        cfs_mutex_up(&pinger_sem);
        return 0;
}

int ptlrpc_del_timeout_client(cfs_list_t *obd_list,
                              enum timeout_event event)
{
        struct timeout_item *ti = NULL, *item;

        if (cfs_list_empty(obd_list))
                return 0;
        cfs_mutex_down(&pinger_sem);
        cfs_list_del_init(obd_list);
        /**
         * If there are no obd attached to the timeout event
         * list, remove this timeout event from the pinger
         */
        cfs_list_for_each_entry(item, &timeout_list, ti_chain) {
                if (item->ti_event == event) {
                        ti = item;
                        break;
                }
        }
        LASSERTF(ti != NULL, "ti is NULL ! \n");
        if (cfs_list_empty(&ti->ti_obd_list)) {
                cfs_list_del(&ti->ti_chain);
                OBD_FREE_PTR(ti);
        }
        cfs_mutex_up(&pinger_sem);
        return 0;
}

int ptlrpc_pinger_remove_timeouts(void)
{
        struct timeout_item *item, *tmp;

        cfs_mutex_down(&pinger_sem);
        cfs_list_for_each_entry_safe(item, tmp, &timeout_list, ti_chain) {
                LASSERT(cfs_list_empty(&item->ti_obd_list));
                cfs_list_del(&item->ti_chain);
                OBD_FREE_PTR(item);
        }
        cfs_mutex_up(&pinger_sem);
        return 0;
}

void ptlrpc_pinger_wake_up()
{
#ifdef ENABLE_PINGER
        thread_add_flags(pinger_thread, SVC_EVENT);
        cfs_waitq_signal(&pinger_thread->t_ctl_waitq);
#endif
}

/* Ping evictor thread */
#define PET_READY     1
#define PET_TERMINATE 2

static int               pet_refcount = 0;
static int               pet_state;
static cfs_waitq_t       pet_waitq;
CFS_LIST_HEAD(pet_list);
static cfs_spinlock_t    pet_lock = CFS_SPIN_LOCK_UNLOCKED;

int ping_evictor_wake(struct obd_export *exp)
{
        struct obd_device *obd;

        cfs_spin_lock(&pet_lock);
        if (pet_state != PET_READY) {
                /* eventually the new obd will call here again. */
                cfs_spin_unlock(&pet_lock);
                return 1;
        }

        obd = class_exp2obd(exp);
        if (cfs_list_empty(&obd->obd_evict_list)) {
                class_incref(obd, "evictor", obd);
                cfs_list_add(&obd->obd_evict_list, &pet_list);
        }
        cfs_spin_unlock(&pet_lock);

        cfs_waitq_signal(&pet_waitq);
        return 0;
}

static int ping_evictor_main(void *arg)
{
        struct obd_device *obd;
        struct obd_export *exp;
        struct l_wait_info lwi = { 0 };
        time_t expire_time;
        ENTRY;

        cfs_daemonize_ctxt("ll_evictor");

        CDEBUG(D_HA, "Starting Ping Evictor\n");
        pet_state = PET_READY;
        while (1) {
                l_wait_event(pet_waitq, (!cfs_list_empty(&pet_list)) ||
                             (pet_state == PET_TERMINATE), &lwi);

                /* loop until all obd's will be removed */
                if ((pet_state == PET_TERMINATE) && cfs_list_empty(&pet_list))
                        break;

                /* we only get here if pet_exp != NULL, and the end of this
                 * loop is the only place which sets it NULL again, so lock
                 * is not strictly necessary. */
                cfs_spin_lock(&pet_lock);
                obd = cfs_list_entry(pet_list.next, struct obd_device,
                                     obd_evict_list);
                cfs_spin_unlock(&pet_lock);

                expire_time = cfs_time_current_sec() - PING_EVICT_TIMEOUT;

                CDEBUG(D_HA, "evicting all exports of obd %s older than %ld\n",
                       obd->obd_name, expire_time);

                /* Exports can't be deleted out of the list while we hold
                 * the obd lock (class_unlink_export), which means we can't
                 * lose the last ref on the export.  If they've already been
                 * removed from the list, we won't find them here. */
                cfs_spin_lock(&obd->obd_dev_lock);
                while (!cfs_list_empty(&obd->obd_exports_timed)) {
                        exp = cfs_list_entry(obd->obd_exports_timed.next,
                                             struct obd_export,
                                             exp_obd_chain_timed);
                        if (expire_time > exp->exp_last_request_time) {
                                class_export_get(exp);
                                cfs_spin_unlock(&obd->obd_dev_lock);
                                LCONSOLE_WARN("%s: haven't heard from client %s"
                                              " (at %s) in %ld seconds. I think"
                                              " it's dead, and I am evicting"
                                              " it. exp %p, cur %ld expire %ld"
                                              " last %ld\n",
                                              obd->obd_name,
                                              obd_uuid2str(&exp->exp_client_uuid),
                                              obd_export_nid2str(exp),
                                              (long)(cfs_time_current_sec() -
                                                     exp->exp_last_request_time),
                                              exp, (long)cfs_time_current_sec(),
                                              (long)expire_time,
                                              (long)exp->exp_last_request_time);
                                CDEBUG(D_HA, "Last request was at %ld\n",
                                       exp->exp_last_request_time);
                                class_fail_export(exp);
                                class_export_put(exp);
                                cfs_spin_lock(&obd->obd_dev_lock);
                        } else {
                                /* List is sorted, so everyone below is ok */
                                break;
                        }
                }
                cfs_spin_unlock(&obd->obd_dev_lock);

                cfs_spin_lock(&pet_lock);
                cfs_list_del_init(&obd->obd_evict_list);
                cfs_spin_unlock(&pet_lock);

                class_decref(obd, "evictor", obd);
        }
        CDEBUG(D_HA, "Exiting Ping Evictor\n");

        RETURN(0);
}

void ping_evictor_start(void)
{
        int rc;

        if (++pet_refcount > 1)
                return;

        cfs_waitq_init(&pet_waitq);

        rc = cfs_create_thread(ping_evictor_main, NULL, CFS_DAEMON_FLAGS);
        if (rc < 0) {
                pet_refcount--;
                CERROR("Cannot start ping evictor thread: %d\n", rc);
        }
}
EXPORT_SYMBOL(ping_evictor_start);

void ping_evictor_stop(void)
{
        if (--pet_refcount > 0)
                return;

        pet_state = PET_TERMINATE;
        cfs_waitq_signal(&pet_waitq);
}
EXPORT_SYMBOL(ping_evictor_stop);
#else /* !__KERNEL__ */

/* XXX
 * the current implementation of pinger in liblustre is not optimized
 */

#ifdef ENABLE_PINGER
static struct pinger_data {
        int             pd_recursion;
        cfs_time_t      pd_this_ping;   /* jiffies */
        cfs_time_t      pd_next_ping;   /* jiffies */
        struct ptlrpc_request_set *pd_set;
} pinger_args;

static int pinger_check_rpcs(void *arg)
{
        cfs_time_t curtime = cfs_time_current();
        struct ptlrpc_request *req;
        struct ptlrpc_request_set *set;
        cfs_list_t *iter;
        struct obd_import *imp;
        struct pinger_data *pd = &pinger_args;
        int rc;

        /* prevent recursion */
        if (pd->pd_recursion++) {
                CDEBUG(D_HA, "pinger: recursion! quit\n");
                LASSERT(pd->pd_set);
                pd->pd_recursion--;
                return 0;
        }

        /* have we reached ping point? */
        if (!pd->pd_set && cfs_time_before(curtime, pd->pd_next_ping)) {
                pd->pd_recursion--;
                return 0;
        }

        /* if we have rpc_set already, continue processing it */
        if (pd->pd_set) {
                LASSERT(pd->pd_this_ping);
                set = pd->pd_set;
                goto do_check_set;
        }

        pd->pd_this_ping = curtime;
        pd->pd_set = ptlrpc_prep_set();
        if (pd->pd_set == NULL)
                goto out;
        set = pd->pd_set;

        /* add rpcs into set */
        cfs_mutex_down(&pinger_sem);
        cfs_list_for_each(iter, &pinger_imports) {
                struct obd_import *imp = cfs_list_entry(iter, struct obd_import,
                                                        imp_pinger_chain);
                int generation, level;

                if (cfs_time_aftereq(pd->pd_this_ping,
                                     imp->imp_next_ping - 5 * CFS_TICK)) {
                        /* Add a ping. */
                        cfs_spin_lock(&imp->imp_lock);
                        generation = imp->imp_generation;
                        level = imp->imp_state;
                        cfs_spin_unlock(&imp->imp_lock);

                        if (level != LUSTRE_IMP_FULL) {
                                CDEBUG(D_HA,
                                       "not pinging %s (in recovery)\n",
                                       obd2cli_tgt(imp->imp_obd));
                                continue;
                        }

                        req = ptlrpc_request_alloc_pack(imp, &RQF_OBD_PING,
                                                        LUSTRE_OBD_VERSION,
                                                        OBD_PING);
                        if (req == NULL) {
                                CERROR("OOM trying to ping %s->%s\n",
                                       imp->imp_obd->obd_uuid.uuid,
                                       obd2cli_tgt(imp->imp_obd));
                                break;
                        }

                        req->rq_no_resend = 1;
                        ptlrpc_request_set_replen(req);
                        req->rq_send_state = LUSTRE_IMP_FULL;
                        ptlrpc_rqphase_move(req, RQ_PHASE_RPC);
                        req->rq_import_generation = generation;
                        ptlrpc_set_add_req(set, req);
                } else {
                        CDEBUG(D_INFO, "don't need to ping %s ("CFS_TIME_T
                               " > "CFS_TIME_T")\n", obd2cli_tgt(imp->imp_obd),
                               imp->imp_next_ping, pd->pd_this_ping);
                }
        }
        pd->pd_this_ping = curtime;
        cfs_mutex_up(&pinger_sem);

        /* Might be empty, that's OK. */
        if (cfs_atomic_read(&set->set_remaining) == 0)
                CDEBUG(D_RPCTRACE, "nothing to ping\n");

        cfs_list_for_each(iter, &set->set_requests) {
                struct ptlrpc_request *req =
                        cfs_list_entry(iter, struct ptlrpc_request,
                                       rq_set_chain);
                DEBUG_REQ(D_RPCTRACE, req, "pinging %s->%s",
                          req->rq_import->imp_obd->obd_uuid.uuid,
                          obd2cli_tgt(req->rq_import->imp_obd));
                (void)ptl_send_rpc(req, 0);
        }

do_check_set:
        rc = ptlrpc_check_set(NULL, set);

        /* not finished, and we are not expired, simply return */
        if (!rc && cfs_time_before(curtime, cfs_time_add(pd->pd_this_ping,
                                            cfs_time_seconds(PING_INTERVAL)))) {
                CDEBUG(D_RPCTRACE, "not finished, but also not expired\n");
                pd->pd_recursion--;
                return 0;
        }

        /* Expire all the requests that didn't come back. */
        cfs_mutex_down(&pinger_sem);
        cfs_list_for_each(iter, &set->set_requests) {
                req = cfs_list_entry(iter, struct ptlrpc_request,
                                     rq_set_chain);

                if (req->rq_phase == RQ_PHASE_COMPLETE)
                        continue;

                CDEBUG(D_RPCTRACE, "Pinger initiate expire request(%p)\n",
                       req);

                /* This will also unregister reply. */
                ptlrpc_expire_one_request(req, 0);

                /* We're done with this req, let's finally move it to complete
                 * phase and take care of inflights. */
                ptlrpc_rqphase_move(req, RQ_PHASE_COMPLETE);
                imp = req->rq_import;
                cfs_spin_lock(&imp->imp_lock);
                if (!cfs_list_empty(&req->rq_list)) {
                        cfs_list_del_init(&req->rq_list);
                        cfs_atomic_dec(&imp->imp_inflight);
                }
                cfs_spin_unlock(&imp->imp_lock);
                cfs_atomic_dec(&set->set_remaining);
        }
        cfs_mutex_up(&pinger_sem);

        ptlrpc_set_destroy(set);
        pd->pd_set = NULL;

out:
        pd->pd_next_ping = cfs_time_add(pd->pd_this_ping,
                                        cfs_time_seconds(PING_INTERVAL));
        pd->pd_this_ping = 0; /* XXX for debug */

        CDEBUG(D_INFO, "finished a round ping\n");
        pd->pd_recursion--;
        return 0;
}

static void *pinger_callback = NULL;
#endif /* ENABLE_PINGER */

int ptlrpc_start_pinger(void)
{
#ifdef ENABLE_PINGER
        memset(&pinger_args, 0, sizeof(pinger_args));
        pinger_callback = liblustre_register_wait_callback("pinger_check_rpcs",
                                                           &pinger_check_rpcs,
                                                           &pinger_args);
#endif
        return 0;
}

int ptlrpc_stop_pinger(void)
{
#ifdef ENABLE_PINGER
        if (pinger_callback)
                liblustre_deregister_wait_callback(pinger_callback);
#endif
        return 0;
}

void ptlrpc_pinger_sending_on_import(struct obd_import *imp)
{
#ifdef ENABLE_PINGER
        cfs_mutex_down(&pinger_sem);
        ptlrpc_update_next_ping(imp, 0);
        if (pinger_args.pd_set == NULL &&
            cfs_time_before(imp->imp_next_ping, pinger_args.pd_next_ping)) {
                CDEBUG(D_HA, "set next ping to "CFS_TIME_T"(cur "CFS_TIME_T")\n",
                        imp->imp_next_ping, cfs_time_current());
                pinger_args.pd_next_ping = imp->imp_next_ping;
        }
        cfs_mutex_up(&pinger_sem);
#endif
}

void ptlrpc_pinger_commit_expected(struct obd_import *imp)
{
#ifdef ENABLE_PINGER
        cfs_mutex_down(&pinger_sem);
        ptlrpc_update_next_ping(imp, 1);
        if (pinger_args.pd_set == NULL &&
            cfs_time_before(imp->imp_next_ping, pinger_args.pd_next_ping)) {
                CDEBUG(D_HA,"set next ping to "CFS_TIME_T"(cur "CFS_TIME_T")\n",
                        imp->imp_next_ping, cfs_time_current());
                pinger_args.pd_next_ping = imp->imp_next_ping;
        }
        cfs_mutex_up(&pinger_sem);
#endif
}

int ptlrpc_add_timeout_client(int time, enum timeout_event event,
                              timeout_cb_t cb, void *data,
                              cfs_list_t *obd_list)
{
        return 0;
}

int ptlrpc_del_timeout_client(cfs_list_t *obd_list,
                              enum timeout_event event)
{
        return 0;
}

int ptlrpc_pinger_add_import(struct obd_import *imp)
{
        ENTRY;
        if (!cfs_list_empty(&imp->imp_pinger_chain))
                RETURN(-EALREADY);

        CDEBUG(D_HA, "adding pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        ptlrpc_pinger_sending_on_import(imp);

        cfs_mutex_down(&pinger_sem);
        cfs_list_add_tail(&imp->imp_pinger_chain, &pinger_imports);
        class_import_get(imp);
        cfs_mutex_up(&pinger_sem);

        RETURN(0);
}

int ptlrpc_pinger_del_import(struct obd_import *imp)
{
        ENTRY;
        if (cfs_list_empty(&imp->imp_pinger_chain))
                RETURN(-ENOENT);

        cfs_mutex_down(&pinger_sem);
        cfs_list_del_init(&imp->imp_pinger_chain);
        CDEBUG(D_HA, "removing pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        class_import_put(imp);
        cfs_mutex_up(&pinger_sem);
        RETURN(0);
}

void ptlrpc_pinger_wake_up()
{
#ifdef ENABLE_PINGER
        /* XXX force pinger to run, if needed */
        struct obd_import *imp;
        ENTRY;
        cfs_list_for_each_entry(imp, &pinger_imports, imp_pinger_chain) {
                CDEBUG(D_RPCTRACE, "checking import %s->%s\n",
                       imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
#ifdef ENABLE_LIBLUSTRE_RECOVERY
                if (imp->imp_state == LUSTRE_IMP_DISCON &&
                    !imp_is_deactive(imp))
#else
                /*XXX only recover for the initial connection */
                if (!lustre_handle_is_used(&imp->imp_remote_handle) &&
                    imp->imp_state == LUSTRE_IMP_DISCON &&
                    !imp_is_deactive(imp))
#endif
                        ptlrpc_initiate_recovery(imp);
                else if (imp->imp_state != LUSTRE_IMP_FULL)
                        CDEBUG(D_HA, "Refused to recover import %s->%s "
                                     "state %d, deactive %d\n",
                                     imp->imp_obd->obd_uuid.uuid,
                                     obd2cli_tgt(imp->imp_obd), imp->imp_state,
                                     imp_is_deactive(imp));
        }
        EXIT;
#endif
}
#endif /* !__KERNEL__ */
