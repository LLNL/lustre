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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Implementation of cl_page for VVP layer.
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 *   Author: Jinshan Xiong <jinshan.xiong@whamcloud.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/page-flags.h>
#include <linux/pagemap.h>

#include <libcfs/libcfs.h>
#include "llite_internal.h"
#include "vvp_internal.h"

/*****************************************************************************
 *
 * Page operations.
 *
 */
static void vvp_page_fini(const struct lu_env *env,
			  struct cl_page_slice *slice,
			  struct pagevec *pvec)
{
	struct vvp_page *vpg     = cl2vvp_page(slice);
	struct page     *vmpage  = vpg->vpg_page;

	/*
	 * vmpage->private was already cleared when page was moved into
	 * VPG_FREEING state.
	 */
	LASSERT((struct cl_page *)vmpage->private != slice->cpl_page);
	LASSERT(vmpage != NULL);
	if (pvec) {
		if (!pagevec_add(pvec, vmpage))
			pagevec_release(pvec);
	} else {
		put_page(vmpage);
	}
}

static int vvp_page_own(const struct lu_env *env,
			const struct cl_page_slice *slice, struct cl_io *io,
			int nonblock)
{
	struct vvp_page *vpg    = cl2vvp_page(slice);
	struct page     *vmpage = vpg->vpg_page;

	ENTRY;

	LASSERT(vmpage != NULL);
	if (nonblock) {
		if (!trylock_page(vmpage))
			return -EAGAIN;

		if (unlikely(PageWriteback(vmpage))) {
			unlock_page(vmpage);
			return -EAGAIN;
		}

		return 0;
	}

	lock_page(vmpage);
	wait_on_page_writeback(vmpage);

	RETURN(0);
}

static void vvp_page_assume(const struct lu_env *env,
			    const struct cl_page_slice *slice,
			    struct cl_io *unused)
{
	struct page *vmpage = cl2vm_page(slice);

	LASSERT(vmpage != NULL);
	LASSERT(PageLocked(vmpage));
	wait_on_page_writeback(vmpage);
}

static void vvp_page_unassume(const struct lu_env *env,
			      const struct cl_page_slice *slice,
			      struct cl_io *unused)
{
	struct page *vmpage = cl2vm_page(slice);

	LASSERT(vmpage != NULL);
	LASSERT(PageLocked(vmpage));
}

static void vvp_page_disown(const struct lu_env *env,
			    const struct cl_page_slice *slice, struct cl_io *io)
{
	struct page *vmpage = cl2vm_page(slice);

	ENTRY;

	LASSERT(vmpage != NULL);
	LASSERT(PageLocked(vmpage));

	unlock_page(cl2vm_page(slice));

	EXIT;
}

static void vvp_page_discard(const struct lu_env *env,
			     const struct cl_page_slice *slice,
			     struct cl_io *unused)
{
	struct page     *vmpage = cl2vm_page(slice);
	struct vvp_page *vpg    = cl2vvp_page(slice);

	LASSERT(vmpage != NULL);
	LASSERT(PageLocked(vmpage));

	if (vpg->vpg_defer_uptodate && !vpg->vpg_ra_used && vmpage->mapping)
		ll_ra_stats_inc(vmpage->mapping->host, RA_STAT_DISCARDED);

	generic_error_remove_page(vmpage->mapping, vmpage);
}

static void vvp_page_export(const struct lu_env *env,
			    const struct cl_page_slice *slice,
			    int uptodate)
{
	struct page *vmpage = cl2vm_page(slice);

	LASSERT(vmpage != NULL);
	LASSERT(PageLocked(vmpage));
	if (uptodate)
		SetPageUptodate(vmpage);
	else
		ClearPageUptodate(vmpage);
}

static int vvp_page_is_vmlocked(const struct lu_env *env,
				const struct cl_page_slice *slice)
{
	return PageLocked(cl2vm_page(slice)) ? -EBUSY : -ENODATA;
}

static int vvp_page_prep_read(const struct lu_env *env,
			      const struct cl_page_slice *slice,
			      struct cl_io *unused)
{
	ENTRY;
	/* Skip the page already marked as PG_uptodate. */
	RETURN(PageUptodate(cl2vm_page(slice)) ? -EALREADY : 0);
}

static int vvp_page_prep_write(const struct lu_env *env,
			       const struct cl_page_slice *slice,
			       struct cl_io *unused)
{
	struct page *vmpage = cl2vm_page(slice);
	struct cl_page *pg = slice->cpl_page;

	LASSERT(PageLocked(vmpage));
	LASSERT(!PageDirty(vmpage));

	/* ll_writepage path is not a sync write, so need to set page writeback
	 * flag
	 */
	if (pg->cp_sync_io == NULL)
		set_page_writeback(vmpage);

	return 0;
}

static void vvp_page_delete(const struct lu_env *env,
			    const struct cl_page_slice *slice)
{
	struct cl_page *cp = slice->cpl_page;

	if (cp->cp_type == CPT_CACHEABLE) {
		struct page *vmpage = cp->cp_vmpage;
		struct inode *inode = vmpage->mapping->host;

		LASSERT(PageLocked(vmpage));
		LASSERT((struct cl_page *)vmpage->private == cp);

		/* Drop the reference count held in vvp_page_init */
		atomic_dec(&cp->cp_ref);

		ClearPagePrivate(vmpage);
		vmpage->private = 0;

		/* clearpageuptodate prevents the page being read by the
		 * kernel after it has been deleted from Lustre, which avoids
		 * potential stale data reads.  The seqlock allows us to see
		 * that a page was potentially deleted and catch the resulting
		 * SIGBUS - see ll_filemap_fault() (LU-16160) */
		write_seqlock(&ll_i2info(inode)->lli_page_inv_lock);
		ClearPageUptodate(vmpage);
		write_sequnlock(&ll_i2info(inode)->lli_page_inv_lock);

		/*
		 * The reference from vmpage to cl_page is removed,
		 * but the reference back is still here. It is removed
		 * later in cl_page_free().
		 */
	}
}

/**
 * Handles page transfer errors at VM level.
 *
 * This takes inode as a separate argument, because inode on which error is to
 * be set can be different from \a vmpage inode in case of direct-io.
 */
static void vvp_vmpage_error(struct inode *inode, struct page *vmpage,
			     int ioret)
{
	struct vvp_object *obj = cl_inode2vvp(inode);

	if (ioret == 0) {
		ClearPageError(vmpage);
		obj->vob_discard_page_warned = 0;
	} else {
		SetPageError(vmpage);
		if (ioret == -ENOSPC)
			set_bit(AS_ENOSPC, &inode->i_mapping->flags);
		else
			set_bit(AS_EIO, &inode->i_mapping->flags);

		if ((ioret == -ESHUTDOWN || ioret == -EINTR ||
		     ioret == -EIO) && obj->vob_discard_page_warned == 0) {
			obj->vob_discard_page_warned = 1;
			ll_dirty_page_discard_warn(inode, ioret);
		}
	}
}

static void vvp_page_completion_read(const struct lu_env *env,
				     const struct cl_page_slice *slice,
				     int ioret)
{
	struct vvp_page *vpg    = cl2vvp_page(slice);
	struct page     *vmpage = vpg->vpg_page;
	struct cl_page  *page   = slice->cpl_page;
	struct inode    *inode  = vvp_object_inode(page->cp_obj);

	ENTRY;
	LASSERT(PageLocked(vmpage));
	CL_PAGE_HEADER(D_PAGE, env, page, "completing READ with %d\n", ioret);

	if (vpg->vpg_defer_uptodate)
		ll_ra_count_put(ll_i2sbi(inode), 1);

	if (ioret == 0)  {
		if (!vpg->vpg_defer_uptodate)
			cl_page_export(env, page, 1);
	} else if (vpg->vpg_defer_uptodate) {
		vpg->vpg_defer_uptodate = 0;
		if (ioret == -EAGAIN) {
			/* mirror read failed, it needs to destroy the page
			 * because subpage would be from wrong osc when trying
			 * to read from a new mirror
			 */
			generic_error_remove_page(vmpage->mapping, vmpage);
		}
	}

	if (page->cp_sync_io == NULL)
		unlock_page(vmpage);

	EXIT;
}

static void vvp_page_completion_write(const struct lu_env *env,
				      const struct cl_page_slice *slice,
				      int ioret)
{
	struct vvp_page *vpg    = cl2vvp_page(slice);
	struct cl_page  *pg     = slice->cpl_page;
	struct page     *vmpage = vpg->vpg_page;

	ENTRY;
	CL_PAGE_HEADER(D_PAGE, env, pg, "completing WRITE with %d\n", ioret);

	if (pg->cp_sync_io != NULL) {
		LASSERT(PageLocked(vmpage));
		LASSERT(!PageWriteback(vmpage));
	} else {
		LASSERT(PageWriteback(vmpage));
		/*
		 * Only mark the page error only when it's an async write
		 * because applications won't wait for IO to finish.
		 */
		vvp_vmpage_error(vvp_object_inode(pg->cp_obj), vmpage, ioret);

		end_page_writeback(vmpage);
	}
	EXIT;
}

/**
 * Implements cl_page_operations::cpo_make_ready() method.
 *
 * This is called to yank a page from the transfer cache and to send it out as
 * a part of transfer. This function try-locks the page. If try-lock failed,
 * page is owned by some concurrent IO, and should be skipped (this is bad,
 * but hopefully rare situation, as it usually results in transfer being
 * shorter than possible).
 *
 * \retval 0      success, page can be placed into transfer
 *
 * \retval -EAGAIN page is either used by concurrent IO has been
 * truncated. Skip it.
 */
static int vvp_page_make_ready(const struct lu_env *env,
			       const struct cl_page_slice *slice)
{
	struct page *vmpage = cl2vm_page(slice);
	struct cl_page *pg = slice->cpl_page;
	int result = 0;

	lock_page(vmpage);
	if (clear_page_dirty_for_io(vmpage)) {
		LASSERT(pg->cp_state == CPS_CACHED);
		/* This actually clears the dirty bit in the radix
		 * tree.
		 */
		set_page_writeback(vmpage);
		CL_PAGE_HEADER(D_PAGE, env, pg, "readied\n");
	} else if (pg->cp_state == CPS_PAGEOUT) {
		/* is it possible for osc_flush_async_page() to already
		 * make it ready?
		 */
		result = -EALREADY;
	} else {
		CL_PAGE_DEBUG(D_ERROR, env, pg, "Unexpecting page state %d.\n",
			      pg->cp_state);
		LBUG();
	}
	unlock_page(vmpage);
	RETURN(result);
}

static int vvp_page_print(const struct lu_env *env,
			  const struct cl_page_slice *slice,
			  void *cookie, lu_printer_t printer)
{
	struct vvp_page *vpg	= cl2vvp_page(slice);
	struct page     *vmpage	= vpg->vpg_page;

	(*printer)(env, cookie,
		   LUSTRE_VVP_NAME"-page@%p(%d:%d) vm@%p ",
		   vpg, vpg->vpg_defer_uptodate, vpg->vpg_ra_used, vmpage);

	if (vmpage != NULL) {
		(*printer)(env, cookie, "%lx %d:%d %lx %lu %slru",
			   (long)vmpage->flags, page_count(vmpage),
			   page_mapcount(vmpage), vmpage->private,
			   page_index(vmpage),
			   list_empty(&vmpage->lru) ? "not-" : "");
	}

	(*printer)(env, cookie, "\n");

	return 0;
}

static int vvp_page_fail(const struct lu_env *env,
			 const struct cl_page_slice *slice)
{
	/*
	 * Cached read?
	 */
	LBUG();

	return 0;
}

static const struct cl_page_operations vvp_page_ops = {
	.cpo_own           = vvp_page_own,
	.cpo_assume        = vvp_page_assume,
	.cpo_unassume      = vvp_page_unassume,
	.cpo_disown        = vvp_page_disown,
	.cpo_discard       = vvp_page_discard,
	.cpo_delete        = vvp_page_delete,
	.cpo_export        = vvp_page_export,
	.cpo_is_vmlocked   = vvp_page_is_vmlocked,
	.cpo_fini          = vvp_page_fini,
	.cpo_print         = vvp_page_print,
	.io = {
		[CRT_READ] = {
			.cpo_prep       = vvp_page_prep_read,
			.cpo_completion = vvp_page_completion_read,
			.cpo_make_ready = vvp_page_fail,
		},
		[CRT_WRITE] = {
			.cpo_prep       = vvp_page_prep_write,
			.cpo_completion = vvp_page_completion_write,
			.cpo_make_ready = vvp_page_make_ready,
		},
	},
};

static void vvp_transient_page_discard(const struct lu_env *env,
				       const struct cl_page_slice *slice,
				       struct cl_io *unused)
{
	struct cl_page *page = slice->cpl_page;

	/*
	 * For transient pages, remove it from the radix tree.
	 */
	cl_page_delete(env, page);
}

static int vvp_transient_page_is_vmlocked(const struct lu_env *env,
					  const struct cl_page_slice *slice)
{
	return -EBUSY;
}

static const struct cl_page_operations vvp_transient_page_ops = {
	.cpo_discard		= vvp_transient_page_discard,
	.cpo_is_vmlocked	= vvp_transient_page_is_vmlocked,
	.cpo_print		= vvp_page_print,
};

int vvp_page_init(const struct lu_env *env, struct cl_object *obj,
		struct cl_page *page, pgoff_t index)
{
	struct vvp_page *vpg = cl_object_page_slice(obj, page);
	struct page     *vmpage = page->cp_vmpage;

	CLOBINVRNT(env, obj, vvp_object_invariant(obj));

	vpg->vpg_page = vmpage;

	if (page->cp_type == CPT_TRANSIENT) {
		/* DIO pages are referenced by userspace, we don't need to take
		 * a reference on them. (contrast with get_page() call above)
		 */
		cl_page_slice_add(page, &vpg->vpg_cl, obj,
				  &vvp_transient_page_ops);
	} else {
		get_page(vmpage);
		/* in cache, decref in vvp_page_delete */
		atomic_inc(&page->cp_ref);
		SetPagePrivate(vmpage);
		vmpage->private = (unsigned long)page;
		cl_page_slice_add(page, &vpg->vpg_cl, obj,
				&vvp_page_ops);
	}

	return 0;
}
