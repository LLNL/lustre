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
 *  gcc -Wall -g -Werror -o llapi_layout_test llapi_layout_test.c -llustrepapi
 *  sudo ./llapi_layout_test
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <lustre/lustreapi.h>
#include <pwd.h>
#include <limits.h>
#include <sys/stat.h>
#include <getopt.h>

static char lustre_dir[PATH_MAX] = { '\0' };
static char poolname[LOV_MAXPOOLNAME + 1] = { '\0' };
static int num_osts = -1;

static void usage(char *prog)
{
	printf("Usage: %s [-d lustre_dir] [-p pool_name] [-o num_osts]\n",
	       prog);
	exit(0);
}

#define T0FILE			"/t0"
#define T0_STRIPE_COUNT		num_osts
#define T0_STRIPE_SIZE		1048576
#define T0_OST_OFFSET		num_osts - 1
/* Sanity test. Read and write layout attributes then create a new file. */
void test0()
{
	int rc;
	lustre_layout_t *layout = llapi_layout_alloc();
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T0FILE);

	assert(NULL != layout);

	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);

	assert(0 == llapi_layout_stripe_count_set(layout, T0_STRIPE_COUNT));
	assert(T0_STRIPE_COUNT == llapi_layout_stripe_count(layout));

	assert(0 == llapi_layout_stripe_size_set(layout, T0_STRIPE_SIZE));
	assert(T0_STRIPE_SIZE == llapi_layout_stripe_size(layout));

	assert(0 == llapi_layout_pool_name_set(layout, poolname));
	assert(0 == strcmp(llapi_layout_pool_name(layout), poolname));

	rc = llapi_layout_ost_index_set(layout, 0, T0_OST_OFFSET);
	assert(0 == rc);

	rc = llapi_layout_file_create(layout, file, 0660, 0);
	assert(0 <= rc);
	assert(0 == close(rc));
	llapi_layout_free(layout);
}

void __test1_helper(lustre_layout_t *layout)
{
	int ost0;
	int ost1;

	assert(llapi_layout_stripe_count(layout) == T0_STRIPE_COUNT);
	assert(llapi_layout_stripe_size(layout) == T0_STRIPE_SIZE);
	ost0 = llapi_layout_ost_index(layout, 0);
	ost1 = llapi_layout_ost_index(layout, 1);

	assert(0 == strcmp(llapi_layout_pool_name(layout), poolname));
	assert(T0_OST_OFFSET == ost0);
	assert(ost1 != ost0);
}

/* Read back file layout from test1 by path and verify attributes. */
void test1()
{
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T0FILE);
	lustre_layout_t *layout = llapi_layout_lookup_bypath(file);
	assert(NULL != layout);
	__test1_helper(layout);
	llapi_layout_free(layout);
}

/* Read back file layout from test1 by open fd and verify attributes. */
void test2()
{
	int fd;
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T0FILE);

	fd = open(file, O_RDONLY);
	assert(-1 != fd);

	lustre_layout_t *layout = llapi_layout_lookup_byfd(fd);
	assert(0 == close(fd));
	assert(NULL != layout);
	__test1_helper(layout);
	llapi_layout_free(layout);
}

#define T3FILE			"/t3"
#define T3_STRIPE_COUNT		2
#define T3_STRIPE_SIZE 		2097152
/* Create a file with 'lfs setstripe' then verify its layout */
void test3()
{
	int rc;
	int ost0;
	int ost1;
	const char *lfs = getenv("LFS");
	char cmd[4096];
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T3FILE);

	if (lfs == NULL)
		lfs = "/usr/bin/lfs";

	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);
	snprintf(cmd, 4096, "%s setstripe -p %s -c %d -s %d %s\n", lfs,
		 poolname, T3_STRIPE_COUNT, T3_STRIPE_SIZE, file);
	assert(0 == system(cmd));

	errno = 0;
	lustre_layout_t *layout = llapi_layout_lookup_bypath(file);
	assert(NULL != layout);
	assert(0 == errno);
	assert(T3_STRIPE_COUNT == llapi_layout_stripe_count(layout));
	assert(T3_STRIPE_SIZE == llapi_layout_stripe_size(layout));
	assert(0 == strcmp(llapi_layout_pool_name(layout), poolname));
	ost0 = llapi_layout_ost_index(layout, 0);
	ost1 = llapi_layout_ost_index(layout, 1);
	assert(ost0 != ost1);
	llapi_layout_free(layout);
}

#define T4FILE			"/t4"
/* llapi_layout_lookup_bypath() returns ENOENT in errno when expected. */
void test4()
{
	int rc;
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T4FILE);
	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);
	errno = 0;
	lustre_layout_t *layout = llapi_layout_lookup_bypath(file);
	assert(NULL == layout);
	assert(ENOENT == errno);
}

/* llapi_layout_lookup_byfd() returns EBADF in errno when expected. */
void test5()
{
	errno = 0;
	lustre_layout_t *layout = llapi_layout_lookup_byfd(9999);
	assert(EBADF == errno);
	assert(NULL == layout);
}

#define T6FILE			"/t6"
/* llapi_layout_lookup_bypath() returns EACCES in errno when expected. */
void test6()
{
	int fd;
	int rc;
	uid_t myuid = getuid();
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T6FILE);
	assert(0 == myuid); /* Need root for this test. */

	/* Create file as root */
	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);
	fd = open(file, O_CREAT, 0400);
	assert(-1 != fd);
	assert(0 == close(fd));

	/* Become unprivileged user */
	struct passwd *pw = getpwnam("nobody");
	assert(NULL != pw);
	assert(0 == seteuid(pw->pw_uid));
	errno = 0;
	lustre_layout_t *layout = llapi_layout_lookup_bypath(file);
	assert(NULL == layout);
	assert(EACCES == errno);
	assert(0 == seteuid(myuid));
}

#define T7FILE			"/t7"
/* llapi_layout_lookup_bypath() returns ENODATA in errno for file with no
 * striping attributes. */
void test7()
{
	int fd;
	int rc;
	lustre_layout_t *layout;
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T7FILE);

	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);
	fd = open(file, O_CREAT, 0640);
	assert(-1 != fd);
	assert(0 == close(fd));

	errno = 0;
	layout = llapi_layout_lookup_bypath(file);
	assert(NULL == layout);
	assert(ENODATA == errno);
}

/* Setting pattern > 0 returns EOPNOTSUPP in errno. */
void test8()
{
	lustre_layout_t *layout;
	int rc;

	assert(layout = llapi_layout_alloc());
	errno = 0;
	rc = llapi_layout_pattern_set(layout, 1);
	assert(-1 == rc);
	assert(EOPNOTSUPP == errno);
	llapi_layout_free(layout);
}


#define T9FILE			"/t9"
#define T9_STRIPE_COUNT		2
#define T9_STRIPE_SIZE 		1048576
/* llapi_layout_create_file() returns EEXIST in errno for
 * already-existing file. */
void test9()
{
	int rc;
	int fd;
	lustre_layout_t *layout;
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T9FILE);

	(void) unlink(file);
	layout = llapi_layout_alloc();
	llapi_layout_stripe_count_set(layout, T9_STRIPE_COUNT);
	llapi_layout_stripe_size_set(layout, T9_STRIPE_SIZE);
	fd = llapi_layout_file_create(layout, file, 0750, 0);
	assert(0 <= fd);
	assert(0 == close(fd));
	errno = 0;
	rc = llapi_layout_file_create(layout, file, 0750, 0);
	assert(0 > rc);
	assert(EEXIST == errno);
	llapi_layout_free(layout);
}

/* Verify stripe_count interfaces return errors as expected */
void test10()
{
	int rc;
	lustre_layout_t *layout;

	layout = llapi_layout_alloc();
	assert(NULL != layout);

	/* stripe count less than -1 (-1 means stripe as widely as possible) */
	errno = 0;
	rc = llapi_layout_stripe_count_set(layout, -2);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_stripe_count_set(NULL, 2);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_stripe_count(NULL);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* stripe count too large */
	errno = 0;
	rc = llapi_layout_stripe_count_set(layout, LOV_MAX_STRIPE_COUNT + 1);
	assert(-1 == rc);
	assert(EINVAL == errno);
	llapi_layout_free(layout);
}

/* Verify stripe_size interfaces return errors as expected */
void test11()
{
	int rc;
	lustre_layout_t *layout;

	layout = llapi_layout_alloc();
	assert(NULL != layout);

	/* negative stripe size */
	errno = 0;
	rc = llapi_layout_stripe_size_set(layout, -1);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* stripe size too big */
	errno = 0;
	rc = llapi_layout_stripe_size_set(layout, 1ULL << 32);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_stripe_size_set(NULL, 1048576);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_stripe_size(NULL);
	assert(-1 == rc);
	assert(EINVAL == errno);

	llapi_layout_free(layout);
}

/* Verify pool_name interfaces return errors as expected */
void test12()
{
	int rc;
	lustre_layout_t *layout;
	const char *poolname;

	layout = llapi_layout_alloc();
	assert(NULL != layout);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_pool_name_set(NULL, "foo");
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* NULL pool name */
	errno = 0;
	rc = llapi_layout_pool_name_set(layout, NULL);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* NULL layout */
	errno = 0;
	poolname = llapi_layout_pool_name(NULL);
	assert(NULL == poolname);
	assert(EINVAL == errno);

	/* Pool name too long*/
	errno = 0;
	rc = llapi_layout_pool_name_set(layout, "0123456789abcdef0");
	assert(-1 == rc);
	assert(EINVAL == errno);

	llapi_layout_free(layout);
}

#define T13FILE			"/t13"
#define T13_STRIPE_COUNT	2
/* Verify ost_index interface returns errors as expected */
void test13()
{
	int rc;
	lustre_layout_t *layout;
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T13FILE);

	layout = llapi_layout_alloc();
	assert(NULL != layout);

	/* Only setting OST index for stripe 0 is supported for now. */
	errno = 0;
	rc = llapi_layout_ost_index_set(layout, 1, 1);
	assert(-1 == rc);
	assert(EOPNOTSUPP == errno);

	/* OST index less than one (-1 means let MDS choose) */
	errno = 0;
	rc = llapi_layout_ost_index_set(layout, 0, -2);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_ost_index_set(NULL, 0, 1);
	assert(-1 == rc);
	assert(EINVAL == errno);

	errno = 0;
	rc = llapi_layout_ost_index(NULL, 0);
	assert(-1 == rc);
	assert(EINVAL == errno);

	/* Layout not read from file so has no OST data. */
	errno = 0;
	assert(llapi_layout_stripe_count_set(layout, T13_STRIPE_COUNT) == 0);
	rc = llapi_layout_ost_index(layout, 0);
	assert(-1 == rc);
	assert(0 == errno);

	/* n greater than stripe count*/
	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);
	assert(llapi_layout_stripe_count_set(layout, T13_STRIPE_COUNT) == 0);
	rc = llapi_layout_file_create(layout, file, 0644, 0);
	assert(0 <= rc);
	close(rc);
	llapi_layout_free(layout);
	layout = llapi_layout_lookup_bypath(file);
	errno = 0;
	rc = llapi_layout_ost_index(layout, 3);
	assert(-1 == rc);
	assert(EINVAL == errno);

	llapi_layout_free(layout);
}

/* Verify llapi_layout_file_create() returns errors as expected */
void test14()
{
	int rc;

	/* NULL layout */
	errno = 0;
	rc = llapi_layout_file_create(NULL, "foo", 0, 0);
	assert(-1 == rc);
	assert(EINVAL == errno);
}

#define T15FILE			"/t15"
#define T15_STRIPE_COUNT	2
/* Can't change striping attributes of existing file. */
void test15()
{
	int rc;
	lustre_layout_t *layout;
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T15FILE);

	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);

	layout = llapi_layout_alloc();
	assert(NULL != layout);
	assert(llapi_layout_stripe_count_set(layout, T15_STRIPE_COUNT) == 0);
	errno = 0;
	rc = llapi_layout_file_create(layout, file, 0640, 0);
	assert(0 <= rc);
	assert(0 == errno);
	assert(0 == close(rc));

	assert(0 == llapi_layout_stripe_count_set(layout, T15_STRIPE_COUNT + 1));
	errno = 0;
	rc = llapi_layout_file_create(layout, file, 0640, 0);
	assert(0 > rc);
	assert(EEXIST == errno);
	llapi_layout_free(layout);

	layout = llapi_layout_lookup_bypath(file);
	assert(NULL != layout);
	assert(T15_STRIPE_COUNT == llapi_layout_stripe_count(layout));
	llapi_layout_free(layout);
}

#define T16FILE			"/t16"
/* Default stripe attributes are applied as expected. */
void test16()
{
	int rc;
	lustre_layout_t *dirlayout;
	lustre_layout_t *filelayout;
	char file[PATH_MAX];
	char lustre_mnt[PATH_MAX];

	rc = llapi_search_mounts(lustre_dir, 0, lustre_mnt, NULL);

	strcat(file, lustre_dir);
	strcat(file, T16FILE);

	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);
	assert(dirlayout = llapi_layout_lookup_bypath(lustre_mnt));
	assert(filelayout = llapi_layout_alloc());
	rc = llapi_layout_file_create(filelayout, file, 0640, 0);
	assert(0 <= rc);
	assert(0 == close(rc));
	llapi_layout_free(filelayout);
	assert(filelayout = llapi_layout_lookup_bypath(file));
	assert(llapi_layout_stripe_count(filelayout) ==
	       llapi_layout_stripe_count(dirlayout));
	assert(llapi_layout_stripe_size(filelayout) ==
	       llapi_layout_stripe_size(dirlayout));
	llapi_layout_free(filelayout);
	llapi_layout_free(dirlayout);
}

#define T17FILE			"/t17"
/* Setting stripe count to -1 uses all available OSTs. */
void test17()
{
	int rc;
	int fd;
	int ost_count;
	lustre_layout_t *layout;
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T17FILE);

	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);
	assert(layout = llapi_layout_alloc());
	llapi_layout_stripe_count_set(layout, -1);
	rc = llapi_layout_file_create(layout, file, 0640, 0);
	assert(0 <= rc);
	assert(0 == close(rc));
	llapi_layout_free(layout);

	/* Get number of available OSTs */
	fd = open(file, O_RDONLY);
	assert(-1 != fd);
	llapi_lov_get_uuids(fd, NULL, &ost_count);
	assert(0 == close(fd));
	assert(layout = llapi_layout_lookup_bypath(file));
	assert(llapi_layout_stripe_count(layout) == ost_count);

	llapi_layout_free(layout);
}

#define T18FILE			"/t18"
/* Setting pool with "fsname.pool" notation. */
void test18()
{
	int rc;
	lustre_layout_t *layout = llapi_layout_alloc();
	char file[PATH_MAX];
	char pool[LOV_MAXPOOLNAME*2 + 1];

	strcat(pool, "lustre.");
	strcat(pool, poolname);

	strcat(file, lustre_dir);
	strcat(file, T18FILE);

	assert(NULL != layout);

	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);

	assert(0==llapi_layout_pool_name_set(layout, pool));
	assert(0==strcmp(llapi_layout_pool_name(layout), poolname));
	rc = llapi_layout_file_create(layout, file, 0640, 0);
	assert(0 <= rc);
	assert(0 == close(rc));
	llapi_layout_free(layout);

	assert(layout = llapi_layout_lookup_bypath(file));
	assert(0 == strcmp(llapi_layout_pool_name(layout), poolname));
	llapi_layout_free(layout);
}

#define T19FILE			"/t19"
#define T19_STRIPE_COUNT	2
#define T19_STRIPE_SIZE 	1048576
/* Look up layout by fid. */
void test19()
{
	int rc;
	lustre_layout_t *layout;
	lustre_fid fid;
	char fidstr[4096];
	char file[PATH_MAX];

	strcat(file, lustre_dir);
	strcat(file, T19FILE);

	rc = unlink(file);
	assert(0 == rc || ENOENT == errno);

	layout = llapi_layout_alloc();
	assert(NULL != layout);
	rc = llapi_layout_stripe_size_set(layout, T19_STRIPE_SIZE);
	assert(0 == rc);
	rc = llapi_layout_stripe_count_set(layout, T19_STRIPE_COUNT);
	assert(0 == rc);
	rc = llapi_layout_file_create(layout, file, 0640, 0);
	assert(0 <= rc);
	assert(0 == close(rc));
	llapi_layout_free(layout);

	rc = llapi_path2fid(file, &fid);
	assert(0 == rc);
	snprintf(fidstr, sizeof(fidstr), "0x%llx:0x%x:0x%x", fid.f_seq,
		 fid.f_oid, fid.f_ver);
	errno = 0;
	layout = llapi_layout_lookup_byfid(file, fidstr);
	assert(layout);
	assert(T19_STRIPE_COUNT == llapi_layout_stripe_count(layout));
	assert(T19_STRIPE_SIZE == llapi_layout_stripe_size(layout));
	llapi_layout_free(layout);
}

/* Maximum length pool name is properly NULL-terminated. */
void test20()
{
	lustre_layout_t *layout;
	const char *str;
	char *name = "0123456789abcdef";

	assert(layout = llapi_layout_alloc());
	assert(0 == llapi_layout_pool_name_set(layout, name));
	str = llapi_layout_pool_name(layout);
	assert(strlen(name) == strlen(str));
	llapi_layout_free(layout);
}

void sigsegv(int signal) {
	printf("Segmentation fault\n");
	exit(1);
}

#define TEST_DESC_LEN	50
typedef struct test_tbl_entry {
	void (*test_fn)(void);
	char test_desc[TEST_DESC_LEN];
} test_tbl_t;

static test_tbl_t test_tbl[] = {
	{ &test0, "Read/write layout attributes then create a file" },
	{ &test1, "Read test0 file by path and verify attributes" },
	{ &test2, "Read test0 file by fd and verify attributes" },
	{ &test3, "Verify compatibility with 'lfs setstripe'" },
	{ &test4, "llapi_layout_lookup_bypath ENOENT handling" },
	{ &test5, "llapi_layout_lookup_byfd EBADF handling" },
	{ &test6, "llapi_layout_lookup_bypath EACCES handling" },
	{ &test7, "llapi_layout_lookup_bypath ENODATA handling" },
	{ &test8, "llapi_layout_pattern_set() EOPNOTSUPP handling" },
	{ &test9, "llapi_layout_create_file EEXIST handling" },
	{ &test10, "stripe_count error handling" },
	{ &test11, "stripe_size error handling" },
	{ &test12, "pool_name error handling" },
	{ &test13, "ost_index error handling" },
	{ &test14, "llapi_layout_file_create error handling" },
	{ &test15, "Can't change striping attributes of existing file" },
	{ &test16, "Default stripe attributes are applied as expected" },
	{ &test17, "Setting stripe count to -1 uses all available OSTs" },
	{ &test18, "Setting pool with fsname.pool notation" },
	{ &test19, "Look up layout by fid" },
	{ &test20, "Maximum length pool name is NULL-terminated" },
};
#define NUM_TESTS	21

/* This function runs a single test by forking the process.  This way,
 * if there is a segfault during a test, the test program won't crash. */
int test(void (*test_fn)(), const char *test_desc, int test_num) {
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
		printf(" %s\n", status ? "fail!" : "pass");
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
#define TEST(i) (test(test_tbl[i].test_fn, test_tbl[i].test_desc, i))

static void process_args(int argc, char *argv[])
{
	int c;

	while ((c = getopt(argc, argv, "d:p:o:")) != -1) {
		switch(c) {
		case 'd':
			strcpy(lustre_dir, optarg);
			break;
		case 'p':
			strcpy(poolname, optarg);
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

	process_args(argc, argv);
	if (lustre_dir[0] == '\0')
		strcpy(lustre_dir, "/mnt/lustre");
	if (poolname[0] == '\0')
		strcpy(poolname, "testpool");
	if (num_osts == -1)
		num_osts = 2;

	if (num_osts < 2) {
		fprintf(stderr, "Error: at least 2 OSTS are required\n");
		exit(1);
	}
	if (-1 == stat(lustre_dir, &s)) {
		fprintf(stderr, "Error: %s: %s\n", lustre_dir, strerror(errno));
		exit(1);
	} else if(!S_ISDIR(s.st_mode)) {
		fprintf(stderr, "Error: %s: not a directory\n", lustre_dir);
		exit(1);
	}

	signal(SIGSEGV, sigsegv);

	/* Play nice with Lustre test scripts. Non-line buffered output
	 * stream under I/O redirection may appear incorrectly. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (i = 0; i < NUM_TESTS; i++) {
		if (TEST(i) != 0)
			rc++;
	}
	return rc;
}
