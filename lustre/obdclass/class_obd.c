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
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/miscdevice.h>
#include <linux/user_namespace.h>
#include <linux/uidgid.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/oom.h>

#include <obd_support.h>
#include <obd_class.h>
#include <uapi/linux/lnet/lnetctl.h>
#include <lustre_kernelcomm.h>
#include <lprocfs_status.h>
#include <cl_object.h>
#ifdef HAVE_SERVER_SUPPORT
# include <dt_object.h>
# include <md_object.h>
#endif /* HAVE_SERVER_SUPPORT */
#include <uapi/linux/lustre/lustre_ioctl.h>
#include "llog_internal.h"

#ifdef CONFIG_PROC_FS
static __u64 obd_max_alloc;
#else
__u64 obd_max_alloc;
#endif

static DEFINE_SPINLOCK(obd_updatemax_lock);

/* The following are visible and mutable through /proc/sys/lustre/. */
unsigned int obd_debug_peer_on_timeout;
EXPORT_SYMBOL(obd_debug_peer_on_timeout);
unsigned int obd_dump_on_timeout;
EXPORT_SYMBOL(obd_dump_on_timeout);
unsigned int obd_dump_on_eviction;
EXPORT_SYMBOL(obd_dump_on_eviction);
unsigned int obd_lbug_on_eviction;
EXPORT_SYMBOL(obd_lbug_on_eviction);
unsigned long obd_max_dirty_pages;
EXPORT_SYMBOL(obd_max_dirty_pages);
atomic_long_t obd_dirty_pages;
EXPORT_SYMBOL(obd_dirty_pages);
unsigned int obd_timeout = OBD_TIMEOUT_DEFAULT;   /* seconds */
EXPORT_SYMBOL(obd_timeout);
unsigned int ldlm_timeout = LDLM_TIMEOUT_DEFAULT; /* seconds */
EXPORT_SYMBOL(ldlm_timeout);
unsigned int obd_timeout_set;
EXPORT_SYMBOL(obd_timeout_set);
unsigned int ldlm_timeout_set;
EXPORT_SYMBOL(ldlm_timeout_set);
/* bulk transfer timeout, give up after 100s by default */
unsigned int bulk_timeout = 100; /* seconds */
EXPORT_SYMBOL(bulk_timeout);
/* Adaptive timeout defs here instead of ptlrpc module for /proc/sys/ access */
unsigned int at_min = 0;
EXPORT_SYMBOL(at_min);
unsigned int at_max = 600;
EXPORT_SYMBOL(at_max);
unsigned int at_history = 600;
EXPORT_SYMBOL(at_history);
int at_early_margin = 5;
EXPORT_SYMBOL(at_early_margin);
int at_extra = 30;
EXPORT_SYMBOL(at_extra);

#ifdef CONFIG_PROC_FS
struct lprocfs_stats *obd_memory = NULL;
EXPORT_SYMBOL(obd_memory);
#endif

static int obdclass_oom_handler(struct notifier_block *self,
				unsigned long notused, void *nfreed)
{
#ifdef CONFIG_PROC_FS
	/* in bytes */
	pr_info("obd_memory max: %llu, obd_memory current: %llu\n",
		obd_memory_max(), obd_memory_sum());
#endif /* CONFIG_PROC_FS */

	return NOTIFY_OK;
}

static struct notifier_block obdclass_oom = {
	.notifier_call = obdclass_oom_handler
};

static int class_resolve_dev_name(__u32 len, const char *name)
{
	int rc;
	int dev;

	ENTRY;
	if (!len || !name) {
		CERROR("No name passed,!\n");
		GOTO(out, rc = -EINVAL);
	}
	if (name[len - 1] != 0) {
		CERROR("Name not nul terminated!\n");
		GOTO(out, rc = -EINVAL);
	}

	CDEBUG(D_IOCTL, "device name %s\n", name);
	dev = class_name2dev(name);
	if (dev == -1) {
		CDEBUG(D_IOCTL, "No device for name %s!\n", name);
		GOTO(out, rc = -EINVAL);
	}

	CDEBUG(D_IOCTL, "device name %s, dev %d\n", name, dev);
	rc = dev;

out:
	RETURN(rc);
}

#define OBD_MAX_IOCTL_BUFFER	8192

static int obd_ioctl_is_invalid(struct obd_ioctl_data *data)
{
	const int maxlen = 1 << 30;
	if (data->ioc_len > maxlen) {
		CERROR("OBD ioctl: ioc_len larger than 1<<30\n");
		return 1;
	}

	if (data->ioc_inllen1 > maxlen) {
		CERROR("OBD ioctl: ioc_inllen1 larger than 1<<30\n");
		return 1;
	}

	if (data->ioc_inllen2 > maxlen) {
		CERROR("OBD ioctl: ioc_inllen2 larger than 1<<30\n");
		return 1;
	}

	if (data->ioc_inllen3 > maxlen) {
		CERROR("OBD ioctl: ioc_inllen3 larger than 1<<30\n");
		return 1;
	}

	if (data->ioc_inllen4 > maxlen) {
		CERROR("OBD ioctl: ioc_inllen4 larger than 1<<30\n");
		return 1;
	}

	if (data->ioc_inlbuf1 && data->ioc_inllen1 == 0) {
		CERROR("OBD ioctl: inlbuf1 pointer but 0 length\n");
		return 1;
	}

	if (data->ioc_inlbuf2 && data->ioc_inllen2 == 0) {
		CERROR("OBD ioctl: inlbuf2 pointer but 0 length\n");
		return 1;
	}

	if (data->ioc_inlbuf3 && data->ioc_inllen3 == 0) {
		CERROR("OBD ioctl: inlbuf3 pointer but 0 length\n");
		return 1;
	}

	if (data->ioc_inlbuf4 && data->ioc_inllen4 == 0) {
		CERROR("OBD ioctl: inlbuf4 pointer but 0 length\n");
		return 1;
	}

	if (data->ioc_pbuf1 && data->ioc_plen1 == 0) {
		CERROR("OBD ioctl: pbuf1 pointer but 0 length\n");
		return 1;
	}

	if (data->ioc_pbuf2 && data->ioc_plen2 == 0) {
		CERROR("OBD ioctl: pbuf2 pointer but 0 length\n");
		return 1;
	}

	if (!data->ioc_pbuf1 && data->ioc_plen1 != 0) {
		CERROR("OBD ioctl: plen1 set but NULL pointer\n");
		return 1;
	}

	if (!data->ioc_pbuf2 && data->ioc_plen2 != 0) {
		CERROR("OBD ioctl: plen2 set but NULL pointer\n");
		return 1;
	}

	if (obd_ioctl_packlen(data) > data->ioc_len) {
		CERROR("OBD ioctl: packlen exceeds ioc_len (%d > %d)\n",
		       obd_ioctl_packlen(data), data->ioc_len);
		return 1;
	}

	return 0;
}

/* buffer MUST be at least the size of obd_ioctl_hdr */
int obd_ioctl_getdata(struct obd_ioctl_data **datap, int *len, void __user *arg)
{
	struct obd_ioctl_hdr hdr;
	struct obd_ioctl_data *data;
	int offset = 0;

	ENTRY;
	if (copy_from_user(&hdr, arg, sizeof(hdr)))
		RETURN(-EFAULT);

	if (hdr.ioc_version != OBD_IOCTL_VERSION) {
		CERROR("Version mismatch kernel (%x) vs application (%x)\n",
		       OBD_IOCTL_VERSION, hdr.ioc_version);
		RETURN(-EINVAL);
	}

	if (hdr.ioc_len > OBD_MAX_IOCTL_BUFFER) {
		CERROR("User buffer len %d exceeds %d max buffer\n",
		       hdr.ioc_len, OBD_MAX_IOCTL_BUFFER);
		RETURN(-EINVAL);
	}

	if (hdr.ioc_len < sizeof(struct obd_ioctl_data)) {
		CERROR("User buffer too small for ioctl (%d)\n", hdr.ioc_len);
		RETURN(-EINVAL);
	}

	/* When there are lots of processes calling vmalloc on multi-core
	 * system, the high lock contention will hurt performance badly,
	 * obdfilter-survey is an example, which relies on ioctl. So we'd
	 * better avoid vmalloc on ioctl path. LU-66
	 */
	OBD_ALLOC_LARGE(data, hdr.ioc_len);
	if (!data) {
		CERROR("Cannot allocate control buffer of len %d\n",
		       hdr.ioc_len);
		RETURN(-EINVAL);
	}
	*len = hdr.ioc_len;

	if (copy_from_user(data, arg, hdr.ioc_len)) {
		OBD_FREE_LARGE(data, hdr.ioc_len);
		RETURN(-EFAULT);
	}

	if (obd_ioctl_is_invalid(data)) {
		CERROR("ioctl not correctly formatted\n");
		OBD_FREE_LARGE(data, hdr.ioc_len);
		RETURN(-EINVAL);
	}

	if (data->ioc_inllen1) {
		data->ioc_inlbuf1 = &data->ioc_bulk[0];
		offset += cfs_size_round(data->ioc_inllen1);
	}

	if (data->ioc_inllen2) {
		data->ioc_inlbuf2 = &data->ioc_bulk[0] + offset;
		offset += cfs_size_round(data->ioc_inllen2);
	}

	if (data->ioc_inllen3) {
		data->ioc_inlbuf3 = &data->ioc_bulk[0] + offset;
		offset += cfs_size_round(data->ioc_inllen3);
	}

	if (data->ioc_inllen4)
		data->ioc_inlbuf4 = &data->ioc_bulk[0] + offset;

	*datap = data;

	RETURN(0);
}
EXPORT_SYMBOL(obd_ioctl_getdata);

int class_handle_ioctl(unsigned int cmd, unsigned long arg)
{
	struct obd_ioctl_data *data;
	struct obd_device *obd = NULL;
	int err = 0, len = 0;

	ENTRY;
	CDEBUG(D_IOCTL, "cmd = %x\n", cmd);
	if (obd_ioctl_getdata(&data, &len, (void __user *)arg)) {
		CERROR("OBD ioctl: data error\n");
		RETURN(-EINVAL);
	}

        switch (cmd) {
        case OBD_IOC_PROCESS_CFG: {
                struct lustre_cfg *lcfg;

                if (!data->ioc_plen1 || !data->ioc_pbuf1) {
                        CERROR("No config buffer passed!\n");
                        GOTO(out, err = -EINVAL);
                }
                OBD_ALLOC(lcfg, data->ioc_plen1);
                if (lcfg == NULL)
                        GOTO(out, err = -ENOMEM);
		err = copy_from_user(lcfg, data->ioc_pbuf1,
                                         data->ioc_plen1);
                if (!err)
                        err = lustre_cfg_sanity_check(lcfg, data->ioc_plen1);
                if (!err)
                        err = class_process_config(lcfg);

                OBD_FREE(lcfg, data->ioc_plen1);
                GOTO(out, err);
        }

#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(3, 0, 53, 0)
	case OBD_GET_VERSION: {
		static bool warned;

		if (!data->ioc_inlbuf1) {
			CERROR("No buffer passed in ioctl\n");
			GOTO(out, err = -EINVAL);
		}

		if (strlen(LUSTRE_VERSION_STRING) + 1 > data->ioc_inllen1) {
			CERROR("ioctl buffer too small to hold version\n");
			GOTO(out, err = -EINVAL);
		}

		if (!warned) {
			warned = true;
			CWARN("%s: ioctl(OBD_GET_VERSION) is deprecated, "
			      "use llapi_get_version_string() and/or relink\n",
			      current->comm);
		}
		memcpy(data->ioc_bulk, LUSTRE_VERSION_STRING,
		       strlen(LUSTRE_VERSION_STRING) + 1);

		if (copy_to_user((void __user *)arg, data, len))
			err = -EFAULT;
		GOTO(out, err);
	}
#endif
        case OBD_IOC_NAME2DEV: {
                /* Resolve a device name.  This does not change the
                 * currently selected device.
                 */
                int dev;

                dev = class_resolve_dev_name(data->ioc_inllen1,
                                             data->ioc_inlbuf1);
                data->ioc_dev = dev;
                if (dev < 0)
                        GOTO(out, err = -EINVAL);

		if (copy_to_user((void __user *)arg, data, sizeof(*data)))
                        err = -EFAULT;
                GOTO(out, err);
        }

        case OBD_IOC_UUID2DEV: {
                /* Resolve a device uuid.  This does not change the
                 * currently selected device.
                 */
                int dev;
                struct obd_uuid uuid;

                if (!data->ioc_inllen1 || !data->ioc_inlbuf1) {
                        CERROR("No UUID passed!\n");
                        GOTO(out, err = -EINVAL);
                }
                if (data->ioc_inlbuf1[data->ioc_inllen1 - 1] != 0) {
                        CERROR("UUID not NUL terminated!\n");
                        GOTO(out, err = -EINVAL);
                }

                CDEBUG(D_IOCTL, "device name %s\n", data->ioc_inlbuf1);
                obd_str2uuid(&uuid, data->ioc_inlbuf1);
                dev = class_uuid2dev(&uuid);
                data->ioc_dev = dev;
                if (dev == -1) {
                        CDEBUG(D_IOCTL, "No device for UUID %s!\n",
                               data->ioc_inlbuf1);
                        GOTO(out, err = -EINVAL);
                }

                CDEBUG(D_IOCTL, "device name %s, dev %d\n", data->ioc_inlbuf1,
                       dev);
		if (copy_to_user((void __user *)arg, data, sizeof(*data)))
                        err = -EFAULT;
                GOTO(out, err);
        }

        case OBD_IOC_GETDEVICE: {
                int     index = data->ioc_count;
                char    *status, *str;

                if (!data->ioc_inlbuf1) {
                        CERROR("No buffer passed in ioctl\n");
                        GOTO(out, err = -EINVAL);
                }
                if (data->ioc_inllen1 < 128) {
                        CERROR("ioctl buffer too small to hold version\n");
                        GOTO(out, err = -EINVAL);
                }

                obd = class_num2obd(index);
                if (!obd)
                        GOTO(out, err = -ENOENT);

		if (obd->obd_stopping)
			status = "ST";
		else if (obd->obd_inactive)
			status = "IN";
		else if (obd->obd_set_up)
			status = "UP";
		else if (obd->obd_attached)
			status = "AT";
		else
			status = "--";

                str = (char *)data->ioc_bulk;
                snprintf(str, len - sizeof(*data), "%3d %s %s %s %s %d",
                         (int)index, status, obd->obd_type->typ_name,
                         obd->obd_name, obd->obd_uuid.uuid,
			 atomic_read(&obd->obd_refcount));

		if (copy_to_user((void __user *)arg, data, len))
			err = -EFAULT;

		GOTO(out, err);
        }

        }

        if (data->ioc_dev == OBD_DEV_BY_DEVNAME) {
                if (data->ioc_inllen4 <= 0 || data->ioc_inlbuf4 == NULL)
                        GOTO(out, err = -EINVAL);
                if (strnlen(data->ioc_inlbuf4, MAX_OBD_NAME) >= MAX_OBD_NAME)
                        GOTO(out, err = -EINVAL);
                obd = class_name2obd(data->ioc_inlbuf4);
        } else if (data->ioc_dev < class_devno_max()) {
                obd = class_num2obd(data->ioc_dev);
        } else {
                CERROR("OBD ioctl: No device\n");
                GOTO(out, err = -EINVAL);
        }

        if (obd == NULL) {
                CERROR("OBD ioctl : No Device %d\n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }
        LASSERT(obd->obd_magic == OBD_DEVICE_MAGIC);

        if (!obd->obd_set_up || obd->obd_stopping) {
                CERROR("OBD ioctl: device not setup %d \n", data->ioc_dev);
                GOTO(out, err = -EINVAL);
        }

	err = obd_iocontrol(cmd, obd->obd_self_export, len, data, NULL);
	if (err)
		GOTO(out, err);

	if (copy_to_user((void __user *)arg, data, len))
		err = -EFAULT;
out:
	OBD_FREE_LARGE(data, len);
	RETURN(err);
} /* class_handle_ioctl */

/* to control /dev/obd */
static long obd_class_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	int err = 0;

	ENTRY;
	/* Allow non-root access for some limited ioctls */
	if (!capable(CAP_SYS_ADMIN))
		RETURN(err = -EACCES);

	if ((cmd & 0xffffff00) == ((int)'T') << 8) /* ignore all tty ioctls */
		RETURN(err = -ENOTTY);

	err = class_handle_ioctl(cmd, (unsigned long)arg);

	RETURN(err);
}

/* declare character device */
static const struct file_operations obd_psdev_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= obd_class_ioctl,	/* unlocked_ioctl */
};

/* modules setup */
static struct miscdevice obd_psdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= OBD_DEV_NAME,
	.fops	= &obd_psdev_fops,
};

#define test_string_to_size_err(value, expect, def_unit, __rc)		       \
({									       \
	u64 __size;							       \
	int __ret;							       \
									       \
	BUILD_BUG_ON(sizeof(value) >= 23);				       \
	__ret = sysfs_memparse(value, sizeof(value) - 1, &__size, def_unit);   \
	if (__ret != __rc)						       \
		CERROR("string_helper: parsing '%s' expect rc %d != got %d\n", \
		       value, __rc, __ret);				       \
	else if (!__ret && (u64)expect != __size)			       \
		CERROR("string_helper: parsing '%s' expect %llu != got %llu\n",\
		       value, (u64)expect, __size);			       \
	__ret;								       \
})
#define test_string_to_size_one(value, expect, def_unit)		       \
	test_string_to_size_err(value, expect, def_unit, 0)

static int __init obd_init_checks(void)
{
	__u64 u64val, div64val;
	char buf[64];
	int len, ret = 0;

	CDEBUG(D_INFO, "OBD_OBJECT_EOF = %#llx\n", (__u64)OBD_OBJECT_EOF);

	u64val = OBD_OBJECT_EOF;
	CDEBUG(D_INFO, "u64val OBD_OBJECT_EOF = %#llx\n", u64val);
	if (u64val != OBD_OBJECT_EOF) {
		CERROR("__u64 %#llx(%d) != 0xffffffffffffffff\n",
		       u64val, (int)sizeof(u64val));
		ret = -EINVAL;
	}
	len = snprintf(buf, sizeof(buf), "%#llx", u64val);
	if (len != 18) {
		CERROR("u64 hex wrong length, strlen(%s)=%d != 18\n", buf, len);
		ret = -EINVAL;
	}

	div64val = OBD_OBJECT_EOF;
	CDEBUG(D_INFO, "u64val OBD_OBJECT_EOF = %#llx\n", u64val);
	if (u64val != OBD_OBJECT_EOF) {
		CERROR("__u64 %#llx(%d) != 0xffffffffffffffff\n",
		       u64val, (int)sizeof(u64val));
		ret = -EOVERFLOW;
	}
	if (u64val >> 8 != OBD_OBJECT_EOF >> 8) {
		CERROR("__u64 %#llx(%d) != 0xffffffffffffffff\n",
		       u64val, (int)sizeof(u64val));
		ret = -EOVERFLOW;
	}
	if (do_div(div64val, 256) != (u64val & 255)) {
		CERROR("do_div(%#llx,256) != %llu\n", u64val, u64val & 255);
		ret = -EOVERFLOW;
	}
	if (u64val >> 8 != div64val) {
		CERROR("do_div(%#llx,256) %llu != %llu\n",
		       u64val, div64val, u64val >> 8);
		ret = -EOVERFLOW;
	}
	len = snprintf(buf, sizeof(buf), "%#llx", u64val);
	if (len != 18) {
		CERROR("u64 hex wrong length! strlen(%s)=%d != 18\n", buf, len);
		ret = -EINVAL;
	}
	len = snprintf(buf, sizeof(buf), "%llu", u64val);
	if (len != 20) {
		CERROR("u64 wrong length! strlen(%s)=%d != 20\n", buf, len);
		ret = -EINVAL;
	}
	len = snprintf(buf, sizeof(buf), "%lld", u64val);
	if (len != 2) {
		CERROR("s64 wrong length! strlen(%s)=%d != 2\n", buf, len);
		ret = -EINVAL;
	}
	if ((u64val & ~PAGE_MASK) >= PAGE_SIZE) {
		CERROR("mask failed: u64val %llu >= %llu\n", u64val,
		       (__u64)PAGE_SIZE);
		ret = -EINVAL;
	}
	if (ret)
		RETURN(ret);

	/* invalid string */
	if (!test_string_to_size_err("256B34", 256, "B", -EINVAL)) {
		CERROR("string_helpers: format should be number then units\n");
		ret = -EINVAL;
	}
	if (!test_string_to_size_err("132OpQ", 132, "B", -EINVAL)) {
		CERROR("string_helpers: invalid units should be rejected\n");
		ret = -EINVAL;
	}
	if (!test_string_to_size_err("1.82B", 1, "B", -EINVAL)) {
		CERROR("string_helpers: 'B' with '.' should be invalid\n");
		ret = -EINVAL;
	}
	if (test_string_to_size_one("343\n", 343, "B")) {
		CERROR("string_helpers: should ignore newline\n");
		ret = -EINVAL;
	}
	if (ret)
		RETURN(ret);

	/* memparse unit handling */
	ret = 0;
	ret += test_string_to_size_one("0B", 0, "B");
	ret += test_string_to_size_one("512B", 512, "B");
	ret += test_string_to_size_one("1.067kB", 1067, "B");
	ret += test_string_to_size_one("1.042KiB", 1067, "B");
	ret += test_string_to_size_one("8", 8388608, "M");
	ret += test_string_to_size_one("65536", 65536, "B");
	ret += test_string_to_size_one("128", 131072, "K");
	ret += test_string_to_size_one("1M", 1048576, "B");
	ret += test_string_to_size_one("0.5T", 549755813888ULL, "T");
	ret += test_string_to_size_one("256.5G", 275414777856ULL, "G");
	if (ret)
		RETURN(ret);

	/* string helper values */
	ret += test_string_to_size_one("16", 16777216, "MiB");
	ret += test_string_to_size_one("8.39MB", 8390000, "MiB");
	ret += test_string_to_size_one("8.00MiB", 8388608, "MiB");
	ret += test_string_to_size_one("256GB", 256000000000ULL, "GiB");
	ret += test_string_to_size_one("238.731GiB", 256335459385ULL, "GiB");
	if (ret)
		RETURN(ret);

	/* huge values */
	ret += test_string_to_size_one("0.4TB", 400000000000ULL, "TiB");
	ret += test_string_to_size_one("12.5TiB", 13743895347200ULL, "TiB");
	ret += test_string_to_size_one("2PB", 2000000000000000ULL, "PiB");
	ret += test_string_to_size_one("16PiB", 18014398509481984ULL, "PiB");
	if (ret)
		RETURN(ret);

	/* huge values should overflow */
	if (!test_string_to_size_err("1000EiB", 0, "EiB", -EOVERFLOW)) {
		CERROR("string_helpers: failed to detect binary overflow\n");
		ret = -EINVAL;
	}
	if (!test_string_to_size_err("1000EB", 0, "EiB", -EOVERFLOW)) {
		CERROR("string_helpers: failed to detect decimal overflow\n");
		ret = -EINVAL;
	}

	return ret;
}

static int __init obdclass_init(void)
{
	int err;

	LCONSOLE_INFO("Lustre: Build Version: "LUSTRE_VERSION_STRING"\n");

	register_oom_notifier(&obdclass_oom);

	libcfs_kkuc_init();

	err = obd_init_checks();
	if (err)
		return err;

#ifdef CONFIG_PROC_FS
	obd_memory = lprocfs_alloc_stats(OBD_STATS_NUM,
					 LPROCFS_STATS_FLAG_NONE |
					 LPROCFS_STATS_FLAG_IRQ_SAFE);
	if (obd_memory == NULL) {
		CERROR("kmalloc of 'obd_memory' failed\n");
		return -ENOMEM;
	}

	lprocfs_counter_init(obd_memory, OBD_MEMORY_STAT,
			     LPROCFS_CNTR_AVGMINMAX,
			     "memused", "bytes");
#endif
	err = obd_zombie_impexp_init();
	if (err)
		goto cleanup_obd_memory;

	err = class_handle_init();
	if (err)
		goto cleanup_zombie_impexp;

	err = misc_register(&obd_psdev);
	if (err) {
		CERROR("cannot register OBD miscdevice: err = %d\n", err);
		goto cleanup_class_handle;
	}

	/* Default the dirty page cache cap to 1/2 of system memory.
	 * For clients with less memory, a larger fraction is needed
	 * for other purposes (mostly for BGL). */
	if (cfs_totalram_pages() <= 512 << (20 - PAGE_SHIFT))
		obd_max_dirty_pages = cfs_totalram_pages() / 4;
	else
		obd_max_dirty_pages = cfs_totalram_pages() / 2;

	err = obd_init_caches();
	if (err)
		goto cleanup_deregister;

	err = class_procfs_init();
	if (err)
		goto cleanup_caches;

	err = lu_global_init();
	if (err)
		goto cleanup_class_procfs;

	err = cl_global_init();
	if (err != 0)
		goto cleanup_lu_global;

	err = llog_info_init();
	if (err)
		goto cleanup_cl_global;

#ifdef HAVE_SERVER_SUPPORT
	err = dt_global_init();
	if (err != 0)
		goto cleanup_llog_info;

	err = lu_ucred_global_init();
	if (err != 0)
		goto cleanup_dt_global;

	err = lustre_tgt_register_fs();
	if (err && err != -EBUSY) {
		/* Don't fail if server code also registers "lustre_tgt" */
		CERROR("obdclass: register fstype 'lustre_tgt' failed: rc = %d\n",
		       err);
		goto cleanup_lu_ucred_global;
	}
#endif /* HAVE_SERVER_SUPPORT */

	/* simulate a late OOM situation now to require all
	 * alloc'ed/initialized resources to be freed
	 */
	if (OBD_FAIL_CHECK(OBD_FAIL_OBDCLASS_MODULE_LOAD)) {
		/* force error to ensure module will be unloaded/cleaned */
		err = -ENOMEM;
		goto cleanup_all;
	}
	return 0;

cleanup_all:
#ifdef HAVE_SERVER_SUPPORT
	/* fake error but filesystem has been registered */
	lustre_tgt_unregister_fs();

cleanup_lu_ucred_global:
	lu_ucred_global_fini();

cleanup_dt_global:
	dt_global_fini();

cleanup_llog_info:
#endif /* HAVE_SERVER_SUPPORT */
	llog_info_fini();

cleanup_cl_global:
	cl_global_fini();

cleanup_lu_global:
	lu_global_fini();

cleanup_class_procfs:
	class_procfs_clean();

cleanup_caches:
	obd_cleanup_caches();

cleanup_deregister:
	misc_deregister(&obd_psdev);

cleanup_class_handle:
	class_handle_cleanup();

cleanup_zombie_impexp:
	obd_zombie_impexp_stop();

cleanup_obd_memory:
#ifdef CONFIG_PROC_FS
	lprocfs_free_stats(&obd_memory);
#endif

	unregister_oom_notifier(&obdclass_oom);
	return err;
}

void obd_update_maxusage(void)
{
	__u64 max;

	max = obd_memory_sum();

	spin_lock(&obd_updatemax_lock);
	if (max > obd_max_alloc)
		obd_max_alloc = max;
	spin_unlock(&obd_updatemax_lock);
}
EXPORT_SYMBOL(obd_update_maxusage);

#ifdef CONFIG_PROC_FS
__u64 obd_memory_max(void)
{
	__u64 ret;

	obd_update_maxusage();
	spin_lock(&obd_updatemax_lock);
	ret = obd_max_alloc;
	spin_unlock(&obd_updatemax_lock);

	return ret;
}
#endif /* CONFIG_PROC_FS */

static void __exit obdclass_exit(void)
{
#ifdef CONFIG_PROC_FS
	__u64 memory_leaked;
	__u64 memory_max;
#endif /* CONFIG_PROC_FS */
	ENTRY;

	misc_deregister(&obd_psdev);
#ifdef HAVE_SERVER_SUPPORT
	lustre_tgt_unregister_fs();
	lu_ucred_global_fini();
	dt_global_fini();
#endif /* HAVE_SERVER_SUPPORT */
	llog_info_fini();
	cl_global_fini();
	lu_global_fini();

	obd_cleanup_caches();

	class_procfs_clean();

	class_handle_cleanup();
	class_del_uuid(NULL); /* Delete all UUIDs. */
	obd_zombie_impexp_stop();

#ifdef CONFIG_PROC_FS
	memory_leaked = obd_memory_sum();
	memory_max = obd_memory_max();

	lprocfs_free_stats(&obd_memory);
	/* the below message is checked in test-framework.sh check_mem_leak() */
	CDEBUG((memory_leaked) ? D_ERROR : D_INFO,
	       "obd_memory max: %llu, leaked: %llu\n",
	       memory_max, memory_leaked);
#endif /* CONFIG_PROC_FS */

	unregister_oom_notifier(&obdclass_oom);

	EXIT;
}

void obd_heat_clear(struct obd_heat_instance *instance, int count)
{
	ENTRY;

	memset(instance, 0, sizeof(*instance) * count);
	RETURN_EXIT;
}
EXPORT_SYMBOL(obd_heat_clear);

/*
 * The file heat is calculated for every time interval period I. The access
 * frequency during each period is counted. The file heat is only recalculated
 * at the end of a time period.  And a percentage of the former file heat is
 * lost when recalculated. The recursion formula to calculate the heat of the
 * file f is as follow:
 *
 * Hi+1(f) = (1-P)*Hi(f)+ P*Ci
 *
 * Where Hi is the heat value in the period between time points i*I and
 * (i+1)*I; Ci is the access count in the period; the symbol P refers to the
 * weight of Ci. The larger the value the value of P is, the more influence Ci
 * has on the file heat.
 */
void obd_heat_decay(struct obd_heat_instance *instance,  __u64 time_second,
		    unsigned int weight, unsigned int period_second)
{
	u64 second;

	ENTRY;

	if (instance->ohi_time_second > time_second) {
		obd_heat_clear(instance, 1);
		RETURN_EXIT;
	}

	if (instance->ohi_time_second == 0)
		RETURN_EXIT;

	for (second = instance->ohi_time_second + period_second;
	     second < time_second;
	     second += period_second) {
		instance->ohi_heat = instance->ohi_heat *
				(256 - weight) / 256 +
				instance->ohi_count * weight / 256;
		instance->ohi_count = 0;
		instance->ohi_time_second = second;
	}
	RETURN_EXIT;
}
EXPORT_SYMBOL(obd_heat_decay);

__u64 obd_heat_get(struct obd_heat_instance *instance, unsigned int time_second,
		   unsigned int weight, unsigned int period_second)
{
	ENTRY;

	obd_heat_decay(instance, time_second, weight, period_second);

	if (instance->ohi_count == 0)
		RETURN(instance->ohi_heat);

	RETURN(instance->ohi_heat * (256 - weight) / 256 +
	       instance->ohi_count * weight / 256);
}
EXPORT_SYMBOL(obd_heat_get);

void obd_heat_add(struct obd_heat_instance *instance,
		  unsigned int time_second,  __u64 count,
		  unsigned int weight, unsigned int period_second)
{
	ENTRY;

	obd_heat_decay(instance, time_second, weight, period_second);
	if (instance->ohi_time_second == 0) {
		instance->ohi_time_second = time_second;
		instance->ohi_heat = 0;
		instance->ohi_count = count;
	} else {
		instance->ohi_count += count;
	}
	RETURN_EXIT;
}
EXPORT_SYMBOL(obd_heat_add);

MODULE_AUTHOR("OpenSFS, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Class Driver");
MODULE_VERSION(LUSTRE_VERSION_STRING);
MODULE_LICENSE("GPL");

module_init(obdclass_init);
module_exit(obdclass_exit);
