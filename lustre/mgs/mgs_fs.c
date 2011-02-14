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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mgs/mgs_fs.c
 *
 * Lustre Management Server (MGS) filesystem interface code
 *
 * Author: Nathan Rutman <nathan@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MGS

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/mount.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre_disk.h>
#include <lustre_lib.h>
#include <lustre_fid.h>
#include <libcfs/list.h>
#include "mgs_internal.h"

int mgs_export_stats_init(struct obd_device *obd, struct obd_export *exp,
                          void *localdata)

{
        lnet_nid_t *client_nid = localdata;
        int rc, newnid;
        ENTRY;

        rc = lprocfs_exp_setup(exp, client_nid, &newnid);
        if (rc) {
                /* Mask error for already created
                 * /proc entries */
                if (rc == -EALREADY)
                        rc = 0;
                RETURN(rc);
        }
        if (newnid) {
                struct nid_stat *tmp = exp->exp_nid_stats;
                int num_stats = 0;

                num_stats = (sizeof(*obd->obd_type->typ_dt_ops) / sizeof(void *)) +
                            LPROC_MGS_LAST - 1;
                tmp->nid_stats = lprocfs_alloc_stats(num_stats,
                                                     LPROCFS_STATS_FLAG_NOPERCPU);
                if (tmp->nid_stats == NULL)
                        return -ENOMEM;
                lprocfs_init_ops_stats(LPROC_MGS_LAST, tmp->nid_stats);
                mgs_stats_counter_init(tmp->nid_stats);
                rc = lprocfs_register_stats(tmp->nid_proc, "stats",
                                            tmp->nid_stats);
                if (rc)
                        GOTO(clean, rc);

                rc = lprocfs_nid_ldlm_stats_init(tmp);
                if (rc)
                        GOTO(clean, rc);
        }
        RETURN(0);
clean:
        return rc;
}

/**
 * Add client export data to the MGS.  This data is currently NOT stored on
 * disk in the last_rcvd file or anywhere else.  In the event of a MGS
 * crash all connections are treated as new connections.
 */
int mgs_client_add(struct obd_device *obd, struct obd_export *exp,
                   void *localdata)
{
        return 0;
}

/* Remove client export data from the MGS */
int mgs_client_free(struct obd_export *exp)
{
        return 0;
}

int mgs_fs_setup(const struct lu_env *env, struct mgs_device *mgs)
{
        struct obd_device       *obd = mgs->mgs_obd;
        struct dt_object_format  dof;
        struct lu_fid            fid;
        struct lu_attr           attr;
        struct dt_object        *o;
        int rc;
        ENTRY;

        /* FIXME what's this?  Do I need it? */
        rc = cfs_cleanup_group_info();
        if (rc)
                RETURN(rc);

        OBD_SET_CTXT_MAGIC(&obd->obd_lvfs_ctxt);
        obd->obd_lvfs_ctxt.dt = mgs->mgs_bottom;

        /* XXX: fix when support for N:1 layering is implemented */
        LASSERT(mgs->mgs_dt_dev.dd_lu_dev.ld_site);
        mgs->mgs_dt_dev.dd_lu_dev.ld_site->ls_top_dev = &mgs->mgs_dt_dev.dd_lu_dev;

        /* Setup the configs dir */
        lu_local_obj_fid(&fid, MGS_CONFIGS_OID);
        memset(&attr, 0, sizeof(attr));
        attr.la_valid = LA_MODE;
        attr.la_mode = S_IFDIR | 0666;
        dof.dof_type = dt_mode_to_dft(S_IFDIR);
        o = dt_find_or_create(env, mgs->mgs_bottom, &fid, &dof, &attr);
        if (IS_ERR(o))
                GOTO(out, rc = PTR_ERR(o));

        if (!dt_try_as_dir(env, o))
                GOTO(out, rc = -ENOTDIR);

        mgs->mgs_configs_dir = o;

out:
        mgs->mgs_dt_dev.dd_lu_dev.ld_site->ls_top_dev = NULL;
        return rc;
}

int mgs_fs_cleanup(const struct lu_env *env, struct mgs_device *mgs)
{
        struct obd_device *obd = mgs->mgs_obd;

        class_disconnect_exports(obd); /* cleans up client info too */

        if (mgs->mgs_configs_dir) {
                lu_object_put(env, &mgs->mgs_configs_dir->do_lu);
                mgs->mgs_configs_dir = NULL;
        }

        return 0;
}
