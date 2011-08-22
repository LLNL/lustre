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

#define LUSTRE_ROOT_FID_SEQ     0
#define DMU_RESERVED_FRACTION   25 /* default reserved fraction 1/25 = 4% */
#define DMU_OSD_SVNAME          "svname"
#define DMU_OSD_OI_NAME         "OBJECTS"

/**
 * Storage representation for fids.
 *
 * Variable size, first byte contains the length of the whole record.
 */
struct osd_fid_pack {
        unsigned char fp_len;
        char fp_area[sizeof(struct lu_fid)];
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

        struct osd_fid_pack    oti_fid_pack;
};

struct osd_thandle {
        struct thandle          ot_super;
        cfs_list_t              ot_dcb_list;
        dmu_tx_t               *ot_tx;
        __u32                   ot_sync:1,
                                ot_assigned:1;
};

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

        /*
         * statfs optimization: we cache a bit.
         */
        struct obd_statfs         od_osfs;
        cfs_time_t                od_osfs_age;
        cfs_spinlock_t            od_osfs_lock;

        cfs_proc_dir_entry_t     *od_proc_entry;
        struct lprocfs_stats     *od_stats;

        dmu_buf_t                *od_root_db;
        dmu_buf_t                *od_objdir_db;

        unsigned int              od_rdonly:1;
        char                      od_mntdev[128];
        char                      od_svname[128];

        int                       od_reserved_fraction;
        int                       od_connects;
        struct lu_site            od_site;
};

struct osd_object {
        struct dt_object       oo_dt;
        /*
         * Inode for file system object represented by this osd_object. This
         * inode is pinned for the whole duration of lu_object life.
         *
         * Not modified concurrently (either setup early during object
         * creation, or assigned by osd_object_create() under write lock).
         */
        dmu_buf_t               *oo_db;

        /* protects inode attributes. */
        cfs_semaphore_t         oo_guard;
        cfs_rw_semaphore_t      oo_sem;

        uint64_t                oo_mode;
        uint64_t                oo_type;
};

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

int osd_statfs(const struct lu_env *env, struct dt_device *d, struct obd_statfs *osfs);

#ifdef LPROCFS
enum {
        LPROC_OSD_READ_BYTES = 0,
        LPROC_OSD_WRITE_BYTES = 1,
        LPROC_OSD_GET_PAGE = 2,
        LPROC_OSD_NO_PAGE = 3,
        LPROC_OSD_CACHE_ACCESS = 4,
        LPROC_OSD_CACHE_HIT = 5,
        LPROC_OSD_CACHE_MISS = 6,
        LPROC_OSD_LAST,
};

/* osd_lproc.c */
extern struct lprocfs_vars lprocfs_osd_obd_vars[];
extern struct lprocfs_vars lprocfs_osd_module_vars[];

int osd_procfs_init(struct osd_device *osd, const char *name);
int osd_procfs_fini(struct osd_device *osd);
#endif

#endif /* _OSD_INTERNAL_H */
