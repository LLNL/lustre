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
 * Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011 Whamcloud, Inc.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/ofd/ofd_grant.c
 *
 * Author: Atul Vidwansa <atul.vidwansa@sun.com>
 * Author: Mike Pershin <tappro@sun.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include "ofd_internal.h"

#define OFD_GRANT_CHUNK (2ULL * PTLRPC_MAX_BRW_SIZE)
#define OFD_GRANT_SHRINK_LIMIT (16ULL * OFD_GRANT_CHUNK)

/**
 * Perform extra sanity checks for grant accounting. This is done at connect,
 * disconnect, and statfs RPC time, so it shouldn't be too bad. We can
 * always get rid of it or turn it off when we know accounting is good.
 *
 * \param obd - is the device to check
 * \param func - is the function to call if an inconsistency is found
 */
void ofd_grant_sanity_check(struct obd_device *obd, const char *func)
{
        struct filter_export_data *fed;
        struct ofd_device         *ofd = ofd_dev(obd->obd_lu_dev);
        struct obd_export         *exp;
        obd_size                   maxsize;
        obd_size                   tot_dirty = 0;
        obd_size                   tot_pending = 0;
        obd_size                   tot_granted = 0;
        obd_size                   fo_tot_dirty, fo_tot_pending, fo_tot_granted;

        if (cfs_list_empty(&obd->obd_exports))
                return;

        /* We don't want to do this for large machines that do lots of
         * mounts or unmounts.  It burns... */
        if (obd->obd_num_exports > 100)
                return;

        maxsize = ofd->ofd_osfs.os_blocks << ofd->ofd_blockbits;

        cfs_spin_lock(&obd->obd_dev_lock);
        cfs_spin_lock(&ofd->ofd_grant_lock);
        cfs_list_for_each_entry(exp, &obd->obd_exports, exp_obd_chain) {
                int error = 0;
                fed = &exp->exp_filter_data;
                if (fed->fed_grant < 0 || fed->fed_pending < 0 ||
                    fed->fed_dirty < 0)
                        error = 1;
                if (fed->fed_grant + fed->fed_pending > maxsize) {
                        CERROR("%s: cli %s/%p fed_grant(%ld) + fed_pending(%ld)"
                               " > maxsise("LPU64")\n", obd->obd_name,
                               exp->exp_client_uuid.uuid, exp, fed->fed_grant,
                               fed->fed_pending, maxsize);
                        cfs_spin_unlock(&obd->obd_dev_lock);
                        cfs_spin_unlock(&ofd->ofd_grant_lock);
                        LBUG();
                }
                if (fed->fed_dirty > maxsize) {
                        CERROR("%s: cli %s/%p fed_dirty(%ld) > maxsize("LPU64
                               ")\n", obd->obd_name, exp->exp_client_uuid.uuid,
                               exp, fed->fed_dirty, maxsize);
                        cfs_spin_unlock(&obd->obd_dev_lock);
                        cfs_spin_unlock(&ofd->ofd_grant_lock);
                        LBUG();
                }
                if (error)
                        CERROR("%s: cli %s/%p dirty %ld pend %ld grant %ld\n",
                               obd->obd_name, exp->exp_client_uuid.uuid, exp,
                               fed->fed_dirty, fed->fed_pending,fed->fed_grant);
                else
                        CDEBUG(D_CACHE, "%s: cli %s/%p dirty %ld pend %ld grant %ld\n",
                               obd->obd_name, exp->exp_client_uuid.uuid, exp,
                               fed->fed_dirty, fed->fed_pending,fed->fed_grant);
                tot_granted += fed->fed_grant + fed->fed_pending;
                tot_pending += fed->fed_pending;
                tot_dirty += fed->fed_dirty;
        }
        cfs_spin_unlock(&obd->obd_dev_lock);
        fo_tot_granted = ofd->ofd_tot_granted;
        fo_tot_pending = ofd->ofd_tot_pending;
        fo_tot_dirty = ofd->ofd_tot_dirty;
        cfs_spin_unlock(&ofd->ofd_grant_lock);

        /* Do these assertions outside the spinlocks so we don't kill system */
        if (tot_granted != fo_tot_granted)
                CERROR("%s: tot_granted "LPU64" != fo_tot_granted "LPU64"\n",
                       func, tot_granted, fo_tot_granted);
        if (tot_pending != fo_tot_pending)
                CERROR("%s: tot_pending "LPU64" != fo_tot_pending "LPU64"\n",
                       func, tot_pending, fo_tot_pending);
        if (tot_dirty != fo_tot_dirty)
                CERROR("%s: tot_dirty "LPU64" != fo_tot_dirty "LPU64"\n",
                       func, tot_dirty, fo_tot_dirty);
        if (tot_pending > tot_granted)
                CERROR("%s: tot_pending "LPU64" > tot_granted "LPU64"\n",
                       func, tot_pending, tot_granted);
        if (tot_granted > maxsize)
                CERROR("%s: tot_granted "LPU64" > maxsize "LPU64"\n",
                       func, tot_granted, maxsize);
        if (tot_dirty > maxsize)
                CERROR("%s: tot_dirty "LPU64" > maxsize "LPU64"\n",
                       func, tot_dirty, maxsize);
}

/**
 * Get fresh statfs information from the OSD layer if the cache is older than 1s
 * or if force is set. The OSD layer is in charge of estimating data & metadata
 * overhead.
 *
 * \param env - is the lu environment passed by the caller
 * \param exp - export used to print client info in debug messages
 * \param force - is used to force a refresh of statfs information
 * \param from_cache - returns whether the statfs information are
 *                     taken from cache
 */
static void ofd_grant_statfs(const struct lu_env *env, struct obd_export *exp,
                             int force, int *from_cache)
{
        struct obd_device *obd = exp->exp_obd;
        struct ofd_device *ofd = ofd_exp(exp);
        struct obd_statfs *osfs = &ofd_info(env)->fti_u.osfs;
        __u64              max_age;
        int                rc;

        if (force)
                max_age = 0; /* get fresh statfs data */
        else
                max_age = cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS);

        rc = ofd_statfs_internal(env, ofd, osfs, max_age, from_cache);
        if (unlikely(rc)) {
                *from_cache = 0;
                return;
        }

        CDEBUG(D_CACHE, "%s: cli %s/%p free: "LPU64" avail: "LPU64"\n",
               obd->obd_name, exp->exp_client_uuid.uuid, exp,
               osfs->os_bfree << ofd->ofd_blockbits,
               osfs->os_bavail << ofd->ofd_blockbits);
}

/**
 * Figure out how much space is available on the backend filesystem.
 * This is done by accessing cached statfs data previously populated by
 * ofd_grant_statfs(), from which we withdraw the space already granted to
 * clients and the reserved space.
 *
 * \param exp - export which received the write request
 */
static obd_size ofd_grant_space_left(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        struct ofd_device *ofd = ofd_exp(exp);
        obd_size           tot_granted;
        obd_size           left, avail;
        obd_size           unstable;

        LASSERT_SPIN_LOCKED(&ofd->ofd_grant_lock);

        cfs_spin_lock(&ofd->ofd_osfs_lock);
        /* get available space from cached statfs data */
        left = ofd->ofd_osfs.os_bavail << ofd->ofd_blockbits;
        unstable = ofd->ofd_osfs_unstable; /* those might be accounted twice */
        cfs_spin_unlock(&ofd->ofd_osfs_lock);

        tot_granted = ofd->ofd_tot_granted;

        if (left < tot_granted) {
                if (left + unstable < tot_granted - ofd->ofd_tot_pending)
                        CERROR("%s: cli %s/%p grant "LPU64" > available "
                               LPU64" unstable "LPU64" and pending "LPU64"\n",
                               obd->obd_name, exp->exp_client_uuid.uuid,
                               exp, tot_granted, left, unstable,
                               ofd->ofd_tot_pending);
                else
                        CDEBUG(D_CACHE, "%s: cli %s/%p left "LPU64" < tot_grant"
                               " "LPU64" unstable "LPU64" pending "LPU64"\n",
                               obd->obd_name, exp->exp_client_uuid.uuid, exp,
                               left, tot_granted, unstable,
                               ofd->ofd_tot_pending);
                return 0;
        }

        avail = left;
        /* Withdraw space already granted to clients */
        left -= tot_granted;

        /* If the left space is below the grant threshold x available space,
         * stop granting space to clients.
         * The purpose of this threshold is to keep some error margin on the
         * overhead estimate made by the OSD layer. If we grant all the free
         * space, we have no way (grant space cannot be revoked yet) to
         * adjust if the write overhead has been underestimated. */
        left -= min_t(obd_size, left, ofd_grant_reserved(ofd, avail));

        /* Align left on block size */
        left &= ~((1ULL << ofd->ofd_blockbits) - 1);

        CDEBUG(D_CACHE, "%s: cli %s/%p avail "LPU64" left "LPU64" unstable "
               LPU64" tot_grant "LPU64" pending "LPU64"\n", obd->obd_name,
               exp->exp_client_uuid.uuid, exp, avail, left, unstable,
               tot_granted, ofd->ofd_tot_pending);

        return left;
}

/**
 * Grab the dirty and seen grant announcements from the incoming obdo.
 * We will later calculate the client's new grant and return it.
 * Caller must hold ofd_grant_lock spinlock.
 *
 * \param env - is the lu environment supplying osfs storage
 * \param exp - is the export for which we received the request
 * \paral oa - is the incoming obdo sent by the client
 */
static void ofd_grant_incoming(const struct lu_env *env, struct obd_export *exp,
                               struct obdo *oa)
{
        struct filter_export_data *fed;
        struct ofd_device         *ofd = ofd_exp(exp);
        struct obd_device         *obd = exp->exp_obd;
        ENTRY;

        LASSERT_SPIN_LOCKED(&ofd->ofd_grant_lock);

        if ((oa->o_valid & (OBD_MD_FLBLOCKS|OBD_MD_FLGRANT)) !=
                                        (OBD_MD_FLBLOCKS|OBD_MD_FLGRANT)) {
                oa->o_valid &= ~OBD_MD_FLGRANT;
                EXIT;
                return;
        }

        fed = &exp->exp_filter_data;

        /* Add some margin, since there is a small race if other RPCs arrive
         * out-or-order and have already consumed some grant.  We want to
         * leave this here in case there is a large error in accounting. */
        CDEBUG(D_CACHE,
               "%s: cli %s/%p reports grant: "LPU64" dropped: %u, local: %lu\n",
               obd->obd_name, exp->exp_client_uuid.uuid, exp, oa->o_grant,
               oa->o_dropped, fed->fed_grant);

        /* Update our accounting now so that statfs takes it into account.
         * Note that fed_dirty is only approximate and can become incorrect
         * if RPCs arrive out-of-order.  No important calculations depend
         * on fed_dirty however, but we must check sanity to not assert. */
        if ((long long)oa->o_dirty < 0)
                oa->o_dirty = 0;
        else if (oa->o_dirty > fed->fed_grant + 4 * OFD_GRANT_CHUNK)
                oa->o_dirty = fed->fed_grant + 4 * OFD_GRANT_CHUNK;
        ofd->ofd_tot_dirty += oa->o_dirty - fed->fed_dirty;
        if (fed->fed_grant < oa->o_dropped) {
                CDEBUG(D_CACHE,"%s: cli %s/%p reports %u dropped > grant %lu\n",
                       obd->obd_name, exp->exp_client_uuid.uuid, exp,
                       oa->o_dropped, fed->fed_grant);
                oa->o_dropped = 0;
        }
        if (ofd->ofd_tot_granted < oa->o_dropped) {
                CERROR("%s: cli %s/%p reports %u dropped > tot_grant "LPU64"\n",
                       obd->obd_name, exp->exp_client_uuid.uuid, exp,
                       oa->o_dropped, ofd->ofd_tot_granted);
                oa->o_dropped = 0;
        }
        ofd->ofd_tot_granted -= oa->o_dropped;
        fed->fed_grant -= oa->o_dropped;
        fed->fed_dirty = oa->o_dirty;

        if (fed->fed_dirty < 0 || fed->fed_grant < 0 || fed->fed_pending < 0) {
                CERROR("%s: cli %s/%p dirty %ld pend %ld grant %ld\n",
                       obd->obd_name, exp->exp_client_uuid.uuid, exp,
                       fed->fed_dirty, fed->fed_pending, fed->fed_grant);
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                LBUG();
        }
        EXIT;
}

/**
 * Called when the client is able to release some grants. Proceed with the
 * shrink request when there is less ungranted space remaining
 * than the amount all of the connected clients would consume if they
 * used their full grant.
 *
 * \param exp - is the export for which we received the request
 * \paral oa - is the incoming obdo sent by the client
 * \param left_space - is the remaining free space with space already granted
 *                     taken out
 */
static void ofd_grant_shrink(struct obd_export *exp,
                             struct obdo *oa, obd_size left_space)
{
        struct filter_export_data *fed;
        struct ofd_device         *ofd = ofd_exp(exp);
        struct obd_device         *obd = exp->exp_obd;

        LASSERT_SPIN_LOCKED(&ofd->ofd_grant_lock);

        if (left_space >= ofd->ofd_tot_granted_clients *
                          OFD_GRANT_SHRINK_LIMIT)
                return;

        fed = &exp->exp_filter_data;
        fed->fed_grant -= oa->o_grant;
        ofd->ofd_tot_granted -= oa->o_grant;

        CDEBUG(D_CACHE, "%s: cli %s/%p shrink "LPU64"fed_grant %ld total "
               LPU64"\n", obd->obd_name, exp->exp_client_uuid.uuid,
               exp, oa->o_grant, fed->fed_grant, ofd->ofd_tot_granted);

        /* client has just released some grant, don't grant any space back */
        oa->o_grant = 0;
}

/**
 * When clients have dirtied as much space as they've been granted they
 * fall through to sync writes.  These sync writes haven't been expressed
 * in grants and need to error with ENOSPC when there isn't room in the
 * filesystem for them after grants are taken into account.  However,
 * writeback of the dirty data that was already granted space can write
 * right on through.
 * Caller must hold ofd_grant_lock spinlock.
 *
 * \param env - is the lu environment passed by the caller
 * \param exp - is the export identifying the client which sent the RPC
 * \param oa  - is the incoming obdo in which we should return the pack the
 *              additional grant
 * \param lnb - is the list of local network iobuf
 * \param niocount - is the number of iobuf in the lnb list
 * \param left - is the remaining free space with space already granted
 *               taken out
 */
static int ofd_grant_check(const struct lu_env *env, struct obd_export *exp,
                           struct obdo *oa, struct niobuf_local *lnb,
                           int niocount, obd_size *left)
{
        struct filter_export_data *fed = &exp->exp_filter_data;
        struct obd_device         *obd = exp->exp_obd;
        struct ofd_device         *ofd = ofd_exp(exp);
        unsigned long              ungranted = 0;
        unsigned long              granted = 0;
        int                        i, rc = -ENOSPC;
        int                        resend = 0;
        int                        blocksize = ofd->ofd_osfs.os_bsize;
        struct ofd_thread_info    *info = ofd_info(env);

        if ((oa->o_valid & OBD_MD_FLFLAGS) &&
            (oa->o_flags & OBD_FL_RECOV_RESEND)) {
                resend = 1;
                CDEBUG(D_CACHE, "Recoverable resend arrived, skipping "
                                "accounting\n");
        }

        LASSERT_SPIN_LOCKED(&ofd->ofd_grant_lock);

        for (i = 0; i < niocount; i++) {
                int tmp, bytes;

                /* should match the code in osc_exit_cache */
                bytes = lnb[i].lnb_len;
                bytes += lnb[i].lnb_page_offset;
                tmp = (lnb[i].lnb_file_offset + lnb[i].lnb_len) & (blocksize - 1);
                if (tmp)
                        bytes += blocksize - tmp;

                if ((lnb[i].lnb_flags & OBD_BRW_FROM_GRANT) &&
                    (oa->o_valid & OBD_MD_FLGRANT)) {
                        if (resend) {
                                /* This is a recoverable resend so grant
                                 * information have already been processed */
                                lnb[i].lnb_flags |= OBD_BRW_GRANTED;
                                rc = 0;
                                continue;
                        } else if (fed->fed_grant < granted + bytes) {
                                CDEBUG(D_CACHE, "%s: cli %s/%p claims %ld+%d "
                                       "GRANT, real grant %lu idx %d\n",
                                       exp->exp_obd->obd_name,
                                       exp->exp_client_uuid.uuid, exp,
                                       granted, bytes, fed->fed_grant, i);
                        } else {
                                granted += bytes;
                                lnb[i].lnb_flags |= OBD_BRW_GRANTED;

                                rc = 0;
                                continue;
                        }
                }
                if (*left > ungranted + bytes) {
                        /* if enough space, pretend it was granted */
                        ungranted += bytes;
                        lnb[i].lnb_flags |= OBD_BRW_GRANTED;
                        CDEBUG(0, "idx %d ungranted=%lu\n", i, ungranted);
                        rc = 0;
                        continue;
                }

                /* We can't check for already-mapped blocks here (make sense
                 * when backend filesystem does not use COW) as it requires
                 * dropping the grant lock.
                 * Instead, we return ENOSPC and in that case we need
                 * to go through and verify if all of the blocks not
                 * marked BRW_GRANTED are already mapped and we can
                 * ignore this error. */
                lnb[i].lnb_rc = -ENOSPC;
                lnb[i].lnb_flags &= ~OBD_BRW_GRANTED;
                CDEBUG(D_CACHE,"%s: cli %s/%p idx %d no space for %d\n",
                                exp->exp_obd->obd_name,
                                exp->exp_client_uuid.uuid, exp, i, bytes);
        }

        /* record space used for the I/O, will be used in ofd_grant_commmit() */
        /* Now substract what the clients has used already.  We don't subtract
         * this from the tot_granted yet, so that other client's can't grab
         * that space before we have actually allocated our blocks. That
         * happens in ofd_grant_commit() after the writes are done. */
        info->fti_used = granted + ungranted;
        *left -= ungranted;
        fed->fed_grant -= granted;
        fed->fed_pending += info->fti_used;
        ofd->ofd_tot_granted += ungranted;
        ofd->ofd_tot_pending += info->fti_used;

        CDEBUG(D_CACHE,
               "%s: cli %s/%p granted: %lu ungranted: %lu grant: %lu dirty: %lu\n",
               obd->obd_name, exp->exp_client_uuid.uuid, exp, granted,
               ungranted, fed->fed_grant, fed->fed_dirty);

        if (fed->fed_dirty < granted) {
                CWARN("%s: cli %s/%p claims granted %lu > fed_dirty %lu\n",
                       obd->obd_name, exp->exp_client_uuid.uuid, exp,
                       granted, fed->fed_dirty);
                granted = fed->fed_dirty;
        }
        ofd->ofd_tot_dirty -= granted;
        fed->fed_dirty -= granted;

        if (fed->fed_dirty < 0 || fed->fed_grant < 0 || fed->fed_pending < 0) {
                CERROR("%s: cli %s/%p dirty %ld pend %ld grant %ld\n",
                       obd->obd_name, exp->exp_client_uuid.uuid, exp,
                       fed->fed_dirty, fed->fed_pending, fed->fed_grant);
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                LBUG();
        }

        return rc;
}

/**
 * Calculate how much grant space to return to client, based on how much space
 * is currently free and how much of that is already granted.
 * Caller must hold ofd_grant_lock spinlock.
 *
 * \param exp - is the export of the client which sent the request
 * \param curgrant - is the current grant claimed by the client
 * \param want - is how much grant space the client would like to have
 * \param left - is the remaining free space with granted space taken out
 */
static long ofd_grant(struct obd_export *exp, obd_size curgrant,
                      obd_size want, obd_size left)
{
        struct obd_device         *obd = exp->exp_obd;
        struct ofd_device         *ofd = ofd_exp(exp);
        struct filter_export_data *fed = &exp->exp_filter_data;
        __u64                      grant;

        if (want > 0x7fffffff) {
                CERROR("%s: client %s/%p requesting > 2GB grant "LPU64"\n",
                       obd->obd_name, exp->exp_client_uuid.uuid, exp, want);
                return 0;
        }

        if (left == 0)
                return 0;

        /* Grant some fraction of the client's requested grant space so that
         * they are not always waiting for write credits (not all of it to
         * avoid overgranting in face of multiple RPCs in flight).  This
         * essentially will be able to control the OSC_MAX_RIF for a client.
         *
         * If we do have a large disparity between what the client thinks it
         * has and what we think it has, don't grant very much and let the
         * client consume its grant first.  Either it just has lots of RPCs
         * in flight, or it was evicted and its grants will soon be used up. */
        if (curgrant >= want || curgrant >= fed->fed_grant + OFD_GRANT_CHUNK)
                   return 0;

        grant = min(want, left >> 3);
        /* align grant on block size */
        grant &= ~((1ULL << ofd->ofd_blockbits) - 1);

        if (!grant)
                return 0;

        /* Allow >OFD_GRANT_CHUNK size when clients reconnect due to a
         * server reboot. */
        if ((grant > OFD_GRANT_CHUNK) && (!obd->obd_recovering))
                grant = OFD_GRANT_CHUNK;

        ofd->ofd_tot_granted += grant;
        fed->fed_grant += grant;

        if (fed->fed_grant < 0) {
                CERROR("%s: cli %s/%p grant %ld want "LPU64"current"LPU64"\n",
                       obd->obd_name, exp->exp_client_uuid.uuid, exp,
                       fed->fed_grant, want, curgrant);
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                LBUG();
        }

        CDEBUG(D_CACHE,
               "%s: cli %s/%p wants: "LPU64" current grant "LPU64
               " granting: "LPU64"\n", obd->obd_name, exp->exp_client_uuid.uuid,
               exp, want, curgrant, grant);
        CDEBUG(D_CACHE,
               "%s: cli %s/%p tot cached:"LPU64" granted:"LPU64
               " num_exports: %d\n", obd->obd_name, exp->exp_client_uuid.uuid,
               exp, ofd->ofd_tot_dirty, ofd->ofd_tot_granted,
               obd->obd_num_exports);

        return grant;
}

/**
 * Client connection or reconnection.
 *
 * \param env - is the lu environment provided by the caller
 * \param exp - is the client's export which is reconnecting
 * \param want - is how much the client would like to get
 */
long ofd_grant_connect(const struct lu_env *env, struct obd_export *exp,
                       obd_size want)
{
        struct ofd_device         *ofd = ofd_exp(exp);
        struct filter_export_data *fed = &exp->exp_filter_data;
        obd_size                   left = 0;
        long                       grant;
        int                        from_cache;
        int                        force = 0; /* can use cached statfs data */

refresh:
        ofd_grant_statfs(env, exp, force, &from_cache);

        cfs_spin_lock(&ofd->ofd_grant_lock);

        /* Grab free space from cached info and take out space already granted
         * to clients as well as reserved space */
        left = ofd_grant_space_left(exp);

        /* get fresh statfs data if we are short in ungranted space */
        if (from_cache && left < 32 * OFD_GRANT_CHUNK) {
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                CDEBUG(D_CACHE, "fs has no space left and statfs too old\n");
                force = 1;
                goto refresh;
        }

        ofd_grant(exp, fed->fed_grant, want, left);

        /* return to client its current grant */
        grant = fed->fed_grant;
        ofd->ofd_tot_granted_clients++;

        cfs_spin_unlock(&ofd->ofd_grant_lock);

        CDEBUG(D_CACHE, "%s: cli %s/%p ocd_grant: %ld want: "LPU64" left: "
               LPU64"\n", exp->exp_obd->obd_name, exp->exp_client_uuid.uuid,
               exp, grant, want, left);

        return grant;
}

/**
 * Remove a client from the grant accounting totals.  We also remove
 * the export from the obd device under the osfs and dev locks to ensure
 * that the ofd_grant_sanity_check() calculations are always valid.
 * The client should do something similar when it invalidates its import.
 *
 * \param exp - is the client's export to remove from grant accounting
 */
void ofd_grant_discard(struct obd_export *exp)
{
        struct obd_device         *obd = exp->exp_obd;
        struct ofd_device         *ofd = ofd_exp(exp);
        struct filter_export_data *fed = &exp->exp_filter_data;

        cfs_spin_lock(&ofd->ofd_grant_lock);
        LASSERTF(ofd->ofd_tot_granted >= fed->fed_grant,
                 "%s: tot_granted "LPU64" cli %s/%p fed_grant %ld\n",
                 obd->obd_name, ofd->ofd_tot_granted,
                 exp->exp_client_uuid.uuid, exp, fed->fed_grant);
        ofd->ofd_tot_granted -= fed->fed_grant;
        fed->fed_grant = 0;
        LASSERTF(ofd->ofd_tot_pending >= fed->fed_pending,
                 "%s: tot_pending "LPU64" cli %s/%p fed_pending %ld\n",
                 obd->obd_name, ofd->ofd_tot_pending,
                 exp->exp_client_uuid.uuid, exp, fed->fed_pending);
        /* ofd_tot_pending is handled in ofd_grant_commit as bulk
         * finishes */
        LASSERTF(ofd->ofd_tot_dirty >= fed->fed_dirty,
                 "%s: tot_dirty "LPU64" cli %s/%p fed_dirty %ld\n",
                 obd->obd_name, ofd->ofd_tot_dirty,
                 exp->exp_client_uuid.uuid, exp, fed->fed_dirty);
        ofd->ofd_tot_dirty -= fed->fed_dirty;
        fed->fed_dirty = 0;
        cfs_spin_unlock(&ofd->ofd_grant_lock);
}

/**
 * Called at prepare time when handling read request. This function extracts
 * incoming grant information from the obdo and processes the grant shrink
 * request, if any.
 *
 * \param env - is the lu environment provided by the caller
 * \param exp - is the export of the client which sent the request
 * \paral oa - is the incoming obdo sent by the client
 */
void ofd_grant_prepare_read(const struct lu_env *env,
                            struct obd_export *exp, struct obdo *oa)
{
        struct ofd_device *ofd = ofd_exp(exp);
        int                do_shrink;
        obd_size           left = 0;

        if (!oa)
                return;

        if ((oa->o_valid & OBD_MD_FLGRANT) == 0)
                /* The read request does not contain any grant
                 * information */
                return;

        if ((oa->o_valid & OBD_MD_FLFLAGS) &&
            (oa->o_flags & OBD_FL_SHRINK_GRANT)) {
                /* To process grant shrink request, we need to know how much
                 * available space remains on the backend filesystem.
                 * Shrink requests are not so common, we always get fresh
                 * statfs information. */
                ofd_grant_statfs(env, exp, 1, NULL);

                /* protect all grant counters */
                cfs_spin_lock(&ofd->ofd_grant_lock);

                /* Grab free space from cached statfs data and take out space
                 * already granted to clients as well as reserved space */
                left = ofd_grant_space_left(exp);

                /* all set now to proceed with shrinking */
                do_shrink = 1;
        } else {
                /* no grant shrinking request packed in the obdo and
                 * since we don't grant space back on reads, no point
                 * in running statfs, so just skip it and process
                 * incoming grant data directly. */
                cfs_spin_lock(&ofd->ofd_grant_lock);
                do_shrink = 0;
        }

        /* extract incoming grant infomation provided by the client */
        ofd_grant_incoming(env, exp, oa);

        /* unlike writes, we don't return grants back on reads unless a grant
         * shrink request was packed and we decided to turn it down. */
        if (do_shrink)
                ofd_grant_shrink(exp, oa, left);
        else
                oa->o_grant = 0;

        cfs_spin_unlock(&ofd->ofd_grant_lock);
}

/**
 * Called at write prepare time to handle incoming grant, check that we have
 * enough space and grant some space back to the client if possible.
 *
 * \param env - is the lu environment provided by the caller
 * \param exp - is the export of the client which sent the request
 * \paral oa - is the incoming obdo sent by the client
 * \param lnb - is the list of local io buffer
 * \param niocont - is the number of pages in the local io buffer
 * \param from_grant - is set to 1 when OBD_BRW_FROM_GRANT is set on all the
 *                     pages
 */
int ofd_grant_prepare_write(const struct lu_env *env,
                            struct obd_export *exp, struct obdo *oa,
                            struct niobuf_local *lnb, int niocount,
                            int from_grant)
{
        struct obd_device *obd = exp->exp_obd;
        struct ofd_device *ofd = ofd_exp(exp);
        obd_size           left;
        int                from_cache;
        int                force = 0; /* can use cached statfs data intially */
        int                rc;
        ENTRY;

refresh:
        /* get statfs information from OSD layer */
        ofd_grant_statfs(env, exp, force, &from_cache);

        cfs_spin_lock(&ofd->ofd_grant_lock); /* protect all grant counters */

        /* Grab free space from cached statfs data and take out space already granted
         * to clients as well as reserved space */
        left = ofd_grant_space_left(exp);

        /* Get fresh statfs data if we are short in ungranted space */
        if (from_cache && left < 32 * OFD_GRANT_CHUNK) {
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                CDEBUG(D_CACHE, "%s: fs has no space left and statfs too old\n",
                       obd->obd_name);
                force = 1;
                goto refresh;
        }

        /* When close to free space exhaustion, trigger a sync to force
         * writeback cache to consume required space immediately and release as
         * much space as possible.
         * That said, it is worth running a sync only if some pages did not
         * consume grant space on the client and could thus fail with ENOSPC
         * later in ofd_grant_check() */
        if (!from_grant && force != 2 && left < OFD_GRANT_CHUNK) {
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                /* discard errors, at least we tried ... */
                rc = dt_sync(env, ofd->ofd_osd);
                force = 2;
                goto refresh;
        }

        /* extract incoming grant information provided by the client */
        ofd_grant_incoming(env, exp, oa);

        /* check limit and return ENOSPC if needed */
        rc = ofd_grant_check(env, exp, oa, lnb, niocount, &left);
        /* rc should not be modified after this line */

        /* if OBD_FL_SHRINK_GRANT is set, the client is willing to release some
         * grant space. */
        if ((oa->o_valid & OBD_MD_FLFLAGS) &&
            (oa->o_flags & OBD_FL_SHRINK_GRANT))
                ofd_grant_shrink(exp, oa, left);
        else
                /* grant more space back to the client if possible */
                oa->o_grant = ofd_grant(exp, oa->o_grant, oa->o_undirty, left);

        cfs_spin_unlock(&ofd->ofd_grant_lock);
        RETURN(rc);
}

/**
 * Called at commit time to update pending grant counter for writes in flight
 *
 * \param env - is the lu environment provided by the caller
 * \param exp - is the export of the client which sent the request
 */
void ofd_grant_commit(const struct lu_env *env, struct obd_export *exp,
                      int rc)
{
        struct ofd_device      *ofd  = ofd_exp(exp);
        struct ofd_thread_info *info = ofd_info(env);
        unsigned long           pending;

        /* get space accounted in tot_pending for the I/O, set in
         * ofd_grant_check() */
        pending = info->fti_used;

        cfs_spin_lock(&ofd->ofd_grant_lock);
        /* Don't update statfs data for errors raised before commit (e.g.
         * bulk transfer failed, ...) since we know those writes have not been
         * processed. For other errors hit during commit, we cannot really tell
         * whether or not something was written, so we update statfs data.
         * In any case, this should not be fatal since we always get fresh
         * statfs data before failing a request with ENOSPC */
        if (rc == 0) {
                cfs_spin_lock(&ofd->ofd_osfs_lock);
                /* Take pending out of cached statfs data */
                ofd->ofd_osfs.os_bavail -= min_t(obd_size,
                                                 ofd->ofd_osfs.os_bavail,
                                                 pending >> ofd->ofd_blockbits);
                if (ofd->ofd_statfs_inflight)
                        /* someone is running statfs and want to be notified of
                         * writes happening meanwhile */
                        ofd->ofd_osfs_inflight += pending;
                cfs_spin_unlock(&ofd->ofd_osfs_lock);
        }

        if (exp->exp_filter_data.fed_pending < pending) {
                CERROR("%s: cli %s/%p fed_pending(%lu) < grant_used(%lu)\n",
                       exp->exp_obd->obd_name, exp->exp_client_uuid.uuid, exp,
                       exp->exp_filter_data.fed_pending, pending);
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                LBUG();
        }
        exp->exp_filter_data.fed_pending -= pending;

        if (ofd->ofd_tot_granted < pending) {
                 CERROR("%s: cli %s/%p tot_granted("LPU64") < grant_used(%lu)"
                        "\n", exp->exp_obd->obd_name,
                        exp->exp_client_uuid.uuid, exp, ofd->ofd_tot_granted,
                        pending);
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                LBUG();
        }
        ofd->ofd_tot_granted -= pending;

        if (ofd->ofd_tot_pending < pending) {
                 CERROR("%s: cli %s/%p tot_pending("LPU64") < grant_used(%lu)"
                        "\n", exp->exp_obd->obd_name, exp->exp_client_uuid.uuid,
                        exp, ofd->ofd_tot_pending, pending);
                cfs_spin_unlock(&ofd->ofd_grant_lock);
                LBUG();
        }
        ofd->ofd_tot_pending -= pending;
        cfs_spin_unlock(&ofd->ofd_grant_lock);
}
