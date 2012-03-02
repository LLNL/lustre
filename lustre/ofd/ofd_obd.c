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
 * lustre/ofd/ofd_obd.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Alex Tomas <alex@clusterfs.com>
 * Author: Mike Pershin <tappro@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include "ofd_internal.h"

extern int ost_handle(struct ptlrpc_request *req);

static int filter_obd_notify(struct obd_device *host,
                          struct obd_device *watched,
                          enum obd_notify_event ev, void *owner)
{
        struct filter_device *ofd = filter_dev(host->obd_lu_dev);
        ENTRY;

        switch (ev) {
        case OBD_NOTIFY_CONFIG:
                target_recovery_init(&ofd->ofd_lut, ost_handle);
                LASSERT(host->obd_no_conn);
                cfs_spin_lock(&host->obd_dev_lock);
                host->obd_no_conn = 0;
                cfs_spin_unlock(&host->obd_dev_lock);
        default:
                CDEBUG(D_INFO, "Notification 0x%x\n", ev);
        }
        RETURN(0);
}

static int filter_parse_connect_data(const struct lu_env *env,
                                     struct obd_export *exp,
                                     struct obd_connect_data *data)
{
        struct filter_device *ofd = filter_exp(exp);
        struct filter_export_data *fed = &exp->exp_filter_data;
        int rc = 0;

        if (!data)
                RETURN(0);

        CDEBUG(D_RPCTRACE, "%s: cli %s/%p ocd_connect_flags: "LPX64
               " ocd_version: %x ocd_grant: %d ocd_index: %u\n",
               exp->exp_obd->obd_name, exp->exp_client_uuid.uuid, exp,
               data->ocd_connect_flags, data->ocd_version,
               data->ocd_grant, data->ocd_index);

        data->ocd_connect_flags &= OST_CONNECT_SUPPORTED;
        exp->exp_connect_flags = data->ocd_connect_flags;
        data->ocd_version = LUSTRE_VERSION_CODE;

#if 0
        if ((exp->exp_connect_flags & OBD_CONNECT_FID) == 0) {
                CWARN("%s: OST requires FID support (flag="LPX64
                      "), but client not\n",
                      exp->exp_obd->obd_name,
                      exp->exp_connect_flags);
                RETURN(-EBADF);
        }
#endif

        if (exp->exp_connect_flags & OBD_CONNECT_GRANT) {
                obd_size left, want;

                cfs_mutex_down(&ofd->ofd_grant_sem);
                left = filter_grant_space_left(env, exp);
                want = data->ocd_grant;
                filter_grant(env, exp, fed->fed_grant, want, left);
                data->ocd_grant = fed->fed_grant;
                cfs_mutex_up(&ofd->ofd_grant_sem);

                CDEBUG(D_CACHE, "%s: cli %s/%p ocd_grant: %d want: "
                       LPU64" left: "LPU64"\n", exp->exp_obd->obd_name,
                       exp->exp_client_uuid.uuid, exp,
                       data->ocd_grant, want, left);

                ofd->ofd_tot_granted_clients ++;
        }

        if (data->ocd_connect_flags & OBD_CONNECT_INDEX) {
                struct lr_server_data *lsd = &ofd->ofd_fsd;
                int index = lsd->lsd_ost_index;

                if (!(lsd->lsd_feature_compat & OBD_COMPAT_OST)) {
                        /* this will only happen on the first connect */
                        lsd->lsd_ost_index = data->ocd_index;
                        lsd->lsd_feature_compat |= OBD_COMPAT_OST;
                        filter_server_data_update(env, ofd);
                } else if (index != data->ocd_index) {
                        LCONSOLE_ERROR_MSG(0x136, "Connection from %s to index"
                                           " %u doesn't match actual OST index"
                                           " %u in last_rcvd file, bad "
                                           "configuration?\n",
                                           obd_export_nid2str(exp), index,
                                           data->ocd_index);
                        RETURN(-EBADF);
                }
        }

        if (OBD_FAIL_CHECK(OBD_FAIL_OST_BRW_SIZE)) {
                data->ocd_brw_size = 65536;
        } else if (data->ocd_connect_flags & OBD_CONNECT_BRW_SIZE) {
                data->ocd_brw_size = min(data->ocd_brw_size,
                             (__u32)(PTLRPC_MAX_BRW_PAGES << CFS_PAGE_SHIFT));
                LASSERT(data->ocd_brw_size);
        }

        if (data->ocd_connect_flags & OBD_CONNECT_CKSUM) {
                __u32 cksum_types = data->ocd_cksum_types;
                /* The client set in ocd_cksum_types the checksum types it
                 * supports. We have to mask off the algorithms that we don't
                 * support */
                data->ocd_cksum_types &= cksum_types_supported();

                /* 1.6.4- only support CRC32 and didn't set ocd_cksum_types */
                if (unlikely(data->ocd_cksum_types == 0))
                        data->ocd_cksum_types = OBD_CKSUM_CRC32;

                CDEBUG(D_RPCTRACE, "%s: cli %s supports cksum type %x, return "
                                   "%x\n", exp->exp_obd->obd_name,
                                   obd_export_nid2str(exp), cksum_types,
                                   data->ocd_cksum_types);
        } else {
                /* This client does not support OBD_CONNECT_CKSUM
                 * fall back to CRC32 */
                CDEBUG(D_RPCTRACE, "%s: cli %s does not support "
                                   "OBD_CONNECT_CKSUM, CRC32 will be used\n",
                                   exp->exp_obd->obd_name,
                                   obd_export_nid2str(exp));
        }

        if (data->ocd_connect_flags & OBD_CONNECT_MAXBYTES)
                data->ocd_maxbytes = ofd->ofd_dt_conf.ddp_maxbytes;


        /* FIXME: Do the same with the MDS UUID and fsd_peeruuid.
         * FIXME: We don't strictly need the COMPAT flag for that,
         * FIXME: as fsd_peeruuid[0] will tell us if that is set.
         * FIXME: We needed it for the index, as index 0 is valid. */

        RETURN(rc);
}

static int filter_obd_reconnect(const struct lu_env *env, struct obd_export *exp,
                                struct obd_device *obd, struct obd_uuid *cluuid,
                                struct obd_connect_data *data, void *localdata)
{
        int rc;
        ENTRY;

        if (exp == NULL || obd == NULL || cluuid == NULL)
                RETURN(-EINVAL);

        rc = lu_env_refill((struct lu_env *)env);
        if (rc != 0) {
                CERROR("Failure to refill session: '%d'\n", rc);
                RETURN(rc);
        }

        filter_info_init(env, exp);
        rc = filter_parse_connect_data(env, exp, data);

        RETURN(rc);
}

static int filter_obd_connect(const struct lu_env *env, struct obd_export **_exp,
                              struct obd_device *obd, struct obd_uuid *cluuid,
                              struct obd_connect_data *data, void *localdata)
{
        struct filter_thread_info *info;
        struct obd_export         *exp;
        struct filter_device      *ofd;
        struct lustre_handle       conn = { 0 };
        int                        rc, group;
        ENTRY;

        if (!_exp || !obd || !cluuid)
                RETURN(-EINVAL);

        ofd = filter_dev(obd->obd_lu_dev);

        rc = class_connect(&conn, obd, cluuid);
        if (rc)
                RETURN(rc);

        exp = class_conn2export(&conn);
        LASSERT(exp != NULL);

        rc = lu_env_refill((struct lu_env *)env);
        if (rc != 0) {
                CERROR("Failure to refill session: '%d'\n", rc);
                GOTO(out, rc);
        }
        info = filter_info_init(env, exp);
        info->fti_no_need_trans = 1;

        rc = filter_parse_connect_data(env, exp, data);
        if (rc)
                GOTO(out, rc);

        filter_export_stats_init(ofd, exp, localdata);
        group = data->ocd_group;
        if (obd->obd_replayable) {
                struct tg_export_data *ted = &exp->exp_target_data;
                memcpy(ted->ted_lcd->lcd_uuid, cluuid,
                       sizeof(ted->ted_lcd->lcd_uuid));
                rc = filter_client_new(env, ofd, exp);
                if (rc != 0)
                        GOTO(out, rc);
        }
        if (group == 0)
                GOTO(out, rc = 0);

        CWARN("%s: Received MDS connection ("LPX64"); group %d\n",
               obd->obd_name, exp->exp_handle.h_cookie, group);

        /* init new group */
        if (group > ofd->ofd_max_group) {
                ofd->ofd_max_group = group;
                rc = filter_group_load(env, ofd, group);
        }

out:
        if (rc != 0) {
                class_disconnect(exp);
                *_exp = NULL;
        } else {
                *_exp = exp;
        }
        RETURN(rc);
}

static int filter_obd_disconnect(struct obd_export *exp)
{
        struct lu_env env;
        struct filter_device *ofd = filter_exp(exp);
        int rc;
        ENTRY;

        LASSERT(exp);
        class_export_get(exp);

        if (!(exp->exp_flags & OBD_OPT_FORCE))
                filter_grant_sanity_check(filter_obd(ofd), __FUNCTION__);

        rc = server_disconnect_export(exp);

        /*
         * discard grants once we're sure no more
         * interaction with the client is possible
         */
        filter_grant_discard(exp);

        /* Do not erase record for recoverable client. */
        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc)
                RETURN(rc);
        if (exp->exp_obd->obd_replayable &&
            (!exp->exp_obd->obd_fail || exp->exp_failed))
                filter_client_free(&env, exp);
        lu_env_fini(&env);

        /* flush any remaining cancel messages out to the target */
        /* XXX: filter_sync_llogs(obd, exp); */
        class_export_put(exp);
        RETURN(rc);
}

/* reverse import is changed, sync all cancels */
static void filter_revimp_update(struct obd_export *exp)
{
        ENTRY;

        LASSERT(exp);
        class_export_get(exp);

        /* flush any remaining cancel messages out to the target */
#if 0
        filter_sync_llogs(exp->exp_obd, exp);
#endif
        class_export_put(exp);
        EXIT;
}

static int filter_init_export(struct obd_export *exp)
{
        int rc;

        cfs_spin_lock_init(&exp->exp_filter_data.fed_lock);
        CFS_INIT_LIST_HEAD(&exp->exp_filter_data.fed_mod_list);
        cfs_spin_lock(&exp->exp_lock);
        exp->exp_connecting = 1;
        cfs_spin_unlock(&exp->exp_lock);

        /* self-export doesn't need client data and ldlm initialization */
        if (unlikely(obd_uuid_equals(&exp->exp_obd->obd_uuid,
                                     &exp->exp_client_uuid)))
                return 0;

        rc = lut_client_alloc(exp);
        if (rc == 0)
                rc = ldlm_init_export(exp);
        if (rc)
                CERROR("%s: Can't initialize export: rc %d\n",
                       exp->exp_obd->obd_name, rc);
         return rc;
}

static int filter_destroy_export(struct obd_export *exp)
{
        struct obd_device *obd = exp->exp_obd;
        struct filter_device *ofd = filter_dev(obd->obd_lu_dev);
        struct lu_env env;
        int rc;
        ENTRY;

        if (exp->exp_filter_data.fed_pending)
                CERROR("%s: cli %s/%p has %lu pending on destroyed export\n",
                       obd->obd_name, exp->exp_client_uuid.uuid,
                       exp, exp->exp_filter_data.fed_pending);

        /* Not ported yet the b1_6 quota functionality
         * lquota_clearinfo(filter_quota_interface_ref, exp, exp->exp_obd);
         */

        target_destroy_export(exp);

        if (unlikely(obd_uuid_equals(&exp->exp_obd->obd_uuid,
                                     &exp->exp_client_uuid)))
                RETURN(0);

        ldlm_destroy_export(exp);
        lut_client_free(exp);

        if (obd_uuid_equals(&exp->exp_client_uuid, &obd->obd_uuid))
                RETURN(0);

        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc)
                RETURN(rc);

        filter_info_init(&env, exp);
        lprocfs_exp_cleanup(exp);

        if (!obd->obd_replayable)
                dt_sync(&env, ofd->ofd_osd);

        /* FIXME Check if cleanup is required here once complete
         * UOSS functionality is implemented. */
        filter_fmd_cleanup(exp);

        if (exp->exp_connect_flags & OBD_CONNECT_GRANT_SHRINK) {
                if (ofd->ofd_tot_granted_clients > 0)
                        ofd->ofd_tot_granted_clients --;
        }

        if (!(exp->exp_flags & OBD_OPT_FORCE))
                filter_grant_sanity_check(exp->exp_obd, __FUNCTION__);

        lu_env_fini(&env);
        RETURN(0);
}

static inline int filter_setup_llog_group(struct obd_export *exp,
                                          struct obd_device *obd,
                                           int group)
{
        struct obd_llog_group *olg;
        struct llog_ctxt *ctxt;
        int rc;

        olg = filter_find_create_olg(obd, group);
        if (IS_ERR(olg))
                RETURN(PTR_ERR(olg));

        llog_group_set_export(olg, exp);

        ctxt = llog_group_get_ctxt(olg, LLOG_MDS_OST_REPL_CTXT);
        LASSERTF(ctxt != NULL, "ctxt is null\n");

        rc = llog_receptor_accept(ctxt, exp->exp_imp_reverse);
        llog_ctxt_put(ctxt);
        return rc;
}

static int filter_adapt_sptlrpc_conf(struct obd_device *obd, int initial)
{
        struct filter_obd       *filter = &obd->u.filter;
        struct sptlrpc_rule_set  tmp_rset;
        int                      rc;

        sptlrpc_rule_set_init(&tmp_rset);
        rc = sptlrpc_conf_target_get_rules(obd, &tmp_rset, initial);
        if (rc) {
                CERROR("obd %s: failed get sptlrpc rules: %d\n",
                       obd->obd_name, rc);
                return rc;
        }

        sptlrpc_target_update_exp_flavor(obd, &tmp_rset);

        cfs_write_lock(&filter->fo_sptlrpc_lock);
        sptlrpc_rule_set_free(&filter->fo_sptlrpc_rset);
        filter->fo_sptlrpc_rset = tmp_rset;
        cfs_write_unlock(&filter->fo_sptlrpc_lock);

        return 0;
}

static int filter_set_mds_conn(struct obd_export *exp, void *val)
{
        int rc = 0, group;
        ENTRY;

        LCONSOLE_WARN("%s: received MDS connection from %s\n",
                      exp->exp_obd->obd_name, obd_export_nid2str(exp));
        //obd->u.filter.fo_mdc_conn.cookie = exp->exp_handle.h_cookie;

        /* setup llog imports */
        if (val != NULL)
                group = (int)(*(__u32 *)val);
        else
                group = 0; /* default value */

#if 0
        LASSERT_SEQ_IS_MDT(group);
        rc = filter_setup_llog_group(exp, obd, group);
        if (rc)
                goto out;

        if (group == FID_SEQ_OST_MDT0) {
                /* setup llog group 1 for interop */
                filter_setup_llog_group(exp, obd, FID_SEQ_LLOG);
        }

        lquota_setinfo(filter_quota_interface_ref, obd, exp);
out:
#endif
        RETURN(rc);
}

static int filter_set_info_async(struct obd_export *exp, __u32 keylen,
                                 void *key, __u32 vallen, void *val,
                                 struct ptlrpc_request_set *set)
{
        struct filter_device *ofd = filter_exp(exp);
        struct obd_device    *obd;
        int                   rc = 0;
        ENTRY;

        obd = exp->exp_obd;
        if (obd == NULL) {
                CDEBUG(D_IOCTL, "invalid export %p\n", exp);
                RETURN(-EINVAL);
        }

        if (KEY_IS(KEY_CAPA_KEY)) {
                rc = filter_update_capa_key(ofd, (struct lustre_capa_key *)val);
                if (rc)
                        CERROR("filter update capability key failed: %d\n", rc);
        } else if (KEY_IS(KEY_REVIMP_UPD)) {
                filter_revimp_update(exp);
        } else if (KEY_IS(KEY_SPTLRPC_CONF)) {
                filter_adapt_sptlrpc_conf(obd, 0);
        } else if (KEY_IS(KEY_MDS_CONN)) {
                rc = filter_set_mds_conn(exp, val);
        } else if (KEY_IS(KEY_GRANT_SHRINK)) {
                struct ost_body *body = val;
                struct lu_env    env;

                rc = lu_env_init(&env, LCT_DT_THREAD);
                if (rc)
                        RETURN(rc);
                filter_info_init(&env, exp);
                /* handle shrink grant */
                cfs_mutex_down(&ofd->ofd_grant_sem);
                filter_grant_incoming(&env, exp, &body->oa);
                cfs_mutex_up(&ofd->ofd_grant_sem);

                lu_env_fini(&env);
        } else {
                CERROR("Invalid key %s\n", (char*)key);
                rc = -EINVAL;
        }

        RETURN(rc);
}

static int filter_get_info(struct obd_export *exp, __u32 keylen, void *key,
                           __u32 *vallen, void *val, struct lov_stripe_md *lsm)
{
        struct filter_device *ofd = filter_exp(exp);
        int rc = 0;
        ENTRY;

        if (exp->exp_obd == NULL) {
                CDEBUG(D_IOCTL, "invalid client export %p\n", exp);
                RETURN(-EINVAL);
        }

        if (KEY_IS(KEY_BLOCKSIZE)) {
                __u32 *blocksize = val;
                if (blocksize) {
                        if (*vallen < sizeof(*blocksize))
                                RETURN(-EOVERFLOW);
                        *blocksize = 1 << ofd->ofd_dt_conf.ddp_block_shift;
                }
                *vallen = sizeof(*blocksize);
        } else if (KEY_IS(KEY_BLOCKSIZE_BITS)) {
                __u32 *blocksize_bits = val;
                if (blocksize_bits) {
                        if (*vallen < sizeof(*blocksize_bits))
                                RETURN(-EOVERFLOW);
                        *blocksize_bits = ofd->ofd_dt_conf.ddp_block_shift;
                }
                *vallen = sizeof(*blocksize_bits);
        } else if (KEY_IS(KEY_LAST_ID)) {
                obd_id *last_id = val;
                if (last_id) {
                        if (*vallen < sizeof(*last_id))
                                RETURN(-EOVERFLOW);
                        *last_id = filter_last_id(ofd,
                                        exp->exp_filter_data.fed_group);
                }
                *vallen = sizeof(*last_id);
        } else if (KEY_IS(KEY_FIEMAP)) {
#if 0
                struct ll_fiemap_info_key *fm_key = key;
                struct dentry *dentry;
                struct ll_user_fiemap *fiemap = val;
                struct lvfs_run_ctxt saved;
                int rc;

                if (fiemap == NULL) {
                        *vallen = fiemap_count_to_size(
                                                fm_key->fiemap.fm_extent_count);
                        RETURN(0);
                }

                dentry = __filter_oa2dentry(exp->exp_obd, &fm_key->oa.o_oi,
                                            __func__, 1);
                if (IS_ERR(dentry))
                        RETURN(PTR_ERR(dentry));

                memcpy(fiemap, &fm_key->fiemap, sizeof(*fiemap));
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                rc = fsfilt_iocontrol(obd, dentry, FSFILT_IOC_FIEMAP,
                                      (long)fiemap);
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);

                f_dput(dentry);
#else
                rc = -EOPNOTSUPP;
#endif
        } else if (KEY_IS(KEY_SYNC_LOCK_CANCEL)) {
                *((__u32 *) val) = ofd->ofd_sync_lock_cancel;
                *vallen = sizeof(__u32);
        } else {
                CERROR("Invalid key %s\n", (char*)key);
                rc = -EINVAL;
        }

        RETURN(rc);
}

static int filter_statfs(struct obd_device *obd,
                         struct obd_statfs *osfs, __u64 max_age, __u32 flags)
{
        struct filter_device      *ofd = filter_dev(obd->obd_lu_dev);
        struct filter_thread_info *info;
        struct lu_env env;
        int rc, blockbits;
        ENTRY;

        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc)
                RETURN(rc);
        info = filter_info_init(&env, NULL);

        /* at least try to account for cached pages.  its still racey and
         * might be under-reporting if clients haven't announced their
         * caches with brw recently */
        rc = dt_statfs(&env, ofd->ofd_osd, &info->fti_u.osfs);
        if (rc)
                GOTO(out, rc);

        *osfs = info->fti_u.osfs;

        LASSERTF(IS_PO2(osfs->os_bsize), "%u\n", osfs->os_bsize);
        blockbits = cfs_fls(osfs->os_bsize) - 1;

        CDEBUG(D_SUPER | D_CACHE, "blocks cached "LPU64" granted "LPU64
               " pending "LPU64" free "LPU64" avail "LPU64"\n",
               ofd->ofd_tot_dirty, ofd->ofd_tot_granted, ofd->ofd_tot_pending,
               osfs->os_bfree << blockbits, osfs->os_bavail << blockbits);

        filter_grant_sanity_check(obd, __FUNCTION__);
        osfs->os_bavail -= min(osfs->os_bavail, GRANT_FOR_LLOG +
                        ((ofd->ofd_tot_dirty + ofd->ofd_tot_pending +
                          osfs->os_bsize - 1) >> blockbits));
        CDEBUG(D_CACHE, LPU64" blocks: "LPU64" free, "LPU64" avail; "
               LPU64" objects: "LPU64" free; state %x\n",
               osfs->os_blocks, osfs->os_bfree, osfs->os_bavail,
               osfs->os_files, osfs->os_ffree, osfs->os_state);

        if (OBD_FAIL_CHECK_VALUE(OBD_FAIL_OST_ENOSPC,
                                 le32_to_cpu(ofd->ofd_fsd.lsd_ost_index)))
                osfs->os_bfree = osfs->os_bavail = 2;

        if (OBD_FAIL_CHECK_VALUE(OBD_FAIL_OST_ENOINO,
                                 le32_to_cpu(ofd->ofd_fsd.lsd_ost_index)))
                osfs->os_ffree = 0;

        if (ofd->ofd_raid_degraded)
                osfs->os_state |= OS_STATE_DEGRADED;
#if 0
        /* set EROFS to state field if FS is mounted as RDONLY. The goal is to
         * stop creating files on MDS if OST is not good shape to create
         * objects.*/
        osfs->os_state = (filter->fo_obt.obt_sb->s_flags & MS_RDONLY) ?  EROFS : 0;
#endif
out:
        lu_env_fini(&env);
        RETURN(rc);
}

int filter_setattr(struct obd_export *exp,
                   struct obd_info *oinfo, struct obd_trans_info *oti)
{
        struct filter_thread_info *info;
        struct filter_device      *ofd = filter_exp(exp);
        struct ldlm_namespace     *ns = ofd->ofd_namespace;
        struct ldlm_resource      *res;
        struct filter_object      *fo;
        struct obdo               *oa = oinfo->oi_oa;
        struct lu_env             *env = oti->oti_env;
        int                        rc = 0;
        ENTRY;

        rc = lu_env_refill(env);
        LASSERT(rc == 0);

        info = filter_info_init(env, exp);
        filter_oti2info(info, oti);

        fid_ostid_unpack(&info->fti_fid, &oinfo->oi_oa->o_oi, 0);
        ofd_build_resid(&info->fti_fid, &info->fti_resid);

        rc = filter_auth_capa(ofd, &info->fti_fid, oa->o_seq,
                              oinfo_capa(oinfo), CAPA_OPC_META_WRITE);
        if (rc)
                GOTO(out, rc);

        /* This would be very bad - accidentally truncating a file when
         * changing the time or similar - bug 12203. */
        if (oinfo->oi_oa->o_valid & OBD_MD_FLSIZE &&
            oinfo->oi_policy.l_extent.end != OBD_OBJECT_EOF) {
                static char mdsinum[48];

                if (oinfo->oi_oa->o_valid & OBD_MD_FLFID)
                        snprintf(mdsinum, sizeof(mdsinum) - 1,
                                 " of inode "LPU64"/%u", oinfo->oi_oa->o_parent_seq,
                                 oinfo->oi_oa->o_parent_oid);
                else
                        mdsinum[0] = '\0';

                CERROR("%s: setattr from %s trying to truncate objid "LPU64
                       " %s\n",
                       exp->exp_obd->obd_name, obd_export_nid2str(exp),
                       oinfo->oi_oa->o_id, mdsinum);
                GOTO(out, rc = -EPERM);
        }

        fo = filter_object_find(env, ofd, &info->fti_fid);
        if (IS_ERR(fo)) {
                CERROR("can't find object %lu:%llu\n",
                       (long unsigned) info->fti_fid.f_oid,
                       info->fti_fid.f_seq);
                GOTO(out, rc = PTR_ERR(fo));
        }

        la_from_obdo(&info->fti_attr, oinfo->oi_oa, oinfo->oi_oa->o_valid);
        info->fti_attr.la_valid &= ~LA_TYPE;

        /* setting objects attributes (including owner/group) */
        rc = filter_attr_set(env, fo, &info->fti_attr);
        if (rc)
                GOTO(out_unlock, rc);

        res = ldlm_resource_get(ns, NULL, &info->fti_resid, LDLM_EXTENT, 0);
        if (res != NULL) {
                ldlm_res_lvbo_update(res, NULL, 0);
                ldlm_resource_putref(res);
        }

        oinfo->oi_oa->o_valid = OBD_MD_FLID;

        /* Quota release needs uid/gid info */
        rc = filter_attr_get(env, fo, &info->fti_attr);
        obdo_from_la(oinfo->oi_oa, &info->fti_attr,
                     FILTER_VALID_FLAGS | LA_UID | LA_GID);
        filter_info2oti(info, oti);
out_unlock:
        filter_object_put(env, fo);
out:
        RETURN(rc);
}

static int filter_punch(struct obd_export *exp, struct obd_info *oinfo,
                        struct obd_trans_info *oti, struct ptlrpc_request_set *rqset)
{
        struct filter_device *ofd = filter_exp(exp);
        struct lu_env *env = oti->oti_env;
        struct filter_thread_info *info;
        struct ldlm_namespace *ns = ofd->ofd_namespace;
        struct ldlm_resource *res;
        struct filter_object *fo;
        int rc = 0;

        ENTRY;

        rc = lu_env_refill(env);
        LASSERT(rc == 0);

        info = filter_info_init(env, exp);
        filter_oti2info(info, oti);

        fid_ostid_unpack(&info->fti_fid, &oinfo->oi_oa->o_oi, 0);
        ofd_build_resid(&info->fti_fid, &info->fti_resid);

        CDEBUG(D_INODE, "calling punch for object "LPU64", valid = "LPX64
               ", start = "LPD64", end = "LPD64"\n", oinfo->oi_oa->o_id,
               oinfo->oi_oa->o_valid, oinfo->oi_policy.l_extent.start,
               oinfo->oi_policy.l_extent.end);

        rc = filter_auth_capa(ofd, &info->fti_fid, oinfo->oi_oa->o_seq,
                              oinfo_capa(oinfo), CAPA_OPC_OSS_TRUNC);
        if (rc)
                GOTO(out_env, rc);

        fo = filter_object_find(env, ofd, &info->fti_fid);
        if (IS_ERR(fo)) {
                CERROR("error finding object %lu:%llu: %ld\n",
                       (unsigned long) info->fti_fid.f_oid,
                       info->fti_fid.f_seq, PTR_ERR(fo));
                GOTO(out_env, rc = PTR_ERR(fo));
        }

        LASSERT(oinfo->oi_policy.l_extent.end == OBD_OBJECT_EOF);
        if (oinfo->oi_policy.l_extent.end == OBD_OBJECT_EOF) {
                /* Truncate case */
                oinfo->oi_oa->o_size = oinfo->oi_policy.l_extent.start;
        } else if (oinfo->oi_policy.l_extent.end >= oinfo->oi_oa->o_size) {
                oinfo->oi_oa->o_size = oinfo->oi_policy.l_extent.end;
        }

        la_from_obdo(&info->fti_attr, oinfo->oi_oa,
                     OBD_MD_FLMTIME | OBD_MD_FLATIME | OBD_MD_FLCTIME);
        info->fti_attr.la_valid &= ~LA_TYPE;
        info->fti_attr.la_size = oinfo->oi_policy.l_extent.start;
        info->fti_attr.la_valid |= LA_SIZE;

        rc = filter_object_punch(env, fo, oinfo->oi_policy.l_extent.start,
                                 oinfo->oi_policy.l_extent.end, &info->fti_attr);
        if (rc)
                GOTO(out, rc);

        res = ldlm_resource_get(ns, NULL, &info->fti_resid, LDLM_EXTENT, 0);
        if (res != NULL) {
                ldlm_res_lvbo_update(res, NULL, 0);
                ldlm_resource_putref(res);
        }

        oinfo->oi_oa->o_valid = OBD_MD_FLID;
        /* Quota release needs uid/gid info */
        rc = filter_attr_get(env, fo, &info->fti_attr);
        obdo_from_la(oinfo->oi_oa, &info->fti_attr,
                     FILTER_VALID_FLAGS | LA_UID | LA_GID);
        filter_info2oti(info, oti);

out:
        filter_object_put(env, fo);
out_env:
        RETURN(rc);
}

static int filter_destroy_by_fid(const struct lu_env *env,
                                 struct filter_device *ofd,
                                 const struct lu_fid *fid)
{
        struct filter_thread_info *info = filter_info(env);
        struct lustre_handle lockh;
        int flags = LDLM_AST_DISCARD_DATA, rc = 0;
        ldlm_policy_data_t policy = { .l_extent = { 0, OBD_OBJECT_EOF } };
        struct filter_object *fo;
        ENTRY;

        /* Tell the clients that the object is gone now and that they should
         * throw away any cached pages. */
        ofd_build_resid(fid, &info->fti_resid);
        rc = ldlm_cli_enqueue_local(ofd->ofd_namespace, &info->fti_resid,
                                    LDLM_EXTENT, &policy, LCK_PW, &flags,
                                    ldlm_blocking_ast, ldlm_completion_ast,
                                    NULL, NULL, 0, NULL, &lockh);

        /* We only care about the side-effects, just drop the lock. */
        if (rc == ELDLM_OK)
                ldlm_lock_decref(&lockh, LCK_PW);

        fo = filter_object_find(env, ofd, fid);
        if (IS_ERR(fo))
                RETURN(PTR_ERR(fo));
        LASSERT(fo != NULL);

        rc = filter_object_destroy(env, fo);

        filter_object_put(env, fo);
        RETURN(rc);
}

int filter_destroy(struct obd_export *exp,
                   struct obdo *oa, struct lov_stripe_md *md,
                   struct obd_trans_info *oti, struct obd_export *md_exp, void *capa)
{
        struct lu_env *env = oti->oti_env;
        struct filter_device *ofd = filter_exp(exp);
        struct filter_thread_info *info;
        struct llog_cookie *fcc = NULL;
        int rc = 0;
        ENTRY;

        rc = lu_env_refill(env);
        LASSERT(rc == 0);

        info = filter_info_init(env, exp);
        filter_oti2info(info, oti);

        if (!(oa->o_valid & OBD_MD_FLGROUP))
                oa->o_seq = 0;

        fid_ostid_unpack(&info->fti_fid, &oa->o_oi, 0);
        rc = filter_destroy_by_fid(env, ofd, &info->fti_fid);
        if (rc == -ENOENT) {
                CDEBUG(D_INODE, "destroying non-existent object "LPU64"\n",
                       oa->o_id);
                /* If object already gone, cancel cookie right now */
                if (oa->o_valid & OBD_MD_FLCOOKIE) {
                        struct llog_ctxt *ctxt;
                        struct obd_llog_group *olg;
                        fcc = &oa->o_lcookie;
                        olg = filter_find_olg(filter_obd(ofd), oa->o_seq);
                        if (IS_ERR(olg))
                                GOTO(out, rc = PTR_ERR(olg));
                        llog_group_set_export(olg, exp);

                        ctxt = llog_group_get_ctxt(olg, fcc->lgc_subsys + 1);
                        llog_cancel(ctxt, NULL, 1, fcc, 0);
                        llog_ctxt_put(ctxt);
                        fcc = NULL; /* we didn't allocate fcc, don't free it */
                }
        } else {
                if (oa->o_valid & OBD_MD_FLCOOKIE) {
                        struct llog_ctxt *ctxt;
                        fcc = &oa->o_lcookie;
                        ctxt = llog_get_context(filter_obd(ofd),
                                                fcc->lgc_subsys + 1);
                        llog_cancel(ctxt, NULL, 1, fcc, 0);
                        llog_ctxt_put(ctxt);
                        fcc = NULL; /* we didn't allocate fcc, don't free it */
                }
        }

        filter_info2oti(info, oti);
out:
        RETURN(rc);
}

static int filter_orphans_destroy(const struct lu_env *env,
                                  struct obd_export *exp,
                                  struct filter_device *ofd,
                                  struct obdo *oa)
{
        struct filter_thread_info *info = filter_info(env);
        obd_id                     last;
        int                        skip_orphan;
        int                        rc = 0;
        struct ost_id              oi = oa->o_oi;
        ENTRY;

        info->fti_no_need_trans = 1;
        LASSERT(exp != NULL);
        skip_orphan = !!(exp->exp_connect_flags & OBD_CONNECT_SKIP_ORPHAN);

        //LASSERT(cfs_mutex_try_down(&ofd->ofd_create_locks[gr]) != 0);

        last = filter_last_id(ofd, oa->o_seq);
        CWARN("%s: deleting orphan objects from "LPU64" to "LPU64"\n",
              filter_obd(ofd)->obd_name, oa->o_id + 1, last);

        for (oi.oi_id = last; oi.oi_id > oa->o_id; oi.oi_id--) {
                fid_ostid_unpack(&info->fti_fid, &oi, 0);
                rc = filter_destroy_by_fid(env, ofd, &info->fti_fid);
                if (rc && rc != -ENOENT) /* this is pretty fatal... */
                        CEMERG("error destroying precreated id "LPU64": %d\n",
                               oi.oi_id, rc);
                if (!skip_orphan) {
                        filter_last_id_set(ofd, oi.oi_id - 1, oa->o_seq);
                        /* update last_id on disk periodically so that if we
                         * restart * we don't need to re-scan all of the just
                         * deleted objects. */
                        if ((oi.oi_id & 511) == 0)
                                filter_last_id_write(env, ofd, oa->o_seq, 0);
                }
        }
        CDEBUG(D_HA, "%s: after destroy: set last_objids["LPU64"] = "LPU64"\n",
               filter_obd(ofd)->obd_name, oa->o_seq, oa->o_id);
        if (!skip_orphan) {
                rc = filter_last_id_write(env, ofd, oa->o_seq, NULL);
        } else {
                /* don't reuse orphan object, return last used objid */
                oa->o_id = last;
                rc = 0;
        }
        RETURN(rc);
}

int filter_create(struct obd_export *exp, struct obdo *oa,
                  struct lov_stripe_md **ea, struct obd_trans_info *oti)
{
        struct filter_device *ofd = filter_exp(exp);
        struct lu_env *env = oti->oti_env;
        struct filter_thread_info *info;
        int rc = 0, diff;
        ENTRY;

        rc = lu_env_refill(env);
        LASSERT(rc == 0);

        info = filter_info_init(env, exp);
        info->fti_no_need_trans = 1;
        filter_oti2info(info, oti);

        LASSERT(ea == NULL);
        LASSERT(oa->o_seq >= FID_SEQ_OST_MDT0);
        LASSERT(oa->o_valid & OBD_MD_FLGROUP);

        CDEBUG(D_INFO, "filter_create(oa->o_seq="LPU64",oa->o_id="LPU64")\n",
               oa->o_seq, oa->o_id);

        if ((oa->o_valid & OBD_MD_FLFLAGS) &&
            (oa->o_flags & OBD_FL_RECREATE_OBJS)) {
                if (!filter_obd(ofd)->obd_recovering ||
                    oa->o_id > filter_last_id(ofd, oa->o_seq)) {
                        CERROR("recreate objid "LPU64" > last id "LPU64"\n",
                               oa->o_id, filter_last_id(ofd, oa->o_seq));
                        GOTO(out, rc = -EINVAL);
                }
                /* do nothing because we create objects during first write */
                GOTO(out, rc = 0);
        }
        /* former filter_handle_precreate */
        if ((oa->o_valid & OBD_MD_FLFLAGS) &&
                   (oa->o_flags & OBD_FL_DELORPHAN)){
                /* destroy orphans */
                if (oti->oti_conn_cnt < exp->exp_conn_cnt) {
                        CERROR("%s: dropping old orphan cleanup request\n",
                               filter_obd(ofd)->obd_name);
                        GOTO(out, rc = 0);
                }
                /* This causes inflight precreates to abort and drop lock */
                cfs_set_bit(oa->o_seq, &ofd->ofd_destroys_in_progress);
                cfs_mutex_down(&ofd->ofd_create_locks[oa->o_seq]);
                if (!cfs_test_bit(oa->o_seq, &ofd->ofd_destroys_in_progress)) {
                        CERROR("%s:["LPU64"] destroys_in_progress already cleared\n",
                               exp->exp_obd->obd_name, oa->o_seq);
                        GOTO(out, rc = 0);
                }
                diff = oa->o_id - filter_last_id(ofd, oa->o_seq);
                CDEBUG(D_HA, "filter_last_id() = "LPU64" -> diff = %d\n",
                       filter_last_id(ofd, oa->o_seq), diff);
                if (-diff > OST_MAX_PRECREATE) {
                        /* FIXME: should reset precreate_next_id on MDS */
                        rc = 0;
                } else if (diff < 0) {
                        rc = filter_orphans_destroy(env, exp, ofd, oa);
                        cfs_clear_bit(oa->o_seq, &ofd->ofd_destroys_in_progress);
                } else {
                        /* XXX: Used by MDS for the first time! */
                        cfs_clear_bit(oa->o_seq, &ofd->ofd_destroys_in_progress);
                }
        } else {
                cfs_mutex_down(&ofd->ofd_create_locks[oa->o_seq]);
                if (oti->oti_conn_cnt < exp->exp_conn_cnt) {
                        CERROR("%s: dropping old precreate request\n",
                               filter_obd(ofd)->obd_name);
                        GOTO(out, rc = 0);
                }
                /* only precreate if group == 0 and o_id is specfied */
                if (!fid_seq_is_mdt(oa->o_seq) || oa->o_id == 0) {
                        diff = 1; /* shouldn't we create this right now? */
                } else {
                        diff = oa->o_id - filter_last_id(ofd, oa->o_seq);
                }
        }
        if (diff > 0) {
                obd_id next_id = filter_last_id(ofd, oa->o_seq) + 1;
                int i;

                CDEBUG(D_HA,
                       "%s: reserve %d objects in group "LPU64" at "LPU64"\n",
                       filter_obd(ofd)->obd_name, diff, oa->o_seq, next_id - diff);
                for (i = 0; i < diff; i++) {
                        rc = filter_precreate_object(env, ofd, next_id + i,
                                                     oa->o_seq);
                        if (rc)
                                break;
                }
                filter_last_id_write(env, ofd, oa->o_seq, 0);
                if (i > 0) {
                        /* some objects got created, we can return
                         * them, even if last creation failed */
                        oa->o_id = filter_last_id(ofd, oa->o_seq);
                        rc = 0;
                } else {
                        CERROR("unable to precreate: %d\n", rc);
                        oa->o_id = filter_last_id(ofd, oa->o_seq);
                }
                oa->o_valid |= OBD_MD_FLID | OBD_MD_FLGROUP;
        }

        filter_info2oti(info, oti);
out:
        cfs_mutex_up(&ofd->ofd_create_locks[oa->o_seq]);
        return rc;
}

int filter_getattr(struct obd_export *exp, struct obd_info *oinfo)
{
        struct filter_device *ofd = filter_exp(exp);
        struct filter_thread_info *info;
        struct filter_object *fo;
        struct lu_env *env = oinfo->oi_env;
        int rc = 0;
        ENTRY;

        rc = lu_env_refill(env);
        if (rc)
                RETURN(rc);
        info = filter_info_init(env, exp);

        fid_ostid_unpack(&info->fti_fid, &oinfo->oi_oa->o_oi, 0);
        rc = filter_auth_capa(ofd, &info->fti_fid, oinfo->oi_oa->o_seq,
                              oinfo_capa(oinfo), CAPA_OPC_META_READ);
        if (rc)
                GOTO(out, rc);

        fo = filter_object_find(env, ofd, &info->fti_fid);
        if (IS_ERR(fo))
                GOTO(out, rc = PTR_ERR(fo));
        LASSERT(fo != NULL);
        rc = filter_attr_get(env, fo, &info->fti_attr);
        oinfo->oi_oa->o_valid = OBD_MD_FLID;
        if (rc == 0)
                obdo_from_la(oinfo->oi_oa, &info->fti_attr,
                             FILTER_VALID_FLAGS | LA_UID | LA_GID);

        filter_object_put(env, fo);
out:
        RETURN(rc);
}

static int filter_sync(struct obd_export *exp, struct obd_info *oinfo,
                       obd_size start, obd_size end,
                       struct ptlrpc_request_set *set)
{
        struct filter_device *ofd = filter_exp(exp);
        struct lu_env env;
        int rc;

        ENTRY;

        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc)
                RETURN(rc);

        rc = dt_sync(&env, ofd->ofd_osd);
        /* TODO: see filter.c in obdfilter/
        filter_sync_llogs(exp->exp_obd, exp);
        */
        lu_env_fini(&env);
        RETURN(rc);
}

int filter_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                     void *karg, void *uarg)
{
        struct lu_env env;
        struct filter_device *ofd = filter_exp(exp);
        struct obd_device *obd = filter_obd(ofd);
        int rc;

        ENTRY;

        CDEBUG(D_IOCTL, "handling ioctl cmd %#x\n", cmd);
        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc)
                RETURN(rc);

        switch (cmd) {
        case OBD_IOC_ABORT_RECOVERY:
                CERROR("aborting recovery for device %s\n", obd->obd_name);
                target_stop_recovery_thread(obd);
                break;
        case OBD_IOC_SYNC:
                CDEBUG(D_RPCTRACE, "syncing ost %s\n", obd->obd_name);
                rc = dt_sync(&env, ofd->ofd_osd);
                break;
        case OBD_IOC_SET_READONLY:
                rc = dt_sync(&env, ofd->ofd_osd);
                dt_ro(&env, ofd->ofd_osd);
                break;
        default:
                CERROR("Not supported cmd = %d for device %s\n",
                       cmd, obd->obd_name);
                rc = -EOPNOTSUPP;
        }

        lu_env_fini(&env);
        RETURN(rc);
}

static int filter_precleanup(struct obd_device *obd,
                             enum obd_cleanup_stage stage)
{
        int rc = 0;
        ENTRY;

        switch(stage) {
        case OBD_CLEANUP_EARLY:
                break;
        case OBD_CLEANUP_EXPORTS:
                target_cleanup_recovery(obd);
                filter_llog_finish(obd, 0);
                break;
        }
        RETURN(rc);
}

struct obd_ops filter_obd_ops = {
        .o_owner          = THIS_MODULE,
        .o_notify         = filter_obd_notify,
        .o_connect        = filter_obd_connect,
        .o_reconnect      = filter_obd_reconnect,
        .o_disconnect     = filter_obd_disconnect,
        .o_set_info_async = filter_set_info_async,
        .o_get_info       = filter_get_info,
        .o_llog_init      = filter_llog_init,
        .o_llog_finish    = filter_llog_finish,
        .o_create         = filter_create,
        .o_statfs         = filter_statfs,
        .o_setattr        = filter_setattr,
        .o_preprw         = filter_preprw,
        .o_commitrw       = filter_commitrw,
        .o_destroy        = filter_destroy,
        .o_init_export    = filter_init_export,
        .o_destroy_export = filter_destroy_export,
        .o_init_export    = filter_init_export,
        .o_punch          = filter_punch,
        .o_getattr        = filter_getattr,
        .o_sync           = filter_sync,
        .o_iocontrol      = filter_iocontrol,
        .o_precleanup     = filter_precleanup

/*        .o_setup          = filter_setup,
        .o_cleanup        = filter_cleanup,
        .o_ping           = filter_ping,

        .o_llog_connect   = filter_llog_connect,
        .o_health_check   = filter_health_check,
        .o_process_config = filter_process_config,*/
};


