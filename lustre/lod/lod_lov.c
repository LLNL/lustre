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
 * Copyright  2009 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lod/lod_lov.c
 *
 * Author: Alex Zhuravlev <bzzz@sun.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <obd_class.h>
#include <obd_lov.h>

#include "lod_internal.h"

int lod_add_device(const struct lu_env *env, struct lod_device *lod,
                       char *osp, unsigned index, unsigned gen)
{
        struct obd_connect_data *data = NULL;
        struct obd_export       *exp = NULL;
        struct obd_device       *obd;
        struct lu_device        *ldev;
        struct dt_device        *d;
        int                      rc;
        ENTRY;

        CDEBUG(D_CONFIG, "osp:%s idx:%d gen:%d\n", osp, index, gen);

        /* XXX: should be dynamic */
        if (index >= LOD_MAX_OSTNR) {
                CERROR("too large index %d\n", index);
                RETURN(-EINVAL);
        }

        if (gen <= 0) {
                CERROR("request to add OBD %s with invalid generation: %d\n",
                       osp, gen);
                RETURN(-EINVAL);
        }

        obd = class_name2obd(osp);
        if (obd == NULL) {
                CERROR("can't find %s device\n", osp);
                RETURN(-EINVAL);
        }

        OBD_ALLOC_PTR(data);
        if (data == NULL)
                RETURN(-ENOMEM);
        /* XXX: which flags we need on MDS? */
        data->ocd_version = LUSTRE_VERSION_CODE;
        data->ocd_connect_flags |= OBD_CONNECT_INDEX;
        data->ocd_index = index;

        rc = obd_connect(env, &exp, obd, &obd->obd_uuid, data, NULL);
        if (rc) {
                CERROR("%s: cannot connect to next dev %s (%d)\n",
                       obd->obd_name, osp, rc);
                GOTO(out_free, rc);
        }

        LASSERT(obd->obd_lu_dev);
        LASSERT(obd->obd_lu_dev->ld_site = lod->lod_dt_dev.dd_lu_dev.ld_site);

        ldev = obd->obd_lu_dev;
        d = lu2dt_dev(ldev);

        cfs_mutex_down(&lod->lod_mutex);
        if (cfs_bitmap_check(lod->lod_ost_bitmap, index)) {
                CERROR("%s: device %d is registered already\n", obd->obd_name,
                       index);
                GOTO(out_mutex, rc = -EINVAL);
        }

        lod->lod_osts[index].ltd_ost    = d;
        lod->lod_osts[index].ltd_exp    = exp;
        lod->lod_osts[index].ltd_uuid   = obd->u.cli.cl_target_uuid;
        lod->lod_osts[index].ltd_gen    = gen;
        lod->lod_osts[index].ltd_index  = index;
        /* XXX: non-active targets not supported yet */
        lod->lod_osts[index].ltd_active = 1;

        /* XXX: we pass LOD_MAX_OSTNR for now, should be fixed once lod_osts can
         * grow dynamically */
        rc = lod_ost_pool_add(&lod->lod_pool_info, index, LOD_MAX_OSTNR);
        if (rc) {
                CERROR("%s: can't set up pool: %d\n", obd->obd_name, rc);
                GOTO(out_mutex, rc);
        }

        rc = qos_add_tgt(lod, index, exp);
        if (rc) {
                CERROR("%s: qos_add failed, %d\n", obd->obd_name, rc);
                GOTO(out_mutex, rc);
        }

        lod->lod_ostnr++;
        cfs_bitmap_set(lod->lod_ost_bitmap, index);
        if (index >= lod->lod_desc.ld_tgt_count)
                lod->lod_desc.ld_tgt_count = index + 1;

        if (lod->lod_recovery_completed)
                ldev->ld_ops->ldo_recovery_complete(env, ldev);
out_mutex:
        cfs_mutex_up(&lod->lod_mutex);
        if (rc)
                obd_disconnect(exp);
out_free:
        OBD_FREE_PTR(data);
        RETURN(rc);
}

/*
 * allocates a buffer
 * generate LOV EA for given striped object into the buffer
 *
 */
int lod_generate_and_set_lovea(const struct lu_env *env,
                               struct lod_object *mo, struct thandle *th)
{
        struct lod_thread_info *info = lod_env_info(env);
        struct dt_object       *next = dt_object_child(&mo->mbo_obj);
        const struct lu_fid    *fid  = lu_object_fid(&mo->mbo_obj.do_lu);
        struct lov_mds_md_v1   *lmm;
        struct lov_ost_data_v1 *objs;
        __u32                   magic;
        int                     i, rc;
        ENTRY;

        LASSERT(mo);
        LASSERT(mo->mbo_stripenr > 0);

        magic = mo->mbo_pool ? LOV_MAGIC_V3 : LOV_MAGIC_V1;
        info->lti_buf.lb_len = lov_mds_md_size(mo->mbo_stripenr, magic);

        /* XXX: use thread info to save on allocation? */
        OBD_ALLOC(lmm, info->lti_buf.lb_len);
        if (lmm == NULL)
                RETURN(-ENOMEM);
        info->lti_buf.lb_buf = lmm;

        lmm->lmm_magic = cpu_to_le32(magic);
        lmm->lmm_pattern = cpu_to_le32(LOV_PATTERN_RAID0);
        lmm->lmm_object_id = cpu_to_le64(fid_ver_oid(fid));
        lmm->lmm_object_seq = cpu_to_le64(fid_seq(fid));
        lmm->lmm_stripe_size = cpu_to_le32(mo->mbo_stripe_size);
        lmm->lmm_stripe_count = cpu_to_le16(mo->mbo_stripenr);
        lmm->lmm_layout_gen = 0;
        if (magic == LOV_MAGIC_V1) {
                objs = &lmm->lmm_objects[0];
        } else {
                struct lov_mds_md_v3 *v3 = (struct lov_mds_md_v3 *) lmm;
                strncpy(v3->lmm_pool_name, mo->mbo_pool, LOV_MAXPOOLNAME);
                objs = &v3->lmm_objects[0];
        }

        for (i = 0; i < mo->mbo_stripenr; i++) {
                const struct lu_fid *fid;

                LASSERT(mo->mbo_stripe[i]);
                fid = lu_object_fid(&mo->mbo_stripe[i]->do_lu);

                rc = fid_ostid_pack(fid, &info->lti_ostid);
                LASSERT(rc == 0);
                LASSERT(info->lti_ostid.oi_seq == FID_SEQ_OST_MDT0);

                objs[i].l_object_id  = cpu_to_le64(info->lti_ostid.oi_id);
                objs[i].l_object_seq = cpu_to_le64(info->lti_ostid.oi_seq);
                objs[i].l_ost_gen    = cpu_to_le32(1); /* XXX */
                objs[i].l_ost_idx    = cpu_to_le32(fid_idif_ost_idx(fid));
        }

        rc = dt_xattr_set(env, next, &info->lti_buf, XATTR_NAME_LOV, 0, th,
                          BYPASS_CAPA);

        OBD_FREE(lmm, info->lti_buf.lb_len);

        RETURN(rc);
}

int lod_get_lov_ea(const struct lu_env *env, struct lod_object *mo)
{
        struct lod_thread_info *info = lod_env_info(env);
        struct dt_object       *next = dt_object_child(&mo->mbo_obj);
        int                     rc;
        ENTRY;

        /* we really don't support that large striping yet? */
        LASSERT(info);
        LASSERT(info->lti_ea_store_size < 1024*1024);

        if (unlikely(info->lti_ea_store == NULL)) {
                /* XXX: set initial allocation to fit default fs striping */
                LASSERT(info->lti_ea_store_size == 0);
                OBD_ALLOC(info->lti_ea_store, 512);
                if (info->lti_ea_store == NULL)
                        RETURN(-ENOMEM);
                info->lti_ea_store_size = 512;
        }

repeat:
        info->lti_buf.lb_buf = info->lti_ea_store;
        info->lti_buf.lb_len = info->lti_ea_store_size;
        rc = dt_xattr_get(env, next, &info->lti_buf, XATTR_NAME_LOV,
                          BYPASS_CAPA);

        /* if object is not striped or inaccessible */
        if (rc == -ENODATA)
                RETURN(0);

        if (rc == -ERANGE) {
                /* EA doesn't fit, reallocate new buffer */
                /* XXX: what's real limit? */
                LASSERT(info->lti_buf.lb_len <= 16 * 1024);
                if (info->lti_ea_store) {
                        LASSERT(info->lti_ea_store_size);
                        OBD_FREE(info->lti_ea_store, info->lti_ea_store_size);
                        info->lti_ea_store = NULL;
                        info->lti_ea_store_size = 0;
                }

                OBD_ALLOC(info->lti_ea_store, info->lti_buf.lb_len * 2);
                if (info->lti_ea_store == NULL)
                        RETURN(-ENOMEM);
                info->lti_ea_store_size = info->lti_buf.lb_len * 2;

                GOTO(repeat, rc);
        }

        RETURN(rc);
}

/*
 * allocate array of objects pointers, find/create objects
 * stripenr and other fields should be initialized by this moment
 */
int lod_initialize_objects(const struct lu_env *env, struct lod_object *mo,
                           struct lov_ost_data_v1 *objs)
{
        struct lod_thread_info *info = lod_env_info(env);
        struct lod_device  *md = lu2lod_dev(mo->mbo_obj.do_lu.lo_dev);
        struct lu_object   *o, *n;
        struct lu_device   *nd;
        int                 i, idx, rc = 0;
        ENTRY;

        LASSERT(mo);
        LASSERT(mo->mbo_stripe == NULL);
        LASSERT(mo->mbo_stripenr > 0);
        LASSERT(mo->mbo_stripe_size > 0);

        i = sizeof(struct dt_object *) * mo->mbo_stripenr;
        OBD_ALLOC(mo->mbo_stripe, i);
        if (mo->mbo_stripe == NULL)
                GOTO(out, rc = -ENOMEM);
        mo->mbo_stripes_allocated = mo->mbo_stripenr;

        for (i = 0; i < mo->mbo_stripenr; i++) {

                info->lti_ostid.oi_id = le64_to_cpu(objs[i].l_object_id);
                /* XXX: support for CMD? */
                info->lti_ostid.oi_seq = le64_to_cpu(objs[i].l_object_seq);
                idx = le64_to_cpu(objs[i].l_ost_idx);
                fid_ostid_unpack(&info->lti_fid, &info->lti_ostid, idx);

                /*
                 * XXX: assertion is left for testing, to make
                 * sure we never process requests till configuration
                 * is completed. to be changed to -EINVAL
                 */

                LASSERTF(md->lod_osts[idx].ltd_ost, "idx %d\n", idx);
                nd = &md->lod_osts[idx].ltd_ost->dd_lu_dev;

                o = lu_object_find_at(env, nd, &info->lti_fid, NULL);
                if (IS_ERR(o))
                        GOTO(out, rc = PTR_ERR(o));

                n = lu_object_locate(o->lo_header, nd->ld_type);
                if (unlikely(n == NULL)) {
                        CERROR("can't find slice\n");
                        lu_object_put(env, o);
                        GOTO(out, rc = -EINVAL);
                }

                mo->mbo_stripe[i] = container_of(n, struct dt_object, do_lu);
        }

out:
        RETURN(rc);
}

/*
 * Parse striping information stored in lti_ea_store
 */
int lod_parse_striping(const struct lu_env *env, struct lod_object *mo,
                       const struct lu_buf *buf)
{
        struct lov_mds_md_v1   *lmm;
        struct lov_ost_data_v1 *objs;
        __u32                   magic;
        int                     rc = 0;
        ENTRY;

        LASSERT(buf);
        LASSERT(buf->lb_buf);
        LASSERT(buf->lb_len);

        lmm = (struct lov_mds_md_v1 *) buf->lb_buf;
        magic = le32_to_cpu(lmm->lmm_magic);

        if (magic != LOV_MAGIC_V1 && magic != LOV_MAGIC_V3)
                GOTO(out, rc = -EINVAL);
        if (le32_to_cpu(lmm->lmm_pattern) != LOV_PATTERN_RAID0)
                GOTO(out, rc = -EINVAL);

        mo->mbo_stripe_size = le32_to_cpu(lmm->lmm_stripe_size);
        mo->mbo_stripenr = le16_to_cpu(lmm->lmm_stripe_count);
        mo->mbo_layout_gen = le16_to_cpu(lmm->lmm_layout_gen);

        LASSERT(buf->lb_len >= lov_mds_md_size(mo->mbo_stripenr, magic));

        if (magic == LOV_MAGIC_V3) {
                struct lov_mds_md_v3 *v3 = (struct lov_mds_md_v3 *) lmm;
                objs = &v3->lmm_objects[0];
                lod_object_set_pool(mo, v3->lmm_pool_name);
        } else {
                objs = &lmm->lmm_objects[0];
        }

        rc = lod_initialize_objects(env, mo, objs);

out:
        RETURN(rc);
}

/*
 * Load and parse striping information, create in-core representation for the
 * stripes
 */
int lod_load_striping(const struct lu_env *env, struct lod_object *mo)
{
        struct dt_object       *next = dt_object_child(&mo->mbo_obj);
        struct lod_thread_info *info = lod_env_info(env);
        int                     rc;
        ENTRY;

        /*
         * currently this code is supposed to be called from declaration
         * phase only, thus the object is not expected to be locked by caller
         */
        dt_write_lock(env, next, 0);
        /* already initialized? */
        if (mo->mbo_stripe) {
                int i;
                /* check validity */
                for (i = 0; i < mo->mbo_stripenr; i++)
                        LASSERTF(mo->mbo_stripe[i], "stripe %d is NULL\n", i);
                GOTO(out, rc = 0);
        }

        if (!dt_object_exists(next))
                GOTO(out, rc = 0);

        /* only regular files can be striped */
        if (!(lu_object_attr(lod2lu_obj(mo)) & S_IFREG))
                GOTO(out, rc = 0);

        LASSERT(mo->mbo_stripenr == 0);

        rc = lod_get_lov_ea(env, mo);
        if (rc <= 0)
                GOTO(out, rc);

        /*
         * there is LOV EA (striping information) in this object
         * let's parse it and create in-core objects for the stripes
         */
        info->lti_buf.lb_buf = info->lti_ea_store;
        info->lti_buf.lb_len = info->lti_ea_store_size;
        rc = lod_parse_striping(env, mo, &info->lti_buf);
out:
        dt_write_unlock(env, next);
        RETURN(rc);
}

int lod_store_def_striping(const struct lu_env *env, struct dt_object *dt,
                           struct thandle *th)
{
        struct lod_thread_info *info = lod_env_info(env);
        struct lod_object     *mo = lod_dt_obj(dt);
        struct dt_object      *next = dt_object_child(dt);
        struct lov_user_md_v3 *v3;
        int                    rc;
        ENTRY;

        LASSERT(S_ISDIR(dt->do_lu.lo_header->loh_attr));

        /*
         * store striping defaults into new directory
         * used to implement defaults inheritance
         */

        /* probably nothing to inherite */
        if (mo->mbo_striping_cached == 0)
                RETURN(0);

        if (LOVEA_DELETE_VALUES(mo->mbo_def_stripe_size, mo->mbo_def_stripenr,
                                mo->mbo_def_stripe_offset))
                RETURN(0);

        OBD_ALLOC_PTR(v3);
        if (v3 == NULL)
                RETURN(-ENOMEM);

        v3->lmm_magic = cpu_to_le32(LOV_MAGIC_V3);
        v3->lmm_pattern = cpu_to_le32(LOV_PATTERN_RAID0);
        v3->lmm_object_id = 0;
        v3->lmm_object_seq = 0;
        v3->lmm_stripe_size = cpu_to_le32(mo->mbo_def_stripe_size);
        v3->lmm_stripe_count = cpu_to_le16(mo->mbo_def_stripenr);
        v3->lmm_stripe_offset = cpu_to_le16(mo->mbo_def_stripe_offset);
        if (mo->mbo_pool)
                strncpy(v3->lmm_pool_name, mo->mbo_pool, LOV_MAXPOOLNAME);

        info->lti_buf.lb_buf = v3;
        info->lti_buf.lb_len = sizeof(*v3);
        rc = dt_xattr_set(env, next, &info->lti_buf, XATTR_NAME_LOV, 0, th,
                          BYPASS_CAPA);

        OBD_FREE_PTR(v3);

        RETURN(rc);
}

void lod_fix_desc_stripe_size(__u64 *val)
{
        if (*val < PTLRPC_MAX_BRW_SIZE) {
                LCONSOLE_WARN("Increasing default stripe size to min %u\n",
                              PTLRPC_MAX_BRW_SIZE);
                *val = PTLRPC_MAX_BRW_SIZE;
        } else if (*val & (LOV_MIN_STRIPE_SIZE - 1)) {
                *val &= ~(LOV_MIN_STRIPE_SIZE - 1);
                LCONSOLE_WARN("Changing default stripe size to "LPU64" (a "
                              "multiple of %u)\n",
                              *val, LOV_MIN_STRIPE_SIZE);
        }
}

void lod_fix_desc_stripe_count(__u32 *val)
{
        if (*val == 0)
                *val = 1;
}

void lod_fix_desc_pattern(__u32 *val)
{
        /* from lov_setstripe */
        if ((*val != 0) && (*val != LOV_PATTERN_RAID0)) {
                LCONSOLE_WARN("Unknown stripe pattern: %#x\n", *val);
                *val = 0;
        }
}

void lod_fix_desc_qos_maxage(__u32 *val)
{
        /* fix qos_maxage */
        if (*val == 0)
                *val = QOS_DEFAULT_MAXAGE;
}

void lod_fix_desc(struct lov_desc *desc)
{
        lod_fix_desc_stripe_size(&desc->ld_default_stripe_size);
        lod_fix_desc_stripe_count(&desc->ld_default_stripe_count);
        lod_fix_desc_pattern(&desc->ld_pattern);
        lod_fix_desc_qos_maxage(&desc->ld_qos_maxage);
}

int lod_lov_init(struct lod_device *lod, struct lustre_cfg *lcfg)
{
        struct lprocfs_static_vars  lvars = { 0 };
        struct obd_device          *obd;
        struct lov_desc            *desc;
        int                         rc;
        ENTRY;

        obd = class_name2obd(lustre_cfg_string(lcfg, 0));
        LASSERT(obd != NULL);
        obd->obd_lu_dev = &lod->lod_dt_dev.dd_lu_dev;

        if (LUSTRE_CFG_BUFLEN(lcfg, 1) < 1) {
                CERROR("LOD setup requires a descriptor\n");
                RETURN(-EINVAL);
        }

        desc = (struct lov_desc *)lustre_cfg_buf(lcfg, 1);

        if (sizeof(*desc) > LUSTRE_CFG_BUFLEN(lcfg, 1)) {
                CERROR("descriptor size wrong: %d > %d\n",
                       (int)sizeof(*desc), LUSTRE_CFG_BUFLEN(lcfg, 1));
                RETURN(-EINVAL);
        }

        if (desc->ld_magic != LOV_DESC_MAGIC) {
                if (desc->ld_magic == __swab32(LOV_DESC_MAGIC)) {
                            CDEBUG(D_OTHER, "%s: Swabbing lov desc %p\n",
                                   obd->obd_name, desc);
                            lustre_swab_lov_desc(desc);
                } else {
                        CERROR("%s: Bad lov desc magic: %#x\n",
                               obd->obd_name, desc->ld_magic);
                        RETURN(-EINVAL);
                }
        }

        lod_fix_desc(desc);

        desc->ld_active_tgt_count = 0;
        lod->lod_desc = *desc;

        lod->lod_sp_me = LUSTRE_SP_CLI;

        /* Set up allocation policy (QoS and RR) */
        CFS_INIT_LIST_HEAD(&lod->lod_qos.lq_oss_list);
        cfs_init_rwsem(&lod->lod_qos.lq_rw_sem);
        lod->lod_qos.lq_dirty = 1;
        lod->lod_qos.lq_rr.lqr_dirty = 1;
        lod->lod_qos.lq_reset = 1;
        /* Default priority is toward free space balance */
        lod->lod_qos.lq_prio_free = 232;
        /* Default threshold for rr (roughly 17%) */
        lod->lod_qos.lq_threshold_rr = 43;
        /* Init statfs fields */
        OBD_ALLOC_PTR(lod->lod_qos.lq_statfs_data);
        if (NULL == lod->lod_qos.lq_statfs_data)
                RETURN(-ENOMEM);
        cfs_waitq_init(&lod->lod_qos.lq_statfs_waitq);

        /* Set up OST pool environment */
        lod->lod_pools_hash_body = cfs_hash_create("POOLS", HASH_POOLS_CUR_BITS,
                                                   HASH_POOLS_MAX_BITS,
                                                   HASH_POOLS_BKT_BITS, 0,
                                                   CFS_HASH_MIN_THETA,
                                                   CFS_HASH_MAX_THETA,
                                                   &pool_hash_operations,
                                                   CFS_HASH_DEFAULT);
        if (!lod->lod_pools_hash_body)
                GOTO(out_statfs, rc = -ENOMEM);
        CFS_INIT_LIST_HEAD(&lod->lod_pool_list);
        lod->lod_pool_count = 0;
        rc = lod_ost_pool_init(&lod->lod_pool_info, 0);
        if (rc)
                GOTO(out_hash, rc);
        rc = lod_ost_pool_init(&lod->lod_qos.lq_rr.lqr_pool, 0);
        if (rc)
                GOTO(out_pool_info, rc);

        /* Allocate OST array and bitmap */
        lod->lod_ost_bitmap = CFS_ALLOCATE_BITMAP(LOD_MAX_OSTNR);
        if (!lod->lod_ost_bitmap)
                GOTO(out_pool_rr, rc = -ENOMEM);
        OBD_ALLOC(lod->lod_osts, sizeof(struct lod_ost_desc) * LOD_MAX_OSTNR);
        if (!lod->lod_osts)
                GOTO(out_bitmap, rc = -ENOMEM);

        lprocfs_lod_init_vars(&lvars);
        lprocfs_obd_setup(obd, lvars.obd_vars);

#ifdef LPROCFS
        rc = lprocfs_seq_create(obd->obd_proc_entry, "target_obd",
                                0444, &lod_proc_target_fops, obd);
        if (rc) {
                CWARN("%s: Error adding the target_obd file %d\n",
                      obd->obd_name, rc);
                GOTO(out_array, rc);
        }
        lod->lod_pool_proc_entry = lprocfs_register("pools",
                                                    obd->obd_proc_entry,
                                                    NULL, NULL);
        if (IS_ERR(lod->lod_pool_proc_entry)) {
                int ret = PTR_ERR(lod->lod_pool_proc_entry);
                lod->lod_pool_proc_entry = NULL;
                CWARN("%s: Failed to create pool proc file %d\n", obd->obd_name,
                      ret);
                rc = lod_lov_fini(lod);
                RETURN(ret);
        }
#endif

        RETURN(0);

out_array:
        lprocfs_obd_cleanup(obd);
        OBD_FREE(lod->lod_osts, sizeof(struct lod_ost_desc) * LOD_MAX_OSTNR);
out_bitmap:
        CFS_FREE_BITMAP(lod->lod_ost_bitmap);
out_pool_rr:
        lod_ost_pool_free(&lod->lod_qos.lq_rr.lqr_pool);
out_pool_info:
        lod_ost_pool_free(&lod->lod_pool_info);
out_hash:
        cfs_hash_putref(lod->lod_pools_hash_body);
out_statfs:
        OBD_FREE_PTR(lod->lod_qos.lq_statfs_data);
        return rc;
}

int lod_lov_fini(struct lod_device *lod)
{
        struct obd_device   *obd = lod2obd(lod);
        cfs_list_t          *pos, *tmp;
        struct pool_desc    *pool;
        struct obd_export   *exp;
        int                  i, rc;
        ENTRY;

        cfs_list_for_each_safe(pos, tmp, &lod->lod_pool_list) {
                pool = cfs_list_entry(pos, struct pool_desc, pool_list);
                /* free pool structs */
                CDEBUG(D_INFO, "delete pool %p\n", pool);
                lod_pool_del(obd, pool->pool_name);
        }
        cfs_hash_putref(lod->lod_pools_hash_body);
        lod_ost_pool_free(&(lod->lod_qos.lq_rr.lqr_pool));
        lod_ost_pool_free(&lod->lod_pool_info);

        cfs_foreach_bit(lod->lod_ost_bitmap, i) {
                exp = lod->lod_osts[i].ltd_exp;
                if (exp == NULL)
                        continue;

                rc = qos_del_tgt(lod, i);
                LASSERT(rc == 0);

                rc = obd_disconnect(exp);
                if (rc)
                        CERROR("error in disconnect from #%u: %d\n", i, rc);
        }

        /* clear pools parent proc entry only after all pools are killed */
        if (lod->lod_pool_proc_entry) {
                lprocfs_remove(&lod->lod_pool_proc_entry);
                lod->lod_pool_proc_entry = NULL;
        }

        lprocfs_obd_cleanup(obd);

        OBD_FREE_PTR(lod->lod_qos.lq_statfs_data);
        CFS_FREE_BITMAP(lod->lod_ost_bitmap);
        OBD_FREE(lod->lod_osts, sizeof(struct lod_ost_desc) * LOD_MAX_OSTNR);

        RETURN(0);
}
