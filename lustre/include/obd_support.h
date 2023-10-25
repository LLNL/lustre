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
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#ifndef _OBD_SUPPORT
#define _OBD_SUPPORT

#include <linux/atomic.h>
#include <linux/ctype.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <libcfs/libcfs.h>
#include <lprocfs_status.h>
#include <lustre_handles.h>

/* global variables */
extern struct lprocfs_stats *obd_memory;
enum {
        OBD_MEMORY_STAT = 0,
        OBD_STATS_NUM,
};

extern unsigned int obd_debug_peer_on_timeout;
extern unsigned int obd_dump_on_timeout;
extern unsigned int obd_dump_on_eviction;
extern unsigned int obd_lbug_on_eviction;
/* obd_timeout should only be used for recovery, not for
   networking / disk / timings affected by load (use Adaptive Timeouts) */
extern unsigned int obd_timeout;          /* seconds */
extern unsigned int ldlm_timeout;         /* seconds */
extern unsigned int obd_timeout_set;
extern unsigned int ldlm_timeout_set;
extern unsigned int bulk_timeout;
extern unsigned int at_min;
extern unsigned int at_max;
extern unsigned int at_history;
extern int at_early_margin;
extern int at_extra;
extern unsigned long obd_max_dirty_pages;
extern atomic_long_t obd_dirty_pages;
extern char obd_jobid_var[];

/* Some hash init argument constants */
#define HASH_NID_STATS_BKT_BITS 5
#define HASH_NID_STATS_CUR_BITS 7
#define HASH_NID_STATS_MAX_BITS 12
#define HASH_GEN_BKT_BITS 5
#define HASH_GEN_CUR_BITS 7
#define HASH_GEN_MAX_BITS 12
#define HASH_LQE_BKT_BITS 5
#define HASH_LQE_CUR_BITS 7
#define HASH_LQE_MAX_BITS 12
#define HASH_EXP_LOCK_BKT_BITS  5
#define HASH_EXP_LOCK_CUR_BITS  7
#define HASH_EXP_LOCK_MAX_BITS  16
#define HASH_JOB_STATS_BKT_BITS 5
#define HASH_JOB_STATS_CUR_BITS 7
#define HASH_JOB_STATS_MAX_BITS 12

/* Timeout definitions */
#define OBD_TIMEOUT_DEFAULT             100
#define LDLM_TIMEOUT_DEFAULT            20
#define MDS_LDLM_TIMEOUT_DEFAULT        6
/* Time to wait for all clients to reconnect during recovery (hard limit) */
#define OBD_RECOVERY_TIME_HARD          (obd_timeout * 9)
/* Time to wait for all clients to reconnect during recovery (soft limit) */
/* Should be very conservative; must catch the first reconnect after reboot */
#define OBD_RECOVERY_TIME_SOFT          (obd_timeout * 3)
/* Change recovery-small 26b time if you change this */
#define PING_INTERVAL max(obd_timeout / 4, 1U)
/* a bit more than maximal journal commit time in seconds */
#define PING_INTERVAL_SHORT min(PING_INTERVAL, 7U)
/* Client may skip 1 ping; we must wait at least 2.5. But for multiple
 * failover targets the client only pings one server at a time, and pings
 * can be lost on a loaded network. Since eviction has serious consequences,
 * and there's no urgent need to evict a client just because it's idle, we
 * should be very conservative here. */
#define PING_EVICT_TIMEOUT (PING_INTERVAL * 6)
#define DISK_TIMEOUT 50          /* Beyond this we warn about disk speed */
#define CONNECTION_SWITCH_MIN 5U /* Connection switching rate limiter */
 /* Max connect interval for nonresponsive servers; ~50s to avoid building up
    connect requests in the LND queues, but within obd_timeout so we don't
    miss the recovery window */
#define CONNECTION_SWITCH_MAX min(50U, max(CONNECTION_SWITCH_MIN,obd_timeout))
#define CONNECTION_SWITCH_INC 5  /* Connection timeout backoff */
/* In general this should be low to have quick detection of a system
   running on a backup server. (If it's too low, import_select_connection
   will increase the timeout anyhow.)  */
#define INITIAL_CONNECT_TIMEOUT max(CONNECTION_SWITCH_MIN,obd_timeout/20)
/* The max delay between connects is SWITCH_MAX + SWITCH_INC + INITIAL */
#define RECONNECT_DELAY_MAX (CONNECTION_SWITCH_MAX + CONNECTION_SWITCH_INC + \
                             INITIAL_CONNECT_TIMEOUT)
/* The min time a target should wait for clients to reconnect in recovery */
#define OBD_RECOVERY_TIME_MIN	(2*RECONNECT_DELAY_MAX)
#define OBD_IR_FACTOR_MIN	1
#define OBD_IR_FACTOR_MAX	10
#define OBD_IR_FACTOR_DEFAULT	(OBD_IR_FACTOR_MAX/2)
/* default timeout for the MGS to become IR_FULL */
#define OBD_IR_MGS_TIMEOUT	(4*obd_timeout)
/* Unlink should happen within this many seconds. */
#define PTLRPC_REQ_LONG_UNLINK	300

/**
 * Time interval of shrink, if the client is "idle" more than this interval,
 * then the ll_grant thread will return the requested grant space to filter
 */
#define GRANT_SHRINK_INTERVAL            1200/*20 minutes*/

#define OBD_FAIL_MDS                     0x100
#define OBD_FAIL_MDS_HANDLE_UNPACK       0x101
#define OBD_FAIL_MDS_GETATTR_NET         0x102
#define OBD_FAIL_MDS_GETATTR_PACK        0x103
#define OBD_FAIL_MDS_READPAGE_NET        0x104
#define OBD_FAIL_MDS_READPAGE_PACK       0x105
#define OBD_FAIL_MDS_SENDPAGE            0x106
#define OBD_FAIL_MDS_REINT_NET           0x107
#define OBD_FAIL_MDS_REINT_UNPACK        0x108
#define OBD_FAIL_MDS_REINT_SETATTR       0x109
#define OBD_FAIL_MDS_REINT_SETATTR_WRITE 0x10a
#define OBD_FAIL_MDS_REINT_CREATE        0x10b
#define OBD_FAIL_MDS_REINT_CREATE_WRITE  0x10c
#define OBD_FAIL_MDS_REINT_UNLINK        0x10d
#define OBD_FAIL_MDS_REINT_UNLINK_WRITE  0x10e
#define OBD_FAIL_MDS_REINT_LINK          0x10f
#define OBD_FAIL_MDS_REINT_LINK_WRITE    0x110
#define OBD_FAIL_MDS_REINT_RENAME        0x111
#define OBD_FAIL_MDS_REINT_RENAME_WRITE  0x112
#define OBD_FAIL_MDS_OPEN_NET            0x113
#define OBD_FAIL_MDS_OPEN_PACK           0x114
#define OBD_FAIL_MDS_CLOSE_NET           0x115
#define OBD_FAIL_MDS_CLOSE_PACK          0x116
#define OBD_FAIL_MDS_CONNECT_NET         0x117
#define OBD_FAIL_MDS_CONNECT_PACK        0x118
#define OBD_FAIL_MDS_REINT_NET_REP       0x119
#define OBD_FAIL_MDS_DISCONNECT_NET      0x11a
#define OBD_FAIL_MDS_GET_ROOT_NET	 0x11b
#define OBD_FAIL_MDS_GET_ROOT_PACK	 0x11c
#define OBD_FAIL_MDS_STATFS_PACK         0x11d
#define OBD_FAIL_MDS_STATFS_SUM_PACK     0x11d
#define OBD_FAIL_MDS_STATFS_NET          0x11e
#define OBD_FAIL_MDS_STATFS_SUM_NET      0x11e
#define OBD_FAIL_MDS_GETATTR_NAME_NET    0x11f
#define OBD_FAIL_MDS_PIN_NET             0x120
#define OBD_FAIL_MDS_UNPIN_NET           0x121
#define OBD_FAIL_MDS_ALL_REPLY_NET       0x122
#define OBD_FAIL_MDS_ALL_REQUEST_NET     0x123
#define OBD_FAIL_MDS_SYNC_NET            0x124
#define OBD_FAIL_MDS_SYNC_PACK           0x125
/*	OBD_FAIL_MDS_DONE_WRITING_NET    0x126 obsolete since 2.8.0 */
/*	OBD_FAIL_MDS_DONE_WRITING_PACK   0x127 obsolete since 2.8.0 */
#define OBD_FAIL_MDS_ALLOC_OBDO          0x128
#define OBD_FAIL_MDS_PAUSE_OPEN          0x129
#define OBD_FAIL_MDS_STATFS_LCW_SLEEP    0x12a
#define OBD_FAIL_MDS_OPEN_CREATE         0x12b
#define OBD_FAIL_MDS_OST_SETATTR         0x12c
/*	OBD_FAIL_MDS_QUOTACHECK_NET      0x12d obsolete since 2.4 */
#define OBD_FAIL_MDS_QUOTACTL_NET        0x12e
#define OBD_FAIL_MDS_CLIENT_ADD          0x12f
#define OBD_FAIL_MDS_GETXATTR_NET        0x130
#define OBD_FAIL_MDS_GETXATTR_PACK       0x131
#define OBD_FAIL_MDS_SETXATTR_NET        0x132
#define OBD_FAIL_MDS_SETXATTR            0x133
#define OBD_FAIL_MDS_SETXATTR_WRITE      0x134
#define OBD_FAIL_MDS_FS_SETUP            0x135
#define OBD_FAIL_MDS_RESEND              0x136
#define OBD_FAIL_MDS_LLOG_CREATE_FAILED  0x137
#define OBD_FAIL_MDS_LOV_SYNC_RACE       0x138
#define OBD_FAIL_MDS_OSC_PRECREATE       0x139
#define OBD_FAIL_MDS_LLOG_SYNC_TIMEOUT   0x13a
#define OBD_FAIL_MDS_CLOSE_NET_REP       0x13b
#define OBD_FAIL_MDS_BLOCK_QUOTA_REQ     0x13c
#define OBD_FAIL_MDS_DROP_QUOTA_REQ      0x13d
#define OBD_FAIL_MDS_REMOVE_COMMON_EA    0x13e
#define OBD_FAIL_MDS_ALLOW_COMMON_EA_SETTING   0x13f
#define OBD_FAIL_MDS_FAIL_LOV_LOG_ADD    0x140
#define OBD_FAIL_MDS_LOV_PREP_CREATE     0x141
#define OBD_FAIL_MDS_REINT_DELAY         0x142
#define OBD_FAIL_MDS_READLINK_EPROTO     0x143
#define OBD_FAIL_MDS_OPEN_WAIT_CREATE    0x144
#define OBD_FAIL_MDS_PDO_LOCK            0x145
#define OBD_FAIL_MDS_PDO_LOCK2           0x146
#define OBD_FAIL_MDS_OSC_CREATE_FAIL     0x147
#define OBD_FAIL_MDS_NEGATIVE_POSITIVE	 0x148
#define OBD_FAIL_MDS_HSM_STATE_GET_NET		0x149
#define OBD_FAIL_MDS_HSM_STATE_SET_NET		0x14a
#define OBD_FAIL_MDS_HSM_PROGRESS_NET		0x14b
#define OBD_FAIL_MDS_HSM_REQUEST_NET		0x14c
#define OBD_FAIL_MDS_HSM_CT_REGISTER_NET	0x14d
#define OBD_FAIL_MDS_HSM_CT_UNREGISTER_NET	0x14e
#define OBD_FAIL_MDS_SWAP_LAYOUTS_NET		0x14f
#define OBD_FAIL_MDS_HSM_ACTION_NET		0x150
#define OBD_FAIL_MDS_CHANGELOG_INIT		0x151
#define OBD_FAIL_MDS_HSM_SWAP_LAYOUTS		0x152
#define OBD_FAIL_MDS_RENAME              0x153
#define OBD_FAIL_MDS_RENAME2             0x154
#define OBD_FAIL_MDS_RENAME3             0x155
#define OBD_FAIL_MDS_RENAME4             0x156
#define OBD_FAIL_MDS_LDLM_REPLY_NET	 0x157
#define OBD_FAIL_MDS_STALE_DIR_LAYOUT	 0x158
#define OBD_FAIL_MDS_REINT_MULTI_NET     0x159
#define OBD_FAIL_MDS_REINT_MULTI_NET_REP 0x15a
#define OBD_FAIL_MDS_LLOG_CREATE_FAILED2 0x15b
#define OBD_FAIL_MDS_FLD_LOOKUP			0x15c
#define OBD_FAIL_MDS_CHANGELOG_REORDER	0x15d
#define OBD_FAIL_MDS_LLOG_UMOUNT_RACE   0x15e
#define OBD_FAIL_MDS_CHANGELOG_RACE	 0x15f
#define OBD_FAIL_MDS_INTENT_DELAY		0x160
#define OBD_FAIL_MDS_XATTR_REP			0x161
#define OBD_FAIL_MDS_TRACK_OVERFLOW	 0x162
#define OBD_FAIL_MDS_LOV_CREATE_RACE	 0x163
#define OBD_FAIL_MDS_HSM_CDT_DELAY	 0x164
#define OBD_FAIL_MDS_ORPHAN_DELETE	 0x165
#define OBD_FAIL_MDS_RMFID_NET		 0x166
#define OBD_FAIL_MDS_CREATE_RACE	 0x167
#define OBD_FAIL_MDS_STATFS_SPOOF	 0x168
#define OBD_FAIL_MDS_REINT_OPEN		 0x169
#define OBD_FAIL_MDS_REINT_OPEN2	 0x16a
#define OBD_FAIL_MDS_COMMITRW_DELAY	 0x16b
#define OBD_FAIL_MDS_CHANGELOG_DEL	 0x16c
#define OBD_FAIL_MDS_CHANGELOG_IDX_PUMP	 0x16d
#define OBD_FAIL_MDS_DELAY_DELORPHAN	 0x16e

/* layout lock */
#define OBD_FAIL_MDS_NO_LL_GETATTR	 0x170
#define OBD_FAIL_MDS_NO_LL_OPEN		 0x171
#define OBD_FAIL_MDS_LL_BLOCK		 0x172
#define OBD_FAIL_MDS_LOD_CREATE_PAUSE	 0x173

/* CMD */
#define OBD_FAIL_MDS_IS_SUBDIR_NET       0x180
#define OBD_FAIL_MDS_IS_SUBDIR_PACK      0x181
#define OBD_FAIL_MDS_SET_INFO_NET        0x182
#define OBD_FAIL_MDS_WRITEPAGE_NET       0x183
#define OBD_FAIL_MDS_WRITEPAGE_PACK      0x184
#define OBD_FAIL_MDS_RECOVERY_ACCEPTS_GAPS 0x185
#define OBD_FAIL_MDS_GET_INFO_NET        0x186
#define OBD_FAIL_MDS_DQACQ_NET           0x187
#define OBD_FAIL_MDS_STRIPE_CREATE	 0x188
#define OBD_FAIL_MDS_STRIPE_FID		 0x189
#define OBD_FAIL_MDS_LINK_RENAME_RACE	 0x18a

/* OI scrub */
#define OBD_FAIL_OSD_SCRUB_DELAY			0x190
#define OBD_FAIL_OSD_SCRUB_CRASH			0x191
#define OBD_FAIL_OSD_SCRUB_FATAL			0x192
#define OBD_FAIL_OSD_FID_MAPPING			0x193
#define OBD_FAIL_OSD_LMA_INCOMPAT			0x194
#define OBD_FAIL_OSD_COMPAT_INVALID_ENTRY		0x195
#define OBD_FAIL_OSD_COMPAT_NO_ENTRY			0x196
#define OBD_FAIL_OSD_OST_EA_FID_SET			0x197
#define OBD_FAIL_OSD_NO_OI_ENTRY			0x198
#define OBD_FAIL_OSD_INDEX_CRASH			0x199
#define OBD_FAIL_OSD_TXN_START				0x19a
#define OBD_FAIL_OSD_DUPLICATE_MAP			0x19b
#define OBD_FAIL_OSD_REF_DEL				0x19c
#define OBD_FAIL_OSD_OI_ENOSPC				0x19d
#define OBD_FAIL_OSD_DOTDOT_ENOSPC			0x19e

#define OBD_FAIL_OFD_SET_OID				0x1e0

#define OBD_FAIL_OST                     0x200
#define OBD_FAIL_OST_CONNECT_NET         0x201
#define OBD_FAIL_OST_DISCONNECT_NET      0x202
#define OBD_FAIL_OST_GET_INFO_NET        0x203
#define OBD_FAIL_OST_CREATE_NET          0x204
#define OBD_FAIL_OST_DESTROY_NET         0x205
#define OBD_FAIL_OST_GETATTR_NET         0x206
#define OBD_FAIL_OST_SETATTR_NET         0x207
#define OBD_FAIL_OST_OPEN_NET            0x208
#define OBD_FAIL_OST_CLOSE_NET           0x209
#define OBD_FAIL_OST_BRW_NET             0x20a
#define OBD_FAIL_OST_PUNCH_NET           0x20b
#define OBD_FAIL_OST_STATFS_NET          0x20c
#define OBD_FAIL_OST_HANDLE_UNPACK       0x20d
#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
#define OBD_FAIL_OST_BRW_READ_BULK       0x20f
#define OBD_FAIL_OST_SYNC_NET            0x210
#define OBD_FAIL_OST_ALL_REPLY_NET       0x211
#define OBD_FAIL_OST_ALL_REQUEST_NET     0x212
#define OBD_FAIL_OST_LDLM_REPLY_NET      0x213
#define OBD_FAIL_OST_BRW_PAUSE_BULK      0x214
#define OBD_FAIL_OST_ENOSPC              0x215
#define OBD_FAIL_OST_EROFS               0x216
#define OBD_FAIL_SRV_ENOENT              0x217
/*	OBD_FAIL_OST_QUOTACHECK_NET      0x218 obsolete since 2.4 */
#define OBD_FAIL_OST_QUOTACTL_NET        0x219
#define OBD_FAIL_OST_CHECKSUM_RECEIVE    0x21a
#define OBD_FAIL_OST_CHECKSUM_SEND       0x21b
#define OBD_FAIL_OST_BRW_SIZE            0x21c
#define OBD_FAIL_OST_DROP_REQ            0x21d
#define OBD_FAIL_OST_SETATTR_CREDITS     0x21e
#define OBD_FAIL_OST_HOLD_WRITE_RPC      0x21f
#define OBD_FAIL_OST_BRW_WRITE_BULK2     0x220
#define OBD_FAIL_OST_LLOG_RECOVERY_TIMEOUT 0x221
#define OBD_FAIL_OST_CANCEL_COOKIE_TIMEOUT 0x222
#define OBD_FAIL_OST_PAUSE_CREATE        0x223
#define OBD_FAIL_OST_BRW_PAUSE_PACK      0x224
#define OBD_FAIL_OST_CONNECT_NET2        0x225
#define OBD_FAIL_OST_NOMEM               0x226
#define OBD_FAIL_OST_BRW_PAUSE_BULK2     0x227
#define OBD_FAIL_OST_MAPBLK_ENOSPC       0x228
#define OBD_FAIL_OST_ENOINO              0x229
#define OBD_FAIL_OST_DQACQ_NET           0x230
#define OBD_FAIL_OST_STATFS_EINPROGRESS  0x231
#define OBD_FAIL_OST_SET_INFO_NET        0x232
#define OBD_FAIL_OST_NODESTROY		 0x233
/*	OBD_FAIL_OST_READ_SIZE		 0x234 obsolete since 2.14 */
#define OBD_FAIL_OST_LADVISE_NET	 0x235
#define OBD_FAIL_OST_PAUSE_PUNCH         0x236
#define OBD_FAIL_OST_LADVISE_PAUSE	 0x237
#define OBD_FAIL_OST_FAKE_RW		 0x238
#define OBD_FAIL_OST_LIST_ASSERT         0x239
#define OBD_FAIL_OST_GL_WORK_ALLOC	 0x240
#define OBD_FAIL_OST_SKIP_LV_CHECK	 0x241
#define OBD_FAIL_OST_STATFS_DELAY	 0x242
#define OBD_FAIL_OST_INTEGRITY_FAULT	 0x243
#define OBD_FAIL_OST_INTEGRITY_CMP	 0x244
#define OBD_FAIL_OST_DISCONNECT_DELAY	 0x245
#define OBD_FAIL_OST_PREPARE_DELAY	 0x247
#define OBD_FAIL_OST_2BIG_NIOBUF	 0x248
#define OBD_FAIL_OST_FALLOCATE_NET	 0x249
#define OBD_FAIL_OST_SEEK_NET		 0x24a
#define OBD_FAIL_OST_WR_ATTR_DELAY	 0x250
#define OBD_FAIL_OST_RESTART_IO		 0x251
#define OBD_FAIL_OST_GET_LAST_FID	 0x252

#define OBD_FAIL_LDLM                    0x300
#define OBD_FAIL_LDLM_NAMESPACE_NEW      0x301
#define OBD_FAIL_LDLM_ENQUEUE_NET	 0x302
#define OBD_FAIL_LDLM_CONVERT_NET	 0x303
#define OBD_FAIL_LDLM_CANCEL_NET	 0x304
#define OBD_FAIL_LDLM_BL_CALLBACK_NET	 0x305
#define OBD_FAIL_LDLM_CP_CALLBACK_NET	 0x306
#define OBD_FAIL_LDLM_GL_CALLBACK_NET	 0x307
#define OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR 0x308
#define OBD_FAIL_LDLM_ENQUEUE_INTENT_ERR 0x309
#define OBD_FAIL_LDLM_CREATE_RESOURCE    0x30a
#define OBD_FAIL_LDLM_ENQUEUE_BLOCKED    0x30b
#define OBD_FAIL_LDLM_REPLY              0x30c
#define OBD_FAIL_LDLM_RECOV_CLIENTS      0x30d
#define OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT 0x30e
#define OBD_FAIL_LDLM_GLIMPSE            0x30f
#define OBD_FAIL_LDLM_CANCEL_RACE        0x310
#define OBD_FAIL_LDLM_CANCEL_EVICT_RACE  0x311
#define OBD_FAIL_LDLM_PAUSE_CANCEL       0x312
#define OBD_FAIL_LDLM_CLOSE_THREAD       0x313
#define OBD_FAIL_LDLM_CANCEL_BL_CB_RACE  0x314
#define OBD_FAIL_LDLM_CP_CB_WAIT         0x315
#define OBD_FAIL_LDLM_OST_FAIL_RACE      0x316
#define OBD_FAIL_LDLM_INTR_CP_AST        0x317
#define OBD_FAIL_LDLM_CP_BL_RACE         0x318
#define OBD_FAIL_LDLM_NEW_LOCK           0x319
#define OBD_FAIL_LDLM_AGL_DELAY          0x31a
#define OBD_FAIL_LDLM_AGL_NOLOCK         0x31b
#define OBD_FAIL_LDLM_OST_LVB		 0x31c
#define OBD_FAIL_LDLM_ENQUEUE_HANG	 0x31d
#define OBD_FAIL_LDLM_BL_EVICT           0x31e
#define OBD_FAIL_LDLM_PAUSE_CANCEL2      0x31f
#define OBD_FAIL_LDLM_CP_CB_WAIT2        0x320
#define OBD_FAIL_LDLM_CP_CB_WAIT3        0x321
#define OBD_FAIL_LDLM_CP_CB_WAIT4        0x322
#define OBD_FAIL_LDLM_CP_CB_WAIT5        0x323
#define OBD_FAIL_LDLM_SRV_BL_AST	 0x324
#define OBD_FAIL_LDLM_SRV_CP_AST	 0x325
#define OBD_FAIL_LDLM_SRV_GL_AST	 0x326
#define OBD_FAIL_LDLM_WATERMARK_LOW	 0x327
#define OBD_FAIL_LDLM_WATERMARK_HIGH	 0x328
#define OBD_FAIL_LDLM_PAUSE_CANCEL_LOCAL 0x329

#define OBD_FAIL_LDLM_GRANT_CHECK        0x32a
#define OBD_FAIL_LDLM_PROLONG_PAUSE	 0x32b
#define OBD_FAIL_LDLM_LOCAL_CANCEL_PAUSE 0x32c
#define OBD_FAIL_LDLM_LOCK_REPLAY	 0x32d
#define OBD_FAIL_LDLM_REPLAY_PAUSE	 0x32e

/* LOCKLESS IO */
#define OBD_FAIL_LDLM_SET_CONTENTION     0x385

#define OBD_FAIL_OSC                     0x400
#define OBD_FAIL_OSC_BRW_READ_BULK       0x401
#define OBD_FAIL_OSC_BRW_WRITE_BULK      0x402
#define OBD_FAIL_OSC_LOCK_BL_AST         0x403
#define OBD_FAIL_OSC_LOCK_CP_AST         0x404
#define OBD_FAIL_OSC_MATCH               0x405
#define OBD_FAIL_OSC_BRW_PREP_REQ        0x406
#define OBD_FAIL_OSC_SHUTDOWN            0x407
#define OBD_FAIL_OSC_CHECKSUM_RECEIVE    0x408
#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
#define OBD_FAIL_OSC_BRW_PREP_REQ2       0x40a
/* #define OBD_FAIL_OSC_CONNECT_CKSUM       0x40b Obsolete since 2.9 */
#define OBD_FAIL_OSC_CKSUM_ADLER_ONLY    0x40c
#define OBD_FAIL_OSC_DIO_PAUSE           0x40d
#define OBD_FAIL_OSC_OBJECT_CONTENTION   0x40e
#define OBD_FAIL_OSC_CP_CANCEL_RACE      0x40f
#define OBD_FAIL_OSC_CP_ENQ_RACE         0x410
#define OBD_FAIL_OSC_NO_GRANT            0x411
#define OBD_FAIL_OSC_DELAY_SETTIME	 0x412
#define OBD_FAIL_OSC_CONNECT_GRANT_PARAM 0x413
#define OBD_FAIL_OSC_DELAY_IO            0x414
#define OBD_FAIL_OSC_NO_SIZE_DATA        0x415
#define OBD_FAIL_OSC_DELAY_CANCEL        0x416
#define OBD_FAIL_OSC_SLOW_PAGE_EVICT	 0x417

#define OBD_FAIL_PTLRPC                  0x500
#define OBD_FAIL_PTLRPC_ACK              0x501
#define OBD_FAIL_PTLRPC_RQBD             0x502
#define OBD_FAIL_PTLRPC_BULK_GET_NET     0x503
#define OBD_FAIL_PTLRPC_BULK_PUT_NET     0x504
#define OBD_FAIL_PTLRPC_DROP_RPC         0x505
#define OBD_FAIL_PTLRPC_DELAY_SEND       0x506
#define OBD_FAIL_PTLRPC_DELAY_RECOV      0x507
#define OBD_FAIL_PTLRPC_CLIENT_BULK_CB   0x508
#define OBD_FAIL_PTLRPC_PAUSE_REQ        0x50a
#define OBD_FAIL_PTLRPC_PAUSE_REP        0x50c
#define OBD_FAIL_PTLRPC_IMP_DEACTIVE     0x50d
#define OBD_FAIL_PTLRPC_DUMP_LOG         0x50e
#define OBD_FAIL_PTLRPC_LONG_REPL_UNLINK 0x50f
#define OBD_FAIL_PTLRPC_LONG_BULK_UNLINK 0x510
#define OBD_FAIL_PTLRPC_HPREQ_TIMEOUT    0x511
#define OBD_FAIL_PTLRPC_HPREQ_NOTIMEOUT  0x512
#define OBD_FAIL_PTLRPC_DROP_REQ_OPC     0x513
#define OBD_FAIL_PTLRPC_FINISH_REPLAY    0x514
#define OBD_FAIL_PTLRPC_CLIENT_BULK_CB2  0x515
#define OBD_FAIL_PTLRPC_DELAY_IMP_FULL   0x516
#define OBD_FAIL_PTLRPC_CANCEL_RESEND    0x517
#define OBD_FAIL_PTLRPC_DROP_BULK	 0x51a
#define OBD_FAIL_PTLRPC_LONG_REQ_UNLINK  0x51b
#define OBD_FAIL_PTLRPC_LONG_BOTH_UNLINK 0x51c
#define OBD_FAIL_PTLRPC_CLIENT_BULK_CB3  0x520
#define OBD_FAIL_PTLRPC_BULK_ATTACH      0x521
#define OBD_FAIL_PTLRPC_BULK_REPLY_ATTACH      0x522
#define OBD_FAIL_PTLRPC_RESEND_RACE	 0x525
#define OBD_FAIL_PTLRPC_ROUND_XID	 0x530
#define OBD_FAIL_PTLRPC_CONNECT_RACE	 0x531
#define OBD_FAIL_NET_ERROR_RPC		 0x532
#define OBD_FAIL_PTLRPC_IDLE_RACE	 0x533
#define OBD_FAIL_PTLRPC_ENQ_RESEND	 0x534

#define OBD_FAIL_OBD_PING_NET            0x600
/*	OBD_FAIL_OBD_LOG_CANCEL_NET      0x601 obsolete since 1.5 */
#define OBD_FAIL_OBD_LOGD_NET            0x602
/*	OBD_FAIL_OBD_QC_CALLBACK_NET     0x603 obsolete since 2.4 */
#define OBD_FAIL_OBD_DQACQ               0x604
#define OBD_FAIL_OBD_LLOG_SETUP          0x605
/*	OBD_FAIL_OBD_LOG_CANCEL_REP      0x606 obsolete since 1.5 */
#define OBD_FAIL_OBD_IDX_READ_NET        0x607
#define OBD_FAIL_OBD_IDX_READ_BREAK	 0x608
#define OBD_FAIL_OBD_NO_LRU		 0x609
#define OBD_FAIL_OBDCLASS_MODULE_LOAD	 0x60a
#define OBD_FAIL_OBD_ZERO_NLINK_RACE	 0x60b
#define OBD_FAIL_OBD_STOP_MDS_RACE	 0x60c
#define OBD_FAIL_OBD_SETUP		 0x60d

#define OBD_FAIL_TGT_REPLY_NET           0x700
#define OBD_FAIL_TGT_CONN_RACE           0x701
#define OBD_FAIL_TGT_FORCE_RECONNECT     0x702
#define OBD_FAIL_TGT_DELAY_CONNECT       0x703
#define OBD_FAIL_TGT_DELAY_RECONNECT     0x704
#define OBD_FAIL_TGT_DELAY_PRECREATE     0x705
#define OBD_FAIL_TGT_TOOMANY_THREADS     0x706
#define OBD_FAIL_TGT_REPLAY_DROP         0x707
#define OBD_FAIL_TGT_FAKE_EXP            0x708
#define OBD_FAIL_TGT_REPLAY_DELAY        0x709
/* #define OBD_FAIL_TGT_LAST_REPLAY         0x710 (obsoleted) */
#define OBD_FAIL_TGT_CLIENT_ADD          0x711
#define OBD_FAIL_TGT_RCVG_FLAG           0x712
#define OBD_FAIL_TGT_DELAY_CONDITIONAL	 0x713
#define OBD_FAIL_TGT_REPLAY_DELAY2       0x714
#define OBD_FAIL_TGT_REPLAY_RECONNECT	 0x715
#define OBD_FAIL_TGT_MOUNT_RACE		 0x716
#define OBD_FAIL_TGT_REPLAY_TIMEOUT	 0x717
#define OBD_FAIL_TGT_CLIENT_DEL		 0x718
#define OBD_FAIL_TGT_SLUGGISH_NET	 0x719
#define OBD_FAIL_TGT_RCVD_EIO		 0x720
#define OBD_FAIL_TGT_RECOVERY_REQ_RACE	 0x721
#define OBD_FAIL_TGT_REPLY_DATA_RACE	 0x722
#define OBD_FAIL_TGT_RECOVERY_CONNECT    0x724
#define OBD_FAIL_TGT_NO_GRANT		 0x725
#define OBD_FAIL_TGT_TXN_NO_CANCEL	 0x726

#define OBD_FAIL_MDC_REVALIDATE_PAUSE    0x800
#define OBD_FAIL_MDC_ENQUEUE_PAUSE       0x801
#define OBD_FAIL_MDC_OLD_EXT_FLAGS       0x802
#define OBD_FAIL_MDC_GETATTR_ENQUEUE     0x803
#define OBD_FAIL_MDC_RPCS_SEM		 0x804 /* deprecated */
#define OBD_FAIL_MDC_LIGHTWEIGHT	 0x805
#define OBD_FAIL_MDC_CLOSE		 0x806
#define OBD_FAIL_MDC_MERGE		 0x807
#define OBD_FAIL_MDC_GLIMPSE_DDOS	 0x808

#define OBD_FAIL_MGS                     0x900
#define OBD_FAIL_MGS_ALL_REQUEST_NET     0x901
#define OBD_FAIL_MGS_ALL_REPLY_NET       0x902
#define OBD_FAIL_MGC_PAUSE_PROCESS_LOG   0x903
#define OBD_FAIL_MGS_PAUSE_REQ           0x904
#define OBD_FAIL_MGS_PAUSE_TARGET_REG    0x905
#define OBD_FAIL_MGS_CONNECT_NET	 0x906
#define OBD_FAIL_MGS_DISCONNECT_NET	 0x907
#define OBD_FAIL_MGS_SET_INFO_NET	 0x908
#define OBD_FAIL_MGS_EXCEPTION_NET	 0x909
#define OBD_FAIL_MGS_TARGET_REG_NET	 0x90a
#define OBD_FAIL_MGS_TARGET_DEL_NET	 0x90b
#define OBD_FAIL_MGS_CONFIG_READ_NET	 0x90c
#define OBD_FAIL_MGS_LDLM_REPLY_NET	 0x90d
#define OBD_FAIL_MGS_WRITE_TARGET_DELAY	 0x90e

#define OBD_FAIL_QUOTA_DQACQ_NET			0xA01
#define OBD_FAIL_QUOTA_EDQUOT            0xA02
#define OBD_FAIL_QUOTA_DELAY_REINT       0xA03
#define OBD_FAIL_QUOTA_RECOVERABLE_ERR   0xA04
#define OBD_FAIL_QUOTA_INIT              0xA05
#define OBD_FAIL_QUOTA_PREACQ            0xA06
#define OBD_FAIL_QUOTA_RECALC            0xA07

#define OBD_FAIL_LPROC_REMOVE            0xB00

#define OBD_FAIL_SEQ                     0x1000
#define OBD_FAIL_SEQ_QUERY_NET           0x1001
#define OBD_FAIL_SEQ_EXHAUST		 0x1002

#define OBD_FAIL_FLD                     0x1100
#define OBD_FAIL_FLD_QUERY_NET           0x1101
#define OBD_FAIL_FLD_READ_NET		 0x1102
#define OBD_FAIL_FLD_QUERY_REQ		 0x1103

#define OBD_FAIL_SEC_CTX                 0x1200
#define OBD_FAIL_SEC_CTX_INIT_NET        0x1201
#define OBD_FAIL_SEC_CTX_INIT_CONT_NET   0x1202
#define OBD_FAIL_SEC_CTX_FINI_NET        0x1203
#define OBD_FAIL_SEC_CTX_HDL_PAUSE       0x1204

#define OBD_FAIL_LLOG                               0x1300
/* was	OBD_FAIL_LLOG_ORIGIN_CONNECT_NET            0x1301 until 2.4 */
#define OBD_FAIL_LLOG_ORIGIN_HANDLE_CREATE_NET      0x1302
/* was	OBD_FAIL_LLOG_ORIGIN_HANDLE_DESTROY_NET     0x1303 until 2.11 */
#define OBD_FAIL_LLOG_ORIGIN_HANDLE_READ_HEADER_NET 0x1304
#define OBD_FAIL_LLOG_ORIGIN_HANDLE_NEXT_BLOCK_NET  0x1305
#define OBD_FAIL_LLOG_ORIGIN_HANDLE_PREV_BLOCK_NET  0x1306
/* was	OBD_FAIL_LLOG_ORIGIN_HANDLE_WRITE_REC_NET   0x1307 until 2.1 */
/* was	OBD_FAIL_LLOG_ORIGIN_HANDLE_CLOSE_NET       0x1308 until 1.8 */
/* was	OBD_FAIL_LLOG_CATINFO_NET                   0x1309 until 2.3 */
#define OBD_FAIL_MDS_SYNC_CAPA_SL                   0x1310
#define OBD_FAIL_SEQ_ALLOC                          0x1311
#define OBD_FAIL_CAT_RECORDS			    0x1312
#define OBD_FAIL_CAT_FREE_RECORDS		    0x1313
#define OBD_FAIL_TIME_IN_CHLOG_USER		    0x1314
#define CFS_FAIL_CHLOG_USER_REG_UNREG_RACE	    0x1315
#define OBD_FAIL_FORCE_GC_THREAD		    0x1316
#define OBD_FAIL_LLOG_PROCESS_TIMEOUT		    0x1317
#define OBD_FAIL_LLOG_PURGE_DELAY		    0x1318
#define OBD_FAIL_PLAIN_RECORDS			    0x1319
#define OBD_FAIL_CATALOG_FULL_CHECK		    0x131a
#define OBD_FAIL_CATLIST			    0x131b
#define OBD_FAIL_LLOG_PAUSE_AFTER_PAD               0x131c
#define OBD_FAIL_LLOG_ADD_GAP			    0x131d

#define OBD_FAIL_LLITE                              0x1400
#define OBD_FAIL_LLITE_FAULT_TRUNC_RACE             0x1401
#define OBD_FAIL_LOCK_STATE_WAIT_INTR               0x1402
#define OBD_FAIL_LOV_INIT			    0x1403
#define OBD_FAIL_GLIMPSE_DELAY			    0x1404
#define OBD_FAIL_LLITE_XATTR_ENOMEM		    0x1405
#define OBD_FAIL_MAKE_LOVEA_HOLE		    0x1406
#define OBD_FAIL_LLITE_LOST_LAYOUT		    0x1407
#define OBD_FAIL_LLITE_NO_CHECK_DEAD		    0x1408
#define OBD_FAIL_GETATTR_DELAY			    0x1409
#define OBD_FAIL_LLITE_CREATE_FILE_PAUSE	    0x1409
#define OBD_FAIL_LLITE_NEWNODE_PAUSE		    0x140a
#define OBD_FAIL_LLITE_SETDIRSTRIPE_PAUSE	    0x140b
#define OBD_FAIL_LLITE_CREATE_NODE_PAUSE	    0x140c
#define OBD_FAIL_LLITE_IMUTEX_SEC		    0x140e
#define OBD_FAIL_LLITE_IMUTEX_NOSEC		    0x140f
#define OBD_FAIL_LLITE_OPEN_BY_NAME		    0x1410
#define OBD_FAIL_LLITE_PCC_FAKE_ERROR		    0x1411
#define OBD_FAIL_LLITE_PCC_DETACH_MKWRITE	    0x1412
#define OBD_FAIL_LLITE_PCC_MKWRITE_PAUSE	    0x1413
#define OBD_FAIL_LLITE_PCC_ATTACH_PAUSE		    0x1414
#define OBD_FAIL_LLITE_SHORT_COMMIT		    0x1415
#define OBD_FAIL_LLITE_CREATE_FILE_PAUSE2	    0x1416
#define OBD_FAIL_LLITE_RACE_MOUNT		    0x1417
#define OBD_FAIL_LLITE_PAGE_ALLOC		    0x1418
#define OBD_FAIL_LLITE_OPEN_DELAY		    0x1419
#define OBD_FAIL_LLITE_XATTR_PAUSE		    0x1420
#define OBD_FAIL_LLITE_READPAGE_PAUSE2		    0x1424

#define OBD_FAIL_FID_INDIR	0x1501
#define OBD_FAIL_FID_INLMA	0x1502
#define OBD_FAIL_FID_IGIF	0x1504
#define OBD_FAIL_FID_LOOKUP	0x1505
#define OBD_FAIL_FID_NOLMA	0x1506

/* LFSCK */
#define OBD_FAIL_LFSCK_DELAY1		0x1600
#define OBD_FAIL_LFSCK_DELAY2		0x1601
#define OBD_FAIL_LFSCK_DELAY3		0x1602
#define OBD_FAIL_LFSCK_LINKEA_CRASH	0x1603
#define OBD_FAIL_LFSCK_LINKEA_MORE	0x1604
#define OBD_FAIL_LFSCK_LINKEA_MORE2	0x1605
#define OBD_FAIL_LFSCK_FATAL1		0x1608
#define OBD_FAIL_LFSCK_FATAL2		0x1609
#define OBD_FAIL_LFSCK_CRASH		0x160a
#define OBD_FAIL_LFSCK_NO_AUTO		0x160b
#define OBD_FAIL_LFSCK_NO_DOUBLESCAN	0x160c
#define OBD_FAIL_LFSCK_SKIP_LASTID	0x160d
#define OBD_FAIL_LFSCK_DELAY4		0x160e
#define OBD_FAIL_LFSCK_BAD_LMMOI	0x160f
#define OBD_FAIL_LFSCK_DANGLING 	0x1610
#define OBD_FAIL_LFSCK_UNMATCHED_PAIR1	0x1611
#define OBD_FAIL_LFSCK_UNMATCHED_PAIR2	0x1612
#define OBD_FAIL_LFSCK_BAD_OWNER	0x1613
#define OBD_FAIL_LFSCK_MULTIPLE_REF	0x1614
#define OBD_FAIL_LFSCK_LOST_STRIPE	0x1615
#define OBD_FAIL_LFSCK_LOST_MDTOBJ	0x1616
#define OBD_FAIL_LFSCK_NOPFID		0x1617
#define OBD_FAIL_LFSCK_CHANGE_STRIPE	0x1618
#define OBD_FAIL_LFSCK_INVALID_PFID	0x1619
#define OBD_FAIL_LFSCK_LOST_SPEOBJ	0x161a
#define OBD_FAIL_LFSCK_DELAY5		0x161b
#define OBD_FAIL_LFSCK_BAD_NETWORK	0x161c
#define OBD_FAIL_LFSCK_NO_LINKEA	0x161d
#define OBD_FAIL_LFSCK_BAD_PARENT	0x161e
#define OBD_FAIL_LFSCK_DANGLING2	0x1620
#define OBD_FAIL_LFSCK_DANGLING3	0x1621
#define OBD_FAIL_LFSCK_MUL_REF		0x1622
#define OBD_FAIL_LFSCK_BAD_TYPE		0x1623
#define OBD_FAIL_LFSCK_NO_NAMEENTRY	0x1624
#define OBD_FAIL_LFSCK_LESS_NLINK	0x1626
#define OBD_FAIL_LFSCK_BAD_NAME_HASH	0x1628
#define OBD_FAIL_LFSCK_LOST_MASTER_LMV	0x1629
#define OBD_FAIL_LFSCK_LOST_SLAVE_LMV	0x162a
#define OBD_FAIL_LFSCK_BAD_SLAVE_LMV	0x162b
#define OBD_FAIL_LFSCK_BAD_SLAVE_NAME	0x162c
#define OBD_FAIL_LFSCK_ENGINE_DELAY	0x162d
#define OBD_FAIL_LFSCK_LOST_MDTOBJ2	0x162e
#define OBD_FAIL_LFSCK_BAD_PFL_RANGE	0x162f
#define OBD_FAIL_LFSCK_NO_AGENTOBJ	0x1630
#define OBD_FAIL_LFSCK_NO_AGENTENT	0x1631
#define OBD_FAIL_LFSCK_NO_ENCFLAG	0x1632

#define OBD_FAIL_LFSCK_NOTIFY_NET	0x16f0
#define OBD_FAIL_LFSCK_QUERY_NET	0x16f1

/* UPDATE */
#define OBD_FAIL_OUT_UPDATE_NET		0x1700
#define OBD_FAIL_OUT_UPDATE_NET_REP	0x1701
#define OBD_FAIL_SPLIT_UPDATE_REC	0x1702
#define OBD_FAIL_LARGE_STRIPE		0x1703
#define OBD_FAIL_OUT_ENOSPC             0x1704
#define OBD_FAIL_INVALIDATE_UPDATE	0x1705
#define OBD_FAIL_OUT_UPDATE_DROP        0x1707
#define OBD_FAIL_OUT_OBJECT_MISS	0x1708

/* MIGRATE */
#define OBD_FAIL_MIGRATE_ENTRIES		0x1801

/* LMV */
#define OBD_FAIL_UNKNOWN_LMV_STRIPE		0x1901

/* FLR */
#define OBD_FAIL_FLR_GLIMPSE_IMMUTABLE		0x1A00
#define OBD_FAIL_FLR_LV_DELAY			0x1A01
#define OBD_FAIL_FLR_LV_INC			0x1A02
#define OBD_FAIL_FLR_RANDOM_PICK_MIRROR	0x1A03

/* DT */
#define OBD_FAIL_DT_DECLARE_ATTR_GET		0x2000
#define OBD_FAIL_DT_ATTR_GET			0x2001
#define OBD_FAIL_DT_DECLARE_ATTR_SET		0x2002
#define OBD_FAIL_DT_ATTR_SET			0x2003
#define OBD_FAIL_DT_DECLARE_XATTR_GET		0x2004
#define OBD_FAIL_DT_XATTR_GET			0x2005
#define OBD_FAIL_DT_DECLARE_XATTR_SET		0x2006
#define OBD_FAIL_DT_XATTR_SET			0x2007
#define OBD_FAIL_DT_DECLARE_XATTR_DEL		0x2008
#define OBD_FAIL_DT_XATTR_DEL			0x2009
#define OBD_FAIL_DT_XATTR_LIST			0x200a
#define OBD_FAIL_DT_DECLARE_CREATE		0x200b
#define OBD_FAIL_DT_CREATE			0x200c
#define OBD_FAIL_DT_DECLARE_DESTROY		0x200d
#define OBD_FAIL_DT_DESTROY			0x200e
#define OBD_FAIL_DT_INDEX_TRY			0x200f
#define OBD_FAIL_DT_DECLARE_REF_ADD		0x2010
#define OBD_FAIL_DT_REF_ADD			0x2011
#define OBD_FAIL_DT_DECLARE_REF_DEL		0x2012
#define OBD_FAIL_DT_REF_DEL			0x2013
#define OBD_FAIL_DT_DECLARE_INSERT		0x2014
#define OBD_FAIL_DT_INSERT			0x2015
#define OBD_FAIL_DT_DECLARE_DELETE		0x2016
#define OBD_FAIL_DT_DELETE			0x2017
#define OBD_FAIL_DT_LOOKUP			0x2018
#define OBD_FAIL_DT_TXN_STOP			0x2019

#define OBD_FAIL_OSP_CHECK_INVALID_REC		0x2100
#define OBD_FAIL_OSP_CHECK_ENOMEM		0x2101
#define OBD_FAIL_OSP_FAKE_PRECREATE		0x2102
#define OBD_FAIL_OSP_RPCS_SEM			0x2104
#define OBD_FAIL_OSP_CANT_PROCESS_LLOG		0x2105
#define OBD_FAIL_OSP_INVALID_LOGID		0x2106
#define OBD_FAIL_OSP_CON_EVENT_DELAY		0x2107
#define OBD_FAIL_OSP_PRECREATE_PAUSE		0x2108
#define OBD_FAIL_OSP_GET_LAST_FID		0x2109

/* barrier */
#define OBD_FAIL_MGS_BARRIER_READ_NET		0x2200
#define OBD_FAIL_MGS_BARRIER_NOTIFY_NET		0x2201

#define OBD_FAIL_BARRIER_DELAY			0x2202
#define OBD_FAIL_BARRIER_FAILURE		0x2203

#define OBD_FAIL_OSD_FAIL_AT_TRUNCATE		0x2301

/* LNet is allocated failure locations 0xe000 to 0xffff */
/* Assign references to moved code to reduce code changes */
#define OBD_FAIL_PRECHECK(id)                   (unlikely(CFS_FAIL_PRECHECK(id)))
#define OBD_FAIL_CHECK(id)                      CFS_FAIL_CHECK(id)
#define OBD_FAIL_CHECK_QUIET(id)                CFS_FAIL_CHECK_QUIET(id)
#define OBD_FAIL_CHECK_VALUE(id, value)         CFS_FAIL_CHECK_VALUE(id, value)
#define OBD_FAIL_CHECK_ORSET(id, value)         CFS_FAIL_CHECK_ORSET(id, value)
#define OBD_FAIL_CHECK_RESET(id, value)         CFS_FAIL_CHECK_RESET(id, value)
#define OBD_FAIL_RETURN(id, ret)                CFS_FAIL_RETURN(id, ret)
#define OBD_FAIL_TIMEOUT(id, secs)              CFS_FAIL_TIMEOUT(id, secs)
#define OBD_FAIL_TIMEOUT_MS(id, ms)             CFS_FAIL_TIMEOUT_MS(id, ms)
#define OBD_FAIL_TIMEOUT_ORSET(id, value, secs) CFS_FAIL_TIMEOUT_ORSET(id, value, secs)
#define OBD_RACE(id)                            CFS_RACE(id)
#define OBD_FAIL_ONCE                           CFS_FAIL_ONCE
#define OBD_FAILED                              CFS_FAILED

#define LUT_FAIL_CLASS(fail_id)			(((fail_id) >> 8) << 16)
#define LUT_FAIL_MGT				LUT_FAIL_CLASS(OBD_FAIL_MGS)
#define LUT_FAIL_MDT				LUT_FAIL_CLASS(OBD_FAIL_MDS)
#define LUT_FAIL_OST				LUT_FAIL_CLASS(OBD_FAIL_OST)

extern atomic64_t libcfs_kmem;

#ifdef CONFIG_PROC_FS
#define obd_memory_add(size)                                                  \
        lprocfs_counter_add(obd_memory, OBD_MEMORY_STAT, (long)(size))
#define obd_memory_sub(size)                                                  \
        lprocfs_counter_sub(obd_memory, OBD_MEMORY_STAT, (long)(size))
#define obd_memory_sum()                                                      \
        lprocfs_stats_collector(obd_memory, OBD_MEMORY_STAT,                  \
                                LPROCFS_FIELDS_FLAGS_SUM)

extern void obd_update_maxusage(void);
extern __u64 obd_memory_max(void);

#else /* CONFIG_PROC_FS */

extern __u64 obd_alloc;

extern __u64 obd_max_alloc;

static inline void obd_memory_add(long size)
{
        obd_alloc += size;
        if (obd_alloc > obd_max_alloc)
                obd_max_alloc = obd_alloc;
}

static inline void obd_memory_sub(long size)
{
        obd_alloc -= size;
}

#define obd_memory_sum() (obd_alloc)

#define obd_memory_max() (obd_max_alloc)

#endif /* !CONFIG_PROC_FS */

#define OBD_DEBUG_MEMUSAGE (1)

#if OBD_DEBUG_MEMUSAGE
#define OBD_ALLOC_POST(ptr, size, name)                                 \
                obd_memory_add(size);                                   \
                CDEBUG(D_MALLOC, name " '" #ptr "': %d at %p.\n",       \
                       (int)(size), ptr)

#define OBD_FREE_PRE(ptr, size, name)                                   \
        LASSERT(ptr);                                                   \
        obd_memory_sub(size);                                           \
        CDEBUG(D_MALLOC, name " '" #ptr "': %d at %p.\n",               \
	       (int)(size), ptr);

#else /* !OBD_DEBUG_MEMUSAGE */

#define OBD_ALLOC_POST(ptr, size, name) ((void)0)
#define OBD_FREE_PRE(ptr, size, name)   ((void)0)

#endif /* !OBD_DEBUG_MEMUSAGE */

#define __OBD_MALLOC_VERBOSE(ptr, cptab, cpt, size, flags)		      \
do {									      \
	if (cptab)							      \
		ptr = cfs_cpt_malloc((cptab), (cpt), (size),		      \
				     (flags) | __GFP_ZERO | __GFP_NOWARN);    \
	if (!(cptab) || unlikely(!(ptr))) /* retry without CPT if failure */  \
		ptr = kmalloc(size, (flags) | __GFP_ZERO);		      \
	if (likely((ptr) != NULL))					      \
		OBD_ALLOC_POST((ptr), (size), "kmalloced");		      \
} while (0)

#define OBD_ALLOC_GFP(ptr, size, gfp_mask)				      \
	__OBD_MALLOC_VERBOSE(ptr, NULL, 0, size, gfp_mask)

#define OBD_ALLOC(ptr, size) OBD_ALLOC_GFP(ptr, size, GFP_NOFS)
#define OBD_ALLOC_WAIT(ptr, size) OBD_ALLOC_GFP(ptr, size, GFP_KERNEL)
#define OBD_ALLOC_PTR(ptr) OBD_ALLOC(ptr, sizeof(*(ptr)))
#define OBD_ALLOC_PTR_WAIT(ptr) OBD_ALLOC_WAIT(ptr, sizeof(*(ptr)))
#define OBD_ALLOC_PTR_ARRAY(ptr, n) OBD_ALLOC(ptr, (n) * sizeof(*(ptr)))
#define OBD_ALLOC_PTR_ARRAY_WAIT(ptr, n)				      \
		OBD_ALLOC_WAIT(ptr, (n) * sizeof(*(ptr)))

#define OBD_CPT_ALLOC_GFP(ptr, cptab, cpt, size, gfp_mask)		      \
	__OBD_MALLOC_VERBOSE(ptr, cptab, cpt, size, gfp_mask)

#define OBD_CPT_ALLOC(ptr, cptab, cpt, size)				      \
	OBD_CPT_ALLOC_GFP(ptr, cptab, cpt, size, GFP_NOFS)

#define OBD_CPT_ALLOC_PTR(ptr, cptab, cpt)				      \
	OBD_CPT_ALLOC(ptr, cptab, cpt, sizeof(*(ptr)))

/* Direct use of __vmalloc() allows for protection flag specification
 * (and particularly to not set __GFP_FS, which is likely to cause some
 * deadlock situations in our code).
 */
#define __OBD_VMALLOC_VERBOSE(ptr, cptab, cpt, size)			      \
do {									      \
	(ptr) = cptab == NULL ?						      \
		__ll_vmalloc(size, GFP_NOFS | __GFP_HIGHMEM | __GFP_ZERO) :   \
		cfs_cpt_vzalloc(cptab, cpt, size);			      \
	if (unlikely((ptr) == NULL)) {                                        \
		CERROR("vmalloc of '" #ptr "' (%d bytes) failed\n",           \
		       (int)(size));                                          \
		CERROR("%llu total bytes allocated by Lustre, %lld by LNET\n",\
		       obd_memory_sum(), libcfs_kmem_read());\
	} else {                                                              \
		OBD_ALLOC_POST(ptr, size, "vmalloced");                       \
	}                                                                     \
} while(0)

#define OBD_VMALLOC(ptr, size)						      \
	 __OBD_VMALLOC_VERBOSE(ptr, NULL, 0, size)
#define OBD_CPT_VMALLOC(ptr, cptab, cpt, size)				      \
	 __OBD_VMALLOC_VERBOSE(ptr, cptab, cpt, size)

#define OBD_ALLOC_LARGE(ptr, size)                                            \
do {                                                                          \
	/* LU-8196 - force large allocations to use vmalloc, not kmalloc */   \
	if ((size) > KMALLOC_MAX_SIZE)                                          \
		ptr = NULL;                                                   \
	else                                                                  \
		OBD_ALLOC_GFP(ptr, size, GFP_NOFS | __GFP_NOWARN);            \
	if (ptr == NULL)                                                      \
                OBD_VMALLOC(ptr, size);                                       \
} while (0)

#define OBD_ALLOC_PTR_ARRAY_LARGE(ptr, n)				\
	OBD_ALLOC_LARGE(ptr, (n) * sizeof(*(ptr)))

#define OBD_CPT_ALLOC_LARGE(ptr, cptab, cpt, size)			      \
do {									      \
	OBD_CPT_ALLOC_GFP(ptr, cptab, cpt, size, GFP_NOFS | __GFP_NOWARN);    \
	if (ptr == NULL)                                                      \
		OBD_CPT_VMALLOC(ptr, cptab, cpt, size);			      \
} while (0)

#ifdef CONFIG_DEBUG_SLAB
#define POISON(ptr, c, s) do {} while (0)
#define POISON_PTR(ptr)  ((void)0)
#else
#define POISON(ptr, c, s) memset(ptr, c, s)
#define POISON_PTR(ptr)  (ptr) = (void *)0xdeadbeef
#endif

#ifdef POISON_BULK
#define POISON_PAGE(page, val) do { memset(kmap(page), val, PAGE_SIZE); \
                                    kunmap(page); } while (0)
#else
#define POISON_PAGE(page, val) do { } while (0)
#endif

#define OBD_FREE(ptr, size)						      \
do {									      \
	OBD_FREE_PRE(ptr, size, "kfreed");				      \
	POISON(ptr, 0x5a, size);					      \
	kfree(ptr);							      \
	POISON_PTR(ptr);						      \
} while (0)

#define OBD_FREE_LARGE(ptr, size)					      \
do {									      \
	if (is_vmalloc_addr(ptr)) {					      \
		OBD_FREE_PRE(ptr, size, "vfreed");			      \
		POISON(ptr, 0x5a, size);				      \
		libcfs_vfree_atomic(ptr);				      \
		POISON_PTR(ptr);					      \
	} else {							      \
		OBD_FREE(ptr, size);					      \
	}                                                                     \
} while (0)

#define OBD_FREE_PTR_ARRAY_LARGE(ptr, n)			\
	OBD_FREE_LARGE(ptr, (n) * sizeof(*(ptr)))

/* we memset() the slab object to 0 when allocation succeeds, so DO NOT
 * HAVE A CTOR THAT DOES ANYTHING.  its work will be cleared here.  we'd
 * love to assert on that, but slab.c keeps kmem_cache_s all to itself. */
#define OBD_SLAB_FREE_RTN0(ptr, slab)                                         \
({                                                                            \
	kmem_cache_free((slab), (ptr));                                    \
        (ptr) = NULL;                                                         \
        0;                                                                    \
})

#define __OBD_SLAB_ALLOC_VERBOSE(ptr, slab, cptab, cpt, size, type)	      \
do {									      \
	LASSERT(ergo((type) != GFP_ATOMIC, !in_interrupt()));		      \
	(ptr) = (cptab) == NULL ?					      \
		kmem_cache_zalloc(slab, (type)) :			      \
		cfs_mem_cache_cpt_alloc(slab, cptab, cpt, (type) | __GFP_ZERO); \
	if (likely((ptr)))                                                    \
		OBD_ALLOC_POST(ptr, size, "slab-alloced");                    \
} while(0)

#define OBD_SLAB_ALLOC_GFP(ptr, slab, size, flags)			      \
	__OBD_SLAB_ALLOC_VERBOSE(ptr, slab, NULL, 0, size, flags)
#define OBD_SLAB_CPT_ALLOC_GFP(ptr, slab, cptab, cpt, size, flags)	      \
	__OBD_SLAB_ALLOC_VERBOSE(ptr, slab, cptab, cpt, size, flags)

#define OBD_FREE_PTR(ptr) OBD_FREE(ptr, sizeof(*(ptr)))
#define OBD_FREE_PTR_ARRAY(ptr, n) OBD_FREE(ptr, (n) * sizeof(*(ptr)))

#define OBD_SLAB_FREE(ptr, slab, size)                                        \
do {                                                                          \
        OBD_FREE_PRE(ptr, size, "slab-freed");                                \
	POISON(ptr, 0x5a, size);					      \
	kmem_cache_free(slab, ptr);                                        \
        POISON_PTR(ptr);                                                      \
} while(0)

#define OBD_SLAB_ALLOC(ptr, slab, size)					      \
	OBD_SLAB_ALLOC_GFP(ptr, slab, size, GFP_NOFS)

#define OBD_SLAB_CPT_ALLOC(ptr, slab, cptab, cpt, size)			      \
	OBD_SLAB_CPT_ALLOC_GFP(ptr, slab, cptab, cpt, size, GFP_NOFS)

#define OBD_SLAB_ALLOC_PTR(ptr, slab)					      \
	OBD_SLAB_ALLOC(ptr, slab, sizeof(*(ptr)))

#define OBD_SLAB_CPT_ALLOC_PTR(ptr, slab, cptab, cpt)			      \
	OBD_SLAB_CPT_ALLOC(ptr, slab, cptab, cpt, sizeof(*(ptr)))

#define OBD_SLAB_ALLOC_PTR_GFP(ptr, slab, flags)			      \
	OBD_SLAB_ALLOC_GFP(ptr, slab, sizeof(*(ptr)), flags)

#define OBD_SLAB_CPT_ALLOC_PTR_GFP(ptr, slab, cptab, cpt, flags)		      \
	OBD_SLAB_CPT_ALLOC_GFP(ptr, slab, cptab, cpt, sizeof(*(ptr)), flags)

#define OBD_SLAB_FREE_PTR(ptr, slab)					      \
	OBD_SLAB_FREE((ptr), (slab), sizeof(*(ptr)))

#define KEY_IS(str) \
        (keylen >= (sizeof(str)-1) && memcmp(key, str, (sizeof(str)-1)) == 0)

#ifdef HAVE_SERVER_SUPPORT
/* LUSTRE_LMA_FL_MASKS defines which flags will be stored in LMA */

static inline int lma_to_lustre_flags(__u32 lma_flags)
{
	return (((lma_flags & LMAI_ORPHAN) ? LUSTRE_ORPHAN_FL : 0) |
		((lma_flags & LMAI_ENCRYPT) ? LUSTRE_ENCRYPT_FL : 0));
}

static inline int lustre_to_lma_flags(__u32 la_flags)
{
	return (((la_flags & LUSTRE_ORPHAN_FL) ? LMAI_ORPHAN : 0) |
		((la_flags & LUSTRE_ENCRYPT_FL) ? LMAI_ENCRYPT : 0));
}
#endif /* HAVE_SERVER_SUPPORT */

/* Convert wire LUSTRE_*_FL to corresponding client local VFS S_* values
 * for the client inode i_flags.  The LUSTRE_*_FL are the Lustre wire
 * protocol equivalents of LDISKFS_*_FL values stored on disk, while
 * the S_* flags are kernel-internal values that change between kernel
 * versions. These flags are set/cleared via FSFILT_IOC_{GET,SET}_FLAGS.
 * See b=16526 for a full history.
 */
static inline int ll_ext_to_inode_flags(int ext_flags)
{
	return (((ext_flags & LUSTRE_SYNC_FL)      ? S_SYNC      : 0) |
		((ext_flags & LUSTRE_NOATIME_FL)   ? S_NOATIME   : 0) |
		((ext_flags & LUSTRE_APPEND_FL)    ? S_APPEND    : 0) |
		((ext_flags & LUSTRE_DIRSYNC_FL)   ? S_DIRSYNC   : 0) |
#if defined(S_ENCRYPTED)
		((ext_flags & LUSTRE_ENCRYPT_FL)   ? S_ENCRYPTED : 0) |
#endif
		((ext_flags & LUSTRE_IMMUTABLE_FL) ? S_IMMUTABLE : 0));
}

static inline int ll_inode_to_ext_flags(int inode_flags)
{
	return (((inode_flags & S_SYNC)      ? LUSTRE_SYNC_FL      : 0) |
		((inode_flags & S_NOATIME)   ? LUSTRE_NOATIME_FL   : 0) |
		((inode_flags & S_APPEND)    ? LUSTRE_APPEND_FL    : 0) |
		((inode_flags & S_DIRSYNC)   ? LUSTRE_DIRSYNC_FL   : 0) |
#if defined(S_ENCRYPTED)
		((inode_flags & S_ENCRYPTED) ? LUSTRE_ENCRYPT_FL   : 0) |
#endif
		((inode_flags & S_IMMUTABLE) ? LUSTRE_IMMUTABLE_FL : 0));
}

struct obd_heat_instance {
	__u64 ohi_heat;
	__u64 ohi_time_second;
	__u64 ohi_count;
};

/* Define a fixed 4096-byte encryption unit size */
#define LUSTRE_ENCRYPTION_BLOCKBITS   12
#define LUSTRE_ENCRYPTION_UNIT_SIZE   ((size_t)1 << LUSTRE_ENCRYPTION_BLOCKBITS)
#define LUSTRE_ENCRYPTION_MASK        (~(LUSTRE_ENCRYPTION_UNIT_SIZE - 1))

#endif
