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
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/osd/osd_compat.c
 *
 * on-disk compatibility stuff for OST
 *
 */

/* LUSTRE_VERSION_CODE */
#include <lustre_ver.h>
/* prerequisite for linux/xattr.h */
#include <linux/types.h>
/* prerequisite for linux/xattr.h */
#include <linux/fs.h>

/*
 * struct OBD_{ALLOC,FREE}*()
 * OBD_FAIL_CHECK
 */
#include <obd_support.h>
/* struct ptlrpc_thread */
#include <lustre_net.h>

/* fid_is_local() */
#include <lustre_fid.h>

#include "osd_internal.h"
#include <lvfs.h>

struct osd_compat_objid_group {
        cfs_semaphore_t        dir_init_sem; /* protects on-fly initialization */
        struct osd_inode_id    last_id;      /* file storing last created objid */
        struct dentry         *groot;        /* O/<group> */
        struct dentry        **dirs;         /* O/<group>/d0-dXX */
};

#define MAX_OBJID_GROUP (FID_SEQ_OST_MAX - 1)

struct osd_compat_objid {
        int                             subdir_count;
        struct dentry                  *root;
        struct osd_inode_id             last_rcvd_id;
        struct osd_inode_id             last_group_id;
        struct osd_compat_objid_group   groups[MAX_OBJID_GROUP];
};



static void osd_push_ctxt(const struct osd_device *dev,
                          struct lvfs_run_ctxt *newctxt,
                          struct lvfs_run_ctxt *save)
{
        OBD_SET_CTXT_MAGIC(newctxt);
        newctxt->pwdmnt = dev->od_mnt;
        newctxt->pwd = dev->od_mnt->mnt_root;
        newctxt->fs = get_ds();

        push_ctxt(save, newctxt, NULL);
}

/*
 * directory structure on legacy OST:
 *
 * O/<group>/d0-31/<objid>
 * O/<group>/LAST_ID
 * last_rcvd
 * LAST_GROUP
 * CONFIGS
 *
 */

int osd_ost_init(struct osd_device *dev)
{
        struct lvfs_run_ctxt  new;
        struct lvfs_run_ctxt  save;
        struct dentry        *rootd = osd_sb(dev)->s_root;
        struct dentry        *d;
        int                   rc = 0;
        int                   i; 

        OBD_ALLOC_PTR(dev->od_ost_map);
        if (dev->od_ost_map == NULL)
                RETURN(-ENOMEM);

        /* XXX: to be initialized from last_rcvd */
        dev->od_ost_map->subdir_count = 32;
        for (i = 0; i < MAX_OBJID_GROUP; i++)
                cfs_sema_init(&dev->od_ost_map->groups[i].dir_init_sem, 1);

        LASSERT(dev->od_fsops);

        osd_push_ctxt(dev, &new, &save);

        d = simple_mkdir(rootd, dev->od_mnt, "O", 0755, 1);
        if (IS_ERR(d))
                GOTO(cleanup, rc = PTR_ERR(d));

        dev->od_ost_map->root = d;

cleanup:
        pop_ctxt(&save, &new, NULL);


        RETURN(rc);
}

int osd_compat_init(struct osd_device *dev)
{
        int rc;
        ENTRY;

        /* prepare structures for OST */
        rc = osd_ost_init(dev);

        /* prepare structures for MDS */

        RETURN(rc);
}

void osd_compat_fini(struct osd_device *dev)
{
        struct osd_compat_objid_group *grp;
        struct osd_compat_objid       *map = dev->od_ost_map;
        int                            i, j;
        ENTRY;

        map = dev->od_ost_map;
        if (map == NULL)
                return;

        for (i = 0; i < MAX_OBJID_GROUP; i++) {
                grp = map->groups + i;
                if (grp == NULL)
                        continue;

                if (grp->groot)
                        dput(grp->groot);

                if (grp->dirs) {
                        for (j = 0; j < map->subdir_count; j++) {
                                if (grp->dirs[j])
                                        dput(grp->dirs[j]);
                        }
                        OBD_FREE(grp->dirs,
                                 sizeof(struct dentry *) * map->subdir_count);
                }
        }
        if (map->root)
                dput(map->root);
        OBD_FREE_PTR(dev->od_ost_map);

        EXIT;
}

int osd_compat_del_entry(struct osd_thread_info *info,
                         struct osd_device *osd,
                         struct dentry *dird, char *name,
                         struct thandle *th)
{
	struct ldiskfs_dir_entry_2 *de;
        struct buffer_head         *bh;
        struct osd_thandle         *oh;
        struct dentry              *child;
        struct inode               *dir = dird->d_inode;
        int                         rc;
        ENTRY;

        oh = container_of(th, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle != NULL);
        LASSERT(oh->ot_handle->h_transaction != NULL);


        child = &info->oti_child_dentry;
        child->d_name.hash = 0;
        child->d_name.name = name;
        child->d_name.len = strlen(name);
        child->d_parent = dird;
        child->d_inode = NULL;

        LOCK_INODE_MUTEX(dir);
        rc = -ENOENT;
        bh = osd_ldiskfs_find_entry(dir, child, &de, NULL);
        if (bh) {
                rc = ldiskfs_delete_entry(oh->ot_handle, dir, de, bh);
                brelse(bh);
        }
        UNLOCK_INODE_MUTEX(dir);

        RETURN(rc);
}

int osd_compat_add_entry(struct osd_thread_info *info,
                         struct osd_device *osd,
                         struct dentry *dir, char *name,
                         const struct osd_inode_id *id,
                         struct thandle *th)
{
        struct osd_thandle *oh;
        struct dentry *child;
        struct inode *inode;
        int rc;
        ENTRY;

        oh = container_of(th, struct osd_thandle, ot_super);
        LASSERT(oh->ot_handle != NULL);
        LASSERT(oh->ot_handle->h_transaction != NULL);

        inode = &info->oti_inode;
        inode->i_sb = osd_sb(osd);
        inode->i_ino = id->oii_ino;
        inode->i_generation = id->oii_gen;

        child = &info->oti_child_dentry;
        child->d_name.hash = 0;
        child->d_name.name = name;
        child->d_name.len = strlen(name);
        child->d_parent = dir;
        child->d_inode = inode;

        LOCK_INODE_MUTEX(dir->d_inode);
        rc = osd_ldiskfs_add_entry(oh->ot_handle, child, inode, NULL);
        UNLOCK_INODE_MUTEX(dir->d_inode);

        RETURN(rc);
}

/* external locking is required */
int __osd_compat_load_group(struct osd_device *osd, int group)
{
        struct osd_compat_objid *map;
        struct dentry           *o;
        int                      rc = 0;
        char                     name[32];
        ENTRY;

        map = osd->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);

        if (map->groups[group].groot != NULL)
                RETURN(0);

        sprintf(name, "%u", group);
        o = ll_lookup_one_len(name, map->root, strlen(name));
        if (IS_ERR(o))
                rc = PTR_ERR(o);
        else if (o->d_inode != NULL)
                map->groups[group].groot = o;
        else {
                rc = -ENOENT;
                dput(o);
        }

        if (map->groups[group].dirs == NULL)
                OBD_ALLOC(map->groups[group].dirs,
                          sizeof(o) * map->subdir_count);
        LASSERT(map->groups[group].dirs);

        RETURN(rc);
}

int osd_compat_load_group(struct osd_device *osd, int group)
{
        struct osd_compat_objid *map;
        int                      rc = 0;
        ENTRY;

        map = osd->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);

        if (map->groups[group].groot != NULL)
                RETURN(0);

        cfs_down(&map->groups[group].dir_init_sem);
        rc = __osd_compat_load_group(osd, group);
        cfs_up(&map->groups[group].dir_init_sem);

        RETURN(rc);
}

int osd_compat_load_or_make_group(struct osd_device *osd, int group)
{
        struct osd_compat_objid *map;
        struct dentry           *o;
        int                      rc = 0;
        char                     name[32];
        ENTRY;

        map = osd->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);

        if (map->groups[group].groot != NULL)
                RETURN(0);

        cfs_down(&map->groups[group].dir_init_sem);

        rc = __osd_compat_load_group(osd, group);

        if (rc == -ENOENT) {
                sprintf(name, "%d", group);
                o = simple_mkdir(map->root, osd->od_mnt, name, 0700, 1);

                if (IS_ERR(o)) {
                        rc = PTR_ERR(o);

                } else if (o->d_inode) {
                        map->groups[group].groot = o;
                        rc = 0;
                } else {
                        LBUG();
                }
        }

        cfs_up(&map->groups[group].dir_init_sem);

        RETURN(rc);
}

/* external locking is required */
int __osd_compat_load_dir(struct osd_device *osd, int group, int dirn)
{
        struct osd_compat_objid_group *grp;
        struct osd_compat_objid       *map;
        struct dentry                 *o;
        int                            rc = 0;
        char                           name[32];
        ENTRY;

        map = osd->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);

        grp = map->groups + group;
        LASSERT(grp);
        LASSERT(grp->groot);

        if (grp->dirs[dirn] != NULL)
                RETURN(0);

        sprintf(name, "d%u", dirn);
        o = ll_lookup_one_len(name, grp->groot, strlen(name));
        if (IS_ERR(o))
                rc = PTR_ERR(o);
        else if (o->d_inode != NULL)
                grp->dirs[dirn] = o;
        else {
                rc = -ENOENT;
                dput(o);
        }

        RETURN(rc);
}

int osd_compat_load_dir(struct osd_device *osd, int group, int dirn)
{
        struct osd_compat_objid_group *grp;
        struct osd_compat_objid       *map;
        int                            rc = 0;
        ENTRY;

        map = osd->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);

        grp = map->groups + group;
        LASSERT(grp);
        LASSERT(grp->groot);

        if (grp->dirs[dirn] != NULL)
                RETURN(0);

        cfs_down(&grp->dir_init_sem);
        rc = __osd_compat_load_dir(osd, group, dirn);
        cfs_up(&grp->dir_init_sem);

        RETURN(rc);
}

int osd_compat_load_or_make_dir(struct osd_device *osd, int group, int dirn)
{
        struct osd_compat_objid_group *grp;
        struct osd_compat_objid       *map;
        struct dentry                 *o;
        int                            rc = 0;
        char                           name[32];
        ENTRY;

        map = osd->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);

        grp = map->groups + group;
        LASSERT(grp);
        LASSERT(grp->groot);

        if (grp->dirs[dirn] != NULL)
                RETURN(0);

        cfs_down(&grp->dir_init_sem);

        rc = __osd_compat_load_dir(osd, group, dirn);

        if (rc == -ENOENT) {
                sprintf(name, "d%u", dirn);
                o = simple_mkdir(grp->groot, osd->od_mnt, name, 0700, 1);

                if (IS_ERR(o)) {
                        rc = PTR_ERR(o);
                } else if (o->d_inode) {
                        grp->dirs[dirn] = o;
                        rc = 0;
                } else {
                        LBUG();
                }
        }

        cfs_up(&grp->dir_init_sem);

        RETURN(rc);
}

int osd_compat_objid_lookup(struct osd_thread_info *info, struct osd_device *dev,
                            const struct lu_fid *fid, struct osd_inode_id *id)
{
        struct osd_compat_objid *map;
        struct dentry           *d;
        struct dentry           *o;
        struct ost_id           *ostid = &info->oti_ostid;
        int                      rc = 0;
        int                      dirn;
        char                     name[32];
        struct ldiskfs_dir_entry_2 *de;
        struct buffer_head         *bh;
        struct inode               *dir;
        ENTRY;

        /* on the very first lookup we find and open directories */

        map = dev->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);
        LASSERT(map->subdir_count);

        fid_ostid_pack(fid, ostid);
        LASSERT(ostid->oi_seq < MAX_OBJID_GROUP);

        rc = osd_compat_load_group(dev, ostid->oi_seq);
        if (rc)
                RETURN(rc);

        dirn = ostid->oi_id & (map->subdir_count - 1);
        rc = osd_compat_load_dir(dev, ostid->oi_seq, dirn);
        if (rc)
                RETURN(rc);

        d = map->groups[ostid->oi_seq].dirs[dirn];
        LASSERT(d);

        sprintf(name, "%Lu", ostid->oi_id);
        o = &info->oti_child_dentry;
        o->d_parent = d;
        o->d_name.hash = 0;
        o->d_name.name = name;
        /* XXX: we can use rc from sprintf() instead of strlen() */
        o->d_name.len = strlen(name);

        dir = d->d_inode;
        LOCK_INODE_MUTEX(dir);
        bh = osd_ldiskfs_find_entry(dir, o, &de, NULL);
        UNLOCK_INODE_MUTEX(dir);

        rc = -ENOENT;
        if (bh) {
                struct inode *inode;

                id->oii_ino = le32_to_cpu(de->inode);
                brelse(bh);

                id->oii_gen = OSD_OII_NOGEN;
                inode = osd_iget(info, dev, id);

                if (IS_ERR(inode))
                        GOTO(cleanup, rc = PTR_ERR(inode));
                rc = 0;
                id->oii_gen = inode->i_generation;
                iput(inode);
        }

cleanup:
        RETURN(rc);
}

int osd_compat_objid_insert(struct osd_thread_info *info,
                            struct osd_device *osd,
                            const struct lu_fid *fid,
                            const struct osd_inode_id *id,
                            struct thandle *th)
{
        struct osd_compat_objid        *map;
        struct dentry                  *d;
        struct ost_id                  *ostid = &info->oti_ostid;
        int                             dirn, rc = 0;
        char                            name[32];
        ENTRY;

        map = osd->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);
        LASSERT(map->subdir_count);

        /* map fid to group:objid */
        fid_ostid_pack(fid, ostid);

        rc = osd_compat_load_or_make_group(osd, ostid->oi_seq);
        if (rc)
                GOTO(cleanup, rc);

        dirn = ostid->oi_id & (map->subdir_count - 1);
        rc = osd_compat_load_or_make_dir(osd, ostid->oi_seq, dirn);
        if (rc)
                GOTO(cleanup, rc);

        d = map->groups[ostid->oi_seq].dirs[dirn];
        LASSERT(d);

        sprintf(name, "%Lu", ostid->oi_id);
        rc = osd_compat_add_entry(info, osd, d, name, id, th);

cleanup:
        RETURN(rc);
}

int osd_compat_objid_delete(struct osd_thread_info *info, struct osd_device *osd,
                            const struct lu_fid *fid, struct thandle *th)
{
        struct osd_compat_objid        *map;
        struct dentry                  *d;
        struct ost_id                  *ostid = &info->oti_ostid;
        int                             dirn, rc = 0;
        char                            name[32];
        ENTRY;

        map = osd->od_ost_map;
        LASSERT(map);
        LASSERT(map->root);
        LASSERT(map->subdir_count);

        /* map fid to group:objid */
        fid_ostid_pack(fid, ostid);

        rc = osd_compat_load_group(osd, ostid->oi_seq);
        if (rc)
                GOTO(cleanup, rc);

        dirn = ostid->oi_id & (map->subdir_count - 1);
        rc = osd_compat_load_dir(osd, ostid->oi_seq, dirn);
        if (rc)
                GOTO(cleanup, rc);

        d = map->groups[ostid->oi_seq].dirs[dirn];
        LASSERT(d);

        sprintf(name, "%Lu", ostid->oi_id);
        rc = osd_compat_del_entry(info, osd, d, name, th);

cleanup:
        RETURN(rc);
}

struct named_oid {
        unsigned long  oid;
        char          *name;
};

static const struct named_oid oids[] = {
        { OFD_LAST_RECV_OID,  LAST_RCVD },
        { OFD_LAST_GROUP_OID, "LAST_GROUP" },
        { LLOG_CATALOGS_OID,  "CATALOGS" },
        { FID_SEQ_SRV_OID,    "seq_srv" },
        { FID_SEQ_CTL_OID,    "seq_ctl" },
        { MDD_CAPA_KEYS_OID,  CAPA_KEYS },
        { FLD_INDEX_OID,      "fld" },
        { MDD_LOV_OBJ_OID,    LOV_OBJID },
        { MDT_LAST_RECV_OID,  LAST_RCVD },
        { MDD_ROOT_INDEX_OID, "" },
        { MDD_ORPHAN_OID,     "" },
        { OFD_HEALTH_CHECK_OID, HEALTH_CHECK },
        { 0,                  NULL }
};

static char * oid2name(const unsigned long oid)
{
        int i = 0;

        while (oids[i].oid) {
                if (oids[i].oid == oid)
                        return oids[i].name;
                i++;
        }
        return NULL;
}

int osd_compat_spec_insert(struct osd_thread_info *info, struct osd_device *osd,
                           const struct lu_fid *fid, const struct osd_inode_id *id,
                           struct thandle *th)
{
        struct osd_compat_objid *map = osd->od_ost_map;
        struct dentry           *root = osd_sb(osd)->s_root;
        char                    *name;
        int                      rc = 0;
        int                      group;
        ENTRY;

        if (fid_oid(fid) >= OFD_GROUP0_LAST_OID &&
            fid_oid(fid) < OFD_GROUP4K_LAST_OID) {
                /* on creation of LAST_ID we create O/<group> hierarchy */
                LASSERT(map);
                group = fid_oid(fid) - OFD_GROUP0_LAST_OID;
                rc = osd_compat_load_or_make_group(osd, group);
                if (rc)
                        GOTO(cleanup, rc);
                rc = osd_compat_add_entry(info, osd, map->groups[group].groot,
                                          "LAST_ID", id, th);
        } else {
                name = oid2name(fid_oid(fid));
                if (name == NULL)
                        CERROR("************ UNKNOWN COMPAT FID "DFID"\n", PFID(fid));
                else if (name[0])
                        rc = osd_compat_add_entry(info, osd, root, name, id, th);
        }

cleanup:
        RETURN(rc);
}

int osd_compat_spec_lookup(struct osd_thread_info *info, struct osd_device *osd,
                           const struct lu_fid *fid, struct osd_inode_id *id)
{
        struct dentry *dentry;
        char          *name;
        int            rc = -ERESTART;
        ENTRY;

        name = oid2name(fid_oid(fid));
        if (name == NULL || strlen(name) == 0)
                return -ERESTART;

        dentry = ll_lookup_one_len(name, osd_sb(osd)->s_root, strlen(name));
        if (!IS_ERR(dentry)) {
                if (dentry->d_inode) {
                        if (is_bad_inode(dentry->d_inode)) {
                                rc = -EIO;
                        } else {
                                id->oii_ino = dentry->d_inode->i_ino;
                                id->oii_gen = dentry->d_inode->i_generation;
                                rc = 0;
                        }
                }
                dput(dentry);
        }

        RETURN(rc);
}

