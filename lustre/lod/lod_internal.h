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
 * Copyright  2009 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lod/lod_internal.h
 *
 * Author: Alex Zhuravlev <bzzz@sun.com>
 * Author: Mikhail Pershin <tappro@whamcloud.com>
 */

#ifndef _LOD_INTERNAL_H
#define _LOD_INTERNAL_H

#include <libcfs/libcfs.h>
#include <obd.h>
#include <dt_object.h>

#define LOV_USES_ASSIGNED_STRIPE        0
#define LOV_USES_DEFAULT_STRIPE         1

#define LOD_MAX_OSTNR                  128  /* XXX: should be dynamic */
#define LOD_BITMAP_SIZE                (LOD_MAX_OSTNR / sizeof(unsigned long) + 1)

struct lod_ost_desc {
        struct dt_device  *ltd_ost;
        struct obd_export *ltd_exp;
        struct ltd_qos     ltd_qos; /* qos info per target */
        struct obd_statfs  ltd_statfs;
};

struct lod_device {
        struct dt_device      lod_dt_dev;
        struct obd_export    *lod_child_exp;
        struct dt_device     *lod_child;
        cfs_proc_dir_entry_t *lod_proc_entry;
        struct lprocfs_stats *lod_stats;
        int                   lod_connects;
        int                   lod_recovery_completed;

        /* list of known OSTs, XXX: to be dynamic */
        struct lod_ost_desc   lod_desc[LOD_MAX_OSTNR];

        /* bitmap of OSTs available */
        unsigned long         lod_ost_bitmap[LOD_BITMAP_SIZE];

        /* number of known OSTs */
        int                   lod_ostnr;
        cfs_semaphore_t       lod_mutex;
        /* maximum EA size underlied OSD may have */
        unsigned int          lod_osd_max_easize;
        struct lov_qos        lod_qos; /* qos info per lod */

        cfs_proc_dir_entry_t *lod_symlink;
};

/*
 * XXX: shrink this structure, currently it's 72bytes on 32bit arch,
 *      so, slab will be allocating 128bytes
 */
struct lod_object {
        struct dt_object   mbo_obj;

        /* if object is striped, then the next fields describe stripes */
        __u16              mbo_stripenr;
        __u16              mbo_layout_gen;
        __u32              mbo_stripe_size;
        char              *mbo_pool;
        struct dt_object **mbo_stripe;
        /* to know how much memory to free, mbo_stripenr can be less */
        int                mbo_stripes_allocated;
        /* default striping for directory represented by this object
         * is cached in stripenr/stripe_size */
        int                mbo_striping_cached;
        __u32              mbo_def_stripe_size;
        __u16              mbo_def_stripenr;
        __u16              mbo_def_stripe_offset;
};

struct lod_thread_info {
        /* array of OSTs selected in striping creation */
        char         *lti_ea_store;              /* a buffer for lov ea */
        int           lti_ea_store_size;
        struct lu_buf lti_buf;
        struct ost_id lti_ostid;
        struct lu_fid lti_fid;
};

extern const struct lu_device_operations lod_lu_ops;

static inline int lu_device_is_lod(struct lu_device *d)
{
        return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &lod_lu_ops);
}

static inline struct lod_device* lu2lod_dev(struct lu_device *d)
{
        LASSERT(lu_device_is_lod(d));
        return container_of0(d, struct lod_device, lod_dt_dev.dd_lu_dev);
}

static inline struct lu_device *lod2lu_dev(struct lod_device *d)
{
        return &d->lod_dt_dev.dd_lu_dev;
}

static inline struct obd_device *lod2obd(struct lod_device *d)
{
        return d->lod_dt_dev.dd_lu_dev.ld_obd;
}

static inline struct lov_obd *lod2lov(struct lod_device *d)
{
        return &d->lod_dt_dev.dd_lu_dev.ld_obd->u.lov;
}

static inline struct lod_device *dt2lod_dev(struct dt_device *d)
{
        LASSERT(lu_device_is_lod(&d->dd_lu_dev));
        return container_of0(d, struct lod_device, lod_dt_dev);
}

static inline struct lod_object *lu2lod_obj(struct lu_object *o)
{
        LASSERT(ergo(o != NULL, lu_device_is_lod(o->lo_dev)));
        return container_of0(o, struct lod_object, mbo_obj.do_lu);
}

static inline struct lu_object *lod2lu_obj(struct lod_object *obj)
{
        return &obj->mbo_obj.do_lu;
}

static inline struct lod_object *lod_obj(const struct lu_object *o)
{
        LASSERT(lu_device_is_lod(o->lo_dev));
        return container_of0(o, struct lod_object, mbo_obj.do_lu);
}

static inline struct lod_object *lod_dt_obj(const struct dt_object *d)
{
        return lod_obj(&d->do_lu);
}

static inline struct dt_object* lod_object_child(struct lod_object *o)
{
        return container_of0(lu_object_next(lod2lu_obj(o)),
                             struct dt_object, do_lu);
}

static inline struct dt_object *lu2dt_obj(struct lu_object *o)
{
        LASSERT(ergo(o != NULL, lu_device_is_dt(o->lo_dev)));
        return container_of0(o, struct dt_object, do_lu);
}

static inline struct dt_object *dt_object_child(struct dt_object *o)
{
        return container_of0(lu_object_next(&(o)->do_lu),
                             struct dt_object, do_lu);
}

extern struct lu_context_key lod_thread_key;

static inline struct lod_thread_info *lod_env_info(const struct lu_env *env)
{
        struct lod_thread_info *info;
        info = lu_context_key_get(&env->le_ctx, &lod_thread_key);
        LASSERT(info);
        return info;
}

/* lod_lov.c */
int lod_lov_add_device(const struct lu_env *env, struct lod_device *m,
                        char *osp, unsigned index, unsigned gen);
int lod_lov_del_device(const struct lu_env *env, struct lod_device *m,
                        char *osp, unsigned gen);
int lod_generate_and_set_lovea(const struct lu_env *env,
                                struct lod_object *mo,
                                struct thandle *th);
int lod_load_striping(const struct lu_env *env, struct lod_object *mo);
int lod_get_lov_ea(const struct lu_env *env, struct lod_object *mo);
void lod_fix_desc(struct lov_desc *desc);
void lod_fix_desc_qos_maxage(__u32 *val);
void lod_fix_desc_pattern(__u32 *val);
void lod_fix_desc_stripe_count(__u32 *val);
void lod_fix_desc_stripe_size(__u64 *val);
int lod_lov_init(struct lod_device *m, struct lustre_cfg *cfg);
int lod_lov_fini(struct lod_device *m);
int lod_parse_striping(const struct lu_env *env, struct lod_object *mo,
                       const struct lu_buf *buf);
int lod_initialize_objects(const struct lu_env *env, struct lod_object *mo,
                           struct lov_ost_data_v1 *objs);
int lod_store_def_striping(const struct lu_env *env, struct dt_object *dt,
                           struct thandle *th);

/* lod_pool.c */
int lov_ost_pool_add(struct ost_pool *op, __u32 idx, unsigned int min_count);
int lov_ost_pool_extend(struct ost_pool *op, unsigned int min_count);
struct pool_desc *lov_find_pool(struct lov_obd *lov, char *poolname);
void lod_pool_putref(struct pool_desc *pool);
int lov_ost_pool_free(struct ost_pool *op);
int lod_pool_del(struct obd_device *obd, char *poolname);
int lov_ost_pool_init(struct ost_pool *op, unsigned int count);
extern cfs_hash_ops_t pool_hash_operations;
int lov_check_index_in_pool(__u32 idx, struct pool_desc *pool);
int lod_pool_new(struct obd_device *obd, char *poolname);
int lod_pool_add(struct obd_device *obd, char *poolname, char *ostname);
int lod_pool_remove(struct obd_device *obd, char *poolname, char *ostname);

/* lod_qos.c */
int lod_qos_prep_create(const struct lu_env *env, struct lod_object *lo,
                        struct lu_attr *attr, const struct lu_buf *,
                        struct thandle *th);
int lod_alloc_replay(const struct lu_env *env, struct lod_object *lo,
                     struct lu_attr *attr, const struct lu_buf *buf,
                     struct thandle *th);
int qos_add_tgt(struct lod_device*, int);
int qos_del_tgt(struct lod_device*, int);

/* lproc_lod.c */
extern struct file_operations lod_proc_target_fops;
void lprocfs_lod_init_vars(struct lprocfs_static_vars *lvars);

/* lod_object.c */
int lod_object_set_pool(struct lod_object *o, char *pool);
int lod_declare_striped_object(const struct lu_env *env, struct dt_object *dt,
                               struct lu_attr *attr, const struct lu_buf *buf,
                               struct thandle *th);
int lod_striping_create(const struct lu_env *env, struct dt_object *dt,
                        struct lu_attr *attr, struct dt_object_format *dof,
                        struct thandle *th);
void lod_object_free_striping(const struct lu_env *env, struct lod_object *o);

#endif

