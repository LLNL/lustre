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
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef _LUSTREAPI_H_
#define _LUSTREAPI_H_

/** \defgroup llapi llapi
 *
 * @{
 */

#include <stdarg.h>
#include <stdint.h>
#include <lustre/lustre_user.h>

typedef void (*llapi_cb_t)(char *obd_type_name, char *obd_name, char *obd_uuid,
			   void *args);

/* lustreapi message severity level */
enum llapi_message_level {
        LLAPI_MSG_OFF    = 0,
        LLAPI_MSG_FATAL  = 1,
        LLAPI_MSG_ERROR  = 2,
        LLAPI_MSG_WARN   = 3,
        LLAPI_MSG_NORMAL = 4,
        LLAPI_MSG_INFO   = 5,
        LLAPI_MSG_DEBUG  = 6,
        LLAPI_MSG_MAX
};

typedef void (*llapi_log_callback_t)(enum llapi_message_level level, int err,
				     const char *fmt, va_list ap);


/* the bottom three bits reserved for llapi_message_level */
#define LLAPI_MSG_MASK          0x00000007
#define LLAPI_MSG_NO_ERRNO      0x00000010

static inline const char *llapi_msg_level2str(enum llapi_message_level level)
{
	static const char *levels[LLAPI_MSG_MAX] = {"OFF", "FATAL", "ERROR",
						    "WARNING", "NORMAL",
						    "INFO", "DEBUG"};

	if (level >= LLAPI_MSG_MAX)
		return NULL;

	return levels[level];
}
extern void llapi_msg_set_level(int level);
int llapi_msg_get_level(void);
extern llapi_log_callback_t llapi_error_callback_set(llapi_log_callback_t cb);
extern llapi_log_callback_t llapi_info_callback_set(llapi_log_callback_t cb);

void llapi_error(enum llapi_message_level level, int err, const char *fmt, ...)
	__attribute__((__format__(__printf__, 3, 4)));
#define llapi_err_noerrno(level, fmt, a...)			\
	llapi_error((level) | LLAPI_MSG_NO_ERRNO, 0, fmt, ## a)
void llapi_printf(enum llapi_message_level level, const char *fmt, ...)
	__attribute__((__format__(__printf__, 2, 3)));

extern int llapi_file_create(const char *name, unsigned long long stripe_size,
                             int stripe_offset, int stripe_count,
                             int stripe_pattern);
extern int llapi_file_open(const char *name, int flags, int mode,
                           unsigned long long stripe_size, int stripe_offset,
                           int stripe_count, int stripe_pattern);
extern int llapi_file_create_pool(const char *name,
                                  unsigned long long stripe_size,
                                  int stripe_offset, int stripe_count,
                                  int stripe_pattern, char *pool_name);
extern int llapi_file_open_pool(const char *name, int flags, int mode,
                                unsigned long long stripe_size,
                                int stripe_offset, int stripe_count,
                                int stripe_pattern, char *pool_name);
extern int llapi_poollist(const char *name);
extern int llapi_get_poollist(const char *name, char **poollist, int list_size,
                              char *buffer, int buffer_size);
extern int llapi_get_poolmembers(const char *poolname, char **members,
                                 int list_size, char *buffer, int buffer_size);
extern int llapi_file_get_stripe(const char *path, struct lov_user_md *lum);
#define HAVE_LLAPI_FILE_LOOKUP
extern int llapi_file_lookup(int dirfd, const char *name);

#define VERBOSE_COUNT		0x1
#define VERBOSE_SIZE		0x2
#define VERBOSE_OFFSET		0x4
#define VERBOSE_POOL		0x8
#define VERBOSE_DETAIL		0x10
#define VERBOSE_OBJID		0x20
#define VERBOSE_GENERATION	0x40
#define VERBOSE_MDTINDEX	0x80
#define VERBOSE_LAYOUT		0x100
#define VERBOSE_ALL		(VERBOSE_COUNT | VERBOSE_SIZE | \
				 VERBOSE_OFFSET | VERBOSE_POOL | \
				 VERBOSE_OBJID | VERBOSE_GENERATION |\
				 VERBOSE_LAYOUT)

struct find_param {
	unsigned int		 maxdepth;
	time_t			 atime;
	time_t			 mtime;
	time_t			 ctime;
	/* cannot be bitfields due to using pointers to */
	int			 asign;
	/* access them during argument parsing. */
	int			 csign;
	int			 msign;
	int			 type;
	/* these need to be signed values */
	int			 size_sign:2,
				 stripesize_sign:2,
				 stripecount_sign:2;
	unsigned long long	 size;
	unsigned long long	 size_units;
	uid_t			 uid;
	gid_t			 gid;

	unsigned long		 zeroend:1,
				 recursive:1,
				 exclude_pattern:1,
				 exclude_type:1,
				 exclude_obd:1,
				 exclude_mdt:1,
				 exclude_gid:1,
				 exclude_uid:1,
				 check_gid:1,		/* group ID */
				 check_uid:1,		/* user ID */
				 check_pool:1,		/* LOV pool name */
				 check_size:1,		/* file size */
				 exclude_pool:1,
				 exclude_size:1,
				 exclude_atime:1,
				 exclude_mtime:1,
				 exclude_ctime:1,
				 get_lmv:1,	/* get MDT list from LMV */
				 raw:1,		/* do not fill in defaults */
				 check_stripesize:1,	/* LOV stripe size */
				 exclude_stripesize:1,
				 check_stripecount:1,	/* LOV stripe count */
				 exclude_stripecount:1,
				 check_layout:1,
				 exclude_layout:1;

	int			 verbose;
	int			 quiet;

	/* regular expression */
	char			*pattern;

	char			*print_fmt;

	struct  obd_uuid	*obduuid;
	int			 num_obds;
	int			 num_alloc_obds;
	int			 obdindex;
	int			*obdindexes;

	struct  obd_uuid	*mdtuuid;
	int			 num_mdts;
	int			 num_alloc_mdts;
	int			 mdtindex;
	int			*mdtindexes;
	int			 file_mdtindex;

	int			 lumlen;
	struct  lov_user_mds_data	*lmd;

	char			poolname[LOV_MAXPOOLNAME + 1];

	int			 fp_lmv_count;
	struct lmv_user_md	*fp_lmv_md;

	unsigned long long	 stripesize;
	unsigned long long	 stripesize_units;
	unsigned long long	 stripecount;
	__u32			 layout;

	/* In-process parameters. */
	unsigned long		 got_uuids:1,
				 obds_printed:1,
				 have_fileinfo:1; /* file attrs and LOV xattr */
	unsigned int		 depth;
	dev_t			 st_dev;
	__u64			 padding1;
	__u64			 padding2;
	__u64			 padding3;
	__u64			 padding4;
};

extern int llapi_ostlist(char *path, struct find_param *param);
extern int llapi_uuid_match(char *real_uuid, char *search_uuid);
extern int llapi_getstripe(char *path, struct find_param *param);
extern int llapi_find(char *path, struct find_param *param);

extern int llapi_file_fget_mdtidx(int fd, int *mdtidx);
extern int llapi_dir_create_pool(const char *name, int flags, int stripe_offset,
				 int stripe_count, int stripe_pattern,
				 char *poolname);
int llapi_direntry_remove(char *dname);
extern int llapi_obd_statfs(char *path, __u32 type, __u32 index,
                     struct obd_statfs *stat_buf,
                     struct obd_uuid *uuid_buf);
extern int llapi_ping(char *obd_type, char *obd_name);
extern int llapi_target_check(int num_types, char **obd_types, char *dir);
extern int llapi_file_get_lov_uuid(const char *path, struct obd_uuid *lov_uuid);
extern int llapi_file_get_lmv_uuid(const char *path, struct obd_uuid *lmv_uuid);
extern int llapi_file_fget_lov_uuid(int fd, struct obd_uuid *lov_uuid);
extern int llapi_lov_get_uuids(int fd, struct obd_uuid *uuidp, int *ost_count);
extern int llapi_lmv_get_uuids(int fd, struct obd_uuid *uuidp, int *mdt_count);
extern int llapi_is_lustre_mnttype(const char *type);
extern int llapi_search_ost(char *fsname, char *poolname, char *ostname);
extern int llapi_get_obd_count(char *mnt, int *count, int is_mdt);
extern int llapi_parse_size(const char *optarg, unsigned long long *size,
			    unsigned long long *size_units, int bytes_spec);
extern int llapi_search_mounts(const char *pathname, int index,
                               char *mntdir, char *fsname);
extern int llapi_search_fsname(const char *pathname, char *fsname);
extern int llapi_getname(const char *path, char *buf, size_t size);

extern void llapi_ping_target(char *obd_type, char *obd_name,
                              char *obd_uuid, void *args);

extern int llapi_search_rootpath(char *pathname, const char *fsname);

struct mntent;
#define HAVE_LLAPI_IS_LUSTRE_MNT
extern int llapi_is_lustre_mnt(struct mntent *mnt);
extern int llapi_quotachown(char *path, int flag);
extern int llapi_quotacheck(char *mnt, int check_type);
extern int llapi_poll_quotacheck(char *mnt, struct if_quotacheck *qchk);
extern int llapi_quotactl(char *mnt, struct if_quotactl *qctl);
extern int llapi_target_iterate(int type_num, char **obd_type, void *args,
				llapi_cb_t cb);
extern int llapi_get_connect_flags(const char *mnt, __u64 *flags);
extern int llapi_lsetfacl(int argc, char *argv[]);
extern int llapi_lgetfacl(int argc, char *argv[]);
extern int llapi_rsetfacl(int argc, char *argv[]);
extern int llapi_rgetfacl(int argc, char *argv[]);
extern int llapi_cp(int argc, char *argv[]);
extern int llapi_ls(int argc, char *argv[]);
extern int llapi_fid2path(const char *device, const char *fidstr, char *path,
			  int pathlen, long long *recno, int *linkno);
extern int llapi_path2fid(const char *path, lustre_fid *fid);
extern int llapi_fd2fid(const int fd, lustre_fid *fid);
extern int llapi_chomp_string(char *buf);
extern int llapi_open_by_fid(const char *dir, const lustre_fid *fid,
			     int open_flags);

extern int llapi_get_version(char *buffer, int buffer_size, char **version);
extern int llapi_get_data_version(int fd, __u64 *data_version, __u64 flags);
extern int llapi_hsm_state_get_fd(int fd, struct hsm_user_state *hus);
extern int llapi_hsm_state_get(const char *path, struct hsm_user_state *hus);
extern int llapi_hsm_state_set_fd(int fd, __u64 setmask, __u64 clearmask,
				  __u32 archive_id);
extern int llapi_hsm_state_set(const char *path, __u64 setmask, __u64 clearmask,
			       __u32 archive_id);
extern int llapi_hsm_register_event_fifo(const char *path);
extern int llapi_hsm_unregister_event_fifo(const char *path);
extern void llapi_hsm_log_error(enum llapi_message_level level, int _rc,
				const char *fmt, va_list args);

extern int llapi_get_agent_uuid(char *path, char *buf, size_t bufsize);
extern int llapi_create_volatile_idx(char *directory, int idx, int mode);
static inline int llapi_create_volatile(char *directory, int mode)
{
	return llapi_create_volatile_idx(directory, -1, mode);
}


extern int llapi_fswap_layouts(const int fd1, const int fd2,
			       __u64 dv1, __u64 dv2, __u64 flags);
extern int llapi_swap_layouts(const char *path1, const char *path2,
			      __u64 dv1, __u64 dv2, __u64 flags);

/* Changelog interface.  priv is private state, managed internally
   by these functions */
#define CHANGELOG_FLAG_FOLLOW 0x01   /* Not yet implemented */
#define CHANGELOG_FLAG_BLOCK  0x02   /* Blocking IO makes sense in case of
   slow user parsing of the records, but it also prevents us from cleaning
   up if the records are not consumed. */

/* Records received are in extentded format now, though most of them are still
 * written in disk in changelog_rec format (to save space and time), it's
 * converted to extented format in the lustre api to ease changelog analysis. */
#define HAVE_CHANGELOG_EXTEND_REC 1

extern int llapi_changelog_start(void **priv, int flags, const char *mdtname,
                                 long long startrec);
extern int llapi_changelog_fini(void **priv);
extern int llapi_changelog_recv(void *priv, struct changelog_ext_rec **rech);
extern int llapi_changelog_free(struct changelog_ext_rec **rech);
/* Allow records up to endrec to be destroyed; requires registered id. */
extern int llapi_changelog_clear(const char *mdtname, const char *idstr,
                                 long long endrec);

/* HSM copytool interface.
 * priv is private state, managed internally by these functions
 */
struct hsm_copytool_private;
struct hsm_copyaction_private;

extern int llapi_hsm_copytool_register(struct hsm_copytool_private **priv,
				       const char *mnt, int archive_count,
				       int *archives, int rfd_flags);
extern int llapi_hsm_copytool_unregister(struct hsm_copytool_private **priv);
extern int llapi_hsm_copytool_get_fd(struct hsm_copytool_private *ct);
extern int llapi_hsm_copytool_recv(struct hsm_copytool_private *priv,
				   struct hsm_action_list **hal, int *msgsize);
extern int llapi_hsm_action_begin(struct hsm_copyaction_private **phcp,
				  const struct hsm_copytool_private *ct,
				  const struct hsm_action_item *hai,
				  int restore_mdt_index, int restore_open_flags,
				  bool is_error);
extern int llapi_hsm_action_end(struct hsm_copyaction_private **phcp,
				const struct hsm_extent *he,
				int hp_flags, int errval);
extern int llapi_hsm_action_progress(struct hsm_copyaction_private *hcp,
				     const struct hsm_extent *he, __u64 total,
				     int hp_flags);
extern int llapi_hsm_action_get_dfid(const struct hsm_copyaction_private *hcp,
				     lustre_fid *fid);
extern int llapi_hsm_action_get_fd(const struct hsm_copyaction_private *hcp);
extern int llapi_hsm_import(const char *dst, int archive, const struct stat *st,
			    unsigned long long stripe_size, int stripe_offset,
			    int stripe_count, int stripe_pattern,
			    char *pool_name, lustre_fid *newfid);

/* HSM user interface */
extern struct hsm_user_request *llapi_hsm_user_request_alloc(int itemcount,
							     int data_len);
extern int llapi_hsm_request(const char *path,
			     const struct hsm_user_request *request);
extern int llapi_hsm_current_action(const char *path,
				    struct hsm_current_action *hca);

/* JSON handling */
extern int llapi_json_init_list(struct llapi_json_item_list **item_list);
extern int llapi_json_destroy_list(struct llapi_json_item_list **item_list);
extern int llapi_json_add_item(struct llapi_json_item_list **item_list,
			       char *key, __u32 type, void *val);
extern int llapi_json_write_list(struct llapi_json_item_list **item_list,
				 FILE *fp);

/* llapi_layout user interface */

/** Opaque data type abstracting the layout of a Lustre file. */
struct llapi_layout;

/*
 * Flags to control how layouts are retrieved.
 */

/* Replace non-specified values with expected inherited values. */
#define LAYOUT_GET_EXPECTED 0x1

/**
 * Return a pointer to a newly-allocated opaque data structure containing
 * the layout for the file at \a path.  The pointer should be freed with
 * llapi_layout_free() when it is no longer needed. Failure is indicated
 * by a NULL return value and an appropriate error code stored in errno.
 */
struct llapi_layout *llapi_layout_get_by_path(const char *path, uint32_t flags);

/**
 * Return a pointer to a newly-allocated opaque data type containing the
 * layout for the file referenced by open file descriptor \a fd.  The
 * pointer should be freed with llapi_layout_free() when it is no longer
 * needed. Failure is indicated by a NULL return value and an
 * appropriate error code stored in errno.
 */
struct llapi_layout *llapi_layout_get_by_fd(int fd, uint32_t flags);

/**
 * Return a pointer to a newly-allocated opaque data type containing the
 * layout for the file associated with Lustre file identifier string
 * \a fidstr.  The string \a path must name a path within the
 * filesystem that contains the file being looked up, such as the
 * filesystem root.  The returned pointer should be freed with
 * llapi_layout_free() when it is no longer needed.  Failure is
 * indicated with a NULL return value and an appropriate error code
 * stored in errno.
 */
struct llapi_layout *llapi_layout_get_by_fid(const char *path,
					     const lustre_fid *fid,
					     uint32_t flags);

/**
 * Allocate a new layout. Use this when creating a new file with
 * llapi_layout_file_create().
 */
struct llapi_layout *llapi_layout_alloc(void);

/**
 * Free memory allocated for \a layout.
 */
void llapi_layout_free(struct llapi_layout *layout);

/** Not a valid stripe size, offset, or RAID pattern. */
#define LLAPI_LAYOUT_INVALID	0x1000000000000001ULL

/**
 * When specified or returned as the value for stripe count,
 * stripe size, offset, or RAID pattern, the filesystem-wide
 * default behavior will apply.
 */
#define LLAPI_LAYOUT_DEFAULT	(LLAPI_LAYOUT_INVALID + 1)

/**
 * When specified or returned as the value for stripe count, all
 * available OSTs will be used.
 */
#define LLAPI_LAYOUT_WIDE	(LLAPI_LAYOUT_INVALID + 2)

/**
 * When specified as the value for layout pattern, file objects will be
 * stored using RAID0.  That is, data will be split evenly and without
 * redundancy across all OSTs in the layout.
 */
#define LLAPI_LAYOUT_RAID0	0

/**
 * Flags to modify how layouts are retrieved.
 */
/******************** Stripe Count ********************/

/**
 * Store the stripe count of \a layout in \a count.
 *
 * \retval  0 Success
 * \retval -1 Error with status code in errno.
 */
int llapi_layout_stripe_count_get(const struct llapi_layout *layout,
				  uint64_t *count);

/**
 * Set the stripe count of \a layout to \a count.
 *
 * \retval  0 Success.
 * \retval -1 Invalid argument, errno set to EINVAL.
 */
int llapi_layout_stripe_count_set(struct llapi_layout *layout, uint64_t count);

/******************** Stripe Size ********************/

/**
 * Store the stripe size of \a layout in \a size.
 *
 * \retval  0 Success.
 * \retval -1 Invalid argument, errno set to EINVAL.
 */
int llapi_layout_stripe_size_get(const struct llapi_layout *layout,
				 uint64_t *size);

/**
 * Set the stripe size of \a layout to \a stripe_size.
 *
 * \retval  0 Success.
 * \retval -1 Invalid argument, errno set to EINVAL.
 */
int llapi_layout_stripe_size_set(struct llapi_layout *layout, uint64_t size);

/******************** Stripe Pattern ********************/

/**
 * Store the stripe pattern of \a layout in \a pattern.
 *
 * \retval 0  Success.
 * \retval -1 Error with status code in errno.
 */
int llapi_layout_pattern_get(const struct llapi_layout *layout,
			     uint64_t *pattern);

/**
 * Set the stripe pattern of \a layout to \a pattern.
 *
 * \retval  0 Success.
 * \retval -1 Invalid argument, errno set to EINVAL.
 */
int llapi_layout_pattern_set(struct llapi_layout *layout, uint64_t pattern);

/******************** OST Index ********************/

/**
 * Store the index of the OST where stripe number \a stripe_number is stored
 * in \a index.
 *
 * An error return value will result from a NULL layout, if \a
 * stripe_number is out of range, or if \a layout was not initialized
 * with llapi_layout_lookup_by{path,fd,fid}().
 *
 * \retval  0 Success
 * \retval -1 Invalid argument, errno set to EINVAL.
 */
int llapi_layout_ost_index_get(const struct llapi_layout *layout,
			       uint64_t stripe_number, uint64_t *index);

/**
 * Set the OST index associated with stripe number \a stripe_number to
 * \a ost_index.
 * NB: This is currently supported only for \a stripe_number = 0 and
 * other usage will return ENOTSUPP in errno.  A NULL \a layout or
 * out-of-range \a stripe_number will return EINVAL in errno.
 *
 * \retval  0 Success.
 * \retval -1 Error with errno set to non-zero value.
 */
int llapi_layout_ost_index_set(struct llapi_layout *layout, int stripe_number,
			       uint64_t index);

/******************** Pool Name ********************/

/**
 * Store up to \a pool_name_len characters of the name of the pool of
 * OSTs associated with \a layout into the buffer pointed to by
 * \a pool_name.
 *
 * The correct calling form is:
 *
 *   llapi_layout_pool_name_get(layout, pool_name, sizeof(pool_name));
 *
 * A pool defines a set of OSTs from which file objects may be
 * allocated for a file using \a layout.
 *
 * On success, the number of bytes stored is returned, excluding the
 * terminating '\0' character (zero indicates that \a layout does not
 * have an associated OST pool).  On error, -1 is returned and errno is
 * set appropriately. Possible sources of error include a NULL pointer
 * argument or insufficient space in \a dest to store the pool name,
 * in which cases errno will be set to EINVAL.
 *
 * \retval 0+		The number of bytes stored in \a dest.
 * \retval -1		Invalid argument, errno set to EINVAL.
 */
int llapi_layout_pool_name_get(const struct llapi_layout *layout,
			      char *pool_name, size_t pool_name_len);

/**
 * Set the name of the pool of OSTs from which file objects will be
 * allocated to \a pool_name.
 *
 * If the pool name uses "fsname.pool" notation to qualify the pool name
 * with a filesystem name, the "fsname." portion will be silently
 * discarded before storing the value. No validation that \a pool_name
 * is an existing non-empty pool in filesystem \a fsname will be
 * performed.  Such validation can be performed by the application if
 * desired using the llapi_search_ost() function.  The maximum length of
 * the stored value is defined by the constant LOV_MAXPOOLNAME.
 *
 * \retval  0	Success.
 * \retval -1	Invalid argument, errno set to EINVAL.
 */
int llapi_layout_pool_name_set(struct llapi_layout *layout,
			      const char *pool_name);

/******************** File Creation ********************/

/**
 * Open an existing file at \a path, or create it with the specified
 * \a layout and \a mode.
 *
 * One access mode and zero or more file creation flags and file status
 * flags May be bitwise-or'd in \a open_flags (see open(2)).  Return an
 * open file descriptor for the file.  If \a layout is non-NULL and
 * \a path is not on a Lustre filesystem this function will fail and set
 * errno to ENOTTY.
 *
 * An already existing file may be opened with this function, but
 * \a layout and \a mode will not be applied to it.  Callers requiring a
 * guarantee that the opened file is created with the specified
 * \a layout and \a mode should use llapi_layout_file_create().
 *
 * A NULL \a layout may be specified, in which case the standard Lustre
 * behavior for assigning layouts to newly-created files will apply.
 *
 * \retval 0+ An open file descriptor.
 * \retval -1 Error with status code in errno.
 */
int llapi_layout_file_open(const char *path, int open_flags, mode_t mode,
			   const struct llapi_layout *layout);

/**
 * Create a new file at \a path with the specified \a layout and \a mode.
 *
 * One access mode and zero or more file creation flags and file status
 * flags May be bitwise-or'd in \a open_flags (see open(2)).  Return an
 * open file descriptor for the file.  If \a layout is non-NULL and
 * \a path is not on a Lustre filesystem this function will fail and set
 * errno to ENOTTY.
 *
 * The function call
 *
 *   llapi_layout_file_create(path, open_flags, mode, layout)
 *
 * shall be equivalent to:
 *
 *   llapi_layout_file_open(path, open_flags|O_CREAT|O_EXCL, mode, layout)
 *
 * It is an error if \a path specifies an existing file.
 *
 * A NULL \a layout may be specified, in which the standard Lustre
 * behavior for assigning layouts to newly-created files will apply.
 *
 * \retval 0+ An open file descriptor.
 * \retval -1 Error with status code in errno.
 */
int llapi_layout_file_create(const char *path, int open_flags, int mode,
			     const struct llapi_layout *layout);

/** @} llapi */

#endif
