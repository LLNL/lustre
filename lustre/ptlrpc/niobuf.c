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
#include <liblustre.h>
#endif
#include <obd_support.h>
#include <lustre_net.h>
#include <lustre_lib.h>
#include <obd.h>
#include "ptlrpc_internal.h"

static int ptl_send_buf (lnet_handle_md_t *mdh, void *base, int len,
                         lnet_ack_req_t ack, struct ptlrpc_cb_id *cbid,
                         struct ptlrpc_connection *conn, int portal, __u64 xid,
                         unsigned int offset)
{
        int              rc;
        lnet_md_t         md;
        ENTRY;

        LASSERT (portal != 0);
        LASSERT (conn != NULL);
        CDEBUG (D_INFO, "conn=%p id %s\n", conn, libcfs_id2str(conn->c_peer));
        md.start     = base;
        md.length    = len;
        md.threshold = (ack == LNET_ACK_REQ) ? 2 : 1;
        md.options   = PTLRPC_MD_OPTIONS;
        md.user_ptr  = cbid;
        md.eq_handle = ptlrpc_eq_h;

        if (ack == LNET_ACK_REQ &&
            OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_ACK | OBD_FAIL_ONCE)) {
                /* don't ask for the ack to simulate failing client */
                ack = LNET_NOACK_REQ;
                obd_fail_loc |= OBD_FAIL_ONCE | OBD_FAILED;
        }

        rc = LNetMDBind (md, LNET_UNLINK, mdh);
        if (rc != 0) {
                CERROR ("LNetMDBind failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                RETURN (-ENOMEM);
        }

        CDEBUG(D_NET, "Sending %d bytes to portal %d, xid "LPD64", offset %u\n",
               len, portal, xid, offset);

        rc = LNetPut (conn->c_self, *mdh, ack,
                      conn->c_peer, portal, xid, offset, 0);
        if (rc != 0) {
                int rc2;
                /* We're going to get an UNLINK event when I unlink below,
                 * which will complete just like any other failed send, so
                 * I fall through and return success here! */
                CERROR("LNetPut(%s, %d, "LPD64") failed: %d\n",
                       libcfs_id2str(conn->c_peer), portal, xid, rc);
                rc2 = LNetMDUnlink(*mdh);
                LASSERTF(rc2 == 0, "rc2 = %d\n", rc2);
        }

        RETURN (0);
}

int ptlrpc_start_bulk_transfer(struct ptlrpc_bulk_desc *desc)
{
        struct ptlrpc_connection *conn = desc->bd_export->exp_connection;
        int                       rc;
        int                       rc2;
        lnet_md_t                 md;
        __u64                     xid;
        ENTRY;

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_BULK_PUT_NET))
                RETURN(0);

        /* NB no locking required until desc is on the network */
        LASSERT (!desc->bd_network_rw);
        LASSERT (desc->bd_type == BULK_PUT_SOURCE ||
                 desc->bd_type == BULK_GET_SINK);
        desc->bd_success = 0;
        desc->bd_sender = LNET_NID_ANY;

        md.user_ptr = &desc->bd_cbid;
        md.eq_handle = ptlrpc_eq_h;
        md.threshold = 2; /* SENT and ACK/REPLY */
        md.options = PTLRPC_MD_OPTIONS;
        ptlrpc_fill_bulk_md(&md, desc);

        LASSERT (desc->bd_cbid.cbid_fn == server_bulk_callback);
        LASSERT (desc->bd_cbid.cbid_arg == desc);

        /* NB total length may be 0 for a read past EOF, so we send a 0
         * length bulk, since the client expects a bulk event. */

        rc = LNetMDBind(md, LNET_UNLINK, &desc->bd_md_h);
        if (rc != 0) {
                CERROR("LNetMDBind failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                RETURN(-ENOMEM);
        }

        /* Client's bulk and reply matchbits are the same */
        xid = desc->bd_req->rq_xid;
        CDEBUG(D_NET, "Transferring %u pages %u bytes via portal %d "
               "id %s xid "LPX64"\n", desc->bd_iov_count,
               desc->bd_nob, desc->bd_portal,
               libcfs_id2str(conn->c_peer), xid);

        /* Network is about to get at the memory */
        desc->bd_network_rw = 1;

        if (desc->bd_type == BULK_PUT_SOURCE)
                rc = LNetPut (conn->c_self, desc->bd_md_h, LNET_ACK_REQ,
                              conn->c_peer, desc->bd_portal, xid, 0, 0);
        else
                rc = LNetGet (conn->c_self, desc->bd_md_h,
                              conn->c_peer, desc->bd_portal, xid, 0);

        if (rc != 0) {
                /* Can't send, so we unlink the MD bound above.  The UNLINK
                 * event this creates will signal completion with failure,
                 * so we return SUCCESS here! */
                CERROR("Transfer(%s, %d, "LPX64") failed: %d\n",
                       libcfs_id2str(conn->c_peer), desc->bd_portal, xid, rc);
                rc2 = LNetMDUnlink(desc->bd_md_h);
                LASSERT (rc2 == 0);
        }

        RETURN(0);
}

/* Server side bulk abort. Idempotent. Not thread-safe (i.e. only
 * serialises with completion callback) */
void ptlrpc_abort_bulk(struct ptlrpc_bulk_desc *desc)
{
        struct l_wait_info       lwi;
        int                      rc;

        LASSERT(!in_interrupt());               /* might sleep */

        if (!ptlrpc_server_bulk_active(desc))   /* completed or */
                return;                         /* never started */

        /* We used to poison the pages with 0xab here because we did not want to
         * send any meaningful data over the wire for evicted clients (bug 9297)
         * However, this is no longer safe now that we use the page cache on the
         * OSS (bug 20560) */

        /* The unlink ensures the callback happens ASAP and is the last
         * one.  If it fails, it must be because completion just happened,
         * but we must still l_wait_event() in this case, to give liblustre
         * a chance to run server_bulk_callback()*/

        LNetMDUnlink(desc->bd_md_h);

        for (;;) {
                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT_INTERVAL(cfs_time_seconds(LONG_UNLINK),
                                           cfs_time_seconds(1), NULL, NULL);
                rc = l_wait_event(desc->bd_waitq,
                                  !ptlrpc_server_bulk_active(desc), &lwi);
                if (rc == 0)
                        return;

                LASSERT(rc == -ETIMEDOUT);
                CWARN("Unexpectedly long timeout: desc %p\n", desc);
        }
}

int ptlrpc_register_bulk(struct ptlrpc_request *req)
{
        struct ptlrpc_bulk_desc *desc = req->rq_bulk;
        lnet_process_id_t peer;
        int rc;
        int rc2;
        lnet_handle_me_t  me_h;
        lnet_md_t         md;
        ENTRY;

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_BULK_GET_NET))
                RETURN(0);

        /* NB no locking required until desc is on the network */
        LASSERT (desc->bd_nob > 0);
        LASSERT (!desc->bd_network_rw);
        LASSERT (desc->bd_iov_count <= PTLRPC_MAX_BRW_PAGES);
        LASSERT (desc->bd_req != NULL);
        LASSERT (desc->bd_type == BULK_PUT_SINK ||
                 desc->bd_type == BULK_GET_SOURCE);

        desc->bd_success = 0;
        desc->bd_sender = LNET_NID_ANY;

        peer = desc->bd_import->imp_connection->c_peer;

        md.user_ptr = &desc->bd_cbid;
        md.eq_handle = ptlrpc_eq_h;
        md.threshold = 1;                       /* PUT or GET */
        md.options = PTLRPC_MD_OPTIONS |
                     ((desc->bd_type == BULK_GET_SOURCE) ?
                      LNET_MD_OP_GET : LNET_MD_OP_PUT);
        ptlrpc_fill_bulk_md(&md, desc);

        LASSERT (desc->bd_cbid.cbid_fn == client_bulk_callback);
        LASSERT (desc->bd_cbid.cbid_arg == desc);

        /* XXX Registering the same xid on retried bulk makes my head
         * explode trying to understand how the original request's bulk
         * might interfere with the retried request -eeb
         * On the other hand replaying with the same xid is fine, since
         * we are guaranteed old request have completed. -green */
        LASSERTF(!(desc->bd_registered &&
                 req->rq_send_state != LUSTRE_IMP_REPLAY) ||
                 req->rq_xid != desc->bd_last_xid,
                 "registered: %d  rq_xid: "LPU64" bd_last_xid: "LPU64"\n",
                 desc->bd_registered, req->rq_xid, desc->bd_last_xid);
        desc->bd_registered = 1;
        desc->bd_last_xid = req->rq_xid;

        rc = LNetMEAttach(desc->bd_portal, peer,
                         req->rq_xid, 0, LNET_UNLINK, LNET_INS_AFTER, &me_h);
        if (rc != 0) {
                CERROR("LNetMEAttach failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                RETURN (-ENOMEM);
        }

        /* About to let the network at it... */
        desc->bd_network_rw = 1;
        rc = LNetMDAttach(me_h, md, LNET_UNLINK, &desc->bd_md_h);
        if (rc != 0) {
                CERROR("LNetMDAttach failed: %d\n", rc);
                LASSERT (rc == -ENOMEM);
                desc->bd_network_rw = 0;
                rc2 = LNetMEUnlink (me_h);
                LASSERT (rc2 == 0);
                RETURN (-ENOMEM);
        }

        CDEBUG(D_NET, "Setup bulk %s buffers: %u pages %u bytes, xid "LPU64", "
               "portal %u\n",
               desc->bd_type == BULK_GET_SOURCE ? "get-source" : "put-sink",
               desc->bd_iov_count, desc->bd_nob,
               req->rq_xid, desc->bd_portal);
        RETURN(0);
}

/* Disconnect a bulk desc from the network. Idempotent. Not
 * thread-safe (i.e. only interlocks with completion callback). */
int ptlrpc_unregister_bulk(struct ptlrpc_request *req, int async)
{
        struct ptlrpc_bulk_desc *desc = req->rq_bulk;
        cfs_waitq_t             *wq;
        struct l_wait_info       lwi;
        int                      rc;
        ENTRY;

        LASSERT(!in_interrupt());     /* might sleep */

        /* Let's setup deadline for reply unlink. */
        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_BULK_UNLINK) &&
            async && req->rq_bulk_deadline == 0)
                req->rq_bulk_deadline = cfs_time_current_sec() + LONG_UNLINK;

        if (!ptlrpc_client_bulk_active(req))  /* completed or */
                RETURN(1);                    /* never registered */

        LASSERT(desc->bd_req == req);  /* bd_req NULL until registered */

        /* the unlink ensures the callback happens ASAP and is the last
         * one.  If it fails, it must be because completion just happened,
         * but we must still l_wait_event() in this case to give liblustre
         * a chance to run client_bulk_callback() */

        LNetMDUnlink(desc->bd_md_h);

        if (!ptlrpc_client_bulk_active(req))  /* completed or */
                RETURN(1);                    /* never registered */

        /* Move to "Unregistering" phase as bulk was not unlinked yet. */
        ptlrpc_rqphase_move(req, RQ_PHASE_UNREGISTERING);

        /* Do not wait for unlink to finish. */
        if (async)
                RETURN(0);

        if (req->rq_set != NULL)
                wq = &req->rq_set->set_waitq;
        else
                wq = &req->rq_reply_waitq;

        for (;;) {
                /* Network access will complete in finite time but the HUGE
                 * timeout lets us CWARN for visibility of sluggish NALs */
                lwi = LWI_TIMEOUT_INTERVAL(cfs_time_seconds(LONG_UNLINK),
                                           cfs_time_seconds(1), NULL, NULL);
                rc = l_wait_event(*wq, !ptlrpc_client_bulk_active(req), &lwi);
                if (rc == 0) {
                        ptlrpc_rqphase_move(req, req->rq_next_phase);
                        RETURN(1);
                }

                LASSERT(rc == -ETIMEDOUT);
                DEBUG_REQ(D_WARNING, req, "Unexpectedly long timeout: desc %p",
                          desc);
        }
        RETURN(0);
}

int ptlrpc_send_reply(struct ptlrpc_request *req, int flags)
{
        struct ptlrpc_service     *svc = req->rq_rqbd->rqbd_service;
        struct ptlrpc_reply_state *rs = req->rq_reply_state;
        struct ptlrpc_connection  *conn;
        int                        service_time;
        unsigned int               offset = 0;
        int                        rc;

        /* We must already have a reply buffer (only ptlrpc_error() may be
         * called without one).  We must also have a request buffer which
         * is either the actual (swabbed) incoming request, or a saved copy
         * if this is a req saved in target_queue_final_reply(). */
        LASSERT (req->rq_reqmsg != NULL);
        LASSERT (req->rq_repmsg != NULL);
        LASSERT (rs != NULL);
        LASSERT (req->rq_repmsg == rs->rs_msg);
        LASSERT ((flags & PTLRPC_REPLY_MAYBE_DIFFICULT) || !rs->rs_difficult);
        LASSERT (rs->rs_cb_id.cbid_fn == reply_out_callback);
        LASSERT (rs->rs_cb_id.cbid_arg == rs);

        /* There may be no rq_export during failover */

        if (req->rq_export && req->rq_export->exp_obd &&
            req->rq_export->exp_obd->obd_fail) {
                /* Failed obd's only send ENODEV */
                req->rq_type = PTL_RPC_MSG_ERR;
                req->rq_status = -ENODEV;
                CDEBUG(D_HA, "sending ENODEV from failed obd %d\n",
                       req->rq_export->exp_obd->obd_minor);
        }

        if (req->rq_type != PTL_RPC_MSG_ERR)
                req->rq_type = PTL_RPC_MSG_REPLY;

        lustre_msg_set_type(req->rq_repmsg, req->rq_type);
        lustre_msg_set_status(req->rq_repmsg, req->rq_status);
        lustre_msg_set_opc(req->rq_repmsg, lustre_msg_get_opc(req->rq_reqmsg));

        service_time = max_t(int, cfs_time_current_sec() -
                             req->rq_arrival_time.tv_sec, 1);
        if (!(flags & PTLRPC_REPLY_EARLY) &&
            (req->rq_type != PTL_RPC_MSG_ERR) &&
            !(lustre_msg_get_flags(req->rq_reqmsg) &
              (MSG_RESENT | MSG_REPLAY | MSG_LAST_REPLAY))) {
                /* early replies, errors and recovery requests don't count
                 * toward our service time estimate */
                int oldse = at_add(&svc->srv_at_estimate, service_time);
                if (oldse != 0)
                        DEBUG_REQ(D_ADAPTTO, req,
                                  "svc %s changed estimate from %d to %d",
                                  svc->srv_name, oldse,
                                  at_get(&svc->srv_at_estimate));
        }
        /* Report actual service time for client latency calc */
        lustre_msg_set_service_time(req->rq_repmsg, service_time);
        /* Report service time estimate for future client reqs, but report 0
         * (to be ignored by client) if it's a error reply during recovery.
         * (bz15815) */
        if (req->rq_type == PTL_RPC_MSG_ERR &&
            (req->rq_export == NULL || req->rq_export->exp_obd->obd_recovering))
                lustre_msg_set_timeout(req->rq_repmsg, 0);
        else
                lustre_msg_set_timeout(req->rq_repmsg,
                                       at_get(&svc->srv_at_estimate));

        target_pack_pool_reply(req);

        if (lustre_msghdr_get_flags(req->rq_reqmsg) & MSGHDR_AT_SUPPORT) {
                /* early replies go to offset 0, regular replies go after that*/
                if (flags & PTLRPC_REPLY_EARLY) {
                        offset = 0;
                        /* Include a checksum on early replies - must be done
                           after all other lustre_msg_set_* */
                        lustre_msg_set_cksum(req->rq_repmsg,
                                         lustre_msg_calc_cksum(req->rq_repmsg));
                } else {
                        offset = lustre_msg_early_size(req);
                }
        } else {
                CDEBUG(D_ADAPTTO, "No early reply support: flags=%#x "
                       "req_flags=%#x magic=%d:%x/%x len=%d\n",
                      flags, lustre_msg_get_flags(req->rq_reqmsg),
                      lustre_msg_is_v1(req->rq_reqmsg),
                      lustre_msg_get_magic(req->rq_reqmsg),
                      lustre_msg_get_magic(req->rq_repmsg), req->rq_replen);
        }

        if (req->rq_export == NULL || req->rq_export->exp_connection == NULL)
                conn = ptlrpc_connection_get(req->rq_peer, req->rq_self, NULL);
        else
                conn = ptlrpc_connection_addref(req->rq_export->exp_connection);

        if (conn == NULL) {
                CERROR("not replying on NULL connection\n"); /* bug 9635 */
                return -ENOTCONN;
        }

        atomic_inc (&svc->srv_outstanding_replies);
        ptlrpc_rs_addref(rs);                   /* +1 ref for the network */
        req->rq_sent = cfs_time_current_sec();

        rc = ptl_send_buf (&rs->rs_md_h, req->rq_repmsg, req->rq_replen,
                           rs->rs_difficult ? LNET_ACK_REQ : LNET_NOACK_REQ,
                           &rs->rs_cb_id, conn, svc->srv_rep_portal,
                           req->rq_xid, offset);
        if (rc != 0) {
                atomic_dec (&svc->srv_outstanding_replies);
                ptlrpc_req_drop_rs(req);
        }
        ptlrpc_connection_put(conn);
        return rc;
}

int ptlrpc_reply (struct ptlrpc_request *req)
{
        return (ptlrpc_send_reply (req, 0));
}

int ptlrpc_send_error(struct ptlrpc_request *req, int may_be_difficult)
{
        int rc;
        ENTRY;

        if (!req->rq_repmsg) {
                rc = lustre_pack_reply(req, 1, NULL, NULL);
                if (rc)
                        RETURN(rc);
        }

        req->rq_type = PTL_RPC_MSG_ERR;

        rc = ptlrpc_send_reply(req, may_be_difficult);
        RETURN(rc);
}

int ptlrpc_error(struct ptlrpc_request *req)
{
        return ptlrpc_send_error(req, 0);
}

int ptl_send_rpc(struct ptlrpc_request *request, int noreply)
{
        int rc;
        int rc2;
        struct ptlrpc_connection *connection;
        lnet_handle_me_t  reply_me_h;
        lnet_md_t         reply_md;
        struct obd_device *obd = request->rq_import->imp_obd;
        ENTRY;

        OBD_FAIL_RETURN(OBD_FAIL_PTLRPC_DROP_RPC, 0);

        LASSERT (request->rq_type == PTL_RPC_MSG_REQUEST);

        /* If this is a re-transmit, we're required to have disengaged
         * cleanly from the previous attempt */
        LASSERT(!request->rq_receiving_reply);

        if (request->rq_import->imp_obd &&
            request->rq_import->imp_obd->obd_fail) {
                CDEBUG(D_HA, "muting rpc for failed imp obd %s\n",
                       request->rq_import->imp_obd->obd_name);
                /* this prevents us from waiting in ptlrpc_queue_wait */
                request->rq_err = 1;
                request->rq_status = -ENODEV;
                RETURN(-ENODEV);
        }

        connection = request->rq_import->imp_connection;

        if (request->rq_bulk != NULL) {
                rc = ptlrpc_register_bulk (request);
                if (rc != 0)
                        RETURN(rc);
        }

        lustre_msg_set_handle(request->rq_reqmsg,
                              &request->rq_import->imp_remote_handle);
        lustre_msg_set_type(request->rq_reqmsg, PTL_RPC_MSG_REQUEST);
        lustre_msg_set_conn_cnt(request->rq_reqmsg,
                                request->rq_import->imp_conn_cnt);
        lustre_msghdr_set_flags(request->rq_reqmsg,
                                request->rq_import->imp_msghdr_flags);

        if (request->rq_resend)
                lustre_msg_add_flags(request->rq_reqmsg, MSG_RESENT);

        if (!noreply) {
                LASSERT (request->rq_replen != 0);
                if (request->rq_repbuf == NULL)
                        OBD_ALLOC(request->rq_repbuf, request->rq_replen);
                if (request->rq_repbuf == NULL) {
                        /* this prevents us from looping in ptlrpc_queue_wait */
                        request->rq_err = 1;
                        request->rq_status = -ENOMEM;
                        GOTO(cleanup_bulk, rc = -ENOMEM);
                }
                request->rq_repmsg = NULL;

                rc = LNetMEAttach(request->rq_reply_portal,/*XXX FIXME bug 249*/
                                  connection->c_peer, request->rq_xid, 0,
                                  LNET_UNLINK, LNET_INS_AFTER, &reply_me_h);
                if (rc != 0) {
                        CERROR("LNetMEAttach failed: %d\n", rc);
                        LASSERT (rc == -ENOMEM);
                        GOTO(cleanup_repmsg, rc = -ENOMEM);
                }
        }

        spin_lock(&request->rq_lock);
        /* If the MD attach succeeds, there _will_ be a reply_in callback */
        request->rq_receiving_reply = !noreply;
        /* We are responsible for unlinking the reply buffer */
        request->rq_must_unlink = !noreply;
        /* Clear any flags that may be present from previous sends. */
        request->rq_replied = 0;
        request->rq_err = 0;
        request->rq_timedout = 0;
        request->rq_net_err = 0;
        request->rq_resend = 0;
        request->rq_restart = 0;
        request->rq_rep_swab_mask = 0;
        request->rq_reply_truncate = 0;
        spin_unlock(&request->rq_lock);

        if (!noreply) {
                reply_md.start     = request->rq_repbuf;
                reply_md.length    = request->rq_replen;
                /* Allow multiple early replies */
                reply_md.threshold = LNET_MD_THRESH_INF;
                /* Manage remote for early replies */
                reply_md.options   = PTLRPC_MD_OPTIONS | LNET_MD_OP_PUT |
                        LNET_MD_MANAGE_REMOTE |
                        LNET_MD_TRUNCATE; /* allow to make EBIG error */
                reply_md.user_ptr  = &request->rq_reply_cbid;
                reply_md.eq_handle = ptlrpc_eq_h;

                /* We must see the unlink callback to unset rq_must_unlink,
                   so we can't auto-unlink */
                rc = LNetMDAttach(reply_me_h, reply_md, LNET_RETAIN,
                                 &request->rq_reply_md_h);
                if (rc != 0) {
                        CERROR("LNetMDAttach failed: %d\n", rc);
                        LASSERT (rc == -ENOMEM);
                        spin_lock(&request->rq_lock);
                        /* ...but the MD attach didn't succeed... */
                        request->rq_receiving_reply = 0;
                        spin_unlock(&request->rq_lock);
                        GOTO(cleanup_me, rc = -ENOMEM);
                }

                CDEBUG(D_NET, "Setup reply buffer: %u bytes, xid "LPU64
                       ", portal %u\n",
                       request->rq_replen, request->rq_xid,
                       request->rq_reply_portal);
        }

        /* add references on request for request_out_callback */
        ptlrpc_request_addref(request);
        if (obd->obd_svc_stats != NULL)
                lprocfs_counter_add(obd->obd_svc_stats, PTLRPC_REQACTIVE_CNTR,
                                atomic_read(&request->rq_import->imp_inflight));

        OBD_FAIL_TIMEOUT(OBD_FAIL_PTLRPC_DELAY_SEND, request->rq_timeout + 5);

        request->rq_sent = cfs_time_current_sec();
        do_gettimeofday(&request->rq_arrival_time);
        /* We give the server rq_timeout secs to process the req, and
           add the network latency for our local timeout. */
        request->rq_deadline = request->rq_sent + request->rq_timeout +
                    ptlrpc_at_get_net_latency(request);

        ptlrpc_pinger_sending_on_import(request->rq_import);

        DEBUG_REQ(D_INFO, request, "send flg=%x",
                  lustre_msg_get_flags(request->rq_reqmsg));
        rc = ptl_send_buf(&request->rq_req_md_h,
                          request->rq_reqmsg, request->rq_reqlen,
                          LNET_NOACK_REQ, &request->rq_req_cbid,
                          connection,
                          request->rq_request_portal,
                          request->rq_xid, 0);
        if (rc == 0)
                RETURN(rc);

        ptlrpc_req_finished(request);
        if (noreply)
                RETURN(rc);

 cleanup_me:
        /* MEUnlink is safe; the PUT didn't even get off the ground, and
         * nobody apart from the PUT's target has the right nid+XID to
         * access the reply buffer. */
        rc2 = LNetMEUnlink(reply_me_h);
        LASSERT (rc2 == 0);
        /* UNLINKED callback called synchronously */
        LASSERT(!request->rq_receiving_reply);

 cleanup_repmsg:
        OBD_FREE(request->rq_repbuf, request->rq_replen);
        request->rq_repbuf = NULL;
        request->rq_repmsg = NULL; //remove

 cleanup_bulk:
        /* We do sync unlink here as there was no real transfer here so
         * the chance to have long unlink to sluggish net is smaller here. */
        ptlrpc_unregister_bulk(request, 0);
        return rc;
}

int ptlrpc_register_rqbd(struct ptlrpc_request_buffer_desc *rqbd)
{
        struct ptlrpc_service   *service = rqbd->rqbd_service;
        static lnet_process_id_t  match_id = {LNET_NID_ANY, LNET_PID_ANY};
        int                      rc;
        lnet_md_t                 md;
        lnet_handle_me_t          me_h;

        CDEBUG(D_NET, "LNetMEAttach: portal %d\n",
               service->srv_req_portal);

        if (OBD_FAIL_CHECK_ONCE(OBD_FAIL_PTLRPC_RQBD))
                return (-ENOMEM);

        rc = LNetMEAttach(service->srv_req_portal,
                          match_id, 0, ~0, LNET_UNLINK, LNET_INS_AFTER, &me_h);
        if (rc != 0) {
                CERROR("LNetMEAttach failed: %d\n", rc);
                return (-ENOMEM);
        }

        LASSERT(rqbd->rqbd_refcount == 0);
        rqbd->rqbd_refcount = 1;

        md.start     = rqbd->rqbd_buffer;
        md.length    = service->srv_buf_size;
        md.max_size  = service->srv_max_req_size;
        md.threshold = LNET_MD_THRESH_INF;
        md.options   = PTLRPC_MD_OPTIONS | LNET_MD_OP_PUT | LNET_MD_MAX_SIZE;
        md.user_ptr  = &rqbd->rqbd_cbid;
        md.eq_handle = ptlrpc_eq_h;

        rc = LNetMDAttach(me_h, md, LNET_UNLINK, &rqbd->rqbd_md_h);
        if (rc == 0)
                return (0);

        CERROR("LNetMDAttach failed: %d; \n", rc);
        LASSERT (rc == -ENOMEM);
        rc = LNetMEUnlink (me_h);
        LASSERT (rc == 0);
        rqbd->rqbd_refcount = 0;

        return (-ENOMEM);
}
