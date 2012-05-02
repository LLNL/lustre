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
 * lustre/mdt/mdt_xattr.c
 *
 * Lustre Metadata Target (mdt) extended attributes management.
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Huang Hua <huanghua@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <lustre_acl.h>
#include "mdt_internal.h"


/* return EADATA length to the caller. negative value means error */
static int mdt_getxattr_pack_reply(struct mdt_thread_info * info)
{
        struct req_capsule     *pill = info->mti_pill ;
        struct ptlrpc_request  *req = mdt_info_req(info);
        char                   *xattr_name;
        __u64                   valid = info->mti_body->valid;
        static const char       user_string[] = "user.";
        int                     size, rc;
        ENTRY;

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_GETXATTR_PACK))
                RETURN(-ENOMEM);

        /* Determine how many bytes we need */
        if (valid & OBD_MD_FLXATTR) {
                xattr_name = req_capsule_client_get(pill, &RMF_NAME);
                if (!xattr_name)
                        RETURN(-EFAULT);

                if (!(req->rq_export->exp_connect_flags & OBD_CONNECT_XATTR) &&
                    !strncmp(xattr_name, user_string, sizeof(user_string) - 1))
                        RETURN(-EOPNOTSUPP);

                size = mo_xattr_get(info->mti_env,
                                    mdt_object_child(info->mti_object),
                                    &LU_BUF_NULL, xattr_name);
        } else if (valid & OBD_MD_FLXATTRLS) {
                size = mo_xattr_list(info->mti_env,
                                     mdt_object_child(info->mti_object),
                                     &LU_BUF_NULL);
        } else {
                CDEBUG(D_INFO, "Valid bits: "LPX64"\n", info->mti_body->valid);
                RETURN(-EINVAL);
        }

        if (size == -ENODATA) {
                size = 0;
        } else if (size < 0) {
                CERROR("Error geting EA size: %d\n", size);
                RETURN(size);
        }

        if (info->mti_body->eadatasize != 0 &&
            info->mti_body->eadatasize < size)
                RETURN(-ERANGE);

        req_capsule_set_size(pill, &RMF_EADATA, RCL_SERVER,
                             info->mti_body->eadatasize == 0 ? 0 : size);
        rc = req_capsule_server_pack(pill);
        if (rc) {
                LASSERT(rc < 0);
                RETURN(rc);
        }

        RETURN(size);
}

int mdt_getxattr(struct mdt_thread_info *info)
{
        struct ptlrpc_request  *req = mdt_info_req(info);
        struct mdt_export_data *med = mdt_req2med(req);
        struct md_ucred        *uc  = mdt_ucred(info);
        struct mdt_body        *reqbody;
        struct mdt_body        *repbody = NULL;
        struct md_object       *next;
        struct lu_buf          *buf;
        __u32                   remote = exp_connect_rmtclient(info->mti_exp);
        __u32                   perm;
        int                     easize, rc;
        ENTRY;

        LASSERT(info->mti_object != NULL);
        LASSERT(lu_object_assert_exists(&info->mti_object->mot_obj.mo_lu));

        CDEBUG(D_INODE, "getxattr "DFID"\n", PFID(&info->mti_body->fid1));

        reqbody = req_capsule_client_get(info->mti_pill, &RMF_MDT_BODY);
        if (reqbody == NULL)
                RETURN(err_serious(-EFAULT));

        rc = mdt_init_ucred(info, reqbody);
        if (rc)
                RETURN(err_serious(rc));

        next = mdt_object_child(info->mti_object);

        if (info->mti_body->valid & OBD_MD_FLRMTRGETFACL) {
                if (unlikely(!remote))
                        GOTO(out, rc = err_serious(-EINVAL));

                perm = mdt_identity_get_perm(uc->mu_identity, remote,
                                             req->rq_peer.nid);
                if (!(perm & CFS_RMTACL_PERM))
                        GOTO(out, rc = err_serious(-EPERM));

                rc = mo_permission(info->mti_env, NULL, next, NULL,
                                   MAY_RGETFACL);
                if (rc)
                        GOTO(out, rc = err_serious(rc));
        }

        easize = mdt_getxattr_pack_reply(info);
        if (easize < 0)
                GOTO(out, rc = err_serious(easize));

        repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);
        LASSERT(repbody != NULL);

        /* No need further getxattr. */
        if (easize == 0 || reqbody->eadatasize == 0)
                GOTO(out, rc = easize);


        buf = &info->mti_buf;
        buf->lb_buf = req_capsule_server_get(info->mti_pill, &RMF_EADATA);
        buf->lb_len = easize;

        if (info->mti_body->valid & OBD_MD_FLXATTR) {
                int flags = CFS_IC_NOTHING;
                char *xattr_name = req_capsule_client_get(info->mti_pill,
                                                          &RMF_NAME);
                CDEBUG(D_INODE, "getxattr %s\n", xattr_name);

                rc = mo_xattr_get(info->mti_env, next, buf, xattr_name);
                if (rc < 0) {
                        CERROR("getxattr failed: %d\n", rc);
                        GOTO(out, rc);
                }

                if (info->mti_body->valid &
                    (OBD_MD_FLRMTLSETFACL | OBD_MD_FLRMTLGETFACL))
                        flags = CFS_IC_ALL;
                else if (info->mti_body->valid & OBD_MD_FLRMTRGETFACL)
                        flags = CFS_IC_MAPPED;

                if (rc > 0 && flags != CFS_IC_NOTHING) {
                        int rc1;

                        if (unlikely(!remote))
                                GOTO(out, rc = -EINVAL);

                        rc1 = lustre_posix_acl_xattr_id2client(uc,
                                        med->med_idmap,
                                        (posix_acl_xattr_header *)(buf->lb_buf),
                                        rc, flags);
                        if (unlikely(rc1 < 0))
                                rc = rc1;
                }
        } else if (info->mti_body->valid & OBD_MD_FLXATTRLS) {
                CDEBUG(D_INODE, "listxattr\n");

                rc = mo_xattr_list(info->mti_env, next, buf);
                if (rc < 0)
                        CDEBUG(D_INFO, "listxattr failed: %d\n", rc);
        } else
                LBUG();

        EXIT;
out:
        if (rc >= 0) {
                mdt_counter_incr(req->rq_export, LPROC_MDT_GETXATTR);
                repbody->eadatasize = rc;
                rc = 0;
        }
        mdt_exit_ucred(info);
        return rc;
}

static int mdt_rmtlsetfacl(struct mdt_thread_info *info,
                           struct md_object *next,
                           const char *xattr_name,
                           ext_acl_xattr_header *header,
                           posix_acl_xattr_header **out)
{
        struct ptlrpc_request  *req = mdt_info_req(info);
        struct mdt_export_data *med = mdt_req2med(req);
        struct md_ucred        *uc = mdt_ucred(info);
        struct lu_buf          *buf = &info->mti_buf;
        int                     rc;
        ENTRY;

        rc = lustre_ext_acl_xattr_id2server(uc, med->med_idmap, header);
        if (rc)
                RETURN(rc);

        rc = mo_xattr_get(info->mti_env, next, &LU_BUF_NULL, xattr_name);
        if (rc == -ENODATA)
                rc = 0;
        else if (rc < 0)
                RETURN(rc);

        buf->lb_len = rc;
        if (buf->lb_len > 0) {
                OBD_ALLOC_LARGE(buf->lb_buf, buf->lb_len);
                if (unlikely(buf->lb_buf == NULL))
                        RETURN(-ENOMEM);

                rc = mo_xattr_get(info->mti_env, next, buf, xattr_name);
                if (rc < 0) {
                        CERROR("getxattr failed: %d\n", rc);
                        GOTO(_out, rc);
                }
        } else
                buf->lb_buf = NULL;

        rc = lustre_acl_xattr_merge2posix((posix_acl_xattr_header *)(buf->lb_buf),
                                          buf->lb_len, header, out);
        EXIT;

_out:
        if (rc <= 0 && buf->lb_buf != NULL)
                OBD_FREE_LARGE(buf->lb_buf, buf->lb_len);
        return rc;
}

int mdt_reint_setxattr(struct mdt_thread_info *info,
                       struct mdt_lock_handle *unused)
{
        struct ptlrpc_request   *req = mdt_info_req(info);
        struct md_ucred         *uc  = mdt_ucred(info);
        struct mdt_lock_handle  *lh;
        const struct lu_env     *env  = info->mti_env;
        struct lu_buf           *buf  = &info->mti_buf;
        struct mdt_reint_record *rr   = &info->mti_rr;
        struct md_attr          *ma = &info->mti_attr;
        struct lu_attr          *attr = &info->mti_attr.ma_attr;
        struct mdt_object       *obj;
        struct md_object        *child;
        __u64                    valid = attr->la_valid;
        const char              *xattr_name = rr->rr_name;
        int                      xattr_len = rr->rr_eadatalen;
        __u64                    lockpart;
        int                      rc;
        posix_acl_xattr_header  *new_xattr = NULL;
        __u32                    remote = exp_connect_rmtclient(info->mti_exp);
        __u32                    perm;
        ENTRY;

        CDEBUG(D_INODE, "setxattr for "DFID"\n", PFID(rr->rr_fid1));

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_SETXATTR))
                RETURN(err_serious(-ENOMEM));

        CDEBUG(D_INODE, "%s xattr %s\n",
               valid & OBD_MD_FLXATTR ? "set" : "remove", xattr_name);

        rc = mdt_init_ucred_reint(info);
        if (rc != 0)
                RETURN(rc);

        if (valid & OBD_MD_FLRMTRSETFACL) {
                if (unlikely(!remote))
                        GOTO(out, rc = err_serious(-EINVAL));

                perm = mdt_identity_get_perm(uc->mu_identity, remote,
                                             req->rq_peer.nid);
                if (!(perm & CFS_RMTACL_PERM))
                        GOTO(out, rc = err_serious(-EPERM));
        }

        if (strncmp(xattr_name, XATTR_USER_PREFIX,
                    sizeof(XATTR_USER_PREFIX) - 1) == 0) {
                if (!(req->rq_export->exp_connect_flags & OBD_CONNECT_XATTR))
                        GOTO(out, rc = -EOPNOTSUPP);
                if (strcmp(xattr_name, XATTR_NAME_LOV) == 0)
                        GOTO(out, rc = -EACCES);
                if (strcmp(xattr_name, XATTR_NAME_LMA) == 0)
                        GOTO(out, rc = 0);
                if (strcmp(xattr_name, XATTR_NAME_LINK) == 0)
                        GOTO(out, rc = 0);
        } else if ((valid & OBD_MD_FLXATTR) &&
                   (strncmp(xattr_name, XATTR_NAME_ACL_ACCESS,
                            sizeof(XATTR_NAME_ACL_ACCESS) - 1) == 0 ||
                    strncmp(xattr_name, XATTR_NAME_ACL_DEFAULT,
                            sizeof(XATTR_NAME_ACL_DEFAULT) - 1) == 0)) {
                /* currently lustre limit acl access size */
                if (xattr_len > LUSTRE_POSIX_ACL_MAX_SIZE)
                        GOTO(out, -ERANGE);
        }

        lockpart = MDS_INODELOCK_UPDATE;
        /* Revoke all clients' lookup lock, since the access
         * permissions for this inode is changed when ACL_ACCESS is
         * set. This isn't needed for ACL_DEFAULT, since that does
         * not change the access permissions of this inode, nor any
         * other existing inodes. It is setting the ACLs inherited
         * by new directories/files at create time. */
        if (!strcmp(xattr_name, XATTR_NAME_ACL_ACCESS))
                lockpart |= MDS_INODELOCK_LOOKUP;

        lh = &info->mti_lh[MDT_LH_PARENT];
        /* ACLs were sent to clients under LCK_CR locks, so taking LCK_EX
         * to cancel them. */
        mdt_lock_reg_init(lh, LCK_EX);
        obj = mdt_object_find_lock(info, rr->rr_fid1, lh, lockpart);
        if (IS_ERR(obj))
                GOTO(out, rc =  PTR_ERR(obj));

        info->mti_mos = obj;
        rc = mdt_version_get_check_save(info, obj, 0);
        if (rc)
                GOTO(out_unlock, rc);

        if (unlikely(!(valid & OBD_MD_FLCTIME))) {
                /* This isn't strictly an error, but all current clients
                 * should set OBD_MD_FLCTIME when setting attributes. */
                CWARN("%s: client miss to set OBD_MD_FLCTIME when "
                      "setxattr %s: [object "DFID"] [valid "LPU64"]\n",
                      info->mti_exp->exp_obd->obd_name, xattr_name,
                      PFID(rr->rr_fid1), valid);
                attr->la_ctime = cfs_time_current_sec();
        }
        attr->la_valid = LA_CTIME;
        child = mdt_object_child(obj);
        if (valid & OBD_MD_FLXATTR) {
                char *xattr = (void *)rr->rr_eadata;

                if (xattr_len > 0) {
                        int flags = 0;

                        if (valid & OBD_MD_FLRMTLSETFACL) {
                                if (unlikely(!remote))
                                        GOTO(out_unlock, rc = -EINVAL);

                                xattr_len = mdt_rmtlsetfacl(info, child,
                                                xattr_name,
                                                (ext_acl_xattr_header *)xattr,
                                                &new_xattr);
                                if (xattr_len < 0)
                                        GOTO(out_unlock, rc = xattr_len);

                                xattr = (char *)new_xattr;
                        }

                        if (attr->la_flags & XATTR_REPLACE)
                                flags |= LU_XATTR_REPLACE;

                        if (attr->la_flags & XATTR_CREATE)
                                flags |= LU_XATTR_CREATE;

                        mdt_fail_write(env, info->mti_mdt->mdt_bottom,
                                       OBD_FAIL_MDS_SETXATTR_WRITE);

                        buf->lb_buf = xattr;
                        buf->lb_len = xattr_len;
                        rc = mo_xattr_set(env, child, buf, xattr_name, flags);
                        /* update ctime after xattr changed */
                        if (rc == 0) {
                                ma->ma_attr_flags |= MDS_PERM_BYPASS;
                                mo_attr_set(env, child, ma);
                        }
                }
        } else if (valid & OBD_MD_FLXATTRRM) {
                rc = mo_xattr_del(env, child, xattr_name);
                /* update ctime after xattr changed */
                if (rc == 0) {
                        ma->ma_attr_flags |= MDS_PERM_BYPASS;
                        mo_attr_set(env, child, ma);
                }
        } else {
                CDEBUG(D_INFO, "valid bits: "LPX64"\n", valid);
                rc = -EINVAL;
        }
        if (rc == 0)
                mdt_counter_incr(req->rq_export, LPROC_MDT_SETXATTR);

        EXIT;
out_unlock:
        mdt_object_unlock_put(info, obj, lh, rc);
        if (unlikely(new_xattr != NULL))
                lustre_posix_acl_xattr_free(new_xattr, xattr_len);
out:
        mdt_exit_ucred(info);
        return rc;
}
