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
 * Copyright (c) 2011 Whamcloud, Inc.
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

static int ofd_preprw_read(const struct lu_env *env, struct ofd_device *ofd,
                           struct lu_fid *fid, struct lu_attr *la, int niocount,
                           struct niobuf_remote *rnb, int *nr_local,
                           struct niobuf_local *lnb)
{
        struct ofd_object *fo;
        int i, j, rc, tot_bytes = 0;
        LASSERT(env != NULL);

        fo = ofd_object_find(env, ofd, fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));
        LASSERT(fo != NULL);

        ofd_read_lock(env, fo);
        if (!ofd_object_exists(fo))
                GOTO(unlock, rc = -ENOENT);

        /* parse remote buffers to local buffers and prepare the latter */
        for (i = 0, j = 0; i < niocount; i++) {
                rc = dt_bufs_get(env, ofd_object_child(fo), rnb + i,
                                 lnb + j, 0, ofd_object_capa(env, fo));
                LASSERT(rc > 0);
                LASSERT(rc <= PTLRPC_MAX_BRW_PAGES);
                /* correct index for local buffers to continue with */
                j += rc;
                LASSERT(j <= PTLRPC_MAX_BRW_PAGES);
                tot_bytes += rnb[i].rnb_len;
        }

        *nr_local = j;
        LASSERT(*nr_local > 0 && *nr_local <= PTLRPC_MAX_BRW_PAGES);
        rc = dt_attr_get(env, ofd_object_child(fo), la,
                         ofd_object_capa(env, fo));
        if (unlikely(rc))
                GOTO(unlock, rc);
        rc = dt_read_prep(env, ofd_object_child(fo), lnb, *nr_local);
        lprocfs_counter_add(ofd_obd(ofd)->obd_stats,
                            LPROC_OFD_READ_BYTES, tot_bytes);
unlock:
        if (unlikely(rc))
                ofd_read_unlock(env, fo);
        ofd_object_put(env, fo);

        RETURN(rc);
}

static int ofd_preprw_write(const struct lu_env *env, struct obd_export *exp,
                            struct ofd_device *ofd, struct lu_fid *fid,
                            struct lu_attr *la, struct obdo *oa,
                            int objcount, struct obd_ioobj *obj,
                            struct niobuf_remote *rnb, int *nr_local,
                            struct niobuf_local *lnb,
                            struct obd_trans_info *oti)
{
        struct ofd_object *fo;
        int i, j, k, rc = 0, tot_bytes = 0;
        ENTRY;
        LASSERT(env != NULL);
        LASSERT(objcount == 1);

        if (unlikely(exp->exp_obd->obd_recovering)) {
                struct ofd_thread_info *info = ofd_info(env);

                /* taken from ofd_precreate_object */
                memset(&info->fti_attr, 0, sizeof(info->fti_attr));
                info->fti_attr.la_valid = LA_TYPE | LA_MODE;
                info->fti_attr.la_mode = S_IFREG | S_ISUID | S_ISGID | 0666;
                info->fti_attr.la_valid |= LA_ATIME | LA_MTIME | LA_CTIME;
                info->fti_attr.la_atime = INT_MIN + 24 * 3600;
                info->fti_attr.la_mtime = INT_MIN + 24 * 3600;
                info->fti_attr.la_ctime = INT_MIN + 24 * 3600;

                fo = ofd_object_find_or_create(env, ofd, fid, &info->fti_attr);
        } else {
                fo = ofd_object_find(env, ofd, fid);
        }

        if (IS_ERR(fo))
                GOTO(out, rc = PTR_ERR(fo));
        LASSERT(fo != NULL);

        ofd_read_lock(env, fo);
        if (!ofd_object_exists(fo)) {
                CERROR("%s: BRW to missing obj "LPU64"/"LPU64"\n",
                       exp->exp_obd->obd_name, obj->ioo_id, obj->ioo_seq);
                ofd_read_unlock(env, fo);
                ofd_object_put(env, fo);
                GOTO(out, rc = -ENOENT);
        }

        /* Always sync if syncjournal parameter is set */
        oti->oti_sync_write = ofd->ofd_syncjournal;

        /* Process incoming grant info, set OBD_BRW_GRANTED flag and grant some
         * space back if possible */
        ofd_grant_prepare_write(env, exp, oa, rnb, obj->ioo_bufcnt);

        /* parse remote buffers to local buffers and prepare the latter */
        for (i = 0, j = 0; i < obj->ioo_bufcnt; i++) {
                rc = dt_bufs_get(env, ofd_object_child(fo),
                                 rnb + i, lnb + j, 1,
                                 ofd_object_capa(env, fo));
                LASSERT(rc > 0);
                LASSERT(rc <= PTLRPC_MAX_BRW_PAGES);
                /* correct index for local buffers to continue with */
                for (k = 0; k < rc; k++) {
                        lnb[j+k].lnb_flags = rnb[i].rnb_flags;
                        if (!(rnb[i].rnb_flags & OBD_BRW_GRANTED))
                                lnb[j+k].lnb_rc = -ENOSPC;
                        if (!(rnb[i].rnb_flags & OBD_BRW_ASYNC))
                                oti->oti_sync_write = 1;
                }
                j += rc;
                LASSERT(j <= PTLRPC_MAX_BRW_PAGES);
                tot_bytes += rnb[i].rnb_len;
        }
        *nr_local = j;
        LASSERT(*nr_local > 0 && *nr_local <= PTLRPC_MAX_BRW_PAGES);

        lprocfs_counter_add(ofd_obd(ofd)->obd_stats,
                            LPROC_OFD_WRITE_BYTES, tot_bytes);
        rc = dt_write_prep(env, ofd_object_child(fo), lnb, *nr_local);
        if (unlikely(rc != 0)) {
                dt_bufs_put(env, ofd_object_child(fo), lnb, *nr_local);
                ofd_read_unlock(env, fo);
                /* ofd_grant_prepare_write() was called, so we must commit */
                ofd_grant_commit(env, exp, rc);
        }

        ofd_object_put(env, fo);
        RETURN(rc);
out:
        /* let's still process incoming grant information packed in the oa,
         * but without enforcing grant since we won't proceed with the write.
         * Just like a read request actually. */
        ofd_grant_prepare_read(env, exp, oa);
        RETURN(rc);
}

int ofd_preprw(int cmd, struct obd_export *exp, struct obdo *oa,
               int objcount, struct obd_ioobj *obj,
               struct niobuf_remote *rnb, int *nr_local,
               struct niobuf_local *lnb, struct obd_trans_info *oti,
               struct lustre_capa *capa)
{
        struct lu_env *env = oti->oti_env;
        struct ofd_device *ofd = ofd_exp(exp);
        struct ofd_thread_info *info;
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

        info = ofd_info_init(env, exp);

        LASSERT(objcount == 1);
        LASSERT(obj->ioo_bufcnt > 0);

        fid_ostid_unpack(&info->fti_fid, &oa->o_oi, 0);
        if (cmd == OBD_BRW_WRITE) {
                rc = ofd_auth_capa(ofd, &info->fti_fid, oa->o_seq,
                                      capa, CAPA_OPC_OSS_WRITE);
                if (rc == 0) {
                        LASSERT(oa != NULL);
                        la_from_obdo(&info->fti_attr, oa, OBD_MD_FLGETATTR);
                        rc = ofd_preprw_write(env, exp, ofd, &info->fti_fid,
                                                 &info->fti_attr, oa, objcount,
                                                 obj, rnb, nr_local, lnb, oti);
                }
        } else if (cmd == OBD_BRW_READ) {
                rc = ofd_auth_capa(ofd, &info->fti_fid, oa->o_seq,
                                      capa, CAPA_OPC_OSS_READ);
                if (rc == 0) {
                        ofd_grant_prepare_read(env, exp, oa);
                        rc = ofd_preprw_read(env, ofd, &info->fti_fid,
                                             &info->fti_attr, obj->ioo_bufcnt,
                                             rnb, nr_local, lnb);
                        obdo_from_la(oa, &info->fti_attr, LA_ATIME);
                }
        } else {
                LBUG();
                rc = -EPROTO;
        }
        RETURN(rc);
}

static int
ofd_commitrw_read(const struct lu_env *env, struct ofd_device *ofd,
                  struct lu_fid *fid, int objcount, int niocount,
                  struct niobuf_local *lnb)
{
        struct ofd_object *fo;
        ENTRY;

        LASSERT(niocount > 0);

        fo = ofd_object_find(env, ofd, fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));
        LASSERT(fo != NULL);
        LASSERT(ofd_object_exists(fo));
        dt_bufs_put(env, ofd_object_child(fo), lnb, niocount);

        ofd_read_unlock(env, fo);
        ofd_object_put(env, fo);

        RETURN(0);
}

/*
 * If the object still has SUID+SGID bits set (see ofd_precreate_object()) then
 * we will accept the UID+GID if sent by the client for initializing the
 * ownership of this object.  We only allow this to happen once (so clear these
 * bits) and later only allow setattr.
 */
int
ofd_attr_handle_ugid(const struct lu_env *env, struct ofd_object *fo,
                     struct lu_attr *la, int is_setattr)
{
        struct ofd_thread_info *info = ofd_info(env);
        struct lu_attr         *ln = &info->fti_attr2;
        __u32                   mask = 0;
        int                     rc;
        ENTRY;

        if (!(la->la_valid & LA_UID) && !(la->la_valid & LA_GID))
                RETURN(0);

        rc = dt_attr_get(env, ofd_object_child(fo), ln, BYPASS_CAPA);
        if (rc != 0)
                RETURN(rc);

        LASSERT(ln->la_valid & LA_MODE);

        if (!is_setattr) {
                if (!(ln->la_mode & S_ISUID))
                        la->la_valid &= ~LA_UID;
                if (!(ln->la_mode & S_ISGID))
                        la->la_valid &= ~LA_GID;
        }

        if ((la->la_valid & LA_UID) && (ln->la_mode & S_ISUID))
                mask |= S_ISUID;
        if ((la->la_valid & LA_GID) && (ln->la_mode & S_ISGID))
                mask |= S_ISGID;
        if (mask != 0) {
                if (!(la->la_valid & LA_MODE) || !is_setattr) {
                        la->la_mode = ln->la_mode;
                        la->la_valid |= LA_MODE;
                }
                la->la_mode &= ~mask;
        }

        RETURN(0);
}

static int
ofd_write_attr_set(const struct lu_env *env, struct ofd_device *ofd,
                   struct ofd_object *ofd_obj, struct lu_attr *la,
                   struct filter_fid *ff)
{
        struct ofd_thread_info *info = ofd_info(env);
        __u64                   valid = la->la_valid;
        int                     rc;
        struct thandle         *th;
        struct dt_object       *dt_obj;
        int                     ff_needed = 0;
        ENTRY;

        LASSERT(la);

        dt_obj = ofd_object_child(ofd_obj);
        LASSERT(dt_obj != NULL);

        la->la_valid &= LA_UID | LA_GID;

        rc = ofd_attr_handle_ugid(env, ofd_obj, la, 0 /* !is_setattr */);
        if (rc != 0)
                GOTO(out, rc);

        if (ff != NULL) {
                rc = ofd_object_ff_check(env, ofd_obj);
                if (rc == -ENODATA)
                        ff_needed = 1;
                else if (rc < 0)
                        GOTO(out, rc);
        }

        if (!la->la_valid && !ff_needed)
                /* no attributes to set */
                GOTO(out, 0);

        th = ofd_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(out, PTR_ERR(th));

        if (la->la_valid) {
                rc = dt_declare_attr_set(env, dt_obj, la, th);
                if (rc)
                        GOTO(out_tx, rc);
        }

        if (ff_needed) {
                info->fti_buf.lb_buf = ff;
                info->fti_buf.lb_len = sizeof(*ff);
                rc = dt_declare_xattr_set(env, dt_obj, &info->fti_buf,
                                          XATTR_NAME_FID, 0, th);
                if (rc)
                        GOTO(out_tx, rc);
        }

        /* We don't need a transno for this operation which will be re-executed
         * anyway when the OST_WRITE (with a transno assigned) is replayed */
        rc = dt_trans_start_local(env, ofd->ofd_osd , th);
        if (rc)
                GOTO(out_tx, rc);

        /* set uid/gid */
        if (la->la_valid) {
                rc = dt_attr_set(env, dt_obj, la, th,
                                 ofd_object_capa(env, ofd_obj));
                if (rc)
                        GOTO(out_tx, rc);
        }

        /* set filter fid EA */
        if (ff_needed) {
                rc = dt_xattr_set(env, dt_obj, &info->fti_buf, XATTR_NAME_FID,
                                  0, th, BYPASS_CAPA);
                if (rc)
                        GOTO(out_tx, rc);
        }

        EXIT;
out_tx:
        dt_trans_stop(env, ofd->ofd_osd, th);
out:
        la->la_valid = valid;
        return rc;
}

static int
ofd_commitrw_write(const struct lu_env *env, struct ofd_device *ofd,
                   struct lu_fid *fid, struct lu_attr *la,
                   struct filter_fid *ff, int objcount,
                   int niocount, struct niobuf_local *lnb,
                   struct obd_trans_info *oti, int old_rc)
{
        struct ofd_thread_info *info = ofd_info(env);
        struct ofd_object      *fo;
        struct dt_object       *o;
        struct thandle         *th;
        int                     rc = 0;
        int                     retries = 0;
        ENTRY;

        LASSERT(objcount == 1);

        fo = ofd_object_find(env, ofd, fid);
        if (IS_ERR(fo))
                GOTO(out_nofo, rc = PTR_ERR(fo));

        LASSERT(fo != NULL);
        LASSERT(ofd_object_exists(fo));

        o = ofd_object_child(fo);
        LASSERT(o != NULL);

        if (old_rc)
                GOTO(out, rc = old_rc);

        /*
         * The first write to each object must set some attributes.  It is
         * important to set the uid/gid before calling
         * dt_declare_write_commit() since quota enforcement is now handled in
         * declare phases.
         */
        rc = ofd_write_attr_set(env, ofd, fo, la, ff);
        if (rc)
                GOTO(out, rc);

        la->la_valid &= LA_ATIME | LA_MTIME | LA_CTIME;

retry:
        th = ofd_trans_create(env, ofd);
        if (IS_ERR(th))
                GOTO(out, rc = PTR_ERR(th));

        th->th_sync |= oti->oti_sync_write;

        rc = dt_declare_write_commit(env, o, lnb, niocount, th);
        if (rc)
                GOTO(out_stop, rc);

        if (la->la_valid) {
                /* update [mac]time if needed */
                rc = dt_declare_attr_set(env, o, la, th);
                if (rc)
                        GOTO(out_stop, rc);
        }

        rc = ofd_trans_start(env, ofd, fo, th);
        if (rc)
                GOTO(out_stop, rc);

        rc = dt_write_commit(env, o, lnb, niocount, th);
        if (rc)
                GOTO(out_stop, rc);

        if (la->la_valid) {
                rc = dt_attr_set(env, o, la, th, ofd_object_capa(env, fo));
                if (rc)
                        GOTO(out_stop, rc);
        }

        /* get attr to return */
        dt_attr_get(env, o, la, ofd_object_capa(env, fo));

out_stop:
        /* Force commit to make the just-deleted blocks
         * reusable. LU-456 */
        if (rc == -ENOSPC)
                th->th_sync = 1;

        ofd_trans_stop(env, ofd, th, rc);
        if (rc == -ENOSPC && retries++ < 3) {
                CDEBUG(D_INODE, "retry after force commit, retries:%d\n",
                       retries);
                goto retry;
        }

out:
        dt_bufs_put(env, o, lnb, niocount);
        ofd_read_unlock(env, fo);
        ofd_object_put(env, fo);
out_nofo:
        ofd_grant_commit(env, info->fti_exp, old_rc);
        RETURN(rc);
}

void ofd_prepare_fidea(struct filter_fid *ff, struct obdo *oa)
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

int ofd_commitrw(int cmd, struct obd_export *exp,
                 struct obdo *oa, int objcount, struct obd_ioobj *obj,
                 struct niobuf_remote *rnb, int npages,
                 struct niobuf_local *lnb, struct obd_trans_info *oti,
                 int old_rc)
{
        struct ofd_thread_info *info;
        struct ofd_mod_data    *fmd;
        __u64                   valid;
        struct ofd_device      *ofd = ofd_exp(exp);
        struct lu_env          *env = oti->oti_env;
        struct filter_fid      *ff = NULL;
        int                     rc = 0;

        info = ofd_info(env);
        ofd_oti2info(info, oti);

        LASSERT(npages > 0);

        fid_ostid_unpack(&info->fti_fid, &oa->o_oi, 0);
        if (cmd == OBD_BRW_WRITE) {
                /* Don't update timestamps if this write is older than a
                 * setattr which modifies the timestamps. b=10150 */

                /* XXX when we start having persistent reservations this needs
                 * to be changed to ofd_fmd_get() to create the fmd if it
                 * doesn't already exist so we can store the reservation handle
                 * there. */
                valid = OBD_MD_FLUID | OBD_MD_FLGID;
                fmd = ofd_fmd_find(exp, &info->fti_fid);
                if (!fmd || fmd->fmd_mactime_xid < info->fti_xid)
                        valid |= OBD_MD_FLATIME | OBD_MD_FLMTIME |
                                 OBD_MD_FLCTIME;
                ofd_fmd_put(exp, fmd);
                la_from_obdo(&info->fti_attr, oa, valid);

                if (oa->o_valid & OBD_MD_FLFID) {
                        ff = &info->fti_mds_fid;
                        ofd_prepare_fidea(ff, oa);
                }

                rc = ofd_commitrw_write(env, ofd, &info->fti_fid,
                                        &info->fti_attr, ff, objcount, npages,
                                        lnb, oti, old_rc);
                if (rc == 0)
                        obdo_from_la(oa, &info->fti_attr,
                                     OFD_VALID_FLAGS | LA_GID | LA_UID);
                else
                        obdo_from_la(oa, &info->fti_attr, LA_GID | LA_UID);

                if (ofd_grant_prohibit(exp, ofd))
                        /* Trick to prevent clients from waiting for bulk write
                         * in flight since they won't get any grant in the reply
                         * anyway so they had better firing the sync write RPC
                         * straight away */
                        oa->o_valid |= OBD_MD_FLUSRQUOTA | OBD_MD_FLGRPQUOTA;

                if (old_rc == 0) {
#if 0
                        /* update per-buffer error codes */
                        if (rcs != NULL) {
                                memset(rcs, 0, npages * sizeof(__u32));
                                /* XXX: update rcs */
                                /* for (i = 0; i < npages; i++)
                                if (lnb[i].lnb_rc < 0)
                                        rcs[lnb[i].rindex] = lnb[i].lnb_rc;
                                */
                        }
#endif
                }
        } else if (cmd == OBD_BRW_READ) {
                struct ldlm_namespace *ns = ofd->ofd_namespace;

                /* If oa != NULL then ofd_preprw_read updated the inode
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
                rc = ofd_commitrw_read(env, ofd, &info->fti_fid, objcount,
                                          npages, lnb);
                if (old_rc)
                        rc = old_rc;
        } else {
                LBUG();
                rc = -EPROTO;
        }

        ofd_info2oti(info, oti);
        RETURN(rc);
}
