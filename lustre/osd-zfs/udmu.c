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
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/dmu/udmu.c
 * Module that interacts with the ZFS DMU and provides an abstraction
 * to the rest of Lustre.
 *
 * Author: Alex Tomas <alex@clusterfs.com>
 * Author: Atul Vidwansa <atul.vidwansa@sun.com>
 * Author: Manoj Joseph <manoj.joseph@sun.com>
 * Author: Mike Pershin <tappro@sun.com>
 */

#include <sys/dnode.h>
#include <sys/dbuf.h>
#include <sys/spa.h>
#include <sys/stat.h>
#include <sys/zap.h>
#include <sys/spa_impl.h>
#include <sys/zfs_znode.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_prop.h>
#include <sys/sa_impl.h>
#include <sys/txg.h>

#include <lustre/lustre_idl.h>  /* OBD_OBJECT_EOF */
#include <lustre/lustre_user.h> /* struct obd_statfs */

#include "udmu.h"

static void udmu_gethrestime(struct timespec *tp)
{
        struct timeval time;
        cfs_gettimeofday(&time);
        tp->tv_nsec = 0;
        tp->tv_sec = time.tv_sec;
}

int udmu_objset_open(char *osname, udmu_objset_t *uos)
{
        uint64_t refdbytes, availbytes, usedobjs, availobjs;
        uint64_t version = ZPL_VERSION;
        int      error;

        memset(uos, 0, sizeof(udmu_objset_t));

        error = dmu_objset_own(osname, DMU_OST_ZFS, B_FALSE, uos, &uos->os);
        if (error) {
                uos->os = NULL;
                goto out;
        }

        /* Check ZFS version */
        error = zap_lookup(uos->os, MASTER_NODE_OBJ, ZPL_VERSION_STR, 8, 1,
                           &version);
        if (error) {
                CERROR("Error looking up ZPL VERSION\n");
                /*
                 * We can't return ENOENT because that would mean the objset
                 * didn't exist.
                 */
                error = EIO;
                goto out;
        }

        error = zap_lookup(uos->os, MASTER_NODE_OBJ, ZFS_ROOT_OBJ,
                           8, 1, &uos->root);
        if (error) {
                CERROR("Error looking up ZFS root object.\n");
                error = EIO;
                goto out;
        }
        ASSERT(uos->root != 0);

        /* Check that user/group usage tracking is supported */
        if (!dmu_objset_userused_enabled(uos->os) ||
            DMU_USERUSED_DNODE(uos->os)->dn_type != DMU_OT_USERGROUP_USED ||
            DMU_GROUPUSED_DNODE(uos->os)->dn_type != DMU_OT_USERGROUP_USED) {
                CERROR("Space accounting not supported by this target, "
                       "aborting\n");
                error = ENOTSUPP;
                goto out;
        }

        /*
         * as DMU doesn't maintain f_files absolutely actual (it's updated
         * at flush, not when object is create/destroed) we've implemented
         * own counter which is initialized from on-disk at mount, then is
         * being maintained by DMU OSD
         */
        dmu_objset_space(uos->os, &refdbytes, &availbytes, &usedobjs,
                         &availobjs);
        uos->objects = usedobjs;
        cfs_spin_lock_init(&uos->lock);

out:
        if (error && uos->os != NULL)
                dmu_objset_disown(uos->os, uos);

        return error;
}

uint64_t udmu_get_txg(udmu_objset_t *uos, dmu_tx_t *tx)
{
        ASSERT(tx != NULL);
        return tx->tx_txg;
}

void udmu_wait_txg_synced(udmu_objset_t *uos, uint64_t txg)
{
        /* Wait for the pool to be synced */
        txg_wait_synced(dmu_objset_pool(uos->os), txg);
}

void udmu_wait_synced(udmu_objset_t *uos, dmu_tx_t *tx)
{
        /* Wait for the pool to be synced */
        txg_wait_synced(dmu_objset_pool(uos->os),
                        tx ? tx->tx_txg : 0ULL);
}

void udmu_force_commit(udmu_objset_t *uos)
{
        tx_state_t *tx = &dmu_objset_pool(uos->os)->dp_tx;
        uint64_t txg;

        mutex_enter(&tx->tx_sync_lock);
        txg = tx->tx_open_txg + 1;
        if (tx->tx_quiesce_txg_waiting < txg) {
                tx->tx_quiesce_txg_waiting = txg;
                cv_broadcast(&tx->tx_quiesce_more_cv);
        }
        mutex_exit(&tx->tx_sync_lock);
}

void udmu_objset_close(udmu_objset_t *uos)
{
        ASSERT(uos->os != NULL);

        /*
         * Force a txg sync.  This should not be needed, neither for
         * correctness nor safety.  Presumably, we are only doing
         * this to force commit callbacks to be called sooner.
         */
        udmu_wait_synced(uos, NULL);

        /* close the object set */
        dmu_objset_disown(uos->os, uos);

        uos->os = NULL;
}

int udmu_objset_statfs(udmu_objset_t *uos, struct obd_statfs *osfs)
{
        uint64_t refdbytes, availbytes, usedobjs, availobjs;
        uint64_t est_refdblocks, est_usedobjs, est_availobjs;
        uint64_t reserved;

        dmu_objset_space(uos->os, &refdbytes, &availbytes, &usedobjs,
                         &availobjs);

        /*
         * ZFS allows multiple block sizes.  For statfs, Linux makes no
         * proper distinction between bsize and frsize.  For calculations
         * of free and used blocks incorrectly uses bsize instead of frsize,
         * but bsize is also used as the optimal blocksize.  We return the
         * largest possible block size as IO size for the optimum performance
         * and scale the free and used blocks count appropriately.
         */
        osfs->os_bsize = 1ULL << SPA_MAXBLOCKSHIFT;

        osfs->os_blocks = (refdbytes + availbytes) >> SPA_MAXBLOCKSHIFT;
        osfs->os_bfree = availbytes >> SPA_MAXBLOCKSHIFT;
        osfs->os_bavail = osfs->os_bfree; /* no extra root reservation */

        /* Take replication (i.e. number of copies) into account */
        osfs->os_bavail /= uos->os->os_copies;

        /*
         * Reserve some space so we don't run into ENOSPC due to grants not
         * accounting for metadata overhead in ZFS, and to avoid fragmentation.
         * Rather than report this via os_bavail (which makes users unhappy if
         * they can't fill the filesystem 100%), reduce os_blocks as well.
         *
         * Reserve 0.78% of total space, at least 4MB for small filesystems,
         * for internal files to be created/unlinked when space is tight.
         */
        CLASSERT(OSD_STATFS_RESERVED_BLKS > 0);
        if (likely(osfs->os_blocks >=
                   OSD_STATFS_RESERVED_BLKS << OSD_STATFS_RESERVED_SHIFT))
                reserved = osfs->os_blocks >> OSD_STATFS_RESERVED_SHIFT;
        else
                reserved = OSD_STATFS_RESERVED_BLKS;

        osfs->os_blocks -= reserved;
        osfs->os_bfree  -= MIN(reserved, osfs->os_bfree);
        osfs->os_bavail -= MIN(reserved, osfs->os_bavail);

        /*
         * The availobjs value returned from dmu_objset_space() is largely
         * useless, since it reports the number of objects that might
         * theoretically still fit into the dataset, independent of minor
         * issues like how much space is actually available in the pool.
         *
         * Compute an os_bfree estimate based on the actual number of
         * dnodes that could fit in the available space.  Since we don't
         * know the overhead associated with each dnode (xattrs, SAs,
         * VDEV overhead, etc) just using DNODE_SHIFT isn't going to give
         * a good estimate.  Instead, compute an estimate based on the
         * average space usage per dnode, with an upper and lower cap.
         *
         * In case there aren't many dnodes or blocks used yet, add a small
         * correction factor using OSD_DNODE_EST_SHIFT.  This correction
         * factor gradually disappears as the number of real dnodes grows.
         * This also avoids the need to check for divide-by-zero later.
         */
        CLASSERT(OSD_DNODE_MIN_BLKSHIFT > 0);
        CLASSERT(OSD_DNODE_EST_BLKSHIFT > 0);

        est_refdblocks = (refdbytes >> SPA_MAXBLOCKSHIFT) +
                         (OSD_DNODE_EST_COUNT << OSD_DNODE_EST_BLKSHIFT);
        est_usedobjs   = usedobjs + OSD_DNODE_EST_COUNT;

        /* Average space/dnode more than maximum dnode size, use max dnode
         * size to estimate free dnodes from adjusted free blocks count.
         * OSTs typically use more than one block dnode so this case applies. */
        if (est_usedobjs <= est_refdblocks * 2) {
                est_availobjs = osfs->os_bfree;

        /* Average space/dnode smaller than min dnode size (probably due to
         * metadnode compression), use min dnode size to estimate free objs.
         * An MDT typically uses below 512 bytes/dnode so this case applies. */
        } else if (est_usedobjs >= (est_refdblocks << OSD_DNODE_MIN_BLKSHIFT)) {
                est_availobjs = osfs->os_bfree << OSD_DNODE_MIN_BLKSHIFT;

        /* Between the extremes, we try to use the average size of existing
         * dnodes to compute the number of dnodes that fit into the free space:
         *
         * est_availobjs = osfs->os_bfree * (est_usedobjs / est_refblocks);
         *
         * but this may overflow 64 bits or become 0 if not handled well.
         *
         * We know os_bfree is below (64 - 17 = 47) bits from SPA_MAXBLKSHIFT,
         * and est_usedobjs is under 48 bits due to DN_MAX_OBJECT_SHIFT, which
         * means that multiplying them may get as large as 2 ^ 95.
         *
         * We also know (est_usedobjs / est_refdblocks) is between 2 and 256,
         * due to above checks, so we can safely compute this first.  We care
         * more about accuracy on the MDT (many dnodes/block) which is good
         * because this is where truncation errors are smallest.  This adds
         * 8 bits to os_bfree so we can use 7 bits to compute a fixed-point
         * fraction and os_bfree can still fit in 64 bits. */
        } else {
                unsigned dnodes_per_block = (est_usedobjs << 7)/est_refdblocks;

                est_availobjs = (osfs->os_bfree * dnodes_per_block) >> 7;
        }

        osfs->os_ffree = min(availobjs, est_availobjs);
        osfs->os_files = osfs->os_ffree + uos->objects;

        /* ZFS XXX: fill in backing dataset FSID/UUID
        memcpy(osfs->os_fsid, .... );*/

        /* We're a zfs filesystem. */
        osfs->os_type = UBERBLOCK_MAGIC;

        /* ZFS XXX: fill in appropriate OS_STATE_{DEGRADED,READONLY} flags
        osfs->os_state = vf_to_stf(vfsp->vfs_flag);
        if (sb->s_flags & MS_RDONLY)
                osfs->os_state = OS_STATE_READONLY;
         */

        osfs->os_namelen = 256;
        osfs->os_maxbytes = OBD_OBJECT_EOF;

        return 0;
}

/* Get the objset name.
   buf must have at least MAXNAMELEN bytes */
void udmu_objset_name_get(udmu_objset_t *uos, char *buf)
{
        dmu_objset_name(uos->os, buf);
}

static int udmu_userprop_setup(udmu_objset_t *uos, const char *prop_name,
                           char **os_name, char **real_prop)
{
        if (os_name != NULL) {
                *os_name = kmem_alloc(MAXNAMELEN, KM_SLEEP);
                udmu_objset_name_get(uos, *os_name);
        }

        *real_prop = kmem_alloc(MAXNAMELEN, KM_SLEEP);

        if (snprintf(*real_prop, MAXNAMELEN, "lustre:%s", prop_name) >=
            MAXNAMELEN) {
                if (os_name != NULL)
                        kmem_free(*os_name, MAXNAMELEN);
                kmem_free(*real_prop, MAXNAMELEN);

                CERROR("property name too long: %s\n", prop_name);
                return ENAMETOOLONG;
        }

        return 0;
}

static void udmu_userprop_cleanup(char **os_name, char **real_prop)
{
        if (os_name != NULL)
                kmem_free(*os_name, MAXNAMELEN);
        kmem_free(*real_prop, MAXNAMELEN);
}

/* Set ZFS user property 'prop_name' of objset 'uos' to string 'val' */
int udmu_userprop_set_str(udmu_objset_t *uos, const char *prop_name,
                      const char *val)
{
        char *os_name;
        char *real_prop;
        int rc;

        rc = udmu_userprop_setup(uos, prop_name, &os_name, &real_prop);
        if (rc != 0)
                return rc;

        rc = dsl_prop_set(os_name, real_prop, ZPROP_SRC_LOCAL, 1,
                          strlen(val) + 1, val);
        udmu_userprop_cleanup(&os_name, &real_prop);

        return rc;
}

/* Get ZFS user property 'prop_name' of objset 'uos' into buffer 'buf' of size
   'buf_size' */
int udmu_userprop_get_str(udmu_objset_t *uos, const char *prop_name, char *buf,
                      size_t buf_size)
{
        char *real_prop;
        char *nvp_val;
        size_t nvp_len;
        nvlist_t *nvl = NULL;
        nvlist_t *nvl_val;
        nvpair_t *elem = NULL;
        int rc;

        rc = udmu_userprop_setup(uos, prop_name, NULL, &real_prop);
        if (rc != 0)
                return rc;

        /* We can't just pass buf_size to dsl_prop_get() because it expects the
           exact value size (zap_lookup() requirement), so we must get all props
           and extract the one we want. */
        rc = dsl_prop_get_all(uos->os, &nvl);
        if (rc != 0) {
                nvl = NULL;
                goto out;
        }

        while ((elem = nvlist_next_nvpair(nvl, elem)) != NULL) {
                const char *name = nvpair_name(elem);
                if (strcmp(name, real_prop) != 0)
                        continue;

                /* Got the property we were looking for, but the val is not the
                   string yet, it's an nvlist */

                rc = nvpair_value_nvlist(elem, &nvl_val);
                if (rc != 0)
                        goto out;

                rc = nvlist_lookup_string(nvl_val, ZPROP_VALUE, &nvp_val);
                if (rc != 0)
                        goto out;

                nvp_len = strlen(nvp_val);
                if (buf_size < nvp_len + 1) {
                        CWARN("buffer too small (%llu) for string(%llu): '%s'"
                              "\n", (u_longlong_t) buf_size,
                              (u_longlong_t) nvp_len, nvp_val);
                        rc = EOVERFLOW;
                        goto out;
                }
                strcpy(buf, nvp_val);
                goto out;
        }
        /* Not found */
        rc = ENOENT;
out:
        if (nvl != NULL)
                nvlist_free(nvl);
        udmu_userprop_cleanup(NULL, &real_prop);

        return rc;
}

static int udmu_obj2dbuf(objset_t *os, uint64_t oid, dmu_buf_t **dbp, void *tag)
{
        dmu_object_info_t doi;
        int err;

        ASSERT(tag);

        err = dmu_bonus_hold(os, oid, tag, dbp);
        if (err) {
                return (err);
        }

        dmu_object_info_from_db(*dbp, &doi);
        if (unlikely (oid != DMU_USERUSED_OBJECT &&
                      oid != DMU_GROUPUSED_OBJECT &&
                      (doi.doi_bonus_type != DMU_OT_ZNODE ||
                       doi.doi_bonus_size < sizeof (znode_phys_t)))) {
                dmu_buf_rele(*dbp, tag);
                *dbp = NULL;
                return (EINVAL);
        }

        ASSERT(*dbp);
        ASSERT((*dbp)->db_object == oid);
        ASSERT((*dbp)->db_offset == -1);
        ASSERT((*dbp)->db_data != NULL);

        return (0);
}

int udmu_objset_root(udmu_objset_t *uos, dmu_buf_t **dbp, void *tag)
{
        return udmu_obj2dbuf(uos->os, uos->root, dbp, tag);
}

int udmu_zap_lookup(udmu_objset_t *uos, dmu_buf_t *zap_db, const char *name,
                    void *value, int value_size, int intsize)
{
        uint64_t oid;
        oid = zap_db->db_object;

        if (strlen(name) >= MAXNAMELEN)
                return EOVERFLOW;
        /*
         * value_size should be a multiple of intsize.
         * intsize is 8 for micro ZAP and 1, 2, 4 or 8 for a fat ZAP.
         */
        ASSERT(value_size % intsize == 0);
        return (zap_lookup(uos->os, oid, name, intsize,
                           value_size / intsize, value));
}

int udmu_object_set_blocksize(udmu_objset_t *uos, uint64_t oid,
                              unsigned bsize, dmu_tx_t *tx)
{
        return dmu_object_set_blocksize(uos->os, oid, bsize, 0, tx);
}

/*
 * The transaction passed to this routine must have
 * udmu_tx_hold_bonus(tx, DMU_NEW_OBJECT) called and then assigned
 * to a transaction group.
 */
static void udmu_object_create_impl(objset_t *os, dmu_buf_t **dbp, dmu_tx_t *tx,
                                    void *tag)
{
        znode_phys_t    *zp;
        uint64_t        oid;
        uint64_t        gen;
        timestruc_t     now;

        ASSERT(tag);

        /* Assert that the transaction has been assigned to a
           transaction group. */
        ASSERT(tx->tx_txg != 0);

        udmu_gethrestime(&now);
        gen = dmu_tx_get_txg(tx);

        /* Create a new DMU object. */
        oid = dmu_object_alloc(os, DMU_OT_PLAIN_FILE_CONTENTS, 0, DMU_OT_ZNODE,
                               sizeof (znode_phys_t), tx);

#if 0
        /* XXX: do we really need 128K blocksize by default? even on OSS? */
        dmu_object_set_blocksize(os, oid, 128ULL << 10, 0, tx);
#endif

        VERIFY(0 == dmu_bonus_hold(os, oid, tag, dbp));

        dmu_buf_will_dirty(*dbp, tx);

        /* Initialize the znode physical data to zero. */
        ASSERT((*dbp)->db_size >= sizeof (znode_phys_t));
        bzero((*dbp)->db_data, (*dbp)->db_size);
        zp = (*dbp)->db_data;
        zp->zp_gen = gen;
        zp->zp_links = 1;
        ZFS_TIME_ENCODE(&now, zp->zp_crtime);
        ZFS_TIME_ENCODE(&now, zp->zp_ctime);
        ZFS_TIME_ENCODE(&now, zp->zp_atime);
        ZFS_TIME_ENCODE(&now, zp->zp_mtime);
        zp->zp_mode = MAKEIMODE(VREG, 0007);
}

void udmu_object_create(udmu_objset_t *uos, dmu_buf_t **dbp, dmu_tx_t *tx,
                        void *tag)
{
        cfs_spin_lock(&uos->lock);
        uos->objects++;
        cfs_spin_unlock(&uos->lock);
        udmu_object_create_impl(uos->os, dbp, tx, tag);
}

/*
 * The transaction passed to this routine must have
 * udmu_tx_hold_zap(tx, DMU_NEW_OBJECT, ...) called and then assigned
 * to a transaction group.
 *
 * Using ZAP_FLAG_HASH64 will force the ZAP to always be a FAT ZAP.
 * This is fine for directories today, because storing the FID in the dirent
 * will also require a FAT ZAP.  If there is a new type of micro ZAP created
 * then we might need to re-evaluate the use of this flag and instead do
 * a conversion from the different internal ZAP hash formats being used. */
static void udmu_zap_create_impl(objset_t *os, dmu_buf_t **zap_dbp,
                                 dmu_tx_t *tx, void *tag)
{
        znode_phys_t *zp;
        uint64_t      oid;
        timestruc_t   now;
        uint64_t      gen;
        zap_flags_t   flags = ZAP_FLAG_HASH64;

        ASSERT(tag);

        /* Assert that the transaction has been assigned to a
           transaction group. */
        ASSERT(tx->tx_txg != 0);

        oid = 0;
        udmu_gethrestime(&now);
        gen = dmu_tx_get_txg(tx);

        oid = zap_create_flags(os, 0, flags, DMU_OT_DIRECTORY_CONTENTS, 9, 12,
                               DMU_OT_ZNODE, sizeof(znode_phys_t), tx);

        VERIFY(0 == dmu_bonus_hold(os, oid, tag, zap_dbp));

        dmu_buf_will_dirty(*zap_dbp, tx);

        bzero((*zap_dbp)->db_data, (*zap_dbp)->db_size);
        zp = (*zap_dbp)->db_data;
        zp->zp_size = 2;
        zp->zp_links = 1;
        zp->zp_gen = gen;
        zp->zp_mode = MAKEIMODE(VDIR, 0007);

        ZFS_TIME_ENCODE(&now, zp->zp_crtime);
        ZFS_TIME_ENCODE(&now, zp->zp_ctime);
        ZFS_TIME_ENCODE(&now, zp->zp_atime);
        ZFS_TIME_ENCODE(&now, zp->zp_mtime);
}

void udmu_zap_create(udmu_objset_t *uos, dmu_buf_t **zap_dbp, dmu_tx_t *tx,
                     void *tag)
{
        cfs_spin_lock(&uos->lock);
        uos->objects++;
        cfs_spin_unlock(&uos->lock);
        udmu_zap_create_impl(uos->os, zap_dbp, tx, tag);
}

int udmu_object_get_dmu_buf(udmu_objset_t *uos, uint64_t object,
                            dmu_buf_t **dbp, void *tag)
{
        return udmu_obj2dbuf(uos->os, object, dbp, tag);
}


/*
 * The transaction passed to this routine must have
 * udmu_tx_hold_bonus(tx, oid) and
 * udmu_tx_hold_zap(tx, oid, ...)
 * called and then assigned to a transaction group.
 */
static int udmu_zap_insert_impl(objset_t *os, dmu_buf_t *zap_db, dmu_tx_t *tx,
                                const char *name, void *value, int len)
{
        uint64_t oid = zap_db->db_object;
        int num_int = 1, int_size = 8;

        /* fid record is byte stream*/
        if (len == 17) {
                int_size = 1;
                num_int = len;
        } else if (len == 6) {
                int_size = 1;
                num_int = len;
        }

        /* Assert that the transaction has been assigned to a
           transaction group. */
        ASSERT(tx->tx_txg != 0);

        dmu_buf_will_dirty(zap_db, tx);
        return (zap_add(os, oid, name, int_size, num_int, value, tx));
}

int udmu_zap_insert(udmu_objset_t *uos, dmu_buf_t *zap_db, dmu_tx_t *tx,
                    const char *name, void *value, int len)
{
        return udmu_zap_insert_impl(uos->os, zap_db, tx, name, value, len);
}

/*
 * The transaction passed to this routine must have
 * udmu_tx_hold_zap(tx, oid, ...) called and then
 * assigned to a transaction group.
 */
int udmu_zap_delete(udmu_objset_t *uos, dmu_buf_t *zap_db, dmu_tx_t *tx,
                    const char *name)
{
        uint64_t oid = zap_db->db_object;

        /* Assert that the transaction has been assigned to a
           transaction group. */
        ASSERT(tx->tx_txg != 0);

        return (zap_remove(uos->os, oid, name, tx));
}

/* We don't actually have direct access to the zap_hashbits() function
 * so just pretend like we do for now.  If this ever breaks we can look at
 * it at that time. */
#define zap_hashbits(zc) 48
/*
 * ZFS hash format:
 * | cd (16 bits) | hash (48 bits) |
 * we need it in other form:
 * |0| hash (48 bit) | cd (15 bit) |
 * to be a full 64-bit ordered hash so that Lustre readdir can use it to merge
 * the readdir hashes from multiple directory stripes uniformly on the client.
 * Another point is sign bit, the hash range should be in [0, 2^63-1] because
 * loff_t (for llseek) needs to be a positive value.  This means the "cd" field
 * should only be the low 15 bits.
 */
uint64_t udmu_zap_cursor_serialize(zap_cursor_t *zc)
{
        uint64_t zfs_hash = zap_cursor_serialize(zc) & (~0ULL >> 1);

        return (zfs_hash >> zap_hashbits(zc)) |
               (zfs_hash << (63 - zap_hashbits(zc)));
}

void udmu_zap_cursor_init_serialized(zap_cursor_t *zc, udmu_objset_t *uos,
                                     uint64_t zapobj, uint64_t dirhash)
{
        uint64_t zfs_hash = ((dirhash << zap_hashbits(zc)) & (~0ULL >> 1)) |
                            (dirhash >> (63 - zap_hashbits(zc)));
        zap_cursor_init_serialized(zc, uos->os, zapobj, zfs_hash);
}

/*
 * Zap cursor APIs
 */
int udmu_zap_cursor_init(zap_cursor_t **zc, udmu_objset_t *uos,
                         uint64_t zapobj, uint64_t dirhash)
{
        zap_cursor_t *t;

        t = kmem_alloc(sizeof(*t), KM_NOSLEEP);
        if (t) {
                udmu_zap_cursor_init_serialized(t, uos, zapobj, dirhash);
                *zc = t;
                return 0;
        }
        return (ENOMEM);
}

void udmu_zap_cursor_fini(zap_cursor_t *zc)
{
        zap_cursor_fini(zc);
        kmem_free(zc, sizeof(*zc));
}

int udmu_zap_cursor_retrieve_key(zap_cursor_t *zc, char *key, int max)
{
        zap_attribute_t za;
        int             err;

        if ((err = zap_cursor_retrieve(zc, &za)))
                return err;

        if (key) {
                if (strlen(za.za_name) > max)
                        return EOVERFLOW;
                strcpy(key, za.za_name);
        }

        return 0;
}

/*
 * zap_cursor_retrieve read from current record.
 * to read bytes we need to call zap_lookup explicitly.
 */
int udmu_zap_cursor_retrieve_value(zap_cursor_t *zc,  char *buf,
                int buf_size, int *bytes_read)
{
        int err, actual_size;
        zap_attribute_t za;


        if ((err = zap_cursor_retrieve(zc, &za)))
                return err;

        if (za.za_integer_length <= 0)
                return (ERANGE);

        actual_size = za.za_integer_length * za.za_num_integers;

        if (actual_size > buf_size) {
                actual_size = buf_size;
                buf_size = actual_size / za.za_integer_length;
        } else {
                buf_size = za.za_num_integers;
        }

        err = zap_lookup(zc->zc_objset, zc->zc_zapobj,
                        za.za_name, za.za_integer_length, buf_size, buf);

        if (!err)
                *bytes_read = actual_size;

        return err;
}

void udmu_zap_cursor_advance(zap_cursor_t *zc)
{
        zap_cursor_advance(zc);
}

int udmu_zap_cursor_move_to_key(zap_cursor_t *zc, const char *name)
{
        return zap_cursor_move_to_key(zc, name, MT_BEST);
}

/*
 * Read data from a DMU object
 */
static int udmu_object_read_impl(objset_t *os, dmu_buf_t *db, uint64_t offset,
                                 uint64_t size, void *buf)
{
        uint64_t oid = db->db_object;
        vattr_t  va;
        int rc;

        udmu_object_getattr(db, &va);
        if (offset + size > va.va_size) {
                if (va.va_size < offset)
                        size = 0;
                else
                        size = va.va_size - offset;
        }

        rc = dmu_read(os, oid, offset, size, buf, DMU_READ_PREFETCH);
        if (rc == 0)
                return size;
        else
                return (-rc);
}

int udmu_object_read(udmu_objset_t *uos, dmu_buf_t *db, uint64_t offset,
                     uint64_t size, void *buf)
{
        return udmu_object_read_impl(uos->os, db, offset, size, buf);
}

/*
 * Write data to a DMU object
 *
 * The transaction passed to this routine must have had
 * udmu_tx_hold_write(tx, oid, offset, size) called and then
 * assigned to a transaction group.
 */
void udmu_object_write(udmu_objset_t *uos, dmu_buf_t *db, struct dmu_tx *tx,
                       uint64_t offset, uint64_t size, void *buf)
{
        uint64_t oid = db->db_object;

        dmu_write(uos->os, oid, offset, size, buf, tx);
}

/*
 * Retrieve the attributes of a DMU object
 */
void udmu_object_getattr(dmu_buf_t *db, vattr_t *vap)
{
        dmu_buf_impl_t *dbi = (dmu_buf_impl_t *) db;
        dnode_t *dn;

        znode_phys_t *zp = db->db_data;

        vap->va_mask = DMU_AT_ATIME | DMU_AT_MTIME | DMU_AT_CTIME | DMU_AT_MODE
                       | DMU_AT_SIZE | DMU_AT_UID | DMU_AT_GID | DMU_AT_TYPE
                       | DMU_AT_NLINK | DMU_AT_RDEV;

        vap->va_atime.tv_sec    = zp->zp_atime[0];
        vap->va_atime.tv_nsec   = 0;
        vap->va_mtime.tv_sec    = zp->zp_mtime[0];
        vap->va_mtime.tv_nsec   = 0;
        vap->va_ctime.tv_sec    = zp->zp_ctime[0];
        vap->va_ctime.tv_nsec   = 0;
        vap->va_mode     = zp->zp_mode & MODEMASK;;
        vap->va_size     = zp->zp_size;
        vap->va_uid      = zp->zp_uid;
        vap->va_gid      = zp->zp_gid;
        vap->va_type     = IFTOVT((mode_t)zp->zp_mode);
        vap->va_nlink    = zp->zp_links;
        vap->va_rdev     = zp->zp_rdev;

        DB_DNODE_ENTER(dbi);
        dn = DB_DNODE(dbi);

        vap->va_blksize = dn->dn_datablksz;
        /* vap->va_blkbits = dn->dn_datablkshift; */
        /* in 512-bytes units*/
        vap->va_nblocks = DN_USED_BYTES(dn->dn_phys) >> SPA_MINBLOCKSHIFT;
        vap->va_mask |= DMU_AT_NBLOCKS | DMU_AT_BLKSIZE;

        DB_DNODE_EXIT(dbi);
}

/*
 * Set the attributes of an object
 *
 * The transaction passed to this routine must have
 * udmu_tx_hold_bonus(tx, oid) called and then assigned
 * to a transaction group.
 */
void udmu_object_setattr(dmu_buf_t *db, dmu_tx_t *tx, vattr_t *vap)
{
        znode_phys_t *zp = db->db_data;
        uint_t mask = vap->va_mask;

        /* Assert that the transaction has been assigned to a
           transaction group. */
        ASSERT(tx->tx_txg != 0);

        if (mask == 0) {
                return;
        }

        dmu_buf_will_dirty(db, tx);

        /*
         * Set each attribute requested.
         * We group settings according to the locks they need to acquire.
         *
         * Note: you cannot set ctime directly, although it will be
         * updated as a side-effect of calling this function.
         */

        if (mask & DMU_AT_MODE)
                zp->zp_mode = MAKEIMODE(vap->va_type, vap->va_mode);

        if (mask & DMU_AT_UID)
                zp->zp_uid = (uint64_t)vap->va_uid;

        if (mask & DMU_AT_GID)
                zp->zp_gid = (uint64_t)vap->va_gid;

        if (mask & DMU_AT_SIZE)
                zp->zp_size = vap->va_size;

        if (mask & DMU_AT_ATIME)
                ZFS_TIME_ENCODE(&vap->va_atime, zp->zp_atime);

        if (mask & DMU_AT_MTIME)
                ZFS_TIME_ENCODE(&vap->va_mtime, zp->zp_mtime);

        if (mask & DMU_AT_CTIME)
                ZFS_TIME_ENCODE(&vap->va_ctime, zp->zp_ctime);

        if (mask & DMU_AT_NLINK)
                zp->zp_links = vap->va_nlink;

        if (mask & DMU_AT_RDEV)
                zp->zp_rdev = vap->va_rdev;
}

/*
 * Punch/truncate an object
 *
 *      IN:     db      - dmu_buf of the object to free data in.
 *              off     - start of section to free.
 *              len     - length of section to free (0 => to EOF).
 *
 *      RETURN: 0 if success
 *              error code if failure
 *
 * The transaction passed to this routine must have
 * udmu_tx_hold_bonus(tx, oid) and
 * if off < size, udmu_tx_hold_free(tx, oid, off, len ? len : DMU_OBJECT_END)
 * called and then assigned to a transaction group.
 */
int udmu_object_punch_impl(objset_t *os, dmu_buf_t *db, dmu_tx_t *tx,
                            uint64_t off, uint64_t len)
{
        znode_phys_t *zp = db->db_data;
        uint64_t oid = db->db_object;
        uint64_t end = off + len;
        uint64_t size = zp->zp_size;
        int rc = 0;

        /* Assert that the transaction has been assigned to a
           transaction group. */
        ASSERT(tx->tx_txg != 0);

        /*
         * Nothing to do if file already at desired length.
         */
        if (len == 0 && size == off) {
                return 0;
        }

        if (end > size || len == 0) {
                zp->zp_size = end;
        }

        if (off < size) {
                uint64_t rlen = len;

                if (len == 0)
                        rlen = -1;
                else if (end > size)
                        rlen = size - off;

                rc = dmu_free_range(os, oid, off, rlen, tx);
        }
        return rc;
}

int udmu_object_punch(udmu_objset_t *uos, dmu_buf_t *db, dmu_tx_t *tx,
                      uint64_t off, uint64_t len)
{
        return udmu_object_punch_impl(uos->os, db, tx, off, len);
}

void udmu_declare_object_delete(udmu_objset_t *uos, dmu_tx_t *tx, dmu_buf_t *db)
{
        znode_phys_t    *zp = db->db_data;
        uint64_t         oid = db->db_object, xid;
        zap_attribute_t  za;
        zap_cursor_t    *zc;
        int              rc;

        dmu_tx_hold_free(tx, oid, 0, DMU_OBJECT_END);

        /* zap holding xattrs */
        if ((oid = zp->zp_xattr)) {
                dmu_tx_hold_free(tx, oid, 0, DMU_OBJECT_END);

                rc = udmu_zap_cursor_init(&zc, uos, oid, 0);
                if (rc) {
                        if (tx->tx_err == 0)
                                tx->tx_err = rc;
                        return;
                }
                while ((rc = zap_cursor_retrieve(zc, &za)) == 0) {
                        BUG_ON(za.za_integer_length != sizeof(uint64_t));
                        BUG_ON(za.za_num_integers != 1);

                        rc = zap_lookup(uos->os, zp->zp_xattr, za.za_name,
                                        sizeof(uint64_t), 1, &xid);
                        if (rc) {
                                printk("error during xattr lookup: %d\n", rc);
                                break;
                        }
                        dmu_tx_hold_free(tx, xid, 0, DMU_OBJECT_END);

                        zap_cursor_advance(zc);
                }
                udmu_zap_cursor_fini(zc);
        }
}

static int udmu_object_free(udmu_objset_t *uos, uint64_t oid, dmu_tx_t *tx)
{
        ASSERT(uos->objects != 0);
        cfs_spin_lock(&uos->lock);
        uos->objects--;
        cfs_spin_unlock(&uos->lock);

        return dmu_object_free(uos->os, oid, tx);
}

/*
 * Delete a DMU object
 *
 * The transaction passed to this routine must have
 * udmu_tx_hold_free(tx, oid, 0, DMU_OBJECT_END) called
 * and then assigned to a transaction group.
 *
 * This will release db and set it to NULL to prevent further dbuf releases.
 */
static int udmu_object_delete_impl(udmu_objset_t *uos, dmu_buf_t **db, dmu_tx_t *tx,
                       void *tag)
{
        znode_phys_t    *zp = (*db)->db_data;
        uint64_t         oid, xid;
        zap_attribute_t  za;
        zap_cursor_t    *zc;
        int              rc;

        /* Assert that the transaction has been assigned to a
           transaction group. */
        ASSERT(tx->tx_txg != 0);

        /* zap holding xattrs */
        if ((oid = zp->zp_xattr)) {

                rc = udmu_zap_cursor_init(&zc, uos, oid, 0);
                if (rc)
                        return rc;
                while ((rc = zap_cursor_retrieve(zc, &za)) == 0) {
                        BUG_ON(za.za_integer_length != sizeof(uint64_t));
                        BUG_ON(za.za_num_integers != 1);

                        rc = zap_lookup(uos->os, zp->zp_xattr, za.za_name,
                                        sizeof(uint64_t), 1, &xid);
                        if (rc) {
                                printk("error during xattr lookup: %d\n", rc);
                                break;
                        }
                        udmu_object_free(uos, xid, tx);

                        zap_cursor_advance(zc);
                }
                udmu_zap_cursor_fini(zc);

                udmu_object_free(uos, zp->zp_xattr, tx);
        }

        oid = (*db)->db_object;

        return udmu_object_free(uos, oid, tx);
}

int udmu_object_delete(udmu_objset_t *uos, dmu_buf_t **db, dmu_tx_t *tx,
                       void *tag)
{
        return udmu_object_delete_impl(uos, db, tx, tag);
}

/*
 * Get the object id from dmu_buf_t
 */
uint64_t udmu_object_get_id(dmu_buf_t *db)
{
        ASSERT(db != NULL);
        return (db->db_object);
}

int udmu_object_is_zap(dmu_buf_t *db)
{
        dmu_buf_impl_t *dbi = (dmu_buf_impl_t *) db;
        dnode_t *dn;
        int rc;

        DB_DNODE_ENTER(dbi);

        dn = DB_DNODE(dbi);
        rc = (dn->dn_type == DMU_OT_DIRECTORY_CONTENTS ||
              dn->dn_type == DMU_OT_USERGROUP_USED);

        DB_DNODE_EXIT(dbi);

        return rc;
}

/*
 * Release the reference to a dmu_buf object.
 */
void udmu_object_put_dmu_buf(dmu_buf_t *db, void *tag)
{
        ASSERT(tag);
        ASSERT(db);
        dmu_buf_rele(db, tag);
}

dmu_tx_t *udmu_tx_create(udmu_objset_t *uos)
{
        return (dmu_tx_create(uos->os));
}

void udmu_tx_hold_write(dmu_tx_t *tx, uint64_t object, uint64_t off, int len)
{
        dmu_tx_hold_write(tx, object, off, len);
}

void udmu_tx_hold_free(dmu_tx_t *tx, uint64_t object, uint64_t off,
                       uint64_t len)
{
        dmu_tx_hold_free(tx, object, off, len);
}

void udmu_tx_hold_zap(dmu_tx_t *tx, uint64_t object, int add, char *name)
{
        dmu_tx_hold_zap(tx, object, add, name);
}

void udmu_tx_hold_bonus(dmu_tx_t *tx, uint64_t object)
{
        dmu_tx_hold_bonus(tx, object);
}

void udmu_tx_abort(dmu_tx_t *tx)
{
        dmu_tx_abort(tx);
}

int udmu_tx_assign(dmu_tx_t *tx, uint64_t txg_how)
{
        return dmu_tx_assign(tx, txg_how);
}

void udmu_tx_wait(dmu_tx_t *tx)
{
        dmu_tx_wait(tx);
}

void udmu_tx_commit(dmu_tx_t *tx)
{
        dmu_tx_commit(tx);
}

/* commit callback API */
void udmu_tx_cb_register(dmu_tx_t *tx, udmu_tx_callback_func_t *func, void *data)
{
        dmu_tx_callback_register(tx, func, data);
}

uint64_t udmu_object_get_links(dmu_buf_t *db)
{
        znode_phys_t *zp = db->db_data;

        return zp->zp_links;
}

void udmu_object_links_inc(dmu_buf_t *db, dmu_tx_t *tx)
{
        znode_phys_t *zp = db->db_data;

        dmu_buf_will_dirty(db, tx);
        zp->zp_links++;
}

void udmu_object_links_dec(dmu_buf_t *db, dmu_tx_t *tx)
{
        znode_phys_t *zp = db->db_data;

        ASSERT(zp->zp_links != 0);

        dmu_buf_will_dirty(db, tx);
        zp->zp_links--;
}


/*
 * Copy an extended attribute into the buffer provided, or compute the
 * required buffer size.
 *
 * If buf is NULL, it computes the required buffer size.
 *
 * Returns 0 on success or a positive error number on failure.
 * On success, the number of bytes used / required is stored in 'size'.
 *
 * No locking is done here.
 */
int udmu_xattr_get(udmu_objset_t *uos, dmu_buf_t *db, void *buf,
                   int buflen, const char *name, int *size)
{
        znode_phys_t *zp = db->db_data;
        uint64_t xa_data_obj;
        dmu_buf_t *xa_data_db;
        vattr_t xa_data_va;
        int error;

        /*
         * If zp_xattr == 0, the xattr ZAP hasn't been created, which means
         * the dnode doesn't have any extended attributes.
         */
        if (zp->zp_xattr == 0)
                return ENOENT;

        /* Lookup the object number containing the xattr data */
        error = zap_lookup(uos->os, zp->zp_xattr, name, sizeof(uint64_t), 1,
                           &xa_data_obj);
        if (error)
                return error;

        error = udmu_obj2dbuf(uos->os, xa_data_obj, &xa_data_db, FTAG);
        if (error)
                return error;

        /* Get the xattr value length / object size */
        udmu_object_getattr(xa_data_db, &xa_data_va);

        if (xa_data_va.va_size > INT_MAX) {
                error = EOVERFLOW;
                goto out;
        }
        *size = (int) xa_data_va.va_size;

        if (buf == NULL) {
                /* We only need to return the required size */
                goto out;
        }
        if (*size > buflen) {
                error = ERANGE; /* match ldiskfs error */
                goto out;
        }

        error = dmu_read(uos->os, xa_data_db->db_object, 0, xa_data_va.va_size, buf, DMU_READ_PREFETCH);

out:
        udmu_object_put_dmu_buf(xa_data_db, FTAG);

        return error;
}

void udmu_xattr_declare_set(udmu_objset_t *uos, dmu_buf_t *db,
                            int vallen, const char *name, dmu_tx_t *tx)
{
        znode_phys_t *zp = NULL;
        uint64_t      xa_data_obj;
        int           error;

        if (db)
                zp = db->db_data;

        if (db == NULL || zp->zp_xattr == 0) {
                /* we'll be updating zp_xattr */
                if (db)
                        dmu_tx_hold_bonus(tx, db->db_object);
                /* xattr zap + entry */
                dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, TRUE, (char *) name);
                /* xattr value obj */
                dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
                dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, vallen);
                return;
        }

        error = zap_lookup(uos->os, zp->zp_xattr, name, sizeof(uint64_t), 1,
                           &xa_data_obj);
        if (error == 0) {
                /*
                 * Entry already exists.
                 * We'll truncate the existing object.
                 */
                dmu_tx_hold_bonus(tx, xa_data_obj);
                dmu_tx_hold_free(tx, xa_data_obj, vallen, DMU_OBJECT_END);
                dmu_tx_hold_write(tx, xa_data_obj, 0, vallen);
                return;
        } else if (error == ENOENT) {
                /*
                 * Entry doesn't exist, we need to create a new one and a new
                 * object to store the value.
                 */
                dmu_tx_hold_bonus(tx, zp->zp_xattr);
                dmu_tx_hold_zap(tx, zp->zp_xattr, TRUE, (char *) name);
                dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT);
                dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, vallen);
                return;
        }

        /* An error happened */
        tx->tx_err = error;
}

/*
 * Set an extended attribute.
 * This transaction must have called udmu_xattr_declare_set() first.
 *
 * Returns 0 on success or a positive error number on failure.
 *
 * No locking is done here.
 */
int udmu_xattr_set(udmu_objset_t *uos, dmu_buf_t *db, void *val,
                   int vallen, const char *name, int fl, dmu_tx_t *tx)
{
        znode_phys_t *zp = db->db_data;
        dmu_buf_t    *xa_zap_db = NULL;
        dmu_buf_t    *xa_data_db = NULL;
        uint64_t      xa_data_obj;
        vattr_t       va;
        int           error;

        if (zp->zp_xattr == 0) {
                udmu_zap_create(uos, &xa_zap_db, tx, FTAG);

                zp->zp_xattr = xa_zap_db->db_object;
                dmu_buf_will_dirty(db, tx);
        }

        error = zap_lookup(uos->os, zp->zp_xattr, name, sizeof(uint64_t), 1,
                           &xa_data_obj);
        if (error == 0) {
                if (fl & XATTR_CREATE) {
                        error = EEXIST;
                        goto out;
                }
                /*
                 * Entry already exists.
                 * We'll truncate the existing object.
                 */
                error = udmu_obj2dbuf(uos->os, xa_data_obj, &xa_data_db, FTAG);
                if (error)
                        goto out;

                error = udmu_object_punch_impl(uos->os, xa_data_db, tx, vallen, 0);
                if (error)
                        goto out;
        } else if (error == ENOENT) {
                /*
                 * Entry doesn't exist, we need to create a new one and a new
                 * object to store the value.
                 */
                if (fl & XATTR_REPLACE) {
                        /* should be ENOATTR according to the
                         * man, but that is undefined here */
                        error = ENODATA;
                        goto out;
                }
                udmu_object_create(uos, &xa_data_db, tx, FTAG);
                xa_data_obj = xa_data_db->db_object;
                error = zap_add(uos->os, zp->zp_xattr, name, sizeof(uint64_t), 1,
                                &xa_data_obj, tx);
                if (error)
                        goto out;
        } else {
                /* There was an error looking up the xattr name */
                goto out;
        }

        /* Finally write the xattr value */
        dmu_write(uos->os, xa_data_obj, 0, vallen, val, tx);

        va.va_size = vallen;
        va.va_mask = DMU_AT_SIZE;
        udmu_object_setattr(xa_data_db, tx, &va);

out:
        if (xa_data_db != NULL)
                udmu_object_put_dmu_buf(xa_data_db, FTAG);
        if (xa_zap_db != NULL)
                udmu_object_put_dmu_buf(xa_zap_db, FTAG);

        return error;
}

void udmu_xattr_declare_del(udmu_objset_t *uos, dmu_buf_t *db,
                            const char *name, dmu_tx_t *tx)
{
        znode_phys_t *zp = db->db_data;
        int error;
        uint64_t xa_data_obj;

        if (zp->zp_xattr == 0)
                return;

        error = zap_lookup(uos->os, zp->zp_xattr, name, sizeof(uint64_t), 1,
                           &xa_data_obj);
        if (error == 0) {
                /*
                 * Entry exists.
                 * We'll delete the existing object and ZAP entry.
                 */
                dmu_tx_hold_bonus(tx, xa_data_obj);
                dmu_tx_hold_free(tx, xa_data_obj, 0, DMU_OBJECT_END);
                dmu_tx_hold_zap(tx, zp->zp_xattr, FALSE, (char *) name);
                return;
        } else if (error == ENOENT) {
                /*
                 * Entry doesn't exist, nothing to be changed.
                 */
                return;
        }

        /* An error happened */
        tx->tx_err = error;
}

/*
 * Delete an extended attribute.
 * This transaction must have called udmu_xattr_declare_del() first.
 *
 * Returns 0 on success or a positive error number on failure.
 *
 * No locking is done here.
 */
int udmu_xattr_del(udmu_objset_t *uos, dmu_buf_t *db,
                   const char *name, dmu_tx_t *tx)
{
        znode_phys_t *zp = db->db_data;
        int error;
        uint64_t xa_data_obj;

        if (zp->zp_xattr == 0)
                return ENOENT;

        error = zap_lookup(uos->os, zp->zp_xattr, name, sizeof(uint64_t), 1,
                           &xa_data_obj);
        if (error == ENOENT) {
                error = 0;
                goto out;
        }

        if (error == 0) {
                /*
                 * Entry exists.
                 * We'll delete the existing object and ZAP entry.
                 */
                error = udmu_object_free(uos, xa_data_obj, tx);
                if (error)
                        goto out;

                error = zap_remove(uos->os, zp->zp_xattr, name, tx);
        }

out:
        return error;
}

int udmu_xattr_list(udmu_objset_t *uos, dmu_buf_t *db, void *buf, int buflen)
{
        znode_phys_t    *zp = db->db_data;
        char             key[MAXNAMELEN + 1];
        zap_cursor_t    *zc;
        int              rc;
        int              remain = buflen;
        int              counted = 0;

        if (zp->zp_xattr == 0)
                return 0;

        rc = udmu_zap_cursor_init(&zc, uos, zp->zp_xattr, 0);
        if (rc)
                return -rc;

        while ((rc = udmu_zap_cursor_retrieve_key(zc, key, MAXNAMELEN)) == 0) {
                rc = strlen(key);
                if (rc + 1 <= remain) {
                        memcpy(buf, key, rc);
                        buf += rc;
                        *((char *)buf) = '\0';
                        buf++;
                        remain -= rc + 1;
                }
                counted += rc + 1;
                udmu_zap_cursor_advance(zc);
        }

        udmu_zap_cursor_fini(zc);

        return counted;
}

void udmu_freeze(udmu_objset_t *uos)
{
        spa_freeze(dmu_objset_spa(uos->os));
}

void udmu_wait_callbacks(udmu_objset_t *uos)
{
        txg_wait_callbacks(spa_get_dsl(dmu_objset_spa(uos->os)));
}
