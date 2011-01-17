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
 * Copyright (c) 2012 Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#if HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <config.h>
#include <lustre_disk.h>
#include <lustre_ver.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include "mount_utils.h"

extern char *progname;
extern int verbose;

#define vprint(fmt, arg...) if (verbose > 0) printf(fmt, ##arg)
#define verrprint(fmt, arg...) if (verbose >= 0) fprintf(stderr, fmt, ##arg)

void fatal(void)
{
        verbose = 0;
        fprintf(stderr, "\n%s FATAL: ", progname);
}

int run_command_err(char *cmd, int cmdsz, char *error_msg)
{
        char log[] = "/tmp/run_command_logXXXXXX";
        int fd = -1, rc;

        if ((cmdsz - strlen(cmd)) < 6) {
                fatal();
                fprintf(stderr, "Command buffer overflow: %.*s...\n",
                        cmdsz, cmd);
                return ENOMEM;
        }

        if (verbose > 1)
                printf("cmd: %s\n", cmd);

        if ((fd = mkstemp(log)) >= 0) {
                close(fd);
                strcat(cmd, " >");
                strcat(cmd, log);
        }
        strcat(cmd, " 2>&1");

        /* Can't use popen because we need the rv of the command */
        rc = system(cmd);
        if (rc && (fd >= 0)) {
                char buf[256];

                if (error_msg != NULL) {
                        if (snprintf(buf, sizeof(buf), "grep -q \"%s\" %s",
                                     error_msg, log) >= sizeof(buf)) {
                                fatal();
                                buf[sizeof(buf) - 1] = '\0';
                                fprintf(stderr, "grep command buf overflow: "
                                        "'%s'\n", buf);
                                return ENOMEM;
                        }
                        if (system(buf) == 0) {
                                /* The command had the expected error */
                                rc = -2;
                                goto out;
                        }
                }

                FILE *fp;
                fp = fopen(log, "r");
                if (fp) {
                        if (verbose <= 1)
                                printf("cmd: %s\n", cmd);

                        while (fgets(buf, sizeof(buf), fp) != NULL)
                                printf("   %s", buf);

                        fclose(fp);
                }
        }
out:
        if (fd >= 0)
                remove(log);
        return rc;
}

int run_command(char *cmd, int cmdsz)
{
        return run_command_err(cmd, cmdsz, NULL);
}

static int readcmd(char *cmd, char *buf, int len)
{
        FILE *fp;
        int red;

        fp = popen(cmd, "r");
        if (!fp)
                return errno;

        red = fread(buf, 1, len, fp);
        pclose(fp);

        /* strip trailing newline */
        if (buf[red - 1] == '\n')
                buf[red - 1] = '\0';

        return (red == 0) ? -ENOENT : 0;
}

int get_mountdata(char *dev, struct lustre_disk_data *mo_ldd)
{

        char tmpdir[] = "/tmp/lustre_tmp.XXXXXX";
        char cmd[256];
        char filepnm[128];
        FILE *filep;
        int ret = 0;
        int ret2 = 0;
        int cmdsz = sizeof(cmd);

        /* Make a temporary directory to hold Lustre data files. */
        if (!mkdtemp(tmpdir)) {
                verrprint("%s: Can't create temporary directory %s: %s\n",
                        progname, tmpdir, strerror(errno));
                return errno;
        }

        snprintf(cmd, cmdsz, "%s -c -R 'dump /%s %s/mountdata' %s",
                 DEBUGFS, MOUNT_DATA_FILE, tmpdir, dev);

        ret = run_command(cmd, cmdsz);
        if (ret) {
                verrprint("%s: Unable to dump %s from %s (%d)\n",
                          progname, MOUNT_DATA_FILE, dev, ret);
                goto out_rmdir;
        }

        sprintf(filepnm, "%s/mountdata", tmpdir);
        filep = fopen(filepnm, "r");
        if (filep) {
                size_t num_read;
                vprint("Reading %s\n", MOUNT_DATA_FILE);
                num_read = fread(mo_ldd, sizeof(*mo_ldd), 1, filep);
                if (num_read < 1 && ferror(filep)) {
                        fprintf(stderr, "%s: Unable to read from file (%s): %s\n",
                                progname, filepnm, strerror(errno));
                        goto out_close;
                }
        } else {
                verrprint("%s: Unable to open temp data file %s\n",
                          progname, filepnm);
                ret = 1;
                goto out_rmdir;
        }

out_close:
        fclose(filep);

out_rmdir:
        snprintf(cmd, cmdsz, "rm -rf %s", tmpdir);
        ret2 = run_command(cmd, cmdsz);
        if (ret2)
                verrprint("Failed to remove temp dir %s (%d)\n", tmpdir, ret2);

        /* As long as we at least have the label, we're good to go */
        snprintf(cmd, sizeof(cmd), E2LABEL" %s", dev);
        ret = readcmd(cmd, mo_ldd->ldd_svname, sizeof(mo_ldd->ldd_svname) - 1);

        return ret;
}

/* Convert symbolic hostnames to ipaddrs, since we can't do this lookup in the
 * kernel. */
#define MAXNIDSTR 1024
char *convert_hostnames(char *s1)
{
        char *converted, *s2 = 0, *c, *end, sep;
        int left = MAXNIDSTR;
        lnet_nid_t nid;

        converted = malloc(left);
        if (converted == NULL) {
                return NULL;
        }

        end = s1 + strlen(s1);
        c = converted;
        while ((left > 0) && (s1 < end)) {
                s2 = strpbrk(s1, ",:");
                if (!s2)
                        s2 = end;
                sep = *s2;
                *s2 = '\0';
                nid = libcfs_str2nid(s1);

                if (nid == LNET_NID_ANY) {
                        fprintf(stderr, "%s: Can't parse NID '%s'\n", progname, s1);
                        free(converted);
                        return NULL;
                }
                if (strncmp(libcfs_nid2str(nid), "127.0.0.1",
                            strlen("127.0.0.1")) == 0) {
                        fprintf(stderr, "%s: The NID '%s' resolves to the "
                                "loopback address '%s'.  Lustre requires a "
                                "non-loopback address.\n",
                                progname, s1, libcfs_nid2str(nid));
                        free(converted);
                        return NULL;
                }

                c += snprintf(c, left, "%s%c", libcfs_nid2str(nid), sep);
                left = converted + MAXNIDSTR - c;
                s1 = s2 + 1;
        }
        return converted;
}

