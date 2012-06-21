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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lnet/selftest/conctl.c
 *
 * Console framework rpcs
 *
 * Author: Liang Zhen <liangzhen@clusterfs.com>
 */

#ifdef __KERNEL__

#include <libcfs/libcfs.h>
#include <lnet/lib-lnet.h>
#include "timer.h"
#include "conrpc.h"
#include "console.h"

void lstcon_rpc_stat_reply(int, srpc_msg_t *,
                           lstcon_node_t *, lstcon_trans_stat_t *);

static void
lstcon_rpc_done(srpc_client_rpc_t *rpc)
{
        lstcon_rpc_t *crpc = (lstcon_rpc_t *)rpc->crpc_priv;

        LASSERT (crpc != NULL && rpc == crpc->crp_rpc);
        LASSERT (crpc->crp_posted && !crpc->crp_finished);

        cfs_spin_lock(&rpc->crpc_lock);

        if (crpc->crp_trans == NULL) {
                /* Orphan RPC is not in any transaction, 
                 * I'm just a poor body and nobody loves me */
                cfs_spin_unlock(&rpc->crpc_lock);

                /* release it */
                lstcon_rpc_put(crpc);
                return;
        }

        /* not an orphan RPC */
        crpc->crp_finished = 1;

        if (crpc->crp_stamp == 0) {
                /* not aborted */
                LASSERT (crpc->crp_status == 0);

                crpc->crp_stamp  = cfs_time_current();
                crpc->crp_status = rpc->crpc_status;
        }

        /* wakeup (transaction)thread if I'm the last RPC in the transaction */
        if (cfs_atomic_dec_and_test(&crpc->crp_trans->tas_remaining))
                cfs_waitq_signal(&crpc->crp_trans->tas_waitq);

        cfs_spin_unlock(&rpc->crpc_lock);
}

int
lstcon_rpc_init(lstcon_node_t *nd, int service,
                int npg, int cached, lstcon_rpc_t *crpc)
{

        crpc->crp_rpc = sfw_create_rpc(nd->nd_id, service, 
                                       npg, npg * CFS_PAGE_SIZE,
                                       lstcon_rpc_done, (void *)crpc);
        if (crpc->crp_rpc == NULL)
                return -ENOMEM;

        crpc->crp_trans    = NULL;
        crpc->crp_node     = nd;
        crpc->crp_posted   = 0;
        crpc->crp_finished = 0;
        crpc->crp_unpacked = 0;
        crpc->crp_status   = 0;
        crpc->crp_stamp    = 0;
        crpc->crp_static   = !cached;
        CFS_INIT_LIST_HEAD(&crpc->crp_link);

        cfs_atomic_inc(&console_session.ses_rpc_counter);

        return 0;
}

int
lstcon_rpc_prep(lstcon_node_t *nd, int service,
                int npg, lstcon_rpc_t **crpcpp)
{
        lstcon_rpc_t  *crpc = NULL;
        int            rc;

        cfs_spin_lock(&console_session.ses_rpc_lock);

        if (!cfs_list_empty(&console_session.ses_rpc_freelist)) {
                crpc = cfs_list_entry(console_session.ses_rpc_freelist.next,
                                      lstcon_rpc_t, crp_link);
                cfs_list_del_init(&crpc->crp_link);
        }

        cfs_spin_unlock(&console_session.ses_rpc_lock);

        if (crpc == NULL) {
                LIBCFS_ALLOC(crpc, sizeof(*crpc));
                if (crpc == NULL)
                        return -ENOMEM;
        }

        rc = lstcon_rpc_init(nd, service, npg, 1, crpc);
        if (rc == 0) {
                *crpcpp = crpc;
                return 0;
        }

        LIBCFS_FREE(crpc, sizeof(*crpc));

        return rc;
}

void
lstcon_rpc_put(lstcon_rpc_t *crpc)
{
        srpc_bulk_t *bulk = &crpc->crp_rpc->crpc_bulk;
        int          i;

        LASSERT (cfs_list_empty(&crpc->crp_link));

        for (i = 0; i < bulk->bk_niov; i++) {
                if (bulk->bk_iovs[i].kiov_page == NULL)
                        continue;

                cfs_free_page(bulk->bk_iovs[i].kiov_page);
        }

        srpc_client_rpc_decref(crpc->crp_rpc);

        if (crpc->crp_static) {
                /* Static RPC, not allocated */
                memset(crpc, 0, sizeof(*crpc));
                crpc->crp_static = 1;

        } else {
                cfs_spin_lock(&console_session.ses_rpc_lock);

                cfs_list_add(&crpc->crp_link,
                             &console_session.ses_rpc_freelist);

                cfs_spin_unlock(&console_session.ses_rpc_lock);
        }

        /* RPC is not alive now */
        cfs_atomic_dec(&console_session.ses_rpc_counter);
}

void
lstcon_rpc_post(lstcon_rpc_t *crpc)
{
        lstcon_rpc_trans_t *trans = crpc->crp_trans;

        LASSERT (trans != NULL);

        cfs_atomic_inc(&trans->tas_remaining);
        crpc->crp_posted = 1;

        sfw_post_rpc(crpc->crp_rpc);
}

static char *
lstcon_rpc_trans_name(int transop)
{
        if (transop == LST_TRANS_SESNEW)
                return "SESNEW";

        if (transop == LST_TRANS_SESEND)
                return "SESEND";

        if (transop == LST_TRANS_SESQRY)
                return "SESQRY";

        if (transop == LST_TRANS_SESPING)
                return "SESPING";

        if (transop == LST_TRANS_TSBCLIADD)
                return "TSBCLIADD";

        if (transop == LST_TRANS_TSBSRVADD)
                return "TSBSRVADD";

        if (transop == LST_TRANS_TSBRUN)
                return "TSBRUN";

        if (transop == LST_TRANS_TSBSTOP)
                return "TSBSTOP";

        if (transop == LST_TRANS_TSBCLIQRY)
                return "TSBCLIQRY";

        if (transop == LST_TRANS_TSBSRVQRY)
                return "TSBSRVQRY";

        if (transop == LST_TRANS_STATQRY)
                return "STATQRY";

        return "Unknown";
}

int
lstcon_rpc_trans_prep(cfs_list_t *translist,
                      int transop, lstcon_rpc_trans_t **transpp)
{
        lstcon_rpc_trans_t *trans;

        if (translist != NULL) {
                cfs_list_for_each_entry_typed(trans, translist,
                                              lstcon_rpc_trans_t, tas_link) {
                        /* Can't enqueue two private transaction on
                         * the same object */
                        if ((trans->tas_opc & transop) == LST_TRANS_PRIVATE)
                                return -EPERM;
                }
        }

        /* create a trans group */
        LIBCFS_ALLOC(trans, sizeof(*trans));
        if (trans == NULL)
                return -ENOMEM;
        
        trans->tas_opc = transop;

        if (translist == NULL)       
                CFS_INIT_LIST_HEAD(&trans->tas_olink);
        else
                cfs_list_add_tail(&trans->tas_olink, translist);

        cfs_list_add_tail(&trans->tas_link, &console_session.ses_trans_list);

        CFS_INIT_LIST_HEAD(&trans->tas_rpcs_list);
        cfs_atomic_set(&trans->tas_remaining, 0);
        cfs_waitq_init(&trans->tas_waitq);

        *transpp = trans;

        return 0;
}

void
lstcon_rpc_trans_addreq(lstcon_rpc_trans_t *trans, lstcon_rpc_t *crpc)
{
        cfs_list_add_tail(&crpc->crp_link, &trans->tas_rpcs_list);
        crpc->crp_trans = trans;
}

void
lstcon_rpc_trans_abort(lstcon_rpc_trans_t *trans, int error)
{
        srpc_client_rpc_t *rpc;
        lstcon_rpc_t      *crpc;
        lstcon_node_t     *nd;

        cfs_list_for_each_entry_typed (crpc, &trans->tas_rpcs_list,
                                       lstcon_rpc_t, crp_link) {
                rpc = crpc->crp_rpc;

                cfs_spin_lock(&rpc->crpc_lock);

                if (!crpc->crp_posted || /* not posted */
                    crpc->crp_stamp != 0) { /* rpc done or aborted already */
                        if (crpc->crp_stamp == 0) {
                                crpc->crp_stamp = cfs_time_current();
                                crpc->crp_status = -EINTR;
                        }
                        cfs_spin_unlock(&rpc->crpc_lock);
                        continue;
                }

                crpc->crp_stamp  = cfs_time_current();
                crpc->crp_status = error;

                cfs_spin_unlock(&rpc->crpc_lock);

                sfw_abort_rpc(rpc);

                if  (error != ETIMEDOUT)
                        continue;

                nd = crpc->crp_node;
                if (cfs_time_after(nd->nd_stamp, crpc->crp_stamp))
                        continue;

                nd->nd_stamp = crpc->crp_stamp;
                nd->nd_state = LST_NODE_DOWN;
        }
}

static int
lstcon_rpc_trans_check(lstcon_rpc_trans_t *trans)
{
        if (console_session.ses_shutdown &&
            !cfs_list_empty(&trans->tas_olink)) /* Not an end session RPC */
                return 1;

        return (cfs_atomic_read(&trans->tas_remaining) == 0) ? 1: 0;
}

int
lstcon_rpc_trans_postwait(lstcon_rpc_trans_t *trans, int timeout)
{
        lstcon_rpc_t  *crpc;
        int            rc;

        if (cfs_list_empty(&trans->tas_rpcs_list))
                return 0;

        if (timeout < LST_TRANS_MIN_TIMEOUT)
                timeout = LST_TRANS_MIN_TIMEOUT;

        CDEBUG(D_NET, "Transaction %s started\n",
               lstcon_rpc_trans_name(trans->tas_opc));

        /* post all requests */
        cfs_list_for_each_entry_typed (crpc, &trans->tas_rpcs_list,
                                       lstcon_rpc_t, crp_link) {
                LASSERT (!crpc->crp_posted);

                lstcon_rpc_post(crpc);
        }

        cfs_mutex_unlock(&console_session.ses_mutex);

        cfs_waitq_wait_event_interruptible_timeout(trans->tas_waitq,
                                              lstcon_rpc_trans_check(trans),
                                              cfs_time_seconds(timeout), rc);

        rc = (rc > 0)? 0: ((rc < 0)? -EINTR: -ETIMEDOUT);

        cfs_mutex_lock(&console_session.ses_mutex);

        if (console_session.ses_shutdown)
                rc = -ESHUTDOWN;

        if (rc != 0 || atomic_read(&trans->tas_remaining) != 0) {
                /* treat short timeout as canceled */
                if (rc == -ETIMEDOUT && timeout < LST_TRANS_MIN_TIMEOUT * 2)
                        rc = -EINTR;

                lstcon_rpc_trans_abort(trans, rc);
        }

        CDEBUG(D_NET, "Transaction %s stopped: %d\n",
               lstcon_rpc_trans_name(trans->tas_opc), rc);

        lstcon_rpc_trans_stat(trans, lstcon_trans_stat());

        return rc;
}

int
lstcon_rpc_get_reply(lstcon_rpc_t *crpc, srpc_msg_t **msgpp)
{
        lstcon_node_t        *nd  = crpc->crp_node;
        srpc_client_rpc_t    *rpc = crpc->crp_rpc;
        srpc_generic_reply_t *rep;

        LASSERT (nd != NULL && rpc != NULL);
        LASSERT (crpc->crp_stamp != 0);

        if (crpc->crp_status != 0) {
                *msgpp = NULL;
                return crpc->crp_status;
        }

        *msgpp = &rpc->crpc_replymsg;
        if (!crpc->crp_unpacked) {
                sfw_unpack_message(*msgpp);
                crpc->crp_unpacked = 1;
        }
       
        if (cfs_time_after(nd->nd_stamp, crpc->crp_stamp))
                return 0;

        nd->nd_stamp = crpc->crp_stamp;
        rep = &(*msgpp)->msg_body.reply;

        if (rep->sid.ses_nid == LNET_NID_ANY)
                nd->nd_state = LST_NODE_UNKNOWN;
        else if (lstcon_session_match(rep->sid))
                nd->nd_state = LST_NODE_ACTIVE;
        else
                nd->nd_state = LST_NODE_BUSY;

        return 0;
}

void
lstcon_rpc_trans_stat(lstcon_rpc_trans_t *trans, lstcon_trans_stat_t *stat)
{
        lstcon_rpc_t      *crpc;
        srpc_msg_t        *rep;
        int                error;

        LASSERT (stat != NULL);

        memset(stat, 0, sizeof(*stat));

        cfs_list_for_each_entry_typed(crpc, &trans->tas_rpcs_list,
                                      lstcon_rpc_t, crp_link) {
                lstcon_rpc_stat_total(stat, 1);

                LASSERT (crpc->crp_stamp != 0);

                error = lstcon_rpc_get_reply(crpc, &rep);
                if (error != 0) {
                        lstcon_rpc_stat_failure(stat, 1);
                        if (stat->trs_rpc_errno == 0)
                                stat->trs_rpc_errno = -error;

                        continue;
                }

                lstcon_rpc_stat_success(stat, 1);

                lstcon_rpc_stat_reply(trans->tas_opc, rep,
                                      crpc->crp_node, stat);
        }

        CDEBUG(D_NET, "transaction %s : success %d, failure %d, total %d, "
                      "RPC error(%d), Framework error(%d)\n",
               lstcon_rpc_trans_name(trans->tas_opc),
               lstcon_rpc_stat_success(stat, 0),
               lstcon_rpc_stat_failure(stat, 0),
               lstcon_rpc_stat_total(stat, 0),
               stat->trs_rpc_errno, stat->trs_fwk_errno);

        return;
}

int
lstcon_rpc_trans_interpreter(lstcon_rpc_trans_t *trans,
                             cfs_list_t *head_up,
                             lstcon_rpc_readent_func_t readent)
{
        cfs_list_t            tmp;
        cfs_list_t           *next;
        lstcon_rpc_ent_t     *ent;
        srpc_generic_reply_t *rep;
        lstcon_rpc_t         *crpc;
        srpc_msg_t           *msg;
        lstcon_node_t        *nd;
        cfs_duration_t        dur;
        struct timeval        tv;
        int                   error;

        LASSERT (head_up != NULL);

        next = head_up;

        cfs_list_for_each_entry_typed(crpc, &trans->tas_rpcs_list,
                                      lstcon_rpc_t, crp_link) {
                if (cfs_copy_from_user(&tmp, next,
                                       sizeof(cfs_list_t)))
                        return -EFAULT;

                if (tmp.next == head_up)
                        return 0;

                next = tmp.next;

                ent = cfs_list_entry(next, lstcon_rpc_ent_t, rpe_link);

                LASSERT (crpc->crp_stamp != 0);

                error = lstcon_rpc_get_reply(crpc, &msg);

                nd = crpc->crp_node;

                dur = (cfs_duration_t)cfs_time_sub(crpc->crp_stamp,
                      (cfs_time_t)console_session.ses_id.ses_stamp);
                cfs_duration_usec(dur, &tv);

                if (cfs_copy_to_user(&ent->rpe_peer,
                                     &nd->nd_id, sizeof(lnet_process_id_t)) ||
                    cfs_copy_to_user(&ent->rpe_stamp, &tv, sizeof(tv)) ||
                    cfs_copy_to_user(&ent->rpe_state,
                                     &nd->nd_state, sizeof(nd->nd_state)) ||
                    cfs_copy_to_user(&ent->rpe_rpc_errno, &error,
                                     sizeof(error)))
                        return -EFAULT;

                if (error != 0)
                        continue;

                /* RPC is done */
                rep = (srpc_generic_reply_t *)&msg->msg_body.reply;

                if (cfs_copy_to_user(&ent->rpe_sid,
                                     &rep->sid, sizeof(lst_sid_t)) ||
                    cfs_copy_to_user(&ent->rpe_fwk_errno,
                                     &rep->status, sizeof(rep->status)))
                        return -EFAULT;

                if (readent == NULL)
                        continue;

                if ((error = readent(trans->tas_opc, msg, ent)) != 0)
                        return error;
        }

        return 0;
}

void
lstcon_rpc_trans_destroy(lstcon_rpc_trans_t *trans)
{
        srpc_client_rpc_t *rpc;
        lstcon_rpc_t      *crpc;
        lstcon_rpc_t      *tmp;
        int                count = 0;

        cfs_list_for_each_entry_safe_typed(crpc, tmp,
                                           &trans->tas_rpcs_list,
                                           lstcon_rpc_t, crp_link) {
                rpc = crpc->crp_rpc;

                cfs_spin_lock(&rpc->crpc_lock);

                /* free it if not posted or finished already */
                if (!crpc->crp_posted || crpc->crp_finished) {
                        cfs_spin_unlock(&rpc->crpc_lock);

                        cfs_list_del_init(&crpc->crp_link);
                        lstcon_rpc_put(crpc);

                        continue;
                }

                /* rpcs can be still not callbacked (even LNetMDUnlink is called)
                 * because huge timeout for inaccessible network, don't make
                 * user wait for them, just abandon them, they will be recycled
                 * in callback */

                LASSERT (crpc->crp_status != 0);

                crpc->crp_node  = NULL;
                crpc->crp_trans = NULL;
                cfs_list_del_init(&crpc->crp_link);
                count ++;

                cfs_spin_unlock(&rpc->crpc_lock);

                cfs_atomic_dec(&trans->tas_remaining);
        }

        LASSERT (cfs_atomic_read(&trans->tas_remaining) == 0);

        cfs_list_del(&trans->tas_link);
        if (!cfs_list_empty(&trans->tas_olink))
                cfs_list_del(&trans->tas_olink);

        CDEBUG(D_NET, "Transaction %s destroyed with %d pending RPCs\n",
               lstcon_rpc_trans_name(trans->tas_opc), count);

        LIBCFS_FREE(trans, sizeof(*trans));

        return;
}

int
lstcon_sesrpc_prep(lstcon_node_t *nd, int transop, lstcon_rpc_t **crpc)
{
        srpc_mksn_reqst_t *msrq;
        srpc_rmsn_reqst_t *rsrq;
        int                rc;

        switch (transop) {
        case LST_TRANS_SESNEW:
                rc = lstcon_rpc_prep(nd, SRPC_SERVICE_MAKE_SESSION, 0, crpc);
                if (rc != 0)
                        return rc;

                msrq = &(*crpc)->crp_rpc->crpc_reqstmsg.msg_body.mksn_reqst;
                msrq->mksn_sid     = console_session.ses_id;
                msrq->mksn_force   = console_session.ses_force;
                strncpy(msrq->mksn_name, console_session.ses_name,
                        strlen(console_session.ses_name));
                break;

        case LST_TRANS_SESEND:
                rc = lstcon_rpc_prep(nd, SRPC_SERVICE_REMOVE_SESSION, 0, crpc);
                if (rc != 0)
                        return rc;

                rsrq = &(*crpc)->crp_rpc->crpc_reqstmsg.msg_body.rmsn_reqst;
                rsrq->rmsn_sid = console_session.ses_id;
                break;

        default:
                LBUG();
        }

        return 0;
}

int
lstcon_dbgrpc_prep(lstcon_node_t *nd, lstcon_rpc_t **crpc)
{
        srpc_debug_reqst_t *drq;
        int                 rc;

        rc = lstcon_rpc_prep(nd, SRPC_SERVICE_DEBUG, 0, crpc);
        if (rc != 0)
                return rc;

        drq = &(*crpc)->crp_rpc->crpc_reqstmsg.msg_body.dbg_reqst;

        drq->dbg_sid   = console_session.ses_id;
        drq->dbg_flags = 0;
        
        return rc;
}

int
lstcon_batrpc_prep(lstcon_node_t *nd, int transop,
                   lstcon_tsb_hdr_t *tsb, lstcon_rpc_t **crpc)
{
        lstcon_batch_t     *batch;
        srpc_batch_reqst_t *brq;
        int                 rc;

        rc = lstcon_rpc_prep(nd, SRPC_SERVICE_BATCH, 0, crpc);
        if (rc != 0)
                return rc;

        brq = &(*crpc)->crp_rpc->crpc_reqstmsg.msg_body.bat_reqst;

        brq->bar_sid     = console_session.ses_id;
        brq->bar_bid     = tsb->tsb_id;
        brq->bar_testidx = tsb->tsb_index;
        brq->bar_opc     = transop == LST_TRANS_TSBRUN ? SRPC_BATCH_OPC_RUN :
                           (transop == LST_TRANS_TSBSTOP ? SRPC_BATCH_OPC_STOP:
                            SRPC_BATCH_OPC_QUERY);

        if (transop != LST_TRANS_TSBRUN &&
            transop != LST_TRANS_TSBSTOP)
                return 0;

        LASSERT (tsb->tsb_index == 0);

        batch = (lstcon_batch_t *)tsb;
        brq->bar_arg = batch->bat_arg;
        
        return 0;
}

int
lstcon_statrpc_prep(lstcon_node_t *nd, lstcon_rpc_t **crpc)
{
        srpc_stat_reqst_t *srq;
        int                rc;

        rc = lstcon_rpc_prep(nd, SRPC_SERVICE_QUERY_STAT, 0, crpc);
        if (rc != 0)
                return rc;

        srq = &(*crpc)->crp_rpc->crpc_reqstmsg.msg_body.stat_reqst;

        srq->str_sid  = console_session.ses_id;
        srq->str_type = 0; /* XXX remove it */

        return 0;
}

lnet_process_id_packed_t *
lstcon_next_id(int idx, int nkiov, lnet_kiov_t *kiov)
{
        lnet_process_id_packed_t *pid;
        int                       i;

        i = idx / SFW_ID_PER_PAGE;
        
        LASSERT (i < nkiov);

        pid = (lnet_process_id_packed_t *)cfs_page_address(kiov[i].kiov_page);

        return &pid[idx % SFW_ID_PER_PAGE];
}

int
lstcon_dstnodes_prep(lstcon_group_t *grp, int idx,
                     int dist, int span, int nkiov, lnet_kiov_t *kiov)
{
        lnet_process_id_packed_t *pid;
        lstcon_ndlink_t          *ndl;
        lstcon_node_t            *nd;
        int                       start;
        int                       end;
        int                       i = 0;

        LASSERT (dist >= 1);
        LASSERT (span >= 1);
        LASSERT (grp->grp_nnode >= 1);

        if (span > grp->grp_nnode)
                return -EINVAL;

        start = ((idx / dist) * span) % grp->grp_nnode;
        end   = ((idx / dist) * span + span - 1) % grp->grp_nnode;

        cfs_list_for_each_entry_typed(ndl, &grp->grp_ndl_list,
                                      lstcon_ndlink_t, ndl_link) {
                nd = ndl->ndl_node;
                if (i < start) {
                        i ++;
                        continue;
                }

                if (i > (end >= start ? end: grp->grp_nnode))
                        break;

                pid = lstcon_next_id((i - start), nkiov, kiov);
                pid->nid = nd->nd_id.nid;
                pid->pid = nd->nd_id.pid;
                i++;
        }

        if (start <= end) /* done */
                return 0;

        cfs_list_for_each_entry_typed(ndl, &grp->grp_ndl_list,
                                      lstcon_ndlink_t, ndl_link) {
                if (i > grp->grp_nnode + end)
                        break;

                nd = ndl->ndl_node;
                pid = lstcon_next_id((i - start), nkiov, kiov);
                pid->nid = nd->nd_id.nid;
                pid->pid = nd->nd_id.pid;
                i++;
        }

        return 0;
}

int
lstcon_pingrpc_prep(lst_test_ping_param_t *param, srpc_test_reqst_t *req)
{
        test_ping_req_t *prq = &req->tsr_u.ping;
        
        prq->png_size   = param->png_size;
        prq->png_flags  = param->png_flags;
        /* TODO dest */
        return 0;
}

int
lstcon_bulkrpc_prep(lst_test_bulk_param_t *param, srpc_test_reqst_t *req)
{
        test_bulk_req_t *brq = &req->tsr_u.bulk;

        brq->blk_opc    = param->blk_opc;
        brq->blk_npg    = (param->blk_size + CFS_PAGE_SIZE - 1) / CFS_PAGE_SIZE;
        brq->blk_flags  = param->blk_flags;

        return 0;
}

int
lstcon_testrpc_prep(lstcon_node_t *nd, int transop,
                    lstcon_test_t *test, lstcon_rpc_t **crpc)
{
        lstcon_group_t    *sgrp = test->tes_src_grp;
        lstcon_group_t    *dgrp = test->tes_dst_grp;
        srpc_test_reqst_t *trq;
        srpc_bulk_t       *bulk;
        int                i;
        int                n  = 0;
        int                rc = 0;

        if (transop == LST_TRANS_TSBCLIADD)
                n = sfw_id_pages(test->tes_span);

        rc = lstcon_rpc_prep(nd, SRPC_SERVICE_TEST, n, crpc);
        if (rc != 0) 
                return rc;

        trq  = &(*crpc)->crp_rpc->crpc_reqstmsg.msg_body.tes_reqst;

        if (transop == LST_TRANS_TSBSRVADD) {
                int ndist = (sgrp->grp_nnode + test->tes_dist - 1) / test->tes_dist;
                int nspan = (dgrp->grp_nnode + test->tes_span - 1) / test->tes_span;
                int nmax = (ndist + nspan - 1) / nspan;

                trq->tsr_ndest = 0;
                trq->tsr_loop  = nmax * test->tes_dist * test->tes_concur;

        } else {
                bulk = &(*crpc)->crp_rpc->crpc_bulk;

                for (i = 0; i < n; i++) {
                        bulk->bk_iovs[i].kiov_offset = 0;
                        bulk->bk_iovs[i].kiov_len    = CFS_PAGE_SIZE;
                        bulk->bk_iovs[i].kiov_page   = cfs_alloc_page(CFS_ALLOC_STD);

                        if (bulk->bk_iovs[i].kiov_page != NULL) 
                                continue;

                        lstcon_rpc_put(*crpc);
                        return -ENOMEM;
                }

                bulk->bk_sink = 0;

                LASSERT (transop == LST_TRANS_TSBCLIADD);

                rc = lstcon_dstnodes_prep(test->tes_dst_grp,
                                          test->tes_cliidx++, test->tes_dist,
                                          test->tes_span, n, &bulk->bk_iovs[0]);
                if (rc != 0) {
                        lstcon_rpc_put(*crpc);
                        return rc;
                }

                trq->tsr_ndest = test->tes_span;
                trq->tsr_loop  = test->tes_loop;
        } 

        trq->tsr_sid        = console_session.ses_id;
        trq->tsr_bid        = test->tes_hdr.tsb_id;
        trq->tsr_concur     = test->tes_concur;
        trq->tsr_is_client  = (transop == LST_TRANS_TSBCLIADD) ? 1 : 0;
        trq->tsr_stop_onerr = !!test->tes_stop_onerr;

        switch (test->tes_type) {
        case LST_TEST_PING:
                trq->tsr_service = SRPC_SERVICE_PING;
                rc = lstcon_pingrpc_prep((lst_test_ping_param_t *)&test->tes_param[0], trq);
                break;
        case LST_TEST_BULK:
                trq->tsr_service = SRPC_SERVICE_BRW;
                rc = lstcon_bulkrpc_prep((lst_test_bulk_param_t *)&test->tes_param[0], trq);
                break;
        default:
                LBUG();
                break;
        }

        return rc;
}

void
lstcon_rpc_stat_reply(int transop, srpc_msg_t *msg,
                      lstcon_node_t *nd, lstcon_trans_stat_t *stat)
{
        srpc_mksn_reply_t  *mksn_rep;
        srpc_rmsn_reply_t  *rmsn_rep;
        srpc_debug_reply_t *dbg_rep;
        srpc_batch_reply_t *bat_rep;
        srpc_test_reply_t  *test_rep;
        srpc_stat_reply_t  *stat_rep;
        int                 rc = 0;

        switch (transop) {
        case LST_TRANS_SESNEW:
                mksn_rep = &msg->msg_body.mksn_reply;

                if (mksn_rep->mksn_status == 0) {
                        lstcon_sesop_stat_success(stat, 1);
                        /* session timeout on remote node */
                        nd->nd_timeout = mksn_rep->mksn_timeout;
                        return;
                }

                LASSERT (mksn_rep->mksn_status == EBUSY ||
                         mksn_rep->mksn_status == EINVAL);

                lstcon_sesop_stat_failure(stat, 1);
                rc = mksn_rep->mksn_status;
                break;

        case LST_TRANS_SESEND:
                rmsn_rep = &msg->msg_body.rmsn_reply;
                /* ESRCH is not an error for end session */
                if (rmsn_rep->rmsn_status == 0 ||
                    rmsn_rep->rmsn_status == ESRCH) {
                        lstcon_sesop_stat_success(stat, 1);
                        return;
                }

                LASSERT (rmsn_rep->rmsn_status == EBUSY ||
                         rmsn_rep->rmsn_status == EINVAL);

                lstcon_sesop_stat_failure(stat, 1);
                rc = rmsn_rep->rmsn_status;
                break;

        case LST_TRANS_SESQRY:
        case LST_TRANS_SESPING:
                dbg_rep = &msg->msg_body.dbg_reply;

                if (dbg_rep->dbg_status == ESRCH) {
                        lstcon_sesqry_stat_unknown(stat, 1);
                        return;
                } 

                LASSERT (dbg_rep->dbg_status == 0);

                if (lstcon_session_match(dbg_rep->dbg_sid))
                        lstcon_sesqry_stat_active(stat, 1);
                else
                        lstcon_sesqry_stat_busy(stat, 1);
                return;

        case LST_TRANS_TSBRUN:
        case LST_TRANS_TSBSTOP:
                bat_rep = &msg->msg_body.bat_reply;

                if (bat_rep->bar_status == 0) {
                        lstcon_tsbop_stat_success(stat, 1);
                        return;
                }

                if (bat_rep->bar_status == EPERM && 
                    transop == LST_TRANS_TSBSTOP) {
                        lstcon_tsbop_stat_success(stat, 1);
                        return;
                }

                lstcon_tsbop_stat_failure(stat, 1);
                rc = bat_rep->bar_status;
                break;

        case LST_TRANS_TSBCLIQRY:
        case LST_TRANS_TSBSRVQRY:
                bat_rep = &msg->msg_body.bat_reply;

                if (bat_rep->bar_active != 0) 
                        lstcon_tsbqry_stat_run(stat, 1);
                else
                        lstcon_tsbqry_stat_idle(stat, 1);

                if (bat_rep->bar_status == 0) 
                        return;

                lstcon_tsbqry_stat_failure(stat, 1);
                rc = bat_rep->bar_status;
                break;

        case LST_TRANS_TSBCLIADD:
        case LST_TRANS_TSBSRVADD:
                test_rep = &msg->msg_body.tes_reply;

                if (test_rep->tsr_status == 0) {
                        lstcon_tsbop_stat_success(stat, 1);
                        return;
                }

                lstcon_tsbop_stat_failure(stat, 1);
                rc = test_rep->tsr_status;
                break;

        case LST_TRANS_STATQRY:
                stat_rep = &msg->msg_body.stat_reply;

                if (stat_rep->str_status == 0) {
                        lstcon_statqry_stat_success(stat, 1);
                        return;
                }

                lstcon_statqry_stat_failure(stat, 1);
                rc = stat_rep->str_status;
                break;

        default:
                LBUG();
        }

        if (stat->trs_fwk_errno == 0)
                stat->trs_fwk_errno = rc;

        return;
}

int
lstcon_rpc_trans_ndlist(cfs_list_t *ndlist,
                        cfs_list_t *translist, int transop,
                        void *arg, lstcon_rpc_cond_func_t condition,
                        lstcon_rpc_trans_t **transpp)
{
        lstcon_rpc_trans_t *trans;
        lstcon_ndlink_t    *ndl;
        lstcon_node_t      *nd;
        lstcon_rpc_t       *rpc;
        int                 rc;

        /* Creating session RPG for list of nodes */

        rc = lstcon_rpc_trans_prep(translist, transop, &trans);
        if (rc != 0) {
                CERROR("Can't create transaction %d: %d\n", transop, rc);
                return rc;
        }

        cfs_list_for_each_entry_typed(ndl, ndlist, lstcon_ndlink_t, ndl_link) {
                rc = condition == NULL ? 1 :
                     condition(transop, ndl->ndl_node, arg);

                if (rc == 0)
                        continue;

                if (rc < 0) {
                        CDEBUG(D_NET, "Condition error while creating RPC "
                                      " for transaction %d: %d\n", transop, rc);
                        break;
                }

                nd = ndl->ndl_node;

                switch (transop) {
                case LST_TRANS_SESNEW:
                case LST_TRANS_SESEND:
                        rc = lstcon_sesrpc_prep(nd, transop, &rpc);
                        break;
                case LST_TRANS_SESQRY:
                case LST_TRANS_SESPING:
                        rc = lstcon_dbgrpc_prep(nd, &rpc);
                        break;
                case LST_TRANS_TSBCLIADD:
                case LST_TRANS_TSBSRVADD:
                        rc = lstcon_testrpc_prep(nd, transop,
                                                 (lstcon_test_t *)arg, &rpc);
                        break;
                case LST_TRANS_TSBRUN:
                case LST_TRANS_TSBSTOP:
                case LST_TRANS_TSBCLIQRY:
                case LST_TRANS_TSBSRVQRY:
                        rc = lstcon_batrpc_prep(nd, transop,
                                                (lstcon_tsb_hdr_t *)arg, &rpc);
                        break;
                case LST_TRANS_STATQRY:
                        rc = lstcon_statrpc_prep(nd, &rpc);
                        break;
                default:
                        rc = -EINVAL;
                        break;
                }

                if (rc != 0) {
                        CERROR("Failed to create RPC for transaction %s: %d\n",
                               lstcon_rpc_trans_name(transop), rc);
                        break;
                }
                                
                lstcon_rpc_trans_addreq(trans, rpc);
        }

        if (rc == 0) {
                *transpp = trans;
                return 0;
        }

        lstcon_rpc_trans_destroy(trans);

        return rc;
}

void
lstcon_rpc_pinger(void *arg)
{
        stt_timer_t        *ptimer = (stt_timer_t *)arg;
        lstcon_rpc_trans_t *trans;
        lstcon_rpc_t       *crpc;
        srpc_msg_t         *rep;
        srpc_debug_reqst_t *drq;
        lstcon_ndlink_t    *ndl;
        lstcon_node_t      *nd;
        time_t              intv;
        int                 count = 0;
        int                 rc;

        /* RPC pinger is a special case of transaction,
         * it's called by timer at 8 seconds interval.
         */
        cfs_mutex_lock(&console_session.ses_mutex);

        if (console_session.ses_shutdown || console_session.ses_expired) {
                cfs_mutex_unlock(&console_session.ses_mutex);
                return;
        }

        if (!console_session.ses_expired &&
            cfs_time_current_sec() - console_session.ses_laststamp >
            (time_t)console_session.ses_timeout)
                console_session.ses_expired = 1;

        trans = console_session.ses_ping;

        LASSERT (trans != NULL);

        cfs_list_for_each_entry_typed(ndl, &console_session.ses_ndl_list,
                                      lstcon_ndlink_t, ndl_link) {
                nd = ndl->ndl_node;

                if (console_session.ses_expired) {
                        /* idle console, end session on all nodes */
                        if (nd->nd_state != LST_NODE_ACTIVE)
                                continue;

                        rc = lstcon_sesrpc_prep(nd, LST_TRANS_SESEND, &crpc);
                        if (rc != 0) {
                                CERROR("Out of memory\n");
                                break;
                        }

                        lstcon_rpc_trans_addreq(trans, crpc);
                        lstcon_rpc_post(crpc);

                        continue;
                }

                crpc = &nd->nd_ping;

                if (crpc->crp_rpc != NULL) {
                        LASSERT (crpc->crp_trans == trans);
                        LASSERT (!cfs_list_empty(&crpc->crp_link));

                        cfs_spin_lock(&crpc->crp_rpc->crpc_lock);

                        LASSERT (crpc->crp_posted);

                        if (!crpc->crp_finished) {
                                /* in flight */
                                cfs_spin_unlock(&crpc->crp_rpc->crpc_lock);
                                continue;
                        }

                        cfs_spin_unlock(&crpc->crp_rpc->crpc_lock);

                        lstcon_rpc_get_reply(crpc, &rep);

                        cfs_list_del_init(&crpc->crp_link);
                
                        lstcon_rpc_put(crpc);
                }

                if (nd->nd_state != LST_NODE_ACTIVE)
                        continue;

                intv = cfs_duration_sec(cfs_time_sub(cfs_time_current(),
                                                     nd->nd_stamp));
                if (intv < (time_t)nd->nd_timeout / 2)
                        continue;

                rc = lstcon_rpc_init(nd, SRPC_SERVICE_DEBUG, 0, 0, crpc);
                if (rc != 0) {
                        CERROR("Out of memory\n");
                        break;
                }

                drq = &crpc->crp_rpc->crpc_reqstmsg.msg_body.dbg_reqst;

                drq->dbg_sid   = console_session.ses_id;
                drq->dbg_flags = 0;

                lstcon_rpc_trans_addreq(trans, crpc);
                lstcon_rpc_post(crpc);

                count ++;
        }

        if (console_session.ses_expired) {
                cfs_mutex_unlock(&console_session.ses_mutex);
                return;
        }

        CDEBUG(D_NET, "Ping %d nodes in session\n", count);

        ptimer->stt_expires = (cfs_time_t)(cfs_time_current_sec() + LST_PING_INTERVAL);
        stt_add_timer(ptimer);

        cfs_mutex_unlock(&console_session.ses_mutex);
}

int
lstcon_rpc_pinger_start(void)
{
        stt_timer_t    *ptimer;
        int             rc;

        LASSERT (cfs_list_empty(&console_session.ses_rpc_freelist));
        LASSERT (cfs_atomic_read(&console_session.ses_rpc_counter) == 0);

        rc = lstcon_rpc_trans_prep(NULL, LST_TRANS_SESPING,
                                   &console_session.ses_ping);
        if (rc != 0) {
                CERROR("Failed to create console pinger\n");
                return rc;
        }

        ptimer = &console_session.ses_ping_timer;
        ptimer->stt_expires = (cfs_time_t)(cfs_time_current_sec() + LST_PING_INTERVAL);

        stt_add_timer(ptimer);

        return 0;
}

void
lstcon_rpc_pinger_stop(void)
{
        LASSERT (console_session.ses_shutdown);

        stt_del_timer(&console_session.ses_ping_timer);

        lstcon_rpc_trans_abort(console_session.ses_ping, -ESHUTDOWN);
        lstcon_rpc_trans_stat(console_session.ses_ping, lstcon_trans_stat());
        lstcon_rpc_trans_destroy(console_session.ses_ping);

        memset(lstcon_trans_stat(), 0, sizeof(lstcon_trans_stat_t));

        console_session.ses_ping = NULL;
}

void
lstcon_rpc_cleanup_wait(void)
{
        lstcon_rpc_trans_t *trans;
        lstcon_rpc_t       *crpc;
        cfs_list_t         *pacer;
        cfs_list_t          zlist;

        /* Called with hold of global mutex */

        LASSERT (console_session.ses_shutdown);

        while (!cfs_list_empty(&console_session.ses_trans_list)) { 
                cfs_list_for_each(pacer, &console_session.ses_trans_list) {
                        trans = cfs_list_entry(pacer, lstcon_rpc_trans_t,
                                               tas_link);

                        CDEBUG(D_NET, "Session closed, wakeup transaction %s\n",
                               lstcon_rpc_trans_name(trans->tas_opc));

                        cfs_waitq_signal(&trans->tas_waitq);
                }

                cfs_mutex_unlock(&console_session.ses_mutex);

                CWARN("Session is shutting down, "
                      "waiting for termination of transactions\n");
                cfs_pause(cfs_time_seconds(1));

                cfs_mutex_lock(&console_session.ses_mutex);
        }

        cfs_spin_lock(&console_session.ses_rpc_lock);

        lst_wait_until((cfs_atomic_read(&console_session.ses_rpc_counter) == 0),
                       console_session.ses_rpc_lock,
                       "Network is not accessable or target is down, "
                       "waiting for %d console RPCs to being recycled\n",
                       cfs_atomic_read(&console_session.ses_rpc_counter));

        cfs_list_add(&zlist, &console_session.ses_rpc_freelist);
        cfs_list_del_init(&console_session.ses_rpc_freelist);

        cfs_spin_unlock(&console_session.ses_rpc_lock);

        while (!cfs_list_empty(&zlist)) {
                crpc = cfs_list_entry(zlist.next, lstcon_rpc_t, crp_link);

                cfs_list_del(&crpc->crp_link);
                LIBCFS_FREE(crpc, sizeof(lstcon_rpc_t));
        }
}

int
lstcon_rpc_module_init(void)
{
        CFS_INIT_LIST_HEAD(&console_session.ses_ping_timer.stt_list);
        console_session.ses_ping_timer.stt_func = lstcon_rpc_pinger;
        console_session.ses_ping_timer.stt_data = &console_session.ses_ping_timer;

        console_session.ses_ping = NULL;

        cfs_spin_lock_init(&console_session.ses_rpc_lock);
        cfs_atomic_set(&console_session.ses_rpc_counter, 0);
        CFS_INIT_LIST_HEAD(&console_session.ses_rpc_freelist);

        return 0;
}

void
lstcon_rpc_module_fini(void)
{
        LASSERT (cfs_list_empty(&console_session.ses_rpc_freelist));
        LASSERT (cfs_atomic_read(&console_session.ses_rpc_counter) == 0);
}

#endif
