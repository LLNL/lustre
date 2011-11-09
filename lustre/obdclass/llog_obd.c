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
#include "llog_internal.h"

/* helper functions for calling the llog obd methods */
static struct llog_ctxt* llog_new_ctxt(struct obd_device *obd)
{
        struct llog_ctxt *ctxt;

        OBD_ALLOC_PTR(ctxt);
        if (!ctxt)
                return NULL;

        ctxt->loc_obd = obd;
        cfs_atomic_set(&ctxt->loc_refcount, 1);

        return ctxt;
}

static void llog_ctxt_destroy(struct llog_ctxt *ctxt)
{
        if (ctxt->loc_exp) {
                class_export_put(ctxt->loc_exp);
                ctxt->loc_exp = NULL;
        }
        if (ctxt->loc_imp) {
                class_import_put(ctxt->loc_imp);
                ctxt->loc_imp = NULL;
        }
        LASSERT(ctxt->loc_llcd == NULL);
        OBD_FREE_PTR(ctxt);
}

int __llog_ctxt_put(const struct lu_env *env, struct llog_ctxt *ctxt)
{
        struct obd_llog_group *olg = ctxt->loc_olg;
        struct obd_device *obd;
        int rc = 0;

        cfs_spin_lock(&olg->olg_lock);
        if (!cfs_atomic_dec_and_test(&ctxt->loc_refcount)) {
                cfs_spin_unlock(&olg->olg_lock);
                return rc;
        }
        olg->olg_ctxts[ctxt->loc_idx] = NULL;
        cfs_spin_unlock(&olg->olg_lock);

        if (ctxt->loc_lcm)
                lcm_put(ctxt->loc_lcm);

        obd = ctxt->loc_obd;
        cfs_spin_lock(&obd->obd_dev_lock);
        /* sync with llog ctxt user thread */
        cfs_spin_unlock(&obd->obd_dev_lock);

        /* obd->obd_starting is needed for the case of cleanup
         * in error case while obd is starting up. */
        LASSERTF(obd->obd_starting == 1 ||
                 obd->obd_stopping == 1 || obd->obd_set_up == 0,
                 "wrong obd state: %d/%d/%d\n", !!obd->obd_starting,
                 !!obd->obd_stopping, !!obd->obd_set_up);

        /* cleanup the llog ctxt here */
        if (CTXTP(ctxt, cleanup))
                rc = CTXTP(ctxt, cleanup)(env, ctxt);

        llog_ctxt_destroy(ctxt);
        cfs_waitq_signal(&olg->olg_waitq);
        return rc;
}
EXPORT_SYMBOL(__llog_ctxt_put);

int llog_cleanup(const struct lu_env *env, struct llog_ctxt *ctxt)
{
        struct l_wait_info lwi = LWI_INTR(LWI_ON_SIGNAL_NOOP, NULL);
        struct obd_llog_group *olg;
        int rc, idx;
        ENTRY;

        LASSERT(ctxt != NULL);
        LASSERT(ctxt != LP_POISON);

        olg = ctxt->loc_olg;
        LASSERT(olg != NULL);
        LASSERT(olg != LP_POISON);

        idx = ctxt->loc_idx;

        /* 
         * Banlance the ctxt get when calling llog_cleanup()
         */
        LASSERT(cfs_atomic_read(&ctxt->loc_refcount) < LI_POISON);
        LASSERT(cfs_atomic_read(&ctxt->loc_refcount) > 1);
        llog_ctxt_put(ctxt);

        /* 
         * Try to free the ctxt. 
         */
        rc = __llog_ctxt_put(env, ctxt);
        if (rc)
                CERROR("Error %d while cleaning up ctxt %p\n",
                       rc, ctxt);

        l_wait_event(olg->olg_waitq,
                     llog_group_ctxt_null(olg, idx), &lwi);

        RETURN(rc);
}
EXPORT_SYMBOL(llog_cleanup);

int llog_setup_named(const struct lu_env *env, struct obd_device *obd,
                     struct obd_llog_group *olg, int index,
                     struct obd_device *disk_obd, int count,
                     struct llog_logid *logid, const char *logname,
                     struct llog_operations *op)
{
        struct llog_ctxt *ctxt;
        int rc = 0;
        ENTRY;

        if (index < 0 || index >= LLOG_MAX_CTXTS)
                RETURN(-EINVAL);

        LASSERT(olg != NULL);

        ctxt = llog_new_ctxt(obd);
        if (!ctxt)
                RETURN(-ENOMEM);
        ctxt->loc_flags = LLOG_CTXT_FLAG_UNINITIALIZED;

        ctxt->loc_obd = obd;
        ctxt->loc_olg = olg;
        ctxt->loc_idx = index;
        ctxt->loc_logops = op;
        cfs_sema_init(&ctxt->loc_sem, 1);
        ctxt->loc_exp = class_export_get(disk_obd->obd_self_export);

        rc = llog_group_set_ctxt(olg, ctxt, index);
        if (rc) {
                llog_ctxt_destroy(ctxt);
                if (rc == -EEXIST) {
                        ctxt = llog_group_get_ctxt(olg, index);
                        if (ctxt) {
                                CDEBUG(D_CONFIG, "obd %s ctxt %d already set up\n",
                                       obd->obd_name, index);
                                LASSERT(ctxt->loc_olg == olg);
                                LASSERT(ctxt->loc_obd == obd);
                                LASSERT(ctxt->loc_exp == disk_obd->obd_self_export);
                                LASSERT(ctxt->loc_logops == op);
                                llog_ctxt_put(ctxt);
                        }
                        rc = 0;
                }
                RETURN(rc);
        }

        if (op->lop_setup) {
                if (OBD_FAIL_CHECK(OBD_FAIL_OBD_LLOG_SETUP))
                        rc = -EOPNOTSUPP;
                else
                        rc = op->lop_setup(env, obd, olg, index, disk_obd, count,
                                           logid, logname);
        }

        if (rc) {
                CERROR("obd %s ctxt %d lop_setup=%p failed %d\n",
                       obd->obd_name, index, op->lop_setup, rc);
                llog_group_clear_ctxt(olg, index);
                llog_ctxt_destroy(ctxt);
        } else {
                CDEBUG(D_CONFIG, "obd %s ctxt %d is initialized\n",
                       obd->obd_name, index);
                ctxt->loc_flags &= ~LLOG_CTXT_FLAG_UNINITIALIZED;
        }

        RETURN(rc);
}
EXPORT_SYMBOL(llog_setup_named);

int llog_setup(const struct lu_env *env, struct obd_device *obd,
               struct obd_llog_group *olg, int index,
               struct obd_device *disk_obd, int count,
               struct llog_logid *logid, struct llog_operations *op)
{
        return llog_setup_named(env, obd, olg, index, disk_obd, count, logid,
                                NULL, op);
}
EXPORT_SYMBOL(llog_setup);

int llog_sync(struct llog_ctxt *ctxt, struct obd_export *exp)
{
        int rc = 0;
        ENTRY;

        if (!ctxt)
                RETURN(0);

        if (CTXTP(ctxt, sync))
                rc = CTXTP(ctxt, sync)(ctxt, exp);

        RETURN(rc);
}
EXPORT_SYMBOL(llog_sync);

int llog_cancel(const struct lu_env *env, struct llog_ctxt *ctxt,
                int count, struct llog_cookie *cookies, int flags)
{
        int rc;
        ENTRY;

        if (!ctxt) {
                //CERROR("No ctxt\n");
                RETURN(-ENODEV);
        }

        CTXT_CHECK_OP(ctxt, cancel, -EOPNOTSUPP);
        rc = CTXTP(ctxt, cancel)(env, ctxt, count, cookies, flags);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_cancel);

/* callback func for llog_process in llog_obd_origin_setup */
static int cat_cancel_cb(const struct lu_env *env, struct llog_handle *cathandle,
                          struct llog_rec_hdr *rec, void *data)
{
        struct llog_logid_rec *lir = (struct llog_logid_rec *)rec;
        struct llog_handle *loghandle;
        struct llog_log_hdr *llh;
        int rc, index;
        ENTRY;

        if (rec->lrh_type != LLOG_LOGID_MAGIC) {
                CERROR("invalid record in catalog\n");
                RETURN(-EINVAL);
        }
        CDEBUG(D_HA, "processing log "LPX64":%x at index %u of catalog "
               LPX64"\n", lir->lid_id.lgl_oid, lir->lid_id.lgl_ogen,
               rec->lrh_index, cathandle->lgh_id.lgl_oid);

        rc = llog_cat_id2handle(env, cathandle, &loghandle, &lir->lid_id);
        if (rc) {
                CERROR("Cannot find handle for log "LPX64"\n",
                       lir->lid_id.lgl_oid);
                if (rc == -ENOENT) {
                        index = rec->lrh_index;
                        goto cat_cleanup;
                }
                RETURN(rc);
        }

        llh = loghandle->lgh_hdr;
        if ((llh->llh_flags & LLOG_F_ZAP_WHEN_EMPTY) &&
            (llh->llh_count == 1)) {
                rc = llog_destroy(env, loghandle);
                if (rc)
                        CERROR("failure destroying log in postsetup: %d\n", rc);

                index = loghandle->u.phd.phd_cookie.lgc_index;
                llog_close(env, loghandle);

cat_cleanup:
                LASSERT(index);
                llog_cat_set_first_idx(cathandle, index);
                rc = llog_cancel_rec(env, cathandle, index);
                if (rc == 0)
                        CDEBUG(D_HA, "cancel log "LPX64":%x at index %u of catalog "
                              LPX64"\n", lir->lid_id.lgl_oid,
                              lir->lid_id.lgl_ogen, rec->lrh_index,
                              cathandle->lgh_id.lgl_oid);
        }

        RETURN(rc);
}

/* lop_setup method for filter/osc */
// XXX how to set exports
int llog_obd_origin_setup(const struct lu_env *env, struct obd_device *obd,
                          struct obd_llog_group *olg, int index,
                          struct obd_device *disk_obd, int count,
                          struct llog_logid *logid, const char *name)
{
        struct llog_ctxt   *ctxt;
        struct llog_handle *handle;
        int rc;
        ENTRY;

        if (count == 0)
                RETURN(0);

        LASSERT(count == 1);
        LASSERT(olg != NULL);
        ctxt = llog_group_get_ctxt(olg, index);
        if (!ctxt)
                RETURN(rc = -ENODEV);

        if (logid && logid->lgl_oid) {
                rc = llog_open_create(env, ctxt, &handle, logid, NULL);
        } else {
                rc = llog_open_create(env, ctxt, &handle, NULL, (char *)name);
                if (rc == 0 && logid)
                        *logid = handle->lgh_id;
        }
        if (rc)
                GOTO(out, rc);

        ctxt->loc_handle = handle;
        rc = llog_init_handle(env, handle, LLOG_F_IS_CAT, NULL);
        if (rc)
                GOTO(out, rc);

        rc = llog_process(env, handle, (llog_cb_t)cat_cancel_cb, NULL, NULL);
        if (rc)
                CERROR("llog_process() with cat_cancel_cb failed: %d\n", rc);
        GOTO(out, rc);
out:
        llog_ctxt_put(ctxt);
        return rc;
}
EXPORT_SYMBOL(llog_obd_origin_setup);

int llog_obd_origin_cleanup(const struct lu_env *env, struct llog_ctxt *ctxt)
{
        ENTRY;

        if (!ctxt)
                RETURN(0);

        if (ctxt->loc_handle)
                llog_cat_close(env, ctxt->loc_handle);
        RETURN(0);
}
EXPORT_SYMBOL(llog_obd_origin_cleanup);

/* add for obdfilter/sz and mds/unlink */
int llog_obd_origin_add(const struct lu_env *env, struct llog_ctxt *ctxt,
                        struct llog_rec_hdr *rec, struct lov_stripe_md *lsm,
                        struct llog_cookie *logcookies, int numcookies,
                        struct thandle *th)
{
        struct llog_handle *cathandle;
        int rc;
        ENTRY;

        cathandle = ctxt->loc_handle;
        LASSERT(cathandle != NULL);
        rc = llog_cat_add_rec(env, cathandle, rec, logcookies, NULL, th);
        if (rc != 0 && rc != 1)
                CERROR("write one catalog record failed: %d\n", rc);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_obd_origin_add);

int llog_obd_origin_declare_add(const struct lu_env *env, struct llog_ctxt *ctxt,
                                struct llog_rec_hdr *rec, struct lov_stripe_md *lsm,
                                struct thandle *th)
{
        struct llog_handle *cathandle;
        int rc;
        ENTRY;

        LASSERT(ctxt);
        cathandle = ctxt->loc_handle;
        LASSERT(cathandle != NULL);
        rc = llog_cat_declare_add_rec(env, cathandle, rec, th);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_obd_origin_declare_add);

/* context key constructor/destructor: tg_key_init, tg_key_fini */
LU_KEY_INIT_FINI(llog, struct llog_thread_info);

/* context key: tg_thread_key */
LU_CONTEXT_KEY_DEFINE(llog, LCT_MD_THREAD | LCT_MG_THREAD | LCT_LOCAL);

LU_KEY_INIT_GENERIC(llog);
EXPORT_SYMBOL(llog_thread_key);

int llog_info_init(void)
{
        llog_key_init_generic(&llog_thread_key, NULL);
        lu_context_key_register(&llog_thread_key);
        return 0;
}

void llog_info_fini(void)
{
        lu_context_key_degister(&llog_thread_key);
}
