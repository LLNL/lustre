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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mdd/mdd_orphans.c
 *
 * Orphan handling code
 *
 * Author: Mike Pershin <tappro@clusterfs.com>
 *         Pravin B Shelar <pravin.shelar@sun.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lustre_fid.h>
#include "mdd_internal.h"

const char orph_index_name[] = "PENDING";
static const char dotdot[] = "..";

enum {
        ORPH_OP_UNLINK,
        ORPH_OP_TRUNCATE
};

#define ORPHAN_FILE_NAME_FORMAT         "%016llx:%08x:%08x:%2x"
#define ORPHAN_FILE_NAME_FORMAT_18      "%llx:%08x"

static char *orph_key_fill(const struct lu_env *env, const struct lu_fid *lf, __u32 op)
{
        char *key = mdd_env_info(env)->mti_orph_key;
        int rc;

        LASSERT(key);
        rc = snprintf(key, NAME_MAX + 1, ORPHAN_FILE_NAME_FORMAT,
                      (long long unsigned int)fid_seq(lf),
                      fid_oid(lf), fid_ver(lf), op);
        if (rc > 0)
                return key;
        else
                return ERR_PTR(rc);
}

static char *orph_key_fill_18(const struct lu_env *env,
                                       const struct lu_fid *lf)
{
        char *key = mdd_env_info(env)->mti_orph_key;
        int rc;

        LASSERT(key);
        rc = snprintf(key, NAME_MAX + 1, ORPHAN_FILE_NAME_FORMAT_18,
                      (unsigned long long)fid_seq(lf), fid_oid(lf));
        if (rc > 0)
                return key;
        else
                return ERR_PTR(rc);
}

static int orphan_key_to_fid(char *key, struct lu_fid *lf)
{
        int rc = 0;
        unsigned int op;

        rc = sscanf(key, ORPHAN_FILE_NAME_FORMAT,
                    (long long unsigned int *)&lf->f_seq, &lf->f_oid,
                    &lf->f_ver, &op);
        if (rc == 4)
                return 0;

        /* build igif */
        rc = sscanf(key, ORPHAN_FILE_NAME_FORMAT_18,
                    (long long unsigned int *)&lf->f_seq, &lf->f_oid);
        if (rc == 2) {
                lf->f_ver = 0;
                return 0;
        }

        CERROR("can not parse orphan file name %s\n",key);
        return -EINVAL;
}

static void orph_index_insert(const struct lu_env *env,
                              struct mdd_object *obj,
                              __u32 op,
                              struct mdd_thandle *th)
{
        struct mdd_device  *mdd    = mdo2mdd(&obj->mod_obj);
        const char         *key;
        ENTRY;

        LASSERT(mdd_write_locked(env, obj) != 0);
        LASSERT(!(obj->mod_flags & ORPHAN_OBJ));
        LASSERT(obj->mod_count > 0);

        key = orph_key_fill(env, mdo2fid(obj), ORPH_OP_UNLINK);
        mdd_tx_idx_insert(env, mdd->mdd_orphans, mdo2fid(obj), key, th);
        mdd_tx_ref_add(env, obj, th);

        if (!S_ISDIR(mdd_object_type(obj)))
                goto out;

        mdd_tx_ref_add(env, obj, th);
        //mdd_tx_ref_add(env, mdd->mdd_orphans, th);

        /* try best to fixup directory, dont return errors from here */
        if (!dt_try_as_dir(env, mdd_object_child(obj)))
                goto out;

        /* unlinked directory is a child of orphan index now */
        mdd_tx_idx_delete(env, obj, "..", th);
        mdd_tx_idx_insert(env, obj, mdo2fid(mdd->mdd_orphans), "..", th);

out:
        obj->mod_flags |= ORPHAN_OBJ;

        EXIT;
}

/**
 * destroy osd object on mdd and associated ost objects.
 *
 * \param obj orphan object
 * \param mdd used for sending llog msg to osts
 *
 * \retval  0   success
 * \retval -ve  error
 */
static void orphan_object_kill(const struct lu_env *env,
                               struct mdd_object *obj,
                               struct mdd_device *mdd,
                               struct mdd_thandle *th)
{
        ENTRY;

        /* No need to lock this object as its recovery phase, and
         * no other thread can access it. But we need to lock it
         * as its precondition for osd api we using. */

        mdd_tx_ref_del(env, obj, th);
        if (S_ISDIR(mdd_object_type(obj)))
                mdd_tx_ref_del(env, obj, th);
        mdd_tx_destroy(env, obj, th);

        EXIT;
}

static int orph_index_delete(const struct lu_env *env,
                             struct mdd_object *obj,
                             __u32 op,
                             struct mdd_thandle *th)
{
        struct mdd_device *mdd = mdo2mdd(&obj->mod_obj);
        char              *key;
        int rc = 0;

        ENTRY;

        LASSERT(mdd_write_locked(env, obj) != 0);
        LASSERT(obj->mod_flags & ORPHAN_OBJ);
        LASSERT(obj->mod_count == 0);
        LASSERT(mdd->mdd_orphans);

        key = orph_key_fill(env, mdo2fid(obj), op);

        mdd_tx_idx_delete(env, mdd->mdd_orphans, key, th);

        if (rc == -ENOENT) {
                /* XXX: none of ops can fail in new model */
                LBUG();
                key = orph_key_fill_18(env, mdo2fid(obj));
                mdd_tx_idx_delete(env, mdd->mdd_orphans, key, th);
        }

        if (!rc) {
                /* lov objects will be destroyed by caller */
                mdd_tx_ref_del(env, obj, th);
                if (S_ISDIR(mdd_object_type(obj)))
                        mdd_tx_ref_del(env, obj, th);
                obj->mod_flags &= ~ORPHAN_OBJ;
        } else {
                CERROR("could not delete object: rc = %d\n",rc);
        }

        RETURN(rc);
}


static int orph_key_test_and_del(const struct lu_env *env,
                                 struct mdd_device *mdd,
                                 struct lu_fid *lf,
                                 char *key)
{
        struct mdd_thandle *th = NULL;
        struct mdd_object  *mdo;
        int rc = -EBUSY;

        mdo = mdd_object_find(env, mdd, lf);
        if (IS_ERR(mdo))
                return PTR_ERR(mdo);

        OBD_RACE(OBD_FAIL_MDS_ORPHAN_RACE);
        mdd_write_lock(env, mdo, MOR_TGT_CHILD);
        if (mdo->mod_count > 0) {
                CDEBUG(D_HA, "Found orphan, open count = %d\n", mdo->mod_count);
                mdo->mod_flags |= ORPHAN_OBJ;
        } else if (mdo->mod_flags & ORPHAN_OBJ) {
                CWARN("Found orphan! Delete it\n");
                th = mdd_tx_start(env, mdd);
                if (!IS_ERR(th)) {
                        mdd_tx_idx_delete(env, mdd->mdd_orphans, key, th);
                        orphan_object_kill(env, mdo, mdd, th);
                        rc = mdd_tx_end(env, th);
                        if (rc)
                                CERROR("can't remove orphan: %d\n", rc);
                }
        }
        mdd_write_unlock(env, mdo);
        mdd_object_put(env, mdo);

        return rc;
}

static int orph_index_iterate(const struct lu_env *env,
                              struct mdd_device *mdd)
{
        struct dt_object *dor = mdd_object_child(mdd->mdd_orphans);
        char             *mti_key = mdd_env_info(env)->mti_orph_key;
        const struct dt_it_ops *iops;
        struct dt_it     *it;
        char             *key;
        struct lu_fid     fid;
        int               result = 0;
        int               key_sz = 0;
        int               rc;
        __u64             cookie;
        ENTRY;

        /* In recovery phase, do not need for any lock here */

        iops = &dor->do_index_ops->dio_it;
        it = iops->init(env, dor, LUDA_64BITHASH, BYPASS_CAPA);
        if (!IS_ERR(it)) {
                result = iops->load(env, it, 0);
                if (result > 0) {
                        /* main cycle */
                        do {

                                key = (void *)iops->key(env, it);
                                if (IS_ERR(key)) {
                                        CERROR("key failed when clean pending.\n");
                                        goto next;
                                }
                                key_sz = iops->key_size(env, it);

                                /* filter out "." and ".." entries from
                                 * PENDING dir. */
                                if (key_sz < 8)
                                        goto next;

                                memcpy(mti_key, key, key_sz);
                                mti_key[key_sz] = 0;

                                if (orphan_key_to_fid(mti_key, &fid))
                                        goto next;
                                if (!fid_is_sane(&fid)) {
                                        CERROR("fid is not sane when clean pending.\n");
                                        goto next;
                                }

                                /* kill orphan object */
                                cookie =  iops->store(env, it);
                                iops->put(env, it);
                                rc = orph_key_test_and_del(env, mdd, &fid, mti_key);

                                /* after index delete reset iterator */
                                if (!rc)
                                        result = iops->get(env, it,
                                                           (const void *)"");
                                else
                                        result = iops->load(env, it, cookie);
next:
                                result = iops->next(env, it);
                        } while (result == 0);
                        result = 0;
                } else if (result == 0) {
                        CERROR("Input/Output for clean pending.\n");
                        /* Index contains no zero key? */
                        result = -EIO;
                }
                iops->put(env, it);
                iops->fini(env, it);
        } else {
                result = PTR_ERR(it);
                CERROR("Cannot clean pending (%d).\n", result);
        }

        RETURN(result);
}

int orph_index_init(const struct lu_env *env, struct mdd_device *mdd)
{
        struct lu_fid fid;
        struct dt_object *d;
        int rc = 0;
        ENTRY;

        d = dt_store_open(env, mdd->mdd_child, "", orph_index_name, &fid);
        if (!IS_ERR(d)) {
                struct lu_object *l;
                l = lu_object_locate(d->do_lu.lo_header,
                                     mdd->mdd_md_dev.md_lu_dev.ld_type);
                LASSERT(l);
                mdd->mdd_orphans = lu2mdd_obj(l);
                if (!dt_try_as_dir(env, d)) {
                        rc = -ENOTDIR;
                        CERROR("\"%s\" is not an index! : rc = %d\n",
                                        orph_index_name, rc);
                }
        } else {
                CERROR("cannot find \"%s\" obj %d\n",
                       orph_index_name, (int)PTR_ERR(d));
                rc = PTR_ERR(d);
        }

        /*
         * to avoid contention on orphan's nlink,
         * make it 1 and do not modify any more
         */
        if (rc == 0 && !(mdd->mdd_orphans->mod_flags & MNLINK_OBJ)) {
                struct lu_attr *la = &mdd_env_info(env)->mti_la;
                struct mdd_thandle *th;

                mdd_write_lock(env, mdd->mdd_orphans, MOR_TGT_CHILD);
                th = mdd_tx_start(env, mdd);
                if (!IS_ERR(th)) {
                        mdd->mdd_orphans->mod_flags |= MNLINK_OBJ;
                        la->la_nlink = 1;
                        la->la_valid = LA_NLINK;
                        mdd_tx_attr_set(env, mdd->mdd_orphans, la, th);
                        rc = mdd_tx_end(env, th);
                }
                mdd_write_unlock(env, mdd->mdd_orphans);
        }
        
        RETURN(rc);
}

void orph_index_fini(const struct lu_env *env, struct mdd_device *mdd)
{
        ENTRY;
        if (mdd->mdd_orphans != NULL) {
                mdd_object_put(env, mdd->mdd_orphans);
                mdd->mdd_orphans = NULL;
        }
        EXIT;
}

/**
 *  Iterate orphan index to cleanup orphan objects in case of recovery.
 *  \param d   mdd device in recovery.
 *
 */

int __mdd_orphan_cleanup(const struct lu_env *env, struct mdd_device *d)
{
        return orph_index_iterate(env, d);
}

/**
 *  delete an orphan \a obj from orphan index.
 *  \param obj file or directory.
 *  \param th  transaction for index insert.
 *
 *  \pre obj nlink == 0 && obj->mod_count != 0
 *
 *  \retval 0  success
 *  \retval  -ve index operation error.
 */

void __mdd_orphan_add(const struct lu_env *env,
                     struct mdd_object *obj, struct mdd_thandle *th)
{
        orph_index_insert(env, obj, ORPH_OP_UNLINK, th);
}

/**
 *  delete an orphan \a obj from orphan index.
 *  \param obj file or directory.
 *  \param th  transaction for index deletion and object destruction.
 *
 *  \pre obj->mod_count == 0 && ORPHAN_OBJ is set for obj.
 *
 *  \retval 0  success
 *  \retval  -ve index operation error.
 */

void __mdd_orphan_del(const struct lu_env *env,
                     struct mdd_object *obj, struct mdd_thandle *th)
{
        orph_index_delete(env, obj, ORPH_OP_UNLINK, th);
}
