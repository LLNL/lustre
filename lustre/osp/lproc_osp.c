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

#include <obd_cksum.h>
#include <obd_class.h>
#include <lprocfs_status.h>
#include <linux/seq_file.h>
#include "osp_internal.h"

#ifdef LPROCFS
static int osp_rd_active(char *page, char **start, off_t off,
                         int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        int rc;

        LPROCFS_CLIMP_CHECK(dev);
        rc = snprintf(page, count, "%d\n", !dev->u.cli.cl_import->imp_deactive);
        LPROCFS_CLIMP_EXIT(dev);
        return rc;
}

static int osp_wr_active(struct file *file, const char *buffer,
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

static int osp_rd_syn_in_flight(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int rc;

        if (osp == NULL)
                return -EINVAL;

        rc = snprintf(page, count, "%u\n", osp->opd_syn_rpc_in_flight);
        return rc;
}

static int osp_rd_syn_in_prog(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int rc;

        if (osp == NULL)
                return -EINVAL;

        rc = snprintf(page, count, "%u\n", osp->opd_syn_rpc_in_progress);
        return rc;
}

static int osp_rd_syn_changes(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int rc;

        if (osp == NULL)
                return -EINVAL;

        rc = snprintf(page, count, "%lu\n", osp->opd_syn_changes);
        return rc;
}

static int osp_rd_max_rpcs_in_flight(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int rc;

        if (osp == NULL)
                return -EINVAL;

        rc = snprintf(page, count, "%u\n", osp->opd_syn_max_rpc_in_flight);
        return rc;
}

static int osp_wr_max_rpcs_in_flight(struct file *file, const char *buffer,
                                     unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int val, rc;

        if (osp == NULL)
                return -EINVAL;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 1)
                return -ERANGE;

        osp->opd_syn_max_rpc_in_flight = val;

        return count;
}

static int osp_rd_max_rpcs_in_prog(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int rc;

        if (osp == NULL)
                return -EINVAL;

        rc = snprintf(page, count, "%u\n", osp->opd_syn_max_rpc_in_progress);
        return rc;
}

static int osp_wr_max_rpcs_in_prog(struct file *file, const char *buffer,
                                     unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int val, rc;

        if (osp == NULL)
                return -EINVAL;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 1)
                return -ERANGE;

        osp->opd_syn_max_rpc_in_progress = val;

        return count;
}

static int osp_rd_create_count(char *page, char **start, off_t off, int count,
                               int *eof, void *data)
{
        struct obd_device *obd = data;
        struct osp_device *osp = lu2osp_dev(obd->obd_lu_dev);

        if (osp == NULL)
                return 0;

        return snprintf(page, count, "%d\n", osp->opd_pre_grow_count);
}

static int osp_wr_create_count(struct file *file, const char *buffer,
                               unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct osp_device *osp = lu2osp_dev(obd->obd_lu_dev);
        int val, rc, i;

        if (osp == NULL)
                return 0;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        /* The MDT ALWAYS needs to limit the precreate count to
         * OST_MAX_PRECREATE, and the constant cannot be changed
         * because it is a value shared between the OSP and OST
         * that is the maximum possible number of objects that will
         * ever be handled by MDT->OST recovery processing.
         *
         * If the OST ever gets a request to delete more orphans,
         * this implies that something has gone badly on the MDT
         * and the OST will refuse to delete so much data from the
         * filesystem as a safety measure. */
        if (val < OST_MIN_PRECREATE || val > OST_MAX_PRECREATE)
                return -ERANGE;
        if (val > osp->opd_pre_max_grow_count)
                return -ERANGE;

        for (i = 1; (i << 1) <= val; i <<= 1)
                ;
        osp->opd_pre_grow_count = i;

        return count;
}

static int osp_rd_max_create_count(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct osp_device *osp = lu2osp_dev(obd->obd_lu_dev);

        if (osp == NULL)
                return 0;

        return snprintf(page, count, "%d\n", osp->opd_pre_max_grow_count);
}

static int osp_wr_max_create_count(struct file *file, const char *buffer,
                                   unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct osp_device *osp = lu2osp_dev(obd->obd_lu_dev);
        int val, rc;

        if (osp == NULL)
                return 0;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 0)
                return -ERANGE;
        if (val > OST_MAX_PRECREATE)
                return -ERANGE;

        if (osp->opd_pre_grow_count > val)
                osp->opd_pre_grow_count = val;

        osp->opd_pre_max_grow_count = val;

        return count;
}

static int osp_rd_prealloc_next_id(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct osp_device *osp = lu2osp_dev(obd->obd_lu_dev);

        if (osp == NULL)
                return 0;

        return snprintf(page, count, LPU64"\n", osp->opd_pre_next);
}

static int osp_rd_prealloc_last_id(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct osp_device *osp = lu2osp_dev(obd->obd_lu_dev);

        if (osp == NULL)
                return 0;

        return snprintf(page, count, LPU64"\n", osp->opd_pre_last_created);
}

static int osp_rd_prealloc_reserved(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct osp_device *osp = lu2osp_dev(obd->obd_lu_dev);

        if (osp == NULL)
                return 0;

        return snprintf(page, count, LPU64"\n", osp->opd_pre_reserved);
}

static int osp_rd_maxage(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int rc;

        if (osp == NULL)
                return -EINVAL;

        rc = snprintf(page, count, "%u\n", osp->opd_statfs_maxage);
        return rc;
}

static int osp_wr_maxage(struct file *file, const char *buffer,
                                     unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int val, rc;

        if (osp == NULL)
                return -EINVAL;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 1)
                return -ERANGE;

        osp->opd_statfs_maxage = val;

        return count;
}

static int osp_rd_pre_status(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int rc;

        if (osp == NULL)
                return -EINVAL;

        rc = snprintf(page, count, "%d\n", osp->opd_pre_status);
        return rc;
}

static int osp_rd_destroys_in_flight(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);

        /*
         * This counter used to determine if OST has space returned.
         * Now we need to wait for the following:
         * - sync changes are zero - no llog records
         * - sync in progress are zero - no RPCs in flight
         */
        return snprintf(page, count, "%lu\n",
                        osp->opd_syn_rpc_in_progress + osp->opd_syn_changes);
}

static int osp_rd_old_sync_processed(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct osp_device *osp = lu2osp_dev(dev->obd_lu_dev);
        int rc;

        if (osp == NULL)
                return -EINVAL;

        rc = snprintf(page, count, "%d\n", osp->opd_syn_prev_done);
        return rc;
}

static struct lprocfs_vars lprocfs_osp_obd_vars[] = {
        { "uuid",                 lprocfs_rd_uuid,          0, 0 },
        { "ping",                 0, lprocfs_wr_ping,       0, 0, 0222 },
        { "connect_flags",        lprocfs_rd_connect_flags, 0, 0 },
        { "blocksize",            lprocfs_rd_blksize,       0, 0 },
        { "kbytestotal",          lprocfs_rd_kbytestotal,   0, 0 },
        { "kbytesfree",           lprocfs_rd_kbytesfree,    0, 0 },
        { "kbytesavail",          lprocfs_rd_kbytesavail,   0, 0 },
        { "filestotal",           lprocfs_rd_filestotal,    0, 0 },
        { "filesfree",            lprocfs_rd_filesfree,     0, 0 },
        { "ost_server_uuid",      lprocfs_rd_server_uuid,   0, 0 },
        { "ost_conn_uuid",        lprocfs_rd_conn_uuid,     0, 0 },
        { "active",               osp_rd_active,
                                  osp_wr_active,               0 },
        { "max_rpcs_in_flight",   osp_rd_max_rpcs_in_flight,
                                  osp_wr_max_rpcs_in_flight,   0 },
        { "max_rpcs_in_progress", osp_rd_max_rpcs_in_prog,
                                  osp_wr_max_rpcs_in_prog,     0 },
        { "create_count",         osp_rd_create_count,
                                  osp_wr_create_count,         0 },
        { "max_create_count",     osp_rd_max_create_count,
                                  osp_wr_max_create_count,     0 },
        { "prealloc_next_id",     osp_rd_prealloc_next_id,  0, 0 },
        { "prealloc_last_id",     osp_rd_prealloc_last_id,  0, 0 },
        { "prealloc_reserved",    osp_rd_prealloc_reserved, 0, 0 },
        { "timeouts",             lprocfs_rd_timeouts,      0, 0 },
        { "import",               lprocfs_rd_import,        0, 0 },
        { "state",                lprocfs_rd_state,         0, 0 },
        { "maxage",               osp_rd_maxage, osp_wr_maxage,0 },
        { "prealloc_status",      osp_rd_pre_status,        0, 0 },
        { "sync_changes",         osp_rd_syn_changes,       0, 0 },
        { "sync_in_flight",       osp_rd_syn_in_flight,     0, 0 },
        { "sync_in_progress",     osp_rd_syn_in_prog,       0, 0 },
        { "old_sync_processed",   osp_rd_old_sync_processed,0, 0 },

        /* for compatibility reasons */
        { "destroys_in_flight",   osp_rd_destroys_in_flight, 0, 0 },
        { 0 }
};

static struct lprocfs_vars lprocfs_osp_module_vars[] = {
        { "num_refs",        lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

void lprocfs_osp_init_vars(struct lprocfs_static_vars *lvars)
{
        lvars->module_vars = lprocfs_osp_module_vars;
        lvars->obd_vars    = lprocfs_osp_obd_vars;
}
#endif /* LPROCFS */

