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
 * Copyright (c) 2011 Whamcloud, Inc.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2012, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/osd/osd_handler.c
 *
 * Top-level entry points into osd module
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 *         Pravin Shelar <pravin.shelar@sun.com> : Added fid in dirent
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>

/* LUSTRE_VERSION_CODE */
#include <lustre_ver.h>
/* prerequisite for linux/xattr.h */
#include <linux/types.h>
/* prerequisite for linux/xattr.h */
#include <linux/fs.h>
/* XATTR_{REPLACE,CREATE} */
#include <linux/xattr.h>
/* simple_mkdir() */
#include <lvfs.h>

/*
 * struct OBD_{ALLOC,FREE}*()
 * OBD_FAIL_CHECK
 */
#include <obd_support.h>
/* struct ptlrpc_thread */
#include <lustre_net.h>

/* fid_is_local() */
#include <lustre_fid.h>

#include "osd_internal.h"
#include "osd_igif.h"

/* llo_* api support */
#include <md_object.h>
/* dt_acct_features */
#include <lu_quota.h>

#ifdef HAVE_LDISKFS_PDO
int ldiskfs_pdo = 1;
CFS_MODULE_PARM(ldiskfs_pdo, "i", int, 0644,
                "ldiskfs with parallel directory operations");
#else
int ldiskfs_pdo = 0;
#endif

static const char dot[] = ".";
static const char dotdot[] = "..";
static const char remote_obj_dir[] = "REM_OBJ_DIR";

static const struct lu_object_operations      osd_lu_obj_ops;
static       struct obd_ops                   osd_obd_device_ops;
static const struct dt_object_operations      osd_obj_ops;
static const struct dt_object_operations      osd_obj_ea_ops;
extern const struct dt_body_operations        osd_body_ops;
extern const struct dt_body_operations        osd_body_ops_new;
static const struct dt_index_operations       osd_index_iam_ops;
static const struct dt_index_operations       osd_index_ea_ops;

static int osd_has_index(const struct osd_object *obj)
{
        return obj->oo_dt.do_index_ops != NULL;
}

static int osd_object_invariant(const struct lu_object *l)
{
        return osd_invariant(osd_obj(l));
}

#ifdef HAVE_QUOTA_SUPPORT
static inline void
osd_push_ctxt(const struct lu_env *env, struct osd_ctxt *save)
{
        struct md_ucred *uc = md_ucred(env);
        struct cred     *tc;

        LASSERT(uc != NULL);

        save->oc_uid = current_fsuid();
        save->oc_gid = current_fsgid();
        save->oc_cap = current_cap();
        if ((tc = prepare_creds())) {
                tc->fsuid         = uc->mu_fsuid;
                tc->fsgid         = uc->mu_fsgid;
                commit_creds(tc);
        }
        /* XXX not suboptimal */
        cfs_curproc_cap_unpack(uc->mu_cap);
}

static inline void
osd_pop_ctxt(struct osd_ctxt *save)
{
        struct cred *tc;

        if ((tc = prepare_creds())) {
                tc->fsuid         = save->oc_uid;
                tc->fsgid         = save->oc_gid;
                tc->cap_effective = save->oc_cap;
                commit_creds(tc);
        }
}
#endif

/*
 * Concurrency: doesn't matter
 */
static int osd_read_locked(const struct lu_env *env, struct osd_object *o)
{
        return osd_oti_get(env)->oti_r_locks > 0;
}

/*
 * Concurrency: doesn't matter
 */
static int osd_write_locked(const struct lu_env *env, struct osd_object *o)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        return oti->oti_w_locks > 0 && o->oo_owner == env;
}

/*
 * Concurrency: doesn't access mutable data
 */
static int osd_root_get(const struct lu_env *env,
                        struct dt_device *dev, struct lu_fid *f)
{
        lu_local_obj_fid(f, OSD_FS_ROOT_OID);
        return 0;
}

static inline int osd_qid_type(struct osd_thandle *oh, int i)
{
        return (oh->ot_id_type & (1 << i)) ? GRPQUOTA : USRQUOTA;
}

static inline void osd_qid_set_type(struct osd_thandle *oh, int i, int type)
{
        oh->ot_id_type |= ((type == GRPQUOTA) ? (1 << i) : 0);
}

void osd_declare_qid(struct dt_object *dt, struct osd_thandle *oh,
                     int type, uid_t id, struct inode *inode)
{
#ifdef CONFIG_QUOTA
        int i, allocated = 0;
        struct osd_object *obj;

        LASSERT(dt != NULL);
        LASSERT(oh != NULL);
        LASSERTF(oh->ot_id_cnt <= OSD_MAX_UGID_CNT, "count=%u",
                 oh->ot_id_cnt);

        /* id entry is allocated in the quota file */
        if (inode && inode->i_dquot[type] && inode->i_dquot[type]->dq_off)
                allocated = 1;

        for (i = 0; i < oh->ot_id_cnt; i++) {
                if (oh->ot_id_array[i] == id && osd_qid_type(oh, i) == type)
                        return;
        }

        if (unlikely(i >= OSD_MAX_UGID_CNT)) {
                CERROR("more than %d uid/gids for a transaction?\n", i);
                return;
        }

        oh->ot_id_array[i] = id;
        osd_qid_set_type(oh, i, type);
        oh->ot_id_cnt++;
        obj = osd_dt_obj(dt);
        oh->ot_credits += (allocated || id == 0) ?
                1 : LDISKFS_QUOTA_INIT_BLOCKS(osd_sb(osd_obj2dev(obj)));
#endif
}

/*
 * OSD object methods.
 */

/*
 * Concurrency: no concurrent access is possible that early in object
 * life-cycle.
 */
static struct lu_object *osd_object_alloc(const struct lu_env *env,
                                          const struct lu_object_header *hdr,
                                          struct lu_device *d)
{
        struct osd_object *mo;

        OBD_ALLOC_PTR(mo);
        if (mo != NULL) {
                struct lu_object *l;

                l = &mo->oo_dt.do_lu;
                dt_object_init(&mo->oo_dt, NULL, d);
                if (osd_dev(d)->od_iop_mode)
                        mo->oo_dt.do_ops = &osd_obj_ea_ops;
                else
                        mo->oo_dt.do_ops = &osd_obj_ops;

                l->lo_ops = &osd_lu_obj_ops;
                cfs_init_rwsem(&mo->oo_sem);
                cfs_init_rwsem(&mo->oo_ext_idx_sem);
                cfs_spin_lock_init(&mo->oo_guard);
                return l;
        } else {
                return NULL;
        }
}

/*
 * retrieve object from backend ext fs.
 **/
struct inode *osd_iget(struct osd_thread_info *info,
                       struct osd_device *dev,
                       const struct osd_inode_id *id)
{
        struct inode *inode = NULL;

        inode = ldiskfs_iget(osd_sb(dev), id->oii_ino);
        if (IS_ERR(inode)) {
                CERROR("Cannot get inode, rc = %li\n", PTR_ERR(inode));
        } else if (id->oii_gen != OSD_OII_NOGEN &&
                   inode->i_generation != id->oii_gen) {
                iput(inode);
                inode = ERR_PTR(-ESTALE);
        } else if (inode->i_nlink == 0) {
                /* due to parallel readdir and unlink,
                * we can have dead inode here. */
                CWARN("stale inode\n");
                make_bad_inode(inode);
                iput(inode);
                inode = ERR_PTR(-ESTALE);
        } else if (is_bad_inode(inode)) {
                CERROR("bad inode %lx\n",inode->i_ino);
                iput(inode);
                inode = ERR_PTR(-ENOENT);
        } else {
                /* Do not update file c/mtime in ldiskfs.
                 * NB: we don't have any lock to protect this because we don't
                 * have reference on osd_object now, but contention with
                 * another lookup + attr_set can't happen in the tiny window
                 * between if (...) and set S_NOCMTIME. */
                if (!(inode->i_flags & S_NOCMTIME))
                        inode->i_flags |= S_NOCMTIME;
        }
        return inode;
}

static int osd_fid_lookup(const struct lu_env *env,
                          struct osd_object *obj, const struct lu_fid *fid)
{
        struct osd_thread_info *info;
        struct lu_device       *ldev = obj->oo_dt.do_lu.lo_dev;
        struct osd_device      *dev;
        struct osd_inode_id    *id;
        struct inode           *inode;
        int                     result;

        LINVRNT(osd_invariant(obj));
        LASSERT(obj->oo_inode == NULL);
        LASSERTF(fid_is_sane(fid) || fid_is_idif(fid), DFID, PFID(fid));
        /*
         * This assertion checks that osd layer sees only local
         * fids. Unfortunately it is somewhat expensive (does a
         * cache-lookup). Disabling it for production/acceptance-testing.
         */
        LASSERT(1 || fid_is_local(env, ldev->ld_site, fid));

        ENTRY;

        info = osd_oti_get(env);
        LASSERT(info);
        dev  = osd_dev(ldev);
        id   = &info->oti_id;

        if (OBD_FAIL_CHECK(OBD_FAIL_OST_ENOENT))
                RETURN(-ENOENT);

        result = osd_oi_lookup(info, dev, fid, id);
        if (result != 0) {
                if (result == -ENOENT)
                        result = 0;
                GOTO(out, result);
        }

        inode = osd_iget(info, dev, id);
        if (IS_ERR(inode)) {
                /*
                 * If fid wasn't found in oi, inode-less object is
                 * created, for which lu_object_exists() returns
                 * false. This is used in a (frequent) case when
                 * objects are created as locking anchors or
                 * place holders for objects yet to be created.
                 */
                result = PTR_ERR(inode);
                GOTO(out, result);
        }

        obj->oo_inode = inode;
        LASSERT(obj->oo_inode->i_sb == osd_sb(dev));
        if (dev->od_iop_mode) {
                obj->oo_compat_dot_created = 1;
                obj->oo_compat_dotdot_created = 1;
        }

        if (!S_ISDIR(inode->i_mode) || !ldiskfs_pdo) /* done */
                goto out;

        LASSERT(obj->oo_hl_head == NULL);
        obj->oo_hl_head = ldiskfs_htree_lock_head_alloc(HTREE_HBITS_DEF);
        if (obj->oo_hl_head == NULL) {
                obj->oo_inode = NULL;
                iput(inode);
                result = -ENOMEM;
        }
out:
        LINVRNT(osd_invariant(obj));
        RETURN(result);
}

/*
 * Concurrency: shouldn't matter.
 */
static void osd_object_init0(struct osd_object *obj)
{
        LASSERT(obj->oo_inode != NULL);
        obj->oo_dt.do_body_ops = &osd_body_ops;
        obj->oo_dt.do_lu.lo_header->loh_attr |=
                (LOHA_EXISTS | (obj->oo_inode->i_mode & S_IFMT));
}

/*
 * Concurrency: no concurrent access is possible that early in object
 * life-cycle.
 */
static int osd_object_init(const struct lu_env *env, struct lu_object *l,
                           const struct lu_object_conf *unused)
{
        struct osd_object *obj = osd_obj(l);
        int result;

        LINVRNT(osd_invariant(obj));

        result = osd_fid_lookup(env, obj, lu_object_fid(l));
        obj->oo_dt.do_body_ops = &osd_body_ops_new;
        LASSERT(obj->oo_dt.do_body_ops->dbo_declare_write);
        if (result == 0) {
                if (obj->oo_inode != NULL)
                        osd_object_init0(obj);
        }
        LINVRNT(osd_invariant(obj));
        return result;
}

/*
 * Concurrency: no concurrent access is possible that late in object
 * life-cycle.
 */
static void osd_object_free(const struct lu_env *env, struct lu_object *l)
{
        struct osd_object *obj = osd_obj(l);

        LINVRNT(osd_invariant(obj));

        dt_object_fini(&obj->oo_dt);
        if (obj->oo_hl_head != NULL)
                ldiskfs_htree_lock_head_free(obj->oo_hl_head);
        OBD_FREE_PTR(obj);
}

/*
 * Concurrency: no concurrent access is possible that late in object
 * life-cycle.
 */
static void osd_index_fini(struct osd_object *o)
{
        struct iam_container *bag;

        if (o->oo_dir != NULL) {
                bag = &o->oo_dir->od_container;
                if (o->oo_inode != NULL) {
                        if (bag->ic_object == o->oo_inode)
                                iam_container_fini(bag);
                }
                OBD_FREE_PTR(o->oo_dir);
                o->oo_dir = NULL;
        }
}

/*
 * Concurrency: no concurrent access is possible that late in object
 * life-cycle (for all existing callers, that is. New callers have to provide
 * their own locking.)
 */
static int osd_inode_unlinked(const struct inode *inode)
{
        return inode->i_nlink == 0;
}

enum {
        OSD_TXN_OI_DELETE_CREDITS    = 20,
        OSD_TXN_INODE_DELETE_CREDITS = 20
};

/*
 * Journal
 */

#ifdef OSD_THANDLE_STATS
/**
 * Set time when the handle is allocated
 */
static void osd_th_alloced(struct osd_thandle *oth)
{
        oth->oth_alloced = cfs_time_current();
}

/**
 * Set time when the handle started
 */
static void osd_th_started(struct osd_thandle *oth)
{
        oth->oth_started = cfs_time_current();
}

/**
 * Helper function to convert time interval to microseconds packed in
 * long int (default time units for the counter in "stats" initialized
 * by lu_time_init() )
 */
static long interval_to_usec(cfs_time_t start, cfs_time_t end)
{
        struct timeval val;

        cfs_duration_usec(cfs_time_sub(end, start), &val);
        return val.tv_sec * 1000000 + val.tv_usec;
}

/**
 * Check whether the we deal with this handle for too long.
 */
static void __osd_th_check_slow(void *oth, struct osd_device *dev,
                                cfs_time_t alloced, cfs_time_t started,
                                cfs_time_t closed)
{
        cfs_time_t now = cfs_time_current();

        LASSERT(dev != NULL);

        lprocfs_counter_add(dev->od_stats, LPROC_OSD_THANDLE_STARTING,
                            interval_to_usec(alloced, started));
        lprocfs_counter_add(dev->od_stats, LPROC_OSD_THANDLE_OPEN,
                            interval_to_usec(started, closed));
        lprocfs_counter_add(dev->od_stats, LPROC_OSD_THANDLE_CLOSING,
                            interval_to_usec(closed, now));

        if (cfs_time_before(cfs_time_add(alloced, cfs_time_seconds(30)), now)) {
                CWARN("transaction handle %p was open for too long: "
                      "now "CFS_TIME_T" ,"
                      "alloced "CFS_TIME_T" ,"
                      "started "CFS_TIME_T" ,"
                      "closed "CFS_TIME_T"\n",
                      oth, now, alloced, started, closed);
                libcfs_debug_dumpstack(NULL);
        }
}

#define OSD_CHECK_SLOW_TH(oth, dev, expr)                               \
{                                                                       \
        cfs_time_t __closed = cfs_time_current();                       \
        cfs_time_t __alloced = oth->oth_alloced;                        \
        cfs_time_t __started = oth->oth_started;                        \
                                                                        \
        expr;                                                           \
        __osd_th_check_slow(oth, dev, __alloced, __started, __closed);  \
}

#else /* OSD_THANDLE_STATS */

#define osd_th_alloced(h)                  do {} while(0)
#define osd_th_started(h)                  do {} while(0)
#define OSD_CHECK_SLOW_TH(oth, dev, expr)  expr

#endif /* OSD_THANDLE_STATS */

/*
 * Concurrency: doesn't access mutable data.
 */
static int osd_param_is_sane(const struct osd_device *dev,
                             const struct thandle *th)
{
        struct osd_thandle *oh;
        oh = container_of0(th, struct osd_thandle, ot_super);
        return oh->ot_credits <= osd_journal(dev)->j_max_transaction_buffers;
}

/*
 * Concurrency: shouldn't matter.
 */
#ifdef HAVE_LDISKFS_JOURNAL_CALLBACK_ADD
static void osd_trans_commit_cb(struct super_block *sb,
                                struct journal_callback *jcb, int error)
#else
static void osd_trans_commit_cb(struct journal_callback *jcb, int error)
#endif
{
        struct osd_thandle *oh = container_of0(jcb, struct osd_thandle, ot_jcb);
        struct thandle     *th  = &oh->ot_super;
        struct lu_device   *lud = &th->th_dev->dd_lu_dev;
        struct dt_txn_commit_cb *dcb, *tmp;

        LASSERT(oh->ot_handle == NULL);

        if (error)
                CERROR("transaction @0x%p commit error: %d\n", th, error);

        dt_txn_hook_commit(th);

        /* call per-transaction callbacks if any */
        cfs_list_for_each_entry_safe(dcb, tmp, &oh->ot_dcb_list, dcb_linkage)
                dcb->dcb_func(NULL, th, dcb, error);

        lu_ref_del_at(&lud->ld_reference, oh->ot_dev_link, "osd-tx", th);
        lu_device_put(lud);
        th->th_dev = NULL;

        lu_context_exit(&th->th_ctx);
        lu_context_fini(&th->th_ctx);
        OBD_FREE_PTR(oh);
}

static struct thandle *osd_trans_create(const struct lu_env *env,
                                        struct dt_device *d)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_iobuf       *iobuf = &oti->oti_iobuf;
        struct osd_thandle     *oh;
        struct thandle         *th;
        ENTRY;

        /* on pending IO in this thread should left from prev. request */
        LASSERT(cfs_atomic_read(&iobuf->dr_numreqs) == 0);

        th = ERR_PTR(-ENOMEM);
        OBD_ALLOC_GFP(oh, sizeof *oh, CFS_ALLOC_IO);
        if (oh != NULL) {
                th = &oh->ot_super;
                th->th_dev = d;
                th->th_result = 0;
                th->th_sync = 0;
                th->th_tags = LCT_TX_HANDLE;
                oh->ot_credits = 0;
                oti->oti_dev = osd_dt_dev(d);
                CFS_INIT_LIST_HEAD(&oh->ot_dcb_list);
                osd_th_alloced(oh);
        }
        RETURN(th);
}

/*
 * Concurrency: shouldn't matter.
 */
int osd_trans_start(const struct lu_env *env, struct dt_device *d,
                    struct thandle *th)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_device  *dev = osd_dt_dev(d);
        handle_t           *jh;
        struct osd_thandle *oh;
        int rc;

        ENTRY;

        LASSERT(current->journal_info == NULL);

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh != NULL);
        LASSERT(oh->ot_handle == NULL);

        rc = dt_txn_hook_start(env, d, th);
        if (rc != 0)
                GOTO(out, rc);

        if (!osd_param_is_sane(dev, th)) {
                CWARN("%s: too many transaction credits (%d > %d)\n",
                      d->dd_lu_dev.ld_obd->obd_name, oh->ot_credits,
                      osd_journal(dev)->j_max_transaction_buffers);
                /* XXX Limit the credits to 'max_transaction_buffers', and
                 *     let the underlying filesystem to catch the error if
                 *     we really need so many credits.
                 *
                 *     This should be removed when we can calculate the
                 *     credits precisely. */
                oh->ot_credits = osd_journal(dev)->j_max_transaction_buffers;
#ifdef OSD_TRACK_DECLARES
                CERROR("  attr_set: %d, punch: %d, xattr_set: %d,\n",
                       oh->ot_declare_attr_set, oh->ot_declare_punch,
                       oh->ot_declare_xattr_set);
                CERROR("  create: %d, ref_add: %d, ref_del: %d, write: %d\n",
                       oh->ot_declare_create, oh->ot_declare_ref_add,
                       oh->ot_declare_ref_del, oh->ot_declare_write);
                CERROR("  insert: %d, delete: %d, destroy: %d\n",
                       oh->ot_declare_insert, oh->ot_declare_delete,
                       oh->ot_declare_destroy);
#endif
        }

        /*
         * XXX temporary stuff. Some abstraction layer should
         * be used.
         */
        jh = ldiskfs_journal_start_sb(osd_sb(dev), oh->ot_credits);
        osd_th_started(oh);
        if (!IS_ERR(jh)) {
                oh->ot_handle = jh;
                LASSERT(oti->oti_txns == 0);
                lu_context_init(&th->th_ctx, th->th_tags);
                lu_context_enter(&th->th_ctx);

                lu_device_get(&d->dd_lu_dev);
                oh->ot_dev_link = lu_ref_add(&d->dd_lu_dev.ld_reference,
                                             "osd-tx", th);

                /*
                 * XXX: current rule is that we first start tx,
                 *      then lock object(s), but we can't use
                 *      this rule for data (due to locking specifics
                 *      in ldiskfs). also in long-term we'd like to
                 *      use usually-used (locks;tx) ordering. so,
                 *      UGLY thing is that we'll use one ordering for
                 *      data (ofd) and reverse ordering for metadata
                 *      (mdd). then at some point we'll fix the latter
                 */
                if (lu_device_is_md(&d->dd_lu_dev)) {
                        LASSERT(oti->oti_r_locks == 0);
                        LASSERT(oti->oti_w_locks == 0);
                }

                oti->oti_txns++;
                rc = 0;
        } else {
                rc = PTR_ERR(jh);
        }
out:
        RETURN(rc);
}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_trans_stop(const struct lu_env *env, struct thandle *th)
{
        int                     rc = 0;
        struct osd_thandle     *oh;
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_iobuf       *iobuf = &oti->oti_iobuf;

        ENTRY;

        oh = container_of0(th, struct osd_thandle, ot_super);

        if (oh->ot_handle != NULL) {
                handle_t *hdl = oh->ot_handle;

                hdl->h_sync = th->th_sync;

                /*
                 * add commit callback
                 * notice we don't do this in osd_trans_start()
                 * as underlying transaction can change during truncate
                 */
                osd_journal_callback_set(hdl, osd_trans_commit_cb,
                                         &oh->ot_jcb);

                LASSERT(oti->oti_txns == 1);
                oti->oti_txns--;
                /*
                 * XXX: current rule is that we first start tx,
                 *      then lock object(s), but we can't use
                 *      this rule for data (due to locking specifics
                 *      in ldiskfs). also in long-term we'd like to
                 *      use usually-used (locks;tx) ordering. so,
                 *      UGLY thing is that we'll use one ordering for
                 *      data (ofd) and reverse ordering for metadata
                 *      (mdd). then at some point we'll fix the latter
                 */
                if (lu_device_is_md(&th->th_dev->dd_lu_dev)) {
                        LASSERT(oti->oti_r_locks == 0);
                        LASSERT(oti->oti_w_locks == 0);
                }
                rc = dt_txn_hook_stop(env, th);
                if (rc != 0)
                        CERROR("Failure in transaction hook: %d\n", rc);
                oh->ot_handle = NULL;
                OSD_CHECK_SLOW_TH(oh, oti->oti_dev,
                                  rc = ldiskfs_journal_stop(hdl));
                if (rc != 0)
                        CERROR("Failure to stop transaction: %d\n", rc);
        } else {
                OBD_FREE_PTR(oh);
        }

        /* as we want IO to journal and data IO be concurrent, we don't block
         * awaiting data IO completion in osd_do_bio(), instead we wait here
         * once transaction is submitted to the journal. all reqular requests
         * don't do direct IO (except read/write), thus this wait_event becomes
         * no-op for them.
         *
         * IMPORTANT: we have to wait till any IO submited by the thread is
         * completed otherwise iobuf may be corrupted by different request
         */
        cfs_wait_event(iobuf->dr_wait,
                       cfs_atomic_read(&iobuf->dr_numreqs) == 0);
        if (!rc)
                rc = iobuf->dr_error;

        RETURN(rc);
}

static int osd_trans_cb_add(struct thandle *th, struct dt_txn_commit_cb *dcb)
{
        struct osd_thandle *oh = container_of0(th, struct osd_thandle,
                                               ot_super);

        cfs_list_add(&dcb->dcb_linkage, &oh->ot_dcb_list);

        return 0;
}

/*
 * Called just before object is freed. Releases all resources except for
 * object itself (that is released by osd_object_free()).
 *
 * Concurrency: no concurrent access is possible that late in object
 * life-cycle.
 */
static void osd_object_delete(const struct lu_env *env, struct lu_object *l)
{
        struct osd_object *obj   = osd_obj(l);
        struct inode      *inode = obj->oo_inode;

        LINVRNT(osd_invariant(obj));

        /*
         * If object is unlinked remove fid->ino mapping from object index.
         */

        osd_index_fini(obj);
        if (inode != NULL) {
                iput(inode);
                obj->oo_inode = NULL;
        }
}

/*
 * Concurrency: ->loo_object_release() is called under site spin-lock.
 */
static void osd_object_release(const struct lu_env *env,
                               struct lu_object *l)
{
}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_object_print(const struct lu_env *env, void *cookie,
                            lu_printer_t p, const struct lu_object *l)
{
        struct osd_object *o = osd_obj(l);
        struct iam_descr  *d;

        if (o->oo_dir != NULL)
                d = o->oo_dir->od_container.ic_descr;
        else
                d = NULL;
        return (*p)(env, cookie,
                    LUSTRE_OSD_LDISKFS_NAME"-object@%p(i:%p:%lu/%u)[%s]",
                    o, o->oo_inode,
                    o->oo_inode ? o->oo_inode->i_ino : 0UL,
                    o->oo_inode ? o->oo_inode->i_generation : 0,
                    d ? d->id_ops->id_name : "plain");
}

/**
 * Estimate space needed for file creations. We assume the largest filename
 * which is 2^64 - 1, hence a filename of 20 chars.
 * This is 28 bytes per object which is 28MB for 1M objects ... no so bad.
 */
#ifdef __LDISKFS_DIR_REC_LEN
#define PER_OBJ_USAGE __LDISKFS_DIR_REC_LEN(20)
#else
#define PER_OBJ_USAGE LDISKFS_DIR_REC_LEN(20)
#endif

#define GRANT_FOR_LOCAL_OIDS 32 /* 128kB for last_rcvd, quota files, ... */

/*
 * Concurrency: shouldn't matter.
 */
int osd_statfs(const struct lu_env *env, struct dt_device *d,
               struct obd_statfs *osfs)
{
        struct osd_device *osd = osd_dt_dev(d);
        struct super_block *sb = osd_sb(osd);
        int result = 0;
        obd_size blocks_used, objects_est;
        cfs_kstatfs_t kfs;

        kfs.f_frsize = 0;
        result = ll_do_statfs(sb, &kfs);
        if (unlikely(result != 0)) /* N.B. statfs can't really fail */
                return result;

        statfs_pack(osfs, &kfs);
        if (sb->s_flags & MS_RDONLY)
                osfs->os_state = OS_STATE_READONLY;

        /* Reserve a small amount of space for local objects like last_rcvd,
         * llog, quota files, ... */
        if (osfs->os_bavail <= GRANT_FOR_LOCAL_OIDS) {
                osfs->os_bavail = 0;
                return 0;
        }
        osfs->os_bavail -= GRANT_FOR_LOCAL_OIDS;

        /* Object creations consume data blocks to store directory entries
         * Use mean object size to estimate how many objects will fill the
         * remaining blocks available for use in the filesystem:
         *
         * objects_est = blocks_avail * (inodes_used / blocks_used);
         * inodes_used = (osfs->os_files - osfs->os_ffree); */
        blocks_used = osfs->os_blocks - osfs->os_bfree;
        objects_est = osfs->os_bavail * (osfs->os_files - osfs->os_ffree);
        if (objects_est && blocks_used) {
                do_div(objects_est, blocks_used);
                if (objects_est > osfs->os_ffree)
                        objects_est = osfs->os_ffree;
                osfs->os_bavail -= min_t(obd_size, osfs->os_bavail,
                                         (objects_est * PER_OBJ_USAGE)
                                                       >> sb->s_blocksize_bits);
        }

        /** Take out metadata overhead for indirect blocks */
        osfs->os_bavail -= osfs->os_bavail >> (sb->s_blocksize_bits - 3);

        /**
         * If delayed allocation is used one day, ext4_statfs() already takes
         * into account space reservation via sbi->s_dirtyblocks_counter
         */

#ifdef LARGE_BS_EMULATION
#if LINUX_VERSION_CODE > KERNEL_VERSION(3,2,0)
#warning "check that ldiskfs handles bigalloc large blocksize"
#endif
        /* emulate 1MB block size */
        osfs->os_blocks >>= 20 - sb->s_blocksize_bits;
        osfs->os_bfree  >>= 20 - sb->s_blocksize_bits;
        osfs->os_bavail >>= 20 - sb->s_blocksize_bits;
        osfs->os_bsize    = 1 << 20;
#endif

        return 0;
}

/*
 * Concurrency: doesn't access mutable data.
 */
static void osd_conf_get(const struct lu_env *env,
                         const struct dt_device *dev,
                         struct dt_device_param *param)
{
        struct super_block *sb = osd_sb(osd_dt_dev(dev));

        /*
         * XXX should be taken from not-yet-existing fs abstraction layer.
         */
        param->ddp_max_name_len   = LDISKFS_NAME_LEN;
        param->ddp_max_nlink      = LDISKFS_LINK_MAX;
        param->ddp_block_shift    = sb->s_blocksize_bits;
        param->ddp_mount_type     = LDD_MT_LDISKFS;
        param->ddp_maxbytes       = sb->s_maxbytes;
        /* Overhead estimate should be fairly accurate, so we really take a tiny
         * error margin which also avoids fragmenting the filesystem too much */
        param->ddp_grant_reserved = 2; /* end up to be 1.9% after conversion */
        /* inode are statically allocated, so per-inode space consumption
         * is 0 for ldiskfs OSD */
        param->ddp_inodespace     = 0;
        /* per-fragment overhead to be used by the client code */
        param->ddp_grant_frag     = 6 * LDISKFS_BLOCK_SIZE(sb);
        param->ddp_mntopts        = 0;
        if (test_opt(sb, XATTR_USER))
                param->ddp_mntopts |= MNTOPT_USERXATTR;
        if (test_opt(sb, POSIX_ACL))
                param->ddp_mntopts |= MNTOPT_ACL;

#if defined(LDISKFS_FEATURE_INCOMPAT_EA_INODE)
        if (LDISKFS_HAS_INCOMPAT_FEATURE(sb, LDISKFS_FEATURE_INCOMPAT_EA_INODE))
                param->ddp_max_ea_size = LDISKFS_XATTR_MAX_LARGE_EA_SIZE;
        else
#endif
                param->ddp_max_ea_size = sb->s_blocksize;

}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_sync(const struct lu_env *env, struct dt_device *d)
{
        CDEBUG(D_HA, "syncing OSD %s\n", LUSTRE_OSD_LDISKFS_NAME);
        return ldiskfs_force_commit(osd_sb(osd_dt_dev(d)));
}

/**
 * Start commit for OSD device.
 *
 * An implementation of dt_commit_async method for OSD device.
 * Asychronously starts underlayng fs sync and thereby a transaction
 * commit.
 *
 * \param env environment
 * \param d dt device
 *
 * \see dt_device_operations
 */
static int osd_commit_async(const struct lu_env *env,
                            struct dt_device *d)
{
        struct super_block *s = osd_sb(osd_dt_dev(d));
        ENTRY;

        CDEBUG(D_HA, "async commit OSD %s\n", LUSTRE_OSD_LDISKFS_NAME);
        RETURN(s->s_op->sync_fs(s, 0));
}

/*
 * Concurrency: shouldn't matter.
 */

static int osd_ro(const struct lu_env *env, struct dt_device *d)
{
        struct super_block *sb = osd_sb(osd_dt_dev(d));
        int rc;
        ENTRY;

        CERROR("*** setting device %s read-only ***\n",
               LUSTRE_OSD_LDISKFS_NAME);

        rc = __lvfs_set_rdonly(sb->s_bdev, LDISKFS_SB(sb)->journal_bdev);
        RETURN(rc);
}

static char *osd_label_get(const struct lu_env *env, const struct dt_device *dt)
{
        struct super_block *sb = osd_sb(osd_dt_dev(dt));
        LASSERT(sb);
        return LDISKFS_SB(sb)->s_es->s_volume_name;
}

static int osd_label_set(const struct lu_env *env, const struct dt_device *dt,
                         char *label)
{
        struct super_block *sb = osd_sb(osd_dt_dev(dt));
        journal_t *journal;
        handle_t *handle;
        int rc;

        journal = LDISKFS_SB(sb)->s_journal;
        handle = ldiskfs_journal_start_sb(sb, 1);
        if (IS_ERR(handle)) {
                CERROR("can't start transaction\n");
                return(PTR_ERR(handle));
        }

        rc = ldiskfs_journal_get_write_access(handle, LDISKFS_SB(sb)->s_sbh);
        if (rc)
                goto out;

        memcpy(LDISKFS_SB(sb)->s_es->s_volume_name, label,
               sizeof(LDISKFS_SB(sb)->s_es->s_volume_name));

        rc = ldiskfs_journal_dirty_metadata(handle, LDISKFS_SB(sb)->s_sbh);

out:
        ldiskfs_journal_stop(handle);

        return rc;
}

/*
 * Concurrency: serialization provided by callers.
 */
static int osd_init_capa_ctxt(const struct lu_env *env, struct dt_device *d,
                              int mode, unsigned long timeout, __u32 alg,
                              struct lustre_capa_key *keys)
{
        struct osd_device *dev = osd_dt_dev(d);
        ENTRY;

        dev->od_fl_capa = mode;
        dev->od_capa_timeout = timeout;
        dev->od_capa_alg = alg;
        dev->od_capa_keys = keys;
        RETURN(0);
}

/**
 * Concurrency: serialization provided by callers.
 */
static int osd_quota_setup(const struct lu_env *env, struct dt_device *d,
                                void *data)
{
        return 0;
}

static void osd_quota_cleanup(const struct lu_env *env, struct dt_device *d)
{
        return;
}

/**
 * Note: we do not count into QUOTA here.
 * If we mount with --data_journal we may need more.
 */
const int osd_dto_credits_noquota[DTO_NR] = {
        /**
         * Insert/Delete.
         * INDEX_EXTRA_TRANS_BLOCKS(8) +
         * SINGLEDATA_TRANS_BLOCKS(8)
         * XXX Note: maybe iam need more, since iam have more level than
         *           EXT3 htree.
         */
        [DTO_INDEX_INSERT]  = 10, /* XXX: 16 */
        [DTO_INDEX_DELETE]  = 16,
        /**
         * Unused now
         */
        [DTO_INDEX_UPDATE]  = 16,
        /**
         * Create a object. The same as create object in EXT3.
         * DATA_TRANS_BLOCKS(14) +
         * INDEX_EXTRA_BLOCKS(8) +
         * 3(inode bits, groups, GDT)
         */
        [DTO_OBJECT_CREATE] = 25,
        /**
         * XXX: real credits to be fixed
         */
        [DTO_OBJECT_DELETE] = 25,
        /**
         * Attr set credits (inode)
         */
        [DTO_ATTR_SET_BASE] = 1,
        /**
         * Xattr set. The same as xattr of EXT3.
         * DATA_TRANS_BLOCKS(14)
         * XXX Note: in original MDS implmentation INDEX_EXTRA_TRANS_BLOCKS
         * are also counted in. Do not know why?
         */
        [DTO_XATTR_SET]     = 14,
        [DTO_LOG_REC]       = 14,
        /**
         * credits for inode change during write.
         */
        [DTO_WRITE_BASE]    = 3,
        /**
         * credits for single block write.
         */
        [DTO_WRITE_BLOCK]   = 10, /* XXX: 14 */
        /**
         * Attr set credits for chown.
         * This is extra credits for setattr, and it is null without quota
         */
        [DTO_ATTR_SET_CHOWN]= 0
};

static const struct dt_device_operations osd_dt_ops = {
        .dt_root_get       = osd_root_get,
        .dt_statfs         = osd_statfs,
        .dt_trans_create   = osd_trans_create,
        .dt_trans_start    = osd_trans_start,
        .dt_trans_stop     = osd_trans_stop,
        .dt_trans_cb_add   = osd_trans_cb_add,
        .dt_conf_get       = osd_conf_get,
        .dt_sync           = osd_sync,
        .dt_ro             = osd_ro,
        .dt_commit_async   = osd_commit_async,
        .dt_init_capa_ctxt = osd_init_capa_ctxt,
        .dt_quota          = {
                .dt_setup   = osd_quota_setup,
                .dt_cleanup = osd_quota_cleanup,
        },
        .dt_label_get      = osd_label_get,
        .dt_label_set      = osd_label_set
};

static void osd_object_read_lock(const struct lu_env *env,
                                 struct dt_object *dt, unsigned role)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_thread_info *oti = osd_oti_get(env);

        LINVRNT(osd_invariant(obj));

        LASSERT(obj->oo_owner != env);
        cfs_down_read_nested(&obj->oo_sem, role);

        LASSERT(obj->oo_owner == NULL);
        oti->oti_r_locks++;
}

static void osd_object_write_lock(const struct lu_env *env,
                                  struct dt_object *dt, unsigned role)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_thread_info *oti = osd_oti_get(env);

        LINVRNT(osd_invariant(obj));

        LASSERT(obj->oo_owner != env);
        cfs_down_write_nested(&obj->oo_sem, role);

        LASSERT(obj->oo_owner == NULL);
        obj->oo_owner = env;
        oti->oti_w_locks++;
}

static void osd_object_read_unlock(const struct lu_env *env,
                                   struct dt_object *dt)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_thread_info *oti = osd_oti_get(env);

        LINVRNT(osd_invariant(obj));

        LASSERT(oti->oti_r_locks > 0);
        oti->oti_r_locks--;
        cfs_up_read(&obj->oo_sem);
}

static void osd_object_write_unlock(const struct lu_env *env,
                                    struct dt_object *dt)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_thread_info *oti = osd_oti_get(env);

        LINVRNT(osd_invariant(obj));

        LASSERT(obj->oo_owner == env);
        LASSERT(oti->oti_w_locks > 0);
        oti->oti_w_locks--;
        obj->oo_owner = NULL;
        cfs_up_write(&obj->oo_sem);
}

static int osd_object_write_locked(const struct lu_env *env,
                                   struct dt_object *dt)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LINVRNT(osd_invariant(obj));

        return obj->oo_owner == env;
}

static int capa_is_sane(const struct lu_env *env,
                        struct osd_device *dev,
                        struct lustre_capa *capa,
                        struct lustre_capa_key *keys)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct lustre_capa *tcapa = &oti->oti_capa;
        struct obd_capa *oc;
        int i, rc = 0;
        ENTRY;

        oc = capa_lookup(dev->od_capa_hash, capa, 0);
        if (oc) {
                if (capa_is_expired(oc)) {
                        DEBUG_CAPA(D_ERROR, capa, "expired");
                        rc = -ESTALE;
                }
                capa_put(oc);
                RETURN(rc);
        }

        if (capa_is_expired_sec(capa)) {
                DEBUG_CAPA(D_ERROR, capa, "expired");
                RETURN(-ESTALE);
        }

        cfs_spin_lock(&capa_lock);
        for (i = 0; i < 2; i++) {
                if (keys[i].lk_keyid == capa->lc_keyid) {
                        oti->oti_capa_key = keys[i];
                        break;
                }
        }
        cfs_spin_unlock(&capa_lock);

        if (i == 2) {
                DEBUG_CAPA(D_ERROR, capa, "no matched capa key");
                RETURN(-ESTALE);
        }

        rc = capa_hmac(tcapa->lc_hmac, capa, oti->oti_capa_key.lk_key);
        if (rc)
                RETURN(rc);

        if (memcmp(tcapa->lc_hmac, capa->lc_hmac, sizeof(capa->lc_hmac))) {
                DEBUG_CAPA(D_ERROR, capa, "HMAC mismatch");
                RETURN(-EACCES);
        }

        oc = capa_add(dev->od_capa_hash, capa);
        capa_put(oc);

        RETURN(0);
}

int osd_object_auth(const struct lu_env *env, struct dt_object *dt,
                    struct lustre_capa *capa, __u64 opc)
{
        const struct lu_fid *fid = lu_object_fid(&dt->do_lu);
        struct osd_device *dev = osd_dev(dt->do_lu.lo_dev);
        struct md_capainfo *ci;
        int rc;

        if (!dev->od_fl_capa)
                return 0;

        if (capa == BYPASS_CAPA)
                return 0;

        ci = md_capainfo(env);
        if (unlikely(!ci))
                return 0;

        if (ci->mc_auth == LC_ID_NONE)
                return 0;

        if (!capa) {
                CERROR("no capability is provided for fid "DFID"\n", PFID(fid));
                return -EACCES;
        }

        if (!lu_fid_eq(fid, &capa->lc_fid)) {
                DEBUG_CAPA(D_ERROR, capa, "fid "DFID" mismatch with",
                           PFID(fid));
                return -EACCES;
        }

        if (!capa_opc_supported(capa, opc)) {
                DEBUG_CAPA(D_ERROR, capa, "opc "LPX64" not supported by", opc);
                return -EACCES;
        }

        if ((rc = capa_is_sane(env, dev, capa, dev->od_capa_keys))) {
                DEBUG_CAPA(D_ERROR, capa, "insane (rc %d)", rc);
                return -EACCES;
        }

        return 0;
}

static struct timespec *osd_inode_time(const struct lu_env *env,
                                       struct inode *inode, __u64 seconds)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct timespec        *t   = &oti->oti_time;

        t->tv_sec  = seconds;
        t->tv_nsec = 0;
        *t = timespec_trunc(*t, get_sb_time_gran(inode->i_sb));
        return t;
}


static void osd_inode_getattr(const struct lu_env *env,
                              struct inode *inode, struct lu_attr *attr)
{
        attr->la_valid      |= LA_ATIME | LA_MTIME | LA_CTIME | LA_MODE |
                               LA_SIZE | LA_BLOCKS | LA_UID | LA_GID |
                               LA_FLAGS | LA_NLINK | LA_RDEV | LA_BLKSIZE;

        attr->la_atime      = LTIME_S(inode->i_atime);
        attr->la_mtime      = LTIME_S(inode->i_mtime);
        attr->la_ctime      = LTIME_S(inode->i_ctime);
        attr->la_mode       = inode->i_mode;
        attr->la_size       = i_size_read(inode);
        attr->la_blocks     = inode->i_blocks;
        attr->la_uid        = inode->i_uid;
        attr->la_gid        = inode->i_gid;
        attr->la_flags      = LDISKFS_I(inode)->i_flags;
        attr->la_nlink      = inode->i_nlink;
        attr->la_rdev       = inode->i_rdev;
        attr->la_blksize    = ll_inode_blksize(inode);
        attr->la_blkbits    = inode->i_blkbits;
}

static int osd_attr_get(const struct lu_env *env,
                        struct dt_object *dt,
                        struct lu_attr *attr,
                        struct lustre_capa *capa)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(dt_object_exists(dt));
        LINVRNT(osd_invariant(obj));

        if (osd_object_auth(env, dt, capa, CAPA_OPC_META_READ))
                return -EACCES;

        cfs_spin_lock(&obj->oo_guard);
        osd_inode_getattr(env, obj->oo_inode, attr);
        cfs_spin_unlock(&obj->oo_guard);
        return 0;
}

static int osd_declare_attr_set(const struct lu_env *env,
                                struct dt_object *dt,
                                const struct lu_attr *attr,
                                struct thandle *handle)
{
        struct osd_thandle *oh;
        struct osd_object *obj;

        LASSERT(dt != NULL);
        LASSERT(handle != NULL);

        obj = osd_dt_obj(dt);
        LASSERT(osd_invariant(obj));

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, attr_set);
        oh->ot_credits += osd_dto_credits_noquota[DTO_ATTR_SET_BASE];

        if (attr && attr->la_valid & LA_UID) {
                if (obj->oo_inode)
                        osd_declare_qid(dt, oh, USRQUOTA, obj->oo_inode->i_uid,
                                        obj->oo_inode);
                osd_declare_qid(dt, oh, USRQUOTA, attr->la_uid, NULL);
        }
        if (attr && attr->la_valid & LA_GID) {
                if (obj->oo_inode)
                        osd_declare_qid(dt, oh, GRPQUOTA, obj->oo_inode->i_gid,
                                        obj->oo_inode);
                osd_declare_qid(dt, oh, GRPQUOTA, attr->la_gid, NULL);
        }

        return 0;
}

static int osd_inode_setattr(const struct lu_env *env,
                             struct inode *inode, const struct lu_attr *attr)
{
        __u64 bits;

        bits = attr->la_valid;

        LASSERT(!(bits & LA_TYPE)); /* Huh? You want too much. */

#ifdef HAVE_QUOTA_SUPPORT
        if ((bits & LA_UID && attr->la_uid != inode->i_uid) ||
            (bits & LA_GID && attr->la_gid != inode->i_gid)) {
                struct osd_ctxt *save = &osd_oti_get(env)->oti_ctxt;
                struct iattr iattr;
                int rc;

                iattr.ia_valid = 0;
                if (bits & LA_UID)
                        iattr.ia_valid |= ATTR_UID;
                if (bits & LA_GID)
                        iattr.ia_valid |= ATTR_GID;
                iattr.ia_uid = attr->la_uid;
                iattr.ia_gid = attr->la_gid;
                osd_push_ctxt(env, save);
                rc = DQUOT_TRANSFER(inode, &iattr) ? -EDQUOT : 0;
                osd_pop_ctxt(save);
                if (rc != 0)
                        return rc;
        }
#endif

#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(2,7,50,0)
        /* Try to correct for a bug in 2.1.0 (LU-221) that caused negative
         * timestamps to appear to be in the far future, due old timestamp
         * being stored on disk as an unsigned value.  This fixes up any
         * bad values held by the client before storing them on disk,
         * and ensures any timestamp updates are correct.  LU-1042 */
        if (unlikely(LTIME_S(inode->i_atime) == LU221_BAD_TIME &&
                     !(bits & LA_ATIME))) {
                inode->i_atime  = *osd_inode_time(env, inode, 0);
        }
        if (unlikely(LTIME_S(inode->i_mtime) == LU221_BAD_TIME &&
                     !(bits & LA_MTIME))) {
                inode->i_mtime  = *osd_inode_time(env, inode, 0);
        }
        if (unlikely((LTIME_S(inode->i_ctime) == LU221_BAD_TIME ||
                      LTIME_S(inode->i_ctime) == 0) &&
                     !(bits & LA_CTIME))) {
                inode->i_ctime  = *osd_inode_time(env, inode,
                                  LDISKFS_SB(inode->i_sb)->s_es->s_mkfs_time);
        }
#else
#warning "remove old LU-221/LU-1042 workaround code"
#endif

        /* When initializating timestamps for new inodes, use the filesystem
         * mkfs time for ctime to avoid e2fsck ibadness incorrectly thinking
         * that this is potentially an invalid inode.  Files with an old ctime
         * migrated to a newly-formatted OST with a newer s_mkfs_time will not
         * hit this check, since it is only for ctime == 0.  LU-1010/LU-1042 */
        if (bits & LA_CTIME && attr->la_ctime == 0)
                inode->i_ctime  = *osd_inode_time(env, inode,
                                  LDISKFS_SB(inode->i_sb)->s_es->s_mkfs_time);
        else if (bits & LA_CTIME)
                inode->i_ctime  = *osd_inode_time(env, inode, attr->la_ctime);

        if (bits & LA_ATIME)
                inode->i_atime  = *osd_inode_time(env, inode, attr->la_atime);
        if (bits & LA_MTIME)
                inode->i_mtime  = *osd_inode_time(env, inode, attr->la_mtime);
        if (bits & LA_SIZE) {
                LDISKFS_I(inode)->i_disksize = attr->la_size;
                i_size_write(inode, attr->la_size);
        }

#if 0
        /* OSD should not change "i_blocks" which is used by quota.
         * "i_blocks" should be changed by ldiskfs only. */
        if (bits & LA_BLOCKS)
                inode->i_blocks = attr->la_blocks;
#endif
        if (bits & LA_MODE)
                inode->i_mode   = (inode->i_mode & S_IFMT) |
                        (attr->la_mode & ~S_IFMT);
        if (bits & LA_UID)
                inode->i_uid    = attr->la_uid;
        if (bits & LA_GID)
                inode->i_gid    = attr->la_gid;
        if (bits & LA_NLINK)
                inode->i_nlink  = attr->la_nlink;
        if (bits & LA_RDEV)
                inode->i_rdev   = attr->la_rdev;

        if (bits & LA_FLAGS) {
                /* always keep S_NOCMTIME */
                inode->i_flags = ll_ext_to_inode_flags(attr->la_flags) |
                                 S_NOCMTIME;
        }
        return 0;
}

static int osd_attr_set(const struct lu_env *env,
                        struct dt_object *dt,
                        const struct lu_attr *attr,
                        struct thandle *handle,
                        struct lustre_capa *capa)
{
        struct osd_object *obj = osd_dt_obj(dt);
        int rc;

        LASSERT(handle != NULL);
        LASSERT(dt_object_exists(dt));
        LASSERT(osd_invariant(obj));

        if (osd_object_auth(env, dt, capa, CAPA_OPC_META_WRITE))
                return -EACCES;

        OSD_EXEC_OP(handle, attr_set);

        cfs_spin_lock(&obj->oo_guard);
        rc = osd_inode_setattr(env, obj->oo_inode, attr);
        cfs_spin_unlock(&obj->oo_guard);

        if (!rc)
                obj->oo_inode->i_sb->s_op->dirty_inode(obj->oo_inode);
        return rc;
}

struct dentry *osd_child_dentry_get(const struct lu_env *env,
                                    struct osd_object *obj,
                                    const char *name, const int namelen)
{
        return osd_child_dentry_by_inode(env, obj->oo_inode, name, namelen);
}

static int osd_mkfile(struct osd_thread_info *info, struct osd_object *obj,
                      cfs_umode_t mode,
                      struct dt_allocation_hint *hint,
                      struct thandle *th)
{
        int result;
        struct osd_device  *osd = osd_obj2dev(obj);
        struct osd_thandle *oth;
        struct dt_object   *parent = NULL;
        struct inode       *inode;
#ifdef HAVE_QUOTA_SUPPORT
        struct osd_ctxt    *save = &info->oti_ctxt;
#endif

        LINVRNT(osd_invariant(obj));
        LASSERT(obj->oo_inode == NULL);
        LASSERT(obj->oo_hl_head == NULL);

        if (S_ISDIR(mode) && ldiskfs_pdo) {
                obj->oo_hl_head =ldiskfs_htree_lock_head_alloc(HTREE_HBITS_DEF);
                if (obj->oo_hl_head == NULL)
                        return -ENOMEM;
        }

        oth = container_of(th, struct osd_thandle, ot_super);
        LASSERT(oth->ot_handle->h_transaction != NULL);

        if (hint && hint->dah_parent)
                parent = hint->dah_parent;

#ifdef HAVE_QUOTA_SUPPORT
        osd_push_ctxt(info->oti_env, save);
#endif
        inode = ldiskfs_create_inode(oth->ot_handle,
                                     parent ? osd_dt_obj(parent)->oo_inode :
                                              osd_sb(osd)->s_root->d_inode,
                                     mode);
#ifdef HAVE_QUOTA_SUPPORT
        osd_pop_ctxt(save);
#endif
        if (!IS_ERR(inode)) {
                /* Do not update file c/mtime in ldiskfs.
                 * NB: don't need any lock because no contention at this
                 * early stage */
                inode->i_flags |= S_NOCMTIME;
                obj->oo_inode = inode;
                result = 0;
        } else {
                if (obj->oo_hl_head != NULL) {
                        ldiskfs_htree_lock_head_free(obj->oo_hl_head);
                        obj->oo_hl_head = NULL;
                }
                result = PTR_ERR(inode);
        }
        LINVRNT(osd_invariant(obj));
        return result;
}

enum {
        OSD_NAME_LEN = 255
};

static int osd_mkdir(struct osd_thread_info *info, struct osd_object *obj,
                     struct lu_attr *attr,
                     struct dt_allocation_hint *hint,
                     struct dt_object_format *dof,
                     struct thandle *th)
{
        int result;
        struct osd_thandle *oth;
        struct osd_device *osd = osd_obj2dev(obj);
        __u32 mode = (attr->la_mode & (S_IFMT | S_IALLUGO | S_ISVTX));

        LASSERT(S_ISDIR(attr->la_mode));

        oth = container_of(th, struct osd_thandle, ot_super);
        LASSERT(oth->ot_handle->h_transaction != NULL);
        result = osd_mkfile(info, obj, mode, hint, th);
        if (result == 0 && osd->od_iop_mode == 0) {
                LASSERT(obj->oo_inode != NULL);
                /*
                 * XXX uh-oh... call low-level iam function directly.
                 */

                result = iam_lvar_create(obj->oo_inode, OSD_NAME_LEN, 4,
                                         sizeof (struct osd_fid_pack),
                                         oth->ot_handle);
        }
        return result;
}

static int osd_mk_index(struct osd_thread_info *info, struct osd_object *obj,
                        struct lu_attr *attr,
                        struct dt_allocation_hint *hint,
                        struct dt_object_format *dof,
                        struct thandle *th)
{
        int result;
        struct osd_thandle *oth;
        const struct dt_index_features *feat = dof->u.dof_idx.di_feat;

        __u32 mode = (attr->la_mode & (S_IFMT | S_IALLUGO | S_ISVTX));

        LASSERT(S_ISREG(attr->la_mode));

        oth = container_of(th, struct osd_thandle, ot_super);
        LASSERT(oth->ot_handle->h_transaction != NULL);

        result = osd_mkfile(info, obj, mode, hint, th);
        if (result == 0) {
                LASSERT(obj->oo_inode != NULL);
                if (feat->dif_flags & DT_IND_VARKEY)
                        result = iam_lvar_create(obj->oo_inode,
                                                 feat->dif_keysize_max,
                                                 feat->dif_ptrsize,
                                                 feat->dif_recsize_max,
                                                 oth->ot_handle);
                else
                        result = iam_lfix_create(obj->oo_inode,
                                                 feat->dif_keysize_max,
                                                 feat->dif_ptrsize,
                                                 feat->dif_recsize_max,
                                                 oth->ot_handle);

        }
        return result;
}

static int osd_mkreg(struct osd_thread_info *info, struct osd_object *obj,
                     struct lu_attr *attr,
                     struct dt_allocation_hint *hint,
                     struct dt_object_format *dof,
                     struct thandle *th)
{
        LASSERT(S_ISREG(attr->la_mode));
        return osd_mkfile(info, obj, (attr->la_mode &
                               (S_IFMT | S_IALLUGO | S_ISVTX)), hint, th);
}

static int osd_mksym(struct osd_thread_info *info, struct osd_object *obj,
                     struct lu_attr *attr,
                     struct dt_allocation_hint *hint,
                     struct dt_object_format *dof,
                     struct thandle *th)
{
        LASSERT(S_ISLNK(attr->la_mode));
        return osd_mkfile(info, obj, (attr->la_mode &
                              (S_IFMT | S_IALLUGO | S_ISVTX)), hint, th);
}

static int osd_mknod(struct osd_thread_info *info, struct osd_object *obj,
                     struct lu_attr *attr,
                     struct dt_allocation_hint *hint,
                     struct dt_object_format *dof,
                     struct thandle *th)
{
        cfs_umode_t mode = attr->la_mode & (S_IFMT | S_IALLUGO | S_ISVTX);
        int result;

        LINVRNT(osd_invariant(obj));
        LASSERT(obj->oo_inode == NULL);
        LASSERT(S_ISCHR(mode) || S_ISBLK(mode) ||
                S_ISFIFO(mode) || S_ISSOCK(mode));

        result = osd_mkfile(info, obj, mode, hint, th);
        if (result == 0) {
                LASSERT(obj->oo_inode != NULL);
                /*
                 * This inode should be marked dirty for i_rdev.  Currently
                 * that is done in the osd_attr_init().
                 */
                init_special_inode(obj->oo_inode, mode, attr->la_rdev);
        }
        LINVRNT(osd_invariant(obj));
        return result;
}

typedef int (*osd_obj_type_f)(struct osd_thread_info *, struct osd_object *,
                              struct lu_attr *,
                              struct dt_allocation_hint *hint,
                              struct dt_object_format *dof,
                              struct thandle *);

static osd_obj_type_f osd_create_type_f(enum dt_format_type type)
{
        osd_obj_type_f result;

        switch (type) {
        case DFT_DIR:
                result = osd_mkdir;
                break;
        case DFT_REGULAR:
                result = osd_mkreg;
                break;
        case DFT_SYM:
                result = osd_mksym;
                break;
        case DFT_NODE:
                result = osd_mknod;
                break;
        case DFT_INDEX:
                result = osd_mk_index;
                break;

        default:
                LBUG();
                break;
        }
        return result;
}


static void osd_ah_init(const struct lu_env *env, struct dt_allocation_hint *ah,
                        struct dt_object *parent, struct dt_object *child,
                        cfs_umode_t child_mode)
{
        LASSERT(ah);

        memset(ah, 0, sizeof(*ah));
        ah->dah_parent = parent;
        ah->dah_mode = child_mode;
}

static void osd_attr_init(struct osd_thread_info *info, struct osd_object *obj,
                          struct lu_attr *attr, struct dt_object_format *dof)
{
        struct inode   *inode = obj->oo_inode;
        __u64           valid = attr->la_valid;
        int             result;

        attr->la_valid &= ~(LA_TYPE | LA_MODE);
#ifdef HAVE_QUOTA_SUPPORT
        attr->la_valid &= ~(LA_UID | LA_GID);
#endif
        if (dof->dof_type != DFT_NODE)
                attr->la_valid &= ~LA_RDEV;
        if ((valid & LA_ATIME) && (attr->la_atime == LTIME_S(inode->i_atime)))
                attr->la_valid &= ~LA_ATIME;
        if ((valid & LA_CTIME) && (attr->la_ctime == LTIME_S(inode->i_ctime)))
                attr->la_valid &= ~LA_CTIME;
        if ((valid & LA_MTIME) && (attr->la_mtime == LTIME_S(inode->i_mtime)))
                attr->la_valid &= ~LA_MTIME;

        if (attr->la_valid != 0) {
                result = osd_inode_setattr(info->oti_env, inode, attr);
                /*
                 * The osd_inode_setattr() should always succeed here.  The
                 * only error that could be returned is EDQUOT when we are
                 * trying to change the UID or GID of the inode and
                 * HAVE_QUOTA_SUPPORT is defined.  That is not the case here,
                 * however, since the inode was created in osd_mkfile() with
                 * the desired UID and GID already.
                 */
                LASSERTF(result == 0, "%d", result);
                inode->i_sb->s_op->dirty_inode(inode);
        }

        attr->la_valid = valid;
}

/**
 * Helper function for osd_object_create()
 *
 * \retval 0, on success
 */
static int __osd_object_create(struct osd_thread_info *info,
                               struct osd_object *obj, struct lu_attr *attr,
                               struct dt_allocation_hint *hint,
                               struct dt_object_format *dof,
                               struct thandle *th)
{

        int result;

        /*
         * we drop umask so that permissions we pass are not affected
         */
        current->fs->umask = 0;

        result = osd_create_type_f(dof->dof_type)(info, obj, attr, hint, dof,
                                                  th);
        if (result == 0) {
                osd_attr_init(info, obj, attr, dof);
                osd_object_init0(obj);
                /* bz 24037 */
                if (obj->oo_inode && (obj->oo_inode->i_state & I_NEW))
                        unlock_new_inode(obj->oo_inode);
        }
        return result;
}

/**
 * Helper function for osd_object_create()
 *
 * \retval 0, on success
 */
static int __osd_oi_insert(const struct lu_env *env, struct osd_object *obj,
                           const struct lu_fid *fid, struct thandle *th)
{
        struct osd_thread_info *info = osd_oti_get(env);
        struct osd_inode_id    *id;
        struct osd_device      *osd  = osd_obj2dev(obj);
#if 0
        struct md_ucred        *uc   = md_ucred(env);
        LASSERT(uc != NULL);
#else
        struct md_ucred        *uc   = NULL;
#endif
        LASSERT(info);
        id = &info->oti_id;
        LASSERT(obj->oo_inode != NULL);

        id->oii_ino = obj->oo_inode->i_ino;
        id->oii_gen = obj->oo_inode->i_generation;

        return osd_oi_insert(info, osd, fid, id, th,
                             uc ? uc->mu_cap & CFS_CAP_SYS_RESOURCE_MASK : 1);
}

static int osd_declare_object_create(const struct lu_env *env,
                                     struct dt_object *dt,
                                     struct lu_attr *attr,
                                     struct dt_allocation_hint *hint,
                                     struct dt_object_format *dof,
                                     struct thandle *handle)
{
        struct osd_thandle *oh;

        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, create);
        oh->ot_credits += osd_dto_credits_noquota[DTO_OBJECT_CREATE];
        /* XXX: So far, only normal fid needs be inserted into the oi,
         *      things could be changed later. Revise following code then. */
        if (fid_is_norm(lu_object_fid(&dt->do_lu))) {
                OSD_DECLARE_OP(oh, insert);
                oh->ot_credits += osd_dto_credits_noquota[DTO_INDEX_INSERT];
        }
        /* If this is directory, then we expect . and .. to be inserted as
         * well. The one directory block always needs to be created for the
         * directory, so we could use DTO_WRITE_BASE here (GDT, block bitmap,
         * block), there is no danger of needing a tree for the first block.
         */
        if (attr && S_ISDIR(attr->la_mode)) {
                OSD_DECLARE_OP(oh, insert);
                OSD_DECLARE_OP(oh, insert);
                oh->ot_credits += osd_dto_credits_noquota[DTO_WRITE_BASE];
        }

        if (attr) {
                osd_declare_qid(dt, oh, USRQUOTA, attr->la_uid, NULL);
                osd_declare_qid(dt, oh, GRPQUOTA, attr->la_gid, NULL);
        }
        return 0;
}

static int osd_object_create(const struct lu_env *env, struct dt_object *dt,
                             struct lu_attr *attr,
                             struct dt_allocation_hint *hint,
                             struct dt_object_format *dof,
                             struct thandle *th)
{
        const struct lu_fid    *fid    = lu_object_fid(&dt->do_lu);
        struct osd_object      *obj    = osd_dt_obj(dt);
        struct osd_thread_info *info   = osd_oti_get(env);
        int result;

        ENTRY;

        LINVRNT(osd_invariant(obj));
        LASSERT(!dt_object_exists(dt));
        LASSERT(osd_write_locked(env, obj));
        LASSERT(th != NULL);

        OSD_EXEC_OP(th, create);

        result = __osd_object_create(info, obj, attr, hint, dof, th);
        if (result == 0)
                result = __osd_oi_insert(env, obj, fid, th);

        LASSERT(ergo(result == 0, dt_object_exists(dt)));
        LASSERT(osd_invariant(obj));
        RETURN(result);
}

/**
 * Called to destroy on-disk representation of the object
 *
 * Concurrency: must be locked
 */
static int osd_declare_object_destroy(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct thandle *th)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct inode       *inode = obj->oo_inode;
        struct osd_thandle *oh;

        ENTRY;

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);
        LASSERT(inode);

        OSD_DECLARE_OP(oh, destroy);
        OSD_DECLARE_OP(oh, delete);
        oh->ot_credits += osd_dto_credits_noquota[DTO_OBJECT_DELETE];
        oh->ot_credits += osd_dto_credits_noquota[DTO_INDEX_DELETE];

        osd_declare_qid(dt, oh, USRQUOTA, inode->i_uid, inode);
        osd_declare_qid(dt, oh, GRPQUOTA, inode->i_gid, inode);

        RETURN(0);
}

static int osd_object_destroy(const struct lu_env *env,
                              struct dt_object *dt,
                              struct thandle *th)
{
        const struct lu_fid    *fid = lu_object_fid(&dt->do_lu);
        struct osd_object      *obj = osd_dt_obj(dt);
        struct inode           *inode = obj->oo_inode;
        struct osd_device      *osd = osd_obj2dev(obj);
        struct osd_thandle     *oh;
        int                     result;
        ENTRY;

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle);
        LASSERT(inode);
        LASSERT(!lu_object_is_dying(dt->do_lu.lo_header));

        if (S_ISDIR(inode->i_mode)) {
                LASSERT(osd_inode_unlinked(inode) ||
                        inode->i_nlink == 1);
                cfs_spin_lock(&obj->oo_guard);
                inode->i_nlink = 0;
                cfs_spin_unlock(&obj->oo_guard);
                inode->i_sb->s_op->dirty_inode(inode);
        } else {
                LASSERT(osd_inode_unlinked(inode));
        }

        OSD_EXEC_OP(th, destroy);

        result = osd_oi_delete(osd_oti_get(env), osd, fid, th);

        /* XXX: add to ext3 orphan list */
        /* rc = ext3_orphan_add(handle_t *handle, struct inode *inode) */

        /* not needed in the cache anymore */
        set_bit(LU_OBJECT_HEARD_BANSHEE, &dt->do_lu.lo_header->loh_flags);

        RETURN(0);
}

/**
 * Helper function for osd_xattr_set()
 */
static int __osd_xattr_set(const struct lu_env *env, struct dt_object *dt,
                           const struct lu_buf *buf, const char *name, int fl)
{
        struct osd_object      *obj      = osd_dt_obj(dt);
        struct inode           *inode    = obj->oo_inode;
        struct osd_thread_info *info     = osd_oti_get(env);
        struct dentry          *dentry   = &info->oti_child_dentry;
        int                     fs_flags = 0;
        int                     rc;

        LASSERT(dt_object_exists(dt));
        LASSERT(inode->i_op != NULL && inode->i_op->setxattr != NULL);

        if (fl & LU_XATTR_REPLACE)
                fs_flags |= XATTR_REPLACE;

        if (fl & LU_XATTR_CREATE)
                fs_flags |= XATTR_CREATE;

        dentry->d_inode = inode;
        rc = inode->i_op->setxattr(dentry, name, buf->lb_buf,
                                   buf->lb_len, fs_flags);
        return rc;
}

/**
 * Helper function to form igif
 */
static inline void osd_igif_get(const struct lu_env *env, struct inode  *inode,
                                struct lu_fid *fid)
{
        LU_IGIF_BUILD(fid, inode->i_ino, inode->i_generation);
}

/**
 * ldiskfs supports fid in dirent, it is passed in dentry->d_fsdata.
 * lustre 1.8 also uses d_fsdata for passing other info to ldiskfs.
 * To have compatilibility with 1.8 ldiskfs driver we need to have
 * magic number at start of fid data.
 * \ldiskfs_dentry_param is used only to pass fid from osd to ldiskfs.
 * its inmemory API.
 */
void osd_get_ldiskfs_dirent_param(struct ldiskfs_dentry_param *param,
                                  const struct dt_rec *fid)
{
        param->edp_magic = LDISKFS_LUFID_MAGIC;
        param->edp_len =  sizeof(struct lu_fid) + 1;

        fid_cpu_to_be((struct lu_fid *)param->edp_data,
                      (struct lu_fid *)fid);
}

/**
 * Try to read the fid from inode ea into dt_rec, if return value
 * i.e. rc is +ve, then we got fid, otherwise we will have to form igif
 *
 * \param fid object fid.
 *
 * \retval 0 on success
 */
static int osd_ea_fid_get(const struct lu_env *env, struct osd_object *obj,
                          __u32 ino, struct lu_fid *fid)
{
        struct osd_thread_info  *info      = osd_oti_get(env);
        struct lustre_mdt_attrs *mdt_attrs = &info->oti_mdt_attrs;
        struct lu_device        *ldev   = obj->oo_dt.do_lu.lo_dev;
        struct dentry           *dentry = &info->oti_child_dentry;
        struct osd_inode_id     *id     = &info->oti_id;
        struct osd_device       *dev;
        struct inode            *inode;
        int                      rc;

        ENTRY;
        dev  = osd_dev(ldev);

        id->oii_ino = ino;
        id->oii_gen = OSD_OII_NOGEN;

        inode = osd_iget(info, dev, id);
        if (IS_ERR(inode)) {
                rc = PTR_ERR(inode);
                GOTO(out,rc);
        }
        dentry->d_inode = inode;

        LASSERT(inode->i_op != NULL && inode->i_op->getxattr != NULL);
        rc = inode->i_op->getxattr(dentry, XATTR_NAME_LMA, (void *)mdt_attrs,
                                   sizeof *mdt_attrs);

        /* Check LMA compatibility */
        if (rc > 0 &&
            (mdt_attrs->lma_incompat & ~cpu_to_le32(LMA_INCOMPAT_SUPP))) {
                CWARN("Inode %lx: Unsupported incompat LMA feature(s) %#x\n",
                      inode->i_ino, le32_to_cpu(mdt_attrs->lma_incompat) &
                      ~LMA_INCOMPAT_SUPP);
                return -ENOSYS;
        }

        if (rc > 0) {
                lustre_lma_swab(mdt_attrs);
                memcpy(fid, &mdt_attrs->lma_self_fid, sizeof(*fid));
                rc = 0;
        } else if (rc == -ENODATA) {
                osd_igif_get(env, inode, fid);
                rc = 0;
        }
        iput(inode);
out:
        RETURN(rc);
}

/**
 * OSD layer object create function for interoperability mode (b11826).
 * This is mostly similar to osd_object_create(). Only difference being, fid is
 * inserted into inode ea here.
 *
 * \retval   0, on success
 * \retval -ve, on error
 */
static int osd_object_ea_create(const struct lu_env *env, struct dt_object *dt,
                                struct lu_attr *attr,
                                struct dt_allocation_hint *hint,
                                struct dt_object_format *dof,
                                struct thandle *th)
{
        const struct lu_fid    *fid    = lu_object_fid(&dt->do_lu);
        struct osd_object      *obj    = osd_dt_obj(dt);
        struct osd_thread_info *info   = osd_oti_get(env);
        int                     result;

        ENTRY;

        LASSERT(osd_invariant(obj));
        LASSERT(!dt_object_exists(dt));
        LASSERT(osd_write_locked(env, obj));
        LASSERT(th != NULL);

        OSD_EXEC_OP(th, create);

        result = __osd_object_create(info, obj, attr, hint, dof, th);
        if (result == 0)
                result = __osd_oi_insert(env, obj, fid, th);

        LASSERT(ergo(result == 0, dt_object_exists(dt)));
        LINVRNT(osd_invariant(obj));
        RETURN(result);
}

static int osd_declare_object_ref_add(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct thandle *handle)
{
        struct osd_thandle *oh;

        /* it's possible that object doesn't exist yet */
        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, ref_add);
        oh->ot_credits += osd_dto_credits_noquota[DTO_ATTR_SET_BASE];

        return 0;
}

/*
 * Concurrency: @dt is write locked.
 */
static int osd_object_ref_add(const struct lu_env *env,
                              struct dt_object *dt, struct thandle *th)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct inode      *inode = obj->oo_inode;

        LINVRNT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(osd_write_locked(env, obj));
        LASSERT(th != NULL);

        OSD_EXEC_OP(th, ref_add);

        /*
         * DIR_NLINK feature is set for compatibility reasons if:
         * 1) nlinks > LDISKFS_LINK_MAX, or
         * 2) nlinks == 2, since this indicates i_nlink was previously 1.
         *
         * It is easier to always set this flag (rather than check and set),
         * since it has less overhead, and the superblock will be dirtied
         * at some point. Both e2fsprogs and any Lustre-supported ldiskfs
         * do not actually care whether this flag is set or not.
         */
        cfs_spin_lock(&obj->oo_guard);
        inode->i_nlink++;
        if (S_ISDIR(inode->i_mode) && inode->i_nlink > 1) {
                if (inode->i_nlink >= LDISKFS_LINK_MAX ||
                    inode->i_nlink == 2)
                        inode->i_nlink = 1;
        }
        LASSERT(inode->i_nlink < LDISKFS_LINK_MAX);
        cfs_spin_unlock(&obj->oo_guard);
        inode->i_sb->s_op->dirty_inode(inode);
        LINVRNT(osd_invariant(obj));

        return 0;
}

static int osd_declare_object_ref_del(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct thandle *handle)
{
        struct osd_thandle *oh;

        LASSERT(dt_object_exists(dt));
        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, ref_del);
        oh->ot_credits += osd_dto_credits_noquota[DTO_ATTR_SET_BASE];

        return 0;
}

/*
 * Concurrency: @dt is write locked.
 */
static int osd_object_ref_del(const struct lu_env *env, struct dt_object *dt,
                              struct thandle *th)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct inode      *inode = obj->oo_inode;

        LINVRNT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(osd_write_locked(env, obj));
        LASSERT(th != NULL);

        OSD_EXEC_OP(th, ref_del);

        cfs_spin_lock(&obj->oo_guard);
        LASSERT(inode->i_nlink > 0);
        inode->i_nlink--;
        /* If this is/was a many-subdir directory (nlink > LDISKFS_LINK_MAX)
         * then the nlink count is 1. Don't let it be set to 0 or the directory
         * inode will be deleted incorrectly. */
        if (S_ISDIR(inode->i_mode) && inode->i_nlink == 0)
                inode->i_nlink++;
        cfs_spin_unlock(&obj->oo_guard);
        inode->i_sb->s_op->dirty_inode(inode);
        LINVRNT(osd_invariant(obj));

        return 0;
}

/*
 * Get the 64-bit version for an inode.
 */
static int osd_object_version_get(const struct lu_env *env,
                                  struct dt_object *dt, dt_obj_version_t *ver)
{
        struct inode *inode = osd_dt_obj(dt)->oo_inode;

        CDEBUG(D_INODE, "Get version "LPX64" for inode %lu\n",
               LDISKFS_I(inode)->i_fs_version, inode->i_ino);
        *ver = LDISKFS_I(inode)->i_fs_version;
        return 0;
}

/*
 * Concurrency: @dt is read locked.
 */
static int osd_xattr_get(const struct lu_env *env, struct dt_object *dt,
                         struct lu_buf *buf, const char *name,
                         struct lustre_capa *capa)
{
        struct osd_object      *obj    = osd_dt_obj(dt);
        struct inode           *inode  = obj->oo_inode;
        struct osd_thread_info *info   = osd_oti_get(env);
        struct dentry          *dentry = &info->oti_obj_dentry;

        /* version get is not real XATTR but uses xattr API */
        if (strcmp(name, XATTR_NAME_VERSION) == 0) {
                /* for version we are just using xattr API but change inode
                 * field instead */
                LASSERT(buf->lb_len == sizeof(dt_obj_version_t));
                osd_object_version_get(env, dt, buf->lb_buf);
                return sizeof(dt_obj_version_t);
        }

        LASSERT(dt_object_exists(dt));
        LASSERT(inode->i_op != NULL && inode->i_op->getxattr != NULL);
        LASSERT(osd_read_locked(env, obj) || osd_write_locked(env, obj));

        if (osd_object_auth(env, dt, capa, CAPA_OPC_META_READ))
                return -EACCES;

        dentry->d_inode = inode;
        return inode->i_op->getxattr(dentry, name, buf->lb_buf, buf->lb_len);
}


static int osd_declare_xattr_set(const struct lu_env *env,
                                 struct dt_object *dt,
                                 const struct lu_buf *buf, const char *name,
                                 int fl, struct thandle *handle)
{
        struct osd_thandle *oh;

        LASSERT(handle != NULL);

        if (strcmp(name, XATTR_NAME_VERSION) == 0) {
                /* no credits for version */
                return 0;
        }

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, xattr_set);
        oh->ot_credits += osd_dto_credits_noquota[DTO_XATTR_SET];

        return 0;
}

/*
 * Set the 64-bit version for object
 */
static void osd_object_version_set(const struct lu_env *env,
                                   struct dt_object *dt,
                                   dt_obj_version_t *new_version)
{
        struct inode *inode = osd_dt_obj(dt)->oo_inode;

        CDEBUG(D_INODE, "Set version "LPX64" (old "LPX64") for inode %lu\n",
               *new_version, LDISKFS_I(inode)->i_fs_version, inode->i_ino);

        LDISKFS_I(inode)->i_fs_version = *new_version;
        /** Version is set after all inode operations are finished,
         *  so we should mark it dirty here */
        inode->i_sb->s_op->dirty_inode(inode);
}

/*
 * Concurrency: @dt is write locked.
 */
static int osd_xattr_set(const struct lu_env *env, struct dt_object *dt,
                         const struct lu_buf *buf, const char *name, int fl,
                         struct thandle *handle, struct lustre_capa *capa)
{
        LASSERT(handle != NULL);

        /* version set is not real XATTR */
        if (strcmp(name, XATTR_NAME_VERSION) == 0) {
                /* for version we are just using xattr API but change inode
                 * field instead */
                LASSERT(buf->lb_len == sizeof(dt_obj_version_t));
                osd_object_version_set(env, dt, buf->lb_buf);
                return sizeof(dt_obj_version_t);
        }

        if (osd_object_auth(env, dt, capa, CAPA_OPC_META_WRITE))
                return -EACCES;

        OSD_EXEC_OP(handle, xattr_set);
        return __osd_xattr_set(env, dt, buf, name, fl);
}

/*
 * Concurrency: @dt is read locked.
 */
static int osd_xattr_list(const struct lu_env *env, struct dt_object *dt,
                          struct lu_buf *buf, struct lustre_capa *capa)
{
        struct osd_object      *obj    = osd_dt_obj(dt);
        struct inode           *inode  = obj->oo_inode;
        struct osd_thread_info *info   = osd_oti_get(env);
        struct dentry          *dentry = &info->oti_obj_dentry;

        LASSERT(dt_object_exists(dt));
        LASSERT(inode->i_op != NULL && inode->i_op->listxattr != NULL);
        LASSERT(osd_read_locked(env, obj) || osd_write_locked(env, obj));

        if (osd_object_auth(env, dt, capa, CAPA_OPC_META_READ))
                return -EACCES;

        dentry->d_inode = inode;
        return inode->i_op->listxattr(dentry, buf->lb_buf, buf->lb_len);
}

static int osd_declare_xattr_del(const struct lu_env *env,
                                 struct dt_object *dt, const char *name,
                                 struct thandle *handle)
{
        struct osd_thandle *oh;

        LASSERT(dt_object_exists(dt));
        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, xattr_set);
        oh->ot_credits += osd_dto_credits_noquota[DTO_XATTR_SET];

        return 0;
}

/*
 * Concurrency: @dt is write locked.
 */
static int osd_xattr_del(const struct lu_env *env, struct dt_object *dt,
                         const char *name, struct thandle *handle,
                         struct lustre_capa *capa)
{
        struct osd_object      *obj    = osd_dt_obj(dt);
        struct inode           *inode  = obj->oo_inode;
        struct osd_thread_info *info   = osd_oti_get(env);
        struct dentry          *dentry = &info->oti_obj_dentry;
        int                     rc;

        LASSERT(dt_object_exists(dt));
        LASSERT(inode->i_op != NULL && inode->i_op->removexattr != NULL);
        LASSERT(osd_write_locked(env, obj));
        LASSERT(handle != NULL);

        if (osd_object_auth(env, dt, capa, CAPA_OPC_META_WRITE))
                return -EACCES;

        OSD_EXEC_OP(handle, xattr_set);

        dentry->d_inode = inode;
        rc = inode->i_op->removexattr(dentry, name);
        return rc;
}

static struct obd_capa *osd_capa_get(const struct lu_env *env,
                                     struct dt_object *dt,
                                     struct lustre_capa *old,
                                     __u64 opc)
{
        struct osd_thread_info *info = osd_oti_get(env);
        const struct lu_fid *fid = lu_object_fid(&dt->do_lu);
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_device *dev = osd_obj2dev(obj);
        struct lustre_capa_key *key = &info->oti_capa_key;
        struct lustre_capa *capa = &info->oti_capa;
        struct obd_capa *oc;
        struct md_capainfo *ci;
        int rc;
        ENTRY;

        if (!dev->od_fl_capa)
                RETURN(ERR_PTR(-ENOENT));

        LASSERT(dt_object_exists(dt));
        LINVRNT(osd_invariant(obj));

        /* renewal sanity check */
        if (old && osd_object_auth(env, dt, old, opc))
                RETURN(ERR_PTR(-EACCES));

        ci = md_capainfo(env);
        if (unlikely(!ci))
                RETURN(ERR_PTR(-ENOENT));

        switch (ci->mc_auth) {
        case LC_ID_NONE:
                RETURN(NULL);
        case LC_ID_PLAIN:
                capa->lc_uid = obj->oo_inode->i_uid;
                capa->lc_gid = obj->oo_inode->i_gid;
                capa->lc_flags = LC_ID_PLAIN;
                break;
        case LC_ID_CONVERT: {
                __u32 d[4], s[4];

                s[0] = obj->oo_inode->i_uid;
                cfs_get_random_bytes(&(s[1]), sizeof(__u32));
                s[2] = obj->oo_inode->i_gid;
                cfs_get_random_bytes(&(s[3]), sizeof(__u32));
                rc = capa_encrypt_id(d, s, key->lk_key, CAPA_HMAC_KEY_MAX_LEN);
                if (unlikely(rc))
                        RETURN(ERR_PTR(rc));

                capa->lc_uid   = ((__u64)d[1] << 32) | d[0];
                capa->lc_gid   = ((__u64)d[3] << 32) | d[2];
                capa->lc_flags = LC_ID_CONVERT;
                break;
        }
        default:
                RETURN(ERR_PTR(-EINVAL));
        }

        capa->lc_fid = *fid;
        capa->lc_opc = opc;
        capa->lc_flags |= dev->od_capa_alg << 24;
        capa->lc_timeout = dev->od_capa_timeout;
        capa->lc_expiry = 0;

        oc = capa_lookup(dev->od_capa_hash, capa, 1);
        if (oc) {
                LASSERT(!capa_is_expired(oc));
                RETURN(oc);
        }

        cfs_spin_lock(&capa_lock);
        *key = dev->od_capa_keys[1];
        cfs_spin_unlock(&capa_lock);

        capa->lc_keyid = key->lk_keyid;
        capa->lc_expiry = cfs_time_current_sec() + dev->od_capa_timeout;

        rc = capa_hmac(capa->lc_hmac, capa, key->lk_key);
        if (rc) {
                DEBUG_CAPA(D_ERROR, capa, "HMAC failed: %d for", rc);
                RETURN(ERR_PTR(rc));
        }

        oc = capa_add(dev->od_capa_hash, capa);
        RETURN(oc);
}

static int osd_object_sync(const struct lu_env *env, struct dt_object *dt)
{
        struct osd_object      *obj    = osd_dt_obj(dt);
        struct inode           *inode  = obj->oo_inode;
        struct osd_thread_info *info   = osd_oti_get(env);
        struct dentry          *dentry = &info->oti_obj_dentry;
        struct file            *file   = &info->oti_file;
        int                     rc;

        ENTRY;

        dentry->d_inode = inode;
        file->f_dentry = dentry;
        file->f_mapping = inode->i_mapping;
        file->f_op = inode->i_fop;
        LOCK_INODE_MUTEX(inode);
        rc = file->f_op->fsync(file, dentry, 0);
        UNLOCK_INODE_MUTEX(inode);
        RETURN(rc);
}

/*
 * Index operations.
 */

static int osd_iam_index_probe(const struct lu_env *env, struct osd_object *o,
                           const struct dt_index_features *feat)
{
        struct iam_descr *descr;

        if (osd_object_is_root(o))
                return feat == &dt_directory_features;

        LASSERT(o->oo_dir != NULL);

        descr = o->oo_dir->od_container.ic_descr;
        if (feat == &dt_directory_features) {
                if (descr->id_rec_size == sizeof(struct osd_fid_pack))
                        return 1;
                else
                        return 0;
        } else {
                return
                        feat->dif_keysize_min <= descr->id_key_size &&
                        descr->id_key_size <= feat->dif_keysize_max &&
                        feat->dif_recsize_min <= descr->id_rec_size &&
                        descr->id_rec_size <= feat->dif_recsize_max &&
                        !(feat->dif_flags & (DT_IND_VARKEY |
                                             DT_IND_VARREC | DT_IND_NONUNQ)) &&
                        ergo(feat->dif_flags & DT_IND_UPDATE,
                             1 /* XXX check that object (and file system) is
                                * writable */);
        }
}

static int osd_iam_container_init(const struct lu_env *env,
                                  struct osd_object *obj,
                                  struct osd_directory *dir)
{
        struct iam_container *bag = &dir->od_container;
        int result;

        result = iam_container_init(bag, &dir->od_descr, obj->oo_inode);
        if (result != 0)
                return result;

        result = iam_container_setup(bag);
        if (result != 0)
                goto out;

        if (osd_obj2dev(obj)->od_iop_mode) {
                u32 ptr = bag->ic_descr->id_ops->id_root_ptr(bag);

                bag->ic_root_bh = ldiskfs_bread(NULL, obj->oo_inode,
                                                ptr, 0, &result);
        }

 out:
        if (result == 0)
                obj->oo_dt.do_index_ops = &osd_index_iam_ops;
        else
                iam_container_fini(bag);

        return result;
}


/*
 * Concurrency: no external locking is necessary.
 */
static int osd_index_try(const struct lu_env *env, struct dt_object *dt,
                         const struct dt_index_features *feat)
{
        int                result;
        int                skip_iam = 0;
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_device *osd = osd_obj2dev(obj);

        LINVRNT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));

        if (osd_object_is_root(obj)) {
                dt->do_index_ops = &osd_index_ea_ops;
                result = 0;
        } else if (feat == &dt_directory_features && osd->od_iop_mode) {
                dt->do_index_ops = &osd_index_ea_ops;
                if (S_ISDIR(obj->oo_inode->i_mode))
                        result = 0;
                else
                        result = -ENOTDIR;
                skip_iam = 1;
        } else if (feat == &dt_acct_features) {
                LASSERT(!osd_has_index(obj));
                dt->do_index_ops = &osd_acct_index_ops;
                result = 0;
                skip_iam = 1;
        } else if (!osd_has_index(obj)) {
                struct osd_directory *dir;

                OBD_ALLOC_PTR(dir);
                if (dir != NULL) {

                        cfs_spin_lock(&obj->oo_guard);
                        if (obj->oo_dir == NULL)
                                obj->oo_dir = dir;
                        else
                                /*
                                 * Concurrent thread allocated container data.
                                 */
                                OBD_FREE_PTR(dir);
                        cfs_spin_unlock(&obj->oo_guard);
                        /*
                         * Now, that we have container data, serialize its
                         * initialization.
                         */
                        cfs_down_write(&obj->oo_ext_idx_sem);
                        /*
                         * recheck under lock.
                         */
                        if (!osd_has_index(obj))
                                result = osd_iam_container_init(env, obj, dir);
                        else
                                result = 0;
                        cfs_up_write(&obj->oo_ext_idx_sem);
                } else {
                        result = -ENOMEM;
                }
        } else {
                result = 0;
        }

        if (result == 0 && skip_iam == 0) {
                if (!osd_iam_index_probe(env, obj, feat))
                        result = -ENOTDIR;
        }
        LINVRNT(osd_invariant(obj));

        return result;
}

static const struct dt_object_operations osd_obj_ops = {
        .do_read_lock         = osd_object_read_lock,
        .do_write_lock        = osd_object_write_lock,
        .do_read_unlock       = osd_object_read_unlock,
        .do_write_unlock      = osd_object_write_unlock,
        .do_write_locked      = osd_object_write_locked,
        .do_attr_get          = osd_attr_get,
        .do_declare_attr_set  = osd_declare_attr_set,
        .do_attr_set          = osd_attr_set,
        .do_ah_init           = osd_ah_init,
        .do_declare_create    = osd_declare_object_create,
        .do_create            = osd_object_create,
        .do_declare_destroy   = osd_declare_object_destroy,
        .do_destroy           = osd_object_destroy,
        .do_index_try         = osd_index_try,
        .do_declare_ref_add   = osd_declare_object_ref_add,
        .do_ref_add           = osd_object_ref_add,
        .do_declare_ref_del   = osd_declare_object_ref_del,
        .do_ref_del           = osd_object_ref_del,
        .do_xattr_get         = osd_xattr_get,
        .do_declare_xattr_set = osd_declare_xattr_set,
        .do_xattr_set         = osd_xattr_set,
        .do_declare_xattr_del = osd_declare_xattr_del,
        .do_xattr_del         = osd_xattr_del,
        .do_xattr_list        = osd_xattr_list,
        .do_capa_get          = osd_capa_get,
        .do_object_sync       = osd_object_sync,
};

/**
 * dt_object_operations for interoperability mode
 * (i.e. to run 2.0 mds on 1.8 disk) (b11826)
 */
static const struct dt_object_operations osd_obj_ea_ops = {
        .do_read_lock         = osd_object_read_lock,
        .do_write_lock        = osd_object_write_lock,
        .do_read_unlock       = osd_object_read_unlock,
        .do_write_unlock      = osd_object_write_unlock,
        .do_write_locked      = osd_object_write_locked,
        .do_attr_get          = osd_attr_get,
        .do_declare_attr_set  = osd_declare_attr_set,
        .do_attr_set          = osd_attr_set,
        .do_ah_init           = osd_ah_init,
        .do_declare_create    = osd_declare_object_create,
        .do_create            = osd_object_ea_create,
        .do_declare_destroy   = osd_declare_object_destroy,
        .do_destroy           = osd_object_destroy,
        .do_index_try         = osd_index_try,
        .do_declare_ref_add   = osd_declare_object_ref_add,
        .do_ref_add           = osd_object_ref_add,
        .do_declare_ref_del   = osd_declare_object_ref_del,
        .do_ref_del           = osd_object_ref_del,
        .do_xattr_get         = osd_xattr_get,
        .do_declare_xattr_set = osd_declare_xattr_set,
        .do_xattr_set         = osd_xattr_set,
        .do_declare_xattr_del = osd_declare_xattr_del,
        .do_xattr_del         = osd_xattr_del,
        .do_xattr_list        = osd_xattr_list,
        .do_capa_get          = osd_capa_get,
        .do_object_sync       = osd_object_sync,
};

static int osd_index_declare_iam_delete(const struct lu_env *env,
                                        struct dt_object *dt,
                                        const struct dt_key *key,
                                        struct thandle *handle)
{
        struct osd_thandle    *oh;

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, delete);
        oh->ot_credits += osd_dto_credits_noquota[DTO_INDEX_DELETE];

        return 0;
}

/**
 *      delete a (key, value) pair from index \a dt specified by \a key
 *
 *      \param  dt      osd index object
 *      \param  key     key for index
 *      \param  rec     record reference
 *      \param  handle  transaction handler
 *
 *      \retval  0  success
 *      \retval -ve   failure
 */

static int osd_index_iam_delete(const struct lu_env *env, struct dt_object *dt,
                                const struct dt_key *key,
                                struct thandle *handle,
                                struct lustre_capa *capa)
{
        struct osd_object     *obj = osd_dt_obj(dt);
        struct osd_thandle    *oh;
        struct iam_path_descr *ipd;
        struct iam_container  *bag = &obj->oo_dir->od_container;
        int                    rc;

        ENTRY;

        LINVRNT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(bag->ic_object == obj->oo_inode);
        LASSERT(handle != NULL);

        if (osd_object_auth(env, dt, capa, CAPA_OPC_INDEX_DELETE))
                RETURN(-EACCES);

        OSD_EXEC_OP(handle, delete);

        ipd = osd_idx_ipd_get(env, bag);
        if (unlikely(ipd == NULL))
                RETURN(-ENOMEM);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle != NULL);
        LASSERT(oh->ot_handle->h_transaction != NULL);

        rc = iam_delete(oh->ot_handle, bag, (const struct iam_key *)key, ipd);
        osd_ipd_put(env, bag, ipd);
        LINVRNT(osd_invariant(obj));
        RETURN(rc);
}

static int osd_index_declare_ea_delete(const struct lu_env *env,
                                       struct dt_object *dt,
                                       const struct dt_key *key,
                                       struct thandle *handle)
{
        struct osd_thandle *oh;

        LASSERT(dt_object_exists(dt));
        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, delete);
        oh->ot_credits += osd_dto_credits_noquota[DTO_INDEX_DELETE];

        LASSERT(osd_dt_obj(dt)->oo_inode);
        osd_declare_qid(dt, oh, USRQUOTA, osd_dt_obj(dt)->oo_inode->i_uid,
                        osd_dt_obj(dt)->oo_inode);
        osd_declare_qid(dt, oh, GRPQUOTA, osd_dt_obj(dt)->oo_inode->i_gid,
                        osd_dt_obj(dt)->oo_inode);

        return 0;
}

static inline int osd_get_fid_from_dentry(struct ldiskfs_dir_entry_2 *de,
                                          struct dt_rec *fid)
{
        struct osd_fid_pack *rec;
        int                  rc = -ENODATA;

        if (de->file_type & LDISKFS_DIRENT_LUFID) {
                rec = (struct osd_fid_pack *) (de->name + de->name_len + 1);
                rc = osd_fid_unpack((struct lu_fid *)fid, rec);
        }
        RETURN(rc);
}

/**
 * Index delete function for interoperability mode (b11826).
 * It will remove the directory entry added by osd_index_ea_insert().
 * This entry is needed to maintain name->fid mapping.
 *
 * \param key,  key i.e. file entry to be deleted
 *
 * \retval   0, on success
 * \retval -ve, on error
 */
static int osd_index_ea_delete(const struct lu_env *env, struct dt_object *dt,
                               const struct dt_key *key,
                               struct thandle *handle,
                               struct lustre_capa *capa)
{
        struct osd_object          *obj    = osd_dt_obj(dt);
        struct inode               *dir    = obj->oo_inode;
        struct dentry              *dentry;
        struct osd_thandle         *oh;
        struct ldiskfs_dir_entry_2 *de;
        struct buffer_head         *bh;
        struct htree_lock          *hlock = NULL;
        int                         rc;

        ENTRY;

        LINVRNT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(handle != NULL);

        OSD_EXEC_OP(handle, delete);

        oh = container_of(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle != NULL);
        LASSERT(oh->ot_handle->h_transaction != NULL);

        if (osd_object_auth(env, dt, capa, CAPA_OPC_INDEX_DELETE))
                RETURN(-EACCES);

        dentry = osd_child_dentry_get(env, obj,
                                      (char *)key, strlen((char *)key));

        if (obj->oo_hl_head != NULL) {
                hlock = osd_oti_get(env)->oti_hlock;
                ldiskfs_htree_lock(hlock, obj->oo_hl_head,
                                   dir, LDISKFS_HLOCK_DEL);
        } else {
                cfs_down_write(&obj->oo_ext_idx_sem);
        }

        bh = osd_ldiskfs_find_entry(dir, dentry, &de, hlock);
        if (bh) {
                rc = ldiskfs_delete_entry(oh->ot_handle, dir, de, bh);
                brelse(bh);
        } else {
                rc = -ENOENT;
        }
        if (hlock != NULL)
                ldiskfs_htree_unlock(hlock);
        else
                cfs_up_write(&obj->oo_ext_idx_sem);

        LASSERT(osd_invariant(obj));
        RETURN(rc);
}

/**
 *      Lookup index for \a key and copy record to \a rec.
 *
 *      \param  dt      osd index object
 *      \param  key     key for index
 *      \param  rec     record reference
 *
 *      \retval  +ve  success : exact mach
 *      \retval  0    return record with key not greater than \a key
 *      \retval -ve   failure
 */
static int osd_index_iam_lookup(const struct lu_env *env, struct dt_object *dt,
                                struct dt_rec *rec, const struct dt_key *key,
                                struct lustre_capa *capa)
{
        struct osd_object      *obj = osd_dt_obj(dt);
        struct iam_path_descr  *ipd;
        struct iam_container   *bag = &obj->oo_dir->od_container;
        struct osd_thread_info *oti = osd_oti_get(env);
        struct iam_iterator    *it = &oti->oti_idx_it;
        struct iam_rec         *iam_rec;
        int                     rc;

        ENTRY;

        LASSERT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(bag->ic_object == obj->oo_inode);

        if (osd_object_auth(env, dt, capa, CAPA_OPC_INDEX_LOOKUP))
                RETURN(-EACCES);

        ipd = osd_idx_ipd_get(env, bag);
        if (IS_ERR(ipd))
                RETURN(-ENOMEM);

        /* got ipd now we can start iterator. */
        iam_it_init(it, bag, 0, ipd);

        rc = iam_it_get(it, (struct iam_key *)key);
        if (rc >= 0) {
                if (S_ISDIR(obj->oo_inode->i_mode))
                        iam_rec = (struct iam_rec *)oti->oti_ldp;
                else
                        iam_rec = (struct iam_rec *) rec;

                iam_reccpy(&it->ii_path.ip_leaf, (struct iam_rec *)iam_rec);
                if (S_ISDIR(obj->oo_inode->i_mode))
                        osd_fid_unpack((struct lu_fid *) rec,
                                       (struct osd_fid_pack *)iam_rec);
        }
        iam_it_put(it);
        iam_it_fini(it);
        osd_ipd_put(env, bag, ipd);

        LINVRNT(osd_invariant(obj));

        RETURN(rc);
}

static int osd_index_declare_iam_insert(const struct lu_env *env,
                                        struct dt_object *dt,
                                        const struct dt_rec *rec,
                                        const struct dt_key *key,
                                        struct thandle *handle)
{
        struct osd_thandle *oh;

        LASSERT(dt_object_exists(dt));
        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, insert);
        oh->ot_credits += osd_dto_credits_noquota[DTO_INDEX_INSERT];

        return 0;
}

/**
 *      Inserts (key, value) pair in \a dt index object.
 *
 *      \param  dt      osd index object
 *      \param  key     key for index
 *      \param  rec     record reference
 *      \param  th      transaction handler
 *
 *      \retval  0  success
 *      \retval -ve failure
 */
static int osd_index_iam_insert(const struct lu_env *env, struct dt_object *dt,
                                const struct dt_rec *rec,
                                const struct dt_key *key, struct thandle *th,
                                struct lustre_capa *capa, int ignore_quota)
{
        struct osd_object     *obj = osd_dt_obj(dt);
        struct iam_path_descr *ipd;
        struct osd_thandle    *oh;
        struct iam_container  *bag = &obj->oo_dir->od_container;
#ifdef HAVE_QUOTA_SUPPORT
        cfs_cap_t              save = cfs_curproc_cap_pack();
#endif
        struct osd_thread_info *oti = osd_oti_get(env);
        struct iam_rec         *iam_rec = (struct iam_rec *)oti->oti_ldp;
        int                     rc;

        ENTRY;

        LINVRNT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(bag->ic_object == obj->oo_inode);
        LASSERT(th != NULL);

        if (osd_object_auth(env, dt, capa, CAPA_OPC_INDEX_INSERT))
                return -EACCES;

        OSD_EXEC_OP(th, insert);

        ipd = osd_idx_ipd_get(env, bag);
        if (unlikely(ipd == NULL))
                RETURN(-ENOMEM);

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle != NULL);
        LASSERT(oh->ot_handle->h_transaction != NULL);
#ifdef HAVE_QUOTA_SUPPORT
        if (ignore_quota)
                cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
        else
                cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
#endif
        if (S_ISDIR(obj->oo_inode->i_mode))
                osd_fid_pack((struct osd_fid_pack *)iam_rec, rec, &oti->oti_fid);
        else
                iam_rec = (struct iam_rec *) rec;
        rc = iam_insert(oh->ot_handle, bag, (const struct iam_key *)key,
                        iam_rec, ipd);
#ifdef HAVE_QUOTA_SUPPORT
        cfs_curproc_cap_unpack(save);
#endif
        osd_ipd_put(env, bag, ipd);
        LINVRNT(osd_invariant(obj));
        RETURN(rc);
}

/**
 * Calls ldiskfs_add_entry() to add directory entry
 * into the directory. This is required for
 * interoperability mode (b11826)
 *
 * \retval   0, on success
 * \retval -ve, on error
 */
static int __osd_ea_add_rec(struct osd_thread_info *info,
                            struct osd_object *pobj, struct inode  *cinode,
                            const char *name, const struct dt_rec *fid,
                            struct htree_lock *hlock, struct thandle *th)
{
        struct ldiskfs_dentry_param *ldp;
        struct dentry               *child;
        struct osd_thandle          *oth;
        int                          rc;

        oth = container_of(th, struct osd_thandle, ot_super);
        LASSERT(oth->ot_handle != NULL);
        LASSERT(oth->ot_handle->h_transaction != NULL);

        child = osd_child_dentry_get(info->oti_env, pobj, name, strlen(name));

        if (!fid_is_igif((struct lu_fid *)fid)) {
                ldp = (struct ldiskfs_dentry_param *)info->oti_ldp;
                osd_get_ldiskfs_dirent_param(ldp, fid);
                child->d_fsdata = (void *)ldp;
        } else {
                child->d_fsdata = NULL;
        }
        rc = osd_ldiskfs_add_entry(oth->ot_handle, child, cinode, hlock);

        RETURN(rc);
}

/**
 * Calls ldiskfs_add_dot_dotdot() to add dot and dotdot entries
 * into the directory.Also sets flags into osd object to
 * indicate dot and dotdot are created. This is required for
 * interoperability mode (b11826)
 *
 * \param dir   directory for dot and dotdot fixup.
 * \param obj   child object for linking
 *
 * \retval   0, on success
 * \retval -ve, on error
 */
static int osd_add_dot_dotdot(struct osd_thread_info *info,
                              struct osd_object *dir,
                              struct inode  *parent_dir, const char *name,
                              const struct dt_rec *dot_fid,
                              const struct dt_rec *dot_dot_fid,
                              struct thandle *th)
{
        struct inode                *inode = dir->oo_inode;
        struct ldiskfs_dentry_param *dot_ldp;
        struct ldiskfs_dentry_param *dot_dot_ldp;
        struct osd_thandle          *oth;
        int result = 0;

        oth = container_of(th, struct osd_thandle, ot_super);
        LASSERT(oth->ot_handle->h_transaction != NULL);
        LASSERT(S_ISDIR(dir->oo_inode->i_mode));

        if (strcmp(name, dot) == 0) {
                if (dir->oo_compat_dot_created) {
                        result = -EEXIST;
                } else {
                        LASSERT(inode == parent_dir);
                        dir->oo_compat_dot_created = 1;
                        result = 0;
                }
        } else if(strcmp(name, dotdot) == 0) {
                dot_ldp = (struct ldiskfs_dentry_param *)info->oti_ldp;
                dot_dot_ldp = (struct ldiskfs_dentry_param *)info->oti_ldp2;

                if (!dir->oo_compat_dot_created)
                        return -EINVAL;
                if (!fid_is_igif((struct lu_fid *)dot_fid)) {
                        osd_get_ldiskfs_dirent_param(dot_ldp, dot_fid);
                        osd_get_ldiskfs_dirent_param(dot_dot_ldp, dot_dot_fid);
                } else {
                        dot_ldp = NULL;
                        dot_dot_ldp = NULL;
                }
                /* in case of rename, dotdot is already created */
                if (dir->oo_compat_dotdot_created) {
                        return __osd_ea_add_rec(info, dir, parent_dir, name,
                                                dot_dot_fid, NULL, th);
                }

                result = ldiskfs_add_dot_dotdot(oth->ot_handle, parent_dir,
                                                inode, dot_ldp, dot_dot_ldp);
                if (result == 0)
                       dir->oo_compat_dotdot_created = 1;
        }

        return result;
}


/**
 * It will call the appropriate osd_add* function and return the
 * value, return by respective functions.
 */
static int osd_ea_add_rec(const struct lu_env *env, struct osd_object *pobj,
                          struct inode *cinode, const char *name,
                          const struct dt_rec *fid, struct thandle *th)
{
        struct osd_thread_info *info   = osd_oti_get(env);
        struct htree_lock      *hlock;
        int                     rc;

        hlock = pobj->oo_hl_head != NULL ? info->oti_hlock : NULL;

        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' &&
                                                   name[2] =='\0'))) {
                if (hlock != NULL) {
                        ldiskfs_htree_lock(hlock, pobj->oo_hl_head,
                                           pobj->oo_inode, 0);
                } else {
                        cfs_down_write(&pobj->oo_ext_idx_sem);
                }
                rc = osd_add_dot_dotdot(info, pobj, cinode, name,
                     (struct dt_rec *)lu_object_fid(&pobj->oo_dt.do_lu),
                                        fid, th);
        } else {
                if (hlock != NULL) {
                        ldiskfs_htree_lock(hlock, pobj->oo_hl_head,
                                           pobj->oo_inode, LDISKFS_HLOCK_ADD);
                } else {
                        cfs_down_write(&pobj->oo_ext_idx_sem);
                }

                rc = __osd_ea_add_rec(info, pobj, cinode, name, fid,
                                      hlock, th);
        }
        if (hlock != NULL)
                ldiskfs_htree_unlock(hlock);
        else
                cfs_up_write(&pobj->oo_ext_idx_sem);

        return rc;
}

/**
 * Calls ->lookup() to find dentry. From dentry get inode and
 * read inode's ea to get fid. This is required for  interoperability
 * mode (b11826)
 *
 * \retval   0, on success
 * \retval -ve, on error
 */
static int osd_ea_lookup_rec(const struct lu_env *env, struct osd_object *obj,
                             struct dt_rec *rec, const struct dt_key *key)
{
        struct inode               *dir    = obj->oo_inode;
        struct dentry              *dentry;
        struct ldiskfs_dir_entry_2 *de;
        struct buffer_head         *bh;
        struct lu_fid              *fid = (struct lu_fid *) rec;
        struct htree_lock          *hlock = NULL;
        int                         ino;
        int                         rc;

        LASSERT(dir->i_op != NULL && dir->i_op->lookup != NULL);

        dentry = osd_child_dentry_get(env, obj,
                                      (char *)key, strlen((char *)key));

        if (obj->oo_hl_head != NULL) {
                hlock = osd_oti_get(env)->oti_hlock;
                ldiskfs_htree_lock(hlock, obj->oo_hl_head,
                                   dir, LDISKFS_HLOCK_LOOKUP);
        } else {
                cfs_down_read(&obj->oo_ext_idx_sem);
        }

        bh = osd_ldiskfs_find_entry(dir, dentry, &de, hlock);
        if (bh) {
                ino = le32_to_cpu(de->inode);
                rc = osd_get_fid_from_dentry(de, rec);

                /* done with de, release bh */
                brelse(bh);
                if (rc != 0)
                        rc = osd_ea_fid_get(env, obj, ino, fid);
        } else {
                rc = -ENOENT;
        }

        if (hlock != NULL)
                ldiskfs_htree_unlock(hlock);
        else
                cfs_up_read(&obj->oo_ext_idx_sem);
        RETURN (rc);
}

/**
 * Find the osd object for given fid.
 *
 * \param fid need to find the osd object having this fid
 *
 * \retval osd_object on success
 * \retval        -ve on error
 */
struct osd_object *osd_object_find(const struct lu_env *env,
                                   struct dt_object *dt,
                                   const struct lu_fid *fid)
{
        struct lu_device  *ludev = dt->do_lu.lo_dev;
        struct osd_object *child = NULL;
        struct lu_object  *luch;
        struct lu_object  *lo;

        /*
         * at this point topdev might not exist yet
         * (i.e. MGS is preparing profiles). so we can
         * not rely on topdev and instead lookup with
         * our device passed as topdev. this can't work
         * if the object isn't cached yet (as osd doesn't
         * allocate lu_header). IOW, the object must be
         * in the cache, otherwise lu_object_alloc() crashes
         * -bzzz
         */
        luch = lu_object_find_at(env, ludev, fid, NULL);
        if (!IS_ERR(luch)) {
                if (lu_object_exists(luch)) {
                        lo = lu_object_locate(luch->lo_header, ludev->ld_type);
                        if (lo != NULL)
                                child = osd_obj(lo);
                        else
                                LU_OBJECT_DEBUG(D_ERROR, env, luch,
                                                "lu_object can't be located"
                                                ""DFID"\n", PFID(fid));

                        if (child == NULL) {
                                lu_object_put(env, luch);
                                CERROR("Unable to get osd_object\n");
                                child = ERR_PTR(-ENOENT);
                        }
                } else {
                        LU_OBJECT_DEBUG(D_ERROR, env, luch,
                                        "lu_object does not exists "DFID"\n",
                                        PFID(fid));
                        child = ERR_PTR(-ENOENT);
                }
        } else
                child = (void *)luch;

        return child;
}

/**
 * Put the osd object once done with it.
 *
 * \param obj osd object that needs to be put
 */
static inline void osd_object_put(const struct lu_env *env,
                                  struct osd_object *obj)
{
        lu_object_put(env, &obj->oo_dt.do_lu);
}

static int osd_index_declare_ea_insert(const struct lu_env *env,
                                       struct dt_object *dt,
                                       const struct dt_rec *rec,
                                       const struct dt_key *key,
                                       struct thandle *handle)
{
        struct osd_thandle *oh;

        LASSERT(dt_object_exists(dt));
        LASSERT(handle != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle == NULL);

        OSD_DECLARE_OP(oh, insert);
        oh->ot_credits += osd_dto_credits_noquota[DTO_INDEX_INSERT];

        LASSERT(osd_dt_obj(dt)->oo_inode);
        osd_declare_qid(dt, oh, USRQUOTA, osd_dt_obj(dt)->oo_inode->i_uid,
                        osd_dt_obj(dt)->oo_inode);
        osd_declare_qid(dt, oh, GRPQUOTA, osd_dt_obj(dt)->oo_inode->i_gid,
                        osd_dt_obj(dt)->oo_inode);

        return 0;
}

/**
 * Index add function for interoperability mode (b11826).
 * It will add the directory entry.This entry is needed to
 * maintain name->fid mapping.
 *
 * \param key it is key i.e. file entry to be inserted
 * \param rec it is value of given key i.e. fid
 *
 * \retval   0, on success
 * \retval -ve, on error
 */
static int osd_index_ea_insert(const struct lu_env *env, struct dt_object *dt,
                               const struct dt_rec *rec,
                               const struct dt_key *key, struct thandle *th,
                               struct lustre_capa *capa, int ignore_quota)
{
        struct osd_object *obj   = osd_dt_obj(dt);
        struct lu_fid     *fid   = (struct lu_fid *) rec;
        const char        *name  = (const char *)key;
        struct osd_object *child;
#ifdef HAVE_QUOTA_SUPPORT
        cfs_cap_t          save  = cfs_curproc_cap_pack();
#endif
        int                rc;

        ENTRY;

        LASSERT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(th != NULL);

        if (osd_object_auth(env, dt, capa, CAPA_OPC_INDEX_INSERT))
                RETURN(-EACCES);

        OSD_EXEC_OP(th, insert);

        child = osd_object_find(env, dt, fid);
        if (!IS_ERR(child)) {
#ifdef HAVE_QUOTA_SUPPORT
                if (ignore_quota)
                        cfs_cap_raise(CFS_CAP_SYS_RESOURCE);
                else
                        cfs_cap_lower(CFS_CAP_SYS_RESOURCE);
#endif
                rc = osd_ea_add_rec(env, obj, child->oo_inode, name, rec, th);
#ifdef HAVE_QUOTA_SUPPORT
                cfs_curproc_cap_unpack(save);
#endif
                osd_object_put(env, child);
        } else {
                rc = PTR_ERR(child);
        }

        LASSERT(osd_invariant(obj));
        RETURN(rc);
}

/**
 *  Initialize osd Iterator for given osd index object.
 *
 *  \param  dt      osd index object
 */

static struct dt_it *osd_it_iam_init(const struct lu_env *env,
                                     struct dt_object *dt,
                                     __u32 unused,
                                     struct lustre_capa *capa)
{
        struct osd_it_iam      *it;
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_object      *obj = osd_dt_obj(dt);
        struct lu_object       *lo  = &dt->do_lu;
        struct iam_path_descr  *ipd;
        struct iam_container   *bag = &obj->oo_dir->od_container;

        LASSERT(lu_object_exists(lo));

        if (osd_object_auth(env, dt, capa, CAPA_OPC_BODY_READ))
                return ERR_PTR(-EACCES);

        it = &oti->oti_it;
        ipd = osd_it_ipd_get(env, bag);
        if (likely(ipd != NULL)) {
                it->oi_obj = obj;
                it->oi_ipd = ipd;
                lu_object_get(lo);
                iam_it_init(&it->oi_it, bag, IAM_IT_MOVE, ipd);
                return (struct dt_it *)it;
        }
        return ERR_PTR(-ENOMEM);
}

/**
 * free given Iterator.
 */

static void osd_it_iam_fini(const struct lu_env *env, struct dt_it *di)
{
        struct osd_it_iam *it = (struct osd_it_iam *)di;
        struct osd_object *obj = it->oi_obj;

        iam_it_fini(&it->oi_it);
        osd_ipd_put(env, &obj->oo_dir->od_container, it->oi_ipd);
        lu_object_put(env, &obj->oo_dt.do_lu);
}

/**
 *  Move Iterator to record specified by \a key
 *
 *  \param  di      osd iterator
 *  \param  key     key for index
 *
 *  \retval +ve  di points to record with least key not larger than key
 *  \retval  0   di points to exact matched key
 *  \retval -ve  failure
 */

static int osd_it_iam_get(const struct lu_env *env,
                          struct dt_it *di, const struct dt_key *key)
{
        struct osd_it_iam *it = (struct osd_it_iam *)di;

        return iam_it_get(&it->oi_it, (const struct iam_key *)key);
}

/**
 *  Release Iterator
 *
 *  \param  di      osd iterator
 */

static void osd_it_iam_put(const struct lu_env *env, struct dt_it *di)
{
        struct osd_it_iam *it = (struct osd_it_iam *)di;

        iam_it_put(&it->oi_it);
}

/**
 *  Move iterator by one record
 *
 *  \param  di      osd iterator
 *
 *  \retval +1   end of container reached
 *  \retval  0   success
 *  \retval -ve  failure
 */

static int osd_it_iam_next(const struct lu_env *env, struct dt_it *di)
{
        struct osd_it_iam *it = (struct osd_it_iam *)di;

        return iam_it_next(&it->oi_it);
}

/**
 * Return pointer to the key under iterator.
 */

static struct dt_key *osd_it_iam_key(const struct lu_env *env,
                                 const struct dt_it *di)
{
        struct osd_it_iam *it = (struct osd_it_iam *)di;

        return (struct dt_key *)iam_it_key_get(&it->oi_it);
}

/**
 * Return size of key under iterator (in bytes)
 */

static int osd_it_iam_key_size(const struct lu_env *env, const struct dt_it *di)
{
        struct osd_it_iam *it = (struct osd_it_iam *)di;

        return iam_it_key_size(&it->oi_it);
}

static inline void osd_it_append_attrs(struct lu_dirent *ent, __u32 attr,
                                       int len, __u16 type)
{
        struct luda_type *lt;
        const unsigned    align = sizeof(struct luda_type) - 1;

        /* check if file type is required */
        if (attr & LUDA_TYPE) {
                        len = (len + align) & ~align;

                        lt = (void *) ent->lde_name + len;
                        lt->lt_type = cpu_to_le16(CFS_DTTOIF(type));
                        ent->lde_attrs |= LUDA_TYPE;
        }

        ent->lde_attrs = cpu_to_le32(ent->lde_attrs);
}

/**
 * build lu direct from backend fs dirent.
 */

static inline void osd_it_pack_dirent(struct lu_dirent *ent,
                                      struct lu_fid *fid, __u64 offset,
                                      char *name, __u16 namelen,
                                      __u16 type, __u32 attr)
{
        fid_cpu_to_le(&ent->lde_fid, fid);
        ent->lde_attrs = LUDA_FID;

        ent->lde_hash = cpu_to_le64(offset);
        ent->lde_reclen = cpu_to_le16(lu_dirent_calc_size(namelen, attr));

        strncpy(ent->lde_name, name, namelen);
        ent->lde_namelen = cpu_to_le16(namelen);

        /* append lustre attributes */
        osd_it_append_attrs(ent, attr, namelen, type);
}

/**
 * Return pointer to the record under iterator.
 */
static int osd_it_iam_rec(const struct lu_env *env,
                          const struct dt_it *di,
                          struct dt_rec *dtrec, __u32 attr)
{
        struct osd_it_iam *it        = (struct osd_it_iam *)di;
        struct osd_thread_info *info = osd_oti_get(env);
        struct lu_fid     *fid       = &info->oti_fid;
        const struct osd_fid_pack *rec;
        struct lu_dirent *lde = (struct lu_dirent *)dtrec;
        char *name;
        int namelen;
        __u64 hash;
        int rc;

        name = (char *)iam_it_key_get(&it->oi_it);
        if (IS_ERR(name))
                RETURN(PTR_ERR(name));

        namelen = iam_it_key_size(&it->oi_it);

        rec = (const struct osd_fid_pack *) iam_it_rec_get(&it->oi_it);
        if (IS_ERR(rec))
                RETURN(PTR_ERR(rec));

        rc = osd_fid_unpack(fid, rec);
        if (rc)
                RETURN(rc);

        hash = iam_it_store(&it->oi_it);

        /* IAM does not store object type in IAM index (dir) */
        osd_it_pack_dirent(lde, fid, hash, name, namelen,
                           0, LUDA_FID);

        return 0;
}

/**
 * Returns cookie for current Iterator position.
 */
static __u64 osd_it_iam_store(const struct lu_env *env, const struct dt_it *di)
{
        struct osd_it_iam *it = (struct osd_it_iam *)di;

        return iam_it_store(&it->oi_it);
}

/**
 * Restore iterator from cookie.
 *
 * \param  di      osd iterator
 * \param  hash    Iterator location cookie
 *
 * \retval +ve  di points to record with least key not larger than key.
 * \retval  0   di points to exact matched key
 * \retval -ve  failure
 */

static int osd_it_iam_load(const struct lu_env *env,
                           const struct dt_it *di, __u64 hash)
{
        struct osd_it_iam *it = (struct osd_it_iam *)di;

        return iam_it_load(&it->oi_it, hash);
}

static const struct dt_index_operations osd_index_iam_ops = {
        .dio_lookup         = osd_index_iam_lookup,
        .dio_declare_insert = osd_index_declare_iam_insert,
        .dio_insert         = osd_index_iam_insert,
        .dio_declare_delete = osd_index_declare_iam_delete,
        .dio_delete         = osd_index_iam_delete,
        .dio_it     = {
                .init     = osd_it_iam_init,
                .fini     = osd_it_iam_fini,
                .get      = osd_it_iam_get,
                .put      = osd_it_iam_put,
                .next     = osd_it_iam_next,
                .key      = osd_it_iam_key,
                .key_size = osd_it_iam_key_size,
                .rec      = osd_it_iam_rec,
                .store    = osd_it_iam_store,
                .load     = osd_it_iam_load
        }
};

/**
 * Creates or initializes iterator context.
 *
 * \retval struct osd_it_ea, iterator structure on success
 *
 */
static struct dt_it *osd_it_ea_init(const struct lu_env *env,
                                    struct dt_object *dt,
                                    __u32 attr,
                                    struct lustre_capa *capa)
{
        struct osd_object       *obj  = osd_dt_obj(dt);
        struct osd_thread_info  *info = osd_oti_get(env);
        struct osd_it_ea        *it   = &info->oti_it_ea;
        struct lu_object        *lo   = &dt->do_lu;
        struct dentry           *obj_dentry = &info->oti_it_dentry;
        ENTRY;
        LASSERT(lu_object_exists(lo));

        obj_dentry->d_inode = obj->oo_inode;
        obj_dentry->d_sb = osd_sb(osd_obj2dev(obj));
        obj_dentry->d_name.hash = 0;

        it->oie_rd_dirent       = 0;
        it->oie_it_dirent       = 0;
        it->oie_dirent          = NULL;
        it->oie_buf             = info->oti_it_ea_buf;
        it->oie_obj             = obj;
        it->oie_file.f_pos      = 0;
        it->oie_file.f_dentry   = obj_dentry;
        if (attr & LUDA_64BITHASH)
                it->oie_file.f_flags = O_64BITHASH;
        else
                it->oie_file.f_flags = O_32BITHASH;
        it->oie_file.f_mapping    = obj->oo_inode->i_mapping;
        it->oie_file.f_op         = obj->oo_inode->i_fop;
        it->oie_file.private_data = NULL;
        lu_object_get(lo);
        RETURN((struct dt_it *) it);
}

/**
 * Destroy or finishes iterator context.
 *
 * \param di iterator structure to be destroyed
 */
static void osd_it_ea_fini(const struct lu_env *env, struct dt_it *di)
{
        struct osd_it_ea     *it   = (struct osd_it_ea *)di;
        struct osd_object    *obj  = it->oie_obj;
        struct inode       *inode  = obj->oo_inode;

        ENTRY;
        it->oie_file.f_op->release(inode, &it->oie_file);
        lu_object_put(env, &obj->oo_dt.do_lu);
        EXIT;
}

/**
 * It position the iterator at given key, so that next lookup continues from
 * that key Or it is similar to dio_it->load() but based on a key,
 * rather than file position.
 *
 * As a special convention, osd_it_ea_get(env, di, "") has to rewind iterator
 * to the beginning.
 *
 * TODO: Presently return +1 considering it is only used by mdd_dir_is_empty().
 */
static int osd_it_ea_get(const struct lu_env *env,
                         struct dt_it *di, const struct dt_key *key)
{
        struct osd_it_ea     *it   = (struct osd_it_ea *)di;

        ENTRY;
        LASSERT(((const char *)key)[0] == '\0');
        it->oie_file.f_pos      = 0;
        it->oie_rd_dirent       = 0;
        it->oie_it_dirent       = 0;
        it->oie_dirent          = NULL;

        RETURN(+1);
}

/**
 * Does nothing
 */
static void osd_it_ea_put(const struct lu_env *env, struct dt_it *di)
{
}

/**
 * It is called internally by ->readdir(). It fills the
 * iterator's in-memory data structure with required
 * information i.e. name, namelen, rec_size etc.
 *
 * \param buf in which information to be filled in.
 * \param name name of the file in given dir
 *
 * \retval 0 on success
 * \retval 1 on buffer full
 */
static int osd_ldiskfs_filldir(char *buf, const char *name, int namelen,
                               loff_t offset, __u64 ino,
                               unsigned d_type)
{
        struct osd_it_ea        *it   = (struct osd_it_ea *)buf;
        struct osd_it_ea_dirent *ent  = it->oie_dirent;
        struct lu_fid           *fid  = &ent->oied_fid;
        struct osd_fid_pack     *rec;
        ENTRY;

        /* this should never happen */
        if (unlikely(namelen == 0 || namelen > LDISKFS_NAME_LEN)) {
                CERROR("ldiskfs return invalid namelen %d\n", namelen);
                RETURN(-EIO);
        }

        if ((void *) ent - it->oie_buf + sizeof(*ent) + namelen >
            OSD_IT_EA_BUFSIZE)
                RETURN(1);

        if (d_type & LDISKFS_DIRENT_LUFID) {
                rec = (struct osd_fid_pack*) (name + namelen + 1);

                if (osd_fid_unpack(fid, rec) != 0)
                        fid_zero(fid);

                d_type &= ~LDISKFS_DIRENT_LUFID;
        } else {
                fid_zero(fid);
        }

        ent->oied_ino     = ino;
        ent->oied_off     = offset;
        ent->oied_namelen = namelen;
        ent->oied_type    = d_type;

        memcpy(ent->oied_name, name, namelen);

        it->oie_rd_dirent++;
        it->oie_dirent = (void *) ent + cfs_size_round(sizeof(*ent) + namelen);
        RETURN(0);
}

/**
 * Calls ->readdir() to load a directory entry at a time
 * and stored it in iterator's in-memory data structure.
 *
 * \param di iterator's in memory structure
 *
 * \retval   0 on success
 * \retval -ve on error
 */
static int osd_ldiskfs_it_fill(const struct lu_env *env,
                               const struct dt_it *di)
{
        struct osd_it_ea   *it    = (struct osd_it_ea *)di;
        struct osd_object  *obj   = it->oie_obj;
        struct inode       *inode = obj->oo_inode;
        struct htree_lock  *hlock = NULL;
        int                 result = 0;

        ENTRY;
        it->oie_dirent = it->oie_buf;
        it->oie_rd_dirent = 0;

        if (obj->oo_hl_head != NULL) {
                hlock = osd_oti_get(env)->oti_hlock;
                ldiskfs_htree_lock(hlock, obj->oo_hl_head,
                                   inode, LDISKFS_HLOCK_READDIR);
        } else {
                cfs_down_read(&obj->oo_ext_idx_sem);
        }

        LASSERT(inode->i_fop->readdir);
        result = inode->i_fop->readdir(&it->oie_file, it,
                                       (filldir_t) osd_ldiskfs_filldir);

        if (hlock != NULL)
                ldiskfs_htree_unlock(hlock);
        else
                cfs_up_read(&obj->oo_ext_idx_sem);

        if (it->oie_rd_dirent == 0) {
                result = -EIO;
        } else {
                it->oie_dirent = it->oie_buf;
                it->oie_it_dirent = 1;
        }

        RETURN(result);
}

/**
 * It calls osd_ldiskfs_it_fill() which will use ->readdir()
 * to load a directory entry at a time and stored it in
 * iterator's in-memory data structure.
 *
 * \param di iterator's in memory structure
 *
 * \retval +ve iterator reached to end
 * \retval   0 iterator not reached to end
 * \retval -ve on error
 */
static int osd_it_ea_next(const struct lu_env *env, struct dt_it *di)
{
        struct osd_it_ea *it = (struct osd_it_ea *)di;
        int rc;

        ENTRY;

        if (it->oie_it_dirent < it->oie_rd_dirent) {
                it->oie_dirent =
                        (void *) it->oie_dirent +
                        cfs_size_round(sizeof(struct osd_it_ea_dirent) +
                                       it->oie_dirent->oied_namelen);
                it->oie_it_dirent++;
                RETURN(0);
        } else {
                if (it->oie_file.f_pos == LDISKFS_HTREE_EOF)
                        rc = +1;
                else
                        rc = osd_ldiskfs_it_fill(env, di);
        }

        RETURN(rc);
}

/**
 * Returns the key at current position from iterator's in memory structure.
 *
 * \param di iterator's in memory structure
 *
 * \retval key i.e. struct dt_key on success
 */
static struct dt_key *osd_it_ea_key(const struct lu_env *env,
                                    const struct dt_it *di)
{
        struct osd_it_ea *it = (struct osd_it_ea *)di;

        return (struct dt_key *)it->oie_dirent->oied_name;
}

/**
 * Returns the key's size at current position from iterator's in memory structure.
 *
 * \param di iterator's in memory structure
 *
 * \retval key_size i.e. struct dt_key on success
 */
static int osd_it_ea_key_size(const struct lu_env *env, const struct dt_it *di)
{
        struct osd_it_ea *it = (struct osd_it_ea *)di;

        return it->oie_dirent->oied_namelen;
}


/**
 * Returns the value (i.e. fid/igif) at current position from iterator's
 * in memory structure.
 *
 * \param di struct osd_it_ea, iterator's in memory structure
 * \param attr attr requested for dirent.
 * \param lde lustre dirent
 *
 * \retval   0 no error and \param lde has correct lustre dirent.
 * \retval -ve on error
 */
static inline int osd_it_ea_rec(const struct lu_env *env,
                                const struct dt_it *di,
                                struct dt_rec *dtrec, __u32 attr)
{
        struct osd_it_ea        *it     = (struct osd_it_ea *)di;
        struct osd_object       *obj    = it->oie_obj;
        struct lu_fid           *fid    = &it->oie_dirent->oied_fid;
        struct lu_dirent        *lde    = (struct lu_dirent *)dtrec;
        int    rc = 0;

        ENTRY;

        if (!fid_is_sane(fid))
                rc = osd_ea_fid_get(env, obj, it->oie_dirent->oied_ino, fid);

        if (rc == 0)
                osd_it_pack_dirent(lde, fid, it->oie_dirent->oied_off,
                                   it->oie_dirent->oied_name,
                                   it->oie_dirent->oied_namelen,
                                   it->oie_dirent->oied_type,
                                   attr);
        RETURN(rc);
}

/**
 * Returns a cookie for current position of the iterator head, so that
 * user can use this cookie to load/start the iterator next time.
 *
 * \param di iterator's in memory structure
 *
 * \retval cookie for current position, on success
 */
static __u64 osd_it_ea_store(const struct lu_env *env, const struct dt_it *di)
{
        struct osd_it_ea *it = (struct osd_it_ea *)di;

        return it->oie_dirent->oied_off;
}

/**
 * It calls osd_ldiskfs_it_fill() which will use ->readdir()
 * to load a directory entry at a time and stored it i inn,
 * in iterator's in-memory data structure.
 *
 * \param di struct osd_it_ea, iterator's in memory structure
 *
 * \retval +ve on success
 * \retval -ve on error
 */
static int osd_it_ea_load(const struct lu_env *env,
                          const struct dt_it *di, __u64 hash)
{
        struct osd_it_ea *it = (struct osd_it_ea *)di;
        int rc;

        ENTRY;
        it->oie_file.f_pos = hash;

        rc =  osd_ldiskfs_it_fill(env, di);
        if (rc == 0)
                rc = +1;

        RETURN(rc);
}

/**
 * Index lookup function for interoperability mode (b11826).
 *
 * \param key,  key i.e. file name to be searched
 *
 * \retval +ve, on success
 * \retval -ve, on error
 */
static int osd_index_ea_lookup(const struct lu_env *env, struct dt_object *dt,
                               struct dt_rec *rec, const struct dt_key *key,
                               struct lustre_capa *capa)
{
        struct osd_object *obj = osd_dt_obj(dt);
        int rc = 0;

        ENTRY;

        LASSERT(S_ISDIR(obj->oo_inode->i_mode));
        LINVRNT(osd_invariant(obj));

        if (osd_object_auth(env, dt, capa, CAPA_OPC_INDEX_LOOKUP))
                return -EACCES;

        rc = osd_ea_lookup_rec(env, obj, rec, key);

        if (rc == 0)
                rc = +1;
        RETURN(rc);
}

/**
 * Index and Iterator operations for interoperability
 * mode (i.e. to run 2.0 mds on 1.8 disk) (b11826)
 */
static const struct dt_index_operations osd_index_ea_ops = {
        .dio_lookup         = osd_index_ea_lookup,
        .dio_declare_insert = osd_index_declare_ea_insert,
        .dio_insert         = osd_index_ea_insert,
        .dio_declare_delete = osd_index_declare_ea_delete,
        .dio_delete         = osd_index_ea_delete,
        .dio_it     = {
                .init     = osd_it_ea_init,
                .fini     = osd_it_ea_fini,
                .get      = osd_it_ea_get,
                .put      = osd_it_ea_put,
                .next     = osd_it_ea_next,
                .key      = osd_it_ea_key,
                .key_size = osd_it_ea_key_size,
                .rec      = osd_it_ea_rec,
                .store    = osd_it_ea_store,
                .load     = osd_it_ea_load
        }
};

static void *osd_key_init(const struct lu_context *ctx,
                          struct lu_context_key *key)
{
        struct osd_thread_info *info;

        OBD_ALLOC_PTR(info);
        if (info == NULL)
                return ERR_PTR(-ENOMEM);

        OBD_ALLOC(info->oti_it_ea_buf, OSD_IT_EA_BUFSIZE);
        if (info->oti_it_ea_buf == NULL)
                goto out_free_info;

        info->oti_env = container_of(ctx, struct lu_env, le_ctx);

        info->oti_hlock = ldiskfs_htree_lock_alloc();
        if (info->oti_hlock == NULL)
                goto out_free_ea;

        return info;

 out_free_ea:
        OBD_FREE(info->oti_it_ea_buf, OSD_IT_EA_BUFSIZE);
 out_free_info:
        OBD_FREE_PTR(info);
        return ERR_PTR(-ENOMEM);
}

static void osd_key_fini(const struct lu_context *ctx,
                         struct lu_context_key *key, void* data)
{
        struct osd_thread_info *info = data;

        if (info->oti_hlock != NULL)
                ldiskfs_htree_lock_free(info->oti_hlock);
        OBD_FREE(info->oti_it_ea_buf, OSD_IT_EA_BUFSIZE);
        OBD_FREE_PTR(info);
}

static void osd_key_exit(const struct lu_context *ctx,
                         struct lu_context_key *key, void *data)
{
        struct osd_thread_info *info = data;

        LASSERT(info->oti_r_locks == 0);
        LASSERT(info->oti_w_locks == 0);
        LASSERT(info->oti_txns    == 0);
}

/* type constructor/destructor: osd_type_init, osd_type_fini */
LU_TYPE_INIT_FINI(osd, &osd_key);

struct lu_context_key osd_key = {
        .lct_tags = LCT_DT_THREAD | LCT_MD_THREAD | LCT_MG_THREAD | LCT_LOCAL,
        .lct_init = osd_key_init,
        .lct_fini = osd_key_fini,
        .lct_exit = osd_key_exit
};

static int osd_shutdown(const struct lu_env *env, struct osd_device *o)
{
        struct osd_thread_info *info = osd_oti_get(env);

        ENTRY;

        if (o->od_oi_table != NULL)
                osd_oi_fini(info, o);

        if (o->od_fsops) {
                fsfilt_put_ops(o->od_fsops);
                o->od_fsops = NULL;
        }

        RETURN(0);
}

static int osd_mount(const struct lu_env *env,
                     struct osd_device *o, struct lustre_cfg *cfg)
{
        const char              *dev = lustre_cfg_string(cfg, 1);
        const char              *opts;
        unsigned long            page, s_flags, ldd_flags = 0;
        struct page             *__page;
        struct file_system_type *type;
        char                    *options = NULL;
        int                      rc = 0;
        ENTRY;

        o->od_fsops = fsfilt_get_ops(mt_str(LDD_MT_LDISKFS));
        if (o->od_fsops == NULL) {
                CERROR("Can't find fsfilt_ldiskfs\n");
                RETURN(-ENOTSUPP);
        }

        if (strlen(dev) >= sizeof(o->od_mntdev))
                RETURN(-E2BIG);

        if (o->od_mnt != NULL)
                RETURN(0);

        strcpy(o->od_mntdev, dev);

        OBD_PAGE_ALLOC(__page, CFS_ALLOC_STD);
        if (__page == NULL)
                GOTO(out, rc = -ENOMEM);

        s_flags = simple_strtoul(lustre_cfg_string(cfg, 2), NULL, 0);
        opts = lustre_cfg_string(cfg, 3);
#if 0
        /* XXX: how to pass flags, probably better to parse mount options? */
        ldd_flags = (unsigned long) lustre_cfg_buf(cfg, 4);
#endif

        page = (unsigned long)cfs_page_address(__page);
        options = (char *)page;
        memset(options, 0, CFS_PAGE_SIZE);
        if (opts == NULL)
                strcat(options, "user_xattr,acl");
        else
                strcat(options, opts);

        /* Glom up mount options */
        if (*options != 0)
                strcat(options, ",");
        strcat(options, "no_mbcache");

        type = get_fs_type("ldiskfs");
        if (!type) {
                CERROR("%s: premount failed: cannot find ldiskfs module\n",
                       dev);
                GOTO(out, rc = -ENODEV);
        }

        o->od_mnt = vfs_kern_mount(type, s_flags, dev, (void *)options);
        cfs_module_put(type->owner);

        if (IS_ERR(o->od_mnt)) {
                rc = PTR_ERR(o->od_mnt);
                CERROR("can't mount %s: %d\n", dev, rc);
                o->od_mnt = NULL;
                GOTO(out, rc);
        }

        if (lvfs_check_rdonly(o->od_mnt->mnt_sb->s_bdev)) {
                CERROR("Underlying device %s is marked as read-only. "
                       "Setup failed\n", dev);
                mntput(o->od_mnt);
                o->od_mnt = NULL;
                GOTO(out, rc = -EROFS);
        }

        if (!LDISKFS_HAS_COMPAT_FEATURE(o->od_mnt->mnt_sb,
                                        LDISKFS_FEATURE_COMPAT_HAS_JOURNAL)) {
                CERROR("%s: underlying device is mounted without journal\n",
                       dev);
                mntput(o->od_mnt);
                o->od_mnt = NULL;
                GOTO(out, rc = -EINVAL);
        }

        if (ldd_flags & LDD_F_IAM_DIR) {
                o->od_iop_mode = 0;
                LCONSOLE_WARN("%s: OSD: IAM mode enabled\n", dev);
        } else
                o->od_iop_mode = 1;

out:
        OBD_PAGE_FREE(__page);

        RETURN(rc);
}

static struct lu_device *osd_device_fini(const struct lu_env *env,
                                         struct lu_device *d)
{
        struct osd_device *osd = osd_dev(d);
        int rc;
        ENTRY;

        osd_compat_fini(osd);

        if (osd->od_mnt) {
                shrink_dcache_sb(osd_sb(osd));
                osd_sync(env, lu2dt_dev(d));
        }

        rc = osd_procfs_fini(osd);
        if (rc) {
                CERROR("proc fini error %d \n", rc);
                RETURN (ERR_PTR(rc));
        }

        if (osd->od_mnt) {
                mntput(osd->od_mnt);
                osd->od_mnt = NULL;
        }

        RETURN(NULL);
}

static int osd_device_init0(const struct lu_env *env,
                            struct osd_device *o,
                            struct lustre_cfg *cfg)
{
        struct lu_device       *l = osd2lu_dev(o);
        struct osd_thread_info *info;
        int                     rc;

        /* if the module was re-loaded, env can loose its keys */
        rc = lu_env_refill((struct lu_env *) env);
        if (rc)
                GOTO(out, rc);
        info = osd_oti_get(env);
        LASSERT(info);

        l->ld_ops = &osd_lu_ops;
        o->od_dt_dev.dd_ops = &osd_dt_ops;

        o->od_capa_hash = init_capa_hash();
        if (o->od_capa_hash == NULL)
                GOTO(out, rc = -ENOMEM);

        o->od_iop_mode = 1;
        o->od_read_cache = 1;
        o->od_writethrough_cache = 1;
        o->od_readcache_max_filesize = OSD_MAX_CACHE_SIZE;

        rc = osd_mount(env, o, cfg);
        if (rc)
                GOTO(out_capa, rc);

        /* 1. initialize oi before any file create or file open */
        rc = osd_oi_init(info, o);
        if (rc < 0)
                GOTO(out_mnt, rc);

        rc = osd_compat_init(o);
        if (rc != 0)
                GOTO(out_oi, rc);

        rc = lu_site_init(&o->od_site, l);
        if (rc)
                GOTO(out_compat, rc);
        o->od_site.ls_bottom_dev = l;

        strncpy(o->od_svname, lustre_cfg_string(cfg, 4),
                        sizeof(o->od_svname) - 1);

        rc = osd_procfs_init(o, o->od_svname);
        if (rc != 0) {
                CERROR("can't initialize procfs entry for %s\n", o->od_svname);
                GOTO(out_site, rc);
        }

        RETURN(0);
out_site:
        lu_site_fini(&o->od_site);
out_compat:
        osd_compat_fini(o);
out_oi:
        osd_oi_fini(info, o);
out_mnt:
        osd_shutdown(env, o);
        mntput(o->od_mnt);
        o->od_mnt = NULL;
out_capa:
        cleanup_capa_hash(o->od_capa_hash);
out:
        RETURN(rc);
}

static struct lu_device *osd_device_alloc(const struct lu_env *env,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *cfg)
{
        struct osd_device *o;
        int                rc;

        OBD_ALLOC_PTR(o);
        if (o == NULL)
                return ERR_PTR(-ENOMEM);

        rc = dt_device_init(&o->od_dt_dev, t);
        if (rc == 0) {
                rc = osd_device_init0(env, o, cfg);
                if (rc)
                        dt_device_fini(&o->od_dt_dev);
        }

        if (unlikely(rc != 0))
                OBD_FREE_PTR(o);

        return rc == 0 ? osd2lu_dev(o) : ERR_PTR(rc);
}

static struct lu_device *osd_device_free(const struct lu_env *env,
                                         struct lu_device *d)
{
        struct osd_device *o = osd_dev(d);
        ENTRY;

        cleanup_capa_hash(o->od_capa_hash);
        /* XXX: make osd top device in order to release reference */
        d->ld_site->ls_top_dev = d;
        lu_site_purge(env, d->ld_site, -1);
        lu_site_fini(&o->od_site);
        dt_device_fini(&o->od_dt_dev);
        OBD_FREE_PTR(o);
        RETURN(NULL);
}

static int osd_process_config(const struct lu_env *env,
                              struct lu_device *d, struct lustre_cfg *cfg)
{
        struct osd_device *o = osd_dev(d);
        int err;
        ENTRY;

        switch(cfg->lcfg_command) {
        case LCFG_SETUP:
                err = osd_mount(env, o, cfg);
                break;
        case LCFG_CLEANUP:
                err = osd_shutdown(env, o);
                break;
        default:
                err = -ENOSYS;
        }

        RETURN(err);
}

static int osd_recovery_complete(const struct lu_env *env,
                                 struct lu_device *d)
{
        RETURN(0);
}

/*
 * we use exports to track all osd users
 */
static int osd_obd_connect(const struct lu_env *env, struct obd_export **exp,
                           struct obd_device *obd, struct obd_uuid *cluuid,
                           struct obd_connect_data *data, void *localdata)
{
        struct osd_device    *osd = osd_dev(obd->obd_lu_dev);
        struct lustre_handle  conn;
        int                   rc;
        ENTRY;

        CDEBUG(D_CONFIG, "connect #%d\n", osd->od_connects);

        rc = class_connect(&conn, obd, cluuid);
        if (rc)
                RETURN(rc);

        *exp = class_conn2export(&conn);

        /* XXX: locking ? */
        osd->od_connects++;

        RETURN(0);
}

/*
 * once last export (we don't count self-export) disappeared
 * osd can be released
 */
static int osd_obd_disconnect(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        struct osd_device *osd = osd_dev(obd->obd_lu_dev);
        int                rc, release = 0;
        ENTRY;

        /* Only disconnect the underlying layers on the final disconnect. */
        /* XXX: locking ? */
        osd->od_connects--;
        if (osd->od_connects == 0)
                release = 1;

        rc = class_disconnect(exp); /* bz 9811 */

        if (rc == 0 && release)
                class_manual_cleanup(obd);
        RETURN(rc);
}

static int osd_start(const struct lu_env *env, struct lu_device *dev)
{
        return 0;
}

static const struct lu_object_operations osd_lu_obj_ops = {
        .loo_object_init      = osd_object_init,
        .loo_object_delete    = osd_object_delete,
        .loo_object_release   = osd_object_release,
        .loo_object_free      = osd_object_free,
        .loo_object_print     = osd_object_print,
        .loo_object_invariant = osd_object_invariant
};

const struct lu_device_operations osd_lu_ops = {
        .ldo_object_alloc      = osd_object_alloc,
        .ldo_process_config    = osd_process_config,
        .ldo_recovery_complete = osd_recovery_complete,
        .ldo_start             = osd_start,
};

static const struct lu_device_type_operations osd_device_type_ops = {
        .ldto_init = osd_type_init,
        .ldto_fini = osd_type_fini,

        .ldto_start = osd_type_start,
        .ldto_stop  = osd_type_stop,

        .ldto_device_alloc = osd_device_alloc,
        .ldto_device_free  = osd_device_free,

        .ldto_device_init    = NULL,
        .ldto_device_fini    = osd_device_fini
};

static struct lu_device_type osd_device_type = {
        .ldt_tags     = LU_DEVICE_DT,
        .ldt_name     = LUSTRE_OSD_LDISKFS_NAME,
        .ldt_ops      = &osd_device_type_ops,
        .ldt_ctx_tags = LCT_LOCAL
};

/*
 * lprocfs legacy support.
 */
static struct obd_ops osd_obd_device_ops = {
        .o_owner       = THIS_MODULE,
        .o_connect     = osd_obd_connect,
        .o_disconnect  = osd_obd_disconnect
};

static int __init osd_mod_init(void)
{
        struct lprocfs_static_vars lvars;

        osd_oi_mod_init();
        lprocfs_osd_init_vars(&lvars);
        return class_register_type(&osd_obd_device_ops, NULL, lvars.module_vars,
                                   LUSTRE_OSD_LDISKFS_NAME, &osd_device_type);
}

static void __exit osd_mod_exit(void)
{
        class_unregister_type(LUSTRE_OSD_LDISKFS_NAME);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Object Storage Device ("LUSTRE_OSD_LDISKFS_NAME")");
MODULE_LICENSE("GPL");

cfs_module(osd, "0.1.0", osd_mod_init, osd_mod_exit);
