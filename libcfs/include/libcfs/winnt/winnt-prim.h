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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * libcfs/include/libcfs/winnt/winnt-prim.h
 *
 * Basic library routines.
 */

#ifndef __LIBCFS_WINNT_CFS_PRIM_H__
#define __LIBCFS_WINNT_CFS_PRIM_H__

#ifndef __LIBCFS_LIBCFS_H__
#error Do not #include this file directly. #include <libcfs/libcfs.h> instead
#endif

/*
 * libcfs proc device object
 */


#define LUSTRE_PROC_DEVICE  L"\\Device\\LNetProcFS"      /* proc fs emulator device object */
#define LUSTRE_PROC_SYMLNK  L"\\DosDevices\\LNetProcFS"  /* proc fs user-visible device */


/*
 * Device IO Control Code Definitions
 */

#define FILE_DEVICE_LIBCFS      ('LC')

#define FUNC_LIBCFS_VERSION     0x101  // get version of current libcfs
#define FUNC_LIBCFS_IOCTL       0x102  // Device i/o control to proc fs


#define IOCTL_LIBCFS_VERSION \
     CTL_CODE (FILE_DEVICE_LIBCFS, FUNC_LIBCFS_VERSION, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LIBCFS_ENTRY   \
     CTL_CODE(FILE_DEVICE_LIBCFS, FUNC_LIBCFS_IOCTL,   METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(4)
typedef struct _CFS_PROC_IOCTL {

    ULONG           cmd;    // ioctl command identifier
    ULONG           len;    // length of data
    int             rc;     // return code
    ULONG           usused; // unused 

    // UCHAR        data[]; // content of the real ioctl

} CFS_PROC_IOCTL, *PCFS_PROC_IOCTL;
#pragma pack()

#ifdef __KERNEL__

void cfs_enter_debugger(void);
#define __builtin_return_address(x) (0)

/*
 * Symbol functions for libcfs
 *
 * OSX has no facility for use to register symbol.
 * So we have to implement it.
 */
#define CFS_SYMBOL_LEN     64

struct  cfs_symbol {
	char       name[CFS_SYMBOL_LEN];
	void      *value;
	int        ref;
	cfs_list_t sym_list;
};

extern int      cfs_symbol_register(const char *, const void *);
extern void     cfs_symbol_unregister(const char *);
extern void *   cfs_symbol_get(const char *);
extern void     cfs_symbol_put(const char *);
extern void     cfs_symbol_clean();

typedef struct file_operations cfs_file_operations_t;
typedef struct file cfs_file_t;

/*
 * Pseudo device register
 */

typedef struct
{
    int                     minor;
    const char *            name;
    cfs_file_operations_t * fops;
} cfs_psdev_t;

int cfs_psdev_register(cfs_psdev_t * psdev);
int cfs_psdev_deregister(cfs_psdev_t * psdev);


/*
 * Proc emulator file system APIs
 */

typedef int cfs_read_proc_t(char *page, char **start, off_t off,
                            int count, int *eof, void *data);
typedef int cfs_write_proc_t(struct file *file, const char *buffer,
                             unsigned long count, void *data);

#define CFS_PROC_ENTRY_MAGIC 'CPEM'

#define CFS_PROC_FLAG_DIRECTORY    0x00000001 // directory node
#define CFS_PROC_FLAG_ATTACHED     0x00000002 // node is attached to proc
#define CFS_PROC_FLAG_MISCDEV      0x00000004 // miscellaneous device

typedef struct cfs_proc_entry
{
    ULONG                   magic;      // Magic
    ULONG                   flags;      // Flags

    struct _dir_entry {                 // proc directory entry
        PRTL_SPLAY_LINKS    root;
    };

    struct cfs_proc_entry  *parent;

    struct _file_entry {                // proc file / leaf entry
	    cfs_read_proc_t  *  read_proc;
	    cfs_write_proc_t *  write_proc;
    };

    mode_t                  mode;
    unsigned short          nlink;
    BOOLEAN                 deleted;

	
    struct file_operations *proc_fops;
    void                   *data;

    // proc_dir_entry ended.

    RTL_SPLAY_LINKS         s_link;       // splay link

    //
    // Maximum length of proc entry name is 0x20
    //

    char                    name[0x20];

} cfs_proc_entry_t, cfs_proc_dir_entry_t;

typedef cfs_proc_entry_t cfs_proc_dir_entry_t;
#define proc_dir_entry cfs_proc_entry

#define PROC_BLOCK_SIZE    PAGE_SIZE

struct proc_dir_entry *PDE(const struct inode *inode);


/*
 * Sysctl register
 */

typedef struct ctl_table                cfs_sysctl_table_t;
typedef struct ctl_table_header         cfs_sysctl_table_header_t;


typedef int ctl_handler (
            cfs_sysctl_table_t *table,
            int *name,  int nlen,
            void *oldval, size_t *oldlenp,
            void *newval, size_t newlen, 
            void **context );

typedef int proc_handler (
            cfs_sysctl_table_t *ctl,
            int write, struct file * filp,
            void *buffer, size_t *lenp );


int proc_dointvec(cfs_sysctl_table_t *table, int write, struct file *filp,
		     void *buffer, size_t *lenp);

int proc_dostring(cfs_sysctl_table_t *table, int write, struct file *filp,
		  void *buffer, size_t *lenp);

int sysctl_string(cfs_sysctl_table_t *table, int *name, int nlen,
		  void *oldval, size_t *oldlenp,
		  void *newval, size_t newlen, void **context);

/*
 *  System io control definitions
 */

#define CTL_MAXNAME 10

#define CTL_ANY     -1  /* Matches any name */
#define CTL_NONE    0

enum
{
    CTL_KERN=1,     /* General kernel info and control */
    CTL_VM=2,       /* VM management */
    CTL_NET=3,      /* Networking */
    CTL_PROC=4,     /* Process info */
    CTL_FS=5,       /* Filesystems */
    CTL_DEBUG=6,        /* Debugging */
    CTL_DEV=7,      /* Devices */
    CTL_BUS=8,      /* Busses */
    CTL_ABI=9,      /* Binary emulation */
    CTL_CPU=10      /* CPU stuff (speed scaling, etc) */
};

/* sysctl table definitons */
struct ctl_table 
{
	int ctl_name;
	char *procname;
	void *data;
	int maxlen;
	mode_t mode;
	cfs_sysctl_table_t *child;
	proc_handler *proc_handler;	/* text formatting callback */
	ctl_handler *strategy;		/* read / write callback functions */
	cfs_proc_entry_t *de;	/* proc entry block */
	void *extra1;
	void *extra2;
};


/* the mantaner of the cfs_sysctl_table trees */
struct ctl_table_header
{
	cfs_sysctl_table_t *    ctl_table;
	cfs_list_t              ctl_entry;
};

/* proc root entries, support routines */
extern cfs_proc_entry_t *  cfs_proc_root;   /* / */
extern cfs_proc_entry_t *  cfs_proc_proc;   /* /proc */
extern cfs_proc_entry_t *  cfs_proc_fs;     /* /proc/fs */
extern cfs_proc_entry_t *  cfs_proc_sys;    /* /proc/sys */
extern cfs_proc_entry_t *  cfs_proc_dev;    /* /dev */

cfs_proc_entry_t * create_proc_entry(const char *name, mode_t mod,
					  cfs_proc_entry_t *parent);
void proc_free_entry(cfs_proc_entry_t *de);
void remove_proc_entry(const char *name, cfs_proc_entry_t *entry);
cfs_proc_entry_t * search_proc_entry(const char * name,
                        cfs_proc_entry_t *  root );
cfs_proc_entry_t *proc_symlink(const char *name,
		                       cfs_proc_entry_t *parent,
                               const char *dest);
cfs_proc_entry_t *proc_mkdir(const char *name,
		                     cfs_proc_entry_t *parent);

#define cfs_create_proc_entry create_proc_entry
#define cfs_free_proc_entry   proc_free_entry
#define cfs_remove_proc_entry remove_proc_entry

struct ctl_table_header *register_sysctl_table(cfs_sysctl_table_t * table,
                                               int insert_at_head);
void unregister_sysctl_table(struct ctl_table_header * header);

#define cfs_register_sysctl_table(t, a)   register_sysctl_table(t, a)
#define cfs_unregister_sysctl_table(t)    unregister_sysctl_table(t)

/*
 * seq device (linux/seq_file.h)
 */


/*
 * seq file definitions
 */

struct dentry;
struct vfsmount;

struct path {
        struct vfsmount *mnt;
        struct dentry *dentry;
};

struct seq_operations;
struct file;
struct inode;

struct seq_file {
	char *buf;
	size_t size;
	size_t from;
	size_t count;
	loff_t index;
	u32    version;
	cfs_mutex_t lock;
	const struct seq_operations *op;
	void *private;
};

struct seq_operations {
	void * (*start) (struct seq_file *m, loff_t *pos);
	void (*stop) (struct seq_file *m, void *v);
	void * (*next) (struct seq_file *m, void *v, loff_t *pos);
	int (*show) (struct seq_file *m, void *v);
};

int seq_open(struct file *, const struct seq_operations *);
ssize_t seq_read(struct file *, char __user *, size_t, loff_t *);
loff_t seq_lseek(struct file *, loff_t, int);
int seq_release(struct inode *, struct file *);
int seq_escape(struct seq_file *, const char *, const char *);
int seq_putc(struct seq_file *m, char c);
int seq_puts(struct seq_file *m, const char *s);

int seq_printf(struct seq_file *, const char *, ...)
	__attribute__ ((format (printf,2,3)));

int seq_path(struct seq_file *, struct path *, char *);

int single_open(struct file *, int (*)(struct seq_file *, void *), void *);
int single_release(struct inode *, struct file *);
void *__seq_open_private(struct file *, const struct seq_operations *, int);
int seq_open_private(struct file *, const struct seq_operations *, int);
int seq_release_private(struct inode *, struct file *);

#define SEQ_START_TOKEN ((void *)1)

/*
 * Helpers for iteration over list_head-s in seq_files
 */

extern cfs_list_t *seq_list_start(cfs_list_t *head, loff_t pos);
extern cfs_list_t *seq_list_start_head(cfs_list_t *head, loff_t pos);
extern cfs_list_t *seq_list_next(void *v, cfs_list_t *head, loff_t *ppos);

/*
 *  declaration of proc kernel process routines
 */

cfs_file_t *
lustre_open_file(char * filename);

int
lustre_close_file(cfs_file_t * fh);

int
lustre_do_ioctl( cfs_file_t * fh,
                 unsigned long cmd,
                 ulong_ptr_t arg );

int
lustre_ioctl_file( cfs_file_t * fh,
                   PCFS_PROC_IOCTL devctl);

size_t
lustre_read_file( cfs_file_t *    fh,
                  loff_t          offl,
                  size_t          size,
                  char *          buf
                  );

size_t
lustre_write_file( cfs_file_t *    fh,
                   loff_t          off,
                   size_t          size,
                   char *          buf
                   );

/*
 * Wait Queue
 */


typedef int cfs_task_state_t;

#define CFS_TASK_INTERRUPTIBLE	 0x00000001
#define CFS_TASK_UNINT	         0x00000002
#define CFS_TASK_RUNNING         0x00000003
#define CFS_TASK_UNINTERRUPTIBLE CFS_TASK_UNINT

#define CFS_WAITQ_MAGIC     'CWQM'
#define CFS_WAITLINK_MAGIC  'CWLM'

typedef struct cfs_waitq {

    unsigned int            magic;
    unsigned int            flags;

    cfs_spinlock_t          guard;
    cfs_list_t              waiters;

} cfs_waitq_t;


typedef struct cfs_waitlink cfs_waitlink_t;

#define CFS_WAITQ_CHANNELS     (2)

#define CFS_WAITQ_CHAN_NORMAL  (0)
#define CFS_WAITQ_CHAN_FORWARD (1)



typedef struct cfs_waitlink_channel {
    cfs_list_t              link;
    cfs_waitq_t *           waitq;
    cfs_waitlink_t *        waitl;
} cfs_waitlink_channel_t;

struct cfs_waitlink {

    unsigned int            magic;
    int                     flags;
    event_t  *              event;
    cfs_atomic_t *          hits;

    cfs_waitlink_channel_t  waitq[CFS_WAITQ_CHANNELS];
};

enum {
	CFS_WAITQ_EXCLUSIVE = 1
};

#define CFS_DECL_WAITQ(name) cfs_waitq_t name

/* Kernel thread */

typedef int (*cfs_thread_t) (void *arg);

typedef struct _cfs_thread_context {
    cfs_thread_t        func;
    void *              arg;
} cfs_thread_context_t;

int cfs_create_thread(int (*func)(void *), void *arg, unsigned long flag);

/*
 * thread creation flags from Linux, not used in winnt
 */
#define CSIGNAL         0x000000ff      /* signal mask to be sent at exit */
#define CLONE_VM        0x00000100      /* set if VM shared between processes */
#define CLONE_FS        0x00000200      /* set if fs info shared between processes */
#define CLONE_FILES     0x00000400      /* set if open files shared between processes */
#define CLONE_SIGHAND   0x00000800      /* set if signal handlers and blocked signals shared */
#define CLONE_PID       0x00001000      /* set if pid shared */
#define CLONE_PTRACE    0x00002000      /* set if we want to let tracing continue on the child too */
#define CLONE_VFORK     0x00004000      /* set if the parent wants the child to wake it up on mm_release */
#define CLONE_PARENT    0x00008000      /* set if we want to have the same parent as the cloner */
#define CLONE_THREAD    0x00010000      /* Same thread group? */
#define CLONE_NEWNS     0x00020000      /* New namespace group? */

#define CLONE_SIGNAL    (CLONE_SIGHAND | CLONE_THREAD)

#define CFS_DAEMON_FLAGS (CLONE_VM|CLONE_FILES)

/*
 * group_info: linux/sched.h
 */
#define NGROUPS_SMALL           32
#define NGROUPS_PER_BLOCK       ((int)(PAGE_SIZE / sizeof(gid_t)))
typedef struct cfs_group_info {
        int ngroups;
        cfs_atomic_t usage;
        gid_t small_block[NGROUPS_SMALL];
        int nblocks;
        gid_t *blocks[0];
} cfs_group_info_t;

#define cfs_get_group_info(group_info) do { \
        cfs_atomic_inc(&(group_info)->usage); \
} while (0)

#define cfs_put_group_info(group_info) do { \
        if (cfs_atomic_dec_and_test(&(group_info)->usage)) \
                cfs_groups_free(group_info); \
} while (0)

static __inline cfs_group_info_t *cfs_groups_alloc(int gidsetsize)
{
    cfs_group_info_t * groupinfo;
    KdPrint(("%s(%d): %s NOT implemented.\n", __FILE__, __LINE__, __FUNCTION__));
    groupinfo =
        (cfs_group_info_t *)cfs_alloc(sizeof(cfs_group_info_t), 0);

    if (groupinfo) {
        memset(groupinfo, 0, sizeof(cfs_group_info_t));
    }
    return groupinfo;
}
static __inline void cfs_groups_free(cfs_group_info_t *group_info)
{
    KdPrint(("%s(%d): %s NOT implemented.\n", __FILE__, __LINE__,
            __FUNCTION__));
    cfs_free(group_info);
}
static __inline int
cfs_set_current_groups(cfs_group_info_t *group_info)
{
    KdPrint(("%s(%d): %s NOT implemented.\n", __FILE__, __LINE__,
             __FUNCTION__));
    return 0;
}
static __inline int groups_search(cfs_group_info_t *group_info,
                                  gid_t grp) {
    KdPrint(("%s(%d): %s NOT implemented.\n", __FILE__, __LINE__,
            __FUNCTION__));
    return 0;
}

/*
 *   capability issue (linux/capability.h)
 */

/* Override resource limits. Set resource limits. */
/* Override quota limits. */
/* Override reserved space on ext2 filesystem */
/* Modify data journaling mode on ext3 filesystem (uses journaling
   resources) */
/* NOTE: ext2 honors fsuid when checking for resource overrides, so
   you can override using fsuid too */
/* Override size restrictions on IPC message queues */
/* Allow more than 64hz interrupts from the real-time clock */
/* Override max number of consoles on console allocation */
/* Override max number of keymaps */

#define CAP_SYS_RESOURCE     24

/*
 *  capabilities support 
 */

typedef __u32 cfs_kernel_cap_t;

#define cap_raise(c, flag)  do {} while(0)
#define cap_lower(c, flag)  do {} while(0)
#define cap_raised(c, flag) do {} while(0)


/*
 * Task struct
 */

#define CFS_MAX_SCHEDULE_TIMEOUT     ((long_ptr_t)(~0UL>>12))
#define cfs_schedule_timeout(t)      cfs_schedule_timeout_and_set_state(0, t)

struct vfsmount;

#define NGROUPS 1
#define CFS_CURPROC_COMM_MAX (16)
typedef struct task_sruct{
    mode_t                umask;
    sigset_t              blocked;

    pid_t                 pid;
    pid_t                 pgrp;

    uid_t                 uid,euid,suid,fsuid;
    gid_t                 gid,egid,sgid,fsgid;

    int                   ngroups;
    int                   cgroups;
    gid_t                 groups[NGROUPS];
    cfs_group_info_t     *group_info;
    cfs_kernel_cap_t      cap_effective,
                          cap_inheritable,
                          cap_permitted;

    char                  comm[CFS_CURPROC_COMM_MAX];
    void                 *journal_info;
    struct vfsmount      *fs;
}  cfs_task_t;

static inline void task_lock(cfs_task_t *t)
{
}

static inline void task_unlock(cfs_task_t *t)
{
}

/*
 *  linux task struct emulator ...
 */

#define TASKMAN_MAGIC  'TMAN'   /* Task Manager */
#define TASKSLT_MAGIC  'TSLT'   /* Task Slot */

typedef struct _TASK_MAN {

    ULONG           Magic;      /* Magic and Flags */
    ULONG           Flags;

    cfs_spinlock_t  Lock;       /* Protection lock */

    cfs_mem_cache_t *slab; /* Memory slab for task slot */

    ULONG           NumOfTasks; /* Total tasks (threads) */
    LIST_ENTRY      TaskList;   /* List of task slots */

} TASK_MAN, *PTASK_MAN;

typedef struct _TASK_SLOT {

    ULONG           Magic;      /* Magic and Flags */
    ULONG           Flags;

    LIST_ENTRY      Link;       /* To be linked to TaskMan */

    event_t         Event;      /* Schedule event */

    HANDLE          Pid;        /* Process id */
    HANDLE          Tid;        /* Thread id */
    PETHREAD        Tet;        /* Pointer to ethread */

    cfs_atomic_t    count;      /* refer count */
    cfs_atomic_t    hits;       /* times of waken event singaled */

    KIRQL           irql;       /* irql for rwlock ... */

    cfs_task_t      task;       /* linux task part */

} TASK_SLOT, *PTASK_SLOT;


#define current                      cfs_current()
#define cfs_set_current_state(s)     do {;} while (0)
#define cfs_set_current_state(state) cfs_set_current_state(state)

#define cfs_wait_event(wq, condition)                           \
do {                                                            \
        cfs_waitlink_t __wait;                                  \
                                                                \
        cfs_waitlink_init(&__wait);                             \
        while (TRUE) {                                          \
            cfs_waitq_add(&wq, &__wait);                        \
            if (condition) {                                    \
                break;                                          \
            }                                                   \
            cfs_waitq_wait(&__wait, CFS_TASK_INTERRUPTIBLE);    \
            cfs_waitq_del(&wq, &__wait);	                \
        }					                \
        cfs_waitq_del(&wq, &__wait);		                \
} while(0)

#define cfs_wait_event_interruptible(wq, condition, __ret)      \
do {                                                            \
        cfs_waitlink_t __wait;	                                \
                                                                \
        __ret = 0;                                              \
        cfs_waitlink_init(&__wait);                             \
        while (TRUE) {                                          \
            cfs_waitq_add(&wq, &__wait);	                \
            if (condition) {                                    \
                break;                                          \
            }                                                   \
            cfs_waitq_wait(&__wait, CFS_TASK_INTERRUPTIBLE);    \
            cfs_waitq_del(&wq, &__wait);	                \
        }                                                       \
        cfs_waitq_del(&wq, &__wait);                            \
} while(0)

# define cfs_wait_event_interruptible_exclusive(wq, condition, rc)  \
         cfs_wait_event_interruptible(wq, condition, rc)

/*
   retval == 0; condition met; we're good.
   retval < 0; interrupted by signal.
   retval > 0; timed out.
*/

#define cfs_waitq_wait_event_interruptible_timeout(             \
                        wq, condition, timeout, rc)             \
do {                                                            \
        cfs_waitlink_t __wait;                                  \
                                                                \
        rc = 0;                                                 \
        cfs_waitlink_init(&__wait);	                        \
        while (TRUE) {                                          \
            cfs_waitq_add(&wq, &__wait);                        \
            if (condition) {                                    \
                break;                                          \
            }                                                   \
            if (cfs_waitq_timedwait(&__wait,                    \
                CFS_TASK_INTERRUPTIBLE, timeout) == 0) {        \
                rc = TRUE;                                      \
                break;                                          \
            }                                                   \
            cfs_waitq_del(&wq, &__wait);	                \
        }					                \
        cfs_waitq_del(&wq, &__wait);		                \
} while(0)


#define cfs_waitq_wait_event_timeout                            \
        cfs_waitq_wait_event_interruptible_timeout

int     init_task_manager();
void    cleanup_task_manager();
cfs_task_t * cfs_current();
int     wake_up_process(cfs_task_t * task);
void sleep_on(cfs_waitq_t *waitq);
#define cfs_might_sleep() do {} while(0)
#define CFS_DECL_JOURNAL_DATA	
#define CFS_PUSH_JOURNAL	    do {;} while(0)
#define CFS_POP_JOURNAL		    do {;} while(0)


/* module related definitions */

#ifndef __exit
#define __exit
#endif
#ifndef __init
#define __init
#endif

typedef struct cfs_module {
    const char *name;
} cfs_module_t;

extern cfs_module_t libcfs_global_module;
#define THIS_MODULE  &libcfs_global_module

#define cfs_request_module(x, y) (0)
#define EXPORT_SYMBOL(s)
#define MODULE_AUTHOR(s)
#define MODULE_DESCRIPTION(s)
#define MODULE_LICENSE(s)
#define MODULE_PARM(a, b)
#define MODULE_PARM_DESC(a, b)

#define module_init(X) int  __init module_##X() {return X();}
#define module_exit(X) void __exit module_##X() {X();}

#define DECLARE_INIT(X) extern int  __init  module_##X(void)
#define DECLARE_EXIT(X) extern void __exit  module_##X(void)

#define MODULE_INIT(X) do { int rc = module_##X(); \
                            if (rc) goto errorout; \
                          } while(0)

#define MODULE_EXIT(X) do { module_##X(); } while(0)


/* Module interfaces */
#define cfs_module(name, version, init, fini) \
        module_init(init);                    \
        module_exit(fini)
#define cfs_module_refcount(x) (1)

/*
 * typecheck
 */

#define typecheck(a, b) do {} while(0)

/*
 * linux/crypto.h
 */

#define CRYPTO_MAX_ALG_NAME		64

#define CRYPTO_TFM_MODE_ECB		0x00000001
#define CRYPTO_TFM_MODE_CBC		0x00000002
#define CRYPTO_TFM_MODE_CFB		0x00000004
#define CRYPTO_TFM_MODE_CTR		0x00000008
#define CRYPTO_TFM_MODE_EME		0x00000010

/*
 * hash
 */
/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32 0x9e370001UL

#if 0 /* defined in libcfs/libcfs_hash.h */
static inline u32 cfs_hash_long(u32 val, unsigned int bits)
{
	/* On some cpus multiply is faster, on others gcc will do shifts */
	u32 hash = val * GOLDEN_RATIO_PRIME_32;

	/* High bits are more random, so use them. */
	return hash >> (32 - bits);
}
#endif

/*
 * Timer
 */

#define CFS_TIMER_FLAG_INITED   0x00000001  // Initialized already
#define CFS_TIMER_FLAG_TIMERED  0x00000002  // KeSetTimer is called

typedef struct cfs_timer {

    KSPIN_LOCK      Lock;

    ULONG           Flags;

    KDPC            Dpc;
    KTIMER          Timer;

    cfs_time_t      deadline;

    void (*proc)(ulong_ptr_t);
    void *          arg;

} cfs_timer_t;

/*
 *  libcfs globals initialization/cleanup
 */

int
libcfs_arch_init(void);

void
libcfs_arch_cleanup(void);

/*
 *  cache alignment size
 */

#define CFS_L1_CACHE_ALIGN(x) (x)

#define __cacheline_aligned

/*
 * SMP ...
 */


#define SMP_CACHE_BYTES             128
#define CFS_NR_CPUS                 (32)
#define smp_num_cpus                ((CCHAR)KeNumberProcessors)
#define cfs_num_possible_cpus()     smp_num_cpus
#define cfs_num_present_cpus()      smp_num_cpus
#define cfs_num_online_cpus()       smp_num_cpus
#define cfs_smp_processor_id()	    ((USHORT)KeGetCurrentProcessorNumber())
#define smp_call_function(f, a, n, w)		do {} while(0)
#define smp_rmb()                   do {} while(0)

/*
 *  Irp related
 */

#define CFS_NR_IRQS                 512
#define cfs_in_interrupt()          (0)

/*
 *  printk flags
 */

#define CFS_KERN_EMERG      "<0>"   /* system is unusable                   */
#define CFS_KERN_ALERT      "<1>"   /* action must be taken immediately     */
#define CFS_KERN_CRIT       "<2>"   /* critical conditions                  */
#define CFS_KERN_ERR        "<3>"   /* error conditions                     */
#define CFS_KERN_WARNING    "<4>"   /* warning conditions                   */
#define CFS_KERN_NOTICE     "<5>"   /* normal but significant condition     */
#define CFS_KERN_INFO       "<6>"   /* informational                        */
#define CFS_KERN_DEBUG      "<7>"   /* debug-level messages                 */

/*
 * Misc
 */

#define inter_module_get(n)			cfs_symbol_get(n)
#define inter_module_put(n)			cfs_symbol_put(n)

#ifndef likely
#define likely(exp) (exp)
#endif
#ifndef unlikely
#define unlikely(exp) (exp)
#endif

#define cfs_lock_kernel()               do {} while(0)
#define cfs_unlock_kernel()             do {} while(0)

#define local_irq_save(x)
#define local_irq_restore(x)

#define THREAD_NAME

#define va_copy(_d, _s)                 (_d = _s)

char *strnchr(const char *s, size_t count, int c);

#define adler32(a,b,l) zlib_adler32(a,b,l)
ULONG zlib_adler32(ULONG adler, const BYTE *buf, UINT len);

typedef ssize_t (*read_actor_t)();

#if DBG
/*
 *  winnt debug routines
 */

VOID
KsPrintf(
    LONG  DebugPrintLevel,
    PCHAR DebugMessage,
    ...
    );

PUCHAR
KsNtStatusToString (IN NTSTATUS Status);
#endif

#else   /* !__KERNEL__ */

void cfs_enter_debugger();

/*
 *  PAGE_SIZE ...
 */

#ifndef PAGE_SIZE
#define PAGE_SIZE       (4096)
#endif

#define getpagesize()   (4096)

#define PAGE_CACHE_SIZE PAGE_SIZE
#define PAGE_CACHE_MASK PAGE_MASK

#define PTHREAD_MUTEX_INITIALIZER ((pthread_mutex_t) -1)
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER ((pthread_mutex_t) -2)
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER ((pthread_mutex_t) -3)

typedef struct file {
    int foo;
} cfs_file_t;

#include "../user-prim.h"
#include "../user-lock.h"
#include <sys/stat.h>
#include <sys/types.h>

#define strcasecmp  strcmp
#define strncasecmp strncmp
#define getpid()   (0)

#define getuid()    (0)
#define getgrgid(x) (NULL)

struct passwd {
        uid_t pw_uid;
        char  pw_name[64];
};
struct passwd * getpwuid(uid_t uid);

int cfs_proc_mknod(const char *path, mode_t mode, dev_t dev);

int gethostname(char * name, int namelen);

#define setlinebuf(x) do {} while(0)


/* Maximum EA Information Length */
#define EA_MAX_LENGTH  (sizeof(FILE_FULL_EA_INFORMATION) + 15)

/*
 *  proc user mode routines
 */

int cfs_proc_open (char * filename, int oflag);
int cfs_proc_close(int fd);
int cfs_proc_read(int fd, void *buffer, unsigned int count);
int cfs_proc_write(int fd, void *buffer, unsigned int count);
int cfs_proc_ioctl(int fd, int cmd, void *buffer);
FILE *cfs_proc_fopen(char *path, char * mode);
char *cfs_proc_fgets(char * buf, int len, FILE *fp);
int cfs_proc_fclose(FILE *fp);

/* Bits set in the FLAGS argument to `glob'.  */
#define	GLOB_ERR	(1 << 0)/* Return on read errors.  */
#define	GLOB_MARK	(1 << 1)/* Append a slash to each name.  */
#define	GLOB_NOSORT	(1 << 2)/* Don't sort the names.  */
#define	GLOB_DOOFFS	(1 << 3)/* Insert PGLOB->gl_offs NULLs.  */
#define	GLOB_NOCHECK	(1 << 4)/* If nothing matches, return the pattern.  */
#define	GLOB_APPEND	(1 << 5)/* Append to results of a previous call.  */
#define	GLOB_NOESCAPE	(1 << 6)/* Backslashes don't quote metacharacters.  */
#define	GLOB_PERIOD	(1 << 7)/* Leading `.' can be matched by metachars.  */

#if !defined __USE_POSIX2 || defined __USE_BSD || defined __USE_GNU
# define GLOB_MAGCHAR	 (1 << 8)/* Set in gl_flags if any metachars seen.  */
# define GLOB_ALTDIRFUNC (1 << 9)/* Use gl_opendir et al functions.  */
# define GLOB_BRACE	 (1 << 10)/* Expand "{a,b}" to "a" "b".  */
# define GLOB_NOMAGIC	 (1 << 11)/* If no magic chars, return the pattern.  */
# define GLOB_TILDE	 (1 << 12)/* Expand ~user and ~ to home directories. */
# define GLOB_ONLYDIR	 (1 << 13)/* Match only directories.  */
# define GLOB_TILDE_CHECK (1 << 14)/* Like GLOB_TILDE but return an error
				      if the user name is not available.  */
# define __GLOB_FLAGS	(GLOB_ERR|GLOB_MARK|GLOB_NOSORT|GLOB_DOOFFS| \
			 GLOB_NOESCAPE|GLOB_NOCHECK|GLOB_APPEND|     \
			 GLOB_PERIOD|GLOB_ALTDIRFUNC|GLOB_BRACE|     \
			 GLOB_NOMAGIC|GLOB_TILDE|GLOB_ONLYDIR|GLOB_TILDE_CHECK)
#else
# define __GLOB_FLAGS	(GLOB_ERR|GLOB_MARK|GLOB_NOSORT|GLOB_DOOFFS| \
			 GLOB_NOESCAPE|GLOB_NOCHECK|GLOB_APPEND|     \
			 GLOB_PERIOD)
#endif

/* Error returns from `glob'.  */
#define	GLOB_NOSPACE	1	/* Ran out of memory.  */
#define	GLOB_ABORTED	2	/* Read error.  */
#define	GLOB_NOMATCH	3	/* No matches found.  */
#define GLOB_NOSYS	4	/* Not implemented.  */
#ifdef __USE_GNU
/* Previous versions of this file defined GLOB_ABEND instead of
   GLOB_ABORTED.  Provide a compatibility definition here.  */
# define GLOB_ABEND GLOB_ABORTED
#endif

/* Structure describing a globbing run.  */
#ifdef __USE_GNU
struct stat;
#endif
typedef struct
  {
    size_t gl_pathc;		/* Count of paths matched by the pattern.  */
    char **gl_pathv;		/* List of matched pathnames.  */
    size_t gl_offs;		/* Slots to reserve in `gl_pathv'.  */
    int gl_flags;		/* Set to FLAGS, maybe | GLOB_MAGCHAR.  */

    /* If the GLOB_ALTDIRFUNC flag is set, the following functions
       are used instead of the normal file access functions.  */
    void (*gl_closedir) (void *);
#ifdef __USE_GNU
    struct dirent *(*gl_readdir) (void *);
#else
    void *(*gl_readdir) (void *);
#endif
    void *(*gl_opendir) (const char *);
#ifdef __USE_GNU
    int (*gl_lstat) (const char *__restrict, struct stat *__restrict);
    int (*gl_stat) (const char *__restrict, struct stat *__restrict);
#else
    int (*gl_lstat) (const char *__restrict, void *__restrict);
    int (*gl_stat) (const char *__restrict, void *__restrict);
#endif
  } glob_t;

#ifdef __USE_LARGEFILE64
# ifdef __USE_GNU
struct stat64;
# endif
typedef struct
  {
    __size_t gl_pathc;
    char **gl_pathv;
    __size_t gl_offs;
    int gl_flags;

    /* If the GLOB_ALTDIRFUNC flag is set, the following functions
       are used instead of the normal file access functions.  */
    void (*gl_closedir) (void *);
# ifdef __USE_GNU
    struct dirent64 *(*gl_readdir) (void *);
# else
    void *(*gl_readdir) (void *);
# endif
    void *(*gl_opendir) (__const char *);
# ifdef __USE_GNU
    int (*gl_lstat) (__const char *__restrict, struct stat64 *__restrict);
    int (*gl_stat) (__const char *__restrict, struct stat64 *__restrict);
# else
    int (*gl_lstat) (__const char *__restrict, void *__restrict);
    int (*gl_stat) (__const char *__restrict, void *__restrict);
# endif
  } glob64_t;
#endif

int glob (const char * __pattern, int __flags,
		 int (*__errfunc) (const char *, int),
		 glob_t * __pglob);
void globfree(glob_t *__pglog);

#endif /* !__KERNEL__ */

/*
 *  module routines
 */

static inline void __cfs_module_get(cfs_module_t *module)
{
}

static inline int cfs_try_module_get(cfs_module_t *module)
{
    return 1;
}

static inline void cfs_module_put(cfs_module_t *module)
{
}

/*
 *  sigset_t routines 
 */

typedef sigset_t cfs_sigset_t;
#define sigaddset(what,sig) (*(what) |= (1<<(sig)), 0)
#define sigdelset(what,sig) (*(what) &= ~(1<<(sig)), 0)
#define sigemptyset(what)   (*(what) = 0, 0)
#define sigfillset(what)    (*(what) = ~(0), 0)
#define sigismember(what,sig) (((*(what)) & (1<<(sig))) != 0)

static __inline int
sigprocmask(int sig, cfs_sigset_t *w1, cfs_sigset_t *w2) {
    return 0;
}
static __inline int
sigpending(cfs_sigset_t *what) {
    return 0;
}

/*
 * common inode flags (user & kernel)
 */

#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)	(((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)	(((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)	(((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

#define S_IRWXUGO   (S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO   (S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO     (S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO     (S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO     (S_IXUSR|S_IXGRP|S_IXOTH)


/*
 *  Linux kernel version definition
 */

#define KERNEL_VERSION(a,b,c) ((a)*100+(b)*10+c)

/*
 *  linux ioctl coding definitions
 */
 
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRMASK     ((1 << _IOC_NRBITS)-1)
#define _IOC_TYPEMASK   ((1 << _IOC_TYPEBITS)-1)
#define _IOC_SIZEMASK   ((1 << _IOC_SIZEBITS)-1)
#define _IOC_DIRMASK    ((1 << _IOC_DIRBITS)-1)

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT+_IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT+_IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT+_IOC_SIZEBITS)

/*
 * Direction bits.
 */
#define _IOC_NONE   0U
#define _IOC_WRITE  1U
#define _IOC_READ   2U

#define _IOC(dir,type,nr,size) \
    (((dir)  << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) | \
     ((size) << _IOC_SIZESHIFT))

/* used to create numbers */
#define _IO(type,nr)      _IOC(_IOC_NONE,(type),(nr),0)
#define _IOR(type,nr,size)    _IOC(_IOC_READ,(type),(nr),sizeof(size))
#define _IOW(type,nr,size)    _IOC(_IOC_WRITE,(type),(nr),sizeof(size))
#define _IOWR(type,nr,size) _IOC(_IOC_READ|_IOC_WRITE,(type),(nr),sizeof(size))

/* used to decode ioctl numbers.. */
#define _IOC_DIR(nr)        (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr)       (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)         (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr)       (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

/* i/o vector sgructure ... */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

/* idr support routines */
struct idr_context *cfs_idr_init();
int cfs_idr_remove(struct idr_context *idp, int id);
int cfs_idr_get_new(struct idr_context *idp, void *ptr);
int cfs_idr_get_new_above(struct idr_context *idp, void *ptr, int starting_id);
void *cfs_idr_find(struct idr_context *idp, int id);
void cfs_idr_exit(struct idr_context *idp);

/* runtime time routines for both kenrel and user mode */
extern int cfs_isalpha(int);
extern int cfs_isspace(int);
extern int cfs_isupper(int);
extern int cfs_isdigit(int);
extern int cfs_isxdigit(int);

#define ULONG_LONG_MAX ((__u64)(0xFFFFFFFFFFFFFFFF))
/*
 * Convert a string to an unsigned long long integer.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 */
__u64 strtoull(char *nptr, char **endptr,int base);

/*
 *  getopt routines
 */

/* For communication from `getopt' to the caller.
   When `getopt' finds an option that takes an argument,
   the argument value is returned here.
   Also, when `ordering' is RETURN_IN_ORDER,
   each non-option ARGV-element is returned here.  */

extern char *optarg;

/* Index in ARGV of the next element to be scanned.
   This is used for communication to and from the caller
   and for communication between successive calls to `getopt'.

   On entry to `getopt', zero means this is the first call; initialize.

   When `getopt' returns -1, this is the index of the first of the
   non-option elements that the caller should itself scan.

   Otherwise, `optind' communicates from one call to the next
   how much of ARGV has been scanned so far.  */

extern int optind;

/* Callers store zero here to inhibit the error message `getopt' prints
   for unrecognized options.  */

extern int opterr;

/* Set to an option character which was unrecognized.  */

extern int optopt;


struct option
{
  const char *name;
  /* has_arg can't be an enum because some compilers complain about
     type mismatches in all the code that assumes it is an int.  */
  int has_arg;
  int *flag;
  int val;
};

/* Names for the values of the `has_arg' field of `struct option'.  */
# define no_argument		0
# define required_argument	1
# define optional_argument	2

extern int getopt(int ___argc, char *const *___argv, const char *__shortopts);
extern int getopt_long (int ___argc, char *const *___argv,
			const char *__shortopts,
		        const struct option *__longopts, int *__longind);
extern int getopt_long_only (int ___argc, char *const *___argv,
			     const char *__shortopts,
		             const struct option *__longopts, int *__longind);

extern char *strcasestr (const char *phaystack, const char *pneedle);

/*
 * global environment runtime routine
 */

static __inline char * __cdecl cfs_getenv(const char *ENV) {return NULL;}
static __inline void   __cdecl set_getenv(const char *ENV, const char *value, int overwrite) {}

int setenv(const char *envname, const char *envval, int overwrite);

typedef struct utsname {
         char sysname[64];
         char nodename[64];
         char release[128];
         char version[128];
         char machine[64];
};

int uname(struct utsname *uts);

#endif
