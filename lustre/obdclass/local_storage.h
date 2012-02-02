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
 * Copyright (c) 2011, 2012, Whamcloud, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/obdclass/local_storage.h
 *
 * Local storage for file/objects with fid generation. Wors on top of OSD.
 *
 * Author: Mikhail Pershin <tappro@whamcloud.com>
 */

#define DEBUG_SUBSYSTEM S_CLASS

#include <dt_object.h>
#include <obd.h>
#include <lustre_fid.h>

struct ls_device {
	struct dt_device	 ls_top_dev;
	/* all initialized ls_devices on this node linked by this */
	cfs_list_t		 ls_linkage;
	/* how many handle's reference this local storage */
	cfs_atomic_t		 ls_refcount;
	/* underlaying OSD device */
	struct dt_device	*ls_osd;
	/* list of all local OID storages */
	cfs_list_t		 ls_los_list;
	cfs_mutex_t		 ls_los_mutex;
};

static inline struct ls_device *dt2ls_dev(struct dt_device *d)
{
	return container_of0(d, struct ls_device, ls_top_dev);
}

struct ls_object {
	struct lu_object_header	 ls_header;
	struct dt_object	 ls_obj;
};

static inline struct ls_object *lu2ls_obj(struct lu_object *o)
{
	return container_of0(o, struct ls_object, ls_obj.do_lu);
}

static inline struct dt_object *ls_locate(const struct lu_env *env,
					  struct ls_device *ls,
					  const struct lu_fid *fid)
{
	return dt_locate_at(env, ls->ls_osd, fid, &ls->ls_top_dev.dd_lu_dev);
}


