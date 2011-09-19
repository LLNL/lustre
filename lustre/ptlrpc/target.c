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
 * Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Lustre Common Target
 * These are common function for MDT and OST recovery-related functionality
 *
 *   Author: Mikhail Pershin <tappro@sun.com>
 */

#include <obd.h>
#include <obd_class.h>
#include <lustre_fid.h>

/**
 * write data in last_rcvd file.
 */
static int lut_last_rcvd_write(const struct lu_env *env, struct lu_target *lut,
                               const struct lu_buf *buf, loff_t *off, int sync)
{
        struct thandle *th;
        int rc;
        ENTRY;

        th = dt_trans_create(env, lut->lut_bottom);
        if (IS_ERR(th))
                RETURN(PTR_ERR(th));

        rc = dt_declare_record_write(env, lut->lut_last_rcvd, buf->lb_len, *off, th);
        if (rc)
                GOTO(out, rc);

        rc = dt_trans_start_local(env, lut->lut_bottom, th);
        if (rc)
                GOTO(out, rc);

        rc = dt_record_write(env, lut->lut_last_rcvd, buf, off, th);

out:
        dt_trans_stop(env, lut->lut_bottom, th);

        CDEBUG(D_INFO, "write last_rcvd header rc = %d:\n"
               "uuid = %s\nlast_transno = "LPU64"\n",
               rc, lut->lut_lsd.lsd_uuid, lut->lut_lsd.lsd_last_transno);

        RETURN(rc);
}

/**
 * Allocate in-memory data for client slot related to export.
 */
int lut_client_alloc(struct obd_export *exp)
{
        OBD_ALLOC_PTR(exp->exp_target_data.ted_lcd);
        if (exp->exp_target_data.ted_lcd == NULL)
                RETURN(-ENOMEM);
        /* Mark that slot is not yet valid, 0 doesn't work here */
        exp->exp_target_data.ted_lr_idx = -1;
        RETURN(0);
}
EXPORT_SYMBOL(lut_client_alloc);

/**
 * Free in-memory data for client slot related to export.
 */
void lut_client_free(struct obd_export *exp)
{
        struct tg_export_data *ted = &exp->exp_target_data;
        struct lu_target *lut = class_exp2tgt(exp);

        LASSERT(lut);
        LASSERT(ted);
        OBD_FREE_PTR(ted->ted_lcd);
        ted->ted_lcd = NULL;

        /* Slot may be not yet assigned */
        if (ted->ted_lr_idx < 0)
                return;
        /* Clear bit when lcd is freed */
        LASSERT(lut->lut_client_bitmap);
        cfs_spin_lock(&lut->lut_client_bitmap_lock);
        if (!cfs_test_and_clear_bit(ted->ted_lr_idx, lut->lut_client_bitmap)) {
                CERROR("%s: client %u bit already clear in bitmap\n",
                       exp->exp_obd->obd_name, ted->ted_lr_idx);
                LBUG();
        }
        cfs_spin_unlock(&lut->lut_client_bitmap_lock);
}
EXPORT_SYMBOL(lut_client_free);

/**
 * Update client data in last_rcvd
 */
int lut_client_data_update(const struct lu_env *env, struct obd_export *exp)
{
        struct tg_export_data *ted = &exp->exp_target_data;
        struct lu_target *lut = class_exp2tgt(exp);
        struct lsd_client_data tmp_lcd;
        loff_t tmp_off = ted->ted_lr_off;
        struct lu_buf tmp_buf = {
                                        .lb_buf = &tmp_lcd,
                                        .lb_len = sizeof(tmp_lcd)
                                };
        int rc = 0;

        lcd_cpu_to_le(ted->ted_lcd, &tmp_lcd);
        LASSERT(lut->lut_last_rcvd);
        rc = lut_last_rcvd_write(env, lut, &tmp_buf, &tmp_off, 0);

        return rc;
}

/**
 * Update server data in last_rcvd
 */
static int lut_server_data_update(const struct lu_env *env,
                                  struct lu_target *lut, int sync)
{
        struct lr_server_data tmp_lsd;
        loff_t tmp_off = 0;
        struct lu_buf tmp_buf = {
                                        .lb_buf = &tmp_lsd,
                                        .lb_len = sizeof(tmp_lsd)
                                };
        int rc = 0;
        ENTRY;

        CDEBUG(D_SUPER,
               "%s: mount_count is "LPU64", last_transno is "LPU64"\n",
               lut->lut_lsd.lsd_uuid, lut->lut_obd->u.obt.obt_mount_count,
               lut->lut_last_transno);

        cfs_spin_lock(&lut->lut_translock);
        lut->lut_lsd.lsd_last_transno = lut->lut_last_transno;
        cfs_spin_unlock(&lut->lut_translock);

        lsd_cpu_to_le(&lut->lut_lsd, &tmp_lsd);
        if (lut->lut_last_rcvd != NULL)
                rc = lut_last_rcvd_write(env, lut, &tmp_buf, &tmp_off, sync);
        RETURN(rc);
}

void lut_client_epoch_update(const struct lu_env *env, struct obd_export *exp)
{
        struct lsd_client_data *lcd = exp->exp_target_data.ted_lcd;
        struct lu_target *lut = class_exp2tgt(exp);

        LASSERT(lut->lut_bottom);
        /** VBR: set client last_epoch to current epoch */
        if (lcd->lcd_last_epoch >= lut->lut_lsd.lsd_start_epoch)
                return;
        lcd->lcd_last_epoch = lut->lut_lsd.lsd_start_epoch;
        lut_client_data_update(env, exp);
}

/**
 * Update boot epoch when recovery ends
 */
void lut_boot_epoch_update(struct lu_target *lut)
{
        struct lu_env env;
        struct ptlrpc_request *req;
        __u32 start_epoch;
        cfs_list_t client_list;
        int rc;

        if (lut->lut_obd->obd_stopping)
                return;
        /** Increase server epoch after recovery */
        LASSERT(lut->lut_bottom);

        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc) {
                CERROR("Can't initialize environment rc=%d\n", rc);
                return;
        }

        cfs_spin_lock(&lut->lut_translock);
        start_epoch = lr_epoch(lut->lut_last_transno) + 1;
        lut->lut_last_transno = (__u64)start_epoch << LR_EPOCH_BITS;
        lut->lut_lsd.lsd_start_epoch = start_epoch;
        cfs_spin_unlock(&lut->lut_translock);

        CFS_INIT_LIST_HEAD(&client_list);
        /**
         * The recovery is not yet finished and final queue can still be updated
         * with resend requests. Move final list to separate one for processing
         */
        cfs_spin_lock(&lut->lut_obd->obd_recovery_task_lock);
        cfs_list_splice_init(&lut->lut_obd->obd_final_req_queue, &client_list);
        cfs_spin_unlock(&lut->lut_obd->obd_recovery_task_lock);

        /**
         * go through list of exports participated in recovery and
         * set new epoch for them
         */
        cfs_list_for_each_entry(req, &client_list, rq_list) {
                LASSERT(!req->rq_export->exp_delayed);
                if (!req->rq_export->exp_vbr_failed)
                        lut_client_epoch_update(&env, req->rq_export);
        }
        /** return list back at once */
        cfs_spin_lock(&lut->lut_obd->obd_recovery_task_lock);
        cfs_list_splice_init(&client_list, &lut->lut_obd->obd_final_req_queue);
        cfs_spin_unlock(&lut->lut_obd->obd_recovery_task_lock);
        /** update server epoch */
        lut_server_data_update(&env, lut, 1);
        lu_env_fini(&env);
}
EXPORT_SYMBOL(lut_boot_epoch_update);

/**
 * commit callback, need to update last_commited value
 */
struct lut_last_committed_callback {
        struct dt_txn_commit_cb llcc_cb;
        struct lu_target       *llcc_lut;
        struct obd_export      *llcc_exp;
        __u64                   llcc_transno;
};

void lut_cb_last_committed(struct lu_env *env, struct thandle *th,
                           struct dt_txn_commit_cb *cb, int err)
{
        struct lut_last_committed_callback *ccb;

        ccb = container_of0(cb, struct lut_last_committed_callback, llcc_cb);

        LASSERT(ccb->llcc_exp->exp_obd == ccb->llcc_lut->lut_obd);

        cfs_spin_lock(&ccb->llcc_lut->lut_translock);
        if (ccb->llcc_transno > ccb->llcc_lut->lut_obd->obd_last_committed)
                ccb->llcc_lut->lut_obd->obd_last_committed = ccb->llcc_transno;

        LASSERT(ccb->llcc_exp);
        if (ccb->llcc_transno > ccb->llcc_exp->exp_last_committed) {
                ccb->llcc_exp->exp_last_committed = ccb->llcc_transno;
                cfs_spin_unlock(&ccb->llcc_lut->lut_translock);
                ptlrpc_commit_replies(ccb->llcc_exp);
        } else {
                cfs_spin_unlock(&ccb->llcc_lut->lut_translock);
        }
        class_export_cb_put(ccb->llcc_exp);
        if (ccb->llcc_transno)
                CDEBUG(D_HA, "%s: transno "LPD64" is committed\n",
                       ccb->llcc_lut->lut_obd->obd_name, ccb->llcc_transno);
        cfs_list_del(&ccb->llcc_cb.dcb_linkage);
        OBD_FREE_PTR(ccb);
}

int lut_last_commit_cb_add(struct thandle *th, struct lu_target *lut,
                           struct obd_export *exp, __u64 transno)
{
        struct lut_last_committed_callback *ccb;
        int rc;

        OBD_ALLOC_PTR(ccb);
        if (ccb == NULL)
                return -ENOMEM;

        ccb->llcc_cb.dcb_func = lut_cb_last_committed;
        CFS_INIT_LIST_HEAD(&ccb->llcc_cb.dcb_linkage);
        ccb->llcc_lut = lut;
        ccb->llcc_exp = class_export_cb_get(exp);
        ccb->llcc_transno = transno;

        rc = dt_trans_cb_add(th, &ccb->llcc_cb);
        if (rc) {
                class_export_cb_put(exp);
                OBD_FREE_PTR(ccb);
        }
        return rc;
}
EXPORT_SYMBOL(lut_last_commit_cb_add);

struct lut_new_client_callback {
        struct dt_txn_commit_cb lncc_cb;
        struct obd_export      *lncc_exp;
};

void lut_cb_new_client(struct lu_env *env, struct thandle *th,
                       struct dt_txn_commit_cb *cb, int err)
{
        struct lut_new_client_callback *ccb;

        ccb = container_of0(cb, struct lut_new_client_callback, lncc_cb);

        LASSERT(ccb->lncc_exp->exp_obd);

        CDEBUG(D_RPCTRACE, "%s: committing for initial connect of %s\n",
               ccb->lncc_exp->exp_obd->obd_name,
               ccb->lncc_exp->exp_client_uuid.uuid);

        cfs_spin_lock(&ccb->lncc_exp->exp_lock);
        ccb->lncc_exp->exp_need_sync = 0;
        cfs_spin_unlock(&ccb->lncc_exp->exp_lock);
        class_export_cb_put(ccb->lncc_exp);

        cfs_list_del(&ccb->lncc_cb.dcb_linkage);
        OBD_FREE_PTR(ccb);
}

int lut_new_client_cb_add(struct thandle *th, struct obd_export *exp)
{
        struct lut_new_client_callback *ccb;
        int rc;

        OBD_ALLOC_PTR(ccb);
        if (ccb == NULL)
                return -ENOMEM;

        ccb->lncc_cb.dcb_func = lut_cb_new_client;
        CFS_INIT_LIST_HEAD(&ccb->lncc_cb.dcb_linkage);
        ccb->lncc_exp = class_export_cb_get(exp);

        rc = dt_trans_cb_add(th, &ccb->lncc_cb);
        if (rc) {
                class_export_cb_put(exp);
                OBD_FREE_PTR(ccb);
        }
        return rc;
}
EXPORT_SYMBOL(lut_new_client_cb_add);

int lut_init(const struct lu_env *env, struct lu_target *lut,
             struct obd_device *obd, struct dt_device *dt)
{
        struct dt_object_format dof;
        struct lu_attr          attr;
        struct lu_fid           fid;
        struct dt_object *o;
        int rc = 0;
        ENTRY;

        lut->lut_obd = obd;
        lut->lut_bottom = dt;
        lut->lut_last_rcvd = NULL;
        obd->u.obt.obt_lut = lut;

        cfs_spin_lock_init(&lut->lut_translock);
        cfs_spin_lock_init(&lut->lut_client_bitmap_lock);

        OBD_ALLOC(lut->lut_client_bitmap, LR_MAX_CLIENTS >> 3);
        if (lut->lut_client_bitmap == NULL)
                RETURN(-ENOMEM);

        /** obdfilter has no lu_device stack yet */
        if (dt == NULL)
                RETURN(rc);

        memset(&attr, 0, sizeof(attr));
        attr.la_valid = LA_MODE;
        attr.la_mode = S_IFREG | 0666;
        dof.dof_type = DFT_REGULAR;

        lu_local_obj_fid(&fid, MDT_LAST_RECV_OID);

        o = dt_find_or_create(env, lut->lut_bottom, &fid, &dof, &attr);
        if (!IS_ERR(o)) {
                lut->lut_last_rcvd = o;
        } else {
                OBD_FREE(lut->lut_client_bitmap, LR_MAX_CLIENTS >> 3);
                lut->lut_client_bitmap = NULL;
                rc = PTR_ERR(o);
                CERROR("cannot open %s: rc = %d\n", LAST_RCVD, rc);
        }

        RETURN(rc);
}
EXPORT_SYMBOL(lut_init);

void lut_fini(const struct lu_env *env, struct lu_target *lut)
{
        ENTRY;

        if (lut->lut_client_bitmap) {
                OBD_FREE(lut->lut_client_bitmap, LR_MAX_CLIENTS >> 3);
                lut->lut_client_bitmap = NULL;
        }
        if (lut->lut_last_rcvd) {
                lu_object_put(env, &lut->lut_last_rcvd->do_lu);
                lut->lut_last_rcvd = NULL;
        }
        EXIT;
}
EXPORT_SYMBOL(lut_fini);
