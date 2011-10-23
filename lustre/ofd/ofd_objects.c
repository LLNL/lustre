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

int ofd_version_get_check(struct ofd_thread_info *info,
                             struct ofd_object *fo)
{
        dt_obj_version_t curr_version;

        LASSERT(ofd_object_exists(fo));
        LASSERT(info->fti_exp);

        curr_version = dt_version_get(info->fti_env, ofd_object_child(fo));
        if ((__s64)curr_version == -EOPNOTSUPP)
                RETURN(0);
        /* VBR: version is checked always because costs nothing */
        if (info->fti_pre_version != 0 &&
            info->fti_pre_version != curr_version) {
                CDEBUG(D_INODE, "Version mismatch "LPX64" != "LPX64"\n",
                       info->fti_pre_version, curr_version);
                cfs_spin_lock(&info->fti_exp->exp_lock);
                info->fti_exp->exp_vbr_failed = 1;
                cfs_spin_unlock(&info->fti_exp->exp_lock);
                RETURN (-EOVERFLOW);
        }
        info->fti_pre_version = curr_version;
        RETURN(0);
}

struct ofd_object *ofd_object_find(const struct lu_env *env,
                                         struct ofd_device *ofd,
                                         const struct lu_fid *fid)
{
        struct ofd_object *fo;
        struct lu_object *o;
        ENTRY;

        o = lu_object_find(env, &ofd->ofd_dt_dev.dd_lu_dev, fid, NULL);
        if (likely(!IS_ERR(o)))
                fo = ofd_obj(o);
        else
                fo = (struct ofd_object *)o; /* return error */
        RETURN(fo);
}

struct ofd_object *ofd_object_find_or_create(const struct lu_env *env,
                                                   struct ofd_device *ofd,
                                                   const struct lu_fid *fid,
                                                   struct lu_attr *attr)
{
        struct ofd_thread_info *info = ofd_info(env);
        struct lu_object *fo_obj;
        struct dt_object *dto;
        ENTRY;

        info->fti_dof.dof_type = dt_mode_to_dft(S_IFREG);

        dto = dt_find_or_create(env, ofd->ofd_osd, fid, &info->fti_dof, attr);
        if (IS_ERR(dto))
                RETURN((struct ofd_object *)dto);

        fo_obj = lu_object_locate(dto->do_lu.lo_header,
                                  ofd->ofd_dt_dev.dd_lu_dev.ld_type);
        RETURN(ofd_obj(fo_obj));
}

void ofd_object_put(const struct lu_env *env, struct ofd_object *fo)
{
        lu_object_put(env, &fo->ofo_obj.do_lu);
}

int ofd_precreate_object(const struct lu_env *env, struct ofd_device *ofd,
                         obd_id id, obd_seq group)
{
        struct ofd_thread_info *info = ofd_info(env);
        struct ofd_object      *fo;
        struct dt_object       *next;
        struct thandle         *th;
        obd_id                  tmp;
        int                     rc;
        ENTRY;

        /* Don't create objects beyond the valid range for this SEQ */
        if (unlikely(fid_seq_is_mdt0(group) && id >= IDIF_MAX_OID)) {
                CERROR("%s:"POSTID" hit the IDIF_MAX_OID (1<<48)!\n",
                       ofd_name(ofd), id, group);
                RETURN(rc = -ENOSPC);
        } else if (unlikely(!fid_seq_is_mdt0(group) && id >= OBIF_MAX_OID)) {
                CERROR("%s:"POSTID" hit the OBIF_MAX_OID (1<<32)!\n",
                       ofd_name(ofd), id, group);
                RETURN(rc = -ENOSPC);
        }
        info->fti_ostid.oi_id = id;
        info->fti_ostid.oi_seq = group;
        fid_ostid_unpack(&info->fti_fid, &info->fti_ostid, 0);

        fo = ofd_object_find(env, ofd, &info->fti_fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));

        info->fti_attr.la_valid = LA_TYPE | LA_MODE;
        /*
         * We mark object SUID+SGID to flag it for accepting UID+GID from
         * client on first write.  Currently the permission bits on the OST are
         * never used, so this is OK.
         */
        info->fti_attr.la_mode = S_IFREG | S_ISUID | S_ISGID | 0666;
        info->fti_dof.dof_type = dt_mode_to_dft(S_IFREG);

        /**
         * Set a/c/m time to a insane large negative value at creation
         * time so that any timestamp arriving from the client will
         * always be newer and update the inode.
         * See LU-221 for details.
         */
        info->fti_attr.la_valid |= LA_ATIME | LA_MTIME | LA_CTIME;
        info->fti_attr.la_atime = INT_MIN + 24 * 3600;
        info->fti_attr.la_mtime = INT_MIN + 24 * 3600;
        info->fti_attr.la_ctime = INT_MIN + 24 * 3600;

        next = ofd_object_child(fo);
        LASSERT(next != NULL);

        info->fti_buf.lb_buf = &tmp;
        info->fti_buf.lb_len = sizeof(tmp);
        info->fti_off = 0;

        ofd_write_lock(env, fo);
        th = ofd_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(out_unlock, rc = PTR_ERR(th));

        rc = dt_declare_record_write(env, ofd->ofd_lastid_obj[group],
                                     sizeof(tmp), info->fti_off, th);
        if (rc)
                GOTO(trans_stop, rc);

        if (unlikely(ofd_object_exists(fo))) {
                /* object may exist being re-created by write replay */
                CDEBUG(D_INODE, "object %u/"LPD64" exists: "DFID"\n",
                       (unsigned) group, id, PFID(&info->fti_fid));
                rc = dt_trans_start_local(env, ofd->ofd_osd, th);
                if (rc)
                        GOTO(trans_stop, rc);
                GOTO(last_id_write, rc);
        }
        rc = dt_declare_create(env, next, &info->fti_attr, NULL,
                               &info->fti_dof, th);
        if (rc)
                GOTO(trans_stop, rc);

        rc = dt_trans_start_local(env, ofd->ofd_osd, th);
        if (rc)
                GOTO(trans_stop, rc);

        CDEBUG(D_OTHER, "create new object %lu:%llu\n",
               (unsigned long) info->fti_fid.f_oid, info->fti_fid.f_seq);

        rc = dt_create(env, next, &info->fti_attr, NULL, &info->fti_dof, th);
        if (rc)
                GOTO(trans_stop, rc);
        LASSERT(ofd_object_exists(fo));

last_id_write:
        ofd_last_id_set(ofd, id, group);

        tmp = cpu_to_le64(ofd_last_id(ofd, group));
        rc = dt_record_write(env, ofd->ofd_lastid_obj[group], &info->fti_buf,
                             &info->fti_off, th);
trans_stop:
        ofd_trans_stop(env, ofd, th, rc);
out_unlock:
        ofd_write_unlock(env, fo);
        ofd_object_put(env, fo);
        RETURN(rc);
}

int ofd_attr_set(const struct lu_env *env, struct ofd_object *fo,
                    const struct lu_attr *la)
{
        struct thandle *th;
        struct ofd_device *ofd = ofd_obj2dev(fo);
        struct ofd_thread_info *info = ofd_info(env);
        struct ofd_mod_data *fmd;
        int rc;
        ENTRY;

        ofd_write_lock(env, fo);
        if (!ofd_object_exists(fo))
                GOTO(unlock, rc = -ENOENT);

        if (la->la_valid & (LA_ATIME | LA_MTIME | LA_CTIME)) {
                fmd = ofd_fmd_get(info->fti_exp, &fo->ofo_header.loh_fid);
                if (fmd && fmd->fmd_mactime_xid < info->fti_xid)
                        fmd->fmd_mactime_xid = info->fti_xid;
                ofd_fmd_put(info->fti_exp, fmd);
        }

        /* VBR: version recovery check */
        rc = ofd_version_get_check(info, fo);
        if (rc)
                GOTO(unlock, rc);

        th = ofd_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(unlock, rc = PTR_ERR(th));

        rc = dt_declare_attr_set(env, ofd_object_child(fo), la, th);
        if (rc)
                GOTO(stop, rc);

        rc = ofd_trans_start(env, ofd, la->la_valid & LA_SIZE ? fo : NULL, th);
        if (rc)
                GOTO(stop, rc);

        rc = dt_attr_set(env, ofd_object_child(fo), la, th,
                        ofd_object_capa(env, fo));
stop:
        ofd_trans_stop(env, ofd, th, rc);
unlock:
        ofd_write_unlock(env, fo);
        RETURN(rc);
}

int ofd_object_punch(const struct lu_env *env, struct ofd_object *fo,
                        __u64 start, __u64 end, const struct lu_attr *la)
{
        struct ofd_thread_info *info = ofd_info(env);
        struct ofd_device      *ofd = ofd_obj2dev(fo);
        struct ofd_mod_data    *fmd;
        struct dt_object          *dob = ofd_object_child(fo);
        struct thandle            *th;
        int rc;
        ENTRY;

        /* we support truncate, not punch yet */
        LASSERT(end == OBD_OBJECT_EOF);

        fmd = ofd_fmd_get(info->fti_exp, &fo->ofo_header.loh_fid);
        if (fmd && fmd->fmd_mactime_xid < info->fti_xid)
                fmd->fmd_mactime_xid = info->fti_xid;
        ofd_fmd_put(info->fti_exp, fmd);

        ofd_write_lock(env, fo);
        if (!ofd_object_exists(fo))
                GOTO(unlock, rc = -ENOENT);

        /* VBR: version recovery check */
        rc = ofd_version_get_check(info, fo);
        if (rc)
                GOTO(unlock, rc);

        th = ofd_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(unlock, rc = PTR_ERR(th));

        rc = dt_declare_attr_set(env, dob, la, th);
        if (rc)
                GOTO(stop, rc);

        rc = dt_declare_punch(env, dob, start, OBD_OBJECT_EOF, th);
        if (rc)
                GOTO(stop, rc);

        rc = ofd_trans_start(env, ofd, fo, th);
        if (rc)
                GOTO(stop, rc);

        rc = dt_punch(env, dob, start, OBD_OBJECT_EOF, th,
                      ofd_object_capa(env, fo));
        if (rc)
                GOTO(stop, rc);

        rc = dt_attr_set(env, dob, la, th, ofd_object_capa(env, fo));

stop:
        ofd_trans_stop(env, ofd, th, rc);
unlock:
        ofd_write_unlock(env, fo);
        RETURN(rc);
}

int ofd_object_destroy(const struct lu_env *env, struct ofd_object *fo,
                          int orphan)
{
        struct ofd_device *ofd = ofd_obj2dev(fo);
        struct thandle *th;
        int rc = 0;
        ENTRY;

        ofd_write_lock(env, fo);
        if (!ofd_object_exists(fo))
                GOTO(unlock, rc = -ENOENT);

        th = ofd_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(unlock, rc = PTR_ERR(th));

        dt_declare_ref_del(env, ofd_object_child(fo), th);
        dt_declare_destroy(env, ofd_object_child(fo), th);
        if (orphan)
                rc = dt_trans_start_local(env, ofd->ofd_osd, th);
        else
                rc = ofd_trans_start(env, ofd, NULL, th);
        if (rc)
                GOTO(stop, rc);

        ofd_fmd_drop(ofd_info(env)->fti_exp, &fo->ofo_header.loh_fid);

        dt_ref_del(env, ofd_object_child(fo), th);
        dt_destroy(env, ofd_object_child(fo), th);
stop:
        ofd_trans_stop(env, ofd, th, rc);
unlock:
        ofd_write_unlock(env, fo);
        RETURN(rc);
}

int ofd_attr_get(const struct lu_env *env, struct ofd_object *fo,
                    struct lu_attr *la)
{
        int rc = 0;
        ENTRY;

        if (ofd_object_exists(fo)) {
                rc = dt_attr_get(env, ofd_object_child(fo), la,
                                 ofd_object_capa(env, fo));
        } else {
                rc = -ENOENT;
        }
        RETURN(rc);
}
