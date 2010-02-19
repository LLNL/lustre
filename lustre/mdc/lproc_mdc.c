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
#include <linux/vfs.h>
#include <obd_class.h>
#include <lprocfs_status.h>

#ifdef LPROCFS

static int mdc_rd_max_rpcs_in_flight(char *page, char **start, off_t off,
                                     int count, int *eof, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int rc;

        spin_lock(&cli->cl_loi_list_lock);
        rc = snprintf(page, count, "%u\n", cli->cl_max_rpcs_in_flight);
        spin_unlock(&cli->cl_loi_list_lock);
        return rc;
}

static int mdc_wr_max_rpcs_in_flight(struct file *file, const char *buffer,
                                     unsigned long count, void *data)
{
        struct obd_device *dev = data;
        struct client_obd *cli = &dev->u.cli;
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 1 || val > MDC_MAX_RIF_MAX)
                return -ERANGE;

        spin_lock(&cli->cl_loi_list_lock);
        cli->cl_max_rpcs_in_flight = val;
        spin_unlock(&cli->cl_loi_list_lock);

        return count;
}
static struct lprocfs_vars lprocfs_mdc_obd_vars[] = {
        { "uuid",            lprocfs_rd_uuid,        0, 0 },
        { "ping",            0, lprocfs_wr_ping,     0, 0, 0222 },
        { "connect_flags",   lprocfs_rd_connect_flags, 0, 0 },
        { "blocksize",       lprocfs_rd_blksize,     0, 0 },
        { "kbytestotal",     lprocfs_rd_kbytestotal, 0, 0 },
        { "kbytesfree",      lprocfs_rd_kbytesfree,  0, 0 },
        { "kbytesavail",     lprocfs_rd_kbytesavail, 0, 0 },
        { "filestotal",      lprocfs_rd_filestotal,  0, 0 },
        { "filesfree",       lprocfs_rd_filesfree,   0, 0 },
        /*{ "filegroups",      lprocfs_rd_filegroups,  0, 0 },*/
        { "mds_server_uuid", lprocfs_rd_server_uuid, 0, 0 },
        { "mds_conn_uuid",   lprocfs_rd_conn_uuid,   0, 0 },
        { "max_rpcs_in_flight", mdc_rd_max_rpcs_in_flight,
                                mdc_wr_max_rpcs_in_flight, 0 },
        { "timeouts",        lprocfs_rd_timeouts,    0, 0 },
        { "import",          lprocfs_rd_import,      0, 0 },
        { "state",           lprocfs_rd_state,       0, 0 },
        { 0 }
};

static struct lprocfs_vars lprocfs_mdc_module_vars[] = {
        { "num_refs",        lprocfs_rd_numrefs,     0, 0 },
        { 0 }
};

void lprocfs_mdc_init_vars(struct lprocfs_static_vars *lvars)
{
    lvars->module_vars  = lprocfs_mdc_module_vars;
    lvars->obd_vars     = lprocfs_mdc_obd_vars;
}
#endif /* LPROCFS */
