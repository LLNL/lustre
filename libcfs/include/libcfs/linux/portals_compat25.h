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
 */

#ifndef __LIBCFS_LINUX_PORTALS_COMPAT_H__
#define __LIBCFS_LINUX_PORTALS_COMPAT_H__

// XXX BUG 1511 -- remove this stanza and all callers when bug 1511 is resolved
#if defined(SPINLOCK_DEBUG) && SPINLOCK_DEBUG
#  define SIGNAL_MASK_ASSERT() \
   LASSERT(current->sighand->siglock.magic == SPINLOCK_MAGIC)
#else
# define SIGNAL_MASK_ASSERT()
#endif
// XXX BUG 1511 -- remove this stanza and all callers when bug 1511 is resolved

#define SIGNAL_MASK_LOCK(task, flags)                                  \
  spin_lock_irqsave(&task->sighand->siglock, flags)
#define SIGNAL_MASK_UNLOCK(task, flags)                                \
  spin_unlock_irqrestore(&task->sighand->siglock, flags)
#define USERMODEHELPER(path, argv, envp)                               \
  call_usermodehelper(path, argv, envp, 1)
#define RECALC_SIGPENDING         recalc_sigpending()
#define CLEAR_SIGPENDING          clear_tsk_thread_flag(current,       \
                                                        TIF_SIGPENDING)
# define CURRENT_SECONDS           get_seconds()
# define smp_num_cpus              num_online_cpus()

#define cfs_wait_event_interruptible(wq, condition, ret)               \
        ret = wait_event_interruptible(wq, condition)
#define cfs_wait_event_interruptible_exclusive(wq, condition, ret)     \
        ret = wait_event_interruptible_exclusive(wq, condition)

#define UML_PID(tsk) ((tsk)->pid)

#define THREAD_NAME(comm, len, fmt, a...)                              \
        snprintf(comm, len, fmt, ## a)

/* 2.6 alloc_page users can use page->lru */
#define PAGE_LIST_ENTRY lru
#define PAGE_LIST(page) ((page)->lru)

#ifndef HAVE_CPU_ONLINE
#define cfs_cpu_online(cpu) ((1<<cpu) & (cpu_online_map))
#else
#define cfs_cpu_online(cpu) cpu_online(cpu)
#endif

#ifndef __user
#define __user
#endif

#ifndef __fls
#define __cfs_fls fls
#else
#define __cfs_fls __fls
#endif

#ifdef HAVE_5ARGS_SYSCTL_PROC_HANDLER
#define ll_proc_dointvec(table, write, filp, buffer, lenp, ppos)        \
        proc_dointvec(table, write, buffer, lenp, ppos);

#define ll_proc_dolongvec(table, write, filp, buffer, lenp, ppos)        \
        proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
#define ll_proc_dostring(table, write, filp, buffer, lenp, ppos)        \
        proc_dostring(table, write, buffer, lenp, ppos);
#define LL_PROC_PROTO(name)                                             \
        name(cfs_sysctl_table_t *table, int write,                      \
             void __user *buffer, size_t *lenp, loff_t *ppos)
#else
#define ll_proc_dointvec(table, write, filp, buffer, lenp, ppos)        \
        proc_dointvec(table, write, filp, buffer, lenp, ppos);

#define ll_proc_dolongvec(table, write, filp, buffer, lenp, ppos)        \
        proc_doulongvec_minmax(table, write, filp, buffer, lenp, ppos);
#define ll_proc_dostring(table, write, filp, buffer, lenp, ppos)        \
        proc_dostring(table, write, filp, buffer, lenp, ppos);
#define LL_PROC_PROTO(name)                                             \
        name(cfs_sysctl_table_t *table, int write, struct file *filp,   \
             void __user *buffer, size_t *lenp, loff_t *ppos)
#endif
#define DECLARE_LL_PROC_PPOS_DECL

/* helper for sysctl handlers */
int proc_call_handler(void *data, int write,
                      loff_t *ppos, void *buffer, size_t *lenp,
                      int (*handler)(void *data, int write,
                                     loff_t pos, void *buffer, int len));

#endif /* _PORTALS_COMPAT_H */
