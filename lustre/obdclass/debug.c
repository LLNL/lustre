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
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdclass/debug.c
 *
 * Helper routines for dumping data structs for debugging.
 */

#define DEBUG_SUBSYSTEM D_OTHER

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <obd_ost.h>
#include <obd_support.h>
#include <lustre_debug.h>
#include <lustre_net.h>

void dump_lniobuf(struct niobuf_local *nb)
{
        CDEBUG(D_RPCTRACE,
               "niobuf_local: offset="LPD64", len=%d, page=%p, rc=%d\n",
               nb->lnb_file_offset, nb->lnb_len, nb->lnb_page, nb->lnb_rc);
        CDEBUG(D_RPCTRACE, "nb->lnb_page: index = %ld\n",
               nb->lnb_page ? cfs_page_index(nb->lnb_page) : -1);
}
EXPORT_SYMBOL(dump_lniobuf);

void dump_lsm(int level, struct lov_stripe_md *lsm)
{
        CDEBUG(level, "lsm %p, objid "LPX64", maxbytes "LPX64", magic 0x%08X, "
               "stripe_size %u, stripe_count %u, "
               "layout_gen %u, pool ["LOV_POOLNAMEF"]\n", lsm,
               lsm->lsm_object_id, lsm->lsm_maxbytes, lsm->lsm_magic,
               lsm->lsm_stripe_size, lsm->lsm_stripe_count,
               lsm->lsm_layout_gen, lsm->lsm_pool_name);
}

#define LPDS sizeof(__u64)
int block_debug_setup(void *addr, int len, __u64 off, __u64 id)
{
        LASSERT(addr);

        off = cpu_to_le64 (off);
        id = cpu_to_le64 (id);
        memcpy(addr, (char *)&off, LPDS);
        memcpy(addr + LPDS, (char *)&id, LPDS);

        addr += len - LPDS - LPDS;
        memcpy(addr, (char *)&off, LPDS);
        memcpy(addr + LPDS, (char *)&id, LPDS);

        return 0;
}

int block_debug_check(char *who, void *addr, int end, __u64 off, __u64 id)
{
        __u64 ne_off;
        int err = 0;

        LASSERT(addr);

        ne_off = le64_to_cpu (off);
        id = le64_to_cpu (id);
        if (memcmp(addr, (char *)&ne_off, LPDS)) {
                CDEBUG(D_ERROR, "%s: id "LPX64" offset "LPU64" off: "LPX64" != "
                       LPX64"\n", who, id, off, *(__u64 *)addr, ne_off);
                err = -EINVAL;
        }
        if (memcmp(addr + LPDS, (char *)&id, LPDS)) {
                CDEBUG(D_ERROR, "%s: id "LPX64" offset "LPU64" id: "LPX64" != "LPX64"\n",
                       who, id, off, *(__u64 *)(addr + LPDS), id);
                err = -EINVAL;
        }

        addr += end - LPDS - LPDS;
        if (memcmp(addr, (char *)&ne_off, LPDS)) {
                CDEBUG(D_ERROR, "%s: id "LPX64" offset "LPU64" end off: "LPX64" != "
                       LPX64"\n", who, id, off, *(__u64 *)addr, ne_off);
                err = -EINVAL;
        }
        if (memcmp(addr + LPDS, (char *)&id, LPDS)) {
                CDEBUG(D_ERROR, "%s: id "LPX64" offset "LPU64" end id: "LPX64" != "
                       LPX64"\n", who, id, off, *(__u64 *)(addr + LPDS), id);
                err = -EINVAL;
        }

        return err;
}
#undef LPDS

//EXPORT_SYMBOL(dump_req);
EXPORT_SYMBOL(dump_lsm);
EXPORT_SYMBOL(block_debug_setup);
EXPORT_SYMBOL(block_debug_check);
