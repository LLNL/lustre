/*
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
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/llite/acl.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 */

#include "llite_internal.h"

struct posix_acl *ll_get_acl(struct inode *inode, int type
#ifdef HAVE_GET_ACL_RCU_ARG
			     , bool rcu
#endif /* HAVE_GET_ACL_RCU_ARG */
			    )
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct posix_acl *acl = NULL;
	ENTRY;

#ifdef HAVE_GET_ACL_RCU_ARG
	if (rcu)
		return ERR_PTR(-ECHILD);
#endif

	read_lock(&lli->lli_lock);
	/* VFS' acl_permission_check->check_acl will release the refcount */
	acl = posix_acl_dup(lli->lli_posix_acl);
	read_unlock(&lli->lli_lock);

	RETURN(acl);
}

#ifdef HAVE_IOP_SET_ACL
int ll_set_acl(struct user_namespace *mnt_userns, struct inode *inode,
	       struct posix_acl *acl, int type)
{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct ptlrpc_request *req = NULL;
	const char *name = NULL;
	char *value = NULL;
	size_t value_size = 0;
	int rc = 0;
	ENTRY;

	switch (type) {
	case ACL_TYPE_ACCESS:
		name = XATTR_NAME_POSIX_ACL_ACCESS;
		if (acl)
			rc = posix_acl_update_mode(mnt_userns, inode,
						   &inode->i_mode, &acl);
		break;

	case ACL_TYPE_DEFAULT:
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
		if (!S_ISDIR(inode->i_mode))
			rc = acl ? -EACCES : 0;
		break;

	default:
		rc = -EINVAL;
		break;
	}
	if (rc)
		return rc;

	if (acl) {
		value_size = posix_acl_xattr_size(acl->a_count);
		value = kmalloc(value_size, GFP_NOFS);
		if (value == NULL)
			GOTO(out, rc = -ENOMEM);

		rc = posix_acl_to_xattr(&init_user_ns, acl, value, value_size);
		if (rc < 0)
			GOTO(out_value, rc);
	}

	rc = md_setxattr(sbi->ll_md_exp, ll_inode2fid(inode),
			 value ? OBD_MD_FLXATTR : OBD_MD_FLXATTRRM,
			 name, value, value_size, 0, 0, &req);

	ptlrpc_req_finished(req);
out_value:
	kfree(value);
out:
	if (rc)
		forget_cached_acl(inode, type);
	else
		set_cached_acl(inode, type, acl);
	RETURN(rc);
}
#endif /* HAVE_IOP_SET_ACL */
