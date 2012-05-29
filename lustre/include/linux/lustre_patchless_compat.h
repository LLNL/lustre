/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
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
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef LUSTRE_PATCHLESS_COMPAT_H
#define LUSTRE_PATCHLESS_COMPAT_H

#include <linux/lustre_version.h>
#include <linux/fs.h>

#ifndef HAVE_TRUNCATE_COMPLETE_PAGE
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/hash.h>

#ifndef HAVE_CANCEL_DIRTY_PAGE /* 2.6.20 */
#define cancel_dirty_page(page, size) clear_page_dirty(page)
#endif

#ifndef HAVE_DELETE_FROM_PAGE_CACHE /* 2.6.39 */
#ifndef HAVE_REMOVE_FROM_PAGE_CACHE /* 2.6.35 - 2.6.38 */
#ifdef HAVE_NR_PAGECACHE /* 2.6.18 */
#define __dec_zone_page_state(page, flag) atomic_add(-1, &nr_pagecache);
#endif /* HAVE_NR_PAGECACHE */

/* XXX copy & paste from 2.6.15 kernel */
static inline void ll_remove_from_page_cache(struct page *page)
{
        struct address_space *mapping = page->mapping;

        BUG_ON(!PageLocked(page));

#ifdef HAVE_RW_TREE_LOCK
        write_lock_irq(&mapping->tree_lock);
#else
	cfs_spin_lock_irq(&mapping->tree_lock);
#endif
        radix_tree_delete(&mapping->page_tree, page->index);
        page->mapping = NULL;
        mapping->nrpages--;
        __dec_zone_page_state(page, NR_FILE_PAGES);

#ifdef HAVE_RW_TREE_LOCK
        write_unlock_irq(&mapping->tree_lock);
#else
	cfs_spin_unlock_irq(&mapping->tree_lock);
#endif
}
#else /* HAVE_REMOVE_FROM_PAGE_CACHE */
#define ll_remove_from_page_cache(page) remove_from_page_cache(page)
#endif /* !HAVE_REMOVE_FROM_PAGE_CACHE */

static inline void ll_delete_from_page_cache(struct page *page)
{
        ll_remove_from_page_cache(page);
        page_cache_release(page);
}
#else /* HAVE_DELETE_FROM_PAGE_CACHE */
#define ll_delete_from_page_cache(page) delete_from_page_cache(page)
#endif /* !HAVE_DELETE_FROM_PAGE_CACHE */

static inline void
truncate_complete_page(struct address_space *mapping, struct page *page)
{
        if (page->mapping != mapping)
                return;

        if (PagePrivate(page))
                page->mapping->a_ops->invalidatepage(page, 0);

        cancel_dirty_page(page, PAGE_SIZE);
        ClearPageMappedToDisk(page);
        ll_delete_from_page_cache(page);
}
#endif /* !HAVE_TRUNCATE_COMPLETE_PAGE */

#if !defined(HAVE_D_REHASH_COND) && !defined(HAVE___D_REHASH)
/* megahack */
static inline void d_rehash_cond(struct dentry * entry, int lock)
{
	if (!lock)
		spin_unlock(&dcache_lock);

	d_rehash(entry);

	if (!lock)
		spin_lock(&dcache_lock);
}

#define __d_rehash(dentry, lock) d_rehash_cond(dentry, lock)
#endif /* !HAVE_D_REHASH_COND && !HAVE___D_REHASH*/

#ifdef ATTR_OPEN
# define ATTR_FROM_OPEN ATTR_OPEN
#else
# ifndef ATTR_FROM_OPEN
#  define ATTR_FROM_OPEN 0
# endif
#endif /* ATTR_OPEN */

#ifndef ATTR_RAW
#define ATTR_RAW 0
#endif

#ifndef ATTR_CTIME_SET
/*
 * set ATTR_CTIME_SET to a high value to avoid any risk of collision with other
 * ATTR_* attributes (see bug 13828)
 */
#define ATTR_CTIME_SET (1 << 28)
#endif

#endif /* LUSTRE_PATCHLESS_COMPAT_H */
