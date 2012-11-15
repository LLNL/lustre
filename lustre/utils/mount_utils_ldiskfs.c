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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/utils/mount_utils_ldiskfs.c
 *
 * Author: Nathan Rutman <nathan@clusterfs.com>
*/

/* This source file is compiled into both mkfs.lustre and tunefs.lustre */

#if HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "mount_utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <mntent.h>
#include <glob.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/utsname.h>

#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <ctype.h>

#ifdef __linux__
/* libcfs.h is not really needed here, but on SLES10/PPC, fs.h includes idr.h
 * which requires BITS_PER_LONG to be defined */
#include <libcfs/libcfs.h>
#ifndef BLKGETSIZE64
#include <linux/fs.h> /* for BLKGETSIZE64 */
#endif
#include <linux/version.h>
#endif
#include <lustre_disk.h>
#include <lustre_param.h>
#include <lnet/lnetctl.h>
#include <lustre_ver.h>

#define MAX_HW_SECTORS_KB_PATH	"queue/max_hw_sectors_kb"
#define MAX_SECTORS_KB_PATH	"queue/max_sectors_kb"
#define STRIPE_CACHE_SIZE	"md/stripe_cache_size"

extern char *progname;

#define L_BLOCK_SIZE 4096
/* keep it less than LL_FID_NAMELEN */
#define DUMMY_FILE_NAME_LEN             25
#define EXT3_DIRENT_SIZE                DUMMY_FILE_NAME_LEN

/* Write the server config files */
int ldiskfs_write_ldd(struct mkfs_opts *mop)
{
	char mntpt[] = "/tmp/mntXXXXXX";
	char filepnm[128];
	char *dev;
	FILE *filep;
	int ret = 0;
	size_t num;

	/* Mount this device temporarily in order to write these files */
	if (!mkdtemp(mntpt)) {
		fprintf(stderr, "%s: Can't create temp mount point %s: %s\n",
			progname, mntpt, strerror(errno));
		return errno;
	}

	dev = mop->mo_device;
	if (mop->mo_flags & MO_IS_LOOP)
		dev = mop->mo_loopdev;

	ret = mount(dev, mntpt, MT_STR(&mop->mo_ldd), 0,
		    mop->mo_ldd.ldd_mount_opts);
	if (ret) {
		fprintf(stderr, "%s: Unable to mount %s: %s\n",
			progname, dev, strerror(errno));
		ret = errno;
		if (errno == ENODEV) {
			fprintf(stderr, "Is the %s module available?\n",
				MT_STR(&mop->mo_ldd));
		}
		goto out_rmdir;
	}

	/* Set up initial directories */
	sprintf(filepnm, "%s/%s", mntpt, MOUNT_CONFIGS_DIR);
	ret = mkdir(filepnm, 0777);
	if ((ret != 0) && (errno != EEXIST)) {
		fprintf(stderr, "%s: Can't make configs dir %s (%s)\n",
			progname, filepnm, strerror(errno));
		goto out_umnt;
	} else if (errno == EEXIST) {
		ret = 0;
	}

	/* Save the persistent mount data into a file. Lustre must pre-read
	   this file to get the real mount options. */
	vprint("Writing %s\n", MOUNT_DATA_FILE);
	sprintf(filepnm, "%s/%s", mntpt, MOUNT_DATA_FILE);
	filep = fopen(filepnm, "w");
	if (!filep) {
		fprintf(stderr, "%s: Unable to create %s file: %s\n",
			progname, filepnm, strerror(errno));
		goto out_umnt;
	}
	num = fwrite(&mop->mo_ldd, sizeof(mop->mo_ldd), 1, filep);
	if (num < 1 && ferror(filep)) {
		fprintf(stderr, "%s: Unable to write to file (%s): %s\n",
			progname, filepnm, strerror(errno));
		fclose(filep);
		goto out_umnt;
	}
	fclose(filep);

out_umnt:
	umount(mntpt);
out_rmdir:
	rmdir(mntpt);
	return ret;
}

static int readcmd(char *cmd, char *buf, int len)
{
	FILE *fp;
	int red;

	fp = popen(cmd, "r");
	if (!fp)
		return errno;

	red = fread(buf, 1, len, fp);
	pclose(fp);

	/* strip trailing newline */
	if (buf[red - 1] == '\n')
		buf[red - 1] = '\0';

	return (red == 0) ? -ENOENT : 0;
}

int ldiskfs_read_ldd(char *dev, struct lustre_disk_data *mo_ldd)
{
	char tmpdir[] = "/tmp/dirXXXXXX";
	char cmd[PATH_MAX];
	char filepnm[128];
	FILE *filep;
	int ret = 0;
	int cmdsz = sizeof(cmd);

	/* Make a temporary directory to hold Lustre data files. */
	if (!mkdtemp(tmpdir)) {
		fprintf(stderr, "%s: Can't create temporary directory %s: %s\n",
			progname, tmpdir, strerror(errno));
		return errno;
	}

	/* TODO: it's worth observing the get_mountdata() function that is
	   in mount_utils.c for getting the mountdata out of the
	   filesystem */

	/* Construct debugfs command line. */
	snprintf(cmd, cmdsz, "%s -c -R 'dump /%s %s/mountdata' '%s'",
		 DEBUGFS, MOUNT_DATA_FILE, tmpdir, dev);

	ret = run_command(cmd, cmdsz);
	if (ret)
		verrprint("%s: Unable to dump %s dir (%d)\n",
			  progname, MOUNT_CONFIGS_DIR, ret);

	sprintf(filepnm, "%s/mountdata", tmpdir);
	filep = fopen(filepnm, "r");
	if (filep) {
		size_t num_read;
		vprint("Reading %s\n", MOUNT_DATA_FILE);
		num_read = fread(mo_ldd, sizeof(*mo_ldd), 1, filep);
		if (num_read < 1 && ferror(filep)) {
			fprintf(stderr, "%s: Unable to read from file %s: %s\n",
				progname, filepnm, strerror(errno));
		}
		fclose(filep);
	}

	snprintf(cmd, cmdsz, "rm -rf %s", tmpdir);
	run_command(cmd, cmdsz);
	if (ret)
		verrprint("Failed to read old data (%d)\n", ret);

	/* As long as we at least have the label, we're good to go */
	snprintf(cmd, sizeof(cmd), E2LABEL" %s", dev);
	ret = readcmd(cmd, mo_ldd->ldd_svname, sizeof(mo_ldd->ldd_svname) - 1);

	return ret;
}


/* Display the need for the latest e2fsprogs to be installed. make_backfs
 * indicates if the caller is make_lustre_backfs() or not. */
void disp_old_e2fsprogs_msg(const char *feature, int make_backfs)
{
	static int msg_displayed;

	if (msg_displayed) {
		fprintf(stderr, "WARNING: %s does not support %s "
			"feature.\n\n", E2FSPROGS, feature);
		return;
	}

	msg_displayed++;

	fprintf(stderr, "WARNING: The %s package currently installed on "
		"your system does not support \"%s\" feature.\n",
		E2FSPROGS, feature);
#if !(HAVE_LDISKFSPROGS)
	fprintf(stderr, "Please install the latest version of e2fsprogs from\n"
		"http://downloads.whamcloud.com/public/e2fsprogs/latest/\n"
		"to enable this feature.\n");
#endif
	if (make_backfs)
		fprintf(stderr, "Feature will not be enabled until %s"
			"is updated and '%s -O %s %%{device}' "
			"is run.\n\n", E2FSPROGS, TUNE2FS, feature);
}

/* Check whether the file exists in the device */
static int file_in_dev(char *file_name, char *dev_name)
{
	FILE *fp;
	char debugfs_cmd[256];
	unsigned int inode_num;
	int i;

	/* Construct debugfs command line. */
	snprintf(debugfs_cmd, sizeof(debugfs_cmd),
		 "%s -c -R 'stat %s' '%s' 2>&1 | egrep '(Inode|unsupported)'",
		 DEBUGFS, file_name, dev_name);

	fp = popen(debugfs_cmd, "r");
	if (!fp) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return 0;
	}

	if (fscanf(fp, "Inode: %u", &inode_num) == 1) { /* exist */
		pclose(fp);
		return 1;
	}
	i = fread(debugfs_cmd, 1, sizeof(debugfs_cmd), fp);
	if (i) {
		debugfs_cmd[i] = 0;
		fprintf(stderr, "%s", debugfs_cmd);
		if (strstr(debugfs_cmd, "unsupported feature")) {
			disp_old_e2fsprogs_msg("an unknown", 0);
		}
		pclose(fp);
		return -1;
	}
	pclose(fp);
	return 0;
}

/* Check whether the device has already been used with lustre */
int ldiskfs_is_lustre(char *dev, unsigned *mount_type)
{
	int ret;

	ret = file_in_dev(MOUNT_DATA_FILE, dev);
	if (ret) {
		/* in the -1 case, 'extents' means IS a lustre target */
		*mount_type = LDD_MT_LDISKFS;
		return 1;
	}

	ret = file_in_dev(LAST_RCVD, dev);
	if (ret) {
		*mount_type = LDD_MT_LDISKFS;
		return 1;
	}

	return 0;
}

/* Check if a certain feature is supported by e2fsprogs.
 * Firstly we try to use "debugfs supported_features" command to check if
 * the feature is supported. If this fails we try to set this feature with
 * mke2fs to check for its support. */
static int is_e2fsprogs_feature_supp(const char *feature)
{
	static char supp_features[4096] = "";
	FILE *fp;
	char cmd[PATH_MAX];
	char imgname[] = "/tmp/test-img-XXXXXX";
	int fd = -1;
	int ret = 1;

	if (supp_features[0] == '\0') {
		snprintf(cmd, sizeof(cmd), "%s -c -R supported_features 2>&1",
			 DEBUGFS);

		/* Using popen() instead of run_command() since debugfs does
		 * not return proper error code if command is not supported */
		fp = popen(cmd, "r");
		if (!fp) {
			fprintf(stderr, "%s: %s\n", progname, strerror(errno));
			return 0;
		}
		ret = fread(supp_features, 1, sizeof(supp_features), fp);
		fclose(fp);
	}
	if (ret > 0 && strstr(supp_features,
			      strncmp(feature, "-O ", 3) ? feature : feature+3))
		return 0;

	if ((fd = mkstemp(imgname)) < 0)
		return -1;
	else
		close(fd);

	snprintf(cmd, sizeof(cmd), "%s -F %s %s 100 >/dev/null 2>&1",
		 MKE2FS, feature, imgname);
	/* run_command() displays the output of mke2fs when it fails for
	 * some feature, so use system() directly */
	ret = system(cmd);
	unlink(imgname);

	return ret;
}


/**
 * append_unique: append @key or @key=@val pair to @buf only if @key does not
 *                exists
 *      @buf: buffer to hold @key or @key=@val
 *      @prefix: prefix string before @key
 *      @key: key string
 *      @val: value string if it's a @key=@val pair
 */
static void append_unique(char *buf, char *prefix, char *key, char *val,
			  size_t maxbuflen)
{
	char *anchor, *end;
	int  len;

	if (key == NULL)
		return;

	anchor = end = strstr(buf, key);
	/* try to find exact match string in @buf */
	while (end && *end != '\0' && *end != ',' && *end != ' ' && *end != '=')
		++end;
	len = end - anchor;
	if (anchor == NULL || strlen(key) != len ||
			strncmp(anchor, key, len) != 0) {
		if (prefix != NULL)
			strscat(buf, prefix, maxbuflen);

		strscat(buf, key, maxbuflen);
		if (val != NULL) {
			strscat(buf, "=", maxbuflen);
			strscat(buf, val, maxbuflen);
		}
	}
}

static int enable_default_ext4_features(struct mkfs_opts *mop, char *anchor,
					size_t maxbuflen, int user_spec)
{
	if (IS_OST(&mop->mo_ldd)) {
		append_unique(anchor, user_spec ? "," : " -O ",
			      "extents", NULL, sizeof(mop->mo_mkfsopts));
		append_unique(anchor, ",", "uninit_bg", NULL, maxbuflen);
	} else if (IS_MDT(&mop->mo_ldd)) {
		append_unique(anchor, user_spec ? "," : " -O ",
			      "dirdata", NULL, maxbuflen);
		append_unique(anchor, ",", "uninit_bg", NULL, maxbuflen);
		append_unique(anchor, ",", "^extents", NULL, maxbuflen);
	} else {
		append_unique(anchor, user_spec ? "," : " -O ",
			      "uninit_bg", NULL, maxbuflen);
	}

	/* Multiple mount protection enabled only if failover node specified */
	if (mop->mo_flags & MO_FAILOVER) {
		if (is_e2fsprogs_feature_supp("-O mmp") == 0)
			append_unique(anchor, ",", "mmp", NULL, maxbuflen);
		else
			disp_old_e2fsprogs_msg("mmp", 1);
	}

	/* Allow more than 65000 subdirectories */
	if (is_e2fsprogs_feature_supp("-O dir_nlink") == 0)
		append_unique(anchor, ",", "dir_nlink", NULL, maxbuflen);

	/* The following options are only valid for ext4-based ldiskfs.
	 * If --backfstype=ext3 is specified, do not enable them. */
	if (mop->mo_ldd.ldd_mount_type == LDD_MT_EXT3)
		return 0;

	/* Enable quota by default */
	if (is_e2fsprogs_feature_supp("-O quota") == 0) {
		append_unique(anchor, ",", "quota", NULL, maxbuflen);
	} else {
		fatal();
		fprintf(stderr, "\"-O quota\" must be supported by "
			"e2fsprogs, please upgrade your e2fsprogs.\n");
		return EINVAL;
	}

	/* Allow files larger than 2TB.  Also needs LU-16, but not harmful. */
	if (is_e2fsprogs_feature_supp("-O huge_file") == 0)
		append_unique(anchor, ",", "huge_file", NULL, maxbuflen);

	/* Enable large block addresses if the LUN is over 2^32 blocks. */
	if (mop->mo_device_sz / (L_BLOCK_SIZE >> 10) >= 0x100002000ULL &&
	    is_e2fsprogs_feature_supp("-O 64bit") == 0)
		append_unique(anchor, ",", "64bit", NULL, maxbuflen);

	/* Cluster inode/block bitmaps and inode table for more efficient IO.
	 * Align the flex groups on a 1MB boundary for better performance. */
	/* This -O feature needs to go last, since it adds the "-G" option. */
	if (is_e2fsprogs_feature_supp("-O flex_bg") == 0) {
		char tmp_buf[64];

		append_unique(anchor, ",", "flex_bg", NULL, maxbuflen);

		if (IS_OST(&mop->mo_ldd)) {
			snprintf(tmp_buf, sizeof(tmp_buf), " -G %u",
				 (1 << 20) / L_BLOCK_SIZE);
			strscat(anchor, tmp_buf, maxbuflen);
		}
	}
	/* Don't add any more "-O" options here, see last comment above */
	return 0;
}

/**
 * moveopts_to_end: find the option string, move remaining strings to
 *                  where option string starts, and append the option
 *                  string at the end
 *      @start: where the option string starts before the move
 *      RETURN: where the option string starts after the move
 */
static char *moveopts_to_end(char *start)
{
	char save[512];
	char *end, *idx;

	/* skip whitespace before options */
	end = start + 2;
	while (*end == ' ')
		++end;

	/* find end of option characters */
	while (*end != ' ' && *end != '\0')
		++end;

	/* save options */
	strncpy(save, start, end - start);
	save[end - start] = '\0';

	/* move remaining options up front */
	if (*end)
		memmove(start, end, strlen(end));
	*(start + strlen(end)) = '\0';

	/* append the specified options */
	if (*(start + strlen(start) - 1) != ' ')
		strcat(start, " ");
	idx = start + strlen(start);
	strcat(start, save);

	return idx;
}

/* Build fs according to type */
int ldiskfs_make_lustre(struct mkfs_opts *mop)
{
	__u64 device_sz = mop->mo_device_sz, block_count = 0;
	char mkfs_cmd[PATH_MAX];
	char buf[64];
	char *start;
	char *dev;
	int ret = 0, ext_opts = 0;
	size_t maxbuflen;

	if (!(mop->mo_flags & MO_IS_LOOP)) {
		mop->mo_device_sz = get_device_size(mop->mo_device);

		if (mop->mo_device_sz == 0)
			return ENODEV;

		/* Compare to real size */
		if (device_sz == 0 || device_sz > mop->mo_device_sz)
			device_sz = mop->mo_device_sz;
		else
			mop->mo_device_sz = device_sz;
	}

	if (mop->mo_device_sz != 0) {
		if (mop->mo_device_sz < 8096){
			fprintf(stderr, "%s: size of filesystem must be larger "
				"than 8MB, but is set to %lldKB\n",
				progname, (long long)mop->mo_device_sz);
			return EINVAL;
		}
		block_count = mop->mo_device_sz / (L_BLOCK_SIZE >> 10);
		/* If the LUN size is just over 2^32 blocks, limit the
		 * filesystem size to 2^32-1 blocks to avoid problems with
		 * ldiskfs/mkfs not handling this size.  Bug 22906 */
		if (block_count > 0xffffffffULL && block_count < 0x100002000ULL)
			block_count = 0xffffffffULL;
	}

	if ((mop->mo_ldd.ldd_mount_type == LDD_MT_EXT3) ||
	    (mop->mo_ldd.ldd_mount_type == LDD_MT_LDISKFS) ||
	    (mop->mo_ldd.ldd_mount_type == LDD_MT_LDISKFS2)) {
		long inode_size = 0;

		/* Journal size in MB */
		if (strstr(mop->mo_mkfsopts, "-J") == NULL) {
			/* Choose our own default journal size */
			long journal_sz = 0, max_sz;
			if (device_sz > 1024 * 1024) /* 1GB */
				journal_sz = (device_sz / 102400) * 4;
			/* cap journal size at 1GB */
			if (journal_sz > 1024L)
				journal_sz = 1024L;
			/* man mkfs.ext3 */
			max_sz = (102400 * L_BLOCK_SIZE) >> 20; /* 400MB */
			if (journal_sz > max_sz)
				journal_sz = max_sz;
			if (journal_sz) {
				sprintf(buf, " -J size=%ld", journal_sz);
				strscat(mop->mo_mkfsopts, buf,
					sizeof(mop->mo_mkfsopts));
			}
		}

		/* Inode size (for extended attributes).  The LOV EA size is
		 * 32 (EA hdr) + 32 (lov_mds_md) + stripes * 24 (lov_ost_data),
		 * and we want some margin above that for ACLs, other EAs... */
		if (strstr(mop->mo_mkfsopts, "-I") == NULL) {
			if (IS_MDT(&mop->mo_ldd)) {
				if (mop->mo_stripe_count > 72)
					inode_size = 512; /* bz 7241 */
				/* see also "-i" below for EA blocks */
				else if (mop->mo_stripe_count > 32)
					inode_size = 2048;
				else if (mop->mo_stripe_count > 10)
					inode_size = 1024;
				else
					inode_size = 512;
			} else if (IS_OST(&mop->mo_ldd)) {
				/* We store MDS FID and OST objid in EA on OST
				 * we need to make inode bigger as well. */
				inode_size = 256;
			}

			if (inode_size > 0) {
				sprintf(buf, " -I %ld", inode_size);
				strscat(mop->mo_mkfsopts, buf,
					sizeof(mop->mo_mkfsopts));
			}
		}

		/* Bytes_per_inode: disk size / num inodes */
		if (strstr(mop->mo_mkfsopts, "-i") == NULL &&
		    strstr(mop->mo_mkfsopts, "-N") == NULL) {
			long bytes_per_inode = 0;

			/* Allocate more inodes on MDT devices.  There is
			 * no data stored on the MDT, and very little extra
			 * metadata beyond the inode.  It could go down as
			 * low as 1024 bytes, but this is conservative.
			 * Account for external EA blocks for wide striping. */
			if (IS_MDT(&mop->mo_ldd)) {
				bytes_per_inode = inode_size + 1536;

				if (mop->mo_stripe_count > 72) {
					int extra = mop->mo_stripe_count * 24;
					extra = ((extra - 1) | 4095) + 1;
					bytes_per_inode += extra;
				}
			}

			/* Allocate fewer inodes on large OST devices.  Most
			 * filesystems can be much more aggressive than even
			 * this, but it is impossible to know in advance. */
			if (IS_OST(&mop->mo_ldd)) {
				/* OST > 16TB assume average file size 1MB */
				if (device_sz > (16ULL << 30))
					bytes_per_inode = 1024 * 1024;
				/* OST > 4TB assume average file size 512kB */
				else if (device_sz > (4ULL << 30))
					bytes_per_inode = 512 * 1024;
				/* OST > 1TB assume average file size 256kB */
				else if (device_sz > (1ULL << 30))
					bytes_per_inode = 256 * 1024;
				/* OST > 10GB assume average file size 64kB,
				 * plus a bit so that inodes will fit into a
				 * 256x flex_bg without overflowing */
				else if (device_sz > (10ULL << 20))
					bytes_per_inode = 69905;
			}

			if (bytes_per_inode > 0) {
				sprintf(buf, " -i %ld", bytes_per_inode);
				strscat(mop->mo_mkfsopts, buf,
					sizeof(mop->mo_mkfsopts));
			}
		}

		if (verbose < 2) {
			strscat(mop->mo_mkfsopts, " -q",
				sizeof(mop->mo_mkfsopts));
		}

		/* start handle -O mkfs options */
		if ((start = strstr(mop->mo_mkfsopts, "-O")) != NULL) {
			if (strstr(start + 2, "-O") != NULL) {
				fprintf(stderr,
					"%s: don't specify multiple -O options\n",
					progname);
				return EINVAL;
			}
			start = moveopts_to_end(start);
			maxbuflen = sizeof(mop->mo_mkfsopts) -
				(start - mop->mo_mkfsopts) - strlen(start);
			ret = enable_default_ext4_features(mop, start, maxbuflen, 1);
		} else {
			start = mop->mo_mkfsopts + strlen(mop->mo_mkfsopts),
			      maxbuflen = sizeof(mop->mo_mkfsopts) -
				      strlen(mop->mo_mkfsopts);
			ret = enable_default_ext4_features(mop, start, maxbuflen, 0);
		}
		if (ret)
			return ret;
		/* end handle -O mkfs options */

		/* start handle -E mkfs options */
		if ((start = strstr(mop->mo_mkfsopts, "-E")) != NULL) {
			if (strstr(start + 2, "-E") != NULL) {
				fprintf(stderr,
					"%s: don't specify multiple -E options\n",
					progname);
				return EINVAL;
			}
			start = moveopts_to_end(start);
			maxbuflen = sizeof(mop->mo_mkfsopts) -
				(start - mop->mo_mkfsopts) - strlen(start);
			ext_opts = 1;
		} else {
			start = mop->mo_mkfsopts + strlen(mop->mo_mkfsopts);
			maxbuflen = sizeof(mop->mo_mkfsopts) -
				strlen(mop->mo_mkfsopts);
		}

		/* In order to align the filesystem metadata on 1MB boundaries,
		 * give a resize value that will reserve a power-of-two group
		 * descriptor blocks, but leave one block for the superblock.
		 * Only useful for filesystems with < 2^32 blocks due to resize
		 * limitations. */
		if (IS_OST(&mop->mo_ldd) && mop->mo_device_sz > 100 * 1024 &&
		    mop->mo_device_sz * 1024 / L_BLOCK_SIZE <= 0xffffffffULL) {
			unsigned group_blocks = L_BLOCK_SIZE * 8;
			unsigned desc_per_block = L_BLOCK_SIZE / 32;
			unsigned resize_blks;

			resize_blks = (1ULL<<32) - desc_per_block*group_blocks;
			snprintf(buf, sizeof(buf), "%u", resize_blks);
			append_unique(start, ext_opts ? "," : " -E ",
				      "resize", buf, maxbuflen);
			ext_opts = 1;
		}

		/* Avoid zeroing out the full journal - speeds up mkfs */
		if (is_e2fsprogs_feature_supp("-E lazy_journal_init") == 0)
			append_unique(start, ext_opts ? "," : " -E ",
				      "lazy_journal_init", NULL, maxbuflen);
		/* end handle -E mkfs options */

		/* Allow reformat of full devices (as opposed to
		   partitions.)  We already checked for mounted dev. */
		strscat(mop->mo_mkfsopts, " -F", sizeof(mop->mo_mkfsopts));

		snprintf(mkfs_cmd, sizeof(mkfs_cmd),
			 "%s -j -b %d -L %s ", MKE2FS, L_BLOCK_SIZE,
			 mop->mo_ldd.ldd_svname);
	} else {
		fprintf(stderr,"%s: unsupported fs type: %d (%s)\n",
			progname, mop->mo_ldd.ldd_mount_type,
			MT_STR(&mop->mo_ldd));
		return EINVAL;
	}

	/* For loop device format the dev, not the filename */
	dev = mop->mo_device;
	if (mop->mo_flags & MO_IS_LOOP)
		dev = mop->mo_loopdev;

	vprint("formatting backing filesystem %s on %s\n",
	       MT_STR(&mop->mo_ldd), dev);
	vprint("\ttarget name  %s\n", mop->mo_ldd.ldd_svname);
	vprint("\t4k blocks     "LPU64"\n", block_count);
	vprint("\toptions       %s\n", mop->mo_mkfsopts);

	/* mkfs_cmd's trailing space is important! */
	strscat(mkfs_cmd, mop->mo_mkfsopts, sizeof(mkfs_cmd));
	strscat(mkfs_cmd, " ", sizeof(mkfs_cmd));
	strscat(mkfs_cmd, dev, sizeof(mkfs_cmd));
	if (block_count != 0) {
		sprintf(buf, " "LPU64, block_count);
		strscat(mkfs_cmd, buf, sizeof(mkfs_cmd));
	}

	vprint("mkfs_cmd = %s\n", mkfs_cmd);
	ret = run_command(mkfs_cmd, sizeof(mkfs_cmd));
	if (ret) {
		fatal();
		fprintf(stderr, "Unable to build fs %s (%d)\n", dev, ret);
	}
	return ret;
}

int ldiskfs_prepare_lustre(struct mkfs_opts *mop,
			   char *default_mountopts, int default_len,
			   char *always_mountopts, int always_len)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int ret;

	/* Set MO_IS_LOOP to indicate a loopback device is needed */
	ret = is_block(mop->mo_device);
	if (ret < 0) {
		return errno;
	} else if (ret == 0) {
		mop->mo_flags |= MO_IS_LOOP;
	}

	strscat(default_mountopts, ",errors=remount-ro", default_len);
	if (IS_MDT(ldd) || IS_MGS(ldd))
		strscat(always_mountopts, ",user_xattr", always_len);

	return 0;
}

int read_file(char *path, char *buf, int size)
{
	FILE *fd;

	fd = fopen(path, "r");
	if (fd == NULL)
		return errno;

	/* should not ignore fgets(3)'s return value */
	if (!fgets(buf, size, fd)) {
		fprintf(stderr, "reading from %s: %s", path, strerror(errno));
		fclose(fd);
		return 1;
	}
	fclose(fd);
	return 0;
}

int write_file(char *path, char *buf)
{
	FILE *fd;

	fd = fopen(path, "w");
	if (fd == NULL)
		return errno;

	fputs(buf, fd);
	fclose(fd);
	return 0;
}

/* This is to tune the kernel for good SCSI performance.
 * For that we set the value of /sys/block/{dev}/queue/max_sectors_kb
 * to the value of /sys/block/{dev}/queue/max_hw_sectors_kb */
int set_blockdev_tunables(char *source, struct mount_opts *mop, int fan_out)
{
	glob_t glob_info = { 0 };
	struct stat stat_buf;
	char *chk_major, *chk_minor;
	char *savept = NULL, *dev;
	char *ret_path;
	char buf[PATH_MAX] = {'\0'}, path[PATH_MAX] = {'\0'};
	char real_path[PATH_MAX] = {'\0'};
	int i, rc = 0;
	int major, minor;

	if (!source)
		return -EINVAL;

	ret_path = realpath(source, real_path);
	if (ret_path == NULL) {
		if (verbose)
			fprintf(stderr, "warning: %s: cannot resolve: %s\n",
				source, strerror(errno));
		return -EINVAL;
	}

	if (strncmp(real_path, "/dev/loop", 9) == 0)
		return 0;

	if ((real_path[0] != '/') && (strpbrk(real_path, ",:") != NULL))
		return 0;

	snprintf(path, sizeof(path), "/sys/block%s", real_path + 4);
	if (access(path, X_OK) == 0)
		goto set_params;

	/* The name of the device say 'X' specified in /dev/X may not
	 * match any entry under /sys/block/. In that case we need to
	 * match the major/minor number to find the entry under
	 * sys/block corresponding to /dev/X */

	/* Don't chop tail digit on /dev/mapper/xxx, LU-478 */
	if (strncmp(real_path, "/dev/mapper", 11) != 0) {
		dev = real_path + strlen(real_path);
		while (--dev > real_path && isdigit(*dev))
			*dev = 0;

		if (strncmp(real_path, "/dev/md_", 8) == 0)
			*dev = 0;
	}

	rc = stat(real_path, &stat_buf);
	if (rc) {
		if (verbose)
			fprintf(stderr, "warning: %s, device %s stat failed\n",
				strerror(errno), real_path);
		return rc;
	}

	major = major(stat_buf.st_rdev);
	minor = minor(stat_buf.st_rdev);
	rc = glob("/sys/block/*", GLOB_NOSORT, NULL, &glob_info);
	if (rc) {
		if (verbose)
			fprintf(stderr, "warning: failed to read entries under "
				"/sys/block\n");
		globfree(&glob_info);
		return rc;
	}

	for (i = 0; i < glob_info.gl_pathc; i++){
		snprintf(path, sizeof(path), "%s/dev", glob_info.gl_pathv[i]);

		rc = read_file(path, buf, sizeof(buf));
		if (rc)
			continue;

		if (buf[strlen(buf) - 1] == '\n')
			buf[strlen(buf) - 1] = '\0';

		chk_major = strtok_r(buf, ":", &savept);
		chk_minor = savept;
		if (major == atoi(chk_major) &&minor == atoi(chk_minor))
			break;
	}

	if (i == glob_info.gl_pathc) {
		if (verbose)
			fprintf(stderr,"warning: device %s does not match any "
				"entry under /sys/block\n", real_path);
		globfree(&glob_info);
		return -EINVAL;
	}

	/* Chop off "/dev" from path we found */
	path[strlen(glob_info.gl_pathv[i])] = '\0';
	globfree(&glob_info);

set_params:
	if (strncmp(real_path, "/dev/md", 7) == 0) {
		snprintf(real_path, sizeof(real_path), "%s/%s", path,
			 STRIPE_CACHE_SIZE);

		rc = read_file(real_path, buf, sizeof(buf));
		if (rc) {
			if (verbose)
				fprintf(stderr, "warning: opening %s: %s\n",
					real_path, strerror(errno));
			return 0;
		}

		if (atoi(buf) >= mop->mo_md_stripe_cache_size)
			return 0;

		if (strlen(buf) - 1 > 0) {
			snprintf(buf, sizeof(buf), "%d",
				 mop->mo_md_stripe_cache_size);
			rc = write_file(real_path, buf);
			if (rc && verbose)
				fprintf(stderr, "warning: opening %s: %s\n",
					real_path, strerror(errno));
		}
		/* Return since raid and disk tunables are different */
		return rc;
	}

	snprintf(real_path, sizeof(real_path), "%s/%s", path,
		 MAX_HW_SECTORS_KB_PATH);
	rc = read_file(real_path, buf, sizeof(buf));
	if (rc) {
		if (verbose)
			fprintf(stderr, "warning: opening %s: %s\n",
				real_path, strerror(errno));
		/* No MAX_HW_SECTORS_KB_PATH isn't necessary an
		 * error for some device. */
		rc = 0;
	}

	if (strlen(buf) - 1 > 0) {
		snprintf(real_path, sizeof(real_path), "%s/%s", path,
			 MAX_SECTORS_KB_PATH);
		rc = write_file(real_path, buf);
		if (rc) {
			if (verbose)
				fprintf(stderr, "warning: writing to %s: %s\n",
					real_path, strerror(errno));
			/* No MAX_SECTORS_KB_PATH isn't necessary an
			 * error for some device. */
			rc = 0;
		}
	}

	if (fan_out) {
		char *slave = NULL;
		glob_info.gl_pathc = 0;
		glob_info.gl_offs = 0;
		/* if device is multipath device, tune its slave devices */
		snprintf(real_path, sizeof(real_path), "%s/slaves/*", path);
		rc = glob(real_path, GLOB_NOSORT, NULL, &glob_info);

		for (i = 0; rc == 0 && i < glob_info.gl_pathc; i++){
			slave = basename(glob_info.gl_pathv[i]);
			snprintf(real_path, sizeof(real_path), "/dev/%s", slave);
			rc = set_blockdev_tunables(real_path, mop, 0);
		}

		if (rc == GLOB_NOMATCH) {
			/* no slave device is not an error */
			rc = 0;
		} else if (rc && verbose) {
			if (slave == NULL) {
				fprintf(stderr, "warning: %s, failed to read"
					" entries under %s/slaves\n",
					strerror(errno), path);
			} else {
				fprintf(stderr, "unable to set tunables for"
					" slave device %s (slave would be"
					" unable to handle IO request from"
					" master %s)\n",
					real_path, source);
			}
		}
		globfree(&glob_info);
	}

	return rc;
}

int ldiskfs_tune_lustre(char *dev, struct mount_opts *mop)
{
	return set_blockdev_tunables(dev, mop, 1);
}

int ldiskfs_label_lustre(struct mount_opts *mop)
{
	char label_cmd[PATH_MAX];
	int rc;

	snprintf(label_cmd, sizeof(label_cmd), E2LABEL" %s %s",
		 mop->mo_source, mop->mo_ldd.ldd_svname);
	rc = run_command(label_cmd, sizeof(label_cmd));

	return rc;
}

/* return canonicalized absolute pathname, even if the target file does not
 * exist, unlike realpath */
static char *absolute_path(char *devname)
{
	char  buf[PATH_MAX + 1];
	char *path;
	char *ptr;

	path = malloc(PATH_MAX + 1);
	if (path == NULL)
		return NULL;

	if (devname[0] != '/') {
		if (getcwd(buf, sizeof(buf) - 1) == NULL) {
			free(path);
			return NULL;
		}
		strcat(buf, "/");
		strcat(buf, devname);
	} else {
		strcpy(buf, devname);
	}
	/* truncate filename before calling realpath */
	ptr = strrchr(buf, '/');
	if (ptr == NULL) {
		free(path);
		return NULL;
	}
	*ptr = '\0';
	if (path != realpath(buf, path)) {
		free(path);
		return NULL;
	}
	/* add the filename back */
	strcat(path, "/");
	strcat(path, ptr + 1);
	return path;
}

/* Determine if a device is a block device (as opposed to a file) */
int is_block(char* devname)
{
	struct stat st;
	int	ret = 0;
	char	*devpath;

	devpath = absolute_path(devname);
	if (devpath == NULL) {
		fprintf(stderr, "%s: failed to resolve path to %s\n",
			progname, devname);
		return -1;
	}

	ret = access(devname, F_OK);
	if (ret != 0) {
		if (strncmp(devpath, "/dev/", 5) == 0) {
			/* nobody sane wants to create a loopback file under
			 * /dev. Let's just report the device doesn't exist */
			fprintf(stderr, "%s: %s apparently does not exist\n",
				progname, devpath);
			ret = -1;
			goto out;
		}
		ret = 0;
		goto out;
	}
	ret = stat(devpath, &st);
	if (ret != 0) {
		fprintf(stderr, "%s: cannot stat %s\n", progname, devpath);
		goto out;
	}
	ret = S_ISBLK(st.st_mode);
out:
	free(devpath);
	return ret;
}

static int is_feature_enabled(const char *feature, const char *devpath)
{
	char cmd[PATH_MAX];
	FILE *fp;
	char enabled_features[4096] = "";

	snprintf(cmd, sizeof(cmd), "%s -R features %s 2>&1",
		 DEBUGFS, devpath);

	/* Using popen() instead of run_command() since debugfs does
	 * not return proper error code if command is not supported */
	fp = popen(cmd, "r");
	if (!fp) {
		fprintf(stderr, "%s: %s\n", progname, strerror(errno));
		return 0;
	}

	fread(enabled_features, 1, sizeof(enabled_features), fp);
	fclose(fp);

	if (strstr(enabled_features, feature))
		return 1;
	return 0;
}

/* Enable quota accounting */
int ldiskfs_enable_quota(struct mkfs_opts *mop)
{
	char *dev;
	char cmd[512];
	int cmdsz = sizeof(cmd), ret;

	if (is_e2fsprogs_feature_supp("-O quota") != 0) {
		fprintf(stderr, "%s: \"-O quota\" is is not supported by "
			"current e2fsprogs\n", progname);
		return EINVAL;
	}

	dev = mop->mo_device;
	if (mop->mo_flags & MO_IS_LOOP)
		dev = mop->mo_loopdev;

	/* Quota feature is already enabled? */
	if (is_feature_enabled("quota", dev)) {
		vprint("Quota feature is already enabled.\n");
		return 0;
	}

	/* Turn on quota feature by "tune2fs -O quota" */
	snprintf(cmd, cmdsz, "%s -O quota %s", TUNE2FS, dev);
	ret = run_command(cmd, cmdsz);
	if (ret)
		fprintf(stderr, "command:%s (%d)", cmd, ret);

	return ret;
}

int ldiskfs_init(void)
{
	/* Required because full path to DEBUGFS is not specified */
	setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin", 0);

	return 0;
}

void ldiskfs_fini(void)
{
	return;
}

