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
 * Copyright (c) 2011, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/version.h>
#include <asm/statfs.h>
#include <obd.h>
#include <obd_class.h>
#include <lprocfs_status.h>
#include <lustre_param.h>
#include "mgs_internal.h"

#ifdef LPROCFS

static int mgs_fs_seq_show(struct seq_file *seq, void *v)
{
        struct obd_device *obd = seq->private;
	struct mgs_device *mgs;
	cfs_list_t list;
	struct mgs_direntry *dirent, *n;
	struct lu_env env;
        int rc, len;
        ENTRY;

        LASSERT(obd != NULL);
	LASSERT(obd->obd_lu_dev != NULL);
	mgs = lu2mgs_dev(obd->obd_lu_dev);

	rc = lu_env_init(&env, LCT_MG_THREAD);
	if (rc)
		RETURN(rc);

	rc = class_dentry_readdir(&env, mgs, &list);
        if (rc) {
                CERROR("Can't read config dir\n");
		GOTO(out, rc);
        }
	cfs_list_for_each_entry_safe(dirent, n, &list, list) {
		cfs_list_del(&dirent->list);
		len = strlen(dirent->name);
		if (len > 7 &&
		    strncmp(dirent->name + len - 7, "-client", len) == 0)
			seq_printf(seq, "%.*s\n", len - 7, dirent->name);
		mgs_direntry_free(dirent);
        }

out:
	lu_env_fini(&env);
        RETURN(0);
}

LPROC_SEQ_FOPS_RO(mgs_fs);

static void seq_show_srpc_rules(struct seq_file *seq, const char *tgtname,
                                struct sptlrpc_rule_set *rset)
{
        struct sptlrpc_rule    *r;
        char                    dirbuf[10];
        char                    flvrbuf[40];
        char                   *net;
        int                     i;

        for (i = 0; i < rset->srs_nrule; i++) {
                r = &rset->srs_rules[i];

                if (r->sr_netid == LNET_NIDNET(LNET_NID_ANY))
                        net = "default";
                else
                        net = libcfs_net2str(r->sr_netid);

                if (r->sr_from == LUSTRE_SP_ANY && r->sr_to == LUSTRE_SP_ANY)
                        dirbuf[0] = '\0';
                else
                        snprintf(dirbuf, sizeof(dirbuf), ".%s2%s",
                                 sptlrpc_part2name(r->sr_from),
                                 sptlrpc_part2name(r->sr_to));

                sptlrpc_flavor2name(&r->sr_flvr, flvrbuf, sizeof(flvrbuf));
                seq_printf(seq, "%s.srpc.flavor.%s%s=%s\n", tgtname,
                           net, dirbuf, flvrbuf);
        }
}

static int mgsself_srpc_seq_show(struct seq_file *seq, void *v)
{
	struct obd_device	*obd = seq->private;
	struct mgs_device	*mgs = lu2mgs_dev(obd->obd_lu_dev);
	struct lu_target	*tgt = &mgs->mgs_lut;

	read_lock(&tgt->lut_sptlrpc_lock);
	seq_show_srpc_rules(seq, MGSSELF_NAME, &tgt->lut_sptlrpc_rset);
	read_unlock(&tgt->lut_sptlrpc_lock);

        return 0;
}

LPROC_SEQ_FOPS_RO(mgsself_srpc);
LPROC_SEQ_FOPS_RO(lprocfs_obd_hash);

int lproc_mgs_setup(struct mgs_device *mgs, const char *osd_name)
{
	struct obd_device *obd = mgs->mgs_obd;
	struct obd_device *osd_obd = mgs->mgs_bottom->dd_lu_dev.ld_obd;
	int		   osd_len = strlen(osd_name) - strlen("-osd");
	int		   rc;
	struct lprocfs_static_vars lvars;

	lprocfs_mgs_init_vars(&lvars);
	rc = lprocfs_obd_setup(obd, lvars.obd_vars);
	if (rc != 0)
		GOTO(out, rc);

        rc = lprocfs_obd_seq_create(obd, "filesystems", 0444,
                                    &mgs_fs_fops, obd);
	if (rc != 0)
		GOTO(out, rc);

	rc = lprocfs_obd_seq_create(obd, "srpc_rules", 0400,
				    &mgsself_srpc_fops, obd);
	if (rc != 0)
		GOTO(out, rc);

	rc = lprocfs_obd_seq_create(obd, "hash_stats", 0400,
				    &lprocfs_obd_hash_fops, obd);
	if (rc != 0)
		GOTO(out, rc);

        mgs->mgs_proc_live = lprocfs_register("live", obd->obd_proc_entry,
                                              NULL, NULL);
        if (IS_ERR(mgs->mgs_proc_live)) {
                rc = PTR_ERR(mgs->mgs_proc_live);
                mgs->mgs_proc_live = NULL;
		GOTO(out, rc);
        }

        obd->obd_proc_exports_entry = lprocfs_register("exports",
                                                       obd->obd_proc_entry,
                                                       NULL, NULL);
        if (IS_ERR(obd->obd_proc_exports_entry)) {
                rc = PTR_ERR(obd->obd_proc_exports_entry);
                obd->obd_proc_exports_entry = NULL;
		GOTO(out, rc);
        }

	mgs->mgs_proc_osd = lprocfs_add_symlink("osd",
						obd->obd_proc_entry,
						"../../%s/%.*s",
						osd_obd->obd_type->typ_name,
						osd_len, /* Strip "-osd". */
						osd_name);
	if (mgs->mgs_proc_osd == NULL)
		GOTO(out, rc = -ENOMEM);

	mgs->mgs_proc_mntdev = lprocfs_add_symlink("mntdev",
						   obd->obd_proc_entry,
						   "osd/mntdev");
	if (mgs->mgs_proc_mntdev == NULL)
		GOTO(out, rc = -ENOMEM);

	mgs->mgs_proc_fstype = lprocfs_add_symlink("fstype",
						   obd->obd_proc_entry,
						   "osd/fstype");
	if (mgs->mgs_proc_fstype == NULL)
		GOTO(out, rc = -ENOMEM);

out:
	if (rc != 0)
		lproc_mgs_cleanup(mgs);

	return rc;
}

void lproc_mgs_cleanup(struct mgs_device *mgs)
{
	struct obd_device *obd = mgs->mgs_obd;

	if (obd == NULL)
		return;

	if (mgs->mgs_proc_osd != NULL)
		lprocfs_remove(&mgs->mgs_proc_osd);

	if (mgs->mgs_proc_fstype != NULL)
		lprocfs_remove(&mgs->mgs_proc_fstype);

	if (mgs->mgs_proc_mntdev != NULL)
		lprocfs_remove(&mgs->mgs_proc_mntdev);

	if (mgs->mgs_proc_live != NULL) {
		/* Should be no live entries */
		LASSERT(mgs->mgs_proc_live->subdir == NULL);
		lprocfs_remove(&mgs->mgs_proc_live);
		mgs->mgs_proc_live = NULL;
	}

        lprocfs_free_per_client_stats(obd);
	lprocfs_obd_cleanup(obd);
        lprocfs_free_obd_stats(obd);
        lprocfs_free_md_stats(obd);
}

static int mgs_live_seq_show(struct seq_file *seq, void *v)
{
        struct fs_db             *fsdb = seq->private;
        struct mgs_tgt_srpc_conf *srpc_tgt;
        int i;

	mutex_lock(&fsdb->fsdb_mutex);

        seq_printf(seq, "fsname: %s\n", fsdb->fsdb_name);
        seq_printf(seq, "flags: %#lx     gen: %d\n",
                   fsdb->fsdb_flags, fsdb->fsdb_gen);
        for (i = 0; i < INDEX_MAP_SIZE * 8; i++)
		if (test_bit(i, fsdb->fsdb_mdt_index_map))
                         seq_printf(seq, "%s-MDT%04x\n", fsdb->fsdb_name, i);
        for (i = 0; i < INDEX_MAP_SIZE * 8; i++)
		if (test_bit(i, fsdb->fsdb_ost_index_map))
                         seq_printf(seq, "%s-OST%04x\n", fsdb->fsdb_name, i);

        seq_printf(seq, "\nSecure RPC Config Rules:\n");
#if 0
        seq_printf(seq, "%s.%s=%s\n", fsdb->fsdb_name,
                   PARAM_SRPC_UDESC, fsdb->fsdb_srpc_fl_udesc ? "yes" : "no");
#endif
        for (srpc_tgt = fsdb->fsdb_srpc_tgt; srpc_tgt;
             srpc_tgt = srpc_tgt->mtsc_next) {
                seq_show_srpc_rules(seq, srpc_tgt->mtsc_tgt,
                                    &srpc_tgt->mtsc_rset);
        }
        seq_show_srpc_rules(seq, fsdb->fsdb_name, &fsdb->fsdb_srpc_gen);

        lprocfs_rd_ir_state(seq, fsdb);

	mutex_unlock(&fsdb->fsdb_mutex);
        return 0;
}

static ssize_t mgs_live_seq_write(struct file *file, const char *buf,
                                  size_t len, loff_t *off)
{
        struct seq_file *seq  = file->private_data;
        struct fs_db    *fsdb = seq->private;
        ssize_t rc;

        rc = lprocfs_wr_ir_state(file, buf, len, fsdb);
        if (rc >= 0)
                rc = len;
        return rc;
}
LPROC_SEQ_FOPS(mgs_live);

int lproc_mgs_add_live(struct mgs_device *mgs, struct fs_db *fsdb)
{
        int rc;

        if (!mgs->mgs_proc_live)
                return 0;
	rc = lprocfs_seq_create(mgs->mgs_proc_live, fsdb->fsdb_name, 0644,
				&mgs_live_fops, fsdb);

        return 0;
}

int lproc_mgs_del_live(struct mgs_device *mgs, struct fs_db *fsdb)
{
        if (!mgs->mgs_proc_live)
                return 0;

	/* didn't create the proc file for MGSSELF_NAME */
	if (!test_bit(FSDB_MGS_SELF, &fsdb->fsdb_flags))
		lprocfs_remove_proc_entry(fsdb->fsdb_name, mgs->mgs_proc_live);
        return 0;
}

struct lprocfs_vars lprocfs_mgs_obd_vars[] = {
        { "uuid",            lprocfs_rd_uuid,        0, 0 },
        { "num_exports",     lprocfs_rd_num_exports, 0, 0 },
        { "evict_client",    0, lprocfs_wr_evict_client, 0 },
        { "ir_timeout",      lprocfs_rd_ir_timeout, lprocfs_wr_ir_timeout, 0 },
        { 0 }
};

struct lprocfs_vars lprocfs_mgs_module_vars[] = {
        { 0 }
};

void mgs_counter_incr(struct obd_export *exp, int opcode)
{
        lprocfs_counter_incr(exp->exp_obd->obd_stats, opcode);
        if (exp->exp_nid_stats && exp->exp_nid_stats->nid_stats != NULL)
                lprocfs_counter_incr(exp->exp_nid_stats->nid_stats, opcode);
}

void mgs_stats_counter_init(struct lprocfs_stats *stats)
{
        lprocfs_counter_init(stats, LPROC_MGS_CONNECT, 0, "connect", "reqs");
        lprocfs_counter_init(stats, LPROC_MGS_DISCONNECT, 0, "disconnect",
                             "reqs");
        lprocfs_counter_init(stats, LPROC_MGS_EXCEPTION, 0, "exception",
                             "reqs");
        lprocfs_counter_init(stats, LPROC_MGS_TARGET_REG, 0, "tgtreg", "reqs");
        lprocfs_counter_init(stats, LPROC_MGS_TARGET_DEL, 0, "tgtdel", "reqs");
}

void lprocfs_mgs_init_vars(struct lprocfs_static_vars *lvars)
{
    lvars->module_vars  = lprocfs_mgs_module_vars;
    lvars->obd_vars     = lprocfs_mgs_obd_vars;
}
#endif
