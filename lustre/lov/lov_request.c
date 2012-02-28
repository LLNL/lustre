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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LOV

#ifdef __KERNEL__
#include <libcfs/libcfs.h>
#else
#include <liblustre.h>
#endif

#include <obd_class.h>
#include <obd_lov.h>
#include <lustre/lustre_idl.h>

#include "lov_internal.h"

static void lov_init_set(struct lov_request_set *set)
{
        set->set_count = 0;
        set->set_completes = 0;
        set->set_success = 0;
        set->set_cookies = 0;
        CFS_INIT_LIST_HEAD(&set->set_list);
        cfs_atomic_set(&set->set_refcount, 1);
        cfs_waitq_init(&set->set_waitq);
        cfs_spin_lock_init(&set->set_lock);
}

void lov_finish_set(struct lov_request_set *set)
{
        cfs_list_t *pos, *n;
        ENTRY;

        LASSERT(set);
        cfs_list_for_each_safe(pos, n, &set->set_list) {
                struct lov_request *req = cfs_list_entry(pos,
                                                         struct lov_request,
                                                         rq_link);
                cfs_list_del_init(&req->rq_link);

                if (req->rq_oi.oi_oa)
                        OBDO_FREE(req->rq_oi.oi_oa);
                if (req->rq_oi.oi_md)
                        OBD_FREE_LARGE(req->rq_oi.oi_md, req->rq_buflen);
                if (req->rq_oi.oi_osfs)
                        OBD_FREE(req->rq_oi.oi_osfs,
                                 sizeof(*req->rq_oi.oi_osfs));
                OBD_FREE(req, sizeof(*req));
        }

        if (set->set_pga) {
                int len = set->set_oabufs * sizeof(*set->set_pga);
                OBD_FREE_LARGE(set->set_pga, len);
        }
        if (set->set_lockh)
                lov_llh_put(set->set_lockh);

        OBD_FREE(set, sizeof(*set));
        EXIT;
}

int lov_finished_set(struct lov_request_set *set)
{
        CDEBUG(D_INFO, "check set %d/%d\n", set->set_completes,
               set->set_count);
        return set->set_completes == set->set_count;
}

void lov_update_set(struct lov_request_set *set,
                    struct lov_request *req, int rc)
{
        req->rq_complete = 1;
        req->rq_rc = rc;

        set->set_completes++;
        if (rc == 0)
                set->set_success++;

        cfs_waitq_signal(&set->set_waitq);
}

int lov_update_common_set(struct lov_request_set *set,
                          struct lov_request *req, int rc)
{
        struct lov_obd *lov = &set->set_exp->exp_obd->u.lov;
        ENTRY;

        lov_update_set(set, req, rc);

        /* grace error on inactive ost */
        if (rc && !(lov->lov_tgts[req->rq_idx] &&
                    lov->lov_tgts[req->rq_idx]->ltd_active))
                rc = 0;

        /* FIXME in raid1 regime, should return 0 */
        RETURN(rc);
}

void lov_set_add_req(struct lov_request *req, struct lov_request_set *set)
{
        cfs_list_add_tail(&req->rq_link, &set->set_list);
        set->set_count++;
        req->rq_rqset = set;
}

extern void osc_update_enqueue(struct lustre_handle *lov_lockhp,
                               struct lov_oinfo *loi, int flags,
                               struct ost_lvb *lvb, __u32 mode, int rc);

static int lov_update_enqueue_lov(struct obd_export *exp,
                                  struct lustre_handle *lov_lockhp,
                                  struct lov_oinfo *loi, int flags, int idx,
                                  __u64 oid, int rc)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;

        if (rc != ELDLM_OK &&
            !(rc == ELDLM_LOCK_ABORTED && (flags & LDLM_FL_HAS_INTENT))) {
                memset(lov_lockhp, 0, sizeof(*lov_lockhp));
                if (lov->lov_tgts[idx] && lov->lov_tgts[idx]->ltd_active) {
                        /* -EUSERS used by OST to report file contention */
                        if (rc != -EINTR && rc != -EUSERS)
                                CERROR("enqueue objid "LPX64" subobj "
                                       LPX64" on OST idx %d: rc %d\n",
                                       oid, loi->loi_id, loi->loi_ost_idx, rc);
                } else
                        rc = ELDLM_OK;
        }
        return rc;
}

int lov_update_enqueue_set(struct lov_request *req, __u32 mode, int rc)
{
        struct lov_request_set *set = req->rq_rqset;
        struct lustre_handle *lov_lockhp;
        struct obd_info *oi = set->set_oi;
        struct lov_oinfo *loi;
        ENTRY;

        LASSERT(oi != NULL);

        lov_lockhp = set->set_lockh->llh_handles + req->rq_stripe;
        loi = oi->oi_md->lsm_oinfo[req->rq_stripe];

        /* XXX LOV STACKING: OSC gets a copy, created in lov_prep_enqueue_set
         * and that copy can be arbitrarily out of date.
         *
         * The LOV API is due for a serious rewriting anyways, and this
         * can be addressed then. */

        lov_stripe_lock(oi->oi_md);
        osc_update_enqueue(lov_lockhp, loi, oi->oi_flags,
                           &req->rq_oi.oi_md->lsm_oinfo[0]->loi_lvb, mode, rc);
        if (rc == ELDLM_LOCK_ABORTED && (oi->oi_flags & LDLM_FL_HAS_INTENT))
                memset(lov_lockhp, 0, sizeof *lov_lockhp);
        rc = lov_update_enqueue_lov(set->set_exp, lov_lockhp, loi, oi->oi_flags,
                                    req->rq_idx, oi->oi_md->lsm_object_id, rc);
        lov_stripe_unlock(oi->oi_md);
        lov_update_set(set, req, rc);
        RETURN(rc);
}

/* The callback for osc_enqueue that updates lov info for every OSC request. */
static int cb_update_enqueue(void *cookie, int rc)
{
        struct obd_info *oinfo = cookie;
        struct ldlm_enqueue_info *einfo;
        struct lov_request *lovreq;

        lovreq = container_of(oinfo, struct lov_request, rq_oi);
        einfo = lovreq->rq_rqset->set_ei;
        return lov_update_enqueue_set(lovreq, einfo->ei_mode, rc);
}

static int enqueue_done(struct lov_request_set *set, __u32 mode)
{
        struct lov_request *req;
        struct lov_obd *lov = &set->set_exp->exp_obd->u.lov;
        int rc = 0;
        ENTRY;

        /* enqueue/match success, just return */
        if (set->set_completes && set->set_completes == set->set_success)
                RETURN(0);

        /* cancel enqueued/matched locks */
        cfs_list_for_each_entry(req, &set->set_list, rq_link) {
                struct lustre_handle *lov_lockhp;

                if (!req->rq_complete || req->rq_rc)
                        continue;

                lov_lockhp = set->set_lockh->llh_handles + req->rq_stripe;
                LASSERT(lov_lockhp);
                if (!lustre_handle_is_used(lov_lockhp))
                        continue;

                rc = obd_cancel(lov->lov_tgts[req->rq_idx]->ltd_exp,
                                req->rq_oi.oi_md, mode, lov_lockhp);
                if (rc && lov->lov_tgts[req->rq_idx] &&
                    lov->lov_tgts[req->rq_idx]->ltd_active)
                        CERROR("cancelling obdjid "LPX64" on OST "
                               "idx %d error: rc = %d\n",
                               req->rq_oi.oi_md->lsm_object_id,
                               req->rq_idx, rc);
        }
        if (set->set_lockh)
                lov_llh_put(set->set_lockh);
        RETURN(rc);
}

int lov_fini_enqueue_set(struct lov_request_set *set, __u32 mode, int rc,
                         struct ptlrpc_request_set *rqset)
{
        int ret = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        /* Do enqueue_done only for sync requests and if any request
         * succeeded. */
        if (!rqset) {
                if (rc)
                        set->set_completes = 0;
                ret = enqueue_done(set, mode);
        } else if (set->set_lockh)
                lov_llh_put(set->set_lockh);

        lov_put_reqset(set);

        RETURN(rc ? rc : ret);
}

int lov_prep_enqueue_set(struct obd_export *exp, struct obd_info *oinfo,
                         struct ldlm_enqueue_info *einfo,
                         struct lov_request_set **reqset)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lov_request_set *set;
        int i, rc = 0;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oi = oinfo;
        set->set_ei = einfo;
        set->set_lockh = lov_llh_new(oinfo->oi_md);
        if (set->set_lockh == NULL)
                GOTO(out_set, rc = -ENOMEM);
        oinfo->oi_lockh->cookie = set->set_lockh->llh_handle.h_cookie;

        for (i = 0; i < oinfo->oi_md->lsm_stripe_count; i++) {
                struct lov_oinfo *loi;
                struct lov_request *req;
                obd_off start, end;

                loi = oinfo->oi_md->lsm_oinfo[i];
                if (!lov_stripe_intersects(oinfo->oi_md, i,
                                           oinfo->oi_policy.l_extent.start,
                                           oinfo->oi_policy.l_extent.end,
                                           &start, &end))
                        continue;

                if (!lov->lov_tgts[loi->loi_ost_idx] ||
                    !lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        continue;
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);

                req->rq_buflen = sizeof(*req->rq_oi.oi_md) +
                        sizeof(struct lov_oinfo *) +
                        sizeof(struct lov_oinfo);
                OBD_ALLOC_LARGE(req->rq_oi.oi_md, req->rq_buflen);
                if (req->rq_oi.oi_md == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }
                req->rq_oi.oi_md->lsm_oinfo[0] =
                        ((void *)req->rq_oi.oi_md) + sizeof(*req->rq_oi.oi_md) +
                        sizeof(struct lov_oinfo *);

                /* Set lov request specific parameters. */
                req->rq_oi.oi_lockh = set->set_lockh->llh_handles + i;
                req->rq_oi.oi_cb_up = cb_update_enqueue;
                req->rq_oi.oi_flags = oinfo->oi_flags;

                LASSERT(req->rq_oi.oi_lockh);

                req->rq_oi.oi_policy.l_extent.gid =
                        oinfo->oi_policy.l_extent.gid;
                req->rq_oi.oi_policy.l_extent.start = start;
                req->rq_oi.oi_policy.l_extent.end = end;

                req->rq_idx = loi->loi_ost_idx;
                req->rq_stripe = i;

                /* XXX LOV STACKING: submd should be from the subobj */
                req->rq_oi.oi_md->lsm_object_id = loi->loi_id;
                req->rq_oi.oi_md->lsm_object_seq = loi->loi_seq;
                req->rq_oi.oi_md->lsm_stripe_count = 0;
                req->rq_oi.oi_md->lsm_oinfo[0]->loi_kms_valid =
                        loi->loi_kms_valid;
                req->rq_oi.oi_md->lsm_oinfo[0]->loi_kms = loi->loi_kms;
                req->rq_oi.oi_md->lsm_oinfo[0]->loi_lvb = loi->loi_lvb;

                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(0);
out_set:
        lov_fini_enqueue_set(set, einfo->ei_mode, rc, NULL);
        RETURN(rc);
}

int lov_update_match_set(struct lov_request_set *set, struct lov_request *req,
                         int rc)
{
        int ret = rc;
        ENTRY;

        if (rc > 0)
                ret = 0;
        else if (rc == 0)
                ret = 1;
        lov_update_set(set, req, ret);
        RETURN(rc);
}

int lov_fini_match_set(struct lov_request_set *set, __u32 mode, int flags)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        rc = enqueue_done(set, mode);
        if ((set->set_count == set->set_success) &&
            (flags & LDLM_FL_TEST_LOCK))
                lov_llh_put(set->set_lockh);

        lov_put_reqset(set);

        RETURN(rc);
}

int lov_prep_match_set(struct obd_export *exp, struct obd_info *oinfo,
                       struct lov_stripe_md *lsm, ldlm_policy_data_t *policy,
                       __u32 mode, struct lustre_handle *lockh,
                       struct lov_request_set **reqset)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lov_request_set *set;
        int i, rc = 0;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oi = oinfo;
        set->set_oi->oi_md = lsm;
        set->set_lockh = lov_llh_new(lsm);
        if (set->set_lockh == NULL)
                GOTO(out_set, rc = -ENOMEM);
        lockh->cookie = set->set_lockh->llh_handle.h_cookie;

        for (i = 0; i < lsm->lsm_stripe_count; i++){
                struct lov_oinfo *loi;
                struct lov_request *req;
                obd_off start, end;

                loi = lsm->lsm_oinfo[i];
                if (!lov_stripe_intersects(lsm, i, policy->l_extent.start,
                                           policy->l_extent.end, &start, &end))
                        continue;

                /* FIXME raid1 should grace this error */
                if (!lov->lov_tgts[loi->loi_ost_idx] ||
                    !lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        GOTO(out_set, rc = -EIO);
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);

                req->rq_buflen = sizeof(*req->rq_oi.oi_md);
                OBD_ALLOC_LARGE(req->rq_oi.oi_md, req->rq_buflen);
                if (req->rq_oi.oi_md == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }

                req->rq_oi.oi_policy.l_extent.start = start;
                req->rq_oi.oi_policy.l_extent.end = end;
                req->rq_oi.oi_policy.l_extent.gid = policy->l_extent.gid;

                req->rq_idx = loi->loi_ost_idx;
                req->rq_stripe = i;

                /* XXX LOV STACKING: submd should be from the subobj */
                req->rq_oi.oi_md->lsm_object_id = loi->loi_id;
                req->rq_oi.oi_md->lsm_object_seq = loi->loi_seq;
                req->rq_oi.oi_md->lsm_stripe_count = 0;

                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(rc);
out_set:
        lov_fini_match_set(set, mode, 0);
        RETURN(rc);
}

int lov_fini_cancel_set(struct lov_request_set *set)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);

        LASSERT(set->set_exp);
        if (set->set_lockh)
                lov_llh_put(set->set_lockh);

        lov_put_reqset(set);

        RETURN(rc);
}

int lov_prep_cancel_set(struct obd_export *exp, struct obd_info *oinfo,
                        struct lov_stripe_md *lsm, __u32 mode,
                        struct lustre_handle *lockh,
                        struct lov_request_set **reqset)
{
        struct lov_request_set *set;
        int i, rc = 0;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oi = oinfo;
        set->set_oi->oi_md = lsm;
        set->set_lockh = lov_handle2llh(lockh);
        if (set->set_lockh == NULL) {
                CERROR("LOV: invalid lov lock handle %p\n", lockh);
                GOTO(out_set, rc = -EINVAL);
        }
        lockh->cookie = set->set_lockh->llh_handle.h_cookie;

        for (i = 0; i < lsm->lsm_stripe_count; i++){
                struct lov_request *req;
                struct lustre_handle *lov_lockhp;
                struct lov_oinfo *loi = lsm->lsm_oinfo[i];

                lov_lockhp = set->set_lockh->llh_handles + i;
                if (!lustre_handle_is_used(lov_lockhp)) {
                        CDEBUG(D_INFO, "lov idx %d subobj "LPX64" no lock\n",
                               loi->loi_ost_idx, loi->loi_id);
                        continue;
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);

                req->rq_buflen = sizeof(*req->rq_oi.oi_md);
                OBD_ALLOC_LARGE(req->rq_oi.oi_md, req->rq_buflen);
                if (req->rq_oi.oi_md == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }

                req->rq_idx = loi->loi_ost_idx;
                req->rq_stripe = i;

                /* XXX LOV STACKING: submd should be from the subobj */
                req->rq_oi.oi_md->lsm_object_id = loi->loi_id;
                req->rq_oi.oi_md->lsm_object_seq = loi->loi_seq;
                req->rq_oi.oi_md->lsm_stripe_count = 0;

                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(rc);
out_set:
        lov_fini_cancel_set(set);
        RETURN(rc);
}

static int lov_update_create_set(struct lov_request_set *set,
                                 struct lov_request *req, int rc)
{
        struct obd_trans_info *oti = set->set_oti;
        struct lov_stripe_md *lsm = set->set_oi->oi_md;
        struct lov_oinfo *loi;
        struct lov_obd *lov = &set->set_exp->exp_obd->u.lov;
        ENTRY;

        if (rc && lov->lov_tgts[req->rq_idx] &&
            lov->lov_tgts[req->rq_idx]->ltd_active) {
                /* Pre-creating objects may timeout via -ETIMEDOUT or
                 * -ENOTCONN both are always non-critical events. */
                CDEBUG(rc == -ETIMEDOUT || rc == -ENOTCONN ? D_HA : D_ERROR,
                       "error creating fid "LPX64" sub-object "
                       "on OST idx %d/%d: rc = %d\n",
                       set->set_oi->oi_oa->o_id, req->rq_idx,
                       lsm->lsm_stripe_count, rc);
                if (rc > 0) {
                        CERROR("obd_create returned invalid err %d\n", rc);
                        rc = -EIO;
                }
        }

        cfs_spin_lock(&set->set_lock);
        req->rq_stripe = set->set_success;
        loi = lsm->lsm_oinfo[req->rq_stripe];


        if (rc) {
                lov_update_set(set, req, rc);
                cfs_spin_unlock(&set->set_lock);
                RETURN(rc);
        }

        loi->loi_id = req->rq_oi.oi_oa->o_id;
        loi->loi_seq = req->rq_oi.oi_oa->o_seq;
        loi->loi_ost_idx = req->rq_idx;
        loi_init(loi);

        if (oti && set->set_cookies)
                ++oti->oti_logcookies;
        if (req->rq_oi.oi_oa->o_valid & OBD_MD_FLCOOKIE)
                set->set_cookie_sent++;

        lov_update_set(set, req, rc);
        cfs_spin_unlock(&set->set_lock);

        CDEBUG(D_INODE, "objid "LPX64" has subobj "LPX64"/"LPU64" at idx %d\n",
               lsm->lsm_object_id, loi->loi_id, loi->loi_id, req->rq_idx);
        RETURN(rc);
}

static int create_done(struct obd_export *exp, struct lov_request_set *set,
                       struct lov_stripe_md **lsmp)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct obd_trans_info *oti = set->set_oti;
        struct obdo *src_oa = set->set_oi->oi_oa;
        struct lov_request *req;
        struct obdo *ret_oa = NULL;
        int attrset = 0, rc = 0;
        ENTRY;

        LASSERT(set->set_completes);

        /* try alloc objects on other osts if osc_create fails for
         * exceptions: RPC failure, ENOSPC, etc */
        if (set->set_count != set->set_success) {
                cfs_list_for_each_entry (req, &set->set_list, rq_link) {
                        if (req->rq_rc == 0)
                                continue;

                        set->set_completes--;
                        req->rq_complete = 0;

                        rc = qos_remedy_create(set, req);
                        lov_update_create_set(set, req, rc);
                }
        }

        /* no successful creates */
        if (set->set_success == 0)
                GOTO(cleanup, rc);

        if (set->set_count != set->set_success) {
                set->set_count = set->set_success;
                qos_shrink_lsm(set);
        }

        OBDO_ALLOC(ret_oa);
        if (ret_oa == NULL)
                GOTO(cleanup, rc = -ENOMEM);

        cfs_list_for_each_entry(req, &set->set_list, rq_link) {
                if (!req->rq_complete || req->rq_rc)
                        continue;
                lov_merge_attrs(ret_oa, req->rq_oi.oi_oa,
                                req->rq_oi.oi_oa->o_valid, set->set_oi->oi_md,
                                req->rq_stripe, &attrset);
        }
        if (src_oa->o_valid & OBD_MD_FLSIZE &&
            ret_oa->o_size != src_oa->o_size) {
                CERROR("original size "LPU64" isn't new object size "LPU64"\n",
                       src_oa->o_size, ret_oa->o_size);
                LBUG();
        }
        ret_oa->o_id = src_oa->o_id;
        ret_oa->o_seq = src_oa->o_seq;
        ret_oa->o_valid |= OBD_MD_FLID | OBD_MD_FLGROUP;
        memcpy(src_oa, ret_oa, sizeof(*src_oa));
        OBDO_FREE(ret_oa);

        *lsmp = set->set_oi->oi_md;
        GOTO(done, rc = 0);

cleanup:
        cfs_list_for_each_entry(req, &set->set_list, rq_link) {
                struct obd_export *sub_exp;
                int err = 0;

                if (!req->rq_complete || req->rq_rc)
                        continue;

                sub_exp = lov->lov_tgts[req->rq_idx]->ltd_exp;
                err = obd_destroy(sub_exp, req->rq_oi.oi_oa, NULL, oti, NULL,
                                  NULL);
                if (err)
                        CERROR("Failed to uncreate objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               src_oa->o_id, req->rq_oi.oi_oa->o_id,
                               req->rq_idx, rc);
        }
        if (*lsmp == NULL)
                obd_free_memmd(exp, &set->set_oi->oi_md);
done:
        if (oti && set->set_cookies) {
                oti->oti_logcookies = set->set_cookies;
                if (!set->set_cookie_sent) {
                        oti_free_cookies(oti);
                        src_oa->o_valid &= ~OBD_MD_FLCOOKIE;
                } else {
                        src_oa->o_valid |= OBD_MD_FLCOOKIE;
                }
        }
        RETURN(rc);
}

int lov_fini_create_set(struct lov_request_set *set,struct lov_stripe_md **lsmp)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        if (set->set_completes)
                rc = create_done(set->set_exp, set, lsmp);

        lov_put_reqset(set);
        RETURN(rc);
}

int cb_create_update(void *cookie, int rc)
{
        struct obd_info *oinfo = cookie;
        struct lov_request *lovreq;

        lovreq = container_of(oinfo, struct lov_request, rq_oi);

        if (CFS_FAIL_CHECK(OBD_FAIL_MDS_OSC_CREATE_FAIL))
                if (lovreq->rq_idx == cfs_fail_val)
                        rc = -ENOTCONN;

        rc= lov_update_create_set(lovreq->rq_rqset, lovreq, rc);
        if (lov_finished_set(lovreq->rq_rqset))
                lov_put_reqset(lovreq->rq_rqset);
        return rc;
}

int lov_prep_create_set(struct obd_export *exp, struct obd_info *oinfo,
                        struct lov_stripe_md **lsmp, struct obdo *src_oa,
                        struct obd_trans_info *oti,
                        struct lov_request_set **reqset)
{
        struct lov_request_set *set;
        int rc = 0;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oi = oinfo;
        set->set_oi->oi_md = *lsmp;
        set->set_oi->oi_oa = src_oa;
        set->set_oti = oti;
        lov_get_reqset(set);

        rc = qos_prep_create(exp, set);
        /* qos_shrink_lsm() may have allocated a new lsm */
        *lsmp = oinfo->oi_md;
        if (rc) {
                lov_fini_create_set(set, lsmp);
                lov_put_reqset(set);
        } else {
                *reqset = set;
        }
        RETURN(rc);
}

static int common_attr_done(struct lov_request_set *set)
{
        cfs_list_t *pos;
        struct lov_request *req;
        struct obdo *tmp_oa;
        int rc = 0, attrset = 0;
        ENTRY;

        LASSERT(set->set_oi != NULL);

        if (set->set_oi->oi_oa == NULL)
                RETURN(0);

        if (!set->set_success)
                RETURN(-EIO);

        OBDO_ALLOC(tmp_oa);
        if (tmp_oa == NULL)
                GOTO(out, rc = -ENOMEM);

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                if (!req->rq_complete || req->rq_rc)
                        continue;
                if (req->rq_oi.oi_oa->o_valid == 0)   /* inactive stripe */
                        continue;
                lov_merge_attrs(tmp_oa, req->rq_oi.oi_oa,
                                req->rq_oi.oi_oa->o_valid,
                                set->set_oi->oi_md, req->rq_stripe, &attrset);
        }
        if (!attrset) {
                CERROR("No stripes had valid attrs\n");
                rc = -EIO;
        }
        if ((set->set_oi->oi_oa->o_valid & OBD_MD_FLEPOCH) &&
            (set->set_oi->oi_md->lsm_stripe_count != attrset)) {
                /* When we take attributes of some epoch, we require all the
                 * ost to be active. */
                CERROR("Not all the stripes had valid attrs\n");
                GOTO(out, rc = -EIO);
        }

        tmp_oa->o_id = set->set_oi->oi_oa->o_id;
        memcpy(set->set_oi->oi_oa, tmp_oa, sizeof(*set->set_oi->oi_oa));
out:
        if (tmp_oa)
                OBDO_FREE(tmp_oa);
        RETURN(rc);

}

static int brw_done(struct lov_request_set *set)
{
        struct lov_stripe_md *lsm = set->set_oi->oi_md;
        struct lov_oinfo     *loi = NULL;
        cfs_list_t *pos;
        struct lov_request *req;
        ENTRY;

        cfs_list_for_each (pos, &set->set_list) {
                req = cfs_list_entry(pos, struct lov_request, rq_link);

                if (!req->rq_complete || req->rq_rc)
                        continue;

                loi = lsm->lsm_oinfo[req->rq_stripe];

                if (req->rq_oi.oi_oa->o_valid & OBD_MD_FLBLOCKS)
                        loi->loi_lvb.lvb_blocks = req->rq_oi.oi_oa->o_blocks;
        }

        RETURN(0);
}

int lov_fini_brw_set(struct lov_request_set *set)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        if (set->set_completes) {
                rc = brw_done(set);
                /* FIXME update qos data here */
        }
        lov_put_reqset(set);

        RETURN(rc);
}

int lov_prep_brw_set(struct obd_export *exp, struct obd_info *oinfo,
                     obd_count oa_bufs, struct brw_page *pga,
                     struct obd_trans_info *oti,
                     struct lov_request_set **reqset)
{
        struct {
                obd_count       index;
                obd_count       count;
                obd_count       off;
        } *info = NULL;
        struct lov_request_set *set;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int rc = 0, i, shift;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oti = oti;
        set->set_oi = oinfo;
        set->set_oabufs = oa_bufs;
        OBD_ALLOC_LARGE(set->set_pga, oa_bufs * sizeof(*set->set_pga));
        if (!set->set_pga)
                GOTO(out, rc = -ENOMEM);

        OBD_ALLOC_LARGE(info, sizeof(*info) * oinfo->oi_md->lsm_stripe_count);
        if (!info)
                GOTO(out, rc = -ENOMEM);

        /* calculate the page count for each stripe */
        for (i = 0; i < oa_bufs; i++) {
                int stripe = lov_stripe_number(oinfo->oi_md, pga[i].off);
                info[stripe].count++;
        }

        /* alloc and initialize lov request */
        shift = 0;
        for (i = 0; i < oinfo->oi_md->lsm_stripe_count; i++){
                struct lov_oinfo *loi = NULL;
                struct lov_request *req;

                if (info[i].count == 0)
                        continue;

                loi = oinfo->oi_md->lsm_oinfo[i];
                if (!lov->lov_tgts[loi->loi_ost_idx] ||
                    !lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        GOTO(out, rc = -EIO);
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out, rc = -ENOMEM);

                OBDO_ALLOC(req->rq_oi.oi_oa);
                if (req->rq_oi.oi_oa == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out, rc = -ENOMEM);
                }

                if (oinfo->oi_oa) {
                        memcpy(req->rq_oi.oi_oa, oinfo->oi_oa,
                               sizeof(*req->rq_oi.oi_oa));
                }
                req->rq_oi.oi_oa->o_id = loi->loi_id;
                req->rq_oi.oi_oa->o_seq = loi->loi_seq;
                req->rq_oi.oi_oa->o_stripe_idx = i;

                req->rq_buflen = sizeof(*req->rq_oi.oi_md);
                OBD_ALLOC_LARGE(req->rq_oi.oi_md, req->rq_buflen);
                if (req->rq_oi.oi_md == NULL) {
                        OBDO_FREE(req->rq_oi.oi_oa);
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out, rc = -ENOMEM);
                }

                req->rq_idx = loi->loi_ost_idx;
                req->rq_stripe = i;

                /* XXX LOV STACKING */
                req->rq_oi.oi_md->lsm_object_id = loi->loi_id;
                req->rq_oi.oi_md->lsm_object_seq = loi->loi_seq;
                req->rq_oabufs = info[i].count;
                req->rq_pgaidx = shift;
                shift += req->rq_oabufs;

                /* remember the index for sort brw_page array */
                info[i].index = req->rq_pgaidx;

                req->rq_oi.oi_capa = oinfo->oi_capa;

                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out, rc = -EIO);

        /* rotate & sort the brw_page array */
        for (i = 0; i < oa_bufs; i++) {
                int stripe = lov_stripe_number(oinfo->oi_md, pga[i].off);

                shift = info[stripe].index + info[stripe].off;
                LASSERT(shift < oa_bufs);
                set->set_pga[shift] = pga[i];
                lov_stripe_offset(oinfo->oi_md, pga[i].off, stripe,
                                  &set->set_pga[shift].off);
                info[stripe].off++;
        }
out:
        if (info)
                OBD_FREE_LARGE(info,
                               sizeof(*info) * oinfo->oi_md->lsm_stripe_count);

        if (rc == 0)
                *reqset = set;
        else
                lov_fini_brw_set(set);

        RETURN(rc);
}

int lov_fini_getattr_set(struct lov_request_set *set)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        if (set->set_completes)
                rc = common_attr_done(set);

        lov_put_reqset(set);

        RETURN(rc);
}

/* The callback for osc_getattr_async that finilizes a request info when a
 * response is received. */
static int cb_getattr_update(void *cookie, int rc)
{
        struct obd_info *oinfo = cookie;
        struct lov_request *lovreq;
        lovreq = container_of(oinfo, struct lov_request, rq_oi);
        return lov_update_common_set(lovreq->rq_rqset, lovreq, rc);
}

int lov_prep_getattr_set(struct obd_export *exp, struct obd_info *oinfo,
                         struct lov_request_set **reqset)
{
        struct lov_request_set *set;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int rc = 0, i;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oi = oinfo;

        for (i = 0; i < oinfo->oi_md->lsm_stripe_count; i++) {
                struct lov_oinfo *loi;
                struct lov_request *req;

                loi = oinfo->oi_md->lsm_oinfo[i];
                if (!lov->lov_tgts[loi->loi_ost_idx] ||
                    !lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        if (oinfo->oi_oa->o_valid & OBD_MD_FLEPOCH)
                                /* SOM requires all the OSTs to be active. */
                                GOTO(out_set, rc = -EIO);
                        continue;
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);

                req->rq_stripe = i;
                req->rq_idx = loi->loi_ost_idx;

                OBDO_ALLOC(req->rq_oi.oi_oa);
                if (req->rq_oi.oi_oa == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }
                memcpy(req->rq_oi.oi_oa, oinfo->oi_oa,
                       sizeof(*req->rq_oi.oi_oa));
                req->rq_oi.oi_oa->o_id = loi->loi_id;
                req->rq_oi.oi_oa->o_seq = loi->loi_seq;
                req->rq_oi.oi_cb_up = cb_getattr_update;
                req->rq_oi.oi_capa = oinfo->oi_capa;

                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(rc);
out_set:
        lov_fini_getattr_set(set);
        RETURN(rc);
}

int lov_fini_destroy_set(struct lov_request_set *set)
{
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        if (set->set_completes) {
                /* FIXME update qos data here */
        }

        lov_put_reqset(set);

        RETURN(0);
}

int lov_prep_destroy_set(struct obd_export *exp, struct obd_info *oinfo,
                         struct obdo *src_oa, struct lov_stripe_md *lsm,
                         struct obd_trans_info *oti,
                         struct lov_request_set **reqset)
{
        struct lov_request_set *set;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int rc = 0, i;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oi = oinfo;
        set->set_oi->oi_md = lsm;
        set->set_oi->oi_oa = src_oa;
        set->set_oti = oti;
        if (oti != NULL && src_oa->o_valid & OBD_MD_FLCOOKIE)
                set->set_cookies = oti->oti_logcookies;

        for (i = 0; i < lsm->lsm_stripe_count; i++) {
                struct lov_oinfo *loi;
                struct lov_request *req;

                loi = lsm->lsm_oinfo[i];
                if (!lov->lov_tgts[loi->loi_ost_idx] ||
                    !lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        continue;
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);

                req->rq_stripe = i;
                req->rq_idx = loi->loi_ost_idx;

                OBDO_ALLOC(req->rq_oi.oi_oa);
                if (req->rq_oi.oi_oa == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }
                memcpy(req->rq_oi.oi_oa, src_oa, sizeof(*req->rq_oi.oi_oa));
                req->rq_oi.oi_oa->o_id = loi->loi_id;
                req->rq_oi.oi_oa->o_seq = loi->loi_seq;
                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(rc);
out_set:
        lov_fini_destroy_set(set);
        RETURN(rc);
}

int lov_fini_setattr_set(struct lov_request_set *set)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        if (set->set_completes) {
                rc = common_attr_done(set);
                /* FIXME update qos data here */
        }

        lov_put_reqset(set);
        RETURN(rc);
}

int lov_update_setattr_set(struct lov_request_set *set,
                           struct lov_request *req, int rc)
{
        struct lov_obd *lov = &req->rq_rqset->set_exp->exp_obd->u.lov;
        struct lov_stripe_md *lsm = req->rq_rqset->set_oi->oi_md;
        ENTRY;

        lov_update_set(set, req, rc);

        /* grace error on inactive ost */
        if (rc && !(lov->lov_tgts[req->rq_idx] &&
                    lov->lov_tgts[req->rq_idx]->ltd_active))
                rc = 0;

        if (rc == 0) {
                if (req->rq_oi.oi_oa->o_valid & OBD_MD_FLCTIME)
                        lsm->lsm_oinfo[req->rq_stripe]->loi_lvb.lvb_ctime =
                                req->rq_oi.oi_oa->o_ctime;
                if (req->rq_oi.oi_oa->o_valid & OBD_MD_FLMTIME)
                        lsm->lsm_oinfo[req->rq_stripe]->loi_lvb.lvb_mtime =
                                req->rq_oi.oi_oa->o_mtime;
                if (req->rq_oi.oi_oa->o_valid & OBD_MD_FLATIME)
                        lsm->lsm_oinfo[req->rq_stripe]->loi_lvb.lvb_atime =
                                req->rq_oi.oi_oa->o_atime;
        }

        RETURN(rc);
}

/* The callback for osc_setattr_async that finilizes a request info when a
 * response is received. */
static int cb_setattr_update(void *cookie, int rc)
{
        struct obd_info *oinfo = cookie;
        struct lov_request *lovreq;
        lovreq = container_of(oinfo, struct lov_request, rq_oi);
        return lov_update_setattr_set(lovreq->rq_rqset, lovreq, rc);
}

int lov_prep_setattr_set(struct obd_export *exp, struct obd_info *oinfo,
                         struct obd_trans_info *oti,
                         struct lov_request_set **reqset)
{
        struct lov_request_set *set;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int rc = 0, i;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oti = oti;
        set->set_oi = oinfo;
        if (oti != NULL && oinfo->oi_oa->o_valid & OBD_MD_FLCOOKIE)
                set->set_cookies = oti->oti_logcookies;

        for (i = 0; i < oinfo->oi_md->lsm_stripe_count; i++) {
                struct lov_oinfo *loi = oinfo->oi_md->lsm_oinfo[i];
                struct lov_request *req;

                if (!lov->lov_tgts[loi->loi_ost_idx] ||
                    !lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        continue;
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);
                req->rq_stripe = i;
                req->rq_idx = loi->loi_ost_idx;

                OBDO_ALLOC(req->rq_oi.oi_oa);
                if (req->rq_oi.oi_oa == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }
                memcpy(req->rq_oi.oi_oa, oinfo->oi_oa,
                       sizeof(*req->rq_oi.oi_oa));
                req->rq_oi.oi_oa->o_id = loi->loi_id;
                req->rq_oi.oi_oa->o_seq= loi->loi_seq;
                req->rq_oi.oi_oa->o_stripe_idx = i;
                req->rq_oi.oi_cb_up = cb_setattr_update;
                req->rq_oi.oi_capa = oinfo->oi_capa;

                if (oinfo->oi_oa->o_valid & OBD_MD_FLSIZE) {
                        int off = lov_stripe_offset(oinfo->oi_md,
                                                    oinfo->oi_oa->o_size, i,
                                                    &req->rq_oi.oi_oa->o_size);

                        if (off < 0 && req->rq_oi.oi_oa->o_size)
                                req->rq_oi.oi_oa->o_size--;

                        CDEBUG(D_INODE, "stripe %d has size "LPU64"/"LPU64"\n",
                               i, req->rq_oi.oi_oa->o_size,
                               oinfo->oi_oa->o_size);
                }
                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(rc);
out_set:
        lov_fini_setattr_set(set);
        RETURN(rc);
}

int lov_fini_punch_set(struct lov_request_set *set)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        if (set->set_completes) {
                rc = -EIO;
                /* FIXME update qos data here */
                if (set->set_success)
                        rc = common_attr_done(set);
        }

        lov_put_reqset(set);

        RETURN(rc);
}

int lov_update_punch_set(struct lov_request_set *set,
                         struct lov_request *req, int rc)
{
        struct lov_obd *lov = &req->rq_rqset->set_exp->exp_obd->u.lov;
        struct lov_stripe_md *lsm = req->rq_rqset->set_oi->oi_md;
        ENTRY;

        lov_update_set(set, req, rc);

        /* grace error on inactive ost */
        if (rc && !lov->lov_tgts[req->rq_idx]->ltd_active)
                rc = 0;

        if (rc == 0) {
                lov_stripe_lock(lsm);
                if (req->rq_oi.oi_oa->o_valid & OBD_MD_FLBLOCKS) {
                        lsm->lsm_oinfo[req->rq_stripe]->loi_lvb.lvb_blocks =
                                req->rq_oi.oi_oa->o_blocks;
                }

                /* Do we need to update lvb_size here? It needn't because
                 * it have been done in ll_truncate(). -jay */
                lov_stripe_unlock(lsm);
        }

        RETURN(rc);
}

/* The callback for osc_punch that finilizes a request info when a response
 * is received. */
static int cb_update_punch(void *cookie, int rc)
{
        struct obd_info *oinfo = cookie;
        struct lov_request *lovreq;
        lovreq = container_of(oinfo, struct lov_request, rq_oi);
        return lov_update_punch_set(lovreq->rq_rqset, lovreq, rc);
}

int lov_prep_punch_set(struct obd_export *exp, struct obd_info *oinfo,
                       struct obd_trans_info *oti,
                       struct lov_request_set **reqset)
{
        struct lov_request_set *set;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int rc = 0, i;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_oi = oinfo;
        set->set_exp = exp;

        for (i = 0; i < oinfo->oi_md->lsm_stripe_count; i++) {
                struct lov_oinfo *loi = oinfo->oi_md->lsm_oinfo[i];
                struct lov_request *req;
                obd_off rs, re;

                if (!lov_stripe_intersects(oinfo->oi_md, i,
                                           oinfo->oi_policy.l_extent.start,
                                           oinfo->oi_policy.l_extent.end,
                                           &rs, &re))
                        continue;

                if (!lov->lov_tgts[loi->loi_ost_idx] ||
                    !lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        GOTO(out_set, rc = -EIO);
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);
                req->rq_stripe = i;
                req->rq_idx = loi->loi_ost_idx;

                OBDO_ALLOC(req->rq_oi.oi_oa);
                if (req->rq_oi.oi_oa == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }
                memcpy(req->rq_oi.oi_oa, oinfo->oi_oa,
                       sizeof(*req->rq_oi.oi_oa));
                req->rq_oi.oi_oa->o_id = loi->loi_id;
                req->rq_oi.oi_oa->o_seq = loi->loi_seq;
                req->rq_oi.oi_oa->o_valid |= OBD_MD_FLGROUP;

                req->rq_oi.oi_oa->o_stripe_idx = i;
                req->rq_oi.oi_cb_up = cb_update_punch;

                req->rq_oi.oi_policy.l_extent.start = rs;
                req->rq_oi.oi_policy.l_extent.end = re;
                req->rq_oi.oi_policy.l_extent.gid = -1;

                req->rq_oi.oi_capa = oinfo->oi_capa;

                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(rc);
out_set:
        lov_fini_punch_set(set);
        RETURN(rc);
}

int lov_fini_sync_set(struct lov_request_set *set)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);
        LASSERT(set->set_exp);
        if (set->set_completes) {
                if (!set->set_success)
                        rc = -EIO;
                /* FIXME update qos data here */
        }

        lov_put_reqset(set);

        RETURN(rc);
}

/* The callback for osc_sync that finilizes a request info when a
 * response is recieved. */
static int cb_sync_update(void *cookie, int rc)
{
        struct obd_info *oinfo = cookie;
        struct lov_request *lovreq;

        lovreq = container_of(oinfo, struct lov_request, rq_oi);
        return lov_update_common_set(lovreq->rq_rqset, lovreq, rc);
}

int lov_prep_sync_set(struct obd_export *exp, struct obd_info *oinfo,
                      obd_off start, obd_off end,
                      struct lov_request_set **reqset)
{
        struct lov_request_set *set;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int rc = 0, i;
        ENTRY;

        OBD_ALLOC_PTR(set);
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_exp = exp;
        set->set_oi = oinfo;

        for (i = 0; i < oinfo->oi_md->lsm_stripe_count; i++) {
                struct lov_oinfo *loi = oinfo->oi_md->lsm_oinfo[i];
                struct lov_request *req;
                obd_off rs, re;

                if (!lov->lov_tgts[loi->loi_ost_idx] ||
                    !lov->lov_tgts[loi->loi_ost_idx]->ltd_active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        continue;
                }

                if (!lov_stripe_intersects(oinfo->oi_md, i, start, end, &rs,
                                           &re))
                        continue;

                OBD_ALLOC_PTR(req);
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);
                req->rq_stripe = i;
                req->rq_idx = loi->loi_ost_idx;

                OBDO_ALLOC(req->rq_oi.oi_oa);
                if (req->rq_oi.oi_oa == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }
                *req->rq_oi.oi_oa = *oinfo->oi_oa;
                req->rq_oi.oi_oa->o_id = loi->loi_id;
                req->rq_oi.oi_oa->o_seq = loi->loi_seq;
                req->rq_oi.oi_oa->o_stripe_idx = i;

                req->rq_oi.oi_policy.l_extent.start = rs;
                req->rq_oi.oi_policy.l_extent.end = re;
                req->rq_oi.oi_policy.l_extent.gid = -1;
                req->rq_oi.oi_cb_up = cb_sync_update;

                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(rc);
out_set:
        lov_fini_sync_set(set);
        RETURN(rc);
}

#define LOV_U64_MAX ((__u64)~0ULL)
#define LOV_SUM_MAX(tot, add)                                           \
        do {                                                            \
                if ((tot) + (add) < (tot))                              \
                        (tot) = LOV_U64_MAX;                            \
                else                                                    \
                        (tot) += (add);                                 \
        } while(0)

int lov_fini_statfs(struct obd_device *obd, struct obd_statfs *osfs,int success)
{
        ENTRY;

        if (success) {
                __u32 expected_stripes = lov_get_stripecnt(&obd->u.lov, 0);

                if (osfs->os_files != LOV_U64_MAX)
                        do_div(osfs->os_files, expected_stripes);
                if (osfs->os_ffree != LOV_U64_MAX)
                        do_div(osfs->os_ffree, expected_stripes);

                cfs_spin_lock(&obd->obd_osfs_lock);
                memcpy(&obd->obd_osfs, osfs, sizeof(*osfs));
                obd->obd_osfs_age = cfs_time_current_64();
                cfs_spin_unlock(&obd->obd_osfs_lock);
                RETURN(0);
        }

        RETURN(-EIO);
}

int lov_fini_statfs_set(struct lov_request_set *set)
{
        int rc = 0;
        ENTRY;

        if (set == NULL)
                RETURN(0);

        if (set->set_completes) {
                rc = lov_fini_statfs(set->set_obd, set->set_oi->oi_osfs,
                                     set->set_success);
        }
        lov_put_reqset(set);
        RETURN(rc);
}

void lov_update_statfs(struct obd_statfs *osfs, struct obd_statfs *lov_sfs,
                       int success)
{
        int shift = 0, quit = 0;
        __u64 tmp;

        if (success == 0) {
                memcpy(osfs, lov_sfs, sizeof(*lov_sfs));
        } else {
                if (osfs->os_bsize != lov_sfs->os_bsize) {
                        /* assume all block sizes are always powers of 2 */
                        /* get the bits difference */
                        tmp = osfs->os_bsize | lov_sfs->os_bsize;
                        for (shift = 0; shift <= 64; ++shift) {
                                if (tmp & 1) {
                                        if (quit)
                                                break;
                                        else
                                                quit = 1;
                                        shift = 0;
                                }
                                tmp >>= 1;
                        }
                }

                if (osfs->os_bsize < lov_sfs->os_bsize) {
                        osfs->os_bsize = lov_sfs->os_bsize;

                        osfs->os_bfree  >>= shift;
                        osfs->os_bavail >>= shift;
                        osfs->os_blocks >>= shift;
                } else if (shift != 0) {
                        lov_sfs->os_bfree  >>= shift;
                        lov_sfs->os_bavail >>= shift;
                        lov_sfs->os_blocks >>= shift;
                }
#ifdef MIN_DF
                /* Sandia requested that df (and so, statfs) only
                   returned minimal available space on
                   a single OST, so people would be able to
                   write this much data guaranteed. */
                if (osfs->os_bavail > lov_sfs->os_bavail) {
                        /* Presumably if new bavail is smaller,
                           new bfree is bigger as well */
                        osfs->os_bfree = lov_sfs->os_bfree;
                        osfs->os_bavail = lov_sfs->os_bavail;
                }
#else
                osfs->os_bfree += lov_sfs->os_bfree;
                osfs->os_bavail += lov_sfs->os_bavail;
#endif
                osfs->os_blocks += lov_sfs->os_blocks;
                /* XXX not sure about this one - depends on policy.
                 *   - could be minimum if we always stripe on all OBDs
                 *     (but that would be wrong for any other policy,
                 *     if one of the OBDs has no more objects left)
                 *   - could be sum if we stripe whole objects
                 *   - could be average, just to give a nice number
                 *
                 * To give a "reasonable" (if not wholly accurate)
                 * number, we divide the total number of free objects
                 * by expected stripe count (watch out for overflow).
                 */
                LOV_SUM_MAX(osfs->os_files, lov_sfs->os_files);
                LOV_SUM_MAX(osfs->os_ffree, lov_sfs->os_ffree);
        }
}

/* The callback for osc_statfs_async that finilizes a request info when a
 * response is received. */
static int cb_statfs_update(void *cookie, int rc)
{
        struct obd_info *oinfo = cookie;
        struct lov_request *lovreq;
        struct obd_statfs *osfs, *lov_sfs;
        struct lov_obd *lov;
        struct lov_tgt_desc *tgt;
        struct obd_device *lovobd, *tgtobd;
        int success;
        ENTRY;

        lovreq = container_of(oinfo, struct lov_request, rq_oi);
        lovobd = lovreq->rq_rqset->set_obd;
        lov = &lovobd->u.lov;
        osfs = lovreq->rq_rqset->set_oi->oi_osfs;
        lov_sfs = oinfo->oi_osfs;
        success = lovreq->rq_rqset->set_success;
        /* XXX: the same is done in lov_update_common_set, however
           lovset->set_exp is not initialized. */
        lov_update_set(lovreq->rq_rqset, lovreq, rc);
        if (rc)
                GOTO(out, rc);

        obd_getref(lovobd);
        tgt = lov->lov_tgts[lovreq->rq_idx];
        if (!tgt || !tgt->ltd_active)
                GOTO(out_update, rc);

        tgtobd = class_exp2obd(tgt->ltd_exp);
        cfs_spin_lock(&tgtobd->obd_osfs_lock);
        memcpy(&tgtobd->obd_osfs, lov_sfs, sizeof(*lov_sfs));
        if ((oinfo->oi_flags & OBD_STATFS_FROM_CACHE) == 0)
                tgtobd->obd_osfs_age = cfs_time_current_64();
        cfs_spin_unlock(&tgtobd->obd_osfs_lock);

out_update:
        lov_update_statfs(osfs, lov_sfs, success);
        qos_update(lov);
        obd_putref(lovobd);

out:
        if (lovreq->rq_rqset->set_oi->oi_flags & OBD_STATFS_PTLRPCD &&
            lov_finished_set(lovreq->rq_rqset)) {
               lov_statfs_interpret(NULL, lovreq->rq_rqset,
                                    lovreq->rq_rqset->set_success !=
                                                  lovreq->rq_rqset->set_count);
               qos_statfs_done(lov);
        }

        RETURN(0);
}

int lov_prep_statfs_set(struct obd_device *obd, struct obd_info *oinfo,
                        struct lov_request_set **reqset)
{
        struct lov_request_set *set;
        struct lov_obd *lov = &obd->u.lov;
        int rc = 0, i;
        ENTRY;

        OBD_ALLOC(set, sizeof(*set));
        if (set == NULL)
                RETURN(-ENOMEM);
        lov_init_set(set);

        set->set_obd = obd;
        set->set_oi = oinfo;

        /* We only get block data from the OBD */
        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                struct lov_request *req;

                if (!lov->lov_tgts[i] || (!lov->lov_tgts[i]->ltd_active
                                          && (oinfo->oi_flags & OBD_STATFS_NODELAY))) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", i);
                        continue;
                }

                /* skip targets that have been explicitely disabled by the
                 * administrator */
                if (!lov->lov_tgts[i]->ltd_exp) {
                        CDEBUG(D_HA, "lov idx %d administratively disabled\n", i);
                        continue;
                }

                OBD_ALLOC(req, sizeof(*req));
                if (req == NULL)
                        GOTO(out_set, rc = -ENOMEM);

                OBD_ALLOC(req->rq_oi.oi_osfs, sizeof(*req->rq_oi.oi_osfs));
                if (req->rq_oi.oi_osfs == NULL) {
                        OBD_FREE(req, sizeof(*req));
                        GOTO(out_set, rc = -ENOMEM);
                }

                req->rq_idx = i;
                req->rq_oi.oi_cb_up = cb_statfs_update;
                req->rq_oi.oi_flags = oinfo->oi_flags;

                lov_set_add_req(req, set);
        }
        if (!set->set_count)
                GOTO(out_set, rc = -EIO);
        *reqset = set;
        RETURN(rc);
out_set:
        lov_fini_statfs_set(set);
        RETURN(rc);
}
