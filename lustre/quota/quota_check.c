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
 * Copyright (c) 2011, 2012, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */


#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LQUOTA

#ifdef __KERNEL__
# include <linux/version.h>
# include <linux/module.h>
# include <linux/init.h>
# include <linux/fs.h>
# include <linux/jbd.h>
# include <linux/ext3_fs.h>
# include <linux/smp_lock.h>
# include <linux/buffer_head.h>
# include <linux/workqueue.h>
# include <linux/mount.h>
#else /* __KERNEL__ */
# include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_mds.h>
#include <lustre_dlm.h>
#include <lustre_cfg.h>
#include <obd_ost.h>
#include <lustre_fsfilt.h>
#include <lustre_quota.h>
#include "quota_internal.h"

#ifdef HAVE_QUOTA_SUPPORT
#ifdef __KERNEL__
static int target_quotacheck_callback(struct obd_export *exp,
                                      struct obd_quotactl *oqctl)
{
        struct ptlrpc_request *req;
        struct obd_quotactl   *body;
        int                    rc;
        ENTRY;

        req = ptlrpc_request_alloc_pack(exp->exp_imp_reverse, &RQF_QC_CALLBACK,
                                        LUSTRE_OBD_VERSION, OBD_QC_CALLBACK);
        if (req == NULL)
                RETURN(-ENOMEM);

        body = req_capsule_client_get(&req->rq_pill, &RMF_OBD_QUOTACTL);
        *body = *oqctl;

        ptlrpc_request_set_replen(req);

        rc = ptlrpc_queue_wait(req);
        ptlrpc_req_finished(req);

        RETURN(rc);
}

static int target_quotacheck_thread(void *data)
{
        struct quotacheck_thread_args *qta = data;
        struct obd_export *exp;
        struct obd_device *obd;
        struct obd_quotactl *oqctl;
        struct lvfs_run_ctxt saved;
        int rc;

        cfs_daemonize_ctxt("quotacheck");

        exp = qta->qta_exp;
        obd = qta->qta_obd;
        oqctl = &qta->qta_oqctl;

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        rc = fsfilt_quotacheck(obd, qta->qta_sb, oqctl);
        if (rc)
                CERROR("%s: fsfilt_quotacheck: %d\n", obd->obd_name, rc);

        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

        rc = target_quotacheck_callback(exp, oqctl);
        class_export_put(exp);
        cfs_up(qta->qta_sem);
        OBD_FREE_PTR(qta);
        return rc;
}

int target_quota_check(struct obd_device *obd, struct obd_export *exp,
                       struct obd_quotactl *oqctl)
{
        struct obd_device_target *obt = &obd->u.obt;
        struct quotacheck_thread_args *qta;
        int rc = 0;
        ENTRY;

        OBD_ALLOC_PTR(qta);
        if (!qta)
                RETURN(ENOMEM);

        cfs_down(&obt->obt_quotachecking);

        qta->qta_exp = exp;
        qta->qta_obd = obd;
        qta->qta_oqctl = *oqctl;
        qta->qta_oqctl.qc_id = obt->obt_qfmt; /* override qfmt version */
        qta->qta_sb = obt->obt_sb;
        qta->qta_sem = &obt->obt_quotachecking;

        /* quotaoff firstly */
        oqctl->qc_cmd = Q_QUOTAOFF;
        if (!strcmp(obd->obd_type->typ_name, LUSTRE_MDS_NAME)) {
                rc = do_mds_quota_off(obd, oqctl);
                if (rc && rc != -EALREADY) {
                        CERROR("off quota on MDS failed: %d\n", rc);
                        GOTO(out, rc);
                }

                /* quota master */
                rc = init_admin_quotafiles(obd, &qta->qta_oqctl);
                if (rc) {
                        CERROR("init_admin_quotafiles failed: %d\n", rc);
                        GOTO(out, rc);
                }
        } else {
                struct lvfs_run_ctxt saved;
                struct lustre_quota_ctxt *qctxt = &obt->obt_qctxt;

                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = fsfilt_quotactl(obd, obt->obt_sb, oqctl);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                if (!rc) {
                        qctxt->lqc_flags &= ~UGQUOTA2LQC(oqctl->qc_type);
                } else if (!quota_is_off(qctxt, oqctl)) {
                        CERROR("off quota on OSS failed: %d\n", rc);
                        GOTO(out, rc);
                }
        }

        /* we get ref for exp because target_quotacheck_callback() will use this
         * export later b=18126 */
        class_export_get(exp);
        rc = cfs_create_thread(target_quotacheck_thread, qta,
                               CFS_DAEMON_FLAGS);
        if (rc >= 0) {
                /* target_quotacheck_thread will drop the ref on exp and release
                 * obt_quotachecking */
                CDEBUG(D_INFO, "%s: target_quotacheck_thread: %d\n",
                       obd->obd_name, rc);
                RETURN(0);
        } else {
                CERROR("%s: error starting quotacheck_thread: %d\n",
                       obd->obd_name, rc);
                class_export_put(exp);
                EXIT;
        }

out:
        cfs_up(&obt->obt_quotachecking);
        OBD_FREE_PTR(qta);
        return rc;
}

#endif /* __KERNEL__ */
#endif /* HAVE_QUOTA_SUPPORT */
