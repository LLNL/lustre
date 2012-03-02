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
 * lustre/ofd/filter_fs.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Alex Tomas <alex@clusterfs.com>
 * Author: Mike Pershin <tappro@sun.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include <libcfs/libcfs.h>
#include "ofd_internal.h"

int filter_record_write(const struct lu_env *env, struct filter_device *ofd,
                        struct dt_object *dt, void *data, loff_t _off, int len,
                        struct thandle *_th)
{
        struct thandle *th;
        struct lu_buf   buf;
        loff_t          off;
        int             rc;
        ENTRY;

        LASSERT(ofd);
        LASSERT(dt);
        LASSERT(len);
        LASSERT(data);

        buf.lb_buf = data;
        buf.lb_len = len;
        off = _off;

        th = _th;
        if (th == NULL) {
                th = filter_trans_create(env, ofd);
                if (IS_ERR(th))
                        RETURN(PTR_ERR(th));
                rc = dt_declare_record_write(env, dt, buf.lb_len, off, th);
                LASSERT(rc == 0);
                rc = filter_trans_start(env, ofd, th);
                if (rc)
                        RETURN(rc);
        }

        rc = dt_record_write(env, dt, &buf, &off, th);

        if (_th == NULL) {
                LASSERT(th);
                filter_trans_stop(env, ofd, th);
        }

        RETURN(rc);
}

obd_id filter_last_id(struct filter_device *ofd, obd_seq group)
{
        obd_id id;

        LASSERT(group <= ofd->ofd_max_group);

        cfs_spin_lock(&ofd->ofd_objid_lock);
        id = ofd->ofd_last_objids[group];
        cfs_spin_unlock(&ofd->ofd_objid_lock);

        return id;
}

void filter_last_id_set(struct filter_device *ofd, obd_id id, obd_seq group)
{
        LASSERT(group <= ofd->ofd_max_group);
        cfs_spin_lock(&ofd->ofd_objid_lock);
        if (ofd->ofd_last_objids[group] < id)
                ofd->ofd_last_objids[group] = id;
        cfs_spin_unlock(&ofd->ofd_objid_lock);
}

int filter_last_id_write(const struct lu_env *env, struct filter_device *ofd,
                         obd_seq group, struct thandle *th)
{
        obd_id          tmp;
        int             rc;
        ENTRY;

        CDEBUG(D_INODE, "%s: write last_objid for group "LPU64": "LPU64"\n",
               filter_obd(ofd)->obd_name, group, filter_last_id(ofd, group));

        tmp = cpu_to_le64(filter_last_id(ofd, group));

        rc = filter_record_write(env, ofd, ofd->ofd_lastid_obj[group],
                                 &tmp, 0, sizeof(tmp), th);

        RETURN(rc);
}

int filter_last_group_write(const struct lu_env *env, struct filter_device *ofd)
{
        __u32   tmp;
        int     rc;
        ENTRY;

        tmp = cpu_to_le32(ofd->ofd_max_group);

        rc = filter_record_write(env, ofd, ofd->ofd_last_group_file,
                                 &tmp, 0, sizeof(tmp), NULL);

        RETURN(rc);
}

int filter_group_load(const struct lu_env *env,
                      struct filter_device *ofd, int group)
{
        struct filter_object      *fo;
        struct dt_object          *dob;
        struct lu_fid              fid;
        struct lu_attr             attr;
        struct lu_buf              buf;
        loff_t                     off;
        __u64                      lastid;
        int                        rc;
        ENTRY;

        /* if group is already initialized */
        if (ofd->ofd_lastid_obj[group])
                GOTO(cleanup, rc = 0);

        lu_local_obj_fid(&fid, OFD_GROUP0_LAST_OID + group);
        memset(&attr, 0, sizeof(attr));
        attr.la_valid = LA_MODE;
        attr.la_mode = S_IFREG | 0666;

        /* create object tracking per-group last created
         * id to be used by orphan recovery mechanism */
        fo = filter_object_find_or_create(env, ofd, &fid, &attr);
        LASSERT(!IS_ERR(fo));
        dob = filter_object_child(fo);
        ofd->ofd_lastid_obj[group] = dob;

        rc = dt_attr_get(env, dob, &attr, BYPASS_CAPA);
        if (rc)
                GOTO(cleanup, rc);

        if (attr.la_size == 0) {
                /* object is just created, initialize last id */
                ofd->ofd_last_objids[group] = FILTER_INIT_OBJID;
                filter_last_id_write(env, ofd, group, 0);

        } else if (attr.la_size == sizeof(lastid)) {
                off = 0;
                buf.lb_buf = &lastid;
                buf.lb_len = sizeof(lastid);
                rc = dt_record_read(env, dob, &buf, &off);
                if (rc) {
                        CERROR("can't read last_id: %d\n", rc);
                        GOTO(cleanup, rc);
                }
                ofd->ofd_last_objids[group] = le64_to_cpu(lastid);

        } else {
                CERROR("corrupted size %Lu LAST_ID of group %u\n",
                       (unsigned long long) attr.la_size, group);
                rc = -EINVAL;
        }

cleanup:
        RETURN(rc);
}

/* filter groups managements */
int filter_groups_init(const struct lu_env *env, struct filter_device *ofd)
{
        struct filter_thread_info *info = filter_info(env);
        unsigned long              groups_size;
        __u32                      last_group;
        struct lu_buf              buf;
        loff_t                     off;
        int rc = 0, i;
        ENTRY;

        cfs_spin_lock_init(&ofd->ofd_objid_lock);

        rc = dt_attr_get(env, ofd->ofd_last_group_file,
                         &info->fti_attr, BYPASS_CAPA);
        if (rc)
                GOTO(cleanup, rc);

        groups_size = (unsigned long)info->fti_attr.la_size;

        if (groups_size == 0) {
                ofd->ofd_max_group = 0;
                goto skip_read;
        }

        if (groups_size != sizeof(last_group)) {
                CERROR("groups file is corrupted? size = %lu\n", groups_size);
                GOTO(cleanup, rc = -EIO);
        }

        off = 0;
        buf.lb_buf = &last_group;
        buf.lb_len = sizeof(last_group);
        rc = dt_record_read(env, ofd->ofd_last_group_file, &buf, &off);
        if (rc) {
                CERROR("can't read LAST_GROUP: %d\n", rc);
                GOTO(cleanup, rc);
        }

        ofd->ofd_max_group = le32_to_cpu(last_group);
        LASSERT(ofd->ofd_max_group <= FILTER_MAX_GROUPS); /* XXX: dynamic? */

skip_read:
        for (i = 0; i <= ofd->ofd_max_group; i++) {
               rc = filter_group_load(env, ofd, i);
               if (rc) {
                        CERROR("can't load group %d: %d\n", i, rc);
                        GOTO(cleanup, rc);
               }
        }

        CWARN("%s: %u groups initialized\n",
              filter_obd(ofd)->obd_name, ofd->ofd_max_group + 1);

cleanup:
        RETURN(rc);
}

static int filter_last_rcvd_header_read(const struct lu_env *env,
                                        struct filter_device *ofd)
{
        struct filter_thread_info *info = filter_info(env);
        struct lu_buf              buf;
        loff_t                     off;
        int rc;

        off = 0;
        buf.lb_buf = &info->fti_fsd;
        buf.lb_len = sizeof(info->fti_fsd);

        rc = dt_record_read(env, ofd->ofd_last_rcvd, &buf, &off);
        if (rc == 0)
                lsd_le_to_cpu(&info->fti_fsd, &ofd->ofd_fsd);
        return rc;
}

int filter_last_rcvd_header_write(const struct lu_env *env,
                                  struct filter_device *ofd,
                                  struct thandle *th)
{
        struct filter_thread_info *info;
        int                        rc;
        ENTRY;

        info = lu_context_key_get(&env->le_ctx, &filter_thread_key);
        LASSERT(info);

        lsd_cpu_to_le(&ofd->ofd_fsd, &info->fti_fsd);

        rc = filter_record_write(env, ofd, ofd->ofd_last_rcvd,
                                 &info->fti_fsd, 0, sizeof(info->fti_fsd), th);

        CDEBUG(D_INFO, "write last_rcvd header rc = %d:\n"
               "uuid = %s\nlast_transno = "LPU64"\n",
               rc, ofd->ofd_fsd.lsd_uuid, ofd->ofd_fsd.lsd_last_transno);

        RETURN(rc);
}

static int filter_last_rcvd_read(const struct lu_env *env,
                                 struct filter_device *ofd,
                                 struct lsd_client_data *lcd, loff_t *off)
{
        struct filter_thread_info *info = filter_info(env);
        struct lu_buf              buf;
        int                        rc;

        buf.lb_buf = &info->fti_fcd;
        buf.lb_len = sizeof(*lcd);

        rc = dt_record_read(env, ofd->ofd_last_rcvd, &buf, off);
        if (rc == 0)
                lcd_le_to_cpu((struct lsd_client_data *) &info->fti_fcd, lcd);
        return rc;
}

int filter_last_rcvd_write(const struct lu_env *env,
                           struct filter_device *ofd,
                           struct lsd_client_data *lcd,
                           loff_t *off, struct thandle *th)
{
        struct filter_thread_info *info = filter_info(env);
        int                        rc;

        lcd_cpu_to_le(lcd, &info->fti_fcd);

        rc = filter_record_write(env, ofd, ofd->ofd_last_rcvd,
                                 &info->fti_fcd, *off, sizeof(*lcd), th);

        return rc;
}

static inline int filter_clients_data_init(const struct lu_env *env,
                                           struct filter_device *ofd,
                                           unsigned long fsize)
{
        struct obd_device *obd = filter_obd(ofd);
        struct lr_server_data *fsd = &ofd->ofd_fsd;
        struct lsd_client_data *lcd = NULL;
        struct filter_export_data *fed;
        int cl_idx, rc = 0;
        loff_t off = fsd->lsd_client_start;

        CLASSERT (offsetof(struct lsd_client_data, lcd_padding) +
                 sizeof(lcd->lcd_padding) == LR_CLIENT_SIZE);

        OBD_ALLOC_PTR(lcd);
        if (!lcd)
                RETURN(-ENOMEM);

        for (cl_idx = 0; off < fsize; cl_idx++) {
                struct obd_export *exp;
                __u64 last_rcvd;

                /* Don't assume off is incremented properly by
                 * fsfilt_read_record(), in case sizeof(*lcd)
                 * isn't the same as fsd->lsd_client_size.  */
                off = fsd->lsd_client_start + cl_idx * fsd->lsd_client_size;
                rc = filter_last_rcvd_read(env, ofd, lcd, &off);
                if (rc) {
                        CERROR("error reading FILT %s idx %d off %llu: rc %d\n",
                               LAST_RCVD, cl_idx, off, rc);
                        rc = 0;
                        break; /* read error shouldn't cause startup to fail */
                }

                if (lcd->lcd_uuid[0] == '\0') {
                        CDEBUG(D_INFO, "skipping zeroed client at offset %d\n",
                               cl_idx);
                        continue;
                }

                last_rcvd = lcd->lcd_last_transno;

                /* These exports are cleaned up by filter_disconnect(), so they
                 * need to be set up like real exports as filter_connect() does.
                 */
                exp = class_new_export(obd, (struct obd_uuid *)lcd->lcd_uuid);

                CDEBUG(D_HA, "RCVRNG CLIENT uuid: %s idx: %d lr: "LPU64
                       " srv lr: "LPU64"\n", lcd->lcd_uuid, cl_idx,
                       last_rcvd, fsd->lsd_last_transno);

                if (IS_ERR(exp)) {
                        if (PTR_ERR(exp) == -EALREADY) {
                                /* export already exists, zero out this one */
                                CERROR("Duplicate export %s!\n", lcd->lcd_uuid);
                                continue;
                        }
                        GOTO(err_out, rc = PTR_ERR(exp));
                }

                fed = &exp->exp_filter_data;
                *fed->fed_ted.ted_lcd = *lcd;
#if 0
                fed->fed_group = lcd->lcd_group;
#endif
                filter_export_stats_init(ofd, exp, NULL);
                rc = filter_client_add(env, ofd, fed, cl_idx);
                LASSERTF(rc == 0, "rc = %d\n", rc); /* can't fail existing */
                /* VBR: set export last committed version */
                exp->exp_last_committed = last_rcvd;
                cfs_spin_lock(&exp->exp_lock);
                exp->exp_connecting = 0;
                exp->exp_in_recovery = 0;
                cfs_spin_unlock(&exp->exp_lock);
                obd->obd_max_recoverable_clients++;
                class_export_put(exp);

                /* Need to check last_rcvd even for duplicated exports. */
                CDEBUG(D_OTHER, "client at idx %d has last_rcvd = "LPU64"\n",
                       cl_idx, last_rcvd);

                cfs_spin_lock(&ofd->ofd_transno_lock);
                if (last_rcvd > fsd->lsd_last_transno)
                        fsd->lsd_last_transno = last_rcvd;
                cfs_spin_unlock(&ofd->ofd_transno_lock);
        }

err_out:
        OBD_FREE_PTR(lcd);
        RETURN(rc);
}

void filter_free_server_data(void)
{
        LBUG();
}

int filter_server_data_update(const struct lu_env *env,
                              struct filter_device *ofd)
{
        int rc = 0;
        ENTRY;

        CDEBUG(D_SUPER, "OSS mount_count is "LPU64", last_transno is "LPU64"\n",
               ofd->ofd_fsd.lsd_mount_count, ofd->ofd_fsd.lsd_last_transno);

        cfs_spin_lock(&ofd->ofd_transno_lock);
        ofd->ofd_fsd.lsd_last_transno = ofd->ofd_lut.lut_last_transno;
        cfs_spin_unlock(&ofd->ofd_transno_lock);

        /*
         * This may be called from difficult reply handler and
         * mdt->mdt_last_rcvd may be NULL that time.
         */
        if (ofd->ofd_last_rcvd != NULL)
                rc = filter_last_rcvd_header_write(env, ofd, NULL);

        RETURN(rc);
}

int filter_server_data_init(const struct lu_env *env,
                            struct filter_device *ofd)
{
        struct filter_thread_info *info = filter_info(env);
        struct lr_server_data *fsd = &ofd->ofd_fsd;
        struct obd_device *obd = filter_obd(ofd);
        unsigned long last_rcvd_size;
#if 0
        __u64 mount_count;
#endif
        int rc;

        rc = dt_attr_get(env, ofd->ofd_last_rcvd, &info->fti_attr, BYPASS_CAPA);
        if (rc)
                RETURN(rc);

        last_rcvd_size = (unsigned long)info->fti_attr.la_size;

        /* ensure padding in the struct is the correct size */
        CLASSERT (offsetof(struct lr_server_data, lsd_padding) +
                  sizeof(fsd->lsd_padding) == LR_SERVER_SIZE);

        if (last_rcvd_size == 0) {
                LCONSOLE_WARN("%s: new disk, initializing\n", obd->obd_name);

                memcpy(fsd->lsd_uuid, obd->obd_uuid.uuid,
                       sizeof(fsd->lsd_uuid));
                fsd->lsd_last_transno = 0;
                fsd->lsd_mount_count = 0;
                fsd->lsd_server_size = LR_SERVER_SIZE;
                fsd->lsd_client_start = LR_CLIENT_START;
                fsd->lsd_client_size = LR_CLIENT_SIZE;
                fsd->lsd_subdir_count = FILTER_SUBDIR_COUNT;
                fsd->lsd_feature_incompat = OBD_INCOMPAT_OST;
        } else {
                rc = filter_last_rcvd_header_read(env, ofd);
                if (rc) {
                        CDEBUG(D_INODE,"OBD filter: error reading %s: rc %d\n",
                               LAST_RCVD, rc);
                        GOTO(err_fsd, rc);
                }
                if (strcmp((char *) fsd->lsd_uuid, (char *) obd->obd_uuid.uuid)) {
                        LCONSOLE_ERROR("Trying to start OBD %s using the wrong"
                                       " disk %s. Were the /dev/ assignments "
                                       "rearranged?\n",
                                       obd->obd_uuid.uuid, fsd->lsd_uuid);
                        GOTO(err_fsd, rc = -EINVAL);
                }
        }

        fsd->lsd_mount_count++;
        obd->u.obt.obt_mount_count = fsd->lsd_mount_count;
        ofd->ofd_subdir_count = fsd->lsd_subdir_count;

        if (fsd->lsd_feature_incompat & ~FILTER_INCOMPAT_SUPP) {
                CERROR("%s: unsupported incompat filesystem feature(s) %x\n",
                       obd->obd_name,
                       fsd->lsd_feature_incompat & ~FILTER_INCOMPAT_SUPP);
                GOTO(err_fsd, rc = -EINVAL);
        }
        if (fsd->lsd_feature_rocompat & ~FILTER_ROCOMPAT_SUPP) {
                CERROR("%s: unsupported read-only filesystem feature(s) %x\n",
                       obd->obd_name, 
                       fsd->lsd_feature_rocompat & ~FILTER_ROCOMPAT_SUPP);
                /* Do something like remount filesystem read-only */
                GOTO(err_fsd, rc = -EINVAL);
        }

        CDEBUG(D_INODE, "%s: server last_transno : "LPU64"\n",
               obd->obd_name, fsd->lsd_last_transno);
        CDEBUG(D_INODE, "%s: server mount_count: "LPU64"\n",
               obd->obd_name, fsd->lsd_mount_count);
        CDEBUG(D_INODE, "%s: server data size: %u\n",
               obd->obd_name, fsd->lsd_server_size);
        CDEBUG(D_INODE, "%s: per-client data start: %u\n",
               obd->obd_name, fsd->lsd_client_start);
        CDEBUG(D_INODE, "%s: per-client data size: %u\n",
               obd->obd_name, fsd->lsd_client_size);
        CDEBUG(D_INODE, "%s: server subdir_count: %u\n",
               obd->obd_name, fsd->lsd_subdir_count);
        CDEBUG(D_INODE, "%s: last_rcvd clients: %lu\n", obd->obd_name,
               last_rcvd_size <= fsd->lsd_client_start ? 0 :
               (last_rcvd_size - fsd->lsd_client_start) /
                fsd->lsd_client_size);

        if (!obd->obd_replayable) {
                CWARN("%s: recovery support OFF\n", obd->obd_name);
        }

        rc = filter_clients_data_init(env, ofd, last_rcvd_size);

        cfs_spin_lock(&ofd->ofd_transno_lock);
        obd->obd_last_committed = fsd->lsd_last_transno;
        cfs_spin_unlock(&ofd->ofd_transno_lock);

        /* save it, so mount count and last_transno is current */
        rc = filter_server_data_update(env, ofd);
        if (rc)
                GOTO(err_fsd, rc);

        RETURN(0);

err_fsd:
        class_disconnect_exports(obd);
        RETURN(rc);
}

