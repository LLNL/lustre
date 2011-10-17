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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/utils/tunefs_lustre.c
 *
 * Author: Nathan Rutman <nathan@clusterfs.com>
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

#include "mount_utils.h"

char *progname;
int verbose = 1;

static void usage(FILE *out)
{
        fprintf(out, "%s v"LUSTRE_VERSION_STRING"\n", progname);
        fprintf(out, "usage: %s <target types> [options] "
                "<pool name>/<dataset name>\n", progname);
        fprintf(out, "usage: %s <target types> [options] <device>\n", progname);
        fprintf(out,
                "\t<device>:block device or file (e.g /dev/sda or /tmp/ost1)\n"
                "\t<pool name>: name of the ZFS pool where to create the "
                "target (e.g. tank)\n"
                "\t<dataset name>: name of the new dataset (e.g. ost1). The "
                "dataset name must be unique within the ZFS pool\n"
                "\n"
                "\ttarget types:\n"
                "\t\t--ost: object storage, mutually exclusive with mdt,mgs\n"
                "\t\t--mdt: metadata storage, mutually exclusive with ost\n"
                "\t\t--mgs: configuration management service - one per site\n"
                "\n"
                "\toptions (in order of popularity):\n"
                "\t\t--mgsnode=<nid>[,<...>] : NID(s) of a remote mgs node\n"
                "\t\t\trequired for all targets other than the mgs node\n"
                "\t\t--fsname=<filesystem_name> : default is 'lustre'\n"
                "\t\t--failnode=<nid>[,<...>] : NID(s) of a failover partner\n"
                "\t\t\tcannot be used with --servicenode\n"
                "\t\t--servicenode=<nid>[,<...>] : NID(s) of all service\n"
                "\t\tpartners treat all nodes as equal service node, cannot\n"
                "\t\tbe used with --failnode\n"
                "\t\t--param <key>=<value> : set a permanent parameter\n"
                "\t\t\te.g. --param sys.timeout=40\n"
                "\t\t\t     --param lov.stripesize=2M\n"
                "\t\t--index=#N : target index (i.e. ost index within lov)\n"
                "\t\t--comment=<user comment>: arbitrary string (%d bytes)\n"
                "\t\t--mountfsoptions=<opts> : permanent mount options\n"
                "\t\t--network=<net>[,<...>] : restrict OST/MDT to network(s)\n"
                "\t\t--erase-params : erase all old parameter settings\n"
                "\t\t--nomgs: turn off MGS service on this MDT\n"
                "\t\t--writeconf: erase all config logs for this fs.\n"
                "\t\t--quota: enable space accounting on old 2.x device.\n"
                "\t\t--dryrun: just report, don't write to disk\n"
                "\t\t--verbose : e.g. show mkfs progress\n"
                "\t\t--quiet\n",
                (int)sizeof(((struct lustre_disk_data *)0)->ldd_userdata));
        return;
}

static void set_defaults(struct mkfs_opts *mop)
{
        memset(mop, 0, sizeof(*mop));
        mop->mo_ldd.ldd_magic = LDD_MAGIC;
        mop->mo_ldd.ldd_config_ver = 1;
        mop->mo_ldd.ldd_flags = LDD_F_NEED_INDEX | LDD_F_UPDATE | LDD_F_VIRGIN;
        mop->mo_mgs_failnodes = 0;
        strcpy(mop->mo_ldd.ldd_fsname, "lustre");
        mop->mo_ldd.ldd_mount_type = LDD_MT_LDISKFS;

        mop->mo_ldd.ldd_svindex = 0;
        mop->mo_stripe_count = 1;
        mop->mo_pool_vdevs = NULL;
}

static void badopt(const char *opt, char *type)
{
        fprintf(stderr, "%s: '--%s' only valid for %s\n",
                progname, opt, type);
        usage(stderr);
}

static int parse_opts(int argc, char *const argv[], struct mkfs_opts *mop,
               char **mountopts)
{
        static struct option long_opt[] = {
                {"comment", 1, 0, 'u'},
                {"dryrun", 0, 0, 'n'},
                {"erase-params", 0, 0, 'e'},
                {"failnode", 1, 0, 'f'},
                {"failover", 1, 0, 'f'},
                {"mgs", 0, 0, 'G'},
                {"help", 0, 0, 'h'},
                {"index", 1, 0, 'i'},
                {"mgsnode", 1, 0, 'm'},
                {"mgsnid", 1, 0, 'm'},
                {"mdt", 0, 0, 'M'},
                {"fsname",1, 0, 'L'},
                {"noformat", 0, 0, 'n'},
                {"nomgs", 0, 0, 'N'},
                {"mountfsoptions", 1, 0, 'o'},
                {"ost", 0, 0, 'O'},
                {"param", 1, 0, 'p'},
                {"print", 0, 0, 'n'},
                {"quiet", 0, 0, 'q'},
                {"servicenode", 1, 0, 's'},
                {"verbose", 0, 0, 'v'},
                {"writeconf", 0, 0, 'w'},
                {"network", 1, 0, 't'},
                {"quota", 0, 0, 'Q'},
                {0, 0, 0, 0}
        };
        char *optstring = "C:ef:Ghi:L:m:MnNo:Op:Pqs:t:u:vw";
        int opt;
        int rc, longidx;
        int failnode_set = 0, servicenode_set = 0;

        while ((opt = getopt_long(argc, argv, optstring, long_opt, &longidx)) !=
               EOF) {
                switch (opt) {
                case 'e':
                        mop->mo_ldd.ldd_params[0] = '\0';
                        /* Must update the mgs logs */
                        mop->mo_ldd.ldd_flags |= LDD_F_UPDATE;
                        break;
                case 'f':
                case 's': {
                        char *nids;

                        if ((opt == 'f' && servicenode_set)
                            || (opt == 's' && failnode_set)) {
                                fprintf(stderr, "%s: %s cannot use with --%s\n",
                                        progname, long_opt[longidx].name,
                                        opt == 'f' ? "servicenode" : "failnode");
                                return 1;
                        }

                        nids = convert_hostnames(optarg);
                        if (!nids)
                                return 1;
                        rc = add_param(mop->mo_ldd.ldd_params, PARAM_FAILNODE,
                                       nids);
                        free(nids);
                        if (rc)
                                return rc;
                        /* Must update the mgs logs */
                        mop->mo_ldd.ldd_flags |= LDD_F_UPDATE;
                        if (opt == 'f') {
                                failnode_set = 1;
                        } else {
                                mop->mo_ldd.ldd_flags |= LDD_F_NO_PRIMNODE;
                                servicenode_set = 1;
                        }
                        mop->mo_flags |= MO_FAILOVER;
                        break;
                }
                case 'G':
                        mop->mo_ldd.ldd_flags |= LDD_F_SV_TYPE_MGS;
                        break;
                case 'h':
                        usage(stdout);
                        return 1;
                case 'i':
                        if (!(mop->mo_ldd.ldd_flags &
                              (LDD_F_UPGRADE14 | LDD_F_VIRGIN |
                               LDD_F_WRITECONF))) {
                                fprintf(stderr, "%s: cannot change the index of"
                                        " a registered target\n", progname);
                                return 1;
                        }
                        if (IS_MDT(&mop->mo_ldd) || IS_OST(&mop->mo_ldd)) {
                                mop->mo_ldd.ldd_svindex = atol(optarg);
                                mop->mo_ldd.ldd_flags &= ~LDD_F_NEED_INDEX;
                        } else {
                                badopt(long_opt[longidx].name, "MDT,OST");
                                return 1;
                        }
                        break;
                case 'L': {
                        char *tmp;
                        if (!(mop->mo_flags & MO_FORCEFORMAT) &&
                            (!(mop->mo_ldd.ldd_flags &
                               (LDD_F_UPGRADE14 | LDD_F_VIRGIN |
                                LDD_F_WRITECONF)))) {
                                fprintf(stderr, "%s: cannot change the name of"
                                        " a registered target\n", progname);
                                return 1;
                        }
                        if ((strlen(optarg) < 1) || (strlen(optarg) > 8)) {
                                fprintf(stderr, "%s: filesystem name must be "
                                        "1-8 chars\n", progname);
                                return 1;
                        }
                        if ((tmp = strpbrk(optarg, "/:"))) {
                                fprintf(stderr, "%s: char '%c' not allowed in "
                                        "filesystem name\n", progname, *tmp);
                                return 1;
                        }
                        strscpy(mop->mo_ldd.ldd_fsname, optarg,
                                sizeof(mop->mo_ldd.ldd_fsname));
                        break;
                }
                case 'm': {
                        char *nids = convert_hostnames(optarg);
                        if (!nids)
                                return 1;
                        rc = add_param(mop->mo_ldd.ldd_params, PARAM_MGSNODE,
                                       nids);
                        free(nids);
                        if (rc)
                                return rc;
                        mop->mo_mgs_failnodes++;
                        break;
                }
                case 'M':
                        mop->mo_ldd.ldd_flags |= LDD_F_SV_TYPE_MDT;
                        break;
                case 'n':
                        mop->mo_flags |= MO_DRYRUN;
                        break;
                case 'N':
                        mop->mo_ldd.ldd_flags &= ~LDD_F_SV_TYPE_MGS;
                        break;
                case 'o':
                        *mountopts = optarg;
                        break;
                case 'O':
                        mop->mo_ldd.ldd_flags |= LDD_F_SV_TYPE_OST;
                        break;
                case 'p':
                        rc = add_param(mop->mo_ldd.ldd_params, NULL, optarg);
                        if (rc)
                                return rc;
                        /* Must update the mgs logs */
                        mop->mo_ldd.ldd_flags |= LDD_F_UPDATE;
                        break;
                case 'q':
                        verbose--;
                        break;
                case 't':
                        if (!IS_MDT(&mop->mo_ldd) && !IS_OST(&mop->mo_ldd)) {
                                badopt(long_opt[longidx].name, "MDT,OST");
                                return 1;
                        }

                        if (!optarg)
                                return 1;

                        rc = add_param(mop->mo_ldd.ldd_params,
                                       PARAM_NETWORK, optarg);
                        if (rc != 0)
                                return rc;
                        /* Must update the mgs logs */
                        mop->mo_ldd.ldd_flags |= LDD_F_UPDATE;
                        break;
                case 'u':
                        strscpy(mop->mo_ldd.ldd_userdata, optarg,
                                sizeof(mop->mo_ldd.ldd_userdata));
                        break;
                case 'v':
                        verbose++;
                        break;
                case 'w':
                        mop->mo_ldd.ldd_flags |= LDD_F_WRITECONF;
                        break;
                case 'Q':
                        mop->mo_flags |= MO_QUOTA;
                        break;
                default:
                        if (opt != '?') {
                                fatal();
                                fprintf(stderr, "Unknown option '%c'\n", opt);
                        }
                        return EINVAL;
                }
        }

        if (optind == argc) {
                /* The user didn't specify device name */
                fatal();
                fprintf(stderr, "Not enough arguments - device name or "
                        "pool/dataset name not specified.\n");
                return EINVAL;
        } else {
                /*  The device or pool/filesystem name */
                strscpy(mop->mo_device, argv[optind], sizeof(mop->mo_device));
        }

        return 0;
}

int main(int argc, char *const argv[])
{
        struct mkfs_opts mop;
        struct lustre_disk_data *ldd = &mop.mo_ldd;
        char *mountopts = NULL;
        char always_mountopts[512] = "";
        char default_mountopts[512] = "";
        unsigned mount_type;
        int ret = 0;

        if ((progname = strrchr(argv[0], '/')) != NULL)
                progname++;
        else
                progname = argv[0];

        if ((argc < 2) || (argv[argc - 1][0] == '-')) {
                usage(stderr);
                return EINVAL;
        }

        set_defaults(&mop);

        /* device is last arg */
        strscpy(mop.mo_device, argv[argc - 1], sizeof(mop.mo_device));

        ret = osd_init();
        if (ret)
                return ret;

        /* Check whether the disk has already been formatted by mkfs.lustre */
        ret = osd_is_lustre(mop.mo_device, &mount_type);
        if (ret == 0) {
                fatal();
                fprintf(stderr, "Device %s has not been formatted with "
                        "mkfs.lustre\n", mop.mo_device);
                ret = ENODEV;
                goto out;
        }

        ldd->ldd_mount_type = mount_type;
        ret = osd_read_ldd(mop.mo_device, ldd);
        if (ret) {
                fatal();
                fprintf(stderr, "Failed to read previous Lustre data from %s "
                        "(%d)\n", mop.mo_device, ret);
                goto out;
        }
        if (strstr(ldd->ldd_params, PARAM_MGSNODE))
            mop.mo_mgs_failnodes++;

        if (verbose > 0)
                osd_print_ldd("Read previous values", ldd);

        ret = parse_opts(argc, argv, &mop, &mountopts);
        if (ret)
                goto out;

        if (!(IS_MDT(ldd) || IS_OST(ldd) || IS_MGS(ldd))) {
                fatal();
                fprintf(stderr, "must set target type: MDT,OST,MGS\n");
                ret = EINVAL;
                goto out;
        }

        if (((IS_MDT(ldd) || IS_MGS(ldd))) && IS_OST(ldd)) {
                fatal();
                fprintf(stderr, "OST type is exclusive with MDT,MGS\n");
                ret = EINVAL;
                goto out;
        }

        if (IS_MGS(ldd) && !IS_MDT(ldd)) {
                mop.mo_ldd.ldd_flags &= ~LDD_F_NEED_INDEX;
                mop.mo_ldd.ldd_svindex = 0;
        }

        if (mop.mo_ldd.ldd_flags & LDD_F_NEED_INDEX) {
                fatal();
                fprintf(stderr, "The target index must be specified with "
                        "--index\n");
                ret = EINVAL;
                goto out;
        }

        ret = osd_prepare_lustre(&mop,
                                 default_mountopts, sizeof(default_mountopts),
                                 always_mountopts, sizeof(always_mountopts));
        if (ret) {
                fatal();
                fprintf(stderr, "unable to prepare backend (%d)\n", ret);
                goto out;
        }

        if (mountopts) {
                trim_mountfsoptions(mountopts);
                (void)check_mountfsoptions(mountopts, default_mountopts, 1);
                if (check_mountfsoptions(mountopts, always_mountopts, 0)) {
                        ret = EINVAL;
                        goto out;
                }
                sprintf(ldd->ldd_mount_opts, "%s", mountopts);
        } else {
                /* use the defaults unless old opts exist */
                if (ldd->ldd_mount_opts[0] == 0) {
                        sprintf(ldd->ldd_mount_opts, "%s%s",
                                always_mountopts, default_mountopts);
                        trim_mountfsoptions(ldd->ldd_mount_opts);
                }
        }

        server_make_name(ldd->ldd_flags, ldd->ldd_svindex,
                         ldd->ldd_fsname, ldd->ldd_svname);

        if (verbose >= 0)
                osd_print_ldd("Permanent disk data", ldd);

        if (mop.mo_flags & MO_DRYRUN) {
                fprintf(stderr, "Dry-run, exiting before disk write.\n");
                goto out;
        }

        if (check_mtab_entry(mop.mo_device, mop.mo_device, NULL, NULL)) {
                fprintf(stderr, "filesystem %s is mounted.\n", mop.mo_device);
                ret = EEXIST;
                goto out;
        }

        /* Create the loopback file */
        if (mop.mo_flags & MO_IS_LOOP) {
                ret = access(mop.mo_device, F_OK);
                if (ret)
                        ret = errno;

                if (ret == 0)
                        ret = loop_setup(&mop);
                if (ret) {
                        fatal();
                        fprintf(stderr, "Loop device setup for %s failed: %s\n",
                                mop.mo_device, strerror(ret));
                        goto out;
                }
        }

        /* Enable quota accounting */
        if (mop.mo_flags & MO_QUOTA) {
                ret = osd_enable_quota(&mop);
                goto out;
        }

        /* Write our config files */
        ret = osd_write_ldd(&mop);
        if (ret != 0) {
                fatal();
                fprintf(stderr, "failed to write local files\n");
                goto out;
        }

out:
        loop_cleanup(&mop);
        osd_fini();

        /* Fix any crazy return values from system() */
        if (ret && ((ret & 255) == 0))
                return (1);
        if (ret)
                verrprint("%s: exiting with %d (%s)\n",
                          progname, ret, strerror(ret));
        return (ret);
}
