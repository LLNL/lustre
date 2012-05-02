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
 * lustre/ofd/ofd_lvb.c
 *
 * Author: Mike Pershin <tappro@sun.com>
 * Author: Alex Tomas <alex.tomas@sun.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include "ofd_internal.h"

static int ofd_lvbo_free(struct ldlm_resource *res) {
        if (res->lr_lvb_data)
                OBD_FREE(res->lr_lvb_data, res->lr_lvb_len);
        return 0;
}

/* Called with res->lr_lvb_sem held */
static int ofd_lvbo_init(struct ldlm_resource *res)
{
        struct ost_lvb *lvb = NULL;
        struct ofd_device *ofd;
        struct ofd_object *fo;
        struct ofd_thread_info *info;
        struct lu_env env;
        int rc = 0;
        ENTRY;

        LASSERT(res);

        if (res->lr_lvb_data)
                RETURN(0);

        ofd = ldlm_res_to_ns(res)->ns_lvbp;
        LASSERT(ofd != NULL);

        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc)
                RETURN(rc);

        OBD_ALLOC_PTR(lvb);
        if (lvb == NULL)
                RETURN(-ENOMEM);

        res->lr_lvb_data = lvb;
        res->lr_lvb_len = sizeof(*lvb);

        info = ofd_info_init(&env, NULL);
        ofd_fid_from_resid(&info->fti_fid, &res->lr_name);
        fo = ofd_object_find(&env, ofd, &info->fti_fid);
        if (IS_ERR(fo))
                GOTO(out, rc = PTR_ERR(fo));

        rc = ofd_attr_get(&env, fo, &info->fti_attr);
        if (rc)
                GOTO(out_put, rc);

        lvb->lvb_size = info->fti_attr.la_size;
        lvb->lvb_blocks = info->fti_attr.la_blocks;
        lvb->lvb_mtime = info->fti_attr.la_mtime;
        lvb->lvb_atime = info->fti_attr.la_atime;
        lvb->lvb_ctime = info->fti_attr.la_ctime;

        CDEBUG(D_DLMTRACE, "res: "LPX64" initial lvb size: "LPX64", "
               "mtime: "LPX64", blocks: "LPX64"\n",
               res->lr_name.name[0], lvb->lvb_size,
               lvb->lvb_mtime, lvb->lvb_blocks);

        EXIT;
out_put:
        ofd_object_put(&env, fo);
out:
        lu_env_fini(&env);
        if (rc)
                OST_LVB_SET_ERR(lvb->lvb_blocks, rc);
        /* Don't free lvb data on lookup error */
        return rc;
}

/* This will be called in two ways:
 *
 *   r != NULL : called by the DLM itself after a glimpse callback
 *   r == NULL : called by the ofd after a disk write
 *
 *   If 'increase_only' is true, don't allow values to move backwards.
 */
static int ofd_lvbo_update(struct ldlm_resource *res,
                              struct ptlrpc_request *r,
                              int increase_only)
{
        struct ofd_device *ofd;
        struct ofd_object *fo;
        struct ofd_thread_info *info;
        struct ost_lvb *lvb;
        struct lu_env env;
        int rc = 0;
        ENTRY;

        LASSERT(res);

        lvb = res->lr_lvb_data;
        if (lvb == NULL) {
                CERROR("No lvb when running lvbo_update!\n");
                GOTO(out_unlock, rc = 0);
        }

        /* XXX: it's too expensive to create env every time */
        rc = lu_env_init(&env, LCT_DT_THREAD);
        if (rc)
                GOTO(out_unlock, rc);

        info = ofd_info_init(&env, NULL);
        /* Update the LVB from the network message */
        if (r != NULL) {
                struct ost_lvb *new;

                /* XXX update always from reply buffer */
                new = req_capsule_server_get(&r->rq_pill, &RMF_DLM_LVB);
                if (new == NULL) {
                        CERROR("lustre_swab_buf failed\n");
                        goto disk_update;
                }
                lock_res(res);
                if (new->lvb_size > lvb->lvb_size || !increase_only) {
                        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb size: "
                               LPU64" -> "LPU64"\n", res->lr_name.name[0],
                               lvb->lvb_size, new->lvb_size);
                        lvb->lvb_size = new->lvb_size;
                }
                if (new->lvb_mtime > lvb->lvb_mtime || !increase_only) {
                        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb mtime: "
                               LPU64" -> "LPU64"\n", res->lr_name.name[0],
                               lvb->lvb_mtime, new->lvb_mtime);
                        lvb->lvb_mtime = new->lvb_mtime;
                }
                if (new->lvb_atime > lvb->lvb_atime || !increase_only) {
                        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb atime: "
                               LPU64" -> "LPU64"\n", res->lr_name.name[0],
                               lvb->lvb_atime, new->lvb_atime);
                        lvb->lvb_atime = new->lvb_atime;
                }
                if (new->lvb_ctime > lvb->lvb_ctime || !increase_only) {
                        CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb ctime: "
                               LPU64" -> "LPU64"\n", res->lr_name.name[0],
                               lvb->lvb_ctime, new->lvb_ctime);
                        lvb->lvb_ctime = new->lvb_ctime;
                }
                unlock_res(res);
        }

 disk_update:
        /* Update the LVB from the disk inode */
        ofd = ldlm_res_to_ns(res)->ns_lvbp;
        LASSERT(ofd != NULL);

        ofd_fid_from_resid(&info->fti_fid, &res->lr_name);
        fo = ofd_object_find(&env, ofd, &info->fti_fid);
        if (IS_ERR(fo))
                GOTO(out_env, rc = PTR_ERR(fo));

        rc = ofd_attr_get(&env, fo, &info->fti_attr);
        if (rc)
                GOTO(out_obj, rc);

        lock_res(res);
        if (info->fti_attr.la_size > lvb->lvb_size || !increase_only) {
                CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb size from disk: "
                       LPU64" -> %llu\n", res->lr_name.name[0],
                       lvb->lvb_size, info->fti_attr.la_size);
                lvb->lvb_size = info->fti_attr.la_size;
        }

        if (info->fti_attr.la_mtime >lvb->lvb_mtime || !increase_only) {
                CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb mtime from disk: "
                       LPU64" -> "LPU64"\n", res->lr_name.name[0],
                       lvb->lvb_mtime, info->fti_attr.la_mtime);
                lvb->lvb_mtime = info->fti_attr.la_mtime;
        }
        if (info->fti_attr.la_atime >lvb->lvb_atime || !increase_only) {
                CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb atime from disk: "
                       LPU64" -> "LPU64"\n", res->lr_name.name[0],
                       lvb->lvb_atime, info->fti_attr.la_atime);
                lvb->lvb_atime = info->fti_attr.la_atime;
        }
        if (info->fti_attr.la_ctime >lvb->lvb_ctime || !increase_only) {
                CDEBUG(D_DLMTRACE, "res: "LPU64" updating lvb ctime from disk: "
                       LPU64" -> "LPU64"\n", res->lr_name.name[0],
                       lvb->lvb_ctime, info->fti_attr.la_ctime);
                lvb->lvb_ctime = info->fti_attr.la_ctime;
        }
        if (lvb->lvb_blocks != info->fti_attr.la_blocks) {
                CDEBUG(D_DLMTRACE,"res: "LPU64" updating lvb blocks from disk: "
                       LPU64" -> %llu\n", res->lr_name.name[0],
                       lvb->lvb_blocks,
                       (unsigned long long)info->fti_attr.la_blocks);
                lvb->lvb_blocks = info->fti_attr.la_blocks;
        }
        unlock_res(res);

out_obj:
        ofd_object_put(&env, fo);
out_env:
        lu_env_fini(&env);
out_unlock:
        return rc;
}

struct ldlm_valblock_ops ofd_lvbo = {
        lvbo_init:   ofd_lvbo_init,
        lvbo_update: ofd_lvbo_update,
        lvbo_free:   ofd_lvbo_free,
};
