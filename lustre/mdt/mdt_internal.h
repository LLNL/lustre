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
 * lustre/mdt/mdt_internal.h
 *
 * Lustre Metadata Target (mdt) request handler
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Mike Shaver <shaver@clusterfs.com>
 * Author: Nikita Danilov <nikita@clusterfs.com>
 * Author: Huang Hua <huanghua@clusterfs.com>
 */

#ifndef _MDT_INTERNAL_H
#define _MDT_INTERNAL_H

#if defined(__KERNEL__)

/*
 * struct ptlrpc_client
 */
#include <lustre_net.h>
#include <obd.h>
/*
 * struct obd_connect_data
 * struct lustre_handle
 */
#include <lustre/lustre_idl.h>
#include <lustre_disk.h>
#include <lu_target.h>
#include <md_object.h>
#include <lustre_fid.h>
#include <lustre_fld.h>
#include <lustre_req_layout.h>
#include <lustre_sec.h>
#include <lvfs.h>
#include <lustre_idmap.h>
#include <lustre_eacl.h>
#include <lustre_fsfilt.h>
#include <lustre_quota.h>

/* check if request's xid is equal to last one or not*/
static inline int req_xid_is_last(struct ptlrpc_request *req)
{
        struct lsd_client_data *lcd = req->rq_export->exp_target_data.ted_lcd;
        return (req->rq_xid == lcd->lcd_last_xid ||
                req->rq_xid == lcd->lcd_last_close_xid);
}

struct mdt_object;
/* file data for open files on MDS */
struct mdt_file_data {
        struct portals_handle mfd_handle; /* must be first */
	int		      mfd_mode;   /* open mode provided by client */
        cfs_list_t            mfd_list;   /* protected by med_open_lock */
        __u64                 mfd_xid;    /* xid of the open request */
        struct lustre_handle  mfd_old_handle; /* old handle in replay case */
        struct mdt_object    *mfd_object; /* point to opened object */
};

/* mdt state flag bits */
#define MDT_FL_CFGLOG 0
#define MDT_FL_SYNCED 1

struct mdt_device {
        /* super-class */
        struct md_device           mdt_md_dev;
	struct md_site		   mdt_mite;
        struct ptlrpc_service     *mdt_regular_service;
        struct ptlrpc_service     *mdt_readpage_service;
        struct ptlrpc_service     *mdt_xmds_service;
        struct ptlrpc_service     *mdt_setattr_service;
        struct ptlrpc_service     *mdt_mdsc_service;
        struct ptlrpc_service     *mdt_mdss_service;
        struct ptlrpc_service     *mdt_dtss_service;
        struct ptlrpc_service     *mdt_fld_service;
        /* DLM name-space for meta-data locks maintained by this server */
        struct ldlm_namespace     *mdt_namespace;
        /* ptlrpc handle for MDS->client connections (for lock ASTs). */
        struct ptlrpc_client      *mdt_ldlm_client;
        /* underlying device */
	struct obd_export         *mdt_child_exp;
        struct md_device          *mdt_child;
        struct dt_device          *mdt_bottom;
	struct obd_export	  *mdt_bottom_exp;
        /** target device */
        struct lu_target           mdt_lut;
        /*
         * Options bit-fields.
         */
        struct {
                signed int         mo_user_xattr :1,
                                   mo_acl        :1,
                                   mo_compat_resname:1,
                                   mo_mds_capa   :1,
                                   mo_oss_capa   :1,
                                   mo_cos        :1;
        } mdt_opts;
        /* mdt state flags */
        unsigned long              mdt_state;
        /* lock to protect IOepoch */
	spinlock_t		   mdt_ioepoch_lock;
        __u64                      mdt_ioepoch;

        /* transaction callbacks */
        struct dt_txn_callback     mdt_txn_cb;

        /* these values should be updated from lov if necessary.
         * or should be placed somewhere else. */
        int                        mdt_max_mdsize;
        int                        mdt_max_cookiesize;

        struct upcall_cache        *mdt_identity_cache;

        /* sptlrpc rules */
	rwlock_t		   mdt_sptlrpc_lock;
        struct sptlrpc_rule_set    mdt_sptlrpc_rset;

        /* capability keys */
        unsigned long              mdt_capa_timeout;
        __u32                      mdt_capa_alg;
        struct dt_object          *mdt_ck_obj;
        unsigned long              mdt_ck_timeout;
        unsigned long              mdt_ck_expiry;
        cfs_timer_t                mdt_ck_timer;
        struct ptlrpc_thread       mdt_ck_thread;
        struct lustre_capa_key     mdt_capa_keys[2];
        unsigned int               mdt_capa_conf:1,
                                   mdt_som_conf:1;

	/* statfs optimization: we cache a bit  */
	struct obd_statfs	   mdt_osfs;
	__u64			   mdt_osfs_age;
	spinlock_t		   mdt_osfs_lock;

        /* root squash */
        uid_t                      mdt_squash_uid;
        gid_t                      mdt_squash_gid;
        cfs_list_t                 mdt_nosquash_nids;
        char                      *mdt_nosquash_str;
        int                        mdt_nosquash_strlen;
	struct rw_semaphore	   mdt_squash_sem;

        cfs_proc_dir_entry_t      *mdt_proc_entry;
        struct lprocfs_stats      *mdt_stats;
        int                        mdt_sec_level;
        struct rename_stats        mdt_rename_stats;
	struct lu_fid		   mdt_md_root_fid;

	/* connection to quota master */
	struct obd_export	  *mdt_qmt_exp;
	/* quota master device associated with this MDT */
	struct lu_device	  *mdt_qmt_dev;
};

#define MDT_SERVICE_WATCHDOG_FACTOR     (2)
#define MDT_ROCOMPAT_SUPP       (OBD_ROCOMPAT_LOVOBJID)
#define MDT_INCOMPAT_SUPP       (OBD_INCOMPAT_MDT | OBD_INCOMPAT_COMMON_LR | \
                                 OBD_INCOMPAT_FID | OBD_INCOMPAT_IAM_DIR | \
                                 OBD_INCOMPAT_LMM_VER | OBD_INCOMPAT_MULTI_OI)
#define MDT_COS_DEFAULT         (0)

struct mdt_object {
        struct lu_object_header mot_header;
        struct md_object        mot_obj;
        __u64                   mot_ioepoch;
        __u64                   mot_flags;
        int                     mot_ioepoch_count;
        int                     mot_writecount;
        /* Lock to protect object's IO epoch. */
	struct mutex		mot_ioepoch_mutex;
        /* Lock to protect create_data */
	struct mutex		mot_lov_mutex;
};

enum mdt_object_flags {
        /** SOM attributes are changed. */
        MOF_SOM_CHANGE  = (1 << 0),
        /**
         * The SOM recovery state for mdt object.
         * This state is an in-memory equivalent of an absent SOM EA, used
         * instead of invalidating SOM EA while IOEpoch is still opened when
         * a client eviction occurs or a client fails to obtain SOM attributes.
         * It indicates that the last IOEpoch holder will need to obtain SOM
         * attributes under [0;EOF] extent lock to flush all the client's
         * cached of evicted from MDS clients (but not necessary evicted from
         * OST) before taking ost attributes.
         */
        MOF_SOM_RECOV   = (1 << 1),
        /** File has been just created. */
        MOF_SOM_CREATED = (1 << 2),
        /** lov object has been created. */
        MOF_LOV_CREATED = (1 << 3),
};

struct mdt_lock_handle {
        /* Lock type, reg for cross-ref use or pdo lock. */
        mdl_type_t              mlh_type;

        /* Regular lock */
        struct lustre_handle    mlh_reg_lh;
        ldlm_mode_t             mlh_reg_mode;

        /* Pdirops lock */
        struct lustre_handle    mlh_pdo_lh;
        ldlm_mode_t             mlh_pdo_mode;
        unsigned int            mlh_pdo_hash;
};

enum {
        MDT_LH_PARENT, /* parent lockh */
        MDT_LH_CHILD,  /* child lockh */
        MDT_LH_OLD,    /* old lockh for rename */
        MDT_LH_NEW,    /* new lockh for rename */
        MDT_LH_RMT,    /* used for return lh to caller */
        MDT_LH_NR
};

enum {
        MDT_LOCAL_LOCK,
        MDT_CROSS_LOCK
};

struct mdt_reint_record {
        mdt_reint_t             rr_opcode;
        const struct lustre_handle *rr_handle;
        const struct lu_fid    *rr_fid1;
        const struct lu_fid    *rr_fid2;
        const char             *rr_name;
        int                     rr_namelen;
        const char             *rr_tgt;
        int                     rr_tgtlen;
        const void             *rr_eadata;
        int                     rr_eadatalen;
        int                     rr_logcookielen;
        const struct llog_cookie  *rr_logcookies;
        __u32                   rr_flags;
};

enum mdt_reint_flag {
        MRF_OPEN_TRUNC = 1 << 0,
};

/*
 * Common data shared by mdt-level handlers. This is allocated per-thread to
 * reduce stack consumption.
 */
struct mdt_thread_info {
        /*
         * XXX: Part One:
         * The following members will be filled explicitly
         * with specific data in mdt_thread_info_init().
         */
        /* TODO: move this into mdt_session_key(with LCT_SESSION), because
         * request handling may migrate from one server thread to another.
         */
        struct req_capsule        *mti_pill;

        /* although we have export in req, there are cases when it is not
         * available, e.g. closing files upon export destroy */
        struct obd_export          *mti_exp;
        /*
         * A couple of lock handles.
         */
        struct mdt_lock_handle     mti_lh[MDT_LH_NR];

        struct mdt_device         *mti_mdt;
        const struct lu_env       *mti_env;

        /*
         * Additional fail id that can be set by handler. Passed to
         * target_send_reply().
         */
        int                        mti_fail_id;

        /* transaction number of current request */
        __u64                      mti_transno;


        /*
         * XXX: Part Two:
         * The following members will be filled expilictly
         * with zero in mdt_thread_info_init(). These members may be used
         * by all requests.
         */

        /*
         * Object attributes.
         */
        struct md_attr             mti_attr;
        /*
         * Body for "habeo corpus" operations.
         */
        const struct mdt_body     *mti_body;
        /*
         * Host object. This is released at the end of mdt_handler().
         */
        struct mdt_object         *mti_object;
        /*
         * Lock request for "habeo clavis" operations.
         */
        const struct ldlm_request *mti_dlm_req;

        __u32                      mti_has_trans:1, /* has txn already? */
                                   mti_cross_ref:1;

        /* opdata for mdt_reint_open(), has the same as
         * ldlm_reply:lock_policy_res1.  mdt_update_last_rcvd() stores this
         * value onto disk for recovery when mdt_trans_stop_cb() is called.
         */
        __u64                      mti_opdata;

        /*
         * XXX: Part Three:
         * The following members will be filled explicitly
         * with zero in mdt_reint_unpack(), because they are only used
         * by reint requests (including mdt_reint_open()).
         */

        /*
         * reint record. contains information for reint operations.
         */
        struct mdt_reint_record    mti_rr;

        /** md objects included in operation */
        struct mdt_object         *mti_mos;
        __u64                      mti_ver[PTLRPC_NUM_VERSIONS];
        /*
         * Operation specification (currently create and lookup)
         */
        struct md_op_spec          mti_spec;

        /*
         * XXX: Part Four:
         * The following members will _NOT_ be initialized at all.
         * DO NOT expect them to contain any valid value.
         * They should be initialized explicitly by the user themselves.
         */

         /* XXX: If something is in a union, make sure they do not conflict */

        struct lu_fid              mti_tmp_fid1;
        struct lu_fid              mti_tmp_fid2;
        ldlm_policy_data_t         mti_policy;    /* for mdt_object_lock() and
                                                   * mdt_rename_lock() */
        struct ldlm_res_id         mti_res_id;    /* for mdt_object_lock() and
                                                     mdt_rename_lock()   */
        union {
                struct obd_uuid    uuid[2];       /* for mdt_seq_init_cli()  */
                char               ns_name[48];   /* for mdt_init0()         */
                struct lustre_cfg_bufs bufs;      /* for mdt_stack_fini()    */
		struct obd_statfs  osfs;          /* for mdt_statfs()        */
                struct {
                        /* for mdt_readpage()      */
                        struct lu_rdpg     mti_rdpg;
                        /* for mdt_sendpage()      */
                        struct l_wait_info mti_wait_info;
                } rdpg;
                struct {
                        struct md_attr attr;
                        struct md_som_data data;
                } som;
        } mti_u;

        /* IO epoch related stuff. */
        struct mdt_ioepoch        *mti_ioepoch;
        __u64                      mti_replayepoch;

        loff_t                     mti_off;
        struct lu_buf              mti_buf;
        struct lustre_capa_key     mti_capa_key;

        /* Ops object filename */
        struct lu_name             mti_name;
	/* per-thread values, can be re-used */
	void			  *mti_big_lmm;
	int			   mti_big_lmmsize;
	/* big_lmm buffer was used and must be used in reply */
	int			   mti_big_lmm_used;
	/* should be enough to fit lustre_mdt_attrs */
	char			   mti_xattr_buf[128];
};

static inline const struct md_device_operations *
mdt_child_ops(struct mdt_device * m)
{
        LASSERT(m->mdt_child);
        return m->mdt_child->md_ops;
}

static inline struct md_object *mdt_object_child(struct mdt_object *o)
{
        LASSERT(o);
        return lu2md(lu_object_next(&o->mot_obj.mo_lu));
}

static inline struct ptlrpc_request *mdt_info_req(struct mdt_thread_info *info)
{
         return info->mti_pill ? info->mti_pill->rc_req : NULL;
}

static inline int req_is_replay(struct ptlrpc_request *req)
{
        LASSERT(req->rq_reqmsg);
        return !!(lustre_msg_get_flags(req->rq_reqmsg) & MSG_REPLAY);
}

static inline __u64 mdt_conn_flags(struct mdt_thread_info *info)
{
        LASSERT(info->mti_exp);
        return info->mti_exp->exp_connect_flags;
}

static inline void mdt_object_get(const struct lu_env *env,
                                  struct mdt_object *o)
{
        ENTRY;
        lu_object_get(&o->mot_obj.mo_lu);
        EXIT;
}

static inline void mdt_object_put(const struct lu_env *env,
                                  struct mdt_object *o)
{
        ENTRY;
        lu_object_put(env, &o->mot_obj.mo_lu);
        EXIT;
}

static inline int mdt_object_exists(const struct mdt_object *o)
{
        return lu_object_exists(&o->mot_obj.mo_lu);
}

static inline const struct lu_fid *mdt_object_fid(const struct mdt_object *o)
{
        return lu_object_fid(&o->mot_obj.mo_lu);
}

static inline int mdt_object_obf(const struct mdt_object *o)
{
        return lu_fid_eq(mdt_object_fid(o), &LU_OBF_FID);
}

static inline struct lu_site *mdt_lu_site(const struct mdt_device *mdt)
{
        return mdt->mdt_md_dev.md_lu_dev.ld_site;
}

static inline struct md_site *mdt_md_site(struct mdt_device *mdt)
{
	return &mdt->mdt_mite;
}

static inline void mdt_export_evict(struct obd_export *exp)
{
        class_fail_export(exp);
        class_export_put(exp);
}

int mdt_get_disposition(struct ldlm_reply *rep, int flag);
void mdt_set_disposition(struct mdt_thread_info *info,
                        struct ldlm_reply *rep, int flag);
void mdt_clear_disposition(struct mdt_thread_info *info,
                        struct ldlm_reply *rep, int flag);

void mdt_lock_pdo_init(struct mdt_lock_handle *lh,
                       ldlm_mode_t lm, const char *name,
                       int namelen);

void mdt_lock_reg_init(struct mdt_lock_handle *lh,
                       ldlm_mode_t lm);

int mdt_lock_setup(struct mdt_thread_info *info,
                   struct mdt_object *o,
                   struct mdt_lock_handle *lh);

int mdt_object_lock(struct mdt_thread_info *,
                    struct mdt_object *,
                    struct mdt_lock_handle *,
                    __u64, int);

void mdt_object_unlock(struct mdt_thread_info *,
                       struct mdt_object *,
                       struct mdt_lock_handle *,
                       int decref);

struct mdt_object *mdt_object_new(const struct lu_env *,
				  struct mdt_device *,
				  const struct lu_fid *);
struct mdt_object *mdt_object_find(const struct lu_env *,
                                   struct mdt_device *,
                                   const struct lu_fid *);
struct mdt_object *mdt_object_find_lock(struct mdt_thread_info *,
                                        const struct lu_fid *,
                                        struct mdt_lock_handle *,
                                        __u64);
void mdt_object_unlock_put(struct mdt_thread_info *,
                           struct mdt_object *,
                           struct mdt_lock_handle *,
                           int decref);

void mdt_client_compatibility(struct mdt_thread_info *info);

int mdt_close_unpack(struct mdt_thread_info *info);
int mdt_reint_unpack(struct mdt_thread_info *info, __u32 op);
int mdt_reint_rec(struct mdt_thread_info *, struct mdt_lock_handle *);
void mdt_pack_attr2body(struct mdt_thread_info *info, struct mdt_body *b,
                        const struct lu_attr *attr, const struct lu_fid *fid);

int mdt_getxattr(struct mdt_thread_info *info);
int mdt_reint_setxattr(struct mdt_thread_info *info,
                       struct mdt_lock_handle *lh);

void mdt_lock_handle_init(struct mdt_lock_handle *lh);
void mdt_lock_handle_fini(struct mdt_lock_handle *lh);

void mdt_reconstruct(struct mdt_thread_info *, struct mdt_lock_handle *);
void mdt_reconstruct_generic(struct mdt_thread_info *mti,
                             struct mdt_lock_handle *lhc);

extern void target_recovery_fini(struct obd_device *obd);
extern void target_recovery_init(struct lu_target *lut,
                                 svc_handler_t handler);
int mdt_fs_setup(const struct lu_env *, struct mdt_device *,
                 struct obd_device *, struct lustre_sb_info *lsi);
void mdt_fs_cleanup(const struct lu_env *, struct mdt_device *);

int mdt_export_stats_init(struct obd_device *obd,
                          struct obd_export *exp,
                          void *client_nid);

int mdt_pin(struct mdt_thread_info* info);

int mdt_lock_new_child(struct mdt_thread_info *info,
                       struct mdt_object *o,
                       struct mdt_lock_handle *child_lockh);

void mdt_mfd_set_mode(struct mdt_file_data *mfd,
                      int mode);

int mdt_reint_open(struct mdt_thread_info *info,
                   struct mdt_lock_handle *lhc);

struct mdt_file_data *mdt_handle2mfd(struct mdt_thread_info *,
                                     const struct lustre_handle *);

enum {
        MDT_IOEPOCH_CLOSED  = 0,
        MDT_IOEPOCH_OPENED  = 1,
        MDT_IOEPOCH_GETATTR = 2,
};

enum {
        MDT_SOM_DISABLE = 0,
        MDT_SOM_ENABLE  = 1,
};

int mdt_attr_get_complex(struct mdt_thread_info *info,
			 struct mdt_object *o, struct md_attr *ma);
int mdt_ioepoch_open(struct mdt_thread_info *info, struct mdt_object *o,
                     int created);
int mdt_object_is_som_enabled(struct mdt_object *mo);
int mdt_write_get(struct mdt_object *o);
void mdt_write_put(struct mdt_object *o);
int mdt_write_read(struct mdt_object *o);
struct mdt_file_data *mdt_mfd_new(void);
int mdt_mfd_close(struct mdt_thread_info *info, struct mdt_file_data *mfd);
void mdt_mfd_free(struct mdt_file_data *mfd);
int mdt_close(struct mdt_thread_info *info);
int mdt_attr_set(struct mdt_thread_info *info, struct mdt_object *mo,
                 struct md_attr *ma, int flags);
int mdt_done_writing(struct mdt_thread_info *info);
int mdt_fix_reply(struct mdt_thread_info *info);
int mdt_handle_last_unlink(struct mdt_thread_info *, struct mdt_object *,
                           const struct md_attr *);
void mdt_reconstruct_open(struct mdt_thread_info *, struct mdt_lock_handle *);

struct lu_buf *mdt_buf(const struct lu_env *env, void *area, ssize_t len);
const struct lu_buf *mdt_buf_const(const struct lu_env *env,
                                   const void *area, ssize_t len);

void mdt_dump_lmm(int level, const struct lov_mds_md *lmm);

int mdt_check_ucred(struct mdt_thread_info *);
int mdt_init_ucred(struct mdt_thread_info *, struct mdt_body *);
int mdt_init_ucred_reint(struct mdt_thread_info *);
void mdt_exit_ucred(struct mdt_thread_info *);
int mdt_version_get_check(struct mdt_thread_info *, struct mdt_object *, int);
void mdt_version_get_save(struct mdt_thread_info *, struct mdt_object *, int);
int mdt_version_get_check_save(struct mdt_thread_info *, struct mdt_object *,
                               int);

/* mdt_idmap.c */
int mdt_init_sec_level(struct mdt_thread_info *);
int mdt_init_idmap(struct mdt_thread_info *);
void mdt_cleanup_idmap(struct mdt_export_data *);
int mdt_handle_idmap(struct mdt_thread_info *);
int ptlrpc_user_desc_do_idmap(struct ptlrpc_request *,
                              struct ptlrpc_user_desc *);
void mdt_body_reverse_idmap(struct mdt_thread_info *,
                            struct mdt_body *);
int mdt_remote_perm_reverse_idmap(struct ptlrpc_request *,
                                  struct mdt_remote_perm *);
int mdt_fix_attr_ucred(struct mdt_thread_info *, __u32);

static inline struct mdt_device *mdt_dev(struct lu_device *d)
{
//        LASSERT(lu_device_is_mdt(d));
        return container_of0(d, struct mdt_device, mdt_md_dev.md_lu_dev);
}

static inline struct dt_object *mdt_obj2dt(struct mdt_object *mo)
{
        struct lu_object *lo;
        struct mdt_device *mdt = mdt_dev(mo->mot_obj.mo_lu.lo_dev);

        lo = lu_object_locate(mo->mot_obj.mo_lu.lo_header,
                              mdt->mdt_bottom->dd_lu_dev.ld_type);
        return lu2dt(lo);
}

/* mdt/mdt_identity.c */
#define MDT_IDENTITY_UPCALL_PATH        "/usr/sbin/l_getidentity"

extern struct upcall_cache_ops mdt_identity_upcall_cache_ops;

struct md_identity *mdt_identity_get(struct upcall_cache *, __u32);

void mdt_identity_put(struct upcall_cache *, struct md_identity *);

void mdt_flush_identity(struct upcall_cache *, int);

__u32 mdt_identity_get_perm(struct md_identity *, __u32, lnet_nid_t);

int mdt_pack_remote_perm(struct mdt_thread_info *, struct mdt_object *, void *);

extern struct lu_context_key       mdt_thread_key;
/* debug issues helper starts here*/
static inline int mdt_fail_write(const struct lu_env *env,
                                 struct dt_device *dd, int id)
{
        if (OBD_FAIL_CHECK_ORSET(id, OBD_FAIL_ONCE)) {
                CERROR(LUSTRE_MDT_NAME": cfs_fail_loc=%x, fail write ops\n",
                       id);
                return dd->dd_ops->dt_ro(env, dd);
                /* We set FAIL_ONCE because we never "un-fail" a device */
        }

        return 0;
}

static inline struct mdt_export_data *mdt_req2med(struct ptlrpc_request *req)
{
        return &req->rq_export->exp_mdt_data;
}

typedef void (*mdt_reconstruct_t)(struct mdt_thread_info *mti,
                                  struct mdt_lock_handle *lhc);
static inline int mdt_check_resent(struct mdt_thread_info *info,
                                   mdt_reconstruct_t reconstruct,
                                   struct mdt_lock_handle *lhc)
{
        struct ptlrpc_request *req = mdt_info_req(info);
        ENTRY;

        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT) {
                if (req_xid_is_last(req)) {
                        reconstruct(info, lhc);
                        RETURN(1);
                }
                DEBUG_REQ(D_HA, req, "no reply for RESENT req (have "LPD64")",
                          req->rq_export->exp_target_data.ted_lcd->lcd_last_xid);
        }
        RETURN(0);
}

struct lu_ucred *mdt_ucred(const struct mdt_thread_info *info);
struct lu_ucred *mdt_ucred_check(const struct mdt_thread_info *info);

static inline int is_identity_get_disabled(struct upcall_cache *cache)
{
        return cache ? (strcmp(cache->uc_upcall, "NONE") == 0) : 1;
}

int mdt_blocking_ast(struct ldlm_lock*, struct ldlm_lock_desc*, void*, int);

/* Issues dlm lock on passed @ns, @f stores it lock handle into @lh. */
static inline int mdt_fid_lock(struct ldlm_namespace *ns,
                               struct lustre_handle *lh,
                               ldlm_mode_t mode,
                               ldlm_policy_data_t *policy,
                               const struct ldlm_res_id *res_id,
			       __u64 flags, const __u64 *client_cookie)
{
        int rc;

        LASSERT(ns != NULL);
        LASSERT(lh != NULL);

        rc = ldlm_cli_enqueue_local(ns, res_id, LDLM_IBITS, policy,
                                    mode, &flags, mdt_blocking_ast,
                                    ldlm_completion_ast, NULL, NULL, 0,
				    LVB_T_NONE, client_cookie, lh);
        return rc == ELDLM_OK ? 0 : -EIO;
}

static inline void mdt_fid_unlock(struct lustre_handle *lh,
                                  ldlm_mode_t mode)
{
        ldlm_lock_decref(lh, mode);
}

extern mdl_mode_t mdt_mdl_lock_modes[];
extern ldlm_mode_t mdt_dlm_lock_modes[];

static inline mdl_mode_t mdt_dlm_mode2mdl_mode(ldlm_mode_t mode)
{
        LASSERT(IS_PO2(mode));
        return mdt_mdl_lock_modes[mode];
}

static inline ldlm_mode_t mdt_mdl_mode2dlm_mode(mdl_mode_t mode)
{
        LASSERT(IS_PO2(mode));
        return mdt_dlm_lock_modes[mode];
}

/* mdt_lvb.c */
extern struct ldlm_valblock_ops mdt_lvbo;

static inline struct lu_name *mdt_name(const struct lu_env *env,
                                       char *name, int namelen)
{
        struct lu_name *lname;
        struct mdt_thread_info *mti;

        LASSERT(namelen > 0);
        /* trailing '\0' in buffer */
        LASSERT(name[namelen] == '\0');

        mti = lu_context_key_get(&env->le_ctx, &mdt_thread_key);
        lname = &mti->mti_name;
        lname->ln_name = name;
        lname->ln_namelen = namelen;
        return lname;
}

static inline struct lu_name *mdt_name_copy(struct lu_name *tlname,
                                            struct lu_name *slname)
{
        LASSERT(tlname);
        LASSERT(slname);

        tlname->ln_name = slname->ln_name;
        tlname->ln_namelen = slname->ln_namelen;
        return tlname;
}

void mdt_enable_cos(struct mdt_device *, int);
int mdt_cos_is_enabled(struct mdt_device *);
int mdt_hsm_copytool_send(struct obd_export *exp);

/* lprocfs stuff */
enum {
        LPROC_MDT_OPEN = 0,
        LPROC_MDT_CLOSE,
        LPROC_MDT_MKNOD,
        LPROC_MDT_LINK,
        LPROC_MDT_UNLINK,
        LPROC_MDT_MKDIR,
        LPROC_MDT_RMDIR,
        LPROC_MDT_RENAME,
        LPROC_MDT_GETATTR,
        LPROC_MDT_SETATTR,
        LPROC_MDT_GETXATTR,
        LPROC_MDT_SETXATTR,
        LPROC_MDT_STATFS,
        LPROC_MDT_SYNC,
        LPROC_MDT_SAMEDIR_RENAME,
        LPROC_MDT_CROSSDIR_RENAME,
        LPROC_MDT_LAST,
};
void mdt_counter_incr(struct ptlrpc_request *req, int opcode);
void mdt_stats_counter_init(struct lprocfs_stats *stats);
void lprocfs_mdt_init_vars(struct lprocfs_static_vars *lvars);
int mdt_procfs_init(struct mdt_device *mdt, const char *name);
int mdt_procfs_fini(struct mdt_device *mdt);
void mdt_rename_counter_tally(struct mdt_thread_info *info,
			      struct mdt_device *mdt,
			      struct ptlrpc_request *req,
			      struct mdt_object *src, struct mdt_object *tgt);

void mdt_time_start(const struct mdt_thread_info *info);
void mdt_time_end(const struct mdt_thread_info *info, int idx);

/* Capability */
int mdt_ck_thread_start(struct mdt_device *mdt);
void mdt_ck_thread_stop(struct mdt_device *mdt);
void mdt_ck_timer_callback(unsigned long castmeharder);
int mdt_capa_keys_init(const struct lu_env *env, struct mdt_device *mdt);

static inline void mdt_set_capainfo(struct mdt_thread_info *info, int offset,
                                    const struct lu_fid *fid,
                                    struct lustre_capa *capa)
{
        struct md_capainfo *ci;

	LASSERT(offset >= 0 && offset < MD_CAPAINFO_MAX);
        if (!info->mti_mdt->mdt_opts.mo_mds_capa ||
            !(info->mti_exp->exp_connect_flags & OBD_CONNECT_MDS_CAPA))
                return;

        ci = md_capainfo(info->mti_env);
        LASSERT(ci);
        ci->mc_fid[offset]  = *fid;
        ci->mc_capa[offset] = capa;
}

static inline void mdt_dump_capainfo(struct mdt_thread_info *info)
{
        struct md_capainfo *ci = md_capainfo(info->mti_env);
        int i;

        if (!ci)
                return;
        for (i = 0; i < MD_CAPAINFO_MAX; i++) {
                if (!ci->mc_capa[i]) {
                        CERROR("no capa for index %d "DFID"\n",
                               i, PFID(&ci->mc_fid[i]));
                        continue;
                }
                if (ci->mc_capa[i] == BYPASS_CAPA) {
                        CERROR("bypass for index %d "DFID"\n",
                               i, PFID(&ci->mc_fid[i]));
                        continue;
                }
                DEBUG_CAPA(D_ERROR, ci->mc_capa[i], "index %d", i);
        }
}

static inline struct obd_device *mdt2obd_dev(const struct mdt_device *mdt)
{
        return mdt->mdt_md_dev.md_lu_dev.ld_obd;
}
#endif /* __KERNEL__ */
#endif /* _MDT_H */
