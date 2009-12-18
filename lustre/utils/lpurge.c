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
 * Copyright  2009 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/utils/lpurge.c
 *
 * Author: Jim Garlick <garlick@llnl.gov>
 */

#if HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <libgen.h>
#include <getopt.h>
#include <sys/vfs.h>
#include <sys/statfs.h>
#define HAVE_GETOPT_LONG 1
#include <sys/time.h>
#include <time.h>

#include <lustre/liblustreapi.h>
#include <lustre/lustre_user.h>

struct elist_struct {
        char **paths;
        int len;
        int count;
};
#define ELIST_CHUNK        16

static struct elist_struct *elist_create(void);
static void elist_destroy(struct elist_struct *elist);
static void elist_add(struct elist_struct *elist, char *path);
static void elist_add_fromfile(struct elist_struct *elist, char *file);
static int elist_find(struct elist_struct *elist, const char *path);
static void elist_verify(struct elist_struct *elist, const char *purgepath);
static unsigned long long purge(const char *path, time_t thresh,
                                struct elist_struct *elist, FILE *outf,
                                FILE *tallyf, int Ropt, int sopt, int lustre);
static void zap_trailing_chars(char *buf, char *chars);
static int is_lustre_fs(const char *path);
static int llapi_stat_mds(int fd, char *fname, struct stat *sb);
static char *timestr(time_t *t);

#ifndef MAX
#define MAX(i,j)        ((i) > (j) ? (i) : (j))
#endif

/* Counters for statistics reporting:
 * elig_files = count of files eligible for purge
 * elig_bytes = total size of eligible files
 */
static unsigned long long elig_files = 0;
static unsigned long long elig_bytes = 0;

static char *prog;

#define OPTIONS "d:o:i:x:X:Rs"
#if HAVE_GETOPT_LONG
#define GETOPT(ac,av,opt,lopt) getopt_long(ac,av,opt,lopt,NULL)
static const struct option longopts[] = {
        {"exclude",         required_argument,  0, 'x'},
        {"exclude-from",    required_argument,  0, 'X'},
        {"days",            required_argument,  0, 'd'},
        {"outfile",         required_argument,  0, 'o'},
        {"inode-tally",     required_argument,  0, 'i'},
        {"remove",          no_argument,        0, 'R'},
        {"stat-always",     no_argument,        0, 's'},
        {0, 0, 0, 0},
};
#else
#define GETOPT(ac,av,opt,lopt) getopt(ac,av,opt)
#endif

static void
usage(void)
{
        fprintf(stderr, "Usage: %s [options] directory\n%s", prog,
 "  -R,--remove                unlink eligible files (requires -o, -d)\n"
 "  -d,--days n                set purge threshold at n days ([cma]time)\n"
 "  -o,--outfile file          file to log eligible files (- for stdout)\n"
 "  -i,--inode-tally           file to log topdir inode counts (- for stdout)\n"
 "  -x,--exclude dir           exclude directory (may specify multiple times)\n"
 "  -X,--exclude-from file     read exclusions from file\n"
 "  -s,--stat-always           disable Lustre optimization, always stat(2)\n");

        fprintf(stderr, "\nExamples:\n%s",
 "  To count files in /mnt/lustre/jones:\n"
 "    lpurge /mnt/lustre/jones\n"
 "  To list files in /mnt/lustre older than 90 days to stdout:\n"
 "    lpurge -d 90 -o- /mnt/lustre\n"
 "  Same as above but remove the files and record in /tmp/purge.log:\n"
 "    lpurge -R -d 90 -o /tmp/purge.log /mnt/lustre\n"
 "  Same as above but exclude joey directory\n"
 "    lpurge -R -d 90 -o /tmp/purge.log -x /mnt/lustre/joey /mnt/lustre\n");
        exit(1);
}

int
main(int argc, char *argv[])
{
        int c, days = 0, lustre = 0, Ropt = 0, sopt = 0;
        struct timeval start, fin;
        unsigned long min;
        unsigned long long chk_files = 0;
        time_t thresh = 0;
        FILE *outf = NULL;
        FILE *tallyf = NULL;
        struct stat sb;
        char purgepath[PATH_MAX];
        struct elist_struct *elist;
        int exit_val = 0;

        prog = basename(argv[0]);
        elist = elist_create();
        while ((c = GETOPT(argc, argv, OPTIONS, longopts)) != -1) {
                switch (c) {
                        case 's':        /* --stat-always */
                                sopt = 1;
                                break;
                        case 'R':        /* --remove */
                                Ropt = 1;
                                break;
                        case 'x':        /* --exclude */
                                elist_add(elist, optarg);
                                break;
                        case 'X':        /* --exclude-from */
                                elist_add_fromfile(elist, optarg);
                                break;
                        case 'd':        /* --days */
                                days = strtoul(optarg, NULL, 10);
                                if (days == 0) {
                                        fprintf(stderr, "%s: -d argument must "
                                                "be greater than zero\n", prog);
                                        exit(1);
                                }
                                break;
                        case 'o':        /* --outfile */
                                if (strcmp(optarg, "-") == 0)
                                        outf = stdout;
                                else if (!(outf = fopen(optarg, "a"))) {
                                        perror(optarg);
                                        exit(1);
                                }
                                break;
                        case 'i':        /* --inode-tallys */
                                if (strcmp(optarg, "-") == 0)
                                        tallyf = stdout;
                                else if (!(tallyf = fopen(optarg, "w+"))) {
                                        perror(optarg);
                                        exit(1);
                                }
                                break;
                        case '?':
                        case 'h':
                        default:
                                usage();
                }
        }
        if (optind != argc - 1)
                usage();

        /* must be root */
        if (geteuid() != 0) {
                fprintf(stderr, "%s: your effective uid must be root\n", prog);
                exit(1);
        }

        /* removal must be logged, and must have a threshold */
        if (Ropt && (!outf || days == 0)) {
                fprintf(stderr, "%s: with -R, you must also supply -o and -d\n",
                        prog);
                exit(1);
        }

        /* verify path */
        realpath(argv[optind], purgepath);
        if (*purgepath != '/') {
                fprintf(stderr, "%s: %s must be a fully qualified path\n",
                        prog, purgepath);
                exit(1);
        }
        if (stat(purgepath, &sb) < 0) {
                fprintf(stderr, "%s: cannot stat %s\n", prog, purgepath);
                exit(1);
        }
        if (!S_ISDIR(sb.st_mode)) {
                fprintf(stderr, "%s: %s is not a directory\n",
                        prog, purgepath);
                exit(1);
        }

        /* activate optimization if Lustre */
        if (is_lustre_fs(purgepath))
                lustre = 1;

        /* verify exception list and list it on stderr */
        elist_verify(elist, purgepath);

        /* some verbose messages at startup */
        if (Ropt) {
                if (isatty(0)) {
                        fprintf(stderr, "%s: WARNING removing files in %s\n",
                                prog, purgepath);
                        fprintf(stderr, "%s: you have 5s to abort...\n", prog);
                        sleep(5);
                }
                fprintf(stderr, "%s: will remove eligible files\n", prog);
        } else {
                fprintf(stderr, "%s: will not remove eligible files\n", prog);
        }
        if (days == 0)
                fprintf(stderr, "%s: no -d option, times will not be read\n",
                        prog);
        else if (lustre && sopt)
                fprintf(stderr, "%s: Lustre, but using stat(2) only \n",
                        prog);
        else if (lustre)
                fprintf(stderr, "%s: Lustre, using MDS-optimized stat\n",
                        prog);
        else
                fprintf(stderr, "%s: not Lustre, using stat(2)\n", prog);
        if (gettimeofday(&start, NULL) < 0) {
                perror("gettimeofday");
                exit(1);
        }

        /* do the work */
        if (days > 0)
                thresh = start.tv_sec - days * (24*60*60);
        chk_files = purge(purgepath, thresh, elist, outf, tallyf, Ropt,
                          sopt, lustre);

        /* clean up */
        if (outf && fclose(outf) != 0) {
                fprintf(stderr, "%s: error closing eligible log file\n", prog);
                exit_val = 1;
        }
        if (tallyf && fclose(tallyf) != 0) {
                fprintf(stderr, "%s: error closing inode-tally file\n", prog);
                exit_val = 1;
        }
        elist_destroy(elist);

        /* display stats */
        if (gettimeofday(&fin, NULL) < 0) {
                fprintf(stderr, "%s: gettimeofday: %s\n",prog, strerror(errno));
                fin.tv_sec = start.tv_sec + 1;
                exit_val = 1;
        }
        fprintf(stderr, "scanned %llu inodes at %.0lf/sec (%.0lf%% elig, %.3lfGB)\n",
                chk_files, (double)chk_files / (fin.tv_sec - start.tv_sec),
                ((double)elig_files / chk_files) * 100.0,
                (double)elig_bytes / (1024.0*1024*1024));

        min = (fin.tv_sec - start.tv_sec) / 60;
        fprintf(stderr, "elapsed time (hh:mm): %-2.2lu:%-2.2lu\n",
                min / 60, min % 60);

        exit(exit_val);
}

static void
oom(void)
{
        fprintf(stderr, "%s: out of memory\n", prog);
        exit(1);
}

static struct elist_struct *
elist_create(void)
{
        struct elist_struct *elist;

        if (!(elist = malloc(sizeof(struct elist_struct))))
                oom();
        elist->len = ELIST_CHUNK;
        if (!(elist->paths = malloc(elist->len * sizeof(char *))))
                oom();
        elist->count = 0;

        return elist;
}

static void
elist_destroy(struct elist_struct *elist)
{
        int i;

        for (i = 0; i < elist->count; i++)
                free(elist->paths[i]);
        free(elist->paths);
        free(elist);
}

static void
elist_grow(struct elist_struct *elist, int n)
{
        elist->len += n;
        if (!(elist->paths = realloc(elist->paths, elist->len*sizeof(char *))))
                oom();
}

static void
elist_add(struct elist_struct *elist, char *path)
{
        char pathtmp[PATH_MAX], *cpy;

        zap_trailing_chars(path, "\t\r\n ");
        realpath(path, pathtmp);
        if (*pathtmp != '/') {
                fprintf(stderr, "%s: %s not fully qualified\n", prog, pathtmp);
                exit(1);
        }
        if (!(cpy = strdup(pathtmp)))
                oom();
        if (elist->count  == elist->len)
                elist_grow(elist, ELIST_CHUNK);
        elist->paths[elist->count++] = cpy;
}

static void
elist_add_fromfile(struct elist_struct *elist, char *file)
{
        FILE *f = fopen(file, "r");
        char buf[PATH_MAX+64], *p;

        if (!file) {
                fprintf(stderr, "%s: %s: %s\n", prog, file, strerror(errno));
                exit(1);
        }
        while (fgets(buf, sizeof(buf), f) != NULL) {
                if ((p = strchr(buf, '#')))
                        *p = '\0';
                zap_trailing_chars(buf, " \t\r\n");
                if (strlen(buf) > 0)
                        elist_add(elist, buf);
        }
        if (ferror(f)) {
                fprintf(stderr, "%s: %s: %s\n", prog, file, strerror(errno));
                exit(1);
        }
        (void)fclose(f);
}

static int
elist_find(struct elist_struct *elist, const char *path)
{
        int i;

        for (i = 0; i < elist->count; i++)
                if (!strcmp(elist->paths[i], path))
                        return 1;
        return 0;
}

static void
elist_verify(struct elist_struct *elist, const char *purgepath)
{
        struct stat sb;
        int i;

        for (i = 0; i < elist->count; i++) {
                if (!strncmp(elist->paths[i], purgepath, strlen(purgepath))) {
                        if (stat(elist->paths[i], &sb) < 0) {
                                fprintf(stderr, "%s: cannot stat exclude"
                                        " path: %s (aborting)\n",
                                        prog, elist->paths[i]);
                                exit(1);
                        }
                        if (!S_ISDIR(sb.st_mode)) {
                                fprintf(stderr, "%s: exclude path is not a"
                                        " directory: %s (aborting)\n",
                                        prog, elist->paths[i]);
                                exit(1);
                        }
                        fprintf(stderr, "%s: excluding %s\n", prog,
                                elist->paths[i]);
                }
        }
}

static void
zap_trailing_chars(char *buf, char *chars)
{
        char *p = buf + strlen(buf) - 1;

        while (p >= buf && strchr(chars, *p))
                *p-- = '\0';
}

const char *
skip_leading_slashes(const char *buf)
{
        const char *p = buf;

        while (*p && *p == '/')
                p++;
        return p;
}

static char *
pathcat(const char *p1, const char *p2)
{
        int len = strlen(p1) + strlen(p2) + 2;
        char *new;

        if (!(new = malloc(len)))
                oom();
        strcpy(new, p1);
        zap_trailing_chars(new, "/");
        strcat(new, "/");
        strcat(new, skip_leading_slashes(p2));

        return new;
}

static time_t
maxtime(struct stat *sb, char *dp)
{
        time_t t = sb->st_atime;
        char d = 'a';

        if (sb->st_ctime > t) {
                t = sb->st_ctime;
                d = 'c';
        }
        if (sb->st_mtime > t) {
                t = sb->st_mtime;
                d = 'm';
        }
        if (dp)
                *dp = d;
        return t;
}

/* N.B.  Actual [acm]time (including OST info) is >= MDS [acm]time.
 * Therefore, if MDS [acm]time > thresh, then actual [acm]time > thresh;
 * however, if MDS [acm]time is < thresh, then we must stat(2).
 * N.B. Don't report an error if we are racing with unlink(2).
 */
static int
purgeable(int dfd, char *name, char *path, time_t thresh, int sopt,
          int lustre, time_t *tp, char *dp, off_t *sp, uid_t *up)
{
        struct stat sb1, sb2;
        time_t t;
        char d;
        int saved_errno = 0, lres = 0;

        if (!sopt && lustre) {
                if ((lres = llapi_stat_mds(dfd, name, &sb1)) < 0)
                        saved_errno = errno;
                else if (maxtime(&sb1, NULL) > thresh)
                        return 0;
        }
        if (lstat(path, &sb2) < 0)
                return 0;
        if (lres < 0)
                fprintf(stderr, "ioctl(IOC_MDC_GETFILEINFO): %s: %s\n",
                        path, strerror(saved_errno));
        if ((t = maxtime(&sb2, &d)) > thresh)
                return 0;

        *tp = t;
        *dp = d;
        *sp = sb2.st_size;
        *up = sb2.st_uid;
        return 1;
}

static char *
timestr(time_t *t)
{
        static char s[256];
        struct tm *tm = localtime(t);

        if (tm)
                snprintf(s, sizeof(s),
                        "%-4.4d-%-2.2d-%-2.2d-%-2.2d:%-2.2d:%-2.2d",
                        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                        tm->tm_hour, tm->tm_min, tm->tm_sec);
        else
                snprintf(s, sizeof(s), "time-conversion-error");

        return s;
}

static unsigned long long
purge(const char *path, time_t thresh, struct elist_struct *elist,
      FILE *outf, FILE *tallyf, int Ropt, int sopt, int lustre)
{
        char *fqp, d, *tmpdate;
        struct dirent *dp;
        DIR *dir;
        time_t t;
        off_t sz;
        uid_t uid;
        unsigned long long inode_count = 0, i;

        if (elist_find(elist, path))
                return 0;
        if (!(dir = opendir(path))) {
                fprintf(stderr, "%s: could not open %s\n", prog, path);
                return 0;
        }
        while ((dp = readdir(dir))) {
                if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
                        continue;
                inode_count++;
                fqp = pathcat(path, dp->d_name);
                if (dp->d_type == DT_DIR) { /* will not follow symlinks */
                        i = purge(fqp, thresh, elist, outf, NULL,
                                  Ropt, sopt, lustre);
                        inode_count += i;
                        if (tallyf)
                                fprintf(tallyf, "%9llu %s\n", i, fqp);
                } else if (thresh > 0) {
                        if (purgeable(dirfd(dir), dp->d_name, fqp, thresh,
                                      sopt, lustre, &t, &d, &sz, &uid)) {
                                elig_bytes += sz;
                                elig_files++;
                                /* report: {a|c|m} {date} {size} {uid} {path} */
                                if (outf) {
                                        tmpdate = timestr(&t);
                                        zap_trailing_chars(tmpdate, "\n");
                                        fprintf(outf, "%c %s %.3lf %lu %s\n",
                                                d, tmpdate,
                                                (double)sz/(1024*1024),
                                                (unsigned long)uid, fqp);
                                }
                                if (Ropt && unlink(fqp) < 0)
                                        fprintf(stderr, "%s: unlink %s: %s\n",
                                                prog, fqp, strerror(errno));
                        }
                }
                free(fqp);
        }
        closedir(dir);

        return inode_count;
}

static int
llapi_stat_mds(int fd, char *fname, struct stat *sb)
{
        const size_t bufsize = MAX(MAXPATHLEN, sizeof(lstat_t));
        char buf[bufsize];
        lstat_t *ls = (lstat_t *)buf;
        int ret = -1;

        if (strlen(fname) >= bufsize) {
                errno = EINVAL;
                goto done;
        }

        /* Usage: ioctl(fd, IOC_MDC_GETFILEINFO, buf)
         * IN:  fd   open file descriptor of file's parent directory
         * IN:  buf  file name (no path)
         * OUT: buf  lstat_t
         */
        strncpy(buf, fname, bufsize);
        if ((ret = ioctl(fd, IOC_MDC_GETFILEINFO, buf)) < 0)
                goto done;

        /* Copy 'lstat_t' to 'struct stat'
         */
        sb->st_dev     = ls->st_dev;
        sb->st_ino     = ls->st_ino;
        sb->st_mode    = ls->st_mode;
        sb->st_nlink   = ls->st_nlink;
        sb->st_uid     = ls->st_uid;
        sb->st_gid     = ls->st_gid;
        sb->st_rdev    = ls->st_rdev;
        sb->st_size    = ls->st_size;
        sb->st_blksize = ls->st_blksize;
        sb->st_blocks  = ls->st_blocks;
        sb->st_atime   = ls->st_atime;
        sb->st_mtime   = ls->st_mtime;
        sb->st_ctime   = ls->st_ctime;
done:
        return ret;
}

static int
is_lustre_fs(const char *path)
{
        struct statfs sfs;

        if (statfs(path, &sfs) < 0)
                return 0;
        if (sfs.f_type != LL_SUPER_MAGIC)
                return 0;
        return 1;
}
