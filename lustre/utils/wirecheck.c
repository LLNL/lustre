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
 * Copyright (c) 2011, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <liblustre.h>
#include <lustre_lib.h>
#include <lustre/lustre_idl.h>
#include <lustre_disk.h>

#define BLANK_LINE()						\
do {								\
	printf("\n");						\
} while(0)

#define COMMENT(c)						\
do {								\
	printf("	/* "c" */\n");				\
} while(0)

#define STRINGIFY(a) #a

#define CHECK_CDEFINE(a)					\
	printf("	CLASSERT("#a" == "STRINGIFY(a) ");\n")

#define CHECK_CVALUE(a)					 \
	printf("	CLASSERT("#a" == %lld);\n", (long long)a)

#define CHECK_CVALUE_X(a)					\
	printf("	CLASSERT("#a" == 0x%.8x);\n", a)

#define CHECK_DEFINE(a)						\
do {								\
	printf("	LASSERTF("#a" == "STRINGIFY(a)		\
		", \"found %%lld\\n\",\n			"\
		"(long long)"#a");\n");				\
} while(0)

#define CHECK_DEFINE_X(a)					\
do {								\
	printf("	LASSERTF("#a" == "STRINGIFY(a)		\
		", \"found 0x%%.8x\\n\",\n		"#a	\
		");\n");					\
} while(0)

#define CHECK_DEFINE_64X(a)					\
do {								\
	printf("	LASSERTF("#a" == "STRINGIFY(a)		\
		", \"found 0x%%.16llxULL\\n\",\n		"\
		" "#a");\n");					\
} while(0)

#define CHECK_VALUE(a)						\
do {								\
	printf("	LASSERTF("#a				\
		" == %lld, \"found %%lld\\n\",\n		"\
		" (long long)"#a");\n", (long long)a);		\
} while(0)

#define CHECK_VALUE_X(a)					\
do {								\
	printf("	LASSERTF("#a				\
		" == 0x%.8xUL, \"found 0x%%.8xUL\\n\",\n	"\
		"	(unsigned)"#a");\n", (unsigned)a);	\
} while(0)

#define CHECK_VALUE_O(a)					\
do {								\
	printf("	LASSERTF("#a				\
		" == 0%.11oUL, \"found 0%%.11oUL\\n\",\n	"\
		"	"#a");\n", a);				\
} while(0)

#define CHECK_VALUE_64X(a)					\
do {								\
	printf("	LASSERTF("#a" == 0x%.16llxULL, "	\
		"\"found 0x%%.16llxULL\\n\",\n			"\
		"(long long)"#a");\n", (long long)a);		\
} while(0)

#define CHECK_VALUE_64O(a)					\
do {								\
	printf("	LASSERTF("#a" == 0%.22lloULL, "		\
		"\"found 0%%.22lloULL\\n\",\n			"\
		"(long long)"#a");\n", (long long)a);		\
} while(0)

#define CHECK_MEMBER_OFFSET(s, m)				\
do {								\
	CHECK_VALUE((int)offsetof(struct s, m));		\
} while(0)

#define CHECK_MEMBER_OFFSET_TYPEDEF(s, m)			\
do {								\
	CHECK_VALUE((int)offsetof(s, m));			\
} while(0)

#define CHECK_MEMBER_SIZEOF(s, m)				\
do {								\
	CHECK_VALUE((int)sizeof(((struct s *)0)->m));		\
} while(0)

#define CHECK_MEMBER_SIZEOF_TYPEDEF(s, m)			\
do {								\
	CHECK_VALUE((int)sizeof(((s *)0)->m));			\
} while(0)

#define CHECK_MEMBER(s, m)					\
do {								\
	CHECK_MEMBER_OFFSET(s, m);				\
	CHECK_MEMBER_SIZEOF(s, m);				\
} while(0)

#define CHECK_MEMBER_TYPEDEF(s, m)				\
do {								\
	CHECK_MEMBER_OFFSET_TYPEDEF(s, m);			\
	CHECK_MEMBER_SIZEOF_TYPEDEF(s, m);			\
} while(0)

#define CHECK_STRUCT(s)						\
do {								\
	COMMENT("Checks for struct "#s);			\
		CHECK_VALUE((int)sizeof(struct s));		\
} while(0)

#define CHECK_STRUCT_TYPEDEF(s)					\
do {								\
	COMMENT("Checks for type "#s);				\
		CHECK_VALUE((int)sizeof(s));			\
} while(0)

#define CHECK_UNION(s)						\
do {								\
	COMMENT("Checks for union "#s);				\
	CHECK_VALUE((int)sizeof(union s));			\
} while(0)

#define CHECK_VALUE_SAME(v1, v2)				\
do {								\
	printf("\tLASSERTF("#v1" == "#v2", "			\
		"\"%%d != %%d\\n\",\n"				\
		"\t\t "#v1", "#v2");\n");			\
} while (0)

#define CHECK_MEMBER_SAME(s1, s2, m)				\
do {								\
	CHECK_VALUE_SAME((int)offsetof(struct s1, m),		\
			 (int)offsetof(struct s2, m));		\
	CHECK_VALUE_SAME((int)sizeof(((struct s1 *)0)->m),	\
			 (int)sizeof(((struct s2 *)0)->m));	\
} while (0)

static void
check_lu_seq_range(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lu_seq_range);
	CHECK_MEMBER(lu_seq_range, lsr_start);
	CHECK_MEMBER(lu_seq_range, lsr_end);
	CHECK_MEMBER(lu_seq_range, lsr_index);
	CHECK_MEMBER(lu_seq_range, lsr_flags);

	CHECK_VALUE(LU_SEQ_RANGE_MDT);
	CHECK_VALUE(LU_SEQ_RANGE_OST);
}

static void
check_lustre_mdt_attrs(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lustre_mdt_attrs);
	CHECK_MEMBER(lustre_mdt_attrs, lma_compat);
	CHECK_MEMBER(lustre_mdt_attrs, lma_incompat);
	CHECK_MEMBER(lustre_mdt_attrs, lma_self_fid);
	CHECK_MEMBER(lustre_mdt_attrs, lma_flags);
	CHECK_VALUE_X(LMAI_RELEASED);

	CHECK_VALUE_X(LMAC_HSM);
	CHECK_VALUE_X(LMAC_SOM);
}

static void
check_som_attrs(void)
{
	BLANK_LINE();
	CHECK_STRUCT(som_attrs);
	CHECK_MEMBER(som_attrs, som_compat);
	CHECK_MEMBER(som_attrs, som_incompat);
	CHECK_MEMBER(som_attrs, som_ioepoch);
	CHECK_MEMBER(som_attrs, som_size);
	CHECK_MEMBER(som_attrs, som_blocks);
	CHECK_MEMBER(som_attrs, som_mountid);
}

static void
check_hsm_attrs(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_attrs);
	CHECK_MEMBER(hsm_attrs, hsm_compat);
	CHECK_MEMBER(hsm_attrs, hsm_flags);
	CHECK_MEMBER(hsm_attrs, hsm_arch_id);
	CHECK_MEMBER(hsm_attrs, hsm_arch_ver);
}

static void
check_ost_id(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ost_id);
	CHECK_MEMBER(ost_id, oi_id);
	CHECK_MEMBER(ost_id, oi_seq);

	CHECK_VALUE(LUSTRE_FID_INIT_OID);

	CHECK_VALUE(FID_SEQ_OST_MDT0);
	CHECK_VALUE(FID_SEQ_LLOG);
	CHECK_VALUE(FID_SEQ_ECHO);
	CHECK_VALUE(FID_SEQ_OST_MDT1);
	CHECK_VALUE(FID_SEQ_OST_MAX);
	CHECK_VALUE(FID_SEQ_RSVD);
	CHECK_VALUE(FID_SEQ_IGIF);
	CHECK_VALUE_64X(FID_SEQ_IGIF_MAX);
	CHECK_VALUE_64X(FID_SEQ_IDIF);
	CHECK_VALUE_64X(FID_SEQ_IDIF_MAX);
	CHECK_VALUE_64X(FID_SEQ_START);
	CHECK_VALUE_64X(FID_SEQ_LOCAL_FILE);
	CHECK_VALUE_64X(FID_SEQ_DOT_LUSTRE);
	CHECK_VALUE_64X(FID_SEQ_SPECIAL);
	CHECK_VALUE_64X(FID_SEQ_QUOTA);
	CHECK_VALUE_64X(FID_SEQ_QUOTA_GLB);
	CHECK_VALUE_64X(FID_SEQ_NORMAL);
	CHECK_VALUE_64X(FID_SEQ_LOV_DEFAULT);

	CHECK_VALUE_X(FID_OID_SPECIAL_BFL);
	CHECK_VALUE_X(FID_OID_DOT_LUSTRE);
	CHECK_VALUE_X(FID_OID_DOT_LUSTRE_OBF);
}

static void
check_lu_dirent(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lu_dirent);
	CHECK_MEMBER(lu_dirent, lde_fid);
	CHECK_MEMBER(lu_dirent, lde_hash);
	CHECK_MEMBER(lu_dirent, lde_reclen);
	CHECK_MEMBER(lu_dirent, lde_namelen);
	CHECK_MEMBER(lu_dirent, lde_attrs);
	CHECK_MEMBER(lu_dirent, lde_name[0]);

	CHECK_VALUE_X(LUDA_FID);
	CHECK_VALUE_X(LUDA_TYPE);
	CHECK_VALUE_X(LUDA_64BITHASH);
}

static void
check_luda_type(void)
{
	BLANK_LINE();
	CHECK_STRUCT(luda_type);
	CHECK_MEMBER(luda_type, lt_type);
}

static void
check_lu_dirpage(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lu_dirpage);
	CHECK_MEMBER(lu_dirpage, ldp_hash_start);
	CHECK_MEMBER(lu_dirpage, ldp_hash_end);
	CHECK_MEMBER(lu_dirpage, ldp_flags);
	CHECK_MEMBER(lu_dirpage, ldp_pad0);
	CHECK_MEMBER(lu_dirpage, ldp_entries[0]);

	CHECK_VALUE(LDF_EMPTY);
	CHECK_VALUE(LDF_COLLIDE);
	CHECK_VALUE(LU_PAGE_SIZE);
	CHECK_UNION(lu_page);
}

static void
check_lustre_handle(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lustre_handle);
	CHECK_MEMBER(lustre_handle, cookie);
}

static void
check_lustre_msg_v2(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lustre_msg_v2);
	CHECK_MEMBER(lustre_msg_v2, lm_bufcount);
	CHECK_MEMBER(lustre_msg_v2, lm_secflvr);
	CHECK_MEMBER(lustre_msg_v2, lm_magic);
	CHECK_MEMBER(lustre_msg_v2, lm_repsize);
	CHECK_MEMBER(lustre_msg_v2, lm_cksum);
	CHECK_MEMBER(lustre_msg_v2, lm_flags);
	CHECK_MEMBER(lustre_msg_v2, lm_padding_2);
	CHECK_MEMBER(lustre_msg_v2, lm_padding_3);
	CHECK_MEMBER(lustre_msg_v2, lm_buflens[0]);

	CHECK_DEFINE_X(LUSTRE_MSG_MAGIC_V1);
	CHECK_DEFINE_X(LUSTRE_MSG_MAGIC_V2);
	CHECK_DEFINE_X(LUSTRE_MSG_MAGIC_V1_SWABBED);
	CHECK_DEFINE_X(LUSTRE_MSG_MAGIC_V2_SWABBED);
}

static void
check_ptlrpc_body(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ptlrpc_body);
	CHECK_MEMBER(ptlrpc_body, pb_handle);
	CHECK_MEMBER(ptlrpc_body, pb_type);
	CHECK_MEMBER(ptlrpc_body, pb_version);
	CHECK_MEMBER(ptlrpc_body, pb_opc);
	CHECK_MEMBER(ptlrpc_body, pb_status);
	CHECK_MEMBER(ptlrpc_body, pb_last_xid);
	CHECK_MEMBER(ptlrpc_body, pb_last_seen);
	CHECK_MEMBER(ptlrpc_body, pb_last_committed);
	CHECK_MEMBER(ptlrpc_body, pb_transno);
	CHECK_MEMBER(ptlrpc_body, pb_flags);
	CHECK_MEMBER(ptlrpc_body, pb_op_flags);
	CHECK_MEMBER(ptlrpc_body, pb_conn_cnt);
	CHECK_MEMBER(ptlrpc_body, pb_timeout);
	CHECK_MEMBER(ptlrpc_body, pb_service_time);
	CHECK_MEMBER(ptlrpc_body, pb_limit);
	CHECK_MEMBER(ptlrpc_body, pb_slv);
	CHECK_CVALUE(PTLRPC_NUM_VERSIONS);
	CHECK_MEMBER(ptlrpc_body, pb_pre_versions);
	CHECK_MEMBER(ptlrpc_body, pb_padding);
	CHECK_CVALUE(JOBSTATS_JOBID_SIZE);
	CHECK_MEMBER(ptlrpc_body, pb_jobid);

	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_handle);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_type);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_version);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_opc);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_status);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_last_xid);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_last_seen);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_last_committed);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_transno);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_flags);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_op_flags);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_conn_cnt);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_timeout);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_service_time);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_limit);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_slv);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_pre_versions);
	CHECK_MEMBER_SAME(ptlrpc_body_v3, ptlrpc_body_v2, pb_padding);

	CHECK_VALUE(MSG_PTLRPC_BODY_OFF);
	CHECK_VALUE(REQ_REC_OFF);
	CHECK_VALUE(REPLY_REC_OFF);
	CHECK_VALUE(DLM_LOCKREQ_OFF);
	CHECK_VALUE(DLM_REQ_REC_OFF);
	CHECK_VALUE(DLM_INTENT_IT_OFF);
	CHECK_VALUE(DLM_INTENT_REC_OFF);
	CHECK_VALUE(DLM_LOCKREPLY_OFF);
	CHECK_VALUE(DLM_REPLY_REC_OFF);
	CHECK_VALUE(MSG_PTLRPC_HEADER_OFF);

	CHECK_DEFINE_X(PTLRPC_MSG_VERSION);
	CHECK_DEFINE_X(LUSTRE_VERSION_MASK);
	CHECK_DEFINE_X(LUSTRE_OBD_VERSION);
	CHECK_DEFINE_X(LUSTRE_MDS_VERSION);
	CHECK_DEFINE_X(LUSTRE_OST_VERSION);
	CHECK_DEFINE_X(LUSTRE_DLM_VERSION);
	CHECK_DEFINE_X(LUSTRE_LOG_VERSION);
	CHECK_DEFINE_X(LUSTRE_MGS_VERSION);

	CHECK_VALUE(MSGHDR_AT_SUPPORT);
	CHECK_VALUE(MSGHDR_CKSUM_INCOMPAT18);

	CHECK_VALUE_X(MSG_OP_FLAG_MASK);
	CHECK_VALUE(MSG_OP_FLAG_SHIFT);
	CHECK_VALUE_X(MSG_GEN_FLAG_MASK);

	CHECK_VALUE_X(MSG_LAST_REPLAY);
	CHECK_VALUE_X(MSG_RESENT);
	CHECK_VALUE_X(MSG_REPLAY);
	CHECK_VALUE_X(MSG_DELAY_REPLAY);
	CHECK_VALUE_X(MSG_VERSION_REPLAY);
	CHECK_VALUE_X(MSG_REQ_REPLAY_DONE);
	CHECK_VALUE_X(MSG_LOCK_REPLAY_DONE);

	CHECK_VALUE_X(MSG_CONNECT_RECOVERING);
	CHECK_VALUE_X(MSG_CONNECT_RECONNECT);
	CHECK_VALUE_X(MSG_CONNECT_REPLAYABLE);
	CHECK_VALUE_X(MSG_CONNECT_LIBCLIENT);
	CHECK_VALUE_X(MSG_CONNECT_INITIAL);
	CHECK_VALUE_X(MSG_CONNECT_ASYNC);
	CHECK_VALUE_X(MSG_CONNECT_NEXT_VER);
	CHECK_VALUE_X(MSG_CONNECT_TRANSNO);
}

static void
check_obd_connect_data(void)
{
	BLANK_LINE();
	CHECK_STRUCT(obd_connect_data);
	CHECK_MEMBER(obd_connect_data, ocd_connect_flags);
	CHECK_MEMBER(obd_connect_data, ocd_version);
	CHECK_MEMBER(obd_connect_data, ocd_grant);
	CHECK_MEMBER(obd_connect_data, ocd_index);
	CHECK_MEMBER(obd_connect_data, ocd_brw_size);
	CHECK_MEMBER(obd_connect_data, ocd_ibits_known);
	CHECK_MEMBER(obd_connect_data, ocd_blocksize);
	CHECK_MEMBER(obd_connect_data, ocd_inodespace);
	CHECK_MEMBER(obd_connect_data, ocd_grant_extent);
	CHECK_MEMBER(obd_connect_data, ocd_unused);
	CHECK_MEMBER(obd_connect_data, ocd_transno);
	CHECK_MEMBER(obd_connect_data, ocd_group);
	CHECK_MEMBER(obd_connect_data, ocd_cksum_types);
	CHECK_MEMBER(obd_connect_data, ocd_max_easize);
	CHECK_MEMBER(obd_connect_data, ocd_instance);
	CHECK_MEMBER(obd_connect_data, ocd_maxbytes);
	CHECK_MEMBER(obd_connect_data, padding1);
	CHECK_MEMBER(obd_connect_data, padding2);
	CHECK_MEMBER(obd_connect_data, padding3);
	CHECK_MEMBER(obd_connect_data, padding4);
	CHECK_MEMBER(obd_connect_data, padding5);
	CHECK_MEMBER(obd_connect_data, padding6);
	CHECK_MEMBER(obd_connect_data, padding7);
	CHECK_MEMBER(obd_connect_data, padding8);
	CHECK_MEMBER(obd_connect_data, padding9);
	CHECK_MEMBER(obd_connect_data, paddingA);
	CHECK_MEMBER(obd_connect_data, paddingB);
	CHECK_MEMBER(obd_connect_data, paddingC);
	CHECK_MEMBER(obd_connect_data, paddingD);
	CHECK_MEMBER(obd_connect_data, paddingE);
	CHECK_MEMBER(obd_connect_data, paddingF);

	CHECK_DEFINE_64X(OBD_CONNECT_RDONLY);
	CHECK_DEFINE_64X(OBD_CONNECT_INDEX);
	CHECK_DEFINE_64X(OBD_CONNECT_MDS);
	CHECK_DEFINE_64X(OBD_CONNECT_GRANT);
	CHECK_DEFINE_64X(OBD_CONNECT_SRVLOCK);
	CHECK_DEFINE_64X(OBD_CONNECT_VERSION);
	CHECK_DEFINE_64X(OBD_CONNECT_REQPORTAL);
	CHECK_DEFINE_64X(OBD_CONNECT_ACL);
	CHECK_DEFINE_64X(OBD_CONNECT_XATTR);
	CHECK_DEFINE_64X(OBD_CONNECT_CROW);
	CHECK_DEFINE_64X(OBD_CONNECT_TRUNCLOCK);
	CHECK_DEFINE_64X(OBD_CONNECT_TRANSNO);
	CHECK_DEFINE_64X(OBD_CONNECT_IBITS);
	CHECK_DEFINE_64X(OBD_CONNECT_JOIN);
	CHECK_DEFINE_64X(OBD_CONNECT_ATTRFID);
	CHECK_DEFINE_64X(OBD_CONNECT_NODEVOH);
	CHECK_DEFINE_64X(OBD_CONNECT_RMT_CLIENT);
	CHECK_DEFINE_64X(OBD_CONNECT_RMT_CLIENT_FORCE);
	CHECK_DEFINE_64X(OBD_CONNECT_BRW_SIZE);
	CHECK_DEFINE_64X(OBD_CONNECT_QUOTA64);
	CHECK_DEFINE_64X(OBD_CONNECT_MDS_CAPA);
	CHECK_DEFINE_64X(OBD_CONNECT_OSS_CAPA);
	CHECK_DEFINE_64X(OBD_CONNECT_CANCELSET);
	CHECK_DEFINE_64X(OBD_CONNECT_SOM);
	CHECK_DEFINE_64X(OBD_CONNECT_AT);
	CHECK_DEFINE_64X(OBD_CONNECT_LRU_RESIZE);
	CHECK_DEFINE_64X(OBD_CONNECT_MDS_MDS);
	CHECK_DEFINE_64X(OBD_CONNECT_REAL);
	CHECK_DEFINE_64X(OBD_CONNECT_CHANGE_QS);
	CHECK_DEFINE_64X(OBD_CONNECT_CKSUM);
	CHECK_DEFINE_64X(OBD_CONNECT_FID);
	CHECK_DEFINE_64X(OBD_CONNECT_VBR);
	CHECK_DEFINE_64X(OBD_CONNECT_LOV_V3);
	CHECK_DEFINE_64X(OBD_CONNECT_GRANT_SHRINK);
	CHECK_DEFINE_64X(OBD_CONNECT_SKIP_ORPHAN);
	CHECK_DEFINE_64X(OBD_CONNECT_MAX_EASIZE);
	CHECK_DEFINE_64X(OBD_CONNECT_FULL20);
	CHECK_DEFINE_64X(OBD_CONNECT_LAYOUTLOCK);
	CHECK_DEFINE_64X(OBD_CONNECT_64BITHASH);
	CHECK_DEFINE_64X(OBD_CONNECT_MAXBYTES);
	CHECK_DEFINE_64X(OBD_CONNECT_IMP_RECOV);
	CHECK_DEFINE_64X(OBD_CONNECT_JOBSTATS);
	CHECK_DEFINE_64X(OBD_CONNECT_UMASK);
	CHECK_DEFINE_64X(OBD_CONNECT_EINPROGRESS);
	CHECK_DEFINE_64X(OBD_CONNECT_GRANT_PARAM);
	CHECK_DEFINE_64X(OBD_CONNECT_FLOCK_OWNER);
	CHECK_DEFINE_64X(OBD_CONNECT_LVB_TYPE);
	CHECK_DEFINE_64X(OBD_CONNECT_NANOSEC_TIME);
	CHECK_DEFINE_64X(OBD_CONNECT_LIGHTWEIGHT);
	CHECK_DEFINE_64X(OBD_CONNECT_SHORTIO);
	CHECK_DEFINE_64X(OBD_CONNECT_PINGLESS);

	CHECK_VALUE_X(OBD_CKSUM_CRC32);
	CHECK_VALUE_X(OBD_CKSUM_ADLER);
	CHECK_VALUE_X(OBD_CKSUM_CRC32C);
}

static void
check_obdo(void)
{
	BLANK_LINE();
	CHECK_STRUCT(obdo);
	CHECK_MEMBER(obdo, o_valid);
	CHECK_MEMBER(obdo, o_id);
	CHECK_MEMBER(obdo, o_seq);
	CHECK_MEMBER(obdo, o_parent_seq);
	CHECK_MEMBER(obdo, o_size);
	CHECK_MEMBER(obdo, o_mtime);
	CHECK_MEMBER(obdo, o_atime);
	CHECK_MEMBER(obdo, o_ctime);
	CHECK_MEMBER(obdo, o_blocks);
	CHECK_MEMBER(obdo, o_grant);
	CHECK_MEMBER(obdo, o_blksize);
	CHECK_MEMBER(obdo, o_mode);
	CHECK_MEMBER(obdo, o_uid);
	CHECK_MEMBER(obdo, o_gid);
	CHECK_MEMBER(obdo, o_flags);
	CHECK_MEMBER(obdo, o_nlink);
	CHECK_MEMBER(obdo, o_parent_oid);
	CHECK_MEMBER(obdo, o_misc);
	CHECK_MEMBER(obdo, o_ioepoch);
	CHECK_MEMBER(obdo, o_stripe_idx);
	CHECK_MEMBER(obdo, o_parent_ver);
	CHECK_MEMBER(obdo, o_handle);
	CHECK_MEMBER(obdo, o_lcookie);
	CHECK_MEMBER(obdo, o_uid_h);
	CHECK_MEMBER(obdo, o_gid_h);
	CHECK_MEMBER(obdo, o_data_version);
	CHECK_MEMBER(obdo, o_padding_4);
	CHECK_MEMBER(obdo, o_padding_5);
	CHECK_MEMBER(obdo, o_padding_6);

	CHECK_DEFINE_64X(OBD_MD_FLID);
	CHECK_DEFINE_64X(OBD_MD_FLATIME);
	CHECK_DEFINE_64X(OBD_MD_FLMTIME);
	CHECK_DEFINE_64X(OBD_MD_FLCTIME);
	CHECK_DEFINE_64X(OBD_MD_FLSIZE);
	CHECK_DEFINE_64X(OBD_MD_FLBLOCKS);
	CHECK_DEFINE_64X(OBD_MD_FLBLKSZ);
	CHECK_DEFINE_64X(OBD_MD_FLMODE);
	CHECK_DEFINE_64X(OBD_MD_FLTYPE);
	CHECK_DEFINE_64X(OBD_MD_FLUID);
	CHECK_DEFINE_64X(OBD_MD_FLGID);
	CHECK_DEFINE_64X(OBD_MD_FLFLAGS);
	CHECK_DEFINE_64X(OBD_MD_FLNLINK);
	CHECK_DEFINE_64X(OBD_MD_FLGENER);
	CHECK_DEFINE_64X(OBD_MD_FLRDEV);
	CHECK_DEFINE_64X(OBD_MD_FLEASIZE);
	CHECK_DEFINE_64X(OBD_MD_LINKNAME);
	CHECK_DEFINE_64X(OBD_MD_FLHANDLE);
	CHECK_DEFINE_64X(OBD_MD_FLCKSUM);
	CHECK_DEFINE_64X(OBD_MD_FLQOS);
	CHECK_DEFINE_64X(OBD_MD_FLCOOKIE);
	CHECK_DEFINE_64X(OBD_MD_FLGROUP);
	CHECK_DEFINE_64X(OBD_MD_FLFID);
	CHECK_DEFINE_64X(OBD_MD_FLEPOCH);
	CHECK_DEFINE_64X(OBD_MD_FLGRANT);
	CHECK_DEFINE_64X(OBD_MD_FLDIREA);
	CHECK_DEFINE_64X(OBD_MD_FLUSRQUOTA);
	CHECK_DEFINE_64X(OBD_MD_FLGRPQUOTA);
	CHECK_DEFINE_64X(OBD_MD_FLMODEASIZE);
	CHECK_DEFINE_64X(OBD_MD_MDS);
	CHECK_DEFINE_64X(OBD_MD_REINT);
	CHECK_DEFINE_64X(OBD_MD_MEA);
	CHECK_DEFINE_64X(OBD_MD_FLXATTR);
	CHECK_DEFINE_64X(OBD_MD_FLXATTRLS);
	CHECK_DEFINE_64X(OBD_MD_FLXATTRRM);
	CHECK_DEFINE_64X(OBD_MD_FLACL);
	CHECK_DEFINE_64X(OBD_MD_FLRMTPERM);
	CHECK_DEFINE_64X(OBD_MD_FLMDSCAPA);
	CHECK_DEFINE_64X(OBD_MD_FLOSSCAPA);
	CHECK_DEFINE_64X(OBD_MD_FLCKSPLIT);
	CHECK_DEFINE_64X(OBD_MD_FLCROSSREF);
	CHECK_DEFINE_64X(OBD_MD_FLGETATTRLOCK);
	CHECK_DEFINE_64X(OBD_MD_FLRMTLSETFACL);
	CHECK_DEFINE_64X(OBD_MD_FLRMTLGETFACL);
	CHECK_DEFINE_64X(OBD_MD_FLRMTRSETFACL);
	CHECK_DEFINE_64X(OBD_MD_FLRMTRGETFACL);
	CHECK_DEFINE_64X(OBD_MD_FLDATAVERSION);

	CHECK_CVALUE_X(OBD_FL_INLINEDATA);
	CHECK_CVALUE_X(OBD_FL_OBDMDEXISTS);
	CHECK_CVALUE_X(OBD_FL_DELORPHAN);
	CHECK_CVALUE_X(OBD_FL_NORPC);
	CHECK_CVALUE_X(OBD_FL_IDONLY);
	CHECK_CVALUE_X(OBD_FL_RECREATE_OBJS);
	CHECK_CVALUE_X(OBD_FL_DEBUG_CHECK);
	CHECK_CVALUE_X(OBD_FL_NO_USRQUOTA);
	CHECK_CVALUE_X(OBD_FL_NO_GRPQUOTA);
	CHECK_CVALUE_X(OBD_FL_CREATE_CROW);
	CHECK_CVALUE_X(OBD_FL_SRVLOCK);
	CHECK_CVALUE_X(OBD_FL_CKSUM_CRC32);
	CHECK_CVALUE_X(OBD_FL_CKSUM_ADLER);
	CHECK_CVALUE_X(OBD_FL_CKSUM_CRC32C);
	CHECK_CVALUE_X(OBD_FL_CKSUM_RSVD2);
	CHECK_CVALUE_X(OBD_FL_CKSUM_RSVD3);
	CHECK_CVALUE_X(OBD_FL_SHRINK_GRANT);
	CHECK_CVALUE_X(OBD_FL_MMAP);
	CHECK_CVALUE_X(OBD_FL_RECOV_RESEND);
	CHECK_CVALUE_X(OBD_FL_NOSPC_BLK);
	CHECK_CVALUE_X(OBD_FL_LOCAL_MASK);
}

static void
check_lov_ost_data_v1(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lov_ost_data_v1);
	CHECK_MEMBER(lov_ost_data_v1, l_object_id);
	CHECK_MEMBER(lov_ost_data_v1, l_object_seq);
	CHECK_MEMBER(lov_ost_data_v1, l_ost_gen);
	CHECK_MEMBER(lov_ost_data_v1, l_ost_idx);
}

static void
check_lov_mds_md_v1(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lov_mds_md_v1);
	CHECK_MEMBER(lov_mds_md_v1, lmm_magic);
	CHECK_MEMBER(lov_mds_md_v1, lmm_pattern);
	CHECK_MEMBER(lov_mds_md_v1, lmm_object_id);
	CHECK_MEMBER(lov_mds_md_v1, lmm_object_seq);
	CHECK_MEMBER(lov_mds_md_v1, lmm_stripe_size);
	CHECK_MEMBER(lov_mds_md_v1, lmm_stripe_count);
	CHECK_MEMBER(lov_mds_md_v1, lmm_layout_gen);
	CHECK_MEMBER(lov_mds_md_v1, lmm_objects[0]);

	CHECK_CDEFINE(LOV_MAGIC_V1);
}

static void
check_lov_mds_md_v3(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lov_mds_md_v3);
	CHECK_MEMBER(lov_mds_md_v3, lmm_magic);
	CHECK_MEMBER(lov_mds_md_v3, lmm_pattern);
	CHECK_MEMBER(lov_mds_md_v3, lmm_object_id);
	CHECK_MEMBER(lov_mds_md_v3, lmm_object_seq);
	CHECK_MEMBER(lov_mds_md_v3, lmm_stripe_size);
	CHECK_MEMBER(lov_mds_md_v3, lmm_stripe_count);
	CHECK_MEMBER(lov_mds_md_v3, lmm_layout_gen);
	CHECK_CVALUE(LOV_MAXPOOLNAME);
	CHECK_MEMBER(lov_mds_md_v3, lmm_pool_name[LOV_MAXPOOLNAME]);
	CHECK_MEMBER(lov_mds_md_v3, lmm_objects[0]);

	CHECK_CDEFINE(LOV_MAGIC_V3);

	CHECK_VALUE_X(LOV_PATTERN_RAID0);
	CHECK_VALUE_X(LOV_PATTERN_RAID1);
	CHECK_VALUE_X(LOV_PATTERN_FIRST);
	CHECK_VALUE_X(LOV_PATTERN_CMOBD);
}

static void
check_obd_statfs(void)
{
	BLANK_LINE();
	CHECK_STRUCT(obd_statfs);
	CHECK_MEMBER(obd_statfs, os_type);
	CHECK_MEMBER(obd_statfs, os_blocks);
	CHECK_MEMBER(obd_statfs, os_bfree);
	CHECK_MEMBER(obd_statfs, os_bavail);
	CHECK_MEMBER(obd_statfs, os_ffree);
	CHECK_MEMBER(obd_statfs, os_fsid);
	CHECK_MEMBER(obd_statfs, os_bsize);
	CHECK_MEMBER(obd_statfs, os_namelen);
	CHECK_MEMBER(obd_statfs, os_state);
	CHECK_MEMBER(obd_statfs, os_fprecreated);
	CHECK_MEMBER(obd_statfs, os_spare2);
	CHECK_MEMBER(obd_statfs, os_spare3);
	CHECK_MEMBER(obd_statfs, os_spare4);
	CHECK_MEMBER(obd_statfs, os_spare5);
	CHECK_MEMBER(obd_statfs, os_spare6);
	CHECK_MEMBER(obd_statfs, os_spare7);
	CHECK_MEMBER(obd_statfs, os_spare8);
	CHECK_MEMBER(obd_statfs, os_spare9);
}

static void
check_obd_ioobj(void)
{
	BLANK_LINE();
	CHECK_STRUCT(obd_ioobj);
	CHECK_MEMBER(obd_ioobj, ioo_id);
	CHECK_MEMBER(obd_ioobj, ioo_seq);
	CHECK_MEMBER(obd_ioobj, ioo_max_brw);
	CHECK_MEMBER(obd_ioobj, ioo_bufcnt);
}

static void
check_obd_quotactl(void)
{

	BLANK_LINE();
	CHECK_UNION(lquota_id);

	BLANK_LINE();
	CHECK_VALUE(QUOTABLOCK_BITS);
	CHECK_VALUE(QUOTABLOCK_SIZE);

	BLANK_LINE();
	CHECK_STRUCT(obd_quotactl);
	CHECK_MEMBER(obd_quotactl, qc_cmd);
	CHECK_MEMBER(obd_quotactl, qc_type);
	CHECK_MEMBER(obd_quotactl, qc_id);
	CHECK_MEMBER(obd_quotactl, qc_stat);
	CHECK_MEMBER(obd_quotactl, qc_dqinfo);
	CHECK_MEMBER(obd_quotactl, qc_dqblk);

	BLANK_LINE();
	CHECK_STRUCT(obd_dqinfo);
	CHECK_MEMBER(obd_dqinfo, dqi_bgrace);
	CHECK_MEMBER(obd_dqinfo, dqi_igrace);
	CHECK_MEMBER(obd_dqinfo, dqi_flags);
	CHECK_MEMBER(obd_dqinfo, dqi_valid);

	BLANK_LINE();
	CHECK_STRUCT(obd_dqblk);
	CHECK_MEMBER(obd_dqblk, dqb_bhardlimit);
	CHECK_MEMBER(obd_dqblk, dqb_bsoftlimit);
	CHECK_MEMBER(obd_dqblk, dqb_curspace);
	CHECK_MEMBER(obd_dqblk, dqb_ihardlimit);
	CHECK_MEMBER(obd_dqblk, dqb_isoftlimit);
	CHECK_MEMBER(obd_dqblk, dqb_curinodes);
	CHECK_MEMBER(obd_dqblk, dqb_btime);
	CHECK_MEMBER(obd_dqblk, dqb_itime);
	CHECK_MEMBER(obd_dqblk, dqb_valid);
	CHECK_MEMBER(obd_dqblk, dqb_padding);

	CHECK_DEFINE_X(Q_QUOTACHECK);
	CHECK_DEFINE_X(Q_INITQUOTA);
	CHECK_DEFINE_X(Q_GETOINFO);
	CHECK_DEFINE_X(Q_GETOQUOTA);
	CHECK_DEFINE_X(Q_FINVALIDATE);

	BLANK_LINE();
	CHECK_STRUCT(lquota_acct_rec);
	CHECK_MEMBER(lquota_acct_rec, bspace);
	CHECK_MEMBER(lquota_acct_rec, ispace);

	BLANK_LINE();
	CHECK_STRUCT(lquota_glb_rec);
	CHECK_MEMBER(lquota_glb_rec, qbr_hardlimit);
	CHECK_MEMBER(lquota_glb_rec, qbr_softlimit);
	CHECK_MEMBER(lquota_glb_rec, qbr_time);
	CHECK_MEMBER(lquota_glb_rec, qbr_granted);

	BLANK_LINE();
	CHECK_STRUCT(lquota_slv_rec);
	CHECK_MEMBER(lquota_slv_rec, qsr_granted);

}

static void
check_obd_idx_read(void)
{
	BLANK_LINE();
	CHECK_STRUCT(idx_info);
	CHECK_MEMBER(idx_info, ii_magic);
	CHECK_MEMBER(idx_info, ii_flags);
	CHECK_MEMBER(idx_info, ii_count);
	CHECK_MEMBER(idx_info, ii_pad0);
	CHECK_MEMBER(idx_info, ii_attrs);
	CHECK_MEMBER(idx_info, ii_fid);
	CHECK_MEMBER(idx_info, ii_version);
	CHECK_MEMBER(idx_info, ii_hash_start);
	CHECK_MEMBER(idx_info, ii_hash_end);
	CHECK_MEMBER(idx_info, ii_keysize);
	CHECK_MEMBER(idx_info, ii_recsize);
	CHECK_MEMBER(idx_info, ii_pad1);
	CHECK_MEMBER(idx_info, ii_pad2);
	CHECK_MEMBER(idx_info, ii_pad3);
	CHECK_CDEFINE(IDX_INFO_MAGIC);

	BLANK_LINE();
	CHECK_STRUCT(lu_idxpage);
	CHECK_MEMBER(lu_idxpage, lip_magic);
	CHECK_MEMBER(lu_idxpage, lip_flags);
	CHECK_MEMBER(lu_idxpage, lip_nr);
	CHECK_MEMBER(lu_idxpage, lip_pad0);

	CHECK_CDEFINE(LIP_MAGIC);
	CHECK_VALUE(LIP_HDR_SIZE);

	CHECK_VALUE(II_FL_NOHASH);
	CHECK_VALUE(II_FL_VARKEY);
	CHECK_VALUE(II_FL_VARREC);
	CHECK_VALUE(II_FL_NONUNQ);
}

static void
check_niobuf_remote(void)
{
	BLANK_LINE();
	CHECK_STRUCT(niobuf_remote);
	CHECK_MEMBER(niobuf_remote, offset);
	CHECK_MEMBER(niobuf_remote, len);
	CHECK_MEMBER(niobuf_remote, flags);

	CHECK_DEFINE_X(OBD_BRW_READ);
	CHECK_DEFINE_X(OBD_BRW_WRITE);
	CHECK_DEFINE_X(OBD_BRW_SYNC);
	CHECK_DEFINE_X(OBD_BRW_CHECK);
	CHECK_DEFINE_X(OBD_BRW_FROM_GRANT);
	CHECK_DEFINE_X(OBD_BRW_GRANTED);
	CHECK_DEFINE_X(OBD_BRW_NOCACHE);
	CHECK_DEFINE_X(OBD_BRW_NOQUOTA);
	CHECK_DEFINE_X(OBD_BRW_SRVLOCK);
	CHECK_DEFINE_X(OBD_BRW_ASYNC);
	CHECK_DEFINE_X(OBD_BRW_MEMALLOC);
}

static void
check_ost_body(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ost_body);
	CHECK_MEMBER(ost_body, oa);
}

static void
check_ll_fid(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ll_fid);
	CHECK_MEMBER(ll_fid, id);
	CHECK_MEMBER(ll_fid, generation);
	CHECK_MEMBER(ll_fid, f_type);
}

static void
check_mdt_body(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_body);
	CHECK_MEMBER(mdt_body, fid1);
	CHECK_MEMBER(mdt_body, fid2);
	CHECK_MEMBER(mdt_body, handle);
	CHECK_MEMBER(mdt_body, valid);
	CHECK_MEMBER(mdt_body, size);
	CHECK_MEMBER(mdt_body, mtime);
	CHECK_MEMBER(mdt_body, atime);
	CHECK_MEMBER(mdt_body, ctime);
	CHECK_MEMBER(mdt_body, blocks);
	CHECK_MEMBER(mdt_body, unused1);
	CHECK_MEMBER(mdt_body, fsuid);
	CHECK_MEMBER(mdt_body, fsgid);
	CHECK_MEMBER(mdt_body, capability);
	CHECK_MEMBER(mdt_body, mode);
	CHECK_MEMBER(mdt_body, uid);
	CHECK_MEMBER(mdt_body, gid);
	CHECK_MEMBER(mdt_body, flags);
	CHECK_MEMBER(mdt_body, rdev);
	CHECK_MEMBER(mdt_body, nlink);
	CHECK_MEMBER(mdt_body, unused2);
	CHECK_MEMBER(mdt_body, suppgid);
	CHECK_MEMBER(mdt_body, eadatasize);
	CHECK_MEMBER(mdt_body, aclsize);
	CHECK_MEMBER(mdt_body, max_mdsize);
	CHECK_MEMBER(mdt_body, max_cookiesize);
	CHECK_MEMBER(mdt_body, uid_h);
	CHECK_MEMBER(mdt_body, gid_h);
	CHECK_MEMBER(mdt_body, padding_5);
	CHECK_MEMBER(mdt_body, padding_6);
	CHECK_MEMBER(mdt_body, padding_7);
	CHECK_MEMBER(mdt_body, padding_8);
	CHECK_MEMBER(mdt_body, padding_9);
	CHECK_MEMBER(mdt_body, padding_10);

	CHECK_VALUE_O(MDS_FMODE_CLOSED);
	CHECK_VALUE_O(MDS_FMODE_EXEC);
	CHECK_VALUE_O(MDS_FMODE_EPOCH);
	CHECK_VALUE_O(MDS_FMODE_TRUNC);
	CHECK_VALUE_O(MDS_FMODE_SOM);

	CHECK_VALUE_O(MDS_OPEN_CREATED);
	CHECK_VALUE_O(MDS_OPEN_CROSS);
	CHECK_VALUE_O(MDS_OPEN_CREAT);
	CHECK_VALUE_O(MDS_OPEN_EXCL);
	CHECK_VALUE_O(MDS_OPEN_TRUNC);
	CHECK_VALUE_O(MDS_OPEN_APPEND);
	CHECK_VALUE_O(MDS_OPEN_SYNC);
	CHECK_VALUE_O(MDS_OPEN_DIRECTORY);
	CHECK_VALUE_O(MDS_OPEN_BY_FID);
	CHECK_VALUE_O(MDS_OPEN_DELAY_CREATE);
	CHECK_VALUE_O(MDS_OPEN_OWNEROVERRIDE);
	CHECK_VALUE_O(MDS_OPEN_JOIN_FILE);
	CHECK_VALUE_O(MDS_OPEN_LOCK);
	CHECK_VALUE_O(MDS_OPEN_HAS_EA);
	CHECK_VALUE_O(MDS_OPEN_HAS_OBJS);
	CHECK_VALUE_64O(MDS_OPEN_NORESTORE);
	CHECK_VALUE_64O(MDS_OPEN_NEWSTRIPE);
	CHECK_VALUE_64O(MDS_OPEN_VOLATILE);

	/* these should be identical to their EXT3_*_FL counterparts, and
	 * are redefined only to avoid dragging in ext3_fs.h */
	CHECK_DEFINE_X(LUSTRE_SYNC_FL);
	CHECK_DEFINE_X(LUSTRE_IMMUTABLE_FL);
	CHECK_DEFINE_X(LUSTRE_APPEND_FL);
	CHECK_DEFINE_X(LUSTRE_NOATIME_FL);
	CHECK_DEFINE_X(LUSTRE_DIRSYNC_FL);

	CHECK_DEFINE_X(MDS_INODELOCK_LOOKUP);
	CHECK_DEFINE_X(MDS_INODELOCK_UPDATE);
	CHECK_DEFINE_X(MDS_INODELOCK_OPEN);
	CHECK_DEFINE_X(MDS_INODELOCK_LAYOUT);
}

static void
check_mdt_ioepoch(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_ioepoch);
	CHECK_MEMBER(mdt_ioepoch, handle);
	CHECK_MEMBER(mdt_ioepoch, ioepoch);
	CHECK_MEMBER(mdt_ioepoch, flags);
	CHECK_MEMBER(mdt_ioepoch, padding);
}

static void
check_mdt_remote_perm(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_remote_perm);
	CHECK_MEMBER(mdt_remote_perm, rp_uid);
	CHECK_MEMBER(mdt_remote_perm, rp_gid);
	CHECK_MEMBER(mdt_remote_perm, rp_fsuid);
	CHECK_MEMBER(mdt_remote_perm, rp_fsgid);
	CHECK_MEMBER(mdt_remote_perm, rp_access_perm);
	CHECK_MEMBER(mdt_remote_perm, rp_padding);

	CHECK_VALUE_X(CFS_SETUID_PERM);
	CHECK_VALUE_X(CFS_SETGID_PERM);
	CHECK_VALUE_X(CFS_SETGRP_PERM);
	CHECK_VALUE_X(CFS_RMTACL_PERM);
	CHECK_VALUE_X(CFS_RMTOWN_PERM);
}

static void
check_mdt_rec_setattr(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_rec_setattr);
	CHECK_MEMBER(mdt_rec_setattr, sa_opcode);
	CHECK_MEMBER(mdt_rec_setattr, sa_cap);
	CHECK_MEMBER(mdt_rec_setattr, sa_fsuid);
	CHECK_MEMBER(mdt_rec_setattr, sa_fsuid_h);
	CHECK_MEMBER(mdt_rec_setattr, sa_fsgid);
	CHECK_MEMBER(mdt_rec_setattr, sa_fsgid_h);
	CHECK_MEMBER(mdt_rec_setattr, sa_suppgid);
	CHECK_MEMBER(mdt_rec_setattr, sa_suppgid_h);
	CHECK_MEMBER(mdt_rec_setattr, sa_padding_1);
	CHECK_MEMBER(mdt_rec_setattr, sa_padding_1_h);
	CHECK_MEMBER(mdt_rec_setattr, sa_fid);
	CHECK_MEMBER(mdt_rec_setattr, sa_valid);
	CHECK_MEMBER(mdt_rec_setattr, sa_uid);
	CHECK_MEMBER(mdt_rec_setattr, sa_gid);
	CHECK_MEMBER(mdt_rec_setattr, sa_size);
	CHECK_MEMBER(mdt_rec_setattr, sa_blocks);
	CHECK_MEMBER(mdt_rec_setattr, sa_mtime);
	CHECK_MEMBER(mdt_rec_setattr, sa_atime);
	CHECK_MEMBER(mdt_rec_setattr, sa_ctime);
	CHECK_MEMBER(mdt_rec_setattr, sa_attr_flags);
	CHECK_MEMBER(mdt_rec_setattr, sa_mode);
	CHECK_MEMBER(mdt_rec_setattr, sa_bias);
	CHECK_MEMBER(mdt_rec_setattr, sa_padding_3);
	CHECK_MEMBER(mdt_rec_setattr, sa_padding_4);
	CHECK_MEMBER(mdt_rec_setattr, sa_padding_5);
}

static void
check_mdt_rec_create(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_rec_create);
	CHECK_MEMBER(mdt_rec_create, cr_opcode);
	CHECK_MEMBER(mdt_rec_create, cr_cap);
	CHECK_MEMBER(mdt_rec_create, cr_fsuid);
	CHECK_MEMBER(mdt_rec_create, cr_fsuid_h);
	CHECK_MEMBER(mdt_rec_create, cr_fsgid);
	CHECK_MEMBER(mdt_rec_create, cr_fsgid_h);
	CHECK_MEMBER(mdt_rec_create, cr_suppgid1);
	CHECK_MEMBER(mdt_rec_create, cr_suppgid1_h);
	CHECK_MEMBER(mdt_rec_create, cr_suppgid2);
	CHECK_MEMBER(mdt_rec_create, cr_suppgid2_h);
	CHECK_MEMBER(mdt_rec_create, cr_fid1);
	CHECK_MEMBER(mdt_rec_create, cr_fid2);
	CHECK_MEMBER(mdt_rec_create, cr_old_handle);
	CHECK_MEMBER(mdt_rec_create, cr_time);
	CHECK_MEMBER(mdt_rec_create, cr_rdev);
	CHECK_MEMBER(mdt_rec_create, cr_ioepoch);
	CHECK_MEMBER(mdt_rec_create, cr_padding_1);
	CHECK_MEMBER(mdt_rec_create, cr_mode);
	CHECK_MEMBER(mdt_rec_create, cr_bias);
	CHECK_MEMBER(mdt_rec_create, cr_flags_l);
	CHECK_MEMBER(mdt_rec_create, cr_flags_h);
	CHECK_MEMBER(mdt_rec_create, cr_umask);
	CHECK_MEMBER(mdt_rec_create, cr_padding_4);
}

static void
check_mdt_rec_link(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_rec_link);
	CHECK_MEMBER(mdt_rec_link, lk_opcode);
	CHECK_MEMBER(mdt_rec_link, lk_cap);
	CHECK_MEMBER(mdt_rec_link, lk_fsuid);
	CHECK_MEMBER(mdt_rec_link, lk_fsuid_h);
	CHECK_MEMBER(mdt_rec_link, lk_fsgid);
	CHECK_MEMBER(mdt_rec_link, lk_fsgid_h);
	CHECK_MEMBER(mdt_rec_link, lk_suppgid1);
	CHECK_MEMBER(mdt_rec_link, lk_suppgid1_h);
	CHECK_MEMBER(mdt_rec_link, lk_suppgid2);
	CHECK_MEMBER(mdt_rec_link, lk_suppgid2_h);
	CHECK_MEMBER(mdt_rec_link, lk_fid1);
	CHECK_MEMBER(mdt_rec_link, lk_fid2);
	CHECK_MEMBER(mdt_rec_link, lk_time);
	CHECK_MEMBER(mdt_rec_link, lk_padding_1);
	CHECK_MEMBER(mdt_rec_link, lk_padding_2);
	CHECK_MEMBER(mdt_rec_link, lk_padding_3);
	CHECK_MEMBER(mdt_rec_link, lk_padding_4);
	CHECK_MEMBER(mdt_rec_link, lk_bias);
	CHECK_MEMBER(mdt_rec_link, lk_padding_5);
	CHECK_MEMBER(mdt_rec_link, lk_padding_6);
	CHECK_MEMBER(mdt_rec_link, lk_padding_7);
	CHECK_MEMBER(mdt_rec_link, lk_padding_8);
	CHECK_MEMBER(mdt_rec_link, lk_padding_9);
}

static void
check_mdt_rec_unlink(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_rec_unlink);
	CHECK_MEMBER(mdt_rec_unlink, ul_opcode);
	CHECK_MEMBER(mdt_rec_unlink, ul_cap);
	CHECK_MEMBER(mdt_rec_unlink, ul_fsuid);
	CHECK_MEMBER(mdt_rec_unlink, ul_fsuid_h);
	CHECK_MEMBER(mdt_rec_unlink, ul_fsgid);
	CHECK_MEMBER(mdt_rec_unlink, ul_fsgid_h);
	CHECK_MEMBER(mdt_rec_unlink, ul_suppgid1);
	CHECK_MEMBER(mdt_rec_unlink, ul_suppgid1_h);
	CHECK_MEMBER(mdt_rec_unlink, ul_suppgid2);
	CHECK_MEMBER(mdt_rec_unlink, ul_suppgid2_h);
	CHECK_MEMBER(mdt_rec_unlink, ul_fid1);
	CHECK_MEMBER(mdt_rec_unlink, ul_fid2);
	CHECK_MEMBER(mdt_rec_unlink, ul_time);
	CHECK_MEMBER(mdt_rec_unlink, ul_padding_2);
	CHECK_MEMBER(mdt_rec_unlink, ul_padding_3);
	CHECK_MEMBER(mdt_rec_unlink, ul_padding_4);
	CHECK_MEMBER(mdt_rec_unlink, ul_padding_5);
	CHECK_MEMBER(mdt_rec_unlink, ul_bias);
	CHECK_MEMBER(mdt_rec_unlink, ul_mode);
	CHECK_MEMBER(mdt_rec_unlink, ul_padding_6);
	CHECK_MEMBER(mdt_rec_unlink, ul_padding_7);
	CHECK_MEMBER(mdt_rec_unlink, ul_padding_8);
	CHECK_MEMBER(mdt_rec_unlink, ul_padding_9);
}

static void
check_mdt_rec_rename(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_rec_rename);
	CHECK_MEMBER(mdt_rec_rename, rn_opcode);
	CHECK_MEMBER(mdt_rec_rename, rn_cap);
	CHECK_MEMBER(mdt_rec_rename, rn_fsuid);
	CHECK_MEMBER(mdt_rec_rename, rn_fsuid_h);
	CHECK_MEMBER(mdt_rec_rename, rn_fsgid);
	CHECK_MEMBER(mdt_rec_rename, rn_fsgid_h);
	CHECK_MEMBER(mdt_rec_rename, rn_suppgid1);
	CHECK_MEMBER(mdt_rec_rename, rn_suppgid1_h);
	CHECK_MEMBER(mdt_rec_rename, rn_suppgid2);
	CHECK_MEMBER(mdt_rec_rename, rn_suppgid2_h);
	CHECK_MEMBER(mdt_rec_rename, rn_fid1);
	CHECK_MEMBER(mdt_rec_rename, rn_fid2);
	CHECK_MEMBER(mdt_rec_rename, rn_time);
	CHECK_MEMBER(mdt_rec_rename, rn_padding_1);
	CHECK_MEMBER(mdt_rec_rename, rn_padding_2);
	CHECK_MEMBER(mdt_rec_rename, rn_padding_3);
	CHECK_MEMBER(mdt_rec_rename, rn_padding_4);
	CHECK_MEMBER(mdt_rec_rename, rn_bias);
	CHECK_MEMBER(mdt_rec_rename, rn_mode);
	CHECK_MEMBER(mdt_rec_rename, rn_padding_5);
	CHECK_MEMBER(mdt_rec_rename, rn_padding_6);
	CHECK_MEMBER(mdt_rec_rename, rn_padding_7);
	CHECK_MEMBER(mdt_rec_rename, rn_padding_8);
}

static void
check_mdt_rec_setxattr(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_rec_setxattr);
	CHECK_MEMBER(mdt_rec_setxattr, sx_opcode);
	CHECK_MEMBER(mdt_rec_setxattr, sx_cap);
	CHECK_MEMBER(mdt_rec_setxattr, sx_fsuid);
	CHECK_MEMBER(mdt_rec_setxattr, sx_fsuid_h);
	CHECK_MEMBER(mdt_rec_setxattr, sx_fsgid);
	CHECK_MEMBER(mdt_rec_setxattr, sx_fsgid_h);
	CHECK_MEMBER(mdt_rec_setxattr, sx_suppgid1);
	CHECK_MEMBER(mdt_rec_setxattr, sx_suppgid1_h);
	CHECK_MEMBER(mdt_rec_setxattr, sx_suppgid2);
	CHECK_MEMBER(mdt_rec_setxattr, sx_suppgid2_h);
	CHECK_MEMBER(mdt_rec_setxattr, sx_fid);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_1);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_2);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_3);
	CHECK_MEMBER(mdt_rec_setxattr, sx_valid);
	CHECK_MEMBER(mdt_rec_setxattr, sx_time);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_5);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_6);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_7);
	CHECK_MEMBER(mdt_rec_setxattr, sx_size);
	CHECK_MEMBER(mdt_rec_setxattr, sx_flags);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_8);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_9);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_10);
	CHECK_MEMBER(mdt_rec_setxattr, sx_padding_11);
}

static void
check_mdt_rec_reint(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mdt_rec_reint);
	CHECK_MEMBER(mdt_rec_reint, rr_opcode);
	CHECK_MEMBER(mdt_rec_reint, rr_cap);
	CHECK_MEMBER(mdt_rec_reint, rr_fsuid);
	CHECK_MEMBER(mdt_rec_reint, rr_fsuid_h);
	CHECK_MEMBER(mdt_rec_reint, rr_fsgid);
	CHECK_MEMBER(mdt_rec_reint, rr_fsgid_h);
	CHECK_MEMBER(mdt_rec_reint, rr_suppgid1);
	CHECK_MEMBER(mdt_rec_reint, rr_suppgid1_h);
	CHECK_MEMBER(mdt_rec_reint, rr_suppgid2);
	CHECK_MEMBER(mdt_rec_reint, rr_suppgid2_h);
	CHECK_MEMBER(mdt_rec_reint, rr_fid1);
	CHECK_MEMBER(mdt_rec_reint, rr_fid2);
	CHECK_MEMBER(mdt_rec_reint, rr_mtime);
	CHECK_MEMBER(mdt_rec_reint, rr_atime);
	CHECK_MEMBER(mdt_rec_reint, rr_ctime);
	CHECK_MEMBER(mdt_rec_reint, rr_size);
	CHECK_MEMBER(mdt_rec_reint, rr_blocks);
	CHECK_MEMBER(mdt_rec_reint, rr_bias);
	CHECK_MEMBER(mdt_rec_reint, rr_mode);
	CHECK_MEMBER(mdt_rec_reint, rr_flags);
	CHECK_MEMBER(mdt_rec_reint, rr_padding_2);
	CHECK_MEMBER(mdt_rec_reint, rr_padding_3);
	CHECK_MEMBER(mdt_rec_reint, rr_padding_4);
}

static void
check_lmv_desc(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lmv_desc);
	CHECK_MEMBER(lmv_desc, ld_tgt_count);
	CHECK_MEMBER(lmv_desc, ld_active_tgt_count);
	CHECK_MEMBER(lmv_desc, ld_default_stripe_count);
	CHECK_MEMBER(lmv_desc, ld_pattern);
	CHECK_MEMBER(lmv_desc, ld_default_hash_size);
	CHECK_MEMBER(lmv_desc, ld_padding_1);
	CHECK_MEMBER(lmv_desc, ld_padding_2);
	CHECK_MEMBER(lmv_desc, ld_qos_maxage);
	CHECK_MEMBER(lmv_desc, ld_padding_3);
	CHECK_MEMBER(lmv_desc, ld_padding_4);
	CHECK_MEMBER(lmv_desc, ld_uuid);
}

static void
check_lmv_stripe_md(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lmv_stripe_md);
	CHECK_MEMBER(lmv_stripe_md, mea_magic);
	CHECK_MEMBER(lmv_stripe_md, mea_count);
	CHECK_MEMBER(lmv_stripe_md, mea_master);
	CHECK_MEMBER(lmv_stripe_md, mea_padding);
	CHECK_CVALUE(LOV_MAXPOOLNAME);
	CHECK_MEMBER(lmv_stripe_md, mea_pool_name[LOV_MAXPOOLNAME]);
	CHECK_MEMBER(lmv_stripe_md, mea_ids[0]);
}

static void
check_lov_desc(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lov_desc);
	CHECK_MEMBER(lov_desc, ld_tgt_count);
	CHECK_MEMBER(lov_desc, ld_active_tgt_count);
	CHECK_MEMBER(lov_desc, ld_default_stripe_count);
	CHECK_MEMBER(lov_desc, ld_pattern);
	CHECK_MEMBER(lov_desc, ld_default_stripe_size);
	CHECK_MEMBER(lov_desc, ld_default_stripe_offset);
	CHECK_MEMBER(lov_desc, ld_padding_0);
	CHECK_MEMBER(lov_desc, ld_qos_maxage);
	CHECK_MEMBER(lov_desc, ld_padding_1);
	CHECK_MEMBER(lov_desc, ld_padding_2);
	CHECK_MEMBER(lov_desc, ld_uuid);

	CHECK_CDEFINE(LOV_DESC_MAGIC);
}

static void
check_ldlm_res_id(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_res_id);
	CHECK_CVALUE(RES_NAME_SIZE);
	CHECK_MEMBER(ldlm_res_id, name[RES_NAME_SIZE]);
}

static void
check_ldlm_extent(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_extent);
	CHECK_MEMBER(ldlm_extent, start);
	CHECK_MEMBER(ldlm_extent, end);
	CHECK_MEMBER(ldlm_extent, gid);
}

static void
check_ldlm_inodebits(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_inodebits);
	CHECK_MEMBER(ldlm_inodebits, bits);
}

static void
check_ldlm_flock(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_flock_wire);
	CHECK_MEMBER(ldlm_flock_wire, lfw_start);
	CHECK_MEMBER(ldlm_flock_wire, lfw_end);
	CHECK_MEMBER(ldlm_flock_wire, lfw_owner);
	CHECK_MEMBER(ldlm_flock_wire, lfw_padding);
	CHECK_MEMBER(ldlm_flock_wire, lfw_pid);
}

static void
check_ldlm_intent(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_intent);
	CHECK_MEMBER(ldlm_intent, opc);
}

static void
check_ldlm_resource_desc(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_resource_desc);
	CHECK_MEMBER(ldlm_resource_desc, lr_type);
	CHECK_MEMBER(ldlm_resource_desc, lr_padding);
	CHECK_MEMBER(ldlm_resource_desc, lr_name);
}

static void
check_ldlm_lock_desc(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_lock_desc);
	CHECK_MEMBER(ldlm_lock_desc, l_resource);
	CHECK_MEMBER(ldlm_lock_desc, l_req_mode);
	CHECK_MEMBER(ldlm_lock_desc, l_granted_mode);
	CHECK_MEMBER(ldlm_lock_desc, l_policy_data);
}

static void
check_ldlm_request(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_request);
	CHECK_MEMBER(ldlm_request, lock_flags);
	CHECK_MEMBER(ldlm_request, lock_count);
	CHECK_MEMBER(ldlm_request, lock_desc);
	CHECK_MEMBER(ldlm_request, lock_handle);
}

static void
check_ldlm_reply(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_reply);
	CHECK_MEMBER(ldlm_reply, lock_flags);
	CHECK_MEMBER(ldlm_reply, lock_padding);
	CHECK_MEMBER(ldlm_reply, lock_desc);
	CHECK_MEMBER(ldlm_reply, lock_handle);
	CHECK_MEMBER(ldlm_reply, lock_policy_res1);
	CHECK_MEMBER(ldlm_reply, lock_policy_res2);
}

static void
check_ldlm_ost_lvb_v1(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ost_lvb_v1);
	CHECK_MEMBER(ost_lvb_v1, lvb_size);
	CHECK_MEMBER(ost_lvb_v1, lvb_mtime);
	CHECK_MEMBER(ost_lvb_v1, lvb_atime);
	CHECK_MEMBER(ost_lvb_v1, lvb_ctime);
	CHECK_MEMBER(ost_lvb_v1, lvb_blocks);
}

static void
check_ldlm_ost_lvb(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ost_lvb);
	CHECK_MEMBER(ost_lvb, lvb_size);
	CHECK_MEMBER(ost_lvb, lvb_mtime);
	CHECK_MEMBER(ost_lvb, lvb_atime);
	CHECK_MEMBER(ost_lvb, lvb_ctime);
	CHECK_MEMBER(ost_lvb, lvb_blocks);
	CHECK_MEMBER(ost_lvb, lvb_mtime_ns);
	CHECK_MEMBER(ost_lvb, lvb_atime_ns);
	CHECK_MEMBER(ost_lvb, lvb_ctime_ns);
	CHECK_MEMBER(ost_lvb, lvb_padding);
}

static void
check_ldlm_lquota_lvb(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lquota_lvb);
	CHECK_MEMBER(lquota_lvb, lvb_flags);
	CHECK_MEMBER(lquota_lvb, lvb_id_may_rel);
	CHECK_MEMBER(lquota_lvb, lvb_id_rel);
	CHECK_MEMBER(lquota_lvb, lvb_id_qunit);
	CHECK_MEMBER(lquota_lvb, lvb_pad1);
	CHECK_VALUE(LQUOTA_FL_EDQUOT);
}

static void
check_ldlm_gl_lquota_desc(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ldlm_gl_lquota_desc);
	CHECK_MEMBER(ldlm_gl_lquota_desc, gl_id);
	CHECK_MEMBER(ldlm_gl_lquota_desc, gl_flags);
	CHECK_MEMBER(ldlm_gl_lquota_desc, gl_ver);
	CHECK_MEMBER(ldlm_gl_lquota_desc, gl_hardlimit);
	CHECK_MEMBER(ldlm_gl_lquota_desc, gl_softlimit);
	CHECK_MEMBER(ldlm_gl_lquota_desc, gl_time);
	CHECK_MEMBER(ldlm_gl_lquota_desc, gl_pad2);
}


static void
check_mgs_send_param(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mgs_send_param);
	CHECK_CVALUE(MGS_PARAM_MAXLEN);
	CHECK_MEMBER(mgs_send_param, mgs_param[MGS_PARAM_MAXLEN]);
}

static void
check_cfg_marker(void)
{
	BLANK_LINE();
	CHECK_STRUCT(cfg_marker);
	CHECK_MEMBER(cfg_marker, cm_step);
	CHECK_MEMBER(cfg_marker, cm_flags);
	CHECK_MEMBER(cfg_marker, cm_vers);
	CHECK_MEMBER(cfg_marker, cm_padding);
	CHECK_MEMBER(cfg_marker, cm_createtime);
	CHECK_MEMBER(cfg_marker, cm_canceltime);
	CHECK_MEMBER(cfg_marker, cm_tgtname);
	CHECK_MEMBER(cfg_marker, cm_comment);
}

static void
check_llog_logid(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_logid);
	CHECK_MEMBER(llog_logid, lgl_oid);
	CHECK_MEMBER(llog_logid, lgl_oseq);
	CHECK_MEMBER(llog_logid, lgl_ogen);

	CHECK_CVALUE(OST_SZ_REC);
	CHECK_CVALUE(MDS_UNLINK_REC);
	CHECK_CVALUE(MDS_UNLINK64_REC);
	CHECK_CVALUE(MDS_SETATTR64_REC);
	CHECK_CVALUE(OBD_CFG_REC);
	CHECK_CVALUE(LLOG_GEN_REC);
	CHECK_CVALUE(CHANGELOG_REC);
	CHECK_CVALUE(CHANGELOG_USER_REC);
	CHECK_CVALUE(LLOG_HDR_MAGIC);
	CHECK_CVALUE(LLOG_LOGID_MAGIC);
}

static void
check_llog_catid(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_catid);
	CHECK_MEMBER(llog_catid, lci_logid);
	CHECK_MEMBER(llog_catid, lci_padding1);
	CHECK_MEMBER(llog_catid, lci_padding2);
	CHECK_MEMBER(llog_catid, lci_padding3);
}

static void
check_llog_rec_hdr(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_rec_hdr);
	CHECK_MEMBER(llog_rec_hdr, lrh_len);
	CHECK_MEMBER(llog_rec_hdr, lrh_index);
	CHECK_MEMBER(llog_rec_hdr, lrh_type);
	CHECK_MEMBER(llog_rec_hdr, lrh_id);
}

static void
check_llog_rec_tail(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_rec_tail);
	CHECK_MEMBER(llog_rec_tail, lrt_len);
	CHECK_MEMBER(llog_rec_tail, lrt_index);
}

static void
check_llog_logid_rec(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_logid_rec);
	CHECK_MEMBER(llog_logid_rec, lid_hdr);
	CHECK_MEMBER(llog_logid_rec, lid_id);
	CHECK_MEMBER(llog_logid_rec, lid_padding1);
	CHECK_MEMBER(llog_logid_rec, lid_padding2);
	CHECK_MEMBER(llog_logid_rec, lid_padding3);
	CHECK_MEMBER(llog_logid_rec, lid_tail);
}

static void
check_llog_unlink_rec(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_unlink_rec);
	CHECK_MEMBER(llog_unlink_rec, lur_hdr);
	CHECK_MEMBER(llog_unlink_rec, lur_oid);
	CHECK_MEMBER(llog_unlink_rec, lur_oseq);
	CHECK_MEMBER(llog_unlink_rec, lur_count);
	CHECK_MEMBER(llog_unlink_rec, lur_tail);
}

static void
check_llog_unlink64_rec(void)
{
	CHECK_STRUCT(llog_unlink64_rec);
	CHECK_MEMBER(llog_unlink64_rec, lur_hdr);
	CHECK_MEMBER(llog_unlink64_rec, lur_fid);
	CHECK_MEMBER(llog_unlink64_rec, lur_count);
	CHECK_MEMBER(llog_unlink64_rec, lur_tail);
	CHECK_MEMBER(llog_unlink64_rec, lur_padding1);
	CHECK_MEMBER(llog_unlink64_rec, lur_padding2);
	CHECK_MEMBER(llog_unlink64_rec, lur_padding3);
}

static void
check_llog_setattr64_rec(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_setattr64_rec);
	CHECK_MEMBER(llog_setattr64_rec, lsr_hdr);
	CHECK_MEMBER(llog_setattr64_rec, lsr_oid);
	CHECK_MEMBER(llog_setattr64_rec, lsr_oseq);
	CHECK_MEMBER(llog_setattr64_rec, lsr_uid);
	CHECK_MEMBER(llog_setattr64_rec, lsr_uid_h);
	CHECK_MEMBER(llog_setattr64_rec, lsr_gid);
	CHECK_MEMBER(llog_setattr64_rec, lsr_gid_h);
	CHECK_MEMBER(llog_setattr64_rec, lsr_padding);
	CHECK_MEMBER(llog_setattr64_rec, lsr_tail);
}

static void
check_llog_size_change_rec(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_size_change_rec);
	CHECK_MEMBER(llog_size_change_rec, lsc_hdr);
	CHECK_MEMBER(llog_size_change_rec, lsc_fid);
	CHECK_MEMBER(llog_size_change_rec, lsc_ioepoch);
	CHECK_MEMBER(llog_size_change_rec, lsc_padding1);
	CHECK_MEMBER(llog_size_change_rec, lsc_padding2);
	CHECK_MEMBER(llog_size_change_rec, lsc_padding3);
	CHECK_MEMBER(llog_size_change_rec, lsc_tail);
}

static void
check_changelog_rec(void)
{
	BLANK_LINE();
	CHECK_STRUCT(changelog_rec);
	CHECK_MEMBER(changelog_rec, cr_namelen);
	CHECK_MEMBER(changelog_rec, cr_flags);
	CHECK_MEMBER(changelog_rec, cr_type);
	CHECK_MEMBER(changelog_rec, cr_index);
	CHECK_MEMBER(changelog_rec, cr_prev);
	CHECK_MEMBER(changelog_rec, cr_time);
	CHECK_MEMBER(changelog_rec, cr_tfid);
	CHECK_MEMBER(changelog_rec, cr_pfid);
}

static void
check_changelog_rec_ext(void)
{
	BLANK_LINE();
	CHECK_STRUCT(changelog_ext_rec);
	CHECK_MEMBER(changelog_ext_rec, cr_namelen);
	CHECK_MEMBER(changelog_ext_rec, cr_flags);
	CHECK_MEMBER(changelog_ext_rec, cr_type);
	CHECK_MEMBER(changelog_ext_rec, cr_index);
	CHECK_MEMBER(changelog_ext_rec, cr_prev);
	CHECK_MEMBER(changelog_ext_rec, cr_time);
	CHECK_MEMBER(changelog_ext_rec, cr_tfid);
	CHECK_MEMBER(changelog_ext_rec, cr_pfid);
	CHECK_MEMBER(changelog_ext_rec, cr_sfid);
	CHECK_MEMBER(changelog_ext_rec, cr_spfid);
}

static void
check_changelog_setinfo(void)
{
	BLANK_LINE();
	CHECK_STRUCT(changelog_setinfo);
	CHECK_MEMBER(changelog_setinfo, cs_recno);
	CHECK_MEMBER(changelog_setinfo, cs_id);
}

static void
check_llog_changelog_rec(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_changelog_rec);
	CHECK_MEMBER(llog_changelog_rec, cr_hdr);
	CHECK_MEMBER(llog_changelog_rec, cr);
	CHECK_MEMBER(llog_changelog_rec, cr_tail);
}

static void
check_llog_changelog_user_rec(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_changelog_user_rec);
	CHECK_MEMBER(llog_changelog_user_rec, cur_hdr);
	CHECK_MEMBER(llog_changelog_user_rec, cur_id);
	CHECK_MEMBER(llog_changelog_user_rec, cur_padding);
	CHECK_MEMBER(llog_changelog_user_rec, cur_endrec);
	CHECK_MEMBER(llog_changelog_user_rec, cur_tail);
}

static void
check_llog_gen(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_gen);
	CHECK_MEMBER(llog_gen, mnt_cnt);
	CHECK_MEMBER(llog_gen, conn_cnt);
}

static void
check_llog_gen_rec(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_gen_rec);
	CHECK_MEMBER(llog_gen_rec, lgr_hdr);
	CHECK_MEMBER(llog_gen_rec, lgr_gen);
	CHECK_MEMBER(llog_gen_rec, lgr_tail);
}

static void
check_llog_log_hdr(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_log_hdr);
	CHECK_MEMBER(llog_log_hdr, llh_hdr);
	CHECK_MEMBER(llog_log_hdr, llh_timestamp);
	CHECK_MEMBER(llog_log_hdr, llh_count);
	CHECK_MEMBER(llog_log_hdr, llh_bitmap_offset);
	CHECK_MEMBER(llog_log_hdr, llh_size);
	CHECK_MEMBER(llog_log_hdr, llh_flags);
	CHECK_MEMBER(llog_log_hdr, llh_cat_idx);
	CHECK_MEMBER(llog_log_hdr, llh_tgtuuid);
	CHECK_MEMBER(llog_log_hdr, llh_reserved);
	CHECK_MEMBER(llog_log_hdr, llh_bitmap);
	CHECK_MEMBER(llog_log_hdr, llh_tail);
}

static void
check_llog_cookie(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llog_cookie);
	CHECK_MEMBER(llog_cookie, lgc_lgl);
	CHECK_MEMBER(llog_cookie, lgc_subsys);
	CHECK_MEMBER(llog_cookie, lgc_index);
	CHECK_MEMBER(llog_cookie, lgc_padding);
}

static void
check_llogd_body(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llogd_body);
	CHECK_MEMBER(llogd_body, lgd_logid);
	CHECK_MEMBER(llogd_body, lgd_ctxt_idx);
	CHECK_MEMBER(llogd_body, lgd_llh_flags);
	CHECK_MEMBER(llogd_body, lgd_index);
	CHECK_MEMBER(llogd_body, lgd_saved_index);
	CHECK_MEMBER(llogd_body, lgd_len);
	CHECK_MEMBER(llogd_body, lgd_cur_offset);

	CHECK_CVALUE(LLOG_ORIGIN_HANDLE_CREATE);
	CHECK_CVALUE(LLOG_ORIGIN_HANDLE_NEXT_BLOCK);
	CHECK_CVALUE(LLOG_ORIGIN_HANDLE_READ_HEADER);
	CHECK_CVALUE(LLOG_ORIGIN_HANDLE_WRITE_REC);
	CHECK_CVALUE(LLOG_ORIGIN_HANDLE_CLOSE);
	CHECK_CVALUE(LLOG_ORIGIN_CONNECT);
	CHECK_CVALUE(LLOG_CATINFO);
	CHECK_CVALUE(LLOG_ORIGIN_HANDLE_PREV_BLOCK);
	CHECK_CVALUE(LLOG_ORIGIN_HANDLE_DESTROY);
	CHECK_CVALUE(LLOG_FIRST_OPC);
	CHECK_CVALUE(LLOG_LAST_OPC);
}

static void
check_llogd_conn_body(void)
{
	BLANK_LINE();
	CHECK_STRUCT(llogd_conn_body);
	CHECK_MEMBER(llogd_conn_body, lgdc_gen);
	CHECK_MEMBER(llogd_conn_body, lgdc_logid);
	CHECK_MEMBER(llogd_conn_body, lgdc_ctxt_idx);
}

static void
check_ll_fiemap_info_key(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ll_fiemap_info_key);
	CHECK_MEMBER(ll_fiemap_info_key, name[8]);
	CHECK_MEMBER(ll_fiemap_info_key, oa);
	CHECK_MEMBER(ll_fiemap_info_key, fiemap);
}

static void
check_quota_body(void)
{
	BLANK_LINE();
	CHECK_STRUCT(quota_body);
	CHECK_MEMBER(quota_body, qb_fid);
	CHECK_MEMBER(quota_body, qb_id);
	CHECK_MEMBER(quota_body, qb_flags);
	CHECK_MEMBER(quota_body, qb_padding);
	CHECK_MEMBER(quota_body, qb_count);
	CHECK_MEMBER(quota_body, qb_usage);
	CHECK_MEMBER(quota_body, qb_slv_ver);
	CHECK_MEMBER(quota_body, qb_lockh);
	CHECK_MEMBER(quota_body, qb_glb_lockh);
	CHECK_MEMBER(quota_body, qb_padding1[4]);
}

static void
check_mgs_target_info(void)
{
	BLANK_LINE();
	CHECK_STRUCT(mgs_target_info);
	CHECK_MEMBER(mgs_target_info, mti_lustre_ver);
	CHECK_MEMBER(mgs_target_info, mti_stripe_index);
	CHECK_MEMBER(mgs_target_info, mti_config_ver);
	CHECK_MEMBER(mgs_target_info, mti_flags);
	CHECK_MEMBER(mgs_target_info, mti_nid_count);
	CHECK_MEMBER(mgs_target_info, mti_instance);
	CHECK_MEMBER(mgs_target_info, mti_fsname);
	CHECK_MEMBER(mgs_target_info, mti_svname);
	CHECK_MEMBER(mgs_target_info, mti_uuid);
	CHECK_MEMBER(mgs_target_info, mti_nids);
	CHECK_MEMBER(mgs_target_info, mti_params);
}

static void
check_lustre_capa(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lustre_capa);
	CHECK_MEMBER(lustre_capa, lc_fid);
	CHECK_MEMBER(lustre_capa, lc_opc);
	CHECK_MEMBER(lustre_capa, lc_uid);
	CHECK_MEMBER(lustre_capa, lc_gid);
	CHECK_MEMBER(lustre_capa, lc_flags);
	CHECK_MEMBER(lustre_capa, lc_keyid);
	CHECK_MEMBER(lustre_capa, lc_timeout);
	CHECK_MEMBER(lustre_capa, lc_expiry);
	CHECK_CVALUE(CAPA_HMAC_MAX_LEN);
	CHECK_MEMBER(lustre_capa, lc_hmac[CAPA_HMAC_MAX_LEN]);
}

static void
check_lustre_capa_key(void)
{
	BLANK_LINE();
	CHECK_STRUCT(lustre_capa_key);
	CHECK_MEMBER(lustre_capa_key, lk_seq);
	CHECK_MEMBER(lustre_capa_key, lk_keyid);
	CHECK_MEMBER(lustre_capa_key, lk_padding);
	CHECK_CVALUE(CAPA_HMAC_KEY_MAX_LEN);
	CHECK_MEMBER(lustre_capa_key, lk_key[CAPA_HMAC_KEY_MAX_LEN]);
}

static void
check_getinfo_fid2path(void)
{
	BLANK_LINE();
	CHECK_STRUCT(getinfo_fid2path);
	CHECK_MEMBER(getinfo_fid2path, gf_fid);
	CHECK_MEMBER(getinfo_fid2path, gf_recno);
	CHECK_MEMBER(getinfo_fid2path, gf_linkno);
	CHECK_MEMBER(getinfo_fid2path, gf_pathlen);
	CHECK_MEMBER(getinfo_fid2path, gf_path[0]);
}

static void
check_posix_acl_xattr_entry(void)
{
	BLANK_LINE();
	CHECK_STRUCT_TYPEDEF(posix_acl_xattr_entry);
	CHECK_MEMBER_TYPEDEF(posix_acl_xattr_entry, e_tag);
	CHECK_MEMBER_TYPEDEF(posix_acl_xattr_entry, e_perm);
	CHECK_MEMBER_TYPEDEF(posix_acl_xattr_entry, e_id);
}

static void
check_posix_acl_xattr_header(void)
{
	BLANK_LINE();
	CHECK_STRUCT_TYPEDEF(posix_acl_xattr_header);
	CHECK_MEMBER_TYPEDEF(posix_acl_xattr_header, a_version);
	CHECK_MEMBER_TYPEDEF(posix_acl_xattr_header, a_entries);
}

static void
check_ll_user_fiemap(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ll_user_fiemap);
	CHECK_MEMBER(ll_user_fiemap, fm_start);
	CHECK_MEMBER(ll_user_fiemap, fm_length);
	CHECK_MEMBER(ll_user_fiemap, fm_flags);
	CHECK_MEMBER(ll_user_fiemap, fm_mapped_extents);
	CHECK_MEMBER(ll_user_fiemap, fm_extent_count);
	CHECK_MEMBER(ll_user_fiemap, fm_reserved);
	CHECK_MEMBER(ll_user_fiemap, fm_extents);

	CHECK_CDEFINE(FIEMAP_FLAG_SYNC);
	CHECK_CDEFINE(FIEMAP_FLAG_XATTR);
	CHECK_CDEFINE(FIEMAP_FLAG_DEVICE_ORDER);
}

static void
check_ll_fiemap_extent(void)
{
	BLANK_LINE();
	CHECK_STRUCT(ll_fiemap_extent);
	CHECK_MEMBER(ll_fiemap_extent, fe_logical);
	CHECK_MEMBER(ll_fiemap_extent, fe_physical);
	CHECK_MEMBER(ll_fiemap_extent, fe_length);
	CHECK_MEMBER(ll_fiemap_extent, fe_flags);
	CHECK_MEMBER(ll_fiemap_extent, fe_device);

	CHECK_CDEFINE(FIEMAP_EXTENT_LAST);
	CHECK_CDEFINE(FIEMAP_EXTENT_UNKNOWN);
	CHECK_CDEFINE(FIEMAP_EXTENT_DELALLOC);
	CHECK_CDEFINE(FIEMAP_EXTENT_ENCODED);
	CHECK_CDEFINE(FIEMAP_EXTENT_DATA_ENCRYPTED);
	CHECK_CDEFINE(FIEMAP_EXTENT_NOT_ALIGNED);
	CHECK_CDEFINE(FIEMAP_EXTENT_DATA_INLINE);
	CHECK_CDEFINE(FIEMAP_EXTENT_DATA_TAIL);
	CHECK_CDEFINE(FIEMAP_EXTENT_UNWRITTEN);
	CHECK_CDEFINE(FIEMAP_EXTENT_MERGED);
	CHECK_CDEFINE(FIEMAP_EXTENT_NO_DIRECT);
	CHECK_CDEFINE(FIEMAP_EXTENT_NET);
}

static void
check_link_ea_header(void)
{
	BLANK_LINE();
	CHECK_STRUCT(link_ea_header);
	CHECK_MEMBER(link_ea_header, leh_magic);
	CHECK_MEMBER(link_ea_header, leh_reccount);
	CHECK_MEMBER(link_ea_header, leh_len);
	CHECK_MEMBER(link_ea_header, padding1);
	CHECK_MEMBER(link_ea_header, padding2);

	CHECK_CDEFINE(LINK_EA_MAGIC);
}

static void
check_link_ea_entry(void)
{
	BLANK_LINE();
	CHECK_STRUCT(link_ea_entry);
	CHECK_MEMBER(link_ea_entry, lee_reclen);
	CHECK_MEMBER(link_ea_entry, lee_parent_fid);
	CHECK_MEMBER(link_ea_entry, lee_name);
}

static void
check_hsm_user_item(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_user_item);
	CHECK_MEMBER(hsm_user_item, hui_fid);
	CHECK_MEMBER(hsm_user_item, hui_extent);
}

static void
check_hsm_user_state(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_user_state);
	CHECK_MEMBER(hsm_user_state, hus_states);
	CHECK_MEMBER(hsm_user_state, hus_archive_id);
	CHECK_MEMBER(hsm_user_state, hus_in_progress_state);
	CHECK_MEMBER(hsm_user_state, hus_in_progress_action);
	CHECK_MEMBER(hsm_user_state, hus_in_progress_location);
}

static void
check_hsm_action_item(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_action_item);
	CHECK_MEMBER(hsm_action_item, hai_len);
	CHECK_MEMBER(hsm_action_item, hai_action);
	CHECK_MEMBER(hsm_action_item, hai_fid);
	CHECK_MEMBER(hsm_action_item, hai_dfid);
	CHECK_MEMBER(hsm_action_item, hai_extent);
	CHECK_MEMBER(hsm_action_item, hai_cookie);
	CHECK_MEMBER(hsm_action_item, hai_gid);
	CHECK_MEMBER(hsm_action_item, hai_data);
}

static void
check_hsm_action_list(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_action_list);
	CHECK_MEMBER(hsm_action_list, hal_version);
	CHECK_MEMBER(hsm_action_list, hal_count);
	CHECK_MEMBER(hsm_action_list, hal_compound_id);
	CHECK_MEMBER(hsm_action_list, hal_flags);
	CHECK_MEMBER(hsm_action_list, hal_archive_id);
	CHECK_MEMBER(hsm_action_list, padding1);
	CHECK_MEMBER(hsm_action_list, hal_fsname);
}

static void
check_hsm_progress_kernel(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_progress_kernel);
	CHECK_MEMBER(hsm_progress_kernel, hpk_fid);
	CHECK_MEMBER(hsm_progress_kernel, hpk_cookie);
	CHECK_MEMBER(hsm_progress_kernel, hpk_extent);
	CHECK_MEMBER(hsm_progress_kernel, hpk_flags);
	CHECK_MEMBER(hsm_progress_kernel, hpk_errval);
	CHECK_MEMBER(hsm_progress_kernel, hpk_padding1);
	CHECK_MEMBER(hsm_progress_kernel, hpk_data_version);
	CHECK_MEMBER(hsm_progress_kernel, hpk_padding2);
}

static void
check_hsm_progress(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_progress);
	CHECK_MEMBER(hsm_progress, hp_fid);
	CHECK_MEMBER(hsm_progress, hp_cookie);
	CHECK_MEMBER(hsm_progress, hp_extent);
	CHECK_MEMBER(hsm_progress, hp_flags);
	CHECK_MEMBER(hsm_progress, hp_errval);
	CHECK_MEMBER(hsm_progress, padding);
	CHECK_DEFINE_X(HP_FLAG_COMPLETED);
	CHECK_DEFINE_X(HP_FLAG_RETRY);
}

static void
check_hsm_copy(void)
{
	BLANK_LINE();
	CHECK_MEMBER(hsm_copy, hc_data_version);
	CHECK_MEMBER(hsm_copy, hc_flags);
	CHECK_MEMBER(hsm_copy, hc_errval);
	CHECK_MEMBER(hsm_copy, padding);
	CHECK_MEMBER(hsm_copy, hc_hai);
}

static void check_layout_intent(void)
{
        BLANK_LINE();
        CHECK_STRUCT(layout_intent);
        CHECK_MEMBER(layout_intent, li_opc);
        CHECK_MEMBER(layout_intent, li_flags);
        CHECK_MEMBER(layout_intent, li_start);
        CHECK_MEMBER(layout_intent, li_end);

	CHECK_VALUE(LAYOUT_INTENT_ACCESS);
	CHECK_VALUE(LAYOUT_INTENT_READ);
	CHECK_VALUE(LAYOUT_INTENT_WRITE);
	CHECK_VALUE(LAYOUT_INTENT_GLIMPSE);
	CHECK_VALUE(LAYOUT_INTENT_TRUNC);
	CHECK_VALUE(LAYOUT_INTENT_RELEASE);
	CHECK_VALUE(LAYOUT_INTENT_RESTORE);
}

static void
check_hsm_state_set(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_state_set);
	CHECK_MEMBER(hsm_state_set, hss_valid);
	CHECK_MEMBER(hsm_state_set, hss_archive_id);
	CHECK_MEMBER(hsm_state_set, hss_setmask);
	CHECK_MEMBER(hsm_state_set, hss_clearmask);
}

static void
check_hsm_current_action(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_current_action);
	CHECK_MEMBER(hsm_current_action, hca_state);
	CHECK_MEMBER(hsm_current_action, hca_action);
	CHECK_MEMBER(hsm_current_action, hca_location);
}

static void
check_hsm_request(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_request);
	CHECK_MEMBER(hsm_request, hr_action);
	CHECK_MEMBER(hsm_request, hr_archive_id);
	CHECK_MEMBER(hsm_request, hr_flags);
	CHECK_MEMBER(hsm_request, hr_itemcount);
	CHECK_MEMBER(hsm_request, hr_data_len);
	CHECK_VALUE_X(HSM_FORCE_ACTION);
	CHECK_VALUE_X(HSM_GHOST_COPY);
}

static void
check_hsm_user_request(void)
{
	BLANK_LINE();
	CHECK_STRUCT(hsm_user_request);
	CHECK_MEMBER(hsm_user_request, hur_request);
	CHECK_MEMBER(hsm_user_request, hur_user_item);
}

static void check_update_buf(void)
{
	BLANK_LINE();
	CHECK_STRUCT(update_buf);
	CHECK_MEMBER(update_buf, ub_magic);
	CHECK_MEMBER(update_buf, ub_count);
	CHECK_MEMBER(update_buf, ub_bufs);
}

static void check_update_reply(void)
{
	BLANK_LINE();
	CHECK_STRUCT(update_reply);
	CHECK_MEMBER(update_reply, ur_version);
	CHECK_MEMBER(update_reply, ur_count);
	CHECK_MEMBER(update_reply, ur_lens);
}

static void check_update(void)
{
	BLANK_LINE();
	CHECK_STRUCT(update);
	CHECK_MEMBER(update, u_type);
	CHECK_MEMBER(update, u_batchid);
	CHECK_MEMBER(update, u_fid);
	CHECK_MEMBER(update, u_lens);
	CHECK_MEMBER(update, u_bufs);
}

static void system_string(char *cmdline, char *str, int len)
{
	int   fds[2];
	int   rc;
	pid_t pid;

	rc = pipe(fds);
	if (rc != 0)
		abort();

	pid = fork();
	if (pid == 0) {
		/* child */
		int   fd = fileno(stdout);

		rc = dup2(fds[1], fd);
		if (rc != fd)
			abort();

		exit(system(cmdline));
		/* notreached */
	} else if ((int)pid < 0) {
		abort();
	} else {
		FILE *f = fdopen(fds[0], "r");

		if (f == NULL)
			abort();

		close(fds[1]);

		if (fgets(str, len, f) == NULL)
			abort();

		if (waitpid(pid, &rc, 0) != pid)
			abort();

		if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0)
			abort();

		if (strnlen(str, len) == len)
			str[len - 1] = 0;

		if (str[strlen(str) - 1] == '\n')
			str[strlen(str) - 1] = 0;

		fclose(f);
	}
}

int
main(int argc, char **argv)
{
	char unameinfo[80];
	char gccinfo[80];

	system_string("uname -a", unameinfo, sizeof(unameinfo));
	system_string(CC " -v 2>&1 | tail -1", gccinfo, sizeof(gccinfo));

	printf ("void lustre_assert_wire_constants(void)\n"
		"{\n"
		"	 /* Wire protocol assertions generated by 'wirecheck'\n"
		"	  * (make -C lustre/utils newwiretest)\n"
		"	  * running on %s\n"
		"	  * with %s */\n"
		"\n", unameinfo, gccinfo);

	BLANK_LINE ();

	COMMENT("Constants...");
	CHECK_VALUE(PTL_RPC_MSG_REQUEST);
	CHECK_VALUE(PTL_RPC_MSG_ERR);
	CHECK_VALUE(PTL_RPC_MSG_REPLY);

	CHECK_DEFINE_64X(MDS_DIR_END_OFF);

	CHECK_DEFINE_64X(DEAD_HANDLE_MAGIC);

	CHECK_CVALUE(MTI_NAME_MAXLEN);

	CHECK_VALUE(OST_REPLY);
	CHECK_VALUE(OST_GETATTR);
	CHECK_VALUE(OST_SETATTR);
	CHECK_VALUE(OST_READ);
	CHECK_VALUE(OST_WRITE);
	CHECK_VALUE(OST_CREATE);
	CHECK_VALUE(OST_DESTROY);
	CHECK_VALUE(OST_GET_INFO);
	CHECK_VALUE(OST_CONNECT);
	CHECK_VALUE(OST_DISCONNECT);
	CHECK_VALUE(OST_PUNCH);
	CHECK_VALUE(OST_OPEN);
	CHECK_VALUE(OST_CLOSE);
	CHECK_VALUE(OST_STATFS);
	CHECK_VALUE(OST_SYNC);
	CHECK_VALUE(OST_SET_INFO);
	CHECK_VALUE(OST_QUOTACHECK);
	CHECK_VALUE(OST_QUOTACTL);
	CHECK_VALUE(OST_QUOTA_ADJUST_QUNIT);
	CHECK_VALUE(OST_LAST_OPC);

	CHECK_DEFINE_64X(OBD_OBJECT_EOF);

	CHECK_VALUE(OST_MIN_PRECREATE);
	CHECK_VALUE(OST_MAX_PRECREATE);

	CHECK_DEFINE_64X(OST_LVB_ERR_INIT);
	CHECK_DEFINE_64X(OST_LVB_ERR_MASK);

	CHECK_VALUE(MDS_FIRST_OPC);
	CHECK_VALUE(MDS_GETATTR);
	CHECK_VALUE(MDS_GETATTR_NAME);
	CHECK_VALUE(MDS_CLOSE);
	CHECK_VALUE(MDS_REINT);
	CHECK_VALUE(MDS_READPAGE);
	CHECK_VALUE(MDS_CONNECT);
	CHECK_VALUE(MDS_DISCONNECT);
	CHECK_VALUE(MDS_GETSTATUS);
	CHECK_VALUE(MDS_STATFS);
	CHECK_VALUE(MDS_PIN);
	CHECK_VALUE(MDS_UNPIN);
	CHECK_VALUE(MDS_SYNC);
	CHECK_VALUE(MDS_DONE_WRITING);
	CHECK_VALUE(MDS_SET_INFO);
	CHECK_VALUE(MDS_QUOTACHECK);
	CHECK_VALUE(MDS_QUOTACTL);
	CHECK_VALUE(MDS_GETXATTR);
	CHECK_VALUE(MDS_SETXATTR);
	CHECK_VALUE(MDS_WRITEPAGE);
	CHECK_VALUE(MDS_IS_SUBDIR);
	CHECK_VALUE(MDS_GET_INFO);
	CHECK_VALUE(MDS_HSM_STATE_GET);
	CHECK_VALUE(MDS_HSM_STATE_SET);
	CHECK_VALUE(MDS_HSM_ACTION);
	CHECK_VALUE(MDS_HSM_PROGRESS);
	CHECK_VALUE(MDS_HSM_REQUEST);
	CHECK_VALUE(MDS_HSM_CT_REGISTER);
	CHECK_VALUE(MDS_HSM_CT_UNREGISTER);
	CHECK_VALUE(MDS_SWAP_LAYOUTS);
	CHECK_VALUE(MDS_LAST_OPC);

	CHECK_VALUE(REINT_SETATTR);
	CHECK_VALUE(REINT_CREATE);
	CHECK_VALUE(REINT_LINK);
	CHECK_VALUE(REINT_UNLINK);
	CHECK_VALUE(REINT_RENAME);
	CHECK_VALUE(REINT_OPEN);
	CHECK_VALUE(REINT_SETXATTR);
	CHECK_VALUE(REINT_RMENTRY);
	CHECK_VALUE(REINT_MAX);

	CHECK_VALUE_X(DISP_IT_EXECD);
	CHECK_VALUE_X(DISP_LOOKUP_EXECD);
	CHECK_VALUE_X(DISP_LOOKUP_NEG);
	CHECK_VALUE_X(DISP_LOOKUP_POS);
	CHECK_VALUE_X(DISP_OPEN_CREATE);
	CHECK_VALUE_X(DISP_OPEN_OPEN);
	CHECK_VALUE_X(DISP_ENQ_COMPLETE);
	CHECK_VALUE_X(DISP_ENQ_OPEN_REF);
	CHECK_VALUE_X(DISP_ENQ_CREATE_REF);
	CHECK_VALUE_X(DISP_OPEN_LOCK);

	CHECK_VALUE(MDS_STATUS_CONN);
	CHECK_VALUE(MDS_STATUS_LOV);

	CHECK_VALUE(LUSTRE_BFLAG_UNCOMMITTED_WRITES);

	CHECK_VALUE_X(MF_SOM_CHANGE);
	CHECK_VALUE_X(MF_EPOCH_OPEN);
	CHECK_VALUE_X(MF_EPOCH_CLOSE);
	CHECK_VALUE_X(MF_MDC_CANCEL_FID1);
	CHECK_VALUE_X(MF_MDC_CANCEL_FID2);
	CHECK_VALUE_X(MF_MDC_CANCEL_FID3);
	CHECK_VALUE_X(MF_MDC_CANCEL_FID4);
	CHECK_VALUE_X(MF_SOM_AU);
	CHECK_VALUE_X(MF_GETATTR_LOCK);

	CHECK_VALUE_64X(MDS_ATTR_MODE);
	CHECK_VALUE_64X(MDS_ATTR_UID);
	CHECK_VALUE_64X(MDS_ATTR_GID);
	CHECK_VALUE_64X(MDS_ATTR_SIZE);
	CHECK_VALUE_64X(MDS_ATTR_ATIME);
	CHECK_VALUE_64X(MDS_ATTR_MTIME);
	CHECK_VALUE_64X(MDS_ATTR_CTIME);
	CHECK_VALUE_64X(MDS_ATTR_ATIME_SET);
	CHECK_VALUE_64X(MDS_ATTR_MTIME_SET);
	CHECK_VALUE_64X(MDS_ATTR_FORCE);
	CHECK_VALUE_64X(MDS_ATTR_ATTR_FLAG);
	CHECK_VALUE_64X(MDS_ATTR_KILL_SUID);
	CHECK_VALUE_64X(MDS_ATTR_KILL_SGID);
	CHECK_VALUE_64X(MDS_ATTR_CTIME_SET);
	CHECK_VALUE_64X(MDS_ATTR_FROM_OPEN);
	CHECK_VALUE_64X(MDS_ATTR_BLOCKS);

	CHECK_VALUE(FLD_QUERY);
	CHECK_VALUE(FLD_FIRST_OPC);
	CHECK_VALUE(FLD_LAST_OPC);

	CHECK_VALUE(SEQ_QUERY);
	CHECK_VALUE(SEQ_FIRST_OPC);
	CHECK_VALUE(SEQ_LAST_OPC);

	CHECK_VALUE(SEQ_ALLOC_SUPER);
	CHECK_VALUE(SEQ_ALLOC_META);

	CHECK_VALUE(LDLM_ENQUEUE);
	CHECK_VALUE(LDLM_CONVERT);
	CHECK_VALUE(LDLM_CANCEL);
	CHECK_VALUE(LDLM_BL_CALLBACK);
	CHECK_VALUE(LDLM_CP_CALLBACK);
	CHECK_VALUE(LDLM_GL_CALLBACK);
	CHECK_VALUE(LDLM_SET_INFO);
	CHECK_VALUE(LDLM_LAST_OPC);

	CHECK_VALUE(LCK_MINMODE);
	CHECK_VALUE(LCK_EX);
	CHECK_VALUE(LCK_PW);
	CHECK_VALUE(LCK_PR);
	CHECK_VALUE(LCK_CW);
	CHECK_VALUE(LCK_CR);
	CHECK_VALUE(LCK_NL);
	CHECK_VALUE(LCK_GROUP);
	CHECK_VALUE(LCK_COS);
	CHECK_VALUE(LCK_MAXMODE);
	CHECK_VALUE(LCK_MODE_NUM);

	CHECK_CVALUE(LDLM_PLAIN);
	CHECK_CVALUE(LDLM_EXTENT);
	CHECK_CVALUE(LDLM_FLOCK);
	CHECK_CVALUE(LDLM_IBITS);
	CHECK_CVALUE(LDLM_MAX_TYPE);

	CHECK_CVALUE(LUSTRE_RES_ID_SEQ_OFF);
	CHECK_CVALUE(LUSTRE_RES_ID_VER_OID_OFF);
	/* CHECK_CVALUE(LUSTRE_RES_ID_WAS_VER_OFF); packed with OID */

	CHECK_VALUE(UPDATE_OBJ);
	CHECK_VALUE(UPDATE_LAST_OPC);
	CHECK_CVALUE(LUSTRE_RES_ID_QUOTA_SEQ_OFF);
	CHECK_CVALUE(LUSTRE_RES_ID_QUOTA_VER_OID_OFF);
	CHECK_CVALUE(LUSTRE_RES_ID_HSH_OFF);

	CHECK_CVALUE(LQUOTA_TYPE_USR);
	CHECK_CVALUE(LQUOTA_TYPE_GRP);

	CHECK_CVALUE(LQUOTA_RES_MD);
	CHECK_CVALUE(LQUOTA_RES_DT);

	CHECK_VALUE(OBD_PING);
	CHECK_VALUE(OBD_LOG_CANCEL);
	CHECK_VALUE(OBD_QC_CALLBACK);
	CHECK_VALUE(OBD_IDX_READ);
	CHECK_VALUE(OBD_LAST_OPC);

	CHECK_VALUE(QUOTA_DQACQ);
	CHECK_VALUE(QUOTA_DQREL);
	CHECK_VALUE(QUOTA_LAST_OPC);

	CHECK_VALUE(MGS_CONNECT);
	CHECK_VALUE(MGS_DISCONNECT);
	CHECK_VALUE(MGS_EXCEPTION);
	CHECK_VALUE(MGS_TARGET_REG);
	CHECK_VALUE(MGS_TARGET_DEL);
	CHECK_VALUE(MGS_SET_INFO);
	CHECK_VALUE(MGS_LAST_OPC);

	CHECK_VALUE(SEC_CTX_INIT);
	CHECK_VALUE(SEC_CTX_INIT_CONT);
	CHECK_VALUE(SEC_CTX_FINI);
	CHECK_VALUE(SEC_LAST_OPC);

	COMMENT("Sizes and Offsets");
	BLANK_LINE();
	CHECK_STRUCT(obd_uuid);
	check_lu_seq_range();
	check_lustre_mdt_attrs();

	CHECK_VALUE(OBJ_CREATE);
	CHECK_VALUE(OBJ_DESTROY);
	CHECK_VALUE(OBJ_REF_ADD);
	CHECK_VALUE(OBJ_REF_DEL);
	CHECK_VALUE(OBJ_ATTR_SET);
	CHECK_VALUE(OBJ_ATTR_GET);
	CHECK_VALUE(OBJ_XATTR_SET);
	CHECK_VALUE(OBJ_XATTR_GET);
	CHECK_VALUE(OBJ_INDEX_LOOKUP);
	CHECK_VALUE(OBJ_INDEX_LOOKUP);
	CHECK_VALUE(OBJ_INDEX_INSERT);
	CHECK_VALUE(OBJ_INDEX_DELETE);

	check_som_attrs();
	check_hsm_attrs();
	check_ost_id();
	check_lu_dirent();
	check_luda_type();
	check_lu_dirpage();
	check_lustre_handle();
	check_lustre_msg_v2();
	check_ptlrpc_body();
	check_obd_connect_data();
	check_obdo();
	check_lov_ost_data_v1();
	check_lov_mds_md_v1();
	check_lov_mds_md_v3();
	check_obd_statfs();
	check_obd_ioobj();
	check_obd_quotactl();
	check_obd_idx_read();
	check_niobuf_remote();
	check_ost_body();
	check_ll_fid();
	check_mdt_body();
	check_mdt_ioepoch();
	check_mdt_remote_perm();
	check_mdt_rec_setattr();
	check_mdt_rec_create();
	check_mdt_rec_link();
	check_mdt_rec_unlink();
	check_mdt_rec_rename();
	check_mdt_rec_setxattr();
	check_mdt_rec_reint();
	check_lmv_desc();
	check_lmv_stripe_md();
	check_lov_desc();
	check_ldlm_res_id();
	check_ldlm_extent();
	check_ldlm_inodebits();
	check_ldlm_flock();
	check_ldlm_intent();
	check_ldlm_resource_desc();
	check_ldlm_lock_desc();
	check_ldlm_request();
	check_ldlm_reply();
	check_ldlm_ost_lvb_v1();
	check_ldlm_ost_lvb();
	check_ldlm_lquota_lvb();
	check_ldlm_gl_lquota_desc();
	check_mgs_send_param();
	check_cfg_marker();
	check_llog_logid();
	check_llog_catid();
	check_llog_rec_hdr();
	check_llog_rec_tail();
	check_llog_logid_rec();
	check_llog_unlink_rec();
	check_llog_unlink64_rec();
	check_llog_setattr64_rec();
	check_llog_size_change_rec();
	check_changelog_rec();
	check_changelog_rec_ext();
	check_changelog_setinfo();
	check_llog_changelog_rec();
	check_llog_changelog_user_rec();
	check_llog_gen();
	check_llog_gen_rec();
	check_llog_log_hdr();
	check_llog_cookie();
	check_llogd_body();
	check_llogd_conn_body();
	check_ll_fiemap_info_key();
	check_quota_body();
	check_mgs_target_info();
	check_lustre_capa();
	check_lustre_capa_key();
	check_getinfo_fid2path();
	check_ll_user_fiemap();
	check_ll_fiemap_extent();
	printf("#ifdef LIBLUSTRE_POSIX_ACL\n");
#ifndef LIBLUSTRE_POSIX_ACL
#error build generator without LIBLUSTRE_POSIX_ACL defined - produce wrong check code.
#endif
	check_posix_acl_xattr_entry();
	check_posix_acl_xattr_header();
	printf("#endif\n");
	check_link_ea_header();
	check_link_ea_entry();
	check_layout_intent();
	check_hsm_action_item();
	check_hsm_action_list();
	check_hsm_progress();
	check_hsm_copy();
	check_hsm_progress_kernel();
	check_hsm_user_item();
	check_hsm_user_state();
	check_hsm_state_set();
	check_hsm_current_action();
	check_hsm_request();
	check_hsm_user_request();

	check_update_buf();
	check_update_reply();
	check_update();

	printf("}\n\n");

	return 0;
}
