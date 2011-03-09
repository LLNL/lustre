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
 * lustre/ofd/ofd_io.c
 *
 * Author: Alex Tomas <alex@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include "ofd_internal.h"

static int filter_preprw_read(const struct lu_env *env,
                              struct filter_device *ofd, struct lu_fid *fid,
                              struct lu_attr *la, int niocount,
                              struct niobuf_remote *nb, int *nr_local,
                              struct niobuf_local *res)
{
        struct filter_object *fo;
        int i, j, rc, tot_bytes = 0;
        LASSERT(env != NULL);

        fo = filter_object_find(env, ofd, fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));
        LASSERT(fo != NULL);

        filter_read_lock(env, fo);
        if (!filter_object_exists(fo))
                GOTO(unlock, rc = -ENOENT);

        /* parse remote buffers to local buffers and prepare the latter */
        for (i = 0, j = 0; i < niocount; i++) {
                rc = dt_bufs_get(env, filter_object_child(fo), nb + i,
                                 res + j, 0, filter_object_capa(env, fo));
                LASSERT(rc > 0);
                LASSERT(rc <= PTLRPC_MAX_BRW_PAGES);
                /* correct index for local buffers to continue with */
                j += rc;
                LASSERT(j <= PTLRPC_MAX_BRW_PAGES);
                tot_bytes += nb[i].len;
        }

        *nr_local = j;
        LASSERT(*nr_local > 0 && *nr_local <= PTLRPC_MAX_BRW_PAGES);
        rc = dt_attr_get(env, filter_object_child(fo), la,
                         filter_object_capa(env, fo));
        if (unlikely(rc))
                GOTO(unlock, rc);
        rc = dt_read_prep(env, filter_object_child(fo), res, *nr_local);
        lprocfs_counter_add(filter_obd(ofd)->obd_stats,
                            LPROC_FILTER_READ_BYTES, tot_bytes);
unlock:
        if (unlikely(rc))
                filter_read_unlock(env, fo);
        filter_object_put(env, fo);

        RETURN(rc);
}

static int filter_preprw_write(const struct lu_env *env, struct obd_export *exp,
                               struct filter_device *ofd, struct lu_fid *fid,
                               struct lu_attr *la, struct obdo *oa,
                               int objcount, struct obd_ioobj *obj,
                               struct niobuf_remote *nb, int *nr_local,
                               struct niobuf_local *res,
                               struct obd_trans_info *oti)
{
        unsigned long used = 0;
        obd_size left;
        struct filter_object *fo;
        int i, j, k, rc = 0, tot_bytes = 0;

        ENTRY;
        LASSERT(env != NULL);
        LASSERT(objcount == 1);

        fo = filter_object_find(env, ofd, fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));
        LASSERT(fo != NULL);

        if (!filter_object_exists(fo)) {
                if (exp->exp_obd->obd_recovering) {
                        struct obdo *noa = oa;

                        if (oa == NULL) {
                                OBDO_ALLOC(noa);
                                if (noa == NULL)
                                        GOTO(out2, rc = -ENOMEM);
                                noa->o_id = obj->ioo_id;
                                noa->o_valid = OBD_MD_FLID;
                        }

                        if (filter_create(exp, noa, NULL, oti) == 0) {
                                filter_object_put(env, fo);
                                fo = filter_object_find(env, ofd, fid);
                        }
                        if (oa == NULL)
                                OBDO_FREE(noa);
                }

                if (IS_ERR(fo) || !filter_object_exists(fo)) {
                        CERROR("%s: BRW to missing obj "LPU64"/"LPU64":rc %d\n",
                               exp->exp_obd->obd_name,
                               obj->ioo_id, obj->ioo_seq,
                               IS_ERR(fo) ? (int)PTR_ERR(fo) : -ENOENT);
                        if (IS_ERR(fo))
                                RETURN(PTR_ERR(fo));
                        GOTO(out2, rc = -ENOENT);
                }
        }

        filter_read_lock(env, fo);
        if (!filter_object_exists(fo)) {
                filter_read_unlock(env, fo);
                GOTO(out2, rc = -ENOENT);
        }
        /* Always sync if syncjournal parameter is set */
        oti->oti_sync_write = ofd->ofd_syncjournal;
        /* parse remote buffers to local buffers and prepare the latter */
        for (i = 0, j = 0; i < obj->ioo_bufcnt; i++) {
                rc = dt_bufs_get(env, filter_object_child(fo),
                                 nb + i, res + j, 1,
                                 filter_object_capa(env, fo));
                LASSERT(rc > 0);
                LASSERT(rc <= PTLRPC_MAX_BRW_PAGES);
                /* correct index for local buffers to continue with */
                for (k = 0; k < rc; k++) {
                        res[j+k].flags = nb[i].flags;
                        if (!(nb[i].flags & OBD_BRW_ASYNC))
                                oti->oti_sync_write = 1;
                }
                j += rc;
                LASSERT(j <= PTLRPC_MAX_BRW_PAGES);
                tot_bytes += nb[i].len;
        }
        *nr_local = j;
        LASSERT(*nr_local > 0 && *nr_local <= PTLRPC_MAX_BRW_PAGES);

        cfs_mutex_down(&ofd->ofd_grant_sem);
        filter_grant_incoming(env, exp, oa);
        left = filter_grant_space_left(env, exp);

        rc = filter_grant_check(env, exp, oa, res, *nr_local, &left, &used);

        /* XXX: how do we calculate used ? */

        /* do not zero out oa->o_valid as it is used in filter_commitrw_write()
         * for setting UID/GID and fid EA in first write time. */
        /* If OBD_FL_SHRINK_GRANT is set, the client just returned us some grant
         * so no sense in allocating it some more. We either return the grant
         * back to the client if we have plenty of space or we don't return
         * anything if we are short. This was decided in filter_grant_incoming*/
        if (oa->o_valid & OBD_MD_FLGRANT &&
            (!(oa->o_valid & OBD_MD_FLFLAGS) ||
             !(oa->o_flags & OBD_FL_SHRINK_GRANT)))
                oa->o_grant = filter_grant(env, exp, oa->o_grant,
                                           oa->o_undirty, left);
        cfs_mutex_up(&ofd->ofd_grant_sem);
        if (rc == 0) {
                lprocfs_counter_add(filter_obd(ofd)->obd_stats,
                                    LPROC_FILTER_WRITE_BYTES, tot_bytes);
                rc = dt_write_prep(env, filter_object_child(fo), res,
                                   *nr_local);
        } else {

        if (unlikely(rc != 0)) {
                dt_bufs_put(env, filter_object_child(fo), res, *nr_local);
                filter_read_unlock(env, fo);
        }

out2:
        filter_object_put(env, fo);
        RETURN(rc);
}

int filter_preprw(int cmd, struct obd_export *exp, struct obdo *oa,
                  int objcount, struct obd_ioobj *obj,
                  struct niobuf_remote *nb, int *nr_local,
                  struct niobuf_local *res, struct obd_trans_info *oti,
                  struct lustre_capa *capa)
{
        struct lu_env *env = oti->oti_env;
        struct filter_device *ofd = filter_exp(exp);
        struct filter_thread_info *info;
        int rc = 0;

        if (OBD_FAIL_CHECK(OBD_FAIL_OST_ENOENT) &&
            ofd->ofd_destroys_in_progress == 0) {
                /* don't fail lookups for orphan recovery, it causes
                 * later LBUGs when objects still exist during precreate */
                CDEBUG(D_INFO, "*** obd_fail_loc=%x ***\n",OBD_FAIL_OST_ENOENT);
                RETURN(-ENOENT);
        }

        rc = lu_env_refill(env);
        LASSERT(rc == 0);

        info = filter_info_init(env, exp);

        LASSERT(objcount == 1);
        LASSERT(obj->ioo_bufcnt > 0);

        fid_ostid_unpack(&info->fti_fid, &oa->o_oi, 0);
        if (cmd == OBD_BRW_WRITE) {
                rc = filter_auth_capa(ofd, &info->fti_fid, oa->o_seq,
                                      capa, CAPA_OPC_OSS_WRITE);
                if (rc == 0) {
                        LASSERT(oa != NULL);
                        la_from_obdo(&info->fti_attr, oa, OBD_MD_FLGETATTR);
                        rc = filter_preprw_write(env, exp, ofd, &info->fti_fid,
                                                 &info->fti_attr, oa, objcount,
                                                 obj, nb, nr_local, res, oti);
                }
        } else if (cmd == OBD_BRW_READ) {
                rc = filter_auth_capa(ofd, &info->fti_fid, oa->o_seq,
                                      capa, CAPA_OPC_OSS_READ);
                if (rc == 0) {
                        if (oa && oa->o_valid & OBD_MD_FLGRANT) {
                                cfs_mutex_down(&ofd->ofd_grant_sem);
                                filter_grant_incoming(env, exp, oa);
                                if (!(oa->o_valid & OBD_MD_FLFLAGS) ||
                                    !(oa->o_flags & OBD_FL_SHRINK_GRANT))
                                        oa->o_grant = 0;
                                cfs_mutex_up(&ofd->ofd_grant_sem);
                        }
                        rc = filter_preprw_read(env, ofd, &info->fti_fid,
                                                &info->fti_attr, obj->ioo_bufcnt,
                                                nb, nr_local, res);
                        obdo_from_la(oa, &info->fti_attr, LA_ATIME);
                }
        } else {
                LBUG();
                rc = -EPROTO;
        }
        RETURN(rc);
}

static int
filter_commitrw_read(const struct lu_env *env, struct filter_device *ofd,
                     struct lu_fid *fid, int objcount, int niocount,
                     struct niobuf_local *res)
{
        struct filter_object *fo;
        ENTRY;

        LASSERT(niocount > 0);

        fo = filter_object_find(env, ofd, fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));
        LASSERT(fo != NULL);
        LASSERT(filter_object_exists(fo));
        dt_bufs_put(env, filter_object_child(fo), res, niocount);

        filter_read_unlock(env, fo);
        filter_object_put(env, fo);

        RETURN(0);
}

static int
filter_commitrw_write(const struct lu_env *env, struct filter_device *ofd,
                      struct lu_fid *fid, struct lu_attr *la,
                      struct filter_fid *ff, int objcount,
                      int niocount, struct niobuf_local *res, 
                      struct obd_trans_info *oti, int old_rc)
{
        struct filter_thread_info *info = filter_info(env);
        struct filter_object *fo;
        struct dt_object     *o;
        struct lu_attr       *ln = &info->fti_attr2;
        struct thandle       *th;
        int                   rc = 0;
        int retries = 0;
        ENTRY;

        LASSERT(objcount == 1);

        fo = filter_object_find(env, ofd, fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));

        LASSERT(fo != NULL);
        LASSERT(filter_object_exists(fo));

        o = filter_object_child(fo);
        LASSERT(o != NULL);

        if (old_rc)
                GOTO(out, rc = old_rc);

        rc = dt_attr_get(env, o, ln, BYPASS_CAPA);
        if (rc)
                GOTO(out, rc);
        LASSERT(ln->la_valid & LA_MODE);

retry:
        th = filter_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(out, rc = PTR_ERR(th));

        th->th_sync |= oti->oti_sync_write;

        info->fti_buf.lb_buf = NULL;

        if (ln->la_mode & S_ISUID || ln->la_mode & S_ISGID) {
                la->la_valid |= LA_MODE;
                la->la_mode = ln->la_mode & ~(S_ISUID | S_ISGID);

                info->fti_buf.lb_len = sizeof(*ff);
                info->fti_buf.lb_buf = ff;

                rc = dt_declare_xattr_set(env, o, &info->fti_buf,
                                          XATTR_NAME_FID, 0, th);
                if (rc)
                        GOTO(out_stop, rc);
        }

        rc = dt_declare_write_commit(env, o, res, niocount, th);
        if (rc)
                GOTO(out_stop, rc);

        if (la->la_valid) {
                rc = dt_declare_attr_set(env, o, la, th);
                if (rc)
                        GOTO(out_stop, rc);
        }

        rc = filter_trans_start(env, ofd, th);
        if (rc)
                GOTO(out_stop, rc);

        rc = dt_write_commit(env, o, res, niocount, th);
        if (rc)
                GOTO(out_stop, rc);

        if (la->la_valid) {
                rc = dt_attr_set(env, o, la, th, filter_object_capa(env, fo));
                if (rc)
                        GOTO(out_stop, rc);
        }

        if (info->fti_buf.lb_buf) {
                rc = dt_xattr_set(env, o, &info->fti_buf, XATTR_NAME_FID, 0,
                                  th, BYPASS_CAPA);
                if (rc)
                        GOTO(out_stop, rc);
        }

        /* get attr to return */
        dt_attr_get(env, o, la, filter_object_capa(env, fo));

out_stop:
        /* Force commit to make the just-deleted blocks
         * reusable. LU-456 */
        if (rc == -ENOSPC)
                th->th_sync = 1;

        filter_trans_stop(env, ofd, fo, th);
        if (rc == -ENOSPC && retries++ < 3) {
                CDEBUG(D_INODE, "retry after force commit, retries:%d\n",
                       retries);
                goto retry;
        }
out:
        filter_grant_commit(info->fti_exp, niocount, res);
        dt_bufs_put(env, o, res, niocount);
        filter_read_unlock(env, fo);
        filter_object_put(env, fo);

        RETURN(rc);
}

void filter_prepare_fidea(struct filter_fid *ff, struct obdo *oa)
{
        if (!(oa->o_valid & OBD_MD_FLGROUP))
                oa->o_seq = 0;
        /* packing fid and converting it to LE for storing into EA.
         * Here ->o_stripe_idx should be filled by LOV and rest of
         * fields - by client. */
        ff->ff_parent.f_seq = cpu_to_le64(oa->o_parent_seq);
        ff->ff_parent.f_oid = cpu_to_le32(oa->o_parent_oid);
        /* XXX: we are ignoring o_parent_ver here, since this should
         *      be the same for all objects in this fileset. */
        ff->ff_parent.f_ver = cpu_to_le32(oa->o_stripe_idx);
        ff->ff_objid = cpu_to_le64(oa->o_id);
        ff->ff_seq = cpu_to_le64(oa->o_seq);
}

int filter_commitrw(int cmd, struct obd_export *exp,
                    struct obdo *oa, int objcount, struct obd_ioobj *obj,
                    struct niobuf_remote *nb, int npages, struct niobuf_local *res,
                    struct obd_trans_info *oti, int old_rc)
{
        struct filter_device      *ofd = filter_exp(exp);
        struct lu_env             *env = oti->oti_env;
        struct filter_thread_info *info;
        struct filter_mod_data    *fmd;
        __u64                      valid;
        int                        rc = 0;

        info = filter_info(env);
        filter_oti2info(info, oti);

        LASSERT(npages > 0);

        fid_ostid_unpack(&info->fti_fid, &oa->o_oi, 0);
        if (cmd == OBD_BRW_WRITE) {
                /* Don't update timestamps if this write is older than a
                 * setattr which modifies the timestamps. b=10150 */

                /* XXX when we start having persistent reservations this needs
                 * to be changed to filter_fmd_get() to create the fmd if it
                 * doesn't already exist so we can store the reservation handle
                 * there. */
                valid = OBD_MD_FLUID | OBD_MD_FLGID;
                fmd = filter_fmd_find(exp, &info->fti_fid);
                if (!fmd || fmd->fmd_mactime_xid < info->fti_xid)
                        valid |= OBD_MD_FLATIME | OBD_MD_FLMTIME |
                                 OBD_MD_FLCTIME | OBD_MD_FLUID | OBD_MD_FLGID;
                filter_fmd_put(exp, fmd);
                la_from_obdo(&info->fti_attr, oa, valid);

                filter_prepare_fidea(&info->fti_mds_fid, oa);

                rc = filter_commitrw_write(env, ofd, &info->fti_fid,
                                           &info->fti_attr, &info->fti_mds_fid,
                                           objcount, npages, res, oti, old_rc);
                if (rc == 0)
                        obdo_from_la(oa, &info->fti_attr,
                                     FILTER_VALID_FLAGS | LA_GID | LA_UID);
                else
                        obdo_from_la(oa, &info->fti_attr, LA_GID | LA_UID);
                if (old_rc == 0) {
#if 0
                        /* update per-buffer error codes */
                        if (rcs != NULL) {
                                memset(rcs, 0, npages * sizeof(__u32));
                                /* XXX: update rcs */
                                /* for (i = 0; i < npages; i++)
                                if (res[i].rc < 0)
                                        rcs[res[i].rindex] = res[i].rc;
                                */
                        }
#endif
                }
        } else if (cmd == OBD_BRW_READ) {
                struct ldlm_namespace *ns = ofd->ofd_namespace;

                /* If oa != NULL then filter_preprw_read updated the inode
                 * atime and we should update the lvb so that other glimpses
                 * will also get the updated value. bug 5972 */
                if (oa && ns && ns->ns_lvbo && ns->ns_lvbo->lvbo_update) {
                         struct ldlm_resource *rs = NULL;

                        ofd_build_resid(&info->fti_fid, &info->fti_resid);
                        rs = ldlm_resource_get(ns, NULL, &info->fti_resid,
                                               LDLM_EXTENT, 0);
                        if (rs != NULL) {
                                ns->ns_lvbo->lvbo_update(rs, NULL, 1);
                                ldlm_resource_putref(rs);
                        }
                }
                rc = filter_commitrw_read(env, ofd, &info->fti_fid, objcount,
                                          npages, res);
                if (old_rc)
                        rc = old_rc;
        } else {
                LBUG();
                rc = -EPROTO;
        }

        filter_info2oti(info, oti);
        RETURN(rc);
}
