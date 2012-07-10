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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Implementation of cl_io for VVP layer.
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE

#ifndef __KERNEL__
# error This file is kernel only.
#endif

#include <obd.h>
#include <lustre_lite.h>

#include "vvp_internal.h"

static struct vvp_io *cl2vvp_io(const struct lu_env *env,
                                const struct cl_io_slice *slice);

/**
 * True, if \a io is a normal io, False for sendfile() / splice_{read|write}
 */
int cl_is_normalio(const struct lu_env *env, const struct cl_io *io)
{
        struct vvp_io *vio = vvp_env_io(env);

        LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);

        return vio->cui_io_subtype == IO_NORMAL;
}

/*****************************************************************************
 *
 * io operations.
 *
 */

static int vvp_io_fault_iter_init(const struct lu_env *env,
                                  const struct cl_io_slice *ios)
{
        struct vvp_io *vio   = cl2vvp_io(env, ios);
        struct inode  *inode = ccc_object_inode(ios->cis_obj);

        LASSERT(inode ==
                cl2ccc_io(env, ios)->cui_fd->fd_file->f_dentry->d_inode);
        vio->u.fault.ft_mtime = LTIME_S(inode->i_mtime);
        return 0;
}

static void vvp_io_fini(const struct lu_env *env, const struct cl_io_slice *ios)
{
        struct cl_io     *io  = ios->cis_io;
        struct cl_object *obj = io->ci_obj;

        CLOBINVRNT(env, obj, ccc_object_invariant(obj));
        if (io->ci_type == CIT_READ) {
                struct vvp_io     *vio  = cl2vvp_io(env, ios);
                struct ccc_io     *cio  = cl2ccc_io(env, ios);

                if (vio->cui_ra_window_set)
                        ll_ra_read_ex(cio->cui_fd->fd_file, &vio->cui_bead);
        }

}

static void vvp_io_fault_fini(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
        struct cl_io   *io   = ios->cis_io;
        struct cl_page *page = io->u.ci_fault.ft_page;

        CLOBINVRNT(env, io->ci_obj, ccc_object_invariant(io->ci_obj));

        if (page != NULL) {
                lu_ref_del(&page->cp_reference, "fault", io);
                cl_page_put(env, page);
                io->u.ci_fault.ft_page = NULL;
        }
        vvp_io_fini(env, ios);
}

enum cl_lock_mode vvp_mode_from_vma(struct vm_area_struct *vma)
{
        /*
         * we only want to hold PW locks if the mmap() can generate
         * writes back to the file and that only happens in shared
         * writable vmas
         */
        if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE))
                return CLM_WRITE;
        return CLM_READ;
}

static int vvp_mmap_locks(const struct lu_env *env,
                          struct ccc_io *vio, struct cl_io *io)
{
        struct ccc_thread_info *cti = ccc_env_info(env);
        struct vm_area_struct  *vma;
        struct cl_lock_descr   *descr = &cti->cti_descr;
        ldlm_policy_data_t      policy;
        unsigned long           addr;
        unsigned long           seg;
        ssize_t                 count;
        int                     result;
        ENTRY;

        LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);

        if (!cl_is_normalio(env, io))
                RETURN(0);

        for (seg = 0; seg < vio->cui_nrsegs; seg++) {
                const struct iovec *iv = &vio->cui_iov[seg];

                addr = (unsigned long)iv->iov_base;
                count = iv->iov_len;
                if (count == 0)
                        continue;

                count += addr & (~CFS_PAGE_MASK);
                addr &= CFS_PAGE_MASK;
                while((vma = our_vma(addr, count)) != NULL) {
                        struct inode *inode = vma->vm_file->f_dentry->d_inode;
                        int flags = CEF_MUST;

                        if (ll_file_nolock(vma->vm_file)) {
                                /* 
                                 * For no lock case, a lockless lock will be
                                 * generated.
                                 */
                                flags = CEF_NEVER;
                        }

                        /*
                         * XXX: Required lock mode can be weakened: CIT_WRITE
                         * io only ever reads user level buffer, and CIT_READ
                         * only writes on it.
                         */
                        policy_from_vma(&policy, vma, addr, count);
                        descr->cld_mode = vvp_mode_from_vma(vma);
                        descr->cld_obj = ll_i2info(inode)->lli_clob;
                        descr->cld_start = cl_index(descr->cld_obj,
                                                    policy.l_extent.start);
                        descr->cld_end = cl_index(descr->cld_obj,
                                                  policy.l_extent.end);
                        descr->cld_enq_flags = flags;
                        result = cl_io_lock_alloc_add(env, io, descr);

                        CDEBUG(D_VFSTRACE, "lock: %d: [%lu, %lu]\n",
                               descr->cld_mode, descr->cld_start,
                               descr->cld_end);

                        if (result < 0)
                                RETURN(result);

                        if (vma->vm_end - addr >= count)
                                break;

                        count -= vma->vm_end - addr;
                        addr = vma->vm_end;
                }
        }
        RETURN(0);
}

static int vvp_io_rw_lock(const struct lu_env *env, struct cl_io *io,
                          enum cl_lock_mode mode, loff_t start, loff_t end)
{
        struct ccc_io *cio = ccc_env_io(env);
        int result;
        int ast_flags = 0;

        LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);
        ENTRY;

        ccc_io_update_iov(env, cio, io);

        if (io->u.ci_rw.crw_nonblock)
                ast_flags |= CEF_NONBLOCK;
        result = vvp_mmap_locks(env, cio, io);
        if (result == 0)
                result = ccc_io_one_lock(env, io, ast_flags, mode, start, end);
        RETURN(result);
}

static int vvp_io_read_lock(const struct lu_env *env,
                            const struct cl_io_slice *ios)
{
        struct cl_io         *io  = ios->cis_io;
        struct ll_inode_info *lli = ll_i2info(ccc_object_inode(io->ci_obj));
        int result;

        ENTRY;
        /* XXX: Layer violation, we shouldn't see lsm at llite level. */
        if (lli->lli_smd != NULL) /* lsm-less file, don't need to lock */
                result = vvp_io_rw_lock(env, io, CLM_READ,
                                        io->u.ci_rd.rd.crw_pos,
                                        io->u.ci_rd.rd.crw_pos +
                                        io->u.ci_rd.rd.crw_count - 1);
        else
                result = 0;
        RETURN(result);
}

static int vvp_io_fault_lock(const struct lu_env *env,
                             const struct cl_io_slice *ios)
{
        struct cl_io *io   = ios->cis_io;
        struct vvp_io *vio = cl2vvp_io(env, ios);
        /*
         * XXX LDLM_FL_CBPENDING
         */
        return ccc_io_one_lock_index
                (env, io, 0, vvp_mode_from_vma(vio->u.fault.ft_vma),
                 io->u.ci_fault.ft_index, io->u.ci_fault.ft_index);
}

static int vvp_io_write_lock(const struct lu_env *env,
                             const struct cl_io_slice *ios)
{
        struct cl_io *io = ios->cis_io;
        loff_t start;
        loff_t end;

        if (io->u.ci_wr.wr_append) {
                start = 0;
                end   = OBD_OBJECT_EOF;
        } else {
                start = io->u.ci_wr.wr.crw_pos;
                end   = start + io->u.ci_wr.wr.crw_count - 1;
        }
        return vvp_io_rw_lock(env, io, CLM_WRITE, start, end);
}

static int vvp_io_setattr_iter_init(const struct lu_env *env,
                                    const struct cl_io_slice *ios)
{
        struct ccc_io *cio   = ccc_env_io(env);
        struct inode  *inode = ccc_object_inode(ios->cis_obj);

        /*
         * We really need to get our PW lock before we change inode->i_size.
         * If we don't we can race with other i_size updaters on our node,
         * like ll_file_read.  We can also race with i_size propogation to
         * other nodes through dirtying and writeback of final cached pages.
         * This last one is especially bad for racing o_append users on other
         * nodes.
         */
        UNLOCK_INODE_MUTEX(inode);
        if (cl_io_is_trunc(ios->cis_io))
                UP_WRITE_I_ALLOC_SEM(inode);
        cio->u.setattr.cui_locks_released = 1;
        return 0;
}

/**
 * Implementation of cl_io_operations::cio_lock() method for CIT_SETATTR io.
 *
 * Handles "lockless io" mode when extent locking is done by server.
 */
static int vvp_io_setattr_lock(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
        struct ccc_io      *cio       = ccc_env_io(env);
        struct cl_io       *io        = ios->cis_io;
        size_t              new_size;
        __u32               enqflags = 0;

        if (cl_io_is_trunc(io)) {
                new_size = io->u.ci_setattr.sa_attr.lvb_size;
                if (new_size == 0)
                        enqflags = CEF_DISCARD_DATA;
        } else {
                if ((io->u.ci_setattr.sa_attr.lvb_mtime >=
                     io->u.ci_setattr.sa_attr.lvb_ctime) ||
                    (io->u.ci_setattr.sa_attr.lvb_atime >=
                     io->u.ci_setattr.sa_attr.lvb_ctime))
                        return 0;
                new_size = 0;
        }
        cio->u.setattr.cui_local_lock = SETATTR_EXTENT_LOCK;
        return ccc_io_one_lock(env, io, enqflags, CLM_WRITE,
                               new_size, OBD_OBJECT_EOF);
}

static int vvp_do_vmtruncate(struct inode *inode, size_t size)
{
        int     result;
        /*
         * Only ll_inode_size_lock is taken at this level. lov_stripe_lock()
         * is grabbed by ll_truncate() only over call to obd_adjust_kms().
         */
        ll_inode_size_lock(inode, 0);
        result = vmtruncate(inode, size);
        ll_inode_size_unlock(inode, 0);

        return result;
}

static int vvp_io_setattr_trunc(const struct lu_env *env,
                                const struct cl_io_slice *ios,
                                struct inode *inode, loff_t size)
{
        struct vvp_io        *vio   = cl2vvp_io(env, ios);
        struct cl_io         *io    = ios->cis_io;
        struct cl_object     *obj   = ios->cis_obj;
        pgoff_t               start = cl_index(obj, size);
        int                   result;

        DOWN_WRITE_I_ALLOC_SEM(inode);

        result = vvp_do_vmtruncate(inode, size);

        /*
         * If a page is partially truncated, keep it owned across truncate to
         * prevent... races.
         *
         * XXX this properly belongs to osc, because races in question are OST
         * specific.
         */
        if (cl_offset(obj, start) != size) {
                struct cl_object_header *hdr;

                hdr = cl_object_header(obj);
                cfs_spin_lock(&hdr->coh_page_guard);
                vio->cui_partpage = cl_page_lookup(hdr, start);
                cfs_spin_unlock(&hdr->coh_page_guard);

                if (vio->cui_partpage != NULL)
                        /*
                         * Wait for the transfer completion for a partially
                         * truncated page to avoid dead-locking an OST with
                         * the concurrent page-wise overlapping WRITE and
                         * PUNCH requests. BUG:17397.
                         *
                         * Partial page is disowned in vvp_io_trunc_end().
                         */
                        cl_page_own(env, io, vio->cui_partpage);
        } else
                vio->cui_partpage = NULL;
        return result;
}

static int vvp_io_setattr_time(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
        struct cl_io       *io    = ios->cis_io;
        struct cl_object   *obj   = io->ci_obj;
        struct cl_attr     *attr  = ccc_env_thread_attr(env);
        int result;
        unsigned valid = CAT_CTIME;

        cl_object_attr_lock(obj);
        attr->cat_ctime = io->u.ci_setattr.sa_attr.lvb_ctime;
        if (io->u.ci_setattr.sa_valid & ATTR_ATIME_SET) {
                attr->cat_atime = io->u.ci_setattr.sa_attr.lvb_atime;
                valid |= CAT_ATIME;
        }
        if (io->u.ci_setattr.sa_valid & ATTR_MTIME_SET) {
                attr->cat_mtime = io->u.ci_setattr.sa_attr.lvb_mtime;
                valid |= CAT_MTIME;
        }
        result = cl_object_attr_set(env, obj, attr, valid);
        cl_object_attr_unlock(obj);

        return result;
}

static int vvp_io_setattr_start(const struct lu_env *env,
                                const struct cl_io_slice *ios)
{
        struct ccc_io        *cio   = cl2ccc_io(env, ios);
        struct cl_io         *io    = ios->cis_io;
        struct inode         *inode = ccc_object_inode(io->ci_obj);

        LASSERT(cio->u.setattr.cui_locks_released);

        LOCK_INODE_MUTEX(inode);
        cio->u.setattr.cui_locks_released = 0;

        if (cl_io_is_trunc(io))
                return vvp_io_setattr_trunc(env, ios, inode,
                                            io->u.ci_setattr.sa_attr.lvb_size);
        else
                return vvp_io_setattr_time(env, ios);
}

static void vvp_io_setattr_end(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
        struct vvp_io        *vio   = cl2vvp_io(env, ios);
        struct cl_io         *io    = ios->cis_io;
        struct inode         *inode = ccc_object_inode(io->ci_obj);

        if (!cl_io_is_trunc(io))
                return;
        if (vio->cui_partpage != NULL) {
                cl_page_disown(env, ios->cis_io, vio->cui_partpage);
                cl_page_put(env, vio->cui_partpage);
                vio->cui_partpage = NULL;
        }

        /*
         * Do vmtruncate again, to remove possible stale pages populated by
         * competing read threads. bz20645.
         */
        vvp_do_vmtruncate(inode, io->u.ci_setattr.sa_attr.lvb_size);
}

static void vvp_io_setattr_fini(const struct lu_env *env,
                                const struct cl_io_slice *ios)
{
        struct ccc_io *cio   = ccc_env_io(env);
        struct cl_io  *io    = ios->cis_io;
        struct inode  *inode = ccc_object_inode(ios->cis_io->ci_obj);

        if (cio->u.setattr.cui_locks_released) {
                LOCK_INODE_MUTEX(inode);
                if (cl_io_is_trunc(io))
                        DOWN_WRITE_I_ALLOC_SEM(inode);
                cio->u.setattr.cui_locks_released = 0;
        }
        vvp_io_fini(env, ios);
}

#ifdef HAVE_FILE_READV
static ssize_t lustre_generic_file_read(struct file *file,
                                        struct ccc_io *vio, loff_t *ppos)
{
        return generic_file_readv(file, vio->cui_iov, vio->cui_nrsegs, ppos);
}

static ssize_t lustre_generic_file_write(struct file *file,
                                         struct ccc_io *vio, loff_t *ppos)
{
        return generic_file_writev(file, vio->cui_iov, vio->cui_nrsegs, ppos);
}
#else
static ssize_t lustre_generic_file_read(struct file *file,
                                        struct ccc_io *vio, loff_t *ppos)
{
        return generic_file_aio_read(vio->cui_iocb, vio->cui_iov,
                                     vio->cui_nrsegs, *ppos);
}

static ssize_t lustre_generic_file_write(struct file *file,
                                        struct ccc_io *vio, loff_t *ppos)
{
        return generic_file_aio_write(vio->cui_iocb, vio->cui_iov,
                                      vio->cui_nrsegs, *ppos);
}
#endif

static int vvp_io_read_start(const struct lu_env *env,
                             const struct cl_io_slice *ios)
{
        struct vvp_io     *vio   = cl2vvp_io(env, ios);
        struct ccc_io     *cio   = cl2ccc_io(env, ios);
        struct cl_io      *io    = ios->cis_io;
        struct cl_object  *obj   = io->ci_obj;
        struct inode      *inode = ccc_object_inode(obj);
        struct ll_ra_read *bead  = &vio->cui_bead;
        struct file       *file  = cio->cui_fd->fd_file;

        int     result;
        loff_t  pos = io->u.ci_rd.rd.crw_pos;
        long    cnt = io->u.ci_rd.rd.crw_count;
        long    tot = cio->cui_tot_count;
        int     exceed = 0;

        CLOBINVRNT(env, obj, ccc_object_invariant(obj));

        CDEBUG(D_VFSTRACE, "read: -> [%lli, %lli)\n", pos, pos + cnt);

        result = ccc_prep_size(env, obj, io, pos, tot, &exceed);
        if (result != 0)
                return result;
        else if (exceed != 0)
                goto out;

        LU_OBJECT_HEADER(D_INODE, env, &obj->co_lu,
                        "Read ino %lu, %lu bytes, offset %lld, size %llu\n",
                        inode->i_ino, cnt, pos, i_size_read(inode));

        /* turn off the kernel's read-ahead */
        cio->cui_fd->fd_file->f_ra.ra_pages = 0;

        /* initialize read-ahead window once per syscall */
        if (!vio->cui_ra_window_set) {
                vio->cui_ra_window_set = 1;
                bead->lrr_start = cl_index(obj, pos);
                /*
                 * XXX: explicit CFS_PAGE_SIZE
                 */
                bead->lrr_count = cl_index(obj, tot + CFS_PAGE_SIZE - 1);
                ll_ra_read_in(file, bead);
        }

        /* BUG: 5972 */
        file_accessed(file);
        switch (vio->cui_io_subtype) {
        case IO_NORMAL:
                 result = lustre_generic_file_read(file, cio, &pos);
                 break;
#ifdef HAVE_KERNEL_SENDFILE
        case IO_SENDFILE:
                result = generic_file_sendfile(file, &pos, cnt,
                                vio->u.sendfile.cui_actor,
                                vio->u.sendfile.cui_target);
                break;
#endif
#ifdef HAVE_KERNEL_SPLICE_READ
        case IO_SPLICE:
                result = generic_file_splice_read(file, &pos,
                                vio->u.splice.cui_pipe, cnt,
                                vio->u.splice.cui_flags);
                /* LU-1109: do splice read stripe by stripe otherwise if it
                 * may make nfsd stuck if this read occupied all internal pipe
                 * buffers. */
                io->ci_continue = 0;
                break;
#endif
        default:
                CERROR("Wrong IO type %u\n", vio->cui_io_subtype);
                LBUG();
        }

out:
        if (result >= 0) {
                if (result < cnt)
                        io->ci_continue = 0;
                io->ci_nob += result;
                ll_rw_stats_tally(ll_i2sbi(inode), current->pid,
                                  cio->cui_fd, pos, result, 0);
                result = 0;
        }
        return result;
}

static int vvp_io_write_start(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
        struct ccc_io      *cio   = cl2ccc_io(env, ios);
        struct cl_io       *io    = ios->cis_io;
        struct cl_object   *obj   = io->ci_obj;
        struct inode       *inode = ccc_object_inode(obj);
        struct file        *file  = cio->cui_fd->fd_file;
        ssize_t result = 0;
        loff_t pos = io->u.ci_wr.wr.crw_pos;
        size_t cnt = io->u.ci_wr.wr.crw_count;

        ENTRY;

        if (cl_io_is_append(io)) {
                /*
                 * PARALLEL IO This has to be changed for parallel IO doing
                 * out-of-order writes.
                 */
                pos = io->u.ci_wr.wr.crw_pos = i_size_read(inode);
#ifndef HAVE_FILE_WRITEV
                cio->cui_iocb->ki_pos = pos;
#endif
        }

        CDEBUG(D_VFSTRACE, "write: [%lli, %lli)\n", pos, pos + (long long)cnt);

        if (cio->cui_iov == NULL) /* from a temp io in ll_cl_init(). */
                result = 0;
        else
                result = lustre_generic_file_write(file, cio, &pos);

        if (result > 0) {
                if (result < cnt)
                        io->ci_continue = 0;
                io->ci_nob += result;
                ll_rw_stats_tally(ll_i2sbi(inode), current->pid,
                                  cio->cui_fd, pos, result, 0);
                result = 0;
        }
        RETURN(result);
}

#ifndef HAVE_VM_OP_FAULT
static int vvp_io_kernel_fault(struct vvp_fault_io *cfio)
{
        cfs_page_t *vmpage;

        vmpage = filemap_nopage(cfio->ft_vma, cfio->nopage.ft_address,
                                cfio->nopage.ft_type);

        if (vmpage == NOPAGE_SIGBUS) {
                CDEBUG(D_PAGE, "got addr %lu type %lx - SIGBUS\n",
                       cfio->nopage.ft_address,(long)cfio->nopage.ft_type);
                return -EFAULT;
        } else if (vmpage == NOPAGE_OOM) {
                CDEBUG(D_PAGE, "got addr %lu type %lx - OOM\n",
                       cfio->nopage.ft_address, (long)cfio->nopage.ft_type);
                return -ENOMEM;
        }

        LL_CDEBUG_PAGE(D_PAGE, vmpage, "got addr %lu type %lx\n",
                       cfio->nopage.ft_address, (long)cfio->nopage.ft_type);

        cfio->ft_vmpage = vmpage;

        return 0;
}
#else
static int vvp_io_kernel_fault(struct vvp_fault_io *cfio)
{
        cfio->fault.ft_flags = filemap_fault(cfio->ft_vma, cfio->fault.ft_vmf);

        if (cfio->fault.ft_vmf->page) {
                LL_CDEBUG_PAGE(D_PAGE, cfio->fault.ft_vmf->page,
                               "got addr %p type NOPAGE\n",
                               cfio->fault.ft_vmf->virtual_address);
                /*XXX workaround to bug in CLIO - he deadlocked with
                 lock cancel if page locked  */
                if (likely(cfio->fault.ft_flags & VM_FAULT_LOCKED)) {
                        unlock_page(cfio->fault.ft_vmf->page);
                        cfio->fault.ft_flags &= ~VM_FAULT_LOCKED;
                }

                cfio->ft_vmpage = cfio->fault.ft_vmf->page;
                return 0;
        }

        if (unlikely (cfio->fault.ft_flags & VM_FAULT_ERROR)) {
                CDEBUG(D_PAGE, "got addr %p - SIGBUS\n",
                       cfio->fault.ft_vmf->virtual_address);
                return -EFAULT;
        }

        if (unlikely (cfio->fault.ft_flags & VM_FAULT_NOPAGE)) {
                CDEBUG(D_PAGE, "got addr %p - OOM\n",
                       cfio->fault.ft_vmf->virtual_address);
                return -ENOMEM;
        }

        if (unlikely(cfio->fault.ft_flags & VM_FAULT_RETRY))
                return -EAGAIN;

        CERROR("unknow error in page fault!\n");
        return -EINVAL;
}

#endif

static int vvp_io_fault_start(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
        struct vvp_io       *vio     = cl2vvp_io(env, ios);
        struct cl_io        *io      = ios->cis_io;
        struct cl_object    *obj     = io->ci_obj;
        struct inode        *inode   = ccc_object_inode(obj);
        struct cl_fault_io  *fio     = &io->u.ci_fault;
        struct vvp_fault_io *cfio    = &vio->u.fault;
        loff_t               offset;
        int                  kernel_result = 0;
        int                  result  = 0;
        struct cl_page      *page;
        loff_t               size;
        pgoff_t              last; /* last page in a file data region */

        if (fio->ft_executable &&
            LTIME_S(inode->i_mtime) != vio->u.fault.ft_mtime)
                CWARN("binary "DFID
                      " changed while waiting for the page fault lock\n",
                      PFID(lu_object_fid(&obj->co_lu)));

        /* offset of the last byte on the page */
        offset = cl_offset(obj, fio->ft_index + 1) - 1;
        LASSERT(cl_index(obj, offset) == fio->ft_index);
        result = ccc_prep_size(env, obj, io, 0, offset + 1, NULL);
        if (result != 0)
                return result;

        /* must return unlocked page */
        kernel_result = vvp_io_kernel_fault(cfio);
        if (kernel_result != 0)
                return kernel_result;

        if (OBD_FAIL_CHECK(OBD_FAIL_LLITE_FAULT_TRUNC_RACE)) {
                truncate_inode_pages_range(inode->i_mapping,
                                         cl_offset(obj, fio->ft_index), offset);
        }

        /* Temporarily lock vmpage to keep cl_page_find() happy. */
        lock_page(cfio->ft_vmpage);

        /* Though we have already held a cl_lock upon this page, but
         * it still can be truncated locally. */
        if (unlikely(cfio->ft_vmpage->mapping == NULL)) {
                unlock_page(cfio->ft_vmpage);

                CDEBUG(D_PAGE, "llite: fault and truncate race happened!\n");

                /* return +1 to stop cl_io_loop() and ll_fault() will catch
                 * and retry. */
                return +1;
        }

        page = cl_page_find(env, obj, fio->ft_index, cfio->ft_vmpage,
                            CPT_CACHEABLE);
        unlock_page(cfio->ft_vmpage);
        if (IS_ERR(page)) {
                page_cache_release(cfio->ft_vmpage);
                cfio->ft_vmpage = NULL;
                return PTR_ERR(page);
        }

        size = i_size_read(inode);
        last = cl_index(obj, size - 1);
        if (fio->ft_index == last)
                /*
                 * Last page is mapped partially.
                 */
                fio->ft_nob = size - cl_offset(obj, fio->ft_index);
         else
                fio->ft_nob = cl_page_size(obj);

         lu_ref_add(&page->cp_reference, "fault", io);
         fio->ft_page = page;
         /*
          * Certain 2.6 kernels return not-NULL from
          * filemap_nopage() when page is beyond the file size,
          * on the grounds that "An external ptracer can access
          * pages that normally aren't accessible.." Don't
          * propagate such page fault to the lower layers to
          * avoid side-effects like KMS updates.
          */
          if (fio->ft_index > last)
                result = +1;

        return result;
}

static int vvp_io_read_page(const struct lu_env *env,
                            const struct cl_io_slice *ios,
                            const struct cl_page_slice *slice)
{
        struct cl_io              *io     = ios->cis_io;
        struct cl_object          *obj    = slice->cpl_obj;
        struct ccc_page           *cp     = cl2ccc_page(slice);
        struct cl_page            *page   = slice->cpl_page;
        struct inode              *inode  = ccc_object_inode(obj);
        struct ll_sb_info         *sbi    = ll_i2sbi(inode);
        struct ll_file_data       *fd     = cl2ccc_io(env, ios)->cui_fd;
        struct ll_readahead_state *ras    = &fd->fd_ras;
        cfs_page_t                *vmpage = cp->cpg_page;
        struct cl_2queue          *queue  = &io->ci_queue;
        int rc;

        CLOBINVRNT(env, obj, ccc_object_invariant(obj));
        LASSERT(slice->cpl_obj == obj);

        ENTRY;

        if (sbi->ll_ra_info.ra_max_pages_per_file)
                ras_update(sbi, inode, ras, page->cp_index,
                           cp->cpg_defer_uptodate);

        /* Sanity check whether the page is protected by a lock. */
        rc = cl_page_is_under_lock(env, io, page);
        if (rc != -EBUSY) {
                CL_PAGE_HEADER(D_WARNING, env, page, "%s: %d\n",
                               rc == -ENODATA ? "without a lock" :
                               "match failed", rc);
                if (rc != -ENODATA)
                        RETURN(rc);
        }

        if (cp->cpg_defer_uptodate) {
                cp->cpg_ra_used = 1;
                cl_page_export(env, page, 1);
        }
        /*
         * Add page into the queue even when it is marked uptodate above.
         * this will unlock it automatically as part of cl_page_list_disown().
         */
        cl_2queue_add(queue, page);
        if (sbi->ll_ra_info.ra_max_pages_per_file)
                ll_readahead(env, io, ras,
                             vmpage->mapping, &queue->c2_qin, fd->fd_flags);

        RETURN(0);
}

static int vvp_page_sync_io(const struct lu_env *env, struct cl_io *io,
                            struct cl_page *page, struct ccc_page *cp,
                            enum cl_req_type crt)
{
        struct cl_2queue  *queue;
        int result;

        LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);

        queue = &io->ci_queue;
        cl_2queue_init_page(queue, page);

        result = cl_io_submit_sync(env, io, crt, queue, CRP_NORMAL, 0);
        LASSERT(cl_page_is_owned(page, io));

        if (crt == CRT_READ)
                /*
                 * in CRT_WRITE case page is left locked even in case of
                 * error.
                 */
                cl_page_list_disown(env, io, &queue->c2_qin);
        cl_2queue_fini(env, queue);

        return result;
}

/**
 * Prepare partially written-to page for a write.
 */
static int vvp_io_prepare_partial(const struct lu_env *env, struct cl_io *io,
                                  struct cl_object *obj, struct cl_page *pg,
                                  struct ccc_page *cp,
                                  unsigned from, unsigned to)
{
        struct cl_attr *attr   = ccc_env_thread_attr(env);
        loff_t          offset = cl_offset(obj, pg->cp_index);
        int             result;

        cl_object_attr_lock(obj);
        result = cl_object_attr_get(env, obj, attr);
        cl_object_attr_unlock(obj);
        if (result == 0) {
                /*
                 * If are writing to a new page, no need to read old data.
                 * The extent locking will have updated the KMS, and for our
                 * purposes here we can treat it like i_size.
                 */
                if (attr->cat_kms <= offset) {
                        char *kaddr = kmap_atomic(cp->cpg_page, KM_USER0);

                        memset(kaddr, 0, cl_page_size(obj));
                        kunmap_atomic(kaddr, KM_USER0);
                } else if (cp->cpg_defer_uptodate)
                        cp->cpg_ra_used = 1;
                else
                        result = vvp_page_sync_io(env, io, pg, cp, CRT_READ);
                /*
                 * In older implementations, obdo_refresh_inode is called here
                 * to update the inode because the write might modify the
                 * object info at OST. However, this has been proven useless,
                 * since LVB functions will be called when user space program
                 * tries to retrieve inode attribute.  Also, see bug 15909 for
                 * details. -jay
                 */
                if (result == 0)
                        cl_page_export(env, pg, 1);
        }
        return result;
}

static int vvp_io_prepare_write(const struct lu_env *env,
                                const struct cl_io_slice *ios,
                                const struct cl_page_slice *slice,
                                unsigned from, unsigned to)
{
        struct cl_object *obj    = slice->cpl_obj;
        struct ccc_page  *cp     = cl2ccc_page(slice);
        struct cl_page   *pg     = slice->cpl_page;
        cfs_page_t       *vmpage = cp->cpg_page;

        int result;

        ENTRY;

        LINVRNT(cl_page_is_vmlocked(env, pg));
        LASSERT(vmpage->mapping->host == ccc_object_inode(obj));

        result = 0;

        CL_PAGE_HEADER(D_PAGE, env, pg, "preparing: [%d, %d]\n", from, to);
        if (!PageUptodate(vmpage)) {
                /*
                 * We're completely overwriting an existing page, so _don't_
                 * set it up to date until commit_write
                 */
                if (from == 0 && to == CFS_PAGE_SIZE) {
                        CL_PAGE_HEADER(D_PAGE, env, pg, "full page write\n");
                        POISON_PAGE(page, 0x11);
                } else
                        result = vvp_io_prepare_partial(env, ios->cis_io, obj,
                                                        pg, cp, from, to);
        } else
                CL_PAGE_HEADER(D_PAGE, env, pg, "uptodate\n");
        RETURN(result);
}

static int vvp_io_commit_write(const struct lu_env *env,
                               const struct cl_io_slice *ios,
                               const struct cl_page_slice *slice,
                               unsigned from, unsigned to)
{
        struct cl_object  *obj    = slice->cpl_obj;
        struct cl_io      *io     = ios->cis_io;
        struct ccc_page   *cp     = cl2ccc_page(slice);
        struct cl_page    *pg     = slice->cpl_page;
        struct inode      *inode  = ccc_object_inode(obj);
        struct ll_sb_info *sbi    = ll_i2sbi(inode);
        cfs_page_t        *vmpage = cp->cpg_page;

        int    result;
        int    tallyop;
        loff_t size;

        ENTRY;

        LINVRNT(cl_page_is_vmlocked(env, pg));
        LASSERT(vmpage->mapping->host == inode);

        LU_OBJECT_HEADER(D_INODE, env, &obj->co_lu, "commiting page write\n");
        CL_PAGE_HEADER(D_PAGE, env, pg, "committing: [%d, %d]\n", from, to);

        /*
         * queue a write for some time in the future the first time we
         * dirty the page.
         *
         * This is different from what other file systems do: they usually
         * just mark page (and some of its buffers) dirty and rely on
         * balance_dirty_pages() to start a write-back. Lustre wants write-back
         * to be started earlier for the following reasons:
         *
         *     (1) with a large number of clients we need to limit the amount
         *     of cached data on the clients a lot;
         *
         *     (2) large compute jobs generally want compute-only then io-only
         *     and the IO should complete as quickly as possible;
         *
         *     (3) IO is batched up to the RPC size and is async until the
         *     client max cache is hit
         *     (/proc/fs/lustre/osc/OSC.../max_dirty_mb)
         *
         */
        if (!PageDirty(vmpage)) {
                tallyop = LPROC_LL_DIRTY_MISSES;
                result = cl_page_cache_add(env, io, pg, CRT_WRITE);
                if (result == 0) {
                        /* page was added into cache successfully. */
                        set_page_dirty(vmpage);
                        vvp_write_pending(cl2ccc(obj), cp);
                } else if (result == -EDQUOT) {
                        pgoff_t last_index = i_size_read(inode) >> CFS_PAGE_SHIFT;
                        bool need_clip = true;

                        /*
                         * Client ran out of disk space grant. Possible
                         * strategies are:
                         *
                         *     (a) do a sync write, renewing grant;
                         *
                         *     (b) stop writing on this stripe, switch to the
                         *     next one.
                         *
                         * (b) is a part of "parallel io" design that is the
                         * ultimate goal. (a) is what "old" client did, and
                         * what the new code continues to do for the time
                         * being.
                         */
                        if (last_index > pg->cp_index) {
                                to = CFS_PAGE_SIZE;
                                need_clip = false;
                        } else if (last_index == pg->cp_index) {
                                int size_to = i_size_read(inode) & ~CFS_PAGE_MASK;
                                if (to < size_to)
                                        to = size_to;
                        }
                        if (need_clip)
                                cl_page_clip(env, pg, 0, to);

                        set_page_dirty(vmpage);
                        result = vvp_page_sync_io(env, io, pg, cp, CRT_WRITE);
                        if (result)
                                CERROR("Write page %lu of inode %p failed %d\n",
                                       pg->cp_index, inode, result);
                }
        } else {
                tallyop = LPROC_LL_DIRTY_HITS;
                result = 0;
        }
        ll_stats_ops_tally(sbi, tallyop, 1);

        size = cl_offset(obj, pg->cp_index) + to;

        ll_inode_size_lock(inode, 0);
        if (result == 0) {
                if (size > i_size_read(inode)) {
                        cl_isize_write_nolock(inode, size);
                        CDEBUG(D_VFSTRACE, DFID" updating i_size %lu\n",
                               PFID(lu_object_fid(&obj->co_lu)),
                               (unsigned long)size);
                }
                cl_page_export(env, pg, 1);
        } else {
                if (size > i_size_read(inode))
                        cl_page_discard(env, io, pg);
        }
        ll_inode_size_unlock(inode, 0);
        RETURN(result);
}

static const struct cl_io_operations vvp_io_ops = {
        .op = {
                [CIT_READ] = {
                        .cio_fini      = vvp_io_fini,
                        .cio_lock      = vvp_io_read_lock,
                        .cio_start     = vvp_io_read_start,
                        .cio_advance   = ccc_io_advance
                },
                [CIT_WRITE] = {
                        .cio_fini      = vvp_io_fini,
                        .cio_lock      = vvp_io_write_lock,
                        .cio_start     = vvp_io_write_start,
                        .cio_advance   = ccc_io_advance
                },
                [CIT_SETATTR] = {
                        .cio_fini       = vvp_io_setattr_fini,
                        .cio_iter_init  = vvp_io_setattr_iter_init,
                        .cio_lock       = vvp_io_setattr_lock,
                        .cio_start      = vvp_io_setattr_start,
                        .cio_end        = vvp_io_setattr_end
                },
                [CIT_FAULT] = {
                        .cio_fini      = vvp_io_fault_fini,
                        .cio_iter_init = vvp_io_fault_iter_init,
                        .cio_lock      = vvp_io_fault_lock,
                        .cio_start     = vvp_io_fault_start,
                        .cio_end       = ccc_io_end
                },
                [CIT_MISC] = {
                        .cio_fini   = vvp_io_fini
                }
        },
        .cio_read_page     = vvp_io_read_page,
        .cio_prepare_write = vvp_io_prepare_write,
        .cio_commit_write  = vvp_io_commit_write
};

int vvp_io_init(const struct lu_env *env, struct cl_object *obj,
                struct cl_io *io)
{
        struct vvp_io      *vio   = vvp_env_io(env);
        struct ccc_io      *cio   = ccc_env_io(env);
        struct inode       *inode = ccc_object_inode(obj);
        struct ll_sb_info  *sbi   = ll_i2sbi(inode);
        int                 result;

        CLOBINVRNT(env, obj, ccc_object_invariant(obj));
        ENTRY;

        CL_IO_SLICE_CLEAN(cio, cui_cl);
        cl_io_slice_add(io, &cio->cui_cl, obj, &vvp_io_ops);
        vio->cui_ra_window_set = 0;
        result = 0;
        if (io->ci_type == CIT_READ || io->ci_type == CIT_WRITE) {
                size_t count;

                count = io->u.ci_rw.crw_count;
                /* "If nbyte is 0, read() will return 0 and have no other
                 *  results."  -- Single Unix Spec */
                if (count == 0)
                        result = 1;
                else {
                        cio->cui_tot_count = count;
                        cio->cui_tot_nrsegs = 0;
                }
        } else if (io->ci_type == CIT_SETATTR) {
                if (cl_io_is_trunc(io))
                        /* lockless truncate? */
                        ll_stats_ops_tally(sbi, LPROC_LL_TRUNC, 1);
                else
                        io->ci_lockreq = CILR_MANDATORY;
        }
        RETURN(result);
}

static struct vvp_io *cl2vvp_io(const struct lu_env *env,
                                const struct cl_io_slice *slice)
{
        /* Caling just for assertion */
        cl2ccc_io(env, slice);
        return vvp_env_io(env);
}

