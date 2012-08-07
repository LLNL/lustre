/*
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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2012, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LDLM

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
#else
# include <liblustre.h>
#endif
#include <obd.h>
#include <obd_class.h>
#include <lustre_dlm.h>
#include <lustre_net.h>
#include <lustre_sec.h>
#include "ldlm_internal.h"

/* @priority: if non-zero, move the selected to the list head
 * @create: if zero, only search in existed connections
 */
static int import_set_conn(struct obd_import *imp, struct obd_uuid *uuid,
                           int priority, int create)
{
        struct ptlrpc_connection *ptlrpc_conn;
        struct obd_import_conn *imp_conn = NULL, *item;
        int rc = 0;
        ENTRY;

        if (!create && !priority) {
                CDEBUG(D_HA, "Nothing to do\n");
                RETURN(-EINVAL);
        }

        ptlrpc_conn = ptlrpc_uuid_to_connection(uuid);
        if (!ptlrpc_conn) {
                CDEBUG(D_HA, "can't find connection %s\n", uuid->uuid);
                RETURN (-ENOENT);
        }

        if (create) {
                OBD_ALLOC(imp_conn, sizeof(*imp_conn));
                if (!imp_conn) {
                        GOTO(out_put, rc = -ENOMEM);
                }
        }

        cfs_spin_lock(&imp->imp_lock);
        cfs_list_for_each_entry(item, &imp->imp_conn_list, oic_item) {
                if (obd_uuid_equals(uuid, &item->oic_uuid)) {
                        if (priority) {
                                cfs_list_del(&item->oic_item);
                                cfs_list_add(&item->oic_item,
                                             &imp->imp_conn_list);
                                item->oic_last_attempt = 0;
                        }
                        CDEBUG(D_HA, "imp %p@%s: found existing conn %s%s\n",
                               imp, imp->imp_obd->obd_name, uuid->uuid,
                               (priority ? ", moved to head" : ""));
                        cfs_spin_unlock(&imp->imp_lock);
                        GOTO(out_free, rc = 0);
                }
        }
        /* not found */
        if (create) {
                imp_conn->oic_conn = ptlrpc_conn;
                imp_conn->oic_uuid = *uuid;
                imp_conn->oic_last_attempt = 0;
                if (priority)
                        cfs_list_add(&imp_conn->oic_item, &imp->imp_conn_list);
                else
                        cfs_list_add_tail(&imp_conn->oic_item,
                                          &imp->imp_conn_list);
                CDEBUG(D_HA, "imp %p@%s: add connection %s at %s\n",
                       imp, imp->imp_obd->obd_name, uuid->uuid,
                       (priority ? "head" : "tail"));
        } else {
                cfs_spin_unlock(&imp->imp_lock);
                GOTO(out_free, rc = -ENOENT);
        }

        cfs_spin_unlock(&imp->imp_lock);
        RETURN(0);
out_free:
        if (imp_conn)
                OBD_FREE(imp_conn, sizeof(*imp_conn));
out_put:
        ptlrpc_connection_put(ptlrpc_conn);
        RETURN(rc);
}

int import_set_conn_priority(struct obd_import *imp, struct obd_uuid *uuid)
{
        return import_set_conn(imp, uuid, 1, 0);
}

int client_import_add_conn(struct obd_import *imp, struct obd_uuid *uuid,
                           int priority)
{
        return import_set_conn(imp, uuid, priority, 1);
}

int client_import_del_conn(struct obd_import *imp, struct obd_uuid *uuid)
{
        struct obd_import_conn *imp_conn;
        struct obd_export *dlmexp;
        int rc = -ENOENT;
        ENTRY;

        cfs_spin_lock(&imp->imp_lock);
        if (cfs_list_empty(&imp->imp_conn_list)) {
                LASSERT(!imp->imp_connection);
                GOTO(out, rc);
        }

        cfs_list_for_each_entry(imp_conn, &imp->imp_conn_list, oic_item) {
                if (!obd_uuid_equals(uuid, &imp_conn->oic_uuid))
                        continue;
                LASSERT(imp_conn->oic_conn);

                /* is current conn? */
                if (imp_conn == imp->imp_conn_current) {
                        LASSERT(imp_conn->oic_conn == imp->imp_connection);

                        if (imp->imp_state != LUSTRE_IMP_CLOSED &&
                            imp->imp_state != LUSTRE_IMP_DISCON) {
                                CERROR("can't remove current connection\n");
                                GOTO(out, rc = -EBUSY);
                        }

                        ptlrpc_connection_put(imp->imp_connection);
                        imp->imp_connection = NULL;

                        dlmexp = class_conn2export(&imp->imp_dlm_handle);
                        if (dlmexp && dlmexp->exp_connection) {
                                LASSERT(dlmexp->exp_connection ==
                                        imp_conn->oic_conn);
                                ptlrpc_connection_put(dlmexp->exp_connection);
                                dlmexp->exp_connection = NULL;
                        }
                }

                cfs_list_del(&imp_conn->oic_item);
                ptlrpc_connection_put(imp_conn->oic_conn);
                OBD_FREE(imp_conn, sizeof(*imp_conn));
                CDEBUG(D_HA, "imp %p@%s: remove connection %s\n",
                       imp, imp->imp_obd->obd_name, uuid->uuid);
                rc = 0;
                break;
        }
out:
        cfs_spin_unlock(&imp->imp_lock);
        if (rc == -ENOENT)
                CERROR("connection %s not found\n", uuid->uuid);
        RETURN(rc);
}

/**
 * Find conn uuid by peer nid. @peer is a server nid. This function is used
 * to find a conn uuid of @imp which can reach @peer.
 */
int client_import_find_conn(struct obd_import *imp, lnet_nid_t peer,
                            struct obd_uuid *uuid)
{
        struct obd_import_conn *conn;
        int rc = -ENOENT;
        ENTRY;

        cfs_spin_lock(&imp->imp_lock);
        cfs_list_for_each_entry(conn, &imp->imp_conn_list, oic_item) {
                /* check if conn uuid does have this peer nid */
                if (class_check_uuid(&conn->oic_uuid, peer)) {
                        *uuid = conn->oic_uuid;
                        rc = 0;
                        break;
                }
        }
        cfs_spin_unlock(&imp->imp_lock);
        RETURN(rc);
}
EXPORT_SYMBOL(client_import_find_conn);

void client_destroy_import(struct obd_import *imp)
{
        /* drop security policy instance after all rpc finished/aborted
         * to let all busy contexts be released. */
        class_import_get(imp);
        class_destroy_import(imp);
        sptlrpc_import_sec_put(imp);
        class_import_put(imp);
}
EXPORT_SYMBOL(client_destroy_import);

/* configure an RPC client OBD device
 *
 * lcfg parameters:
 * 1 - client UUID
 * 2 - server UUID
 * 3 - inactive-on-startup
 */
int client_obd_setup(struct obd_device *obddev, struct lustre_cfg *lcfg)
{
        struct client_obd *cli = &obddev->u.cli;
        struct obd_import *imp;
        struct obd_uuid server_uuid;
        int rq_portal, rp_portal, connect_op;
        char *name = obddev->obd_type->typ_name;
        ldlm_ns_type_t ns_type = LDLM_NS_TYPE_UNKNOWN;
        int rc;
        ENTRY;

        /* In a more perfect world, we would hang a ptlrpc_client off of
         * obd_type and just use the values from there. */
        if (!strcmp(name, LUSTRE_OSC_NAME)) {
                rq_portal = OST_REQUEST_PORTAL;
                rp_portal = OSC_REPLY_PORTAL;
                connect_op = OST_CONNECT;
                cli->cl_sp_me = LUSTRE_SP_CLI;
                cli->cl_sp_to = LUSTRE_SP_OST;
                ns_type = LDLM_NS_TYPE_OSC;

        } else if (!strcmp(name, LUSTRE_MDC_NAME)) {
                rq_portal = MDS_REQUEST_PORTAL;
                rp_portal = MDC_REPLY_PORTAL;
                connect_op = MDS_CONNECT;
                cli->cl_sp_me = LUSTRE_SP_CLI;
                cli->cl_sp_to = LUSTRE_SP_MDT;
                ns_type = LDLM_NS_TYPE_MDC;

        } else if (!strcmp(name, LUSTRE_MGC_NAME)) {
                rq_portal = MGS_REQUEST_PORTAL;
                rp_portal = MGC_REPLY_PORTAL;
                connect_op = MGS_CONNECT;
                cli->cl_sp_me = LUSTRE_SP_MGC;
                cli->cl_sp_to = LUSTRE_SP_MGS;
                cli->cl_flvr_mgc.sf_rpc = SPTLRPC_FLVR_INVALID;
                ns_type = LDLM_NS_TYPE_MGC;
        } else if (!strcmp(name, LUSTRE_OSP_NAME)) {
                rq_portal = OST_REQUEST_PORTAL;
                rp_portal = OSC_REPLY_PORTAL;
                connect_op = OST_CONNECT;
                cli->cl_sp_me = LUSTRE_SP_CLI;
                cli->cl_sp_to = LUSTRE_SP_OST;
                ns_type = LDLM_NS_TYPE_OSC;
        } else {
                CERROR("unknown client OBD type \"%s\", can't setup\n",
                       name);
                RETURN(-EINVAL);
        }

        if (LUSTRE_CFG_BUFLEN(lcfg, 1) < 1) {
                CERROR("requires a TARGET UUID\n");
                RETURN(-EINVAL);
        }

        if (LUSTRE_CFG_BUFLEN(lcfg, 1) > 37) {
                CERROR("client UUID must be less than 38 characters\n");
                RETURN(-EINVAL);
        }

        if (LUSTRE_CFG_BUFLEN(lcfg, 2) < 1) {
                CERROR("setup requires a SERVER UUID\n");
                RETURN(-EINVAL);
        }

        if (LUSTRE_CFG_BUFLEN(lcfg, 2) > 37) {
                CERROR("target UUID must be less than 38 characters\n");
                RETURN(-EINVAL);
        }

        cfs_init_rwsem(&cli->cl_sem);
        cfs_sema_init(&cli->cl_mgc_sem, 1);
        cli->cl_conn_count = 0;
        memcpy(server_uuid.uuid, lustre_cfg_buf(lcfg, 2),
               min_t(unsigned int, LUSTRE_CFG_BUFLEN(lcfg, 2),
                     sizeof(server_uuid)));

        cli->cl_dirty = 0;
        cli->cl_avail_grant = 0;
        /* FIXME: should limit this for the sum of all cl_dirty_max */
        cli->cl_dirty_max = OSC_MAX_DIRTY_DEFAULT * 1024 * 1024;
        if (cli->cl_dirty_max >> CFS_PAGE_SHIFT > cfs_num_physpages / 8)
                cli->cl_dirty_max = cfs_num_physpages << (CFS_PAGE_SHIFT - 3);
        CFS_INIT_LIST_HEAD(&cli->cl_cache_waiters);
        CFS_INIT_LIST_HEAD(&cli->cl_loi_ready_list);
        CFS_INIT_LIST_HEAD(&cli->cl_loi_hp_ready_list);
        CFS_INIT_LIST_HEAD(&cli->cl_loi_write_list);
        CFS_INIT_LIST_HEAD(&cli->cl_loi_read_list);
        client_obd_list_lock_init(&cli->cl_loi_list_lock);
	cfs_atomic_set(&cli->cl_pending_w_pages, 0);
	cfs_atomic_set(&cli->cl_pending_r_pages, 0);
        cli->cl_r_in_flight = 0;
        cli->cl_w_in_flight = 0;

        cfs_spin_lock_init(&cli->cl_read_rpc_hist.oh_lock);
        cfs_spin_lock_init(&cli->cl_write_rpc_hist.oh_lock);
        cfs_spin_lock_init(&cli->cl_read_page_hist.oh_lock);
        cfs_spin_lock_init(&cli->cl_write_page_hist.oh_lock);
        cfs_spin_lock_init(&cli->cl_read_offset_hist.oh_lock);
        cfs_spin_lock_init(&cli->cl_write_offset_hist.oh_lock);

	/* lru for osc. */
	CFS_INIT_LIST_HEAD(&cli->cl_lru_osc);
	cfs_atomic_set(&cli->cl_lru_waiters, 0);
	cfs_atomic_set(&cli->cl_lru_shrinkers, 0);
	cfs_atomic_set(&cli->cl_lru_busy, 0);
	cfs_atomic_set(&cli->cl_lru_in_list, 0);
	CFS_INIT_LIST_HEAD(&cli->cl_lru_list);
	client_obd_list_lock_init(&cli->cl_lru_list_lock);

        cfs_waitq_init(&cli->cl_destroy_waitq);
        cfs_atomic_set(&cli->cl_destroy_in_flight, 0);
#ifdef ENABLE_CHECKSUM
        /* Turn on checksumming by default. */
        cli->cl_checksum = 1;
        /*
         * The supported checksum types will be worked out at connect time
         * Set cl_chksum* to CRC32 for now to avoid returning screwed info
         * through procfs.
         */
        cli->cl_cksum_type = cli->cl_supp_cksum_types = OBD_CKSUM_CRC32;
#endif
        cfs_atomic_set(&cli->cl_resends, OSC_DEFAULT_RESENDS);

        /* This value may be changed at connect time in
           ptlrpc_connect_interpret. */
        cli->cl_max_pages_per_rpc = min((int)PTLRPC_MAX_BRW_PAGES,
                                        (int)(1024 * 1024 >> CFS_PAGE_SHIFT));

        if (!strcmp(name, LUSTRE_MDC_NAME)) {
                cli->cl_max_rpcs_in_flight = MDC_MAX_RIF_DEFAULT;
        } else if (cfs_num_physpages >> (20 - CFS_PAGE_SHIFT) <= 128 /* MB */) {
                cli->cl_max_rpcs_in_flight = 2;
        } else if (cfs_num_physpages >> (20 - CFS_PAGE_SHIFT) <= 256 /* MB */) {
                cli->cl_max_rpcs_in_flight = 3;
        } else if (cfs_num_physpages >> (20 - CFS_PAGE_SHIFT) <= 512 /* MB */) {
                cli->cl_max_rpcs_in_flight = 4;
        } else {
                cli->cl_max_rpcs_in_flight = OSC_MAX_RIF_DEFAULT;
        }

        rc = ldlm_get_ref();
        if (rc) {
                CERROR("ldlm_get_ref failed: %d\n", rc);
                GOTO(err, rc);
        }

        ptlrpc_init_client(rq_portal, rp_portal, name,
                           &obddev->obd_ldlm_client);

        imp = class_new_import(obddev);
        if (imp == NULL)
                GOTO(err_ldlm, rc = -ENOENT);
        imp->imp_client = &obddev->obd_ldlm_client;
        imp->imp_connect_op = connect_op;
        CFS_INIT_LIST_HEAD(&imp->imp_pinger_chain);
        memcpy(cli->cl_target_uuid.uuid, lustre_cfg_buf(lcfg, 1),
               LUSTRE_CFG_BUFLEN(lcfg, 1));
        class_import_put(imp);

        rc = client_import_add_conn(imp, &server_uuid, 1);
        if (rc) {
                CERROR("can't add initial connection\n");
                GOTO(err_import, rc);
        }

        cli->cl_import = imp;
        /* cli->cl_max_mds_{easize,cookiesize} updated by mdc_init_ea_size()
         * use single striped v3 LOV EA to start with */
        cli->cl_max_mds_easize = sizeof(struct lov_mds_md_v3) +
                                 sizeof(struct lov_ost_data);
        cli->cl_max_mds_cookiesize = sizeof(struct llog_cookie);

        if (LUSTRE_CFG_BUFLEN(lcfg, 3) > 0) {
                if (!strcmp(lustre_cfg_string(lcfg, 3), "inactive")) {
                        CDEBUG(D_HA, "marking %s %s->%s as inactive\n",
                               name, obddev->obd_name,
                               cli->cl_target_uuid.uuid);
                        cfs_spin_lock(&imp->imp_lock);
                        imp->imp_deactive = 1;
                        cfs_spin_unlock(&imp->imp_lock);
                }
        }

        obddev->obd_namespace = ldlm_namespace_new(obddev, obddev->obd_name,
                                                   LDLM_NAMESPACE_CLIENT,
                                                   LDLM_NAMESPACE_GREEDY,
                                                   ns_type);
        if (obddev->obd_namespace == NULL) {
                CERROR("Unable to create client namespace - %s\n",
                       obddev->obd_name);
                GOTO(err_import, rc = -ENOMEM);
        }

        cli->cl_qchk_stat = CL_NOT_QUOTACHECKED;

        RETURN(rc);

err_import:
        class_destroy_import(imp);
err_ldlm:
        ldlm_put_ref();
err:
        RETURN(rc);

}

int client_obd_cleanup(struct obd_device *obddev)
{
        ENTRY;

        ldlm_namespace_free_post(obddev->obd_namespace);
        obddev->obd_namespace = NULL;

        LASSERT(obddev->u.cli.cl_import == NULL);

        ldlm_put_ref();
        RETURN(0);
}

/* ->o_connect() method for client side (OSC and MDC and MGC) */
int client_connect_import(const struct lu_env *env,
                          struct obd_export **exp,
                          struct obd_device *obd, struct obd_uuid *cluuid,
                          struct obd_connect_data *data, void *localdata)
{
        struct client_obd *cli = &obd->u.cli;
        struct obd_import *imp = cli->cl_import;
        struct obd_connect_data *ocd;
        struct lustre_handle conn = { 0 };
        int rc;
        ENTRY;

        *exp = NULL;
        cfs_down_write(&cli->cl_sem);
        if (cli->cl_conn_count > 0 )
                GOTO(out_sem, rc = -EALREADY);

        rc = class_connect(&conn, obd, cluuid);
        if (rc)
                GOTO(out_sem, rc);

        cli->cl_conn_count++;
        *exp = class_conn2export(&conn);

        LASSERT(obd->obd_namespace);

        imp->imp_dlm_handle = conn;
        rc = ptlrpc_init_import(imp);
        if (rc != 0)
                GOTO(out_ldlm, rc);

        ocd = &imp->imp_connect_data;
        if (data) {
                *ocd = *data;
                imp->imp_connect_flags_orig = data->ocd_connect_flags;
        }

        rc = ptlrpc_connect_import(imp);
        if (rc != 0) {
                LASSERT (imp->imp_state == LUSTRE_IMP_DISCON);
                GOTO(out_ldlm, rc);
        }
        LASSERT((*exp)->exp_connection);

        if (data) {
                LASSERTF((ocd->ocd_connect_flags & data->ocd_connect_flags) ==
                         ocd->ocd_connect_flags, "old "LPX64", new "LPX64"\n",
                         data->ocd_connect_flags, ocd->ocd_connect_flags);
                data->ocd_connect_flags = ocd->ocd_connect_flags;
        }

        ptlrpc_pinger_add_import(imp);

        EXIT;

        if (rc) {
out_ldlm:
                cli->cl_conn_count--;
                class_disconnect(*exp);
                *exp = NULL;
        }
out_sem:
        cfs_up_write(&cli->cl_sem);

        return rc;
}

int client_disconnect_export(struct obd_export *exp)
{
        struct obd_device *obd = class_exp2obd(exp);
        struct client_obd *cli;
        struct obd_import *imp;
        int rc = 0, err;
        ENTRY;

        if (!obd) {
                CERROR("invalid export for disconnect: exp %p cookie "LPX64"\n",
                       exp, exp ? exp->exp_handle.h_cookie : -1);
                RETURN(-EINVAL);
        }

        cli = &obd->u.cli;
        imp = cli->cl_import;

        cfs_down_write(&cli->cl_sem);
        CDEBUG(D_INFO, "disconnect %s - %d\n", obd->obd_name,
               cli->cl_conn_count);

        if (!cli->cl_conn_count) {
                CERROR("disconnecting disconnected device (%s)\n",
                       obd->obd_name);
                GOTO(out_disconnect, rc = -EINVAL);
        }

        cli->cl_conn_count--;
        if (cli->cl_conn_count)
                GOTO(out_disconnect, rc = 0);

        /* Mark import deactivated now, so we don't try to reconnect if any
         * of the cleanup RPCs fails (e.g. ldlm cancel, etc).  We don't
         * fully deactivate the import, or that would drop all requests. */
        cfs_spin_lock(&imp->imp_lock);
        imp->imp_deactive = 1;
        cfs_spin_unlock(&imp->imp_lock);

        /* Some non-replayable imports (MDS's OSCs) are pinged, so just
         * delete it regardless.  (It's safe to delete an import that was
         * never added.) */
        (void)ptlrpc_pinger_del_import(imp);

        if (obd->obd_namespace != NULL) {
                /* obd_force == local only */
                ldlm_cli_cancel_unused(obd->obd_namespace, NULL,
                                       obd->obd_force ? LCF_LOCAL : 0, NULL);
                ldlm_namespace_free_prior(obd->obd_namespace, imp, obd->obd_force);
        }

        /*
         * there's no need to hold sem during disconnecting an import,
         * and actually it may cause deadlock in gss.
         */
        cfs_up_write(&cli->cl_sem);
        rc = ptlrpc_disconnect_import(imp, 0);
        cfs_down_write(&cli->cl_sem);

        ptlrpc_invalidate_import(imp);

        EXIT;

 out_disconnect:
        /* use server style - class_disconnect should be always called for
         * o_disconnect */
        err = class_disconnect(exp);
        if (!rc && err)
                rc = err;

        cfs_up_write(&cli->cl_sem);

        RETURN(rc);
}

#ifdef HAVE_SERVER_SUPPORT
int server_disconnect_export(struct obd_export *exp)
{
        int rc;
        ENTRY;

        /* Disconnect early so that clients can't keep using export */
        rc = class_disconnect(exp);
        /* close import for avoid sending any requests */
        if (exp->exp_imp_reverse)
                ptlrpc_cleanup_imp(exp->exp_imp_reverse);

        if (exp->exp_obd->obd_namespace != NULL)
                ldlm_cancel_locks_for_export(exp);

        /* complete all outstanding replies */
        cfs_spin_lock(&exp->exp_lock);
        while (!cfs_list_empty(&exp->exp_outstanding_replies)) {
                struct ptlrpc_reply_state *rs =
                        cfs_list_entry(exp->exp_outstanding_replies.next,
                                       struct ptlrpc_reply_state, rs_exp_list);
                struct ptlrpc_service *svc = rs->rs_service;

                cfs_spin_lock(&svc->srv_rs_lock);
                cfs_list_del_init(&rs->rs_exp_list);
                cfs_spin_lock(&rs->rs_lock);
                ptlrpc_schedule_difficult_reply(rs);
                cfs_spin_unlock(&rs->rs_lock);
                cfs_spin_unlock(&svc->srv_rs_lock);
        }
        cfs_spin_unlock(&exp->exp_lock);

        RETURN(rc);
}

/* --------------------------------------------------------------------------
 * from old lib/target.c
 * -------------------------------------------------------------------------- */

static int target_handle_reconnect(struct lustre_handle *conn,
                                   struct obd_export *exp,
                                   struct obd_uuid *cluuid)
{
        ENTRY;

        if (exp->exp_connection && exp->exp_imp_reverse) {
                struct lustre_handle *hdl;
                struct obd_device *target;

                hdl = &exp->exp_imp_reverse->imp_remote_handle;
                target = exp->exp_obd;

                /* Might be a re-connect after a partition. */
                if (!memcmp(&conn->cookie, &hdl->cookie, sizeof conn->cookie)) {
                        if (target->obd_recovering) {
                                int timeout = cfs_duration_sec(cfs_time_sub(
                                        cfs_timer_deadline(
                                        &target->obd_recovery_timer),
                                        cfs_time_current()));

                                LCONSOLE_WARN("%s: Client %s (at %s) reconnect"
                                        "ing, waiting for %d clients in recov"
                                        "ery for %d:%.02d\n", target->obd_name,
                                        obd_uuid2str(&exp->exp_client_uuid),
                                        obd_export_nid2str(exp),
                                        target->obd_max_recoverable_clients,
                                        timeout / 60, timeout % 60);
                        } else {
                                LCONSOLE_WARN("%s: Client %s (at %s) "
                                        "reconnecting\n", target->obd_name,
                                        obd_uuid2str(&exp->exp_client_uuid),
                                        obd_export_nid2str(exp));
                        }

                        conn->cookie = exp->exp_handle.h_cookie;
                        /* target_handle_connect() treats EALREADY and
                         * -EALREADY differently.  EALREADY means we are
                         * doing a valid reconnect from the same client. */
                        RETURN(EALREADY);
                } else {
                        LCONSOLE_WARN("%s: The server has already connected "
                                      "client %s (at %s) with handle " LPX64
                                      ", rejecting a client with the same "
                                      "uuid trying to reconnect with "
                                      "handle " LPX64, target->obd_name,
                                      obd_uuid2str(&exp->exp_client_uuid),
                                      obd_export_nid2str(exp),
                                      hdl->cookie, conn->cookie);
                        memset(conn, 0, sizeof *conn);
                        /* target_handle_connect() treats EALREADY and
                         * -EALREADY differently.  -EALREADY is an error
                         * (same UUID, different handle). */
                        RETURN(-EALREADY);
                }
        }

        conn->cookie = exp->exp_handle.h_cookie;
        CDEBUG(D_HA, "connect export for UUID '%s' at %p, cookie "LPX64"\n",
               cluuid->uuid, exp, conn->cookie);
        RETURN(0);
}

#ifdef __KERNEL__
static void
check_and_start_recovery_timer(struct obd_device *obd,
                               struct ptlrpc_request *req, int new_client);
#else
static inline void
check_and_start_recovery_timer(struct obd_device *obd,
                               struct ptlrpc_request *req, int new_client)
{
}
#endif

int target_handle_connect(struct ptlrpc_request *req)
{
	struct obd_device *target = NULL, *targref = NULL;
        struct obd_export *export = NULL;
        struct obd_import *revimp;
        struct lustre_handle conn;
        struct lustre_handle *tmp;
        struct obd_uuid tgtuuid;
        struct obd_uuid cluuid;
        struct obd_uuid remote_uuid;
        char *str;
        int rc = 0;
        char *target_start;
        int target_len;
        int mds_conn = 0;
        struct obd_connect_data *data, *tmpdata;
        int size, tmpsize;
        lnet_nid_t *client_nid = NULL;
        ENTRY;

        OBD_RACE(OBD_FAIL_TGT_CONN_RACE);

        str = req_capsule_client_get(&req->rq_pill, &RMF_TGTUUID);
        if (str == NULL) {
                DEBUG_REQ(D_ERROR, req, "bad target UUID for connect");
                GOTO(out, rc = -EINVAL);
        }

        obd_str2uuid(&tgtuuid, str);
        target = class_uuid2obd(&tgtuuid);
        if (!target)
                target = class_name2obd(str);

	if (!target) {
		deuuidify(str, NULL, &target_start, &target_len);
		LCONSOLE_ERROR_MSG(0x137, "UUID '%s' is not available for "
				   "connect (no target)\n", str);
		GOTO(out, rc = -ENODEV);
	}

	cfs_spin_lock(&target->obd_dev_lock);
	if (target->obd_stopping || !target->obd_set_up) {
		cfs_spin_unlock(&target->obd_dev_lock);

		deuuidify(str, NULL, &target_start, &target_len);
		LCONSOLE_ERROR_MSG(0x137, "%.*s: Not available for connect "
				   "from %s (%s)\n", target_len, target_start,
				   libcfs_nid2str(req->rq_peer.nid), 
				   (target->obd_stopping ?
				   "stopping" : "not set up"));
		GOTO(out, rc = -ENODEV);
	}

        if (target->obd_no_conn) {
		cfs_spin_unlock(&target->obd_dev_lock);

                LCONSOLE_WARN("%s: Temporarily refusing client connection "
                              "from %s\n", target->obd_name,
                              libcfs_nid2str(req->rq_peer.nid));
                GOTO(out, rc = -EAGAIN);
        }

        /* Make sure the target isn't cleaned up while we're here. Yes,
           there's still a race between the above check and our incref here.
           Really, class_uuid2obd should take the ref. */
        targref = class_incref(target, __FUNCTION__, cfs_current());

	target->obd_conn_inprogress++;
	cfs_spin_unlock(&target->obd_dev_lock);

        str = req_capsule_client_get(&req->rq_pill, &RMF_CLUUID);
        if (str == NULL) {
                DEBUG_REQ(D_ERROR, req, "bad client UUID for connect");
                GOTO(out, rc = -EINVAL);
        }

        obd_str2uuid(&cluuid, str);

        /* XXX extract a nettype and format accordingly */
        switch (sizeof(lnet_nid_t)) {
                /* NB the casts only avoid compiler warnings */
        case 8:
                snprintf(remote_uuid.uuid, sizeof remote_uuid,
                         "NET_"LPX64"_UUID", (__u64)req->rq_peer.nid);
                break;
        case 4:
                snprintf(remote_uuid.uuid, sizeof remote_uuid,
                         "NET_%x_UUID", (__u32)req->rq_peer.nid);
                break;
        default:
                LBUG();
        }

        tmp = req_capsule_client_get(&req->rq_pill, &RMF_CONN);
        if (tmp == NULL)
                GOTO(out, rc = -EPROTO);

        conn = *tmp;

        size = req_capsule_get_size(&req->rq_pill, &RMF_CONNECT_DATA,
                                    RCL_CLIENT);
        data = req_capsule_client_get(&req->rq_pill, &RMF_CONNECT_DATA);
        if (!data)
                GOTO(out, rc = -EPROTO);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                GOTO(out, rc);

        if (lustre_msg_get_op_flags(req->rq_reqmsg) & MSG_CONNECT_LIBCLIENT) {
                if (!data) {
                        DEBUG_REQ(D_WARNING, req, "Refusing old (unversioned) "
                                  "libclient connection attempt");
                        GOTO(out, rc = -EPROTO);
                } else if (data->ocd_version < LUSTRE_VERSION_CODE -
                                               LUSTRE_VERSION_ALLOWED_OFFSET ||
                           data->ocd_version > LUSTRE_VERSION_CODE +
                                               LUSTRE_VERSION_ALLOWED_OFFSET) {
                        DEBUG_REQ(D_WARNING, req, "Refusing %s (%d.%d.%d.%d) "
                                  "libclient connection attempt",
                                  data->ocd_version < LUSTRE_VERSION_CODE ?
                                  "old" : "new",
                                  OBD_OCD_VERSION_MAJOR(data->ocd_version),
                                  OBD_OCD_VERSION_MINOR(data->ocd_version),
                                  OBD_OCD_VERSION_PATCH(data->ocd_version),
                                  OBD_OCD_VERSION_FIX(data->ocd_version));
                        data = req_capsule_server_sized_get(&req->rq_pill,
                                                        &RMF_CONNECT_DATA,
                                    offsetof(typeof(*data), ocd_version) +
                                             sizeof(data->ocd_version));
                        if (data) {
                                data->ocd_connect_flags = OBD_CONNECT_VERSION;
                                data->ocd_version = LUSTRE_VERSION_CODE;
                        }
                        GOTO(out, rc = -EPROTO);
                }
        }

        if ((lustre_msg_get_op_flags(req->rq_reqmsg) & MSG_CONNECT_INITIAL) &&
            (data->ocd_connect_flags & OBD_CONNECT_MDS))
                mds_conn = 1;

        /* lctl gets a backstage, all-access pass. */
        if (obd_uuid_equals(&cluuid, &target->obd_uuid))
                goto dont_check_exports;

        export = cfs_hash_lookup(target->obd_uuid_hash, &cluuid);
        if (!export)
                goto no_export;

        /* we've found an export in the hash */
        if (export->exp_connecting) { /* bug 9635, et. al. */
                LCONSOLE_WARN("%s: Export %p already connecting from %s\n",
                              export->exp_obd->obd_name, export,
                              libcfs_nid2str(req->rq_peer.nid));
                class_export_put(export);
                export = NULL;
                rc = -EALREADY;
        } else if (mds_conn && export->exp_connection) {
                if (req->rq_peer.nid != export->exp_connection->c_peer.nid)
                        /* mds reconnected after failover */
                        LCONSOLE_WARN("%s: Received MDS connection from "
                            "%s, removing former export from %s\n",
                            target->obd_name, libcfs_nid2str(req->rq_peer.nid),
                            libcfs_nid2str(export->exp_connection->c_peer.nid));
                else
                        /* new mds connection from the same nid */
                        LCONSOLE_WARN("%s: Received new MDS connection from "
                            "%s, removing former export from same NID\n",
                            target->obd_name, libcfs_nid2str(req->rq_peer.nid));
                class_fail_export(export);
                class_export_put(export);
                export = NULL;
                rc = 0;
        } else if (export->exp_connection != NULL &&
                   req->rq_peer.nid != export->exp_connection->c_peer.nid &&
                   (lustre_msg_get_op_flags(req->rq_reqmsg) &
                    MSG_CONNECT_INITIAL)) {
                /* in mds failover we have static uuid but nid can be
                 * changed*/
                LCONSOLE_WARN("%s: Client %s seen on new nid %s when "
                              "existing nid %s is already connected\n",
                              target->obd_name, cluuid.uuid,
                              libcfs_nid2str(req->rq_peer.nid),
                              libcfs_nid2str(
                                      export->exp_connection->c_peer.nid));
                rc = -EALREADY;
                class_export_put(export);
                export = NULL;
        } else {
                cfs_spin_lock(&export->exp_lock);

		if (export->exp_connecting) { /* ORI-701 */
			cfs_spin_unlock(&export->exp_lock);
			LCONSOLE_WARN("%s: Export %p already connecting from %s\n",
					export->exp_obd->obd_name, export,
					libcfs_nid2str(req->rq_peer.nid));
			class_export_put(export);
			export = NULL;
			rc = -EALREADY;
		} else {
			export->exp_connecting = 1;
			cfs_spin_unlock(&export->exp_lock);
			LASSERT(export->exp_obd == target);

			rc = target_handle_reconnect(&conn, export, &cluuid);
		}
        }

        /* If we found an export, we already unlocked. */
        if (!export) {
no_export:
                OBD_FAIL_TIMEOUT(OBD_FAIL_TGT_DELAY_CONNECT, 2 * obd_timeout);
        } else if (req->rq_export == NULL &&
                   cfs_atomic_read(&export->exp_rpc_count) > 0) {
                LCONSOLE_WARN("%s: Client %s (at %s) refused connection, "
                              "still busy with %d references\n",
                              target->obd_name, cluuid.uuid,
                              libcfs_nid2str(req->rq_peer.nid),
                              cfs_atomic_read(&export->exp_refcount));
                GOTO(out, rc = -EBUSY);
        } else if (req->rq_export != NULL &&
                   (cfs_atomic_read(&export->exp_rpc_count) > 1)) {
                /* the current connect rpc has increased exp_rpc_count */
                LCONSOLE_WARN("%s: Client %s (at %s) refused reconnection, "
                              "still busy with %d active RPCs\n",
                              target->obd_name, cluuid.uuid,
                              libcfs_nid2str(req->rq_peer.nid),
                              cfs_atomic_read(&export->exp_rpc_count) - 1);
                cfs_spin_lock(&export->exp_lock);
                if (req->rq_export->exp_conn_cnt <
                    lustre_msg_get_conn_cnt(req->rq_reqmsg))
                        /* try to abort active requests */
                        req->rq_export->exp_abort_active_req = 1;
                cfs_spin_unlock(&export->exp_lock);
                GOTO(out, rc = -EBUSY);
        } else if (lustre_msg_get_conn_cnt(req->rq_reqmsg) == 1) {
                if (!strstr(cluuid.uuid, "mdt"))
                        LCONSOLE_WARN("%s: Rejecting reconnect from the "
                                      "known client %s (at %s) because it "
                                      "is indicating it is a new client",
                                      target->obd_name, cluuid.uuid,
                                      libcfs_nid2str(req->rq_peer.nid));
                GOTO(out, rc = -EALREADY);
        } else {
                OBD_FAIL_TIMEOUT(OBD_FAIL_TGT_DELAY_RECONNECT, 2 * obd_timeout);
        }

        if (rc < 0) {
                GOTO(out, rc);
        }

        CDEBUG(D_HA, "%s: connection from %s@%s %st"LPU64" exp %p cur %ld last %ld\n",
               target->obd_name, cluuid.uuid, libcfs_nid2str(req->rq_peer.nid),
              target->obd_recovering ? "recovering/" : "", data->ocd_transno,
              export, (long)cfs_time_current_sec(),
              export ? (long)export->exp_last_request_time : 0);

        /* If this is the first time a client connects, reset the recovery
         * timer */
        if (rc == 0 && target->obd_recovering)
                check_and_start_recovery_timer(target, req, export == NULL);

        /* We want to handle EALREADY but *not* -EALREADY from
         * target_handle_reconnect(), return reconnection state in a flag */
        if (rc == EALREADY) {
                lustre_msg_add_op_flags(req->rq_repmsg, MSG_CONNECT_RECONNECT);
                rc = 0;
        } else {
                LASSERT(rc == 0);
        }

        /* Tell the client if we support replayable requests */
        if (target->obd_replayable)
                lustre_msg_add_op_flags(req->rq_repmsg, MSG_CONNECT_REPLAYABLE);
        client_nid = &req->rq_peer.nid;

        if (export == NULL) {
                if (target->obd_recovering) {
                        cfs_time_t t;

                        t = cfs_timer_deadline(&target->obd_recovery_timer);
                        t = cfs_time_sub(t, cfs_time_current());
                        t = cfs_duration_sec(t);
                        LCONSOLE_WARN("%s: Denying connection for new client "
                                      "%s (at %s), waiting for %d clients in "
                                      "recovery for %d:%.02d\n",
                                      target->obd_name,
                                      libcfs_nid2str(req->rq_peer.nid),
                                      cluuid.uuid,
                                      cfs_atomic_read(&target-> \
                                                      obd_lock_replay_clients),
                                      (int)t / 60, (int)t % 60);
                        rc = -EBUSY;
                } else {
dont_check_exports:
                        rc = obd_connect(req->rq_svc_thread->t_env,
                                         &export, target, &cluuid, data,
                                         client_nid);
                        if (rc == 0)
                                conn.cookie = export->exp_handle.h_cookie;
                }
        } else {
                rc = obd_reconnect(req->rq_svc_thread->t_env,
                                   export, target, &cluuid, data, client_nid);
        }
        if (rc)
                GOTO(out, rc);

        LASSERT(target->u.obt.obt_magic == OBT_MAGIC);
        data->ocd_instance = target->u.obt.obt_instance;

        /* Return only the parts of obd_connect_data that we understand, so the
         * client knows that we don't understand the rest. */
        if (data) {
                tmpsize = req_capsule_get_size(&req->rq_pill, &RMF_CONNECT_DATA,
                                               RCL_SERVER);
                tmpdata = req_capsule_server_get(&req->rq_pill,
                                                 &RMF_CONNECT_DATA);
                /* Don't use struct assignment here, because the client reply
                 * buffer may be smaller/larger than the local struct
                 * obd_connect_data. */
                memcpy(tmpdata, data, min(tmpsize, size));
        }

        /* If all else goes well, this is our RPC return code. */
        req->rq_status = 0;

        lustre_msg_set_handle(req->rq_repmsg, &conn);

        /* If the client and the server are the same node, we will already
         * have an export that really points to the client's DLM export,
         * because we have a shared handles table.
         *
         * XXX this will go away when shaver stops sending the "connect" handle
         * in the real "remote handle" field of the request --phik 24 Apr 2003
         */
        if (req->rq_export != NULL)
                class_export_put(req->rq_export);

	/* request takes one export refcount */
	req->rq_export = class_export_get(export);

        cfs_spin_lock(&export->exp_lock);
        if (export->exp_conn_cnt >= lustre_msg_get_conn_cnt(req->rq_reqmsg)) {
                cfs_spin_unlock(&export->exp_lock);
                CDEBUG(D_RPCTRACE, "%s: %s already connected at higher "
                       "conn_cnt: %d > %d\n",
                       cluuid.uuid, libcfs_nid2str(req->rq_peer.nid),
                       export->exp_conn_cnt,
                       lustre_msg_get_conn_cnt(req->rq_reqmsg));

                GOTO(out, rc = -EALREADY);
        }
        LASSERT(lustre_msg_get_conn_cnt(req->rq_reqmsg) > 0);
        export->exp_conn_cnt = lustre_msg_get_conn_cnt(req->rq_reqmsg);
        export->exp_abort_active_req = 0;

        /* request from liblustre?  Don't evict it for not pinging. */
        if (lustre_msg_get_op_flags(req->rq_reqmsg) & MSG_CONNECT_LIBCLIENT) {
                export->exp_libclient = 1;
                cfs_spin_unlock(&export->exp_lock);

                cfs_spin_lock(&target->obd_dev_lock);
                cfs_list_del_init(&export->exp_obd_chain_timed);
                cfs_spin_unlock(&target->obd_dev_lock);
        } else {
                cfs_spin_unlock(&export->exp_lock);
        }

        if (export->exp_connection != NULL) {
                /* Check to see if connection came from another NID */
                if ((export->exp_connection->c_peer.nid != req->rq_peer.nid) &&
                    !cfs_hlist_unhashed(&export->exp_nid_hash))
                        cfs_hash_del(export->exp_obd->obd_nid_hash,
                                     &export->exp_connection->c_peer.nid,
                                     &export->exp_nid_hash);

                ptlrpc_connection_put(export->exp_connection);
        }

        export->exp_connection = ptlrpc_connection_get(req->rq_peer,
                                                       req->rq_self,
                                                       &remote_uuid);
        if (cfs_hlist_unhashed(&export->exp_nid_hash)) {
                cfs_hash_add(export->exp_obd->obd_nid_hash,
                             &export->exp_connection->c_peer.nid,
                             &export->exp_nid_hash);
        }
        /**
          class_disconnect->class_export_recovery_cleanup() race
         */
        if (target->obd_recovering && !export->exp_in_recovery) {
                int has_transno;
                __u64 transno = data->ocd_transno;

                cfs_spin_lock(&export->exp_lock);
                export->exp_in_recovery = 1;
                export->exp_req_replay_needed = 1;
                export->exp_lock_replay_needed = 1;
                cfs_spin_unlock(&export->exp_lock);

                has_transno = !!(lustre_msg_get_op_flags(req->rq_reqmsg) &
                                 MSG_CONNECT_TRANSNO);
                if (has_transno && transno == 0)
                        CWARN("Connect with zero transno!\n");

                if (has_transno && transno > 0 &&
                    transno < target->obd_next_recovery_transno &&
                    transno > target->obd_last_committed) {
                        /* another way is to use cmpxchg() so it will be
                         * lock free */
                        cfs_spin_lock(&target->obd_recovery_task_lock);
                        if (transno < target->obd_next_recovery_transno)
                                target->obd_next_recovery_transno = transno;
                        cfs_spin_unlock(&target->obd_recovery_task_lock);
                }

                cfs_atomic_inc(&target->obd_req_replay_clients);
                cfs_atomic_inc(&target->obd_lock_replay_clients);
                if (cfs_atomic_inc_return(&target->obd_connected_clients) ==
                    target->obd_max_recoverable_clients)
                        cfs_waitq_signal(&target->obd_next_transno_waitq);
        }

        /* Tell the client we're in recovery, when client is involved in it. */
        if (target->obd_recovering)
                lustre_msg_add_op_flags(req->rq_repmsg, MSG_CONNECT_RECOVERING);

        tmp = req_capsule_client_get(&req->rq_pill, &RMF_CONN);
        conn = *tmp;

        if (export->exp_imp_reverse != NULL) {
                client_destroy_import(export->exp_imp_reverse);
        }

        /* for the rest part, we return -ENOTCONN in case of errors
         * in order to let client initialize connection again.
         */
        revimp = export->exp_imp_reverse = class_new_import(target);
        if (!revimp) {
                CERROR("fail to alloc new reverse import.\n");
                GOTO(out, rc = -ENOTCONN);
        }

        revimp->imp_connection = ptlrpc_connection_addref(export->exp_connection);
        revimp->imp_client = &export->exp_obd->obd_ldlm_client;
        revimp->imp_remote_handle = conn;
        revimp->imp_dlm_fake = 1;
        revimp->imp_state = LUSTRE_IMP_FULL;

        /* unknown versions will be caught in
         * ptlrpc_handle_server_req_in->lustre_unpack_msg() */
        revimp->imp_msg_magic = req->rq_reqmsg->lm_magic;

        if ((export->exp_connect_flags & OBD_CONNECT_AT) &&
            (revimp->imp_msg_magic != LUSTRE_MSG_MAGIC_V1))
                revimp->imp_msghdr_flags |= MSGHDR_AT_SUPPORT;
        else
                revimp->imp_msghdr_flags &= ~MSGHDR_AT_SUPPORT;

        if ((export->exp_connect_flags & OBD_CONNECT_FULL20) &&
            (revimp->imp_msg_magic != LUSTRE_MSG_MAGIC_V1))
                revimp->imp_msghdr_flags |= MSGHDR_CKSUM_INCOMPAT18;
        else
                revimp->imp_msghdr_flags &= ~MSGHDR_CKSUM_INCOMPAT18;

        rc = sptlrpc_import_sec_adapt(revimp, req->rq_svc_ctx, &req->rq_flvr);
        if (rc) {
                CERROR("Failed to get sec for reverse import: %d\n", rc);
                export->exp_imp_reverse = NULL;
                class_destroy_import(revimp);
        }

        class_import_put(revimp);
out:
        if (export) {
                cfs_spin_lock(&export->exp_lock);
                export->exp_connecting = 0;
                cfs_spin_unlock(&export->exp_lock);

                class_export_put(export);
        }
        if (targref) {
		cfs_spin_lock(&target->obd_dev_lock);
		target->obd_conn_inprogress--;
		cfs_spin_unlock(&target->obd_dev_lock);

                class_decref(targref, __FUNCTION__, cfs_current());
	}
        if (rc)
                req->rq_status = rc;
        RETURN(rc);
}

int target_handle_disconnect(struct ptlrpc_request *req)
{
        int rc;
        ENTRY;

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                RETURN(rc);

        /* keep the rq_export around so we can send the reply */
        req->rq_status = obd_disconnect(class_export_get(req->rq_export));

        RETURN(0);
}

void target_destroy_export(struct obd_export *exp)
{
        /* exports created from last_rcvd data, and "fake"
           exports created by lctl don't have an import */
        if (exp->exp_imp_reverse != NULL)
                client_destroy_import(exp->exp_imp_reverse);

        LASSERT_ATOMIC_ZERO(&exp->exp_locks_count);
        LASSERT_ATOMIC_ZERO(&exp->exp_rpc_count);
        LASSERT_ATOMIC_ZERO(&exp->exp_cb_count);
        LASSERT_ATOMIC_ZERO(&exp->exp_replay_count);
}

/*
 * Recovery functions
 */
static void target_request_copy_get(struct ptlrpc_request *req)
{
        class_export_rpc_get(req->rq_export);
        LASSERT(cfs_list_empty(&req->rq_list));
        CFS_INIT_LIST_HEAD(&req->rq_replay_list);

        /* increase refcount to keep request in queue */
        cfs_atomic_inc(&req->rq_refcount);
        /** let export know it has replays to be handled */
        cfs_atomic_inc(&req->rq_export->exp_replay_count);
}

static void target_request_copy_put(struct ptlrpc_request *req)
{
        LASSERT(cfs_list_empty(&req->rq_replay_list));
        LASSERT_ATOMIC_POS(&req->rq_export->exp_replay_count);

        cfs_atomic_dec(&req->rq_export->exp_replay_count);
        class_export_rpc_put(req->rq_export);
        ptlrpc_server_drop_request(req);
}

static int target_exp_enqueue_req_replay(struct ptlrpc_request *req)
{
        __u64                  transno = lustre_msg_get_transno(req->rq_reqmsg);
        struct obd_export     *exp = req->rq_export;
        struct ptlrpc_request *reqiter;
        int                    dup = 0;

        LASSERT(exp);

        cfs_spin_lock(&exp->exp_lock);
        cfs_list_for_each_entry(reqiter, &exp->exp_req_replay_queue,
                                rq_replay_list) {
                if (lustre_msg_get_transno(reqiter->rq_reqmsg) == transno) {
                        dup = 1;
                        break;
                }
        }

        if (dup) {
                /* we expect it with RESENT and REPLAY flags */
                if ((lustre_msg_get_flags(req->rq_reqmsg) &
                     (MSG_RESENT | MSG_REPLAY)) != (MSG_RESENT | MSG_REPLAY))
                        CERROR("invalid flags %x of resent replay\n",
                               lustre_msg_get_flags(req->rq_reqmsg));
        } else {
                cfs_list_add_tail(&req->rq_replay_list,
                                  &exp->exp_req_replay_queue);
        }

        cfs_spin_unlock(&exp->exp_lock);
        return dup;
}

static void target_exp_dequeue_req_replay(struct ptlrpc_request *req)
{
        LASSERT(!cfs_list_empty(&req->rq_replay_list));
        LASSERT(req->rq_export);

        cfs_spin_lock(&req->rq_export->exp_lock);
        cfs_list_del_init(&req->rq_replay_list);
        cfs_spin_unlock(&req->rq_export->exp_lock);
}

#ifdef __KERNEL__
static void target_finish_recovery(struct obd_device *obd)
{
        ENTRY;

	/* only log a recovery message when recovery has occurred */
	if (obd->obd_recovery_start) {
		time_t elapsed_time = max_t(time_t, 1, cfs_time_current_sec() -
					    obd->obd_recovery_start);

		LCONSOLE_INFO("%s: Recovery over after %d:%.02d, of %d "
			      "clients %d recovered and %d %s evicted.\n",
			      obd->obd_name,
			      (int)elapsed_time / 60, (int)elapsed_time % 60,
			      obd->obd_max_recoverable_clients,
			      cfs_atomic_read(&obd->obd_connected_clients),
			      obd->obd_stale_clients,
			      obd->obd_stale_clients == 1 ? "was" : "were");
	}

        ldlm_reprocess_all_ns(obd->obd_namespace);
        cfs_spin_lock(&obd->obd_recovery_task_lock);
        if (!cfs_list_empty(&obd->obd_req_replay_queue) ||
            !cfs_list_empty(&obd->obd_lock_replay_queue) ||
            !cfs_list_empty(&obd->obd_final_req_queue)) {
                LCONSOLE_WARN("%s: Recovery queues ( %s%s%s) are not empty\n",
                       obd->obd_name,
                       cfs_list_empty(&obd->obd_req_replay_queue) ? "" : "req ",
                       cfs_list_empty(&obd->obd_lock_replay_queue) ? \
                               "" : "lock ",
                       cfs_list_empty(&obd->obd_final_req_queue) ? \
                               "" : "final ");
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
                LBUG();
        }
        cfs_spin_unlock(&obd->obd_recovery_task_lock);

        obd->obd_recovery_end = cfs_time_current_sec();

        /* when recovery finished, cleanup orphans on mds and ost */
        if (OBT(obd) && OBP(obd, postrecov)) {
                int rc = OBP(obd, postrecov)(obd);
                if (rc < 0)
                        LCONSOLE_WARN("%s: Post recovery failed, rc %d\n",
                                      obd->obd_name, rc);
        }
        EXIT;
}

static void abort_req_replay_queue(struct obd_device *obd)
{
        struct ptlrpc_request *req, *n;
        cfs_list_t abort_list;

        CFS_INIT_LIST_HEAD(&abort_list);
        cfs_spin_lock(&obd->obd_recovery_task_lock);
        cfs_list_splice_init(&obd->obd_req_replay_queue, &abort_list);
        cfs_spin_unlock(&obd->obd_recovery_task_lock);
        cfs_list_for_each_entry_safe(req, n, &abort_list, rq_list) {
                DEBUG_REQ(D_WARNING, req, "aborted:");
                req->rq_status = -ENOTCONN;
                if (ptlrpc_error(req)) {
                        DEBUG_REQ(D_ERROR, req,
                                  "failed abort_req_reply; skipping");
                }
                target_exp_dequeue_req_replay(req);
                target_request_copy_put(req);
        }
}

static void abort_lock_replay_queue(struct obd_device *obd)
{
        struct ptlrpc_request *req, *n;
        cfs_list_t abort_list;

        CFS_INIT_LIST_HEAD(&abort_list);
        cfs_spin_lock(&obd->obd_recovery_task_lock);
        cfs_list_splice_init(&obd->obd_lock_replay_queue, &abort_list);
        cfs_spin_unlock(&obd->obd_recovery_task_lock);
        cfs_list_for_each_entry_safe(req, n, &abort_list, rq_list){
                DEBUG_REQ(D_ERROR, req, "aborted:");
                req->rq_status = -ENOTCONN;
                if (ptlrpc_error(req)) {
                        DEBUG_REQ(D_ERROR, req,
                                  "failed abort_lock_reply; skipping");
                }
                target_request_copy_put(req);
        }
}

/* Called from a cleanup function if the device is being cleaned up
   forcefully.  The exports should all have been disconnected already,
   the only thing left to do is
     - clear the recovery flags
     - cancel the timer
     - free queued requests and replies, but don't send replies
   Because the obd_stopping flag is set, no new requests should be received.

*/
void target_cleanup_recovery(struct obd_device *obd)
{
        struct ptlrpc_request *req, *n;
        cfs_list_t clean_list;
        ENTRY;

        CFS_INIT_LIST_HEAD(&clean_list);
        cfs_spin_lock(&obd->obd_dev_lock);
        if (!obd->obd_recovering) {
                cfs_spin_unlock(&obd->obd_dev_lock);
                EXIT;
                return;
        }
        obd->obd_recovering = obd->obd_abort_recovery = 0;
        cfs_spin_unlock(&obd->obd_dev_lock);

        cfs_spin_lock(&obd->obd_recovery_task_lock);
        target_cancel_recovery_timer(obd);
        cfs_list_splice_init(&obd->obd_req_replay_queue, &clean_list);
        cfs_spin_unlock(&obd->obd_recovery_task_lock);

        cfs_list_for_each_entry_safe(req, n, &clean_list, rq_list) {
                LASSERT(req->rq_reply_state == 0);
                target_exp_dequeue_req_replay(req);
                target_request_copy_put(req);
        }

        cfs_spin_lock(&obd->obd_recovery_task_lock);
        cfs_list_splice_init(&obd->obd_lock_replay_queue, &clean_list);
        cfs_list_splice_init(&obd->obd_final_req_queue, &clean_list);
        cfs_spin_unlock(&obd->obd_recovery_task_lock);

        cfs_list_for_each_entry_safe(req, n, &clean_list, rq_list){
                LASSERT(req->rq_reply_state == 0);
                target_request_copy_put(req);
        }

        EXIT;
}

/* obd_recovery_task_lock should be held */
void target_cancel_recovery_timer(struct obd_device *obd)
{
        CDEBUG(D_HA, "%s: cancel recovery timer\n", obd->obd_name);
        cfs_timer_disarm(&obd->obd_recovery_timer);
}

static void target_start_recovery_timer(struct obd_device *obd)
{
        if (obd->obd_recovery_start != 0)
                return;

        cfs_spin_lock(&obd->obd_dev_lock);
        if (!obd->obd_recovering || obd->obd_abort_recovery) {
                cfs_spin_unlock(&obd->obd_dev_lock);
                return;
        }

        LASSERT(obd->obd_recovery_timeout != 0);

        if (obd->obd_recovery_start != 0) {
                cfs_spin_unlock(&obd->obd_dev_lock);
                return;
        }

        cfs_timer_arm(&obd->obd_recovery_timer,
                      cfs_time_shift(obd->obd_recovery_timeout));
        obd->obd_recovery_start = cfs_time_current_sec();
        cfs_spin_unlock(&obd->obd_dev_lock);

        LCONSOLE_WARN("%s: Will be in recovery for at least %d:%.02d, "
                      "or until %d client%s reconnect%s.\n",
                      obd->obd_name,
                      obd->obd_recovery_timeout / 60,
                      obd->obd_recovery_timeout % 60,
                      obd->obd_max_recoverable_clients,
                      (obd->obd_max_recoverable_clients == 1) ? "" : "s",
                      (obd->obd_max_recoverable_clients == 1) ? "s": "");
}

/**
 * extend recovery window.
 *
 * if @extend is true, extend recovery window to have @drt remaining at least;
 * otherwise, make sure the recovery timeout value is not less than @drt.
 */
static void extend_recovery_timer(struct obd_device *obd, int drt, bool extend)
{
        cfs_time_t now;
        cfs_time_t end;
        cfs_duration_t left;
        int to;

        cfs_spin_lock(&obd->obd_dev_lock);
        if (!obd->obd_recovering || obd->obd_abort_recovery) {
                cfs_spin_unlock(&obd->obd_dev_lock);
                return;
        }
        LASSERT(obd->obd_recovery_start != 0);

        now  = cfs_time_current_sec();
        to   = obd->obd_recovery_timeout;
        end  = obd->obd_recovery_start + to;
        left = cfs_time_sub(end, now);

        if (extend && (drt > left)) {
                to += drt - left;
        } else if (!extend && (drt > to)) {
                to = drt;
                /* reduce drt by already passed time */
                drt -= obd->obd_recovery_timeout - left;
        }

        if (to > obd->obd_recovery_time_hard)
                to = obd->obd_recovery_time_hard;
        if (obd->obd_recovery_timeout < to) {
                obd->obd_recovery_timeout = to;
                cfs_timer_arm(&obd->obd_recovery_timer,
                              cfs_time_shift(drt));
        }
        cfs_spin_unlock(&obd->obd_dev_lock);

        CDEBUG(D_HA, "%s: recovery timer will expire in %u seconds\n",
               obd->obd_name, (unsigned)drt);
}

/* Reset the timer with each new client connection */
/*
 * This timer is actually reconnect_timer, which is for making sure
 * the total recovery window is at least as big as my reconnect
 * attempt timing. So the initial recovery time_out will be set to
 * OBD_RECOVERY_FACTOR * obd_timeout. If the timeout coming
 * from client is bigger than this, then the recovery time_out will
 * be extended to make sure the client could be reconnected, in the
 * process, the timeout from the new client should be ignored.
 */

static void
check_and_start_recovery_timer(struct obd_device *obd,
                               struct ptlrpc_request *req,
                               int new_client)
{
        int service_time = lustre_msg_get_service_time(req->rq_reqmsg);
        struct lustre_sb_info *lsi;

        if (!new_client && service_time)
                /* Teach server about old server's estimates, as first guess
                 * at how long new requests will take. */
                at_measured(&req->rq_rqbd->rqbd_service->srv_at_estimate,
                            service_time);

        target_start_recovery_timer(obd);

        /* convert the service time to rpc timeout,
         * reuse service_time to limit stack usage */
        service_time = at_est2timeout(service_time);

        /* We expect other clients to timeout within service_time, then try
         * to reconnect, then try the failover server.  The max delay between
         * connect attempts is SWITCH_MAX + SWITCH_INC + INITIAL */
        service_time += 2 * INITIAL_CONNECT_TIMEOUT;

        LASSERT(obd->u.obt.obt_magic == OBT_MAGIC);
        lsi = server_get_mount(obd->obd_name);
        if (lsi == NULL || !(lsi->lsi_flags | LDD_F_IR_CAPABLE))
                service_time += 2 * (CONNECTION_SWITCH_MAX +
                                     CONNECTION_SWITCH_INC);
        if (service_time > obd->obd_recovery_timeout && !new_client)
                extend_recovery_timer(obd, service_time, false);
}

/** Health checking routines */
static inline int exp_connect_healthy(struct obd_export *exp)
{
        return (exp->exp_in_recovery);
}

/** if export done req_replay or has replay in queue */
static inline int exp_req_replay_healthy(struct obd_export *exp)
{
        return (!exp->exp_req_replay_needed ||
                cfs_atomic_read(&exp->exp_replay_count) > 0);
}
/** if export done lock_replay or has replay in queue */
static inline int exp_lock_replay_healthy(struct obd_export *exp)
{
        return (!exp->exp_lock_replay_needed ||
                cfs_atomic_read(&exp->exp_replay_count) > 0);
}

static inline int exp_vbr_healthy(struct obd_export *exp)
{
        return (!exp->exp_vbr_failed);
}

static inline int exp_finished(struct obd_export *exp)
{
        return (exp->exp_in_recovery && !exp->exp_lock_replay_needed);
}

/** Checking routines for recovery */
static int check_for_clients(struct obd_device *obd)
{
        unsigned int clnts = cfs_atomic_read(&obd->obd_connected_clients);

        if (obd->obd_abort_recovery || obd->obd_recovery_expired)
                return 1;

        LASSERT(clnts <= obd->obd_max_recoverable_clients);
        return (clnts + obd->obd_stale_clients ==
                obd->obd_max_recoverable_clients);
}

static int check_for_next_transno(struct obd_device *obd)
{
        struct ptlrpc_request *req = NULL;
        int wake_up = 0, connected, completed, queue_len;
        __u64 next_transno, req_transno;
        ENTRY;

        cfs_spin_lock(&obd->obd_recovery_task_lock);
        if (!cfs_list_empty(&obd->obd_req_replay_queue)) {
                req = cfs_list_entry(obd->obd_req_replay_queue.next,
                                     struct ptlrpc_request, rq_list);
                req_transno = lustre_msg_get_transno(req->rq_reqmsg);
        } else {
                req_transno = 0;
        }

        connected = cfs_atomic_read(&obd->obd_connected_clients);
        completed = connected - cfs_atomic_read(&obd->obd_req_replay_clients);
        queue_len = obd->obd_requests_queued_for_recovery;
        next_transno = obd->obd_next_recovery_transno;

        CDEBUG(D_HA, "max: %d, connected: %d, completed: %d, queue_len: %d, "
               "req_transno: "LPU64", next_transno: "LPU64"\n",
               obd->obd_max_recoverable_clients, connected, completed,
               queue_len, req_transno, next_transno);

        if (obd->obd_abort_recovery) {
                CDEBUG(D_HA, "waking for aborted recovery\n");
                wake_up = 1;
        } else if (obd->obd_recovery_expired) {
                CDEBUG(D_HA, "waking for expired recovery\n");
                wake_up = 1;
        } else if (cfs_atomic_read(&obd->obd_req_replay_clients) == 0) {
                CDEBUG(D_HA, "waking for completed recovery\n");
                wake_up = 1;
        } else if (req_transno == next_transno) {
                CDEBUG(D_HA, "waking for next ("LPD64")\n", next_transno);
                wake_up = 1;
        } else if (queue_len == cfs_atomic_read(&obd->obd_req_replay_clients)) {
                int d_lvl = D_HA;
                /** handle gaps occured due to lost reply or VBR */
                LASSERTF(req_transno >= next_transno,
                         "req_transno: "LPU64", next_transno: "LPU64"\n",
                         req_transno, next_transno);
                if (req_transno > obd->obd_last_committed &&
                    !obd->obd_version_recov)
                        d_lvl = D_ERROR;
                CDEBUG(d_lvl,
                       "%s: waking for gap in transno, VBR is %s (skip: "
                       LPD64", ql: %d, comp: %d, conn: %d, next: "LPD64
                       ", last_committed: "LPD64")\n",
                       obd->obd_name, obd->obd_version_recov ? "ON" : "OFF",
                       next_transno, queue_len, completed, connected,
                       req_transno, obd->obd_last_committed);
                obd->obd_next_recovery_transno = req_transno;
                wake_up = 1;
        } else if (OBD_FAIL_CHECK(OBD_FAIL_MDS_RECOVERY_ACCEPTS_GAPS)) {
                CDEBUG(D_HA, "accepting transno gaps is explicitly allowed"
                       " by fail_lock, waking up ("LPD64")\n", next_transno);
                obd->obd_next_recovery_transno = req_transno;
                wake_up = 1;
        }
        cfs_spin_unlock(&obd->obd_recovery_task_lock);
        return wake_up;
}

static int check_for_next_lock(struct obd_device *obd)
{
        int wake_up = 0;

        cfs_spin_lock(&obd->obd_recovery_task_lock);
        if (!cfs_list_empty(&obd->obd_lock_replay_queue)) {
                CDEBUG(D_HA, "waking for next lock\n");
                wake_up = 1;
        } else if (cfs_atomic_read(&obd->obd_lock_replay_clients) == 0) {
                CDEBUG(D_HA, "waking for completed lock replay\n");
                wake_up = 1;
        } else if (obd->obd_abort_recovery) {
                CDEBUG(D_HA, "waking for aborted recovery\n");
                wake_up = 1;
        } else if (obd->obd_recovery_expired) {
                CDEBUG(D_HA, "waking for expired recovery\n");
                wake_up = 1;
        }
        cfs_spin_unlock(&obd->obd_recovery_task_lock);

        return wake_up;
}

/**
 * wait for recovery events,
 * check its status with help of check_routine
 * evict dead clients via health_check
 */
static int target_recovery_overseer(struct obd_device *obd,
                                    int (*check_routine)(struct obd_device *),
                                    int (*health_check)(struct obd_export *))
{
repeat:
        cfs_wait_event(obd->obd_next_transno_waitq, check_routine(obd));
        if (obd->obd_abort_recovery) {
                CDEBUG(D_HA, "recovery aborted, evicting stale exports\n");
                /** evict exports which didn't finish recovery yet */
                class_disconnect_stale_exports(obd, exp_finished);
                return 1;
        } else if (obd->obd_recovery_expired) {
                obd->obd_recovery_expired = 0;
                /** If some clients died being recovered, evict them */
                CDEBUG(D_HA, "recovery timed out, evicting stale exports\n");
                /** evict cexports with no replay in queue, they are stalled */
                class_disconnect_stale_exports(obd, health_check);
                /** continue with VBR */
                cfs_spin_lock(&obd->obd_dev_lock);
                obd->obd_version_recov = 1;
                cfs_spin_unlock(&obd->obd_dev_lock);
                /**
                 * reset timer, recovery will proceed with versions now,
                 * timeout is set just to handle reconnection delays
                 */
                extend_recovery_timer(obd, RECONNECT_DELAY_MAX, true);
                /** Wait for recovery events again, after evicting bad clients */
                goto repeat;
        }
        return 0;
}

static struct ptlrpc_request *target_next_replay_req(struct obd_device *obd)
{
        struct ptlrpc_request *req = NULL;
        ENTRY;

        CDEBUG(D_HA, "Waiting for transno "LPD64"\n",
               obd->obd_next_recovery_transno);

        if (target_recovery_overseer(obd, check_for_next_transno,
                                     exp_req_replay_healthy)) {
                abort_req_replay_queue(obd);
                abort_lock_replay_queue(obd);
        }

        cfs_spin_lock(&obd->obd_recovery_task_lock);
        if (!cfs_list_empty(&obd->obd_req_replay_queue)) {
                req = cfs_list_entry(obd->obd_req_replay_queue.next,
                                     struct ptlrpc_request, rq_list);
                cfs_list_del_init(&req->rq_list);
                obd->obd_requests_queued_for_recovery--;
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
        } else {
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
                LASSERT(cfs_list_empty(&obd->obd_req_replay_queue));
                LASSERT(cfs_atomic_read(&obd->obd_req_replay_clients) == 0);
                /** evict exports failed VBR */
                class_disconnect_stale_exports(obd, exp_vbr_healthy);
        }
        RETURN(req);
}

static struct ptlrpc_request *target_next_replay_lock(struct obd_device *obd)
{
        struct ptlrpc_request *req = NULL;

        CDEBUG(D_HA, "Waiting for lock\n");
        if (target_recovery_overseer(obd, check_for_next_lock,
                                     exp_lock_replay_healthy))
                abort_lock_replay_queue(obd);

        cfs_spin_lock(&obd->obd_recovery_task_lock);
        if (!cfs_list_empty(&obd->obd_lock_replay_queue)) {
                req = cfs_list_entry(obd->obd_lock_replay_queue.next,
                                     struct ptlrpc_request, rq_list);
                cfs_list_del_init(&req->rq_list);
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
        } else {
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
                LASSERT(cfs_list_empty(&obd->obd_lock_replay_queue));
                LASSERT(cfs_atomic_read(&obd->obd_lock_replay_clients) == 0);
                /** evict exports failed VBR */
                class_disconnect_stale_exports(obd, exp_vbr_healthy);
        }
        return req;
}

static struct ptlrpc_request *target_next_final_ping(struct obd_device *obd)
{
        struct ptlrpc_request *req = NULL;

        cfs_spin_lock(&obd->obd_recovery_task_lock);
        if (!cfs_list_empty(&obd->obd_final_req_queue)) {
                req = cfs_list_entry(obd->obd_final_req_queue.next,
                                     struct ptlrpc_request, rq_list);
                cfs_list_del_init(&req->rq_list);
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
                if (req->rq_export->exp_in_recovery) {
                        cfs_spin_lock(&req->rq_export->exp_lock);
                        req->rq_export->exp_in_recovery = 0;
                        cfs_spin_unlock(&req->rq_export->exp_lock);
                }
        } else {
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
        }
        return req;
}

static int handle_recovery_req(struct ptlrpc_thread *thread,
                               struct ptlrpc_request *req,
                               svc_handler_t handler)
{
        int rc;
        ENTRY;

        /**
         * export can be evicted during recovery, no need to handle replays for
         * it after that, discard such request silently
         */
        if (req->rq_export->exp_disconnected)
                GOTO(reqcopy_put, rc = 0);

        rc = lu_context_init(&req->rq_recov_session, LCT_SESSION);
        if (rc) {
                CERROR("Failure to initialize session: %d\n", rc);
                GOTO(reqcopy_put, rc);
        }

        req->rq_recov_session.lc_thread = thread;
        lu_context_enter(&req->rq_recov_session);
        req->rq_svc_thread = thread;
        req->rq_svc_thread->t_env->le_ses = &req->rq_recov_session;

        /* thread context */
        lu_context_enter(&thread->t_env->le_ctx);
        (void)handler(req);
        lu_context_exit(&thread->t_env->le_ctx);

        lu_context_exit(&req->rq_recov_session);
        lu_context_fini(&req->rq_recov_session);
        /* don't reset timer for final stage */
        if (!exp_finished(req->rq_export)) {
                int to = obd_timeout;

                /**
                 * Add request timeout to the recovery time so next request from
                 * this client may come in recovery time
                 */
                if (!AT_OFF) {
                        struct ptlrpc_service *svc = req->rq_rqbd->rqbd_service;
                        /* If the server sent early reply for this request,
                         * the client will recalculate the timeout according to
                         * current server estimate service time, so we will
                         * use the maxium timeout here for waiting the client
                         * sending the next req */
                        to = max((int)at_est2timeout(
                                 at_get(&svc->srv_at_estimate)),
                                 (int)lustre_msg_get_timeout(req->rq_reqmsg));
                        /* Add net_latency (see ptlrpc_replay_req) */
                        to += lustre_msg_get_service_time(req->rq_reqmsg);
                }
                extend_recovery_timer(class_exp2obd(req->rq_export), to, true);
        }
reqcopy_put:
        RETURN(rc);
}

static int target_recovery_thread(void *arg)
{
        struct lu_target *lut = arg;
        struct obd_device *obd = lut->lut_obd;
        struct ptlrpc_request *req;
        struct target_recovery_data *trd = &obd->obd_recovery_data;
        unsigned long delta;
        unsigned long flags;
        struct lu_env *env;
        struct ptlrpc_thread *thread = NULL;
        int rc = 0;
        ENTRY;

        cfs_daemonize_ctxt("tgt_recov");

        SIGNAL_MASK_LOCK(current, flags);
        sigfillset(&current->blocked);
        RECALC_SIGPENDING;
        SIGNAL_MASK_UNLOCK(current, flags);

        OBD_ALLOC_PTR(thread);
        if (thread == NULL)
                RETURN(-ENOMEM);

        OBD_ALLOC_PTR(env);
        if (env == NULL) {
                OBD_FREE_PTR(thread);
                RETURN(-ENOMEM);
        }

        rc = lu_context_init(&env->le_ctx, LCT_MD_THREAD | LCT_DT_THREAD);
        if (rc) {
                OBD_FREE_PTR(thread);
                OBD_FREE_PTR(env);
                RETURN(rc);
        }

        thread->t_env = env;
        thread->t_id = -1; /* force filter_iobuf_get/put to use local buffers */
        env->le_ctx.lc_thread = thread;
        thread->t_data = NULL;
        thread->t_watchdog = NULL;

        CDEBUG(D_HA, "%s: started recovery thread pid %d\n", obd->obd_name,
               cfs_curproc_pid());
        trd->trd_processing_task = cfs_curproc_pid();

        cfs_spin_lock(&obd->obd_dev_lock);
        obd->obd_recovering = 1;
        cfs_spin_unlock(&obd->obd_dev_lock);
        cfs_complete(&trd->trd_starting);

        /* first of all, we have to know the first transno to replay */
        if (target_recovery_overseer(obd, check_for_clients,
                                     exp_connect_healthy)) {
                abort_req_replay_queue(obd);
                abort_lock_replay_queue(obd);
        }

        /* next stage: replay requests */
        delta = jiffies;
        CDEBUG(D_INFO, "1: request replay stage - %d clients from t"LPU64"\n",
               cfs_atomic_read(&obd->obd_req_replay_clients),
               obd->obd_next_recovery_transno);
        while ((req = target_next_replay_req(obd))) {
                LASSERT(trd->trd_processing_task == cfs_curproc_pid());
                DEBUG_REQ(D_HA, req, "processing t"LPD64" from %s",
                          lustre_msg_get_transno(req->rq_reqmsg),
                          libcfs_nid2str(req->rq_peer.nid));
                handle_recovery_req(thread, req,
                                    trd->trd_recovery_handler);
                /**
                 * bz18031: increase next_recovery_transno before
                 * target_request_copy_put() will drop exp_rpc reference
                 */
                cfs_spin_lock(&obd->obd_recovery_task_lock);
                obd->obd_next_recovery_transno++;
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
                target_exp_dequeue_req_replay(req);
                target_request_copy_put(req);
                obd->obd_replayed_requests++;
        }

        /**
         * The second stage: replay locks
         */
        CDEBUG(D_INFO, "2: lock replay stage - %d clients\n",
               cfs_atomic_read(&obd->obd_lock_replay_clients));
        while ((req = target_next_replay_lock(obd))) {
                LASSERT(trd->trd_processing_task == cfs_curproc_pid());
                DEBUG_REQ(D_HA, req, "processing lock from %s: ",
                          libcfs_nid2str(req->rq_peer.nid));
                handle_recovery_req(thread, req,
                                    trd->trd_recovery_handler);
                target_request_copy_put(req);
                obd->obd_replayed_locks++;
        }

        /**
         * The third stage: reply on final pings, at this moment all clients
         * must have request in final queue
         */
        CDEBUG(D_INFO, "3: final stage - process recovery completion pings\n");
        /** Update server last boot epoch */
        lut_boot_epoch_update(lut);
        /* We drop recoverying flag to forward all new requests
         * to regular mds_handle() since now */
        cfs_spin_lock(&obd->obd_dev_lock);
        obd->obd_recovering = obd->obd_abort_recovery = 0;
        cfs_spin_unlock(&obd->obd_dev_lock);
        cfs_spin_lock(&obd->obd_recovery_task_lock);
        target_cancel_recovery_timer(obd);
        cfs_spin_unlock(&obd->obd_recovery_task_lock);
        while ((req = target_next_final_ping(obd))) {
                LASSERT(trd->trd_processing_task == cfs_curproc_pid());
                DEBUG_REQ(D_HA, req, "processing final ping from %s: ",
                          libcfs_nid2str(req->rq_peer.nid));
                handle_recovery_req(thread, req,
                                    trd->trd_recovery_handler);
                target_request_copy_put(req);
        }

        delta = (jiffies - delta) / CFS_HZ;
        CDEBUG(D_INFO,"4: recovery completed in %lus - %d/%d reqs/locks\n",
              delta, obd->obd_replayed_requests, obd->obd_replayed_locks);

        target_finish_recovery(obd);

        if (delta > OBD_RECOVERY_TIME_SOFT) {
                LCONSOLE_WARN("%s: Recovery exceed expected maximum time of "
                              "%d:%.02d.\n", obd->obd_name,
                              (int)OBD_RECOVERY_TIME_SOFT / 60,
                              (int)OBD_RECOVERY_TIME_SOFT % 60);
                libcfs_debug_dumplog();
        }

        lu_context_fini(&env->le_ctx);
        trd->trd_processing_task = 0;
        cfs_complete(&trd->trd_finishing);

        OBD_FREE_PTR(thread);
        OBD_FREE_PTR(env);
        RETURN(rc);
}

static int target_start_recovery_thread(struct lu_target *lut,
                                        svc_handler_t handler)
{
        struct obd_device *obd = lut->lut_obd;
        int rc = 0;
        struct target_recovery_data *trd = &obd->obd_recovery_data;

        memset(trd, 0, sizeof(*trd));
        cfs_init_completion(&trd->trd_starting);
        cfs_init_completion(&trd->trd_finishing);
        trd->trd_recovery_handler = handler;

        if (cfs_create_thread(target_recovery_thread, lut, 0) > 0)
                cfs_wait_for_completion(&trd->trd_starting);
        else
                rc = -ECHILD;

        return rc;
}

void target_stop_recovery_thread(struct obd_device *obd)
{
        if (obd->obd_recovery_data.trd_processing_task > 0) {
                struct target_recovery_data *trd = &obd->obd_recovery_data;
                /** recovery can be done but postrecovery is not yet */
                cfs_spin_lock(&obd->obd_dev_lock);
                if (obd->obd_recovering) {
                        LCONSOLE_INFO("%s: Aborting recovery\n", obd->obd_name);
                        obd->obd_abort_recovery = 1;
                        cfs_waitq_signal(&obd->obd_next_transno_waitq);
                }
                cfs_spin_unlock(&obd->obd_dev_lock);
                cfs_wait_for_completion(&trd->trd_finishing);
        }
}

void target_recovery_fini(struct obd_device *obd)
{
        class_disconnect_exports(obd);
        target_stop_recovery_thread(obd);
        target_cleanup_recovery(obd);
}
EXPORT_SYMBOL(target_recovery_fini);

static void target_recovery_expired(unsigned long castmeharder)
{
        struct obd_device *obd = (struct obd_device *)castmeharder;
        CDEBUG(D_HA, "%s: recovery timed out; %d clients are still in recovery"
               " after %lds (%d clients connected)\n",
               obd->obd_name, cfs_atomic_read(&obd->obd_lock_replay_clients),
               cfs_time_current_sec()- obd->obd_recovery_start,
               cfs_atomic_read(&obd->obd_connected_clients));

        obd->obd_recovery_expired = 1;
        cfs_waitq_signal(&obd->obd_next_transno_waitq);
}

void target_recovery_init(struct lu_target *lut, svc_handler_t handler)
{
        struct obd_device *obd = lut->lut_obd;

        CDEBUG(D_HA, "RECOVERY: service %s, %d recoverable clients, "
               "last_transno "LPU64"\n", obd->obd_name,
               obd->obd_max_recoverable_clients, obd->obd_last_committed);
        LASSERT(obd->obd_stopping == 0);
        obd->obd_next_recovery_transno = obd->obd_last_committed + 1;
        obd->obd_recovery_start = 0;
        obd->obd_recovery_end = 0;

        cfs_timer_init(&obd->obd_recovery_timer, target_recovery_expired, obd);
        target_start_recovery_thread(lut, handler);
}
EXPORT_SYMBOL(target_recovery_init);

#endif /* __KERNEL__ */

static int target_process_req_flags(struct obd_device *obd,
                                    struct ptlrpc_request *req)
{
        struct obd_export *exp = req->rq_export;
        LASSERT(exp != NULL);
        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REQ_REPLAY_DONE) {
                /* client declares he's ready to replay locks */
                cfs_spin_lock(&exp->exp_lock);
                if (exp->exp_req_replay_needed) {
                        exp->exp_req_replay_needed = 0;
                        cfs_spin_unlock(&exp->exp_lock);

                        LASSERT_ATOMIC_POS(&obd->obd_req_replay_clients);
                        cfs_atomic_dec(&obd->obd_req_replay_clients);
                } else {
                        cfs_spin_unlock(&exp->exp_lock);
                }
        }
        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_LOCK_REPLAY_DONE) {
                /* client declares he's ready to complete recovery
                 * so, we put the request on th final queue */
                cfs_spin_lock(&exp->exp_lock);
                if (exp->exp_lock_replay_needed) {
                        exp->exp_lock_replay_needed = 0;
                        cfs_spin_unlock(&exp->exp_lock);

                        LASSERT_ATOMIC_POS(&obd->obd_lock_replay_clients);
                        cfs_atomic_dec(&obd->obd_lock_replay_clients);
                } else {
                        cfs_spin_unlock(&exp->exp_lock);
                }
        }
        return 0;
}

int target_queue_recovery_request(struct ptlrpc_request *req,
                                  struct obd_device *obd)
{
        cfs_list_t *tmp;
        int inserted = 0;
        __u64 transno = lustre_msg_get_transno(req->rq_reqmsg);
        ENTRY;

        if (obd->obd_recovery_data.trd_processing_task == cfs_curproc_pid()) {
                /* Processing the queue right now, don't re-add. */
                RETURN(1);
        }

        target_process_req_flags(obd, req);

        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_LOCK_REPLAY_DONE) {
                /* client declares he's ready to complete recovery
                 * so, we put the request on th final queue */
                target_request_copy_get(req);
                DEBUG_REQ(D_HA, req, "queue final req");
                cfs_waitq_signal(&obd->obd_next_transno_waitq);
                cfs_spin_lock(&obd->obd_recovery_task_lock);
                if (obd->obd_recovering) {
                        cfs_list_add_tail(&req->rq_list,
                                          &obd->obd_final_req_queue);
                } else {
                        cfs_spin_unlock(&obd->obd_recovery_task_lock);
                        target_request_copy_put(req);
                        RETURN(obd->obd_stopping ? -ENOTCONN : 1);
                }
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
                RETURN(0);
        }
        if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_REQ_REPLAY_DONE) {
                /* client declares he's ready to replay locks */
                target_request_copy_get(req);
                DEBUG_REQ(D_HA, req, "queue lock replay req");
                cfs_waitq_signal(&obd->obd_next_transno_waitq);
                cfs_spin_lock(&obd->obd_recovery_task_lock);
                LASSERT(obd->obd_recovering);
                /* usually due to recovery abort */
                if (!req->rq_export->exp_in_recovery) {
                        cfs_spin_unlock(&obd->obd_recovery_task_lock);
                        target_request_copy_put(req);
                        RETURN(-ENOTCONN);
                }
                LASSERT(req->rq_export->exp_lock_replay_needed);
                cfs_list_add_tail(&req->rq_list, &obd->obd_lock_replay_queue);
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
                RETURN(0);
        }

        /* CAVEAT EMPTOR: The incoming request message has been swabbed
         * (i.e. buflens etc are in my own byte order), but type-dependent
         * buffers (eg mdt_body, ost_body etc) have NOT been swabbed. */

        if (!transno) {
                CFS_INIT_LIST_HEAD(&req->rq_list);
                DEBUG_REQ(D_HA, req, "not queueing");
                RETURN(1);
        }

        /* If we're processing the queue, we want don't want to queue this
         * message.
         *
         * Also, if this request has a transno less than the one we're waiting
         * for, we should process it now.  It could (and currently always will)
         * be an open request for a descriptor that was opened some time ago.
         *
         * Also, a resent, replayed request that has already been
         * handled will pass through here and be processed immediately.
         */
        CDEBUG(D_HA, "Next recovery transno: "LPU64
               ", current: "LPU64", replaying\n",
               obd->obd_next_recovery_transno, transno);
        cfs_spin_lock(&obd->obd_recovery_task_lock);
        if (transno < obd->obd_next_recovery_transno) {
                /* Processing the queue right now, don't re-add. */
                LASSERT(cfs_list_empty(&req->rq_list));
                cfs_spin_unlock(&obd->obd_recovery_task_lock);
                RETURN(1);
        }
        cfs_spin_unlock(&obd->obd_recovery_task_lock);

        if (OBD_FAIL_CHECK(OBD_FAIL_TGT_REPLAY_DROP))
                RETURN(0);

        target_request_copy_get(req);
        if (!req->rq_export->exp_in_recovery) {
                target_request_copy_put(req);
                RETURN(-ENOTCONN);
        }
        LASSERT(req->rq_export->exp_req_replay_needed);

        if (target_exp_enqueue_req_replay(req)) {
                DEBUG_REQ(D_ERROR, req, "dropping resent queued req");
                target_request_copy_put(req);
                RETURN(0);
        }

        /* XXX O(n^2) */
        cfs_spin_lock(&obd->obd_recovery_task_lock);
        LASSERT(obd->obd_recovering);
        cfs_list_for_each(tmp, &obd->obd_req_replay_queue) {
                struct ptlrpc_request *reqiter =
                        cfs_list_entry(tmp, struct ptlrpc_request, rq_list);

                if (lustre_msg_get_transno(reqiter->rq_reqmsg) > transno) {
                        cfs_list_add_tail(&req->rq_list, &reqiter->rq_list);
                        inserted = 1;
                        break;
                }

                if (unlikely(lustre_msg_get_transno(reqiter->rq_reqmsg) ==
                             transno)) {
                        DEBUG_REQ(D_ERROR, req, "dropping replay: transno "
                                  "has been claimed by another client");
                        cfs_spin_unlock(&obd->obd_recovery_task_lock);
                        target_exp_dequeue_req_replay(req);
                        target_request_copy_put(req);
                        RETURN(0);
                }
        }

        if (!inserted)
                cfs_list_add_tail(&req->rq_list, &obd->obd_req_replay_queue);

        obd->obd_requests_queued_for_recovery++;
        cfs_spin_unlock(&obd->obd_recovery_task_lock);
        cfs_waitq_signal(&obd->obd_next_transno_waitq);
        RETURN(0);
}

int target_handle_ping(struct ptlrpc_request *req)
{
        obd_ping(req->rq_svc_thread->t_env, req->rq_export);
        return req_capsule_server_pack(&req->rq_pill);
}

void target_committed_to_req(struct ptlrpc_request *req)
{
        struct obd_export *exp = req->rq_export;

        if (!exp->exp_obd->obd_no_transno && req->rq_repmsg != NULL)
                lustre_msg_set_last_committed(req->rq_repmsg,
                                              exp->exp_last_committed);
        else
                DEBUG_REQ(D_IOCTL, req, "not sending last_committed update (%d/"
                          "%d)", exp->exp_obd->obd_no_transno,
                          req->rq_repmsg == NULL);

        CDEBUG(D_INFO, "last_committed "LPU64", transno "LPU64", xid "LPU64"\n",
               exp->exp_last_committed, req->rq_transno, req->rq_xid);
}
EXPORT_SYMBOL(target_committed_to_req);

#endif /* HAVE_SERVER_SUPPORT */

/**
 * Packs current SLV and Limit into \a req.
 */
int target_pack_pool_reply(struct ptlrpc_request *req)
{
        struct obd_device *obd;
        ENTRY;

        /*
         * Check that we still have all structures alive as this may
         * be some late rpc in shutdown time.
         */
        if (unlikely(!req->rq_export || !req->rq_export->exp_obd ||
                     !exp_connect_lru_resize(req->rq_export))) {
                lustre_msg_set_slv(req->rq_repmsg, 0);
                lustre_msg_set_limit(req->rq_repmsg, 0);
                RETURN(0);
        }

        /*
         * OBD is alive here as export is alive, which we checked above.
         */
        obd = req->rq_export->exp_obd;

        cfs_read_lock(&obd->obd_pool_lock);
        lustre_msg_set_slv(req->rq_repmsg, obd->obd_pool_slv);
        lustre_msg_set_limit(req->rq_repmsg, obd->obd_pool_limit);
        cfs_read_unlock(&obd->obd_pool_lock);

        RETURN(0);
}

int target_send_reply_msg(struct ptlrpc_request *req, int rc, int fail_id)
{
        if (OBD_FAIL_CHECK_ORSET(fail_id & ~OBD_FAIL_ONCE, OBD_FAIL_ONCE)) {
                DEBUG_REQ(D_ERROR, req, "dropping reply");
                return (-ECOMM);
        }

        if (unlikely(rc)) {
                DEBUG_REQ(D_NET, req, "processing error (%d)", rc);
                req->rq_status = rc;
                return (ptlrpc_send_error(req, 1));
        } else {
                DEBUG_REQ(D_NET, req, "sending reply");
        }

        return (ptlrpc_send_reply(req, PTLRPC_REPLY_MAYBE_DIFFICULT));
}

void target_send_reply(struct ptlrpc_request *req, int rc, int fail_id)
{
        int                        netrc;
        struct ptlrpc_reply_state *rs;
        struct obd_export         *exp;
        struct ptlrpc_service     *svc;
        ENTRY;

        if (req->rq_no_reply) {
                EXIT;
                return;
        }

        svc = req->rq_rqbd->rqbd_service;
        rs = req->rq_reply_state;
        if (rs == NULL || !rs->rs_difficult) {
                /* no notifiers */
                target_send_reply_msg (req, rc, fail_id);
                EXIT;
                return;
        }

        /* must be an export if locks saved */
        LASSERT (req->rq_export != NULL);
        /* req/reply consistent */
        LASSERT (rs->rs_service == svc);

        /* "fresh" reply */
        LASSERT (!rs->rs_scheduled);
        LASSERT (!rs->rs_scheduled_ever);
        LASSERT (!rs->rs_handled);
        LASSERT (!rs->rs_on_net);
        LASSERT (rs->rs_export == NULL);
        LASSERT (cfs_list_empty(&rs->rs_obd_list));
        LASSERT (cfs_list_empty(&rs->rs_exp_list));

        exp = class_export_get (req->rq_export);

        /* disable reply scheduling while I'm setting up */
        rs->rs_scheduled = 1;
        rs->rs_on_net    = 1;
        rs->rs_xid       = req->rq_xid;
        rs->rs_transno   = req->rq_transno;
        rs->rs_export    = exp;
        rs->rs_opc       = lustre_msg_get_opc(rs->rs_msg);

        cfs_spin_lock(&exp->exp_uncommitted_replies_lock);
        CDEBUG(D_NET, "rs transno = "LPU64", last committed = "LPU64"\n",
               rs->rs_transno, exp->exp_last_committed);
        if (rs->rs_transno > exp->exp_last_committed) {
                /* not committed already */
                cfs_list_add_tail(&rs->rs_obd_list,
                                  &exp->exp_uncommitted_replies);
        }
        cfs_spin_unlock (&exp->exp_uncommitted_replies_lock);

        cfs_spin_lock(&exp->exp_lock);
        cfs_list_add_tail(&rs->rs_exp_list, &exp->exp_outstanding_replies);
        cfs_spin_unlock(&exp->exp_lock);

        netrc = target_send_reply_msg (req, rc, fail_id);

        cfs_spin_lock(&svc->srv_rs_lock);

        cfs_atomic_inc(&svc->srv_n_difficult_replies);

        if (netrc != 0) {
                /* error sending: reply is off the net.  Also we need +1
                 * reply ref until ptlrpc_handle_rs() is done
                 * with the reply state (if the send was successful, there
                 * would have been +1 ref for the net, which
                 * reply_out_callback leaves alone) */
                rs->rs_on_net = 0;
                ptlrpc_rs_addref(rs);
        }

        cfs_spin_lock(&rs->rs_lock);
        if (rs->rs_transno <= exp->exp_last_committed ||
            (!rs->rs_on_net && !rs->rs_no_ack) ||
             cfs_list_empty(&rs->rs_exp_list) ||     /* completed already */
             cfs_list_empty(&rs->rs_obd_list)) {
                CDEBUG(D_HA, "Schedule reply immediately\n");
                ptlrpc_dispatch_difficult_reply(rs);
        } else {
                cfs_list_add (&rs->rs_list, &svc->srv_active_replies);
                rs->rs_scheduled = 0;           /* allow notifier to schedule */
        }
        cfs_spin_unlock(&rs->rs_lock);
        cfs_spin_unlock(&svc->srv_rs_lock);
        EXIT;
}

int target_handle_qc_callback(struct ptlrpc_request *req)
{
        struct obd_quotactl *oqctl;
        struct client_obd *cli = &req->rq_export->exp_obd->u.cli;

        oqctl = req_capsule_client_get(&req->rq_pill, &RMF_OBD_QUOTACTL);
        if (oqctl == NULL) {
                CERROR("Can't unpack obd_quotactl\n");
                RETURN(-EPROTO);
        }

        cli->cl_qchk_stat = oqctl->qc_stat;

        return 0;
}

#ifdef HAVE_QUOTA_SUPPORT
int target_handle_dqacq_callback(struct ptlrpc_request *req)
{
#ifdef __KERNEL__
        struct obd_device *obd = req->rq_export->exp_obd;
        struct obd_device *master_obd = NULL, *lov_obd = NULL;
        struct obd_device_target *obt;
        struct lustre_quota_ctxt *qctxt;
        struct qunit_data *qdata = NULL;
        int rc = 0;
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_DROP_QUOTA_REQ))
                RETURN(rc);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc) {
                CERROR("packing reply failed!: rc = %d\n", rc);
                RETURN(rc);
        }

        LASSERT(req->rq_export);

        qdata = quota_get_qdata(req, QUOTA_REQUEST, QUOTA_EXPORT);
        if (IS_ERR(qdata)) {
                rc = PTR_ERR(qdata);
                CDEBUG(D_ERROR, "Can't unpack qunit_data(rc: %d)\n", rc);
                req->rq_status = rc;
                GOTO(out, rc);
        }

        /* we use the observer */
        if (obd_pin_observer(obd, &lov_obd) ||
            obd_pin_observer(lov_obd, &master_obd)) {
                CERROR("Can't find the observer, it is recovering\n");
                req->rq_status = -EAGAIN;
                GOTO(out, rc);
        }

        obt = &master_obd->u.obt;
        qctxt = &obt->obt_qctxt;

        if (!qctxt->lqc_setup || !qctxt->lqc_valid) {
                /* quota_type has not been processed yet, return EAGAIN
                 * until we know whether or not quotas are supposed to
                 * be enabled */
                CDEBUG(D_QUOTA, "quota_type not processed yet, return "
                       "-EAGAIN\n");
                req->rq_status = -EAGAIN;
                GOTO(out, rc);
        }

        cfs_down_read(&obt->obt_rwsem);
        if (qctxt->lqc_lqs_hash == NULL) {
                cfs_up_read(&obt->obt_rwsem);
                /* quota_type has not been processed yet, return EAGAIN
                 * until we know whether or not quotas are supposed to
                 * be enabled */
                CDEBUG(D_QUOTA, "quota_ctxt is not ready yet, return "
                       "-EAGAIN\n");
                req->rq_status = -EAGAIN;
                GOTO(out, rc);
        }

        LASSERT(qctxt->lqc_handler);
        rc = qctxt->lqc_handler(master_obd, qdata,
                                lustre_msg_get_opc(req->rq_reqmsg));
        cfs_up_read(&obt->obt_rwsem);
        if (rc && rc != -EDQUOT)
                CDEBUG(rc == -EBUSY  ? D_QUOTA : D_ERROR,
                       "dqacq/dqrel failed! (rc:%d)\n", rc);
        req->rq_status = rc;

        rc = quota_copy_qdata(req, qdata, QUOTA_REPLY, QUOTA_EXPORT);
        if (rc < 0) {
                CERROR("Can't pack qunit_data(rc: %d)\n", rc);
                GOTO(out, rc);
        }

        /* Block the quota req. b=14840 */
        OBD_FAIL_TIMEOUT(OBD_FAIL_MDS_BLOCK_QUOTA_REQ, obd_timeout);
        EXIT;

out:
        if (master_obd)
                obd_unpin_observer(lov_obd);
        if (lov_obd)
                obd_unpin_observer(obd);

        rc = ptlrpc_reply(req);
        return rc;
#else
        return 0;
#endif /* !__KERNEL__ */
}
#endif /* HAVE_QUOTA_SUPPORT */

ldlm_mode_t lck_compat_array[] = {
        [LCK_EX] LCK_COMPAT_EX,
        [LCK_PW] LCK_COMPAT_PW,
        [LCK_PR] LCK_COMPAT_PR,
        [LCK_CW] LCK_COMPAT_CW,
        [LCK_CR] LCK_COMPAT_CR,
        [LCK_NL] LCK_COMPAT_NL,
        [LCK_GROUP] LCK_COMPAT_GROUP,
        [LCK_COS] LCK_COMPAT_COS,
};

/**
 * Rather arbitrary mapping from LDLM error codes to errno values. This should
 * not escape to the user level.
 */
int ldlm_error2errno(ldlm_error_t error)
{
        int result;

        switch (error) {
        case ELDLM_OK:
                result = 0;
                break;
        case ELDLM_LOCK_CHANGED:
                result = -ESTALE;
                break;
        case ELDLM_LOCK_ABORTED:
                result = -ENAVAIL;
                break;
        case ELDLM_LOCK_REPLACED:
                result = -ESRCH;
                break;
        case ELDLM_NO_LOCK_DATA:
                result = -ENOENT;
                break;
        case ELDLM_NAMESPACE_EXISTS:
                result = -EEXIST;
                break;
        case ELDLM_BAD_NAMESPACE:
                result = -EBADF;
                break;
        default:
                if (((int)error) < 0)  /* cast to signed type */
                        result = error; /* as ldlm_error_t can be unsigned */
                else {
                        CERROR("Invalid DLM result code: %d\n", error);
                        result = -EPROTO;
                }
        }
        return result;
}
EXPORT_SYMBOL(ldlm_error2errno);

/**
 * Dual to ldlm_error2errno(): maps errno values back to ldlm_error_t.
 */
ldlm_error_t ldlm_errno2error(int err_no)
{
        int error;

        switch (err_no) {
        case 0:
                error = ELDLM_OK;
                break;
        case -ESTALE:
                error = ELDLM_LOCK_CHANGED;
                break;
        case -ENAVAIL:
                error = ELDLM_LOCK_ABORTED;
                break;
        case -ESRCH:
                error = ELDLM_LOCK_REPLACED;
                break;
        case -ENOENT:
                error = ELDLM_NO_LOCK_DATA;
                break;
        case -EEXIST:
                error = ELDLM_NAMESPACE_EXISTS;
                break;
        case -EBADF:
                error = ELDLM_BAD_NAMESPACE;
                break;
        default:
                error = err_no;
        }
        return error;
}
EXPORT_SYMBOL(ldlm_errno2error);

#if LUSTRE_TRACKS_LOCK_EXP_REFS
void ldlm_dump_export_locks(struct obd_export *exp)
{
        cfs_spin_lock(&exp->exp_locks_list_guard);
        if (!cfs_list_empty(&exp->exp_locks_list)) {
            struct ldlm_lock *lock;

            CERROR("dumping locks for export %p,"
                   "ignore if the unmount doesn't hang\n", exp);
            cfs_list_for_each_entry(lock, &exp->exp_locks_list, l_exp_refs_link)
                LDLM_ERROR(lock, "lock:");
        }
        cfs_spin_unlock(&exp->exp_locks_list_guard);
}
#endif

#ifdef HAVE_SERVER_SUPPORT
static int target_bulk_timeout(void *data)
{
        ENTRY;
        /* We don't fail the connection here, because having the export
         * killed makes the (vital) call to commitrw very sad.
         */
        RETURN(1);
}

static inline char *bulk2type(struct ptlrpc_bulk_desc *desc)
{
        return desc->bd_type == BULK_GET_SINK ? "GET" : "PUT";
}

int target_bulk_io(struct obd_export *exp, struct ptlrpc_bulk_desc *desc,
                   struct l_wait_info *lwi)
{
        struct ptlrpc_request *req = desc->bd_req;
        int rc = 0;
        ENTRY;

        /* Check if there is eviction in progress, and if so, wait for
         * it to finish */
        if (unlikely(cfs_atomic_read(&exp->exp_obd->obd_evict_inprogress))) {
                *lwi = LWI_INTR(NULL, NULL);
                rc = l_wait_event(exp->exp_obd->obd_evict_inprogress_waitq,
                                  !cfs_atomic_read(&exp->exp_obd->
                                                   obd_evict_inprogress),
                                  lwi);
        }

        /* Check if client was evicted or tried to reconnect already */
        if (exp->exp_failed || exp->exp_abort_active_req) {
                rc = -ENOTCONN;
        } else {
                if (desc->bd_type == BULK_PUT_SINK)
                        rc = sptlrpc_svc_wrap_bulk(req, desc);
                if (rc == 0)
                        rc = ptlrpc_start_bulk_transfer(desc);
        }

        if (rc == 0 && OBD_FAIL_CHECK(OBD_FAIL_MDS_SENDPAGE)) {
                ptlrpc_abort_bulk(desc);
        } else if (rc == 0) {
                time_t start = cfs_time_current_sec();
                do {
                        long timeoutl = req->rq_deadline - cfs_time_current_sec();
                        cfs_duration_t timeout = timeoutl <= 0 ?
                                CFS_TICK : cfs_time_seconds(timeoutl);
                        *lwi = LWI_TIMEOUT_INTERVAL(timeout,
                                                    cfs_time_seconds(1),
                                                   target_bulk_timeout,
                                                   desc);
                        rc = l_wait_event(desc->bd_waitq,
                                          !ptlrpc_server_bulk_active(desc) ||
                                          exp->exp_failed ||
                                          exp->exp_abort_active_req,
                                          lwi);
                        LASSERT(rc == 0 || rc == -ETIMEDOUT);
                        /* Wait again if we changed deadline */
                } while ((rc == -ETIMEDOUT) &&
                         (req->rq_deadline > cfs_time_current_sec()));

                if (rc == -ETIMEDOUT) {
                        DEBUG_REQ(D_ERROR, req,
                                  "timeout on bulk %s after %ld%+lds",
                                  bulk2type(desc),
                                  req->rq_deadline - start,
                                  cfs_time_current_sec() -
                                  req->rq_deadline);
                        ptlrpc_abort_bulk(desc);
                } else if (exp->exp_failed) {
                        DEBUG_REQ(D_ERROR, req, "Eviction on bulk %s",
                                  bulk2type(desc));
                        rc = -ENOTCONN;
                        ptlrpc_abort_bulk(desc);
                } else if (exp->exp_abort_active_req) {
                        DEBUG_REQ(D_ERROR, req, "Reconnect on bulk %s",
                                  bulk2type(desc));
                        /* we don't reply anyway */
                        rc = -ETIMEDOUT;
                        ptlrpc_abort_bulk(desc);
                } else if (!desc->bd_success ||
                           desc->bd_nob_transferred != desc->bd_nob) {
                        DEBUG_REQ(D_ERROR, req, "%s bulk %s %d(%d)",
                                  desc->bd_success ?
                                  "truncated" : "network error on",
                                  bulk2type(desc),
                                  desc->bd_nob_transferred,
                                  desc->bd_nob);
                        /* XXX should this be a different errno? */
                        rc = -ETIMEDOUT;
                } else if (desc->bd_type == BULK_GET_SINK) {
                        rc = sptlrpc_svc_unwrap_bulk(req, desc);
                }
        } else {
                DEBUG_REQ(D_ERROR, req, "bulk %s failed: rc %d",
                          bulk2type(desc), rc);
        }

        RETURN(rc);
}
EXPORT_SYMBOL(target_bulk_io);

#endif /* HAVE_SERVER_SUPPORT */
