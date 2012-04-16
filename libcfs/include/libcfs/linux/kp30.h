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

#ifndef __LIBCFS_LINUX_KP30_H__
#define __LIBCFS_LINUX_KP30_H__


#ifndef AUTOCONF_INCLUDED
# include <linux/config.h>
#endif
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <asm/system.h>
#include <linux/kmod.h>
#include <linux/notifier.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/vmalloc.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/smp_lock.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <linux/rwsem.h>
#include <linux/proc_fs.h>
#include <linux/file.h>
#include <linux/smp.h>
#include <linux/ctype.h>
#include <linux/compiler.h>
#ifdef HAVE_MM_INLINE
# include <linux/mm_inline.h>
#endif
#include <linux/kallsyms.h>
#include <linux/moduleparam.h>
#include <linux/scatterlist.h>

#include <libcfs/linux/portals_compat25.h>

#ifdef HAVE_3ARGS_INIT_WORK

#define prepare_work(wq,cb,cbdata)                                            \
do {                                                                          \
        INIT_WORK((wq), (void *)(cb), (void *)(cbdata));                      \
} while (0)

#define cfs_get_work_data(type,field,data)   (data)

#else

#define prepare_work(wq,cb,cbdata)                                            \
do {                                                                          \
        INIT_WORK((wq), (void *)(cb));                                        \
} while (0)

#define cfs_get_work_data(type,field,data) container_of(data,type,field)

#endif

#define cfs_num_online_cpus() num_online_cpus()
#define wait_on_page wait_on_page_locked
#define our_recalc_sigpending(current) recalc_sigpending()
#define strtok(a,b) strpbrk(a, b)
#define work_struct_t      struct work_struct

#ifdef CONFIG_SMP
#define LASSERT_SPIN_LOCKED(lock) LASSERT(spin_is_locked(lock))
#define LINVRNT_SPIN_LOCKED(lock) LINVRNT(spin_is_locked(lock))
#else
#define LASSERT_SPIN_LOCKED(lock) do {(void)sizeof(lock);} while(0)
#define LINVRNT_SPIN_LOCKED(lock) do {(void)sizeof(lock);} while(0)
#endif
#define LASSERT_SEM_LOCKED(sem) LASSERT(down_trylock(sem) != 0)

#ifdef HAVE_SEM_COUNT_ATOMIC
#define SEM_COUNT(sem)          (atomic_read(&(sem)->count))
#else
#define SEM_COUNT(sem)          ((sem)->count)
#endif

#define LIBCFS_PANIC(msg)            panic(msg)

/* ------------------------------------------------------------------- */

#define PORTAL_SYMBOL_REGISTER(x)
#define PORTAL_SYMBOL_UNREGISTER(x)

#define PORTAL_SYMBOL_GET(x) symbol_get(x)
#define PORTAL_SYMBOL_PUT(x) symbol_put(x)

#define PORTAL_MODULE_USE       try_module_get(THIS_MODULE)
#define PORTAL_MODULE_UNUSE     module_put(THIS_MODULE)


/******************************************************************************/
/* Module parameter support */
#define CFS_MODULE_PARM(name, t, type, perm, desc) \
        module_param(name, type, perm);\
        MODULE_PARM_DESC(name, desc)

#define CFS_SYSFS_MODULE_PARM  1 /* module parameters accessible via sysfs */

/******************************************************************************/

#if (__GNUC__)
/* Use the special GNU C __attribute__ hack to have the compiler check the
 * printf style argument string against the actual argument count and
 * types.
 */
#ifdef printf
# warning printf has been defined as a macro...
# undef printf
#endif

#endif /* __GNUC__ */

# define fprintf(a, format, b...) CDEBUG(D_OTHER, format , ## b)
# define printf(format, b...) CDEBUG(D_OTHER, format , ## b)
# define time(a) CURRENT_TIME

#ifndef num_possible_cpus
#define cfs_num_possible_cpus() NR_CPUS
#else
#define cfs_num_possible_cpus() num_possible_cpus()
#endif

/******************************************************************************/
/* Light-weight trace
 * Support for temporary event tracing with minimal Heisenberg effect. */
#define LWT_SUPPORT  0

#define LWT_MEMORY   (16<<20)

#ifndef KLWT_SUPPORT
# if defined(__KERNEL__)
#  if !defined(BITS_PER_LONG)
#   error "BITS_PER_LONG not defined"
#  endif
# elif !defined(__WORDSIZE)
#  error "__WORDSIZE not defined"
# else
#  define BITS_PER_LONG __WORDSIZE
# endif

/* kernel hasn't defined this? */
typedef struct {
        long long   lwte_when;
        char       *lwte_where;
        void       *lwte_task;
        long        lwte_p1;
        long        lwte_p2;
        long        lwte_p3;
        long        lwte_p4;
# if BITS_PER_LONG > 32
        long        lwte_pad;
# endif
} lwt_event_t;
#endif /* !KLWT_SUPPORT */

#if LWT_SUPPORT
# ifdef __KERNEL__
#  if !KLWT_SUPPORT

typedef struct _lwt_page {
        cfs_list_t               lwtp_list;
        struct page             *lwtp_page;
        lwt_event_t             *lwtp_events;
} lwt_page_t;

typedef struct {
        int                lwtc_current_index;
        lwt_page_t        *lwtc_current_page;
} lwt_cpu_t;

extern int       lwt_enabled;
extern lwt_cpu_t lwt_cpus[];

/* Note that we _don't_ define LWT_EVENT at all if LWT_SUPPORT isn't set.
 * This stuff is meant for finding specific problems; it never stays in
 * production code... */

#define LWTSTR(n)       #n
#define LWTWHERE(f,l)   f ":" LWTSTR(l)
#define LWT_EVENTS_PER_PAGE (CFS_PAGE_SIZE / sizeof (lwt_event_t))

#define LWT_EVENT(p1, p2, p3, p4)                                       \
do {                                                                    \
        unsigned long    flags;                                         \
        lwt_cpu_t       *cpu;                                           \
        lwt_page_t      *p;                                             \
        lwt_event_t     *e;                                             \
                                                                        \
        if (lwt_enabled) {                                              \
                local_irq_save (flags);                                 \
                                                                        \
                cpu = &lwt_cpus[smp_processor_id()];                    \
                p = cpu->lwtc_current_page;                             \
                e = &p->lwtp_events[cpu->lwtc_current_index++];         \
                                                                        \
                if (cpu->lwtc_current_index >= LWT_EVENTS_PER_PAGE) {   \
                        cpu->lwtc_current_page =                        \
                                cfs_list_entry (p->lwtp_list.next,      \
                                                lwt_page_t, lwtp_list); \
                        cpu->lwtc_current_index = 0;                    \
                }                                                       \
                                                                        \
                e->lwte_when  = get_cycles();                           \
                e->lwte_where = LWTWHERE(__FILE__,__LINE__);            \
                e->lwte_task  = current;                                \
                e->lwte_p1    = (long)(p1);                             \
                e->lwte_p2    = (long)(p2);                             \
                e->lwte_p3    = (long)(p3);                             \
                e->lwte_p4    = (long)(p4);                             \
                                                                        \
                local_irq_restore (flags);                              \
        }                                                               \
} while (0)

#endif /* !KLWT_SUPPORT */

extern int  lwt_init (void);
extern void lwt_fini (void);
extern int  lwt_lookup_string (int *size, char *knlptr,
                               char *usrptr, int usrsize);
extern int  lwt_control (int enable, int clear);
extern int  lwt_snapshot (cfs_cycles_t *now, int *ncpu, int *total_size,
                          void *user_ptr, int user_size);
# else  /* __KERNEL__ */
#  define LWT_EVENT(p1,p2,p3,p4)     /* no userland implementation yet */
# endif /* __KERNEL__ */
#endif /* LWT_SUPPORT */

/* ------------------------------------------------------------------ */

#define IOCTL_LIBCFS_TYPE long

#ifdef __CYGWIN__
# ifndef BITS_PER_LONG
#  if (~0UL) == 0xffffffffUL
#   define BITS_PER_LONG 32
#  else
#   define BITS_PER_LONG 64
#  endif
# endif
#endif

#if BITS_PER_LONG > 32
# define LI_POISON ((int)0x5a5a5a5a5a5a5a5a)
# define LL_POISON ((long)0x5a5a5a5a5a5a5a5a)
# define LP_POISON ((void *)(long)0x5a5a5a5a5a5a5a5a)
#else
# define LI_POISON ((int)0x5a5a5a5a)
# define LL_POISON ((long)0x5a5a5a5a)
# define LP_POISON ((void *)(long)0x5a5a5a5a)
#endif

/* this is a bit chunky */

#define _LWORDSIZE BITS_PER_LONG

#if (defined(__KERNEL__) && defined(HAVE_KERN__U64_LONG_LONG)) || \
    (!defined(__KERNEL__) && defined(HAVE_USER__U64_LONG_LONG))
# define LPU64 "%Lu"
# define LPD64 "%Ld"
# define LPX64 "%#Lx"
# define LPX64i "%Lx"
# define LPO64 "%#Lo"
# define LPF64 "L"
#else
# define LPU64 "%lu"
# define LPD64 "%ld"
# define LPX64 "%#lx"
# define LPX64i "%lx"
# define LPO64 "%#lo"
# define LPF64 "l"
#endif

/*
 * long_ptr_t & ulong_ptr_t, same to "long" for gcc
 */
# define LPLU "%lu"
# define LPLD "%ld"
# define LPLX "%#lx"

/*
 * pid_t
 */
# define LPPID "%d"

#ifndef LPU64
# error "No word size defined"
#endif

#undef _LWORDSIZE

/* compat macroses */
#ifndef HAVE_SCATTERLIST_SETPAGE
static inline void sg_set_page(struct scatterlist *sg, struct page *page,
                               unsigned int len, unsigned int offset)
{
        sg->page = page;
        sg->offset = offset;
        sg->length = len;
}
#endif

#define cfs_smp_processor_id()  smp_processor_id()

#ifndef get_cpu
# ifdef CONFIG_PREEMPT
#  define cfs_get_cpu()  ({ preempt_disable(); smp_processor_id(); })
#  define cfs_put_cpu()  preempt_enable()
# else
#  define cfs_get_cpu()  smp_processor_id()
#  define cfs_put_cpu()
# endif
#else
# define cfs_get_cpu()   get_cpu()
# define cfs_put_cpu()   put_cpu()
#endif /* get_cpu & put_cpu */

#endif
