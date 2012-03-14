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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_PORTAL_ALLOC
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#ifndef __LINUX_SOCKNAL_LIB_H__
#define __LINUX_SOCKNAL_LIB_H__

#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <linux/uio.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/irq.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <linux/kmod.h>
#include <linux/sysctl.h>
#include <asm/uaccess.h>
#include <asm/div64.h>
#include <linux/syscalls.h>

#include <libcfs/libcfs.h>
#include <libcfs/linux/portals_compat25.h>

#include <linux/crc32.h>
static inline __u32 ksocknal_csum(__u32 crc, unsigned char const *p, size_t len)
{
#if 1
        return crc32_le(crc, p, len);
#else
        while (len-- > 0)
                crc = ((crc + 0x100) & ~0xff) | ((crc + *p++) & 0xff) ;
        return crc;
#endif
}

#define SOCKNAL_WSPACE(sk)       sk_stream_wspace(sk)
#define SOCKNAL_MIN_WSPACE(sk)   sk_stream_min_wspace(sk)

#ifndef CONFIG_SMP
static inline
int ksocknal_nsched(void)
{
        return 1;
}
#else
# if !defined(CONFIG_X86) || defined(CONFIG_X86_64) || !defined(CONFIG_X86_HT)
static inline int
ksocknal_nsched(void)
{
        return num_online_cpus();
}

static inline int
ksocknal_sched2cpu(int i)
{
        return i;
}

static inline int
ksocknal_irqsched2cpu(int i)
{
        return i;
}
# else
static inline int
ksocknal_nsched(void)
{
        if (smp_num_siblings == 1)
                return (num_online_cpus());

        /* We need to know if this assumption is crap */
        LASSERT (smp_num_siblings == 2);
        return (num_online_cpus()/2);
}

static inline int
ksocknal_sched2cpu(int i)
{
        if (smp_num_siblings == 1)
                return i;

        return (i * 2);
}

static inline int
ksocknal_irqsched2cpu(int i)
{
        return (ksocknal_sched2cpu(i) + 1);
}
# endif
#endif

#endif
