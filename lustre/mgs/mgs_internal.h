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
 * Copyright (c) 2011, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef _MGS_INTERNAL_H
#define _MGS_INTERNAL_H

#include <libcfs/libcfs.h>
#include <lustre_log.h>
#include <lustre_export.h>
#include <dt_object.h>

#define MGSSELF_NAME    "_mgs"

#define MGS_SERVICE_WATCHDOG_FACTOR 2

/* -- imperative recovery control data structures -- */
/**
 * restarting targets.
 */
struct mgs_nidtbl;
struct mgs_nidtbl_target {
        cfs_list_t              mnt_list;
        struct mgs_nidtbl      *mnt_fs;
        u64                     mnt_version;
        int                     mnt_type; /* OST or MDT */
        cfs_time_t              mnt_last_active;
        struct mgs_target_info  mnt_mti;
};

enum {
        IR_FULL = 0,
        IR_STARTUP,
        IR_DISABLED,
        IR_PARTIAL
};

#define IR_STRINGS { "full", "startup", "disabled", "partial" }

/**
 */
struct fs_db;

/**
 * maintain fs client nodes of mgs.
 */
struct mgs_fsc {
        struct fs_db      *mfc_fsdb;
        /**
         * Where the fs client comes from.
         */
        struct obd_export *mfc_export;
        /**
         * list of fs clients from the same export,
         * protected by mgs_export_data->med_lock
         */
        cfs_list_t         mfc_export_list;
        /**
         * list of fs clients in the same fsdb, protected by fsdb->fsdb_mutex
         */
        cfs_list_t        mfc_fsdb_list;
        unsigned          mfc_ir_capable:1;
};

struct mgs_nidtbl {
        struct fs_db *mn_fsdb;
        struct file  *mn_version_file;
	struct mutex	mn_lock;
        u64           mn_version;
        int           mn_nr_targets;
        cfs_list_t    mn_targets;
};

struct mgs_tgt_srpc_conf {
        struct mgs_tgt_srpc_conf  *mtsc_next;
        char                      *mtsc_tgt;
        struct sptlrpc_rule_set    mtsc_rset;
};

#define INDEX_MAP_SIZE  8192     /* covers indicies to FFFF */

#define FSDB_LOG_EMPTY          (0)  /* missing client log */
#define FSDB_OLDLOG14           (1)  /* log starts in old (1.4) style */
#define FSDB_REVOKING_LOCK      (2)  /* DLM lock is being revoked */
#define FSDB_MGS_SELF           (3)  /* for '_mgs', used by sptlrpc */
#define FSDB_OSCNAME18          (4)  /* old 1.8 style OSC naming */
#define FSDB_UDESC              (5)  /* sptlrpc user desc, will be obsolete */
#define FSDB_REVOKING_PARAMS	(6)  /* DLM lock is being revoked */

struct fs_db {
        char              fsdb_name[9];
        cfs_list_t        fsdb_list;           /* list of databases */
	struct mutex	  fsdb_mutex;
        void             *fsdb_ost_index_map;  /* bitmap of used indicies */
        void             *fsdb_mdt_index_map;  /* bitmap of used indicies */
        int               fsdb_mdt_count;
        char             *fsdb_clilov;       /* COMPAT_146 client lov name */
        char             *fsdb_clilmv;
        unsigned long     fsdb_flags;
        __u32             fsdb_gen;

        /* in-memory copy of the srpc rules, guarded by fsdb_lock */
        struct sptlrpc_rule_set   fsdb_srpc_gen;
        struct mgs_tgt_srpc_conf *fsdb_srpc_tgt;

        /* list of fs clients, mgs_fsc. protected by mgs_mutex */
        cfs_list_t           fsdb_clients;
        int                  fsdb_nonir_clients;
        int                  fsdb_ir_state;

        /* Target NIDs Table */
        struct mgs_nidtbl    fsdb_nidtbl;

	/* async thread to notify clients */
	struct mgs_device    *fsdb_mgs;
	wait_queue_head_t    fsdb_notify_waitq;
	struct completion    fsdb_notify_comp;
	cfs_time_t           fsdb_notify_start;
	cfs_atomic_t         fsdb_notify_phase;
	volatile unsigned int fsdb_notify_async:1,
                             fsdb_notify_stop:1;
        /* statistic data */
        unsigned int         fsdb_notify_total;
        unsigned int         fsdb_notify_max;
        unsigned int         fsdb_notify_count;
};

struct mgs_device {
	struct dt_device		 mgs_dt_dev;
	struct ptlrpc_service		*mgs_service;
	struct dt_device		*mgs_bottom;
	struct obd_export		*mgs_bottom_exp;
	struct dt_object		*mgs_configs_dir;
	struct dt_object		*mgs_nidtbl_dir;
	cfs_list_t			 mgs_fs_db_list;
	spinlock_t			 mgs_lock; /* covers mgs_fs_db_list */
	cfs_proc_dir_entry_t		*mgs_proc_live;
	cfs_proc_dir_entry_t            *mgs_proc_osd;
	cfs_proc_dir_entry_t            *mgs_proc_fstype;
	cfs_proc_dir_entry_t		*mgs_proc_mntdev;
	cfs_time_t			 mgs_start_time;
	struct obd_device		*mgs_obd;
	struct local_oid_storage	*mgs_los;
	struct mutex			 mgs_mutex;
	struct mutex			 mgs_health_mutex;
	struct lu_target		 mgs_lut;
};

/* this is a top object */
struct mgs_object {
	struct lu_object_header mgo_header;
	struct dt_object        mgo_obj;
	int			mgo_no_attrs;
	int			mgo_reserved;
};

int mgs_init_fsdb_list(struct mgs_device *mgs);
int mgs_cleanup_fsdb_list(struct mgs_device *mgs);
int mgs_params_fsdb_setup(const struct lu_env *env, struct mgs_device *mgs,
			  struct fs_db *fsdb);
int mgs_params_fsdb_cleanup(const struct lu_env *env, struct mgs_device *mgs);
int mgs_find_or_make_fsdb(const struct lu_env *env, struct mgs_device *mgs,
			  char *name, struct fs_db **dbh);
struct fs_db *mgs_find_fsdb(struct mgs_device *mgs, char *fsname);
int mgs_get_fsdb_srpc_from_llog(const struct lu_env *env,
				struct mgs_device *mgs, struct fs_db *fsdb);
int mgs_check_index(const struct lu_env *env, struct mgs_device *mgs,
		    struct mgs_target_info *mti);
int mgs_check_failnid(const struct lu_env *env, struct mgs_device *mgs,
		      struct mgs_target_info *mti);
int mgs_write_log_target(const struct lu_env *env, struct mgs_device *mgs,
			 struct mgs_target_info *mti, struct fs_db *fsdb);
int mgs_replace_nids(const struct lu_env *env, struct mgs_device *mgs,
		     char *devname, char *nids);
int mgs_erase_log(const struct lu_env *env, struct mgs_device *mgs,
		  char *name);
int mgs_erase_logs(const struct lu_env *env, struct mgs_device *mgs,
		   char *fsname);
int mgs_setparam(const struct lu_env *env, struct mgs_device *mgs,
		 struct lustre_cfg *lcfg, char *fsname);
int mgs_list_logs(const struct lu_env *env, struct mgs_device *mgs,
		  struct obd_ioctl_data *data);
int mgs_pool_cmd(const struct lu_env *env, struct mgs_device *mgs,
		 enum lcfg_command_type cmd, char *poolname, char *fsname,
		 char *ostname);

/* mgs_handler.c */
int  mgs_get_lock(struct obd_device *obd, struct ldlm_res_id *res,
                  struct lustre_handle *lockh);
int  mgs_put_lock(struct lustre_handle *lockh);
void mgs_revoke_lock(struct mgs_device *mgs, struct fs_db *fsdb, int type);

/* mgs_nids.c */
int  mgs_ir_update(const struct lu_env *env, struct mgs_device *mgs,
		   struct mgs_target_info *mti);
int mgs_ir_init_fs(const struct lu_env *env, struct mgs_device *mgs,
		   struct fs_db *fsdb);
void mgs_ir_fini_fs(struct mgs_device *mgs, struct fs_db *fsdb);
void mgs_ir_notify_complete(struct fs_db *fsdb);
int  mgs_get_ir_logs(struct ptlrpc_request *req);
int  lprocfs_wr_ir_state(struct file *file, const char *buffer,
                           unsigned long count, void *data);
int  lprocfs_rd_ir_state(struct seq_file *seq, void *data);
int  lprocfs_wr_ir_timeout(struct file *file, const char *buffer,
                           unsigned long count, void *data);
int  lprocfs_rd_ir_timeout(char *page, char **start, off_t off, int count,
                           int *eof, void *data);
void mgs_fsc_cleanup(struct obd_export *exp);
void mgs_fsc_cleanup_by_fsdb(struct fs_db *fsdb);
int  mgs_fsc_attach(const struct lu_env *env, struct obd_export *exp,
		    char *fsname);

/* mgs_fs.c */
int mgs_export_stats_init(struct obd_device *obd, struct obd_export *exp,
                          void *localdata);
int mgs_client_free(struct obd_export *exp);
int mgs_fs_setup(const struct lu_env *env, struct mgs_device *m);
int mgs_fs_cleanup(const struct lu_env *env, struct mgs_device *m);

#ifdef LPROCFS
int lproc_mgs_setup(struct mgs_device *mgs, const char *osd_name);
void lproc_mgs_cleanup(struct mgs_device *mgs);
int lproc_mgs_add_live(struct mgs_device *mgs, struct fs_db *fsdb);
int lproc_mgs_del_live(struct mgs_device *mgs, struct fs_db *fsdb);
void lprocfs_mgs_init_vars(struct lprocfs_static_vars *lvars);
#else
static inline int lproc_mgs_setup(struct mgs_device *mgs, const char *osd_name)
{return 0;}
static inline void lproc_mgs_cleanup(struct mgs_device *mgs)
{}
static inline int lproc_mgs_add_live(struct mgs_device *mgs, struct fs_db *fsdb)
{return 0;}
static inline int lproc_mgs_del_live(struct mgs_device *mgs, struct fs_db *fsdb)
{return 0;}
static void lprocfs_mgs_init_vars(struct lprocfs_static_vars *lvars)
{
        memset(lvars, 0, sizeof(*lvars));
}
#endif

/* mgs/lproc_mgs.c */
enum {
        LPROC_MGS_CONNECT = 0,
        LPROC_MGS_DISCONNECT,
        LPROC_MGS_EXCEPTION,
        LPROC_MGS_TARGET_REG,
        LPROC_MGS_TARGET_DEL,
        LPROC_MGS_LAST
};
void mgs_counter_incr(struct obd_export *exp, int opcode);
void mgs_stats_counter_init(struct lprocfs_stats *stats);

struct temp_comp {
	struct mgs_target_info	*comp_tmti;
	struct mgs_target_info	*comp_mti;
	struct fs_db		*comp_fsdb;
	struct obd_device	*comp_obd;
};

struct mgs_thread_info {
	struct lustre_cfg_bufs	mgi_bufs;
	char			mgi_fsname[MTI_NAME_MAXLEN];
	struct cfg_marker	mgi_marker;
	struct temp_comp	mgi_comp;
};

extern struct lu_context_key mgs_thread_key;

static inline struct mgs_thread_info *mgs_env_info(const struct lu_env *env)
{
	struct mgs_thread_info	*info;
	int			rc;

	info = lu_context_key_get(&env->le_ctx, &mgs_thread_key);
	if (info == NULL) {
		rc = lu_env_refill((struct lu_env *)env);
		if (rc != 0)
			return ERR_PTR(rc);
		info = lu_context_key_get(&env->le_ctx, &mgs_thread_key);
	}
	LASSERT(info != NULL);
	return info;
}

extern const struct lu_device_operations mgs_lu_ops;

static inline int lu_device_is_mgs(struct lu_device *d)
{
	return ergo(d != NULL && d->ld_ops != NULL, d->ld_ops == &mgs_lu_ops);
}

static inline struct mgs_device* lu2mgs_dev(struct lu_device *d)
{
	LASSERT(lu_device_is_mgs(d));
	return container_of0(d, struct mgs_device, mgs_dt_dev.dd_lu_dev);
}

static inline struct mgs_device *exp2mgs_dev(struct obd_export *exp)
{
	return lu2mgs_dev(exp->exp_obd->obd_lu_dev);
}

static inline struct lu_device *mgs2lu_dev(struct mgs_device *d)
{
	return (&d->mgs_dt_dev.dd_lu_dev);
}

static inline struct mgs_device *dt2mgs_dev(struct dt_device *d)
{
	LASSERT(lu_device_is_mgs(&d->dd_lu_dev));
	return container_of0(d, struct mgs_device, mgs_dt_dev);
}

static inline struct mgs_object *lu2mgs_obj(struct lu_object *o)
{
	LASSERT(ergo(o != NULL, lu_device_is_mgs(o->lo_dev)));
	return container_of0(o, struct mgs_object, mgo_obj.do_lu);
}

static inline struct lu_object *mgs2lu_obj(struct mgs_object *obj)
{
	return &obj->mgo_obj.do_lu;
}

static inline struct mgs_object *mgs_obj(const struct lu_object *o)
{
	LASSERT(lu_device_is_mgs(o->lo_dev));
	return container_of0(o, struct mgs_object, mgo_obj.do_lu);
}

static inline struct mgs_object *dt2mgs_obj(const struct dt_object *d)
{
	return mgs_obj(&d->do_lu);
}

static inline struct dt_object* mgs_object_child(struct mgs_object *o)
{
	return container_of0(lu_object_next(mgs2lu_obj(o)),
			     struct dt_object, do_lu);
}

static inline struct dt_object *dt_object_child(struct dt_object *o)
{
	return container_of0(lu_object_next(&(o)->do_lu),
			     struct dt_object, do_lu);
}
struct mgs_direntry {
	cfs_list_t  list;
	char	   *name;
	int	    len;
};

static inline void mgs_direntry_free(struct mgs_direntry *de)
{
	if (de) {
		LASSERT(de->len);
		OBD_FREE(de->name, de->len);
		OBD_FREE_PTR(de);
	}
}

static inline struct mgs_direntry *mgs_direntry_alloc(int len)
{
	struct mgs_direntry *de;

	OBD_ALLOC_PTR(de);
	if (de == NULL)
		return NULL;

	OBD_ALLOC(de->name, len);
	if (de->name == NULL) {
		OBD_FREE_PTR(de);
		return NULL;
	}

	de->len = len;

	return de;
}

/* mgs_llog.c */
int class_dentry_readdir(const struct lu_env *env, struct mgs_device *mgs,
			 cfs_list_t *list);

#endif /* _MGS_INTERNAL_H */
