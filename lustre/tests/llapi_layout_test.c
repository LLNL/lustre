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
 * GPL HEADER END
 */
/*
 * These tests exercise the llapi_layout API which abstracts the layout
 * of a Lustre file behind an opaque data type.  They assume a Lustre
 * file system with at least 2 OSTs and a pool containing at least the
 * first 2 OSTs.  For example,
 *
 *  sudo lctl pool_new lustre.testpool
 *  sudo lctl pool_add lustre.testpool OST[0-1]
 *  gcc -Wall -g -Werror -o llapi_layout_test llapi_layout_test.c -llustreapi
 *  sudo ./llapi_layout_test
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <errno.h>
#include <lustre/lustreapi.h>
#include <pwd.h>
#include <limits.h>
#include <sys/stat.h>
#include <getopt.h>

#define ERROR(fmt, ...)							\
	fprintf(stderr, "%s: %s:%d: %s: " fmt "\n",			\
		program_invocation_short_name, __FILE__, __LINE__,	\
		__func__, ## __VA_ARGS__);

#define DIE(fmt, ...)			\
do {					\
	ERROR(fmt, ## __VA_ARGS__);	\
	exit(EXIT_FAILURE);		\
} while (0)

#define VERIFYF(cond, fmt, ...)						\
do {									\
	if (!(cond))							\
		DIE("failed '" #cond "': " fmt, ## __VA_ARGS__);	\
} while (0)								\

static char *lustre_dir;
static char *poolname;
static int num_osts = -1;

static void usage(char *prog)
{
	printf("Usage: %s [-d lustre_dir] [-p pool_name] [-o num_osts]\n",
	       prog);
	exit(0);
}

#define T0FILE			"t0"
#define T0_STRIPE_COUNT		num_osts
#define T0_STRIPE_SIZE		1048576
#define T0_OST_OFFSET		(num_osts - 1)
#define T0_DESC		"Read/write layout attributes then create a file"
void test0(void)
{
	int rc;
	int fd;
	llapi_layout_t *layout = llapi_layout_alloc();
	char file[PATH_MAX];
	const char *mypool;

	VERIFYF(layout != NULL, "errno %d", errno);

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T0FILE);

	rc = unlink(file);
	VERIFYF(rc >= 0 || errno == ENOENT, "errno = %d", errno);

	/* stripe count */
	rc = llapi_layout_stripe_count_set(layout, T0_STRIPE_COUNT);
	VERIFYF(rc == 0, "errno = %d", errno);
	rc = llapi_layout_stripe_count(layout);
	VERIFYF(rc == T0_STRIPE_COUNT, "%d != %d", rc, T0_STRIPE_COUNT);

	/* stripe size */
	rc = llapi_layout_stripe_size_set(layout, T0_STRIPE_SIZE);
	VERIFYF(rc == 0, "rc = %d", rc);
	rc = llapi_layout_stripe_size(layout);
	VERIFYF(rc == T0_STRIPE_SIZE, "%d != %d", rc, T0_STRIPE_SIZE);

	/* pool_name */
	rc = llapi_layout_pool_name_set(layout, poolname);
	VERIFYF(rc == 0, "rc = %d", rc);
	mypool = llapi_layout_pool_name(layout);
	VERIFYF(mypool != NULL, "errno = %d", errno);
	rc = strcmp(mypool, poolname);
	VERIFYF(rc == 0, "%s != %s", mypool, poolname);

	/* ost_index */
	rc = llapi_layout_ost_index_set(layout, 0, T0_OST_OFFSET);
	VERIFYF(rc == 0, "errno = %d", errno);

	/* create */
	fd = llapi_layout_file_create(layout, file, 0, 0660);
	VERIFYF(fd >= 0, "errno = %d", errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);
	llapi_layout_free(layout);
}

void __test1_helper(llapi_layout_t *layout)
{
	int ost0;
	int ost1;
	int rc;
	const char *mypool;

	rc = llapi_layout_stripe_count(layout);
	VERIFYF(rc == T0_STRIPE_COUNT, "%d != %d", rc, T0_STRIPE_COUNT);

	rc = llapi_layout_stripe_size(layout);
	VERIFYF(rc == T0_STRIPE_SIZE, "%d != %d", rc, T0_STRIPE_SIZE);

	mypool = llapi_layout_pool_name(layout);
	VERIFYF(mypool != NULL, "errno = %d", errno);
	rc = strcmp(mypool, poolname);
	VERIFYF(rc == 0, "%s != %s", mypool, poolname);

	ost0 = llapi_layout_ost_index(layout, 0);
	VERIFYF(ost0 >= 0, "errno = %d", errno);
	ost1 = llapi_layout_ost_index(layout, 1);
	VERIFYF(ost1 >= 0, "errno = %d", errno);
	VERIFYF(ost0 == T0_OST_OFFSET, "%d != %d", ost0, T0_OST_OFFSET);
	VERIFYF(ost1 != ost0, "%d == %d", ost0, ost1);
}

#define T1_DESC		"Read test0 file by path and verify attributes"
void test1(void)
{
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T0FILE);
	llapi_layout_t *layout = llapi_layout_by_path(file);
	VERIFYF(layout != NULL, "errno = %d", errno);
	__test1_helper(layout);
	llapi_layout_free(layout);
}

#define T2_DESC		"Read test0 file by fd and verify attributes"
void test2(void)
{
	int fd;
	int rc;
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T0FILE);

	fd = open(file, O_RDONLY);
	VERIFYF(fd >= 0, "open(%s): errno = %d", file, errno);

	llapi_layout_t *layout = llapi_layout_by_fd(fd);
	VERIFYF(layout != NULL, "errno = %d", errno);

	rc = close(fd);
	VERIFYF(rc == 0, "close(%s): errno = %d", file, errno);

	__test1_helper(layout);
	llapi_layout_free(layout);
}

#define T3FILE			"t3"
#define T3_STRIPE_COUNT		2
#define T3_STRIPE_SIZE		2097152
#define T3_DESC		"Verify compatibility with 'lfs setstripe'"
void test3(void)
{
	int rc;
	int ost0;
	int ost1;
	const char *lfs = getenv("LFS");
	const char *mypool;
	char cmd[4096];
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T3FILE);

	if (lfs == NULL)
		lfs = "/usr/bin/lfs";

	rc = unlink(file);
	VERIFYF(rc == 0 || errno == ENOENT, "errno = %d", errno);

	snprintf(cmd, sizeof(cmd), "%s setstripe -p %s -c %d -s %d %s\n", lfs,
		 poolname, T3_STRIPE_COUNT, T3_STRIPE_SIZE, file);
	rc = system(cmd);
	VERIFYF(rc == 0, "system(%s): exit status %d", cmd, WEXITSTATUS(rc));

	errno = 0;
	llapi_layout_t *layout = llapi_layout_by_path(file);
	VERIFYF(layout != NULL, "errno = %d", errno);

	rc = llapi_layout_stripe_count(layout);
	VERIFYF(rc == T3_STRIPE_COUNT, "%d != %d", rc, T3_STRIPE_COUNT);

	rc = llapi_layout_stripe_size(layout);
	VERIFYF(rc == T3_STRIPE_SIZE, "%d != %d", rc, T3_STRIPE_SIZE);

	mypool = llapi_layout_pool_name(layout);
	VERIFYF(mypool != NULL, "errno = %d", errno);
	rc = strcmp(mypool, poolname);
	VERIFYF(rc == 0, "%s != %s", mypool, poolname);

	ost0 = llapi_layout_ost_index(layout, 0);
	VERIFYF(ost0 >= 0, "errno = %d", errno);
	ost1 = llapi_layout_ost_index(layout, 1);
	VERIFYF(ost1 >= 0, "errno = %d", errno);
	VERIFYF(ost1 != ost0, "%d == %d", ost0, ost1);

	llapi_layout_free(layout);
}


#define T4FILE		"t4"
#define T4_DESC		"llapi_layout_by_path ENOENT handling"
void test4(void)
{
	int rc;
	char file[PATH_MAX];
	llapi_layout_t *layout;

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T4FILE);

	rc = unlink(file);
	VERIFYF(rc == 0 || errno == ENOENT, "errno = %d", errno);

	errno = 0;
	layout = llapi_layout_by_path(file);
	VERIFYF(layout == NULL && errno == ENOENT, "errno = %d", errno);
}

#define T5_DESC		"llapi_layout_by_fd EBADF handling"
void test5(void)
{
	errno = 0;
	llapi_layout_t *layout = llapi_layout_by_fd(9999);
	VERIFYF(layout == NULL && errno == EBADF, "errno = %d", errno);
}


#define T6FILE		"t6"
#define T6_DESC		"llapi_layout_by_path EACCES handling"
void test6(void)
{
	int fd;
	int rc;
	uid_t myuid = getuid();
	char file[PATH_MAX];
	const char *runas = getenv("RUNAS_ID");
	struct passwd *pw;
	uid_t uid;

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T6FILE);
	VERIFYF(myuid == 0, "myuid = %d", myuid); /* Need root for this test. */

	/* Create file as root */
	rc = unlink(file);
	VERIFYF(rc == 0 || errno == ENOENT, "errno = %d", errno);

	fd = open(file, O_CREAT, 0400);
	VERIFYF(fd > 0, "errno = %d", errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);

	/* Become unprivileged user */
	if (runas != NULL) {
		uid = atoi(runas);
		VERIFYF(uid != 0, "runas = %s", runas);
	} else {
		pw = getpwnam("nobody");
		VERIFYF(pw != NULL, "errno = %d", errno);
		uid = pw->pw_uid;
	}
	rc = seteuid(uid);
	VERIFYF(rc == 0, "errno = %d", errno);
	errno = 0;
	llapi_layout_t *layout = llapi_layout_by_path(file);
	VERIFYF(layout == NULL && errno == EACCES, "errno = %d", errno);
	rc = seteuid(myuid);
	VERIFYF(rc == 0, "errno = %d", errno);
}


/* llapi_layout_by_path() returns ENODATA in errno for file with no
 * striping attributes. */
#define T7FILE		"t7"
#define T7_DESC		"llapi_layout_by_path ENODATA handling"
void test7(void)
{
	int fd;
	int rc;
	llapi_layout_t *layout;
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T7FILE);

	rc = unlink(file);
	VERIFYF(rc >= 0 || errno == ENOENT, "errno = %d", errno);
	fd = open(file, O_CREAT, 0640);
	VERIFYF(fd >= 0, "errno = %d", errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);

	errno = 0;
	layout = llapi_layout_by_path(file);
	VERIFYF(layout == NULL && errno == ENODATA, "errno = %d", errno);
}

/* Setting pattern > 0 returns EOPNOTSUPP in errno. */
#define T8_DESC		"llapi_layout_pattern_set() EOPNOTSUPP handling"
void test8(void)
{
	llapi_layout_t *layout;
	int rc;

	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d\n", errno);
	errno = 0;
	rc = llapi_layout_pattern_set(layout, 1);
	VERIFYF(rc == -1 && errno == EOPNOTSUPP, "rc = %d, errno = %d", rc,
		errno);
	llapi_layout_free(layout);
}


/* llapi_layout_create_file() returns EEXIST in errno for
 * already-existing file. */
#define T9FILE		"t9"
#define T9_STRIPE_COUNT	2
#define T9_STRIPE_SIZE	1048576
#define T9_DESC		"llapi_layout_create_file EEXIST handling"
void test9(void)
{
	int rc;
	int fd;
	llapi_layout_t *layout;
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T9FILE);

	(void) unlink(file);
	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);
	rc = llapi_layout_stripe_count_set(layout, T9_STRIPE_COUNT);
	VERIFYF(rc == 0, "errno = %d", errno);
	rc = llapi_layout_stripe_size_set(layout, T9_STRIPE_SIZE);
	VERIFYF(rc == 0, "errno = %d", errno);
	fd = llapi_layout_file_create(layout, file, 0, 0750);
	VERIFYF(fd >= 0, "errno = %d", errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);
	errno = 0;
	rc = llapi_layout_file_create(layout, file, 0, 0750);
	VERIFYF(rc == -1 && errno == EEXIST, "rc = %d, errno = %d", rc, errno);
	llapi_layout_free(layout);
}

/* Verify stripe_count interfaces return errors as expected */
#define T10_DESC	"stripe_count error handling"
void test10(void)
{
	int rc;
	llapi_layout_t *layout;

	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);

	/* stripe count less than -1 (-1 means stripe as widely as possible) */
	errno = 0;
	rc = llapi_layout_stripe_count_set(layout, -2);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_stripe_count_set(NULL, 2);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_stripe_count(NULL);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* stripe count too large */
	errno = 0;
	rc = llapi_layout_stripe_count_set(layout, LOV_MAX_STRIPE_COUNT + 1);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);
	llapi_layout_free(layout);
}

/* Verify stripe_size interfaces return errors as expected */
#define T11_DESC	"stripe_size error handling"
void test11(void)
{
	int rc;
	llapi_layout_t *layout;

	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);

	/* negative stripe size */
	errno = 0;
	rc = llapi_layout_stripe_size_set(layout, -1);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_stripe_size_set(NULL, 1048576);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_stripe_size(NULL);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	llapi_layout_free(layout);
}

/* Verify pool_name interfaces return errors as expected */
#define T12_DESC	"pool_name error handling"
void test12(void)
{
	int rc;
	llapi_layout_t *layout;
	const char *poolname;

	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_pool_name_set(NULL, "foo");
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* NULL pool name */
	errno = 0;
	rc = llapi_layout_pool_name_set(layout, NULL);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* NULL layout */
	errno = 0;
	poolname = llapi_layout_pool_name(NULL);
	VERIFYF(poolname == NULL && errno == EINVAL,
		"poolname = %s, errno = %d", poolname, errno);

	/* Pool name too long*/
	errno = 0;
	rc = llapi_layout_pool_name_set(layout, "0123456789abcdef0");
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	llapi_layout_free(layout);
}

/* Verify ost_index interface returns errors as expected */
#define T13FILE			"t13"
#define T13_STRIPE_COUNT	2
#define T13_DESC		"ost_index error handling"
void test13(void)
{
	int rc;
	int fd;
	llapi_layout_t *layout;
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T13FILE);

	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);

	/* Only setting OST index for stripe 0 is supported for now. */
	errno = 0;
	rc = llapi_layout_ost_index_set(layout, 1, 1);
	VERIFYF(rc == -1 && errno == EOPNOTSUPP, "rc = %d, errno = %d",
		rc, errno);

	/* OST index less than one (-1 means let MDS choose) */
	errno = 0;
	rc = llapi_layout_ost_index_set(layout, 0, -2);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_ost_index_set(NULL, 0, 1);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	errno = 0;
	rc = llapi_layout_ost_index(NULL, 0);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	/* Layout not read from file so has no OST data. */
	errno = 0;
	rc = llapi_layout_stripe_count_set(layout, T13_STRIPE_COUNT);
	VERIFYF(rc == 0, "errno = %d", errno);
	rc = llapi_layout_ost_index(layout, 0);
	VERIFYF(rc == -1 && errno == 0, "rc = %d, errno = %d", rc, errno);

	/* n greater than stripe count*/
	rc = unlink(file);
	VERIFYF(rc >= 0 || errno == ENOENT, "errno = %d", errno);
	rc = llapi_layout_stripe_count_set(layout, T13_STRIPE_COUNT);
	VERIFYF(rc == 0, "errno = %d", errno);
	fd = llapi_layout_file_create(layout, file, 0, 0644);
	VERIFYF(fd >= 0, "errno = %d", errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);
	llapi_layout_free(layout);

	layout = llapi_layout_by_path(file);
	VERIFYF(layout != NULL, "errno = %d", errno);
	errno = 0;
	rc = llapi_layout_ost_index(layout, T13_STRIPE_COUNT + 1);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);

	llapi_layout_free(layout);
}

/* Verify llapi_layout_file_create() returns errors as expected */
#define T14_DESC	"llapi_layout_file_create error handling"
void test14(void)
{
	int rc;

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_file_create(NULL, "foo", 0, 0);
	VERIFYF(rc == -1 && errno == EINVAL, "rc = %d, errno = %d", rc, errno);
}

/* Can't change striping attributes of existing file. */
#define T15FILE			"t15"
#define T15_STRIPE_COUNT	2
#define T15_DESC	"Can't change striping attributes of existing file"
void test15(void)
{
	int rc;
	int fd;
	int count;
	llapi_layout_t *layout;
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T15FILE);

	rc = unlink(file);
	VERIFYF(rc >= 0 || errno == ENOENT, "errno = %d", errno);

	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);
	rc = llapi_layout_stripe_count_set(layout, T15_STRIPE_COUNT);
	VERIFYF(rc == 0, "errno = %d", errno);

	errno = 0;
	fd = llapi_layout_file_create(layout, file, 0640, 0);
	VERIFYF(fd >= 0 && errno == 0, "fd = %d, errno = %d", fd, errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);

	rc = llapi_layout_stripe_count_set(layout, T15_STRIPE_COUNT - 1);
	errno = 0;
	fd = llapi_layout_file_create(layout, file, 0640, 0);
	VERIFYF(fd < 0 && errno == EEXIST, "fd = %d, errno = %d", fd, errno);
	llapi_layout_free(layout);

	layout = llapi_layout_by_path(file);
	VERIFYF(layout != NULL, "errno = %d", errno);
	count = llapi_layout_stripe_count(layout);
	VERIFYF(count == T15_STRIPE_COUNT, "%d != %d", count, T15_STRIPE_COUNT);
	llapi_layout_free(layout);
}

/* Default stripe attributes are applied as expected. */
#define T16FILE		"t16"
#define T16_DESC	"Default stripe attributes are applied as expected"
void test16(void)
{
	int		rc;
	int		fd;
	llapi_layout_t	*dirlayout;
	llapi_layout_t	*filelayout;
	char		file[PATH_MAX];
	char		lustre_mnt[PATH_MAX];
	ssize_t		fsize;
	int		fcount;
	ssize_t		dsize;
	int		dcount;

	rc = llapi_search_mounts(lustre_dir, 0, lustre_mnt, NULL);

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T16FILE);

	rc = unlink(file);
	VERIFYF(rc == 0 || errno == ENOENT, "errno = %d", errno);

	dirlayout = llapi_layout_by_path(lustre_mnt);
	VERIFYF(dirlayout != NULL, "errno = %d", errno);

	filelayout = llapi_layout_alloc();
	VERIFYF(filelayout != NULL, "errno = %d", errno);

	fd = llapi_layout_file_create(filelayout, file, 0640, 0);
	VERIFYF(fd >= 0, "errno = %d", errno);

	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);

	llapi_layout_free(filelayout);

	filelayout = llapi_layout_by_path(file);
	VERIFYF(filelayout != NULL, "errno = %d", errno);

	fcount = llapi_layout_stripe_count(filelayout);
	VERIFYF(fcount != -1, "errno = %d", errno);
	dcount = llapi_layout_stripe_count(dirlayout);
	VERIFYF(dcount != -1, "errno = %d", errno);
	VERIFYF(fcount == dcount, "%d != %d", fcount, dcount);

	fsize = llapi_layout_stripe_size(filelayout);
	VERIFYF(fsize != -1, "errno = %d", errno);
	dsize = llapi_layout_stripe_size(dirlayout);
	VERIFYF(dsize != -1, "errno = %d", errno);
	VERIFYF(fsize == dsize, "%zd != %zd", fsize, dsize);

	llapi_layout_free(filelayout);
	llapi_layout_free(dirlayout);
}


/* Setting stripe count to -1 uses all available OSTs. */
#define T17FILE		"t17"
#define T17_DESC	"Setting stripe count to -1 uses all available OSTs"
void test17(void)
{
	int rc;
	int fd;
	int osts_all;
	int osts_layout;
	llapi_layout_t *layout;
	char file[PATH_MAX];

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T17FILE);

	rc = unlink(file);
	VERIFYF(rc == 0 || errno == ENOENT, "errno = %d", errno);
	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);
	rc = llapi_layout_stripe_count_set(layout, -1);
	VERIFYF(rc == 0, "errno = %d", errno);
	fd = llapi_layout_file_create(layout, file, 0640, 0);
	VERIFYF(fd >= 0, "errno = %d", errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);
	llapi_layout_free(layout);

	/* Get number of available OSTs */
	fd = open(file, O_RDONLY);
	VERIFYF(fd >= 0, "errno = %d", errno);
	rc = llapi_lov_get_uuids(fd, NULL, &osts_all);
	VERIFYF(rc == 0, "rc = %d, errno = %d", rc, errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);

	layout = llapi_layout_by_path(file);
	VERIFYF(layout != NULL, "errno = %d", errno);
	osts_layout = llapi_layout_stripe_count(layout);
	VERIFYF(osts_layout == osts_all, "%d != %d", osts_layout, osts_all);

	llapi_layout_free(layout);
}

/* Setting pool with "fsname.pool" notation. */
#define T18FILE		"t18"
#define T18_DESC	"Setting pool with fsname.pool notation"
void test18(void)
{
	int rc;
	int fd;
	llapi_layout_t *layout = llapi_layout_alloc();
	char file[PATH_MAX];
	char pool[LOV_MAXPOOLNAME*2 + 1];
	const char *mypool;

	snprintf(pool, sizeof(pool), "lustre.%s", poolname);

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T18FILE);

	VERIFYF(layout != NULL, "errno = %d", errno);

	rc = unlink(file);
	VERIFYF(rc == 0 || errno == ENOENT, "errno = %d", errno);

	rc = llapi_layout_pool_name_set(layout, pool);
	VERIFYF(rc == 0, "errno = %d", errno);

	mypool = llapi_layout_pool_name(layout);
	rc = strcmp(mypool, poolname);
	VERIFYF(rc == 0, "%s != %s", mypool, poolname);
	fd = llapi_layout_file_create(layout, file, 0640, 0);
	VERIFYF(fd >= 0, "errno = %d", errno);
	rc = close(fd);
	VERIFYF(rc == 0, "errno = %d", errno);

	llapi_layout_free(layout);

	layout = llapi_layout_by_path(file);
	VERIFYF(layout != NULL, "errno = %d", errno);
	mypool = llapi_layout_pool_name(layout);
	rc = strcmp(mypool, poolname);
	VERIFYF(rc == 0, "%s != %s", mypool, poolname);
	llapi_layout_free(layout);
}

#define T19FILE			"t19"
#define T19_STRIPE_COUNT	2
#define T19_STRIPE_SIZE		1048576
#define T19_DESC		"Look up layout by fid"
void test19(void)
{
	int rc;
	int fd;
	llapi_layout_t *layout;
	lustre_fid fid;
	char fidstr[4096];
	char file[PATH_MAX];
	int count;
	ssize_t size;

	snprintf(file, sizeof(file), "%s/%s", lustre_dir, T19FILE);

	rc = unlink(file);
	VERIFYF(rc == 0 || errno == ENOENT, "errno = %d", errno);

	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);

	rc = llapi_layout_stripe_size_set(layout, T19_STRIPE_SIZE);
	VERIFYF(rc == 0, "errno = %d", errno);
	rc = llapi_layout_stripe_count_set(layout, T19_STRIPE_COUNT);
	VERIFYF(rc == 0, "errno = %d", errno);
	fd = llapi_layout_file_create(layout, file, 0640, 0);
	VERIFYF(fd >= 0, "errno = %d", errno);
	rc = close(rc);
	VERIFYF(rc == 0, "errno = %d", errno);
	llapi_layout_free(layout);

	rc = llapi_path2fid(file, &fid);
	VERIFYF(rc == 0, "rc = %d, errno = %d", rc, errno);
	snprintf(fidstr, sizeof(fidstr), "0x%llx:0x%x:0x%x", fid.f_seq,
		 fid.f_oid, fid.f_ver);
	errno = 0;
	layout = llapi_layout_by_fid(file, fidstr);
	VERIFYF(layout != NULL, "fidstr = %s, errno = %d", fidstr, errno);
	count = llapi_layout_stripe_count(layout);
	VERIFYF(count == T19_STRIPE_COUNT, "%d != %d", count, T19_STRIPE_COUNT);
	size = llapi_layout_stripe_size(layout);
	VERIFYF(size = T19_STRIPE_SIZE, "%zd != %d", size, T19_STRIPE_SIZE);
	llapi_layout_free(layout);
}

#define T20_DESC	"Maximum length pool name is NULL-terminated"
void test20(void)
{
	llapi_layout_t *layout;
	const char *str;
	char *name = "0123456789abcdef";
	int rc;

	layout = llapi_layout_alloc();
	VERIFYF(layout != NULL, "errno = %d", errno);
	rc = llapi_layout_pool_name_set(layout, name);
	VERIFYF(rc == 0, "errno = %d", errno);
	str = llapi_layout_pool_name(layout);
	VERIFYF(strlen(name) == strlen(str), "name = %s, str = %s", name, str);
	llapi_layout_free(layout);
}

void sigsegv(int signal)
{
	printf("Segmentation fault\n");
	exit(1);
}

#define TEST_DESC_LEN	50
struct test_tbl_entry {
	void (*test_fn)(void);
	char test_desc[TEST_DESC_LEN];
};

static struct test_tbl_entry test_tbl[] = {
	{ &test0,  T0_DESC },
	{ &test1,  T1_DESC },
	{ &test2,  T2_DESC },
	{ &test3,  T3_DESC },
	{ &test4,  T4_DESC },
	{ &test5,  T5_DESC },
	{ &test6,  T6_DESC },
	{ &test7,  T7_DESC },
	{ &test8,  T8_DESC },
	{ &test9,  T9_DESC },
	{ &test10, T10_DESC },
	{ &test11, T11_DESC },
	{ &test12, T12_DESC },
	{ &test13, T13_DESC },
	{ &test14, T14_DESC },
	{ &test15, T15_DESC },
	{ &test16, T16_DESC },
	{ &test17, T17_DESC },
	{ &test18, T18_DESC },
	{ &test19, T19_DESC },
	{ &test20, T20_DESC },
};
#define NUM_TESTS	21

/* This function runs a single test by forking the process.  This way,
 * if there is a segfault during a test, the test program won't crash. */
int test(void (*test_fn)(), const char *test_desc, int test_num)
{
	int rc = -1;
	int i;

	pid_t pid = fork();
	if (pid > 0) {
		int status;
		wait(&status);
		printf("test %2d: %s ", test_num, test_desc);
		for (i = 0; i < TEST_DESC_LEN - strlen(test_desc); i++)
			printf(".");
		/* Non-zero value indicates failure. */
		if (status == 0)
			printf("pass\n");
		else
			printf("fail (status = %d)\n", WEXITSTATUS(status));
		rc = status ? -1 : 0;
	} else if (pid == 0) {
		/* Run the test in the child process.  Exit with 0 for success,
		 * non-zero for failure */
		test_fn();
		exit(0);
	} else {
		printf("Fork failed!\n");
	}
	return rc;
}

static void process_args(int argc, char *argv[])
{
	int c;

	while ((c = getopt(argc, argv, "d:p:o:")) != -1) {
		switch (c) {
		case 'd':
			lustre_dir = optarg;
			break;
		case 'p':
			poolname = optarg;
			break;
		case 'o':
			num_osts = atoi(optarg);
			break;
		case '?':
			printf("Unknown option '%c'\n", optopt);
			usage(argv[0]);
		}
	}
}

int main(int argc, char *argv[])
{
	int rc = 0;
	int i;
	struct stat s;
	char fsname[8];

	llapi_msg_set_level(LLAPI_MSG_OFF);

	process_args(argc, argv);
	if (lustre_dir == NULL)
		lustre_dir = "/mnt/lustre";
	if (poolname == NULL)
		poolname = "testpool";
	if (num_osts == -1)
		num_osts = 2;

	if (num_osts < 2) {
		fprintf(stderr, "Error: at least 2 OSTS are required\n");
		exit(EXIT_FAILURE);
	}
	if (stat(lustre_dir, &s) < 0) {
		fprintf(stderr, "Error: %s: %s\n", lustre_dir, strerror(errno));
		exit(EXIT_FAILURE);
	} else if (!S_ISDIR(s.st_mode)) {
		fprintf(stderr, "Error: %s: not a directory\n", lustre_dir);
		exit(EXIT_FAILURE);
	}

	rc = llapi_search_fsname(lustre_dir, fsname);
	if (rc != 0) {
		fprintf(stderr, "Error: %s: not a Lustre filesystem\n",
			lustre_dir);
		exit(EXIT_FAILURE);
	}

	signal(SIGSEGV, sigsegv);

	/* Play nice with Lustre test scripts. Non-line buffered output
	 * stream under I/O redirection may appear incorrectly. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (i = 0; i < NUM_TESTS; i++) {
		if (test(test_tbl[i].test_fn, test_tbl[i].test_desc, i) != 0)
			rc++;
	}
	return rc;
}
