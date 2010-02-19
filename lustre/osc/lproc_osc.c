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
 */
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/version.h>
#include <asm/statfs.h>
#include <obd_class.h>
#include <lprocfs_status.h>
#include <linux/seq_file.h>
#include "osc_internal.h"

#ifdef LPROCFS
static int osc_rd_active(char *page, char **start, off_t off,
                         int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        int rc;

        LPROCFS_CLIMP_CHECK(dev);
        rc = snprintf(page, count, "%d\n", !dev->u.cli.cl_import->imp_deactive);
        LPROCFS_CLIMP_EXIT(dev);
        return rc;
}

static int osc_wr_active(struct file *file, const char *buffer,
                         unsigned long count, void *data)
{
        struct obd_device *dev = data;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;
        if (val < 0 || val > 1)
                return -ERANGE;

        LPROCFS_CLIMP_CHECK(dev);
        /* opposite senses */
        if (dev->u.cli.cl_import->imp_deactive == val)
                rc = ptlrpc_set_import_active(dev->u.cli.cl_import, val);
        else
                CDEBUG(D_CONFIG, "activate %d: ignoring repeat request\n", val);

        LPROCFS_CLIMP_EXIT(dev);
        return count;
}


static int osc_rd_max_pages_per_rpc(char *page, char **start, off_t off,
                                    int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        client_obd_list_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%d\n", cli->cl_max_pages_per_rpc);
        client_obd_list_unlock(&cli->cl_loi_list_lock);
        return rc;
}

static int osc_wr_max_pages_per_rpc(struct file *file, const char *buffer,
                                    unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        struct obd_connect_data *ocd;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        LPROCFS_CLIMP_CHECK(dev);
        ocd = &cli->cl_import->imp_connect_data;
        if (val < 1 || val > ocd->ocd_brw_size >> CFS_PAGE_SHIFT) {
                LPROCFS_CLIMP_EXIT(dev);
                return -ERANGE;
        }
        client_obd_list_lock(&cli->cl_loi_list_lock);
        cli->cl_max_pages_per_rpc = val;
        client_obd_list_unlock(&cli->cl_loi_list_lock);
        LPROCFS_CLIMP_EXIT(dev);
        return count;
}

static int osc_rd_max_rpcs_in_flight(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        client_obd_list_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%u\n", cli->cl_max_rpcs_in_flight);
        client_obd_list_unlock(&cli->cl_loi_list_lock);
        return rc;
}

static int osc_wr_max_rpcs_in_flight(struct file *file, const char *buffer,
                                     unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        struct ptlrpc_request_pool *pool;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;
        if (val < 1 || val > OSC_MAX_RIF_MAX)
                return -ERANGE;

        LPROCFS_CLIMP_CHECK(dev);
        pool = cli->cl_import->imp_rq_pool;
        if (pool && val > cli->cl_max_rpcs_in_flight)
                pool->prp_populate(pool, val-cli->cl_max_rpcs_in_flight);

        client_obd_list_lock(&cli->cl_loi_list_lock);
        cli->cl_max_rpcs_in_flight = val;
        client_obd_list_unlock(&cli->cl_loi_list_lock);

        LPROCFS_CLIMP_EXIT(dev);
        return count;
}

static int osc_rd_max_dirty_mb(char *page, char **start, off_t off, int count,
                               int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        long val;
        int mult;

        client_obd_list_lock(&cli->cl_loi_list_lock);
        val = cli->cl_dirty_max;
        client_obd_list_unlock(&cli->cl_loi_list_lock);

        mult = 1 << 20;
        return lprocfs_read_frac_helper(page, count, val, mult);
}

static int osc_wr_max_dirty_mb(struct file *file, const char *buffer,
                               unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int pages_number, mult, rc;

        mult = 1 << (20 - CFS_PAGE_SHIFT);
        rc = lprocfs_write_frac_helper(buffer, count, &pages_number, mult);
        if (rc)
                return rc;

        if (pages_number < 0 || pages_number > OSC_MAX_DIRTY_MB_MAX << (20 - CFS_PAGE_SHIFT) ||
            pages_number > num_physpages / 4) /* 1/4 of RAM */
                return -ERANGE;

        client_obd_list_lock(&cli->cl_loi_list_lock);
        cli->cl_dirty_max = (obd_count)(pages_number << CFS_PAGE_SHIFT);
        osc_wake_cache_waiters(cli);
        client_obd_list_unlock(&cli->cl_loi_list_lock);

        return count;
}

static int osc_rd_cur_dirty_bytes(char *page, char **start, off_t off,
                                  int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        client_obd_list_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%lu\n", cli->cl_dirty);
        client_obd_list_unlock(&cli->cl_loi_list_lock);
        return rc;
}

static int osc_rd_cur_grant_bytes(char *page, char **start, off_t off,
                                  int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        client_obd_list_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%lu\n", cli->cl_avail_grant);
        client_obd_list_unlock(&cli->cl_loi_list_lock);
        return rc;
}

static int osc_wr_cur_grant_bytes(struct file *file, const char *buffer,
                                  unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct client_obd *cli = &obd->u.cli;
        int                rc;
        __u64              val;

        if (obd == NULL)
                return 0;

        rc = lprocfs_write_u64_helper(buffer, count, &val);
        if (rc)
                return rc;

        /* this is only for shrinking grant */
        client_obd_list_lock(&cli->cl_loi_list_lock);
        if (val >= cli->cl_avail_grant) {
                client_obd_list_unlock(&cli->cl_loi_list_lock);
                return 0;
        }
        client_obd_list_unlock(&cli->cl_loi_list_lock);

        LPROCFS_CLIMP_CHECK(obd);
        if (cli->cl_import->imp_state == LUSTRE_IMP_FULL)
                rc = osc_shrink_grant_to_target(cli, val);
        LPROCFS_CLIMP_EXIT(obd);
        if (rc)
                return rc;
        return count;
}

static int osc_rd_grant_shrink_interval(char *page, char **start, off_t off,
                                        int count, int *eof, void *data)
{
        struct obd_device *obd = data;

        if (obd == NULL)
                return 0;
        return snprintf(page, count, "%d\n",
                        obd->u.cli.cl_grant_shrink_interval);
}

static int osc_wr_grant_shrink_interval(struct file *file, const char *buffer,
                                        unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        if (obd == NULL)
                return 0;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val <= 0)
                return -ERANGE;

        obd->u.cli.cl_grant_shrink_interval = val;

        return count;
}

static int osc_rd_create_count(char *page, char **start, off_t off, int count,
                               int *eof, void *data)
{
        struct obd_device *obd = data;

        if (obd == NULL)
                return 0;

        return snprintf(page, count, "%d\n",
                        obd->u.cli.cl_oscc.oscc_grow_count);
}

static int osc_wr_create_count(struct file *file, const char *buffer,
                               unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc, i;

        if (obd == NULL)
                return 0;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        /* The MDT ALWAYS needs to limit the precreate count to
         * OST_MAX_PRECREATE, and the constant cannot be changed
         * because it is a value shared between the OSC and OST
         * that is the maximum possible number of objects that will
         * ever be handled by MDT->OST recovery processing.
         *
         * If the OST ever gets a request to delete more orphans,
         * this implies that something has gone badly on the MDT
         * and the OST will refuse to delete so much data from the
         * filesystem as a safety measure. */
        if (val < OST_MIN_PRECREATE || val > OST_MAX_PRECREATE)
                return -ERANGE;
        if (val > obd->u.cli.cl_oscc.oscc_max_grow_count)
                return -ERANGE;

        for (i = 1; (i << 1) <= val; i <<= 1)
                ;
        obd->u.cli.cl_oscc.oscc_grow_count = i;

        return count;
}

static int osc_rd_max_create_count(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;

        if (obd == NULL)
                return 0;

        return snprintf(page, count, "%d\n",
                        obd->u.cli.cl_oscc.oscc_max_grow_count);
}

static int osc_wr_max_create_count(struct file *file, const char *buffer,
                                   unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        if (obd == NULL)
                return 0;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 0)
                return -ERANGE;
        if (val > OST_MAX_PRECREATE)
                return -ERANGE;

        if (obd->u.cli.cl_oscc.oscc_grow_count > val)
                obd->u.cli.cl_oscc.oscc_grow_count = val;

        obd->u.cli.cl_oscc.oscc_max_grow_count = val;

        return count;
}

static int osc_rd_prealloc_next_id(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;

        if (obd == NULL)
                return 0;

        return snprintf(page, count, LPU64"\n",
                        obd->u.cli.cl_oscc.oscc_next_id);
}

static int osc_rd_prealloc_last_id(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;

        if (obd == NULL)
                return 0;

        return snprintf(page, count, LPU64"\n",
                        obd->u.cli.cl_oscc.oscc_last_id);
}

static int osc_rd_checksum(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_device *obd = data;

        if (obd == NULL)
                return 0;

        return snprintf(page, count, "%d\n",
                        obd->u.cli.cl_checksum ? 1 : 0);
}

static int osc_wr_checksum(struct file *file, const char *buffer,
                           unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        if (obd == NULL)
                return 0;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        obd->u.cli.cl_checksum = (val ? 1 : 0);

        return count;
}

static int osc_rd_checksum_type(char *page, char **start, off_t off, int count,
                                int *eof, void *data)
{
        struct obd_device *obd = data;
        int i, len =0;
        DECLARE_CKSUM_NAME;

        if (obd == NULL)
                return 0;

        for (i = 0; i < ARRAY_SIZE(cksum_name) && len < count; i++) {
                if (((1 << i) & obd->u.cli.cl_supp_cksum_types) == 0)
                        continue;
                if (obd->u.cli.cl_cksum_type == (1 << i))
                        len += snprintf(page + len, count - len, "[%s] ",
                                        cksum_name[i]);
                else
                        len += snprintf(page + len, count - len, "%s ",
                                        cksum_name[i]);
        }
        if (len < count)
                len += sprintf(page + len, "\n");
        return len;
}

static int osc_wd_checksum_type(struct file *file, const char *buffer,
                                unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int i;
        DECLARE_CKSUM_NAME;
        char kernbuf[10];

        if (obd == NULL)
                return 0;

        if (count > sizeof(kernbuf) - 1)
                return -EINVAL;
        if (copy_from_user(kernbuf, buffer, count))
                return -EFAULT;
        if (count > 0 && kernbuf[count - 1] == '\n')
                kernbuf[count - 1] = '\0';
        else
                kernbuf[count] = '\0';

        for (i = 0; i < ARRAY_SIZE(cksum_name); i++) {
                if (((1 << i) & obd->u.cli.cl_supp_cksum_types) == 0)
                        continue;
                if (!strcmp(kernbuf, cksum_name[i])) {
                       obd->u.cli.cl_cksum_type = 1 << i;
                       return count;
                }
        }
        return -EINVAL;
}

static int osc_rd_resend_count(char *page, char **start, off_t off, int count,
                               int *eof, void *data)
{
        struct obd_device *obd = data;

        return snprintf(page, count, "%u\n", atomic_read(&obd->u.cli.cl_resends));
}

static int osc_wr_resend_count(struct file *file, const char *buffer,
                               unsigned long count, void *data)
{
        struct obd_device *obd = data;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 0)
               return -EINVAL;

        atomic_set(&obd->u.cli.cl_resends, val);

        return count;
}

static struct lprocfs_vars lprocfs_osc_obd_vars[] = {
        { "uuid",            lprocfs_rd_uuid,        0, 0 },
        { "ping",            0, lprocfs_wr_ping,     0, 0, 0222 },
        { "connect_flags",   lprocfs_rd_connect_flags, 0, 0 },
        { "blocksize",       lprocfs_rd_blksize,     0, 0 },
        { "kbytestotal",     lprocfs_rd_kbytestotal, 0, 0 },
        { "kbytesfree",      lprocfs_rd_kbytesfree,  0, 0 },
        { "kbytesavail",     lprocfs_rd_kbytesavail, 0, 0 },
        { "filestotal",      lprocfs_rd_filestotal,  0, 0 },
        { "filesfree",       lprocfs_rd_filesfree,   0, 0 },
        //{ "filegroups",      lprocfs_rd_filegroups,  0, 0 },
        { "ost_server_uuid", lprocfs_rd_server_uuid, 0, 0 },
        { "ost_conn_uuid",   lprocfs_rd_conn_uuid, 0, 0 },
        { "active",          osc_rd_active,
                             osc_wr_active, 0 },
        { "max_pages_per_rpc", osc_rd_max_pages_per_rpc,
                               osc_wr_max_pages_per_rpc, 0 },
        { "max_rpcs_in_flight", osc_rd_max_rpcs_in_flight,
                                osc_wr_max_rpcs_in_flight, 0 },
        { "max_dirty_mb",    osc_rd_max_dirty_mb, osc_wr_max_dirty_mb, 0 },
        { "cur_dirty_bytes", osc_rd_cur_dirty_bytes, 0, 0 },
        { "cur_grant_bytes", osc_rd_cur_grant_bytes,
                             osc_wr_cur_grant_bytes, 0 },
        { "grant_shrink_interval", osc_rd_grant_shrink_interval,
                                   osc_wr_grant_shrink_interval, 0 },
        { "create_count",    osc_rd_create_count, osc_wr_create_count, 0 },
        { "max_create_count", osc_rd_max_create_count,
                              osc_wr_max_create_count, 0},
        { "prealloc_next_id", osc_rd_prealloc_next_id, 0, 0 },
        { "prealloc_last_id", osc_rd_prealloc_last_id, 0, 0 },
        { "checksums",       osc_rd_checksum, osc_wr_checksum, 0 },
        { "checksum_type",   osc_rd_checksum_type, osc_wd_checksum_type, 0 },
        { "resend_count",  osc_rd_resend_count, osc_wr_resend_count, 0},
        { "timeouts",        lprocfs_rd_timeouts,      0, 0 },
        { "import",          lprocfs_rd_import,    0, 0 },
        { "state",           lprocfs_rd_state,         0, 0 },
        { 0 }
};

static struct lprocfs_vars lprocfs_osc_module_vars[] = {
        { "num_refs",        lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

#define pct(a,b) (b ? a * 100 / b : 0)

static int osc_rpc_stats_seq_show(struct seq_file *seq, void *v)
{
        struct timeval now;
        struct obd_device *dev = seq->private;
        struct client_obd *cli = &dev->u.cli;
        unsigned long read_tot = 0, write_tot = 0, read_cum, write_cum;
        int i;

        do_gettimeofday(&now);

        client_obd_list_lock(&cli->cl_loi_list_lock);

        seq_printf(seq, "snapshot_time:         %lu.%lu (secs.usecs)\n",
                   now.tv_sec, now.tv_usec);
        seq_printf(seq, "read RPCs in flight:  %d\n",
                   cli->cl_r_in_flight);
        seq_printf(seq, "write RPCs in flight: %d\n",
                   cli->cl_w_in_flight);
        seq_printf(seq, "pending write pages:  %d\n",
                   cli->cl_pending_w_pages);
        seq_printf(seq, "pending read pages:   %d\n",
                   cli->cl_pending_r_pages);

        seq_printf(seq, "\n\t\t\tread\t\t\twrite\n");
        seq_printf(seq, "pages per rpc         rpcs   %% cum %% |");
        seq_printf(seq, "       rpcs   %% cum %%\n");

        read_tot = lprocfs_oh_sum(&cli->cl_read_page_hist);
        write_tot = lprocfs_oh_sum(&cli->cl_write_page_hist);

        read_cum = 0;
        write_cum = 0;
        for (i = 0; i < OBD_HIST_MAX; i++) {
                unsigned long r = cli->cl_read_page_hist.oh_buckets[i];
                unsigned long w = cli->cl_write_page_hist.oh_buckets[i];
                read_cum += r;
                write_cum += w;
                seq_printf(seq, "%d:\t\t%10lu %3lu %3lu   | %10lu %3lu %3lu\n",
                                 1 << i, r, pct(r, read_tot),
                                 pct(read_cum, read_tot), w,
                                 pct(w, write_tot),
                                 pct(write_cum, write_tot));
                if (read_cum == read_tot && write_cum == write_tot)
                        break;
        }

        seq_printf(seq, "\n\t\t\tread\t\t\twrite\n");
        seq_printf(seq, "rpcs in flight        rpcs   %% cum %% |");
        seq_printf(seq, "       rpcs   %% cum %%\n");

        read_tot = lprocfs_oh_sum(&cli->cl_read_rpc_hist);
        write_tot = lprocfs_oh_sum(&cli->cl_write_rpc_hist);

        read_cum = 0;
        write_cum = 0;
        for (i = 0; i < OBD_HIST_MAX; i++) {
                unsigned long r = cli->cl_read_rpc_hist.oh_buckets[i];
                unsigned long w = cli->cl_write_rpc_hist.oh_buckets[i];
                read_cum += r;
                write_cum += w;
                seq_printf(seq, "%d:\t\t%10lu %3lu %3lu   | %10lu %3lu %3lu\n",
                                 i, r, pct(r, read_tot),
                                 pct(read_cum, read_tot), w,
                                 pct(w, write_tot),
                                 pct(write_cum, write_tot));
                if (read_cum == read_tot && write_cum == write_tot)
                        break;
        }

        seq_printf(seq, "\n\t\t\tread\t\t\twrite\n");
        seq_printf(seq, "offset                rpcs   %% cum %% |");
        seq_printf(seq, "       rpcs   %% cum %%\n");

        read_tot = lprocfs_oh_sum(&cli->cl_read_offset_hist);
        write_tot = lprocfs_oh_sum(&cli->cl_write_offset_hist);

        read_cum = 0;
        write_cum = 0;
        for (i = 0; i < OBD_HIST_MAX; i++) {
                unsigned long r = cli->cl_read_offset_hist.oh_buckets[i];
                unsigned long w = cli->cl_write_offset_hist.oh_buckets[i];
                read_cum += r;
                write_cum += w;
                seq_printf(seq, "%d:\t\t%10lu %3lu %3lu   | %10lu %3lu %3lu\n",
                           (i == 0) ? 0 : 1 << (i - 1),
                           r, pct(r, read_tot), pct(read_cum, read_tot),
                           w, pct(w, write_tot), pct(write_cum, write_tot));
                if (read_cum == read_tot && write_cum == write_tot)
                        break;
        }

        client_obd_list_unlock(&cli->cl_loi_list_lock);

        return 0;
}
#undef pct

static ssize_t osc_rpc_stats_seq_write(struct file *file, const char *buf,
                                       size_t len, loff_t *off)
{
        struct seq_file *seq = file->private_data;
        struct obd_device *dev = seq->private;
        struct client_obd *cli = &dev->u.cli;

        lprocfs_oh_clear(&cli->cl_read_rpc_hist);
        lprocfs_oh_clear(&cli->cl_write_rpc_hist);
        lprocfs_oh_clear(&cli->cl_read_page_hist);
        lprocfs_oh_clear(&cli->cl_write_page_hist);
        lprocfs_oh_clear(&cli->cl_read_offset_hist);
        lprocfs_oh_clear(&cli->cl_write_offset_hist);

        return len;
}

LPROC_SEQ_FOPS(osc_rpc_stats);

int lproc_osc_attach_seqstat(struct obd_device *dev)
{
        return lprocfs_obd_seq_create(dev, "rpc_stats", 0444,
                                      &osc_rpc_stats_fops, dev);
}

void lprocfs_osc_init_vars(struct lprocfs_static_vars *lvars)
{
        lvars->module_vars = lprocfs_osc_module_vars;
        lvars->obd_vars    = lprocfs_osc_obd_vars;
}
#endif /* LPROCFS */
