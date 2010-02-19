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
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdclass/lprocfs_status.c
 *
 * Author: Hariharan Thantry <thantry@users.sourceforge.net>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_CLASS

#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <obd_class.h>
#include <lprocfs_status.h>
#include <lustre_fsfilt.h>

#if defined(LPROCFS)

#define MAX_STRING_SIZE 128

/* for bug 10866, global variable */
DECLARE_RWSEM(_lprocfs_lock);
EXPORT_SYMBOL(_lprocfs_lock);

int lprocfs_seq_release(struct inode *inode, struct file *file)
{
        LPROCFS_EXIT();
        return seq_release(inode, file);
}
EXPORT_SYMBOL(lprocfs_seq_release);

struct proc_dir_entry *lprocfs_srch(struct proc_dir_entry *head,
                                    const char *name)
{
        struct proc_dir_entry *temp;

        if (head == NULL)
                return NULL;

        LPROCFS_SRCH_ENTRY();
        temp = head->subdir;
        while (temp != NULL) {
                if (strcmp(temp->name, name) == 0) {
                        LPROCFS_SRCH_EXIT();
                        return temp;
                }

                temp = temp->next;
        }
        LPROCFS_SRCH_EXIT();
        return NULL;
}

/* lprocfs API calls */

/* Function that emulates snprintf but also has the side effect of advancing
   the page pointer for the next write into the buffer, incrementing the total
   length written to the buffer, and decrementing the size left in the
   buffer. */
static int lprocfs_obd_snprintf(char **page, int end, int *len,
                                const char *format, ...)
{
        va_list list;
        int n;

        if (*len >= end)
                return 0;

        va_start(list, format);
        n = vsnprintf(*page, end - *len, format, list);
        va_end(list);

        *page += n; *len += n;
        return n;
}

cfs_proc_dir_entry_t *lprocfs_add_simple(struct proc_dir_entry *root,
                                        char *name,
                                        read_proc_t *read_proc,
                                        write_proc_t *write_proc,
                                        void *data,
                                        struct file_operations *fops)
{
        cfs_proc_dir_entry_t *proc;
        mode_t mode = 0;

        if (root == NULL || name == NULL)
                return ERR_PTR(-EINVAL);
        if (read_proc)
                mode = 0444;
        if (write_proc)
                mode |= 0200;
        if (fops)
                mode = 0644;
        proc = create_proc_entry(name, mode, root);
        if (!proc) {
                CERROR("LprocFS: No memory to create /proc entry %s", name);
                return ERR_PTR(-ENOMEM);
        }
        proc->read_proc = read_proc;
        proc->write_proc = write_proc;
        proc->data = data;
        if (fops)
                proc->proc_fops = fops;
        return proc;
}

static ssize_t lprocfs_fops_read(struct file *f, char __user *buf, size_t size,
                                 loff_t *ppos)
{
        struct proc_dir_entry *dp = PDE(f->f_dentry->d_inode);
        char *page, *start = NULL;
        int rc = 0, eof = 1, count;

        if (*ppos >= PAGE_SIZE)
                return 0;

        page = (char *)__get_free_page(GFP_KERNEL);
        if (page == NULL)
                return -ENOMEM;

        LPROCFS_ENTRY();
        OBD_FAIL_TIMEOUT(OBD_FAIL_LPROC_REMOVE, 10);
        if (!LPROCFS_CHECK_DELETED(dp) && dp->read_proc)
                rc = dp->read_proc(page, &start, *ppos, PAGE_SIZE,
                        &eof, dp->data);
        LPROCFS_EXIT();
        if (rc <= 0)
                goto out;

        /* for lustre proc read, the read count must be less than PAGE_SIZE */
        LASSERT(eof == 1);

        if (start == NULL) {
                rc -= *ppos;
                if (rc < 0)
                        rc = 0;
                if (rc == 0)
                        goto out;
                start = page + *ppos;
        } else if (start < page) {
                start = page;
        }

        count = (rc < size) ? rc : size;
        if (copy_to_user(buf, start, count)) {
                rc = -EFAULT;
                goto out;
        }
        *ppos += count;

out:
        free_page((unsigned long)page);
        return rc;
}

static ssize_t lprocfs_fops_write(struct file *f, const char __user *buf,
                                  size_t size, loff_t *ppos)
{
        struct proc_dir_entry *dp = PDE(f->f_dentry->d_inode);
        int rc = -EIO;

        LPROCFS_ENTRY();
        if (!LPROCFS_CHECK_DELETED(dp) && dp->write_proc)
                rc = dp->write_proc(f, buf, size, dp->data);
        LPROCFS_EXIT();
        return rc;
}

static struct file_operations lprocfs_generic_fops = {
        .owner = THIS_MODULE,
        .read = lprocfs_fops_read,
        .write = lprocfs_fops_write,
};

int lprocfs_evict_client_open(struct inode *inode, struct file *f)
{
        struct proc_dir_entry *dp = PDE(f->f_dentry->d_inode);
        struct obd_device *obd = dp->data;

        atomic_inc(&obd->obd_evict_inprogress);

        return 0;
}

int lprocfs_evict_client_release(struct inode *inode, struct file *f)
{
        struct proc_dir_entry *dp = PDE(f->f_dentry->d_inode);
        struct obd_device *obd = dp->data;

        atomic_dec(&obd->obd_evict_inprogress);
        wake_up(&obd->obd_evict_inprogress_waitq);

        return 0;
}

struct file_operations lprocfs_evict_client_fops = {
        .owner = THIS_MODULE,
        .read = lprocfs_fops_read,
        .write = lprocfs_fops_write,
        .open = lprocfs_evict_client_open,
        .release = lprocfs_evict_client_release,
};
EXPORT_SYMBOL(lprocfs_evict_client_fops);

/**
 * Add /proc entrys.
 *
 * \param root [in]  The parent proc entry on which new entry will be added.
 * \param list [in]  Array of proc entries to be added.
 * \param data [in]  The argument to be passed when entries read/write routines
 *                   are called through /proc file.
 *
 * \retval 0   on success
 *         < 0 on error
 */
int lprocfs_add_vars(struct proc_dir_entry *root, struct lprocfs_vars *list,
                     void *data)
{
        if (root == NULL || list == NULL)
                return -EINVAL;

        while (list->name != NULL) {
                struct proc_dir_entry *cur_root, *proc;
                char *pathcopy, *cur, *next, pathbuf[64];
                int pathsize = strlen(list->name) + 1;

                proc = NULL;
                cur_root = root;

                /* need copy of path for strsep */
                if (strlen(list->name) > sizeof(pathbuf) - 1) {
                        OBD_ALLOC(pathcopy, pathsize);
                        if (pathcopy == NULL)
                                return -ENOMEM;
                } else {
                        pathcopy = pathbuf;
                }

                next = pathcopy;
                strcpy(pathcopy, list->name);

                while (cur_root != NULL && (cur = strsep(&next, "/"))) {
                        if (*cur =='\0') /* skip double/trailing "/" */
                                continue;

                        proc = lprocfs_srch(cur_root, cur);
                        CDEBUG(D_OTHER, "cur_root=%s, cur=%s, next=%s, (%s)\n",
                               cur_root->name, cur, next,
                               (proc ? "exists" : "new"));
                        if (next != NULL) {
                                cur_root = (proc ? proc :
                                            proc_mkdir(cur, cur_root));
                        } else if (proc == NULL) {
                                mode_t mode = 0;
                                if (list->proc_mode != 0000) {
                                        mode = list->proc_mode;
                                } else {
                                        if (list->read_fptr)
                                                mode = 0444;
                                        if (list->write_fptr)
                                                mode |= 0200;
                                }
                                proc = create_proc_entry(cur, mode, cur_root);
                        }
                }

                if (pathcopy != pathbuf)
                        OBD_FREE(pathcopy, pathsize);

                if (cur_root == NULL || proc == NULL) {
                        CERROR("LprocFS: No memory to create /proc entry %s",
                               list->name);
                        return -ENOMEM;
                }

                if (list->fops)
                        proc->proc_fops = list->fops;
                else
                        proc->proc_fops = &lprocfs_generic_fops;
                proc->read_proc = list->read_fptr;
                proc->write_proc = list->write_fptr;
                proc->data = (list->data ? list->data : data);
                list++;
        }
        return 0;
}

void lprocfs_remove(struct proc_dir_entry **rooth)
{
        struct proc_dir_entry *root = *rooth;
        struct proc_dir_entry *temp = root;
        struct proc_dir_entry *rm_entry;
        struct proc_dir_entry *parent;

        if (!root)
                return;
        *rooth = NULL;

        parent = root->parent;
        LASSERT(parent != NULL);
        LPROCFS_WRITE_ENTRY(); /* search vs remove race */

        while (1) {
                while (temp->subdir != NULL)
                        temp = temp->subdir;

                rm_entry = temp;
                temp = temp->parent;

                /* Memory corruption once caused this to fail, and
                   without this LASSERT we would loop here forever. */
                LASSERTF(strlen(rm_entry->name) == rm_entry->namelen,
                         "0x%p  %s/%s len %d\n", rm_entry, temp->name,
                         rm_entry->name, (int)strlen(rm_entry->name));

#ifdef HAVE_PROCFS_USERS
                /* if procfs uses user count to synchronize deletion of
                 * proc entry, there is no protection for rm_entry->data,
                 * then lprocfs_fops_read and lprocfs_fops_write maybe
                 * call proc_dir_entry->read_proc (or write_proc) with
                 * proc_dir_entry->data == NULL, then cause kernel Oops.
                 * see bug19706 for detailed information */

                /* procfs won't free rm_entry->data if it isn't a LINK,
                 * and Lustre won't use rm_entry->data if it is a LINK */
                if (S_ISLNK(rm_entry->mode))
                        rm_entry->data = NULL;
#else
                /* Now, the rm_entry->deleted flags is protected
                 * by _lprocfs_lock. */
                rm_entry->data = NULL;
#endif
                remove_proc_entry(rm_entry->name, temp);
                if (temp == parent)
                        break;
        }
        LPROCFS_WRITE_EXIT();
}

struct proc_dir_entry *lprocfs_register(const char *name,
                                        struct proc_dir_entry *parent,
                                        struct lprocfs_vars *list, void *data)
{
        struct proc_dir_entry *newchild;

        newchild = lprocfs_srch(parent, name);
        if (newchild != NULL) {
                CERROR(" Lproc: Attempting to register %s more than once \n",
                       name);
                return ERR_PTR(-EALREADY);
        }

        newchild = proc_mkdir(name, parent);
        if (newchild != NULL && list != NULL) {
                int rc = lprocfs_add_vars(newchild, list, data);
                if (rc) {
                        lprocfs_remove(&newchild);
                        return ERR_PTR(rc);
                }
        }
        return newchild;
}

/* Generic callbacks */
int lprocfs_rd_uint(char *page, char **start, off_t off,
                    int count, int *eof, void *data)
{
        unsigned int *temp = (unsigned int *)data;
        return snprintf(page, count, "%u\n", *temp);
}

int lprocfs_wr_uint(struct file *file, const char *buffer,
                    unsigned long count, void *data)
{
        unsigned *p = data;
        char dummy[MAX_STRING_SIZE + 1] = { '\0' }, *end;
        unsigned long tmp;

        if (count >= sizeof(dummy) || count == 0)
                return -EINVAL;

        if (copy_from_user(dummy, buffer, count))
                return -EFAULT;

        tmp = simple_strtoul(dummy, &end, 0);
        if (dummy == end)
                return -EINVAL;

        *p = (unsigned int)tmp;
        return count;
}

int lprocfs_rd_u64(char *page, char **start, off_t off,
                   int count, int *eof, void *data)
{
        LASSERT(data != NULL);
        *eof = 1;
        return snprintf(page, count, LPU64"\n", *(__u64 *)data);
}

int lprocfs_rd_atomic(char *page, char **start, off_t off,
                   int count, int *eof, void *data)
{
        atomic_t *atom = (atomic_t *)data;
        LASSERT(atom != NULL);
        *eof = 1;
        return snprintf(page, count, "%d\n", atomic_read(atom));
}

int lprocfs_wr_atomic(struct file *file, const char *buffer,
                      unsigned long count, void *data)
{
        atomic_t *atm = data;
        int val = 0;
        int rc;

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc < 0)
                return rc;

        if (val <= 0)
                return -ERANGE;

        atomic_set(atm, val);
        return count;
}

int lprocfs_rd_uuid(char *page, char **start, off_t off, int count,
                    int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device*)data;

        LASSERT(obd != NULL);
        *eof = 1;
        return snprintf(page, count, "%s\n", obd->obd_uuid.uuid);
}

int lprocfs_rd_name(char *page, char **start, off_t off, int count,
                    int *eof, void* data)
{
        struct obd_device *dev = (struct obd_device *)data;

        LASSERT(dev != NULL);
        LASSERT(dev->obd_name != NULL);
        *eof = 1;
        return snprintf(page, count, "%s\n", dev->obd_name);
}

int lprocfs_rd_fstype(char *page, char **start, off_t off, int count, int *eof,
                      void *data)
{
        struct obd_device *obd = (struct obd_device *)data;

        LASSERT(obd != NULL);
        LASSERT(obd->obd_fsops != NULL);
        LASSERT(obd->obd_fsops->fs_type != NULL);
        return snprintf(page, count, "%s\n", obd->obd_fsops->fs_type);
}

int lprocfs_rd_blksize(char *page, char **start, off_t off, int count,
                       int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs,
                            cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
                            OBD_STATFS_NODELAY);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, "%u\n", osfs.os_bsize);
        }
        return rc;
}

int lprocfs_rd_kbytestotal(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs,
                            cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
                            OBD_STATFS_NODELAY);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_blocks;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_rd_kbytesfree(char *page, char **start, off_t off, int count,
                          int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs,
                            cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
                            OBD_STATFS_NODELAY);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_bfree;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_rd_kbytesavail(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs,
                            cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
                            OBD_STATFS_NODELAY);
        if (!rc) {
                __u32 blk_size = osfs.os_bsize >> 10;
                __u64 result = osfs.os_bavail;

                while (blk_size >>= 1)
                        result <<= 1;

                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", result);
        }
        return rc;
}

int lprocfs_rd_filestotal(char *page, char **start, off_t off, int count,
                          int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs,
                            cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
                            OBD_STATFS_NODELAY);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", osfs.os_files);
        }

        return rc;
}

int lprocfs_rd_filesfree(char *page, char **start, off_t off, int count,
                         int *eof, void *data)
{
        struct obd_statfs osfs;
        int rc = obd_statfs(data, &osfs,
                            cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
                            OBD_STATFS_NODELAY);
        if (!rc) {
                *eof = 1;
                rc = snprintf(page, count, LPU64"\n", osfs.os_ffree);
        }
        return rc;
}

int lprocfs_rd_server_uuid(char *page, char **start, off_t off, int count,
                           int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        struct obd_import *imp;
        char *imp_state_name = NULL;
        int rc = 0;

        LASSERT(obd != NULL);
        LPROCFS_CLIMP_CHECK(obd);
        imp = obd->u.cli.cl_import;
        imp_state_name = ptlrpc_import_state_name(imp->imp_state);
        *eof = 1;
        rc = snprintf(page, count, "%s\t%s%s\n",
                        obd2cli_tgt(obd), imp_state_name,
                        imp->imp_deactive ? "\tDEACTIVATED" : "");

        LPROCFS_CLIMP_EXIT(obd);
        return rc;
}

int lprocfs_rd_conn_uuid(char *page, char **start, off_t off, int count,
                         int *eof,  void *data)
{
        struct obd_device *obd = (struct obd_device*)data;
        struct ptlrpc_connection *conn;
        int rc = 0;

        LASSERT(obd != NULL);
        LPROCFS_CLIMP_CHECK(obd);
        conn = obd->u.cli.cl_import->imp_connection;
        LASSERT(conn != NULL);
        *eof = 1;
        rc = snprintf(page, count, "%s\n", conn->c_remote_uuid.uuid);

        LPROCFS_CLIMP_EXIT(obd);
        return rc;
}

/** add up per-cpu counters */
void lprocfs_stats_collect(struct lprocfs_stats *stats, int idx,
                           struct lprocfs_counter *cnt)
{
        unsigned int num_cpu;
        struct lprocfs_counter t;
        struct lprocfs_counter *percpu_cntr;
        int centry, i;

        memset(cnt, 0, sizeof(*cnt));

        if (stats == NULL) {
                /* set count to 1 to avoid divide-by-zero errs in callers */
                cnt->lc_count = 1;
                return;
        }

        cnt->lc_min = LC_MIN_INIT;

        if (stats->ls_flags & LPROCFS_STATS_FLAG_NOPERCPU)
                num_cpu = 1;
        else
                num_cpu = num_possible_cpus();

        for (i = 0; i < num_cpu; i++) {
                percpu_cntr = &(stats->ls_percpu[i])->lp_cntr[idx];

                do {
                        centry = atomic_read(&percpu_cntr->lc_cntl.la_entry);
                        t.lc_count = percpu_cntr->lc_count;
                        t.lc_sum = percpu_cntr->lc_sum;
                        t.lc_min = percpu_cntr->lc_min;
                        t.lc_max = percpu_cntr->lc_max;
                        t.lc_sumsquare = percpu_cntr->lc_sumsquare;
                } while (centry != atomic_read(&percpu_cntr->lc_cntl.la_entry) &&
                         centry != atomic_read(&percpu_cntr->lc_cntl.la_exit));
                cnt->lc_count += t.lc_count;
                cnt->lc_sum += t.lc_sum;
                if (t.lc_min < cnt->lc_min)
                        cnt->lc_min = t.lc_min;
                if (t.lc_max > cnt->lc_max)
                        cnt->lc_max = t.lc_max;
                cnt->lc_sumsquare += t.lc_sumsquare;
        }

        cnt->lc_units = stats->ls_percpu[0]->lp_cntr[idx].lc_units;
}

/**
 * Append a space separated list of current set flags to str.
 */
#define flag2str(flag) \
        if (imp->imp_##flag && max - len > 0) \
             len += snprintf(str + len, max - len, "%s" #flag, len ? ", " : "");
static int obd_import_flags2str(struct obd_import *imp, char *str, int max)
{
        int len = 0;

        if (imp->imp_obd->obd_no_recov)
                len += snprintf(str, max - len, " no_recov");

        flag2str(invalid);
        flag2str(deactive);
        flag2str(replayable);
        flag2str(pingable);
        flag2str(recon_bk);
        flag2str(last_recon);
        return len;
}
#undef flags2str

static const char *obd_connect_names[] = {
        "read_only",
        "lov_index",
        "unused",
        "write_grant",
        "server_lock",
        "version",
        "request_portal",
        "acl",
        "xattr",
        "create_on_write",
        "truncate_lock",
        "initial_transno",
        "inode_bit_locks",
        "join_file",
        "getattr_by_fid",
        "no_oh_for_devices",
        "local_client",
        "remote_client",
        "max_byte_per_rpc",
        "64bit_qdata",
        "mds_capability",
        "oss_capability",
        "early_lock_cancel",
        "size_on_mds",
        "adaptive_timeouts",
        "lru_resize",
        "mds_mds_connection",
        "real_conn",
        "change_qunit_size",
        "alt_checksum_algorithm",
        "fid_is_enabled",
        "version_recovery",
        "pools",
        "grant_shrink",
        "skip_orphan",
        NULL
};

static int obd_connect_flags2str(char *page, int count, __u64 flags, char *sep)
{
        __u64 mask = 1;
        int i, ret = 0;

        for (i = 0; obd_connect_names[i] != NULL; i++, mask <<= 1) {
                if (flags & mask)
                        ret += snprintf(page + ret, count - ret, "%s%s",
                                        ret ? sep : "", obd_connect_names[i]);
        }
        if (flags & ~(mask - 1))
                ret += snprintf(page + ret, count - ret,
                                "%sunknown flags "LPX64,
                                ret ? sep : "", flags & ~(mask - 1));
        return ret;
}

int lprocfs_rd_import(char *page, char **start, off_t off, int count,
                      int *eof, void *data)
{
        struct lprocfs_counter ret;
        struct obd_device *obd = (struct obd_device *)data;
        struct obd_import *imp;
        int i, j, k, rw = 0;

        LASSERT(obd != NULL);
        LPROCFS_CLIMP_CHECK(obd);
        imp = obd->u.cli.cl_import;
        *eof = 1;

        i = snprintf(page, count,
                     "import:\n"
                     "    name: %s\n"
                     "    target: %s\n"
                     "    current_connection: %s\n"
                     "    state: %s\n"
                     "    connect_flags: [",
                     obd->obd_name,
                     obd2cli_tgt(obd),
                     imp->imp_connection->c_remote_uuid.uuid,
                     ptlrpc_import_state_name(imp->imp_state));
        i += obd_connect_flags2str(page + i, count - i,
                                   imp->imp_connect_data.ocd_connect_flags,
                                   ", ");
        i += snprintf(page + i, count - i,
                      "]\n"
                      "    import_flags: [");
        i += obd_import_flags2str(imp, page + i, count - i);

        i += snprintf(page + i, count - i,
                      "]\n"
                      "    connection:\n"
                      "       connection_attempts: %u\n"
                      "       generation: %u\n"
                      "       in-progress_invalidations: %u\n",
                      imp->imp_conn_cnt,
                      imp->imp_generation,
                      atomic_read(&imp->imp_inval_count));

        lprocfs_stats_collect(obd->obd_svc_stats, PTLRPC_REQWAIT_CNTR, &ret);
        if (ret.lc_count != 0)
                do_div(ret.lc_sum, ret.lc_count);
        else
                ret.lc_sum = 0;
        i += snprintf(page + i, count - i,
                      "    rpcs:\n"
                      "       inflight: %u\n"
                      "       unregistering: %u\n"
                      "       timeouts: %u\n"
                      "       avg_waittime: "LPU64" %s\n",
                      atomic_read(&imp->imp_inflight),
                      atomic_read(&imp->imp_unregistering),
                      atomic_read(&imp->imp_timeouts),
                      ret.lc_sum, ret.lc_units);

        k = 0;
        for(j = 0; j < IMP_AT_MAX_PORTALS; j++) {
                if (imp->imp_at.iat_portal[j] == 0)
                        break;
                k = max_t(unsigned int, k,
                          at_get(&imp->imp_at.iat_service_estimate[j]));
        }
        i += snprintf(page + i, count - i,
                      "    service_estimates:\n"
                      "       services: %u sec\n"
                      "       network: %u sec\n",
                      k,
                      at_get(&imp->imp_at.iat_net_latency));

        i += snprintf(page + i, count - i,
                      "    transactions:\n"
                      "       last_replay: "LPU64"\n"
                      "       peer_committed: "LPU64"\n"
                      "       last_checked: "LPU64"\n",
                      imp->imp_last_replay_transno,
                      imp->imp_peer_committed_transno,
                      imp->imp_last_transno_checked);

        /* avg data rates */
        for (rw = 0; rw <= 1; rw++) {
                lprocfs_stats_collect(obd->obd_svc_stats,
                                      PTLRPC_LAST_CNTR + BRW_READ_BYTES + rw,
                                      &ret);
                if (ret.lc_sum > 0 && ret.lc_count > 0) {
                        do_div(ret.lc_sum, ret.lc_count);
                        i += snprintf(page + i, count - i,
                                      "    %s_data_averages:\n"
                                      "       bytes_per_rpc: "LPU64"\n",
                                      rw ? "write" : "read",
                                      ret.lc_sum);
                }
                k = (int)ret.lc_sum;
                j = opcode_offset(OST_READ + rw) + EXTRA_MAX_OPCODES;
                lprocfs_stats_collect(obd->obd_svc_stats, j, &ret);
                if (ret.lc_sum > 0 && ret.lc_count != 0) {
                        do_div(ret.lc_sum, ret.lc_count);
                        i += snprintf(page + i, count - i,
                                      "       %s_per_rpc: "LPU64"\n",
                                      ret.lc_units, ret.lc_sum);
                        j = (int)ret.lc_sum;
                        if (j > 0)
                                i += snprintf(page + i, count - i,
                                              "       MB_per_sec: %u.%.02u\n",
                                              k / j, (100 * k / j) % 100);
                }
        }

        LPROCFS_CLIMP_EXIT(obd);
        return i;
}

int lprocfs_rd_state(char *page, char **start, off_t off, int count,
                      int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        struct obd_import *imp;
        int i, j, k;

        LASSERT(obd != NULL);
        LPROCFS_CLIMP_CHECK(obd);
        imp = obd->u.cli.cl_import;
        *eof = 1;

        i = snprintf(page, count, "current_state: %s\n",
                     ptlrpc_import_state_name(imp->imp_state));
        i += snprintf(page + i, count - i,
                      "state_history:\n");
        k = imp->imp_state_hist_idx;
        for (j = 0; j < IMP_STATE_HIST_LEN; j++) {
                struct import_state_hist *ish =
                        &imp->imp_state_hist[(k + j) % IMP_STATE_HIST_LEN];
                if (ish->ish_state == 0)
                        continue;
                i += snprintf(page + i, count - i, " - ["CFS_TIME_T", %s]\n",
                              ish->ish_time,
                              ptlrpc_import_state_name(ish->ish_state));
        }

        LPROCFS_CLIMP_EXIT(obd);
        return i;
}

int lprocfs_at_hist_helper(char *page, int count, int rc,
                           struct adaptive_timeout *at)
{
        int i;
        for (i = 0; i < AT_BINS; i++)
                rc += snprintf(page + rc, count - rc, "%3u ", at->at_hist[i]);
        rc += snprintf(page + rc, count - rc, "\n");
        return rc;
}

/* See also ptlrpc_lprocfs_rd_timeouts */
int lprocfs_rd_timeouts(char *page, char **start, off_t off, int count,
                        int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        struct obd_import *imp;
        unsigned int cur, worst;
        time_t now, worstt;
        struct dhms ts;
        int i, rc = 0;

        LASSERT(obd != NULL);
        LPROCFS_CLIMP_CHECK(obd);
        imp = obd->u.cli.cl_import;
        *eof = 1;

        now = cfs_time_current_sec();

        /* Some network health info for kicks */
        s2dhms(&ts, now - imp->imp_last_reply_time);
        rc += snprintf(page + rc, count - rc,
                       "%-10s : %ld, "DHMS_FMT" ago\n",
                       "last reply", imp->imp_last_reply_time, DHMS_VARS(&ts));

        cur = at_get(&imp->imp_at.iat_net_latency);
        worst = imp->imp_at.iat_net_latency.at_worst_ever;
        worstt = imp->imp_at.iat_net_latency.at_worst_time;
        s2dhms(&ts, now - worstt);
        rc += snprintf(page + rc, count - rc,
                       "%-10s : cur %3u  worst %3u (at %ld, "DHMS_FMT" ago) ",
                       "network", cur, worst, worstt, DHMS_VARS(&ts));
        rc = lprocfs_at_hist_helper(page, count, rc,
                                    &imp->imp_at.iat_net_latency);

        for(i = 0; i < IMP_AT_MAX_PORTALS; i++) {
                if (imp->imp_at.iat_portal[i] == 0)
                        break;
                cur = at_get(&imp->imp_at.iat_service_estimate[i]);
                worst = imp->imp_at.iat_service_estimate[i].at_worst_ever;
                worstt = imp->imp_at.iat_service_estimate[i].at_worst_time;
                s2dhms(&ts, now - worstt);
                rc += snprintf(page + rc, count - rc,
                               "portal %-2d  : cur %3u  worst %3u (at %ld, "
                               DHMS_FMT" ago) ", imp->imp_at.iat_portal[i],
                               cur, worst, worstt, DHMS_VARS(&ts));
                rc = lprocfs_at_hist_helper(page, count, rc,
                                          &imp->imp_at.iat_service_estimate[i]);
        }

        LPROCFS_CLIMP_EXIT(obd);
        return rc;
}

int lprocfs_rd_connect_flags(char *page, char **start, off_t off,
                             int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        __u64 flags;
        int ret = 0;

        LPROCFS_CLIMP_CHECK(obd);
        flags = obd->u.cli.cl_import->imp_connect_data.ocd_connect_flags;
        ret = snprintf(page, count, "flags="LPX64"\n", flags);
        ret += obd_connect_flags2str(page + ret, count - ret, flags, "\n");
        ret += snprintf(page + ret, count - ret, "\n");
        LPROCFS_CLIMP_EXIT(obd);
        return ret;
}
EXPORT_SYMBOL(lprocfs_rd_connect_flags);

int lprocfs_rd_num_exports(char *page, char **start, off_t off, int count,
                           int *eof,  void *data)
{
        struct obd_device *obd = (struct obd_device*)data;

        LASSERT(obd != NULL);
        *eof = 1;
        return snprintf(page, count, "%u\n", obd->obd_num_exports);
}

int lprocfs_rd_numrefs(char *page, char **start, off_t off, int count,
                       int *eof, void *data)
{
        struct obd_type *class = (struct obd_type*) data;

        LASSERT(class != NULL);
        *eof = 1;
        return snprintf(page, count, "%d\n", class->typ_refcnt);
}

int lprocfs_obd_setup(struct obd_device *obd, struct lprocfs_vars *list)
{
        int rc = 0;

        LASSERT(obd != NULL);
        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);
        LASSERT(obd->obd_type->typ_procroot != NULL);

        obd->obd_proc_entry = lprocfs_register(obd->obd_name,
                                               obd->obd_type->typ_procroot,
                                               list, obd);
        if (IS_ERR(obd->obd_proc_entry)) {
                rc = PTR_ERR(obd->obd_proc_entry);
                CERROR("error %d setting up lprocfs for %s\n",rc,obd->obd_name);
                obd->obd_proc_entry = NULL;
        }
        return rc;
}

int lprocfs_obd_cleanup(struct obd_device *obd)
{
        if (!obd)
                return -EINVAL;
        if (obd->obd_proc_exports_entry) {
                /* Should be no exports left */
                LASSERT(obd->obd_proc_exports_entry->subdir == NULL);
                lprocfs_remove(&obd->obd_proc_exports_entry);
        }
        lprocfs_remove(&obd->obd_proc_entry);
        return 0;
}

static void lprocfs_free_client_stats(struct nid_stat *client_stat)
{
        CDEBUG(D_CONFIG, "stat %p - data %p/%p/%p\n", client_stat,
               client_stat->nid_proc, client_stat->nid_stats,
               client_stat->nid_brw_stats);

        LASSERTF(atomic_read(&client_stat->nid_exp_ref_count) == 0,
                 "count %d\n", atomic_read(&client_stat->nid_exp_ref_count));

        hlist_del_init(&client_stat->nid_hash);

        if (client_stat->nid_proc)
                lprocfs_remove(&client_stat->nid_proc);

        if (client_stat->nid_stats)
                lprocfs_free_stats(&client_stat->nid_stats);

        if (client_stat->nid_brw_stats)
                OBD_FREE_PTR(client_stat->nid_brw_stats);

        if (client_stat->nid_ldlm_stats)
                lprocfs_free_stats(&client_stat->nid_ldlm_stats);

        OBD_FREE_PTR(client_stat);
        return;

}

void lprocfs_free_per_client_stats(struct obd_device *obd)
{
        struct nid_stat *stat;
        ENTRY;

        /* we need extra list - because hash_exit called to early */
        /* not need locking because all clients is died */
        while(!list_empty(&obd->obd_nid_stats)) {
                stat = list_entry(obd->obd_nid_stats.next,
                                  struct nid_stat, nid_list);
                list_del_init(&stat->nid_list);
                lprocfs_free_client_stats(stat);
        }

        EXIT;
}

struct lprocfs_stats *lprocfs_alloc_stats(unsigned int num,
                                          enum lprocfs_stats_flags flags)
{
        struct lprocfs_stats *stats;
        unsigned int percpusize;
        unsigned int i, j;
        unsigned int num_cpu;

        if (num == 0)
                return NULL;

        if (flags & LPROCFS_STATS_FLAG_NOPERCPU)
                num_cpu = 1;
        else
                num_cpu = num_possible_cpus();

        OBD_ALLOC(stats, offsetof(typeof(*stats), ls_percpu[num_cpu]));
        if (stats == NULL)
                return NULL;

        if (flags & LPROCFS_STATS_FLAG_NOPERCPU) {
                stats->ls_flags = flags;
                spin_lock_init(&stats->ls_lock);
                /* Use this lock only if there are no percpu areas */
        } else {
                stats->ls_flags = 0;
        }

        percpusize = offsetof(struct lprocfs_percpu, lp_cntr[num]);
        if (num_cpu > 1)
                percpusize = L1_CACHE_ALIGN(percpusize);

        for (i = 0; i < num_cpu; i++) {
                OBD_ALLOC(stats->ls_percpu[i], percpusize);
                if (stats->ls_percpu[i] == NULL) {
                        for (j = 0; j < i; j++) {
                                OBD_FREE(stats->ls_percpu[j], percpusize);
                                stats->ls_percpu[j] = NULL;
                        }
                        break;
                }
        }
        if (stats->ls_percpu[0] == NULL) {
                OBD_FREE(stats, offsetof(typeof(*stats),
                                         ls_percpu[num_cpu]));
                return NULL;
        }

        stats->ls_num = num;
        return stats;
}

void lprocfs_free_stats(struct lprocfs_stats **statsh)
{
        struct lprocfs_stats *stats = *statsh;
        unsigned int num_cpu;
        unsigned int percpusize;
        unsigned int i;

        if (!stats || (stats->ls_num == 0))
                return;
        *statsh = NULL;
        if (stats->ls_flags & LPROCFS_STATS_FLAG_NOPERCPU)
                num_cpu = 1;
        else
                num_cpu = num_possible_cpus();

        percpusize = offsetof(struct lprocfs_percpu, lp_cntr[stats->ls_num]);
        if (num_cpu > 1)
                percpusize = L1_CACHE_ALIGN(percpusize);
        for (i = 0; i < num_cpu; i++)
                OBD_FREE(stats->ls_percpu[i], percpusize);
        OBD_FREE(stats, offsetof(typeof(*stats), ls_percpu[num_cpu]));
}

void lprocfs_clear_stats(struct lprocfs_stats *stats)
{
        struct lprocfs_counter *percpu_cntr;
        int i, j;
        unsigned int num_cpu;

        num_cpu = lprocfs_stats_lock(stats, LPROCFS_GET_NUM_CPU);

        for (i = 0; i < num_cpu; i++) {
                for (j = 0; j < stats->ls_num; j++) {
                        percpu_cntr = &(stats->ls_percpu[i])->lp_cntr[j];
                        atomic_inc(&percpu_cntr->lc_cntl.la_entry);
                        percpu_cntr->lc_count = 0;
                        percpu_cntr->lc_sum = 0;
                        percpu_cntr->lc_min = LC_MIN_INIT;
                        percpu_cntr->lc_max = 0;
                        percpu_cntr->lc_sumsquare = 0;
                        atomic_inc(&percpu_cntr->lc_cntl.la_exit);
                }
        }

        lprocfs_stats_unlock(stats);
}

static ssize_t lprocfs_stats_seq_write(struct file *file, const char *buf,
                                       size_t len, loff_t *off)
{
        struct seq_file *seq = file->private_data;
        struct lprocfs_stats *stats = seq->private;

        lprocfs_clear_stats(stats);

        return len;
}

static void *lprocfs_stats_seq_start(struct seq_file *p, loff_t *pos)
{
        struct lprocfs_stats *stats = p->private;
        /* return 1st cpu location */
        return (*pos >= stats->ls_num) ? NULL :
                &(stats->ls_percpu[0]->lp_cntr[*pos]);
}

static void lprocfs_stats_seq_stop(struct seq_file *p, void *v)
{
}

static void *lprocfs_stats_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
        struct lprocfs_stats *stats = p->private;
        ++*pos;
        return (*pos >= stats->ls_num) ? NULL :
                &(stats->ls_percpu[0]->lp_cntr[*pos]);
}

/* seq file export of one lprocfs counter */
static int lprocfs_stats_seq_show(struct seq_file *p, void *v)
{
       struct lprocfs_stats *stats = p->private;
       struct lprocfs_counter  *cntr = v;
       struct lprocfs_counter ret;
       int idx, rc = 0;

       if (cntr == &(stats->ls_percpu[0])->lp_cntr[0]) {
               struct timeval now;
               do_gettimeofday(&now);
               rc = seq_printf(p, "%-25s %lu.%lu secs.usecs\n",
                               "snapshot_time", now.tv_sec, now.tv_usec);
               if (rc < 0)
                       return rc;
       }
       idx = cntr - &(stats->ls_percpu[0])->lp_cntr[0];

       lprocfs_stats_collect(stats, idx, &ret);

       if (ret.lc_count == 0)
               goto out;

       rc = seq_printf(p, "%-25s "LPD64" samples [%s]", cntr->lc_name,
                       ret.lc_count, cntr->lc_units);

       if (rc < 0)
               goto out;

       if ((cntr->lc_config & LPROCFS_CNTR_AVGMINMAX) && (ret.lc_count > 0)) {
               rc = seq_printf(p, " "LPD64" "LPD64" "LPD64,
                               ret.lc_min, ret.lc_max, ret.lc_sum);
               if (rc < 0)
                       goto out;
               if (cntr->lc_config & LPROCFS_CNTR_STDDEV)
                       rc = seq_printf(p, " "LPD64, ret.lc_sumsquare);
               if (rc < 0)
                       goto out;
       }
       rc = seq_printf(p, "\n");
 out:
       return (rc < 0) ? rc : 0;
}

struct seq_operations lprocfs_stats_seq_sops = {
        start: lprocfs_stats_seq_start,
        stop:  lprocfs_stats_seq_stop,
        next:  lprocfs_stats_seq_next,
        show:  lprocfs_stats_seq_show,
};

static int lprocfs_stats_seq_open(struct inode *inode, struct file *file)
{
        struct proc_dir_entry *dp = PDE(inode);
        struct seq_file *seq;
        int rc;

        LPROCFS_ENTRY_AND_CHECK(dp);
        rc = seq_open(file, &lprocfs_stats_seq_sops);
        if (rc) {
                LPROCFS_EXIT();
                return rc;
        }

        seq = file->private_data;
        seq->private = dp->data;
        return 0;
}

struct file_operations lprocfs_stats_seq_fops = {
        .owner   = THIS_MODULE,
        .open    = lprocfs_stats_seq_open,
        .read    = seq_read,
        .write   = lprocfs_stats_seq_write,
        .llseek  = seq_lseek,
        .release = lprocfs_seq_release,
};

int lprocfs_register_stats(struct proc_dir_entry *root, const char *name,
                           struct lprocfs_stats *stats)
{
        struct proc_dir_entry *entry;
        LASSERT(root != NULL);

        entry = create_proc_entry(name, 0644, root);
        if (entry == NULL)
                return -ENOMEM;
        entry->proc_fops = &lprocfs_stats_seq_fops;
        entry->data = (void *)stats;
        return 0;
}

void lprocfs_counter_init(struct lprocfs_stats *stats, int index,
                          unsigned conf, const char *name, const char *units)
{
        struct lprocfs_counter *c;
        int i;
        unsigned int num_cpu;

        LASSERT(stats != NULL);

        num_cpu = lprocfs_stats_lock(stats, LPROCFS_GET_NUM_CPU);

        for (i = 0; i < num_cpu; i++) {
                c = &(stats->ls_percpu[i]->lp_cntr[index]);
                c->lc_config = conf;
                c->lc_count = 0;
                c->lc_sum = 0;
                c->lc_min = LC_MIN_INIT;
                c->lc_max = 0;
                c->lc_name = name;
                c->lc_units = units;
        }

        lprocfs_stats_unlock(stats);
}
EXPORT_SYMBOL(lprocfs_counter_init);

#define LPROCFS_OBD_OP_INIT(base, stats, op)                               \
do {                                                                       \
        unsigned int coffset = base + OBD_COUNTER_OFFSET(op);              \
        LASSERT(coffset < stats->ls_num);                                  \
        lprocfs_counter_init(stats, coffset, 0, #op, "reqs");              \
} while (0)

void lprocfs_init_ops_stats(int num_private_stats, struct lprocfs_stats *stats)
{
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, iocontrol);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, get_info);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, set_info_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, attach);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, detach);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, setup);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, precleanup);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, cleanup);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, process_config);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, postrecov);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, add_conn);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, del_conn);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, connect);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, reconnect);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, disconnect);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, fid_init);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, fid_fini);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, statfs);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, statfs_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, packmd);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, unpackmd);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, checkmd);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, preallocate);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, precreate);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, create);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, create_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, destroy);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, setattr);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, setattr_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, getattr);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, getattr_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, brw);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, brw_async);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, prep_async_page);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, get_lock);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, queue_async_io);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, queue_group_io);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, trigger_group_io);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, set_async_flags);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, teardown_async_page);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, merge_lvb);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, update_lvb);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, adjust_kms);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, punch);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, sync);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, migrate);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, copy);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, iterate);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, preprw);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, commitrw);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, enqueue);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, match);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, change_cbdata);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, cancel);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, cancel_unused);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, join_lru);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, init_export);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, destroy_export);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, extent_calc);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, llog_init);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, llog_finish);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, pin);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, unpin);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, import_event);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, notify);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, health_check);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, quotacheck);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, quotactl);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, quota_adjust_qunit);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, ping);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, register_page_removal_cb);
        LPROCFS_OBD_OP_INIT(num_private_stats,stats,unregister_page_removal_cb);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, register_lock_cancel_cb);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats,unregister_lock_cancel_cb);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, pool_new);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, pool_rem);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, pool_add);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, pool_del);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, getref);
        LPROCFS_OBD_OP_INIT(num_private_stats, stats, putref);
}

void lprocfs_init_ldlm_stats(struct lprocfs_stats *ldlm_stats)
{
        lprocfs_counter_init(ldlm_stats,
                             LDLM_ENQUEUE - LDLM_FIRST_OPC,
                             0, "ldlm_enqueue", "reqs");
        lprocfs_counter_init(ldlm_stats,
                             LDLM_CONVERT - LDLM_FIRST_OPC,
                             0, "ldlm_convert", "reqs");
        lprocfs_counter_init(ldlm_stats,
                             LDLM_CANCEL - LDLM_FIRST_OPC,
                             0, "ldlm_cancel", "reqs");
        lprocfs_counter_init(ldlm_stats,
                             LDLM_BL_CALLBACK - LDLM_FIRST_OPC,
                             0, "ldlm_bl_callback", "reqs");
        lprocfs_counter_init(ldlm_stats,
                             LDLM_CP_CALLBACK - LDLM_FIRST_OPC,
                             0, "ldlm_cp_callback", "reqs");
        lprocfs_counter_init(ldlm_stats,
                             LDLM_GL_CALLBACK - LDLM_FIRST_OPC,
                             0, "ldlm_gl_callback", "reqs");
}

int lprocfs_alloc_obd_stats(struct obd_device *obd, unsigned num_private_stats)
{
        struct lprocfs_stats *stats;
        unsigned int num_stats;
        int rc, i;

        LASSERT(obd->obd_stats == NULL);
        LASSERT(obd->obd_proc_entry != NULL);
        LASSERT(obd->obd_cntr_base == 0);

        num_stats = ((int)sizeof(*obd->obd_type->typ_ops) / sizeof(void *)) +
                num_private_stats - 1 /* o_owner */;
        stats = lprocfs_alloc_stats(num_stats, 0);
        if (stats == NULL)
                return -ENOMEM;

        lprocfs_init_ops_stats(num_private_stats, stats);

        for (i = num_private_stats; i < num_stats; i++) {
                /* If this LBUGs, it is likely that an obd
                 * operation was added to struct obd_ops in
                 * <obd.h>, and that the corresponding line item
                 * LPROCFS_OBD_OP_INIT(.., .., opname)
                 * is missing from the list above. */
                LASSERTF(stats->ls_percpu[0]->lp_cntr[i].lc_name != NULL,
                         "Missing obd_stat initializer obd_op "
                         "operation at offset %d.\n", i - num_private_stats);
        }
        rc = lprocfs_register_stats(obd->obd_proc_entry, "stats", stats);
        if (rc < 0) {
                lprocfs_free_stats(&stats);
        } else {
                obd->obd_stats  = stats;
                obd->obd_cntr_base = num_private_stats;
        }
        return rc;
}

void lprocfs_free_obd_stats(struct obd_device *obd)
{
        if (obd->obd_stats)
                lprocfs_free_stats(&obd->obd_stats);
}

int lprocfs_exp_rd_nid(char *page, char **start, off_t off, int count,
                         int *eof,  void *data)
{
        struct obd_export *exp = (struct obd_export*)data;
        LASSERT(exp != NULL);
        *eof = 1;
        return snprintf(page, count, "%s\n", obd_export_nid2str(exp));
}

struct exp_uuid_cb_data {
        char                   *page;
        int                     count;
        int                    *eof;
        int                    *len;
};

static void
lprocfs_exp_rd_cb_data_init(struct exp_uuid_cb_data *cb_data, char *page,
                            int count, int *eof, int *len)
{
        cb_data->page = page;
        cb_data->count = count;
        cb_data->eof = eof;
        cb_data->len = len;
}

void lprocfs_exp_print_uuid(void *obj, void *cb_data)
{
        struct obd_export *exp = (struct obd_export *)obj;
        struct exp_uuid_cb_data *data = (struct exp_uuid_cb_data *)cb_data;

        if (exp->exp_nid_stats)
                *data->len += snprintf((data->page + *data->len),
                                       data->count, "%s\n",
                                       obd_uuid2str(&exp->exp_client_uuid));
}

int lprocfs_exp_rd_uuid(char *page, char **start, off_t off, int count,
                        int *eof,  void *data)
{
        struct nid_stat *stats = (struct nid_stat *)data;
        struct exp_uuid_cb_data cb_data;
        struct obd_device *obd = stats->nid_obd;
        int len = 0;

        *eof = 1;
        page[0] = '\0';
        lprocfs_exp_rd_cb_data_init(&cb_data, page, count, eof, &len);

        /* obd->obd_nid_hash may be destroyed before this proc entry */
        if (obd->obd_nid_hash)
                lustre_hash_for_each_key(obd->obd_nid_hash, &stats->nid,
                                         lprocfs_exp_print_uuid, &cb_data);
        return (*cb_data.len);
}

void lprocfs_exp_print_hash(void *obj, void *cb_data)
{
        struct obd_export *exp = (struct obd_export *)obj;
        struct exp_uuid_cb_data *data = (struct exp_uuid_cb_data *)cb_data;
        lustre_hash_t *lh;

        lh = exp->exp_lock_hash;
        if (lh) {
                if (!*data->len)
                        *data->len += lustre_hash_debug_header(data->page,
                                                               data->count);

                *data->len += lustre_hash_debug_str(lh, data->page +
                                                    *data->len,
                                                    data->count);
     }
}

int lprocfs_exp_rd_hash(char *page, char **start, off_t off, int count,
                     int *eof,  void *data)
{
        struct nid_stat *stats = (struct nid_stat *)data;
        struct exp_uuid_cb_data cb_data;
        struct obd_device *obd = stats->nid_obd;
        int len = 0;

        *eof = 1;
        page[0] = '\0';
        lprocfs_exp_rd_cb_data_init(&cb_data, page, count, eof, &len);

        /* obd->obd_nid_hash may be destroyed before this proc entry */
        if (obd->obd_nid_hash)
                lustre_hash_for_each_key(obd->obd_nid_hash, &stats->nid,
                                         lprocfs_exp_print_hash, &cb_data);
        return (*cb_data.len);
}

int lprocfs_nid_stats_clear_read(char *page, char **start, off_t off,
                                        int count, int *eof,  void *data)
{
        *eof = 1;
        return snprintf(page, count, "%s\n",
                        "Write into this file to clear all nid stats and "
                        "stale nid entries");
}
EXPORT_SYMBOL(lprocfs_nid_stats_clear_read);

void lprocfs_nid_stats_clear_write_cb(void *obj, void *data)
{
        struct nid_stat *stat = obj;
        int i;
        ENTRY;
        /* object has only hash + iterate_all references.
         * add/delete blocked by hash bucket lock */
        CDEBUG(D_INFO,"refcnt %d\n", atomic_read(&stat->nid_exp_ref_count));
        if (atomic_read(&stat->nid_exp_ref_count) == 2) {
                hlist_del_init(&stat->nid_hash);
                nidstat_putref(stat);
                spin_lock(&stat->nid_obd->obd_nid_lock);
                list_move(&stat->nid_list, data);
                spin_unlock(&stat->nid_obd->obd_nid_lock);
                EXIT;
                return;
        }
        /* we has reference to object - only clear data*/
        if (stat->nid_stats)
                lprocfs_clear_stats(stat->nid_stats);

        if (stat->nid_brw_stats) {
                for (i = 0; i < BRW_LAST; i++)
                        lprocfs_oh_clear(&stat->nid_brw_stats->hist[i]);
        }
        EXIT;
        return;
}

int lprocfs_nid_stats_clear_write(struct file *file, const char *buffer,
                                         unsigned long count, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        struct nid_stat *client_stat;
        CFS_LIST_HEAD(free_list);

        lustre_hash_for_each(obd->obd_nid_stats_hash,
                             lprocfs_nid_stats_clear_write_cb, &free_list);

        while (!list_empty(&free_list)) {
                client_stat = list_entry(free_list.next, struct nid_stat,
                                         nid_list);
                list_del_init(&client_stat->nid_list);
                lprocfs_free_client_stats(client_stat);
        }

        return count;
}
EXPORT_SYMBOL(lprocfs_nid_stats_clear_write);

int lprocfs_exp_setup(struct obd_export *exp, lnet_nid_t *nid, int reconnect,
                      int *newnid)
{
        int rc = 0;
        struct nid_stat *new_stat, *old_stat;
        struct obd_device *obd;
        cfs_proc_dir_entry_t *entry;
        char *buffer = NULL;
        ENTRY;

        *newnid = 0;

        if (!exp || !exp->exp_obd || !exp->exp_obd->obd_proc_exports_entry ||
            !exp->exp_obd->obd_nid_stats_hash)
                RETURN(-EINVAL);

        /* not test against zero because eric say:
         * You may only test nid against another nid, or LNET_NID_ANY.
         * Anything else is nonsense.*/
        if (!nid || *nid == LNET_NID_ANY)
                RETURN(0);

        obd = exp->exp_obd;

        CDEBUG(D_CONFIG, "using hash %p\n", obd->obd_nid_stats_hash);

        OBD_ALLOC_PTR(new_stat);
        if (new_stat == NULL)
                RETURN(-ENOMEM);

        new_stat->nid = *nid;
        new_stat->nid_obd = exp->exp_obd;
        /* we need set default refcount to 1 to balance obd_disconnect() */
        atomic_set(&new_stat->nid_exp_ref_count, 1);

        old_stat = lustre_hash_findadd_unique(obd->obd_nid_stats_hash,
                                              nid, &new_stat->nid_hash);
        CDEBUG(D_INFO, "Found stats %p for nid %s - ref %d\n",
               old_stat, libcfs_nid2str(*nid),
               atomic_read(&new_stat->nid_exp_ref_count));

        /* Return -EALREADY here so that we know that the /proc
         * entry already has been created */
        if (old_stat != new_stat) {
                /* if this reconnect to live export from diffrent nid, we need
                 * to release old stats because disconnect will be never called.
                 * */
                if (reconnect && exp->exp_nid_stats)
                        nidstat_putref(exp->exp_nid_stats);

                exp->exp_nid_stats = old_stat;
                GOTO(destroy_new, rc = -EALREADY);
        }

        /* not found - create */
        OBD_ALLOC(buffer, LNET_NIDSTR_SIZE);
        if (buffer == NULL)
                GOTO(destroy_new_ns, rc = -ENOMEM);

        memcpy(buffer, libcfs_nid2str(*nid), LNET_NIDSTR_SIZE);
        new_stat->nid_proc = proc_mkdir(buffer, obd->obd_proc_exports_entry);
        OBD_FREE(buffer, LNET_NIDSTR_SIZE);

        if (!new_stat->nid_proc) {
                CERROR("Error making export directory for"
                       " nid %s\n", libcfs_nid2str(*nid));
                GOTO(destroy_new_ns, rc = -ENOMEM);
        }

        entry = lprocfs_add_simple(new_stat->nid_proc, "uuid",
                                   lprocfs_exp_rd_uuid, NULL, new_stat, NULL);
        if (IS_ERR(entry)) {
                CWARN("Error adding the uuid file\n");
                rc = PTR_ERR(entry);
                GOTO(destroy_new_ns, rc);
        }

        entry = lprocfs_add_simple(new_stat->nid_proc, "hash",
                                lprocfs_exp_rd_hash, NULL, new_stat, NULL);
        if (IS_ERR(entry)) {
                CWARN("Error adding the hash file\n");
                rc = PTR_ERR(entry);
                GOTO(destroy_new_ns, rc);
        }

        exp->exp_nid_stats = new_stat;
        *newnid = 1;
        /* protect competitive add to list, not need locking on destroy */
        spin_lock(&obd->obd_nid_lock);
        list_add(&new_stat->nid_list, &obd->obd_nid_stats);
        spin_unlock(&obd->obd_nid_lock);

        RETURN(rc);

destroy_new_ns:
        if (new_stat->nid_proc != NULL)
                lprocfs_remove(&new_stat->nid_proc);
        lustre_hash_del(obd->obd_nid_stats_hash, nid, &new_stat->nid_hash);

destroy_new:
        nidstat_putref(new_stat);
        OBD_FREE_PTR(new_stat);
        RETURN(rc);
}

int lprocfs_exp_cleanup(struct obd_export *exp)
{
        struct nid_stat *stat = exp->exp_nid_stats;

        if(!stat || !exp->exp_obd)
                RETURN(0);

        nidstat_putref(exp->exp_nid_stats);
        exp->exp_nid_stats = NULL;

        return 0;
}

int lprocfs_write_helper(const char *buffer, unsigned long count,
                         int *val)
{
        return lprocfs_write_frac_helper(buffer, count, val, 1);
}

int lprocfs_write_frac_helper(const char *buffer, unsigned long count,
                              int *val, int mult)
{
        char kernbuf[20], *end, *pbuf;

        if (count > (sizeof(kernbuf) - 1))
                return -EINVAL;

        if (copy_from_user(kernbuf, buffer, count))
                return -EFAULT;

        kernbuf[count] = '\0';
        pbuf = kernbuf;
        if (*pbuf == '-') {
                mult = -mult;
                pbuf++;
        }

        *val = (int)simple_strtoul(pbuf, &end, 10) * mult;
        if (pbuf == end)
                return -EINVAL;

        if (end != NULL && *end == '.') {
                int temp_val, pow = 1;
                int i;

                pbuf = end + 1;
                if (strlen(pbuf) > 5)
                        pbuf[5] = '\0'; /*only allow 5bits fractional*/

                temp_val = (int)simple_strtoul(pbuf, &end, 10) * mult;

                if (pbuf < end) {
                        for (i = 0; i < (end - pbuf); i++)
                                pow *= 10;

                        *val += temp_val / pow;
                }
        }
        return 0;
}

int lprocfs_read_frac_helper(char *buffer, unsigned long count, long val,
                             int mult)
{
        long decimal_val, frac_val;
        int prtn;

        if (count < 10)
                return -EINVAL;

        decimal_val = val / mult;
        prtn = snprintf(buffer, count, "%ld", decimal_val);
        frac_val = val % mult;

        if (prtn < (count - 4) && frac_val > 0) {
                long temp_frac;
                int i, temp_mult = 1, frac_bits = 0;

                temp_frac = frac_val * 10;
                buffer[prtn++] = '.';
                while (frac_bits < 2 && (temp_frac / mult) < 1 ) {
                        /*only reserved 2bits fraction*/
                        buffer[prtn++] ='0';
                        temp_frac *= 10;
                        frac_bits++;
                }
                /*
                  Need to think these cases :
                        1. #echo x.00 > /proc/xxx       output result : x
                        2. #echo x.0x > /proc/xxx       output result : x.0x
                        3. #echo x.x0 > /proc/xxx       output result : x.x
                        4. #echo x.xx > /proc/xxx       output result : x.xx
                        Only reserved 2bits fraction.
                 */
                for (i = 0; i < (5 - prtn); i++)
                        temp_mult *= 10;

                frac_bits = min((int)count - prtn, 3 - frac_bits);
                prtn += snprintf(buffer + prtn, frac_bits, "%ld", frac_val * temp_mult / mult);

                prtn--;
                while(buffer[prtn] < '1' || buffer[prtn] > '9') {
                        prtn--;
                        if (buffer[prtn] == '.') {
                                prtn--;
                                break;
                        }
                }
                prtn++;
        }
        buffer[prtn++] ='\n';
        return prtn;
}

int lprocfs_write_u64_helper(const char *buffer, unsigned long count,__u64 *val)
{
        return lprocfs_write_frac_u64_helper(buffer, count, val, 1);
}

int lprocfs_write_frac_u64_helper(const char *buffer, unsigned long count,
                              __u64 *val, int mult)
{
        char kernbuf[22], *end, *pbuf;
        __u64 whole, frac = 0, units;
        unsigned frac_d = 1;

        if (count > (sizeof(kernbuf) - 1))
                return -EINVAL;

        if (copy_from_user(kernbuf, buffer, count))
                return -EFAULT;

        kernbuf[count] = '\0';
        pbuf = kernbuf;
        if (*pbuf == '-') {
                mult = -mult;
                pbuf++;
        }

        whole = simple_strtoull(pbuf, &end, 10);
        if (pbuf == end)
                return -EINVAL;

        if (end != NULL && *end == '.') {
                int i;
                pbuf = end + 1;

                /* need to limit frac_d to a __u32 */
                if (strlen(pbuf) > 10)
                        pbuf[10] = '\0';

                frac = simple_strtoull(pbuf, &end, 10);
                /* count decimal places */
                for (i = 0; i < (end - pbuf); i++)
                        frac_d *= 10;
        }

        units = 1;
        switch(*end) {
        case 'p': case 'P':
                units <<= 10;
        case 't': case 'T':
                units <<= 10;
        case 'g': case 'G':
                units <<= 10;
        case 'm': case 'M':
                units <<= 10;
        case 'k': case 'K':
                units <<= 10;
        }
        /* Specified units override the multiplier */
        if (units)
                mult = mult < 0 ? -units : units;

        frac *= mult;
        do_div(frac, frac_d);
        *val = whole * mult + frac;
        return 0;
}

int lprocfs_seq_create(cfs_proc_dir_entry_t *parent,
                       char *name, mode_t mode,
                       struct file_operations *seq_fops, void *data)
{
        struct proc_dir_entry *entry;
        ENTRY;

        entry = create_proc_entry(name, mode, parent);
        if (entry == NULL)
                RETURN(-ENOMEM);
        entry->proc_fops = seq_fops;
        entry->data = data;

        RETURN(0);
}
EXPORT_SYMBOL(lprocfs_seq_create);

__inline__ int lprocfs_obd_seq_create(struct obd_device *dev, char *name,
                                      mode_t mode,
                                      struct file_operations *seq_fops,
                                      void *data)
{
        return (lprocfs_seq_create(dev->obd_proc_entry, name,
                                   mode, seq_fops, data));
}
EXPORT_SYMBOL(lprocfs_obd_seq_create);

void lprocfs_oh_tally(struct obd_histogram *oh, unsigned int value)
{
        if (value >= OBD_HIST_MAX)
                value = OBD_HIST_MAX - 1;

        spin_lock(&oh->oh_lock);
        oh->oh_buckets[value]++;
        spin_unlock(&oh->oh_lock);
}
EXPORT_SYMBOL(lprocfs_oh_tally);

void lprocfs_oh_tally_log2(struct obd_histogram *oh, unsigned int value)
{
        unsigned int val;

        for (val = 0; ((1 << val) < value) && (val <= OBD_HIST_MAX); val++)
                ;

        lprocfs_oh_tally(oh, val);
}
EXPORT_SYMBOL(lprocfs_oh_tally_log2);

unsigned long lprocfs_oh_sum(struct obd_histogram *oh)
{
        unsigned long ret = 0;
        int i;

        for (i = 0; i < OBD_HIST_MAX; i++)
                ret +=  oh->oh_buckets[i];
        return ret;
}
EXPORT_SYMBOL(lprocfs_oh_sum);

void lprocfs_oh_clear(struct obd_histogram *oh)
{
        spin_lock(&oh->oh_lock);
        memset(oh->oh_buckets, 0, sizeof(oh->oh_buckets));
        spin_unlock(&oh->oh_lock);
}
EXPORT_SYMBOL(lprocfs_oh_clear);

int lprocfs_obd_rd_recovery_status(char *page, char **start, off_t off,
                                   int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        int len = 0, size;

        LASSERT(obd != NULL);
        LASSERT(count >= 0);

        /* Set start of user data returned to
           page + off since the user may have
           requested to read much smaller than
           what we need to read */
        *start = page + off;

        /* We know we are allocated a page here.
           Also we know that this function will
           not need to write more than a page
           so we can truncate at CFS_PAGE_SIZE.  */
        size = min(count + (int)off + 1, (int)CFS_PAGE_SIZE);

        /* Initialize the page */
        memset(page, 0, size);

        if (lprocfs_obd_snprintf(&page, size, &len, "status: ") <= 0)
                goto out;
        if (obd->obd_max_recoverable_clients == 0) {
                if (lprocfs_obd_snprintf(&page, size, &len, "INACTIVE\n") <= 0)
                        goto out;

                goto fclose;
        }

        /* sampled unlocked, but really... */
        if (obd->obd_recovering == 0) {
                if (lprocfs_obd_snprintf(&page, size, &len, "COMPLETE\n") <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "recovery_start: %lu\n",
                                         obd->obd_recovery_start) <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "recovery_duration: %lu\n",
                                         obd->obd_recovery_end -
                                         obd->obd_recovery_start) <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "delayed_clients: %d/%d\n",
                                         obd->obd_delayed_clients,
                                         obd->obd_max_recoverable_clients) <= 0)
                        goto out;
                /* Number of clients that have completed recovery */
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "completed_clients: %d/%d\n",
                                         obd->obd_max_recoverable_clients -
                                         obd->obd_recoverable_clients -
                                         obd->obd_delayed_clients -
                                         obd->obd_stale_clients,
                                         obd->obd_max_recoverable_clients) <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "replayed_requests: %d\n",
                                         obd->obd_replayed_requests) <= 0)
                        goto out;
                if (lprocfs_obd_snprintf(&page, size, &len,
                                         "last_transno: "LPD64"\n",
                                         obd->obd_next_recovery_transno - 1)<=0)
                        goto out;
                goto fclose;
        }

        if (lprocfs_obd_snprintf(&page, size, &len, "RECOVERING\n") <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len, "recovery_start: %lu\n",
                                 obd->obd_recovery_start) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len, "time_remaining: %lu\n",
                           cfs_time_current_sec() >= obd->obd_recovery_end ? 0 :
                           obd->obd_recovery_end - cfs_time_current_sec()) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len,"connected_clients: %d/%d\n",
                                 obd->obd_connected_clients,
                                 obd->obd_max_recoverable_clients) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len,"delayed_clients: %d/%d\n",
                                 obd->obd_delayed_clients,
                                 obd->obd_max_recoverable_clients) <= 0)
                goto out;
        /* Number of clients that have completed recovery */
        if (lprocfs_obd_snprintf(&page, size, &len,"completed_clients: %d/%d\n",
                                 obd->obd_max_recoverable_clients -
                                 obd->obd_recoverable_clients -
                                 obd->obd_delayed_clients,
                                 obd->obd_max_recoverable_clients) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len,"replayed_requests: %d/??\n",
                                 obd->obd_replayed_requests) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len, "queued_requests: %d\n",
                                 obd->obd_requests_queued_for_recovery) <= 0)
                goto out;
        if (lprocfs_obd_snprintf(&page, size, &len, "next_transno: "LPD64"\n",
                                 obd->obd_next_recovery_transno) <= 0)
                goto out;

fclose:
        *eof = 1;
out:
        return min(count, len - (int)off);
}
EXPORT_SYMBOL(lprocfs_obd_rd_recovery_status);

int lprocfs_obd_rd_hash(char *page, char **start, off_t off,
                        int count, int *eof, void *data)
{
        struct obd_device *obd = data;
        int c = 0;

        if (obd == NULL ||
            obd->obd_uuid_hash == NULL ||
            obd->obd_nid_hash == NULL ||
            obd->obd_nid_stats_hash == NULL)
                return 0;

        c += lustre_hash_debug_header(page, count);
        c += lustre_hash_debug_str(obd->obd_uuid_hash, page + c, count - c);
        c += lustre_hash_debug_str(obd->obd_nid_hash, page + c, count - c);
        c += lustre_hash_debug_str(obd->obd_nid_stats_hash, page+c, count-c);

        return c;
}
EXPORT_SYMBOL(lprocfs_obd_rd_hash);

int lprocfs_obd_rd_recovery_time_soft(char *page, char **start, off_t off,
                                      int count, int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        LASSERT(obd != NULL);

        return snprintf(page, count, "%d\n",
                        obd->obd_recovery_timeout);
}
EXPORT_SYMBOL(lprocfs_obd_rd_recovery_time_soft);

int lprocfs_obd_wr_recovery_time_soft(struct file *file, const char *buffer,
                                      unsigned long count, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        int val, rc;
        LASSERT(obd != NULL);

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        obd->obd_recovery_timeout = val;
        return count;
}
EXPORT_SYMBOL(lprocfs_obd_wr_recovery_time_soft);

int lprocfs_obd_rd_recovery_time_hard(char *page, char **start, off_t off,
                                      int count, int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        LASSERT(obd != NULL);

        return snprintf(page, count, "%lu\n",
                        obd->obd_recovery_time_hard);
}
EXPORT_SYMBOL(lprocfs_obd_rd_recovery_time_hard);

int lprocfs_obd_wr_recovery_time_hard(struct file *file, const char *buffer,
                                      unsigned long count, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        int val, rc;
        LASSERT(obd != NULL);

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        obd->obd_recovery_time_hard = val;
        return count;
}
EXPORT_SYMBOL(lprocfs_obd_wr_recovery_time_hard);

#ifdef HAVE_DELAYED_RECOVERY
int lprocfs_obd_rd_stale_export_age(char *page, char **start, off_t off,
                                    int count, int *eof, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        LASSERT(obd != NULL);

        return snprintf(page, count, "%u\n",
                        obd->u.obt.obt_stale_export_age);
}
EXPORT_SYMBOL(lprocfs_obd_rd_stale_export_age);

int lprocfs_obd_wr_stale_export_age(struct file *file, const char *buffer,
                                    unsigned long count, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        int val, rc;
        LASSERT(obd != NULL);

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        target_trans_table_recalc(obd, val);
        obd->u.obt.obt_stale_export_age = val;
        return count;
}
EXPORT_SYMBOL(lprocfs_obd_wr_stale_export_age);

static int obd_stale_exports_seq_show(struct seq_file *seq, void *v)
{
        struct obd_device *obd = seq->private;
        struct obd_export *exp;

        spin_lock(&obd->obd_dev_lock);
        list_for_each_entry(exp, &obd->obd_delayed_exports,
                            exp_obd_chain) {
                seq_printf(seq, "%s: %ld seconds ago%s\n",
                           obd_uuid2str(&exp->exp_client_uuid),
                           cfs_time_current_sec() - exp->exp_last_request_time,
                           exp_expired(exp, obd->u.obt.obt_stale_export_age) ?
                                       " [EXPIRED]" : "");
        }
        spin_unlock(&obd->obd_dev_lock);
        return 0;
}

LPROC_SEQ_FOPS_RO(obd_stale_exports);

int lprocfs_obd_attach_stale_exports(struct obd_device *dev)
{
        return lprocfs_obd_seq_create(dev, "stale_exports", 0444,
                                      &obd_stale_exports_fops, dev);
}
EXPORT_SYMBOL(lprocfs_obd_attach_stale_exports);

int lprocfs_obd_wr_flush_stale_exports(struct file *file, const char *buffer,
                                       unsigned long count, void *data)
{
        struct obd_device *obd = (struct obd_device *)data;
        int val, rc;
        LASSERT(obd != NULL);

        rc = lprocfs_write_helper(buffer, count, &val);
        if (rc)
                return rc;

        class_disconnect_expired_exports(obd);
        return count;
}
EXPORT_SYMBOL(lprocfs_obd_wr_flush_stale_exports);
#endif

EXPORT_SYMBOL(lprocfs_register);
EXPORT_SYMBOL(lprocfs_srch);
EXPORT_SYMBOL(lprocfs_remove);
EXPORT_SYMBOL(lprocfs_add_vars);
EXPORT_SYMBOL(lprocfs_obd_setup);
EXPORT_SYMBOL(lprocfs_obd_cleanup);
EXPORT_SYMBOL(lprocfs_add_simple);
EXPORT_SYMBOL(lprocfs_free_per_client_stats);
EXPORT_SYMBOL(lprocfs_alloc_stats);
EXPORT_SYMBOL(lprocfs_free_stats);
EXPORT_SYMBOL(lprocfs_clear_stats);
EXPORT_SYMBOL(lprocfs_register_stats);
EXPORT_SYMBOL(lprocfs_init_ops_stats);
EXPORT_SYMBOL(lprocfs_init_ldlm_stats);
EXPORT_SYMBOL(lprocfs_alloc_obd_stats);
EXPORT_SYMBOL(lprocfs_free_obd_stats);
EXPORT_SYMBOL(lprocfs_exp_setup);
EXPORT_SYMBOL(lprocfs_exp_cleanup);

EXPORT_SYMBOL(lprocfs_rd_u64);
EXPORT_SYMBOL(lprocfs_rd_atomic);
EXPORT_SYMBOL(lprocfs_wr_atomic);
EXPORT_SYMBOL(lprocfs_rd_uint);
EXPORT_SYMBOL(lprocfs_wr_uint);
EXPORT_SYMBOL(lprocfs_rd_uuid);
EXPORT_SYMBOL(lprocfs_rd_name);
EXPORT_SYMBOL(lprocfs_rd_fstype);
EXPORT_SYMBOL(lprocfs_rd_server_uuid);
EXPORT_SYMBOL(lprocfs_rd_conn_uuid);
EXPORT_SYMBOL(lprocfs_rd_num_exports);
EXPORT_SYMBOL(lprocfs_rd_numrefs);
EXPORT_SYMBOL(lprocfs_at_hist_helper);
EXPORT_SYMBOL(lprocfs_rd_import);
EXPORT_SYMBOL(lprocfs_rd_state);
EXPORT_SYMBOL(lprocfs_rd_timeouts);
EXPORT_SYMBOL(lprocfs_rd_blksize);
EXPORT_SYMBOL(lprocfs_rd_kbytestotal);
EXPORT_SYMBOL(lprocfs_rd_kbytesfree);
EXPORT_SYMBOL(lprocfs_rd_kbytesavail);
EXPORT_SYMBOL(lprocfs_rd_filestotal);
EXPORT_SYMBOL(lprocfs_rd_filesfree);

EXPORT_SYMBOL(lprocfs_write_helper);
EXPORT_SYMBOL(lprocfs_write_frac_helper);
EXPORT_SYMBOL(lprocfs_read_frac_helper);
EXPORT_SYMBOL(lprocfs_write_u64_helper);
EXPORT_SYMBOL(lprocfs_write_frac_u64_helper);
EXPORT_SYMBOL(lprocfs_stats_collect);
#endif /* LPROCFS*/
