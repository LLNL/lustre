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
 * lustre/osp/osp_object.c
 *
 * Lustre OST Proxy Device
 *
 * Author: Alex Zhuravlev <bzzz@sun.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lprocfs_status.h>

#include <lustre_disk.h>
#include <lustre_fid.h>
#include <lustre_mds.h>
#include <lustre/lustre_idl.h>
#include <lustre_param.h>
#include <lustre_fid.h>

#include "osp_internal.h"

static __u64 osp_object_assign_id(const struct lu_env *env,
                                  struct osp_device *d, struct osp_object *o)
{
        const struct lu_fid *f;
        struct lu_fid        fid;
        struct ost_id        ostid;
        __u64                objid;

        f = lu_object_fid(&o->opo_obj.do_lu);
        LASSERT(f->f_oid == 0);
        LASSERT(f->f_seq == 0);
        LASSERT(f->f_ver == 0);

        LASSERT(o->opo_reserved);
        o->opo_reserved = 0;

        objid = osp_precreate_get_id(d);

        /* assign fid to anonymous object */
        ostid.oi_id = objid;
        ostid.oi_seq = FID_SEQ_OST_MDT0; /* XXX: mds number to support CMD? */
        fid_ostid_unpack(&fid, &ostid, d->opd_index);
        lu_object_assign_fid(env, &o->opo_obj.do_lu, &fid);

        return objid;
}


static int osp_declare_attr_set(const struct lu_env *env,
                                 struct dt_object *dt,
                                 const struct lu_attr *attr,
                                 struct thandle *th)
{
        struct osp_device  *d = lu2osp_dev(dt->do_lu.lo_dev);
        struct osp_object  *o = dt2osp_obj(dt);
        int                 rc = 0;
        ENTRY;

        /*
         * Usually we don't allow server stack to manipulate size
         * but there is a special case when striping is created
         * late, after stripless file got truncated to non-zero.
         *
         * In this case we do the following:
         *
         * 1) grab id in declare - this can lead to leaked OST objects
         *    but we don't currently have proper mechanism and the only
         *    options we have are to do truncate RPC holding transaction
         *    open (very bad) or to grab id in declare at cost of leaked
         *    OST object in same very rare unfortunate case (just bad)
         *    notice 1.6-2.0 do assignment outside of running transaction
         *    all the time, meaning many more chances for leaked objects.
         * 
         * 2) send synchronous truncate RPC with just assigned id
         */

        if (attr && (attr->la_valid & LA_SIZE) && attr->la_size) {
                LASSERT(!dt_object_exists(dt));
                osp_object_assign_id(env, d, o);
                rc = osp_object_truncate(env, dt, attr->la_size);
                if (rc)
                        RETURN(rc);
        }

        if (attr && !(attr->la_valid & (LA_UID | LA_GID)))
                RETURN(0);

        /*
         * track all UID/GID changes via llog
         */
        rc = osp_sync_declare_add(env, o, MDS_SETATTR64_REC, th);

        RETURN(rc);
}

static int osp_attr_set(const struct lu_env *env,
                         struct dt_object *dt,
                         const struct lu_attr *attr,
                         struct thandle *th,
                         struct lustre_capa *capa)
{
        struct osp_object  *o = dt2osp_obj(dt);
        int                 rc = 0;
        ENTRY;

        /* 
         * XXX: first ->attr_set() can be ignored, but this is a trick
         * better would be to make ->do_create() to initialize attributes
         * and not do the very first ->attr_set() at all
         */
        if (o->opo_no_attrs) {
                o->opo_no_attrs = 0;
                RETURN(0);
        }

        /* we're interested in uid/gid changes only */
        if (!(attr->la_valid & (LA_UID | LA_GID)))
                RETURN(0);

        /*
         * once transaction is committed put proper command on
         * the queue going to our OST
         */
        rc = osp_sync_add(env, o, MDS_SETATTR64_REC, th);

        /* XXX: we need a change in OSD API to track committness */

        /* XXX: send new uid/gid to OST ASAP? */

        RETURN(rc);
}

static int osp_declare_object_create(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct lu_attr *attr,
                                      struct dt_allocation_hint *hint,
                                      struct dt_object_format *dof,
                                      struct thandle *th)
{
        struct osp_device   *d = lu2osp_dev(dt->do_lu.lo_dev);
        struct osp_object   *o = dt2osp_obj(dt);
        const struct lu_fid *fid;
        int                  rc = 0;
        ENTRY;

        LASSERT(d->opd_last_used_file);
        fid = lu_object_fid(&dt->do_lu);

        if (unlikely((fid_oid(fid) || fid_seq(fid)))) {
                /* replace case: caller knows fid */
                /* XXX: for compatibility use common for all OSPs file */
                rc = dt_declare_record_write(env, d->opd_last_used_file, 8, 0, th);
                RETURN(rc);
        }

        /*
         * in declaration we need to reserve object so that we don't block
         * awaiting precreation RPC to complete
         */
        rc = osp_precreate_reserve(d);

         /*
         * we also need to declare update to local "last used id" file for recovery
         * if object isn't used for a reason, we need to release reservation,
         * this can be made in osd_object_release()
         */
        if (rc == 0) {
                /* mark id is reserved: in create we don't want to talk to OST */
                LASSERT(o->opo_reserved == 0);
                o->opo_reserved = 1;

                /* XXX: for compatibility use common for all OSPs file */
                rc = dt_declare_record_write(env, d->opd_last_used_file, 8, 0, th);
        } else {
                /* not needed in the cache anymore */
                set_bit(LU_OBJECT_HEARD_BANSHEE, &dt->do_lu.lo_header->loh_flags);
        }

        RETURN(rc);
}

static int osp_object_create(const struct lu_env *env,
                              struct dt_object *dt,
                              struct lu_attr *attr,
                              struct dt_allocation_hint *hint,
                              struct dt_object_format *dof,
                              struct thandle *th)
{
        struct osp_device   *d = lu2osp_dev(dt->do_lu.lo_dev);
        struct osp_object   *o = dt2osp_obj(dt);
        struct lu_buf        lb;
        obd_id               objid;
        loff_t               offset;
        int                  rc = 0;
        int                  update = 0;
        ENTRY;

        /* XXX: to support CMD we need group here, to be put into config? */

        if (o->opo_reserved) {
                /* regular case, id is assigned holding trunsaction open */
                objid = osp_object_assign_id(env, d, o);
        } else {
                /* special case, id was assigned outside of transaction
                 * see comments in osp_declare_attr_set */
                struct ost_id   ostid = { 0 };
                rc = fid_ostid_pack(lu_object_fid(&dt->do_lu), &ostid);
                LASSERT(rc == 0);
                objid = ostid_id(&ostid);
        }

        LASSERT(objid);

        /*
         * update last_used object id for our OST
         * XXX: can we use 0-copy OSD methods to save memcpy()
         * which is going to be each creation * <# stripes>
         * XXX: needs volatile
         */
        if (objid > d->opd_last_used_id) {
                cfs_spin_lock(&d->opd_pre_lock);
                if (objid > d->opd_last_used_id) {
                        d->opd_last_used_id = objid;
                        update = 1;
                }
                cfs_spin_unlock(&d->opd_pre_lock);
        }

        /*
         * it's OK if the import is inactive by this moment - id was created
         * by OST earlier, we just need to maintain it consistently on the disk
         * once import is reconnected, OSP will claim this and other objects
         * used and OST either keep them, if they exist or recreate
         */
        if (update) {
                /* we updated last_used in-core, so we update on a disk */
                objid = cpu_to_le64(objid);
                lb.lb_buf = &objid;
                lb.lb_len = sizeof(objid);

                /* XXXXXXXXXXXXXXXXX: don't use local var, otherwise racy */
                /* andreas asked more and more */
                offset = d->opd_index * sizeof(objid);
                rc = dt_record_write(env, d->opd_last_used_file, &lb,
                                     &offset, th);
        }

        /* object is created, we can ignore first attr_set from being logged */
        o->opo_no_attrs = 1;

        RETURN(rc);
}

static int osp_declare_object_destroy(const struct lu_env *env,
                                      struct dt_object *dt,
                                      struct thandle *th)
{
        struct osp_object  *o = dt2osp_obj(dt);
        int                 rc = 0;
        ENTRY;

        /*
         * track objects to be destroyed via llog
         */
        rc = osp_sync_declare_add(env, o, MDS_UNLINK_REC, th);

        RETURN(rc);
}

static int osp_object_destroy(const struct lu_env *env,
                         struct dt_object *dt,
                         struct thandle *th)
{
        struct osp_object  *o = dt2osp_obj(dt);
        int                 rc = 0;
        ENTRY;

        /*
         * once transaction is committed put proper command on
         * the queue going to our OST
         */
        rc = osp_sync_add(env, o, MDS_UNLINK_REC, th);

        /* XXX: we need a change in OSD API to track committness */


        /* not needed in cache any more */
        set_bit(LU_OBJECT_HEARD_BANSHEE, &dt->do_lu.lo_header->loh_flags);

        RETURN(rc);
}

struct dt_object_operations osp_obj_ops = {
        .do_declare_attr_set  = osp_declare_attr_set,
        .do_attr_set          = osp_attr_set,
        .do_declare_create    = osp_declare_object_create,
        .do_create            = osp_object_create,
        .do_declare_destroy   = osp_declare_object_destroy,
        .do_destroy           = osp_object_destroy,
};

static int osp_object_init(const struct lu_env *env, struct lu_object *o,
                            const struct lu_object_conf *unused)
{
        struct osp_object *po = lu2osp_obj(o);

        po->opo_obj.do_ops = &osp_obj_ops;

        return 0;
}

static void osp_object_free(const struct lu_env *env, struct lu_object *o)
{
        struct osp_object *obj = lu2osp_obj(o);
        struct lu_object_header *h = o->lo_header;

        dt_object_fini(&obj->opo_obj);
        lu_object_header_fini(h);
        OBD_FREE_PTR(obj);
}

static void osp_object_release(const struct lu_env *env, struct lu_object *o)
{
        struct osp_object *po = lu2osp_obj(o);
        struct osp_device *d  = lu2osp_dev(o->lo_dev);
        ENTRY;

        /*
         * release reservation if object was declared but not created
         * this may require lu_object_put() in LOD
         */
        if (unlikely(po->opo_reserved)) {
                LASSERT(d->opd_pre_reserved > 0);
                cfs_spin_lock(&d->opd_pre_lock);
                d->opd_pre_reserved--;
                cfs_spin_unlock(&d->opd_pre_lock);

                /* not needed in cache any more */
                set_bit(LU_OBJECT_HEARD_BANSHEE, &o->lo_header->loh_flags);
        }

#if 0
        /*
         * XXX: this is a small dirty hack to deal with objects
         * allocated with lu_object_anon() and not put into lu_site
         * we want to release such objects with lu_object_put():
         * we manipulate site's internals to keep is consistent
         * ls_guard is already taken by lu_object_put()
         */
        CDEBUG(D_OTHER, "tweak "DFID"\n", PFID(lu_object_fid(o)));
        if (fid_seq(lu_object_fid(o)) == 0
                        && fid_oid(lu_object_fid(o)) == 0
                        && fid_ver(lu_object_fid(o)) == 0) {
                struct lu_site *s = o->lo_dev->ld_site;
                LASSERT(s);
                CFS_INIT_HLIST_NODE(&o->lo_header->loh_hash);
                CFS_INIT_LIST_HEAD(&o->lo_header->loh_lru);
                s->ls_busy++;
                s->ls_total++;
        }
#endif
}

static int osp_object_print(const struct lu_env *env, void *cookie,
                             lu_printer_t p, const struct lu_object *l)
{
        const struct osp_object *o = lu2osp_obj((struct lu_object *) l);

        return (*p)(env, cookie, LUSTRE_OSP_NAME"-object@%p", o);
}

static int osp_object_invariant(const struct lu_object *o)
{
        LBUG();
}


struct lu_object_operations osp_lu_obj_ops = {
        .loo_object_init      = osp_object_init,
        .loo_object_free      = osp_object_free,
        .loo_object_release   = osp_object_release,
        .loo_object_print     = osp_object_print,
        .loo_object_invariant = osp_object_invariant
};

