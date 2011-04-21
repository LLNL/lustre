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
 * lustre/mdd/mdd_trans.c
 *
 * Lustre Metadata Server (mdd) routines
 *
 * Author: Wang Di <wangdi@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lprocfs_status.h>

#include <lustre_mds.h>
#include <lustre/lustre_idl.h>
#include <obd_cksum.h>

#include "mdd_internal.h"

/*
 * Here is a short description of programming model used in MDD
 * to do with transactions and related error handling.
 *
 * in OSD API there is strict separation between declaration phase
 * and execution phase of a transaction. given each MDD operation
 * easily takes 8+ changes and any can return an error, the code
 * implementing file creation or rename with using OSD API directly,
 * turned out to be pretty hairy (lots of similar lines and goto's).
 *
 * Instead we build specific infrastructure in MDD to, basically,
 * eliminate one phase from the code. In some sense this is similar
 * to database language: MDD just tells what changes it want, with
 * all the arguments, infrastructure calls corresponding declaration
 * methods and stores result in transaction handle. once all the
 * changes are told, the infrastructure analyzes stored error and
 * start executing (if no error was observed during declarations)
 * calling corresponding OSD API methods with arguments stored in
 * the transaction handle.
 *
 * for example, creation of hardlink looks like this:
 *  handle = mdd_tx_start();
 *  mdd_tx_idx_insert(parent, name, fid, handle);
 *  mdd_tx_ref_add(source, handle);
 *  mdd_tx_attr_set(parent, <new ctime/mtime>, handle);
 *  mdd_tx_attr_set(source, <new ctime>, handle);
 *  rc = mdd_tx_end(handle);
 *
 * notice there is no need to analyze error code between mdd_tx_start()
 * and mdd_tx_end() -- mdd_tx_*() takes care of that and in case of
 * error it'll be returned by mdd_tx_end(). first error will make all
 * subsequent mdd_tx_*() calls no-op.
 * 
 * another thing to be careful about is the memory re-used for arguments
 * of different calls to mdd_tx_*(): it's prohibited as arguments are 
 * supposed to be used during execution in mdd_tx_end() context. you
 * should have different storage for every argument. the only exception
 * is mdd_tx_attr_set() making copy of struct lu_attr into transaction.
 * so, the rule is that if argument is passed as a reference, then the
 * memory should not be re-used till return from mdd_tx_end().
 *
 * if execution fail, then we can survive if executed ops are undoable
 *
 */

struct mdd_tx_arg *mdd_tx_add_exec(struct mdd_thandle *tx,
                                   mdd_tx_exec_func_t func,
                                   mdd_tx_exec_func_t undo,
                                   char *file, int line)
{
        int i;

        LASSERT(tx);
        LASSERT(func);

        i = tx->mtx_argno;
        LASSERT(i < MDD_TX_MAX_OPS);

        tx->mtx_argno++;

        tx->mtx_args[i].exec_fn = func;
        tx->mtx_args[i].undo_fn = undo;
        tx->mtx_args[i].file    = file;
        tx->mtx_args[i].line    = line;

        return &tx->mtx_args[i];
}

int mdd_tx_idx_insert_exec(const struct lu_env *env, struct thandle *th,
                           struct mdd_tx_arg *arg)
{
        struct md_ucred   *uc = md_ucred(env);
        struct mdd_device *mdd;
        int                rc;

        LASSERT(arg->object);
        mdd = mdd_obj2mdd_dev(arg->object);
        LASSERT(arg->u.insert.rec);
        LASSERT(arg->u.insert.key);
        LASSERT(mdd_write_locked(env, arg->object) || arg->object == mdd->mdd_orphans);

        if (unlikely(dt_try_as_dir(env, mdd_object_child(arg->object)) == 0))
                return -ENOTDIR;

        CDEBUG(D_OTHER, "insert '*%s' in "DFID"\n", (char *) arg->u.insert.key,
               PFID(mdd_object_fid(arg->object)));
        rc = dt_insert(env, mdd_object_child(arg->object),
                       arg->u.insert.rec, arg->u.insert.key, th,
                       mdd_object_capa(env, arg->object),
                       uc->mu_cap & CFS_CAP_SYS_RESOURCE_MASK);

        return rc;
}

void __mdd_tx_idx_insert(const struct lu_env *env, struct mdd_object *pobj,
                         const struct lu_fid *lf, const char *name,
                         struct mdd_thandle *tx, char *file, int line)
{
        struct mdd_device *mdd = mdd_obj2mdd_dev(pobj);
        struct dt_object  *next = mdd_object_child(pobj);
        struct mdd_tx_arg *arg;
        int               rc = 0;

        LASSERT(mdd_write_locked(env, pobj) || pobj == mdd->mdd_orphans);

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        /*
         * if object doesn't exist than declaration of new object
         * should be enough for new ./.. entries as well
         */
        if (mdd_object_exists(pobj)) {
                if (unlikely(dt_try_as_dir(env, next) == 0)) {
                        tx->mtx_err = -ENOTDIR;
                        return;
                }
                rc = dt_declare_insert(env, next, (const struct dt_rec *) lf,
                                       (const struct dt_key *) name,
                                       tx->mtx_handle);
                tx->mtx_err = rc;
        }

        if (likely(rc == 0)) {
                arg = mdd_tx_add_exec(tx, mdd_tx_idx_insert_exec, NULL, file, line);
                LASSERT(arg);
                arg->object       = pobj;
                arg->u.insert.rec = (const struct dt_rec *) lf;
                arg->u.insert.key = (const struct dt_key *) name;
        }
}

int mdd_tx_idx_delete_exec(const struct lu_env *env, struct thandle *th,
                           struct mdd_tx_arg *arg)
{
        struct mdd_device *mdd;
        int                rc;

        LASSERT(arg->object);
        mdd = mdd_obj2mdd_dev(arg->object);
        LASSERT(arg->u.insert.key);
        LASSERT(mdd_write_locked(env, arg->object) || arg->object == mdd->mdd_orphans);

        if (unlikely(dt_try_as_dir(env, mdd_object_child(arg->object)) == 0))
                return -ENOTDIR;

        CDEBUG(D_OTHER, "delete '*%s' from "DFID"\n", (char *) arg->u.insert.key,
               PFID(mdd_object_fid(arg->object)));
        rc = dt_delete(env, mdd_object_child(arg->object),
                       arg->u.insert.key, th, mdd_object_capa(env, arg->object));

        return rc;
}

void __mdd_tx_idx_delete(const struct lu_env *env, struct mdd_object *pobj,
                         const char *name, struct mdd_thandle *tx,
                         char *file, int line)
{
        struct mdd_device *mdd = mdd_obj2mdd_dev(pobj);
        struct dt_object  *next = mdd_object_child(pobj);
        struct mdd_tx_arg *arg;
        int               rc;

        LASSERT(mdd_object_exists(pobj));
        LASSERT(mdd_write_locked(env, pobj) || pobj == mdd->mdd_orphans);

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        if (unlikely(dt_try_as_dir(env, next) == 0)) {
                tx->mtx_err = -ENOTDIR;
                return;
        }

        rc = dt_declare_delete(env, next, (const struct dt_key *) name,
                               tx->mtx_handle);
        tx->mtx_err = rc;

        if (likely(rc == 0)) {
                arg = mdd_tx_add_exec(tx, mdd_tx_idx_delete_exec, NULL, file, line);
                LASSERT(arg);
                arg->object = pobj;
                arg->u.insert.key    = (const struct dt_key *) name;
        }
}

int mdd_tx_ref_add_undo(const struct lu_env *env, struct thandle *th,
                        struct mdd_tx_arg *arg)
{

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object));

        CDEBUG(D_OTHER, "undo add ref on "DFID"\n", PFID(mdd_object_fid(arg->object)));
        dt_ref_del(env, mdd_object_child(arg->object), th);

        return 0;
}

int mdd_tx_ref_add_exec(const struct lu_env *env, struct thandle *th,
                        struct mdd_tx_arg *arg)
{

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object));

        CDEBUG(D_OTHER, "add ref on "DFID"\n", PFID(mdd_object_fid(arg->object)));
        dt_ref_add(env, mdd_object_child(arg->object), th);

        return 0;
}

void __mdd_tx_ref_add(const struct lu_env *env, struct mdd_object *obj,
                      struct mdd_thandle *tx, char *file, int line)
{
        struct mdd_tx_arg *arg;

        LASSERT(mdd_write_locked(env, obj));

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        dt_declare_ref_add(env, mdd_object_child(obj), tx->mtx_handle);

        arg = mdd_tx_add_exec(tx, mdd_tx_ref_add_exec,
                              mdd_tx_ref_add_undo, file, line);
        LASSERT(arg);
        arg->object = obj;
}

int mdd_tx_ref_del_undo(const struct lu_env *env, struct thandle *th,
                        struct mdd_tx_arg *arg)
{

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object));

        CDEBUG(D_OTHER, "undo del ref on "DFID"\n", PFID(mdd_object_fid(arg->object)));
        dt_ref_add(env, mdd_object_child(arg->object), th);

        return 0;
}

int mdd_tx_ref_del_exec(const struct lu_env *env, struct thandle *th,
                        struct mdd_tx_arg *arg)
{

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object));

        CDEBUG(D_OTHER, "del ref on "DFID"\n", PFID(mdd_object_fid(arg->object)));
        dt_ref_del(env, mdd_object_child(arg->object), th);

        return 0;
}


void __mdd_tx_ref_del(const struct lu_env *env, struct mdd_object *obj,
                      struct mdd_thandle *tx, char *file, int line)
{
        struct mdd_tx_arg *arg;

        LASSERT(mdd_write_locked(env, obj));

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        dt_declare_ref_del(env, mdd_object_child(obj), tx->mtx_handle);

        arg = mdd_tx_add_exec(tx, mdd_tx_ref_del_exec,
                              mdd_tx_ref_del_undo, file, line);
        LASSERT(arg);
        arg->object = obj;
}

int mdd_tx_attr_set_undo(const struct lu_env *env, struct thandle *th,
                        struct mdd_tx_arg *arg)
{
        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object) != 0);

        /*
         * XXX: we can ignore simple changes like [amc]time,
         * but others are easy to undo ?
         */
        if (arg->u.attr_set.attr.la_valid & ~(LA_CTIME | LA_MTIME | LA_ATIME))
                LBUG();

        return 0;
}

int mdd_tx_attr_set_exec(const struct lu_env *env, struct thandle *th,
                        struct mdd_tx_arg *arg)
{
        int              rc;

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object) != 0);

        rc = dt_attr_set(env, mdd_object_child(arg->object),
                         &arg->u.attr_set.attr, th,
                         mdd_object_capa(env, arg->object));
        CDEBUG(D_OTHER, "set attr on "DFID": %d\n",
               PFID(mdd_object_fid(arg->object)), rc);

        return rc;
}

void __mdd_tx_attr_set(const struct lu_env *env, struct mdd_object *obj,
                       const struct lu_attr *attr, struct mdd_thandle *tx,
                       char *file, int line)
{
        struct mdd_tx_arg *arg;
        int               rc;

        LASSERT(mdd_write_locked(env, obj) != 0);

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        rc = dt_declare_attr_set(env, mdd_object_child(obj), attr, tx->mtx_handle);
        tx->mtx_err = rc;

        if (likely(rc == 0)) {
                arg = mdd_tx_add_exec(tx, mdd_tx_attr_set_exec,
                                      mdd_tx_attr_set_undo, file, line);
                LASSERT(arg);
                arg->object = obj;
                arg->u.attr_set.attr = *attr;
        }
}

int mdd_tx_xattr_set_exec(const struct lu_env *env, struct thandle *th,
                          struct mdd_tx_arg *arg)
{
        const struct lu_buf *b;
        int                  rc;

        LASSERT(arg->object);
        LASSERT(arg->u.xattr_set.name);
        LASSERT(mdd_write_locked(env, arg->object) != 0);

        /* sanity check to make sure the args hasn't changed since declare */
        b = &arg->u.xattr_set.buf;
#ifdef HAVE_ADLER
        if (arg->u.xattr_set.csum)
                LASSERTF(arg->u.xattr_set.csum == adler32(1UL, b->lb_buf, b->lb_len),
                         "bytes changed since declaration at %s:%u\n",
                         arg->file, arg->line);
#endif

        rc = dt_xattr_set(env, mdd_object_child(arg->object),
                          b, arg->u.xattr_set.name, arg->u.xattr_set.flags, th,
                          mdd_object_capa(env, arg->object));

        /*
         * ignore errors if this is LINK EA
         */
        if (unlikely(rc && !strncmp(arg->u.xattr_set.name, XATTR_NAME_LINK,
                                    strlen(XATTR_NAME_LINK)))) {
                CERROR("ignore linkea error: %d\n", rc);
                rc = 0;
        }

        CDEBUG(D_OTHER, "set xattr %s from %p/%u on "DFID": %d\n",
               arg->u.xattr_set.name, b->lb_buf, (unsigned) b->lb_len,
               PFID(mdd_object_fid(arg->object)), rc);

        return rc;
}


void __mdd_tx_xattr_set(const struct lu_env *env, struct mdd_object *obj,
                        const struct lu_buf *buf, const char *name, int flags,
                        struct mdd_thandle *tx, char *file, int line)
{
        struct mdd_tx_arg *arg;
        int               rc;

        LASSERT(obj);
        LASSERT(buf);
        LASSERT(buf->lb_buf || buf->lb_len == 0);

        LASSERT(mdd_write_locked(env, obj) != 0);

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        rc = dt_declare_xattr_set(env, mdd_object_child(obj), buf,
                                  name, flags, tx->mtx_handle);
        tx->mtx_err = rc;

        if (likely(rc == 0)) {
                arg = mdd_tx_add_exec(tx, mdd_tx_xattr_set_exec, NULL, file, line);
                LASSERT(arg);
                arg->object = obj;
                arg->u.xattr_set.name = name;
                arg->u.xattr_set.flags = flags;
                arg->u.xattr_set.buf = *buf;
#ifdef HAVE_ADLER
                arg->u.xattr_set.csum = 0;
                if (buf->lb_buf)
                        arg->u.xattr_set.csum = adler32(1, buf->lb_buf, buf->lb_len);
#endif
        }
}

int mdd_tx_xattr_del_exec(const struct lu_env *env, struct thandle *th,
                          struct mdd_tx_arg *arg)
{
        int              rc;

        LASSERT(arg->object);
        LASSERT(arg->u.xattr_set.name);
        LASSERT(mdd_write_locked(env, arg->object) != 0);

        rc = dt_xattr_del(env, mdd_object_child(arg->object),
                             arg->u.xattr_set.name, th,
                             mdd_object_capa(env, arg->object));

        return rc;
}


void __mdd_tx_xattr_del(const struct lu_env *env, struct mdd_object *obj,
                        const char *name, struct mdd_thandle *tx,
                        char *file, int line)
{
        struct mdd_tx_arg *arg;
        int               rc;

        LASSERT(mdd_write_locked(env, obj) != 0);

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        rc = dt_declare_xattr_del(env, mdd_object_child(obj),
                                  name, tx->mtx_handle);
        tx->mtx_err = rc;

        if (likely(rc == 0)) {
                arg = mdd_tx_add_exec(tx, mdd_tx_xattr_del_exec, NULL, file, line);
                LASSERT(arg);
                arg->object = obj;
                arg->u.xattr_set.name = name;
        }
}

int mdd_tx_write_undo(const struct lu_env *env, struct thandle *th,
                      struct mdd_tx_arg *arg)
{
        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object) != 0);

        /*
         * we don't need to undo write to hardlink:
         * it's done once at object initialization time
         * so object will be destroyed as part of undo
         */
        LASSERT(S_ISLNK(mdd_object_type(arg->object)));

        return 0;
}

int mdd_tx_write_exec(const struct lu_env *env, struct thandle *th,
                      struct mdd_tx_arg *arg)
{
        struct dt_object *next;
        loff_t            pos;
        int               rc;

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object) != 0);
        pos = arg->u.write.pos;

        CDEBUG(D_OTHER, "write to "DFID"\n", PFID(mdd_object_fid(arg->object)));
        next = mdd_object_child(arg->object);
        rc = next->do_body_ops->dbo_write(env, next, &arg->u.write.buf,
                                          &pos, th, BYPASS_CAPA, 1);
        if (rc == arg->u.write.buf.lb_len)
                rc = 0;
        else if (rc > 0)
                rc = -EFAULT;

        return rc;
}


void __mdd_tx_write(const struct lu_env *env, struct mdd_object *obj,
                    const struct lu_buf *buf, loff_t pos, struct mdd_thandle *tx,
                    char *file, int line)
{
        struct dt_object  *next = mdd_object_child(obj);
        struct mdd_tx_arg *arg;
        int               rc;

        LASSERT(mdd_write_locked(env, obj) != 0);

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        rc = next->do_body_ops->dbo_declare_write(env, next, buf->lb_len,
                                                  pos, tx->mtx_handle);
        tx->mtx_err = rc;

        if (likely(rc == 0)) {
                arg = mdd_tx_add_exec(tx, mdd_tx_write_exec,
                                      mdd_tx_write_undo, file, line);
                LASSERT(arg);
                arg->object = obj;
                arg->u.write.buf = *buf;
                arg->u.write.pos = pos;
        }
}

int mdd_tx_create_undo(const struct lu_env *env, struct thandle *th,
                       struct mdd_tx_arg *arg)
{
        int rc;

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object) != 0);

        rc = dt_destroy(env, mdd_object_child(arg->object), th);

        /* we don't like double failures */
        LASSERT(rc == 0);

        return rc;
}

int mdd_tx_create_exec(const struct lu_env *env, struct thandle *th,
                       struct mdd_tx_arg *arg)
{
        int rc;

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object) != 0);

        CDEBUG(D_OTHER, "create "DFID": dof %u, mode %o\n",
               PFID(mdd_object_fid(arg->object)),
               arg->u.create.dof.dof_type,
               arg->u.create.attr.la_mode & S_IFMT);

        rc = dt_create(env, mdd_object_child(arg->object),
                       (struct lu_attr *) &arg->u.create.attr,
                       &arg->u.create.hint, &arg->u.create.dof, th);

        return rc;
}

void __mdd_tx_create(const struct lu_env *env, struct mdd_object *pobj,
                     struct lu_attr *attr, struct dt_allocation_hint *hint,
                     struct dt_object_format *dof, struct mdd_thandle *tx,
                     char *file, int line)
{
        struct dt_object  *next = mdd_object_child(pobj);
        struct mdd_tx_arg *arg;
        int               rc;

        LASSERT(mdd_write_locked(env, pobj) != 0);

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        rc = dt_declare_create(env, next, attr, hint, dof, tx->mtx_handle);
        tx->mtx_err = rc;

        if (likely(rc == 0)) {
                arg = mdd_tx_add_exec(tx, mdd_tx_create_exec,
                                      mdd_tx_create_undo, file, line);
                LASSERT(arg);
                arg->object = pobj;
                arg->u.create.attr = *attr;
                arg->u.create.hint = *hint;
                arg->u.create.dof  = *dof;
        }
}

int mdd_tx_destroy_exec(const struct lu_env *env, struct thandle *th,
                        struct mdd_tx_arg *arg)
{
        int rc;

        LASSERT(arg->object);
        LASSERT(mdd_write_locked(env, arg->object) != 0);

        CDEBUG(D_OTHER, "destroy "DFID"\n", PFID(mdd_object_fid(arg->object)));

        rc = dt_destroy(env, mdd_object_child(arg->object), th);

        return rc;
}

void __mdd_tx_destroy(const struct lu_env *env, struct mdd_object *pobj,
                    struct mdd_thandle *tx, char *file, int line)
{
        struct dt_object  *next = mdd_object_child(pobj);
        struct mdd_tx_arg *arg;
        int               rc;

        /* don't proceed if any of previous declaration failed */
        if (tx->mtx_err)
                return;

        rc = dt_declare_destroy(env, next, tx->mtx_handle);
        tx->mtx_err = rc;

        if (likely(rc == 0)) {
                arg = mdd_tx_add_exec(tx, mdd_tx_destroy_exec, NULL, file, line);
                LASSERT(arg);
                arg->object = pobj;
        }
}

struct mdd_thandle *mdd_tx_start(const struct lu_env *env, struct mdd_device *mdd)
{
        struct mdd_thandle *th = &mdd_env_info(env)->mti_thandle;

        LASSERT(mdd);
        LASSERT(th->mtx_handle == NULL);
        LASSERT(th->mtx_argno == 0);
        LASSERT(th->mtx_err == 0);

        th->mtx_dev = mdd;

        th->mtx_handle = dt_trans_create(env, mdd->mdd_child);
        if (IS_ERR(th->mtx_handle)) {
                CERROR("can't create transaction: %d\n",
                       (int) PTR_ERR(th->mtx_handle));
                th = (void *) th->mtx_handle;
        }
        return th;
}

int mdd_tx_end(const struct lu_env *env, struct mdd_thandle *th)
{
        struct mdd_thread_info *info = mdd_env_info(env);
        struct mdd_thandle *_th = &info->mti_thandle;
        int i = 0, rc;

        LASSERT(th == _th);
        LASSERT(th->mtx_dev);
        LASSERT(th->mtx_handle);

        if ((rc = th->mtx_err)) {
                CDEBUG(D_OTHER, "error during declaration: %d\n", rc);
                GOTO(stop, rc);
        }

        rc = dt_trans_start(env, th->mtx_dev->mdd_child, th->mtx_handle);
        if (unlikely(rc))
                GOTO(stop, rc);

        CDEBUG(D_OTHER, "start execution\n");
        for (i = 0; i < th->mtx_argno; i++) {
                rc = th->mtx_args[i].exec_fn(env, th->mtx_handle, &th->mtx_args[i]);
                if (unlikely(rc)) {
                        CERROR("error during execution of #%u from %s:%d: %d\n",
                               i, th->mtx_args[i].file, th->mtx_args[i].line, rc);
                        while (--i >= 0) {
                                LASSERTF(th->mtx_args[i].undo_fn,
                                         "can't undo changes, hope for failover!\n");
                                th->mtx_args[i].undo_fn(env, th->mtx_handle,
                                                        &th->mtx_args[i]);
                        }
                        break;
                }
        }

stop:
        CDEBUG(D_OTHER, "executed %u/%u: %d\n", i, th->mtx_argno, rc);
        th->mtx_handle->th_result = rc;
        dt_trans_stop(env, th->mtx_dev->mdd_child, th->mtx_handle);
        th->mtx_handle = NULL;
        th->mtx_argno = 0;
        th->mtx_err = 0;
        if (info->mti_big_buf.lb_len > OBD_ALLOC_BIG)
                /* if we vmalloced a large buffer drop it */
                mdd_buf_put(&info->mti_big_buf);

        RETURN(rc);
}

void mdd_tx_set_error(struct mdd_thandle *th, int err)
{
        if (th->mtx_err == 0)
                th->mtx_err = err;
}

/*
 * TODO
 *  - think of undo logic in rename
 *  - orph_index_delete() to choose orphan name before mdd_tx_end()
 *  - test undo logic
 *  - minor todo
 *    - optimize attr_set copy?
 */

