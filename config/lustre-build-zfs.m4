dnl #
dnl # Supported configure options.  When no options are specified support
dnl # for ZFS OSDs will be autodetected assuming server support is enabled.
dnl # If the ZFS OSD cannot be built support for it is disabled and a
dnl # warning is issued but the configure process is allowed to continue.
dnl #
dnl # --without-zfs   - Disable zfs support.
dnl # --with-zfs=no
dnl #
dnl # --with-zfs      - Enable zfs support and attempt to autodetect the zfs
dnl # --with-zfs=yes    headers in one of the following places.  Because zfs
dnl #                   support was explicitly required if the headers cannot
dnl #                   be located it is treated as a fatal error.
dnl #
dnl #                       * /var/lib/dkms/zfs/${VERSION}/build
dnl #                       * /usr/src/zfs-${VERSION}/${LINUXRELEASE}
dnl #                       * /usr/src/zfs-${VERSION}
dnl #                       * ../spl/
dnl #                       * $LINUX
dnl #
dnl # --with-zfs=path - Enable zfs support and use the zfs headers in the
dnl #                   provided path.  No autodetection is performed and
dnl #                   if no headers are found this is a fatal error.
dnl #
dnl # --with-zfs-obj  - When zfs support is enabled the object directory
dnl #                   will be based on the --with-zfs directory.  If this
dnl #                   is detected incorrectly it can be explicitly
dnl #                   specified using this option.
dnl #
dnl # --without-spl   - Disable spl support.
dnl # --with-spl=no
dnl #
dnl # --with-spl      - Enable spl support and attempt to autodetect the spl
dnl # --with-spl=yes    headers in one of the following places in this order:
dnl #                   * /var/lib/dkms/spl/${VERSION}/build
dnl #                   * /usr/src/spl-${VERSION}/${LINUXRELEASE}
dnl #                   * /usr/src/spl-${VERSION}
dnl #                   * ../spl/
dnl #                   * $LINUX
dnl #
dnl # --with-spl=path - Enable spl support and use the spl headers in the
dnl #                   provided path.  No autodetection is performed.
dnl #
dnl # --with-spl-obj  - When spl support is enabled the object directory
dnl #                   will be based on the --with-spl directory.  If this
dnl #                   is detected incorrectly it can be explicitly
dnl #                   specified using this option.
dnl #
AC_DEFUN([LB_SPL], [
	AC_ARG_WITH([spl],
		AS_HELP_STRING([--with-spl=PATH],
		[Path to spl source]),
		[splsrc="$withval"])

	AC_ARG_WITH([spl-obj],
		AS_HELP_STRING([--with-spl-obj=PATH],
		[Path to spl build objects]),
		[splobj="$withval"])

	dnl #
	dnl # The existence of spl.release[.in] is used to identify a valid
	dnl # source directory.  In order of preference:
	dnl #
	splver=$(ls -1 /usr/src/ | grep -m1 spl | cut -f2 -d'-')
	splsrc0="/var/lib/dkms/spl/${splver}/build"
	splsrc1="/usr/src/spl-${splver}/${LINUXRELEASE}"
	splsrc2="/usr/src/spl-${splver}"
	splsrc3="../spl/"
	splsrc4="$LINUX"

	AC_MSG_CHECKING([spl source directory])
	AS_IF([test -z "${splsrc}"], [
		AS_IF([test -e "${splsrc0}/spl.release.in"], [
			splsrc=${splsrc0}
		], [test -e "${splsrc1}/spl.release.in"], [
			splsrc=${splsrc1}
		], [test -e "${splsrc2}/spl.release.in"], [
			splsrc=${splsrc2}
		], [test -e "${splsrc3}/spl.release.in"], [
			splsrc=$(readlink -f "${splsrc3}")
		], [test -e "${splsrc4}/spl.release.in"], [
			splsrc=${splsrc4}
		], [
			splsrc="[Not found]"
		])
	])
	AC_MSG_RESULT([$splsrc])

	AS_IF([test ! -e "$splsrc/spl.release" &&
	    test ! -e "$splsrc/spl.release.in"], [
		enable_zfs=no
	])

	dnl #
	dnl # The existence of the spl_config.h is used to identify a valid
	dnl # spl object directory.  In many cases the object and source
	dnl # directory are the same, however the objects may also reside
	dnl # is a subdirectory named after the kernel version.
	dnl #
	AC_MSG_CHECKING([spl build directory])
	AS_IF([test -z "$splobj"], [
		AS_IF([test -e "${splsrc}/${LINUXRELEASE}/spl_config.h"], [
			splobj="${splsrc}/${LINUXRELEASE}"
		], [test -e "${splsrc}/spl_config.h"], [
			splobj="${splsrc}"
		], [
			splobj="[Not found]"
		])
	])
	AC_MSG_RESULT([$splobj])

	AS_IF([test ! -e "$splobj/spl_config.h"], [
		enable_zfs=no
	])

	dnl #
	dnl # Verify the source version using SPL_META_VERSION in spl_config.h
	dnl #
	AS_IF([test x$enable_zfs = xyes], [
		AC_MSG_CHECKING([spl source version])
		AS_IF([fgrep -q SPL_META_VERSION $splobj/spl_config.h], [
			splver=$((echo "#include <spl_config.h>";
			    echo "splver=SPL_META_VERSION-SPL_META_RELEASE") |
			    cpp -I $splobj |
			    grep "^splver=" | tr -d \" | cut -d= -f2)
		], [
			splver="[Not found]"
			enable_zfs=no
		])
		AC_MSG_RESULT([$splver])
	])

	dnl #
	dnl # Verify the modules systems exist by the expect name.
	dnl #
	AS_IF([test x$enable_zfs = xyes], [
		AC_MSG_CHECKING([spl file name for module symbols])
		AS_IF([test -r $splobj/$SYMVERFILE], [
			splsym=$SYMVERFILE
		], [test -r $splobj/module/$SYMVERFILE], [
			splsym=$SYMVERFILE
		], [
			splsym="[Not found]"
			enable_zfs=no
		])
		AC_MSG_RESULT([$splsym])
	])

	SPL=${splsrc}
	SPL_OBJ=${splobj}
	SPL_VERSION=${splver}
	SPL_SYMBOLS=${splsym}

	AC_SUBST(SPL)
	AC_SUBST(SPL_OBJ)
	AC_SUBST(SPL_VERSION)
	AC_SUBST(SPL_SYMBOLS)
])

AC_DEFUN([LB_ZFS], [
	AC_ARG_WITH([zfs-obj],
		AS_HELP_STRING([--with-zfs-obj=PATH],
		[Path to zfs build objects]),
		[zfsobj="$withval"])

	dnl #
	dnl # The existence of zfs.release[.in] is used to identify a valid
	dnl # source directory.  In order of preference:
	dnl #
	zfsver=$(ls -1 /usr/src/ | grep -m1 zfs | cut -f2 -d'-')
	zfssrc0="/var/lib/dkms/zfs/${zfsver}/build"
	zfssrc1="/usr/src/zfs-${zfsver}/${LINUXRELEASE}"
	zfssrc2="/usr/src/zfs-${zfsver}"
	zfssrc3="../zfs/"
	zfssrc4="$LINUX"

	AC_MSG_CHECKING([zfs source directory])
	AS_IF([test -z "${zfssrc}"], [
		AS_IF([test -e "${zfssrc0}/zfs.release.in"], [
			zfssrc=${zfssrc0}
		], [test -e "${zfssrc1}/zfs.release.in"], [
			zfssrc=${zfssrc1}
		], [test -e "${zfssrc2}/zfs.release.in"], [
			zfssrc=${zfssrc2}
		], [test -e "${zfssrc3}/zfs.release.in"], [
			zfssrc=$(readlink -f "${zfssrc3}")
		], [test -e "${zfssrc4}/zfs.release.in"], [
			zfssrc=${zfssrc4}
		], [
			zfssrc="[Not found]"
		])
	])
	AC_MSG_RESULT([$zfssrc])

	AS_IF([test ! -e "$zfssrc/zfs.release.in" &&
	    test ! -e "$zfssrc/zfs.release"], [
		enable_zfs=no
	])

	dnl #
	dnl # The existence of the zfs_config.h is used to identify a valid
	dnl # zfs object directory.  In many cases the object and source
	dnl # directory are the same, however the objects may also reside
	dnl # is a subdirectory named after the kernel version.
	dnl #
	AC_MSG_CHECKING([zfs build directory])
	AS_IF([test -z "$zfsobj"], [
		AS_IF([test -e "${zfssrc}/${LINUXRELEASE}/zfs_config.h"], [
			zfsobj="${zfssrc}/${LINUXRELEASE}"
		], [test -e "${zfssrc}/zfs_config.h"], [
			zfsobj="${zfssrc}"
		], [
			zfsobj="[Not found]"
		])
	])

	AC_MSG_RESULT([$zfsobj])
	AS_IF([test ! -e "$zfsobj/zfs_config.h"], [
		enable_zfs=no
	])

	dnl #
	dnl # Verify the source version using SPL_META_VERSION in spl_config.h
	dnl #
	AS_IF([test x$enable_zfs = xyes], [
		AC_MSG_CHECKING([zfs source version])
		AS_IF([fgrep -q ZFS_META_VERSION $zfsobj/zfs_config.h], [
			zfsver=$((echo "#include <zfs_config.h>";
			    echo "zfsver=ZFS_META_VERSION-ZFS_META_RELEASE") |
			    cpp -I $zfsobj |
			    grep "^zfsver=" | tr -d \" | cut -d= -f2)
		],[
			zfsver="[Not found]"
			enable_zfs=no
		])
		AC_MSG_RESULT([$zfsver])
	])

	dnl #
	dnl # Verify the modules systems exist by the expect name.
	dnl #
	AS_IF([test x$enable_zfs = xyes], [
		AC_MSG_CHECKING([zfs file name for module symbols])
		AS_IF([test -r $zfsobj/$SYMVERFILE], [
			zfssym=$SYMVERFILE
		], [test -r $zfsobj/module/$SYMVERFILE], [
			zfssym=$SYMVERFILE
		], [
			zfssym="[Not found]"
			enable_zfs=no
		])
		AC_MSG_RESULT([$zfssym])
	])

	ZFS=${zfssrc}
	ZFS_OBJ=${zfsobj}
	ZFS_VERSION=${zfsver}
	ZFS_SYMBOLS=${zfssym}

	AC_SUBST(ZFS)
	AC_SUBST(ZFS_OBJ)
	AC_SUBST(ZFS_VERSION)
	AC_SUBST(ZFS_SYMBOLS)
])

AC_DEFUN([LB_ZFS_USER], [
	dnl #
	dnl # Detect user space zfs development headers.
	dnl #
	AC_MSG_CHECKING([zfs devel headers])
	AS_IF([test -d /usr/include/libzfs && test -d /usr/include/libspl], [
		zfslib="-I /usr/include/libspl -I /usr/include/libzfs"
	], [
		zfslib="[Not found]"
		enable_zfs=no
	])
	AC_MSG_RESULT([$zfslib])

	ZFS_LIBZFS_INCLUDE=${zfslib}
	AC_SUBST(ZFS_LIBZFS_INCLUDE)
])

AC_DEFUN([LB_PATH_ZFS], [
	AC_ARG_WITH([zfs],
		[AS_HELP_STRING([--with-zfs=PATH], [Path to zfs source])],
		[
			AS_IF([test x$withval = xno], [
				enable_zfs=no
				require_zfs=no
			], [test x$withval = xyes], [
				enable_zfs=yes
				require_zfs=yes
			], [
				enable_zfs=yes
				require_zfs=yes
				zfssrc="$withval"
			])
		], [
			AS_IF([test x$enable_server = xyes &&
			    test x$enable_dist = xno], [
				require_zfs=no
				enable_zfs=yes
			], [
				require_zfs=no
				enable_zfs=no
			])
		])

	AC_MSG_CHECKING([whether to enable zfs])
	AC_MSG_RESULT([$enable_zfs])

	AS_IF([test x$enable_zfs = xyes], [
		AS_IF([test x$enable_modules = xyes], [
			LB_SPL
			LB_ZFS
		])

		LB_ZFS_USER

		dnl #
		dnl # enable_zfs will be set to no in LB_SPL or LB_ZFS if
		dnl # one of more of the build requirements is not met.
		dnl #
		AS_IF([test x$enable_zfs = xyes], [
			AC_DEFINE(HAVE_ZFS_OSD, 1, Enable zfs osd)
		],[
			AS_IF([test x$require_zfs = xyes], [
				AC_MSG_ERROR([

Required zfs osd cannot be built due to missing zfs development headers.

Support for zfs can be enabled by downloading the required packages for your
distribution.  See http://zfsonlinux.org/ to determine is zfs is supported by
your distribution.
				])
			], [
				AC_MSG_WARN([

Disabling optional zfs osd due to missing development headers.

Support for zfs can be enabled by downloading the required packages for your
distribution.  See http://zfsonlinux.org/ to determine is zfs is supported by
your distribution.
				])
			])
		])
	])

	AM_CONDITIONAL(ZFS_ENABLED, test x$enable_zfs = xyes)
])
