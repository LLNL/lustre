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
 * libcfs/include/libcfs/libcfs_private.h
 *
 * Various defines for libcfs.
 *
 */

#ifndef __LIBCFS_PRIVATE_H__
#define __LIBCFS_PRIVATE_H__

/* XXX this layering violation is for nidstrings */
#include <lnet/types.h>

#ifndef DEBUG_SUBSYSTEM
# define DEBUG_SUBSYSTEM S_UNDEFINED
#endif

#ifdef __KERNEL__

#ifdef LIBCFS_DEBUG

/*
 * When this is on, LASSERT macro includes check for assignment used instead
 * of equality check, but doesn't have unlikely(). Turn this on from time to
 * time to make test-builds. This shouldn't be on for production release.
 */
#define LASSERT_CHECKED (0)

#if LASSERT_CHECKED
/*
 * Assertion.
 *
 * Strange construction with empty "then" clause is used to trigger compiler
 * warnings on the assertions of the form LASSERT(a = b);
 *
 * "warning: suggest parentheses around assignment used as truth value"
 *
 * requires -Wall. Unfortunately this rules out use of likely/unlikely.
 */
#define LASSERT(cond)                                                   \
do {                                                                    \
        if (cond)                                                       \
                ;                                                       \
        else {                                                          \
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, NULL);              \
                msgdata.msg_mask = D_EMERG;                             \
                libcfs_assertion_failed( #cond , &msgdata);             \
        }                                                               \
} while(0)

#define LASSERTF(cond, fmt, ...)                                        \
do {                                                                    \
        if (cond)                                                       \
                ;                                                       \
        else {                                                          \
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, NULL);              \
                msgdata.msg_mask = D_EMERG;                             \
                libcfs_debug_msg(&msgdata,                              \
                                 "ASSERTION(" #cond ") failed: " fmt,   \
                                  ## __VA_ARGS__);                      \
                 LBUG();                                                \
         }                                                              \
} while(0)

#else /* !LASSERT_CHECKED */

#define LASSERT(cond)                                                   \
do {                                                                    \
        if (unlikely(!(cond))) {                                        \
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, NULL);              \
                msgdata.msg_mask = D_EMERG;                             \
                libcfs_assertion_failed(#cond , &msgdata);              \
        }                                                               \
} while(0)

#define LASSERTF(cond, fmt, ...)                                        \
do {                                                                    \
        if (unlikely(!(cond))) {                                        \
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, NULL);              \
                msgdata.msg_mask = D_EMERG;                             \
                libcfs_debug_msg(&msgdata,                              \
                                 "ASSERTION(" #cond ") failed: " fmt,   \
                                 ## __VA_ARGS__ );                      \
                LBUG();                                                 \
        }                                                               \
} while(0)

#endif /* !LASSERT_CHECKED */
#else /* !LIBCFS_DEBUG */
/* sizeof is to use expression without evaluating it. */
# define LASSERT(e) ((void)sizeof!!(e))
# define LASSERTF(cond, ...) ((void)sizeof!!(cond))
#endif /* !LIBCFS_DEBUG */

#ifdef INVARIANT_CHECK
/**
 * This is for more expensive checks that one doesn't want to be enabled all
 * the time. LINVRNT() has to be explicitly enabled by --enable-invariants
 * configure option.
 */
# define LINVRNT(exp) LASSERT(exp)
#else
# define LINVRNT(exp) ((void)sizeof!!(exp))
#endif

#define KLASSERT(e) LASSERT(e)

void lbug_with_loc(struct libcfs_debug_msg_data *) __attribute__((noreturn));

#define LBUG()                                                          \
do {                                                                    \
        LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, NULL);                      \
        msgdata.msg_mask = D_EMERG;                                     \
        lbug_with_loc(&msgdata);                                        \
} while(0)

extern cfs_atomic_t libcfs_kmemory;
/*
 * Memory
 */
#ifdef LIBCFS_DEBUG

# define libcfs_kmem_inc(ptr, size)             \
do {                                            \
        cfs_atomic_add(size, &libcfs_kmemory);  \
} while (0)

# define libcfs_kmem_dec(ptr, size) do {        \
        cfs_atomic_sub(size, &libcfs_kmemory);  \
} while (0)

#else
# define libcfs_kmem_inc(ptr, size) do {} while (0)
# define libcfs_kmem_dec(ptr, size) do {} while (0)
#endif /* LIBCFS_DEBUG */

#ifndef LIBCFS_VMALLOC_SIZE
#define LIBCFS_VMALLOC_SIZE        (2 << CFS_PAGE_SHIFT) /* 2 pages */
#endif

#define LIBCFS_ALLOC_GFP(ptr, size, mask)                                 \
do {                                                                      \
        LASSERT(!cfs_in_interrupt() ||                                    \
               (size <= LIBCFS_VMALLOC_SIZE && mask == CFS_ALLOC_ATOMIC));\
        if (unlikely((size) > LIBCFS_VMALLOC_SIZE))                       \
                (ptr) = cfs_alloc_large(size);                            \
        else                                                              \
                (ptr) = cfs_alloc((size), (mask));                        \
        if (unlikely((ptr) == NULL)) {                                    \
                CERROR("LNET: out of memory at %s:%d (tried to alloc '"   \
                       #ptr "' = %d)\n", __FILE__, __LINE__, (int)(size));\
                CERROR("LNET: %d total bytes allocated by lnet\n",        \
                       cfs_atomic_read(&libcfs_kmemory));                 \
                break;                                                    \
        }                                                                 \
        libcfs_kmem_inc((ptr), (size));                                   \
        memset((ptr), 0, (size));                                         \
        CDEBUG(D_MALLOC, "kmalloced '" #ptr "': %d at %p (tot %d).\n",    \
               (int)(size), (ptr), cfs_atomic_read (&libcfs_kmemory));    \
} while (0)

#define LIBCFS_ALLOC(ptr, size) \
        LIBCFS_ALLOC_GFP(ptr, size, CFS_ALLOC_IO)

#define LIBCFS_ALLOC_ATOMIC(ptr, size) \
        LIBCFS_ALLOC_GFP(ptr, size, CFS_ALLOC_ATOMIC)

#define LIBCFS_FREE(ptr, size)                                          \
do {                                                                    \
        int s = (size);                                                 \
        if (unlikely((ptr) == NULL)) {                                  \
                CERROR("LIBCFS: free NULL '" #ptr "' (%d bytes) at "    \
                       "%s:%d\n", s, __FILE__, __LINE__);               \
                break;                                                  \
        }                                                               \
        libcfs_kmem_dec((ptr), s);                                      \
        CDEBUG(D_MALLOC, "kfreed '" #ptr "': %d at %p (tot %d).\n",     \
               s, (ptr), cfs_atomic_read(&libcfs_kmemory));             \
        if (unlikely(s > LIBCFS_VMALLOC_SIZE))                          \
                cfs_free_large(ptr);                                    \
        else                                                            \
                cfs_free(ptr);                                          \
} while (0)

/******************************************************************************/

/* htonl hack - either this, or compile with -O2. Stupid byteorder/generic.h */
#if defined(__GNUC__) && (__GNUC__ >= 2) && !defined(__OPTIMIZE__)
#define ___htonl(x) __cpu_to_be32(x)
#define ___htons(x) __cpu_to_be16(x)
#define ___ntohl(x) __be32_to_cpu(x)
#define ___ntohs(x) __be16_to_cpu(x)
#define htonl(x) ___htonl(x)
#define ntohl(x) ___ntohl(x)
#define htons(x) ___htons(x)
#define ntohs(x) ___ntohs(x)
#endif

void libcfs_debug_dumpstack(cfs_task_t *tsk);
void libcfs_run_upcall(char **argv);
void libcfs_run_lbug_upcall(struct libcfs_debug_msg_data *);
void libcfs_debug_dumplog(void);
int libcfs_debug_init(unsigned long bufsize);
int libcfs_debug_cleanup(void);
int libcfs_debug_clear_buffer(void);
int libcfs_debug_mark_buffer(const char *text);

void libcfs_debug_set_level(unsigned int debug_level);

#else  /* !__KERNEL__ */
# ifdef LIBCFS_DEBUG
#  undef NDEBUG
#  include <assert.h>
#  define LASSERT(e)     assert(e)
#  define LASSERTF(cond, ...)                                                  \
do {                                                                           \
          if (!(cond))                                                         \
                CERROR(__VA_ARGS__);                                           \
          assert(cond);                                                        \
} while (0)
#  define LBUG()   assert(0)
#  ifdef INVARIANT_CHECK
#   define LINVRNT(exp) LASSERT(exp)
#  else
#   define LINVRNT(exp) ((void)sizeof!!(exp))
#  endif
# else
#  define LASSERT(e) ((void)sizeof!!(e))
#  define LASSERTF(cond, ...) ((void)sizeof!!(cond))
#  define LBUG()   ((void)(0))
#  define LINVRNT(exp) ((void)sizeof!!(exp))
# endif /* LIBCFS_DEBUG */
# define KLASSERT(e) ((void)0)
# define printk printf
# ifdef CRAY_XT3                                /* buggy calloc! */
#  define LIBCFS_ALLOC(ptr, size)               \
   do {                                         \
        (ptr) = malloc(size);                   \
        memset(ptr, 0, size);                   \
   } while (0)
# else
#  define LIBCFS_ALLOC(ptr, size) do { (ptr) = calloc(1,size); } while (0)
# endif
# define LIBCFS_FREE(a, b) do { free(a); } while (0)

void libcfs_debug_dumplog(void);
int libcfs_debug_init(unsigned long bufsize);
int libcfs_debug_cleanup(void);

#define libcfs_debug_dumpstack(tsk)     ((void)0)

/*
 * Generic compiler-dependent macros required for kernel
 * build go below this comment. Actual compiler/compiler version
 * specific implementations come from the above header files
 */
#ifdef __GNUC__
#define likely(x)      __builtin_expect(!!(x), 1)
#define unlikely(x)    __builtin_expect(!!(x), 0)
#else
#define likely(x)	(!!(x))
#define unlikely(x)	(!!(x))
#endif
/* !__KERNEL__ */
#endif

#define LASSERT_ATOMIC_ENABLED          (1)

#if LASSERT_ATOMIC_ENABLED

/** assert value of @a is equal to @v */
#define LASSERT_ATOMIC_EQ(a, v)                                 \
do {                                                            \
        LASSERTF(cfs_atomic_read(a) == v,                       \
                 "value: %d\n", cfs_atomic_read((a)));          \
} while (0)

/** assert value of @a is unequal to @v */
#define LASSERT_ATOMIC_NE(a, v)                                 \
do {                                                            \
        LASSERTF(cfs_atomic_read(a) != v,                       \
                 "value: %d\n", cfs_atomic_read((a)));          \
} while (0)

/** assert value of @a is little than @v */
#define LASSERT_ATOMIC_LT(a, v)                                 \
do {                                                            \
        LASSERTF(cfs_atomic_read(a) < v,                        \
                 "value: %d\n", cfs_atomic_read((a)));          \
} while (0)

/** assert value of @a is little/equal to @v */
#define LASSERT_ATOMIC_LE(a, v)                                 \
do {                                                            \
        LASSERTF(cfs_atomic_read(a) <= v,                       \
                 "value: %d\n", cfs_atomic_read((a)));          \
} while (0)

/** assert value of @a is great than @v */
#define LASSERT_ATOMIC_GT(a, v)                                 \
do {                                                            \
        LASSERTF(cfs_atomic_read(a) > v,                        \
                 "value: %d\n", cfs_atomic_read((a)));          \
} while (0)

/** assert value of @a is great/equal to @v */
#define LASSERT_ATOMIC_GE(a, v)                                 \
do {                                                            \
        LASSERTF(cfs_atomic_read(a) >= v,                       \
                 "value: %d\n", cfs_atomic_read((a)));          \
} while (0)

/** assert value of @a is great than @v1 and little than @v2 */
#define LASSERT_ATOMIC_GT_LT(a, v1, v2)                         \
do {                                                            \
        int __v = cfs_atomic_read(a);                           \
        LASSERTF(__v > v1 && __v < v2, "value: %d\n", __v);     \
} while (0)

/** assert value of @a is great than @v1 and little/equal to @v2 */
#define LASSERT_ATOMIC_GT_LE(a, v1, v2)                         \
do {                                                            \
        int __v = cfs_atomic_read(a);                           \
        LASSERTF(__v > v1 && __v <= v2, "value: %d\n", __v);    \
} while (0)

/** assert value of @a is great/equal to @v1 and little than @v2 */
#define LASSERT_ATOMIC_GE_LT(a, v1, v2)                         \
do {                                                            \
        int __v = cfs_atomic_read(a);                           \
        LASSERTF(__v >= v1 && __v < v2, "value: %d\n", __v);    \
} while (0)

/** assert value of @a is great/equal to @v1 and little/equal to @v2 */
#define LASSERT_ATOMIC_GE_LE(a, v1, v2)                         \
do {                                                            \
        int __v = cfs_atomic_read(a);                           \
        LASSERTF(__v >= v1 && __v <= v2, "value: %d\n", __v);   \
} while (0)

#else /* !LASSERT_ATOMIC_ENABLED */

#define LASSERT_ATOMIC_EQ(a, v)                 do {} while (0)
#define LASSERT_ATOMIC_NE(a, v)                 do {} while (0)
#define LASSERT_ATOMIC_LT(a, v)                 do {} while (0)
#define LASSERT_ATOMIC_LE(a, v)                 do {} while (0)
#define LASSERT_ATOMIC_GT(a, v)                 do {} while (0)
#define LASSERT_ATOMIC_GE(a, v)                 do {} while (0)
#define LASSERT_ATOMIC_GT_LT(a, v1, v2)         do {} while (0)
#define LASSERT_ATOMIC_GT_LE(a, v1, v2)         do {} while (0)
#define LASSERT_ATOMIC_GE_LT(a, v1, v2)         do {} while (0)
#define LASSERT_ATOMIC_GE_LE(a, v1, v2)         do {} while (0)

#endif /* LASSERT_ATOMIC_ENABLED */

#define LASSERT_ATOMIC_ZERO(a)                  LASSERT_ATOMIC_EQ(a, 0)
#define LASSERT_ATOMIC_POS(a)                   LASSERT_ATOMIC_GT(a, 0)

#define CFS_ALLOC_PTR(ptr)      LIBCFS_ALLOC(ptr, sizeof (*(ptr)));
#define CFS_FREE_PTR(ptr)       LIBCFS_FREE(ptr, sizeof (*(ptr)));

/** Compile-time assertion.

 * Check an invariant described by a constant expression at compile time by
 * forcing a compiler error if it does not hold.  \a cond must be a constant
 * expression as defined by the ISO C Standard:
 *
 *       6.8.4.2  The switch statement
 *       ....
 *       [#3] The expression of each case label shall be  an  integer
 *       constant   expression  and  no  two  of  the  case  constant
 *       expressions in the same switch statement shall have the same
 *       value  after  conversion...
 *
 */
#define CLASSERT(cond) do {switch(42) {case (cond): case 0: break;}} while (0)

/* support decl needed both by kernel and liblustre */
int         libcfs_isknown_lnd(int type);
char       *libcfs_lnd2modname(int type);
char       *libcfs_lnd2str(int type);
int         libcfs_str2lnd(const char *str);
char       *libcfs_net2str(__u32 net);
char       *libcfs_nid2str(lnet_nid_t nid);
__u32       libcfs_str2net(const char *str);
lnet_nid_t  libcfs_str2nid(const char *str);
int         libcfs_str2anynid(lnet_nid_t *nid, const char *str);
char       *libcfs_id2str(lnet_process_id_t id);
int         cfs_iswhite(char c);
void        cfs_free_nidlist(cfs_list_t *list);
int         cfs_parse_nidlist(char *str, int len, cfs_list_t *list);
int         cfs_match_nid(lnet_nid_t nid, cfs_list_t *list);

/** \addtogroup lnet_addr
 * @{ */
/* how an LNET NID encodes net:address */
/** extract the address part of an lnet_nid_t */
#define LNET_NIDADDR(nid)      ((__u32)((nid) & 0xffffffff))
/** extract the network part of an lnet_nid_t */
#define LNET_NIDNET(nid)       ((__u32)(((nid) >> 32)) & 0xffffffff)
/** make an lnet_nid_t from a network part and an address part */
#define LNET_MKNID(net,addr)   ((((__u64)(net))<<32)|((__u64)(addr)))
/* how net encodes type:number */
#define LNET_NETNUM(net)       ((net) & 0xffff)
#define LNET_NETTYP(net)       (((net) >> 16) & 0xffff)
#define LNET_MKNET(typ,num)    ((((__u32)(typ))<<16)|((__u32)(num)))
/** @} lnet_addr */

/* max value for numeric network address */
#define MAX_NUMERIC_VALUE 0xffffffff

/* implication */
#define ergo(a, b) (!(a) || (b))
/* logical equivalence */
#define equi(a, b) (!!(a) == !!(b))

#ifndef CFS_CURRENT_TIME
# define CFS_CURRENT_TIME time(0)
#endif

/* --------------------------------------------------------------------
 * Light-weight trace
 * Support for temporary event tracing with minimal Heisenberg effect.
 * All stuff about lwt are put in arch/kp30.h
 * -------------------------------------------------------------------- */

struct libcfs_device_userstate
{
        int           ldu_memhog_pages;
        cfs_page_t   *ldu_memhog_root_page;
};

/* what used to be in portals_lib.h */
#ifndef MIN
# define MIN(a,b) (((a)<(b)) ? (a): (b))
#endif
#ifndef MAX
# define MAX(a,b) (((a)>(b)) ? (a): (b))
#endif

#define MKSTR(ptr) ((ptr))? (ptr) : ""

static inline int cfs_size_round4 (int val)
{
        return (val + 3) & (~0x3);
}

#ifndef HAVE_CFS_SIZE_ROUND
static inline int cfs_size_round (int val)
{
        return (val + 7) & (~0x7);
}
#define HAVE_CFS_SIZE_ROUND
#endif

static inline int cfs_size_round16(int val)
{
        return (val + 0xf) & (~0xf);
}

static inline int cfs_size_round32(int val)
{
        return (val + 0x1f) & (~0x1f);
}

static inline int cfs_size_round0(int val)
{
        if (!val)
                return 0;
        return (val + 1 + 7) & (~0x7);
}

static inline size_t cfs_round_strlen(char *fset)
{
        return (size_t)cfs_size_round((int)strlen(fset) + 1);
}

#define LOGL(var,len,ptr)                                       \
do {                                                            \
        if (var)                                                \
                memcpy((char *)ptr, (const char *)var, len);    \
        ptr += cfs_size_round(len);                             \
} while (0)

#define LOGU(var,len,ptr)                                       \
do {                                                            \
        if (var)                                                \
                memcpy((char *)var, (const char *)ptr, len);    \
        ptr += cfs_size_round(len);                             \
} while (0)

#define LOGL0(var,len,ptr)                              \
do {                                                    \
        if (!len)                                       \
                break;                                  \
        memcpy((char *)ptr, (const char *)var, len);    \
        *((char *)(ptr) + len) = 0;                     \
        ptr += cfs_size_round(len + 1);                 \
} while (0)

/**
 *  Lustre Network Driver types.
 */
enum {
        /* Only add to these values (i.e. don't ever change or redefine them):
         * network addresses depend on them... */
        QSWLND    = 1,
        SOCKLND   = 2,
        GMLND     = 3, /* obsolete, keep it so that libcfs_nid2str works */
        PTLLND    = 4,
        O2IBLND   = 5,
        CIBLND    = 6,
        OPENIBLND = 7,
        IIBLND    = 8,
        LOLND     = 9,
        RALND     = 10,
        VIBLND    = 11,
        MXLND     = 12,
        GNILND    = 13,
};

#endif
