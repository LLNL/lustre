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
 */
/** \defgroup PtlRPC Portal RPC and networking module.
 *
 * PortalRPC is the layer used by rest of lustre code to achieve network
 * communications: establish connections with corresponding export and import
 * states, listen for a service, send and receive RPCs.
 * PortalRPC also includes base recovery framework: packet resending and
 * replaying, reconnections, pinger.
 *
 * PortalRPC utilizes LNet as its transport layer.
 *
 * @{
 */


#ifndef _LUSTRE_NET_H
#define _LUSTRE_NET_H

/** \defgroup net net
 *
 * @{
 */

#if defined(__linux__)
#include <linux/lustre_net.h>
#elif defined(__APPLE__)
#include <darwin/lustre_net.h>
#elif defined(__WINNT__)
#include <winnt/lustre_net.h>
#else
#error Unsupported operating system.
#endif

#include <libcfs/libcfs.h>
// #include <obd.h>
#include <lnet/lnet.h>
#include <lustre/lustre_idl.h>
#include <lustre_ha.h>
#include <lustre_sec.h>
#include <lustre_import.h>
#include <lprocfs_status.h>
#include <lu_object.h>
#include <lustre_req_layout.h>

#include <obd_support.h>
#include <lustre_ver.h>

/* MD flags we _always_ use */
#define PTLRPC_MD_OPTIONS  0

/**
 * Define maxima for bulk I/O
 * CAVEAT EMPTOR, with multinet (i.e. routers forwarding between networks)
 * these limits are system wide and not interface-local. */
#define PTLRPC_MAX_BRW_BITS     LNET_MTU_BITS
#define PTLRPC_MAX_BRW_SIZE     (1<<LNET_MTU_BITS)
#define PTLRPC_MAX_BRW_PAGES    (PTLRPC_MAX_BRW_SIZE >> CFS_PAGE_SHIFT)

/* When PAGE_SIZE is a constant, we can check our arithmetic here with cpp! */
#ifdef __KERNEL__
# if ((PTLRPC_MAX_BRW_PAGES & (PTLRPC_MAX_BRW_PAGES - 1)) != 0)
#  error "PTLRPC_MAX_BRW_PAGES isn't a power of two"
# endif
# if (PTLRPC_MAX_BRW_SIZE != (PTLRPC_MAX_BRW_PAGES * CFS_PAGE_SIZE))
#  error "PTLRPC_MAX_BRW_SIZE isn't PTLRPC_MAX_BRW_PAGES * CFS_PAGE_SIZE"
# endif
# if (PTLRPC_MAX_BRW_SIZE > LNET_MTU)
#  error "PTLRPC_MAX_BRW_SIZE too big"
# endif
# if (PTLRPC_MAX_BRW_PAGES > LNET_MAX_IOV)
#  error "PTLRPC_MAX_BRW_PAGES too big"
# endif
#endif /* __KERNEL__ */

#define PTLRPC_NTHRS_INIT	2

/**
 * Buffer Constants
 *
 * Constants determine how memory is used to buffer incoming service requests.
 *
 * ?_NBUFS              # buffers to allocate when growing the pool
 * ?_BUFSIZE            # bytes in a single request buffer
 * ?_MAXREQSIZE         # maximum request service will receive
 *
 * When fewer than ?_NBUFS/2 buffers are posted for receive, another chunk
 * of ?_NBUFS is added to the pool.
 *
 * Messages larger than ?_MAXREQSIZE are dropped.  Request buffers are
 * considered full when less than ?_MAXREQSIZE is left in them.
 */
/**
 * Thread Constants
 *
 * Constants determine how threads are created for ptlrpc service.
 *
 * ?_NTHRS_INIT	        # threads to create for each service partition on
 *			  initializing. If it's non-affinity service and
 *			  there is only one partition, it's the overall #
 *			  threads for the service while initializing.
 * ?_NTHRS_BASE		# threads should be created at least for each
 *			  ptlrpc partition to keep the service healthy.
 *			  It's the low-water mark of threads upper-limit
 *			  for each partition.
 * ?_THR_FACTOR         # threads can be added on threads upper-limit for
 *			  each CPU core. This factor is only for reference,
 *			  we might decrease value of factor if number of cores
 *			  per CPT is above a limit.
 * ?_NTHRS_MAX		# overall threads can be created for a service,
 *			  it's a soft limit because if service is running
 *			  on machine with hundreds of cores and tens of
 *			  CPU partitions, we need to guarantee each partition
 *			  has ?_NTHRS_BASE threads, which means total threads
 *			  will be ?_NTHRS_BASE * number_of_cpts which can
 *			  exceed ?_NTHRS_MAX.
 *
 * Examples
 *
 * #define MDT_NTHRS_INIT	2
 * #define MDT_NTHRS_BASE	64
 * #define MDT_NTHRS_FACTOR	8
 * #define MDT_NTHRS_MAX	1024
 *
 * Example 1):
 * ---------------------------------------------------------------------
 * Server(A) has 16 cores, user configured it to 4 partitions so each
 * partition has 4 cores, then actual number of service threads on each
 * partition is:
 *     MDT_NTHRS_BASE(64) + cores(4) * MDT_NTHRS_FACTOR(8) = 96
 *
 * Total number of threads for the service is:
 *     96 * partitions(4) = 384
 *
 * Example 2):
 * ---------------------------------------------------------------------
 * Server(B) has 32 cores, user configured it to 4 partitions so each
 * partition has 8 cores, then actual number of service threads on each
 * partition is:
 *     MDT_NTHRS_BASE(64) + cores(8) * MDT_NTHRS_FACTOR(8) = 128
 *
 * Total number of threads for the service is:
 *     128 * partitions(4) = 512
 *
 * Example 3):
 * ---------------------------------------------------------------------
 * Server(B) has 96 cores, user configured it to 8 partitions so each
 * partition has 12 cores, then actual number of service threads on each
 * partition is:
 *     MDT_NTHRS_BASE(64) + cores(12) * MDT_NTHRS_FACTOR(8) = 160
 *
 * Total number of threads for the service is:
 *     160 * partitions(8) = 1280
 *
 * However, it's above the soft limit MDT_NTHRS_MAX, so we choose this number
 * as upper limit of threads number for each partition:
 *     MDT_NTHRS_MAX(1024) / partitions(8) = 128
 *
 * Example 4):
 * ---------------------------------------------------------------------
 * Server(C) have a thousand of cores and user configured it to 32 partitions
 *     MDT_NTHRS_BASE(64) * 32 = 2048
 *
 * which is already above soft limit MDT_NTHRS_MAX(1024), but we still need
 * to guarantee that each partition has at least MDT_NTHRS_BASE(64) threads
 * to keep service healthy, so total number of threads will just be 2048.
 *
 * NB: we don't suggest to choose server with that many cores because backend
 *     filesystem itself, buffer cache, or underlying network stack might
 *     have some SMP scalability issues at that large scale.
 *
 *     If user already has a fat machine with hundreds or thousands of cores,
 *     there are two choices for configuration:
 *     a) create CPU table from subset of all CPUs and run Lustre on
 *        top of this subset
 *     b) bind service threads on a few partitions, see modparameters of
 *        MDS and OSS for details
*
 * NB: these calculations (and examples below) are simplified to help
 *     understanding, the real implementation is a little more complex,
 *     please see ptlrpc_server_nthreads_check() for details.
 *
 */

 /*
  * LDLM threads constants:
  *
  * Given 8 as factor and 24 as base threads number
  *
  * example 1)
  * On 4-core machine we will have 24 + 8 * 4 = 56 threads.
  *
  * example 2)
  * On 8-core machine with 2 partitions we will have 24 + 4 * 8 = 56
  * threads for each partition and total threads number will be 112.
  *
  * example 3)
  * On 64-core machine with 8 partitions we will need LDLM_NTHRS_BASE(24)
  * threads for each partition to keep service healthy, so total threads
  * number should be 24 * 8 = 192.
  *
  * So with these constants, threads number wil be at the similar level
  * of old versions, unless target machine has over a hundred cores
  */
#define LDLM_THR_FACTOR		8
#define LDLM_NTHRS_INIT		PTLRPC_NTHRS_INIT
#define LDLM_NTHRS_BASE		24
#define LDLM_NTHRS_MAX		(cfs_num_online_cpus() == 1 ? 64 : 128)

#define LDLM_BL_THREADS  LDLM_NTHRS_AUTO_INIT
#define LDLM_NBUFS      (64 * cfs_num_online_cpus())
#define LDLM_BUFSIZE    (8 * 1024)
#define LDLM_MAXREQSIZE (5 * 1024)
#define LDLM_MAXREPSIZE (1024)

 /*
  * MDS threads constants:
  *
  * Please see examples in "Thread Constants", MDS threads number will be at
  * the comparable level of old versions, unless the server has many cores.
  */
#ifndef MDT_MAX_THREADS
#define MDT_MAX_THREADS		1024
#define MDT_MAX_OTHR_THREADS	256

#else /* MDT_MAX_THREADS */
#if MDT_MAX_THREADS < PTLRPC_NTHRS_INIT
#undef MDT_MAX_THREADS
#define MDT_MAX_THREADS	PTLRPC_NTHRS_INIT
#endif
#define MDT_MAX_OTHR_THREADS	max(PTLRPC_NTHRS_INIT, MDT_MAX_THREADS / 2)
#endif

/* default service */
#define MDT_THR_FACTOR		8
#define MDT_NTHRS_INIT		PTLRPC_NTHRS_INIT
#define MDT_NTHRS_MAX		MDT_MAX_THREADS
#define MDT_NTHRS_BASE		min(64, MDT_NTHRS_MAX)

/* read-page service */
#define MDT_RDPG_THR_FACTOR	4
#define MDT_RDPG_NTHRS_INIT	PTLRPC_NTHRS_INIT
#define MDT_RDPG_NTHRS_MAX	MDT_MAX_OTHR_THREADS
#define MDT_RDPG_NTHRS_BASE	min(48, MDT_RDPG_NTHRS_MAX)

/* these should be removed when we remove setattr service in the future */
#define MDT_SETA_THR_FACTOR	4
#define MDT_SETA_NTHRS_INIT	PTLRPC_NTHRS_INIT
#define MDT_SETA_NTHRS_MAX	MDT_MAX_OTHR_THREADS
#define MDT_SETA_NTHRS_BASE	min(48, MDT_SETA_NTHRS_MAX)

/* non-affinity threads */
#define MDT_OTHR_NTHRS_INIT	PTLRPC_NTHRS_INIT
#define MDT_OTHR_NTHRS_MAX	MDT_MAX_OTHR_THREADS

#define MDS_NBUFS		(64 * cfs_num_online_cpus())
/**
 * Assume file name length = FNAME_MAX = 256 (true for ext3).
 *        path name length = PATH_MAX = 4096
 *        LOV MD size max  = EA_MAX = 48000 (2000 stripes)
 * symlink:  FNAME_MAX + PATH_MAX  <- largest
 * link:     FNAME_MAX + PATH_MAX  (mds_rec_link < mds_rec_create)
 * rename:   FNAME_MAX + FNAME_MAX
 * open:     FNAME_MAX + EA_MAX
 *
 * MDS_MAXREQSIZE ~= 4736 bytes =
 * lustre_msg + ldlm_request + mdt_body + mds_rec_create + FNAME_MAX + PATH_MAX
 * MDS_MAXREPSIZE ~= 8300 bytes = lustre_msg + llog_header
 * or, for mds_close() and mds_reint_unlink() on a many-OST filesystem:
 *      = 9210 bytes = lustre_msg + mdt_body + 160 * (easize + cookiesize)
 *
 * Realistic size is about 512 bytes (20 character name + 128 char symlink),
 * except in the open case where there are a large number of OSTs in a LOV.
 */
#define MDS_MAXREPSIZE  max(10 * 1024, 362 + LOV_MAX_STRIPE_COUNT * 56)
#define MDS_MAXREQSIZE  MDS_MAXREPSIZE

/** MDS_BUFSIZE = max_reqsize + max sptlrpc payload size */
#define MDS_BUFSIZE     (MDS_MAXREQSIZE + 1024)

/** FLD_MAXREQSIZE == lustre_msg + __u32 padding + ptlrpc_body + opc */
#define FLD_MAXREQSIZE  (160)

/** FLD_MAXREPSIZE == lustre_msg + ptlrpc_body */
#define FLD_MAXREPSIZE  (152)

/**
 * SEQ_MAXREQSIZE == lustre_msg + __u32 padding + ptlrpc_body + opc + lu_range +
 * __u32 padding */
#define SEQ_MAXREQSIZE  (160)

/** SEQ_MAXREPSIZE == lustre_msg + ptlrpc_body + lu_range */
#define SEQ_MAXREPSIZE  (152)

/** MGS threads must be >= 3, see bug 22458 comment #28 */
#define MGS_NTHRS_INIT	(PTLRPC_NTHRS_INIT + 1)
#define MGS_NTHRS_MAX	32

#define MGS_NBUFS       (64 * cfs_num_online_cpus())
#define MGS_BUFSIZE     (8 * 1024)
#define MGS_MAXREQSIZE  (7 * 1024)
#define MGS_MAXREPSIZE  (9 * 1024)

 /*
  * OSS threads constants:
  *
  * Given 8 as factor and 64 as base threads number
  *
  * example 1):
  * On 8-core server configured to 2 partitions, we will have
  * 64 + 8 * 4 = 96 threads for each partition, 192 total threads.
  *
  * example 2):
  * On 32-core machine configured to 4 partitions, we will have
  * 64 + 8 * 8 = 112 threads for each partition, so total threads number
  * will be 112 * 4 = 448.
  *
  * example 3):
  * On 64-core machine configured to 4 partitions, we will have
  * 64 + 16 * 8 = 192 threads for each partition, so total threads number
  * will be 192 * 4 = 768 which is above limit OSS_NTHRS_MAX(512), so we
  * cut off the value to OSS_NTHRS_MAX(512) / 4 which is 128 threads
  * for each partition.
  *
  * So we can see that with these constants, threads number wil be at the
  * similar level of old versions, unless the server has many cores.
  */
 /* depress threads factor for VM with small memory size */
#define OSS_THR_FACTOR		min_t(int, 8, \
				CFS_NUM_CACHEPAGES >> (28 - CFS_PAGE_SHIFT))
#define OSS_NTHRS_INIT		(PTLRPC_NTHRS_INIT + 1)
#define OSS_NTHRS_BASE		64
#define OSS_NTHRS_MAX		512

/* threads for handling "create" request */
#define OSS_CR_THR_FACTOR	1
#define OSS_CR_NTHRS_INIT	PTLRPC_NTHRS_INIT
#define OSS_CR_NTHRS_BASE	8
#define OSS_CR_NTHRS_MAX	64

#define OST_NBUFS       (64 * cfs_num_online_cpus())
#define OST_BUFSIZE     (8 * 1024)

/**
 * OST_MAXREQSIZE ~= 4768 bytes =
 * lustre_msg + obdo + 16 * obd_ioobj + 256 * niobuf_remote
 *
 * - single object with 16 pages is 512 bytes
 * - OST_MAXREQSIZE must be at least 1 page of cookies plus some spillover
 */
#define OST_MAXREQSIZE  (5 * 1024)
#define OST_MAXREPSIZE  (9 * 1024)

/* Macro to hide a typecast. */
#define ptlrpc_req_async_args(req) ((void *)&req->rq_async_args)

/**
 * Structure to single define portal connection.
 */
struct ptlrpc_connection {
        /** linkage for connections hash table */
        cfs_hlist_node_t        c_hash;
        /** Our own lnet nid for this connection */
        lnet_nid_t              c_self;
        /** Remote side nid for this connection */
        lnet_process_id_t       c_peer;
        /** UUID of the other side */
        struct obd_uuid         c_remote_uuid;
        /** reference counter for this connection */
        cfs_atomic_t            c_refcount;
};

/** Client definition for PortalRPC */
struct ptlrpc_client {
        /** What lnet portal does this client send messages to by default */
        __u32                   cli_request_portal;
        /** What portal do we expect replies on */
        __u32                   cli_reply_portal;
        /** Name of the client */
        char                   *cli_name;
};

/** state flags of requests */
/* XXX only ones left are those used by the bulk descs as well! */
#define PTL_RPC_FL_INTR      (1 << 0)  /* reply wait was interrupted by user */
#define PTL_RPC_FL_TIMEOUT   (1 << 7)  /* request timed out waiting for reply */

#define REQ_MAX_ACK_LOCKS 8

union ptlrpc_async_args {
        /**
         * Scratchpad for passing args to completion interpreter. Users
         * cast to the struct of their choosing, and CLASSERT that this is
         * big enough.  For _tons_ of context, OBD_ALLOC a struct and store
         * a pointer to it here.  The pointer_arg ensures this struct is at
         * least big enough for that.
         */
        void      *pointer_arg[11];
	__u64      space[7];
};

struct ptlrpc_request_set;
typedef int (*set_interpreter_func)(struct ptlrpc_request_set *, void *, int);
typedef int (*set_producer_func)(struct ptlrpc_request_set *, void *);

/**
 * Definition of request set structure.
 * Request set is a list of requests (not necessary to the same target) that
 * once populated with RPCs could be sent in parallel.
 * There are two kinds of request sets. General purpose and with dedicated
 * serving thread. Example of the latter is ptlrpcd set.
 * For general purpose sets once request set started sending it is impossible
 * to add new requests to such set.
 * Provides a way to call "completion callbacks" when all requests in the set
 * returned.
 */
struct ptlrpc_request_set {
	cfs_atomic_t          set_refcount;
	/** number of in queue requests */
	cfs_atomic_t          set_new_count;
	/** number of uncompleted requests */
	cfs_atomic_t          set_remaining;
	/** wait queue to wait on for request events */
	cfs_waitq_t           set_waitq;
	cfs_waitq_t          *set_wakeup_ptr;
	/** List of requests in the set */
	cfs_list_t            set_requests;
	/**
	 * List of completion callbacks to be called when the set is completed
	 * This is only used if \a set_interpret is NULL.
	 * Links struct ptlrpc_set_cbdata.
	 */
	cfs_list_t            set_cblist;
	/** Completion callback, if only one. */
	set_interpreter_func  set_interpret;
	/** opaq argument passed to completion \a set_interpret callback. */
	void                 *set_arg;
	/**
	 * Lock for \a set_new_requests manipulations
	 * locked so that any old caller can communicate requests to
	 * the set holder who can then fold them into the lock-free set
	 */
	cfs_spinlock_t        set_new_req_lock;
	/** List of new yet unsent requests. Only used with ptlrpcd now. */
	cfs_list_t            set_new_requests;

	/** rq_status of requests that have been freed already */
	int                   set_rc;
	/** Additional fields used by the flow control extension */
	/** Maximum number of RPCs in flight */
	int                   set_max_inflight;
	/** Callback function used to generate RPCs */
	set_producer_func     set_producer;
	/** opaq argument passed to the producer callback */
	void                 *set_producer_arg;
};

/**
 * Description of a single ptrlrpc_set callback
 */
struct ptlrpc_set_cbdata {
        /** List linkage item */
        cfs_list_t              psc_item;
        /** Pointer to interpreting function */
        set_interpreter_func    psc_interpret;
        /** Opaq argument to pass to the callback */
        void                   *psc_data;
};

struct ptlrpc_bulk_desc;
struct ptlrpc_service_part;

/**
 * ptlrpc callback & work item stuff
 */
struct ptlrpc_cb_id {
        void   (*cbid_fn)(lnet_event_t *ev);     /* specific callback fn */
        void    *cbid_arg;                      /* additional arg */
};

/** Maximum number of locks to fit into reply state */
#define RS_MAX_LOCKS 8
#define RS_DEBUG     0

/**
 * Structure to define reply state on the server
 * Reply state holds various reply message information. Also for "difficult"
 * replies (rep-ack case) we store the state after sending reply and wait
 * for the client to acknowledge the reception. In these cases locks could be
 * added to the state for replay/failover consistency guarantees.
 */
struct ptlrpc_reply_state {
        /** Callback description */
        struct ptlrpc_cb_id    rs_cb_id;
        /** Linkage for list of all reply states in a system */
        cfs_list_t             rs_list;
        /** Linkage for list of all reply states on same export */
        cfs_list_t             rs_exp_list;
        /** Linkage for list of all reply states for same obd */
        cfs_list_t             rs_obd_list;
#if RS_DEBUG
        cfs_list_t             rs_debug_list;
#endif
        /** A spinlock to protect the reply state flags */
        cfs_spinlock_t         rs_lock;
        /** Reply state flags */
        unsigned long          rs_difficult:1;     /* ACK/commit stuff */
        unsigned long          rs_no_ack:1;    /* no ACK, even for
                                                  difficult requests */
        unsigned long          rs_scheduled:1;     /* being handled? */
        unsigned long          rs_scheduled_ever:1;/* any schedule attempts? */
        unsigned long          rs_handled:1;  /* been handled yet? */
        unsigned long          rs_on_net:1;   /* reply_out_callback pending? */
        unsigned long          rs_prealloc:1; /* rs from prealloc list */
        unsigned long          rs_committed:1;/* the transaction was committed
                                                 and the rs was dispatched
                                                 by ptlrpc_commit_replies */
        /** Size of the state */
        int                    rs_size;
        /** opcode */
        __u32                  rs_opc;
        /** Transaction number */
        __u64                  rs_transno;
        /** xid */
        __u64                  rs_xid;
        struct obd_export     *rs_export;
	struct ptlrpc_service_part *rs_svcpt;
        /** Lnet metadata handle for the reply */
        lnet_handle_md_t       rs_md_h;
        cfs_atomic_t           rs_refcount;

        /** Context for the sevice thread */
        struct ptlrpc_svc_ctx *rs_svc_ctx;
        /** Reply buffer (actually sent to the client), encoded if needed */
        struct lustre_msg     *rs_repbuf;       /* wrapper */
        /** Size of the reply buffer */
        int                    rs_repbuf_len;   /* wrapper buf length */
        /** Size of the reply message */
        int                    rs_repdata_len;  /* wrapper msg length */
        /**
         * Actual reply message. Its content is encrupted (if needed) to
         * produce reply buffer for actual sending. In simple case
         * of no network encryption we jus set \a rs_repbuf to \a rs_msg
         */
        struct lustre_msg     *rs_msg;          /* reply message */

        /** Number of locks awaiting client ACK */
        int                    rs_nlocks;
        /** Handles of locks awaiting client reply ACK */
        struct lustre_handle   rs_locks[RS_MAX_LOCKS];
        /** Lock modes of locks in \a rs_locks */
        ldlm_mode_t            rs_modes[RS_MAX_LOCKS];
};

struct ptlrpc_thread;

/** RPC stages */
enum rq_phase {
        RQ_PHASE_NEW            = 0xebc0de00,
        RQ_PHASE_RPC            = 0xebc0de01,
        RQ_PHASE_BULK           = 0xebc0de02,
        RQ_PHASE_INTERPRET      = 0xebc0de03,
        RQ_PHASE_COMPLETE       = 0xebc0de04,
        RQ_PHASE_UNREGISTERING  = 0xebc0de05,
        RQ_PHASE_UNDEFINED      = 0xebc0de06
};

/** Type of request interpreter call-back */
typedef int (*ptlrpc_interpterer_t)(const struct lu_env *env,
                                    struct ptlrpc_request *req,
                                    void *arg, int rc);

/**
 * Definition of request pool structure.
 * The pool is used to store empty preallocated requests for the case
 * when we would actually need to send something without performing
 * any allocations (to avoid e.g. OOM).
 */
struct ptlrpc_request_pool {
        /** Locks the list */
        cfs_spinlock_t prp_lock;
        /** list of ptlrpc_request structs */
        cfs_list_t prp_req_list;
        /** Maximum message size that would fit into a rquest from this pool */
        int prp_rq_size;
        /** Function to allocate more requests for this pool */
        void (*prp_populate)(struct ptlrpc_request_pool *, int);
};

struct lu_context;
struct lu_env;

struct ldlm_lock;

/**
 * Basic request prioritization operations structure.
 * The whole idea is centered around locks and RPCs that might affect locks.
 * When a lock is contended we try to give priority to RPCs that might lead
 * to fastest release of that lock.
 * Currently only implemented for OSTs only in a way that makes all
 * IO and truncate RPCs that are coming from a locked region where a lock is
 * contended a priority over other requests.
 */
struct ptlrpc_hpreq_ops {
        /**
         * Check if the lock handle of the given lock is the same as
         * taken from the request.
         */
        int  (*hpreq_lock_match)(struct ptlrpc_request *, struct ldlm_lock *);
        /**
         * Check if the request is a high priority one.
         */
        int  (*hpreq_check)(struct ptlrpc_request *);
        /**
         * Called after the request has been handled.
         */
        void (*hpreq_fini)(struct ptlrpc_request *);
};

/**
 * Represents remote procedure call.
 *
 * This is a staple structure used by everybody wanting to send a request
 * in Lustre.
 */
struct ptlrpc_request {
	/* Request type: one of PTL_RPC_MSG_* */
	int rq_type;
	/** Result of request processing */
	int rq_status;
        /**
         * Linkage item through which this request is included into
         * sending/delayed lists on client and into rqbd list on server
         */
        cfs_list_t rq_list;
        /**
         * Server side list of incoming unserved requests sorted by arrival
         * time.  Traversed from time to time to notice about to expire
         * requests and sent back "early replies" to clients to let them
         * know server is alive and well, just very busy to service their
         * requests in time
         */
        cfs_list_t rq_timed_list;
        /** server-side history, used for debuging purposes. */
        cfs_list_t rq_history_list;
        /** server-side per-export list */
        cfs_list_t rq_exp_list;
        /** server-side hp handlers */
        struct ptlrpc_hpreq_ops *rq_ops;

	/** initial thread servicing this request */
	struct ptlrpc_thread *rq_svc_thread;

        /** history sequence # */
        __u64 rq_history_seq;
        /** the index of service's srv_at_array into which request is linked */
        time_t rq_at_index;
        /** Lock to protect request flags and some other important bits, like
         * rq_list
         */
        cfs_spinlock_t rq_lock;
        /** client-side flags are serialized by rq_lock */
	unsigned int rq_intr:1, rq_replied:1, rq_err:1,
                rq_timedout:1, rq_resend:1, rq_restart:1,
                /**
                 * when ->rq_replay is set, request is kept by the client even
                 * after server commits corresponding transaction. This is
                 * used for operations that require sequence of multiple
                 * requests to be replayed. The only example currently is file
                 * open/close. When last request in such a sequence is
                 * committed, ->rq_replay is cleared on all requests in the
                 * sequence.
                 */
                rq_replay:1,
                rq_no_resend:1, rq_waiting:1, rq_receiving_reply:1,
                rq_no_delay:1, rq_net_err:1, rq_wait_ctx:1,
                rq_early:1, rq_must_unlink:1,
                rq_fake:1,          /* this fake req */
                rq_memalloc:1,      /* req originated from "kswapd" */
                /* server-side flags */
                rq_packed_final:1,  /* packed final reply */
                rq_hp:1,            /* high priority RPC */
                rq_at_linked:1,     /* link into service's srv_at_array */
                rq_reply_truncate:1,
                rq_committed:1,
                /* whether the "rq_set" is a valid one */
                rq_invalid_rqset:1,
		rq_generation_set:1,
		/* do not resend request on -EINPROGRESS */
		rq_no_retry_einprogress:1;

	unsigned int rq_nr_resend;

        enum rq_phase rq_phase; /* one of RQ_PHASE_* */
        enum rq_phase rq_next_phase; /* one of RQ_PHASE_* to be used next */
        cfs_atomic_t rq_refcount;/* client-side refcount for SENT race,
                                    server-side refcounf for multiple replies */

	/** Portal to which this request would be sent */
	short rq_request_portal;  /* XXX FIXME bug 249 */
	/** Portal where to wait for reply and where reply would be sent */
	short rq_reply_portal;    /* XXX FIXME bug 249 */

        /**
         * client-side:
         * !rq_truncate : # reply bytes actually received,
         *  rq_truncate : required repbuf_len for resend
         */
        int rq_nob_received;
        /** Request length */
        int rq_reqlen;
        /** Reply length */
        int rq_replen;
	/** Request message - what client sent */
	struct lustre_msg *rq_reqmsg;
        /** Reply message - server response */
        struct lustre_msg *rq_repmsg;
        /** Transaction number */
        __u64 rq_transno;
        /** xid */
        __u64 rq_xid;
        /**
         * List item to for replay list. Not yet commited requests get linked
         * there.
         * Also see \a rq_replay comment above.
         */
        cfs_list_t rq_replay_list;

        /**
         * security and encryption data
         * @{ */
        struct ptlrpc_cli_ctx   *rq_cli_ctx;     /**< client's half ctx */
        struct ptlrpc_svc_ctx   *rq_svc_ctx;     /**< server's half ctx */
        cfs_list_t               rq_ctx_chain;   /**< link to waited ctx */

        struct sptlrpc_flavor    rq_flvr;        /**< for client & server */
        enum lustre_sec_part     rq_sp_from;

	/* client/server security flags */
	unsigned int
                                 rq_ctx_init:1,      /* context initiation */
                                 rq_ctx_fini:1,      /* context destroy */
                                 rq_bulk_read:1,     /* request bulk read */
                                 rq_bulk_write:1,    /* request bulk write */
                                 /* server authentication flags */
                                 rq_auth_gss:1,      /* authenticated by gss */
                                 rq_auth_remote:1,   /* authed as remote user */
                                 rq_auth_usr_root:1, /* authed as root */
                                 rq_auth_usr_mdt:1,  /* authed as mdt */
                                 rq_auth_usr_ost:1,  /* authed as ost */
                                 /* security tfm flags */
                                 rq_pack_udesc:1,
                                 rq_pack_bulk:1,
                                 /* doesn't expect reply FIXME */
                                 rq_no_reply:1,
                                 rq_pill_init:1;     /* pill initialized */

        uid_t                    rq_auth_uid;        /* authed uid */
        uid_t                    rq_auth_mapped_uid; /* authed uid mapped to */

        /* (server side), pointed directly into req buffer */
        struct ptlrpc_user_desc *rq_user_desc;

        /* various buffer pointers */
        struct lustre_msg       *rq_reqbuf;      /* req wrapper */
	char                    *rq_repbuf;      /* rep buffer */
	struct lustre_msg       *rq_repdata;     /* rep wrapper msg */
	struct lustre_msg       *rq_clrbuf;      /* only in priv mode */
        int                      rq_reqbuf_len;  /* req wrapper buf len */
        int                      rq_reqdata_len; /* req wrapper msg len */
        int                      rq_repbuf_len;  /* rep buffer len */
        int                      rq_repdata_len; /* rep wrapper msg len */
        int                      rq_clrbuf_len;  /* only in priv mode */
        int                      rq_clrdata_len; /* only in priv mode */

	/** early replies go to offset 0, regular replies go after that */
	unsigned int             rq_reply_off;

        /** @} */

        /** Fields that help to see if request and reply were swabbed or not */
        __u32 rq_req_swab_mask;
        __u32 rq_rep_swab_mask;

        /** What was import generation when this request was sent */
        int rq_import_generation;
        enum lustre_imp_state rq_send_state;

        /** how many early replies (for stats) */
        int rq_early_count;

        /** client+server request */
        lnet_handle_md_t     rq_req_md_h;
        struct ptlrpc_cb_id  rq_req_cbid;
        /** optional time limit for send attempts */
        cfs_duration_t       rq_delay_limit;
        /** time request was first queued */
        cfs_time_t           rq_queued_time;

        /* server-side... */
        /** request arrival time */
        struct timeval       rq_arrival_time;
        /** separated reply state */
        struct ptlrpc_reply_state *rq_reply_state;
        /** incoming request buffer */
        struct ptlrpc_request_buffer_desc *rq_rqbd;

        /** client-only incoming reply */
        lnet_handle_md_t     rq_reply_md_h;
        cfs_waitq_t          rq_reply_waitq;
        struct ptlrpc_cb_id  rq_reply_cbid;

        /** our LNet NID */
        lnet_nid_t           rq_self;
        /** Peer description (the other side) */
        lnet_process_id_t    rq_peer;
        /** Server-side, export on which request was received */
        struct obd_export   *rq_export;
        /** Client side, import where request is being sent */
        struct obd_import   *rq_import;

        /** Replay callback, called after request is replayed at recovery */
        void (*rq_replay_cb)(struct ptlrpc_request *);
        /**
         * Commit callback, called when request is committed and about to be
         * freed.
         */
        void (*rq_commit_cb)(struct ptlrpc_request *);
        /** Opaq data for replay and commit callbacks. */
        void  *rq_cb_data;

        /** For bulk requests on client only: bulk descriptor */
        struct ptlrpc_bulk_desc *rq_bulk;

        /** client outgoing req */
        /**
         * when request/reply sent (secs), or time when request should be sent
         */
        time_t rq_sent;
        /** time for request really sent out */
        time_t rq_real_sent;

        /** when request must finish. volatile
         * so that servers' early reply updates to the deadline aren't
         * kept in per-cpu cache */
        volatile time_t rq_deadline;
        /** when req reply unlink must finish. */
        time_t rq_reply_deadline;
        /** when req bulk unlink must finish. */
        time_t rq_bulk_deadline;
        /**
         * service time estimate (secs) 
         * If the requestsis not served by this time, it is marked as timed out.
         */
        int    rq_timeout;

        /** Multi-rpc bits */
        /** Per-request waitq introduced by bug 21938 for recovery waiting */
        cfs_waitq_t rq_set_waitq;
	/** Link item for request set lists */
	cfs_list_t  rq_set_chain;
        /** Link back to the request set */
        struct ptlrpc_request_set *rq_set;
        /** Async completion handler, called when reply is received */
        ptlrpc_interpterer_t rq_interpret_reply;
        /** Async completion context */
        union ptlrpc_async_args rq_async_args;

        /** Pool if request is from preallocated list */
        struct ptlrpc_request_pool *rq_pool;

        struct lu_context           rq_session;
        struct lu_context           rq_recov_session;

        /** request format description */
        struct req_capsule          rq_pill;
};

/**
 * Call completion handler for rpc if any, return it's status or original
 * rc if there was no handler defined for this request.
 */
static inline int ptlrpc_req_interpret(const struct lu_env *env,
                                       struct ptlrpc_request *req, int rc)
{
        if (req->rq_interpret_reply != NULL) {
                req->rq_status = req->rq_interpret_reply(env, req,
                                                         &req->rq_async_args,
                                                         rc);
                return req->rq_status;
        }
        return rc;
}

/**
 * Returns 1 if request buffer at offset \a index was already swabbed
 */
static inline int lustre_req_swabbed(struct ptlrpc_request *req, int index)
{
        LASSERT(index < sizeof(req->rq_req_swab_mask) * 8);
        return req->rq_req_swab_mask & (1 << index);
}

/**
 * Returns 1 if request reply buffer at offset \a index was already swabbed
 */
static inline int lustre_rep_swabbed(struct ptlrpc_request *req, int index)
{
        LASSERT(index < sizeof(req->rq_rep_swab_mask) * 8);
        return req->rq_rep_swab_mask & (1 << index);
}

/**
 * Returns 1 if request needs to be swabbed into local cpu byteorder
 */
static inline int ptlrpc_req_need_swab(struct ptlrpc_request *req)
{
        return lustre_req_swabbed(req, MSG_PTLRPC_HEADER_OFF);
}

/**
 * Returns 1 if request reply needs to be swabbed into local cpu byteorder
 */
static inline int ptlrpc_rep_need_swab(struct ptlrpc_request *req)
{
        return lustre_rep_swabbed(req, MSG_PTLRPC_HEADER_OFF);
}

/**
 * Mark request buffer at offset \a index that it was already swabbed
 */
static inline void lustre_set_req_swabbed(struct ptlrpc_request *req, int index)
{
        LASSERT(index < sizeof(req->rq_req_swab_mask) * 8);
        LASSERT((req->rq_req_swab_mask & (1 << index)) == 0);
        req->rq_req_swab_mask |= 1 << index;
}

/**
 * Mark request reply buffer at offset \a index that it was already swabbed
 */
static inline void lustre_set_rep_swabbed(struct ptlrpc_request *req, int index)
{
        LASSERT(index < sizeof(req->rq_rep_swab_mask) * 8);
        LASSERT((req->rq_rep_swab_mask & (1 << index)) == 0);
        req->rq_rep_swab_mask |= 1 << index;
}

/**
 * Convert numerical request phase value \a phase into text string description
 */
static inline const char *
ptlrpc_phase2str(enum rq_phase phase)
{
        switch (phase) {
        case RQ_PHASE_NEW:
                return "New";
        case RQ_PHASE_RPC:
                return "Rpc";
        case RQ_PHASE_BULK:
                return "Bulk";
        case RQ_PHASE_INTERPRET:
                return "Interpret";
        case RQ_PHASE_COMPLETE:
                return "Complete";
        case RQ_PHASE_UNREGISTERING:
                return "Unregistering";
        default:
                return "?Phase?";
        }
}

/**
 * Convert numerical request phase of the request \a req into text stringi
 * description
 */
static inline const char *
ptlrpc_rqphase2str(struct ptlrpc_request *req)
{
        return ptlrpc_phase2str(req->rq_phase);
}

/**
 * Debugging functions and helpers to print request structure into debug log
 * @{
 */ 
/* Spare the preprocessor, spoil the bugs. */
#define FLAG(field, str) (field ? str : "")

/** Convert bit flags into a string */
#define DEBUG_REQ_FLAGS(req)                                                    \
        ptlrpc_rqphase2str(req),                                                \
        FLAG(req->rq_intr, "I"), FLAG(req->rq_replied, "R"),                    \
        FLAG(req->rq_err, "E"),                                                 \
        FLAG(req->rq_timedout, "X") /* eXpired */, FLAG(req->rq_resend, "S"),   \
        FLAG(req->rq_restart, "T"), FLAG(req->rq_replay, "P"),                  \
        FLAG(req->rq_no_resend, "N"),                                           \
        FLAG(req->rq_waiting, "W"),                                             \
        FLAG(req->rq_wait_ctx, "C"), FLAG(req->rq_hp, "H"),                     \
        FLAG(req->rq_committed, "M")

#define REQ_FLAGS_FMT "%s:%s%s%s%s%s%s%s%s%s%s%s%s"

void _debug_req(struct ptlrpc_request *req,
                struct libcfs_debug_msg_data *data, const char *fmt, ...)
        __attribute__ ((format (printf, 3, 4)));

/**
 * Helper that decides if we need to print request accordig to current debug
 * level settings
 */
#define debug_req(msgdata, mask, cdls, req, fmt, a...)                        \
do {                                                                          \
        CFS_CHECK_STACK(msgdata, mask, cdls);                                 \
                                                                              \
        if (((mask) & D_CANTMASK) != 0 ||                                     \
            ((libcfs_debug & (mask)) != 0 &&                                  \
             (libcfs_subsystem_debug & DEBUG_SUBSYSTEM) != 0))                \
                _debug_req((req), msgdata, fmt, ##a);                         \
} while(0)

/**
 * This is the debug print function you need to use to print request sturucture
 * content into lustre debug log.
 * for most callers (level is a constant) this is resolved at compile time */
#define DEBUG_REQ(level, req, fmt, args...)                                   \
do {                                                                          \
        if ((level) & (D_ERROR | D_WARNING)) {                                \
                static cfs_debug_limit_state_t cdls;                          \
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, level, &cdls);            \
                debug_req(&msgdata, level, &cdls, req, "@@@ "fmt" ", ## args);\
        } else {                                                              \
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, level, NULL);             \
                debug_req(&msgdata, level, NULL, req, "@@@ "fmt" ", ## args); \
        }                                                                     \
} while (0)
/** @} */

/**
 * Structure that defines a single page of a bulk transfer
 */
struct ptlrpc_bulk_page {
        /** Linkage to list of pages in a bulk */
        cfs_list_t       bp_link;
        /**
         * Number of bytes in a page to transfer starting from \a bp_pageoffset
         */
        int              bp_buflen;
        /** offset within a page */
        int              bp_pageoffset;
        /** The page itself */
        struct page     *bp_page;
};

#define BULK_GET_SOURCE   0
#define BULK_PUT_SINK     1
#define BULK_GET_SINK     2
#define BULK_PUT_SOURCE   3

/**
 * Definition of buk descriptor.
 * Bulks are special "Two phase" RPCs where initial request message
 * is sent first and it is followed bt a transfer (o receiving) of a large
 * amount of data to be settled into pages referenced from the bulk descriptors.
 * Bulks transfers (the actual data following the small requests) are done
 * on separate LNet portals.
 * In lustre we use bulk transfers for READ and WRITE transfers from/to OSTs.
 *  Another user is readpage for MDT.
 */
struct ptlrpc_bulk_desc {
        /** completed successfully */
        unsigned long bd_success:1;
        /** accessible to the network (network io potentially in progress) */
        unsigned long bd_network_rw:1;
        /** {put,get}{source,sink} */
        unsigned long bd_type:2;
        /** client side */
        unsigned long bd_registered:1;
        /** For serialization with callback */
        cfs_spinlock_t bd_lock;
        /** Import generation when request for this bulk was sent */
        int bd_import_generation;
        /** Server side - export this bulk created for */
        struct obd_export *bd_export;
        /** Client side - import this bulk was sent on */
        struct obd_import *bd_import;
        /** LNet portal for this bulk */
        __u32 bd_portal;
        /** Back pointer to the request */
        struct ptlrpc_request *bd_req;
        cfs_waitq_t            bd_waitq;        /* server side only WQ */
        int                    bd_iov_count;    /* # entries in bd_iov */
        int                    bd_max_iov;      /* allocated size of bd_iov */
        int                    bd_nob;          /* # bytes covered */
        int                    bd_nob_transferred; /* # bytes GOT/PUT */

        __u64                  bd_last_xid;

        struct ptlrpc_cb_id    bd_cbid;         /* network callback info */
        lnet_handle_md_t       bd_md_h;         /* associated MD */
        lnet_nid_t             bd_sender;       /* stash event::sender */

#if defined(__KERNEL__)
        /*
         * encrypt iov, size is either 0 or bd_iov_count.
         */
        lnet_kiov_t           *bd_enc_iov;

        lnet_kiov_t            bd_iov[0];
#else
        lnet_md_iovec_t        bd_iov[0];
#endif
};

enum {
        SVC_STOPPED     = 1 << 0,
        SVC_STOPPING    = 1 << 1,
        SVC_STARTING    = 1 << 2,
        SVC_RUNNING     = 1 << 3,
        SVC_EVENT       = 1 << 4,
        SVC_SIGNAL      = 1 << 5,
};

#define PTLRPC_THR_NAME_LEN		32
/**
 * Definition of server service thread structure
 */
struct ptlrpc_thread {
        /**
         * List of active threads in svc->srv_threads
         */
        cfs_list_t t_link;
        /**
         * thread-private data (preallocated memory)
         */
        void *t_data;
        __u32 t_flags;
        /**
         * service thread index, from ptlrpc_start_threads
         */
        unsigned int t_id;
        /**
         * service thread pid
         */
        pid_t t_pid; 
        /**
         * put watchdog in the structure per thread b=14840
         */
        struct lc_watchdog *t_watchdog;
        /**
         * the svc this thread belonged to b=18582
         */
	struct ptlrpc_service_part	*t_svcpt;
	cfs_waitq_t			t_ctl_waitq;
	struct lu_env			*t_env;
	char				t_name[PTLRPC_THR_NAME_LEN];
};

static inline int thread_is_init(struct ptlrpc_thread *thread)
{
	return thread->t_flags == 0;
}

static inline int thread_is_stopped(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_STOPPED);
}

static inline int thread_is_stopping(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_STOPPING);
}

static inline int thread_is_starting(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_STARTING);
}

static inline int thread_is_running(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_RUNNING);
}

static inline int thread_is_event(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_EVENT);
}

static inline int thread_is_signal(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_SIGNAL);
}

static inline void thread_clear_flags(struct ptlrpc_thread *thread, __u32 flags)
{
        thread->t_flags &= ~flags;
}

static inline void thread_set_flags(struct ptlrpc_thread *thread, __u32 flags)
{
        thread->t_flags = flags;
}

static inline void thread_add_flags(struct ptlrpc_thread *thread, __u32 flags)
{
        thread->t_flags |= flags;
}

static inline int thread_test_and_clear_flags(struct ptlrpc_thread *thread,
                                              __u32 flags)
{
        if (thread->t_flags & flags) {
                thread->t_flags &= ~flags;
                return 1;
        }
        return 0;
}

/**
 * Request buffer descriptor structure.
 * This is a structure that contains one posted request buffer for service.
 * Once data land into a buffer, event callback creates actual request and
 * notifies wakes one of the service threads to process new incoming request.
 * More than one request can fit into the buffer.
 */
struct ptlrpc_request_buffer_desc {
        /** Link item for rqbds on a service */
        cfs_list_t             rqbd_list;
        /** History of requests for this buffer */
        cfs_list_t             rqbd_reqs;
        /** Back pointer to service for which this buffer is registered */
	struct ptlrpc_service_part *rqbd_svcpt;
        /** LNet descriptor */
        lnet_handle_md_t       rqbd_md_h;
        int                    rqbd_refcount;
        /** The buffer itself */
        char                  *rqbd_buffer;
        struct ptlrpc_cb_id    rqbd_cbid;
        /**
         * This "embedded" request structure is only used for the
         * last request to fit into the buffer
         */
        struct ptlrpc_request  rqbd_req;
};

typedef int  (*svc_handler_t)(struct ptlrpc_request *req);

struct ptlrpc_service_ops {
	/**
	 * if non-NULL called during thread creation (ptlrpc_start_thread())
	 * to initialize service specific per-thread state.
	 */
	int		(*so_thr_init)(struct ptlrpc_thread *thr);
	/**
	 * if non-NULL called during thread shutdown (ptlrpc_main()) to
	 * destruct state created by ->srv_init().
	 */
	void		(*so_thr_done)(struct ptlrpc_thread *thr);
	/**
	 * Handler function for incoming requests for this service
	 */
	int		(*so_req_handler)(struct ptlrpc_request *req);
	/**
	 * function to determine priority of the request, it's called
	 * on every new request
	 */
	int		(*so_hpreq_handler)(struct ptlrpc_request *);
	/**
	 * service-specific print fn
	 */
	void		(*so_req_printer)(void *, struct ptlrpc_request *);
};

#ifndef __cfs_cacheline_aligned
/* NB: put it here for reducing patche dependence */
# define __cfs_cacheline_aligned
#endif

/**
 * How many high priority requests to serve before serving one normal
 * priority request
 */
#define PTLRPC_SVC_HP_RATIO 10

/**
 * Definition of PortalRPC service.
 * The service is listening on a particular portal (like tcp port)
 * and perform actions for a specific server like IO service for OST
 * or general metadata service for MDS.
 */
struct ptlrpc_service {
	/** serialize /proc operations */
	cfs_spinlock_t			srv_lock;
        /** most often accessed fields */
        /** chain thru all services */
        cfs_list_t                      srv_list;
	/** service operations table */
	struct ptlrpc_service_ops	srv_ops;
        /** only statically allocated strings here; we don't clean them */
        char                           *srv_name;
        /** only statically allocated strings here; we don't clean them */
        char                           *srv_thread_name;
        /** service thread list */
        cfs_list_t                      srv_threads;
	/** threads # should be created for each partition on initializing */
	int				srv_nthrs_cpt_init;
	/** limit of threads number for each partition */
	int				srv_nthrs_cpt_limit;
        /** Root of /proc dir tree for this service */
        cfs_proc_dir_entry_t           *srv_procroot;
        /** Pointer to statistic data for this service */
        struct lprocfs_stats           *srv_stats;
        /** # hp per lp reqs to handle */
        int                             srv_hpreq_ratio;
        /** biggest request to receive */
        int                             srv_max_req_size;
        /** biggest reply to send */
        int                             srv_max_reply_size;
        /** size of individual buffers */
        int                             srv_buf_size;
        /** # buffers to allocate in 1 group */
        int                             srv_nbuf_per_group;
        /** Local portal on which to receive requests */
        __u32                           srv_req_portal;
        /** Portal on the client to send replies to */
        __u32                           srv_rep_portal;
        /**
         * Tags for lu_context associated with this thread, see struct
         * lu_context.
         */
        __u32                           srv_ctx_tags;
        /** soft watchdog timeout multiplier */
        int                             srv_watchdog_factor;
        /** under unregister_service */
        unsigned                        srv_is_stopping:1;

	/** max # request buffers in history per partition */
	int				srv_hist_nrqbds_cpt_max;
	/** number of CPTs this service bound on */
	int				srv_ncpts;
	/** CPTs array this service bound on */
	__u32				*srv_cpts;
	/** 2^srv_cptab_bits >= cfs_cpt_numbert(srv_cptable) */
	int				srv_cpt_bits;
	/** CPT table this service is running over */
	struct cfs_cpt_table		*srv_cptable;
	/**
	 * partition data for ptlrpc service
	 */
	struct ptlrpc_service_part	*srv_parts[0];
};

/**
 * Definition of PortalRPC service partition data.
 * Although a service only has one instance of it right now, but we
 * will have multiple instances very soon (instance per CPT).
 *
 * it has four locks:
 * \a scp_lock
 *    serialize operations on rqbd and requests waiting for preprocess
 * \a scp_req_lock
 *    serialize operations active requests sent to this portal
 * \a scp_at_lock
 *    serialize adaptive timeout stuff
 * \a scp_rep_lock
 *    serialize operations on RS list (reply states)
 *
 * We don't have any use-case to take two or more locks at the same time
 * for now, so there is no lock order issue.
 */
struct ptlrpc_service_part {
	/** back reference to owner */
	struct ptlrpc_service		*scp_service __cfs_cacheline_aligned;
	/* CPT id, reserved */
	int				scp_cpt;
	/** always increasing number */
	int				scp_thr_nextid;
	/** # of starting threads */
	int				scp_nthrs_starting;
	/** # of stopping threads, reserved for shrinking threads */
	int				scp_nthrs_stopping;
	/** # running threads */
	int				scp_nthrs_running;
	/** service threads list */
	cfs_list_t			scp_threads;

	/**
	 * serialize the following fields, used for protecting
	 * rqbd list and incoming requests waiting for preprocess,
	 * threads starting & stopping are also protected by this lock.
	 */
	cfs_spinlock_t			scp_lock  __cfs_cacheline_aligned;
	/** total # req buffer descs allocated */
	int				scp_nrqbds_total;
	/** # posted request buffers for receiving */
	int				scp_nrqbds_posted;
	/** # incoming reqs */
	int				scp_nreqs_incoming;
	/** request buffers to be reposted */
	cfs_list_t			scp_rqbd_idle;
	/** req buffers receiving */
	cfs_list_t			scp_rqbd_posted;
	/** incoming reqs */
	cfs_list_t			scp_req_incoming;
	/** timeout before re-posting reqs, in tick */
	cfs_duration_t			scp_rqbd_timeout;
	/**
	 * all threads sleep on this. This wait-queue is signalled when new
	 * incoming request arrives and when difficult reply has to be handled.
	 */
	cfs_waitq_t			scp_waitq;

	/** request history */
	cfs_list_t			scp_hist_reqs;
	/** request buffer history */
	cfs_list_t			scp_hist_rqbds;
	/** # request buffers in history */
	int				scp_hist_nrqbds;
	/** sequence number for request */
	__u64				scp_hist_seq;
	/** highest seq culled from history */
	__u64				scp_hist_seq_culled;

	/**
	 * serialize the following fields, used for processing requests
	 * sent to this portal
	 */
	cfs_spinlock_t			scp_req_lock __cfs_cacheline_aligned;
	/** # reqs in either of the queues below */
	/** reqs waiting for service */
	cfs_list_t			scp_req_pending;
	/** high priority queue */
	cfs_list_t			scp_hreq_pending;
	/** # reqs being served */
	int				scp_nreqs_active;
	/** # HPreqs being served */
	int				scp_nhreqs_active;
	/** # hp requests handled */
	int				scp_hreq_count;

	/** AT stuff */
	/** @{ */
	/**
	 * serialize the following fields, used for changes on
	 * adaptive timeout
	 */
	cfs_spinlock_t			scp_at_lock __cfs_cacheline_aligned;
	/** estimated rpc service time */
	struct adaptive_timeout		scp_at_estimate;
	/** reqs waiting for replies */
	struct ptlrpc_at_array		scp_at_array;
	/** early reply timer */
	cfs_timer_t			scp_at_timer;
	/** debug */
	cfs_time_t			scp_at_checktime;
	/** check early replies */
	unsigned			scp_at_check;
	/** @} */

	/**
	 * serialize the following fields, used for processing
	 * replies for this portal
	 */
	cfs_spinlock_t			scp_rep_lock __cfs_cacheline_aligned;
	/** all the active replies */
	cfs_list_t			scp_rep_active;
#ifndef __KERNEL__
	/** replies waiting for service */
	cfs_list_t			scp_rep_queue;
#endif
	/** List of free reply_states */
	cfs_list_t			scp_rep_idle;
	/** waitq to run, when adding stuff to srv_free_rs_list */
	cfs_waitq_t			scp_rep_waitq;
	/** # 'difficult' replies */
	cfs_atomic_t			scp_nreps_difficult;
};

#define ptlrpc_service_for_each_part(part, i, svc)			\
	for (i = 0;							\
	     i < (svc)->srv_ncpts &&					\
	     (svc)->srv_parts != NULL &&				\
	     ((part) = (svc)->srv_parts[i]) != NULL; i++)

/**
 * Declaration of ptlrpcd control structure
 */
struct ptlrpcd_ctl {
        /**
         * Ptlrpc thread control flags (LIOD_START, LIOD_STOP, LIOD_FORCE)
         */
        unsigned long               pc_flags;
        /**
         * Thread lock protecting structure fields.
         */
        cfs_spinlock_t              pc_lock;
        /**
         * Start completion.
         */
        cfs_completion_t            pc_starting;
        /**
         * Stop completion.
         */
        cfs_completion_t            pc_finishing;
        /**
         * Thread requests set.
         */
        struct ptlrpc_request_set  *pc_set;
        /**
         * Thread name used in cfs_daemonize()
         */
        char                        pc_name[16];
        /**
         * Environment for request interpreters to run in.
         */
        struct lu_env               pc_env;
        /**
         * Index of ptlrpcd thread in the array.
         */
        int                         pc_index;
        /**
         * Number of the ptlrpcd's partners.
         */
        int                         pc_npartners;
        /**
         * Pointer to the array of partners' ptlrpcd_ctl structure.
         */
        struct ptlrpcd_ctl        **pc_partners;
        /**
         * Record the partner index to be processed next.
         */
        int                         pc_cursor;
#ifndef __KERNEL__
        /**
         * Async rpcs flag to make sure that ptlrpcd_check() is called only
         * once.
         */
        int                         pc_recurred;
        /**
         * Currently not used.
         */
        void                       *pc_callback;
        /**
         * User-space async rpcs callback.
         */
        void                       *pc_wait_callback;
        /**
         * User-space check idle rpcs callback.
         */
        void                       *pc_idle_callback;
#endif
};

/* Bits for pc_flags */
enum ptlrpcd_ctl_flags {
        /**
         * Ptlrpc thread start flag.
         */
        LIOD_START       = 1 << 0,
        /**
         * Ptlrpc thread stop flag.
         */
        LIOD_STOP        = 1 << 1,
        /**
         * Ptlrpc thread force flag (only stop force so far).
         * This will cause aborting any inflight rpcs handled
         * by thread if LIOD_STOP is specified.
         */
        LIOD_FORCE       = 1 << 2,
        /**
         * This is a recovery ptlrpc thread.
         */
        LIOD_RECOVERY    = 1 << 3,
        /**
         * The ptlrpcd is bound to some CPU core.
         */
        LIOD_BIND        = 1 << 4,
};

/* ptlrpc/events.c */
extern lnet_handle_eq_t ptlrpc_eq_h;
extern int ptlrpc_uuid_to_peer(struct obd_uuid *uuid,
                               lnet_process_id_t *peer, lnet_nid_t *self);
/**
 * These callbacks are invoked by LNet when something happened to
 * underlying buffer
 * @{
 */
extern void request_out_callback(lnet_event_t *ev);
extern void reply_in_callback(lnet_event_t *ev);
extern void client_bulk_callback(lnet_event_t *ev);
extern void request_in_callback(lnet_event_t *ev);
extern void reply_out_callback(lnet_event_t *ev);
#ifdef HAVE_SERVER_SUPPORT
extern void server_bulk_callback(lnet_event_t *ev);
#endif
/** @} */

/* ptlrpc/connection.c */
struct ptlrpc_connection *ptlrpc_connection_get(lnet_process_id_t peer,
                                                lnet_nid_t self,
                                                struct obd_uuid *uuid);
int ptlrpc_connection_put(struct ptlrpc_connection *c);
struct ptlrpc_connection *ptlrpc_connection_addref(struct ptlrpc_connection *);
int ptlrpc_connection_init(void);
void ptlrpc_connection_fini(void);
extern lnet_pid_t ptl_get_pid(void);

/* ptlrpc/niobuf.c */
/**
 * Actual interfacing with LNet to put/get/register/unregister stuff
 * @{
 */
#ifdef HAVE_SERVER_SUPPORT
struct ptlrpc_bulk_desc *ptlrpc_prep_bulk_exp(struct ptlrpc_request *req,
                                              int npages, int type, int portal);
int ptlrpc_start_bulk_transfer(struct ptlrpc_bulk_desc *desc);
void ptlrpc_abort_bulk(struct ptlrpc_bulk_desc *desc);

static inline int ptlrpc_server_bulk_active(struct ptlrpc_bulk_desc *desc)
{
        int rc;

        LASSERT(desc != NULL);

        cfs_spin_lock(&desc->bd_lock);
        rc = desc->bd_network_rw;
        cfs_spin_unlock(&desc->bd_lock);
        return rc;
}
#endif

int ptlrpc_register_bulk(struct ptlrpc_request *req);
int ptlrpc_unregister_bulk(struct ptlrpc_request *req, int async);

static inline int ptlrpc_client_bulk_active(struct ptlrpc_request *req)
{
        struct ptlrpc_bulk_desc *desc = req->rq_bulk;
        int                      rc;

        LASSERT(req != NULL);

        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_BULK_UNLINK) &&
            req->rq_bulk_deadline > cfs_time_current_sec())
                return 1;

        if (!desc)
                return 0;

        cfs_spin_lock(&desc->bd_lock);
        rc = desc->bd_network_rw;
        cfs_spin_unlock(&desc->bd_lock);
        return rc;
}

#define PTLRPC_REPLY_MAYBE_DIFFICULT 0x01
#define PTLRPC_REPLY_EARLY           0x02
int ptlrpc_send_reply(struct ptlrpc_request *req, int flags);
int ptlrpc_reply(struct ptlrpc_request *req);
int ptlrpc_send_error(struct ptlrpc_request *req, int difficult);
int ptlrpc_error(struct ptlrpc_request *req);
void ptlrpc_resend_req(struct ptlrpc_request *request);
int ptlrpc_at_get_net_latency(struct ptlrpc_request *req);
int ptl_send_rpc(struct ptlrpc_request *request, int noreply);
int ptlrpc_register_rqbd(struct ptlrpc_request_buffer_desc *rqbd);
/** @} */

/* ptlrpc/client.c */
/**
 * Client-side portals API. Everything to send requests, receive replies,
 * request queues, request management, etc.
 * @{
 */
void ptlrpc_init_client(int req_portal, int rep_portal, char *name,
                        struct ptlrpc_client *);
void ptlrpc_cleanup_client(struct obd_import *imp);
struct ptlrpc_connection *ptlrpc_uuid_to_connection(struct obd_uuid *uuid);

int ptlrpc_queue_wait(struct ptlrpc_request *req);
int ptlrpc_replay_req(struct ptlrpc_request *req);
int ptlrpc_unregister_reply(struct ptlrpc_request *req, int async);
void ptlrpc_restart_req(struct ptlrpc_request *req);
void ptlrpc_abort_inflight(struct obd_import *imp);
void ptlrpc_cleanup_imp(struct obd_import *imp);
void ptlrpc_abort_set(struct ptlrpc_request_set *set);

struct ptlrpc_request_set *ptlrpc_prep_set(void);
struct ptlrpc_request_set *ptlrpc_prep_fcset(int max, set_producer_func func,
					     void *arg);
int ptlrpc_set_add_cb(struct ptlrpc_request_set *set,
                      set_interpreter_func fn, void *data);
int ptlrpc_set_next_timeout(struct ptlrpc_request_set *);
int ptlrpc_check_set(const struct lu_env *env, struct ptlrpc_request_set *set);
int ptlrpc_set_wait(struct ptlrpc_request_set *);
int ptlrpc_expired_set(void *data);
void ptlrpc_interrupted_set(void *data);
void ptlrpc_mark_interrupted(struct ptlrpc_request *req);
void ptlrpc_set_destroy(struct ptlrpc_request_set *);
void ptlrpc_set_add_req(struct ptlrpc_request_set *, struct ptlrpc_request *);
void ptlrpc_set_add_new_req(struct ptlrpcd_ctl *pc,
                            struct ptlrpc_request *req);

void ptlrpc_free_rq_pool(struct ptlrpc_request_pool *pool);
void ptlrpc_add_rqs_to_pool(struct ptlrpc_request_pool *pool, int num_rq);

struct ptlrpc_request_pool *
ptlrpc_init_rq_pool(int, int,
                    void (*populate_pool)(struct ptlrpc_request_pool *, int));

void ptlrpc_at_set_req_timeout(struct ptlrpc_request *req);
struct ptlrpc_request *ptlrpc_request_alloc(struct obd_import *imp,
                                            const struct req_format *format);
struct ptlrpc_request *ptlrpc_request_alloc_pool(struct obd_import *imp,
                                            struct ptlrpc_request_pool *,
                                            const struct req_format *format);
void ptlrpc_request_free(struct ptlrpc_request *request);
int ptlrpc_request_pack(struct ptlrpc_request *request,
                        __u32 version, int opcode);
struct ptlrpc_request *ptlrpc_request_alloc_pack(struct obd_import *imp,
                                                const struct req_format *format,
                                                __u32 version, int opcode);
int ptlrpc_request_bufs_pack(struct ptlrpc_request *request,
                             __u32 version, int opcode, char **bufs,
                             struct ptlrpc_cli_ctx *ctx);
struct ptlrpc_request *ptlrpc_prep_fakereq(struct obd_import *imp,
                                           unsigned int timeout,
                                           ptlrpc_interpterer_t interpreter);
void ptlrpc_fakereq_finished(struct ptlrpc_request *req);

struct ptlrpc_request *ptlrpc_prep_req(struct obd_import *imp, __u32 version,
                                       int opcode, int count, __u32 *lengths,
                                       char **bufs);
struct ptlrpc_request *ptlrpc_prep_req_pool(struct obd_import *imp,
                                             __u32 version, int opcode,
                                            int count, __u32 *lengths, char **bufs,
                                            struct ptlrpc_request_pool *pool);
void ptlrpc_req_finished(struct ptlrpc_request *request);
void ptlrpc_req_finished_with_imp_lock(struct ptlrpc_request *request);
struct ptlrpc_request *ptlrpc_request_addref(struct ptlrpc_request *req);
struct ptlrpc_bulk_desc *ptlrpc_prep_bulk_imp(struct ptlrpc_request *req,
                                              int npages, int type, int portal);
void __ptlrpc_free_bulk(struct ptlrpc_bulk_desc *bulk, int pin);
static inline void ptlrpc_free_bulk_pin(struct ptlrpc_bulk_desc *bulk)
{
	__ptlrpc_free_bulk(bulk, 1);
}
static inline void ptlrpc_free_bulk_nopin(struct ptlrpc_bulk_desc *bulk)
{
	__ptlrpc_free_bulk(bulk, 0);
}
void __ptlrpc_prep_bulk_page(struct ptlrpc_bulk_desc *desc,
			     cfs_page_t *page, int pageoffset, int len, int);
static inline void ptlrpc_prep_bulk_page_pin(struct ptlrpc_bulk_desc *desc,
					     cfs_page_t *page, int pageoffset,
					     int len)
{
	__ptlrpc_prep_bulk_page(desc, page, pageoffset, len, 1);
}

static inline void ptlrpc_prep_bulk_page_nopin(struct ptlrpc_bulk_desc *desc,
					       cfs_page_t *page, int pageoffset,
					       int len)
{
	__ptlrpc_prep_bulk_page(desc, page, pageoffset, len, 0);
}

void ptlrpc_retain_replayable_request(struct ptlrpc_request *req,
                                      struct obd_import *imp);
__u64 ptlrpc_next_xid(void);
__u64 ptlrpc_sample_next_xid(void);
__u64 ptlrpc_req_xid(struct ptlrpc_request *request);

/* Set of routines to run a function in ptlrpcd context */
void *ptlrpcd_alloc_work(struct obd_import *imp,
                         int (*cb)(const struct lu_env *, void *), void *data);
void ptlrpcd_destroy_work(void *handler);
int ptlrpcd_queue_work(void *handler);

/** @} */
struct ptlrpc_service_buf_conf {
	/* nbufs is how many buffers to post */
	unsigned int			bc_nbufs;
	/* buffer size to post */
	unsigned int			bc_buf_size;
	/* portal to listed for requests on */
	unsigned int			bc_req_portal;
	/* portal of where to send replies to */
	unsigned int			bc_rep_portal;
	/* maximum request size to be accepted for this service */
	unsigned int			bc_req_max_size;
	/* maximum reply size this service can ever send */
	unsigned int			bc_rep_max_size;
};

struct ptlrpc_service_thr_conf {
	/* threadname should be 8 characters or less - 6 will be added on */
	char				*tc_thr_name;
	/* threads increasing factor for each CPU */
	unsigned int			tc_thr_factor;
	/* service threads # to start on each partition while initializing */
	unsigned int			tc_nthrs_init;
	/*
	 * low water of threads # upper-limit on each partition while running,
	 * service availability may be impacted if threads number is lower
	 * than this value. It can be ZERO if the service doesn't require
	 * CPU affinity or there is only one partition.
	 */
	unsigned int			tc_nthrs_base;
	/* "soft" limit for total threads number */
	unsigned int			tc_nthrs_max;
	/* user specified threads number, it will be validated due to
	 * other members of this structure. */
	unsigned int			tc_nthrs_user;
	/* set NUMA node affinity for service threads */
	unsigned int			tc_cpu_affinity;
	/* Tags for lu_context associated with service thread */
	__u32				tc_ctx_tags;
};

struct ptlrpc_service_cpt_conf {
	struct cfs_cpt_table		*cc_cptable;
	/* string pattern to describe CPTs for a service */
	char				*cc_pattern;
};

struct ptlrpc_service_conf {
	/* service name */
	char				*psc_name;
	/* soft watchdog timeout multiplifier to print stuck service traces */
	unsigned int			psc_watchdog_factor;
	/* buffer information */
	struct ptlrpc_service_buf_conf	psc_buf;
	/* thread information */
	struct ptlrpc_service_thr_conf	psc_thr;
	/* CPU partition information */
	struct ptlrpc_service_cpt_conf	psc_cpt;
	/* function table */
	struct ptlrpc_service_ops	psc_ops;
};

/* ptlrpc/service.c */
/**
 * Server-side services API. Register/unregister service, request state
 * management, service thread management
 *
 * @{
 */
void ptlrpc_save_lock(struct ptlrpc_request *req,
                      struct lustre_handle *lock, int mode, int no_ack);
void ptlrpc_commit_replies(struct obd_export *exp);
void ptlrpc_dispatch_difficult_reply(struct ptlrpc_reply_state *rs);
void ptlrpc_schedule_difficult_reply(struct ptlrpc_reply_state *rs);
int ptlrpc_hpreq_handler(struct ptlrpc_request *req);
struct ptlrpc_service *ptlrpc_register_service(
				struct ptlrpc_service_conf *conf,
				struct proc_dir_entry *proc_entry);
void ptlrpc_stop_all_threads(struct ptlrpc_service *svc);

int ptlrpc_start_threads(struct ptlrpc_service *svc);
int ptlrpc_unregister_service(struct ptlrpc_service *service);
int liblustre_check_services(void *arg);
void ptlrpc_daemonize(char *name);
int ptlrpc_service_health_check(struct ptlrpc_service *);
void ptlrpc_hpreq_reorder(struct ptlrpc_request *req);
void ptlrpc_server_drop_request(struct ptlrpc_request *req);

#ifdef __KERNEL__
int ptlrpc_hr_init(void);
void ptlrpc_hr_fini(void);
#else
# define ptlrpc_hr_init() (0)
# define ptlrpc_hr_fini() do {} while(0)
#endif

/** @} */

/* ptlrpc/import.c */
/**
 * Import API
 * @{
 */
int ptlrpc_connect_import(struct obd_import *imp);
int ptlrpc_init_import(struct obd_import *imp);
int ptlrpc_disconnect_import(struct obd_import *imp, int noclose);
int ptlrpc_import_recovery_state_machine(struct obd_import *imp);
void deuuidify(char *uuid, const char *prefix, char **uuid_start,
               int *uuid_len);

/* ptlrpc/pack_generic.c */
int ptlrpc_reconnect_import(struct obd_import *imp);
/** @} */

/**
 * ptlrpc msg buffer and swab interface 
 *
 * @{
 */
int ptlrpc_buf_need_swab(struct ptlrpc_request *req, const int inout,
                         int index);
void ptlrpc_buf_set_swabbed(struct ptlrpc_request *req, const int inout,
                                int index);
int ptlrpc_unpack_rep_msg(struct ptlrpc_request *req, int len);
int ptlrpc_unpack_req_msg(struct ptlrpc_request *req, int len);

int lustre_msg_check_version(struct lustre_msg *msg, __u32 version);
void lustre_init_msg_v2(struct lustre_msg_v2 *msg, int count, __u32 *lens,
                        char **bufs);
int lustre_pack_request(struct ptlrpc_request *, __u32 magic, int count,
                        __u32 *lens, char **bufs);
int lustre_pack_reply(struct ptlrpc_request *, int count, __u32 *lens,
                      char **bufs);
int lustre_pack_reply_v2(struct ptlrpc_request *req, int count,
                         __u32 *lens, char **bufs, int flags);
#define LPRFL_EARLY_REPLY 1
int lustre_pack_reply_flags(struct ptlrpc_request *, int count, __u32 *lens,
                            char **bufs, int flags);
int lustre_shrink_msg(struct lustre_msg *msg, int segment,
                      unsigned int newlen, int move_data);
void lustre_free_reply_state(struct ptlrpc_reply_state *rs);
int __lustre_unpack_msg(struct lustre_msg *m, int len);
int lustre_msg_hdr_size(__u32 magic, int count);
int lustre_msg_size(__u32 magic, int count, __u32 *lengths);
int lustre_msg_size_v2(int count, __u32 *lengths);
int lustre_packed_msg_size(struct lustre_msg *msg);
int lustre_msg_early_size(void);
void *lustre_msg_buf_v2(struct lustre_msg_v2 *m, int n, int min_size);
void *lustre_msg_buf(struct lustre_msg *m, int n, int minlen);
int lustre_msg_buflen(struct lustre_msg *m, int n);
void lustre_msg_set_buflen(struct lustre_msg *m, int n, int len);
int lustre_msg_bufcount(struct lustre_msg *m);
char *lustre_msg_string(struct lustre_msg *m, int n, int max_len);
__u32 lustre_msghdr_get_flags(struct lustre_msg *msg);
void lustre_msghdr_set_flags(struct lustre_msg *msg, __u32 flags);
__u32 lustre_msg_get_flags(struct lustre_msg *msg);
void lustre_msg_add_flags(struct lustre_msg *msg, int flags);
void lustre_msg_set_flags(struct lustre_msg *msg, int flags);
void lustre_msg_clear_flags(struct lustre_msg *msg, int flags);
__u32 lustre_msg_get_op_flags(struct lustre_msg *msg);
void lustre_msg_add_op_flags(struct lustre_msg *msg, int flags);
void lustre_msg_set_op_flags(struct lustre_msg *msg, int flags);
struct lustre_handle *lustre_msg_get_handle(struct lustre_msg *msg);
__u32 lustre_msg_get_type(struct lustre_msg *msg);
__u32 lustre_msg_get_version(struct lustre_msg *msg);
void lustre_msg_add_version(struct lustre_msg *msg, int version);
__u32 lustre_msg_get_opc(struct lustre_msg *msg);
__u64 lustre_msg_get_last_xid(struct lustre_msg *msg);
__u64 lustre_msg_get_last_committed(struct lustre_msg *msg);
__u64 *lustre_msg_get_versions(struct lustre_msg *msg);
__u64 lustre_msg_get_transno(struct lustre_msg *msg);
__u64 lustre_msg_get_slv(struct lustre_msg *msg);
__u32 lustre_msg_get_limit(struct lustre_msg *msg);
void lustre_msg_set_slv(struct lustre_msg *msg, __u64 slv);
void lustre_msg_set_limit(struct lustre_msg *msg, __u64 limit);
int lustre_msg_get_status(struct lustre_msg *msg);
__u32 lustre_msg_get_conn_cnt(struct lustre_msg *msg);
int lustre_msg_is_v1(struct lustre_msg *msg);
__u32 lustre_msg_get_magic(struct lustre_msg *msg);
__u32 lustre_msg_get_timeout(struct lustre_msg *msg);
__u32 lustre_msg_get_service_time(struct lustre_msg *msg);
char *lustre_msg_get_jobid(struct lustre_msg *msg);
__u32 lustre_msg_get_cksum(struct lustre_msg *msg);
#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(2, 7, 50, 0)
__u32 lustre_msg_calc_cksum(struct lustre_msg *msg, int compat18);
#else
# warning "remove checksum compatibility support for b1_8"
__u32 lustre_msg_calc_cksum(struct lustre_msg *msg);
#endif
void lustre_msg_set_handle(struct lustre_msg *msg,struct lustre_handle *handle);
void lustre_msg_set_type(struct lustre_msg *msg, __u32 type);
void lustre_msg_set_opc(struct lustre_msg *msg, __u32 opc);
void lustre_msg_set_last_xid(struct lustre_msg *msg, __u64 last_xid);
void lustre_msg_set_last_committed(struct lustre_msg *msg,__u64 last_committed);
void lustre_msg_set_versions(struct lustre_msg *msg, __u64 *versions);
void lustre_msg_set_transno(struct lustre_msg *msg, __u64 transno);
void lustre_msg_set_status(struct lustre_msg *msg, __u32 status);
void lustre_msg_set_conn_cnt(struct lustre_msg *msg, __u32 conn_cnt);
void ptlrpc_req_set_repsize(struct ptlrpc_request *req, int count, __u32 *sizes);
void ptlrpc_request_set_replen(struct ptlrpc_request *req);
void lustre_msg_set_timeout(struct lustre_msg *msg, __u32 timeout);
void lustre_msg_set_service_time(struct lustre_msg *msg, __u32 service_time);
void lustre_msg_set_jobid(struct lustre_msg *msg, char *jobid);
void lustre_msg_set_cksum(struct lustre_msg *msg, __u32 cksum);

static inline void
lustre_shrink_reply(struct ptlrpc_request *req, int segment,
                    unsigned int newlen, int move_data)
{
        LASSERT(req->rq_reply_state);
        LASSERT(req->rq_repmsg);
        req->rq_replen = lustre_shrink_msg(req->rq_repmsg, segment,
                                           newlen, move_data);
}
/** @} */

/** Change request phase of \a req to \a new_phase */
static inline void
ptlrpc_rqphase_move(struct ptlrpc_request *req, enum rq_phase new_phase)
{
        if (req->rq_phase == new_phase)
                return;

        if (new_phase == RQ_PHASE_UNREGISTERING) {
                req->rq_next_phase = req->rq_phase;
                if (req->rq_import)
                        cfs_atomic_inc(&req->rq_import->imp_unregistering);
        }

        if (req->rq_phase == RQ_PHASE_UNREGISTERING) {
                if (req->rq_import)
                        cfs_atomic_dec(&req->rq_import->imp_unregistering);
        }

        DEBUG_REQ(D_INFO, req, "move req \"%s\" -> \"%s\"",
                  ptlrpc_rqphase2str(req), ptlrpc_phase2str(new_phase));

        req->rq_phase = new_phase;
}

/**
 * Returns true if request \a req got early reply and hard deadline is not met 
 */
static inline int
ptlrpc_client_early(struct ptlrpc_request *req)
{
        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_REPL_UNLINK) &&
            req->rq_reply_deadline > cfs_time_current_sec())
                return 0;
        return req->rq_early;
}

/**
 * Returns true if we got real reply from server for this request
 */
static inline int
ptlrpc_client_replied(struct ptlrpc_request *req)
{
        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_REPL_UNLINK) &&
            req->rq_reply_deadline > cfs_time_current_sec())
                return 0;
        return req->rq_replied;
}

/** Returns true if request \a req is in process of receiving server reply */
static inline int
ptlrpc_client_recv(struct ptlrpc_request *req)
{
        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_REPL_UNLINK) &&
            req->rq_reply_deadline > cfs_time_current_sec())
                return 1;
        return req->rq_receiving_reply;
}

static inline int
ptlrpc_client_recv_or_unlink(struct ptlrpc_request *req)
{
        int rc;

        cfs_spin_lock(&req->rq_lock);
        if (OBD_FAIL_CHECK(OBD_FAIL_PTLRPC_LONG_REPL_UNLINK) &&
            req->rq_reply_deadline > cfs_time_current_sec()) {
                cfs_spin_unlock(&req->rq_lock);
                return 1;
        }
        rc = req->rq_receiving_reply || req->rq_must_unlink;
        cfs_spin_unlock(&req->rq_lock);
        return rc;
}

static inline void
ptlrpc_client_wake_req(struct ptlrpc_request *req)
{
        if (req->rq_set == NULL)
                cfs_waitq_signal(&req->rq_reply_waitq);
        else
                cfs_waitq_signal(&req->rq_set->set_waitq);
}

static inline void
ptlrpc_rs_addref(struct ptlrpc_reply_state *rs)
{
        LASSERT(cfs_atomic_read(&rs->rs_refcount) > 0);
        cfs_atomic_inc(&rs->rs_refcount);
}

static inline void
ptlrpc_rs_decref(struct ptlrpc_reply_state *rs)
{
        LASSERT(cfs_atomic_read(&rs->rs_refcount) > 0);
        if (cfs_atomic_dec_and_test(&rs->rs_refcount))
                lustre_free_reply_state(rs);
}

/* Should only be called once per req */
static inline void ptlrpc_req_drop_rs(struct ptlrpc_request *req)
{
        if (req->rq_reply_state == NULL)
                return; /* shouldn't occur */
        ptlrpc_rs_decref(req->rq_reply_state);
        req->rq_reply_state = NULL;
        req->rq_repmsg = NULL;
}

static inline __u32 lustre_request_magic(struct ptlrpc_request *req)
{
        return lustre_msg_get_magic(req->rq_reqmsg);
}

static inline int ptlrpc_req_get_repsize(struct ptlrpc_request *req)
{
        switch (req->rq_reqmsg->lm_magic) {
        case LUSTRE_MSG_MAGIC_V2:
                return req->rq_reqmsg->lm_repsize;
        default:
                LASSERTF(0, "incorrect message magic: %08x\n",
                         req->rq_reqmsg->lm_magic);
                return -EFAULT;
        }
}

static inline int ptlrpc_send_limit_expired(struct ptlrpc_request *req)
{
        if (req->rq_delay_limit != 0 &&
            cfs_time_before(cfs_time_add(req->rq_queued_time,
                                         cfs_time_seconds(req->rq_delay_limit)),
                            cfs_time_current())) {
                return 1;
        }
        return 0;
}

static inline int ptlrpc_no_resend(struct ptlrpc_request *req)
{
        if (!req->rq_no_resend && ptlrpc_send_limit_expired(req)) {
                cfs_spin_lock(&req->rq_lock);
                req->rq_no_resend = 1;
                cfs_spin_unlock(&req->rq_lock);
        }
        return req->rq_no_resend;
}

static inline int
ptlrpc_server_get_timeout(struct ptlrpc_service_part *svcpt)
{
	int at = AT_OFF ? 0 : at_get(&svcpt->scp_at_estimate);

	return svcpt->scp_service->srv_watchdog_factor *
	       max_t(int, at, obd_timeout);
}

static inline struct ptlrpc_service *
ptlrpc_req2svc(struct ptlrpc_request *req)
{
	LASSERT(req->rq_rqbd != NULL);
	return req->rq_rqbd->rqbd_svcpt->scp_service;
}

/* ldlm/ldlm_lib.c */
/**
 * Target client logic
 * @{
 */
int client_obd_setup(struct obd_device *obddev, struct lustre_cfg *lcfg);
int client_obd_cleanup(struct obd_device *obddev);
int client_connect_import(const struct lu_env *env,
                          struct obd_export **exp, struct obd_device *obd,
                          struct obd_uuid *cluuid, struct obd_connect_data *,
                          void *localdata);
int client_disconnect_export(struct obd_export *exp);
int client_import_add_conn(struct obd_import *imp, struct obd_uuid *uuid,
                           int priority);
int client_import_del_conn(struct obd_import *imp, struct obd_uuid *uuid);
int client_import_find_conn(struct obd_import *imp, lnet_nid_t peer,
                            struct obd_uuid *uuid);
int import_set_conn_priority(struct obd_import *imp, struct obd_uuid *uuid);
void client_destroy_import(struct obd_import *imp);
/** @} */

#ifdef HAVE_SERVER_SUPPORT
int server_disconnect_export(struct obd_export *exp);
#endif

/* ptlrpc/pinger.c */
/**
 * Pinger API (client side only)
 * @{
 */
enum timeout_event {
        TIMEOUT_GRANT = 1
};
struct timeout_item;
typedef int (*timeout_cb_t)(struct timeout_item *, void *);
int ptlrpc_pinger_add_import(struct obd_import *imp);
int ptlrpc_pinger_del_import(struct obd_import *imp);
int ptlrpc_add_timeout_client(int time, enum timeout_event event,
                              timeout_cb_t cb, void *data,
                              cfs_list_t *obd_list);
int ptlrpc_del_timeout_client(cfs_list_t *obd_list,
                              enum timeout_event event);
struct ptlrpc_request * ptlrpc_prep_ping(struct obd_import *imp);
int ptlrpc_obd_ping(struct obd_device *obd);
cfs_time_t ptlrpc_suspend_wakeup_time(void);
#ifdef __KERNEL__
void ping_evictor_start(void);
void ping_evictor_stop(void);
#else
#define ping_evictor_start()    do {} while (0)
#define ping_evictor_stop()     do {} while (0)
#endif
int ptlrpc_check_and_wait_suspend(struct ptlrpc_request *req);
/** @} */

/* ptlrpc daemon bind policy */
typedef enum {
        /* all ptlrpcd threads are free mode */
        PDB_POLICY_NONE          = 1,
        /* all ptlrpcd threads are bound mode */
        PDB_POLICY_FULL          = 2,
        /* <free1 bound1> <free2 bound2> ... <freeN boundN> */
        PDB_POLICY_PAIR          = 3,
        /* <free1 bound1> <bound1 free2> ... <freeN boundN> <boundN free1>,
         * means each ptlrpcd[X] has two partners: thread[X-1] and thread[X+1].
         * If kernel supports NUMA, pthrpcd threads are binded and
         * grouped by NUMA node */
        PDB_POLICY_NEIGHBOR      = 4,
} pdb_policy_t;

/* ptlrpc daemon load policy
 * It is caller's duty to specify how to push the async RPC into some ptlrpcd
 * queue, but it is not enforced, affected by "ptlrpcd_bind_policy". If it is
 * "PDB_POLICY_FULL", then the RPC will be processed by the selected ptlrpcd,
 * Otherwise, the RPC may be processed by the selected ptlrpcd or its partner,
 * depends on which is scheduled firstly, to accelerate the RPC processing. */
typedef enum {
        /* on the same CPU core as the caller */
        PDL_POLICY_SAME         = 1,
        /* within the same CPU partition, but not the same core as the caller */
        PDL_POLICY_LOCAL        = 2,
        /* round-robin on all CPU cores, but not the same core as the caller */
        PDL_POLICY_ROUND        = 3,
        /* the specified CPU core is preferred, but not enforced */
        PDL_POLICY_PREFERRED    = 4,
} pdl_policy_t;

#define PTLRPCD_NTHRS_MAX	32

/* ptlrpc/ptlrpcd.c */
void ptlrpcd_stop(struct ptlrpcd_ctl *pc, int force);
void ptlrpcd_wake(struct ptlrpc_request *req);
void ptlrpcd_add_req(struct ptlrpc_request *req, pdl_policy_t policy, int idx);
void ptlrpcd_add_rqset(struct ptlrpc_request_set *set);
int ptlrpcd_addref(void);
void ptlrpcd_decref(void);

/* ptlrpc/lproc_ptlrpc.c */
/**
 * procfs output related functions
 * @{
 */
const char* ll_opcode2str(__u32 opcode);
#ifdef LPROCFS
void ptlrpc_lprocfs_register_obd(struct obd_device *obd);
void ptlrpc_lprocfs_unregister_obd(struct obd_device *obd);
void ptlrpc_lprocfs_brw(struct ptlrpc_request *req, int bytes);
#else
static inline void ptlrpc_lprocfs_register_obd(struct obd_device *obd) {}
static inline void ptlrpc_lprocfs_unregister_obd(struct obd_device *obd) {}
static inline void ptlrpc_lprocfs_brw(struct ptlrpc_request *req, int bytes) {}
#endif
/** @} */

/* ptlrpc/llog_server.c */
int llog_origin_handle_open(struct ptlrpc_request *req);
int llog_origin_handle_destroy(struct ptlrpc_request *req);
int llog_origin_handle_prev_block(struct ptlrpc_request *req);
int llog_origin_handle_next_block(struct ptlrpc_request *req);
int llog_origin_handle_read_header(struct ptlrpc_request *req);
int llog_origin_handle_close(struct ptlrpc_request *req);
int llog_origin_handle_cancel(struct ptlrpc_request *req);

/* ptlrpc/llog_client.c */
extern struct llog_operations llog_client_ops;

/** @} net */

#endif
/** @} PtlRPC */
