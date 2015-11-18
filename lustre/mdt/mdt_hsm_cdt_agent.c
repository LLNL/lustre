/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * (C) Copyright 2012 Commissariat a l'energie atomique et aux energies
 *     alternatives
 *
 */
/*
 * lustre/mdt/mdt_hsm_cdt_agent.c
 *
 * Lustre HSM Coordinator
 *
 * Author: Jacques-Charles Lafoucriere <jacques-charles.lafoucriere@cea.fr>
 * Author: Aurelien Degremont <aurelien.degremont@cea.fr>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <obd.h>
#include <obd_support.h>
#include <lustre_export.h>
#include <lustre/lustre_user.h>
#include <lprocfs_status.h>
#include "mdt_internal.h"

/*
 * Agent external API
 */

/*
 * find a hsm_agent by uuid
 * lock cdt_agent_lock needs to be hold by caller
 * \param cdt [IN] coordinator
 * \param uuid [IN] agent UUID
 * \retval hsm_agent pointer or NULL if not found
 */
static struct hsm_agent *mdt_hsm_agent_lookup(struct coordinator *cdt,
					      const struct obd_uuid *uuid)
{
	struct hsm_agent	*ha;

	list_for_each_entry(ha, &cdt->cdt_agents, ha_list) {
		if (obd_uuid_equals(&ha->ha_uuid, uuid))
			return ha;
	}
	return NULL;
}

/**
 * register a copy tool
 * \param mti [IN] MDT context
 * \param uuid [IN] client UUID to be registered
 * \param count [IN] number of archives agent serves
 * \param archive_id [IN] vector of archive number served by the copytool
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_agent_register(struct mdt_thread_info *mti,
			   const struct obd_uuid *uuid,
			   int nr_archives, __u32 *archive_id)
{
	struct coordinator	*cdt = &mti->mti_mdt->mdt_coordinator;
	struct hsm_agent	*ha, *tmp;
	int			 rc;
	ENTRY;

	/* no coordinator started, so we cannot serve requests */
	if (cdt->cdt_state == CDT_STOPPED) {
		LCONSOLE_WARN("HSM coordinator thread is not running - "
			      "denying agent registration.\n");
		RETURN(-ENXIO);
	}

	OBD_ALLOC_PTR(ha);
	if (ha == NULL)
		GOTO(out, rc = -ENOMEM);

	ha->ha_uuid = *uuid;
	ha->ha_archive_cnt = nr_archives;
	if (ha->ha_archive_cnt != 0) {
		int sz;

		sz = ha->ha_archive_cnt * sizeof(*ha->ha_archive_id);
		OBD_ALLOC(ha->ha_archive_id, sz);
		if (ha->ha_archive_id == NULL)
			GOTO(out_free, rc = -ENOMEM);
		memcpy(ha->ha_archive_id, archive_id, sz);
	}
	atomic_set(&ha->ha_requests, 0);
	atomic_set(&ha->ha_success, 0);
	atomic_set(&ha->ha_failure, 0);

	down_write(&cdt->cdt_agent_lock);
	tmp = mdt_hsm_agent_lookup(cdt, uuid);
	if (tmp != NULL) {
		LCONSOLE_WARN("HSM agent %s already registered\n",
			      obd_uuid2str(uuid));
		up_write(&cdt->cdt_agent_lock);
		GOTO(out_free, rc = -EEXIST);
	}

	list_add_tail(&ha->ha_list, &cdt->cdt_agents);

	if (ha->ha_archive_cnt == 0)
		CDEBUG(D_HSM, "agent %s registered for all archives\n",
		       obd_uuid2str(&ha->ha_uuid));
	else
		CDEBUG(D_HSM, "agent %s registered for %d archives\n",
		       obd_uuid2str(&ha->ha_uuid), ha->ha_archive_cnt);

	up_write(&cdt->cdt_agent_lock);
	GOTO(out, rc = 0);

out_free:

	if (ha != NULL && ha->ha_archive_id != NULL)
		OBD_FREE(ha->ha_archive_id,
			 ha->ha_archive_cnt * sizeof(*ha->ha_archive_id));
	if (ha != NULL)
		OBD_FREE_PTR(ha);
out:
	return rc;
}

/**
 * register a copy tool
 * \param mti [IN] MDT context
 * \param uuid [IN] uuid to be registered
 * \param archive_mask [IN] bitmask of archive number served by the copytool
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_agent_register_mask(struct mdt_thread_info *mti,
				const struct obd_uuid *uuid, __u32 archive_mask)
{
	int		 rc, i, nr_archives = 0;
	__u32		*archive_id = NULL;
	ENTRY;

	nr_archives = hweight32(archive_mask);

	if (nr_archives != 0) {
		OBD_ALLOC(archive_id, nr_archives * sizeof(*archive_id));
		if (!archive_id)
			RETURN(-ENOMEM);

		nr_archives = 0;
		for (i = 0; i < sizeof(archive_mask) * 8; i++) {
			if ((1 << i) & archive_mask) {
				archive_id[nr_archives] = i + 1;
				nr_archives++;
			}
		}
	}

	rc = mdt_hsm_agent_register(mti, uuid, nr_archives, archive_id);

	if (archive_id != NULL)
		OBD_FREE(archive_id, nr_archives * sizeof(*archive_id));

	RETURN(rc);
}

/**
 * unregister a copy tool
 * \param mti [IN] MDT context
 * \param uuid [IN] uuid to be unregistered
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_agent_unregister(struct mdt_thread_info *mti,
			     const struct obd_uuid *uuid)
{
	struct coordinator	*cdt = &mti->mti_mdt->mdt_coordinator;
	struct hsm_agent	*ha;
	int			 rc;
	ENTRY;

	/* no coordinator started, so we cannot serve requests */
	if (cdt->cdt_state == CDT_STOPPED)
		RETURN(-ENXIO);

	down_write(&cdt->cdt_agent_lock);

	ha = mdt_hsm_agent_lookup(cdt, uuid);
	if (ha != NULL)
		list_del_init(&ha->ha_list);

	up_write(&cdt->cdt_agent_lock);

	if (ha == NULL)
		GOTO(out, rc = -ENOENT);

	if (ha->ha_archive_cnt != 0)
		OBD_FREE(ha->ha_archive_id,
			 ha->ha_archive_cnt * sizeof(*ha->ha_archive_id));
	OBD_FREE_PTR(ha);

	GOTO(out, rc = 0);
out:
	CDEBUG(D_HSM, "agent %s unregistration: %d\n", obd_uuid2str(uuid), rc);

	return rc;
}

/**
 * update agent statistics
 * \param mdt [IN] MDT device
 * \param succ_rq [IN] number of success
 * \param fail_rq [IN] number of failure
 * \param new_rq [IN] number of new requests
 * \param uuid [IN] agent uuid
 * if all counters == 0, clear counters
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_agent_update_statistics(struct coordinator *cdt,
				    int succ_rq, int fail_rq, int new_rq,
				    const struct obd_uuid *uuid)
{
	struct hsm_agent	*ha;
	int			 rc;
	ENTRY;

	down_read(&cdt->cdt_agent_lock);
	list_for_each_entry(ha, &cdt->cdt_agents, ha_list) {
		if (obd_uuid_equals(&ha->ha_uuid, uuid)) {
			if (succ_rq == 0 && fail_rq == 0 && new_rq == 0) {
				atomic_set(&ha->ha_success, 0);
				atomic_set(&ha->ha_failure, 0);
				atomic_set(&ha->ha_requests, 0);
			} else {
				atomic_add(succ_rq, &ha->ha_success);
				atomic_add(fail_rq, &ha->ha_failure);
				atomic_add(new_rq, &ha->ha_requests);
				atomic_sub(succ_rq, &ha->ha_requests);
				atomic_sub(fail_rq, &ha->ha_requests);
			}
			GOTO(out, rc = 0);
		}

	}
	rc = -ENOENT;
out:
	up_read(&cdt->cdt_agent_lock);
	RETURN(rc);
}

/**
 * find the best agent
 * \param cdt [IN] coordinator
 * \param archive [IN] archive number
 * \param uuid [OUT] agent who can serve archive
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_find_best_agent(struct coordinator *cdt, __u32 archive,
			    struct obd_uuid *uuid)
{
	int			 rc = -EAGAIN, i, load = -1;
	struct hsm_agent	*ha;
	ENTRY;

	/* Choose an export to send a copytool req to */
	down_read(&cdt->cdt_agent_lock);
	list_for_each_entry(ha, &cdt->cdt_agents, ha_list) {
		for (i = 0; (i < ha->ha_archive_cnt) &&
			      (ha->ha_archive_id[i] != archive); i++) {
			/* nothing to do, just skip unmatching records */
		}

		/* archive count == 0 means copy tool serves any backend */
		if (ha->ha_archive_cnt != 0 && i == ha->ha_archive_cnt)
			continue;

		if (load == -1 || load > atomic_read(&ha->ha_requests)) {
			load = atomic_read(&ha->ha_requests);
			*uuid = ha->ha_uuid;
			rc = 0;
		}
		if (atomic_read(&ha->ha_requests) == 0)
			break;
	}
	up_read(&cdt->cdt_agent_lock);

	RETURN(rc);
}

/**
 * send a compound request to the agent
 * \param mti [IN] context
 * \param hal [IN] request (can be a kuc payload)
 * \param purge [IN] purge mode (no record)
 * \retval 0 success
 * \retval -ve failure
 * This function supposes:
 *  - all actions are for the same archive number
 *  - in case of cancel, all cancel are for the same agent
 * This implies that request split has to be done
 *  before when building the hal
 */
int mdt_hsm_agent_send(struct mdt_thread_info *mti,
		       struct hsm_action_list *hal, bool purge)
{
	struct obd_export	*exp;
	struct mdt_device	*mdt = mti->mti_mdt;
	struct coordinator	*cdt = &mti->mti_mdt->mdt_coordinator;
	struct hsm_action_list	*buf = NULL;
	struct hsm_action_item	*hai;
	struct obd_uuid		 uuid;
	int			 len, i, rc = 0;
	bool			 fail_request;
	bool			 is_registered = false;
	ENTRY;

	rc = mdt_hsm_find_best_agent(cdt, hal->hal_archive_id, &uuid);
	if (rc) {
		CERROR("%s: Cannot find agent for archive %d: rc = %d\n",
		       mdt_obd_name(mdt), hal->hal_archive_id, rc);
		RETURN(rc);
	}

	CDEBUG(D_HSM, "Agent %s selected for archive %d\n", obd_uuid2str(&uuid),
	       hal->hal_archive_id);

	len = hal_size(hal);
	if (kuc_ispayload(hal)) {
		/* hal is already a kuc payload
		 * we do not need to alloc a new one
		 * this avoid a alloc/memcpy/free
		 */
		buf = hal;
	} else {
		buf = kuc_alloc(len, KUC_TRANSPORT_HSM, HMT_ACTION_LIST);
		if (IS_ERR(buf))
			RETURN(PTR_ERR(buf));
		memcpy(buf, hal, len);
	}

	/* Check if request is still valid (cf file hsm flags) */
	fail_request = false;
	hai = hai_first(hal);
	for (i = 0; i < hal->hal_count; i++, hai = hai_next(hai)) {
		if (hai->hai_action != HSMA_CANCEL) {
			struct mdt_object *obj;
			struct md_hsm hsm;

			obj = mdt_hsm_get_md_hsm(mti, &hai->hai_fid, &hsm);
			if (!IS_ERR(obj) && obj != NULL) {
				mdt_object_put(mti->mti_env, obj);
			} else {
				if (hai->hai_action == HSMA_REMOVE)
					continue;

				if (obj == NULL) {
					fail_request = true;
					rc = mdt_agent_record_update(
							     mti->mti_env, mdt,
							     &hai->hai_cookie,
							     1, ARS_FAILED);
					if (rc) {
						CERROR(
					      "%s: mdt_agent_record_update() "
					      "failed, cannot update "
					      "status to %s for cookie "
					      LPX64": rc = %d\n",
					      mdt_obd_name(mdt),
					      agent_req_status2name(ARS_FAILED),
					      hai->hai_cookie, rc);
						GOTO(out_buf, rc);
					}
					continue;
				}
				GOTO(out_buf, rc = PTR_ERR(obj));
			}

			if (!mdt_hsm_is_action_compat(hai, hal->hal_archive_id,
						      hal->hal_flags, &hsm)) {
				/* incompatible request, we abort the request */
				/* next time coordinator will wake up, it will
				 * make the same compound with valid only
				 * records */
				fail_request = true;
				rc = mdt_agent_record_update(mti->mti_env, mdt,
							     &hai->hai_cookie,
							     1, ARS_FAILED);
				if (rc) {
					CERROR("%s: mdt_agent_record_update() "
					      "failed, cannot update "
					      "status to %s for cookie "
					      LPX64": rc = %d\n",
					      mdt_obd_name(mdt),
					      agent_req_status2name(ARS_FAILED),
					      hai->hai_cookie, rc);
					GOTO(out_buf, rc);
				}
			}
		}
	}

	/* we found incompatible requests, so the compound cannot be send
	 * as is. Bad records have been invalidated in llog.
	 * Valid one will be reschedule next time coordinator will wake up
	 * So no need the rebuild a full valid compound request now
	 */
	if (fail_request)
		GOTO(out_buf, rc = 0);

	/* Cancel memory registration is useless for purge
	 * non registration avoid a deadlock :
	 * in case of failure we have to take the write lock
	 * to remove entry which conflict with the read loack needed
	 * by purge
	 */
	if (!purge) {
		/* set is_registered even if failure because we may have
		 * partial work done */
		is_registered = true;
		rc = mdt_hsm_add_hal(mti, hal, &uuid);
		if (rc)
			GOTO(out_buf, rc);
	}

	/* Uses the ldlm reverse import; this rpc will be seen by
	 *  the ldlm_callback_handler. Note this sends a request RPC
	 * from a server (MDT) to a client (MDC), backwards of normal comms.
	 */
	exp = cfs_hash_lookup(mdt2obd_dev(mdt)->obd_uuid_hash, &uuid);
	if (exp == NULL || exp->exp_disconnected) {
		/* This should clean up agents on evicted exports */
		rc = -ENOENT;
		CERROR("%s: agent uuid (%s) not found, unregistering:"
		       " rc = %d\n",
		       mdt_obd_name(mdt), obd_uuid2str(&uuid), rc);
		mdt_hsm_agent_unregister(mti, &uuid);
		GOTO(out, rc);
	}

	/* send request to agent */
	rc = do_set_info_async(exp->exp_imp_reverse, LDLM_SET_INFO,
			       LUSTRE_OBD_VERSION,
			       sizeof(KEY_HSM_COPYTOOL_SEND),
			       KEY_HSM_COPYTOOL_SEND,
			       kuc_len(len), kuc_ptr(buf), NULL);

	if (rc)
		CERROR("%s: cannot send request to agent '%s': rc = %d\n",
		       mdt_obd_name(mdt), obd_uuid2str(&uuid), rc);

	class_export_put(exp);

	if (rc == -EPIPE) {
		CDEBUG(D_HSM, "Lost connection to agent '%s', unregistering\n",
		       obd_uuid2str(&uuid));
		mdt_hsm_agent_unregister(mti, &uuid);
	}

out:
	if (rc != 0 && is_registered) {
		/* in case of error, we have to unregister requests */
		hai = hai_first(hal);
		for (i = 0; i < hal->hal_count; i++, hai = hai_next(hai)) {
			if (hai->hai_action == HSMA_CANCEL)
				continue;
			mdt_cdt_remove_request(cdt, hai->hai_cookie);
		}
	}

out_buf:
	if (buf != hal)
		kuc_free(buf, len);

	RETURN(rc);
}

/**
 * update status of a request
 * \param mti [IN]
 * \param pgs [IN] progress of the copy tool
 * \retval 0 success
 * \retval -ve failure
 */
int mdt_hsm_coordinator_update(struct mdt_thread_info *mti,
			       struct hsm_progress_kernel *pgs)
{
	int      rc;

	ENTRY;
	/* ask to coodinator to update request state and
	 * to record on disk the result */
	rc = mdt_hsm_update_request_state(mti, pgs, 1);
	RETURN(rc);
}

/**
 * seq_file method called to start access to /proc file
 */
static void *mdt_hsm_agent_proc_start(struct seq_file *s, loff_t *off)
{
	struct mdt_device	*mdt = s->private;
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	struct list_head	*pos;
	loff_t			 i;
	ENTRY;

	down_read(&cdt->cdt_agent_lock);

	if (list_empty(&cdt->cdt_agents))
		RETURN(NULL);

	if (*off == 0)
		RETURN(SEQ_START_TOKEN);

	i = 0;
	list_for_each(pos, &cdt->cdt_agents) {
		i++;
		if (i >= *off)
			RETURN(pos);
	}

	RETURN(NULL);
}

/**
 * seq_file method called to get next item
 * just returns NULL at eof
 */
static void *mdt_hsm_agent_proc_next(struct seq_file *s, void *v, loff_t *p)
{
	struct mdt_device	*mdt = s->private;
	struct coordinator	*cdt = &mdt->mdt_coordinator;
	struct list_head	*pos = v;
	ENTRY;

	if (pos == SEQ_START_TOKEN)
		pos = cdt->cdt_agents.next;
	else
		pos = pos->next;

	(*p)++;
	if (pos != &cdt->cdt_agents)
		RETURN(pos);

	RETURN(NULL);
}

/**
 */
static int mdt_hsm_agent_proc_show(struct seq_file *s, void *v)
{
	struct list_head	*pos = v;
	struct hsm_agent	*ha;
	int			 i;
	ENTRY;

	if (pos == SEQ_START_TOKEN)
		RETURN(0);

	ha = list_entry(pos, struct hsm_agent, ha_list);
	seq_printf(s, "uuid=%s archive_id=", ha->ha_uuid.uuid);
	if (ha->ha_archive_cnt == 0) {
		seq_printf(s, "ANY");
	} else {
		seq_printf(s, "%d", ha->ha_archive_id[0]);
		for (i = 1; i < ha->ha_archive_cnt; i++)
			seq_printf(s, ",%d", ha->ha_archive_id[i]);
	}

	seq_printf(s, " requests=[current:%d ok:%d errors:%d]\n",
		   atomic_read(&ha->ha_requests),
		   atomic_read(&ha->ha_success),
		   atomic_read(&ha->ha_failure));
	RETURN(0);
}

/**
 * seq_file method called to stop access to /proc file
 */
static void mdt_hsm_agent_proc_stop(struct seq_file *s, void *v)
{
	struct mdt_device	*mdt = s->private;
	struct coordinator	*cdt = &mdt->mdt_coordinator;

	up_read(&cdt->cdt_agent_lock);
}

/* hsm agent list proc functions */
static const struct seq_operations mdt_hsm_agent_proc_ops = {
	.start	= mdt_hsm_agent_proc_start,
	.next	= mdt_hsm_agent_proc_next,
	.show	= mdt_hsm_agent_proc_show,
	.stop	= mdt_hsm_agent_proc_stop,
};

/**
 * public function called at open of /proc file to get
 * list of agents
 */
static int lprocfs_open_hsm_agent(struct inode *inode, struct file *file)
{
	struct seq_file	*s;
	int		 rc;
	ENTRY;

	if (LPROCFS_ENTRY_CHECK(inode))
		RETURN(-ENOENT);

	rc = seq_open(file, &mdt_hsm_agent_proc_ops);
	if (rc)
		RETURN(rc);

	s = file->private_data;
	s->private = PDE_DATA(inode);

	RETURN(rc);
}

/* methods to access hsm agent list */
const struct file_operations mdt_hsm_agent_fops = {
	.owner		= THIS_MODULE,
	.open		= lprocfs_open_hsm_agent,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= lprocfs_seq_release,
};

