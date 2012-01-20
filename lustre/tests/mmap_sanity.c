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
 * Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <sys/wait.h>

char *dir = NULL, *dir2 = NULL;
long page_size;
char mmap_sanity[256];


static void usage(void)
{
        printf("Usage: mmap_sanity -d dir [-m dir2]\n");
        printf("       dir      lustre mount point\n");
        printf("       dir2     another mount point\n");
        exit(127);
}

static int remote_tst(int tc, char *mnt);
static int mmap_run(int tc)
{
        pid_t child;
        int rc = 0;

        child = fork();
        if (child < 0)
                return errno;
        else if (child)
                return 0;

        if (dir2 != NULL) {
                rc = remote_tst(tc, dir2);
        } else {
                rc = EINVAL;
                fprintf(stderr, "invalid argument!\n");
        }
        _exit(rc);
}

static int mmap_initialize(char *myself)
{
        char buf[1024], *file;
        int fdr, fdw, count, rc = 0;
        
        page_size = sysconf(_SC_PAGESIZE);
        if (page_size == -1) {
                perror("sysconf(_SC_PAGESIZE)");
                return errno;
        }

        /* copy myself to lustre for another client */
        fdr = open(myself, O_RDONLY);
        if (fdr < 0) {
                perror(myself);
                return EINVAL;
        }
        file = strrchr(myself, '/');
        if (file == NULL) {
                fprintf(stderr, "can't get test filename\n");
                close(fdr);
                return EINVAL;
        }
        file++;
        sprintf(mmap_sanity, "%s/%s", dir, file);

        fdw = open(mmap_sanity, O_CREAT|O_WRONLY, 0777);
        if (fdw < 0) {
                perror(mmap_sanity);
                close(fdr);
                return EINVAL;
        }
        while ((count = read(fdr, buf, sizeof(buf))) != 0) {
                int writes;

                if (count < 0) {
                        perror("read()");
                        rc = errno;
                        break;
                }
                writes = write(fdw, buf, count);
                if (writes != count) {
                        perror("write()");
                        rc = errno;
                        break;
                }
        }
        close(fdr);
        close(fdw);
        return rc;
}

static void mmap_finalize()
{
        unlink(mmap_sanity);
}

/* basic mmap operation on single node */
static int mmap_tst1(char *mnt)
{
        char *ptr, mmap_file[256];
        int region, fd, rc = 0;

        region = page_size * 10;
        sprintf(mmap_file, "%s/%s", mnt, "mmap_file1");
        
        if (unlink(mmap_file) && errno != ENOENT) {
                perror("unlink()");
                return errno;
        }

        fd = open(mmap_file, O_CREAT|O_RDWR, 0600);
        if (fd < 0) {
                perror(mmap_file);
                return errno;
        }
        if (ftruncate(fd, region) < 0) {
                perror("ftruncate()");
                rc = errno;
                goto out_close;
        }

        ptr = mmap(NULL, region, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out_close;
        }
        memset(ptr, 'a', region);

        munmap(ptr, region);
out_close:
        close(fd);
        unlink(mmap_file);
        return rc;
}

/* MAP_PRIVATE create a copy-on-write mmap */
static int mmap_tst2(char *mnt)
{
        char *ptr, mmap_file[256], buf[256];
        int fd, rc = 0;

        sprintf(mmap_file, "%s/%s", mnt, "mmap_file2");

        if (unlink(mmap_file) && errno != ENOENT) {
                perror("unlink()");
                return errno;
        }

        fd = open(mmap_file, O_CREAT|O_RDWR, 0600);
        if (fd < 0) {
                perror(mmap_file);
                return errno;
        }
        if (ftruncate(fd, page_size) < 0) {
                perror("ftruncate()");
                rc = errno;
                goto out_close;
        }

        ptr = mmap(NULL, page_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (ptr == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out_close;
        }
        memcpy(ptr, "blah", strlen("blah"));

        munmap(ptr, page_size);
out_close:
        close(fd);
        if (rc)
                return rc;

        fd = open(mmap_file, O_RDONLY);
        if (fd < 0) {
                perror(mmap_file);
                return errno;
        }
        rc = read(fd, buf, sizeof(buf));
        if (rc < 0) {
                perror("read()");
                rc = errno;
                goto out_close;
        }
        rc = 0;
        
        if (strncmp("blah", buf, strlen("blah")) == 0) {
                fprintf(stderr, "mmap write back with MAP_PRIVATE!\n");
                rc = EFAULT;
        }
        close(fd);
        unlink(mmap_file);
        return rc;
}

/* concurrent mmap operations on two nodes */
static int mmap_tst3(char *mnt)
{
        char *ptr, mmap_file[256];
        int region, fd, rc = 0;

        region = page_size * 100;
        sprintf(mmap_file, "%s/%s", mnt, "mmap_file3");
        
        if (unlink(mmap_file) && errno != ENOENT) {
                perror("unlink()");
                return errno;
        }

        fd = open(mmap_file, O_CREAT|O_RDWR, 0600);
        if (fd < 0) {
                perror(mmap_file);
                return errno;
        }
        if (ftruncate(fd, region) < 0) {
                perror("ftruncate()");
                rc = errno;
                goto out_close;
        }

        ptr = mmap(NULL, region, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out_close;
        }

        rc = mmap_run(3);
        if (rc)
                goto out_unmap;
        
        memset(ptr, 'a', region);
        sleep(2);       /* wait for remote test finish */
out_unmap:
        munmap(ptr, region);
out_close:
        close(fd);
        unlink(mmap_file);
        return rc;
}       

static int remote_tst3(char *mnt)
{
        char *ptr, mmap_file[256];
        int region, fd, rc = 0;

        region = page_size * 100;
        sprintf(mmap_file, "%s/%s", mnt, "mmap_file3");

        fd = open(mmap_file, O_RDWR, 0600);
        if (fd < 0) {
                perror(mmap_file);
                return errno;
        }

        ptr = mmap(NULL, region, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out_close;
        }
        memset(ptr, 'b', region);
        memset(ptr, 'c', region);
        
        munmap(ptr, region);
out_close:
        close(fd);
        return rc;
}

/* client1 write to file_4a from mmap()ed file_4b;
 * client2 write to file_4b from mmap()ed file_4a. */
static int mmap_tst4(char *mnt)
{
        char *ptr, filea[256], fileb[256];
        int region, fdr, fdw, rc = 0;

        region = page_size * 100;
        sprintf(filea, "%s/%s", mnt, "mmap_file_4a");
        sprintf(fileb, "%s/%s", mnt, "mmap_file_4b");

        if (unlink(filea) && errno != ENOENT) {
                perror("unlink()");
                return errno;
        }
        if (unlink(fileb) && errno != ENOENT) {
                perror("unlink()");
                return errno;
        }

        fdr = fdw = -1;
        fdr = open(fileb, O_CREAT|O_RDWR, 0600);
        if (fdr < 0) {
                perror(fileb);
                return errno;
        }
        if (ftruncate(fdr, region) < 0) {
                perror("ftruncate()");
                rc = errno;
                goto out_close;
        }
        fdw = open(filea, O_CREAT|O_RDWR, 0600);
        if (fdw < 0) {
                perror(filea);
                rc = errno;
                goto out_close;
        }
        if (ftruncate(fdw, region) < 0) {
                perror("ftruncate()");
                rc = errno;
                goto out_close;
        }
        
        ptr = mmap(NULL, region, PROT_READ|PROT_WRITE, MAP_SHARED, fdr, 0);
        if (ptr == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out_close;
        }

        rc = mmap_run(4);
        if (rc)
                goto out_unmap;
        
        memset(ptr, '1', region);
        
        rc = write(fdw, ptr, region);
        if (rc <= 0) {
                perror("write()");
                rc = errno;
        } else
                rc = 0;

        sleep(2);       /* wait for remote test finish */
out_unmap:
        munmap(ptr, region);
out_close:
        if (fdr >= 0)
                close(fdr);
        if (fdw >= 0)
                close(fdw);
        unlink(filea);
        unlink(fileb);
        return rc;
}

static int remote_tst4(char *mnt)
{
        char *ptr, filea[256], fileb[256];
        int region, fdr, fdw, rc = 0;

        region = page_size * 100;
        sprintf(filea, "%s/%s", mnt, "mmap_file_4a");
        sprintf(fileb, "%s/%s", mnt, "mmap_file_4b");

        fdr = fdw = -1;
        fdr = open(filea, O_RDWR, 0600);
        if (fdr < 0) {
                perror(filea);
                return errno;
        }
        fdw = open(fileb, O_RDWR, 0600);
        if (fdw < 0) {
                perror(fileb);
                rc = errno;
                goto out_close;
        }

        ptr = mmap(NULL, region, PROT_READ|PROT_WRITE, MAP_SHARED, fdr, 0);
        if (ptr == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out_close;
        }

        memset(ptr, '2', region);

        rc = write(fdw, ptr, region);
        if (rc <= 0) {
                perror("write()");
                rc = errno;
        } else
                rc = 0;
     
        munmap(ptr, region);
out_close:
        if (fdr >= 0)
                close(fdr);
        if (fdw >= 0)
                close(fdw);
        return rc;
}

static int cancel_lru_locks(char *prefix)
{
        char cmd[256], line[1024];
        FILE *file;
        pid_t child;
        int len = 1024, rc = 0;

        child = fork();
        if (child < 0)
                return errno;
        else if (child) {
                int status;
                rc = waitpid(child, &status, WNOHANG);
                if (rc == child)
                        rc = 0;
                return rc;
        }

        if (prefix)
                sprintf(cmd, "ls /proc/fs/lustre/ldlm/namespaces/*-%s-*/lru_size", prefix);
        else
                sprintf(cmd, "ls /proc/fs/lustre/ldlm/namespaces/*/lru_size");

        file = popen(cmd, "r");
        if (file == NULL) {
                perror("popen()");
                return errno;
        }

        while (fgets(line, len, file)) {
                FILE *f;

                if (!strlen(line))
                        continue;
                /* trim newline character */
                *(line + strlen(line) - 1) = '\0';
                f = fopen(line, "w");
                if (f == NULL) {
                        perror("fopen()");
                        rc = errno;
                        break;
                }
                rc = fwrite("clear", strlen("clear") + 1, 1, f);
                if (rc < 1) {
                        perror("fwrite()");
                        rc = errno;
                        fclose(f);
                        break;
                }
                fclose(f);
        }

        pclose(file);
        _exit(rc);
}

/* don't dead lock while read/write file to/from the buffer which
 * mmaped to just this file */
static int mmap_tst5(char *mnt)
{
        char *ptr, mmap_file[256];
        int region, fd, off, rc = 0;

        region = page_size * 40;
        off = page_size * 10;
        sprintf(mmap_file, "%s/%s", mnt, "mmap_file5");

        if (unlink(mmap_file) && errno != ENOENT) {
                perror("unlink()");
                return errno;
        }

        fd = open(mmap_file, O_CREAT|O_RDWR, 0600);
        if (fd < 0) {
                perror(mmap_file);
                return errno;
        }
        if (ftruncate(fd, region) < 0) {
                perror("ftruncate()");
                rc = errno;
                goto out_close;
        }

        ptr = mmap(NULL, region, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out_close;
        }
        memset(ptr, 'a', region);

        /* cancel unused locks */
        rc = cancel_lru_locks("osc");
        if (rc)
                goto out_unmap;

        /* read/write region of file and buffer should be overlap */
        rc = read(fd, ptr + off, off * 2);
        if (rc != off * 2) {
                perror("read()");
                rc = errno;
                goto out_unmap;
        }
        rc = write(fd, ptr + off, off * 2);
        if (rc != off * 2) {
                perror("write()");
                rc = errno;
        }
        rc = 0;
out_unmap:
        munmap(ptr, region);
out_close:
        close(fd);
        unlink(mmap_file);
        return rc;
}

/* mmap write to a file form client1 then mmap read from client2 */
static int mmap_tst6(char *mnt)
{
        char mmap_file[256], mmap_file2[256];
        char *ptr = NULL, *ptr2 = NULL;
        int fd = 0, fd2 = 0, rc = 0;

        sprintf(mmap_file, "%s/%s", mnt, "mmap_file6");
        sprintf(mmap_file2, "%s/%s", dir2, "mmap_file6");
        if (unlink(mmap_file) && errno != ENOENT) {
                perror("unlink()");
                return errno;
        }

        fd = open(mmap_file, O_CREAT|O_RDWR, 0600);
        if (fd < 0) {
                perror(mmap_file);
                return errno;
        }
        if (ftruncate(fd, page_size) < 0) {
                perror("ftruncate()");
                rc = errno;
                goto out;
        }

        fd2 = open(mmap_file2, O_RDWR, 0600);
        if (fd2 < 0) {
                perror(mmap_file2);
                goto out;
        }

        ptr = mmap(NULL, page_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out;
        }
        
        ptr2 = mmap(NULL, page_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd2, 0);
        if (ptr2 == MAP_FAILED) {
                perror("mmap()");
                rc = errno;
                goto out;
        }

        rc = cancel_lru_locks("osc");
        if (rc)
                goto out;

        memcpy(ptr, "blah", strlen("blah"));
        if (strncmp(ptr, ptr2, strlen("blah"))) {
                fprintf(stderr, "client2 mmap mismatch!\n");
                rc = EFAULT;
                goto out;
        }
        memcpy(ptr2, "foo", strlen("foo"));
        if (strncmp(ptr, ptr2, strlen("foo"))) {
                fprintf(stderr, "client1 mmap mismatch!\n");
                rc = EFAULT;
        }
out:
        if (ptr2)
                munmap(ptr2, page_size);
        if (ptr)
                munmap(ptr, page_size);
        if (fd2 > 0)
                close(fd2);
        if (fd > 0)
                close(fd);
        unlink(mmap_file);
        return rc;
}

static int mmap_tst7_func(char *mnt, int rw)
{
        char  fname[256];
        char *buf = MAP_FAILED;
        ssize_t bytes;
        int fd = -1;
        int rc = 0;

        if (snprintf(fname, 256, "%s/mmap_tst7.%s",
                     mnt, (rw == 0) ? "read":"write") >= 256) {
                fprintf(stderr, "dir name too long\n");
                rc = ENAMETOOLONG;
                goto out;
        }
        fd = open(fname, O_RDWR | O_DIRECT | O_CREAT, 0644);
        if (fd == -1) {
                perror("open");
                rc = errno;
                goto out;
        }
        if (ftruncate(fd, 2 * page_size) == -1) {
                perror("truncate");
                rc = errno;
                goto out;
        }
        buf = mmap(NULL, page_size * 2,
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED) {
                perror("mmap");
                rc = errno;
                goto out;
        }
        /* ensure the second page isn't mapped */
        munmap(buf + page_size, page_size);
        bytes = (rw == 0) ? read(fd, buf, 2 * page_size) :
                write(fd, buf, 2 * page_size);
        /* Expected behavior */
        if (bytes == page_size)
                goto out;
        fprintf(stderr, "%s returned %zd, errno = %d\n",
                (rw == 0)?"read":"write", bytes, errno);
        rc = EIO;
out:
        if (buf != MAP_FAILED)
                munmap(buf, page_size);
        if (fd != -1)
                close(fd);
        return rc;
}

static int mmap_tst7(char *mnt)
{
        int rc;

        rc = mmap_tst7_func(mnt, 0);
        if (rc != 0)
                return rc;
        rc = mmap_tst7_func(mnt, 1);
        return rc;
}

static int mmap_tst8(char *mnt)
{
        char  fname[256];
        char *buf = MAP_FAILED;
        int fd = -1;
        int rc = 0;
        pid_t pid;
        char xyz[page_size * 2];

        if (snprintf(fname, 256, "%s/mmap_tst8", mnt) >= 256) {
                fprintf(stderr, "dir name too long\n");
                rc = ENAMETOOLONG;
                goto out;
        }
        fd = open(fname, O_RDWR | O_CREAT, 0644);
        if (fd == -1) {
                perror("open");
                rc = errno;
                goto out;
        }
        if (ftruncate(fd, page_size) == -1) {
                perror("truncate");
                rc = errno;
                goto out;
        }
        buf = mmap(NULL, page_size * 2,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (buf == MAP_FAILED) {
                perror("mmap");
                rc = errno;
                goto out;
        }

        pid = fork();
        if (pid == 0) { /* child */
                memcpy(xyz, buf, page_size * 2);
                /* shouldn't reach here. */
                exit(0);
        } else if (pid > 0) { /* parent */
                int status = 0;
                pid = waitpid(pid, &status, 0);
                if (pid < 0) {
                        perror("wait");
                        rc = errno;
                        goto out;
                }

                rc = EFAULT;
                if (WIFSIGNALED(status) && SIGBUS == WTERMSIG(status))
                        rc = 0;
        } else {
                perror("fork");
                rc = errno;
        }

out:
        if (buf != MAP_FAILED)
                munmap(buf, page_size);
        if (fd != -1)
                close(fd);
        return rc;
}

static int remote_tst(int tc, char *mnt)
{
        int rc = 0;
        switch(tc) {
        case 3:
                rc = remote_tst3(mnt);
                break;
        case 4:
                rc = remote_tst4(mnt);
                break;
        default:
                fprintf(stderr, "wrong test case number %d\n", tc);
                rc = EINVAL;
                break;
        }
        return rc;
}

struct test_case {
        int     tc;                     /* test case number */
        char    *desc;                  /* test description */
        int     (* test_fn)(char *mnt); /* test function */
        int     node_cnt;               /* node count */
};

struct test_case tests[] = {
        { 1, "mmap test1: basic mmap operation", mmap_tst1, 1 },
        { 2, "mmap test2: MAP_PRIVATE not write back", mmap_tst2, 1 },
        { 3, "mmap test3: concurrent mmap ops on two nodes", mmap_tst3, 2 },
        { 4, "mmap test4: c1 write to f1 from mmapped f2, " 
             "c2 write to f1 from mmapped f1", mmap_tst4, 2 },
        { 5, "mmap test5: read/write file to/from the buffer "
             "which mmapped to just this file", mmap_tst5, 1 },
        { 6, "mmap test6: check mmap write/read content on two nodes", 
                mmap_tst6, 2 },
        { 7, "mmap test7: file i/o with an unmapped buffer", mmap_tst7, 1},
        { 8, "mmap test8: SIGBUS for beyond file size", mmap_tst8, 1},
        { 0, NULL, 0, 0 }
};

int main(int argc, char **argv)
{
        extern char *optarg;
        struct test_case *test;
        int c, rc = 0;

        for(;;) {
                c = getopt(argc, argv, "d:m:");
                if ( c == -1 )
                        break;

                switch(c) {
                        case 'd':
                                dir = optarg;
                                break;
                        case 'm':
                                dir2 = optarg;
                                break;
                        default:
                        case '?':
                                usage();
                                break;
                }
        }

        if (dir == NULL)
                usage();

        if (mmap_initialize(argv[0]) != 0) {
                fprintf(stderr, "mmap_initialize failed!\n");
                return EINVAL;
        }

        for (test = tests; test->tc; test++) {
                char *rs = "skip";
                rc = 0;
                if (test->node_cnt == 1 || dir2 != NULL) {
                        rc = test->test_fn(dir);
                        rs = rc ? "fail" : "pass";
                }
                fprintf(stderr, "%s (%s)\n", test->desc, rs);
                if (rc)
                        break;
        }

        mmap_finalize();
        return rc;
}
