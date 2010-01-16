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
 * lustre/ptlrpc/llog_net.c
 *
 * OST<->MDS recovery logging infrastructure.
 *
 * Invariants in implementation:
 * - we do not share logs among different OST<->MDS connections, so that
 *   if an OST or MDS fails it need only look at log(s) relevant to itself
 *
 * Author: Andreas Dilger <adilger@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_LOG

#ifndef EXPORT_SYMTAB
#define EXPORT_SYMTAB
#endif

#ifdef __KERNEL__
#include <libcfs/libcfs.h>
#else
#include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_log.h>
#include <libcfs/list.h>
#include <lvfs.h>
#include <lustre_fsfilt.h>

#ifdef __KERNEL__
int llog_origin_connect(struct llog_ctxt *ctxt,
                        struct llog_logid *logid, struct llog_gen *gen,
                        struct obd_uuid *uuid)
{
        struct llog_gen_rec *lgr;
        struct obd_import *imp;
        struct ptlrpc_request *request;
        struct llogd_conn_body *req_body;
        int size[2] = { sizeof(struct ptlrpc_body),
                        sizeof(struct llogd_conn_body) };
        struct inode *inode;
        void *handle;
        int rc, rc1;
        ENTRY;

        LASSERT(ctxt != NULL);
        LASSERT(ctxt->loc_handle != NULL);
        LASSERT(ctxt->loc_handle->lgh_file != NULL);
        LASSERT(ctxt->loc_handle->lgh_file->f_dentry != NULL);
        inode = ctxt->loc_handle->lgh_file->f_dentry->d_inode;

        if (list_empty(&ctxt->loc_handle->u.chd.chd_head)) {
                CDEBUG(D_HA, "there is no record related to ctxt %p\n", ctxt);
                RETURN(0);
        }

        /* FIXME what value for gen->conn_cnt */
        LLOG_GEN_INC(ctxt->loc_gen);

        /* first add llog_gen_rec */
        OBD_ALLOC(lgr, sizeof(*lgr));
        if (!lgr)
                RETURN(-ENOMEM);
        lgr->lgr_hdr.lrh_len = lgr->lgr_tail.lrt_len = sizeof(*lgr);
        lgr->lgr_hdr.lrh_type = LLOG_GEN_REC;

        handle = fsfilt_start_log(ctxt->loc_exp->exp_obd, inode,
                                  FSFILT_OP_CANCEL_UNLINK, NULL, 1);

        if (IS_ERR(handle)) {
                CERROR("fsfilt_start failed: %ld\n", PTR_ERR(handle));
                OBD_FREE(lgr, sizeof(*lgr));
                rc = PTR_ERR(handle);
                RETURN(rc);
        }
        lgr->lgr_gen = ctxt->loc_gen;
        rc = llog_add(ctxt, &lgr->lgr_hdr, NULL, NULL, 1);
        OBD_FREE(lgr, sizeof(*lgr));

        rc1 = fsfilt_commit(ctxt->loc_exp->exp_obd, inode, handle, 0);
        if (rc != 1 || rc1 != 0) {
                rc = (rc != 1) ? rc : rc1;
                RETURN(rc);
        }

        LASSERT(ctxt->loc_imp);
        imp = ctxt->loc_imp;

        request = ptlrpc_prep_req(imp, LUSTRE_LOG_VERSION,
                                  LLOG_ORIGIN_CONNECT, 2, size, NULL);
        if (!request)
                RETURN(-ENOMEM);

        req_body = lustre_msg_buf(request->rq_reqmsg, REQ_REC_OFF,
                                  sizeof(*req_body));

        req_body->lgdc_gen = ctxt->loc_gen;
        req_body->lgdc_logid = ctxt->loc_handle->lgh_id;
        req_body->lgdc_ctxt_idx = ctxt->loc_idx + 1;
        ptlrpc_req_set_repsize(request, 1, NULL);

        rc = ptlrpc_queue_wait(request);
        ptlrpc_req_finished(request);

        RETURN(rc);
}
EXPORT_SYMBOL(llog_origin_connect);

int llog_handle_connect(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct llogd_conn_body *req_body;
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;

        req_body = lustre_msg_buf(req->rq_reqmsg, REQ_REC_OFF,
                                  sizeof(*req_body));

        ctxt = llog_get_context(obd, req_body->lgdc_ctxt_idx);
        rc = llog_connect(ctxt, &req_body->lgdc_logid,
                          &req_body->lgdc_gen, NULL);

        llog_ctxt_put(ctxt);
        if (rc != 0)
                CERROR("failed at llog_relp_connect\n");

        RETURN(rc);
}
EXPORT_SYMBOL(llog_handle_connect);

int llog_receptor_accept(struct llog_ctxt *ctxt, struct obd_import *imp)
{
        ENTRY;
        LASSERT(ctxt);
        mutex_down(&ctxt->loc_sem);
        if (ctxt->loc_imp != imp) {
                if (ctxt->loc_imp) {
                        CDEBUG(D_HA, "changing the import %p - %p\n",
                               ctxt->loc_imp, imp);
                        class_import_put(ctxt->loc_imp);
                }
                ctxt->loc_imp = class_import_get(imp);
        }
        mutex_up(&ctxt->loc_sem);
        RETURN(0);
}
EXPORT_SYMBOL(llog_receptor_accept);

#else /* !__KERNEL__ */

int llog_origin_connect(struct llog_ctxt *ctxt,
                        struct llog_logid *logid, struct llog_gen *gen,
                        struct obd_uuid *uuid)
{
        return 0;
}
#endif

int llog_initiator_connect(struct llog_ctxt *ctxt)
{
        struct obd_import *new_imp;
        ENTRY;
        LASSERT(ctxt);
        new_imp = ctxt->loc_obd->u.cli.cl_import;
        mutex_down(&ctxt->loc_sem);
        if (ctxt->loc_imp != new_imp) {
                if (ctxt->loc_imp)
                        class_import_put(ctxt->loc_imp);
                ctxt->loc_imp = class_import_get(new_imp);
        }
        mutex_up(&ctxt->loc_sem);
        RETURN(0);
}
EXPORT_SYMBOL(llog_initiator_connect);
