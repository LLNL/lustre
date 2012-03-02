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
 * lustre/obdclass/llog_cat.c
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

#ifndef __KERNEL__
#include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_log.h>
#include <libcfs/list.h>

/* Create a new log handle and add it to the open list.
 * This log handle will be closed when all of the records in it are removed.
 *
 * Assumes caller has already pushed us into the kernel context and is locking.
 */
static int llog_cat_new_log(const struct lu_env *env,
                            struct llog_handle *cathandle,
                            struct llog_handle *loghandle, 
                            struct thandle *th)
{
        struct llog_log_hdr *llh;
        struct llog_logid_rec rec = { { 0 }, };
        int rc, index, bitmap_size;
        ENTRY;

        llh = cathandle->lgh_hdr;
        bitmap_size = LLOG_BITMAP_SIZE(llh);

        index = (cathandle->lgh_last_idx + 1) % bitmap_size;

        /* maximum number of available slots in catlog is bitmap_size - 2 */
        if (llh->llh_cat_idx == index) {
                CERROR("no free catalog slots for log...\n");
                RETURN(-ENOSPC);
        }

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_LLOG_CREATE_FAILED))
                RETURN(-ENOSPC);

        rc = llog_create_2(env, loghandle, th);
        /* if llog is already created, no need to initialize it */
        if (rc == -EEXIST)
                RETURN(0);
        if (rc) {
                CERROR("can't create new plain llog: %d\n", rc);
                RETURN(rc);
        }

        rc = llog_init_handle(loghandle,
                              LLOG_F_IS_PLAIN | LLOG_F_ZAP_WHEN_EMPTY,
                              &cathandle->lgh_hdr->llh_tgtuuid);
        if (rc)
                GOTO(out_destroy, rc);

        if (index == 0)
                index = 1;
        if (ext2_set_bit(index, llh->llh_bitmap)) {
                CERROR("argh, index %u already set in log bitmap?\n",
                       index);
                LBUG(); /* should never happen */
        }
        cathandle->lgh_last_idx = index;
        llh->llh_count++;
        llh->llh_tail.lrt_index = index;

        CDEBUG(D_RPCTRACE,"new recovery log "LPX64":%x for index %u of catalog "
               LPX64"\n", loghandle->lgh_id.lgl_oid, loghandle->lgh_id.lgl_ogen,
               index, cathandle->lgh_id.lgl_oid);
        /* build the record for this log in the catalog */
        rec.lid_hdr.lrh_len = sizeof(rec);
        rec.lid_hdr.lrh_index = index;
        rec.lid_hdr.lrh_type = LLOG_LOGID_MAGIC;
        rec.lid_id = loghandle->lgh_id;
        rec.lid_tail.lrt_len = sizeof(rec);
        rec.lid_tail.lrt_index = index;

        /* update the catalog: header and record */
        rc = llog_write_rec_2(env, cathandle, &rec.lid_hdr,
                              &loghandle->u.phd.phd_cookie, 1, NULL, index, th);
        if (rc < 0) 
                GOTO(out_destroy, rc);

        loghandle->lgh_hdr->llh_cat_idx = index;
        LASSERT(cfs_list_empty(&loghandle->u.phd.phd_entry));
        cfs_list_add_tail(&loghandle->u.phd.phd_entry,
                          &cathandle->u.chd.chd_head);

out_destroy:
        if (rc < 0)
                llog_destroy(env, loghandle);

        RETURN(rc);
}

/* Open an existent log handle and add it to the open list.
 * This log handle will be closed when all of the records in it are removed.
 *
 * Assumes caller has already pushed us into the kernel context and is locking.
 * We return a lock on the handle to ensure nobody yanks it from us.
 */
int llog_cat_id2handle(const struct lu_env *env, struct llog_handle *cathandle,
                       struct llog_handle **res, struct llog_logid *logid)
{
        struct llog_handle *loghandle;
        int rc = 0;
        ENTRY;

        LASSERT(env);

        if (cathandle == NULL)
                RETURN(-EBADF);

        cfs_list_for_each_entry(loghandle, &cathandle->u.chd.chd_head,
                                u.phd.phd_entry) {
                struct llog_logid *cgl = &loghandle->lgh_id;
                if (cgl->lgl_oid == logid->lgl_oid) {
                        if (cgl->lgl_ogen != logid->lgl_ogen) {
                                CERROR("log "LPX64" generation %x != %x\n",
                                       logid->lgl_oid, cgl->lgl_ogen,
                                       logid->lgl_ogen);
                                continue;
                        }
                        loghandle->u.phd.phd_cat_handle = cathandle;
                        GOTO(out, rc = 0);
                }
        }

        rc = llog_open_2(env, cathandle->lgh_ctxt, &loghandle, logid, NULL);
        if (rc) {
                CERROR("error opening log id "LPX64":%x: rc %d\n",
                       logid->lgl_oid, logid->lgl_ogen, rc);
        } else {
                LASSERT(llog_exist_2(loghandle));
                rc = llog_init_handle(loghandle, LLOG_F_IS_PLAIN, NULL);
                if (!rc) {
                        cfs_list_add(&loghandle->u.phd.phd_entry,
                                     &cathandle->u.chd.chd_head);
                }
        }
        if (!rc) {
                loghandle->u.phd.phd_cat_handle = cathandle;
                loghandle->u.phd.phd_cookie.lgc_lgl = cathandle->lgh_id;
                loghandle->u.phd.phd_cookie.lgc_index =
                        loghandle->lgh_hdr->llh_cat_idx;
        }

out:
        *res = loghandle;
        RETURN(rc);
}

int llog_cat_put(struct llog_handle *cathandle)
{
        struct llog_handle *loghandle, *n;
        int rc;
        ENTRY;

        cfs_list_for_each_entry_safe(loghandle, n, &cathandle->u.chd.chd_head,
                                     u.phd.phd_entry) {
                /* unlink open-not-created llogs */
                cfs_list_del_init(&loghandle->u.phd.phd_entry);
                if (llog_close(loghandle))
                        CERROR("error closing loghandle\n");
        }
        rc = llog_close(cathandle);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_cat_put);

/**
 * lockdep markers for nested struct llog_handle::lgh_lock locking.
 */
enum {
        LLOGH_CAT,
        LLOGH_LOG
};

/** Return the currently active log handle.  If the current log handle doesn't
 * have enough space left for the current record, start a new one.
 *
 * If reclen is 0, we only want to know what the currently active log is,
 * otherwise we get a lock on this log so nobody can steal our space.
 *
 * Assumes caller has already pushed us into the kernel context and is locking.
 *
 * NOTE: loghandle is write-locked upon successful return
 */
static struct llog_handle *llog_cat_current_log(struct llog_handle *cathandle,
                                                int create,
                                                struct thandle *th)
{
        struct llog_handle *loghandle = NULL;
        ENTRY;

        cfs_down_read_nested(&cathandle->lgh_lock, LLOGH_CAT);
        loghandle = cathandle->u.chd.chd_current_log;
        if (loghandle) {
                struct llog_log_hdr *llh = loghandle->lgh_hdr;
                cfs_down_write_nested(&loghandle->lgh_lock, LLOGH_LOG);
                if (!llh || loghandle->lgh_last_idx < LLOG_BITMAP_SIZE(llh) - 1) {
                        cfs_up_read(&cathandle->lgh_lock);
                        RETURN(loghandle);
                } else {
                        cfs_up_write(&loghandle->lgh_lock);
                }
        }
        if (!create) {
                if (loghandle)
                        cfs_down_write(&loghandle->lgh_lock);
                cfs_up_read(&cathandle->lgh_lock);
                RETURN(loghandle);
        }
        cfs_up_read(&cathandle->lgh_lock);

        /* time to use next log */

        /* first, we have to make sure the state hasn't changed */
        cfs_down_write_nested(&cathandle->lgh_lock, LLOGH_CAT);
        loghandle = cathandle->u.chd.chd_current_log;
        if (loghandle) {
                struct llog_log_hdr *llh = loghandle->lgh_hdr;
                cfs_down_write_nested(&loghandle->lgh_lock, LLOGH_LOG);
                if (loghandle->lgh_last_idx < LLOG_BITMAP_SIZE(llh) - 1) {
                        cfs_up_write(&cathandle->lgh_lock);
                        RETURN(loghandle);
                } else {
                        cfs_up_write(&loghandle->lgh_lock);
                }
        }

        CDEBUG(D_INODE, "use next log\n");

        loghandle = cathandle->u.chd.chd_next_log;
        cathandle->u.chd.chd_current_log = loghandle;
        cathandle->u.chd.chd_next_log = NULL;
        cfs_down_write_nested(&loghandle->lgh_lock, LLOGH_LOG);
        cfs_up_write(&cathandle->lgh_lock);

        RETURN(loghandle);
}

/* Add a single record to the recovery log(s) using a catalog
 * Returns as llog_write_record
 *
 * Assumes caller has already pushed us into the kernel context.
 */
int llog_cat_add_rec_2(const struct lu_env *env, struct llog_handle *cathandle,
                       struct llog_rec_hdr *rec, struct llog_cookie *reccookie,
                       void *buf, struct thandle *th)
{
        struct llog_handle *loghandle;
        int rc, created = 0;
        ENTRY;

        LASSERT(th);
        LASSERT(rec->lrh_len <= LLOG_CHUNK_SIZE);

        loghandle = llog_cat_current_log(cathandle, 1, th);
        LASSERT(loghandle);
        if (IS_ERR(loghandle))
                RETURN(PTR_ERR(loghandle));

        /* loghandle is already locked by llog_cat_current_log() for us */
        if (!llog_exist_2(loghandle)) {
                cfs_list_del(&loghandle->u.phd.phd_entry);
                rc = llog_cat_new_log(env, cathandle, loghandle, th);
                LASSERTF(rc >= 0, "rc = %d\n", rc);
                created = 1;
        }

        /* now let's try to add the record */
        rc = llog_write_rec_2(env, loghandle, rec, reccookie, 1, buf, -1, th);
        if (rc < 0)
                CERROR("llog_write_rec %d: lh=%p\n", rc, loghandle);
        cfs_up_write(&loghandle->lgh_lock);

        if (rc == -ENOSPC) {
                /* try to use next log */
                loghandle = llog_cat_current_log(cathandle, 1, th);
                LASSERT(loghandle);
                if (IS_ERR(loghandle))
                        GOTO(out, rc = PTR_ERR(loghandle));
                cfs_list_del(&loghandle->u.phd.phd_entry);
                rc = llog_cat_new_log(env, cathandle, loghandle, th);
                LASSERT(rc == 0);
                /* now let's try to add the record */
                rc = llog_write_rec_2(env, loghandle, rec, reccookie, 1, buf, -1, th);
                if (rc < 0)
                        CERROR("llog_write_rec %d: lh=%p\n", rc, loghandle);
                cfs_up_write(&loghandle->lgh_lock);
        } else if (0 && !created && rc >= 0 && cathandle->u.chd.chd_next_log != NULL) {
                /* create next llog ahead */
                cfs_down_write_nested(&cathandle->lgh_lock, LLOGH_CAT);
                loghandle = cathandle->u.chd.chd_next_log;
                if (loghandle && !llog_exist_2(loghandle)) {
                        cfs_list_del_init(&loghandle->u.phd.phd_entry);
                        llog_cat_new_log(env, cathandle, loghandle, th);
                }
                cfs_up_write(&cathandle->lgh_lock);
        }

out:
        RETURN(rc);
}
EXPORT_SYMBOL(llog_cat_add_rec_2);

int llog_cat_declare_add_rec(const struct lu_env *env,
                             struct llog_handle *cathandle,
                             struct llog_rec_hdr *rec,
                             struct thandle *th)
{

        struct llog_handle *loghandle, *next;
        int rc = 0;
        ENTRY;

        LASSERT(th);

        if (cathandle->u.chd.chd_current_log == NULL) {
                /* declare new plain llog */
                cfs_down_write(&cathandle->lgh_lock);
                if (cathandle->u.chd.chd_current_log == NULL) {
                        rc = llog_open_2(env, cathandle->lgh_ctxt, &loghandle,
                                         NULL, NULL);
                        if (rc == 0) {
                                cathandle->u.chd.chd_current_log = loghandle;
                                cfs_list_add_tail(&loghandle->u.phd.phd_entry,
                                                  &cathandle->u.chd.chd_head);
                        }
                }
                cfs_up_write(&cathandle->lgh_lock);
        
        } else if (cathandle->u.chd.chd_next_log == NULL) {
                        /* declare next plain llog */
                cfs_down_write(&cathandle->lgh_lock);
                if (cathandle->u.chd.chd_next_log == NULL) {
                        rc = llog_open_2(env, cathandle->lgh_ctxt, &loghandle,
                                         NULL, NULL);
                        if (rc == 0) {
                                cathandle->u.chd.chd_next_log = loghandle;
                                cfs_list_add_tail(&loghandle->u.phd.phd_entry,
                                                  &cathandle->u.chd.chd_head);
                        }
                }
                cfs_up_write(&cathandle->lgh_lock);
        }

        if (rc)
                GOTO(out, rc);

        if (!llog_exist_2(cathandle->u.chd.chd_current_log)) {
                rc = llog_declare_create_2(env, cathandle->u.chd.chd_current_log, th);
                llog_declare_write_rec_2(env, cathandle, NULL, -1, th);
        }

        next = cathandle->u.chd.chd_next_log;
        if (next && !llog_exist_2(next)) {
                rc = llog_declare_create_2(env, next, th);
                llog_declare_write_rec_2(env, cathandle, NULL, -1, th);
        }

        /* declare records in the llogs */
        rc = llog_declare_write_rec_2(env, cathandle->u.chd.chd_current_log,
                                      rec, -1, th);
        if (cathandle->u.chd.chd_next_log &&
            llog_exist_2(cathandle->u.chd.chd_next_log))
                llog_declare_write_rec_2(env, cathandle->u.chd.chd_next_log, rec,-1,th);

out:
        RETURN(rc);
}
EXPORT_SYMBOL(llog_cat_declare_add_rec);

int llog_cat_add_rec(struct llog_handle *cathandle, struct llog_rec_hdr *rec,
                       struct llog_cookie *reccookie, void *buf)
{
        struct lu_env      env;
        struct llog_ctxt  *ctxt;
        struct dt_device  *dt;
        struct thandle    *th;
        int                rc;
        
        ctxt = cathandle->lgh_ctxt;
        LASSERT(ctxt);
        LASSERT(ctxt->loc_exp);
        
        dt = ctxt->loc_exp->exp_obd->obd_lvfs_ctxt.dt;
        LASSERT(dt);
        
        rc = lu_env_init(&env, dt->dd_lu_dev.ld_type->ldt_ctx_tags);
        if (rc) {
                CERROR("can't initialize env: %d\n", rc);
                GOTO(out, rc);
        }
        
        th = dt_trans_create(&env, dt);
        if (IS_ERR(th))
                GOTO(out, rc = PTR_ERR(th));

        rc = llog_cat_declare_add_rec(&env, cathandle, rec, th);
        if (rc)
                GOTO(out_trans, rc);

        rc = dt_trans_start(&env, dt, th);
        if (rc == 0)
                rc = llog_cat_add_rec_2(&env, cathandle, rec, reccookie, buf, th);

out_trans:
        dt_trans_stop(&env, dt, th);
        
out:
        lu_env_fini(&env);

        RETURN(rc);
}
EXPORT_SYMBOL(llog_cat_add_rec);

/* For each cookie in the cookie array, we clear the log in-use bit and either:
 * - the log is empty, so mark it free in the catalog header and delete it
 * - the log is not empty, just write out the log header
 *
 * The cookies may be in different log files, so we need to get new logs
 * each time.
 *
 * Assumes caller has already pushed us into the kernel context.
 */
int llog_cat_cancel_records_2(const struct lu_env *env, struct llog_handle *cathandle,
                              int count, struct llog_cookie *cookies)
{
        int i, index, rc = 0;
        ENTRY;

        cfs_down_write_nested(&cathandle->lgh_lock, LLOGH_CAT);
        for (i = 0; i < count; i++, cookies++) {
                struct llog_handle *loghandle;
                struct llog_logid *lgl = &cookies->lgc_lgl;

                rc = llog_cat_id2handle(env, cathandle, &loghandle, lgl);
                if (rc) {
                        CERROR("Cannot find log "LPX64"\n", lgl->lgl_oid);
                        break;
                }

                cfs_down_write_nested(&loghandle->lgh_lock, LLOGH_LOG);
                rc = llog_cancel_rec(env, loghandle, cookies->lgc_index);
                cfs_up_write(&loghandle->lgh_lock);

                if (rc == 1) {          /* log has been destroyed */
                        index = loghandle->u.phd.phd_cookie.lgc_index;
                        if (cathandle->u.chd.chd_current_log == loghandle)
                                cathandle->u.chd.chd_current_log = NULL;
                        llog_free_handle(loghandle);

                        LASSERT(index);
                        llog_cat_set_first_idx(cathandle, index);
                        rc = llog_cancel_rec(env, cathandle, index);
                        if (rc == 0)
                                CDEBUG(D_RPCTRACE,"cancel plain log at index %u"
                                       " of catalog "LPX64"\n",
                                       index, cathandle->lgh_id.lgl_oid);
                }
        }
        cfs_up_write(&cathandle->lgh_lock);

        RETURN(rc);
}
EXPORT_SYMBOL(llog_cat_cancel_records_2);

int llog_cat_cancel_records(struct llog_handle *cathandle, int count,
                            struct llog_cookie *cookies)
{
        struct llog_ctxt *ctxt;
        struct dt_device *dt;
        struct lu_env     env;
        int               rc;

        ctxt = cathandle->lgh_ctxt;
        LASSERT(ctxt);
        LASSERT(ctxt->loc_exp);

        dt = ctxt->loc_exp->exp_obd->obd_lvfs_ctxt.dt;
        LASSERT(dt);

        rc = lu_env_init(&env, dt->dd_lu_dev.ld_type->ldt_ctx_tags);
        if (rc) {
                CERROR("can't initialize env: %d\n", rc);
                GOTO(out, rc);
        }
        rc = llog_cat_cancel_records_2(&env, cathandle, count, cookies);

out:
        lu_env_fini(&env);

        return rc;
}
EXPORT_SYMBOL(llog_cat_cancel_records);

int llog_cat_process_cb(const struct lu_env *env, struct llog_handle *cat_llh,
                        struct llog_rec_hdr *rec, void *data)
{
        struct llog_process_data *d = data;
        struct llog_logid_rec *lir = (struct llog_logid_rec *)rec;
        struct llog_handle *llh;
        int rc;

        ENTRY;
        if (rec->lrh_type != LLOG_LOGID_MAGIC) {
                CERROR("invalid record in catalog\n");
                RETURN(-EINVAL);
        }
        CDEBUG(D_HA, "processing log "LPX64":%x at index %u of catalog "
               LPX64"\n", lir->lid_id.lgl_oid, lir->lid_id.lgl_ogen,
               rec->lrh_index, cat_llh->lgh_id.lgl_oid);

        rc = llog_cat_id2handle(env, cat_llh, &llh, &lir->lid_id);
        if (rc) {
                CERROR("Cannot find handle for log "LPX64"\n",
                       lir->lid_id.lgl_oid);
                RETURN(rc);
        }

        if (rec->lrh_index < d->lpd_startcat)
                /* Skip processing of the logs until startcat */
                RETURN(0);

        if (d->lpd_startidx > 0) {
                struct llog_process_cat_data cd;

                cd.lpcd_first_idx = d->lpd_startidx;
                cd.lpcd_last_idx = 0;
                rc = __llog_process(env, llh, d->lpd_cb, d->lpd_data, &cd, 0);
                /* Continue processing the next log from idx 0 */
                d->lpd_startidx = 0;
        } else {
                rc = __llog_process(env, llh, d->lpd_cb, d->lpd_data, NULL, 0);
        }

        RETURN(rc);
}

int __llog_cat_process(const struct lu_env *env, struct llog_handle *cat_llh,
                       llog_cb_t cb, void *data, int startcat, int startidx, int fork)
{
        struct llog_process_data d;
        struct llog_log_hdr *llh = cat_llh->lgh_hdr;
        int rc;
        ENTRY;

        LASSERT(env);
        LASSERT(llh->llh_flags & LLOG_F_IS_CAT);
        d.lpd_data = data;
        d.lpd_cb = cb;
        d.lpd_startcat = startcat;
        d.lpd_startidx = startidx;

        if (llh->llh_cat_idx > cat_llh->lgh_last_idx) {
                struct llog_process_cat_data cd;

                CWARN("catlog "LPX64" crosses index zero\n",
                      cat_llh->lgh_id.lgl_oid);

                cd.lpcd_first_idx = llh->llh_cat_idx;
                cd.lpcd_last_idx = 0;
                rc = __llog_process(env, cat_llh, llog_cat_process_cb, &d, &cd, fork);
                if (rc != 0)
                        RETURN(rc);

                cd.lpcd_first_idx = 0;
                cd.lpcd_last_idx = cat_llh->lgh_last_idx;
                rc = __llog_process(env, cat_llh, llog_cat_process_cb, &d, &cd, fork);
        } else {
                rc = __llog_process(env, cat_llh, llog_cat_process_cb, &d, NULL, fork);
        }

        RETURN(rc);
}
EXPORT_SYMBOL(__llog_cat_process);

int llog_cat_process(const struct lu_env *env, struct llog_handle *cat_llh,
                     llog_cb_t cb, void *data, int startcat, int startidx)
{
        return __llog_cat_process(env, cat_llh, cb, data, startcat, startidx, 1);
}
EXPORT_SYMBOL(llog_cat_process);

#ifdef __KERNEL__
int llog_cat_process_thread(void *data)
{
        struct llog_process_cat_args *args = data;
        struct llog_ctxt *ctxt = args->lpca_ctxt;
        struct llog_handle *llh = NULL;
        llog_cb_t cb = args->lpca_cb;
        struct llog_logid logid;
        struct dt_device   *dt = NULL;
        struct lu_env       env;
        int rc;
        ENTRY;

        cfs_daemonize_ctxt("ll_log_process");

        LASSERT(ctxt->loc_obd);
        if (ctxt->loc_obd->obd_lvfs_ctxt.dt) {
                dt = ctxt->loc_obd->obd_lvfs_ctxt.dt;
                rc = lu_env_init(&env, dt->dd_lu_dev.ld_type->ldt_ctx_tags);
                if (rc)
                        RETURN(rc);
        }

        logid = *(struct llog_logid *)(args->lpca_arg);
        rc = llog_create(ctxt, &llh, &logid, NULL);
        if (rc) {
                CERROR("llog_create() failed %d\n", rc);
                GOTO(out, rc);
        }
        rc = llog_init_handle(llh, LLOG_F_IS_CAT, NULL);
        if (rc) {
                CERROR("llog_init_handle failed %d\n", rc);
                GOTO(release_llh, rc);
        }

        if (cb) {
                rc = llog_cat_process(&env, llh, cb, NULL, 0, 0);
                if (rc != LLOG_PROC_BREAK && rc != 0)
                        CERROR("llog_cat_process() failed %d\n", rc);
                cb(&env, llh, NULL, NULL);
        } else {
                CWARN("No callback function for recovery\n");
        }

        /*
         * Make sure that all cached data is sent.
         */
        llog_sync(ctxt, NULL);
        GOTO(release_llh, rc);
release_llh:
        rc = llog_cat_put(llh);
        if (rc)
                CERROR("llog_cat_put() failed %d\n", rc);
out:
        llog_ctxt_put(ctxt);
        OBD_FREE_PTR(args);
        if (dt)
                lu_env_fini(&env);
        return rc;
}
EXPORT_SYMBOL(llog_cat_process_thread);
#endif

static int llog_cat_reverse_process_cb(const struct lu_env *env,
                                       struct llog_handle *cat_llh,
                                       struct llog_rec_hdr *rec, void *data)
{
        struct llog_process_data *d = data;
        struct llog_logid_rec *lir = (struct llog_logid_rec *)rec;
        struct llog_handle *llh;
        int rc;

        if (le32_to_cpu(rec->lrh_type) != LLOG_LOGID_MAGIC) {
                CERROR("invalid record in catalog\n");
                RETURN(-EINVAL);
        }
        CDEBUG(D_HA, "processing log "LPX64":%x at index %u of catalog "
               LPX64"\n", lir->lid_id.lgl_oid, lir->lid_id.lgl_ogen,
               le32_to_cpu(rec->lrh_index), cat_llh->lgh_id.lgl_oid);

        rc = llog_cat_id2handle(env, cat_llh, &llh, &lir->lid_id);
        if (rc) {
                CERROR("Cannot find handle for log "LPX64"\n",
                       lir->lid_id.lgl_oid);
                RETURN(rc);
        }

        rc = llog_reverse_process(env, llh, d->lpd_cb, d->lpd_data, NULL);
        RETURN(rc);
}

int llog_cat_reverse_process(struct llog_handle *cat_llh,
                             llog_cb_t cb, void *data)
{
        struct llog_process_data d;
        struct llog_process_cat_data cd;
        struct llog_log_hdr *llh = cat_llh->lgh_hdr;
        struct dt_device    *dt = NULL;
        struct lu_env        env;
        int rc;
        ENTRY;

        LASSERT(cat_llh->lgh_ctxt);
        LASSERT(cat_llh->lgh_ctxt->loc_obd);
        if (cat_llh->lgh_ctxt->loc_obd->obd_lvfs_ctxt.dt) {
                dt = cat_llh->lgh_ctxt->loc_obd->obd_lvfs_ctxt.dt;
                rc = lu_env_init(&env, dt->dd_lu_dev.ld_type->ldt_ctx_tags);
                if (rc)
                        RETURN(rc);
        }

        LASSERT(llh->llh_flags & LLOG_F_IS_CAT);
        d.lpd_data = data;
        d.lpd_cb = cb;

        if (llh->llh_cat_idx > cat_llh->lgh_last_idx) {
                CWARN("catalog "LPX64" crosses index zero\n",
                      cat_llh->lgh_id.lgl_oid);

                cd.lpcd_first_idx = 0;
                cd.lpcd_last_idx = cat_llh->lgh_last_idx;
                rc = llog_reverse_process(&env, cat_llh, llog_cat_reverse_process_cb,
                                          &d, &cd);
                if (rc != 0)
                        GOTO(out, rc);

                cd.lpcd_first_idx = le32_to_cpu(llh->llh_cat_idx);
                cd.lpcd_last_idx = 0;
                rc = llog_reverse_process(&env, cat_llh, llog_cat_reverse_process_cb,
                                          &d, &cd);
        } else {
                rc = llog_reverse_process(&env, cat_llh, llog_cat_reverse_process_cb,
                                          &d, NULL);
        }

out:
        if (dt)
                lu_env_fini(&env);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_cat_reverse_process);

int llog_cat_set_first_idx(struct llog_handle *cathandle, int index)
{
        struct llog_log_hdr *llh = cathandle->lgh_hdr;
        int i, bitmap_size, idx;
        ENTRY;

        bitmap_size = LLOG_BITMAP_SIZE(llh);
        if (llh->llh_cat_idx == (index - 1)) {
                idx = llh->llh_cat_idx + 1;
                llh->llh_cat_idx = idx;
                if (idx == cathandle->lgh_last_idx)
                        goto out;
                for (i = (index + 1) % bitmap_size;
                     i != cathandle->lgh_last_idx;
                     i = (i + 1) % bitmap_size) {
                        if (!ext2_test_bit(i, llh->llh_bitmap)) {
                                idx = llh->llh_cat_idx + 1;
                                llh->llh_cat_idx = idx;
                        } else if (i == 0) {
                                llh->llh_cat_idx = 0;
                        } else {
                                break;
                        }
                }
out:
                CDEBUG(D_RPCTRACE, "set catlog "LPX64" first idx %u\n",
                       cathandle->lgh_id.lgl_oid, llh->llh_cat_idx);
        }

        RETURN(0);
}
