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
 */

#ifndef _LUSTRE_LU_TARGET_H
#define _LUSTRE_LU_TARGET_H

#include <dt_object.h>
#include <lustre_disk.h>

struct lu_target {
        struct obd_device       *lut_obd;
        struct dt_device        *lut_bottom;
        /** last_rcvd file */
        struct dt_object        *lut_last_rcvd;
        /* transaction callbacks */
        struct dt_txn_callback   lut_txn_cb;
        /** server data in last_rcvd file */
        struct lr_server_data    lut_lsd;
        /** Server last transaction number */
        __u64                    lut_last_transno;
        /** Lock protecting last transaction number */
        cfs_spinlock_t           lut_translock;
        /** Lock protecting client bitmap */
        cfs_spinlock_t           lut_client_bitmap_lock;
        /** Bitmap of known clients */
        unsigned long           *lut_client_bitmap;
};

typedef void (*lut_cb_t)(struct lu_target *lut, __u64 transno,
                         void *data, int err);
struct lut_commit_cb {
        lut_cb_t  lut_cb_func;
        void     *lut_cb_data;
};

void lut_boot_epoch_update(struct lu_target *);
int lut_last_commit_cb_add(struct thandle *th, struct lu_target *lut,
                           struct obd_export *exp, __u64 transno);
int lut_new_client_cb_add(struct thandle *th, struct obd_export *exp);
int lut_init(const struct lu_env *, struct lu_target *,
             struct obd_device *, struct dt_device *);
void lut_fini(const struct lu_env *, struct lu_target *);
int lut_client_alloc(struct obd_export *);
void lut_client_free(struct obd_export *);
#endif /* __LUSTRE_LU_TARGET_H */
