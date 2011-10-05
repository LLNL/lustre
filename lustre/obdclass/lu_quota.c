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
 * version 2 along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2011 Whamcloud, Inc.
 * Use is subject to license terms.
 *
 * Author: Johann Lombardi <johann@whamcloud.com>
 */

#include <lu_quota.h>
#include <obd.h>

/* index features supported by the accounting objects */
const struct dt_index_features dt_acct_features = {
        .dif_flags       = DT_IND_UPDATE,
        .dif_keysize_min = sizeof(__u64), /* 64-bit uid/gid */
        .dif_keysize_max = sizeof(__u64), /* 64-bit uid/gid */
        .dif_recsize_min = sizeof(struct acct_rec), /* 32 bytes */
        .dif_recsize_max = sizeof(struct acct_rec), /* 32 bytes */
        .dif_ptrsize     = 4
};
EXPORT_SYMBOL(dt_acct_features);

/**
 * Initialize accounting objects to collect space usage information for user
 * or group.
 *
 * \param env - is the environment passed by the caller
 * \param dev - is the dt_device storing the accounting object
 * \param oid - is the object id of the accounting object to initialize, must be
 *              either ACCT_USER_OID or ACCT_GROUP_OID.
 */
static struct dt_object *acct_init(const struct lu_env *env,
                                   struct dt_device *dev, __u32 oid)
{
        struct dt_object *obj = NULL;
        struct lu_fid     fid;
        int               rc;
        ENTRY;

        /* look up the accounting object */
        lu_local_obj_fid(&fid, oid);
        obj = dt_locate(env, dev, &fid);
        if (IS_ERR(obj))
                RETURN(NULL);

        /* if the object doesn't exist on-disk, then accounting can't be used */
        if (dt_object_exists(obj) <= 0) {
                lu_object_put(env, &obj->do_lu);
                RETURN(NULL);
        }

        /* set up indexing operations */
        rc = obj->do_ops->do_index_try(env, obj, &dt_acct_features);
        if (rc) {
                CERROR("%s: failed to set up indexing operations for %s acct "
                       "object %d\n", dev->dd_lu_dev.ld_obd->obd_name,
                       oid == ACCT_USER_OID ? "user" : "group", rc);
                lu_object_put(env, &obj->do_lu);
                RETURN(NULL);
        }
        RETURN(obj);
}
EXPORT_SYMBOL(acct_init);

/*
 * Initialize lu_quota structure to be used to collect usage/quota
 * information from the OSD layer
 *
 * \param env - is the environment passed by the caller
 * \param dev - is the dt_device storing the accounting/quota objects
 * \param lu_quota - is the lu_quota structure to initialize
 */
void lu_quota_init(const struct lu_env *env, struct dt_device *dev,
                   struct lu_quota *lu_quota)
{
         /* initialize accounting objects */
        lu_quota->acct_user_obj  = acct_init(env, dev, ACCT_USER_OID);
        lu_quota->acct_group_obj = acct_init(env, dev, ACCT_GROUP_OID);

        if (lu_quota->acct_user_obj == NULL || lu_quota->acct_group_obj == NULL)
               LCONSOLE_INFO("%s: no %s space accounting support\n",
                             dev->dd_lu_dev.ld_obd->obd_name,
                             lu_quota->acct_user_obj ? "group" :
                             (lu_quota->acct_group_obj ? "user":"user & group"));
}
EXPORT_SYMBOL(lu_quota_init);

/*
 * Cleanup lu_quota data before shutdown.
 *
 * \param env - is the environment passed by the caller
 * \param dev - is the dt_device storing the quota objects
 * \param lu_quota - is the lu_quota structure to be cleaned up
 */
void lu_quota_fini(const struct lu_env *env, struct dt_device *dev,
                   struct lu_quota *lu_quota)
{
        if (lu_quota->acct_user_obj) {
                lu_object_put(env, &lu_quota->acct_user_obj->do_lu);
                lu_quota->acct_user_obj = NULL;
        }
        if (lu_quota->acct_group_obj) {
                lu_object_put(env, &lu_quota->acct_group_obj->do_lu);
                lu_quota->acct_group_obj = NULL;
        }
}
EXPORT_SYMBOL(lu_quota_fini);

/*
 * Handle quotactl request. This function converts a quotactl request into
 * quota/accounting object operations.
 *
 * \param env - is the environment passed by the caller
 * \param lu_quota - is the lu_quota structure storing references to the
 *                   accounting/quota objects to be used to handle the quotactl
 *                   operation.
 * \param oqctl - is the quotactl request
 */
int lu_quotactl(const struct lu_env *env, struct lu_quota *lu_quota,
                struct obd_quotactl *oqctl)
{
        int rc = 0;
        ENTRY;

        switch (oqctl->qc_cmd) {
        case Q_QUOTACHECK:
                /* deprecated since quotacheck is not needed any more */
        case LUSTRE_Q_INVALIDATE:
        case LUSTRE_Q_FINVALIDATE:
                /* deprecated, not used any more */
                RETURN(-EOPNOTSUPP);
                break;

        case Q_QUOTAON:
                if (!lu_quota->acct_user_obj || !lu_quota->acct_group_obj)
                        /* space tracking is not enabled, so enforcement cannot
                         * be turned on */
                        RETURN(-EINVAL);
        case Q_QUOTAOFF:
                /* TODO should just enable/disable enforcement */
        case Q_GETINFO:
        case Q_GETOINFO:
                /* TODO should fill obd_quotactl::obd_dqinfo with grace time
                 * info */
        case Q_SETINFO:
                /* TODO change grace time */
        case Q_SETQUOTA:
                /* TODO handle new quota limit set by admin */
        case Q_INITQUOTA:
                /* TODO init slave limit */
                CERROR("quotactl operation %d not implemented yet\n",
                       oqctl->qc_cmd);
                RETURN(-ENOSYS);
                break;

        case Q_GETQUOTA:
                /* TODO return global quota limit
                 * XXX always return no limit for now, for testing purpose */
                memset(&oqctl->qc_dqblk, 0, sizeof(struct obd_dqblk));
                oqctl->qc_dqblk.dqb_valid = QIF_LIMITS;
                break;

        case Q_GETOQUOTA: {
                struct acct_rec  rec;
                __u64            key;
                struct dt_object *obj;

                if (oqctl->qc_type == USRQUOTA)
                        obj = lu_quota->acct_user_obj;
                else if (oqctl->qc_type == GRPQUOTA)
                        obj = lu_quota->acct_group_obj;
                else
                        RETURN(-EINVAL);

                if (!obj || !obj->do_index_ops)
                        RETURN(-EINVAL);

                /* qc_id is a 32-bit field while a key has 64 bits */
                key = oqctl->qc_id;
                rc = dt_lookup(env, obj, (struct dt_rec*)&rec,
                               (struct dt_key*)&key, BYPASS_CAPA);
                if (rc < 0)
                        RETURN(rc);

                memset(&oqctl->qc_dqblk, 0, sizeof(struct obd_dqblk));
                oqctl->qc_dqblk.dqb_curspace  = rec.bspace;
                oqctl->qc_dqblk.dqb_curinodes = rec.ispace;
                oqctl->qc_dqblk.dqb_valid = QIF_USAGE;
                break;
        }
        default:
                CERROR("Unsupported quotactl command: %d\n", oqctl->qc_cmd);
                RETURN(-EFAULT);
        }
        RETURN(rc);
}
EXPORT_SYMBOL(lu_quotactl);
