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
 * Copyright (c) 2014, 2015, Intel Corporation.
 */
/*
 * lustre/osp/osp_trans.c
 *
 *
 * 1. OSP (Object Storage Proxy) transaction methods
 *
 * Implement OSP layer transaction related interfaces for the dt_device API
 * dt_device_operations.
 *
 *
 * 2. Handle asynchronous idempotent operations
 *
 * The OSP uses OUT (Object Unified Target) RPC to talk with other server
 * (MDT or OST) for kinds of operations, such as create, unlink, insert,
 * delete, lookup, set_(x)attr, get_(x)attr, and etc. To reduce the number
 * of RPCs, we allow multiple operations to be packaged together in single
 * OUT RPC.
 *
 * For the asynchronous idempotent operations, such as get_(x)attr, related
 * RPCs will be inserted into an osp_device based shared asynchronous request
 * queue - osp_device::opd_async_requests. When the queue is full, all the
 * requests in the queue will be packaged into a single OUT RPC and given to
 * the ptlrpcd daemon (for sending), then the queue is purged and other new
 * requests can be inserted into it.
 *
 * When the asynchronous idempotent operation inserts the request into the
 * shared queue, it will register an interpreter. When the packaged OUT RPC
 * is replied (or failed to be sent out), all the registered interpreters
 * will be called one by one to handle each own result.
 *
 *
 * There are three kinds of transactions
 *
 * 1. Local transaction, all of updates of the transaction are in the local MDT.
 * 2. Remote transaction, all of updates of the transaction are in one remote
 * MDT, which only happens in LFSCK now.
 * 3. Distribute transaction, updates for the transaction are in mulitple MDTs.
 *
 * Author: Di Wang <di.wang@intel.com>
 * Author: Fan, Yong <fan.yong@intel.com>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <lustre_net.h>
#include "osp_internal.h"

/**
 * The argument for the interpreter callback of osp request.
 */
struct osp_update_args {
	struct osp_update_request *oaua_update;
	atomic_t		 *oaua_count;
	wait_queue_head_t	 *oaua_waitq;
	bool			  oaua_flow_control;
};

/**
 * Call back for each update request.
 */
struct osp_update_callback {
	/* list in the osp_update_request::our_cb_items */
	struct list_head		 ouc_list;

	/* The target of the async update request. */
	struct osp_object		*ouc_obj;

	/* The data used by or_interpreter. */
	void				*ouc_data;

	/* The interpreter function called after the async request handled. */
	osp_update_interpreter_t	ouc_interpreter;
};

static struct object_update_request *object_update_request_alloc(size_t size)
{
	struct object_update_request *ourq;

	OBD_ALLOC_LARGE(ourq, size);
	if (ourq == NULL)
		return ERR_PTR(-ENOMEM);

	ourq->ourq_magic = UPDATE_REQUEST_MAGIC;
	ourq->ourq_count = 0;

	return ourq;
}

/**
 * Allocate new update request
 *
 * Allocate new update request and insert it to the req_update_list.
 *
 * \param [in] our	osp_udate_request where to create a new
 *                      update request
 *
 * \retval	0 if creation succeeds.
 * \retval	negative errno if creation fails.
 */
int osp_object_update_request_create(struct osp_update_request *our,
				     size_t size)
{
	struct osp_update_request_sub *ours;

	OBD_ALLOC_PTR(ours);
	if (ours == NULL)
		return -ENOMEM;

	if (size < OUT_UPDATE_INIT_BUFFER_SIZE)
		size = OUT_UPDATE_INIT_BUFFER_SIZE;

	ours->ours_req = object_update_request_alloc(size);

	if (IS_ERR(ours->ours_req)) {
		OBD_FREE_PTR(ours);
		return -ENOMEM;
	}

	ours->ours_req_size = size;
	INIT_LIST_HEAD(&ours->ours_list);
	list_add_tail(&ours->ours_list, &our->our_req_list);
	our->our_req_nr++;

	return 0;
}

/**
 * Get current update request
 *
 * Get current object update request from our_req_list in
 * osp_update_request, because we always insert the new update
 * request in the last position, so the last update request
 * in the list will be the current update req.
 *
 * \param[in] our	osp update request where to get the
 *                      current object update.
 *
 * \retval		the current object update.
 **/
struct osp_update_request_sub *
osp_current_object_update_request(struct osp_update_request *our)
{
	if (list_empty(&our->our_req_list))
		return NULL;

	return list_entry(our->our_req_list.prev, struct osp_update_request_sub,
			  ours_list);
}

/**
 * Allocate and initialize osp_update_request
 *
 * osp_update_request is being used to track updates being executed on
 * this dt_device(OSD or OSP). The update buffer will be 4k initially,
 * and increased if needed.
 *
 * \param [in] dt	dt device
 *
 * \retval		osp_update_request being allocated if succeed
 * \retval		ERR_PTR(errno) if failed
 */
struct osp_update_request *osp_update_request_create(struct dt_device *dt)
{
	struct osp_update_request *our;

	OBD_ALLOC_PTR(our);
	if (our == NULL)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&our->our_req_list);
	INIT_LIST_HEAD(&our->our_cb_items);
	INIT_LIST_HEAD(&our->our_list);
	spin_lock_init(&our->our_list_lock);

	osp_object_update_request_create(our, OUT_UPDATE_INIT_BUFFER_SIZE);
	return our;
}

void osp_update_request_destroy(struct osp_update_request *our)
{
	struct osp_update_request_sub *ours;
	struct osp_update_request_sub *tmp;

	if (our == NULL)
		return;

	list_for_each_entry_safe(ours, tmp, &our->our_req_list, ours_list) {
		list_del(&ours->ours_list);
		if (ours->ours_req != NULL)
			OBD_FREE_LARGE(ours->ours_req, ours->ours_req_size);
		OBD_FREE_PTR(ours);
	}
	OBD_FREE_PTR(our);
}

static void
object_update_request_dump(const struct object_update_request *ourq,
			   unsigned int mask)
{
	unsigned int i;
	size_t total_size = 0;

	for (i = 0; i < ourq->ourq_count; i++) {
		struct object_update	*update;
		size_t			size = 0;

		update = object_update_request_get(ourq, i, &size);
		LASSERT(update != NULL);
		CDEBUG(mask, "i = %u fid = "DFID" op = %s "
		       "params = %d batchid = "LPU64" size = %zu repsize %u\n",
		       i, PFID(&update->ou_fid),
		       update_op_str(update->ou_type),
		       update->ou_params_count,
		       update->ou_batchid, size,
		       (unsigned)update->ou_result_size);

		total_size += size;
	}

	CDEBUG(mask, "updates = %p magic = %x count = %d size = %zu\n", ourq,
	       ourq->ourq_magic, ourq->ourq_count, total_size);
}

/**
 * Prepare inline update request
 *
 * Prepare OUT update ptlrpc inline request, and the request usually includes
 * one update buffer, which does not need bulk transfer.
 *
 * \param[in] env	execution environment
 * \param[in] req	ptlrpc request
 * \param[in] ours	sub osp_update_request to be packed
 *
 * \retval		0 if packing succeeds
 * \retval		negative errno if packing fails
 */
int osp_prep_inline_update_req(const struct lu_env *env,
			       struct ptlrpc_request *req,
			       struct osp_update_request *our,
			       int repsize)
{
	struct osp_update_request_sub *ours;
	struct out_update_header *ouh;
	__u32 update_req_size;
	int rc;

	ours = list_entry(our->our_req_list.next,
			  struct osp_update_request_sub, ours_list);
	update_req_size = object_update_request_size(ours->ours_req);
	req_capsule_set_size(&req->rq_pill, &RMF_OUT_UPDATE_HEADER, RCL_CLIENT,
			     update_req_size + sizeof(*ouh));

	rc = ptlrpc_request_pack(req, LUSTRE_MDS_VERSION, OUT_UPDATE);
	if (rc != 0)
		RETURN(rc);

	ouh = req_capsule_client_get(&req->rq_pill, &RMF_OUT_UPDATE_HEADER);
	ouh->ouh_magic = OUT_UPDATE_HEADER_MAGIC;
	ouh->ouh_count = 1;
	ouh->ouh_inline_length = update_req_size;
	ouh->ouh_reply_size = repsize;

	memcpy(ouh->ouh_inline_data, ours->ours_req, update_req_size);

	req_capsule_set_size(&req->rq_pill, &RMF_OUT_UPDATE_REPLY,
			     RCL_SERVER, repsize);

	ptlrpc_request_set_replen(req);
	req->rq_request_portal = OUT_PORTAL;
	req->rq_reply_portal = OSC_REPLY_PORTAL;

	RETURN(rc);
}

/**
 * Prepare update request.
 *
 * Prepare OUT update ptlrpc request, and the request usually includes
 * all of updates (stored in \param ureq) from one operation.
 *
 * \param[in] env	execution environment
 * \param[in] imp	import on which ptlrpc request will be sent
 * \param[in] ureq	hold all of updates which will be packed into the req
 * \param[in] reqp	request to be created
 *
 * \retval		0 if preparation succeeds.
 * \retval		negative errno if preparation fails.
 */
int osp_prep_update_req(const struct lu_env *env, struct obd_import *imp,
			struct osp_update_request *our,
			struct ptlrpc_request **reqp)
{
	struct ptlrpc_request		*req;
	struct ptlrpc_bulk_desc		*desc;
	struct osp_update_request_sub	*ours;
	const struct object_update_request *ourq;
	struct out_update_header	*ouh;
	struct out_update_buffer	*oub;
	__u32				buf_count = 0;
	int				repsize = 0;
	struct object_update_reply	*reply;
	int				rc, i;
	int				total = 0;
	ENTRY;

	list_for_each_entry(ours, &our->our_req_list, ours_list) {
		object_update_request_dump(ours->ours_req, D_INFO);

		ourq = ours->ours_req;
		for (i = 0; i < ourq->ourq_count; i++) {
			struct object_update	*update;
			size_t			size = 0;


			/* XXX: it's very inefficient to lookup update
			 *	this way, iterating from the beginning
			 *	each time */
			update = object_update_request_get(ourq, i, &size);
			LASSERT(update != NULL);

			repsize += sizeof(reply->ourp_lens[0]);
			repsize += sizeof(struct object_update_result);
			repsize += update->ou_result_size;
		}

		buf_count++;
	}
	repsize += sizeof(*reply);
	repsize = (repsize + OUT_UPDATE_REPLY_SIZE - 1) &
			~(OUT_UPDATE_REPLY_SIZE - 1);
	LASSERT(buf_count > 0);

	req = ptlrpc_request_alloc(imp, &RQF_OUT_UPDATE);
	if (req == NULL)
		RETURN(-ENOMEM);

	if (buf_count == 1) {
		ours = list_entry(our->our_req_list.next,
				  struct osp_update_request_sub, ours_list);

		/* Let's check if it can be packed inline */
		if (object_update_request_size(ours->ours_req) +
		    sizeof(struct out_update_header) <
				OUT_UPDATE_MAX_INLINE_SIZE) {
			rc = osp_prep_inline_update_req(env, req, our, repsize);
			if (rc == 0)
				*reqp = req;
			GOTO(out_req, rc);
		}
	}

	req_capsule_set_size(&req->rq_pill, &RMF_OUT_UPDATE_HEADER, RCL_CLIENT,
			     sizeof(struct osp_update_request));

	req_capsule_set_size(&req->rq_pill, &RMF_OUT_UPDATE_BUF, RCL_CLIENT,
			     buf_count * sizeof(*oub));

	rc = ptlrpc_request_pack(req, LUSTRE_MDS_VERSION, OUT_UPDATE);
	if (rc != 0)
		GOTO(out_req, rc);

	ouh = req_capsule_client_get(&req->rq_pill, &RMF_OUT_UPDATE_HEADER);
	ouh->ouh_magic = OUT_UPDATE_HEADER_MAGIC;
	ouh->ouh_count = buf_count;
	ouh->ouh_inline_length = 0;
	ouh->ouh_reply_size = repsize;
	oub = req_capsule_client_get(&req->rq_pill, &RMF_OUT_UPDATE_BUF);
	list_for_each_entry(ours, &our->our_req_list, ours_list) {
		oub->oub_size = ours->ours_req_size;
		oub++;
	}

	req->rq_bulk_write = 1;
	desc = ptlrpc_prep_bulk_imp(req, buf_count,
		MD_MAX_BRW_SIZE >> LNET_MTU_BITS,
		PTLRPC_BULK_GET_SOURCE | PTLRPC_BULK_BUF_KVEC,
		MDS_BULK_PORTAL, &ptlrpc_bulk_kvec_ops);
	if (desc == NULL)
		GOTO(out_req, rc = -ENOMEM);

	/* NB req now owns desc and will free it when it gets freed */
	list_for_each_entry(ours, &our->our_req_list, ours_list) {
		desc->bd_frag_ops->add_iov_frag(desc, ours->ours_req,
						ours->ours_req_size);
		total += ours->ours_req_size;
	}
	CDEBUG(D_OTHER, "total %d in %u\n", total, our->our_update_nr);

	req_capsule_set_size(&req->rq_pill, &RMF_OUT_UPDATE_REPLY,
			     RCL_SERVER, repsize);

	ptlrpc_request_set_replen(req);
	req->rq_request_portal = OUT_PORTAL;
	req->rq_reply_portal = OSC_REPLY_PORTAL;
	*reqp = req;

out_req:
	if (rc < 0)
		ptlrpc_req_finished(req);

	RETURN(rc);
}

/**
 * Send update RPC.
 *
 * Send update request to the remote MDT synchronously.
 *
 * \param[in] env	execution environment
 * \param[in] imp	import on which ptlrpc request will be sent
 * \param[in] our	hold all of updates which will be packed into the req
 * \param[in] reqp	request to be created
 *
 * \retval		0 if RPC succeeds.
 * \retval		negative errno if RPC fails.
 */
int osp_remote_sync(const struct lu_env *env, struct osp_device *osp,
		    struct osp_update_request *our,
		    struct ptlrpc_request **reqp)
{
	struct obd_import	*imp = osp->opd_obd->u.cli.cl_import;
	struct ptlrpc_request	*req = NULL;
	int			rc;
	ENTRY;

	rc = osp_prep_update_req(env, imp, our, &req);
	if (rc != 0)
		RETURN(rc);

	osp_set_req_replay(osp, req);
	req->rq_allow_intr = 1;

	/* Note: some dt index api might return non-zero result here, like
	 * osd_index_ea_lookup, so we should only check rc < 0 here */
	rc = ptlrpc_queue_wait(req);
	our->our_rc = rc;
	if (rc < 0 || reqp == NULL)
		ptlrpc_req_finished(req);
	else
		*reqp = req;

	RETURN(rc);
}

/**
 * Invalidate all objects in the osp thandle
 *
 * invalidate all of objects in the update request, which will be called
 * when the transaction is aborted.
 *
 * \param[in] oth	osp thandle.
 */
static void osp_thandle_invalidate_object(const struct lu_env *env,
					  struct osp_thandle *oth)
{
	struct osp_update_request *our = oth->ot_our;
	struct osp_update_request_sub *ours;

	if (our == NULL)
		return;

	list_for_each_entry(ours, &our->our_req_list, ours_list) {
		struct object_update_request *our_req = ours->ours_req;
		unsigned int i;
		struct lu_object *obj;
		struct osp_object *osp_obj;

		for (i = 0; i < our_req->ourq_count; i++) {
			struct object_update *update;

			update = object_update_request_get(our_req, i, NULL);
			if (update == NULL)
				break;

			if (update->ou_type != OUT_WRITE)
				continue;

			if (!fid_is_sane(&update->ou_fid))
				continue;

			obj = lu_object_find_slice(env,
					&oth->ot_super.th_dev->dd_lu_dev,
					&update->ou_fid, NULL);
			if (IS_ERR(obj))
				break;

			osp_obj = lu2osp_obj(obj);
			if (osp_obj->opo_ooa != NULL) {
				spin_lock(&osp_obj->opo_lock);
				osp_obj->opo_ooa->ooa_attr.la_valid = 0;
				osp_obj->opo_stale = 1;
				spin_unlock(&osp_obj->opo_lock);
			}
			lu_object_put(env, obj);
		}
	}
}

static void osp_trans_stop_cb(const struct lu_env *env,
			      struct osp_thandle *oth, int result)
{
	struct dt_txn_commit_cb	*dcb;
	struct dt_txn_commit_cb	*tmp;

	/* call per-transaction stop callbacks if any */
	list_for_each_entry_safe(dcb, tmp, &oth->ot_stop_dcb_list,
				 dcb_linkage) {
		LASSERTF(dcb->dcb_magic == TRANS_COMMIT_CB_MAGIC,
			 "commit callback entry: magic=%x name='%s'\n",
			 dcb->dcb_magic, dcb->dcb_name);
		list_del_init(&dcb->dcb_linkage);
		dcb->dcb_func(NULL, &oth->ot_super, dcb, result);
	}

	if (result < 0)
		osp_thandle_invalidate_object(env, oth);
}

/**
 * Allocate an osp request and initialize it with the given parameters.
 *
 * \param[in] obj		pointer to the operation target
 * \param[in] data		pointer to the data used by the interpreter
 * \param[in] interpreter	pointer to the interpreter function
 *
 * \retval			pointer to the asychronous request
 * \retval			NULL if the allocation failed
 */
static struct osp_update_callback *
osp_update_callback_init(struct osp_object *obj, void *data,
			 osp_update_interpreter_t interpreter)
{
	struct osp_update_callback *ouc;

	OBD_ALLOC_PTR(ouc);
	if (ouc == NULL)
		return NULL;

	lu_object_get(osp2lu_obj(obj));
	INIT_LIST_HEAD(&ouc->ouc_list);
	ouc->ouc_obj = obj;
	ouc->ouc_data = data;
	ouc->ouc_interpreter = interpreter;

	return ouc;
}

/**
 * Destroy the osp_update_callback.
 *
 * \param[in] env	pointer to the thread context
 * \param[in] ouc	pointer to osp_update_callback
 */
static void osp_update_callback_fini(const struct lu_env *env,
				     struct osp_update_callback *ouc)
{
	LASSERT(list_empty(&ouc->ouc_list));

	lu_object_put(env, osp2lu_obj(ouc->ouc_obj));
	OBD_FREE_PTR(ouc);
}

/**
 * Interpret the packaged OUT RPC results.
 *
 * For every packaged sub-request, call its registered interpreter function.
 * Then destroy the sub-request.
 *
 * \param[in] env	pointer to the thread context
 * \param[in] req	pointer to the RPC
 * \param[in] arg	pointer to data used by the interpreter
 * \param[in] rc	the RPC return value
 *
 * \retval		0 for success
 * \retval		negative error number on failure
 */
static int osp_update_interpret(const struct lu_env *env,
				struct ptlrpc_request *req, void *arg, int rc)
{
	struct object_update_reply	*reply	= NULL;
	struct osp_update_args		*oaua	= arg;
	struct osp_update_request	*our = oaua->oaua_update;
	struct osp_thandle		*oth;
	struct osp_update_callback	*ouc;
	struct osp_update_callback	*next;
	int				 count	= 0;
	int				 index  = 0;
	int				 rc1	= 0;

	ENTRY;

	if (our == NULL)
		RETURN(0);

	oaua->oaua_update = NULL;
	oth = our->our_th;
	if (oaua->oaua_flow_control) {
		struct osp_device *osp;

		LASSERT(oth != NULL);
		osp = dt2osp_dev(oth->ot_super.th_dev);
		obd_put_request_slot(&osp->opd_obd->u.cli);
	}

	/* Unpack the results from the reply message. */
	if (req->rq_repmsg != NULL && req->rq_replied) {
		reply = req_capsule_server_sized_get(&req->rq_pill,
						     &RMF_OUT_UPDATE_REPLY,
						     OUT_UPDATE_REPLY_SIZE);
		if (reply == NULL || reply->ourp_magic != UPDATE_REPLY_MAGIC) {
			if (rc == 0)
				rc = -EPROTO;
		} else {
			count = reply->ourp_count;
		}
	}

	list_for_each_entry_safe(ouc, next, &our->our_cb_items, ouc_list) {
		list_del_init(&ouc->ouc_list);

		/* The peer may only have handled some requests (indicated
		 * by the 'count') in the packaged OUT RPC, we can only get
		 * results for the handled part. */
		if (index < count && reply->ourp_lens[index] > 0 && rc >= 0) {
			struct object_update_result *result;

			result = object_update_result_get(reply, index, NULL);
			if (result == NULL)
				rc1 = rc = -EPROTO;
			else
				rc1 = rc = result->our_rc;
		} else if (rc1 >= 0) {
			/* The peer did not handle these request, let's return
			 * -EINVAL to update interpret for now */
			if (rc >= 0)
				rc1 = -EINVAL;
			else
				rc1 = rc;
		}

		if (ouc->ouc_interpreter != NULL)
			ouc->ouc_interpreter(env, reply, req, ouc->ouc_obj,
					     ouc->ouc_data, index, rc1);

		osp_update_callback_fini(env, ouc);
		index++;
	}

	if (oaua->oaua_count != NULL && atomic_dec_and_test(oaua->oaua_count))
		wake_up_all(oaua->oaua_waitq);

	if (oth != NULL) {
		/* oth and osp_update_requests will be destoryed in
		 * osp_thandle_put */
		osp_trans_stop_cb(env, oth, rc);
		osp_thandle_put(oth);
	} else {
		osp_update_request_destroy(our);
	}

	RETURN(rc);
}

/**
 * Pack all the requests in the shared asynchronous idempotent request queue
 * into a single OUT RPC that will be given to the background ptlrpcd daemon.
 *
 * \param[in] env	pointer to the thread context
 * \param[in] osp	pointer to the OSP device
 * \param[in] our	pointer to the shared queue
 *
 * \retval		0 for success
 * \retval		negative error number on failure
 */
int osp_unplug_async_request(const struct lu_env *env,
			     struct osp_device *osp,
			     struct osp_update_request *our)
{
	struct osp_update_args	*args;
	struct ptlrpc_request	*req = NULL;
	int			 rc;

	rc = osp_prep_update_req(env, osp->opd_obd->u.cli.cl_import,
				 our, &req);
	if (rc != 0) {
		struct osp_update_callback *ouc;
		struct osp_update_callback *next;

		list_for_each_entry_safe(ouc, next,
					 &our->our_cb_items, ouc_list) {
			list_del_init(&ouc->ouc_list);
			if (ouc->ouc_interpreter != NULL)
				ouc->ouc_interpreter(env, NULL, NULL,
						     ouc->ouc_obj,
						     ouc->ouc_data, 0, rc);
			osp_update_callback_fini(env, ouc);
		}
		osp_update_request_destroy(our);
	} else {
		args = ptlrpc_req_async_args(req);
		args->oaua_update = our;
		args->oaua_count = NULL;
		args->oaua_waitq = NULL;
		args->oaua_flow_control = false;
		req->rq_interpret_reply = osp_update_interpret;
		ptlrpcd_add_req(req);
	}

	return rc;
}

/**
 * Find or create (if NOT exist or purged) the shared asynchronous idempotent
 * request queue - osp_device::opd_async_requests.
 *
 * If the osp_device::opd_async_requests is not NULL, then return it directly;
 * otherwise create new osp_update_request and attach it to opd_async_requests.
 *
 * \param[in] osp	pointer to the OSP device
 *
 * \retval		pointer to the shared queue
 * \retval		negative error number on failure
 */
static struct osp_update_request *
osp_find_or_create_async_update_request(struct osp_device *osp)
{
	struct osp_update_request *our = osp->opd_async_requests;

	if (our != NULL)
		return our;

	our = osp_update_request_create(&osp->opd_dt_dev);
	if (IS_ERR(our))
		return our;

	osp->opd_async_requests = our;

	return our;
}

/**
 * Insert an osp_update_callback into the osp_update_request.
 *
 * Insert an osp_update_callback to the osp_update_request. Usually each update
 * in the osp_update_request will have one correspondent callback, and these
 * callbacks will be called in rq_interpret_reply.
 *
 * \param[in] env		pointer to the thread context
 * \param[in] obj		pointer to the operation target object
 * \param[in] data		pointer to the data used by the interpreter
 * \param[in] interpreter	pointer to the interpreter function
 *
 * \retval			0 for success
 * \retval			negative error number on failure
 */
int osp_insert_update_callback(const struct lu_env *env,
			       struct osp_update_request *our,
			       struct osp_object *obj, void *data,
			       osp_update_interpreter_t interpreter)
{
	struct osp_update_callback  *ouc;

	ouc = osp_update_callback_init(obj, data, interpreter);
	if (ouc == NULL)
		RETURN(-ENOMEM);

	list_add_tail(&ouc->ouc_list, &our->our_cb_items);

	return 0;
}

/**
 * Insert an asynchronous idempotent request to the shared request queue that
 * is attached to the osp_device.
 *
 * This function generates a new osp_async_request with the given parameters,
 * then tries to insert the request into the osp_device-based shared request
 * queue. If the queue is full, then triggers the packaged OUT RPC to purge
 * the shared queue firstly, and then re-tries.
 *
 * NOTE: must hold the osp::opd_async_requests_mutex to serialize concurrent
 *	 osp_insert_async_request call from others.
 *
 * \param[in] env		pointer to the thread context
 * \param[in] op		operation type, see 'enum update_type'
 * \param[in] obj		pointer to the operation target
 * \param[in] count		array size of the subsequent \a lens and \a bufs
 * \param[in] lens		buffer length array for the subsequent \a bufs
 * \param[in] bufs		the buffers to compose the request
 * \param[in] data		pointer to the data used by the interpreter
 * \param[in] repsize		how many bytes the caller allocated for \a data
 * \param[in] interpreter	pointer to the interpreter function
 *
 * \retval			0 for success
 * \retval			negative error number on failure
 */
int osp_insert_async_request(const struct lu_env *env, enum update_type op,
			     struct osp_object *obj, int count,
			     __u16 *lens, const void **bufs,
			     void *data, __u32 repsize,
			     osp_update_interpreter_t interpreter)
{
	struct osp_device		*osp;
	struct osp_update_request	*our;
	struct object_update		*object_update;
	size_t				max_update_size;
	struct object_update_request	*ureq;
	struct osp_update_request_sub	*ours;
	int				rc = 0;
	ENTRY;

	osp = lu2osp_dev(osp2lu_obj(obj)->lo_dev);
	our = osp_find_or_create_async_update_request(osp);
	if (IS_ERR(our))
		RETURN(PTR_ERR(our));

again:
	ours = osp_current_object_update_request(our);

	ureq = ours->ours_req;
	max_update_size = ours->ours_req_size -
			  object_update_request_size(ureq);

	object_update = update_buffer_get_update(ureq, ureq->ourq_count);
	rc = out_update_pack(env, object_update, &max_update_size, op,
			     lu_object_fid(osp2lu_obj(obj)), count, lens, bufs,
			     repsize);
	/* The queue is full. */
	if (rc == -E2BIG) {
		osp->opd_async_requests = NULL;
		mutex_unlock(&osp->opd_async_requests_mutex);

		rc = osp_unplug_async_request(env, osp, our);
		mutex_lock(&osp->opd_async_requests_mutex);
		if (rc != 0)
			RETURN(rc);

		our = osp_find_or_create_async_update_request(osp);
		if (IS_ERR(our))
			RETURN(PTR_ERR(our));

		goto again;
	} else {
		if (rc < 0)
			RETURN(rc);

		ureq->ourq_count++;
		our->our_update_nr++;
	}

	rc = osp_insert_update_callback(env, our, obj, data, interpreter);

	RETURN(rc);
}

int osp_trans_update_request_create(struct thandle *th)
{
	struct osp_thandle		*oth = thandle_to_osp_thandle(th);
	struct osp_update_request	*our;

	if (oth->ot_our != NULL)
		return 0;

	our = osp_update_request_create(th->th_dev);
	if (IS_ERR(our)) {
		th->th_result = PTR_ERR(our);
		return PTR_ERR(our);
	}

	oth->ot_our = our;
	our->our_th = oth;

	return 0;
}

void osp_thandle_destroy(struct osp_thandle *oth)
{
	LASSERT(oth->ot_magic == OSP_THANDLE_MAGIC);
	LASSERT(list_empty(&oth->ot_commit_dcb_list));
	LASSERT(list_empty(&oth->ot_stop_dcb_list));
	if (oth->ot_our != NULL)
		osp_update_request_destroy(oth->ot_our);
	OBD_FREE_PTR(oth);
}

/**
 * The OSP layer dt_device_operations::dt_trans_create() interface
 * to create a transaction.
 *
 * There are two kinds of transactions that will involve OSP:
 *
 * 1) If the transaction only contains the updates on remote server
 *    (MDT or OST), such as re-generating the lost OST-object for
 *    LFSCK, then it is a remote transaction. For remote transaction,
 *    the upper layer caller (such as the LFSCK engine) will call the
 *    dt_trans_create() (with the OSP dt_device as the parameter),
 *    then the call will be directed to the osp_trans_create() that
 *    creates the transaction handler and returns it to the caller.
 *
 * 2) If the transcation contains both local and remote updates,
 *    such as cross MDTs create under DNE mode, then the upper layer
 *    caller will not trigger osp_trans_create(). Instead, it will
 *    call dt_trans_create() on other dt_device, such as LOD that
 *    will generate the transaction handler. Such handler will be
 *    used by the whole transaction in subsequent sub-operations.
 *
 * \param[in] env	pointer to the thread context
 * \param[in] d		pointer to the OSP dt_device
 *
 * \retval		pointer to the transaction handler
 * \retval		negative error number on failure
 */
struct thandle *osp_trans_create(const struct lu_env *env, struct dt_device *d)
{
	struct osp_thandle		*oth;
	struct thandle			*th = NULL;
	ENTRY;

	OBD_ALLOC_PTR(oth);
	if (unlikely(oth == NULL))
		RETURN(ERR_PTR(-ENOMEM));

	oth->ot_magic = OSP_THANDLE_MAGIC;
	th = &oth->ot_super;
	th->th_dev = d;
	th->th_tags = LCT_TX_HANDLE;

	atomic_set(&oth->ot_refcount, 1);
	INIT_LIST_HEAD(&oth->ot_commit_dcb_list);
	INIT_LIST_HEAD(&oth->ot_stop_dcb_list);

	RETURN(th);
}

/**
 * Add commit callback to transaction.
 *
 * Add commit callback to the osp thandle, which will be called
 * when the thandle is committed remotely.
 *
 * \param[in] th	the thandle
 * \param[in] dcb	commit callback structure
 *
 * \retval		only return 0 for now.
 */
int osp_trans_cb_add(struct thandle *th, struct dt_txn_commit_cb *dcb)
{
	struct osp_thandle *oth = thandle_to_osp_thandle(th);

	LASSERT(dcb->dcb_magic == TRANS_COMMIT_CB_MAGIC);
	LASSERT(&dcb->dcb_func != NULL);
	if (dcb->dcb_flags & DCB_TRANS_STOP)
		list_add(&dcb->dcb_linkage, &oth->ot_stop_dcb_list);
	else
		list_add(&dcb->dcb_linkage, &oth->ot_commit_dcb_list);
	return 0;
}

static void osp_trans_commit_cb(struct osp_thandle *oth, int result)
{
	struct dt_txn_commit_cb *dcb;
	struct dt_txn_commit_cb *tmp;

	LASSERT(atomic_read(&oth->ot_refcount) > 0);
	/* call per-transaction callbacks if any */
	list_for_each_entry_safe(dcb, tmp, &oth->ot_commit_dcb_list,
				 dcb_linkage) {
		LASSERTF(dcb->dcb_magic == TRANS_COMMIT_CB_MAGIC,
			 "commit callback entry: magic=%x name='%s'\n",
			 dcb->dcb_magic, dcb->dcb_name);
		list_del_init(&dcb->dcb_linkage);
		dcb->dcb_func(NULL, &oth->ot_super, dcb, result);
	}
}

static void osp_request_commit_cb(struct ptlrpc_request *req)
{
	struct thandle		*th = req->rq_cb_data;
	struct osp_thandle	*oth;
	__u64			last_committed_transno = 0;
	int			result = req->rq_status;
	ENTRY;

	if (th == NULL)
		RETURN_EXIT;

	oth = thandle_to_osp_thandle(th);
	if (req->rq_repmsg != NULL &&
	    lustre_msg_get_last_committed(req->rq_repmsg))
		last_committed_transno =
			lustre_msg_get_last_committed(req->rq_repmsg);

	if (last_committed_transno <
		req->rq_import->imp_peer_committed_transno)
		last_committed_transno =
			req->rq_import->imp_peer_committed_transno;

	CDEBUG(D_HA, "trans no "LPU64" committed transno "LPU64"\n",
	       req->rq_transno, last_committed_transno);

	/* If the transaction is not really committed, mark result = 1 */
	if (req->rq_transno != 0 &&
	    (req->rq_transno > last_committed_transno) && result == 0)
		result = 1;

	osp_trans_commit_cb(oth, result);
	req->rq_committed = 1;
	osp_thandle_put(oth);
	EXIT;
}

/**
 * callback of osp transaction
 *
 * Call all of callbacks for this osp thandle. This will only be
 * called in error handler path. In the normal processing path,
 * these callback will be called in osp_request_commit_cb() and
 * osp_update_interpret().
 *
 * \param [in] env	execution environment
 * \param [in] oth	osp thandle
 * \param [in] rc	result of the osp thandle
 */
void osp_trans_callback(const struct lu_env *env,
			struct osp_thandle *oth, int rc)
{
	struct osp_update_callback *ouc;
	struct osp_update_callback *next;

	if (oth->ot_our != NULL) {
		list_for_each_entry_safe(ouc, next,
					 &oth->ot_our->our_cb_items, ouc_list) {
			list_del_init(&ouc->ouc_list);
			if (ouc->ouc_interpreter != NULL)
				ouc->ouc_interpreter(env, NULL, NULL,
						     ouc->ouc_obj,
						     ouc->ouc_data, 0, rc);
			osp_update_callback_fini(env, ouc);
		}
	}
	osp_trans_stop_cb(env, oth, rc);
	osp_trans_commit_cb(oth, rc);
}

/**
 * Send the request for remote updates.
 *
 * Send updates to the remote MDT. Prepare the request by osp_update_req
 * and send them to remote MDT, for sync request, it will wait
 * until the reply return, otherwise hand it to ptlrpcd.
 *
 * Please refer to osp_trans_create() for transaction type.
 *
 * \param[in] env		pointer to the thread context
 * \param[in] osp		pointer to the OSP device
 * \param[in] our		pointer to the osp_update_request
 *
 * \retval			0 for success
 * \retval			negative error number on failure
 */
static int osp_send_update_req(const struct lu_env *env,
			       struct osp_device *osp,
			       struct osp_update_request *our)
{
	struct osp_update_args	*args;
	struct ptlrpc_request	*req;
	struct osp_thandle	*oth = our->our_th;
	int	rc = 0;
	ENTRY;

	LASSERT(oth != NULL);
	rc = osp_prep_update_req(env, osp->opd_obd->u.cli.cl_import,
				 our, &req);
	if (rc != 0) {
		osp_trans_callback(env, oth, rc);
		RETURN(rc);
	}

	args = ptlrpc_req_async_args(req);
	args->oaua_update = our;
	osp_thandle_get(oth); /* hold for update interpret */
	req->rq_interpret_reply = osp_update_interpret;
	if (!oth->ot_super.th_wait_submit && !oth->ot_super.th_sync) {
		if (!osp->opd_imp_active || !osp->opd_imp_connected) {
			osp_trans_callback(env, oth, rc);
			osp_thandle_put(oth);
			GOTO(out, rc = -ENOTCONN);
		}

		rc = obd_get_request_slot(&osp->opd_obd->u.cli);
		if (rc != 0) {
			osp_trans_callback(env, oth, rc);
			osp_thandle_put(oth);
			GOTO(out, rc = -ENOTCONN);
		}
		args->oaua_flow_control = true;

		if (!osp->opd_connect_mdt) {
			down_read(&osp->opd_async_updates_rwsem);
			args->oaua_count = &osp->opd_async_updates_count;
			args->oaua_waitq = &osp->opd_syn_barrier_waitq;
			up_read(&osp->opd_async_updates_rwsem);
			atomic_inc(args->oaua_count);
		}

		ptlrpcd_add_req(req);
		req = NULL;
	} else {
		osp_thandle_get(oth); /* hold for commit callback */
		req->rq_commit_cb = osp_request_commit_cb;
		req->rq_cb_data = &oth->ot_super;
		args->oaua_flow_control = false;

		/* If the transaction is created during MDT recoverying
		 * process, it means this is an recovery update, we need
		 * to let OSP send it anyway without checking recoverying
		 * status, in case the other target is being recoveried
		 * at the same time, and if we wait here for the import
		 * to be recoveryed, it might cause deadlock */
		osp_set_req_replay(osp, req);

		if (osp->opd_connect_mdt)
			osp_get_rpc_lock(osp);
		rc = ptlrpc_queue_wait(req);
		if (osp->opd_connect_mdt)
			osp_put_rpc_lock(osp);
		if ((rc == -ENOMEM && req->rq_set == NULL) ||
		    (req->rq_transno == 0 && !req->rq_committed)) {
			if (args->oaua_update != NULL) {
				/* If osp_update_interpret is not being called,
				 * release the osp_thandle */
				args->oaua_update = NULL;
				osp_thandle_put(oth);
			}

			req->rq_cb_data = NULL;
			rc = rc == 0 ? req->rq_status : rc;
			osp_trans_callback(env, oth, rc);
			osp_thandle_put(oth);
			GOTO(out, rc);
		}
	}
out:
	if (req != NULL)
		ptlrpc_req_finished(req);

	RETURN(rc);
}

/**
 * Get local thandle for osp_thandle
 *
 * Get the local OSD thandle from the OSP thandle. Currently, there
 * are a few OSP API (osp_object_create() and osp_sync_add()) needs
 * to update the object on local OSD device.
 *
 * If the osp_thandle comes from normal stack (MDD->LOD->OSP), then
 * we will get local thandle by thandle_get_sub_by_dt.
 *
 * If the osp_thandle is remote thandle (th_top == NULL, only used
 * by LFSCK), then it will create a local thandle, and stop it in
 * osp_trans_stop(). And this only happens on OSP for OST.
 *
 * These are temporary solution, once OSP accessing OSD object is
 * being fixed properly, this function should be removed. XXX
 *
 * \param[in] env		pointer to the thread context
 * \param[in] th		pointer to the transaction handler
 * \param[in] dt		pointer to the OSP device
 *
 * \retval			pointer to the local thandle
 * \retval			ERR_PTR(errno) if it fails.
 **/
struct thandle *osp_get_storage_thandle(const struct lu_env *env,
					struct thandle *th,
					struct osp_device *osp)
{
	struct osp_thandle	*oth;
	struct thandle		*local_th;

	if (th->th_top != NULL)
		return thandle_get_sub_by_dt(env, th->th_top,
					     osp->opd_storage);

	LASSERT(!osp->opd_connect_mdt);
	oth = thandle_to_osp_thandle(th);
	if (oth->ot_storage_th != NULL)
		return oth->ot_storage_th;

	local_th = dt_trans_create(env, osp->opd_storage);
	if (IS_ERR(local_th))
		return local_th;

	oth->ot_storage_th = local_th;

	return local_th;
}

/**
 * Set version for the transaction
 *
 * Set the version for the transaction and add the request to
 * the sending list, then after transaction stop, the request
 * will be picked in the order of version, by sending thread.
 *
 * \param [in] oth	osp thandle to be set version.
 *
 * \retval		0 if set version succeeds
 *                      negative errno if set version fails.
 */
int osp_check_and_set_rpc_version(struct osp_thandle *oth,
				  struct osp_object *obj)
{
	struct osp_device *osp = dt2osp_dev(oth->ot_super.th_dev);
	struct osp_updates *ou = osp->opd_update;

	if (ou == NULL)
		return -EIO;

	if (oth->ot_our->our_version != 0)
		return 0;

	spin_lock(&ou->ou_lock);
	spin_lock(&oth->ot_our->our_list_lock);
	if (obj->opo_stale) {
		spin_unlock(&oth->ot_our->our_list_lock);
		spin_unlock(&ou->ou_lock);
		return -ESTALE;
	}

	/* Assign the version and add it to the sending list */
	osp_thandle_get(oth);
	oth->ot_our->our_version = ou->ou_version++;
	list_add_tail(&oth->ot_our->our_list,
		      &osp->opd_update->ou_list);
	oth->ot_our->our_req_ready = 0;
	spin_unlock(&oth->ot_our->our_list_lock);
	spin_unlock(&ou->ou_lock);

	LASSERT(oth->ot_super.th_wait_submit == 1);
	CDEBUG(D_INFO, "%s: version "LPU64" oth:version %p:"LPU64"\n",
	       osp->opd_obd->obd_name, ou->ou_version, oth,
	       oth->ot_our->our_version);

	return 0;
}

/**
 * Get next OSP update request in the sending list
 * Get next OSP update request in the sending list by version number, next
 * request will be
 * 1. transaction which does not have a version number.
 * 2. transaction whose version == opd_rpc_version.
 *
 * \param [in] ou	osp update structure.
 * \param [out] ourp	the pointer holding the next update request.
 *
 * \retval		true if getting the next transaction.
 * \retval		false if not getting the next transaction.
 */
static bool
osp_get_next_request(struct osp_updates *ou, struct osp_update_request **ourp)
{
	struct osp_update_request *our;
	struct osp_update_request *tmp;
	bool			got_req = false;

	spin_lock(&ou->ou_lock);
	list_for_each_entry_safe(our, tmp, &ou->ou_list, our_list) {
		LASSERT(our->our_th != NULL);
		CDEBUG(D_HA, "ou %p version "LPU64" rpc_version "LPU64"\n",
		       ou, our->our_version, ou->ou_rpc_version);
		spin_lock(&our->our_list_lock);
		/* Find next osp_update_request in the list */
		if (our->our_version == ou->ou_rpc_version &&
		    our->our_req_ready) {
			list_del_init(&our->our_list);
			spin_unlock(&our->our_list_lock);
			*ourp = our;
			got_req = true;
			break;
		}
		spin_unlock(&our->our_list_lock);
	}
	spin_unlock(&ou->ou_lock);

	return got_req;
}

/**
 * Invalidate update request
 *
 * Invalidate update request in the OSP sending list, so all of
 * requests in the sending list will return error, which happens
 * when it finds one update (with writing llog) requests fails or
 * the OSP is evicted by remote target. see osp_send_update_thread().
 *
 * \param[in] osp	OSP device whose update requests will be
 *                      invalidated.
 **/
void osp_invalidate_request(struct osp_device *osp)
{
	struct lu_env env;
	struct osp_updates *ou = osp->opd_update;
	struct osp_update_request *our;
	struct osp_update_request *tmp;
	LIST_HEAD(list);
	int			rc;
	ENTRY;

	if (ou == NULL)
		return;

	rc = lu_env_init(&env, osp->opd_dt_dev.dd_lu_dev.ld_type->ldt_ctx_tags);
	if (rc < 0) {
		CERROR("%s: init env error: rc = %d\n", osp->opd_obd->obd_name,
		       rc);
		return;
	}

	INIT_LIST_HEAD(&list);

	spin_lock(&ou->ou_lock);
	/* invalidate all of request in the sending list */
	list_for_each_entry_safe(our, tmp, &ou->ou_list, our_list) {
		spin_lock(&our->our_list_lock);
		if (our->our_req_ready)
			list_move(&our->our_list, &list);
		else
			list_del_init(&our->our_list);

		if (our->our_th->ot_super.th_result == 0)
			our->our_th->ot_super.th_result = -EIO;

		if (our->our_version >= ou->ou_rpc_version)
			ou->ou_rpc_version = our->our_version + 1;
		spin_unlock(&our->our_list_lock);

		CDEBUG(D_HA, "%s invalidate our %p\n", osp->opd_obd->obd_name,
		       our);
	}

	spin_unlock(&ou->ou_lock);

	/* invalidate all of request in the sending list */
	list_for_each_entry_safe(our, tmp, &list, our_list) {
		spin_lock(&our->our_list_lock);
		list_del_init(&our->our_list);
		spin_unlock(&our->our_list_lock);
		osp_trans_callback(&env, our->our_th,
				   our->our_th->ot_super.th_result);
		osp_thandle_put(our->our_th);
	}
	lu_env_fini(&env);
}

/**
 * Sending update thread
 *
 * Create thread to send update request to other MDTs, this thread will pull
 * out update request from the list in OSP by version number, i.e. it will
 * make sure the update request with lower version number will be sent first.
 *
 * \param[in] arg	hold the OSP device.
 *
 * \retval		0 if the thread is created successfully.
 * \retal		negative error if the thread is not created
 *                      successfully.
 */
int osp_send_update_thread(void *arg)
{
	struct lu_env		env;
	struct osp_device	*osp = arg;
	struct l_wait_info	 lwi = { 0 };
	struct osp_updates	*ou = osp->opd_update;
	struct ptlrpc_thread	*thread = &osp->opd_update_thread;
	struct osp_update_request *our = NULL;
	int			rc;
	ENTRY;

	LASSERT(ou != NULL);
	rc = lu_env_init(&env, osp->opd_dt_dev.dd_lu_dev.ld_type->ldt_ctx_tags);
	if (rc < 0) {
		CERROR("%s: init env error: rc = %d\n", osp->opd_obd->obd_name,
		       rc);
		RETURN(rc);
	}

	thread->t_flags = SVC_RUNNING;
	wake_up(&thread->t_ctl_waitq);
	while (1) {
		our = NULL;
		l_wait_event(ou->ou_waitq,
			     !osp_send_update_thread_running(osp) ||
			     osp_get_next_request(ou, &our), &lwi);

		if (!osp_send_update_thread_running(osp)) {
			if (our != NULL) {
				osp_trans_callback(&env, our->our_th, -EINTR);
				osp_thandle_put(our->our_th);
			}
			break;
		}

		LASSERT(our->our_th != NULL);
		if (our->our_th->ot_super.th_result != 0) {
			osp_trans_callback(&env, our->our_th,
				our->our_th->ot_super.th_result);
			rc = our->our_th->ot_super.th_result;
		} else if (OBD_FAIL_CHECK(OBD_FAIL_INVALIDATE_UPDATE)) {
			rc = -EIO;
			osp_trans_callback(&env, our->our_th, rc);
		} else {
			rc = osp_send_update_req(&env, osp, our);
		}

		/* Update the rpc version */
		spin_lock(&ou->ou_lock);
		if (our->our_version == ou->ou_rpc_version)
			ou->ou_rpc_version++;
		spin_unlock(&ou->ou_lock);

		/* If one update request fails, let's fail all of the requests
		 * in the sending list, because the request in the sending
		 * list are dependent on either other, continue sending these
		 * request might cause llog or filesystem corruption */
		if (rc < 0)
			osp_invalidate_request(osp);

		/* Balanced for thandle_get in osp_check_and_set_rpc_version */
		osp_thandle_put(our->our_th);
	}

	thread->t_flags = SVC_STOPPED;
	lu_env_fini(&env);
	wake_up(&thread->t_ctl_waitq);

	RETURN(0);
}

/**
 * The OSP layer dt_device_operations::dt_trans_start() interface
 * to start the transaction.
 *
 * If the transaction is a remote transaction, then related remote
 * updates will be triggered in the osp_trans_stop().
 * Please refer to osp_trans_create() for transaction type.
 *
 * \param[in] env		pointer to the thread context
 * \param[in] dt		pointer to the OSP dt_device
 * \param[in] th		pointer to the transaction handler
 *
 * \retval			0 for success
 * \retval			negative error number on failure
 */
int osp_trans_start(const struct lu_env *env, struct dt_device *dt,
		    struct thandle *th)
{
	struct osp_thandle	*oth = thandle_to_osp_thandle(th);

	if (oth->ot_super.th_sync)
		oth->ot_our->our_flags |= UPDATE_FL_SYNC;
	/* For remote thandle, if there are local thandle, start it here*/
	if (is_only_remote_trans(th) && oth->ot_storage_th != NULL)
		return dt_trans_start(env, oth->ot_storage_th->th_dev,
				      oth->ot_storage_th);
	return 0;
}

/**
 * The OSP layer dt_device_operations::dt_trans_stop() interface
 * to stop the transaction.
 *
 * If the transaction is a remote transaction, related remote
 * updates will be triggered at the end of this function.
 *
 * For synchronous mode update or any failed update, the request
 * will be destroyed explicitly when the osp_trans_stop().
 *
 * Please refer to osp_trans_create() for transaction type.
 *
 * \param[in] env		pointer to the thread context
 * \param[in] dt		pointer to the OSP dt_device
 * \param[in] th		pointer to the transaction handler
 *
 * \retval			0 for success
 * \retval			negative error number on failure
 */
int osp_trans_stop(const struct lu_env *env, struct dt_device *dt,
		   struct thandle *th)
{
	struct osp_thandle	 *oth = thandle_to_osp_thandle(th);
	struct osp_update_request *our = oth->ot_our;
	struct osp_device	 *osp = dt2osp_dev(dt);
	int			 rc = 0;
	ENTRY;

	/* For remote transaction, if there is local storage thandle,
	 * stop it first */
	if (oth->ot_storage_th != NULL && th->th_top == NULL) {
		dt_trans_stop(env, oth->ot_storage_th->th_dev,
			      oth->ot_storage_th);
		oth->ot_storage_th = NULL;
	}

	if (our == NULL || list_empty(&our->our_req_list)) {
		osp_trans_callback(env, oth, th->th_result);
		GOTO(out, rc = th->th_result);
	}

	if (!osp->opd_connect_mdt) {
		osp_trans_callback(env, oth, th->th_result);
		rc = osp_send_update_req(env, osp, oth->ot_our);
		GOTO(out, rc);
	}

	if (osp->opd_update == NULL ||
	    !osp_send_update_thread_running(osp)) {
		osp_trans_callback(env, oth, -EIO);
		GOTO(out, rc = -EIO);
	}

	CDEBUG(D_HA, "%s: add oth %p with version "LPU64"\n",
	       osp->opd_obd->obd_name, oth, our->our_version);

	LASSERT(our->our_req_ready == 0);
	spin_lock(&our->our_list_lock);
	if (likely(!list_empty(&our->our_list))) {
		/* notify sending thread */
		our->our_req_ready = 1;
		wake_up(&osp->opd_update->ou_waitq);
		spin_unlock(&our->our_list_lock);
	} else if (th->th_result == 0) {
		/* if the request does not needs to be serialized,
		 * read-only request etc, let's send it right away */
		spin_unlock(&our->our_list_lock);
		rc = osp_send_update_req(env, osp, our);
	} else {
		spin_unlock(&our->our_list_lock);
		osp_trans_callback(env, oth, th->th_result);
	}
out:
	osp_thandle_put(oth);

	RETURN(rc);
}
