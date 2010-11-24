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
#include <lustre_net.h>
#include "ptlrpc_internal.h"

#ifdef __KERNEL__
/* What time pinger is supposed to wake up next.
 * pd_next_ping is the equivalent for liblustre. */
cfs_time_t pinger_next_wake;
#endif

struct semaphore pinger_sem;
static struct list_head pinger_imports = CFS_LIST_HEAD_INIT(pinger_imports);
static struct list_head timeout_list = CFS_LIST_HEAD_INIT(timeout_list);

struct ptlrpc_request *
ptlrpc_prep_ping(struct obd_import *imp)
{
        struct ptlrpc_request *req;

        req = ptlrpc_prep_req(imp, LUSTRE_OBD_VERSION,
                              OBD_PING, 1, NULL, NULL);
        if (req) {
                ptlrpc_req_set_repsize(req, 1, NULL);
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
        int rc = 0;
        ENTRY;

        req = ptlrpc_prep_ping(imp);
        if (req) {
                DEBUG_REQ(D_INFO, req, "pinging %s->%s",
                          imp->imp_obd->obd_uuid.uuid,
                          obd2cli_tgt(imp->imp_obd));

                /* To quickly detect server failure ping timeouts must be
                 * kept small.  Therefore we must override/ignore the server
                 * rpc completion estimate which may be very large since
                 * it includes non-ping service times.  The right long term
                 * fix will be to add a per-server (not per-service) thread
                 * in order to reduce the number of pings in the system in
                 * general (see bug 12471). */
                if (!AT_OFF) {
                        req->rq_timeout = PING_SVC_TIMEOUT +
                                          at_get(&imp->imp_at.iat_net_latency);
                        lustre_msg_set_timeout(req->rq_reqmsg, req->rq_timeout);
                }

                ptlrpcd_add_req(req);
        } else {
                CERROR("OOM trying to ping %s->%s\n",
                       imp->imp_obd->obd_uuid.uuid,
                       obd2cli_tgt(imp->imp_obd));
                rc = -ENOMEM;
        }

        RETURN(rc);
}
EXPORT_SYMBOL(ptlrpc_ping);

static void ptlrpc_update_next_ping(struct obd_import *imp, int soon)
{
#ifdef ENABLE_PINGER
        cfs_time_t delay, dtime, ctime = cfs_time_current();

        if (imp->imp_state == LUSTRE_IMP_DISCON ||
            imp->imp_state == LUSTRE_IMP_CONNECTING) {
                /* In the disconnected case aggressively reconnect, for
                 * this request the AT service timeout will be set to
                 * INITIAL_CONNECT_TIMEOUT.  To ensure the request times
                 * out before we send another we add one extra second. */
                dtime = cfs_time_seconds(max_t(int, CONNECTION_SWITCH_MIN,
                                AT_OFF ? 0 : INITIAL_CONNECT_TIMEOUT + 1 +
                                at_get(&imp->imp_at.iat_net_latency)));
        } else {
                /* In the common case we want to cluster the pings at
                 * at regular intervals to minimize system noise. */
                delay = cfs_time_seconds(soon ? PING_INTERVAL_SHORT :
                                         PING_INTERVAL);
                dtime = delay - (ctime % delay);
        }

        dtime = cfs_time_add(ctime, dtime);

        if (soon && cfs_time_after(imp->imp_next_ping, ctime) &&
            cfs_time_after(dtime, imp->imp_next_ping)) {
                /* if the next ping is due to be sent before the
                 * new deadline, don't delay it */
                 return;
        }

        /* May harmlessly race with ptlrpc_update_next_ping() */
        imp->imp_next_ping = dtime;

#ifdef __KERNEL__
        if (pinger_next_wake != 0 && cfs_time_after(pinger_next_wake, dtime))
                /* pinger is supposed to sleep until after the new ping
                 * deadline, wake it up to take into account our update.
                 * no needed for liblustre which updates pd_next_ping. */
                ptlrpc_pinger_wake_up();
#endif

        CDEBUG(D_INFO, "Setting %s next ping to "CFS_TIME_T" ("CFS_TIME_T")\n",
               obd2cli_tgt(imp->imp_obd), imp->imp_next_ping, dtime);

#endif /* ENABLE_PINGER */
}

static inline int imp_is_deactive(struct obd_import *imp)
{
        return (imp->imp_deactive ||
                OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_IMP_DEACTIVE));
}

cfs_duration_t pinger_check_timeout(cfs_time_t time)
{
        struct timeout_item *item;
        cfs_time_t timeout = PING_INTERVAL;

        /* The timeout list is a increase order sorted list */
        mutex_down(&pinger_sem);
        list_for_each_entry(item, &timeout_list, ti_chain) {
                int ti_timeout = item->ti_timeout;
                if (timeout > ti_timeout)
                         timeout = ti_timeout;
                break;
        }
        mutex_up(&pinger_sem);

        return cfs_time_sub(cfs_time_add(time, cfs_time_seconds(timeout)),
                                         cfs_time_current());
}

#ifdef __KERNEL__
static int ptlrpc_pinger_main(void *arg)
{
        struct ptlrpc_svc_data *data = (struct ptlrpc_svc_data *)arg;
        struct ptlrpc_thread *thread = data->thread;
        ENTRY;

        cfs_daemonize(data->name);

        /* Record that the thread is running */
        thread->t_flags = SVC_RUNNING;
        cfs_waitq_signal(&thread->t_ctl_waitq);

        /* And now, loop forever, pinging as needed. */
        while (1) {
                cfs_time_t this_ping = cfs_time_current();
                struct l_wait_info lwi;
                cfs_duration_t time_to_next_wake;
                cfs_time_t time_of_next_wake;
                struct timeout_item *item;
                struct list_head *iter;

                time_of_next_wake = cfs_time_shift(PING_INTERVAL);

                mutex_down(&pinger_sem);
                list_for_each_entry(item, &timeout_list, ti_chain) {
                        item->ti_cb(item, item->ti_cb_data);
                }
                list_for_each(iter, &pinger_imports) {
                        struct obd_import *imp =
                                list_entry(iter, struct obd_import,
                                           imp_pinger_chain);
                        int force, level;

                        spin_lock(&imp->imp_lock);
                        level = imp->imp_state;
                        force = imp->imp_force_verify;
                        imp->imp_force_verify = 0;
                        spin_unlock(&imp->imp_lock);

                        CDEBUG(level == LUSTRE_IMP_FULL ? D_INFO : D_RPCTRACE,
                               "level %s/%u force %u deactive %u pingable %u\n",
                               ptlrpc_import_state_name(level), level,
                               force, imp->imp_deactive, imp->imp_pingable);

                        /* Include any ping which misses the deadline by up to
                         * 1/10 of a second.  The pings are designed to clump
                         * and this helps ensure the entire batch gets sent
                         * promptly, which minimizes system noise from pings */

                        if (force ||
                            cfs_time_aftereq(this_ping, imp->imp_next_ping -
                                             (cfs_time_seconds(1) + 9) / 10)) {
                                if (level == LUSTRE_IMP_DISCON &&
                                    !imp_is_deactive(imp)) {
                                        ptlrpc_update_next_ping(imp, 0);
                                        ptlrpc_initiate_recovery(imp);
                                } else if (level != LUSTRE_IMP_FULL ||
                                         imp->imp_obd->obd_no_recov ||
                                         imp_is_deactive(imp)) {
                                        CDEBUG(D_HA, "not pinging %s "
                                               "(in recovery: %s or recovery "
                                               "disabled: %u/%u)\n",
                                               obd2cli_tgt(imp->imp_obd),
                                               ptlrpc_import_state_name(level),
                                               imp->imp_deactive,
                                               imp->imp_obd->obd_no_recov);
                                } else if (imp->imp_pingable || force) {
                                                ptlrpc_ping(imp);
                                                /* ptlrpc_pinger_sending_on_import()
                                                 * will asynch update imp_next_ping
                                                 * so it must not be used below to
                                                 * calculate minimum wait time. */
                                                continue;
                                }
                        } else {
                                if (!imp->imp_pingable)
                                        continue;
                                CDEBUG(D_INFO,
                                       "don't need to ping %s ("CFS_TIME_T
                                       " > "CFS_TIME_T")\n",
                                       obd2cli_tgt(imp->imp_obd),
                                       imp->imp_next_ping, this_ping);
                        }

                        /* Wait time until next ping, or until we stopped. */
                        if (cfs_time_before(imp->imp_next_ping,
                                            time_of_next_wake))
                                time_of_next_wake = imp->imp_next_ping;
                }
                pinger_next_wake = time_of_next_wake;
                mutex_up(&pinger_sem);
                obd_update_maxusage();

                time_to_next_wake = max_t(cfs_duration_t,
                                          cfs_time_seconds(1),
                                          cfs_time_sub(time_of_next_wake,
                                                       cfs_time_current()));
                CDEBUG(D_INFO, "next ping in "CFS_DURATION_T" ("CFS_TIME_T")\n",
                               time_to_next_wake, time_of_next_wake);

                if (time_to_next_wake > 0) {
                        lwi = LWI_TIMEOUT(time_to_next_wake, NULL, NULL);
                        l_wait_event(thread->t_ctl_waitq,
                                     thread->t_flags & (SVC_STOPPING|SVC_EVENT),
                                     &lwi);
                        if (thread->t_flags & SVC_STOPPING) {
                                thread->t_flags &= ~SVC_STOPPING;
                                EXIT;
                                break;
                        } else if (thread->t_flags & SVC_EVENT) {
                                /* woken after adding import to reset timer */
                                thread->t_flags &= ~SVC_EVENT;
                        }
                }
        }

        thread->t_flags = SVC_STOPPED;
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

        OBD_ALLOC(pinger_thread, sizeof(*pinger_thread));
        if (pinger_thread == NULL)
                RETURN(-ENOMEM);
        cfs_waitq_init(&pinger_thread->t_ctl_waitq);

        d.name = "ll_ping";
        d.thread = pinger_thread;

        /* CLONE_VM and CLONE_FILES just avoid a needless copy, because we
         * just drop the VM and FILES in cfs_daemonize_ctxt() right away. */
        rc = cfs_kernel_thread(ptlrpc_pinger_main, &d, CLONE_VM | CLONE_FILES);
        if (rc < 0) {
                CERROR("cannot start thread: %d\n", rc);
                OBD_FREE(pinger_thread, sizeof(*pinger_thread));
                pinger_thread = NULL;
                RETURN(rc);
        }
        l_wait_event(pinger_thread->t_ctl_waitq,
                     pinger_thread->t_flags & SVC_RUNNING, &lwi);

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
        mutex_down(&pinger_sem);
        pinger_thread->t_flags = SVC_STOPPING;
        cfs_waitq_signal(&pinger_thread->t_ctl_waitq);
        mutex_up(&pinger_sem);

        l_wait_event(pinger_thread->t_ctl_waitq,
                     (pinger_thread->t_flags & SVC_STOPPED), &lwi);

        OBD_FREE(pinger_thread, sizeof(*pinger_thread));
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
        if (!list_empty(&imp->imp_pinger_chain))
                RETURN(-EALREADY);

        mutex_down(&pinger_sem);
        CDEBUG(D_HA, "adding pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        /* if we add to pinger we want recovery on this import */
        imp->imp_obd->obd_no_recov = 0;

        ptlrpc_update_next_ping(imp, 0);
        /* XXX sort, blah blah */
        list_add_tail(&imp->imp_pinger_chain, &pinger_imports);
        class_import_get(imp);

        ptlrpc_pinger_wake_up();
        mutex_up(&pinger_sem);

        RETURN(0);
}

int ptlrpc_pinger_del_import(struct obd_import *imp)
{
        ENTRY;
        if (list_empty(&imp->imp_pinger_chain))
                RETURN(-ENOENT);

        mutex_down(&pinger_sem);
        list_del_init(&imp->imp_pinger_chain);
        CDEBUG(D_HA, "removing pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        /* if we remove from pinger we don't want recovery on this import */
        imp->imp_obd->obd_no_recov = 1;
        class_import_put(imp);
        mutex_up(&pinger_sem);
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

        list_for_each_entry(item, &timeout_list, ti_chain)
                if (item->ti_event == event)
                        goto out;

        item = ptlrpc_new_timeout(time, event, cb, data);
        if (item) {
                list_for_each_entry_reverse(tmp, &timeout_list, ti_chain) {
                        if (tmp->ti_timeout < time) {
                                list_add(&item->ti_chain, &tmp->ti_chain);
                                goto out;
                        }
                }
                list_add(&item->ti_chain, &timeout_list);
        }
out:
        return item;
}

/* Add a client_obd to the timeout event list, when timeout(@time)
 * happens, the callback(@cb) will be called.
 */
int ptlrpc_add_timeout_client(int time, enum timeout_event event,
                              timeout_cb_t cb, void *data,
                              struct list_head *obd_list)
{
        struct timeout_item *ti;

        mutex_down(&pinger_sem);
        ti = ptlrpc_pinger_register_timeout(time, event, cb, data);
        if (!ti) {
                mutex_up(&pinger_sem);
                return (-EINVAL);
        }
        list_add(obd_list, &ti->ti_obd_list);
        mutex_up(&pinger_sem);
        return 0;
}

int ptlrpc_del_timeout_client(struct list_head *obd_list,
                              enum timeout_event event)
{
        struct timeout_item *ti = NULL, *item;

        if (list_empty(obd_list))
                return 0;
        mutex_down(&pinger_sem);
        list_del_init(obd_list);
        /**
         * If there are no obd attached to the timeout event
         * list, remove this timeout event from the pinger
         */
        list_for_each_entry(item, &timeout_list, ti_chain) {
                if (item->ti_event == event) {
                        ti = item;
                        break;
                }
        }
        LASSERTF(ti != NULL, "ti is NULL ! \n");
        if (list_empty(&ti->ti_obd_list)) {
                list_del(&ti->ti_chain);
                OBD_FREE_PTR(ti);
        }
        mutex_up(&pinger_sem);
        return 0;
}

int ptlrpc_pinger_remove_timeouts(void)
{
        struct timeout_item *item, *tmp;

        mutex_down(&pinger_sem);
        list_for_each_entry_safe(item, tmp, &timeout_list, ti_chain) {
                LASSERT(list_empty(&item->ti_obd_list));
                list_del(&item->ti_chain);
                OBD_FREE_PTR(item);
        }
        mutex_up(&pinger_sem);
        return 0;
}

void ptlrpc_pinger_wake_up()
{
#ifdef ENABLE_PINGER
        pinger_thread->t_flags |= SVC_EVENT;
        cfs_waitq_signal(&pinger_thread->t_ctl_waitq);
#endif
}

/* Ping evictor thread */
#define PET_READY     1
#define PET_TERMINATE 2

static int               pet_refcount = 0;
static int               pet_state;
static wait_queue_head_t pet_waitq;
CFS_LIST_HEAD(pet_list);
static spinlock_t        pet_lock = SPIN_LOCK_UNLOCKED;

int ping_evictor_wake(struct obd_export *exp)
{
        struct obd_device *obd;

        spin_lock(&pet_lock);
        if (pet_state != PET_READY) {
                /* eventually the new obd will call here again. */
                spin_unlock(&pet_lock);
                return 1;
        }

        obd = class_exp2obd(exp);
        if (list_empty(&obd->obd_evict_list)) {
                class_incref(obd);
                list_add(&obd->obd_evict_list, &pet_list);
        }
        spin_unlock(&pet_lock);

        wake_up(&pet_waitq);
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
                l_wait_event(pet_waitq, (!list_empty(&pet_list)) ||
                             (pet_state == PET_TERMINATE), &lwi);

                /* loop until all obd's will be removed */
                if ((pet_state == PET_TERMINATE) && list_empty(&pet_list))
                        break;

                /* we only get here if pet_exp != NULL, and the end of this
                 * loop is the only place which sets it NULL again, so lock
                 * is not strictly necessary. */
                spin_lock(&pet_lock);
                obd = list_entry(pet_list.next, struct obd_device,
                                 obd_evict_list);
                spin_unlock(&pet_lock);

                if (obd->obd_recovering) {
                        /* bug 18948: ensure recovery is aborted in a timely fashion */
                        target_recovery_check_and_stop(obd);
                        /* no evictor during recovery */
                        GOTO(skip, 0);
                }

                expire_time = cfs_time_current_sec() - PING_EVICT_TIMEOUT;

                CDEBUG(D_HA, "evicting all exports of obd %s older than %ld\n",
                       obd->obd_name, expire_time);

                /* Exports can't be deleted out of the list while we hold
                 * the obd lock (class_unlink_export), which means we can't
                 * lose the last ref on the export.  If they've already been
                 * removed from the list, we won't find them here. */
                spin_lock(&obd->obd_dev_lock);
                while (!list_empty(&obd->obd_exports_timed)) {
                        exp = list_entry(obd->obd_exports_timed.next,
                                         struct obd_export,exp_obd_chain_timed);
                        if (expire_time > exp->exp_last_request_time) {
                                class_export_get(exp);
                                spin_unlock(&obd->obd_dev_lock);
                                LCONSOLE_WARN("%s: client %s (at %s) evicted, "
                                              "unresponsive for %lds\n",
                                              obd->obd_name,
                                              obd_uuid2str(&exp->exp_client_uuid),
                                              obd_export_nid2str(exp),
                                              (long)(cfs_time_current_sec() -
                                                     exp->exp_last_request_time));
                                CDEBUG(D_HA, "Last request was at %ld\n",
                                       exp->exp_last_request_time);
                                class_fail_export(exp);
                                class_export_put(exp);
                                spin_lock(&obd->obd_dev_lock);
                        } else {
                                /* List is sorted, so everyone below is ok */
                                break;
                        }
                }
                spin_unlock(&obd->obd_dev_lock);
skip:
                spin_lock(&pet_lock);
                list_del_init(&obd->obd_evict_list);
                spin_unlock(&pet_lock);

                class_decref(obd);
        }

        CDEBUG(D_HA, "Exiting Ping Evictor\n");

        RETURN(0);
}

void ping_evictor_start(void)
{
        int rc;

        if (++pet_refcount > 1)
                return;

        init_waitqueue_head(&pet_waitq);

        rc = cfs_kernel_thread(ping_evictor_main, NULL, CLONE_VM | CLONE_FILES);
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
        wake_up(&pet_waitq);
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
        struct list_head *iter;
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
        if (!pd->pd_set && time_before(curtime, pd->pd_next_ping)) {
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
        mutex_down(&pinger_sem);
        list_for_each(iter, &pinger_imports) {
                struct obd_import *imp =
                        list_entry(iter, struct obd_import, imp_pinger_chain);
                int generation, level;

                /* Include any ping within 1/10 of a second of the deadline */
                if (cfs_time_aftereq(pd->pd_this_ping, imp->imp_next_ping -
                                     (cfs_time_seconds(1) + 9) / 10)) {
                        /* Add a ping. */
                        spin_lock(&imp->imp_lock);
                        generation = imp->imp_generation;
                        level = imp->imp_state;
                        spin_unlock(&imp->imp_lock);

                        if (level != LUSTRE_IMP_FULL) {
                                CDEBUG(D_HA,
                                       "not pinging %s (in recovery)\n",
                                       obd2cli_tgt(imp->imp_obd));
                                continue;
                        }

                        req = ptlrpc_prep_req(imp, LUSTRE_OBD_VERSION, OBD_PING,
                                              1, NULL, NULL);
                        if (!req) {
                                CERROR("out of memory\n");
                                break;
                        }
                        req->rq_no_resend = 1;
                        ptlrpc_req_set_repsize(req, 1, NULL);
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
        mutex_up(&pinger_sem);

        /* Might be empty, that's OK. */
        if (atomic_read(&set->set_remaining) == 0)
                CDEBUG(D_RPCTRACE, "nothing to ping\n");

        list_for_each(iter, &set->set_requests) {
                struct ptlrpc_request *req =
                        list_entry(iter, struct ptlrpc_request,
                                   rq_set_chain);
                DEBUG_REQ(D_RPCTRACE, req, "pinging %s->%s",
                          req->rq_import->imp_obd->obd_uuid.uuid,
                          obd2cli_tgt(req->rq_import->imp_obd));
                (void)ptl_send_rpc(req, 0);
        }

do_check_set:
        rc = ptlrpc_check_set(set);

        /* not finished, and we are not expired, simply return */
        if (!rc && cfs_time_before(curtime, cfs_time_add(pd->pd_this_ping,
                                            cfs_time_seconds(PING_INTERVAL)))) {
                CDEBUG(D_RPCTRACE, "not finished, but also not expired\n");
                pd->pd_recursion--;
                return 0;
        }

        /* Expire all the requests that didn't come back. */
        mutex_down(&pinger_sem);
        list_for_each(iter, &set->set_requests) {
                req = list_entry(iter, struct ptlrpc_request,
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
                spin_lock(&imp->imp_lock);
                if (!list_empty(&req->rq_list)) {
                        list_del_init(&req->rq_list);
                        atomic_dec(&imp->imp_inflight);
                }
                spin_unlock(&imp->imp_lock);
                atomic_dec(&set->set_remaining);
        }
        mutex_up(&pinger_sem);

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
        mutex_down(&pinger_sem);
        ptlrpc_update_next_ping(imp, 0);
        if (pinger_args.pd_set == NULL &&
            time_before(imp->imp_next_ping, pinger_args.pd_next_ping)) {
                CDEBUG(D_HA, "set next ping to "CFS_TIME_T"(cur "CFS_TIME_T")\n",
                        imp->imp_next_ping, cfs_time_current());
                pinger_args.pd_next_ping = imp->imp_next_ping;
        }
        mutex_up(&pinger_sem);
#endif
}

void ptlrpc_pinger_commit_expected(struct obd_import *imp)
{
#ifdef ENABLE_PINGER
        mutex_down(&pinger_sem);
        ptlrpc_update_next_ping(imp, 1);
        if (pinger_args.pd_set == NULL &&
            time_before(imp->imp_next_ping, pinger_args.pd_next_ping)) {
                CDEBUG(D_HA, "set next ping to "CFS_TIME_T"(cur "CFS_TIME_T")\n",
                        imp->imp_next_ping, cfs_time_current());
                pinger_args.pd_next_ping = imp->imp_next_ping;
        }
        mutex_up(&pinger_sem);
#endif
}

int ptlrpc_add_timeout_client(int time, enum timeout_event event,
                              timeout_cb_t cb, void *data,
                              struct list_head *obd_list)
{
        return 0;
}

int ptlrpc_del_timeout_client(struct list_head *obd_list,
                              enum timeout_event event)
{
        return 0;
}

int ptlrpc_pinger_add_import(struct obd_import *imp)
{
        ENTRY;
        if (!list_empty(&imp->imp_pinger_chain))
                RETURN(-EALREADY);

        CDEBUG(D_HA, "adding pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        ptlrpc_pinger_sending_on_import(imp);

        mutex_down(&pinger_sem);
        list_add_tail(&imp->imp_pinger_chain, &pinger_imports);
        class_import_get(imp);
        mutex_up(&pinger_sem);

        RETURN(0);
}

int ptlrpc_pinger_del_import(struct obd_import *imp)
{
        ENTRY;
        if (list_empty(&imp->imp_pinger_chain))
                RETURN(-ENOENT);

        mutex_down(&pinger_sem);
        list_del_init(&imp->imp_pinger_chain);
        CDEBUG(D_HA, "removing pingable import %s->%s\n",
               imp->imp_obd->obd_uuid.uuid, obd2cli_tgt(imp->imp_obd));
        class_import_put(imp);
        mutex_up(&pinger_sem);
        RETURN(0);
}

void ptlrpc_pinger_wake_up()
{
#ifdef ENABLE_PINGER
        ENTRY;
        /* XXX force pinger to run, if needed */
        struct obd_import *imp;
        list_for_each_entry(imp, &pinger_imports, imp_pinger_chain) {
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
#endif
        EXIT;
}
#endif /* !__KERNEL__ */
