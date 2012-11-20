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
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pull in O_DIRECTORY in bits/fcntl.h */
#endif
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <semaphore.h>

#include <lustre/lustreapi.h>

#define T1 "write data before unlink\n"
#define T2 "write data after unlink\n"
char msg[] = "yabba dabba doo, I'm coming for you, I live in a shoe, I don't know what to do.\n'Bigger, bigger,and bigger yet!' cried the Creator.  'You are not yet substantial enough for my boundless intents!'  And ever greater and greater the object became, until all was lost 'neath its momentus bulk.\n";
char *buf, *buf_align;
int bufsize = 0;
sem_t sem;
#define ALIGN 65535

char usage[] =
"Usage: %s filename command-sequence\n"
"    command-sequence items:\n"
"        c  close\n"
"        C[num] create with optional stripes\n"
"        d  mkdir\n"
"        D  open(O_DIRECTORY)\n"
"        f  statfs\n"
"        G gid get grouplock\n"
"        g gid put grouplock\n"
"        L  link\n"
"        l  symlink\n"
"        m  mknod\n"
"        M  rw mmap to EOF (must open and stat prior)\n"
"        N  rename\n"
"        o  open(O_RDONLY)\n"
"        O  open(O_CREAT|O_RDWR)\n"
"        r[num] read [optional length]\n"
"        R  reference entire mmap-ed region\n"
"        s  stat\n"
"        S  fstat\n"
"        t  fchmod\n"
"        T[num] ftruncate [optional position, default 0]\n"
"        u  unlink\n"
"        U  munmap\n"
"        v  verbose\n"
"        w[num] write optional length\n"
"        W  write entire mmap-ed region\n"
"        y  fsync\n"
"        Y  fdatasync\n"
"        z[num] seek [optional position, default 0]\n"
"        _  wait for signal\n";

void usr1_handler(int unused)
{
        int saved_errno = errno;

        /*
         * signal(7): POSIX.1-2004 ...requires an implementation to guarantee
         * that the following functions can be safely called inside a signal
         * handler:
         *            sem_post()
         */
        sem_post(&sem);

        errno = saved_errno;
}

static const char *
pop_arg(int argc, char *argv[])
{
        static int cur_arg = 3;

        if (cur_arg >= argc)
                return NULL;

        return argv[cur_arg++];
}

struct flag_mapping {
       const char *string;
       const int  flag;
} flag_table[] = {
       {"O_RDONLY", O_RDONLY},
       {"O_WRONLY", O_WRONLY},
       {"O_RDWR", O_RDWR},
       {"O_CREAT", O_CREAT},
       {"O_EXCL", O_EXCL},
       {"O_NOCTTY", O_NOCTTY},
       {"O_TRUNC", O_TRUNC},
       {"O_APPEND", O_APPEND},
       {"O_NONBLOCK", O_NONBLOCK},
       {"O_NDELAY", O_NDELAY},
       {"O_SYNC", O_SYNC},
#ifdef O_DIRECT
       {"O_DIRECT", O_DIRECT},
#endif
       {"O_LARGEFILE", O_LARGEFILE},
       {"O_DIRECTORY", O_DIRECTORY},
       {"O_NOFOLLOW", O_NOFOLLOW},
       {"", -1}
};

int get_flags(char *data, int *rflags)
{
        char *cloned_flags;
        char *tmp;
        int flag_set = 0;
        int flags = 0;
        int size = 0;

        cloned_flags = strdup(data);
        if (cloned_flags == NULL) {
                fprintf(stderr, "Insufficient memory.\n");
                exit(-1);
        }

        for (tmp = strtok(cloned_flags, ":"); tmp;
             tmp = strtok(NULL, ":")) {
                int i;

                size = tmp - cloned_flags;
                for (i = 0; flag_table[i].flag != -1; i++) {
                        if (!strcmp(tmp, flag_table[i].string)){
                                flags |= flag_table[i].flag;
                                size += strlen(flag_table[i].string);
                                flag_set = 1;
                                break;
                        }
                }
        }
        free(cloned_flags);

        if (!flag_set) {
                *rflags = O_RDONLY;
                return 0;
        }

        *rflags = flags;
        return size;
}

#define POP_ARG() (pop_arg(argc, argv))

int main(int argc, char **argv)
{
        char *fname, *commands;
        const char *newfile;
        struct stat st;
        struct statfs stfs;
        size_t mmap_len = 0, i;
        unsigned char *mmap_ptr = NULL, junk = 0;
        int rc, len, fd = -1;
        int flags;
        int save_errno;
        int verbose = 0;
        int gid = 0;

        if (argc < 3) {
                fprintf(stderr, usage, argv[0]);
                exit(1);
        }

        memset(&st, 0, sizeof(st));
        sem_init(&sem, 0, 0);
        /* use sigaction instead of signal to avoid SA_ONESHOT semantics */
        sigaction(SIGUSR1, &(const struct sigaction){.sa_handler = &usr1_handler},
                  NULL);

        fname = argv[1];

        for (commands = argv[2]; *commands; commands++) {
                switch (*commands) {
                case '_':
                        if (verbose) {
                                printf("PAUSING\n");
                                fflush(stdout);
                        }
                        while (sem_wait(&sem) == -1 && errno == EINTR);
                        break;
                case 'c':
                        if (close(fd) == -1) {
                                save_errno = errno;
                                perror("close");
                                exit(save_errno);
                        }
                        fd = -1;
                        break;
                case 'C':
                        len = atoi(commands+1);
                        fd = llapi_file_open(fname, O_CREAT | O_WRONLY, 0644,
                                             0, 0, len, 0);
                        if (fd == -1) {
                                save_errno = errno;
                                perror("create stripe file");
                                exit(save_errno);
                        }
                        break;
                case 'd':
                        if (mkdir(fname, 0755) == -1) {
                                save_errno = errno;
                                perror("mkdir(0755)");
                                exit(save_errno);
                        }
                        break;
                case 'D':
                        fd = open(fname, O_DIRECTORY);
                        if (fd == -1) {
                                save_errno = errno;
                                perror("open(O_DIRECTORY)");
                                exit(save_errno);
                        }
                        break;
                case 'f':
                        if (statfs(fname, &stfs) == -1) {
                                save_errno = errno;
                                perror("statfs()");
                                exit(save_errno);
                        }
                        break;
                case 'G':
                        gid = atoi(commands+1);
                        if (ioctl(fd, LL_IOC_GROUP_LOCK, gid) == -1) {
                                save_errno = errno;
                                perror("ioctl(GROUP_LOCK)");
                                exit(save_errno);
                        }
                        break;
                case 'g':
                        gid = atoi(commands+1);
                        if (ioctl(fd, LL_IOC_GROUP_UNLOCK, gid) == -1) {
                                save_errno = errno;
                                perror("ioctl(GROUP_UNLOCK)");
                                exit(save_errno);
                        }
                        break;
                case 'l':
                        newfile = POP_ARG();
                        if (!newfile)
                                newfile = fname;
                        if (symlink(fname, newfile)) {
                                save_errno = errno;
                                perror("symlink()");
                                exit(save_errno);
                        }
                        break;
                case 'L':
                        newfile = POP_ARG();
                        if (!newfile)
                                newfile = fname;
                        if (link(fname, newfile)) {
                                save_errno = errno;
                                perror("symlink()");
                                exit(save_errno);
                        }
                        break;
                case 'm':
                        if (mknod(fname, S_IFREG | 0644, 0) == -1) {
                                save_errno = errno;
                                perror("mknod(S_IFREG|0644, 0)");
                                exit(save_errno);
                        }
                        break;
                case 'M':
                        if (st.st_size == 0) {
                                fprintf(stderr, "mmap without preceeding stat, or on"
                                        " zero length file.\n");
                                exit(-1);
                        }
                        mmap_len = st.st_size;
                        mmap_ptr = mmap(NULL, mmap_len, PROT_WRITE | PROT_READ,
                                        MAP_SHARED, fd, 0);
                        if (mmap_ptr == MAP_FAILED) {
                                save_errno = errno;
                                perror("mmap");
                                exit(save_errno);
                        }
                        break;
                case 'N':
                        newfile = POP_ARG();
                        if (!newfile)
                                newfile = fname;
                        if (rename (fname, newfile)) {
                                save_errno = errno;
                                perror("rename()");
                                exit(save_errno);
                        }
                        break;
                case 'O':
                        fd = open(fname, O_CREAT|O_RDWR, 0644);
                        if (fd == -1) {
                                save_errno = errno;
                                perror("open(O_RDWR|O_CREAT)");
                                exit(save_errno);
                        }
                        break;
                case 'o':
                        len = get_flags(commands+1, &flags);
                        commands += len;
                        if (flags & O_CREAT)
                                fd = open(fname, flags, 0666);
                        else
                                fd = open(fname, flags);
                        if (fd == -1) {
                                save_errno = errno;
                                perror("open");
                                exit(save_errno);
                        }
                        break;
                case 'r':
                        len = atoi(commands+1);
                        if (len <= 0)
                                len = 1;
                        if (bufsize < len) {
                                buf = realloc(buf, len + ALIGN);
                                if (buf == NULL) {
                                        save_errno = errno;
                                        perror("allocating buf for read\n");
                                        exit(save_errno);
                                }
                                bufsize = len;
                                buf_align = (char *)((long)(buf + ALIGN) &
                                                     ~ALIGN);
                        }
                        while (len > 0) {
                                rc = read(fd, buf_align, len);
                                if (rc == -1) {
                                        save_errno = errno;
                                        perror("read");
                                        exit(save_errno);
                                }
				if (rc < len) {
					fprintf(stderr, "short read: %u/%u\n",
						rc, len);
					if (rc == 0)
						exit(ENODATA);
				}
                                len -= rc;
                                if (verbose >= 2)
                                        printf("%.*s\n", rc, buf_align);
                        }
                        break;
                case 'R':
                        for (i = 0; i < mmap_len && mmap_ptr; i += 4096)
                                junk += mmap_ptr[i];
                        break;
                case 's':
                        if (stat(fname, &st) == -1) {
                                save_errno = errno;
                                perror("stat");
                                exit(save_errno);
                        }
                        break;
                case 'S':
                        if (fstat(fd, &st) == -1) {
                                save_errno = errno;
                                perror("fstat");
                                exit(save_errno);
                        }
                        break;
                case 't':
                        if (fchmod(fd, 0) == -1) {
                                save_errno = errno;
                                perror("fchmod");
                                exit(save_errno);
                        }
                        break;
                case 'T':
                        len = atoi(commands+1);
                        if (ftruncate(fd, len) == -1) {
                                save_errno = errno;
                                printf("ftruncate (%d,%d)\n", fd, len);
                                perror("ftruncate");
                                exit(save_errno);
                        }
                        break;
                case 'u':
                        if (unlink(fname) == -1) {
                                save_errno = errno;
                                perror("unlink");
                                exit(save_errno);
                        }
                        break;
                case 'U':
                        if (munmap(mmap_ptr, mmap_len)) {
                                save_errno = errno;
                                perror("munmap");
                                exit(save_errno);
                        }
                        break;
                case 'v':
                        verbose++;
                        break;
                case 'w':
                        len = atoi(commands+1);
                        if (len <= 0)
                                len = 1;
                        if (bufsize < len) {
                                buf = realloc(buf, len + ALIGN);
                                if (buf == NULL) {
                                        save_errno = errno;
                                        perror("allocating buf for write\n");
                                        exit(save_errno);
                                }
                                bufsize = len;
                                buf_align = (char *)((long)(buf + ALIGN) &
                                                     ~ALIGN);
                                strncpy(buf_align, msg, bufsize);
                        }
                        while (len > 0) {
                                rc = write(fd, buf_align, len);
                                if (rc == -1) {
                                        save_errno = errno;
                                        perror("write");
                                        exit(save_errno);
                                }
                                if (rc < len)
                                        fprintf(stderr, "short write: %u/%u\n",
                                                rc, len);
                                len -= rc;
                        }
                        break;
                case 'W':
                        for (i = 0; i < mmap_len && mmap_ptr; i += 4096)
                                mmap_ptr[i] += junk++;
                        break;
                case 'y':
                        if (fsync(fd) == -1) {
                                save_errno = errno;
                                perror("fsync");
                                exit(save_errno);
                        }
                        break;
                case 'Y':
                        if (fdatasync(fd) == -1) {
                                save_errno = errno;
                                perror("fdatasync");
                                exit(save_errno);
                        }
                case 'z':
                        len = atoi(commands+1);
                        if (lseek(fd, len, SEEK_SET) == -1) {
                                save_errno = errno;
                                perror("lseek");
                                exit(save_errno);
                        }
                        break;
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                        break;
                default:
                        fprintf(stderr, "unknown command \"%c\"\n", *commands);
                        fprintf(stderr, usage, argv[0]);
                        exit(1);
                }
        }

        if (buf)
                free(buf);

        return 0;
}
