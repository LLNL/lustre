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
 *
 * lustre/fid/fid_handler.c
 *
 * Lustre Sequence Manager
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FID

#include <libcfs/libcfs.h>
#include <linux/module.h>
#include <obd.h>
#include <obd_class.h>
#include <dt_object.h>
#include <md_object.h>
#include <obd_support.h>
#include <lustre_req_layout.h>
#include <lustre_fid.h>
#include "fid_internal.h"

static void seq_server_proc_fini(struct lu_server_seq *seq);

/* Assigns client to sequence controller node. */
int seq_server_set_cli(struct lu_server_seq *seq,
                       struct lu_client_seq *cli,
                       const struct lu_env *env)
{
        int rc = 0;
        ENTRY;

        /*
         * Ask client for new range, assign that range to ->seq_space and write
         * seq state to backing store should be atomic.
         */
	mutex_lock(&seq->lss_mutex);

        if (cli == NULL) {
                CDEBUG(D_INFO, "%s: Detached sequence client %s\n",
                       seq->lss_name, cli->lcs_name);
                seq->lss_cli = cli;
                GOTO(out_up, rc = 0);
        }

	if (seq->lss_cli != NULL) {
		CDEBUG(D_HA, "%s: Sequence controller is already "
		       "assigned\n", seq->lss_name);
		GOTO(out_up, rc = -EEXIST);
	}

        CDEBUG(D_INFO, "%s: Attached sequence controller %s\n",
               seq->lss_name, cli->lcs_name);

	seq->lss_cli = cli;
	cli->lcs_space.lsr_index = seq->lss_site->ss_node_id;
	EXIT;
out_up:
	mutex_unlock(&seq->lss_mutex);
        return rc;
}
EXPORT_SYMBOL(seq_server_set_cli);
/*
 * allocate \a w units of sequence from range \a from.
 */
static inline void range_alloc(struct lu_seq_range *to,
                               struct lu_seq_range *from,
                               __u64 width)
{
        width = min(range_space(from), width);
        to->lsr_start = from->lsr_start;
        to->lsr_end = from->lsr_start + width;
        from->lsr_start += width;
}

/**
 * On controller node, allocate new super sequence for regular sequence server.
 * As this super sequence controller, this node suppose to maintain fld
 * and update index.
 * \a out range always has currect mds node number of requester.
 */

static int __seq_server_alloc_super(struct lu_server_seq *seq,
                                    struct lu_seq_range *out,
                                    const struct lu_env *env)
{
	struct lu_seq_range *space = &seq->lss_space;
	int rc;
	ENTRY;

	LASSERT(range_is_sane(space));

	if (range_is_exhausted(space)) {
		CERROR("%s: Sequences space is exhausted\n",
		       seq->lss_name);
		RETURN(-ENOSPC);
	} else {
		range_alloc(out, space, seq->lss_width);
	}

	rc = seq_store_update(env, seq, out, 1 /* sync */);

	LCONSOLE_INFO("%s: super-sequence allocation rc = %d " DRANGE"\n",
		      seq->lss_name, rc, PRANGE(out));

	RETURN(rc);
}

int seq_server_alloc_super(struct lu_server_seq *seq,
                           struct lu_seq_range *out,
                           const struct lu_env *env)
{
        int rc;
        ENTRY;

	mutex_lock(&seq->lss_mutex);
        rc = __seq_server_alloc_super(seq, out, env);
	mutex_unlock(&seq->lss_mutex);

        RETURN(rc);
}

int seq_server_alloc_spec(struct lu_server_seq *seq,
			  struct lu_seq_range *spec,
			  const struct lu_env *env)
{
	struct lu_seq_range *space = &seq->lss_space;
	int rc = -ENOSPC;
	ENTRY;

	/*
	 * In some cases (like recovery after a disaster)
	 * we may need to allocate sequences manually
	 * Notice some sequences can be lost if requested
	 * range doesn't start at the beginning of current
	 * free space. Also notice it's not possible now
	 * to allocate sequences out of natural order.
	 */
	if (spec->lsr_start >= spec->lsr_end)
		RETURN(-EINVAL);
	if (spec->lsr_flags != LU_SEQ_RANGE_MDT &&
	    spec->lsr_flags != LU_SEQ_RANGE_OST)
		RETURN(-EINVAL);

	mutex_lock(&seq->lss_mutex);
	if (spec->lsr_start >= space->lsr_start) {
		space->lsr_start = spec->lsr_end;
		rc = seq_store_update(env, seq, spec, 1 /* sync */);

		LCONSOLE_INFO("%s: "DRANGE" sequences allocated: rc = %d \n",
			      seq->lss_name, PRANGE(spec), rc);
	}
	mutex_unlock(&seq->lss_mutex);

	RETURN(rc);
}

static int __seq_set_init(const struct lu_env *env,
                            struct lu_server_seq *seq)
{
        struct lu_seq_range *space = &seq->lss_space;
        int rc;

        range_alloc(&seq->lss_lowater_set, space, seq->lss_set_width);
        range_alloc(&seq->lss_hiwater_set, space, seq->lss_set_width);

        rc = seq_store_update(env, seq, NULL, 1);

        return rc;
}

/*
 * This function implements new seq allocation algorithm using async
 * updates to seq file on disk. ref bug 18857 for details.
 * there are four variable to keep track of this process
 *
 * lss_space; - available lss_space
 * lss_lowater_set; - lu_seq_range for all seqs before barrier, i.e. safe to use
 * lss_hiwater_set; - lu_seq_range after barrier, i.e. allocated but may be
 *                    not yet committed
 *
 * when lss_lowater_set reaches the end it is replaced with hiwater one and
 * a write operation is initiated to allocate new hiwater range.
 * if last seq write opearion is still not commited, current operation is
 * flaged as sync write op.
 */
static int range_alloc_set(const struct lu_env *env,
                            struct lu_seq_range *out,
                            struct lu_server_seq *seq)
{
        struct lu_seq_range *space = &seq->lss_space;
        struct lu_seq_range *loset = &seq->lss_lowater_set;
        struct lu_seq_range *hiset = &seq->lss_hiwater_set;
        int rc = 0;

        if (range_is_zero(loset))
                __seq_set_init(env, seq);

        if (OBD_FAIL_CHECK(OBD_FAIL_SEQ_ALLOC)) /* exhaust set */
                loset->lsr_start = loset->lsr_end;

        if (range_is_exhausted(loset)) {
                /* reached high water mark. */
		struct lu_device *dev = seq->lss_site->ss_lu->ls_top_dev;
                int obd_num_clients = dev->ld_obd->obd_num_exports;
                __u64 set_sz;

                /* calculate new seq width based on number of clients */
                set_sz = max(seq->lss_set_width,
                             obd_num_clients * seq->lss_width);
                set_sz = min(range_space(space), set_sz);

                /* Switch to hiwater range now */
                *loset = *hiset;
                /* allocate new hiwater range */
                range_alloc(hiset, space, set_sz);

                /* update ondisk seq with new *space */
                rc = seq_store_update(env, seq, NULL, seq->lss_need_sync);
        }

        LASSERTF(!range_is_exhausted(loset) || range_is_sane(loset),
                 DRANGE"\n", PRANGE(loset));

        if (rc == 0)
                range_alloc(out, loset, seq->lss_width);

        RETURN(rc);
}

static int __seq_server_alloc_meta(struct lu_server_seq *seq,
                                   struct lu_seq_range *out,
                                   const struct lu_env *env)
{
	struct lu_seq_range *space = &seq->lss_space;
	int rc = 0;

	ENTRY;

	LASSERT(range_is_sane(space));

	/* Check if available space ends and allocate new super seq */
	if (range_is_exhausted(space)) {
		if (!seq->lss_cli) {
			CERROR("%s: No sequence controller is attached.\n",
			       seq->lss_name);
			RETURN(-ENODEV);
		}

		rc = seq_client_alloc_super(seq->lss_cli, env);
		if (rc) {
			CERROR("%s: Can't allocate super-sequence, rc %d\n",
			       seq->lss_name, rc);
			RETURN(rc);
		}

		/* Saving new range to allocation space. */
		*space = seq->lss_cli->lcs_space;
		LASSERT(range_is_sane(space));
	}

	rc = range_alloc_set(env, out, seq);
	if (rc != 0) {
		CERROR("%s: Allocated meta-sequence failed: rc = %d\n",
			seq->lss_name, rc);
		RETURN(rc);
	}

	CDEBUG(D_INFO, "%s: Allocated meta-sequence " DRANGE"\n",
		seq->lss_name, PRANGE(out));

	RETURN(rc);
}

int seq_server_alloc_meta(struct lu_server_seq *seq,
                          struct lu_seq_range *out,
                          const struct lu_env *env)
{
        int rc;
        ENTRY;

	mutex_lock(&seq->lss_mutex);
        rc = __seq_server_alloc_meta(seq, out, env);
	mutex_unlock(&seq->lss_mutex);

        RETURN(rc);
}
EXPORT_SYMBOL(seq_server_alloc_meta);

static int seq_server_handle(struct lu_site *site,
                             const struct lu_env *env,
                             __u32 opc, struct lu_seq_range *out)
{
	int rc;
	struct seq_server_site *ss_site;
	ENTRY;

	ss_site = lu_site2seq(site);

	switch (opc) {
	case SEQ_ALLOC_META:
		if (!ss_site->ss_server_seq) {
			CERROR("Sequence server is not "
			       "initialized\n");
			RETURN(-EINVAL);
		}
		rc = seq_server_alloc_meta(ss_site->ss_server_seq, out, env);
		break;
	case SEQ_ALLOC_SUPER:
		if (!ss_site->ss_control_seq) {
			CERROR("Sequence controller is not "
			       "initialized\n");
			RETURN(-EINVAL);
		}
		rc = seq_server_alloc_super(ss_site->ss_control_seq, out, env);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	RETURN(rc);
}

static int seq_handler(struct tgt_session_info *tsi)
{
	struct lu_seq_range	*out, *tmp;
	struct lu_site		*site;
	int			 rc;
	__u32			*opc;

	ENTRY;

	LASSERT(!(lustre_msg_get_flags(tgt_ses_req(tsi)->rq_reqmsg) & MSG_REPLAY));
	site = tsi->tsi_exp->exp_obd->obd_lu_dev->ld_site;
	LASSERT(site != NULL);

	opc = req_capsule_client_get(tsi->tsi_pill, &RMF_SEQ_OPC);
	if (opc != NULL) {
		out = req_capsule_server_get(tsi->tsi_pill, &RMF_SEQ_RANGE);
		if (out == NULL)
			RETURN(err_serious(-EPROTO));

		tmp = req_capsule_client_get(tsi->tsi_pill, &RMF_SEQ_RANGE);

		/* seq client passed mdt id, we need to pass that using out
		 * range parameter */

		out->lsr_index = tmp->lsr_index;
		out->lsr_flags = tmp->lsr_flags;
		rc = seq_server_handle(site, tsi->tsi_env, *opc, out);
	} else {
		rc = err_serious(-EPROTO);
	}

	RETURN(rc);
}

struct tgt_handler seq_handlers[] = {
TGT_SEQ_HDL(HABEO_REFERO,	SEQ_QUERY,	seq_handler),
};
EXPORT_SYMBOL(seq_handlers);

/* context key constructor/destructor: seq_key_init, seq_key_fini */
LU_KEY_INIT_FINI(seq, struct seq_thread_info);

/* context key: seq_thread_key */
LU_CONTEXT_KEY_DEFINE(seq, LCT_MD_THREAD | LCT_DT_THREAD);

extern const struct file_operations seq_fld_proc_seq_fops;

static int seq_server_proc_init(struct lu_server_seq *seq)
{
#ifdef LPROCFS
	int rc;
	ENTRY;

	seq->lss_proc_dir = lprocfs_seq_register(seq->lss_name,
						 seq_type_proc_dir,
						 NULL, NULL);
	if (IS_ERR(seq->lss_proc_dir)) {
		rc = PTR_ERR(seq->lss_proc_dir);
		RETURN(rc);
	}

	rc = lprocfs_seq_add_vars(seq->lss_proc_dir,
				  seq_server_proc_list, seq);
	if (rc) {
		CERROR("%s: Can't init sequence manager "
		       "proc, rc %d\n", seq->lss_name, rc);
		GOTO(out_cleanup, rc);
	}

	if (seq->lss_type == LUSTRE_SEQ_CONTROLLER) {
		rc = lprocfs_seq_create(seq->lss_proc_dir, "fldb", 0644,
					&seq_fld_proc_seq_fops, seq);
		if (rc) {
			CERROR("%s: Can't create fldb for sequence manager "
			       "proc: rc = %d\n", seq->lss_name, rc);
			GOTO(out_cleanup, rc);
		}
	}

	RETURN(0);

out_cleanup:
	seq_server_proc_fini(seq);
	return rc;
#else /* LPROCFS */
	return 0;
#endif
}

static void seq_server_proc_fini(struct lu_server_seq *seq)
{
#ifdef LPROCFS
        ENTRY;
        if (seq->lss_proc_dir != NULL) {
                if (!IS_ERR(seq->lss_proc_dir))
                        lprocfs_remove(&seq->lss_proc_dir);
                seq->lss_proc_dir = NULL;
        }
        EXIT;
#endif /* LPROCFS */
}

int seq_server_init(struct lu_server_seq *seq,
		    struct dt_device *dev,
		    const char *prefix,
		    enum lu_mgr_type type,
		    struct seq_server_site *ss,
		    const struct lu_env *env)
{
	int rc, is_srv = (type == LUSTRE_SEQ_SERVER);
	ENTRY;

	LASSERT(dev != NULL);
	LASSERT(prefix != NULL);
	LASSERT(ss != NULL);
	LASSERT(ss->ss_lu != NULL);

	/* A compile-time check for FIDs that used to be in lustre_idl.h
	 * but is moved here to remove CLASSERT/LASSERT in that header.
	 * Check all lu_fid fields are converted in fid_cpu_to_le() and friends
	 * and that there is no padding added by compiler to the struct. */
	{
		struct lu_fid tst;

		CLASSERT(sizeof(tst) == sizeof(tst.f_seq) +
			 sizeof(tst.f_oid) + sizeof(tst.f_ver));
	}

	seq->lss_cli = NULL;
	seq->lss_type = type;
	seq->lss_site = ss;
	range_init(&seq->lss_space);

        range_init(&seq->lss_lowater_set);
        range_init(&seq->lss_hiwater_set);
        seq->lss_set_width = LUSTRE_SEQ_BATCH_WIDTH;

	mutex_init(&seq->lss_mutex);

        seq->lss_width = is_srv ?
                LUSTRE_SEQ_META_WIDTH : LUSTRE_SEQ_SUPER_WIDTH;

        snprintf(seq->lss_name, sizeof(seq->lss_name),
                 "%s-%s", (is_srv ? "srv" : "ctl"), prefix);

        rc = seq_store_init(seq, env, dev);
        if (rc)
                GOTO(out, rc);
        /* Request backing store for saved sequence info. */
        rc = seq_store_read(seq, env);
        if (rc == -ENODATA) {

                /* Nothing is read, init by default value. */
                seq->lss_space = is_srv ?
                        LUSTRE_SEQ_ZERO_RANGE:
                        LUSTRE_SEQ_SPACE_RANGE;

		seq->lss_space.lsr_index = ss->ss_node_id;
		LCONSOLE_INFO("%s: No data found "
			      "on store. Initialize space\n",
			      seq->lss_name);

                rc = seq_store_update(env, seq, NULL, 0);
                if (rc) {
                        CERROR("%s: Can't write space data, "
                               "rc %d\n", seq->lss_name, rc);
                }
        } else if (rc) {
                CERROR("%s: Can't read space data, rc %d\n",
                       seq->lss_name, rc);
                GOTO(out, rc);
        }

        if (is_srv) {
                LASSERT(range_is_sane(&seq->lss_space));
        } else {
                LASSERT(!range_is_zero(&seq->lss_space) &&
                        range_is_sane(&seq->lss_space));
        }

        rc  = seq_server_proc_init(seq);
        if (rc)
                GOTO(out, rc);

        EXIT;
out:
        if (rc)
                seq_server_fini(seq, env);
        return rc;
}
EXPORT_SYMBOL(seq_server_init);

void seq_server_fini(struct lu_server_seq *seq,
                     const struct lu_env *env)
{
        ENTRY;

        seq_server_proc_fini(seq);
        seq_store_fini(seq, env);

        EXIT;
}
EXPORT_SYMBOL(seq_server_fini);

int seq_site_fini(const struct lu_env *env, struct seq_server_site *ss)
{
	if (ss == NULL)
		RETURN(0);

	if (ss->ss_server_seq) {
		seq_server_fini(ss->ss_server_seq, env);
		OBD_FREE_PTR(ss->ss_server_seq);
		ss->ss_server_seq = NULL;
	}

	if (ss->ss_control_seq) {
		seq_server_fini(ss->ss_control_seq, env);
		OBD_FREE_PTR(ss->ss_control_seq);
		ss->ss_control_seq = NULL;
	}

	if (ss->ss_client_seq) {
		seq_client_fini(ss->ss_client_seq);
		OBD_FREE_PTR(ss->ss_client_seq);
		ss->ss_client_seq = NULL;
	}

	RETURN(0);
}
EXPORT_SYMBOL(seq_site_fini);

int fid_server_mod_init(void)
{
	LU_CONTEXT_KEY_INIT(&seq_thread_key);
	return lu_context_key_register(&seq_thread_key);
}

void fid_server_mod_exit(void)
{
	lu_context_key_degister(&seq_thread_key);
}
