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
 * Copyright (c) 2011 Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/tests/ll_dirstripe_verify.c
 *
 * ll_dirstripe_verify <dir> <file>:
 * - to verify if the file has the same lov_user_md setting as the parent dir.
 * - if dir's offset is set -1, ll_dirstripe_verify <dir> <file1> <file2>
 *      is used to further verify if file1 and file2's obdidx is continuous.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

#include <liblustre.h>
#include <obd.h>
#include <lustre_lib.h>
#include <lustre/liblustreapi.h>
#include <obd_lov.h>

#include <lnet/lnetctl.h>


#define MAX_LOV_UUID_COUNT      1000
union {
        struct obd_uuid uuid;
        char name[0];
} lov;
#define lov_uuid lov.uuid
#define lov_name lov.name

/* Returns bytes read on success and a negative value on failure.
 * If zero bytes are read it will be treated as failure as such
 * zero cannot be returned from this function.
 */
int read_proc_entry(char *proc_path, char *buf, int len)
{
        int rc, fd;

        memset(buf, 0, len);

        fd = open(proc_path, O_RDONLY);
        if (fd == -1) {
                llapi_error(LLAPI_MSG_ERROR, -errno, "open('%s') failed: %s\n",
                            proc_path);
                return -2;
        }

        rc = read(fd, buf, len - 1);
        if (rc < 0) {
                llapi_error(LLAPI_MSG_ERROR, -errno,
                            "read('%s') failed: %s\n", proc_path);
                rc = -3;
        } else if (rc == 0) {
                llapi_err_noerrno(LLAPI_MSG_ERROR,
                                  "read('%s') zero bytes\n", proc_path);
                rc = -4;
        } else if (/* rc > 0 && */ buf[rc - 1] == '\n') {
                buf[rc - 1] = '\0'; /* Remove trailing newline */
        }
        close(fd);

        return (rc);
}

int compare(struct lov_user_md *lum_dir, struct lov_user_md *lum_file1,
            struct lov_user_md *lum_file2)
{
        int stripe_count = 0, min_stripe_count = 0, def_stripe_count = 1;
        int stripe_size = 0;
        int stripe_offset = -1;
        int ost_count;
        char buf[128];
        char lov_path[PATH_MAX];
        char tmp_path[PATH_MAX];
        int i;
        FILE *fp;

        fp = popen("\\ls -d  /proc/fs/lustre/lov/*lov* | head -1", "r");
        if (!fp) {
                llapi_error(LLAPI_MSG_ERROR, -errno,
                            "open(lustre/lov/*lov*) failed: %s\n");
                return 2;
        }
        if (fscanf(fp, "%s", lov_path) < 1) {
                llapi_error(LLAPI_MSG_ERROR, -EINVAL,
                            "read lustre/lov/*lov* failed: %s\n");
                pclose(fp);
                return 3;
        }
        pclose(fp);

        snprintf(tmp_path, sizeof(tmp_path) - 1, "%s/stripecount", lov_path);
        if (read_proc_entry(tmp_path, buf, sizeof(buf)) < 0)
                return 5;
        def_stripe_count = (short)atoi(buf);

        snprintf(tmp_path, sizeof(tmp_path) - 1, "%s/numobd", lov_path);
        if (read_proc_entry(tmp_path, buf, sizeof(buf)) < 0)
                return 6;
        ost_count = atoi(buf);

        if (lum_dir == NULL) {
                stripe_count = def_stripe_count;
                min_stripe_count = -1;
        } else {
                stripe_count = (short)lum_dir->lmm_stripe_count;
                printf("dir stripe %d, ", stripe_count);
                min_stripe_count = 1;
        }

        printf("default stripe %d, ost count %d\n",
               def_stripe_count, ost_count);
        if (stripe_count == 0) {
                min_stripe_count = -1;
                stripe_count = 1;
        }

        stripe_count = (stripe_count > 0 && stripe_count <= ost_count) ?
                                                stripe_count : ost_count;
        min_stripe_count = min_stripe_count > 0 ? stripe_count :
                                                ((stripe_count + 1) / 2);

        if (lum_file1->lmm_stripe_count != stripe_count ||
            lum_file1->lmm_stripe_count < min_stripe_count) {
                llapi_err_noerrno(LLAPI_MSG_ERROR,
                                  "file1 stripe count %d != dir %d\n",
                                  lum_file1->lmm_stripe_count, stripe_count);
                return 7;
        }

        if (lum_file1->lmm_stripe_count < stripe_count)
                llapi_err_noerrno(LLAPI_MSG_WARN,
                                  "warning: file1 used fewer stripes "
                                  "%d < dir %d (likely due to bug 4900)\n",
                                  lum_file1->lmm_stripe_count, stripe_count);

        if (lum_dir != NULL)
                stripe_size = (int)lum_dir->lmm_stripe_size;
        if (stripe_size == 0) {
                snprintf(tmp_path, sizeof(tmp_path) - 1, "%s/stripesize",
                         lov_path);
                if (read_proc_entry(tmp_path, buf, sizeof(buf)) < 0)
                        return 5;

                stripe_size = atoi(buf);
        }

        if (lum_file1->lmm_stripe_size != stripe_size) {
                llapi_err_noerrno(LLAPI_MSG_ERROR,
                                  "file1 stripe size %d != dir %d\n",
                                  lum_file1->lmm_stripe_size, stripe_size);
                return 8;
        }

        if (lum_dir != NULL)
                stripe_offset = (short int)lum_dir->lmm_stripe_offset;
        if (stripe_offset != -1) {
                for (i = 0; i < stripe_count; i++)
                        if (lum_file1->lmm_objects[i].l_ost_idx !=
                            (stripe_offset + i) % ost_count) {
                                llapi_err_noerrno(LLAPI_MSG_WARN,
                                          "warning: file1 non-sequential "
                                          "stripe[%d] %d != %d\n", i,
                                          lum_file1->lmm_objects[i].l_ost_idx,
                                          (stripe_offset + i) % ost_count);
                        }
        } else if (lum_file2 != NULL) {
                int next, idx, stripe = stripe_count - 1;
                next = (lum_file1->lmm_objects[stripe].l_ost_idx + 1) %
                       ost_count;
                idx = lum_file2->lmm_objects[0].l_ost_idx;
                if (idx != next) {
                        llapi_err_noerrno(LLAPI_MSG_WARN,
                                  "warning: non-sequential "
                                  "file1 stripe[%d] %d != file2 stripe[0] %d\n",
                                  stripe,
                                  lum_file1->lmm_objects[stripe].l_ost_idx,
                                  idx);
                }
        }

        return 0;
}

int main(int argc, char **argv)
{
        DIR * dir;
        struct lov_user_md *lum_dir, *lum_file1 = NULL, *lum_file2 = NULL;
        int rc;
        int lum_size;

        if (argc < 3) {
                llapi_err_noerrno(LLAPI_MSG_ERROR,
                                "Usage: %s <dirname> <filename1> [filename2]\n",
                                argv[0]);
                return 1;
        }

        dir = opendir(argv[1]);
        if (dir == NULL) {
                rc = -errno;
                llapi_error(LLAPI_MSG_ERROR, rc,
                            "error: %s opendir failed\n", argv[1]);
                return rc;
        }

        lum_size = lov_mds_md_size(MAX_LOV_UUID_COUNT, LOV_MAGIC);
        if ((lum_dir = (struct lov_user_md *)malloc(lum_size)) == NULL) {
                rc = -ENOMEM;
                llapi_error(LLAPI_MSG_ERROR, rc,
                            "error: can't allocate %d bytes "
                            "for dir EA", lum_size);
                goto cleanup;
        }

        rc = llapi_file_get_stripe(argv[1], lum_dir);
        if (rc) {
                if (rc == -ENODATA) {
                        free(lum_dir);
                        lum_dir = NULL;
                } else {
                        llapi_error(LLAPI_MSG_ERROR, rc,
                                    "error: can't get EA for %s\n", argv[1]);
                        goto cleanup;
                }
        }

        /* XXX should be llapi_lov_getname() */
        rc = llapi_file_get_lov_uuid(argv[1], &lov_uuid);
        if (rc) {
                llapi_error(LLAPI_MSG_ERROR, rc,
                            "error: can't get lov name for %s\n",
                            argv[1]);
                return rc;
        }

        if ((lum_file1 = (struct lov_user_md *)malloc(lum_size)) == NULL) {
                rc = -ENOMEM;
                llapi_error(LLAPI_MSG_ERROR, rc,
                            "error: can't allocate %d bytes for EA\n",
                            lum_size);
                goto cleanup;
        }

        rc = llapi_file_get_stripe(argv[2], lum_file1);
        if (rc) {
                llapi_error(LLAPI_MSG_ERROR, rc,
                            "error: unable to get EA for %s\n", argv[2]);
                goto cleanup;
        }

        if (argc == 4) {
                lum_file2 = (struct lov_user_md *)malloc(lum_size);
                if (lum_file2 == NULL) {
                        rc = -ENOMEM;
                        llapi_error(LLAPI_MSG_ERROR, rc,
                                    "error: can't allocate %d "
                                    "bytes for file2 EA\n", lum_size);
                        goto cleanup;
                }

                rc = llapi_file_get_stripe(argv[3], lum_file2);
                if (rc) {
                        llapi_error(LLAPI_MSG_ERROR, rc,
                                    "error: can't get EA for %s\n", argv[3]);
                        goto cleanup;
                }
        }

        rc = compare(lum_dir, lum_file1, lum_file2);

cleanup:
        closedir(dir);
        if (lum_dir != NULL)
                free(lum_dir);
        if (lum_file1 != NULL)
                free(lum_file1);
        if (lum_file2 != NULL)
                free(lum_file2);

        return rc;
}
