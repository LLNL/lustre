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
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2010, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

/** \defgroup LDLM Lustre Distributed Lock Manager
 *
 * Lustre DLM is based on VAX DLM.
 * Its two main roles are:
 *   - To provide locking assuring consistency of data on all Lustre nodes.
 *   - To allow clients to cache state protected by a lock by holding the
 *     lock until a conflicting lock is requested or it is expired by the LRU.
 *
 * @{
 */

#ifndef _LUSTRE_DLM_H__
#define _LUSTRE_DLM_H__

#include <lustre_lib.h>
#include <lustre_net.h>
#include <lustre_import.h>
#include <lustre_handles.h>
#include <interval_tree.h> /* for interval_node{}, ldlm_extent */
#include <lu_ref.h>

#include "lustre_dlm_flags.h"

struct obd_ops;
struct obd_device;

extern struct kset *ldlm_ns_kset;
extern struct kset *ldlm_svc_kset;

#define OBD_LDLM_DEVICENAME  "ldlm"

#define LDLM_DEFAULT_LRU_SIZE (100 * num_online_cpus())
#define LDLM_DEFAULT_MAX_ALIVE		3900	/* 3900 seconds ~65 min */
#define LDLM_CTIME_AGE_LIMIT (10)
/* if client lock is unused for that time it can be cancelled if any other
 * client shows interest in that lock, e.g. glimpse is occured. */
#define LDLM_DIRTY_AGE_LIMIT (10)
#define LDLM_DEFAULT_PARALLEL_AST_LIMIT 1024

/**
 * LDLM non-error return states
 */
enum ldlm_error {
	ELDLM_OK		= 0,
	ELDLM_LOCK_MATCHED	= 1,

	ELDLM_LOCK_CHANGED	= 300,
	ELDLM_LOCK_ABORTED	= 301,
	ELDLM_LOCK_REPLACED	= 302,
	ELDLM_NO_LOCK_DATA	= 303,
	ELDLM_LOCK_WOULDBLOCK	= 304,

	ELDLM_NAMESPACE_EXISTS	= 400,
	ELDLM_BAD_NAMESPACE	= 401,
};

/**
 * LDLM namespace type.
 * The "client" type is actually an indication that this is a narrow local view
 * into complete namespace on the server. Such namespaces cannot make any
 * decisions about lack of conflicts or do any autonomous lock granting without
 * first speaking to a server.
 */
enum ldlm_side {
	LDLM_NAMESPACE_SERVER = 0x01,
	LDLM_NAMESPACE_CLIENT = 0x02
};

/**
 * The blocking callback is overloaded to perform two functions.  These flags
 * indicate which operation should be performed.
 */
#define LDLM_CB_BLOCKING    1
#define LDLM_CB_CANCELING   2

/**
 * \name Lock Compatibility Matrix.
 *
 * A lock has both a type (extent, flock, inode bits, or plain) and a mode.
 * Lock types are described in their respective implementation files:
 * ldlm_{extent,flock,inodebits,plain}.c.
 *
 * There are six lock modes along with a compatibility matrix to indicate if
 * two locks are compatible.
 *
 * - EX: Exclusive mode. Before a new file is created, MDS requests EX lock
 *   on the parent.
 * - PW: Protective Write (normal write) mode. When a client requests a write
 *   lock from an OST, a lock with PW mode will be issued.
 * - PR: Protective Read (normal read) mode. When a client requests a read from
 *   an OST, a lock with PR mode will be issued. Also, if the client opens a
 *   file for execution, it is granted a lock with PR mode.
 * - CW: Concurrent Write mode. The type of lock that the MDS grants if a client
 *   requests a write lock during a file open operation.
 * - CR Concurrent Read mode. When a client performs a path lookup, MDS grants
 *   an inodebit lock with the CR mode on the intermediate path component.
 * - NL Null mode.
 *
 * <PRE>
 *       NL  CR  CW  PR  PW  EX
 *  NL    1   1   1   1   1   1
 *  CR    1   1   1   1   1   0
 *  CW    1   1   1   0   0   0
 *  PR    1   1   0   1   0   0
 *  PW    1   1   0   0   0   0
 *  EX    1   0   0   0   0   0
 * </PRE>
 */
/** @{ */
#define LCK_COMPAT_EX  LCK_NL
#define LCK_COMPAT_PW  (LCK_COMPAT_EX | LCK_CR)
#define LCK_COMPAT_PR  (LCK_COMPAT_PW | LCK_PR)
#define LCK_COMPAT_CW  (LCK_COMPAT_PW | LCK_CW)
#define LCK_COMPAT_CR  (LCK_COMPAT_CW | LCK_PR | LCK_PW)
#define LCK_COMPAT_NL  (LCK_COMPAT_CR | LCK_EX | LCK_GROUP)
#define LCK_COMPAT_GROUP  (LCK_GROUP | LCK_NL)
#define LCK_COMPAT_COS (LCK_COS)
/** @} Lock Compatibility Matrix */

extern enum ldlm_mode lck_compat_array[];

static inline void lockmode_verify(enum ldlm_mode mode)
{
	LASSERT(mode > LCK_MINMODE && mode < LCK_MAXMODE);
}

static inline int lockmode_compat(enum ldlm_mode exist_mode,
				  enum ldlm_mode new_mode)
{
	return lck_compat_array[exist_mode] & new_mode;
}

/*
 *
 * cluster name spaces
 *
 */

#define DLM_OST_NAMESPACE 1
#define DLM_MDS_NAMESPACE 2

/* XXX
   - do we just separate this by security domains and use a prefix for
     multiple namespaces in the same domain?
   -
*/

/**
 * Locking rules for LDLM:
 *
 * lr_lock
 *
 * lr_lock
 *     waiting_locks_spinlock
 *
 * lr_lock
 *     led_lock
 *
 * lr_lock
 *     ns_lock
 *
 * lr_lvb_mutex
 *     lr_lock
 *
 */

struct ldlm_pool;
struct ldlm_lock;
struct ldlm_resource;
struct ldlm_namespace;

/**
 * Operations on LDLM pools.
 * LDLM pool is a pool of locks in the namespace without any implicitly
 * specified limits.
 * Locks in the pool are organized in LRU.
 * Local memory pressure or server instructions (e.g. mempressure on server)
 * can trigger freeing of locks from the pool
 */
struct ldlm_pool_ops {
	/** Recalculate pool \a pl usage */
	int (*po_recalc)(struct ldlm_pool *pl);
	/** Cancel at least \a nr locks from pool \a pl */
	int (*po_shrink)(struct ldlm_pool *pl, int nr, gfp_t gfp_mask);
	int (*po_setup)(struct ldlm_pool *pl, int limit);
};

/** One second for pools thread check interval. Each pool has own period. */
#define LDLM_POOLS_THREAD_PERIOD (1)

/** ~6% margin for modest pools. See ldlm_pool.c for details. */
#define LDLM_POOLS_MODEST_MARGIN_SHIFT (4)

/** Default recalc period for server side pools in sec. */
#define LDLM_POOL_SRV_DEF_RECALC_PERIOD (1)

/** Default recalc period for client side pools in sec. */
#define LDLM_POOL_CLI_DEF_RECALC_PERIOD (10)

/**
 * LDLM pool structure to track granted locks.
 * For purposes of determining when to release locks on e.g. memory pressure.
 * This feature is commonly referred to as lru_resize.
 */
struct ldlm_pool {
	/** Pool debugfs directory. */
	struct dentry		*pl_debugfs_entry;
	/** Pool name, must be long enough to hold compound proc entry name. */
	char			pl_name[100];
	/** Lock for protecting SLV/CLV updates. */
	spinlock_t		pl_lock;
	/** Number of allowed locks in in pool, both, client and server side. */
	atomic_t		pl_limit;
	/** Number of granted locks in */
	atomic_t		pl_granted;
	/** Grant rate per T. */
	atomic_t		pl_grant_rate;
	/** Cancel rate per T. */
	atomic_t		pl_cancel_rate;
	/** Server lock volume (SLV). Protected by pl_lock. */
	__u64			pl_server_lock_volume;
	/** Current biggest client lock volume. Protected by pl_lock. */
	__u64			pl_client_lock_volume;
	/** Lock volume factor. SLV on client is calculated as following:
	 *  server_slv * lock_volume_factor. */
	atomic_t		pl_lock_volume_factor;
	/** Time when last SLV from server was obtained. */
	time64_t		pl_recalc_time;
	/** Recalculation period for pool. */
	time64_t		pl_recalc_period;
	/** Recalculation and shrink operations. */
	struct ldlm_pool_ops	*pl_ops;
	/** Number of planned locks for next period. */
	int			pl_grant_plan;
	/** Pool statistics. */
	struct lprocfs_stats	*pl_stats;

	/* sysfs object */
	struct kobject		 pl_kobj;
	struct completion	 pl_kobj_unregister;
};

typedef int (*ldlm_res_policy)(const struct lu_env *env,
			       struct ldlm_namespace *,
			       struct ldlm_lock **, void *req_cookie,
			       enum ldlm_mode mode, __u64 flags, void *data);

typedef int (*ldlm_cancel_cbt)(struct ldlm_lock *lock);

/**
 * LVB operations.
 * LVB is Lock Value Block. This is a special opaque (to LDLM) value that could
 * be associated with an LDLM lock and transferred from client to server and
 * back.
 *
 * Currently LVBs are used by:
 *  - OSC-OST code to maintain current object size/times
 *  - layout lock code to return the layout when the layout lock is granted
 *
 * To ensure delayed LVB initialization, it is highly recommended to use the set
 * of ldlm_[res_]lvbo_[init,update,fill]() functions.
 */
struct ldlm_valblock_ops {
	int (*lvbo_init)(struct ldlm_resource *res);
	int (*lvbo_update)(struct ldlm_resource *res, struct ldlm_lock *lock,
			   struct ptlrpc_request *r, int increase);
	int (*lvbo_free)(struct ldlm_resource *res);
	/* Return size of lvb data appropriate RPC size can be reserved */
	int (*lvbo_size)(struct ldlm_lock *lock);
	/* Called to fill in lvb data to RPC buffer @buf */
	int (*lvbo_fill)(struct ldlm_lock *lock, void *buf, int *buflen);
};

/**
 * LDLM pools related, type of lock pool in the namespace.
 * Greedy means release cached locks aggressively
 */
enum ldlm_appetite {
	LDLM_NAMESPACE_GREEDY = 1 << 0,
	LDLM_NAMESPACE_MODEST = 1 << 1
};

/**
 * Default values for the "max_nolock_size", "contention_time" and
 * "contended_locks" namespace tunables.
 */
#define NS_DEFAULT_MAX_NOLOCK_BYTES 0
#define NS_DEFAULT_CONTENTION_SECONDS 2
#define NS_DEFAULT_CONTENDED_LOCKS 32

struct ldlm_ns_bucket {
	/** back pointer to namespace */
	struct ldlm_namespace      *nsb_namespace;
	/**
	 * Estimated lock callback time.  Used by adaptive timeout code to
	 * avoid spurious client evictions due to unresponsiveness when in
	 * fact the network or overall system load is at fault
	 */
	struct adaptive_timeout     nsb_at_estimate;
	/**
	 * Which res in the bucket should we start with the reclaim.
	 */
	int			    nsb_reclaim_start;
};

enum {
	/** LDLM namespace lock stats */
	LDLM_NSS_LOCKS          = 0,
	LDLM_NSS_LAST
};

enum ldlm_ns_type {
	LDLM_NS_TYPE_UNKNOWN = 0,	/**< invalid type */
	LDLM_NS_TYPE_MDC,		/**< MDC namespace */
	LDLM_NS_TYPE_MDT,		/**< MDT namespace */
	LDLM_NS_TYPE_OSC,		/**< OSC namespace */
	LDLM_NS_TYPE_OST,		/**< OST namespace */
	LDLM_NS_TYPE_MGC,		/**< MGC namespace */
	LDLM_NS_TYPE_MGT,		/**< MGT namespace */
};

/**
 * LDLM Namespace.
 *
 * Namespace serves to contain locks related to a particular service.
 * There are two kinds of namespaces:
 * - Server namespace has knowledge of all locks and is therefore authoritative
 *   to make decisions like what locks could be granted and what conflicts
 *   exist during new lock enqueue.
 * - Client namespace only has limited knowledge about locks in the namespace,
 *   only seeing locks held by the client.
 *
 * Every Lustre service has one server namespace present on the server serving
 * that service. Every client connected to the service has a client namespace
 * for it.
 * Every lock obtained by client in that namespace is actually represented by
 * two in-memory locks. One on the server and one on the client. The locks are
 * linked by a special cookie by which one node can tell to the other which lock
 * it actually means during communications. Such locks are called remote locks.
 * The locks held by server only without any reference to a client are called
 * local locks.
 */
struct ldlm_namespace {
	/** Backward link to OBD, required for LDLM pool to store new SLV. */
	struct obd_device	*ns_obd;

	/** Flag indicating if namespace is on client instead of server */
	enum ldlm_side		ns_client;

	/** name of this namespace */
	char			*ns_name;

	/** Resource hash table for namespace. */
	struct cfs_hash		*ns_rs_hash;

	/** serialize */
	spinlock_t		ns_lock;

	/** big refcount (by bucket) */
	atomic_t		ns_bref;

	/**
	 * Namespace connect flags supported by server (may be changed via
	 * /proc, LRU resize may be disabled/enabled).
	 */
	__u64			ns_connect_flags;

	/** Client side original connect flags supported by server. */
	__u64			ns_orig_connect_flags;

	/* namespace debugfs dir entry */
	struct dentry		*ns_debugfs_entry;

	/**
	 * Position in global namespace list linking all namespaces on
	 * the node.
	 */
	struct list_head	ns_list_chain;

	/**
	 * List of unused locks for this namespace. This list is also called
	 * LRU lock list.
	 * Unused locks are locks with zero reader/writer reference counts.
	 * This list is only used on clients for lock caching purposes.
	 * When we want to release some locks voluntarily or if server wants
	 * us to release some locks due to e.g. memory pressure, we take locks
	 * to release from the head of this list.
	 * Locks are linked via l_lru field in \see struct ldlm_lock.
	 */
	struct list_head	ns_unused_list;
	/** Number of locks in the LRU list above */
	int			ns_nr_unused;
	struct list_head	*ns_last_pos;

	/**
	 * Maximum number of locks permitted in the LRU. If 0, means locks
	 * are managed by pools and there is no preset limit, rather it is all
	 * controlled by available memory on this client and on server.
	 */
	unsigned int		ns_max_unused;

	/** Maximum allowed age (last used time) for locks in the LRU */
	ktime_t			ns_max_age;

	/**
	 * Server only: number of times we evicted clients due to lack of reply
	 * to ASTs.
	 */
	unsigned int		ns_timeouts;
	/**
	 * Number of seconds since the file change time after which the
	 * MDT will return an UPDATE lock along with a LOOKUP lock.
	 * This allows the client to start caching negative dentries
	 * for a directory and may save an RPC for a later stat.
	 */
	time64_t		ns_ctime_age_limit;
	/**
	 * Number of seconds since the lock was last used. The client may
	 * cancel the lock limited by this age and flush related data if
	 * any other client shows interest in it doing glimpse request.
	 * This allows to cache stat data locally for such files early.
	 */
	time64_t		ns_dirty_age_limit;
	/**
	 * Used to rate-limit ldlm_namespace_dump calls.
	 * \see ldlm_namespace_dump. Increased by 10 seconds every time
	 * it is called.
	 */
	time64_t		ns_next_dump;

	/** "policy" function that does actual lock conflict determination */
	ldlm_res_policy		ns_policy;

	/**
	 * LVB operations for this namespace.
	 * \see struct ldlm_valblock_ops
	 */
	struct ldlm_valblock_ops *ns_lvbo;

	/**
	 * Used by filter code to store pointer to OBD of the service.
	 * Should be dropped in favor of \a ns_obd
	 */
	void			*ns_lvbp;

	/**
	 * Wait queue used by __ldlm_namespace_free. Gets woken up every time
	 * a resource is removed.
	 */
	wait_queue_head_t	ns_waitq;
	/** LDLM pool structure for this namespace */
	struct ldlm_pool	ns_pool;
	/** Definition of how eagerly unused locks will be released from LRU */
	enum ldlm_appetite	ns_appetite;

	/**
	 * If more than \a ns_contended_locks are found, the resource is
	 * considered to be contended. Lock enqueues might specify that no
	 * contended locks should be granted
	 */
	unsigned		ns_contended_locks;

	/**
	 * The resources in this namespace remember contended state during
	 * \a ns_contention_time, in seconds.
	 */
	time64_t		ns_contention_time;

	/**
	 * Limit size of contended extent locks, in bytes.
	 * If extended lock is requested for more then this many bytes and
	 * caller instructs us not to grant contended locks, we would disregard
	 * such a request.
	 */
	unsigned		ns_max_nolock_size;

	/** Limit of parallel AST RPC count. */
	unsigned		ns_max_parallel_ast;

	/**
	 * Callback to check if a lock is good to be canceled by ELC or
	 * during recovery.
	 */
	ldlm_cancel_cbt		ns_cancel;

	/** LDLM lock stats */
	struct lprocfs_stats	*ns_stats;

	/**
	 * Flag to indicate namespace is being freed. Used to determine if
	 * recalculation of LDLM pool statistics should be skipped.
	 */
	unsigned		ns_stopping:1;

	/**
	 * Which bucket should we start with the lock reclaim.
	 */
	int			ns_reclaim_start;

	struct kobject		ns_kobj; /* sysfs object */
	struct completion	ns_kobj_unregister;
};

/**
 * Returns 1 if namespace \a ns is a client namespace.
 */
static inline int ns_is_client(struct ldlm_namespace *ns)
{
        LASSERT(ns != NULL);
        LASSERT(ns->ns_client == LDLM_NAMESPACE_CLIENT ||
                ns->ns_client == LDLM_NAMESPACE_SERVER);
        return ns->ns_client == LDLM_NAMESPACE_CLIENT;
}

/**
 * Returns 1 if namespace \a ns is a server namespace.
 */
static inline int ns_is_server(struct ldlm_namespace *ns)
{
        LASSERT(ns != NULL);
        LASSERT(ns->ns_client == LDLM_NAMESPACE_CLIENT ||
                ns->ns_client == LDLM_NAMESPACE_SERVER);
        return ns->ns_client == LDLM_NAMESPACE_SERVER;
}

/**
 * Returns 1 if namespace \a ns supports early lock cancel (ELC).
 */
static inline int ns_connect_cancelset(struct ldlm_namespace *ns)
{
	LASSERT(ns != NULL);
	return !!(ns->ns_connect_flags & OBD_CONNECT_CANCELSET);
}

/**
 * Returns 1 if this namespace supports lru_resize.
 */
static inline int ns_connect_lru_resize(struct ldlm_namespace *ns)
{
        LASSERT(ns != NULL);
        return !!(ns->ns_connect_flags & OBD_CONNECT_LRU_RESIZE);
}

static inline void ns_register_cancel(struct ldlm_namespace *ns,
				      ldlm_cancel_cbt arg)
{
	LASSERT(ns != NULL);
	ns->ns_cancel = arg;
}

struct ldlm_lock;

/** Type for blocking callback function of a lock. */
typedef int (*ldlm_blocking_callback)(struct ldlm_lock *lock,
				      struct ldlm_lock_desc *new, void *data,
				      int flag);
/** Type for completion callback function of a lock. */
typedef int (*ldlm_completion_callback)(struct ldlm_lock *lock, __u64 flags,
					void *data);
/** Type for glimpse callback function of a lock. */
typedef int (*ldlm_glimpse_callback)(struct ldlm_lock *lock, void *data);

/** Work list for sending GL ASTs to multiple locks. */
struct ldlm_glimpse_work {
	struct ldlm_lock	*gl_lock; /* lock to glimpse */
	struct list_head	 gl_list; /* linkage to other gl work structs */
	__u32			 gl_flags;/* see LDLM_GL_WORK_* below */
	union ldlm_gl_desc	*gl_desc; /* glimpse descriptor to be packed in
					   * glimpse callback request */
	ptlrpc_interpterer_t	 gl_interpret_reply;
	void			*gl_interpret_data;
};

struct ldlm_bl_desc {
	unsigned int bl_same_client:1,
		     bl_cos_incompat:1;
};

struct ldlm_cb_set_arg {
	struct ptlrpc_request_set	*set;
	int				 type; /* LDLM_{CP,BL,GL}_CALLBACK */
	atomic_t			 restart;
	struct list_head		*list;
	union ldlm_gl_desc		*gl_desc; /* glimpse AST descriptor */
	ptlrpc_interpterer_t		 gl_interpret_reply;
	void				*gl_interpret_data;
	struct ldlm_bl_desc		*bl_desc;
};

struct ldlm_cb_async_args {
	struct ldlm_cb_set_arg	*ca_set_arg;
	struct ldlm_lock	*ca_lock;
};

/** The ldlm_glimpse_work was slab allocated & must be freed accordingly.*/
#define LDLM_GL_WORK_SLAB_ALLOCATED 0x1

/** Interval node data for each LDLM_EXTENT lock. */
struct ldlm_interval {
	struct interval_node	li_node;  /* node for tree management */
	struct list_head	li_group; /* the locks which have the same
					   * policy - group of the policy */
};
#define to_ldlm_interval(n) container_of(n, struct ldlm_interval, li_node)

/**
 * Interval tree for extent locks.
 * The interval tree must be accessed under the resource lock.
 * Interval trees are used for granted extent locks to speed up conflicts
 * lookup. See ldlm/interval_tree.c for more details.
 */
struct ldlm_interval_tree {
	/** Tree size. */
	int			lit_size;
	enum ldlm_mode		lit_mode;  /* lock mode */
	struct interval_node	*lit_root; /* actual ldlm_interval */
};

/**
 * Lists of waiting locks for each inodebit type.
 * A lock can be in several liq_waiting lists and it remains in lr_waiting.
 */
struct ldlm_ibits_queues {
	struct list_head	liq_waiting[MDS_INODELOCK_NUMBITS];
};

struct ldlm_ibits_node {
	struct list_head	lin_link[MDS_INODELOCK_NUMBITS];
	struct ldlm_lock	*lock;
};

/** Whether to track references to exports by LDLM locks. */
#define LUSTRE_TRACKS_LOCK_EXP_REFS (0)

/** Cancel flags. */
enum ldlm_cancel_flags {
	LCF_ASYNC	= 0x1, /* Cancel locks asynchronously. */
	LCF_LOCAL	= 0x2, /* Cancel locks locally, not notifing server */
	LCF_BL_AST	= 0x4, /* Cancel LDLM_FL_BL_AST locks in the same RPC */
	LCF_CONVERT	= 0x8, /* Try to convert IBITS lock before cancel */
};

struct ldlm_flock {
	__u64 start;
	__u64 end;
	__u64 owner;
	__u64 blocking_owner;
	struct obd_export *blocking_export;
	atomic_t blocking_refs;
	__u32 pid;
};

union ldlm_policy_data {
	struct ldlm_extent l_extent;
	struct ldlm_flock l_flock;
	struct ldlm_inodebits l_inodebits;
};

void ldlm_convert_policy_to_wire(enum ldlm_type type,
				 const union ldlm_policy_data *lpolicy,
				 union ldlm_wire_policy_data *wpolicy);
void ldlm_convert_policy_to_local(struct obd_export *exp, enum ldlm_type type,
				  const union ldlm_wire_policy_data *wpolicy,
				  union ldlm_policy_data *lpolicy);

enum lvb_type {
	LVB_T_NONE	= 0,
	LVB_T_OST	= 1,
	LVB_T_LQUOTA	= 2,
	LVB_T_LAYOUT	= 3,
};

/**
 * LDLM_GID_ANY is used to match any group id in ldlm_lock_match().
 */
#define LDLM_GID_ANY  ((__u64)-1)

/**
 * LDLM lock structure
 *
 * Represents a single LDLM lock and its state in memory. Each lock is
 * associated with a single ldlm_resource, the object which is being
 * locked. There may be multiple ldlm_locks on a single resource,
 * depending on the lock type and whether the locks are conflicting or
 * not.
 */
struct ldlm_lock {
	/**
	 * Local lock handle.
	 * When remote side wants to tell us about a lock, they address
	 * it by this opaque handle.  The handle does not hold a
	 * reference on the ldlm_lock, so it can be safely passed to
	 * other threads or nodes. When the lock needs to be accessed
	 * from the handle, it is looked up again in the lock table, and
	 * may no longer exist.
	 *
	 * Must be first in the structure.
	 */
	struct portals_handle	l_handle;
	/**
	 * Lock reference count.
	 * This is how many users have pointers to actual structure, so that
	 * we do not accidentally free lock structure that is in use.
	 */
	atomic_t		l_refc;
	/**
	 * Internal spinlock protects l_resource.  We should hold this lock
	 * first before taking res_lock.
	 */
	spinlock_t		l_lock;
	/**
	 * Pointer to actual resource this lock is in.
	 * ldlm_lock_change_resource() can change this.
	 */
	struct ldlm_resource	*l_resource;
	/**
	 * List item for client side LRU list.
	 * Protected by ns_lock in struct ldlm_namespace.
	 */
	struct list_head	l_lru;
	/**
	 * Linkage to resource's lock queues according to current lock state.
	 * (could be granted or waiting)
	 * Protected by lr_lock in struct ldlm_resource.
	 */
	struct list_head	l_res_link;
	/**
	 * Internal structures per lock type..
	 */
	union {
		struct ldlm_interval	*l_tree_node;
		struct ldlm_ibits_node  *l_ibits_node;
	};
	/**
	 * Per export hash of locks.
	 * Protected by per-bucket exp->exp_lock_hash locks.
	 */
	struct hlist_node	l_exp_hash;
	/**
	 * Per export hash of flock locks.
	 * Protected by per-bucket exp->exp_flock_hash locks.
	 */
	struct hlist_node	l_exp_flock_hash;
	/**
	 * Requested mode.
	 * Protected by lr_lock.
	 */
	enum ldlm_mode		l_req_mode;
	/**
	 * Granted mode, also protected by lr_lock.
	 */
	enum ldlm_mode		l_granted_mode;
	/** Lock completion handler pointer. Called when lock is granted. */
	ldlm_completion_callback l_completion_ast;
	/**
	 * Lock blocking AST handler pointer.
	 * It plays two roles:
	 * - as a notification of an attempt to queue a conflicting lock (once)
	 * - as a notification when the lock is being cancelled.
	 *
	 * As such it's typically called twice: once for the initial conflict
	 * and then once more when the last user went away and the lock is
	 * cancelled (could happen recursively).
	 */
	ldlm_blocking_callback	l_blocking_ast;
	/**
	 * Lock glimpse handler.
	 * Glimpse handler is used to obtain LVB updates from a client by
	 * server
	 */
	ldlm_glimpse_callback	l_glimpse_ast;

	/**
	 * Lock export.
	 * This is a pointer to actual client export for locks that were granted
	 * to clients. Used server-side.
	 */
	struct obd_export	*l_export;
	/**
	 * Lock connection export.
	 * Pointer to server export on a client.
	 */
	struct obd_export	*l_conn_export;

	/**
	 * Remote lock handle.
	 * If the lock is remote, this is the handle of the other side lock
	 * (l_handle)
	 */
	struct lustre_handle	l_remote_handle;

	/**
	 * Representation of private data specific for a lock type.
	 * Examples are: extent range for extent lock or bitmask for ibits locks
	 */
	union ldlm_policy_data	l_policy_data;

	/**
	 * Lock state flags. Protected by lr_lock.
	 * \see lustre_dlm_flags.h where the bits are defined.
	 */
	__u64			l_flags;

	/**
	 * Lock r/w usage counters.
	 * Protected by lr_lock.
	 */
	__u32			l_readers;
	__u32			l_writers;
	/**
	 * If the lock is granted, a process sleeps on this waitq to learn when
	 * it's no longer in use.  If the lock is not granted, a process sleeps
	 * on this waitq to learn when it becomes granted.
	 */
	wait_queue_head_t	l_waitq;

	/**
	 * Time, in nanoseconds, last used by e.g. being matched by lock match.
	 */
	ktime_t			l_last_used;

	/** Originally requested extent for the extent lock. */
	struct ldlm_extent	l_req_extent;

	/*
	 * Client-side-only members.
	 */

	enum lvb_type	      l_lvb_type;

	/**
	 * Temporary storage for a LVB received during an enqueue operation.
	 * May be vmalloc'd, so needs to be freed with OBD_FREE_LARGE().
	 */
	__u32			l_lvb_len;
	void			*l_lvb_data;

	/** Private storage for lock user. Opaque to LDLM. */
	void			*l_ast_data;

	union {
	/**
	 * Seconds. It will be updated if there is any activity related to
	 * the lock at client, e.g. enqueue the lock. For server it is the
	 * time when blocking ast was sent.
	 */
		time64_t	l_activity;
		time64_t	l_blast_sent;
	};

	/* separate ost_lvb used mostly by Data-on-MDT for now.
	 * It is introduced to don't mix with layout lock data. */
	struct ost_lvb		 l_ost_lvb;
	/*
	 * Server-side-only members.
	 */

	/**
	 * Connection cookie for the client originating the operation.
	 * Used by Commit on Share (COS) code. Currently only used for
	 * inodebits locks on MDS.
	 */
	__u64			l_client_cookie;

	/**
	 * List item for locks waiting for cancellation from clients.
	 * The lists this could be linked into are:
	 * waiting_locks_list (protected by waiting_locks_spinlock),
	 * then if the lock timed out, it is moved to
	 * expired_lock_list for further processing.
	 */
	struct list_head	l_pending_chain;

	/**
	 * Set when lock is sent a blocking AST. Time in seconds when timeout
	 * is reached and client holding this lock could be evicted.
	 * This timeout could be further extended by e.g. certain IO activity
	 * under this lock.
	 * \see ost_rw_prolong_locks
	 */
	time64_t		l_callback_timeout;

	/** Local PID of process which created this lock. */
	__u32			l_pid;

	/**
	 * Number of times blocking AST was sent for this lock.
	 * This is for debugging. Valid values are 0 and 1, if there is an
	 * attempt to send blocking AST more than once, an assertion would be
	 * hit. \see ldlm_work_bl_ast_lock
	 */
	int			l_bl_ast_run;
	/** List item ldlm_add_ast_work_item() for case of blocking ASTs. */
	struct list_head	l_bl_ast;
	/** List item ldlm_add_ast_work_item() for case of completion ASTs. */
	struct list_head	l_cp_ast;
	/** For ldlm_add_ast_work_item() for "revoke" AST used in COS. */
	struct list_head	l_rk_ast;

	/**
	 * Pointer to a conflicting lock that caused blocking AST to be sent
	 * for this lock
	 */
	struct ldlm_lock	*l_blocking_lock;

	/**
	 * Protected by lr_lock, linkages to "skip lists".
	 * For more explanations of skip lists see ldlm/ldlm_inodebits.c
	 */
	struct list_head	l_sl_mode;
	struct list_head	l_sl_policy;

	/** Reference tracking structure to debug leaked locks. */
	struct lu_ref		l_reference;
#if LUSTRE_TRACKS_LOCK_EXP_REFS
	/* Debugging stuff for bug 20498, for tracking export references. */
	/** number of export references taken */
	int			l_exp_refs_nr;
	/** link all locks referencing one export */
	struct list_head	l_exp_refs_link;
	/** referenced export object */
	struct obd_export	*l_exp_refs_target;
#endif
	/**
	 * export blocking dlm lock list, protected by
	 * l_export->exp_bl_list_lock.
	 * Lock order of waiting_lists_spinlock, exp_bl_list_lock and res lock
	 * is: res lock -> exp_bl_list_lock -> wanting_lists_spinlock.
	 */
	struct list_head	l_exp_list;
};

/** For uncommitted cross-MDT lock, store transno this lock belongs to */
#define l_transno l_client_cookie

/** For uncommitted cross-MDT lock, which is client lock, share with l_rk_ast
 *  which is for server. */
#define l_slc_link l_rk_ast

#define HANDLE_MAP_SIZE  ((LMV_MAX_STRIPE_COUNT + 7) >> 3)

struct lustre_handle_array {
	unsigned int		ha_count;
	/* ha_map is used as bit flag to indicate handle is remote or local */
	char			ha_map[HANDLE_MAP_SIZE];
	struct lustre_handle	ha_handles[0];
};

/**
 * LDLM resource description.
 * Basically, resource is a representation for a single object.
 * Object has a name which is currently 4 64-bit integers. LDLM user is
 * responsible for creation of a mapping between objects it wants to be
 * protected and resource names.
 *
 * A resource can only hold locks of a single lock type, though there may be
 * multiple ldlm_locks on a single resource, depending on the lock type and
 * whether the locks are conflicting or not.
 */
struct ldlm_resource {
	struct ldlm_ns_bucket	*lr_ns_bucket;

	/**
	 * List item for list in namespace hash.
	 * protected by ns_lock
	 */
	struct hlist_node	lr_hash;

	/** Reference count for this resource */
	atomic_t		lr_refcount;

	/** Spinlock to protect locks under this resource. */
	spinlock_t		lr_lock;

	/**
	 * protected by lr_lock
	 * @{ */
	/** List of locks in granted state */
	struct list_head	lr_granted;
	/**
	 * List of locks that could not be granted due to conflicts and
	 * that are waiting for conflicts to go away */
	struct list_head	lr_waiting;
	/** @} */

	/** Resource name */
	struct ldlm_res_id	lr_name;

	union {
		/**
		 * Interval trees (only for extent locks) for all modes of
		 * this resource
		 */
		struct ldlm_interval_tree *lr_itree;
		struct ldlm_ibits_queues *lr_ibits_queues;
	};

	union {
		/**
		 * When the resource was considered as contended,
		 * used only on server side.
		 */
		time64_t	lr_contention_time;
		/**
		 * Associated inode, used only on client side.
		 */
		struct inode	*lr_lvb_inode;
	};

	/** Type of locks this resource can hold. Only one type per resource. */
	enum ldlm_type		lr_type; /* LDLM_{PLAIN,EXTENT,FLOCK,IBITS} */

	/**
	 * Server-side-only lock value block elements.
	 * To serialize lvbo_init.
	 */
	int			lr_lvb_len;
	struct mutex		lr_lvb_mutex;
	/** protected by lr_lock */
	void			*lr_lvb_data;
	/** is lvb initialized ? */
	bool			lr_lvb_initialized;

	/** List of references to this resource. For debugging. */
	struct lu_ref		lr_reference;
};

static inline int ldlm_is_granted(struct ldlm_lock *lock)
{
	return lock->l_req_mode == lock->l_granted_mode;
}

static inline bool ldlm_has_layout(struct ldlm_lock *lock)
{
	return lock->l_resource->lr_type == LDLM_IBITS &&
		lock->l_policy_data.l_inodebits.bits & MDS_INODELOCK_LAYOUT;
}

static inline bool ldlm_has_dom(struct ldlm_lock *lock)
{
	return lock->l_resource->lr_type == LDLM_IBITS &&
		lock->l_policy_data.l_inodebits.bits & MDS_INODELOCK_DOM;
}

static inline char *
ldlm_ns_name(struct ldlm_namespace *ns)
{
	return ns->ns_name;
}

static inline struct ldlm_namespace *
ldlm_res_to_ns(struct ldlm_resource *res)
{
        return res->lr_ns_bucket->nsb_namespace;
}

static inline struct ldlm_namespace *
ldlm_lock_to_ns(struct ldlm_lock *lock)
{
        return ldlm_res_to_ns(lock->l_resource);
}

static inline char *
ldlm_lock_to_ns_name(struct ldlm_lock *lock)
{
        return ldlm_ns_name(ldlm_lock_to_ns(lock));
}

static inline struct adaptive_timeout *
ldlm_lock_to_ns_at(struct ldlm_lock *lock)
{
        return &lock->l_resource->lr_ns_bucket->nsb_at_estimate;
}

static inline int ldlm_lvbo_init(struct ldlm_resource *res)
{
	struct ldlm_namespace *ns = ldlm_res_to_ns(res);
	int rc = 0;

	if (ns->ns_lvbo == NULL || ns->ns_lvbo->lvbo_init == NULL ||
	    res->lr_lvb_initialized)
		return 0;

	mutex_lock(&res->lr_lvb_mutex);
	/* Did we lose the race? */
	if (res->lr_lvb_initialized) {
		mutex_unlock(&res->lr_lvb_mutex);
		return 0;
	}
	rc = ns->ns_lvbo->lvbo_init(res);
	if (rc < 0) {
		CDEBUG(D_DLMTRACE, "lvbo_init failed for resource : rc = %d\n",
		       rc);
		if (res->lr_lvb_data != NULL) {
			OBD_FREE(res->lr_lvb_data, res->lr_lvb_len);
			res->lr_lvb_data = NULL;
		}
		res->lr_lvb_len = rc;
	} else {
		res->lr_lvb_initialized = true;
	}
	mutex_unlock(&res->lr_lvb_mutex);
	return rc;
}

static inline int ldlm_lvbo_size(struct ldlm_lock *lock)
{
	struct ldlm_namespace *ns = ldlm_lock_to_ns(lock);

	if (ns->ns_lvbo != NULL && ns->ns_lvbo->lvbo_size != NULL)
		return ns->ns_lvbo->lvbo_size(lock);

	return 0;
}

static inline int ldlm_lvbo_fill(struct ldlm_lock *lock, void *buf, int *len)
{
	struct ldlm_namespace *ns = ldlm_lock_to_ns(lock);
	int rc;

	if (ns->ns_lvbo != NULL) {
		LASSERT(ns->ns_lvbo->lvbo_fill != NULL);
		/* init lvb now if not already */
		rc = ldlm_lvbo_init(lock->l_resource);
		if (rc < 0) {
			CERROR("lock %p: delayed lvb init failed (rc %d)",
			       lock, rc);
			return rc;
		}
		return ns->ns_lvbo->lvbo_fill(lock, buf, len);
	}
	return 0;
}

struct ldlm_ast_work {
	struct ldlm_lock       *w_lock;
	int			w_blocking;
	struct ldlm_lock_desc	w_desc;
	struct list_head	w_list;
	int			w_flags;
	void		       *w_data;
	int			w_datalen;
};

/**
 * Common ldlm_enqueue parameters
 */
struct ldlm_enqueue_info {
	enum ldlm_type	ei_type;	/** Type of the lock being enqueued. */
	enum ldlm_mode	ei_mode;	/** Mode of the lock being enqueued. */
	void		*ei_cb_bl;	/** blocking lock callback */
	void		*ei_cb_local_bl; /** blocking local lock callback */
	void		*ei_cb_cp;	/** lock completion callback */
	void		*ei_cb_gl;	/** lock glimpse callback */
	void		*ei_cbdata;	/** Data to be passed into callbacks. */
	void		*ei_namespace;	/** lock namespace **/
	u64		ei_inodebits;	/** lock inode bits **/
	unsigned int	ei_enq_slave:1;	/** whether enqueue slave stripes */
};

#define ei_res_id	ei_cb_gl

extern struct obd_ops ldlm_obd_ops;

extern char *ldlm_lockname[];
extern char *ldlm_typename[];
extern const char *ldlm_it2str(enum ldlm_intent_flags it);

/**
 * Just a fancy CDEBUG call with log level preset to LDLM_DEBUG.
 * For the cases where we do not have actual lock to print along
 * with a debugging message that is ldlm-related
 */
#define LDLM_DEBUG_NOLOCK(format, a...)			\
	CDEBUG(D_DLMTRACE, "### " format "\n" , ##a)

/**
 * Support function for lock information printing into debug logs.
 * \see LDLM_DEBUG
 */
#ifdef LIBCFS_DEBUG
#define ldlm_lock_debug(msgdata, mask, cdls, lock, fmt, a...) do {      \
        CFS_CHECK_STACK(msgdata, mask, cdls);                           \
                                                                        \
        if (((mask) & D_CANTMASK) != 0 ||                               \
            ((libcfs_debug & (mask)) != 0 &&                            \
             (libcfs_subsystem_debug & DEBUG_SUBSYSTEM) != 0))          \
                _ldlm_lock_debug(lock, msgdata, fmt, ##a);              \
} while(0)

void _ldlm_lock_debug(struct ldlm_lock *lock,
                      struct libcfs_debug_msg_data *data,
                      const char *fmt, ...)
        __attribute__ ((format (printf, 3, 4)));

/**
 * Rate-limited version of lock printing function.
 */
#define LDLM_DEBUG_LIMIT(mask, lock, fmt, a...) do {                         \
	static struct cfs_debug_limit_state _ldlm_cdls;			     \
        LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, mask, &_ldlm_cdls);              \
        ldlm_lock_debug(&msgdata, mask, &_ldlm_cdls, lock, "### " fmt , ##a);\
} while (0)

#define LDLM_ERROR(lock, fmt, a...) LDLM_DEBUG_LIMIT(D_ERROR, lock, fmt, ## a)
#define LDLM_WARN(lock, fmt, a...)  LDLM_DEBUG_LIMIT(D_WARNING, lock, fmt, ## a)

/** Non-rate-limited lock printing function for debugging purposes. */
#define LDLM_DEBUG(lock, fmt, a...)   do {                                  \
	if (likely(lock != NULL)) {					    \
		LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, D_DLMTRACE, NULL);      \
		ldlm_lock_debug(&msgdata, D_DLMTRACE, NULL, lock, 	    \
				"### " fmt , ##a);			    \
	} else {							    \
		LDLM_DEBUG_NOLOCK("no dlm lock: " fmt, ##a);		    \
	}								    \
} while (0)
#else /* !LIBCFS_DEBUG */
# define LDLM_DEBUG_LIMIT(mask, lock, fmt, a...) ((void)0)
# define LDLM_DEBUG(lock, fmt, a...) ((void)0)
# define LDLM_ERROR(lock, fmt, a...) ((void)0)
#endif

/*
 * Three intentions can be used for the policy functions in
 * ldlm_processing_policy.
 *
 * LDLM_PROCESS_RESCAN:
 *
 * It's used when policy functions are called from ldlm_reprocess_queue() to
 * reprocess the wait list and try to grant locks, blocking ASTs
 * have already been sent in this situation, completion ASTs need be sent for
 * the locks being granted.
 *
 * LDLM_PROCESS_ENQUEUE:
 *
 * It's used when policy functions are called from ldlm_lock_enqueue() to
 * process the wait list for handling an enqueue request, blocking
 * ASTs have not been sent yet, so list of conflicting locks would be
 * collected and ASTs sent.
 *
 * LDLM_PROCESS_RECOVERY:
 *
 * It's used when policy functions are called from ldlm_reprocess_queue() to
 * reprocess the wait list when recovery done. In case of blocking
 * ASTs are lost before recovery, it needs not only to grant locks if
 * available, but also send blocking ASTs to the locks doesn't have AST sent
 * flag. Completion ASTs need be sent for the locks being granted.
 */
enum ldlm_process_intention {
	LDLM_PROCESS_RESCAN = 0,
	LDLM_PROCESS_ENQUEUE = 1,
	LDLM_PROCESS_RECOVERY = 2,
};

typedef int (*ldlm_processing_policy)(struct ldlm_lock *lock, __u64 *flags,
				      enum ldlm_process_intention intention,
				      enum ldlm_error *err,
				      struct list_head *work_list);

typedef int (*ldlm_reprocessing_policy)(struct ldlm_resource *res,
					struct list_head *queue,
					struct list_head *work_list,
					enum ldlm_process_intention intention,
					struct ldlm_lock *hint);

/**
 * Return values for lock iterators.
 * Also used during deciding of lock grants and cancellations.
 */
#define LDLM_ITER_CONTINUE 1 /* keep iterating */
#define LDLM_ITER_STOP     2 /* stop iterating */

typedef int (*ldlm_iterator_t)(struct ldlm_lock *, void *);
typedef int (*ldlm_res_iterator_t)(struct ldlm_resource *, void *);

/** \defgroup ldlm_iterator Lock iterators
 *
 * LDLM provides for a way to iterate through every lock on a resource or
 * namespace or every resource in a namespace.
 * @{ */
int ldlm_resource_foreach(struct ldlm_resource *res, ldlm_iterator_t iter,
			  void *closure);
void ldlm_namespace_foreach(struct ldlm_namespace *ns, ldlm_iterator_t iter,
			    void *closure);
int ldlm_resource_iterate(struct ldlm_namespace *, const struct ldlm_res_id *,
			  ldlm_iterator_t iter, void *data);
/** @} ldlm_iterator */

int ldlm_replay_locks(struct obd_import *imp);

/* ldlm_flock.c */
int ldlm_flock_completion_ast(struct ldlm_lock *lock, __u64 flags, void *data);

/* ldlm_extent.c */
__u64 ldlm_extent_shift_kms(struct ldlm_lock *lock, __u64 old_kms);

struct ldlm_prolong_args {
	struct obd_export	*lpa_export;
	struct ldlm_res_id	lpa_resid;
	struct ldlm_extent	lpa_extent;
	enum ldlm_mode		lpa_mode;
	time64_t		lpa_timeout;
	int			lpa_locks_cnt;
	int			lpa_blocks_cnt;
};
void ldlm_lock_prolong_one(struct ldlm_lock *lock,
			   struct ldlm_prolong_args *arg);
void ldlm_resource_prolong(struct ldlm_prolong_args *arg);

struct ldlm_callback_suite {
        ldlm_completion_callback lcs_completion;
        ldlm_blocking_callback   lcs_blocking;
        ldlm_glimpse_callback    lcs_glimpse;
};

/* ldlm_lockd.c */
#ifdef HAVE_SERVER_SUPPORT
/** \defgroup ldlm_srv_ast Server AST handlers
 * These are AST handlers used by server code.
 * Their property is that they are just preparing RPCs to be sent to clients.
 * @{
 */
int ldlm_server_blocking_ast(struct ldlm_lock *, struct ldlm_lock_desc *,
			     void *data, int flag);
int ldlm_server_completion_ast(struct ldlm_lock *lock, __u64 flags, void *data);
int ldlm_server_glimpse_ast(struct ldlm_lock *lock, void *data);
int ldlm_glimpse_locks(struct ldlm_resource *res,
		       struct list_head *gl_work_list);
/** @} ldlm_srv_ast */

/** \defgroup ldlm_handlers Server LDLM handlers
 * These are handler functions that should be called by "frontends" such as
 * MDT or OST to pass through LDLM requests to LDLM for handling
 * @{
 */
int ldlm_handle_enqueue0(struct ldlm_namespace *ns, struct ptlrpc_request *req,
			 const struct ldlm_request *dlm_req,
			 const struct ldlm_callback_suite *cbs);
int ldlm_handle_convert0(struct ptlrpc_request *req,
			 const struct ldlm_request *dlm_req);
int ldlm_handle_cancel(struct ptlrpc_request *req);
int ldlm_request_cancel(struct ptlrpc_request *req,
			const struct ldlm_request *dlm_req,
			int first, enum lustre_at_flags flags);
/** @} ldlm_handlers */

void ldlm_revoke_export_locks(struct obd_export *exp);
time64_t ldlm_bl_timeout(struct ldlm_lock *lock);
#endif
int ldlm_del_waiting_lock(struct ldlm_lock *lock);
int ldlm_refresh_waiting_lock(struct ldlm_lock *lock, time64_t timeout);
int ldlm_get_ref(void);
void ldlm_put_ref(void);
int ldlm_init_export(struct obd_export *exp);
void ldlm_destroy_export(struct obd_export *exp);
struct ldlm_lock *ldlm_request_lock(struct ptlrpc_request *req);

/* ldlm_lock.c */
#ifdef HAVE_SERVER_SUPPORT
ldlm_processing_policy ldlm_get_processing_policy(struct ldlm_resource *res);
ldlm_reprocessing_policy
ldlm_get_reprocessing_policy(struct ldlm_resource *res);
#endif
void ldlm_register_intent(struct ldlm_namespace *ns, ldlm_res_policy arg);
void ldlm_lock2handle(const struct ldlm_lock *lock,
                      struct lustre_handle *lockh);
struct ldlm_lock *__ldlm_handle2lock(const struct lustre_handle *, __u64 flags);
void ldlm_cancel_callback(struct ldlm_lock *);
int ldlm_lock_remove_from_lru(struct ldlm_lock *);
int ldlm_lock_set_data(const struct lustre_handle *lockh, void *data);

/**
 * Obtain a lock reference by its handle.
 */
static inline struct ldlm_lock *ldlm_handle2lock(const struct lustre_handle *h)
{
        return __ldlm_handle2lock(h, 0);
}

#define LDLM_LOCK_REF_DEL(lock) \
	lu_ref_del(&lock->l_reference, "handle", current)

static inline struct ldlm_lock *
ldlm_handle2lock_long(const struct lustre_handle *h, __u64 flags)
{
        struct ldlm_lock *lock;

        lock = __ldlm_handle2lock(h, flags);
        if (lock != NULL)
                LDLM_LOCK_REF_DEL(lock);
        return lock;
}

/**
 * Update Lock Value Block Operations (LVBO) on a resource taking into account
 * data from request \a r
 */
static inline int ldlm_lvbo_update(struct ldlm_resource *res,
				   struct ldlm_lock *lock,
				   struct ptlrpc_request *req, int increase)
{
	struct ldlm_namespace *ns = ldlm_res_to_ns(res);
	int rc;

	/* delayed lvb init may be required */
	rc = ldlm_lvbo_init(res);
	if (rc < 0) {
		CERROR("delayed lvb init failed (rc %d)\n", rc);
		return rc;
	}

	if (ns->ns_lvbo && ns->ns_lvbo->lvbo_update)
		return ns->ns_lvbo->lvbo_update(res, lock, req, increase);

	return 0;
}

static inline int ldlm_res_lvbo_update(struct ldlm_resource *res,
				       struct ptlrpc_request *req,
				       int increase)
{
	return ldlm_lvbo_update(res, NULL, req, increase);
}

int is_granted_or_cancelled_nolock(struct ldlm_lock *lock);

int ldlm_error2errno(enum ldlm_error error);
enum ldlm_error ldlm_errno2error(int err_no); /* don't call it `errno': this
					       * confuses user-space. */
#if LUSTRE_TRACKS_LOCK_EXP_REFS
void ldlm_dump_export_locks(struct obd_export *exp);
#endif

/**
 * Release a temporary lock reference obtained by ldlm_handle2lock() or
 * __ldlm_handle2lock().
 */
#define LDLM_LOCK_PUT(lock)                     \
do {                                            \
        LDLM_LOCK_REF_DEL(lock);                \
        /*LDLM_DEBUG((lock), "put");*/          \
        ldlm_lock_put(lock);                    \
} while (0)

/**
 * Release a lock reference obtained by some other means (see
 * LDLM_LOCK_PUT()).
 */
#define LDLM_LOCK_RELEASE(lock)                 \
do {                                            \
        /*LDLM_DEBUG((lock), "put");*/          \
        ldlm_lock_put(lock);                    \
} while (0)

#define LDLM_LOCK_GET(lock)                     \
({                                              \
        ldlm_lock_get(lock);                    \
        /*LDLM_DEBUG((lock), "get");*/          \
        lock;                                   \
})

#define ldlm_lock_list_put(head, member, count)			\
({								\
	struct ldlm_lock *_lock, *_next;			\
	int c = count;						\
	list_for_each_entry_safe(_lock, _next, head, member) {	\
		if (c-- == 0)					\
			break;					\
		list_del_init(&_lock->member);			\
		LDLM_LOCK_RELEASE(_lock);			\
	}							\
	LASSERT(c <= 0);					\
})

struct ldlm_lock *ldlm_lock_get(struct ldlm_lock *lock);
void ldlm_lock_put(struct ldlm_lock *lock);
void ldlm_lock_destroy(struct ldlm_lock *lock);
void ldlm_lock2desc(struct ldlm_lock *lock, struct ldlm_lock_desc *desc);
void ldlm_lock_addref(const struct lustre_handle *lockh, enum ldlm_mode mode);
int  ldlm_lock_addref_try(const struct lustre_handle *lockh,
			  enum ldlm_mode mode);
void ldlm_lock_decref(const struct lustre_handle *lockh, enum ldlm_mode mode);
void ldlm_lock_decref_and_cancel(const struct lustre_handle *lockh,
				 enum ldlm_mode mode);
void ldlm_lock_fail_match_locked(struct ldlm_lock *lock);
void ldlm_lock_fail_match(struct ldlm_lock *lock);
void ldlm_lock_allow_match(struct ldlm_lock *lock);
void ldlm_lock_allow_match_locked(struct ldlm_lock *lock);
enum ldlm_mode ldlm_lock_match_with_skip(struct ldlm_namespace *ns,
					 __u64 flags, __u64 skip_flags,
					 const struct ldlm_res_id *res_id,
					 enum ldlm_type type,
					 union ldlm_policy_data *policy,
					 enum ldlm_mode mode,
					 struct lustre_handle *lh,
					 int unref);
static inline enum ldlm_mode ldlm_lock_match(struct ldlm_namespace *ns,
					     __u64 flags,
					     const struct ldlm_res_id *res_id,
					     enum ldlm_type type,
					     union ldlm_policy_data *policy,
					     enum ldlm_mode mode,
					     struct lustre_handle *lh,
					     int unref)
{
	return ldlm_lock_match_with_skip(ns, flags, 0, res_id, type, policy,
					 mode, lh, unref);
}

enum ldlm_mode ldlm_revalidate_lock_handle(const struct lustre_handle *lockh,
					   __u64 *bits);
void ldlm_lock_mode_downgrade(struct ldlm_lock *lock, enum ldlm_mode new_mode);
void ldlm_lock_cancel(struct ldlm_lock *lock);
void ldlm_reprocess_all(struct ldlm_resource *res, struct ldlm_lock *hint);
void ldlm_reprocess_recovery_done(struct ldlm_namespace *ns);
void ldlm_lock_dump_handle(int level, const struct lustre_handle *lockh);
void ldlm_unlink_lock_skiplist(struct ldlm_lock *req);

/* resource.c */
struct ldlm_namespace *ldlm_namespace_new(struct obd_device *obd, char *name,
					  enum ldlm_side client,
					  enum ldlm_appetite apt,
					  enum ldlm_ns_type ns_type);
int ldlm_namespace_cleanup(struct ldlm_namespace *ns, __u64 flags);
void ldlm_namespace_free_prior(struct ldlm_namespace *ns,
			       struct obd_import *imp,
			       int force);
void ldlm_namespace_free_post(struct ldlm_namespace *ns);
void ldlm_namespace_free(struct ldlm_namespace *ns,
			 struct obd_import *imp, int force);
void ldlm_namespace_register(struct ldlm_namespace *ns, enum ldlm_side client);
void ldlm_namespace_unregister(struct ldlm_namespace *ns,
			       enum ldlm_side client);
void ldlm_namespace_get(struct ldlm_namespace *ns);
void ldlm_namespace_put(struct ldlm_namespace *ns);

int ldlm_debugfs_setup(void);
void ldlm_debugfs_cleanup(void);

static inline void ldlm_svc_get_eopc(const struct ldlm_request *dlm_req,
				     struct lprocfs_stats *srv_stats)
{
	int lock_type = 0, op = 0;

	lock_type = dlm_req->lock_desc.l_resource.lr_type;

	switch (lock_type) {
	case LDLM_PLAIN:
		op = PTLRPC_LAST_CNTR + LDLM_PLAIN_ENQUEUE;
		break;
	case LDLM_EXTENT:
		op = PTLRPC_LAST_CNTR + LDLM_EXTENT_ENQUEUE;
		break;
	case LDLM_FLOCK:
		op = PTLRPC_LAST_CNTR + LDLM_FLOCK_ENQUEUE;
		break;
	case LDLM_IBITS:
		op = PTLRPC_LAST_CNTR + LDLM_IBITS_ENQUEUE;
		break;
	default:
		op = 0;
		break;
	}

	if (op != 0)
		lprocfs_counter_incr(srv_stats, op);

	return;
}

/* resource.c - internal */
struct ldlm_resource *ldlm_resource_get(struct ldlm_namespace *ns,
					struct ldlm_resource *parent,
					const struct ldlm_res_id *,
					enum ldlm_type type, int create);
struct ldlm_resource *ldlm_resource_getref(struct ldlm_resource *res);
int ldlm_resource_putref(struct ldlm_resource *res);
void ldlm_resource_add_lock(struct ldlm_resource *res,
			    struct list_head *head,
			    struct ldlm_lock *lock);
void ldlm_resource_unlink_lock(struct ldlm_lock *lock);
void ldlm_res2desc(struct ldlm_resource *res, struct ldlm_resource_desc *desc);
void ldlm_dump_all_namespaces(enum ldlm_side client, int level);
void ldlm_namespace_dump(int level, struct ldlm_namespace *);
void ldlm_resource_dump(int level, struct ldlm_resource *);
int ldlm_lock_change_resource(struct ldlm_namespace *, struct ldlm_lock *,
                              const struct ldlm_res_id *);

#define LDLM_RESOURCE_ADDREF(res) do {                                  \
	lu_ref_add_atomic(&(res)->lr_reference, __FUNCTION__, current);  \
} while (0)

#define LDLM_RESOURCE_DELREF(res) do {                                  \
	lu_ref_del(&(res)->lr_reference, __FUNCTION__, current);  \
} while (0)

/* ldlm_request.c */
int ldlm_expired_completion_wait(void *data);
/** \defgroup ldlm_local_ast Default AST handlers for local locks
 * These AST handlers are typically used for server-side local locks and are
 * also used by client-side lock handlers to perform minimum level base
 * processing.
 * @{ */
int ldlm_blocking_ast_nocheck(struct ldlm_lock *lock);
int ldlm_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *desc,
		      void *data, int flag);
int ldlm_glimpse_ast(struct ldlm_lock *lock, void *reqp);
int ldlm_completion_ast_async(struct ldlm_lock *lock, __u64 flags, void *data);
int ldlm_completion_ast(struct ldlm_lock *lock, __u64 flags, void *data);
/** @} ldlm_local_ast */

/** \defgroup ldlm_cli_api API to operate on locks from actual LDLM users.
 * These are typically used by client and server (*_local versions)
 * to obtain and release locks.
 * @{ */
int ldlm_cli_enqueue(struct obd_export *exp, struct ptlrpc_request **reqp,
		     struct ldlm_enqueue_info *einfo,
		     const struct ldlm_res_id *res_id,
		     union ldlm_policy_data const *policy, __u64 *flags,
		     void *lvb, __u32 lvb_len, enum lvb_type lvb_type,
		     struct lustre_handle *lockh, int async);
int ldlm_prep_enqueue_req(struct obd_export *exp,
			  struct ptlrpc_request *req,
			  struct list_head *cancels,
			  int count);
int ldlm_prep_elc_req(struct obd_export *exp, struct ptlrpc_request *req,
		      int version, int opc, int canceloff,
		      struct list_head *cancels, int count);

struct ptlrpc_request *ldlm_enqueue_pack(struct obd_export *exp, int lvb_len);
int ldlm_handle_enqueue0(struct ldlm_namespace *ns, struct ptlrpc_request *req,
			 const struct ldlm_request *dlm_req,
			 const struct ldlm_callback_suite *cbs);
int ldlm_cli_enqueue_fini(struct obd_export *exp, struct ptlrpc_request *req,
			  enum ldlm_type type, __u8 with_policy,
			  enum ldlm_mode mode, __u64 *flags, void *lvb,
			  __u32 lvb_len,
			  const struct lustre_handle *lockh, int rc);
int ldlm_cli_enqueue_local(const struct lu_env *env,
			   struct ldlm_namespace *ns,
			   const struct ldlm_res_id *res_id,
			   enum ldlm_type type, union ldlm_policy_data *policy,
			   enum ldlm_mode mode, __u64 *flags,
			   ldlm_blocking_callback blocking,
			   ldlm_completion_callback completion,
			   ldlm_glimpse_callback glimpse,
			   void *data, __u32 lvb_len, enum lvb_type lvb_type,
			   const __u64 *client_cookie,
			   struct lustre_handle *lockh);
int ldlm_cli_convert(struct ldlm_lock *lock, __u32 *flags);
int ldlm_cli_update_pool(struct ptlrpc_request *req);
int ldlm_cli_cancel(const struct lustre_handle *lockh,
		    enum ldlm_cancel_flags cancel_flags);
int ldlm_cli_cancel_unused(struct ldlm_namespace *, const struct ldlm_res_id *,
			   enum ldlm_cancel_flags flags, void *opaque);
int ldlm_cli_cancel_unused_resource(struct ldlm_namespace *ns,
				    const struct ldlm_res_id *res_id,
				    union ldlm_policy_data *policy,
				    enum ldlm_mode mode,
				    enum ldlm_cancel_flags flags, void *opaque);
int ldlm_cli_cancel_req(struct obd_export *exp, struct list_head *head,
			int count, enum ldlm_cancel_flags flags);
int ldlm_cancel_resource_local(struct ldlm_resource *res,
			       struct list_head *cancels,
			       union ldlm_policy_data *policy,
			       enum ldlm_mode mode, __u64 lock_flags,
			       enum ldlm_cancel_flags cancel_flags,
			       void *opaque);
int ldlm_cli_cancel_list_local(struct list_head *cancels, int count,
			       enum ldlm_cancel_flags flags);
int ldlm_cli_cancel_list(struct list_head *head, int count,
			 struct ptlrpc_request *req,
			 enum ldlm_cancel_flags flags);

int ldlm_inodebits_drop(struct ldlm_lock *lock, __u64 to_drop);
int ldlm_cli_dropbits(struct ldlm_lock *lock, __u64 drop_bits);
int ldlm_cli_dropbits_list(struct list_head *converts, __u64 drop_bits);

/** @} ldlm_cli_api */

extern unsigned int ldlm_enqueue_min;

/* mds/handler.c */
/* This has to be here because recursive inclusion sucks. */
int intent_disposition(struct ldlm_reply *rep, int flag);
void intent_set_disposition(struct ldlm_reply *rep, int flag);

/**
 * "Modes" of acquiring lock_res, necessary to tell lockdep that taking more
 * than one lock_res is dead-lock safe.
 */
enum lock_res_type {
        LRT_NORMAL,
        LRT_NEW
};

/** Lock resource. */
static inline void lock_res(struct ldlm_resource *res)
{
	spin_lock(&res->lr_lock);
}

/** Lock resource with a way to instruct lockdep code about nestedness-safe. */
static inline void lock_res_nested(struct ldlm_resource *res,
				   enum lock_res_type mode)
{
	spin_lock_nested(&res->lr_lock, mode);
}

/** Unlock resource. */
static inline void unlock_res(struct ldlm_resource *res)
{
	spin_unlock(&res->lr_lock);
}

/** Check if resource is already locked, assert if not. */
static inline void check_res_locked(struct ldlm_resource *res)
{
	assert_spin_locked(&res->lr_lock);
}

struct ldlm_resource * lock_res_and_lock(struct ldlm_lock *lock);
void unlock_res_and_lock(struct ldlm_lock *lock);

/* ldlm_pool.c */
/** \defgroup ldlm_pools Various LDLM pool related functions
 * There are not used outside of ldlm.
 * @{
 */
int ldlm_pools_init(void);
void ldlm_pools_fini(void);

int ldlm_pool_init(struct ldlm_pool *pl, struct ldlm_namespace *ns,
		   int idx, enum ldlm_side client);
int ldlm_pool_shrink(struct ldlm_pool *pl, int nr, gfp_t gfp_mask);
void ldlm_pool_fini(struct ldlm_pool *pl);
int ldlm_pool_setup(struct ldlm_pool *pl, int limit);
time64_t ldlm_pool_recalc(struct ldlm_pool *pl);
__u32 ldlm_pool_get_lvf(struct ldlm_pool *pl);
__u64 ldlm_pool_get_slv(struct ldlm_pool *pl);
__u64 ldlm_pool_get_clv(struct ldlm_pool *pl);
__u32 ldlm_pool_get_limit(struct ldlm_pool *pl);
void ldlm_pool_set_slv(struct ldlm_pool *pl, __u64 slv);
void ldlm_pool_set_clv(struct ldlm_pool *pl, __u64 clv);
void ldlm_pool_set_limit(struct ldlm_pool *pl, __u32 limit);
void ldlm_pool_add(struct ldlm_pool *pl, struct ldlm_lock *lock);
void ldlm_pool_del(struct ldlm_pool *pl, struct ldlm_lock *lock);
/** @} */

static inline int ldlm_extent_overlap(const struct ldlm_extent *ex1,
				      const struct ldlm_extent *ex2)
{
	return ex1->start <= ex2->end && ex2->start <= ex1->end;
}

/* check if @ex1 contains @ex2 */
static inline int ldlm_extent_contain(const struct ldlm_extent *ex1,
				      const struct ldlm_extent *ex2)
{
	return ex1->start <= ex2->start && ex1->end >= ex2->end;
}

int ldlm_inodebits_drop(struct ldlm_lock *lock,  __u64 to_drop);

#endif
/** @} LDLM */
