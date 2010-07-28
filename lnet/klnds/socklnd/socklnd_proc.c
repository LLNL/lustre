/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2010 Lawrence Livermore National Security, LLC
 *   Author: Christopher J. Morrone <morrone2@llnl.gov>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "socklnd.h"

#if defined(__KERNEL__) && defined(LNET_ROUTER)

#include <linux/seq_file.h>

#define SOCKLND_PROC_ROOT    "sys/lnet/socklnd"
#define SOCKLND_PROC_STATS   "sys/lnet/socklnd/stats"

/* FIXME: I should really make a shared lnet version of these and combine the
   o2iblnd and socklnd histogram patches. */

/* copy of lprocfs_oh_tally */
static void
ksocknal_tally(struct ksocknal_histogram *kh, unsigned int value)
{
        if (value >= KSOCKNAL_HIST_MAX)
                value = KSOCKNAL_HIST_MAX - 1;

        spin_lock(&kh->kh_lock);
        kh->kh_buckets[value]++;
        spin_unlock(&kh->kh_lock);
}

/* copy of lprocfs_oh_tally_log2() */
void
ksocknal_tally_log2(struct ksocknal_histogram *kh, unsigned int value)
{
        unsigned int val;

        for (val = 0; ((1 << val) < value) && (val <= KSOCKNAL_HIST_MAX); val++)
                ;

        ksocknal_tally(kh, val);
}

/* copy of lprocfs_oh_sum() */
static unsigned long
ksocknal_sum(struct ksocknal_histogram *kh)
{
        unsigned long ret = 0;
        int i;

        for (i = 0; i < KSOCKNAL_HIST_MAX; i++)
                ret +=  kh->kh_buckets[i];
        return ret;
}

/* copy of lprocfs_oh_clear() */
static void
ksocknal_clear(struct ksocknal_histogram *kh)
{
        spin_lock(&kh->kh_lock);
        memset(kh->kh_buckets, 0, sizeof(kh->kh_buckets));
        spin_unlock(&kh->kh_lock);
}


static void *
ksocknal_stats_seq_start (struct seq_file *s, loff_t *pos)
{
        if (*pos == 0)
                return (void *)1;
        return NULL;
}

static void
ksocknal_stats_seq_stop (struct seq_file *s, void *iter)
{
}

static void *
ksocknal_stats_seq_next (struct seq_file *s, void *v, loff_t *pos)
{
        ++*pos;
        return NULL;
}

#define pct(a,b) (b ? a * 100 / b : 0)

static int
ksocknal_stats_seq_show (struct seq_file *s, void *v)
{
        unsigned long t, t_tot, t_cum;
        int i;
        char scale[12];

        snprintf(scale, 12, "1/%ds", (int)cfs_time_seconds(1));

        seq_printf(s, "TX message queue time\n");
        seq_printf(s, "%-12s %10s %3s %3s\n", scale, "txs", "%", "cum %");
        t_cum = 0;
        t_tot = ksocknal_sum(&ksocknal_data.ksnd_hist[KSOCKNAL_HIST_TX]);
        for (i = 0; i < KSOCKNAL_HIST_MAX; i++) {
                t = ksocknal_data.ksnd_hist[KSOCKNAL_HIST_TX].kh_buckets[i];
                t_cum += t;
                seq_printf(s, "%-12u %10lu %3lu %3lu\n",
                           1 << i, t, pct(t, t_tot), pct(t_cum, t_tot));
                if (t_cum == t_tot)
                        break;
        }

        return 0;
}

static struct seq_operations ksocknal_stats_sops = {
        .start = ksocknal_stats_seq_start,
        .stop  = ksocknal_stats_seq_stop,
        .next  = ksocknal_stats_seq_next,
        .show  = ksocknal_stats_seq_show,
};

static int
ksocknal_stats_seq_open(struct inode *inode, struct file *file)
{
        return seq_open(file, &ksocknal_stats_sops);
}

static ssize_t
ksocknal_stats_seq_write(struct file *file, const char *buf,
                       size_t len, loff_t *off)
{
        int i;

        for (i = 0; i < KSOCKNAL_HIST_LAST; i++)
                ksocknal_clear(&ksocknal_data.ksnd_hist[i]);

        return len;
}

static struct file_operations ksocknal_stats_fops = {
        .owner   = THIS_MODULE,
        .open    = ksocknal_stats_seq_open,
        .read    = seq_read,
        .write   = ksocknal_stats_seq_write,
        .llseek  = seq_lseek,
        .release = seq_release,
};

int
ksocknal_proc_init(void)
{
        struct proc_dir_entry *entry;

        if (!proc_mkdir(SOCKLND_PROC_ROOT, NULL)) {
                CERROR("couldn't create proc dir %s\n", SOCKLND_PROC_ROOT);
                return -1;
        }

        entry = create_proc_entry(SOCKLND_PROC_STATS, 0644, NULL);
        if (entry == NULL) {
                CERROR("couldn't create proc entry %s\n", SOCKLND_PROC_STATS);
                return -1;
        }

        entry->proc_fops = &ksocknal_stats_fops;
        entry->data = NULL;

        return 0;
}

void
ksocknal_proc_fini(void)
{
        remove_proc_entry(SOCKLND_PROC_STATS, NULL);
        remove_proc_entry(SOCKLND_PROC_ROOT, NULL);
}

#else

int
ksocknal_proc_init(void)
{
        return 0;
}

void
ksocknal_proc_fini(void)
{
}

#endif
