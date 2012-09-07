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
 */
/*
 * Copyright (c) 2012 Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/include/lustre_mdc.h
 *
 * MDS data structures.
 * See also lustre_idl.h for wire formats of requests.
 */

#ifndef _LUSTRE_MDC_H
#define _LUSTRE_MDC_H

/** \defgroup mdc mdc
 *
 * @{
 */

#ifdef __KERNEL__
# include <linux/fs.h>
# include <linux/dcache.h>
# ifdef CONFIG_FS_POSIX_ACL
#  include <linux/posix_acl_xattr.h>
# endif /* CONFIG_FS_POSIX_ACL */
# include <linux/lustre_intent.h>
#endif /* __KERNEL__ */
#include <lustre_handles.h>
#include <libcfs/libcfs.h>
#include <obd_class.h>
#include <lustre/lustre_idl.h>
#include <lustre_lib.h>
#include <lustre_dlm.h>
#include <lustre_export.h>

struct ptlrpc_client;
struct obd_export;
struct ptlrpc_request;
struct obd_device;

struct mdc_rpc_lock {
	cfs_mutex_t		rpcl_mutex;
	struct lookup_intent	*rpcl_it;
	int			rpcl_fakes;
};

#define MDC_FAKE_RPCL_IT ((void *)0x2c0012bfUL)

static inline void mdc_init_rpc_lock(struct mdc_rpc_lock *lck)
{
        cfs_mutex_init(&lck->rpcl_mutex);
        lck->rpcl_it = NULL;
}

static inline void mdc_get_rpc_lock(struct mdc_rpc_lock *lck,
				    struct lookup_intent *it)
{
	ENTRY;

	if (it != NULL && (it->it_op == IT_GETATTR || it->it_op == IT_LOOKUP))
		return;

	/* This would normally block until the existing request finishes.
	 * If fail_loc is set it will block until the regular request is
	 * done, then set rpcl_it to MDC_FAKE_RPCL_IT.  Once that is set
	 * it will only be cleared when all fake requests are finished.
	 * Only when all fake requests are finished can normal requests
	 * be sent, to ensure they are recoverable again. */
 again:
	cfs_mutex_lock(&lck->rpcl_mutex);

	if (CFS_FAIL_CHECK_QUIET(OBD_FAIL_MDC_RPCS_SEM)) {
		lck->rpcl_it = MDC_FAKE_RPCL_IT;
		lck->rpcl_fakes++;
		cfs_mutex_unlock(&lck->rpcl_mutex);
		return;
	}

	/* This will only happen when the CFS_FAIL_CHECK() was
	 * just turned off but there are still requests in progress.
	 * Wait until they finish.  It doesn't need to be efficient
	 * in this extremely rare case, just have low overhead in
	 * the common case when it isn't true. */
	while (unlikely(lck->rpcl_it == MDC_FAKE_RPCL_IT)) {
		cfs_mutex_unlock(&lck->rpcl_mutex);
		cfs_schedule_timeout(cfs_time_seconds(1) / 4);
		goto again;
	}

	LASSERT(lck->rpcl_it == NULL);
	lck->rpcl_it = it;
}

static inline void mdc_put_rpc_lock(struct mdc_rpc_lock *lck,
				    struct lookup_intent *it)
{
	if (it != NULL && (it->it_op == IT_GETATTR || it->it_op == IT_LOOKUP))
		goto out;

	if (lck->rpcl_it == MDC_FAKE_RPCL_IT) { /* OBD_FAIL_MDC_RPCS_SEM */
		cfs_mutex_lock(&lck->rpcl_mutex);

		LASSERTF(lck->rpcl_fakes > 0, "%d\n", lck->rpcl_fakes);
		lck->rpcl_fakes--;

		if (lck->rpcl_fakes == 0)
			lck->rpcl_it = NULL;

	} else {
		LASSERTF(it == lck->rpcl_it, "%p != %p\n", it, lck->rpcl_it);
		lck->rpcl_it = NULL;
	}

	cfs_mutex_unlock(&lck->rpcl_mutex);
 out:
	EXIT;
}

static inline void mdc_update_max_ea_from_body(struct obd_export *exp,
                                               struct mdt_body *body)
{
        if (body->valid & OBD_MD_FLMODEASIZE) {
                if (exp->exp_obd->u.cli.cl_max_mds_easize < body->max_mdsize)
                        exp->exp_obd->u.cli.cl_max_mds_easize =
                                                body->max_mdsize;
                if (exp->exp_obd->u.cli.cl_max_mds_cookiesize <
                                                body->max_cookiesize)
                        exp->exp_obd->u.cli.cl_max_mds_cookiesize =
                                                body->max_cookiesize;
        }
}


struct mdc_cache_waiter {
        cfs_list_t              mcw_entry;
        cfs_waitq_t             mcw_waitq;
};

/* mdc/mdc_locks.c */
int it_disposition(struct lookup_intent *it, int flag);
void it_clear_disposition(struct lookup_intent *it, int flag);
void it_set_disposition(struct lookup_intent *it, int flag);
int it_open_error(int phase, struct lookup_intent *it);
#ifdef HAVE_SPLIT_SUPPORT
int mdc_sendpage(struct obd_export *exp, const struct lu_fid *fid,
                 const struct page *page, int offset);
#endif

/** @} mdc */

#endif
