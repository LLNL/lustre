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
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/lvfs/fsfilt_ext3.c
 *
 * Author: Andreas Dilger <adilger@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#ifdef HAVE_EXT4_LDISKFS
#include <ext4/ext4.h>
#include <ext4/ext4_jbd2.h>
#else
#include <linux/jbd.h>
#include <linux/ext3_fs.h>
#include <linux/ext3_jbd.h>
#endif
#include <linux/version.h>
#include <linux/bitops.h>
#include <linux/quota.h>

#include <libcfs/libcfs.h>
#include <lustre_fsfilt.h>
#include <obd.h>
#include <lustre_quota.h>
#include <linux/lustre_compat25.h>
#include <linux/lprocfs_status.h>

#ifdef HAVE_EXT4_LDISKFS
#include <ext4/ext4_extents.h>
#else
#include <linux/ext3_extents.h>
#endif

#include "lustre_quota_fmt.h"

#define fsfilt_ext3_ext_insert_extent(handle, inode, path, newext, flag) \
               ext3_ext_insert_extent(handle, inode, path, newext, flag)

#define ext3_mb_discard_inode_preallocations(inode) \
                 ext3_discard_preallocations(inode)


static cfs_mem_cache_t *fcb_cache;

struct fsfilt_cb_data {
        struct journal_callback cb_jcb; /* jbd private data - MUST BE FIRST */
        fsfilt_cb_t cb_func;            /* MDS/OBD completion function */
        struct obd_device *cb_obd;      /* MDS/OBD completion device */
        __u64 cb_last_rcvd;             /* MDS/OST last committed operation */
        void *cb_data;                  /* MDS/OST completion function data */
};

#ifndef HAVE_EXT4_LDISKFS
#define ext_pblock(ex) le32_to_cpu((ex)->ee_start)
#define ext3_ext_store_pblock(ex, pblock)  ((ex)->ee_start = cpu_to_le32(pblock))
#endif

#ifndef EXT3_EXTENTS_FL
#define EXT3_EXTENTS_FL                 0x00080000 /* Inode uses extents */
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17))
# define fsfilt_up_truncate_sem(inode)  up(&LDISKFS_I(inode)->truncate_sem);
# define fsfilt_down_truncate_sem(inode)  down(&LDISKFS_I(inode)->truncate_sem);
#else
# ifdef HAVE_EXT4_LDISKFS
#  define fsfilt_up_truncate_sem(inode) do{ }while(0)
#  define fsfilt_down_truncate_sem(inode) do{ }while(0)
# else
#  define fsfilt_up_truncate_sem(inode)  mutex_unlock(&EXT3_I(inode)->truncate_mutex)
#  define fsfilt_down_truncate_sem(inode)  mutex_lock(&EXT3_I(inode)->truncate_mutex)
# endif
#endif

#ifndef EXT_ASSERT
#define EXT_ASSERT(cond)  BUG_ON(!(cond))
#endif

#ifdef EXT3_EXT_HAS_NO_TREE
/* for kernels 2.6.18 and later */
#ifdef HAVE_EXT4_LDISKFS
#define EXT_GENERATION(inode)           (EXT4_I(inode)->i_ext_generation)
#else
#define EXT_GENERATION(inode)           ext_generation(inode)
#endif
#define ext3_ext_base                   inode
#define ext3_ext_base2inode(inode)      (inode)
#define EXT_DEPTH(inode)                ext_depth(inode)
#define fsfilt_ext3_ext_walk_space(inode, block, num, cb, cbdata) \
                        ext3_ext_walk_space(inode, block, num, cb, cbdata);
#else
#define ext3_ext_base                   ext3_extents_tree
#define ext3_ext_base2inode(tree)       (tree->inode)
#define fsfilt_ext3_ext_walk_space(tree, block, num, cb, cbdata) \
                        ext3_ext_walk_space(tree, block, num, cb);
#endif

struct bpointers {
        unsigned long *blocks;
        unsigned long start;
        int num;
        int init_num;
        int create;
};

static long ext3_ext_find_goal(struct inode *inode, struct ext3_ext_path *path,
                               unsigned long block, int *aflags)
{
        struct ext3_inode_info *ei = EXT3_I(inode);
        unsigned long bg_start;
        unsigned long colour;
        int depth;

        if (path) {
                struct ext3_extent *ex;
                depth = path->p_depth;

                /* try to predict block placement */
                if ((ex = path[depth].p_ext))
                        return ext_pblock(ex) + (block - le32_to_cpu(ex->ee_block));

                /* it looks index is empty
                 * try to find starting from index itself */
                if (path[depth].p_bh)
                        return path[depth].p_bh->b_blocknr;
        }

        /* OK. use inode's group */
        bg_start = (ei->i_block_group * EXT3_BLOCKS_PER_GROUP(inode->i_sb)) +
                le32_to_cpu(EXT3_SB(inode->i_sb)->s_es->s_first_data_block);
        colour = (current->pid % 16) *
                (EXT3_BLOCKS_PER_GROUP(inode->i_sb) / 16);
        return bg_start + colour + block;
}

#define ll_unmap_underlying_metadata(sb, blocknr) \
        unmap_underlying_metadata((sb)->s_bdev, blocknr)

#ifndef EXT3_MB_HINT_GROUP_ALLOC
static unsigned long new_blocks(handle_t *handle, struct ext3_ext_base *base,
                                struct ext3_ext_path *path, unsigned long block,
                                unsigned long *count, int *err)
{
        unsigned long pblock, goal;
        int aflags = 0;
        struct inode *inode = ext3_ext_base2inode(base);

        goal = ext3_ext_find_goal(inode, path, block, &aflags);
        aflags |= 2; /* block have been already reserved */
        pblock = ext3_mb_new_blocks(handle, inode, goal, count, aflags, err);
        return pblock;

}
#else
static unsigned long new_blocks(handle_t *handle, struct ext3_ext_base *base,
                                struct ext3_ext_path *path, unsigned long block,
                                unsigned long *count, int *err)
{
        struct inode *inode = ext3_ext_base2inode(base);
        struct ext3_allocation_request ar;
        unsigned long pblock;
        int aflags;

        /* find neighbour allocated blocks */
        ar.lleft = block;
        *err = ext3_ext_search_left(base, path, &ar.lleft, &ar.pleft);
        if (*err)
                return 0;
        ar.lright = block;
        *err = ext3_ext_search_right(base, path, &ar.lright, &ar.pright);
        if (*err)
                return 0;

        /* allocate new block */
        ar.goal = ext3_ext_find_goal(inode, path, block, &aflags);
        ar.inode = inode;
        ar.logical = block;
        ar.len = *count;
        ar.flags = EXT3_MB_HINT_DATA;
        pblock = ext3_mb_new_blocks(handle, &ar, err);
        *count = ar.len;
        return pblock;
}
#endif

#ifdef EXT3_EXT_HAS_NO_TREE
static int ext3_ext_new_extent_cb(struct ext3_ext_base *base,
                                  struct ext3_ext_path *path,
                                  struct ext3_ext_cache *cex,
#ifdef HAVE_EXT_PREPARE_CB_EXTENT
                                   struct ext3_extent *ex,
#endif
                                  void *cbdata)
{
        struct bpointers *bp = cbdata;
#else
static int ext3_ext_new_extent_cb(struct ext3_ext_base *base,
                                  struct ext3_ext_path *path,
                                  struct ext3_ext_cache *cex
#ifdef HAVE_EXT_PREPARE_CB_EXTENT
                                  , struct ext3_extent *ex
#endif
                                 )
{
        struct bpointers *bp = base->private;
#endif
        struct inode *inode = ext3_ext_base2inode(base);
        struct ext3_extent nex;
        unsigned long pblock;
        unsigned long tgen;
        int err, i, depth;
        unsigned long count;
        handle_t *handle;

        i = depth = EXT_DEPTH(base);
        EXT_ASSERT(i == path->p_depth);
        EXT_ASSERT(path[i].p_hdr);

        if (cex->ec_type == EXT3_EXT_CACHE_EXTENT) {
                err = EXT_CONTINUE;
                goto map;
        }

        if (bp->create == 0) {
                i = 0;
                if (cex->ec_block < bp->start)
                        i = bp->start - cex->ec_block;
                if (i >= cex->ec_len)
                        CERROR("nothing to do?! i = %d, e_num = %u\n",
                                        i, cex->ec_len);
                for (; i < cex->ec_len && bp->num; i++) {
                        *(bp->blocks) = 0;
                        bp->blocks++;
                        bp->num--;
                        bp->start++;
                }

                return EXT_CONTINUE;
        }

        tgen = EXT_GENERATION(base);
        count = ext3_ext_calc_credits_for_insert(base, path);
        fsfilt_up_truncate_sem(inode);

        handle = ext3_journal_start(inode, count+EXT3_ALLOC_NEEDED+1);
        if (IS_ERR(handle)) {
                fsfilt_down_truncate_sem(inode);
                return PTR_ERR(handle);
        }

        fsfilt_down_truncate_sem(inode);
        if (tgen != EXT_GENERATION(base)) {
                /* the tree has changed. so path can be invalid at moment */
                ext3_journal_stop(handle);
                return EXT_REPEAT;
        }

#if defined(HAVE_EXT4_LDISKFS)
        /* In 2.6.32 kernel, ext4_ext_walk_space()'s callback func is not
         * protected by i_data_sem as whole. so we patch it to store
         * generation to path and now verify the tree hasn't changed */
        down_write((&EXT4_I(inode)->i_data_sem));

        /* validate extent, make sure the extent tree does not changed */
        if (EXT_GENERATION(base) != path[0].p_generation) {
                /* cex is invalid, try again */
                up_write(&EXT4_I(inode)->i_data_sem);
                ext3_journal_stop(handle);
                return EXT_REPEAT;
        }
#endif

        count = cex->ec_len;
        pblock = new_blocks(handle, base, path, cex->ec_block, &count, &err);
        if (!pblock)
                goto out;
        EXT_ASSERT(count <= cex->ec_len);

        /* insert new extent */
        nex.ee_block = cpu_to_le32(cex->ec_block);
        ext3_ext_store_pblock(&nex, pblock);
        nex.ee_len = cpu_to_le16(count);
        err = fsfilt_ext3_ext_insert_extent(handle, base, path, &nex, 0);
        if (err) {
                /* free data blocks we just allocated */
                /* not a good idea to call discard here directly,
                 * but otherwise we'd need to call it every free() */
#ifdef EXT3_MB_HINT_GROUP_ALLOC
                ext3_mb_discard_inode_preallocations(inode);
#endif
                ext3_free_blocks(handle, inode, ext_pblock(&nex),
                                 cpu_to_le16(nex.ee_len), 0);
                goto out;
        }

        /*
         * Putting len of the actual extent we just inserted,
         * we are asking ext3_ext_walk_space() to continue
         * scaning after that block
         */
        cex->ec_len = le16_to_cpu(nex.ee_len);
        cex->ec_start = ext_pblock(&nex);
        BUG_ON(le16_to_cpu(nex.ee_len) == 0);
        BUG_ON(le32_to_cpu(nex.ee_block) != cex->ec_block);

out:
#if defined(HAVE_EXT4_LDISKFS)
        up_write((&EXT4_I(inode)->i_data_sem));
#endif
        ext3_journal_stop(handle);
map:
        if (err >= 0) {
                /* map blocks */
                if (bp->num == 0) {
                        CERROR("hmm. why do we find this extent?\n");
                        CERROR("initial space: %lu:%u\n",
                                bp->start, bp->init_num);
                        CERROR("current extent: %u/%u/%llu %d\n",
                                cex->ec_block, cex->ec_len,
                                (unsigned long long)cex->ec_start,
                                cex->ec_type);
                }
                i = 0;
                if (cex->ec_block < bp->start)
                        i = bp->start - cex->ec_block;
                if (i >= cex->ec_len)
                        CERROR("nothing to do?! i = %d, e_num = %u\n",
                                        i, cex->ec_len);
                for (; i < cex->ec_len && bp->num; i++) {
                        *(bp->blocks) = cex->ec_start + i;
                        if (cex->ec_type != EXT3_EXT_CACHE_EXTENT) {
                                /* unmap any possible underlying metadata from
                                 * the block device mapping.  bug 6998. */
                                ll_unmap_underlying_metadata(inode->i_sb,
                                                             *(bp->blocks));
                        }
                        bp->blocks++;
                        bp->num--;
                        bp->start++;
                }
        }
        return err;
}

int fsfilt_map_nblocks(struct inode *inode, unsigned long block,
                       unsigned long num, unsigned long *blocks,
                       int *created, int create)
{
#ifdef EXT3_EXT_HAS_NO_TREE
        struct ext3_ext_base *base = inode;
#else
        struct ext3_extents_tree tree;
        struct ext3_ext_base *base = &tree;
#endif
        struct bpointers bp;
        int err;

        CDEBUG(D_OTHER, "blocks %lu-%lu requested for inode %u\n",
               block, block + num - 1, (unsigned) inode->i_ino);

#ifndef EXT3_EXT_HAS_NO_TREE
        ext3_init_tree_desc(base, inode);
        tree.private = &bp;
#endif
        bp.blocks = blocks;
        bp.start = block;
        bp.init_num = bp.num = num;
        bp.create = create;

        fsfilt_down_truncate_sem(inode);
        err = fsfilt_ext3_ext_walk_space(base, block, num,
                                         ext3_ext_new_extent_cb, &bp);
        ext3_ext_invalidate_cache(base);
        fsfilt_up_truncate_sem(inode);

        return err;
}

int fsfilt_ext3_map_ext_inode_pages(struct inode *inode, struct page **page,
                                    int pages, unsigned long *blocks,
                                    int *created, int create)
{
        int blocks_per_page = CFS_PAGE_SIZE >> inode->i_blkbits;
        int rc = 0, i = 0;
        struct page *fp = NULL;
        int clen = 0;

        CDEBUG(D_OTHER, "inode %lu: map %d pages from %lu\n",
                inode->i_ino, pages, (*page)->index);

        /* pages are sorted already. so, we just have to find
         * contig. space and process them properly */
        while (i < pages) {
                if (fp == NULL) {
                        /* start new extent */
                        fp = *page++;
                        clen = 1;
                        i++;
                        continue;
                } else if (fp->index + clen == (*page)->index) {
                        /* continue the extent */
                        page++;
                        clen++;
                        i++;
                        continue;
                }

                /* process found extent */
                rc = fsfilt_map_nblocks(inode, fp->index * blocks_per_page,
                                        clen * blocks_per_page, blocks,
                                        created, create);
                if (rc)
                        GOTO(cleanup, rc);

                /* look for next extent */
                fp = NULL;
                blocks += blocks_per_page * clen;
                created += blocks_per_page * clen;
        }

        if (fp)
                rc = fsfilt_map_nblocks(inode, fp->index * blocks_per_page,
                                        clen * blocks_per_page, blocks,
                                        created, create);
cleanup:
        return rc;
}

extern int ext3_map_inode_page(struct inode *inode, struct page *page,
                               unsigned long *blocks, int *created, int create);
int fsfilt_ext3_map_bm_inode_pages(struct inode *inode, struct page **page,
                                   int pages, unsigned long *blocks,
                                   int *created, int create)
{
        int blocks_per_page = CFS_PAGE_SIZE >> inode->i_blkbits;
        unsigned long *b;
        int rc = 0, i;

        for (i = 0, b = blocks; i < pages; i++, page++) {
                rc = ext3_map_inode_page(inode, *page, b, NULL, create);
                if (rc) {
                        CERROR("ino %lu, blk %lu create %d: rc %d\n",
                               inode->i_ino, *b, create, rc);
                        break;
                }

                b += blocks_per_page;
        }
        return rc;
}

int fsfilt_ext3_map_inode_pages(struct inode *inode, struct page **page,
                                int pages, unsigned long *blocks,
                                int *created, int create,
                                cfs_semaphore_t *optional_sem)
{
        int rc;

        if (EXT3_I(inode)->i_flags & EXT3_EXTENTS_FL) {
                rc = fsfilt_ext3_map_ext_inode_pages(inode, page, pages,
                                                     blocks, created, create);
                return rc;
        }
        if (optional_sem != NULL)
                cfs_down(optional_sem);
        rc = fsfilt_ext3_map_bm_inode_pages(inode, page, pages, blocks,
                                            created, create);
        if (optional_sem != NULL)
                cfs_up(optional_sem);

        return rc;
}

static struct fsfilt_operations fsfilt_ext3_ops = {
        .fs_type                = "ext3",
        .fs_owner               = THIS_MODULE,
        .fs_map_inode_pages     = fsfilt_ext3_map_inode_pages,
};

static int __init fsfilt_ext3_init(void)
{
        int rc;

        fcb_cache = cfs_mem_cache_create("fsfilt_ext3_fcb",
                                         sizeof(struct fsfilt_cb_data), 0, 0);
        if (!fcb_cache) {
                CERROR("error allocating fsfilt journal callback cache\n");
                GOTO(out, rc = -ENOMEM);
        }

        rc = fsfilt_register_ops(&fsfilt_ext3_ops);

        if (rc) {
                int err = cfs_mem_cache_destroy(fcb_cache);
                LASSERTF(err == 0, "error destroying new cache: rc %d\n", err);
        }
out:
        return rc;
}

static void __exit fsfilt_ext3_exit(void)
{
        int rc;

        fsfilt_unregister_ops(&fsfilt_ext3_ops);
        rc = cfs_mem_cache_destroy(fcb_cache);
        LASSERTF(rc == 0, "couldn't destroy fcb_cache slab\n");
}

module_init(fsfilt_ext3_init);
module_exit(fsfilt_ext3_exit);

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre ext3 Filesystem Helper v0.1");
MODULE_LICENSE("GPL");
