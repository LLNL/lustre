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
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lnet/lnet/lib-me.c
 *
 * Match Entry management routines
 */

#define DEBUG_SUBSYSTEM S_LNET

#include <lnet/lib-lnet.h>

static int
lnet_me_match_portal(lnet_portal_t *ptl, lnet_process_id_t id,
                     __u64 match_bits, __u64 ignore_bits)
{
        struct list_head *mhash;
        int               rdma;

        LASSERT (!lnet_portal_is_rdma(ptl) ||
                 !lnet_portal_is_request(ptl));

        /* prefer to check w/o any lock */
        rdma = lnet_match_is_unique(id, match_bits, ignore_bits);
        if (likely(lnet_portal_is_rdma(ptl) ||
                   lnet_portal_is_request(ptl)))
                goto match;

        /* unset, new portal */
        if (rdma) {
                mhash = lnet_portal_mhash_alloc();
                if (mhash == NULL)
                        return -ENOMEM;
        } else {
                mhash = NULL;
        }

        LNET_LOCK();
        if (lnet_portal_is_rdma(ptl) ||
            lnet_portal_is_request(ptl)) {
                /* someone set it before me */
                if (mhash != NULL)
                        lnet_portal_mhash_free(mhash);
                LNET_UNLOCK();
                goto match;
        }

        /* still not set */
        LASSERT (ptl->ptl_mhash == NULL);
        if (rdma) {
                ptl->ptl_mhash = mhash;
                lnet_portal_set(ptl, LNET_PTL_RDMA);
        } else {
                lnet_portal_set(ptl, LNET_PTL_REQUEST);
        }
        LNET_UNLOCK();
        return 0;

 match:
        if (lnet_portal_is_rdma(ptl) && !rdma)
                return -EPERM;

        if (lnet_portal_is_request(ptl) && rdma)
                return -EPERM;

        return 0;
}

int
LNetMEAttach(unsigned int portal,
             lnet_process_id_t match_id,
             __u64 match_bits, __u64 ignore_bits,
             lnet_unlink_t unlink, lnet_ins_pos_t pos,
             lnet_handle_me_t *handle)
{
        lnet_me_t        *me;
        lnet_portal_t    *ptl;
        struct list_head *head;
        int               rc;

        LASSERT (the_lnet.ln_init);
        LASSERT (the_lnet.ln_refcount > 0);

        if (portal >= the_lnet.ln_nportals)
                return -EINVAL;

        ptl = &the_lnet.ln_portals[portal];
        rc = lnet_me_match_portal(ptl, match_id, match_bits, ignore_bits);
        if (rc != 0)
                return rc;

        me = lnet_me_alloc();
        if (me == NULL)
                return -ENOMEM;

        LNET_LOCK();

        me->me_portal = portal;
        me->me_match_id = match_id;
        me->me_match_bits = match_bits;
        me->me_ignore_bits = ignore_bits;
        me->me_unlink = unlink;
        me->me_md = NULL;

        lnet_initialise_handle (&me->me_lh, LNET_COOKIE_TYPE_ME);

        head = lnet_portal_me_head(portal, match_id, match_bits);
        LASSERT (head != NULL);

        if (pos == LNET_INS_AFTER)
                list_add_tail(&me->me_list, head);
        else
                list_add(&me->me_list, head);

        lnet_me2handle(handle, me);

        LNET_UNLOCK();

        return 0;
}

int
LNetMEInsert(lnet_handle_me_t current_meh,
             lnet_process_id_t match_id,
             __u64 match_bits, __u64 ignore_bits,
             lnet_unlink_t unlink, lnet_ins_pos_t pos,
             lnet_handle_me_t *handle)
{
        lnet_me_t     *current_me;
        lnet_me_t     *new_me;
        lnet_portal_t *ptl;

        LASSERT (the_lnet.ln_init);
        LASSERT (the_lnet.ln_refcount > 0);

        new_me = lnet_me_alloc();
        if (new_me == NULL)
                return -ENOMEM;

        LNET_LOCK();

        current_me = lnet_handle2me(&current_meh);
        if (current_me == NULL) {
                lnet_me_free (new_me);

                LNET_UNLOCK();
                return -ENOENT;
        }

        LASSERT (current_me->me_portal >= 0 &&
                 current_me->me_portal < the_lnet.ln_nportals);

        ptl = &the_lnet.ln_portals[current_me->me_portal];
        if (lnet_portal_is_rdma(ptl)) {
                /* nosense to insertion on rdma portal */
                lnet_me_free (new_me);
                LNET_UNLOCK();
                return -EPERM;
        }

        new_me->me_portal = current_me->me_portal;
        new_me->me_match_id = match_id;
        new_me->me_match_bits = match_bits;
        new_me->me_ignore_bits = ignore_bits;
        new_me->me_unlink = unlink;
        new_me->me_md = NULL;

        lnet_initialise_handle (&new_me->me_lh, LNET_COOKIE_TYPE_ME);

        if (pos == LNET_INS_AFTER)
                list_add(&new_me->me_list, &current_me->me_list);
        else
                list_add_tail(&new_me->me_list, &current_me->me_list);

        lnet_me2handle(handle, new_me);

        LNET_UNLOCK();

        return 0;
}

int
LNetMEUnlink(lnet_handle_me_t meh)
{
        lnet_me_t    *me;
        lnet_libmd_t *md;
        lnet_event_t  ev;

        LASSERT (the_lnet.ln_init);
        LASSERT (the_lnet.ln_refcount > 0);

        LNET_LOCK();

        me = lnet_handle2me(&meh);
        if (me == NULL) {
                LNET_UNLOCK();
                return -ENOENT;
        }

        md = me->me_md;
        if (md != NULL &&
            md->md_eq != NULL &&
            md->md_refcount == 0) {
                lnet_build_unlink_event(md, &ev);
                lnet_enq_event_locked(md->md_eq, &ev);
        }

        lnet_me_unlink(me);

        LNET_UNLOCK();
        return 0;
}

/* call with LNET_LOCK please */
void
lnet_me_unlink(lnet_me_t *me)
{
        list_del (&me->me_list);

        if (me->me_md != NULL) {
                me->me_md->md_me = NULL;
                lnet_md_unlink(me->me_md);
        }

        lnet_invalidate_handle (&me->me_lh);
        lnet_me_free(me);
}

#if 0
static void
lib_me_dump(lnet_me_t *me)
{
        CWARN("Match Entry %p ("LPX64")\n", me,
              me->me_lh.lh_cookie);

        CWARN("\tMatch/Ignore\t= %016lx / %016lx\n",
              me->me_match_bits, me->me_ignore_bits);

        CWARN("\tMD\t= %p\n", me->md);
        CWARN("\tprev\t= %p\n",
              list_entry(me->me_list.prev, lnet_me_t, me_list));
        CWARN("\tnext\t= %p\n",
              list_entry(me->me_list.next, lnet_me_t, me_list));
}
#endif
