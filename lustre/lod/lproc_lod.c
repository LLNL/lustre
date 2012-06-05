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

#include <lprocfs_status.h>
#include <obd_class.h>
#include <linux/seq_file.h>
#include "lod_internal.h"
#include <lustre_param.h>

#ifdef LPROCFS
static int lod_rd_stripesize(char *page, char **start, off_t off, int count,
                             int *eof, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_desc *desc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        *eof = 1;
        return snprintf(page, count, LPU64"\n", desc->ld_default_stripe_size);
}

static int lod_wr_stripesize(struct file *file, const char *buffer,
                               unsigned long count, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_desc *desc;
        __u64 val;
        int rc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        rc = lprocfs_write_u64_helper(buffer, count, &val);
        if (rc)
                return rc;

        lod_fix_desc_stripe_size(&val);
        desc->ld_default_stripe_size = val;
        return count;
}

static int lod_rd_stripeoffset(char *page, char **start, off_t off, int count,
                               int *eof, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_desc *desc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        *eof = 1;
        return snprintf(page, count, LPU64"\n", desc->ld_default_stripe_offset);
}

static int lod_wr_stripeoffset(struct file *file, const char *buffer,
                               unsigned long count, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_desc *desc;
        __u64 val;
        int rc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        rc = lprocfs_write_u64_helper(buffer, count, &val);
        if (rc)
                return rc;

        desc->ld_default_stripe_offset = val;
        return count;
}

static int lod_rd_stripetype(char *page, char **start, off_t off, int count,
                             int *eof, void *data)
{
        struct obd_device* dev = (struct obd_device*)data;
        struct lov_desc *desc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        *eof = 1;
        return snprintf(page, count, "%u\n", desc->ld_pattern);
}

static int lod_wr_stripetype(struct file *file, const char *buffer,
                             unsigned long count, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_desc *desc;
        int val, rc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        lod_fix_desc_pattern(&val);
        desc->ld_pattern = val;
        return count;
}

static int lod_rd_stripecount(char *page, char **start, off_t off, int count,
                              int *eof, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_desc *desc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        *eof = 1;
        return snprintf(page, count, "%d\n",
                        (__s16)(desc->ld_default_stripe_count + 1) - 1);
}

static int lod_wr_stripecount(struct file *file, const char *buffer,
                              unsigned long count, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_desc *desc;
        int val, rc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        lod_fix_desc_stripe_count(&val);
        desc->ld_default_stripe_count = val;
        return count;
}

static int lod_rd_numobd(char *page, char **start, off_t off, int count,
                         int *eof, void *data)
{
        struct obd_device *dev = (struct obd_device*)data;
        struct lov_desc *desc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        *eof = 1;
        return snprintf(page, count, "%u\n", desc->ld_tgt_count);

}

static int lod_rd_activeobd(char *page, char **start, off_t off, int count,
                            int *eof, void *data)
{
        struct obd_device* dev = (struct obd_device*)data;
        struct lov_desc *desc;

        LASSERT(dev != NULL);
        desc = &dev->u.lov.desc;
        *eof = 1;
        return snprintf(page, count, "%u\n", desc->ld_active_tgt_count);
}

static int lod_rd_desc_uuid(char *page, char **start, off_t off, int count,
                            int *eof, void *data)
{
        struct obd_device *dev = (struct obd_device*) data;
        struct lov_obd *lov;

        LASSERT(dev != NULL);
        lov = &dev->u.lov;
        *eof = 1;
        return snprintf(page, count, "%s\n", lov->desc.ld_uuid.uuid);
}

/* free priority (0-255): how badly user wants to choose empty osts */ 
static int lod_rd_qos_priofree(char *page, char **start, off_t off, int count,
                               int *eof, void *data)
{
        struct obd_device *dev = (struct obd_device*) data;
        struct lov_obd *lov;

        LASSERT(dev != NULL);
        lov = &dev->u.lov;
        *eof = 1;
        return snprintf(page, count, "%d%%\n", 
                        (lov->lov_qos.lq_prio_free * 100) >> 8);
}

static int lod_wr_qos_priofree(struct file *file, const char *buffer,
                               unsigned long count, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_obd *lov;
        int val, rc;
        LASSERT(dev != NULL);

        lov = &dev->u.lov;
        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val > 100)
                return -EINVAL;
        lov->lov_qos.lq_prio_free = (val << 8) / 100;
        lov->lov_qos.lq_dirty = 1;
        lov->lov_qos.lq_reset = 1;
        return count;
}

static int lod_rd_qos_thresholdrr(char *page, char **start, off_t off,
                                  int count, int *eof, void *data)
{
        struct obd_device *dev = (struct obd_device*) data;
        struct lov_obd *lov;

        LASSERT(dev != NULL);
        lov = &dev->u.lov;
        *eof = 1;
        return snprintf(page, count, "%d%%\n",
                        (lov->lov_qos.lq_threshold_rr * 100) >> 8);
}

static int lod_wr_qos_thresholdrr(struct file *file, const char *buffer,
                                  unsigned long count, void *data)
{
        struct obd_device *dev = (struct obd_device *)data;
        struct lov_obd *lov;
        int val, rc;
        LASSERT(dev != NULL);

        lov = &dev->u.lov;
        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val > 100 || val < 0)
                return -EINVAL;

        lov->lov_qos.lq_threshold_rr = (val << 8) / 100;
        lov->lov_qos.lq_dirty = 1;
        return count;
}

static int lod_rd_qos_maxage(char *page, char **start, off_t off, int count,
                             int *eof, void *data)
{
        struct obd_device *dev = (struct obd_device*) data;
        struct lov_obd *lov;

        LASSERT(dev != NULL);
        lov = &dev->u.lov;
        *eof = 1;
        return snprintf(page, count, "%u Sec\n", lov->desc.ld_qos_maxage);
}

static int lod_wr_qos_maxage(struct file *file, const char *buffer,
                             unsigned long count, void *data)
{
        struct obd_device      *dev = (struct obd_device *)data;
        struct lustre_cfg_bufs  bufs;
        struct lov_obd         *lov;
        struct lu_device       *next;
        struct lod_device      *d;
        struct lustre_cfg      *lcfg;
        char                    str[32];
        int                     val, rc, i;

        LASSERT(dev != NULL);
        d = lu2lod_dev(dev->obd_lu_dev);

        lov = &dev->u.lov;
        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val <= 0)
                return -EINVAL;
        lov->desc.ld_qos_maxage = val;

        /*
         * propogate the value down to OSPs
         */
        lustre_cfg_bufs_reset(&bufs, NULL);
        sprintf(str, "%smaxage=%d", PARAM_OSP, val); 
        lustre_cfg_bufs_set_string(&bufs, 1, str);
        lcfg = lustre_cfg_new(LCFG_PARAM, &bufs);
        for (i = 0; i < LOD_MAX_OSTNR; i++) {
                if (d->lod_ost[i] == NULL)
                        continue;
                next = &d->lod_ost[i]->dd_lu_dev;
                rc = next->ld_ops->ldo_process_config(NULL, next, lcfg);
                if (rc)
                        CERROR("can't set maxage on #%d: %d\n", i, rc);
        }
        lustre_cfg_free(lcfg);

        return count;
}

static void *lod_tgt_seq_start(struct seq_file *p, loff_t *pos)
{
        struct obd_device *dev = p->private;
        struct lov_obd *lov = &dev->u.lov;

        while (*pos < lov->desc.ld_tgt_count) {
                if (lov->lov_tgts[*pos])
                        return lov->lov_tgts[*pos];
                ++*pos;
        }
        return NULL;
}

static void lod_tgt_seq_stop(struct seq_file *p, void *v)
{
}

static void *lod_tgt_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
        struct obd_device *dev = p->private;
        struct lov_obd *lov = &dev->u.lov;

        while (++*pos < lov->desc.ld_tgt_count) {
                if (lov->lov_tgts[*pos])
                        return lov->lov_tgts[*pos];
        }
        return NULL;
}

static int lod_tgt_seq_show(struct seq_file *p, void *v)
{
        struct obd_device   *obd = p->private;
        struct lov_tgt_desc *tgt = v;
        struct lov_obd      *lov;
        struct lod_device   *d;
        int                  idx, rc;
        struct dt_device    *next;
        struct obd_statfs    sfs;

        LASSERT(obd->obd_lu_dev);
        d = lu2lod_dev(obd->obd_lu_dev);
        lov = &obd->u.lov;

        idx = tgt->ltd_index;
        next = d->lod_ost[idx];
        if (next == NULL)
                return -EINVAL;

        /* XXX: should be non-NULL env, but it's very expensive */
        rc = next->dd_ops->dt_statfs(NULL, next, &sfs);
        if (rc)
                return rc;

        tgt = lov->lov_tgts[idx];
        return seq_printf(p, "%d: %s %sACTIVE\n", idx,
                          obd_uuid2str(&tgt->ltd_uuid), 
                          sfs.os_blocks > 0 ? "" : "IN");
}

struct seq_operations lod_tgt_sops = {
        .start = lod_tgt_seq_start,
        .stop = lod_tgt_seq_stop,
        .next = lod_tgt_seq_next,
        .show = lod_tgt_seq_show,
};

static int lod_target_seq_open(struct inode *inode, struct file *file)
{
        struct proc_dir_entry *dp = PDE(inode);
        struct seq_file *seq;
        int rc;

        LPROCFS_ENTRY_AND_CHECK(dp);
        rc = seq_open(file, &lod_tgt_sops);
        if (rc) {
                LPROCFS_EXIT();
                return rc;
        }

        seq = file->private_data;
        seq->private = dp->data;
        return 0;
}

struct lprocfs_vars lprocfs_lod_obd_vars[] = {
        { "uuid",         lprocfs_rd_uuid,        0, 0 },
        { "stripesize",   lod_rd_stripesize,      lod_wr_stripesize, 0 },
        { "stripeoffset", lod_rd_stripeoffset,    lod_wr_stripeoffset, 0 },
        { "stripecount",  lod_rd_stripecount,     lod_wr_stripecount, 0 },
        { "stripetype",   lod_rd_stripetype,      lod_wr_stripetype, 0 },
        { "numobd",       lod_rd_numobd,          0, 0 },
        { "activeobd",    lod_rd_activeobd,       0, 0 },
        { "filestotal",   lprocfs_rd_filestotal,  0, 0 },
        { "filesfree",    lprocfs_rd_filesfree,   0, 0 },
        /*{ "filegroups", lprocfs_rd_filegroups,  0, 0 },*/
        { "blocksize",    lprocfs_rd_blksize,     0, 0 },
        { "kbytestotal",  lprocfs_rd_kbytestotal, 0, 0 },
        { "kbytesfree",   lprocfs_rd_kbytesfree,  0, 0 },
        { "kbytesavail",  lprocfs_rd_kbytesavail, 0, 0 },
        { "desc_uuid",    lod_rd_desc_uuid,       0, 0 },
        { "qos_prio_free",lod_rd_qos_priofree,    lod_wr_qos_priofree, 0 },
        { "qos_threshold_rr",  lod_rd_qos_thresholdrr, lod_wr_qos_thresholdrr, 0 },
        { "qos_maxage",   lod_rd_qos_maxage,      lod_wr_qos_maxage, 0 },
        { 0 }
};

static struct lprocfs_vars lprocfs_lod_module_vars[] = {
        { "num_refs",     lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

void lprocfs_lod_init_vars(struct lprocfs_static_vars *lvars)
{
    lvars->module_vars  = lprocfs_lod_module_vars;
    lvars->obd_vars     = lprocfs_lod_obd_vars;
}

struct file_operations lod_proc_target_fops = {
        .owner   = THIS_MODULE,
        .open    = lod_target_seq_open,
        .read    = seq_read,
        .llseek  = seq_lseek,
        .release = lprocfs_seq_release,
};
#endif /* LPROCFS */

