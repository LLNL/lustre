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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2012, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mgs/mgs_handler.c
 *
 * Author: Nathan Rutman <nathan@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MGS
#define D_MGS D_CONFIG

#ifdef __KERNEL__
# include <linux/module.h>
# include <linux/pagemap.h>
# include <linux/miscdevice.h>
# include <linux/init.h>
#else
# include <liblustre.h>
#endif

#include <obd_class.h>
#include <lustre_dlm.h>
#include <lprocfs_status.h>
#include <lustre_disk.h>
#include "mgs_internal.h"
#include <lustre_param.h>

/* Establish a connection to the MGS.*/
static int mgs_connect(const struct lu_env *env,
                       struct obd_export **exp, struct obd_device *obd,
                       struct obd_uuid *cluuid, struct obd_connect_data *data,
                       void *localdata)
{
        struct obd_export *lexp;
        struct lustre_handle conn = { 0 };
        int rc;
        ENTRY;

        if (!exp || !obd || !cluuid)
                RETURN(-EINVAL);

        rc = class_connect(&conn, obd, cluuid);
        if (rc)
                RETURN(rc);

        lexp = class_conn2export(&conn);
        LASSERT(lexp);

        mgs_counter_incr(lexp, LPROC_MGS_CONNECT);

        if (data != NULL) {
                data->ocd_connect_flags &= MGS_CONNECT_SUPPORTED;
                lexp->exp_connect_flags = data->ocd_connect_flags;
                data->ocd_version = LUSTRE_VERSION_CODE;
        }

        rc = mgs_export_stats_init(obd, lexp, localdata);

        if (rc) {
                class_disconnect(lexp);
        } else {
                *exp = lexp;
        }

        RETURN(rc);
}

static int mgs_reconnect(const struct lu_env *env,
                         struct obd_export *exp, struct obd_device *obd,
                         struct obd_uuid *cluuid, struct obd_connect_data *data,
                         void *localdata)
{
        ENTRY;

        if (exp == NULL || obd == NULL || cluuid == NULL)
                RETURN(-EINVAL);

        mgs_counter_incr(exp, LPROC_MGS_CONNECT);

        if (data != NULL) {
                data->ocd_connect_flags &= MGS_CONNECT_SUPPORTED;
                exp->exp_connect_flags = data->ocd_connect_flags;
                data->ocd_version = LUSTRE_VERSION_CODE;
        }

        RETURN(mgs_export_stats_init(obd, exp, localdata));
}

static int mgs_disconnect(struct obd_export *exp)
{
        int rc;
        ENTRY;

        LASSERT(exp);

        mgs_fsc_cleanup(exp);

        class_export_get(exp);
        mgs_counter_incr(exp, LPROC_MGS_DISCONNECT);

        rc = server_disconnect_export(exp);
        class_export_put(exp);
        RETURN(rc);
}

static int mgs_handle(struct ptlrpc_request *req);

static int mgs_completion_ast_config(struct ldlm_lock *lock, int flags,
                                     void *cbdata)
{
        ENTRY;

        if (!(flags & (LDLM_FL_BLOCK_WAIT | LDLM_FL_BLOCK_GRANTED |
                       LDLM_FL_BLOCK_CONV))) {
                struct fs_db *fsdb = (struct fs_db *)lock->l_ast_data;
                struct lustre_handle lockh;

                /* clear the bit before lock put */
                cfs_clear_bit(FSDB_REVOKING_LOCK, &fsdb->fsdb_flags);

                ldlm_lock2handle(lock, &lockh);
                ldlm_lock_decref_and_cancel(&lockh, LCK_EX);
        }

        RETURN(ldlm_completion_ast(lock, flags, cbdata));
}

static int mgs_completion_ast_ir(struct ldlm_lock *lock, int flags,
                                 void *cbdata)
{
        ENTRY;

        if (!(flags & (LDLM_FL_BLOCK_WAIT | LDLM_FL_BLOCK_GRANTED |
                       LDLM_FL_BLOCK_CONV))) {
                struct fs_db *fsdb;

                /* l_ast_data is used as a marker to avoid cancel ldlm lock
                 * twice. See LU-1259. */
                lock_res_and_lock(lock);
                fsdb = (struct fs_db *)lock->l_ast_data;
                lock->l_ast_data = NULL;
                unlock_res_and_lock(lock);

                if (fsdb != NULL) {
                        struct lustre_handle lockh;

                        mgs_ir_notify_complete(fsdb);

                        ldlm_lock2handle(lock, &lockh);
                        ldlm_lock_decref_and_cancel(&lockh, LCK_EX);
                }
        }

        RETURN(ldlm_completion_ast(lock, flags, cbdata));
}

void mgs_revoke_lock(struct mgs_device *mgs, struct fs_db *fsdb, int type)
{
        ldlm_completion_callback cp = NULL;
        struct lustre_handle     lockh = { 0 };
        struct ldlm_res_id       res_id;
        int flags = LDLM_FL_ATOMIC_CB;
        int rc;
        ENTRY;

        LASSERT(fsdb->fsdb_name[0] != '\0');
        rc = mgc_fsname2resid(fsdb->fsdb_name, &res_id, type);
        LASSERT(rc == 0);

        switch (type) {
        case CONFIG_T_CONFIG:
                cp = mgs_completion_ast_config;
                if (cfs_test_and_set_bit(FSDB_REVOKING_LOCK, &fsdb->fsdb_flags))
                        rc = -EALREADY;
                break;
        case CONFIG_T_RECOVER:
                cp = mgs_completion_ast_ir;
        default:
                break;
        }

        if (!rc) {
                LASSERT(cp != NULL);
                rc = ldlm_cli_enqueue_local(mgs->mgs_obd->obd_namespace,
                                            &res_id, LDLM_PLAIN, NULL, LCK_EX,
                                            &flags, ldlm_blocking_ast, cp,
                                            NULL, fsdb, 0, NULL, &lockh);
                if (rc != ELDLM_OK) {
                        CERROR("can't take cfg lock for "LPX64"/"LPX64"(%d)\n",
                               le64_to_cpu(res_id.name[0]),
                               le64_to_cpu(res_id.name[1]), rc);

                        if (type == CONFIG_T_CONFIG)
                                cfs_clear_bit(FSDB_REVOKING_LOCK,
                                              &fsdb->fsdb_flags);
                }
                /* lock has been cancelled in completion_ast. */
        }

        RETURN_EXIT;
}

/* rc=0 means ok
      1 means update
     <0 means error */
static int mgs_check_target(const struct lu_env *env,
                            struct mgs_device *mgs,
                            struct mgs_target_info *mti)
{
        int rc;
        ENTRY;

        rc = mgs_check_index(env, mgs, mti);
        if (rc == 0) {
                LCONSOLE_ERROR_MSG(0x13b, "%s claims to have registered, but "
                                   "this MGS does not know about it.  Use '-o "
                                   "writeconf' to re-register.\n",
                                   mti->mti_svname);
                rc = -ENOENT;
        } else if (rc == -1) {
                /* Writeconf was not specified, but no client log. */
                LCONSOLE_ERROR_MSG(0x13c, "Client log %s-client is "
                                   "missing. All servers must be restarted "
                                   "with '-o writeconf'.\n",
                                   mti->mti_fsname);
                rc = -ENOENT;
        } else {
                /* Index is correctly marked as used */

                /* If the logs don't contain the mti_nids then add
                   them as failover nids */
                rc = mgs_check_failnid(env, mgs, mti);
        }

        RETURN(rc);
}

/* Ensure this is not a failover node that is connecting first*/
static int mgs_check_failover_reg(struct mgs_target_info *mti)
{
        lnet_nid_t nid;
        char *ptr;
        int i;

        ptr = mti->mti_params;
        while (class_find_param(ptr, PARAM_FAILNODE, &ptr) == 0) {
                while (class_parse_nid_quiet(ptr, &nid, &ptr) == 0) {
                        for (i = 0; i < mti->mti_nid_count; i++) {
                                if (nid == mti->mti_nids[i]) {
                                        LCONSOLE_WARN("Denying initial registra"
                                                      "tion attempt from nid %s"
                                                      ", specified as failover"
                                                      "\n",libcfs_nid2str(nid));
                                        return -EADDRNOTAVAIL;
                                }
                        }
                }
        }
        return 0;
}

/* Called whenever a target starts up.  Flags indicate first connect, etc. */
static int mgs_handle_target_reg(struct ptlrpc_request *req)
{
        struct obd_device *obd = req->rq_export->exp_obd;
        struct mgs_device *mgs = exp2mgs_dev(req->rq_export);
        struct lu_env     *env = req->rq_svc_thread->t_env;
        struct mgs_target_info *mti, *rep_mti;
        struct fs_db *fsdb;
        int opc;
        int rc = 0;
        ENTRY;

        mgs_counter_incr(req->rq_export, LPROC_MGS_TARGET_REG);

        mti = req_capsule_client_get(&req->rq_pill, &RMF_MGS_TARGET_INFO);

        opc = mti->mti_flags & LDD_F_OPC_MASK;
        if (opc == LDD_F_OPC_READY) {
                CDEBUG(D_MGS, "fs: %s index: %d is ready to reconnect.\n",
                       mti->mti_fsname, mti->mti_stripe_index);
                rc = mgs_ir_update(env, mgs, mti);
                if (rc) {
                        LASSERT(!(mti->mti_flags & LDD_F_IR_CAPABLE));
                        CERROR("Update IR return with %d(ignore and IR "
                               "disabled)\n", rc);
                }
                GOTO(out_nolock, rc);
        }

        /* Do not support unregistering right now. */
        if (opc != LDD_F_OPC_REG)
                GOTO(out_nolock, rc = -EINVAL);

        CDEBUG(D_MGS, "fs: %s index: %d is registered to MGS.\n",
               mti->mti_fsname, mti->mti_stripe_index);

        if (mti->mti_flags & LDD_F_NEED_INDEX)
                mti->mti_flags |= LDD_F_WRITECONF;

        if (!(mti->mti_flags & (LDD_F_WRITECONF | LDD_F_UPGRADE14 |
                                LDD_F_UPDATE))) {
                /* We're just here as a startup ping. */
                CDEBUG(D_MGS, "Server %s is running on %s\n",
                       mti->mti_svname, obd_export_nid2str(req->rq_export));
                rc = mgs_check_target(env, mgs, mti);
                /* above will set appropriate mti flags */
                if (rc <= 0)
                        /* Nothing wrong, or fatal error */
                        GOTO(out_nolock, rc);
        } else {
                if (!(mti->mti_flags & LDD_F_NO_PRIMNODE)
                    && (rc = mgs_check_failover_reg(mti)))
                        GOTO(out_nolock, rc);
        }

        OBD_FAIL_TIMEOUT(OBD_FAIL_MGS_PAUSE_TARGET_REG, 10);

        if (mti->mti_flags & LDD_F_WRITECONF) {
                if (mti->mti_flags & LDD_F_SV_TYPE_MDT &&
                    mti->mti_stripe_index == 0) {
                        rc = mgs_erase_logs(env, mgs, mti->mti_fsname);
                        LCONSOLE_WARN("%s: Logs for fs %s were removed by user "
                                      "request.  All servers must be restarted "
                                      "in order to regenerate the logs."
                                      "\n", obd->obd_name, mti->mti_fsname);
                } else if (mti->mti_flags &
                           (LDD_F_SV_TYPE_OST | LDD_F_SV_TYPE_MDT)) {
                        rc = mgs_erase_log(env, mgs, mti->mti_svname);
                        LCONSOLE_WARN("%s: Regenerating %s log by user "
                                      "request.\n",
                                      obd->obd_name, mti->mti_svname);
                }
                mti->mti_flags |= LDD_F_UPDATE;
                /* Erased logs means start from scratch. */
                mti->mti_flags &= ~LDD_F_UPGRADE14;
        }

        rc = mgs_find_or_make_fsdb(env, mgs, mti->mti_fsname, &fsdb);
        if (rc) {
                CERROR("Can't get db for %s: %d\n", mti->mti_fsname, rc);
                GOTO(out_nolock, rc);
        }

        /*
         * Log writing contention is handled by the fsdb_mutex.
         *
         * It should be alright if someone was reading while we were
         * updating the logs - if we revoke at the end they will just update
         * from where they left off.
         */

        if (mti->mti_flags & LDD_F_UPGRADE14) {
                CERROR("Can't upgrade from 1.4 (%d)\n", rc);
                GOTO(out, rc);
        }

        if (mti->mti_flags & LDD_F_UPDATE) {
                CDEBUG(D_MGS, "updating %s, index=%d\n", mti->mti_svname,
                       mti->mti_stripe_index);

                /* create or update the target log
                   and update the client/mdt logs */
                rc = mgs_write_log_target(env, mgs, mti, fsdb);
                if (rc) {
                        CERROR("Failed to write %s log (%d)\n",
                               mti->mti_svname, rc);
                        GOTO(out, rc);
                }

                mti->mti_flags &= ~(LDD_F_VIRGIN | LDD_F_UPDATE |
                                    LDD_F_NEED_INDEX | LDD_F_WRITECONF |
                                    LDD_F_UPGRADE14);
                mti->mti_flags |= LDD_F_REWRITE_LDD;
        }

out:
        mgs_revoke_lock(mgs, fsdb, CONFIG_T_CONFIG);
out_nolock:
        CDEBUG(D_MGS, "replying with %s, index=%d, rc=%d\n", mti->mti_svname,
               mti->mti_stripe_index, rc);
        req->rq_status = rc;
        if (rc)
                /* we need an error flag to tell the target what's going on,
                 * instead of just doing it by error code only. */
                mti->mti_flags |= LDD_F_ERROR;

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc)
                RETURN(rc);

        /* send back the whole mti in the reply */
        rep_mti = req_capsule_server_get(&req->rq_pill, &RMF_MGS_TARGET_INFO);
        *rep_mti = *mti;

        /* Flush logs to disk */
        dt_sync(req->rq_svc_thread->t_env, mgs->mgs_bottom);
        RETURN(rc);
}

static int mgs_set_info_rpc(struct ptlrpc_request *req)
{
        struct mgs_device *mgs = exp2mgs_dev(req->rq_export);
        struct lu_env     *env = req->rq_svc_thread->t_env;
        struct mgs_send_param *msp, *rep_msp;
        int rc;
        struct lustre_cfg_bufs bufs;
        struct lustre_cfg *lcfg;
        char fsname[MTI_NAME_MAXLEN];
        ENTRY;

        msp = req_capsule_client_get(&req->rq_pill, &RMF_MGS_SEND_PARAM);
        LASSERT(msp);

        /* Construct lustre_cfg structure to pass to function mgs_setparam */
        lustre_cfg_bufs_reset(&bufs, NULL);
        lustre_cfg_bufs_set_string(&bufs, 1, msp->mgs_param);
        lcfg = lustre_cfg_new(LCFG_PARAM, &bufs);
        rc = mgs_setparam(env, mgs, lcfg, fsname);
        if (rc) {
                CERROR("Error %d in setting the parameter %s for fs %s\n",
                       rc, msp->mgs_param, fsname);
                RETURN(rc);
        }

        lustre_cfg_free(lcfg);

        rc = req_capsule_server_pack(&req->rq_pill);
        if (rc == 0) {
                rep_msp = req_capsule_server_get(&req->rq_pill, &RMF_MGS_SEND_PARAM);
                rep_msp = msp;
        }
        RETURN(rc);
}

static int mgs_config_read(struct ptlrpc_request *req)
{
        struct mgs_config_body *body;
        int rc;
        ENTRY;

        body = req_capsule_client_get(&req->rq_pill, &RMF_MGS_CONFIG_BODY);
        if (body == NULL)
                RETURN(-EINVAL);

        switch (body->mcb_type) {
        case CONFIG_T_RECOVER:
                rc = mgs_get_ir_logs(req);
                break;

        case CONFIG_T_CONFIG:
                rc = -ENOTSUPP;
                break;

        default:
                rc = -EINVAL;
                break;
        }

        RETURN(rc);
}

/*
 * similar as in ost_connect_check_sptlrpc()
 */
static int mgs_connect_check_sptlrpc(struct ptlrpc_request *req)
{
        struct obd_export     *exp = req->rq_export;
        struct mgs_device     *mgs = exp2mgs_dev(req->rq_export);
        struct lu_env         *env = req->rq_svc_thread->t_env;
        struct fs_db          *fsdb;
        struct sptlrpc_flavor  flvr;
        int                    rc = 0;

        if (exp->exp_flvr.sf_rpc == SPTLRPC_FLVR_INVALID) {
                rc = mgs_find_or_make_fsdb(env, mgs, MGSSELF_NAME, &fsdb);
                if (rc)
                        return rc;

                cfs_mutex_lock(&fsdb->fsdb_mutex);
                if (sptlrpc_rule_set_choose(&fsdb->fsdb_srpc_gen,
                                            LUSTRE_SP_MGC, LUSTRE_SP_MGS,
                                            req->rq_peer.nid,
                                            &flvr) == 0) {
                        /* by defualt allow any flavors */
                        flvr.sf_rpc = SPTLRPC_FLVR_ANY;
                }
                cfs_mutex_unlock(&fsdb->fsdb_mutex);

                cfs_spin_lock(&exp->exp_lock);

                exp->exp_sp_peer = req->rq_sp_from;
                exp->exp_flvr = flvr;

                if (exp->exp_flvr.sf_rpc != SPTLRPC_FLVR_ANY &&
                    exp->exp_flvr.sf_rpc != req->rq_flvr.sf_rpc) {
                        CERROR("invalid rpc flavor %x, expect %x, from %s\n",
                               req->rq_flvr.sf_rpc, exp->exp_flvr.sf_rpc,
                               libcfs_nid2str(req->rq_peer.nid));
                        rc = -EACCES;
                }

                cfs_spin_unlock(&exp->exp_lock);
        } else {
                if (exp->exp_sp_peer != req->rq_sp_from) {
                        CERROR("RPC source %s doesn't match %s\n",
                               sptlrpc_part2name(req->rq_sp_from),
                               sptlrpc_part2name(exp->exp_sp_peer));
                        rc = -EACCES;
                } else {
                        rc = sptlrpc_target_export_check(exp, req);
                }
        }

        return rc;
}

/* Called whenever a target cleans up. */
/* XXX - Currently unused */
static int mgs_handle_target_del(struct ptlrpc_request *req)
{
        ENTRY;
        mgs_counter_incr(req->rq_export, LPROC_MGS_TARGET_DEL);
        RETURN(0);
}

/* XXX - Currently unused */
static int mgs_handle_exception(struct ptlrpc_request *req)
{
        ENTRY;
        mgs_counter_incr(req->rq_export, LPROC_MGS_EXCEPTION);
        RETURN(0);
}

/*
 * For old clients there is no direct way of knowing which filesystems
 * a client is operating at the MGS side. But we need to pick up those
 * clients so that the MGS can mark the corresponding filesystem as
 * non-IR capable because old clients are not ready to be notified.
 *
 * This is why we have this _hack_ function. We detect the filesystem's
 * name by hacking llog operation which is currently used by the clients
 * to fetch configuration logs. At present this is fine because this is
 * the ONLY llog operation between mgc and the MGS.
 *
 * If extra llog operation is going to be added, this function needs fixing.
 *
 * If releases prior than 2.0 are not supported, we can remove this function.
 */
static int mgs_handle_fslog_hack(struct ptlrpc_request *req)
{
        char *logname;
        char fsname[16];
        char *ptr;
        int rc;

        /* XXX: We suppose that llog at mgs is only used for
         * fetching file system log */
        logname = req_capsule_client_get(&req->rq_pill, &RMF_NAME);
        if (logname == NULL) {
                CERROR("No logname, is llog on MGS used for something else?\n");
                return -EINVAL;
        }

        ptr = strchr(logname, '-');
        rc = (int)(ptr - logname);
        if (ptr == NULL || rc >= sizeof(fsname)) {
                CERROR("Invalid logname received: %s\n", logname);
                return -EINVAL;
        }

        strncpy(fsname, logname, rc);
        fsname[rc] = 0;
        rc = mgs_fsc_attach(req->rq_svc_thread->t_env, req->rq_export, fsname);
        if (rc < 0 && rc != -EEXIST)
                CERROR("add fs client %s returns %d\n", fsname, rc);

        return rc;
}

/* TODO: handle requests in a similar way as MDT: see mdt_handle_common() */
int mgs_handle(struct ptlrpc_request *req)
{
        int fail = OBD_FAIL_MGS_ALL_REPLY_NET;
        int opc, rc = 0;
        ENTRY;

        req_capsule_init(&req->rq_pill, req, RCL_SERVER);
        CFS_FAIL_TIMEOUT_MS(OBD_FAIL_MGS_PAUSE_REQ, cfs_fail_val);
        if (CFS_FAIL_CHECK(OBD_FAIL_MGS_ALL_REQUEST_NET))
                RETURN(0);

        LASSERT(current->journal_info == NULL);
        opc = lustre_msg_get_opc(req->rq_reqmsg);

        if (opc == SEC_CTX_INIT ||
            opc == SEC_CTX_INIT_CONT ||
            opc == SEC_CTX_FINI)
                GOTO(out, rc = 0);

        if (opc != MGS_CONNECT) {
                if (!class_connected_export(req->rq_export)) {
                        DEBUG_REQ(D_MGS, req, "operation on unconnected MGS\n");
                        req->rq_status = -ENOTCONN;
                        GOTO(out, rc = -ENOTCONN);
                }
        }

        switch (opc) {
        case MGS_CONNECT:
                DEBUG_REQ(D_MGS, req, "connect");
                /* MGS and MDS have same request format for connect */
                req_capsule_set(&req->rq_pill, &RQF_MDS_CONNECT);
                rc = target_handle_connect(req);
                if (rc == 0)
                        rc = mgs_connect_check_sptlrpc(req);

                if (!rc && (lustre_msg_get_conn_cnt(req->rq_reqmsg) > 1))
                        /* Make clients trying to reconnect after a MGS restart
                           happy; also requires obd_replayable */
                        lustre_msg_add_op_flags(req->rq_repmsg,
                                                MSG_CONNECT_RECONNECT);
                break;
        case MGS_DISCONNECT:
                DEBUG_REQ(D_MGS, req, "disconnect");
                /* MGS and MDS have same request format for disconnect */
                req_capsule_set(&req->rq_pill, &RQF_MDS_DISCONNECT);
                rc = target_handle_disconnect(req);
                req->rq_status = rc;            /* superfluous? */
                break;
        case MGS_EXCEPTION:
                DEBUG_REQ(D_MGS, req, "exception");
                rc = mgs_handle_exception(req);
                break;
        case MGS_TARGET_REG:
                DEBUG_REQ(D_MGS, req, "target add");
                req_capsule_set(&req->rq_pill, &RQF_MGS_TARGET_REG);
                rc = mgs_handle_target_reg(req);
                break;
        case MGS_TARGET_DEL:
                DEBUG_REQ(D_MGS, req, "target del");
                rc = mgs_handle_target_del(req);
                break;
        case MGS_SET_INFO:
                DEBUG_REQ(D_MGS, req, "set_info");
                req_capsule_set(&req->rq_pill, &RQF_MGS_SET_INFO);
                rc = mgs_set_info_rpc(req);
                break;
        case MGS_CONFIG_READ:
                DEBUG_REQ(D_MGS, req, "read config");
                req_capsule_set(&req->rq_pill, &RQF_MGS_CONFIG_READ);
                rc = mgs_config_read(req);
                break;
        case LDLM_ENQUEUE:
                DEBUG_REQ(D_MGS, req, "enqueue");
                req_capsule_set(&req->rq_pill, &RQF_LDLM_ENQUEUE);
                rc = ldlm_handle_enqueue(req, ldlm_server_completion_ast,
                                         ldlm_server_blocking_ast, NULL);
                break;
        case LDLM_BL_CALLBACK:
        case LDLM_CP_CALLBACK:
                DEBUG_REQ(D_MGS, req, "callback");
                CERROR("callbacks should not happen on MGS\n");
                LBUG();
                break;

        case OBD_PING:
                DEBUG_REQ(D_INFO, req, "ping");
                req_capsule_set(&req->rq_pill, &RQF_OBD_PING);
                rc = target_handle_ping(req);
                break;
        case OBD_LOG_CANCEL:
                DEBUG_REQ(D_MGS, req, "log cancel");
                rc = -ENOTSUPP; /* la la la */
                break;

        case LLOG_ORIGIN_HANDLE_CREATE:
                DEBUG_REQ(D_MGS, req, "llog_init");
                req_capsule_set(&req->rq_pill, &RQF_LLOG_ORIGIN_HANDLE_CREATE);
                rc = llog_origin_handle_open(req);
                if (rc == 0)
                        (void)mgs_handle_fslog_hack(req);
                break;
        case LLOG_ORIGIN_HANDLE_NEXT_BLOCK:
                DEBUG_REQ(D_MGS, req, "llog next block");
                req_capsule_set(&req->rq_pill,
                                &RQF_LLOG_ORIGIN_HANDLE_NEXT_BLOCK);
                rc = llog_origin_handle_next_block(req);
                break;
        case LLOG_ORIGIN_HANDLE_PREV_BLOCK:
                DEBUG_REQ(D_MGS, req, "llog prev block");
                req_capsule_set(&req->rq_pill,
                                &RQF_LLOG_ORIGIN_HANDLE_PREV_BLOCK);
                rc = llog_origin_handle_prev_block(req);
                break;
        case LLOG_ORIGIN_HANDLE_READ_HEADER:
                DEBUG_REQ(D_MGS, req, "llog read header");
                req_capsule_set(&req->rq_pill,
                                &RQF_LLOG_ORIGIN_HANDLE_READ_HEADER);
                rc = llog_origin_handle_read_header(req);
                break;
        case LLOG_ORIGIN_HANDLE_CLOSE:
                DEBUG_REQ(D_MGS, req, "llog close");
                rc = llog_origin_handle_close(req);
                break;
        case LLOG_CATINFO:
                DEBUG_REQ(D_MGS, req, "llog catinfo");
                req_capsule_set(&req->rq_pill, &RQF_LLOG_CATINFO);
                rc = llog_catinfo(req);
                break;
        default:
                rc = -ENOTSUPP;
        }

        LASSERT(current->journal_info == NULL);

        if (rc) {
                CDEBUG(D_MGS, "MGS handle cmd=%d rc=%d\n", opc, rc);
                req->rq_status = rc;
                rc = ptlrpc_error(req);
                RETURN(rc);
        }

out:
        target_send_reply(req, rc, fail);
        RETURN(0);
}

static inline int mgs_init_export(struct obd_export *exp)
{
        struct mgs_export_data *data = &exp->u.eu_mgs_data;

        /* init mgs_export_data for fsc */
        cfs_spin_lock_init(&data->med_lock);
        CFS_INIT_LIST_HEAD(&data->med_clients);

        cfs_spin_lock(&exp->exp_lock);
        exp->exp_connecting = 1;
        cfs_spin_unlock(&exp->exp_lock);

        /* self-export doesn't need client data and ldlm initialization */
        if (unlikely(obd_uuid_equals(&exp->exp_obd->obd_uuid,
                                     &exp->exp_client_uuid)))
                return 0;
        return ldlm_init_export(exp);
}

static inline int mgs_destroy_export(struct obd_export *exp)
{
        ENTRY;

        target_destroy_export(exp);
        mgs_client_free(exp);

        if (unlikely(obd_uuid_equals(&exp->exp_obd->obd_uuid,
                                     &exp->exp_client_uuid)))
                RETURN(0);

        ldlm_destroy_export(exp);

        RETURN(0);
}

static int mgs_extract_fs_pool(char * arg, char *fsname, char *poolname)
{
        char *ptr;

        ENTRY;
        for (ptr = arg;  (*ptr != '\0') && (*ptr != '.'); ptr++ ) {
                *fsname = *ptr;
                fsname++;
        }
        if (*ptr == '\0')
                return -EINVAL;
        *fsname = '\0';
        ptr++;
        strcpy(poolname, ptr);

        RETURN(0);
}

static int mgs_iocontrol_pool(const struct lu_env *env,
                              struct mgs_device *mgs,
                              struct obd_ioctl_data *data)
{
        int rc;
        struct lustre_cfg *lcfg = NULL;
        struct llog_rec_hdr rec;
        char *fsname = NULL;
        char *poolname = NULL;
        ENTRY;

        OBD_ALLOC(fsname, MTI_NAME_MAXLEN);
        if (fsname == NULL)
                RETURN(-ENOMEM);

        OBD_ALLOC(poolname, LOV_MAXPOOLNAME + 1);
        if (poolname == NULL) {
                rc = -ENOMEM;
                GOTO(out_pool, rc);
        }
        rec.lrh_len = llog_data_len(data->ioc_plen1);

        if (data->ioc_type == LUSTRE_CFG_TYPE) {
                rec.lrh_type = OBD_CFG_REC;
        } else {
                CERROR("unknown cfg record type:%d \n", data->ioc_type);
                rc = -EINVAL;
                GOTO(out_pool, rc);
        }

        if (data->ioc_plen1 > CFS_PAGE_SIZE) {
                rc = -E2BIG;
                GOTO(out_pool, rc);
        }

        OBD_ALLOC(lcfg, data->ioc_plen1);
        if (lcfg == NULL)
                GOTO(out_pool, rc = -ENOMEM);

        if (cfs_copy_from_user(lcfg, data->ioc_pbuf1, data->ioc_plen1))
                GOTO(out_pool, rc = -EFAULT);

        if (lcfg->lcfg_bufcount < 2) {
                GOTO(out_pool, rc = -EFAULT);
        }

        /* first arg is always <fsname>.<poolname> */
        mgs_extract_fs_pool(lustre_cfg_string(lcfg, 1), fsname,
                            poolname);

        switch (lcfg->lcfg_command) {
        case LCFG_POOL_NEW: {
                if (lcfg->lcfg_bufcount != 2)
                        RETURN(-EINVAL);
                rc = mgs_pool_cmd(env, mgs, LCFG_POOL_NEW, fsname,
                                  poolname, NULL);
                break;
        }
        case LCFG_POOL_ADD: {
                if (lcfg->lcfg_bufcount != 3)
                        RETURN(-EINVAL);
                rc = mgs_pool_cmd(env, mgs, LCFG_POOL_ADD, fsname, poolname,
                                  lustre_cfg_string(lcfg, 2));
                break;
        }
        case LCFG_POOL_REM: {
                if (lcfg->lcfg_bufcount != 3)
                        RETURN(-EINVAL);
                rc = mgs_pool_cmd(env, mgs, LCFG_POOL_REM, fsname, poolname,
                                  lustre_cfg_string(lcfg, 2));
                break;
        }
        case LCFG_POOL_DEL: {
                if (lcfg->lcfg_bufcount != 2)
                        RETURN(-EINVAL);
                rc = mgs_pool_cmd(env, mgs, LCFG_POOL_DEL, fsname,
                                  poolname, NULL);
                break;
        }
        default: {
                 rc = -EINVAL;
                 GOTO(out_pool, rc);
        }
        }

        if (rc) {
                CERROR("OBD_IOC_POOL err %d, cmd %X for pool %s.%s\n",
                       rc, lcfg->lcfg_command, fsname, poolname);
                GOTO(out_pool, rc);
        }

out_pool:
        if (lcfg != NULL)
                OBD_FREE(lcfg, data->ioc_plen1);

        if (fsname != NULL)
                OBD_FREE(fsname, MTI_NAME_MAXLEN);

        if (poolname != NULL)
                OBD_FREE(poolname, LOV_MAXPOOLNAME + 1);

        RETURN(rc);
}

/* from mdt_iocontrol */
int mgs_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                  void *karg, void *uarg)
{
        struct mgs_device *mgs = exp2mgs_dev(exp);
        struct obd_ioctl_data *data = karg;
        struct lu_env env;
        int rc = 0;

        ENTRY;
        CDEBUG(D_IOCTL, "handling ioctl cmd %#x\n", cmd);

        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc)
                RETURN(rc);

        switch (cmd) {

        case OBD_IOC_PARAM: {
                struct lustre_cfg *lcfg;
                struct llog_rec_hdr rec;
                char fsname[MTI_NAME_MAXLEN];

                rec.lrh_len = llog_data_len(data->ioc_plen1);

                if (data->ioc_type == LUSTRE_CFG_TYPE) {
                        rec.lrh_type = OBD_CFG_REC;
                } else {
                        CERROR("unknown cfg record type:%d \n", data->ioc_type);
                        GOTO(out, rc = -EINVAL);
                }

                OBD_ALLOC(lcfg, data->ioc_plen1);
                if (lcfg == NULL)
                        GOTO(out, rc = -ENOMEM);
                if (cfs_copy_from_user(lcfg, data->ioc_pbuf1, data->ioc_plen1))
                        GOTO(out_free, rc = -EFAULT);

                if (lcfg->lcfg_bufcount < 1)
                        GOTO(out_free, rc = -EINVAL);

                rc = mgs_setparam(&env, mgs, lcfg, fsname);
                if (rc)
                        CERROR("setparam err %d\n", rc);
out_free:
                OBD_FREE(lcfg, data->ioc_plen1);
                break;
        }
        case OBD_IOC_POOL:
                rc = mgs_iocontrol_pool(&env, mgs, data);
                break;
        case OBD_IOC_DUMP_LOG: {
                struct llog_ctxt *ctxt;
                ctxt = llog_get_context(mgs->mgs_obd, LLOG_CONFIG_ORIG_CTXT);
                rc = class_config_dump_llog(&env, ctxt, data->ioc_inlbuf1, NULL);
                llog_ctxt_put(ctxt);
                break;
        }
        case OBD_IOC_CATLOGLIST:
                rc = mgs_list_logs(&env, mgs, data);
                break;
        case OBD_IOC_LLOG_CANCEL:
        case OBD_IOC_LLOG_REMOVE:
        case OBD_IOC_LLOG_CHECK:
        case OBD_IOC_LLOG_INFO:
        case OBD_IOC_LLOG_PRINT: {
                struct llog_ctxt *ctxt;
                ctxt = llog_get_context(mgs->mgs_obd, LLOG_CONFIG_ORIG_CTXT);

                rc = llog_ioctl(&env, ctxt, cmd, data);
                llog_ctxt_put(ctxt);
                break;
        }
        default:
                CERROR("%s: unknown command %#x\n",
                       mgs->mgs_obd->obd_name, cmd);
                rc = -ENOTTY;
                break;
        }

out:
        lu_env_fini(&env);
        RETURN(rc);
}

static int mgs_connect_to_osd(struct mgs_device *m, const char *nextdev)
{
        struct obd_connect_data *data = NULL;
        struct obd_device       *obd;
        int                      rc;
        ENTRY;

        OBD_ALLOC_PTR(data);
        if (data == NULL)
                GOTO(out, rc = -ENOMEM);

        obd = class_name2obd(nextdev);
        if (obd == NULL) {
                CERROR("can't locate next device: %s\n", nextdev);
                GOTO(out, rc = -ENOTCONN);
        }

        data->ocd_version = LUSTRE_VERSION_CODE;

        rc = obd_connect(NULL, &m->mgs_bottom_exp, obd,
                         &obd->obd_uuid, data, NULL);
        if (rc) {
                CERROR("cannot connect to next dev %s (%d)\n", nextdev, rc);
                GOTO(out, rc);
        }

        m->mgs_bottom = lu2dt_dev(m->mgs_bottom_exp->exp_obd->obd_lu_dev);
        m->mgs_dt_dev.dd_lu_dev.ld_site = m->mgs_bottom->dd_lu_dev.ld_site;
        LASSERT(m->mgs_dt_dev.dd_lu_dev.ld_site);
out:
        if (data)
                OBD_FREE_PTR(data);
        RETURN(rc);
}


static int mgs_init0(const struct lu_env *env, struct mgs_device *mgs,
                      struct lu_device_type *ldt, struct lustre_cfg *cfg)
{
        struct lprocfs_static_vars  lvars = { 0 };
        struct obd_device          *obd;
        struct llog_ctxt           *ctxt;
        int                         rc;
        ENTRY;

        mgs->mgs_dt_dev.dd_lu_dev.ld_ops = &mgs_lu_ops;

        rc = mgs_connect_to_osd(mgs, lustre_cfg_string(cfg, 3));
        if (rc)
                RETURN(rc);

        obd = class_name2obd(lustre_cfg_string(cfg, 0));
        LASSERT(obd);
        mgs->mgs_obd = obd;
        mgs->mgs_obd->obd_lu_dev = &mgs->mgs_dt_dev.dd_lu_dev;

        obd->u.obt.obt_magic = OBT_MAGIC;
        obd->u.obt.obt_instance = 0;

        /* namespace for mgs llog */
        obd->obd_namespace = ldlm_namespace_new(obd ,"MGS",
                                                LDLM_NAMESPACE_SERVER,
                                                LDLM_NAMESPACE_MODEST,
                                                LDLM_NS_TYPE_MGT);
        if (obd->obd_namespace == NULL)
                GOTO(err_ops, rc = -ENOMEM);

        /* ldlm setup */
        ptlrpc_init_client(LDLM_CB_REQUEST_PORTAL, LDLM_CB_REPLY_PORTAL,
                           "mgs_ldlm_client", &obd->obd_ldlm_client);

        rc = mgs_fs_setup(env, mgs);
        if (rc) {
                CERROR("%s: MGS filesystem method init failed: rc = %d\n",
                       obd->obd_name, rc);
                GOTO(err_ns, rc);
        }

        rc = llog_setup_named(obd, &obd->obd_olg, LLOG_CONFIG_ORIG_CTXT,
                              obd, 0, NULL, "CONFIGS", &llog_osd_ops);
        if (rc)
                GOTO(err_fs, rc);

        /* XXX: we need this trick till N:1 stack is supported
         *      set "current" directory for named llogs */
        ctxt = llog_get_context(mgs->mgs_obd, LLOG_CONFIG_ORIG_CTXT);
        LASSERT(ctxt);
        ctxt->loc_dir = mgs->mgs_configs_dir;
        llog_ctxt_put(ctxt);

        /* No recovery for MGC's */
        obd->obd_replayable = 0;

        /* Internal mgs setup */
        mgs_init_fsdb_list(mgs);
        cfs_sema_init(&mgs->mgs_sem, 1);
        mgs->mgs_start_time = cfs_time_current_sec();

        /* Setup proc */
        lprocfs_mgs_init_vars(&lvars);
        if (lprocfs_obd_setup(obd, lvars.obd_vars) == 0) {
                lproc_mgs_setup(mgs);
                rc = lprocfs_alloc_md_stats(obd, LPROC_MGS_LAST);
                if (rc)
                        GOTO(err_llog, rc);
        }

        /* Start the service threads */
        mgs->mgs_service =
                ptlrpc_init_svc(MGS_NBUFS, MGS_BUFSIZE, MGS_MAXREQSIZE,
                                MGS_MAXREPSIZE, MGS_REQUEST_PORTAL,
                                MGC_REPLY_PORTAL, 2,
                                mgs_handle, LUSTRE_MGS_NAME,
                                obd->obd_proc_entry, target_print_req,
                                MGS_THREADS_AUTO_MIN, MGS_THREADS_AUTO_MAX,
                                "ll_mgs", LCT_MD_THREAD, NULL);
        if (!mgs->mgs_service) {
                CERROR("failed to start service\n");
                GOTO(err_llog, rc = -ENOMEM);
        }

        rc = ptlrpc_start_threads(mgs->mgs_service);
        if (rc)
                GOTO(err_thread, rc);

        ping_evictor_start();

        CDEBUG(D_INFO, "MGS %s started\n", obd->obd_name);

        /* device stack is not yet fully setup to keep no objects behind */
        lu_site_purge(env, mgs2lu_dev(mgs)->ld_site, ~0);
        RETURN(0);

err_thread:
        ptlrpc_unregister_service(mgs->mgs_service);
err_llog:
        lproc_mgs_cleanup(mgs);
        obd_llog_finish(obd, 0);
err_fs:
        /* No extra cleanup needed for llog_init_commit_thread() */
        mgs_fs_cleanup(env, mgs);
err_ns:
        ldlm_namespace_free(obd->obd_namespace, NULL, 0);
        obd->obd_namespace = NULL;
err_ops:
        lu_site_purge(env, mgs2lu_dev(mgs)->ld_site, ~0);
        if (!cfs_hash_is_empty(mgs2lu_dev(mgs)->ld_site->ls_obj_hash)) {
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, D_ERROR, NULL);
                lu_site_print(env, mgs2lu_dev(mgs)->ld_site, &msgdata,
                              lu_cdebug_printer);
        }
        obd_disconnect(mgs->mgs_bottom_exp);
        RETURN(rc);
}

static struct lu_device *mgs_device_free(const struct lu_env *env,
                                         struct lu_device *lu)
{
        struct mgs_device *mgs = lu2mgs_dev(lu);
        ENTRY;

        dt_device_fini(&mgs->mgs_dt_dev);
        OBD_FREE_PTR(mgs);
        RETURN(NULL);
}

static int mgs_process_config(const struct lu_env *env,
                              struct lu_device *dev,
                              struct lustre_cfg *lcfg)
{
        LBUG();
        return 0;
}

static int mgs_object_init(const struct lu_env *env, struct lu_object *o,
                           const struct lu_object_conf *unused)
{
        struct mgs_device *d = lu2mgs_dev(o->lo_dev);
        struct lu_device  *under;
        struct lu_object  *below;
        int                rc = 0;
        ENTRY;

        /* do no set .do_ops as mgs calls to bottom osd directly */

        CDEBUG(D_INFO, "object init, fid = "DFID"\n",
               PFID(lu_object_fid(o)));

        under = &d->mgs_bottom->dd_lu_dev;
        below = under->ld_ops->ldo_object_alloc(env, o->lo_header, under);
        if (below != NULL)
                lu_object_add(o, below);
        else
                rc = -ENOMEM;

        return 0;
}

static void mgs_object_free(const struct lu_env *env, struct lu_object *o)
{
        struct mgs_object *obj = lu2mgs_obj(o);
        struct lu_object_header *h = o->lo_header;

        dt_object_fini(&obj->mgo_obj);
        lu_object_header_fini(h);
        OBD_FREE_PTR(obj);
}

static int mgs_object_print(const struct lu_env *env, void *cookie,
                             lu_printer_t p, const struct lu_object *l)
{
        const struct mgs_object *o = lu2mgs_obj((struct lu_object *) l);

        return (*p)(env, cookie, LUSTRE_MGS_NAME"-object@%p", o);
}

struct lu_object_operations mgs_lu_obj_ops = {
        .loo_object_init      = mgs_object_init,
        .loo_object_free      = mgs_object_free,
        .loo_object_print     = mgs_object_print,
};

struct lu_object *mgs_object_alloc(const struct lu_env *env,
                                    const struct lu_object_header *hdr,
                                    struct lu_device *d)
{
        struct lu_object_header *h;
        struct mgs_object       *o;
        struct lu_object        *l;

        LASSERT(hdr == NULL);

        OBD_ALLOC_PTR(o);
        if (o != NULL) {
                l = &o->mgo_obj.do_lu;
                h = &o->mgo_header;

                lu_object_header_init(h);
                dt_object_init(&o->mgo_obj, h, d);
                lu_object_add_top(h, l);

                l->lo_ops = &mgs_lu_obj_ops;

                return l;
        } else {
                return NULL;
        }
}

const struct lu_device_operations mgs_lu_ops = {
        .ldo_object_alloc      = mgs_object_alloc,
        .ldo_process_config    = mgs_process_config,
};

static struct lu_device *mgs_device_alloc(const struct lu_env *env,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *lcfg)
{
        struct mgs_device *m;
        struct lu_device  *l;

        OBD_ALLOC_PTR(m);
        if (m == NULL) {
                l = ERR_PTR(-ENOMEM);
        } else {
                int rc;

                l = mgs2lu_dev(m);
                dt_device_init(&m->mgs_dt_dev, t);
                rc = mgs_init0(env, m, t, lcfg);
                if (rc != 0) {
                        mgs_device_free(env, l);
                        l = ERR_PTR(rc);
                }
        }
        return l;
}

static struct lu_device *mgs_device_fini(const struct lu_env *env,
                                         struct lu_device *d)
{
        struct mgs_device *mgs = lu2mgs_dev(d);
        struct obd_device *obd = mgs->mgs_obd;
        struct llog_ctxt  *ctxt;
        int                rc;
        ENTRY;

        LASSERT(mgs->mgs_bottom);

        ping_evictor_stop();

        ptlrpc_unregister_service(mgs->mgs_service);

        obd_exports_barrier(obd);
        obd_zombie_barrier();

        mgs_cleanup_fsdb_list(mgs);
        lproc_mgs_cleanup(mgs);
        mgs_fs_cleanup(env, mgs);

        ctxt = llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT);
        if (ctxt) {
                rc = llog_cleanup(ctxt);
                if (rc)
                        CERROR("can't cleanup llog: %d\n", rc);
        }

        ldlm_namespace_free(obd->obd_namespace, NULL, 1);
        obd->obd_namespace = NULL;

        lu_site_purge(env, d->ld_site, ~0);
        if (!cfs_hash_is_empty(d->ld_site->ls_obj_hash)) {
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, D_ERROR, NULL);
                lu_site_print(env, d->ld_site, &msgdata, lu_cdebug_printer);
        }

        LASSERT(mgs->mgs_bottom_exp);
        obd_disconnect(mgs->mgs_bottom_exp);

        RETURN(NULL);
}

/* context key constructor/destructor: mgs_key_init, mgs_key_fini */
LU_KEY_INIT_FINI(mgs, struct mgs_thread_info);

LU_TYPE_INIT_FINI(mgs, &mgs_thread_key);

LU_CONTEXT_KEY_DEFINE(mgs, LCT_DT_THREAD);

static struct lu_device_type_operations mgs_device_type_ops = {
        .ldto_init           = mgs_type_init,
        .ldto_fini           = mgs_type_fini,

        .ldto_start          = mgs_type_start,
        .ldto_stop           = mgs_type_stop,

        .ldto_device_alloc   = mgs_device_alloc,
        .ldto_device_free    = mgs_device_free,

        .ldto_device_fini    = mgs_device_fini
};

static struct lu_device_type mgs_device_type = {
        .ldt_tags     = LU_DEVICE_DT,
        .ldt_name     = LUSTRE_MGS_NAME,
        .ldt_ops      = &mgs_device_type_ops,
        .ldt_ctx_tags = LCT_MD_THREAD
};


/* use obd ops to offer management infrastructure */
static struct obd_ops mgs_obd_ops = {
        .o_owner           = THIS_MODULE,
        .o_connect         = mgs_connect,
        .o_reconnect       = mgs_reconnect,
        .o_disconnect      = mgs_disconnect,
        .o_init_export     = mgs_init_export,
        .o_destroy_export  = mgs_destroy_export,
        .o_iocontrol       = mgs_iocontrol,
};

static int __init mgs_init(void)
{
        struct lprocfs_static_vars lvars;

        lprocfs_mgs_init_vars(&lvars);
        class_register_type(&mgs_obd_ops, NULL, lvars.module_vars,
                            LUSTRE_MGS_NAME, &mgs_device_type);

        return 0;
}

static void /*__exit*/ mgs_exit(void)
{
        class_unregister_type(LUSTRE_MGS_NAME);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre  Management Server (MGS)");
MODULE_LICENSE("GPL");

module_init(mgs_init);
module_exit(mgs_exit);
