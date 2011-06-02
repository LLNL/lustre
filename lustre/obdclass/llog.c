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
 * lustre/obdclass/llog.c
 *
 * OST<->MDS recovery logging infrastructure.
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
#include "llog_internal.h"

/* Allocate a new log or catalog handle */
struct llog_handle *llog_alloc_handle(void)
{
        struct llog_handle *loghandle;
        ENTRY;

        OBD_ALLOC(loghandle, sizeof(*loghandle));
        if (loghandle == NULL)
                RETURN(ERR_PTR(-ENOMEM));

        cfs_init_rwsem(&loghandle->lgh_lock);
        CFS_INIT_LIST_HEAD(&loghandle->u.phd.phd_entry);

        RETURN(loghandle);
}
EXPORT_SYMBOL(llog_alloc_handle);


void llog_free_handle(struct llog_handle *loghandle)
{
        if (!loghandle)
                return;

        if (!loghandle->lgh_hdr)
                goto out;
        if (loghandle->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN)
                cfs_list_del_init(&loghandle->u.phd.phd_entry);
        if (loghandle->lgh_hdr->llh_flags & LLOG_F_IS_CAT)
                LASSERT(cfs_list_empty(&loghandle->u.chd.chd_head));
        LASSERT(sizeof(*(loghandle->lgh_hdr)) == LLOG_CHUNK_SIZE);
        OBD_FREE(loghandle->lgh_hdr, LLOG_CHUNK_SIZE);
out:
        OBD_FREE(loghandle, sizeof(*loghandle));
}
EXPORT_SYMBOL(llog_free_handle);

/* returns negative on error; 0 if success; 1 if success & log destroyed */
int llog_cancel_rec(const struct lu_env *env, struct llog_handle *loghandle, int index)
{
        struct llog_log_hdr *llh = loghandle->lgh_hdr;
        int rc = 0;
        ENTRY;

        CDEBUG(D_RPCTRACE, "Canceling %d in log "LPX64"\n",
               index, loghandle->lgh_id.lgl_oid);

        if (index == 0) {
                CERROR("Can't cancel index 0 which is header\n");
                RETURN(-EINVAL);
        }

        if (!ext2_clear_bit(index, llh->llh_bitmap)) {
                CDEBUG(D_RPCTRACE, "Catalog index %u already clear?\n", index);
                RETURN(-ENOENT);
        }

        llh->llh_count--;

        if ((llh->llh_flags & LLOG_F_ZAP_WHEN_EMPTY) &&
            (llh->llh_count == 1) &&
            (loghandle->lgh_last_idx == (LLOG_BITMAP_BYTES * 8) - 1)) {
                rc = llog_destroy(env, loghandle);
                if (rc) {
                        CERROR("Failure destroying log after last cancel: %d\n",
                               rc);
                        ext2_set_bit(index, llh->llh_bitmap);
                        llh->llh_count++;
                } else {
                        rc = 1;
                }
                RETURN(rc);
        }

        /*rc = llog_write_rec(env, loghandle, &llh->llh_hdr, NULL, 0, NULL, 0);
        if (rc) {
                CERROR("Failure re-writing header %d\n", rc);
                ext2_set_bit(index, llh->llh_bitmap);
                llh->llh_count++;
        }*/
        RETURN(rc);
}
EXPORT_SYMBOL(llog_cancel_rec);

int llog_init_handle(struct llog_handle *handle, int flags,
                     struct obd_uuid *uuid)
{
        int rc;
        struct llog_log_hdr *llh;
        ENTRY;
        LASSERT(handle->lgh_hdr == NULL);

        OBD_ALLOC_PTR(llh);
        if (llh == NULL)
                RETURN(-ENOMEM);
        handle->lgh_hdr = llh;
        /* first assign flags to use llog_client_ops */
        llh->llh_flags = flags;
        rc = llog_read_header(handle);
        if (rc == 0) {
                flags = llh->llh_flags;
                if (uuid && !obd_uuid_equals(uuid, &llh->llh_tgtuuid)) {
                        CERROR("uuid mismatch: %s/%s\n", (char *)uuid->uuid,
                               (char *)llh->llh_tgtuuid.uuid);
                        rc = -EEXIST;
                }
                GOTO(out, rc);
        } else if (rc != LLOG_EEMPTY || !flags) {
                /* set a pesudo flag for initialization */
                flags = LLOG_F_IS_CAT;
                GOTO(out, rc);
        }
        rc = 0;

        handle->lgh_last_idx = 0; /* header is record with index 0 */
        llh->llh_count = 1;         /* for the header record */
        llh->llh_hdr.lrh_type = LLOG_HDR_MAGIC;
        llh->llh_hdr.lrh_len = llh->llh_tail.lrt_len = LLOG_CHUNK_SIZE;
        llh->llh_hdr.lrh_index = llh->llh_tail.lrt_index = 0;
        llh->llh_timestamp = cfs_time_current_sec();
        if (uuid)
                memcpy(&llh->llh_tgtuuid, uuid, sizeof(llh->llh_tgtuuid));
        llh->llh_bitmap_offset = offsetof(typeof(*llh),llh_bitmap);
        ext2_set_bit(0, llh->llh_bitmap);

out:
        if (flags & LLOG_F_IS_CAT) {
                LASSERT(cfs_list_empty(&handle->u.chd.chd_head));
                CFS_INIT_LIST_HEAD(&handle->u.chd.chd_head);
                llh->llh_size = sizeof(struct llog_logid_rec);
        } else if (!(flags & LLOG_F_IS_PLAIN)) {
                CERROR("Unknown flags: %#x (Expected %#x or %#x\n",
                       flags, LLOG_F_IS_CAT, LLOG_F_IS_PLAIN);
                LBUG();
        }

        if (rc) {
                OBD_FREE_PTR(llh);
                handle->lgh_hdr = NULL;
        }
        RETURN(rc);
}
EXPORT_SYMBOL(llog_init_handle);

static int __llog_process_thread(void *arg)
{
        struct llog_process_info     *lpi = (struct llog_process_info *)arg;
        struct llog_handle           *loghandle = lpi->lpi_loghandle;
        struct llog_log_hdr          *llh = loghandle->lgh_hdr;
        struct llog_process_cat_data *cd  = lpi->lpi_catdata;
        struct dt_device             *dt = NULL;
        const struct lu_env          *env;
        struct lu_env                 _env;
        char                         *buf;
        __u64                         cur_offset = LLOG_CHUNK_SIZE;
        __u64                         last_offset;
        int                           rc = 0, index = 1, last_index;
        int                           saved_index = 0, last_called_index = 0;

        LASSERT(llh);

        OBD_ALLOC(buf, LLOG_CHUNK_SIZE);
        if (!buf) {
                lpi->lpi_rc = -ENOMEM;
                return 0;
        }

        LASSERT(loghandle->lgh_ctxt);
        LASSERT(loghandle->lgh_ctxt->loc_obd);
        env = lpi->lpi_env;
        if (env == NULL && loghandle->lgh_ctxt->loc_obd->obd_lvfs_ctxt.dt) {
                dt = loghandle->lgh_ctxt->loc_obd->obd_lvfs_ctxt.dt;
                rc = lu_env_init(&_env, dt->dd_lu_dev.ld_type->ldt_ctx_tags);
                if (rc)
                        GOTO(out, rc);
                env = &_env;
        }

        if (cd != NULL) {
                last_called_index = cd->lpcd_first_idx;
                index = cd->lpcd_first_idx + 1;
        }
        if (cd != NULL && cd->lpcd_last_idx)
                last_index = cd->lpcd_last_idx;
        else
                last_index = LLOG_BITMAP_BYTES * 8 - 1;

        while (rc == 0) {
                struct llog_rec_hdr *rec;

                /* skip records not set in bitmap */
                while (index <= last_index &&
                       !ext2_test_bit(index, llh->llh_bitmap))
                        ++index;

                LASSERT(index <= last_index + 1);
                if (index == last_index + 1)
                        break;

repeat:
                CDEBUG(D_OTHER, "index: %d last_index %d\n",
                       index, last_index);

                /* get the buf with our target record; avoid old garbage */
                memset(buf, 0, LLOG_CHUNK_SIZE);
                last_offset = cur_offset;
                rc = llog_next_block(env, loghandle, &saved_index, index,
                                     &cur_offset, buf, LLOG_CHUNK_SIZE);
                if (rc)
                        GOTO(out, rc);

                /* NB: when rec->lrh_len is accessed it is already swabbed
                 * since it is used at the "end" of the loop and the rec
                 * swabbing is done at the beginning of the loop. */
                for (rec = (struct llog_rec_hdr *)buf;
                     (char *)rec < buf + LLOG_CHUNK_SIZE;
                     rec = (struct llog_rec_hdr *)((char *)rec + rec->lrh_len)){

                        CDEBUG(D_OTHER, "processing rec 0x%p type %#x\n",
                               rec, rec->lrh_type);

                        if (LLOG_REC_HDR_NEEDS_SWABBING(rec))
                                lustre_swab_llog_rec(rec, NULL);

                        CDEBUG(D_OTHER, "after swabbing, type=%#x idx=%d\n",
                               rec->lrh_type, rec->lrh_index);

                        if (rec->lrh_index == 0) {
                                /* probably another rec just got added? */
                                if (index <= loghandle->lgh_last_idx)
                                        GOTO(repeat, rc = 0);
                                GOTO(out, 0); /* no more records */
                        }

                        if (rec->lrh_len == 0 || rec->lrh_len >LLOG_CHUNK_SIZE){
                                CWARN("invalid length %d in llog record for "
                                      "index %d/%d\n", rec->lrh_len,
                                      rec->lrh_index, index);
                                GOTO(out, rc = -EINVAL);
                        }

                        if (rec->lrh_index < index) {
                                CDEBUG(D_OTHER, "skipping lrh_index %d\n",
                                       rec->lrh_index);
                                continue;
                        }

                        CDEBUG(D_OTHER,
                               "lrh_index: %d lrh_len: %d (%d remains)\n",
                               rec->lrh_index, rec->lrh_len,
                               (int)(buf + LLOG_CHUNK_SIZE - (char *)rec));

                        loghandle->lgh_cur_idx = rec->lrh_index;
                        loghandle->lgh_cur_offset = (char *)rec - (char *)buf +
                                                    last_offset;

                        /* if set, process the callback on this record */
                        if (ext2_test_bit(index, llh->llh_bitmap)) {
                                rc = lpi->lpi_cb(env, loghandle, rec,
                                                 lpi->lpi_cbdata);
                                last_called_index = index;
                                if (rc == LLOG_PROC_BREAK) {
                                        GOTO(out, rc);
                                } else if (rc == LLOG_DEL_RECORD) {
                                        llog_cancel_rec(env, loghandle,
                                                        rec->lrh_index);
                                        rc = 0;
                                }
                                if (rc)
                                        GOTO(out, rc);
                        } else {
                                CDEBUG(D_OTHER, "Skipped index %d\n", index);
                        }

                        /* next record, still in buffer? */
                        ++index;
                        if (index > last_index)
                                GOTO(out, rc = 0);
                }
        }

 out:
        if (cd != NULL)
                cd->lpcd_last_idx = last_called_index;
        if (buf)
                OBD_FREE(buf, LLOG_CHUNK_SIZE);
        if (dt)
                lu_env_fini(&_env);
        lpi->lpi_rc = rc;
        return 0;
}

#ifdef __KERNEL__
static int llog_process_thread(void *arg)
{
        struct llog_process_info *lpi = (struct llog_process_info *)arg;
        int                       rc;

        cfs_daemonize_ctxt("llog_process_thread");

        rc = __llog_process_thread(arg);

        cfs_complete(&lpi->lpi_completion);
        return rc;
}
#endif

int __llog_process(const struct lu_env *env, struct llog_handle *loghandle,
                   llog_cb_t cb, void *data, void *catdata, int fork)
{
        struct llog_process_info *lpi;
        int                      rc;
        ENTRY;

        OBD_ALLOC_PTR(lpi);
        if (lpi == NULL) {
                CERROR("cannot alloc pointer\n");
                RETURN(-ENOMEM);
        }
        lpi->lpi_loghandle = loghandle;
        lpi->lpi_cb        = cb;
        lpi->lpi_cbdata    = data;
        lpi->lpi_catdata   = catdata;
        lpi->lpi_env       = env;

#ifdef __KERNEL__
        if (fork) {
                cfs_init_completion(&lpi->lpi_completion);
                rc = cfs_create_thread(llog_process_thread, lpi,
                                       CFS_DAEMON_FLAGS);
                if (rc < 0) {
                        CERROR("cannot start thread: %d\n", rc);
                        OBD_FREE_PTR(lpi);
                        RETURN(rc);
                }
                cfs_wait_for_completion(&lpi->lpi_completion);
        } else
                __llog_process_thread(lpi);
#else
        __llog_process_thread(lpi);
#endif
        rc = lpi->lpi_rc;
        OBD_FREE_PTR(lpi);
        RETURN(rc);
}

int llog_process(struct llog_handle *loghandle, llog_cb_t cb,
                 void *data, void *catdata)
{
        return __llog_process(NULL, loghandle, cb, data, catdata, 1);
}
EXPORT_SYMBOL(llog_process);

int llog_process_2(const struct lu_env *env,
                   struct llog_handle *loghandle, llog_cb_t cb,
                   void *data, void *catdata)
{
        return __llog_process(env, loghandle, cb, data, catdata, 1);
}
EXPORT_SYMBOL(llog_process_2);

inline int llog_get_size(struct llog_handle *loghandle)
{
        if (loghandle && loghandle->lgh_hdr)
                return loghandle->lgh_hdr->llh_count;
        return 0;
}
EXPORT_SYMBOL(llog_get_size);

int llog_reverse_process(const struct lu_env *env,
                         struct llog_handle *loghandle, llog_cb_t cb,
                         void *data, void *catdata)
{
        struct llog_log_hdr *llh = loghandle->lgh_hdr;
        struct llog_process_cat_data *cd = catdata;
        void *buf;
        int rc = 0, first_index = 1, index, idx;
        ENTRY;

        OBD_ALLOC(buf, LLOG_CHUNK_SIZE);
        if (!buf)
                RETURN(-ENOMEM);

        if (cd != NULL)
                first_index = cd->lpcd_first_idx + 1;
        if (cd != NULL && cd->lpcd_last_idx)
                index = cd->lpcd_last_idx;
        else
                index = LLOG_BITMAP_BYTES * 8 - 1;

        while (rc == 0) {
                struct llog_rec_hdr *rec;
                struct llog_rec_tail *tail;

                /* skip records not set in bitmap */
                while (index >= first_index &&
                       !ext2_test_bit(index, llh->llh_bitmap))
                        --index;

                LASSERT(index >= first_index - 1);
                if (index == first_index - 1)
                        break;

                /* get the buf with our target record; avoid old garbage */
                memset(buf, 0, LLOG_CHUNK_SIZE);
                rc = llog_prev_block(env, loghandle, index, buf, LLOG_CHUNK_SIZE);
                if (rc)
                        GOTO(out, rc);

                rec = buf;
                idx = le32_to_cpu(rec->lrh_index);
                if (idx < index)
                        CDEBUG(D_RPCTRACE, "index %u : idx %u\n", index, idx);
                while (idx < index) {
                        rec = ((void *)rec + le32_to_cpu(rec->lrh_len));
                        idx ++;
                }
                tail = (void *)rec + le32_to_cpu(rec->lrh_len) - sizeof(*tail);

                /* process records in buffer, starting where we found one */
                while ((void *)tail > buf) {
                        rec = (void *)tail - le32_to_cpu(tail->lrt_len) +
                                sizeof(*tail);

                        if (rec->lrh_index == 0)
                                GOTO(out, 0); /* no more records */

                        /* if set, process the callback on this record */
                        if (ext2_test_bit(index, llh->llh_bitmap)) {
                                rc = cb(env, loghandle, rec, data);
                                if (rc == LLOG_PROC_BREAK) {
                                        GOTO(out, rc);
                                }
                                if (rc)
                                        GOTO(out, rc);
                        }

                        /* previous record, still in buffer? */
                        --index;
                        if (index < first_index)
                                GOTO(out, rc = 0);
                        tail = (void *)rec - sizeof(*tail);
                }
        }

out:
        if (buf)
                OBD_FREE(buf, LLOG_CHUNK_SIZE);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_reverse_process);

int llog_open_create(const struct lu_env *env, struct llog_ctxt *ctxt,
                     struct llog_handle **res, struct llog_logid *logid,
                     char *name)
{
        struct thandle    *th;
        int                rc;
        ENTRY;

        rc = llog_open_2(env, ctxt, res, logid, name);
        if (rc)
                RETURN(rc);

        if (!llog_exist_2(*res)) {
                struct dt_device *d;

                d = lu2dt_dev((*res)->lgh_obj->do_lu.lo_dev);

                th = dt_trans_create(env, d);
                if (IS_ERR(th))
                        GOTO(out, rc = PTR_ERR(th));

                rc = llog_declare_create_2(env, *res, th);
                if (rc == 0) {
                        rc = dt_trans_start_local(env, d, th);
                        if (rc == 0)
                                rc = llog_create_2(env, *res, th);
                }
                dt_trans_stop(env, d, th);
out:
                if (rc)
                        llog_close_2(env, *res);
        }
        RETURN(rc);
}
EXPORT_SYMBOL(llog_open_create);

int llog_erase(const struct lu_env *env, struct llog_ctxt *ctxt,
               struct llog_logid *logid, char *name)
{
        struct llog_handle *handle;
        int rc = 0;
        ENTRY;

        /* nothing to erase */
        if (name == NULL && logid == NULL)
                RETURN(0);

        rc = llog_open_2(env, ctxt, &handle, logid, name);
        if (rc)
                RETURN(rc);

        if (llog_exist_2(handle)) {
                rc = llog_init_handle(handle, LLOG_F_IS_PLAIN, NULL);
                if (rc == 0) {
                        rc = llog_destroy(env, handle);
                        llog_free_handle(handle);
                        RETURN(rc);
                }
        }
        llog_close_2(env, handle);
        RETURN(rc);
}
EXPORT_SYMBOL(llog_erase);
