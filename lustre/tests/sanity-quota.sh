#!/bin/bash
#
# Run select tests by setting ONLY, or as arguments to the script.
# Skip specific tests by setting EXCEPT.
#
# Run test by setting NOSETUP=true when ltest has setup env for us
set -e

SRCDIR=`dirname $0`
export PATH=$PWD/$SRCDIR:$SRCDIR:$PWD/$SRCDIR/../utils:$PATH:/sbin

ONLY=${ONLY:-"$*"}
ALWAYS_EXCEPT="$SANITY_QUOTA_EXCEPT"
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

[ "$ALWAYS_EXCEPT$EXCEPT" ] &&
	echo "Skipping tests: `echo $ALWAYS_EXCEPT $EXCEPT`"

TMP=${TMP:-/tmp}

ORIG_PWD=${PWD}
TSTID=${TSTID:-60000}
TSTID2=${TSTID2:-60001}
TSTUSR=${TSTUSR:-"quota_usr"}
TSTUSR2=${TSTUSR2:-"quota_2usr"}
BLK_SZ=1024
MAX_DQ_TIME=604800
MAX_IQ_TIME=604800

LUSTRE=${LUSTRE:-$(cd $(dirname $0)/..; echo $PWD)}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}
init_logging
DIRECTIO=${DIRECTIO:-$LUSTRE/tests/directio}

require_dsh_mds || exit 0
require_dsh_ost || exit 0

# XXX Once we drop the interoperability with old server (< 2.3.50), the
#     sanity-quota-old.sh should be removed.
if [ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.3.50) ]; then
	exec $LUSTRE/tests/sanity-quota-old.sh
fi

# Does e2fsprogs support quota feature?
if [ $(facet_fstype $SINGLEMDS) == ldiskfs ] &&
	do_facet $SINGLEMDS "! $DEBUGFS -c -R supported_features |
		grep -q 'quota'"; then
	skip "e2fsprogs doesn't support quota" && exit 0
fi

if [ $(facet_fstype $SINGLEMDS) = "zfs" ]; then
# bug number for skipped test:        LU-2872 LU-2836 LU-2836 LU-2059
	ALWAYS_EXCEPT="$ALWAYS_EXCEPT 1       3       6       7d"
# bug number:     LU-2887
	ZFS_SLOW="12a"
fi

[ "$SLOW" = "no" ] && EXCEPT_SLOW="$ZFS_SLOW 9 18 21"

QUOTALOG=${TESTSUITELOG:-$TMP/$(basename $0 .sh).log}

[ "$QUOTALOG" ] && rm -f $QUOTALOG || true

DIR=${DIR:-$MOUNT}
DIR2=${DIR2:-$MOUNT2}

QUOTA_AUTO_OLD=$QUOTA_AUTO
export QUOTA_AUTO=0

check_and_setup_lustre

LOVNAME=`lctl get_param -n llite.*.lov.common_name | tail -n 1`
OSTCOUNT=`lctl get_param -n lov.$LOVNAME.numobd`

SHOW_QUOTA_USER="$LFS quota -v -u $TSTUSR $DIR"
SHOW_QUOTA_USERID="$LFS quota -v -u $TSTID $DIR"
SHOW_QUOTA_USER2="$LFS quota -v -u $TSTUSR2 $DIR"
SHOW_QUOTA_GROUP="$LFS quota -v -g $TSTUSR $DIR"
SHOW_QUOTA_GROUPID="$LFS quota -v -g $TSTID $DIR"
SHOW_QUOTA_GROUP2="$LFS quota -v -g $TSTUSR2 $DIR"
SHOW_QUOTA_INFO_USER="$LFS quota -t -u $DIR"
SHOW_QUOTA_INFO_GROUP="$LFS quota -t -g $DIR"

build_test_filter

lustre_fail() {
        local fail_node=$1
	local fail_loc=$2
	local fail_val=${3:-0}

	if [ $fail_node == "mds" ] || [ $fail_node == "mds_ost" ]; then
		do_facet $SINGLEMDS "lctl set_param fail_val=$fail_val"
		do_facet $SINGLEMDS "lctl set_param fail_loc=$fail_loc"
	fi

	if [ $fail_node == "ost" ] || [ $fail_node == "mds_ost" ]; then
		for num in `seq $OSTCOUNT`; do
			do_facet ost$num "lctl set_param fail_val=$fail_val"
			do_facet ost$num "lctl set_param fail_loc=$fail_loc"
		done
	fi
}

RUNAS="runas -u $TSTID -g $TSTID"
RUNAS2="runas -u $TSTID2 -g $TSTID2"
DD="dd if=/dev/zero bs=1M"

FAIL_ON_ERROR=false

check_runas_id_ret $TSTUSR $TSTUSR $RUNAS ||
	error "Please create user $TSTUSR($TSTID) and group $TSTUSR($TSTID)"
check_runas_id_ret $TSTUSR2 $TSTUSR2 $RUNAS2 ||
	error "Please create user $TSTUSR2($TSTID2) and group $TSTUSR2($TSTID2)"

# clear quota limits for a user or a group
# usage: resetquota -u username
#        resetquota -g groupname

resetquota() {
	[ "$#" != 2 ] && error "resetquota: wrong number of arguments: $#"
	[ "$1" != "-u" -a "$1" != "-g" ] &&
		error "resetquota: wrong specifier $1 passed"

	$LFS setquota "$1" "$2" -b 0 -B 0 -i 0 -I 0 $MOUNT ||
		error "clear quota for [type:$1 name:$2] failed"
	# give a chance to slave to release space
	sleep 1
}

quota_scan() {
	local LOCAL_UG=$1
	local LOCAL_ID=$2

	if [ "$LOCAL_UG" == "a" -o "$LOCAL_UG" == "u" ]; then
		$LFS quota -v -u $LOCAL_ID $DIR
		log "Files for user ($LOCAL_ID):"
		($LFS find -user $LOCAL_ID $DIR | head -n 4 |
			xargs stat 2>/dev/null)
	fi

	if [ "$LOCAL_UG" == "a" -o "$LOCAL_UG" == "g" ]; then
		$LFS quota -v -u $LOCAL_ID $DIR
		log "Files for group ($LOCAL_ID):"
		($LFS find -group $LOCAL_ID $DIR | head -n 4 |
			xargs stat 2>/dev/null)
	fi
}

quota_error() {
	quota_scan $1 $2
	shift 2
	error "$*"
}

quota_log() {
	quota_scan $1 $2
	shift 2
	log "$*"
}

# get quota for a user or a group
# usage: getquota -u|-g <username>|<groupname> global|<obd_uuid> \
#		  bhardlimit|bsoftlimit|bgrace|ihardlimit|isoftlimit|igrace
#
getquota() {
	local spec
	local uuid

	[ "$#" != 4 ] && error "getquota: wrong number of arguments: $#"
	[ "$1" != "-u" -a "$1" != "-g" ] &&
		error "getquota: wrong u/g specifier $1 passed"

	uuid="$3"

	case "$4" in
		curspace)   spec=1;;
		bsoftlimit) spec=2;;
		bhardlimit) spec=3;;
		bgrace)     spec=4;;
		curinodes)  spec=5;;
		isoftlimit) spec=6;;
		ihardlimit) spec=7;;
		igrace)     spec=8;;
		*)          error "unknown quota parameter $4";;
	esac

	[ "$uuid" = "global" ] && uuid=$DIR

	$LFS quota -v "$1" "$2" $DIR |
		awk 'BEGIN { num='$spec' } { if ($1 == "'$uuid'") \
		{ if (NF == 1) { getline } else { num++ } ; print $num;} }' \
		| tr -d "*"
}

# set mdt quota type
# usage: set_mdt_qtype ug|u|g|none
set_mdt_qtype() {
	local qtype=$1
	local varsvc
	local mdts=$(get_facets MDS)
	local cmd
	do_facet mgs $LCTL conf_param $FSNAME.quota.mdt=$qtype
	# we have to make sure each MDT received config changes
	for mdt in ${mdts//,/ }; do
		varsvc=${mdt}_svc
		cmd="$LCTL get_param -n "
		cmd=${cmd}osd-$(facet_fstype $mdt).${!varsvc}
		cmd=${cmd}.quota_slave.enabled

		if $(facet_up $mdt); then
			wait_update_facet $mdt "$cmd" "$qtype" || return 1
		fi
	done
	return 0
}

# set ost quota type
# usage: set_ost_qtype ug|u|g|none
set_ost_qtype() {
	local qtype=$1
	local varsvc
	local osts=$(get_facets OST)
	local cmd
	do_facet mgs $LCTL conf_param $FSNAME.quota.ost=$qtype
	# we have to make sure each OST received config changes
	for ost in ${osts//,/ }; do
		varsvc=${ost}_svc
		cmd="$LCTL get_param -n "
		cmd=${cmd}osd-$(facet_fstype $ost).${!varsvc}
		cmd=${cmd}.quota_slave.enabled

		if $(facet_up $ost); then
			wait_update_facet $ost "$cmd" "$qtype" || return 1
		fi
	done
	return 0
}

wait_reintegration() {
	local ntype=$1
	local qtype=$2
	local max=$3
	local result="glb[1],slv[1],reint[0]"
	local varsvc
	local cmd
	local tgts

	if [ $ntype == "mdt" ]; then
		tgts=$(get_facets MDS)
	else
		tgts=$(get_facets OST)
	fi

	for tgt in ${tgts//,/ }; do
		varsvc=${tgt}_svc
		cmd="$LCTL get_param -n "
		cmd=${cmd}osd-$(facet_fstype $tgt).${!varsvc}
		cmd=${cmd}.quota_slave.info

		if $(facet_up $tgt); then
			wait_update_facet $tgt "$cmd |
				grep "$qtype" | awk '{ print \\\$3 }'" \
					"$result" $max || return 1
		fi
	done
	return 0
}

wait_mdt_reint() {
	local qtype=$1
	local max=${2:-90}

	if [ $qtype == "u" ] || [ $qtype == "ug" ]; then
		wait_reintegration "mdt" "user" $max || return 1
	fi

	if [ $qtype == "g" ] || [ $qtype == "ug" ]; then
		wait_reintegration "mdt" "group" $max || return 1
	fi
	return 0
}

wait_ost_reint() {
	local qtype=$1
	local max=${2:-90}

	if [ $qtype == "u" ] || [ $qtype == "ug" ]; then
		wait_reintegration "ost" "user" $max || return 1
	fi

	if [ $qtype == "g" ] || [ $qtype == "ug" ]; then
		wait_reintegration "ost" "group" $max || return 1
	fi
	return 0
}

setup_quota_test() {
	rm -rf $DIR/$tdir
	wait_delete_completed
	echo "Creating test directory"
	mkdir -p $DIR/$tdir
	chmod 0777 $DIR/$tdir
	# always clear fail_loc in case of fail_loc isn't cleared
	# properly when previous test failed
	lustre_fail mds_ost 0
}

cleanup_quota_test() {
	trap 0
	echo "Delete files..."
	rm -rf $DIR/$tdir
	echo "Wait for unlink objects finished..."
	wait_delete_completed
	sync_all_data || true
}

quota_show_check() {
	local bf=$1
	local ug=$2
	local qid=$3
	local usage

	$LFS quota -v -$ug $qid $DIR

	if [ "$bf" == "a" -o "$bf" == "b" ]; then
		usage=$(getquota -$ug $qid global curspace)
		if [ -z $usage ]; then
			quota_error $ug $qid \
				"Query block quota failed ($ug:$qid)."
		else
			[ $usage -ne 0 ] && quota_log $ug $qid \
				"Block quota isn't 0 ($ug:$qid:$usage)."
		fi
	fi

	if [ "$bf" == "a" -o "$bf" == "f" ]; then
		usage=$(getquota -$ug $qid global curinodes)
		if [ -z $usage ]; then
			quota_error $ug $qid \
				"Query file quota failed ($ug:$qid)."
		else
			[ $usage -ne 0 ] && quota_log $ug $qid \
				"File quota isn't 0 ($ug:$qid:$usage)."
		fi
	fi
}

# enable quota debug
quota_init() {
	do_nodes $(comma_list $(nodes_list)) "lctl set_param debug=+quota"
}
quota_init

resetquota -u $TSTUSR
resetquota -g $TSTUSR
resetquota -u $TSTUSR2
resetquota -g $TSTUSR2

test_quota_performance() {
	local TESTFILE="$DIR/$tdir/$tfile-0"
	local size=$1 # in MB
	local stime=$(date +%s)
	$RUNAS $DD of=$TESTFILE count=$size conv=fsync ||
		quota_error u $TSTUSR "write failure"
	local etime=$(date +%s)
	delta=$((etime - stime))
	if [ $delta -gt 0 ]; then
	    rate=$((size * 1024 / delta))
	    if [ $(facet_fstype $SINGLEMDS) = "zfs" ]; then
		# LU-2872 - see LU-2887 for fix
		[ $rate -gt 64 ] ||
			error "SLOW IO for $TSTUSR (user): $rate KB/sec"
	    else
		[ $rate -gt 1024 ] ||
			error "SLOW IO for $TSTUSR (user): $rate KB/sec"
	    fi
	fi
	rm -f $TESTFILE
}

# test basic quota performance b=21696
test_0() {
	local MB=100 # 100M
	[ "$SLOW" = "no" ] && MB=10

	local free_space=$(lfs_df | grep "summary" | awk '{print $4}')
	[ $free_space -le $((MB * 1024)) ] &&
		skip "not enough space ${free_space} KB, " \
			"required $((MB * 1024)) KB" && return
	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "none" || error "disable ost quota failed"
	test_quota_performance $MB

	set_ost_qtype "ug" || error "enable ost quota failed"
	$LFS setquota -u $TSTUSR -b 0 -B 10G -i 0 -I 0 $DIR ||
		error "set quota failed"
	test_quota_performance $MB

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 0 "Test basic quota performance"

# test block hardlimit
test_1() {
	local LIMIT=10  # 10M
	local TESTFILE="$DIR/$tdir/$tfile-0"

	setup_quota_test
	trap cleanup_quota_test EXIT

	# enable ost quota
	set_ost_qtype "ug" || "enable ost quota failed"

	# test for user
	log "User quota (block hardlimit:$LIMIT MB)"
	$LFS setquota -u $TSTUSR -b 0 -B ${LIMIT}M -i 0 -I 0 $DIR ||
		error "set user quota failed"

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for user $TSTUSR isn't 0."

	$LFS setstripe $TESTFILE -c 1
	chown $TSTUSR.$TSTUSR $TESTFILE

	log "Write..."
	$RUNAS $DD of=$TESTFILE count=$((LIMIT/2)) ||
		quota_error u $TSTUSR "user write failure, but expect success"
	log "Write out of block quota ..."
	# this time maybe cache write,  ignore it's failure
	$RUNAS $DD of=$TESTFILE count=$((LIMIT/2)) seek=$((LIMIT/2)) || true
	# flush cache, ensure noquota flag is set on client
	cancel_lru_locks osc
	$RUNAS $DD of=$TESTFILE count=1 seek=$LIMIT &&
		quota_error u $TSTUSR "user write success, but expect EDQUOT"

	rm -f $TESTFILE
	wait_delete_completed
	sync_all_data || true
	USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && quota_error u $TSTUSR \
		"user quota isn't released after deletion"
	resetquota -u $TSTUSR

	# test for group
	log "--------------------------------------"
	log "Group quota (block hardlimit:$LIMIT MB)"
	$LFS setquota -g $TSTUSR -b 0 -B ${LIMIT}M -i 0 -I 0 $DIR ||
		error "set group quota failed"

	TESTFILE="$DIR/$tdir/$tfile-1"
	# make sure the system is clean
	USED=$(getquota -g $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for group $TSTUSR isn't 0"

	$LFS setstripe $TESTFILE -c 1
	chown $TSTUSR.$TSTUSR $TESTFILE

	log "Write ..."
	$RUNAS $DD of=$TESTFILE count=$((LIMIT/2)) ||
		quota_error g $TSTUSR "group write failure, but expect success"
	log "Write out of block quota ..."
	# this time maybe cache write, ignore it's failure
	$RUNAS $DD of=$TESTFILE count=$((LIMIT/2)) seek=$((LIMIT/2)) || true
	cancel_lru_locks osc
	$RUNAS $DD of=$TESTFILE count=10 seek=$LIMIT &&
		quota_error g $TSTUSR "group write success, but expect EDQUOT"

	# cleanup
	cleanup_quota_test

	USED=$(getquota -g $TSTUSR global curspace)
	[ $USED -ne 0 ] && quota_error g $TSTUSR \
		"group quota isn't released after deletion"

	resetquota -g $TSTUSR
}
run_test 1 "Block hard limit (normal use and out of quota)"

# test inode hardlimit
test_2() {
	local LIMIT=$((1024 * 1024)) # 1M inodes
	local TESTFILE="$DIR/$tdir/$tfile-0"

	[ "$SLOW" = "no" ] && LIMIT=1024 # 1k inodes

	local FREE_INODES=$(mdt_free_inodes 0)
	echo "$FREE_INODES free inodes on master MDT"
	[ $FREE_INODES -lt $LIMIT ] &&
		skip "not enough free inodes $FREE_INODES required $LIMIT" &&
		return

	setup_quota_test
	trap cleanup_quota_test EXIT

	# enable mdt quota
	set_mdt_qtype "ug" || "enable mdt quota failed"

	# test for user
	log "User quota (inode hardlimit:$LIMIT files)"
	$LFS setquota -u $TSTUSR -b 0 -B 0 -i 0 -I $LIMIT $DIR ||
		error "set user quota failed"

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curinodes)
	[ $USED -ne 0 ] && error "Used inodes($USED) for user $TSTUSR isn't 0."

	log "Create $LIMIT files ..."
	$RUNAS createmany -m ${TESTFILE} $LIMIT || \
		quota_error u $TSTUSR "user create failure, but expect success"
	log "Create out of file quota ..."
	$RUNAS touch ${TESTFILE}_xxx && \
		quota_error u $TSTUSR "user create success, but expect EDQUOT"

	# cleanup
	unlinkmany ${TESTFILE} $LIMIT
	rm -f ${TESTFILE}_xxx
	wait_delete_completed

	USED=$(getquota -u $TSTUSR global curinodes)
	[ $USED -ne 0 ] && quota_error u $TSTUSR \
		"user quota isn't released after deletion"
	resetquota -u $TSTUSR

	# test for group
	log "--------------------------------------"
	log "Group quota (inode hardlimit:$LIMIT files)"
	$LFS setquota -g $TSTUSR -b 0 -B 0 -i 0 -I $LIMIT $DIR ||
		error "set group quota failed"

        TESTFILE=$DIR/$tdir/$tfile-1
	# make sure the system is clean
	USED=$(getquota -g $TSTUSR global curinodes)
	[ $USED -ne 0 ] && error "Used inodes($USED) for group $TSTUSR isn't 0."

	log "Create $LIMIT files ..."
	$RUNAS createmany -m ${TESTFILE} $LIMIT ||
		quota_error g $TSTUSR "group create failure, but expect success"
	log "Create out of file quota ..."
	$RUNAS touch ${TESTFILE}_xxx && \
		quota_error g $TSTUSR "group create success, but expect EDQUOT"

	# cleanup
	unlinkmany ${TESTFILE} $LIMIT
	rm -f ${TESTFILE}_xxx
	wait_delete_completed

	USED=$(getquota -g $TSTUSR global curinodes)
	[ $USED -ne 0 ] && quota_error g $TSTUSR \
		"user quota isn't released after deletion"

	cleanup_quota_test
	resetquota -g $TSTUSR
}
run_test 2 "File hard limit (normal use and out of quota)"

test_block_soft() {
	local TESTFILE=$1
	local TIMER=$(($2 * 3 / 2))
	local LIMIT=$3
	local OFFSET=0

	setup_quota_test
	trap cleanup_quota_test EXIT

	$LFS setstripe $TESTFILE -c 1 -i 0
	chown $TSTUSR.$TSTUSR $TESTFILE

	echo "Write up to soft limit"
	$RUNAS $DD of=$TESTFILE count=$LIMIT ||
		quota_error a $TSTUSR "write failure, but expect success"
	OFFSET=$((LIMIT * 1024))
	cancel_lru_locks osc

	echo "Write to exceed soft limit"
	$RUNAS dd if=/dev/zero of=$TESTFILE bs=1K count=10 seek=$OFFSET ||
		quota_error a $TSTUSR "write failure, but expect success"
	OFFSET=$((OFFSET + 1024)) # make sure we don't write to same block
	cancel_lru_locks osc

	$SHOW_QUOTA_USER
	$SHOW_QUOTA_GROUP
	$SHOW_QUOTA_INFO_USER
	$SHOW_QUOTA_INFO_GROUP

	echo "Write before timer goes off"
	$RUNAS dd if=/dev/zero of=$TESTFILE bs=1K count=10 seek=$OFFSET ||
		quota_error a $TSTUSR "write failure, but expect success"
	OFFSET=$((OFFSET + 1024))
	cancel_lru_locks osc

	echo "Sleep $TIMER seconds ..."
	sleep $TIMER

	$SHOW_QUOTA_USER
	$SHOW_QUOTA_GROUP
	$SHOW_QUOTA_INFO_USER
	$SHOW_QUOTA_INFO_GROUP

	echo "Write after timer goes off"
	# maybe cache write, ignore.
	$RUNAS dd if=/dev/zero of=$TESTFILE bs=1K count=10 seek=$OFFSET || true
	OFFSET=$((OFFSET + 1024))
	cancel_lru_locks osc
	$RUNAS dd if=/dev/zero of=$TESTFILE bs=1K count=10 seek=$OFFSET &&
		quota_error a $TSTUSR "write success, but expect EDQUOT"

	$SHOW_QUOTA_USER
	$SHOW_QUOTA_GROUP
	$SHOW_QUOTA_INFO_USER
	$SHOW_QUOTA_INFO_GROUP

	echo "Unlink file to stop timer"
	rm -f $TESTFILE
	wait_delete_completed
	sync_all_data || true

	$SHOW_QUOTA_USER
	$SHOW_QUOTA_GROUP
	$SHOW_QUOTA_INFO_USER
	$SHOW_QUOTA_INFO_GROUP

	$LFS setstripe $TESTFILE -c 1 -i 0
	chown $TSTUSR.$TSTUSR $TESTFILE

	echo "Write ..."
	$RUNAS $DD of=$TESTFILE count=$LIMIT ||
		quota_error a $TSTUSR "write failure, but expect success"
	# cleanup
	cleanup_quota_test
}

# block soft limit
test_3() {
	local LIMIT=1  # 1MB
	local GRACE=20 # 20s
	local TESTFILE=$DIR/$tdir/$tfile-0

	set_ost_qtype "ug" || error "enable ost quota failed"

	echo "User quota (soft limit:$LIMIT MB  grace:$GRACE seconds)"
	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for user $TSTUSR isn't 0."

	$LFS setquota -t -u --block-grace $GRACE --inode-grace \
		$MAX_IQ_TIME $DIR || error "set user grace time failed"
	$LFS setquota -u $TSTUSR -b ${LIMIT}M -B 0 -i 0 -I 0 $DIR ||
		error "set user quota failed"

	test_block_soft $TESTFILE $GRACE $LIMIT
	resetquota -u $TSTUSR

	echo "Group quota (soft limit:$LIMIT MB  grace:$GRACE seconds)"
	TESTFILE=$DIR/$tdir/$tfile-1
	# make sure the system is clean
	USED=$(getquota -g $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for group $TSTUSR isn't 0."

	$LFS setquota -t -g --block-grace $GRACE --inode-grace \
		$MAX_IQ_TIME $DIR || error "set group grace time failed"
	$LFS setquota -g $TSTUSR -b ${LIMIT}M -B 0 -i 0 -I 0 $DIR ||
		error "set group quota failed"

	test_block_soft $TESTFILE $GRACE $LIMIT
	resetquota -g $TSTUSR

	# cleanup
	$LFS setquota -t -u --block-grace $MAX_DQ_TIME --inode-grace \
		$MAX_IQ_TIME $DIR || error "restore user grace time failed"
	$LFS setquota -t -g --block-grace $MAX_DQ_TIME --inode-grace \
		$MAX_IQ_TIME $DIR || error "restore group grace time failed"
}
run_test 3 "Block soft limit (start timer, timer goes off, stop timer)"

test_file_soft() {
	local TESTFILE=$1
	local LIMIT=$2
	local TIMER=$(($3 * 3 / 2))

	setup_quota_test
	trap cleanup_quota_test EXIT

	echo "Create files to exceed soft limit"
	$RUNAS createmany -m ${TESTFILE}_ $((LIMIT + 1)) ||
		quota_error a $TSTUSR "create failure, but expect success"
	sync; sleep 1; sync

	echo "Create file before timer goes off"
	$RUNAS touch ${TESTFILE}_before ||
		quota_error a $TSTUSR "failed create before timer expired," \
			"but expect success"
	sync; sleep 1; sync

	echo "Sleep $TIMER seconds ..."
	sleep $TIMER

	$SHOW_QUOTA_USER
	$SHOW_QUOTA_GROUP
	$SHOW_QUOTA_INFO_USER
	$SHOW_QUOTA_INFO_GROUP

	echo "Create file after timer goes off"
	# There is a window that space is accounted in the quota usage but
	# hasn't been decreased from the pending write, if we acquire quota
	# in this window, we'll acquire more than we needed.
	$RUNAS touch ${TESTFILE}_after_1 ${TESTFILE}_after_2 || true
	sync; sleep 1; sync
	$RUNAS touch ${TESTFILE}_after_3 &&
		quota_error a $TSTUSR "create after timer expired," \
			"but expect EDQUOT"
	sync; sleep 1; sync

	$SHOW_QUOTA_USER
	$SHOW_QUOTA_GROUP
	$SHOW_QUOTA_INFO_USER
	$SHOW_QUOTA_INFO_GROUP

	echo "Unlink files to stop timer"
	find $(dirname $TESTFILE) -name "$(basename ${TESTFILE})*" | xargs rm -f
	wait_delete_completed

	echo "Create file"
	$RUNAS touch ${TESTFILE}_xxx ||
		quota_error a $TSTUSR "touch after timer stop failure," \
			"but expect success"
	sync; sleep 1; sync

	# cleanup
	cleanup_quota_test
}

# file soft limit
test_4a() {
	local LIMIT=10 # inodes
	local TESTFILE=$DIR/$tdir/$tfile-0
	local GRACE=5

	set_mdt_qtype "ug" || error "enable mdt quota failed"

	echo "User quota (soft limit:$LIMIT files  grace:$GRACE seconds)"
	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curinodes)
	[ $USED -ne 0 ] && error "Used space($USED) for user $TSTUSR isn't 0."

	$LFS setquota -t -u --block-grace $MAX_DQ_TIME --inode-grace \
		$GRACE $DIR || error "set user grace time failed"
	$LFS setquota -u $TSTUSR -b 0 -B 0 -i $LIMIT -I 0 $DIR ||
		error "set user quota failed"

	test_file_soft $TESTFILE $LIMIT $GRACE
	resetquota -u $TSTUSR

	echo "Group quota (soft limit:$LIMIT files  grace:$GRACE seconds)"
	# make sure the system is clean
	USED=$(getquota -g $TSTUSR global curinodes)
	[ $USED -ne 0 ] && error "Used space($USED) for group $TSTUSR isn't 0."

	$LFS setquota -t -g --block-grace $MAX_DQ_TIME --inode-grace \
		$GRACE $DIR || error "set group grace time failed"
	$LFS setquota -g $TSTUSR -b 0 -B 0 -i $LIMIT -I 0 $DIR ||
		error "set group quota failed"
	TESTFILE=$DIR/$tdir/$tfile-1

	test_file_soft $TESTFILE $LIMIT $GRACE
	resetquota -g $TSTUSR

	# cleanup
	$LFS setquota -t -u --block-grace $MAX_DQ_TIME --inode-grace \
		$MAX_IQ_TIME $DIR || error "restore user grace time failed"
	$LFS setquota -t -g --block-grace $MAX_DQ_TIME --inode-grace \
		$MAX_IQ_TIME $DIR || error "restore group grace time failed"
}
run_test 4a "File soft limit (start timer, timer goes off, stop timer)"

test_4b() {
	local GR_STR1="1w3d"
	local GR_STR2="1000s"
	local GR_STR3="5s"
	local GR_STR4="1w2d3h4m5s"
	local GR_STR5="5c"
	local GR_STR6="1111111111111111"

	wait_delete_completed

	# test of valid grace strings handling
	echo "Valid grace strings test"
	$LFS setquota -t -u --block-grace $GR_STR1 --inode-grace \
		$GR_STR2 $DIR || error "set user grace time failed"
	$LFS quota -u -t $DIR | grep "Block grace time: $GR_STR1"
	$LFS setquota -t -g --block-grace $GR_STR3 --inode-grace \
		$GR_STR4 $DIR || error "set group grace time quota failed"
	$LFS quota -g -t $DIR | grep "Inode grace time: $GR_STR4"

	# test of invalid grace strings handling
	echo "  Invalid grace strings test"
	! $LFS setquota -t -u --block-grace $GR_STR4 --inode-grace $GR_STR5 $DIR
	! $LFS setquota -t -g --block-grace $GR_STR4 --inode-grace $GR_STR6 $DIR

	# cleanup
	$LFS setquota -t -u --block-grace $MAX_DQ_TIME --inode-grace \
		$MAX_IQ_TIME $DIR || error "restore user grace time failed"
	$LFS setquota -t -g --block-grace $MAX_DQ_TIME --inode-grace \
		$MAX_IQ_TIME $DIR || error "restore group grace time failed"
}
run_test 4b "Grace time strings handling"

# chown & chgrp (chown & chgrp successfully even out of block/file quota)
test_5() {
	local BLIMIT=10 # 10M
	local ILIMIT=10 # 10 inodes

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_mdt_qtype "ug" || error "enable mdt quota failed"
	set_ost_qtype "ug" || error "enable ost quota failed"

	echo "Set quota limit (0 ${BLIMIT}M 0 $ILIMIT) for $TSTUSR.$TSTUSR"
	$LFS setquota -u $TSTUSR -b 0 -B ${BLIMIT}M -i 0 -I $ILIMIT $DIR ||
		error "set user quota failed"
	$LFS setquota -g $TSTUSR -b 0 -B ${BLIMIT}M -i 0 -I $ILIMIT $DIR ||
		error "set group quota failed"

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curinodes)
	[ $USED -ne 0 ] && error "Used inode($USED) for user $TSTUSR isn't 0."
	USED=$(getquota -g $TSTUSR global curinodes)
	[ $USED -ne 0 ] && error "Used inode($USED) for group $TSTUSR isn't 0."
	USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used block($USED) for user $TSTUSR isn't 0."
	USED=$(getquota -g $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used block($USED) for group $TSTUSR isn't 0."

	echo "Create more than $ILIMIT files and more than $BLIMIT MB ..."
	createmany -m $DIR/$tdir/$tfile-0_ $((ILIMIT + 1)) ||
		error "create failure, expect success"
	$DD of=$DIR/$tdir/$tfile-0_1 count=$((BLIMIT+1)) ||
		error "write failure, expect success"

	echo "Chown files to $TSTUSR.$TSTUSR ..."
	for i in `seq 0 $ILIMIT`; do
		chown $TSTUSR.$TSTUSR $DIR/$tdir/$tfile-0_$i ||
			quota_error a $TSTUSR "chown failure, expect success"
	done

	# cleanup
	unlinkmany $DIR/$tdir/$tfile-0_ $((ILIMIT + 1))
	cleanup_quota_test

	resetquota -u $TSTUSR
	resetquota -g $TSTUSR
}
run_test 5 "Chown & chgrp successfully even out of block/file quota"

# test dropping acquire request on master
test_6() {
	local LIMIT=3 # 3M

	# Clear dmesg so watchdog is not triggered by previous
	# test output
	do_facet ost1 dmesg -c > /dev/null

	setup_quota_test
	trap cleanup_quota_test EXIT

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for user $TSTUSR isn't 0."

	# make sure no granted quota on ost
	set_ost_qtype "ug" || error "enable ost quota failed"
	resetquota -u $TSTUSR

	# create file for $TSTUSR
	local TESTFILE=$DIR/$tdir/$tfile-$TSTUSR
	$LFS setstripe $TESTFILE -c 1 -i 0
	chown $TSTUSR.$TSTUSR $TESTFILE

	# create file for $TSTUSR2
	local TESTFILE2=$DIR/$tdir/$tfile-$TSTUSR2
	$LFS setstripe $TESTFILE2 -c 1 -i 0
	chown $TSTUSR2.$TSTUSR2 $TESTFILE2

	# cache per-ID lock for $TSTUSR on slave
	$LFS setquota -u $TSTUSR -b 0 -B ${LIMIT}M -i 0 -I 0 $DIR ||
		error "set quota failed"
	$RUNAS $DD of=$TESTFILE count=1 ||
		error "write $TESTFILE failure, expect success"
	$RUNAS2 $DD of=$TESTFILE2 count=1 ||
		error "write $TESTFILE2 failure, expect success"
	sync; sync
	sync_all_data || true

	#define QUOTA_DQACQ 601
	#define OBD_FAIL_PTLRPC_DROP_REQ_OPC 0x513
	lustre_fail mds 0x513 601

	# write to un-enforced ID ($TSTUSR2) should succeed
	$RUNAS2 $DD of=$TESTFILE2 count=$LIMIT seek=1 oflag=sync conv=notrunc ||
		error "write failure, expect success"

	# write to enforced ID ($TSTUSR) in background, exceeding limit
	# to make sure DQACQ is sent
	$RUNAS $DD of=$TESTFILE count=$LIMIT seek=1 oflag=sync conv=notrunc &
	DDPID=$!

	# watchdog timer uses a factor of 2
	echo "Sleep for $((TIMEOUT * 2 + 1)) seconds ..."
	sleep $((TIMEOUT * 2 + 1))

	# write should be blocked and never finished
	if ! ps -p $DDPID  > /dev/null 2>&1; then
		lustre_fail mds 0 0
		error "write finished incorrectly!"
	fi

	lustre_fail mds 0 0

	# no watchdog is triggered
	do_facet ost1 dmesg > $TMP/lustre-log-${TESTNAME}.log
	watchdog=$(awk '/Service thread pid/ && /was inactive/ \
			{ print; }' $TMP/lustre-log-${TESTNAME}.log)
	[ -z "$watchdog" ] || error "$watchdog"

	rm -f $TMP/lustre-log-${TESTNAME}.log

	# write should continue then fail with EDQUOT
	local count=0
	local c_size
	while [ true ]; do
		if ! ps -p ${DDPID} > /dev/null 2>&1; then break; fi
		if [ $count -ge 240 ]; then
			quota_error u $TSTUSR "dd not finished in $count secs"
		fi
		count=$((count + 1))
		if [ $((count % 30)) -eq 0 ]; then
			c_size=$(stat -c %s $TESTFILE)
			echo "Waiting $count secs. $c_size"
			$SHOW_QUOTA_USER
		fi
		sleep 1
	done

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 6 "Test dropping acquire request on master"

# quota reintegration (global index)
test_7a() {
	local TESTFILE=$DIR/$tdir/$tfile
	local LIMIT=20 # 20M

	[ "$SLOW" = "no" ] && LIMIT=5

	setup_quota_test
	trap cleanup_quota_test EXIT

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for user $TSTUSR isn't 0."

	# make sure no granted quota on ost1
	set_ost_qtype "ug" || error "enable ost quota failed"
	resetquota -u $TSTUSR
	set_ost_qtype "none" || error "disable ost quota failed"

	local OSTUUID=$(ostuuid_from_index 0)
	USED=$(getquota -u $TSTUSR $OSTUUID bhardlimit)
	[ $USED -ne 0 ] && error "limit($USED) on $OSTUUID for user" \
		"$TSTUSR isn't 0."

	# create test file
	$LFS setstripe $TESTFILE -c 1 -i 0
	chown $TSTUSR.$TSTUSR $TESTFILE

	echo "Stop ost1..."
	stop ost1

	echo "Enable quota & set quota limit for $TSTUSR"
	set_ost_qtype "ug" || error "enable ost quota failed"
	$LFS setquota -u $TSTUSR -b 0 -B ${LIMIT}M -i 0 -I 0 $DIR ||
		error "set quota failed"

	echo "Start ost1..."
	start ost1 $(ostdevname 1) $OST_MOUNT_OPTS
	quota_init

	wait_ost_reint "ug" || error "reintegration failed"

	# hardlimit should have been fetched by slave during global
	# reintegration, write will exceed quota
	$RUNAS $DD of=$TESTFILE count=$((LIMIT + 1)) oflag=sync &&
		quota_error u $TSTUSR "write success, but expect EDQUOT"

	rm -f $TESTFILE
	wait_delete_completed
	sync_all_data || true
	sleep 3

	echo "Stop ost1..."
	stop ost1

	$LFS setquota -u $TSTUSR -b 0 -B 0 -i 0 -I 0 $DIR ||
		error "clear quota failed"

	echo "Start ost1..."
	start ost1 $(ostdevname 1) $OST_MOUNT_OPTS
	quota_init

	wait_ost_reint "ug" || error "reintegration failed"

	# hardlimit should be cleared on slave during reintegration
	$RUNAS $DD of=$TESTFILE count=$((LIMIT + 1)) oflag=sync ||
		quota_error u $TSTUSR "write error, but expect success"

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 7a "Quota reintegration (global index)"

# quota reintegration (slave index)
test_7b() {
	local LIMIT="100G"
	local TESTFILE=$DIR/$tdir/$tfile

	setup_quota_test
	trap cleanup_quota_test EXIT

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for user $TSTUSR isn't 0."

	# make sure no granted quota on ost1
	set_ost_qtype "ug" || error "enable ost quota failed"
	resetquota -u $TSTUSR
	set_ost_qtype "none" || error "disable ost quota failed"

	local OSTUUID=$(ostuuid_from_index 0)
	USED=$(getquota -u $TSTUSR $OSTUUID bhardlimit)
	[ $USED -ne 0 ] && error "limit($USED) on $OSTUUID for user" \
		"$TSTUSR isn't 0."

	# create test file
	$LFS setstripe $TESTFILE -c 1 -i 0
	chown $TSTUSR.$TSTUSR $TESTFILE

	# consume some space to make sure the granted space will not
	# be released during reconciliation
	$RUNAS $DD of=$TESTFILE count=1 oflag=sync ||
		error "consume space failure, expect success"

	# define OBD_FAIL_QUOTA_EDQUOT 0xa02
	lustre_fail mds 0xa02

	set_ost_qtype "ug" || error "enable ost quota failed"
	$LFS setquota -u $TSTUSR -b 0 -B $LIMIT -i 0 -I 0 $DIR ||
		error "set quota failed"

	# ignore the write error
	$RUNAS $DD of=$TESTFILE count=1 seek=1 oflag=sync conv=notrunc

	local old_used=$(getquota -u $TSTUSR $OSTUUID bhardlimit)

	lustre_fail mds 0

	echo "Restart ost to trigger reintegration..."
	stop ost1
	start ost1 $(ostdevname 1) $OST_MOUNT_OPTS
	quota_init

	wait_ost_reint "ug" || error "reintegration failed"

	USED=$(getquota -u $TSTUSR $OSTUUID bhardlimit)
	[ $USED -gt $old_used ] || error "limit on $OSTUUID $USED <= $old_used"

	cleanup_quota_test
	resetquota -u $TSTUSR
	$SHOW_QUOTA_USER
}
run_test 7b "Quota reintegration (slave index)"

# quota reintegration (restart mds during reintegration)
test_7c() {
	local LIMIT=20 # 20M
	local TESTFILE=$DIR/$tdir/$tfile

	[ "$SLOW" = "no" ] && LIMIT=5

	setup_quota_test
	trap cleanup_quota_test EXIT

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for user $TSTUSR isn't 0."

	set_ost_qtype "none" || error "disable ost quota failed"
	$LFS setquota -u $TSTUSR -b 0 -B ${LIMIT}M -i 0 -I 0 $DIR ||
		error "set quota failed"

	# define OBD_FAIL_QUOTA_DELAY_REINT 0xa03
	lustre_fail ost 0xa03

	# enable ost quota
	set_ost_qtype "ug" || error "enable ost quota failed"
	# trigger reintegration
	local procf="osd-$(facet_fstype ost1).$FSNAME-OST*."
	procf=${procf}quota_slave.force_reint
	do_facet ost1 $LCTL set_param $procf=1 ||
		error "force reintegration failed"

	echo "Stop mds..."
	stop mds1

	lustre_fail ost 0

	echo "Start mds..."
	start mds1 $(mdsdevname 1) $MDS_MOUNT_OPTS
	quota_init

	# wait longer than usual to make sure the reintegration
	# is triggered by quota wb thread.
	wait_ost_reint "ug" 200 || error "reintegration failed"

	# hardlimit should have been fetched by slave during global
	# reintegration, write will exceed quota
	$RUNAS $DD of=$TESTFILE count=$((LIMIT + 1)) oflag=sync &&
		quota_error u $TSTUSR "write success, but expect EDQUOT"

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 7c "Quota reintegration (restart mds during reintegration)"

# Quota reintegration (Transfer index in multiple bulks)
test_7d(){
	local TESTFILE=$DIR/$tdir/$tfile
	local TESTFILE1="$DIR/$tdir/$tfile"-1
	local limit=20 #20M

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "none" || error "disable ost quota failed"
	$LFS setquota -u $TSTUSR -B ${limit}M $DIR ||
		error "set quota for $TSTUSR failed"
	$LFS setquota -u $TSTUSR2 -B ${limit}M $DIR ||
		error "set quota for $TSTUSR2 failed"

	#define OBD_FAIL_OBD_IDX_READ_BREAK 0x608
	lustre_fail mds 0x608 0

	# enable quota to tirgger reintegration
	set_ost_qtype "u" || error "enable ost quota failed"
	wait_ost_reint "u" || error "reintegration failed"

	lustre_fail mds 0

	# hardlimit should have been fetched by slave during global
	# reintegration, write will exceed quota
	$RUNAS $DD of=$TESTFILE count=$((limit + 1)) oflag=sync &&
		quota_error u $TSTUSR "$TSTUSR write success, expect EDQUOT"

	$RUNAS2 $DD of=$TESTFILE1 count=$((limit + 1)) oflag=sync &&
		quota_error u $TSTUSR2 "$TSTUSR2 write success, expect EDQUOT"

	cleanup_quota_test
	resetquota -u $TSTUSR
	resetquota -u $TSTUSR2
}
run_test 7d "Quota reintegration (Transfer index in multiple bulks)"

# quota reintegration (inode limits)
test_7e() {
	[ "$MDSCOUNT" -lt "2" ] && skip "Required more MDTs" && return

	local ilimit=$((1024 * 2)) # 2k inodes
	local TESTFILE=$DIR/${tdir}-1/$tfile

	setup_quota_test
	trap cleanup_quota_test EXIT

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curinodes)
	[ $USED -ne 0 ] && error "Used inode($USED) for user $TSTUSR isn't 0."

	# make sure no granted quota on mdt1
	set_mdt_qtype "ug" || error "enable mdt quota failed"
	resetquota -u $TSTUSR
	set_mdt_qtype "none" || error "disable mdt quota failed"

	local MDTUUID=$(mdtuuid_from_index $((MDSCOUNT - 1)))
	USED=$(getquota -u $TSTUSR $MDTUUID ihardlimit)
	[ $USED -ne 0 ] && error "limit($USED) on $MDTUUID for user" \
		"$TSTUSR isn't 0."

	echo "Stop mds${MDSCOUNT}..."
	stop mds${MDSCOUNT}

	echo "Enable quota & set quota limit for $TSTUSR"
	set_mdt_qtype "ug" || error "enable mdt quota failed"
	$LFS setquota -u $TSTUSR -b 0 -B 0 -i 0 -I $ilimit  $DIR ||
		error "set quota failed"

	echo "Start mds${MDSCOUNT}..."
	start mds${MDSCOUNT} $(mdsdevname $MDSCOUNT) $MDS_MOUNT_OPTS
	quota_init

	wait_mdt_reint "ug" || error "reintegration failed"

	echo "create remote dir"
	$LFS mkdir -i $((MDSCOUNT - 1)) $DIR/${tdir}-1 ||
		error "create remote dir failed"
	chmod 0777 $DIR/${tdir}-1

	# hardlimit should have been fetched by slave during global
	# reintegration, create will exceed quota
	$RUNAS createmany -m $TESTFILE $((ilimit + 1)) &&
		quota_error u $TSTUSR "create succeeded, expect EDQUOT"

	$RUNAS unlinkmany $TESTFILE $ilimit || "unlink files failed"
	wait_delete_completed
	sync_all_data || true

	echo "Stop mds${MDSCOUNT}..."
	stop mds${MDSCOUNT}

	$LFS setquota -u $TSTUSR -b 0 -B 0 -i 0 -I 0 $DIR ||
		error "clear quota failed"

	echo "Start mds${MDSCOUNT}..."
	start mds${MDSCOUNT} $(mdsdevname $MDSCOUNT) $MDS_MOUNT_OPTS
	quota_init

	wait_mdt_reint "ug" || error "reintegration failed"

	# hardlimit should be cleared on slave during reintegration
	$RUNAS createmany -m $TESTFILE $((ilimit + 1)) ||
		quota_error -u $TSTUSR "create failed, expect success"

	$RUNAS unlinkmany $TESTFILE $((ilimit + 1)) || "unlink failed"
	rmdir $DIR/${tdir}-1 || "unlink remote dir failed"

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 7e "Quota reintegration (inode limits)"

# run dbench with quota enabled
test_8() {
	local BLK_LIMIT="100g" #100G
	local FILE_LIMIT=1000000

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_mdt_qtype "ug" || error "enable mdt quota failed"
	set_ost_qtype "ug" || error "enable ost quota failed"

	echo "Set enough high limit for user: $TSTUSR"
	$LFS setquota -u $TSTUSR -b 0 -B $BLK_LIMIT -i 0 -I $FILE_LIMIT $DIR ||
		error "set user quota failed"
	echo "Set enough high limit for group: $TSTUSR"
	$LFS setquota -g $TSTUSR -b 0 -B $BLK_LIMIT -i 0 -I $FILE_LIMIT $DIR ||
		error "set group quota failed"

	local duration=""
	[ "$SLOW" = "no" ] && duration=" -t 120"
	$RUNAS bash rundbench -D $DIR/$tdir 3 $duration ||
		quota_error a $TSTUSR "dbench failed!"

	cleanup_quota_test
	resetquota -u $TSTUSR
	resetquota -g $TSTUSR
}
run_test 8 "Run dbench with quota enabled"

# this check is just for test_9
OST0_MIN=4900000 #4.67G

check_whether_skip () {
	local OST0_SIZE=$($LFS df $DIR | awk '/\[OST:0\]/ {print $4}')
	log "OST0_SIZE: $OST0_SIZE  required: $OST0_MIN"
	if [ $OST0_SIZE -lt $OST0_MIN ]; then
		echo "WARN: OST0 has less than $OST0_MIN free, skip this test."
		return 0
	else
		return 1
	fi
}

# run for fixing bug10707, it needs a big room. test for 64bit
test_9() {
	local filesize=$((1024 * 9 / 2)) # 4.5G

	check_whether_skip && return 0

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "ug" || error "enable ost quota failed"

	local TESTFILE="$DIR/$tdir/$tfile-0"
	local BLK_LIMIT=100G #100G
	local FILE_LIMIT=1000000

	echo "Set block limit $BLK_LIMIT bytes to $TSTUSR.$TSTUSR"

	log "Set enough high limit(block:$BLK_LIMIT; file: $FILE_LIMIT)" \
		"for user: $TSTUSR"
	$LFS setquota -u $TSTUSR -b 0 -B $BLK_LIMIT -i 0 -I $FILE_LIMIT $DIR ||
		error "set user quota failed"

	log "Set enough high limit(block:$BLK_LIMIT; file: $FILE_LIMIT)" \
		"for group: $TSTUSR"
	$LFS setquota -g $TSTUSR -b 0 -B $BLK_LIMIT -i 0 -I $FILE_LIMIT $DIR ||
		error "set group quota failed"

	quota_show_check a u $TSTUSR
	quota_show_check a g $TSTUSR

	echo "Create test file"
	$LFS setstripe $TESTFILE -c 1 -i 0
	chown $TSTUSR.$TSTUSR $TESTFILE

	log "Write the big file of 4.5G ..."
	$RUNAS $DD of=$TESTFILE count=$filesize ||
		quota_error a $TSTUSR "write 4.5G file failure, expect success"

	$SHOW_QUOTA_USER
	$SHOW_QUOTA_GROUP

	cleanup_quota_test
	resetquota -u $TSTUSR
	resetquota -g $TSTUSR

	$SHOW_QUOTA_USER
	$SHOW_QUOTA_GROUP
}
run_test 9 "Block limit larger than 4GB (b10707)"

test_10() {
	local TESTFILE=$DIR/$tdir/$tfile

	setup_quota_test
	trap cleanup_quota_test EXIT

	# set limit to root user should fail
	$LFS setquota -u root -b 100G -B 500G -i 1K -I 1M $DIR &&
		error "set limit for root user successfully, expect failure"
	$LFS setquota -g root -b 1T -B 10T -i 5K -I 100M $DIR &&
		error "set limit for root group successfully, expect failure"

	# root user can overrun quota
	set_ost_qtype "ug" || "enable ost quota failed"

	$LFS setquota -u $TSTUSR -b 0 -B 2M -i 0 -I 0 $DIR ||
		error "set quota failed"
	quota_show_check b u $TSTUSR

	$LFS setstripe $TESTFILE -c 1
	chown $TSTUSR.$TSTUSR $TESTFILE

	runas -u 0 -g 0 $DD of=$TESTFILE count=3 oflag=sync ||
		error "write failure, expect success"

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 10 "Test quota for root user"

test_11() {
	local TESTFILE=$DIR/$tdir/$tfile
	setup_quota_test
	trap cleanup_quota_test EXIT

	set_mdt_qtype "ug" || "enable mdt quota failed"
	$LFS setquota -u $TSTUSR -b 0 -B 0 -i 0 -I 1 $DIR ||
		error "set quota failed"

	touch "$TESTFILE"-0
	touch "$TESTFILE"-1

	chown $TSTUSR.$TSTUSR "$TESTFILE"-0
	chown $TSTUSR.$TSTUSR "$TESTFILE"-1

	$SHOW_QUOTA_USER
	local USED=$(getquota -u $TSTUSR global curinodes)
	[ $USED -ge 2 ] || error "Used inodes($USED) is less than 2"

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 11 "Chown/chgrp ignores quota"

test_12a() {
	[ "$OSTCOUNT" -lt "2" ] && skip "skipping rebalancing test" && return

	local blimit=22 # 22M
	local blk_cnt=$((blimit - 5))
	local TESTFILE0="$DIR/$tdir/$tfile"-0
	local TESTFILE1="$DIR/$tdir/$tfile"-1

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "u" || "enable ost quota failed"
	quota_show_check b u $TSTUSR

	$LFS setquota -u $TSTUSR -b 0 -B "$blimit"M -i 0 -I 0 $DIR ||
		error "set quota failed"

	$LFS setstripe $TESTFILE0 -c 1 -i 0
	$LFS setstripe $TESTFILE1 -c 1 -i 1
	chown $TSTUSR.$TSTUSR $TESTFILE0
	chown $TSTUSR.$TSTUSR $TESTFILE1

	echo "Write to ost0..."
	$RUNAS $DD of=$TESTFILE0 count=$blk_cnt oflag=sync ||
		quota_error a $TSTUSR "dd failed"

	echo "Write to ost1..."
	$RUNAS $DD of=$TESTFILE1 count=$blk_cnt oflag=sync &&
		quota_error a $TSTUSR "dd succeed, expect EDQUOT"

	echo "Free space from ost0..."
	rm -f $TESTFILE0
	wait_delete_completed
	sync_all_data || true

	echo "Write to ost1 after space freed from ost0..."
	$RUNAS $DD of=$TESTFILE1 count=$blk_cnt oflag=sync ||
		quota_error a $TSTUSR "rebalancing failed"

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 12a "Block quota rebalancing"

test_12b() {
	[ "$MDSCOUNT" -lt "2" ] && skip "skipping rebalancing test" && return

	local ilimit=$((1024 * 2)) # 2k inodes
	local TESTFILE0=$DIR/$tdir/$tfile
	local TESTFILE1=$DIR/${tdir}-1/$tfile

	setup_quota_test
	trap cleanup_quota_test EXIT

	$LFS mkdir -i 1 $DIR/${tdir}-1 || error "create remote dir failed"
	chmod 0777 $DIR/${tdir}-1

	set_mdt_qtype "u" || "enable mdt quota failed"
	quota_show_check f u $TSTUSR

	$LFS setquota -u $TSTUSR -b 0 -B 0 -i 0 -I $ilimit $DIR ||
		error "set quota failed"

	echo "Create $ilimit files on mdt0..."
	$RUNAS createmany -m $TESTFILE0 $ilimit ||
		quota_error u $TSTUSR "create failed, but expect success"

	echo "Create files on mdt1..."
	$RUNAS createmany -m $TESTFILE1 1 &&
		quota_error a $TSTUSR "create succeeded, expect EDQUOT"

	echo "Free space from mdt0..."
	$RUNAS unlinkmany $TESTFILE0 $ilimit || error "unlink mdt0 files failed"
	wait_delete_completed
	sync_all_data || true

	echo "Create files on mdt1 after space freed from mdt0..."
	$RUNAS createmany -m $TESTFILE1 $((ilimit / 2)) ||
		quota_error a $TSTUSR "rebalancing failed"

	$RUNAS unlinkmany $TESTFILE1 $((ilimit / 2)) ||
		error "unlink mdt1 files failed"
	rmdir $DIR/${tdir}-1 || error "unlink remote dir failed"

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 12b "Inode quota rebalancing"

test_13(){
	local TESTFILE=$DIR/$tdir/$tfile
	# the name of lwp on ost1 name is MDT0000-lwp-OST0000
	local procf="ldlm.namespaces.*MDT0000-lwp-OST0000.lru_size"

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "u" || "enable ost quota failed"
	quota_show_check b u $TSTUSR

	$LFS setquota -u $TSTUSR -b 0 -B 10M -i 0 -I 0 $DIR ||
		error "set quota failed"
	$LFS setstripe $TESTFILE -c 1 -i 0
	chown $TSTUSR.$TSTUSR $TESTFILE

	# clear the locks in cache first
	do_facet ost1 $LCTL set_param -n $procf=clear
	local nlock=$(do_facet ost1 $LCTL get_param -n $procf)
	[ $nlock -eq 0 ] || error "$nlock cached locks"

	# write to acquire the per-ID lock
	$RUNAS $DD of=$TESTFILE count=1 oflag=sync ||
		quota_error a $TSTUSR "dd failed"

	nlock=$(do_facet ost1 $LCTL get_param -n $procf)
	[ $nlock -eq 1 ] || error "lock count($nlock) isn't 1"

	# clear quota doesn't trigger per-ID lock cancellation
	resetquota -u $TSTUSR
	nlock=$(do_facet ost1 $LCTL get_param -n $procf)
	[ $nlock -eq 1 ] || error "per-ID lock is lost on quota clear"

	# clear the per-ID lock
	do_facet ost1 $LCTL set_param -n $procf=clear
	nlock=$(do_facet ost1 $LCTL get_param -n $procf)
	[ $nlock -eq 0 ] || error "per-ID lock isn't cleared"

	# spare quota should be released
	local OSTUUID=$(ostuuid_from_index 0)
	local limit=$(getquota -u $TSTUSR $OSTUUID bhardlimit)
	local space=$(getquota -u $TSTUSR $OSTUUID curspace)
	[ $limit -le $space ] ||
		error "spare quota isn't released, limit:$limit, space:$space"

	cleanup_quota_test
}
run_test 13 "Cancel per-ID lock in the LRU list"

test_15(){
	local LIMIT=$((24 * 1024 * 1024 * 1024 * 1024)) # 24 TB

	wait_delete_completed
	sync_all_data || true

        # test for user
	$LFS setquota -u $TSTUSR -b 0 -B $LIMIT -i 0 -I 0 $DIR ||
		error "set user quota failed"
	local TOTAL_LIMIT=$(getquota -u $TSTUSR global bhardlimit)
	[ $TOTAL_LIMIT -eq $LIMIT ] ||
		error "(user) limit:$TOTAL_LIMIT, expect:$LIMIT, failed!"
	resetquota -u $TSTUSR

	# test for group
	$LFS setquota -g $TSTUSR -b 0 -B $LIMIT -i 0 -I 0 $DIR ||
		error "set group quota failed"
	TOTAL_LIMIT=$(getquota -g $TSTUSR global bhardlimit)
	[ $TOTAL_LIMIT -eq $LIMIT ] ||
		error "(group) limits:$TOTAL_LIMIT, expect:$LIMIT, failed!"
	resetquota -g $TSTUSR
}
run_test 15 "Set over 4T block quota"

test_17sub() {
	local err_code=$1
	local BLKS=1    # 1M less than limit
	local TESTFILE=$DIR/$tdir/$tfile

	setup_quota_test
	trap cleanup_quota_test EXIT

	# make sure the system is clean
	local USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ne 0 ] && error "Used space($USED) for user $TSTUSR isn't 0."

	set_ost_qtype "ug" || error "enable ost quota failed"
	# make sure no granted quota on ost
	resetquota -u $TSTUSR
	$LFS setquota -u $TSTUSR -b 0 -B 10M -i 0 -I 0 $DIR ||
		error "set quota failed"

	quota_show_check b u $TSTUSR

	#define OBD_FAIL_QUOTA_RECOVERABLE_ERR 0xa04
	lustre_fail mds 0xa04 $err_code

	# write in background
	$RUNAS $DD of=$TESTFILE count=$BLKS oflag=direct &
	local DDPID=$!

	sleep 2
	# write should be blocked and never finished
	if ! ps -p $DDPID  > /dev/null 2>&1; then
		lustre_fail mds 0 0
		quota_error u $TSTUSR "write finished incorrectly!"
	fi

	lustre_fail mds 0 0

	local count=0
	local timeout=30
	while [ true ]; do
		if ! ps -p ${DDPID} > /dev/null 2>&1; then break; fi
		count=$((count+1))
		if [ $count -gt $timeout ]; then
			quota_error u $TSTUSR "dd is not finished!"
		fi
		sleep 1
	done

	sync; sync_all_data || true

	USED=$(getquota -u $TSTUSR global curspace)
	[ $USED -ge $(($BLKS * 1024)) ] || quota_error u $TSTUSR \
		"Used space(${USED}K) is less than ${BLKS}M"

	cleanup_quota_test
	resetquota -u $TSTUSR
}

# DQACQ return recoverable error
test_17() {
	echo "DQACQ return -ENOLCK"
	#define ENOLCK  37
	test_17sub 37 || error "Handle -ENOLCK failed"

	echo "DQACQ return -EAGAIN"
	#define EAGAIN  11
	test_17sub 11 || error "Handle -EAGAIN failed"

	echo "DQACQ return -ETIMEDOUT"
	#define ETIMEDOUT 110
	test_17sub 110 || error "Handle -ETIMEDOUT failed"

	echo "DQACQ return -ENOTCONN"
	#define ENOTCONN 107
	test_17sub 107 || error "Handle -ENOTCONN failed"
}

run_test 17 "DQACQ return recoverable error"

test_18_sub () {
	local io_type=$1
	local blimit="200m" # 200M
	local TESTFILE="$DIR/$tdir/$tfile"

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "u" || error "enable ost quota failed"
	log "User quota (limit: $blimit)"
	$LFS setquota -u $TSTUSR -b 0 -B $blimit -i 0 -I 0 $MOUNT ||
		error "set quota failed"
	quota_show_check b u $TSTUSR

	$LFS setstripe $TESTFILE -i 0 -c 1
	chown $TSTUSR.$TSTUSR $TESTFILE

	local timeout=$(sysctl -n lustre.timeout)

	if [ $io_type = "directio" ]; then
		log "Write 100M (directio) ..."
		$RUNAS $DD of=$TESTFILE count=100 oflag=direct &
	else
		log "Write 100M (buffered) ..."
		$RUNAS $DD of=$TESTFILE count=100 &
	fi
	local DDPID=$!

	replay_barrier $SINGLEMDS
	log "Fail mds for $((2 * timeout)) seconds"
	fail $SINGLEMDS $((2 * timeout))

	local count=0
	if at_is_enabled; then
		timeout=$(at_max_get mds)
	else
		timeout=$(lctl get_param -n timeout)
	fi

	while [ true ]; do
		if ! ps -p ${DDPID} > /dev/null 2>&1; then break; fi
		if [ $((++count % (2 * timeout) )) -eq 0 ]; then
			log "it took $count second"
		fi
		sleep 1
	done

	log "(dd_pid=$DDPID, time=$count, timeout=$timeout)"
	sync
	cancel_lru_locks mdc
	cancel_lru_locks osc
	$SHOW_QUOTA_USER

	local testfile_size=$(stat -c %s $TESTFILE)
	if [ $testfile_size -ne $((BLK_SZ * 1024 * 100)) ] ; then
		quota_error u $TSTUSR "expect $((BLK_SZ * 1024 * 100))," \
			"got ${testfile_size}. Verifying file failed!"
	fi
	cleanup_quota_test
	resetquota -u $TSTUSR
}

# test when mds does failover, the ost still could work well
# this test shouldn't trigger watchdog b=14840
test_18() {
	# Clear dmesg so watchdog is not triggered by previous
	# test output
	do_facet ost1 dmesg -c > /dev/null

	test_18_sub normal
	test_18_sub directio

	# check if watchdog is triggered
	do_facet ost1 dmesg > $TMP/lustre-log-${TESTNAME}.log
	local watchdog=$(awk '/Service thread pid/ && /was inactive/ \
			{ print; }' $TMP/lustre-log-${TESTNAME}.log)
	[ -z "$watchdog" ] || error "$watchdog"
	rm -f $TMP/lustre-log-${TESTNAME}.log
}
run_test 18 "MDS failover while writing, no watchdog triggered (b14840)"

test_19() {
	local blimit=5 # 5M
	local TESTFILE=$DIR/$tdir/$tfile

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "ug" || error "enable ost quota failed"

	# bind file to a single OST
	$LFS setstripe -c 1 $TESTFILE
	chown $TSTUSR.$TSTUSR $TESTFILE

	echo "Set user quota (limit: "$blimit"M)"
	$LFS setquota -u $TSTUSR -b 0 -B "$blimit"M -i 0 -I 0 $MOUNT ||
		error "set user quota failed"
	quota_show_check b u $TSTUSR
	echo "Update quota limits"
	$LFS setquota -u $TSTUSR -b 0 -B "$blimit"M -i 0 -I 0 $MOUNT ||
		error "set group quota failed"
	quota_show_check b u $TSTUSR

	# first wirte might be cached
	$RUNAS $DD of=$TESTFILE count=$((blimit + 1))
	cancel_lru_locks osc
	$SHOW_QUOTA_USER
	$RUNAS $DD of=$TESTFILE count=$((blimit + 1)) seek=$((blimit + 1)) &&
		quota_error u $TSTUSR "Write success, expect failure"
	$SHOW_QUOTA_USER

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 19 "Updating admin limits doesn't zero operational limits(b14790)"

test_20() { # b15754
	local LSTR=(2g 1t 4k 3m) # limits strings
	# limits values
	local LVAL=($[2*1024*1024] $[1*1024*1024*1024] $[4*1024] $[3*1024*1024])

	resetquota -u $TSTUSR

	$LFS setquota -u $TSTUSR --block-softlimit ${LSTR[0]} \
		$MOUNT || error "could not set quota limits"
	$LFS setquota -u $TSTUSR --block-hardlimit ${LSTR[1]} \
				--inode-softlimit ${LSTR[2]} \
				--inode-hardlimit ${LSTR[3]} \
				$MOUNT || error "could not set quota limits"

	[ "$(getquota -u $TSTUSR global bsoftlimit)" = "${LVAL[0]}" ] ||
		error "bsoftlimit was not set properly"
	[ "$(getquota -u $TSTUSR global bhardlimit)" = "${LVAL[1]}" ] ||
		error "bhardlimit was not set properly"
	[ "$(getquota -u $TSTUSR global isoftlimit)" = "${LVAL[2]}" ] ||
		error "isoftlimit was not set properly"
	[ "$(getquota -u $TSTUSR global ihardlimit)" = "${LVAL[3]}" ] ||
		error "ihardlimit was not set properly"

	resetquota -u $TSTUSR
}
run_test 20 "Test if setquota specifiers work properly (b15754)"

test_21_sub() {
	local testfile=$1
	local blk_number=$2
	local seconds=$3

	local time=$(($(date +%s) + seconds))
	while [ $(date +%s) -lt $time ]; do
		$RUNAS $DD of=$testfile count=$blk_number > /dev/null 2>&1
	done
}

# run for fixing bug16053, setquota shouldn't fail when writing and
# deleting are happening
test_21() {
	local TESTFILE="$DIR/$tdir/$tfile"
	local BLIMIT=10 # 10G
	local ILIMIT=1000000

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "ug" || error "Enable ost quota failed"

	log "Set limit(block:${BLIMIT}G; file:$ILIMIT) for user: $TSTUSR"
	$LFS setquota -u $TSTUSR -b 0 -B ${BLIMIT}G -i 0 -I $ILIMIT $MOUNT ||
		error "set user quota failed"
	log "Set limit(block:${BLIMIT}G; file:$ILIMIT) for group: $TSTUSR"
	$LFS setquota -g $TSTUSR -b 0 -B $BLIMIT -i 0 -I $ILIMIT $MOUNT ||
		error "set group quota failed"

	# repeat writing on a 1M file
	test_21_sub ${TESTFILE}_1 1 30 &
	local DDPID1=$!
	# repeat writing on a 128M file
	test_21_sub ${TESTFILE}_2 128 30 &
	local DDPID2=$!

	local time=$(($(date +%s) + 30))
	local i=1
	while [ $(date +%s) -lt $time ]; do
		log "Set quota for $i times"
		$LFS setquota -u $TSTUSR -b 0 -B "$((BLIMIT + i))G" -i 0 \
			-I $((ILIMIT + i)) $MOUNT ||
				error "Set user quota failed"
		$LFS setquota -g $TSTUSR -b 0 -B "$((BLIMIT + i))G" -i 0 \
			-I $((ILIMIT + i)) $MOUNT ||
				error "Set group quota failed"
		i=$((i+1))
		sleep 1
	done

	local count=0
	while [ true ]; do
		if ! ps -p ${DDPID1} > /dev/null 2>&1; then break; fi
		count=$((count+1))
		if [ $count -gt 60 ]; then
			quota_error a $TSTUSR "dd should be finished!"
		fi
		sleep 1
	done
	echo "(dd_pid=$DDPID1, time=$count)successful"

	count=0
	while [ true ]; do
		if ! ps -p ${DDPID2} > /dev/null 2>&1; then break; fi
		count=$((count+1))
		if [ $count -gt 60 ]; then
			quota_error a $TSTUSR "dd should be finished!"
		fi
		sleep 1
	done
	echo "(dd_pid=$DDPID2, time=$count)successful"

	cleanup_quota_test
	resetquota -u $TSTUSR
	resetquota -g $TSTUSR
}
run_test 21 "Setquota while writing & deleting (b16053)"

# enable/disable quota enforcement permanently
test_22() {
	echo "Set both mdt & ost quota type as ug"
	set_mdt_qtype "ug" || error "enable mdt quota failed"
	set_ost_qtype "ug" || error "enable ost quota failed"

	echo "Restart..."
	stopall
	mount
	setupall

	echo "Verify if quota is enabled"
	local qtype=$(mdt_quota_type)
	[ $qtype != "ug" ] && error "mdt quota setting is lost"
	qtype=$(ost_quota_type)
	[ $qtype != "ug" ] && error "ost quota setting is lost"

	echo "Set both mdt & ost quota type as none"
	set_mdt_qtype "none" || error "disable mdt quota failed"
	set_ost_qtype "none" || error "disable ost quota failed"

	echo "Restart..."
	stopall
	mount
	setupall
	quota_init

	echo "Verify if quota is disabled"
	qtype=$(mdt_quota_type)
	[ $qtype != "none" ] && error "mdt quota setting is lost"
	qtype=$(ost_quota_type)
	[ $qtype != "none" ] && error "ost quota setting is lost"

	return 0
}
run_test 22 "enable/disable quota by 'lctl conf_param'"

test_23_sub() {
	local TESTFILE="$DIR/$tdir/$tfile"
	local LIMIT=$1

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "ug" || error "Enable ost quota failed"

	# test for user
	log "User quota (limit: $LIMIT MB)"
	$LFS setquota -u $TSTUSR -b 0 -B "$LIMIT"M -i 0 -I 0 $DIR ||
		error "set quota failed"
	quota_show_check b u $TSTUSR

	$LFS setstripe $TESTFILE -c 1 -i 0
	chown $TSTUSR.$TSTUSR $TESTFILE

	log "Step1: trigger EDQUOT with O_DIRECT"
	log "Write half of file"
	$RUNAS $DD of=$TESTFILE count=$((LIMIT/2)) oflag=direct ||
		quota_error u $TSTUSR "(1) Write failure, expect success." \
			"limit=$LIMIT"
	log "Write out of block quota ..."
	$RUNAS $DD of=$TESTFILE count=$((LIMIT/2 + 1)) seek=$((LIMIT/2)) \
		oflag=direct conv=notrunc &&
		quota_error u $TSTUSR "(2) Write success, expect EDQUOT." \
			"limit=$LIMIT"
	log "Step1: done"

	log "Step2: rewrite should succeed"
	$RUNAS $DD of=$TESTFILE count=1 oflag=direct conv=notrunc||
		quota_error u $TSTUSR "(3) Write failure, expect success." \
			"limit=$LIMIT"
	log "Step2: done"

	cleanup_quota_test

	local OST0_UUID=$(ostuuid_from_index 0)
	local OST0_QUOTA_USED=$(getquota -u $TSTUSR $OST0_UUID curspace)
	[ $OST0_QUOTA_USED -ne 0 ] &&
		($SHOW_QUOTA_USER; \
		quota_error u $TSTUSR "quota isn't released")
	$SHOW_QUOTA_USER
	resetquota -u $TSTUSR
}

test_23() {
	local OST0_MIN=$((6 * 1024)) # 6MB, extra space for meta blocks.
	check_whether_skip && return 0
	log "run for 4MB test file"
	test_23_sub 4

	OST0_MIN=$((60 * 1024)) # 60MB, extra space for meta blocks.
	check_whether_skip && return 0
	log "run for 40MB test file"
	test_23_sub 40
}
run_test 23 "Quota should be honored with directIO (b16125)"

test_24() {
	local blimit=5 # 5M
	local TESTFILE="$DIR/$tdir/$tfile"

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "ug" || error "enable ost quota failed"

	# bind file to a single OST
	$LFS setstripe -c 1 $TESTFILE
	chown $TSTUSR.$TSTUSR $TESTFILE

	echo "Set user quota (limit: "$blimit"M)"
	$LFS setquota -u $TSTUSR -b 0 -B "$blimit"M -i 0 -I 0 $MOUNT ||
		error "set quota failed"

	# overrun quota by root user
	runas -u 0 -g 0 $DD of=$TESTFILE count=$((blimit + 1)) ||
		error "write failure, expect success"
	cancel_lru_locks osc
	sync_all_data || true

	$SHOW_QUOTA_USER | grep '*' || error "no matching *"

	cleanup_quota_test
	resetquota -u $TSTUSR
}
run_test 24 "lfs draws an asterix when limit is reached (b16646)"

test_27a() { # b19612
	$LFS quota $TSTUSR $DIR &&
		error "lfs succeeded with no type, but should have failed"
	$LFS setquota $TSTUSR $DIR &&
		error "lfs succeeded with no type, but should have failed"
	return 0
}
run_test 27a "lfs quota/setquota should handle wrong arguments (b19612)"

test_27b() { # b20200
	$LFS setquota -u $TSTID -b 1000 -B 1000 -i 1000 -I 1000 $DIR ||
		error "lfs setquota failed with uid argument"
	$LFS setquota -g $TSTID -b 1000 -B 1000 -i 1000 -I 1000 $DIR ||
		error "lfs stequota failed with gid argument"
	$SHOW_QUOTA_USERID || error "lfs quota failed with uid argument"
	$SHOW_QUOTA_GROUPID || error "lfs quota failed with gid argument"
	resetquota -u $TSTUSR
	resetquota -g $TSTUSR
	return 0
}
run_test 27b "lfs quota/setquota should handle user/group ID (b20200)"

test_27c() {
	local limit

	$LFS setquota -u $TSTID -b 30M -B 3T $DIR ||
		error "lfs setquota failed"

	limit=$($LFS quota -u $TSTID -v -h $DIR | grep $DIR | awk '{print $3}')
	[ $limit != "30M" ] && error "softlimit $limit isn't human-readable"
	limit=$($LFS quota -u $TSTID -v -h $DIR | grep $DIR | awk '{print $4}')
	[ $limit != "3T" ] && error "hardlimit $limit isn't human-readable"

	$LFS setquota -u $TSTID -b 1500M -B 18500G $DIR ||
		error "lfs setquota for $TSTID failed"

	limit=$($LFS quota -u $TSTID -v -h $DIR | grep $DIR | awk '{print $3}')
	[ $limit != "1.465G" ] && error "wrong softlimit $limit"
	limit=$($LFS quota -u $TSTID -v -h $DIR | grep $DIR | awk '{print $4}')
	[ $limit != "18.07T" ] && error "wrong hardlimit $limit"

	$LFS quota -u $TSTID -v -h $DIR | grep -q "Total allocated" ||
		error "total allocated inode/block limit not printed"

	resetquota -u $TSTUSR
}
run_test 27c "lfs quota should support human-readable output"

test_30() {
	local output
	local LIMIT=4 # 4MB
	local TESTFILE="$DIR/$tdir/$tfile"
	local GRACE=10

	setup_quota_test
	trap cleanup_quota_test EXIT

	set_ost_qtype "u" || error "enable ost quota failed"

	$LFS setstripe $TESTFILE -i 0 -c 1
	chown $TSTUSR.$TSTUSR $TESTFILE

	$LFS setquota -t -u --block-grace $GRACE --inode-grace \
		$MAX_IQ_TIME $DIR || error "set grace time failed"
	$LFS setquota -u $TSTUSR -b ${LIMIT}M -B 0 -i 0 -I 0 $DIR ||
		error "set quota failed"
	$RUNAS $DD of=$TESTFILE count=$((LIMIT * 2)) || true
	cancel_lru_locks osc
	sleep $GRACE
	$LFS setquota -u $TSTUSR -B 0 $DIR || error "clear quota failed"
	# over-quota flag has not yet settled since we do not trigger async
	# events based on grace time period expiration
	$SHOW_QUOTA_USER
	$RUNAS $DD of=$TESTFILE conv=notrunc oflag=append count=4 || true
	cancel_lru_locks osc
	# now over-quota flag should be settled and further writes should fail
	$SHOW_QUOTA_USER
	$RUNAS $DD of=$TESTFILE conv=notrunc oflag=append count=4 &&
		error "grace times were reset"
	# cleanup
	cleanup_quota_test
	resetquota -u $TSTUSR
	$LFS setquota -t -u --block-grace $MAX_DQ_TIME --inode-grace \
		$MAX_IQ_TIME $DIR || error "restore grace time failed"
}
run_test 30 "Hard limit updates should not reset grace times"

# basic usage tracking for user & group
test_33() {
	local INODES=10 # 10 files
	local BLK_CNT=2 # of 2M each
	local TOTAL_BLKS=$((INODES * BLK_CNT * 1024))

	setup_quota_test
	trap cleanup_quota_test EXIT

	# make sure the system is clean
	local USED=$(getquota -u $TSTID global curspace)
	[ $USED -ne 0 ] &&
		error "Used space ($USED) for user $TSTID isn't 0."
	USED=$(getquota -g $TSTID global curspace)
	[ $USED -ne 0 ] &&
		error "Used space ($USED) for group $TSTID isn't 0."

	echo "Write files..."
	for i in `seq 0 $INODES`; do
		$RUNAS $DD of=$DIR/$tdir/$tfile-$i count=$BLK_CNT 2>/dev/null ||
			error "write failed"
		echo "Iteration $i/$INODES completed"
	done
	cancel_lru_locks osc
	sync; sync_all_data || true

	echo "Verify disk usage after write"
	USED=$(getquota -u $TSTID global curspace)
	[ $USED -lt $TOTAL_BLKS ] &&
		error "Used space for user $TSTID:$USED, expected:$TOTAL_BLKS"
	USED=$(getquota -g $TSTID global curspace)
	[ $USED -lt $TOTAL_BLKS ] &&
		error "Used space for group $TSTID:$USED, expected:$TOTAL_BLKS"

	echo "Verify inode usage after write"
	USED=$(getquota -u $TSTID global curinodes)
	[ $USED -lt $INODES ] &&
		error "Used inode for user $TSTID is $USED, expected $INODES"
	USED=$(getquota -g $TSTID global curinodes)
	[ $USED -lt $INODES ] &&
		error "Used inode for group $TSTID is $USED, expected $INODES"

	cleanup_quota_test

	echo "Verify disk usage after delete"
	USED=$(getquota -u $TSTID global curspace)
	[ $USED -eq 0 ] || error "Used space for user $TSTID isn't 0. $USED"
	USED=$(getquota -u $TSTID global curinodes)
	[ $USED -eq 0 ] || error "Used inodes for user $TSTID isn't 0. $USED"
	USED=$(getquota -g $TSTID global curspace)
	[ $USED -eq 0 ] || error "Used space for group $TSTID isn't 0. $USED"
	USED=$(getquota -g $TSTID global curinodes)
	[ $USED -eq 0 ] || error "Used inodes for group $TSTID isn't 0. $USED"
}
run_test 33 "Basic usage tracking for user & group"

# usage transfer test for user & group
test_34() {
	[[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.5.3) ]] &&
		skip "Need MDS version at least 2.5.3" && return

        local BLK_CNT=2 # 2MB

	setup_quota_test
        trap cleanup_quota_test EXIT

	# make sure the system is clean
	local USED=$(getquota -u $TSTID global curspace)
	[ $USED -ne 0 ] && error "Used space ($USED) for user $TSTID isn't 0."
	USED=$(getquota -g $TSTID global curspace)
	[ $USED -ne 0 ] && error "Used space ($USED) for group $TSTID isn't 0."

	local USED=$(getquota -u $TSTID2 global curspace)
	[ $USED -ne 0 ] && error "Used space ($USED) for user $TSTID2 isn't 0."

	echo "Write file..."
	$DD of=$DIR/$tdir/$tfile count=$BLK_CNT 2>/dev/null ||
		error "write failed"
	cancel_lru_locks osc
	sync; sync_all_data || true

	echo "chown the file to user $TSTID"
	chown $TSTID $DIR/$tdir/$tfile || error "chown failed"

	echo "Wait for setattr on objects finished..."
	wait_delete_completed

	BLK_CNT=$((BLK_CNT * 1024))

	echo "Verify disk usage for user $TSTID"
	USED=$(getquota -u $TSTID global curspace)
	[ $USED -lt $BLK_CNT ] &&
		error "Used space for user $TSTID is ${USED}, expected $BLK_CNT"
	USED=$(getquota -u $TSTID global curinodes)
	[ $USED -ne 1 ] &&
		error "Used inodes for user $TSTID is $USED, expected 1"

	echo "chgrp the file to group $TSTID"
	chgrp $TSTID $DIR/$tdir/$tfile || error "chgrp failed"

	echo "Wait for setattr on objects finished..."
	wait_delete_completed

	echo "Verify disk usage for group $TSTID"
	USED=$(getquota -g $TSTID global curspace)
	[ $USED -ge $BLK_CNT ] ||
		error "Used space for group $TSTID is $USED, expected $BLK_CNT"
	USED=$(getquota -g $TSTID global curinodes)
	[ $USED -eq 1 ] ||
		error "Used inodes for group $TSTID is $USED, expected 1"

	# chown won't change the ost object group. LU-4345 */
	echo "chown the file to user $TSTID2"
	chown $TSTID2 $DIR/$tdir/$tfile || error "chown to $TSTID2 failed"

	echo "Wait for setattr on objects finished..."
	wait_delete_completed

	echo "Verify disk usage for user $TSTID2/$TSTID and group $TSTID"
	USED=$(getquota -u $TSTID2 global curspace)
	[ $USED -lt $BLK_CNT ] &&
		error "Used space for user $TSTID2 is $USED, expected $BLK_CNT"
	USED=$(getquota -u $TSTID global curspace)
	[ $USED -ne 0 ] &&
		error "Used space for user $TSTID is $USED, expected 0"
	USED=$(getquota -g $TSTID global curspace)
	[ $USED -lt $BLK_CNT ] &&
		error "Used space for group $TSTID is $USED, expected $BLK_CNT"

	cleanup_quota_test
}
run_test 34 "Usage transfer for user & group"

# usage is still accessible across restart
test_35() {
	local BLK_CNT=2 # 2 MB

	setup_quota_test
	trap cleanup_quota_test EXIT

	echo "Write file..."
	$RUNAS $DD of=$DIR/$tdir/$tfile count=$BLK_CNT 2>/dev/null ||
		error "write failed"
	cancel_lru_locks osc
	sync; sync_all_data || true

	echo "Save disk usage before restart"
	local ORIG_USR_SPACE=$(getquota -u $TSTID global curspace)
	[ $ORIG_USR_SPACE -eq 0 ] &&
		error "Used space for user $TSTID is 0, expected ${BLK_CNT}M"
	local ORIG_USR_INODES=$(getquota -u $TSTID global curinodes)
	[ $ORIG_USR_INODES -eq 0 ] &&
		error "Used inodes for user $TSTID is 0, expected 1"
	echo "User $TSTID: ${ORIG_USR_SPACE}KB $ORIG_USR_INODES inodes"
	local ORIG_GRP_SPACE=$(getquota -g $TSTID global curspace)
	[ $ORIG_GRP_SPACE -eq 0 ] &&
		error "Used space for group $TSTID is 0, expected ${BLK_CNT}M"
	local ORIG_GRP_INODES=$(getquota -g $TSTID global curinodes)
	[ $ORIG_GRP_INODES -eq 0 ] &&
		error "Used inodes for group $TSTID is 0, expected 1"
	echo "Group $TSTID: ${ORIG_GRP_SPACE}KB $ORIG_GRP_INODES inodes"

	log "Restart..."
	local ORIG_REFORMAT=$REFORMAT
	REFORMAT=""
	cleanup_and_setup_lustre
	REFORMAT=$ORIG_REFORMAT
	quota_init

	echo "Verify disk usage after restart"
	local USED=$(getquota -u $TSTID global curspace)
	[ $USED -eq $ORIG_USR_SPACE ] ||
		error "Used space for user $TSTID changed from " \
			"$ORIG_USR_SPACE to $USED"
	USED=$(getquota -u $TSTID global curinodes)
	[ $USED -eq $ORIG_USR_INODES ] ||
		error "Used inodes for user $TSTID changed from " \
			"$ORIG_USR_INODES to $USED"
	USED=$(getquota -g $TSTID global curspace)
	[ $USED -eq $ORIG_GRP_SPACE ] ||
		error "Used space for group $TSTID changed from " \
			"$ORIG_GRP_SPACE to $USED"
	USED=$(getquota -g $TSTID global curinodes)
	[ $USED -eq $ORIG_GRP_INODES ] ||
		error "Used inodes for group $TSTID changed from " \
			"$ORIG_GRP_INODES to $USED"

	# check if the vfs_dq_init() is called before writing
	echo "Append to the same file..."
	$RUNAS $DD of=$DIR/$tdir/$tfile count=$BLK_CNT seek=1 2>/dev/null ||
		error "write failed"
	cancel_lru_locks osc
	sync; sync_all_data || true

	echo "Verify space usage is increased"
	USED=$(getquota -u $TSTID global curspace)
	[ $USED -gt $ORIG_USR_SPACE ] ||
		error "Used space for user $TSTID isn't increased" \
			"orig:$ORIG_USR_SPACE, now:$USED"
	USED=$(getquota -g $TSTID global curspace)
	[ $USED -gt $ORIG_GRP_SPACE ] ||
		error "Used space for group $TSTID isn't increased" \
			"orig:$ORIG_GRP_SPACE, now:$USED"

	cleanup_quota_test
}
run_test 35 "Usage is still accessible across reboot"

# test migrating old amdin quota files (in Linux quota file format v2) into new
# quota global index (in IAM format)
test_36() {
	[ $(facet_fstype $SINGLEMDS) != ldiskfs ] && \
		skip "skipping migration test" && return

	# get the mdt0 device name
	local mdt0_node=$(facet_active_host $SINGLEMDS)
	local mdt0_dev=$(mdsdevname ${SINGLEMDS//mds/})

	echo "Reformat..."
	formatall

	echo "Copy admin quota files into MDT0..."
	local mntpt=$(facet_mntpt $SINGLEMDS)
	local mdt0_fstype=$(facet_fstype $SINGLEMDS)
	local opt
	if ! do_node $mdt0_node test -b $mdt0_fstype; then
		opt="-o loop"
	fi
	echo "$mdt0_node, $mdt0_dev, $mntpt, $opt"
	do_node $mdt0_node mount -t $mdt0_fstype $opt $mdt0_dev $mntpt
	do_node $mdt0_node mkdir $mntpt/OBJECTS
	do_node $mdt0_node cp $LUSTRE/tests/admin_quotafile_v2.usr $mntpt/OBJECTS
	do_node $mdt0_node cp $LUSTRE/tests/admin_quotafile_v2.grp $mntpt/OBJECTS
	do_node $mdt0_node umount -d -f $mntpt

	echo "Setup all..."
	setupall

	echo "Verify global limits..."
	local id_cnt
	local limit

	local proc="qmt.*.md-0x0.glb-usr"
	id_cnt=$(do_node $mdt0_node $LCTL get_param -n $proc | wc -l)
	[ $id_cnt -eq 403 ] || error "Migrate inode user limit failed: $id_cnt"
	limit=$(getquota -u 1 global isoftlimit)
	[ $limit -eq 1024 ] || error "User inode softlimit: $limit"
	limit=$(getquota -u 1 global ihardlimit)
	[ $limit -eq 2048 ] || error "User inode hardlimit: $limit"

	proc="qmt.*.md-0x0.glb-grp"
	id_cnt=$(do_node $mdt0_node $LCTL get_param -n $proc | wc -l)
	[ $id_cnt -eq 403 ] || error "Migrate inode group limit failed: $id_cnt"
	limit=$(getquota -g 1 global isoftlimit)
	[ $limit -eq 1024 ] || error "Group inode softlimit: $limit"
	limit=$(getquota -g 1 global ihardlimit)
	[ $limit -eq 2048 ] || error "Group inode hardlimit: $limit"

	proc=" qmt.*.dt-0x0.glb-usr"
	id_cnt=$(do_node $mdt0_node $LCTL get_param -n $proc | wc -l)
	[ $id_cnt -eq 403 ] || error "Migrate block user limit failed: $id_cnt"
	limit=$(getquota -u 60001 global bsoftlimit)
	[ $limit -eq 10485760 ] || error "User block softlimit: $limit"
	limit=$(getquota -u 60001 global bhardlimit)
	[ $limit -eq 20971520 ] || error "User block hardlimit: $limit"

	proc="qmt.*.dt-0x0.glb-grp"
	id_cnt=$(do_node $mdt0_node $LCTL get_param -n $proc | wc -l)
	[ $id_cnt -eq 403 ] || error "Migrate block user limit failed: $id_cnt"
	limit=$(getquota -g 60001 global bsoftlimit)
	[ $limit -eq 10485760 ] || error "Group block softlimit: $limit"
	limit=$(getquota -g 60001 global bhardlimit)
	[ $limit -eq 20971520 ] || error "Group block hardlimit: $limit"

	echo "Cleanup..."
	formatall
	setupall
}
run_test 36 "Migrate old admin files into new global indexes"

quota_fini()
{
        do_nodes $(comma_list $(nodes_list)) "lctl set_param debug=-quota"
}
quota_fini

cd $ORIG_PWD
complete $SECONDS
check_and_cleanup_lustre
export QUOTA_AUTO=$QUOTA_AUTO_OLD
exit_status
