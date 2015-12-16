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
 *
 * Copyright (c) 2011, 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */
/** \defgroup llog_reader Lustre Log Reader
 *
 * Interpret llogs used for storing configuration and changelog data
 *
 * @{
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/vfs.h>
#include <linux/magic.h>

#include <time.h>
#include <liblustre.h>
#include <lustre/lustre_idl.h>
#include <lustre_log_user.h>

int llog_pack_buffer(int fd, struct llog_log_hdr **llog_buf,
                     struct llog_rec_hdr ***recs, int *recs_number);

void print_llog_header(struct llog_log_hdr *llog_buf);
static void print_records(struct llog_rec_hdr **recs_buf,
			  int rec_number, int is_ext);
void llog_unpack_buffer(int fd, struct llog_log_hdr *llog_buf,
                        struct llog_rec_hdr **recs_buf);

#define CANCELLED 0x678

#define PTL_CMD_BASE 100
char* portals_command[17]=
{
        "REGISTER_PEER_FD",
        "CLOSE_CONNECTION",
        "REGISTER_MYNID",
        "PUSH_CONNECTION",
        "GET_CONN",
        "DEL_PEER",
        "ADD_PEER",
        "GET_PEER",
        "GET_TXDESC",
        "ADD_ROUTE",
        "DEL_ROUTE",
        "GET_ROUTE",
        "NOTIFY_ROUTER",
        "ADD_INTERFACE",
        "DEL_INTERFACE",
        "GET_INTERFACE",
        ""
};

int is_fstype_ext(int fd)
{
	struct statfs		st;
	int			rc;

	rc = fstatfs(fd, &st);
	if (rc < 0) {
		printf("Got statfs error: %d", rc);
		return -errno;
	}

	return (st.f_type == EXT4_SUPER_MAGIC);
}


/**
 * Attempt to display a path to the object (file) containing changelog entries,
 * referred to by this changelog_catalog record.
 *
 * This path depends on the implementation of the OSD device; zfs-osd and
 * ldiskfs-osd are different.
 *
 * Assumes that if the file system containing the changelog_catalog is
 * ext{2,3,4}, the backend is ldiskfs-osd; otherwise it is either zfs-osd or at
 * least names objects based on FID and the zfs-osd path (which includes the
 * FID) will be sufficient.
 *
 * The Object ID stored in the record is also displayed untranslated.
 */
#define OSD_OI_FID_NR         (1UL << 7)
static void print_log_path(struct llog_logid_rec *lid, int is_ext)
{

	char			object_path[255];
	struct lu_fid		fid_from_logid;

	logid_to_fid(&lid->lid_id, &fid_from_logid);

	if (is_ext)
		snprintf(object_path, sizeof(object_path),
			 "O/"LPU64"/d%u/%u", fid_from_logid.f_seq,
			 fid_from_logid.f_oid % 32, fid_from_logid.f_oid);
	else
		snprintf(object_path, sizeof(object_path),
			 "oi."LPU64"/"DFID_NOBRACE,
			 fid_from_logid.f_seq & (OSD_OI_FID_NR - 1) ,
			 PFID(&fid_from_logid));

	printf("ogen=%X id="DOSTID" path=%s\n",
		lid->lid_id.lgl_ogen, POSTID(&lid->lid_id.lgl_oi),
		object_path);
}

int main(int argc, char **argv)
{
	int rc = 0;
	int is_ext;
	int fd, rec_number;
	struct llog_log_hdr *llog_buf = NULL;
	struct llog_rec_hdr **recs_buf = NULL;

	setlinebuf(stdout);

	if (argc != 2) {
		printf("Usage: llog_reader filename\n");
		return -1;
	}

        fd = open(argv[1],O_RDONLY);
        if (fd < 0){
                printf("Could not open the file %s\n", argv[1]);
                goto out;
        }

	is_ext = is_fstype_ext(fd);
	if (is_ext < 0) {
		printf("Unable to determine type of filesystem containing %s\n",
		       argv[1]);
		goto out;
	}

        rc = llog_pack_buffer(fd, &llog_buf, &recs_buf, &rec_number);
        if (rc < 0) {
                printf("Could not pack buffer; rc=%d\n", rc);
                goto out_fd;
        }

	print_llog_header(llog_buf);
	print_records(recs_buf, rec_number, is_ext);
	llog_unpack_buffer(fd, llog_buf, recs_buf);

out_fd:
	close(fd);
out:
	return rc;
}



int llog_pack_buffer(int fd, struct llog_log_hdr **llog,
		     struct llog_rec_hdr ***recs,
		     int *recs_number)
{
        int rc = 0, recs_num,rd;
        off_t file_size;
        struct stat st;
        char *file_buf=NULL, *recs_buf=NULL;
        struct llog_rec_hdr **recs_pr=NULL;
        char *ptr=NULL;
        int i;

        rc = fstat(fd,&st);
        if (rc < 0){
                printf("Get file stat error.\n");
                goto out;
        }
        file_size = st.st_size;

        file_buf = malloc(file_size);
        if (file_buf == NULL){
                printf("Memory Alloc for file_buf error.\n");
                rc = -ENOMEM;
                goto out;
        }
        *llog = (struct llog_log_hdr*)file_buf;

        rd = read(fd,file_buf,file_size);
        if (rd < file_size){
                printf("Read file error.\n");
                rc = -EIO; /*FIXME*/
                goto clear_file_buf;
        }

	/* the llog header not countable here.*/
	recs_num = le32_to_cpu((*llog)->llh_count) - 1;

        recs_buf = malloc(recs_num * sizeof(struct llog_rec_hdr *));
        if (recs_buf == NULL){
                printf("Memory Alloc for recs_buf error.\n");
                rc = -ENOMEM;
                goto clear_file_buf;
        }
        recs_pr = (struct llog_rec_hdr **)recs_buf;

	ptr = file_buf + le32_to_cpu((*llog)->llh_hdr.lrh_len);
	i = 0;

        while (i < recs_num){
                struct llog_rec_hdr *cur_rec = (struct llog_rec_hdr*)ptr;
                int idx = le32_to_cpu(cur_rec->lrh_index);
                recs_pr[i] = cur_rec;

                if (ext2_test_bit(idx, (*llog)->llh_bitmap)) {
			printf("rec #%d type=%x len=%u\n", idx,
			       cur_rec->lrh_type, cur_rec->lrh_len);
                } else {
                        printf("Bit %d of %d not set\n", idx, recs_num);
                        cur_rec->lrh_id = CANCELLED;
                        /* The header counts only set records */
                        i--;
                }

		ptr += le32_to_cpu(cur_rec->lrh_len);
		if ((ptr - file_buf) > file_size) {
			printf("The log is corrupt (too big at %d)\n", i);
			rc = -EINVAL;
			goto clear_recs_buf;
		}
		i++;
	}

	*recs = recs_pr;
	*recs_number = recs_num;

out:
	return rc;

clear_recs_buf:
	free(recs_buf);

clear_file_buf:
	free(file_buf);

	*llog = NULL;
	goto out;
}

void llog_unpack_buffer(int fd, struct llog_log_hdr *llog_buf,
                        struct llog_rec_hdr **recs_buf)
{
        free(llog_buf);
        free(recs_buf);
        return;
}

void print_llog_header(struct llog_log_hdr *llog_buf)
{
        time_t t;

        printf("Header size : %u\n",
               le32_to_cpu(llog_buf->llh_hdr.lrh_len));

        t = le64_to_cpu(llog_buf->llh_timestamp);
        printf("Time : %s", ctime(&t));

        printf("Number of records: %u\n",
               le32_to_cpu(llog_buf->llh_count)-1);

        printf("Target uuid : %s \n",
               (char *)(&llog_buf->llh_tgtuuid));

        /* Add the other info you want to view here */

        printf("-----------------------\n");
        return;
}

static void print_1_cfg(struct lustre_cfg *lcfg)
{
        int i;

        if (lcfg->lcfg_nid)
                printf("nid=%s("LPX64")  ", libcfs_nid2str(lcfg->lcfg_nid),
                       lcfg->lcfg_nid);
        if (lcfg->lcfg_nal)
                printf("nal=%d ", lcfg->lcfg_nal);
        for (i = 0; i <  lcfg->lcfg_bufcount; i++)
                printf("%d:%.*s  ", i, lcfg->lcfg_buflens[i],
                       (char*)lustre_cfg_buf(lcfg, i));
        return;
}


static void print_setup_cfg(struct lustre_cfg *lcfg)
{
        struct lov_desc *desc;

        if ((lcfg->lcfg_bufcount == 2) &&
            (lcfg->lcfg_buflens[1] == sizeof(*desc))) {
                printf("lov_setup ");
                printf("0:%s  ", lustre_cfg_string(lcfg, 0));
                printf("1:(struct lov_desc)\n");
                desc = (struct lov_desc*)(lustre_cfg_string(lcfg, 1));
                printf("\t\tuuid=%s  ", (char*)desc->ld_uuid.uuid);
                printf("stripe:cnt=%u ", desc->ld_default_stripe_count);
                printf("size="LPU64" ", desc->ld_default_stripe_size);
                printf("offset="LPU64" ", desc->ld_default_stripe_offset);
                printf("pattern=%#x", desc->ld_pattern);
        } else {
                printf("setup     ");
                print_1_cfg(lcfg);
        }

        return;
}

void print_lustre_cfg(struct lustre_cfg *lcfg, int *skip)
{
        enum lcfg_command_type cmd = le32_to_cpu(lcfg->lcfg_command);

        if (*skip > 0)
                printf("SKIP ");

        switch(cmd){
        case(LCFG_ATTACH):{
                printf("attach    ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_SETUP):{
                print_setup_cfg(lcfg);
                break;
        }
        case(LCFG_DETACH):{
                printf("detach    ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_CLEANUP):{
                printf("cleanup   ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_ADD_UUID):{
                printf("add_uuid  ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_DEL_UUID):{
                printf("del_uuid  ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_ADD_CONN):{
                printf("add_conn  ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_DEL_CONN):{
                printf("del_conn  ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_LOV_ADD_OBD):{
                printf("lov_modify_tgts add ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_LOV_DEL_OBD):{
                printf("lov_modify_tgts del ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_ADD_MDC):{
                printf("modify_mdc_tgts add ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_DEL_MDC):{
                printf("modify_mdc_tgts del ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_MOUNTOPT):{
                printf("mount_option ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_DEL_MOUNTOPT):{
                printf("del_mount_option ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_SET_TIMEOUT):{
                printf("set_timeout=%d ", lcfg->lcfg_num);
                break;
        }
        case(LCFG_SET_LDLM_TIMEOUT):{
                printf("set_ldlm_timeout=%d ", lcfg->lcfg_num);
                break;
        }
        case(LCFG_SET_UPCALL):{
                printf("set_lustre_upcall ");
                print_1_cfg(lcfg);
                break;
        }
	case(LCFG_PARAM):{
		printf("param ");
		print_1_cfg(lcfg);
		break;
	}
	case(LCFG_SET_PARAM):{
		printf("set_param ");
		print_1_cfg(lcfg);
		break;
	}
        case(LCFG_SPTLRPC_CONF):{
                printf("sptlrpc_conf ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_MARKER):{
                struct cfg_marker *marker = lustre_cfg_buf(lcfg, 1);
                char createtime[26], canceltime[26] = "";
                time_t time_tmp;

                if (marker->cm_flags & CM_SKIP) {
                        if (marker->cm_flags & CM_START) {
                                printf("SKIP START ");
                                (*skip)++;
                        } else {
                                printf(     "END   ");
                                *skip = 0;
                        }
                }

                if (marker->cm_flags & CM_EXCLUDE) {
                        if (marker->cm_flags & CM_START)
                                printf("EXCLUDE START ");
                        else
                                printf("EXCLUDE END   ");
                }

                /* Handle overflow of 32-bit time_t gracefully.
                 * The copy to time_tmp is needed in any case to
                 * keep the pointer happy, even on 64-bit systems. */
                time_tmp = marker->cm_createtime;
                if (time_tmp == marker->cm_createtime) {
                        ctime_r(&time_tmp, createtime);
                        createtime[strlen(createtime) - 1] = 0;
                } else {
                        strcpy(createtime, "in the distant future");
                }

                if (marker->cm_canceltime) {
                        /* Like cm_createtime, we try to handle overflow of
                         * 32-bit time_t gracefully. The copy to time_tmp
                         * is also needed on 64-bit systems to keep the
                         * pointer happy, see bug 16771 */
                        time_tmp = marker->cm_canceltime;
                        if (time_tmp == marker->cm_canceltime) {
                                ctime_r(&time_tmp, canceltime);
                                canceltime[strlen(canceltime) - 1] = 0;
                        } else {
                                strcpy(canceltime, "in the distant future");
                        }
                }

                printf("marker %3d (flags=%#04x, v%d.%d.%d.%d) %-15s '%s' %s-%s",
                       marker->cm_step, marker->cm_flags,
                       OBD_OCD_VERSION_MAJOR(marker->cm_vers),
                       OBD_OCD_VERSION_MINOR(marker->cm_vers),
                       OBD_OCD_VERSION_PATCH(marker->cm_vers),
                       OBD_OCD_VERSION_FIX(marker->cm_vers),
                       marker->cm_tgtname, marker->cm_comment,
                       createtime, canceltime);
                break;
        }
        case(LCFG_POOL_NEW):{
                printf("pool new ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_POOL_ADD):{
                printf("pool add ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_POOL_REM):{
                printf("pool remove ");
                print_1_cfg(lcfg);
                break;
        }
        case(LCFG_POOL_DEL):{
                printf("pool destroy ");
                print_1_cfg(lcfg);
                break;
        }
        default:
                printf("unsupported cmd_code = %x\n",cmd);
        }
        printf("\n");
        return;
}

static void print_hsm_action(struct llog_agent_req_rec *larr)
{
	char	buf[12];
	int	sz;

	sz = larr->arr_hai.hai_len - sizeof(larr->arr_hai);
	printf("lrh=[type=%X len=%d idx=%d] fid="DFID
	       " compound/cookie="LPX64"/"LPX64
	       " status=%s action=%s archive#=%d flags="LPX64
	       " create="LPU64" change="LPU64
	       " extent="LPX64"-"LPX64" gid="LPX64" datalen=%d"
	       " data=[%s]\n",
	       larr->arr_hdr.lrh_type,
	       larr->arr_hdr.lrh_len, larr->arr_hdr.lrh_index,
	       PFID(&larr->arr_hai.hai_fid),
	       larr->arr_compound_id, larr->arr_hai.hai_cookie,
	       agent_req_status2name(larr->arr_status),
	       hsm_copytool_action2name(larr->arr_hai.hai_action),
	       larr->arr_archive_id,
	       larr->arr_flags,
	       larr->arr_req_create, larr->arr_req_change,
	       larr->arr_hai.hai_extent.offset,
	       larr->arr_hai.hai_extent.length,
	       larr->arr_hai.hai_gid, sz,
	       hai_dump_data_field(&larr->arr_hai, buf, sizeof(buf)));
}

void print_changelog_rec(struct llog_changelog_rec *rec)
{
	printf("changelog record id:0x%x cr_flags:0x%x cr_type:%s(0x%x)\n",
	       le32_to_cpu(rec->cr_hdr.lrh_id),
	       le32_to_cpu(rec->cr.cr_flags),
	       changelog_type2str(le32_to_cpu(rec->cr.cr_type)),
	       le32_to_cpu(rec->cr.cr_type));
}

static void print_records(struct llog_rec_hdr **recs,
			  int rec_number, int is_ext)
{
	__u32 lopt;
	int i, skip = 0;

	for (i = 0; i < rec_number; i++) {
		printf("#%.2d (%.3d)", le32_to_cpu(recs[i]->lrh_index),
		       le32_to_cpu(recs[i]->lrh_len));

		lopt = le32_to_cpu(recs[i]->lrh_type);

		if (recs[i]->lrh_id == CANCELLED)
			printf("NOT SET ");

		switch (lopt) {
		case OBD_CFG_REC:
			print_lustre_cfg(
				(struct lustre_cfg *)((char *)(recs[i]) +
				sizeof(struct llog_rec_hdr)), &skip);
			break;
		case LLOG_PAD_MAGIC:
			printf("padding\n");
			break;
		case LLOG_LOGID_MAGIC:
			print_log_path((struct llog_logid_rec *)recs[i],
				       is_ext);
			break;
		case HSM_AGENT_REC:
			print_hsm_action((struct llog_agent_req_rec *)recs[i]);
			break;
		case CHANGELOG_REC:
			print_changelog_rec((struct llog_changelog_rec *)
					    recs[i]);
			break;
		case CHANGELOG_USER_REC:
			printf("changelog_user record id:0x%x\n",
			       le32_to_cpu(recs[i]->lrh_id));
			break;
		default:
			printf("unknown type %x\n", lopt);
			break;
		}
	}
}

/** @} llog_reader */
