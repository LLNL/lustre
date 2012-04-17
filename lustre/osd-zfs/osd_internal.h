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
 * lustre/osd/osd_internal.h
 * Shared definitions and declarations for zfs/dmu osd
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 */

#ifndef _OSD_INTERNAL_H
#define _OSD_INTERNAL_H

#include <dt_object.h>

#include <sys/nvpair.h>

#include "udmu.h"

#define LUSTRE_ROOT_FID_SEQ     0
#define DMU_OSD_SVNAME          "svname"
#define DMU_OSD_OI_NAME_BASE    "oi"

#define OSD_GFP_IO              (GFP_NOFS | __GFP_HIGHMEM)

/**
 * Iterator's in-memory data structure for quota file.
 */
struct osd_it_quota {
        struct osd_object    *oiq_obj;
        /* DMU accounting object id */
        uint64_t              oiq_oid;
        /* ZAP cursor */
        zap_cursor_t         *oiq_zc;
        /** identifier for current quota record */
        __u64                 oiq_id;
        unsigned              oiq_reset:1; /* 1 -- no need to advance */
};

/**
 * Iterator's in-memory data structure for ZAPs
 */
#define IT_REC_SIZE 256

struct osd_zap_it {
        zap_cursor_t            *ozi_zc;
        struct osd_object       *ozi_obj;
        struct lustre_capa      *ozi_capa;
        unsigned                 ozi_reset:1;     /* 1 -- no need to advance */
        char                     ozi_name[NAME_MAX + 1];
        char                     ozi_rec[IT_REC_SIZE];
};
#define DT_IT2DT(it) (&((struct osd_zap_it *)it)->ozi_obj->oo_dt)

/*
 * regular ZFS direntry
 */
struct zpl_direntry {
        uint64_t        zde_dnode:48,
                        zde_pad:12,
                        zde_type:4;
} __attribute__((packed));

/*
 * lustre direntry adds a fid to regular ZFS direntry
 */
struct luz_direntry {
        struct zpl_direntry     lzd_reg;
        struct lu_fid           lzd_fid;
} __attribute__((packed));


/* cached SA attributes */
struct osa_attr {
        uint64_t  mode;
        uint64_t  gid;
        uint64_t  uid;
        uint64_t  nlink;
        uint64_t  rdev;
        uint64_t  flags;
        uint64_t  size;
        uint64_t  atime[2];
        uint64_t  mtime[2];
        uint64_t  ctime[2];
};

struct osd_thread_info {
        const struct lu_env   *oti_env;

        struct lu_fid          oti_fid;
        /*
         * XXX temporary: for ->i_op calls.
         */
        struct timespec        oti_time;
        /*
         * XXX temporary: for capa operations.
         */
        struct lustre_capa_key oti_capa_key;
        struct lustre_capa     oti_capa;

        struct ost_id          oti_ostid;

        char                   oti_buf[64];

        /** osd iterator context used for iterator session */
        union {
                struct osd_zap_it   oti_it_zap;
                struct osd_it_quota oti_it_quota;
        };

        char                   oti_str[64];
        char                   oti_key[MAXNAMELEN + 1];

        struct lu_attr         oti_la;
        struct osa_attr        oti_osa;
        zap_attribute_t        oti_za;
        dmu_object_info_t      oti_doi;
        struct luz_direntry    oti_zde;
};

extern struct lu_context_key osd_key;

static inline struct osd_thread_info *osd_oti_get(const struct lu_env *env)
{
        return lu_context_key_get(&env->le_ctx, &osd_key);
}

struct osd_thandle {
        struct thandle          ot_super;
        cfs_list_t              ot_dcb_list;
        cfs_list_t              ot_sa_list;
        cfs_semaphore_t         ot_sa_lock;
        dmu_tx_t               *ot_tx;
        __u32                   ot_write_commit:1,
                                ot_assigned:1;
};

#define OSD_OI_NAME_SIZE        16

/*
 * Object Index (OI) instance.
 */
struct osd_oi {
        char                    oi_name[OSD_OI_NAME_SIZE]; /* unused */
        uint64_t                oi_zapid;
};

#define OSD_OST_MAP_SIZE        32

/*
 * osd device.
 */
struct osd_device {
        /* super-class */
        struct dt_device          od_dt_dev;
        /* information about underlying file system */
        udmu_objset_t             od_objset;

        /*
         * Fid Capability
         */
        unsigned int              od_fl_capa:1;
        unsigned long             od_capa_timeout;
        __u32                     od_capa_alg;
        struct lustre_capa_key   *od_capa_keys;
        cfs_hlist_head_t         *od_capa_hash;

        cfs_proc_dir_entry_t     *od_proc_entry;
        struct lprocfs_stats     *od_stats;

        uint64_t                  od_root;
        struct osd_oi           **od_oi_table;
        unsigned int              od_oi_count;
        uint64_t                  od_ost_compat_dirs[OSD_OST_MAP_SIZE];
        uint64_t                  od_ost_compat_grp0;

        unsigned int              od_rdonly:1,
                                  od_quota_iused_est:1;
        char                      od_mntdev[128];
        char                      od_svname[128];

        int                       od_connects;
        struct lu_site            od_site;

        /* object IDs of the inode accounting indexes */
        uint64_t                  od_iusr_oid;
        uint64_t                  od_igrp_oid;

        /* used to debug zerocopy logic: the fields track all
         * allocated, loaned and referenced buffers in use.
         * to be removed once the change is tested well. */
        cfs_atomic_t              od_zerocopy_alloc;
        cfs_atomic_t              od_zerocopy_loan;
        cfs_atomic_t              od_zerocopy_pin;
};

struct osd_object {
        struct dt_object     oo_dt;
        /*
         * Inode for file system object represented by this osd_object. This
         * inode is pinned for the whole duration of lu_object life.
         *
         * Not modified concurrently (either setup early during object
         * creation, or assigned by osd_object_create() under write lock).
         */
        dmu_buf_t           *oo_db;
        sa_handle_t         *oo_sa_hdl;
        nvlist_t            *oo_sa_xattr;
        cfs_list_t           oo_sa_linkage;

        cfs_rw_semaphore_t   oo_sem;

        /* cached attributes */
        cfs_rwlock_t         oo_attr_lock;
        struct lu_attr       oo_attr;

        /* protects extended attributes */
        cfs_semaphore_t      oo_guard;
        uint64_t             oo_xattr;

        const struct lu_env *oo_owner;
};

int osd_statfs(const struct lu_env *env, struct dt_device *d, struct obd_statfs *osfs);
extern const struct dt_index_operations osd_acct_index_ops;
uint64_t osd_quota_fid2dmu(const struct lu_fid *fid);
extern struct lu_device_operations  osd_lu_ops;

/*
 * Helpers.
 */
static inline int lu_device_is_osd(const struct lu_device *d)
{
        return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &osd_lu_ops);
}

static inline struct osd_object *osd_obj(const struct lu_object *o)
{
        LASSERT(lu_device_is_osd(o->lo_dev));
        return container_of0(o, struct osd_object, oo_dt.do_lu);
}

static inline struct osd_device *osd_dt_dev(const struct dt_device *d)
{
        LASSERT(lu_device_is_osd(&d->dd_lu_dev));
        return container_of0(d, struct osd_device, od_dt_dev);
}

static inline struct osd_device *osd_dev(const struct lu_device *d)
{
        LASSERT(lu_device_is_osd(d));
        return osd_dt_dev(container_of0(d, struct dt_device, dd_lu_dev));
}

static inline struct osd_object *osd_dt_obj(const struct dt_object *d)
{
        return osd_obj(&d->do_lu);
}

static inline struct osd_device *osd_obj2dev(const struct osd_object *o)
{
        return osd_dev(o->oo_dt.do_lu.lo_dev);
}

static inline struct lu_device *osd2lu_dev(struct osd_device *osd)
{
        return &osd->od_dt_dev.dd_lu_dev;
}

static inline struct objset * osd_dtobj2objset(struct dt_object *o)
{
        return osd_dev(o->do_lu.lo_dev)->od_objset.os;
}

static inline int osd_invariant(const struct osd_object *obj)
{
        return 1;
}

static inline int osd_object_invariant(const struct lu_object *l)
{
        return osd_invariant(osd_obj(l));
}


#ifdef LPROCFS
enum {
        LPROC_OSD_READ_BYTES = 0,
        LPROC_OSD_WRITE_BYTES = 1,
        LPROC_OSD_GET_PAGE = 2,
        LPROC_OSD_NO_PAGE = 3,
        LPROC_OSD_CACHE_ACCESS = 4,
        LPROC_OSD_CACHE_HIT = 5,
        LPROC_OSD_CACHE_MISS = 6,
        LPROC_OSD_COPY_IO = 7,
        LPROC_OSD_ZEROCOPY_IO = 8,
        LPROC_OSD_TAIL_IO = 9,
        LPROC_OSD_LAST,
};

/* osd_lproc.c */
extern struct lprocfs_vars lprocfs_osd_obd_vars[];
extern struct lprocfs_vars lprocfs_osd_module_vars[];

int osd_procfs_init(struct osd_device *osd, const char *name);
int osd_procfs_fini(struct osd_device *osd);

int udmu_zap_cursor_retrieve_key(const struct lu_env *env,
                                 zap_cursor_t *zc, char *key, int max);
int udmu_zap_cursor_retrieve_value(const struct lu_env *env,
                                   zap_cursor_t *zc,  char *buf,
                                   int buf_size, int *bytes_read);

#endif
#endif /* _OSD_INTERNAL_H */
