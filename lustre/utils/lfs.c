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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/utils/lfs.c
 *
 * Author: Peter J. Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Robert Read <rread@clusterfs.com>
 */

/* for O_DIRECTORY */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <mntent.h>
#include <errno.h>
#include <err.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>
#ifdef HAVE_SYS_QUOTA_H
# include <sys/quota.h>
#endif

/* For dirname() */
#include <libgen.h>

#include <lnet/lnetctl.h>

#include <liblustre.h>
#include <lustre/lustreapi.h>

#include <libcfs/libcfsutil.h>
#include <obd.h>
#include <obd_lov.h>
#include "obdctl.h"

/* all functions */
static int lfs_setstripe(int argc, char **argv);
static int lfs_find(int argc, char **argv);
static int lfs_getstripe(int argc, char **argv);
static int lfs_getdirstripe(int argc, char **argv);
static int lfs_setdirstripe(int argc, char **argv);
static int lfs_rmentry(int argc, char **argv);
static int lfs_osts(int argc, char **argv);
static int lfs_mdts(int argc, char **argv);
static int lfs_df(int argc, char **argv);
static int lfs_getname(int argc, char **argv);
static int lfs_check(int argc, char **argv);
#ifdef HAVE_SYS_QUOTA_H
static int lfs_quotacheck(int argc, char **argv);
static int lfs_quotaon(int argc, char **argv);
static int lfs_quotaoff(int argc, char **argv);
static int lfs_setquota(int argc, char **argv);
static int lfs_quota(int argc, char **argv);
#endif
static int lfs_flushctx(int argc, char **argv);
static int lfs_join(int argc, char **argv);
static int lfs_lsetfacl(int argc, char **argv);
static int lfs_lgetfacl(int argc, char **argv);
static int lfs_rsetfacl(int argc, char **argv);
static int lfs_rgetfacl(int argc, char **argv);
static int lfs_cp(int argc, char **argv);
static int lfs_ls(int argc, char **argv);
static int lfs_poollist(int argc, char **argv);
static int lfs_changelog(int argc, char **argv);
static int lfs_changelog_clear(int argc, char **argv);
static int lfs_fid2path(int argc, char **argv);
static int lfs_path2fid(int argc, char **argv);
static int lfs_data_version(int argc, char **argv);
static int lfs_hsm_state(int argc, char **argv);
static int lfs_hsm_set(int argc, char **argv);
static int lfs_hsm_clear(int argc, char **argv);
static int lfs_hsm_action(int argc, char **argv);
static int lfs_hsm_archive(int argc, char **argv);
static int lfs_hsm_restore(int argc, char **argv);
static int lfs_hsm_release(int argc, char **argv);
static int lfs_hsm_remove(int argc, char **argv);
static int lfs_hsm_cancel(int argc, char **argv);
static int lfs_swap_layouts(int argc, char **argv);

#define SETSTRIPE_USAGE(_cmd, _tgt) \
	"usage: "_cmd" [--stripe-count|-c <stripe_count>]\n"\
	"                 [--stripe-index|-i <start_ost_idx>]\n"\
	"                 [--stripe-size|-S <stripe_size>]\n"\
	"                 [--pool|-p <pool_name>]\n"\
	"                 [--block|-b] "_tgt"\n"\
	"\tstripe_size:  Number of bytes on each OST (0 filesystem default)\n"\
	"\t              Can be specified with k, m or g (in KB, MB and GB\n"\
	"\t              respectively)\n"\
	"\tstart_ost_idx: OST index of first stripe (-1 default)\n"\
	"\tstripe_count: Number of OSTs to stripe over (0 default, -1 all)\n"\
	"\tpool_name:    Name of OST pool to use (default none)\n"\
	"\tblock:	 Block file access during data migration"

/* all avaialable commands */
command_t cmdlist[] = {
	{"setstripe", lfs_setstripe, 0,
	 "Create a new file with a specific striping pattern or\n"
	 "set the default striping pattern on an existing directory or\n"
	 "delete the default striping pattern from an existing directory\n"
	 "usage: setstripe -d <directory>   (to delete default striping)\n"\
	 " or\n"
	 SETSTRIPE_USAGE("setstripe", "<directory|filename>")},
	{"getstripe", lfs_getstripe, 0,
	 "To list the striping info for a given file or files in a\n"
	 "directory or recursively for all files in a directory tree.\n"
	 "usage: getstripe [--ost|-O <uuid>] [--quiet | -q] [--verbose | -v]\n"
	 "		   [--stripe-count|-c] [--stripe-index|-i]\n"
	 "		   [--pool|-p] [--stripe-size|-S] [--directory|-d]\n"
	 "		   [--mdt-index|-M] [--recursive|-r] [--raw|-R]\n"
	 "		   [--layout|-L]\n"
	 "		   <directory|filename> ..."},
	{"setdirstripe", lfs_setdirstripe, 0,
	 "To create a remote directory on a specified MDT.\n"
	 "usage: setdirstripe <--index|-i mdt_index> <dir>\n"
	 "\tmdt_index:    MDT index of first stripe\n"},
	{"getdirstripe", lfs_getdirstripe, 0,
	 "To list the striping info for a given directory\n"
	 "or recursively for all directories in a directory tree.\n"
	 "usage: getdirstripe [--obd|-O <uuid>] [--quiet|-q] [--verbose|-v]\n"
	 "		 [--count|-c ] [--index|-i ] [--raw|-R]\n"
	 "		 [--recursive | -r] <dir> ..."},
	{"mkdir", lfs_setdirstripe, 0,
	 "To create a remote directory on a specified MDT. And this can only\n"
	 "be done on MDT0 by administrator.\n"
	 "usage: mkdir <--index|-i mdt_index> <dir>\n"
	 "\tmdt_index:    MDT index of the remote directory.\n"},
	{"rm_entry", lfs_rmentry, 0,
	 "To remove the name entry of the remote directory. Note: This\n"
	 "command will only delete the name entry, i.e. the remote directory\n"
	 "will become inaccessable after this command. This can only be done\n"
	 "by the administrator\n"
	 "usage: rm_entry <dir>\n"},
        {"pool_list", lfs_poollist, 0,
         "List pools or pool OSTs\n"
         "usage: pool_list <fsname>[.<pool>] | <pathname>\n"},
        {"find", lfs_find, 0,
         "find files matching given attributes recursively in directory tree.\n"
         "usage: find <directory|filename> ...\n"
         "     [[!] --atime|-A [+-]N] [[!] --ctime|-C [+-]N]\n"
         "     [[!] --mtime|-M [+-]N] [[!] --mdt|-m <uuid|index,...>]\n"
         "     [--maxdepth|-D N] [[!] --name|-n <pattern>]\n"
         "     [[!] --ost|-O <uuid|index,...>] [--print|-p] [--print0|-P]\n"
         "     [[!] --size|-s [+-]N[bkMGTPE]]\n"
         "     [[!] --stripe-count|-c [+-]<stripes>]\n"
         "     [[!] --stripe-index|-i <index,...>]\n"
         "     [[!] --stripe-size|-S [+-]N[kMGT]] [[!] --type|-t <filetype>]\n"
         "     [[!] --gid|-g|--group|-G <gid>|<gname>]\n"
         "     [[!] --uid|-u|--user|-U <uid>|<uname>] [[!] --pool <pool>]\n"
	 "     [[!] --layout|-L released,raid0]\n"
         "\t !: used before an option indicates 'NOT' requested attribute\n"
         "\t -: used before a value indicates 'AT MOST' requested value\n"
         "\t +: used before a value indicates 'AT LEAST' requested value\n"},
        {"check", lfs_check, 0,
         "Display the status of MDS or OSTs (as specified in the command)\n"
         "or all the servers (MDS and OSTs).\n"
         "usage: check <osts|mds|servers>"},
        {"join", lfs_join, 0,
         "join two lustre files into one.\n"
         "obsolete, HEAD does not support it anymore.\n"},
        {"osts", lfs_osts, 0, "list OSTs connected to client "
         "[for specified path only]\n" "usage: osts [path]"},
        {"mdts", lfs_mdts, 0, "list MDTs connected to client "
         "[for specified path only]\n" "usage: mdts [path]"},
        {"df", lfs_df, 0,
         "report filesystem disk space usage or inodes usage"
         "of each MDS and all OSDs or a batch belonging to a specific pool .\n"
         "Usage: df [-i] [-h] [--lazy|-l] [--pool|-p <fsname>[.<pool>] [path]"},
        {"getname", lfs_getname, 0, "list instances and specified mount points "
         "[for specified path only]\n"
         "Usage: getname [-h]|[path ...] "},
#ifdef HAVE_SYS_QUOTA_H
        {"quotacheck", lfs_quotacheck, 0,
         "Scan the specified filesystem for disk usage, and create,\n"
         "or update quota files. Deprecated as of 2.4.0.\n"
         "usage: quotacheck [ -ug ] <filesystem>"},
        {"quotaon", lfs_quotaon, 0, "Turn filesystem"
         " quotas on. Deprecated as of 2.4.0.\n"
         "usage: quotaon [ -ugf ] <filesystem>"},
        {"quotaoff", lfs_quotaoff, 0, "Turn filesystem"
         " quotas off. Deprecated as of 2.4.0.\n"
         "usage: quotaoff [ -ug ] <filesystem>"},
        {"setquota", lfs_setquota, 0, "Set filesystem quotas.\n"
         "usage: setquota <-u|-g> <uname>|<uid>|<gname>|<gid>\n"
         "                -b <block-softlimit> -B <block-hardlimit>\n"
         "                -i <inode-softlimit> -I <inode-hardlimit> <filesystem>\n"
         "       setquota <-u|--user|-g|--group> <uname>|<uid>|<gname>|<gid>\n"
         "                [--block-softlimit <block-softlimit>]\n"
         "                [--block-hardlimit <block-hardlimit>]\n"
         "                [--inode-softlimit <inode-softlimit>]\n"
         "                [--inode-hardlimit <inode-hardlimit>] <filesystem>\n"
         "       setquota [-t] <-u|--user|-g|--group>\n"
         "                [--block-grace <block-grace>]\n"
         "                [--inode-grace <inode-grace>] <filesystem>\n"
         "       -b can be used instead of --block-softlimit/--block-grace\n"
         "       -B can be used instead of --block-hardlimit\n"
         "       -i can be used instead of --inode-softlimit/--inode-grace\n"
	 "       -I can be used instead of --inode-hardlimit\n\n"
	 "Note: The total quota space will be split into many qunits and\n"
	 "      balanced over all server targets, the minimal qunit size is\n"
	 "      1M bytes for block space and 1K inodes for inode space.\n\n"
	 "      Quota space rebalancing process will stop when this mininum\n"
	 "      value is reached. As a result, quota exceeded can be returned\n"
	 "      while many targets still have 1MB or 1K inodes of spare\n"
	 "      quota space."},
        {"quota", lfs_quota, 0, "Display disk usage and limits.\n"
	 "usage: quota [-q] [-v] [-h] [-o <obd_uuid>|-i <mdt_idx>|-I "
		       "<ost_idx>]\n"
         "             [<-u|-g> <uname>|<uid>|<gname>|<gid>] <filesystem>\n"
         "       quota [-o <obd_uuid>|-i <mdt_idx>|-I <ost_idx>] -t <-u|-g> <filesystem>"},
#endif
        {"flushctx", lfs_flushctx, 0, "Flush security context for current user.\n"
         "usage: flushctx [-k] [mountpoint...]"},
        {"lsetfacl", lfs_lsetfacl, 0,
         "Remote user setfacl for user/group on the same remote client.\n"
         "usage: lsetfacl [-bkndRLPvh] [{-m|-x} acl_spec] [{-M|-X} acl_file] file ..."},
        {"lgetfacl", lfs_lgetfacl, 0,
         "Remote user getfacl for user/group on the same remote client.\n"
         "usage: lgetfacl [-dRLPvh] file ..."},
        {"rsetfacl", lfs_rsetfacl, 0,
         "Remote user setfacl for user/group on other clients.\n"
         "usage: rsetfacl [-bkndRLPvh] [{-m|-x} acl_spec] [{-M|-X} acl_file] file ..."},
        {"rgetfacl", lfs_rgetfacl, 0,
         "Remote user getfacl for user/group on other clients.\n"
         "usage: rgetfacl [-dRLPvh] file ..."},
        {"cp", lfs_cp, 0,
         "Remote user copy files and directories.\n"
         "usage: cp [OPTION]... [-T] SOURCE DEST\n\tcp [OPTION]... SOURCE... DIRECTORY\n\tcp [OPTION]... -t DIRECTORY SOURCE..."},
        {"ls", lfs_ls, 0,
         "Remote user list directory contents.\n"
         "usage: ls [OPTION]... [FILE]..."},
        {"changelog", lfs_changelog, 0,
         "Show the metadata changes on an MDT."
         "\nusage: changelog <mdtname> [startrec [endrec]]"},
        {"changelog_clear", lfs_changelog_clear, 0,
         "Indicate that old changelog records up to <endrec> are no longer of "
         "interest to consumer <id>, allowing the system to free up space.\n"
         "An <endrec> of 0 means all records.\n"
         "usage: changelog_clear <mdtname> <id> <endrec>"},
	{"fid2path", lfs_fid2path, 0,
	 "Resolve the full path(s) for given FID(s). For a specific hardlink "
	 "specify link number <linkno>.\n"
	/* "For a historical link name, specify changelog record <recno>.\n" */
	 "usage: fid2path [--link <linkno>] <fsname|rootpath> <fid> ..."
		/* [ --rec <recno> ] */ },
	{"path2fid", lfs_path2fid, 0, "Display the fid(s) for a given path(s).\n"
	 "usage: path2fid <path> ..."},
	{"data_version", lfs_data_version, 0, "Display file data version for "
	 "a given path.\n" "usage: data_version -[n|r|w] <path>"},
	{"hsm_state", lfs_hsm_state, 0, "Display the HSM information (states, "
	 "undergoing actions) for given files.\n usage: hsm_state <file> ..."},
	{"hsm_set", lfs_hsm_set, 0, "Set HSM user flag on specified files.\n"
	 "usage: hsm_set [--norelease] [--noarchive] [--dirty] [--exists] "
	 "[--archived] [--lost] <file> ..."},
	{"hsm_clear", lfs_hsm_clear, 0, "Clear HSM user flag on specified "
	 "files.\n"
	 "usage: hsm_clear [--norelease] [--noarchive] [--dirty] [--exists] "
	 "[--archived] [--lost] <file> ..."},
	{"hsm_action", lfs_hsm_action, 0, "Display current HSM request for "
	 "given files.\n" "usage: hsm_action <file> ..."},
	{"hsm_archive", lfs_hsm_archive, 0,
	 "Archive file to external storage.\n"
	 "usage: hsm_archive [--filelist FILELIST] [--data DATA] [--archive NUM] "
	 "<file> ..."},
	{"hsm_restore", lfs_hsm_restore, 0,
	 "Restore file from external storage.\n"
	 "usage: hsm_restore [--filelist FILELIST] [--data DATA] <file> ..."},
	{"hsm_release", lfs_hsm_release, 0,
	 "Release files from Lustre.\n"
	 "usage: hsm_release [--filelist FILELIST] [--data DATA] <file> ..."},
	{"hsm_remove", lfs_hsm_remove, 0,
	 "Remove file copy from external storage.\n"
	 "usage: hsm_remove [--filelist FILELIST] [--data DATA] <file> ..."},
	{"hsm_cancel", lfs_hsm_cancel, 0,
	 "Cancel requests related to specified files.\n"
	 "usage: hsm_cancel [--filelist FILELIST] [--data DATA] <file> ..."},
	{"swap_layouts", lfs_swap_layouts, 0, "Swap layouts between 2 files.\n"
	 "usage: swap_layouts <path1> <path2>"},
	{"migrate", lfs_setstripe, 0, "migrate file from one layout to "
	 "another (may be not safe with concurent writes).\n"
	 SETSTRIPE_USAGE("migrate  ", "<filename>")},
        {"help", Parser_help, 0, "help"},
        {"exit", Parser_quit, 0, "quit"},
        {"quit", Parser_quit, 0, "quit"},
        { 0, 0, 0, NULL }
};

static int isnumber(const char *str)
{
        const char *ptr;

        if (str[0] != '-' && !isdigit(str[0]))
                return 0;

        for (ptr = str + 1; *ptr != '\0'; ptr++) {
                if (!isdigit(*ptr))
                        return 0;
        }

        return 1;
}

#define MIGRATION_BLOCKS 1

static int lfs_migrate(char *name, unsigned long long stripe_size,
		       int stripe_offset, int stripe_count,
		       int stripe_pattern, char *pool_name,
		       __u64 migration_flags)
{
	int			 fd, fdv;
	char			 volatile_file[PATH_MAX];
	char			 parent[PATH_MAX];
	char			*ptr;
	int			 rc;
	__u64			 dv1;
	struct lov_user_md	*lum = NULL;
	int			 lumsz;
	int			 bufsz;
	void			*buf = NULL;
	int			 rsize, wsize;
	__u64			 rpos, wpos, bufoff;
	int			 gid = 0, sz;
	int			 have_gl = 0;
	struct stat		 st, stv;

	/* find the right size for the IO and allocate the buffer */
	lumsz = lov_user_md_size(LOV_MAX_STRIPE_COUNT, LOV_USER_MAGIC_V3);
	lum = malloc(lumsz);
	if (lum == NULL) {
		rc = -ENOMEM;
		goto free;
	}

	rc = llapi_file_get_stripe(name, lum);
	/* failure can come from may case and some may be not real error
	 * (eg: no stripe)
	 * in case of a real error, a later call will failed with a better
	 * error management */
	if (rc < 0)
		bufsz = 1024*1024;
	else
		bufsz = lum->lmm_stripe_size;
	rc = posix_memalign(&buf, getpagesize(), bufsz);
	if (rc != 0) {
		rc = -rc;
		goto free;
	}

	if (migration_flags & MIGRATION_BLOCKS) {
		/* generate a random id for the grouplock */
		fd = open("/dev/urandom", O_RDONLY);
		if (fd == -1) {
			rc = -errno;
			fprintf(stderr, "cannot open /dev/urandom (%s)\n",
				strerror(-rc));
			goto free;
		}
		sz = sizeof(gid);
		rc = read(fd, &gid, sz);
		close(fd);
		if (rc < sz) {
			rc = -errno;
			fprintf(stderr, "cannot read %d bytes from"
				" /dev/urandom (%s)\n", sz, strerror(-rc));
			goto free;
		}
	}

	/* search for file directory pathname */
	if (strlen(name) > sizeof(parent)-1) {
		rc = -E2BIG;
		goto free;
	}
	strncpy(parent, name, sizeof(parent));
	ptr = strrchr(parent, '/');
	if (ptr == NULL) {
		if (getcwd(parent, sizeof(parent)) == NULL) {
			rc = -errno;
			goto free;
		}
	} else {
		if (ptr == parent)
			strcpy(parent, "/");
		else
			*ptr = '\0';
	}
	sprintf(volatile_file, "%s/%s::", parent, LUSTRE_VOLATILE_HDR);

	/* create, open a volatile file, use caching (ie no directio) */
	/* exclusive create is not needed because volatile files cannot
	 * conflict on name by construction */
	fdv = llapi_file_open_pool(volatile_file, O_CREAT | O_WRONLY,
				   0644, stripe_size, stripe_offset,
				   stripe_count, stripe_pattern, pool_name);
	if (fdv < 0) {
		rc = fdv;
		fprintf(stderr, "cannot create volatile file in %s (%s)\n",
			parent, strerror(-rc));
		goto free;
	}

	/* open file, direct io */
	/* even if the file is only read, WR mode is nedeed to allow
	 * layout swap on fd */
	fd = open(name, O_RDWR | O_DIRECT);
	if (fd == -1) {
		rc = -errno;
		fprintf(stderr, "cannot open %s (%s)\n", name, strerror(-rc));
		close(fdv);
		goto free;
	}

	/* Not-owner (root?) special case.
	 * Need to set owner/group of volatile file like original.
	 * This will allow to pass related check during layout_swap.
	 */
	rc = fstat(fd, &st);
	if (rc != 0) {
		rc = -errno;
		fprintf(stderr, "cannot stat %s (%s)\n", name,
			strerror(errno));
		goto error;
	}
	rc = fstat(fdv, &stv);
	if (rc != 0) {
		rc = -errno;
		fprintf(stderr, "cannot stat %s (%s)\n", volatile_file,
			strerror(errno));
		goto error;
	}
	if (st.st_uid != stv.st_uid || st.st_gid != stv.st_gid) {
		rc = fchown(fdv, st.st_uid, st.st_gid);
		if (rc != 0) {
			rc = -errno;
			fprintf(stderr, "cannot chown %s (%s)\n", name,
				strerror(errno));
			goto error;
		}
	}

	/* get file data version */
	rc = llapi_get_data_version(fd, &dv1, LL_DV_RD_FLUSH);
	if (rc != 0) {
		fprintf(stderr, "cannot get dataversion on %s (%s)\n",
			name, strerror(-rc));
		goto error;
	}

	if (migration_flags & MIGRATION_BLOCKS) {
		/* take group lock to limit concurent access
		 * this will be no more needed when exclusive access will
		 * be implemented (see LU-2919) */
		/* group lock is taken after data version read because it
		 * blocks data version call */
		if (ioctl(fd, LL_IOC_GROUP_LOCK, gid) == -1) {
			rc = -errno;
			fprintf(stderr, "cannot get group lock on %s (%s)\n",
				name, strerror(-rc));
			goto error;
		}
		have_gl = 1;
	}

	/* copy data */
	rpos = 0;
	wpos = 0;
	bufoff = 0;
	rsize = -1;
	do {
		/* read new data only if we have written all
		 * previously read data */
		if (wpos == rpos) {
			rsize = read(fd, buf, bufsz);
			if (rsize < 0) {
				rc = -errno;
				fprintf(stderr, "read failed on %s"
					" (%s)\n", name,
					strerror(-rc));
				goto error;
			}
			rpos += rsize;
			bufoff = 0;
		}
		/* eof ? */
		if (rsize == 0)
			break;
		wsize = write(fdv, buf + bufoff, rpos - wpos);
		if (wsize < 0) {
			rc = -errno;
			fprintf(stderr, "write failed on volatile"
				" for %s (%s)\n", name, strerror(-rc));
			goto error;
		}
		wpos += wsize;
		bufoff += wsize;
	} while (1);

	/* flush data */
	fsync(fdv);

	if (migration_flags & MIGRATION_BLOCKS) {
		/* give back group lock */
		if (ioctl(fd, LL_IOC_GROUP_UNLOCK, gid) == -1) {
			rc = -errno;
			fprintf(stderr, "cannot put group lock on %s (%s)\n",
				name, strerror(-rc));
		}
		have_gl = 0;
	}

	/* swap layouts
	 * for a migration we need to:
	 * - check data version on file did not change
	 * - keep file mtime
	 * - keep file atime
	 */
	rc = llapi_fswap_layouts(fd, fdv, dv1, 0,
				 SWAP_LAYOUTS_CHECK_DV1 |
				 SWAP_LAYOUTS_KEEP_MTIME |
				 SWAP_LAYOUTS_KEEP_ATIME);
	if (rc == -EAGAIN) {
		fprintf(stderr, "%s: dataversion changed during copy, "
			"migration aborted\n", name);
		goto error;
	}
	if (rc != 0)
		fprintf(stderr, "%s: swap layout to new file failed: %s\n",
			name, strerror(-rc));

error:
	/* give back group lock */
	if ((migration_flags & MIGRATION_BLOCKS) && have_gl &&
	    (ioctl(fd, LL_IOC_GROUP_UNLOCK, gid) == -1)) {
		/* we keep in rc the original error */
		fprintf(stderr, "cannot put group lock on %s (%s)\n",
			name, strerror(-errno));
	}

	close(fdv);
	close(fd);
free:
	if (lum)
		free(lum);
	if (buf)
		free(buf);
	return rc;
}

/* functions */
static int lfs_setstripe(int argc, char **argv)
{
	char			*fname;
	int			 result;
	unsigned long long	 st_size;
	int			 st_offset, st_count;
	char			*end;
	int			 c;
	int			 delete = 0;
	char			*stripe_size_arg = NULL;
	char			*stripe_off_arg = NULL;
	char			*stripe_count_arg = NULL;
	char			*pool_name_arg = NULL;
	unsigned long long	 size_units = 1;
	int			 migrate_mode = 0;
	__u64			 migration_flags = 0;

	struct option		 long_opts[] = {
		/* valid only in migrate mode */
		{"block",	 no_argument,	    0, 'b'},
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --count option"
#else
		/* This formerly implied "stripe-count", but was explicitly
		 * made "stripe-count" for consistency with other options,
		 * and to separate it from "mdt-count" when DNE arrives. */
		{"count",	 required_argument, 0, 'c'},
#endif
		{"stripe-count", required_argument, 0, 'c'},
		{"stripe_count", required_argument, 0, 'c'},
		{"delete",       no_argument,       0, 'd'},
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --index option"
#else
		/* This formerly implied "stripe-index", but was explicitly
		 * made "stripe-index" for consistency with other options,
		 * and to separate it from "mdt-index" when DNE arrives. */
		{"index",	 required_argument, 0, 'i'},
#endif
		{"stripe-index", required_argument, 0, 'i'},
		{"stripe_index", required_argument, 0, 'i'},
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --offset option"
#else
		/* This formerly implied "stripe-index", but was confusing
		 * with "file offset" (which will eventually be needed for
		 * with different layouts by offset), so deprecate it. */
		{"offset",       required_argument, 0, 'o'},
#endif
		{"pool",	 required_argument, 0, 'p'},
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --size option"
#else
		/* This formerly implied "--stripe-size", but was confusing
		 * with "lfs find --size|-s", which means "file size", so use
		 * the consistent "--stripe-size|-S" for all commands. */
		{"size",	 required_argument, 0, 's'},
#endif
		{"stripe-size",  required_argument, 0, 'S'},
		{"stripe_size",  required_argument, 0, 'S'},
		{0, 0, 0, 0}
	};

        st_size = 0;
        st_offset = -1;
        st_count = 0;

	if (strcmp(argv[0], "migrate") == 0)
		migrate_mode = 1;

#if LUSTRE_VERSION < OBD_OCD_VERSION(2,4,50,0)
        if (argc == 5 && argv[1][0] != '-' &&
            isnumber(argv[2]) && isnumber(argv[3]) && isnumber(argv[4])) {
                fprintf(stderr, "error: obsolete usage of setstripe "
                        "positional parameters.  Use -c, -i, -S instead.\n");
                return CMD_HELP;
        } else
#else
#warning "remove obsolete positional parameter error"
#endif
        {
                while ((c = getopt_long(argc, argv, "c:di:o:p:s:S:",
                                        long_opts, NULL)) >= 0) {
                switch (c) {
                case 0:
                        /* Long options. */
                        break;
		case 'b':
			if (migrate_mode == 0) {
				fprintf(stderr, "--block is valid only for"
						" migrate mode");
				return CMD_HELP;
			}
			migration_flags |= MIGRATION_BLOCKS;
			break;
                case 'c':
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --count option"
#elif LUSTRE_VERSION >= OBD_OCD_VERSION(2,6,50,0)
                        if (strcmp(argv[optind - 1], "--count") == 0)
                                fprintf(stderr, "warning: '--count' deprecated"
                                        ", use '--stripe-count' instead\n");
#endif
                        stripe_count_arg = optarg;
                        break;
                case 'd':
                        /* delete the default striping pattern */
                        delete = 1;
                        break;
                case 'o':
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,4,50,0)
                        fprintf(stderr, "warning: '--offset|-o' deprecated, "
                                "use '--stripe-index|-i' instead\n");
#else
                        if (strcmp(argv[optind - 1], "--offset") == 0)
                                /* need --stripe-index established first */
                                fprintf(stderr, "warning: '--offset' deprecated"
                                        ", use '--index' instead\n");
#endif
                case 'i':
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --offset and --index options"
#elif LUSTRE_VERSION >= OBD_OCD_VERSION(2,6,50,0)
                        if (strcmp(argv[optind - 1], "--index") == 0)
                                fprintf(stderr, "warning: '--index' deprecated"
                                        ", use '--stripe-index' instead\n");
#endif
                        stripe_off_arg = optarg;
                        break;
                case 's':
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --size option"
#elif LUSTRE_VERSION >= OBD_OCD_VERSION(2,6,50,0)
                        fprintf(stderr, "warning: '--size|-s' deprecated, "
                                "use '--stripe-size|-S' instead\n");
#endif
                case 'S':
                        stripe_size_arg = optarg;
                        break;
                case 'p':
                        pool_name_arg = optarg;
                        break;
                default:
                        return CMD_HELP;
                }
                }

                fname = argv[optind];

                if (delete &&
                    (stripe_size_arg != NULL || stripe_off_arg != NULL ||
                     stripe_count_arg != NULL || pool_name_arg != NULL)) {
                        fprintf(stderr, "error: %s: cannot specify -d with "
                                        "-s, -c, -o, or -p options\n",
                                        argv[0]);
                        return CMD_HELP;
                }
        }

        if (optind == argc) {
                fprintf(stderr, "error: %s: missing filename|dirname\n",
                        argv[0]);
                return CMD_HELP;
        }

	/* get the stripe size */
	if (stripe_size_arg != NULL) {
		result = llapi_parse_size(stripe_size_arg, &st_size,
					  &size_units, 0);
		if (result) {
			fprintf(stderr, "error: %s: bad stripe size '%s'\n",
				argv[0], stripe_size_arg);
			return result;
		}
	}
        /* get the stripe offset */
        if (stripe_off_arg != NULL) {
                st_offset = strtol(stripe_off_arg, &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe offset '%s'\n",
                                argv[0], stripe_off_arg);
                        return CMD_HELP;
                }
        }
        /* get the stripe count */
        if (stripe_count_arg != NULL) {
                st_count = strtoul(stripe_count_arg, &end, 0);
                if (*end != '\0') {
                        fprintf(stderr, "error: %s: bad stripe count '%s'\n",
                                argv[0], stripe_count_arg);
                        return CMD_HELP;
                }
        }

	do {
		if (migrate_mode)
			result = lfs_migrate(fname, st_size, st_offset,
					     st_count, 0, pool_name_arg,
					     migration_flags);
		else
			result = llapi_file_create_pool(fname, st_size,
							st_offset, st_count,
							0, pool_name_arg);
		if (result) {
			fprintf(stderr,
				"error: %s: %s stripe file '%s' failed\n",
				argv[0], migrate_mode ? "migrate" : "create",
				fname);
			break;
		}
		fname = argv[++optind];
	} while (fname != NULL);

	return result;
}

static int lfs_poollist(int argc, char **argv)
{
        if (argc != 2)
                return CMD_HELP;

        return llapi_poollist(argv[1]);
}

static int set_time(time_t *time, time_t *set, char *str)
{
        time_t t;
        int res = 0;

        if (str[0] == '+')
                res = 1;
        else if (str[0] == '-')
                res = -1;

        if (res)
                str++;

        t = strtol(str, NULL, 0);
        if (*time < t * 24 * 60 * 60) {
                if (res)
                        str--;
                fprintf(stderr, "Wrong time '%s' is specified.\n", str);
                return INT_MAX;
        }

        *set = *time - t * 24 * 60 * 60;
        return res;
}

#define USER 0
#define GROUP 1

static int name2id(unsigned int *id, char *name, int type)
{
        if (type == USER) {
                struct passwd *entry;

                if (!(entry = getpwnam(name))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *id = entry->pw_uid;
        } else {
                struct group *entry;

                if (!(entry = getgrnam(name))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *id = entry->gr_gid;
        }

        return 0;
}

static int id2name(char **name, unsigned int id, int type)
{
        if (type == USER) {
                struct passwd *entry;

                if (!(entry = getpwuid(id))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *name = entry->pw_name;
        } else {
                struct group *entry;

                if (!(entry = getgrgid(id))) {
                        if (!errno)
                                errno = ENOENT;
                        return -1;
                }

                *name = entry->gr_name;
        }

        return 0;
}

static int name2layout(__u32 *layout, char *name)
{
	char *ptr, *lyt;

	*layout = 0;
	for (ptr = name; ; ptr = NULL) {
		lyt = strtok(ptr, ",");
		if (lyt == NULL)
			break;
		if (strcmp(lyt, "released") == 0)
			*layout |= LOV_PATTERN_F_RELEASED;
		else if (strcmp(lyt, "raid0") == 0)
			*layout |= LOV_PATTERN_RAID0;
		else
			return -1;
	}
	return 0;
}

#define FIND_POOL_OPT 3
static int lfs_find(int argc, char **argv)
{
	int c, rc;
	int ret = 0;
        time_t t;
        struct find_param param = { .maxdepth = -1, .quiet = 1 };
        struct option long_opts[] = {
                {"atime",        required_argument, 0, 'A'},
                {"stripe-count", required_argument, 0, 'c'},
                {"stripe_count", required_argument, 0, 'c'},
                {"ctime",        required_argument, 0, 'C'},
                {"maxdepth",     required_argument, 0, 'D'},
                {"gid",          required_argument, 0, 'g'},
                {"group",        required_argument, 0, 'G'},
                {"stripe-index", required_argument, 0, 'i'},
                {"stripe_index", required_argument, 0, 'i'},
		{"layout",	 required_argument, 0, 'L'},
                {"mdt",          required_argument, 0, 'm'},
                {"mtime",        required_argument, 0, 'M'},
                {"name",         required_argument, 0, 'n'},
     /* reserve {"or",           no_argument,     , 0, 'o'}, to match find(1) */
                {"obd",          required_argument, 0, 'O'},
                {"ost",          required_argument, 0, 'O'},
                /* no short option for pool, p/P already used */
                {"pool",         required_argument, 0, FIND_POOL_OPT},
                {"print0",       no_argument,       0, 'p'},
                {"print",        no_argument,       0, 'P'},
                {"size",         required_argument, 0, 's'},
                {"stripe-size",  required_argument, 0, 'S'},
                {"stripe_size",  required_argument, 0, 'S'},
                {"type",         required_argument, 0, 't'},
                {"uid",          required_argument, 0, 'u'},
                {"user",         required_argument, 0, 'U'},
                {0, 0, 0, 0}
        };
        int pathstart = -1;
        int pathend = -1;
        int neg_opt = 0;
        time_t *xtime;
        int *xsign;
        int isoption;
        char *endptr;

        time(&t);

	/* when getopt_long_only() hits '!' it returns 1, puts "!" in optarg */
	while ((c = getopt_long_only(argc, argv,
				     "-A:c:C:D:g:G:i:L:m:M:n:O:Ppqrs:S:t:u:U:v",
				     long_opts, NULL)) >= 0) {
                xtime = NULL;
                xsign = NULL;
                if (neg_opt)
                        --neg_opt;
                /* '!' is part of option */
                /* when getopt_long_only() finds a string which is not
                 * an option nor a known option argument it returns 1
                 * in that case if we already have found pathstart and pathend
                 * (i.e. we have the list of pathnames),
                 * the only supported value is "!"
                 */
                isoption = (c != 1) || (strcmp(optarg, "!") == 0);
                if (!isoption && pathend != -1) {
                        fprintf(stderr, "err: %s: filename|dirname must either "
                                        "precede options or follow options\n",
                                        argv[0]);
                        ret = CMD_HELP;
                        goto err;
                }
                if (!isoption && pathstart == -1)
                        pathstart = optind - 1;
                if (isoption && pathstart != -1 && pathend == -1)
                        pathend = optind - 2;
                switch (c) {
                case 0:
                        /* Long options. */
                        break;
                case 1:
                        /* unknown; opt is "!" or path component,
                         * checking done above.
                         */
                        if (strcmp(optarg, "!") == 0)
                                neg_opt = 2;
                        break;
                case 'A':
                        xtime = &param.atime;
                        xsign = &param.asign;
                        param.exclude_atime = !!neg_opt;
                        /* no break, this falls through to 'C' for ctime */
                case 'C':
                        if (c == 'C') {
                                xtime = &param.ctime;
                                xsign = &param.csign;
                                param.exclude_ctime = !!neg_opt;
                        }
                        /* no break, this falls through to 'M' for mtime */
		case 'M':
			if (c == 'M') {
				xtime = &param.mtime;
				xsign = &param.msign;
				param.exclude_mtime = !!neg_opt;
			}
			rc = set_time(&t, xtime, optarg);
			if (rc == INT_MAX) {
				ret = -1;
				goto err;
			}
			if (rc)
				*xsign = rc;
			break;
                case 'c':
                        if (optarg[0] == '+') {
                                param.stripecount_sign = -1;
                                optarg++;
                        } else if (optarg[0] == '-') {
                                param.stripecount_sign =  1;
                                optarg++;
                        }

                        param.stripecount = strtoul(optarg, &endptr, 0);
                        if (*endptr != '\0') {
                                fprintf(stderr,"error: bad stripe_count '%s'\n",
                                        optarg);
                                ret = -1;
                                goto err;
                        }
                        param.check_stripecount = 1;
                        param.exclude_stripecount = !!neg_opt;
                        break;
                case 'D':
                        param.maxdepth = strtol(optarg, 0, 0);
                        break;
                case 'g':
		case 'G':
			rc = name2id(&param.gid, optarg, GROUP);
			if (rc) {
				param.gid = strtoul(optarg, &endptr, 10);
                                if (*endptr != '\0') {
                                        fprintf(stderr, "Group/GID: %s cannot "
                                                "be found.\n", optarg);
                                        ret = -1;
                                        goto err;
                                }
                        }
                        param.exclude_gid = !!neg_opt;
                        param.check_gid = 1;
                        break;
		case 'L':
			ret = name2layout(&param.layout, optarg);
			if (ret)
				goto err;
			param.exclude_layout = !!neg_opt;
			param.check_layout = 1;
			break;
		case 'u':
		case 'U':
			rc = name2id(&param.uid, optarg, USER);
			if (rc) {
				param.uid = strtoul(optarg, &endptr, 10);
                                if (*endptr != '\0') {
                                        fprintf(stderr, "User/UID: %s cannot "
                                                "be found.\n", optarg);
                                        ret = -1;
                                        goto err;
                                }
                        }
                        param.exclude_uid = !!neg_opt;
                        param.check_uid = 1;
                        break;
                case FIND_POOL_OPT:
                        if (strlen(optarg) > LOV_MAXPOOLNAME) {
                                fprintf(stderr,
                                        "Pool name %s is too long"
                                        " (max is %d)\n", optarg,
                                        LOV_MAXPOOLNAME);
                                ret = -1;
                                goto err;
                        }
                        /* we do check for empty pool because empty pool
                         * is used to find V1 lov attributes */
                        strncpy(param.poolname, optarg, LOV_MAXPOOLNAME);
                        param.poolname[LOV_MAXPOOLNAME] = '\0';
                        param.exclude_pool = !!neg_opt;
                        param.check_pool = 1;
                        break;
                case 'n':
                        param.pattern = (char *)optarg;
                        param.exclude_pattern = !!neg_opt;
                        break;
                case 'm':
                case 'i':
                case 'O': {
                        char *buf, *token, *next, *p;
                        int len = 1;
                        void *tmp;

                        buf = strdup(optarg);
                        if (buf == NULL) {
                                ret = -ENOMEM;
                                goto err;
                        }

                        param.exclude_obd = !!neg_opt;

                        token = buf;
                        while (token && *token) {
                                token = strchr(token, ',');
                                if (token) {
                                        len++;
                                        token++;
                                }
                        }
                        if (c == 'm') {
                                param.exclude_mdt = !!neg_opt;
                                param.num_alloc_mdts += len;
                                tmp = realloc(param.mdtuuid,
                                              param.num_alloc_mdts *
                                              sizeof(*param.mdtuuid));
                                if (tmp == NULL)
                                        GOTO(err_free, ret = -ENOMEM);
                                param.mdtuuid = tmp;
                        } else {
                                param.exclude_obd = !!neg_opt;
                                param.num_alloc_obds += len;
                                tmp = realloc(param.obduuid,
                                              param.num_alloc_obds *
                                              sizeof(*param.obduuid));
                                if (tmp == NULL)
                                        GOTO(err_free, ret = -ENOMEM);
                                param.obduuid = tmp;
                        }
                        for (token = buf; token && *token; token = next) {
				struct obd_uuid *puuid;
				if (c == 'm') {
					puuid =
					  &param.mdtuuid[param.num_mdts++];
				} else {
					puuid =
					  &param.obduuid[param.num_obds++];
				}
                                p = strchr(token, ',');
                                next = 0;
                                if (p) {
                                        *p = 0;
                                        next = p+1;
                                }
				if (strlen(token) > sizeof(puuid->uuid)-1)
					GOTO(err_free, ret = -E2BIG);
				strncpy(puuid->uuid, token,
					sizeof(puuid->uuid));
                        }
err_free:
                        if (buf)
                                free(buf);
                        break;
                }
                case 'p':
                        param.zeroend = 1;
                        break;
                case 'P':
                        break;
		case 's':
			if (optarg[0] == '+') {
				param.size_sign = -1;
				optarg++;
			} else if (optarg[0] == '-') {
				param.size_sign =  1;
				optarg++;
			}

			ret = llapi_parse_size(optarg, &param.size,
					       &param.size_units, 0);
			if (ret) {
				fprintf(stderr, "error: bad file size '%s'\n",
					optarg);
				goto err;
			}
			param.check_size = 1;
			param.exclude_size = !!neg_opt;
			break;
		case 'S':
			if (optarg[0] == '+') {
				param.stripesize_sign = -1;
				optarg++;
			} else if (optarg[0] == '-') {
				param.stripesize_sign =  1;
				optarg++;
			}

			ret = llapi_parse_size(optarg, &param.stripesize,
					       &param.stripesize_units, 0);
			if (ret) {
				fprintf(stderr, "error: bad stripe_size '%s'\n",
					optarg);
				goto err;
			}
			param.check_stripesize = 1;
			param.exclude_stripesize = !!neg_opt;
			break;
                case 't':
                        param.exclude_type = !!neg_opt;
                        switch(optarg[0]) {
                        case 'b': param.type = S_IFBLK; break;
                        case 'c': param.type = S_IFCHR; break;
                        case 'd': param.type = S_IFDIR; break;
                        case 'f': param.type = S_IFREG; break;
                        case 'l': param.type = S_IFLNK; break;
                        case 'p': param.type = S_IFIFO; break;
                        case 's': param.type = S_IFSOCK; break;
#ifdef S_IFDOOR /* Solaris only */
                        case 'D': param.type = S_IFDOOR; break;
#endif
                        default: fprintf(stderr, "error: %s: bad type '%s'\n",
                                         argv[0], optarg);
                                 ret = CMD_HELP;
                                 goto err;
                        };
                        break;
                default:
                        ret = CMD_HELP;
                        goto err;
                };
        }

        if (pathstart == -1) {
                fprintf(stderr, "error: %s: no filename|pathname\n",
                        argv[0]);
                ret = CMD_HELP;
                goto err;
        } else if (pathend == -1) {
                /* no options */
                pathend = argc;
        }

	do {
		rc = llapi_find(argv[pathstart], &param);
		if (rc != 0 && ret == 0)
			ret = rc;
	} while (++pathstart < pathend);

        if (ret)
                fprintf(stderr, "error: %s failed for %s.\n",
                        argv[0], argv[optind - 1]);
err:
        if (param.obduuid && param.num_alloc_obds)
                free(param.obduuid);

        if (param.mdtuuid && param.num_alloc_mdts)
                free(param.mdtuuid);

        return ret;
}

static int lfs_getstripe_internal(int argc, char **argv,
				  struct find_param *param)
{
	struct option long_opts[] = {
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --count option"
#else
		/* This formerly implied "stripe-count", but was explicitly
		 * made "stripe-count" for consistency with other options,
		 * and to separate it from "mdt-count" when DNE arrives. */
		{"count",		no_argument,		0, 'c'},
#endif
		{"stripe-count",	no_argument,		0, 'c'},
		{"stripe_count",	no_argument,		0, 'c'},
		{"directory",		no_argument,		0, 'd'},
		{"generation",		no_argument,		0, 'g'},
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --index option"
#else
		/* This formerly implied "stripe-index", but was explicitly
		 * made "stripe-index" for consistency with other options,
		 * and to separate it from "mdt-index" when DNE arrives. */
		{"index",		no_argument,		0, 'i'},
#endif
		{"stripe-index",	no_argument,		0, 'i'},
		{"stripe_index",	no_argument,		0, 'i'},
		{"layout",		no_argument,		0, 'L'},
		{"mdt-index",		no_argument,		0, 'M'},
		{"mdt_index",		no_argument,		0, 'M'},
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --offset option"
#else
		/* This formerly implied "stripe-index", but was confusing
		 * with "file offset" (which will eventually be needed for
		 * with different layouts by offset), so deprecate it. */
		{"offset",		no_argument,		0, 'o'},
#endif
		{"obd",			required_argument,	0, 'O'},
		{"ost",			required_argument,	0, 'O'},
		{"pool",		no_argument,		0, 'p'},
		{"quiet",		no_argument,		0, 'q'},
		{"recursive",		no_argument,		0, 'r'},
		{"raw",			no_argument,		0, 'R'},
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --size option"
#else
		/* This formerly implied "--stripe-size", but was confusing
		 * with "lfs find --size|-s", which means "file size", so use
		 * the consistent "--stripe-size|-S" for all commands. */
		{"size",		no_argument,		0, 's'},
#endif
		{"stripe-size",		no_argument,		0, 'S'},
		{"stripe_size",		no_argument,		0, 'S'},
		{"verbose",		no_argument,		0, 'v'},
		{0, 0, 0, 0}
	};
	int c, rc;

	param->maxdepth = 1;
	while ((c = getopt_long(argc, argv, "cdghiLMoO:pqrRsSv",
				long_opts, NULL)) != -1) {
		switch (c) {
		case 'O':
			if (param->obduuid) {
				fprintf(stderr,
					"error: %s: only one obduuid allowed",
					argv[0]);
				return CMD_HELP;
			}
			param->obduuid = (struct obd_uuid *)optarg;
			break;
		case 'q':
			param->quiet++;
			break;
		case 'd':
			param->maxdepth = 0;
			break;
		case 'r':
			param->recursive = 1;
			break;
		case 'v':
			param->verbose = VERBOSE_ALL | VERBOSE_DETAIL;
			break;
		case 'c':
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --count option"
#elif LUSTRE_VERSION >= OBD_OCD_VERSION(2,6,50,0)
			if (strcmp(argv[optind - 1], "--count") == 0)
				fprintf(stderr, "warning: '--count' deprecated,"
					" use '--stripe-count' instead\n");
#endif
			if (!(param->verbose & VERBOSE_DETAIL)) {
				param->verbose |= VERBOSE_COUNT;
				param->maxdepth = 0;
			}
			break;
		case 's':
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --size option"
#elif LUSTRE_VERSION >= OBD_OCD_VERSION(2,6,50,0)
			fprintf(stderr, "warning: '--size|-s' deprecated, "
				"use '--stripe-size|-S' instead\n");
#endif
		case 'S':
			if (!(param->verbose & VERBOSE_DETAIL)) {
				param->verbose |= VERBOSE_SIZE;
				param->maxdepth = 0;
			}
			break;
		case 'o':
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,4,50,0)
			fprintf(stderr, "warning: '--offset|-o' deprecated, "
				"use '--stripe-index|-i' instead\n");
#else
			if (strcmp(argv[optind - 1], "--offset") == 0)
				/* need --stripe-index established first */
				fprintf(stderr, "warning: '--offset' deprecated"
					", use '--index' instead\n");
#endif
		case 'i':
#if LUSTRE_VERSION >= OBD_OCD_VERSION(2,9,50,0)
#warning "remove deprecated --offset and --index options"
#elif LUSTRE_VERSION >= OBD_OCD_VERSION(2,6,50,0)
			if (strcmp(argv[optind - 1], "--index") == 0)
				fprintf(stderr, "warning: '--index' deprecated"
					", use '--stripe-index' instead\n");
#endif
			if (!(param->verbose & VERBOSE_DETAIL)) {
				param->verbose |= VERBOSE_OFFSET;
				param->maxdepth = 0;
			}
			break;
		case 'p':
			if (!(param->verbose & VERBOSE_DETAIL)) {
				param->verbose |= VERBOSE_POOL;
				param->maxdepth = 0;
			}
			break;
		case 'g':
			if (!(param->verbose & VERBOSE_DETAIL)) {
				param->verbose |= VERBOSE_GENERATION;
				param->maxdepth = 0;
			}
			break;
		case 'L':
			if (!(param->verbose & VERBOSE_DETAIL)) {
				param->verbose |= VERBOSE_LAYOUT;
				param->maxdepth = 0;
			}
			break;
		case 'M':
			if (!(param->verbose & VERBOSE_DETAIL))
				param->maxdepth = 0;
			param->verbose |= VERBOSE_MDTINDEX;
			break;
		case 'R':
			param->raw = 1;
			break;
		default:
			return CMD_HELP;
		}
	}

	if (optind >= argc)
		return CMD_HELP;

	if (param->recursive)
		param->maxdepth = -1;

	if (!param->verbose)
		param->verbose = VERBOSE_ALL;
	if (param->quiet)
		param->verbose = VERBOSE_OBJID;

	do {
		rc = llapi_getstripe(argv[optind], param);
	} while (++optind < argc && !rc);

	if (rc)
		fprintf(stderr, "error: %s failed for %s.\n",
			argv[0], argv[optind - 1]);
	return rc;
}

static int lfs_tgts(int argc, char **argv)
{
        char mntdir[PATH_MAX] = {'\0'}, path[PATH_MAX] = {'\0'};
        struct find_param param;
        int index = 0, rc=0;

        if (argc > 2)
                return CMD_HELP;

        if (argc == 2 && !realpath(argv[1], path)) {
                rc = -errno;
                fprintf(stderr, "error: invalid path '%s': %s\n",
                        argv[1], strerror(-rc));
                return rc;
        }

        while (!llapi_search_mounts(path, index++, mntdir, NULL)) {
                /* Check if we have a mount point */
                if (mntdir[0] == '\0')
                        continue;

                memset(&param, 0, sizeof(param));
                if (!strcmp(argv[0], "mdts"))
                        param.get_lmv = 1;

                rc = llapi_ostlist(mntdir, &param);
                if (rc) {
                        fprintf(stderr, "error: %s: failed on %s\n",
                                argv[0], mntdir);
                }
                if (path[0] != '\0')
                        break;
                memset(mntdir, 0, PATH_MAX);
        }

        return rc;
}

static int lfs_getstripe(int argc, char **argv)
{
	struct find_param param = { 0 };
	return lfs_getstripe_internal(argc, argv, &param);
}

/* functions */
static int lfs_getdirstripe(int argc, char **argv)
{
	struct find_param param = { 0 };

	param.get_lmv = 1;
	return lfs_getstripe_internal(argc, argv, &param);
}

/* functions */
static int lfs_setdirstripe(int argc, char **argv)
{
	char *dname;
	int result;
	int  st_offset, st_count;
	char *end;
	int c;
	char *stripe_off_arg = NULL;
	int  flags = 0;

	struct option long_opts[] = {
		{"index",    required_argument, 0, 'i'},
		{0, 0, 0, 0}
	};

	st_offset = -1;
	st_count = 1;
	while ((c = getopt_long(argc, argv, "i:o",
				long_opts, NULL)) >= 0) {
		switch (c) {
		case 0:
			/* Long options. */
			break;
		case 'i':
			stripe_off_arg = optarg;
			break;
		default:
			fprintf(stderr, "error: %s: option '%s' "
					"unrecognized\n",
					argv[0], argv[optind - 1]);
			return CMD_HELP;
		}
	}

	if (optind == argc) {
		fprintf(stderr, "error: %s: missing dirname\n",
			argv[0]);
		return CMD_HELP;
	}

	dname = argv[optind];
	if (stripe_off_arg == NULL) {
		fprintf(stderr, "error: %s: missing stripe_off.\n",
			argv[0]);
		return CMD_HELP;
	}
	/* get the stripe offset */
	st_offset = strtoul(stripe_off_arg, &end, 0);
	if (*end != '\0') {
		fprintf(stderr, "error: %s: bad stripe offset '%s'\n",
			argv[0], stripe_off_arg);
		return CMD_HELP;
	}
	do {
		result = llapi_dir_create_pool(dname, flags, st_offset,
					       st_count, 0, NULL);
		if (result) {
			fprintf(stderr, "error: %s: create stripe dir '%s' "
				"failed\n", argv[0], dname);
			break;
		}
		dname = argv[++optind];
	} while (dname != NULL);

	return result;
}

/* functions */
static int lfs_rmentry(int argc, char **argv)
{
	char *dname;
	int   index;
	int   result = 0;

	if (argc <= 1) {
		fprintf(stderr, "error: %s: missing dirname\n",
			argv[0]);
		return CMD_HELP;
	}

	index = 1;
	dname = argv[index];
	while (dname != NULL) {
		result = llapi_direntry_remove(dname);
		if (result) {
			fprintf(stderr, "error: %s: remove dir entry '%s' "
				"failed\n", argv[0], dname);
			break;
		}
		dname = argv[++index];
	}
	return result;
}

static int lfs_osts(int argc, char **argv)
{
        return lfs_tgts(argc, argv);
}

static int lfs_mdts(int argc, char **argv)
{
        return lfs_tgts(argc, argv);
}

#define COOK(value)                                                     \
({                                                                      \
        int radix = 0;                                                  \
        while (value > 1024) {                                          \
                value /= 1024;                                          \
                radix++;                                                \
        }                                                               \
        radix;                                                          \
})
#define UUF     "%-20s"
#define CSF     "%11s"
#define CDF     "%11llu"
#define HDF     "%8.1f%c"
#define RSF     "%4s"
#define RDF     "%3d%%"

static int showdf(char *mntdir, struct obd_statfs *stat,
                  char *uuid, int ishow, int cooked,
                  char *type, int index, int rc)
{
        long long avail, used, total;
        double ratio = 0;
        char *suffix = "KMGTPEZY";
        /* Note if we have >2^64 bytes/fs these buffers will need to be grown */
        char tbuf[20], ubuf[20], abuf[20], rbuf[20];

        if (!uuid || !stat)
                return -EINVAL;

        switch (rc) {
        case 0:
                if (ishow) {
                        avail = stat->os_ffree;
                        used = stat->os_files - stat->os_ffree;
                        total = stat->os_files;
                } else {
                        int shift = cooked ? 0 : 10;

                        avail = (stat->os_bavail * stat->os_bsize) >> shift;
                        used  = ((stat->os_blocks - stat->os_bfree) *
                                 stat->os_bsize) >> shift;
                        total = (stat->os_blocks * stat->os_bsize) >> shift;
                }

                if ((used + avail) > 0)
                        ratio = (double)used / (double)(used + avail);

                if (cooked) {
                        int i;
                        double cook_val;

                        cook_val = (double)total;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(tbuf, HDF, cook_val, suffix[i - 1]);
                        else
                                sprintf(tbuf, CDF, total);

                        cook_val = (double)used;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(ubuf, HDF, cook_val, suffix[i - 1]);
                        else
                                sprintf(ubuf, CDF, used);

                        cook_val = (double)avail;
                        i = COOK(cook_val);
                        if (i > 0)
                                sprintf(abuf, HDF, cook_val, suffix[i - 1]);
                        else
                                sprintf(abuf, CDF, avail);
                } else {
                        sprintf(tbuf, CDF, total);
                        sprintf(ubuf, CDF, used);
                        sprintf(abuf, CDF, avail);
                }

                sprintf(rbuf, RDF, (int)(ratio * 100 + 0.5));
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s",
                       uuid, tbuf, ubuf, abuf, rbuf, mntdir);
                if (type)
                        printf("[%s:%d]\n", type, index);
                else
                        printf("\n");

                break;
        case -ENODATA:
                printf(UUF": inactive device\n", uuid);
                break;
        default:
                printf(UUF": %s\n", uuid, strerror(-rc));
                break;
        }

        return 0;
}

struct ll_stat_type {
        int   st_op;
        char *st_name;
};

static int mntdf(char *mntdir, char *fsname, char *pool, int ishow,
		int cooked, int lazy)
{
        struct obd_statfs stat_buf, sum = { .os_bsize = 1 };
        struct obd_uuid uuid_buf;
        char *poolname = NULL;
        struct ll_stat_type types[] = { { LL_STATFS_LMV, "MDT" },
                                        { LL_STATFS_LOV, "OST" },
                                        { 0, NULL } };
        struct ll_stat_type *tp;
        __u32 index;
        __u32 type;
        int rc;

        if (pool) {
                poolname = strchr(pool, '.');
                if (poolname != NULL) {
                        if (strncmp(fsname, pool, strlen(fsname))) {
                                fprintf(stderr, "filesystem name incorrect\n");
                                return -ENODEV;
                        }
                        poolname++;
                } else
                        poolname = pool;
        }

        if (ishow)
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
                       "UUID", "Inodes", "IUsed", "IFree",
                       "IUse%", "Mounted on");
        else
                printf(UUF" "CSF" "CSF" "CSF" "RSF" %-s\n",
                       "UUID", cooked ? "bytes" : "1K-blocks",
                       "Used", "Available", "Use%", "Mounted on");

        for (tp = types; tp->st_name != NULL; tp++) {
                for (index = 0; ; index++) {
                        memset(&stat_buf, 0, sizeof(struct obd_statfs));
                        memset(&uuid_buf, 0, sizeof(struct obd_uuid));
			type = lazy ? tp->st_op | LL_STATFS_NODELAY : tp->st_op;
			rc = llapi_obd_statfs(mntdir, type, index,
                                              &stat_buf, &uuid_buf);
                        if (rc == -ENODEV)
                                break;

                        if (poolname && tp->st_op == LL_STATFS_LOV &&
                            llapi_search_ost(fsname, poolname,
                                             obd_uuid2str(&uuid_buf)) != 1)
                                continue;

                        /* the llapi_obd_statfs() call may have returned with
                         * an error, but if it filled in uuid_buf we will at
                         * lease use that to print out a message for that OBD.
                         * If we didn't get anything in the uuid_buf, then fill
                         * it in so that we can print an error message. */
                        if (uuid_buf.uuid[0] == '\0')
                                sprintf(uuid_buf.uuid, "%s%04x",
                                        tp->st_name, index);
                        showdf(mntdir, &stat_buf, obd_uuid2str(&uuid_buf),
                               ishow, cooked, tp->st_name, index, rc);

                        if (rc == 0) {
                                if (tp->st_op == LL_STATFS_LMV) {
                                        sum.os_ffree += stat_buf.os_ffree;
                                        sum.os_files += stat_buf.os_files;
                                } else /* if (tp->st_op == LL_STATFS_LOV) */ {
                                        sum.os_blocks += stat_buf.os_blocks *
                                                stat_buf.os_bsize;
                                        sum.os_bfree  += stat_buf.os_bfree *
                                                stat_buf.os_bsize;
                                        sum.os_bavail += stat_buf.os_bavail *
                                                stat_buf.os_bsize;
                                }
                        } else if (rc == -EINVAL || rc == -EFAULT) {
                                break;
                        }
                }
        }

        printf("\n");
        showdf(mntdir, &sum, "filesystem summary:", ishow, cooked, NULL, 0,0);
        printf("\n");
        return 0;
}

static int lfs_df(int argc, char **argv)
{
        char mntdir[PATH_MAX] = {'\0'}, path[PATH_MAX] = {'\0'};
        int ishow = 0, cooked = 0;
	int lazy = 0;
        int c, rc = 0, index = 0;
        char fsname[PATH_MAX] = "", *pool_name = NULL;
        struct option long_opts[] = {
                {"pool", required_argument, 0, 'p'},
                {"lazy", 0, 0, 'l'},
                {0, 0, 0, 0}
        };

	while ((c = getopt_long(argc, argv, "hilp:", long_opts, NULL)) != -1) {
		switch (c) {
		case 'i':
			ishow = 1;
			break;
		case 'h':
			cooked = 1;
			break;
		case 'l':
			lazy = 1;
			break;
		case 'p':
			pool_name = optarg;
			break;
		default:
			return CMD_HELP;
		}
	}
        if (optind < argc && !realpath(argv[optind], path)) {
                rc = -errno;
                fprintf(stderr, "error: invalid path '%s': %s\n",
                        argv[optind], strerror(-rc));
                return rc;
        }

        while (!llapi_search_mounts(path, index++, mntdir, fsname)) {
                /* Check if we have a mount point */
                if (mntdir[0] == '\0')
                        continue;

		rc = mntdf(mntdir, fsname, pool_name, ishow, cooked, lazy);
                if (rc || path[0] != '\0')
                        break;
                fsname[0] = '\0'; /* avoid matching in next loop */
                mntdir[0] = '\0'; /* avoid matching in next loop */
        }

        return rc;
}

static int lfs_getname(int argc, char **argv)
{
        char mntdir[PATH_MAX] = "", path[PATH_MAX] = "", fsname[PATH_MAX] = "";
        int rc = 0, index = 0, c;
        char buf[sizeof(struct obd_uuid)];

        while ((c = getopt(argc, argv, "h")) != -1)
                return CMD_HELP;

        if (optind == argc) { /* no paths specified, get all paths. */
                while (!llapi_search_mounts(path, index++, mntdir, fsname)) {
                        rc = llapi_getname(mntdir, buf, sizeof(buf));
                        if (rc < 0) {
                                fprintf(stderr,
                                        "cannot get name for `%s': %s\n",
                                        mntdir, strerror(-rc));
                                break;
                        }

                        printf("%s %s\n", buf, mntdir);

                        path[0] = fsname[0] = mntdir[0] = 0;
                }
        } else { /* paths specified, only attempt to search these. */
                for (; optind < argc; optind++) {
                        rc = llapi_getname(argv[optind], buf, sizeof(buf));
                        if (rc < 0) {
                                fprintf(stderr,
                                        "cannot get name for `%s': %s\n",
                                        argv[optind], strerror(-rc));
                                break;
                        }

                        printf("%s %s\n", buf, argv[optind]);
                }
        }
        return rc;
}

static int lfs_check(int argc, char **argv)
{
        int rc;
        char mntdir[PATH_MAX] = {'\0'};
        int num_types = 1;
        char *obd_types[2];
        char obd_type1[4];
        char obd_type2[4];

        if (argc != 2)
                return CMD_HELP;

        obd_types[0] = obd_type1;
        obd_types[1] = obd_type2;

        if (strcmp(argv[1], "osts") == 0) {
                strcpy(obd_types[0], "osc");
        } else if (strcmp(argv[1], "mds") == 0) {
                strcpy(obd_types[0], "mdc");
        } else if (strcmp(argv[1], "servers") == 0) {
                num_types = 2;
                strcpy(obd_types[0], "osc");
                strcpy(obd_types[1], "mdc");
        } else {
                fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[1]);
                        return CMD_HELP;
        }

        rc = llapi_search_mounts(NULL, 0, mntdir, NULL);
        if (rc < 0 || mntdir[0] == '\0') {
                fprintf(stderr, "No suitable Lustre mount found\n");
                return rc;
        }

        rc = llapi_target_iterate(num_types, obd_types,
                                  mntdir, llapi_ping_target);

        if (rc)
                fprintf(stderr, "error: %s: %s status failed\n",
                                argv[0],argv[1]);

        return rc;

}

static int lfs_join(int argc, char **argv)
{
        fprintf(stderr, "join two lustre files into one.\n"
                        "obsolete, HEAD does not support it anymore.\n");
        return 0;
}

#ifdef HAVE_SYS_QUOTA_H
static int lfs_quotacheck(int argc, char **argv)
{
        int c, check_type = 0;
        char *mnt;
        struct if_quotacheck qchk;
        struct if_quotactl qctl;
        char *obd_type = (char *)qchk.obd_type;
        int rc;

        memset(&qchk, 0, sizeof(qchk));

        while ((c = getopt(argc, argv, "gu")) != -1) {
                switch (c) {
                case 'u':
                        check_type |= 0x01;
                        break;
                case 'g':
                        check_type |= 0x02;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (check_type)
                check_type--;
        else    /* do quotacheck for both user & group quota by default */
                check_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotacheck(mnt, check_type);
	if (rc == -EOPNOTSUPP) {
		fprintf(stderr, "error: quotacheck not supported by the quota "
			"master.\nPlease note that quotacheck is deprecated as "
			"of lustre 2.4.0 since space accounting is always "
			"enabled.\nFilesystems not formatted with 2.4 utils or "
			"beyond can be upgraded with tunefs.lustre --quota.\n");
		return rc;
	} else if (rc) {
                fprintf(stderr, "quotacheck failed: %s\n", strerror(-rc));
                return rc;
        }

        rc = llapi_poll_quotacheck(mnt, &qchk);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qchk.obd_uuid));
                fprintf(stderr, "quota check failed: %s\n", strerror(-rc));
                return rc;
        }

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAON;
        qctl.qc_type = check_type;
        rc = llapi_quotactl(mnt, &qctl);
        if (rc && rc != -EALREADY) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", (char *)qctl.obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
                fprintf(stderr, "%s turn on quota failed: %s\n",
                        argv[0], strerror(-rc));
                return rc;
        }

        return 0;
}

static int lfs_quotaon(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        char *obd_type = (char *)qctl.obd_type;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAON;

        while ((c = getopt(argc, argv, "fgu")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                case 'f':
                        qctl.qc_cmd = LUSTRE_Q_QUOTAOFF;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;
        else /* by default, enable quota for both user & group */
                qctl.qc_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (rc == -EOPNOTSUPP) {
                        fprintf(stderr, "error: quotaon not supported by the "
                                "quota master.\nPlease note that quotaon/off is"
                                " deprecated as of lustre 2.4.0.\nQuota "
                                "enforcement should now be enabled on the MGS "
                                "via:\nmgs# lctl conf_param ${FSNAME}.quota."
                                "<ost|mdt>=<u|g|ug>\n(ost for block quota, mdt "
                                "for inode quota, u for user and g for group"
                                "\n");
                } else if (rc == -EALREADY) {
                        rc = 0;
                } else if (rc == -ENOENT) {
                        fprintf(stderr, "error: cannot find quota database, "
                                        "make sure you have run quotacheck\n");
                } else {
                        if (*obd_type)
                                fprintf(stderr, "%s %s ", obd_type,
                                        obd_uuid2str(&qctl.obd_uuid));
                        fprintf(stderr, "%s failed: %s\n", argv[0],
                                strerror(-rc));
                }
        }

        return rc;
}

static int lfs_quotaoff(int argc, char **argv)
{
        int c;
        char *mnt;
        struct if_quotactl qctl;
        char *obd_type = (char *)qctl.obd_type;
        int rc;

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd = LUSTRE_Q_QUOTAOFF;

        while ((c = getopt(argc, argv, "gu")) != -1) {
                switch (c) {
                case 'u':
                        qctl.qc_type |= 0x01;
                        break;
                case 'g':
                        qctl.qc_type |= 0x02;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type)
                qctl.qc_type--;
        else /* by default, disable quota for both user & group */
                qctl.qc_type = 0x02;

        if (argc == optind)
                return CMD_HELP;

        mnt = argv[optind];

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (rc == -EOPNOTSUPP) {
                        fprintf(stderr, "error: quotaoff not supported by the "
                                "quota master.\nPlease note that quotaon/off is"
                                " deprecated as of lustre 2.4.0.\nQuota "
                                "enforcement can be disabled on the MGS via:\n"
                                "mgs# lctl conf_param ${FSNAME}.quota.<ost|mdt>"
                                "=\"\"\n");
                } else if (rc == -EALREADY) {
                        rc = 0;
                } else {
                        if (*obd_type)
                                fprintf(stderr, "%s %s ", obd_type,
                                        obd_uuid2str(&qctl.obd_uuid));
                        fprintf(stderr, "quotaoff failed: %s\n",
                                strerror(-rc));
                }
        }

        return rc;
}

#define ARG2INT(nr, str, msg)                                           \
do {                                                                    \
        char *endp;                                                     \
        nr = strtol(str, &endp, 0);                                     \
        if (*endp) {                                                    \
                fprintf(stderr, "error: bad %s: %s\n", msg, str);       \
                return CMD_HELP;                                        \
        }                                                               \
} while (0)

#define ADD_OVERFLOW(a,b) ((a + b) < a) ? (a = ULONG_MAX) : (a = a + b)

/* Convert format time string "XXwXXdXXhXXmXXs" into seconds value
 * returns the value or ULONG_MAX on integer overflow or incorrect format
 * Notes:
 *        1. the order of specifiers is arbitrary (may be: 5w3s or 3s5w)
 *        2. specifiers may be encountered multiple times (2s3s is 5 seconds)
 *        3. empty integer value is interpreted as 0
 */
static unsigned long str2sec(const char* timestr)
{
        const char spec[] = "smhdw";
        const unsigned long mult[] = {1, 60, 60*60, 24*60*60, 7*24*60*60};
        unsigned long val = 0;
        char *tail;

        if (strpbrk(timestr, spec) == NULL) {
                /* no specifiers inside the time string,
                   should treat it as an integer value */
                val = strtoul(timestr, &tail, 10);
                return *tail ? ULONG_MAX : val;
        }

        /* format string is XXwXXdXXhXXmXXs */
        while (*timestr) {
                unsigned long v;
                int ind;
                char* ptr;

                v = strtoul(timestr, &tail, 10);
                if (v == ULONG_MAX || *tail == '\0')
                        /* value too large (ULONG_MAX or more)
                           or missing specifier */
                        goto error;

                ptr = strchr(spec, *tail);
                if (ptr == NULL)
                        /* unknown specifier */
                        goto error;

                ind = ptr - spec;

                /* check if product will overflow the type */
                if (!(v < ULONG_MAX / mult[ind]))
                        goto error;

                ADD_OVERFLOW(val, mult[ind] * v);
                if (val == ULONG_MAX)
                        goto error;

                timestr = tail + 1;
        }

        return val;

error:
        return ULONG_MAX;
}

#define ARG2ULL(nr, str, def_units)					\
do {									\
	unsigned long long limit, units = def_units;			\
	int rc;								\
									\
	rc = llapi_parse_size(str, &limit, &units, 1);			\
	if (rc < 0) {							\
		fprintf(stderr, "error: bad limit value %s\n", str);	\
		return CMD_HELP;					\
	}								\
	nr = limit;							\
} while (0)

static inline int has_times_option(int argc, char **argv)
{
        int i;

        for (i = 1; i < argc; i++)
                if (!strcmp(argv[i], "-t"))
                        return 1;

        return 0;
}

int lfs_setquota_times(int argc, char **argv)
{
        int c, rc;
        struct if_quotactl qctl;
        char *mnt, *obd_type = (char *)qctl.obd_type;
        struct obd_dqblk *dqb = &qctl.qc_dqblk;
        struct obd_dqinfo *dqi = &qctl.qc_dqinfo;
        struct option long_opts[] = {
                {"block-grace",     required_argument, 0, 'b'},
                {"group",           no_argument,       0, 'g'},
                {"inode-grace",     required_argument, 0, 'i'},
                {"times",           no_argument,       0, 't'},
                {"user",            no_argument,       0, 'u'},
                {0, 0, 0, 0}
        };

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd  = LUSTRE_Q_SETINFO;
        qctl.qc_type = UGQUOTA;

        while ((c = getopt_long(argc, argv, "b:gi:tu", long_opts, NULL)) != -1) {
                switch (c) {
                case 'u':
                case 'g':
                        if (qctl.qc_type != UGQUOTA) {
                                fprintf(stderr, "error: -u and -g can't be used "
                                                "more than once\n");
                                return CMD_HELP;
                        }
                        qctl.qc_type = (c == 'u') ? USRQUOTA : GRPQUOTA;
                        break;
                case 'b':
                        if ((dqi->dqi_bgrace = str2sec(optarg)) == ULONG_MAX) {
                                fprintf(stderr, "error: bad block-grace: %s\n",
                                        optarg);
                                return CMD_HELP;
                        }
                        dqb->dqb_valid |= QIF_BTIME;
                        break;
                case 'i':
                        if ((dqi->dqi_igrace = str2sec(optarg)) == ULONG_MAX) {
                                fprintf(stderr, "error: bad inode-grace: %s\n",
                                        optarg);
                                return CMD_HELP;
                        }
                        dqb->dqb_valid |= QIF_ITIME;
                        break;
                case 't': /* Yes, of course! */
                        break;
                default: /* getopt prints error message for us when opterr != 0 */
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type == UGQUOTA) {
                fprintf(stderr, "error: neither -u nor -g specified\n");
                return CMD_HELP;
        }

        if (optind != argc - 1) {
                fprintf(stderr, "error: unexpected parameters encountered\n");
                return CMD_HELP;
        }

        mnt = argv[optind];
        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
                fprintf(stderr, "setquota failed: %s\n", strerror(-rc));
                return rc;
        }

        return 0;
}

#define BSLIMIT (1 << 0)
#define BHLIMIT (1 << 1)
#define ISLIMIT (1 << 2)
#define IHLIMIT (1 << 3)

int lfs_setquota(int argc, char **argv)
{
        int c, rc;
        struct if_quotactl qctl;
        char *mnt, *obd_type = (char *)qctl.obd_type;
        struct obd_dqblk *dqb = &qctl.qc_dqblk;
        struct option long_opts[] = {
                {"block-softlimit", required_argument, 0, 'b'},
                {"block-hardlimit", required_argument, 0, 'B'},
                {"group",           required_argument, 0, 'g'},
                {"inode-softlimit", required_argument, 0, 'i'},
                {"inode-hardlimit", required_argument, 0, 'I'},
                {"user",            required_argument, 0, 'u'},
                {0, 0, 0, 0}
        };
        unsigned limit_mask = 0;
        char *endptr;

        if (has_times_option(argc, argv))
                return lfs_setquota_times(argc, argv);

        memset(&qctl, 0, sizeof(qctl));
        qctl.qc_cmd  = LUSTRE_Q_SETQUOTA;
        qctl.qc_type = UGQUOTA; /* UGQUOTA makes no sense for setquota,
                                 * so it can be used as a marker that qc_type
                                 * isn't reinitialized from command line */

        while ((c = getopt_long(argc, argv, "b:B:g:i:I:u:", long_opts, NULL)) != -1) {
                switch (c) {
                case 'u':
                case 'g':
                        if (qctl.qc_type != UGQUOTA) {
                                fprintf(stderr, "error: -u and -g can't be used"
                                                " more than once\n");
                                return CMD_HELP;
                        }
                        qctl.qc_type = (c == 'u') ? USRQUOTA : GRPQUOTA;
                        rc = name2id(&qctl.qc_id, optarg,
                                     (qctl.qc_type == USRQUOTA) ? USER : GROUP);
                        if (rc) {
                                qctl.qc_id = strtoul(optarg, &endptr, 10);
                                if (*endptr != '\0') {
                                        fprintf(stderr, "error: can't find id "
                                                "for name %s\n", optarg);
                                        return CMD_HELP;
                                }
                        }
                        break;
                case 'b':
                        ARG2ULL(dqb->dqb_bsoftlimit, optarg, 1024);
                        dqb->dqb_bsoftlimit >>= 10;
                        limit_mask |= BSLIMIT;
			if (dqb->dqb_bsoftlimit &&
			    dqb->dqb_bsoftlimit <= 1024) /* <= 1M? */
				fprintf(stderr, "warning: block softlimit is "
					"smaller than the miminal qunit size, "
					"please see the help of setquota or "
					"Lustre manual for details.\n");
                        break;
                case 'B':
                        ARG2ULL(dqb->dqb_bhardlimit, optarg, 1024);
                        dqb->dqb_bhardlimit >>= 10;
                        limit_mask |= BHLIMIT;
			if (dqb->dqb_bhardlimit &&
			    dqb->dqb_bhardlimit <= 1024) /* <= 1M? */
				fprintf(stderr, "warning: block hardlimit is "
					"smaller than the miminal qunit size, "
					"please see the help of setquota or "
					"Lustre manual for details.\n");
                        break;
                case 'i':
                        ARG2ULL(dqb->dqb_isoftlimit, optarg, 1);
                        limit_mask |= ISLIMIT;
			if (dqb->dqb_isoftlimit &&
			    dqb->dqb_isoftlimit <= 1024) /* <= 1K inodes? */
				fprintf(stderr, "warning: inode softlimit is "
					"smaller than the miminal qunit size, "
					"please see the help of setquota or "
					"Lustre manual for details.\n");
                        break;
                case 'I':
                        ARG2ULL(dqb->dqb_ihardlimit, optarg, 1);
                        limit_mask |= IHLIMIT;
			if (dqb->dqb_ihardlimit &&
			    dqb->dqb_ihardlimit <= 1024) /* <= 1K inodes? */
				fprintf(stderr, "warning: inode hardlimit is "
					"smaller than the miminal qunit size, "
					"please see the help of setquota or "
					"Lustre manual for details.\n");
                        break;
                default: /* getopt prints error message for us when opterr != 0 */
                        return CMD_HELP;
                }
        }

        if (qctl.qc_type == UGQUOTA) {
                fprintf(stderr, "error: neither -u nor -g was specified\n");
                return CMD_HELP;
        }

        if (limit_mask == 0) {
                fprintf(stderr, "error: at least one limit must be specified\n");
                return CMD_HELP;
        }

        if (optind != argc - 1) {
                fprintf(stderr, "error: unexpected parameters encountered\n");
                return CMD_HELP;
        }

        mnt = argv[optind];

        if ((!(limit_mask & BHLIMIT) ^ !(limit_mask & BSLIMIT)) ||
            (!(limit_mask & IHLIMIT) ^ !(limit_mask & ISLIMIT))) {
                /* sigh, we can't just set blimits/ilimits */
                struct if_quotactl tmp_qctl = {.qc_cmd  = LUSTRE_Q_GETQUOTA,
                                               .qc_type = qctl.qc_type,
                                               .qc_id   = qctl.qc_id};

                rc = llapi_quotactl(mnt, &tmp_qctl);
                if (rc < 0) {
                        fprintf(stderr, "error: setquota failed while retrieving"
                                        " current quota settings (%s)\n",
                                        strerror(-rc));
                        return rc;
                }

                if (!(limit_mask & BHLIMIT))
                        dqb->dqb_bhardlimit = tmp_qctl.qc_dqblk.dqb_bhardlimit;
                if (!(limit_mask & BSLIMIT))
                        dqb->dqb_bsoftlimit = tmp_qctl.qc_dqblk.dqb_bsoftlimit;
                if (!(limit_mask & IHLIMIT))
                        dqb->dqb_ihardlimit = tmp_qctl.qc_dqblk.dqb_ihardlimit;
                if (!(limit_mask & ISLIMIT))
                        dqb->dqb_isoftlimit = tmp_qctl.qc_dqblk.dqb_isoftlimit;

                /* Keep grace times if we have got no softlimit arguments */
                if ((limit_mask & BHLIMIT) && !(limit_mask & BSLIMIT)) {
                        dqb->dqb_valid |= QIF_BTIME;
                        dqb->dqb_btime = tmp_qctl.qc_dqblk.dqb_btime;
                }

                if ((limit_mask & IHLIMIT) && !(limit_mask & ISLIMIT)) {
                        dqb->dqb_valid |= QIF_ITIME;
                        dqb->dqb_itime = tmp_qctl.qc_dqblk.dqb_itime;
                }
        }

        dqb->dqb_valid |= (limit_mask & (BHLIMIT | BSLIMIT)) ? QIF_BLIMITS : 0;
        dqb->dqb_valid |= (limit_mask & (IHLIMIT | ISLIMIT)) ? QIF_ILIMITS : 0;

        rc = llapi_quotactl(mnt, &qctl);
        if (rc) {
                if (*obd_type)
                        fprintf(stderr, "%s %s ", obd_type,
                                obd_uuid2str(&qctl.obd_uuid));
                fprintf(stderr, "setquota failed: %s\n", strerror(-rc));
                return rc;
        }

        return 0;
}

static inline char *type2name(int check_type)
{
        if (check_type == USRQUOTA)
                return "user";
        else if (check_type == GRPQUOTA)
                return "group";
        else
                return "unknown";
}

/* Converts seconds value into format string
 * result is returned in buf
 * Notes:
 *        1. result is in descenting order: 1w2d3h4m5s
 *        2. zero fields are not filled (except for p. 3): 5d1s
 *        3. zero seconds value is presented as "0s"
 */
static char * __sec2str(time_t seconds, char *buf)
{
        const char spec[] = "smhdw";
        const unsigned long mult[] = {1, 60, 60*60, 24*60*60, 7*24*60*60};
        unsigned long c;
        char *tail = buf;
        int i;

        for (i = sizeof(mult) / sizeof(mult[0]) - 1 ; i >= 0; i--) {
                c = seconds / mult[i];

                if (c > 0 || (i == 0 && buf == tail))
                        tail += snprintf(tail, 40-(tail-buf), "%lu%c", c, spec[i]);

                seconds %= mult[i];
        }

        return tail;
}

static void sec2str(time_t seconds, char *buf, int rc)
{
        char *tail = buf;

        if (rc)
                *tail++ = '[';

        tail = __sec2str(seconds, tail);

        if (rc && tail - buf < 39) {
                *tail++ = ']';
                *tail++ = 0;
        }
}

static void diff2str(time_t seconds, char *buf, time_t now)
{

        buf[0] = 0;
        if (!seconds)
                return;
        if (seconds <= now) {
                strcpy(buf, "none");
                return;
        }
        __sec2str(seconds - now, buf);
}

static void print_quota_title(char *name, struct if_quotactl *qctl,
			      bool human_readable)
{
	printf("Disk quotas for %s %s (%cid %u):\n",
	       type2name(qctl->qc_type), name,
	       *type2name(qctl->qc_type), qctl->qc_id);
	printf("%15s%8s %7s%8s%8s%8s %7s%8s%8s\n",
	       "Filesystem", human_readable ? "used" : "kbytes",
	       "quota", "limit", "grace",
	       "files", "quota", "limit", "grace");
}

static void kbytes2str(__u64 num, char *buf, bool h)
{
	if (!h) {
		sprintf(buf, LPU64, num);
	} else {
		if (num >> 30)
			sprintf(buf, "%5.4gT", (double)num / (1 << 30));
		else if (num >> 20)
			sprintf(buf, "%5.4gG", (double)num / (1 << 20));
		else if (num >> 10)
			sprintf(buf, "%5.4gM", (double)num / (1 << 10));
		else
			sprintf(buf, LPU64"%s", num, "k");
	}
}

static void print_quota(char *mnt, struct if_quotactl *qctl, int type,
			int rc, bool h)
{
        time_t now;

        time(&now);

        if (qctl->qc_cmd == LUSTRE_Q_GETQUOTA || qctl->qc_cmd == Q_GETOQUOTA) {
		int bover = 0, iover = 0;
		struct obd_dqblk *dqb = &qctl->qc_dqblk;
		char numbuf[3][32];
		char timebuf[40];
		char strbuf[32];

                if (dqb->dqb_bhardlimit &&
                    toqb(dqb->dqb_curspace) >= dqb->dqb_bhardlimit) {
                        bover = 1;
                } else if (dqb->dqb_bsoftlimit && dqb->dqb_btime) {
                        if (dqb->dqb_btime > now) {
                                bover = 2;
                        } else {
                                bover = 3;
                        }
                }

                if (dqb->dqb_ihardlimit &&
                    dqb->dqb_curinodes >= dqb->dqb_ihardlimit) {
                        iover = 1;
                } else if (dqb->dqb_isoftlimit && dqb->dqb_itime) {
			if (dqb->dqb_itime > now) {
				iover = 2;
			} else {
				iover = 3;
			}
                }


		if (strlen(mnt) > 15)
			printf("%s\n%15s", mnt, "");
		else
			printf("%15s", mnt);

		if (bover)
			diff2str(dqb->dqb_btime, timebuf, now);

		kbytes2str(toqb(dqb->dqb_curspace), strbuf, h);
		if (rc == -EREMOTEIO)
			sprintf(numbuf[0], "%s*", strbuf);
		else
			sprintf(numbuf[0], (dqb->dqb_valid & QIF_SPACE) ?
				"%s" : "[%s]", strbuf);

		kbytes2str(dqb->dqb_bsoftlimit, strbuf, h);
		if (type == QC_GENERAL)
			sprintf(numbuf[1], (dqb->dqb_valid & QIF_BLIMITS) ?
				"%s" : "[%s]", strbuf);
		else
			sprintf(numbuf[1], "%s", "-");

		kbytes2str(dqb->dqb_bhardlimit, strbuf, h);
		sprintf(numbuf[2], (dqb->dqb_valid & QIF_BLIMITS) ?
			"%s" : "[%s]", strbuf);

		printf(" %7s%c %6s %7s %7s",
		       numbuf[0], bover ? '*' : ' ', numbuf[1],
		       numbuf[2], bover > 1 ? timebuf : "-");

		if (iover)
			diff2str(dqb->dqb_itime, timebuf, now);

		sprintf(numbuf[0], (dqb->dqb_valid & QIF_INODES) ?
			LPU64 : "["LPU64"]", dqb->dqb_curinodes);

		if (type == QC_GENERAL)
			sprintf(numbuf[1], (dqb->dqb_valid & QIF_ILIMITS) ?
				LPU64 : "["LPU64"]", dqb->dqb_isoftlimit);
		else
			sprintf(numbuf[1], "%s", "-");

		sprintf(numbuf[2], (dqb->dqb_valid & QIF_ILIMITS) ?
			LPU64 : "["LPU64"]", dqb->dqb_ihardlimit);

		if (type != QC_OSTIDX)
			printf(" %7s%c %6s %7s %7s",
			       numbuf[0], iover ? '*' : ' ', numbuf[1],
			       numbuf[2], iover > 1 ? timebuf : "-");
		else
			printf(" %7s %7s %7s %7s", "-", "-", "-", "-");
		printf("\n");

        } else if (qctl->qc_cmd == LUSTRE_Q_GETINFO ||
                   qctl->qc_cmd == Q_GETOINFO) {
                char bgtimebuf[40];
                char igtimebuf[40];

                sec2str(qctl->qc_dqinfo.dqi_bgrace, bgtimebuf, rc);
                sec2str(qctl->qc_dqinfo.dqi_igrace, igtimebuf, rc);
                printf("Block grace time: %s; Inode grace time: %s\n",
                       bgtimebuf, igtimebuf);
        }
}

static int print_obd_quota(char *mnt, struct if_quotactl *qctl, int is_mdt,
			   bool h, __u64 *total)
{
        int rc = 0, rc1 = 0, count = 0;
        __u32 valid = qctl->qc_valid;

        rc = llapi_get_obd_count(mnt, &count, is_mdt);
        if (rc) {
                fprintf(stderr, "can not get %s count: %s\n",
                        is_mdt ? "mdt": "ost", strerror(-rc));
                return rc;
        }

        for (qctl->qc_idx = 0; qctl->qc_idx < count; qctl->qc_idx++) {
                qctl->qc_valid = is_mdt ? QC_MDTIDX : QC_OSTIDX;
                rc = llapi_quotactl(mnt, qctl);
                if (rc) {
                        /* It is remote client case. */
                        if (-rc == EOPNOTSUPP) {
                                rc = 0;
                                goto out;
                        }

                        if (!rc1)
                                rc1 = rc;
                        fprintf(stderr, "quotactl %s%d failed.\n",
                                is_mdt ? "mdt": "ost", qctl->qc_idx);
                        continue;
                }

		print_quota(obd_uuid2str(&qctl->obd_uuid), qctl,
			    qctl->qc_valid, 0, h);
		*total += is_mdt ? qctl->qc_dqblk.dqb_ihardlimit :
				   qctl->qc_dqblk.dqb_bhardlimit;
	}
out:
	qctl->qc_valid = valid;
	return rc ? : rc1;
}

static int lfs_quota(int argc, char **argv)
{
	int c;
	char *mnt, *name = NULL;
	struct if_quotactl qctl = { .qc_cmd = LUSTRE_Q_GETQUOTA,
				    .qc_type = UGQUOTA };
	char *obd_type = (char *)qctl.obd_type;
	char *obd_uuid = (char *)qctl.obd_uuid.uuid;
	int rc, rc1 = 0, rc2 = 0, rc3 = 0,
	    verbose = 0, pass = 0, quiet = 0, inacc;
	char *endptr;
	__u32 valid = QC_GENERAL, idx = 0;
	__u64 total_ialloc = 0, total_balloc = 0;
	bool human_readable = false;

	while ((c = getopt(argc, argv, "gi:I:o:qtuvh")) != -1) {
                switch (c) {
                case 'u':
                        if (qctl.qc_type != UGQUOTA) {
                                fprintf(stderr, "error: use either -u or -g\n");
                                return CMD_HELP;
                        }
                        qctl.qc_type = USRQUOTA;
                        break;
                case 'g':
                        if (qctl.qc_type != UGQUOTA) {
                                fprintf(stderr, "error: use either -u or -g\n");
                                return CMD_HELP;
                        }
                        qctl.qc_type = GRPQUOTA;
                        break;
                case 't':
                        qctl.qc_cmd = LUSTRE_Q_GETINFO;
                        break;
                case 'o':
                        valid = qctl.qc_valid = QC_UUID;
                        strncpy(obd_uuid, optarg, sizeof(qctl.obd_uuid));
                        break;
                case 'i':
                        valid = qctl.qc_valid = QC_MDTIDX;
                        idx = qctl.qc_idx = atoi(optarg);
                        break;
                case 'I':
                        valid = qctl.qc_valid = QC_OSTIDX;
                        idx = qctl.qc_idx = atoi(optarg);
                        break;
                case 'v':
                        verbose = 1;
                        break;
                case 'q':
                        quiet = 1;
                        break;
		case 'h':
			human_readable = true;
			break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        /* current uid/gid info for "lfs quota /path/to/lustre/mount" */
        if (qctl.qc_cmd == LUSTRE_Q_GETQUOTA && qctl.qc_type == UGQUOTA &&
            optind == argc - 1) {
ug_output:
                memset(&qctl, 0, sizeof(qctl)); /* spoiled by print_*_quota */
                qctl.qc_cmd = LUSTRE_Q_GETQUOTA;
                qctl.qc_valid = valid;
                qctl.qc_idx = idx;
                if (pass++ == 0) {
                        qctl.qc_type = USRQUOTA;
                        qctl.qc_id = geteuid();
                } else {
                        qctl.qc_type = GRPQUOTA;
                        qctl.qc_id = getegid();
                }
                rc = id2name(&name, qctl.qc_id,
                             (qctl.qc_type == USRQUOTA) ? USER : GROUP);
                if (rc)
                        name = "<unknown>";
        /* lfs quota -u username /path/to/lustre/mount */
        } else if (qctl.qc_cmd == LUSTRE_Q_GETQUOTA) {
                /* options should be followed by u/g-name and mntpoint */
                if (optind + 2 != argc || qctl.qc_type == UGQUOTA) {
                        fprintf(stderr, "error: missing quota argument(s)\n");
                        return CMD_HELP;
                }

                name = argv[optind++];
                rc = name2id(&qctl.qc_id, name,
                             (qctl.qc_type == USRQUOTA) ? USER : GROUP);
                if (rc) {
                        qctl.qc_id = strtoul(name, &endptr, 10);
                        if (*endptr != '\0') {
                                fprintf(stderr, "error: can't find id for name "
                                        "%s\n", name);
                                return CMD_HELP;
                        }
                }
        } else if (optind + 1 != argc || qctl.qc_type == UGQUOTA) {
                fprintf(stderr, "error: missing quota info argument(s)\n");
                return CMD_HELP;
        }

        mnt = argv[optind];

        rc1 = llapi_quotactl(mnt, &qctl);
        if (rc1 < 0) {
                switch (rc1) {
                case -ESRCH:
                        fprintf(stderr, "%s quotas are not enabled.\n",
                                qctl.qc_type == USRQUOTA ? "user" : "group");
                        goto out;
                case -EPERM:
                        fprintf(stderr, "Permission denied.\n");
                case -ENOENT:
                        /* We already got a "No such file..." message. */
                        goto out;
                default:
                        fprintf(stderr, "Unexpected quotactl error: %s\n",
                                strerror(-rc1));
                }
        }

	if (qctl.qc_cmd == LUSTRE_Q_GETQUOTA && !quiet)
		print_quota_title(name, &qctl, human_readable);

        if (rc1 && *obd_type)
                fprintf(stderr, "%s %s ", obd_type, obd_uuid);

        if (qctl.qc_valid != QC_GENERAL)
                mnt = "";

	inacc = (qctl.qc_cmd == LUSTRE_Q_GETQUOTA) &&
		((qctl.qc_dqblk.dqb_valid & (QIF_LIMITS|QIF_USAGE)) !=
		 (QIF_LIMITS|QIF_USAGE));

	print_quota(mnt, &qctl, QC_GENERAL, rc1, human_readable);

	if (qctl.qc_valid == QC_GENERAL && qctl.qc_cmd != LUSTRE_Q_GETINFO &&
	    verbose) {
		char strbuf[32];

		rc2 = print_obd_quota(mnt, &qctl, 1, human_readable,
				      &total_ialloc);
		rc3 = print_obd_quota(mnt, &qctl, 0, human_readable,
				      &total_balloc);
		kbytes2str(total_balloc, strbuf, human_readable);
		printf("Total allocated inode limit: "LPU64", total "
		       "allocated block limit: %s\n", total_ialloc, strbuf);
	}

        if (rc1 || rc2 || rc3 || inacc)
                printf("Some errors happened when getting quota info. "
                       "Some devices may be not working or deactivated. "
                       "The data in \"[]\" is inaccurate.\n");

out:
        if (pass == 1)
                goto ug_output;

        return rc1;
}
#endif /* HAVE_SYS_QUOTA_H! */

static int flushctx_ioctl(char *mp)
{
        int fd, rc;

        fd = open(mp, O_RDONLY);
        if (fd == -1) {
                fprintf(stderr, "flushctx: error open %s: %s\n",
                        mp, strerror(errno));
                return -1;
        }

        rc = ioctl(fd, LL_IOC_FLUSHCTX);
        if (rc == -1)
                fprintf(stderr, "flushctx: error ioctl %s: %s\n",
                        mp, strerror(errno));

        close(fd);
        return rc;
}

static int lfs_flushctx(int argc, char **argv)
{
        int     kdestroy = 0, c;
	FILE   *proc = NULL;
        char    procline[PATH_MAX], *line;
        int     rc = 0;

        while ((c = getopt(argc, argv, "k")) != -1) {
                switch (c) {
                case 'k':
                        kdestroy = 1;
                        break;
                default:
                        fprintf(stderr, "error: %s: option '-%c' "
                                        "unrecognized\n", argv[0], c);
                        return CMD_HELP;
                }
        }

        if (kdestroy) {
            int rc;
            if ((rc = system("kdestroy > /dev/null")) != 0) {
                rc = WEXITSTATUS(rc);
                fprintf(stderr, "error destroying tickets: %d, continuing\n", rc);
            }
        }

        if (optind >= argc) {
                /* flush for all mounted lustre fs. */
                proc = fopen("/proc/mounts", "r");
                if (!proc) {
                        fprintf(stderr, "error: %s: can't open /proc/mounts\n",
                                argv[0]);
                        return -1;
                }

                while ((line = fgets(procline, PATH_MAX, proc)) != NULL) {
                        char dev[PATH_MAX];
                        char mp[PATH_MAX];
                        char fs[PATH_MAX];

                        if (sscanf(line, "%s %s %s", dev, mp, fs) != 3) {
                                fprintf(stderr, "%s: unexpected format in "
                                                "/proc/mounts\n",
                                        argv[0]);
				rc = -1;
				goto out;
                        }

                        if (strcmp(fs, "lustre") != 0)
                                continue;
                        /* we use '@' to determine it's a client. are there
                         * any other better way?
                         */
                        if (strchr(dev, '@') == NULL)
                                continue;

                        if (flushctx_ioctl(mp))
                                rc = -1;
                }
        } else {
                /* flush fs as specified */
                while (optind < argc) {
                        if (flushctx_ioctl(argv[optind++]))
                                rc = -1;
                }
        }

out:
	if (proc != NULL)
		fclose(proc);
        return rc;
}

static int lfs_lsetfacl(int argc, char **argv)
{
        argv[0]++;
        return(llapi_lsetfacl(argc, argv));
}

static int lfs_lgetfacl(int argc, char **argv)
{
        argv[0]++;
        return(llapi_lgetfacl(argc, argv));
}

static int lfs_rsetfacl(int argc, char **argv)
{
        argv[0]++;
        return(llapi_rsetfacl(argc, argv));
}

static int lfs_rgetfacl(int argc, char **argv)
{
        argv[0]++;
        return(llapi_rgetfacl(argc, argv));
}

static int lfs_cp(int argc, char **argv)
{
        return(llapi_cp(argc, argv));
}

static int lfs_ls(int argc, char **argv)
{
        return(llapi_ls(argc, argv));
}

static int lfs_changelog(int argc, char **argv)
{
        void *changelog_priv;
	struct changelog_ext_rec *rec;
        long long startrec = 0, endrec = 0;
        char *mdd;
        struct option long_opts[] = {
                {"follow", no_argument, 0, 'f'},
                {0, 0, 0, 0}
        };
        char short_opts[] = "f";
        int rc, follow = 0;

        while ((rc = getopt_long(argc, argv, short_opts,
                                long_opts, NULL)) != -1) {
                switch (rc) {
                case 'f':
                        follow++;
                        break;
                case '?':
                        return CMD_HELP;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                }
        }
        if (optind >= argc)
                return CMD_HELP;

        mdd = argv[optind++];
        if (argc > optind)
                startrec = strtoll(argv[optind++], NULL, 10);
        if (argc > optind)
                endrec = strtoll(argv[optind++], NULL, 10);

        rc = llapi_changelog_start(&changelog_priv,
                                   CHANGELOG_FLAG_BLOCK |
                                   (follow ? CHANGELOG_FLAG_FOLLOW : 0),
                                   mdd, startrec);
        if (rc < 0) {
                fprintf(stderr, "Can't start changelog: %s\n",
                        strerror(errno = -rc));
                return rc;
        }

        while ((rc = llapi_changelog_recv(changelog_priv, &rec)) == 0) {
                time_t secs;
                struct tm ts;

                if (endrec && rec->cr_index > endrec) {
                        llapi_changelog_free(&rec);
                        break;
                }
                if (rec->cr_index < startrec) {
                        llapi_changelog_free(&rec);
                        continue;
                }

                secs = rec->cr_time >> 30;
                gmtime_r(&secs, &ts);
                printf(LPU64" %02d%-5s %02d:%02d:%02d.%06d %04d.%02d.%02d "
                       "0x%x t="DFID, rec->cr_index, rec->cr_type,
                       changelog_type2str(rec->cr_type),
                       ts.tm_hour, ts.tm_min, ts.tm_sec,
                       (int)(rec->cr_time & ((1<<30) - 1)),
                       ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday,
                       rec->cr_flags & CLF_FLAGMASK, PFID(&rec->cr_tfid));
                if (rec->cr_namelen)
                        /* namespace rec includes parent and filename */
			printf(" p="DFID" %.*s", PFID(&rec->cr_pfid),
				rec->cr_namelen, rec->cr_name);
		if (fid_is_sane(&rec->cr_sfid))
			printf(" s="DFID" sp="DFID" %.*s",
				PFID(&rec->cr_sfid), PFID(&rec->cr_spfid),
				changelog_rec_snamelen(rec),
				changelog_rec_sname(rec));
		printf("\n");

                llapi_changelog_free(&rec);
        }

        llapi_changelog_fini(&changelog_priv);

        if (rc < 0)
                fprintf(stderr, "Changelog: %s\n", strerror(errno = -rc));

        return (rc == 1 ? 0 : rc);
}

static int lfs_changelog_clear(int argc, char **argv)
{
        long long endrec;
        int rc;

        if (argc != 4)
                return CMD_HELP;

        endrec = strtoll(argv[3], NULL, 10);

        rc = llapi_changelog_clear(argv[1], argv[2], endrec);
        if (rc)
                fprintf(stderr, "%s error: %s\n", argv[0],
                        strerror(errno = -rc));
        return rc;
}

static int lfs_fid2path(int argc, char **argv)
{
        struct option long_opts[] = {
                {"cur", no_argument, 0, 'c'},
                {"link", required_argument, 0, 'l'},
                {"rec", required_argument, 0, 'r'},
                {0, 0, 0, 0}
        };
        char  short_opts[] = "cl:r:";
        char *device, *fid, *path;
        long long recno = -1;
        int linkno = -1;
        int lnktmp;
        int printcur = 0;
	int rc = 0;

        while ((rc = getopt_long(argc, argv, short_opts,
                                long_opts, NULL)) != -1) {
                switch (rc) {
                case 'c':
                        printcur++;
                        break;
                case 'l':
                        linkno = strtol(optarg, NULL, 10);
                        break;
                case 'r':
                        recno = strtoll(optarg, NULL, 10);
                        break;
                case '?':
                        return CMD_HELP;
                default:
                        fprintf(stderr, "error: %s: option '%s' unrecognized\n",
                                argv[0], argv[optind - 1]);
                        return CMD_HELP;
                }
        }

	if (argc < 3)
		return CMD_HELP;

	device = argv[optind++];
	path = calloc(1, PATH_MAX);

	rc = 0;
	while (optind < argc) {
		fid = argv[optind++];

		lnktmp = (linkno >= 0) ? linkno : 0;
		while (1) {
			int oldtmp = lnktmp;
			long long rectmp = recno;
			int rc2;
			rc2 = llapi_fid2path(device, fid, path, PATH_MAX,
					     &rectmp, &lnktmp);
			if (rc2 < 0) {
				fprintf(stderr, "%s: error on FID %s: %s\n",
					argv[0], fid, strerror(errno = -rc2));
				if (rc == 0)
					rc = rc2;
				break;
			}

			if (printcur)
				fprintf(stdout, "%lld ", rectmp);
			if (device[0] == '/') {
				fprintf(stdout, "%s", device);
				if (device[strlen(device) - 1] != '/')
					fprintf(stdout, "/");
			} else if (path[0] == '\0') {
				fprintf(stdout, "/");
			}
			fprintf(stdout, "%s\n", path);

			if (linkno >= 0)
				/* specified linkno */
				break;
			if (oldtmp == lnktmp)
				/* no more links */
				break;
		}
	}

	free(path);
	return rc;
}

static int lfs_path2fid(int argc, char **argv)
{
	char **path;
	const char *sep = "";
	lustre_fid fid;
	int rc = 0;

	if (argc < 2)
		return CMD_HELP;
	else if (argc > 2)
		sep = ": ";

	path = argv + 1;
	while (*path != NULL) {
		int err = llapi_path2fid(*path, &fid);

		if (err) {
			fprintf(stderr, "%s: can't get fid for %s: %s\n",
				argv[0], *path, strerror(-err));
			if (rc == 0) {
				rc = err;
				errno = -err;
			}
			goto out;
		}
		printf("%s%s"DFID"\n", *sep != '\0' ? *path : "", sep,
		       PFID(&fid));
out:
		path++;
	}

	return rc;
}

static int lfs_data_version(int argc, char **argv)
{
	char *path;
	__u64 data_version;
	int fd;
	int rc;
	int c;
	int data_version_flags = LL_DV_RD_FLUSH; /* Read by default */

	if (argc < 2)
		return CMD_HELP;

	while ((c = getopt(argc, argv, "nrw")) != -1) {
		switch (c) {
		case 'n':
			data_version_flags = 0;
			break;
		case 'r':
			data_version_flags |= LL_DV_RD_FLUSH;
			break;
		case 'w':
			data_version_flags |= LL_DV_WR_FLUSH;
			break;
		default:
			return CMD_HELP;
		}
	}
	if (optind == argc)
		return CMD_HELP;

	path = argv[optind];
	fd = open(path, O_RDONLY);
	if (fd < 0)
		err(errno, "cannot open file %s", path);

	rc = llapi_get_data_version(fd, &data_version, data_version_flags);
	if (rc < 0)
		err(errno, "cannot get version for %s", path);
	else
		printf(LPU64 "\n", data_version);

	close(fd);
	return rc;
}

static int lfs_hsm_state(int argc, char **argv)
{
	int rc;
	int i = 1;
	char *path;
	struct hsm_user_state hus;

	if (argc < 2)
		return CMD_HELP;

	do {
		path = argv[i];

		rc = llapi_hsm_state_get(path, &hus);
		if (rc) {
			fprintf(stderr, "can't get hsm state for %s: %s\n",
				path, strerror(errno = -rc));
			return rc;
		}

		/* Display path name and status flags */
		printf("%s: (0x%08x)", path, hus.hus_states);

		if (hus.hus_states & HS_RELEASED)
			printf(" released");
		if (hus.hus_states & HS_EXISTS)
			printf(" exists");
		if (hus.hus_states & HS_DIRTY)
			printf(" dirty");
		if (hus.hus_states & HS_ARCHIVED)
			printf(" archived");
		/* Display user-settable flags */
		if (hus.hus_states & HS_NORELEASE)
			printf(" never_release");
		if (hus.hus_states & HS_NOARCHIVE)
			printf(" never_archive");
		if (hus.hus_states & HS_LOST)
			printf(" lost_from_hsm");

		if (hus.hus_archive_id != 0)
			printf(", archive_id:%d", hus.hus_archive_id);
		printf("\n");

	} while (++i < argc);

	return 0;
}

#define LFS_HSM_SET   0
#define LFS_HSM_CLEAR 1

/**
 * Generic function to set or clear HSM flags.
 * Used by hsm_set and hsm_clear.
 *
 * @mode  if LFS_HSM_SET, set the flags, if LFS_HSM_CLEAR, clear the flags.
 */
static int lfs_hsm_change_flags(int argc, char **argv, int mode)
{
	struct option long_opts[] = {
		{"lost", 0, 0, 'l'},
		{"norelease", 0, 0, 'r'},
		{"noarchive", 0, 0, 'a'},
		{"archived", 0, 0, 'A'},
		{"dirty", 0, 0, 'd'},
		{"exists", 0, 0, 'e'},
		{0, 0, 0, 0}
	};
	char short_opts[] = "lraAde";
	__u64 mask = 0;
	int c, rc;
	char *path;

	if (argc < 3)
		return CMD_HELP;

	while ((c = getopt_long(argc, argv, short_opts,
				long_opts, NULL)) != -1) {
		switch (c) {
		case 'l':
			mask |= HS_LOST;
			break;
		case 'a':
			mask |= HS_NOARCHIVE;
			break;
		case 'A':
			mask |= HS_ARCHIVED;
			break;
		case 'r':
			mask |= HS_NORELEASE;
			break;
		case 'd':
			mask |= HS_DIRTY;
			break;
		case 'e':
			mask |= HS_EXISTS;
			break;
		case '?':
			return CMD_HELP;
		default:
			fprintf(stderr, "error: %s: option '%s' unrecognized\n",
				argv[0], argv[optind - 1]);
			return CMD_HELP;
		}
	}

	/* User should have specified a flag */
	if (mask == 0)
		return CMD_HELP;

	while (optind < argc) {

		path = argv[optind];

		/* If mode == 0, this means we apply the mask. */
		if (mode == LFS_HSM_SET)
			rc = llapi_hsm_state_set(path, mask, 0, 0);
		else
			rc = llapi_hsm_state_set(path, 0, mask, 0);

		if (rc != 0) {
			fprintf(stderr, "Can't change hsm flags for %s: %s\n",
				path, strerror(errno = -rc));
			return rc;
		}
		optind++;
	}

	return 0;
}

static int lfs_hsm_action(int argc, char **argv)
{
	int				 rc;
	int				 i = 1;
	char				*path;
	struct hsm_current_action	 hca;
	struct hsm_extent		 he;
	enum hsm_user_action		 hua;
	enum hsm_progress_states	 hps;

	if (argc < 2)
		return CMD_HELP;

	do {
		path = argv[i];

		rc = llapi_hsm_current_action(path, &hca);
		if (rc) {
			fprintf(stderr, "can't get hsm action for %s: %s\n",
				path, strerror(errno = -rc));
			return rc;
		}
		he = hca.hca_location;
		hua = hca.hca_action;
		hps = hca.hca_state;

		printf("%s: %s", path, hsm_user_action2name(hua));

		/* Skip file without action */
		if (hca.hca_action == HUA_NONE) {
			printf("\n");
			continue;
		}

		printf(" %s ", hsm_progress_state2name(hps));

		if ((hps == HPS_RUNNING) &&
		    (hua == HUA_ARCHIVE || hua == HUA_RESTORE))
			printf("("LPX64 " bytes moved)\n", he.length);
		else if ((he.offset + he.length) == OBD_OBJECT_EOF)
			printf("(from "LPX64 " to EOF)\n", he.offset);
		else
			printf("(from "LPX64 " to "LPX64")\n",
			       he.offset, he.offset + he.length);

	} while (++i < argc);

	return 0;
}

static int lfs_hsm_set(int argc, char **argv)
{
	return lfs_hsm_change_flags(argc, argv, LFS_HSM_SET);
}

static int lfs_hsm_clear(int argc, char **argv)
{
	return lfs_hsm_change_flags(argc, argv, LFS_HSM_CLEAR);
}

/**
 * Check file state and return its fid, to be used by lfs_hsm_request().
 *
 * \param[in]     file      Path to file to check
 * \param[in,out] fid       Pointer to allocated lu_fid struct.
 * \param[in,out] last_dev  Pointer to last device id used.
 *
 * \return 0 on success.
 */
static int lfs_hsm_prepare_file(char *file, struct lu_fid *fid,
				dev_t *last_dev)
{
	struct stat	st;
	int		rc;

	rc = lstat(file, &st);
	if (rc) {
		fprintf(stderr, "Cannot stat %s: %s\n", file, strerror(errno));
		return -errno;
	}
	/* A request should be ... */
	if (*last_dev != st.st_dev && *last_dev != 0) {
		fprintf(stderr, "All files should be "
			"on the same filesystem: %s\n", file);
		return -EINVAL;
	}
	*last_dev = st.st_dev;

	rc = llapi_path2fid(file, fid);
	if (rc) {
		fprintf(stderr, "Cannot read FID of %s: %s\n",
			file, strerror(-rc));
		return rc;
	}
	return 0;
}

static int lfs_hsm_request(int argc, char **argv, int action)
{
	struct option		 long_opts[] = {
		{"filelist", 1, 0, 'l'},
		{"data", 1, 0, 'D'},
		{"archive", 1, 0, 'a'},
		{0, 0, 0, 0}
	};
	dev_t			 last_dev = 0;
	char			 short_opts[] = "l:D:a:";
	struct hsm_user_request	*hur, *oldhur;
	int			 c, i;
	size_t			 len;
	int			 nbfile;
	char			*line = NULL;
	char			*filelist = NULL;
	char			 fullpath[PATH_MAX];
	char			*opaque = NULL;
	int			 opaque_len = 0;
	int			 archive_id = 0;
	FILE			*fp;
	int			 nbfile_alloc = 0;
	char			 some_file[PATH_MAX+1] = "";
	int			 rc;

	if (argc < 2)
		return CMD_HELP;

	while ((c = getopt_long(argc, argv, short_opts,
				long_opts, NULL)) != -1) {
		switch (c) {
		case 'l':
			filelist = optarg;
			break;
		case 'D':
			opaque = optarg;
			break;
		case 'a':
			if (action != HUA_ARCHIVE) {
				fprintf(stderr,
					"error: -a is supported only "
					"when archiving\n");
				return CMD_HELP;
			}
			archive_id = atoi(optarg);
			break;
		case '?':
			return CMD_HELP;
		default:
			fprintf(stderr, "error: %s: option '%s' unrecognized\n",
				argv[0], argv[optind - 1]);
			return CMD_HELP;
		}
	}

	/* All remaining args are files, so we have at least nbfile */
	nbfile = argc - optind;

	if ((nbfile == 0) && (filelist == NULL))
		return CMD_HELP;

	if (opaque != NULL)
		opaque_len = strlen(opaque);

	/* Alloc the request structure with enough place to store all files
	 * from command line. */
	hur = llapi_hsm_user_request_alloc(nbfile, opaque_len);
	if (hur == NULL) {
		fprintf(stderr, "Cannot create the request: %s\n",
			strerror(errno));
		return errno;
	}
	nbfile_alloc = nbfile;

	hur->hur_request.hr_action = action;
	hur->hur_request.hr_archive_id = archive_id;
	hur->hur_request.hr_flags = 0;

	/* All remaining args are files, add them */
	if (nbfile != 0) {
		if (strlen(argv[optind]) > sizeof(some_file)-1) {
			free(hur);
			return -E2BIG;
		}
		strncpy(some_file, argv[optind], sizeof(some_file));
	}

	for (i = 0; i < nbfile; i++) {
		hur->hur_user_item[i].hui_extent.length = -1;
		rc = lfs_hsm_prepare_file(argv[optind + i],
					  &hur->hur_user_item[i].hui_fid,
					  &last_dev);
		hur->hur_request.hr_itemcount++;
		if (rc)
			goto out_free;
	}

	/* from here stop using nb_file, use hur->hur_request.hr_itemcount */

	/* If a filelist was specified, read the filelist from it. */
	if (filelist != NULL) {
		fp = fopen(filelist, "r");
		if (fp == NULL) {
			fprintf(stderr, "Cannot read the file list %s: %s\n",
				filelist, strerror(errno));
			rc = -errno;
			goto out_free;
		}

		while ((rc = getline(&line, &len, fp)) != -1) {
			struct hsm_user_item *hui;

			/* If allocated buffer was too small, gets something
			 * bigger */
			if (nbfile_alloc <= hur->hur_request.hr_itemcount) {
				nbfile_alloc = nbfile_alloc * 2 + 1;
				oldhur = hur;
				hur = llapi_hsm_user_request_alloc(nbfile_alloc,
								   opaque_len);
				if (hur == NULL) {
					fprintf(stderr, "Cannot allocate "
						"the request: %s\n",
						strerror(errno));
					hur = oldhur;
					rc = -errno;
					goto out_free;
				}
				memcpy(hur, oldhur, hur_len(oldhur));
				free(oldhur);
			}

			/* Chop CR */
			if (line[strlen(line) - 1] == '\n')
				line[strlen(line) - 1] = '\0';

			hui =
			     &hur->hur_user_item[hur->hur_request.hr_itemcount];
			hui->hui_extent.length = -1;
			rc = lfs_hsm_prepare_file(line, &hui->hui_fid,
						  &last_dev);
			hur->hur_request.hr_itemcount++;
			if (rc)
				goto out_free;

			if ((some_file[0] == '\0') &&
			    (strlen(line) < sizeof(some_file)))
				strcpy(some_file, line);
		}

		rc = fclose(fp);
		if (line)
			free(line);
	}

	/* If a --data was used, add it to the request */
	hur->hur_request.hr_data_len = opaque_len;
	if (opaque != NULL)
		memcpy(hur_data(hur), opaque, opaque_len);

	/* Send the HSM request */
	if (realpath(some_file, fullpath) == NULL) {
		fprintf(stderr, "Could not find path '%s': %s\n",
			some_file, strerror(errno));
	}
	rc = llapi_hsm_request(fullpath, hur);
	if (rc) {
		fprintf(stderr, "Cannot send HSM request (use of %s): %s\n",
			some_file, strerror(-rc));
		goto out_free;
	}

out_free:
	free(hur);
	return rc;
}

static int lfs_hsm_archive(int argc, char **argv)
{
	return lfs_hsm_request(argc, argv, HUA_ARCHIVE);
}

static int lfs_hsm_restore(int argc, char **argv)
{
	return lfs_hsm_request(argc, argv, HUA_RESTORE);
}

static int lfs_hsm_release(int argc, char **argv)
{
	return lfs_hsm_request(argc, argv, HUA_RELEASE);
}

static int lfs_hsm_remove(int argc, char **argv)
{
	return lfs_hsm_request(argc, argv, HUA_REMOVE);
}

static int lfs_hsm_cancel(int argc, char **argv)
{
	return lfs_hsm_request(argc, argv, HUA_CANCEL);
}

static int lfs_swap_layouts(int argc, char **argv)
{
	if (argc != 3)
		return CMD_HELP;

	return llapi_swap_layouts(argv[1], argv[2], 0, 0,
				  SWAP_LAYOUTS_KEEP_MTIME |
				  SWAP_LAYOUTS_KEEP_ATIME);
}

int main(int argc, char **argv)
{
        int rc;

        setlinebuf(stdout);

        ptl_initialize(argc, argv);
        if (obd_initialize(argc, argv) < 0)
                exit(2);

        Parser_init("lfs > ", cmdlist);

        if (argc > 1) {
                rc = Parser_execarg(argc - 1, argv + 1, cmdlist);
        } else {
                rc = Parser_commands();
        }

        obd_finalize(argc, argv);
        return rc < 0 ? -rc : rc;
}

