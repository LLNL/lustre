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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * lnet/include/lnet/lib-types.h
 *
 * Types used by the library side routines that do not need to be
 * exposed to the user application
 */

#ifndef __LNET_LIB_TYPES_H__
#define __LNET_LIB_TYPES_H__

#ifndef __KERNEL__
# error This include is only for kernel use.
#endif

#include <linux/kthread.h>
#include <linux/uio.h>
#include <linux/semaphore.h>
#include <linux/types.h>
#include <linux/kref.h>
#include <net/genetlink.h>

#include <uapi/linux/lnet/lnet-nl.h>
#include <uapi/linux/lnet/lnet-dlc.h>
#include <uapi/linux/lnet/lnetctl.h>
#include <uapi/linux/lnet/nidstr.h>

char *libcfs_nidstr_r(const struct lnet_nid *nid,
		      char *buf, size_t buf_size);

static inline char *libcfs_nidstr(const struct lnet_nid *nid)
{
	return libcfs_nidstr_r(nid, libcfs_next_nidstring(),
			       LNET_NIDSTR_SIZE);
}

int libcfs_strnid(struct lnet_nid *nid, const char *str);
char *libcfs_idstr(struct lnet_processid *id);

int cfs_match_nid_net(struct lnet_nid *nid, u32 net,
		      struct list_head *net_num_list,
		      struct list_head *addr);

/* Max payload size */
#define LNET_MAX_PAYLOAD	LNET_MTU

/** limit on the number of fragments in discontiguous MDs */
#define LNET_MAX_IOV	256

/*
 * This is the maximum health value.
 * All local and peer NIs created have their health default to this value.
 */
#define LNET_MAX_HEALTH_VALUE 1000
#define LNET_MAX_SELECTION_PRIORITY UINT_MAX

/* forward refs */
struct lnet_libmd;

enum lnet_msg_hstatus {
	LNET_MSG_STATUS_OK = 0,
	LNET_MSG_STATUS_LOCAL_INTERRUPT,
	LNET_MSG_STATUS_LOCAL_DROPPED,
	LNET_MSG_STATUS_LOCAL_ABORTED,
	LNET_MSG_STATUS_LOCAL_NO_ROUTE,
	LNET_MSG_STATUS_LOCAL_ERROR,
	LNET_MSG_STATUS_LOCAL_TIMEOUT,
	LNET_MSG_STATUS_REMOTE_ERROR,
	LNET_MSG_STATUS_REMOTE_DROPPED,
	LNET_MSG_STATUS_REMOTE_TIMEOUT,
	LNET_MSG_STATUS_NETWORK_TIMEOUT,
	LNET_MSG_STATUS_END,
};

struct lnet_rsp_tracker {
	/* chain on the waiting list */
	struct list_head rspt_on_list;
	/* cpt to lock */
	int rspt_cpt;
	/* nid of next hop */
	struct lnet_nid rspt_next_hop_nid;
	/* deadline of the REPLY/ACK */
	ktime_t rspt_deadline;
	/* parent MD */
	struct lnet_handle_md rspt_mdh;
};

struct lnet_msg {
	struct list_head	msg_activelist;
	struct list_head	msg_list;	/* Q for credits/MD */

	struct lnet_processid	msg_target;
	/* Primary NID of the source. */
	struct lnet_nid		msg_initiator;
	/* where is it from, it's only for building event */
	struct lnet_nid		msg_from;
	__u32			msg_type;

	/*
	 * hold parameters in case message is with held due
	 * to discovery
	 */
	struct lnet_nid		msg_src_nid_param;
	struct lnet_nid		msg_rtr_nid_param;

	/*
	 * Deadline for the message after which it will be finalized if it
	 * has not completed.
	 */
	ktime_t			msg_deadline;

	/* The message health status. */
	enum lnet_msg_hstatus	msg_health_status;
	/* This is a recovery message */
	bool			msg_recovery;
	/* force an RDMA even if the message size is < 4K */
	bool			msg_rdma_force;
	/* the number of times a transmission has been retried */
	int			msg_retry_count;
	/* flag to indicate that we do not want to resend this message */
	bool			msg_no_resend;

	/* committed for sending */
	unsigned int		msg_tx_committed:1;
	/* CPT # this message committed for sending */
	unsigned int		msg_tx_cpt:15;
	/* committed for receiving */
	unsigned int		msg_rx_committed:1;
	/* CPT # this message committed for receiving */
	unsigned int		msg_rx_cpt:15;
	/* queued for tx credit */
	unsigned int		msg_tx_delayed:1;
	/* queued for RX buffer */
	unsigned int		msg_rx_delayed:1;
	/* ready for pending on RX delay list */
	unsigned int		msg_rx_ready_delay:1;

	unsigned int          msg_vmflush:1;      /* VM trying to free memory */
	unsigned int          msg_target_is_router:1; /* sending to a router */
	unsigned int          msg_routing:1;      /* being forwarded */
	unsigned int          msg_ack:1;          /* ack on finalize (PUT) */
	unsigned int          msg_sending:1;      /* outgoing message */
	unsigned int          msg_receiving:1;    /* being received */
	unsigned int          msg_txcredit:1;     /* taken an NI send credit */
	unsigned int          msg_peertxcredit:1; /* taken a peer send credit */
	unsigned int          msg_rtrcredit:1;    /* taken a globel router credit */
	unsigned int          msg_peerrtrcredit:1; /* taken a peer router credit */
	unsigned int          msg_onactivelist:1; /* on the activelist */
	unsigned int	      msg_rdma_get:1;

	struct lnet_peer_ni  *msg_txpeer;         /* peer I'm sending to */
	struct lnet_peer_ni  *msg_rxpeer;         /* peer I received from */

	void                 *msg_private;
	struct lnet_libmd    *msg_md;
	/* the NI the message was sent or received over */
	struct lnet_ni       *msg_txni;
	struct lnet_ni       *msg_rxni;

	unsigned int          msg_len;
	unsigned int          msg_wanted;
	unsigned int          msg_offset;
	unsigned int          msg_niov;
	struct bio_vec	     *msg_kiov;

	struct lnet_event	msg_ev;
	struct lnet_hdr		msg_hdr;
};

struct lnet_libhandle {
	struct list_head	lh_hash_chain;
	__u64			lh_cookie;
};

#define lh_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(char *)(&((type *)0)->member)))

struct lnet_me {
	struct list_head	me_list;
	int			me_cpt;
	struct lnet_processid	me_match_id;
	unsigned int		me_portal;
	unsigned int		me_pos;		/* hash offset in mt_hash */
	__u64			me_match_bits;
	__u64			me_ignore_bits;
	enum lnet_unlink	me_unlink;
	struct lnet_libmd      *me_md;
};

struct lnet_libmd {
	struct list_head	 md_list;
	struct lnet_libhandle	 md_lh;
	struct lnet_me	        *md_me;
	char		        *md_start;
	unsigned int		 md_offset;
	unsigned int		 md_length;
	unsigned int		 md_max_size;
	int			 md_threshold;
	int			 md_refcount;
	unsigned int		 md_options;
	unsigned int		 md_flags;
	unsigned int		 md_niov;	/* # frags at end of struct */
	void		        *md_user_ptr;
	struct lnet_rsp_tracker *md_rspt_ptr;
	lnet_handler_t		 md_handler;
	struct lnet_handle_md	 md_bulk_handle;
	struct bio_vec		 md_kiov[LNET_MAX_IOV];
};

#define LNET_MD_FLAG_ZOMBIE	 BIT(0)
#define LNET_MD_FLAG_AUTO_UNLINK BIT(1)
#define LNET_MD_FLAG_ABORTED	 BIT(2)
/* LNET_MD_FLAG_HANDLING is set when a non-unlink event handler
 * is being called for an event relating to the md.
 * It ensures only one such handler runs at a time.
 * The final "unlink" event is only called once the
 * md_refcount has reached zero, and this flag has been cleared,
 * ensuring that it doesn't race with any other event handler
 * call.
 */
#define LNET_MD_FLAG_HANDLING	 BIT(3)

struct lnet_test_peer {
	/* info about peers we are trying to fail */
	struct list_head	tp_list;	/* ln_test_peers */
	struct lnet_nid		tp_nid;		/* matching nid */
	unsigned int		tp_threshold;	/* # failures to simulate */
};

#define LNET_COOKIE_TYPE_MD    1
#define LNET_COOKIE_TYPE_ME    2
#define LNET_COOKIE_TYPE_EQ    3
#define LNET_COOKIE_TYPE_BITS  2
#define LNET_COOKIE_MASK	((1ULL << LNET_COOKIE_TYPE_BITS) - 1ULL)

struct netstrfns {
	u32	nf_type;
	char	*nf_name;
	char	*nf_modname;
	void	(*nf_addr2str)(u32 addr, char *str, size_t size);
	void	(*nf_addr2str_size)(const __be32 *addr, size_t asize,
				    char *str, size_t size);
	int	(*nf_str2addr)(const char *str, int nob, u32 *addr);
	int	(*nf_str2addr_size)(const char *str, int nob,
				    __be32 *addr, size_t *asize);
	int	(*nf_parse_addrlist)(char *str, int len,
				     struct list_head *list);
	int	(*nf_print_addrlist)(char *buffer, int count,
				     struct list_head *list);
	int	(*nf_match_addr)(u32 addr, struct list_head *list);
	int	(*nf_min_max)(struct list_head *nidlist, u32 *min_nid,
			      u32 *max_nid);
};

struct lnet_ni;					 /* forward ref */
struct socket;

struct lnet_lnd {
	/* fields initialized by the LND */
	__u32			lnd_type;

	int  (*lnd_startup)(struct lnet_ni *ni);
	void (*lnd_shutdown)(struct lnet_ni *ni);
	int  (*lnd_ctl)(struct lnet_ni *ni, unsigned int cmd, void *arg);

	/* In data movement APIs below, payload buffers are described as a set
	 * of 'niov' fragments which are in pages.
	 * The LND may NOT overwrite these fragment descriptors.
	 * An 'offset' and may specify a byte offset within the set of
	 * fragments to start from
	 */

	/* Start sending a preformatted message.  'private' is NULL for PUT and
	 * GET messages; otherwise this is a response to an incoming message
	 * and 'private' is the 'private' passed to lnet_parse().  Return
	 * non-zero for immediate failure, otherwise complete later with
	 * lnet_finalize() */
	int (*lnd_send)(struct lnet_ni *ni, void *private,
			struct lnet_msg *msg);

	/* Start receiving 'mlen' bytes of payload data, skipping the following
	 * 'rlen' - 'mlen' bytes. 'private' is the 'private' passed to
	 * lnet_parse().  Return non-zero for immedaite failure, otherwise
	 * complete later with lnet_finalize().  This also gives back a receive
	 * credit if the LND does flow control. */
	int (*lnd_recv)(struct lnet_ni *ni, void *private, struct lnet_msg *msg,
			int delayed, unsigned int niov,
			struct bio_vec *kiov,
			unsigned int offset, unsigned int mlen, unsigned int rlen);

	/* lnet_parse() has had to delay processing of this message
	 * (e.g. waiting for a forwarding buffer or send credits).  Give the
	 * LND a chance to free urgently needed resources.  If called, return 0
	 * for success and do NOT give back a receive credit; that has to wait
	 * until lnd_recv() gets called.  On failure return < 0 and
	 * release resources; lnd_recv() will not be called. */
	int (*lnd_eager_recv)(struct lnet_ni *ni, void *private,
			      struct lnet_msg *msg, void **new_privatep);

	/* notification of peer down */
	void (*lnd_notify_peer_down)(struct lnet_nid *peer);

	/* accept a new connection */
	int (*lnd_accept)(struct lnet_ni *ni, struct socket *sock);

	/* get dma_dev priority */
	unsigned int (*lnd_get_dev_prio)(struct lnet_ni *ni,
					 unsigned int dev_idx);
};

struct lnet_tx_queue {
	int			tq_credits;	/* # tx credits free */
	int			tq_credits_min;	/* lowest it's been */
	int			tq_credits_max;	/* total # tx credits */
	struct list_head	tq_delayed;	/* delayed TXs */
};

enum lnet_net_state {
	/* set when net block is allocated */
	LNET_NET_STATE_INIT = 0,
	/* set when NIs in net are started successfully */
	LNET_NET_STATE_ACTIVE,
	/* set if all NIs in net are in FAILED state */
	LNET_NET_STATE_INACTIVE,
	/* set when shutting down a NET */
	LNET_NET_STATE_DELETING
};

enum lnet_ni_state {
	/* initial state when NI is created */
	LNET_NI_STATE_INIT = 0,
	/* set when NI is brought up */
	LNET_NI_STATE_ACTIVE,
	/* set when NI is being shutdown */
	LNET_NI_STATE_DELETING,
};

#define LNET_NI_RECOVERY_PENDING	BIT(0)
#define LNET_NI_RECOVERY_FAILED		BIT(1)

enum lnet_stats_type {
	LNET_STATS_TYPE_SEND = 0,
	LNET_STATS_TYPE_RECV,
	LNET_STATS_TYPE_DROP
};

struct lnet_comm_count {
	atomic_t co_get_count;
	atomic_t co_put_count;
	atomic_t co_reply_count;
	atomic_t co_ack_count;
	atomic_t co_hello_count;
};

struct lnet_element_stats {
	struct lnet_comm_count el_send_stats;
	struct lnet_comm_count el_recv_stats;
	struct lnet_comm_count el_drop_stats;
};

struct lnet_health_local_stats {
	atomic_t hlt_local_interrupt;
	atomic_t hlt_local_dropped;
	atomic_t hlt_local_aborted;
	atomic_t hlt_local_no_route;
	atomic_t hlt_local_timeout;
	atomic_t hlt_local_error;
};

struct lnet_health_remote_stats {
	atomic_t hlt_remote_dropped;
	atomic_t hlt_remote_timeout;
	atomic_t hlt_remote_error;
	atomic_t hlt_network_timeout;
};

struct lnet_net {
	/* chain on the ln_nets */
	struct list_head	net_list;

	/* net ID, which is composed of
	 * (net_type << 16) | net_num.
	 * net_type can be one of the enumerated types defined in
	 * lnet/include/lnet/nidstr.h */
	__u32			net_id;

	/* round robin selection */
	__u32			net_seq;

	/* total number of CPTs in the array */
	__u32			net_ncpts;

	/* cumulative CPTs of all NIs in this net */
	__u32			*net_cpts;

	/* relative net selection priority */
	__u32			net_sel_priority;

	/* network tunables */
	struct lnet_ioctl_config_lnd_cmn_tunables net_tunables;

	/*
	 * boolean to indicate that the tunables have been set and
	 * shouldn't be reset
	 */
	bool			net_tunables_set;

	/* procedural interface */
	const struct lnet_lnd	*net_lnd;

	/* list of NIs on this net */
	struct list_head	net_ni_list;

	/* list of NIs being added, but not started yet */
	struct list_head	net_ni_added;

	/* dying LND instances */
	struct list_head	net_ni_zombie;

	/* when I was last alive */
	time64_t		net_last_alive;

	/* protects access to net_last_alive */
	spinlock_t		net_lock;

	/* list of router nids preferred for this network */
	struct list_head	net_rtr_pref_nids;
};

struct lnet_ni {
	/* chain on the lnet_net structure */
	struct list_head	ni_netlist;

	/* chain on the recovery queue */
	struct list_head	ni_recovery;

	/* MD handle for recovery ping */
	struct lnet_handle_md	ni_ping_mdh;

	spinlock_t		ni_lock;

	/* number of CPTs */
	int			ni_ncpts;

	/* bond NI on some CPTs */
	__u32			*ni_cpts;

	/* interface's NID */
	struct lnet_nid		ni_nid;

	/* instance-specific data */
	void			*ni_data;

	/* per ni credits */
	atomic_t		ni_tx_credits;

	/* percpt TX queues */
	struct lnet_tx_queue	**ni_tx_queues;

	/* percpt reference count */
	int			**ni_refs;

	/* pointer to parent network */
	struct lnet_net		*ni_net;

	/* my health status */
	struct lnet_ni_status	*ni_status;

	/* NI FSM. Protected by lnet_ni_lock() */
	enum lnet_ni_state	ni_state;

	/* Recovery state. Protected by lnet_ni_lock() */
	__u32			ni_recovery_state;

	/* When to send the next recovery ping */
	time64_t                ni_next_ping;
	/* How many pings sent during current recovery period did not receive
	 * a reply. NB: reset whenever _any_ message arrives on this NI
	 */
	unsigned int		ni_ping_count;

	/* per NI LND tunables */
	struct lnet_lnd_tunables ni_lnd_tunables;

	/* lnd tunables set explicitly */
	bool ni_lnd_tunables_set;

	/* NI statistics */
	struct lnet_element_stats ni_stats;
	struct lnet_health_local_stats ni_hstats;

	/* physical device CPT */
	int			ni_dev_cpt;

	/* sequence number used to round robin over nis within a net */
	__u32			ni_seq;

	/*
	 * health value
	 *	initialized to LNET_MAX_HEALTH_VALUE
	 * Value is decremented every time we fail to send a message over
	 * this NI because of a NI specific failure.
	 * Value is incremented if we successfully send a message.
	 */
	atomic_t		ni_healthv;

	/*
	 * Set to 1 by the LND when it receives an event telling it the device
	 * has gone into a fatal state. Set to 0 when the LND receives an
	 * even telling it the device is back online.
	 */
	atomic_t		ni_fatal_error_on;

	/* the relative selection priority of this NI */
	__u32			ni_sel_priority;

	/*
	 * equivalent interface to use
	 */
	char			*ni_interface;
	struct net		*ni_net_ns;     /* original net namespace */
};

#define LNET_PROTO_PING_MATCHBITS	0x8000000000000000LL

/*
 * Descriptor of a ping info buffer: keep a separate indicator of the
 * size and a reference count. The type is used both as a source and
 * sink of data, so we need to keep some information outside of the
 * area that may be overwritten by network data.
 */
struct lnet_ping_buffer {
	int			pb_nnis;
	atomic_t		pb_refcnt;
	bool			pb_needs_post;
	struct lnet_ping_info	pb_info;
};

#define LNET_PING_BUFFER_SIZE(NNIDS) \
	offsetof(struct lnet_ping_buffer, pb_info.pi_ni[NNIDS])
#define LNET_PING_BUFFER_LONI(PBUF)	((PBUF)->pb_info.pi_ni[0].ns_nid)
#define LNET_PING_BUFFER_SEQNO(PBUF)	((PBUF)->pb_info.pi_ni[0].ns_status)

#define LNET_PING_INFO_TO_BUFFER(PINFO)	\
	container_of((PINFO), struct lnet_ping_buffer, pb_info)

struct lnet_nid_list {
	struct list_head nl_list;
	struct lnet_nid nl_nid;
};

struct lnet_peer_ni {
	/* chain on lpn_peer_nis */
	struct list_head	lpni_peer_nis;
	/* chain on remote peer list */
	struct list_head	lpni_on_remote_peer_ni_list;
	/* chain on recovery queue */
	struct list_head	lpni_recovery;
	/* chain on peer hash */
	struct list_head	lpni_hashlist;
	/* messages blocking for tx credits */
	struct list_head	lpni_txq;
	/* pointer to peer net I'm part of */
	struct lnet_peer_net	*lpni_peer_net;
	/* statistics kept on each peer NI */
	struct lnet_element_stats lpni_stats;
	struct lnet_health_remote_stats lpni_hstats;
	/* spin lock protecting credits and lpni_txq */
	spinlock_t		lpni_lock;
	/* # tx credits available */
	int			lpni_txcredits;
	/* low water mark */
	int			lpni_mintxcredits;
	/*
	 * Each peer_ni in a gateway maintains its own credits. This
	 * allows more traffic to gateways that have multiple interfaces.
	 */
	/* # router credits */
	int			lpni_rtrcredits;
	/* low water mark */
	int			lpni_minrtrcredits;
	/* bytes queued for sending */
	long			lpni_txqnob;
	/* network peer is on */
	struct lnet_net		*lpni_net;
	/* peer's NID */
	struct lnet_nid		lpni_nid;
	/* # refs */
	struct kref		lpni_kref;
	/* health value for the peer */
	atomic_t		lpni_healthv;
	/* recovery ping mdh */
	struct lnet_handle_md	lpni_recovery_ping_mdh;
	/* When to send the next recovery ping */
	time64_t		lpni_next_ping;
	/* How many pings sent during current recovery period did not receive
	 * a reply. NB: reset whenever _any_ message arrives from this peer NI
	 */
	unsigned int		lpni_ping_count;
	/* CPT this peer attached on */
	int			lpni_cpt;
	/* state flags -- protected by lpni_lock */
	unsigned		lpni_state;
	/* status of the peer NI as reported by the peer */
	__u32			lpni_ns_status;
	/* sequence number used to round robin over peer nis within a net */
	__u32			lpni_seq;
	/* sequence number used to round robin over gateways */
	__u32			lpni_gw_seq;
	/* returned RC ping features. Protected with lpni_lock */
	unsigned int		lpni_ping_feats;
	/* time last message was received from the peer */
	time64_t		lpni_last_alive;
	/* preferred local nids: if only one, use lpni_pref.nid */
	union lpni_pref {
		struct lnet_nid nid;
		struct list_head nids;
	} lpni_pref;
	/* list of router nids preferred for this peer NI */
	struct list_head	lpni_rtr_pref_nids;
	/* The relative selection priority of this peer NI */
	__u32			lpni_sel_priority;
	/* number of preferred NIDs in lnpi_pref_nids */
	__u32			lpni_pref_nnids;
};

/* Preferred path added due to traffic on non-MR peer_ni */
#define LNET_PEER_NI_NON_MR_PREF	BIT(0)
/* peer is being recovered. */
#define LNET_PEER_NI_RECOVERY_PENDING	BIT(1)
/* recovery ping failed */
#define LNET_PEER_NI_RECOVERY_FAILED	BIT(2)
/* peer is being deleted */
#define LNET_PEER_NI_DELETING		BIT(3)

struct lnet_peer {
	/* chain on pt_peer_list */
	struct list_head	lp_peer_list;

	/* list of peer nets */
	struct list_head	lp_peer_nets;

	/* list of messages pending discovery*/
	struct list_head	lp_dc_pendq;

	/* chain on router list */
	struct list_head	lp_rtr_list;

	/* primary NID of the peer */
	struct lnet_nid		lp_primary_nid;

	/* source NID to use during discovery */
	struct lnet_nid		lp_disc_src_nid;
	/* destination NID to use during discovery */
	struct lnet_nid		lp_disc_dst_nid;

	/* net to perform discovery on */
	__u32			lp_disc_net_id;

	/* CPT of peer_table */
	int			lp_cpt;

	/* number of NIDs on this peer */
	int			lp_nnis;

	/* # refs from lnet_route::lr_gateway */
	int			lp_rtr_refcount;

	/*
	 * peer specific health sensitivity value to decrement peer nis in
	 * this peer with if set to something other than 0
	 */
	__u32			lp_health_sensitivity;

	/* messages blocking for router credits */
	struct list_head	lp_rtrq;

	/* routes on this peer */
	struct list_head	lp_routes;

	/* reference count */
	atomic_t		lp_refcount;

	/* lock protecting peer state flags and lpni_rtrq */
	spinlock_t		lp_lock;

	/* peer state flags */
	unsigned		lp_state;

	/* buffer for data pushed by peer */
	struct lnet_ping_buffer	*lp_data;

	/* MD handle for ping in progress */
	struct lnet_handle_md	lp_ping_mdh;

	/* MD handle for push in progress */
	struct lnet_handle_md	lp_push_mdh;

	/* number of NIDs for sizing push data */
	int			lp_data_nnis;

	/* NI config sequence number of peer */
	__u32			lp_peer_seqno;

	/* Local NI config sequence number acked by peer */
	__u32			lp_node_seqno;

	/* Local NI config sequence number sent to peer */
	__u32			lp_node_seqno_sent;

	/* Ping error encountered during discovery. */
	int			lp_ping_error;

	/* Push error encountered during discovery. */
	int			lp_push_error;

	/* Error encountered during discovery. */
	int			lp_dc_error;

	/* time it was put on the ln_dc_working queue */
	time64_t		lp_last_queued;

	/* link on discovery-related lists */
	struct list_head	lp_dc_list;

	/* tasks waiting on discovery of this peer */
	wait_queue_head_t	lp_dc_waitq;

	/* cached peer aliveness */
	bool			lp_alive;
};

/*
 * The status flags in lp_state. Their semantics have chosen so that
 * lp_state can be zero-initialized.
 *
 * A peer is marked MULTI_RAIL in two cases: it was configured using DLC
 * as multi-rail aware, or the LNET_PING_FEAT_MULTI_RAIL bit was set.
 *
 * A peer is marked NO_DISCOVERY if the LNET_PING_FEAT_DISCOVERY bit was
 * NOT set when the peer was pinged by discovery.
 *
 * A peer is marked ROUTER if it indicates so in the feature bit.
 */
#define LNET_PEER_MULTI_RAIL		BIT(0)	/* Multi-rail aware */
#define LNET_PEER_NO_DISCOVERY		BIT(1)	/* Peer disabled discovery */
#define LNET_PEER_ROUTER_ENABLED	BIT(2)	/* router feature enabled */

/*
 * A peer is marked CONFIGURED if it was configured by DLC.
 *
 * In addition, a peer is marked DISCOVERED if it has fully passed
 * through Peer Discovery.
 *
 * When Peer Discovery is disabled, the discovery thread will mark
 * peers REDISCOVER to indicate that they should be re-examined if
 * discovery is (re)enabled on the node.
 *
 * A peer that was created as the result of inbound traffic will not
 * be marked at all.
 */
#define LNET_PEER_CONFIGURED		BIT(3)	/* Configured via DLC */
#define LNET_PEER_DISCOVERED		BIT(4)	/* Peer was discovered */
#define LNET_PEER_REDISCOVER		BIT(5)	/* Discovery was disabled */
/*
 * A peer is marked DISCOVERING when discovery is in progress.
 * The other flags below correspond to stages of discovery.
 */
#define LNET_PEER_DISCOVERING		BIT(6)	/* Discovering */
#define LNET_PEER_DATA_PRESENT		BIT(7)	/* Remote peer data present */
#define LNET_PEER_NIDS_UPTODATE		BIT(8)	/* Remote peer info uptodate */
#define LNET_PEER_PING_SENT		BIT(9)	/* Waiting for REPLY to Ping */
#define LNET_PEER_PUSH_SENT		BIT(10)	/* Waiting for ACK of Push */
#define LNET_PEER_PING_FAILED		BIT(11)	/* Ping send failure */
#define LNET_PEER_PUSH_FAILED		BIT(12)	/* Push send failure */
/*
 * A ping can be forced as a way to fix up state, or as a manual
 * intervention by an admin.
 * A push can be forced in circumstances that would normally not
 * allow for one to happen.
 */
#define LNET_PEER_FORCE_PING		BIT(13)	/* Forced Ping */
#define LNET_PEER_FORCE_PUSH		BIT(14)	/* Forced Push */

/* force delete even if router */
#define LNET_PEER_RTR_NI_FORCE_DEL	BIT(15)

/* gw undergoing alive discovery */
#define LNET_PEER_RTR_DISCOVERY		BIT(16)
/* gw has undergone discovery (does not indicate success or failure) */
#define LNET_PEER_RTR_DISCOVERED	BIT(17)

/* peer is marked for deletion */
#define LNET_PEER_MARK_DELETION		BIT(18)
/* lnet_peer_del()/lnet_peer_del_locked() has been called on the peer */
#define LNET_PEER_MARK_DELETED		BIT(19)
/* lock primary NID to what's requested by ULP */
#define LNET_PEER_LOCK_PRIMARY		BIT(20)
/* this is for informational purposes only. It is set if a peer gets
 * configured from Lustre with a primary NID which belongs to another peer
 * which is also configured by Lustre as the primary NID.
 */
#define LNET_PEER_BAD_CONFIG		BIT(21)

struct lnet_peer_net {
	/* chain on lp_peer_nets */
	struct list_head	lpn_peer_nets;

	/* list of peer_nis on this network */
	struct list_head	lpn_peer_nis;

	/* pointer to the peer I'm part of */
	struct lnet_peer	*lpn_peer;

	/* Net ID */
	__u32			lpn_net_id;

	/* peer net health */
	int			lpn_healthv;

	/* time of next router ping on this net */
	time64_t		lpn_next_ping;

	/* selection sequence number */
	__u32			lpn_seq;

	/* relative peer net selection priority */
	__u32			lpn_sel_priority;

	/* reference count */
	atomic_t		lpn_refcount;
};

/* peer hash size */
#define LNET_PEER_HASH_BITS	9
#define LNET_PEER_HASH_SIZE	(1 << LNET_PEER_HASH_BITS)

/*
 * peer hash table - one per CPT
 *
 * protected by lnet_net_lock/EX for update
 *    pt_version
 *    pt_hash[...]
 *    pt_peer_list
 *    pt_peers
 * protected by pt_zombie_lock:
 *    pt_zombie_list
 *    pt_zombies
 *
 * pt_zombie lock nests inside lnet_net_lock
 */
struct lnet_peer_table {
	int			pt_version;	/* /proc validity stamp */
	struct list_head	*pt_hash;	/* NID->peer hash */
	struct list_head	pt_peer_list;	/* peers */
	int			pt_peers;	/* # peers */
	struct list_head	pt_zombie_list;	/* zombie peer_ni */
	int			pt_zombies;	/* # zombie peers_ni */
	spinlock_t		pt_zombie_lock;	/* protect list and count */
};

/* peer aliveness is enabled only on routers for peers in a network where the
 * struct lnet_ni::ni_peertimeout has been set to a positive value
 */
#define lnet_peer_aliveness_enabled(lp) (the_lnet.ln_routing != 0 && \
					((lp)->lpni_net) && \
					(lp)->lpni_net->net_tunables.lct_peer_timeout > 0)

struct lnet_route {
	struct list_head	lr_list;	/* chain on net */
	struct list_head	lr_gwlist;	/* chain on gateway */
	struct lnet_peer	*lr_gateway;	/* router node */
	struct lnet_nid		lr_nid;		/* NID used to add route */
	__u32			lr_net;		/* remote network number */
	__u32			lr_lnet;	/* local network number */
	int			lr_seq;		/* sequence for round-robin */
	__u32			lr_hops;	/* how far I am */
	unsigned int		lr_priority;	/* route priority */
	atomic_t		lr_alive;	/* cached route aliveness */
	bool			lr_single_hop;  /* this route is single-hop */
};

#define LNET_REMOTE_NETS_HASH_DEFAULT	(1U << 7)
#define LNET_REMOTE_NETS_HASH_MAX	(1U << 16)
#define LNET_REMOTE_NETS_HASH_SIZE	(1 << the_lnet.ln_remote_nets_hbits)

struct lnet_remotenet {
	/* chain on ln_remote_nets_hash */
	struct list_head	lrn_list;
	/* routes to me */
	struct list_head	lrn_routes;
	/* my net number */
	__u32			lrn_net;
};

/** lnet message has credit and can be submitted to lnd for send/receive */
#define LNET_CREDIT_OK		0
/** lnet message is waiting for credit */
#define LNET_CREDIT_WAIT	1
/** lnet message is waiting for discovery */
#define LNET_DC_WAIT		2

struct lnet_rtrbufpool {
	/* my free buffer pool */
	struct list_head	rbp_bufs;
	/* messages blocking for a buffer */
	struct list_head	rbp_msgs;
	/* # pages in each buffer */
	int			rbp_npages;
	/* requested number of buffers */
	int			rbp_req_nbuffers;
	/* # buffers actually allocated */
	int			rbp_nbuffers;
	/* # free buffers / blocked messages */
	int			rbp_credits;
	/* low water mark */
	int			rbp_mincredits;
};

struct lnet_rtrbuf {
	struct list_head	 rb_list;	/* chain on rbp_bufs */
	struct lnet_rtrbufpool	*rb_pool;	/* owning pool */
	struct bio_vec		 rb_kiov[0];	/* the buffer space */
};

#define LNET_PEER_HASHSIZE   503		/* prime! */

enum lnet_match_flags {
	/* Didn't match anything */
	LNET_MATCHMD_NONE	= BIT(0),
	/* Matched OK */
	LNET_MATCHMD_OK		= BIT(1),
	/* Must be discarded */
	LNET_MATCHMD_DROP	= BIT(2),
	/* match and buffer is exhausted */
	LNET_MATCHMD_EXHAUSTED	= BIT(3),
	/* match or drop */
	LNET_MATCHMD_FINISH	= (LNET_MATCHMD_OK | LNET_MATCHMD_DROP),
};

/* Options for struct lnet_portal::ptl_options */
#define LNET_PTL_LAZY		BIT(0)
#define LNET_PTL_MATCH_UNIQUE	BIT(1)	/* unique match, for RDMA */
#define LNET_PTL_MATCH_WILDCARD	BIT(2)	/* wildcard match, request portal */

/* parameter for matching operations (GET, PUT) */
struct lnet_match_info {
	__u64			mi_mbits;
	struct lnet_processid	mi_id;
	unsigned int		mi_cpt;
	unsigned int		mi_opc;
	unsigned int		mi_portal;
	unsigned int		mi_rlength;
	unsigned int		mi_roffset;
};

/* ME hash of RDMA portal */
#define LNET_MT_HASH_BITS		8
#define LNET_MT_HASH_SIZE		(1 << LNET_MT_HASH_BITS)
#define LNET_MT_HASH_MASK		(LNET_MT_HASH_SIZE - 1)
/* we allocate (LNET_MT_HASH_SIZE + 1) entries for lnet_match_table::mt_hash,
 * the last entry is reserved for MEs with ignore-bits */
#define LNET_MT_HASH_IGNORE		LNET_MT_HASH_SIZE
/* __u64 has 2^6 bits, so need 2^(LNET_MT_HASH_BITS - LNET_MT_BITS_U64) which
 * is 4 __u64s as bit-map, and add an extra __u64 (only use one bit) for the
 * ME-list with ignore-bits, which is mtable::mt_hash[LNET_MT_HASH_IGNORE] */
#define LNET_MT_BITS_U64		6	/* 2^6 bits */
#define LNET_MT_EXHAUSTED_BITS		(LNET_MT_HASH_BITS - LNET_MT_BITS_U64)
#define LNET_MT_EXHAUSTED_BMAP		((1 << LNET_MT_EXHAUSTED_BITS) + 1)

/* portal match table */
struct lnet_match_table {
	/* reserved for upcoming patches, CPU partition ID */
	unsigned int		mt_cpt;
	unsigned int		mt_portal;	/* portal index */
	/* match table is set as "enabled" if there's non-exhausted MD
	 * attached on mt_mhash, it's only valid for wildcard portal */
	unsigned int		mt_enabled;
	/* bitmap to flag whether MEs on mt_hash are exhausted or not */
	__u64			mt_exhausted[LNET_MT_EXHAUSTED_BMAP];
	struct list_head	*mt_mhash;	/* matching hash */
};

/* these are only useful for wildcard portal */
/* Turn off message rotor for wildcard portals */
#define	LNET_PTL_ROTOR_OFF	0
/* round-robin dispatch all PUT messages for wildcard portals */
#define	LNET_PTL_ROTOR_ON	1
/* round-robin dispatch routed PUT message for wildcard portals */
#define	LNET_PTL_ROTOR_RR_RT	2
/* dispatch routed PUT message by hashing source NID for wildcard portals */
#define	LNET_PTL_ROTOR_HASH_RT	3

struct lnet_portal {
	spinlock_t		ptl_lock;
	unsigned int		ptl_index;	/* portal ID, reserved */
	/* flags on this portal: lazy, unique... */
	unsigned int		ptl_options;
	/* list of messages which are stealing buffer */
	struct list_head	ptl_msg_stealing;
	/* messages blocking for MD */
	struct list_head	ptl_msg_delayed;
	/* Match table for each CPT */
	struct lnet_match_table	**ptl_mtables;
	/* spread rotor of incoming "PUT" */
	unsigned int		ptl_rotor;
	/* # active entries for this portal */
	int			ptl_mt_nmaps;
	/* array of active entries' cpu-partition-id */
	int			ptl_mt_maps[0];
};

#define LNET_LH_HASH_BITS	12
#define LNET_LH_HASH_SIZE	(1ULL << LNET_LH_HASH_BITS)
#define LNET_LH_HASH_MASK	(LNET_LH_HASH_SIZE - 1)

/* resource container (ME, MD, EQ) */
struct lnet_res_container {
	unsigned int		rec_type;	/* container type */
	__u64			rec_lh_cookie;	/* cookie generator */
	struct list_head	rec_active;	/* active resource list */
	struct list_head	*rec_lh_hash;	/* handle hash */
};

/* message container */
struct lnet_msg_container {
	int			msc_init;	/* initialized or not */
	/* max # threads finalizing */
	int			msc_nfinalizers;
	/* msgs waiting to complete finalizing */
	struct list_head	msc_finalizing;
	/* msgs waiting to be resent */
	struct list_head	msc_resending;
	struct list_head	msc_active;	/* active message list */
	/* threads doing finalization */
	void			**msc_finalizers;
	/* threads doing resends */
	void			**msc_resenders;
};

/* This UDSP structures need to match the user space liblnetconfig structures
 * in order for the marshall and unmarshall functions to be common.
 */

/* Net is described as a
 *  1. net type
 *  2. num range
 */
struct lnet_ud_net_descr {
	__u32 udn_net_type;
	struct list_head udn_net_num_range;
};

/* each NID range is defined as
 *  1. net descriptor
 *  2. address range descriptor
 */
struct lnet_ud_nid_descr {
	struct lnet_ud_net_descr ud_net_id;
	struct list_head ud_addr_range;
	__u32 ud_mem_size;
};

/* a UDSP rule can have up to three user defined NID descriptors
 *	- src: defines the local NID range for the rule
 *	- dst: defines the peer NID range for the rule
 *	- rte: defines the router NID range for the rule
 *
 * An action union defines the action to take when the rule
 * is matched
 */
struct lnet_udsp {
	struct list_head udsp_on_list;
	__u32 udsp_idx;
	struct lnet_ud_nid_descr udsp_src;
	struct lnet_ud_nid_descr udsp_dst;
	struct lnet_ud_nid_descr udsp_rte;
	enum lnet_udsp_action_type udsp_action_type;
	union {
		__u32 udsp_priority;
	} udsp_action;
};

/* Peer Discovery states */
#define LNET_DC_STATE_SHUTDOWN		0	/* not started */
#define LNET_DC_STATE_RUNNING		1	/* started up OK */
#define LNET_DC_STATE_STOPPING		2	/* telling thread to stop */

/* Router Checker states */
#define LNET_MT_STATE_SHUTDOWN		0	/* not started */
#define LNET_MT_STATE_RUNNING		1	/* started up OK */
#define LNET_MT_STATE_STOPPING		2	/* telling thread to stop */

/* LNet states */
#define LNET_STATE_SHUTDOWN		0	/* not started */
#define LNET_STATE_RUNNING		1	/* started up OK */
#define LNET_STATE_STOPPING		2	/* telling thread to stop */

struct lnet {
	/* CPU partition table of LNet */
	struct cfs_cpt_table		*ln_cpt_table;
	/* number of CPTs in ln_cpt_table */
	unsigned int			ln_cpt_number;
	unsigned int			ln_cpt_bits;

	/* protect LNet resources (ME/MD/EQ) */
	struct cfs_percpt_lock		*ln_res_lock;
	/* # portals */
	int				ln_nportals;
	/* the vector of portals */
	struct lnet_portal		**ln_portals;
	/* percpt MD container */
	struct lnet_res_container	**ln_md_containers;

	/* Event Queue container */
	struct lnet_res_container	ln_eq_container;
	spinlock_t			ln_eq_wait_lock;

	unsigned int			ln_remote_nets_hbits;

	/* protect NI, peer table, credits, routers, rtrbuf... */
	struct cfs_percpt_lock		*ln_net_lock;
	/* percpt message containers for active/finalizing/freed message */
	struct lnet_msg_container	**ln_msg_containers;
	struct lnet_counters		**ln_counters;
	struct lnet_peer_table		**ln_peer_tables;
	/* list of peer nis not on a local network */
	struct list_head		ln_remote_peer_ni_list;
	/* failure simulation */
	struct list_head		ln_test_peers;
	struct list_head		ln_drop_rules;
	struct list_head		ln_delay_rules;
	/* LND instances */
	struct list_head		ln_nets;
	/* the loopback NI */
	struct lnet_ni			*ln_loni;
	/* network zombie list */
	struct list_head		ln_net_zombie;
	/* resend messages list */
	struct list_head		ln_msg_resend;
	/* spin lock to protect the msg resend list */
	spinlock_t			ln_msg_resend_lock;

	/* remote networks with routes to them */
	struct list_head		*ln_remote_nets_hash;
	/* validity stamp */
	__u64				ln_remote_nets_version;
	/* list of all known routers */
	struct list_head		ln_routers;
	/* validity stamp */
	__u64				ln_routers_version;
	/* percpt router buffer pools */
	struct lnet_rtrbufpool		**ln_rtrpools;

	/*
	 * Ping target / Push source
	 *
	 * The ping target and push source share a single buffer. The
	 * ln_ping_target is protected against concurrent updates by
	 * ln_api_mutex.
	 */
	struct lnet_handle_md		ln_ping_target_md;
	lnet_handler_t			ln_ping_target_handler;
	struct lnet_ping_buffer		*ln_ping_target;
	atomic_t			ln_ping_target_seqno;

	/*
	 * Push Target
	 *
	 * ln_push_nnis contains the desired size of the push target.
	 * The lnet_net_lock is used to handle update races. The old
	 * buffer may linger a while after it has been unlinked, in
	 * which case the event handler cleans up.
	 */
	lnet_handler_t			ln_push_target_handler;
	struct lnet_handle_md		ln_push_target_md;
	struct lnet_ping_buffer		*ln_push_target;
	int				ln_push_target_nnis;

	/* discovery event queue handle */
	lnet_handler_t			ln_dc_handler;
	/* discovery requests */
	struct list_head		ln_dc_request;
	/* discovery working list */
	struct list_head		ln_dc_working;
	/* discovery expired list */
	struct list_head		ln_dc_expired;
	/* discovery thread wait queue */
	wait_queue_head_t		ln_dc_waitq;
	/* discovery startup/shutdown state */
	int				ln_dc_state;

	/* monitor thread startup/shutdown state */
	int				ln_mt_state;
	/* serialise startup/shutdown */
	struct semaphore		ln_mt_signal;

	struct mutex			ln_api_mutex;
	struct mutex			ln_lnd_mutex;
	/* Have I called LNetNIInit myself? */
	int				ln_niinit_self;
	/* LNetNIInit/LNetNIFini counter */
	int				ln_refcount;
	/* SHUTDOWN/RUNNING/STOPPING */
	int				ln_state;

	int				ln_routing;	/* am I a router? */
	lnet_pid_t			ln_pid;		/* requested pid */
	/* uniquely identifies this ni in this epoch */
	__u64				ln_interface_cookie;
	/* registered LNDs */
	const struct lnet_lnd		*ln_lnds[NUM_LNDS];

	/* test protocol compatibility flags */
	unsigned long			ln_testprotocompat;

	/* 0 - load the NIs from the mod params
	 * 1 - do not load the NIs from the mod params
	 * Reverse logic to ensure that other calls to LNetNIInit
	 * need no change
	 */
	bool				ln_nis_from_mod_params;

	/*
	 * completion for the monitor thread. The monitor thread takes care of
	 * checking routes, timedout messages and resending messages.
	 */
	struct completion		ln_mt_wait_complete;

	/* per-cpt resend queues */
	struct list_head		**ln_mt_resendqs;
	/* local NIs to recover */
	struct list_head		ln_mt_localNIRecovq;
	/* local NIs to recover */
	struct list_head		ln_mt_peerNIRecovq;
	/*
	 * An array of queues for GET/PUT waiting for REPLY/ACK respectively.
	 * There are CPT number of queues. Since response trackers will be
	 * added on the fast path we can't afford to grab the exclusive
	 * net lock to protect these queues. The CPT will be calculated
	 * based on the mdh cookie.
	 */
	struct list_head		**ln_mt_rstq;
	/*
	 * A response tracker becomes a zombie when the associated MD is queued
	 * for unlink before the response tracker is detached from the MD. An
	 * entry on a zombie list can be freed when either the remaining
	 * operations on the MD complete or when LNet has shut down.
	 */
	struct list_head		**ln_mt_zombie_rstqs;
	/* recovery handler */
	lnet_handler_t			ln_mt_handler;

	/*
	 * Completed when the discovery and monitor threads can enter their
	 * work loops
	 */
	struct completion		ln_started;
	/* UDSP list */
	struct list_head		ln_udsp_list;
};

struct genl_filter_list {
	struct list_head	 lp_list;
	void			*lp_cursor;
	bool			 lp_first;
};

static const struct nla_policy scalar_attr_policy[LN_SCALAR_MAX + 1] = {
	[LN_SCALAR_ATTR_LIST]		= { .type = NLA_NESTED },
	[LN_SCALAR_ATTR_LIST_SIZE]	= { .type = NLA_U16 },
	[LN_SCALAR_ATTR_INDEX]		= { .type = NLA_U16 },
	[LN_SCALAR_ATTR_NLA_TYPE]	= { .type = NLA_U16 },
	[LN_SCALAR_ATTR_VALUE]		= { .type = NLA_STRING },
	[LN_SCALAR_ATTR_KEY_FORMAT]	= { .type = NLA_U16 },
};

int lnet_genl_send_scalar_list(struct sk_buff *msg, u32 portid, u32 seq,
			       const struct genl_family *family, int flags,
			       u8 cmd, const struct ln_key_list *data[]);

/* Special workaround for pre-4.19 kernels to send error messages
 * from dumpit routines. Newer kernels will send message with
 * NL_SET_ERR_MSG information by default if NETLINK_EXT_ACK is set.
 */
static inline int lnet_nl_send_error(struct sk_buff *msg, int portid, int seq,
				     int error)
{
#ifndef HAVE_NL_DUMP_WITH_EXT_ACK
	struct nlmsghdr *nlh;

	if (!error)
		return 0;

	nlh = nlmsg_put(msg, portid, seq, NLMSG_ERROR, sizeof(error), 0);
	if (!nlh)
		return -ENOMEM;
#ifdef HAVE_NL_PARSE_WITH_EXT_ACK
	netlink_ack(msg, nlh, error, NULL);
#else
	netlink_ack(msg, nlh, error);
#endif
	return nlmsg_len(nlh);
#else
	return error;
#endif
}

#endif
