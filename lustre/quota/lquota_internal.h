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
 * version 2 along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2012, Intel Corporation.
 * Use is subject to license terms.
 */

#include <obd.h>
#include <lustre_quota.h>

#ifndef _LQUOTA_INTERNAL_H
#define _LQUOTA_INTERNAL_H

#define QTYPE_NAME(qtype) ((qtype) == USRQUOTA ? "usr" : "grp")
#define RES_NAME(res) ((res) == LQUOTA_RES_MD ? "md" : "dt")

#define QIF_IFLAGS (QIF_INODES | QIF_ITIME | QIF_ILIMITS)
#define QIF_BFLAGS (QIF_SPACE | QIF_BTIME | QIF_BLIMITS)

/* The biggest filename are the one used for slave index which are in the form
 * of 0x%x-%s,glb_fid.f_oid,slv_uuid, that's to say:
 * 2(0x) + 8(f_oid) + 1(-) + 40(UUID_MAX) which means 51 chars + '\0' */
#define LQUOTA_NAME_MAX 52

/* reserved OID in FID_SEQ_QUOTA for local objects */
enum lquota_local_oid {
	LQUOTA_USR_OID		= 1UL, /* slave index copy for user quota */
	LQUOTA_GRP_OID		= 2UL, /* slave index copy for group quota */
	/* all OIDs after this are allocated dynamically by the QMT */
	LQUOTA_GENERATED_OID	= 4096UL,
};

/*
 * lquota_entry support
 */

/* Common operations supported by a lquota_entry */
struct lquota_entry_operations {
	/* Initialize specific fields of a lquota entry */
	void (*lqe_init)(struct lquota_entry *, void *arg);

	/* Read quota settings from disk and update lquota entry */
	int (*lqe_read)(const struct lu_env *, struct lquota_entry *,
			void *arg);

	/* Print debug information about a given lquota entry */
	void (*lqe_debug)(struct lquota_entry *, void *,
			  struct libcfs_debug_msg_data *, const char *,
			  va_list);
};

/* Per-ID information specific to the quota master target */
struct lquota_mst_entry {
	/* global hard limit, in inodes or kbytes */
	__u64			lme_hardlimit;

	/* global quota soft limit, in inodes or kbytes */
	__u64			lme_softlimit;

	/* grace time, in seconds */
	__u64			lme_gracetime;

	/* last time we glimpsed */
	__u64			lme_revoke_time;

	/* r/w semaphore used to protect concurrent access to the quota
	 * parameters which are stored on disk */
	struct rw_semaphore	lme_sem;

	/* quota space that may be released after glimpse */
	__u64			lme_may_rel;
};

/* Per-ID information specific to the quota slave */
struct lquota_slv_entry {
	/* [ib]tune size, inodes or kbytes */
	__u64			lse_qtune;

	/* per-ID lock handle */
	struct lustre_handle	lse_lockh;

	/* pending write which were granted quota space but haven't completed
	 * yet, in inodes or kbytes. */
	__u64			lse_pending_write;

	/* writes waiting for quota space, in inodes or kbytes. */
	__u64			lse_waiting_write;

	/* pending release, in inodes or kbytes */
	__u64			lse_pending_rel;

	/* pending dqacq/dqrel requests. */
	unsigned int		lse_pending_req;

	/* rw spinlock protecting in-memory counters (i.e. lse_pending*) */
	rwlock_t		lse_lock;

	/* waiter for pending request done */
	wait_queue_head_t	lse_waiters;

	/* hint on current on-disk usage, in inodes or kbytes */
	__u64			lse_usage;

	/* time to trigger quota adjust */
	__u64			lse_adjust_time;

	/* return code of latest acquire RPC */
	int			lse_acq_rc;

	/* when latest acquire RPC completed */
	__u64			lse_acq_time;

	/* when latest edquot set */
	__u64			lse_edquot_time;
};

/* In-memory entry for each enforced quota id
 * A lquota_entry structure belong to a single lquota_site */
struct lquota_entry {
	/* link to site hash table */
	cfs_hlist_node_t	 lqe_hash;

	/* quota identifier associated with this entry */
	union lquota_id		 lqe_id;

	/* site this quota entry belongs to */
	struct lquota_site	*lqe_site;

	/* reference counter */
	cfs_atomic_t		 lqe_ref;

	/* linked to list of lqes which:
	 * - need quota space adjustment on slave
	 * - need glimpse to be sent on master */
	cfs_list_t		 lqe_link;

	/* current quota settings/usage of this ID */
	__u64		lqe_granted; /* granted limit, inodes or kbytes */
	__u64		lqe_qunit; /* [ib]unit size, inodes or kbytes */
	union {
		struct	lquota_mst_entry me; /* params specific to QMT */
		struct	lquota_slv_entry se; /* params specific to QSD */
	} u;

	/* flags describing the state of the lquota_entry */
	unsigned long	lqe_enforced:1,/* quota enforced or not */
			lqe_uptodate:1,/* successfully read from disk */
			lqe_edquot:1,  /* id out of quota space on QMT */
			lqe_gl:1,      /* glimpse is in progress */
			lqe_nopreacq:1;/* pre-acquire disabled */
};

/* Compartment within which lquota_entry are unique.
 * lquota_entry structures are kept in a hash table and read from disk if not
 * present.  */
struct lquota_site {
	/* Hash table storing lquota_entry structures */
	cfs_hash_t	*lqs_hash;

	/* Quota type, either user or group. */
	int		 lqs_qtype;

	/* Record whether this site is for a QMT or a slave */
	int		 lqs_is_mst;

	/* Vector of operations which can be done on lquota entry belonging to
	 * this quota site */
	struct lquota_entry_operations	*lqs_ops;

	/* Backpointer to parent structure, either QMT pool info for master or
	 * QSD for slave */
	void		*lqs_parent;
};

#define lqe_hardlimit		u.me.lme_hardlimit
#define lqe_softlimit		u.me.lme_softlimit
#define lqe_gracetime		u.me.lme_gracetime
#define lqe_revoke_time		u.me.lme_revoke_time
#define lqe_sem			u.me.lme_sem
#define lqe_may_rel		u.me.lme_may_rel

#define lqe_qtune		u.se.lse_qtune
#define lqe_pending_write	u.se.lse_pending_write
#define lqe_waiting_write	u.se.lse_waiting_write
#define lqe_pending_rel		u.se.lse_pending_rel
#define lqe_pending_req		u.se.lse_pending_req
#define lqe_waiters		u.se.lse_waiters
#define lqe_lock		u.se.lse_lock
#define lqe_usage		u.se.lse_usage
#define lqe_adjust_time		u.se.lse_adjust_time
#define lqe_lockh		u.se.lse_lockh
#define lqe_acq_rc		u.se.lse_acq_rc
#define lqe_acq_time		u.se.lse_acq_time
#define lqe_edquot_time		u.se.lse_edquot_time

#define LQUOTA_BUMP_VER 0x1
#define LQUOTA_SET_VER  0x2

extern struct kmem_cache *lqe_kmem;

/* helper routine to get/put reference on lquota_entry */
static inline void lqe_getref(struct lquota_entry *lqe)
{
	LASSERT(lqe != NULL);
	cfs_atomic_inc(&lqe->lqe_ref);
}

static inline void lqe_putref(struct lquota_entry *lqe)
{
	LASSERT(lqe != NULL);
	LASSERT(atomic_read(&lqe->lqe_ref) > 0);
	if (atomic_dec_and_test(&lqe->lqe_ref))
		OBD_SLAB_FREE_PTR(lqe, lqe_kmem);
}

static inline int lqe_is_master(struct lquota_entry *lqe)
{
	return lqe->lqe_site->lqs_is_mst;
}

/* lqe locking helpers */
static inline void lqe_write_lock(struct lquota_entry *lqe)
{
	if (lqe_is_master(lqe))
		down_write(&lqe->lqe_sem);
	else
		write_lock(&lqe->lqe_lock);
}

static inline void lqe_write_unlock(struct lquota_entry *lqe)
{
	if (lqe_is_master(lqe))
		up_write(&lqe->lqe_sem);
	else
		write_unlock(&lqe->lqe_lock);
}

static inline void lqe_read_lock(struct lquota_entry *lqe)
{
	if (lqe_is_master(lqe))
		down_read(&lqe->lqe_sem);
	else
		read_lock(&lqe->lqe_lock);
}

static inline void lqe_read_unlock(struct lquota_entry *lqe)
{
	if (lqe_is_master(lqe))
		up_read(&lqe->lqe_sem);
	else
		read_unlock(&lqe->lqe_lock);
}

/*
 * Helper functions & prototypes
 */

/* minimum qunit size, 1K inode for metadata pool and 1MB for data pool */
#define LQUOTA_LEAST_QUNIT(type) \
	(type == LQUOTA_RES_MD ? (1 << 10) : toqb(OFD_MAX_BRW_SIZE))

#define LQUOTA_OVER_FL(type) \
	(type == USRQUOTA ? QUOTA_FL_OVER_USRQUOTA : QUOTA_FL_OVER_GRPQUOTA)

/* Common data shared by quota-level handlers. This is allocated per-thread to
 * reduce stack consumption */
struct lquota_thread_info {
	union  lquota_rec	qti_rec;
	struct lu_buf		qti_lb;
	struct lu_attr		qti_attr;
	struct dt_object_format	qti_dof;
	struct lustre_mdt_attrs	qti_lma;
	struct lu_fid		qti_fid;
	char			qti_buf[LQUOTA_NAME_MAX];
};

#define qti_glb_rec	qti_rec.lqr_glb_rec
#define qti_acct_rec	qti_rec.lqr_acct_rec
#define qti_slv_rec	qti_rec.lqr_slv_rec

#define LQUOTA_BUMP_VER	0x1
#define LQUOTA_SET_VER	0x2

extern struct lu_context_key lquota_thread_key;

/* extract lquota_threa_info context from environment */
static inline
struct lquota_thread_info *lquota_info(const struct lu_env *env)
{
	struct lquota_thread_info	*info;

	info = lu_context_key_get(&env->le_ctx, &lquota_thread_key);
	if (info == NULL) {
		lu_env_refill((struct lu_env *)env);
		info = lu_context_key_get(&env->le_ctx, &lquota_thread_key);
	}
	LASSERT(info);
	return info;
}

#define req_is_acq(flags)    ((flags & QUOTA_DQACQ_FL_ACQ) != 0)
#define req_is_preacq(flags) ((flags & QUOTA_DQACQ_FL_PREACQ) != 0)
#define req_is_rel(flags)    ((flags & QUOTA_DQACQ_FL_REL) != 0)
#define req_has_rep(flags)   ((flags & QUOTA_DQACQ_FL_REPORT) != 0)

/* debugging macros */
#ifdef LIBCFS_DEBUG
#define lquota_lqe_debug(msgdata, mask, cdls, lqe, fmt, a...) do {      \
	CFS_CHECK_STACK(msgdata, mask, cdls);                           \
                                                                        \
	if (((mask) & D_CANTMASK) != 0 ||                               \
	    ((libcfs_debug & (mask)) != 0 &&                            \
	     (libcfs_subsystem_debug & DEBUG_SUBSYSTEM) != 0))          \
		lquota_lqe_debug0(lqe, msgdata, fmt, ##a);              \
} while(0)

void lquota_lqe_debug0(struct lquota_entry *lqe,
		       struct libcfs_debug_msg_data *data, const char *fmt, ...)
	__attribute__ ((format (printf, 3, 4)));

#define LQUOTA_DEBUG_LIMIT(mask, lqe, fmt, a...) do {                          \
	static cfs_debug_limit_state_t _lquota_cdls;                           \
	LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, mask, &_lquota_cdls);              \
	lquota_lqe_debug(&msgdata, mask, &_lquota_cdls, lqe, "$$$ "fmt" ",     \
			 ##a);                                                 \
} while (0)

#define LQUOTA_ERROR(lqe, fmt, a...) LQUOTA_DEBUG_LIMIT(D_ERROR, lqe, fmt, ## a)
#define LQUOTA_WARN(lqe, fmt, a...) \
	LQUOTA_DEBUG_LIMIT(D_WARNING, lqe, fmt, ## a)
#define LQUOTA_CONSOLE(lqe, fmt, a...) \
	LQUOTA_DEBUG_LIMIT(D_CONSOLE, lqe, fmt, ## a)

#define LQUOTA_DEBUG(lock, fmt, a...) do {                                 \
	LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, D_QUOTA, NULL);                \
	lquota_lqe_debug(&msgdata, D_QUOTA, NULL, lqe, "$$$ "fmt" ", ##a); \
} while (0)
#else /* !LIBCFS_DEBUG */
# define LQUOTA_DEBUG(lqe, fmt, a...) ((void)0)
# define LQUOTA_ERROR(lqe, fmt, a...) ((void)0)
# define LQUOTA_WARN(lqe, fmt, a...) ((void)0)
# define LQUOTA_CONSOLE(lqe, fmt, a...) ((void)0)
# define lquota_lqe_debug(cdls, level, lqe, file, func, line, fmt, a...) \
		((void)0)
#endif

/* lquota_lib.c */
struct dt_object *acct_obj_lookup(const struct lu_env *, struct dt_device *,
				  int);
void lquota_generate_fid(struct lu_fid *, int, int, int);
int lquota_extract_fid(const struct lu_fid *, int *, int *, int *);
const struct dt_index_features *glb_idx_feature(struct lu_fid *);

/* lquota_entry.c */
/* site create/destroy */
struct lquota_site *lquota_site_alloc(const struct lu_env *, void *, bool,
				      short, struct lquota_entry_operations *);
void lquota_site_free(const struct lu_env *, struct lquota_site *);
/* quota entry operations */
struct lquota_entry *lqe_locate(const struct lu_env *, struct lquota_site *,
				union lquota_id *);

/* lquota_disk.c */
struct dt_object *lquota_disk_dir_find_create(const struct lu_env *,
					      struct dt_device *,
					      struct dt_object *, const char *);
struct dt_object *lquota_disk_glb_find_create(const struct lu_env *,
					      struct dt_device *,
					      struct dt_object *,
					      struct lu_fid *, bool);
struct dt_object *lquota_disk_slv_find_create(const struct lu_env *,
					      struct dt_device *,
					      struct dt_object *,
					      struct lu_fid *,
					      struct obd_uuid *, bool);
typedef int (*lquota_disk_slv_cb_t) (const struct lu_env *, struct lu_fid *,
				     char *, struct lu_fid *, void *);
int lquota_disk_for_each_slv(const struct lu_env *, struct dt_object *,
			     struct lu_fid *, lquota_disk_slv_cb_t, void *);
struct dt_object *lquota_disk_slv_find(const struct lu_env *,
				       struct dt_device *, struct dt_object *,
				       const struct lu_fid *,
				       struct obd_uuid *);
int lquota_disk_read(const struct lu_env *, struct dt_object *,
		     union lquota_id *, struct dt_rec *);
int lquota_disk_declare_write(const struct lu_env *, struct thandle *,
			      struct dt_object *, union lquota_id *);
int lquota_disk_write(const struct lu_env *, struct thandle *,
		      struct dt_object *, union lquota_id *, struct dt_rec *,
		      __u32, __u64 *);
int lquota_disk_update_ver(const struct lu_env *, struct dt_device *,
			   struct dt_object *, __u64);

/* qmt_dev.c */
int qmt_glb_init(void);
void qmt_glb_fini(void);

/* lproc_quota.c */
extern struct file_operations lprocfs_quota_seq_fops;

/* qsd_lib.c */
int qsd_glb_init(void);
void qsd_glb_fini(void);
#endif /* _LQUOTA_INTERNAL_H */
