/*
 * LGPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 or (at your discretion) any later version.
 * (LGPL) version 2.1 accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * LGPL HEADER END
 */
/*
 * lustre/utils/liblustreapi_layout.c
 *
 * lustreapi library for layout calls for interacting with the layout of
 * Lustre files while hiding details of the internal data structures
 * from the user.
 *
 * Author: Ned Bass <bass6@llnl.gov>
 */

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/xattr.h>

#include <liblustre.h>
#include <obd.h>
#include <lustre/lustreapi.h>
#include "lustreapi_internal.h"

/**
 * lustre_layout - an opaque data type abstracting the layout of a
 * Lustre file.
 *
 * Duplicate the fields we care about from struct lov_user_md_v3.
 * Deal with v1 versus v3 format issues only when we read or write
 * files.  Default to v3 format for new files.
 */
struct lustre_layout {
	int llot_pattern;
	int llot_stripe_size;
	int llot_stripe_count;
	int llot_stripe_offset;
	/** Indicates if llot_objects array has been initialized. */
	int	llot_objects_validity;
	/* Add 1 so user always gets back a NULL-terminated string. */
	char	llot_pool_name[LOV_MAXPOOLNAME + 1];
	struct	lov_user_ost_data_v1 llot_objects[0];
};

/**
 * Private helper to allocate storage for a lustre_layout with
 * \a num_stripes stripes.
 */
static lustre_layout_t *__llapi_layout_alloc(int num_stripes)
{
	lustre_layout_t *layout;
	size_t size = sizeof(lustre_layout_t) +
		(num_stripes * sizeof(struct lov_user_ost_data_v1));

	layout = malloc(size);
	if (layout != NULL)
		bzero(layout, size);
	return layout;
}

/**
 * Private helper to copy the data from a lov_user_md_v3 to a
 * newly allocated lustre_layout.
 */
static lustre_layout_t *
__llapi_layout_from_lum(const struct lov_user_md_v3 *lum)
{
	lustre_layout_t *layout;
	struct lov_user_md_v1 *lum_v1 = (struct lov_user_md_v1 *)lum;
	size_t objects_sz;

	objects_sz = lum->lmm_stripe_count * sizeof(struct lov_ost_data_v1);

	layout = __llapi_layout_alloc(lum->lmm_stripe_count);
	if (layout == NULL)
		return NULL;

	layout->llot_pattern		= lum->lmm_pattern;
	layout->llot_stripe_size	= lum->lmm_stripe_size;
	layout->llot_stripe_count	= lum->lmm_stripe_count;
	/* Don't copy lmm_stripe_offset: it is always zero
	 * when reading attributes. */

	if (lum->lmm_magic == LOV_USER_MAGIC_V3) {
		strncpy(layout->llot_pool_name, lum->lmm_pool_name,
			sizeof(layout->llot_pool_name));
		memcpy(layout->llot_objects, lum->lmm_objects, objects_sz);
	} else {
		memcpy(layout->llot_objects, lum_v1->lmm_objects, objects_sz);
	}
	layout->llot_objects_validity = 1;

	return layout;
}

/**
 * Private helper to copy the data from a lustre_layout to a
 * newly allocated lov_user_md_v3.
 */
static struct lov_user_md_v3 *
__llapi_layout_to_lum(const lustre_layout_t *layout)
{
	struct lov_user_md_v3 *lum;

	lum = malloc(sizeof(*lum));
	if (lum == NULL)
		return NULL;

	lum->lmm_magic		= LOV_USER_MAGIC_V3;
	lum->lmm_pattern	= layout->llot_pattern;
	lum->lmm_stripe_size	= layout->llot_stripe_size;
	lum->lmm_stripe_count	= layout->llot_stripe_count;
	lum->lmm_stripe_offset	= layout->llot_stripe_offset;

	strncpy(lum->lmm_pool_name, layout->llot_pool_name,
		sizeof(lum->lmm_pool_name));

	return lum;
}

/** Allocate and initialize a new layout. */
lustre_layout_t *llapi_layout_alloc()
{
	lustre_layout_t *layout;

	layout = __llapi_layout_alloc(0);
	if (layout == NULL)
		return layout;

	/* Set defaults. */
	layout->llot_pattern = 0; /* Only RAID0 is supported for now. */
	layout->llot_stripe_size = 0;
	layout->llot_stripe_count = 0;
	layout->llot_stripe_offset = -1;
	layout->llot_objects_validity = 0;
	layout->llot_pool_name[0] = '\0';

	return layout;
}

/**
 *  Populate an opaque data type containing the layout for the file
 *  referenced by open file descriptor \a fd.
 */
lustre_layout_t *llapi_layout_lookup_byfd(int fd)
{
	int rc;
	ssize_t lum_len;
	struct lov_user_md_v3 *lum = NULL;
	lustre_layout_t *layout = NULL;

	lum_len = sizeof(struct lov_user_md_v3) +
		LOV_MAX_STRIPE_COUNT * sizeof(struct lov_user_ost_data_v1);
	lum = malloc(lum_len);
	if (lum == NULL) {
		rc = ENOMEM;
		goto out;
	}

	rc = fgetxattr(fd, XATTR_LUSTRE_LOV, lum, lum_len);
	rc = (rc < 0) ? errno : 0;
	if (rc == 0) {
		layout = __llapi_layout_from_lum(lum);
		if (layout == NULL)
			rc = ENOMEM;
	}
out:
	free(lum);
	errno = rc;
	return layout;
}

/**
 *  Populate an opaque data type containing the layout for the file at \a path.
 */
lustre_layout_t *llapi_layout_lookup_bypath(const char *path)
{
	int fd;
	lustre_layout_t *layout = NULL;

	fd = open(path, O_RDONLY);
	if (fd < 0 && errno == EISDIR) {
		fd = open(path, O_DIRECTORY | O_RDONLY);
	}
	if (fd >= 0) {
		layout = llapi_layout_lookup_byfd(fd);
		close (fd);
	}
	return layout;
}

/**
 *  Populate an opaque data type containing layout for the file with
 *  Lustre file identifier string \a fidstr in filesystem \a lustre_dir.
 */
lustre_layout_t
*llapi_layout_lookup_byfid(const char *lustre_dir, const char *fidstr)
{
	char file[PATH_MAX];
	char path[PATH_MAX];
	int rc;
	long long recno = -1;
	int linkno = 0;
	lustre_layout_t *layout = NULL;

	llapi_msg_set_level(LLAPI_MSG_OFF);
	rc = llapi_fid2path(lustre_dir, fidstr, file, sizeof(path), &recno,
			    &linkno);

	if (rc != 0) {
		errno = -rc;
		return NULL;
	}

	if (llapi_search_mounts(lustre_dir, 0, path, NULL) == 0) {
		strcat(path, "/");
		strcat(path, file);
		layout = llapi_layout_lookup_bypath(path);
	}
	return layout;
}

/** Free memory allocated for \a layout */
void llapi_layout_free(lustre_layout_t *layout)
{
	if (layout != NULL)
		free(layout);
}

/** Read stripe count of \a layout. */
int llapi_layout_stripe_count(const lustre_layout_t *layout)
{
	if (layout != NULL) {
		/* Distinguish valid -1 return value (meaning use all
		 * available OSTs) from an error. */
		if (layout->llot_stripe_count == -1)
			errno = 0;
		return layout->llot_stripe_count;
	}
	errno = EINVAL;
	return -1;
}

/** Modify stripe count of \a layout. */
int llapi_layout_stripe_count_set(lustre_layout_t *layout, int stripe_count)
{
	if (layout == NULL || llapi_stripe_count_validity(stripe_count) != 0) {
		errno = EINVAL;
		return -1;
	}
	layout->llot_stripe_count = stripe_count;
	return 0;
}

/** Read the size of each stripe in \a layout. */
unsigned long long llapi_layout_stripe_size(const lustre_layout_t *layout)
{
	if (layout != NULL)
		return layout->llot_stripe_size;
	errno = EINVAL;
	return -1;
}

/** Modify the size of each stripe in \a layout. */
int llapi_layout_stripe_size_set(lustre_layout_t *layout,
				 unsigned long long stripe_size)
{
	if (layout == NULL || llapi_stripe_size_validity(stripe_size) != 0 ||
	    llapi_stripe_size_excess(stripe_size) != 0) {
		errno = EINVAL;
		return -1;
	}

	layout->llot_stripe_size = stripe_size;
	return 0;
}

/** Read stripe pattern of \a layout. */
int llapi_layout_pattern(const lustre_layout_t *layout)
{
	if (layout != NULL)
		return layout->llot_pattern;
	errno = EINVAL;
	return -1;
}

/** Modify stripe count of \a layout. */
int llapi_layout_pattern_set(lustre_layout_t *layout, int pattern)
{
	if (layout == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (pattern != 0) {
		errno = EOPNOTSUPP;
		return -1;
	}
	layout->llot_pattern = pattern;
	return 0;
}

/**
 * Set the OST index in layout associated with stripe number
 * \a stripe_number to \a ost_index.
 * NB: this only works for stripe_number=0 today.
 */
int llapi_layout_ost_index_set(lustre_layout_t *layout, int stripe_number,
			       int ost_index)
{
	if (layout == NULL || llapi_stripe_offset_validity(ost_index) != 0) {
		errno = EINVAL;
		return -1;
	}

	if (stripe_number != 0) {
		errno = EOPNOTSUPP;
		return -1;
	}

	layout->llot_stripe_offset = ost_index;
	return 0;
}

/**
 * Return the OST idex associated with stripe \a stripe_number.
 * Stripes are indexed starting from zero.
 */
int llapi_layout_ost_index(const lustre_layout_t *layout, int stripe_number)
{
	if (layout == NULL || stripe_number >= layout->llot_stripe_count) {
		errno = EINVAL;
		return -1;
	}

	if (layout->llot_objects_validity == 0) {
		errno = 0;
		return -1;
	}

	return layout->llot_objects[stripe_number].l_ost_idx;
}

/** Return the object identifier associated with stripe \a stripe_number. */
int llapi_layout_obj_id(const lustre_layout_t *layout, int stripe_number)
{
	if (layout == NULL || stripe_number >= layout->llot_stripe_count) {
		errno = EINVAL;
		return -1;
	}

	if (layout->llot_objects_validity == 0) {
		errno = 0;
		return -1;
	}

	return layout->llot_objects[stripe_number].l_ost_oi.oi.oi_id;
}

/**
 * Return a string containing the name of the pool of OSTs
 * on which file objects in \a layout will be stored.
 */
const char *llapi_layout_pool_name(const lustre_layout_t *layout)
{
	if (layout == NULL) {
		errno = EINVAL;
		return NULL;
	}
	return layout->llot_pool_name;
}

/**
 * Set the name of the pool of OSTs on which file objects in \a layout
 * will be stored to \a pool_name.
 */
int llapi_layout_pool_name_set(lustre_layout_t *layout, const char *pool_name)
{
	char *ptr;

	if (layout == NULL || pool_name == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Strip off any 'fsname.' portion. */
	ptr = strchr(pool_name, '.');
	if (ptr != NULL)
		pool_name = ptr + 1;

	if (strlen(pool_name) > LOV_MAXPOOLNAME) {
		errno = EINVAL;
		return -1;
	}

	strncpy(layout->llot_pool_name, pool_name,
		sizeof(layout->llot_pool_name));
	return 0;
}

/**
 * Create a new file with the specified \a layout with the name \a path
 * using permissions in \a mode and open() \a flags.  Return an open
 * file descriptor for the new file.
 */
int llapi_layout_file_create(const lustre_layout_t *layout, char *path,
			     int mode, int flags)
{
	int fd;
	struct lov_user_md_v3 *lum;

	if (layout == NULL) {
		errno = EINVAL;
		return -1;
	}

	fd = open(path, flags | O_LOV_DELAY_CREATE | O_CREAT | O_EXCL, mode);

	if (fd < 0)
		return -1;

	lum = __llapi_layout_to_lum(layout);
	if (lum == NULL) {
		errno = ENOMEM;
		close(fd);
		fd = -1;
	}

	if (fd != -1) {
		if (fsetxattr(fd, XATTR_LUSTRE_LOV, (void *)lum, sizeof(*lum),
			      XATTR_CREATE) < 0)  {
			close(fd);
			fd = -1;
		}
	}
	free(lum);
	return fd;
}
