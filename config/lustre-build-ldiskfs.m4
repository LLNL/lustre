AC_DEFUN([LDISKFS_LINUX_SERIES],
[
LDISKFS_SERIES=
AC_MSG_CHECKING([which ldiskfs series to use])

SER=
AS_IF([test x$RHEL_KERNEL = xyes], [
	AS_VERSION_COMPARE([$RHEL_KERNEL_VERSION],[2.6.32-343],[
	AS_VERSION_COMPARE([$RHEL_KERNEL_VERSION],[2.6.32],[],
	[SER="2.6-rhel6.series"],[SER="2.6-rhel6.series"])],
	[SER="2.6-rhel6.4.series"],[SER="2.6-rhel6.4.series"])
], [test x$SUSE_KERNEL = xyes], [
	AS_VERSION_COMPARE([$LINUXRELEASE],[3.0.0],[
	AS_VERSION_COMPARE([$LINUXRELEASE],[2.6.32],[],
	[SER="2.6-sles11.series"],[SER="2.6-sles11.series"])],
	[SER="3.0-sles11.series"],[SER="3.0-sles11.series"])
])
LDISKFS_SERIES=$SER

AS_IF([test -z "$LDISKFS_SERIES"],
	[AC_MSG_WARN([Unknown kernel version $LDISKFS_VERSIONRELEASE])])
AC_MSG_RESULT([$LDISKFS_SERIES])

AC_SUBST(LDISKFS_SERIES)
])

#
# 2.6.32-rc7 ext4_free_blocks requires struct buffer_head
#
AC_DEFUN([LB_EXT_FREE_BLOCKS_WITH_BUFFER_HEAD],
[AC_MSG_CHECKING([if ext4_free_blocks needs struct buffer_head])
 LB_LINUX_TRY_COMPILE([
	#include <linux/fs.h>
	#include "$EXT4_SRC_DIR/ext4.h"
],[
	ext4_free_blocks(NULL, NULL, NULL, 0, 0, 0);
],[
	AC_MSG_RESULT([yes])
	AC_DEFINE(HAVE_EXT_FREE_BLOCK_WITH_BUFFER_HEAD, 1,
		  [ext4_free_blocks do not require struct buffer_head])
],[
	AC_MSG_RESULT([no])
])
])

#
# 2.6.35 renamed ext_pblock to ext4_ext_pblock(ex)
#
AC_DEFUN([LB_EXT_PBLOCK],
[AC_MSG_CHECKING([if kernel has ext_pblocks])
 LB_LINUX_TRY_COMPILE([
	#include <linux/fs.h>
	#include "$EXT4_SRC_DIR/ext4_extents.h"
],[
	ext_pblock(NULL);
],[
	AC_MSG_RESULT([yes])
	AC_DEFINE(HAVE_EXT_PBLOCK, 1,
		  [kernel has ext_pblocks])
],[
	AC_MSG_RESULT([no])
])
])

#
# LDISKFS_AC_PATCH_PROGRAM
#
# Determine which program should be used to apply the patches to
# the ext4 source code to produce the ldiskfs source code.
#
AC_DEFUN([LDISKFS_AC_PATCH_PROGRAM], [
	AC_ARG_ENABLE([quilt],
		[AC_HELP_STRING([--disable-quilt],
			[disable use of quilt for ldiskfs])],
		[AS_IF([test "x$enableval" = xno],
			[use_quilt=no],
			[use_quilt=maybe])],
		[use_quilt=maybe]
	)

	AS_IF([test x$use_quilt = xmaybe], [
		AC_PATH_PROG([quilt_avail], [quilt], [no])
		AS_IF([test x$quilt_avail = xno], [
			use_quilt=no
		], [
			use_quilt=yes
		])
	])

	AS_IF([test x$use_quilt = xno], [
		AC_PATH_PROG([patch_avail], [patch], [no])
		AS_IF([test x$patch_avail = xno], [
			AC_MSG_ERROR([*** Need "quilt" or "patch" command])
		])
	])
])

#
# LB_CONDITIONAL_LDISKFS
#
AC_DEFUN([LB_CONFIG_LDISKFS],
[
# --with-ldiskfs is deprecated now that ldiskfs is fully merged with lustre.
# However we continue to support this option through Lustre 2.5.
AC_ARG_WITH([ldiskfs],
	[],
	[AC_MSG_WARN([--with-ldiskfs is deprecated, please use --enable-ldiskfs])
	AS_IF([test x$withval != xyes -a x$withval != xno],
		[AC_MSG_ERROR([the ldiskfs option is deprecated, and
			no longer supports paths to external ldiskfs source])])
	]
)

AC_ARG_ENABLE([ldiskfs],
	[AS_HELP_STRING([--disable-ldiskfs],
		[disable ldiskfs osd (default is enable)])],
	[AS_IF([test x$enable_ldiskfs != xyes -a x$enable_ldiskfs != xno],
		[AC_MSG_ERROR([ldiskfs valid options are "yes" or "no"])])],
	[AS_IF([test "${with_ldiskfs+set}" = set],
		[enable_ldiskfs=$with_ldiskfs],
		[enable_ldiskfs=maybe])
	]
)

AS_IF([test x$enable_server = xno],
	[AS_CASE([$enable_ldiskfs],
		[maybe], [enable_ldiskfs=no],
		[yes], [AC_MSG_ERROR([cannot build ldiskfs when servers are disabled])]
	)]
)

AS_IF([test x$enable_ldiskfs != xno],[
	# In the future, we chould change enable_ldiskfs from maybe to
	# either yes or no based on additional tests, e.g.  whether a patch
	# set is available for the detected kernel.  For now, we just always
	# set it to "yes".
	AS_IF([test x$enable_ldiskfs = xmaybe], [enable_ldiskfs=yes])

	LDISKFS_LINUX_SERIES
	LDISKFS_AC_PATCH_PROGRAM
	LB_EXT4_SRC_DIR
	LB_EXT_FREE_BLOCKS_WITH_BUFFER_HEAD
	LB_EXT_PBLOCK
	AC_DEFINE(CONFIG_LDISKFS_FS_POSIX_ACL, 1, [posix acls for ldiskfs])
	AC_DEFINE(CONFIG_LDISKFS_FS_SECURITY, 1, [fs security for ldiskfs])
	AC_DEFINE(CONFIG_LDISKFS_FS_XATTR, 1, [extened attributes for ldiskfs])
	AC_SUBST(LDISKFS_SUBDIR, ldiskfs)
	AC_DEFINE(HAVE_LDISKFS_OSD, 1, Enable ldiskfs osd)
])

AC_MSG_CHECKING([whether to build ldiskfs])
AC_MSG_RESULT([$enable_ldiskfs])

AM_CONDITIONAL([LDISKFS_ENABLED], [test x$enable_ldiskfs = xyes])
])

#
# LB_VALIDATE_EXT4_SRC_DIR
#
# Spot check the existance of several source files common to ext4.
# Detecting this at configure time allows us to avoid a potential build
# failure and provide a useful error message to explain what is wrong.
#
AC_DEFUN([LB_VALIDATE_EXT4_SRC_DIR],
[
if test x$EXT4_SRC_DIR = x; then
	enable_ldiskfs_build='no'
else
	LB_CHECK_FILE([$EXT4_SRC_DIR/dir.c], [], [
		enable_ldiskfs_build='no'
		AC_MSG_WARN([ext4 must exist for ldiskfs build])])
	LB_CHECK_FILE([$EXT4_SRC_DIR/file.c], [], [
		enable_ldiskfs_build='no'
		AC_MSG_WARN([ext4 must exist for ldiskfs build])])
	LB_CHECK_FILE([$EXT4_SRC_DIR/inode.c], [], [
		enable_ldiskfs_build='no'
		AC_MSG_WARN([ext4 must exist for ldiskfs build])])
	LB_CHECK_FILE([$EXT4_SRC_DIR/super.c], [], [
		enable_ldiskfs_build='no'
		AC_MSG_WARN([ext4 must exist for ldiskfs build])])
fi

if test x$enable_ldiskfs_build = xno; then
	enable_ldiskfs='no'

	AC_MSG_WARN([

Disabling ldiskfs support because complete ext4 source does not exist.

If you are building using kernel-devel packages and require ldiskfs
server support then ensure that the matching kernel-debuginfo-common
and kernel-debuginfo-common-<arch> packages are installed.

])

fi
])

#
# LB_EXT4_SRC_DIR
#
# Determine the location of the ext4 source code.  It it required
# for several configure tests and to build ldiskfs.
#
AC_DEFUN([LB_EXT4_SRC_DIR],
[
# Kernel ext source located with devel headers
linux_src=$LINUX
if test -e "$linux_src/fs/ext4/super.c"; then
	EXT4_SRC_DIR=$linux_src/fs/ext4
else
	# Kernel ext source provided by kernel-debuginfo-common package
	linux_src=$(ls -1d /usr/src/debug/*/linux-$LINUXRELEASE \
		2>/dev/null | tail -1)
	if test -e "$linux_src/fs/ext4/super.c"; then
		EXT4_SRC_DIR=$linux_src/fs/ext4
	else
		EXT4_SRC_DIR=
	fi
fi

AC_MSG_CHECKING([ext4 source directory])
AC_MSG_RESULT([$EXT4_SRC_DIR])
AC_SUBST(EXT4_SRC_DIR)

LB_VALIDATE_EXT4_SRC_DIR
])

#
# LB_DEFINE_E2FSPROGS_NAMES
#
# Enable the use of alternate naming of ldiskfs-enabled e2fsprogs package.
#
AC_DEFUN([LB_DEFINE_E2FSPROGS_NAMES],
[
AC_ARG_WITH([ldiskfsprogs],
        AC_HELP_STRING([--with-ldiskfsprogs],
                       [use alternate names for ldiskfs-enabled e2fsprogs]),
	[],[withval='no'])

AC_MSG_CHECKING([whether to use alternate names for e2fsprogs])
if test x$withval = xyes ; then
	AC_DEFINE(HAVE_LDISKFSPROGS, 1, [enable use of ldiskfsprogs package])
	E2FSPROGS="ldiskfsprogs"
	MKE2FS="mkfs.ldiskfs"
	DEBUGFS="debugfs.ldiskfs"
	TUNE2FS="tunefs.ldiskfs"
	E2LABEL="label.ldiskfs"
	DUMPE2FS="dumpfs.ldiskfs"
	E2FSCK="fsck.ldiskfs"
	PFSCK="pfsck.ldiskfs"
	AC_MSG_RESULT([enabled])
else
	E2FSPROGS="e2fsprogs"
	MKE2FS="mke2fs"
	DEBUGFS="debugfs"
	TUNE2FS="tune2fs"
	E2LABEL="e2label"
	DUMPE2FS="dumpe2fs"
	E2FSCK="e2fsck"
	PFSCK="fsck"
	AC_MSG_RESULT([disabled])
fi

AC_DEFINE_UNQUOTED(E2FSPROGS, "$E2FSPROGS", [name of ldiskfs e2fsprogs package])
AC_DEFINE_UNQUOTED(MKE2FS, "$MKE2FS", [name of ldiskfs mkfs program])
AC_DEFINE_UNQUOTED(DEBUGFS, "$DEBUGFS", [name of ldiskfs debug program])
AC_DEFINE_UNQUOTED(TUNE2FS, "$TUNE2FS", [name of ldiskfs tune program])
AC_DEFINE_UNQUOTED(E2LABEL, "$E2LABEL", [name of ldiskfs label program])
AC_DEFINE_UNQUOTED(DUMPE2FS,"$DUMPE2FS", [name of ldiskfs dump program])
AC_DEFINE_UNQUOTED(E2FSCK, "$E2FSCK", [name of ldiskfs fsck program])
AC_DEFINE_UNQUOTED(PFSCK, "$PFSCK", [name of parallel fsck program])

AC_SUBST([E2FSPROGS], [$E2FSPROGS])
AC_SUBST([MKE2FS], [$MKE2FS])
AC_SUBST([DEBUGFS], [$DEBUGFS])
AC_SUBST([TUNE2FS], [$TUNE2FS])
AC_SUBST([E2LABEL], [$E2LABEL])
AC_SUBST([DUMPE2FS], [$DUMPE2FS])
AC_SUBST([E2FSCK], [$E2FSCK])
AC_SUBST([PFSCK], [$PFSCK])
])
