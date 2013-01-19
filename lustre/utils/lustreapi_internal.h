/*
 * LGPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * (C) Copyright 2012 Commissariat a l'energie atomique et aux energies
 *     alternatives
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 or (at your discretion) any later version.
 * (LGPL) version 2.1 accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * LGPL HEADER END
 */
/*
 *
 * lustre/utils/lustreapi_internal.h
 *
 */
/*
 *
 * Author: Aurelien Degremont <aurelien.degremont@cea.fr>
 * Author: JC Lafoucriere <jacques-charles.lafoucriere@cea.fr>
 * Author: Thomas Leibovici <thomas.leibovici@cea.fr>
 */

#ifndef _LUSTREAPI_INTERNAL_H_
#define _LUSTREAPI_INTERNAL_H_

#define WANT_PATH   0x1
#define WANT_FSNAME 0x2
#define WANT_FD     0x4
#define WANT_INDEX  0x8
#define WANT_ERROR  0x10
int get_root_path(int want, char *fsname, int *outfd, char *path, int index);
int root_ioctl(const char *mdtname, int opc, void *data, int *mdtidxp,
	       int want_error);

/* Helper functions for testing validity of stripe attributes. */

static inline int llapi_stripe_size_is_valid(size_t stripe_size)
{
	return (stripe_size < 0 || (stripe_size & (LOV_MIN_STRIPE_SIZE - 1)))
		? 0 : 1;
}

static inline int llapi_stripe_size_is_too_big(size_t stripe_size)
{
	return (stripe_size >= (1ULL << 32)) ? 1 : 0;
}

static inline int llapi_stripe_count_is_valid(int stripe_count)
{
	return (stripe_count < -1 || stripe_count > LOV_MAX_STRIPE_COUNT)
		? 0 : 1;
}

static inline int llapi_stripe_offset_is_valid(int stripe_offset)
{
	return (stripe_offset < -1 || stripe_offset > MAX_OBD_DEVICES)
		? 0 : 1;
}

#endif /* _LUSTREAPI_INTERNAL_H_ */
