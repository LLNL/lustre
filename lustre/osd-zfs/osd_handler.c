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
 * lustre/osd/osd_handler.c
 * Top-level entry points into osd module
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 * Author: Alex Tomas <alex@clusterfs.com>
 * Author: Mike Pershin <tappro@sun.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <lustre_ver.h>
#include <libcfs/libcfs.h>
#include <lustre_fsfilt.h>
#include <obd_support.h>
#include <lustre_net.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_disk.h>
#include <lustre_fid.h>
#include <lu_quota.h>

#include "osd_internal.h"

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

struct lu_context_key               osd_key;
static struct lu_object_operations  osd_lu_obj_ops;
static struct dt_object_operations  osd_obj_ops;
static struct dt_body_operations    osd_body_ops;

static char *osd_obj_tag = "osd_object";
static char *root_tag = "osd_mount, rootdb";
static char *objdir_tag = "osd_mount, objdb";

/* Slab for OSD object allocation */
static cfs_mem_cache_t *osd_object_kmem;

static struct lu_kmem_descr osd_caches[] = {
        {
                .ckd_cache = &osd_object_kmem,
                .ckd_name  = "zfs_osd_obj",
                .ckd_size  = sizeof(struct osd_object)
        },
        {
                .ckd_cache = NULL
        }
};

/*
 * Retrieve the attributes of a DMU object
 */
int __osd_object_attr_get(const struct lu_env *env, udmu_objset_t *uos,
                          sa_handle_t *sa_hdl, struct lu_attr *la)
{
        struct osa_attr *osa = &osd_oti_get(env)->oti_osa;
        sa_bulk_attr_t  *bulk;
        int              cnt = 0;
        int              rc;

        ENTRY;
        LASSERT(sa_hdl != NULL);
        LASSERT(sa_hdl->sa_bonus != NULL);
        LASSERT(sa_hdl->sa_os == uos->os);

        OBD_ALLOC(bulk, sizeof(sa_bulk_attr_t) * 9);
        if (bulk == NULL)
                RETURN(-ENOMEM);

        la->la_valid |= LA_ATIME | LA_MTIME | LA_CTIME | LA_MODE | LA_TYPE |
                        LA_SIZE | LA_UID | LA_GID | LA_FLAGS | LA_NLINK;

        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_ATIME(uos), NULL, osa->atime, 16);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MTIME(uos), NULL, osa->mtime, 16);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_CTIME(uos), NULL, osa->ctime, 16);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MODE(uos), NULL, &osa->mode, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_SIZE(uos), NULL, &osa->size, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_LINKS(uos), NULL, &osa->nlink, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_UID(uos), NULL, &osa->uid, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_GID(uos), NULL, &osa->gid, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_FLAGS(uos), NULL, &osa->flags, 8);

        rc = -sa_bulk_lookup(sa_hdl, bulk, cnt);
        if (rc) {
                LCONSOLE_ERROR("Failed to get bulk of attrs %d\n", rc);
                dump_stack();
                RETURN(rc);
        }
        OBD_FREE(bulk, sizeof(sa_bulk_attr_t) * 9);

        la->la_atime = osa->atime[0];
        la->la_mtime = osa->mtime[0];
        la->la_ctime = osa->ctime[0];
        la->la_mode = osa->mode;
        la->la_uid = osa->uid;
        la->la_gid = osa->gid;
        la->la_nlink = osa->nlink;
        la->la_flags = osa->flags;
        la->la_size = osa->size;

        if (S_ISCHR(la->la_mode) || S_ISBLK(la->la_mode)) {
                rc = -sa_lookup(sa_hdl, SA_ZPL_RDEV(uos), &osa->rdev, 8);
                if (rc)
                        RETURN(rc);
                la->la_rdev = osa->rdev;
                la->la_valid |= LA_RDEV;
        }

        RETURN(0);
}

/*
 * zfs osd maintains names for known fids in the name hierarchy
 * so that one can mount filesystem with regular ZFS stack and
 * access files
 */
struct named_oid {
        unsigned long  oid;
        char          *name;
};

static const struct named_oid oids[] = {
        { OFD_LAST_RECV_OID,            LAST_RCVD },
        { OFD_LAST_GROUP_OID,           "LAST_GROUP" },
        { LLOG_CATALOGS_OID,            "CATALOGS" },
        { MGS_CONFIGS_OID,              MOUNT_CONFIGS_DIR },
        { FID_SEQ_SRV_OID,              "seq_srv" },
        { FID_SEQ_CTL_OID,              "seq_ctl" },
        { MDD_CAPA_KEYS_OID,            CAPA_KEYS },
        { FLD_INDEX_OID,                "fld" },
        { MDD_LOV_OBJ_OID,              LOV_OBJID },
        { MDT_LAST_RECV_OID,            LAST_RCVD },
        { OFD_HEALTH_CHECK_OID,         HEALTH_CHECK },
        { OFD_GROUP0_LAST_OID,           "LAST_ID" },
        { MDD_ROOT_INDEX_OID,           NULL },
        { MDD_ORPHAN_OID,               NULL },
        { 0,                            NULL }
};

static char *oid2name(const unsigned long oid)
{
        int i = 0;

        while (oids[i].oid) {
                if (oids[i].oid == oid)
                        return oids[i].name;
                i++;
        }
        return NULL;
}

/*
 * objects w/o a natural reference (unlike a file on a MDS)
 * are put under a special hierarchy /O/<seq>/d0..dXX
 * this function returns a directory specific fid belongs to
 */
static uint64_t osd_get_idx_for_ost_obj(const struct lu_env *env,
                                        struct osd_device *osd,
                                        const struct lu_fid *fid,
                                        char *buf)
{
        unsigned long b;
        int           rc;

        rc = fid_ostid_pack(fid, &osd_oti_get(env)->oti_ostid);
        LASSERT(rc == 0); /* we should not get here with IGIF */
        b = osd_oti_get(env)->oti_ostid.oi_id % OSD_OST_MAP_SIZE;
        LASSERT(osd->od_ost_compat_dirs[b]);

        sprintf(buf, LPU64, osd_oti_get(env)->oti_ostid.oi_id);

        return osd->od_ost_compat_dirs[b];
}

/* XXX: f_ver is not counted, but may differ too */
static void osd_fid2str(char *buf, const struct lu_fid *fid)
{
        sprintf(buf, DFID_NOBRACE, PFID(fid));
}

static uint64_t osd_get_name_n_idx(const struct lu_env *env,
                                   struct osd_device *osd,
                                   const struct lu_fid *fid,
                                   char *buf)
{
        uint64_t zapid;

        LASSERT(fid);
        LASSERT(buf);

        if (fid_is_idif(fid)) {
                zapid = osd_get_idx_for_ost_obj(env, osd, fid, buf);
        } else if (unlikely(fid_seq(fid) == FID_SEQ_LOCAL_FILE)) {
                /* special objects with fixed known fids get their name */
                char *name = oid2name(fid_oid(fid));
                if (name) {
                        zapid = osd->od_root;
                        strcpy(buf, name);
                        if (fid_oid(fid) == OFD_GROUP0_LAST_OID)
                                zapid = osd->od_ost_compat_grp0;
                } else {
                        CERROR("unknown object with fid "DFID"\n", PFID(fid));
                        osd_fid2str(buf, fid);
                        zapid = osd->od_objdir;
                }
        } else {
                osd_fid2str(buf, fid);
                zapid = osd->od_objdir;
        }

        return zapid;
}

int __osd_obj2dbuf(const struct lu_env *env, objset_t *os,
                   uint64_t oid, dmu_buf_t **dbp, void *tag)
{
        dmu_object_info_t *doi = &osd_oti_get(env)->oti_doi;
        int rc;

        LASSERT(tag);

        rc = -sa_buf_hold(os, oid, tag, dbp);
        if (rc)
                return rc;

        dmu_object_info_from_db(*dbp, doi);
        if (unlikely (oid != DMU_USERUSED_OBJECT &&
                      oid != DMU_GROUPUSED_OBJECT &&
                      doi->doi_bonus_type != DMU_OT_SA)) {
                sa_buf_rele(*dbp, tag);
                *dbp = NULL;
                return -EINVAL;
        }

        LASSERT(*dbp);
        LASSERT((*dbp)->db_object == oid);
        LASSERT((*dbp)->db_offset == -1);
        LASSERT((*dbp)->db_data != NULL);

        return 0;
}

int osd_object_sa_init(struct osd_object *obj, udmu_objset_t *uos)
{
        LASSERT(obj->oo_sa_hdl == NULL);
        LASSERT(obj->oo_db != NULL);

        return -sa_handle_get(uos->os, obj->oo_db->db_object, obj,
                              SA_HDL_PRIVATE, &obj->oo_sa_hdl);
}

void osd_object_sa_fini(struct osd_object *obj)
{
        LASSERT(obj->oo_sa_hdl != NULL);

        sa_handle_destroy(obj->oo_sa_hdl);
        obj->oo_sa_hdl = NULL;
}

static inline int fid_is_fs_root(const struct lu_fid *fid)
{
        /* Map root inode to special local object FID */
        return fid_seq(fid) == FID_SEQ_LOCAL_FILE &&
               fid_oid(fid) == OSD_FS_ROOT_OID;
}

static int osd_fid_lookup(const struct lu_env *env,
                          struct osd_object *obj, const struct lu_fid *fid)
{
        struct osd_thread_info *info = osd_oti_get(env);
        char                   *buf = info->oti_str;
        struct osd_device      *dev;
        uint64_t                oid, zapid;
        int                     rc = 0;
        ENTRY;

        LASSERT(osd_invariant(obj));

        dev  = osd_dev(obj->oo_dt.do_lu.lo_dev);

        if (OBD_FAIL_CHECK(OBD_FAIL_OST_ENOENT))
                RETURN(-ENOENT);

        LASSERT(obj->oo_db == NULL);

        if (unlikely(fid_is_acct(fid))) {
                /* DMU_USERUSED_OBJECT & DMU_GROUPUSED_OBJECT are special
                 * objects which have no d_buf_t structure.
                 * As a consequence, udmu_object_get_dmu_buf() gets a fake
                 * buffer which is good enough to pass all the checks done
                 * during object creation, but this buffer should really not
                 * be used by osd_quota.c */
                oid = osd_quota_fid2dmu(fid);

        } else if (unlikely(fid_is_fs_root(fid))) {
                oid = dev->od_root;

        } else {
                zapid = osd_get_name_n_idx(env, dev, fid, buf);

                rc = -zap_lookup(dev->od_objset.os, zapid, buf,
                                 8, 1, &info->oti_zde);
                if (rc)
                        RETURN(rc);
                oid = info->oti_zde.lzd_reg.zde_dnode;
        }

        rc = __osd_obj2dbuf(env, dev->od_objset.os, oid,
                            &obj->oo_db, osd_obj_tag);
        if (rc == 0) {
                LASSERT(obj->oo_db != NULL);
        } else if (rc == -ENOENT) {
                LASSERT(obj->oo_db == NULL);
                rc = 0;
        } else {
                osd_fid2str(buf, fid);
                CERROR("Error during lookup %s/"LPX64": %d\n", buf, oid, rc);
        }

        LASSERT(osd_invariant(obj));
        RETURN(rc);
}

/*
 * Concurrency: doesn't access mutable data
 */
static int osd_root_get(const struct lu_env *env,
                        struct dt_device *dev, struct lu_fid *f)
{
        lu_local_obj_fid(f, OSD_FS_ROOT_OID);
        return 0;
}

/*
 * OSD object methods.
 */

/*
 * Concurrency: no concurrent access is possible that early in object
 * life-cycle.
 */
static struct lu_object *osd_object_alloc(const struct lu_env *env,
                                          const struct lu_object_header *hdr,
                                          struct lu_device *d)
{
        struct osd_object *mo;

        OBD_SLAB_ALLOC_PTR_GFP(mo, osd_object_kmem, CFS_ALLOC_IO);
        if (mo != NULL) {
                struct lu_object *l;

                l = &mo->oo_dt.do_lu;
                dt_object_init(&mo->oo_dt, NULL, d);
                mo->oo_dt.do_ops = &osd_obj_ops;
                l->lo_ops = &osd_lu_obj_ops;
                cfs_init_rwsem(&mo->oo_sem);
                cfs_sema_init(&mo->oo_guard, 1);
                cfs_rwlock_init(&mo->oo_attr_lock);
                return l;
        } else
                return NULL;
}

/*
 * Concurrency: shouldn't matter.
 */
int osd_object_init0(const struct lu_env *env, struct osd_object *obj)
{
        struct osd_device   *osd = osd_obj2dev(obj);
        udmu_objset_t       *uos = &osd->od_objset;
        const struct lu_fid *fid  = lu_object_fid(&obj->oo_dt.do_lu);
        int                  rc = 0;
        ENTRY;

        if (obj->oo_db == NULL)
                RETURN(0);

        /* object exist */

        if (unlikely(fid_is_acct(fid)))
                GOTO(out, rc = 0);

        rc = osd_object_sa_init(obj, uos);
        if (rc)
                RETURN(rc);

        /* cache attrs in object */
        rc = __osd_object_attr_get(env, &osd->od_objset,
                                   obj->oo_sa_hdl, &obj->oo_attr);
        if (rc)
                RETURN(rc);

        obj->oo_dt.do_body_ops = &osd_body_ops;

out:
        /*
         * initialize object before marking it existing
         */
        obj->oo_dt.do_lu.lo_header->loh_attr |= obj->oo_attr.la_mode & S_IFMT;

        cfs_mb();
        obj->oo_dt.do_lu.lo_header->loh_attr |= LOHA_EXISTS;

        RETURN(0);
}

/*
 * Concurrency: no concurrent access is possible that early in object
 * life-cycle.
 */
static int osd_object_init(const struct lu_env *env, struct lu_object *l,
                           const struct lu_object_conf *conf)
{
        struct osd_object *obj = osd_obj(l);
        int result;
        ENTRY;

        LASSERT(osd_invariant(obj));

        result = osd_fid_lookup(env, obj, lu_object_fid(l));
        if (result == 0)
                result = osd_object_init0(env, obj);
        else if (result == -ENOENT)
                result = 0;
        LASSERT(osd_invariant(obj));
        RETURN(result);
}

/*
 * Concurrency: no concurrent access is possible that late in object
 * life-cycle.
 */
static void osd_object_free(const struct lu_env *env, struct lu_object *l)
{
        struct osd_object *obj = osd_obj(l);

        LASSERT(osd_invariant(obj));

        dt_object_fini(&obj->oo_dt);
        OBD_SLAB_FREE_PTR(obj, osd_object_kmem);
}

/*
 * Concurrency: shouldn't matter.
 */
static void osd_trans_commit_cb(void *cb_data, int error)
{
        struct osd_thandle *oh = cb_data;
        struct thandle     *th = &oh->ot_super;
        struct lu_device   *lud = &th->th_dev->dd_lu_dev;
        struct dt_txn_commit_cb *dcb, *tmp;

        ENTRY;

        if (error) {
                if (error == ECANCELED)
                        CWARN("transaction @0x%p was aborted\n", th);
                else
                        CERROR("transaction @0x%p commit error: %d\n",
                               th, error);
        }

        dt_txn_hook_commit(th);

        /* call per-transaction callbacks if any */
        cfs_list_for_each_entry_safe(dcb, tmp, &oh->ot_dcb_list, dcb_linkage)
                dcb->dcb_func(NULL, th, dcb, error);

        lu_device_put(lud);
        th->th_dev = NULL;
        lu_context_exit(&th->th_ctx);
        lu_context_fini(&th->th_ctx);
        OBD_FREE_PTR(oh);

        EXIT;
}

static int osd_trans_cb_add(struct thandle *th, struct dt_txn_commit_cb *dcb)
{
        struct osd_thandle *oh = container_of0(th, struct osd_thandle,
                                               ot_super);

        cfs_list_add(&dcb->dcb_linkage, &oh->ot_dcb_list);

        return 0;
}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_trans_start(const struct lu_env *env, struct dt_device *d,
                           struct thandle *th)
{
        struct osd_thandle *oh;
        int                 rc;
        ENTRY;

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh);
        LASSERT(oh->ot_tx);

        rc = dt_txn_hook_start(env, d, th);
        if (rc != 0)
                RETURN(rc);

        if (oh->ot_write_commit && OBD_FAIL_CHECK(OBD_FAIL_OST_MAPBLK_ENOSPC))
                /* Unlike ldiskfs, ZFS checks for available space and returns
                 * -ENOSPC when assigning txg */
                RETURN(-ENOSPC);

        rc = -dmu_tx_assign(oh->ot_tx, TXG_WAIT);
        if (unlikely(rc != 0)) {
                struct osd_device *osd = osd_dt_dev(d);
                /* dmu will call commit callback with error code during abort */
                if (!lu_device_is_md(&d->dd_lu_dev) && rc == ENOSPC)
                        CERROR("%s: failed to start transaction due to ENOSPC. "
                               "Metadata overhead is underestimated or "
                               "grant_ratio is too low.\n",
                               osd->od_dt_dev.dd_lu_dev.ld_obd->obd_name);
                else
                        CERROR("%s: can't assign tx: %d\n",
                               osd->od_dt_dev.dd_lu_dev.ld_obd->obd_name, -rc);
        } else {
                /* add commit callback */
                dmu_tx_callback_register(oh->ot_tx, osd_trans_commit_cb, oh);
                oh->ot_assigned = 1;
                lu_context_init(&th->th_ctx, th->th_tags);
                lu_context_enter(&th->th_ctx);
                lu_device_get(&d->dd_lu_dev);
        }

        RETURN(rc);
}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_trans_stop(const struct lu_env *env, struct thandle *th)
{
        struct osd_device  *osd = osd_dt_dev(th->th_dev);
        struct osd_thandle *oh;
        uint64_t txg;
        int result;
        ENTRY;

        oh = container_of0(th, struct osd_thandle, ot_super);

        if (oh->ot_assigned == 0) {
                LASSERT(oh->ot_tx);
                dmu_tx_abort(oh->ot_tx);
                OBD_FREE_PTR(oh);
                RETURN(0);
        }

        result = dt_txn_hook_stop(env, th);
        if (result != 0)
                CERROR("Failure in transaction hook: %d\n", result);

        LASSERT(oh->ot_tx);
        txg = oh->ot_tx->tx_txg;

        dmu_tx_commit(oh->ot_tx);

        if (th->th_sync)
                txg_wait_synced(dmu_objset_pool(osd->od_objset.os), txg);

        RETURN(result);
}

static struct thandle *osd_trans_create(const struct lu_env *env,
                                       struct dt_device *dt)
{
        struct osd_device *osd = osd_dt_dev(dt);
        struct osd_thandle *oh;
        struct thandle *th;
        dmu_tx_t *tx;
        ENTRY;

        tx = dmu_tx_create(osd->od_objset.os);
        if (tx == NULL)
                RETURN(ERR_PTR(-ENOMEM));

        /* alloc callback data */
        OBD_ALLOC_PTR(oh);
        if (oh == NULL) {
                dmu_tx_abort(tx);
                RETURN(ERR_PTR(-ENOMEM));
        }

        oh->ot_tx = tx;
        CFS_INIT_LIST_HEAD(&oh->ot_dcb_list);
        th = &oh->ot_super;
        th->th_dev = dt;
        th->th_result = 0;
        th->th_tags = LCT_TX_HANDLE;
        RETURN(th);
}

static void __osd_declare_object_destroy(const struct lu_env *env,
                                         struct osd_object *obj, dmu_tx_t *tx)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        dmu_buf_t         *db = obj->oo_db;
        zap_attribute_t   *za = &osd_oti_get(env)->oti_za;
        uint64_t           oid = db->db_object, xid;
        zap_cursor_t      *zc;
        int                rc;

        dmu_tx_hold_free(tx, oid, 0, DMU_OBJECT_END);

        /* zap holding xattrs */
        rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_XATTR(uos), &obj->oo_xattr, 8);
        if (rc == 0) {
                oid = obj->oo_xattr;

                dmu_tx_hold_free(tx, oid, 0, DMU_OBJECT_END);

                rc = -udmu_zap_cursor_init(&zc, uos, oid, 0);
                if (rc)
                        goto out;

                while ((rc = -zap_cursor_retrieve(zc, za)) == 0) {
                        BUG_ON(za->za_integer_length != sizeof(uint64_t));
                        BUG_ON(za->za_num_integers != 1);

                        rc = -zap_lookup(uos->os, obj->oo_xattr, za->za_name,
                                         sizeof(uint64_t), 1, &xid);
                        if (rc) {
                                CERROR("error during xattr lookup: %d\n", rc);
                                break;
                        }
                        dmu_tx_hold_free(tx, xid, 0, DMU_OBJECT_END);

                        zap_cursor_advance(zc);
                }
                udmu_zap_cursor_fini(zc);
        }
        if (rc == -ENOENT)
                return;
out:
        if (rc && tx->tx_err == 0)
                tx->tx_err = -rc;

}

static int osd_declare_object_destroy(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct thandle *th)
{
        char                   *buf = osd_oti_get(env)->oti_str;
        const struct lu_fid    *fid = lu_object_fid(&dt->do_lu);
        struct osd_object      *obj = osd_dt_obj(dt);
        struct osd_device      *osd = osd_obj2dev(obj);
        struct osd_thandle     *oh;
        uint64_t                zapid;
        ENTRY;

        LASSERT(th != NULL);
        LASSERT(dt_object_exists(dt));

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh->ot_tx != NULL);

        /* declare that we'll destroy the object */
        __osd_declare_object_destroy(env, obj, oh->ot_tx);

        /* declare that we'll remove object from fid-dnode mapping */
        zapid = osd_get_name_n_idx(env, osd, fid, buf);
        dmu_tx_hold_bonus(oh->ot_tx, zapid);
        dmu_tx_hold_zap(oh->ot_tx, zapid, 0, buf);

        RETURN(0);
}

static int __osd_object_free(udmu_objset_t *uos, uint64_t oid, dmu_tx_t *tx)
{
        LASSERT(uos->objects != 0);
        cfs_spin_lock(&uos->lock);
        uos->objects--;
        cfs_spin_unlock(&uos->lock);

        return -dmu_object_free(uos->os, oid, tx);
}

/*
 * Delete a DMU object
 *
 * The transaction passed to this routine must have
 * dmu_tx_hold_free(tx, oid, 0, DMU_OBJECT_END) called
 * and then assigned to a transaction group.
 *
 * This will release db and set it to NULL to prevent further dbuf releases.
 */
static int __osd_object_destroy(const struct lu_env *env,
                                struct osd_object *obj,
                                dmu_tx_t *tx, void *tag)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        uint64_t           xid;
        zap_attribute_t   *za = &osd_oti_get(env)->oti_za;
        zap_cursor_t      *zc;
        int                rc;

        /* Assert that the transaction has been assigned to a
           transaction group. */
        LASSERT(tx->tx_txg != 0);

        /* zap holding xattrs */
        if (obj->oo_xattr != 0) {
                rc = -udmu_zap_cursor_init(&zc, uos, obj->oo_xattr, 0);
                if (rc)
                        return rc;
                while ((rc = -zap_cursor_retrieve(zc, za)) == 0) {
                        BUG_ON(za->za_integer_length != sizeof(uint64_t));
                        BUG_ON(za->za_num_integers != 1);

                        rc = -zap_lookup(uos->os, obj->oo_xattr, za->za_name,
                                         sizeof(uint64_t), 1, &xid);
                        if (rc) {
                                CERROR("error during lookup xattr %s: %d\n",
                                       za->za_name, rc);
                                continue;
                        }
                        rc = __osd_object_free(uos, xid, tx);
                        if (rc)
                                CERROR("error freeing xattr %s: %d\n",
                                       za->za_name, rc);
                        zap_cursor_advance(zc);
                }
                udmu_zap_cursor_fini(zc);

                rc = __osd_object_free(uos, obj->oo_xattr, tx);
                if (rc)
                        CERROR("error freeing xattr zap: %d\n", rc);
        }

        return __osd_object_free(uos, obj->oo_db->db_object, tx);
}

static int osd_object_destroy(const struct lu_env *env,
                              struct dt_object *dt,
                              struct thandle *th)
{
        char                   *buf = osd_oti_get(env)->oti_str;
        struct osd_object      *obj = osd_dt_obj(dt);
        struct osd_device      *osd = osd_obj2dev(obj);
        const struct lu_fid    *fid = lu_object_fid(&dt->do_lu);
        struct osd_thandle     *oh;
        int                     rc;
        uint64_t                zapid;
        ENTRY;

        LASSERT(obj->oo_db != NULL);
        LASSERT(dt_object_exists(dt));
        LASSERT(!lu_object_is_dying(dt->do_lu.lo_header));

        oh = container_of0(th, struct osd_thandle, ot_super);
        LASSERT(oh != NULL);
        LASSERT(oh->ot_tx != NULL);

        zapid = osd_get_name_n_idx(env, osd, fid, buf);

        /* remove obj ref from index dir (it depends) */
        rc = -zap_remove(osd->od_objset.os, zapid, buf, oh->ot_tx);
        if (rc) {
                CERROR("zap_remove() failed with error %d\n", rc);
                GOTO(out, rc);
        }

        /* kill object */
        rc = __osd_object_destroy(env, obj, oh->ot_tx, osd_obj_tag);
        if (rc) {
                CERROR("__osd_object_destroy() failed with error %d\n", rc);
                GOTO(out, rc);
        }

out:
        /* not needed in the cache anymore */
        set_bit(LU_OBJECT_HEARD_BANSHEE, &dt->do_lu.lo_header->loh_flags);

        RETURN (0);
}

static void osd_object_delete(const struct lu_env *env, struct lu_object *l)
{
        struct osd_object *obj = osd_obj(l);

        if (obj->oo_db != NULL) {
                if (likely(!fid_is_acct(lu_object_fid(l))))
                        osd_object_sa_fini(obj);
                if (obj->oo_sa_xattr) {
                        nvlist_free(obj->oo_sa_xattr);
                        obj->oo_sa_xattr = NULL;
                }
                sa_buf_rele(obj->oo_db, osd_obj_tag);
                obj->oo_db = NULL;
        }
}

/*
 * Concurrency: ->loo_object_release() is called under site spin-lock.
 */
static void osd_object_release(const struct lu_env *env,
                               struct lu_object *l)
{
}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_object_print(const struct lu_env *env, void *cookie,
                            lu_printer_t p, const struct lu_object *l)
{
        struct osd_object *o = osd_obj(l);

        return (*p)(env, cookie, LUSTRE_OSD_ZFS_NAME"-object@%p", o);
}

/*
 * Concurrency: shouldn't matter.
 */
int osd_statfs(const struct lu_env *env, struct dt_device *d,
               struct obd_statfs *osfs)
{
        struct osd_device *osd = osd_dt_dev(d);
        int                rc;
        ENTRY;

        rc = udmu_objset_statfs(&osd->od_objset, osfs);
        if (rc)
                RETURN(rc);
        osfs->os_bavail -= min_t(obd_size,
                                 OSD_GRANT_FOR_LOCAL_OIDS / osfs->os_bsize,
                                 osfs->os_bavail);
        RETURN(0);
}

/*
 * Concurrency: doesn't access mutable data.
 */
static void osd_conf_get(const struct lu_env *env,
                         const struct dt_device *dev,
                         struct dt_device_param *param)
{
        /*
         * XXX should be taken from not-yet-existing fs abstraction layer.
         */
        param->ddp_max_name_len  = 256;
        param->ddp_max_nlink     = 2048;
        param->ddp_block_shift   = 12; /* XXX */
        param->ddp_mount_type    = LDD_MT_ZFS;

        param->ddp_mntopts        = MNTOPT_USERXATTR | MNTOPT_ACL;
        param->ddp_max_ea_size    = DXATTR_MAX_ENTRY_SIZE;

        /* for maxbytes, report same value as ZPL */
        param->ddp_maxbytes      = MAX_LFS_FILESIZE;

        /* Default reserved fraction of the available space that should be kept
         * for error margin. Unfortunately, there are many factors that can
         * impact the overhead with zfs, so let's be very cautious for now and
         * reserve 20% of the available space which is not given out as grant.
         * This tunable can be changed on a live system via procfs if needed. */
        param->ddp_grant_reserved = 20;

        /* inodes are dynamically allocated, so we report the per-inode space
         * consumption to upper layers. This static value is not really accurate
         * and we should use the same logic as in udmu_objset_statfs() to
         * estimate the real size consumed by an object */
        param->ddp_inodespace = OSD_DNODE_EST_COUNT;
        /* per-fragment overhead to be used by the client code */
        param->ddp_grant_frag = udmu_blk_insert_cost();
}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_sync(const struct lu_env *env, struct dt_device *d)
{
        struct osd_device  *osd = osd_dt_dev(d);
        CDEBUG(D_HA, "syncing OSD %s\n", LUSTRE_OSD_ZFS_NAME);
        txg_wait_synced(dmu_objset_pool(osd->od_objset.os), 0ULL);
        return 0;
}

static int osd_commit_async(const struct lu_env *env, struct dt_device *dev)
{
        struct osd_device *osd = osd_dt_dev(dev);
        tx_state_t        *tx = &dmu_objset_pool(osd->od_objset.os)->dp_tx;
        uint64_t           txg;

        txg = tx->tx_open_txg + 1;
        if (tx->tx_quiesce_txg_waiting < txg) {
                tx->tx_quiesce_txg_waiting = txg;
                cv_broadcast(&tx->tx_quiesce_more_cv);
        }
        mutex_exit(&tx->tx_sync_lock);
        return 0;
}

/*
 * Concurrency: shouldn't matter.
 */
static int osd_ro(const struct lu_env *env, struct dt_device *d)
{
        struct osd_device  *osd = osd_dt_dev(d);
        ENTRY;

        CERROR("*** setting device %s read-only ***\n", LUSTRE_OSD_ZFS_NAME);
        osd->od_rdonly = 1;
        spa_freeze(dmu_objset_spa(osd->od_objset.os));

        RETURN(0);
}

/*
 * Concurrency: serialization provided by callers.
 */
static int osd_init_capa_ctxt(const struct lu_env *env, struct dt_device *d,
                              int mode, unsigned long timeout, __u32 alg,
                              struct lustre_capa_key *keys)
{
        struct osd_device *dev = osd_dt_dev(d);
        ENTRY;

        dev->od_fl_capa = mode;
        dev->od_capa_timeout = timeout;
        dev->od_capa_alg = alg;
        dev->od_capa_keys = keys;
        RETURN(0);
}

static int osd_quota_setup(const struct lu_env *env, struct dt_device *d,
                           void *data)
{
        RETURN(0);
}

static void osd_quota_cleanup(const struct lu_env *env, struct dt_device *d)
{
}

static char *osd_label_get(const struct lu_env *env, const struct dt_device *d)
{
        struct osd_device *dev = osd_dt_dev(d);
        int rc;
        ENTRY;

        rc = -udmu_userprop_get_str(&dev->od_objset, DMU_OSD_SVNAME,
                                    dev->od_svname, sizeof(dev->od_svname));
        if (rc != 0)
                RETURN(NULL);

        RETURN(&dev->od_svname[0]);
}

static int osd_label_set(const struct lu_env *env, const struct dt_device *d,
                         char *name)
{
        struct osd_device *dev = osd_dt_dev(d);
        int rc;
        ENTRY;

        rc = -udmu_userprop_set_str(&dev->od_objset, DMU_OSD_SVNAME, name);
        if (rc) {
                CERROR("error setting ZFS label to '%s': %d\n", name, rc);
                RETURN(rc);
        }

        /* rename procfs entry (it's special on the first setup ) */
        osd_procfs_fini(dev);
        osd_procfs_init(dev, name);

        RETURN(rc);
}

static struct dt_device_operations osd_dt_ops = {
        .dt_root_get       = osd_root_get,
        .dt_statfs         = osd_statfs,
        .dt_trans_create   = osd_trans_create,
        .dt_trans_start    = osd_trans_start,
        .dt_trans_stop     = osd_trans_stop,
        .dt_trans_cb_add   = osd_trans_cb_add,
        .dt_conf_get       = osd_conf_get,
        .dt_sync           = osd_sync,
        .dt_commit_async   = osd_commit_async,
        .dt_ro             = osd_ro,
        .dt_init_capa_ctxt = osd_init_capa_ctxt,
        .dt_quota          = {
                .dt_setup   = osd_quota_setup,
                .dt_cleanup = osd_quota_cleanup,
        },
        .dt_label_get      = osd_label_get,
        .dt_label_set      = osd_label_set
};

static void osd_object_read_lock(const struct lu_env *env,
                                 struct dt_object *dt, unsigned role)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(osd_invariant(obj));

        cfs_down_read(&obj->oo_sem);
}

static void osd_object_write_lock(const struct lu_env *env,
                                  struct dt_object *dt, unsigned role)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(osd_invariant(obj));

        cfs_down_write(&obj->oo_sem);
}

static void osd_object_read_unlock(const struct lu_env *env,
                                   struct dt_object *dt)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(osd_invariant(obj));
        cfs_up_read(&obj->oo_sem);
}

static void osd_object_write_unlock(const struct lu_env *env,
                                    struct dt_object *dt)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(osd_invariant(obj));
        cfs_up_write(&obj->oo_sem);
}

static int osd_object_write_locked(const struct lu_env *env,
                                   struct dt_object *dt)
{
        struct osd_object *obj = osd_dt_obj(dt);
        int rc = 1;

        LASSERT(osd_invariant(obj));

        if (cfs_down_write_trylock(&obj->oo_sem)) {
                rc = 0;
                cfs_up_write(&obj->oo_sem);
        }
        return rc;
}

static int osd_attr_get(const struct lu_env *env,
                        struct dt_object *dt,
                        struct lu_attr *attr,
                        struct lustre_capa *capa)
{
        struct osd_object *obj = osd_dt_obj(dt);
        uint64_t           blocks;

        LASSERT(dt_object_exists(dt));
        LASSERT(osd_invariant(obj));
        LASSERT(obj->oo_db);

        if (unlikely(fid_is_acct(lu_object_fid(&dt->do_lu)))) {
                /* XXX: at some point quota objects will get own methods */
                memset(attr, 0, sizeof(*attr));
                return 0;
        }

        cfs_read_lock(&obj->oo_attr_lock);
        sa_object_size(obj->oo_sa_hdl, &obj->oo_attr.la_blksize, &blocks);
        /* Block size may be not set; suggest maximal I/O transfers. */
        if (obj->oo_attr.la_blksize == 0)
                obj->oo_attr.la_blksize = 1ULL << SPA_MAXBLOCKSHIFT;
        obj->oo_attr.la_blocks = blocks;
        obj->oo_attr.la_valid |= LA_BLOCKS | LA_BLKSIZE;
        *attr = obj->oo_attr;
        cfs_read_unlock(&obj->oo_attr_lock);

        return 0;
}

static int osd_declare_attr_set(const struct lu_env *env,
                                struct dt_object *dt,
                                const struct lu_attr *attr,
                                struct thandle *handle)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        ENTRY;

        if (!dt_object_exists(dt)) {
                /* XXX: sanity check that object creation is declared */
                RETURN(0);
        }

        LASSERT(handle != NULL);
        LASSERT(osd_invariant(obj));

        oh = container_of0(handle, struct osd_thandle, ot_super);

        LASSERT(obj->oo_sa_hdl != NULL);
        dmu_tx_hold_sa(oh->ot_tx, obj->oo_sa_hdl, 0);

        RETURN(0);
}

/*
 * Set the attributes of an object
 *
 * The transaction passed to this routine must have
 * dmu_tx_hold_bonus(tx, oid) called and then assigned
 * to a transaction group.
 */
static int osd_attr_set(const struct lu_env *env, struct dt_object *dt,
                        const struct lu_attr *la, struct thandle *handle,
                        struct lustre_capa *capa)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_device  *osd = osd_obj2dev(obj);
        udmu_objset_t      *uos = &osd->od_objset;
        struct osd_thandle *oh;
        struct osa_attr    *osa = &osd_oti_get(env)->oti_osa;
        sa_bulk_attr_t     *bulk;
        int                 cnt;
        int                 rc = 0;

        ENTRY;
        LASSERT(handle != NULL);
        LASSERT(dt_object_exists(dt));
        LASSERT(osd_invariant(obj));
        LASSERT(obj->oo_sa_hdl);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        /* Assert that the transaction has been assigned to a
           transaction group. */
        LASSERT(oh->ot_tx->tx_txg != 0);

        if (la->la_valid == 0)
                RETURN(0);

        OBD_ALLOC(bulk, sizeof(sa_bulk_attr_t) * 10);
        if (bulk == NULL)
                RETURN(-ENOMEM);

        cfs_write_lock(&obj->oo_attr_lock);
        cnt = 0;
        if (la->la_valid & LA_ATIME) {
                osa->atime[0] = obj->oo_attr.la_atime = la->la_atime;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_ATIME(uos), NULL,
                                 osa->atime, 16);
        }
        if (la->la_valid & LA_MTIME) {
                osa->mtime[0] = obj->oo_attr.la_mtime = la->la_mtime;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MTIME(uos), NULL,
                                 osa->mtime, 16);
        }
        if (la->la_valid & LA_CTIME) {
                osa->ctime[0] = obj->oo_attr.la_ctime = la->la_ctime;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_CTIME(uos), NULL,
                                 osa->ctime, 16);
        }
        if (la->la_valid & LA_MODE) {
                /* mode is stored along with type, so read it first */
                obj->oo_attr.la_mode = (obj->oo_attr.la_mode & S_IFMT) |
                                       (la->la_mode & ~S_IFMT);
                osa->mode = obj->oo_attr.la_mode;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MODE(uos), NULL,
                                 &osa->mode, 8);
        }
        if (la->la_valid & LA_SIZE) {
                osa->size = obj->oo_attr.la_size = la->la_size;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_SIZE(uos), NULL,
                                 &osa->size, 8);
        }
        if (la->la_valid & LA_NLINK) {
                osa->nlink = obj->oo_attr.la_nlink = la->la_nlink;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_LINKS(uos), NULL,
                                 &osa->nlink, 8);
        }
        if (la->la_valid & LA_RDEV) {
                osa->rdev = obj->oo_attr.la_rdev = la->la_rdev;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_RDEV(uos), NULL,
                                 &osa->rdev, 8);
        }
        if (la->la_valid & LA_FLAGS) {
                osa->flags = obj->oo_attr.la_flags = la->la_flags;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_FLAGS(uos), NULL,
                                 &osa->flags, 8);
        }
        if (la->la_valid & LA_UID) {
                osa->uid = obj->oo_attr.la_uid = la->la_uid;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_UID(uos), NULL,
                                 &osa->uid, 8);
        }
        if (la->la_valid & LA_GID) {
                osa->gid = obj->oo_attr.la_gid = la->la_gid;
                SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_GID(uos), NULL,
                                 &osa->gid, 8);
        }

        obj->oo_attr.la_valid |= la->la_valid;
        rc = -sa_bulk_update(obj->oo_sa_hdl, bulk, cnt, oh->ot_tx);
        cfs_write_unlock(&obj->oo_attr_lock);

        OBD_FREE(bulk, sizeof(sa_bulk_attr_t) * 10);
        RETURN(rc);
}

static int osd_declare_punch(const struct lu_env *env, struct dt_object *dt,
                             __u64 start, __u64 end, struct thandle *handle)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        ENTRY;

        oh = container_of0(handle, struct osd_thandle, ot_super);

        /* declare we'll free some blocks ... */
        dmu_tx_hold_free(oh->ot_tx, obj->oo_db->db_object, start, end);

        /* ... and we'll modify size attribute */
        dmu_tx_hold_sa(oh->ot_tx, obj->oo_sa_hdl, 0);

        RETURN(0);
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
 * dmu_tx_hold_sa() and if off < size, dmu_tx_hold_free()
 * called and then assigned to a transaction group.
 */
static int __osd_object_punch(objset_t *os, dmu_buf_t *db, dmu_tx_t *tx,
                              uint64_t size, uint64_t off, uint64_t len)
{
        uint64_t end = off + len;
        int      rc = 0;

        /* Assert that the transaction has been assigned to a
           transaction group. */
        LASSERT(tx->tx_txg != 0);
        /*
         * Nothing to do if file already at desired length.
         */
        if (len == 0 && size == off) {
                return 0;
        }

        if (off < size) {
                uint64_t rlen = len;

                if (len == 0)
                        rlen = -1;
                else if (end > size)
                        rlen = size - off;

                rc = -dmu_free_range(os, db->db_object, off, rlen, tx);
        }
        return rc;
}

static int osd_punch(const struct lu_env *env, struct dt_object *dt,
                     __u64 start, __u64 end, struct thandle *th,
                     struct lustre_capa *capa)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_device  *osd = osd_obj2dev(obj);
        udmu_objset_t      *uos = &osd->od_objset;
        struct osd_thandle *oh;
        __u64               len = start - end;
        int                 rc = 0;
        ENTRY;

        LASSERT(dt_object_exists(dt));
        LASSERT(osd_invariant(obj));

        LASSERT(th != NULL);
        oh = container_of0(th, struct osd_thandle, ot_super);

        /* truncate */
        if (end == OBD_OBJECT_EOF)
                len = 0;

        cfs_write_lock(&obj->oo_attr_lock);
        rc = __osd_object_punch(osd->od_objset.os, obj->oo_db, oh->ot_tx,
                                obj->oo_attr.la_size, start, len);
        /* set new size */
        if ((end == OBD_OBJECT_EOF) || (start + end > obj->oo_attr.la_size)) {
                obj->oo_attr.la_size = start;
                rc = -sa_update(obj->oo_sa_hdl, SA_ZPL_SIZE(uos),
                                &obj->oo_attr.la_size, 8, oh->ot_tx);
        }
        cfs_write_unlock(&obj->oo_attr_lock);
        RETURN(rc);
}

/*
 * Object creation.
 *
 * XXX temporary solution.
 */

static void osd_ah_init(const struct lu_env *env, struct dt_allocation_hint *ah,
                        struct dt_object *parent, struct dt_object *child,
                        cfs_umode_t child_mode)
{
        LASSERT(ah);

        memset(ah, 0, sizeof(*ah));
        ah->dah_parent = parent;
        ah->dah_mode = child_mode;
}

static int osd_declare_object_create(const struct lu_env *env,
                                     struct dt_object *dt,
                                     struct lu_attr *attr,
                                     struct dt_allocation_hint *hint,
                                     struct dt_object_format *dof,
                                     struct thandle *handle)
{
        char                *buf = osd_oti_get(env)->oti_str;
        const struct lu_fid *fid = lu_object_fid(&dt->do_lu);
        struct osd_object   *obj = osd_dt_obj(dt);
        struct osd_device   *osd = osd_obj2dev(obj);
        struct osd_thandle  *oh;
        uint64_t             zapid;
        ENTRY;

        LASSERT(dof);

        switch (dof->dof_type) {
                case DFT_REGULAR:
                case DFT_SYM:
                case DFT_NODE:
                        if (obj->oo_dt.do_body_ops == NULL)
                                obj->oo_dt.do_body_ops = &osd_body_ops;
                        break;
                default:
                        break;
        }

        LASSERT(handle != NULL);
        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_tx != NULL);

        switch (dof->dof_type) {
                case DFT_DIR:
                case DFT_INDEX:
                        /* for zap create */
                        dmu_tx_hold_zap(oh->ot_tx, DMU_NEW_OBJECT, 1, NULL);
                        break;
                case DFT_REGULAR:
                case DFT_SYM:
                case DFT_NODE:
                        /* first, we'll create new object */
                        dmu_tx_hold_bonus(oh->ot_tx, DMU_NEW_OBJECT);
                        break;

                default:
                        LBUG();
                        break;
        }

        /* and we'll add it to some mapping */
        zapid = osd_get_name_n_idx(env, osd, fid, buf);
        dmu_tx_hold_bonus(oh->ot_tx, zapid);
        dmu_tx_hold_zap(oh->ot_tx, zapid, TRUE, buf);

        dmu_tx_hold_sa_create(oh->ot_tx, ZFS_SA_BASE_ATTR_SIZE);

        RETURN(0);
}

int __osd_attr_init(const struct lu_env *env, udmu_objset_t *uos,
                    uint64_t oid, dmu_tx_t *tx, struct lu_attr *la)
{
        sa_bulk_attr_t  *bulk;
        sa_handle_t     *sa_hdl;
        struct osa_attr *osa = &osd_oti_get(env)->oti_osa;
        uint64_t         gen;
        uint64_t         parent;
        uint64_t         crtime[2];
        timestruc_t      now;
        int              cnt;
        int              rc;

        gethrestime(&now);
        gen = dmu_tx_get_txg(tx);

        ZFS_TIME_ENCODE(&now, crtime);
        /* XXX: this should be real id of parent for ZPL access, but we have no
         * such info in OSD, probably it can be part of dt_object_format */
        parent = 0;

        osa->atime[0] = la->la_atime;
        osa->ctime[0] = la->la_ctime;
        osa->mtime[0] = la->la_mtime;
        osa->mode = la->la_mode;
        osa->uid = la->la_uid;
        osa->gid = la->la_gid;
        osa->rdev = la->la_rdev;
        osa->nlink = la->la_nlink;
        osa->flags = la->la_flags;
        osa->size  = la->la_size;

        /* Now add in all of the "SA" attributes */
        rc = -sa_handle_get(uos->os, oid, NULL, SA_HDL_PRIVATE, &sa_hdl);
        if (rc)
                return rc;

        OBD_ALLOC(bulk, sizeof(sa_bulk_attr_t) * 13);
        if (bulk == NULL) {
                rc = -ENOMEM;
                goto out;
        }
        /*
         * we need to create all SA below upon object create
         */
        cnt = 0;
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_CRTIME(uos), NULL, crtime, 16);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_GEN(uos), NULL, &gen, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_PARENT(uos), NULL, &parent, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_ATIME(uos), NULL, osa->atime, 16);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MTIME(uos), NULL, osa->mtime, 16);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_CTIME(uos), NULL, osa->ctime, 16);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_MODE(uos), NULL, &osa->mode, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_SIZE(uos), NULL, &osa->size, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_LINKS(uos), NULL, &osa->nlink, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_RDEV(uos), NULL, &osa->rdev, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_FLAGS(uos), NULL, &osa->flags, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_UID(uos), NULL, &osa->uid, 8);
        SA_ADD_BULK_ATTR(bulk, cnt, SA_ZPL_GID(uos), NULL, &osa->gid, 8);

        rc = -sa_replace_all_by_template(sa_hdl, bulk, cnt, tx);

        OBD_FREE(bulk, sizeof(sa_bulk_attr_t) * 13);
out:
        sa_handle_destroy(sa_hdl);
        return rc;
}

/*
 * The transaction passed to this routine must have
 * dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT) called and then assigned
 * to a transaction group.
 */
int __osd_object_create(const struct lu_env *env, udmu_objset_t *uos,
                        dmu_buf_t **dbp, dmu_tx_t *tx,
                        struct lu_attr *la, void *tag)
{
        uint64_t oid;
        int      rc;

        LASSERT(tag);
        cfs_spin_lock(&uos->lock);
        uos->objects++;
        cfs_spin_unlock(&uos->lock);

        /* Assert that the transaction has been assigned to a
           transaction group. */
        LASSERT(tx->tx_txg != 0);

        /* Create a new DMU object. */
        oid = dmu_object_alloc(uos->os, DMU_OT_PLAIN_FILE_CONTENTS, 0,
                               DMU_OT_SA, DN_MAX_BONUSLEN, tx);
        rc = -sa_buf_hold(uos->os, oid, tag, dbp);
        if (rc)
                return rc;

        LASSERT(la->la_valid & LA_MODE);
        la->la_size = 0;
        la->la_nlink = 1;

        return __osd_attr_init(env, uos, oid, tx, la);
}

/*
 * The transaction passed to this routine must have
 * dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, ...) called and then assigned
 * to a transaction group.
 *
 * Using ZAP_FLAG_HASH64 will force the ZAP to always be a FAT ZAP.
 * This is fine for directories today, because storing the FID in the dirent
 * will also require a FAT ZAP.  If there is a new type of micro ZAP created
 * then we might need to re-evaluate the use of this flag and instead do
 * a conversion from the different internal ZAP hash formats being used. */
int __osd_zap_create(const struct lu_env *env, udmu_objset_t *uos,
                     dmu_buf_t **zap_dbp, dmu_tx_t *tx,
                     struct lu_attr *la, void *tag)
{
        zap_flags_t zap_flags = ZAP_FLAG_HASH64;
        uint64_t    oid;
        int         rc;

        LASSERT(tag);
        cfs_spin_lock(&uos->lock);
        uos->objects++;
        cfs_spin_unlock(&uos->lock);

        /* Assert that the transaction has been assigned to a
           transaction group. */
        LASSERT(tx->tx_txg != 0);

        oid = zap_create_flags(uos->os, 0, zap_flags,
                               DMU_OT_DIRECTORY_CONTENTS, 12, 12,
                               DMU_OT_SA, DN_MAX_BONUSLEN, tx);

        rc = -sa_buf_hold(uos->os, oid, tag, zap_dbp);
        if (rc)
                return rc;

        LASSERT(la->la_valid & LA_MODE);
        la->la_size = 2;
        la->la_nlink = 1;

        return __osd_attr_init(env, uos, oid, tx, la);
}

static dmu_buf_t *osd_mkdir(const struct lu_env *env, struct osd_device *osd,
                            struct lu_attr *la, struct osd_thandle *oh)
{
        dmu_buf_t *db;
        int        rc;

        /* XXX: LASSERT(S_ISDIR(la->la_mode)); */
        rc = __osd_zap_create(env, &osd->od_objset, &db, oh->ot_tx, la,
                              osd_obj_tag);
        if (rc)
                return ERR_PTR(rc);
        return db;
}

static dmu_buf_t* osd_mkreg(const struct lu_env *env, struct osd_device *osd,
                            struct lu_attr *la, struct osd_thandle *oh)
{
        dmu_buf_t *db;
        int        rc;

        LASSERT(S_ISREG(la->la_mode));
        rc = __osd_object_create(env, &osd->od_objset, &db, oh->ot_tx, la,
                                 osd_obj_tag);
        if (rc)
                return ERR_PTR(rc);

        /*
         * XXX: a hack, OST to use bigger blocksize. we need
         * a method in OSD API to control this from OFD/MDD
         */
        if (!lu_device_is_md(osd2lu_dev(osd))) {
                rc = -dmu_object_set_blocksize(osd->od_objset.os,
                                               db->db_object,
                                               128 << 10, 0, oh->ot_tx);
                if (unlikely(rc)) {
                        CERROR("can't change blocksize: %d\n", rc);
                        return ERR_PTR(rc);
                }
        }

        return db;
}

static dmu_buf_t *osd_mksym(const struct lu_env *env, struct osd_device *osd,
                            struct lu_attr *la, struct osd_thandle *oh)
{
        dmu_buf_t *db;
        int        rc;

        LASSERT(S_ISLNK(la->la_mode));
        rc = __osd_object_create(env, &osd->od_objset, &db, oh->ot_tx, la,
                                 osd_obj_tag);
        if (rc)
                return ERR_PTR(rc);
        return db;
}

static dmu_buf_t *osd_mknod(const struct lu_env *env, struct osd_device *osd,
                            struct lu_attr *la, struct osd_thandle *oh)
{
        dmu_buf_t *db;
        int        rc;

        la->la_valid = LA_MODE;
        if (S_ISCHR(la->la_mode) || S_ISBLK(la->la_mode))
                la->la_valid |= LA_RDEV;

        rc = __osd_object_create(env, &osd->od_objset, &db, oh->ot_tx, la,
                                 osd_obj_tag);
        if (rc)
                return ERR_PTR(rc);
        return db;
}

typedef dmu_buf_t *(*osd_obj_type_f)(const struct lu_env *env, struct osd_device *osd,
                                     struct lu_attr *la, struct osd_thandle *oh);

static osd_obj_type_f osd_create_type_f(enum dt_format_type type)
{
        osd_obj_type_f result;

        switch (type) {
        case DFT_DIR:
        case DFT_INDEX:
                result = osd_mkdir;
                break;
        case DFT_REGULAR:
                result = osd_mkreg;
                break;
        case DFT_SYM:
                result = osd_mksym;
                break;
        case DFT_NODE:
                result = osd_mknod;
                break;
        default:
                LBUG();
                break;
        }
        return result;
}


/*
 * Concurrency: @dt is write locked.
 */
static int osd_object_create(const struct lu_env *env, struct dt_object *dt,
                             struct lu_attr *attr,
                             struct dt_allocation_hint *hint,
                             struct dt_object_format *dof,
                             struct thandle *th)
{
        struct zpl_direntry    *zde = &osd_oti_get(env)->oti_zde.lzd_reg;
        const struct lu_fid    *fid = lu_object_fid(&dt->do_lu);
        struct osd_object      *obj = osd_dt_obj(dt);
        struct osd_device      *osd = osd_obj2dev(obj);
        char                   *buf = osd_oti_get(env)->oti_str;
        struct osd_thandle     *oh;
        dmu_buf_t              *db;
        uint64_t                zapid;
        int                     rc;

        ENTRY;

        LASSERT(osd->od_objdir);
        LASSERT(osd_invariant(obj));
        LASSERT(!dt_object_exists(dt));
        LASSERT(dof != NULL);

        LASSERT(th != NULL);
        oh = container_of0(th, struct osd_thandle, ot_super);

        /*
         * XXX missing: Quote handling.
         */

        LASSERT(obj->oo_db == NULL);

        db = osd_create_type_f(dof->dof_type)(env, osd, attr, oh);
        if (IS_ERR(db))
                RETURN(PTR_ERR(th));

        zde->zde_pad = 0;
        zde->zde_dnode = db->db_object;
        zde->zde_type = IFTODT(attr->la_mode & S_IFMT);

        zapid = osd_get_name_n_idx(env, osd, fid, buf);

        rc = -zap_add(osd->od_objset.os, zapid, buf, 8, 1, zde, oh->ot_tx);
        if (likely(rc == 0)) {
                obj->oo_db = db;
                rc = osd_object_init0(env, obj);

                LASSERT(ergo(rc == 0, dt_object_exists(dt)));
                LASSERT(osd_invariant(obj));
        }

        RETURN(rc);
}

static struct dt_it *osd_zap_it_init(const struct lu_env *env,
                                     struct dt_object *dt,
                                     __u32 unused,
                                     struct lustre_capa *capa)
{
        struct osd_thread_info  *info = osd_oti_get(env);
        struct osd_zap_it       *it;
        struct osd_object       *obj = osd_dt_obj(dt);
        struct osd_device       *osd = osd_obj2dev(obj);
        struct lu_object        *lo  = &dt->do_lu;
        ENTRY;

        /* XXX: check capa ? */

        LASSERT(lu_object_exists(lo));
        LASSERT(obj->oo_db);
        LASSERT(udmu_object_is_zap(obj->oo_db));

        if (info == NULL)
                RETURN(ERR_PTR(-ENOMEM));
        it = &info->oti_it_zap;

        if (udmu_zap_cursor_init(&it->ozi_zc, &osd->od_objset,
                                 obj->oo_db->db_object, 0))
                RETURN(ERR_PTR(-ENOMEM));

        it->ozi_obj   = obj;
        it->ozi_capa  = capa;
        it->ozi_reset = 1;
        lu_object_get(lo);
        RETURN((struct dt_it *)it);
}

static void osd_zap_it_fini(const struct lu_env *env, struct dt_it *di)
{
        struct osd_zap_it *it = (struct osd_zap_it *)di;
        struct osd_object *obj;
        ENTRY;

        LASSERT(it);
        LASSERT(it->ozi_obj);

        obj = it->ozi_obj;

        udmu_zap_cursor_fini(it->ozi_zc);
        lu_object_put(env, &obj->oo_dt.do_lu);

        EXIT;
}

/**
 *  Move Iterator to record specified by \a key
 *
 *  \param  di      osd iterator
 *  \param  key     key for index
 *
 *  \retval +ve  di points to record with least key not larger than key
 *  \retval  0   di points to exact matched key
 *  \retval -ve  failure
 */

static int osd_zap_it_get(const struct lu_env *env,
                          struct dt_it *di, const struct dt_key *key)
{
        struct osd_zap_it *it = (struct osd_zap_it *)di;
        struct osd_object *obj = it->ozi_obj;
        struct osd_device *osd = osd_obj2dev(obj);
        ENTRY;

        LASSERT(it);
        LASSERT(it->ozi_zc);

        /* XXX: API is broken at the moment */
        LASSERT(((const char *)key)[0] == '\0');

        udmu_zap_cursor_fini(it->ozi_zc);
        if (udmu_zap_cursor_init(&it->ozi_zc, &osd->od_objset,
                                 obj->oo_db->db_object, 0))
                RETURN(-ENOMEM);

        it->ozi_reset = 1;

        RETURN(+1);
}

static void osd_zap_it_put(const struct lu_env *env, struct dt_it *di)
{
        /* PBS: do nothing : ref are incremented at retrive and decreamented
         *      next/finish. */
}


int udmu_zap_cursor_retrieve_key(const struct lu_env *env,
                                 zap_cursor_t *zc, char *key, int max)
{
        zap_attribute_t *za = &osd_oti_get(env)->oti_za;
        int             err;

        if ((err = zap_cursor_retrieve(zc, za)))
                return err;

        if (key) {
                if (strlen(za->za_name) > max)
                        return EOVERFLOW;
                strcpy(key, za->za_name);
        }

        return 0;
}

/**
 * to load a directory entry at a time and stored it in
 * iterator's in-memory data structure.
 *
 * \param di, struct osd_it_ea, iterator's in memory structure
 *
 * \retval +ve, iterator reached to end
 * \retval   0, iterator not reached to end
 * \retval -ve, on error
 */
static int osd_zap_it_next(const struct lu_env *env, struct dt_it *di)
{
        struct osd_zap_it *it = (struct osd_zap_it *)di;
        int                rc;
        ENTRY;

        if (it->ozi_reset == 0)
                zap_cursor_advance(it->ozi_zc);
        it->ozi_reset = 0;

        /*
         * According to current API we need to return error if its last entry.
         * zap_cursor_advance() does return any value. So we need to call
         * retrieve to check if there is any record.  We should make
         * changes to Iterator API to not return status for this API
         */
        rc = -udmu_zap_cursor_retrieve_key(env, it->ozi_zc, NULL, NAME_MAX);
        if (rc == -ENOENT) /* end of dir*/
                RETURN(+1);

        RETURN((rc));
}

static struct dt_key *osd_zap_it_key(const struct lu_env *env,
                                     const struct dt_it *di)
{
        struct osd_zap_it *it = (struct osd_zap_it *)di;
        int                rc = 0;
        ENTRY;

        it->ozi_reset = 0;
        rc = -udmu_zap_cursor_retrieve_key(env, it->ozi_zc, it->ozi_name,
                                           NAME_MAX + 1);
        if (!rc)
                RETURN((struct dt_key *)it->ozi_name);
        else
                RETURN(ERR_PTR(rc));
}

static int osd_zap_it_key_size(const struct lu_env *env,
                               const struct dt_it *di)
{
        struct osd_zap_it *it = (struct osd_zap_it *)di;
        int                rc;
        ENTRY;

        it->ozi_reset = 0;
        rc = -udmu_zap_cursor_retrieve_key(env, it->ozi_zc, it->ozi_name,
                                           NAME_MAX + 1);
        if (!rc)
                RETURN(strlen(it->ozi_name));
        else
                RETURN(rc);
}

/*
 * zap_cursor_retrieve read from current record.
 * to read bytes we need to call zap_lookup explicitly.
 */
int udmu_zap_cursor_retrieve_value(const struct lu_env *env,
                                   zap_cursor_t *zc,  char *buf,
                                   int buf_size, int *bytes_read)
{
        zap_attribute_t *za = &osd_oti_get(env)->oti_za;
        int err, actual_size;


        if ((err = zap_cursor_retrieve(zc, za)))
                return err;

        if (za->za_integer_length <= 0)
                return (ERANGE);

        actual_size = za->za_integer_length * za->za_num_integers;

        if (actual_size > buf_size) {
                actual_size = buf_size;
                buf_size = actual_size / za->za_integer_length;
        } else {
                buf_size = za->za_num_integers;
        }

        err = -zap_lookup(zc->zc_objset, zc->zc_zapobj,
                          za->za_name, za->za_integer_length,
                          buf_size, buf);

        if (!err)
                *bytes_read = actual_size;

        return err;
}

static inline void osd_it_append_attrs(struct lu_dirent *ent, __u32 attr,
                                       int len, __u16 type)
{
        const unsigned    align = sizeof(struct luda_type) - 1;
        struct luda_type *lt;

        /* check if file type is required */
        if (attr & LUDA_TYPE) {
                len = (len + align) & ~align;

                lt = (void *)ent->lde_name + len;
                lt->lt_type = cpu_to_le16(CFS_DTTOIF(type));
                ent->lde_attrs |= LUDA_TYPE;
        }

        ent->lde_attrs = cpu_to_le32(ent->lde_attrs);
}

static int osd_zap_it_rec(const struct lu_env *env, const struct dt_it *di,
                          struct dt_rec *dtrec, __u32 attr)
{
        struct luz_direntry *zde = &osd_oti_get(env)->oti_zde;
        zap_attribute_t     *za = &osd_oti_get(env)->oti_za;
        struct osd_zap_it   *it = (struct osd_zap_it *)di;
        struct lu_dirent    *lde = (struct lu_dirent *)dtrec;
        int                  rc, namelen;
        ENTRY;

        it->ozi_reset = 0;
        LASSERT(lde);

        lde->lde_hash = cpu_to_le64(udmu_zap_cursor_serialize(it->ozi_zc));

        if ((rc = -zap_cursor_retrieve(it->ozi_zc, za)))
                GOTO(out, rc);

        namelen = strlen(za->za_name);
        if (namelen > NAME_MAX)
                GOTO(out, rc = -EOVERFLOW);
        strcpy(lde->lde_name, za->za_name);
        lde->lde_namelen = cpu_to_le16(namelen);

        if (za->za_integer_length != 8 || za->za_num_integers < 3) {
                CERROR("%s: unsupported direntry format: %d %d\n",
                       osd_obj2dev(it->ozi_obj)->od_svname,
                       za->za_integer_length, (int)za->za_num_integers);

                GOTO(out, rc = -EIO);
        }

        rc = -zap_lookup(it->ozi_zc->zc_objset, it->ozi_zc->zc_zapobj,
                         za->za_name, za->za_integer_length, 3, zde);
        if (rc)
                GOTO(out, rc);

        lde->lde_fid = zde->lzd_fid;
        lde->lde_attrs = LUDA_FID;

        /* append lustre attributes */
        osd_it_append_attrs(lde, attr, namelen, zde->lzd_reg.zde_type);

        lde->lde_reclen = cpu_to_le16(lu_dirent_calc_size(namelen, attr));

out:
        RETURN(rc);
}

static __u64 osd_zap_it_store(const struct lu_env *env, const struct dt_it *di)
{
        struct osd_zap_it *it = (struct osd_zap_it *)di;

        it->ozi_reset = 0;
        RETURN(udmu_zap_cursor_serialize(it->ozi_zc));
}
/*
 * return status :
 *  rc == 0 -> ok, proceed.
 *  rc >  0 -> end of directory.
 *  rc <  0 -> error.  ( EOVERFLOW  can be masked.)
 */

static int osd_zap_it_load(const struct lu_env *env,
                           const struct dt_it *di, __u64 hash)
{
        struct osd_zap_it *it = (struct osd_zap_it *)di;
        struct osd_object *obj = it->ozi_obj;
        struct osd_device *osd = osd_obj2dev(obj);
        int                rc;
        ENTRY;

        udmu_zap_cursor_fini(it->ozi_zc);
        if (udmu_zap_cursor_init(&it->ozi_zc, &osd->od_objset,
                                 obj->oo_db->db_object, hash))
                RETURN(-ENOMEM);
        it->ozi_reset = 0;

        /* same as osd_zap_it_next()*/
        rc = -udmu_zap_cursor_retrieve_key(env, it->ozi_zc, NULL,
                                           NAME_MAX + 1);
        if (rc == 0)
                RETURN(+1);
        else if (rc == -ENOENT) /* end of dir*/
                RETURN(0);

        RETURN(rc);
}

static int osd_index_lookup(const struct lu_env *env, struct dt_object *dt,
                            struct dt_rec *rec, const struct dt_key *key,
                            struct lustre_capa *capa)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_device  *osd = osd_obj2dev(obj);
        int                 rc;
        ENTRY;

        LASSERT(udmu_object_is_zap(obj->oo_db));

        rc = -zap_lookup(osd->od_objset.os, obj->oo_db->db_object,
                         (char *)key, 8, sizeof(oti->oti_zde) / 8,
                         (void *)&oti->oti_zde);
        memcpy(rec, &oti->oti_zde.lzd_fid, sizeof(struct lu_fid));

        RETURN(rc == 0 ? 1 : rc);
}

static int osd_declare_index_insert(const struct lu_env *env,
                                    struct dt_object *dt,
                                    const struct dt_rec *rec,
                                    const struct dt_key *key,
                                    struct thandle *th)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        ENTRY;

        LASSERT(th != NULL);
        oh = container_of0(th, struct osd_thandle, ot_super);

        LASSERT(obj->oo_db);
        LASSERT(udmu_object_is_zap(obj->oo_db));

        dmu_tx_hold_bonus(oh->ot_tx, obj->oo_db->db_object);
        dmu_tx_hold_zap(oh->ot_tx, obj->oo_db->db_object, TRUE, (char *)key);

        RETURN(0);
}

/**
 * Find the osd object for given fid.
 *
 * \param fid need to find the osd object having this fid
 *
 * \retval osd_object on success
 * \retval        -ve on error
 */
struct osd_object *osd_object_find(const struct lu_env *env,
                                   struct dt_object *dt,
                                   const struct lu_fid *fid)
{
        struct lu_device         *ludev = dt->do_lu.lo_dev;
        struct osd_object        *child = NULL;
        struct lu_object         *luch;
        struct lu_object         *lo;

        /*
         * at this point topdev might not exist yet
         * (i.e. MGS is preparing profiles). so we can
         * not rely on topdev and instead lookup with
         * our device passed as topdev. this can't work
         * if the object isn't cached yet (as osd doesn't
         * allocate lu_header). IOW, the object must be
         * in the cache, otherwise lu_object_alloc() crashes
         * -bzzz
         */
        luch = lu_object_find_at(env, ludev, fid, NULL);
        if (IS_ERR(luch))
                return (void *)luch;

        if (lu_object_exists(luch)) {
                lo = lu_object_locate(luch->lo_header, ludev->ld_type);
                if (lo != NULL)
                        child = osd_obj(lo);
                else
                        LU_OBJECT_DEBUG(D_ERROR, env, luch,
                                        "%s: object can't be located "DFID"\n",
                                        ludev->ld_obd->obd_name, PFID(fid));

                if (child == NULL) {
                        lu_object_put(env, luch);
                        CERROR("%s: Unable to get osd_object "DFID"\n",
                              ludev->ld_obd->obd_name, PFID(fid));
                        child = ERR_PTR(-ENOENT);
                }
        } else {
                LU_OBJECT_DEBUG(D_ERROR, env, luch,
                                "%s: lu_object does not exists "DFID"\n",
                                ludev->ld_obd->obd_name, PFID(fid));
                child = ERR_PTR(-ENOENT);
        }

        return child;
}

/**
 * Put the osd object once done with it.
 *
 * \param obj osd object that needs to be put
 */
static inline void osd_object_put(const struct lu_env *env,
                                  struct osd_object *obj)
{
        lu_object_put(env, &obj->oo_dt.do_lu);
}

/**
 *      Inserts (key, value) pair in \a dt index object.
 *
 *      \param  dt      osd index object
 *      \param  key     key for index
 *      \param  rec     record reference
 *      \param  th      transaction handler
 *      \param  capa    capability descriptor
 *      \param  ignore_quota update should not affect quota
 *
 *      \retval  0  success
 *      \retval -ve failure
 */
static int osd_index_insert(const struct lu_env *env, struct dt_object *dt,
                            const struct dt_rec *rec, const struct dt_key *key,
                            struct thandle *th, struct lustre_capa *capa,
                            int ignore_quota)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_object   *parent = osd_dt_obj(dt);
        struct osd_device   *osd = osd_obj2dev(parent);
        struct lu_fid       *fid = (struct lu_fid *)rec;
        struct osd_thandle  *oh;
        struct osd_object   *child;
        __u32                attr;
        int                  rc;
        ENTRY;

        LASSERT(parent->oo_db);
        LASSERT(udmu_object_is_zap(parent->oo_db));

        LASSERT(dt_object_exists(dt));
        LASSERT(osd_invariant(parent));

        /*
         * zfs_readdir() generates ./.. on fly, but
         * we want own entries (.. at least) with a fid
         */
#if LUSTRE_VERSION_CODE > OBD_OCD_VERSION(2, 2, 53, 0)
#warning "fix '.' and '..' handling"
#endif

        LASSERT(th != NULL);
        oh = container_of0(th, struct osd_thandle, ot_super);

        /* XXX: Shouldn't rec be any data and not just a FID?
           If so, rec should have the size of the data and
           a pointer to the data - something like this:
           typedf struct {
                int dt_size;
                void * dt_data;
           } dt_data;
         */

        child = osd_object_find(env, dt, fid);
        if (IS_ERR(child))
                RETURN(PTR_ERR(child));

        LASSERT(child->oo_db);

        CLASSERT(sizeof(oti->oti_zde.lzd_reg) == 8);
        CLASSERT(sizeof(oti->oti_zde) % 8 == 0);
        attr = child->oo_dt.do_lu.lo_header ->loh_attr;
        oti->oti_zde.lzd_reg.zde_type = IFTODT(attr & S_IFMT);
        oti->oti_zde.lzd_reg.zde_dnode = child->oo_db->db_object;
        oti->oti_zde.lzd_fid = *fid;

        /* Insert (key,oid) into ZAP */
        rc = -zap_add(osd->od_objset.os, parent->oo_db->db_object,
                      (char *)key, 8, sizeof(oti->oti_zde) / 8,
                      (void *)&oti->oti_zde, oh->ot_tx);

        osd_object_put(env, child);

        RETURN(rc);
}

static int osd_declare_index_delete(const struct lu_env *env,
                                    struct dt_object *dt,
                                    const struct dt_key *key,
                                    struct thandle *th)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        ENTRY;

        LASSERT(dt_object_exists(dt));
        LASSERT(osd_invariant(obj));

        LASSERT(th != NULL);
        oh = container_of0(th, struct osd_thandle, ot_super);

        LASSERT(obj->oo_db);
        LASSERT(udmu_object_is_zap(obj->oo_db));

        dmu_tx_hold_zap(oh->ot_tx, obj->oo_db->db_object, TRUE, (char *)key);

        RETURN(0);

}

static int osd_index_delete(const struct lu_env *env, struct dt_object *dt,
                            const struct dt_key *key, struct thandle *th,
                            struct lustre_capa *capa)
{
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_device *osd = osd_obj2dev(obj);
        struct osd_thandle *oh;
        dmu_buf_t *zap_db = obj->oo_db;
        int rc;
        ENTRY;

        LASSERT(obj->oo_db);
        LASSERT(udmu_object_is_zap(obj->oo_db));

        LASSERT(th != NULL);
        oh = container_of0(th, struct osd_thandle, ot_super);

        /* Remove key from the ZAP */
        rc = -zap_remove(osd->od_objset.os, zap_db->db_object,
                         (char *) key, oh->ot_tx);

        if (rc && rc != -ENOENT)
                CERROR("zap_remove() failed with error %d\n", rc);

        RETURN(rc);
}

static struct dt_index_operations osd_index_ops = {
        .dio_lookup         = osd_index_lookup,
        .dio_declare_insert = osd_declare_index_insert,
        .dio_insert         = osd_index_insert,
        .dio_declare_delete = osd_declare_index_delete,
        .dio_delete         = osd_index_delete,
        .dio_it     = {
                .init     = osd_zap_it_init,
                .fini     = osd_zap_it_fini,
                .get      = osd_zap_it_get,
                .put      = osd_zap_it_put,
                .next     = osd_zap_it_next,
                .key      = osd_zap_it_key,
                .key_size = osd_zap_it_key_size,
                .rec      = osd_zap_it_rec,
                .store    = osd_zap_it_store,
                .load     = osd_zap_it_load
        }
};

static int osd_index_try(const struct lu_env *env, struct dt_object *dt,
                         const struct dt_index_features *feat)
{
        struct osd_object *obj  = osd_dt_obj(dt);
        LASSERT(obj->oo_db != NULL);
        ENTRY;
        /*
         * XXX: implement support for fixed-size keys sorted with natural
         *      numerical way (not using internal hash value)
         */
        if (feat->dif_flags & DT_IND_RANGE)
                RETURN(-ERANGE);

        if (unlikely(feat == &dt_acct_features)) {
                LASSERT(fid_is_acct(lu_object_fid(&dt->do_lu)));
                dt->do_index_ops = &osd_acct_index_ops;
                RETURN(0);
        }

        if (udmu_object_is_zap(obj->oo_db))
                dt->do_index_ops = &osd_index_ops;

        RETURN(0);
}

static int osd_declare_object_ref_add(const struct lu_env *env,
                               struct dt_object *dt,
                               struct thandle *th)
{
        return osd_declare_attr_set(env, dt, NULL, th);
}

/*
 * Concurrency: @dt is write locked.
 */
static int osd_object_ref_add(const struct lu_env *env,
                              struct dt_object *dt,
                              struct thandle *handle)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        struct osd_device  *osd = osd_obj2dev(obj);
        udmu_objset_t      *uos = &osd->od_objset;
        uint64_t            nlink;
        int rc;

        ENTRY;

        LASSERT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_sa_hdl != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);

        cfs_write_lock(&obj->oo_attr_lock);
        nlink = ++obj->oo_attr.la_nlink;
        rc = sa_update(obj->oo_sa_hdl, SA_ZPL_LINKS(uos), &nlink, 8,
                       oh->ot_tx);
        cfs_write_unlock(&obj->oo_attr_lock);
        return rc;
}

static int osd_declare_object_ref_del(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct thandle *handle)
{
        return osd_declare_attr_set(env, dt, NULL, handle);
}

/*
 * Concurrency: @dt is write locked.
 */
static int osd_object_ref_del(const struct lu_env *env,
                              struct dt_object *dt,
                              struct thandle *handle)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        struct osd_device  *osd = osd_obj2dev(obj);
        udmu_objset_t      *uos = &osd->od_objset;
        uint64_t            nlink;
        int rc;

        ENTRY;

        LASSERT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_sa_hdl != NULL);

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(!lu_object_is_dying(dt->do_lu.lo_header));

        cfs_write_lock(&obj->oo_attr_lock);
        nlink = --obj->oo_attr.la_nlink;
        rc = sa_update(obj->oo_sa_hdl, SA_ZPL_LINKS(uos), &nlink, 8,
                       oh->ot_tx);
        cfs_write_unlock(&obj->oo_attr_lock);
        return rc;
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
int __osd_xattr_cache(const struct lu_env *env, struct osd_object *obj)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        char              *buf;
        int                size;
        int                rc;

        LASSERT(obj->oo_sa_hdl);
        LASSERT(obj->oo_sa_xattr == NULL);

        rc = -sa_size(obj->oo_sa_hdl, SA_ZPL_DXATTR(uos), &size);
        if (rc) {
                if (rc == -ENOENT)
                        return -nvlist_alloc(&obj->oo_sa_xattr,
                                             NV_UNIQUE_NAME, KM_SLEEP);
                else
                        return rc;
        }

        buf = sa_spill_alloc(KM_SLEEP);
        if (buf == NULL)
                return -ENOMEM;
        rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_DXATTR(uos), buf, size);
        if (rc == 0)
                rc = -nvlist_unpack(buf, size, &obj->oo_sa_xattr, KM_SLEEP);
        sa_spill_free(buf);

        return rc;
}

int __osd_sa_xattr_get(const struct lu_env *env, struct osd_object *obj,
                       const struct lu_buf *buf, const char *name, int *sizep)
{
        uchar_t *nv_value;
        int      rc;

        LASSERT(obj->oo_sa_hdl);

        if (obj->oo_sa_xattr == NULL) {
                rc = __osd_xattr_cache(env, obj);
                if (rc)
                        return rc;
        }

        LASSERT(obj->oo_sa_xattr);
        rc = -nvlist_lookup_byte_array(obj->oo_sa_xattr, name, &nv_value,
                                       sizep);
        if (rc)
                return rc;

        if (buf == NULL || buf->lb_buf == NULL) {
                /* We only need to return the required size */
                return 0;
        }

        if (*sizep > buf->lb_len)
                return -ERANGE; /* match ldiskfs error */

        memcpy(buf->lb_buf, nv_value, *sizep);
        return 0;
}

int __osd_xattr_get(const struct lu_env *env, struct osd_object *obj,
                    struct lu_buf *buf, const char *name, int *sizep)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        uint64_t           xa_data_obj;
        dmu_buf_t         *xa_data_db;
        sa_handle_t       *sa_hdl = NULL;
        uint64_t           size;
        int                rc;

        /* check SA_ZPL_DXATTR first then fallback to directory xattr */
        rc = __osd_sa_xattr_get(env, obj, buf, name, sizep);
        if (rc != -ENOENT)
                return rc;

        /* are there any extended attributes? */
        rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_XATTR(uos), &obj->oo_xattr, 8);
        if (rc)
                return rc;

        /* Lookup the object number containing the xattr data */
        rc = -zap_lookup(uos->os, obj->oo_xattr, name, sizeof(uint64_t), 1,
                         &xa_data_obj);
        if (rc)
                return rc;

        rc = __osd_obj2dbuf(env, uos->os, xa_data_obj, &xa_data_db, FTAG);
        if (rc)
                return rc;

        rc = -sa_handle_get(uos->os, xa_data_obj, NULL, SA_HDL_PRIVATE,
                            &sa_hdl);
        if (rc)
                goto out_rele;

        /* Get the xattr value length / object size */
        rc = -sa_lookup(sa_hdl, SA_ZPL_SIZE(uos), &size, 8);
        if (rc)
                goto out;

        if (size > INT_MAX) {
                rc = -EOVERFLOW;
                goto out;
        }

        *sizep = (int)size;

        if (buf == NULL || buf->lb_buf == NULL) {
                /* We only need to return the required size */
                goto out;
        }
        if (*sizep > buf->lb_len) {
                rc = -ERANGE; /* match ldiskfs error */
                goto out;
        }

        rc = -dmu_read(uos->os, xa_data_db->db_object, 0,
                       size, buf->lb_buf, DMU_READ_PREFETCH);

out:
        sa_handle_destroy(sa_hdl);
out_rele:
        dmu_buf_rele(xa_data_db, FTAG);
        return rc;
}

static int osd_xattr_get(const struct lu_env *env, struct dt_object *dt,
                         struct lu_buf *buf, const char *name,
                         struct lustre_capa *capa)
{
        struct osd_object  *obj  = osd_dt_obj(dt);
        int                 rc, size = 0;
        ENTRY;

        LASSERT(obj->oo_db != NULL);
        LASSERT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));

        cfs_down(&obj->oo_guard);
        rc = __osd_xattr_get(env, obj, buf, name, &size);
        cfs_up(&obj->oo_guard);

        if (rc == -ENOENT)
                rc = -ENODATA;
        else if (rc == 0)
                rc = size;
        RETURN(rc);
}

void __osd_xattr_declare_set(const struct lu_env *env, struct osd_object *obj,
                             int vallen, const char *name, dmu_tx_t *tx)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        dmu_buf_t         *db = obj->oo_db;
        uint64_t           xa_data_obj;
        int                rc = 0;

        /* object may be not yet created */
        if (db != NULL) {
                /* we might just update SA_ZPL_DXATTR */
                dmu_tx_hold_sa(tx, obj->oo_sa_hdl, 1);

                rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_XATTR(uos),
                                &obj->oo_xattr, 8);
                if (rc != 0 && rc != -ENOENT)
                        goto out;
        }

        if (db == NULL || rc == -ENOENT) {
                /* we'll be updating SA_ZPL_XATTR */
                if (db)
                        dmu_tx_hold_sa(tx, obj->oo_sa_hdl, 1);
                /* xattr zap + entry */
                dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, TRUE, (char *) name);
                /* xattr value obj */
                dmu_tx_hold_sa_create(tx, ZFS_SA_BASE_ATTR_SIZE);
                dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, vallen);
                return;
        }

        rc = -zap_lookup(uos->os, obj->oo_xattr, name, sizeof(uint64_t), 1,
                         &xa_data_obj);
        if (rc == 0) {
                /*
                 * Entry already exists.
                 * We'll truncate the existing object.
                 */
                dmu_tx_hold_bonus(tx, xa_data_obj);
                dmu_tx_hold_free(tx, xa_data_obj, vallen, DMU_OBJECT_END);
                dmu_tx_hold_write(tx, xa_data_obj, 0, vallen);
                return;
        } else if (rc == -ENOENT) {
                /*
                 * Entry doesn't exist, we need to create a new one and a new
                 * object to store the value.
                 */
                dmu_tx_hold_bonus(tx, obj->oo_xattr);
                dmu_tx_hold_zap(tx, obj->oo_xattr, TRUE, (char *) name);
                dmu_tx_hold_sa_create(tx, ZFS_SA_BASE_ATTR_SIZE);
                dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, vallen);
                return;
        }
out:
        /* An error happened */
        tx->tx_err = -rc;
}

static int osd_declare_xattr_set(const struct lu_env *env, struct dt_object *dt,
                                 const struct lu_buf *buf, const char *name,
                                 int fl, struct thandle *handle)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        ENTRY;

        LASSERT(handle != NULL);
        oh = container_of0(handle, struct osd_thandle, ot_super);

        cfs_down(&obj->oo_guard);
        __osd_xattr_declare_set(env, obj, buf->lb_len, name, oh->ot_tx);
        cfs_up(&obj->oo_guard);

        RETURN(0);
}

/*
 * Set an extended attribute.
 * This transaction must have called udmu_xattr_declare_set() first.
 *
 * Returns 0 on success or a positive error number on failure.
 *
 * No locking is done here.
 */
static int __osd_sa_xattr_update(const struct lu_env *env,
                                 struct osd_object *obj, dmu_tx_t *tx)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        char              *dxattr;
        size_t             sa_size;
        int                rc;

        ENTRY;
        LASSERT(obj->oo_sa_hdl);
        LASSERT(obj->oo_sa_xattr);

        /* Update the SA for additions, modifications, and removals. */
        rc = -nvlist_size(obj->oo_sa_xattr, &sa_size, NV_ENCODE_XDR);
        if (rc)
                return rc;

        dxattr = sa_spill_alloc(KM_SLEEP);
        if (dxattr == NULL)
                RETURN(-ENOMEM);

        rc = -nvlist_pack(obj->oo_sa_xattr, &dxattr, &sa_size,
                          NV_ENCODE_XDR, KM_SLEEP);
        if (rc)
                GOTO(out_free, rc);

        rc = -sa_update(obj->oo_sa_hdl, SA_ZPL_DXATTR(uos), dxattr, sa_size,
                        tx);
out_free:
        sa_spill_free(dxattr);
        RETURN(rc);
}

int __osd_sa_xattr_set(const struct lu_env *env, struct osd_object *obj,
                       const struct lu_buf *buf, const char *name, int fl,
                       dmu_tx_t *tx)
{
        uchar_t *nv_value;
        size_t  size;
        int     nv_size;
        int     rc;

        LASSERT(obj->oo_sa_hdl);
        if (obj->oo_sa_xattr == NULL) {
                rc = __osd_xattr_cache(env, obj);
                if (rc)
                        return rc;
        }

        LASSERT(obj->oo_sa_xattr);
        /* Limited to 32k to keep nvpair memory allocations small */
        if (buf->lb_len > DXATTR_MAX_ENTRY_SIZE)
                return -EFBIG;

        /* Prevent the DXATTR SA from consuming the entire SA region */
        rc = -nvlist_size(obj->oo_sa_xattr, &size, NV_ENCODE_XDR);
        if (rc)
                return rc;

        if (size + buf->lb_len > DXATTR_MAX_SA_SIZE)
                return -EFBIG;

        rc = -nvlist_lookup_byte_array(obj->oo_sa_xattr, name, &nv_value,
                                       &nv_size);
        if (rc == 0) {
                if (fl & LU_XATTR_CREATE)
                        return -EEXIST;
        } else if (rc == -ENOENT) {
                if (fl & LU_XATTR_REPLACE)
                        return -ENODATA;
        } else {
                return rc;
        }

        rc = -nvlist_add_byte_array(obj->oo_sa_xattr, name,
                                    (uchar_t *)buf->lb_buf, buf->lb_len);
        if (rc)
                return rc;

        rc = __osd_sa_xattr_update(env, obj, tx);
        return rc;
}

int __osd_xattr_set(const struct lu_env *env, struct osd_object *obj,
                    const struct lu_buf *buf, const char *name, int fl,
                    dmu_tx_t *tx)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        dmu_buf_t         *xa_zap_db = NULL;
        dmu_buf_t         *xa_data_db = NULL;
        uint64_t           xa_data_obj;
        sa_handle_t       *sa_hdl = NULL;
        uint64_t           size;
        int                rc;

        LASSERT(obj->oo_sa_hdl);

        rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_XATTR(uos),
                        &obj->oo_xattr, 8);
        if (rc == -ENOENT) {
                struct lu_attr *la = &osd_oti_get(env)->oti_la;

                la->la_valid = LA_MODE;
                la->la_mode = S_IFDIR | S_IRUGO | S_IWUSR | S_IXUGO;
                rc = __osd_zap_create(env, uos, &xa_zap_db, tx, la, FTAG);
                if (rc)
                        return rc;

                obj->oo_xattr = xa_zap_db->db_object;
                rc = -sa_update(obj->oo_sa_hdl, SA_ZPL_XATTR(uos),
                                  &obj->oo_xattr, 8, tx);
                if (rc)
                        goto out;
        } else if (rc) {
                return rc;
        }

        rc = -zap_lookup(uos->os, obj->oo_xattr, name, sizeof(uint64_t), 1,
                         &xa_data_obj);
        if (rc == 0) {
                if (fl & LU_XATTR_CREATE) {
                        rc = -EEXIST;
                        goto out;
                }
                /*
                 * Entry already exists.
                 * We'll truncate the existing object.
                 */
                rc = __osd_obj2dbuf(env, uos->os, xa_data_obj,
                                    &xa_data_db, FTAG);
                if (rc)
                        goto out;

                rc = -sa_handle_get(uos->os, xa_data_obj, NULL,
                                   SA_HDL_PRIVATE, &sa_hdl);
                if (rc)
                        goto out;

                rc = -sa_lookup(sa_hdl, SA_ZPL_SIZE(uos), &size, 8);
                if (rc)
                        goto out_sa;

                rc = __osd_object_punch(uos->os, xa_data_db, tx, size,
                                        buf->lb_len, 0);
                if (rc)
                        goto out_sa;
        } else if (rc == -ENOENT) {
                struct lu_attr *la = &osd_oti_get(env)->oti_la;
                /*
                 * Entry doesn't exist, we need to create a new one and a new
                 * object to store the value.
                 */
                if (fl & LU_XATTR_REPLACE) {
                        /* should be ENOATTR according to the
                         * man, but that is undefined here */
                        rc = -ENODATA;
                        goto out;
                }

                la->la_valid = LA_MODE;
                la->la_mode = S_IFREG | S_IRUGO | S_IWUSR;
                rc = __osd_object_create(env, uos, &xa_data_db, tx, la, FTAG);
                if (rc)
                        goto out;
                xa_data_obj = xa_data_db->db_object;

                rc = -sa_handle_get(uos->os, xa_data_obj, NULL,
                                    SA_HDL_PRIVATE, &sa_hdl);
                if (rc)
                        goto out;

                rc = -zap_add(uos->os, obj->oo_xattr, name, sizeof(uint64_t),
                              1, &xa_data_obj, tx);
                if (rc)
                        goto out_sa;
        } else {
                /* There was an error looking up the xattr name */
                goto out;
        }

        /* Finally write the xattr value */
        dmu_write(uos->os, xa_data_obj, 0, buf->lb_len, buf->lb_buf, tx);

        size = buf->lb_len;
        rc = -sa_update(sa_hdl, SA_ZPL_SIZE(uos), &size, 8, tx);

out_sa:
        sa_handle_destroy(sa_hdl);
out:
        if (xa_data_db != NULL)
                dmu_buf_rele(xa_data_db, FTAG);
        if (xa_zap_db != NULL)
                dmu_buf_rele(xa_zap_db, FTAG);

        return rc;
}

static int osd_xattr_set(const struct lu_env *env,
                         struct dt_object *dt, const struct lu_buf *buf,
                         const char *name, int fl, struct thandle *handle,
                         struct lustre_capa *capa)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        int rc = 0;
        ENTRY;

        LASSERT(handle != NULL);
        LASSERT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        oh = container_of0(handle, struct osd_thandle, ot_super);

        cfs_down(&obj->oo_guard);
        rc = __osd_sa_xattr_set(env, obj, buf, name, fl, oh->ot_tx);
        /* place xattr in dnode if SA is full */
        if (rc == -EFBIG)
                rc = __osd_xattr_set(env, obj, buf, name, fl, oh->ot_tx);
        cfs_up(&obj->oo_guard);

        RETURN(rc);
}

void __osd_xattr_declare_del(const struct lu_env *env, struct osd_object *obj,
                             const char *name, dmu_tx_t *tx)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        uint64_t           xa_data_obj;
        int                rc;

        /* update SA_ZPL_DXATTR if xattr was in SA */
        dmu_tx_hold_sa(tx, obj->oo_sa_hdl, 0);

        rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_XATTR(uos),
                        &obj->oo_xattr, 8);
        if (rc == -ENOENT)
                return;
        else if (rc)
                goto out;

        rc = -zap_lookup(uos->os, obj->oo_xattr, name, 8, 1, &xa_data_obj);
        if (rc == 0) {
                /*
                 * Entry exists.
                 * We'll delete the existing object and ZAP entry.
                 */
                dmu_tx_hold_bonus(tx, xa_data_obj);
                dmu_tx_hold_free(tx, xa_data_obj, 0, DMU_OBJECT_END);
                dmu_tx_hold_zap(tx, obj->oo_xattr, FALSE, (char *) name);
                return;
        } else if (rc == -ENOENT) {
                /*
                 * Entry doesn't exist, nothing to be changed.
                 */
                return;
        }
out:
        /* An error happened */
        tx->tx_err = -rc;
}

static int osd_declare_xattr_del(const struct lu_env *env, struct dt_object *dt,
                                 const char *name, struct thandle *handle)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        ENTRY;

        LASSERT(handle != NULL);
        LASSERT(dt_object_exists(dt));
        LASSERT(osd_invariant(obj));

        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_tx != NULL);
        LASSERT(obj->oo_db != NULL);

        cfs_down(&obj->oo_guard);
        __osd_xattr_declare_del(env, obj, name, oh->ot_tx);
        cfs_up(&obj->oo_guard);

        RETURN(0);
}

int __osd_sa_xattr_del(const struct lu_env *env, struct osd_object *obj,
                       const char *name, dmu_tx_t *tx)
{
        int rc;

        if (obj->oo_sa_xattr == NULL) {
                rc = __osd_xattr_cache(env, obj);
                if (rc)
                        return rc;
        }

        rc = -nvlist_remove(obj->oo_sa_xattr, name, DATA_TYPE_BYTE_ARRAY);
        if (rc == -ENOENT)
                rc = 0;
        else if (rc == 0)
                rc = __osd_sa_xattr_update(env, obj, tx);
        return rc;
}

int __osd_xattr_del(const struct lu_env *env, struct osd_object *obj,
                    const char *name, dmu_tx_t *tx)
{
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        uint64_t           xa_data_obj;
        int                rc;

        /* try remove xattr from SA at first */
        rc = __osd_sa_xattr_del(env, obj, name, tx);
        if (rc != -ENOENT)
                return rc;

        rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_XATTR(uos),
                        &obj->oo_xattr, 8);
        if (rc)
                return rc;

        rc = -zap_lookup(uos->os, obj->oo_xattr, name, sizeof(uint64_t), 1,
                         &xa_data_obj);
        if (rc == -ENOENT) {
                rc = 0;
        } else if (rc == 0) {
                /*
                 * Entry exists.
                 * We'll delete the existing object and ZAP entry.
                 */
                rc = __osd_object_free(uos, xa_data_obj, tx);
                if (rc)
                        return rc;

                rc = -zap_remove(uos->os, obj->oo_xattr, name, tx);
        }

        return rc;
}

static int osd_xattr_del(const struct lu_env *env, struct dt_object *dt,
                         const char *name, struct thandle *handle,
                         struct lustre_capa *capa)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        int                 rc;
        ENTRY;

        LASSERT(handle != NULL);
        LASSERT(obj->oo_db != NULL);
        LASSERT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));
        oh = container_of0(handle, struct osd_thandle, ot_super);
        LASSERT(oh->ot_tx != NULL);

        cfs_down(&obj->oo_guard);
        rc = __osd_xattr_del(env, obj, name, oh->ot_tx);
        cfs_up(&obj->oo_guard);

        RETURN(rc);
}

static int osd_sa_xattr_list(const struct lu_env *env,
                             struct osd_object *obj, struct lu_buf *lb)
{
        nvpair_t *nvp = NULL;
        int       len, counted = 0, remain = lb->lb_len;
        int       rc = 0;

        if (obj->oo_sa_xattr == NULL) {
                rc = __osd_xattr_cache(env, obj);
                if (rc)
                        return rc;
        }

        LASSERT(obj->oo_sa_xattr);

        while ((nvp = nvlist_next_nvpair(obj->oo_sa_xattr, nvp)) != NULL) {
                len = strlen(nvpair_name(nvp));
                if (len < remain) {
                        memcpy(lb->lb_buf, nvpair_name(nvp), len);
                        lb->lb_buf += len;
                        *((char *)lb->lb_buf) = '\0';
                        lb->lb_buf++;
                        remain -= len + 1;
                }
                counted += len + 1;
        }
        return counted;
}

static int osd_xattr_list(const struct lu_env *env,
                          struct dt_object *dt, struct lu_buf *lb,
                          struct lustre_capa *capa)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct osd_object      *obj = osd_dt_obj(dt);
        struct osd_device      *osd = osd_obj2dev(obj);
        udmu_objset_t          *uos = &osd->od_objset;
        zap_cursor_t           *zc;
        int                    rc, counted = 0, remain = lb->lb_len;
        ENTRY;

        LASSERT(obj->oo_db != NULL);
        LASSERT(osd_invariant(obj));
        LASSERT(dt_object_exists(dt));

        cfs_down(&obj->oo_guard);

        rc = osd_sa_xattr_list(env, obj, lb);
        if (rc < 0)
                GOTO(out, rc);
        counted = rc;
        remain -= counted;

        /* continue with dnode xattr if any */
        rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_XATTR(uos), &obj->oo_xattr, 8);
        if (rc == -ENOENT)
                GOTO(out, rc = counted);
        else if (rc)
                GOTO(out, rc);

        rc = -udmu_zap_cursor_init(&zc, uos, obj->oo_xattr, 0);
        if (rc)
                GOTO(out, rc);

        while ((rc = -udmu_zap_cursor_retrieve_key(env, zc, oti->oti_key,
                                                   MAXNAMELEN)) == 0) {
                rc = strlen(oti->oti_key);
                if (rc + 1 <= remain) {
                        memcpy(lb->lb_buf, oti->oti_key, rc);
                        lb->lb_buf += rc;
                        *((char *)lb->lb_buf) = '\0';
                        lb->lb_buf++;
                        remain -= rc + 1;
                }
                counted += rc + 1;
                zap_cursor_advance(zc);
        }
        if (rc < 0)
                GOTO(out_fini, rc);
        rc = counted;

out_fini:
        udmu_zap_cursor_fini(zc);
out:
        cfs_up(&obj->oo_guard);
        RETURN(rc);

}

static int capa_is_sane(const struct lu_env *env, struct osd_device *dev,
                        struct lustre_capa *capa, struct lustre_capa_key *keys)
{
        struct osd_thread_info *oti = osd_oti_get(env);
        struct obd_capa *oc;
        int i, rc = 0;
        ENTRY;

        oc = capa_lookup(dev->od_capa_hash, capa, 0);
        if (oc) {
                if (capa_is_expired(oc)) {
                        DEBUG_CAPA(D_ERROR, capa, "expired");
                        rc = -ESTALE;
                }
                capa_put(oc);
                RETURN(rc);
        }

        cfs_spin_lock(&capa_lock);
        for (i = 0; i < 2; i++) {
                if (keys[i].lk_keyid == capa->lc_keyid) {
                        oti->oti_capa_key = keys[i];
                        break;
                }
        }
        cfs_spin_unlock(&capa_lock);

        if (i == 2) {
                DEBUG_CAPA(D_ERROR, capa, "no matched capa key");
                RETURN(-ESTALE);
        }

        rc = capa_hmac(oti->oti_capa.lc_hmac, capa, oti->oti_capa_key.lk_key);
        if (rc)
                RETURN(rc);
        if (memcmp(oti->oti_capa.lc_hmac, capa->lc_hmac, sizeof(capa->lc_hmac)))
        {
                DEBUG_CAPA(D_ERROR, capa, "HMAC mismatch");
                LBUG();
                RETURN(-EACCES);
        }

        oc = capa_add(dev->od_capa_hash, capa);
        capa_put(oc);

        RETURN(0);
}

static int osd_object_auth(const struct lu_env *env, struct dt_object *dt,
                           struct lustre_capa *capa, __u64 opc)
{
        const struct lu_fid *fid = lu_object_fid(&dt->do_lu);
        struct osd_device *dev = osd_dev(dt->do_lu.lo_dev);
        int rc;

        if (!dev->od_fl_capa)
                return 0;

        if (capa == BYPASS_CAPA)
                return 0;

        if (!capa) {
                CERROR("no capability is provided for fid "DFID"\n", PFID(fid));
                return -EACCES;
        }

        if (!lu_fid_eq(fid, &capa->lc_fid)) {
                DEBUG_CAPA(D_ERROR, capa, "fid "DFID" mismatch with",
                           PFID(fid));
                return -EACCES;
        }

        if (!capa_opc_supported(capa, opc)) {
                DEBUG_CAPA(D_ERROR, capa, "opc "LPX64" not supported by", opc);
                return -EACCES;
        }

        if ((rc = capa_is_sane(env, dev, capa, dev->od_capa_keys))) {
                DEBUG_CAPA(D_ERROR, capa, "insane (rc %d)", rc);
                return -EACCES;
        }

        return 0;
}

static struct obd_capa *osd_capa_get(const struct lu_env *env,
                                     struct dt_object *dt,
                                     struct lustre_capa *old,
                                     __u64 opc)
{
        struct osd_thread_info *info = osd_oti_get(env);
        const struct lu_fid *fid = lu_object_fid(&dt->do_lu);
        struct osd_object *obj = osd_dt_obj(dt);
        struct osd_device *dev = osd_obj2dev(obj);
        struct lustre_capa_key *key = &info->oti_capa_key;
        struct lustre_capa *capa = &info->oti_capa;
        struct obd_capa *oc;
        int rc;
        ENTRY;

        if (!dev->od_fl_capa)
                RETURN(ERR_PTR(-ENOENT));

        LASSERT(dt_object_exists(dt));
        LASSERT(osd_invariant(obj));

        /* renewal sanity check */
        if (old && osd_object_auth(env, dt, old, opc))
                RETURN(ERR_PTR(-EACCES));

        capa->lc_fid = *fid;
        capa->lc_opc = opc;
        capa->lc_uid = 0;
        capa->lc_flags = dev->od_capa_alg << 24;
        capa->lc_timeout = dev->od_capa_timeout;
        capa->lc_expiry = 0;

        oc = capa_lookup(dev->od_capa_hash, capa, 1);
        if (oc) {
                LASSERT(!capa_is_expired(oc));
                RETURN(oc);
        }

        cfs_spin_lock(&capa_lock);
        *key = dev->od_capa_keys[1];
        cfs_spin_unlock(&capa_lock);

        capa->lc_keyid = key->lk_keyid;
        capa->lc_expiry = cfs_time_current_sec() + dev->od_capa_timeout;

        rc = capa_hmac(capa->lc_hmac, capa, key->lk_key);
        if (rc) {
                DEBUG_CAPA(D_ERROR, capa, "HMAC failed: %d for", rc);
                LBUG();
                RETURN(ERR_PTR(rc));
        }

        oc = capa_add(dev->od_capa_hash, capa);
        RETURN(oc);
}

static int osd_object_sync(const struct lu_env *env, struct dt_object *dt)
{
        ENTRY;

        /* XXX: no other option than syncing the whole filesystem until we
         * support ZIL */
        osd_sync(env, lu2dt_dev(dt->do_lu.lo_dev));

        RETURN(0);
}

static struct dt_object_operations osd_obj_ops = {
        .do_read_lock         = osd_object_read_lock,
        .do_write_lock        = osd_object_write_lock,
        .do_read_unlock       = osd_object_read_unlock,
        .do_write_unlock      = osd_object_write_unlock,
        .do_write_locked      = osd_object_write_locked,
        .do_attr_get          = osd_attr_get,
        .do_declare_attr_set  = osd_declare_attr_set,
        .do_attr_set          = osd_attr_set,
        .do_ah_init           = osd_ah_init,
        .do_declare_create    = osd_declare_object_create,
        .do_create            = osd_object_create,
        .do_declare_destroy   = osd_declare_object_destroy,
        .do_destroy           = osd_object_destroy,
        .do_index_try         = osd_index_try,
        .do_declare_ref_add   = osd_declare_object_ref_add,
        .do_ref_add           = osd_object_ref_add,
        .do_declare_ref_del   = osd_declare_object_ref_del,
        .do_ref_del           = osd_object_ref_del,
        .do_xattr_get         = osd_xattr_get,
        .do_declare_xattr_set = osd_declare_xattr_set,
        .do_xattr_set         = osd_xattr_set,
        .do_declare_xattr_del = osd_declare_xattr_del,
        .do_xattr_del         = osd_xattr_del,
        .do_xattr_list        = osd_xattr_list,
        .do_capa_get          = osd_capa_get,
        .do_object_sync       = osd_object_sync,
};

/*
 * Body operations.
 */

static ssize_t osd_read(const struct lu_env *env, struct dt_object *dt,
                        struct lu_buf *buf, loff_t *pos,
                        struct lustre_capa *capa)
{
        struct osd_object *obj  = osd_dt_obj(dt);
        struct osd_device *osd = osd_obj2dev(obj);
        udmu_objset_t     *uos = &osd->od_objset;
        uint64_t           old_size;
        int                size = buf->lb_len;
        int                rc;

        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        rc = -sa_lookup(obj->oo_sa_hdl, SA_ZPL_SIZE(uos), &old_size, 8);
        if (rc)
                return rc;

        if (*pos + size > old_size) {
                if (old_size < *pos)
                        size = 0;
                else
                        size = old_size - *pos;
        }

        rc = -dmu_read(osd->od_objset.os, obj->oo_db->db_object, *pos, size,
                       buf->lb_buf, DMU_READ_PREFETCH);
        if (rc == 0) {
                rc = size;
                *pos += size;

                /* XXX: workaround for bug in HEAD: fsfilt_ldiskfs_read() returns
                 * requested number of bytes, not actually read ones */
                if (S_ISLNK(obj->oo_dt.do_lu.lo_header->loh_attr))
                        rc = buf->lb_len;
        }
        return rc;
}

static ssize_t osd_declare_write(const struct lu_env *env, struct dt_object *dt,
                                 const loff_t size, loff_t pos,
                                 struct thandle *th)
{
        struct osd_object  *obj  = osd_dt_obj(dt);
        struct osd_thandle *oh;
        uint64_t            oid;
        ENTRY;

        oh = container_of0(th, struct osd_thandle, ot_super);

        if (obj->oo_db) {
                LASSERT(dt_object_exists(dt));

                oid = obj->oo_db->db_object;

                /*
                 * declare possible size change. notice we can't check current
                 * size here as another thread can change it
                 */
                dmu_tx_hold_sa(oh->ot_tx, obj->oo_sa_hdl, 0);
        } else {
                LASSERT(!dt_object_exists(dt));

                oid = DMU_NEW_OBJECT;
        }

        dmu_tx_hold_write(oh->ot_tx, oid, pos, size);

        RETURN(0);
}

static ssize_t osd_write(const struct lu_env *env, struct dt_object *dt,
                         const struct lu_buf *buf, loff_t *pos,
                         struct thandle *th, struct lustre_capa *capa,
                         int ignore_quota)
{
        struct osd_object  *obj  = osd_dt_obj(dt);
        struct osd_device  *osd = osd_obj2dev(obj);
        udmu_objset_t      *uos = &osd->od_objset;
        struct osd_thandle *oh;
        uint64_t            offset = *pos;
        int                 rc;
        ENTRY;

        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        LASSERT(th != NULL);
        oh = container_of0(th, struct osd_thandle, ot_super);

        dmu_write(osd->od_objset.os, obj->oo_db->db_object, offset,
                  (uint64_t)buf->lb_len, buf->lb_buf, oh->ot_tx);
        cfs_write_lock(&obj->oo_attr_lock);
        if (obj->oo_attr.la_size < offset + buf->lb_len) {
                obj->oo_attr.la_size = offset + buf->lb_len;
                rc = -sa_update(obj->oo_sa_hdl, SA_ZPL_SIZE(uos),
                                &obj->oo_attr.la_size, 8, oh->ot_tx);
                if (rc) {
                        cfs_write_unlock(&obj->oo_attr_lock);
                        RETURN(rc);
                }
        }
        cfs_write_unlock(&obj->oo_attr_lock);

        *pos += buf->lb_len;
        rc = buf->lb_len;

        RETURN(rc);
}

static int osd_map_remote_to_local(struct dt_object *dt, loff_t offset,
                                   ssize_t len, struct niobuf_local *lnb)
{
        int plen;
        int nrpages = 0;

        while (len > 0) {
                plen = len;
                if (plen > CFS_PAGE_SIZE)
                        plen = CFS_PAGE_SIZE;

                lnb->lnb_file_offset = offset;
                lnb->lnb_page_offset = 0;
                lnb->lnb_len = plen;
                lnb->lnb_page = NULL;
                lnb->lnb_rc = 0;
                lnb->lnb_obj = dt;

                offset += plen;
                len -= plen;
                lnb++;
                nrpages++;
        }
        return nrpages;
}

static int osd_bufs_get(const struct lu_env *env, struct dt_object *dt,
                        loff_t offset, ssize_t len, struct niobuf_local *lnb,
                        int rw, struct lustre_capa *capa)
{
        struct osd_object   *obj     = osd_dt_obj(dt);
        int                  rc, i, npages;
        ENTRY;

        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        npages = osd_map_remote_to_local(dt, offset, len, lnb);

        for (i = 0; i < npages; i++) {
                lnb[i].lnb_page = alloc_page(GFP_NOFS | __GFP_HIGHMEM);
                if (lnb[i].lnb_page == NULL)
                        GOTO(out_err, rc = -ENOMEM);
                lu_object_get(&dt->do_lu);
        }

        RETURN(npages);
out_err:
        while (--i >= 0) {
                LASSERT(lnb[i].lnb_page);
                lu_object_put(env, &dt->do_lu);
                __free_page(lnb[i].lnb_page);
                lnb[i].lnb_page = NULL;
        }
        RETURN(rc);
}

static int osd_bufs_put(const struct lu_env *env, struct dt_object *dt,
                        struct niobuf_local *lnb, int npages)
{
        struct osd_object *obj  = osd_dt_obj(dt);
        int                i;

        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        for (i = 0; i < npages; i++) {
                LASSERT(lnb[i].lnb_obj == dt);
                if (lnb[i].lnb_page == NULL)
                        continue;
                lu_object_put(env, &dt->do_lu);
                __free_page(lnb[i].lnb_page);
                lnb[i].lnb_page = NULL;
        }

        return 0;
}

static int osd_write_prep(const struct lu_env *env, struct dt_object *dt,
                          struct niobuf_local *lnb, int npages)
{
        struct osd_object *obj = osd_dt_obj(dt);

        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        return 0;
}

static int osd_declare_write_commit(const struct lu_env *env,
                                    struct dt_object *dt,
                                    struct niobuf_local *lnb, int npages,
                                    struct thandle *th)
{
        struct osd_object  *obj = osd_dt_obj(dt);
        struct osd_thandle *oh;
        uint64_t            offset = 0;
        uint32_t            size = 0;
        int                 i;
        ENTRY;

        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        LASSERT(lnb);
        LASSERT(npages > 0);
        LASSERT(lnb->lnb_obj == dt);

        oh = container_of0(th, struct osd_thandle, ot_super);

        for (i = 0; i < npages; i++) {
                if (lnb[i].lnb_rc)
                        /* ENOSPC, network RPC error, etc.
                         * We don't want to book space for pages which will be
                         * skipped in osd_write_commit(). Hence we skip pages
                         * with lnb_rc != 0 here too */
                        continue;
                if (size == 0) {
                        /* first valid lnb */
                        offset = lnb[i].lnb_file_offset;
                        size = lnb[i].lnb_len;
                        continue;
                }
                if (offset + size == lnb[i].lnb_file_offset) {
                        /* this lnb is contiguous to the previous one */
                        size += lnb[i].lnb_len;
                        continue;
                }

                dmu_tx_hold_write(oh->ot_tx, obj->oo_db->db_object, offset,size);

                offset = lnb->lnb_file_offset;
                size = lnb->lnb_len;
        }

        if (size)
                dmu_tx_hold_write(oh->ot_tx, obj->oo_db->db_object, offset,size);

        dmu_tx_hold_sa(oh->ot_tx, obj->oo_sa_hdl, 0);

        oh->ot_write_commit = 1; /* used in osd_trans_start() for fail_loc */

        RETURN(0);
}

static int osd_write_commit(const struct lu_env *env, struct dt_object *dt,
                            struct niobuf_local *lnb, int npages,
                            struct thandle *th)
{
        struct osd_object  *obj  = osd_dt_obj(dt);
        struct osd_device  *osd = osd_obj2dev(obj);
        udmu_objset_t      *uos = &osd->od_objset;
        struct osd_thandle *oh;
        uint64_t            new_size = 0;
        int                 i, rc = 0;
        ENTRY;

        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        LASSERT(th != NULL);
        oh = container_of0(th, struct osd_thandle, ot_super);

        for (i = 0; i < npages; i++) {
                CDEBUG(D_INODE, "write %u bytes at %u\n",
                       (unsigned) lnb[i].lnb_len,
                       (unsigned) lnb[i].lnb_file_offset);

                if (lnb[i].lnb_rc) {
                        /* ENOSPC, network RPC error, etc.
                         * Unlike ldiskfs, zfs allocates new blocks on rewrite,
                         * so we skip this page if lnb_rc is set to -ENOSPC */
                        CDEBUG(D_INODE, "Skipping [%d] == %d\n", i, lnb[i].lnb_rc);
                        continue;
                }

                dmu_write(osd->od_objset.os, obj->oo_db->db_object,
                          lnb[i].lnb_file_offset, lnb[i].lnb_len,
                          kmap(lnb[i].lnb_page), oh->ot_tx);
                kunmap(lnb[i].lnb_page);
                if (new_size < lnb[i].lnb_file_offset + lnb[i].lnb_len)
                        new_size = lnb[i].lnb_file_offset + lnb[i].lnb_len;
        }

        if (unlikely(new_size == 0)) {
                /* no pages to write, no transno is needed */
                th->th_local = 1;
                /* it is important to return 0 even when all lnb_rc == -ENOSPC
                 * since ofd_commitrw_write() retries several times on ENOSPC */
                RETURN(0);
        }

        cfs_write_lock(&obj->oo_attr_lock);
        if (obj->oo_attr.la_size < new_size) {
                obj->oo_attr.la_size = new_size;
                rc = -sa_update(obj->oo_sa_hdl, SA_ZPL_SIZE(uos),
                                &obj->oo_attr.la_size, 8, oh->ot_tx);
        }
        cfs_write_unlock(&obj->oo_attr_lock);

        RETURN(rc);
}

static int osd_read_prep(const struct lu_env *env, struct dt_object *dt,
                         struct niobuf_local *lnb, int npages)
{
        struct osd_object *obj  = osd_dt_obj(dt);
        struct lu_buf      buf;
        loff_t             offset;
        int                i;

        LASSERT(dt_object_exists(dt));
        LASSERT(obj->oo_db);

        for (i = 0; i < npages; i++) {
                buf.lb_buf = kmap(lnb[i].lnb_page);
                buf.lb_len = lnb[i].lnb_len;
                offset = lnb[i].lnb_file_offset;

                CDEBUG(D_OTHER, "read %u bytes at %u\n",
                       (unsigned) lnb[i].lnb_len,
                       (unsigned) lnb[i].lnb_file_offset);
                lnb[i].lnb_rc = osd_read(env, dt, &buf, &offset, NULL);
                kunmap(lnb[i].lnb_page);

                if (lnb[i].lnb_rc < buf.lb_len) {
                        /* all subsequent rc should be 0 */
                        while (++i < npages)
                                lnb[i].lnb_rc = 0;
                        break;
                }
        }

        return 0;
}

static struct dt_body_operations osd_body_ops = {
        .dbo_read                 = osd_read,
        .dbo_declare_write        = osd_declare_write,
        .dbo_write                = osd_write,
        .dbo_bufs_get             = osd_bufs_get,
        .dbo_bufs_put             = osd_bufs_put,
        .dbo_write_prep           = osd_write_prep,
        .dbo_declare_write_commit = osd_declare_write_commit,
        .dbo_write_commit         = osd_write_commit,
        .dbo_read_prep            = osd_read_prep,
        .do_declare_punch         = osd_declare_punch,
        .do_punch                 = osd_punch,
};

/*
 * DMU OSD device type methods
 */
static int osd_type_init(struct lu_device_type *t)
{
        LU_CONTEXT_KEY_INIT(&osd_key);
        return lu_context_key_register(&osd_key);
}

static void osd_type_fini(struct lu_device_type *t)
{
        lu_context_key_degister(&osd_key);
}

static void *osd_key_init(const struct lu_context *ctx,
                          struct lu_context_key *key)
{
        struct osd_thread_info *info;

        OBD_ALLOC_PTR(info);
        if (info != NULL)
                info->oti_env = container_of(ctx, struct lu_env, le_ctx);
        else
                info = ERR_PTR(-ENOMEM);
        return info;
}

static void osd_key_fini(const struct lu_context *ctx,
                         struct lu_context_key *key, void *data)
{
        struct osd_thread_info *info = data;
        OBD_FREE_PTR(info);
}

static void osd_key_exit(const struct lu_context *ctx,
                         struct lu_context_key *key, void *data)
{
        struct osd_thread_info *info = data;
        memset(info, 0, sizeof(*info));
}

struct lu_context_key osd_key = {
        .lct_tags = LCT_DT_THREAD | LCT_MD_THREAD | LCT_MG_THREAD | LCT_LOCAL,
        .lct_init = osd_key_init,
        .lct_fini = osd_key_fini,
        .lct_exit = osd_key_exit
};

static int osd_shutdown(const struct lu_env *env, struct osd_device *o)
{
        RETURN(0);
}

static int osd_oi_find_or_create(const struct lu_env *env, struct osd_device *o,
                                 uint64_t parent, const char *name,
                                 uint64_t *child)
{
        struct zpl_direntry *zde = &osd_oti_get(env)->oti_zde.lzd_reg;
        dmu_buf_t *db;
        int rc;

        rc = -zap_lookup(o->od_objset.os, parent, name, 8, 1, (void *)zde);
        if (rc == 0) {
                rc = __osd_obj2dbuf(env, o->od_objset.os, zde->zde_dnode,
                                    &db, objdir_tag);
                if (rc)
                        return rc;
                *child = db->db_object;
                sa_buf_rele(db, objdir_tag);
        } else {
                struct lu_attr *la = &osd_oti_get(env)->oti_la;

                /* create fid-to-dnode index */
                dmu_tx_t *tx = dmu_tx_create(o->od_objset.os);
                if (tx == NULL)
                        return -ENOMEM;

                dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, 1, NULL);
                dmu_tx_hold_bonus(tx, parent);
                dmu_tx_hold_zap(tx, parent, TRUE, name);
                LASSERT(tx->tx_objset->os_sa);
                dmu_tx_hold_sa_create(tx, ZFS_SA_BASE_ATTR_SIZE);

                rc = -dmu_tx_assign(tx, TXG_WAIT);
                if (rc) {
                        dmu_tx_abort(tx);
                        RETURN(rc);
                }

                la->la_valid = LA_MODE | LA_UID | LA_GID;
                la->la_mode = S_IFDIR | S_IRUGO | S_IWUSR | S_IXUGO;
                la->la_uid = la->la_gid = 0;
                __osd_zap_create(env, &o->od_objset, &db, tx, la, osd_obj_tag);

                memset(zde, sizeof(*zde), 0);
                zde->zde_dnode = db->db_object;
                zde->zde_type = IFTODT(S_IFDIR);

                rc = -zap_add(o->od_objset.os, parent, name,
                              8, 1, (void*)zde, tx);

                dmu_tx_commit(tx);

                *child = db->db_object;
                sa_buf_rele(db, objdir_tag);
        }

        return rc;
}

static int osd_oi_init(const struct lu_env *env, struct osd_device *o)
{
        char      *name = osd_oti_get(env)->oti_buf;
        uint64_t   odb, sdb;
        dmu_buf_t *db;
        int        i, rc;
        ENTRY;

        rc = osd_oi_find_or_create(env, o, o->od_root, DMU_OSD_OI_NAME, &odb);
        if (rc)
                GOTO(out, rc);

        rc = __osd_obj2dbuf(env, o->od_objset.os, odb, &db, objdir_tag);
        if (rc)
                GOTO(out, rc);
        o->od_objdir = odb;
        sa_buf_rele(db, objdir_tag);

        /* create /O subdirectory to map legacy OST objects */
        rc = osd_oi_find_or_create(env, o, o->od_root, "O", &sdb);
        if (rc)
                GOTO(out, rc);

        /* create /O/0 subdirectory to map legacy OST objects */
        rc = osd_oi_find_or_create(env, o, sdb, "0", &odb);
        if (rc)
                GOTO(out, rc);

        o->od_ost_compat_grp0 = odb;

        for (i = 0; i < OSD_OST_MAP_SIZE; i++) {
                sprintf(name, "d%d", i);
                rc = osd_oi_find_or_create(env, o, odb, name, &sdb);
                if (rc)
                        break;
                o->od_ost_compat_dirs[i] = sdb;
        }

out:
        RETURN(rc);
}

static int osd_mount(const struct lu_env *env,
                     struct osd_device *o, struct lustre_cfg *cfg)
{
        char               *dev  = lustre_cfg_string(cfg, 1);
        dmu_buf_t          *rootdb;
        int                rc;
        ENTRY;

        if (o->od_objset.os != NULL)
                RETURN(0);

        if (strlen(dev) >= sizeof(o->od_mntdev))
                RETURN(-E2BIG);

        strcpy(o->od_mntdev, dev);

        rc = -udmu_objset_open(dev, &o->od_objset);
        if (rc) {
                CERROR("can't open objset %s: %d\n", dev, rc);
                RETURN(rc);
        }

        rc = __osd_obj2dbuf(env, o->od_objset.os, o->od_objset.root,
                            &rootdb, root_tag);
        if (rc) {
                CERROR("udmu_obj2dbuf() failed with error %d\n", rc);
                udmu_objset_close(&o->od_objset);
                RETURN(rc);
        }

        o->od_root = rootdb->db_object;
        sa_buf_rele(rootdb, root_tag);

        RETURN(rc);
}

static void osd_umount(const struct lu_env *env, struct osd_device *o)
{
        ENTRY;

        if (o->od_objset.os != NULL) {
                udmu_objset_close(&o->od_objset);
                o->od_objset.os = NULL;
        }

        EXIT;
}

static int osd_device_init0(const struct lu_env *env,
                            struct osd_device *o,
                            struct lustre_cfg *cfg)
{
        struct lu_device       *l = osd2lu_dev(o);
        char                   *label;
        int                     rc;

        /* if the module was re-loaded, env can loose its keys */
        rc = lu_env_refill((struct lu_env *) env);
        if (rc)
                GOTO(out, rc);

        l->ld_ops = &osd_lu_ops;
        o->od_dt_dev.dd_ops = &osd_dt_ops;

        o->od_capa_hash = init_capa_hash();
        if (o->od_capa_hash == NULL)
                GOTO(out, rc = -ENOMEM);

        rc = osd_mount(env, o, cfg);
        if (rc)
                GOTO(out_capa, rc);

        /* 1. initialize oi before any file create or file open */
        rc = osd_oi_init(env, o);
        if (rc)
                GOTO(out_oi, rc);

        rc = lu_site_init(&o->od_site, l);
        if (rc)
                GOTO(out_oi, rc);
        o->od_site.ls_bottom_dev = l;

        rc = lu_site_init_finish(&o->od_site);
        if (rc)
                GOTO(out_oi, rc);

        label = osd_label_get(env, &o->od_dt_dev);
        if (label == NULL)
                GOTO(out_oi, rc = -ENODEV);

        rc = osd_procfs_init(o, label);
        if (rc)
                GOTO(out_oi, rc);

        GOTO(out, rc);

out_oi:
        osd_umount(env, o);
out_capa:
        cleanup_capa_hash(o->od_capa_hash);
out:
        RETURN(rc);
}

static struct lu_device *osd_device_alloc(const struct lu_env *env,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *cfg)
{
        struct osd_device *o;
        int                rc;

        OBD_ALLOC_PTR(o);
        if (o == NULL)
                return ERR_PTR(-ENOMEM);

        rc = dt_device_init(&o->od_dt_dev, t);
        if (rc == 0) {
                rc = osd_device_init0(env, o, cfg);
                if (rc)
                        dt_device_fini(&o->od_dt_dev);
        }

        if (unlikely(rc != 0))
                OBD_FREE_PTR(o);

        return rc == 0 ? osd2lu_dev(o) : ERR_PTR(rc);
}

static struct lu_device *osd_device_free(const struct lu_env *env,
                                         struct lu_device *d)
{
        struct osd_device *o = osd_dev(d);
        ENTRY;

        cleanup_capa_hash(o->od_capa_hash);
        /* XXX: make osd top device in order to release reference */
        d->ld_site->ls_top_dev = d;
        lu_site_purge(env, d->ld_site, -1);
        lu_site_fini(&o->od_site);
        dt_device_fini(&o->od_dt_dev);
        OBD_FREE_PTR(o);

        RETURN (NULL);
}

static struct lu_device *osd_device_fini(const struct lu_env *env,
                                         struct lu_device *d)
{
        struct osd_device *o = osd_dev(d);
        int rc;
        ENTRY;

        if (o->od_objset.os) {
                osd_sync(env, lu2dt_dev(d));
                txg_wait_callbacks(spa_get_dsl(dmu_objset_spa(o->od_objset.os)));
        }

        rc = osd_procfs_fini(o);
        if (rc) {
                CERROR("proc fini error %d\n", rc);
                RETURN(ERR_PTR(rc));
        }

        if (o->od_objset.os)
                osd_umount(env, o);

        RETURN(NULL);
}

/*
 * To be removed, setup is performed by osd_device_{init,alloc} and
 * cleanup is performed by osd_device_{fini,free).
 */
static int osd_process_config(const struct lu_env *env,
                              struct lu_device *d, struct lustre_cfg *cfg)
{
        struct osd_device *o = osd_dev(d);
        int err;
        ENTRY;

        switch(cfg->lcfg_command) {
        case LCFG_SETUP:
                err = osd_mount(env, o, cfg);
                break;
        case LCFG_CLEANUP:
                err = osd_shutdown(env, o);
                break;
        default:
                err = -ENOTTY;
        }

        RETURN(err);
}

static int osd_recovery_complete(const struct lu_env *env, struct lu_device *d)
{
        ENTRY;
        RETURN(0);
}

/*
 * we use exports to track all osd users
 */
static int osd_obd_connect(const struct lu_env *env, struct obd_export **exp,
                           struct obd_device *obd, struct obd_uuid *cluuid,
                           struct obd_connect_data *data, void *localdata)
{
        struct osd_device    *osd = osd_dev(obd->obd_lu_dev);
        struct lustre_handle  conn;
        int                   rc;
        ENTRY;

        CDEBUG(D_CONFIG, "connect #%d\n", osd->od_connects);

        rc = class_connect(&conn, obd, cluuid);
        if (rc)
                RETURN(rc);

        *exp = class_conn2export(&conn);

        /* XXX: locking ? */
        osd->od_connects++;

        RETURN(0);
}

/*
 * once last export (we don't count self-export) disappeared
 * osd can be released
 */
static int osd_obd_disconnect(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        struct osd_device *osd = osd_dev(obd->obd_lu_dev);
        int                rc, release = 0;
        ENTRY;

        /* Only disconnect the underlying layers on the final disconnect. */
        /* XXX: locking ? */
        osd->od_connects--;
        if (osd->od_connects == 0)
                release = 1;

        rc = class_disconnect(exp); /* bz 9811 */

        if (rc == 0 && release)
                class_manual_cleanup(obd);
        RETURN(rc);
}

static int osd_start(const struct lu_env *env, struct lu_device *dev)
{
        return 0;
}

static struct lu_object_operations osd_lu_obj_ops = {
        .loo_object_init      = osd_object_init,
        .loo_object_delete    = osd_object_delete,
        .loo_object_release   = osd_object_release,
        .loo_object_free      = osd_object_free,
        .loo_object_print     = osd_object_print,
        .loo_object_invariant = osd_object_invariant
};

struct lu_device_operations osd_lu_ops = {
        .ldo_object_alloc      = osd_object_alloc,
        .ldo_process_config    = osd_process_config,
        .ldo_recovery_complete = osd_recovery_complete,
        .ldo_start             = osd_start,
};

static void osd_type_start(struct lu_device_type *t)
{
}

static void osd_type_stop(struct lu_device_type *t)
{
}

static struct lu_device_type_operations osd_device_type_ops = {
        .ldto_init = osd_type_init,
        .ldto_fini = osd_type_fini,

        .ldto_start = osd_type_start,
        .ldto_stop  = osd_type_stop,

        .ldto_device_alloc = osd_device_alloc,
        .ldto_device_free  = osd_device_free,

        .ldto_device_init    = NULL,
        .ldto_device_fini    = osd_device_fini
};

static struct lu_device_type osd_device_type = {
        .ldt_tags     = LU_DEVICE_DT,
        .ldt_name     = LUSTRE_OSD_ZFS_NAME,
        .ldt_ops      = &osd_device_type_ops,
        .ldt_ctx_tags = LCT_LOCAL
};


static struct obd_ops osd_obd_device_ops = {
        .o_owner       = THIS_MODULE,
        .o_connect     = osd_obd_connect,
        .o_disconnect  = osd_obd_disconnect,
};

int __init osd_init(void)
{
        int rc;

        rc = lu_kmem_init(osd_caches);
        if (rc)
                return rc;

        rc = class_register_type(&osd_obd_device_ops, NULL,
                                 lprocfs_osd_module_vars,
                                 LUSTRE_OSD_ZFS_NAME, &osd_device_type);
        if (rc)
                lu_kmem_fini(osd_caches);
        return rc;
}

void __exit osd_exit(void)
{
        class_unregister_type(LUSTRE_OSD_ZFS_NAME);
        lu_kmem_fini(osd_caches);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Object Storage Device ("LUSTRE_OSD_ZFS_NAME")");
MODULE_LICENSE("GPL");

cfs_module(osd, "0.6.0", osd_init, osd_exit);
