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
 * Copyright (c) 2011, 2012, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mdd/mdd_lov.c
 *
 * Lustre Metadata Server (mds) handling of striped file data
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: wangdi <wangdi@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <obd_class.h>
#include <obd_support.h>
#include <lustre_fid.h>

#include "mdd_internal.h"

int mdd_get_md(const struct lu_env *env, struct mdd_object *obj,
               void *md, int *md_size, const char *name)
{
        int rc;
        ENTRY;

        if (*md_size == 0)
                md = NULL;
        rc = mdo_xattr_get(env, obj, mdd_buf_get(env, md, *md_size), name,
                           mdd_object_capa(env, obj));
        /*
         * XXX: Handling of -ENODATA, the right way is to have ->do_md_get()
         * exported by dt layer.
         */
        if (rc == 0 || rc == -ENODATA) {
                *md_size = 0;
                rc = 0;
        } else if (rc < 0) {
                int mdsize = mdo_xattr_get(env, obj, mdd_buf_get(env, NULL, 0), name,
                                           mdd_object_capa(env, obj));
                CERROR("Error %d reading eadata - %d (%p)\n", mdsize, *md_size, md);
                dump_stack();
        } else {
                /* XXX: Convert lov EA but fixed after verification test. */
                *md_size = rc;
        }

        RETURN(rc);
}

int mdd_get_md_locked(const struct lu_env *env, struct mdd_object *obj,
                      void *md, int *md_size, const char *name)
{
        int rc = 0;
        mdd_read_lock(env, obj, MOR_TGT_CHILD);
        rc = mdd_get_md(env, obj, md, md_size, name);
        mdd_read_unlock(env, obj);
        return rc;
}

int mdd_lsm_sanity_check(const struct lu_env *env,  struct mdd_object *obj)
{
        struct lu_attr   *tmp_la = &mdd_env_info(env)->mti_la;
        struct md_ucred  *uc     = md_ucred(env);
        int rc;
        ENTRY;

        rc = mdd_la_get(env, obj, tmp_la, BYPASS_CAPA);
        if (rc)
                RETURN(rc);

        if ((uc->mu_fsuid != tmp_la->la_uid) &&
            !mdd_capable(uc, CFS_CAP_FOWNER))
                rc = mdd_permission_internal(env, obj, tmp_la, MAY_WRITE);

        RETURN(rc);
}
