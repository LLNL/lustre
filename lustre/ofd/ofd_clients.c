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
 * lustre/ofd/ofd_clients.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Alex Tomas <alex@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FILTER

#include <libcfs/libcfs.h>
#include "ofd_internal.h"

/* Add client data to the FILTER.  We use a bitmap to locate a free space
 * in the last_rcvd file if cl_idx is -1 (i.e. a new client).
 * Otherwise, we have just read the data from the last_rcvd file and
 * we know its offset. */
int filter_client_new(const struct lu_env *env, struct filter_device *ofd,
                      struct filter_export_data *fed)
{
        struct obd_device *obd = filter_obd(ofd);
        unsigned long *bitmap = ofd->ofd_lut.lut_client_bitmap;
        struct tg_export_data *ted = &fed->fed_ted;
        struct lsd_client_data *lcd = ted->ted_lcd;
        loff_t off;
        int err, cl_idx = 0;
        struct thandle *th;
        ENTRY;

        LASSERT(bitmap != NULL);

        /* XXX if lcd_uuid were a real obd_uuid, I could use obd_uuid_equals */
        if (!strcmp((char *)lcd->lcd_uuid, (char *)obd->obd_uuid.uuid))
                RETURN(0);

        /* the bitmap operations can handle cl_idx > sizeof(long) * 8, so
         * there's no need for extra complication here
         */
        cl_idx = cfs_find_first_zero_bit(bitmap, LR_MAX_CLIENTS);
repeat:
        if (cl_idx >= LR_MAX_CLIENTS) {
                CERROR("no client slots - fix LR_MAX_CLIENTS\n");
                RETURN(-EOVERFLOW);
        }
        if (cfs_test_and_set_bit(cl_idx, bitmap)) {
                cl_idx = cfs_find_next_zero_bit(bitmap, LR_MAX_CLIENTS, cl_idx);
                goto repeat;
        }

        ted->ted_lr_idx = cl_idx;
        ted->ted_lr_off = ofd->ofd_fsd.lsd_client_start +
                          cl_idx * ofd->ofd_fsd.lsd_client_size;
        LASSERTF(ted->ted_lr_off > 0, "fed_lr_off = %llu\n", ted->ted_lr_off);

        CDEBUG(D_INFO, "client at index %d (%llu) with UUID '%s' added\n",
               ted->ted_lr_idx, ted->ted_lr_off, lcd->lcd_uuid);

        CDEBUG(D_INFO, "writing client lcd at idx %u (%llu) (len %u)\n",
               ted->ted_lr_idx, ted->ted_lr_off,
               (unsigned int)sizeof(*ted->ted_lcd));

        if (OBD_FAIL_CHECK(OBD_FAIL_TGT_CLIENT_ADD))
                RETURN(-ENOSPC);

        th = filter_trans_create(env, ofd);
        if (IS_ERR(th))
                RETURN(PTR_ERR(th));
        /* off is changed, use tmp value */
        off = ted->ted_lr_off;
        dt_declare_record_write(env, ofd->ofd_last_rcvd, sizeof(*lcd), off, th);
        err = filter_trans_start(env, ofd, th);
        if (err)
                RETURN(err);
        /* XXX: until this operations will be committed the sync is needed for this
         * export */
        /*
        mdt_trans_add_cb(th, mdt_cb_new_client, mti->mti_exp);
        cfs_spin_lock(&mti->mti_exp->exp_lock);
        mti->mti_exp->exp_need_sync = 1;
        cfs_spin_unlock(&mti->mti_exp->exp_lock);
        */

        err = filter_last_rcvd_write(env, ofd, lcd, &off, th);

        CDEBUG(D_INFO, "wrote client lcd at idx %u off %llu (len %u)\n",
               cl_idx, ted->ted_lr_off, (unsigned) sizeof(*lcd));

        filter_trans_stop(env, ofd, th);

        RETURN(err);
}

int filter_client_add(const struct lu_env *env, struct filter_device *ofd,
                      struct filter_export_data *fed, int cl_idx)
{
        struct obd_device *obd = filter_obd(ofd);
        unsigned long *bitmap = ofd->ofd_lut.lut_client_bitmap;
        struct tg_export_data *ted = &fed->fed_ted;
        ENTRY;

        LASSERT(bitmap != NULL);
        LASSERT(cl_idx >= 0);

        /* XXX if lcd_uuid were a real obd_uuid, I could use obd_uuid_equals */
        if (!strcmp((char *)ted->ted_lcd->lcd_uuid, (char *)obd->obd_uuid.uuid))
                RETURN(0);

        /* the bitmap operations can handle cl_idx > sizeof(long) * 8, so
         * there's no need for extra complication here
         */
        if (cfs_test_and_set_bit(cl_idx, bitmap)) {
                CERROR("FILTER client %d: bit already set in bitmap!\n",
                       cl_idx);
                LBUG();
        }

        ted->ted_lr_idx = cl_idx;
        ted->ted_lr_off = ofd->ofd_fsd.lsd_client_start +
                          cl_idx * ofd->ofd_fsd.lsd_client_size;
        LASSERTF(ted->ted_lr_off > 0, "fed_lr_off = %llu\n", ted->ted_lr_off);

        CDEBUG(D_INFO, "client at index %d (%llu) with UUID '%s' added\n",
               ted->ted_lr_idx, ted->ted_lr_off, ted->ted_lcd->lcd_uuid);

        RETURN(0);
}

int filter_client_free(struct lu_env *env, struct obd_export *exp)
{
        struct filter_export_data *fed = &exp->exp_filter_data;
        struct filter_thread_info *info;
        struct obd_device *obd = exp->exp_obd;
        struct filter_device *ofd = filter_exp(exp);
        struct tg_export_data *ted = &fed->fed_ted;
        struct lsd_client_data *lcd = ted->ted_lcd;
        struct thandle *th;
        loff_t off;
        int rc;
        ENTRY;

        if (lcd == NULL)
                RETURN(0);

        /* XXX if lcd_uuid were a real obd_uuid, I could use obd_uuid_equals */
        if (!strcmp((char *)lcd->lcd_uuid, (char *)obd->obd_uuid.uuid))
                GOTO(free, 0);

        info = filter_info_init(env, exp);
        info->fti_no_need_trans = 1;

        CDEBUG(D_INFO, "freeing client at idx %u, offset %lld with UUID '%s'\n",
               ted->ted_lr_idx, ted->ted_lr_off, lcd->lcd_uuid);

        LASSERT(ofd->ofd_lut.lut_client_bitmap != NULL);

        /* Clear the bit _after_ zeroing out the client so we don't
           race with filter_client_add and zero out new clients.*/
        if (!cfs_test_bit(ted->ted_lr_idx, ofd->ofd_lut.lut_client_bitmap)) {
                CERROR("FILTER client %u: bit already clear in bitmap!!\n",
                       ted->ted_lr_idx);
                LBUG();
        }

        if (!(exp->exp_flags & OBD_OPT_FAILOVER)) {
                th = filter_trans_create(env, ofd);
                if (IS_ERR(th))
                        GOTO(free, rc = PTR_ERR(th));
                /* declare last_rcvd write */
                dt_declare_record_write(env, ofd->ofd_last_rcvd, sizeof(*lcd),
                                        ted->ted_lr_off, th);
                /* declare header write */
                dt_declare_record_write(env, ofd->ofd_last_rcvd,
                                        sizeof(ofd->ofd_fsd), 0, th);

                rc = filter_trans_start(env, ofd, th);
                if (rc)
                        GOTO(free, rc);
                memset(lcd->lcd_uuid, 0, sizeof(lcd->lcd_uuid));
                /* off is changed after write, use tmp value */
                off = ted->ted_lr_off;
                rc = filter_last_rcvd_write(env, ofd, lcd, &off, th);
                LASSERT(rc == 0);

                /* update server's transno */
                filter_last_rcvd_header_write(env, ofd, th);

                filter_trans_stop(env, ofd, th);

                CDEBUG(rc == 0 ? D_INFO : D_ERROR,
                       "zeroing out client %s at idx %u (%llu) in %s rc %d\n",
                       lcd->lcd_uuid, ted->ted_lr_idx, ted->ted_lr_off,
                       LAST_RCVD, rc);
        }
        EXIT;
free:
        return 0;
}

