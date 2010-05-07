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
 * lustre/include/lustre_lib.h
 *
 * Basic Lustre library routines.
 */

#ifndef _LUSTRE_LIB_H
#define _LUSTRE_LIB_H

#include <libcfs/kp30.h>
#include <lustre/lustre_idl.h>
#include <lustre_ver.h>
#include <lustre_cfg.h>
#if defined(__linux__)
#include <linux/lustre_lib.h>
#elif defined(__APPLE__)
#include <darwin/lustre_lib.h>
#elif defined(__WINNT__)
#include <winnt/lustre_lib.h>
#else
#error Unsupported operating system.
#endif

/* prng.c */
unsigned int ll_rand(void);        /* returns a random 32-bit integer */
void ll_srand(unsigned int, unsigned int);     /* seed the generator */
void ll_get_random_bytes(void *buf, int size);

/* target.c */
struct ptlrpc_request;
struct recovd_data;
struct recovd_obd;
struct obd_export;
#include <lustre_ha.h>
#include <lustre_net.h>
#include <lvfs.h>

void target_client_add_cb(struct obd_device *obd, __u64 transno, void *cb_data,
                          int error);
void target_trans_table_init(struct obd_device *obd);
__u32 target_trans_table_last_time(struct obd_export *exp);
void target_trans_table_recalc(struct obd_device *obd, __u32 new_age);
void target_trans_table_update(struct obd_export *exp, __u64 transno);
#ifdef __KERNEL__
int target_fs_version_capable(struct obd_device *obd);
#else
static inline int target_fs_version_capable(struct obd_device *obd)
{
        return 0;
}
#endif
int target_handle_connect(struct ptlrpc_request *req, svc_handler_t handler);
int target_handle_disconnect(struct ptlrpc_request *req);
void target_destroy_export(struct obd_export *exp);
int target_handle_ping(struct ptlrpc_request *req);
int target_pack_pool_reply(struct ptlrpc_request *req);
void target_committed_to_req(struct ptlrpc_request *req);

/* quotacheck callback, dqacq/dqrel callback handler */
int target_handle_qc_callback(struct ptlrpc_request *req);
#ifdef HAVE_QUOTA_SUPPORT
int target_handle_dqacq_callback(struct ptlrpc_request *req);
#else
#define target_handle_dqacq_callback(req) ldlm_callback_reply(req, -ENOTSUPP)
#endif

void target_cancel_recovery_timer(struct obd_device *obd);
void target_abort_recovery(void *data);
int target_recovery_check_and_stop(struct obd_device *obd);
void target_cleanup_recovery(struct obd_device *obd);
int target_queue_recovery_request(struct ptlrpc_request *req,
                                  struct obd_device *obd);
int target_handle_reply(struct ptlrpc_request *req, int rc, int fail);
void target_send_reply(struct ptlrpc_request *req, int rc, int fail_id);

/* client.c */

int client_sanobd_setup(struct obd_device *obddev, obd_count len, void *buf);
struct client_obd *client_conn2cli(struct lustre_handle *conn);

struct mdc_open_data;
struct obd_client_handle {
        struct lustre_handle och_fh;
        struct llog_cookie och_cookie;
        struct mdc_open_data *och_mod;
        __u32 och_magic;
};
#define OBD_CLIENT_HANDLE_MAGIC 0xd15ea5ed

/* statfs_pack.c */
void statfs_pack(struct obd_statfs *osfs, struct kstatfs *sfs);
void statfs_unpack(struct kstatfs *sfs, struct obd_statfs *osfs);

/* l_lock.c */
struct lustre_lock {
        int l_depth;
        cfs_task_t *l_owner;
        struct semaphore l_sem;
        spinlock_t l_spin;
};

void l_lock_init(struct lustre_lock *);
void l_lock(struct lustre_lock *);
void l_unlock(struct lustre_lock *);
int l_has_lock(struct lustre_lock *);


/*
 *   OBD IOCTLS
 */
#define OBD_IOCTL_VERSION 0x00010004

struct obd_ioctl_data {
        __u32 ioc_len;
        __u32 ioc_version;

        __u64 ioc_cookie;
        __u32 ioc_conn1;
        __u32 ioc_conn2;

        struct obdo ioc_obdo1;
        struct obdo ioc_obdo2;

        obd_size         ioc_count;
        obd_off          ioc_offset;
        __u32            ioc_dev;
        __u32            ioc_command;

        __u64 ioc_nid;
        __u32 ioc_nal;
        __u32 ioc_type;

        /* buffers the kernel will treat as user pointers */
        __u32    ioc_plen1;
        char    *ioc_pbuf1;
        __u32    ioc_plen2;
        char    *ioc_pbuf2;

        /* inline buffers for various arguments */
        __u32    ioc_inllen1;
        char    *ioc_inlbuf1;
        __u32    ioc_inllen2;
        char    *ioc_inlbuf2;
        __u32    ioc_inllen3;
        char    *ioc_inlbuf3;
        __u32    ioc_inllen4;
        char    *ioc_inlbuf4;

        char    ioc_bulk[0];
};

struct obd_ioctl_hdr {
        __u32    ioc_len;
        __u32    ioc_version;
};

static inline int obd_ioctl_packlen(struct obd_ioctl_data *data)
{
        int len = size_round(sizeof(struct obd_ioctl_data));
        len += size_round(data->ioc_inllen1);
        len += size_round(data->ioc_inllen2);
        len += size_round(data->ioc_inllen3);
        len += size_round(data->ioc_inllen4);
        return len;
}


static inline int obd_ioctl_is_invalid(struct obd_ioctl_data *data)
{
        if (data->ioc_len > (1<<30)) {
                CERROR("OBD ioctl: ioc_len larger than 1<<30\n");
                return 1;
        }
        if (data->ioc_inllen1 > (1<<30)) {
                CERROR("OBD ioctl: ioc_inllen1 larger than 1<<30\n");
                return 1;
        }
        if (data->ioc_inllen2 > (1<<30)) {
                CERROR("OBD ioctl: ioc_inllen2 larger than 1<<30\n");
                return 1;
        }
        if (data->ioc_inllen3 > (1<<30)) {
                CERROR("OBD ioctl: ioc_inllen3 larger than 1<<30\n");
                return 1;
        }
        if (data->ioc_inllen4 > (1<<30)) {
                CERROR("OBD ioctl: ioc_inllen4 larger than 1<<30\n");
                return 1;
        }
        if (data->ioc_inlbuf1 && !data->ioc_inllen1) {
                CERROR("OBD ioctl: inlbuf1 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_inlbuf2 && !data->ioc_inllen2) {
                CERROR("OBD ioctl: inlbuf2 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_inlbuf3 && !data->ioc_inllen3) {
                CERROR("OBD ioctl: inlbuf3 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_inlbuf4 && !data->ioc_inllen4) {
                CERROR("OBD ioctl: inlbuf4 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_pbuf1 && !data->ioc_plen1) {
                CERROR("OBD ioctl: pbuf1 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_pbuf2 && !data->ioc_plen2) {
                CERROR("OBD ioctl: pbuf2 pointer but 0 length\n");
                return 1;
        }
        if (data->ioc_plen1 && !data->ioc_pbuf1) {
                CERROR("OBD ioctl: plen1 set but NULL pointer\n");
                return 1;
        }
        if (data->ioc_plen2 && !data->ioc_pbuf2) {
                CERROR("OBD ioctl: plen2 set but NULL pointer\n");
                return 1;
        }
        if (obd_ioctl_packlen(data) > data->ioc_len) {
                CERROR("OBD ioctl: packlen exceeds ioc_len (%d > %d)\n",
                       obd_ioctl_packlen(data), data->ioc_len);
                return 1;
        }
        return 0;
}

#ifndef __KERNEL__
static inline int obd_ioctl_pack(struct obd_ioctl_data *data, char **pbuf,
                                 int max)
{
        char *ptr;
        struct obd_ioctl_data *overlay;
        data->ioc_len = obd_ioctl_packlen(data);
        data->ioc_version = OBD_IOCTL_VERSION;

        if (*pbuf && data->ioc_len > max)
                return -EINVAL;
        if (*pbuf == NULL) {
                *pbuf = malloc(data->ioc_len);
        }
        if (!*pbuf)
                return -ENOMEM;
        overlay = (struct obd_ioctl_data *)*pbuf;
        memcpy(*pbuf, data, sizeof(*data));

        ptr = overlay->ioc_bulk;
        if (data->ioc_inlbuf1)
                LOGL(data->ioc_inlbuf1, data->ioc_inllen1, ptr);
        if (data->ioc_inlbuf2)
                LOGL(data->ioc_inlbuf2, data->ioc_inllen2, ptr);
        if (data->ioc_inlbuf3)
                LOGL(data->ioc_inlbuf3, data->ioc_inllen3, ptr);
        if (data->ioc_inlbuf4)
                LOGL(data->ioc_inlbuf4, data->ioc_inllen4, ptr);
        if (obd_ioctl_is_invalid(overlay))
                return -EINVAL;

        return 0;
}

static inline int obd_ioctl_unpack(struct obd_ioctl_data *data, char *pbuf,
                                   int max)
{
        char *ptr;
        struct obd_ioctl_data *overlay;

        if (!pbuf)
                return 1;
        overlay = (struct obd_ioctl_data *)pbuf;

        /* Preserve the caller's buffer pointers */
        overlay->ioc_inlbuf1 = data->ioc_inlbuf1;
        overlay->ioc_inlbuf2 = data->ioc_inlbuf2;
        overlay->ioc_inlbuf3 = data->ioc_inlbuf3;
        overlay->ioc_inlbuf4 = data->ioc_inlbuf4;

        memcpy(data, pbuf, sizeof(*data));

        ptr = overlay->ioc_bulk;
        if (data->ioc_inlbuf1)
                LOGU(data->ioc_inlbuf1, data->ioc_inllen1, ptr);
        if (data->ioc_inlbuf2)
                LOGU(data->ioc_inlbuf2, data->ioc_inllen2, ptr);
        if (data->ioc_inlbuf3)
                LOGU(data->ioc_inlbuf3, data->ioc_inllen3, ptr);
        if (data->ioc_inlbuf4)
                LOGU(data->ioc_inlbuf4, data->ioc_inllen4, ptr);

        return 0;
}
#endif

#include <obd_support.h>

#ifdef __KERNEL__
/* function defined in lustre/obdclass/<platform>/<platform>-module.c */
int obd_ioctl_getdata(char **buf, int *len, void *arg);
int obd_ioctl_popdata(void *arg, void *data, int len);
#else
/* buffer MUST be at least the size of obd_ioctl_hdr */
static inline int obd_ioctl_getdata(char **buf, int *len, void *arg)
{
        struct obd_ioctl_hdr hdr;
        struct obd_ioctl_data *data;
        int err;
        int offset = 0;
        ENTRY;

        err = copy_from_user(&hdr, (void *)arg, sizeof(hdr));
        if (err) 
                RETURN(err);

        if (hdr.ioc_version != OBD_IOCTL_VERSION) {
                CERROR("Version mismatch kernel vs application\n");
                RETURN(-EINVAL);
        }

        if (hdr.ioc_len > OBD_MAX_IOCTL_BUFFER) {
                CERROR("User buffer len %d exceeds %d max buffer\n",
                       hdr.ioc_len, OBD_MAX_IOCTL_BUFFER);
                RETURN(-EINVAL);
        }

        if (hdr.ioc_len < sizeof(struct obd_ioctl_data)) {
                CERROR("User buffer too small for ioctl (%d)\n", hdr.ioc_len);
                RETURN(-EINVAL);
        }

        /* XXX allocate this more intelligently, using kmalloc when
         * appropriate */
        OBD_VMALLOC(*buf, hdr.ioc_len);
        if (*buf == NULL) {
                CERROR("Cannot allocate control buffer of len %d\n",
                       hdr.ioc_len);
                RETURN(-EINVAL);
        }
        *len = hdr.ioc_len;
        data = (struct obd_ioctl_data *)*buf;

        err = copy_from_user(*buf, (void *)arg, hdr.ioc_len);
        if (err) {
                OBD_VFREE(*buf, hdr.ioc_len);
                RETURN(err);
        }

        if (obd_ioctl_is_invalid(data)) {
                CERROR("ioctl not correctly formatted\n");
                OBD_VFREE(*buf, hdr.ioc_len);
                RETURN(-EINVAL);
        }

        if (data->ioc_inllen1) {
                data->ioc_inlbuf1 = &data->ioc_bulk[0];
                offset += size_round(data->ioc_inllen1);
        }

        if (data->ioc_inllen2) {
                data->ioc_inlbuf2 = &data->ioc_bulk[0] + offset;
                offset += size_round(data->ioc_inllen2);
        }

        if (data->ioc_inllen3) {
                data->ioc_inlbuf3 = &data->ioc_bulk[0] + offset;
                offset += size_round(data->ioc_inllen3);
        }

        if (data->ioc_inllen4) {
                data->ioc_inlbuf4 = &data->ioc_bulk[0] + offset;
        }

        RETURN(0);
}

static inline int obd_ioctl_popdata(void *arg, void *data, int len)
{
        int err = copy_to_user(arg, data, len);
        if (err)
                err = -EFAULT;
        return err;
}
#endif

static inline void obd_ioctl_freedata(char *buf, int len)
{
        ENTRY;

        OBD_VFREE(buf, len);
        EXIT;
        return;
}

/*
 * BSD ioctl description:
 * #define IOC_V1       _IOR(g, n1, long)
 * #define IOC_V2       _IOW(g, n2, long)
 *
 * ioctl(f, IOC_V1, arg);
 * arg will be treated as a long value,
 *
 * ioctl(f, IOC_V2, arg)
 * arg will be treated as a pointer, bsd will call
 * copyin(buf, arg, sizeof(long))
 *
 * To make BSD ioctl handles argument correctly and simplely, 
 * we change _IOR to _IOWR so BSD will copyin obd_ioctl_data 
 * for us. Does this change affect Linux?  (XXX Liang)
 */
#define OBD_IOC_CREATE                 _IOWR('f', 101, OBD_IOC_DATA_TYPE)
#define OBD_IOC_DESTROY                _IOW ('f', 104, OBD_IOC_DATA_TYPE)
#define OBD_IOC_PREALLOCATE            _IOWR('f', 105, OBD_IOC_DATA_TYPE)

#define OBD_IOC_SETATTR                _IOW ('f', 107, OBD_IOC_DATA_TYPE)
#define OBD_IOC_GETATTR                _IOWR ('f', 108, OBD_IOC_DATA_TYPE)
#define OBD_IOC_READ                   _IOWR('f', 109, OBD_IOC_DATA_TYPE)
#define OBD_IOC_WRITE                  _IOWR('f', 110, OBD_IOC_DATA_TYPE)


#define OBD_IOC_STATFS                 _IOWR('f', 113, OBD_IOC_DATA_TYPE)
#define OBD_IOC_SYNC                   _IOW ('f', 114, OBD_IOC_DATA_TYPE)
#define OBD_IOC_READ2                  _IOWR('f', 115, OBD_IOC_DATA_TYPE)
#define OBD_IOC_FORMAT                 _IOWR('f', 116, OBD_IOC_DATA_TYPE)
#define OBD_IOC_PARTITION              _IOWR('f', 117, OBD_IOC_DATA_TYPE)
#define OBD_IOC_COPY                   _IOWR('f', 120, OBD_IOC_DATA_TYPE)
#define OBD_IOC_MIGR                   _IOWR('f', 121, OBD_IOC_DATA_TYPE)
#define OBD_IOC_PUNCH                  _IOWR('f', 122, OBD_IOC_DATA_TYPE)

#define OBD_IOC_MODULE_DEBUG           _IOWR('f', 124, OBD_IOC_DATA_TYPE)
#define OBD_IOC_BRW_READ               _IOWR('f', 125, OBD_IOC_DATA_TYPE)
#define OBD_IOC_BRW_WRITE              _IOWR('f', 126, OBD_IOC_DATA_TYPE)
#define OBD_IOC_NAME2DEV               _IOWR('f', 127, OBD_IOC_DATA_TYPE)
#define OBD_IOC_UUID2DEV               _IOWR('f', 130, OBD_IOC_DATA_TYPE)
/* OBD_IOC_GETNAME_OLD is for compatibility with 1.4.x */
#define OBD_IOC_GETNAME_OLD            _IOR ('f', 131, OBD_IOC_DATA_TYPE)
#define OBD_IOC_GETNAME                _IOWR('f', 131, OBD_IOC_DATA_TYPE)

#define OBD_IOC_LOV_GET_CONFIG         _IOWR('f', 132, OBD_IOC_DATA_TYPE)
#define OBD_IOC_CLIENT_RECOVER         _IOW ('f', 133, OBD_IOC_DATA_TYPE)
#define OBD_IOC_PING_TARGET            _IOW ('f', 136, OBD_IOC_DATA_TYPE)

#define OBD_IOC_DEC_FS_USE_COUNT       _IO  ('f', 139      )
#define OBD_IOC_NO_TRANSNO             _IOW ('f', 140, OBD_IOC_DATA_TYPE)
#define OBD_IOC_SET_READONLY           _IOW ('f', 141, OBD_IOC_DATA_TYPE)
#define OBD_IOC_ABORT_RECOVERY         _IOR ('f', 142, OBD_IOC_DATA_TYPE)

#define OBD_GET_VERSION                _IOWR ('f', 144, OBD_IOC_DATA_TYPE)

#define OBD_IOC_CLOSE_UUID             _IOWR ('f', 147, OBD_IOC_DATA_TYPE)

#define OBD_IOC_GETDEVICE              _IOWR ('f', 149, OBD_IOC_DATA_TYPE)

#define OBD_IOC_LOV_SETSTRIPE          _IOW ('f', 154, OBD_IOC_DATA_TYPE)
#define OBD_IOC_LOV_GETSTRIPE          _IOW ('f', 155, OBD_IOC_DATA_TYPE)
#define OBD_IOC_LOV_SETEA              _IOW ('f', 156, OBD_IOC_DATA_TYPE)

#define OBD_IOC_QUOTACHECK             _IOW ('f', 160, int)
#define OBD_IOC_POLL_QUOTACHECK        _IOR ('f', 161, struct if_quotacheck *)
#define OBD_IOC_QUOTACTL               _IOWR('f', 162, struct if_quotactl *)

#define OBD_IOC_MOUNTOPT               _IOWR('f', 170, OBD_IOC_DATA_TYPE)

#define OBD_IOC_RECORD                 _IOWR('f', 180, OBD_IOC_DATA_TYPE)
#define OBD_IOC_ENDRECORD              _IOWR('f', 181, OBD_IOC_DATA_TYPE)
#define OBD_IOC_PARSE                  _IOWR('f', 182, OBD_IOC_DATA_TYPE)
#define OBD_IOC_DORECORD               _IOWR('f', 183, OBD_IOC_DATA_TYPE)
#define OBD_IOC_PROCESS_CFG            _IOWR('f', 184, OBD_IOC_DATA_TYPE)
#define OBD_IOC_DUMP_LOG               _IOWR('f', 185, OBD_IOC_DATA_TYPE)
#define OBD_IOC_CLEAR_LOG              _IOWR('f', 186, OBD_IOC_DATA_TYPE)
#define OBD_IOC_PARAM                  _IOW ('f', 187, OBD_IOC_DATA_TYPE)
#define OBD_IOC_POOL                   _IOWR('f', 188, OBD_IOC_DATA_TYPE)

#define OBD_IOC_CATLOGLIST             _IOWR('f', 190, OBD_IOC_DATA_TYPE)
#define OBD_IOC_LLOG_INFO              _IOWR('f', 191, OBD_IOC_DATA_TYPE)
#define OBD_IOC_LLOG_PRINT             _IOWR('f', 192, OBD_IOC_DATA_TYPE)
#define OBD_IOC_LLOG_CANCEL            _IOWR('f', 193, OBD_IOC_DATA_TYPE)
#define OBD_IOC_LLOG_REMOVE            _IOWR('f', 194, OBD_IOC_DATA_TYPE)
#define OBD_IOC_LLOG_CHECK             _IOWR('f', 195, OBD_IOC_DATA_TYPE)
#define OBD_IOC_LLOG_CATINFO           _IOWR('f', 196, OBD_IOC_DATA_TYPE)

#define ECHO_IOC_GET_STRIPE            _IOWR('f', 200, OBD_IOC_DATA_TYPE)
#define ECHO_IOC_SET_STRIPE            _IOWR('f', 201, OBD_IOC_DATA_TYPE)
#define ECHO_IOC_ENQUEUE               _IOWR('f', 202, OBD_IOC_DATA_TYPE)
#define ECHO_IOC_CANCEL                _IOWR('f', 203, OBD_IOC_DATA_TYPE)

#define OBD_IOC_GET_OBJ_VERSION        _IOR('f', 210, OBD_IOC_DATA_TYPE)

/* XXX _IOWR('f', 250, long) has been defined in
 * lnet/include/libcfs/kp30.h for debug, don't use it
 */

/* Until such time as we get_info the per-stripe maximum from the OST,
 * we define this to be 2T - 4k, which is the ext3 maxbytes. */
#define LUSTRE_STRIPE_MAXBYTES 0x1fffffff000ULL

/* Special values for remove LOV EA from disk */
#define LOVEA_DELETE_VALUES(size, count, offset) (size == 0 && count == 0 && \
                                                 offset == (typeof(offset))(-1))

/* #define POISON_BULK 0 */

/*
 * l_wait_event is a flexible sleeping function, permitting simple caller
 * configuration of interrupt and timeout sensitivity along with actions to
 * be performed in the event of either exception.
 *
 * The first form of usage looks like this:
 *
 * struct l_wait_info lwi = LWI_TIMEOUT_INTR(timeout, timeout_handler,
 *                                           intr_handler, callback_data);
 * rc = l_wait_event(waitq, condition, &lwi);
 *
 * l_wait_event() makes the current process wait on 'waitq' until 'condition'
 * is TRUE or a "killable" signal (SIGTERM, SIKGILL, SIGINT) is pending.  It
 * returns 0 to signify 'condition' is TRUE, but if a signal wakes it before
 * 'condition' becomes true, it optionally calls the specified 'intr_handler'
 * if not NULL, and returns -EINTR.
 *
 * If a non-zero timeout is specified, signals are ignored until the timeout
 * has expired.  At this time, if 'timeout_handler' is not NULL it is called.
 * If it returns FALSE l_wait_event() continues to wait as described above with
 * signals enabled.  Otherwise it returns -ETIMEDOUT.
 *
 * LWI_INTR(intr_handler, callback_data) is shorthand for
 * LWI_TIMEOUT_INTR(0, NULL, intr_handler, callback_data)
 *
 * The second form of usage looks like this:
 *
 * struct l_wait_info lwi = LWI_TIMEOUT(timeout, timeout_handler);
 * rc = l_wait_event(waitq, condition, &lwi);
 *
 * This form is the same as the first except that it COMPLETELY IGNORES
 * SIGNALS.  The caller must therefore beware that if 'timeout' is zero, or if
 * 'timeout_handler' is not NULL and returns FALSE, then the ONLY thing that
 * can unblock the current process is 'condition' becoming TRUE.
 *
 * Another form of usage is:
 * struct l_wait_info lwi = LWI_TIMEOUT_INTERVAL(timeout, interval,
 *                                               timeout_handler);
 * rc = l_wait_event(waitq, condition, &lwi);
 * This is the same as previous case, but condition is checked once every
 * 'interval' jiffies (if non-zero).
 *
 * Subtle synchronization point: this macro does *not* necessary takes
 * wait-queue spin-lock before returning, and, hence, following idiom is safe
 * ONLY when caller provides some external locking:
 *
 *             Thread1                            Thread2
 *
 *   l_wait_event(&obj->wq, ....);                                       (1)
 *
 *                                    wake_up(&obj->wq):                 (2)
 *                                         spin_lock(&q->lock);          (2.1)
 *                                         __wake_up_common(q, ...);     (2.2)
 *                                         spin_unlock(&q->lock, flags); (2.3)
 *
 *   OBD_FREE_PTR(obj);                                                  (3)
 *
 * As l_wait_event() may "short-cut" execution and return without taking
 * wait-queue spin-lock, some additional synchronization is necessary to
 * guarantee that step (3) can begin only after (2.3) finishes.
 *
 * XXX nikita: some ptlrpc daemon threads have races of that sort.
 *
 */

static inline int back_to_sleep(void *arg)
{
        return 0;
}

#define LWI_ON_SIGNAL_NOOP ((void (*)(void *))(-1))

struct l_wait_info {
        cfs_duration_t lwi_timeout;
        cfs_duration_t lwi_interval;
        int  (*lwi_on_timeout)(void *);
        void (*lwi_on_signal)(void *);
        void  *lwi_cb_data;
};

/* NB: LWI_TIMEOUT ignores signals completely */
#define LWI_TIMEOUT(time, cb, data)             \
((struct l_wait_info) {                         \
        .lwi_timeout    = time,                 \
        .lwi_on_timeout = cb,                   \
        .lwi_cb_data    = data,                 \
        .lwi_interval   = 0                     \
})

#define LWI_TIMEOUT_INTERVAL(time, interval, cb, data)  \
((struct l_wait_info) {                                 \
        .lwi_timeout    = time,                         \
        .lwi_on_timeout = cb,                           \
        .lwi_cb_data    = data,                         \
        .lwi_interval   = interval                      \
})

#define LWI_TIMEOUT_INTR(time, time_cb, sig_cb, data)   \
((struct l_wait_info) {                                 \
        .lwi_timeout    = time,                         \
        .lwi_on_timeout = time_cb,                      \
        .lwi_on_signal = sig_cb,                        \
        .lwi_cb_data    = data,                         \
        .lwi_interval    = 0                            \
})

#define LWI_INTR(cb, data)  LWI_TIMEOUT_INTR(0, NULL, cb, data)

#ifdef __KERNEL__

/*
 * wait for @condition to become true, but no longer than timeout, specified
 * by @info.
 */
#define __l_wait_event(wq, condition, info, ret, excl)                         \
do {                                                                           \
        cfs_waitlink_t __wait;                                                 \
        cfs_duration_t __timeout = info->lwi_timeout;                          \
        cfs_sigset_t   __blocked;                                              \
                                                                               \
        ret = 0;                                                               \
        if (condition)                                                         \
                break;                                                         \
                                                                               \
        cfs_waitlink_init(&__wait);                                            \
        if (excl)                                                              \
                cfs_waitq_add_exclusive(&wq, &__wait);                         \
        else                                                                   \
                cfs_waitq_add(&wq, &__wait);                                   \
                                                                               \
        /* Block all signals (just the non-fatal ones if no timeout). */       \
        if (info->lwi_on_signal != NULL && __timeout == 0)                     \
                __blocked = l_w_e_set_sigs(LUSTRE_FATAL_SIGS);                 \
        else                                                                   \
                __blocked = l_w_e_set_sigs(0);                                 \
                                                                               \
        for (;;) {                                                             \
                set_current_state(TASK_INTERRUPTIBLE);                         \
                                                                               \
                if (condition)                                                 \
                        break;                                                 \
                                                                               \
                if (__timeout == 0) {                                          \
                        cfs_waitq_wait(&__wait, CFS_TASK_INTERRUPTIBLE);       \
                } else {                                                       \
                        cfs_duration_t interval = info->lwi_interval?          \
                                             min_t(cfs_duration_t,             \
                                                 info->lwi_interval,__timeout):\
                                             __timeout;                        \
                        cfs_duration_t remaining = cfs_waitq_timedwait(&__wait,\
                                                   CFS_TASK_INTERRUPTIBLE,     \
                                                   interval);                  \
                        __timeout = cfs_time_sub(__timeout,                    \
                                            cfs_time_sub(interval, remaining));\
                        if (__timeout == 0) {                                  \
                                if (info->lwi_on_timeout == NULL ||            \
                                    info->lwi_on_timeout(info->lwi_cb_data)) { \
                                        ret = -ETIMEDOUT;                      \
                                        break;                                 \
                                }                                              \
                                /* Take signals after the timeout expires. */  \
                                if (info->lwi_on_signal != NULL)               \
                                    (void)l_w_e_set_sigs(LUSTRE_FATAL_SIGS);   \
                        }                                                      \
                }                                                              \
                                                                               \
                if (condition)                                                 \
                        break;                                                 \
                if (cfs_signal_pending()) {                                    \
                        if (info->lwi_on_signal != NULL && __timeout == 0) {   \
                                if (info->lwi_on_signal != LWI_ON_SIGNAL_NOOP) \
                                        info->lwi_on_signal(info->lwi_cb_data);\
                                ret = -EINTR;                                  \
                                break;                                         \
                        }                                                      \
                        /* We have to do this here because some signals */     \
                        /* are not blockable - ie from strace(1).       */     \
                        /* In these cases we want to schedule_timeout() */     \
                        /* again, because we don't want that to return  */     \
                        /* -EINTR when the RPC actually succeeded.      */     \
                        /* the RECALC_SIGPENDING below will deliver the */     \
                        /* signal properly.                             */     \
                        cfs_clear_sigpending();                                \
                }                                                              \
        }                                                                      \
                                                                               \
        cfs_block_sigs(__blocked);                                             \
                                                                               \
        set_current_state(TASK_RUNNING);                                       \
        cfs_waitq_del(&wq, &__wait);                                           \
} while (0)

#else /* !__KERNEL__ */
#define __l_wait_event(wq, condition, info, ret, excl)                         \
do {                                                                    \
        long __timeout = info->lwi_timeout;                             \
        long __now;                                                     \
        long __then = 0;                                                \
        int  __timed_out = 0;                                           \
        int  __interval = obd_timeout;                                  \
                                                                        \
        ret = 0;                                                        \
        if (condition)                                                  \
                break;                                                  \
                                                                        \
        if (__timeout != 0)                                             \
                __then = time(NULL);                                    \
                                                                        \
        if (__timeout && __timeout < __interval)                        \
                __interval = __timeout;                                 \
        if (info->lwi_interval && info->lwi_interval < __interval)      \
                __interval = info->lwi_interval;                        \
                                                                        \
        while (!(condition)) {                                          \
                liblustre_wait_event(__interval);                       \
                if (condition)                                          \
                        break;                                          \
                                                                        \
                if (!__timed_out && info->lwi_timeout != 0) {           \
                        __now = time(NULL);                             \
                        __timeout -= __now - __then;                    \
                        __then = __now;                                 \
                                                                        \
                        if (__timeout > 0)                              \
                                continue;                               \
                                                                        \
                        __timeout = 0;                                  \
                        __timed_out = 1;                                \
                        if (info->lwi_on_timeout == NULL ||             \
                            info->lwi_on_timeout(info->lwi_cb_data)) {  \
                                ret = -ETIMEDOUT;                       \
                                break;                                  \
                        }                                               \
                }                                                       \
        }                                                               \
} while (0)

#endif /* __KERNEL__ */

#define l_wait_event(wq, condition, info)                       \
({                                                              \
        int                 __ret;                              \
        struct l_wait_info *__info = (info);                    \
                                                                \
        __l_wait_event(wq, condition, __info, __ret, 0);        \
        __ret;                                                  \
})

#define l_wait_event_exclusive(wq, condition, info)             \
({                                                              \
        int                 __ret;                              \
        struct l_wait_info *__info = (info);                    \
                                                                \
        __l_wait_event(wq, condition, __info, __ret, 1);        \
        __ret;                                                  \
})

#ifdef __KERNEL__
#define LIBLUSTRE_CLIENT (0)
#else
#define LIBLUSTRE_CLIENT (1)
#endif

#endif /* _LUSTRE_LIB_H */
