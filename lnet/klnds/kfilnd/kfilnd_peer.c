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
 * kfilnd peer management implementation.
 */
#include "kfilnd_peer.h"
#include "kfilnd_dev.h"

static const struct rhashtable_params peer_cache_params = {
	.head_offset = offsetof(struct kfilnd_peer, kp_node),
	.key_offset = offsetof(struct kfilnd_peer, kp_nid),
	.key_len = sizeof_field(struct kfilnd_peer, kp_nid),
	.automatic_shrinking = true,
};

/**
 * kfilnd_peer_free() - RCU safe way to free a peer.
 * @ptr: Pointer to peer.
 * @arg: Unused.
 */
static void kfilnd_peer_free(void *ptr, void *arg)
{
	struct kfilnd_peer *kp = ptr;

	CDEBUG(D_NET, "%s(0x%llx) peer entry freed\n",
	       libcfs_nid2str(kp->kp_nid), kp->kp_addr);

	kfi_av_remove(kp->kp_dev->kfd_av, &kp->kp_addr, 1, 0);

	kfree_rcu(kp, kp_rcu_head);
}

/**
 * kfilnd_peer_down() - Mark a peer as down.
 * @kp: Peer to be downed.
 */
void kfilnd_peer_down(struct kfilnd_peer *kp)
{
	if (atomic_cmpxchg(&kp->kp_remove_peer, 0, 1) == 0) {
		CDEBUG(D_NET, "%s(0x%llx) marked for removal from peer cache\n",
		       libcfs_nid2str(kp->kp_nid), kp->kp_addr);

		lnet_notify(kp->kp_dev->kfd_ni, kp->kp_nid, false, false,
			    kp->kp_last_alive);
	}
}

/**
 * kfilnd_peer_put() - Return a reference for a peer.
 * @kp: Peer where the reference should be returned.
 */
void kfilnd_peer_put(struct kfilnd_peer *kp)
{
	rcu_read_lock();

	/* Return allocation reference if the peer was marked for removal. */
	if (atomic_cmpxchg(&kp->kp_remove_peer, 1, 2) == 1) {
		rhashtable_remove_fast(&kp->kp_dev->peer_cache, &kp->kp_node,
				       peer_cache_params);
		refcount_dec(&kp->kp_cnt);

		CDEBUG(D_NET, "%s(0x%llx) removed from peer cache\n",
		       libcfs_nid2str(kp->kp_nid), kp->kp_addr);
	}

	if (refcount_dec_and_test(&kp->kp_cnt))
		kfilnd_peer_free(kp, NULL);

	rcu_read_unlock();
}

u16 kfilnd_peer_target_rx_base(struct kfilnd_peer *kp)
{
	int cpt = lnet_cpt_of_nid(kp->kp_nid, kp->kp_dev->kfd_ni);
	struct kfilnd_ep *ep = kp->kp_dev->cpt_to_endpoint[cpt];

	return ep->end_context_id;
}

/**
 * kfilnd_peer_get() - Get a reference for a peer.
 * @dev: Device used to lookup peer.
 * @nid: LNet NID of peer.
 *
 * Return: On success, pointer to a valid peer structed. Else, ERR_PTR.
 */
struct kfilnd_peer *kfilnd_peer_get(struct kfilnd_dev *dev, lnet_nid_t nid)
{
	char *node;
	char *service;
	int rc;
	u32 nid_addr = LNET_NIDADDR(nid);
	u32 net_num = LNET_NETNUM(LNET_NIDNET(nid));
	struct kfilnd_peer *kp;
	struct kfilnd_peer *clash_peer;

again:
	/* Check the cache for a match. */
	rcu_read_lock();
	kp = rhashtable_lookup_fast(&dev->peer_cache, &nid,
				      peer_cache_params);
	if (kp && !refcount_inc_not_zero(&kp->kp_cnt))
		kp = NULL;
	rcu_read_unlock();

	if (kp)
		return kp;

	/* Allocate a new peer for the cache. */
	kp = kzalloc(sizeof(*kp), GFP_KERNEL);
	if (!kp) {
		rc = -ENOMEM;
		goto err;
	}

	node = kasprintf(GFP_KERNEL, "%#x", nid_addr);
	if (!node) {
		rc = -ENOMEM;
		goto err_free_peer;
	}

	service = kasprintf(GFP_KERNEL, "%u", net_num);
	if (!service) {
		rc = -ENOMEM;
		goto err_free_node_str;
	}

	/* Use the KFI address vector to translate node and service string into
	 * a KFI address handle.
	 */
	rc = kfi_av_insertsvc(dev->kfd_av, node, service, &kp->kp_addr, 0, dev);

	kfree(service);
	kfree(node);

	if (rc < 0) {
		goto err_free_peer;
	} else if (rc != 1) {
		rc = -ECONNABORTED;
		goto err_free_peer;
	}

	kp->kp_dev = dev;
	kp->kp_nid = nid;
	atomic_set(&kp->kp_rx_base, 0);
	atomic_set(&kp->kp_remove_peer, 0);
	kp->kp_local_session_key = kfilnd_dev_get_session_key(dev);

	/* One reference for the allocation and another for get operation
	 * performed for this peer. The allocation reference is returned when
	 * the entry is marked for removal.
	 */
	refcount_set(&kp->kp_cnt, 2);

	clash_peer = rhashtable_lookup_get_insert_fast(&dev->peer_cache,
						       &kp->kp_node,
						       peer_cache_params);

	if (clash_peer) {
		kfi_av_remove(dev->kfd_av, &kp->kp_addr, 1, 0);
		kfree(kp);

		if (IS_ERR(clash_peer)) {
			rc = PTR_ERR(clash_peer);
			goto err;
		} else {
			goto again;
		}
	}

	kfilnd_peer_alive(kp);

	CDEBUG(D_NET, "%s(0x%llx) peer entry allocated\n",
	       libcfs_nid2str(kp->kp_nid), kp->kp_addr);

	return kp;

err_free_node_str:
	kfree(node);
err_free_peer:
	kfree(kp);
err:
	return ERR_PTR(rc);
}

/**
 * kfilnd_peer_get_kfi_addr() - Return kfi_addr_t used for eager untagged send
 * kfi operations.
 * @kp: Peer struct.
 *
 * The returned kfi_addr_t is updated to target a specific RX context. The
 * address return by this function should not be used if a specific RX context
 * needs to be targeted (i/e the response RX context for a bulk transfer
 * operation).
 *
 * Return: kfi_addr_t.
 */
kfi_addr_t kfilnd_peer_get_kfi_addr(struct kfilnd_peer *kp)
{
	/* TODO: Support RX count by round-robining the generated kfi_addr_t's
	 * across multiple RX contexts using RX base and RX count.
	 */
	return kfi_rx_addr(KFILND_BASE_ADDR(kp->kp_addr),
			   atomic_read(&kp->kp_rx_base),
				       KFILND_FAB_RX_CTX_BITS);
}

/**
 * kfilnd_peer_update_rx_contexts() - Update the RX context for a peer.
 * @kp: Peer to be updated.
 * @rx_base: New RX base for peer.
 * @rx_count: New RX count for peer.
 */
void kfilnd_peer_update_rx_contexts(struct kfilnd_peer *kp,
				    unsigned int rx_base, unsigned int rx_count)
{
	/* TODO: Support RX count. */
	LASSERT(rx_count > 0);
	atomic_set(&kp->kp_rx_base, rx_base);
}

/**
 * kfilnd_peer_alive() - Update when the peer was last alive.
 * @kp: Peer to be updated.
 */
void kfilnd_peer_alive(struct kfilnd_peer *kp)
{
	kp->kp_last_alive = ktime_get_seconds();

	/* Ensure timestamp is committed to memory before used. */
	smp_mb();
}

/**
 * kfilnd_peer_destroy() - Destroy peer cache.
 * @dev: Device peer cache to be destroyed.
 */
void kfilnd_peer_destroy(struct kfilnd_dev *dev)
{
	rhashtable_free_and_destroy(&dev->peer_cache, kfilnd_peer_free, NULL);
}

/**
 * kfilnd_peer_init() - Initialize peer cache.
 * @dev: Device peer cache to be initialized.
 */
void kfilnd_peer_init(struct kfilnd_dev *dev)
{
	rhashtable_init(&dev->peer_cache, &peer_cache_params);
}
