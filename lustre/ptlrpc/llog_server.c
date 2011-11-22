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
 * lustre/ptlrpc/llog_server.c
 *
 * remote api for llog - server side
 *
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Mikhail Pershin <tappro@whamcloud.com>
 */

#define DEBUG_SUBSYSTEM S_LOG

#ifndef EXPORT_SYMTAB
#define EXPORT_SYMTAB
#endif

#ifndef __KERNEL__
#include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_log.h>
#include <lustre_net.h>

#if defined(__KERNEL__) && defined(LUSTRE_LOG_SERVER)
static int llog_origin_close(const struct lu_env *env, struct llog_handle *lgh)
{
        if (lgh->lgh_hdr != NULL && lgh->lgh_hdr->llh_flags & LLOG_F_IS_CAT)
                return llog_cat_close(env, lgh);
        else
                return llog_close(env, lgh);
}

/* Only open is supported, no new llog can be created remotely */
int llog_origin_handle_open(struct ptlrpc_request *req)
{
        struct obd_export    *exp = req->rq_export;
        struct obd_device    *obd = exp->exp_obd;
        struct llog_handle   *loghandle;
        struct llogd_body    *body;
        struct llog_logid    *logid = NULL;
        struct llog_ctxt     *ctxt;
        char                 *name = NULL;
        int                   rc;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        if (body->lgd_logid.lgl_oid > 0)
                logid = &body->lgd_logid;

        if (req_capsule_field_present(&req->rq_pill, &RMF_NAME, RCL_CLIENT)) {
                name = req_capsule_client_get(&req->rq_pill, &RMF_NAME);
                if (name == NULL)
                        RETURN(-EFAULT);
                CDEBUG(D_INFO, "%s: opening log %s\n", obd->obd_name, name);
        }

        ctxt = llog_get_context(obd, body->lgd_ctxt_idx);
        if (ctxt == NULL) {
                CDEBUG(D_WARNING, "%s: no ctxt. group=%p idx=%d name=%s\n",
                       obd->obd_name, &obd->obd_olg, body->lgd_ctxt_idx, name);
                RETURN(-ENODEV);
        }

        rc = llog_open(req->rq_svc_thread->t_env, ctxt, &loghandle, logid,
                       name, LLOG_OPEN_OLD);
        if (rc)
                GOTO(out_pop, rc);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                GOTO(out_close, rc = -ENOMEM);

        body = req_capsule_server_get(&req->rq_pill, &RMF_LLOGD_BODY);
        body->lgd_logid = loghandle->lgh_id;

out_close:
        llog_origin_close(req->rq_svc_thread->t_env, loghandle);
out_pop:
        llog_ctxt_put(ctxt);
        RETURN(rc);
}

int llog_origin_handle_destroy(struct ptlrpc_request *req)
{
        struct llogd_body *body;
        struct llog_logid *logid = NULL;
        struct llog_ctxt  *ctxt;
        int                rc;

        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        if (body->lgd_logid.lgl_oid > 0)
                logid = &body->lgd_logid;

        LASSERT(body->lgd_llh_flags == LLOG_F_IS_PLAIN);
        ctxt = llog_get_context(req->rq_export->exp_obd, body->lgd_ctxt_idx);
        if (ctxt == NULL)
                RETURN(-ENODEV);

        rc = req_capsule_server_pack(&req->rq_pill);
        /* erase only if no error and logid is valid */
        if (rc == 0)
                rc = llog_erase(req->rq_svc_thread->t_env, ctxt, logid, NULL);
        llog_ctxt_put(ctxt);
        return rc;
}

int llog_origin_handle_next_block(struct ptlrpc_request *req)
{
        struct llog_handle  *loghandle;
        struct llogd_body   *body,  *repbody;
        struct llog_ctxt    *ctxt;
        __u32                flags;
        void                *ptr;
        int                  rc;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        ctxt = llog_get_context(req->rq_export->exp_obd, body->lgd_ctxt_idx);
        if (ctxt == NULL)
                RETURN(-ENODEV);

        rc = llog_open(req->rq_svc_thread->t_env, ctxt, &loghandle,
                       &body->lgd_logid, NULL, LLOG_OPEN_OLD);
        if (rc)
                GOTO(out_pop, rc);

        flags = body->lgd_llh_flags;
        rc = llog_init_handle(req->rq_svc_thread->t_env, loghandle, flags,
                              NULL);
        if (rc)
                GOTO(out_close, rc);

        req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_SERVER,
                             LLOG_CHUNK_SIZE);
        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                GOTO(out_close, rc = -ENOMEM);

        repbody = req_capsule_server_get(&req->rq_pill, &RMF_LLOGD_BODY);
        *repbody = *body;

        ptr = req_capsule_server_get(&req->rq_pill, &RMF_EADATA);

        rc = llog_next_block(req->rq_svc_thread->t_env,
                             loghandle, &repbody->lgd_saved_index,
                             repbody->lgd_index, &repbody->lgd_cur_offset,
                             ptr, LLOG_CHUNK_SIZE);
out_close:
        llog_origin_close(req->rq_svc_thread->t_env, loghandle);
out_pop:
        llog_ctxt_put(ctxt);
        return rc;
}

int llog_origin_handle_prev_block(struct ptlrpc_request *req)
{
        struct llog_handle   *loghandle;
        struct llogd_body    *body, *repbody;
        struct llog_ctxt     *ctxt;
        __u32                 flags;
        void                 *ptr;
        int                   rc;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        ctxt = llog_get_context(req->rq_export->exp_obd, body->lgd_ctxt_idx);
        if (ctxt == NULL)
                RETURN(-ENODEV);

        rc = llog_open(req->rq_svc_thread->t_env, ctxt, &loghandle,
                         &body->lgd_logid, NULL, LLOG_OPEN_OLD);
        if (rc)
                GOTO(out_pop, rc);

        flags = body->lgd_llh_flags;
        rc = llog_init_handle(req->rq_svc_thread->t_env, loghandle, flags,
                              NULL);
        if (rc)
                GOTO(out_close, rc);

        req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_SERVER,
                             LLOG_CHUNK_SIZE);
        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                GOTO(out_close, rc = -ENOMEM);

        repbody = req_capsule_server_get(&req->rq_pill, &RMF_LLOGD_BODY);
        *repbody = *body;

        ptr = req_capsule_server_get(&req->rq_pill, &RMF_EADATA);
        rc = llog_prev_block(req->rq_svc_thread->t_env, loghandle,
                             body->lgd_index, ptr, LLOG_CHUNK_SIZE);
out_close:
        llog_origin_close(req->rq_svc_thread->t_env, loghandle);
out_pop:
        llog_ctxt_put(ctxt);
        return rc;
}

int llog_origin_handle_read_header(struct ptlrpc_request *req)
{
        struct obd_export    *exp = req->rq_export;
        struct obd_device    *obd = exp->exp_obd;
        struct llog_handle   *loghandle;
        struct llogd_body    *body;
        struct llog_log_hdr  *hdr;
        struct llog_ctxt     *ctxt;
        __u32                 flags;
        int                   rc;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        ctxt = llog_get_context(obd, body->lgd_ctxt_idx);
        if (ctxt == NULL)
                RETURN(-ENODEV);

        rc = llog_open(req->rq_svc_thread->t_env, ctxt, &loghandle,
                       &body->lgd_logid, NULL, LLOG_OPEN_OLD);
        if (rc)
                GOTO(out_pop, rc);

        /*
         * llog_init_handle() reads the llog header
         */
        flags = body->lgd_llh_flags;
        rc = llog_init_handle(req->rq_svc_thread->t_env, loghandle, flags,
                              NULL);
        if (rc)
                GOTO(out_close, rc);
        flags = loghandle->lgh_hdr->llh_flags;

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                GOTO(out_close, rc = -ENOMEM);

        hdr = req_capsule_server_get(&req->rq_pill, &RMF_LLOG_LOG_HDR);
        *hdr = *loghandle->lgh_hdr;
        GOTO(out_close, rc);
out_close:
        llog_origin_close(req->rq_svc_thread->t_env, loghandle);
out_pop:
        llog_ctxt_put(ctxt);
        return rc;
}

int llog_origin_handle_close(struct ptlrpc_request *req)
{
        ENTRY;
        /* Nothing to do */
        RETURN(0);
}

int llog_origin_handle_cancel(struct ptlrpc_request *req)
{
        struct llog_cookie *logcookies;
        struct llog_ctxt *ctxt = NULL;
        struct llog_handle *cathandle;
        int num_cookies, rc = 0;
        ENTRY;

        logcookies = req_capsule_client_get(&req->rq_pill, &RMF_LOGCOOKIES);
        num_cookies = req_capsule_get_size(&req->rq_pill, &RMF_LOGCOOKIES,
                                           RCL_CLIENT) / sizeof(*logcookies);
        if (logcookies == NULL || num_cookies == 0) {
                DEBUG_REQ(D_HA, req, "No llog cookies sent");
                RETURN(-EFAULT);
        }

        ctxt = llog_get_context(req->rq_export->exp_obd, logcookies->lgc_subsys);
        if (ctxt == NULL)
                RETURN(-ENODEV);

        cathandle = ctxt->loc_handle;
        LASSERT(cathandle != NULL);

        rc = llog_cat_cancel_records(req->rq_svc_thread->t_env, cathandle,
                                     num_cookies, logcookies);
        /*
         * Do not raise -ENOENT errors for resent rpcs. This rec already
         * might be killed.
         */
        if (rc == -ENOENT &&
            lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT) {
                /*
                 * Do not change this message, reply-single.sh test_59b
                 * expects to find this in log.
                 */
                CDEBUG(D_RPCTRACE, "RESENT cancel req %p - ignored\n", req);
                rc = 0;
        } else if (rc == 0) {
                CDEBUG(D_RPCTRACE, "Canceled %d llog-records\n", num_cookies);
        }

        llog_ctxt_put(ctxt);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_origin_handle_cancel);

/* Obsoleted lfs catinfo command. It is not supported now. */
int llog_catinfo(struct ptlrpc_request *req)
{
        return -EOPNOTSUPP;
}

#else /* !__KERNEL__ */
int llog_origin_handle_open(struct ptlrpc_request *req)
{
        LBUG();
        return 0;
}

int llog_origin_handle_destroy(struct ptlrpc_request *req)
{
        LBUG();
        return 0;
}

int llog_origin_handle_next_block(struct ptlrpc_request *req)
{
        LBUG();
        return 0;
}
int llog_origin_handle_prev_block(struct ptlrpc_request *req)
{
        LBUG();
        return 0;
}
int llog_origin_handle_read_header(struct ptlrpc_request *req)
{
        LBUG();
        return 0;
}
int llog_origin_handle_close(struct ptlrpc_request *req)
{
        LBUG();
        return 0;
}
int llog_origin_handle_cancel(struct ptlrpc_request *req)
{
        LBUG();
        return 0;
}
#endif
