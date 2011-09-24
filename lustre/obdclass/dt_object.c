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
 * Copyright (c) 2011 Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdclass/dt_object.c
 *
 * Dt Object.
 * Generic functions from dt_object.h
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#include <obd.h>
#include <dt_object.h>
#include <libcfs/list.h>
/* fid_be_to_cpu() */
#include <lustre_fid.h>

/* all initialized local oid storages on this node are linked on this */
static CFS_LIST_HEAD(los_list_head);
static CFS_DEFINE_MUTEX(los_list_mutex);

struct dt_find_hint {
        struct lu_fid        *dfh_fid;
        struct dt_device     *dfh_dt;
        struct dt_object     *dfh_o;
};

struct dt_thread_info {
        char                     dti_buf[DT_MAX_PATH];
        struct dt_find_hint      dti_dfh;
        struct lu_attr           dti_attr;
        struct lu_fid            dti_fid;
        struct dt_object_format  dti_dof;
        struct lustre_mdt_attrs  dti_lma;
        struct lu_buf            dti_lb;
        loff_t                   dti_off;
};

/* context key constructor/destructor: dt_global_key_init, dt_global_key_fini */
LU_KEY_INIT(dt_global, struct dt_thread_info);
LU_KEY_FINI(dt_global, struct dt_thread_info);

static struct lu_context_key dt_key = {
        .lct_tags = LCT_MD_THREAD | LCT_DT_THREAD | LCT_MG_THREAD | LCT_LOCAL,
        .lct_init = dt_global_key_init,
        .lct_fini = dt_global_key_fini
};

static inline struct dt_thread_info *dt_info(const struct lu_env *env)
{
        struct dt_thread_info *dti;

        dti = lu_context_key_get(&env->le_ctx, &dt_key);
        LASSERT(dti);
        return dti;
}

/* no lock is necessary to protect the list, because call-backs
 * are added during system startup. Please refer to "struct dt_device".
 */
void dt_txn_callback_add(struct dt_device *dev, struct dt_txn_callback *cb)
{
        cfs_list_add(&cb->dtc_linkage, &dev->dd_txn_callbacks);
}
EXPORT_SYMBOL(dt_txn_callback_add);

void dt_txn_callback_del(struct dt_device *dev, struct dt_txn_callback *cb)
{
        cfs_list_del_init(&cb->dtc_linkage);
}
EXPORT_SYMBOL(dt_txn_callback_del);

int dt_txn_hook_start(const struct lu_env *env,
                      struct dt_device *dev, struct thandle *th)
{
        int rc = 0;
        struct dt_txn_callback *cb;

        if (th->th_local)
                return 0;

        cfs_list_for_each_entry(cb, &dev->dd_txn_callbacks, dtc_linkage) {
                if (cb->dtc_txn_start == NULL ||
                    !(cb->dtc_tag & env->le_ctx.lc_tags))
                        continue;
                rc = cb->dtc_txn_start(env, th, cb->dtc_cookie);
                if (rc < 0)
                        break;
        }
        return rc;
}
EXPORT_SYMBOL(dt_txn_hook_start);

int dt_txn_hook_stop(const struct lu_env *env, struct thandle *txn)
{
        struct dt_device       *dev = txn->th_dev;
        struct dt_txn_callback *cb;
        int                     rc = 0;

        if (txn->th_local)
                return 0;

        cfs_list_for_each_entry(cb, &dev->dd_txn_callbacks, dtc_linkage) {
                if (cb->dtc_txn_stop == NULL ||
                    !(cb->dtc_tag & env->le_ctx.lc_tags))
                        continue;
                rc = cb->dtc_txn_stop(env, txn, cb->dtc_cookie);
                if (rc < 0)
                        break;
        }
        return rc;
}
EXPORT_SYMBOL(dt_txn_hook_stop);

void dt_txn_hook_commit(struct thandle *txn)
{
        struct dt_txn_callback *cb;

        if (txn->th_local)
                return;

        cfs_list_for_each_entry(cb, &txn->th_dev->dd_txn_callbacks,
                                dtc_linkage) {
                if (cb->dtc_txn_commit)
                        cb->dtc_txn_commit(txn, cb->dtc_cookie);
        }
}
EXPORT_SYMBOL(dt_txn_hook_commit);

int dt_device_init(struct dt_device *dev, struct lu_device_type *t)
{

        CFS_INIT_LIST_HEAD(&dev->dd_txn_callbacks);
        return lu_device_init(&dev->dd_lu_dev, t);
}
EXPORT_SYMBOL(dt_device_init);

void dt_device_fini(struct dt_device *dev)
{
        lu_device_fini(&dev->dd_lu_dev);
}
EXPORT_SYMBOL(dt_device_fini);

int dt_object_init(struct dt_object *obj,
                   struct lu_object_header *h, struct lu_device *d)

{
        return lu_object_init(&obj->do_lu, h, d);
}
EXPORT_SYMBOL(dt_object_init);

void dt_object_fini(struct dt_object *obj)
{
        lu_object_fini(&obj->do_lu);
}
EXPORT_SYMBOL(dt_object_fini);

int dt_try_as_dir(const struct lu_env *env, struct dt_object *obj)
{
        if (obj->do_index_ops == NULL)
                obj->do_ops->do_index_try(env, obj, &dt_directory_features);
        return obj->do_index_ops != NULL;
}
EXPORT_SYMBOL(dt_try_as_dir);

enum dt_format_type dt_mode_to_dft(__u32 mode)
{
        enum dt_format_type result;

        switch (mode & S_IFMT) {
        case S_IFDIR:
                result = DFT_DIR;
                break;
        case S_IFREG:
                result = DFT_REGULAR;
                break;
        case S_IFLNK:
                result = DFT_SYM;
                break;
        case S_IFCHR:
        case S_IFBLK:
        case S_IFIFO:
        case S_IFSOCK:
                result = DFT_NODE;
                break;
        default:
                LBUG();
                break;
        }
        return result;
}
EXPORT_SYMBOL(dt_mode_to_dft);

/**
 * lookup fid for object named \a name in directory \a dir.
 */

int dt_lookup_dir(const struct lu_env *env, struct dt_object *dir,
                  const char *name, struct lu_fid *fid)
{
        if (dt_try_as_dir(env, dir))
                return dt_lookup(env, dir, (struct dt_rec *)fid,
                                 (const struct dt_key *)name, BYPASS_CAPA);
        return -ENOTDIR;
}
EXPORT_SYMBOL(dt_lookup_dir);

/**
 * get object for given \a fid.
 */
struct dt_object *dt_locate(const struct lu_env *env,
                            struct dt_device *dev,
                            const struct lu_fid *fid)
{
        struct lu_object *obj;
        struct dt_object *dt;

        obj = lu_object_find(env, &dev->dd_lu_dev, fid, NULL);
        if (!IS_ERR(obj)) {
                obj = lu_object_locate(obj->lo_header, dev->dd_lu_dev.ld_type);
                LASSERT(obj != NULL);
                dt = container_of(obj, struct dt_object, do_lu);
        } else
                dt = (struct dt_object *)obj;
        return dt;
}
EXPORT_SYMBOL(dt_locate);

/* this differs from dt_locate by top_dev as parameter
 * but not one from lu_site */
struct dt_object *dt_locate_at(const struct lu_env *env,
                               struct dt_device *dev, const struct lu_fid *fid,
                               struct lu_device *top_dev)
{
        struct lu_object *lo, *n;
        ENTRY;

        lo = lu_object_find_at(env, top_dev, fid, NULL);
        if (IS_ERR(lo))
                return (void *) lo;

        LASSERT(lo != NULL);

        cfs_list_for_each_entry(n, &lo->lo_header->loh_layers, lo_linkage) {
                if (n->lo_dev == &dev->dd_lu_dev)
                        return container_of0(n, struct dt_object, do_lu);
        }
        return ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL(dt_locate_at);

/**
 * find a object named \a entry in given \a dfh->dfh_o directory.
 */
static int dt_find_entry(const struct lu_env *env, const char *entry, void *data)
{
        struct dt_find_hint  *dfh = data;
        struct dt_device     *dt = dfh->dfh_dt;
        struct lu_fid        *fid = dfh->dfh_fid;
        struct dt_object     *obj = dfh->dfh_o;
        int                   result;

        result = dt_lookup_dir(env, obj, entry, fid);
        lu_object_put(env, &obj->do_lu);
        if (result == 0) {
                obj = dt_locate(env, dt, fid);
                if (IS_ERR(obj))
                        result = PTR_ERR(obj);
        }
        dfh->dfh_o = obj;
        return result;
}

/**
 * Abstract function which parses path name. This function feeds
 * path component to \a entry_func.
 */
int dt_path_parser(const struct lu_env *env,
                   char *path, dt_entry_func_t entry_func,
                   void *data)
{
        char *e;
        int rc = 0;

        while (1) {
                e = strsep(&path, "/");
                if (e == NULL)
                        break;

                if (e[0] == 0) {
                        if (!path || path[0] == '\0')
                                break;
                        continue;
                }
                rc = entry_func(env, e, data);
                if (rc)
                        break;
        }

        return rc;
}

static struct dt_object *dt_store_resolve(const struct lu_env *env,
                                          struct dt_device *dt,
                                          const char *path,
                                          struct lu_fid *fid)
{
        struct dt_thread_info *info = dt_info(env);
        struct dt_find_hint   *dfh = &info->dti_dfh;
        struct dt_object      *obj;
        char                  *local = info->dti_buf;
        int                    result;

        dfh->dfh_dt = dt;
        dfh->dfh_fid = fid;

        strncpy(local, path, DT_MAX_PATH);
        local[DT_MAX_PATH - 1] = '\0';

        result = dt->dd_ops->dt_root_get(env, dt, fid);
        if (result == 0) {
                obj = dt_locate(env, dt, fid);
                if (!IS_ERR(obj)) {
                        dfh->dfh_o = obj;
                        result = dt_path_parser(env, local, dt_find_entry, dfh);
                        if (result != 0)
                                obj = ERR_PTR(result);
                        else
                                obj = dfh->dfh_o;
                }
        } else {
                obj = ERR_PTR(result);
        }
        return obj;
}

static struct dt_object *dt_reg_open(const struct lu_env *env,
                                     struct dt_device *dt,
                                     struct dt_object *p,
                                     const char *name,
                                     struct lu_fid *fid)
{
        struct dt_object *o;
        int result;

        result = dt_lookup_dir(env, p, name, fid);
        if (result == 0){
                o = dt_locate(env, dt, fid);
        }
        else
                o = ERR_PTR(result);

        return o;
}

/**
 * Open dt object named \a filename from \a dirname directory.
 *      \param  dt      dt device
 *      \param  fid     on success, object fid is stored in *fid
 */
struct dt_object *dt_store_open(const struct lu_env *env,
                                struct dt_device *dt,
                                const char *dirname,
                                const char *filename,
                                struct lu_fid *fid)
{
        struct dt_object *file;
        struct dt_object *dir;

        dir = dt_store_resolve(env, dt, dirname, fid);
        if (!IS_ERR(dir)) {
                file = dt_reg_open(env, dt, dir,
                                   filename, fid);
                lu_object_put(env, &dir->do_lu);
        } else {
                file = dir;
        }
        return file;
}
EXPORT_SYMBOL(dt_store_open);

struct dt_object *dt_find_or_create(const struct lu_env *env,
                                    struct dt_device *dt,
                                    const struct lu_fid *fid,
                                    struct dt_object_format *dof,
                                    struct lu_attr *at)
{
        struct dt_object *dto;
        struct thandle   *th;
        int rc;
        ENTRY;

        dto = dt_locate(env, dt, fid);
        if (unlikely(IS_ERR(dto)))
                RETURN(dto);

        LASSERT(dto != NULL);
        if (dt_object_exists(dto))
                RETURN(dto);

        LASSERT(dto != NULL);
        LASSERT(at != NULL);

        th = dt_trans_create(env, dt);
        if (IS_ERR(th))
                GOTO(out, rc = PTR_ERR(th));

        rc = dt_declare_create(env, dto, at, NULL, dof, th);
        LASSERT(rc == 0);

        rc = dt_trans_start_local(env, dt, th);
        if (rc)
                GOTO(trans_stop, rc);

        dt_write_lock(env, dto, 0);
        if (dt_object_exists(dto))
                GOTO(unlock, rc = 0);

        CDEBUG(D_OTHER, "create new object %lu:0x"LPX64"\n",
               (unsigned long) fid->f_oid, fid->f_seq);

        rc = dt_create(env, dto, at, NULL, dof, th);
        LASSERT(rc == 0);
        LASSERT(dt_object_exists(dto));

unlock:
        dt_write_unlock(env, dto);

trans_stop:
        dt_trans_stop(env, dt, th);
out:
        if (rc) {
                lu_object_put(env, &dto->do_lu);
                RETURN(ERR_PTR(rc));
        }
        RETURN(dto);
}
EXPORT_SYMBOL(dt_find_or_create);

/**
 * Set of functions to support local file operations
 * and fid generation for them
 */
int local_object_fid_generate(const struct lu_env *env,
                              struct local_oid_storage *los,
                              struct lu_fid *fid, int sync)
{
        struct dt_thread_info *dti = dt_info(env);
        struct los_ondisk      losd;
        struct thandle        *th;
        int rc;

        /* take next OID */
        cfs_spin_lock(&los->los_id_lock);
        fid->f_seq = los->los_seq;
        fid->f_oid = los->los_last_oid++;
        cfs_spin_unlock(&los->los_id_lock);
        fid->f_ver = 0;

        LASSERT(los->los_dev);
        LASSERT(los->los_obj);

        th = dt_trans_create(env, los->los_dev);
        if (IS_ERR(th))
                GOTO(out, rc = PTR_ERR(th));

        th->th_sync = sync; /* update table synchronously */
        rc = dt_declare_record_write(env, los->los_obj, sizeof(losd), 0, th);
        if (rc)
                GOTO(out_stop, rc);

        rc = dt_trans_start_local(env, los->los_dev, th);
        if (rc)
                GOTO(out_stop, rc);

        /* update local oid number on disk */
        losd.magic = 0xdecafbee;
        losd.next_id = cpu_to_le32(los->los_last_oid);

        dti->dti_off = 0;
        dti->dti_lb.lb_buf = &losd;
        dti->dti_lb.lb_len = sizeof(losd);
        rc = dt_record_write(env, los->los_obj, &dti->dti_lb, &dti->dti_off,
                             th);
out_stop:
        dt_trans_stop(env, los->los_dev, th);
out:
        return rc;
}

int local_object_declare_create(const struct lu_env *env, struct dt_object *o,
                                struct lu_attr *attr,
                                struct dt_object_format *dof,
                                struct thandle *th)
{
        struct dt_thread_info *dti = dt_info(env);
        int                    rc;

        ENTRY;

        rc = dt_declare_create(env, o, attr, NULL, dof, th);
        if (rc)
                RETURN(rc);

        dti->dti_lb.lb_buf = NULL;
        dti->dti_lb.lb_len = sizeof(dti->dti_lma);
        rc = dt_declare_xattr_set(env, o, &dti->dti_lb, XATTR_NAME_LMA, 0, th);

        RETURN(rc);
}

int local_object_create(const struct lu_env *env, struct dt_object *o,
                        struct lu_attr *attr, struct dt_object_format *dof,
                        struct thandle *th)
{
        struct dt_thread_info   *dti = dt_info(env);
        int                      rc;

        ENTRY;

        rc = dt_create(env, o, attr, NULL, dof, th);
        if (rc)
                RETURN(rc);

        lustre_lma_init(&dti->dti_lma, lu_object_fid(&o->do_lu));
        lustre_lma_swab(&dti->dti_lma);
        dti->dti_lb.lb_buf = &dti->dti_lma;
        dti->dti_lb.lb_len = sizeof(dti->dti_lma);
        rc = dt_xattr_set(env, o, &dti->dti_lb, XATTR_NAME_LMA, 0, th, BYPASS_CAPA);

        RETURN(rc);
}

/*
 * Create local named object in parent directory.
 */
struct dt_object *local_file_find_or_create(const struct lu_env *env,
                                            struct local_oid_storage *los,
                                            struct dt_object *parent,
                                            const char *name, __u32 mode)
{
        struct dt_thread_info *dti = dt_info(env);
        struct dt_object      *dto = NULL;
        struct thandle        *th;
        int                    rc;
        ENTRY;

        LASSERT(parent);

        rc = dt_lookup_dir(env, parent, name, &dti->dti_fid);
        if (rc == 0) {
                /* name is found, get the object */
                dto = dt_locate_at(env, los->los_dev, &dti->dti_fid,
                                   los->los_top);
                RETURN(dto);
        } else if (rc == -ENOENT) {
                rc = local_object_fid_generate(env, los, &dti->dti_fid, 1);
        }
        if (rc < 0)
                RETURN(ERR_PTR(rc));

        dto = dt_locate_at(env, los->los_dev, &dti->dti_fid, los->los_top);
        if (unlikely(IS_ERR(dto)))
                RETURN(dto);

        LASSERT(dto != NULL);
        if (dt_object_exists(dto))
                GOTO(out, rc = -EEXIST);

        th = dt_trans_create(env, los->los_dev);
        if (IS_ERR(th))
                GOTO(out, rc = PTR_ERR(th));

        dti->dti_attr.la_valid = LA_MODE;
        dti->dti_attr.la_mode = mode;
        dti->dti_dof.dof_type = dt_mode_to_dft(mode & S_IFMT);

        rc = local_object_declare_create(env, dto, &dti->dti_attr,
                                         &dti->dti_dof, th);
        if (rc)
                GOTO(trans_stop, rc);

        if (S_ISDIR(mode))
                dt_declare_ref_add(env, dto, th);

        rc = dt_declare_insert(env, parent, (void *)&dti->dti_fid,
                               (void *)name, th);
        if (rc)
                GOTO(trans_stop, rc);

        rc = dt_trans_start_local(env, los->los_dev, th);
        if (rc)
                GOTO(trans_stop, rc);

        dt_write_lock(env, dto, 0);
        if (dt_object_exists(dto))
                GOTO(unlock, rc = 0);

        CDEBUG(D_OTHER, "create new object %lu:0x"LPX64"\n",
               (unsigned long) dti->dti_fid.f_oid, dti->dti_fid.f_seq);

        rc = local_object_create(env, dto, &dti->dti_attr, &dti->dti_dof, th);
        if (rc)
                GOTO(unlock, rc);
        LASSERT(dt_object_exists(dto));

        if (S_ISDIR(mode)) {
                if (!dt_try_as_dir(env, dto))
                        GOTO(destroy, rc = -ENOTDIR);
                /* Add "." and ".." for newly created dir */
                rc = dt_insert(env, dto, (void *)&dti->dti_fid,
                               (void *)".", th, BYPASS_CAPA, 1);
                if (rc)
                        GOTO(destroy, rc);
                rc = dt_insert(env, dto, (void *)lu_object_fid(&parent->do_lu),
                               (void *)"..", th, BYPASS_CAPA, 1);
                if (rc)
                        GOTO(destroy, rc);
                dt_ref_add(env, dto, th);
        }

        dt_write_lock(env, parent, 0);
        rc = dt_insert(env, parent, (const struct dt_rec *)&dti->dti_fid,
                       (const struct dt_key *)name, th, BYPASS_CAPA, 1);
        dt_write_unlock(env, parent);
        if (rc)
                GOTO(destroy, rc);
destroy:
        if (rc)
                dt_destroy(env, dto, th);
unlock:
        dt_write_unlock(env, dto);
trans_stop:
        dt_trans_stop(env, los->los_dev, th);
out:
        if (rc) {
                lu_object_put(env, &dto->do_lu);
                dto = ERR_PTR(rc);
        }
        RETURN(dto);
}
EXPORT_SYMBOL(local_file_find_or_create);

static struct local_oid_storage *dt_los_find(struct dt_device *dev, __u64 seq)
{
        struct local_oid_storage *los, *ret = NULL;

        cfs_list_for_each_entry(los, &los_list_head, los_list) {
                if (los->los_dev == dev &&
                    los->los_seq == seq) {
                        cfs_atomic_inc(&los->los_refcount);
                        ret = los;
                        break;
                }
        }
        return ret;
}

/**
 * Initialize local OID storage for required sequence.
 * That may be needed for services that uses local files and requires
 * dynamic OID allocation for them.
 *
 * Per each sequence we have an object with 'first_fid' identificator
 * containing the counter for OIDs of locally created files with that
 * sequence.
 *
 * It is used now by llog subsystem and MGS for NID tables
 *
 * Function gets first_fid to create counter object.
 * All dynamic fids will be generated with the same sequence and incremented
 * OIDs
 *
 * Returned dt_oid_storage is in-memory representaion of OID storage
 */
int local_oid_storage_init(const struct lu_env *env,
                           struct dt_device *dev, struct lu_device *top,
                           const struct lu_fid *first_fid,
                           struct local_oid_storage **los)
{
        struct dt_thread_info *dti = dt_info(env);
        struct los_ondisk      losd;
        struct dt_object      *o;
        struct thandle        *th;
        int                    rc;
        ENTRY;

        cfs_mutex_lock(&los_list_mutex);
        *los = dt_los_find(dev, fid_seq(first_fid));
        if (*los != NULL)
                GOTO(out, rc = 0);

        /* not found, then create */
        OBD_ALLOC_PTR(*los);
        if (*los == NULL)
                GOTO(out, rc = -ENOMEM);

        cfs_atomic_set(&(*los)->los_refcount, 1);
        cfs_spin_lock_init(&(*los)->los_id_lock);
        (*los)->los_dev = dev;
        (*los)->los_top = top;
        cfs_list_add(&(*los)->los_list, &los_list_head);

        /* initialize data allowing to generate new fids,
         * literally we need a sequence */
        o = dt_locate_at(env, dev, first_fid, top);
        if (IS_ERR(o))
                GOTO(out_los, rc = PTR_ERR(o));

        dt_write_lock(env, o, 0);
        if (!dt_object_exists(o)) {
                th = dt_trans_create(env, dev);
                if (IS_ERR(th))
                        GOTO(out_lock, rc = PTR_ERR(th));

                dti->dti_attr.la_valid = LA_MODE;
                dti->dti_attr.la_mode = S_IFREG | S_IRUGO | S_IWUSR;
                dti->dti_dof.dof_type = dt_mode_to_dft(S_IFREG);

                rc = local_object_declare_create(env, o, &dti->dti_attr,
                                                 &dti->dti_dof, th);
                if (rc)
                        GOTO(out_trans, rc);

                rc = dt_declare_record_write(env, o, sizeof(losd), 0, th);
                if (rc)
                        GOTO(out_trans, rc);

                rc = dt_trans_start_local(env, dev, th);
                if (rc)
                        GOTO(out_trans, rc);

                LASSERT(!dt_object_exists(o));
                rc = local_object_create(env, o,  &dti->dti_attr,
                                         &dti->dti_dof, th);
                if (rc)
                        GOTO(out_trans, rc);
                LASSERT(dt_object_exists(o));

                losd.magic   = 0xdecafbee;
                losd.next_id = cpu_to_le32(fid_oid(first_fid) + 1);

                dti->dti_off = 0;
                dti->dti_lb.lb_buf = &losd;
                dti->dti_lb.lb_len = sizeof(losd);
                rc = dt_record_write(env, o, &dti->dti_lb, &dti->dti_off, th);
                if (rc)
                        GOTO(out_trans, rc);
out_trans:
                dt_trans_stop(env, dev, th);
        } else {
                dti->dti_off = 0;
                dti->dti_lb.lb_buf = &losd;
                dti->dti_lb.lb_len = sizeof(losd);
                rc = dt_record_read(env, o, &dti->dti_lb, &dti->dti_off);
                LASSERT(ergo(rc == 0, losd.magic == 0xdecafbee));
        }
out_lock:
        dt_write_unlock(env, o);
out_los:
        if (rc) {
                OBD_FREE_PTR(*los);
                *los = NULL;
                if (o) {
                        /* drop object immediately from cache */
                        cfs_set_bit(LU_OBJECT_HEARD_BANSHEE,
                                    &o->do_lu.lo_header->loh_flags);
                        lu_object_put(env, &o->do_lu);
                }
        } else {
                (*los)->los_seq = fid_seq(first_fid);
                (*los)->los_last_oid = le32_to_cpu(losd.next_id);
                (*los)->los_obj = o;
        }
out:
        cfs_mutex_unlock(&los_list_mutex);
        return rc;
}
EXPORT_SYMBOL(local_oid_storage_init);

void local_oid_storage_fini(const struct lu_env *env,
                            struct local_oid_storage *los)
{
        struct lu_object *lo;

        LASSERT(env);

        if (!cfs_atomic_dec_and_test(&los->los_refcount))
                return;

        cfs_mutex_lock(&los_list_mutex);
        if (cfs_atomic_read(&los->los_refcount) == 0) {
                if (los->los_obj) {
                        /*
                         * set the flag to release object from
                         * the cache immediately. 
                         */
                        lo = &los->los_obj->do_lu;
                        cfs_set_bit(LU_OBJECT_HEARD_BANSHEE,
                                    &lo->lo_header->loh_flags);
                        lu_object_put(env, lo);
                }
                cfs_list_del(&los->los_list);
                OBD_FREE_PTR(los);
        }
        cfs_mutex_unlock(&los_list_mutex);
}
EXPORT_SYMBOL(local_oid_storage_fini);

/* dt class init function. */
int dt_global_init(void)
{
        int result;

        LU_CONTEXT_KEY_INIT(&dt_key);
        result = lu_context_key_register(&dt_key);
        return result;
}

void dt_global_fini(void)
{
        lu_context_key_degister(&dt_key);
}

/* generic read, may return short read to the caller */
int dt_read(const struct lu_env *env, struct dt_object *dt,
            struct lu_buf *buf, loff_t *pos)
{
        LASSERTF(dt != NULL, "dt is NULL when we want to read record\n");
        return dt->do_body_ops->dbo_read(env, dt, buf, pos, BYPASS_CAPA);
}
EXPORT_SYMBOL(dt_read);

/* contrary to the dt_read the dt_record_read() demand only the same
 * size as requested and must be used for reading structures of known size */
int dt_record_read(const struct lu_env *env, struct dt_object *dt,
                   struct lu_buf *buf, loff_t *pos)
{
        int rc;

        LASSERTF(dt != NULL, "dt is NULL when we want to read record\n");

        rc = dt->do_body_ops->dbo_read(env, dt, buf, pos, BYPASS_CAPA);

        if (rc == buf->lb_len)
                rc = 0;
        else if (rc >= 0)
                rc = -EFAULT;
        return rc;
}
EXPORT_SYMBOL(dt_record_read);

int dt_record_write(const struct lu_env *env, struct dt_object *dt,
                    const struct lu_buf *buf, loff_t *pos, struct thandle *th)
{
        int rc;

        LASSERTF(dt != NULL, "dt is NULL when we want to write record\n");
        LASSERT(th != NULL);
        LASSERT(dt->do_body_ops);
        LASSERT(dt->do_body_ops->dbo_write);
        rc = dt->do_body_ops->dbo_write(env, dt, buf, pos, th, BYPASS_CAPA, 1);
        if (rc == buf->lb_len)
                rc = 0;
        else if (rc >= 0)
                rc = -EFAULT;
        return rc;
}
EXPORT_SYMBOL(dt_record_write);

int dt_declare_version_set(const struct lu_env *env, struct dt_object *o,
                           struct thandle *th)
{
        struct lu_buf vbuf;
        char *xname = XATTR_NAME_VERSION;

        LASSERT(o);
        vbuf.lb_buf = NULL;
        vbuf.lb_len = sizeof(dt_obj_version_t);
        return dt_declare_xattr_set(env, o, &vbuf, xname, 0, th);

}
EXPORT_SYMBOL(dt_declare_version_set);

void dt_version_set(const struct lu_env *env, struct dt_object *o,
                    dt_obj_version_t version, struct thandle *th)
{
        struct lu_buf vbuf;
        char *xname = XATTR_NAME_VERSION;
        int rc;

        LASSERT(o);
        vbuf.lb_buf = &version;
        vbuf.lb_len = sizeof(version);

        rc = dt_xattr_set(env, o, &vbuf, xname, 0, th, BYPASS_CAPA);
        if (rc < 0)
                CDEBUG(D_INODE, "Can't set version, rc %d\n", rc);
        return;
}
EXPORT_SYMBOL(dt_version_set);

dt_obj_version_t dt_version_get(const struct lu_env *env, struct dt_object *o)
{
        struct lu_buf vbuf;
        char *xname = XATTR_NAME_VERSION;
        dt_obj_version_t version;
        int rc;

        LASSERT(o);
        vbuf.lb_buf = &version;
        vbuf.lb_len = sizeof(version);
        rc = dt_xattr_get(env, o, &vbuf, xname, BYPASS_CAPA);
        if (rc != sizeof(version)) {
                CDEBUG(D_INODE, "Can't get version, rc %d\n", rc);
                version = 0;
        }
        return version;
}
EXPORT_SYMBOL(dt_version_get);

const struct dt_index_features dt_directory_features;
EXPORT_SYMBOL(dt_directory_features);
