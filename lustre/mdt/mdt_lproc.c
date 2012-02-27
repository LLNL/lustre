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
 * lustre/mdt/mdt_lproc.c
 *
 * Author: Lai Siyao <lsy@clusterfs.com>
 * Author: Fan Yong <fanyong@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/version.h>
#include <asm/statfs.h>

#include <linux/module.h>

/* LUSTRE_VERSION_CODE */
#include <lustre_ver.h>
/*
 * struct OBD_{ALLOC,FREE}*()
 * MDT_FAIL_CHECK
 */
#include <obd_support.h>
/* struct obd_export */
#include <lustre_export.h>
/* struct obd_device */
#include <obd.h>
#include <obd_class.h>
#include <lustre_mds.h>
#include <lustre_mdt.h>
#include <lprocfs_status.h>
#include <lu_time.h>
#include "mdt_internal.h"
#include <lnet/lib-lnet.h>

enum {
        LPROC_MDT_NR
};
static const char *mdt_proc_names[LPROC_MDT_NR] = {
};

int mdt_procfs_init(struct mdt_device *mdt, const char *name)
{
        struct lu_device *ld = &mdt->mdt_md_dev.md_lu_dev;
        struct obd_device *obd = ld->ld_obd;
        struct lprocfs_static_vars lvars;
        int rc;
        ENTRY;

        LASSERT(name != NULL);

        lprocfs_mdt_init_vars(&lvars);
        rc = lprocfs_obd_setup(obd, lvars.obd_vars);
        if (rc) {
                CERROR("Can't init lprocfs, rc %d\n", rc);
                return rc;
        }
        ptlrpc_lprocfs_register_obd(obd);

        mdt->mdt_proc_entry = obd->obd_proc_entry;
        LASSERT(mdt->mdt_proc_entry != NULL);

        rc = lu_time_init(&mdt->mdt_stats, mdt->mdt_proc_entry,
                          mdt_proc_names, ARRAY_SIZE(mdt_proc_names));
        if (rc == 0)
                rc = lu_time_named_init(&ld->ld_site->ls_time_stats,
                                        "site_time", mdt->mdt_proc_entry,
                                         lu_time_names,
                                         ARRAY_SIZE(lu_time_names));
        if (rc)
                return rc;

        obd->obd_proc_exports_entry = proc_mkdir("exports",
                                                 obd->obd_proc_entry);
        if (obd->obd_proc_exports_entry)
                lprocfs_add_simple(obd->obd_proc_exports_entry,
                                   "clear", lprocfs_nid_stats_clear_read,
                                   lprocfs_nid_stats_clear_write, obd, NULL);
        rc = lprocfs_alloc_md_stats(obd, LPROC_MDT_LAST);
        if (rc == 0)
                mdt_stats_counter_init(obd->md_stats);

        RETURN(rc);
}

int mdt_procfs_fini(struct mdt_device *mdt)
{
        struct lu_device *ld = &mdt->mdt_md_dev.md_lu_dev;
        struct obd_device *obd = ld->ld_obd;

        if (obd->obd_proc_exports_entry) {
                lprocfs_remove_proc_entry("clear", obd->obd_proc_exports_entry);
                obd->obd_proc_exports_entry = NULL;
        }
        lprocfs_free_per_client_stats(obd);
        lprocfs_obd_cleanup(obd);
        ptlrpc_lprocfs_unregister_obd(obd);
        if (mdt->mdt_proc_entry) {
                lu_time_fini(&ld->ld_site->ls_time_stats);
                lu_time_fini(&mdt->mdt_stats);
                mdt->mdt_proc_entry = NULL;
        }
        lprocfs_free_md_stats(obd);
        lprocfs_free_obd_stats(obd);

        RETURN(0);
}

void mdt_time_start(const struct mdt_thread_info *info)
{
        lu_lprocfs_time_start(info->mti_env);
}

void mdt_time_end(const struct mdt_thread_info *info, int idx)
{
        lu_lprocfs_time_end(info->mti_env, info->mti_mdt->mdt_stats, idx);
}

static int lprocfs_rd_identity_expire(char *page, char **start, off_t off,
                                      int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        *eof = 1;
        return snprintf(page, count, "%u\n",
                        mdt->mdt_identity_cache->uc_entry_expire);
}

static int lprocfs_wr_identity_expire(struct file *file, const char *buffer,
                                      unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int rc, val;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        mdt->mdt_identity_cache->uc_entry_expire = val;
        return count;
}

static int lprocfs_rd_identity_acquire_expire(char *page, char **start,
                                              off_t off, int count, int *eof,
                                              void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        *eof = 1;
        return snprintf(page, count, "%u\n",
                        mdt->mdt_identity_cache->uc_acquire_expire);
}

static int lprocfs_wr_identity_acquire_expire(struct file *file,
                                              const char *buffer,
                                              unsigned long count,
                                              void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int rc, val;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        mdt->mdt_identity_cache->uc_acquire_expire = val;
        return count;
}

static int lprocfs_rd_identity_upcall(char *page, char **start, off_t off,
                                      int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        struct upcall_cache *hash = mdt->mdt_identity_cache;
        int len;

        *eof = 1;
        cfs_read_lock(&hash->uc_upcall_rwlock);
        len = snprintf(page, count, "%s\n", hash->uc_upcall);
        cfs_read_unlock(&hash->uc_upcall_rwlock);
        return len;
}

static int lprocfs_wr_identity_upcall(struct file *file, const char *buffer,
                                      unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        struct upcall_cache *hash = mdt->mdt_identity_cache;
        int rc;
        char *kernbuf;

        if (count >= UC_CACHE_UPCALL_MAXPATH) {
                CERROR("%s: identity upcall too long\n", obd->obd_name);
                return -EINVAL;
        }
        OBD_ALLOC(kernbuf, count + 1);
        if (kernbuf == NULL)
                GOTO(failed, rc = -ENOMEM);
        if (cfs_copy_from_user(kernbuf, buffer, count))
                GOTO(failed, rc = -EFAULT);

        /* Remove any extraneous bits from the upcall (e.g. linefeeds) */
        cfs_write_lock(&hash->uc_upcall_rwlock);
        sscanf(kernbuf, "%s", hash->uc_upcall);
        cfs_write_unlock(&hash->uc_upcall_rwlock);

        if (strcmp(hash->uc_name, obd->obd_name) != 0)
                CWARN("%s: write to upcall name %s\n",
                      obd->obd_name, hash->uc_upcall);

        if (strcmp(hash->uc_upcall, "NONE") == 0 && mdt->mdt_opts.mo_acl)
                CWARN("%s: disable \"identity_upcall\" with ACL enabled maybe "
                      "cause unexpected \"EACCESS\"\n", obd->obd_name);

        CWARN("%s: identity upcall set to %s\n", obd->obd_name, hash->uc_upcall);
        OBD_FREE(kernbuf, count + 1);
        RETURN(count);

 failed:
        if (kernbuf)
                OBD_FREE(kernbuf, count + 1);
        RETURN(rc);
}

static int lprocfs_wr_identity_flush(struct file *file, const char *buffer,
                                     unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int rc, uid;

        rc = lprocfs_write_helper(buffer, count, &uid);
        if (rc)
                return rc;

        mdt_flush_identity(mdt->mdt_identity_cache, uid);
        return count;
}

static int lprocfs_wr_identity_info(struct file *file, const char *buffer,
                                    unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        struct identity_downcall_data *param;
        int size = sizeof(*param), rc, checked = 0;

again:
        if (count < size) {
                CERROR("%s: invalid data count = %lu, size = %d\n",
                       obd->obd_name, count, size);
                return -EINVAL;
        }

        OBD_ALLOC(param, size);
        if (param == NULL)
                return -ENOMEM;

        if (cfs_copy_from_user(param, buffer, size)) {
                CERROR("%s: bad identity data\n", obd->obd_name);
                GOTO(out, rc = -EFAULT);
        }

        if (checked == 0) {
                checked = 1;
                if (param->idd_magic != IDENTITY_DOWNCALL_MAGIC) {
                        CERROR("%s: MDS identity downcall bad params\n",
                               obd->obd_name);
                        GOTO(out, rc = -EINVAL);
                }

                if (param->idd_nperms > N_PERMS_MAX) {
                        CERROR("%s: perm count %d more than maximum %d\n",
                               obd->obd_name, param->idd_nperms, N_PERMS_MAX);
                        GOTO(out, rc = -EINVAL);
                }

                if (param->idd_ngroups > NGROUPS_MAX) {
                        CERROR("%s: group count %d more than maximum %d\n",
                               obd->obd_name, param->idd_ngroups, NGROUPS_MAX);
                        GOTO(out, rc = -EINVAL);
                }

                if (param->idd_ngroups) {
                        rc = param->idd_ngroups; /* save idd_ngroups */
                        OBD_FREE(param, size);
                        size = offsetof(struct identity_downcall_data,
                                        idd_groups[rc]);
                        goto again;
                }
        }

        rc = upcall_cache_downcall(mdt->mdt_identity_cache, param->idd_err,
                                   param->idd_uid, param);

out:
        if (param != NULL)
                OBD_FREE(param, size);

        return rc ? rc : count;
}

/* for debug only */
static int lprocfs_rd_capa(char *page, char **start, off_t off,
                           int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        return snprintf(page, count, "capability on: %s %s\n",
                        mdt->mdt_opts.mo_oss_capa ? "oss" : "",
                        mdt->mdt_opts.mo_mds_capa ? "mds" : "");
}

static int lprocfs_wr_capa(struct file *file, const char *buffer,
                           unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val < 0 || val > 3) {
                CERROR("invalid capability mode, only 0/2/3 is accepted.\n"
                       " 0:  disable fid capability\n"
                       " 2:  enable MDS fid capability\n"
                       " 3:  enable both MDS and OSS fid capability\n");
                return -EINVAL;
        }

        /* OSS fid capability needs enable both MDS and OSS fid capability on
         * MDS */
        if (val == 1) {
                CERROR("can't enable OSS fid capability only, you should use "
                       "'3' to enable both MDS and OSS fid capability.\n");
                return -EINVAL;
        }

        mdt->mdt_opts.mo_oss_capa = (val & 0x1);
        mdt->mdt_opts.mo_mds_capa = !!(val & 0x2);
        mdt->mdt_capa_conf = 1;
        LCONSOLE_INFO("MDS %s %s MDS fid capability.\n",
                      obd->obd_name,
                      mdt->mdt_opts.mo_mds_capa ? "enabled" : "disabled");
        LCONSOLE_INFO("MDS %s %s OSS fid capability.\n",
                      obd->obd_name,
                      mdt->mdt_opts.mo_oss_capa ? "enabled" : "disabled");
        return count;
}

static int lprocfs_rd_capa_count(char *page, char **start, off_t off,
                                 int count, int *eof, void *data)
{
        return snprintf(page, count, "%d %d\n",
                        capa_count[CAPA_SITE_CLIENT],
                        capa_count[CAPA_SITE_SERVER]);
}

static int lprocfs_rd_site_stats(char *page, char **start, off_t off,
                                 int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        return lu_site_stats_print(mdt_lu_site(mdt), page, count);
}

static int lprocfs_rd_capa_timeout(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        return snprintf(page, count, "%lu\n", mdt->mdt_capa_timeout);
}

static int lprocfs_wr_capa_timeout(struct file *file, const char *buffer,
                                   unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        mdt->mdt_capa_timeout = (unsigned long)val;
        mdt->mdt_capa_conf = 1;
        return count;
}

static int lprocfs_rd_ck_timeout(char *page, char **start, off_t off, int count,
                                 int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        return snprintf(page, count, "%lu\n", mdt->mdt_ck_timeout);
}

static int lprocfs_wr_ck_timeout(struct file *file, const char *buffer,
                                 unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        mdt->mdt_ck_timeout = (unsigned long)val;
        mdt->mdt_capa_conf = 1;
        return count;
}

static int lprocfs_mdt_wr_evict_client(struct file *file, const char *buffer,
                                       unsigned long count, void *data)
{
        char tmpbuf[sizeof(struct obd_uuid)];

        sscanf(buffer, "%40s", tmpbuf);

        if (strncmp(tmpbuf, "nid:", 4) != 0)
                return lprocfs_wr_evict_client(file, buffer, count, data);

        CERROR("NOT implement evict client by nid %s\n", tmpbuf);

        return count;
}

static int lprocfs_rd_sec_level(char *page, char **start, off_t off,
                                int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        return snprintf(page, count, "%d\n", mdt->mdt_sec_level);
}

static int lprocfs_wr_sec_level(struct file *file, const char *buffer,
                                unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        if (val > LUSTRE_SEC_ALL || val < LUSTRE_SEC_NONE)
                return -EINVAL;

        if (val == LUSTRE_SEC_SPECIFY) {
                CWARN("security level %d will be supported in future.\n",
                      LUSTRE_SEC_SPECIFY);
                return -EINVAL;
        }

        mdt->mdt_sec_level = val;
        return count;
}

static int lprocfs_rd_cos(char *page, char **start, off_t off,
                              int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        return snprintf(page, count, "%u\n", mdt_cos_is_enabled(mdt));
}

static int lprocfs_wr_cos(struct file *file, const char *buffer,
                                  unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int val, rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;
        mdt_enable_cos(mdt, val);
        return count;
}

static int lprocfs_rd_root_squash(char *page, char **start, off_t off,
                                  int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        return snprintf(page, count, "%u:%u\n", mdt->mdt_squash_uid,
                        mdt->mdt_squash_gid);
}

static int safe_strtoul(const char *str, char **endp, unsigned long *res)
{
        char n[24];

        *res = simple_strtoul(str, endp, 0);
        if (str == *endp)
                return 1;

        sprintf(n, "%lu", *res);
        if (strncmp(n, str, *endp - str))
                /* overflow */
                return 1;
        return 0;
}

static int lprocfs_wr_root_squash(struct file *file, const char *buffer,
                                  unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int rc;
        char kernbuf[50], *tmp, *end, *errmsg;
        unsigned long uid, gid;
        int nouid, nogid;
        ENTRY;

        if (count >= sizeof(kernbuf)) {
                errmsg = "string too long";
                GOTO(failed, rc = -EINVAL);
        }
        if (cfs_copy_from_user(kernbuf, buffer, count)) {
                errmsg = "bad address";
                GOTO(failed, rc = -EFAULT);
        }
        kernbuf[count] = '\0';

        nouid = nogid = 0;
        if (safe_strtoul(buffer, &tmp, &uid)) {
                uid = mdt->mdt_squash_uid;
                nouid = 1;
        }

        /* skip ':' */
        if (*tmp == ':') {
                tmp++;
                if (safe_strtoul(tmp, &end, &gid)) {
                        gid = mdt->mdt_squash_gid;
                        nogid = 1;
                }
        } else {
                gid = mdt->mdt_squash_gid;
                nogid = 1;
        }

        mdt->mdt_squash_uid = uid;
        mdt->mdt_squash_gid = gid;

        if (nouid && nogid) {
                errmsg = "needs uid:gid format";
                GOTO(failed, rc = -EINVAL);
        }

        LCONSOLE_INFO("%s: root_squash is set to %u:%u\n",
                      obd->obd_name,
                      mdt->mdt_squash_uid,  mdt->mdt_squash_gid);
        RETURN(count);

 failed:
        CWARN("%s: failed to set root_squash to \"%s\", %s: rc %d\n",
              obd->obd_name, buffer, errmsg, rc);
        RETURN(rc);
}

static int lprocfs_rd_nosquash_nids(char *page, char **start, off_t off,
                                    int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        if (mdt->mdt_nosquash_str)
                return snprintf(page, count, "%s\n", mdt->mdt_nosquash_str);
        return snprintf(page, count, "NONE\n");
}

static int lprocfs_wr_nosquash_nids(struct file *file, const char *buffer,
                                    unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        int rc;
        char *kernbuf, *errmsg;
        cfs_list_t tmp;
        ENTRY;

        OBD_ALLOC(kernbuf, count + 1);
        if (kernbuf == NULL) {
                errmsg = "no memory";
                GOTO(failed, rc = -ENOMEM);
        }
        if (cfs_copy_from_user(kernbuf, buffer, count)) {
                errmsg = "bad address";
                GOTO(failed, rc = -EFAULT);
        }
        kernbuf[count] = '\0';

        if (!strcmp(kernbuf, "NONE") || !strcmp(kernbuf, "clear")) {
                /* empty string is special case */
                cfs_down_write(&mdt->mdt_squash_sem);
                if (!cfs_list_empty(&mdt->mdt_nosquash_nids)) {
                        cfs_free_nidlist(&mdt->mdt_nosquash_nids);
                        OBD_FREE(mdt->mdt_nosquash_str,
                                 mdt->mdt_nosquash_strlen);
                        mdt->mdt_nosquash_str = NULL;
                        mdt->mdt_nosquash_strlen = 0;
                }
                cfs_up_write(&mdt->mdt_squash_sem);
                LCONSOLE_INFO("%s: nosquash_nids is cleared\n",
                              obd->obd_name);
                OBD_FREE(kernbuf, count + 1);
                RETURN(count);
        }

        CFS_INIT_LIST_HEAD(&tmp);
        if (cfs_parse_nidlist(kernbuf, count, &tmp) <= 0) {
                errmsg = "can't parse";
                GOTO(failed, rc = -EINVAL);
        }

        cfs_down_write(&mdt->mdt_squash_sem);
        if (!cfs_list_empty(&mdt->mdt_nosquash_nids)) {
                cfs_free_nidlist(&mdt->mdt_nosquash_nids);
                OBD_FREE(mdt->mdt_nosquash_str, mdt->mdt_nosquash_strlen);
        }
        mdt->mdt_nosquash_str = kernbuf;
        mdt->mdt_nosquash_strlen = count + 1;
        cfs_list_splice(&tmp, &mdt->mdt_nosquash_nids);

        LCONSOLE_INFO("%s: nosquash_nids is set to %s\n",
                      obd->obd_name, kernbuf);
        cfs_up_write(&mdt->mdt_squash_sem);
        RETURN(count);

 failed:
        CWARN("%s: failed to set nosquash_nids to \"%s\", %s: rc %d\n",
              obd->obd_name, kernbuf, errmsg, rc);
        if (kernbuf)
                OBD_FREE(kernbuf, count + 1);
        RETURN(rc);
}

static int lprocfs_rd_mdt_som(char *page, char **start, off_t off,
                              int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);

        return snprintf(page, count, "%sabled\n",
                        mdt->mdt_som_conf ? "en" : "dis");
}

static int lprocfs_wr_mdt_som(struct file *file, const char *buffer,
                              unsigned long count, void *data)
{
        struct obd_export *exp;
        struct obd_device *obd = data;
        struct mdt_device *mdt = mdt_dev(obd->obd_lu_dev);
        char kernbuf[16];
        unsigned long val = 0;

        if (count > (sizeof(kernbuf) - 1))
                return -EINVAL;

        if (cfs_copy_from_user(kernbuf, buffer, count))
                return -EFAULT;

        kernbuf[count] = '\0';

        if (!strcmp(kernbuf, "enabled"))
                val = 1;
        else if (strcmp(kernbuf, "disabled"))
                return -EINVAL;

        if (mdt->mdt_som_conf == val)
                return count;

        if (!obd->obd_process_conf) {
                CERROR("Temporary SOM change is not supported, use lctl "
                       "conf_param for permanent setting\n");
                return count;
        }

        /* 1 stands for self export. */
        cfs_list_for_each_entry(exp, &obd->obd_exports, exp_obd_chain) {
                if (exp == obd->obd_self_export)
                        continue;
                if (exp->exp_connect_flags & OBD_CONNECT_MDS_MDS)
                        continue;
                /* Some clients are already connected, skip the change */
                LCONSOLE_INFO("%s is already connected, SOM will be %s on "
                              "the next mount\n", exp->exp_client_uuid.uuid,
                              val ? "enabled" : "disabled");
                return count;
        }

        mdt->mdt_som_conf = val;
        LCONSOLE_INFO("Enabling SOM\n");

        return count;
}

/* Temporary; for testing purposes only */
static int lprocfs_mdt_wr_mdc(struct file *file, const char *buffer,
                              unsigned long count, void *data)
{
        struct obd_device *obd = data;
        struct obd_export *exp = NULL;
        struct obd_uuid uuid;
        char tmpbuf[sizeof(struct obd_uuid)];

        sscanf(buffer, "%40s", tmpbuf);

        obd_str2uuid(&uuid, tmpbuf);
        exp = cfs_hash_lookup(obd->obd_uuid_hash, &uuid);
        if (exp == NULL) {
                CERROR("%s: no export %s found\n",
                       obd->obd_name, obd_uuid2str(&uuid));
        } else {
                mdt_hsm_copytool_send(exp);
                class_export_put(exp);
        }

        return count;
}

static struct lprocfs_vars lprocfs_mdt_obd_vars[] = {
        { "uuid",                       lprocfs_rd_uuid,                 0, 0 },
        { "recovery_status",            lprocfs_obd_rd_recovery_status,  0, 0 },
        { "num_exports",                lprocfs_rd_num_exports,          0, 0 },
        { "identity_expire",            lprocfs_rd_identity_expire,
                                        lprocfs_wr_identity_expire,         0 },
        { "identity_acquire_expire",    lprocfs_rd_identity_acquire_expire,
                                        lprocfs_wr_identity_acquire_expire, 0 },
        { "identity_upcall",            lprocfs_rd_identity_upcall,
                                        lprocfs_wr_identity_upcall,         0 },
        { "identity_flush",             0, lprocfs_wr_identity_flush,       0 },
        { "identity_info",              0, lprocfs_wr_identity_info,        0 },
        { "capa",                       lprocfs_rd_capa,
                                        lprocfs_wr_capa,                    0 },
        { "capa_timeout",               lprocfs_rd_capa_timeout,
                                        lprocfs_wr_capa_timeout,            0 },
        { "capa_key_timeout",           lprocfs_rd_ck_timeout,
                                        lprocfs_wr_ck_timeout,              0 },
        { "capa_count",                 lprocfs_rd_capa_count,           0, 0 },
        { "site_stats",                 lprocfs_rd_site_stats,           0, 0 },
        { "evict_client",               0, lprocfs_mdt_wr_evict_client,     0 },
        { "hash_stats",                 lprocfs_obd_rd_hash,    0, 0 },
        { "sec_level",                  lprocfs_rd_sec_level,
                                        lprocfs_wr_sec_level,               0 },
        { "commit_on_sharing",          lprocfs_rd_cos, lprocfs_wr_cos, 0 },
        { "root_squash",                lprocfs_rd_root_squash,
                                        lprocfs_wr_root_squash,             0 },
        { "nosquash_nids",              lprocfs_rd_nosquash_nids,
                                        lprocfs_wr_nosquash_nids,           0 },
        { "som",                        lprocfs_rd_mdt_som,
                                        lprocfs_wr_mdt_som, 0 },
        { "mdccomm",                    0, lprocfs_mdt_wr_mdc,              0 },
        { 0 }
};

static struct lprocfs_vars lprocfs_mdt_module_vars[] = {
        { "num_refs",                   lprocfs_rd_numrefs,              0, 0 },
        { 0 }
};

void lprocfs_mdt_init_vars(struct lprocfs_static_vars *lvars)
{
    lvars->module_vars  = lprocfs_mdt_module_vars;
    lvars->obd_vars     = lprocfs_mdt_obd_vars;
}

void mdt_counter_incr(struct obd_export *exp, int opcode)
{
        if (exp->exp_obd && exp->exp_obd->md_stats)
                lprocfs_counter_incr(exp->exp_obd->md_stats, opcode);
        if (exp->exp_nid_stats && exp->exp_nid_stats->nid_stats != NULL)
                lprocfs_counter_incr(exp->exp_nid_stats->nid_stats, opcode);

}

void mdt_stats_counter_init(struct lprocfs_stats *stats)
{
        lprocfs_counter_init(stats, LPROC_MDT_OPEN, 0, "open", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_CLOSE, 0, "close", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_MKNOD, 0, "mknod", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_LINK, 0, "link", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_UNLINK, 0, "unlink", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_MKDIR, 0, "mkdir", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_RMDIR, 0, "rmdir", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_RENAME, 0, "rename", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_GETATTR, 0, "getattr", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_SETATTR, 0, "setattr", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_GETXATTR, 0, "getxattr", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_SETXATTR, 0, "setxattr", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_STATFS, 0, "statfs", "reqs");
        lprocfs_counter_init(stats, LPROC_MDT_SYNC, 0, "sync", "reqs");
}
