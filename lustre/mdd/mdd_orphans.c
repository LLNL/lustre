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
 * Copyright (c) 2011, 2012, Whamcloud, Inc.
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

#define DEBUG_SUBSYSTEM S_MDS

#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lustre_fid.h>
#include "mdd_internal.h"

const char orph_index_name[] = "PENDING";
const char *dotdot = "..";

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

int orph_declare_index_insert(const struct lu_env *env,
                              struct mdd_object *obj, struct thandle *th)
{
        struct mdd_device *mdd = mdo2mdd(&obj->mod_obj);
        int                rc;

        rc = mdo_declare_index_insert(env, mdd->mdd_orphans, NULL, NULL, th);
        if (rc)
                return rc;

        rc = mdo_declare_ref_add(env, obj, th);
        if (rc)
                return rc;

        if (!S_ISDIR(mdd_object_type(obj)))
                return 0;

        rc = mdo_declare_ref_add(env, obj, th);
        if (rc)
                return rc;

        rc = mdo_declare_index_delete(env, obj, dotdot, th);
        if (rc)
                return rc;

        rc = mdo_declare_index_insert(env, obj, NULL, dotdot, th);

        return rc;
}

/* ORPHANS dir nlink is not changed upon insert/delete */
int orph_index_insert(const struct lu_env *env, struct mdd_object *obj,
                      struct thandle *th)
{
        struct mdd_device  *mdd = mdo2mdd(&obj->mod_obj);
        const char         *key;
        int                 rc;

        ENTRY;

        LASSERT(mdd_write_locked(env, obj) != 0);
        LASSERT(!(obj->mod_flags & ORPHAN_OBJ));
        LASSERT(obj->mod_count > 0);

        key = orph_key_fill(env, mdo2fid(obj), ORPH_OP_UNLINK);
        rc = mdo_index_insert(env, mdd->mdd_orphans, mdo2fid(obj), key, th);
        if (rc)
                GOTO(out, rc);

        mdo_ref_add(env, obj, th);

        if (!S_ISDIR(mdd_object_type(obj)))
                goto out;

        mdo_ref_add(env, obj, th);

        /* try best to fixup directory, dont return errors from here */
        if (!dt_try_as_dir(env, mdd_object_child(obj)))
                goto out;

        /* unlinked directory is a child of orphan index now */
        mdo_index_delete(env, obj, "..", th);
        mdo_index_insert(env, obj, mdo2fid(mdd->mdd_orphans), "..", th);

        EXIT;
out:
        obj->mod_flags |= ORPHAN_OBJ;

        return rc;
}

int orph_declare_index_delete(const struct lu_env *env,
                              struct mdd_object *obj,
                              struct thandle *th)
{
        struct mdd_device *mdd = mdo2mdd(&obj->mod_obj);
        int                rc;

        rc = mdo_declare_index_delete(env, mdd->mdd_orphans, NULL, th);
        if (rc)
                return rc;

        rc = mdo_declare_ref_del(env, obj, th);
        if (rc)
                return rc;

        if (S_ISDIR(mdd_object_type(obj))) {
                rc = mdo_declare_ref_del(env, obj, th);
                if (rc)
                        return rc;
        }

        return rc;
}

int orph_index_delete(const struct lu_env *env, struct mdd_object *obj,
                      struct thandle *th)
{
        struct mdd_device *mdd = mdo2mdd(&obj->mod_obj);
        char              *key;
        int rc = 0;

        ENTRY;

        LASSERT(mdd_write_locked(env, obj) != 0);
        LASSERT(obj->mod_flags & ORPHAN_OBJ);
        LASSERT(obj->mod_count == 0);
        LASSERT(mdd->mdd_orphans);

        key = orph_key_fill(env, mdo2fid(obj), ORPH_OP_UNLINK);

        rc = mdo_index_delete(env, mdd->mdd_orphans, key, th);
        if (rc == -ENOENT) {
                key = orph_key_fill_18(env, mdo2fid(obj));
                rc = mdo_index_delete(env, mdd->mdd_orphans, key, th);
        }

        if (rc == 0) {
                mdo_ref_del(env, obj, th);
                if (S_ISDIR(mdd_object_type(obj)))
                        mdo_ref_del(env, obj, th);
                obj->mod_flags &= ~ORPHAN_OBJ;
        } else {
                CERROR("could not delete object: rc = %d\n",rc);
        }

        RETURN(rc);
}

/**
 * Delete unused orphan with FID \a lf from PENDING directory
 *
 * \param mdd  MDD device finishing recovery
 * \param lf   FID of file or directory to delete
 * \param key  cookie for this entry in index iterator
 *
 * \retval 0   success
 * \retval -ve error
 */
static int orph_key_test_and_del(const struct lu_env *env,
                                 struct mdd_device *mdd,
                                 struct lu_fid *lf,
                                 char *key)
{
        struct thandle *th = NULL;
        struct mdd_object  *mdo;
        int rc = -EBUSY;

        mdo = mdd_object_find(env, mdd, lf);
        if (IS_ERR(mdo))
                return PTR_ERR(mdo);

        OBD_RACE(OBD_FAIL_MDS_ORPHAN_RACE);

        if (mdo->mod_count > 0) {
                mdd_write_lock(env, mdo, MOR_TGT_CHILD);
                CDEBUG(D_HA, "Found orphan "DFID", open count = %d\n",
                       PFID(lf), mdo->mod_count);
                mdo->mod_flags |= ORPHAN_OBJ;
                mdd_write_unlock(env, mdo);
        } else if (mdo->mod_flags & ORPHAN_OBJ) {
                CWARN("Found orphan "DFID"! Delete it\n", PFID(lf));

                th = mdd_trans_create(env, mdd);
                if (IS_ERR(th))
                        return -ENOMEM;

                rc = orph_declare_index_delete(env, mdo, th);
                if (rc)
                        goto stop;

                rc = mdo_declare_destroy(env, mdo, th);
                if (rc)
                        goto stop;

                rc = mdd_trans_start(env, mdd, th);
                if (rc)
                        goto stop;

                mdd_write_lock(env, mdo, MOR_TGT_CHILD);
                if (likely(mdo->mod_count == 0)) {
                        rc = orph_index_delete(env, mdo, th);
                        if (rc == 0)
                                rc = mdo_destroy(env, mdo, th);
                        else
                                CERROR("%s: error unlinking orphan "DFID" "
                                       "from PENDING: rc = %d\n",
                                       mdd2obd_dev(mdd)->obd_name, PFID(lf),
                                       rc);
                }
                mdd_write_unlock(env, mdo);
stop:
                mdd_trans_stop(env, mdd, 0, th);
        }
        mdd_object_put(env, mdo);
        return rc;
}

/**
 * delete unreferenced files and directories in the PENDING directory
 *
 * Files that remain in PENDING after client->MDS recovery has completed
 * have to be referenced (opened) by some client during recovery, or they
 * will be deleted here (for clients that did not complete recovery).
 *
 * \param mdd  MDD device finishing recovery
 *
 * \retval 0   success
 * \retval -ve error
 */
static int orph_index_iterate(const struct lu_env *env,
			      struct mdd_device *mdd)
{
	struct dt_object	*dor = mdd_object_child(mdd->mdd_orphans);
	struct lu_dirent	*ent = &mdd_env_info(env)->mti_orph_ent;
	const struct dt_it_ops	*iops;
	struct dt_it		*it;
	struct lu_fid		 fid;
	int			 key_sz = 0;
	int			 rc;
	__u64			 cookie;

        ENTRY;

        /* In recovery phase, do not need for any lock here */
        iops = &dor->do_index_ops->dio_it;
        it = iops->init(env, dor, LUDA_64BITHASH, BYPASS_CAPA);
        if (IS_ERR(it)) {
                rc = PTR_ERR(it);
                CERROR("%s: cannot clean PENDING: rc = %d\n",
                       mdd2obd_dev(mdd)->obd_name, rc);
                GOTO(out, rc);
        }

        rc = iops->load(env, it, 0);
        if (rc < 0)
                GOTO(out_put, rc);
        if (rc == 0) {
                CERROR("%s: error loading iterator to clean PENDING\n",
                       mdd2obd_dev(mdd)->obd_name);
                /* Index contains no zero key? */
                GOTO(out_put, rc = -EIO);
        }

	do {
		key_sz = iops->key_size(env, it);
		/* filter out "." and ".." entries from PENDING dir. */
		if (key_sz < 8)
			goto next;

		rc = iops->rec(env, it, (struct dt_rec *)ent, LUDA_64BITHASH);
		if (rc != 0) {
			CERROR("%s: fail to get FID for orphan it: rc = %d\n",
			       mdd2obd_dev(mdd)->obd_name, rc);
			goto next;
		}

		fid_le_to_cpu(&fid, &ent->lde_fid);
		if (!fid_is_sane(&fid)) {
			CERROR("%s: bad FID "DFID" cleaning PENDING\n",
			       mdd2obd_dev(mdd)->obd_name, PFID(&fid));
			goto next;
		}

		/* kill orphan object */
		cookie = iops->store(env, it);
		iops->put(env, it);
		rc = orph_key_test_and_del(env, mdd, &fid, ent->lde_name);

		/* after index delete reset iterator */
		if (rc == 0)
			rc = iops->get(env, it, (const void *)"");
		else
			rc = iops->load(env, it, cookie);
next:
		rc = iops->next(env, it);
	} while (rc == 0);

	GOTO(out_put, rc = 0);
out_put:
	iops->put(env, it);
	iops->fini(env, it);

out:
	return rc;
}

/**
 * open the PENDING directory for device \a mdd
 *
 * The PENDING directory persistently tracks files and directories that were
 * unlinked from the namespace (nlink == 0) but are still held open by clients.
 * Those inodes shouldn't be deleted if the MDS crashes, because the clients
 * would not be able to recover and reopen those files.  Instead, these inodes
 * are linked into the PENDING directory on disk, and only deleted if all
 * clients close them, or the MDS finishes client recovery without any client
 * reopening them (i.e. former clients didn't join recovery).
 *  \param d   mdd device being started.
 *
 *  \retval 0  success
 *  \retval  -ve index operation error.
 *
 */
int orph_index_init(const struct lu_env *env, struct mdd_device *mdd)
{
        struct lu_attr *la = &mdd_env_info(env)->mti_la;
        int rc = 0;

        ENTRY;

        LASSERT(mdd->mdd_orphans != NULL);

        /*
         * to avoid contention on orphan's nlink,
         * make it 1 and do not modify any more
         */
        rc = mdo_attr_get(env, mdd->mdd_orphans, la, BYPASS_CAPA);
        if (rc == 0 && la->la_nlink != 1) {
                struct thandle *th;

                th = mdd_trans_create(env, mdd);
                if (IS_ERR(th))
                        return -ENOMEM;

                la->la_nlink = 1;
                la->la_valid = LA_NLINK;

                rc = mdo_declare_attr_set(env, mdd->mdd_orphans, la, th);
                if (rc)
                        goto stop;

                rc = mdd_trans_start(env, mdd, th);
                if (rc)
                        goto stop;

                mdd_write_lock(env, mdd->mdd_orphans, MOR_TGT_CHILD);
                rc = mdo_attr_set(env, mdd->mdd_orphans, la, th, BYPASS_CAPA);
                mdd_write_unlock(env, mdd->mdd_orphans);
stop:
                mdd_trans_stop(env, mdd, 0, th);
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
 *  Iterate orphan index to cleanup orphan objects after recovery is done.
 *  \param d   mdd device in recovery.
 */
int __mdd_orphan_cleanup(const struct lu_env *env, struct mdd_device *d)
{
        return orph_index_iterate(env, d);
}

