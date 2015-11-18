/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2011 Intel Corporation
 *
 * Copyright 2012 Xyratex Technology Limited
 */
/*
 * lustre/ptlrpc/nrs_crr.c
 *
 * Network Request Scheduler (NRS) CRR-N policy
 *
 * Request ordering in a batched Round-Robin manner over client NIDs
 *
 * Author: Liang Zhen <liang@whamcloud.com>
 * Author: Nikitas Angelinas <nikitas_angelinas@xyratex.com>
 */
/**
 * \addtogoup nrs
 * @{
 */
#ifdef HAVE_SERVER_SUPPORT

#define DEBUG_SUBSYSTEM S_RPC
#ifndef __KERNEL__
#include <liblustre.h>
#endif
#include <obd_support.h>
#include <obd_class.h>
#include <lustre_net.h>
#include <lprocfs_status.h>
#include "ptlrpc_internal.h"

/**
 * \name CRR-N policy
 *
 * Client Round-Robin scheduling over client NIDs
 *
 * @{
 *
 */

#define NRS_POL_NAME_CRRN	"crrn"

/**
 * Binary heap predicate.
 *
 * Uses ptlrpc_nrs_request::nr_u::crr::cr_round and
 * ptlrpc_nrs_request::nr_u::crr::cr_sequence to compare two binheap nodes and
 * produce a binary predicate that shows their relative priority, so that the
 * binary heap can perform the necessary sorting operations.
 *
 * \param[in] e1 the first binheap node to compare
 * \param[in] e2 the second binheap node to compare
 *
 * \retval 0 e1 > e2
 * \retval 1 e1 <= e2
 */
static int crrn_req_compare(cfs_binheap_node_t *e1, cfs_binheap_node_t *e2)
{
	struct ptlrpc_nrs_request *nrq1;
	struct ptlrpc_nrs_request *nrq2;

	nrq1 = container_of(e1, struct ptlrpc_nrs_request, nr_node);
	nrq2 = container_of(e2, struct ptlrpc_nrs_request, nr_node);

	if (nrq1->nr_u.crr.cr_round < nrq2->nr_u.crr.cr_round)
		return 1;
	else if (nrq1->nr_u.crr.cr_round > nrq2->nr_u.crr.cr_round)
		return 0;

	return nrq1->nr_u.crr.cr_sequence < nrq2->nr_u.crr.cr_sequence;
}

static cfs_binheap_ops_t nrs_crrn_heap_ops = {
	.hop_enter	= NULL,
	.hop_exit	= NULL,
	.hop_compare	= crrn_req_compare,
};

/**
 * libcfs_hash operations for nrs_crrn_net::cn_cli_hash
 *
 * This uses ptlrpc_request::rq_peer.nid as its key, in order to hash
 * nrs_crrn_client objects.
 */
#define NRS_NID_BKT_BITS	8
#define NRS_NID_BITS		16

static unsigned nrs_crrn_hop_hash(cfs_hash_t *hs, const void *key,
				  unsigned mask)
{
	return cfs_hash_djb2_hash(key, sizeof(lnet_nid_t), mask);
}

static int nrs_crrn_hop_keycmp(const void *key, cfs_hlist_node_t *hnode)
{
	lnet_nid_t		*nid = (lnet_nid_t *)key;
	struct nrs_crrn_client	*cli = cfs_hlist_entry(hnode,
						       struct nrs_crrn_client,
						       cc_hnode);
	return *nid == cli->cc_nid;
}

static void *nrs_crrn_hop_key(cfs_hlist_node_t *hnode)
{
	struct nrs_crrn_client	*cli = cfs_hlist_entry(hnode,
						       struct nrs_crrn_client,
						       cc_hnode);
	return &cli->cc_nid;
}

static void *nrs_crrn_hop_object(cfs_hlist_node_t *hnode)
{
	return cfs_hlist_entry(hnode, struct nrs_crrn_client, cc_hnode);
}

static void nrs_crrn_hop_get(cfs_hash_t *hs, cfs_hlist_node_t *hnode)
{
	struct nrs_crrn_client *cli = cfs_hlist_entry(hnode,
						      struct nrs_crrn_client,
						      cc_hnode);
	cfs_atomic_inc(&cli->cc_ref);
}

static void nrs_crrn_hop_put(cfs_hash_t *hs, cfs_hlist_node_t *hnode)
{
	struct nrs_crrn_client	*cli = cfs_hlist_entry(hnode,
						       struct nrs_crrn_client,
						       cc_hnode);
	cfs_atomic_dec(&cli->cc_ref);
}

static void nrs_crrn_hop_exit(cfs_hash_t *hs, cfs_hlist_node_t *hnode)
{
	struct nrs_crrn_client	*cli = cfs_hlist_entry(hnode,
						       struct nrs_crrn_client,
						       cc_hnode);
	LASSERTF(cfs_atomic_read(&cli->cc_ref) == 0,
		 "Busy CRR-N object from client with NID %s, with %d refs\n",
		 libcfs_nid2str(cli->cc_nid), cfs_atomic_read(&cli->cc_ref));

	OBD_FREE_PTR(cli);
}

static cfs_hash_ops_t nrs_crrn_hash_ops = {
	.hs_hash	= nrs_crrn_hop_hash,
	.hs_keycmp	= nrs_crrn_hop_keycmp,
	.hs_key		= nrs_crrn_hop_key,
	.hs_object	= nrs_crrn_hop_object,
	.hs_get		= nrs_crrn_hop_get,
	.hs_put		= nrs_crrn_hop_put,
	.hs_put_locked	= nrs_crrn_hop_put,
	.hs_exit	= nrs_crrn_hop_exit,
};

/**
 * Called when a CRR-N policy instance is started.
 *
 * \param[in] policy the policy
 *
 * \retval -ENOMEM OOM error
 * \retval 0	   success
 */
static int nrs_crrn_start(struct ptlrpc_nrs_policy *policy)
{
	struct nrs_crrn_net    *net;
	int			rc = 0;
	ENTRY;

	OBD_CPT_ALLOC_PTR(net, nrs_pol2cptab(policy), nrs_pol2cptid(policy));
	if (net == NULL)
		RETURN(-ENOMEM);

	net->cn_binheap = cfs_binheap_create(&nrs_crrn_heap_ops,
					     CBH_FLAG_ATOMIC_GROW, 4096, NULL,
					     nrs_pol2cptab(policy),
					     nrs_pol2cptid(policy));
	if (net->cn_binheap == NULL)
		GOTO(failed, rc = -ENOMEM);

	net->cn_cli_hash = cfs_hash_create("nrs_crrn_nid_hash",
					   NRS_NID_BITS, NRS_NID_BITS,
					   NRS_NID_BKT_BITS, 0,
					   CFS_HASH_MIN_THETA,
					   CFS_HASH_MAX_THETA,
					   &nrs_crrn_hash_ops,
					   CFS_HASH_RW_BKTLOCK);
	if (net->cn_cli_hash == NULL)
		GOTO(failed, rc = -ENOMEM);

	/**
	 * Set default quantum value to max_rpcs_in_flight for non-MDS OSCs;
	 * there may be more RPCs pending from each struct nrs_crrn_client even
	 * with the default max_rpcs_in_flight value, as we are scheduling over
	 * NIDs, and there may be more than one mount point per client.
	 */
	net->cn_quantum = OSC_MAX_RIF_DEFAULT;
	/**
	 * Set to 1 so that the test inside nrs_crrn_req_add() can evaluate to
	 * true.
	 */
	net->cn_sequence = 1;

	policy->pol_private = net;

	RETURN(rc);

failed:
	if (net->cn_binheap != NULL)
		cfs_binheap_destroy(net->cn_binheap);

	OBD_FREE_PTR(net);

	RETURN(rc);
}

/**
 * Called when a CRR-N policy instance is stopped.
 *
 * Called when the policy has been instructed to transition to the
 * ptlrpc_nrs_pol_state::NRS_POL_STATE_STOPPED state and has no more pending
 * requests to serve.
 *
 * \param[in] policy the policy
 */
static void nrs_crrn_stop(struct ptlrpc_nrs_policy *policy)
{
	struct nrs_crrn_net	*net = policy->pol_private;
	ENTRY;

	LASSERT(net != NULL);
	LASSERT(net->cn_binheap != NULL);
	LASSERT(net->cn_cli_hash != NULL);
	LASSERT(cfs_binheap_is_empty(net->cn_binheap));

	cfs_binheap_destroy(net->cn_binheap);
	cfs_hash_putref(net->cn_cli_hash);

	OBD_FREE_PTR(net);
}

/**
 * Performs a policy-specific ctl function on CRR-N policy instances; similar
 * to ioctl.
 *
 * \param[in]	  policy the policy instance
 * \param[in]	  opc	 the opcode
 * \param[in,out] arg	 used for passing parameters and information
 *
 * \pre spin_is_locked(&policy->pol_nrs->->nrs_lock)
 * \post spin_is_locked(&policy->pol_nrs->->nrs_lock)
 *
 * \retval 0   operation carried out successfully
 * \retval -ve error
 */
int nrs_crrn_ctl(struct ptlrpc_nrs_policy *policy, enum ptlrpc_nrs_ctl opc,
		 void *arg)
{
	LASSERT(spin_is_locked(&policy->pol_nrs->nrs_lock));

	switch((enum nrs_ctl_crr)opc) {
	default:
		RETURN(-EINVAL);

	/**
	 * Read Round Robin quantum size of a policy instance.
	 */
	case NRS_CTL_CRRN_RD_QUANTUM: {
		struct nrs_crrn_net	*net = policy->pol_private;

		*(__u16 *)arg = net->cn_quantum;
		}
		break;

	/**
	 * Write Round Robin quantum size of a policy instance.
	 */
	case NRS_CTL_CRRN_WR_QUANTUM: {
		struct nrs_crrn_net	*net = policy->pol_private;

		net->cn_quantum = *(__u16 *)arg;
		LASSERT(net->cn_quantum != 0);
		}
		break;
	}

	RETURN(0);
}

/**
 * Obtains resources from CRR-N policy instances. The top-level resource lives
 * inside \e nrs_crrn_net and the second-level resource inside
 * \e nrs_crrn_client object instances.
 *
 * \param[in]  policy	  the policy for which resources are being taken for
 *			  request \a nrq
 * \param[in]  nrq	  the request for which resources are being taken
 * \param[in]  parent	  parent resource, embedded in nrs_crrn_net for the
 *			  CRR-N policy
 * \param[out] resp	  resources references are placed in this array
 * \param[in]  moving_req signifies limited caller context; used to perform
 *			  memory allocations in an atomic context in this
 *			  policy
 *
 * \retval 0   we are returning a top-level, parent resource, one that is
 *	       embedded in an nrs_crrn_net object
 * \retval 1   we are returning a bottom-level resource, one that is embedded
 *	       in an nrs_crrn_client object
 *
 * \see nrs_resource_get_safe()
 */
int nrs_crrn_res_get(struct ptlrpc_nrs_policy *policy,
		     struct ptlrpc_nrs_request *nrq,
		     const struct ptlrpc_nrs_resource *parent,
		     struct ptlrpc_nrs_resource **resp, bool moving_req)
{
	struct nrs_crrn_net	*net;
	struct nrs_crrn_client	*cli;
	struct nrs_crrn_client	*tmp;
	struct ptlrpc_request	*req;

	if (parent == NULL) {
		*resp = &((struct nrs_crrn_net *)policy->pol_private)->cn_res;
		return 0;
	}

	net = container_of(parent, struct nrs_crrn_net, cn_res);
	req = container_of(nrq, struct ptlrpc_request, rq_nrq);

	cli = cfs_hash_lookup(net->cn_cli_hash, &req->rq_peer.nid);
	if (cli != NULL)
		goto out;

	OBD_CPT_ALLOC_GFP(cli, nrs_pol2cptab(policy), nrs_pol2cptid(policy),
			  sizeof(*cli), moving_req ? GFP_ATOMIC : GFP_NOFS);
	if (cli == NULL)
		return -ENOMEM;

	cli->cc_nid = req->rq_peer.nid;

	cfs_atomic_set(&cli->cc_ref, 1);
	tmp = cfs_hash_findadd_unique(net->cn_cli_hash, &cli->cc_nid,
				      &cli->cc_hnode);
	if (tmp != cli) {
		OBD_FREE_PTR(cli);
		cli = tmp;
	}
out:
	*resp = &cli->cc_res;

	return 1;
}

/**
 * Called when releasing references to the resource hierachy obtained for a
 * request for scheduling using the CRR-N policy.
 *
 * \param[in] policy   the policy the resource belongs to
 * \param[in] res      the resource to be released
 */
static void nrs_crrn_res_put(struct ptlrpc_nrs_policy *policy,
			     const struct ptlrpc_nrs_resource *res)
{
	struct nrs_crrn_net	*net;
	struct nrs_crrn_client	*cli;

	/**
	 * Do nothing for freeing parent, nrs_crrn_net resources
	 */
	if (res->res_parent == NULL)
		return;

	cli = container_of(res, struct nrs_crrn_client, cc_res);
	net = container_of(res->res_parent, struct nrs_crrn_net, cn_res);

	cfs_hash_put(net->cn_cli_hash, &cli->cc_hnode);
}

/**
 * Called when getting a request from the CRR-N policy for handlingso that it can be served
 *
 * \param[in] policy the policy being polled
 * \param[in] peek   when set, signifies that we just want to examine the
 *		     request, and not handle it, so the request is not removed
 *		     from the policy.
 * \param[in] force  force the policy to return a request; unused in this policy
 *
 * \retval the request to be handled
 * \retval NULL no request available
 *
 * \see ptlrpc_nrs_req_get_nolock()
 * \see nrs_request_get()
 */
static
struct ptlrpc_nrs_request *nrs_crrn_req_get(struct ptlrpc_nrs_policy *policy,
					    bool peek, bool force)
{
	struct nrs_crrn_net	  *net = policy->pol_private;
	cfs_binheap_node_t	  *node = cfs_binheap_root(net->cn_binheap);
	struct ptlrpc_nrs_request *nrq;

	nrq = unlikely(node == NULL) ? NULL :
	      container_of(node, struct ptlrpc_nrs_request, nr_node);

	if (likely(!peek && nrq != NULL)) {
		struct nrs_crrn_client *cli;
		struct ptlrpc_request *req = container_of(nrq,
							  struct ptlrpc_request,
							  rq_nrq);

		cli = container_of(nrs_request_resource(nrq),
				   struct nrs_crrn_client, cc_res);

		LASSERT(nrq->nr_u.crr.cr_round <= cli->cc_round);

		cfs_binheap_remove(net->cn_binheap, &nrq->nr_node);
		cli->cc_active--;

		CDEBUG(D_RPCTRACE,
		       "NRS: starting to handle %s request from %s, with round "
		       LPU64"\n", NRS_POL_NAME_CRRN,
		       libcfs_id2str(req->rq_peer), nrq->nr_u.crr.cr_round);

		/** Peek at the next request to be served */
		node = cfs_binheap_root(net->cn_binheap);

		/** No more requests */
		if (unlikely(node == NULL)) {
			net->cn_round++;
		} else {
			struct ptlrpc_nrs_request *next;

			next = container_of(node, struct ptlrpc_nrs_request,
					    nr_node);

			if (net->cn_round < next->nr_u.crr.cr_round)
				net->cn_round = next->nr_u.crr.cr_round;
		}
	}

	return nrq;
}

/**
 * Adds request \a nrq to a CRR-N \a policy instance's set of queued requests
 *
 * A scheduling round is a stream of requests that have been sorted in batches
 * according to the client that they originate from (as identified by its NID);
 * there can be only one batch for each client in each round. The batches are of
 * maximum size nrs_crrn_net:cn_quantum. When a new request arrives for
 * scheduling from a client that has exhausted its quantum in its current round,
 * it will start scheduling requests on the next scheduling round. Clients are
 * allowed to schedule requests against a round until all requests for the round
 * are serviced, so a client might miss a round if it is not generating requests
 * for a long enough period of time. Clients that miss a round will continue
 * with scheduling the next request that they generate, starting at the round
 * that requests are being dispatched for, at the time of arrival of this new
 * request.
 *
 * Requests are tagged with the round number and a sequence number; the sequence
 * number indicates the relative ordering amongst the batches of requests in a
 * round, and is identical for all requests in a batch, as is the round number.
 * The round and sequence numbers are used by crrn_req_compare() in order to
 * maintain an ordered set of rounds, with each round consisting of an ordered
 * set of batches of requests.
 *
 * \param[in] policy the policy
 * \param[in] nrq    the request to add
 *
 * \retval 0	request successfully added
 * \retval != 0 error
 */
static int nrs_crrn_req_add(struct ptlrpc_nrs_policy *policy,
			    struct ptlrpc_nrs_request *nrq)
{
	struct nrs_crrn_net	*net;
	struct nrs_crrn_client	*cli;
	int			 rc;

	cli = container_of(nrs_request_resource(nrq),
			   struct nrs_crrn_client, cc_res);
	net = container_of(nrs_request_resource(nrq)->res_parent,
			   struct nrs_crrn_net, cn_res);

	if (cli->cc_quantum == 0 || cli->cc_round < net->cn_round ||
	    (cli->cc_active == 0 && cli->cc_quantum > 0)) {

		/**
		 * If the client has no pending requests, and still some of its
		 * quantum remaining unused, which implies it has not had a
		 * chance to schedule up to its maximum allowed batch size of
		 * requests in the previous round it participated, schedule this
		 * next request on a new round; this avoids fragmentation of
		 * request batches caused by client inactivity, at the expense
		 * of potentially slightly increased service time for the
		 * request batch this request will be a part of.
		 */
		if (cli->cc_active == 0 && cli->cc_quantum > 0)
			cli->cc_round++;

		/** A new scheduling round has commenced */
		if (cli->cc_round < net->cn_round)
			cli->cc_round = net->cn_round;

		/** I was not the last client through here */
		if (cli->cc_sequence < net->cn_sequence)
			cli->cc_sequence = ++net->cn_sequence;
		/**
		 * Reset the quantum if we have reached the maximum quantum
		 * size for this batch, or even if we have not managed to
		 * complete a batch size up to its maximum allowed size.
		 * XXX: Accessed unlocked
		 */
		cli->cc_quantum = net->cn_quantum;
	}

	nrq->nr_u.crr.cr_round = cli->cc_round;
	nrq->nr_u.crr.cr_sequence = cli->cc_sequence;

	rc = cfs_binheap_insert(net->cn_binheap, &nrq->nr_node);
	if (rc == 0) {
		cli->cc_active++;
		if (--cli->cc_quantum == 0)
			cli->cc_round++;
	}
	return rc;
}

/**
 * Removes request \a nrq from a CRR-N \a policy instance's set of queued
 * requests.
 *
 * \param[in] policy the policy
 * \param[in] nrq    the request to remove
 */
static void nrs_crrn_req_del(struct ptlrpc_nrs_policy *policy,
			     struct ptlrpc_nrs_request *nrq)
{
	struct nrs_crrn_net	*net;
	struct nrs_crrn_client	*cli;
	bool			 is_root;

	cli = container_of(nrs_request_resource(nrq),
			   struct nrs_crrn_client, cc_res);
	net = container_of(nrs_request_resource(nrq)->res_parent,
			   struct nrs_crrn_net, cn_res);

	LASSERT(nrq->nr_u.crr.cr_round <= cli->cc_round);

	is_root = &nrq->nr_node == cfs_binheap_root(net->cn_binheap);

	cfs_binheap_remove(net->cn_binheap, &nrq->nr_node);
	cli->cc_active--;

	/**
	 * If we just deleted the node at the root of the binheap, we may have
	 * to adjust round numbers.
	 */
	if (unlikely(is_root)) {
		/** Peek at the next request to be served */
		cfs_binheap_node_t *node = cfs_binheap_root(net->cn_binheap);

		/** No more requests */
		if (unlikely(node == NULL)) {
			net->cn_round++;
		} else {
			nrq = container_of(node, struct ptlrpc_nrs_request,
					   nr_node);

			if (net->cn_round < nrq->nr_u.crr.cr_round)
				net->cn_round = nrq->nr_u.crr.cr_round;
		}
	}
}

/**
 * Called right after the request \a nrq finishes being handled by CRR-N policy
 * instance \a policy.
 *
 * \param[in] policy the policy that handled the request
 * \param[in] nrq    the request that was handled
 */
static void nrs_crrn_req_stop(struct ptlrpc_nrs_policy *policy,
			      struct ptlrpc_nrs_request *nrq)
{
	struct ptlrpc_request *req = container_of(nrq, struct ptlrpc_request,
						  rq_nrq);

	CDEBUG(D_RPCTRACE,
	       "NRS: finished handling %s request from %s, with round "LPU64
	       "\n", NRS_POL_NAME_CRRN,
	       libcfs_id2str(req->rq_peer), nrq->nr_u.crr.cr_round);
}

#ifdef LPROCFS

/**
 * lprocfs interface
 */

/**
 * Retrieves the value of the Round Robin quantum (i.e. the maximum batch size)
 * for CRR-N policy instances on both the regular and high-priority NRS head
 * of a service, as long as a policy instance is not in the
 * ptlrpc_nrs_pol_state::NRS_POL_STATE_STOPPED state; policy instances in this
 * state are skipped later by nrs_crrn_ctl().
 *
 * Quantum values are in # of RPCs, and output is in YAML format.
 *
 * For example:
 *
 *	reg_quantum:8
 *	hp_quantum:4
 */
static int ptlrpc_lprocfs_rd_nrs_crrn_quantum(char *page, char **start,
					      off_t off, int count, int *eof,
					      void *data)
{
	struct ptlrpc_service	    *svc = data;
	__u16			     quantum;
	int			     rc;
	int			     rc2 = 0;

	/**
	 * Perform two separate calls to this as only one of the NRS heads'
	 * policies may be in the ptlrpc_nrs_pol_state::NRS_POL_STATE_STARTED or
	 * ptlrpc_nrs_pol_state::NRS_POL_STATE_STOPPING state.
	 */
	rc = ptlrpc_nrs_policy_control(svc, PTLRPC_NRS_QUEUE_REG,
				       NRS_POL_NAME_CRRN,
				       NRS_CTL_CRRN_RD_QUANTUM,
				       true, &quantum);
	if (rc == 0) {
		*eof = 1;
		rc2 = snprintf(page, count, NRS_LPROCFS_QUANTUM_NAME_REG
			       "%-5d\n", quantum);
		/**
		 * Ignore -ENODEV as the regular NRS head's policy may be in the
		 * ptlrpc_nrs_pol_state::NRS_POL_STATE_STOPPED state.
		 */
	} else if (rc != -ENODEV) {
		return rc;
	}

	if (!nrs_svc_has_hp(svc))
		goto no_hp;

	rc = ptlrpc_nrs_policy_control(svc, PTLRPC_NRS_QUEUE_HP,
				       NRS_POL_NAME_CRRN,
				       NRS_CTL_CRRN_RD_QUANTUM,
				       true, &quantum);
	if (rc == 0) {
		*eof = 1;
		rc2 += snprintf(page + rc2, count - rc2,
				NRS_LPROCFS_QUANTUM_NAME_HP"%-5d\n", quantum);
		/**
		 * Ignore -ENODEV as the high priority NRS head's policy may be
		 * in the ptlrpc_nrs_pol_state::NRS_POL_STATE_STOPPED state.
		 */
	} else if (rc != -ENODEV) {
		return rc;
	}

no_hp:

	return rc2 ? : rc;
}

/**
 * Sets the value of the Round Robin quantum (i.e. the maximum batch size)
 * for CRR-N policy instances of a service. The user can set the quantum size
 * for the regular or high priority NRS head individually by specifying each
 * value, or both together in a single invocation.
 *
 * For example:
 *
 * lctl set_param *.*.*.nrs_crrn_quantum=reg_quantum:32, to set the regular
 * request quantum size on all PTLRPC services to 32
 *
 * lctl set_param *.*.*.nrs_crrn_quantum=hp_quantum:16, to set the high
 * priority request quantum size on all PTLRPC services to 16, and
 *
 * lctl set_param *.*.ost_io.nrs_crrn_quantum=16, to set both the regular and
 * high priority request quantum sizes of the ost_io service to 16.
 *
 * policy instances in the ptlrpc_nrs_pol_state::NRS_POL_STATE_STOPPED state
 * are skipped later by nrs_crrn_ctl().
 */
static int ptlrpc_lprocfs_wr_nrs_crrn_quantum(struct file *file,
					      const char *buffer,
					      unsigned long count, void *data)
{
	struct ptlrpc_service	    *svc = data;
	enum ptlrpc_nrs_queue_type   queue = 0;
	char			     kernbuf[LPROCFS_NRS_WR_QUANTUM_MAX_CMD];
	char			    *val;
	long			     quantum_reg;
	long			     quantum_hp;
	/** lprocfs_find_named_value() modifies its argument, so keep a copy */
	size_t			     count_copy;
	int			     rc = 0;
	int			     rc2 = 0;

        if (count > (sizeof(kernbuf) - 1))
                return -EINVAL;

	if (copy_from_user(kernbuf, buffer, count))
		return -EFAULT;

        kernbuf[count] = '\0';

	count_copy = count;

	/**
	 * Check if the regular quantum value has been specified
	 */
	val = lprocfs_find_named_value(kernbuf, NRS_LPROCFS_QUANTUM_NAME_REG,
				       &count_copy);
	if (val != kernbuf) {
		quantum_reg = simple_strtol(val, NULL, 10);

		queue |= PTLRPC_NRS_QUEUE_REG;
	}

	count_copy = count;

	/**
	 * Check if the high priority quantum value has been specified
	 */
	val = lprocfs_find_named_value(kernbuf, NRS_LPROCFS_QUANTUM_NAME_HP,
				       &count_copy);
	if (val != kernbuf) {
		if (!nrs_svc_has_hp(svc))
			return -ENODEV;

		quantum_hp = simple_strtol(val, NULL, 10);

		queue |= PTLRPC_NRS_QUEUE_HP;
	}

	/**
	 * If none of the queues has been specified, look for a valid numerical
	 * value
	 */
	if (queue == 0) {
		if (!isdigit(kernbuf[0]))
			return -EINVAL;

		quantum_reg = simple_strtol(kernbuf, NULL, 10);

		queue = PTLRPC_NRS_QUEUE_REG;

		if (nrs_svc_has_hp(svc)) {
			queue |= PTLRPC_NRS_QUEUE_HP;
			quantum_hp = quantum_reg;
		}
	}

	if ((((queue & PTLRPC_NRS_QUEUE_REG) != 0) &&
	    ((quantum_reg > LPROCFS_NRS_QUANTUM_MAX || quantum_reg <= 0))) ||
	    (((queue & PTLRPC_NRS_QUEUE_HP) != 0) &&
	    ((quantum_hp > LPROCFS_NRS_QUANTUM_MAX || quantum_hp <= 0))))
		return -EINVAL;

	/**
	 * We change the values on regular and HP NRS heads separately, so that
	 * we do not exit early from ptlrpc_nrs_policy_control() with an error
	 * returned by nrs_policy_ctl_locked(), in cases where the user has not
	 * started the policy on either the regular or HP NRS head; i.e. we are
	 * ignoring -ENODEV within nrs_policy_ctl_locked(). -ENODEV is returned
	 * only if the operation fails with -ENODEV on all heads that have been
	 * specified by the command; if at least one operation succeeds,
	 * success is returned.
	 */
	if ((queue & PTLRPC_NRS_QUEUE_REG) != 0) {
		rc = ptlrpc_nrs_policy_control(svc, PTLRPC_NRS_QUEUE_REG,
					       NRS_POL_NAME_CRRN,
					       NRS_CTL_CRRN_WR_QUANTUM, false,
					       &quantum_reg);
		if ((rc < 0 && rc != -ENODEV) ||
		    (rc == -ENODEV && queue == PTLRPC_NRS_QUEUE_REG))
			return rc;
	}

	if ((queue & PTLRPC_NRS_QUEUE_HP) != 0) {
		rc2 = ptlrpc_nrs_policy_control(svc, PTLRPC_NRS_QUEUE_HP,
						NRS_POL_NAME_CRRN,
						NRS_CTL_CRRN_WR_QUANTUM, false,
						&quantum_hp);
		if ((rc2 < 0 && rc2 != -ENODEV) ||
		    (rc2 == -ENODEV && queue == PTLRPC_NRS_QUEUE_HP))
			return rc2;
	}

	return rc == -ENODEV && rc2 == -ENODEV ? -ENODEV : count;
}

/**
 * Initializes a CRR-N policy's lprocfs interface for service \a svc
 *
 * \param[in] svc the service
 *
 * \retval 0	success
 * \retval != 0	error
 */
int nrs_crrn_lprocfs_init(struct ptlrpc_service *svc)
{
	int	rc;

	struct lprocfs_vars nrs_crrn_lprocfs_vars[] = {
		{ .name		= "nrs_crrn_quantum",
		  .read_fptr	= ptlrpc_lprocfs_rd_nrs_crrn_quantum,
		  .write_fptr	= ptlrpc_lprocfs_wr_nrs_crrn_quantum,
		  .data = svc },
		{ NULL }
	};

	if (svc->srv_procroot == NULL)
		return 0;

	rc = lprocfs_add_vars(svc->srv_procroot, nrs_crrn_lprocfs_vars, NULL);

	return rc;
}

/**
 * Cleans up a CRR-N policy's lprocfs interface for service \a svc
 *
 * \param[in] svc the service
 */
void nrs_crrn_lprocfs_fini(struct ptlrpc_service *svc)
{
	if (svc->srv_procroot == NULL)
		return;

	lprocfs_remove_proc_entry("nrs_crrn_quantum", svc->srv_procroot);
}

#endif /* LPROCFS */

/**
 * CRR-N policy operations
 */
static const struct ptlrpc_nrs_pol_ops nrs_crrn_ops = {
	.op_policy_start	= nrs_crrn_start,
	.op_policy_stop		= nrs_crrn_stop,
	.op_policy_ctl		= nrs_crrn_ctl,
	.op_res_get		= nrs_crrn_res_get,
	.op_res_put		= nrs_crrn_res_put,
	.op_req_get		= nrs_crrn_req_get,
	.op_req_enqueue		= nrs_crrn_req_add,
	.op_req_dequeue		= nrs_crrn_req_del,
	.op_req_stop		= nrs_crrn_req_stop,
#ifdef LPROCFS
	.op_lprocfs_init	= nrs_crrn_lprocfs_init,
	.op_lprocfs_fini	= nrs_crrn_lprocfs_fini,
#endif
};

/**
 * CRR-N policy configuration
 */
struct ptlrpc_nrs_pol_conf nrs_conf_crrn = {
	.nc_name		= NRS_POL_NAME_CRRN,
	.nc_ops			= &nrs_crrn_ops,
	.nc_compat		= nrs_policy_compat_all,
};

/** @} CRR-N policy */

/** @} nrs */

#endif /* HAVE_SERVER_SUPPORT */
