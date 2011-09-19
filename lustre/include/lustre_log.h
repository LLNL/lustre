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
 * lustre/include/lustre_log.h
 *
 * Generic infrastructure for managing a collection of logs.
 * These logs are used for:
 *
 * - orphan recovery: OST adds record on create
 * - mtime/size consistency: the OST adds a record on first write
 * - open/unlinked objects: OST adds a record on destroy
 *
 * - mds unlink log: the MDS adds an entry upon delete
 *
 * - raid1 replication log between OST's
 * - MDS replication logs
 */

#ifndef _LUSTRE_LOG_H
#define _LUSTRE_LOG_H

/** \defgroup log log
 *
 * @{
 */

#if defined(__linux__)
#include <linux/lustre_log.h>
#elif defined(__APPLE__)
#include <darwin/lustre_log.h>
#elif defined(__WINNT__)
#include <winnt/lustre_log.h>
#else
#error Unsupported operating system.
#endif

#include <obd_class.h>
#include <obd_ost.h>
#include <lustre/lustre_idl.h>
#include <dt_object.h>

#define LOG_NAME_LIMIT(logname, name)                   \
        snprintf(logname, sizeof(logname), "LOGS/%s", name)
#define LLOG_EEMPTY 4711

struct plain_handle_data {
        cfs_list_t          phd_entry;
        struct llog_handle *phd_cat_handle;
        struct llog_cookie  phd_cookie; /* cookie of this log in its cat */
        int                 phd_last_idx;
};

struct cat_handle_data {
        cfs_list_t              chd_head;
        struct llog_handle     *chd_current_log; /* currently open log */
        struct llog_handle     *chd_next_log;    /* log to be used next */
};

/* In-memory descriptor for a log object or log catalog */
struct llog_handle {
        cfs_rw_semaphore_t      lgh_lock;
        struct llog_logid       lgh_id;              /* id of this log */
        struct llog_log_hdr    *lgh_hdr;
        union {
                struct file             *lgh_file;
                struct dt_object        *lgh_obj;
        } lgh_store;
        int                     lgh_last_idx;
        int                     lgh_cur_idx;    /* used during llog_process */
        __u64                   lgh_cur_offset; /* used during llog_process */
        struct llog_ctxt       *lgh_ctxt;
        union {
                struct plain_handle_data phd;
                struct cat_handle_data   chd;
        } u;
        void                   *private_data;
};

#define lgh_file        lgh_store.lgh_file
#define lgh_obj         lgh_store.lgh_obj

/* llog.c  -  general API */
typedef int (*llog_cb_t)(const struct lu_env *, struct llog_handle *,
                         struct llog_rec_hdr *, void *);
typedef int (*llog_fill_rec_cb_t)(struct llog_rec_hdr *rec, void *data);
extern struct llog_handle *llog_alloc_handle(void);
int llog_init_handle(struct llog_handle *handle, int flags,
                     struct obd_uuid *uuid);
extern void llog_free_handle(struct llog_handle *handle);
int __llog_process(const struct lu_env *, struct llog_handle *loghandle, llog_cb_t cb,
                   void *data, void *catdata, int fork);
int llog_process(struct llog_handle *loghandle, llog_cb_t cb,
                 void *data, void *catdata);
int llog_reverse_process(const struct lu_env *, struct llog_handle *, llog_cb_t,
                         void *, void *);
extern int llog_cancel_rec(const struct lu_env *, struct llog_handle *, int);
extern int llog_close(struct llog_handle *cathandle);
extern int llog_get_size(struct llog_handle *loghandle);

/* llog_cat.c - catalog api */
struct llog_process_data {
        /**
         * Any useful data needed while processing catalog. This is
         * passed later to process callback.
         */
        void                *lpd_data;
        /**
         * Catalog process callback function, called for each record
         * in catalog.
         */
        llog_cb_t            lpd_cb;
        /**
         * Start processing the catalog from startcat/startidx
         */
        int                  lpd_startcat;
        int                  lpd_startidx;
        int                  lpd_flags;  /** llog_process flags */
};

struct llog_process_cat_data {
        /**
         * Temporary stored first_idx while scanning log.
         */
        int                  lpcd_first_idx;
        /**
         * Temporary stored last_idx while scanning log.
         */
        int                  lpcd_last_idx;
};

struct llog_process_cat_args {
        /**
         * Llog context used in recovery thread on OST (recov_thread.c)
         */
        struct llog_ctxt    *lpca_ctxt;
        /**
         * Llog callback used in recovery thread on OST (recov_thread.c)
         */
        void                *lpca_cb;
        /**
         * Data pointer for llog callback.
         */
        void                *lpca_arg;
};

int llog_cat_put(struct llog_handle *cathandle);
int llog_cat_declare_add_rec(const struct lu_env *env,
                             struct llog_handle *cathandle,
                             struct llog_rec_hdr *rec,
                             struct thandle *);
int llog_cat_add_rec(struct llog_handle *, struct llog_rec_hdr *,
                     struct llog_cookie *, void *);
int llog_cat_add_rec_2(const struct lu_env *, struct llog_handle *,
                       struct llog_rec_hdr *, struct llog_cookie *, void *,
                       struct thandle *);
int llog_cat_cancel_records(struct llog_handle *cathandle, int count,
                            struct llog_cookie *cookies);
int llog_cat_cancel_records_2(const struct lu_env *, struct llog_handle *, int,
                            struct llog_cookie *cookies);
int __llog_cat_process(const struct lu_env *, struct llog_handle *cat_llh,
                       llog_cb_t cb, void *data, int startcat, int startidx, int fork);
int llog_cat_process(const struct lu_env *, struct llog_handle *, llog_cb_t, void *,
                     int, int);
int llog_cat_process_thread(void *data);
int llog_cat_reverse_process(struct llog_handle *cat_llh, llog_cb_t cb, void *data);
int llog_cat_set_first_idx(struct llog_handle *cathandle, int index);

/* llog_obd.c */
int llog_setup_named(struct obd_device *obd, struct obd_llog_group *olg,
                     int index, struct obd_device *disk_obd, int count,
                     struct llog_logid *logid, const char *logname,
                     struct llog_operations *op);
int llog_setup(struct obd_device *obd, struct obd_llog_group *olg, int index,
               struct obd_device *disk_obd, int count, struct llog_logid *logid,
               struct llog_operations *op);
int __llog_ctxt_put(struct llog_ctxt *ctxt);
int llog_cleanup(struct llog_ctxt *);
int llog_sync(struct llog_ctxt *ctxt, struct obd_export *exp);
int llog_add(struct llog_ctxt *ctxt, struct llog_rec_hdr *rec,
             struct lov_stripe_md *lsm, struct llog_cookie *logcookies,
             int numcookies);
int llog_cancel(struct llog_ctxt *, struct lov_stripe_md *lsm,
                int count, struct llog_cookie *cookies, int flags);

int llog_obd_origin_setup(struct obd_device *obd, struct obd_llog_group *olg,
                          int index, struct obd_device *disk_obd, int count,
                          struct llog_logid *logid, const char *name);
int llog_obd_origin_cleanup(struct llog_ctxt *ctxt);
int llog_obd_origin_declare_add(const struct lu_env *, struct llog_ctxt *ctxt,
                                struct llog_rec_hdr *rec,
                                struct lov_stripe_md *lsm,
                                struct thandle *th);
int llog_obd_origin_add(struct llog_ctxt *ctxt,
                        struct llog_rec_hdr *rec, struct lov_stripe_md *lsm,
                        struct llog_cookie *logcookies, int numcookies);
int llog_obd_origin_add_2(const struct lu_env *, struct llog_ctxt *ctxt,
                          struct llog_rec_hdr *rec, struct lov_stripe_md *lsm,
                          struct llog_cookie *logcookies, int numcookies,
                          struct thandle *th);

int obd_llog_init(struct obd_device *obd, struct obd_llog_group *olg,
                  struct obd_device *disk_obd, int *idx);

int obd_llog_finish(struct obd_device *obd, int count);

/* llog_ioctl.c */
int llog_ioctl(struct llog_ctxt *ctxt, int cmd, struct obd_ioctl_data *data);
int llog_catalog_list(struct obd_device *obd, int count,
                      struct obd_ioctl_data *data);

/* llog_net.c */
int llog_initiator_connect(struct llog_ctxt *ctxt);
int llog_receptor_accept(struct llog_ctxt *ctxt, struct obd_import *imp);
int llog_origin_connect(struct llog_ctxt *ctxt,
                        struct llog_logid *logid, struct llog_gen *gen,
                        struct obd_uuid *uuid);
int llog_handle_connect(struct ptlrpc_request *req);

/* recov_thread.c */
int llog_obd_repl_cancel(struct llog_ctxt *ctxt,
                         struct lov_stripe_md *lsm, int count,
                         struct llog_cookie *cookies, int flags);
int llog_obd_repl_sync(struct llog_ctxt *ctxt, struct obd_export *exp);
int llog_obd_repl_connect(struct llog_ctxt *ctxt,
                          struct llog_logid *logid, struct llog_gen *gen,
                          struct obd_uuid *uuid);

struct thandle;

struct llog_operations {
        int (*lop_write_rec)(struct llog_handle *loghandle,
                             struct llog_rec_hdr *rec,
                             struct llog_cookie *logcookies, int numcookies,
                             void *, int idx);
        int (*lop_destroy)(const struct lu_env *env, struct llog_handle *handle);
        int (*lop_next_block)(const struct lu_env *, struct llog_handle *, int *,
                              int next_idx, __u64 *offset, void *buf, int len);
        int (*lop_prev_block)(const struct lu_env *, struct llog_handle *,
                              int prev_idx, void *buf, int len);
        int (*lop_create)(struct llog_ctxt *ctxt, struct llog_handle **,
                          struct llog_logid *logid, char *name);
        int (*lop_close)(struct llog_handle *handle);
        int (*lop_read_header)(struct llog_handle *handle);

        int (*lop_setup)(struct obd_device *obd, struct obd_llog_group *olg,
                         int ctxt_idx, struct obd_device *disk_obd, int count,
                         struct llog_logid *logid, const char *name);
        int (*lop_sync)(struct llog_ctxt *ctxt, struct obd_export *exp);
        int (*lop_cleanup)(struct llog_ctxt *ctxt);
        int (*lop_add)(struct llog_ctxt *ctxt, struct llog_rec_hdr *rec,
                       struct lov_stripe_md *lsm,
                       struct llog_cookie *logcookies, int numcookies);
        int (*lop_cancel)(struct llog_ctxt *ctxt, struct lov_stripe_md *lsm,
                          int count, struct llog_cookie *cookies, int flags);
        int (*lop_connect)(struct llog_ctxt *ctxt,
                           struct llog_logid *logid, struct llog_gen *gen,
                           struct obd_uuid *uuid);

        /* XXX add 2 more: commit callbacks and llog recovery functions */

        /* new API */
        int (*lop_open_2)(const struct lu_env *, struct llog_ctxt *,
                          struct llog_handle **, struct llog_logid *, char *);
        int (*lop_exist_2)(struct llog_handle *);
        int (*lop_declare_create_2)(const struct lu_env *, struct llog_handle *,
                                    struct thandle *);
        int (*lop_create_2)(const struct lu_env *, struct llog_handle *, struct thandle *);
        int (*lop_declare_write_rec_2)(const struct lu_env *, struct llog_handle *,
                                       struct llog_rec_hdr *, int, struct thandle *);
        int (*lop_write_rec_2)(const struct lu_env *, struct llog_handle *,
                               struct llog_rec_hdr *, struct llog_cookie *, int,
                               void *, int, struct thandle *);
        int (*lop_declare_add_2)(const struct lu_env *, struct llog_ctxt *,
                                 struct llog_rec_hdr *, struct lov_stripe_md *,
                                 struct thandle *);
        int (*lop_add_2)(const struct lu_env *, struct llog_ctxt *, struct llog_rec_hdr *,
                         struct lov_stripe_md *, struct llog_cookie *,
                         int, struct thandle *);
};

/* llog_lvfs.c */
extern struct llog_operations llog_lvfs_ops;
extern struct llog_operations llog_osd_ops;
int llog_get_cat_list(struct obd_device *disk_obd,
                      char *name, int idx, int count,
                      struct llog_catid *idarray);

int llog_put_cat_list(struct obd_device *disk_obd,
                      char *name, int idx, int count, struct llog_catid *idarray);

#define LLOG_CTXT_FLAG_UNINITIALIZED     0x00000001

struct llog_ctxt {
        int                      loc_idx; /* my index the obd array of ctxt's */
        struct llog_gen          loc_gen;
        struct obd_device       *loc_obd; /* points back to the containing obd*/
        struct obd_llog_group   *loc_olg; /* group containing that ctxt */
        struct obd_export       *loc_exp; /* parent "disk" export (e.g. MDS) */
        struct obd_import       *loc_imp; /* to use in RPC's: can be backward
                                             pointing import */
        struct llog_operations  *loc_logops;
        struct llog_handle      *loc_handle;
        struct llog_commit_master *loc_lcm;
        struct llog_canceld_ctxt *loc_llcd;
        cfs_semaphore_t          loc_sem; /* protects loc_llcd and loc_imp */
        cfs_atomic_t             loc_refcount;
        void                    *llog_proc_cb;
        long                     loc_flags; /* flags, see above defines */
};

#define LCM_NAME_SIZE 64

struct llog_commit_master {
        /**
         * Thread control flags (start, stop, etc.)
         */
        long                       lcm_flags;
        /**
         * Number of llcds onthis lcm.
         */
        cfs_atomic_t               lcm_count;
        /**
         * The refcount for lcm
         */
         cfs_atomic_t              lcm_refcount;
        /**
         * Thread control structure. Used for control commit thread.
         */
        struct ptlrpcd_ctl         lcm_pc;
        /**
         * Lock protecting list of llcds.
         */
        cfs_spinlock_t             lcm_lock;
        /**
         * Llcds in flight for debugging purposes.
         */
        cfs_list_t                 lcm_llcds;
        /**
         * Commit thread name buffer. Only used for thread start.
         */
        char                       lcm_name[LCM_NAME_SIZE];
};

static inline struct llog_commit_master
*lcm_get(struct llog_commit_master *lcm)
{
        LASSERT(cfs_atomic_read(&lcm->lcm_refcount) > 0);
        cfs_atomic_inc(&lcm->lcm_refcount);
        return lcm;
}

static inline void
lcm_put(struct llog_commit_master *lcm)
{
        if (!cfs_atomic_dec_and_test(&lcm->lcm_refcount)) {
                return ;
        }
        OBD_FREE_PTR(lcm);
}

struct llog_canceld_ctxt {
        /**
         * Llog context this llcd is attached to. Used for accessing
         * ->loc_import and others in process of canceling cookies
         * gathered in this llcd.
         */
        struct llog_ctxt          *llcd_ctxt;
        /**
         * Cancel thread control stucture pointer. Used for accessing
         * it to see if should stop processing and other needs.
         */
        struct llog_commit_master *llcd_lcm;
        /**
         * Maximal llcd size. Used in calculations on how much of room
         * left in llcd to cookie comming cookies.
         */
        int                        llcd_size;
        /**
         * Link to lcm llcds list.
         */
        cfs_list_t                 llcd_list;
        /**
         * Current llcd size while gathering cookies. This should not be
         * more than ->llcd_size. Used for determining if we need to
         * send this llcd (if full) and allocate new one. This is also
         * used for copying new cookie at the end of buffer.
         */
        int                        llcd_cookiebytes;
        /**
         * Pointer to the start of cookies buffer.
         */
        struct llog_cookie         llcd_cookies[0];
};

/* ptlrpc/recov_thread.c */
extern struct llog_commit_master *llog_recov_thread_init(char *name);
extern void llog_recov_thread_fini(struct llog_commit_master *lcm,
                                   int force);
extern int llog_recov_thread_start(struct llog_commit_master *lcm);
extern void llog_recov_thread_stop(struct llog_commit_master *lcm,
                                    int force);

static inline void llog_gen_init(struct llog_ctxt *ctxt)
{
        struct obd_device *obd = ctxt->loc_exp->exp_obd;

        LASSERTF(obd->u.obt.obt_magic == OBT_MAGIC,
                 "%s: wrong obt magic %#x\n",
                 obd->obd_name, obd->u.obt.obt_magic);
        ctxt->loc_gen.mnt_cnt = obd->u.obt.obt_mount_count;
        ctxt->loc_gen.conn_cnt++;
}

static inline int llog_gen_lt(struct llog_gen a, struct llog_gen b)
{
        if (a.mnt_cnt < b.mnt_cnt)
                return 1;
        if (a.mnt_cnt > b.mnt_cnt)
                return 0;
        return(a.conn_cnt < b.conn_cnt ? 1 : 0);
}

#define LLOG_PROC_BREAK 0x0001
#define LLOG_DEL_RECORD 0x0002

static inline int llog_obd2ops(struct llog_ctxt *ctxt,
                               struct llog_operations **lop)
{
        if (ctxt == NULL)
                return -ENOTCONN;

        *lop = ctxt->loc_logops;
        if (*lop == NULL)
                return -EOPNOTSUPP;

        return 0;
}

static inline int llog_handle2ops(struct llog_handle *loghandle,
                                  struct llog_operations **lop)
{
        if (loghandle == NULL)
                return -EINVAL;

        return llog_obd2ops(loghandle->lgh_ctxt, lop);
}

static inline int llog_data_len(int len)
{
        return cfs_size_round(len);
}

static inline struct llog_ctxt *llog_ctxt_get(struct llog_ctxt *ctxt)
{
        LASSERT(cfs_atomic_read(&ctxt->loc_refcount) > 0);
        cfs_atomic_inc(&ctxt->loc_refcount);
        CDEBUG(D_INFO, "GETting ctxt %p : new refcount %d\n", ctxt,
               cfs_atomic_read(&ctxt->loc_refcount));
        return ctxt;
}

static inline void llog_ctxt_put(struct llog_ctxt *ctxt)
{
        if (ctxt == NULL)
                return;
        LASSERT(cfs_atomic_read(&ctxt->loc_refcount) > 0);
        LASSERT(cfs_atomic_read(&ctxt->loc_refcount) < 0x5a5a5a);
        CDEBUG(D_INFO, "PUTting ctxt %p : new refcount %d\n", ctxt,
               cfs_atomic_read(&ctxt->loc_refcount) - 1);
        __llog_ctxt_put(ctxt);
}

static inline void llog_group_init(struct obd_llog_group *olg, int group)
{
        cfs_waitq_init(&olg->olg_waitq);
        cfs_spin_lock_init(&olg->olg_lock);
        cfs_sema_init(&olg->olg_cat_processing, 1);
        olg->olg_seq = group;
}

static inline void llog_group_set_export(struct obd_llog_group *olg,
                                         struct obd_export *exp)
{
        LASSERT(exp != NULL);

        cfs_spin_lock(&olg->olg_lock);
        if (olg->olg_exp != NULL && olg->olg_exp != exp)
                CWARN("%s: export for group %d is changed: 0x%p -> 0x%p\n",
                      exp->exp_obd->obd_name, olg->olg_seq,
                      olg->olg_exp, exp);
        olg->olg_exp = exp;
        cfs_spin_unlock(&olg->olg_lock);
}

static inline int llog_group_set_ctxt(struct obd_llog_group *olg,
                                      struct llog_ctxt *ctxt, int index)
{
        LASSERT(index >= 0 && index < LLOG_MAX_CTXTS);

        cfs_spin_lock(&olg->olg_lock);
        if (olg->olg_ctxts[index] != NULL) {
                cfs_spin_unlock(&olg->olg_lock);
                return -EEXIST;
        }
        olg->olg_ctxts[index] = ctxt;
        cfs_spin_unlock(&olg->olg_lock);
        return 0;
}

static inline struct llog_ctxt *llog_group_get_ctxt(struct obd_llog_group *olg,
                                                    int index)
{
        struct llog_ctxt *ctxt;

        LASSERT(index >= 0 && index < LLOG_MAX_CTXTS);

        cfs_spin_lock(&olg->olg_lock);
        if (olg->olg_ctxts[index] == NULL) {
                ctxt = NULL;
        } else {
                ctxt = llog_ctxt_get(olg->olg_ctxts[index]);
        }
        cfs_spin_unlock(&olg->olg_lock);
        return ctxt;
}

static inline struct llog_ctxt *llog_get_context(struct obd_device *obd,
                                                 int index)
{
        return llog_group_get_ctxt(&obd->obd_olg, index);
}

static inline int llog_group_ctxt_null(struct obd_llog_group *olg, int index)
{
        return (olg->olg_ctxts[index] == NULL);
}

static inline int llog_ctxt_null(struct obd_device *obd, int index)
{
        return (llog_group_ctxt_null(&obd->obd_olg, index));
}

static inline int llog_write_rec(struct llog_handle *handle,
                                 struct llog_rec_hdr *rec,
                                 struct llog_cookie *logcookies,
                                 int numcookies, void *buf, int idx)
{
        struct llog_operations *lop;
        int raised, rc, buflen;
        ENTRY;

        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        LASSERT(lop);
        if (lop->lop_write_rec == NULL)
                RETURN(-EOPNOTSUPP);

        /* FIXME:  Why doesn't caller just set the right lrh_len itself? */
        if (buf)
                buflen = rec->lrh_len + sizeof(struct llog_rec_hdr)
                                + sizeof(struct llog_rec_tail);
        else
                buflen = rec->lrh_len;
        LASSERT(cfs_size_round(buflen) == buflen);

        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = lop->lop_write_rec(handle, rec, logcookies, numcookies, buf, idx);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_read_header(struct llog_handle *handle)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_read_header == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_read_header(handle);
        RETURN(rc);
}

static inline int llog_destroy(const struct lu_env *env, struct llog_handle *handle)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_destroy == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_destroy(env, handle);
        RETURN(rc);
}

#if 0
static inline int llog_cancel(struct obd_export *exp,
                              struct lov_stripe_md *lsm, int count,
                              struct llog_cookie *cookies, int flags)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_cancel == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_cancel(exp, lsm, count, cookies, flags);
        RETURN(rc);
}
#endif

static inline int llog_next_block(const struct lu_env *env,
                                  struct llog_handle *loghandle, int *cur_idx,
                                  int next_idx, __u64 *cur_offset, void *buf,
                                  int len)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_next_block == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_next_block(env, loghandle, cur_idx, next_idx,
                                 cur_offset, buf, len);
        RETURN(rc);
}

static inline int llog_prev_block(const struct lu_env *env,
                                  struct llog_handle *loghandle,
                                  int prev_idx, void *buf, int len)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_prev_block == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_prev_block(env, loghandle, prev_idx, buf, len);
        RETURN(rc);
}

static inline int llog_create(struct llog_ctxt *ctxt, struct llog_handle **res,
                              struct llog_logid *logid, char *name)
{
        struct llog_operations *lop;
        int raised, rc;
        ENTRY;

        rc = llog_obd2ops(ctxt, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_create == NULL)
                RETURN(-EOPNOTSUPP);

        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = lop->lop_create(ctxt, res, logid, name);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_connect(struct llog_ctxt *ctxt,
                               struct llog_logid *logid, struct llog_gen *gen,
                               struct obd_uuid *uuid)
{
        struct llog_operations *lop;
        int rc;
        ENTRY;

        rc = llog_obd2ops(ctxt, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_connect == NULL)
                RETURN(-EOPNOTSUPP);

        rc = lop->lop_connect(ctxt, logid, gen, uuid);
        RETURN(rc);
}


/*
 * new API
 */
static inline int llog_open_2(const struct lu_env *env,
                              struct llog_ctxt *ctxt,
                              struct llog_handle **res,
                              struct llog_logid *logid,
                              char *name)
{
        struct llog_operations *lop;
        int raised, rc;
        ENTRY;

        rc = llog_obd2ops(ctxt, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_open_2 == NULL)
                RETURN(-EOPNOTSUPP);

        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = lop->lop_open_2(env, ctxt, res, logid, name);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_exist_2(struct llog_handle *loghandle)
{
        struct llog_operations *lop;
        int raised, rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_exist_2 == NULL)
                RETURN(-EOPNOTSUPP);

        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = lop->lop_exist_2(loghandle);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_create_2(const struct lu_env *env,
                                struct llog_handle *res,
                                struct thandle *th)
{
        struct llog_operations *lop;
        int raised, rc;
        ENTRY;

        rc = llog_handle2ops(res, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_create_2 == NULL)
                RETURN(-EOPNOTSUPP);

        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = lop->lop_create_2(env, res, th);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_declare_create_2(const struct lu_env *env,
                                        struct llog_handle *loghandle,
                                        struct thandle *th)
{
        struct llog_operations *lop;
        int raised, rc;
        ENTRY;

        rc = llog_handle2ops(loghandle, &lop);
        if (rc)
                RETURN(rc);
        if (lop->lop_declare_create_2 == NULL)
                RETURN(-EOPNOTSUPP);

        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = lop->lop_declare_create_2(env, loghandle, th);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_declare_write_rec_2(const struct lu_env *env,
                                           struct llog_handle *handle,
                                           struct llog_rec_hdr *rec, int idx,
                                           struct thandle *th)
{
        struct llog_operations *lop;
        int raised, rc;
        ENTRY;

        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        LASSERT(lop);
        if (lop->lop_declare_write_rec_2 == NULL)
                RETURN(-EOPNOTSUPP);

        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = lop->lop_declare_write_rec_2(env, handle, rec, idx, th);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_write_rec_2(const struct lu_env *env,
                                   struct llog_handle *handle,
                                   struct llog_rec_hdr *rec,
                                   struct llog_cookie *logcookies,
                                   int numcookies, void *buf, int idx,
                                   struct thandle *th)
{
        struct llog_operations *lop;
        int raised, rc, buflen;
        ENTRY;

        rc = llog_handle2ops(handle, &lop);
        if (rc)
                RETURN(rc);
        LASSERT(lop);
        if (lop->lop_write_rec_2 == NULL)
                RETURN(-EOPNOTSUPP);

        /* FIXME:  Why doesn't caller just set the right lrh_len itself? */
        if (buf)
                buflen = rec->lrh_len + sizeof(struct llog_rec_hdr)
                                + sizeof(struct llog_rec_tail);
        else
                buflen = rec->lrh_len;
        LASSERT(cfs_size_round(buflen) == buflen);

        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = lop->lop_write_rec_2(env, handle, rec, logcookies, numcookies, buf, idx, th);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_add_2(const struct lu_env *env,
                             struct llog_ctxt *ctxt, struct llog_rec_hdr *rec,
                             struct lov_stripe_md *lsm,
                             struct llog_cookie *logcookies,
                             int numcookies, struct thandle *th)
{
        int raised, rc;
        ENTRY;

        if (!ctxt) {
                CERROR("No ctxt\n");
                RETURN(-ENODEV);
        }

        if (ctxt->loc_flags & LLOG_CTXT_FLAG_UNINITIALIZED)
                RETURN(-ENXIO);

        CTXT_CHECK_OP(ctxt, add_2, -EOPNOTSUPP);
        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = CTXTP(ctxt, add_2)(env, ctxt, rec, lsm, logcookies, numcookies, th);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}

static inline int llog_declare_add_2(const struct lu_env *env,
                                     struct llog_ctxt *ctxt,
                                     struct llog_rec_hdr *rec,
                                     struct lov_stripe_md *lsm,
                                     struct thandle *th)
{
        int raised, rc;
        ENTRY;

        if (!ctxt) {
                CERROR("No ctxt\n");
                RETURN(-ENODEV);
        }

        if (ctxt->loc_flags & LLOG_CTXT_FLAG_UNINITIALIZED) {
                CERROR("llog is not ready\n");
                RETURN(-ENXIO);
        }

        if (ctxt->loc_logops->lop_declare_add_2 == NULL)
                LBUG();
        raised = cfs_cap_raised(CFS_CAP_SYS_RESOURCE);
        if (!raised)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        rc = CTXTP(ctxt, declare_add_2)(env, ctxt, rec, lsm, th);
        if (!raised)
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
        RETURN(rc);
}


int lustre_log_process(struct lustre_sb_info *lsi, char *logname,
                       struct config_llog_instance *cfg);
int lustre_log_end(struct lustre_sb_info *lsi, char *logname,
                   struct config_llog_instance *cfg);

/** @} log */

#endif
