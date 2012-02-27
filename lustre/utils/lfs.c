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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/utils/lfs.c
 *
 * Author: Peter J. Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Robert Read <rread@clusterfs.com>
 */

/* for O_DIRECTORY */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#ifdef HAVE_SYS_QUOTA_H
# include <sys/quota.h>
#endif

/* For dirname() */
#include <libgen.h>

#include <lnet/api-support.h>
#include <lnet/lnetctl.h>

#include <liblustre.h>
#include <lustre/lustre_idl.h>
#include <lustre/liblustreapi.h>
#include <lustre/lustre_user.h>
#include <lustre_quota.h>

#include <libcfs/libcfsutil.h>
#include "obdctl.h"

unsigned int libcfs_subsystem_debug = 0;

/* all functions */
static int lfs_setstripe(int argc, char **argv);
static int lfs_find(int argc, char **argv);
static int lfs_getstripe(int argc, char **argv);
static int lfs_osts(int argc, char **argv);
static int lfs_df(int argc, char **argv);
static int lfs_getname(int argc, char **argv);
static int lfs_check(int argc, char **argv);
static int lfs_catinfo(int argc, char **argv);
#ifdef HAVE_SYS_QUOTA_H
static int lfs_quotachown(int argc, char **argv);
static int lfs_quotacheck(int argc, char **argv);
static int lfs_quotaon(int argc, char **argv);
static int lfs_quotaoff(int argc, char **argv);
static int lfs_setquota(int argc, char **argv);
static int lfs_quota(int argc, char **argv);
static int lfs_quotainv(int argc, char **argv);
#endif
static int lfs_flushctx(int argc, char **argv);
static int lfs_join(int argc, char **argv);
static int lfs_lsetfacl(int argc, char **argv);
static int lfs_lgetfacl(int argc, char **argv);
static int lfs_rsetfacl(int argc, char **argv);
static int lfs_rgetfacl(int argc, char **argv);
static int lfs_cp(int argc, char **argv);
static int lfs_ls(int argc, char **argv);
static int lfs_poollist(int argc, char **argv);
static int lfs_changelog(int argc, char **argv);
static int lfs_changelog_clear(int argc, char **argv);
static int lfs_fid2path(int argc, char **argv);
static int lfs_path2fid(int argc, char **argv);

/* all avaialable commands */
command_t cmdlist[] = {
        {"setstripe", lfs_setstripe, 0,
         "Create a new file with a specific striping pattern or\n"
         "set the default striping pattern on an existing directory or\n"
         "delete the default striping pattern from an existing directory\n"
         "usage: setstripe [--size|-s stripe_size] [--count|-c stripe_count]\n"
         "                 [--index|-i|--offset|-o start_ost_index]\n"
         "                 [--pool|-p <pool>] <directory|filename>\n"
         "       or \n"
         "       setstripe -d <directory>   (to delete default striping)\n"
         "\tstripe_size:  Number of bytes on each OST (0 filesystem default)\n"
         "\t              Can be specified with k, m or g (in KB, MB and GB\n"
         "\t              respectively)\n"
         "\tstart_ost_index: OST index of first stripe (-1 default)\n"
         "\tstripe_count: Number of OSTs to stripe over (0 default, -1 all)\n"
         "\tpool:         Name of OST pool to use (default none)"},
        {"getstripe", lfs_getstripe, 0,
         "To list the striping info for a given file or files in a\n"
         "directory or recursively for all files in a directory tree.\n"
         "usage: getstripe [--obd|-O <uuid>] [--quiet | -q] [--verbose | -v]\n"
         "                 [--count | -c ] [--index | -i | --offset | -o]\n"
         "                 [--size | -s ] [--pool | -p ] [--directory | -d]\n"
         "                 [--mdt | -M] [--recursive | -r] [--raw | -R]\n"
         "                 <directory|filename> ..."},
        {"pool_list", lfs_poollist, 0,
         "List pools or pool OSTs\n"
         "usage: pool_list <fsname>[.<pool>] | <pathname>\n"},
        {"find", lfs_find, 0,
         "To find files that match given parameters recursively in a directory tree.\n"
         "usage: find <directory|filename> ...\n"
         "     [[!] --atime|-A [+-]N] [[!] --mtime|-M [+-]N] [[!] --ctime|-C [+-]N]\n"
         "     [--maxdepth|-D N] [[!] --name|-n <pattern>] [--print0|-P]\n"
         "     [--print|-p] [[!] --obd|-O <uuid[s]>] [[!] --size|-s [+-]N[bkMGTP]]\n"
         "     [[!] --type|-t <filetype>] [[!] --gid|-g|--group|-G <gid>|<gname>]\n"
         "     [[!] --uid|-u|--user|-U <uid>|<uname>]\n"
         "     [[!] --pool <pool>]\n"
         "\t !: used before an option indicates 'NOT' the requested attribute\n"
         "\t -: used before an value indicates 'AT MOST' the requested value\n"
         "\t +: used before an option indicates 'AT LEAST' the requested value\n"},
        {"check", lfs_check, 0,
         "Display the status of MDS or OSTs (as specified in the command)\n"
         "or all the servers (MDS and OSTs).\n"
         "usage: check <osts|mds|servers>"},
        {"catinfo", lfs_catinfo, 0,
         "Show information of specified type logs.\n"
         "usage: catinfo {keyword} [node name]\n"
         "\tkeywords are one of followings: config, deletions.\n"
         "\tnode name must be provided when use keyword config."},
        {"join", lfs_join, 0,
         "join two lustre files into one.\n"
         "obsolete, HEAD does not support it anymore.\n"},
        {"osts", lfs_osts, 0, "list OSTs connected to client "
         "[for specified path only]\n" "usage: osts [path]"},
        {"df", lfs_df, 0,
         "report filesystem disk space usage or inodes usage"
         "of each MDS and all OSDs or a batch belonging to a specific pool .\n"
         "Usage: df [-i] [-h] [--pool|-p <fsname>[.<pool>] [path]"},
        {"getname", lfs_getname, 0, "list instances and specified mount points "
         "[for specified path only]\n"
         "Usage: getname [-h]|[path ...] "},
#ifdef HAVE_SYS_QUOTA_H
        {"quotachown",lfs_quotachown, 0,
         "Change files' owner or group on the specified filesystem.\n"
         "usage: quotachown [-i] <filesystem>\n"
         "\t-i: ignore error if file is not exist\n"},
        {"quotacheck", lfs_quotacheck, 0,
         "Scan the specified filesystem for disk usage, and create,\n"
         "or update quota files.\n"
         "usage: quotacheck [ -ug ] <filesystem>"},
        {"quotaon", lfs_quotaon, 0, "Turn filesystem quotas on.\n"
         "usage: quotaon [ -ugf ] <filesystem>"},
        {"quotaoff", lfs_quotaoff, 0, "Turn filesystem quotas off.\n"
         "usage: quotaoff [ -ug ] <filesystem>"},
        {"setquota", lfs_setquota, 0, "Set filesystem quotas.\n"
         "usage: setquota <-u|-g> <uname>|<uid>|<gname>|<gid>\n"
         "                -b <block-softlimit> -B <block-hardlimit>\n"
         "                -i <inode-softlimit> -I <inode-hardlimit> <filesystem>\n"
         "       setquota -t <-u|-g> <block-grace> <inode-grace> <filesystem>\n"
         "       setquota <-u|--user|-g|--group> <uname>|<uid>|<gname>|<gid>\n"
         "                [--block-softlimit <block-softlimit>]\n"
         "                [--block-hardlimit <block-hardlimit>]\n"
         "                [--inode-softlimit <inode-softlimit>]\n"
         "                [--inode-hardlimit <inode-hardlimit>] <filesystem>\n"
         "       setquota [-t] <-u|--user|-g|--group>\n"
         "                [--block-grace <block-grace>]\n"
         "                [--inode-grace <inode-grace>] <filesystem>\n"
         "       -b can be used instead of --block-softlimit/--block-grace\n"
         "       -B can be used instead of --block-hardlimit\n"
         "       -i can be used instead of --inode-softlimit/--inode-grace\n"
         "       -I can be used instead of --inode-hardlimit"},
        {"quota", lfs_quota, 0, "Display disk usage and limits.\n"
         "usage: quota [-q] [-v] [-o <obd_uuid>|-i <mdt_idx>|-I <ost_idx>]\n"
         "             [<-u|-g> <uname>|<uid>|<gname>|<gid>] <filesystem>\n"
         "       quota [-o <obd_uuid>|-i <mdt_idx>|-I <ost_idx>] -t <-u|-g> <filesystem>"},
        {"quotainv", lfs_quotainv, 0, "Invalidate quota data.\n"
         "usage: quotainv [-u|-g] <filesystem>"},
#endif
        {"flushctx", lfs_flushctx, 0, "Flush security context for current user.\n"
         "usage: flushctx [-k] [mountpoint...]"},
        {"lsetfacl", lfs_lsetfacl, 0,
         "Remote user setfacl for user/group on the same remote client.\n"
         "usage: lsetfacl [-bkndRLPvh] [{-m|-x} acl_spec] [{-M|-X} acl_file] file ..."},
        {"lgetfacl", lfs_lgetfacl, 0,
         "Remote user getfacl for user/group on the same remote client.\n"
         "usage: lgetfacl [-dRLPvh] file ..."},
        {"rsetfacl", lfs_rsetfacl, 0,
         "Remote user setfacl for user/group on other clients.\n"
         "usage: rsetfacl [-bkndRLPvh] [{-m|-x} acl_spec] [{-M|-X} acl_file] file ..."},
        {"rgetfacl", lfs_rgetfacl, 0,
         "Remote user getfacl for user/group on other clients.\n"
         "usage: rgetfacl [-dRLPvh] file ..."},
        {"cp", lfs_cp, 0,
         "Remote user copy files and directories.\n"
         "usage: cp [OPTION]... [-T] SOURCE DEST\n\tcp [OPTION]... SOURCE... DIRECTORY\n\tcp [OPTION]... -t DIRECTORY SOURCE..."},
        {"ls", lfs_ls, 0,
         "Remote user list directory contents.\n"
         "usage: ls [OPTION]... [FILE]..."},
        {"changelog", lfs_changelog, 0,
         "Show the metadata changes on an MDT."
         "\nusage: changelog <mdtname> [startrec [endrec]]"},
        {"changelog_clear", lfs_changelog_clear, 0,
         "Indicate that old changelog records up to <endrec> are no longer of "
         "interest to consumer <id>, allowing the system to free up space.\n"
         "An <endrec> of 0 means all records.\n"
         "usage: changelog_clear <mdtname> <id> <endrec>"},
        {"fid2path", lfs_fid2path, 0,
         "Resolve the full path to a given FID. For a specific hardlink "
         "specify link number <linkno>.\n"
         /* "For a historical name, specify changelog record <recno>.\n" */
         "usage: fid2path <fsname|rootpath> <fid> [--link <linkno>]"
                /*[--rec <recno>]*/},
        {"path2fid", lfs_path2fid, 0, "Display the fid for a given path.\n"
         "usage: path2fid <path>"},
        {"help", Parser_help, 0, "help"},
        {"exit", Parser_quit, 0, "quit"},
        {"quit", Parser_quit, 0, "quit"},
        { 0, 0, 0, NULL }
};

static int isnumber(const char *str)
{
        const char *ptr;

        if (str[0] != '-' && !isdigit(str[0]))
                return 0;

        for (ptr = str + 1; *ptr != '\0'; ptr++) {
                if (!isdigit(*ptr))
                        return 0;
        }

        return 1;
}

/* functions */
static int lfs_setstripe(int argc, char **argv)
{
        char *fname;
        int result;
        unsigned long long st_size;
        int  st_offset, st_count;
        char *end;
        int c;
        int delete = 0;
        char *stripe_size_arg = NULL;
        char *stripe_off_arg = NULL;
        char *stripe_count_arg = NULL;
        char *pool_name_arg = NULL;
        unsigned long long size_units;

        struct option long_opts[] = {
                {"count",       required_argument, 0, 'c'},
                {"delete",      no_argument,       0, 'd'},
                {"index",       required_argument, 0, 'i'},
                {"offset",      required_argument, 0, 'o'},
                {"pool",        required_argument, 0, 'p'},
                {"size",        required_argument, 0, 's'},
                {0, 0, 0, 0}
        };

        st_size = 0;
        st_offset = -1;
        st_count = 0;

#if LUSTRE_VERSION < OBD_OCD_VERSION(2,4,50,0)
        if (argc == 5 && argv[1][0] != '-' &&
            isnumber(argv[2]) && isnumber(argv[3]) && isnumber(argv[4])) {
                fprintf(stderr, "error: obsolete usage of setstripe "
                        "positional parameters.  Use -c, -i, -s instead.\n");
                return CMD_HELP;
        } else
#else
#warning "remove obsolete positional parameter code"
#endif
        {
                optind = 0;
                while ((c = getopt_long(argc, argv, "c:di:o:p:s:",
                                        long_opts, NULL)) >= 0) {
                        switch (c) {
                        case 0:
                                /* Long options. */
                                break;
                        case 'c':
                                stripe_count_arg = optarg;
                                break;
                        case 'd':
                                /* delete the default striping pattern */
                                delete = 1;
                                break;
                        case 'i':
                        case 'o':
                                stripe_off_arg = optarg;
                                break;
                        case 's':
                                stripe_size_arg = optarg;
                                break;
                        case 'p':
                                pool_name_arg = optarg;
                                break;
                        case '?':
                                return CMD_HELP;
                        default:
                                fprintf(stderr, "error: %s: option '%s' "
                                                "unrecognized\n",
                                                argv[0], argv[optind - 1]);
                                return CMD_HELP;
                        }
                }

                fname = argv[optind];

                if (delete &&
                    (stripe_size_arg != NULL || stripe_off_arg != NULL ||
                     stripe_count_arg != NULL || pool_name_arg != NULL)) {
                        fprintf(stderr, "error: %s: cannot specify -d with "
                                        "-s, -c -o or -p options\n",
                                        argv[0]);
                        return CMD_HELP;
                }
        }

        if (optind == argc) {
                fprintf(stderr, "error: %s: missing filename|dirname\n",
                        argv[0]);
                return CMD_HELP;
        }

        /* get the stripe size */
        if (stripe_size_arg != NULL) {
                result = parse_size(stripe_size_arg, &st_size, &size_units, 0);
                if (result) {
                        fprintf(stderr, "error: %s: bad size '%s'\n",
                                argv[0], stripe_size_arg);
                        return result;
                }
        }
        /* get the stripe offset */
        if (stripe_off_arg != NULL) {
                st_offset = strtol(stripe_off_arg, &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe offset '%s'\n",
                                argv[0], stripe_off_arg);
                        return CMD_HELP;
                }
        }
        /* get the stripe count */
        if (stripe_count_arg != NULL) {
                st_count = strtoul(stripe_count_arg, &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe count '%s'\n",
                                argv[0], stripe_count_arg);
                        return CMD_HELP;
                }
        }

        do {
                result = llapi_file_create_pool(fname, st_size, st_offset,
                                                st_count, 0, pool_name_arg);
                if (result) {
                        fprintf(stderr,"error: %s: create stripe file '%s' "
                                "failed\n", argv[0], fname);
                        break;
                }
                fname = argv[++optind];
        } while (fname != NULL);

        return result;
}

static int lfs_poollist(int argc, char **argv)
{
        if (argc != 2)
                return CMD_HELP;

        return llapi_poollist(argv[1]);
}

static int set_time(time_t *time, time_t *set, char *str)
{
        time_t t;
        int res = 0;

        if (str[0] == '+')
                res = 1;
        else if (str[0] == '-')
                res = -1;

        if (res)
                str++;

        t = strtol(str, NULL, 0);
        if (*time < t * 24 * 60 * 60) {
                if (res)
                        str--;
                fprintf(stderr, "Wrong time '%s' is specified.\n", str);
                return INT_MAX;
        }

        *set = *time - t * 24 * 60 * 60;
        return res;
}

#define USER 0
#define GROUP 1

static int name2id(unsigned int *id, char *name, int type)
{
        if (type == USER) {
                struct passwd *entry;

                if (!(entry = getpwnam(name))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *id = entry->pw_uid;
        } else {
                struct group *entry;

                if (!(entry = getgrnam(name))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *id = entry->gr_gid;
        }

        return 0;
}

static int id2name(char **name, unsigned int id, int type)
{
        if (type == USER) {
                struct passwd *entry;

                if (!(entry = getpwuid(id))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *name = entry->pw_name;
        } else {
                struct group *entry;

                if (!(entry = getgrgid(id))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *name = entry->gr_name;
        }

        return 0;
}

#define FIND_POOL_OPT 3
static int lfs_find(int argc, char **argv)
{
        int new_fashion = 1;
        int c, ret;
        time_t t;
        struct find_param param = { .maxdepth = -1 };
        struct option long_opts[] = {
                {"atime",     required_argument, 0, 'A'},
                {"ctime",     required_argument, 0, 'C'},
                {"maxdepth",  required_argument, 0, 'D'},
                {"gid",       required_argument, 0, 'g'},
                {"group",     required_argument, 0, 'G'},
                {"mtime",     required_argument, 0, 'M'},
                {"name",      required_argument, 0, 'n'},
                /* --obd is considered as a new option. */
                {"obd",       required_argument, 0, 'O'},
                {"ost",       required_argument, 0, 'O'},
                /* no short option for pool, p/P already used */
                {"pool",      required_argument, 0, FIND_POOL_OPT},
                {"print0",    no_argument,       0, 'p'},
                {"print",     no_argument,       0, 'P'},
                {"quiet",     no_argument,       0, 'q'},
                {"recursive", no_argument,       0, 'r'},
                {"size",      required_argument, 0, 's'},
                {"type",      required_argument, 0, 't'},
                {"uid",       required_argument, 0, 'u'},
                {"user",      required_argument, 0, 'U'},
                {"verbose",   no_argument,       0, 'v'},
                {0, 0, 0, 0}
        };
        int pathstart = -1;
        int pathend = -1;
        int neg_opt = 0;
        time_t *xtime;
        int *xsign;
        int isoption;
        char *endptr;

        time(&t);

        optind = 0;
        /* when getopt_long_only() hits '!' it returns 1, puts "!" in optarg */
        while ((c = getopt_long_only(argc,argv,"-A:C:D:g:G:M:n:O:Ppqrs:t:u:U:v",
                                     long_opts, NULL)) >= 0) {
                xtime = NULL;
                xsign = NULL;
                if (neg_opt)
                        --neg_opt;
                /* '!' is part of option */
                /* when getopt_long_only() finds a string which is not
                 * an option nor a known option argument it returns 1
                 * in that case if we already have found pathstart and pathend
                 * (i.e. we have the list of pathnames),
                 * the only supported value is "!"
                 */
                isoption = (c != 1) || (strcmp(optarg, "!") == 0);
                if (!isoption && pathend != -1) {
                        fprintf(stderr, "err: %s: filename|dirname must either "
                                        "precede options or follow options\n",
                                        argv[0]);
                        return CMD_HELP;
                }
                if (!isoption && pathstart == -1)
                        pathstart = optind - 1;
                if (isoption && pathstart != -1 && pathend == -1) {
                        pathend = optind - 2;
                        if ((c == 1 && strcmp(optarg, "!") == 0) ||
                            c == 'P' || c == 'p' || c == 'O' ||
                            c == 'q' || c == 'r' || c == 'v')
                                pathend = optind - 1;
                }
                switch (c) {
                case 0:
                        /* Long options. */
                        break;
                case 1:
                        /* unknown; opt is "!" or path component,
                         * checking done above.
                         */
                        if (strcmp(optarg, "!") == 0)
                                neg_opt = 2;
                        break;
                case 'A':
                        xtime = &param.atime;
                        xsign = &param.asign;
                        param.exclude_atime = !!neg_opt;
                case 'C':
                        if (c == 'C') {
                                xtime = &param.ctime;
                                xsign = &param.csign;
                                param.exclude_ctime = !!neg_opt;
                        }
                case 'M':
                        if (c == 'M') {
                                xtime = &param.mtime;
                                xsign = &param.msign;
                                param.exclude_mtime = !!neg_opt;
                        }
                        new_fashion = 1;
                        ret = set_time(&t, xtime, optarg);
                        if (ret == INT_MAX)
                                return -1;
                        if (ret)
                                *xsign = ret;
                        break;
                case 'D':
                        new_fashion = 1;
                        param.maxdepth = strtol(optarg, 0, 0);
                        break;
                case 'g':
                case 'G':
                        new_fashion = 1;
                        ret = name2id(&param.gid, optarg, GROUP);
                        if (ret) {
                                param.gid = strtoul(optarg, &endptr, 10);
                                if (*endptr != '\0') {
                                        fprintf(stderr, "Group/GID: %s cannot "
                                                "be found.\n", optarg);
                                        return -1;
                                }
                        }
                        param.exclude_gid = !!neg_opt;
                        param.check_gid = 1;
                        break;
                case 'u':
                case 'U':
                        new_fashion = 1;
                        ret = name2id(&param.uid, optarg, USER);
                        if (ret) {
                                param.uid = strtoul(optarg, &endptr, 10);
                                if (*endptr != '\0') {
                                        fprintf(stderr, "User/UID: %s cannot "
                                                "be found.\n", optarg);
                                        return -1;
                                }
                        }
                        param.exclude_uid = !!neg_opt;
                        param.check_uid = 1;
                        break;
                case FIND_POOL_OPT:
                        new_fashion = 1;
                        if (strlen(optarg) > LOV_MAXPOOLNAME) {
                                fprintf(stderr,
                                        "Pool name %s is too long"
                                        " (max is %d)\n", optarg,
                                        LOV_MAXPOOLNAME);
                                return -1;
                        }
                        /* we do check for empty pool because empty pool
                         * is used to find V1 lov attributes */
                        strncpy(param.poolname, optarg, LOV_MAXPOOLNAME);
                        param.poolname[LOV_MAXPOOLNAME] = '\0';
                        param.exclude_pool = !!neg_opt;
                        param.check_pool = 1;
                        break;
                case 'n':
                        new_fashion = 1;
                        param.pattern = (char *)optarg;
                        param.exclude_pattern = !!neg_opt;
                        break;
                case 'O': {
                        char *buf, *token, *next, *p;
                        int len;

                        len = strlen((char *)optarg);
                        buf = malloc(len+1);
                        if (buf == NULL)
                                return -ENOMEM;
                        strcpy(buf, (char *)optarg);

                        param.exclude_obd = !!neg_opt;

                        if (param.num_alloc_obds == 0) {
                                param.obduuid = malloc(FIND_MAX_OSTS *
                                                       sizeof(struct obd_uuid));
                                if (param.obduuid == NULL)
                                        return -ENOMEM;
                                param.num_alloc_obds = INIT_ALLOC_NUM_OSTS;
                        }

                        for (token = buf; token && *token; token = next) {
                                p = strchr(token, ',');
                                next = 0;
                                if (p) {
                                        *p = 0;
                                        next = p+1;
                                }
                                strcpy((char *)&param.obduuid[param.num_obds++].uuid,
                                       token);
                        }

                        if (buf)
                                free(buf);
                        break;
                }
                case 'p':
                        new_fashion = 1;
                        param.zeroend = 1;
                        break;
                case 'P':
                        break;
                case 'q':
                        new_fashion = 0;
                        param.quiet++;
                        param.verbose = 0;
                        break;
                case 'r':
                        new_fashion = 0;
                        param.recursive = 1;
                        break;
                case 't':
                        param.exclude_type = !!neg_opt;
                        switch(optarg[0]) {
                        case 'b': param.type = S_IFBLK; break;
                        case 'c': param.type = S_IFCHR; break;
                        case 'd': param.type = S_IFDIR; break;
                        case 'f': param.type = S_IFREG; break;
                        case 'l': param.type = S_IFLNK; break;
                        case 'p': param.type = S_IFIFO; break;
                        case 's': param.type = S_IFSOCK; break;
#ifdef S_IFDOOR /* Solaris only */
                        case 'D': param.type = S_IFDOOR; break;
#endif
                        default: fprintf(stderr, "error: %s: bad type '%s'\n",
                                         argv[0], optarg);
                                 return CMD_HELP;
                        };
                        break;
                case 's':
                        if (optarg[0] == '+')
                                param.size_sign = -1;
                        else if (optarg[0] == '-')
                                param.size_sign = +1;

                        if (param.size_sign)
                                optarg++;
                        ret = parse_size(optarg, &param.size,
                                         &param.size_units, 0);
                        if (ret) {
                                fprintf(stderr,"error: bad size '%s'\n",
                                        optarg);
                                return ret;
                        }
                        param.check_size = 1;
                        param.exclude_size = !!neg_opt;
                        break;
                case 'v':
                        new_fashion = 0;
                        param.verbose++;
                        param.quiet = 0;
                        break;
                case '?':
                        return CMD_HELP;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                };
        }

        if (pathstart == -1) {
                fprintf(stderr, "error: %s: no filename|pathname\n",
                        argv[0]);
                return CMD_HELP;
        } else if (pathend == -1) {
                /* no options */
                pathend = argc;
        }

        if (new_fashion) {
                param.quiet = 1;
        } else {
                static int deprecated_warning;
                if (!deprecated_warning) {
                        fprintf(stderr, "lfs find: -q, -r, -v options "
                                "deprecated.  Use 'lfs getstripe' instead.\n");
                        deprecated_warning = 1;
                }
                if (!param.recursive && param.maxdepth == -1)
                        param.maxdepth = 1;
        }

        do {
                if (new_fashion)
                        ret = llapi_find(argv[pathstart], &param);
                else
                        ret = llapi_getstripe(argv[pathstart], &param);
        } while (++pathstart < pathend && !ret);

        if (ret)
                fprintf(stderr, "error: %s failed for %s.\n",
                        argv[0], argv[optind - 1]);

        if (param.obduuid && param.num_alloc_obds)
                free(param.obduuid);

        return ret;
}

static int lfs_getstripe(int argc, char **argv)
{
        struct option long_opts[] = {
                {"count", 0, 0, 'c'},
                {"directory", 0, 0, 'd'},
                {"index", 0, 0, 'i'},
                {"mdt", 0, 0, 'M'},
                {"offset", 0, 0, 'o'},
                {"obd", 1, 0, 'O'},
                {"pool", 0, 0, 'p'},
                {"quiet", 0, 0, 'q'},
                {"recursive", 0, 0, 'r'},
                {"raw", 0, 0, 'R'},
                {"size", 0, 0, 's'},
                {"verbose", 0, 0, 'v'},
                {0, 0, 0, 0}
        };
        int c, rc;
        struct find_param param = { 0 };

        param.maxdepth = 1;
        optind = 0;
        while ((c = getopt_long(argc, argv, "cdhiMoO:pqrRsv",
                                long_opts, NULL)) != -1) {
                switch (c) {
                case 'O':
                        if (param.obduuid) {
                                fprintf(stderr,
                                        "error: %s: only one obduuid allowed",
                                        argv[0]);
                                return CMD_HELP;
                        }
                        param.obduuid = (struct obd_uuid *)optarg;
                        break;
                case 'q':
                        param.quiet++;
                        break;
                case 'd':
                        param.maxdepth = 0;
                        break;
                case 'r':
                        param.recursive = 1;
                        break;
                case 'v':
                        param.verbose = VERBOSE_ALL | VERBOSE_DETAIL;
                        break;
                case 'c':
                        if (!(param.verbose & VERBOSE_DETAIL)) {
                                param.verbose |= VERBOSE_COUNT;
                                param.maxdepth = 0;
                        }
                        break;
                case 's':
                        if (!(param.verbose & VERBOSE_DETAIL)) {
                                param.verbose |= VERBOSE_SIZE;
                                param.maxdepth = 0;
                        }
                        break;
                case 'i':
                case 'o':
                        if (!(param.verbose & VERBOSE_DETAIL)) {
                                param.verbose |= VERBOSE_OFFSET;
                                param.maxdepth = 0;
                        }
                        break;
                case 'p':
                        if (!(param.verbose & VERBOSE_DETAIL)) {
                                param.verbose |= VERBOSE_POOL;
                                param.maxdepth = 0;
                        }
                        break;
                case 'M':
                        param.get_mdt_index = 1;
                        break;
                case 'R':
                        param.raw = 1;
                        break;
                case '?':
                        return CMD_HELP;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                }
        }

        if (optind >= argc)
                return CMD_HELP;

        if (param.recursive)
                param.maxdepth = -1;

        if (!param.verbose)
                param.verbose = VERBOSE_ALL;
        if (param.quiet)
                param.verbose = VERBOSE_OBJID;

        do {
                rc = llapi_getstripe(argv[optind], &param);
        } while (++optind < argc && !rc);

        if (rc)
                fprintf(stderr, "error: %s failed for %s.\n",
                        argv[0], argv[optind - 1]);
        return rc;
}

static int lfs_osts(int argc, char **argv)
{
        char mntdir[PATH_MAX] = {'\0'}, path[PATH_MAX] = {'\0'};
        struct find_param param;
        int index = 0, rc=0;

        if (argc > 2)
                return CMD_HELP;

        if (argc == 2 && !realpath(argv[1], path)) {
                rc = -errno;
                fprintf(stderr, "error: invalid path '%s': %s\n",
                        argv[1], strerror(-rc));
                return rc;
        }

        while (!llapi_search_mounts(path, index++, mntdir, NULL)) {
                /* Check if we have a mount point */
                if (mntdir[0] == '\0')
                        continue;

                memset(&param, 0, sizeof(param));
                rc = llapi_ostlist(mntdir, &param);
                if (rc) {
                        fprintf(stderr, "error: %s: failed on %s\n",
                                argv[0], mntdir);
                }
                if (path[0] != '\0')
                        break;
                memset(mntdir, 0, PATH_MAX);
        }

        return rc;
}

#define COOK(value)                                                     \
({                                                                      \
        int radix = 0;                                                  \
        while (value > 1024) {                                          \
                value /= 1024;                                          \
                radix++;                                                \
        }                                                               \
        radix;                                                          \
})
#define UUF     "%-20s"
#define CSF     "%11s"
#define CDF     "%11llu"
#define HDF     "%8.1f%c"
#define RSF     "%4s"
#define RDF     "%3d%%"

static int showdf(char *mntdir, struct obd_statfs *stat,
                  char *uuid, int ishow, int cooked,
                  char *type, int index, int rc)
{
        long long avail, used, total;
        double ratio = 0;
        char *suffix = "KMGTPEZY";
        /* Note if we have >2^64 bytes/fs these buffers will need to be grown */
        char tbuf[20], ubuf[20], abuf[20], rbuf[20];

        if (!uuid || !stat)
                return -EINVAL;

        switch (rc) {
        case 0:
                if (ishow) {
                        avail = stat->os_ffree;
                        used = stat->os_files - stat->os_ffree;
                        total = stat->os_files;
                } else {
                        int shift = cooked ? 0 : 10;

                        avail = (stat->os_bavail * stat->os_bsize) >> shift;
                        used  = ((stat->os_blocks - stat->os_bfree) *
                                 stat->os_bsize) >> shift;
                        total = (stat->os_blocks * stat->os_bsize) >> shift;
                }

                if ((used + avail) > 0)
                        ratio = (double)used / (double)(used + avail);

                if (cooked) {
                        int i;
                        double cook_val;

                        cook_val = (double)total;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(tbuf, HDF, cook_val, suffix[i - 1]);
                        else
                                sprintf(tbuf, CDF, total);

                        cook_val = (double)used;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(ubuf, HDF, cook_val, suffix[i - 1]);
                        else
                                sprintf(ubuf, CDF, used);

                        cook_val = (double)avail;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(abuf, HDF, cook_val, suffix[i - 1]);
                        else
                                sprintf(abuf, CDF, avail);
                } else {
                        sprintf(tbuf, CDF, total);
                        sprintf(ubuf, CDF, used);
                        sprintf(abuf, CDF, avail);
                }

                sprintf(rbuf, RDF, (int)(ratio * 100 + 0.5));
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s",
                       uuid, tbuf, ubuf, abuf, rbuf, mntdir);
                if (type)
                        printf("[%s:%d]\n", type, index);
                else
                        printf("\n");

                break;
        case -ENODATA:
                printf(UUF": inactive device\n", uuid);
                break;
        default:
                printf(UUF": %s\n", uuid, strerror(-rc));
                break;
        }

        return 0;
}

struct ll_stat_type {
        int   st_op;
        char *st_name;
};

static int mntdf(char *mntdir, char *fsname, char *pool, int ishow, int cooked)
{
        struct obd_statfs stat_buf, sum = { .os_bsize = 1 };
        struct obd_uuid uuid_buf;
        char *poolname = NULL;
        struct ll_stat_type types[] = { { LL_STATFS_MDC, "MDT" },
                                        { LL_STATFS_LOV, "OST" },
                                        { 0, NULL } };
        struct ll_stat_type *tp;
        __u32 index;
        int rc;

        if (pool) {
                poolname = strchr(pool, '.');
                if (poolname != NULL) {
                        if (strncmp(fsname, pool, strlen(fsname))) {
                                fprintf(stderr, "filesystem name incorrect\n");
                                return -ENODEV;
                        }
                        poolname++;
                } else
                        poolname = pool;
        }

        if (ishow)
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
                       "UUID", "Inodes", "IUsed", "IFree",
                       "IUse%", "Mounted on");
        else
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
                       "UUID", cooked ? "bytes" : "1K-blocks",
                       "Used", "Available", "Use%", "Mounted on");

        for (tp = types; tp->st_name != NULL; tp++) {
                for (index = 0; ; index++) {
                        memset(&stat_buf, 0, sizeof(struct obd_statfs));
                        memset(&uuid_buf, 0, sizeof(struct obd_uuid));
                        rc = llapi_obd_statfs(mntdir, tp->st_op, index,
                                              &stat_buf, &uuid_buf);
                        if (rc == -ENODEV)
                                break;

                        if (poolname && tp->st_op == LL_STATFS_LOV &&
                            llapi_search_ost(fsname, poolname,
                                             obd_uuid2str(&uuid_buf)) != 1)
                                continue;

                        /* the llapi_obd_statfs() call may have returned with
                         * an error, but if it filled in uuid_buf we will at
                         * lease use that to print out a message for that OBD.
                         * If we didn't get anything in the uuid_buf, then fill
                         * it in so that we can print an error message. */
                        if (uuid_buf.uuid[0] == '\0')
                                sprintf(uuid_buf.uuid, "%s%04x",
					tp->st_name, index);
			showdf(mntdir,&stat_buf,obd_uuid2str(&uuid_buf),
			       ishow, cooked, tp->st_name, index, rc);

                        if (rc == 0) {
                                if (tp->st_op == LL_STATFS_MDC) {
                                        sum.os_ffree += stat_buf.os_ffree;
                                        sum.os_files += stat_buf.os_files;
                                } else /* if (tp->st_op == LL_STATFS_LOV) */ {
                                        sum.os_blocks += stat_buf.os_blocks *
                                                stat_buf.os_bsize;
                                        sum.os_bfree  += stat_buf.os_bfree *
                                                stat_buf.os_bsize;
                                        sum.os_bavail += stat_buf.os_bavail *
                                                stat_buf.os_bsize;
                                }
                        } else if (rc == -EINVAL || rc == -EFAULT) {
                                break;
                        }
                }
        }

        printf("\n");
        showdf(mntdir, &sum, "filesystem summary:", ishow, cooked, NULL, 0,0);
        printf("\n");
        return 0;
}

static int lfs_df(int argc, char **argv)
{
        char mntdir[PATH_MAX] = {'\0'}, path[PATH_MAX] = {'\0'};
        int ishow = 0, cooked = 0;
        int c, rc = 0, index = 0;
        char fsname[PATH_MAX] = "", *pool_name = NULL;
        struct option long_opts[] = {
                {"pool", required_argument, 0, 'p'},
                {0, 0, 0, 0}
        };

        optind = 0;
        while ((c = getopt_long(argc, argv, "hip:", long_opts, NULL)) != -1) {
                switch (c) {
                case 'i':
                        ishow = 1;
                        break;
                case 'h':
                        cooked = 1;
                        break;
                case 'p':
                        pool_name = optarg;
                        break;
                default:
                        return CMD_HELP;
                }
        }
        if (optind < argc && !realpath(argv[optind], path)) {
                rc = -errno;
                fprintf(stderr, "error: invalid path '%s': %s\n",
                        argv[optind], strerror(-rc));
                return rc;
        }

        while (!llapi_search_mounts(path, index++, mntdir, fsname)) {
                /* Check if we have a mount point */
                if (mntdir[0] == '\0')
                        continue;

                rc = mntdf(mntdir, fsname, pool_name, ishow, cooked);
                if (rc || path[0] != '\0')
                        break;
                fsname[0] = '\0'; /* avoid matching in next loop */
                mntdir[0] = '\0'; /* avoid matching in next loop */
        }

        return rc;
}

static int lfs_getname(int argc, char **argv)
{
        char mntdir[PATH_MAX] = "", path[PATH_MAX] = "", fsname[PATH_MAX] = "";
        int rc = 0, index = 0, c;
        char buf[sizeof(struct obd_uuid)];

        optind = 0;
        while ((c = getopt(argc, argv, "h")) != -1)
                return CMD_HELP;

        if (optind == argc) { /* no paths specified, get all paths. */
                while (!llapi_search_mounts(path, index++, mntdir, fsname)) {
                        rc = llapi_getname(mntdir, buf, sizeof(buf));
                        if (rc < 0) {
                                fprintf(stderr,
                                        "cannot get name for `%s': %s\n",
                                        mntdir, strerror(-rc));
                                break;
                        }

                        printf("%s %s\n", buf, mntdir);

                        path[0] = fsname[0] = mntdir[0] = 0;
                }
        } else { /* paths specified, only attempt to search these. */
                for (; optind < argc; optind++) {
                        rc = llapi_getname(argv[optind], buf, sizeof(buf));
                        if (rc < 0) {
                                fprintf(stderr,
                                        "cannot get name for `%s': %s\n",
                                        argv[optind], strerror(-rc));
                                break;
                        }

                        printf("%s %s\n", buf, argv[optind]);
                }
        }
        return rc;
}

static int lfs_check(int argc, char **argv)
{
        int rc;
        char mntdir[PATH_MAX] = {'\0'};
        int num_types = 1;
        char *obd_types[2];
        char obd_type1[4];
        char obd_type2[4];

        if (argc != 2)
                return CMD_HELP;

        obd_types[0] = obd_type1;
        obd_types[1] = obd_type2;

        if (strcmp(argv[1], "osts") == 0) {
                strcpy(obd_types[0], "osc");
        } else if (strcmp(argv[1], "mds") == 0) {
                strcpy(obd_types[0], "mdc");
        } else if (strcmp(argv[1], "servers") == 0) {
                num_types = 2;
                strcpy(obd_types[0], "osc");
                strcpy(obd_types[1], "mdc");
        } else {
                fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[1]);
                        return CMD_HELP;
        }

        rc = llapi_search_mounts(NULL, 0, mntdir, NULL);
        if (rc < 0 || mntdir[0] == '\0') {
                fprintf(stderr, "No suitable Lustre mount found\n");
                return rc;
        }

        rc = llapi_target_iterate(num_types, obd_types,
                                  mntdir, llapi_ping_target);

        if (rc)
                fprintf(stderr, "error: %s: %s status failed\n",
                                argv[0],argv[1]);

        return rc;

}

static int lfs_catinfo(int argc, char **argv)
{
        char mntdir[PATH_MAX] = {'\0'};
        int rc;

        if (argc < 2 || (!strcmp(argv[1],"config") && argc < 3))
                return CMD_HELP;

        if (strcmp(argv[1], "config") && strcmp(argv[1], "deletions"))
                return CMD_HELP;

        rc = llapi_search_mounts(NULL, 0, mntdir, NULL);
        if (rc == 0 && mntdir[0] != '\0') {
                if (argc == 3)
                        rc = llapi_catinfo(mntdir, argv[1], argv[2]);
                else
                        rc = llapi_catinfo(mntdir, argv[1], NULL);
        } else {
                fprintf(stderr, "no lustre_lite mounted.\n");
                rc = -1;
        }

        return rc;
}

static int lfs_join(int argc, char **argv)
{
        fprintf(stderr, "join two lustre files into one.\n"
                        "obsolete, HEAD does not support it anymore.\n");
        return 0;
}

#ifdef HAVE_SYS_QUOTA_H
static int lfs_quotachown(int argc, char **argv)
{

        int c,rc;
        int flag = 0;

        optind = 0;
        while ((c = getopt(argc, argv, "i")) != -1) {
                switch (c) {
                case 'i':
                        flag++;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }
        if (optind == argc)
                return CMD_HELP;
        rc = llapi_quotachown(argv[optind], flag);
        if(rc)
                fprintf(stderr,"error: change file owner/group failed.\n");
        return rc;
}

static int lfs_quotacheck(int argc, char **argv)
{
        int c, check_type = 0;
        char *mnt;
        struct if_quotacheck qchk;
        struct if_quotactl qctl;
        char *obd_type = (char *)qchk.obd_type;
        int rc;

        memset(&qchk, 0, sizeof(qchk));

        optind = 0;
        while ((c = getopt(argc, argv, "gu")) != -1) {
                switch (c) {
                case 'u':
                        check_type |= 0x01;
                        break;
                case 'g':
                        check_type |= 0x02;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (check_type)
                check_type--;
        else    /* do quotacheck for both user & group quota by default */
                check_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotacheck(mnt, check_type);
        if (rc) {
                fprintf(stderr, "quotacheck failed: %s\n", strerror(-rc));
                return rc;
        }

        rc = llapi_poll_quotacheck(mnt, &qchk);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qchk.obd_uuid));
                fprintf(stderr, "quota check failed: %s\n", strerror(-rc));
                return rc;
        }

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAON;
        qctl.qc_type = check_type;
        rc = llapi_quotactl(mnt, &qctl);
        if (rc && rc != -EALREADY) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", (char *)qctl.obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
                fprintf(stderr, "%s turn on quota failed: %s\n",
                        argv[0], strerror(-rc));
                return rc;
        }

        return 0;
}

static int lfs_quotaon(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        char *obd_type = (char *)qctl.obd_type;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAON;

        optind = 0;
        while ((c = getopt(argc, argv, "fgu")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                case 'f':
                        qctl.qc_cmd = LUSTRE_Q_QUOTAOFF;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;
        else /* by default, enable quota for both user & group */
                qctl.qc_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (rc == -EALREADY) {
                        rc = 0;
                } else if (rc == -ENOENT) {
                        fprintf(stderr, "error: cannot find quota database, "
                                        "make sure you have run quotacheck\n");
                } else {
                        if (*obd_type)
                                fprintf(stderr, "%s %s ", obd_type,
                                        obd_uuid2str(&qctl.obd_uuid));
                        fprintf(stderr, "%s failed: %s\n", argv[0],
                                strerror(-rc));
                }
        }

        return rc;
}

static int lfs_quotaoff(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        char *obd_type = (char *)qctl.obd_type;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAOFF;

        optind = 0;
        while ((c = getopt(argc, argv, "gu")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;
        else /* by default, disable quota for both user & group */
                qctl.qc_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (rc == -EALREADY) {
                        rc = 0;
                } else {
                        if (*obd_type)
                                fprintf(stderr, "%s %s ", obd_type,
                                        obd_uuid2str(&qctl.obd_uuid));
                        fprintf(stderr, "quotaoff failed: %s\n",
                                strerror(-rc));
                }
        }

        return rc;
}

static int lfs_quotainv(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_INVALIDATE;

        optind = 0;
        while ((c = getopt(argc, argv, "fgu")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                case 'f':
                        qctl.qc_cmd = LUSTRE_Q_FINVALIDATE;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;
        else /* by default, invalidate quota for both user & group */
                qctl.qc_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                fprintf(stderr, "quotainv failed: %s\n", strerror(-rc));
                return rc;
        }

        return 0;
}

#define ARG2INT(nr, str, msg)                                           \
do {                                                                    \
        char *endp;                                                     \
        nr = strtol(str, &endp, 0);                                     \
        if (*endp) {                                                    \
                fprintf(stderr, "error: bad %s: %s\n", msg, str);       \
                return CMD_HELP;                                        \
        }                                                               \
} while (0)

#define ADD_OVERFLOW(a,b) ((a + b) < a) ? (a = ULONG_MAX) : (a = a + b)

/* Convert format time string "XXwXXdXXhXXmXXs" into seconds value
 * returns the value or ULONG_MAX on integer overflow or incorrect format
 * Notes:
 *        1. the order of specifiers is arbitrary (may be: 5w3s or 3s5w)
 *        2. specifiers may be encountered multiple times (2s3s is 5 seconds)
 *        3. empty integer value is interpreted as 0
 */
static unsigned long str2sec(const char* timestr)
{
        const char spec[] = "smhdw";
        const unsigned long mult[] = {1, 60, 60*60, 24*60*60, 7*24*60*60};
        unsigned long val = 0;
        char *tail;

        if (strpbrk(timestr, spec) == NULL) {
                /* no specifiers inside the time string,
                   should treat it as an integer value */
                val = strtoul(timestr, &tail, 10);
                return *tail ? ULONG_MAX : val;
        }

        /* format string is XXwXXdXXhXXmXXs */
        while (*timestr) {
                unsigned long v;
                int ind;
                char* ptr;

                v = strtoul(timestr, &tail, 10);
                if (v == ULONG_MAX || *tail == '\0')
                        /* value too large (ULONG_MAX or more)
                           or missing specifier */
                        goto error;

                ptr = strchr(spec, *tail);
                if (ptr == NULL)
                        /* unknown specifier */
                        goto error;

                ind = ptr - spec;

                /* check if product will overflow the type */
                if (!(v < ULONG_MAX / mult[ind]))
                        goto error;

                ADD_OVERFLOW(val, mult[ind] * v);
                if (val == ULONG_MAX)
                        goto error;

                timestr = tail + 1;
        }

        return val;

error:
        return ULONG_MAX;
}

#define ARG2ULL(nr, str, defscale)                                      \
do {                                                                    \
        unsigned long long limit, units = 0;                            \
        int rc;                                                         \
                                                                        \
        rc = parse_size(str, &limit, &units, 1);                        \
        if (rc < 0) {                                                   \
                fprintf(stderr, "error: bad limit value %s\n", str);    \
                return CMD_HELP;                                        \
        }                                                               \
        nr = ((units == 0) ? (defscale) : 1) * limit;                   \
} while (0)

static inline int has_times_option(int argc, char **argv)
{
        int i;

        for (i = 1; i < argc; i++)
                if (!strcmp(argv[i], "-t"))
                        return 1;

        return 0;
}

int lfs_setquota_times(int argc, char **argv)
{
        int c, rc;
        struct if_quotactl qctl;
        char *mnt, *obd_type = (char *)qctl.obd_type;
        struct obd_dqblk *dqb = &qctl.qc_dqblk;
        struct obd_dqinfo *dqi = &qctl.qc_dqinfo;
        struct option long_opts[] = {
                {"block-grace",     required_argument, 0, 'b'},
                {"group",           no_argument,       0, 'g'},
                {"inode-grace",     required_argument, 0, 'i'},
                {"times",           no_argument,       0, 't'},
                {"user",            no_argument,       0, 'u'},
                {0, 0, 0, 0}
        };

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd  = LUSTRE_Q_SETINFO;
        qctl.qc_type = UGQUOTA;

        optind = 0;
        while ((c = getopt_long(argc, argv, "b:gi:tu", long_opts, NULL)) != -1) {
                switch (c) {
                case 'u':
                case 'g':
                        if (qctl.qc_type != UGQUOTA) {
                                fprintf(stderr, "error: -u and -g can't be used "
                                                "more than once\n");
                                return CMD_HELP;
                        }
                        qctl.qc_type = (c == 'u') ? USRQUOTA : GRPQUOTA;
                        break;
                case 'b':
                        if ((dqi->dqi_bgrace = str2sec(optarg)) == ULONG_MAX) {
                                fprintf(stderr, "error: bad block-grace: %s\n",
                                        optarg);
                                return CMD_HELP;
                        }
                        dqb->dqb_valid |= QIF_BTIME;
                        break;
                case 'i':
                        if ((dqi->dqi_igrace = str2sec(optarg)) == ULONG_MAX) {
                                fprintf(stderr, "error: bad inode-grace: %s\n",
                                        optarg);
                                return CMD_HELP;
                        }
                        dqb->dqb_valid |= QIF_ITIME;
                        break;
                case 't': /* Yes, of course! */
                        break;
                default: /* getopt prints error message for us when opterr != 0 */
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type == UGQUOTA) {
                fprintf(stderr, "error: neither -u nor -g specified\n");
                return CMD_HELP;
        }

        if (optind != argc - 1) {
                fprintf(stderr, "error: unexpected parameters encountered\n");
                return CMD_HELP;
        }

        mnt = argv[optind];
        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
                fprintf(stderr, "setquota failed: %s\n", strerror(-rc));
                return rc;
        }

        return 0;
}

#define BSLIMIT (1 << 0)
#define BHLIMIT (1 << 1)
#define ISLIMIT (1 << 2)
#define IHLIMIT (1 << 3)

int lfs_setquota(int argc, char **argv)
{
        int c, rc;
        struct if_quotactl qctl;
        char *mnt, *obd_type = (char *)qctl.obd_type;
        struct obd_dqblk *dqb = &qctl.qc_dqblk;
        struct option long_opts[] = {
                {"block-softlimit", required_argument, 0, 'b'},
                {"block-hardlimit", required_argument, 0, 'B'},
                {"group",           required_argument, 0, 'g'},
                {"inode-softlimit", required_argument, 0, 'i'},
                {"inode-hardlimit", required_argument, 0, 'I'},
                {"user",            required_argument, 0, 'u'},
                {0, 0, 0, 0}
        };
        unsigned limit_mask = 0;
        char *endptr;

        if (has_times_option(argc, argv))
                return lfs_setquota_times(argc, argv);

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd  = LUSTRE_Q_SETQUOTA;
        qctl.qc_type = UGQUOTA; /* UGQUOTA makes no sense for setquota,
                                 * so it can be used as a marker that qc_type
                                 * isn't reinitialized from command line */

        optind = 0;
        while ((c = getopt_long(argc, argv, "b:B:g:i:I:u:", long_opts, NULL)) != -1) {
                switch (c) {
                case 'u':
                case 'g':
                        if (qctl.qc_type != UGQUOTA) {
                                fprintf(stderr, "error: -u and -g can't be used"
                                                " more than once\n");
                                return CMD_HELP;
                        }
                        qctl.qc_type = (c == 'u') ? USRQUOTA : GRPQUOTA;
                        rc = name2id(&qctl.qc_id, optarg,
                                     (qctl.qc_type == USRQUOTA) ? USER : GROUP);
                        if (rc) {
                                qctl.qc_id = strtoul(optarg, &endptr, 10);
                                if (*endptr != '\0') {
                                        fprintf(stderr, "error: can't find id "
                                                "for name %s\n", optarg);
                                        return CMD_HELP;
                                }
                        }
                        break;
                case 'b':
                        ARG2ULL(dqb->dqb_bsoftlimit, optarg, 1024);
                        dqb->dqb_bsoftlimit >>= 10;
                        limit_mask |= BSLIMIT;
                        break;
                case 'B':
                        ARG2ULL(dqb->dqb_bhardlimit, optarg, 1024);
                        dqb->dqb_bhardlimit >>= 10;
                        limit_mask |= BHLIMIT;
                        break;
                case 'i':
                        ARG2ULL(dqb->dqb_isoftlimit, optarg, 1);
                        limit_mask |= ISLIMIT;
                        break;
                case 'I':
                        ARG2ULL(dqb->dqb_ihardlimit, optarg, 1);
                        limit_mask |= IHLIMIT;
                        break;
                default: /* getopt prints error message for us when opterr != 0 */
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type == UGQUOTA) {
                fprintf(stderr, "error: neither -u nor -g was specified\n");
                return CMD_HELP;
        }

        if (limit_mask == 0) {
                fprintf(stderr, "error: at least one limit must be specified\n");
                return CMD_HELP;
        }

        if (optind != argc - 1) {
                fprintf(stderr, "error: unexpected parameters encountered\n");
                return CMD_HELP;
        }

        mnt = argv[optind];

        if ((!(limit_mask & BHLIMIT) ^ !(limit_mask & BSLIMIT)) ||
            (!(limit_mask & IHLIMIT) ^ !(limit_mask & ISLIMIT))) {
                /* sigh, we can't just set blimits/ilimits */
                struct if_quotactl tmp_qctl = {.qc_cmd  = LUSTRE_Q_GETQUOTA,
                                               .qc_type = qctl.qc_type,
                                               .qc_id   = qctl.qc_id};

                rc = llapi_quotactl(mnt, &tmp_qctl);
                if (rc < 0) {
                        fprintf(stderr, "error: setquota failed while retrieving"
                                        " current quota settings (%s)\n",
                                        strerror(-rc));
                        return rc;
                }

                if (!(limit_mask & BHLIMIT))
                        dqb->dqb_bhardlimit = tmp_qctl.qc_dqblk.dqb_bhardlimit;
                if (!(limit_mask & BSLIMIT))
                        dqb->dqb_bsoftlimit = tmp_qctl.qc_dqblk.dqb_bsoftlimit;
                if (!(limit_mask & IHLIMIT))
                        dqb->dqb_ihardlimit = tmp_qctl.qc_dqblk.dqb_ihardlimit;
                if (!(limit_mask & ISLIMIT))
                        dqb->dqb_isoftlimit = tmp_qctl.qc_dqblk.dqb_isoftlimit;

                /* Keep grace times if we have got no softlimit arguments */
                if ((limit_mask & BHLIMIT) && !(limit_mask & BSLIMIT)) {
                        dqb->dqb_valid |= QIF_BTIME;
                        dqb->dqb_btime = tmp_qctl.qc_dqblk.dqb_btime;
                }

                if ((limit_mask & IHLIMIT) && !(limit_mask & ISLIMIT)) {
                        dqb->dqb_valid |= QIF_ITIME;
                        dqb->dqb_itime = tmp_qctl.qc_dqblk.dqb_itime;
                }
        }

        dqb->dqb_valid |= (limit_mask & (BHLIMIT | BSLIMIT)) ? QIF_BLIMITS : 0;
        dqb->dqb_valid |= (limit_mask & (IHLIMIT | ISLIMIT)) ? QIF_ILIMITS : 0;

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
                fprintf(stderr, "setquota failed: %s\n", strerror(-rc));
                return rc;
        }

        return 0;
}

static inline char *type2name(int check_type)
{
        if (check_type == USRQUOTA)
                return "user";
        else if (check_type == GRPQUOTA)
                return "group";
        else
                return "unknown";
}

/* Converts seconds value into format string
 * result is returned in buf
 * Notes:
 *        1. result is in descenting order: 1w2d3h4m5s
 *        2. zero fields are not filled (except for p. 3): 5d1s
 *        3. zero seconds value is presented as "0s"
 */
static char * __sec2str(time_t seconds, char *buf)
{
        const char spec[] = "smhdw";
        const unsigned long mult[] = {1, 60, 60*60, 24*60*60, 7*24*60*60};
        unsigned long c;
        char *tail = buf;
        int i;

        for (i = sizeof(mult) / sizeof(mult[0]) - 1 ; i >= 0; i--) {
                c = seconds / mult[i];

                if (c > 0 || (i == 0 && buf == tail))
                        tail += snprintf(tail, 40-(tail-buf), "%lu%c", c, spec[i]);

                seconds %= mult[i];
        }

        return tail;
}

static void sec2str(time_t seconds, char *buf, int rc)
{
        char *tail = buf;

        if (rc)
                *tail++ = '[';

        tail = __sec2str(seconds, tail);

        if (rc && tail - buf < 39) {
                *tail++ = ']';
                *tail++ = 0;
        }
}

static void diff2str(time_t seconds, char *buf, time_t now)
{

        buf[0] = 0;
        if (!seconds)
                return;
        if (seconds <= now) {
                strcpy(buf, "none");
                return;
        }
        __sec2str(seconds - now, buf);
}

static void print_quota_title(char *name, struct if_quotactl *qctl)
{
        printf("Disk quotas for %s %s (%cid %u):\n",
               type2name(qctl->qc_type), name,
               *type2name(qctl->qc_type), qctl->qc_id);
        printf("%15s%8s %7s%8s%8s%8s %7s%8s%8s\n",
               "Filesystem",
               "kbytes", "quota", "limit", "grace",
               "files", "quota", "limit", "grace");
}

static void print_quota(char *mnt, struct if_quotactl *qctl, int type, int rc)
{
        time_t now;

        time(&now);

        if (qctl->qc_cmd == LUSTRE_Q_GETQUOTA || qctl->qc_cmd == Q_GETOQUOTA) {
                int bover = 0, iover = 0;
                struct obd_dqblk *dqb = &qctl->qc_dqblk;

                if (dqb->dqb_bhardlimit &&
                    toqb(dqb->dqb_curspace) >= dqb->dqb_bhardlimit) {
                        bover = 1;
                } else if (dqb->dqb_bsoftlimit &&
                           toqb(dqb->dqb_curspace) >= dqb->dqb_bsoftlimit) {
                        if (dqb->dqb_btime > now) {
                                bover = 2;
                        } else {
                                bover = 3;
                        }
                }

                if (dqb->dqb_ihardlimit &&
                    dqb->dqb_curinodes >= dqb->dqb_ihardlimit) {
                        iover = 1;
                } else if (dqb->dqb_isoftlimit &&
                           dqb->dqb_curinodes >= dqb->dqb_isoftlimit) {
                        if (dqb->dqb_btime > now) {
                                iover = 2;
                        } else {
                                iover = 3;
                        }
                }

#if 0           /* XXX: always print quotas even when no usages */
                if (dqb->dqb_curspace || dqb->dqb_curinodes)
#endif
                {
                        char numbuf[3][32];
                        char timebuf[40];

                        if (strlen(mnt) > 15)
                                printf("%s\n%15s", mnt, "");
                        else
                                printf("%15s", mnt);

                        if (bover)
                                diff2str(dqb->dqb_btime, timebuf, now);
                        if (rc == -EREMOTEIO)
                                sprintf(numbuf[0], LPU64"*",
                                        toqb(dqb->dqb_curspace));
                        else
                                sprintf(numbuf[0],
                                        (dqb->dqb_valid & QIF_SPACE) ?
                                        LPU64 : "["LPU64"]",
                                        toqb(dqb->dqb_curspace));
                        if (type == QC_GENERAL)
                                sprintf(numbuf[1], (dqb->dqb_valid & QIF_BLIMITS)
                                        ? LPU64 : "["LPU64"]",
                                        dqb->dqb_bsoftlimit);
                        else
                                sprintf(numbuf[1], "%s", "-");
                        sprintf(numbuf[2], (dqb->dqb_valid & QIF_BLIMITS)
                                ? LPU64 : "["LPU64"]", dqb->dqb_bhardlimit);
                        printf(" %7s%c %6s %7s %7s",
                               numbuf[0], bover ? '*' : ' ', numbuf[1],
                               numbuf[2], bover > 1 ? timebuf : "-");

                        if (iover)
                                diff2str(dqb->dqb_itime, timebuf, now);

                        sprintf(numbuf[0], (dqb->dqb_valid & QIF_INODES) ?
                                LPU64 : "["LPU64"]", dqb->dqb_curinodes);
                       if (type == QC_GENERAL)
                                sprintf(numbuf[1], (dqb->dqb_valid & QIF_ILIMITS)
                                        ? LPU64 : "["LPU64"]",
                                        dqb->dqb_isoftlimit);
                        else
                                sprintf(numbuf[1], "%s", "-");
                        sprintf(numbuf[2], (dqb->dqb_valid & QIF_ILIMITS) ?
                                LPU64 : "["LPU64"]", dqb->dqb_ihardlimit);
                        if (type != QC_OSTIDX)
                                printf(" %7s%c %6s %7s %7s",
                                       numbuf[0], iover ? '*' : ' ', numbuf[1],
                                       numbuf[2], iover > 1 ? timebuf : "-");
                        else
                                printf(" %7s %7s %7s %7s", "-", "-", "-", "-");
                        printf("\n");
                }
        } else if (qctl->qc_cmd == LUSTRE_Q_GETINFO ||
                   qctl->qc_cmd == Q_GETOINFO) {
                char bgtimebuf[40];
                char igtimebuf[40];

                sec2str(qctl->qc_dqinfo.dqi_bgrace, bgtimebuf, rc);
                sec2str(qctl->qc_dqinfo.dqi_igrace, igtimebuf, rc);
                printf("Block grace time: %s; Inode grace time: %s\n",
                       bgtimebuf, igtimebuf);
        }
}

static int print_obd_quota(char *mnt, struct if_quotactl *qctl, int is_mdt)
{
        int rc = 0, rc1 = 0, count = 0;
        __u32 valid = qctl->qc_valid;

        rc = llapi_get_obd_count(mnt, &count, is_mdt);
        if (rc) {
                fprintf(stderr, "can not get %s count: %s\n",
                        is_mdt ? "mdt": "ost", strerror(-rc));
                return rc;
        }

        for (qctl->qc_idx = 0; qctl->qc_idx < count; qctl->qc_idx++) {
                qctl->qc_valid = is_mdt ? QC_MDTIDX : QC_OSTIDX;
                rc = llapi_quotactl(mnt, qctl);
                if (rc) {
                        /* It is remote client case. */
                        if (-rc == EOPNOTSUPP) {
                                rc = 0;
                                goto out;
                        }

                        if (!rc1)
                                rc1 = rc;
                        fprintf(stderr, "quotactl %s%d failed.\n",
                                is_mdt ? "mdt": "ost", qctl->qc_idx);
                        continue;
                }

                print_quota(obd_uuid2str(&qctl->obd_uuid), qctl, qctl->qc_valid, 0);
        }

out:
        qctl->qc_valid = valid;
        return rc ? : rc1;
}

static int lfs_quota(int argc, char **argv)
{
        int c;
        char *mnt, *name = NULL;
        struct if_quotactl qctl = { .qc_cmd = LUSTRE_Q_GETQUOTA,
                                    .qc_type = UGQUOTA };
        char *obd_type = (char *)qctl.obd_type;
        char *obd_uuid = (char *)qctl.obd_uuid.uuid;
        int rc, rc1 = 0, rc2 = 0, rc3 = 0,
            verbose = 0, pass = 0, quiet = 0, inacc;
        char *endptr;
        __u32 valid = QC_GENERAL, idx = 0;

        optind = 0;
        while ((c = getopt(argc, argv, "gi:I:o:qtuv")) != -1) {
                switch (c) {
                case 'u':
                        if (qctl.qc_type != UGQUOTA) {
                                fprintf(stderr, "error: use either -u or -g\n");
                                return CMD_HELP;
                        }
                        qctl.qc_type = USRQUOTA;
                        break;
                case 'g':
                        if (qctl.qc_type != UGQUOTA) {
                                fprintf(stderr, "error: use either -u or -g\n");
                                return CMD_HELP;
                        }
                        qctl.qc_type = GRPQUOTA;
                        break;
                case 't':
                        qctl.qc_cmd = LUSTRE_Q_GETINFO;
                        break;
                case 'o':
                        valid = qctl.qc_valid = QC_UUID;
                        strncpy(obd_uuid, optarg, sizeof(qctl.obd_uuid));
                        break;
                case 'i':
                        valid = qctl.qc_valid = QC_MDTIDX;
                        idx = qctl.qc_idx = atoi(optarg);
                        break;
                case 'I':
                        valid = qctl.qc_valid = QC_OSTIDX;
                        idx = qctl.qc_idx = atoi(optarg);
                        break;
                case 'v':
                        verbose = 1;
                        break;
                case 'q':
                        quiet = 1;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        /* current uid/gid info for "lfs quota /path/to/lustre/mount" */
        if (qctl.qc_cmd == LUSTRE_Q_GETQUOTA && qctl.qc_type == UGQUOTA &&
            optind == argc - 1) {
ug_output:
                memset(&qctl, 0, sizeof(qctl)); /* spoiled by print_*_quota */
                qctl.qc_cmd = LUSTRE_Q_GETQUOTA;
                qctl.qc_valid = valid;
                qctl.qc_idx = idx;
                if (pass++ == 0) {
                        qctl.qc_type = USRQUOTA;
                        qctl.qc_id = geteuid();
                } else {
                        qctl.qc_type = GRPQUOTA;
                        qctl.qc_id = getegid();
                }
                rc = id2name(&name, qctl.qc_id,
                             (qctl.qc_type == USRQUOTA) ? USER : GROUP);
                if (rc)
                        name = "<unknown>";
        /* lfs quota -u username /path/to/lustre/mount */
        } else if (qctl.qc_cmd == LUSTRE_Q_GETQUOTA) {
                /* options should be followed by u/g-name and mntpoint */
                if (optind + 2 != argc || qctl.qc_type == UGQUOTA) {
                        fprintf(stderr, "error: missing quota argument(s)\n");
                        return CMD_HELP;
                }

                name = argv[optind++];
                rc = name2id(&qctl.qc_id, name,
                             (qctl.qc_type == USRQUOTA) ? USER : GROUP);
                if (rc) {
                        qctl.qc_id = strtoul(name, &endptr, 10);
                        if (*endptr != '\0') {
                                fprintf(stderr, "error: can't find id for name "
                                        "%s\n", name);
                                return CMD_HELP;
                        }
                }
        } else if (optind + 1 != argc || qctl.qc_type == UGQUOTA) {
                fprintf(stderr, "error: missing quota info argument(s)\n");
                return CMD_HELP;
        }

        mnt = argv[optind];

        rc1 = llapi_quotactl(mnt, &qctl);
        if (rc1 < 0) {
                switch (rc1) {
                case -ESRCH:
                        fprintf(stderr, "%s quotas are not enabled.\n",
                                qctl.qc_type == USRQUOTA ? "user" : "group");
                        goto out;
                case -EPERM:
                        fprintf(stderr, "Permission denied.\n");
                case -ENOENT:
                        /* We already got a "No such file..." message. */
                        goto out;
                default:
                        fprintf(stderr, "Unexpected quotactl error: %s\n",
                                strerror(-rc1));
                }
        }

        if (qctl.qc_cmd == LUSTRE_Q_GETQUOTA && !quiet)
                print_quota_title(name, &qctl);

        if (rc1 && *obd_type)
                fprintf(stderr, "%s %s ", obd_type, obd_uuid);

        if (qctl.qc_valid != QC_GENERAL)
                mnt = "";

        inacc = (qctl.qc_cmd == LUSTRE_Q_GETQUOTA) &&
                ((qctl.qc_dqblk.dqb_valid&(QIF_LIMITS|QIF_USAGE))!=(QIF_LIMITS|QIF_USAGE));

        print_quota(mnt, &qctl, QC_GENERAL, rc1);

        if (qctl.qc_valid == QC_GENERAL && qctl.qc_cmd != LUSTRE_Q_GETINFO && verbose) {
                rc2 = print_obd_quota(mnt, &qctl, 1);
                rc3 = print_obd_quota(mnt, &qctl, 0);
        }

        if (rc1 || rc2 || rc3 || inacc)
                printf("Some errors happened when getting quota info. "
                       "Some devices may be not working or deactivated. "
                       "The data in \"[]\" is inaccurate.\n");

out:
        if (pass == 1)
                goto ug_output;

        return rc1;
}
#endif /* HAVE_SYS_QUOTA_H! */

static int flushctx_ioctl(char *mp)
{
        int fd, rc;

        fd = open(mp, O_RDONLY);
        if (fd == -1) {
                fprintf(stderr, "flushctx: error open %s: %s\n",
                        mp, strerror(errno));
                return -1;
        }

        rc = ioctl(fd, LL_IOC_FLUSHCTX);
        if (rc == -1)
                fprintf(stderr, "flushctx: error ioctl %s: %s\n",
                        mp, strerror(errno));

        close(fd);
        return rc;
}

static int lfs_flushctx(int argc, char **argv)
{
        int     kdestroy = 0, c;
        FILE   *proc;
        char    procline[PATH_MAX], *line;
        int     rc = 0;

        optind = 0;
        while ((c = getopt(argc, argv, "k")) != -1) {
                switch (c) {
                case 'k':
                        kdestroy = 1;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (kdestroy) {
            int rc;
            if ((rc = system("kdestroy > /dev/null")) != 0) {
                rc = WEXITSTATUS(rc);
                fprintf(stderr, "error destroying tickets: %d, continuing\n", rc);
            }
        }

        if (optind >= argc) {
                /* flush for all mounted lustre fs. */
                proc = fopen("/proc/mounts", "r");
                if (!proc) {
                        fprintf(stderr, "error: %s: can't open /proc/mounts\n",
                                argv[0]);
                        return -1;
                }

                while ((line = fgets(procline, PATH_MAX, proc)) != NULL) {
                        char dev[PATH_MAX];
                        char mp[PATH_MAX];
                        char fs[PATH_MAX];

                        if (sscanf(line, "%s %s %s", dev, mp, fs) != 3) {
                                fprintf(stderr, "%s: unexpected format in "
                                                "/proc/mounts\n",
                                        argv[0]);
                                return -1;
                        }

                        if (strcmp(fs, "lustre") != 0)
                                continue;
                        /* we use '@' to determine it's a client. are there
                         * any other better way?
                         */
                        if (strchr(dev, '@') == NULL)
                                continue;

                        if (flushctx_ioctl(mp))
                                rc = -1;
                }
        } else {
                /* flush fs as specified */
                while (optind < argc) {
                        if (flushctx_ioctl(argv[optind++]))
                                rc = -1;
                }
        }

        return rc;
}

static int lfs_lsetfacl(int argc, char **argv)
{
        argv[0]++;
        return(llapi_lsetfacl(argc, argv));
}

static int lfs_lgetfacl(int argc, char **argv)
{
        argv[0]++;
        return(llapi_lgetfacl(argc, argv));
}

static int lfs_rsetfacl(int argc, char **argv)
{
        argv[0]++;
        return(llapi_rsetfacl(argc, argv));
}

static int lfs_rgetfacl(int argc, char **argv)
{
        argv[0]++;
        return(llapi_rgetfacl(argc, argv));
}

static int lfs_cp(int argc, char **argv)
{
        return(llapi_cp(argc, argv));
}

static int lfs_ls(int argc, char **argv)
{
        return(llapi_ls(argc, argv));
}

static int lfs_changelog(int argc, char **argv)
{
        void *changelog_priv;
        struct changelog_rec *rec;
        long long startrec = 0, endrec = 0;
        char *mdd;
        struct option long_opts[] = {
                {"follow", no_argument, 0, 'f'},
                {0, 0, 0, 0}
        };
        char short_opts[] = "f";
        int rc, follow = 0;

        optind = 0;
        while ((rc = getopt_long(argc, argv, short_opts,
                                long_opts, NULL)) != -1) {
                switch (rc) {
                case 'f':
                        follow++;
                        break;
                case '?':
                        return CMD_HELP;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                }
        }
        if (optind >= argc)
                return CMD_HELP;

        mdd = argv[optind++];
        if (argc > optind)
                startrec = strtoll(argv[optind++], NULL, 10);
        if (argc > optind)
                endrec = strtoll(argv[optind++], NULL, 10);

        rc = llapi_changelog_start(&changelog_priv,
                                   CHANGELOG_FLAG_BLOCK |
                                   (follow ? CHANGELOG_FLAG_FOLLOW : 0),
                                   mdd, startrec);
        if (rc < 0) {
                fprintf(stderr, "Can't start changelog: %s\n",
                        strerror(errno = -rc));
                return rc;
        }

        while ((rc = llapi_changelog_recv(changelog_priv, &rec)) == 0) {
                time_t secs;
                struct tm ts;

                if (endrec && rec->cr_index > endrec) {
                        llapi_changelog_free(&rec);
                        break;
                }
                if (rec->cr_index < startrec) {
                        llapi_changelog_free(&rec);
                        continue;
                }

                secs = rec->cr_time >> 30;
                gmtime_r(&secs, &ts);
                printf(LPU64" %02d%-5s %02d:%02d:%02d.%06d %04d.%02d.%02d "
                       "0x%x t="DFID, rec->cr_index, rec->cr_type,
                       changelog_type2str(rec->cr_type),
                       ts.tm_hour, ts.tm_min, ts.tm_sec,
                       (int)(rec->cr_time & ((1<<30) - 1)),
                       ts.tm_year+1900, ts.tm_mon+1, ts.tm_mday,
                       rec->cr_flags & CLF_FLAGMASK, PFID(&rec->cr_tfid));
                if (rec->cr_namelen)
                        /* namespace rec includes parent and filename */
                        printf(" p="DFID" %.*s\n", PFID(&rec->cr_pfid),
                               rec->cr_namelen, rec->cr_name);
                else
                        printf("\n");

                llapi_changelog_free(&rec);
        }

        llapi_changelog_fini(&changelog_priv);

        if (rc < 0)
                fprintf(stderr, "Changelog: %s\n", strerror(errno = -rc));

        return (rc == 1 ? 0 : rc);
}

static int lfs_changelog_clear(int argc, char **argv)
{
        long long endrec;
        int rc;

        if (argc != 4)
                return CMD_HELP;

        endrec = strtoll(argv[3], NULL, 10);

        rc = llapi_changelog_clear(argv[1], argv[2], endrec);
        if (rc)
                fprintf(stderr, "%s error: %s\n", argv[0],
                        strerror(errno = -rc));
        return rc;
}

static int lfs_fid2path(int argc, char **argv)
{
        struct option long_opts[] = {
                {"cur", no_argument, 0, 'c'},
                {"link", required_argument, 0, 'l'},
                {"rec", required_argument, 0, 'r'},
                {0, 0, 0, 0}
        };
        char  short_opts[] = "cl:r:";
        char *device, *fid, *path;
        long long recno = -1;
        int linkno = -1;
        int lnktmp;
        int printcur = 0;
        int rc;

        optind = 0;

        while ((rc = getopt_long(argc, argv, short_opts,
                                long_opts, NULL)) != -1) {
                switch (rc) {
                case 'c':
                        printcur++;
                        break;
                case 'l':
                        linkno = strtol(optarg, NULL, 10);
                        break;
                case 'r':
                        recno = strtoll(optarg, NULL, 10);
                        break;
                case '?':
                        return CMD_HELP;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                }
        }
        device = argv[optind++];
        fid = argv[optind++];
        if (optind != argc)
                return CMD_HELP;

        path = calloc(1, PATH_MAX);

        lnktmp = (linkno >= 0) ? linkno : 0;
        while (1) {
                int oldtmp = lnktmp;
                long long rectmp = recno;
                rc = llapi_fid2path(device, fid, path, PATH_MAX, &rectmp,
                                    &lnktmp);
                if (rc < 0) {
                        fprintf(stderr, "%s error: %s\n", argv[0],
                                strerror(errno = -rc));
                        break;
                }

                if (printcur)
                        fprintf(stdout, "%lld ", rectmp);
                if (device[0] == '/') {
                        fprintf(stdout, "%s", device);
                        if (device[strlen(device) - 1] != '/')
                                fprintf(stdout, "/");
                } else if (path[0] == '\0') {
                        fprintf(stdout, "/");
                }
                fprintf(stdout, "%s\n", path);

                if (linkno >= 0)
                        /* specified linkno */
                        break;
                if (oldtmp == lnktmp)
                        /* no more links */
                        break;
        }

        free(path);
        return rc;
}

static int lfs_path2fid(int argc, char **argv)
{
        char *path;
        lustre_fid fid;
        int rc;

        if (argc != 2)
                return CMD_HELP;

        path = argv[1];
        rc = llapi_path2fid(path, &fid);
        if (rc) {
                fprintf(stderr, "can't get fid for %s: %s\n", path,
                        strerror(errno = -rc));
                return rc;
        }

        printf(DFID"\n", PFID(&fid));

        return 0;
}

int main(int argc, char **argv)
{
        int rc;

        setlinebuf(stdout);

        ptl_initialize(argc, argv);
        if (obd_initialize(argc, argv) < 0)
                exit(2);
        if (dbg_initialize(argc, argv) < 0)
                exit(3);

        Parser_init("lfs > ", cmdlist);

        if (argc > 1) {
                rc = Parser_execarg(argc - 1, argv + 1, cmdlist);
        } else {
                rc = Parser_commands();
        }

        obd_finalize(argc, argv);
        return rc < 0 ? -rc : rc;
}

