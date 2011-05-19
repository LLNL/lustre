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
 * lustre/ofd/ofd_recovery.c
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 * Author: Alex Tomas <alex@clusterfs.com>
 * Author: Mike Pershin <tappro@sun.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include "ofd_internal.h"

struct thandle *ofd_trans_create(const struct lu_env *env,
                                    struct ofd_device *ofd)
{
        struct ofd_thread_info *info = ofd_info(env);
        struct thandle *th;
        struct filter_export_data *fed;
        struct tg_export_data *ted;
        int rc;

        LASSERT(info);

        th = dt_trans_create(env, ofd->ofd_osd);
        if (IS_ERR(th))
                return th;

        /* export can require sync operations */
        if (info->fti_exp != NULL)
                th->th_sync |= info->fti_exp->exp_need_sync;

        /* no last_rcvd update needed */
        if (info->fti_exp == NULL)
                return th;

        /* declare last_rcvd update */
        fed = &info->fti_exp->exp_filter_data;
        ted = &fed->fed_ted;
        rc = dt_declare_record_write(env, ofd->ofd_last_rcvd,
                                     sizeof(*ted->ted_lcd),
                                     ted->ted_lr_off, th);
        if (rc)
                RETURN(ERR_PTR(rc));
        /* declare last_rcvd header update */
        rc = dt_declare_record_write(env, ofd->ofd_last_rcvd,
                                     sizeof(ofd->ofd_fsd), 0, th);
        if (rc)
                RETURN(ERR_PTR(rc));
        return th;
}

int ofd_trans_start(const struct lu_env *env,
                       struct ofd_device *ofd,
                       struct thandle *th)
{
        return dt_trans_start(env, ofd->ofd_osd, th);
}

void ofd_trans_stop(const struct lu_env *env,
                       struct ofd_device *ofd,
                       struct ofd_object *obj,
                       struct thandle *th)
{
        /* version change is required for this object */
        if (obj)
                ofd_info(env)->fti_obj = obj;

        dt_trans_stop(env, ofd->ofd_osd, th);
}

/*
 * last_rcvd & last_committed update callbacks
 */
static int ofd_last_rcvd_update(struct ofd_thread_info *info,
                                   struct thandle *th)
{
        struct ofd_device *ofd = ofd_exp(info->fti_exp);
        struct filter_export_data *fed;
        struct lsd_client_data *lcd;
        __s32 rc = th->th_result;
        __u64 *transno_p;
        loff_t off;
        int err;
        ENTRY;

        LASSERT(ofd);
        LASSERT(info->fti_exp);

        fed = &info->fti_exp->exp_filter_data;
        LASSERT(fed);
        lcd = fed->fed_ted.ted_lcd;

        /* if the export has already been failed, we have no last_rcvd slot */
        if (info->fti_exp->exp_failed) {
                CWARN("commit transaction for disconnected client %s: rc %d\n",
                      info->fti_exp->exp_client_uuid.uuid, rc);
                if (rc == 0)
                        rc = -ENOTCONN;
                RETURN(rc);
        }
        LASSERT(lcd);
        off = fed->fed_ted.ted_lr_off;

        transno_p = &lcd->lcd_last_transno;
        lcd->lcd_last_xid = info->fti_xid;

        /*
         * When we store zero transno in mcd we can lost last transno value
         * because mcd contains 0, but msd is not yet written
         * The server data should be updated also if the latest
         * transno is rewritten by zero. See the bug 11125 for details.
         */
        if (info->fti_transno == 0 &&
            *transno_p == ofd->ofd_last_transno) {
                cfs_spin_lock(&ofd->ofd_transno_lock);
                ofd->ofd_fsd.lsd_last_transno = ofd->ofd_last_transno;
                cfs_spin_unlock(&ofd->ofd_transno_lock);
                lut_server_data_write(info->fti_env, &ofd->ofd_lut, th);
        }

        *transno_p = info->fti_transno;
        LASSERT(fed->fed_ted.ted_lr_off > 0);
        err = lut_client_data_write(info->fti_env, &ofd->ofd_lut, lcd,
                                    &off, th);

        RETURN(err);
}

/* Set new object versions */
static void ofd_version_set(struct ofd_thread_info *info)
{
        if (info->fti_obj != NULL) {
                dt_version_set(info->fti_env,
                               ofd_object_child(info->fti_obj),
                               info->fti_transno);
                info->fti_obj = NULL;
        }
}

/* Update last_rcvd records with latests transaction data */
int ofd_txn_stop_cb(const struct lu_env *env, struct thandle *txn,
                       void *cookie)
{
        struct ofd_device *ofd = cookie;
        struct ofd_thread_info *info;
        ENTRY;

        info = lu_context_key_get(&env->le_ctx, &ofd_thread_key);

        if (info->fti_exp == NULL ||
            info->fti_exp->exp_filter_data.fed_ted.ted_lcd == NULL) {
                 RETURN(0);
        }

        LASSERT(ofd_exp(info->fti_exp) == ofd);
        if (info->fti_has_trans) {
                if (info->fti_mult_trans == 0) {
                        CERROR("More than one transaction "LPU64"\n",
                               info->fti_transno);
                        RETURN(0);
                }
                /* we need another transno to be assigned */
                info->fti_transno = 0;
        } else {
                info->fti_has_trans = 1;
        }

        cfs_spin_lock(&ofd->ofd_transno_lock);
        if (txn->th_result != 0) {
                if (info->fti_transno != 0) {
                        CERROR("Replay transno "LPU64" failed: rc %d\n",
                               info->fti_transno, txn->th_result);
                        info->fti_transno = 0;
                }
        } else if (info->fti_transno == 0) {
                info->fti_transno = ++ ofd->ofd_last_transno;
        } else {
                /* should be replay */
                if (info->fti_transno > ofd->ofd_last_transno)
                       ofd->ofd_last_transno = info->fti_transno;
        }
        cfs_spin_unlock(&ofd->ofd_transno_lock);

        /** VBR: set new versions */
        if (txn->th_result == 0)
                ofd_version_set(info);

        /* filling reply data */
        CDEBUG(D_INODE, "transno = %llu, last_committed = %llu\n",
               info->fti_transno, ofd_obd(ofd)->obd_last_committed);

        /* if can't add callback, do sync write */
        txn->th_sync = !!lut_last_commit_cb_add(txn, &ofd->ofd_lut,
                                                info->fti_exp,
                                                info->fti_transno);

        return ofd_last_rcvd_update(info, txn);
}

int ofd_fs_setup(const struct lu_env *env, struct ofd_device *ofd,
                    struct obd_device *obd)
{
        struct ofd_thread_info *info = ofd_info(env);
        struct ofd_object *fo;
        int rc = 0;
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_FS_SETUP))
                RETURN (-ENOENT);

        /* prepare transactions callbacks */
        ofd->ofd_txn_cb.dtc_txn_start = NULL;
        ofd->ofd_txn_cb.dtc_txn_stop = ofd_txn_stop_cb;
        ofd->ofd_txn_cb.dtc_txn_commit = NULL;
        ofd->ofd_txn_cb.dtc_cookie = ofd;
        ofd->ofd_txn_cb.dtc_tag = LCT_DT_THREAD;
        CFS_INIT_LIST_HEAD(&ofd->ofd_txn_cb.dtc_linkage);

        dt_txn_callback_add(ofd->ofd_osd, &ofd->ofd_txn_cb);

        LASSERT(ofd->ofd_osd);
        rc = lut_init(env, &ofd->ofd_lut, obd, ofd->ofd_osd);
        if (rc)
                GOTO(out, rc);

        rc = ofd_server_data_init(env, ofd);
        if (rc)
                GOTO(out_lut, rc);

        lu_local_obj_fid(&info->fti_fid, OFD_HEALTH_CHECK_OID);
        memset(&info->fti_attr, 0, sizeof(info->fti_attr));
        info->fti_attr.la_valid = LA_MODE;
        info->fti_attr.la_mode = S_IFREG | 0666;

        fo = ofd_object_find_or_create(env, ofd, &info->fti_fid,
                                       &info->fti_attr);
        if (IS_ERR(fo))
                GOTO(out_lut, rc = PTR_ERR(fo));

        ofd->ofd_health_check_file = ofd_object_child(fo);

        lu_local_obj_fid(&info->fti_fid, OFD_LAST_GROUP_OID);
        memset(&info->fti_attr, 0, sizeof(info->fti_attr));
        info->fti_attr.la_valid = LA_MODE;
        info->fti_attr.la_mode = S_IFREG | 0666;

        fo = ofd_object_find_or_create(env, ofd, &info->fti_fid,
                                       &info->fti_attr);
        if (IS_ERR(fo))
                GOTO(out_hc, rc = PTR_ERR(fo));

        ofd->ofd_last_group_file = ofd_object_child(fo);

        rc = ofd_groups_init(env, ofd);
        if (rc)
                GOTO(out_lg, rc);

        RETURN(0);
out_lg:
        lu_object_put(env, &ofd->ofd_last_group_file->do_lu);
out_hc:
        lu_object_put(env, &ofd->ofd_health_check_file->do_lu);
out_lut:
        lut_fini(env, &ofd->ofd_lut);
out:
        dt_txn_callback_del(ofd->ofd_osd, &ofd->ofd_txn_cb);
        target_recovery_fini(obd);
        return rc;
}

void ofd_fs_cleanup(const struct lu_env *env, struct ofd_device *ofd)
{
        int i;
        ENTRY;

        ofd_info_init(env, NULL);

        for (i = 0; i <= ofd->ofd_max_group; i++) {
                if (ofd->ofd_lastid_obj[i]) {
                        ofd_last_id_write(env, ofd, i);
                        ofd_group_fini(env, ofd, i);
                }
        }

        i = dt_sync(env, ofd->ofd_osd);
        if (i)
                CERROR("can't sync: %d\n", i);

        /* Remove transaction callback */
        dt_txn_callback_del(ofd->ofd_osd, &ofd->ofd_txn_cb);

        if (ofd->ofd_last_group_file) {
                lu_object_put(env, &ofd->ofd_last_group_file->do_lu);
                ofd->ofd_last_group_file = NULL;
        }

        if (ofd->ofd_health_check_file) {
                lu_object_put(env, &ofd->ofd_health_check_file->do_lu);
                ofd->ofd_health_check_file = NULL;
        }

        ofd_free_capa_keys(ofd);
        cleanup_capa_hash(ofd->ofd_capa_hash);

        EXIT;
}


