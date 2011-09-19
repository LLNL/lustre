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
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mdt/mdt_capa.c
 *
 * Lustre Metadata Target (mdt) capability key read/write/update.
 *
 * Author: Lai Siyao <lsy@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include "mdt_internal.h"

static inline void set_capa_key_expiry(struct mdt_device *mdt)
{
        mdt->mdt_ck_expiry = jiffies + mdt->mdt_ck_timeout * CFS_HZ;
}

static void make_capa_key(struct lustre_capa_key *key,
                          mdsno_t mdsnum, int keyid)
{
        key->lk_seq = mdsnum;
        key->lk_keyid = keyid + 1;
        cfs_get_random_bytes(key->lk_key, sizeof(key->lk_key));
}

static inline void lck_cpu_to_le(struct lustre_capa_key *tgt,
                                 struct lustre_capa_key *src)
{
        tgt->lk_seq   = cpu_to_le64(src->lk_seq);
        tgt->lk_keyid   = cpu_to_le32(src->lk_keyid);
        tgt->lk_padding = cpu_to_le32(src->lk_padding);
        memcpy(tgt->lk_key, src->lk_key, sizeof(src->lk_key));
}

static inline void lck_le_to_cpu(struct lustre_capa_key *tgt,
                                 struct lustre_capa_key *src)
{
        tgt->lk_seq   = le64_to_cpu(src->lk_seq);
        tgt->lk_keyid   = le32_to_cpu(src->lk_keyid);
        tgt->lk_padding = le32_to_cpu(src->lk_padding);
        memcpy(tgt->lk_key, src->lk_key, sizeof(src->lk_key));
}

static int write_capa_keys(const struct lu_env *env,
                           struct mdt_device *mdt,
                           struct lustre_capa_key *keys)
{
        struct mdt_thread_info *mti;
        struct lustre_capa_key *tmp;
        struct thandle *th;
        loff_t off = 0;
        int i, rc;

        mti = lu_context_key_get(&env->le_ctx, &mdt_thread_key);
        th = mdt_trans_create(env, mdt);
        if (IS_ERR(th))
                RETURN(PTR_ERR(th));
        rc = mdt_declare_record_write(env, mdt->mdt_ck_obj,
                                      sizeof(*tmp) * 2, 0, th);
        if (rc)
                GOTO(cleanup, rc);
        rc = mdt_trans_start(env, mdt, th);
        if (rc)
                GOTO(cleanup, rc);

        tmp = &mti->mti_capa_key;

        for (i = 0; i < 2; i++) {
                lck_cpu_to_le(tmp, &keys[i]);

                rc = dt_record_write(env, mdt->mdt_ck_obj,
                                     mdt_buf_const(env, tmp, sizeof(*tmp)),
                                     &off, th);
                if (rc)
                        break;
        }
cleanup:
        mdt_trans_stop(env, mdt, th);

        CDEBUG(D_INFO, "write capability keys rc = %d:\n", rc);
        return rc;
}

static int read_capa_keys(const struct lu_env *env,
                          struct mdt_device *mdt,
                          struct lustre_capa_key *keys)
{
        struct mdt_thread_info *mti;
        struct lustre_capa_key *tmp;
        loff_t off = 0;
        int i, rc;

        mti = lu_context_key_get(&env->le_ctx, &mdt_thread_key);
        tmp = &mti->mti_capa_key;

        for (i = 0; i < 2; i++) {
                rc = dt_record_read(env, mdt->mdt_ck_obj,
                                    mdt_buf(env, tmp, sizeof(*tmp)), &off);
                if (rc)
                        return rc;

                lck_le_to_cpu(&keys[i], tmp);
                DEBUG_CAPA_KEY(D_SEC, &keys[i], "read");
        }

        return 0;
}

int mdt_capa_keys_init(const struct lu_env *env, struct mdt_device *mdt)
{
        struct lustre_capa_key  *keys = mdt->mdt_capa_keys;
        struct mdt_thread_info  *mti;
        struct dt_object        *obj;
        struct lu_attr          *la;
        mdsno_t                  mdsnum;
        unsigned long            size;
        int                      rc;
        ENTRY;

        mdsnum = mdt_md_site(mdt)->ms_node_id;

        mti = lu_context_key_get(&env->le_ctx, &mdt_thread_key);
        LASSERT(mti != NULL);
        la = &mti->mti_attr.ma_attr;

        obj = mdt->mdt_ck_obj;
        rc = obj->do_ops->do_attr_get(env, mdt->mdt_ck_obj, la, BYPASS_CAPA);
        if (rc)
                RETURN(rc);

        size = (unsigned long)la->la_size;
        if (size == 0) {
                int i;

                for (i = 0; i < 2; i++) {
                        make_capa_key(&keys[i], mdsnum, i);
                        DEBUG_CAPA_KEY(D_SEC, &keys[i], "initializing");
                }

                rc = write_capa_keys(env, mdt, keys);
                if (rc) {
                        CERROR("error writing MDS %s: rc %d\n", CAPA_KEYS, rc);
                        RETURN(rc);
                }
        } else {
                rc = read_capa_keys(env, mdt, keys);
                if (rc) {
                        CERROR("error reading MDS %s: rc %d\n", CAPA_KEYS, rc);
                        RETURN(rc);
                }
        }
        set_capa_key_expiry(mdt);
        cfs_timer_arm(&mdt->mdt_ck_timer, mdt->mdt_ck_expiry);
        CDEBUG(D_SEC, "mds_ck_timer %lu\n", mdt->mdt_ck_expiry);
        RETURN(0);
}

void mdt_ck_timer_callback(unsigned long castmeharder)
{
        struct mdt_device *mdt = (struct mdt_device *)castmeharder;
        struct ptlrpc_thread *thread = &mdt->mdt_ck_thread;

        ENTRY;
        thread->t_flags |= SVC_EVENT;
        cfs_waitq_signal(&thread->t_ctl_waitq);
        EXIT;
}

static int mdt_ck_thread_main(void *args)
{
        struct mdt_device      *mdt = args;
        struct ptlrpc_thread   *thread = &mdt->mdt_ck_thread;
        struct lustre_capa_key *bkey = &mdt->mdt_capa_keys[0],
                               *rkey = &mdt->mdt_capa_keys[1];
        struct lustre_capa_key *tmp;
        struct lu_env           env;
        struct mdt_thread_info *info;
        struct md_device       *next;
        struct l_wait_info      lwi = { 0 };
        mdsno_t                 mdsnum;
        int                     rc;
        ENTRY;

        cfs_daemonize_ctxt("mdt_ck");
        cfs_block_allsigs();

        thread->t_flags = SVC_RUNNING;
        cfs_waitq_signal(&thread->t_ctl_waitq);

        rc = lu_env_init(&env, LCT_MD_THREAD|LCT_REMEMBER|LCT_NOREF);
        if (rc)
                RETURN(rc);

        thread->t_env = &env;
        env.le_ctx.lc_thread = thread;
        env.le_ctx.lc_cookie = 0x1;

        info = lu_context_key_get(&env.le_ctx, &mdt_thread_key);
        LASSERT(info != NULL);

        tmp = &info->mti_capa_key;
        mdsnum = mdt_md_site(mdt)->ms_node_id;
        while (1) {
                l_wait_event(thread->t_ctl_waitq,
                             thread->t_flags & (SVC_STOPPING | SVC_EVENT),
                             &lwi);

                if (thread->t_flags & SVC_STOPPING)
                        break;
                thread->t_flags &= ~SVC_EVENT;

                if (cfs_time_before(cfs_time_current(), mdt->mdt_ck_expiry))
                        break;

                *tmp = *rkey;
                make_capa_key(tmp, mdsnum, rkey->lk_keyid);

                next = mdt->mdt_child;
                rc = next->md_ops->mdo_update_capa_key(&env, next, tmp);
                if (!rc) {
                        cfs_spin_lock(&capa_lock);
                        *bkey = *rkey;
                        *rkey = *tmp;
                        cfs_spin_unlock(&capa_lock);

                        rc = write_capa_keys(&env, mdt, mdt->mdt_capa_keys);
                        if (rc) {
                                cfs_spin_lock(&capa_lock);
                                *rkey = *bkey;
                                memset(bkey, 0, sizeof(*bkey));
                                cfs_spin_unlock(&capa_lock);
                        } else {
                                set_capa_key_expiry(mdt);
                                DEBUG_CAPA_KEY(D_SEC, rkey, "new");
                        }
                }
                if (rc) {
                        DEBUG_CAPA_KEY(D_ERROR, rkey, "update failed for");
                        /* next retry is in 300 sec */
                        mdt->mdt_ck_expiry = jiffies + 300 * CFS_HZ;
                }

                cfs_timer_arm(&mdt->mdt_ck_timer, mdt->mdt_ck_expiry);
                CDEBUG(D_SEC, "mdt_ck_timer %lu\n", mdt->mdt_ck_expiry);
        }
        lu_env_fini(&env);

        thread->t_flags = SVC_STOPPED;
        cfs_waitq_signal(&thread->t_ctl_waitq);
        RETURN(0);
}

int mdt_ck_thread_start(struct mdt_device *mdt)
{
        struct ptlrpc_thread *thread = &mdt->mdt_ck_thread;
        int rc;

        cfs_waitq_init(&thread->t_ctl_waitq);
        rc = cfs_create_thread(mdt_ck_thread_main, mdt, CFS_DAEMON_FLAGS);
        if (rc < 0) {
                CERROR("cannot start mdt_ck thread, rc = %d\n", rc);
                return rc;
        }

        l_wait_condition(thread->t_ctl_waitq, thread->t_flags & SVC_RUNNING);
        return 0;
}

void mdt_ck_thread_stop(struct mdt_device *mdt)
{
        struct ptlrpc_thread *thread = &mdt->mdt_ck_thread;

        if (!(thread->t_flags & SVC_RUNNING))
                return;

        thread->t_flags = SVC_STOPPING;
        cfs_waitq_signal(&thread->t_ctl_waitq);
        l_wait_condition(thread->t_ctl_waitq, thread->t_flags & SVC_STOPPED);
}
