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
 */

#define DEBUG_SUBSYSTEM S_RPC

#ifndef __KERNEL__
# include <liblustre.h>
#else
# ifdef __mips64__
#  include <linux/kernel.h>
# endif
#endif
#include <obd_class.h>
#include <lustre_net.h>
#include "ptlrpc_internal.h"

lnet_handle_eq_t   ptlrpc_eq_h;

/*  
 *  Client's outgoing request callback
 */
void request_out_callback(lnet_event_t *ev)
{
        struct ptlrpc_cb_id   *cbid = ev->md.user_ptr;
        struct ptlrpc_request *req = cbid->cbid_arg;
        ENTRY;

        LASSERT (ev->type == LNET_EVENT_SEND ||
                 ev->type == LNET_EVENT_UNLINK);
        LASSERT (ev->unlinked);

        DEBUG_REQ(D_NET, req, "type %d, status %d", ev->type, ev->status);

        if (ev->type == LNET_EVENT_UNLINK || ev->status != 0) {

                /* Failed send: make it seem like the reply timed out, just
                 * like failing sends in client.c does currently...  */

                spin_lock(&req->rq_lock);
                req->rq_net_err = 1;
                spin_unlock(&req->rq_lock);

                ptlrpc_client_wake_req(req);
        }

        ptlrpc_req_finished(req);

        EXIT;
}

/*
 * Client's incoming reply callback
 */
void reply_in_callback(lnet_event_t *ev)
{
        struct ptlrpc_cb_id   *cbid = ev->md.user_ptr;
        struct ptlrpc_request *req = cbid->cbid_arg;
        ENTRY;

        DEBUG_REQ(D_NET, req, "type %d, status %d", ev->type, ev->status);

        LASSERT(ev->type == LNET_EVENT_PUT || ev->type == LNET_EVENT_UNLINK);
        LASSERT(ev->md.start == req->rq_repbuf);
        LASSERT(ev->offset + ev->mlength <= req->rq_replen);
        /* We've set LNET_MD_MANAGE_REMOTE for all outgoing requests
           for adaptive timeouts' early reply. */
        LASSERT((ev->md.options & LNET_MD_MANAGE_REMOTE) != 0);

        spin_lock(&req->rq_lock);

        req->rq_receiving_reply = 0;
        req->rq_early = 0;
        if (ev->unlinked)
                req->rq_must_unlink = 0;

        if (ev->status)
                goto out_wake;

        if (ev->type == LNET_EVENT_UNLINK) {
                LASSERT(ev->unlinked);
                DEBUG_REQ(D_RPCTRACE, req, "unlink");
                goto out_wake;
        }

        if (ev->mlength < ev->rlength ) {
                CDEBUG(D_RPCTRACE, "truncate req %p rpc %d - %d+%d\n", req,
                       req->rq_replen, ev->rlength, ev->offset);
                req->rq_reply_truncate = 1;
                req->rq_replied = 1;
                req->rq_status = -EOVERFLOW;
                req->rq_nob_received = ev->rlength + ev->offset;
                goto out_wake;
        }

        if ((ev->offset == 0) &&
            (lustre_msghdr_get_flags(req->rq_reqmsg) & MSGHDR_AT_SUPPORT)) {
                /* Early reply */
                DEBUG_REQ(D_ADAPTTO, req,
                          "Early reply received: mlen=%u offset=%d replen=%d "
                          "replied=%d unlinked=%d", ev->mlength, ev->offset,
                          req->rq_replen, req->rq_replied, ev->unlinked);

                if (unlikely(ev->mlength != lustre_msg_early_size(req)))
                        CERROR("early reply sized %u, expect %u\n",
                               ev->mlength, lustre_msg_early_size(req));

                req->rq_early_count++; /* number received, client side */

                if (req->rq_replied)   /* already got the real reply */
                        goto out_wake;

                req->rq_early = 1;
                req->rq_nob_received = ev->mlength;
                /* repmsg points to early reply */
                req->rq_repmsg = req->rq_repbuf;
                /* And we're still receiving */
                req->rq_receiving_reply = 1;
        } else {
                /* Real reply */
                req->rq_rep_swab_mask = 0;
                req->rq_replied = 1;
                req->rq_nob_received = ev->mlength;
                /* repmsg points to real reply */
                req->rq_repmsg = (struct lustre_msg *)((char *)req->rq_repbuf +
                                                       ev->offset);
                /* LNetMDUnlink can't be called under the LNET_LOCK,
                   so we must unlink in ptlrpc_unregister_reply */
                DEBUG_REQ(D_INFO, req, 
                          "reply in flags=%x mlen=%u offset=%d replen=%d",
                          lustre_msg_get_flags(req->rq_reqmsg),
                          ev->mlength, ev->offset, req->rq_replen);
        }

        req->rq_import->imp_last_reply_time = cfs_time_current_sec();

out_wake:
        /* NB don't unlock till after wakeup; req can disappear under us
         * since we don't have our own ref */
        ptlrpc_client_wake_req(req);
        spin_unlock(&req->rq_lock);
        EXIT;
}

/* 
 * Client's bulk has been written/read
 */
void client_bulk_callback (lnet_event_t *ev)
{
        struct ptlrpc_cb_id     *cbid = ev->md.user_ptr;
        struct ptlrpc_bulk_desc *desc = cbid->cbid_arg;
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_CLIENT_BULK_CB))
                ev->status = -EIO;

        LASSERT ((desc->bd_type == BULK_PUT_SINK && 
                  ev->type == LNET_EVENT_PUT) ||
                 (desc->bd_type == BULK_GET_SOURCE &&
                  ev->type == LNET_EVENT_GET) ||
                 ev->type == LNET_EVENT_UNLINK);
        LASSERT (ev->unlinked);

        CDEBUG((ev->status == 0) ? D_NET : D_ERROR,
               "event type %d, status %d, desc %p\n", 
               ev->type, ev->status, desc);

        spin_lock(&desc->bd_lock);

        LASSERT(desc->bd_network_rw);
        desc->bd_network_rw = 0;

        if (ev->type != LNET_EVENT_UNLINK && ev->status == 0) {
                desc->bd_success = 1;
                desc->bd_nob_transferred = ev->mlength;
                desc->bd_sender = ev->sender;
        }

        /* NB don't unlock till after wakeup; desc can disappear under us
         * otherwise */
        ptlrpc_client_wake_req(desc->bd_req);

        spin_unlock(&desc->bd_lock);
        EXIT;
}

/* 
 * Server's incoming request callback
 */
void request_in_callback(lnet_event_t *ev)
{
        struct ptlrpc_cb_id               *cbid = ev->md.user_ptr;
        struct ptlrpc_request_buffer_desc *rqbd = cbid->cbid_arg;
        struct ptlrpc_service             *service = rqbd->rqbd_service;
        struct ptlrpc_request             *req;
        ENTRY;

        LASSERT (ev->type == LNET_EVENT_PUT ||
                 ev->type == LNET_EVENT_UNLINK);
        LASSERT ((char *)ev->md.start >= rqbd->rqbd_buffer);
        LASSERT ((char *)ev->md.start + ev->offset + ev->mlength <=
                 rqbd->rqbd_buffer + service->srv_buf_size);

        CDEBUG((ev->status == 0) ? D_NET : D_ERROR,
               "event type %d, status %d, service %s\n", 
               ev->type, ev->status, service->srv_name);

        if (ev->unlinked) {
                /* If this is the last request message to fit in the
                 * request buffer we can use the request object embedded in
                 * rqbd.  Note that if we failed to allocate a request,
                 * we'd have to re-post the rqbd, which we can't do in this
                 * context. */
                req = &rqbd->rqbd_req;
                memset(req, 0, sizeof (*req));
        } else {
                LASSERT (ev->type == LNET_EVENT_PUT);
                if (ev->status != 0) {
                        /* We moaned above already... */
                        return;
                }
                OBD_ALLOC_GFP(req, sizeof(*req), CFS_ALLOC_ATOMIC_TRY);
                if (req == NULL) {
                        CERROR("Can't allocate incoming request descriptor: "
                               "Dropping %s RPC from %s\n",
                               service->srv_name, 
                               libcfs_id2str(ev->initiator));
                        return;
                }
        }

        /* NB we ABSOLUTELY RELY on req being zeroed, so pointers are NULL,
         * flags are reset and scalars are zero.  We only set the message
         * size to non-zero if this was a successful receive. */
        req->rq_xid = ev->match_bits;
        req->rq_reqmsg = ev->md.start + ev->offset;
        if (ev->type == LNET_EVENT_PUT && ev->status == 0)
                req->rq_reqlen = ev->mlength;
        do_gettimeofday(&req->rq_arrival_time);
        req->rq_peer = ev->initiator;
        req->rq_self = ev->target.nid;
        req->rq_rqbd = rqbd;
        req->rq_phase = RQ_PHASE_NEW;
#ifdef CRAY_XT3
        req->rq_uid = ev->uid;
#endif
        spin_lock_init(&req->rq_lock);
        CFS_INIT_LIST_HEAD(&req->rq_list);
        CFS_INIT_LIST_HEAD(&req->rq_timed_list);
        CFS_INIT_LIST_HEAD(&req->rq_replay_list);
        CFS_INIT_LIST_HEAD(&req->rq_set_chain);
        CFS_INIT_LIST_HEAD(&req->rq_history_list);
        CFS_INIT_LIST_HEAD(&req->rq_exp_list);
        atomic_set(&req->rq_refcount, 1);
        if (ev->type == LNET_EVENT_PUT)
                DEBUG_REQ(D_RPCTRACE, req, "incoming req");

        spin_lock(&service->srv_lock);

        req->rq_history_seq = service->srv_request_seq++;
        list_add_tail(&req->rq_history_list, &service->srv_request_history);

        if (ev->unlinked) {
                service->srv_nrqbd_receiving--;
                CDEBUG(D_INFO, "Buffer complete: %d buffers still posted\n",
                       service->srv_nrqbd_receiving);

                /* Normally, don't complain about 0 buffers posted; LNET won't
                 * drop incoming reqs since we set the portal lazy */
                if (test_req_buffer_pressure &&
                    ev->type != LNET_EVENT_UNLINK &&
                    service->srv_nrqbd_receiving == 0)
                        CWARN("All %s request buffers busy\n",
                              service->srv_name);

                /* req takes over the network's ref on rqbd */
        } else {
                /* req takes a ref on rqbd */
                rqbd->rqbd_refcount++;
        }

        list_add_tail(&req->rq_list, &service->srv_req_in_queue);
        service->srv_n_queued_reqs++;

        /* NB everything can disappear under us once the request
         * has been queued and we unlock, so do the wake now... */
        cfs_waitq_signal(&service->srv_waitq);

        spin_unlock(&service->srv_lock);
        EXIT;
}

/*
 *  Server's outgoing reply callback
 */
void reply_out_callback(lnet_event_t *ev)
{
        struct ptlrpc_cb_id       *cbid = ev->md.user_ptr;
        struct ptlrpc_reply_state *rs = cbid->cbid_arg;
        struct ptlrpc_service     *svc = rs->rs_service;
        ENTRY;

        LASSERT (ev->type == LNET_EVENT_SEND ||
                 ev->type == LNET_EVENT_ACK ||
                 ev->type == LNET_EVENT_UNLINK);

        if (!rs->rs_difficult) {
                /* 'Easy' replies have no further processing so I drop the
                 * net's ref on 'rs' */
                LASSERT (ev->unlinked);
                ptlrpc_rs_decref(rs);
                atomic_dec (&svc->srv_outstanding_replies);
                EXIT;
                return;
        }

        LASSERT (rs->rs_on_net);

        if (ev->unlinked) {
                /* Last network callback.  The net's ref on 'rs' stays put
                 * until ptlrpc_server_handle_reply() is done with it */
                spin_lock(&svc->srv_lock);
                rs->rs_on_net = 0;
                ptlrpc_schedule_difficult_reply (rs);
                spin_unlock(&svc->srv_lock);
        }

        EXIT;
}

/*
 * Server's bulk completion callback
 */
void server_bulk_callback (lnet_event_t *ev)
{
        struct ptlrpc_cb_id     *cbid = ev->md.user_ptr;
        struct ptlrpc_bulk_desc *desc = cbid->cbid_arg;
        ENTRY;

        LASSERT (ev->type == LNET_EVENT_SEND ||
                 ev->type == LNET_EVENT_UNLINK ||
                 (desc->bd_type == BULK_PUT_SOURCE &&
                  ev->type == LNET_EVENT_ACK) ||
                 (desc->bd_type == BULK_GET_SINK &&
                  ev->type == LNET_EVENT_REPLY));

        CDEBUG((ev->status == 0) ? D_NET : D_ERROR,
               "event type %d, status %d, desc %p\n", 
               ev->type, ev->status, desc);

        spin_lock(&desc->bd_lock);
        
        if ((ev->type == LNET_EVENT_ACK ||
             ev->type == LNET_EVENT_REPLY) &&
            ev->status == 0) {
                /* We heard back from the peer, so even if we get this
                 * before the SENT event (oh yes we can), we know we
                 * read/wrote the peer buffer and how much... */
                desc->bd_success = 1;
                desc->bd_nob_transferred = ev->mlength;
                desc->bd_sender = ev->sender;
        }

        if (ev->unlinked) {
                /* This is the last callback no matter what... */
                desc->bd_network_rw = 0;
                cfs_waitq_signal(&desc->bd_waitq);
        }

        spin_unlock(&desc->bd_lock);
        EXIT;
}

static void ptlrpc_master_callback(lnet_event_t *ev)
{
        struct ptlrpc_cb_id *cbid = ev->md.user_ptr;
        void (*callback)(lnet_event_t *ev) = cbid->cbid_fn;

        /* Honestly, it's best to find out early. */
        LASSERT (cbid->cbid_arg != LP_POISON);
        LASSERT (callback == request_out_callback ||
                 callback == reply_in_callback ||
                 callback == client_bulk_callback ||
                 callback == request_in_callback ||
                 callback == reply_out_callback ||
                 callback == server_bulk_callback);
        
        callback (ev);
}

int ptlrpc_uuid_to_peer (struct obd_uuid *uuid, 
                         lnet_process_id_t *peer, lnet_nid_t *self)
{
        int               best_dist = 0;
        __u32             best_order = 0;
        int               count = 0;
        int               rc = -ENOENT;
        int               portals_compatibility;
        int               dist;
        __u32             order;
        lnet_nid_t        dst_nid;
        lnet_nid_t        src_nid;

        portals_compatibility = LNetCtl(IOC_LIBCFS_PORTALS_COMPATIBILITY, NULL);

        peer->pid = LUSTRE_SRV_LNET_PID;

        /* Choose the matching UUID that's closest */
        while (lustre_uuid_to_peer(uuid->uuid, &dst_nid, count++) == 0) {
                dist = LNetDist(dst_nid, &src_nid, &order);
                if (dist < 0)
                        continue;

                if (dist == 0) {                /* local! use loopback LND */
                        peer->nid = *self = LNET_MKNID(LNET_MKNET(LOLND, 0), 0);
                        rc = 0;
                        break;
                }
                
                if (rc < 0 ||
                    dist < best_dist ||
                    (dist == best_dist && order < best_order)) {
                        best_dist = dist;
                        best_order = order;

                        if (portals_compatibility > 1) {
                                /* Strong portals compatibility: Zero the nid's
                                 * NET, so if I'm reading new config logs, or
                                 * getting configured by (new) lconf I can
                                 * still talk to old servers. */
                                dst_nid = LNET_MKNID(0, LNET_NIDADDR(dst_nid));
                                src_nid = LNET_MKNID(0, LNET_NIDADDR(src_nid));
                        }
                        peer->nid = dst_nid;
                        *self = src_nid;
                        rc = 0;
                }
        }

        CDEBUG(D_NET,"%s->%s\n", uuid->uuid, libcfs_id2str(*peer));
        if (rc != 0) 
                CERROR("No NID found for %s\n", uuid->uuid);
        return rc;
}

void ptlrpc_ni_fini(void)
{
        cfs_waitq_t         waitq;
        struct l_wait_info  lwi;
        int                 rc;
        int                 retries;
        
        /* Wait for the event queue to become idle since there may still be
         * messages in flight with pending events (i.e. the fire-and-forget
         * messages == client requests and "non-difficult" server
         * replies */

        for (retries = 0;; retries++) {
                rc = LNetEQFree(ptlrpc_eq_h);
                switch (rc) {
                default:
                        LBUG();

                case 0:
                        LNetNIFini();
                        return;
                        
                case -EBUSY:
                        if (retries != 0)
                                CWARN("Event queue still busy\n");
                        
                        /* Wait for a bit */
                        cfs_waitq_init(&waitq);
                        lwi = LWI_TIMEOUT(cfs_time_seconds(2), NULL, NULL);
                        l_wait_event(waitq, 0, &lwi);
                        break;
                }
        }
        /* notreached */
}

lnet_pid_t ptl_get_pid(void)
{
        lnet_pid_t        pid;

#ifndef  __KERNEL__
        pid = getpid();
#else
        pid = LUSTRE_SRV_LNET_PID;
#endif
        return pid;
}
        
int ptlrpc_ni_init(void)
{
        int              rc;
        lnet_pid_t       pid;

        pid = ptl_get_pid();
        CDEBUG(D_NET, "My pid is: %x\n", pid);

        /* We're not passing any limits yet... */
        rc = LNetNIInit(pid);
        if (rc < 0) {
                CDEBUG (D_NET, "Can't init network interface: %d\n", rc);
                return (-ENOENT);
        }

        /* CAVEAT EMPTOR: how we process portals events is _radically_
         * different depending on... */
#ifdef __KERNEL__
        /* kernel portals calls our master callback when events are added to
         * the event queue.  In fact lustre never pulls events off this queue,
         * so it's only sized for some debug history. */
        rc = LNetEQAlloc(1024, ptlrpc_master_callback, &ptlrpc_eq_h);
#else
        /* liblustre calls the master callback when it removes events from the
         * event queue.  The event queue has to be big enough not to drop
         * anything */
        rc = LNetEQAlloc(10240, LNET_EQ_HANDLER_NONE, &ptlrpc_eq_h);
#endif
        if (rc == 0)
                return 0;

        CERROR ("Failed to allocate event queue: %d\n", rc);
        LNetNIFini();

        return (-ENOMEM);
}

#ifndef __KERNEL__
CFS_LIST_HEAD(liblustre_wait_callbacks);
CFS_LIST_HEAD(liblustre_idle_callbacks);
void *liblustre_services_callback;

void *
liblustre_register_waitidle_callback (struct list_head *callback_list,
                                      const char *name,
                                      int (*fn)(void *arg), void *arg)
{
        struct liblustre_wait_callback *llwc;
        
        OBD_ALLOC(llwc, sizeof(*llwc));
        LASSERT (llwc != NULL);
        
        llwc->llwc_name = name;
        llwc->llwc_fn = fn;
        llwc->llwc_arg = arg;
        list_add_tail(&llwc->llwc_list, callback_list);
        
        return (llwc);
}

void
liblustre_deregister_waitidle_callback (void *opaque)
{
        struct liblustre_wait_callback *llwc = opaque;
        
        list_del(&llwc->llwc_list);
        OBD_FREE(llwc, sizeof(*llwc));
}

void *
liblustre_register_wait_callback (const char *name,
                                  int (*fn)(void *arg), void *arg)
{
        return liblustre_register_waitidle_callback(&liblustre_wait_callbacks,
                                                    name, fn, arg);
}

void
liblustre_deregister_wait_callback (void *opaque)
{
        liblustre_deregister_waitidle_callback(opaque);
}

void *
liblustre_register_idle_callback (const char *name,
                                  int (*fn)(void *arg), void *arg)
{
        return liblustre_register_waitidle_callback(&liblustre_idle_callbacks,
                                                    name, fn, arg);
}

void
liblustre_deregister_idle_callback (void *opaque)
{
        liblustre_deregister_waitidle_callback(opaque);
}

int
liblustre_check_events (int timeout)
{
        lnet_event_t ev;
        int         rc;
        int         i;
        ENTRY;

        rc = LNetEQPoll(&ptlrpc_eq_h, 1, timeout * 1000, &ev, &i);
        if (rc == 0)
                RETURN(0);
        
        LASSERT (rc == -EOVERFLOW || rc == 1);
        
        /* liblustre: no asynch callback so we can't affort to miss any
         * events... */
        if (rc == -EOVERFLOW) {
                CERROR ("Dropped an event!!!\n");
                abort();
        }
        
        ptlrpc_master_callback (&ev);
        RETURN(1);
}

int liblustre_waiting = 0;

int
liblustre_wait_event (int timeout)
{
        struct list_head               *tmp;
        struct liblustre_wait_callback *llwc;
        int                             found_something = 0;

        /* single threaded recursion check... */
        liblustre_waiting = 1;

        for (;;) {
                /* Deal with all pending events */
                while (liblustre_check_events(0))
                        found_something = 1;

                /* Give all registered callbacks a bite at the cherry */
                list_for_each(tmp, &liblustre_wait_callbacks) {
                        llwc = list_entry(tmp, struct liblustre_wait_callback, 
                                          llwc_list);
                
                        if (llwc->llwc_fn(llwc->llwc_arg))
                                found_something = 1;
                }

                if (found_something || timeout == 0)
                        break;

                /* Nothing so far, but I'm allowed to block... */
                found_something = liblustre_check_events(timeout);
                if (!found_something)           /* still nothing */
                        break;                  /* I timed out */
        }

        liblustre_waiting = 0;

        return found_something;
}

void
liblustre_wait_idle(void)
{
        static int recursed = 0;
        
        struct list_head               *tmp;
        struct liblustre_wait_callback *llwc;
        int                             idle = 0;

        LASSERT(!recursed);
        recursed = 1;
        
        do {
                liblustre_wait_event(0);

                idle = 1;

                list_for_each(tmp, &liblustre_idle_callbacks) {
                        llwc = list_entry(tmp, struct liblustre_wait_callback,
                                          llwc_list);
                        
                        if (!llwc->llwc_fn(llwc->llwc_arg)) {
                                idle = 0;
                                break;
                        }
                }
                        
        } while (!idle);

        recursed = 0;
}

#endif /* __KERNEL__ */

int ptlrpc_init_portals(void)
{
        int   rc = ptlrpc_ni_init();

        if (rc != 0) {
                CERROR("network initialisation failed\n");
                return -EIO;
        }
#ifndef __KERNEL__
        liblustre_services_callback = 
                liblustre_register_wait_callback("liblustre_check_services",
                                                 &liblustre_check_services, NULL);
#endif
        rc = ptlrpcd_addref();
        if (rc == 0)
                return 0;
        
        CERROR("rpcd initialisation failed\n");
#ifndef __KERNEL__
        liblustre_deregister_wait_callback(liblustre_services_callback);
#endif
        ptlrpc_ni_fini();
        return rc;
}

void ptlrpc_exit_portals(void)
{
#ifndef __KERNEL__
        liblustre_deregister_wait_callback(liblustre_services_callback);
#endif
        ptlrpcd_decref();
        ptlrpc_ni_fini();
}
