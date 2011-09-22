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
 *
 * Copyright (c) 2011 Whamcloud, Inc.
 *
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/utils/mount_lustre.c
 *
 * Author: Robert Read <rread@clusterfs.com>
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
#include <sys/mount.h>

#include "mount_utils.h"

#define MAX_RETRIES 99
#define MAXOPT 4096

char *progname = NULL;
int   verbose = 0;

static void usage(FILE *out)
{
        fprintf(out, "%s v"LUSTRE_VERSION_STRING"\n", progname);
        fprintf(out, "\nThis mount helper should only be invoked via the "
                "mount (8) command,\ne.g. mount -t lustre dev dir\n\n");
        fprintf(out, "usage: %s [-fhnv] [-o <mntopt>] <device> <mountpt>\n",
                progname);
        fprintf(out,
                "\t<device>: the disk device, or for a client:\n"
                "\t\t<mgmtnid>[:<altmgt>...]:/<filesystem>\n"
                "\t<filesystem>: name of the Lustre filesystem (e.g. lustre1)\n"
                "\t<mountpt>: filesystem mountpoint (e.g. /mnt/lustre)\n"
                "\t-f|--fake: fake mount (updates /etc/mtab)\n"
                "\t-o force|--force: force mount even if already in /etc/mtab\n"
                "\t-h|--help: print this usage message\n"
                "\t-n|--nomtab: do not update /etc/mtab after mount\n"
                "\t-v|--verbose: print verbose config settings\n"
                "\t<mntopt>: one or more comma separated of:\n"
                "\t\t(no)flock,(no)user_xattr,(no)acl\n"
                "\t\tabort_recov: abort server recovery handling\n"
                "\t\tmgs: start a Management Server with this MDT (MDT only)\n"
                "\t\tnomgs: do not start the MGS with this MDT\n"
                "\t\tnosvc: with the 'mgs' option, start only the MGS and not "
                "this MDT\n"
                "\t\texclude=<ostname>[:<ostname>] : colon-separated list of "
                "inactive OSTs (e.g. lustre-OST0001)\n"
                "\t\tretry=<num>: number of times mount is retried by client\n"
                "\t\tmd_stripe_cache_size=<num>: set the raid stripe cache "
                "size for the underlying raid if present\n"
                );
        exit((out != stdout) ? EINVAL : 0);
}

/*****************************************************************************
 *
 * This part was cribbed from util-linux/mount/mount.c.  There was no clear
 * license information, but many other files in the package are identified as
 * GNU GPL, so it's a pretty safe bet that was their intent.
 *
 ****************************************************************************/
struct opt_map {
        const char *opt;        /* option name */
        int inv;                /* true if flag value should be inverted */
        int mask;               /* flag mask value */
};

static const struct opt_map opt_map[] = {
  /*"optname", inv,ms_mask */
  /* These flags are parsed by mount, not lustre */
  { "defaults", 0, 0         },      /* default options */
  { "remount",  0, MS_REMOUNT},      /* remount with different options */
  { "rw",       1, MS_RDONLY },      /* read-write */
  { "ro",       0, MS_RDONLY },      /* read-only */
  { "exec",     1, MS_NOEXEC },      /* permit execution of binaries */
  { "noexec",   0, MS_NOEXEC },      /* don't execute binaries */
  { "suid",     1, MS_NOSUID },      /* honor suid executables */
  { "nosuid",   0, MS_NOSUID },      /* don't honor suid executables */
  { "dev",      1, MS_NODEV  },      /* interpret device files  */
  { "nodev",    0, MS_NODEV  },      /* don't interpret devices */
  { "sync",     0, MS_SYNCHRONOUS},  /* synchronous I/O */
  { "async",    1, MS_SYNCHRONOUS},  /* asynchronous I/O */
  { "atime",    1, MS_NOATIME  },    /* set file access time on read */
  { "noatime",  0, MS_NOATIME  },    /* do not set file access time on read */
#ifdef MS_NODIRATIME
  { "diratime", 1, MS_NODIRATIME },  /* set file access time on read */
  { "nodiratime",0,MS_NODIRATIME },  /* do not set file access time on read */
#endif
#ifdef MS_RELATIME
  { "relatime", 0, MS_RELATIME },  /* set file access time on read */
  { "norelatime",1,MS_RELATIME },  /* do not set file access time on read */
#endif
#ifdef MS_STRICTATIME
  { "strictatime",0,MS_STRICTATIME },  /* update access time strictly */
#endif
  { "auto",     0, 0         },      /* Can be mounted using -a */
  { "noauto",   0, 0         },      /* Can only be mounted explicitly */
  { "nousers",  1, 0         },      /* Forbid ordinary user to mount */
  { "nouser",   1, 0         },      /* Forbid ordinary user to mount */
  { "noowner",  1, 0         },      /* Device owner has no special privs */
  { "_netdev",  0, 0         },      /* Device accessible only via network */
  { "loop",     0, 0         },
  { NULL,       0, 0         }
};
/****************************************************************************/

/* 1  = don't pass on to lustre
   0  = pass on to lustre */
static int parse_one_option(const char *check, int *flagp)
{
        const struct opt_map *opt;

        for (opt = &opt_map[0]; opt->opt != NULL; opt++) {
                if (strncmp(check, opt->opt, strlen(opt->opt)) == 0) {
                        if (opt->mask) {
                                if (opt->inv)
                                        *flagp &= ~(opt->mask);
                                else
                                        *flagp |= opt->mask;
                        }
                        return 1;
                }
        }
        /* Assume any unknown options are valid and pass them on.  The mount
           will fail if lmd_parse, ll_options or ldiskfs doesn't recognize it.*/
        return 0;
}

static void append_option(char *options, const char *one)
{
        if (*options)
                strcat(options, ",");
        strcat(options, one);
}

static void append_mgsnid(struct mount_opts *mop, char *options,
                          const char *val)
{
        char *resolved;

        append_option(options, PARAM_MGSNODE);
        resolved = convert_hostnames((char *)val);
        if (resolved) {
                strcat(options, resolved);
                free(resolved);
        } else {
                strcat(options, val);
        }
        mop->mo_have_mgsnid++;
}

/* Replace options with subset of Lustre-specific options, and
   fill in mount flags */
static int parse_options(struct mount_opts *mop, char *orig_options, int *flagp)
{
        char *options, *opt, *nextopt, *arg, *val;

        options = calloc(MAXOPT, 1);
        *flagp = 0;
        nextopt = orig_options;
        while ((opt = strsep(&nextopt, ","))) {
                if (!*opt)
                        /* empty option */
                        continue;

                /* Handle retries in a slightly different
                 * manner */
                arg = opt;
                val = strchr(opt, '=');
                /* please note that some ldiskfs mount options are also in the
                 * form of param=value. We should pay attention not to remove
                 * those mount options, see bug 22097. */
                if (val && strncmp(arg, "md_stripe_cache_size", 20) == 0) {
                        mop->mo_md_stripe_cache_size = atoi(val + 1);
                } else if (val && strncmp(arg, "retry", 5) == 0) {
                        mop->mo_retry = atoi(val + 1);
                        if (mop->mo_retry > MAX_RETRIES)
                                mop->mo_retry = MAX_RETRIES;
                        else if (mop->mo_retry < 0)
                                mop->mo_retry = 0;
                } else if (val && strncmp(arg, PARAM_MGSNODE,
                                           sizeof(PARAM_MGSNODE) - 1) == 0) {
                        /* mgs*=val (no val for plain "mgs" option) */
                        append_mgsnid(mop, options, val + 1);
                } else if (val && strncmp(arg, "mgssec", 6) == 0) {
                        append_option(options, opt);
                } else if (strcmp(opt, "force") == 0) {
                        //XXX special check for 'force' option
                        mop->mo_force++;
                        printf("force: %d\n", mop->mo_force);
                } else if (parse_one_option(opt, flagp) == 0) {
                        /* pass this on as an option */
                        append_option(options, opt);
                }
        }
#ifdef MS_STRICTATIME
        /* set strictatime to default if NOATIME or RELATIME
         * not given explicit */
        if (!(*flagp & (MS_NOATIME | MS_RELATIME)))
                *flagp |= MS_STRICTATIME;
#endif
        fflush(stdout);
        strcpy(orig_options, options);
        free(options);
        return 0;
}

/* Add mgsnids from ldd params */
static int add_mgsnids(struct mount_opts *mop, char *options,
                       const char *params)
{
        char *ptr = (char *)params;
        char tmp, *sep;

        while ((ptr = strstr(ptr, PARAM_MGSNODE)) != NULL) {
                sep = strchr(ptr, ' ');
                if (sep != NULL) {
                        tmp = *sep;
                        *sep = '\0';
                }
                append_option(options, ptr);
                mop->mo_have_mgsnid++;
                if (sep) {
                        *sep = tmp;
                        ptr = sep;
                } else {
                        break;
                }
        }

        return 0;
}

/* Glean any data we can from the disk */
static int parse_ldd(char *source, struct mount_opts *mop, char *options)
{
        struct lustre_disk_data *ldd = &mop->mo_ldd;
        int rc;

        rc = osd_is_lustre(source, &ldd->ldd_mount_type);
        if (rc == 0) {
                fprintf(stderr, "%s: %s has not been formatted "
                        "with mkfs.lustre\n", progname, source);
                return ENODEV;
        }

        /* We no longer read the MOUNT_DATA_FILE from within Lustre.
         * We only read it here, and convert critical info into mount
         * options. */
        rc = osd_read_ldd(source, ldd);
        if (rc) {
                fprintf(stderr, "%s: %s failed to read permanent mount"
                        " data: %s\n", progname, source, strerror(rc));
                return rc;
        }

        if (ldd->ldd_flags & LDD_F_NEED_INDEX) {
                fprintf(stderr, "%s: %s has no index assigned "
                        "(probably formatted with old mkfs)\n",
                        progname, source);
                return EINVAL;
        }

        if (ldd->ldd_flags & LDD_F_WRITECONF) {
                fprintf(stderr, "%s: writeconf may no longer be specified "
                        "with tunefs.  Use the temporary mount option '-o "
                        "writeconf' instead.\n", progname);
                return EINVAL;
        }

        if (ldd->ldd_flags & LDD_F_UPGRADE14) {
                fprintf(stderr, "%s: we cannot upgrade %s from this (very old) "
                        "Lustre version\n", progname, source);
                return EINVAL;
        }

        /* Since we never rewrite ldd, ignore temp flags */
        ldd->ldd_flags &= ~(LDD_F_VIRGIN | LDD_F_UPDATE);

        /* svname of the form lustre:OST1234 means never registered */
        rc = strlen(ldd->ldd_svname);
        if (ldd->ldd_svname[rc - 8] == ':') {
                ldd->ldd_svname[rc - 8] = '-';
                ldd->ldd_flags |= LDD_F_VIRGIN;
        }

        /* backend osd type */
        append_option(options, "osd=");
        strcat(options, mt_type(ldd->ldd_mount_type));

        append_option(options, ldd->ldd_mount_opts);

        if (!mop->mo_have_mgsnid) {
                /* Only use disk data if mount -o mgsnode=nid wasn't
                 * specified */
                if (ldd->ldd_flags & LDD_F_SV_TYPE_MGS) {
                        append_option(options, "mgs");
                        mop->mo_have_mgsnid++;
                } else {
                        add_mgsnids(mop, options, ldd->ldd_params);
                }
        }
        /* Better have an mgsnid by now */
        if (!mop->mo_have_mgsnid) {
                fprintf(stderr, "%s: missing option mgsnode=<nid>\n",
                        progname);
                return EINVAL;
        }

        if (ldd->ldd_flags & LDD_F_VIRGIN)
                append_option(options, "writeconf");
        if (ldd->ldd_flags & LDD_F_IAM_DIR)
                append_option(options, "iam");
        if (ldd->ldd_flags & LDD_F_NO_PRIMNODE)
                append_option(options, "noprimnode");

        /* svname must be last option */
        append_option(options, "svname=");
        strcat(options, ldd->ldd_svname);

        return 0;
}

static void set_defaults(struct mount_opts *mop)
{
        memset(mop, 0, sizeof(*mop));
        mop->mo_ldd.ldd_magic = LDD_MAGIC;
        mop->mo_ldd.ldd_config_ver = 1;
        mop->mo_ldd.ldd_flags = 0;
        strcpy(mop->mo_ldd.ldd_fsname, "lustre");
        mop->mo_ldd.ldd_mount_type = LDD_MT_LDISKFS;
        mop->mo_usource = NULL;
        mop->mo_source = NULL;
        mop->mo_nomtab = 0;
        mop->mo_fake = 0;
        mop->mo_force = 0;
        mop->mo_retry = 0;
        mop->mo_have_mgsnid = 0;
        mop->mo_md_stripe_cache_size = 16384;
}

static int parse_opts(int argc, char *const argv[], struct mount_opts *mop)
{
        static struct option long_opt[] = {
                {"fake", 0, 0, 'f'},
                {"force", 0, 0, 1},
                {"help", 0, 0, 'h'},
                {"nomtab", 0, 0, 'n'},
                {"options", 1, 0, 'o'},
                {"verbose", 0, 0, 'v'},
                {0, 0, 0, 0}
        };
        char *ptr, *optstring = "fhno:v";
        int opt;
        int rc;

        while ((opt = getopt_long(argc, argv, optstring,
                                  long_opt, NULL)) != EOF){
                switch (opt) {
                case 1:
                        mop->mo_force++;
                        printf("force: %d\n", mop->mo_force);
                        break;
                case 'f':
                        mop->mo_fake++;
                        printf("fake: %d\n", mop->mo_fake);
                        break;
                case 'h':
                        usage(stdout);
                        break;
                case 'n':
                        mop->mo_nomtab++;
                        printf("nomtab: %d\n", mop->mo_nomtab);
                        break;
                case 'o':
                        mop->mo_orig_options = optarg;
                        break;
                case 'v':
                        ++verbose;
                        break;
                default:
                        fprintf(stderr, "%s: unknown option '%c'\n",
                                progname, opt);
                        usage(stderr);
                        break;
                }
        }

        if (optind + 2 > argc) {
                fprintf(stderr, "%s: too few arguments\n", progname);
                usage(stderr);
        }

        mop->mo_usource = argv[optind];
        if (mop->mo_usource == NULL) {
                usage(stderr);
        }

        if ((ptr = devname_is_client(mop->mo_usource)) != NULL) {
                char tmp, *nids;

                /* convert nids part, but not fsname part */
                tmp = *ptr;
                *ptr = '\0';
                nids = convert_hostnames(mop->mo_usource);
                if (!nids)
                        usage(stderr);
                *ptr = tmp;
                mop->mo_source = malloc(strlen(nids) + strlen(ptr) + 1);
                sprintf(mop->mo_source, "%s%s", nids, ptr);
        } else {
                mop->mo_source = strdup(mop->mo_usource);
        }

        if (realpath(argv[optind + 1], mop->mo_target) == NULL) {
                rc = errno;
                fprintf(stderr, "warning: %s: cannot resolve: %s\n",
                        argv[optind + 1], strerror(errno));
                return rc;
        }

        return 0;
}

int main(int argc, char *const argv[])
{
        struct mount_opts mop;
        char *options = NULL;
        char cmd[256];
        int i, rc, flags;

        progname = strrchr(argv[0], '/');
        progname = progname ? progname + 1 : argv[0];

        set_defaults(&mop);

        rc = osd_init();
        if (rc)
                return rc;

        rc = parse_opts(argc, argv, &mop);
        if (rc)
                return rc;

        if (verbose) {
                for (i = 0; i < argc; i++)
                        printf("arg[%d] = %s\n", i, argv[i]);
                printf("source = %s (%s)\n", mop.mo_usource, mop.mo_source);
                printf("target = %s\n", mop.mo_target);
                printf("options = %s\n", mop.mo_orig_options);
        }

        options = malloc(MAXOPT);
        if (options == NULL) {
                fprintf(stderr, "can't allocate memory for options\n");
                rc = ENOMEM;
                goto out;
        }

        strcpy(options, mop.mo_orig_options);
        rc = parse_options(&mop, options, &flags);
        if (rc) {
                fprintf(stderr, "%s: can't parse options: %s\n",
                        progname, options);
                rc = EINVAL;
                goto out;
        }

        if (!mop.mo_force) {
                rc = check_mtab_entry(mop.mo_usource, mop.mo_source,
                                      mop.mo_target, "lustre");
                if (rc && !(flags & MS_REMOUNT)) {
                        fprintf(stderr, "%s: according to %s %s is "
                                "already mounted on %s\n", progname,
                                MOUNTED, mop.mo_usource, mop.mo_target);
                        rc = EEXIST;
                        goto out;
                }
                if (!rc && (flags & MS_REMOUNT)) {
                        fprintf(stderr, "%s: according to %s %s is "
                                "not already mounted on %s\n", progname,
                                MOUNTED, mop.mo_usource, mop.mo_target);
                        rc = ENOENT;
                        goto out;
                }
        }

        if (flags & MS_REMOUNT)
                mop.mo_nomtab++;

        /* Make sure we have a mount point */
        rc = access(mop.mo_target, F_OK);
        if (rc) {
                rc = errno;
                fprintf(stderr, "%s: %s inaccessible: %s\n", progname,
                        mop.mo_target, strerror(errno));
                goto out;
        }

        if (!devname_is_client(mop.mo_usource)) {
                rc = parse_ldd(mop.mo_source, &mop, options);
                if (rc)
                        goto out;
        }

        /* In Linux 2.4, the target device doesn't get passed to any of our
           functions.  So we'll stick it on the end of the options. */
        append_option(options, "device=");
        strcat(options, mop.mo_source);

        if (verbose)
                printf("mounting device %s at %s, flags=%#x options=%s\n",
                       mop.mo_source, mop.mo_target, flags, options);

        if (!devname_is_client(mop.mo_usource) &&
            osd_tune_lustre(mop.mo_source, &mop)) {
                if (verbose)
                        fprintf(stderr, "%s: unable to set tunables for %s"
                                " (may cause reduced IO performance)\n",
                                argv[0], mop.mo_source);
        }

        if (!mop.mo_fake) {
                /* flags and target get to lustre_get_sb, but not
                   lustre_fill_super.  Lustre ignores the flags, but mount
                   does not. */
                for (i = 0, rc = -EAGAIN; i <= mop.mo_retry; i++) {
                        rc = mount(mop.mo_source, mop.mo_target, "lustre",
                                   flags, (void *)options);
                        if ((rc == 0) || (mop.mo_retry == 0))
                                break;

                        if (verbose) {
                                fprintf(stderr, "%s: mount %s at %s "
                                        "failed: %s retries left: %d\n",
                                        basename(progname), mop.mo_usource,
                                        mop.mo_target, strerror(errno),
                                        mop.mo_retry-i);
                        }

                        sleep(1 << max((i/2), 5));
                }
        }

        if (rc == 0) {
                if (!mop.mo_nomtab)
                        rc = update_mtab_entry(mop.mo_usource, mop.mo_target,
                                               "lustre", mop.mo_orig_options,
                                               0,0,0);

                /* change label from <fsname>:<index> to <fsname>-<index>
                 * to indicate the device has been registered. */
                if (mop.mo_ldd.ldd_flags & LDD_F_VIRGIN)
                        (void) osd_label_lustre(&mop);
        } else {
                char *cli;

                rc = errno;

                cli = strrchr(mop.mo_usource, ':');
                if (cli && (strlen(cli) > 2))
                        cli += 2;
                else
                        cli = NULL;

                fprintf(stderr, "%s: mount %s at %s failed: %s\n", progname,
                        mop.mo_usource, mop.mo_target, strerror(errno));
                if (errno == ENODEV)
                        fprintf(stderr, "Are the lustre modules loaded?\n"
                                "Check /etc/modprobe.conf and "
                                "/proc/filesystems\n");
                if (errno == ENOTBLK)
                        fprintf(stderr, "Do you need -o loop?\n");
                if (errno == ENOMEDIUM)
                        fprintf(stderr,
                                "This filesystem needs at least 1 OST\n");
                if (errno == ENOENT) {
                        fprintf(stderr, "Is the MGS specification correct?\n");
                        fprintf(stderr, "Is the filesystem name correct?\n");
                        fprintf(stderr, "If upgrading, is the copied client log"
                                " valid? (see upgrade docs)\n");
                }
                if (errno == EALREADY)
                        fprintf(stderr, "The target service is already running."
                                " (%s)\n", mop.mo_usource);
                if (errno == ENXIO)
                        fprintf(stderr, "The target service failed to start "
                                "(needs -o writeconf?  bad config log?) (%s)."
                                "See /var/log/messages.\n", mop.mo_usource);
                if (errno == EIO)
                        fprintf(stderr, "Is the MGS running?\n");
                if (errno == EADDRINUSE)
                        fprintf(stderr, "The target service's index is already "
                                "in use. (%s)\n", mop.mo_usource);
                if (errno == EINVAL) {
                        fprintf(stderr, "This may have multiple causes.\n");
                        if (cli)
                                fprintf(stderr, "Is '%s' the correct filesystem"
                                        " name?\n", cli);
                        fprintf(stderr, "Are the mount options correct?\n");
                        fprintf(stderr, "Check the syslog for more info.\n");
                }

                /* May as well try to clean up loop devs */
                if (strncmp(mop.mo_usource, "/dev/loop", 9) == 0) {
                        sprintf(cmd, "/sbin/losetup -d %s", mop.mo_usource);
                        rc = run_command(cmd, sizeof(cmd));
                }
        }

out:
        if (options)
                free(options);

        if (mop.mo_source)
                free(mop.mo_source);

        osd_fini();

        return rc;
}
