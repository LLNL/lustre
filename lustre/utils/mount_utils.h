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
 */

#ifndef _MOUNT_UTILS_H_
#define _MOUNT_UTILS_H_

#include <lustre_disk.h>

#define CONFIGS_FILE      "mountdata"
/** Persistent mount data are stored on the disk in this file. */
#define MOUNT_DATA_FILE    MOUNT_CONFIGS_DIR"/"CONFIGS_FILE

#define LDD_MAGIC 0x1dd00001

/* On-disk configuration file. In host-endian order. */
struct lustre_disk_data {
        __u32      ldd_magic;
        __u32      ldd_feature_compat;  /* compatible feature flags */
        __u32      ldd_feature_rocompat;/* read-only compatible feature flags */
        __u32      ldd_feature_incompat;/* incompatible feature flags */

        __u32      ldd_config_ver;      /* config rewrite count - not used */
        __u32      ldd_flags;           /* LDD_SV_TYPE */
        __u32      ldd_svindex;         /* server index (0001), must match
                                         svname */
        __u32      ldd_mount_type;      /* target fs type LDD_MT_* */
        char       ldd_fsname[64];      /* filesystem this server is part of,
                                         MTI_NAME_MAXLEN */
        char       ldd_svname[64];      /* this server's name (lustre-mdt0001)*/
        __u8       ldd_uuid[40];        /* server UUID (COMPAT_146) */

        /*200*/ char       ldd_userdata[1024 - 200]; /* arbitrary user string */
        /*1024*/__u8       ldd_padding[4096 - 1024];
        /*4096*/char       ldd_mount_opts[4096]; /* target fs mount opts */
        /*8192*/char       ldd_params[4096];     /* key=value pairs */
};

#undef IS_MDT
#define IS_MDT(data)   ((data)->ldd_flags & LDD_F_SV_TYPE_MDT)
#undef IS_OST
#define IS_OST(data)   ((data)->ldd_flags & LDD_F_SV_TYPE_OST)
#undef IS_MGS
#define IS_MGS(data)  ((data)->ldd_flags & LDD_F_SV_TYPE_MGS)

void fatal(void);
int run_command(char *, int);
int run_command_err(char *, int, char *);
int get_mountdata(char *, struct lustre_disk_data *);
char *convert_hostnames(char *s1);
void register_service_tags(char *, char *, char *);

#endif
