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

#define PARENT_URN "urn:uuid:2bb5bdbf-6c4b-11dc-9b8e-080020a9ed93"
#define PARENT_PRODUCT "Lustre"

static int stclient(char *type, char *arch)
{

        char product[64];
        char *urn = NULL;
        char cmd[1024];
        FILE *fp;
        int i;

        if (strcmp(type, "Client") == 0)
                urn = CLIENT_URN;
        else if (strcmp(type, "MDS") == 0)
                urn = MDS_URN;
        else if (strcmp(type, "MGS") == 0)
                urn = MGS_URN;
        else if (strcmp(type, "OSS") == 0)
                urn = OSS_URN;

        snprintf(product, 64, "Lustre %s %d.%d.%d", type, LUSTRE_MAJOR,
                 LUSTRE_MINOR, LUSTRE_PATCH);

        /* need to see if the entry exists first */
        snprintf(cmd, 1024,
                 "/opt/sun/servicetag/bin/stclient -f -t '%s' ", urn);
        fp = popen(cmd, "r");
        if (!fp) {
                if (verbose)
                        fprintf(stderr, "%s: trying to run stclient -f: %s\n",
                                progname, strerror(errno));
                return 0;
        }

        i = fread(cmd, 1, sizeof(cmd) - 1, fp);
        if (i) {
                cmd[i] = 0;
                if (strcmp(cmd, "Record not found\n") != 0) {
                        /* exists, just return */
                        pclose(fp);
                        return 0;
                }
        }
        pclose(fp);

        snprintf(cmd, 1024, "/opt/sun/servicetag/bin/stclient -a -p '%s' "
                 "-e %d.%d.%d -t '%s' -S mount -F '%s' -P '%s' -m SUN "
                 "-A %s -z global", product, LUSTRE_MAJOR, LUSTRE_MINOR,
                 LUSTRE_PATCH, urn, PARENT_URN, PARENT_PRODUCT, arch);

        return(run_command(cmd, sizeof(cmd)));
}

void register_service_tags(char *usource, char *source, char *target)
{
        struct lustre_disk_data mo_ldd;
        struct utsname utsname_buf;
        struct stat stat_buf;
        char stclient_loc[] = "/opt/sun/servicetag/bin/stclient";
        int rc;

        rc = stat(stclient_loc, &stat_buf);

        if (rc) {
                if (errno != ENOENT && verbose) {
                        fprintf(stderr,
                                "%s: trying to stat stclient failed: %s\n",
                                progname, strerror(errno));
                }

                return;
        }

        /* call service tags stclient to show Lustre is in use on this system */
        rc = uname(&utsname_buf);
        if (rc) {
                if (verbose)
                        fprintf(stderr,
                                "%s: trying to get uname failed: %s, "
                                "inventory tags will not be created\n",
                                progname, strerror(errno));
                return;
        }

        /* client or server? */
        if (strchr(usource, ':')) {
                stclient("Client", utsname_buf.machine);
        } else {
                /* first figure what type of device it is */
                rc = get_mountdata(source, &mo_ldd);
                if (rc) {
                        if (verbose)
                                fprintf(stderr,
                                        "%s: trying to read mountdata from %s "
                                        "failed: %s, inventory tags will not "
                                        "be created\n",
                                        progname, target, strerror(errno));
                        return;
                }

                if (IS_MDT(&mo_ldd))
                        stclient("MDS", utsname_buf.machine);

                if (IS_MGS(&mo_ldd))
                        stclient("MGS", utsname_buf.machine);

                if (IS_OST(&mo_ldd))
                        stclient("OSS", utsname_buf.machine);
        }
}
