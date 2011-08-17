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
 * lustre/lod/lod_dev.c
 *
 * Lustre Logical Object Device
 *
 * Author: Alex Zhuravlev <bzzz@sun.com>
 * Author: Mikhail Pershin <tappro@whamcloud.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <obd_class.h>
#include <lustre_param.h>

#include "lod_internal.h"

extern struct lu_object_operations lod_lu_obj_ops;
extern struct dt_object_operations lod_obj_ops;

struct lu_object *lod_object_alloc(const struct lu_env *env,
                                    const struct lu_object_header *hdr,
                                    struct lu_device *d)
{
        struct lod_object *o;

        OBD_ALLOC_PTR(o);
        if (o != NULL) {
                struct lu_object *l;

                l = lod2lu_obj(o);
                dt_object_init(&o->mbo_obj, NULL, d);
                o->mbo_obj.do_ops = &lod_obj_ops;
                l->lo_ops = &lod_lu_obj_ops;
                return l;
        } else {
                return NULL;
        }
}

static int lod_process_config(const struct lu_env *env,
                               struct lu_device *dev,
                               struct lustre_cfg *lcfg)
{
        struct lod_device *lod = lu2lod_dev(dev);
        struct lu_device  *next = &lod->lod_child->dd_lu_dev;
        char              *arg1;
        int                rc, i;
        ENTRY;

        switch(lcfg->lcfg_command) {
                case LCFG_LOV_DEL_OBD:
                case LCFG_LOV_ADD_INA:
                case LCFG_LOV_ADD_OBD: {
                        __u32 index;
                        int gen;
                        /* lov_modify_tgts add  0:lov_mdsA  1:osp  2:0  3:1 */
                        arg1 = lustre_cfg_string(lcfg, 1);

                        if (sscanf(lustre_cfg_buf(lcfg, 2), "%d", &index) != 1)
                                GOTO(out, rc = -EINVAL);
                        if (sscanf(lustre_cfg_buf(lcfg, 3), "%d", &gen) != 1)
                                GOTO(out, rc = -EINVAL);
                        if (lcfg->lcfg_command == LCFG_LOV_ADD_OBD)
                                rc = lod_add_device(env, lod, arg1, index, gen,
                                                    1);
                        else if (lcfg->lcfg_command == LCFG_LOV_ADD_INA)
                                rc = lod_add_device(env, lod, arg1, index, gen,
                                                    0);
                        else
                                rc = lod_del_device(env, lod, arg1, index, gen);
                        break;
                }
                case LCFG_PARAM: {
                        struct lprocfs_static_vars  lvars = { 0 };
                        struct obd_device          *obd = lod2obd(lod);

                        lprocfs_lod_init_vars(&lvars);

                        rc = class_process_proc_param(PARAM_LOV, lvars.obd_vars,
                                                      lcfg, obd);
                        if (rc > 0)
                                rc = 0;
                        GOTO(out, rc);
                }

                case LCFG_CLEANUP:
                        lod_getref(lod);
                        if (lod->lod_osts_size > 0)
                                cfs_foreach_bit(lod->lod_ost_bitmap, i) {
                                        LASSERT(lod->lod_osts[i] &&
                                                lod->lod_osts[i]->ltd_ost);
                                        next =
                                          &lod->lod_osts[i]->ltd_ost->dd_lu_dev;
                                        rc =
                                           next->ld_ops->ldo_process_config(env,
                                                                            next,
                                                                            lcfg);
                                        if (rc)
                                                CERROR("can't process %u: %d\n",
                                                       lcfg->lcfg_command, rc);
                                }
                        lod_putref(lod);
                        /*
                         * do cleanup on underlying storage only when
                         * all OSPs are cleaned up, as they use that OSD as well
                         */
                        next = &lod->lod_child->dd_lu_dev;
                        rc = next->ld_ops->ldo_process_config(env, next, lcfg);
                        if (rc)
                                CERROR("can't process %u: %d\n",
                                       lcfg->lcfg_command, rc);
                        break;

                default:
                        CERROR("unknown command %u\n", lcfg->lcfg_command);
                        rc = 0;
                        break;
        }

out:
        RETURN(rc);
}

static int lod_recovery_complete(const struct lu_env *env,
                                 struct lu_device *dev)
{
        struct lod_device *lod = lu2lod_dev(dev);
        struct lu_device  *next = &lod->lod_child->dd_lu_dev;
        int                i, rc;
        ENTRY;

        LASSERT(lod->lod_recovery_completed == 0);
        lod->lod_recovery_completed = 1;

        rc = next->ld_ops->ldo_recovery_complete(env, next);

        lod_getref(lod);
        if (lod->lod_osts_size > 0)
                cfs_foreach_bit(lod->lod_ost_bitmap, i) {
                        LASSERT(lod->lod_osts[i] && lod->lod_osts[i]->ltd_ost);
                        next = &lod->lod_osts[i]->ltd_ost->dd_lu_dev;
                        rc = next->ld_ops->ldo_recovery_complete(env, next);
                        if (rc)
                                CERROR("can't complete recovery on #%d: %d\n",
                                       i, rc);
                }
        lod_putref(lod);

        RETURN(rc);
}

static int lod_start(const struct lu_env *env,
                       struct lu_device *dev)
{
        struct lod_device *lod = lu2lod_dev(dev);
        struct lu_device  *next = &lod->lod_child->dd_lu_dev;
        int rc;
        ENTRY;
        rc = next->ld_ops->ldo_start(env, next);
        RETURN(rc);
}

const struct lu_device_operations lod_lu_ops = {
        .ldo_object_alloc      = lod_object_alloc,
        .ldo_process_config    = lod_process_config,
        .ldo_recovery_complete = lod_recovery_complete,
        .ldo_start             = lod_start,
};

static int lod_root_get(const struct lu_env *env,
                        struct dt_device *dev, struct lu_fid *f)
{
        struct lod_device *d = dt2lod_dev(dev);
        return dt_root_get(env, d->lod_child, f);
}

static int lod_statfs(const struct lu_env *env,
                      struct dt_device *dev, struct obd_statfs *sfs)
{
        struct lod_device *d = dt2lod_dev(dev);
        return dt_statfs(env, d->lod_child, sfs);
}

static struct thandle *lod_trans_create(const struct lu_env *env,
                                        struct dt_device *dev)
{
        struct lod_device *d = dt2lod_dev(dev);
        return dt_trans_create(env, d->lod_child);
}

static int lod_trans_start(const struct lu_env *env, struct dt_device *dev,
                           struct thandle *th)
{
        struct lod_device *d = dt2lod_dev(dev);
        return dt_trans_start(env, d->lod_child, th);
}

static int lod_trans_stop(const struct lu_env *env, struct thandle *th)
{
        /* XXX: currently broken as we don't know next device */
        LASSERT(th);
        LASSERT(th->th_dev);
        return dt_trans_stop(env, th->th_dev, th);
}

static void lod_conf_get(const struct lu_env *env,
                         const struct dt_device *dev,
                         struct dt_device_param *param)
{
        struct lod_device *d = dt2lod_dev((struct dt_device *) dev);
        LASSERT(d->lod_child);
        return dt_conf_get(env, d->lod_child, param);
}

static int lod_sync(const struct lu_env *env, struct dt_device *dev)
{
        struct lod_device *d = dt2lod_dev(dev);
        int                rc = 0, i;
        ENTRY;

        lod_getref(d);
        if (d->lod_osts_size > 0)
                cfs_foreach_bit(d->lod_ost_bitmap, i) {
                        LASSERT(d->lod_osts[i] && d->lod_osts[i]->ltd_ost);
                        rc = dt_sync(env, d->lod_osts[i]->ltd_ost);
                        if (rc) {
                                CERROR("can't sync %u: %d\n", i, rc);
                                break;
                        }
                }
        lod_putref(d);
        if (rc == 0)
                rc = dt_sync(env, d->lod_child);

        RETURN(rc);
}

static void lod_ro(const struct lu_env *env, struct dt_device *dev)
{
        struct lod_device *d = dt2lod_dev(dev);
        LASSERT(d->lod_child);
        return dt_ro(env, d->lod_child);
}

static int lod_commit_async(const struct lu_env *env, struct dt_device *dev)
{
        struct lod_device *d = dt2lod_dev(dev);
        struct dt_device  *next = d->lod_child;
        LASSERT(next);
        return next->dd_ops->dt_commit_async(env, next);
}

static int lod_init_capa_ctxt(const struct lu_env *env,
                                   struct dt_device *dev,
                                   int mode, unsigned long timeout,
                                   __u32 alg, struct lustre_capa_key *keys)
{
        struct lod_device *d = dt2lod_dev(dev);
        struct dt_device  *next = d->lod_child;
        LASSERT(next);
        return next->dd_ops->dt_init_capa_ctxt(env, next, mode, timeout,
                                               alg, keys);
}

#if 0
static void lod_init_quota_ctxt(const struct lu_env *env,
                                   struct dt_device *dev,
                                   struct dt_quota_ctxt *ctxt, void *data)
{
        struct lod_device *d = dt2lod_dev(dev);
        struct dt_device   *next = d->lod_child;
        ENTRY;

        next->dd_ops->dt_init_quota_ctxt(env, next, ctxt, data);

        EXIT;
}
#endif

static char *lod_label_get(const struct lu_env *env,
                           const struct dt_device *dev)
{
        struct lod_device *d = dt2lod_dev((struct dt_device *) dev);
        struct dt_device  *next = d->lod_child;
        LASSERT(next);
        return next->dd_ops->dt_label_get(env, next);
}


static int lod_label_set(const struct lu_env *env, const struct dt_device *dev,
                         char *l)
{
        struct lod_device *d = dt2lod_dev((struct dt_device *) dev);
        struct dt_device  *next = d->lod_child;
        LASSERT(next);
        return next->dd_ops->dt_label_set(env, next, l);
}

static int lod_quota_setup(const struct lu_env *env, struct dt_device *dev,
                           void *data)
{
        struct lod_device *d = dt2lod_dev((struct dt_device *) dev);
        struct dt_device  *next = d->lod_child;
        LASSERT(next);
        return next->dd_ops->dt_quota.dt_setup(env, next, data);
}


static void lod_quota_cleanup(const struct lu_env *env, struct dt_device *dev)
{
        struct lod_device *d = dt2lod_dev((struct dt_device *) dev);
        struct dt_device  *next = d->lod_child;
        LASSERT(next);
        next->dd_ops->dt_quota.dt_cleanup(env, next);
}

static const struct dt_device_operations lod_dt_ops = {
        .dt_root_get         = lod_root_get,
        .dt_statfs           = lod_statfs,
        .dt_trans_create     = lod_trans_create,
        .dt_trans_start      = lod_trans_start,
        .dt_trans_stop       = lod_trans_stop,
        .dt_conf_get         = lod_conf_get,
        .dt_sync             = lod_sync,
        .dt_ro               = lod_ro,
        .dt_commit_async     = lod_commit_async,
        .dt_init_capa_ctxt   = lod_init_capa_ctxt,
        /*.dt_init_quota_ctxt= lod_init_quota_ctxt,*/
        .dt_label_get        = lod_label_get,
        .dt_label_set        = lod_label_set,
        .dt_quota.dt_setup   = lod_quota_setup,
        .dt_quota.dt_cleanup = lod_quota_cleanup
};

static int lod_connect_to_osd(const struct lu_env *env, struct lod_device *m,
                               const char *nextdev)
{
        struct obd_connect_data *data = NULL;
        struct obd_device       *obd;
        int                      rc;
        ENTRY;

        LASSERT(m->lod_child_exp == NULL);

        OBD_ALLOC_PTR(data);
        if (data == NULL)
                RETURN(-ENOMEM);

        obd = class_name2obd(nextdev);
        if (obd == NULL) {
                CERROR("can't locate next device: %s\n", nextdev);
                GOTO(out, rc = -ENOTCONN);
        }

        /* XXX: which flags we need on MDS? */
#if 0
        data->ocd_connect_flags = OBD_CONNECT_VERSION   | OBD_CONNECT_INDEX   |
                                  OBD_CONNECT_REQPORTAL | OBD_CONNECT_QUOTA64 |
                                  OBD_CONNECT_OSS_CAPA  | OBD_CONNECT_FID     |
                                  OBD_CONNECT_BRW_SIZE  | OBD_CONNECT_CKSUM   |
                                  OBD_CONNECT_CHANGE_QS | OBD_CONNECT_AT      |
                                  OBD_CONNECT_MDS | OBD_CONNECT_SKIP_ORPHAN   |
                                  OBD_CONNECT_SOM;
#ifdef HAVE_LRU_RESIZE_SUPPORT
        data->ocd_connect_flags |= OBD_CONNECT_LRU_RESIZE;
#endif
        data->ocd_group = mdt_to_obd_objgrp(mds->mds_id);
#endif
        data->ocd_version = LUSTRE_VERSION_CODE;

        rc = obd_connect(env, &m->lod_child_exp, obd, &obd->obd_uuid,
                         data, NULL);
        if (rc) {
                CERROR("cannot connect to next dev %s (%d)\n", nextdev, rc);
                GOTO(out, rc);
        }

        m->lod_dt_dev.dd_lu_dev.ld_site =
                m->lod_child_exp->exp_obd->obd_lu_dev->ld_site;
        LASSERT(m->lod_dt_dev.dd_lu_dev.ld_site);
        m->lod_child = lu2dt_dev(m->lod_child_exp->exp_obd->obd_lu_dev);

out:
        OBD_FREE_PTR(data);
        RETURN(rc);
}

static int lod_init0(const struct lu_env *env, struct lod_device *m,
                      struct lu_device_type *ldt, struct lustre_cfg *cfg)
{
        struct proc_dir_entry *lov_proc_dir;
        struct obd_device     *obd;
        int rc;
        ENTRY;

        obd = class_name2obd(lustre_cfg_string(cfg, 0));
        if (obd == NULL) {
                CERROR("Cannot find obd with name %s\n",
                       lustre_cfg_string(cfg, 0));
                RETURN(-ENODEV);
        }

        m->lod_dt_dev.dd_lu_dev.ld_ops = &lod_lu_ops;
        m->lod_dt_dev.dd_ops = &lod_dt_ops;

        rc = lod_connect_to_osd(env, m, lustre_cfg_string(cfg, 3));
        if (rc)
                RETURN(rc);

        /* setup obd to be used with old lov code */
        rc = lod_lov_init(m, cfg);
        if (rc)
                GOTO(out_disconnect, rc);

        /* for compatibility we link old procfs's OSC entries to osp ones */
        lov_proc_dir = lprocfs_srch(proc_lustre_root, "lov");
        if (lov_proc_dir) {
                cfs_proc_dir_entry_t *symlink = NULL;
                char *name, *p;
                OBD_ALLOC(name, strlen(obd->obd_name) + 1);
                if (name) {
                        strcpy(name, obd->obd_name);
                        p = strstr(name, "lod");
                        if (p) {
                                p[2] = 'v';
                                symlink = lprocfs_add_symlink(name,
                                                lov_proc_dir,
                                                "../lod/%s",
                                                obd->obd_name);
                        }
                        OBD_FREE(name, strlen(obd->obd_name) + 1);
                        m->lod_symlink = symlink;
                }
        }

        cfs_mutex_init(&m->lod_mutex);
        cfs_init_rwsem(&m->lod_rw_sem);

        RETURN(0);

out_disconnect:
        obd_disconnect(m->lod_child_exp);
        RETURN(rc);
}

static struct lu_device *lod_device_free(const struct lu_env *env,
                                         struct lu_device *lu)
{
        struct lod_device *m = lu2lod_dev(lu);
        struct lu_device  *next = &m->lod_child->dd_lu_dev;
        ENTRY;

        LASSERT(atomic_read(&lu->ld_ref) == 0);
        dt_device_fini(&m->lod_dt_dev);
        OBD_FREE_PTR(m);
        RETURN(next);
}

static struct lu_device *lod_device_alloc(const struct lu_env *env,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *lcfg)
{
        struct lod_device *m;
        struct lu_device  *l;

        OBD_ALLOC_PTR(m);
        if (m == NULL) {
                l = ERR_PTR(-ENOMEM);
        } else {
                int rc;

                l = lod2lu_dev(m);
                dt_device_init(&m->lod_dt_dev, t);
                rc = lod_init0(env, m, t, lcfg);
                if (rc != 0) {
                        lod_device_free(env, l);
                        l = ERR_PTR(rc);
                }
        }

        return l;
}

static struct lu_device *lod_device_fini(const struct lu_env *env,
                                          struct lu_device *d)
{
        struct lod_device *m = lu2lod_dev(d);
        int                rc;
        ENTRY;

        if (m->lod_symlink)
                lprocfs_remove(&m->lod_symlink);

        lod_lov_fini(m);

        rc = obd_disconnect(m->lod_child_exp);
        if (rc)
                CERROR("error in disconnect from storage: %d\n", rc);

        RETURN(NULL);
}

/*
 * we use exports to track all LOD users
 */
static int lod_obd_connect(const struct lu_env *env, struct obd_export **exp,
                           struct obd_device *obd, struct obd_uuid *cluuid,
                           struct obd_connect_data *data, void *localdata)
{
        struct lod_device    *lod = lu2lod_dev(obd->obd_lu_dev);
        struct lustre_handle  conn;
        int                   rc;
        ENTRY;

        CDEBUG(D_CONFIG, "connect #%d\n", lod->lod_connects);

        rc = class_connect(&conn, obd, cluuid);
        if (rc)
                RETURN(rc);

        *exp = class_conn2export(&conn);

        /* Why should there ever be more than 1 connect? */
        /* XXX: locking ? */
        lod->lod_connects++;
        LASSERT(lod->lod_connects == 1);

        RETURN(0);
}

/*
 * once last export (we don't count self-export) disappeared
 * lod can be released
 */
static int lod_obd_disconnect(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        struct lod_device *lod = lu2lod_dev(obd->obd_lu_dev);
        int                rc, release = 0;
        ENTRY;

        /* Only disconnect the underlying layers on the final disconnect. */
        /* XXX: locking ? */
        lod->lod_connects--;
        if (lod->lod_connects != 0) {
                /* why should there be more than 1 connect? */
                CERROR("disconnect #%d\n", lod->lod_connects);
                goto out;
        }

        /* XXX: the last user of lod has gone, let's release the device */
        release = 1;

out:
        rc = class_disconnect(exp); /* bz 9811 */

        if (rc == 0 && release)
                class_manual_cleanup(obd);
        RETURN(rc);
}

LU_KEY_INIT(lod, struct lod_thread_info);

static void lod_key_fini(const struct lu_context *ctx,
                         struct lu_context_key *key, void *data)
{
        struct lod_thread_info *info = data;
        /* allocated in lod_get_lov_ea 
         * XXX: this is overload, a tread may have such store but used only
         * once. Probably better would be pool of such stores per LOD.
         */
        if (info->lti_ea_store) {
                OBD_FREE(info->lti_ea_store, info->lti_ea_store_size);
                info->lti_ea_store = NULL;
                info->lti_ea_store_size = 0;
        }
        OBD_FREE_PTR(info);
}

/* context key: lod_thread_key */
LU_CONTEXT_KEY_DEFINE(lod, LCT_MD_THREAD);

LU_TYPE_INIT_FINI(lod, &lod_thread_key);

static struct lu_device_type_operations lod_device_type_ops = {
        .ldto_init           = lod_type_init,
        .ldto_fini           = lod_type_fini,

        .ldto_start          = lod_type_start,
        .ldto_stop           = lod_type_stop,

        .ldto_device_alloc   = lod_device_alloc,
        .ldto_device_free    = lod_device_free,

        .ldto_device_fini    = lod_device_fini
};

static struct lu_device_type lod_device_type = {
        .ldt_tags     = LU_DEVICE_DT,
        .ldt_name     = LUSTRE_LOD_NAME,
        .ldt_ops      = &lod_device_type_ops,
        .ldt_ctx_tags = LCT_MD_THREAD,
};

static int lod_obd_health_check(struct obd_device *obd)
{
        struct lod_device *d = lu2lod_dev(obd->obd_lu_dev);
        int                i, rc = 1;
        ENTRY;

        LASSERT(d);
        lod_getref(d);
        if (d->lod_osts_size > 0)
                cfs_foreach_bit(d->lod_ost_bitmap, i) {
                        LASSERT(d->lod_osts[i] && d->lod_osts[i]->ltd_ost);
                        rc = obd_health_check(d->lod_osts[i]->ltd_exp->exp_obd);
                        /* one healthy device is enough */
                        if (rc == 0)
                                break;
                }
        lod_putref(d);
        RETURN(rc);
}

static struct obd_ops lod_obd_device_ops = {
        .o_owner        = THIS_MODULE,
        .o_connect      = lod_obd_connect,
        .o_disconnect   = lod_obd_disconnect,
        .o_health_check = lod_obd_health_check,
        .o_pool_new     = lod_pool_new,
        .o_pool_rem     = lod_pool_remove,
        .o_pool_add     = lod_pool_add,
        .o_pool_del     = lod_pool_del,
};

static int __init lod_mod_init(void)
{
        struct lprocfs_static_vars  lvars;
        cfs_proc_dir_entry_t       *lov_proc_dir;
        int                         rc;

        lprocfs_lod_init_vars(&lvars);

        rc = class_register_type(&lod_obd_device_ops, NULL, lvars.module_vars,
                                 LUSTRE_LOD_NAME, &lod_device_type);

        /* create "lov" entry in procfs for compatibility purposes */
        if (rc == 0) {
                lov_proc_dir = lprocfs_srch(proc_lustre_root, "lov");
                if (lov_proc_dir == NULL) {
                        lov_proc_dir = lprocfs_register("lov", proc_lustre_root,
                                                        NULL, NULL);
                        if (IS_ERR(lov_proc_dir))
                                CERROR("lod: can't create compat entry \"lov\": %d\n",
                                       (int)PTR_ERR(lov_proc_dir));
                }
        }

        return rc;
}

static void __exit lod_mod_exit(void)
{

        lprocfs_try_remove_proc_entry("lov", proc_lustre_root);

        class_unregister_type(LUSTRE_LOD_NAME);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Logical Object Device ("LUSTRE_LOD_NAME")");
MODULE_LICENSE("GPL");

cfs_module(lod, "0.1.0", lod_mod_init, lod_mod_exit);

