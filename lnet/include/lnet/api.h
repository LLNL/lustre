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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2016, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#ifndef __LNET_API_H__
#define __LNET_API_H__

/** \defgroup lnet LNet
 *
 * The Lustre Networking subsystem.
 *
 * LNet is an asynchronous message-passing API, which provides an unreliable
 * connectionless service that can't guarantee any order. It supports OFA IB,
 * TCP/IP, and Cray Portals, and routes between heterogeneous networks.
 * @{
 */

#ifndef __KERNEL__
# error This include is only for kernel use.
#endif

#include <uapi/linux/lnet/lnet-types.h>

/** \defgroup lnet_init_fini Initialization and cleanup
 * The LNet must be properly initialized before any LNet calls can be made.
 * @{ */
int LNetNIInit(lnet_pid_t requested_pid);
int LNetNIFini(void);
/** @} lnet_init_fini */

/** \defgroup lnet_addr LNet addressing and basic types
 *
 * Addressing scheme and basic data types of LNet.
 *
 * The LNet API is memory-oriented, so LNet must be able to address not only
 * end-points but also memory region within a process address space.
 * An ::lnet_nid_t addresses an end-point. An ::lnet_pid_t identifies a process
 * in a node. A portal represents an opening in the address space of a
 * process. Match bits is criteria to identify a region of memory inside a
 * portal, and offset specifies an offset within the memory region.
 *
 * LNet creates a table of portals for each process during initialization.
 * This table has MAX_PORTALS entries and its size can't be dynamically
 * changed. A portal stays empty until the owning process starts to add
 * memory regions to it. A portal is sometimes called an index because
 * it's an entry in the portals table of a process.
 *
 * \see LNetMEAttach
 * @{ */
int LNetGetId(unsigned int index, struct lnet_processid *id);
int LNetDist(lnet_nid_t nid, lnet_nid_t *srcnid, __u32 *order);
lnet_nid_t LNetPrimaryNID(lnet_nid_t nid);
bool LNetIsPeerLocal(lnet_nid_t nid);

/** @} lnet_addr */


/** \defgroup lnet_me Match entries
 *
 * A match entry (abbreviated as ME) describes a set of criteria to accept
 * incoming requests.
 *
 * A portal is essentially a match list plus a set of attributes. A match
 * list is a chain of MEs. Each ME includes a pointer to a memory descriptor
 * and a set of match criteria. The match criteria can be used to reject
 * incoming requests based on process ID or the match bits provided in the
 * request. MEs can be dynamically inserted into a match list by LNetMEAttach(),
 * and must then be attached to an MD with LNetMDAttach().
 * @{ */
struct lnet_me *
LNetMEAttach(unsigned int portal,
	     struct lnet_processid *match_id_in,
	     __u64 match_bits_in,
	     __u64 ignore_bits_in,
	     enum lnet_unlink unlink_in,
	     enum lnet_ins_pos pos_in);
/** @} lnet_me */

/** \defgroup lnet_md Memory descriptors
 *
 * A memory descriptor contains information about a region of a user's
 * memory (either in kernel or user space) and optionally points to an
 * event queue where information about the operations performed on the
 * memory descriptor are recorded. Memory descriptor is abbreviated as
 * MD and can be used interchangeably with the memory region it describes.
 *
 * The LNet API provides two operations to create MDs: LNetMDAttach()
 * and LNetMDBind(); one operation to unlink and release the resources
 * associated with a MD: LNetMDUnlink().
 * @{ */
int LNetMDAttach(struct lnet_me *current_in,
		 const struct lnet_md *md_in,
		 enum lnet_unlink unlink_in,
		 struct lnet_handle_md *md_handle_out);

int LNetMDBind(const struct lnet_md *md_in,
	       enum lnet_unlink unlink_in,
	       struct lnet_handle_md *md_handle_out);

int LNetMDUnlink(struct lnet_handle_md md_in);

void lnet_assert_handler_unused(lnet_handler_t handler);
/** @} lnet_md */

/** \defgroup lnet_data Data movement operations
 *
 * The LNet API provides two data movement operations: LNetPut()
 * and LNetGet().
 * @{ */
int LNetPut(lnet_nid_t	      self,
	    struct lnet_handle_md md_in,
	    enum lnet_ack_req	ack_req_in,
	    struct lnet_process_id target_in,
	    unsigned int      portal_in,
	    __u64	      match_bits_in,
	    unsigned int      offset_in,
	    __u64	      hdr_data_in);

int LNetGet(lnet_nid_t	      self,
	    struct lnet_handle_md md_in,
	    struct lnet_process_id target_in,
	    unsigned int      portal_in,
	    __u64	      match_bits_in,
	    unsigned int      offset_in,
	    bool	      recovery);
/** @} lnet_data */


/** \defgroup lnet_misc Miscellaneous operations.
 * Miscellaneous operations.
 * @{ */

int LNetSetLazyPortal(int portal);
int LNetClearLazyPortal(int portal);
int LNetCtl(unsigned int cmd, void *arg);
void LNetDebugPeer(struct lnet_processid *id);
int LNetGetPeerDiscoveryStatus(void);
int LNetAddPeer(lnet_nid_t *nids, __u32 num_nids);

/** @} lnet_misc */

/** @} lnet */
#endif
