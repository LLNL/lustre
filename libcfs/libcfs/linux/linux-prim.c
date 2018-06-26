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
 * Copyright (c) 2011, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_LNET
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/sched.h>
#ifdef HAVE_SCHED_HEADERS
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#endif
#include <linux/uaccess.h>
#include <libcfs/libcfs.h>

#if defined(CONFIG_KGDB)
#include <asm/kgdb.h>
#endif

#ifndef HAVE_KTIME_GET_TS64
void ktime_get_ts64(struct timespec64 *ts)
{
	struct timespec now;

	ktime_get_ts(&now);
	*ts = timespec_to_timespec64(now);
}
EXPORT_SYMBOL(ktime_get_ts64);
#endif /* HAVE_KTIME_GET_TS64 */

#ifndef HAVE_KTIME_GET_REAL_TS64
void ktime_get_real_ts64(struct timespec64 *ts)
{
	struct timespec now;

	getnstimeofday(&now);
	*ts = timespec_to_timespec64(now);
}
EXPORT_SYMBOL(ktime_get_real_ts64);
#endif /* HAVE_KTIME_GET_REAL_TS64 */

#ifndef HAVE_KTIME_GET_REAL_SECONDS
/*
 * Get the seconds portion of CLOCK_REALTIME (wall clock).
 * This is the clock that can be altered by NTP and is
 * independent of a reboot.
 */
time64_t ktime_get_real_seconds(void)
{
	return (time64_t)get_seconds();
}
EXPORT_SYMBOL(ktime_get_real_seconds);
#endif /* HAVE_KTIME_GET_REAL_SECONDS */

#ifndef HAVE_KTIME_GET_SECONDS
/*
 * Get the seconds portion of CLOCK_MONOTONIC
 * This clock is immutable and is reset across
 * reboots. For older platforms this is a
 * wrapper around get_seconds which is valid
 * until 2038. By that time this will be gone
 * one would hope.
 */
time64_t ktime_get_seconds(void)
{
	struct timespec64 now;

	ktime_get_ts64(&now);
	return now.tv_sec;
}
EXPORT_SYMBOL(ktime_get_seconds);
#endif /* HAVE_KTIME_GET_SECONDS */

int cfs_kernel_write(struct file *filp, const void *buf, size_t count,
		     loff_t *pos)
{
#ifdef HAVE_NEW_KERNEL_WRITE
	return kernel_write(filp, buf, count, pos);
#else
	mm_segment_t __old_fs = get_fs();
	int rc;

	set_fs(get_ds());
	rc = vfs_write(filp, (__force const char __user *)buf, count, pos);
	set_fs(__old_fs);

	return rc;
#endif
}
EXPORT_SYMBOL(cfs_kernel_write);

#ifndef HAVE_KSET_FIND_OBJ
struct kobject *kset_find_obj(struct kset *kset, const char *name)
{
	struct kobject *ret = NULL;
	struct kobject *k;

	spin_lock(&kset->list_lock);

	list_for_each_entry(k, &kset->list, entry) {
		if (kobject_name(k) && !strcmp(kobject_name(k), name)) {
			if (kref_get_unless_zero(&k->kref))
				ret = k;
			break;
		}
	}

	spin_unlock(&kset->list_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(kset_find_obj);
#endif

#ifndef HAVE_KSTRTOBOOL_FROM_USER
int kstrtobool_from_user(const char __user *s, size_t count, bool *res)
{
	/* Longest string needed to differentiate, newline, terminator */
	char buf[4];

	count = min(count, sizeof(buf) - 1);
	if (copy_from_user(buf, s, count))
		return -EFAULT;
	buf[count] = '\0';
	return strtobool(buf, res);
}
EXPORT_SYMBOL(kstrtobool_from_user);
#endif /* !HAVE_KSTRTOBOOL_FROM_USER */

sigset_t
cfs_block_allsigs(void)
{
	unsigned long	flags;
	sigset_t	old;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	old = current->blocked;
	sigfillset(&current->blocked);
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
	return old;
}
EXPORT_SYMBOL(cfs_block_allsigs);

sigset_t cfs_block_sigs(unsigned long sigs)
{
	unsigned long  flags;
	sigset_t	old;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	old = current->blocked;
	sigaddsetmask(&current->blocked, sigs);
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
	return old;
}
EXPORT_SYMBOL(cfs_block_sigs);

/* Block all signals except for the @sigs */
sigset_t cfs_block_sigsinv(unsigned long sigs)
{
	unsigned long flags;
	sigset_t old;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	old = current->blocked;
	sigaddsetmask(&current->blocked, ~sigs);
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
	return old;
}
EXPORT_SYMBOL(cfs_block_sigsinv);

void
cfs_restore_sigs(sigset_t old)
{
	unsigned long  flags;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	current->blocked = old;
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
}
EXPORT_SYMBOL(cfs_restore_sigs);

void
cfs_clear_sigpending(void)
{
	unsigned long flags;

	spin_lock_irqsave(&current->sighand->siglock, flags);
	clear_tsk_thread_flag(current, TIF_SIGPENDING);
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
}
EXPORT_SYMBOL(cfs_clear_sigpending);
