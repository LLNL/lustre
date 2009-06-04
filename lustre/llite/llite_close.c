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
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/llite/llite_close.c
 *
 * Lustre Lite routines to issue a secondary close after writeback
 */

#include <linux/module.h>

#define DEBUG_SUBSYSTEM S_LLITE

//#include <lustre_mdc.h>
#include <lustre_lite.h>
#include "llite_internal.h"

/** records that a write is in flight */
void vvp_write_pending(struct ccc_object *club, struct ccc_page *page)
{
        struct ll_inode_info *lli = ll_i2info(club->cob_inode);

        ENTRY;
        spin_lock(&lli->lli_lock);
        lli->lli_flags |= LLIF_SOM_DIRTY;
        if (page != NULL && list_empty(&page->cpg_pending_linkage))
                list_add(&page->cpg_pending_linkage, &club->cob_pending_list);
        spin_unlock(&lli->lli_lock);
        EXIT;
}

/** records that a write has completed */
void vvp_write_complete(struct ccc_object *club, struct ccc_page *page)
{
        struct ll_inode_info *lli = ll_i2info(club->cob_inode);
        int rc = 0;

        ENTRY;
        spin_lock(&lli->lli_lock);
        if (page != NULL && !list_empty(&page->cpg_pending_linkage)) {
                list_del_init(&page->cpg_pending_linkage);
                rc = 1;
        }
        spin_unlock(&lli->lli_lock);
        if (rc)
                ll_queue_done_writing(club->cob_inode, 0);
        EXIT;
}

/** Queues DONE_WRITING if
 * - done writing is allowed;
 * - inode has no no dirty pages; */
void ll_queue_done_writing(struct inode *inode, unsigned long flags)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ccc_object *club = cl2ccc(ll_i2info(inode)->lli_clob);
        spin_lock(&lli->lli_lock);
        lli->lli_flags |= flags;
        ENTRY;

        if ((lli->lli_flags & LLIF_DONE_WRITING) &&
            list_empty(&club->cob_pending_list)) {
                struct ll_close_queue *lcq = ll_i2sbi(inode)->ll_lcq;

                if (lli->lli_flags & LLIF_MDS_SIZE_LOCK)
                         CWARN("ino %lu/%u(flags %lu) som valid it just after "
                               "recovery\n",
                               inode->i_ino, inode->i_generation,
                               lli->lli_flags);
                /* DONE_WRITING is allowed and inode has no dirty page. */
                spin_lock(&lcq->lcq_lock);

                LASSERT(list_empty(&lli->lli_close_list));
                CDEBUG(D_INODE, "adding inode %lu/%u to close list\n",
                       inode->i_ino, inode->i_generation);
                list_add_tail(&lli->lli_close_list, &lcq->lcq_head);

                /* Avoid a concurrent insertion into the close thread queue:
                 * an inode is already in the close thread, open(), write(),
                 * close() happen, epoch is closed as the inode is marked as
                 * LLIF_EPOCH_PENDING. When pages are written inode should not
                 * be inserted into the queue again, clear this flag to avoid
                 * it. */
                lli->lli_flags &= ~LLIF_DONE_WRITING;

                wake_up(&lcq->lcq_waitq);
                spin_unlock(&lcq->lcq_lock);
        }
        spin_unlock(&lli->lli_lock);
        EXIT;
}

/** Closes epoch and sends Size-on-MDS attribute update if possible.  Call
 * this under ll_inode_info::lli_lock spinlock. */
void ll_epoch_close(struct inode *inode, struct md_op_data *op_data,
                    struct obd_client_handle **och, unsigned long flags)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ccc_object *club = cl2ccc(ll_i2info(inode)->lli_clob);
        ENTRY;

        spin_lock(&lli->lli_lock);
        if (!(list_empty(&club->cob_pending_list))) {
                if (!(lli->lli_flags & LLIF_EPOCH_PENDING)) {
                        LASSERT(*och != NULL);
                        LASSERT(lli->lli_pending_och == NULL);
                        /* Inode is dirty and there is no pending write done
                         * request yet, DONE_WRITE is to be sent later. */
                        lli->lli_flags |= LLIF_EPOCH_PENDING;
                        lli->lli_pending_och = *och;
                        spin_unlock(&lli->lli_lock);

                        inode = igrab(inode);
                        LASSERT(inode);
                        GOTO(out, 0);
                }
                if (flags & LLIF_DONE_WRITING) {
                        /* Some pages are still dirty, it is early to send
                         * DONE_WRITE. Wait untill all pages will be flushed
                         * and try DONE_WRITE again later. */
                        LASSERT(!(lli->lli_flags & LLIF_DONE_WRITING));
                        lli->lli_flags |= LLIF_DONE_WRITING;
                        spin_unlock(&lli->lli_lock);

                        inode = igrab(inode);
                        LASSERT(inode);
                        GOTO(out, 0);
                }
        }
        CDEBUG(D_INODE, "Epoch "LPU64" closed on "DFID"\n",
               ll_i2info(inode)->lli_ioepoch, PFID(&lli->lli_fid));
        op_data->op_flags |= MF_EPOCH_CLOSE;

        if (flags & LLIF_DONE_WRITING) {
                LASSERT(lli->lli_flags & LLIF_SOM_DIRTY);
                LASSERT(!(lli->lli_flags & LLIF_DONE_WRITING));
                *och = lli->lli_pending_och;
                lli->lli_pending_och = NULL;
                lli->lli_flags &= ~LLIF_EPOCH_PENDING;
        } else {
                /* Pack Size-on-MDS inode attributes only if they has changed */
                if (!(lli->lli_flags & LLIF_SOM_DIRTY)) {
                        spin_unlock(&lli->lli_lock);
                        GOTO(out, 0);
                }

                /* There is a pending DONE_WRITE -- close epoch with no
                 * attribute change. */
                if (lli->lli_flags & LLIF_EPOCH_PENDING) {
                        spin_unlock(&lli->lli_lock);
                        GOTO(out, 0);
                }
        }

        LASSERT(list_empty(&club->cob_pending_list));
        lli->lli_flags &= ~LLIF_SOM_DIRTY;
        spin_unlock(&lli->lli_lock);
        op_data->op_flags |= MF_SOM_CHANGE;

        /* Check if Size-on-MDS attributes are valid. */
        if (lli->lli_flags & LLIF_MDS_SIZE_LOCK)
                CWARN("ino %lu/%u(flags %lu) som valid it just after "
                      "recovery\n",
                      inode->i_ino, inode->i_generation, lli->lli_flags);

        if (!cl_local_size(inode)) {
                /* Send Size-on-MDS Attributes if valid. Atime is sent along
                 * with all the attributes. */
                op_data->op_attr.ia_valid |= ATTR_MTIME_SET | ATTR_CTIME_SET |
                                ATTR_ATIME_SET | ATTR_SIZE | ATTR_BLOCKS;
        }
        EXIT;
out:
        return;
}

int ll_sizeonmds_update(struct inode *inode, struct lustre_handle *fh,
                        __u64 ioepoch)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct md_op_data *op_data;
        struct obdo *oa;
        int rc;
        ENTRY;

        /* LASSERT(!(lli->lli_flags & LLIF_MDS_SIZE_LOCK)); */
        /* After recovery that can be valid. */
        if (lli->lli_flags & LLIF_MDS_SIZE_LOCK)
                CWARN("ino %lu/%u(flags %lu) som valid it just after "
                      "recovery\n", inode->i_ino, inode->i_generation,
                      lli->lli_flags);

        OBDO_ALLOC(oa);
        OBD_ALLOC_PTR(op_data);
        if (!oa || !op_data) {
                CERROR("can't allocate memory for Size-on-MDS update.\n");
                RETURN(-ENOMEM);
        }
        rc = ll_inode_getattr(inode, oa);
        if (rc == -ENOENT) {
                oa->o_valid = 0;
                CDEBUG(D_INODE, "objid "LPX64" is already destroyed\n",
                       lli->lli_smd->lsm_object_id);
        } else if (rc) {
                CERROR("inode_getattr failed (%d): unable to send a "
                       "Size-on-MDS attribute update for inode %lu/%u\n",
                       rc, inode->i_ino, inode->i_generation);
                GOTO(out, rc);
        }
        CDEBUG(D_INODE, "Size-on-MDS update on "DFID"\n", PFID(&lli->lli_fid));

        md_from_obdo(op_data, oa, oa->o_valid);
        memcpy(&op_data->op_handle, fh, sizeof(*fh));

        op_data->op_ioepoch = ioepoch;
        op_data->op_flags |= MF_SOM_CHANGE;

        rc = ll_md_setattr(inode, op_data, NULL);
        EXIT;
out:
        if (oa)
                OBDO_FREE(oa);
        if (op_data)
                ll_finish_md_op_data(op_data);
        return rc;
}

/** Sends a DONE_WRITING rpc, packs Size-on-MDS attributes into it, if
 * possible */
static void ll_done_writing(struct inode *inode)
{
        struct obd_client_handle *och = NULL;
        struct md_op_data *op_data;
        int rc;
        ENTRY;

        LASSERT(ll_i2mdexp(inode)->exp_connect_flags & OBD_CONNECT_SOM);

        OBD_ALLOC_PTR(op_data);
        if (op_data == NULL) {
                CERROR("can't allocate op_data\n");
                EXIT;
                return;
        }

        ll_epoch_close(inode, op_data, &och, LLIF_DONE_WRITING);
        /* If there is no @och, we do not do D_W yet. */
        if (och == NULL)
                GOTO(out, 0);

        ll_pack_inode2opdata(inode, op_data, &och->och_fh);

        rc = md_done_writing(ll_i2sbi(inode)->ll_md_exp, op_data, NULL);
        if (rc == -EAGAIN) {
                /* MDS has instructed us to obtain Size-on-MDS attribute from
                 * OSTs and send setattr to back to MDS. */
                rc = ll_sizeonmds_update(inode, &och->och_fh,
                                         op_data->op_ioepoch);
        } else if (rc) {
                CERROR("inode %lu mdc done_writing failed: rc = %d\n",
                       inode->i_ino, rc);
        }
out:
        ll_finish_md_op_data(op_data);
        if (och) {
                md_clear_open_replay_data(ll_i2sbi(inode)->ll_md_exp, och);
                OBD_FREE_PTR(och);
        }
        EXIT;
}

static struct ll_inode_info *ll_close_next_lli(struct ll_close_queue *lcq)
{
        struct ll_inode_info *lli = NULL;

        spin_lock(&lcq->lcq_lock);

        if (!list_empty(&lcq->lcq_head)) {
                lli = list_entry(lcq->lcq_head.next, struct ll_inode_info,
                                 lli_close_list);
                list_del_init(&lli->lli_close_list);
        } else if (atomic_read(&lcq->lcq_stop))
                lli = ERR_PTR(-EALREADY);

        spin_unlock(&lcq->lcq_lock);
        return lli;
}

static int ll_close_thread(void *arg)
{
        struct ll_close_queue *lcq = arg;
        ENTRY;

        {
                char name[CFS_CURPROC_COMM_MAX];
                snprintf(name, sizeof(name) - 1, "ll_close");
                cfs_daemonize(name);
        }

        complete(&lcq->lcq_comp);

        while (1) {
                struct l_wait_info lwi = { 0 };
                struct ll_inode_info *lli;
                struct inode *inode;

                l_wait_event_exclusive(lcq->lcq_waitq,
                                       (lli = ll_close_next_lli(lcq)) != NULL,
                                       &lwi);
                if (IS_ERR(lli))
                        break;

                inode = ll_info2i(lli);
                CDEBUG(D_INFO, "done_writting for inode %lu/%u\n",
                       inode->i_ino, inode->i_generation);
                ll_done_writing(inode);
                iput(inode);
        }

        CDEBUG(D_INFO, "ll_close exiting\n");
        complete(&lcq->lcq_comp);
        RETURN(0);
}

int ll_close_thread_start(struct ll_close_queue **lcq_ret)
{
        struct ll_close_queue *lcq;
        pid_t pid;

        if (OBD_FAIL_CHECK(OBD_FAIL_LDLM_CLOSE_THREAD))
                return -EINTR;

        OBD_ALLOC(lcq, sizeof(*lcq));
        if (lcq == NULL)
                return -ENOMEM;

        spin_lock_init(&lcq->lcq_lock);
        INIT_LIST_HEAD(&lcq->lcq_head);
        init_waitqueue_head(&lcq->lcq_waitq);
        init_completion(&lcq->lcq_comp);

        pid = kernel_thread(ll_close_thread, lcq, 0);
        if (pid < 0) {
                OBD_FREE(lcq, sizeof(*lcq));
                return pid;
        }

        wait_for_completion(&lcq->lcq_comp);
        *lcq_ret = lcq;
        return 0;
}

void ll_close_thread_shutdown(struct ll_close_queue *lcq)
{
        init_completion(&lcq->lcq_comp);
        atomic_inc(&lcq->lcq_stop);
        wake_up(&lcq->lcq_waitq);
        wait_for_completion(&lcq->lcq_comp);
        OBD_FREE(lcq, sizeof(*lcq));
}
