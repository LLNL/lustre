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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 * Copyright (c) 2012, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#if HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#include "mount_utils.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <config.h>
#include <lustre_disk.h>
#include <lustre_ver.h>
#include <sys/stat.h>
#include <sys/utsname.h>

extern char *progname;
extern int verbose;

#define vprint(fmt, arg...) if (verbose > 0) printf(fmt, ##arg)
#define verrprint(fmt, arg...) if (verbose >= 0) fprintf(stderr, fmt, ##arg)

void fatal(void)
{
        verbose = 0;
        fprintf(stderr, "\n%s FATAL: ", progname);
}

int run_command(char *cmd, int cmdsz)
{
        char log[] = "/tmp/run_command_logXXXXXX";
        int fd = -1, rc;

        if ((cmdsz - strlen(cmd)) < 6) {
                fatal();
                fprintf(stderr, "Command buffer overflow: %.*s...\n",
                        cmdsz, cmd);
                return ENOMEM;
        }

        if (verbose > 1) {
                printf("cmd: %s\n", cmd);
        } else {
                if ((fd = mkstemp(log)) >= 0) {
                        close(fd);
                        strcat(cmd, " >");
                        strcat(cmd, log);
                }
        }
        strcat(cmd, " 2>&1");

        /* Can't use popen because we need the rv of the command */
        rc = system(cmd);
        if (rc && (fd >= 0)) {
                char buf[128];
                FILE *fp;
                fp = fopen(log, "r");
                if (fp) {
                        while (fgets(buf, sizeof(buf), fp) != NULL) {
                                printf("   %s", buf);
                        }
                        fclose(fp);
                }
        }
        if (fd >= 0)
                remove(log);
        return rc;
}

int add_param(char *buf, char *key, char *val)
{
	int end = sizeof(((struct lustre_disk_data *)0)->ldd_params);
	int start = strlen(buf);
	int keylen = 0;

	if (key)
		keylen = strlen(key);
	if (start + 1 + keylen + strlen(val) >= end) {
		fprintf(stderr, "%s: params are too long-\n%s %s%s\n",
			progname, buf, key ? key : "", val);
		return 1;
	}

	sprintf(buf + start, " %s%s", key ? key : "", val);
	return 0;
}

int get_param(char *buf, char *key, char **val)
{
	int i, key_len = strlen(key);
	char *ptr;

	ptr = strstr(buf, key);
	if (ptr) {
		*val = strdup(ptr + key_len);
		if (*val == NULL)
			return ENOMEM;

		for (i = 0; i < strlen(*val); i++)
			if (((*val)[i] == ' ') || ((*val)[i] == '\0'))
				break;

		(*val)[i] = '\0';
		return 0;
	}

	return ENOENT;
}

char *strscat(char *dst, char *src, int buflen)
{
	dst[buflen - 1] = 0;
	if (strlen(dst) + strlen(src) >= buflen) {
		fprintf(stderr, "string buffer overflow (max %d): '%s' + '%s'"
			"\n", buflen, dst, src);
		exit(EOVERFLOW);
	}
	return strcat(dst, src);
}

char *strscpy(char *dst, char *src, int buflen)
{
	dst[0] = 0;
	return strscat(dst, src, buflen);
}

int check_mtab_entry(char *spec1, char *spec2, char *mtpt, char *type)
{
	FILE *fp;
	struct mntent *mnt;

	fp = setmntent(MOUNTED, "r");
	if (fp == NULL)
		return 0;

	while ((mnt = getmntent(fp)) != NULL) {
		if ((strcmp(mnt->mnt_fsname, spec1) == 0 ||
		     strcmp(mnt->mnt_fsname, spec2) == 0) &&
		    (mtpt == NULL || strcmp(mnt->mnt_dir, mtpt) == 0) &&
		    (type == NULL || strcmp(mnt->mnt_type, type) == 0)) {
			endmntent(fp);
			return(EEXIST);
		}
	}
	endmntent(fp);

	return 0;
}

#define PROC_DIR	"/proc/"
static int mtab_is_proc(const char *mtab)
{
	char path[16];

	if (readlink(mtab, path, sizeof(path)) < 0)
		return 0;

	if (strncmp(path, PROC_DIR, strlen(PROC_DIR)))
		return 0;

	return 1;
}

int update_mtab_entry(char *spec, char *mtpt, char *type, char *opts,
		int flags, int freq, int pass)
{
	FILE *fp;
	struct mntent mnt;
	int rc = 0;

	/* Don't update mtab if it is linked to any file in /proc direcotry.*/
	if (mtab_is_proc(MOUNTED))
		return 0;

	mnt.mnt_fsname = spec;
	mnt.mnt_dir = mtpt;
	mnt.mnt_type = type;
	mnt.mnt_opts = opts ? opts : "";
	mnt.mnt_freq = freq;
	mnt.mnt_passno = pass;

	fp = setmntent(MOUNTED, "a+");
	if (fp == NULL) {
		fprintf(stderr, "%s: setmntent(%s): %s:",
			progname, MOUNTED, strerror (errno));
		rc = 16;
	} else {
		if ((addmntent(fp, &mnt)) == 1) {
			fprintf(stderr, "%s: addmntent: %s:",
				progname, strerror (errno));
			rc = 16;
		}
		endmntent(fp);
	}

	return rc;
}

/* Search for opt in mntlist, returning true if found.
 */
static int in_mntlist(char *opt, char *mntlist)
{
	char *ml, *mlp, *item, *ctx = NULL;

	if (!(ml = strdup(mntlist))) {
		fprintf(stderr, "%s: out of memory\n", progname);
		exit(1);
	}
	mlp = ml;
	while ((item = strtok_r(mlp, ",", &ctx))) {
		if (!strcmp(opt, item))
			break;
		mlp = NULL;
	}
	free(ml);
	return (item != NULL);
}

/* Issue a message on stderr for every item in wanted_mountopts that is not
 * present in mountopts.  The justwarn boolean toggles between error and
 * warning message.  Return an error count.
 */
int check_mountfsoptions(char *mountopts, char *wanted_mountopts,
			 int justwarn)
{
	char *ml, *mlp, *item, *ctx = NULL;
	int errors = 0;

	if (!(ml = strdup(wanted_mountopts))) {
		fprintf(stderr, "%s: out of memory\n", progname);
		exit(1);
	}
	mlp = ml;
	while ((item = strtok_r(mlp, ",", &ctx))) {
		if (!in_mntlist(item, mountopts)) {
			fprintf(stderr, "%s: %s mount option `%s' is missing\n",
				progname, justwarn ? "Warning: default"
				: "Error: mandatory", item);
			errors++;
		}
		mlp = NULL;
	}
	free(ml);
	return errors;
}

/* Trim embedded white space, leading and trailing commas from string s.
 */
void trim_mountfsoptions(char *s)
{
	char *p;

	for (p = s; *p; ) {
		if (isspace(*p)) {
			memmove(p, p + 1, strlen(p + 1) + 1);
			continue;
		}
		p++;
	}

	while (s[0] == ',')
		memmove(&s[0], &s[1], strlen(&s[1]) + 1);

	p = s + strlen(s) - 1;
	while (p >= s && *p == ',')
		*p-- = '\0';
}

/* Setup a file in the first unused loop_device */
int loop_setup(struct mkfs_opts *mop)
{
	char loop_base[20];
	char l_device[64];
	int i, ret = 0;

	/* Figure out the loop device names */
	if (!access("/dev/loop0", F_OK | R_OK)) {
		strcpy(loop_base, "/dev/loop\0");
	} else if (!access("/dev/loop/0", F_OK | R_OK)) {
		strcpy(loop_base, "/dev/loop/\0");
	} else {
		fprintf(stderr, "%s: can't access loop devices\n", progname);
		return EACCES;
	}

	/* Find unused loop device */
	for (i = 0; i < MAX_LOOP_DEVICES; i++) {
		char cmd[PATH_MAX];
		int cmdsz = sizeof(cmd);

		sprintf(l_device, "%s%d", loop_base, i);
		if (access(l_device, F_OK | R_OK))
			break;
		snprintf(cmd, cmdsz, "losetup %s > /dev/null 2>&1", l_device);
		ret = system(cmd);

		/* losetup gets 1 (ret=256) for non-set-up device */
		if (ret) {
			/* Set up a loopback device to our file */
			snprintf(cmd, cmdsz, "losetup %s %s", l_device,
				 mop->mo_device);
			ret = run_command(cmd, cmdsz);
			if (ret == 256)
				/* someone else picked up this loop device
				 * behind our back */
				continue;
			if (ret) {
				fprintf(stderr, "%s: error %d on losetup: %s\n",
					progname, ret,
					ret >= 0 ? strerror(ret) : "");
				return ret;
			}
			strscpy(mop->mo_loopdev, l_device,
				sizeof(mop->mo_loopdev));
			return ret;
		}
	}

	fprintf(stderr, "%s: out of loop devices!\n", progname);
	return EMFILE;
}

int loop_cleanup(struct mkfs_opts *mop)
{
	char cmd[150];
	int ret = 1;
	if ((mop->mo_flags & MO_IS_LOOP) && *mop->mo_loopdev) {
		sprintf(cmd, "losetup -d %s", mop->mo_loopdev);
		ret = run_command(cmd, sizeof(cmd));
	}
	return ret;
}

int loop_format(struct mkfs_opts *mop)
{
	int fd;

	if (mop->mo_device_sz == 0) {
		fatal();
		fprintf(stderr, "loop device requires a --device-size= "
			"param\n");
		return EINVAL;
	}

	fd = creat(mop->mo_device, S_IRUSR|S_IWUSR);
	if (fd < 0) {
		fatal();
		fprintf(stderr, "%s: Unable to create backing store: %s\n",
			progname, strerror(errno));
		return errno;
	}

	if (ftruncate(fd, mop->mo_device_sz * 1024) != 0) {
		close(fd);
		fatal();
		fprintf(stderr, "%s: Unable to truncate backing store: %s\n",
			progname, strerror(errno));
		return errno;
	}

	close(fd);
	return 0;
}

/* Write the server config files */
int osd_write_ldd(struct mkfs_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	switch (ldd->ldd_mount_type) {
#ifdef HAVE_LDISKFS_OSD
	case LDD_MT_LDISKFS:
	case LDD_MT_LDISKFS2:
		ret = ldiskfs_write_ldd(mop);
		break;
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	case LDD_MT_ZFS:
		ret = zfs_write_ldd(mop);
		break;
#endif /* HAVE_ZFS_OSD */
	default:
		fatal();
		fprintf(stderr, "unknown fs type %d '%s'\n",
			ldd->ldd_mount_type, MT_STR(ldd));
		ret = EINVAL;
		break;
	}

	return ret;
}

/* Read the server config files */
int osd_read_ldd(char *dev, struct lustre_disk_data *ldd)
{
	int ret;

	switch (ldd->ldd_mount_type) {
#ifdef HAVE_LDISKFS_OSD
	case LDD_MT_LDISKFS:
	case LDD_MT_LDISKFS2:
		ret = ldiskfs_read_ldd(dev, ldd);
		break;
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	case LDD_MT_ZFS:
		ret = zfs_read_ldd(dev, ldd);
		break;
#endif /* HAVE_ZFS_OSD */
	default:
		fatal();
		fprintf(stderr, "unknown fs type %d '%s'\n",
			ldd->ldd_mount_type, MT_STR(ldd));
		ret = EINVAL;
		break;
	}

	return ret;
}

/* Was this device formatted for Lustre */
int osd_is_lustre(char *dev, unsigned *mount_type)
{
	vprint("checking for existing Lustre data: ");

#ifdef HAVE_LDISKFS_OSD
	if (ldiskfs_is_lustre(dev, mount_type)) {
		vprint("found\n");
		return 1;
	}
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	if (zfs_is_lustre(dev, mount_type)) {
		vprint("found\n");
		return 1;
	}
#endif /* HAVE_ZFS_OSD */

	vprint("not found\n");
	return 0;
}

/* Build fs according to type */
int osd_make_lustre(struct mkfs_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	switch (ldd->ldd_mount_type) {
#ifdef HAVE_LDISKFS_OSD
	case LDD_MT_LDISKFS:
	case LDD_MT_LDISKFS2:
		ret = ldiskfs_make_lustre(mop);
		break;
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	case LDD_MT_ZFS:
		ret = zfs_make_lustre(mop);
		break;
#endif /* HAVE_ZFS_OSD */
	default:
		fatal();
		fprintf(stderr, "unknown fs type %d '%s'\n",
			ldd->ldd_mount_type, MT_STR(ldd));
		ret = EINVAL;
		break;
	}

	return ret;
}

int osd_prepare_lustre(struct mkfs_opts *mop,
		char *default_mountopts, int default_len,
		char *always_mountopts, int always_len)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	switch (ldd->ldd_mount_type) {
#ifdef HAVE_LDISKFS_OSD
	case LDD_MT_LDISKFS:
	case LDD_MT_LDISKFS2:
		ret = ldiskfs_prepare_lustre(mop,
					     default_mountopts, default_len,
					     always_mountopts, always_len);
		break;
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	case LDD_MT_ZFS:
		ret = zfs_prepare_lustre(mop,
					 default_mountopts, default_len,
					 always_mountopts, always_len);
		break;
#endif /* HAVE_ZFS_OSD */
	default:
		fatal();
		fprintf(stderr, "unknown fs type %d '%s'\n",
			ldd->ldd_mount_type, MT_STR(ldd));
		ret = EINVAL;
		break;
	}

	return ret;
}

int osd_tune_lustre(char *dev, struct mount_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	switch (ldd->ldd_mount_type) {
#ifdef HAVE_LDISKFS_OSD
	case LDD_MT_LDISKFS:
	case LDD_MT_LDISKFS2:
		ret = ldiskfs_tune_lustre(dev, mop);
		break;
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	case LDD_MT_ZFS:
		ret = zfs_tune_lustre(dev, mop);
		break;
#endif /* HAVE_ZFS_OSD */
	default:
		fatal();
		fprintf(stderr, "unknown fs type %d '%s'\n",
				ldd->ldd_mount_type, MT_STR(ldd));
		ret = EINVAL;
		break;
	}

	return ret;
}

int osd_label_lustre(struct mount_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	switch (ldd->ldd_mount_type) {
#ifdef HAVE_LDISKFS_OSD
	case LDD_MT_LDISKFS:
	case LDD_MT_LDISKFS2:
		ret = ldiskfs_label_lustre(mop);
		break;
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	case LDD_MT_ZFS:
		ret = zfs_label_lustre(mop);
		break;
#endif /* HAVE_ZFS_OSD */
	default:
		fatal();
		fprintf(stderr, "unknown fs type %d '%s'\n",
			ldd->ldd_mount_type, MT_STR(ldd));
		ret = EINVAL;
		break;
	}

	return ret;
}

/* Enable quota accounting */
int osd_enable_quota(struct mkfs_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	switch (ldd->ldd_mount_type) {
#ifdef HAVE_LDISKFS_OSD
	case LDD_MT_EXT3:
	case LDD_MT_LDISKFS:
	case LDD_MT_LDISKFS2:
		ret = ldiskfs_enable_quota(mop);
		break;
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	case LDD_MT_ZFS:
		fprintf(stderr, "this option is only valid for ldiskfs\n");
		ret = EINVAL;
		break;
#endif /* HAVE_ZFS_OSD */
	default:
		fatal();
		fprintf(stderr, "unknown fs type %d '%s'\n",
			ldd->ldd_mount_type, MT_STR(ldd));
		ret = EINVAL;
		break;
	}

	return ret;
}

int osd_init(void)
{
	int ret = 0;

#ifdef HAVE_LDISKFS_OSD
	ret = ldiskfs_init();
	if (ret)
		return ret;
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	ret = zfs_init();
	/* we want to be able to set up a ldiskfs-based filesystem w/o
	 * the ZFS modules installed, see ORI-425 */
	if (ret)
		ret = 0;
#endif /* HAVE_ZFS_OSD */

	return ret;
}

void osd_fini(void)
{
#ifdef HAVE_LDISKFS_OSD
	ldiskfs_fini();
#endif /* HAVE_LDISKFS_OSD */
#ifdef HAVE_ZFS_OSD
	zfs_fini();
#endif /* HAVE_ZFS_OSD */
}

__u64 get_device_size(char* device)
{
	int ret, fd;
	__u64 size = 0;

	fd = open(device, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n",
			progname, device, strerror(errno));
		return 0;
	}

#ifdef BLKGETSIZE64
	/* size in bytes. bz5831 */
	ret = ioctl(fd, BLKGETSIZE64, (void*)&size);
#else
	{
		__u32 lsize = 0;
		/* size in blocks */
		ret = ioctl(fd, BLKGETSIZE, (void*)&lsize);
		size = (__u64)lsize * 512;
	}
#endif
	close(fd);
	if (ret < 0) {
		fprintf(stderr, "%s: size ioctl failed: %s\n",
			progname, strerror(errno));
		return 0;
	}

	vprint("device size = "LPU64"MB\n", size >> 20);
	/* return value in KB */
	return size >> 10;
}

int file_create(char *path, __u64 size)
{
	__u64 size_max;
	int ret;
	int fd;

	/*
	 * Since "size" is in KB, the file offset it represents could overflow
	 * off_t.
	 */
	size_max = (off_t)1 << (_FILE_OFFSET_BITS - 1 - 10);
	if (size >= size_max) {
		fprintf(stderr, "%s: "LPU64" KB: Backing store size must be "
			"smaller than "LPU64" KB\n", progname, size, size_max);
		return EFBIG;
	}

	ret = access(path, F_OK);
	if (ret == 0) {
		ret = unlink(path);
		if (ret != 0)
			return errno;
	}

	fd = creat(path, S_IRUSR|S_IWUSR);
	if (fd < 0) {
		fatal();
		fprintf(stderr, "%s: Unable to create backing store: %s\n",
			progname, strerror(errno));
		return errno;
	}

	ret = ftruncate(fd, size * 1024);
	close(fd);
	if (ret != 0) {
		fatal();
		fprintf(stderr, "%s: Unable to truncate backing store: %s\n",
			progname, strerror(errno));
		return errno;
	}

	return 0;
}
