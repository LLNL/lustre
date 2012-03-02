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
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/osd/osd_lproc.c
 *
 * Author: Mikhail Pershin <tappro@sun.com>
 * Author: Alex Zhuravlev <bzzz@sun.com>
 */

#define DEBUG_SUBSYSTEM S_CLASS

#include <obd.h>
#include <lprocfs_status.h>
#include <lu_time.h>

#include <lustre/lustre_idl.h>

#include "osd_internal.h"

#ifdef LPROCFS

void osd_brw_stats_update(struct osd_device *osd, struct osd_iobuf *iobuf)
{
        struct brw_stats *s = &osd->od_brw_stats;
        unsigned long    *last_block = NULL;
        struct page     **pages = iobuf->dr_pages;
        struct page      *last_page = NULL;
        unsigned long     discont_pages = 0;
        unsigned long     discont_blocks = 0;
        unsigned long    *blocks = iobuf->dr_blocks;
        int               i, nr_pages = iobuf->dr_npages;
        int               blocks_per_page;
        int               rw = iobuf->dr_rw;

        if (unlikely(nr_pages == 0))
                return;

        blocks_per_page = CFS_PAGE_SIZE >> osd_sb(osd)->s_blocksize_bits;

        lprocfs_oh_tally_log2(&s->hist[BRW_R_PAGES+rw], nr_pages);

        while (nr_pages-- > 0) {
                if (last_page && (*pages)->index != (last_page->index + 1))
                        discont_pages++;
                last_page = *pages;
                pages++;
                for (i = 0; i < blocks_per_page; i++) {
                        if (last_block && *blocks != (*last_block + 1))
                                discont_blocks++;
                        last_block = blocks++;
                }
        }

        lprocfs_oh_tally(&s->hist[BRW_R_DISCONT_PAGES+rw], discont_pages);
        lprocfs_oh_tally(&s->hist[BRW_R_DISCONT_BLOCKS+rw], discont_blocks);
}

#define pct(a,b) (b ? a * 100 / b : 0)

static void display_brw_stats(struct seq_file *seq, char *name, char *units,
        struct obd_histogram *read, struct obd_histogram *write, int scale)
{
        unsigned long read_tot, write_tot, r, w, read_cum = 0, write_cum = 0;
        int i;

        seq_printf(seq, "\n%26s read      |     write\n", " ");
        seq_printf(seq, "%-22s %-5s %% cum %% |  %-11s %% cum %%\n",
                   name, units, units);

        read_tot = lprocfs_oh_sum(read);
        write_tot = lprocfs_oh_sum(write);
        for (i = 0; i < OBD_HIST_MAX; i++) {
                r = read->oh_buckets[i];
                w = write->oh_buckets[i];
                read_cum += r;
                write_cum += w;
                if (read_cum == 0 && write_cum == 0)
                        continue;

                if (!scale)
                        seq_printf(seq, "%u", i);
                else if (i < 10)
                        seq_printf(seq, "%u", scale << i);
                else if (i < 20)
                        seq_printf(seq, "%uK", scale << (i-10));
                else
                        seq_printf(seq, "%uM", scale << (i-20));

                seq_printf(seq, ":\t\t%10lu %3lu %3lu   | %4lu %3lu %3lu\n",
                           r, pct(r, read_tot), pct(read_cum, read_tot),
                           w, pct(w, write_tot), pct(write_cum, write_tot));

                if (read_cum == read_tot && write_cum == write_tot)
                        break;
        }
}

static void brw_stats_show(struct seq_file *seq, struct brw_stats *brw_stats)
{
        struct timeval now;

        /* this sampling races with updates */
        cfs_gettimeofday(&now);
        seq_printf(seq, "snapshot_time:         %lu.%lu (secs.usecs)\n",
                   now.tv_sec, now.tv_usec);

        display_brw_stats(seq, "pages per bulk r/w", "rpcs",
                          &brw_stats->hist[BRW_R_PAGES],
                          &brw_stats->hist[BRW_W_PAGES], 1);

        display_brw_stats(seq, "discontiguous pages", "rpcs",
                          &brw_stats->hist[BRW_R_DISCONT_PAGES],
                          &brw_stats->hist[BRW_W_DISCONT_PAGES], 0);

        display_brw_stats(seq, "discontiguous blocks", "rpcs",
                          &brw_stats->hist[BRW_R_DISCONT_BLOCKS],
                          &brw_stats->hist[BRW_W_DISCONT_BLOCKS], 0);

        display_brw_stats(seq, "disk fragmented I/Os", "ios",
                          &brw_stats->hist[BRW_R_DIO_FRAGS],
                          &brw_stats->hist[BRW_W_DIO_FRAGS], 0);

        display_brw_stats(seq, "disk I/Os in flight", "ios",
                          &brw_stats->hist[BRW_R_RPC_HIST],
                          &brw_stats->hist[BRW_W_RPC_HIST], 0);

        display_brw_stats(seq, "I/O time (1/1000s)", "ios",
                          &brw_stats->hist[BRW_R_IO_TIME],
                          &brw_stats->hist[BRW_W_IO_TIME], 1000/CFS_HZ);

        display_brw_stats(seq, "disk I/O size", "ios",
                          &brw_stats->hist[BRW_R_DISK_IOSIZE],
                          &brw_stats->hist[BRW_W_DISK_IOSIZE], 1);
}

#undef pct

static int osd_brw_stats_seq_show(struct seq_file *seq, void *v)
{
        struct osd_device *osd = seq->private;

        brw_stats_show(seq, &osd->od_brw_stats);

        return 0;
}

static ssize_t osd_brw_stats_seq_write(struct file *file, const char *buf,
                                       size_t len, loff_t *off)
{
        struct seq_file *seq = file->private_data;
        struct osd_device *osd = seq->private;
        int i;

        for (i = 0; i < BRW_LAST; i++)
                lprocfs_oh_clear(&osd->od_brw_stats.hist[i]);

        return len;
}

LPROC_SEQ_FOPS(osd_brw_stats);

static int osd_stats_init(struct osd_device *osd)
{
        int i, result;
        ENTRY;

        for (i = 0; i < BRW_LAST; i++)
                cfs_spin_lock_init(&osd->od_brw_stats.hist[i].oh_lock);

        osd->od_stats = lprocfs_alloc_stats(LPROC_OSD_LAST, 0);
        if (osd->od_stats != NULL) {
                result = lprocfs_register_stats(osd->od_proc_entry, "stats",
                                                osd->od_stats);
                if (result)
                        GOTO(out, result);

                lprocfs_counter_init(osd->od_stats, LPROC_OSD_GET_PAGE,
                                     LPROCFS_CNTR_AVGMINMAX|LPROCFS_CNTR_STDDEV,
                                     "get_page", "usec");
                lprocfs_counter_init(osd->od_stats, LPROC_OSD_NO_PAGE,
                                     LPROCFS_CNTR_AVGMINMAX,
                                     "get_page_failures", "num");
                lprocfs_counter_init(osd->od_stats, LPROC_OSD_CACHE_ACCESS,
                                     LPROCFS_CNTR_AVGMINMAX,
                                     "cache_access", "pages");
                lprocfs_counter_init(osd->od_stats, LPROC_OSD_CACHE_HIT,
                                     LPROCFS_CNTR_AVGMINMAX,
                                     "cache_hit", "pages");
                lprocfs_counter_init(osd->od_stats, LPROC_OSD_CACHE_MISS,
                                     LPROCFS_CNTR_AVGMINMAX,
                                     "cache_miss", "pages");
#ifdef OSD_THANDLE_STATS
                lprocfs_counter_init(osd->od_stats, LPROC_OSD_THANDLE_STARTING,
                                     LPROCFS_CNTR_AVGMINMAX,
                                     "thandle starting", "usec");
                lprocfs_counter_init(osd->od_stats, LPROC_OSD_THANDLE_OPEN,
                                     LPROCFS_CNTR_AVGMINMAX,
                                     "thandle open", "usec");
                lprocfs_counter_init(osd->od_stats, LPROC_OSD_THANDLE_CLOSING,
                                     LPROCFS_CNTR_AVGMINMAX,
                                     "thandle closing", "usec");
#endif
                lprocfs_seq_create(osd->od_proc_entry, "brw_stats",
                                   0444, &osd_brw_stats_fops, osd);
        } else
                result = -ENOMEM;

out:
        RETURN(result);
}

int osd_procfs_init(struct osd_device *osd, const char *name)
{
        struct lprocfs_static_vars lvars;
        struct lu_device    *ld = &osd->od_dt_dev.dd_lu_dev;
        struct obd_type     *type;
        int                  rc;
        ENTRY;

        type = ld->ld_type->ldt_obd_type;
        LASSERT(type);

        if (osd->od_proc_entry)
                RETURN(0);

        LASSERT(name != NULL);
        LASSERT(type != NULL);

        /* Find the type procroot and add the proc entry for this device */
        lprocfs_osd_init_vars(&lvars);
        osd->od_proc_entry = lprocfs_register(name, type->typ_procroot,
                                              lvars.obd_vars, osd);
        if (IS_ERR(osd->od_proc_entry)) {
                rc = PTR_ERR(osd->od_proc_entry);
                CERROR("Error %d setting up lprocfs for %s\n", rc, name);
                osd->od_proc_entry = NULL;
                GOTO(out, rc);
        }

        //osd_quota_procfs_init(osd);

        rc = osd_stats_init(osd);

        EXIT;
out:
        if (rc)
               osd_procfs_fini(osd);
        return rc;
}

int osd_procfs_fini(struct osd_device *osd)
{
        ENTRY;

        if (osd->od_stats)
                lprocfs_free_stats(&osd->od_stats);

        if (osd->od_proc_entry) {
                lprocfs_remove(&osd->od_proc_entry);
                osd->od_proc_entry = NULL;
        }

        RETURN(0);
}

void osd_lprocfs_time_start(const struct lu_env *env)
{
        lu_lprocfs_time_start(env);
}

void osd_lprocfs_time_end(const struct lu_env *env, struct osd_device *osd,
                          int idx)
{
        lu_lprocfs_time_end(env, osd->od_stats, idx);
}

int lprocfs_osd_rd_blksize(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct osd_device *osd = data;
        struct obd_statfs osfs;
        int rc;

        if (unlikely(osd->od_mnt == NULL))
                return -EINPROGRESS;

        rc = osd_statfs(NULL, &osd->od_dt_dev, &osfs);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, "%d\n",
                              (unsigned) osfs.os_bsize);
        }
        return rc;
}

int lprocfs_osd_rd_kbytestotal(char *page, char **start, off_t off, int count,
                               int *eof, void *data)
{
        struct osd_device *osd = data;
        struct obd_statfs osfs;
        int rc;

        if (unlikely(osd->od_mnt == NULL))
                return -EINPROGRESS;

        rc = osd_statfs(NULL, &osd->od_dt_dev, &osfs);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_blocks;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_osd_rd_kbytesfree(char *page, char **start, off_t off, int count,
                              int *eof, void *data)
{
        struct osd_device *osd = data;
        struct obd_statfs osfs;
        int rc;

        if (unlikely(osd->od_mnt == NULL))
                return -EINPROGRESS;

        rc = osd_statfs(NULL, &osd->od_dt_dev, &osfs);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_bfree;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_osd_rd_kbytesavail(char *page, char **start, off_t off, int count,
                               int *eof, void *data)
{
        struct osd_device *osd = data;
        struct obd_statfs osfs;
        int rc;

        if (unlikely(osd->od_mnt == NULL))
                return -EINPROGRESS;

        rc = osd_statfs(NULL, &osd->od_dt_dev, &osfs);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_bavail;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_osd_rd_filestotal(char *page, char **start, off_t off, int count,
                              int *eof, void *data)
{
        struct osd_device *osd = data;
        struct obd_statfs osfs;
        int rc;

        if (unlikely(osd->od_mnt == NULL))
                return -EINPROGRESS;

        rc = osd_statfs(NULL, &osd->od_dt_dev, &osfs);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", osfs.os_files);
        }

        return rc;
}

int lprocfs_osd_rd_filesfree(char *page, char **start, off_t off, int count,
                             int *eof, void *data)
{
        struct osd_device *osd = data;
        struct obd_statfs osfs;
        int rc;

        if (unlikely(osd->od_mnt == NULL))
                return -EINPROGRESS;

        rc = osd_statfs(NULL, &osd->od_dt_dev, &osfs);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", osfs.os_ffree);
        }
        return rc;
}

int lprocfs_osd_rd_fstype(char *page, char **start, off_t off, int count,
                          int *eof, void *data)
{
        struct obd_device *osd = data;

        LASSERT(osd != NULL);
        return snprintf(page, count, "ldiskfs\n");
}

static int lprocfs_osd_rd_mntdev(char *page, char **start, off_t off, int count,
                                 int *eof, void *data)
{
        struct osd_device *osd = data;

        LASSERT(osd != NULL);
        if (unlikely(osd->od_mnt == NULL))
                return -EINPROGRESS;

        *eof = 1;

        return snprintf(page, count, "%s\n", osd->od_mntdev);
}

static int lprocfs_osd_rd_cache(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct osd_device *osd = data;
        LASSERT(osd != NULL);

        return snprintf(page, count, "%u\n", osd->od_read_cache);
}

static int lprocfs_osd_wr_cache(struct file *file, const char *buffer,
                     unsigned long count, void *data)
{
        struct osd_device *osd = data;
        int val, rc;
        LASSERT(osd != NULL);

        rc = lprocfs_write_helper(buffer, count, &val);

        if (rc)
                return rc;

        osd->od_read_cache = !!val;
        return count;
}


static int lprocfs_osd_rd_wcache(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct osd_device *osd = data;
        LASSERT(osd != NULL);

        return snprintf(page, count, "%u\n", osd->od_writethrough_cache);
}

static int lprocfs_osd_wr_wcache(struct file *file, const char *buffer,
                     unsigned long count, void *data)
{
        struct osd_device *osd = data;
        int val, rc;
        LASSERT(osd != NULL);

        rc = lprocfs_write_helper(buffer, count, &val);

        if (rc)
                return rc;

        osd->od_writethrough_cache = !!val;
        return count;
}


static int lprocfs_osd_wr_force_sync(struct file *file, const char *buffer,
                                     unsigned long count, void *data)
{
        struct osd_device *osd = data;
        struct dt_device  *dt;
        struct lu_env      env;
        int rc;

        LASSERT(osd != NULL);
        dt = &osd->od_dt_dev;

        rc = lu_env_init(&env, dt->dd_lu_dev.ld_type->ldt_ctx_tags);
        if (rc)
                return rc;
        rc = dt_sync(&env, dt);
        lu_env_fini(&env);

        return rc == 0 ? count : rc;
}

#ifdef HAVE_LDISKFS_PDO
static int lprocfs_osd_rd_pdo(char *page, char **start, off_t off, int count,
                              int *eof, void *data)
{
        *eof = 1;

        return snprintf(page, count, "%s\n", ldiskfs_pdo ? "ON" : "OFF");
}

static int lprocfs_osd_wr_pdo(struct file *file, const char *buffer,
                              unsigned long count, void *data)
{
        int     pdo;
        int     rc;

        rc = lprocfs_write_helper(buffer, count, &pdo);
        if (rc != 0)
                return rc;

        ldiskfs_pdo = !!pdo;

        return count;
}
#endif

static int print_brw_stats_raw(char *page, int count, int scale,
                               struct obd_histogram *read,
                               struct obd_histogram *write)
{
        unsigned long read_tot, write_tot, r, w, read_cum = 0, write_cum = 0;
        struct timeval now;
        int i, out, offset = 0;

        cfs_gettimeofday(&now);
        read_tot = lprocfs_oh_sum(read);
        write_tot = lprocfs_oh_sum(write);

        offset = snprintf(page + offset, count,
                       "snapshot_time: %lu.%lu\ntotal: %lu %lu\n",
                       now.tv_sec, now.tv_usec, read_tot, write_tot);
        count -= offset;
        if (count < 0)
                count = 0;

        for (i = 0; i < OBD_HIST_MAX; i++) {
                r = read->oh_buckets[i];
                w = write->oh_buckets[i];
                read_cum += r;
                write_cum += w;
                if (r == 0 && w == 0)
                        continue;

                out = snprintf(page + offset, count, "%u: %lu %lu\n",
                                scale ? scale << i : i, r, w);
                offset += out;
                count -= out;
                if (count < 0)
                        count = 0;

                if (read_cum == read_tot && write_cum == write_tot)
                        break;
        }

        return offset;
}

static int lprocfs_osd_rd_brw_stats_pages_per_bulk(char *page, char **start,
                                                   off_t off, int count,
                                                   int *eof, void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);

        return print_brw_stats_raw(page, count, 1,
                                   &d->od_brw_stats.hist[BRW_R_PAGES],
                                   &d->od_brw_stats.hist[BRW_W_PAGES]);
}

static int lprocfs_osd_wr_brw_stats_pages_per_bulk(struct file *file,
                                                   const char *buffer,
                                                   unsigned long count,
                                                   void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_R_PAGES]);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_W_PAGES]);
        return count;
}

static int lprocfs_osd_rd_brw_stats_discont_pages(char *page, char **start,
                                                  off_t off, int count,
                                                  int *eof, void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);

        return print_brw_stats_raw(page, count, 0,
                                   &d->od_brw_stats.hist[BRW_R_DISCONT_PAGES],
                                   &d->od_brw_stats.hist[BRW_W_DISCONT_PAGES]);
}

static int lprocfs_osd_wr_brw_stats_discont_pages(struct file *file,
                                                  const char *buffer,
                                                  unsigned long count,
                                                  void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_R_DISCONT_PAGES]);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_W_DISCONT_PAGES]);
        return count;
}

static int lprocfs_osd_rd_brw_stats_discont_blocks(char *page, char **start,
                                                   off_t off, int count,
                                                   int *eof, void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);

        return print_brw_stats_raw(page, count, 0,
                                   &d->od_brw_stats.hist[BRW_R_DISCONT_BLOCKS],
                                   &d->od_brw_stats.hist[BRW_W_DISCONT_BLOCKS]);
}

static int lprocfs_osd_wr_brw_stats_discont_blocks(struct file *file,
                                                   const char *buffer,
                                                   unsigned long count,
                                                   void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_R_DISCONT_BLOCKS]);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_W_DISCONT_BLOCKS]);
        return count;
}

static int lprocfs_osd_rd_brw_stats_fragmented_ios(char *page, char **start,
                                                   off_t off, int count,
                                                   int *eof, void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);

        return print_brw_stats_raw(page, count, 0,
                                   &d->od_brw_stats.hist[BRW_R_DIO_FRAGS],
                                   &d->od_brw_stats.hist[BRW_W_DIO_FRAGS]);
}

static int lprocfs_osd_wr_brw_stats_fragmented_ios(struct file *file,
                                                   const char *buffer,
                                                   unsigned long count,
                                                   void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_R_DIO_FRAGS]);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_W_DIO_FRAGS]);
        return count;
}

static int lprocfs_osd_rd_brw_stats_ios_in_flight(char *page, char **start,
                                                  off_t off, int count,
                                                  int *eof, void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);

        return print_brw_stats_raw(page, count, 0,
                                   &d->od_brw_stats.hist[BRW_R_RPC_HIST],
                                   &d->od_brw_stats.hist[BRW_W_RPC_HIST]);
}

static int lprocfs_osd_wr_brw_stats_ios_in_flight(struct file *file,
                                                  const char *buffer,
                                                  unsigned long count,
                                                  void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_R_RPC_HIST]);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_W_RPC_HIST]);
        return count;
}

static int lprocfs_osd_rd_brw_stats_io_time(char *page, char **start, off_t off,
                                            int count, int *eof, void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);

        return print_brw_stats_raw(page, count, 1000 / CFS_HZ,
                                   &d->od_brw_stats.hist[BRW_R_IO_TIME],
                                   &d->od_brw_stats.hist[BRW_W_IO_TIME]);
}

static int lprocfs_osd_wr_brw_stats_io_time(struct file *file,
                                            const char *buffer,
                                            unsigned long count, void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_R_IO_TIME]);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_W_IO_TIME]);
        return count;
}

static int lprocfs_osd_rd_brw_stats_io_size(char *page, char **start,
                                            off_t off, int count, int *eof,
                                            void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);

        return print_brw_stats_raw(page, count, 1,
                                   &d->od_brw_stats.hist[BRW_R_DISK_IOSIZE],
                                   &d->od_brw_stats.hist[BRW_W_DISK_IOSIZE]);
}

static int lprocfs_osd_wr_brw_stats_io_size(struct file *file,
                                            const char *buffer,
                                            unsigned long count, void *data)
{
        struct osd_device *d = data;
        LASSERT(d != NULL);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_R_DISK_IOSIZE]);
        lprocfs_oh_clear(&d->od_brw_stats.hist[BRW_W_DISK_IOSIZE]);
        return count;
}

struct lprocfs_vars lprocfs_osd_obd_vars[] = {
        { "blocksize",       lprocfs_osd_rd_blksize,     0, 0 },
        { "kbytestotal",     lprocfs_osd_rd_kbytestotal, 0, 0 },
        { "kbytesfree",      lprocfs_osd_rd_kbytesfree,  0, 0 },
        { "kbytesavail",     lprocfs_osd_rd_kbytesavail, 0, 0 },
        { "filestotal",      lprocfs_osd_rd_filestotal,  0, 0 },
        { "filesfree",       lprocfs_osd_rd_filesfree,   0, 0 },
        { "fstype",          lprocfs_osd_rd_fstype,      0, 0 },
        { "mntdev",          lprocfs_osd_rd_mntdev,      0, 0 },
#ifdef HAVE_LDISKFS_PDO
        { "pdo",             lprocfs_osd_rd_pdo, lprocfs_osd_wr_pdo, 0 },
#endif
        { "force_sync",      0, lprocfs_osd_wr_force_sync     },
        { "read_cache_enable",lprocfs_osd_rd_cache,
                              lprocfs_osd_wr_cache,          0 },
        { "writethrough_cache_enable",lprocfs_osd_rd_wcache,
                               lprocfs_osd_wr_wcache,         0 },
        { "brw_pages_per_bulk",lprocfs_osd_rd_brw_stats_pages_per_bulk,
                               lprocfs_osd_wr_brw_stats_pages_per_bulk, 0 },
        { "brw_discont_pages", lprocfs_osd_rd_brw_stats_discont_pages,
                               lprocfs_osd_wr_brw_stats_discont_pages, 0 },
        { "brw_discont_blocks",lprocfs_osd_rd_brw_stats_discont_blocks,
                               lprocfs_osd_wr_brw_stats_discont_blocks, 0 },
        { "brw_fragmented_ios",lprocfs_osd_rd_brw_stats_fragmented_ios,
                               lprocfs_osd_wr_brw_stats_fragmented_ios, 0 },
        { "brw_ios_in_flight", lprocfs_osd_rd_brw_stats_ios_in_flight,
                               lprocfs_osd_wr_brw_stats_ios_in_flight, 0 },
        { "brw_io_time",       lprocfs_osd_rd_brw_stats_io_time,
                               lprocfs_osd_wr_brw_stats_io_time, 0 },
        { "brw_io_size",       lprocfs_osd_rd_brw_stats_io_size,
                               lprocfs_osd_wr_brw_stats_io_size, 0 },
        { 0 }
};

struct lprocfs_vars lprocfs_osd_module_vars[] = {
        { "num_refs",        lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

void lprocfs_osd_init_vars(struct lprocfs_static_vars *lvars)
{
        lvars->module_vars = lprocfs_osd_module_vars;
        lvars->obd_vars = lprocfs_osd_obd_vars;
}
#endif
