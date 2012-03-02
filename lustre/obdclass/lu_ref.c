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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdclass/lu_ref.c
 *
 * Lustre reference.
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
#else
# include <liblustre.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lu_ref.h>

#ifdef USE_LU_REF

/**
 * Asserts a condition for a given lu_ref. Must be called with
 * lu_ref::lf_guard held.
 */
#define REFASSERT(ref, expr)                            \
  do {                                                  \
          struct lu_ref *r = (ref);                 \
                                                        \
          if (unlikely(!(expr))) {                      \
                  lu_ref_print(r);                  \
                  cfs_spin_unlock(&r->lf_guard);    \
                  lu_ref_print_all();                   \
                  LASSERT(0);                           \
                  cfs_spin_lock(&r->lf_guard);      \
          }                                             \
  } while (0)

struct lu_ref_link {
        struct lu_ref    *ll_ref;
        cfs_list_t        ll_linkage;
        const char       *ll_scope;
        const void       *ll_source;
};

static cfs_mem_cache_t *lu_ref_link_kmem;

static struct lu_kmem_descr lu_ref_caches[] = {
        {
                .ckd_cache = &lu_ref_link_kmem,
                .ckd_name  = "lu_ref_link_kmem",
                .ckd_size  = sizeof (struct lu_ref_link)
        },
        {
                .ckd_cache = NULL
        }
};

/**
 * Global list of active (initialized, but not finalized) lu_ref's.
 *
 * Protected by lu_ref_refs_guard.
 */
static CFS_LIST_HEAD(lu_ref_refs);
static cfs_spinlock_t lu_ref_refs_guard;
static struct lu_ref lu_ref_marker = {
        .lf_guard   = CFS_SPIN_LOCK_UNLOCKED,
        .lf_list    = CFS_LIST_HEAD_INIT(lu_ref_marker.lf_list),
        .lf_linkage = CFS_LIST_HEAD_INIT(lu_ref_marker.lf_linkage)
};

void lu_ref_print(const struct lu_ref *ref)
{
        struct lu_ref_link *link;

        CERROR("lu_ref: %p %d %d %s:%d\n",
               ref, ref->lf_refs, ref->lf_failed, ref->lf_func, ref->lf_line);
        cfs_list_for_each_entry(link, &ref->lf_list, ll_linkage) {
                CERROR("     link: %s %p\n", link->ll_scope, link->ll_source);
        }
}
EXPORT_SYMBOL(lu_ref_print);

static int lu_ref_is_marker(const struct lu_ref *ref)
{
        return (ref == &lu_ref_marker);
}

void lu_ref_print_all(void)
{
        struct lu_ref *ref;

        cfs_spin_lock(&lu_ref_refs_guard);
        cfs_list_for_each_entry(ref, &lu_ref_refs, lf_linkage) {
                if (lu_ref_is_marker(ref))
                        continue;

                cfs_spin_lock(&ref->lf_guard);
                lu_ref_print(ref);
                cfs_spin_unlock(&ref->lf_guard);
        }
        cfs_spin_unlock(&lu_ref_refs_guard);
}
EXPORT_SYMBOL(lu_ref_print_all);

void lu_ref_init_loc(struct lu_ref *ref, const char *func, const int line)
{
        ref->lf_refs = 0;
        ref->lf_func = func;
        ref->lf_line = line;
        cfs_spin_lock_init(&ref->lf_guard);
        CFS_INIT_LIST_HEAD(&ref->lf_list);
        cfs_spin_lock(&lu_ref_refs_guard);
        cfs_list_add(&ref->lf_linkage, &lu_ref_refs);
        cfs_spin_unlock(&lu_ref_refs_guard);
}
EXPORT_SYMBOL(lu_ref_init_loc);

void lu_ref_fini(struct lu_ref *ref)
{
        cfs_spin_lock(&ref->lf_guard);
        REFASSERT(ref, cfs_list_empty(&ref->lf_list));
        REFASSERT(ref, ref->lf_refs == 0);
        cfs_spin_unlock(&ref->lf_guard);
        cfs_spin_lock(&lu_ref_refs_guard);
        cfs_list_del_init(&ref->lf_linkage);
        cfs_spin_unlock(&lu_ref_refs_guard);
}
EXPORT_SYMBOL(lu_ref_fini);

static struct lu_ref_link *lu_ref_add_context(struct lu_ref *ref,
                                              enum cfs_alloc_flags flags,
                                              const char *scope,
                                              const void *source)
{
        struct lu_ref_link *link;

        link = NULL;
        if (lu_ref_link_kmem != NULL) {
                OBD_SLAB_ALLOC_PTR_GFP(link, lu_ref_link_kmem, flags);
                if (link != NULL) {
                        link->ll_ref    = ref;
                        link->ll_scope  = scope;
                        link->ll_source = source;
                        cfs_spin_lock(&ref->lf_guard);
                        cfs_list_add_tail(&link->ll_linkage, &ref->lf_list);
                        ref->lf_refs++;
                        cfs_spin_unlock(&ref->lf_guard);
                }
        }

        if (link == NULL) {
                cfs_spin_lock(&ref->lf_guard);
                ref->lf_failed++;
                cfs_spin_unlock(&ref->lf_guard);
                link = ERR_PTR(-ENOMEM);
        }

        return link;
}

struct lu_ref_link *lu_ref_add(struct lu_ref *ref, const char *scope,
                               const void *source)
{
        cfs_might_sleep();
        return lu_ref_add_context(ref, CFS_ALLOC_STD, scope, source);
}
EXPORT_SYMBOL(lu_ref_add);

/**
 * Version of lu_ref_add() to be used in non-blockable contexts.
 */
struct lu_ref_link *lu_ref_add_atomic(struct lu_ref *ref, const char *scope,
                                      const void *source)
{
        return lu_ref_add_context(ref, CFS_ALLOC_ATOMIC, scope, source);
}
EXPORT_SYMBOL(lu_ref_add_atomic);

static inline int lu_ref_link_eq(const struct lu_ref_link *link,
                                 const char *scope, const void *source)
{
        return link->ll_source == source && !strcmp(link->ll_scope, scope);
}

/**
 * Maximal chain length seen so far.
 */
static unsigned lu_ref_chain_max_length = 127;

/**
 * Searches for a lu_ref_link with given [scope, source] within given lu_ref.
 */
static struct lu_ref_link *lu_ref_find(struct lu_ref *ref, const char *scope,
                                       const void *source)
{
        struct lu_ref_link *link;
        unsigned            iterations;

        iterations = 0;
        cfs_list_for_each_entry(link, &ref->lf_list, ll_linkage) {
                ++iterations;
                if (lu_ref_link_eq(link, scope, source)) {
                        if (iterations > lu_ref_chain_max_length) {
                                CWARN("Long lu_ref chain %d \"%s\":%p\n",
                                      iterations, scope, source);
                                lu_ref_chain_max_length = iterations * 3 / 2;
                        }
                        return link;
                }
        }
        return NULL;
}

void lu_ref_del(struct lu_ref *ref, const char *scope, const void *source)
{
        struct lu_ref_link *link;

        cfs_spin_lock(&ref->lf_guard);
        link = lu_ref_find(ref, scope, source);
        if (link != NULL) {
                cfs_list_del(&link->ll_linkage);
                ref->lf_refs--;
                cfs_spin_unlock(&ref->lf_guard);
                OBD_SLAB_FREE(link, lu_ref_link_kmem, sizeof(*link));
        } else {
                REFASSERT(ref, ref->lf_failed > 0);
                ref->lf_failed--;
                cfs_spin_unlock(&ref->lf_guard);
        }
}
EXPORT_SYMBOL(lu_ref_del);

void lu_ref_set_at(struct lu_ref *ref, struct lu_ref_link *link,
                   const char *scope,
                   const void *source0, const void *source1)
{
        cfs_spin_lock(&ref->lf_guard);
        if (link != ERR_PTR(-ENOMEM)) {
                REFASSERT(ref, link->ll_ref == ref);
                REFASSERT(ref, lu_ref_link_eq(link, scope, source0));
                link->ll_source = source1;
        } else {
                REFASSERT(ref, ref->lf_failed > 0);
        }
        cfs_spin_unlock(&ref->lf_guard);
}
EXPORT_SYMBOL(lu_ref_set_at);

void lu_ref_del_at(struct lu_ref *ref, struct lu_ref_link *link,
                   const char *scope, const void *source)
{
        if (link != ERR_PTR(-ENOMEM)) {
                cfs_spin_lock(&ref->lf_guard);
                REFASSERT(ref, link->ll_ref == ref);
                REFASSERT(ref, lu_ref_link_eq(link, scope, source));
                cfs_list_del(&link->ll_linkage);
                ref->lf_refs--;
                cfs_spin_unlock(&ref->lf_guard);
                OBD_SLAB_FREE(link, lu_ref_link_kmem, sizeof(*link));
        } else {
                cfs_spin_lock(&ref->lf_guard);
                REFASSERT(ref, ref->lf_failed > 0);
                ref->lf_failed--;
                cfs_spin_unlock(&ref->lf_guard);
        }
}
EXPORT_SYMBOL(lu_ref_del_at);

#if defined(__KERNEL__) && defined(LPROCFS)

static void *lu_ref_seq_start(struct seq_file *seq, loff_t *pos)
{
        struct lu_ref *ref = seq->private;

        cfs_spin_lock(&lu_ref_refs_guard);
        if (cfs_list_empty(&ref->lf_linkage))
                ref = NULL;
        cfs_spin_unlock(&lu_ref_refs_guard);

        return ref;
}

static void *lu_ref_seq_next(struct seq_file *seq, void *p, loff_t *pos)
{
        struct lu_ref *ref = p;
        struct lu_ref *next;

        LASSERT(seq->private == p);
        LASSERT(!cfs_list_empty(&ref->lf_linkage));

        cfs_spin_lock(&lu_ref_refs_guard);
        next = cfs_list_entry(ref->lf_linkage.next, struct lu_ref, lf_linkage);
        if (&next->lf_linkage == &lu_ref_refs) {
                p = NULL;
        } else {
                (*pos)++;
                cfs_list_move(&ref->lf_linkage, &next->lf_linkage);
        }
        cfs_spin_unlock(&lu_ref_refs_guard);
        return p;
}

static void lu_ref_seq_stop(struct seq_file *seq, void *p)
{
        /* Nothing to do */
}


static int lu_ref_seq_show(struct seq_file *seq, void *p)
{
        struct lu_ref *ref  = p;
        struct lu_ref *next; 

        cfs_spin_lock(&lu_ref_refs_guard);
        next = cfs_list_entry(ref->lf_linkage.next, struct lu_ref, lf_linkage);
        if ((&next->lf_linkage == &lu_ref_refs) || lu_ref_is_marker(next)) {
                cfs_spin_unlock(&lu_ref_refs_guard);
                return 0;
        }

        /* print the entry */

        cfs_spin_lock(&next->lf_guard);
        seq_printf(seq, "lu_ref: %p %d %d %s:%d\n",
                   next, next->lf_refs, next->lf_failed,
                   next->lf_func, next->lf_line);
        if (next->lf_refs > 64) {
                seq_printf(seq, "  too many references, skip\n");
        } else {
                struct lu_ref_link *link;
                int i = 0;

                cfs_list_for_each_entry(link, &next->lf_list, ll_linkage)
                        seq_printf(seq, "  #%d link: %s %p\n",
                                   i++, link->ll_scope, link->ll_source);
        }
        cfs_spin_unlock(&next->lf_guard);
        cfs_spin_unlock(&lu_ref_refs_guard);

        return 0;
}

static struct seq_operations lu_ref_seq_ops = {
        .start = lu_ref_seq_start,
        .stop  = lu_ref_seq_stop,
        .next  = lu_ref_seq_next,
        .show  = lu_ref_seq_show
};

static int lu_ref_seq_open(struct inode *inode, struct file *file)
{
        struct lu_ref *marker = &lu_ref_marker;
        int result = 0;

        result = seq_open(file, &lu_ref_seq_ops);
        if (result == 0) {
                cfs_spin_lock(&lu_ref_refs_guard);
                if (!cfs_list_empty(&marker->lf_linkage))
                        result = -EAGAIN;
                else
                        cfs_list_add(&marker->lf_linkage, &lu_ref_refs);
                cfs_spin_unlock(&lu_ref_refs_guard);

                if (result == 0) {
                        struct seq_file *f = file->private_data;
                        f->private = marker;
                } else {
                        seq_release(inode, file);
                }
        }

        return result;
}

static int lu_ref_seq_release(struct inode *inode, struct file *file)
{
        struct lu_ref *ref = ((struct seq_file *)file->private_data)->private;

        cfs_spin_lock(&lu_ref_refs_guard);
        cfs_list_del_init(&ref->lf_linkage);
        cfs_spin_unlock(&lu_ref_refs_guard);

        return seq_release(inode, file);
}

static struct file_operations lu_ref_dump_fops = {
        .owner   = THIS_MODULE,
        .open    = lu_ref_seq_open,
        .read    = seq_read,
        .llseek  = seq_lseek,
        .release = lu_ref_seq_release
};

#endif

int lu_ref_global_init(void)
{
        int result;

        CDEBUG(D_CONSOLE,
               "lu_ref tracking is enabled. Performance isn't.\n");


        cfs_spin_lock_init(&lu_ref_refs_guard);
        result = lu_kmem_init(lu_ref_caches);

#if defined(__KERNEL__) && defined(LPROCFS)
        if (result == 0) {
                result = lprocfs_seq_create(proc_lustre_root, "lu_refs",
                                            0444, &lu_ref_dump_fops, NULL);
                if (result)
                        lu_kmem_fini(lu_ref_caches);
        }
#endif

        return result;
}

void lu_ref_global_fini(void)
{
#if defined(__KERNEL__) && defined(LPROCFS)
        lprocfs_remove_proc_entry("lu_refs", proc_lustre_root);
#endif
        lu_kmem_fini(lu_ref_caches);
}

#endif /* USE_LU_REF */
