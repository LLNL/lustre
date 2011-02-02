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
 * lustre/ofd/ofd_objects.c
 *
 * Author: Alex Tomas <alex@clusterfs.com>
 * Author: Mike Pershin <tappro@sun.com>
 */


#define DEBUG_SUBSYSTEM S_FILTER

#include <libcfs/libcfs.h>
#include "ofd_internal.h"

struct filter_object *filter_object_find(const struct lu_env *env,
                                         struct filter_device *ofd,
                                         const struct lu_fid *fid)
{
        struct filter_object *fo;
        struct lu_object *o;
        ENTRY;

        o = lu_object_find(env, &ofd->ofd_dt_dev.dd_lu_dev, fid, NULL);
        if (likely(!IS_ERR(o)))
                fo = filter_obj(o);
        else
                fo = (struct filter_object *)o; /* return error */
        RETURN(fo);
}

struct filter_object *filter_object_find_or_create(const struct lu_env *env,
                                                   struct filter_device *ofd,
                                                   const struct lu_fid *fid,
                                                   struct lu_attr *attr)
{
        struct filter_thread_info *info = filter_info(env);
        struct lu_object *fo_obj;
        struct dt_object *dto;
        ENTRY;

        info->fti_dof.dof_type = dt_mode_to_dft(S_IFREG);

        dto = dt_find_or_create(env, ofd->ofd_osd, fid, &info->fti_dof, attr);
        if (IS_ERR(dto))
                RETURN((struct filter_object *)dto);

        fo_obj = lu_object_locate(dto->do_lu.lo_header,
                                  ofd->ofd_dt_dev.dd_lu_dev.ld_type);
        RETURN(filter_obj(fo_obj));
}

void filter_object_put(const struct lu_env *env, struct filter_object *fo)
{
        lu_object_put(env, &fo->ofo_obj.do_lu);
}

int filter_precreate_object(const struct lu_env *env, struct filter_device *ofd,
                            obd_id id, obd_seq group)
{
        struct filter_thread_info *info = filter_info(env);
        struct filter_object    *fo;
        struct dt_object        *next;
        struct thandle          *th;
        obd_id                   tmp;
        int                      rc;

        /* Don't create objects beyond the valid range for this SEQ */
        if (unlikely(fid_seq_is_mdt0(group) && id >= IDIF_MAX_OID)) {
                CERROR("%s:"POSTID" hit the IDIF_MAX_OID (1<<48)!\n",
                       filter_name(ofd), id, group);
                RETURN(rc = -ENOSPC);
        } else if (unlikely(!fid_seq_is_mdt0(group) && id >= OBIF_MAX_OID)) {
                CERROR("%s:"POSTID" hit the OBIF_MAX_OID (1<<32)!\n",
                       filter_name(ofd), id, group);
                RETURN(rc = -ENOSPC);
        }
        info->fti_ostid.oi_id = id;
        info->fti_ostid.oi_seq = group;
        fid_ostid_unpack(&info->fti_fid, &info->fti_ostid, 0);

        fo = filter_object_find(env, ofd, &info->fti_fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));

        info->fti_attr.la_valid = LA_TYPE | LA_MODE;
        info->fti_attr.la_mode = S_IFREG | S_ISUID | S_ISGID | 0666;
        info->fti_dof.dof_type = dt_mode_to_dft(S_IFREG);

        /**
         * Set a/c/m time to a insane large negative value at creation
         * time so that any timestamp arriving from the client will
         * always be newer and update the inode.
         * See LU-221 for details.
         */
        attr.la_valid |= LA_ATIME | LA_MTIME | LA_CTIME;
        attr.la_atime = INT_MIN + 24 * 3600;
        attr.la_mtime = INT_MIN + 24 * 3600;
        attr.la_ctime = INT_MIN + 24 * 3600;

        next = filter_object_child(fo);
        LASSERT(next != NULL);

        info->fti_buf.lb_buf = &tmp;
        info->fti_buf.lb_len = sizeof(tmp);
        info->fti_off = group * sizeof(tmp);

        filter_write_lock(env, fo);
        if (filter_object_exists(fo)) {
                /* underlying filesystem is broken - object must not exist */
                CERROR("object %u/"LPD64" exists: "DFID"\n",
                       (unsigned) group, id, PFID(&info->fti_fid));
                GOTO(out_unlock, rc = -EEXIST);
        }

        th = filter_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(out_unlock, rc = PTR_ERR(th));

        rc = dt_declare_create(env, next, &info->fti_attr, NULL,
                               &info->fti_dof, th);
        if (rc)
                GOTO(trans_stop, rc);
        rc = dt_declare_attr_set(env, next, &info->fti_attr, th);
        if (rc)
                GOTO(trans_stop, rc);

        rc = dt_declare_record_write(env, ofd->ofd_lastid_obj[group],
                                     sizeof(tmp), info->fti_off, th);
        if (rc)
                GOTO(trans_stop, rc);

        rc = filter_trans_start(env, ofd, th);
        if (rc)
                GOTO(trans_stop, rc);

        CDEBUG(D_OTHER, "create new object %lu:%llu\n",
               (unsigned long) info->fti_fid.f_oid, info->fti_fid.f_seq);

        rc = dt_create(env, next, &info->fti_attr, NULL, &info->fti_dof, th);
        if (rc)
                GOTO(trans_stop, rc);
        LASSERT(filter_object_exists(fo));

        info->fti_attr.la_valid &= ~LA_TYPE;
        rc = dt_attr_set(env, next, &info->fti_attr, th, BYPASS_CAPA);
        if (rc)
                GOTO(trans_stop, rc);

        filter_last_id_set(ofd, id, group);

        rc = filter_last_id_write(env, ofd, group, th);

trans_stop:
        filter_trans_stop(env, ofd, th);
out_unlock:
        filter_write_unlock(env, fo);
        filter_object_put(env, fo);
        RETURN(rc);
}

int filter_attr_set(const struct lu_env *env, struct filter_object *fo,
                    const struct lu_attr *la)
{
        struct thandle *th;
        struct filter_device *ofd = filter_obj2dev(fo);
        struct filter_thread_info *info = filter_info(env);
        struct filter_mod_data *fmd;
        int rc;
        ENTRY;

        if (la->la_valid & (LA_ATIME | LA_MTIME | LA_CTIME)) {
                fmd = filter_fmd_get(info->fti_exp, &fo->ofo_header.loh_fid);
                if (fmd && fmd->fmd_mactime_xid < info->fti_xid)
                        fmd->fmd_mactime_xid = info->fti_xid;
                filter_fmd_put(info->fti_exp, fmd);
        }

        filter_write_lock(env, fo);
        if (!filter_object_exists(fo))
                GOTO(unlock, rc = -ENOENT);

        th = filter_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(unlock, rc = PTR_ERR(th));

        rc = dt_declare_attr_set(env, filter_object_child(fo), la, th);
        if (rc)
                GOTO(stop, rc);

        rc = filter_trans_start(env, ofd, th);
        if (rc)
                GOTO(stop, rc);

        rc = dt_attr_set(env, filter_object_child(fo), la, th,
                        filter_object_capa(env, fo));
stop:
        filter_trans_stop(env, ofd, th);
unlock:
        filter_write_unlock(env, fo);
        RETURN(rc);
}

int filter_object_punch(const struct lu_env *env, struct filter_object *fo,
                        __u64 start, __u64 end, const struct lu_attr *la)
{
        struct filter_thread_info *info = filter_info(env);
        struct filter_device      *ofd = filter_obj2dev(fo);
        struct filter_mod_data    *fmd;
        struct dt_object          *dob = filter_object_child(fo);
        struct thandle            *th;
        int rc;
        ENTRY;

        /* we support truncate, not punch yet */
        LASSERT(end == OBD_OBJECT_EOF);

        fmd = filter_fmd_get(info->fti_exp, &fo->ofo_header.loh_fid);
        if (fmd && fmd->fmd_mactime_xid < info->fti_xid)
                fmd->fmd_mactime_xid = info->fti_xid;
        filter_fmd_put(info->fti_exp, fmd);

        filter_write_lock(env, fo);
        if (!filter_object_exists(fo))
                GOTO(unlock, rc = -ENOENT);

        th = filter_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(unlock, rc = PTR_ERR(th));

        rc = dt_declare_attr_set(env, dob, la, th);
        if (rc)
                GOTO(stop, rc);

        rc = dt_declare_punch(env, dob, start, OBD_OBJECT_EOF, th);
        if (rc)
                GOTO(stop, rc);

        rc = filter_trans_start(env, ofd, th);
        if (rc)
                GOTO(stop, rc);

        rc = dt_punch(env, dob, start, OBD_OBJECT_EOF, th,
                      filter_object_capa(env, fo));
        if (rc)
                GOTO(stop, rc);

        rc = dt_attr_set(env, dob, la, th, filter_object_capa(env, fo));

stop:
        filter_trans_stop(env, ofd, th);
unlock:
        filter_write_unlock(env, fo);
        RETURN(rc);

}

int filter_object_destroy(const struct lu_env *env, struct filter_object *fo)
{
        struct thandle *th;
        int rc = 0;
        ENTRY;

        filter_write_lock(env, fo);
        if (!filter_object_exists(fo))
                GOTO(unlock, rc = -ENOENT);

        th = filter_trans_create(env, filter_obj2dev(fo));
        if (IS_ERR(th))
                GOTO(unlock, rc = PTR_ERR(th));

        dt_declare_ref_del(env, filter_object_child(fo), th);
        dt_declare_destroy(env, filter_object_child(fo), th);
        rc = filter_trans_start(env, filter_obj2dev(fo), th);
        if (rc)
                GOTO(stop, rc);

        filter_fmd_drop(filter_info(env)->fti_exp, &fo->ofo_header.loh_fid);

        dt_ref_del(env, filter_object_child(fo), th);
        dt_destroy(env, filter_object_child(fo), th);
stop:
        filter_trans_stop(env, filter_obj2dev(fo), th);
unlock:
        filter_write_unlock(env, fo);
        RETURN(rc);
}

int filter_attr_get(const struct lu_env *env, struct filter_object *fo,
                    struct lu_attr *la)
{
        int rc = 0;
        ENTRY;

        if (filter_object_exists(fo)) {
                rc = dt_attr_get(env, filter_object_child(fo), la,
                                 filter_object_capa(env, fo));
        } else {
                rc = -ENOENT;
        }
        RETURN(rc);
}
