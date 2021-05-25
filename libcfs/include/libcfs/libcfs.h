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
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef __LIBCFS_LIBCFS_H__
#define __LIBCFS_LIBCFS_H__

#include <linux/kernel.h>
#include <linux/module.h>

#include <libcfs/linux/linux-misc.h>
#include <libcfs/linux/linux-time.h>
#include <libcfs/linux/linux-wait.h>

#include <uapi/linux/lnet/libcfs_ioctl.h>
#include <libcfs/libcfs_debug.h>
#include <libcfs/libcfs_private.h>
#include <libcfs/bitmap.h>
#include <libcfs/libcfs_cpu.h>
#include <libcfs/libcfs_prim.h>
#include <libcfs/libcfs_string.h>
#include <libcfs/libcfs_workitem.h>
#include <libcfs/libcfs_hash.h>
#include <libcfs/libcfs_heap.h>
#include <libcfs/libcfs_fail.h>
#include "curproc.h"

#define LIBCFS_VERSION	"0.7.1"

#define PO2_ROUNDUP_TYPED(x, po2, type) (-(-(type)(x) & -(type)(po2)))
#define LOWEST_BIT_SET(x) ((x) & ~((x) - 1))

/* Sparse annotations */
#if !defined(__must_hold)
# ifdef __CHECKER__
#  define __must_hold(x) __attribute__((context(x, 1, 1)))
# else	/* __CHECKER__ */
#  define __must_hold(x)
# endif /* !__CHECKER__ */
#endif /* !__must_hold */

/* libcfs watchdogs */
struct lc_watchdog;

/* Add a watchdog which fires after "time" milliseconds of delay.  You have to
 * touch it once to enable it. */
struct lc_watchdog *lc_watchdog_add(int time,
                                    void (*cb)(pid_t pid, void *),
                                    void *data);

/* Enables a watchdog and resets its timer. */
void lc_watchdog_touch(struct lc_watchdog *lcw, int timeout);
#define CFS_GET_TIMEOUT(svc) (max_t(int, obd_timeout,                   \
                          AT_OFF ? 0 : at_get(&svc->srv_at_estimate)) * \
                          svc->srv_watchdog_factor)

/* Disable a watchdog; touch it to restart it. */
void lc_watchdog_disable(struct lc_watchdog *lcw);

/* Clean up the watchdog */
void lc_watchdog_delete(struct lc_watchdog *lcw);

#ifdef HAVE_TOTALRAM_PAGES_AS_FUNC
 #ifndef cfs_totalram_pages
  #define cfs_totalram_pages() totalram_pages()
 #endif
#else
 #ifndef cfs_totalram_pages
  #define cfs_totalram_pages() totalram_pages
 #endif
#endif

/* need both kernel and user-land acceptor */
#define LNET_ACCEPTOR_MIN_RESERVED_PORT    512
#define LNET_ACCEPTOR_MAX_RESERVED_PORT    1023

extern struct blocking_notifier_head libcfs_ioctl_list;
static inline int notifier_from_ioctl_errno(int err)
{
	if (err == -EINVAL)
		return NOTIFY_OK;
	return notifier_from_errno(err) | NOTIFY_STOP_MASK;
}

/*
 * Defined by platform
 */
int unshare_fs_struct(void);
sigset_t cfs_block_allsigs(void);
sigset_t cfs_block_sigs(unsigned long sigs);
sigset_t cfs_block_sigsinv(unsigned long sigs);
void cfs_restore_sigs(sigset_t);
void cfs_clear_sigpending(void);

/*
 * Random number handling
 */

/* returns a random 32-bit integer */
unsigned int cfs_rand(void);
/* seed the generator */
void cfs_srand(unsigned int, unsigned int);
void cfs_get_random_bytes(void *buf, int size);

int libcfs_ioctl_data_adjust(struct libcfs_ioctl_data *data);
int libcfs_ioctl(unsigned long cmd, void __user *uparam);

/* container_of depends on "likely" which is defined in libcfs_private.h */
static inline void *__container_of(const void *ptr, unsigned long shift)
{
	if (unlikely(IS_ERR(ptr) || ptr == NULL))
		return ERR_CAST(ptr);
	else
		return (char *)ptr - shift;
}

#define container_of0(ptr, type, member) \
	((type *)__container_of((ptr), offsetof(type, member)))

struct lnet_debugfs_symlink_def {
	const char *name;
	const char *target;
};

void lnet_insert_debugfs(struct ctl_table *table);
void lnet_remove_debugfs(struct ctl_table *table);

/* helper for sysctl handlers */
int lprocfs_call_handler(void *data, int write, loff_t *ppos,
			 void __user *buffer, size_t *lenp,
			 int (*handler)(void *data, int write, loff_t pos,
					void __user *buffer, int len));

/* atomic-context safe vfree */
#ifdef HAVE_LIBCFS_VFREE_ATOMIC
void libcfs_vfree_atomic(const void *addr);
#else
#define libcfs_vfree_atomic(ptr) vfree(ptr)
#endif

#endif /* _LIBCFS_LIBCFS_H_ */
