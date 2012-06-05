/*
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
 *
 * Copyright (c) 2011, 2012, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/osd/osd_internal.h
 *
 * Shared definitions and declarations for osd module
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 */

#ifndef _OSD_INTERNAL_H
#define _OSD_INTERNAL_H

#if defined(__KERNEL__)

/* struct rw_semaphore */
#include <linux/rwsem.h>
/* struct dentry */
#include <linux/dcache.h>
/* struct dirent64 */
#include <linux/dirent.h>

#include <ldiskfs/ldiskfs.h>
#include <ldiskfs/ldiskfs_jbd2.h>
#include <ldiskfs/ldiskfs_extents.h>
#ifdef HAVE_LDISKFS_JOURNAL_CALLBACK_ADD
# define journal_callback ldiskfs_journal_cb_entry
# define osd_journal_callback_set(handle, func, jcb) \
         ldiskfs_journal_callback_add(handle, func, jcb)
#else
# define osd_journal_callback_set(handle, func, jcb) \
         jbd2_journal_callback_set(handle, func, jcb)
#endif

/* fsfilt_{get|put}_ops */
#include <lustre_fsfilt.h>

/* LUSTRE_OSD_LDISKFS_NAME */
#include <obd.h>
/* class_register_type(), class_unregister_type(), class_get_type() */
#include <obd_class.h>
#include <lustre_disk.h>

#include <lustre_fsfilt.h>

#include <dt_object.h>
#include "osd_oi.h"
#include "osd_iam.h"

struct inode;

#define OSD_OII_NOGEN (0)
#define OSD_COUNTERS (0)

/** Enable thandle usage statistics */
#define OSD_THANDLE_STATS

#ifdef HAVE_QUOTA_SUPPORT
struct osd_ctxt {
        __u32 oc_uid;
        __u32 oc_gid;
        __u32 oc_cap;
};
#endif

struct osd_compat_objid;

struct osd_directory {
        struct iam_container od_container;
        struct iam_descr     od_descr;
};

/*
 * Object Index (oi) instance.
 */
struct osd_oi {
        /*
         * underlying index object, where fid->id mapping in stored.
         */
        struct inode         *oi_inode;
        struct osd_directory  oi_dir;
};

extern const int osd_dto_credits_noquota[];

struct osd_object {
        struct dt_object        oo_dt;
        /**
         * Inode for file system object represented by this osd_object. This
         * inode is pinned for the whole duration of lu_object life.
         *
         * Not modified concurrently (either setup early during object
         * creation, or assigned by osd_object_create() under write lock).
         */
        struct inode           *oo_inode;
        /**
         * to protect index ops.
         */
        struct htree_lock_head *oo_hl_head;
        cfs_rw_semaphore_t      oo_ext_idx_sem;
        cfs_rw_semaphore_t      oo_sem;
        struct osd_directory   *oo_dir;
        /** protects inode attributes. */
        cfs_spinlock_t          oo_guard;
        /**
         * Following two members are used to indicate the presence of dot and
         * dotdot in the given directory. This is required for interop mode
         * (b11826).
         */
        int                     oo_compat_dot_created;
        int                     oo_compat_dotdot_created;

        const struct lu_env    *oo_owner;
#ifdef CONFIG_LOCKDEP
        struct lockdep_map      oo_dep_map;
#endif
};

#ifdef HAVE_LDISKFS_PDO

#define osd_ldiskfs_find_entry(dir, dentry, de, lock)   \
        ll_ldiskfs_find_entry(dir, dentry, de, lock)
#define osd_ldiskfs_add_entry(handle, child, cinode, hlock) \
        ldiskfs_add_entry(handle, child, cinode, hlock)

#else /* HAVE_LDISKFS_PDO */

struct htree_lock {
        int     dummy;
};

struct htree_lock_head {
        int     dummy;
};

#define ldiskfs_htree_lock(lock, head, inode, op)  do { LBUG(); } while (0)
#define ldiskfs_htree_unlock(lock)                 do { LBUG(); } while (0)

static inline struct htree_lock_head *ldiskfs_htree_lock_head_alloc(int dep)
{
        LBUG();
        return NULL;
}

#define ldiskfs_htree_lock_head_free(lh)           do { LBUG(); } while (0)

#define LDISKFS_DUMMY_HTREE_LOCK        0xbabecafe

static inline struct htree_lock *ldiskfs_htree_lock_alloc(void)
{
        return (struct htree_lock *)LDISKFS_DUMMY_HTREE_LOCK;
}

static inline void ldiskfs_htree_lock_free(struct htree_lock *lk)
{
        LASSERT((unsigned long)lk == LDISKFS_DUMMY_HTREE_LOCK);
}

#define HTREE_HBITS_DEF         0

#define osd_ldiskfs_find_entry(dir, dentry, de, lock)   \
        ll_ldiskfs_find_entry(dir, dentry, de)
#define osd_ldiskfs_add_entry(handle, child, cinode, lock) \
        ldiskfs_add_entry(handle, child, cinode)

#endif /* HAVE_LDISKFS_PDO */

/*
 * osd device.
 */
struct osd_device {
        /* super-class */
        struct dt_device          od_dt_dev;
        /* information about underlying file system */
        struct vfsmount          *od_mnt;
        /* object index */
        struct osd_oi           **od_oi_table;
        /* total number of OI containers */
        int                       od_oi_count;
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

        /**
         * The following flag indicates, if it is interop mode or not.
         * It will be initialized, using mount param.
         */
        __u32                     od_iop_mode;

        struct fsfilt_operations *od_fsops;
        int                       od_connects;
        struct lu_site            od_site;

        char                      od_mntdev[128];
        char                      od_svname[128];

        /*
         * mapping for legacy OST objids
         */
        struct osd_compat_objid  *od_ost_map;

        unsigned long long        od_readcache_max_filesize;
        int                       od_read_cache;
        int                       od_writethrough_cache;

        struct brw_stats          od_brw_stats;
        cfs_atomic_t              od_r_in_flight;
        cfs_atomic_t              od_w_in_flight;
};

#define OSD_TRACK_DECLARES
#ifdef OSD_TRACK_DECLARES
#define OSD_DECLARE_OP(oh, op)   {                               \
        LASSERT(oh->ot_handle == NULL);                          \
        ((oh)->ot_declare_ ##op)++; }
#define OSD_EXEC_OP(handle,op)      {                            \
        struct osd_thandle *oh;                                  \
        oh = container_of0(handle, struct osd_thandle, ot_super);\
        if (((oh)->ot_declare_ ##op) > 0) {                      \
                ((oh)->ot_declare_ ##op)--;                      \
        }                                                        \
        }
#else
#define OSD_DECLARE_OP(oh, op)
#define OSD_EXEC_OP(oh, op)
#endif

/* There are at most 10 uid/gids are affected in a transaction, and
 * that's rename case:
 * - 2 for source parent uid & gid;
 * - 2 for source child uid & gid ('..' entry update when the child
 *   is directory);
 * - 2 for target parent uid & gid;
 * - 2 for target child uid & gid (if the target child exists);
 * - 2 for root uid & gid (last_rcvd, llog, etc);
 *
 * The 0 to (OSD_MAX_UGID_CNT - 1) bits of ot_id_type is for indicating
 * the id type of each id in the ot_id_array.
 */
#define OSD_MAX_UGID_CNT        10

struct osd_thandle {
        struct thandle          ot_super;
        handle_t               *ot_handle;
        struct journal_callback ot_jcb;
        cfs_list_t              ot_dcb_list;
        /* Link to the device, for debugging. */
        struct lu_ref_link     *ot_dev_link;
        unsigned short          ot_credits;
        unsigned short          ot_id_cnt;
        unsigned short          ot_id_type;
        uid_t                   ot_id_array[OSD_MAX_UGID_CNT];

#ifdef OSD_TRACK_DECLARES
        unsigned char           ot_declare_attr_set;
        unsigned char           ot_declare_punch;
        unsigned char           ot_declare_xattr_set;
        unsigned char           ot_declare_create;
        unsigned char           ot_declare_destroy;
        unsigned char           ot_declare_ref_add;
        unsigned char           ot_declare_ref_del;
        unsigned char           ot_declare_write;
        unsigned char           ot_declare_insert;
        unsigned char           ot_declare_delete;
#endif

#ifdef OSD_THANDLE_STATS
        /** time when this handle was allocated */
        cfs_time_t oth_alloced;

        /** time when this thanle was started */
        cfs_time_t oth_started;
#endif
};

/**
 * Basic transaction credit op
 */
enum dt_txn_op {
        DTO_INDEX_INSERT,
        DTO_INDEX_DELETE,
        DTO_INDEX_UPDATE,
        DTO_OBJECT_CREATE,
        DTO_OBJECT_DELETE,
        DTO_ATTR_SET_BASE,
        DTO_XATTR_SET,
        DTO_LOG_REC, /**< XXX temporary: dt layer knows nothing about llog. */
        DTO_WRITE_BASE,
        DTO_WRITE_BLOCK,
        DTO_ATTR_SET_CHOWN,

        DTO_NR
};

/*
 * osd dev stats
 */

#ifdef LPROCFS
enum {
        LPROC_OSD_READ_BYTES    = 0,
        LPROC_OSD_WRITE_BYTES   = 1,
        LPROC_OSD_GET_PAGE      = 2,
        LPROC_OSD_NO_PAGE       = 3,
        LPROC_OSD_CACHE_ACCESS  = 4,
        LPROC_OSD_CACHE_HIT     = 5,
        LPROC_OSD_CACHE_MISS    = 6,

#ifdef OSD_THANDLE_STATS
        LPROC_OSD_THANDLE_STARTING,
        LPROC_OSD_THANDLE_OPEN,
        LPROC_OSD_THANDLE_CLOSING,
#endif
        LPROC_OSD_LAST,
};
#endif

/**
 * Storage representation for fids.
 *
 * Variable size, first byte contains the length of the whole record.
 */
struct osd_fid_pack {
        unsigned char fp_len;
        char fp_area[sizeof(struct lu_fid)];
};

struct osd_it_ea_dirent {
        struct lu_fid   oied_fid;
        __u64           oied_ino;
        __u64           oied_off;
        unsigned short  oied_namelen;
        unsigned int    oied_type;
        char            oied_name[0];
} __attribute__((packed));

/**
 * as osd_it_ea_dirent (in memory dirent struct for osd) is greater
 * than lu_dirent struct. osd readdir reads less number of dirent than
 * required for mdd dir page. so buffer size need to be increased so that
 * there  would be one ext3 readdir for every mdd readdir page.
 */

#define OSD_IT_EA_BUFSIZE       (CFS_PAGE_SIZE + CFS_PAGE_SIZE/4)

/**
 * This is iterator's in-memory data structure in interoperability
 * mode (i.e. iterator over ldiskfs style directory)
 */
struct osd_it_ea {
        struct osd_object   *oie_obj;
        /** used in ldiskfs iterator, to stored file pointer */
        struct file          oie_file;
        /** how many entries have been read-cached from storage */
        int                  oie_rd_dirent;
        /** current entry is being iterated by caller */
        int                  oie_it_dirent;
        /** current processing entry */
        struct osd_it_ea_dirent *oie_dirent;
        /** buffer to hold entries, size == OSD_IT_EA_BUFSIZE */
        void                *oie_buf;
};

/**
 * Iterator's in-memory data structure for IAM mode.
 */
struct osd_it_iam {
        struct osd_object     *oi_obj;
        struct iam_path_descr *oi_ipd;
        struct iam_iterator    oi_it;
};

#define MAX_BLOCKS_PER_PAGE (CFS_PAGE_SIZE / 512)

struct osd_iobuf {
        cfs_waitq_t        dr_wait;
        cfs_atomic_t       dr_numreqs;  /* number of reqs being processed */
        int                dr_max_pages;
        int                dr_npages;
        int                dr_error;
        int                dr_frags;
        unsigned int       dr_ignore_quota:1;
        unsigned int       dr_elapsed_valid:1; /* we really did count time */
        unsigned int       dr_rw:1;
        struct page       *dr_pages[PTLRPC_MAX_BRW_PAGES];
        unsigned long      dr_blocks[PTLRPC_MAX_BRW_PAGES*MAX_BLOCKS_PER_PAGE];
        unsigned long      dr_start_time;
        unsigned long      dr_elapsed;  /* how long io took */
        struct osd_device *dr_dev;
};

struct osd_thread_info {
        const struct lu_env   *oti_env;
        /**
         * used for index operations.
         */
        struct dentry          oti_obj_dentry;
        struct dentry          oti_child_dentry;

        /** dentry for Iterator context. */
        struct dentry          oti_it_dentry;
        struct htree_lock     *oti_hlock;

        struct lu_fid          oti_fid;
        struct osd_inode_id    oti_id;
        struct ost_id          oti_ostid;

        /*
         * XXX temporary: for ->i_op calls.
         */
        struct timespec        oti_time;
        /*
         * XXX temporary: fake struct file for osd_object_sync
         */
        struct file            oti_file;
        /*
         * XXX temporary: for capa operations.
         */
        struct lustre_capa_key oti_capa_key;
        struct lustre_capa     oti_capa;

        /** osd_device reference, initialized in osd_trans_start() and
            used in osd_trans_stop() */
        struct osd_device     *oti_dev;

        /**
         * following ipd and it structures are used for osd_index_iam_lookup()
         * these are defined separately as we might do index operation
         * in open iterator session.
         */

        /** osd iterator context used for iterator session */

        union {
                struct osd_it_iam      oti_it;
                /** ldiskfs iterator data structure, see osd_it_ea_{init, fini} */
                struct osd_it_ea       oti_it_ea;
        };

        /** pre-allocated buffer used by oti_it_ea, size OSD_IT_EA_BUFSIZE */
        void                  *oti_it_ea_buf;

        cfs_kstatfs_t          oti_ksfs;

        /** IAM iterator for index operation. */
        struct iam_iterator    oti_idx_it;

        /** union to guarantee that ->oti_ipd[] has proper alignment. */
        union {
                char           oti_it_ipd[DX_IPD_MAX_SIZE];
                long long      oti_alignment_lieutenant;
        };

        union {
                char           oti_idx_ipd[DX_IPD_MAX_SIZE];
                long long      oti_alignment_lieutenant_colonel;
        };


        int                    oti_r_locks;
        int                    oti_w_locks;
        int                    oti_txns;
        /** used in osd_ea_fid_set() to set fid into common ea */
        struct lustre_mdt_attrs oti_mdt_attrs;
        /** 0-copy IO */
        struct osd_iobuf       oti_iobuf;
        struct inode           oti_inode;
        int                    oti_created[PTLRPC_MAX_BRW_PAGES];
#ifdef HAVE_QUOTA_SUPPORT
        struct osd_ctxt        oti_ctxt;
#endif

#define OSD_FID_REC_SZ 32
        char                   oti_ldp[OSD_FID_REC_SZ];
        char                   oti_ldp2[OSD_FID_REC_SZ];

        /* used by quota code */
        union {
                struct if_dqblk  oti_dqblk;
                struct if_dqinfo oti_dqinfo;
        };
};

extern int ldiskfs_pdo;

#ifdef LPROCFS
/* osd_lproc.c */
void lprocfs_osd_init_vars(struct lprocfs_static_vars *lvars);
int osd_procfs_init(struct osd_device *osd, const char *name);
int osd_procfs_fini(struct osd_device *osd);
void osd_brw_stats_update(struct osd_device *osd, struct osd_iobuf *iobuf);

#endif
int osd_statfs(const struct lu_env *env, struct dt_device *dev,
               struct obd_statfs *sfs);
int osd_object_auth(const struct lu_env *env, struct dt_object *dt,
                    struct lustre_capa *capa, __u64 opc);
void osd_declare_qid(struct dt_object *dt, struct osd_thandle *oh,
                     int type, uid_t id, struct inode *inode);
struct inode *osd_iget(struct osd_thread_info *info,
                       struct osd_device *dev,
                       const struct osd_inode_id *id);

int osd_compat_init(struct osd_device *dev);
void osd_compat_fini(struct osd_device *dev);
int osd_compat_objid_lookup(struct osd_thread_info *info,
                            struct osd_device *osd,
                            const struct lu_fid *fid, struct osd_inode_id *id);
int osd_compat_objid_insert(struct osd_thread_info *info,
                            struct osd_device *osd,
                            const struct lu_fid *fid,
                            const struct osd_inode_id *id, struct thandle *th);
int osd_compat_objid_delete(struct osd_thread_info *info,
                            struct osd_device *osd,
                            const struct lu_fid *fid, struct thandle *th);
int osd_compat_spec_lookup(struct osd_thread_info *info,
                           struct osd_device *osd,
                           const struct lu_fid *fid, struct osd_inode_id *id);
int osd_compat_spec_insert(struct osd_thread_info *info,
                           struct osd_device *osd,
                           const struct lu_fid *fid,
                           const struct osd_inode_id *id, struct thandle *th);

extern struct inode *ldiskfs_create_inode(handle_t *handle,
                                          struct inode * dir, int mode);
extern int iam_lvar_create(struct inode *obj, int keysize, int ptrsize,
                           int recsize, handle_t *handle);

extern int iam_lfix_create(struct inode *obj, int keysize, int ptrsize,
                           int recsize, handle_t *handle);
extern int ldiskfs_delete_entry(handle_t *handle,
                                struct inode * dir,
                                struct ldiskfs_dir_entry_2 * de_del,
                                struct buffer_head * bh);

int osd_compat_init(struct osd_device *osd);
void osd_compat_fini(struct osd_device *dev);
int osd_compat_objid_lookup(struct osd_thread_info *info, struct osd_device *osd,
                            const struct lu_fid *fid, struct osd_inode_id *id);
int osd_compat_objid_insert(struct osd_thread_info *info, struct osd_device *osd,
                            const struct lu_fid *fid, const struct osd_inode_id *id,
                            struct thandle *th);
int osd_compat_objid_delete(struct osd_thread_info *info, struct osd_device *osd,
                            const struct lu_fid *fid, struct thandle *th);
int osd_compat_spec_lookup(struct osd_thread_info *info, struct osd_device *osd,
                           const struct lu_fid *fid, struct osd_inode_id *id);
int osd_compat_spec_insert(struct osd_thread_info *info, struct osd_device *osd,
                           const struct lu_fid *fid, const struct osd_inode_id *id,
                           struct thandle *th);
struct inode *osd_iget(struct osd_thread_info *info, struct osd_device *dev,
                       const struct osd_inode_id *id);

void osd_declare_qid(struct dt_object *dt, struct osd_thandle *oh,
                     int type, uid_t id, struct inode *inode);

/*
 * Invariants, assertions.
 */

/*
 * XXX: do not enable this, until invariant checking code is made thread safe
 * in the face of pdirops locking.
 */
#define OSD_INVARIANT_CHECKS (0)

#if OSD_INVARIANT_CHECKS
static inline int osd_invariant(const struct osd_object *obj)
{
        return
                obj != NULL &&
                ergo(obj->oo_inode != NULL,
                     obj->oo_inode->i_sb == osd_sb(osd_obj2dev(obj)) &&
                     atomic_read(&obj->oo_inode->i_count) > 0) &&
                ergo(obj->oo_dir != NULL &&
                     obj->oo_dir->od_conationer.ic_object != NULL,
                     obj->oo_dir->od_conationer.ic_object == obj->oo_inode);
}
#else
#define osd_invariant(obj) (1)
#endif

#define OSD_MAX_CACHE_SIZE OBD_OBJECT_EOF

static inline struct osd_oi *osd_fid2oi(struct osd_device *osd,
                                        const struct lu_fid *fid)
{
        LASSERT(!fid_is_idif(fid));
        LASSERT(!fid_is_igif(fid));
        LASSERT(osd->od_oi_table != NULL && osd->od_oi_count >= 1);
        /* It can work even od_oi_count equals to 1 although it's unexpected,
         * the only reason we set it to 1 is for performance measurement */
        return osd->od_oi_table[fid->f_seq & (osd->od_oi_count - 1)];
}

extern const struct lu_device_operations  osd_lu_ops;

static inline int lu_device_is_osd(const struct lu_device *d)
{
        return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &osd_lu_ops);
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

static inline struct osd_device *osd_obj2dev(const struct osd_object *o)
{
        return osd_dev(o->oo_dt.do_lu.lo_dev);
}

static inline struct super_block *osd_sb(const struct osd_device *dev)
{
        return dev->od_mnt->mnt_sb;
}

static inline int osd_object_is_root(const struct osd_object *obj)
{
        return osd_sb(osd_obj2dev(obj))->s_root->d_inode == obj->oo_inode;
}

static inline struct osd_object *osd_obj(const struct lu_object *o)
{
        LASSERT(lu_device_is_osd(o->lo_dev));
        return container_of0(o, struct osd_object, oo_dt.do_lu);
}

static inline struct osd_object *osd_dt_obj(const struct dt_object *d)
{
        return osd_obj(&d->do_lu);
}

static inline struct lu_device *osd2lu_dev(struct osd_device *osd)
{
        return &osd->od_dt_dev.dd_lu_dev;
}

static inline journal_t *osd_journal(const struct osd_device *dev)
{
        return LDISKFS_SB(osd_sb(dev))->s_journal;
}

extern const struct dt_body_operations osd_body_ops;
extern struct lu_context_key osd_key;

static inline struct osd_thread_info *osd_oti_get(const struct lu_env *env)
{
        return lu_context_key_get(&env->le_ctx, &osd_key);
}

extern const struct dt_body_operations osd_body_ops_new;

/**
 * IAM Iterator
 */
static inline
struct iam_path_descr *osd_it_ipd_get(const struct lu_env *env,
                                      const struct iam_container *bag)
{
        return bag->ic_descr->id_ops->id_ipd_alloc(bag,
                                           osd_oti_get(env)->oti_it_ipd);
}

static inline
struct iam_path_descr *osd_idx_ipd_get(const struct lu_env *env,
                                       const struct iam_container *bag)
{
        return bag->ic_descr->id_ops->id_ipd_alloc(bag,
                                           osd_oti_get(env)->oti_idx_ipd);
}

static inline void osd_ipd_put(const struct lu_env *env,
                               const struct iam_container *bag,
                               struct iam_path_descr *ipd)
{
        bag->ic_descr->id_ops->id_ipd_free(ipd);
}

int osd_ldiskfs_read(struct inode *inode, void *buf, int size, loff_t *offs);

static inline
struct dentry *osd_child_dentry_by_inode(const struct lu_env *env,
                                         struct inode *inode,
                                         const char *name, const int namelen)
{
        struct osd_thread_info *info = osd_oti_get(env);
        struct dentry *child_dentry = &info->oti_child_dentry;
        struct dentry *obj_dentry = &info->oti_obj_dentry;

        obj_dentry->d_inode = inode;
        obj_dentry->d_sb = inode->i_sb;
        obj_dentry->d_name.hash = 0;

        child_dentry->d_name.hash = 0;
        child_dentry->d_parent = obj_dentry;
        child_dentry->d_name.name = name;
        child_dentry->d_name.len = namelen;
        return child_dentry;
}

/**
 * Helper function to pack the fid, ldiskfs stores fid in packed format.
 */
static inline
void osd_fid_pack(struct osd_fid_pack *pack, const struct dt_rec *fid,
                  struct lu_fid *befider)
{
        fid_cpu_to_be(befider, (struct lu_fid *)fid);
        memcpy(pack->fp_area, befider, sizeof(*befider));
        pack->fp_len =  sizeof(*befider) + 1;
}

static inline
int osd_fid_unpack(struct lu_fid *fid, const struct osd_fid_pack *pack)
{
        int result;

        result = 0;
        switch (pack->fp_len) {
        case sizeof *fid + 1:
                memcpy(fid, pack->fp_area, sizeof *fid);
                fid_be_to_cpu(fid, fid);
                break;
        default:
                CERROR("Unexpected packed fid size: %d\n", pack->fp_len);
                result = -EIO;
        }
        return result;
}

/**
 * Quota/Accounting handling
 */
extern const struct dt_index_operations osd_acct_index_ops;
int osd_acct_obj_lookup(struct osd_thread_info *info, struct osd_device *osd,
                        const struct lu_fid *fid, struct osd_inode_id *id);

#endif /* __KERNEL__ */

#ifdef LPROCFS
void osd_quota_procfs_init(struct osd_device *osd);
#endif
#endif /* _OSD_INTERNAL_H */
