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
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lnet/selftest/console.h
 *
 * kernel structure for LST console
 *
 * Author: Liang Zhen <liangzhen@clusterfs.com>
 */

#ifndef __LST_CONSOLE_H__
#define __LST_CONSOLE_H__

#ifdef __KERNEL__

#include <libcfs/libcfs.h>
#include <lnet/lnet.h>
#include <lnet/lib-types.h>
#include <lnet/lnetst.h>
#include "selftest.h"
#include "conrpc.h"

typedef struct lstcon_node {
        lnet_process_id_t    nd_id;          /* id of the node */
        int                  nd_ref;         /* reference count */
        int                  nd_state;       /* state of the node */
        int                  nd_timeout;     /* session timeout */
        cfs_time_t           nd_stamp;       /* timestamp of last replied RPC */
        struct lstcon_rpc    nd_ping;        /* ping rpc */
} lstcon_node_t;                                /*** node descriptor */

typedef struct {
        cfs_list_t           ndl_link;       /* chain on list */
        cfs_list_t           ndl_hlink;      /* chain on hash */
        lstcon_node_t       *ndl_node;       /* pointer to node */
} lstcon_ndlink_t;                              /*** node link descriptor */

typedef struct {
        cfs_list_t           grp_link;       /* chain on global group list */
        int                  grp_ref;        /* reference count */
        int                  grp_userland;   /* has userland nodes */
        int                  grp_nnode;      /* # of nodes */
        char                 grp_name[LST_NAME_SIZE]; /* group name */

        cfs_list_t           grp_trans_list; /* transaction list */
        cfs_list_t           grp_ndl_list;   /* nodes list */
        cfs_list_t           grp_ndl_hash[0];/* hash table for nodes */
} lstcon_group_t;                    /*** (alias of nodes) group descriptor */

#define LST_BATCH_IDLE          0xB0            /* idle batch */
#define LST_BATCH_RUNNING       0xB1            /* running batch */

typedef struct lstcon_tsb_hdr {
        lst_bid_t               tsb_id;         /* batch ID */
        int                     tsb_index;      /* test index */
} lstcon_tsb_hdr_t;

typedef struct {
        lstcon_tsb_hdr_t        bat_hdr;        /* test_batch header */
        cfs_list_t              bat_link;       /* chain on session's batches list */
        int                     bat_ntest;      /* # of test */
        int                     bat_state;      /* state of the batch */
        int                     bat_arg;        /* parameter for run|stop, timeout for run, force for stop */
        char                    bat_name[LST_NAME_SIZE]; /* name of batch */

        cfs_list_t              bat_test_list;  /* list head of tests (lstcon_test_t) */
        cfs_list_t              bat_trans_list; /* list head of transaction */
        cfs_list_t              bat_cli_list;   /* list head of client nodes (lstcon_node_t) */
        cfs_list_t             *bat_cli_hash;   /* hash table of client nodes */ 
        cfs_list_t              bat_srv_list;   /* list head of server nodes */
        cfs_list_t             *bat_srv_hash;   /* hash table of server nodes */
} lstcon_batch_t;                             /*** (tests ) batch descritptor */

typedef struct lstcon_test {
        lstcon_tsb_hdr_t      tes_hdr;        /* test batch header */
        cfs_list_t            tes_link;       /* chain on batch's tests list */
        lstcon_batch_t       *tes_batch;      /* pointer to batch */

        int                   tes_type;       /* type of the test, i.e: bulk, ping */
        int                   tes_stop_onerr; /* stop on error */
        int                   tes_oneside;    /* one-sided test */
        int                   tes_concur;     /* concurrency */
        int                   tes_loop;       /* loop count */
        int                   tes_dist;       /* nodes distribution of target group */
        int                   tes_span;       /* nodes span of target group */
        int                   tes_cliidx;     /* client index, used for RPC creating */

        cfs_list_t  tes_trans_list; /* transaction list */
        lstcon_group_t       *tes_src_grp;    /* group run the test */
        lstcon_group_t       *tes_dst_grp;    /* target group */

        int                   tes_paramlen;   /* test parameter length */
        char                  tes_param[0];   /* test parameter */
} lstcon_test_t;                                /*** a single test descriptor */

#define LST_GLOBAL_HASHSIZE     503             /* global nodes hash table size */
#define LST_NODE_HASHSIZE       239             /* node hash table (for batch or group) */

#define LST_SESSION_NONE        0x0             /* no session */
#define LST_SESSION_ACTIVE      0x1             /* working session */

#define LST_CONSOLE_TIMEOUT     300             /* default console timeout */

typedef struct {
        cfs_mutex_t             ses_mutex;      /* lock for session, only one thread can enter session */
        lst_sid_t               ses_id;         /* global session id */
        int                     ses_key;        /* local session key */
        int                     ses_state;      /* state of session */
        int                     ses_timeout;    /* timeout in seconds */
        time_t                  ses_laststamp;  /* last operation stamp (seconds) */
        int                     ses_force:1;    /* force creating */
        int                     ses_shutdown:1; /* session is shutting down */
        int                     ses_expired:1;  /* console is timedout */
        __u64                   ses_id_cookie;  /* batch id cookie */
        char                    ses_name[LST_NAME_SIZE];  /* session name */
        lstcon_rpc_trans_t     *ses_ping;       /* session pinger */
        stt_timer_t             ses_ping_timer; /* timer for pinger */
        lstcon_trans_stat_t     ses_trans_stat; /* transaction stats */

        cfs_list_t              ses_trans_list; /* global list of transaction */
        cfs_list_t              ses_grp_list;   /* global list of groups */
        cfs_list_t              ses_bat_list;   /* global list of batches */
        cfs_list_t              ses_ndl_list;   /* global list of nodes */
        cfs_list_t             *ses_ndl_hash;   /* hash table of nodes */

        cfs_spinlock_t          ses_rpc_lock;   /* serialize */
        cfs_atomic_t            ses_rpc_counter;/* # of initialized RPCs */
        cfs_list_t              ses_rpc_freelist; /* idle console rpc */
} lstcon_session_t;                             /*** session descriptor */

extern lstcon_session_t         console_session;
static inline lstcon_trans_stat_t *
lstcon_trans_stat(void)
{
        return &console_session.ses_trans_stat;
}

static inline cfs_list_t *
lstcon_id2hash (lnet_process_id_t id, cfs_list_t *hash)
{
        unsigned int idx = LNET_NIDADDR(id.nid) % LST_NODE_HASHSIZE;

        return &hash[idx];
}

extern int lstcon_session_match(lst_sid_t sid);
extern int lstcon_session_new(char *name, int key,
                              int timeout, int flags, lst_sid_t *sid_up);
extern int lstcon_session_info(lst_sid_t *sid_up, int *key,
                               lstcon_ndlist_ent_t *entp, char *name_up, int len);
extern int lstcon_session_end(void);
extern int lstcon_session_debug(int timeout, cfs_list_t *result_up);
extern int lstcon_batch_debug(int timeout, char *name, 
                              int client, cfs_list_t *result_up);
extern int lstcon_group_debug(int timeout, char *name,
                              cfs_list_t *result_up);
extern int lstcon_nodes_debug(int timeout, int nnd, lnet_process_id_t *nds_up,
                              cfs_list_t *result_up);
extern int lstcon_group_add(char *name);
extern int lstcon_group_del(char *name);
extern int lstcon_group_clean(char *name, int args);
extern int lstcon_group_refresh(char *name, cfs_list_t *result_up);
extern int lstcon_nodes_add(char *name, int nnd, lnet_process_id_t *nds_up,
                            cfs_list_t *result_up);
extern int lstcon_nodes_remove(char *name, int nnd, lnet_process_id_t *nds_up,
                               cfs_list_t *result_up);
extern int lstcon_group_info(char *name, lstcon_ndlist_ent_t *gent_up, 
                             int *index_p, int *ndent_p, lstcon_node_ent_t *ndents_up);
extern int lstcon_group_list(int idx, int len, char *name_up);
extern int lstcon_batch_add(char *name);
extern int lstcon_batch_run(char *name, int timeout,
                            cfs_list_t *result_up);
extern int lstcon_batch_stop(char *name, int force,
                             cfs_list_t *result_up);
extern int lstcon_test_batch_query(char *name, int testidx,
                                   int client, int timeout,
                                   cfs_list_t *result_up);
extern int lstcon_batch_del(char *name);
extern int lstcon_batch_list(int idx, int namelen, char *name_up);
extern int lstcon_batch_info(char *name, lstcon_test_batch_ent_t *ent_up,
                             int server, int testidx, int *index_p,
                             int *ndent_p, lstcon_node_ent_t *dents_up);
extern int lstcon_group_stat(char *grp_name, int timeout,
                             cfs_list_t *result_up);
extern int lstcon_nodes_stat(int count, lnet_process_id_t *ids_up,
                             int timeout, cfs_list_t *result_up);
extern int lstcon_test_add(char *name, int type, int loop, int concur,
                           int dist, int span, char *src_name, char * dst_name,
                           void *param, int paramlen, int *retp,
                           cfs_list_t *result_up);
#endif

#endif
