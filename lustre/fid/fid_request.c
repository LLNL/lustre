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
 * lustre/fid/fid_request.c
 *
 * Lustre Sequence Manager
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_FID

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
# include <linux/module.h>
#else /* __KERNEL__ */
# include <liblustre.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <dt_object.h>
#include <md_object.h>
#include <obd_support.h>
#include <lustre_req_layout.h>
#include <lustre_fid.h>
/* mdc RPC locks */
#include <lustre_mdc.h>
#include "fid_internal.h"

static int seq_client_rpc(struct lu_client_seq *seq,
                          struct lu_seq_range *output, __u32 opc,
                          const char *opcname)
{
        struct obd_export     *exp = seq->lcs_exp;
        struct ptlrpc_request *req;
        struct lu_seq_range   *out, *in;
        __u32                 *op;
	unsigned int           debug_mask;
        int                    rc;
        ENTRY;

        req = ptlrpc_request_alloc_pack(class_exp2cliimp(exp), &RQF_SEQ_QUERY,
                                        LUSTRE_MDS_VERSION, SEQ_QUERY);
        if (req == NULL)
                RETURN(-ENOMEM);

        /* Init operation code */
        op = req_capsule_client_get(&req->rq_pill, &RMF_SEQ_OPC);
        *op = opc;

        /* Zero out input range, this is not recovery yet. */
        in = req_capsule_client_get(&req->rq_pill, &RMF_SEQ_RANGE);
        range_init(in);

        ptlrpc_request_set_replen(req);

       if (seq->lcs_type == LUSTRE_SEQ_METADATA) {
                req->rq_request_portal = SEQ_METADATA_PORTAL;
                in->lsr_flags = LU_SEQ_RANGE_MDT;
        } else {
                LASSERTF(seq->lcs_type == LUSTRE_SEQ_DATA,
                         "unknown lcs_type %u\n", seq->lcs_type);
                req->rq_request_portal = SEQ_DATA_PORTAL;
                in->lsr_flags = LU_SEQ_RANGE_OST;
        }

        if (opc == SEQ_ALLOC_SUPER) {
                /* Update index field of *in, it is required for
                 * FLD update on super sequence allocator node. */
                in->lsr_index = seq->lcs_space.lsr_index;
                req->rq_request_portal = SEQ_CONTROLLER_PORTAL;
		debug_mask = D_WARNING;
        } else {
		debug_mask = D_INFO;
                LASSERTF(opc == SEQ_ALLOC_META,
                         "unknown opcode %u\n, opc", opc);
        }

        ptlrpc_at_set_req_timeout(req);

        mdc_get_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);
        rc = ptlrpc_queue_wait(req);
        mdc_put_rpc_lock(exp->exp_obd->u.cli.cl_rpc_lock, NULL);

        if (rc)
                GOTO(out_req, rc);

        out = req_capsule_server_get(&req->rq_pill, &RMF_SEQ_RANGE);
        *output = *out;

        if (!range_is_sane(output)) {
                CERROR("%s: Invalid range received from server: "
                       DRANGE"\n", seq->lcs_name, PRANGE(output));
                GOTO(out_req, rc = -EINVAL);
        }

        if (range_is_exhausted(output)) {
                CERROR("%s: Range received from server is exhausted: "
                       DRANGE"]\n", seq->lcs_name, PRANGE(output));
                GOTO(out_req, rc = -EINVAL);
        }

	CDEBUG_LIMIT(debug_mask, "%s: Allocated %s-sequence "DRANGE"]\n",
			seq->lcs_name, opcname, PRANGE(output));

        EXIT;
out_req:
        ptlrpc_req_finished(req);
        return rc;
}

/* Request sequence-controller node to allocate new super-sequence. */
int seq_client_alloc_super(struct lu_client_seq *seq,
                           const struct lu_env *env)
{
        int rc;
        ENTRY;

        cfs_down(&seq->lcs_sem);

#ifdef __KERNEL__
        if (seq->lcs_srv) {
                LASSERT(env != NULL);
                rc = seq_server_alloc_super(seq->lcs_srv, &seq->lcs_space,
                                            env);
        } else {
#endif
                rc = seq_client_rpc(seq, &seq->lcs_space,
                                    SEQ_ALLOC_SUPER, "super");
#ifdef __KERNEL__
        }
#endif
        cfs_up(&seq->lcs_sem);
        RETURN(rc);
}

/* Request sequence-controller node to allocate new meta-sequence. */
static int seq_client_alloc_meta(struct lu_client_seq *seq,
                                 const struct lu_env *env)
{
        int rc;
        ENTRY;

#ifdef __KERNEL__
        if (seq->lcs_srv) {
                LASSERT(env != NULL);
                rc = seq_server_alloc_meta(seq->lcs_srv, &seq->lcs_space, env);
        } else {
#endif
                rc = seq_client_rpc(seq, &seq->lcs_space,
                                    SEQ_ALLOC_META, "meta");
#ifdef __KERNEL__
        }
#endif
        RETURN(rc);
}

/* Allocate new sequence for client. */
static int seq_client_alloc_seq(struct lu_client_seq *seq, seqno_t *seqnr)
{
        int rc;
        ENTRY;

        LASSERT(range_is_sane(&seq->lcs_space));

        if (range_is_exhausted(&seq->lcs_space)) {
                rc = seq_client_alloc_meta(seq, NULL);
                if (rc) {
                        CERROR("%s: Can't allocate new meta-sequence, "
                               "rc %d\n", seq->lcs_name, rc);
                        RETURN(rc);
                } else {
                        CDEBUG(D_INFO, "%s: New range - "DRANGE"\n",
                               seq->lcs_name, PRANGE(&seq->lcs_space));
                }
        } else {
                rc = 0;
        }

        LASSERT(!range_is_exhausted(&seq->lcs_space));
        *seqnr = seq->lcs_space.lsr_start;
        seq->lcs_space.lsr_start += 1;

        CDEBUG(D_INFO, "%s: Allocated sequence ["LPX64"]\n", seq->lcs_name,
               *seqnr);

        RETURN(rc);
}

static int seq_fid_alloc_prep(struct lu_client_seq *seq,
                              cfs_waitlink_t *link)
{
        if (seq->lcs_update) {
                cfs_waitq_add(&seq->lcs_waitq, link);
                cfs_set_current_state(CFS_TASK_UNINT);
                cfs_up(&seq->lcs_sem);

                cfs_waitq_wait(link, CFS_TASK_UNINT);

                cfs_down(&seq->lcs_sem);
                cfs_waitq_del(&seq->lcs_waitq, link);
                cfs_set_current_state(CFS_TASK_RUNNING);
                return -EAGAIN;
        }
        ++seq->lcs_update;
        cfs_up(&seq->lcs_sem);
        return 0;
}

static void seq_fid_alloc_fini(struct lu_client_seq *seq)
{
        LASSERT(seq->lcs_update == 1);
        cfs_down(&seq->lcs_sem);
        --seq->lcs_update;
        cfs_waitq_signal(&seq->lcs_waitq);
}

/* Allocate new fid on passed client @seq and save it to @fid. */
int seq_client_alloc_fid(struct lu_client_seq *seq, struct lu_fid *fid)
{
        cfs_waitlink_t link;
        int rc;
        ENTRY;

        LASSERT(seq != NULL);
        LASSERT(fid != NULL);

        cfs_waitlink_init(&link);
        cfs_down(&seq->lcs_sem);

        while (1) {
                seqno_t seqnr;

                if (!fid_is_zero(&seq->lcs_fid) &&
                    fid_oid(&seq->lcs_fid) < seq->lcs_width) {
                        /* Just bump last allocated fid and return to caller. */
                        seq->lcs_fid.f_oid += 1;
                        rc = 0;
                        break;
                }

                rc = seq_fid_alloc_prep(seq, &link);
                if (rc)
                        continue;

                rc = seq_client_alloc_seq(seq, &seqnr);
                if (rc) {
                        CERROR("%s: Can't allocate new sequence, "
                               "rc %d\n", seq->lcs_name, rc);
                        seq_fid_alloc_fini(seq);
                        cfs_up(&seq->lcs_sem);
                        RETURN(rc);
                }

                CDEBUG(D_INFO, "%s: Switch to sequence "
                       "[0x%16.16"LPF64"x]\n", seq->lcs_name, seqnr);

                seq->lcs_fid.f_oid = LUSTRE_FID_INIT_OID;
                seq->lcs_fid.f_seq = seqnr;
                seq->lcs_fid.f_ver = 0;

                /*
                 * Inform caller that sequence switch is performed to allow it
                 * to setup FLD for it.
                 */
                rc = 1;

                seq_fid_alloc_fini(seq);
                break;
        }

        *fid = seq->lcs_fid;
        cfs_up(&seq->lcs_sem);

        CDEBUG(D_INFO, "%s: Allocated FID "DFID"\n", seq->lcs_name,  PFID(fid));
        RETURN(rc);
}
EXPORT_SYMBOL(seq_client_alloc_fid);

/*
 * Finish the current sequence due to disconnect.
 * See mdc_import_event()
 */
void seq_client_flush(struct lu_client_seq *seq)
{
        cfs_waitlink_t link;

        LASSERT(seq != NULL);
        cfs_waitlink_init(&link);
        cfs_down(&seq->lcs_sem);

        while (seq->lcs_update) {
                cfs_waitq_add(&seq->lcs_waitq, &link);
                cfs_set_current_state(CFS_TASK_UNINT);
                cfs_up(&seq->lcs_sem);

                cfs_waitq_wait(&link, CFS_TASK_UNINT);

                cfs_down(&seq->lcs_sem);
                cfs_waitq_del(&seq->lcs_waitq, &link);
                cfs_set_current_state(CFS_TASK_RUNNING);
        }

        fid_zero(&seq->lcs_fid);
        /**
         * this id shld not be used for seq range allocation.
         * set to -1 for dgb check.
         */

        seq->lcs_space.lsr_index = -1;

        range_init(&seq->lcs_space);
        cfs_up(&seq->lcs_sem);
}
EXPORT_SYMBOL(seq_client_flush);

static void seq_client_proc_fini(struct lu_client_seq *seq);

#ifdef LPROCFS
static int seq_client_proc_init(struct lu_client_seq *seq)
{
        int rc;
        ENTRY;

        seq->lcs_proc_dir = lprocfs_register(seq->lcs_name,
                                             seq_type_proc_dir,
                                             NULL, NULL);

        if (IS_ERR(seq->lcs_proc_dir)) {
                CERROR("%s: LProcFS failed in seq-init\n",
                       seq->lcs_name);
                rc = PTR_ERR(seq->lcs_proc_dir);
                RETURN(rc);
        }

        rc = lprocfs_add_vars(seq->lcs_proc_dir,
                              seq_client_proc_list, seq);
        if (rc) {
                CERROR("%s: Can't init sequence manager "
                       "proc, rc %d\n", seq->lcs_name, rc);
                GOTO(out_cleanup, rc);
        }

        RETURN(0);

out_cleanup:
        seq_client_proc_fini(seq);
        return rc;
}

static void seq_client_proc_fini(struct lu_client_seq *seq)
{
        ENTRY;
        if (seq->lcs_proc_dir) {
                if (!IS_ERR(seq->lcs_proc_dir))
                        lprocfs_remove(&seq->lcs_proc_dir);
                seq->lcs_proc_dir = NULL;
        }
        EXIT;
}
#else
static int seq_client_proc_init(struct lu_client_seq *seq)
{
        return 0;
}

static void seq_client_proc_fini(struct lu_client_seq *seq)
{
        return;
}
#endif

int seq_client_init(struct lu_client_seq *seq,
                    struct obd_export *exp,
                    enum lu_cli_type type,
                    const char *prefix,
                    struct lu_server_seq *srv)
{
        int rc;
        ENTRY;

        LASSERT(seq != NULL);
        LASSERT(prefix != NULL);

        seq->lcs_exp = exp;
        seq->lcs_srv = srv;
        seq->lcs_type = type;
        cfs_sema_init(&seq->lcs_sem, 1);
        seq->lcs_width = LUSTRE_SEQ_MAX_WIDTH;
        cfs_waitq_init(&seq->lcs_waitq);

        /* Make sure that things are clear before work is started. */
        seq_client_flush(seq);

        if (exp == NULL) {
                LASSERT(seq->lcs_srv != NULL);
        } else {
                LASSERT(seq->lcs_exp != NULL);
                seq->lcs_exp = class_export_get(seq->lcs_exp);
        }

        snprintf(seq->lcs_name, sizeof(seq->lcs_name),
                 "cli-%s", prefix);

        rc = seq_client_proc_init(seq);
        if (rc)
                seq_client_fini(seq);
        RETURN(rc);
}
EXPORT_SYMBOL(seq_client_init);

void seq_client_fini(struct lu_client_seq *seq)
{
        ENTRY;

        seq_client_proc_fini(seq);

        if (seq->lcs_exp != NULL) {
                class_export_put(seq->lcs_exp);
                seq->lcs_exp = NULL;
        }

        seq->lcs_srv = NULL;
        EXIT;
}
EXPORT_SYMBOL(seq_client_fini);
