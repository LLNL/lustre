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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ptlrpc/sec_gc.c
 *
 * Author: Eric Mei <ericm@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_SEC

#ifndef __KERNEL__
#include <liblustre.h>
#else
#include <libcfs/libcfs.h>
#endif

#include <obd_support.h>
#include <obd_class.h>
#include <lustre_net.h>
#include <lustre_sec.h>

#define SEC_GC_INTERVAL (30 * 60)

#ifdef __KERNEL__

static cfs_mutex_t sec_gc_mutex;
static CFS_LIST_HEAD(sec_gc_list);
static cfs_spinlock_t sec_gc_list_lock;

static CFS_LIST_HEAD(sec_gc_ctx_list);
static cfs_spinlock_t sec_gc_ctx_list_lock;

static struct ptlrpc_thread sec_gc_thread;
static cfs_atomic_t sec_gc_wait_del = CFS_ATOMIC_INIT(0);


void sptlrpc_gc_add_sec(struct ptlrpc_sec *sec)
{
        LASSERT(sec->ps_policy->sp_cops->gc_ctx);
        LASSERT(sec->ps_gc_interval > 0);
        LASSERT(cfs_list_empty(&sec->ps_gc_list));

        sec->ps_gc_next = cfs_time_current_sec() + sec->ps_gc_interval;

        cfs_spin_lock(&sec_gc_list_lock);
        cfs_list_add_tail(&sec_gc_list, &sec->ps_gc_list);
        cfs_spin_unlock(&sec_gc_list_lock);

        CDEBUG(D_SEC, "added sec %p(%s)\n", sec, sec->ps_policy->sp_name);
}
EXPORT_SYMBOL(sptlrpc_gc_add_sec);

void sptlrpc_gc_del_sec(struct ptlrpc_sec *sec)
{
        if (cfs_list_empty(&sec->ps_gc_list))
                return;

        cfs_might_sleep();

        /* signal before list_del to make iteration in gc thread safe */
        cfs_atomic_inc(&sec_gc_wait_del);

        cfs_spin_lock(&sec_gc_list_lock);
        cfs_list_del_init(&sec->ps_gc_list);
        cfs_spin_unlock(&sec_gc_list_lock);

        /* barrier */
        cfs_mutex_lock(&sec_gc_mutex);
        cfs_mutex_unlock(&sec_gc_mutex);

        cfs_atomic_dec(&sec_gc_wait_del);

        CDEBUG(D_SEC, "del sec %p(%s)\n", sec, sec->ps_policy->sp_name);
}
EXPORT_SYMBOL(sptlrpc_gc_del_sec);

void sptlrpc_gc_add_ctx(struct ptlrpc_cli_ctx *ctx)
{
        LASSERT(cfs_list_empty(&ctx->cc_gc_chain));

        CDEBUG(D_SEC, "hand over ctx %p(%u->%s)\n",
               ctx, ctx->cc_vcred.vc_uid, sec2target_str(ctx->cc_sec));
        cfs_spin_lock(&sec_gc_ctx_list_lock);
        cfs_list_add(&ctx->cc_gc_chain, &sec_gc_ctx_list);
        cfs_spin_unlock(&sec_gc_ctx_list_lock);

        sec_gc_thread.t_flags |= SVC_SIGNAL;
        cfs_waitq_signal(&sec_gc_thread.t_ctl_waitq);
}
EXPORT_SYMBOL(sptlrpc_gc_add_ctx);

static void sec_process_ctx_list(void)
{
        struct ptlrpc_cli_ctx *ctx;

        cfs_spin_lock(&sec_gc_ctx_list_lock);

        while (!cfs_list_empty(&sec_gc_ctx_list)) {
                ctx = cfs_list_entry(sec_gc_ctx_list.next,
                                     struct ptlrpc_cli_ctx, cc_gc_chain);
                cfs_list_del_init(&ctx->cc_gc_chain);
                cfs_spin_unlock(&sec_gc_ctx_list_lock);

                LASSERT(ctx->cc_sec);
                LASSERT(cfs_atomic_read(&ctx->cc_refcount) == 1);
                CDEBUG(D_SEC, "gc pick up ctx %p(%u->%s)\n",
                       ctx, ctx->cc_vcred.vc_uid, sec2target_str(ctx->cc_sec));
                sptlrpc_cli_ctx_put(ctx, 1);

                cfs_spin_lock(&sec_gc_ctx_list_lock);
        }

        cfs_spin_unlock(&sec_gc_ctx_list_lock);
}

static void sec_do_gc(struct ptlrpc_sec *sec)
{
        LASSERT(sec->ps_policy->sp_cops->gc_ctx);

        if (unlikely(sec->ps_gc_next == 0)) {
                CDEBUG(D_SEC, "sec %p(%s) has 0 gc time\n",
                      sec, sec->ps_policy->sp_name);
                return;
        }

        CDEBUG(D_SEC, "check on sec %p(%s)\n", sec, sec->ps_policy->sp_name);

        if (cfs_time_after(sec->ps_gc_next, cfs_time_current_sec()))
                return;

        sec->ps_policy->sp_cops->gc_ctx(sec);
        sec->ps_gc_next = cfs_time_current_sec() + sec->ps_gc_interval;
}

static int sec_gc_main(void *arg)
{
        struct ptlrpc_thread *thread = (struct ptlrpc_thread *) arg;
        struct l_wait_info    lwi;

        cfs_daemonize_ctxt("sptlrpc_gc");

        /* Record that the thread is running */
        thread->t_flags = SVC_RUNNING;
        cfs_waitq_signal(&thread->t_ctl_waitq);

        while (1) {
                struct ptlrpc_sec *sec;

                thread->t_flags &= ~SVC_SIGNAL;
                sec_process_ctx_list();
again:
                /* go through sec list do gc.
                 * FIXME here we iterate through the whole list each time which
                 * is not optimal. we perhaps want to use balanced binary tree
                 * to trace each sec as order of expiry time.
                 * another issue here is we wakeup as fixed interval instead of
                 * according to each sec's expiry time */
                cfs_mutex_lock(&sec_gc_mutex);
                cfs_list_for_each_entry(sec, &sec_gc_list, ps_gc_list) {
                        /* if someone is waiting to be deleted, let it
                         * proceed as soon as possible. */
                        if (cfs_atomic_read(&sec_gc_wait_del)) {
                                CDEBUG(D_SEC, "deletion pending, start over\n");
                                cfs_mutex_unlock(&sec_gc_mutex);
                                goto again;
                        }

                        sec_do_gc(sec);
                }
                cfs_mutex_unlock(&sec_gc_mutex);

                /* check ctx list again before sleep */
                sec_process_ctx_list();

                lwi = LWI_TIMEOUT(SEC_GC_INTERVAL * CFS_HZ, NULL, NULL);
                l_wait_event(thread->t_ctl_waitq,
                             thread->t_flags & (SVC_STOPPING | SVC_SIGNAL),
                             &lwi);

                if (thread->t_flags & SVC_STOPPING) {
                        thread->t_flags &= ~SVC_STOPPING;
                        break;
                }
        }

        thread->t_flags = SVC_STOPPED;
        cfs_waitq_signal(&thread->t_ctl_waitq);
        return 0;
}

int sptlrpc_gc_init(void)
{
        struct l_wait_info lwi = { 0 };
        int                rc;

        cfs_mutex_init(&sec_gc_mutex);
        cfs_spin_lock_init(&sec_gc_list_lock);
        cfs_spin_lock_init(&sec_gc_ctx_list_lock);

        /* initialize thread control */
        memset(&sec_gc_thread, 0, sizeof(sec_gc_thread));
        cfs_waitq_init(&sec_gc_thread.t_ctl_waitq);

        rc = cfs_create_thread(sec_gc_main, &sec_gc_thread, CFS_DAEMON_FLAGS);
        if (rc < 0) {
                CERROR("can't start gc thread: %d\n", rc);
                return rc;
        }

        l_wait_event(sec_gc_thread.t_ctl_waitq,
                     sec_gc_thread.t_flags & SVC_RUNNING, &lwi);
        return 0;
}

void sptlrpc_gc_fini(void)
{
        struct l_wait_info lwi = { 0 };

        sec_gc_thread.t_flags = SVC_STOPPING;
        cfs_waitq_signal(&sec_gc_thread.t_ctl_waitq);

        l_wait_event(sec_gc_thread.t_ctl_waitq,
                     sec_gc_thread.t_flags & SVC_STOPPED, &lwi);
}

#else /* !__KERNEL__ */

void sptlrpc_gc_add_sec(struct ptlrpc_sec *sec)
{
}
void sptlrpc_gc_del_sec(struct ptlrpc_sec *sec)
{
}
int sptlrpc_gc_init(void)
{
        return 0;
}
void sptlrpc_gc_fini(void)
{
}

#endif /* __KERNEL__ */
