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

#include <stdio.h>
#include <string.h>
#include <libzfs/libzfs.h>

#include "mount_utils.h"

/* Persistent mount data is stored in these user  attributes */
#define LDD_VERSION_PROP        "lustre:version"
#define LDD_FLAGS_PROP          "lustre:flags"
#define LDD_INDEX_PROP          "lustre:index"
#define LDD_FSNAME_PROP         "lustre:fsname"
#define LDD_SVNAME_PROP         "lustre:svname"
#define LDD_UUID_PROP           "lustre:uuid"
#define LDD_USERDATA_PROP       "lustre:userdata"
#define LDD_MOUNTOPTS_PROP      "lustre:mountopts"
#define LDD_MGSNODE_PROP        "lustre:mgsnode"
#define LDD_FAILNODE_PROP       "lustre:failnode"
#define LDD_FAILMODE_PROP       "lustre:failmode"

static libzfs_handle_t *g_zfs;

static int zfs_set_prop_int(zfs_handle_t *zhp, char *prop, __u32 val)
{
        char str[64];
        int ret;

        (void) snprintf(str, sizeof (str), "%lu", (unsigned long)val);
        vprint("  %s=%s\n", prop, str);
        ret = zfs_prop_set(zhp, prop, str);

        return ret;
}

/*
 * Write the zfs property string, note that properties with a NULL or
 * zero-length value will not be written and 0 returned.
 */
static int zfs_set_prop_str(zfs_handle_t *zhp, char *prop, char *val)
{
        int ret = 0;

        if (val && strlen(val) > 0) {
                vprint("  %s=%s\n", prop, val);
                ret = zfs_prop_set(zhp, prop, val);
        }

        return ret;
}

static int zfs_set_prop_param(zfs_handle_t *zhp, struct lustre_disk_data *ldd,
                              char *param, char *prop)
{
        char *str;
        int ret = 0;

        if (get_param(ldd->ldd_params, param, &str) == 0) {
                vprint("  %s=%s\n", prop, str);
                ret = zfs_prop_set(zhp, prop, str);
                free(str);
        }

        return ret;
}

/* Write the server config as properties associated with the dataset */
int zfs_write_ldd(struct mkfs_opts *mop)
{
        struct lustre_disk_data *ldd = &mop->mo_ldd;
        char *ds = mop->mo_device;
        zfs_handle_t *zhp;
        int ret = EINVAL;

        zhp = zfs_open(g_zfs, ds, ZFS_TYPE_FILESYSTEM);
        if (zhp == NULL) {
                fprintf(stderr, "Failed to open zfs dataset %s\n", ds);
                goto out;
        }

        vprint("Writing %s properties\n", ds);

        ret = zfs_set_prop_int(zhp, LDD_VERSION_PROP, ldd->ldd_config_ver);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_int(zhp, LDD_FLAGS_PROP, ldd->ldd_flags);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_int(zhp, LDD_INDEX_PROP, ldd->ldd_svindex);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_str(zhp, LDD_FSNAME_PROP, ldd->ldd_fsname);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_str(zhp, LDD_SVNAME_PROP, ldd->ldd_svname);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_str(zhp, LDD_UUID_PROP, (char *)ldd->ldd_uuid);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_str(zhp, LDD_USERDATA_PROP, ldd->ldd_userdata);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_str(zhp, LDD_MOUNTOPTS_PROP, ldd->ldd_mount_opts);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_param(zhp, ldd, PARAM_MGSNODE, LDD_MGSNODE_PROP);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_param(zhp, ldd, PARAM_FAILNODE, LDD_FAILNODE_PROP);
        if (ret)
                goto out_close;

        ret = zfs_set_prop_param(zhp, ldd, PARAM_FAILMODE, LDD_FAILMODE_PROP);
        if (ret)
                goto out_close;

out_close:
        zfs_close(zhp);
out:
        return ret;
}

static int zfs_get_prop_int(zfs_handle_t *zhp, char *prop, __u32 *val)
{
        nvlist_t *propval;
        char *propstr;
        int ret;

        ret = nvlist_lookup_nvlist(zfs_get_user_props(zhp), prop, &propval);
        if (ret)
                return ret;

        ret = nvlist_lookup_string(propval, ZPROP_VALUE, &propstr);
        if (ret)
                return ret;

        errno = 0;
        *val = strtoul(propstr, NULL, 10);
        if (errno)
                return errno;

        return ret;
}

static int zfs_get_prop_str(zfs_handle_t *zhp, char *prop, char *val)
{
        nvlist_t *propval;
        char *propstr;
        int ret;

        ret = nvlist_lookup_nvlist(zfs_get_user_props(zhp), prop, &propval);
        if (ret)
                return ret;

        ret = nvlist_lookup_string(propval, ZPROP_VALUE, &propstr);
        if (ret)
                return ret;

        (void) strcpy(val, propstr);

        return ret;
}

static int zfs_get_prop_param(zfs_handle_t *zhp, struct lustre_disk_data *ldd,
                              char *param, char *prop)
{
        nvlist_t *propval;
        char *propstr;
        int ret;

        ret = nvlist_lookup_nvlist(zfs_get_user_props(zhp), prop, &propval);
        if (ret)
                return ret;

        ret = nvlist_lookup_string(propval, ZPROP_VALUE, &propstr);
        if (ret)
                return ret;

        ret = add_param(ldd->ldd_params, param, propstr);

        return ret;
}

/*
 * Read the server config as properties associated with the dataset.
 * Missing entries as not treated error and are simply skipped.
 */
int zfs_read_ldd(char *ds,  struct lustre_disk_data *ldd)
{
        zfs_handle_t *zhp;
        int ret = EINVAL;

        zhp = zfs_open(g_zfs, ds, ZFS_TYPE_FILESYSTEM);
        if (zhp == NULL)
                goto out;

        ret = zfs_get_prop_int(zhp, LDD_VERSION_PROP, &ldd->ldd_config_ver);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_int(zhp, LDD_FLAGS_PROP, &ldd->ldd_flags);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_int(zhp, LDD_INDEX_PROP, &ldd->ldd_svindex);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_str(zhp, LDD_FSNAME_PROP, ldd->ldd_fsname);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_str(zhp, LDD_SVNAME_PROP, ldd->ldd_svname);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_str(zhp, LDD_UUID_PROP, (char *)ldd->ldd_uuid);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_str(zhp, LDD_USERDATA_PROP, ldd->ldd_userdata);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_str(zhp, LDD_MOUNTOPTS_PROP, ldd->ldd_mount_opts);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_param(zhp, ldd, PARAM_MGSNODE, LDD_MGSNODE_PROP);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_param(zhp, ldd, PARAM_FAILNODE, LDD_FAILNODE_PROP);
        if (ret && (ret != ENOENT))
                goto out_close;

        ret = zfs_get_prop_param(zhp, ldd, PARAM_FAILMODE, LDD_FAILMODE_PROP);
        if (ret && (ret != ENOENT))
                goto out_close;

        ldd->ldd_mount_type = LDD_MT_ZFS;
        ret = 0;
out_close:
        zfs_close(zhp);
out:
        return ret;
}

int zfs_is_lustre(char *ds, unsigned *mount_type)
{
        struct lustre_disk_data tmp_ldd;
        int ret;

        ret = zfs_read_ldd(ds, &tmp_ldd);
        if ((ret == 0) && (tmp_ldd.ldd_config_ver > 0) &&
            (strlen(tmp_ldd.ldd_fsname) > 0) &&
            (strlen(tmp_ldd.ldd_svname) > 0)) {
                *mount_type = tmp_ldd.ldd_mount_type;
                return 1;
        }

        return 0;
}

static char *zfs_refquota_opts(struct mkfs_opts *mop, char *str, int len)
{
        memset(str, 0, len);

        if (mop->mo_device_sz > 0)
                snprintf(str, len, " -o refquota="LPU64"K", mop->mo_device_sz);

        return str;

}

static char *zfs_mkfs_opts(struct mkfs_opts *mop, char *str, int len)
{
        memset(str, 0, len);

        if (mop->mo_mkfsopts != NULL)
                snprintf(str, len, " -o %s", mop->mo_mkfsopts);

        return str;
}

static int zfs_create_vdev(struct mkfs_opts *mop, char *vdev)
{
        int ret = 0;

        /* Silently ignore reserved vdev names */
        if ((strncmp(vdev, "disk", 4) == 0) ||
            (strncmp(vdev, "file", 4) == 0) ||
            (strncmp(vdev, "mirror", 6) == 0) ||
            (strncmp(vdev, "raidz", 5) == 0) ||
            (strncmp(vdev, "spare", 5) == 0) ||
            (strncmp(vdev, "log", 3) == 0) ||
            (strncmp(vdev, "cache", 5) == 0))
                return ret;

        /*
         * Verify a file exists at the provided absolute path.  If it doesn't
         * and mo_vdev_sz is set attempt to create a file vdev to be used.
         * Relative paths will be passed directly to 'zpool create' which
         * will check multiple multiple locations under /dev/.
         */
        if (vdev[0] == '/') {
                ret = access(vdev, F_OK);
                if (ret == 0)
                        return ret;

                ret = errno;
                if (ret != ENOENT) {
                        fatal();
                        fprintf(stderr, "Unable to access required vdev "
                                "for pool %s (%d)\n", vdev, ret);
                        return ret;
                }

                if (mop->mo_vdev_sz == 0) {
                        fatal();
                        fprintf(stderr, "Unable to create vdev due to "
                                "missing --vdev-size=#N(KB) parameter\n");
                        return EINVAL;
                }

                ret = file_create(vdev, mop->mo_vdev_sz);
                if (ret) {
                        fatal();
                        fprintf(stderr, "Unable to create vdev %s (%d)\n",
                                vdev, ret);
                        return ret;
                }
        }

        return ret;
}

int zfs_make_lustre(struct mkfs_opts *mop)
{
        zfs_handle_t *zhp;
        zpool_handle_t *php;
        char *pool = NULL;
        char *mkfs_cmd = NULL;
        char *mkfs_tmp = NULL;
        char *ds = mop->mo_device;
        int pool_exists = 0, ret;

        pool = strdup(ds);
        if (pool == NULL)
                return ENOMEM;

        mkfs_cmd = malloc(PATH_MAX);
        if (mkfs_cmd == NULL) {
                ret = ENOMEM;
                goto out;
        }

        mkfs_tmp = malloc(PATH_MAX);
        if (mkfs_tmp == NULL) {
                ret = ENOMEM;
                goto out;
        }

        /* Due to zfs_name_valid() check the '/' must exist */
        strchr(pool, '/')[0] = '\0';

        /* If --reformat was given attempt to destroy the previous dataset */
        if ((mop->mo_flags & MO_FORCEFORMAT) &&
            ((zhp = zfs_open(g_zfs, ds, ZFS_TYPE_FILESYSTEM)) != NULL)) {

                ret = zfs_destroy(zhp, 0);
                if (ret) {
                        zfs_close(zhp);
                        fprintf(stderr, "Failed destroy zfs dataset %s (%d)\n",
                                ds, ret);
                        goto out;
                }

                zfs_close(zhp);
        }

        /*
         * Create the zpool if the vdevs have been specified and the pool
         * does not already exists.  The pool creation itself will be done
         * with the zpool command rather than the zpool_create() library call
         * so the existing zpool error handling can be leveraged.
         */
        php = zpool_open(g_zfs, pool);
        if (php) {
                pool_exists = 1;
                zpool_close(php);
        }

        if ((mop->mo_pool_vdevs != NULL) && (pool_exists == 0)) {

                memset(mkfs_cmd, 0, PATH_MAX);
                snprintf(mkfs_cmd, PATH_MAX,
                         "zpool create -f -O canmount=off %s", pool);

                /* Append the vdev config and create file vdevs as required */
                while (*mop->mo_pool_vdevs != NULL) {
                        strscat(mkfs_cmd, " ", PATH_MAX);
                        strscat(mkfs_cmd, *mop->mo_pool_vdevs, PATH_MAX);

                        ret = zfs_create_vdev(mop, *mop->mo_pool_vdevs);
                        if (ret)
                                goto out;

                        mop->mo_pool_vdevs++;
                }

                vprint("mkfs_cmd = %s\n", mkfs_cmd);
                ret = run_command(mkfs_cmd, PATH_MAX);
                if (ret) {
                        fatal();
                        fprintf(stderr, "Unable to create pool %s (%d)\n",
                                pool, ret);
                        goto out;
                }
        }

        /*
         * Create the ZFS filesystem with any required mkfs options:
         * - canmount=off is set to prevent zfs automounting
         * - version=4 is set because SA are not yet handled by the osd
         */
        memset(mkfs_cmd, 0, PATH_MAX);
        snprintf(mkfs_cmd, PATH_MAX,
                 "zfs create -o canmount=off -o version=4%s%s %s",
                 zfs_refquota_opts(mop, mkfs_tmp, PATH_MAX),
                 zfs_mkfs_opts(mop, mkfs_tmp, PATH_MAX),
                 ds);

        vprint("mkfs_cmd = %s\n", mkfs_cmd);
        ret = run_command(mkfs_cmd, PATH_MAX);
        if (ret) {
                fatal();
                fprintf(stderr, "Unable to create filesystem %s (%d)\n",
                        ds, ret);
                goto out;
        }

out:
        if (pool != NULL)
                free(pool);

        if (mkfs_cmd != NULL)
                free(mkfs_cmd);

        if (mkfs_cmd != NULL)
                free(mkfs_tmp);

        return ret;
}

int zfs_prepare_lustre(struct mkfs_opts *mop,
                       char *default_mountopts, int default_len,
                       char *always_mountopts, int always_len)
{
        int ret;

        ret = zfs_name_valid(mop->mo_device, ZFS_TYPE_FILESYSTEM);
        if (!ret) {
                fatal();
                fprintf(stderr, "Invalid filesystem name %s\n", mop->mo_device);
                return EINVAL;
        }

        return 0;
}

int zfs_tune_lustre(char *dev, struct mount_opts *mop)
{
        return 0;
}

int zfs_label_lustre(struct mount_opts *mop)
{
        zfs_handle_t *zhp;
        int ret;

        zhp = zfs_open(g_zfs, mop->mo_source, ZFS_TYPE_FILESYSTEM);
        if (zhp == NULL)
                return EINVAL;

        ret = zfs_set_prop_str(zhp, LDD_SVNAME_PROP, mop->mo_ldd.ldd_svname);
        zfs_close(zhp);

        return ret;
}

int zfs_init(void)
{
        g_zfs = libzfs_init();
        if (g_zfs == NULL) {
                fprintf(stderr, "Failed to initialize ZFS library\n");
                return EINVAL;
        }

        return 0;
}

void zfs_fini(void)
{
        libzfs_fini(g_zfs);
}
