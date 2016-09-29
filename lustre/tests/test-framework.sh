#!/bin/bash

trap 'print_summary && touch $TF_FAIL && \
    echo "test-framework exiting on error"' ERR
set -e
#set -x

export LANG=en_US
export REFORMAT=${REFORMAT:-""}
export WRITECONF=${WRITECONF:-""}
export VERBOSE=${VERBOSE:-false}
export GSS=false
export GSS_KRB5=false
export GSS_PIPEFS=false
export IDENTITY_UPCALL=default
export QUOTA_AUTO=1
# specify environment variable containing batch job name for server statistics
export JOBID_VAR=${JOBID_VAR:-"procname_uid"}  # or "existing" or "disable"

# LOAD_LLOOP: LU-409: only load llite_lloop module if kernel < 2.6.32 or
#             LOAD_LLOOP is true. LOAD_LLOOP is false by default.
export LOAD_LLOOP=${LOAD_LLOOP:-false}

#export PDSH="pdsh -S -Rssh -w"
export MOUNT_CMD=${MOUNT_CMD:-"mount -t lustre"}
export UMOUNT=${UMOUNT:-"umount -d"}
# sles12 umount has a issue with -d option
[ -e /etc/SuSE-release ] && grep -w VERSION /etc/SuSE-release | grep -wq 12 && {
	export UMOUNT="umount"
}

# function used by scripts run on remote nodes
LUSTRE=${LUSTRE:-$(cd $(dirname $0)/..; echo $PWD)}
. $LUSTRE/tests/functions.sh
. $LUSTRE/tests/yaml.sh

export LD_LIBRARY_PATH=${LUSTRE}/utils:${LD_LIBRARY_PATH}

LUSTRE_TESTS_CFG_DIR=${LUSTRE_TESTS_CFG_DIR:-${LUSTRE}/tests/cfg}

EXCEPT_LIST_FILE=${EXCEPT_LIST_FILE:-${LUSTRE_TESTS_CFG_DIR}/tests-to-skip.sh}

if [ -f "$EXCEPT_LIST_FILE" ]; then
    echo "Reading test skip list from $EXCEPT_LIST_FILE"
    cat $EXCEPT_LIST_FILE
    . $EXCEPT_LIST_FILE
fi

# check config files for options in decreasing order of preference
[ -z "$MODPROBECONF" -a -f /etc/modprobe.d/lustre.conf ] &&
    MODPROBECONF=/etc/modprobe.d/lustre.conf
[ -z "$MODPROBECONF" -a -f /etc/modprobe.d/Lustre ] &&
    MODPROBECONF=/etc/modprobe.d/Lustre
[ -z "$MODPROBECONF" -a -f /etc/modprobe.conf ] &&
    MODPROBECONF=/etc/modprobe.conf

assert_DIR () {
    local failed=""
    [[ $DIR/ = $MOUNT/* ]] || \
        { failed=1 && echo "DIR=$DIR not in $MOUNT. Aborting."; }
    [[ $DIR1/ = $MOUNT1/* ]] || \
        { failed=1 && echo "DIR1=$DIR1 not in $MOUNT1. Aborting."; }
    [[ $DIR2/ = $MOUNT2/* ]] || \
        { failed=1 && echo "DIR2=$DIR2 not in $MOUNT2. Aborting"; }

    [ -n "$failed" ] && exit 99 || true
}

usage() {
    echo "usage: $0 [-r] [-f cfgfile]"
    echo "       -r: reformat"

    exit
}

print_summary () {
    trap 0
	[ -z "$DEFAULT_SUITES"] && return 0
    [ -n "$ONLY" ] && echo "WARNING: ONLY is set to $(echo $ONLY)"
    local details
    local form="%-13s %-17s %-9s %s %s\n"
    printf "$form" "status" "script" "Total(sec)" "E(xcluded) S(low)"
    echo "------------------------------------------------------------------------------------"
    for O in $DEFAULT_SUITES; do
        O=$(echo $O  | tr "-" "_" | tr "[:lower:]" "[:upper:]")
        [ "${!O}" = "no" ] && continue || true
        local o=$(echo $O  | tr "[:upper:]_" "[:lower:]-")
        local log=${TMP}/${o}.log
        if is_sanity_benchmark $o; then
            log=${TMP}/sanity-benchmark.log
        fi
        local slow=
        local skipped=
        local total=
        local status=Unfinished
        if [ -f $log ]; then
		skipped=$(grep excluded $log | awk '{ printf " %s", $3 }' |
			sed 's/test_//g')
		slow=$(egrep "^PASS|^FAIL" $log | tr -d "("| sed s/s\)$//g |
			sort -nr -k 3  | head -n5 |  awk '{ print $2":"$3"s" }')
		total=$(grep duration $log | awk '{ print $2 }')
		if [ "${!O}" = "done" ]; then
			status=Done
		fi
		if $DDETAILS; then
			local durations=$(egrep "^PASS|^FAIL" $log |
				tr -d "("| sed s/s\)$//g |
				awk '{ print $2":"$3"|" }')
			details=$(printf "%s\n%s %s %s\n" "$details" \
				"DDETAILS" "$O" "$(echo $durations)")
		fi
        fi
        printf "$form" $status "$O" "${total}" "E=$skipped"
        printf "$form" "-" "-" "-" "S=$(echo $slow)"
    done

    for O in $DEFAULT_SUITES; do
        O=$(echo $O  | tr "-" "_" | tr "[:lower:]" "[:upper:]")
        if [ "${!O}" = "no" ]; then
            printf "$form" "Skipped" "$O" ""
        fi
    done

    # print the detailed tests durations if DDETAILS=true
    if $DDETAILS; then
        echo "$details"
    fi
}

init_test_env() {
	export LUSTRE=$(absolute_path $LUSTRE)
	export TESTSUITE=$(basename $0 .sh)
	export TEST_FAILED=false
	export FAIL_ON_SKIP_ENV=${FAIL_ON_SKIP_ENV:-false}
	export RPC_MODE=${RPC_MODE:-false}

    export MKE2FS=$MKE2FS
    if [ -z "$MKE2FS" ]; then
        if which mkfs.ldiskfs >/dev/null 2>&1; then
            export MKE2FS=mkfs.ldiskfs
        else
            export MKE2FS=mke2fs
        fi
    fi

    export DEBUGFS=$DEBUGFS
    if [ -z "$DEBUGFS" ]; then
        if which debugfs.ldiskfs >/dev/null 2>&1; then
            export DEBUGFS=debugfs.ldiskfs
        else
            export DEBUGFS=debugfs
        fi
    fi

    export TUNE2FS=$TUNE2FS
    if [ -z "$TUNE2FS" ]; then
        if which tunefs.ldiskfs >/dev/null 2>&1; then
            export TUNE2FS=tunefs.ldiskfs
        else
            export TUNE2FS=tune2fs
        fi
    fi

    export E2LABEL=$E2LABEL
    if [ -z "$E2LABEL" ]; then
        if which label.ldiskfs >/dev/null 2>&1; then
            export E2LABEL=label.ldiskfs
        else
            export E2LABEL=e2label
        fi
    fi

    export DUMPE2FS=$DUMPE2FS
    if [ -z "$DUMPE2FS" ]; then
        if which dumpfs.ldiskfs >/dev/null 2>&1; then
            export DUMPE2FS=dumpfs.ldiskfs
        else
            export DUMPE2FS=dumpe2fs
        fi
    fi

    export E2FSCK=$E2FSCK
    if [ -z "$E2FSCK" ]; then
        if which fsck.ldiskfs >/dev/null 2>&1; then
            export E2FSCK=fsck.ldiskfs
        else
            export E2FSCK=e2fsck
        fi
    fi

	export RESIZE2FS=$RESIZE2FS
	if [ -z "$RESIZE2FS" ]; then
		if which resizefs.ldiskfs >/dev/null 2>&1; then
			export RESIZE2FS=resizefs.ldiskfs
		else
			export RESIZE2FS=resize2fs
		fi
	fi

	export LFSCK_ALWAYS=${LFSCK_ALWAYS:-"no"} # check fs after test suite
	export FSCK_MAX_ERR=4   # File system errors left uncorrected

	export ZFS=${ZFS:-zfs}
	export ZPOOL=${ZPOOL:-zpool}
	export ZDB=${ZDB:-zdb}
	export PARTPROBE=${PARTPROBE:-partprobe}

    #[ -d /r ] && export ROOT=${ROOT:-/r}
    export TMP=${TMP:-$ROOT/tmp}
    export TESTSUITELOG=${TMP}/${TESTSUITE}.log
    export LOGDIR=${LOGDIR:-${TMP}/test_logs/$(date +%s)}
    export TESTLOG_PREFIX=$LOGDIR/$TESTSUITE

    export HOSTNAME=${HOSTNAME:-$(hostname -s)}
    if ! echo $PATH | grep -q $LUSTRE/utils; then
        export PATH=$LUSTRE/utils:$PATH
    fi
    if ! echo $PATH | grep -q $LUSTRE/utils/gss; then
        export PATH=$LUSTRE/utils/gss:$PATH
    fi
    if ! echo $PATH | grep -q $LUSTRE/tests; then
        export PATH=$LUSTRE/tests:$PATH
    fi
    if ! echo $PATH | grep -q $LUSTRE/../lustre-iokit/sgpdd-survey; then
        export PATH=$LUSTRE/../lustre-iokit/sgpdd-survey:$PATH
    fi
    export LST=${LST:-"$LUSTRE/../lnet/utils/lst"}
    [ ! -f "$LST" ] && export LST=$(which lst)
    export SGPDDSURVEY=${SGPDDSURVEY:-"$LUSTRE/../lustre-iokit/sgpdd-survey/sgpdd-survey")}
    [ ! -f "$SGPDDSURVEY" ] && export SGPDDSURVEY=$(which sgpdd-survey)
	export MCREATE=${MCREATE:-mcreate}
    # Ubuntu, at least, has a truncate command in /usr/bin
    # so fully path our truncate command.
    export TRUNCATE=${TRUNCATE:-$LUSTRE/tests/truncate}
	export FSX=${FSX:-$LUSTRE/tests/fsx}
    export MDSRATE=${MDSRATE:-"$LUSTRE/tests/mpi/mdsrate"}
    [ ! -f "$MDSRATE" ] && export MDSRATE=$(which mdsrate 2> /dev/null)
    if ! echo $PATH | grep -q $LUSTRE/tests/racer; then
        export PATH=$LUSTRE/tests/racer:$PATH:
    fi
    if ! echo $PATH | grep -q $LUSTRE/tests/mpi; then
        export PATH=$LUSTRE/tests/mpi:$PATH
    fi
    export RSYNC_RSH=${RSYNC_RSH:-rsh}

    export LCTL=${LCTL:-"$LUSTRE/utils/lctl"}
    [ ! -f "$LCTL" ] && export LCTL=$(which lctl)
    export LFS=${LFS:-"$LUSTRE/utils/lfs"}
    [ ! -f "$LFS" ] && export LFS=$(which lfs)
    SETSTRIPE=${SETSTRIPE:-"$LFS setstripe"}
    GETSTRIPE=${GETSTRIPE:-"$LFS getstripe"}

    export L_GETIDENTITY=${L_GETIDENTITY:-"$LUSTRE/utils/l_getidentity"}
    if [ ! -f "$L_GETIDENTITY" ]; then
        if `which l_getidentity > /dev/null 2>&1`; then
            export L_GETIDENTITY=$(which l_getidentity)
        else
            export L_GETIDENTITY=NONE
        fi
    fi
    export LL_DECODE_FILTER_FID=${LL_DECODE_FILTER_FID:-"$LUSTRE/utils/ll_decode_filter_fid"}
    [ ! -f "$LL_DECODE_FILTER_FID" ] && export LL_DECODE_FILTER_FID="ll_decode_filter_fid"
    export MKFS=${MKFS:-"$LUSTRE/utils/mkfs.lustre"}
    [ ! -f "$MKFS" ] && export MKFS="mkfs.lustre"
    export TUNEFS=${TUNEFS:-"$LUSTRE/utils/tunefs.lustre"}
    [ ! -f "$TUNEFS" ] && export TUNEFS="tunefs.lustre"
    export CHECKSTAT="${CHECKSTAT:-"checkstat -v"} "
    export LUSTRE_RMMOD=${LUSTRE_RMMOD:-$LUSTRE/scripts/lustre_rmmod}
    [ ! -f "$LUSTRE_RMMOD" ] &&
        export LUSTRE_RMMOD=$(which lustre_rmmod 2> /dev/null)
    export LFS_MIGRATE=${LFS_MIGRATE:-$LUSTRE/scripts/lfs_migrate}
    [ ! -f "$LFS_MIGRATE" ] &&
        export LFS_MIGRATE=$(which lfs_migrate 2> /dev/null)
    export LR_READER=${LR_READER:-"$LUSTRE/utils/lr_reader"}
    [ ! -f "$LR_READER" ] && export LR_READER=$(which lr_reader 2> /dev/null)
    [ -z "$LR_READER" ] && export LR_READER="/usr/sbin/lr_reader"
    export NAME=${NAME:-local}
    export LGSSD=${LGSSD:-"$LUSTRE/utils/gss/lgssd"}
    [ "$GSS_PIPEFS" = "true" ] && [ ! -f "$LGSSD" ] && \
        export LGSSD=$(which lgssd)
    export LSVCGSSD=${LSVCGSSD:-"$LUSTRE/utils/gss/lsvcgssd"}
    [ ! -f "$LSVCGSSD" ] && export LSVCGSSD=$(which lsvcgssd 2> /dev/null)
    export KRB5DIR=${KRB5DIR:-"/usr/kerberos"}
    export DIR2
    export SAVE_PWD=${SAVE_PWD:-$LUSTRE/tests}
    export AT_MAX_PATH

    if [ "$ACCEPTOR_PORT" ]; then
        export PORT_OPT="--port $ACCEPTOR_PORT"
    fi

    case "x$SEC" in
        xkrb5*)
            echo "Using GSS/krb5 ptlrpc security flavor"
            which lgss_keyring > /dev/null 2>&1 || \
                error_exit "built with gss disabled! SEC=$SEC"
            GSS=true
            GSS_KRB5=true
            ;;
    esac

    case "x$IDUP" in
        xtrue)
            IDENTITY_UPCALL=true
            ;;
        xfalse)
            IDENTITY_UPCALL=false
            ;;
    esac

    export LOAD_MODULES_REMOTE=${LOAD_MODULES_REMOTE:-false}

    # Paths on remote nodes, if different
    export RLUSTRE=${RLUSTRE:-$LUSTRE}
    export RPWD=${RPWD:-$PWD}
    export I_MOUNTED=${I_MOUNTED:-"no"}
	if [ ! -f /lib/modules/$(uname -r)/kernel/fs/lustre/mdt.ko -a \
	     ! -f /lib/modules/$(uname -r)/updates/kernel/fs/lustre/mdt.ko -a \
	     ! -f /lib/modules/$(uname -r)/extra/kernel/fs/lustre/mdt.ko -a \
	     ! -f $LUSTRE/mdt/mdt.ko ]; then
	    export CLIENTMODSONLY=yes
	fi

	export SHUTDOWN_ATTEMPTS=${SHUTDOWN_ATTEMPTS:-3}
	export OSD_TRACK_DECLARES_LBUG=${OSD_TRACK_DECLARES_LBUG:-"yes"}

    # command line

    while getopts "rvwf:" opt $*; do
        case $opt in
            f) CONFIG=$OPTARG;;
            r) REFORMAT=--reformat;;
            v) VERBOSE=true;;
            w) WRITECONF=writeconf;;
            \?) usage;;
        esac
    done

    shift $((OPTIND - 1))
    ONLY=${ONLY:-$*}

	# print the durations of each test if "true"
	DDETAILS=${DDETAILS:-false}
	[ "$TESTSUITELOG" ] && rm -f $TESTSUITELOG || true
	if ! $RPC_MODE; then
		rm -f $TMP/*active
	fi

	export TF_FAIL=${TF_FAIL:-$TMP/tf.fail}
}

check_cpt_number() {
	local facet=$1
	local ncpts

	ncpts=$(do_facet $facet "lctl get_param -n " \
		"cpu_partition_table 2>/dev/null| wc -l" || echo 1)

	if [ $ncpts -eq 0 ]; then
		echo "1"
	else
		echo $ncpts
	fi
}

# Return a numeric version code based on a version string.  The version
# code is useful for comparison two version strings to see which is newer.
version_code() {
	# split arguments like "1.8.6-wc3" into "1", "8", "6", "wc3"
	eval set -- $(tr "[:punct:]" " " <<< $*)

	echo -n "$((($1 << 16) | ($2 << 8) | $3))"
}

export LINUX_VERSION=$(uname -r | sed -e "s/\([0-9]*\.[0-9]*\.[0-9]*\).*/\1/")
export LINUX_VERSION_CODE=$(version_code ${LINUX_VERSION//\./ })

# Report the Lustre build version string (e.g. 1.8.7.3 or 2.4.1).
#
# usage: lustre_build_version
#
# All Lustre versions support "lctl get_param" to report the version of the
# code running in the kernel (what our tests are interested in), but it
# doesn't work without modules loaded.  If that fails, use "lctl version"
# instead, which is easy to parse and works without the kernel modules,
# but was only added in 2.6.50.  If that also fails, fall back to calling
# "lctl lustre_build_version" which prints either (or both) the userspace
# and kernel build versions, but is deprecated and should eventually be
# removed.
#
# output: prints version string to stdout in dotted-decimal format
lustre_build_version() {
	local facet=${1:-client}

	# lustre: 2.6.52
	# kernel: patchless_client
	# build: v2_6_92_0-gadb3ee4-2.6.32-431.29.2.el6_lustre.x86_64
	local VER=$(do_facet $facet $LCTL get_param -n version 2> /dev/null |
		    awk '/lustre: / { print $2 }')
	# lctl 2.6.50
	[ -z "$VER" ] && VER=$(do_facet $facet $LCTL --version 2>/dev/null |
			       awk '{ print $2 }')
	# Lustre version: 2.5.3-gfcfd782-CHANGED-2.6.32.26-175.fc12.x86_64
	# lctl   version: 2.5.3-gfcfd782-CHANGED-2.6.32.26-175.fc12.x86_64
	[ -z "$VER" ] && VER=$(do_facet $facet $LCTL lustre_build_version |
			       awk '/version:/ { print $3; exit; }')
	sed -e 's/^v//' -e 's/-.*//' -e 's/_/./g' <<<$VER
}

# Report the Lustre numeric build version code for the supplied facet.
lustre_version_code() {
	version_code $(lustre_build_version $1)
}

module_loaded () {
	/sbin/lsmod | grep -q "^\<$1\>"
}

# Load a module on the system where this is running.
#
# usage: load_module module_name [module arguments for insmod/modprobe]
#
# If module arguments are not given but MODOPTS_<MODULE> is set, then its value
# will be used as the arguments.  Otherwise arguments will be obtained from
# /etc/modprobe.conf, from /etc/modprobe.d/Lustre, or else none will be used.
#
load_module() {
    local optvar
    EXT=".ko"
    module=$1
    shift
    BASE=$(basename $module $EXT)

    module_loaded ${BASE} && return

    # If no module arguments were passed, get them from $MODOPTS_<MODULE>,
    # else from modprobe.conf
    if [ $# -eq 0 ]; then
        # $MODOPTS_<MODULE>; we could use associative arrays, but that's not in
        # Bash until 4.x, so we resort to eval.
        optvar="MODOPTS_$(basename $module | tr a-z A-Z)"
        eval set -- \$$optvar
        if [ $# -eq 0 -a -n "$MODPROBECONF" ]; then
		# Nothing in $MODOPTS_<MODULE>; try modprobe.conf
		local opt
		opt=$(awk -v var="^options $BASE" '$0 ~ var \
			{gsub("'"options $BASE"'",""); print}' $MODPROBECONF)
		set -- $(echo -n $opt)

		# Ensure we have accept=all for lnet
		if [ $(basename $module) = lnet ]; then
			# OK, this is a bit wordy...
			local arg accept_all_present=false

			for arg in "$@"; do
				[ "$arg" = accept=all ] && \
					accept_all_present=true
			done
			$accept_all_present || set -- "$@" accept=all
		fi
		export $optvar="$*"
        fi
    fi

    [ $# -gt 0 ] && echo "${module} options: '$*'"

	# Note that insmod will ignore anything in modprobe.conf, which is why
	# we're passing options on the command-line.
	if [[ "$BASE" == "lnet_selftest" ]] &&
		[[ -f ${LUSTRE}/../lnet/selftest/${module}${EXT} ]]; then
		insmod ${LUSTRE}/../lnet/selftest/${module}${EXT}
	elif [[ -f ${LUSTRE}/${module}${EXT} ]]; then
		[[ "$BASE" != "ptlrpc_gss" ]] || modprobe sunrpc
		insmod ${LUSTRE}/${module}${EXT} "$@"
	else
		# must be testing a "make install" or "rpm" installation
		# note failed to load ptlrpc_gss is considered not fatal
		if [[ "$BASE" == "ptlrpc_gss" ]]; then
			modprobe $BASE "$@" 2>/dev/null ||
				echo "gss/krb5 is not supported"
		else
			modprobe $BASE "$@"
		fi
	fi
}

llite_lloop_enabled() {
    local n1=$(uname -r | cut -d. -f1)
    local n2=$(uname -r | cut -d. -f2)
    local n3=$(uname -r | cut -d- -f1 | cut -d. -f3)

    # load the llite_lloop module for < 2.6.32 kernels
    if [[ $n1 -lt 2 ]] || [[ $n1 -eq 2 && $n2 -lt 6 ]] || \
       [[ $n1 -eq 2 && $n2 -eq 6 && $n3 -lt 32 ]] || \
        $LOAD_LLOOP; then
        return 0
    fi
    return 1
}

load_modules_local() {
	if [ -n "$MODPROBE" ]; then
		# use modprobe
		echo "Using modprobe to load modules"
		return 0
	fi

	echo Loading modules from $LUSTRE

	local ncpus

	if [ -f /sys/devices/system/cpu/online ]; then
		ncpus=$(($(cut -d "-" -f 2 /sys/devices/system/cpu/online) + 1))
		echo "detected $ncpus online CPUs by sysfs"
	else
		ncpus=$(getconf _NPROCESSORS_CONF 2>/dev/null)
		local rc=$?
		if [ $rc -eq 0 ]; then
			echo "detected $ncpus online CPUs by getconf"
		else
			echo "Can't detect number of CPUs"
			ncpus=1
		fi
	fi

	# if there is only one CPU core, libcfs can only create one partition
	# if there is more than 4 CPU cores, libcfs should create multiple CPU
	# partitions. So we just force libcfs to create 2 partitions for
	# system with 2 or 4 cores
	if [ $ncpus -le 4 ] && [ $ncpus -gt 1 ]; then
		# force to enable multiple CPU partitions
		echo "Force libcfs to create 2 CPU partitions"
		MODOPTS_LIBCFS="cpu_npartitions=2 $MODOPTS_LIBCFS"
	else
		echo "libcfs will create CPU partition based on online CPUs"
	fi

	load_module ../libcfs/libcfs/libcfs

    [ "$PTLDEBUG" ] && lctl set_param debug="$PTLDEBUG"
    [ "$SUBSYSTEM" ] && lctl set_param subsystem_debug="${SUBSYSTEM# }"
    load_module ../lnet/lnet/lnet
	case $NETTYPE in
	o2ib)
		LNETLND="o2iblnd/ko2iblnd"
		;;
	*)
		;;
	esac
    LNETLND=${LNETLND:-"socklnd/ksocklnd"}
    load_module ../lnet/klnds/$LNETLND
    load_module obdclass/obdclass
    load_module ptlrpc/ptlrpc
    load_module ptlrpc/gss/ptlrpc_gss
    load_module fld/fld
    load_module fid/fid
    load_module lmv/lmv
    load_module mdc/mdc
    load_module osc/osc
    load_module lov/lov
    load_module mgc/mgc
    load_module obdecho/obdecho
    if ! client_only; then
        SYMLIST=/proc/kallsyms
        grep -q crc16 $SYMLIST || { modprobe crc16 2>/dev/null || true; }
        grep -q -w jbd $SYMLIST || { modprobe jbd 2>/dev/null || true; }
        grep -q -w jbd2 $SYMLIST || { modprobe jbd2 2>/dev/null || true; }
		load_module lfsck/lfsck
		[ "$LQUOTA" != "no" ] && load_module quota/lquota $LQUOTAOPTS
		if [[ $(node_fstypes $HOSTNAME) == *zfs* ]]; then
			modprobe zfs
			load_module osd-zfs/osd_zfs
		fi
		if [[ $(node_fstypes $HOSTNAME) == *ldiskfs* ]]; then
			grep -q exportfs_decode_fh $SYMLIST ||
				{ modprobe exportfs 2> /dev/null || true; }
			load_module ../ldiskfs/ldiskfs
			load_module osd-ldiskfs/osd_ldiskfs
		fi
		load_module mgs/mgs
		load_module mdd/mdd
		load_module mdt/mdt
		load_module ost/ost
		load_module lod/lod
		load_module osp/osp
		load_module ofd/ofd
		load_module osp/osp
    fi

	load_module llite/lustre
	llite_lloop_enabled && load_module llite/llite_lloop
	[ -d /r ] && OGDB=${OGDB:-"/r/tmp"}
	OGDB=${OGDB:-$TMP}
	rm -f $OGDB/ogdb-$HOSTNAME
	$LCTL modules > $OGDB/ogdb-$HOSTNAME

	# 'mount' doesn't look in $PATH, just sbin
	local mount_lustre=$LUSTRE/utils/mount.lustre
	if [ -f $mount_lustre ]; then
		local sbin_mount=$(readlink -f /sbin)/mount.lustre
		if grep -qw "$sbin_mount" /proc/mounts; then
			cmp -s $mount_lustre $sbin_mount || umount $sbin_mount
		fi
		if ! grep -qw "$sbin_mount" /proc/mounts; then
			[ ! -f "$sbin_mount" ] && touch "$sbin_mount"
			if [ ! -s "$sbin_mount" -a -w "$sbin_mount" ]; then
				cat <<- EOF > "$sbin_mount"
				#!/bin/sh
				#STUB MARK
				echo "This $sbin_mount just a mountpoint." 1>&2
				echo "It is never supposed to be run." 1>&2
				logger -p emerg -- "using stub $sbin_mount $@"
				exit 1
				EOF
				chmod a+x $sbin_mount
			fi
			mount --bind $mount_lustre $sbin_mount ||
				error "can't bind $mount_lustre to $sbin_mount"
		fi
	fi
}

load_modules () {
	load_modules_local
	# bug 19124
	# load modules on remote nodes optionally
	# lustre-tests have to be installed on these nodes
	if $LOAD_MODULES_REMOTE; then
		local list=$(comma_list $(remote_nodes_list))
		if [ -n "$list" ]; then
			echo "loading modules on: '$list'"
			do_rpc_nodes "$list" load_modules_local
		fi
	fi
}

check_mem_leak () {
    LEAK_LUSTRE=$(dmesg | tail -n 30 | grep "obd_memory.*leaked" || true)
    LEAK_PORTALS=$(dmesg | tail -n 20 | grep "Portals memory leaked" || true)
    if [ "$LEAK_LUSTRE" -o "$LEAK_PORTALS" ]; then
        echo "$LEAK_LUSTRE" 1>&2
        echo "$LEAK_PORTALS" 1>&2
        mv $TMP/debug $TMP/debug-leak.`date +%s` || true
        echo "Memory leaks detected"
        [ -n "$IGNORE_LEAK" ] && { echo "ignoring leaks" && return 0; } || true
        return 1
    fi
}

unload_modules() {
	wait_exit_ST client # bug 12845

	$LUSTRE_RMMOD ldiskfs || return 2

	if $LOAD_MODULES_REMOTE; then
		local list=$(comma_list $(remote_nodes_list))
		if [ -n "$list" ]; then
			echo "unloading modules on: '$list'"
			do_rpc_nodes "$list" $LUSTRE_RMMOD ldiskfs
			do_rpc_nodes "$list" check_mem_leak
		fi
	fi

	local sbin_mount=$(readlink -f /sbin)/mount.lustre
	if grep -qe "$sbin_mount " /proc/mounts; then
		umount $sbin_mount || true
		[ -s $sbin_mount ] && ! grep -q "STUB MARK" $sbin_mount ||
			rm -f $sbin_mount
	fi

	check_mem_leak || return 254

	echo "modules unloaded."
	return 0
}

fs_log_size() {
	local facet=${1:-$SINGLEMDS}
	local fstype=$(facet_fstype $facet)
	local size=0
	case $fstype in
		ldiskfs) size=50;; # largest seen is 44, leave some headroom
		zfs)     size=400;; # largest seen is 384
	esac

	echo -n $size
}

check_gss_daemon_nodes() {
    local list=$1
    dname=$2

    do_nodesv $list "num=\\\$(ps -o cmd -C $dname | grep $dname | wc -l);
if [ \\\"\\\$num\\\" -ne 1 ]; then
    echo \\\$num instance of $dname;
    exit 1;
fi; "
}

check_gss_daemon_facet() {
    facet=$1
    dname=$2

    num=`do_facet $facet ps -o cmd -C $dname | grep $dname | wc -l`
    if [ $num -ne 1 ]; then
        echo "$num instance of $dname on $facet"
        return 1
    fi
    return 0
}

send_sigint() {
    local list=$1
    shift
    echo Stopping $@ on $list
    do_nodes $list "killall -2 $@ 2>/dev/null || true"
}

# start gss daemons on all nodes, or
# "daemon" on "list" if set
start_gss_daemons() {
    local list=$1
    local daemon=$2

    if [ "$list" ] && [ "$daemon" ] ; then
        echo "Starting gss daemon on nodes: $list"
        do_nodes $list "$daemon" || return 8
        return 0
    fi

    local list=$(comma_list $(mdts_nodes))
    echo "Starting gss daemon on mds: $list"
    do_nodes $list "$LSVCGSSD -v" || return 1
    if $GSS_PIPEFS; then
        do_nodes $list "$LGSSD -v" || return 2
    fi

    list=$(comma_list $(osts_nodes))
    echo "Starting gss daemon on ost: $list"
    do_nodes $list "$LSVCGSSD -v" || return 3
    # starting on clients

    local clients=${CLIENTS:-`hostname`}
    if $GSS_PIPEFS; then
        echo "Starting $LGSSD on clients $clients "
        do_nodes $clients  "$LGSSD -v" || return 4
    fi

    # wait daemons entering "stable" status
    sleep 5

    #
    # check daemons are running
    #
    list=$(comma_list $(mdts_nodes) $(osts_nodes))
    check_gss_daemon_nodes $list lsvcgssd || return 5
    if $GSS_PIPEFS; then
        list=$(comma_list $(mdts_nodes))
        check_gss_daemon_nodes $list lgssd || return 6
    fi
    if $GSS_PIPEFS; then
        check_gss_daemon_nodes $clients lgssd || return 7
    fi
}

stop_gss_daemons() {
    local list=$(comma_list $(mdts_nodes))

    send_sigint $list lsvcgssd lgssd

    list=$(comma_list $(osts_nodes))
    send_sigint $list lsvcgssd

    list=${CLIENTS:-`hostname`}
    send_sigint $list lgssd
}

init_gss() {
    if $GSS; then
        if ! module_loaded ptlrpc_gss; then
            load_module ptlrpc/gss/ptlrpc_gss
            module_loaded ptlrpc_gss ||
                error_exit "init_gss : GSS=$GSS, but gss/krb5 is not supported!"
        fi
        if $GSS_KRB5; then
                start_gss_daemons || error_exit "start gss daemon failed! rc=$?"
        fi

        if [ -n "$LGSS_KEYRING_DEBUG" ]; then
		lctl set_param -n \
		    sptlrpc.gss.lgss_keyring.debug_level=$LGSS_KEYRING_DEBUG
        fi
    fi
}

cleanup_gss() {
    if $GSS; then
        stop_gss_daemons
        # maybe cleanup credential cache?
    fi
}

facet_svc() {
	local facet=$1
	local var=${facet}_svc

	echo -n ${!var}
}

facet_type() {
	local facet=$1

	echo -n $facet | sed -e 's/^fs[0-9]\+//' -e 's/[0-9_]\+//' |
		tr '[:lower:]' '[:upper:]'
}

facet_number() {
	local facet=$1

	if [ $facet == mgs ]; then
		return 1
	fi

	echo -n $facet | sed -e 's/^fs[0-9]\+//' | sed -e 's/^[a-z]\+//'
}

facet_fstype() {
	local facet=$1
	local var

	var=${facet}_FSTYPE
	if [ -n "${!var}" ]; then
		echo -n ${!var}
		return
	fi

	var=$(facet_type $facet)FSTYPE
	if [ -n "${!var}" ]; then
		echo -n ${!var}
		return
	fi

	if [ -n "$FSTYPE" ]; then
		echo -n $FSTYPE
		return
	fi

	if [[ $facet == mgs ]] && combined_mgs_mds; then
		facet_fstype mds1
		return
	fi

	return 1
}

node_fstypes() {
	local node=$1
	local fstypes
	local fstype
	local facets=$(get_facets)
	local facet

	for facet in ${facets//,/ }; do
		if [ $node == $(facet_host $facet) ] ||
		   [ $node == "$(facet_failover_host $facet)" ]; then
			fstype=$(facet_fstype $facet)
			if [[ $fstypes != *$fstype* ]]; then
				fstypes+="${fstypes:+,}$fstype"
			fi
		fi
	done
	echo -n $fstypes
}

facet_index() {
	local facet=$1
	local num=$(facet_number $facet)
	local index

	if [[ $(facet_type $facet) = OST ]]; then
		index=OSTINDEX${num}
		if [[ -n "${!index}" ]]; then
			echo -n ${!index}
			return
		fi

		index=${OST_INDICES[num - 1]}
	fi

	[[ -n "$index" ]] || index=$((num - 1))
	echo -n $index
}

devicelabel() {
	local facet=$1
	local dev=$2
	local label
	local fstype=$(facet_fstype $facet)

	case $fstype in
	ldiskfs)
		label=$(do_facet ${facet} "$E2LABEL ${dev} 2>/dev/null");;
	zfs)
		label=$(do_facet ${facet} "$ZFS get -H -o value lustre:svname \
		                           ${dev} 2>/dev/null");;
	*)
		error "unknown fstype!";;
	esac

	echo -n $label
}

mdsdevlabel() {
	local num=$1
	local device=$(mdsdevname $num)
	local label=$(devicelabel mds$num ${device} | grep -v "CMD: ")
	echo -n $label
}

ostdevlabel() {
	local num=$1
	local device=$(ostdevname $num)
	local label=$(devicelabel ost$num ${device} | grep -v "CMD: ")
	echo -n $label
}

#
# Get the device of a facet.
#
facet_device() {
	local facet=$1
	local device

	case $facet in
		mgs) device=$(mgsdevname) ;;
		mds*) device=$(mdsdevname $(facet_number $facet)) ;;
		ost*) device=$(ostdevname $(facet_number $facet)) ;;
		fs2mds) device=$(mdsdevname 1_2) ;;
		fs2ost) device=$(ostdevname 1_2) ;;
		fs3ost) device=$(ostdevname 2_2) ;;
		*) ;;
	esac

	echo -n $device
}

#
# Get the virtual device of a facet.
#
facet_vdevice() {
	local facet=$1
	local device

	case $facet in
		mgs) device=$(mgsvdevname) ;;
		mds*) device=$(mdsvdevname $(facet_number $facet)) ;;
		ost*) device=$(ostvdevname $(facet_number $facet)) ;;
		fs2mds) device=$(mdsvdevname 1_2) ;;
		fs2ost) device=$(ostvdevname 1_2) ;;
		fs3ost) device=$(ostvdevname 2_2) ;;
		*) ;;
	esac

	echo -n $device
}

#
# Re-read the partition table on failover partner host.
# After a ZFS storage pool is created on a shared device, the partition table
# on the device may change. However, the operating system on the failover
# host may not notice the change automatically. Without the up-to-date partition
# block devices, 'zpool import ..' cannot find the labels, whose positions are
# relative to partition rather than disk beginnings.
#
# This function performs partprobe on the failover host to make it re-read the
# partition table.
#
refresh_partition_table() {
	local facet=$1
	local device=$2
	local host

	host=$(facet_passive_host $facet)
	if [[ -n "$host" ]]; then
		do_node $host "$PARTPROBE $device"
	fi
}

#
# Get ZFS storage pool name.
#
zpool_name() {
	local facet=$1
	local device
	local poolname

	device=$(facet_device $facet)
	# poolname is string before "/"
	poolname="${device%%/*}"

	echo -n $poolname
}

#
# Create ZFS storage pool.
#
create_zpool() {
	local facet=$1
	local poolname=$2
	local vdev=$3
	shift 3
	local opts=${@:-"-o cachefile=none"}

	do_facet $facet "$ZPOOL list -H $poolname >/dev/null 2>&1 ||
		$ZPOOL create -f $opts $poolname $vdev"
}

#
# Create ZFS file system.
#
create_zfs() {
	local facet=$1
	local dataset=$2
	shift 2
	local opts=${@:-"-o mountpoint=legacy"}

	do_facet $facet "$ZFS list -H $dataset >/dev/null 2>&1 ||
		$ZFS create $opts $dataset"
}

#
# Export ZFS storage pool.
# Before exporting the pool, all datasets within the pool should be unmounted.
#
export_zpool() {
	local facet=$1
	shift
	local opts="$@"
	local poolname

	poolname=$(zpool_name $facet)

	if [[ -n "$poolname" ]]; then
		do_facet $facet "! $ZPOOL list -H $poolname >/dev/null 2>&1 ||
			grep -q ^$poolname/ /proc/mounts ||
			$ZPOOL export $opts $poolname"
	fi
}

#
# Destroy ZFS storage pool.
# Destroy the given pool and free up any devices for other use. This command
# tries to unmount any active datasets before destroying the pool.
# -f    Force any active datasets contained within the pool to be unmounted.
#
destroy_zpool() {
	local facet=$1
	local poolname=${2:-$(zpool_name $facet)}

	if [[ -n "$poolname" ]]; then
		do_facet $facet "! $ZPOOL list -H $poolname >/dev/null 2>&1 ||
			$ZPOOL destroy -f $poolname"
	fi
}

#
# Import ZFS storage pool.
# Force importing, even if the pool appears to be potentially active.
#
import_zpool() {
	local facet=$1
	shift
	local opts=${@:-"-o cachefile=none"}
	local poolname

	poolname=$(zpool_name $facet)

	if [[ -n "$poolname" ]]; then
		opts+=" -d $(dirname $(facet_vdevice $facet))"
		do_facet $facet "$ZPOOL list -H $poolname >/dev/null 2>&1 ||
			$ZPOOL import -f $opts $poolname"
	fi
}

#
# Set the "cachefile=none" property on ZFS storage pool so that the pool
# is not automatically imported on system startup.
#
# In a failover environment, this will provide resource level fencing which
# will ensure that the same ZFS storage pool will not be imported concurrently
# on different nodes.
#
disable_zpool_cache() {
	local facet=$1
	local poolname

	poolname=$(zpool_name $facet)

	if [[ -n "$poolname" ]]; then
		do_facet $facet "$ZPOOL set cachefile=none $poolname"
	fi
}

#
# This and set_osd_param() shall be used to access OSD parameters
# once existed under "obdfilter":
#
#   mntdev
#   stats
#   read_cache_enable
#   writethrough_cache_enable
#
get_osd_param() {
	local nodes=$1
	local device=${2:-$FSNAME-OST*}
	local name=$3

	do_nodes $nodes "$LCTL get_param -n obdfilter.$device.$name \
		osd-*.$device.$name 2>&1" | grep -v 'error:'
}

set_osd_param() {
	local nodes=$1
	local device=${2:-$FSNAME-OST*}
	local name=$3
	local value=$4

	do_nodes $nodes "$LCTL set_param -n obdfilter.$device.$name=$value \
		osd-*.$device.$name=$value 2>&1" | grep -v 'error:'
}

set_debug_size () {
    local dz=${1:-$DEBUG_SIZE}

    if [ -f /sys/devices/system/cpu/possible ]; then
        local cpus=$(($(cut -d "-" -f 2 /sys/devices/system/cpu/possible)+1))
    else
        local cpus=$(getconf _NPROCESSORS_CONF)
    fi

    # bug 19944, adjust size to be -gt num_possible_cpus()
    # promise 2MB for every cpu at least
    if [ -n "$cpus" ] && [ $((cpus * 2)) -gt $dz ]; then
        dz=$((cpus * 2))
    fi
    lctl set_param debug_mb=$dz
}

set_default_debug () {
    local debug=${1:-"$PTLDEBUG"}
    local subsys=${2:-"$SUBSYSTEM"}
    local debug_size=${3:-$DEBUG_SIZE}

    [ -n "$debug" ] && lctl set_param debug="$debug" >/dev/null
    [ -n "$subsys" ] && lctl set_param subsystem_debug="${subsys# }" >/dev/null

    [ -n "$debug_size" ] && set_debug_size $debug_size > /dev/null
}

set_default_debug_nodes () {
	local nodes="$1"

	if [[ ,$nodes, = *,$HOSTNAME,* ]]; then
		nodes=$(exclude_items_from_list "$nodes" "$HOSTNAME")
		set_default_debug
	fi

	do_rpc_nodes "$nodes" set_default_debug \
		\\\"$PTLDEBUG\\\" \\\"$SUBSYSTEM\\\" $DEBUG_SIZE || true
}

set_default_debug_facet () {
    local facet=$1
    local node=$(facet_active_host $facet)
    [ -z "$node" ] && echo "No host defined for facet $facet" && exit 1

    set_default_debug_nodes $node
}

# Facet functions
mount_facets () {
	local facets=${1:-$(get_facets)}
	local facet

	for facet in ${facets//,/ }; do
		mount_facet $facet
		local RC=$?
		[ $RC -eq 0 ] && continue

		if [ "$TESTSUITE.$TESTNAME" = "replay-dual.test_0a" ]; then
			skip "Restart of $facet failed!." && touch $LU482_FAILED
		else
			error "Restart of $facet failed!"
		fi
		return $RC
	done
}

#
# Add argument "arg" (e.g., "loop") to the comma-separated list
# of arguments for option "opt" (e.g., "-o") on command
# line "opts" (e.g., "-o flock").
#
csa_add() {
	local opts=$1
	local opt=$2
	local arg=$3
	local opt_pattern="\([[:space:]]\+\|^\)$opt"

	if echo "$opts" | grep -q $opt_pattern; then
		opts=$(echo "$opts" | sed -e \
			"s/$opt_pattern[[:space:]]*[^[:space:]]\+/&,$arg/")
	else
		opts+="${opts:+ }$opt $arg"
	fi
	echo -n "$opts"
}

mount_facet() {
	local facet=$1
	shift
	local dev=$(facet_active $facet)_dev
	local opt=${facet}_opt
	local mntpt=$(facet_mntpt $facet)
	local opts="${!opt} $@"

	module_loaded lustre || load_modules

	if [ $(facet_fstype $facet) == ldiskfs ] &&
	   ! do_facet $facet test -b ${!dev}; then
		opts=$(csa_add "$opts" -o loop)
	fi

	if [[ $(facet_fstype $facet) == zfs ]]; then
		# import ZFS storage pool
		import_zpool $facet || return ${PIPESTATUS[0]}
	fi

	echo "Starting ${facet}: $opts ${!dev} $mntpt"
	# for testing LU-482 error handling in mount_facets() and test_0a()
	if [ -f $TMP/test-lu482-trigger ]; then
		RC=2
	else
		do_facet ${facet} "mkdir -p $mntpt; $MOUNT_CMD $opts \
		                   ${!dev} $mntpt"
		RC=${PIPESTATUS[0]}
	fi

	if [ $RC -ne 0 ]; then
		echo "Start of ${!dev} on ${facet} failed ${RC}"
		return $RC
	fi

	set_default_debug_facet $facet

	if [[ $facet == mds* ]]; then
		do_facet $facet \
		lctl set_param -n mdt.${FSNAME}*.enable_remote_dir=1 2>/dev/null
	fi

	if [[ $opts =~ .*nosvc.* ]]; then
		echo "Start ${!dev} without service"
	else
		local fstype=$(facet_fstype $facet)

		case $fstype in
		ldiskfs)
			wait_update_facet ${facet} "$E2LABEL ${!dev} \
				2>/dev/null | grep -E ':[a-zA-Z]{3}[0-9]{4}'" \
				"" || error "${!dev} failed to initialize!";;
		zfs)
			wait_update_facet ${facet} "$ZFS get -H -o value \
				lustre:svname ${!dev} 2>/dev/null | \
				grep -E ':[a-zA-Z]{3}[0-9]{4}'" "" ||
				error "${!dev} failed to initialize!";;

		*)
			error "unknown fstype!";;
		esac
	fi

	label=$(devicelabel ${facet} ${!dev})
	[ -z "$label" ] && echo no label for ${!dev} && exit 1
	eval export ${facet}_svc=${label}
	echo Started ${label}

	return $RC
}

# start facet device options
start() {
	local facet=$1
	shift
	local device=$1
	shift
	eval export ${facet}_dev=${device}
	eval export ${facet}_opt=\"$@\"

	local varname=${facet}failover_dev
	if [ -n "${!varname}" ] ; then
		eval export ${facet}failover_dev=${!varname}
	else
		eval export ${facet}failover_dev=$device
	fi

	local mntpt=$(facet_mntpt $facet)
	do_facet ${facet} mkdir -p $mntpt
	eval export ${facet}_MOUNT=$mntpt
	mount_facet ${facet}
	RC=$?

	if [[ $facet == mds* ]]; then
		do_facet $facet \
			lctl set_param -n mdt.${FSNAME}*.enable_remote_dir=1 \
				2>/dev/null
	fi

	return $RC
}

stop() {
    local running
    local facet=$1
    shift
    local HOST=`facet_active_host $facet`
    [ -z $HOST ] && echo stop: no host for $facet && return 0

    local mntpt=$(facet_mntpt $facet)
    running=$(do_facet ${facet} "grep -c $mntpt' ' /proc/mounts") || true
    if [ ${running} -ne 0 ]; then
        echo "Stopping $mntpt (opts:$@) on $HOST"
	do_facet ${facet} $UMOUNT $@ $mntpt
    fi

	# umount should block, but we should wait for unrelated obd's
	# like the MGS or MGC to also stop.
	wait_exit_ST ${facet} || return ${PIPESTATUS[0]}

	if [[ $(facet_fstype $facet) == zfs ]]; then
		# export ZFS storage pool
		export_zpool $facet
	fi
}

# save quota version (both administrative and operational quotas)
# add an additional parameter if mountpoint is ever different from $MOUNT
#
# XXX This function is kept for interoperability with old server (< 2.3.50),
#     it should be removed whenever we drop the interoperability for such
#     server.
quota_save_version() {
    local fsname=${2:-$FSNAME}
    local spec=$1
    local ver=$(tr -c -d "123" <<< $spec)
    local type=$(tr -c -d "ug" <<< $spec)

    [ -n "$ver" -a "$ver" != "3" ] && error "wrong quota version specifier"

    [ -n "$type" ] && { $LFS quotacheck -$type $MOUNT || error "quotacheck has failed"; }

    do_facet mgs "lctl conf_param ${fsname}-MDT*.mdd.quota_type=$spec"
    local varsvc
    local osts=$(get_facets OST)
    for ost in ${osts//,/ }; do
        varsvc=${ost}_svc
        do_facet mgs "lctl conf_param ${!varsvc}.ost.quota_type=$spec"
    done
}

# client could mount several lustre
#
# XXX This function is kept for interoperability with old server (< 2.3.50),
#     it should be removed whenever we drop the interoperability for such
#     server.
quota_type() {
	local fsname=${1:-$FSNAME}
	local rc=0
	do_facet $SINGLEMDS lctl get_param mdd.${fsname}-MDT*.quota_type ||
		rc=$?
	do_nodes $(comma_list $(osts_nodes)) \
		lctl get_param obdfilter.${fsname}-OST*.quota_type || rc=$?
	return $rc
}

# get mdt quota type
mdt_quota_type() {
	local varsvc=${SINGLEMDS}_svc
	do_facet $SINGLEMDS $LCTL get_param -n \
		osd-$(facet_fstype $SINGLEMDS).${!varsvc}.quota_slave.enabled
}

# get ost quota type
ost_quota_type() {
	# All OSTs should have same quota type
	local varsvc=ost1_svc
	do_facet ost1 $LCTL get_param -n \
		osd-$(facet_fstype ost1).${!varsvc}.quota_slave.enabled
}

# restore old quota type settings
restore_quota() {
	if [ "$old_MDT_QUOTA_TYPE" ]; then
		do_facet mgs $LCTL conf_param \
			$FSNAME.quota.mdt=$old_MDT_QUOTA_TYPE
	fi
	if [ "$old_OST_QUOTA_TYPE" ]; then
		do_facet mgs $LCTL conf_param \
			$FSNAME.quota.ost=$old_OST_QUOTA_TYPE
	fi
}

# Handle the case when there is a space in the lfs df
# "filesystem summary" line the same as when there is no space.
# This will allow fixing the "lfs df" summary line in the future.
lfs_df() {
	$LFS df $* | sed -e 's/filesystem /filesystem_/'
}

# Get free inodes on the MDT specified by mdt index, free indoes on
# the whole filesystem will be returned when index == -1.
mdt_free_inodes() {
	local index=$1
	local free_inodes
	local mdt_uuid

	if [ $index -eq -1 ]; then
		mdt_uuid="summary"
	else
		mdt_uuid=$(mdtuuid_from_index $index)
	fi

	free_inodes=$(lfs_df -i $MOUNT | grep $mdt_uuid | awk '{print $4}')
	echo $free_inodes
}

setup_quota(){
	local mntpt=$1

	# save old quota type & set new quota type
	local mdt_qtype=$(mdt_quota_type)
	local ost_qtype=$(ost_quota_type)

	echo "[HOST:$HOSTNAME] [old_mdt_qtype:$mdt_qtype]" \
		"[old_ost_qtype:$ost_qtype] [new_qtype:$QUOTA_TYPE]"

	export old_MDT_QUOTA_TYPE=$mdt_qtype
	export old_OST_QUOTA_TYPE=$ost_qtype

	do_facet mgs $LCTL conf_param $FSNAME.quota.mdt=$QUOTA_TYPE ||
		error "set mdt quota type failed"
	do_facet mgs $LCTL conf_param $FSNAME.quota.ost=$QUOTA_TYPE ||
		error "set ost quota type failed"

	local quota_usrs=$QUOTA_USERS

	# get_filesystem_size
	local disksz=$(lfs_df $mntpt | grep "summary" | awk '{print $2}')
	local blk_soft=$((disksz + 1024))
	local blk_hard=$((blk_soft + blk_soft / 20)) # Go 5% over

	local inodes=$(lfs_df -i $mntpt | grep "summary" | awk '{print $2}')
	local i_soft=$inodes
	local i_hard=$((i_soft + i_soft / 20))

	echo "Total disk size: $disksz  block-softlimit: $blk_soft" \
		"block-hardlimit: $blk_hard inode-softlimit: $i_soft" \
		"inode-hardlimit: $i_hard"

	local cmd
	for usr in $quota_usrs; do
		echo "Setting up quota on $HOSTNAME:$mntpt for $usr..."
		for type in u g; do
			cmd="$LFS setquota -$type $usr -b $blk_soft"
			cmd="$cmd -B $blk_hard -i $i_soft -I $i_hard $mntpt"
			echo "+ $cmd"
			eval $cmd || error "$cmd FAILED!"
		done
		# display the quota status
		echo "Quota settings for $usr : "
		$LFS quota -v -u $usr $mntpt || true
	done
}

zconf_mount() {
    local client=$1
    local mnt=$2
    local opts=${3:-$MOUNT_OPTS}
    opts=${opts:+-o $opts}
    local flags=${4:-$MOUNT_FLAGS}

    local device=$MGSNID:/$FSNAME
    if [ -z "$mnt" -o -z "$FSNAME" ]; then
        echo Bad zconf mount command: opt=$flags $opts dev=$device mnt=$mnt
        exit 1
    fi

    echo "Starting client: $client: $flags $opts $device $mnt"
    do_node $client mkdir -p $mnt
    do_node $client $MOUNT_CMD $flags $opts $device $mnt || return 1

    set_default_debug_nodes $client

    return 0
}

zconf_umount() {
    local client=$1
    local mnt=$2
    local force
    local busy 
    local need_kill

    [ "$3" ] && force=-f
    local running=$(do_node $client "grep -c $mnt' ' /proc/mounts") || true
    if [ $running -ne 0 ]; then
        echo "Stopping client $client $mnt (opts:$force)"
        do_node $client lsof -t $mnt || need_kill=no
        if [ "x$force" != "x" -a "x$need_kill" != "xno" ]; then
            pids=$(do_node $client lsof -t $mnt | sort -u);
            if [ -n $pids ]; then
                do_node $client kill -9 $pids || true
            fi
        fi

        busy=$(do_node $client "umount $force $mnt 2>&1" | grep -c "busy") || true
        if [ $busy -ne 0 ] ; then
            echo "$mnt is still busy, wait one second" && sleep 1
            do_node $client umount $force $mnt
        fi
    fi
}

# nodes is comma list
sanity_mount_check_nodes () {
    local nodes=$1
    shift
    local mnts="$@"
    local mnt

    # FIXME: assume that all cluster nodes run the same os
    [ "$(uname)" = Linux ] || return 0

    local rc=0
    for mnt in $mnts ; do
        do_nodes $nodes "running=\\\$(grep -c $mnt' ' /proc/mounts);
mpts=\\\$(mount | grep -c $mnt' ');
if [ \\\$running -ne \\\$mpts ]; then
    echo \\\$(hostname) env are INSANE!;
    exit 1;
fi"
    [ $? -eq 0 ] || rc=1 
    done
    return $rc
}

sanity_mount_check_servers () {
    [ "$CLIENTONLY" ] && 
        { echo "CLIENTONLY mode, skip mount_check_servers"; return 0; } || true
    echo Checking servers environments

    # FIXME: modify get_facets to display all facets wo params
    local facets="$(get_facets OST),$(get_facets MDS),mgs"
    local node
    local mntpt
    local facet
    for facet in ${facets//,/ }; do
        node=$(facet_host ${facet})
        mntpt=$(facet_mntpt $facet)
        sanity_mount_check_nodes $node $mntpt ||
            { error "server $node environments are insane!"; return 1; }
    done
}

sanity_mount_check_clients () {
    local clients=${1:-$CLIENTS}
    local mntpt=${2:-$MOUNT}
    local mntpt2=${3:-$MOUNT2}

    [ -z $clients ] && clients=$(hostname)
    echo Checking clients $clients environments

    sanity_mount_check_nodes $clients $mntpt $mntpt2 ||
       error "clients environments are insane!"
}

sanity_mount_check () {
    sanity_mount_check_servers || return 1
    sanity_mount_check_clients || return 2
}

# mount clients if not mouted
zconf_mount_clients() {
    local clients=$1
    local mnt=$2
    local opts=${3:-$MOUNT_OPTS}
    opts=${opts:+-o $opts}
    local flags=${4:-$MOUNT_FLAGS}

    local device=$MGSNID:/$FSNAME
    if [ -z "$mnt" -o -z "$FSNAME" ]; then
        echo Bad zconf mount command: opt=$flags $opts dev=$device mnt=$mnt
        exit 1
    fi

    echo "Starting client $clients: $flags $opts $device $mnt"

    do_nodes $clients "
running=\\\$(mount | grep -c $mnt' ');
rc=0;
if [ \\\$running -eq 0 ] ; then
    mkdir -p $mnt;
    $MOUNT_CMD $flags $opts $device $mnt;
    rc=\\\$?;
fi;
exit \\\$rc" || return ${PIPESTATUS[0]}

    echo "Started clients $clients: "
    do_nodes $clients "mount | grep $mnt' '"

    set_default_debug_nodes $clients

    return 0
}

zconf_umount_clients() {
    local clients=$1
    local mnt=$2
    local force

    [ "$3" ] && force=-f

    echo "Stopping clients: $clients $mnt (opts:$force)"
    do_nodes $clients "running=\\\$(grep -c $mnt' ' /proc/mounts);
if [ \\\$running -ne 0 ] ; then
echo Stopping client \\\$(hostname) $mnt opts:$force;
lsof $mnt || need_kill=no;
if [ "x$force" != "x" -a "x\\\$need_kill" != "xno" ]; then
    pids=\\\$(lsof -t $mnt | sort -u);
    if [ -n \\\"\\\$pids\\\" ]; then
             kill -9 \\\$pids;
    fi
fi;
while umount $force $mnt 2>&1 | grep -q "busy"; do
    echo "$mnt is still busy, wait one second" && sleep 1;
done;
fi"
}

shutdown_node () {
    local node=$1
    echo + $POWER_DOWN $node
    $POWER_DOWN $node
}

shutdown_node_hard () {
    local host=$1
    local attempts=$SHUTDOWN_ATTEMPTS

    for i in $(seq $attempts) ; do
        shutdown_node $host
        sleep 1
        wait_for_function --quiet "! ping -w 3 -c 1 $host" 5 1 && return 0
        echo "waiting for $host to fail attempts=$attempts"
        [ $i -lt $attempts ] || \
            { echo "$host still pingable after power down! attempts=$attempts" && return 1; } 
    done
}

shutdown_client() {
    local client=$1
    local mnt=${2:-$MOUNT}
    local attempts=3

    if [ "$FAILURE_MODE" = HARD ]; then
        shutdown_node_hard $client
    else
       zconf_umount_clients $client $mnt -f
    fi
}

facets_on_host () {
    local host=$1
    local facets="$(get_facets OST),$(get_facets MDS)"
    local affected

    combined_mgs_mds || facets="$facets,mgs"

    for facet in ${facets//,/ }; do
        if [ $(facet_active_host $facet) == $host ]; then
           affected="$affected $facet"
        fi
    done

    echo $(comma_list $affected)
}

facet_up() {
	local facet=$1
	local host=${2:-$(facet_host $facet)}

	local label=$(convert_facet2label $facet)
	do_node $host $LCTL dl | awk '{ print $4 }' | grep -q "^$label\$"
}

facets_up_on_host () {
    local host=$1
    local facets=$(facets_on_host $host)
    local affected_up

    for facet in ${facets//,/ }; do
        if $(facet_up $facet $host); then
            affected_up="$affected_up $facet"
        fi
    done

    echo $(comma_list $affected_up)
}

shutdown_facet() {
    local facet=$1

    if [ "$FAILURE_MODE" = HARD ]; then
        shutdown_node_hard $(facet_active_host $facet)
    else
        stop $facet
    fi
}

reboot_node() {
    local node=$1
    echo + $POWER_UP $node
    $POWER_UP $node
}

remount_facet() {
    local facet=$1

    stop $facet
    mount_facet $facet
}

reboot_facet() {
	local facet=$1
	if [ "$FAILURE_MODE" = HARD ]; then
		reboot_node $(facet_active_host $facet)
	else
		sleep 10
	fi
}

boot_node() {
    local node=$1
    if [ "$FAILURE_MODE" = HARD ]; then
       reboot_node $node
       wait_for_host $node
    fi
}

facets_hosts () {
    local facets=$1
    local hosts

    for facet in ${facets//,/ }; do
        hosts=$(expand_list $hosts $(facet_host $facet) )
    done

    echo $hosts
}

_check_progs_installed () {
    local progs=$@
    local rc=0

    for prog in $progs; do
        if ! [ "$(which $prog)"  -o  "${!prog}" ]; then
           echo $prog missing on $(hostname)
           rc=1
        fi
    done
    return $rc
}

check_progs_installed () {
	local nodes=$1
	shift

	do_rpc_nodes "$nodes" _check_progs_installed $@
}

# recovery-scale functions
node_var_name() {
    echo __$(echo $1 | tr '-' '_' | tr '.' '_')
}

start_client_load() {
    local client=$1
    local load=$2
    local var=$(node_var_name $client)_load
    eval export ${var}=$load

    do_node $client "PATH=$PATH MOUNT=$MOUNT ERRORS_OK=$ERRORS_OK \
BREAK_ON_ERROR=$BREAK_ON_ERROR \
END_RUN_FILE=$END_RUN_FILE \
LOAD_PID_FILE=$LOAD_PID_FILE \
TESTLOG_PREFIX=$TESTLOG_PREFIX \
TESTNAME=$TESTNAME \
DBENCH_LIB=$DBENCH_LIB \
DBENCH_SRC=$DBENCH_SRC \
CLIENT_COUNT=$((CLIENTCOUNT - 1)) \
LFS=$LFS \
run_${load}.sh" &
    local ppid=$!
    log "Started client load: ${load} on $client"

    # get the children process IDs
    local pids=$(ps --ppid $ppid -o pid= | xargs)
    CLIENT_LOAD_PIDS="$CLIENT_LOAD_PIDS $ppid $pids"
    return 0
}

start_client_loads () {
    local -a clients=(${1//,/ })
    local numloads=${#CLIENT_LOADS[@]}
    local testnum

    for ((nodenum=0; nodenum < ${#clients[@]}; nodenum++ )); do
        testnum=$((nodenum % numloads))
        start_client_load ${clients[nodenum]} ${CLIENT_LOADS[testnum]}
    done
    # bug 22169: wait the background threads to start
    sleep 2
}

# only for remote client
check_client_load () {
    local client=$1
    local var=$(node_var_name $client)_load
    local TESTLOAD=run_${!var}.sh

    ps auxww | grep -v grep | grep $client | grep -q "$TESTLOAD" || return 1

    # bug 18914: try to connect several times not only when
    # check ps, but  while check_catastrophe also
    local tries=3
    local RC=254
    while [ $RC = 254 -a $tries -gt 0 ]; do
        let tries=$tries-1
        # assume success
        RC=0
        if ! check_catastrophe $client; then
            RC=${PIPESTATUS[0]}
            if [ $RC -eq 254 ]; then
                # FIXME: not sure how long we shuold sleep here
                sleep 10
                continue
            fi
            echo "check catastrophe failed: RC=$RC "
            return $RC
        fi
    done
    # We can continue try to connect if RC=254
    # Just print the warning about this
    if [ $RC = 254 ]; then
        echo "got a return status of $RC from do_node while checking catastrophe on $client"
    fi

    # see if the load is still on the client
    tries=3
    RC=254
    while [ $RC = 254 -a $tries -gt 0 ]; do
        let tries=$tries-1
        # assume success
        RC=0
        if ! do_node $client "ps auxwww | grep -v grep | grep -q $TESTLOAD"; then
            RC=${PIPESTATUS[0]}
            sleep 30
        fi
    done
    if [ $RC = 254 ]; then
        echo "got a return status of $RC from do_node while checking (catastrophe and 'ps') the client load on $client"
        # see if we can diagnose a bit why this is
    fi

    return $RC
}
check_client_loads () {
   local clients=${1//,/ }
   local client=
   local rc=0

   for client in $clients; do
      check_client_load $client
      rc=${PIPESTATUS[0]}
      if [ "$rc" != 0 ]; then
        log "Client load failed on node $client, rc=$rc"
        return $rc
      fi
   done
}

restart_client_loads () {
    local clients=${1//,/ }
    local expectedfail=${2:-""}
    local client=
    local rc=0

    for client in $clients; do
        check_client_load $client
        rc=${PIPESTATUS[0]}
        if [ "$rc" != 0 -a "$expectedfail" ]; then
            local var=$(node_var_name $client)_load
            start_client_load $client ${!var}
            echo "Restarted client load ${!var}: on $client. Checking ..."
            check_client_load $client
            rc=${PIPESTATUS[0]}
            if [ "$rc" != 0 ]; then
                log "Client load failed to restart on node $client, rc=$rc"
                # failure one client load means test fail
                # we do not need to check other
                return $rc
            fi
        else
            return $rc
        fi
    done
}

# Start vmstat and save its process ID in a file.
start_vmstat() {
    local nodes=$1
    local pid_file=$2

    [ -z "$nodes" -o -z "$pid_file" ] && return 0

    do_nodes $nodes \
        "vmstat 1 > $TESTLOG_PREFIX.$TESTNAME.vmstat.\\\$(hostname -s).log \
        2>/dev/null </dev/null & echo \\\$! > $pid_file"
}

# Display the nodes on which client loads failed.
print_end_run_file() {
    local file=$1
    local node

    [ -s $file ] || return 0

    echo "Found the END_RUN_FILE file: $file"
    cat $file

    # A client load will stop if it finds the END_RUN_FILE file.
    # That does not mean the client load actually failed though.
    # The first node in END_RUN_FILE is the one we are interested in.
    read node < $file

    if [ -n "$node" ]; then
        local var=$(node_var_name $node)_load

        local prefix=$TESTLOG_PREFIX
        [ -n "$TESTNAME" ] && prefix=$prefix.$TESTNAME
        local stdout_log=$prefix.run_${!var}_stdout.$node.log
        local debug_log=$(echo $stdout_log | sed 's/\(.*\)stdout/\1debug/')

        echo "Client load ${!var} failed on node $node:"
        echo "$stdout_log"
        echo "$debug_log"
    fi
}

# Stop the process which had its PID saved in a file.
stop_process() {
    local nodes=$1
    local pid_file=$2

    [ -z "$nodes" -o -z "$pid_file" ] && return 0

    do_nodes $nodes "test -f $pid_file &&
        { kill -s TERM \\\$(cat $pid_file); rm -f $pid_file; }" || true
}

# Stop all client loads.
stop_client_loads() {
    local nodes=${1:-$CLIENTS}
    local pid_file=$2

    # stop the client loads
    stop_process $nodes $pid_file

    # clean up the processes that started them
    [ -n "$CLIENT_LOAD_PIDS" ] && kill -9 $CLIENT_LOAD_PIDS 2>/dev/null || true
}
# End recovery-scale functions

# verify that lustre actually cleaned up properly
cleanup_check() {
	VAR=$(lctl get_param -n catastrophe 2>&1)
	if [ $? = 0 ] ; then
		if [ $VAR != 0 ]; then
			error "LBUG/LASSERT detected"
		fi
	fi
	BUSY=$(dmesg | grep -i destruct || true)
	if [ -n "$BUSY" ]; then
		echo "$BUSY" 1>&2
		[ -e $TMP/debug ] && mv $TMP/debug $TMP/debug-busy.$(date +%s)
		exit 205
	fi

	check_mem_leak || exit 204

	[[ $($LCTL dl 2>/dev/null | wc -l) -gt 0 ]] && $LCTL dl &&
		echo "$TESTSUITE: lustre didn't clean up..." 1>&2 &&
		return 202 || true

	if module_loaded lnet || module_loaded libcfs; then
		echo "$TESTSUITE: modules still loaded..." 1>&2
		/sbin/lsmod 1>&2
		return 203
	fi
	return 0
}

wait_update () {
	local verbose=false
	if [[ "$1" == "--verbose" ]]; then
		shift
		verbose=true
	fi

	local node=$1
	local TEST=$2
	local FINAL=$3
	local MAX=${4:-90}
	local RESULT
	local PREV_RESULT
	local WAIT=0
	local sleep=1
	local print=10

	PREV_RESULT=$(do_node $node "$TEST")
	while [ true ]; do
		RESULT=$(do_node $node "$TEST")
		if [[ "$RESULT" == "$FINAL" ]]; then
			[[ -z "$RESULT" || $WAIT -le $sleep ]] ||
				echo "Updated after ${WAIT}s: wanted '$FINAL'"\
				     "got '$RESULT'"
			return 0
		fi
		if [[ $verbose && "$RESULT" != "$PREV_RESULT" ]]; then
			echo "Changed after ${WAIT}s: from '$PREV_RESULT'"\
			     "to '$RESULT'"
			PREV_RESULT=$RESULT
		fi
		[[ $WAIT -ge $MAX ]] && break
		[[ $((WAIT % print)) -eq 0 ]] &&
			echo "Waiting $((MAX - WAIT)) secs for update"
		WAIT=$((WAIT + sleep))
		sleep $sleep
	done
	echo "Update not seen after ${MAX}s: wanted '$FINAL' got '$RESULT'"
	return 3
}

wait_update_facet() {
	local verbose=
	[ "$1" = "--verbose" ] && verbose="$1" && shift

	local facet=$1
	shift
	wait_update $verbose $(facet_active_host $facet) "$@"
}

sync_all_data() {
	do_nodes $(comma_list $(mdts_nodes)) \
	    "lctl set_param -n osd*.*MDT*.force_sync=1"
	do_nodes $(comma_list $(osts_nodes)) \
	    "lctl set_param -n osd*.*OS*.force_sync=1" 2>&1 |
		grep -v 'Found no match'
}

wait_delete_completed_mds() {
	local MAX_WAIT=${1:-20}
	# for ZFS, waiting more time for DMUs to be committed
	local ZFS_WAIT=${2:-5}
	local mds2sync=""
	local stime=$(date +%s)
	local etime
	local node
	local changes

	# find MDS with pending deletions
	for node in $(mdts_nodes); do
		changes=$(do_node $node "$LCTL get_param -n osc.*MDT*.sync_*" \
			2>/dev/null | calc_sum)
		if [[ $changes -eq 0 ]]; then
			continue
		fi
		mds2sync="$mds2sync $node"
	done
	if [ -z "$mds2sync" ]; then
		return
	fi
	mds2sync=$(comma_list $mds2sync)

	# sync MDS transactions
	do_nodes $mds2sync "$LCTL set_param -n osd*.*MD*.force_sync 1"

	# wait till all changes are sent and commmitted by OSTs
	# for ldiskfs space is released upon execution, but DMU
	# do this upon commit

	local WAIT=0
	while [[ $WAIT -ne $MAX_WAIT ]]; do
		changes=$(do_nodes $mds2sync \
			"$LCTL get_param -n osc.*MDT*.sync_*" | calc_sum)
		#echo "$node: $changes changes on all"
		if [[ $changes -eq 0 ]]; then
			etime=$(date +%s)
			#echo "delete took $((etime - stime)) seconds"

			# the occupied disk space will be released
			# only after DMUs are committed
			if [[ $(facet_fstype $SINGLEMDS) == zfs ]]; then
				echo "sleep $ZFS_WAIT for ZFS OSD"
				sleep $ZFS_WAIT
			fi

			return
		fi
		sleep 1
		WAIT=$(( WAIT + 1))
	done

	etime=$(date +%s)
	echo "Delete is not completed in $((etime - stime)) seconds"
	do_nodes $mds2sync "$LCTL get_param osc.*MDT*.sync_*"
}

wait_for_host() {
    local hostlist=$1

    # we can use "for" here because we are waiting the slowest
    for host in ${hostlist//,/ }; do
        check_network "$host" 900
    done
    while ! do_nodes $hostlist hostname  > /dev/null; do sleep 5; done
}

wait_for_facet() {
    local facetlist=$1
    local hostlist

    for facet in ${facetlist//,/ }; do
        hostlist=$(expand_list $hostlist $(facet_active_host $facet))
    done
    wait_for_host $hostlist
}

_wait_recovery_complete () {
    local param=$1

    # Use default policy if $2 is not passed by caller.
    local MAX=${2:-$(max_recovery_time)}

    local WAIT=0
    local STATUS=

    while [ $WAIT -lt $MAX ]; do
        STATUS=$(lctl get_param -n $param | grep status)
        echo $param $STATUS
        [[ $STATUS = "status: COMPLETE" || $STATUS = "status: INACTIVE" ]] && return 0
        sleep 5
        WAIT=$((WAIT + 5))
        echo "Waiting $((MAX - WAIT)) secs for $param recovery done. $STATUS"
    done
    echo "$param recovery not done in $MAX sec. $STATUS"
    return 1
}

wait_recovery_complete () {
    local facet=$1

    # with an assumption that at_max is the same on all nodes
    local MAX=${2:-$(max_recovery_time)}

    local facets=$facet
    if [ "$FAILURE_MODE" = HARD ]; then
        facets=$(facets_on_host $(facet_active_host $facet))
    fi
    echo affected facets: $facets

	# we can use "for" here because we are waiting the slowest
	for facet in ${facets//,/ }; do
		local var_svc=${facet}_svc
		local param="*.${!var_svc}.recovery_status"

		local host=$(facet_active_host $facet)
		do_rpc_nodes "$host" _wait_recovery_complete $param $MAX
	done
}

wait_mds_ost_sync () {
	# just because recovery is done doesn't mean we've finished
	# orphan cleanup. Wait for llogs to get synchronized.
	echo "Waiting for orphan cleanup..."
	# MAX value includes time needed for MDS-OST reconnection
	local MAX=$(( TIMEOUT * 2 ))
	local WAIT_TIMEOUT=${1:-$MAX}
	local WAIT=0
	local new_wait=true
	local list=$(comma_list $(mdts_nodes))
	local cmd="$LCTL get_param -n osp.*osc*.old_sync_processed"
	if ! do_facet $SINGLEMDS \
		"$LCTL list_param osp.*osc*.old_sync_processed 2> /dev/null"
	then
		# old way, use mds_sync
		new_wait=false
		list=$(comma_list $(osts_nodes))
		cmd="$LCTL get_param -n obdfilter.*.mds_sync"
	fi

	echo "wait $WAIT_TIMEOUT secs maximumly for $list mds-ost sync done."
	while [ $WAIT -lt $WAIT_TIMEOUT ]; do
		local -a sync=($(do_nodes $list "$cmd"))
		local con=1
		local i
		for ((i=0; i<${#sync[@]}; i++)); do
			if $new_wait; then
				[ ${sync[$i]} -eq 1 ] && continue
			else
				[ ${sync[$i]} -eq 0 ] && continue
			fi
			# there is a not finished MDS-OST synchronization
			con=0
			break;
		done
		sleep 2 # increase waiting time and cover statfs cache
		[ ${con} -eq 1 ] && return 0
		echo "Waiting $WAIT secs for $list $i mds-ost sync done."
		WAIT=$((WAIT + 2))
	done

	# show which nodes are not finished.
	do_nodes $list "$cmd"
	echo "$facet recovery node $i not done in $WAIT_TIMEOUT sec. $STATUS"
	return 1
}

wait_destroy_complete () {
	echo "Waiting for local destroys to complete"
	# MAX value shouldn't be big as this mean server responsiveness
	# never increase this just to make test pass but investigate
	# why it takes so long time
	local MAX=5
	local WAIT=0
	while [ $WAIT -lt $MAX ]; do
		local -a RPCs=($($LCTL get_param -n osc.*.destroys_in_flight))
		local con=1
		local i

		for ((i=0; i<${#RPCs[@]}; i++)); do
			[ ${RPCs[$i]} -eq 0 ] && continue
			# there are still some destroy RPCs in flight
			con=0
			break;
		done
		sleep 1
		[ ${con} -eq 1 ] && return 0 # done waiting
		echo "Waiting ${WAIT}s for local destroys to complete"
		WAIT=$((WAIT + 1))
	done
	echo "Local destroys weren't done in $MAX sec."
	return 1
}

wait_delete_completed() {
	wait_delete_completed_mds $1 || return $?
	wait_destroy_complete
}

wait_exit_ST () {
    local facet=$1

    local WAIT=0
    local INTERVAL=1
    local running
    # conf-sanity 31 takes a long time cleanup
    while [ $WAIT -lt 300 ]; do
        running=$(do_facet ${facet} "lsmod | grep lnet > /dev/null && lctl dl | grep ' ST '") || true
        [ -z "${running}" ] && return 0
        echo "waited $WAIT for${running}"
        [ $INTERVAL -lt 64 ] && INTERVAL=$((INTERVAL + INTERVAL))
        sleep $INTERVAL
        WAIT=$((WAIT + INTERVAL))
    done
    echo "service didn't stop after $WAIT seconds.  Still running:"
    echo ${running}
    return 1
}

wait_remote_prog () {
   local prog=$1
   local WAIT=0
   local INTERVAL=5
   local rc=0

   [ "$PDSH" = "no_dsh" ] && return 0

   while [ $WAIT -lt $2 ]; do
        running=$(ps uax | grep "$PDSH.*$prog.*$MOUNT" | grep -v grep) || true
        [ -z "${running}" ] && return 0 || true
        echo "waited $WAIT for: "
        echo "$running"
        [ $INTERVAL -lt 60 ] && INTERVAL=$((INTERVAL + INTERVAL))
        sleep $INTERVAL
        WAIT=$((WAIT + INTERVAL))
    done
    local pids=$(ps  uax | grep "$PDSH.*$prog.*$MOUNT" | grep -v grep | awk '{print $2}')
    [ -z "$pids" ] && return 0
    echo "$PDSH processes still exists after $WAIT seconds.  Still running: $pids"
    # FIXME: not portable
    for pid in $pids; do
        cat /proc/${pid}/status || true
        cat /proc/${pid}/wchan || true
        echo "Killing $pid"
        kill -9 $pid || true
        sleep 1
        ps -P $pid && rc=1
    done

    return $rc
}

clients_up() {
    # not every config has many clients
    sleep 1
    if [ ! -z "$CLIENTS" ]; then
        $PDSH $CLIENTS "stat -f $MOUNT" > /dev/null
    else
        stat -f $MOUNT > /dev/null
    fi
}

client_up() {
    local client=$1
    # usually checked on particular client or locally
    sleep 1
    if [ ! -z "$client" ]; then
        $PDSH $client "stat -f $MOUNT" > /dev/null
    else
        stat -f $MOUNT > /dev/null
    fi
}

client_evicted() {
    ! client_up $1
}

client_reconnect_try() {
    uname -n >> $MOUNT/recon
    if [ -z "$CLIENTS" ]; then
        df $MOUNT; uname -n >> $MOUNT/recon
    else
        do_nodes $CLIENTS "df $MOUNT; uname -n >> $MOUNT/recon" > /dev/null
    fi
    echo Connected clients:
    cat $MOUNT/recon
    ls -l $MOUNT/recon > /dev/null
    rm $MOUNT/recon
}

client_reconnect() {
	# one client_reconnect_try call does not always do the job...
	while true ; do
		client_reconnect_try && break
		sleep 1
	done
}

affected_facets () {
    local facet=$1

    local host=$(facet_active_host $facet)
    local affected=$facet

    if [ "$FAILURE_MODE" = HARD ]; then
        affected=$(facets_up_on_host $host)
    fi
    echo $affected
}

facet_failover() {
	local E2FSCK_ON_MDT0=false
	if [ "$1" == "--fsck" ]; then
		shift
		[ $(facet_fstype $SINGLEMDS) == ldiskfs ] &&
			E2FSCK_ON_MDT0=true
	fi

	local facets=$1
	local sleep_time=$2
	local -a affecteds
	local facet
	local total=0
	local index=0
	local skip

	#Because it will only get up facets, we need get affected
	#facets before shutdown
	#For HARD Failure mode, it needs make sure facets on the same
	#HOST will only be shutdown and reboot once
	for facet in ${facets//,/ }; do
		local affected_facet
		skip=0
		#check whether facet has been included in other affected facets
		for ((index=0; index<$total; index++)); do
			[[ *,$facet,* == ,${affecteds[index]}, ]] && skip=1
		done

		if [ $skip -eq 0 ]; then
			affecteds[$total]=$(affected_facets $facet)
			total=$((total+1))
		fi
	done

	for ((index=0; index<$total; index++)); do
		facet=$(echo ${affecteds[index]} | tr -s " " | cut -d"," -f 1)
		local host=$(facet_active_host $facet)
		echo "Failing ${affecteds[index]} on $host"
		shutdown_facet $facet
	done

	$E2FSCK_ON_MDT0 && (run_e2fsck $(facet_active_host $SINGLEMDS) \
		$(mdsdevname 1) "-n" || error "Running e2fsck")

	for ((index=0; index<$total; index++)); do
		facet=$(echo ${affecteds[index]} | tr -s " " | cut -d"," -f 1)
		echo reboot facets: ${affecteds[index]}

		reboot_facet $facet

		change_active ${affecteds[index]}

		wait_for_facet ${affecteds[index]}
		# start mgs first if it is affected
		if ! combined_mgs_mds &&
			list_member ${affecteds[index]} mgs; then
			mount_facet mgs || error "Restart of mgs failed"
		fi
		# FIXME; has to be changed to mount all facets concurrently
		affected=$(exclude_items_from_list ${affecteds[index]} mgs)
		echo mount facets: ${affecteds[index]}
		mount_facets ${affecteds[index]}
	done
}

obd_name() {
    local facet=$1
}

replay_barrier() {
	local facet=$1
	do_facet $facet "sync; sync; sync"
	df $MOUNT

	# make sure there will be no seq change
	local clients=${CLIENTS:-$HOSTNAME}
	local f=fsa-\\\$\(hostname\)
	do_nodes $clients "mcreate $MOUNT/$f; rm $MOUNT/$f"
	do_nodes $clients "if [ -d $MOUNT2 ]; then mcreate $MOUNT2/$f; rm $MOUNT2/$f; fi"

	local svc=${facet}_svc
	do_facet $facet $LCTL --device ${!svc} notransno
	#
	# If a ZFS OSD is made read-only here, its pool is "freezed". This
	# in-memory state has to be cleared by either rebooting the host or
	# exporting and reimporting the pool.
	#
	# Although the uberblocks are not updated when a pool is freezed,
	# transactions are still written to the disks. Modified blocks may be
	# cached in memory when tests try reading them back. The
	# export-and-reimport process also evicts any cached pool data from
	# memory to provide the correct "data loss" semantics.
	#
	# In the test framework, the exporting and importing operations are
	# handled by stop() and mount_facet() separately, which are used
	# inside fail() and fail_abort().
	#
	do_facet $facet $LCTL --device ${!svc} readonly
	do_facet $facet $LCTL mark "$facet REPLAY BARRIER on ${!svc}"
	$LCTL mark "local REPLAY BARRIER on ${!svc}"
}

replay_barrier_nodf() {
	local facet=$1    echo running=${running}
	do_facet $facet "sync; sync; sync"
	local svc=${facet}_svc
	echo Replay barrier on ${!svc}
	do_facet $facet $LCTL --device ${!svc} notransno
	do_facet $facet $LCTL --device ${!svc} readonly
	do_facet $facet $LCTL mark "$facet REPLAY BARRIER on ${!svc}"
	$LCTL mark "local REPLAY BARRIER on ${!svc}"
}

replay_barrier_nosync() {
	local facet=$1    echo running=${running}
	local svc=${facet}_svc
	echo Replay barrier on ${!svc}
	do_facet $facet $LCTL --device ${!svc} notransno
	do_facet $facet $LCTL --device ${!svc} readonly
	do_facet $facet $LCTL mark "$facet REPLAY BARRIER on ${!svc}"
	$LCTL mark "local REPLAY BARRIER on ${!svc}"
}

#
# Get Lustre client uuid for a given Lustre mount point.
#
get_client_uuid() {
	local mntpnt=${1:-$MOUNT}

	local name=$($LFS getname $mntpnt | cut -d' ' -f1)
	local uuid=$($LCTL get_param -n llite.$name.uuid)

	echo -n $uuid
}

mds_evict_client() {
	local mntpnt=${1:-$MOUNT}
	local uuid=$(get_client_uuid $mntpnt)

	do_facet $SINGLEMDS \
		"$LCTL set_param -n mdt.${mds1_svc}.evict_client $uuid"
}

ost_evict_client() {
	local mntpnt=${1:-$MOUNT}
	local uuid=$(get_client_uuid $mntpnt)

	do_facet ost1 \
		"$LCTL set_param -n obdfilter.${ost1_svc}.evict_client $uuid"
}

fail() {
	local facets=$1
	local clients=${CLIENTS:-$HOSTNAME}

	facet_failover $* || error "failover: $?"
	wait_clients_import_state "$clients" "$facets" FULL
	clients_up || error "post-failover df: $?"
}

fail_nodf() {
        local facet=$1
        facet_failover $facet
}

fail_abort() {
	local facet=$1
	stop $facet
	change_active $facet
	wait_for_facet $facet
	mount_facet $facet -o abort_recovery
	clients_up || echo "first df failed: $?"
	clients_up || error "post-failover df: $?"
}

do_lmc() {
    echo There is no lmc.  This is mountconf, baby.
    exit 1
}

host_nids_address() {
    local nodes=$1
    local kind=$2

    if [ -n "$kind" ]; then
        nids=$(do_nodes $nodes "$LCTL list_nids | grep $kind | cut -f 1 -d '@'")
    else
        nids=$(do_nodes $nodes "$LCTL list_nids all | cut -f 1 -d '@'")
    fi
    echo $nids
}

h2name_or_ip() {
	if [ "$1" = "'*'" ]; then echo \'*\'; else
		echo $1"@$2"
	fi
}

h2ptl() {
	if [ "$1" = "'*'" ]; then echo \'*\'; else
		ID=`xtprocadmin -n $1 2>/dev/null | egrep -v 'NID' | \
							awk '{print $1}'`
		if [ -z "$ID" ]; then
			echo "Could not get a ptl id for $1..."
			exit 1
		fi
		echo $ID"@ptl"
	fi
}
declare -fx h2ptl

h2tcp() {
	h2name_or_ip "$1" "tcp"
}
declare -fx h2tcp

h2elan() {
	if [ "$1" = "'*'" ]; then echo \'*\'; else
		if type __h2elan >/dev/null 2>&1; then
			ID=$(__h2elan $1)
		else
			ID=`echo $1 | sed 's/[^0-9]*//g'`
		fi
		echo $ID"@elan"
	fi
}
declare -fx h2elan

h2o2ib() {
	h2name_or_ip "$1" "o2ib"
}
declare -fx h2o2ib

# This enables variables in cfg/"setup".sh files to support the pdsh HOSTLIST
# expressions format. As a bonus we can then just pass in those variables
# to pdsh. What this function does is take a HOSTLIST type string and
# expand it into a space deliminated list for us.
hostlist_expand() {
    local hostlist=$1
    local offset=$2
    local myList
    local item
    local list

    [ -z "$hostlist" ] && return

    # Translate the case of [..],..,[..] to [..] .. [..]
    list="${hostlist/],/] }"
    front=${list%%[*}
    [[ "$front" == *,* ]] && {
        new="${list%,*} "
        old="${list%,*},"
        list=${list/${old}/${new}}
    }

    for item in $list; do
        # Test if we have any []'s at all
        if [ "$item" != "${item/\[/}" ]; then {
            # Expand the [*] into list
            name=${item%%[*}
            back=${item#*]}

            if [ "$name" != "$item" ]; then
                group=${item#$name[*}
                group=${group%%]*}

                for range in ${group//,/ }; do
                    begin=${range%-*}
                    end=${range#*-}

                    # Number of leading zeros
                    padlen=${#begin}
                    padlen2=${#end}
                    end=$(echo $end | sed 's/0*//')
                    [[ -z "$end" ]] && end=0
                    [[ $padlen2 -gt $padlen ]] && {
                        [[ $padlen2 -eq ${#end} ]] && padlen2=0
                        padlen=$padlen2
                    }
                    begin=$(echo $begin | sed 's/0*//')
                    [ -z $begin ] && begin=0

                    for num in $(seq -f "%0${padlen}g" $begin $end); do
                        value="${name#*,}${num}${back}"
                        [ "$value" != "${value/\[/}" ] && {
                            value=$(hostlist_expand "$value")
                        }
                        myList="$myList $value"
                    done
                done
            fi
        } else {
            myList="$myList $item"
        } fi
    done
    myList=${myList//,/ }
    myList=${myList:1} # Remove first character which is a space

    # Filter any duplicates without sorting
    list="$myList "
    myList="${list%% *}"

    while [[ "$list" != ${myList##* } ]]; do
        list=${list//${list%% *} /}
        myList="$myList ${list%% *}"
    done
    myList="${myList%* }";

    # We can select an object at an offset in the list
    [ $# -eq 2 ] && {
        cnt=0
        for item in $myList; do
            let cnt=cnt+1
            [ $cnt -eq $offset ] && {
                myList=$item
            }
        done
        [ $(get_node_count $myList) -ne 1 ] && myList=""
    }
    echo $myList
}

facet_host() {
	local facet=$1
	local varname

	[ "$facet" == client ] && echo -n $HOSTNAME && return
	varname=${facet}_HOST
	if [ -z "${!varname}" ]; then
		if [ "${facet:0:3}" == "ost" ]; then
			local fh=${facet%failover}_HOST
			eval export ${facet}_HOST=${!fh}
			if [ -z "${!varname}" ]; then
				eval export ${facet}_HOST=${ost_HOST}
			fi
		elif [ "${facet:0:3}" == "mdt" -o \
			"${facet:0:3}" == "mds" -o \
			"${facet:0:3}" == "mgs" ]; then
			eval export ${facet}_HOST=${mds_HOST}
		fi
	fi
	echo -n ${!varname}
}

facet_failover_host() {
	local facet=$1
	local varname

	var=${facet}failover_HOST
	if [ -n "${!var}" ]; then
		echo ${!var}
		return
	fi

	if [ "${facet:0:3}" == "mdt" -o "${facet:0:3}" == "mds" -o \
	     "${facet:0:3}" == "mgs" ]; then

		eval export ${facet}failover_host=${mds_HOST}
		echo ${mds_HOST}
		return
	fi

	if [[ $facet == ost* ]]; then
		eval export ${facet}failover_host=${ost_HOST}
		echo ${ost_HOST}
		return
	fi
}

facet_active() {
    local facet=$1
    local activevar=${facet}active

    if [ -f $TMP/${facet}active ] ; then
        source $TMP/${facet}active
    fi

    active=${!activevar}
    if [ -z "$active" ] ; then
        echo -n ${facet}
    else
        echo -n ${active}
    fi
}

facet_active_host() {
	facet_host $(facet_active $1)
}

# Get the passive failover partner host of facet.
facet_passive_host() {
	local facet=$1
	[[ $facet = client ]] && return

	local host=${facet}_HOST
	local failover_host=${facet}failover_HOST
	local active_host=$(facet_active_host $facet)

	[[ -z ${!failover_host} || ${!failover_host} = ${!host} ]] && return

	if [[ $active_host = ${!host} ]]; then
		echo -n ${!failover_host}
	else
		echo -n ${!host}
	fi
}

change_active() {
    local facetlist=$1
    local facet

    facetlist=$(exclude_items_from_list $facetlist mgs)

    for facet in ${facetlist//,/ }; do
    local failover=${facet}failover
    local host=`facet_host $failover`
    [ -z "$host" ] && return

    local curactive=`facet_active $facet`
    if [ -z "${curactive}" -o "$curactive" == "$failover" ] ; then
        eval export ${facet}active=$facet
    else
        eval export ${facet}active=$failover
    fi
    # save the active host for this facet
    local activevar=${facet}active
    echo "$activevar=${!activevar}" > $TMP/$activevar
    [[ $facet = mds1 ]] && combined_mgs_mds && \
        echo "mgsactive=${!activevar}" > $TMP/mgsactive
    local TO=`facet_active_host $facet`
    echo "Failover $facet to $TO"
    done
}

do_node() {
    local verbose=false
    # do not stripe off hostname if verbose, bug 19215
    if [ x$1 = x--verbose ]; then
        shift
        verbose=true
    fi

    local HOST=$1
    shift
    local myPDSH=$PDSH
    if [ "$HOST" = "$HOSTNAME" ]; then
        myPDSH="no_dsh"
    elif [ -z "$myPDSH" -o "$myPDSH" = "no_dsh" ]; then
        echo "cannot run remote command on $HOST with $myPDSH"
        return 128
    fi
    if $VERBOSE; then
        echo "CMD: $HOST $@" >&2
        $myPDSH $HOST "$LCTL mark \"$@\"" > /dev/null 2>&1 || :
    fi

    if [ "$myPDSH" = "rsh" ]; then
# we need this because rsh does not return exit code of an executed command
        local command_status="$TMP/cs"
        rsh $HOST ":> $command_status"
        rsh $HOST "(PATH=\$PATH:$RLUSTRE/utils:$RLUSTRE/tests:/sbin:/usr/sbin;
                    cd $RPWD; LUSTRE=\"$RLUSTRE\" sh -c \"$@\") ||
                    echo command failed >$command_status"
        [ -n "$($myPDSH $HOST cat $command_status)" ] && return 1 || true
        return 0
    fi

    if $verbose ; then
        # print HOSTNAME for myPDSH="no_dsh"
        if [[ $myPDSH = no_dsh ]]; then
            $myPDSH $HOST "(PATH=\$PATH:$RLUSTRE/utils:$RLUSTRE/tests:/sbin:/usr/sbin; cd $RPWD; LUSTRE=\"$RLUSTRE\" sh -c \"$@\")" | sed -e "s/^/${HOSTNAME}: /"
        else
            $myPDSH $HOST "(PATH=\$PATH:$RLUSTRE/utils:$RLUSTRE/tests:/sbin:/usr/sbin; cd $RPWD; LUSTRE=\"$RLUSTRE\" sh -c \"$@\")"
        fi
    else
        $myPDSH $HOST "(PATH=\$PATH:$RLUSTRE/utils:$RLUSTRE/tests:/sbin:/usr/sbin; cd $RPWD; LUSTRE=\"$RLUSTRE\" sh -c \"$@\")" | sed "s/^${HOST}: //"
    fi
    return ${PIPESTATUS[0]}
}

do_nodev() {
    do_node --verbose "$@"
}

single_local_node () {
   [ "$1" = "$HOSTNAME" ]
}

# Outputs environment variable assignments that should be passed to remote nodes
get_env_vars() {
	local var
	local value
	local facets=$(get_facets)
	local facet

	for var in ${!MODOPTS_*}; do
		value=${!var//\"/\\\"}
		echo -n " ${var}=\"$value\""
	done

	for facet in ${facets//,/ }; do
		var=${facet}_FSTYPE
		if [ -n "${!var}" ]; then
			echo -n " $var=${!var}"
		fi
	done

	for var in MGSFSTYPE MDSFSTYPE OSTFSTYPE; do
		if [ -n "${!var}" ]; then
			echo -n " $var=${!var}"
		fi
	done

	for var in VERBOSE; do
		if [ -n "${!var}" ]; then
			echo -n " $var=${!var}"
		fi
	done

	if [ -n "$FSTYPE" ]; then
		echo -n " FSTYPE=$FSTYPE"
	fi

	for var in LNETLND NETTYPE; do
		if [ -n "${!var}" ]; then
			echo -n " $var=${!var}"
		fi
	done
}

do_nodes() {
    local verbose=false
    # do not stripe off hostname if verbose, bug 19215
    if [ x$1 = x--verbose ]; then
        shift
        verbose=true
    fi

    local rnodes=$1
    shift

    if single_local_node $rnodes; then
        if $verbose; then
           do_nodev $rnodes "$@"
        else
           do_node $rnodes "$@"
        fi
        return $?
    fi

    # This is part from do_node
    local myPDSH=$PDSH

    [ -z "$myPDSH" -o "$myPDSH" = "no_dsh" -o "$myPDSH" = "rsh" ] && \
        echo "cannot run remote command on $rnodes with $myPDSH" && return 128

    export FANOUT=$(get_node_count "${rnodes//,/ }")
    if $VERBOSE; then
        echo "CMD: $rnodes $@" >&2
        $myPDSH $rnodes "$LCTL mark \"$@\"" > /dev/null 2>&1 || :
    fi

    # do not replace anything from pdsh output if -N is used
    # -N     Disable hostname: prefix on lines of output.
    if $verbose || [[ $myPDSH = *-N* ]]; then
        $myPDSH $rnodes "(PATH=\$PATH:$RLUSTRE/utils:$RLUSTRE/tests:/sbin:/usr/sbin; cd $RPWD; LUSTRE=\"$RLUSTRE\" $(get_env_vars) sh -c \"$@\")"
    else
        $myPDSH $rnodes "(PATH=\$PATH:$RLUSTRE/utils:$RLUSTRE/tests:/sbin:/usr/sbin; cd $RPWD; LUSTRE=\"$RLUSTRE\" $(get_env_vars) sh -c \"$@\")" | sed -re "s/^[^:]*: //g"
    fi
    return ${PIPESTATUS[0]}
}

##
# Execute commands on a single service's host
#
# The \a facet (service) may be on a local or remote node, which is
# determined at the time the command is run.
#
# usage: do_facet $facet command [arg ...]
do_facet() {
	local facet=$1
	shift
	local HOST=$(facet_active_host $facet)
	[ -z $HOST ] && echo "No host defined for facet ${facet}" && exit 1
	do_node $HOST "$@"
}

# Function: do_facet_random_file $FACET $FILE $SIZE
# Creates FILE with random content on the given FACET of given SIZE

do_facet_random_file() {
	local facet="$1"
	local fpath="$2"
	local fsize="$3"
	local cmd="dd if=/dev/urandom of='$fpath' bs=$fsize count=1"
	do_facet $facet "$cmd 2>/dev/null"
}

do_facet_create_file() {
	local facet="$1"
	local fpath="$2"
	local fsize="$3"
	local cmd="dd if=/dev/zero of='$fpath' bs=$fsize count=1"
	do_facet $facet "$cmd 2>/dev/null"
}

do_nodesv() {
    do_nodes --verbose "$@"
}

add() {
	local facet=$1
	shift
	# make sure its not already running
	stop ${facet} -f
	rm -f $TMP/${facet}active
	[[ $facet = mds1 ]] && combined_mgs_mds && rm -f $TMP/mgsactive
	do_facet ${facet} $MKFS $* || return ${PIPESTATUS[0]}

	if [[ $(facet_fstype $facet) == zfs ]]; then
		#
		# After formatting a ZFS target, "cachefile=none" property will
		# be set on the ZFS storage pool so that the pool is not
		# automatically imported on system startup. And then the pool
		# will be exported so as to leave the importing and exporting
		# operations handled by mount_facet() and stop() separately.
		#
		refresh_partition_table $facet $(facet_vdevice $facet)
		disable_zpool_cache $facet
		export_zpool $facet
	fi
}

# Device formatted as ost
ostdevname() {
	local num=$1
	local DEVNAME=OSTDEV$num

	local fstype=$(facet_fstype ost$num)

	case $fstype in
		ldiskfs )
			#if $OSTDEVn isn't defined, default is $OSTDEVBASE + num
			eval DEVPTR=${!DEVNAME:=${OSTDEVBASE}${num}};;
		zfs )
			#try $OSTZFSDEVn - independent of vdev
			DEVNAME=OSTZFSDEV$num
			eval DEVPTR=${!DEVNAME:=${FSNAME}-ost${num}/ost${num}};;
		* )
			error "unknown fstype!";;
	esac

    echo -n $DEVPTR
}

# Physical device location of data
ostvdevname() {
	local num=$1
	local DEVNAME
	local VDEVPTR

	local fstype=$(facet_fstype ost$num)

	case $fstype in
		ldiskfs )
			# vdevs are not supported by ldiskfs
			eval VDEVPTR="";;
		zfs )
			#if $OSTDEVn isn't defined, default is $OSTDEVBASE{n}
			# Device formatted by zfs
			DEVNAME=OSTDEV$num
			eval VDEVPTR=${!DEVNAME:=${OSTDEVBASE}${num}};;
		* )
			error "unknown fstype!";;
	esac

	echo -n $VDEVPTR
}

# Logical device formatted for lustre
mdsdevname() {
	local num=$1
	local DEVNAME=MDSDEV$num

	local fstype=$(facet_fstype mds$num)

	case $fstype in
		ldiskfs )
			#if $MDSDEVn isn't defined, default is $MDSDEVBASE{n}
			eval DEVPTR=${!DEVNAME:=${MDSDEVBASE}${num}};;
		zfs )
			# try $MDSZFSDEVn - independent of vdev
			DEVNAME=MDSZFSDEV$num
			eval DEVPTR=${!DEVNAME:=${FSNAME}-mdt${num}/mdt${num}};;
		* )
			error "unknown fstype!";;
	esac

	echo -n $DEVPTR
}

# Physical location of data
mdsvdevname() {
	local VDEVPTR=""
	local num=$1
	local fstype=$(facet_fstype mds$num)

	case $fstype in
		ldiskfs )
			# vdevs are not supported by ldiskfs
			eval VDEVPTR="";;
		zfs )
			# if $MDSDEVn isn't defined, default is $MDSDEVBASE{n}
			# Device formatted by ZFS
			local DEVNAME=MDSDEV$num
			eval VDEVPTR=${!DEVNAME:=${MDSDEVBASE}${num}};;
		* )
			error "unknown fstype!";;
	esac

	echo -n $VDEVPTR
}

mgsdevname() {
	local DEVPTR
	local fstype=$(facet_fstype mgs)

	case $fstype in
	ldiskfs )
		if [ $(facet_host mgs) = $(facet_host mds1) ] &&
		   ( [ -z "$MGSDEV" ] || [ $MGSDEV = $(mdsdevname 1) ] ); then
			DEVPTR=$(mdsdevname 1)
		else
			DEVPTR=$MGSDEV
		fi;;
	zfs )
		if [ $(facet_host mgs) = $(facet_host mds1) ] &&
		    ( [ -z "$MGSZFSDEV" ] &&
			[ -z "$MGSDEV" -o "$MGSDEV" = $(mdsvdevname 1) ] ); then
			DEVPTR=$(mdsdevname 1)
		else
			DEVPTR=${MGSZFSDEV:-${FSNAME}-mgs/mgs}
		fi;;
	* )
		error "unknown fstype!";;
	esac

	echo -n $DEVPTR
}

mgsvdevname() {
	local VDEVPTR=""

	local fstype=$(facet_fstype mgs)

	case $fstype in
	ldiskfs )
		# vdevs are not supported by ldiskfs
		;;
	zfs )
		if [ $(facet_host mgs) = $(facet_host mds1) ] &&
		   ( [ -z "$MGSDEV" ] &&
		       [ -z "$MGSZFSDEV" -o "$MGSZFSDEV" = $(mdsdevname 1) ]); then
			VDEVPTR=$(mdsvdevname 1)
		elif [ -n "$MGSDEV" ]; then
			VDEVPTR=$MGSDEV
		fi;;
	* )
		error "unknown fstype!";;
	esac

	echo -n $VDEVPTR
}

facet_mntpt () {
    local facet=$1
    [[ $facet = mgs ]] && combined_mgs_mds && facet="mds1"

    local var=${facet}_MOUNT
    eval mntpt=${!var:-${MOUNT%/*}/$facet}

    echo -n $mntpt
}

mount_ldiskfs() {
	local facet=$1
	local dev=$(facet_device $facet)
	local mnt=$(facet_mntpt $facet)
	local opts

	if ! do_facet $facet test -b $dev; then
		opts="-o loop"
	fi
	do_facet $facet mount -t ldiskfs $opts $dev $mnt
}

unmount_ldiskfs() {
	local facet=$1
	local dev=$(facet_device $facet)
	local mnt=$(facet_mntpt $facet)

	do_facet $facet $UMOUNT $mnt
}

var_name() {
	echo -n "$1" | tr -c '[:alnum:]\n' '_'
}

mount_zfs() {
	local facet=$1
	local ds=$(facet_device $facet)
	local mnt=$(facet_mntpt $facet)
	local canmnt
	local mntpt

	import_zpool $facet
	canmnt=$(do_facet $facet $ZFS get -H -o value canmount $ds)
	mntpt=$(do_facet $facet $ZFS get -H -o value mountpoint $ds)
	do_facet $facet $ZFS set canmount=noauto $ds
	#
	# The "legacy" mount method is used here because "zfs unmount $mnt"
	# calls stat(2) on $mnt/../*, which may include $MOUNT.  If certain
	# targets are not available at the time, the stat(2) on $MOUNT will
	# hang.
	#
	do_facet $facet $ZFS set mountpoint=legacy $ds
	do_facet $facet mount -t zfs $ds $mnt
	eval export mz_$(var_name ${facet}_$ds)_canmount=$canmnt
	eval export mz_$(var_name ${facet}_$ds)_mountpoint=$mntpt
}

unmount_zfs() {
	local facet=$1
	local ds=$(facet_device $facet)
	local mnt=$(facet_mntpt $facet)
	local var_mntpt=mz_$(var_name ${facet}_$ds)_mountpoint
	local var_canmnt=mz_$(var_name ${facet}_$ds)_canmount
	local mntpt=${!var_mntpt}
	local canmnt=${!var_canmnt}

	unset $var_mntpt
	unset $var_canmnt
	do_facet $facet umount $mnt
	do_facet $facet $ZFS set mountpoint=$mntpt $ds
	do_facet $facet $ZFS set canmount=$canmnt $ds
	export_zpool $facet
}

mount_fstype() {
	local facet=$1
	local fstype=$(facet_fstype $facet)

	mount_$fstype $facet
}

unmount_fstype() {
	local facet=$1
	local fstype=$(facet_fstype $facet)

	unmount_$fstype $facet
}

########
## MountConf setup

stopall() {
    # make sure we are using the primary server, so test-framework will
    # be able to clean up properly.
    activemds=`facet_active mds1`
    if [ $activemds != "mds1" ]; then
        fail mds1
    fi

    local clients=$CLIENTS
    [ -z $clients ] && clients=$(hostname)

    zconf_umount_clients $clients $MOUNT "$*" || true
    [ -n "$MOUNT2" ] && zconf_umount_clients $clients $MOUNT2 "$*" || true

    [ "$CLIENTONLY" ] && return
    # The add fn does rm ${facet}active file, this would be enough
    # if we use do_facet <facet> only after the facet added, but
    # currently we use do_facet mds in local.sh
    for num in `seq $MDSCOUNT`; do
        stop mds$num -f
        rm -f ${TMP}/mds${num}active
    done
    combined_mgs_mds && rm -f $TMP/mgsactive

    for num in `seq $OSTCOUNT`; do
        stop ost$num -f
        rm -f $TMP/ost${num}active
    done

    if ! combined_mgs_mds ; then
        stop mgs
    fi

    return 0
}

cleanup_echo_devs () {
    local devs=$($LCTL dl | grep echo | awk '{print $4}')

    for dev in $devs; do
        $LCTL --device $dev cleanup
        $LCTL --device $dev detach
    done
}

cleanupall() {
    nfs_client_mode && return
	cifs_client_mode && return

    stopall $*
    cleanup_echo_devs

    unload_modules
    cleanup_gss
}

combined_mgs_mds () {
	[[ "$(mdsdevname 1)" = "$(mgsdevname)" ]] &&
		[[ "$(facet_host mds1)" = "$(facet_host mgs)" ]]
}

lower() {
	echo -n "$1" | tr '[:upper:]' '[:lower:]'
}

upper() {
	echo -n "$1" | tr '[:lower:]' '[:upper:]'
}

mkfs_opts() {
	local facet=$1
	local dev=$2
	local fsname=${3:-"$FSNAME"}
	local type=$(facet_type $facet)
	local index=$(facet_index $facet)
	local fstype=$(facet_fstype $facet)
	local host=$(facet_host $facet)
	local opts
	local fs_mkfs_opts
	local var

	if [ $type == MGS ] || ( [ $type == MDS ] &&
                                 [ "$dev" == $(mgsdevname) ] &&
				 [ "$host" == "$(facet_host mgs)" ] ); then
		opts="--mgs"
	else
		opts="--mgsnode=$MGSNID"
	fi

	if [ $type != MGS ]; then
		opts+=" --fsname=$fsname --$(lower ${type/MDS/MDT}) \
			--index=$index"
	fi

	var=${facet}failover_HOST
	if [ -n "${!var}" ] && [ ${!var} != $(facet_host $facet) ]; then
		opts+=" --failnode=$(h2$NETTYPE ${!var})"
	fi

	opts+=${TIMEOUT:+" --param=sys.timeout=$TIMEOUT"}
	opts+=${LDLM_TIMEOUT:+" --param=sys.ldlm_timeout=$LDLM_TIMEOUT"}

	if [ $type == MDS ]; then
		opts+=${SECLEVEL:+" --param=mdt.sec_level"}
		opts+=${MDSCAPA:+" --param-mdt.capa=$MDSCAPA"}
		opts+=${STRIPE_BYTES:+" --param=lov.stripesize=$STRIPE_BYTES"}
		opts+=${STRIPES_PER_OBJ:+" --param=lov.stripecount=$STRIPES_PER_OBJ"}
		opts+=${L_GETIDENTITY:+" --param=mdt.identity_upcall=$L_GETIDENTITY"}

		if [ $fstype == ldiskfs ]; then
			# Check for wide striping
			if [ $OSTCOUNT -gt 160 ]; then
				MDSJOURNALSIZE=${MDSJOURNALSIZE:-4096}
				fs_mkfs_opts+="-O large_xattr"
			fi

			var=${facet}_JRN
			if [ -n "${!var}" ]; then
				fs_mkfs_opts+=" -J device=${!var}"
			else
				fs_mkfs_opts+=${MDSJOURNALSIZE:+" -J size=$MDSJOURNALSIZE"}
			fi
			fs_mkfs_opts+=${MDSISIZE:+" -i $MDSISIZE"}
		fi
	fi

	if [ $type == OST ]; then
		opts+=${SECLEVEL:+" --param=ost.sec_level"}
		opts+=${OSSCAPA:+" --param=ost.capa=$OSSCAPA"}

		if [ $fstype == ldiskfs ]; then
			var=${facet}_JRN
			if [ -n "${!var}" ]; then
				fs_mkfs_opts+=" -J device=${!var}"
			else
				fs_mkfs_opts+=${OSTJOURNALSIZE:+" -J size=$OSTJOURNALSIZE"}
			fi
		fi
	fi

	opts+=" --backfstype=$fstype"

	var=${type}SIZE
	if [ -n "${!var}" ]; then
		opts+=" --device-size=${!var}"
	fi

	var=$(upper $fstype)_MKFS_OPTS
	fs_mkfs_opts+=${!var:+" ${!var}"}

	var=${type}_FS_MKFS_OPTS
	fs_mkfs_opts+=${!var:+" ${!var}"}

	if [ -n "${fs_mkfs_opts## }" ]; then
		opts+=" --mkfsoptions=\\\"${fs_mkfs_opts## }\\\""
	fi

	var=${type}OPT
	opts+=${!var:+" ${!var}"}

	echo -n "$opts"
}

check_ost_indices() {
	local index_count=${#OST_INDICES[@]}
	[[ $index_count -eq 0 || $OSTCOUNT -le $index_count ]] && return 0

	# OST count is greater than the index count in $OST_INDEX_LIST.
	# We need check whether there are duplicate indices.
	local i
	local j
	local index
	for i in $(seq $((index_count + 1)) $OSTCOUNT); do
		index=$(facet_index ost$i)
		for j in $(seq 0 $((index_count - 1))); do
			[[ $index -ne ${OST_INDICES[j]} ]] ||
			error "ost$i has the same index $index as ost$((j+1))"
		done
	done
}

format_mgs() {
	local quiet

	if ! $VERBOSE; then
		quiet=yes
	fi
	echo "Format mgs: $(mgsdevname)"
	reformat_external_journal mgs
	add mgs $(mkfs_opts mgs $(mgsdevname)) --reformat \
		$(mgsdevname) $(mgsvdevname) ${quiet:+>/dev/null} || exit 10
}

format_mdt() {
	local num=$1
	local quiet

	if ! $VERBOSE; then
		quiet=yes
	fi
	echo "Format mds$num: $(mdsdevname $num)"
	reformat_external_journal mds$num
	add mds$num $(mkfs_opts mds$num $(mdsdevname ${num})) \
		--reformat $(mdsdevname $num) $(mdsvdevname $num) \
		${quiet:+>/dev/null} || exit 10
}

format_ost() {
	local num=$1

	if ! $VERBOSE; then
		quiet=yes
	fi
	echo "Format ost$num: $(ostdevname $num)"
	reformat_external_journal ost$num
	add ost$num $(mkfs_opts ost$num $(ostdevname ${num})) \
		--reformat $(ostdevname $num) $(ostvdevname ${num}) \
		${quiet:+>/dev/null} || exit 10
}

formatall() {
	stopall
	# We need ldiskfs here, may as well load them all
	load_modules
	[ "$CLIENTONLY" ] && return
	echo Formatting mgs, mds, osts
	if ! combined_mgs_mds ; then
		format_mgs
	fi

	for num in $(seq $MDSCOUNT); do
		format_mdt $num
	done

	export OST_INDICES=($(hostlist_expand "$OST_INDEX_LIST"))
	check_ost_indices
	for num in $(seq $OSTCOUNT); do
		format_ost $num
	done
}

mount_client() {
    grep " $1 " /proc/mounts || zconf_mount $HOSTNAME $*
}

umount_client() {
    grep " $1 " /proc/mounts && zconf_umount `hostname` $*
}

# return value:
# 0: success, the old identity set already.
# 1: success, the old identity does not set.
# 2: fail.
switch_identity() {
    local num=$1
    local switch=$2
    local j=`expr $num - 1`
    local MDT="`(do_facet mds$num lctl get_param -N mdt.*MDT*$j 2>/dev/null | cut -d"." -f2 2>/dev/null) || true`"

    if [ -z "$MDT" ]; then
        return 2
    fi

    local old="`do_facet mds$num "lctl get_param -n mdt.$MDT.identity_upcall"`"

    if $switch; then
        do_facet mds$num "lctl set_param -n mdt.$MDT.identity_upcall \"$L_GETIDENTITY\""
    else
        do_facet mds$num "lctl set_param -n mdt.$MDT.identity_upcall \"NONE\""
    fi

    do_facet mds$num "lctl set_param -n mdt/$MDT/identity_flush=-1"

    if [ $old = "NONE" ]; then
        return 1
    else
        return 0
    fi
}

remount_client()
{
        zconf_umount `hostname` $1 || error "umount failed"
        zconf_mount `hostname` $1 || error "mount failed"
}

writeconf_facet() {
	local facet=$1
	local dev=$2

	stop ${facet} -f
	rm -f $TMP/${facet}active
	do_facet ${facet} "$TUNEFS --quiet --writeconf $dev" || return 1
	return 0
}

writeconf_all () {
	local mdt_count=${1:-$MDSCOUNT}
	local ost_count=${2:-$OSTCOUNT}
	local rc=0

	for num in $(seq $mdt_count); do
		DEVNAME=$(mdsdevname $num)
		writeconf_facet mds$num $DEVNAME || rc=$?
	done

	for num in $(seq $ost_count); do
		DEVNAME=$(ostdevname $num)
		writeconf_facet ost$num $DEVNAME || rc=$?
	done
	return $rc
}

setupall() {
    nfs_client_mode && return
	cifs_client_mode && return

    sanity_mount_check ||
        error "environments are insane!"

    load_modules

    if [ -z "$CLIENTONLY" ]; then
        echo Setup mgs, mdt, osts
        echo $WRITECONF | grep -q "writeconf" && \
            writeconf_all
        if ! combined_mgs_mds ; then
			start mgs $(mgsdevname) $MGS_MOUNT_OPTS
        fi

        for num in `seq $MDSCOUNT`; do
            DEVNAME=$(mdsdevname $num)
            start mds$num $DEVNAME $MDS_MOUNT_OPTS

            # We started mds, now we should set failover variables properly.
            # Set mds${num}failover_HOST if it is not set (the default failnode).
            local varname=mds${num}failover_HOST
            if [ -z "${!varname}" ]; then
                eval mds${num}failover_HOST=$(facet_host mds$num)
            fi

            if [ $IDENTITY_UPCALL != "default" ]; then
                switch_identity $num $IDENTITY_UPCALL
            fi
        done
        for num in `seq $OSTCOUNT`; do
            DEVNAME=$(ostdevname $num)
            start ost$num $DEVNAME $OST_MOUNT_OPTS

            # We started ost$num, now we should set ost${num}failover variable properly.
            # Set ost${num}failover_HOST if it is not set (the default failnode).
            varname=ost${num}failover_HOST
            if [ -z "${!varname}" ]; then
                eval ost${num}failover_HOST=$(facet_host ost${num})
            fi

        done
    fi

    init_gss

    # wait a while to allow sptlrpc configuration be propogated to targets,
    # only needed when mounting new target devices.
    if $GSS; then
        sleep 10
    fi

    [ "$DAEMONFILE" ] && $LCTL debug_daemon start $DAEMONFILE $DAEMONSIZE
    mount_client $MOUNT
    [ -n "$CLIENTS" ] && zconf_mount_clients $CLIENTS $MOUNT
    clients_up

    if [ "$MOUNT_2" ]; then
        mount_client $MOUNT2
        [ -n "$CLIENTS" ] && zconf_mount_clients $CLIENTS $MOUNT2
    fi

    init_param_vars

    # by remounting mdt before ost, initial connect from mdt to ost might
    # timeout because ost is not ready yet. wait some time to its fully
    # recovery. initial obd_connect timeout is 5s; in GSS case it's preceeded
    # by a context negotiation rpc with $TIMEOUT.
    # FIXME better by monitoring import status.
    if $GSS; then
        set_flavor_all $SEC
        sleep $((TIMEOUT + 5))
    else
        sleep 5
    fi
}

mounted_lustre_filesystems() {
        awk '($3 ~ "lustre" && $1 ~ ":") { print $2 }' /proc/mounts
}

init_facet_vars () {
	[ "$CLIENTONLY" ] && return 0
	local facet=$1
	shift
	local device=$1

	shift

	eval export ${facet}_dev=${device}
	eval export ${facet}_opt=\"$@\"

	local dev=${facet}_dev

	# We need to loop for the label
	# in case its not initialized yet.
	for wait_time in {0,1,3,5,10}; do

		if [ $wait_time -gt 0 ]; then
			echo "${!dev} not yet initialized,"\
				"waiting ${wait_time} seconds."
			sleep $wait_time
		fi

		local label=$(devicelabel ${facet} ${!dev})

		# Check to make sure the label does
		# not include ffff at the end of the label.
		# This indicates it has not been initialized yet.

		if [[ $label =~ [f|F]{4}$ ]]; then
			# label is not initialized, unset the result
			# and either try again or fail
			unset label
		else
			break
		fi
	done

	[ -z "$label" ] && echo no label for ${!dev} && exit 1

	eval export ${facet}_svc=${label}

	local varname=${facet}failover_HOST
	if [ -z "${!varname}" ]; then
		eval export $varname=$(facet_host $facet)
	fi

	varname=${facet}_HOST
	if [ -z "${!varname}" ]; then
		eval export $varname=$(facet_host $facet)
 	fi

	# ${facet}failover_dev is set in cfg file
	varname=${facet}failover_dev
	if [ -n "${!varname}" ] ; then
		eval export ${facet}failover_dev=${!varname}
	else
		eval export ${facet}failover_dev=$device
	fi

	# get mount point of already mounted device
	# is facet_dev is already mounted then use the real
	#  mount point of this facet; otherwise use $(facet_mntpt $facet)
	# i.e. ${facet}_MOUNT if specified by user or default
	local mntpt=$(do_facet ${facet} cat /proc/mounts | \
			awk '"'${!dev}'" == $1 && $3 == "lustre" { print $2 }')
	if [ -z $mntpt ]; then
		mntpt=$(facet_mntpt $facet)
	fi
	eval export ${facet}_MOUNT=$mntpt
}

init_facets_vars () {
	local DEVNAME

	if ! remote_mds_nodsh; then
		for num in $(seq $MDSCOUNT); do
			DEVNAME=`mdsdevname $num`
			init_facet_vars mds$num $DEVNAME $MDS_MOUNT_OPTS
		done
	fi

	combined_mgs_mds || init_facet_vars mgs $(mgsdevname) $MGS_MOUNT_OPTS

	if ! remote_ost_nodsh; then
		for num in $(seq $OSTCOUNT); do
			DEVNAME=$(ostdevname $num)
			init_facet_vars ost$num $DEVNAME $OST_MOUNT_OPTS
		done
	fi
}

osc_ensure_active () {
    local facet=$1
    local timeout=$2
    local period=0

    while [ $period -lt $timeout ]; do
        count=$(do_facet $facet "lctl dl | grep ' IN osc ' 2>/dev/null | wc -l")
        if [ $count -eq 0 ]; then
            break
        fi

        echo "There are $count OST are inactive, wait $period seconds, and try again"
        sleep 3
        period=$((period+3))
    done

    [ $period -lt $timeout ] || log "$count OST are inactive after $timeout seconds, give up"
}

set_conf_param_and_check() {
	local myfacet=$1
	local TEST=$2
	local PARAM=$3
	local ORIG=$(do_facet $myfacet "$TEST")
	if [ $# -gt 3 ]; then
		local FINAL=$4
	else
		local -i FINAL
		FINAL=$((ORIG + 5))
	fi
	echo "Setting $PARAM from $ORIG to $FINAL"
	do_facet mgs "$LCTL conf_param $PARAM='$FINAL'" ||
		error "conf_param $PARAM failed"

	wait_update $(facet_host $myfacet) "$TEST" "$FINAL" ||
		error "check $PARAM failed!"
}

init_param_vars () {
	remote_mds_nodsh ||
		TIMEOUT=$(do_facet $SINGLEMDS "lctl get_param -n timeout")

	log "Using TIMEOUT=$TIMEOUT"

	osc_ensure_active $SINGLEMDS $TIMEOUT
	osc_ensure_active client $TIMEOUT

	if [ -n "$(lctl get_param -n mdc.*.connect_flags|grep jobstats)" ]; then
		local current_jobid_var=$($LCTL get_param -n jobid_var)

		if [ $JOBID_VAR = "existing" ]; then
			echo "keeping jobstats as $current_jobid_var"
		elif [ $current_jobid_var != $JOBID_VAR ]; then
			echo "seting jobstats to $JOBID_VAR"

			set_conf_param_and_check client			\
				"$LCTL get_param -n jobid_var"		\
				"$FSNAME.sys.jobid_var" $JOBID_VAR
		fi
	else
		echo "jobstats not supported by server"
	fi

	if [ $QUOTA_AUTO -ne 0 ]; then
		if [ "$ENABLE_QUOTA" ]; then
			echo "enable quota as required"
			setup_quota $MOUNT || return 2
		else
			echo "disable quota as required"
			# $LFS quotaoff -ug $MOUNT > /dev/null 2>&1
		fi
	fi
	return 0
}

nfs_client_mode () {
    if [ "$NFSCLIENT" ]; then
        echo "NFSCLIENT mode: setup, cleanup, check config skipped"
        local clients=$CLIENTS
        [ -z $clients ] && clients=$(hostname)

        # FIXME: remove hostname when 19215 fixed
        do_nodes $clients "echo \\\$(hostname); grep ' '$MOUNT' ' /proc/mounts"
        declare -a nfsexport=(`grep ' '$MOUNT' ' /proc/mounts | awk '{print $1}' | awk -F: '{print $1 " "  $2}'`)
        if [[ ${#nfsexport[@]} -eq 0 ]]; then
                error_exit NFSCLIENT=$NFSCLIENT mode, but no NFS export found!
        fi
        do_nodes ${nfsexport[0]} "echo \\\$(hostname); df -T  ${nfsexport[1]}"
        return
    fi
    return 1
}

cifs_client_mode () {
	[ x$CIFSCLIENT = xyes ] &&
		echo "CIFSCLIENT=$CIFSCLIENT mode: setup, cleanup, check config skipped"
}

check_config_client () {
    local mntpt=$1

    local mounted=$(mount | grep " $mntpt ")
    if [ "$CLIENTONLY" ]; then
        # bug 18021
        # CLIENTONLY should not depend on *_HOST settings
        local mgc=$($LCTL device_list | awk '/MGC/ {print $4}')
        # in theory someone could create a new,
        # client-only config file that assumed lustre was already
        # configured and didn't set the MGSNID. If MGSNID is not set,
        # then we should use the mgs nid currently being used 
        # as the default value. bug 18021
        [[ x$MGSNID = x ]] &&
            MGSNID=${mgc//MGC/}

        if [[ x$mgc != xMGC$MGSNID ]]; then
            if [ "$mgs_HOST" ]; then
                local mgc_ip=$(ping -q -c1 -w1 $mgs_HOST | grep PING | awk '{print $3}' | sed -e "s/(//g" -e "s/)//g")
#                [[ x$mgc = xMGC$mgc_ip@$NETTYPE ]] ||
#                    error_exit "MGSNID=$MGSNID, mounted: $mounted, MGC : $mgc"
            fi
        fi
        return 0
    fi

    local myMGS_host=$mgs_HOST   
    if [ "$NETTYPE" = "ptl" ]; then
        myMGS_host=$(h2ptl $mgs_HOST | sed -e s/@ptl//) 
    fi

    echo Checking config lustre mounted on $mntpt
    local mgshost=$(mount | grep " $mntpt " | awk -F@ '{print $1}')
    mgshost=$(echo $mgshost | awk -F: '{print $1}')

#    if [ "$mgshost" != "$myMGS_host" ]; then
#            log "Bad config file: lustre is mounted with mgs $mgshost, but mgs_HOST=$mgs_HOST, NETTYPE=$NETTYPE
#                   Please use correct config or set mds_HOST correctly!"
#    fi

}

check_config_clients () {
	local clients=${CLIENTS:-$HOSTNAME}
	local mntpt=$1

	nfs_client_mode && return
	cifs_client_mode && return

	do_rpc_nodes "$clients" check_config_client $mntpt

	sanity_mount_check || error "environments are insane!"
}

check_timeout () {
    local mdstimeout=$(do_facet $SINGLEMDS "lctl get_param -n timeout")
    local cltimeout=$(lctl get_param -n timeout)
    if [ $mdstimeout -ne $TIMEOUT ] || [ $mdstimeout -ne $cltimeout ]; then
        error "timeouts are wrong! mds: $mdstimeout, client: $cltimeout, TIMEOUT=$TIMEOUT"
        return 1
    fi
}

is_mounted () {
    local mntpt=$1
    [ -z $mntpt ] && return 1
    local mounted=$(mounted_lustre_filesystems)

    echo $mounted' ' | grep -w -q $mntpt' '
}

is_empty_dir() {
	[ $(find $1 -maxdepth 1 -print | wc -l) = 1 ] && return 0
	return 1
}

# empty lustre filesystem may have empty directories lost+found and .lustre
is_empty_fs() {
	# exclude .lustre & lost+found
	[ $(find $1 -maxdepth 1 -name lost+found -o -name .lustre -prune -o \
		-print | wc -l) = 1 ] || return 1
	[ ! -d $1/lost+found ] || is_empty_dir $1/lost+found || return 1
	if [ $(lustre_version_code $SINGLEMDS) -gt $(version_code 2.4.0) ]; then
		# exclude .lustre/fid (LU-2780)
		[ $(find $1/.lustre -maxdepth 1 -name fid -prune -o \
			-print | wc -l) = 1 ] || return 1
	else
		[ ! -d $1/.lustre ] || is_empty_dir $1/.lustre || return 1
	fi
	return 0
}

check_and_setup_lustre() {
    nfs_client_mode && return
	cifs_client_mode && return

    local MOUNTED=$(mounted_lustre_filesystems)

    local do_check=true
    # 1.
    # both MOUNT and MOUNT2 are not mounted
    if ! is_mounted $MOUNT && ! is_mounted $MOUNT2; then
        [ "$REFORMAT" ] && formatall
        # setupall mounts both MOUNT and MOUNT2 (if MOUNT_2 is set)
        setupall
        is_mounted $MOUNT || error "NAME=$NAME not mounted"
        export I_MOUNTED=yes
        do_check=false
    # 2.
    # MOUNT2 is mounted
    elif is_mounted $MOUNT2; then
            # 3.
            # MOUNT2 is mounted, while MOUNT_2 is not set
            if ! [ "$MOUNT_2" ]; then
                cleanup_mount $MOUNT2
                export I_UMOUNTED2=yes

            # 4.
            # MOUNT2 is mounted, MOUNT_2 is set
            else
                # FIXME: what to do if check_config failed?
                # i.e. if:
                # 1) remote client has mounted other Lustre fs ?
                # 2) it has insane env ?
                # let's try umount MOUNT2 on all clients and mount it again:
                if ! check_config_clients $MOUNT2; then
                    cleanup_mount $MOUNT2
                    restore_mount $MOUNT2
                    export I_MOUNTED2=yes
                fi
            fi 

    # 5.
    # MOUNT is mounted MOUNT2 is not mounted
    elif [ "$MOUNT_2" ]; then
        restore_mount $MOUNT2
        export I_MOUNTED2=yes
    fi

    if $do_check; then
        # FIXME: what to do if check_config failed?
        # i.e. if:
        # 1) remote client has mounted other Lustre fs?
        # 2) lustre is mounted on remote_clients atall ?
        check_config_clients $MOUNT
        init_facets_vars
        init_param_vars

        set_default_debug_nodes $(comma_list $(nodes_list))
    fi

	if [ $(lower $OSD_TRACK_DECLARES_LBUG) == 'yes' ] ; then
		local facets=""
		[ "$(facet_fstype ost1)" = "ldiskfs" ] &&
			facets="$(get_facets OST)"
		[ "$(facet_fstype mds1)" = "ldiskfs" ] &&
			facets="$facets,$(get_facets MDS)"
		[ "$(facet_fstype mgs)" = "ldiskfs" ] &&
			facets="$facets,mgs"
		local nodes="$(facets_hosts ${facets})"
		if [ -n "$nodes" ] ; then
			do_nodes $nodes "$LCTL set_param \
				 osd-ldiskfs.track_declares_assert=1 || true"
		fi
	fi

	init_gss
	if $GSS; then
		set_flavor_all $SEC
	fi

	#Enable remote MDT create for testing
	for num in $(seq $MDSCOUNT); do
		do_facet mds$num \
			lctl set_param -n mdt.${FSNAME}*.enable_remote_dir=1 \
				2>/dev/null
	done

	if [ "$ONLY" == "setup" ]; then
		exit 0
	fi
}

restore_mount () {
   local clients=${CLIENTS:-$HOSTNAME}
   local mntpt=$1

   zconf_mount_clients $clients $mntpt
}

cleanup_mount () {
	local clients=${CLIENTS:-$HOSTNAME}
	local mntpt=$1

	zconf_umount_clients $clients $mntpt
}

cleanup_and_setup_lustre() {
    if [ "$ONLY" == "cleanup" -o "`mount | grep $MOUNT`" ]; then
        lctl set_param debug=0 || true
        cleanupall
        if [ "$ONLY" == "cleanup" ]; then
            exit 0
        fi
    fi
    check_and_setup_lustre
}

# Get all of the server target devices from a given server node and type.
get_mnt_devs() {
	local node=$1
	local type=$2
	local devs
	local dev

	if [ "$type" == ost ]; then
		devs=$(get_osd_param $node "" mntdev)
	else
		devs=$(do_node $node $LCTL get_param -n osd-*.$FSNAME-M*.mntdev)
	fi
	for dev in $devs; do
		case $dev in
		*loop*) do_node $node "losetup $dev" | \
				sed -e "s/.*(//" -e "s/).*//" ;;
		*) echo $dev ;;
		esac
	done
}

# Get all of the server target devices.
get_svr_devs() {
	local node
	local i

	# Master MDS parameters used by lfsck
	MDTNODE=$(facet_active_host $SINGLEMDS)
	MDTDEV=$(echo $(get_mnt_devs $MDTNODE mdt) | awk '{print $1}')

	# MDT devices
	i=0
	for node in $(mdts_nodes); do
		MDTDEVS[i]=$(get_mnt_devs $node mdt)
		i=$((i + 1))
	done

	# OST devices
	i=0
	for node in $(osts_nodes); do
		OSTDEVS[i]=$(get_mnt_devs $node ost)
		i=$((i + 1))
	done
}

# Run e2fsck on MDT or OST device.
run_e2fsck() {
	local node=$1
	local target_dev=$2
	local extra_opts=$3
	local cmd="$E2FSCK -d -v -t -t -f $extra_opts $target_dev"
	local log=$TMP/e2fsck.log
	local rc=0

	echo $cmd
	do_node $node $cmd 2>&1 | tee $log
	rc=${PIPESTATUS[0]}
	if [ -n "$(grep "DNE mode isn't supported" $log)" ]; then
		rm -f $log
		if [ $MDSCOUNT -gt 1 ]; then
			skip "DNE mode isn't supported!"
			cleanupall
			exit_status
		else
			error "It's not DNE mode."
		fi
	fi
	rm -f $log

	[ $rc -le $FSCK_MAX_ERR ] ||
		error "$cmd returned $rc, should be <= $FSCK_MAX_ERR"

	return 0
}

#
# Run resize2fs on MDT or OST device.
#
run_resize2fs() {
	local facet=$1
	local device=$2
	local size=$3
	shift 3
	local opts="$@"

	do_facet $facet "$RESIZE2FS $opts $device $size"
}

# verify a directory is shared among nodes.
check_shared_dir() {
	local dir=$1
	local list=${2:-$(comma_list $(nodes_list))}

	[ -z "$dir" ] && return 1
	do_rpc_nodes "$list" check_logdir $dir
	check_write_access $dir "$list" || return 1
	return 0
}

run_lfsck() {
	do_nodes $(comma_list $(mdts_nodes) $(osts_nodes)) \
		$LCTL set_param printk=+lfsck
	do_facet $SINGLEMDS "$LCTL lfsck_start -M $FSNAME-MDT0000 -r -A -t all"

	for k in $(seq $MDSCOUNT); do
		# wait up to 10+1 minutes for LFSCK to complete
		wait_update_facet --verbose mds${k} "$LCTL get_param -n \
			mdd.$(facet_svc mds${k}).lfsck_layout |
			awk '/^status/ { print \\\$2 }'" "completed" 600 ||
			error "MDS${k} layout isn't the expected 'completed'"
		wait_update_facet --verbose mds${k} "$LCTL get_param -n \
			mdd.$(facet_svc mds${k}).lfsck_namespace |
			awk '/^status/ { print \\\$2 }'" "completed" 60 ||
			error "MDS${k} namespace isn't the expected 'completed'"
	done
	local rep_mdt=$(do_nodes $(comma_list $(mdts_nodes)) \
			$LCTL get_param -n mdd.$FSNAME-*.lfsck_* |
			awk '/repaired/ { print $2 }' | calc_sum)
	local rep_ost=$(do_nodes $(comma_list $(osts_nodes)) \
			$LCTL get_param -n obdfilter.$FSNAME-*.lfsck_* |
			awk '/repaired/ { print $2 }' | calc_sum)
	local repaired=$((rep_mdt + rep_ost))
	[ $repaired -eq 0 ] ||
		error "lfsck repaired $rep_mdt MDT and $rep_ost OST errors"
}

dump_file_contents() {
	local nodes=$1
	local dir=$2
	local logname=$3
	local node

	if [ -z "$nodes" -o -z "$dir" -o -z "$logname" ]; then
		error_noexit false \
			"Invalid parameters for dump_file_contents()"
		return 1
	fi
	for node in ${nodes}; do
		do_node $node "for i in \\\$(find $dir -type f); do
				echo ====\\\${i}=======================;
				cat \\\${i};
				done" >> ${logname}.${node}.log
	done
}

dump_command_output() {
	local nodes=$1
	local cmd=$2
	local logname=$3
	local node

	if [ -z "$nodes" -o -z "$cmd" -o -z "$logname" ]; then
		error_noexit false \
			"Invalid parameters for dump_command_output()"
		return 1
	fi

	for node in ${nodes}; do
		do_node $node "echo ====${cmd}=======================;
				$cmd" >> ${logname}.${node}.log
	done
}

log_zfs_info() {
	local logname=$1

	# dump file contents from /proc/spl in case of zfs test
	if [ "$(facet_fstype ost1)" = "zfs" ]; then
		dump_file_contents "$(osts_nodes)" "/proc/spl" "${logname}"
		dump_command_output \
			"$(osts_nodes)" "zpool events -v" "${logname}"
	fi

	if [ "$(facet_fstype $SINGLEMDS)" = "zfs" ]; then
		dump_file_contents "$(mdts_nodes)" "/proc/spl" "${logname}"
		dump_command_output \
			"$(mdts_nodes)" "zpool events -v" "${logname}"
	fi
}

check_and_cleanup_lustre() {
	if [ "$LFSCK_ALWAYS" = "yes" -a "$TESTSUITE" != "sanity-lfsck" -a \
	     "$TESTSUITE" != "sanity-scrub" ]; then
		run_lfsck
	fi

	if is_mounted $MOUNT; then
		[ -n "$DIR" ] && rm -rf $DIR/[Rdfs][0-9]* ||
			error "remove sub-test dirs failed"
		[ "$ENABLE_QUOTA" ] && restore_quota || true
	fi

	if [ "$I_UMOUNTED2" = "yes" ]; then
		restore_mount $MOUNT2 || error "restore $MOUNT2 failed"
	fi

	if [ "$I_MOUNTED2" = "yes" ]; then
		cleanup_mount $MOUNT2
	fi

	if [ "$I_MOUNTED" = "yes" ]; then
		cleanupall -f || error "cleanup failed"
		unset I_MOUNTED
	fi
}

#######
# General functions

wait_for_function () {
    local quiet=""

    # suppress fn both stderr and stdout
    if [ "$1" = "--quiet" ]; then
        shift
        quiet=" > /dev/null 2>&1"

    fi

    local fn=$1
    local max=${2:-900}
    local sleep=${3:-5}

    local wait=0

    while true; do

        eval $fn $quiet && return 0

        wait=$((wait + sleep))
        [ $wait -lt $max ] || return 1
        echo waiting $fn, $((max - wait)) secs left ...
        sleep $sleep
    done
}

check_network() {
    local host=$1
    local max=$2
    local sleep=${3:-5}

    echo `date +"%H:%M:%S (%s)"` waiting for $host network $max secs ...
    if ! wait_for_function --quiet "ping -c 1 -w 3 $host" $max $sleep ; then
        echo "Network not available!"
        exit 1
    fi

    echo `date +"%H:%M:%S (%s)"` network interface is UP
}

no_dsh() {
    shift
    eval $@
}

# Convert a space-delimited list to a comma-delimited list.  If the input is
# only whitespace, ensure the output is empty (i.e. "") so [ -n $list ] works
comma_list() {
	# echo is used to convert newlines to spaces, since it doesn't
	# introduce a trailing space as using "tr '\n' ' '" does
	echo $(tr -s " " "\n" <<< $* | sort -b -u) | tr ' ' ','
}

list_member () {
    local list=$1
    local item=$2
    echo $list | grep -qw $item
}

# list, excluded are the comma separated lists
exclude_items_from_list () {
    local list=$1
    local excluded=$2
    local item

    list=${list//,/ }
    for item in ${excluded//,/ }; do
        list=$(echo " $list " | sed -re "s/\s+$item\s+/ /g")
    done
    echo $(comma_list $list)
}

# list, expand  are the comma separated lists
expand_list () {
    local list=${1//,/ }
    local expand=${2//,/ }
    local expanded=

    expanded=$(for i in $list $expand; do echo $i; done | sort -u)
    echo $(comma_list $expanded)
}

testslist_filter () {
    local script=$LUSTRE/tests/${TESTSUITE}.sh

    [ -f $script ] || return 0

    local start_at=$START_AT
    local stop_at=$STOP_AT

    local var=${TESTSUITE//-/_}_START_AT
    [ x"${!var}" != x ] && start_at=${!var}
    var=${TESTSUITE//-/_}_STOP_AT
    [ x"${!var}" != x ] && stop_at=${!var}

    sed -n 's/^test_\([^ (]*\).*/\1/p' $script | \
        awk ' BEGIN { if ("'${start_at:-0}'" != 0) flag = 1 }
            /^'${start_at}'$/ {flag = 0}
            {if (flag == 1) print $0}
            /^'${stop_at}'$/ { flag = 1 }'
}

absolute_path() {
    (cd `dirname $1`; echo $PWD/`basename $1`)
}

get_facets () {
    local types=${1:-"OST MDS MGS"}

    local list=""

    for entry in $types; do
        local name=$(echo $entry | tr "[:upper:]" "[:lower:]")
        local type=$(echo $entry | tr "[:lower:]" "[:upper:]")

        case $type in
                MGS ) list="$list $name";;
            MDS|OST|AGT ) local count=${type}COUNT
                       for ((i=1; i<=${!count}; i++)) do
                          list="$list ${name}$i"
                      done;;
                  * ) error "Invalid facet type"
                 exit 1;;
        esac
    done
    echo $(comma_list $list)
}

##################################
# Adaptive Timeouts funcs

at_is_enabled() {
    # only check mds, we assume at_max is the same on all nodes
    local at_max=$(do_facet $SINGLEMDS "lctl get_param -n at_max")
    if [ $at_max -eq 0 ]; then
        return 1
    else
        return 0
    fi
}

at_get() {
    local facet=$1
    local at=$2

    # suppose that all ost-s have the same $at value set
    [ $facet != "ost" ] || facet=ost1

    do_facet $facet "lctl get_param -n $at"
}

at_max_get() {
    at_get $1 at_max
}

at_min_get() {
	at_get $1 at_min
}

at_max_set() {
    local at_max=$1
    shift

    local facet
    local hosts
    for facet in $@; do
        if [ $facet == "ost" ]; then
            facet=$(get_facets OST)
        elif [ $facet == "mds" ]; then
            facet=$(get_facets MDS)
        fi
        hosts=$(expand_list $hosts $(facets_hosts $facet))
    done

    do_nodes $hosts lctl set_param at_max=$at_max
}

##################################
# OBD_FAIL funcs

drop_request() {
# OBD_FAIL_MDS_ALL_REQUEST_NET
    RC=0
    do_facet $SINGLEMDS lctl set_param fail_loc=0x123
    do_facet client "$1" || RC=$?
    do_facet $SINGLEMDS lctl set_param fail_loc=0
    return $RC
}

drop_reply() {
# OBD_FAIL_MDS_ALL_REPLY_NET
	RC=0
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0x122
	eval "$@" || RC=$?
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0
	return $RC
}

drop_reint_reply() {
# OBD_FAIL_MDS_REINT_NET_REP
	RC=0
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0x119
	eval "$@" || RC=$?
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0
	return $RC
}

drop_update_reply() {
# OBD_FAIL_OUT_UPDATE_NET_REP
	local index=$1
	shift 1
	RC=0
	do_facet mds${index} lctl set_param fail_loc=0x1701
	do_facet client "$@" || RC=$?
	do_facet mds${index} lctl set_param fail_loc=0
	return $RC
}

pause_bulk() {
#define OBD_FAIL_OST_BRW_PAUSE_BULK      0x214
	RC=0

	local timeout=${2:-0}
	# default is (obd_timeout / 4) if unspecified
	echo "timeout is $timeout/$2"
	do_facet ost1 lctl set_param fail_val=$timeout fail_loc=0x80000214
	do_facet client "$1" || RC=$?
	do_facet client "sync"
	do_facet ost1 lctl set_param fail_loc=0
	return $RC
}

drop_ldlm_cancel() {
#define OBD_FAIL_LDLM_CANCEL_NET			0x304
	local RC=0
	local list=$(comma_list $(mdts_nodes) $(osts_nodes))
	do_nodes $list lctl set_param fail_loc=0x304

	do_facet client "$@" || RC=$?

	do_nodes $list lctl set_param fail_loc=0
	return $RC
}

drop_bl_callback_once() {
	local rc=0
	do_facet client lctl set_param ldlm.namespaces.*.early_lock_cancel=0
#define OBD_FAIL_LDLM_BL_CALLBACK_NET			0x305
	do_facet client lctl set_param fail_loc=0x80000305
	do_facet client "$@" || rc=$?
	do_facet client lctl set_param fail_loc=0
	do_facet client lctl set_param fail_val=0
	do_facet client lctl set_param ldlm.namespaces.*.early_lock_cancel=1
	return $rc
}

drop_bl_callback() {
	rc=0
	do_facet client lctl set_param ldlm.namespaces.*.early_lock_cancel=0
#define OBD_FAIL_LDLM_BL_CALLBACK_NET			0x305
	do_facet client lctl set_param fail_loc=0x305
	do_facet client "$@" || rc=$?
	do_facet client lctl set_param fail_loc=0
	do_facet client lctl set_param fail_val=0
	do_facet client lctl set_param ldlm.namespaces.*.early_lock_cancel=1
	return $rc
}

drop_ldlm_reply() {
#define OBD_FAIL_LDLM_REPLY              0x30c
    RC=0
    local list=$(comma_list $(mdts_nodes) $(osts_nodes))
    do_nodes $list lctl set_param fail_loc=0x30c

    do_facet client "$@" || RC=$?

    do_nodes $list lctl set_param fail_loc=0
    return $RC
}

drop_ldlm_reply_once() {
#define OBD_FAIL_LDLM_REPLY              0x30c
    RC=0
    local list=$(comma_list $(mdts_nodes) $(osts_nodes))
    do_nodes $list lctl set_param fail_loc=0x8000030c

    do_facet client "$@" || RC=$?

    do_nodes $list lctl set_param fail_loc=0
    return $RC
}

clear_failloc() {
    facet=$1
    pause=$2
    sleep $pause
    echo "clearing fail_loc on $facet"
    do_facet $facet "lctl set_param fail_loc=0 2>/dev/null || true"
}

set_nodes_failloc () {
	do_nodes $(comma_list $1)  lctl set_param fail_val=0 fail_loc=$2
}

cancel_lru_locks() {
	#$LCTL mark "cancel_lru_locks $1 start"
	$LCTL set_param -n ldlm.namespaces.*$1*.lru_size=clear
	$LCTL get_param ldlm.namespaces.*$1*.lock_unused_count | grep -v '=0'
	#$LCTL mark "cancel_lru_locks $1 stop"
}

default_lru_size()
{
        NR_CPU=$(grep -c "processor" /proc/cpuinfo)
        DEFAULT_LRU_SIZE=$((100 * NR_CPU))
        echo "$DEFAULT_LRU_SIZE"
}

lru_resize_enable()
{
    lctl set_param ldlm.namespaces.*$1*.lru_size=0
}

lru_resize_disable()
{
    lctl set_param ldlm.namespaces.*$1*.lru_size $(default_lru_size)
}

flock_is_enabled()
{
	local RC=0
	[ -z "$(mount | grep "$MOUNT.*flock" | grep -v noflock)" ] && RC=1
	return $RC
}

pgcache_empty() {
    local FILE
    for FILE in `lctl get_param -N "llite.*.dump_page_cache"`; do
        if [ `lctl get_param -n $FILE | wc -l` -gt 1 ]; then
            echo there is still data in page cache $FILE ?
            lctl get_param -n $FILE
            return 1
        fi
    done
    return 0
}

debugsave() {
    DEBUGSAVE="$(lctl get_param -n debug)"
}

debugrestore() {
    [ -n "$DEBUGSAVE" ] && \
        do_nodes $(comma_list $(nodes_list)) "$LCTL set_param debug=\\\"${DEBUGSAVE}\\\";"
    DEBUGSAVE=""
}

debug_size_save() {
    DEBUG_SIZE_SAVED="$(lctl get_param -n debug_mb)"
}

debug_size_restore() {
    [ -n "$DEBUG_SIZE_SAVED" ] && \
        do_nodes $(comma_list $(nodes_list)) "$LCTL set_param debug_mb=$DEBUG_SIZE_SAVED"
    DEBUG_SIZE_SAVED=""
}

start_full_debug_logging() {
    debugsave
    debug_size_save

    local FULLDEBUG=-1
    local DEBUG_SIZE=150

    do_nodes $(comma_list $(nodes_list)) "$LCTL set_param debug_mb=$DEBUG_SIZE"
    do_nodes $(comma_list $(nodes_list)) "$LCTL set_param debug=$FULLDEBUG;"
}

stop_full_debug_logging() {
    debug_size_restore
    debugrestore
}

# prints bash call stack
print_stack_trace() {
	echo "  Trace dump:"
	for (( i=1; i < ${#BASH_LINENO[*]} ; i++ )) ; do
		local s=${BASH_SOURCE[$i]}
		local l=${BASH_LINENO[$i-1]}
		local f=${FUNCNAME[$i]}
		echo "  = $s:$l:$f()"
	done
}

##################################
# Test interface
##################################

error_noexit() {
	local TYPE=${TYPE:-"FAIL"}

	local dump=true
	# do not dump logs if $1=false
	if [ "x$1" = "xfalse" ]; then
		shift
		dump=false
	fi


	log " ${TESTSUITE} ${TESTNAME}: @@@@@@ ${TYPE}: $@ "
	print_stack_trace >&2

	mkdir -p $LOGDIR
	# We need to dump the logs on all nodes
	if $dump; then
		gather_logs $(comma_list $(nodes_list))
	fi

	debugrestore
	[ "$TESTSUITELOG" ] &&
		echo "$TESTSUITE: $TYPE: $TESTNAME $@" >> $TESTSUITELOG
	if [ -z "$*" ]; then
		echo "error() without useful message, please fix" > $LOGDIR/err
	else
		if [[ `echo $TYPE | grep ^IGNORE` ]]; then
			echo "$@" > $LOGDIR/ignore
		else
			echo "$@" > $LOGDIR/err
		fi
	fi

	# cleanup the env for failed tests
	reset_fail_loc
}

exit_status () {
	local status=0
	local log=$TESTSUITELOG

	[ -f "$log" ] && grep -q FAIL $log && status=1
	exit $status
}

error() {
	error_noexit "$@"
	exit 1
}

error_exit() {
	error "$@"
}

# use only if we are ignoring failures for this test, bugno required.
# (like ALWAYS_EXCEPT, but run the test and ignore the results.)
# e.g. error_ignore bz5494 "your message" or
# error_ignore LU-5494 "your message"
error_ignore() {
	local TYPE="IGNORE ($1)"
	shift
	error_noexit "$@"
}

error_and_remount() {
	error_noexit "$@"
	remount_client $MOUNT
	exit 1
}

skip_env () {
	$FAIL_ON_SKIP_ENV && error false $@ || skip $@
}

skip() {
	echo
	log " SKIP: $TESTSUITE $TESTNAME $@"

	if [[ -n "$ALWAYS_SKIPPED" ]]; then
		skip_logged $TESTNAME "$@"
	else
		mkdir -p $LOGDIR
		echo "$@" > $LOGDIR/skip
	fi

	[[ -n "$TESTSUITELOG" ]] &&
		echo "$TESTSUITE: SKIP: $TESTNAME $@" >> $TESTSUITELOG || true
}

build_test_filter() {
    EXCEPT="$EXCEPT $(testslist_filter)"

	for O in $ONLY; do
		if [[ $O = [0-9]*-[0-9]* ]]; then
			for num in $(seq $(echo $O | tr '-' ' ')); do
				eval ONLY_$num=true
			done
		else
			eval ONLY_${O}=true
		fi
	done

    [ "$EXCEPT$ALWAYS_EXCEPT" ] && \
        log "excepting tests: `echo $EXCEPT $ALWAYS_EXCEPT`"
    [ "$EXCEPT_SLOW" ] && \
        log "skipping tests SLOW=no: `echo $EXCEPT_SLOW`"
    for E in $EXCEPT; do
        eval EXCEPT_${E}=true
    done
    for E in $ALWAYS_EXCEPT; do
        eval EXCEPT_ALWAYS_${E}=true
    done
    for E in $EXCEPT_SLOW; do
        eval EXCEPT_SLOW_${E}=true
    done
    for G in $GRANT_CHECK_LIST; do
        eval GCHECK_ONLY_${G}=true
        done
}

basetest() {
    if [[ $1 = [a-z]* ]]; then
        echo $1
    else
        echo ${1%%[a-z]*}
    fi
}

# print a newline if the last test was skipped
export LAST_SKIPPED=
export ALWAYS_SKIPPED=
#
# Main entry into test-framework. This is called with the name and
# description of a test. The name is used to find the function to run
# the test using "test_$name".
#
# This supports a variety of methods of specifying specific test to
# run or not run.  These need to be documented...
#
run_test() {
	assert_DIR

	export base=$(basetest $1)
	if [ -n "$ONLY" ]; then
		testname=ONLY_$1
		if [ ${!testname}x != x ]; then
			[ -n "$LAST_SKIPPED" ] && echo "" && LAST_SKIPPED=
			run_one_logged $1 "$2"
			return $?
		fi
		testname=ONLY_$base
		if [ ${!testname}x != x ]; then
			[ -n "$LAST_SKIPPED" ] && echo "" && LAST_SKIPPED=
			run_one_logged $1 "$2"
			return $?
		fi
		LAST_SKIPPED="y"
		return 0
	fi

	LAST_SKIPPED="y"
	ALWAYS_SKIPPED="y"
	testname=EXCEPT_$1
	if [ ${!testname}x != x ]; then
		TESTNAME=test_$1 skip "skipping excluded test $1"
		return 0
	fi
	testname=EXCEPT_$base
	if [ ${!testname}x != x ]; then
		TESTNAME=test_$1 skip "skipping excluded test $1 (base $base)"
		return 0
	fi
	testname=EXCEPT_ALWAYS_$1
	if [ ${!testname}x != x ]; then
		TESTNAME=test_$1 skip "skipping ALWAYS excluded test $1"
		return 0
	fi
	testname=EXCEPT_ALWAYS_$base
	if [ ${!testname}x != x ]; then
		TESTNAME=test_$1 skip "skipping ALWAYS excluded test $1 (base $base)"
		return 0
	fi
	testname=EXCEPT_SLOW_$1
	if [ ${!testname}x != x ]; then
		TESTNAME=test_$1 skip "skipping SLOW test $1"
		return 0
	fi
	testname=EXCEPT_SLOW_$base
	if [ ${!testname}x != x ]; then
		TESTNAME=test_$1 skip "skipping SLOW test $1 (base $base)"
		return 0
	fi

	LAST_SKIPPED=
	ALWAYS_SKIPPED=
	run_one_logged $1 "$2"

	return $?
}

log() {
	echo "$*" >&2
	load_module ../libcfs/libcfs/libcfs

    local MSG="$*"
    # Get rid of '
    MSG=${MSG//\'/\\\'}
    MSG=${MSG//\(/\\\(}
    MSG=${MSG//\)/\\\)}
    MSG=${MSG//\;/\\\;}
    MSG=${MSG//\|/\\\|}
    MSG=${MSG//\>/\\\>}
    MSG=${MSG//\</\\\<}
    MSG=${MSG//\//\\\/}
    do_nodes $(comma_list $(nodes_list)) $LCTL mark "$MSG" 2> /dev/null || true
}

trace() {
        log "STARTING: $*"
        strace -o $TMP/$1.strace -ttt $*
        RC=$?
        log "FINISHED: $*: rc $RC"
        return 1
}

complete () {
    local duration=$1

    banner test complete, duration $duration sec
    [ -f "$TESTSUITELOG" ] && egrep .FAIL $TESTSUITELOG || true
    echo duration $duration >>$TESTSUITELOG
}

pass() {
	# Set TEST_STATUS here. It will be used for logging the result.
	TEST_STATUS="PASS"

	if [[ -f $LOGDIR/err ]]; then
		TEST_STATUS="FAIL"
	elif [[ -f $LOGDIR/skip ]]; then
		TEST_STATUS="SKIP"
	fi
	echo "$TEST_STATUS $@" 2>&1 | tee -a $TESTSUITELOG
}

check_mds() {
    local FFREE=$(do_node $SINGLEMDS \
        lctl get_param -n osd*.*MDT*.filesfree | calc_sum)
    local FTOTAL=$(do_node $SINGLEMDS \
        lctl get_param -n osd*.*MDT*.filestotal | calc_sum)

    [ $FFREE -ge $FTOTAL ] && error "files free $FFREE > total $FTOTAL" || true
}

reset_fail_loc () {
    echo -n "Resetting fail_loc on all nodes..."
    do_nodes $(comma_list $(nodes_list)) "lctl set_param -n fail_loc=0 \
	    fail_val=0 2>/dev/null || true"
    echo done.
}


#
# Log a message (on all nodes) padded with "=" before and after. 
# Also appends a timestamp and prepends the testsuite name.
# 

EQUALS="===================================================================================================="
banner() {
    msg="== ${TESTSUITE} $*"
    last=${msg: -1:1}
    [[ $last != "=" && $last != " " ]] && msg="$msg "
    msg=$(printf '%s%.*s'  "$msg"  $((${#EQUALS} - ${#msg})) $EQUALS )
    # always include at least == after the message
    log "$msg== $(date +"%H:%M:%S (%s)")"
}

#
# Run a single test function and cleanup after it.
#
# This function should be run in a subshell so the test func can
# exit() without stopping the whole script.
#
run_one() {
	local testnum=$1
	local message=$2
	export tfile=f${testnum}.${TESTSUITE}
	export tdir=d${testnum}.${TESTSUITE}
	export TESTNAME=test_$testnum
	local SAVE_UMASK=`umask`
	umask 0022

	banner "test $testnum: $message"
	test_${testnum} || error "test_$testnum failed with $?"
	cd $SAVE_PWD
	reset_fail_loc
	check_grant ${testnum} || error "check_grant $testnum failed with $?"
	check_catastrophe || error "LBUG/LASSERT detected"
	if [ "$PARALLEL" != "yes" ]; then
		ps auxww | grep -v grep | grep -q multiop &&
					error "multiop still running"
	fi
	unset TESTNAME
	unset tdir
	unset tfile
	umask $SAVE_UMASK
	return 0
}

#
# Wrapper around run_one to ensure:
#  - test runs in subshell
#  - output of test is saved to separate log file for error reporting
#  - test result is saved to data file
#
run_one_logged() {
	local BEFORE=$(date +%s)
	local TEST_ERROR
	local name=${TESTSUITE}.test_${1}.test_log.$(hostname -s).log
	local test_log=$LOGDIR/$name
	local zfs_log_name=${TESTSUITE}.test_${1}.zfs_log
	local zfs_debug_log=$LOGDIR/$zfs_log_name
	rm -rf $LOGDIR/err
	rm -rf $LOGDIR/ignore
	rm -rf $LOGDIR/skip
	local SAVE_UMASK=$(umask)
	umask 0022

	echo
	log_sub_test_begin test_${1}
	(run_one $1 "$2") 2>&1 | tee -i $test_log
	local RC=${PIPESTATUS[0]}

	[ $RC -ne 0 ] && [ ! -f $LOGDIR/err ] &&
		echo "test_$1 returned $RC" | tee $LOGDIR/err

	duration=$(($(date +%s) - $BEFORE))
	pass "$1" "(${duration}s)"

	if [[ -f $LOGDIR/err ]]; then
		TEST_ERROR=$(cat $LOGDIR/err)
	elif [[ -f $LOGDIR/ignore ]]; then
		TEST_ERROR=$(cat $LOGDIR/ignore)
	elif [[ -f $LOGDIR/skip ]]; then
		TEST_ERROR=$(cat $LOGDIR/skip)
	fi
	log_sub_test_end $TEST_STATUS $duration "$RC" "$TEST_ERROR"

	if [[ "$TEST_STATUS" != "SKIP" ]] && [[ -f $TF_SKIP ]]; then
		rm -f $TF_SKIP
	fi

	if [ -f $LOGDIR/err ]; then
		log_zfs_info "$zfs_debug_log"
		$FAIL_ON_ERROR && exit $RC
	fi

	umask $SAVE_UMASK

	return 0
}

#
# Print information of skipped tests to result.yml
#
skip_logged(){
	log_sub_test_begin $1
	shift
	log_sub_test_end "SKIP" "0" "0" "$@"
}

canonical_path() {
	(cd $(dirname $1); echo $PWD/$(basename $1))
}


check_grant() {
	export base=$(basetest $1)
	[ "$CHECK_GRANT" == "no" ] && return 0

	testname=GCHECK_ONLY_${base}
	[ ${!testname}x == x ] && return 0

	echo -n "checking grant......"

	local clients=$CLIENTS
	[ -z "$clients" ] && clients=$(hostname)

	# sync all the data and make sure no pending data on server
	do_nodes $clients sync

	# get client grant
	client_grant=$(do_nodes $clients \
		"$LCTL get_param -n osc.${FSNAME}-*.cur_*grant_bytes" |
		awk '{ total += $1 } END { printf("%0.0f", total) }')

	# get server grant
	server_grant=$(do_nodes $(comma_list $(osts_nodes)) \
		"$LCTL get_param -n obdfilter.${FSNAME}-OST*.tot_granted" |
		awk '{ total += $1 } END { printf("%0.0f", total) }')

	# check whether client grant == server grant
	if [[ $client_grant -ne $server_grant ]]; then
		echo "failed: client:${client_grant} server: ${server_grant}."
		do_nodes $(comma_list $(osts_nodes)) \
			"$LCTL get_param obdfilter.${FSNAME}-OST*.tot*"
		do_nodes $clients "$LCTL get_param osc.${FSNAME}-*.cur_*_bytes"
		return 1
	else
		echo "pass: client:${client_grant} server: ${server_grant}"
	fi
}

########################
# helper functions

osc_to_ost()
{
    osc=$1
    ost=`echo $1 | awk -F_ '{print $3}'`
    if [ -z $ost ]; then
        ost=`echo $1 | sed 's/-osc.*//'`
    fi
    echo $ost
}

ostuuid_from_index()
{
    $LFS osts $2 | sed -ne "/^$1: /s/.* \(.*\) .*$/\1/p"
}

ostname_from_index() {
    local uuid=$(ostuuid_from_index $1)
    echo ${uuid/_UUID/}
}

index_from_ostuuid()
{
    $LFS osts $2 | sed -ne "/${1}/s/\(.*\): .* .*$/\1/p"
}

mdtuuid_from_index()
{
    $LFS mdts $2 | sed -ne "/^$1: /s/.* \(.*\) .*$/\1/p"
}

# Description:
#   Return unique identifier for given hostname
host_id() {
	local host_name=$1
	echo $host_name | md5sum | cut -d' ' -f1
}

# Description:
#   Returns list of ip addresses for each interface
local_addr_list() {
	ip addr | awk '/inet\ / {print $2}' | awk -F\/ '{print $1}'
}

is_local_addr() {
	local addr=$1
	# Cache address list to avoid mutiple execution of local_addr_list
	LOCAL_ADDR_LIST=${LOCAL_ADDR_LIST:-$(local_addr_list)}
	local i
	for i in $LOCAL_ADDR_LIST ; do
		[[ "$i" == "$addr" ]] && return 0
	done
	return 1
}

local_node() {
	local host_name=$1
	local is_local="IS_LOCAL_$(host_id $host_name)"
	if [ -z "${!is_local-}" ] ; then
		eval $is_local=0
		local host_ip=$($LUSTRE/tests/resolveip $host_name)
		is_local_addr "$host_ip" && eval $is_local=1
	fi
	[[ "${!is_local}" == "1" ]]
}

remote_node () {
	local node=$1
	local_node $node && return 1
	return 0
}

remote_mds ()
{
    local node
    for node in $(mdts_nodes); do
        remote_node $node && return 0
    done
    return 1
}

remote_mds_nodsh()
{
    [ "$CLIENTONLY" ] && return 0 || true
    remote_mds && [ "$PDSH" = "no_dsh" -o -z "$PDSH" -o -z "$mds_HOST" ]
}

require_dsh_mds()
{
        remote_mds_nodsh && echo "SKIP: $TESTSUITE: remote MDS with nodsh" && \
            MSKIPPED=1 && return 1
        return 0
}

remote_ost ()
{
    local node
    for node in $(osts_nodes) ; do
        remote_node $node && return 0
    done
    return 1
}

remote_ost_nodsh()
{
    [ "$CLIENTONLY" ] && return 0 || true 
    remote_ost && [ "$PDSH" = "no_dsh" -o -z "$PDSH" -o -z "$ost_HOST" ]
}

require_dsh_ost()
{
        remote_ost_nodsh && echo "SKIP: $TESTSUITE: remote OST with nodsh" && \
            OSKIPPED=1 && return 1
        return 0
}

remote_mgs_nodsh()
{
	[ "$CLIENTONLY" ] && return 0 || true
    local MGS 
    MGS=$(facet_host mgs)
    remote_node $MGS && [ "$PDSH" = "no_dsh" -o -z "$PDSH" -o -z "$ost_HOST" ]
}

local_mode ()
{
    remote_mds_nodsh || remote_ost_nodsh || \
        $(single_local_node $(comma_list $(nodes_list)))
}

remote_servers () {
    remote_ost && remote_mds
}

# Get the active nodes for facets.
facets_nodes () {
	local facets=$1
	local facet
	local nodes
	local nodes_sort
	local i

	for facet in ${facets//,/ }; do
		nodes="$nodes $(facet_active_host $facet)"
	done

	nodes_sort=$(for i in $nodes; do echo $i; done | sort -u)
	echo -n $nodes_sort
}

# Get all of the active MDS nodes.
mdts_nodes () {
	echo -n $(facets_nodes $(get_facets MDS))
}

# Get all of the active OSS nodes.
osts_nodes () {
	echo -n $(facets_nodes $(get_facets OST))
}

# Get all of the active AGT (HSM agent) nodes.
agts_nodes () {
	echo -n $(facets_nodes $(get_facets AGT))
}

# Get all of the client nodes and active server nodes.
nodes_list () {
	local nodes=$HOSTNAME
	local nodes_sort
	local i

	# CLIENTS (if specified) contains the local client
	[ -n "$CLIENTS" ] && nodes=${CLIENTS//,/ }

	if [ "$PDSH" -a "$PDSH" != "no_dsh" ]; then
		nodes="$nodes $(facets_nodes $(get_facets))"
	fi

	nodes_sort=$(for i in $nodes; do echo $i; done | sort -u)
	echo -n $nodes_sort
}

# Get all of the remote client nodes and remote active server nodes.
remote_nodes_list () {
	echo -n $(nodes_list) | sed -re "s/\<$HOSTNAME\>//g"
}

# Get all of the MDS nodes, including active and passive nodes.
all_mdts_nodes () {
	local host
	local failover_host
	local nodes
	local nodes_sort
	local i

	for i in $(seq $MDSCOUNT); do
		host=mds${i}_HOST
		failover_host=mds${i}failover_HOST
		nodes="$nodes ${!host} ${!failover_host}"
	done

	nodes_sort=$(for i in $nodes; do echo $i; done | sort -u)
	echo -n $nodes_sort
}

# Get all of the OSS nodes, including active and passive nodes.
all_osts_nodes () {
	local host
	local failover_host
	local nodes
	local nodes_sort
	local i

	for i in $(seq $OSTCOUNT); do
		host=ost${i}_HOST
		failover_host=ost${i}failover_HOST
		nodes="$nodes ${!host} ${!failover_host}"
	done

	nodes_sort=$(for i in $nodes; do echo $i; done | sort -u)
	echo -n $nodes_sort
}

# Get all of the server nodes, including active and passive nodes.
all_server_nodes () {
	local nodes
	local nodes_sort
	local i

	nodes="$mgs_HOST $mgsfailover_HOST $(all_mdts_nodes) $(all_osts_nodes)"

	nodes_sort=$(for i in $nodes; do echo $i; done | sort -u)
	echo -n $nodes_sort
}

# Get all of the client and server nodes, including active and passive nodes.
all_nodes () {
	local nodes=$HOSTNAME
	local nodes_sort
	local i

	# CLIENTS (if specified) contains the local client
	[ -n "$CLIENTS" ] && nodes=${CLIENTS//,/ }

	if [ "$PDSH" -a "$PDSH" != "no_dsh" ]; then
		nodes="$nodes $(all_server_nodes)"
	fi

	nodes_sort=$(for i in $nodes; do echo $i; done | sort -u)
	echo -n $nodes_sort
}

init_clients_lists () {
    # Sanity check: exclude the local client from RCLIENTS
    local clients=$(hostlist_expand "$RCLIENTS")
    local rclients=$(exclude_items_from_list "$clients" $HOSTNAME)

    # Sanity check: exclude the dup entries
    RCLIENTS=$(for i in ${rclients//,/ }; do echo $i; done | sort -u)

    clients="$SINGLECLIENT $HOSTNAME $RCLIENTS"

    # Sanity check: exclude the dup entries from CLIENTS
    # for those configs which has SINGLCLIENT set to local client
    clients=$(for i in $clients; do echo $i; done | sort -u)

    CLIENTS=$(comma_list $clients)
    local -a remoteclients=($RCLIENTS)
    for ((i=0; $i<${#remoteclients[@]}; i++)); do
            varname=CLIENT$((i + 2))
            eval $varname=${remoteclients[i]}
    done

    CLIENTCOUNT=$((${#remoteclients[@]} + 1))
}

get_random_entry () {
    local rnodes=$1

    rnodes=${rnodes//,/ }

    local -a nodes=($rnodes)
    local num=${#nodes[@]} 
    local i=$((RANDOM * num * 2 / 65536))

    echo ${nodes[i]}
}

client_only () {
    [ "$CLIENTONLY" ] || [ "$CLIENTMODSONLY" = yes ]
}

is_patchless ()
{
    lctl get_param version | grep -q patchless
}

check_versions () {
    [ "$(lustre_version_code client)" = "$(lustre_version_code $SINGLEMDS)" -a \
      "$(lustre_version_code client)" = "$(lustre_version_code ost1)" ]
}

get_node_count() {
    local nodes="$@"
    echo $nodes | wc -w || true
}

mixed_ost_devs () {
    local nodes=$(osts_nodes)
    local osscount=$(get_node_count "$nodes")
    [ ! "$OSTCOUNT" = "$osscount" ]
}

mixed_mdt_devs () {
    local nodes=$(mdts_nodes)
    local mdtcount=$(get_node_count "$nodes")
    [ ! "$MDSCOUNT" = "$mdtcount" ]
}

generate_machine_file() {
    local nodes=${1//,/ }
    local machinefile=$2
    rm -f $machinefile
    for node in $nodes; do
        echo $node >>$machinefile || \
            { echo "can not generate machinefile $machinefile" && return 1; }
    done
}

get_stripe () {
	local file=$1/stripe

	touch $file
	$LFS getstripe -v $file || error "getstripe $file failed"
	rm -f $file
}

setstripe_nfsserver () {
	local dir=$1

	local nfsserver=$(awk '"'$dir'" ~ $2 && $3 ~ "nfs" && $2 != "/" \
		{ print $1 }' /proc/mounts | cut -f 1 -d : | head -n1)

	[ -z $nfsserver ] && echo "$dir is not nfs mounted" && return 1

	do_nodev $nfsserver lfs setstripe "$@"
}

# Check and add a test group.
add_group() {
	local group_id=$1
	local group_name=$2
	local rc=0

	local gid=$(getent group $group_name | cut -d: -f3)
	if [[ -n "$gid" ]]; then
		[[ "$gid" -eq "$group_id" ]] || {
			error_noexit "inconsistent group ID:" \
				     "new: $group_id, old: $gid"
			rc=1
		}
	else
		groupadd -g $group_id $group_name
		rc=${PIPESTATUS[0]}
	fi

	return $rc
}

# Check and add a test user.
add_user() {
	local user_id=$1
	shift
	local user_name=$1
	shift
	local group_name=$1
	shift
	local home=$1
	shift
	local opts="$@"
	local rc=0

	local uid=$(getent passwd $user_name | cut -d: -f3)
	if [[ -n "$uid" ]]; then
		if [[ "$uid" -eq "$user_id" ]]; then
			local dir=$(getent passwd $user_name | cut -d: -f6)
			if [[ "$dir" != "$home" ]]; then
				mkdir -p $home
				usermod -d $home $user_name
				rc=${PIPESTATUS[0]}
			fi
		else
			error_noexit "inconsistent user ID:" \
				     "new: $user_id, old: $uid"
			rc=1
		fi
	else
		mkdir -p $home
		useradd -M -u $user_id -d $home -g $group_name $opts $user_name
		rc=${PIPESTATUS[0]}
	fi

	return $rc
}

check_runas_id_ret() {
    local myRC=0
    local myRUNAS_UID=$1
    local myRUNAS_GID=$2
    shift 2
    local myRUNAS=$@
    if [ -z "$myRUNAS" ]; then
        error_exit "myRUNAS command must be specified for check_runas_id"
    fi
    if $GSS_KRB5; then
        $myRUNAS krb5_login.sh || \
            error "Failed to refresh Kerberos V5 TGT for UID $myRUNAS_ID."
    fi
    mkdir $DIR/d0_runas_test
    chmod 0755 $DIR
    chown $myRUNAS_UID:$myRUNAS_GID $DIR/d0_runas_test
    $myRUNAS touch $DIR/d0_runas_test/f$$ || myRC=$?
    rm -rf $DIR/d0_runas_test
    return $myRC
}

check_runas_id() {
    local myRUNAS_UID=$1
    local myRUNAS_GID=$2
    shift 2
    local myRUNAS=$@
    check_runas_id_ret $myRUNAS_UID $myRUNAS_GID $myRUNAS || \
        error "unable to write to $DIR/d0_runas_test as UID $myRUNAS_UID.
        Please set RUNAS_ID to some UID which exists on MDS and client or
        add user $myRUNAS_UID:$myRUNAS_GID on these nodes."
}

# obtain the UID/GID for MPI_USER
get_mpiuser_id() {
    local mpi_user=$1

    MPI_USER_UID=$(do_facet client "getent passwd $mpi_user | cut -d: -f3;
exit \\\${PIPESTATUS[0]}") || error_exit "failed to get the UID for $mpi_user"

    MPI_USER_GID=$(do_facet client "getent passwd $mpi_user | cut -d: -f4;
exit \\\${PIPESTATUS[0]}") || error_exit "failed to get the GID for $mpi_user"
}

# obtain and cache Kerberos ticket-granting ticket
refresh_krb5_tgt() {
    local myRUNAS_UID=$1
    local myRUNAS_GID=$2
    shift 2
    local myRUNAS=$@
    if [ -z "$myRUNAS" ]; then
        error_exit "myRUNAS command must be specified for refresh_krb5_tgt"
    fi

    CLIENTS=${CLIENTS:-$HOSTNAME}
    do_nodes $CLIENTS "set -x
if ! $myRUNAS krb5_login.sh; then
    echo "Failed to refresh Krb5 TGT for UID/GID $myRUNAS_UID/$myRUNAS_GID."
    exit 1
fi"
}

# Run multiop in the background, but wait for it to print
# "PAUSING" to its stdout before returning from this function.
multiop_bg_pause() {
    MULTIOP_PROG=${MULTIOP_PROG:-$MULTIOP}
    FILE=$1
    ARGS=$2

    TMPPIPE=/tmp/multiop_open_wait_pipe.$$
    mkfifo $TMPPIPE

    echo "$MULTIOP_PROG $FILE v$ARGS"
    $MULTIOP_PROG $FILE v$ARGS > $TMPPIPE &

    echo "TMPPIPE=${TMPPIPE}"
    read -t 60 multiop_output < $TMPPIPE
    if [ $? -ne 0 ]; then
        rm -f $TMPPIPE
        return 1
    fi
    rm -f $TMPPIPE
    if [ "$multiop_output" != "PAUSING" ]; then
        echo "Incorrect multiop output: $multiop_output"
        kill -9 $PID
        return 1
    fi

    return 0
}

do_and_time () {
    local cmd=$1
    local rc

    SECONDS=0
    eval '$cmd'

    [ ${PIPESTATUS[0]} -eq 0 ] || rc=1

    echo $SECONDS
    return $rc
}

inodes_available () {
	local IFree=$($LFS df -i $MOUNT | grep ^$FSNAME | awk '{ print $4 }' |
		sort -un | head -n1) || return 1
	echo $((IFree))
}

mdsrate_inodes_available () {
    local min_inodes=$(inodes_available)
    echo $((min_inodes * 99 / 100))
}

# reset llite stat counters
clear_llite_stats(){
        lctl set_param -n llite.*.stats 0
}

# sum llite stat items
calc_llite_stats() {
	local res=$(lctl get_param -n llite.*.stats |
		awk '/^'"$1"'/ {sum += $2} END { printf("%0.0f", sum) }')
	echo $((res))
}

# reset osc stat counters
clear_osc_stats(){
	lctl set_param -n osc.*.osc_stats 0
}

# sum osc stat items
calc_osc_stats() {
	local res=$(lctl get_param -n osc.*.osc_stats |
		awk '/^'"$1"'/ {sum += $2} END { printf("%0.0f", sum) }')
	echo $((res))
}

calc_sum () {
	awk '{sum += $1} END { printf("%0.0f", sum) }'
}

calc_osc_kbytes () {
        df $MOUNT > /dev/null
        $LCTL get_param -n osc.*[oO][sS][cC][-_][0-9a-f]*.$1 | calc_sum
}

# save_lustre_params(comma separated facet list, parameter_mask)
# generate a stream of formatted strings (<facet> <param name>=<param value>)
save_lustre_params() {
	local facets=$1
	local facet
	local nodes
	local node

	for facet in ${facets//,/ }; do
		node=$(facet_active_host $facet)
		[[ *\ $node\ * = " $nodes " ]] && continue
		nodes="$nodes $node"

		do_node $node "$LCTL get_param $2 |
			while read s; do echo $facet \\\$s; done"
	done
}

# restore lustre parameters from input stream, produces by save_lustre_params
restore_lustre_params() {
	local facet
	local name
	local val

	while IFS=" =" read facet name val; do
		do_facet $facet "$LCTL set_param -n $name $val"
	done
}

check_catastrophe() {
	local nodes=${1:-$(comma_list $(nodes_list))}

	do_nodes $nodes "rc=0;
val=\\\$($LCTL get_param -n catastrophe 2>&1);
if [[ \\\$? -eq 0 && \\\$val -ne 0 ]]; then
	echo \\\$(hostname -s): \\\$val;
	rc=\\\$val;
fi;
exit \\\$rc"
}

mdsrate_cleanup () {
	if [ -d $4 ]; then
		mpi_run ${MACHINEFILE_OPTION} $2 -np $1 ${MDSRATE} --unlink \
			--nfiles $3 --dir $4 --filefmt $5 $6
		rmdir $4
	fi
}

delayed_recovery_enabled () {
    local var=${SINGLEMDS}_svc
    do_facet $SINGLEMDS lctl get_param -n mdd.${!var}.stale_export_age > /dev/null 2>&1
}

########################

convert_facet2label() {
    local facet=$1

    if [ x$facet = xost ]; then
       facet=ost1
    fi

    local varsvc=${facet}_svc

    if [ -n ${!varsvc} ]; then
        echo ${!varsvc}
    else
        error "No lablel for $facet!"
    fi
}

get_clientosc_proc_path() {
	echo "${1}-osc-*"
}

# If the 2.0 MDS was mounted on 1.8 device, then the OSC and LOV names
# used by MDT would not be changed.
# mdt lov: fsname-mdtlov
# mdt osc: fsname-OSTXXXX-osc
mds_on_old_device() {
    local mds=${1:-"$SINGLEMDS"}

    if [ $(lustre_version_code $mds) -gt $(version_code 1.9.0) ]; then
        do_facet $mds "lctl list_param osc.$FSNAME-OST*-osc \
            > /dev/null 2>&1" && return 0
    fi
    return 1
}

get_mdtosc_proc_path() {
	local mds_facet=$1
	local ost_label=${2:-"*OST*"}

	[ "$mds_facet" = "mds" ] && mds_facet=$SINGLEMDS
	local mdt_label=$(convert_facet2label $mds_facet)
	local mdt_index=$(echo $mdt_label | sed -e 's/^.*-//')

	if [ $(lustre_version_code $mds_facet) -le $(version_code 1.8.0) ] ||
	   mds_on_old_device $mds_facet; then
		echo "${ost_label}-osc"
	elif [[ $ost_label = *OST* ]]; then
		echo "${ost_label}-osc-${mdt_index}"
	else
		echo "${ost_label}-osp-${mdt_index}"
	fi
}

get_osc_import_name() {
    local facet=$1
    local ost=$2
    local label=$(convert_facet2label $ost)

    if [ "${facet:0:3}" = "mds" ]; then
        get_mdtosc_proc_path $facet $label
        return 0
    fi

    get_clientosc_proc_path $label
    return 0
}

_wait_import_state () {
    local expected=$1
    local CONN_PROC=$2
    local maxtime=${3:-$(max_recovery_time)}
    local error_on_failure=${4:-1}
    local CONN_STATE
    local i=0

	CONN_STATE=$($LCTL get_param -n $CONN_PROC 2>/dev/null | cut -f2 | uniq)
    while [ "${CONN_STATE}" != "${expected}" ]; do
        if [ "${expected}" == "DISCONN" ]; then
            # for disconn we can check after proc entry is removed
            [ "x${CONN_STATE}" == "x" ] && return 0
            #  with AT enabled, we can have connect request timeout near of
            # reconnect timeout and test can't see real disconnect
            [ "${CONN_STATE}" == "CONNECTING" ] && return 0
        fi
	if [ $i -ge $maxtime ]; then
	    [ $error_on_failure -ne 0 ] && \
		error "can't put import for $CONN_PROC into ${expected}" \
		      "state after $i sec, have ${CONN_STATE}"
            return 1
	fi
        sleep 1
	# Add uniq for multi-mount case
	CONN_STATE=$($LCTL get_param -n $CONN_PROC 2>/dev/null | cut -f2 | uniq)
        i=$(($i + 1))
    done

    log "$CONN_PROC in ${CONN_STATE} state after $i sec"
    return 0
}

wait_import_state() {
    local state=$1
    local params=$2
    local maxtime=${3:-$(max_recovery_time)}
    local error_on_failure=${4:-1}
    local param

    for param in ${params//,/ }; do
	_wait_import_state $state $param $maxtime $error_on_failure || return
    done
}

wait_import_state_mount() {
	if ! is_mounted $MOUNT && ! is_mounted $MOUNT2; then
		return 0
	fi

	wait_import_state $*
}

# One client request could be timed out because server was not ready
# when request was sent by client.
# The request timeout calculation details :
# ptl_send_rpc ()
#      /* We give the server rq_timeout secs to process the req, and
#      add the network latency for our local timeout. */
#      request->rq_deadline = request->rq_sent + request->rq_timeout +
#           ptlrpc_at_get_net_latency(request) ;
#
# ptlrpc_connect_import ()
#      request->rq_timeout = INITIAL_CONNECT_TIMEOUT
#
# init_imp_at () ->
#   -> at_init(&at->iat_net_latency, 0, 0) -> iat_net_latency=0
# ptlrpc_at_get_net_latency(request) ->
#       at_get (max (iat_net_latency=0, at_min)) = at_min
#
# i.e.:
# request->rq_timeout + ptlrpc_at_get_net_latency(request) =
# INITIAL_CONNECT_TIMEOUT + at_min
#
# We will use obd_timeout instead of INITIAL_CONNECT_TIMEOUT
# because we can not get this value in runtime,
# the value depends on configure options, and it is not stored in /proc.
# obd_support.h:
# #define CONNECTION_SWITCH_MIN 5U
# #define INITIAL_CONNECT_TIMEOUT max(CONNECTION_SWITCH_MIN,obd_timeout/20)

request_timeout () {
    local facet=$1

    # request->rq_timeout = INITIAL_CONNECT_TIMEOUT
    local init_connect_timeout=$TIMEOUT
    [[ $init_connect_timeout -ge 5 ]] || init_connect_timeout=5

    local at_min=$(at_get $facet at_min)

    echo $(( init_connect_timeout + at_min ))
}

_wait_osc_import_state() {
	local facet=$1
	local ost_facet=$2
	local expected=$3
	local target=$(get_osc_import_name $facet $ost_facet)
	local param="osc.${target}.ost_server_uuid"
	local params=$param
	local i=0

	# 1. wait the deadline of client 1st request (it could be skipped)
	# 2. wait the deadline of client 2nd request
	local maxtime=$(( 2 * $(request_timeout $facet)))

	if [[ $facet == client* ]]; then
		# During setup time, the osc might not be setup, it need wait
		# until list_param can return valid value. And also if there
		# are mulitple osc entries we should list all of them before
		# go to wait.
		params=$($LCTL list_param $param 2>/dev/null || true)
		while [ -z "$params" ]; do
			if [ $i -ge $maxtime ]; then
				echo "can't get $param in $maxtime secs"
				return 1
			fi
			sleep 1
			i=$((i + 1))
			params=$($LCTL list_param $param 2>/dev/null || true)
		done
	fi

	if [[ $ost_facet = mds* ]]; then
		# no OSP connection to itself
		if [[ $facet = $ost_facet ]]; then
			return 0
		fi
		param="osp.${target}.mdt_server_uuid"
		params=$param
	fi

	if ! do_rpc_nodes "$(facet_active_host $facet)" \
			wait_import_state $expected "$params" $maxtime; then
		error "import is not in ${expected} state"
		return 1
	fi

	return 0
}

wait_osc_import_state() {
	local facet=$1
	local ost_facet=$2
	local expected=$3
	local num

	if [[ $facet = mds ]]; then
		for num in $(seq $MDSCOUNT); do
			_wait_osc_import_state mds$num "$ost_facet" "$expected"
		done
	else
		_wait_osc_import_state "$facet" "$ost_facet" "$expected"
	fi
}

_wait_mgc_import_state() {
	local facet=$1
	local expected=$2
	local error_on_failure=${3:-1}
	local param="mgc.*.mgs_server_uuid"
	local params=$param
	local i=0

	# 1. wait the deadline of client 1st request (it could be skipped)
	# 2. wait the deadline of client 2nd request
	local maxtime=$(( 2 * $(request_timeout $facet)))

	if [[ $facet == client* ]]; then
		# During setup time, the osc might not be setup, it need wait
		# until list_param can return valid value. And also if there
		# are mulitple osc entries we should list all of them before
		# go to wait.
		params=$($LCTL list_param $param 2>/dev/null || true)
		while [ -z "$params" ]; do
			if [ $i -ge $maxtime ]; then
				echo "can't get $param in $maxtime secs"
				return 1
			fi
			sleep 1
			i=$((i + 1))
			params=$($LCTL list_param $param 2>/dev/null || true)
		done
	fi
	if ! do_rpc_nodes "$(facet_active_host $facet)" \
			wait_import_state $expected "$params" $maxtime \
					  $error_on_failure; then
		if [ $error_on_failure -ne 0 ]; then
		    error "import is not in ${expected} state"
		fi
		return 1
	fi

	return 0
}

wait_mgc_import_state() {
	local facet=$1
	local expected=$2
	local error_on_failure=${3:-1}
	local num

	if [[ $facet = mds ]]; then
		for num in $(seq $MDSCOUNT); do
			_wait_mgc_import_state mds$num "$expected" \
					       $error_on_failure || return
		done
	else
		_wait_mgc_import_state "$facet" "$expected"
				       $error_on_failure || return
	fi
}

wait_dne_interconnect() {
	local num

	if [ $MDSCOUNT -gt 1 ]; then
		for num in $(seq $MDSCOUNT); do
			wait_osc_import_state mds mds$num FULL
		done
	fi
}

get_clientmdc_proc_path() {
    echo "${1}-mdc-*"
}

get_clientmgc_proc_path() {
    echo "*"
}

do_rpc_nodes () {
	local list=$1
	shift

	[ -z "$list" ] && return 0

	# Add paths to lustre tests for 32 and 64 bit systems.
	local LIBPATH="/usr/lib/lustre/tests:/usr/lib64/lustre/tests:"
	local TESTPATH="$RLUSTRE/tests:"
	local RPATH="PATH=${TESTPATH}${LIBPATH}${PATH}:/sbin:/bin:/usr/sbin:"
	do_nodesv $list "${RPATH} NAME=${NAME} sh rpc.sh $@ "
}

wait_clients_import_state () {
	local list=$1
	local facet=$2
	local expected=$3

	local facets=$facet

	if [ "$FAILURE_MODE" = HARD ]; then
		facets=$(facets_on_host $(facet_active_host $facet))
	fi

	for facet in ${facets//,/ }; do
		local label=$(convert_facet2label $facet)
		local proc_path
		case $facet in
		ost* ) proc_path="osc.$(get_clientosc_proc_path \
				  $label).ost_server_uuid" ;;
		mds* ) proc_path="mdc.$(get_clientmdc_proc_path \
				  $label).mds_server_uuid" ;;
		mgs* ) proc_path="mgc.$(get_clientmgc_proc_path \
				  $label).mgs_server_uuid" ;;
		*) error "unknown facet!" ;;
		esac

		local params=$(expand_list $params $proc_path)
	done

	if ! do_rpc_nodes "$list" wait_import_state_mount $expected $params;
	then
		error "import is not in ${expected} state"
		return 1
	fi
}

oos_full() {
	local -a AVAILA
	local -a GRANTA
	local -a TOTALA
	local OSCFULL=1
	AVAILA=($(do_nodes $(comma_list $(osts_nodes)) \
	          $LCTL get_param obdfilter.*.kbytesavail))
	GRANTA=($(do_nodes $(comma_list $(osts_nodes)) \
	          $LCTL get_param -n obdfilter.*.tot_granted))
	TOTALA=($(do_nodes $(comma_list $(osts_nodes)) \
	          $LCTL get_param -n obdfilter.*.kbytestotal))
	for ((i=0; i<${#AVAILA[@]}; i++)); do
		local -a AVAIL1=(${AVAILA[$i]//=/ })
		local -a TOTAL=(${TOTALA[$i]//=/ })
		GRANT=$((${GRANTA[$i]}/1024))
		# allow 1% of total space in bavail because of delayed
		# allocation with ZFS which might release some free space after
		# txg commit.  For small devices, we set a mininum of 8MB
		local LIMIT=$((${TOTAL} / 100 + 8000))
		echo -n $(echo ${AVAIL1[0]} | cut -d"." -f2) avl=${AVAIL1[1]} \
			grnt=$GRANT diff=$((AVAIL1[1] - GRANT)) limit=${LIMIT}
		[ $((AVAIL1[1] - GRANT)) -lt $LIMIT ] && OSCFULL=0 && \
			echo " FULL" || echo
	done
	return $OSCFULL
}

pool_list () {
   do_facet mgs lctl pool_list $1
}

create_pool() {
    local fsname=${1%%.*}
    local poolname=${1##$fsname.}

    do_facet mgs lctl pool_new $1
    local RC=$?
    # get param should return err unless pool is created
    [[ $RC -ne 0 ]] && return $RC

    wait_update $HOSTNAME "lctl get_param -n lov.$fsname-*.pools.$poolname \
        2>/dev/null || echo foo" "" || RC=1
    if [[ $RC -eq 0 ]]; then
        add_pool_to_list $1
    else
        error "pool_new failed $1"
    fi
    return $RC
}

add_pool_to_list () {
    local fsname=${1%%.*}
    local poolname=${1##$fsname.}

    local listvar=${fsname}_CREATED_POOLS
    eval export ${listvar}=$(expand_list ${!listvar} $poolname)
}

remove_pool_from_list () {
    local fsname=${1%%.*}
    local poolname=${1##$fsname.}

    local listvar=${fsname}_CREATED_POOLS
    eval export ${listvar}=$(exclude_items_from_list ${!listvar} $poolname)
}

destroy_pool_int() {
    local ost
    local OSTS=$(do_facet $SINGLEMDS lctl pool_list $1 | \
        awk '$1 !~ /^Pool:/ {print $1}')
    for ost in $OSTS; do
        do_facet mgs lctl pool_remove $1 $ost
    done
    do_facet mgs lctl pool_destroy $1
}

# <fsname>.<poolname> or <poolname>
destroy_pool() {
    local fsname=${1%%.*}
    local poolname=${1##$fsname.}

    [[ x$fsname = x$poolname ]] && fsname=$FSNAME

    local RC

    pool_list $fsname.$poolname || return $?

    destroy_pool_int $fsname.$poolname
    RC=$?
    [[ $RC -ne 0 ]] && return $RC

    wait_update $HOSTNAME "lctl get_param -n lov.$fsname-*.pools.$poolname \
      2>/dev/null || echo foo" "foo" || RC=1

    if [[ $RC -eq 0 ]]; then
        remove_pool_from_list $fsname.$poolname
    else
        error "destroy pool failed $1"
    fi
    return $RC
}

destroy_pools () {
    local fsname=${1:-$FSNAME}
    local poolname
    local listvar=${fsname}_CREATED_POOLS

    pool_list $fsname

    [ x${!listvar} = x ] && return 0

    echo destroy the created pools: ${!listvar}
    for poolname in ${!listvar//,/ }; do
        destroy_pool $fsname.$poolname
    done
}

cleanup_pools () {
    local fsname=${1:-$FSNAME}
    trap 0
    destroy_pools $fsname
}

gather_logs () {
    local list=$1

    local ts=$(date +%s)
    local docp=true

    if [[ ! -f "$YAML_LOG" ]]; then
        # init_logging is not performed before gather_logs,
        # so the $LOGDIR needs to be checked here
        check_shared_dir $LOGDIR && touch $LOGDIR/shared
    fi

    [ -f $LOGDIR/shared ] && docp=false

    # dump lustre logs, dmesg

    prefix="$TESTLOG_PREFIX.$TESTNAME"
    suffix="$ts.log"
    echo "Dumping lctl log to ${prefix}.*.${suffix}"

    if [ "$CLIENTONLY" -o "$PDSH" == "no_dsh" ]; then
        echo "Dumping logs only on local client."
        $LCTL dk > ${prefix}.debug_log.$(hostname -s).${suffix}
        dmesg > ${prefix}.dmesg.$(hostname -s).${suffix}
        return
    fi

    do_nodesv $list \
        "$LCTL dk > ${prefix}.debug_log.\\\$(hostname -s).${suffix};
         dmesg > ${prefix}.dmesg.\\\$(hostname -s).${suffix}"
    if [ ! -f $LOGDIR/shared ]; then
        do_nodes $list rsync -az "${prefix}.*.${suffix}" $HOSTNAME:$LOGDIR
    fi
}

do_ls () {
    local mntpt_root=$1
    local num_mntpts=$2
    local dir=$3
    local i
    local cmd
    local pids
    local rc=0

    for i in $(seq 0 $num_mntpts); do
        cmd="ls -laf ${mntpt_root}$i/$dir"
        echo + $cmd;
        $cmd > /dev/null &
        pids="$pids $!"
    done
    echo pids=$pids
    for pid in $pids; do
        wait $pid || rc=$?
    done

    return $rc
}

# check_and_start_recovery_timer()
#	service_time = at_est2timeout(service_time);
#	service_time += 2 * INITIAL_CONNECT_TIMEOUT;
#	service_time += 2 * (CONNECTION_SWITCH_MAX + CONNECTION_SWITCH_INC);

#define INITIAL_CONNECT_TIMEOUT max(CONNECTION_SWITCH_MIN, obd_timeout/20)
#define CONNECTION_SWITCH_MAX min(50, max(CONNECTION_SWITCH_MIN, obd_timeout))
#define CONNECTION_SWITCH_MIN 5
#define CONNECTION_SWITCH_INC 5
max_recovery_time() {
	local init_connect_timeout=$((TIMEOUT / 20))
	((init_connect_timeout >= 5)) || init_connect_timeout=5

	local service_time=$(($(at_max_get client) * 9 / 4 + 5))
	service_time=$((service_time + 2 * (init_connect_timeout + 50 + 5)))

	echo -n $service_time
}

recovery_time_min() {
	local connection_switch_min=5
	local connection_switch_inc=5
	local connection_switch_max
	local reconnect_delay_max
	local initial_connect_timeout
	local max
	local timout_20

	#connection_switch_max=min(50, max($connection_switch_min,$TIMEOUT)
	(($connection_switch_min > $TIMEOUT)) &&
		max=$connection_switch_min || max=$TIMEOUT
	(($max < 50)) && connection_switch_max=$max || connection_switch_max=50

	#initial_connect_timeout = max(connection_switch_min, obd_timeout/20)
	timeout_20=$((TIMEOUT/20))
	(($connection_switch_min > $timeout_20)) &&
		initial_connect_timeout=$connection_switch_min ||
		initial_connect_timeout=$timeout_20

	reconnect_delay_max=$((connection_switch_max + connection_switch_inc + \
			       initial_connect_timeout))
	echo $((2 * reconnect_delay_max))
}

get_clients_mount_count () {
    local clients=${CLIENTS:-`hostname`}

    # we need to take into account the clients mounts and
    # exclude mds/ost mounts if any;
    do_nodes $clients cat /proc/mounts | grep lustre | grep $MOUNT | wc -l
}

# gss functions
PROC_CLI="srpc_info"

combination()
{
    local M=$1
    local N=$2
    local R=1

    if [ $M -lt $N ]; then
        R=0
    else
        N=$((N + 1))
        while [ $N -lt $M ]; do
            R=$((R * N))
            N=$((N + 1))
        done
    fi

    echo $R
    return 0
}

calc_connection_cnt() {
    local dir=$1

    # MDT->MDT = 2 * C(M, 2)
    # MDT->OST = M * O
    # CLI->OST = C * O
    # CLI->MDT = C * M
    comb_m2=$(combination $MDSCOUNT 2)

    local num_clients=$(get_clients_mount_count)

    local cnt_mdt2mdt=$((comb_m2 * 2))
    local cnt_mdt2ost=$((MDSCOUNT * OSTCOUNT))
    local cnt_cli2ost=$((num_clients * OSTCOUNT))
    local cnt_cli2mdt=$((num_clients * MDSCOUNT))
    local cnt_all2ost=$((cnt_mdt2ost + cnt_cli2ost))
    local cnt_all2mdt=$((cnt_mdt2mdt + cnt_cli2mdt))
    local cnt_all2all=$((cnt_mdt2ost + cnt_mdt2mdt + cnt_cli2ost + cnt_cli2mdt))

    local var=cnt_$dir
    local res=${!var}

    echo $res
}

set_rule()
{
    local tgt=$1
    local net=$2
    local dir=$3
    local flavor=$4
    local cmd="$tgt.srpc.flavor"

    if [ $net == "any" ]; then
        net="default"
    fi
    cmd="$cmd.$net"

    if [ $dir != "any" ]; then
        cmd="$cmd.$dir"
    fi

    cmd="$cmd=$flavor"
    log "Setting sptlrpc rule: $cmd"
    do_facet mgs "$LCTL conf_param $cmd"
}

count_flvr()
{
    local output=$1
    local flavor=$2
    local count=0

    rpc_flvr=`echo $flavor | awk -F - '{ print $1 }'`
    bulkspec=`echo $flavor | awk -F - '{ print $2 }'`

    count=`echo "$output" | grep "rpc flavor" | grep $rpc_flvr | wc -l`

    if [ "x$bulkspec" != "x" ]; then
        algs=`echo $bulkspec | awk -F : '{ print $2 }'`

        if [ "x$algs" != "x" ]; then
            bulk_count=`echo "$output" | grep "bulk flavor" | grep $algs | wc -l`
        else
            bulk=`echo $bulkspec | awk -F : '{ print $1 }'`
            if [ $bulk == "bulkn" ]; then
                bulk_count=`echo "$output" | grep "bulk flavor" \
                            | grep "null/null" | wc -l`
            elif [ $bulk == "bulki" ]; then
                bulk_count=`echo "$output" | grep "bulk flavor" \
                            | grep "/null" | grep -v "null/" | wc -l`
            else
                bulk_count=`echo "$output" | grep "bulk flavor" \
                            | grep -v "/null" | grep -v "null/" | wc -l`
            fi
        fi

        [ $bulk_count -lt $count ] && count=$bulk_count
    fi

    echo $count
}

flvr_cnt_cli2mdt()
{
    local flavor=$1
    local cnt

    local clients=${CLIENTS:-`hostname`}

    for c in ${clients//,/ }; do
        output=`do_node $c lctl get_param -n mdc.*-MDT*-mdc-*.$PROC_CLI 2>/dev/null`
        tmpcnt=`count_flvr "$output" $flavor`
        cnt=$((cnt + tmpcnt))
    done
    echo $cnt
}

flvr_cnt_cli2ost()
{
    local flavor=$1
    local cnt

    local clients=${CLIENTS:-`hostname`}

    for c in ${clients//,/ }; do
        output=`do_node $c lctl get_param -n osc.*OST*-osc-[^M][^D][^T]*.$PROC_CLI 2>/dev/null`
        tmpcnt=`count_flvr "$output" $flavor`
        cnt=$((cnt + tmpcnt))
    done
    echo $cnt
}

flvr_cnt_mdt2mdt()
{
    local flavor=$1
    local cnt=0

    if [ $MDSCOUNT -le 1 ]; then
        echo 0
        return
    fi

    for num in `seq $MDSCOUNT`; do
        output=`do_facet mds$num lctl get_param -n mdc.*-MDT*-mdc[0-9]*.$PROC_CLI 2>/dev/null`
        tmpcnt=`count_flvr "$output" $flavor`
        cnt=$((cnt + tmpcnt))
    done
    echo $cnt;
}

flvr_cnt_mdt2ost()
{
    local flavor=$1
    local cnt=0
    local mdtosc

    for num in `seq $MDSCOUNT`; do
        mdtosc=$(get_mdtosc_proc_path mds$num)
        mdtosc=${mdtosc/-MDT*/-MDT\*}
        output=$(do_facet mds$num lctl get_param -n \
            osc.$mdtosc.$PROC_CLI 2>/dev/null)
        tmpcnt=`count_flvr "$output" $flavor`
        cnt=$((cnt + tmpcnt))
    done
    echo $cnt;
}

flvr_cnt_mgc2mgs()
{
    local flavor=$1

    output=`do_facet client lctl get_param -n mgc.*.$PROC_CLI 2>/dev/null`
    count_flvr "$output" $flavor
}

do_check_flavor()
{
    local dir=$1        # from to
    local flavor=$2     # flavor expected
    local res=0

    if [ $dir == "cli2mdt" ]; then
        res=`flvr_cnt_cli2mdt $flavor`
    elif [ $dir == "cli2ost" ]; then
        res=`flvr_cnt_cli2ost $flavor`
    elif [ $dir == "mdt2mdt" ]; then
        res=`flvr_cnt_mdt2mdt $flavor`
    elif [ $dir == "mdt2ost" ]; then
        res=`flvr_cnt_mdt2ost $flavor`
    elif [ $dir == "all2ost" ]; then
        res1=`flvr_cnt_mdt2ost $flavor`
        res2=`flvr_cnt_cli2ost $flavor`
        res=$((res1 + res2))
    elif [ $dir == "all2mdt" ]; then
        res1=`flvr_cnt_mdt2mdt $flavor`
        res2=`flvr_cnt_cli2mdt $flavor`
        res=$((res1 + res2))
    elif [ $dir == "all2all" ]; then
        res1=`flvr_cnt_mdt2ost $flavor`
        res2=`flvr_cnt_cli2ost $flavor`
        res3=`flvr_cnt_mdt2mdt $flavor`
        res4=`flvr_cnt_cli2mdt $flavor`
        res=$((res1 + res2 + res3 + res4))
    fi

    echo $res
}

wait_flavor()
{
    local dir=$1        # from to
    local flavor=$2     # flavor expected
    local expect=${3:-$(calc_connection_cnt $dir)}     # number expected

    local res=0

    for ((i=0;i<20;i++)); do
        echo -n "checking $dir..."
        res=$(do_check_flavor $dir $flavor)
        echo "found $res/$expect $flavor connections"
        [ $res -ge $expect ] && return 0
        sleep 4
    done

    echo "Error checking $flavor of $dir: expect $expect, actual $res"
    return 1
}

restore_to_default_flavor()
{
    local proc="mgs.MGS.live.$FSNAME"

    echo "restoring to default flavor..."

    nrule=`do_facet mgs lctl get_param -n $proc 2>/dev/null | grep ".srpc.flavor." | wc -l`

    # remove all existing rules if any
    if [ $nrule -ne 0 ]; then
        echo "$nrule existing rules"
        for rule in `do_facet mgs lctl get_param -n $proc 2>/dev/null | grep ".srpc.flavor."`; do
            echo "remove rule: $rule"
            spec=`echo $rule | awk -F = '{print $1}'`
            do_facet mgs "$LCTL conf_param -d $spec"
        done
    fi

    # verify no rules left
    nrule=`do_facet mgs lctl get_param -n $proc 2>/dev/null | grep ".srpc.flavor." | wc -l`
    [ $nrule -ne 0 ] && error "still $nrule rules left"

    # wait for default flavor to be applied
    # currently default flavor for all connections are 'null'
    wait_flavor all2all null
    echo "now at default flavor settings"
}

set_flavor_all()
{
    local flavor=${1:-null}

    echo "setting all flavor to $flavor"

    # FIXME need parameter to this fn
    # and remove global vars
    local cnt_all2all=$(calc_connection_cnt all2all)

    local res=$(do_check_flavor all2all $flavor)
    if [ $res -eq $cnt_all2all ]; then
        echo "already have total $res $flavor connections"
        return
    fi

    echo "found $res $flavor out of total $cnt_all2all connections"
    restore_to_default_flavor

    [[ $flavor = null ]] && return 0

    set_rule $FSNAME any any $flavor
    wait_flavor all2all $flavor
}


check_logdir() {
    local dir=$1
    # Checking for shared logdir
    if [ ! -d $dir ]; then
        # Not found. Create local logdir
        mkdir -p $dir
    else
        touch $dir/check_file.$(hostname -s)
    fi
    return 0
}

check_write_access() {
	local dir=$1
	local list=${2:-$(comma_list $(nodes_list))}
	local node
	local file

	for node in ${list//,/ }; do
		file=$dir/check_file.$(short_nodename $node)
		if [[ ! -f "$file" ]]; then
			# Logdir not accessible/writable from this node.
			return 1
		fi
		rm -f $file || return 1
	done
	return 0
}

init_logging() {
    if [[ -n $YAML_LOG ]]; then
        return
    fi
    local SAVE_UMASK=`umask`
    umask 0000

    export YAML_LOG=${LOGDIR}/results.yml
    mkdir -p $LOGDIR
    init_clients_lists

    if [ ! -f $YAML_LOG ]; then       # If the yaml log already exists then we will just append to it
      if check_shared_dir $LOGDIR; then
          touch $LOGDIR/shared
          echo "Logging to shared log directory: $LOGDIR"
      else
          echo "Logging to local directory: $LOGDIR"
      fi

      yml_nodes_file $LOGDIR >> $YAML_LOG
      yml_results_file >> $YAML_LOG
    fi

    umask $SAVE_UMASK
}

log_test() {
    yml_log_test $1 >> $YAML_LOG
}

log_test_status() {
     yml_log_test_status $@ >> $YAML_LOG
}

log_sub_test_begin() {
    yml_log_sub_test_begin "$@" >> $YAML_LOG
}

log_sub_test_end() {
    yml_log_sub_test_end "$@" >> $YAML_LOG
}

run_llverdev()
{
        local dev=$1
        local llverdev_opts=$2
        local devname=$(basename $1)
        local size=$(grep "$devname"$ /proc/partitions | awk '{print $3}')
        # loop devices aren't in /proc/partitions
        [ "x$size" == "x" ] && local size=$(ls -l $dev | awk '{print $5}')

        size=$(($size / 1024 / 1024)) # Gb

        local partial_arg=""
        # Run in partial (fast) mode if the size
        # of a partition > 1 GB
        [ $size -gt 1 ] && partial_arg="-p"

        llverdev --force $partial_arg $llverdev_opts $dev
}

run_llverfs()
{
        local dir=$1
        local llverfs_opts=$2
        local use_partial_arg=$3
        local partial_arg=""
        local size=$(df -B G $dir |tail -n 1 |awk '{print $2}' |sed 's/G//') #GB

        # Run in partial (fast) mode if the size
        # of a partition > 1 GB
        [ "x$use_partial_arg" != "xno" ] && [ $size -gt 1 ] && partial_arg="-p"

        llverfs $partial_arg $llverfs_opts $dir
}

#Remove objects from OST
remove_ost_objects() {
	local facet=$1
	local ostdev=$2
	local group=$3
	shift 3
	local objids="$@"
	local mntpt=$(facet_mntpt $facet)
	local opts=$OST_MOUNT_OPTS
	local i
	local rc

	echo "removing objects from $ostdev on $facet: $objids"
	if ! test -b $ostdev; then
		opts=$(csa_add "$opts" -o loop)
	fi
	mount -t $(facet_fstype $facet) $opts $ostdev $mntpt ||
		return $?
	rc=0
	for i in $objids; do
		rm $mntpt/O/$group/d$((i % 32))/$i || { rc=$?; break; }
	done
	umount -f $mntpt || return $?
	return $rc
}

#Remove files from MDT
remove_mdt_files() {
	local facet=$1
	local mdtdev=$2
	shift 2
	local files="$@"
	local mntpt=$(facet_mntpt $facet)
	local opts=$MDS_MOUNT_OPTS

	echo "removing files from $mdtdev on $facet: $files"
	if [ $(facet_fstype $facet) == ldiskfs ] &&
	   ! do_facet $facet test -b $mdtdev; then
		opts=$(csa_add "$opts" -o loop)
	fi
	mount -t $(facet_fstype $facet) $opts $mdtdev $mntpt ||
		return $?
	rc=0
	for f in $files; do
		rm $mntpt/ROOT/$f || { rc=$?; break; }
	done
	umount -f $mntpt || return $?
	return $rc
}

duplicate_mdt_files() {
	local facet=$1
	local mdtdev=$2
	shift 2
	local files="$@"
	local mntpt=$(facet_mntpt $facet)
	local opts=$MDS_MOUNT_OPTS

	echo "duplicating files on $mdtdev on $facet: $files"
	mkdir -p $mntpt || return $?
	if [ $(facet_fstype $facet) == ldiskfs ] &&
	   ! do_facet $facet test -b $mdtdev; then
		opts=$(csa_add "$opts" -o loop)
	fi
	mount -t $(facet_fstype $facet) $opts $mdtdev $mntpt ||
		return $?

    do_umount() {
        trap 0
        popd > /dev/null
        rm $tmp
        umount -f $mntpt
    }
    trap do_umount EXIT

    tmp=$(mktemp $TMP/setfattr.XXXXXXXXXX)
    pushd $mntpt/ROOT > /dev/null || return $?
    rc=0
    for f in $files; do
        touch $f.bad || return $?
        getfattr -n trusted.lov $f | sed "s#$f#&.bad#" > $tmp
        rc=${PIPESTATUS[0]}
        [ $rc -eq 0 ] || return $rc
        setfattr --restore $tmp || return $?
    done
    do_umount
}

run_sgpdd () {
    local devs=${1//,/ }
    shift
    local params=$@
    local rslt=$TMP/sgpdd_survey

    # sgpdd-survey cleanups ${rslt}.* files

    local cmd="rslt=$rslt $params scsidevs=\"$devs\" $SGPDDSURVEY"
    echo + $cmd
    eval $cmd
    cat ${rslt}.detail
}

# returns the canonical name for an ldiskfs device
ldiskfs_canon() {
        local dev="$1"
        local facet="$2"

        do_facet $facet "dv=\\\$(lctl get_param -n $dev);
if foo=\\\$(lvdisplay -c \\\$dv 2>/dev/null); then
    echo dm-\\\${foo##*:};
else
    echo \\\$(basename \\\$dv);
fi;"
}

is_sanity_benchmark() {
    local benchmarks="dbench bonnie iozone fsx"
    local suite=$1
    for b in $benchmarks; do
        if [ "$b" == "$suite" ]; then
            return 0
        fi
    done
    return 1
}

min_ost_size () {
    $LCTL get_param -n osc.*.kbytesavail | sort -n | head -n1
}

#
# Get the available size (KB) of a given obd target.
#
get_obd_size() {
	local facet=$1
	local obd=$2
	local size

	[[ $facet != client ]] || return 0

	size=$(do_facet $facet $LCTL get_param -n *.$obd.kbytesavail | head -n1)
	echo -n $size
}

#
# Get the page size (bytes) on a given facet node.
#
get_page_size() {
	local facet=$1
	local size

	size=$(do_facet $facet getconf PAGE_SIZE)
	[[ ${PIPESTATUS[0]} = 0 && -n "$size" ]] || size=4096
	echo -n $size
}

#
# Get the block count of the filesystem.
#
get_block_count() {
	local facet=$1
	local device=$2
	local count

	count=$(do_facet $facet "$DUMPE2FS -h $device 2>&1" |
		awk '/^Block count:/ {print $3}')
	echo -n $count
}

# Get the block size of the filesystem.
get_block_size() {
    local facet=$1
    local device=$2
    local size

    size=$(do_facet $facet "$DUMPE2FS -h $device 2>&1" |
           awk '/^Block size:/ {print $3}')
    echo $size
}

# Check whether the "large_xattr" feature is enabled or not.
large_xattr_enabled() {
	[[ $(facet_fstype $SINGLEMDS) == zfs ]] && return 0

	local mds_dev=$(mdsdevname ${SINGLEMDS//mds/})

	do_facet $SINGLEMDS "$DUMPE2FS -h $mds_dev 2>&1 |
		grep -E -q '(ea_inode|large_xattr)'"
	return ${PIPESTATUS[0]}
}

# Get the maximum xattr size supported by the filesystem.
max_xattr_size() {
    local size

    if large_xattr_enabled; then
        # include/linux/limits.h: #define XATTR_SIZE_MAX 65536
        size=65536
    else
        local mds_dev=$(mdsdevname ${SINGLEMDS//mds/})
        local block_size=$(get_block_size $SINGLEMDS $mds_dev)

        # maximum xattr size = size of block - size of header -
        #                      size of 1 entry - 4 null bytes
        size=$((block_size - 32 - 32 - 4))
    fi

    echo $size
}

# Dump the value of the named xattr from a file.
get_xattr_value() {
    local xattr_name=$1
    local file=$2

    echo "$(getfattr -n $xattr_name --absolute-names --only-values $file)"
}

# Generate a string with size of $size bytes.
generate_string() {
    local size=${1:-1024} # in bytes

    echo "$(head -c $size < /dev/zero | tr '\0' y)"
}

reformat_external_journal() {
	local facet=$1
	local var

	var=${facet}_JRN
	if [ -n "${!var}" ]; then
		local rcmd="do_facet $facet"

		echo "reformat external journal on $facet:${!var}"
		${rcmd} mke2fs -O journal_dev ${!var} || return 1
	fi
}

# MDT file-level backup/restore
mds_backup_restore() {
	local facet=$1
	local igif=$2
	local devname=$(mdsdevname $(facet_number $facet))
	local mntpt=$(facet_mntpt brpt)
	local rcmd="do_facet $facet"
	local metaea=${TMP}/backup_restore.ea
	local metadata=${TMP}/backup_restore.tgz
	local opts=${MDS_MOUNT_OPTS}
	local svc=${facet}_svc

	if ! ${rcmd} test -b ${devname}; then
		opts=$(csa_add "$opts" -o loop)
	fi

	echo "file-level backup/restore on $facet:${devname}"

	# step 1: build mount point
	${rcmd} mkdir -p $mntpt
	# step 2: cleanup old backup
	${rcmd} rm -f $metaea $metadata
	# step 3: mount dev
	${rcmd} mount -t ldiskfs $opts $devname $mntpt || return 1
	if [ ! -z $igif ]; then
		# step 3.5: rm .lustre
		${rcmd} rm -rf $mntpt/ROOT/.lustre || return 1
	fi
	# step 4: backup metaea
	echo "backup EA"
	${rcmd} "cd $mntpt && getfattr -R -d -m '.*' -P . > $metaea && cd -" ||
		return 2
	# step 5: backup metadata
	echo "backup data"
	${rcmd} tar zcf $metadata -C $mntpt/ . > /dev/null 2>&1 || return 3
	# step 6: umount
	${rcmd} $UMOUNT $mntpt || return 4
	# step 8: reformat dev
	echo "reformat new device"
	format_mdt $(facet_number $facet)
	# step 9: mount dev
	${rcmd} mount -t ldiskfs $opts $devname $mntpt || return 7
	# step 10: restore metadata
	echo "restore data"
	${rcmd} tar zxfp $metadata -C $mntpt > /dev/null 2>&1 || return 8
	# step 11: restore metaea
	echo "restore EA"
	${rcmd} "cd $mntpt && setfattr --restore=$metaea && cd - " || return 9
	# step 12: remove recovery logs
	echo "remove recovery logs"
	${rcmd} rm -fv $mntpt/OBJECTS/* $mntpt/CATALOGS
	# step 13: umount dev
	${rcmd} $UMOUNT $mntpt || return 10
	# step 14: cleanup tmp backup
	${rcmd} rm -f $metaea $metadata
	# step 15: reset device label - it's not virgin on
	${rcmd} e2label $devname ${!svc}
}

# remove OI files
mds_remove_ois() {
	local facet=$1
	local idx=$2
	local devname=$(mdsdevname $(facet_number $facet))
	local mntpt=$(facet_mntpt brpt)
	local rcmd="do_facet $facet"
	local opts=${MDS_MOUNT_OPTS}

	if ! ${rcmd} test -b ${devname}; then
		opts=$(csa_add "$opts" -o loop)
	fi

	echo "removing OI files on $facet: idx=${idx}"

	# step 1: build mount point
	${rcmd} mkdir -p $mntpt
	# step 2: mount dev
	${rcmd} mount -t ldiskfs $opts $devname $mntpt || return 1
	if [ -z $idx ]; then
		# step 3: remove all OI files
		${rcmd} rm -fv $mntpt/oi.16*
	elif [ $idx -lt 2 ]; then
		${rcmd} rm -fv $mntpt/oi.16.${idx}
	else
		local i

		# others, rm oi.16.[idx, idx * idx, idx ** ...]
		for ((i=${idx}; i<64; i=$((i * idx)))); do
			${rcmd} rm -fv $mntpt/oi.16.${i}
		done
	fi
	# step 4: umount
	${rcmd} $UMOUNT $mntpt || return 2
	# OI files will be recreated when mounted as lustre next time.
}

# generate maloo upload-able log file name
# \param logname specify unique part of file name
generate_logname() {
	local logname=${1:-"default_logname"}

	echo "$TESTLOG_PREFIX.$TESTNAME.$logname.$(hostname -s).log"
}

# make directory on different MDTs
test_mkdir() {
	local path
	local p_option
	local stripe_count=2
	local stripe_index=-1
	local OPTIND=1

	while getopts "c:i:p" opt; do
		case $opt in
			c) stripe_count=$OPTARG;;
			i) stripe_index=$OPTARG;;
			p) p_option="-p";;
			\?) error "only support -i -c -p";;
		esac
	done

	shift $((OPTIND - 1))
	[ $# -eq 1 ] || error "Only creating single directory is supported"
	path="$*"

	if [ "$p_option" == "-p" ]; then
		local parent=$(dirname $path)

		[ -d $path ] && return 0
		[ ! -d ${parent} ] && mkdir -p ${parent}
	fi

	if [ $MDSCOUNT -le 1 ]; then
		mkdir $path
	else
		local test_num=$(echo $testnum | sed -e 's/[^0-9]*//g')
		local mdt_index

		if [ $stripe_index -eq -1 ]; then
			mdt_index=$((test_num % MDSCOUNT))
		else
			mdt_index=$stripe_index
		fi
		echo "striped dir -i$mdt_index -c$stripe_count $path"
		$LFS setdirstripe -i$mdt_index -c$stripe_count $path
	fi
}

# find the smallest and not in use file descriptor
free_fd()
{
        local max_fd=$(ulimit -n)
        local fd=3
        while [[ $fd -le $max_fd && -e /proc/self/fd/$fd ]]; do
                ((++fd))
        done
        [ $fd -lt $max_fd ] || error "finding free file descriptor failed"
        echo $fd
}

check_mount_and_prep()
{
	is_mounted $MOUNT || setupall

	rm -rf $DIR/[df][0-9]* || error "Fail to cleanup the env!"
	mkdir $DIR/$tdir || error "Fail to mkdir $DIR/$tdir."
	for idx in $(seq $MDSCOUNT); do
		local name="MDT$(printf '%04x' $((idx - 1)))"
		rm -rf $MOUNT/.lustre/lost+found/$name/*
	done
}

# calcule how many ost-objects to be created.
precreated_ost_obj_count()
{
	local mdt_idx=$1
	local ost_idx=$2
	local mdt_name="MDT$(printf '%04x' $mdt_idx)"
	local ost_name="OST$(printf '%04x' $ost_idx)"
	local proc_path="${FSNAME}-${ost_name}-osc-${mdt_name}"
	local last_id=$(do_facet mds$((mdt_idx + 1)) lctl get_param -n \
			osp.$proc_path.prealloc_last_id)
	local next_id=$(do_facet mds$((mdt_idx + 1)) lctl get_param -n \
			osp.$proc_path.prealloc_next_id)
	echo $((last_id - next_id + 1))
}

check_file_in_pool()
{
	local file=$1
	local pool=$2
	local tlist="$3"
	local res=$($GETSTRIPE $file | grep 0x | cut -f2)
	for i in $res
	do
		for t in $tlist ; do
			[ "$i" -eq "$t" ] && continue 2
		done

		echo "pool list: $tlist"
		echo "striping: $res"
		error_noexit "$file not allocated in $pool"
		return 1
	done
	return 0
}

pool_add() {
	echo "Creating new pool"
	local pool=$1

	create_pool $FSNAME.$pool ||
		{ error_noexit "No pool created, result code $?"; return 1; }
	[ $($LFS pool_list $FSNAME | grep -c "$FSNAME.${pool}\$") -eq 1 ] ||
		{ error_noexit "$pool not in lfs pool_list"; return 2; }
}

pool_add_targets() {
	echo "Adding targets to pool"
	local pool=$1
	local first=$2
	local last=$3
	local step=${4:-1}

	local list=$(seq $first $step $last)

	local t=$(for i in $list; do printf "$FSNAME-OST%04x_UUID " $i; done)
	do_facet mgs $LCTL pool_add \
			$FSNAME.$pool $FSNAME-OST[$first-$last/$step]
	wait_update $HOSTNAME "lctl get_param -n lov.$FSNAME-*.pools.$pool \
			| sort -u | tr '\n' ' ' " "$t" || {
		error_noexit "Add to pool failed"
		return 1
	}
	local lfscount=$($LFS pool_list $FSNAME.$pool | grep -c "\-OST")
	local addcount=$(((last - first) / step + 1))
	[ $lfscount -eq $addcount ] || {
		error_noexit "lfs pool_list bad ost count" \
						"$lfscount != $addcount"
		return 2
	}
}

pool_set_dir() {
	local pool=$1
	local tdir=$2
	echo "Setting pool on directory $tdir"

	$SETSTRIPE -c 2 -p $pool $tdir && return 0

	error_noexit "Cannot set pool $pool to $tdir"
	return 1
}

pool_check_dir() {
	local pool=$1
	local tdir=$2
	echo "Checking pool on directory $tdir"

	local res=$($GETSTRIPE --pool $tdir | sed "s/\s*$//")
	[ "$res" = "$pool" ] && return 0

	error_noexit "Pool on '$tdir' is '$res', not '$pool'"
	return 1
}

pool_dir_rel_path() {
	echo "Testing relative path works well"
	local pool=$1
	local tdir=$2
	local root=$3

	mkdir -p $root/$tdir/$tdir
	cd $root/$tdir
	pool_set_dir $pool $tdir          || return 1
	pool_set_dir $pool ./$tdir        || return 2
	pool_set_dir $pool ../$tdir       || return 3
	pool_set_dir $pool ../$tdir/$tdir || return 4
	rm -rf $tdir; cd - > /dev/null
}

pool_alloc_files() {
	echo "Checking files allocation from directory pool"
	local pool=$1
	local tdir=$2
	local count=$3
	local tlist="$4"

	local failed=0
	for i in $(seq -w 1 $count)
	do
		local file=$tdir/file-$i
		touch $file
		check_file_in_pool $file $pool "$tlist" || \
			failed=$((failed + 1))
	done
	[ "$failed" = 0 ] && return 0

	error_noexit "$failed files not allocated in $pool"
	return 1
}

pool_create_files() {
	echo "Creating files in pool"
	local pool=$1
	local tdir=$2
	local count=$3
	local tlist="$4"

	mkdir -p $tdir
	local failed=0
	for i in $(seq -w 1 $count)
	do
		local file=$tdir/spoo-$i
		$SETSTRIPE -p $pool $file
		check_file_in_pool $file $pool "$tlist" || \
			failed=$((failed + 1))
	done
	[ "$failed" = 0 ] && return 0

	error_noexit "$failed files not allocated in $pool"
	return 1
}

pool_lfs_df() {
	echo "Checking 'lfs df' output"
	local pool=$1

	local t=$($LCTL get_param -n lov.$FSNAME-clilov-*.pools.$pool |
			tr '\n' ' ')
	local res=$($LFS df --pool $FSNAME.$pool |
			awk '{print $1}' |
			grep "$FSNAME-OST" |
			tr '\n' ' ')
	[ "$res" = "$t" ] && return 0

	error_noexit "Pools OSTs '$t' is not '$res' that lfs df reports"
	return 1
}

pool_file_rel_path() {
	echo "Creating files in a pool with relative pathname"
	local pool=$1
	local tdir=$2

	mkdir -p $tdir ||
		{ error_noexit "unable to create $tdir"; return 1 ; }
	local file="/..$tdir/$tfile-1"
	$SETSTRIPE -p $pool $file ||
		{ error_noexit "unable to create $file" ; return 2 ; }

	cd $tdir
	$SETSTRIPE -p $pool $tfile-2 || {
		error_noexit "unable to create $tfile-2 in $tdir"
		return 3
	}
}

pool_remove_first_target() {
	echo "Removing first target from a pool"
	local pool=$1

	local pname="lov.$FSNAME-*.pools.$pool"
	local t=$($LCTL get_param -n $pname | head -1)
	do_facet mgs $LCTL pool_remove $FSNAME.$pool $t
	wait_update $HOSTNAME "lctl get_param -n $pname | grep $t" "" || {
		error_noexit "$t not removed from $FSNAME.$pool"
		return 1
	}
}

pool_remove_all_targets() {
	echo "Removing all targets from pool"
	local pool=$1
	local file=$2
	local pname="lov.$FSNAME-*.pools.$pool"
	for t in $($LCTL get_param -n $pname | sort -u)
	do
		do_facet mgs $LCTL pool_remove $FSNAME.$pool $t
	done
	wait_update $HOSTNAME "lctl get_param -n $pname" "" || {
		error_noexit "Pool $FSNAME.$pool cannot be drained"
		return 1
	}
	# striping on an empty/nonexistant pool should fall back
	# to "pool of everything"
	touch $file || {
		error_noexit "failed to use fallback striping for empty pool"
		return 2
	}
	# setstripe on an empty pool should fail
	$SETSTRIPE -p $pool $file 2>/dev/null && {
		error_noexit "expected failure when creating file" \
							"with empty pool"
		return 3
	}
	return 0
}

pool_remove() {
	echo "Destroying pool"
	local pool=$1
	local file=$2

	do_facet mgs $LCTL pool_destroy $FSNAME.$pool

	sleep 2
	# striping on an empty/nonexistant pool should fall back
	# to "pool of everything"
	touch $file || {
		error_noexit "failed to use fallback striping for missing pool"
		return 1
	}
	# setstripe on an empty pool should fail
	$SETSTRIPE -p $pool $file 2>/dev/null && {
		error_noexit "expected failure when creating file" \
							"with missing pool"
		return 2
	}

	# get param should return err once pool is gone
	if wait_update $HOSTNAME "lctl get_param -n \
		lov.$FSNAME-*.pools.$pool 2>/dev/null || echo foo" "foo"
	then
		remove_pool_from_list $FSNAME.$pool
		return 0
	fi
	error_noexit "Pool $FSNAME.$pool is not destroyed"
	return 3
}

# Get and check the actual stripe count of one file.
# Usage: check_stripe_count <file> <expected_stripe_count>
check_stripe_count() {
	local file=$1
	local expected=$2
	local actual

	[[ -z "$file" || -z "$expected" ]] &&
		error "check_stripe_count: invalid argument"

	local cmd="$GETSTRIPE -c $file"
	actual=$($cmd) || error "$cmd failed"
	actual=${actual%% *}

	if [[ $actual -ne $expected ]]; then
		[[ $expected -eq -1 ]] ||
			error "$cmd wrong: found $actual, expected $expected"
		[[ $actual -eq $OSTCOUNT ]] ||
			error "$cmd wrong: found $actual, expected $OSTCOUNT"
	fi
}

# Get and check the actual list of OST indices on one file.
# Usage: check_obdidx <file> <expected_comma_separated_list_of_ost_indices>
check_obdidx() {
	local file=$1
	local expected=$2
	local obdidx

	[[ -z "$file" || -z "$expected" ]] &&
		error "check_obdidx: invalid argument!"

	obdidx=$(comma_list $($GETSTRIPE $file | grep -A $OSTCOUNT obdidx |
			      grep -v obdidx | awk '{print $1}' | xargs))

	[[ $obdidx = $expected ]] ||
		error "list of OST indices on $file is $obdidx," \
		      "should be $expected"
}

# Get and check the actual OST index of the first stripe on one file.
# Usage: check_start_ost_idx <file> <expected_start_ost_idx>
check_start_ost_idx() {
	local file=$1
	local expected=$2
	local start_ost_idx

	[[ -z "$file" || -z "$expected" ]] &&
		error "check_start_ost_idx: invalid argument!"

	start_ost_idx=$($GETSTRIPE $file | grep -A 1 obdidx | grep -v obdidx |
			awk '{print $1}')

	[[ $start_ost_idx = $expected ]] ||
		error "OST index of the first stripe on $file is" \
		      "$start_ost_idx, should be $expected"
}

killall_process () {
	local clients=${1:-$(hostname)}
	local name=$2
	local signal=$3
	local rc=0

	do_nodes $clients "killall $signal $name"
}
