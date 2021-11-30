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
 * version 2 along with this program; If not, see http://www.gnu.org/licenses
 *
 * GPL HEADER END
 */

/*
 * Copyright (c) 2014 Bull SAS
 *
 * Copyright (c) 2015, 2016, Intel Corporation.
 * Author: Sebastien Buisson sebastien.buisson@bull.net
 */

/*
 * lustre/llite/xattr_security.c
 * Handler for storing security labels as extended attributes.
 */

#include <linux/types.h>
#include <linux/security.h>
#ifdef HAVE_LINUX_SELINUX_IS_ENABLED
#include <linux/selinux.h>
#endif
#include <linux/xattr.h>
#include "llite_internal.h"

#ifndef XATTR_SELINUX_SUFFIX
# define XATTR_SELINUX_SUFFIX "selinux"
#endif

#ifndef XATTR_NAME_SELINUX
# define XATTR_NAME_SELINUX XATTR_SECURITY_PREFIX XATTR_SELINUX_SUFFIX
#endif

/*
 * Check for LL_SBI_FILE_SECCTX before calling.
 */
int ll_dentry_init_security(struct dentry *dentry, int mode, struct qstr *name,
			    const char **secctx_name, void **secctx,
			    __u32 *secctx_size)
{
	int rc;

	/*
	 * security_dentry_init_security() is strange. Like
	 * security_inode_init_security() it may return a context (provided a
	 * Linux security module is enabled) but unlike
	 * security_inode_init_security() it does not return to us the name of
	 * the extended attribute to store the context under (for example
	 * "security.selinux"). So we only call it when we think we know what
	 * the name of the extended attribute will be. This is OK-ish since
	 * SELinux is the only module that implements
	 * security_dentry_init_security(). Note that the NFS client code just
	 * calls it and assumes that if anything is returned then it must come
	 * from SELinux.
	 */

	if (!selinux_is_enabled())
		return 0;

	rc = security_dentry_init_security(dentry, mode, name, secctx,
					   secctx_size);
	/* Usually, security_dentry_init_security() returns -EOPNOTSUPP when
	 * SELinux is disabled.
	 * But on some kernels (e.g. rhel 8.5) it returns 0 when SELinux is
	 * disabled, and in this case the security context is empty.
	 */
	if (rc == -EOPNOTSUPP || (rc == 0 && *secctx_size == 0))
		/* do nothing */
		return 0;
	if (rc < 0)
		return rc;

	*secctx_name = XATTR_NAME_SELINUX;

	return 0;
}

/**
 * A helper function for security_inode_init_security()
 * that takes care of setting xattrs
 *
 * Get security context of @inode from @xattr_array,
 * and put it in 'security.xxx' xattr of dentry
 * stored in @fs_info.
 *
 * \retval 0        success
 * \retval -ENOMEM  if no memory could be allocated for xattr name
 * \retval < 0      failure to set xattr
 */
static int
ll_initxattrs(struct inode *inode, const struct xattr *xattr_array,
	      void *fs_info)
{
	struct dentry *dentry = fs_info;
	const struct xattr *xattr;
	int err = 0;

	for (xattr = xattr_array; xattr->name; xattr++) {
		char *full_name;

		full_name = kasprintf(GFP_KERNEL, "%s%s",
				      XATTR_SECURITY_PREFIX, xattr->name);
		if (!full_name) {
			err = -ENOMEM;
			break;
		}

		err = ll_vfs_setxattr(dentry, inode, full_name, xattr->value,
				      xattr->value_len, XATTR_CREATE);
		kfree(full_name);
		if (err < 0)
			break;
	}
	return err;
}

/**
 * Initializes security context
 *
 * Get security context of @inode in @dir,
 * and put it in 'security.xxx' xattr of @dentry.
 *
 * \retval 0        success, or SELinux is disabled
 * \retval -ENOMEM  if no memory could be allocated for xattr name
 * \retval < 0      failure to get security context or set xattr
 */
int
ll_inode_init_security(struct dentry *dentry, struct inode *inode,
		       struct inode *dir)
{
	int rc;

	if (!selinux_is_enabled())
		return 0;

	rc = security_inode_init_security(inode, dir, NULL,
					  &ll_initxattrs, dentry);
	if (rc == -EOPNOTSUPP)
		return 0;

	return rc;
}

/**
 * Get security context xattr name used by policy.
 *
 * \retval >= 0     length of xattr name
 * \retval < 0      failure to get security context xattr name
 */
int
ll_listsecurity(struct inode *inode, char *secctx_name, size_t secctx_name_size)
{
	int rc;

	if (!selinux_is_enabled())
		return 0;

	rc = security_inode_listsecurity(inode, secctx_name, secctx_name_size);
	if (rc >= secctx_name_size)
		rc = -ERANGE;
	else if (rc >= 0)
		secctx_name[rc] = '\0';
	return rc;
}
