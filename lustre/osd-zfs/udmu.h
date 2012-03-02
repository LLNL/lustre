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
 * Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/dmu-osd/udmu.h
 *
 * Author: Alex Tomas <alex@clusterfs.com>
 * Author: Atul Vidwansa <atul.vidwansa@sun.com>
 * Author: Manoj Joseph <manoj.joseph@sun.com>
 */

#ifndef _DMU_H
#define _DMU_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <sys/zap.h>
#include <sys/vnode.h>
#include <sys/mode.h>

#include <lustre/lustre_user.h>

#define DMU_RESERVED_MAX (64ULL << 20)

#ifndef DMU_AT_TYPE
#define DMU_AT_TYPE    0x0001
#define DMU_AT_MODE    0x0002
#define DMU_AT_UID     0x0004
#define DMU_AT_GID     0x0008
#define DMU_AT_FSID    0x0010
#define DMU_AT_NODEID  0x0020
#define DMU_AT_NLINK   0x0040
#define DMU_AT_SIZE    0x0080
#define DMU_AT_ATIME   0x0100
#define DMU_AT_MTIME   0x0200
#define DMU_AT_CTIME   0x0400
#define DMU_AT_RDEV    0x0800
#define DMU_AT_BLKSIZE 0x1000
#define DMU_AT_NBLOCKS 0x2000
#define DMU_AT_SEQ     0x8000
#endif

#define LOOKUP_DIR              0x01    /* want parent dir vp */
#define LOOKUP_XATTR            0x02    /* lookup up extended attr dir */
#define CREATE_XATTR_DIR        0x04    /* Create extended attr dir */

#define S_IFDOOR        0xD000  /* door */
#define S_IFPORT        0xE000  /* event port */

typedef struct udmu_objset {
        struct objset *os;
        uint64_t root;  /* id of root znode */
        cfs_spinlock_t  lock;
        uint64_t objects; /* in-core counter of objects, protected by lock */
} udmu_objset_t;

#ifndef _SYS_TXG_H
#define TXG_WAIT        1ULL
#define TXG_NOWAIT      2ULL
#endif

#define ZFS_DIRENT_MAKE(type, obj) (((uint64_t)type << 60) | obj)

void udmu_init(void);
void udmu_fini(void);

/* udmu object-set API */
int udmu_objset_open(char *osname, udmu_objset_t *uos);
void udmu_objset_close(udmu_objset_t *uos);
int udmu_objset_statfs(udmu_objset_t *uos, struct obd_statfs *osfs);
int udmu_objset_root(udmu_objset_t *uos, dmu_buf_t **dbp, void *tag);
void udmu_wait_synced(udmu_objset_t *uos, dmu_tx_t *tx);
void udmu_wait_txg_synced(udmu_objset_t *uos, uint64_t txg);
uint64_t udmu_get_txg(udmu_objset_t *uos, dmu_tx_t *tx);

/* buf must have at least MAXNAMELEN bytes */
void udmu_objset_name_get(udmu_objset_t *uos, char *buf);

/* get/set ZFS user properties */
int udmu_userprop_set_str(udmu_objset_t *uos, const char *prop_name,
                      const char *val);
int udmu_userprop_get_str(udmu_objset_t *uos, const char *prop_name, char *buf,
                      size_t buf_size);

/* udmu ZAP API */
int udmu_zap_lookup(udmu_objset_t *uos, dmu_buf_t *zap_db, const char *name,
                    void *value, int value_size, int intsize);

void udmu_zap_create(udmu_objset_t *uos, dmu_buf_t **zap_dbp, dmu_tx_t *tx,
                     void *tag);

int udmu_zap_insert(udmu_objset_t *uos, dmu_buf_t *zap_db, dmu_tx_t *tx,
                    const char *name, void *value, int len);

int udmu_zap_delete(udmu_objset_t *uos, dmu_buf_t *zap_db, dmu_tx_t *tx,
                    const char *name);

/* zap cursor apis */
int udmu_zap_cursor_init(zap_cursor_t **zc, udmu_objset_t *uos,
                         uint64_t zapobj, uint64_t hash);

void udmu_zap_cursor_fini(zap_cursor_t *zc);

int udmu_zap_cursor_retrieve_key(zap_cursor_t *zc, char *key, int max);

int udmu_zap_cursor_retrieve_value(zap_cursor_t *zc,  char *buf,
                int buf_size, int *bytes_read);

void udmu_zap_cursor_advance(zap_cursor_t *zc);

uint64_t udmu_zap_cursor_serialize(zap_cursor_t *zc);

int udmu_zap_cursor_move_to_key(zap_cursor_t *zc, const char *name);


/* udmu object API */
void udmu_object_create(udmu_objset_t *uos, dmu_buf_t **dbp, dmu_tx_t *tx,
                        void *tag);

int udmu_object_get_dmu_buf(udmu_objset_t *uos, uint64_t object,
                            dmu_buf_t **dbp, void *tag);

void udmu_object_put_dmu_buf(dmu_buf_t *db, void *tag);

uint64_t udmu_object_get_id(dmu_buf_t *db);

int udmu_object_read(udmu_objset_t *uos, dmu_buf_t *db, uint64_t offset,
                     uint64_t size, void *buf);

void udmu_object_write(udmu_objset_t *uos, dmu_buf_t *db, struct dmu_tx *tx,
                      uint64_t offset, uint64_t size, void *buf);

void udmu_object_getattr(dmu_buf_t *db, vattr_t *vap);

void udmu_object_setattr(dmu_buf_t *db, dmu_tx_t *tx, vattr_t *vap);

int udmu_object_punch(udmu_objset_t *uos, dmu_buf_t *db, dmu_tx_t *tx,
                      uint64_t offset, uint64_t len);

void udmu_declare_object_delete(udmu_objset_t *uos, dmu_tx_t *tx,
                                dmu_buf_t *db);
int udmu_object_delete(udmu_objset_t *uos, dmu_buf_t **db, dmu_tx_t *tx,
                       void *tag);
int udmu_object_set_blocksize(udmu_objset_t *os, uint64_t oid,
                              unsigned bsize, dmu_tx_t *tx);

/*udmu transaction API */

dmu_tx_t *udmu_tx_create(udmu_objset_t *uos);

void udmu_tx_hold_write(dmu_tx_t *tx, uint64_t object, uint64_t off, int len);

void udmu_tx_hold_free(dmu_tx_t *tx, uint64_t object, uint64_t off,
    uint64_t len);

void udmu_tx_hold_zap(dmu_tx_t *tx, uint64_t object, int add, char *name);

void udmu_tx_hold_bonus(dmu_tx_t *tx, uint64_t object);

void udmu_tx_abort(dmu_tx_t *tx);

int udmu_tx_assign(dmu_tx_t *tx, uint64_t txg_how);

void udmu_tx_wait(dmu_tx_t *tx);

void udmu_tx_commit(dmu_tx_t *tx);

/* Commit callbacks */
typedef void udmu_tx_callback_func_t(void *dcb_data, int error);
void udmu_tx_cb_register(dmu_tx_t *tx, udmu_tx_callback_func_t *func,
                         void *data);
void udmu_wait_callbacks(udmu_objset_t *uos);

int udmu_object_is_zap(dmu_buf_t *);

uint64_t udmu_object_get_links(dmu_buf_t *db);
void udmu_object_links_inc(dmu_buf_t *db, dmu_tx_t *tx);
void udmu_object_links_dec(dmu_buf_t *db, dmu_tx_t *tx);

/* Extended attributes */
int udmu_xattr_get(udmu_objset_t *uos, dmu_buf_t *db, void *buf, int buflen,
                   const char *name, int *size);
int udmu_xattr_list(udmu_objset_t *uos, dmu_buf_t *db, void *val, int vallen);

void udmu_xattr_declare_set(udmu_objset_t *uos, dmu_buf_t *db, int vallen,
                            const char *name, dmu_tx_t *tx);
int udmu_xattr_set(udmu_objset_t *uos, dmu_buf_t *db, void *val, int vallen,
                   const char *name, dmu_tx_t *tx);

void udmu_xattr_declare_del(udmu_objset_t *uos, dmu_buf_t *db,
                            const char *name, dmu_tx_t *tx);
int udmu_xattr_del(udmu_objset_t *uos, dmu_buf_t *db,
                   const char *name, dmu_tx_t *tx);

void udmu_freeze(udmu_objset_t *uos);
#ifdef  __cplusplus
}
#endif

#endif /* _DMU_H */
