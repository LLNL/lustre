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
 * Copyright 2022 Hewlett Packard Enterprise Development LP
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */
/*
 * kfilnd main interface.
 */

#ifndef _KFILND_
#define _KFILND_

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/uio.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/rhashtable.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>

#include <asm/uaccess.h>
#include <asm/io.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <linux/kmod.h>
#include <linux/sysctl.h>
#include <linux/pci.h>

#include <net/sock.h>
#include <linux/in.h>

#define KFILND_VERSION "0.2.0"

#define DEBUG_SUBSYSTEM S_LND

#include <libcfs/libcfs.h>
#include <lnet/lib-lnet.h>
#include "kfi_endpoint.h"
#include "kfi_errno.h"
#include "kfi_rma.h"
#include "kfi_tagged.h"
#include "kfi_cxi_ext.h"

/* KFILND CFS fail range 0xF100 - 0xF1FF. */

#define CFS_KFI_FAIL_SEND_EVENT 0xF100
#define CFS_KFI_FAIL_READ_EVENT 0xF101
#define CFS_KFI_FAIL_WRITE_EVENT 0xF102
#define CFS_KFI_FAIL_TAGGED_SEND_EVENT 0xF103
#define CFS_KFI_FAIL_TAGGED_RECV_EVENT 0xF104
#define CFS_KFI_FAIL_BULK_TIMEOUT 0xF105
#define CFS_KFI_FAIL_SEND 0xF106
#define CFS_KFI_FAIL_READ 0xF107
#define CFS_KFI_FAIL_WRITE 0xF108
#define CFS_KFI_FAIL_TAGGED_SEND 0xF109
#define CFS_KFI_FAIL_TAGGED_RECV 0xF10A
#define CFS_KFI_FAIL_SEND_EAGAIN 0xF10B
#define CFS_KFI_FAIL_READ_EAGAIN 0xF10C
#define CFS_KFI_FAIL_WRITE_EAGAIN 0xF10D
#define CFS_KFI_FAIL_TAGGED_SEND_EAGAIN 0xF10E
#define CFS_KFI_FAIL_TAGGED_RECV_EAGAIN 0xF10F
#define CFS_KFI_FAIL_TAGGED_RECV_CANCEL_EAGAIN 0xF110
#define CFS_KFI_FAIL_RECV_EAGAIN 0xF111
#define CFS_KFI_FAIL_RECV 0xF112
#define CFS_KFI_FAIL_MSG_UNPACK 0xF113

/* Maximum number of transaction keys supported. */
#define KFILND_EP_KEY_BITS 16U
#define KFILND_EP_KEY_MAX (BIT(KFILND_EP_KEY_BITS) - 1)

/* Some constants which should be turned into tunables */
#define KFILND_IMMEDIATE_MSG_SIZE 4096

#define KFILND_MY_PROCID 49152

/* 256 Rx contexts max */
#define KFILND_FAB_RX_CTX_BITS 8

/* Get the KFI base address from a KFI RX address. RX context information is
 * stored in the MSBs of the KFI address.
 */
#define KFILND_BASE_ADDR(addr) \
	((addr) & ((1UL << (64 - KFILND_FAB_RX_CTX_BITS)) - 1))

/* States used by all kfilnd structures */
enum kfilnd_object_states {
	KFILND_STATE_UNINITIALIZED,
	KFILND_STATE_INITIALIZED,
	KFILND_STATE_SHUTTING_DOWN
};

extern struct dentry *kfilnd_debug_dir;
extern const struct file_operations kfilnd_initiator_state_stats_file_ops;
extern const struct file_operations kfilnd_target_state_stats_file_ops;
extern const struct file_operations kfilnd_target_stats_file_ops;
extern const struct file_operations kfilnd_initiator_stats_file_ops;
extern const struct file_operations kfilnd_reset_stats_file_ops;

extern struct workqueue_struct *kfilnd_wq;

extern unsigned int cksum;
extern unsigned int tx_scale_factor;
extern unsigned int rx_cq_scale_factor;
extern unsigned int tx_cq_scale_factor;
extern unsigned int eq_size;
extern unsigned int immediate_rx_buf_count;

int kfilnd_tunables_setup(struct lnet_ni *ni);
int kfilnd_tunables_init(void);

struct kfilnd_transaction;
struct kfilnd_ep;
struct kfilnd_dev;

/* Multi-receive buffers for immediate receives */
struct kfilnd_immediate_buffer {
	void *immed_buf;
	size_t immed_buf_size;
	struct page *immed_buf_page;
	atomic_t immed_ref;
	bool immed_no_repost;
	struct list_head replay_entry;
	struct kfilnd_ep *immed_end;
};

extern atomic_t kfilnd_rx_count;

struct kfilnd_cq;

struct kfilnd_cq_work {
	struct kfilnd_cq *cq;
	unsigned int work_cpu;
	struct work_struct work;
};

struct kfilnd_cq {
	struct kfilnd_ep *ep;
	struct kfid_cq *cq;
	unsigned int cq_work_count;
	struct kfilnd_cq_work cq_works[];
};

struct kfilnd_ep {
	/* The contexts for this CPT */
	struct kfid_ep *end_tx;
	struct kfid_ep *end_rx;

	/* Corresponding CQs */
	struct kfilnd_cq *end_tx_cq;
	struct kfilnd_cq *end_rx_cq;

	/* Specific config values for this endpoint */
	struct kfilnd_dev *end_dev;
	int end_cpt;
	int end_context_id;

	/* List of transactions. */
	struct list_head tn_list;
	spinlock_t tn_list_lock;

	/* Replay queues. */
	struct list_head tn_replay;
	struct list_head imm_buffer_replay;
	spinlock_t replay_lock;
	struct timer_list replay_timer;
	struct work_struct replay_work;
	atomic_t replay_count;

	/* Key used to build the tag for tagged buffers. */
	struct ida keys;

	/* Pre-posted immediate buffers */
	struct kfilnd_immediate_buffer end_immed_bufs[];
};

struct kfilnd_peer {
	struct rhash_head node;
	struct rcu_head rcu_head;
	struct kfilnd_dev *dev;
	lnet_nid_t nid;
	kfi_addr_t addr;
	atomic_t rx_base;
	atomic_t remove_peer;
	refcount_t cnt;
	time64_t last_alive;
	u16 version;
	u32 local_session_key;
	u32 remote_session_key;
};

static inline bool kfilnd_peer_is_new_peer(struct kfilnd_peer *peer)
{
	return peer->version == 0;
}

static inline void kfilnd_peer_set_version(struct kfilnd_peer *peer,
					   u16 version)
{
	peer->version = version;
}

static inline void kfilnd_peer_set_remote_session_key(struct kfilnd_peer *peer,
						      u32 session_key)
{
	peer->remote_session_key = session_key;
}

struct kfilnd_fab {
	struct list_head entry;
	struct list_head dom_list;
	struct mutex dom_list_lock;
	struct kfid_fabric *fabric;
	struct kref cnt;
};

struct kfilnd_dom {
	struct list_head entry;
	struct list_head dev_list;
	spinlock_t lock;
	struct kfilnd_fab *fab;
	struct kfid_domain *domain;
	struct kref cnt;
};

/* Transaction States */
enum tn_states {
	TN_STATE_INVALID,

	/* Shared initiator and target states. */
	TN_STATE_IDLE,
	TN_STATE_WAIT_TAG_COMP,

	/* Initiator immediate states. */
	TN_STATE_IMM_SEND,

	/* Initiator bulk states. */
	TN_STATE_TAGGED_RECV_POSTED,
	TN_STATE_SEND_FAILED,
	TN_STATE_WAIT_COMP,
	TN_STATE_WAIT_TIMEOUT_COMP,
	TN_STATE_WAIT_SEND_COMP,
	TN_STATE_WAIT_TIMEOUT_TAG_COMP,
	TN_STATE_FAIL,

	/* Target states. */
	TN_STATE_IMM_RECV,
	TN_STATE_WAIT_TAG_RMA_COMP,

	/* Invalid max value. */
	TN_STATE_MAX,
};

/* Base duration state stats. */
struct kfilnd_tn_duration_stat {
	atomic64_t accumulated_duration;
	atomic_t accumulated_count;
};

/* Transaction state stats group into 22 buckets. Bucket zero corresponds to
 * LNet message size of 0 bytes and buckets 1 through 21 correspond to LNet
 * message sizes of 1 to 1048576 bytes increasing by a power of 2. LNet message
 * sizes are round up to the nearest power of 2.
 */
#define KFILND_DATA_SIZE_BUCKETS 22U
#define KFILND_DATA_SIZE_MAX_SIZE (1U << (KFILND_DATA_SIZE_BUCKETS - 2))
struct kfilnd_tn_data_size_duration_stats {
	struct kfilnd_tn_duration_stat data_size[KFILND_DATA_SIZE_BUCKETS];
};

static inline unsigned int kfilnd_msg_len_to_data_size_bucket(size_t size)
{
	u64 bit;

	if (size == 0)
		return 0;
	if (size >= KFILND_DATA_SIZE_MAX_SIZE)
		return KFILND_DATA_SIZE_BUCKETS - 1;

	/* Round size up to the nearest power of 2. */
	bit = fls64(size);
	if (BIT(bit) < size)
		bit++;

	return (unsigned int)bit;
}

/* One data size duraction state bucket for each transaction state. */
struct kfilnd_tn_state_data_size_duration_stats {
	struct kfilnd_tn_data_size_duration_stats state[TN_STATE_MAX];
};

struct kfilnd_dev {
	struct list_head	kfd_list;	/* chain on kfid_devs */
	struct lnet_ni		*kfd_ni;
	enum kfilnd_object_states kfd_state;

	/* KFI LND domain the device is associated with. */
	struct kfilnd_dom	*dom;

	/* Fields specific to kfabric operation */
	spinlock_t		kfd_lock;
	struct kfid_ep		*kfd_sep;
	struct kfid_av		*kfd_av;
	struct kfilnd_ep	**kfd_endpoints;

	/* Map of LNet NI CPTs to endpoints. */
	struct kfilnd_ep	**cpt_to_endpoint;

	/* Hash of LNet NIDs to KFI addresses. */
	struct rhashtable peer_cache;

	/* Per LNet NI states. */
	struct kfilnd_tn_state_data_size_duration_stats initiator_state_stats;
	struct kfilnd_tn_state_data_size_duration_stats target_state_stats;
	struct kfilnd_tn_data_size_duration_stats initiator_stats;
	struct kfilnd_tn_data_size_duration_stats target_stats;

	/* Per LNet NI debugfs stats. */
	struct dentry *dev_dir;
	struct dentry *initiator_state_stats_file;
	struct dentry *initiator_stats_file;
	struct dentry *target_state_stats_file;
	struct dentry *target_stats_file;
	struct dentry *reset_stats_file;

	/* Physical NIC address. */
	unsigned int nic_addr;
	atomic_t session_keys;
};

/* Invalid checksum value is treated as no checksum. */
/* TODO: Module parameter to disable checksum? */
#define NO_CHECKSUM 0x0

/* Hello message header. */
struct kfilnd_hello_msg {
	/* Support kfilnd version. */
	__u16 version;

	/* Base RX context peer should used. */
	__u16 rx_base;

	/* Session key used by peer. */
	__u32 session_key;

	/* RX context count peer can target. */
	__u16 rx_count;
} __packed;

/* Immediate message header. */
struct kfilnd_immed_msg {
	/* Entire LNet header needed by the destination to match incoming
	 * message.
	 */
	struct lnet_hdr_nid4	hdr;

	/* Entire LNet message payload. */
	char payload[0];
} __packed;

/* Bulk request message header. */
struct kfilnd_bulk_req_msg {
	/* Entire LNet header needed by the destination to match incoming
	 * message.
	 */
	struct lnet_hdr_nid4	hdr;

	/* Specific RX context the target must target to push/pull LNet
	 * payload.
	 */
	__u32 response_rx;

	/* Memory key needed by the target to push/pull LNet payload. */
	__u16 key;
} __packed;

/* Kfilnd message. Includes base transport header plus embedded protocol
 * message.
 */
struct kfilnd_msg {
	/* Unique kfilnd magic. */
	__u32 magic;

	/* Version of the kfilnd protocol. */
	__u16 version;

	/* Specific kfilnd protocol type. */
	__u8 type;

	/* Unused 8 bits. */
	__u8 reserved;

	/* Number of bytes in message. */
	__u16 nob;

	/* Checksum of entire message. 0 is checksum disabled. */
	__sum16 cksum;

	/* Message LNet source NID. */
	__u64 srcnid;

	/* Message LNet target NID. */
	__u64 dstnid;

	/* Embedded protocol headers. Must remain at bottom. */
	union {
		struct kfilnd_immed_msg immed;
		struct kfilnd_bulk_req_msg bulk_req;
		struct kfilnd_hello_msg hello;
	} __packed proto;
} __packed;

#define KFILND_MSG_MAGIC LNET_PROTO_KFI_MAGIC	/* unique magic */

#define KFILND_MSG_VERSION_1	0x1
#define KFILND_MSG_VERSION	KFILND_MSG_VERSION_1

/* Get the KFI RX context from a KFI RX address. RX context information is
 * stored in the MSBs of the KFI address.
 */
#define KFILND_RX_CONTEXT(addr) ((addr) >> (64 - KFILND_FAB_RX_CTX_BITS))

#define KFILND_EP_DEBUG(ep, fmt, ...) \
	CDEBUG(D_NET, "%s:%d " fmt "\n", \
	       libcfs_nidstr(&(ep)->end_dev->kfd_ni->ni_nid), \
	       (ep)->end_context_id, ##__VA_ARGS__)

#define KFILND_EP_ERROR(ep, fmt, ...) \
	CNETERR("%s:%d " fmt "\n", \
		libcfs_nidstr(&(ep)->end_dev->kfd_ni->ni_nid), \
		(ep)->end_context_id, ##__VA_ARGS__)

#define KFILND_TN_PEER_VALID(tn) \
	!IS_ERR_OR_NULL((tn)->peer)

#define KFILND_TN_DIR_DEBUG(tn, fmt, dir, ...) \
	CDEBUG(D_NET, "Transaction ID %p: %s:%u %s %s:%llu " fmt "\n", \
	       (tn), \
	       libcfs_nidstr(&(tn)->tn_ep->end_dev->kfd_ni->ni_nid), \
	       (tn)->tn_ep->end_context_id, dir, \
	       libcfs_nid2str((tn)->peer->nid), \
	       KFILND_TN_PEER_VALID(tn) ? \
		KFILND_RX_CONTEXT((tn)->peer->addr) : 0, \
	       ##__VA_ARGS__)

#define KFILND_TN_DEBUG(tn, fmt, ...) \
	do { \
		if ((tn)->is_initiator) \
			KFILND_TN_DIR_DEBUG(tn, fmt, "->", ##__VA_ARGS__); \
		else \
			KFILND_TN_DIR_DEBUG(tn, fmt, "<-", ##__VA_ARGS__); \
	} while (0)

#define KFILND_TN_DIR_ERROR(tn, fmt, dir, ...) \
	CNETERR("Transaction ID %p: %s:%u %s %s:%llu " fmt "\n", \
		(tn), \
		libcfs_nidstr(&(tn)->tn_ep->end_dev->kfd_ni->ni_nid), \
		(tn)->tn_ep->end_context_id, dir, \
		libcfs_nid2str((tn)->peer->nid), \
		KFILND_TN_PEER_VALID(tn) ? \
			KFILND_RX_CONTEXT((tn)->peer->addr) : 0, \
		##__VA_ARGS__)

#define KFILND_TN_ERROR(tn, fmt, ...) \
	do { \
		if ((tn)->is_initiator) \
			KFILND_TN_DIR_ERROR(tn, fmt, "->", ##__VA_ARGS__); \
		else \
			KFILND_TN_DIR_ERROR(tn, fmt, "<-", ##__VA_ARGS__); \
	} while (0)

/* TODO: Support NOOPs? */
enum kfilnd_msg_type {
	/* Valid message types start at 1. */
	KFILND_MSG_INVALID,

	/* Valid message types. */
	KFILND_MSG_IMMEDIATE,
	KFILND_MSG_BULK_PUT_REQ,
	KFILND_MSG_BULK_GET_REQ,
	KFILND_MSG_HELLO_REQ,
	KFILND_MSG_HELLO_RSP,

	/* Invalid max value. */
	KFILND_MSG_MAX,
};

static inline const char *msg_type_to_str(enum kfilnd_msg_type type)
{
	static const char *str[KFILND_MSG_MAX] = {
		[KFILND_MSG_INVALID] = "KFILND_MSG_INVALID",
		[KFILND_MSG_IMMEDIATE] = "KFILND_MSG_IMMEDIATE",
		[KFILND_MSG_BULK_PUT_REQ] = "KFILND_MSG_BULK_PUT_REQ",
		[KFILND_MSG_BULK_GET_REQ] = "KFILND_MSG_BULK_GET_REQ",
		[KFILND_MSG_HELLO_REQ] = "KFILND_MSG_HELLO_REQ",
		[KFILND_MSG_HELLO_RSP] = "KFILND_MSG_HELLO_RSP",
	};

	if (type >= KFILND_MSG_MAX)
		return "KFILND_MSG_INVALID";

	return str[type];
};

static inline const char *tn_state_to_str(enum tn_states type)
{
	static const char *str[TN_STATE_MAX] = {
		[TN_STATE_INVALID] = "TN_STATE_INVALID",
		[TN_STATE_IDLE] = "TN_STATE_IDLE",
		[TN_STATE_WAIT_TAG_COMP] = "TN_STATE_WAIT_TAG_COMP",
		[TN_STATE_IMM_SEND] = "TN_STATE_IMM_SEND",
		[TN_STATE_TAGGED_RECV_POSTED] = "TN_STATE_TAGGED_RECV_POSTED",
		[TN_STATE_SEND_FAILED] = "TN_STATE_SEND_FAILED",
		[TN_STATE_WAIT_COMP] = "TN_STATE_WAIT_COMP",
		[TN_STATE_WAIT_TIMEOUT_COMP] = "TN_STATE_WAIT_TIMEOUT_COMP",
		[TN_STATE_WAIT_SEND_COMP] = "TN_STATE_WAIT_SEND_COMP",
		[TN_STATE_WAIT_TIMEOUT_TAG_COMP] = "TN_STATE_WAIT_TIMEOUT_TAG_COMP",
		[TN_STATE_FAIL] = "TN_STATE_FAIL",
		[TN_STATE_IMM_RECV] = "TN_STATE_IMM_RECV",
		[TN_STATE_WAIT_TAG_RMA_COMP] = "TN_STATE_WAIT_TAG_RMA_COMP",
	};

	return str[type];
};

/* Transaction Events */
enum tn_events {
	TN_EVENT_INVALID,

	/* Initiator events. */
	TN_EVENT_INIT_IMMEDIATE,
	TN_EVENT_INIT_BULK,
	TN_EVENT_TX_HELLO,
	TN_EVENT_TX_OK,
	TN_EVENT_TX_FAIL,
	TN_EVENT_TAG_RX_OK,
	TN_EVENT_TAG_RX_FAIL,
	TN_EVENT_TAG_RX_CANCEL,
	TN_EVENT_TIMEOUT,

	/* Target events. */
	TN_EVENT_RX_HELLO,
	TN_EVENT_RX_OK,
	TN_EVENT_RX_FAIL,
	TN_EVENT_INIT_TAG_RMA,
	TN_EVENT_SKIP_TAG_RMA,
	TN_EVENT_TAG_TX_OK,
	TN_EVENT_TAG_TX_FAIL,

	/* Invalid max value. */
	TN_EVENT_MAX,
};

static inline const char *tn_event_to_str(enum tn_events type)
{
	static const char *str[TN_EVENT_MAX] = {
		[TN_EVENT_INVALID] = "TN_EVENT_INVALID",
		[TN_EVENT_INIT_IMMEDIATE] = "TN_EVENT_INIT_IMMEDIATE",
		[TN_EVENT_INIT_BULK] = "TN_EVENT_INIT_BULK",
		[TN_EVENT_TX_HELLO] = "TN_EVENT_TX_HELLO",
		[TN_EVENT_TX_OK] = "TN_EVENT_TX_OK",
		[TN_EVENT_TX_FAIL] = "TN_EVENT_TX_FAIL",
		[TN_EVENT_TAG_RX_OK] = "TN_EVENT_TAG_RX_OK",
		[TN_EVENT_TAG_RX_FAIL] = "TN_EVENT_TAG_RX_FAIL",
		[TN_EVENT_TAG_RX_CANCEL] = "TN_EVENT_TAG_RX_CANCEL",
		[TN_EVENT_TIMEOUT] = "TN_EVENT_TIMEOUT",
		[TN_EVENT_RX_HELLO] = "TN_EVENT_RX_HELLO",
		[TN_EVENT_RX_OK] = "TN_EVENT_RX_OK",
		[TN_EVENT_RX_FAIL] = "TN_EVENT_RX_FAIL",
		[TN_EVENT_INIT_TAG_RMA] = "TN_EVENT_INIT_TAG_RMA",
		[TN_EVENT_SKIP_TAG_RMA] = "TN_EVENT_SKIP_TAG_RMA",
		[TN_EVENT_TAG_TX_FAIL] = "TN_EVENT_TAG_TX_FAIL",
	};

	return str[type];
};

struct kfilnd_transaction_msg {
	struct kfilnd_msg *msg;
	size_t length;
};

/* Initiator and target transaction structure. */
struct kfilnd_transaction {
	/* Endpoint list transaction lives on. */
	struct list_head	tn_entry;
	struct mutex		tn_lock;	/* to serialize events */
	int			tn_status;	/* return code from ops */
	struct kfilnd_ep	*tn_ep;		/* endpoint we operate under */
	enum tn_states		tn_state;	/* current state of Tn */
	struct lnet_msg		*tn_lntmsg;	/* LNet msg to finalize */
	struct lnet_msg		*tn_getreply;	/* GET LNet msg to finalize */

	bool			is_initiator;	/* Initiated LNet transfer. */

	/* Transaction send message and target address. */
	kfi_addr_t		tn_target_addr;
	struct kfilnd_peer	*peer;
	struct kfilnd_transaction_msg tn_tx_msg;

	/* Transaction multi-receive buffer and associated receive message. */
	struct kfilnd_immediate_buffer *tn_posted_buf;
	struct kfilnd_transaction_msg tn_rx_msg;

	/* LNet buffer used to register a memory region or perform a RMA
	 * operation.
	 */
	struct bio_vec		tn_kiov[LNET_MAX_IOV];
	unsigned int		tn_num_iovec;

	/* LNet transaction payload byte count. */
	unsigned int		tn_nob;

	/* Bulk transaction buffer is sink or source buffer. */
	bool sink_buffer;

	/* Memory region and remote key used to cover initiator's buffer. */
	u16			tn_mr_key;

	/* RX context used to perform response operations to a Put/Get
	 * request. This is required since the request initiator locks in a
	 * transactions to a specific RX context.
	 */
	u16			tn_response_mr_key;
	u8			tn_response_rx;

	/* Immediate data used to convey transaction state from LNet target to
	 * LNet intiator.
	 */
	u64 tagged_data;

	/* Bulk operation timeout timer. */
	struct timer_list timeout_timer;
	struct work_struct timeout_work;

	/* Transaction health status. */
	enum lnet_msg_hstatus hstatus;

	/* Transaction deadline. */
	ktime_t deadline;

	ktime_t tn_alloc_ts;
	ktime_t tn_state_ts;
	size_t lnet_msg_len;

	/* Fields used to replay transaction. */
	struct list_head replay_entry;
	enum tn_events replay_event;
	int replay_status;

	enum kfilnd_msg_type msg_type;
};

int kfilnd_send_hello_request(struct kfilnd_dev *dev, int cpt, lnet_nid_t nid);

#endif /* _KFILND_ */
