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
 * lustre/fid/fid_store.c
 *
 * Lustre Sequence Manager
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_FID

#ifdef __KERNEL__
# include <libcfs/libcfs.h>
# include <linux/module.h>
#else /* __KERNEL__ */
# include <liblustre.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <dt_object.h>
#include <md_object.h>
#include <obd_support.h>
#include <lustre_req_layout.h>
#include <lustre_fid.h>
#include "fid_internal.h"

#ifdef __KERNEL__

static struct lu_buf *seq_store_buf(struct seq_thread_info *info)
{
        struct lu_buf *buf;

        buf = &info->sti_buf;
        buf->lb_buf = &info->sti_space;
        buf->lb_len = sizeof(info->sti_space);
        return buf;
}

struct seq_update_callback {
        struct dt_txn_commit_cb suc_cb;
        struct lu_server_seq   *suc_seq;
};

void seq_update_cb(struct lu_env *env, struct thandle *th,
                   struct dt_txn_commit_cb *cb, int err)
{
        struct seq_update_callback *ccb;

        ccb = container_of0(cb, struct seq_update_callback, suc_cb);
        ccb->suc_seq->lss_need_sync = 0;
        OBD_FREE_PTR(ccb);
}

int seq_update_cb_add(struct thandle *th, struct lu_server_seq *seq)
{
        struct seq_update_callback *ccb;
        int rc;

        OBD_ALLOC_PTR(ccb);
        if (ccb == NULL)
                return -ENOMEM;

        ccb->suc_cb.dcb_func = seq_update_cb;
        CFS_INIT_LIST_HEAD(&ccb->suc_cb.dcb_linkage);
        ccb->suc_seq = seq;
        seq->lss_need_sync = 1;
        rc = dt_trans_cb_add(th, &ccb->suc_cb);
        if (rc)
                OBD_FREE_PTR(ccb);
        return rc;
}

/* This function implies that caller takes care about locking. */
int seq_store_update(const struct lu_env *env, struct lu_server_seq *seq,
                     struct lu_seq_range *out, int sync)
{
        struct dt_device *dt_dev = lu2dt_dev(seq->lss_obj->do_lu.lo_dev);
        struct seq_thread_info *info;
        struct thandle *th;
        loff_t pos = 0;
        int rc;

        info = lu_context_key_get(&env->le_ctx, &seq_thread_key);
        LASSERT(info != NULL);

        th = dt_trans_create(env, dt_dev);
        if (IS_ERR(th))
                RETURN(PTR_ERR(th));

        rc = dt_declare_record_write(env, seq->lss_obj,
                                     sizeof(struct lu_seq_range), 0, th);
        if (rc)
                GOTO(out, rc);

        if (out != NULL) {
                rc = fld_declare_server_create(seq->lss_site->ms_server_fld,
                                               env, th);
                if (rc)
                        GOTO(out, rc);
        }

        rc = dt_trans_start_local(env, dt_dev, th);
        if (rc)
                GOTO(out, rc);

        /* Store ranges in le format. */
        range_cpu_to_le(&info->sti_space, &seq->lss_space);

        rc = dt_record_write(env, seq->lss_obj, seq_store_buf(info), &pos, th);
        if (rc) {
                CERROR("%s: Can't write space data, rc %d\n",
                       seq->lss_name, rc);
                GOTO(out, rc);
        } else if (out != NULL) {
                rc = fld_server_create(seq->lss_site->ms_server_fld,
                                       env, out, th);
                if (rc) {
                        CERROR("%s: Can't Update fld database, rc %d\n",
                               seq->lss_name, rc);
                        GOTO(out, rc);
                }
        }
        /* next sequence update will need sync until this update is committed
         * in case of sync operation this is not needed obviously */
        if (!sync)
                /* if callback can't be added then sync always */
                sync = !!seq_update_cb_add(th, seq);

        th->th_sync |= sync;
out:
        dt_trans_stop(env, dt_dev, th);
        return rc;
}

/*
 * This function implies that caller takes care about locking or locking is not
 * needed (init time).
 */
int seq_store_read(struct lu_server_seq *seq,
                   const struct lu_env *env)
{
        struct seq_thread_info *info;
        loff_t pos = 0;
        int rc;
        ENTRY;

        info = lu_context_key_get(&env->le_ctx, &seq_thread_key);
        LASSERT(info != NULL);

        rc = seq->lss_obj->do_body_ops->dbo_read(env, seq->lss_obj,
                                                 seq_store_buf(info),
                                                 &pos, BYPASS_CAPA);
        if (rc == sizeof(info->sti_space)) {
                range_le_to_cpu(&seq->lss_space, &info->sti_space);
                CDEBUG(D_INFO, "%s: Space - "DRANGE"\n",
                       seq->lss_name, PRANGE(&seq->lss_space));
                rc = 0;
        } else if (rc == 0) {
                rc = -ENODATA;
        } else if (rc > 0) {
                CERROR("%s: Read only %d bytes of %d\n", seq->lss_name,
                       rc, (int)sizeof(info->sti_space));
                rc = -EIO;
        }

        RETURN(rc);
}

int seq_store_init(struct lu_server_seq *seq,
                   const struct lu_env *env,
                   struct dt_device *dt)
{
        struct dt_object *dt_obj;
        struct lu_fid fid;
        const char *name;
        int rc;
        ENTRY;

        name = seq->lss_type == LUSTRE_SEQ_SERVER ?
                LUSTRE_SEQ_SRV_NAME : LUSTRE_SEQ_CTL_NAME;

        dt_obj = dt_store_open(env, dt, "", name, &fid);
        if (!IS_ERR(dt_obj)) {
                seq->lss_obj = dt_obj;
                rc = 0;
        } else {
                CERROR("%s: Can't find \"%s\" obj %d\n",
                       seq->lss_name, name, (int)PTR_ERR(dt_obj));
                rc = PTR_ERR(dt_obj);
        }

        RETURN(rc);
}

void seq_store_fini(struct lu_server_seq *seq,
                    const struct lu_env *env)
{
        ENTRY;

        if (seq->lss_obj != NULL) {
                if (!IS_ERR(seq->lss_obj))
                        lu_object_put(env, &seq->lss_obj->do_lu);
                seq->lss_obj = NULL;
        }

        EXIT;
}
#endif
