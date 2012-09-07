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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, Whamcloud, Inc.
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
 */

#define DEBUG_SUBSYSTEM S_LOG

#ifndef __KERNEL__
#include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_log.h>
#include <lustre_net.h>
#include <libcfs/list.h>
#include <lustre_fsfilt.h>

#if defined(__KERNEL__) && defined(LUSTRE_LOG_SERVER)

int llog_origin_handle_create(struct ptlrpc_request *req)
{
        struct obd_export    *exp = req->rq_export;
        struct obd_device    *obd = exp->exp_obd;
        struct obd_device    *disk_obd;
        struct llog_handle   *loghandle;
        struct llogd_body    *body;
        struct lvfs_run_ctxt  saved;
        struct llog_logid    *logid = NULL;
        struct llog_ctxt     *ctxt;
        char                 *name = NULL;
        int                   rc, rc2;
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
        disk_obd = ctxt->loc_exp->exp_obd;
        push_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);

        rc = llog_create(ctxt, &loghandle, logid, name);
        if (rc)
                GOTO(out_pop, rc);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                GOTO(out_close, rc = -ENOMEM);

        body = req_capsule_server_get(&req->rq_pill, &RMF_LLOGD_BODY);
        body->lgd_logid = loghandle->lgh_id;

        GOTO(out_close, rc);
out_close:
        rc2 = llog_close(loghandle);
        if (!rc)
                rc = rc2;
out_pop:
        pop_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);
        llog_ctxt_put(ctxt);
        return rc;
}
EXPORT_SYMBOL(llog_origin_handle_create);

int llog_origin_handle_destroy(struct ptlrpc_request *req)
{
        struct obd_export    *exp = req->rq_export;
        struct obd_device    *obd = exp->exp_obd;
        struct obd_device    *disk_obd;
        struct llog_handle   *loghandle;
        struct llogd_body    *body;
        struct lvfs_run_ctxt  saved;
        struct llog_logid    *logid = NULL;
        struct llog_ctxt     *ctxt;
        int                   rc;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        if (body->lgd_logid.lgl_oid > 0)
                logid = &body->lgd_logid;

        ctxt = llog_get_context(obd, body->lgd_ctxt_idx);
        if (ctxt == NULL)
                RETURN(-ENODEV);

        disk_obd = ctxt->loc_exp->exp_obd;
        push_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);

        rc = llog_create(ctxt, &loghandle, logid, NULL);
        if (rc)
                GOTO(out_pop, rc);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                GOTO(out_close, rc = -ENOMEM);

        body = req_capsule_server_get(&req->rq_pill, &RMF_LLOGD_BODY);
        body->lgd_logid = loghandle->lgh_id;
        rc = llog_init_handle(loghandle, LLOG_F_IS_PLAIN, NULL);
        if (rc)
                GOTO(out_close, rc);
        rc = llog_destroy(req->rq_svc_thread->t_env, loghandle);
        if (rc)
                GOTO(out_close, rc);
        llog_free_handle(loghandle);
        GOTO(out_close, rc);
out_close:
        if (rc)
                llog_close(loghandle);
out_pop:
        pop_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);
        llog_ctxt_put(ctxt);
        return rc;
}
EXPORT_SYMBOL(llog_origin_handle_destroy);

int llog_origin_handle_next_block(struct ptlrpc_request *req)
{
        struct obd_export   *exp = req->rq_export;
        struct obd_device   *obd = exp->exp_obd;
        struct obd_device   *disk_obd;
        struct llog_handle  *loghandle;
        struct llogd_body   *body;
        struct llogd_body   *repbody;
        struct lvfs_run_ctxt saved;
        struct llog_ctxt    *ctxt;
        __u32                flags;
        __u8                *buf;
        void                *ptr;
        int                  rc, rc2;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        OBD_ALLOC(buf, LLOG_CHUNK_SIZE);
        if (!buf)
                RETURN(-ENOMEM);

        ctxt = llog_get_context(obd, body->lgd_ctxt_idx);
        if (ctxt == NULL)
                GOTO(out_free, rc = -ENODEV);
        disk_obd = ctxt->loc_exp->exp_obd;
        push_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);

        rc = llog_create(ctxt, &loghandle, &body->lgd_logid, NULL);
        if (rc)
                GOTO(out_pop, rc);

        flags = body->lgd_llh_flags;
        rc = llog_init_handle(loghandle, flags, NULL);
        if (rc)
                GOTO(out_close, rc);

        memset(buf, 0, LLOG_CHUNK_SIZE);
        rc = llog_next_block(req->rq_svc_thread->t_env,
                             loghandle, &body->lgd_saved_index,
                             body->lgd_index,
                             &body->lgd_cur_offset, buf, LLOG_CHUNK_SIZE);
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
        memcpy(ptr, buf, LLOG_CHUNK_SIZE);
        GOTO(out_close, rc);
out_close:
        rc2 = llog_close(loghandle);
        if (!rc)
                rc = rc2;
out_pop:
        pop_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);
        llog_ctxt_put(ctxt);
out_free:
        OBD_FREE(buf, LLOG_CHUNK_SIZE);
        return rc;
}
EXPORT_SYMBOL(llog_origin_handle_next_block);

int llog_origin_handle_prev_block(struct ptlrpc_request *req)
{
        struct obd_export    *exp = req->rq_export;
        struct obd_device    *obd = exp->exp_obd;
        struct llog_handle   *loghandle;
        struct llogd_body    *body;
        struct llogd_body    *repbody;
        struct obd_device    *disk_obd;
        struct lvfs_run_ctxt  saved;
        struct llog_ctxt     *ctxt;
        __u32                 flags;
        __u8                 *buf;
        void                 *ptr;
        int                   rc, rc2;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        OBD_ALLOC(buf, LLOG_CHUNK_SIZE);
        if (!buf)
                RETURN(-ENOMEM);

        ctxt = llog_get_context(obd, body->lgd_ctxt_idx);
        if (ctxt == NULL)
                GOTO(out_free, rc = -ENODEV);

        disk_obd = ctxt->loc_exp->exp_obd;
        push_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);

        rc = llog_create(ctxt, &loghandle, &body->lgd_logid, NULL);
        if (rc)
                GOTO(out_pop, rc);

        flags = body->lgd_llh_flags;
        rc = llog_init_handle(loghandle, flags, NULL);
        if (rc)
                GOTO(out_close, rc);

        memset(buf, 0, LLOG_CHUNK_SIZE);
        rc = llog_prev_block(req->rq_svc_thread->t_env, loghandle,
                             body->lgd_index, buf, LLOG_CHUNK_SIZE);
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
        memcpy(ptr, buf, LLOG_CHUNK_SIZE);
        GOTO(out_close, rc);
out_close:
        rc2 = llog_close(loghandle);
        if (!rc)
                rc = rc2;

out_pop:
        pop_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);
        llog_ctxt_put(ctxt);
out_free:
        OBD_FREE(buf, LLOG_CHUNK_SIZE);
        return rc;
}
EXPORT_SYMBOL(llog_origin_handle_prev_block);

int llog_origin_handle_read_header(struct ptlrpc_request *req)
{
        struct obd_export    *exp = req->rq_export;
        struct obd_device    *obd = exp->exp_obd;
        struct obd_device    *disk_obd;
        struct llog_handle   *loghandle;
        struct llogd_body    *body;
        struct llog_log_hdr  *hdr;
        struct lvfs_run_ctxt  saved;
        struct llog_ctxt     *ctxt;
        __u32                 flags;
        int                   rc, rc2;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_LLOGD_BODY);
        if (body == NULL)
                RETURN(-EFAULT);

        ctxt = llog_get_context(obd, body->lgd_ctxt_idx);
        if (ctxt == NULL)
                RETURN(-ENODEV);

        disk_obd = ctxt->loc_exp->exp_obd;
        push_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);

        rc = llog_create(ctxt, &loghandle, &body->lgd_logid, NULL);
        if (rc)
                GOTO(out_pop, rc);

        /*
         * llog_init_handle() reads the llog header
         */
        flags = body->lgd_llh_flags;
        rc = llog_init_handle(loghandle, flags, NULL);
        if (rc)
                GOTO(out_close, rc);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                GOTO(out_close, rc = -ENOMEM);

        hdr = req_capsule_server_get(&req->rq_pill, &RMF_LLOG_LOG_HDR);
        *hdr = *loghandle->lgh_hdr;
        GOTO(out_close, rc);
out_close:
        rc2 = llog_close(loghandle);
        if (!rc)
                rc = rc2;
out_pop:
        pop_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);
        llog_ctxt_put(ctxt);
        return rc;
}
EXPORT_SYMBOL(llog_origin_handle_read_header);

int llog_origin_handle_close(struct ptlrpc_request *req)
{
        ENTRY;
        /* Nothing to do */
        RETURN(0);
}
EXPORT_SYMBOL(llog_origin_handle_close);

int llog_origin_handle_cancel(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        int num_cookies, rc = 0, err, i, failed = 0;
        struct obd_device *disk_obd;
        struct llog_cookie *logcookies;
        struct llog_ctxt *ctxt = NULL;
        struct lvfs_run_ctxt saved;
        struct llog_handle *cathandle;
        struct inode *inode;
        void *handle;
        ENTRY;

        logcookies = req_capsule_client_get(&req->rq_pill, &RMF_LOGCOOKIES);
        num_cookies = req_capsule_get_size(&req->rq_pill, &RMF_LOGCOOKIES,
                                           RCL_CLIENT) / sizeof(*logcookies);
        if (logcookies == NULL || num_cookies == 0) {
                DEBUG_REQ(D_HA, req, "No llog cookies sent");
                RETURN(-EFAULT);
        }

        ctxt = llog_get_context(obd, logcookies->lgc_subsys);
        if (ctxt == NULL)
                RETURN(-ENODEV);

        disk_obd = ctxt->loc_exp->exp_obd;
        push_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);
        for (i = 0; i < num_cookies; i++, logcookies++) {
                cathandle = ctxt->loc_handle;
                LASSERT(cathandle != NULL);
                inode = cathandle->lgh_file->f_dentry->d_inode;

                handle = fsfilt_start_log(disk_obd, inode,
                                          FSFILT_OP_CANCEL_UNLINK, NULL, 1);
                if (IS_ERR(handle)) {
                        CERROR("fsfilt_start_log() failed: %ld\n",
                               PTR_ERR(handle));
                        GOTO(pop_ctxt, rc = PTR_ERR(handle));
                }

                rc = llog_cat_cancel_records(cathandle, 1, logcookies);

                /*
                 * Do not raise -ENOENT errors for resent rpcs. This rec already
                 * might be killed.
                 */
                if (rc == -ENOENT &&
                    (lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT)) {
                        /*
                         * Do not change this message, reply-single.sh test_59b
                         * expects to find this in log.
                         */
                        CDEBUG(D_RPCTRACE, "RESENT cancel req %p - ignored\n",
                               req);
                        rc = 0;
                } else if (rc == 0) {
                        CDEBUG(D_RPCTRACE, "Canceled %d llog-records\n",
                               num_cookies);
                }

                err = fsfilt_commit(disk_obd, inode, handle, 0);
                if (err) {
                        CERROR("Error committing transaction: %d\n", err);
                        if (!rc)
                                rc = err;
                        failed++;
                        GOTO(pop_ctxt, rc);
                } else if (rc)
                        failed++;
        }
        GOTO(pop_ctxt, rc);
pop_ctxt:
        pop_ctxt(&saved, &disk_obd->obd_lvfs_ctxt, NULL);
        if (rc)
                CERROR("Cancel %d of %d llog-records failed: %d\n",
                       failed, num_cookies, rc);

        llog_ctxt_put(ctxt);
        return rc;
}
EXPORT_SYMBOL(llog_origin_handle_cancel);

#else /* !__KERNEL__ */
int llog_origin_handle_create(struct ptlrpc_request *req)
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
