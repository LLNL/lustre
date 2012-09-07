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
 * Copyright (c) 2012, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/osd/osd_oi.h
 *
 * OSD Object Index
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 */

/*
 * Object Index (oi) service runs in the bottom layer of server stack. In
 * translates fid local to this service to the storage cookie that uniquely
 * and efficiently identifies object (inode) of the underlying file system.
 */

#ifndef _OSD_OI_H
#define _OSD_OI_H

#if defined(__KERNEL__)

/* struct rw_semaphore */
#include <linux/rwsem.h>
#include <lustre_fid.h>
#include <lu_object.h>
#include <md_object.h>

#define OSD_OI_FID_NR         (1UL << OSD_OI_FID_OID_BITS)
#define OSD_OI_FID_NR_MAX     (1UL << OSD_OI_FID_OID_BITS_MAX)

#define OSD_OII_NOGEN (0)

struct lu_fid;
struct osd_thread_info;
struct lu_site;
struct thandle;
struct osd_oi;

struct dt_device;
struct osd_device;
struct osd_oi;

/*
 * Storage cookie. Datum uniquely identifying inode on the underlying file
 * system.
 *
 * osd_inode_id is the internal ldiskfs identifier for an object. It should
 * not be visible outside of the osd-ldiskfs. Other OSDs may have different
 * identifiers, so this cannot form any part of the OSD API.
 */
struct osd_inode_id {
	__u32 oii_ino; /* inode number */
	__u32 oii_gen; /* inode generation */
};

struct osd_idmap_cache {
	struct lu_fid		oic_fid;
	struct osd_inode_id	oic_lid;
};

/* FID is supposed to have name idmap able */
static inline int fid_has_name(const struct lu_fid *fid)
{
	return (fid_is_norm(fid) || FID_SEQ_LOCAL_NAME == fid_seq(fid) ||
		FID_SEQ_DOT_LUSTRE == fid_seq(fid));
}

static inline void osd_id_pack(struct osd_inode_id *tgt,
			       const struct osd_inode_id *src)
{
	tgt->oii_ino = cpu_to_be32(src->oii_ino);
	tgt->oii_gen = cpu_to_be32(src->oii_gen);
}

static inline void osd_id_unpack(struct osd_inode_id *tgt,
				 struct osd_inode_id *src)
{
	tgt->oii_ino = be32_to_cpu(src->oii_ino);
	tgt->oii_gen = be32_to_cpu(src->oii_gen);
}

static inline void osd_id_gen(struct osd_inode_id *id, __u32 ino, __u32 gen)
{
	id->oii_ino = ino;
	id->oii_gen = gen;
}

static inline void osd_id_to_inode(struct inode *inode,
				   const struct osd_inode_id *id)
{
	inode->i_ino        = id->oii_ino;
	inode->i_generation = id->oii_gen;
}

static inline int osd_id_eq(const struct osd_inode_id *id0,
			    const struct osd_inode_id *id1)
{
	return (id0->oii_ino == id1->oii_ino) &&
	       (id0->oii_gen == id1->oii_gen ||
		id0->oii_gen == OSD_OII_NOGEN ||
		id1->oii_gen == OSD_OII_NOGEN);
}

int osd_oi_mod_init(void);
int osd_oi_init(struct osd_thread_info *info, struct osd_device *osd);
void osd_oi_fini(struct osd_thread_info *info, struct osd_device *osd);
int __osd_oi_lookup(struct osd_thread_info *info, struct osd_device *osd,
		    const struct lu_fid *fid, struct osd_inode_id *id);
int  osd_oi_lookup(struct osd_thread_info *info, struct osd_device *osd,
		   const struct lu_fid *fid, struct osd_inode_id *id);
int  osd_oi_insert(struct osd_thread_info *info, struct osd_device *osd,
		   const struct lu_fid *fid, const struct osd_inode_id *id,
		   struct thandle *th);
int  osd_oi_delete(struct osd_thread_info *info,
		   struct osd_device *osd, const struct lu_fid *fid,
		   struct thandle *th);

#endif /* __KERNEL__ */
#endif /* _OSD_OI_H */
