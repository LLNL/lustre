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
 *
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef __LIBCFS_LIBCFS_H__
#define __LIBCFS_LIBCFS_H__

#if !__GNUC__
#define __attribute__(x)
#endif

#if !defined(__WINNT__) && !defined(__KERNEL__)
#include <libcfs/posix/libcfs.h>
#elif defined(__linux__)
#include <libcfs/linux/libcfs.h>
#elif defined(__APPLE__)
#include <libcfs/darwin/libcfs.h>
#elif defined(__WINNT__)
#include <libcfs/winnt/libcfs.h>
#else
#error Unsupported operating system.
#endif

#include "curproc.h"

#ifndef offsetof
# define offsetof(typ,memb) ((long)(long_ptr_t)((char *)&(((typ *)0)->memb)))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) ((sizeof (a)) / (sizeof ((a)[0])))
#endif

#if !defined(container_of)
/* given a pointer @ptr to the field @member embedded into type (usually
 * struct) @type, return pointer to the embedding instance of @type. */
#define container_of(ptr, type, member) \
        ((type *)((char *)(ptr)-(char *)(&((type *)0)->member)))
#endif

static inline int __is_po2(unsigned long long val)
{
        return !(val & (val - 1));
}

#define IS_PO2(val) __is_po2((unsigned long long)(val))

#define LOWEST_BIT_SET(x)       ((x) & ~((x) - 1))

/*
 * Lustre Error Checksum: calculates checksum
 * of Hex number by XORing each bit.
 */
#define LERRCHKSUM(hexnum) (((hexnum) & 0xf) ^ ((hexnum) >> 4 & 0xf) ^ \
                           ((hexnum) >> 8 & 0xf))


/*
 * Some (nomina odiosa sunt) platforms define NULL as naked 0. This confuses
 * Lustre RETURN(NULL) macro.
 */
#if defined(NULL)
#undef NULL
#endif

#define NULL ((void *)0)

#define LUSTRE_SRV_LNET_PID      LUSTRE_LNET_PID

#ifdef __KERNEL__

#include <libcfs/list.h>

#ifndef cfs_for_each_possible_cpu
#  error cfs_for_each_possible_cpu is not supported by kernel!
#endif

/* libcfs tcpip */
int libcfs_ipif_query(char *name, int *up, __u32 *ip, __u32 *mask);
int libcfs_ipif_enumerate(char ***names);
void libcfs_ipif_free_enumeration(char **names, int n);
int libcfs_sock_listen(cfs_socket_t **sockp, __u32 ip, int port, int backlog);
int libcfs_sock_accept(cfs_socket_t **newsockp, cfs_socket_t *sock);
void libcfs_sock_abort_accept(cfs_socket_t *sock);
int libcfs_sock_connect(cfs_socket_t **sockp, int *fatal,
                        __u32 local_ip, int local_port,
                        __u32 peer_ip, int peer_port);
int libcfs_sock_setbuf(cfs_socket_t *socket, int txbufsize, int rxbufsize);
int libcfs_sock_getbuf(cfs_socket_t *socket, int *txbufsize, int *rxbufsize);
int libcfs_sock_getaddr(cfs_socket_t *socket, int remote, __u32 *ip, int *port);
int libcfs_sock_write(cfs_socket_t *sock, void *buffer, int nob, int timeout);
int libcfs_sock_read(cfs_socket_t *sock, void *buffer, int nob, int timeout);
void libcfs_sock_release(cfs_socket_t *sock);

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

/* Dump a debug log */
void lc_watchdog_dumplog(pid_t pid, void *data);

#endif /* __KERNEL__ */

/* need both kernel and user-land acceptor */
#define LNET_ACCEPTOR_MIN_RESERVED_PORT    512
#define LNET_ACCEPTOR_MAX_RESERVED_PORT    1023

/*
 * libcfs pseudo device operations
 *
 * struct cfs_psdev_t and
 * cfs_psdev_register() and
 * cfs_psdev_deregister() are declared in
 * libcfs/<os>/<os>-prim.h
 *
 * It's just draft now.
 */

struct cfs_psdev_file {
        unsigned long   off;
        void            *private_data;
        unsigned long   reserved1;
        unsigned long   reserved2;
};

struct cfs_psdev_ops {
        int (*p_open)(unsigned long, void *);
        int (*p_close)(unsigned long, void *);
        int (*p_read)(struct cfs_psdev_file *, char *, unsigned long);
        int (*p_write)(struct cfs_psdev_file *, char *, unsigned long);
        int (*p_ioctl)(struct cfs_psdev_file *, unsigned long, void *);
};

/*
 * Universal memory allocator API
 */
enum cfs_alloc_flags {
        /* allocation is not allowed to block */
        CFS_ALLOC_ATOMIC = 0x1,
        /* allocation is allowed to block */
        CFS_ALLOC_WAIT   = 0x2,
        /* allocation should return zeroed memory */
        CFS_ALLOC_ZERO   = 0x4,
        /* allocation is allowed to call file-system code to free/clean
         * memory */
        CFS_ALLOC_FS     = 0x8,
        /* allocation is allowed to do io to free/clean memory */
        CFS_ALLOC_IO     = 0x10,
        /* don't report allocation failure to the console */
        CFS_ALLOC_NOWARN = 0x20,
        /* standard allocator flag combination */
        CFS_ALLOC_STD    = CFS_ALLOC_FS | CFS_ALLOC_IO,
        CFS_ALLOC_USER   = CFS_ALLOC_WAIT | CFS_ALLOC_FS | CFS_ALLOC_IO,
};

/* flags for cfs_page_alloc() in addition to enum cfs_alloc_flags */
enum cfs_alloc_page_flags {
        /* allow to return page beyond KVM. It has to be mapped into KVM by
         * cfs_page_map(); */
        CFS_ALLOC_HIGH   = 0x40,
        CFS_ALLOC_HIGHUSER = CFS_ALLOC_WAIT | CFS_ALLOC_FS | CFS_ALLOC_IO | CFS_ALLOC_HIGH,
};

/*
 * Drop into debugger, if possible. Implementation is provided by platform.
 */

void cfs_enter_debugger(void);

/*
 * Defined by platform
 */
void cfs_daemonize(char *str);
int cfs_daemonize_ctxt(char *str);
cfs_sigset_t cfs_get_blocked_sigs(void);
cfs_sigset_t cfs_block_allsigs(void);
cfs_sigset_t cfs_block_sigs(unsigned long sigs);
cfs_sigset_t cfs_block_sigsinv(unsigned long sigs);
void cfs_restore_sigs(cfs_sigset_t);
int cfs_signal_pending(void);
void cfs_clear_sigpending(void);

/*
 * XXX Liang:
 * these macros should be removed in the future,
 * we keep them just for keeping libcfs compatible
 * with other branches.
 */
#define libcfs_daemonize(s)     cfs_daemonize(s)
#define cfs_sigmask_lock(f)     do { f= 0; } while (0)
#define cfs_sigmask_unlock(f)   do { f= 0; } while (0)

int convert_server_error(__u64 ecode);
int convert_client_oflag(int cflag, int *result);

/*
 * Stack-tracing filling.
 */

/*
 * Platform-dependent data-type to hold stack frames.
 */
struct cfs_stack_trace;

/*
 * Fill @trace with current back-trace.
 */
void cfs_stack_trace_fill(struct cfs_stack_trace *trace);

/*
 * Return instruction pointer for frame @frame_no. NULL if @frame_no is
 * invalid.
 */
void *cfs_stack_trace_frame(struct cfs_stack_trace *trace, int frame_no);

#ifndef O_NOACCESS
#define O_NOACCESS O_NONBLOCK
#endif

/*
 * Universal open flags.
 */
#define CFS_O_NOACCESS          0003
#define CFS_O_ACCMODE           CFS_O_NOACCESS
#define CFS_O_CREAT             0100
#define CFS_O_EXCL              0200
#define CFS_O_NOCTTY            0400
#define CFS_O_TRUNC             01000
#define CFS_O_APPEND            02000
#define CFS_O_NONBLOCK          04000
#define CFS_O_NDELAY            CFS_O_NONBLOCK
#define CFS_O_SYNC              010000
#define CFS_O_ASYNC             020000
#define CFS_O_DIRECT            040000
#define CFS_O_LARGEFILE         0100000
#define CFS_O_DIRECTORY         0200000
#define CFS_O_NOFOLLOW          0400000
#define CFS_O_NOATIME           01000000

/* convert local open flags to universal open flags */
int cfs_oflags2univ(int flags);
/* convert universal open flags to local open flags */
int cfs_univ2oflags(int flags);

/*
 * Random number handling
 */

/* returns a random 32-bit integer */
unsigned int cfs_rand(void);
/* seed the generator */
void cfs_srand(unsigned int, unsigned int);
void cfs_get_random_bytes(void *buf, int size);

#include <libcfs/libcfs_debug.h>
#include <libcfs/libcfs_private.h>
#include <libcfs/libcfs_ioctl.h>
#include <libcfs/libcfs_prim.h>
#include <libcfs/libcfs_time.h>
#include <libcfs/libcfs_string.h>
#include <libcfs/libcfs_kernelcomm.h>
#include <libcfs/libcfs_workitem.h>
#include <libcfs/libcfs_hash.h>
#include <libcfs/libcfs_fail.h>
#include <libcfs/params_tree.h>

/* container_of depends on "likely" which is defined in libcfs_private.h */
static inline void *__container_of(void *ptr, unsigned long shift)
{
        if (unlikely(IS_ERR(ptr) || ptr == NULL))
                return ptr;
        else
                return (char *)ptr - shift;
}

#define container_of0(ptr, type, member) \
        ((type *)__container_of((void *)(ptr), offsetof(type, member)))

#define _LIBCFS_H

#endif /* _LIBCFS_H */
