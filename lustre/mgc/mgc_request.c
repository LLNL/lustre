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
 * Copyright (c) 2011, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mgc/mgc_request.c
 *
 * Author: Nathan Rutman <nathan@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_MGC
#define D_MGC D_CONFIG /*|D_WARNING*/

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
#include <lustre_log.h>
#include <lustre_fsfilt.h>
#include <lustre_disk.h>
#include "mgc_internal.h"

static int mgc_name2resid(char *name, int len, struct ldlm_res_id *res_id,
                          int type)
{
        __u64 resname = 0;

        if (len > 8) {
                CERROR("name too long: %s\n", name);
                return -EINVAL;
        }
        if (len <= 0) {
                CERROR("missing name: %s\n", name);
                return -EINVAL;
        }
        memcpy(&resname, name, len);

        /* Always use the same endianness for the resid */
        memset(res_id, 0, sizeof(*res_id));
        res_id->name[0] = cpu_to_le64(resname);
        /* XXX: unfortunately, sptlprc and config llog share one lock */
        switch(type) {
        case CONFIG_T_CONFIG:
        case CONFIG_T_SPTLRPC:
                resname = 0;
                break;
        case CONFIG_T_RECOVER:
                resname = type;
                break;
        default:
                LBUG();
        }
        res_id->name[1] = cpu_to_le64(resname);
        CDEBUG(D_MGC, "log %s to resid "LPX64"/"LPX64" (%.8s)\n", name,
               res_id->name[0], res_id->name[1], (char *)&res_id->name[0]);
        return 0;
}

int mgc_fsname2resid(char *fsname, struct ldlm_res_id *res_id, int type)
{
        /* fsname is at most 8 chars long, maybe contain "-".
         * e.g. "lustre", "SUN-000" */
        return mgc_name2resid(fsname, strlen(fsname), res_id, type);
}
EXPORT_SYMBOL(mgc_fsname2resid);

int mgc_logname2resid(char *logname, struct ldlm_res_id *res_id, int type)
{
        char *name_end;
        int len;

        /* logname consists of "fsname-nodetype".
         * e.g. "lustre-MDT0001", "SUN-000-client" */
        name_end = strrchr(logname, '-');
        LASSERT(name_end);
        len = name_end - logname;
        return mgc_name2resid(logname, len, res_id, type);
}

/********************** config llog list **********************/
static CFS_LIST_HEAD(config_llog_list);
static DEFINE_SPINLOCK(config_list_lock);

/* Take a reference to a config log */
static int config_log_get(struct config_llog_data *cld)
{
        ENTRY;
        cfs_atomic_inc(&cld->cld_refcount);
        CDEBUG(D_INFO, "log %s refs %d\n", cld->cld_logname,
               cfs_atomic_read(&cld->cld_refcount));
        RETURN(0);
}

/* Drop a reference to a config log.  When no longer referenced,
   we can free the config log data */
static void config_log_put(struct config_llog_data *cld)
{
        ENTRY;

        CDEBUG(D_INFO, "log %s refs %d\n", cld->cld_logname,
               cfs_atomic_read(&cld->cld_refcount));
        LASSERT(cfs_atomic_read(&cld->cld_refcount) > 0);

        /* spinlock to make sure no item with 0 refcount in the list */
        if (cfs_atomic_dec_and_lock(&cld->cld_refcount, &config_list_lock)) {
                cfs_list_del(&cld->cld_list_chain);
		spin_unlock(&config_list_lock);

                CDEBUG(D_MGC, "dropping config log %s\n", cld->cld_logname);

                if (cld->cld_recover)
                        config_log_put(cld->cld_recover);
                if (cld->cld_sptlrpc)
                        config_log_put(cld->cld_sptlrpc);
                if (cld_is_sptlrpc(cld))
                        sptlrpc_conf_log_stop(cld->cld_logname);

                class_export_put(cld->cld_mgcexp);
                OBD_FREE(cld, sizeof(*cld) + strlen(cld->cld_logname) + 1);
        }

        EXIT;
}

/* Find a config log by name */
static
struct config_llog_data *config_log_find(char *logname,
                                         struct config_llog_instance *cfg)
{
        struct config_llog_data *cld;
        struct config_llog_data *found = NULL;
        void *                   instance;
        ENTRY;

        LASSERT(logname != NULL);

        instance = cfg ? cfg->cfg_instance : NULL;
	spin_lock(&config_list_lock);
        cfs_list_for_each_entry(cld, &config_llog_list, cld_list_chain) {
                /* check if instance equals */
                if (instance != cld->cld_cfg.cfg_instance)
                        continue;

                /* instance may be NULL, should check name */
                if (strcmp(logname, cld->cld_logname) == 0) {
                        found = cld;
                        break;
                }
        }
        if (found) {
                cfs_atomic_inc(&found->cld_refcount);
                LASSERT(found->cld_stopping == 0 || cld_is_sptlrpc(found) == 0);
        }
	spin_unlock(&config_list_lock);
	RETURN(found);
}

static
struct config_llog_data *do_config_log_add(struct obd_device *obd,
                                           char *logname,
                                           int type,
                                           struct config_llog_instance *cfg,
                                           struct super_block *sb)
{
        struct config_llog_data *cld;
        int                      rc;
        ENTRY;

        CDEBUG(D_MGC, "do adding config log %s:%p\n", logname,
               cfg ? cfg->cfg_instance : 0);

        OBD_ALLOC(cld, sizeof(*cld) + strlen(logname) + 1);
        if (!cld)
                RETURN(ERR_PTR(-ENOMEM));

        strcpy(cld->cld_logname, logname);
        if (cfg)
                cld->cld_cfg = *cfg;
	else
		cld->cld_cfg.cfg_callback = class_config_llog_handler;
	mutex_init(&cld->cld_lock);
        cld->cld_cfg.cfg_last_idx = 0;
        cld->cld_cfg.cfg_flags = 0;
        cld->cld_cfg.cfg_sb = sb;
        cld->cld_type = type;
        cfs_atomic_set(&cld->cld_refcount, 1);

        /* Keep the mgc around until we are done */
        cld->cld_mgcexp = class_export_get(obd->obd_self_export);

        if (cld_is_sptlrpc(cld)) {
                sptlrpc_conf_log_start(logname);
                cld->cld_cfg.cfg_obdname = obd->obd_name;
        }

        rc = mgc_logname2resid(logname, &cld->cld_resid, type);

	spin_lock(&config_list_lock);
	cfs_list_add(&cld->cld_list_chain, &config_llog_list);
	spin_unlock(&config_list_lock);

        if (rc) {
                config_log_put(cld);
                RETURN(ERR_PTR(rc));
        }

        if (cld_is_sptlrpc(cld)) {
                rc = mgc_process_log(obd, cld);
		if (rc && rc != -ENOENT)
                        CERROR("failed processing sptlrpc log: %d\n", rc);
        }

        RETURN(cld);
}

static struct config_llog_data *config_recover_log_add(struct obd_device *obd,
        char *fsname,
        struct config_llog_instance *cfg,
        struct super_block *sb)
{
        struct config_llog_instance lcfg = *cfg;
        struct lustre_sb_info *lsi = s2lsi(sb);
        struct config_llog_data *cld;
        char logname[32];

	if (IS_OST(lsi))
                return NULL;

	/* for osp-on-ost, see lustre_start_osp() */
	if (IS_MDT(lsi) && lcfg.cfg_instance)
		return NULL;

        /* we have to use different llog for clients and mdts for cmd
         * where only clients are notified if one of cmd server restarts */
        LASSERT(strlen(fsname) < sizeof(logname) / 2);
        strcpy(logname, fsname);
	if (IS_SERVER(lsi)) { /* mdt */
                LASSERT(lcfg.cfg_instance == NULL);
                lcfg.cfg_instance = sb;
                strcat(logname, "-mdtir");
        } else {
                LASSERT(lcfg.cfg_instance != NULL);
                strcat(logname, "-cliir");
        }

        cld = do_config_log_add(obd, logname, CONFIG_T_RECOVER, &lcfg, sb);
        return cld;
}


/** Add this log to the list of active logs watched by an MGC.
 * Active means we're watching for updates.
 * We have one active log per "mount" - client instance or servername.
 * Each instance may be at a different point in the log.
 */
static int config_log_add(struct obd_device *obd, char *logname,
                          struct config_llog_instance *cfg,
                          struct super_block *sb)
{
        struct lustre_sb_info *lsi = s2lsi(sb);
        struct config_llog_data *cld;
        struct config_llog_data *sptlrpc_cld;
        char                     seclogname[32];
        char                    *ptr;
        ENTRY;

        CDEBUG(D_MGC, "adding config log %s:%p\n", logname, cfg->cfg_instance);

        /*
         * for each regular log, the depended sptlrpc log name is
         * <fsname>-sptlrpc. multiple regular logs may share one sptlrpc log.
         */
        ptr = strrchr(logname, '-');
        if (ptr == NULL || ptr - logname > 8) {
                CERROR("logname %s is too long\n", logname);
                RETURN(-EINVAL);
        }

        memcpy(seclogname, logname, ptr - logname);
        strcpy(seclogname + (ptr - logname), "-sptlrpc");

        sptlrpc_cld = config_log_find(seclogname, NULL);
        if (sptlrpc_cld == NULL) {
                sptlrpc_cld = do_config_log_add(obd, seclogname,
                                                CONFIG_T_SPTLRPC, NULL, NULL);
                if (IS_ERR(sptlrpc_cld)) {
                        CERROR("can't create sptlrpc log: %s\n", seclogname);
                        RETURN(PTR_ERR(sptlrpc_cld));
                }
        }

        cld = do_config_log_add(obd, logname, CONFIG_T_CONFIG, cfg, sb);
        if (IS_ERR(cld)) {
                CERROR("can't create log: %s\n", logname);
                config_log_put(sptlrpc_cld);
                RETURN(PTR_ERR(cld));
        }

        cld->cld_sptlrpc = sptlrpc_cld;

        LASSERT(lsi->lsi_lmd);
        if (!(lsi->lsi_lmd->lmd_flags & LMD_FLG_NOIR)) {
                struct config_llog_data *recover_cld;
                *strrchr(seclogname, '-') = 0;
                recover_cld = config_recover_log_add(obd, seclogname, cfg, sb);
                if (IS_ERR(recover_cld)) {
                        config_log_put(cld);
                        RETURN(PTR_ERR(recover_cld));
                }
                cld->cld_recover = recover_cld;
        }

        RETURN(0);
}

DEFINE_MUTEX(llog_process_lock);

/** Stop watching for updates on this log.
 */
static int config_log_end(char *logname, struct config_llog_instance *cfg)
{
        struct config_llog_data *cld;
        struct config_llog_data *cld_sptlrpc = NULL;
        struct config_llog_data *cld_recover = NULL;
        int rc = 0;
        ENTRY;

        cld = config_log_find(logname, cfg);
        if (cld == NULL)
                RETURN(-ENOENT);

	mutex_lock(&cld->cld_lock);
        /*
         * if cld_stopping is set, it means we didn't start the log thus
         * not owning the start ref. this can happen after previous umount:
         * the cld still hanging there waiting for lock cancel, and we
         * remount again but failed in the middle and call log_end without
         * calling start_log.
         */
        if (unlikely(cld->cld_stopping)) {
		mutex_unlock(&cld->cld_lock);
                /* drop the ref from the find */
                config_log_put(cld);
                RETURN(rc);
        }

        cld->cld_stopping = 1;

        cld_recover = cld->cld_recover;
        cld->cld_recover = NULL;
	mutex_unlock(&cld->cld_lock);

	if (cld_recover) {
		mutex_lock(&cld_recover->cld_lock);
		cld_recover->cld_stopping = 1;
		mutex_unlock(&cld_recover->cld_lock);
		config_log_put(cld_recover);
	}

	spin_lock(&config_list_lock);
	cld_sptlrpc = cld->cld_sptlrpc;
	cld->cld_sptlrpc = NULL;
	spin_unlock(&config_list_lock);

        if (cld_sptlrpc)
                config_log_put(cld_sptlrpc);

        /* drop the ref from the find */
        config_log_put(cld);
        /* drop the start ref */
        config_log_put(cld);

        CDEBUG(D_MGC, "end config log %s (%d)\n", logname ? logname : "client",
               rc);
        RETURN(rc);
}

int lprocfs_mgc_rd_ir_state(char *page, char **start, off_t off,
                            int count, int *eof, void *data)
{
        struct obd_device       *obd = data;
        struct obd_import       *imp = obd->u.cli.cl_import;
        struct obd_connect_data *ocd = &imp->imp_connect_data;
        struct config_llog_data *cld;
        int rc = 0;
        ENTRY;

        rc = snprintf(page, count, "imperative_recovery: %s\n",
		      OCD_HAS_FLAG(ocd, IMP_RECOV) ? "ENABLED" : "DISABLED");
        rc += snprintf(page + rc, count - rc, "client_state:\n");

	spin_lock(&config_list_lock);
	cfs_list_for_each_entry(cld, &config_llog_list, cld_list_chain) {
		if (cld->cld_recover == NULL)
			continue;
		rc += snprintf(page + rc, count - rc,
			       "    - { client: %s, nidtbl_version: %u }\n",
			       cld->cld_logname,
			       cld->cld_recover->cld_cfg.cfg_last_idx);
	}
	spin_unlock(&config_list_lock);

	RETURN(rc);
}

/* reenqueue any lost locks */
#define RQ_RUNNING 0x1
#define RQ_NOW     0x2
#define RQ_LATER   0x4
#define RQ_STOP    0x8
static int                    rq_state = 0;
static cfs_waitq_t            rq_waitq;
static DECLARE_COMPLETION(rq_exit);

static void do_requeue(struct config_llog_data *cld)
{
        ENTRY;
        LASSERT(cfs_atomic_read(&cld->cld_refcount) > 0);

        /* Do not run mgc_process_log on a disconnected export or an
           export which is being disconnected. Take the client
           semaphore to make the check non-racy. */
	down_read(&cld->cld_mgcexp->exp_obd->u.cli.cl_sem);
        if (cld->cld_mgcexp->exp_obd->u.cli.cl_conn_count != 0) {
                CDEBUG(D_MGC, "updating log %s\n", cld->cld_logname);
                mgc_process_log(cld->cld_mgcexp->exp_obd, cld);
        } else {
                CDEBUG(D_MGC, "disconnecting, won't update log %s\n",
                       cld->cld_logname);
        }
	up_read(&cld->cld_mgcexp->exp_obd->u.cli.cl_sem);

        EXIT;
}

/* this timeout represents how many seconds MGC should wait before
 * requeue config and recover lock to the MGS. We need to randomize this
 * in order to not flood the MGS.
 */
#define MGC_TIMEOUT_MIN_SECONDS   5
#define MGC_TIMEOUT_RAND_CENTISEC 0x1ff /* ~500 */

static int mgc_requeue_thread(void *data)
{
        char name[] = "ll_cfg_requeue";
        int rc = 0;
        ENTRY;

        cfs_daemonize(name);

        CDEBUG(D_MGC, "Starting requeue thread\n");

        /* Keep trying failed locks periodically */
	spin_lock(&config_list_lock);
	rq_state |= RQ_RUNNING;
	while (1) {
		struct l_wait_info lwi;
		struct config_llog_data *cld, *cld_prev;
		int rand = cfs_rand() & MGC_TIMEOUT_RAND_CENTISEC;
		int stopped = !!(rq_state & RQ_STOP);
		int to;

		/* Any new or requeued lostlocks will change the state */
		rq_state &= ~(RQ_NOW | RQ_LATER);
		spin_unlock(&config_list_lock);

                /* Always wait a few seconds to allow the server who
                   caused the lock revocation to finish its setup, plus some
                   random so everyone doesn't try to reconnect at once. */
                to = MGC_TIMEOUT_MIN_SECONDS * CFS_HZ;
                to += rand * CFS_HZ / 100; /* rand is centi-seconds */
                lwi = LWI_TIMEOUT(to, NULL, NULL);
                l_wait_event(rq_waitq, rq_state & RQ_STOP, &lwi);

                /*
                 * iterate & processing through the list. for each cld, process
                 * its depending sptlrpc cld firstly (if any) and then itself.
                 *
                 * it's guaranteed any item in the list must have
                 * reference > 0; and if cld_lostlock is set, at
                 * least one reference is taken by the previous enqueue.
                 */
                cld_prev = NULL;

		spin_lock(&config_list_lock);
		cfs_list_for_each_entry(cld, &config_llog_list,
					cld_list_chain) {
			if (!cld->cld_lostlock)
				continue;

			spin_unlock(&config_list_lock);

                        LASSERT(cfs_atomic_read(&cld->cld_refcount) > 0);

                        /* Whether we enqueued again or not in mgc_process_log,
                         * we're done with the ref from the old enqueue */
                        if (cld_prev)
                                config_log_put(cld_prev);
                        cld_prev = cld;

                        cld->cld_lostlock = 0;
                        if (likely(!stopped))
                                do_requeue(cld);

			spin_lock(&config_list_lock);
		}
		spin_unlock(&config_list_lock);
		if (cld_prev)
			config_log_put(cld_prev);

		/* break after scanning the list so that we can drop
		 * refcount to losing lock clds */
		if (unlikely(stopped)) {
			spin_lock(&config_list_lock);
			break;
		}

		/* Wait a bit to see if anyone else needs a requeue */
		lwi = (struct l_wait_info) { 0 };
		l_wait_event(rq_waitq, rq_state & (RQ_NOW | RQ_STOP),
			     &lwi);
		spin_lock(&config_list_lock);
	}
	/* spinlock and while guarantee RQ_NOW and RQ_LATER are not set */
	rq_state &= ~RQ_RUNNING;
	spin_unlock(&config_list_lock);

	complete(&rq_exit);

	CDEBUG(D_MGC, "Ending requeue thread\n");
	RETURN(rc);
}

/* Add a cld to the list to requeue.  Start the requeue thread if needed.
   We are responsible for dropping the config log reference from here on out. */
static void mgc_requeue_add(struct config_llog_data *cld)
{
        ENTRY;

        CDEBUG(D_INFO, "log %s: requeue (r=%d sp=%d st=%x)\n",
               cld->cld_logname, cfs_atomic_read(&cld->cld_refcount),
               cld->cld_stopping, rq_state);
        LASSERT(cfs_atomic_read(&cld->cld_refcount) > 0);

	mutex_lock(&cld->cld_lock);
	if (cld->cld_stopping || cld->cld_lostlock) {
		mutex_unlock(&cld->cld_lock);
		RETURN_EXIT;
	}
	/* this refcount will be released in mgc_requeue_thread. */
	config_log_get(cld);
	cld->cld_lostlock = 1;
	mutex_unlock(&cld->cld_lock);

	/* Hold lock for rq_state */
	spin_lock(&config_list_lock);
	if (rq_state & RQ_STOP) {
		spin_unlock(&config_list_lock);
		cld->cld_lostlock = 0;
		config_log_put(cld);
	} else {
		rq_state |= RQ_NOW;
		spin_unlock(&config_list_lock);
		cfs_waitq_signal(&rq_waitq);
	}
	EXIT;
}

/********************** class fns **********************/

static int mgc_fs_setup(struct obd_device *obd, struct super_block *sb,
                        struct vfsmount *mnt)
{
        struct lvfs_run_ctxt saved;
        struct lustre_sb_info *lsi = s2lsi(sb);
        struct client_obd *cli = &obd->u.cli;
        struct dentry *dentry;
        char *label;
        int err = 0;
        ENTRY;

        LASSERT(lsi);
        LASSERT(lsi->lsi_srv_mnt == mnt);

        /* The mgc fs exclusion sem. Only one fs can be setup at a time. */
	down(&cli->cl_mgc_sem);

        cfs_cleanup_group_info();

	obd->obd_fsops = fsfilt_get_ops(lsi->lsi_fstype);
        if (IS_ERR(obd->obd_fsops)) {
		up(&cli->cl_mgc_sem);
		CERROR("%s: No fstype %s: rc = %ld\n", lsi->lsi_fstype,
		       obd->obd_name, PTR_ERR(obd->obd_fsops));
                RETURN(PTR_ERR(obd->obd_fsops));
        }

        cli->cl_mgc_vfsmnt = mnt;
        err = fsfilt_setup(obd, mnt->mnt_sb);
        if (err)
                GOTO(err_ops, err);

        OBD_SET_CTXT_MAGIC(&obd->obd_lvfs_ctxt);
        obd->obd_lvfs_ctxt.pwdmnt = mnt;
        obd->obd_lvfs_ctxt.pwd = mnt->mnt_root;
        obd->obd_lvfs_ctxt.fs = get_ds();

        push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        dentry = ll_lookup_one_len(MOUNT_CONFIGS_DIR, cfs_fs_pwd(current->fs),
                                   strlen(MOUNT_CONFIGS_DIR));
        pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
        if (IS_ERR(dentry)) {
                err = PTR_ERR(dentry);
                CERROR("cannot lookup %s directory: rc = %d\n",
                       MOUNT_CONFIGS_DIR, err);
                GOTO(err_ops, err);
        }
        cli->cl_mgc_configs_dir = dentry;

        /* We take an obd ref to insure that we can't get to mgc_cleanup
           without calling mgc_fs_cleanup first. */
        class_incref(obd, "mgc_fs", obd);

        label = fsfilt_get_label(obd, mnt->mnt_sb);
        if (label)
                CDEBUG(D_MGC, "MGC using disk labelled=%s\n", label);

        /* We keep the cl_mgc_sem until mgc_fs_cleanup */
        RETURN(0);

err_ops:
        fsfilt_put_ops(obd->obd_fsops);
        obd->obd_fsops = NULL;
        cli->cl_mgc_vfsmnt = NULL;
	up(&cli->cl_mgc_sem);
        RETURN(err);
}

static int mgc_fs_cleanup(struct obd_device *obd)
{
        struct client_obd *cli = &obd->u.cli;
        int rc = 0;
        ENTRY;

        LASSERT(cli->cl_mgc_vfsmnt != NULL);

        if (cli->cl_mgc_configs_dir != NULL) {
                struct lvfs_run_ctxt saved;
                push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                l_dput(cli->cl_mgc_configs_dir);
                cli->cl_mgc_configs_dir = NULL;
                pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
                class_decref(obd, "mgc_fs", obd);
        }

        cli->cl_mgc_vfsmnt = NULL;
        if (obd->obd_fsops)
                fsfilt_put_ops(obd->obd_fsops);

	up(&cli->cl_mgc_sem);

        RETURN(rc);
}

static cfs_atomic_t mgc_count = CFS_ATOMIC_INIT(0);
static int mgc_precleanup(struct obd_device *obd, enum obd_cleanup_stage stage)
{
        int rc = 0;
        ENTRY;

        switch (stage) {
        case OBD_CLEANUP_EARLY:
                break;
        case OBD_CLEANUP_EXPORTS:
                if (cfs_atomic_dec_and_test(&mgc_count)) {
                        int running;
                        /* stop requeue thread */
			spin_lock(&config_list_lock);
			running = rq_state & RQ_RUNNING;
			if (running)
				rq_state |= RQ_STOP;
			spin_unlock(&config_list_lock);
			if (running) {
				cfs_waitq_signal(&rq_waitq);
				wait_for_completion(&rq_exit);
                        }
                }
                obd_cleanup_client_import(obd);
                rc = obd_llog_finish(obd, 0);
                if (rc != 0)
                        CERROR("failed to cleanup llogging subsystems\n");
                break;
        }
        RETURN(rc);
}

static int mgc_cleanup(struct obd_device *obd)
{
        struct client_obd *cli = &obd->u.cli;
        int rc;
        ENTRY;

        LASSERT(cli->cl_mgc_vfsmnt == NULL);

        /* COMPAT_146 - old config logs may have added profiles we don't
           know about */
        if (obd->obd_type->typ_refcnt <= 1)
                /* Only for the last mgc */
                class_del_profiles();

        lprocfs_obd_cleanup(obd);
        ptlrpcd_decref();

        rc = client_obd_cleanup(obd);
        RETURN(rc);
}

static int mgc_setup(struct obd_device *obd, struct lustre_cfg *lcfg)
{
        struct lprocfs_static_vars lvars;
        int rc;
        ENTRY;

        ptlrpcd_addref();

        rc = client_obd_setup(obd, lcfg);
        if (rc)
                GOTO(err_decref, rc);

        rc = obd_llog_init(obd, &obd->obd_olg, obd, NULL);
        if (rc) {
                CERROR("failed to setup llogging subsystems\n");
                GOTO(err_cleanup, rc);
        }

        lprocfs_mgc_init_vars(&lvars);
        lprocfs_obd_setup(obd, lvars.obd_vars);
        sptlrpc_lprocfs_cliobd_attach(obd);

        if (cfs_atomic_inc_return(&mgc_count) == 1) {
                rq_state = 0;
                cfs_waitq_init(&rq_waitq);

                /* start requeue thread */
                rc = cfs_create_thread(mgc_requeue_thread, NULL,
                                       CFS_DAEMON_FLAGS);
                if (rc < 0) {
                        CERROR("%s: Cannot start requeue thread (%d),"
                               "no more log updates!\n",
                               obd->obd_name, rc);
                        GOTO(err_cleanup, rc);
                }
                /* rc is the pid of mgc_requeue_thread. */
                rc = 0;
        }

        RETURN(rc);

err_cleanup:
        client_obd_cleanup(obd);
err_decref:
        ptlrpcd_decref();
        RETURN(rc);
}

/* based on ll_mdc_blocking_ast */
static int mgc_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *desc,
                            void *data, int flag)
{
        struct lustre_handle lockh;
        struct config_llog_data *cld = (struct config_llog_data *)data;
        int rc = 0;
        ENTRY;

        switch (flag) {
        case LDLM_CB_BLOCKING:
                /* mgs wants the lock, give it up... */
                LDLM_DEBUG(lock, "MGC blocking CB");
                ldlm_lock2handle(lock, &lockh);
                rc = ldlm_cli_cancel(&lockh);
                break;
        case LDLM_CB_CANCELING:
                /* We've given up the lock, prepare ourselves to update. */
                LDLM_DEBUG(lock, "MGC cancel CB");

                CDEBUG(D_MGC, "Lock res "LPX64" (%.8s)\n",
                       lock->l_resource->lr_name.name[0],
                       (char *)&lock->l_resource->lr_name.name[0]);

                if (!cld) {
                        CDEBUG(D_INFO, "missing data, won't requeue\n");
                        break;
                }

                /* held at mgc_process_log(). */
                LASSERT(cfs_atomic_read(&cld->cld_refcount) > 0);
                /* Are we done with this log? */
                if (cld->cld_stopping) {
                        CDEBUG(D_MGC, "log %s: stopping, won't requeue\n",
                               cld->cld_logname);
                        config_log_put(cld);
                        break;
                }
                /* Make sure not to re-enqueue when the mgc is stopping
                   (we get called from client_disconnect_export) */
                if (!lock->l_conn_export ||
                    !lock->l_conn_export->exp_obd->u.cli.cl_conn_count) {
                        CDEBUG(D_MGC, "log %.8s: disconnecting, won't requeue\n",
                               cld->cld_logname);
                        config_log_put(cld);
                        break;
                }

                /* Re-enqueue now */
                mgc_requeue_add(cld);
                config_log_put(cld);
                break;
        default:
                LBUG();
        }

        RETURN(rc);
}

/* Not sure where this should go... */
#define  MGC_ENQUEUE_LIMIT 50
#define  MGC_TARGET_REG_LIMIT 10
#define  MGC_SEND_PARAM_LIMIT 10

/* Send parameter to MGS*/
static int mgc_set_mgs_param(struct obd_export *exp,
                             struct mgs_send_param *msp)
{
        struct ptlrpc_request *req;
        struct mgs_send_param *req_msp, *rep_msp;
        int rc;
        ENTRY;

        req = ptlrpc_request_alloc_pack(class_exp2cliimp(exp),
                                        &RQF_MGS_SET_INFO, LUSTRE_MGS_VERSION,
                                        MGS_SET_INFO);
        if (!req)
                RETURN(-ENOMEM);

        req_msp = req_capsule_client_get(&req->rq_pill, &RMF_MGS_SEND_PARAM);
        if (!req_msp) {
                ptlrpc_req_finished(req);
                RETURN(-ENOMEM);
        }

        memcpy(req_msp, msp, sizeof(*req_msp));
        ptlrpc_request_set_replen(req);

        /* Limit how long we will wait for the enqueue to complete */
        req->rq_delay_limit = MGC_SEND_PARAM_LIMIT;
        rc = ptlrpc_queue_wait(req);
        if (!rc) {
                rep_msp = req_capsule_server_get(&req->rq_pill, &RMF_MGS_SEND_PARAM);
                memcpy(msp, rep_msp, sizeof(*rep_msp));
        }

        ptlrpc_req_finished(req);

        RETURN(rc);
}

/* Take a config lock so we can get cancel notifications */
static int mgc_enqueue(struct obd_export *exp, struct lov_stripe_md *lsm,
                       __u32 type, ldlm_policy_data_t *policy, __u32 mode,
		       __u64 *flags, void *bl_cb, void *cp_cb, void *gl_cb,
		       void *data, __u32 lvb_len, void *lvb_swabber,
		       struct lustre_handle *lockh)
{
        struct config_llog_data *cld = (struct config_llog_data *)data;
        struct ldlm_enqueue_info einfo = { type, mode, mgc_blocking_ast,
                         ldlm_completion_ast, NULL, NULL, NULL };
        struct ptlrpc_request *req;
        int short_limit = cld_is_sptlrpc(cld);
        int rc;
        ENTRY;

        CDEBUG(D_MGC, "Enqueue for %s (res "LPX64")\n", cld->cld_logname,
               cld->cld_resid.name[0]);

        /* We need a callback for every lockholder, so don't try to
           ldlm_lock_match (see rev 1.1.2.11.2.47) */
        req = ptlrpc_request_alloc_pack(class_exp2cliimp(exp),
                                        &RQF_LDLM_ENQUEUE, LUSTRE_DLM_VERSION,
                                        LDLM_ENQUEUE);
        if (req == NULL)
                RETURN(-ENOMEM);

	req_capsule_set_size(&req->rq_pill, &RMF_DLM_LVB, RCL_SERVER, 0);
        ptlrpc_request_set_replen(req);

        /* check if this is server or client */
        if (cld->cld_cfg.cfg_sb) {
                struct lustre_sb_info *lsi = s2lsi(cld->cld_cfg.cfg_sb);
		if (lsi && IS_SERVER(lsi))
                        short_limit = 1;
        }
        /* Limit how long we will wait for the enqueue to complete */
        req->rq_delay_limit = short_limit ? 5 : MGC_ENQUEUE_LIMIT;
        rc = ldlm_cli_enqueue(exp, &req, &einfo, &cld->cld_resid, NULL, flags,
			      NULL, 0, LVB_T_NONE, lockh, 0);
        /* A failed enqueue should still call the mgc_blocking_ast,
           where it will be requeued if needed ("grant failed"). */
        ptlrpc_req_finished(req);
        RETURN(rc);
}

static int mgc_cancel(struct obd_export *exp, struct lov_stripe_md *md,
                      __u32 mode, struct lustre_handle *lockh)
{
        ENTRY;

        ldlm_lock_decref(lockh, mode);

        RETURN(0);
}

static void mgc_notify_active(struct obd_device *unused)
{
	/* wakeup mgc_requeue_thread to requeue mgc lock */
	spin_lock(&config_list_lock);
	rq_state |= RQ_NOW;
	spin_unlock(&config_list_lock);
	cfs_waitq_signal(&rq_waitq);

	/* TODO: Help the MGS rebuild nidtbl. -jay */
}

/* Send target_reg message to MGS */
static int mgc_target_register(struct obd_export *exp,
                               struct mgs_target_info *mti)
{
        struct ptlrpc_request  *req;
        struct mgs_target_info *req_mti, *rep_mti;
        int                     rc;
        ENTRY;

        req = ptlrpc_request_alloc_pack(class_exp2cliimp(exp),
                                        &RQF_MGS_TARGET_REG, LUSTRE_MGS_VERSION,
                                        MGS_TARGET_REG);
        if (req == NULL)
                RETURN(-ENOMEM);

        req_mti = req_capsule_client_get(&req->rq_pill, &RMF_MGS_TARGET_INFO);
        if (!req_mti) {
                ptlrpc_req_finished(req);
                RETURN(-ENOMEM);
        }

        memcpy(req_mti, mti, sizeof(*req_mti));
        ptlrpc_request_set_replen(req);
        CDEBUG(D_MGC, "register %s\n", mti->mti_svname);
        /* Limit how long we will wait for the enqueue to complete */
        req->rq_delay_limit = MGC_TARGET_REG_LIMIT;

        rc = ptlrpc_queue_wait(req);
        if (!rc) {
                rep_mti = req_capsule_server_get(&req->rq_pill,
                                                 &RMF_MGS_TARGET_INFO);
                memcpy(mti, rep_mti, sizeof(*rep_mti));
                CDEBUG(D_MGC, "register %s got index = %d\n",
                       mti->mti_svname, mti->mti_stripe_index);
        }
        ptlrpc_req_finished(req);

        RETURN(rc);
}

int mgc_set_info_async(const struct lu_env *env, struct obd_export *exp,
                       obd_count keylen, void *key, obd_count vallen,
                       void *val, struct ptlrpc_request_set *set)
{
        int rc = -EINVAL;
        ENTRY;

        /* Turn off initial_recov after we try all backup servers once */
        if (KEY_IS(KEY_INIT_RECOV_BACKUP)) {
                struct obd_import *imp = class_exp2cliimp(exp);
                int value;
                if (vallen != sizeof(int))
                        RETURN(-EINVAL);
                value = *(int *)val;
                CDEBUG(D_MGC, "InitRecov %s %d/d%d:i%d:r%d:or%d:%s\n",
                       imp->imp_obd->obd_name, value,
                       imp->imp_deactive, imp->imp_invalid,
                       imp->imp_replayable, imp->imp_obd->obd_replayable,
                       ptlrpc_import_state_name(imp->imp_state));
                /* Resurrect if we previously died */
                if ((imp->imp_state != LUSTRE_IMP_FULL &&
                     imp->imp_state != LUSTRE_IMP_NEW) || value > 1)
                        ptlrpc_reconnect_import(imp);
                RETURN(0);
        }
        /* FIXME move this to mgc_process_config */
        if (KEY_IS(KEY_REGISTER_TARGET)) {
                struct mgs_target_info *mti;
                if (vallen != sizeof(struct mgs_target_info))
                        RETURN(-EINVAL);
                mti = (struct mgs_target_info *)val;
                CDEBUG(D_MGC, "register_target %s %#x\n",
                       mti->mti_svname, mti->mti_flags);
                rc =  mgc_target_register(exp, mti);
                RETURN(rc);
        }
        if (KEY_IS(KEY_SET_FS)) {
                struct super_block *sb = (struct super_block *)val;
                struct lustre_sb_info *lsi;
                if (vallen != sizeof(struct super_block))
                        RETURN(-EINVAL);
                lsi = s2lsi(sb);
                rc = mgc_fs_setup(exp->exp_obd, sb, lsi->lsi_srv_mnt);
                if (rc) {
                        CERROR("set_fs got %d\n", rc);
                }
                RETURN(rc);
        }
        if (KEY_IS(KEY_CLEAR_FS)) {
                if (vallen != 0)
                        RETURN(-EINVAL);
                rc = mgc_fs_cleanup(exp->exp_obd);
                if (rc) {
                        CERROR("clear_fs got %d\n", rc);
                }
                RETURN(rc);
        }
        if (KEY_IS(KEY_SET_INFO)) {
                struct mgs_send_param *msp;

                msp = (struct mgs_send_param *)val;
                rc =  mgc_set_mgs_param(exp, msp);
                RETURN(rc);
        }
        if (KEY_IS(KEY_MGSSEC)) {
                struct client_obd     *cli = &exp->exp_obd->u.cli;
                struct sptlrpc_flavor  flvr;

                /*
                 * empty string means using current flavor, if which haven't
                 * been set yet, set it as null.
                 *
                 * if flavor has been set previously, check the asking flavor
                 * must match the existing one.
                 */
                if (vallen == 0) {
                        if (cli->cl_flvr_mgc.sf_rpc != SPTLRPC_FLVR_INVALID)
                                RETURN(0);
                        val = "null";
                        vallen = 4;
                }

                rc = sptlrpc_parse_flavor(val, &flvr);
                if (rc) {
                        CERROR("invalid sptlrpc flavor %s to MGS\n",
                               (char *) val);
                        RETURN(rc);
                }

                /*
                 * caller already hold a mutex
                 */
                if (cli->cl_flvr_mgc.sf_rpc == SPTLRPC_FLVR_INVALID) {
                        cli->cl_flvr_mgc = flvr;
                } else if (memcmp(&cli->cl_flvr_mgc, &flvr,
                                  sizeof(flvr)) != 0) {
                        char    str[20];

                        sptlrpc_flavor2name(&cli->cl_flvr_mgc,
                                            str, sizeof(str));
                        LCONSOLE_ERROR("asking sptlrpc flavor %s to MGS but "
                                       "currently %s is in use\n",
                                       (char *) val, str);
                        rc = -EPERM;
                }
                RETURN(rc);
        }

        RETURN(rc);
}

static int mgc_get_info(const struct lu_env *env, struct obd_export *exp,
                        __u32 keylen, void *key, __u32 *vallen, void *val,
                        struct lov_stripe_md *unused)
{
        int rc = -EINVAL;

        if (KEY_IS(KEY_CONN_DATA)) {
                struct obd_import *imp = class_exp2cliimp(exp);
                struct obd_connect_data *data = val;

                if (*vallen == sizeof(*data)) {
                        *data = imp->imp_connect_data;
                        rc = 0;
                }
        }

        return rc;
}

static int mgc_import_event(struct obd_device *obd,
                            struct obd_import *imp,
                            enum obd_import_event event)
{
        int rc = 0;

        LASSERT(imp->imp_obd == obd);
        CDEBUG(D_MGC, "import event %#x\n", event);

        switch (event) {
        case IMP_EVENT_DISCON:
                /* MGC imports should not wait for recovery */
                break;
        case IMP_EVENT_INACTIVE:
                break;
        case IMP_EVENT_INVALIDATE: {
                struct ldlm_namespace *ns = obd->obd_namespace;
                ldlm_namespace_cleanup(ns, LDLM_FL_LOCAL_ONLY);
                break;
        }
	case IMP_EVENT_ACTIVE:
		CDEBUG(D_INFO, "%s: Reactivating import\n", obd->obd_name);
		/* Clearing obd_no_recov allows us to continue pinging */
		obd->obd_no_recov = 0;
		mgc_notify_active(obd);
		break;
        case IMP_EVENT_OCD:
                break;
        case IMP_EVENT_DEACTIVATE:
        case IMP_EVENT_ACTIVATE:
                break;
        default:
                CERROR("Unknown import event %#x\n", event);
                LBUG();
        }
        RETURN(rc);
}

static int mgc_llog_init(struct obd_device *obd, struct obd_llog_group *olg,
                         struct obd_device *tgt, int *index)
{
        struct llog_ctxt *ctxt;
        int rc;
        ENTRY;

        LASSERT(olg == &obd->obd_olg);

#ifdef HAVE_LDISKFS_OSD
	rc = llog_setup(NULL, obd, olg, LLOG_CONFIG_ORIG_CTXT, tgt,
			&llog_lvfs_ops);
	if (rc)
		RETURN(rc);
#endif

	rc = llog_setup(NULL, obd, olg, LLOG_CONFIG_REPL_CTXT, tgt,
			&llog_client_ops);
	if (rc)
		GOTO(out, rc);

	ctxt = llog_group_get_ctxt(olg, LLOG_CONFIG_REPL_CTXT);
	if (!ctxt)
		GOTO(out, rc = -ENODEV);

	llog_initiator_connect(ctxt);
	llog_ctxt_put(ctxt);

	RETURN(0);
out:
	ctxt = llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT);
	if (ctxt)
		llog_cleanup(NULL, ctxt);
        RETURN(rc);
}

static int mgc_llog_finish(struct obd_device *obd, int count)
{
	struct llog_ctxt *ctxt;

	ENTRY;

	ctxt = llog_get_context(obd, LLOG_CONFIG_REPL_CTXT);
	if (ctxt)
		llog_cleanup(NULL, ctxt);

	ctxt = llog_get_context(obd, LLOG_CONFIG_ORIG_CTXT);
	if (ctxt)
		llog_cleanup(NULL, ctxt);
	RETURN(0);
}

enum {
        CONFIG_READ_NRPAGES_INIT = 1 << (20 - CFS_PAGE_SHIFT),
        CONFIG_READ_NRPAGES      = 4
};

static int mgc_apply_recover_logs(struct obd_device *mgc,
				  struct config_llog_data *cld,
				  __u64 max_version,
				  void *data, int datalen, bool mne_swab)
{
        struct config_llog_instance *cfg = &cld->cld_cfg;
        struct lustre_sb_info       *lsi = s2lsi(cfg->cfg_sb);
        struct mgs_nidtbl_entry *entry;
        struct lustre_cfg       *lcfg;
        struct lustre_cfg_bufs   bufs;
        u64   prev_version = 0;
        char *inst;
        char *buf;
        int   bufsz;
        int   pos;
        int   rc  = 0;
        int   off = 0;
        ENTRY;

        LASSERT(cfg->cfg_instance != NULL);
        LASSERT(cfg->cfg_sb == cfg->cfg_instance);

        OBD_ALLOC(inst, CFS_PAGE_SIZE);
        if (inst == NULL)
                RETURN(-ENOMEM);

	if (!IS_SERVER(lsi)) {
                pos = sprintf(inst, "%p", cfg->cfg_instance);
        } else {
		LASSERT(IS_MDT(lsi));
		rc = server_name2svname(lsi->lsi_svname, inst, NULL);
		if (rc) {
			OBD_FREE(inst, CFS_PAGE_SIZE);
			RETURN(-EINVAL);
		}
		pos = strlen(inst);
        }

        ++pos;
        buf   = inst + pos;
        bufsz = CFS_PAGE_SIZE - pos;

        while (datalen > 0) {
                int   entry_len = sizeof(*entry);
                int   is_ost;
                struct obd_device *obd;
                char *obdname;
                char *cname;
                char *params;
                char *uuid;

                rc = -EINVAL;
                if (datalen < sizeof(*entry))
                        break;

                entry = (typeof(entry))(data + off);

                /* sanity check */
                if (entry->mne_nid_type != 0) /* only support type 0 for ipv4 */
                        break;
                if (entry->mne_nid_count == 0) /* at least one nid entry */
                        break;
                if (entry->mne_nid_size != sizeof(lnet_nid_t))
                        break;

                entry_len += entry->mne_nid_count * entry->mne_nid_size;
                if (datalen < entry_len) /* must have entry_len at least */
                        break;

		/* Keep this swab for normal mixed endian handling. LU-1644 */
		if (mne_swab)
			lustre_swab_mgs_nidtbl_entry(entry);
		if (entry->mne_length > CFS_PAGE_SIZE) {
			CERROR("MNE too large (%u)\n", entry->mne_length);
			break;
		}

		if (entry->mne_length < entry_len)
			break;

                off     += entry->mne_length;
                datalen -= entry->mne_length;
                if (datalen < 0)
                        break;

                if (entry->mne_version > max_version) {
                        CERROR("entry index(%lld) is over max_index(%lld)\n",
                               entry->mne_version, max_version);
                        break;
                }

                if (prev_version >= entry->mne_version) {
                        CERROR("index unsorted, prev %lld, now %lld\n",
                               prev_version, entry->mne_version);
                        break;
                }
                prev_version = entry->mne_version;

                /*
                 * Write a string with format "nid::instance" to
                 * lustre/<osc|mdc>/<target>-<osc|mdc>-<instance>/import.
                 */

                is_ost = entry->mne_type == LDD_F_SV_TYPE_OST;
                memset(buf, 0, bufsz);
                obdname = buf;
                pos = 0;

                /* lustre-OST0001-osc-<instance #> */
                strcpy(obdname, cld->cld_logname);
                cname = strrchr(obdname, '-');
                if (cname == NULL) {
                        CERROR("mgc %s: invalid logname %s\n",
                               mgc->obd_name, obdname);
                        break;
                }

                pos = cname - obdname;
                obdname[pos] = 0;
                pos += sprintf(obdname + pos, "-%s%04x",
                                  is_ost ? "OST" : "MDT", entry->mne_index);

                cname = is_ost ? "osc" : "mdc",
                pos += sprintf(obdname + pos, "-%s-%s", cname, inst);
                lustre_cfg_bufs_reset(&bufs, obdname);

                /* find the obd by obdname */
                obd = class_name2obd(obdname);
                if (obd == NULL) {
                        CDEBUG(D_INFO, "mgc %s: cannot find obdname %s\n",
                               mgc->obd_name, obdname);
			rc = 0;
                        /* this is a safe race, when the ost is starting up...*/
                        continue;
                }

                /* osc.import = "connection=<Conn UUID>::<target instance>" */
                ++pos;
                params = buf + pos;
                pos += sprintf(params, "%s.import=%s", cname, "connection=");
                uuid = buf + pos;

		down_read(&obd->u.cli.cl_sem);
		if (obd->u.cli.cl_import == NULL) {
			/* client does not connect to the OST yet */
			up_read(&obd->u.cli.cl_sem);
			rc = 0;
			continue;
		}

                /* TODO: iterate all nids to find one */
                /* find uuid by nid */
                rc = client_import_find_conn(obd->u.cli.cl_import,
                                             entry->u.nids[0],
                                             (struct obd_uuid *)uuid);
		up_read(&obd->u.cli.cl_sem);
                if (rc < 0) {
                        CERROR("mgc: cannot find uuid by nid %s\n",
                               libcfs_nid2str(entry->u.nids[0]));
                        break;
                }

                CDEBUG(D_INFO, "Find uuid %s by nid %s\n",
                       uuid, libcfs_nid2str(entry->u.nids[0]));

                pos += strlen(uuid);
                pos += sprintf(buf + pos, "::%u", entry->mne_instance);
                LASSERT(pos < bufsz);

                lustre_cfg_bufs_set_string(&bufs, 1, params);

                rc = -ENOMEM;
                lcfg = lustre_cfg_new(LCFG_PARAM, &bufs);
                if (lcfg == NULL) {
                        CERROR("mgc: cannot allocate memory\n");
                        break;
                }

                CDEBUG(D_INFO, "ir apply logs "LPD64"/"LPD64" for %s -> %s\n",
                       prev_version, max_version, obdname, params);

                rc = class_process_config(lcfg);
                lustre_cfg_free(lcfg);
                if (rc)
                        CDEBUG(D_INFO, "process config for %s error %d\n",
                               obdname, rc);

                /* continue, even one with error */
        }

        OBD_FREE(inst, CFS_PAGE_SIZE);
        RETURN(rc);
}

/**
 * This function is called if this client was notified for target restarting
 * by the MGS. A CONFIG_READ RPC is going to send to fetch recovery logs.
 */
static int mgc_process_recover_log(struct obd_device *obd,
                                   struct config_llog_data *cld)
{
        struct ptlrpc_request *req = NULL;
        struct config_llog_instance *cfg = &cld->cld_cfg;
        struct mgs_config_body *body;
        struct mgs_config_res  *res;
        struct ptlrpc_bulk_desc *desc;
        cfs_page_t **pages;
        int nrpages;
        bool eof = true;
	bool mne_swab = false;
        int i;
        int ealen;
        int rc;
        ENTRY;

        /* allocate buffer for bulk transfer.
         * if this is the first time for this mgs to read logs,
         * CONFIG_READ_NRPAGES_INIT will be used since it will read all logs
         * once; otherwise, it only reads increment of logs, this should be
         * small and CONFIG_READ_NRPAGES will be used.
         */
        nrpages = CONFIG_READ_NRPAGES;
        if (cfg->cfg_last_idx == 0) /* the first time */
                nrpages = CONFIG_READ_NRPAGES_INIT;

        OBD_ALLOC(pages, sizeof(*pages) * nrpages);
        if (pages == NULL)
                GOTO(out, rc = -ENOMEM);

        for (i = 0; i < nrpages; i++) {
                pages[i] = cfs_alloc_page(CFS_ALLOC_STD);
                if (pages[i] == NULL)
                        GOTO(out, rc = -ENOMEM);
        }

again:
        LASSERT(cld_is_recover(cld));
	LASSERT(mutex_is_locked(&cld->cld_lock));
        req = ptlrpc_request_alloc(class_exp2cliimp(cld->cld_mgcexp),
                                   &RQF_MGS_CONFIG_READ);
        if (req == NULL)
                GOTO(out, rc = -ENOMEM);

        rc = ptlrpc_request_pack(req, LUSTRE_MGS_VERSION, MGS_CONFIG_READ);
        if (rc)
                GOTO(out, rc);

        /* pack request */
        body = req_capsule_client_get(&req->rq_pill, &RMF_MGS_CONFIG_BODY);
        LASSERT(body != NULL);
        LASSERT(sizeof(body->mcb_name) > strlen(cld->cld_logname));
        strncpy(body->mcb_name, cld->cld_logname, sizeof(body->mcb_name));
        body->mcb_offset = cfg->cfg_last_idx + 1;
        body->mcb_type   = cld->cld_type;
        body->mcb_bits   = CFS_PAGE_SHIFT;
        body->mcb_units  = nrpages;

	/* allocate bulk transfer descriptor */
	desc = ptlrpc_prep_bulk_imp(req, nrpages, 1, BULK_PUT_SINK,
				    MGS_BULK_PORTAL);
	if (desc == NULL)
		GOTO(out, rc = -ENOMEM);

	for (i = 0; i < nrpages; i++)
		ptlrpc_prep_bulk_page_pin(desc, pages[i], 0, CFS_PAGE_SIZE);

        ptlrpc_request_set_replen(req);
        rc = ptlrpc_queue_wait(req);
        if (rc)
                GOTO(out, rc);

        res = req_capsule_server_get(&req->rq_pill, &RMF_MGS_CONFIG_RES);
        if (res->mcr_size < res->mcr_offset)
                GOTO(out, rc = -EINVAL);

        /* always update the index even though it might have errors with
         * handling the recover logs */
        cfg->cfg_last_idx = res->mcr_offset;
        eof = res->mcr_offset == res->mcr_size;

        CDEBUG(D_INFO, "Latest version "LPD64", more %d.\n",
               res->mcr_offset, eof == false);

        ealen = sptlrpc_cli_unwrap_bulk_read(req, req->rq_bulk, 0);
        if (ealen < 0)
                GOTO(out, rc = ealen);

        if (ealen > nrpages << CFS_PAGE_SHIFT)
                GOTO(out, rc = -EINVAL);

        if (ealen == 0) { /* no logs transferred */
                if (!eof)
                        rc = -EINVAL;
                GOTO(out, rc);
        }

	mne_swab = !!ptlrpc_rep_need_swab(req);
#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(3, 2, 50, 0)
	/* This import flag means the server did an extra swab of IR MNE
	 * records (fixed in LU-1252), reverse it here if needed. LU-1644 */
	if (unlikely(req->rq_import->imp_need_mne_swab))
		mne_swab = !mne_swab;
#else
#warning "LU-1644: Remove old OBD_CONNECT_MNE_SWAB fixup and imp_need_mne_swab"
#endif

        for (i = 0; i < nrpages && ealen > 0; i++) {
                int rc2;
                void *ptr;

                ptr = cfs_kmap(pages[i]);
                rc2 = mgc_apply_recover_logs(obd, cld, res->mcr_offset, ptr,
					     min_t(int, ealen, CFS_PAGE_SIZE),
					     mne_swab);
                cfs_kunmap(pages[i]);
                if (rc2 < 0) {
                        CWARN("Process recover log %s error %d\n",
                              cld->cld_logname, rc2);
                        break;
                }

                ealen -= CFS_PAGE_SIZE;
        }

out:
        if (req)
                ptlrpc_req_finished(req);

        if (rc == 0 && !eof)
                goto again;

        if (pages) {
                for (i = 0; i < nrpages; i++) {
                        if (pages[i] == NULL)
                                break;
                        cfs_free_page(pages[i]);
                }
                OBD_FREE(pages, sizeof(*pages) * nrpages);
        }
        return rc;
}

#ifdef HAVE_LDISKFS_OSD

/*
 * XXX: mgc_copy_llog() does not support osd-based llogs yet
 */

/* identical to mgs_log_is_empty */
static int mgc_llog_is_empty(struct obd_device *obd, struct llog_ctxt *ctxt,
			     char *name)
{
	struct lvfs_run_ctxt	 saved;
	struct llog_handle	*llh;
	int			 rc = 0;

	push_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
	rc = llog_open(NULL, ctxt, &llh, NULL, name, LLOG_OPEN_EXISTS);
	if (rc == 0) {
		rc = llog_init_handle(NULL, llh, LLOG_F_IS_PLAIN, NULL);
		if (rc == 0)
			rc = llog_get_size(llh);
		llog_close(NULL, llh);
	} else if (rc == -ENOENT) {
		rc = 0;
	}
	pop_ctxt(&saved, &obd->obd_lvfs_ctxt, NULL);
	/* header is record 1 */
	return (rc <= 1);
}

/* Copy a remote log locally */
static int mgc_copy_llog(struct obd_device *obd, struct llog_ctxt *rctxt,
                         struct llog_ctxt *lctxt, char *logname)
{
        struct llog_handle *local_llh, *remote_llh;
        struct obd_uuid *uuid;
        char *temp_log;
        int rc, rc2;
        ENTRY;

        /* Write new log to a temp name, then vfs_rename over logname
           upon successful completion. */

        OBD_ALLOC(temp_log, strlen(logname) + 1);
        if (!temp_log)
                RETURN(-ENOMEM);
        sprintf(temp_log, "%sT", logname);

        /* Make sure there's no old temp log */
	rc = llog_erase(NULL, lctxt, NULL, temp_log);
	if (rc < 0 && rc != -ENOENT)
		GOTO(out, rc);

	/* open local log */
	rc = llog_open_create(NULL, lctxt, &local_llh, NULL, temp_log);
	if (rc)
		GOTO(out, rc);

	/* set the log header uuid for fun */
	OBD_ALLOC_PTR(uuid);
	obd_str2uuid(uuid, logname);
	rc = llog_init_handle(NULL, local_llh, LLOG_F_IS_PLAIN, uuid);
	OBD_FREE_PTR(uuid);
	if (rc)
		GOTO(out_closel, rc);

	/* open remote log */
	rc = llog_open(NULL, rctxt, &remote_llh, NULL, logname,
		       LLOG_OPEN_EXISTS);
	if (rc < 0) {
		if (rc == -ENOENT)
			rc = 0;
		GOTO(out_closel, rc);
	}

	rc = llog_init_handle(NULL, remote_llh, LLOG_F_IS_PLAIN, NULL);
	if (rc)
		GOTO(out_closer, rc);

	/* Copy remote log */
	rc = llog_process(NULL, remote_llh, llog_copy_handler,
			  (void *)local_llh, NULL);

out_closer:
	rc2 = llog_close(NULL, remote_llh);
	if (!rc)
		rc = rc2;
out_closel:
	rc2 = llog_close(NULL, local_llh);
        if (!rc)
                rc = rc2;

        /* We've copied the remote log to the local temp log, now
           replace the old local log with the temp log. */
	if (rc == 0) {
                struct client_obd *cli = &obd->u.cli;

                LASSERT(cli);
                LASSERT(cli->cl_mgc_configs_dir);
                rc = lustre_rename(cli->cl_mgc_configs_dir, cli->cl_mgc_vfsmnt,
                                   temp_log, logname);
        }
        CDEBUG(D_MGC, "Copied remote log %s (%d)\n", logname, rc);
out:
        if (rc)
                CERROR("Failed to copy remote log %s (%d)\n", logname, rc);
        OBD_FREE(temp_log, strlen(logname) + 1);
        RETURN(rc);
}
#endif

/* local_only means it cannot get remote llogs */
static int mgc_process_cfg_log(struct obd_device *mgc,
                               struct config_llog_data *cld,
                               int local_only)
{
        struct llog_ctxt *ctxt, *lctxt = NULL;
#ifdef HAVE_LDISKFS_OSD
        struct client_obd *cli = &mgc->u.cli;
#endif
        struct lvfs_run_ctxt *saved_ctxt;
        struct lustre_sb_info *lsi = NULL;
        int rc = 0, must_pop = 0;
        bool sptlrpc_started = false;

        ENTRY;

        LASSERT(cld);
	LASSERT(mutex_is_locked(&cld->cld_lock));

        /*
         * local copy of sptlrpc log is controlled elsewhere, don't try to
         * read it up here.
         */
        if (cld_is_sptlrpc(cld) && local_only)
                RETURN(0);

        if (cld->cld_cfg.cfg_sb)
                lsi = s2lsi(cld->cld_cfg.cfg_sb);

        ctxt = llog_get_context(mgc, LLOG_CONFIG_REPL_CTXT);
        if (!ctxt) {
                CERROR("missing llog context\n");
                RETURN(-EINVAL);
        }

        OBD_ALLOC_PTR(saved_ctxt);
        if (saved_ctxt == NULL)
                RETURN(-ENOMEM);

        lctxt = llog_get_context(mgc, LLOG_CONFIG_ORIG_CTXT);

#ifdef HAVE_LDISKFS_OSD
	/*
	 * XXX: at the moment mgc_copy_llog() works with lvfs-based llogs
	 */
        /* Copy the setup log locally if we can. Don't mess around if we're
           running an MGS though (logs are already local). */
	if (lctxt && lsi && IS_SERVER(lsi) &&
            (lsi->lsi_srv_mnt == cli->cl_mgc_vfsmnt) &&
	    !IS_MGS(lsi) && lsi->lsi_srv_mnt) {
                push_ctxt(saved_ctxt, &mgc->obd_lvfs_ctxt, NULL);
                must_pop++;
                if (!local_only)
                        /* Only try to copy log if we have the lock. */
                        rc = mgc_copy_llog(mgc, ctxt, lctxt, cld->cld_logname);
                if (local_only || rc) {
                        if (mgc_llog_is_empty(mgc, lctxt, cld->cld_logname)) {
                                LCONSOLE_ERROR_MSG(0x13a, "Failed to get MGS "
                                                   "log %s and no local copy."
                                                   "\n", cld->cld_logname);
                                GOTO(out_pop, rc = -ENOTCONN);
                        }
                        CDEBUG(D_MGC, "Failed to get MGS log %s, using local "
                               "copy for now, will try to update later.\n",
                               cld->cld_logname);
                }
                /* Now, whether we copied or not, start using the local llog.
                   If we failed to copy, we'll start using whatever the old
                   log has. */
                llog_ctxt_put(ctxt);
                ctxt = lctxt;
                lctxt = NULL;
	} else
#endif
		if (local_only) { /* no local log at client side */
                GOTO(out_pop, rc = -EIO);
        }

        if (cld_is_sptlrpc(cld)) {
                sptlrpc_conf_log_update_begin(cld->cld_logname);
                sptlrpc_started = true;
        }

        /* logname and instance info should be the same, so use our
           copy of the instance for the update.  The cfg_last_idx will
           be updated here. */
	rc = class_config_parse_llog(NULL, ctxt, cld->cld_logname,
				     &cld->cld_cfg);
        EXIT;

out_pop:
        llog_ctxt_put(ctxt);
        if (lctxt)
                llog_ctxt_put(lctxt);
        if (must_pop)
                pop_ctxt(saved_ctxt, &mgc->obd_lvfs_ctxt, NULL);

        OBD_FREE_PTR(saved_ctxt);
        /*
         * update settings on existing OBDs. doing it inside
         * of llog_process_lock so no device is attaching/detaching
         * in parallel.
         * the logname must be <fsname>-sptlrpc
         */
        if (sptlrpc_started) {
                LASSERT(cld_is_sptlrpc(cld));
                sptlrpc_conf_log_update_end(cld->cld_logname);
                class_notify_sptlrpc_conf(cld->cld_logname,
                                          strlen(cld->cld_logname) -
                                          strlen("-sptlrpc"));
        }

        RETURN(rc);
}

/** Get a config log from the MGS and process it.
 * This func is called for both clients and servers.
 * Copy the log locally before parsing it if appropriate (non-MGS server)
 */
int mgc_process_log(struct obd_device *mgc, struct config_llog_data *cld)
{
        struct lustre_handle lockh = { 0 };
	__u64 flags = LDLM_FL_NO_LRU;
	int rc = 0, rcl;
        ENTRY;

        LASSERT(cld);

        /* I don't want multiple processes running process_log at once --
           sounds like badness.  It actually might be fine, as long as
           we're not trying to update from the same log
           simultaneously (in which case we should use a per-log sem.) */
	mutex_lock(&cld->cld_lock);
	if (cld->cld_stopping) {
		mutex_unlock(&cld->cld_lock);
                RETURN(0);
        }

        OBD_FAIL_TIMEOUT(OBD_FAIL_MGC_PAUSE_PROCESS_LOG, 20);

        CDEBUG(D_MGC, "Process log %s:%p from %d\n", cld->cld_logname,
               cld->cld_cfg.cfg_instance, cld->cld_cfg.cfg_last_idx + 1);

        /* Get the cfg lock on the llog */
        rcl = mgc_enqueue(mgc->u.cli.cl_mgc_mgsexp, NULL, LDLM_PLAIN, NULL,
                          LCK_CR, &flags, NULL, NULL, NULL,
                          cld, 0, NULL, &lockh);
        if (rcl == 0) {
                /* Get the cld, it will be released in mgc_blocking_ast. */
                config_log_get(cld);
                rc = ldlm_lock_set_data(&lockh, (void *)cld);
                LASSERT(rc == 0);
        } else {
                CDEBUG(D_MGC, "Can't get cfg lock: %d\n", rcl);

                /* mark cld_lostlock so that it will requeue
                 * after MGC becomes available. */
                cld->cld_lostlock = 1;
                /* Get extra reference, it will be put in requeue thread */
                config_log_get(cld);
        }


        if (cld_is_recover(cld)) {
                rc = 0; /* this is not a fatal error for recover log */
                if (rcl == 0)
                        rc = mgc_process_recover_log(mgc, cld);
        } else {
                rc = mgc_process_cfg_log(mgc, cld, rcl != 0);
        }

        CDEBUG(D_MGC, "%s: configuration from log '%s' %sed (%d).\n",
               mgc->obd_name, cld->cld_logname, rc ? "fail" : "succeed", rc);

	mutex_unlock(&cld->cld_lock);

        /* Now drop the lock so MGS can revoke it */
        if (!rcl) {
                rcl = mgc_cancel(mgc->u.cli.cl_mgc_mgsexp, NULL,
                                 LCK_CR, &lockh);
                if (rcl)
                        CERROR("Can't drop cfg lock: %d\n", rcl);
        }

        RETURN(rc);
}


/** Called from lustre_process_log.
 * LCFG_LOG_START gets the config log from the MGS, processes it to start
 * any services, and adds it to the list logs to watch (follow).
 */
static int mgc_process_config(struct obd_device *obd, obd_count len, void *buf)
{
        struct lustre_cfg *lcfg = buf;
        struct config_llog_instance *cfg = NULL;
        char *logname;
        int rc = 0;
        ENTRY;

        switch(lcfg->lcfg_command) {
        case LCFG_LOV_ADD_OBD: {
                /* Overloading this cfg command: register a new target */
                struct mgs_target_info *mti;

                if (LUSTRE_CFG_BUFLEN(lcfg, 1) !=
                    sizeof(struct mgs_target_info))
                        GOTO(out, rc = -EINVAL);

                mti = (struct mgs_target_info *)lustre_cfg_buf(lcfg, 1);
                CDEBUG(D_MGC, "add_target %s %#x\n",
                       mti->mti_svname, mti->mti_flags);
                rc = mgc_target_register(obd->u.cli.cl_mgc_mgsexp, mti);
                break;
        }
        case LCFG_LOV_DEL_OBD:
                /* Unregister has no meaning at the moment. */
                CERROR("lov_del_obd unimplemented\n");
                rc = -ENOSYS;
                break;
        case LCFG_SPTLRPC_CONF: {
                rc = sptlrpc_process_config(lcfg);
                break;
        }
        case LCFG_LOG_START: {
                struct config_llog_data *cld;
                struct super_block *sb;

                logname = lustre_cfg_string(lcfg, 1);
                cfg = (struct config_llog_instance *)lustre_cfg_buf(lcfg, 2);
                sb = *(struct super_block **)lustre_cfg_buf(lcfg, 3);

                CDEBUG(D_MGC, "parse_log %s from %d\n", logname,
                       cfg->cfg_last_idx);

                /* We're only called through here on the initial mount */
                rc = config_log_add(obd, logname, cfg, sb);
                if (rc)
                        break;
                cld = config_log_find(logname, cfg);
                if (cld == NULL) {
                        rc = -ENOENT;
                        break;
                }

                /* COMPAT_146 */
                /* FIXME only set this for old logs!  Right now this forces
                   us to always skip the "inside markers" check */
                cld->cld_cfg.cfg_flags |= CFG_F_COMPAT146;

                rc = mgc_process_log(obd, cld);
                if (rc == 0 && cld->cld_recover != NULL) {
                        if (OCD_HAS_FLAG(&obd->u.cli.cl_import->
                                         imp_connect_data, IMP_RECOV)) {
                                rc = mgc_process_log(obd, cld->cld_recover);
                        } else {
                                struct config_llog_data *cir = cld->cld_recover;
                                cld->cld_recover = NULL;
                                config_log_put(cir);
                        }
                        if (rc)
                                CERROR("Cannot process recover llog %d\n", rc);
                }
                config_log_put(cld);

                break;
        }
        case LCFG_LOG_END: {
                logname = lustre_cfg_string(lcfg, 1);

                if (lcfg->lcfg_bufcount >= 2)
                        cfg = (struct config_llog_instance *)lustre_cfg_buf(
                                lcfg, 2);
                rc = config_log_end(logname, cfg);
                break;
        }
        default: {
                CERROR("Unknown command: %d\n", lcfg->lcfg_command);
                GOTO(out, rc = -EINVAL);

        }
        }
out:
        RETURN(rc);
}

struct obd_ops mgc_obd_ops = {
        .o_owner        = THIS_MODULE,
        .o_setup        = mgc_setup,
        .o_precleanup   = mgc_precleanup,
        .o_cleanup      = mgc_cleanup,
        .o_add_conn     = client_import_add_conn,
        .o_del_conn     = client_import_del_conn,
        .o_connect      = client_connect_import,
        .o_disconnect   = client_disconnect_export,
        //.o_enqueue      = mgc_enqueue,
        .o_cancel       = mgc_cancel,
        //.o_iocontrol    = mgc_iocontrol,
        .o_set_info_async = mgc_set_info_async,
        .o_get_info       = mgc_get_info,
        .o_import_event = mgc_import_event,
        .o_llog_init    = mgc_llog_init,
        .o_llog_finish  = mgc_llog_finish,
        .o_process_config = mgc_process_config,
};

int __init mgc_init(void)
{
        return class_register_type(&mgc_obd_ops, NULL, NULL,
                                   LUSTRE_MGC_NAME, NULL);
}

#ifdef __KERNEL__
static void /*__exit*/ mgc_exit(void)
{
        class_unregister_type(LUSTRE_MGC_NAME);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Management Client");
MODULE_LICENSE("GPL");

module_init(mgc_init);
module_exit(mgc_exit);
#endif
