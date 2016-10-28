#!/bin/bash
#
# Run select tests by setting ONLY, or as arguments to the script.
# Skip specific tests by setting EXCEPT.
#
# exit on error
set -e
set +o monitor

SRCDIR=$(dirname $0)
export PATH=$PWD/$SRCDIR:$SRCDIR:$PWD/$SRCDIR/utils:$PATH:/sbin:/usr/sbin

ONLY=${ONLY:-"$*"}
# bug number for skipped test:    LU-3815
ALWAYS_EXCEPT="$SANITY_HSM_EXCEPT 34 35 36"
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

LUSTRE=${LUSTRE:-$(cd $(dirname $0)/..; echo $PWD)}

. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}
init_logging

MULTIOP=${MULTIOP:-multiop}
OPENFILE=${OPENFILE:-openfile}
MMAP_CAT=${MMAP_CAT:-mmap_cat}
MOUNT_2=${MOUNT_2:-"yes"}
FAIL_ON_ERROR=false

# script only handles up to 10 MDTs (because of MDT_PREFIX)
[ $MDSCOUNT -gt 9 ] &&
	error "script cannot handle more than 9 MDTs, please fix" && exit

check_and_setup_lustre

if [[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.4.53) ]]; then
	skip_env "Need MDS version at least 2.4.53" && exit
fi

# $RUNAS_ID may get set incorrectly somewhere else
if [[ $UID -eq 0 && $RUNAS_ID -eq 0 ]]; then
	skip_env "\$RUNAS_ID set to 0, but \$UID is also 0!" && exit
fi
check_runas_id $RUNAS_ID $RUNAS_GID $RUNAS

build_test_filter

# if there is no CLIENT1 defined, some tests can be ran on localhost
CLIENT1=${CLIENT1:-$HOSTNAME}
# if CLIENT2 doesn't exist then use CLIENT1 instead
# All tests should use CLIENT2 with MOUNT2 only therefore it will work if
# $CLIENT2 == CLIENT1
# Exception is the test which need two separate nodes
CLIENT2=${CLIENT2:-$CLIENT1}

#
# In order to test multiple remote HSM agents, a new facet type named "AGT" and
# the following associated variables are added:
#
# AGTCOUNT: number of agents
# AGTDEV{N}: target HSM mount point (root path of the backend)
# agt{N}_HOST: hostname of the agent agt{N}
# SINGLEAGT: facet of the single agent
#
# The number of agents is initialized as the number of remote client nodes.
# By default, only single copytool is started on a remote client/agent. If there
# was no remote client, then the copytool will be started on the local client.
#
init_agt_vars() {
	local n
	local agent

	export AGTCOUNT=${AGTCOUNT:-$((CLIENTCOUNT - 1))}
	[[ $AGTCOUNT -gt 0 ]] || AGTCOUNT=1

	export SHARED_DIRECTORY=${SHARED_DIRECTORY:-$TMP}
	if [[ $CLIENTCOUNT -gt 1 ]] &&
		! check_shared_dir $SHARED_DIRECTORY $CLIENTS; then
		skip_env "SHARED_DIRECTORY should be accessible"\
			 "on all client nodes"
		exit 0
	fi

	# We used to put the HSM archive in $SHARED_DIRECTORY but that
	# meant NFS issues could hose sanity-hsm sessions. So now we
	# use $TMP instead.
	for n in $(seq $AGTCOUNT); do
		eval export AGTDEV$n=\$\{AGTDEV$n:-"$TMP/arc$n"\}
		agent=CLIENT$((n + 1))
		if [[ -z "${!agent}" ]]; then
			[[ $CLIENTCOUNT -eq 1 ]] && agent=CLIENT1 ||
				agent=CLIENT2
		fi
		eval export agt${n}_HOST=\$\{agt${n}_HOST:-${!agent}\}
	done

	export SINGLEAGT=${SINGLEAGT:-agt1}

	export HSMTOOL=${HSMTOOL:-"lhsmtool_posix"}
	export HSMTOOL_VERBOSE=${HSMTOOL_VERBOSE:-""}
	export HSMTOOL_UPDATE_INTERVAL=${HSMTOOL_UPDATE_INTERVAL:=""}
	export HSMTOOL_EVENT_FIFO=${HSMTOOL_EVENT_FIFO:=""}
	export HSMTOOL_TESTDIR
	export HSMTOOL_BASE=$(basename "$HSMTOOL" | cut -f1 -d" ")
	HSM_ARCHIVE=$(copytool_device $SINGLEAGT)
	HSM_ARCHIVE_NUMBER=2

	# The test only support up to 10 MDTs
	MDT_PREFIX="mdt.$FSNAME-MDT000"
	HSM_PARAM="${MDT_PREFIX}0.hsm"

	# archive is purged at copytool setup
	HSM_ARCHIVE_PURGE=true

	# Don't allow copytool error upon start/setup
	HSMTOOL_NOERROR=false
}

# Get the backend root path for the given agent facet.
copytool_device() {
	local facet=$1
	local dev=AGTDEV$(facet_number $facet)

	echo -n ${!dev}
}

# Stop copytool and unregister an existing changelog user.
cleanup() {
	copytool_monitor_cleanup
	copytool_cleanup
	changelog_cleanup
	cdt_set_sanity_policy
}

get_mdt_devices() {
	local mdtno
	# get MDT device for each mdc
	for mdtno in $(seq 1 $MDSCOUNT); do
		local idx=$(($mdtno - 1))
		MDT[$idx]=$($LCTL get_param -n \
			mdc.$FSNAME-MDT000${idx}-mdc-*.mds_server_uuid |
			awk '{gsub(/_UUID/,""); print $1}' | head -n1)
	done
}

search_copytools() {
	local hosts=${1:-$(facet_active_host $SINGLEAGT)}
	do_nodesv $hosts "pgrep -x $HSMTOOL_BASE"
}

kill_copytools() {
	local hosts=${1:-$(facet_active_host $SINGLEAGT)}

	echo "Killing existing copytools on $hosts"
	do_nodesv $hosts "killall -q $HSMTOOL_BASE" || true
}

wait_copytools() {
	local hosts=${1:-$(facet_active_host $SINGLEAGT)}
	local wait_timeout=200
	local wait_start=$SECONDS
	local wait_end=$((wait_start + wait_timeout))

	while ((SECONDS < wait_end)); do
		sleep 2
		if ! search_copytools $hosts; then
			echo "copytools stopped in $((SECONDS - wait_start))s"
			return 0
		fi

		echo "copytools still running on $hosts"
	done

	# try to dump Copytool's stack
	do_nodesv $hosts "echo 1 >/proc/sys/kernel/sysrq ; " \
			 "echo t >/proc/sysrq-trigger"

	echo "copytools failed to stop in ${wait_timeout}s"

	return 1
}

copytool_monitor_setup() {
	local facet=${1:-$SINGLEAGT}
	local agent=$(facet_active_host $facet)

	local cmd="mktemp --tmpdir=/tmp -d ${TESTSUITE}.${TESTNAME}.XXXX"
	local test_dir=$(do_node $agent "$cmd") ||
		error "Failed to create tempdir on $agent"
	export HSMTOOL_MONITOR_DIR=$test_dir

	# Create the fifo and a monitor (cat dies when copytool dies)
	do_node $agent "mkfifo -m 0644 $test_dir/fifo" ||
		error "failed to create copytool fifo on $agent"
	cmd="cat $test_dir/fifo > $test_dir/events &"
	cmd+=" echo \\\$! > $test_dir/monitor_pid"

	if [[ $PDSH == *Rmrsh* ]]; then
		# This is required for pdsh -Rmrsh and its handling of remote
		# shells.
		# Regular ssh and pdsh -Rssh work fine without this
		# backgrounded subshell nonsense.
		(do_node $agent "$cmd") &
		export HSMTOOL_MONITOR_PDSH=$!

		# Slightly racy, but just making a best-effort to catch obvious
		# problems.
		sleep 1
		ps -p $HSMTOOL_MONITOR_PDSH > /dev/null ||
			error "Failed to start copytool monitor on $agent"
	else
		do_node $agent "$cmd"
		if [ $? != 0 ]; then
			error "Failed to start copytool monitor on $agent"
		fi
	fi
}

copytool_monitor_cleanup() {
	local facet=${1:-$SINGLEAGT}
	local agent=$(facet_active_host $facet)

	if [ -n "$HSMTOOL_MONITOR_DIR" ]; then
		# Should die when the copytool dies, but just in case.
		local cmd="kill \\\$(cat $HSMTOOL_MONITOR_DIR/monitor_pid)"
		cmd+=" 2>/dev/null || true"
		do_node $agent "$cmd"
		do_node $agent "rm -fr $HSMTOOL_MONITOR_DIR"
		export HSMTOOL_MONITOR_DIR=
	fi

	# The pdsh should die on its own when the monitor dies. Just
	# in case, though, try to clean up to avoid any cruft.
	if [ -n "$HSMTOOL_MONITOR_PDSH" ]; then
		kill $HSMTOOL_MONITOR_PDSH 2>/dev/null
		export HSMTOOL_MONITOR_PDSH=
	fi
}

copytool_setup() {
	local facet=${1:-$SINGLEAGT}
	# Use MOUNT2 by default if defined
	local lustre_mntpnt=${2:-${MOUNT2:-$MOUNT}}
	local arc_id=$3
	local hsm_root=${4:-$(copytool_device $facet)}
	local agent=$(facet_active_host $facet)

	if [[ -z "$arc_id" ]] &&
		do_facet $facet "pkill -CONT -x $HSMTOOL_BASE"; then
			echo "Only wakeup running copytool $facet on $agent"
			return 0
	fi

	if $HSM_ARCHIVE_PURGE; then
		echo "Purging archive on $agent"
		do_facet $facet "rm -rf $hsm_root/*"
	fi

	echo "Starting copytool $facet on $agent"
	do_facet $facet "mkdir -p $hsm_root" || error "mkdir '$hsm_root' failed"
	# bandwidth is limited to 1MB/s so the copy time is known and
	# independent of hardware
	local cmd="$HSMTOOL $HSMTOOL_VERBOSE --daemon --hsm-root $hsm_root"
	[[ -z "$arc_id" ]] || cmd+=" --archive $arc_id"
	[[ -z "$HSMTOOL_UPDATE_INTERVAL" ]] ||
		cmd+=" --update-interval $HSMTOOL_UPDATE_INTERVAL"
	[[ -z "$HSMTOOL_EVENT_FIFO" ]] ||
		cmd+=" --event-fifo $HSMTOOL_EVENT_FIFO"
	cmd+=" --bandwidth 1 $lustre_mntpnt"

	# Redirect the standard output and error to a log file which
	# can be uploaded to Maloo.
	local prefix=$TESTLOG_PREFIX
	[[ -z "$TESTNAME" ]] || prefix=$prefix.$TESTNAME
	local copytool_log=$prefix.copytool${arc_id}_log.$agent.log

	do_facet $facet "$cmd < /dev/null > $copytool_log 2>&1"
	if [[ $? !=  0 ]]; then
		[[ $HSMTOOL_NOERROR == true ]] ||
			error "start copytool $facet on $agent failed"
		echo "start copytool $facet on $agent failed"
	fi

	trap cleanup EXIT
}

get_copytool_event_log() {
	local facet=${1:-$SINGLEAGT}
	local agent=$(facet_active_host $facet)

	[ -z "$HSMTOOL_MONITOR_DIR" ] &&
		error "Can't get event log: No monitor directory!"

	do_node $agent "cat $HSMTOOL_MONITOR_DIR/events" ||
		error "Could not collect event log from $agent"
}

copytool_cleanup() {
	trap - EXIT
	local agt_facet=$SINGLEAGT
	local agt_hosts=${1:-$(facet_active_host $agt_facet)}
	local hsm_root=$(copytool_device $agt_facet)
	local i
	local facet
	local param
	local -a state

	kill_copytools $agt_hosts
	wait_copytools $agt_hosts || error "copytools failed to stop"

	# Clean all CDTs orphans requests from previous tests that
	# would otherwise need to timeout to clear.
	for ((i = 0; i < MDSCOUNT; i++)); do
		facet=mds$((i + 1))
		param=$(printf 'mdt.%s-MDT%04x.hsm_control' $FSNAME $i)
		state[$i]=$(do_facet $facet "$LCTL get_param -n $param")

		# Skip already stopping or stopped CDTs.
		[[ "${state[$i]}" =~ ^stop ]] && continue

		do_facet $facet "$LCTL set_param $param=shutdown"
	done

	for ((i = 0; i < MDSCOUNT; i++)); do
		# Only check and restore CDTs that we stopped in the first loop.
		[[ "${state[$i]}" =~ ^stop ]] && continue

		facet=mds$((i + 1))
		param=$(printf 'mdt.%s-MDT%04x.hsm_control' $FSNAME $i)

		wait_result $facet "$LCTL get_param -n $param" stopped 20 ||
			error "$facet CDT state is not stopped"

		# Restore old CDT state.
		do_facet $facet "$LCTL set_param $param=${state[$i]}"
	done

	for ((i = 0; i < MDSCOUNT; i++)); do
		# Only check CDTs that we stopped in the first loop.
		[[ "${state[$i]}" =~ ^stop ]] && continue

		facet=mds$((i + 1))
		param=$(printf 'mdt.%s-MDT%04x.hsm_control' $FSNAME $i)

		# Check that the old CDT state was restored.
		wait_result $facet "$LCTL get_param -n $param" "${state[$i]}" \
			20 || error "$facet CDT state is not '${state[$i]}'"
	done

	if do_facet $agt_facet "df $hsm_root" >/dev/null 2>&1 ; then
		do_facet $agt_facet "rm -rf $hsm_root/*"
	fi
}

copytool_suspend() {
	local agents=${1:-$(facet_active_host $SINGLEAGT)}

	do_nodesv $agents "pkill -STOP -x $HSMTOOL_BASE" || return 0
	echo "Copytool is suspended on $agents"
}

copytool_remove_backend() {
	local fid=$1
	local be=$(do_facet $SINGLEAGT find $HSM_ARCHIVE -name $fid)
	echo "Remove from backend: $fid = $be"
	do_facet $SINGLEAGT rm -f $be
}

import_file() {
	do_facet $SINGLEAGT \
		"$HSMTOOL --archive $HSM_ARCHIVE_NUMBER --hsm-root $HSM_ARCHIVE\
		--import $1 $2 $MOUNT" ||
		error "import of $1 to $2 failed"
}

make_archive() {
	local file=$HSM_ARCHIVE/$1
	do_facet $SINGLEAGT mkdir -p $(dirname $file)
	do_facet $SINGLEAGT dd if=/dev/urandom of=$file count=32 bs=1000000 ||
		file_creation_failure dd $file $?
}

copy2archive() {
	local file=$HSM_ARCHIVE/$2
	do_facet $SINGLEAGT mkdir -p $(dirname $file)
	do_facet $SINGLEAGT cp -p $1 $file || error "cannot copy $1 to $file"
}

mdts_set_param() {
	local arg=$1
	local key=$2
	local value=$3
	local mdtno
	local rc=0
	if [[ "$value" != "" ]]; then
		value="=$value"
	fi
	for mdtno in $(seq 1 $MDSCOUNT); do
		local idx=$(($mdtno - 1))
		local facet=mds${mdtno}
		# if $arg include -P option, run 1 set_param per MDT on the MGS
		# else, run set_param on each MDT
		[[ $arg = *"-P"* ]] && facet=mgs
		do_facet $facet $LCTL set_param $arg mdt.${MDT[$idx]}.$key$value
		[[ $? != 0 ]] && rc=1
	done
	return $rc
}

mdts_check_param() {
	local key="$1"
	local target="$2"
	local timeout="$3"
	local mdtno
	for mdtno in $(seq 1 $MDSCOUNT); do
		local idx=$(($mdtno - 1))
		wait_result mds${mdtno} \
			"$LCTL get_param -n $MDT_PREFIX${idx}.$key" "$target" \
			$timeout ||
			error "$key state is not '$target' on mds${mdtno}"
	done
}

changelog_setup() {
	CL_USERS=()
	local mdtno
	for mdtno in $(seq 1 $MDSCOUNT); do
		local idx=$(($mdtno - 1))
		local cl_user=$(do_facet mds${mdtno} $LCTL \
			     --device ${MDT[$idx]} \
			     changelog_register -n)
		CL_USERS+=($cl_user)
		do_facet mds${mdtno} lctl set_param \
			mdd.${MDT[$idx]}.changelog_mask="+hsm"
		$LFS changelog_clear ${MDT[$idx]} $cl_user 0
	done
}

changelog_cleanup() {
	local mdtno
	for mdtno in $(seq 1 $MDSCOUNT); do
		local idx=$(($mdtno - 1))
		[[ -z  ${CL_USERS[$idx]} ]] && continue
		$LFS changelog_clear ${MDT[$idx]} ${CL_USERS[$idx]} 0
		do_facet mds${mdtno} lctl --device ${MDT[$idx]} \
			changelog_deregister ${CL_USERS[$idx]}
	done
	CL_USERS=()
}

changelog_get_flags() {
	local mdt=$1
	local cltype=$2
	local fid=$3

	$LFS changelog $mdt | awk "/$cltype/ && /t=\[$fid\]/ {print \$5}"
}

get_hsm_param() {
	local param=$1
	local val=$(do_facet $SINGLEMDS $LCTL get_param -n $HSM_PARAM.$param)
	echo $val
}

set_hsm_param() {
	local param=$1
	local value=$2
	local opt=$3
	mdts_set_param "$opt -n" "hsm.$param" "$value"
	return $?
}

set_test_state() {
	local cmd=$1
	local target=$2
	mdts_set_param "" hsm_control "$cmd"
	mdts_check_param hsm_control "$target" 10
}

cdt_set_sanity_policy() {
	if [[ "$CDT_POLICY_HAD_CHANGED" ]]
	then
		# clear all
		mdts_set_param "" hsm.policy "+NRA"
		mdts_set_param "" hsm.policy "-NBR"
		CDT_POLICY_HAD_CHANGED=
	fi
}

cdt_set_no_retry() {
	mdts_set_param "" hsm.policy "+NRA"
	CDT_POLICY_HAD_CHANGED=true
}

cdt_clear_no_retry() {
	mdts_set_param "" hsm.policy "-NRA"
	CDT_POLICY_HAD_CHANGED=true
}

cdt_set_non_blocking_restore() {
	mdts_set_param "" hsm.policy "+NBR"
	CDT_POLICY_HAD_CHANGED=true
}

cdt_clear_non_blocking_restore() {
	mdts_set_param "" hsm.policy "-NBR"
	CDT_POLICY_HAD_CHANGED=true
}

cdt_clear_mount_state() {
	mdts_set_param "-P -d" hsm_control ""
}

cdt_set_mount_state() {
	mdts_set_param "-P" hsm_control "$1"
	# set_param -P is asynchronous operation and could race with set_param.
	# In such case configs could be retrieved and applied at mgc after
	# set_param -P completion. Sleep here to avoid race with set_param.
	# We need at least 20 seconds. 10 for mgc_requeue_thread to wake up
	# MGC_TIMEOUT_MIN_SECONDS + MGC_TIMEOUT_RAND_CENTISEC(5 + 5)
	# and 10 seconds to retrieve config from server.
	sleep 20
}

cdt_check_state() {
	mdts_check_param hsm_control "$1" 20
}

cdt_disable() {
	set_test_state disabled disabled
}

cdt_enable() {
	set_test_state enabled enabled
}

cdt_shutdown() {
	set_test_state shutdown stopped
}

cdt_purge() {
	set_test_state purge enabled
}

cdt_restart() {
	cdt_shutdown
	cdt_enable
	cdt_set_sanity_policy
}

needclients() {
	local client_count=$1
	if [[ $CLIENTCOUNT -lt $client_count ]]; then
		skip "Need $client_count or more clients, have $CLIENTCOUNT"
		return 1
	fi
	return 0
}

path2fid() {
	$LFS path2fid $1 | tr -d '[]'
	return ${PIPESTATUS[0]}
}

get_hsm_flags() {
	local f=$1
	local u=$2
	local st

	if [[ $u == "user" ]]; then
		st=$($RUNAS $LFS hsm_state $f)
	else
		u=root
		st=$($LFS hsm_state $f)
	fi

	[[ $? == 0 ]] || error "$LFS hsm_state $f failed (run as $u)"

	st=$(echo $st | cut -f 2 -d" " | tr -d "()," )
	echo $st
}

get_hsm_archive_id() {
	local f=$1
	local st
	st=$($LFS hsm_state $f)
	[[ $? == 0 ]] || error "$LFS hsm_state $f failed"

	local ar=$(echo $st | grep "archive_id" | cut -f5 -d" " |
		   cut -f2 -d:)
	echo $ar
}

check_hsm_flags() {
	local f=$1
	local fl=$2

	local st=$(get_hsm_flags $f)
	[[ $st == $fl ]] || error "hsm flags on $f are $st != $fl"
}

check_hsm_flags_user() {
	local f=$1
	local fl=$2

	local st=$(get_hsm_flags $f user)
	[[ $st == $fl ]] || error "hsm flags on $f are $st != $fl"
}

file_creation_failure() {
	local cmd=$1
	local f=$2
	local err=$3

	df $MOUNT $MOUNT2 >&2
	error "cannot create $f with $cmd, status=$err"
}

copy_file() {
	local f=

	if [[ -d $2 ]]; then
		f=$2/$(basename $1)
	else
		f=$2
	fi

	if [[ "$3" != 1 ]]; then
		f=${f/$DIR/$DIR2}
	fi
	rm -f $f
	cp $1 $f || file_creation_failure cp $f $?

	path2fid $f || error "cannot get fid on $f"
}

make_small() {
        local file2=${1/$DIR/$DIR2}
        dd if=/dev/urandom of=$file2 count=2 bs=1M conv=fsync ||
		file_creation_failure dd $file2 $?

        path2fid $1 || error "cannot get fid on $1"
}

make_small_sync() {
	dd if=/dev/urandom of=$1 count=1 bs=1M conv=sync ||
		file_creation_failure dd $1 $?
	path2fid $1 || error "cannot get fid on $1"
}

cleanup_large_files() {
	local ratio=$(df -P $MOUNT | tail -1 | awk '{print $5}' |
		      sed 's/%//g')
	[ $ratio -gt 50 ] && find $MOUNT -size +10M -exec rm -f {} \;
}

check_enough_free_space() {
	local nb=$1
	local unit=$2
	local need=$((nb * unit /1024))
	local free=$(df -kP $MOUNT | tail -1 | awk '{print $4}')
	(( $need >= $free )) && return 1
	return 0
}

make_large_for_striping() {
	local file2=${1/$DIR/$DIR2}
	local sz=$($LCTL get_param -n lov.*-clilov-*.stripesize | head -n1)

	cleanup_large_files

	check_enough_free_space 5 $sz
	[ $? != 0 ] && return $?

	dd if=/dev/urandom of=$file2 count=5 bs=$sz conv=fsync ||
		file_creation_failure dd $file2 $?

	path2fid $1 || error "cannot get fid on $1"
}

make_large_for_progress() {
	local file2=${1/$DIR/$DIR2}

	cleanup_large_files

	check_enough_free_space 39 1000000
	[ $? != 0 ] && return $?

	# big file is large enough, so copy time is > 30s
	# so copytool make 1 progress
	# size is not a multiple of 1M to avoid stripe
	# aligment
	dd if=/dev/urandom of=$file2 count=39 bs=1000000 conv=fsync ||
		file_creation_failure dd $file2 $?

	path2fid $1 || error "cannot get fid on $1"
}

make_large_for_progress_aligned() {
	local file2=${1/$DIR/$DIR2}

	cleanup_large_files

	check_enough_free_space 33 1048576
	[ $? != 0 ] && return $?

	# big file is large enough, so copy time is > 30s
	# so copytool make 1 progress
	# size is a multiple of 1M to have stripe
	# aligment
	dd if=/dev/urandom of=$file2 count=33 bs=1M conv=fsync ||
		file_creation_failure dd $file2 $?
	path2fid $1 || error "cannot get fid on $1"
}

make_large_for_cancel() {
	local file2=${1/$DIR/$DIR2}

	cleanup_large_files

	check_enough_free_space 103 1048576
	[ $? != 0 ] && return $?

	# Copy timeout is 100s. 105MB => 105s
	dd if=/dev/urandom of=$file2 count=103 bs=1M conv=fsync ||
		file_creation_failure dd $file2 $?
	path2fid $1 || error "cannot get fid on $1"
}

wait_result() {
	local facet=$1
	shift
	wait_update --verbose $(facet_active_host $facet) "$@"
}

wait_request_state() {
	local fid=$1
	local request=$2
	local state=$3
	# 4th arg (mdt index) is optional
	local mdtidx=${4:-0}
	local mds=mds$(($mdtidx + 1))

	local cmd="$LCTL get_param -n ${MDT_PREFIX}${mdtidx}.hsm.actions"
	cmd+=" | awk '/'$fid'.*action='$request'/ {print \\\$13}' | cut -f2 -d="

	wait_result $mds "$cmd" $state 200 ||
		error "request on $fid is not $state on $mds"
}

get_request_state() {
	local fid=$1
	local request=$2

	do_facet $SINGLEMDS "$LCTL get_param -n $HSM_PARAM.actions |"\
		"awk '/'$fid'.*action='$request'/ {print \\\$13}' | cut -f2 -d="
}

get_request_count() {
	local fid=$1
	local request=$2

	do_facet $SINGLEMDS "$LCTL get_param -n $HSM_PARAM.actions |"\
		"awk -vn=0 '/'$fid'.*action='$request'/ {n++}; END {print n}'"
}

wait_all_done() {
	local timeout=$1
	local fid=$2

	local cmd="$LCTL get_param -n $HSM_PARAM.actions"
	[[ -n $fid ]] && cmd+=" | grep '$fid'"
	cmd+=" | egrep 'WAITING|STARTED'"

	wait_result $SINGLEMDS "$cmd" "" $timeout ||
		error "requests did not complete"
}

wait_for_grace_delay() {
	local val=$(get_hsm_param grace_delay)
	sleep $val
}

parse_json_event() {
	local raw_event=$1

	# python2.6 in EL6 includes an internal json module
	local json_parser='import json; import fileinput;'
	json_parser+=' print "\n".join(["local %s=\"%s\"" % tuple for tuple in '
	json_parser+='json.loads([line for line in '
	json_parser+='fileinput.input()][0]).items()])'

	echo $raw_event | python -c "$json_parser"
}

# populate MDT device array
get_mdt_devices

# initiate variables
init_agt_vars

# cleanup from previous bad setup
kill_copytools

# for recovery tests, coordinator needs to be started at mount
# so force it
# the lustre conf must be without hsm on (like for sanity.sh)
echo "Set HSM on and start"
cdt_set_mount_state enabled
cdt_check_state enabled

echo "Start copytool"
copytool_setup

echo "Set sanity-hsm HSM policy"
cdt_set_sanity_policy

# finished requests are quickly removed from list
set_hsm_param grace_delay 10

test_1() {
	mkdir -p $DIR/$tdir
	chmod 777 $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	$RUNAS touch $f

	# User flags
	check_hsm_flags_user $f "0x00000000"

	$RUNAS $LFS hsm_set --norelease $f ||
		error "user could not change hsm flags"
	check_hsm_flags_user $f "0x00000010"

	$RUNAS $LFS hsm_clear --norelease $f ||
		error "user could not clear hsm flags"
	check_hsm_flags_user $f "0x00000000"

	# User could not change those flags...
	$RUNAS $LFS hsm_set --exists $f &&
		error "user should not set this flag"
	check_hsm_flags_user $f "0x00000000"

	# ...but root can
	$LFS hsm_set --exists $f ||
		error "root could not change hsm flags"
	check_hsm_flags_user $f "0x00000001"

	$LFS hsm_clear --exists $f ||
		error "root could not clear hsm state"
	check_hsm_flags_user $f "0x00000000"

}
run_test 1 "lfs hsm flags root/non-root access"

test_1a() {
	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(make_small $f)

	$LFS hsm_archive $f || error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED

	# Release and check states
	$LFS hsm_release $f || error "could not release file"
	echo -n "Verifying released state: "
	check_hsm_flags $f "0x0000000d"

	$MMAP_CAT $f > /dev/null || error "failed mmap & cat release file"
}
run_test 1a "mmap & cat a HSM released file"

test_2() {
	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	touch $f
	# New files are not dirty
	check_hsm_flags $f "0x00000000"

	# For test, we simulate an archived file.
	$LFS hsm_set --exists $f || error "user could not change hsm flags"
	check_hsm_flags $f "0x00000001"

	# chmod do not put the file dirty
	chmod 600 $f || error "could not chmod test file"
	check_hsm_flags $f "0x00000001"

	# chown do not put the file dirty
	chown $RUNAS_ID $f || error "could not chown test file"
	check_hsm_flags $f "0x00000001"

	# truncate put the file dirty
	$TRUNCATE $f 1 || error "could not truncate test file"
	check_hsm_flags $f "0x00000003"

	$LFS hsm_clear --dirty $f || error "could not clear hsm flags"
	check_hsm_flags $f "0x00000001"
}
run_test 2 "Check file dirtyness when doing setattr"

test_3() {
	mkdir -p $DIR/$tdir
	f=$DIR/$tdir/$tfile

	# New files are not dirty
	cp -p /etc/passwd $f
	check_hsm_flags $f "0x00000000"

	# For test, we simulate an archived file.
	$LFS hsm_set --exists $f ||
		error "user could not change hsm flags"
	check_hsm_flags $f "0x00000001"

	# Reading a file, does not set dirty
	cat $f > /dev/null || error "could not read file"
	check_hsm_flags $f "0x00000001"

	# Open for write without modifying data, does not set dirty
	openfile -f O_WRONLY $f || error "could not open test file"
	check_hsm_flags $f "0x00000001"

	# Append to a file sets it dirty
	cp -p /etc/passwd $f.append || error "could not create file"
	$LFS hsm_set --exists $f.append ||
		error "user could not change hsm flags"
	dd if=/etc/passwd of=$f.append bs=1 count=3\
	   conv=notrunc oflag=append status=noxfer ||
		file_creation_failure dd $f.append $?
	check_hsm_flags $f.append "0x00000003"

	# Modify a file sets it dirty
	cp -p /etc/passwd $f.modify || error "could not create file"
	$LFS hsm_set --exists $f.modify ||
		error "user could not change hsm flags"
	dd if=/dev/zero of=$f.modify bs=1 count=3\
	   conv=notrunc status=noxfer ||
		file_creation_failure dd $f.modify $?
	check_hsm_flags $f.modify "0x00000003"

	# Open O_TRUNC sets dirty
	cp -p /etc/passwd $f.trunc || error "could not create file"
	$LFS hsm_set --exists $f.trunc ||
		error "user could not change hsm flags"
	cp /etc/group $f.trunc || error "could not override a file"
	check_hsm_flags $f.trunc "0x00000003"

	# Mmapped a file sets dirty
	cp -p /etc/passwd $f.mmap || error "could not create file"
	$LFS hsm_set --exists $f.mmap ||
		error "user could not change hsm flags"
	multiop $f.mmap OSMWUc || error "could not mmap a file"
	check_hsm_flags $f.mmap "0x00000003"
}
run_test 3 "Check file dirtyness when opening for write"

test_4() {
	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(make_small $f)

	$LFS hsm_cancel $f
	local st=$(get_request_state $fid CANCEL)
	[[ -z "$st" ]] || error "hsm_cancel must not be registered (state=$st)"
}
run_test 4 "Useless cancel must not be registered"

test_8() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)
	$LFS hsm_archive $f
	wait_request_state $fid ARCHIVE SUCCEED

	check_hsm_flags $f "0x00000009"

	copytool_cleanup
}
run_test 8 "Test default archive number"

test_9() {
	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)
	# we do not use the default one to be sure
	local new_an=$((HSM_ARCHIVE_NUMBER + 1))
	copytool_cleanup
	copytool_setup $SINGLEAGT $MOUNT $new_an
	$LFS hsm_archive --archive $new_an $f
	wait_request_state $fid ARCHIVE SUCCEED

	check_hsm_flags $f "0x00000009"

	copytool_cleanup
}
run_test 9 "Use of explicit archive number, with dedicated copytool"

test_9a() {
	needclients 3 || return 0

	local n
	local file
	local fid

	copytool_cleanup $(comma_list $(agts_nodes))

	# start all of the copytools
	for n in $(seq $AGTCOUNT); do
		copytool_setup agt$n
	done

	trap "copytool_cleanup $(comma_list $(agts_nodes))" EXIT
	# archive files
	mkdir -p $DIR/$tdir
	for n in $(seq $AGTCOUNT); do
		file=$DIR/$tdir/$tfile.$n
		fid=$(make_small $file)

		$LFS hsm_archive $file || error "could not archive file $file"
		wait_request_state $fid ARCHIVE SUCCEED
		check_hsm_flags $file "0x00000009"
	done

	trap - EXIT
	copytool_cleanup $(comma_list $(agts_nodes))
}
run_test 9a "Multiple remote agents"

test_10a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir/d1
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)
	$LFS hsm_archive -a $HSM_ARCHIVE_NUMBER $f ||
		error "hsm_archive failed"
	wait_request_state $fid ARCHIVE SUCCEED

	local AFILE=$(do_facet $SINGLEAGT ls $HSM_ARCHIVE'/*/*/*/*/*/*/'$fid) ||
		error "fid $fid not in archive $HSM_ARCHIVE"
	echo "Verifying content"
	do_facet $SINGLEAGT diff $f $AFILE || error "archived file differs"
	echo "Verifying hsm state "
	check_hsm_flags $f "0x00000009"

	echo "Verifying archive number is $HSM_ARCHIVE_NUMBER"
	local st=$(get_hsm_archive_id $f)
	[[ $st == $HSM_ARCHIVE_NUMBER ]] ||
		error "Wrong archive number, $st != $HSM_ARCHIVE_NUMBER"

	copytool_cleanup

}
run_test 10a "Archive a file"

test_10b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)
	$LFS hsm_archive $f || error "archive request failed"
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_archive $f || error "archive of non dirty file failed"
	local cnt=$(get_request_count $fid ARCHIVE)
	[[ "$cnt" == "1" ]] ||
		error "archive of non dirty file must not make a request"

	copytool_cleanup
}
run_test 10b "Archive of non dirty file must work without doing request"

test_10c() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)
	$LFS hsm_set --noarchive $f
	$LFS hsm_archive $f && error "archive a noarchive file must fail"

	copytool_cleanup
}
run_test 10c "Check forbidden archive"

test_10d() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)
	$LFS hsm_archive $f || error "cannot archive $f"
	wait_request_state $fid ARCHIVE SUCCEED

	local ar=$(get_hsm_archive_id $f)
	local dflt=$(get_hsm_param default_archive_id)
	[[ $ar == $dflt ]] ||
		error "archived file is not on default archive: $ar != $dflt"

	copytool_cleanup
}
run_test 10d "Archive a file on the default archive id"

test_11a() {
	mkdir -p $DIR/$tdir
	copy2archive /etc/hosts $tdir/$tfile
	local f=$DIR/$tdir/$tfile

	import_file $tdir/$tfile $f
	echo -n "Verifying released state: "
	check_hsm_flags $f "0x0000000d"

	local LSZ=$(stat -c "%s" $f)
	local ASZ=$(do_facet $SINGLEAGT stat -c "%s" $HSM_ARCHIVE/$tdir/$tfile)

	echo "Verifying imported size $LSZ=$ASZ"
	[[ $LSZ -eq $ASZ ]] || error "Incorrect size $LSZ != $ASZ"
	echo -n "Verifying released pattern: "
	local PTRN=$($GETSTRIPE -L $f)
	echo $PTRN
	[[ $PTRN == 80000001 ]] || error "Is not released"
	local fid=$(path2fid $f)
	echo "Verifying new fid $fid in archive"

	local AFILE=$(do_facet $SINGLEAGT ls $HSM_ARCHIVE'/*/*/*/*/*/*/'$fid) ||
		error "fid $fid not in archive $HSM_ARCHIVE"
}
run_test 11a "Import a file"

test_11b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)
	$LFS hsm_archive -a $HSM_ARCHIVE_NUMBER $f ||
		error "hsm_archive failed"
	wait_request_state $fid ARCHIVE SUCCEED

	local FILE_HASH=$(md5sum $f)
	rm -f $f

	import_file $fid $f

	echo "$FILE_HASH" | md5sum -c

	[[ $? -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 11b "Import a deleted file using its FID"

test_12a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	copy2archive /etc/hosts $tdir/$tfile

	local f=$DIR/$tdir/$tfile
	import_file $tdir/$tfile $f
	local f2=$DIR2/$tdir/$tfile
	echo "Verifying released state: "
	check_hsm_flags $f2 "0x0000000d"

	local fid=$(path2fid $f2)
	$LFS hsm_restore $f2
	wait_request_state $fid RESTORE SUCCEED

	echo "Verifying file state: "
	check_hsm_flags $f2 "0x00000009"

	do_facet $SINGLEAGT diff -q $HSM_ARCHIVE/$tdir/$tfile $f

	[[ $? -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 12a "Restore an imported file explicitly"

test_12b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	copy2archive /etc/hosts $tdir/$tfile

	local f=$DIR/$tdir/$tfile
	import_file $tdir/$tfile $f
	echo "Verifying released state: "
	check_hsm_flags $f "0x0000000d"

	cat $f > /dev/null || error "File read failed"

	echo "Verifying file state after restore: "
	check_hsm_flags $f "0x00000009"

	do_facet $SINGLEAGT diff -q $HSM_ARCHIVE/$tdir/$tfile $f

	[[ $? -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 12b "Restore an imported file implicitly"

test_12c() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping 2-stripe test" && return

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	$LFS setstripe -c 2 $f
	local fid
	fid=$(make_large_for_striping $f)
	[ $? != 0 ] && skip "not enough free space" && return

	local FILE_CRC=$(md5sum $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f || error "release $f failed"

	echo "$FILE_CRC" | md5sum -c

	[[ $? -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 12c "Restore a file with stripe of 2"

test_12d() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)
	$LFS hsm_restore $f || error "restore of non archived file failed"
	local cnt=$(get_request_count $fid RESTORE)
	[[ "$cnt" == "0" ]] ||
		error "restore non archived must not make a request"
	$LFS hsm_archive $f ||
		error "archive request failed"
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_restore $f ||
		error "restore of non released file failed"
	local cnt=$(get_request_count $fid RESTORE)
	[[ "$cnt" == "0" ]] ||
		error "restore a non dirty file must not make a request"

	copytool_cleanup
}
run_test 12d "Restore of a non archived, non released file must work"\
		" without doing request"

test_12e() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir $HSM_ARCHIVE/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)
	$LFS hsm_archive $f || error "archive request failed"
	wait_request_state $fid ARCHIVE SUCCEED

	# make file dirty
	cat /etc/hosts >> $f
	sync
	$LFS hsm_state $f

	$LFS hsm_restore $f && error "restore a dirty file must fail"

	copytool_cleanup
}
run_test 12e "Check forbidden restore"

test_12f() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f || error "release of $f failed"
	$LFS hsm_restore $f
	wait_request_state $fid RESTORE SUCCEED

	echo -n "Verifying file state: "
	check_hsm_flags $f "0x00000009"

	diff -q /etc/hosts $f

	[[ $? -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 12f "Restore a released file explicitly"

test_12g() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f || error "release of $f failed"

	diff -q /etc/hosts $f
	local st=$?

	# we check we had a restore done
	wait_request_state $fid RESTORE SUCCEED

	[[ $st -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 12g "Restore a released file implicitly"

test_12h() {
	needclients 2 || return 0

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f || error "release of $f failed"

	do_node $CLIENT2 diff -q /etc/hosts $f
	local st=$?

	# we check we had a restore done
	wait_request_state $fid RESTORE SUCCEED

	[[ $st -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 12h "Restore a released file implicitly from a second node"

test_12m() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)
	$LFS hsm_archive $f || error "archive of $f failed"
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_release $f || error "release of $f failed"

	cmp /etc/passwd $f

	[[ $? -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 12m "Archive/release/implicit restore"

test_12n() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	copy2archive /etc/hosts $tdir/$tfile

	local f=$DIR/$tdir/$tfile
	import_file $tdir/$tfile $f

	do_facet $SINGLEAGT cmp /etc/hosts $f ||
		error "Restored file differs"

	$LFS hsm_release $f || error "release of $f failed"

	copytool_cleanup
}
run_test 12n "Import/implicit restore/release"

test_12o() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f || error "release of $f failed"

#define OBD_FAIL_MDS_HSM_SWAP_LAYOUTS		0x152
	do_facet $SINGLEMDS lctl set_param fail_loc=0x152

	# set no retry action mode
	cdt_set_no_retry

	diff -q /etc/hosts $f
	local st=$?

	# we check we had a restore failure
	wait_request_state $fid RESTORE FAILED

	[[ $st -eq 0 ]] && error "Restore must fail"

	# remove no retry action mode
	cdt_clear_no_retry

	# check file is still released
	check_hsm_flags $f "0x0000000d"

	# retry w/o failure injection
	do_facet $SINGLEMDS lctl set_param fail_loc=0

	# to be sure previous RESTORE result is gone
	cdt_purge
	wait_for_grace_delay

	diff -q /etc/hosts $f
	st=$?

	# we check we had a restore done
	wait_request_state $fid RESTORE SUCCEED

	[[ $st -eq 0 ]] || error "Restored file differs"

	copytool_cleanup
}
run_test 12o "Layout-swap failure during Restore leaves file released"

test_12p() {
	# test needs a running copytool
	copytool_setup

	mkdir $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	do_facet $SINGLEAGT cat $f > /dev/null || error "cannot cat $f"
	$LFS hsm_release $f || error "cannot release $f"
	do_facet $SINGLEAGT cat $f > /dev/null || error "cannot cat $f"
	$LFS hsm_release $f || error "cannot release $f"
	do_facet $SINGLEAGT cat $f > /dev/null || error "cannot cat $f"

	copytool_cleanup
}
run_test 12p "implicit restore of a file on copytool mount point"

cleanup_test_12q() {
	trap 0
	zconf_umount $(facet_host $SINGLEAGT) $MOUNT3 ||
		error "cannot umount $MOUNT3 on $SINGLEAGT"
}

test_12q() {
	[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.7.58) ] &&
		skip "need MDS version at least 2.7.58" && return 0

	zconf_mount $(facet_host $SINGLEAGT) $MOUNT3 ||
		error "cannot mount $MOUNT3 on $SINGLEAGT"

	trap cleanup_test_12q EXIT

	# test needs a running copytool
	copytool_setup $SINGLEAGT $MOUNT3

	mkdir $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local f2=$DIR2/$tdir/$tfile
	local fid=$(make_small $f)
	local orig_size=$(stat -c "%s" $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_release $f || error "could not release file"
	check_hsm_flags $f "0x0000000d"

	kill_copytools
	wait_copytools || error "copytool failed to stop"

	cat $f > /dev/null &

	# wait a bit to allow implicit restore request to be handled.
	# if not, next stat would also block on layout-lock.
	sleep 5

	local size=$(stat -c "%s" $f2)
	[ $size -eq $orig_size ] ||
		error "$f2: wrong size after archive: $size != $orig_size"

	HSM_ARCHIVE_PURGE=false copytool_setup $SINGLEAGT /mnt/lustre3

	wait

	size=$(stat -c "%s" $f)
	[ $size -eq $orig_size ] ||
		error "$f: wrong size after restore: $size != $orig_size"

	size=$(stat -c "%s" $f2)
	[ $size -eq $orig_size ] ||
		error "$f2: wrong size after restore: $size != $orig_size"

	:>$f

	size=$(stat -c "%s" $f)
	[ $size -eq 0 ] ||
		error "$f: wrong size after overwrite: $size != 0"

	size=$(stat -c "%s" $f2)
	[ $size -eq 0 ] ||
		error "$f2: wrong size after overwrite: $size != 0"

	copytool_cleanup
	zconf_umount $(facet_host $SINGLEAGT) $MOUNT3 ||
		error "cannot umount $MOUNT3 on $SINGLEAGT"
}
run_test 12q "file attributes are refreshed after restore"

test_13() {
	# test needs a running copytool
	copytool_setup

	local ARC_SUBDIR="import.orig"
	local d=""
	local f=""

	# populate directory to be imported
	for d in $(seq 1 10); do
		local CURR_DIR="$HSM_ARCHIVE/$ARC_SUBDIR/dir.$d"
		do_facet $SINGLEAGT mkdir -p "$CURR_DIR"
		for f in $(seq 1 10); do
			CURR_FILE="$CURR_DIR/$tfile.$f"
			# write file-specific data
			do_facet $SINGLEAGT \
				"echo d=$d, f=$f, dir=$CURR_DIR, "\
					"file=$CURR_FILE > $CURR_FILE"
		done
	done
	# import to Lustre
	import_file "$ARC_SUBDIR" $DIR/$tdir
	# diff lustre content and origin (triggers file restoration)
	# there must be 10x10 identical files, and no difference
	local cnt_ok=$(do_facet $SINGLEAGT diff -rs $HSM_ARCHIVE/$ARC_SUBDIR \
		       $DIR/$tdir/$ARC_SUBDIR | grep identical | wc -l)
	local cnt_diff=$(do_facet $SINGLEAGT diff -r $HSM_ARCHIVE/$ARC_SUBDIR \
			 $DIR/$tdir/$ARC_SUBDIR | wc -l)

	[ $cnt_diff -eq 0 ] ||
		error "$cnt_diff imported files differ from read data"
	[ $cnt_ok -eq 100 ] ||
		error "not enough identical files ($cnt_ok != 100)"

	copytool_cleanup
}
run_test 13 "Recursively import and restore a directory"

test_14() {
	# test needs a running copytool
	copytool_setup

	# archive a file
	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(make_small $f)
	local sum=$(md5sum $f | awk '{print $1}')
	$LFS hsm_archive $f || error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED

	# delete the file
	rm -f $f
	# create released file (simulate llapi_hsm_import call)
	touch $f
	local fid2=$(path2fid $f)
	$LFS hsm_set --archived --exists $f || error "could not force hsm flags"
	$LFS hsm_release $f || error "could not release file"

	# rebind the archive to the newly created file
	echo "rebind $fid to $fid2"

	do_facet $SINGLEAGT \
		"$HSMTOOL --archive $HSM_ARCHIVE_NUMBER --hsm-root $HSM_ARCHIVE\
		 --rebind $fid $fid2 $DIR" || error "could not rebind file"

	# restore file and compare md5sum
	local sum2=$(md5sum $f | awk '{print $1}')

	[[ $sum == $sum2 ]] || error "md5sum mismatch after restore"

	copytool_cleanup
}
run_test 14 "Rebind archived file to a new fid"

test_15() {
	# test needs a running copytool
	copytool_setup

	# archive files
	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local count=5
	local tmpfile=$SHARED_DIRECTORY/tmp.$$

	local fids=()
	local sums=()
	for i in $(seq 1 $count); do
		fids[$i]=$(make_small $f.$i)
		sums[$i]=$(md5sum $f.$i | awk '{print $1}')
		$LFS hsm_archive $f.$i || error "could not archive file"
	done
	wait_all_done $(($count*60))

	:>$tmpfile
	# delete the files
	for i in $(seq 1 $count); do
		rm -f $f.$i
		touch $f.$i
		local fid2=$(path2fid $f.$i)
		# add the rebind operation to the list
		echo ${fids[$i]} $fid2 >> $tmpfile

		# set it released (simulate llapi_hsm_import call)
		$LFS hsm_set --archived --exists $f.$i ||
			error "could not force hsm flags"
		$LFS hsm_release $f.$i || error "could not release file"
	done
	nl=$(wc -l < $tmpfile)
	[[ $nl == $count ]] || error "$nl files in list, $count expected"

	echo "rebind list of files"
	do_facet $SINGLEAGT \
		"$HSMTOOL --archive $HSM_ARCHIVE_NUMBER --hsm-root $HSM_ARCHIVE\
		 --rebind $tmpfile $DIR" || error "could not rebind file list"

	# restore files and compare md5sum
	for i in $(seq 1 $count); do
		local sum2=$(md5sum $f.$i | awk '{print $1}')
		[[ $sum2 == ${sums[$i]} ]] ||
		    error "md5sum mismatch after restore ($sum2 != ${sums[$i]})"
	done

	rm -f $tmpfile
	copytool_cleanup
}
run_test 15 "Rebind a list of files"

test_16() {
	# test needs a running copytool
	copytool_setup

	local ref=/tmp/ref
	# create a known size file so we can verify transfer speed
	# 20 MB <-> 20s
	local goal=20
	dd if=/dev/zero of=$ref bs=1M count=20

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file $ref $f)
	rm $ref
	local start=$(date +%s)
	$LFS hsm_archive $f
	wait_request_state $fid ARCHIVE SUCCEED
	local end=$(date +%s)
	local duration=$((end - start))

	[[ $duration -ge $goal ]] ||
		error "Transfer is too fast $duration < $goal"

	copytool_cleanup
}
run_test 16 "Test CT bandwith control option"

test_20() {
	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	touch $f || error "touch $f failed"

	# Could not release a non-archived file
	$LFS hsm_release $f && error "release should not succeed"

	# For following tests, we must test them with HS_ARCHIVED set
	$LFS hsm_set --exists --archived $f || error "could not add flag"

	# Could not release a file if no-release is set
	$LFS hsm_set --norelease $f || error "could not add flag"
	$LFS hsm_release $f && error "release should not succeed"
	$LFS hsm_clear --norelease $f || error "could not remove flag"

	# Could not release a file if lost
	$LFS hsm_set --lost $f || error "could not add flag"
	$LFS hsm_release $f && error "release should not succeed"
	$LFS hsm_clear --lost $f || error "could not remove flag"

	# Could not release a file if dirty
	$LFS hsm_set --dirty $f || error "could not add flag"
	$LFS hsm_release $f && error "release should not succeed"
	$LFS hsm_clear --dirty $f || error "could not remove flag"
}
run_test 20 "Release is not permitted"

test_21() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/test_release

	# Create a file and check its states
	local fid=$(make_small $f)
	check_hsm_flags $f "0x00000000"

	# LU-4388/LU-4389 - ZFS does not report full number of blocks
	# used until file is flushed to disk
	if [  $(facet_fstype ost1) == "zfs" ]; then
	    # this causes an OST_SYNC rpc to be sent
	    dd if=/dev/zero of=$f bs=512 count=1 oflag=sync conv=notrunc,fsync
	    # clear locks to reread file data
	    cancel_lru_locks osc
	fi

	local orig_size=$(stat -c "%s" $f)
	local orig_blocks=$(stat -c "%b" $f)

	start_full_debug_logging

	$LFS hsm_archive $f || error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED

	local blocks=$(stat -c "%b" $f)
	[ $blocks -eq $orig_blocks ] ||
		error "$f: wrong block number after archive: " \
		      "$blocks != $orig_blocks"
	local size=$(stat -c "%s" $f)
	[ $size -eq $orig_size ] ||
		error "$f: wrong size after archive: $size != $orig_size"

	# Release and check states
	$LFS hsm_release $f || error "could not release file"
	check_hsm_flags $f "0x0000000d"

	blocks=$(stat -c "%b" $f)
	[ $blocks -gt 5 ] &&
		error "$f: too many blocks after release: $blocks > 5"
	size=$(stat -c "%s" $f)
	[ $size -ne $orig_size ] &&
		error "$f: wrong size after release: $size != $orig_size"

	# Check we can release an file without stripe info
	f=$f.nolov
	$MCREATE $f
	fid=$(path2fid $f)
	check_hsm_flags $f "0x00000000"
	$LFS hsm_archive $f || error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED

	# Release and check states
	$LFS hsm_release $f || error "could not release file"
	check_hsm_flags $f "0x0000000d"

	# Release again a file that is already released is OK
	$LFS hsm_release $f || fail "second release should succeed"
	check_hsm_flags $f "0x0000000d"

	stop_full_debug_logging

	copytool_cleanup
}
run_test 21 "Simple release tests"

test_22() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/test_release
	local swap=$DIR/$tdir/test_swap

	# Create a file and check its states
	local fid=$(make_small $f)
	check_hsm_flags $f "0x00000000"

	$LFS hsm_archive $f || error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED

	# Release and check states
	$LFS hsm_release $f || error "could not release file"
	check_hsm_flags $f "0x0000000d"

	make_small $swap
	$LFS swap_layouts $swap $f && error "swap_layouts should failed"

	true
	copytool_cleanup
}
run_test 22 "Could not swap a release file"

test_23() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/test_mtime

	# Create a file and check its states
	local fid=$(make_small $f)
	check_hsm_flags $f "0x00000000"

	$LFS hsm_archive $f || error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED

	# Set modification time in the past
	touch -m -a -d @978261179 $f

	# Release and check states
	$LFS hsm_release $f || error "could not release file"
	check_hsm_flags $f "0x0000000d"

	local MTIME=$(stat -c "%Y" $f)
	local ATIME=$(stat -c "%X" $f)
	[ $MTIME -eq "978261179" ] || fail "bad mtime: $MTIME"
	[ $ATIME -eq "978261179" ] || fail "bad atime: $ATIME"

	copytool_cleanup
}
run_test 23 "Release does not change a/mtime (utime)"

test_24a() {
	local file=$DIR/$tdir/$tfile
	local fid
	local atime0
	local atime1
	local mtime0
	local mtime1
	local ctime0
	local ctime1

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	rm -f $file
	fid=$(make_small $file)

	# Create a file and check its states
	check_hsm_flags $file "0x00000000"

	# Ensure atime is less than mtime and ctime.
	sleep 1
	echo >> $file

	atime0=$(stat -c "%X" $file)
	mtime0=$(stat -c "%Y" $file)
	ctime0=$(stat -c "%Z" $file)

	[ $atime0 -lt $mtime0 ] ||
		error "atime $atime0 is not less than mtime $mtime0"

	[ $atime0 -lt $ctime0 ] ||
		error "atime $atime0 is not less than ctime $ctime0"

	# Archive should not change any timestamps.
	$LFS hsm_archive $file || error "cannot archive '$file'"
	wait_request_state $fid ARCHIVE SUCCEED

	atime1=$(stat -c "%X" $file)
	mtime1=$(stat -c "%Y" $file)
	ctime1=$(stat -c "%Z" $file)

	[ $atime0 -eq $atime1 ] ||
		error "archive changed atime from $atime0 to $atime1"

	[ $mtime0 -eq $mtime1 ] ||
		error "archive changed mtime from $mtime0 to $mtime1"

	[ $ctime0 -eq $ctime1 ] ||
		error "archive changed ctime from $ctime0 to $ctime1"

	# Release should not change any timestamps.
	$LFS hsm_release $file || error "cannot release '$file'"
	check_hsm_flags $file "0x0000000d"

	atime1=$(stat -c "%X" $file)
	mtime1=$(stat -c "%Y" $file)
	ctime1=$(stat -c "%Z" $file)

	[ $atime0 -eq $atime1 ] ||
		error "release changed atime from $atime0 to $atime1"

	[ $mtime0 -eq $mtime1 ] ||
		error "release changed mtime from $mtime0 to $mtime1"

	[ $ctime0 -eq $ctime1 ] ||
		error "release changed ctime from $ctime0 to $ctime1"

	# Restore should not change any timestamps.
	$LFS hsm_restore $file
	wait_request_state $fid RESTORE SUCCEED

	atime1=$(stat -c "%X" $file)
	mtime1=$(stat -c "%Y" $file)
	ctime1=$(stat -c "%Z" $file)

	[ $atime0 -eq $atime1 ] ||
		error "restore changed atime from $atime0 to $atime1"

	[ $mtime0 -eq $mtime1 ] ||
		error "restore changed mtime from $mtime0 to $mtime1"

	[ $ctime0 -eq $ctime1 ] ||
		error "restore changed ctime from $ctime0 to $ctime1"

	copytool_cleanup

	# Once more, after unmount and mount.
	umount_client $MOUNT || error "cannot unmount '$MOUNT'"
	mount_client $MOUNT || error "cannot mount '$MOUNT'"

	atime1=$(stat -c "%X" $file)
	mtime1=$(stat -c "%Y" $file)
	ctime1=$(stat -c "%Z" $file)

	[ $atime0 -eq $atime1 ] ||
		error "remount changed atime from $atime0 to $atime1"

	[ $mtime0 -eq $mtime1 ] ||
		error "remount changed mtime from $mtime0 to $mtime1"

	[ $ctime0 -eq $ctime1 ] ||
		error "remount changed ctime from $ctime0 to $ctime1"
}
run_test 24a "Archive, release, and restore does not change a/mtime (i/o)"

test_24b() {
	local file=$DIR/$tdir/$tfile
	local fid
	local sum0
	local sum1
	# LU-3811

	# Test needs a running copytool.
	copytool_setup
	mkdir -p $DIR/$tdir

	# Check that root can do HSM actions on a regular user's file.
	rm -f $file
	fid=$(make_small $file)
	sum0=$(md5sum $file)

	chown $RUNAS_ID:$RUNAS_GID $file ||
		error "cannot chown '$file' to '$RUNAS_ID'"

	chmod ugo-w $DIR/$tdir ||
		error "cannot chmod '$DIR/$tdir'"

	$LFS hsm_archive $file
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_release $file
	check_hsm_flags $file "0x0000000d"

	$LFS hsm_restore $file
	wait_request_state $fid RESTORE SUCCEED

	# Check that ordinary user can get HSM state.
	$RUNAS $LFS hsm_state $file ||
		error "user '$RUNAS_ID' cannot get HSM state of '$file'"

	$LFS hsm_release $file
	check_hsm_flags $file "0x0000000d"

	# Check that ordinary user can accessed released file.
	sum1=$($RUNAS md5sum $file) ||
		error "user '$RUNAS_ID' cannot read '$file'"

	[ "$sum0" == "$sum1" ] ||
		error "md5sum mismatch for '$file'"

	copytool_cleanup
}
run_test 24b "root can archive, release, and restore user files"

cleanup_test_24c() {
	trap 0
	set_hsm_param user_request_mask RESTORE
	set_hsm_param group_request_mask RESTORE
	set_hsm_param other_request_mask RESTORE
}

test_24c() {
	local file=$DIR/$tdir/$tfile
	local action=archive
	local user_save
	local group_save
	local other_save

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	# Save the default masks and check that cleanup_24c will
	# restore the request masks correctly.
	user_save=$(get_hsm_param user_request_mask)
	group_save=$(get_hsm_param group_request_mask)
	other_save=$(get_hsm_param other_request_mask)

	[ "$user_save" == RESTORE ] ||
		error "user_request_mask is '$user_save' expected 'RESTORE'"
	[ "$group_save" == RESTORE ] ||
		error "group_request_mask is '$group_save' expected 'RESTORE'"
	[ "$other_save" == RESTORE ] ||
		error "other_request_mask is '$other_save' expected 'RESTORE'"

	trap cleanup_test_24c EXIT

	# User.
	rm -f $file
	make_small $file
	chown $RUNAS_ID:nobody $file ||
		error "cannot chown '$file' to '$RUNAS_ID:nobody'"

	set_hsm_param user_request_mask ""
	$RUNAS $LFS hsm_$action $file &&
		error "$action by user should fail"

	set_hsm_param user_request_mask $action
	$RUNAS $LFS hsm_$action $file ||
		error "$action by user should succeed"

	# Group.
	rm -f $file
	make_small $file
	chown nobody:$RUNAS_GID $file ||
		error "cannot chown '$file' to 'nobody:$RUNAS_GID'"

	set_hsm_param group_request_mask ""
	$RUNAS $LFS hsm_$action $file &&
		error "$action by group should fail"

	set_hsm_param group_request_mask $action
	$RUNAS $LFS hsm_$action $file ||
		error "$action by group should succeed"

	# Other.
	rm -f $file
	make_small $file
	chown nobody:nobody $file ||
		error "cannot chown '$file' to 'nobody:nobody'"

	set_hsm_param other_request_mask ""
	$RUNAS $LFS hsm_$action $file &&
		error "$action by other should fail"

	set_hsm_param other_request_mask $action
	$RUNAS $LFS hsm_$action $file ||
		error "$action by other should succeed"

	copytool_cleanup
	cleanup_test_24c
}
run_test 24c "check that user,group,other request masks work"

cleanup_test_24d() {
	trap 0
	mount -o remount,rw $MOUNT2
}

test_24d() {
	local file1=$DIR/$tdir/$tfile
	local file2=$DIR2/$tdir/$tfile
	local fid1
	local fid2

	copytool_setup

	mkdir -p $DIR/$tdir
	rm -f $file1
	fid1=$(make_small $file1)

	trap cleanup_test_24d EXIT

	mount -o remount,ro $MOUNT2

	fid2=$(path2fid $file2)
	[ "$fid1" == "$fid2" ] ||
		error "FID mismatch '$fid1' != '$fid2'"

	$LFS hsm_archive $file2 &&
		error "archive should fail on read-only mount"
	check_hsm_flags $file1 "0x00000000"

	$LFS hsm_archive $file1
	wait_request_state $fid1 ARCHIVE SUCCEED

	$LFS hsm_release $file1
	$LFS hsm_restore $file2
	wait_request_state $fid1 RESTORE SUCCEED

	$LFS hsm_release $file1 || error "cannot release '$file1'"
	dd if=$file2 of=/dev/null bs=1M || "cannot read '$file2'"

	$LFS hsm_release $file2 &&
		error "release should fail on read-only mount"

	copytool_cleanup
	cleanup_test_24d
}
run_test 24d "check that read-only mounts are respected"

test_24e() {
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid

	fid=$(make_small $f) || error "cannot create $f"
	$LFS hsm_archive $f || error "cannot archive $f"
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f || error "cannot release $f"
	while ! $LFS hsm_state $f | grep released; do
		sleep 1
	done

	tar -cf $TMP/$tfile.tar $DIR/$tdir || error "cannot tar $DIR/$tdir"

	copytool_cleanup
}
run_test 24e "tar succeeds on HSM released files" # LU-6213

test_24f() {

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir/d1
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/hosts $f)
	sum0=$(md5sum $f)
	echo $sum0
	$LFS hsm_archive -a $HSM_ARCHIVE_NUMBER $f ||
		error "hsm_archive failed"
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f || error "cannot release $f"
	tar --xattrs -cvf $f.tar -C $DIR/$tdir $tfile
	rm -f $f
	sync
	tar --xattrs -xvf $f.tar -C $DIR/$tdir ||
		error "Can not recover the tar contents"
	sum1=$(md5sum $f)
	echo "Sum0 = $sum0, sum1 = $sum1"
	[ "$sum0" == "$sum1" ] || error "md5sum mismatch for '$tfile'"

	copytool_cleanup
}
run_test 24f "root can archive, release, and restore tar files"

test_25a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	copy2archive /etc/hosts $tdir/$tfile

	local f=$DIR/$tdir/$tfile

	import_file $tdir/$tfile $f

	$LFS hsm_set --lost $f

	md5sum $f
	local st=$?

	[[ $st == 1 ]] || error "lost file access should failed (returns $st)"

	copytool_cleanup
}
run_test 25a "Restore lost file (HS_LOST flag) from import"\
	     " (Operation not permitted)"

test_25b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_release $f
	$LFS hsm_set --lost $f
	md5sum $f
	st=$?

	[[ $st == 1 ]] || error "lost file access should failed (returns $st)"

	copytool_cleanup
}
run_test 25b "Restore lost file (HS_LOST flag) after release"\
	     " (Operation not permitted)"

test_26() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_remove $f
	wait_request_state $fid REMOVE SUCCEED

	check_hsm_flags $f "0x00000000"

	copytool_cleanup
}
run_test 26 "Remove the archive of a valid file"

test_27a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	make_archive $tdir/$tfile
	local f=$DIR/$tdir/$tfile
	import_file $tdir/$tfile $f
	local fid=$(path2fid $f)

	$LFS hsm_remove $f

	[[ $? != 0 ]] || error "Remove of a released file should fail"

	copytool_cleanup
}
run_test 27a "Remove the archive of an imported file (Operation not permitted)"

test_27b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	$LFS hsm_remove $f

	[[ $? != 0 ]] || error "Remove of a released file should fail"

	copytool_cleanup
}
run_test 27b "Remove the archive of a relased file (Operation not permitted)"

test_28() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	cdt_disable
	$LFS hsm_remove $f

	rm -f $f

	cdt_enable

	wait_request_state $fid REMOVE SUCCEED

	copytool_cleanup
}
run_test 28 "Concurrent archive/file remove"

test_29a() {
	# Tests --mntpath and --archive options

	local archive_id=7
	copytool_setup $SINGLEAGT $MOUNT $archive_id

	# Bad archive number
	$LFS hsm_remove -m $MOUNT -a 33 0x857765760:0x8:0x2 2>&1 |
		grep "Invalid argument" ||
		error "unexpected hsm_remove failure (1)"

	# mntpath is present but file is given
	$LFS hsm_remove --mntpath $MOUNT --archive 30 /qwerty/uyt 2>&1 |
		grep "hsm: '/qwerty/uyt' is not a valid FID" ||
		error "unexpected hsm_remove failure (2)"

	copytool_cleanup
}
run_test 29a "Tests --mntpath and --archive options"

test_29b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(make_small $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	rm -f $f

	$LFS hsm_remove -m $MOUNT -a $HSM_ARCHIVE_NUMBER $fid
	wait_request_state $fid REMOVE SUCCEED

	copytool_cleanup
}
run_test 29b "Archive/delete/remove by FID from the archive."

test_29c() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local fid1=$(make_small $DIR/$tdir/$tfile-1)
	local fid2=$(make_small $DIR/$tdir/$tfile-2)
	local fid3=$(make_small $DIR/$tdir/$tfile-3)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $DIR/$tdir/$tfile-[1-3]
	wait_request_state $fid1 ARCHIVE SUCCEED
	wait_request_state $fid2 ARCHIVE SUCCEED
	wait_request_state $fid3 ARCHIVE SUCCEED

	rm -f $DIR/$tdir/$tfile-[1-3]

	echo $fid1 > $DIR/$tdir/list
	echo $fid2 >> $DIR/$tdir/list
	echo $fid3 >> $DIR/$tdir/list

	$LFS hsm_remove -m $MOUNT -a $HSM_ARCHIVE_NUMBER \
		--filelist $DIR/$tdir/list
	wait_request_state $fid1 REMOVE SUCCEED
	wait_request_state $fid2 REMOVE SUCCEED
	wait_request_state $fid3 REMOVE SUCCEED

	copytool_cleanup
}
run_test 29c "Archive/delete/remove by FID, using a file list."

test_30a() {
	# restore at exec cannot work on agent node (because of Linux kernel
	# protection of executables)
	needclients 2 || return 0

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	copy2archive /bin/true $tdir/$tfile

	local f=$DIR/$tdir/true
	import_file $tdir/$tfile $f

	local fid=$(path2fid $f)

	# set no retry action mode
	cdt_set_no_retry
	do_node $CLIENT2 $f
	local st=$?

	# cleanup
	# remove no try action mode
	cdt_clear_no_retry
	$LFS hsm_state $f

	[[ $st == 0 ]] || error "Failed to exec a released file"

	copytool_cleanup
}
run_test 30a "Restore at exec (import case)"

test_30b() {
	# restore at exec cannot work on agent node (because of Linux kernel
	# protection of executables)
	needclients 2 || return 0

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/true
	local fid=$(copy_file /bin/true $f)
	chmod 755 $f
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f
	$LFS hsm_state $f
	# set no retry action mode
	cdt_set_no_retry
	do_node $CLIENT2 $f
	local st=$?

	# cleanup
	# remove no try action mode
	cdt_clear_no_retry
	$LFS hsm_state $f

	[[ $st == 0 ]] || error "Failed to exec a released file"

	copytool_cleanup
}
run_test 30b "Restore at exec (release case)"

test_30c() {
	needclients 2 || return 0

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/SLEEP
	local slp_sum1=$(md5sum /bin/sleep)
	local fid=$(copy_file /bin/sleep $f)
	chmod 755 $f
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f
	check_hsm_flags $f "0x0000000d"
	# set no retry action mode
	cdt_set_no_retry
	do_node $CLIENT2 "$f 10" &
	local pid=$!
	sleep 3
	echo 'Hi!' > $f
	[[ $? == 0 ]] && error "Update during exec of released file must fail"
	wait $pid
	[[ $? == 0 ]] || error "Execution failed during run"
	cmp /bin/sleep $f
	if [[ $? != 0 ]]; then
		local slp_sum2=$(md5sum /bin/sleep)
		# in case sleep file is modified during the test
		[[ $slp_sum1 == $slp_sum2 ]] &&
			error "Binary overwritten during exec"
	fi

	# cleanup
	# remove no try action mode
	cdt_clear_no_retry
	check_hsm_flags $f "0x00000009"

	copytool_cleanup
}
run_test 30c "Update during exec of released file must fail"

restore_and_check_size() {
	local f=$1
	local fid=$2
	local s=$(stat -c "%s" $f)
	local n=$s
	local st=$(get_hsm_flags $f)
	local err=0
	local cpt=0
	$LFS hsm_restore $f
	while [[ "$st" != "0x00000009" && $cpt -le 10 ]]
	do
		n=$(stat -c "%s" $f)
		# we echo in both cases to show stat is not
		# hang
		if [[ $n != $s ]]; then
			echo "size seen is $n != $s"
			err=1
		else
			echo "size seen is right: $n == $s"
		fi
		st=$(get_hsm_flags $f)
		sleep 10
		cpt=$((cpt + 1))
	done
	if [[ $cpt -lt 10 ]]; then
		echo " "done
	else
		echo " restore is too long"
		wait_request_state $fid RESTORE SUCCEED
	fi
	return $err
}

test_31a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	make_archive $tdir/$tfile
	local f=$DIR/$tdir/$tfile
	import_file $tdir/$tfile $f
	local fid=$($LFS path2fid $f)
	HSM_ARCHIVE_PURGE=false copytool_setup

	restore_and_check_size $f $fid
	local err=$?

	[[ $err -eq 0 ]] || error "File size changed during restore"

	copytool_cleanup
}
run_test 31a "Import a large file and check size during restore"


test_31b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	restore_and_check_size $f $fid
	local err=$?

	[[ $err -eq 0 ]] || error "File size changed during restore"

	copytool_cleanup
}
run_test 31b "Restore a large unaligned file and check size during restore"

test_31c() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress_aligned $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	restore_and_check_size $f $fid
	local err=$?

	[[ $err -eq 0 ]] || error "File size changed during restore"

	copytool_cleanup
}
run_test 31c "Restore a large aligned file and check size during restore"

test_33() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	# to be sure wait_all_done will not be mislead by previous tests
	# and ops.
	cdt_purge
	wait_for_grace_delay
	# Also raise grace_delay significantly so the Canceled
	# Restore action will stay enough long avail.
	local old_grace=$(get_hsm_param grace_delay)
	set_hsm_param grace_delay 100

	md5sum $f >/dev/null &
	local pid=$!
	wait_request_state $fid RESTORE STARTED

	kill -15 $pid
	sleep 1

	# Check restore trigger process was killed
	local killed=$(ps -o pid,comm hp $pid >/dev/null)

	$LFS hsm_cancel $f

	# instead of waiting+checking both Restore and Cancel ops
	# sequentially, wait for both to be finished and then check
	# each results.
	wait_all_done 100 $fid
	local rstate=$(get_request_state $fid RESTORE)
	local cstate=$(get_request_state $fid CANCEL)

	# restore orig grace_delay.
	set_hsm_param grace_delay $old_grace

	if [[ "$rstate" == "CANCELED" ]] ; then
		[[ "$cstate" == "SUCCEED" ]] ||
			error "Restore state is CANCELED and Cancel state " \
			       "is not SUCCEED but $cstate"
		echo "Restore state is CANCELED, Cancel state is SUCCEED"
	elif [[ "$rstate" == "SUCCEED" ]] ; then
		[[ "$cstate" == "FAILED" ]] ||
			error "Restore state is SUCCEED and Cancel state " \
				"is not FAILED but $cstate"
		echo "Restore state is SUCCEED, Cancel state is FAILED"
	else
		error "Restore state is $rstate and Cancel state is $cstate"
	fi

	[ -z $killed ] ||
		error "Cannot kill process waiting for restore ($killed)"

	copytool_cleanup
}
run_test 33 "Kill a restore waiting process"

test_34() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	md5sum $f >/dev/null &
	local pid=$!
	wait_request_state $fid RESTORE STARTED

	rm $f || error "rm $f failed"
	# rm must not block during restore
	wait_request_state $fid RESTORE STARTED

	wait_request_state $fid RESTORE SUCCEED
	# check md5sum pgm finished
	local there=$(ps -o pid,comm hp $pid >/dev/null)
	[[ -z $there ]] || error "Restore initiator does not exit"

	local rc=$(wait $pid)
	[[ $rc -eq 0 ]] || error "Restore initiator failed with $rc"

	copytool_cleanup
}
run_test 34 "Remove file during restore"

test_35() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local f1=$DIR/$tdir/$tfile-1
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	local fid1=$(copy_file /etc/passwd $f1)
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	md5sum $f >/dev/null &
	local pid=$!
	wait_request_state $fid RESTORE STARTED

	mv $f1 $f || error "mv $f1 $f failed"
	# mv must not block during restore
	wait_request_state $fid RESTORE STARTED

	wait_request_state $fid RESTORE SUCCEED
	# check md5sum pgm finished
	local there=$(ps -o pid,comm hp $pid >/dev/null)
	[[ -z $there ]] || error "Restore initiator does not exit"

	local rc=$(wait $pid)
	[[ $rc -eq 0 ]] || error "Restore initiator failed with $rc"

	fid2=$(path2fid $f)
	[[ $fid2 == $fid1 ]] || error "Wrong fid after mv $fid2 != $fid1"

	copytool_cleanup
}
run_test 35 "Overwrite file during restore"

test_36() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	md5sum $f >/dev/null &
	local pid=$!
	wait_request_state $fid RESTORE STARTED

	mv $f $f.new
	# rm must not block during restore
	wait_request_state $fid RESTORE STARTED

	wait_request_state $fid RESTORE SUCCEED
	# check md5sum pgm finished
	local there=$(ps -o pid,comm hp $pid >/dev/null)
	[[ -z $there ]] ||
		error "Restore initiator does not exit"

	local rc=$(wait $pid)
	[[ $rc -eq 0 ]] ||
		error "Restore initiator failed with $rc"

	copytool_cleanup
}
run_test 36 "Move file during restore"

test_37() {
	# LU-5683: check that an archived dirty file can be rearchived.
	copytool_cleanup
	copytool_setup $SINGLEAGT $MOUNT2

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid

	fid=$(make_small $f) || error "cannot create small file"

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f || error "cannot release $f"

	# Dirty file.
	dd if=/dev/urandom of=$f bs=1M count=1 || error "cannot dirty file"

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	copytool_cleanup
}
run_test 37 "re-archive a dirty file"

multi_archive() {
	local prefix=$1
	local count=$2
	local n=""

	for n in $(seq 1 $count); do
		$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $prefix.$n
	done
	echo "$count archive requests submitted"
}

test_40() {
	local stream_count=4
	local file_count=100
	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local i=""
	local p=""
	local fid=""

	for i in $(seq 1 $file_count); do
		for p in $(seq 1 $stream_count); do
			fid=$(copy_file /etc/hosts $f.$p.$i)
		done
	done
	# force copytool to use a local/temp archive dir to ensure best
	# performance vs remote/NFS mounts used in auto-tests
	if do_facet $SINGLEAGT "df --local $HSM_ARCHIVE" >/dev/null 2>&1 ; then
		copytool_setup
	else
		copytool_setup $SINGLEAGT $MOUNT $HSM_ARCHIVE_NUMBER $TMP/$tdir
	fi
	# to be sure wait_all_done will not be mislead by previous tests
	cdt_purge
	wait_for_grace_delay
	typeset -a pids
	# start archive streams in background (archive files in parallel)
	for p in $(seq 1 $stream_count); do
		multi_archive $f.$p $file_count &
		pids[$p]=$!
	done
	echo -n  "Wait for all requests being enqueued..."
	wait ${pids[*]}
	echo OK
	wait_all_done 100
	copytool_cleanup
}
run_test 40 "Parallel archive requests"

test_52() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/motd $f 1)

	$LFS hsm_archive $f || error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED
	check_hsm_flags $f "0x00000009"

	multiop_bg_pause $f O_c || error "multiop failed"
	local MULTIPID=$!

	mds_evict_client
	client_up || client_up || true

	kill -USR1 $MULTIPID
	wait $MULTIPID || error "multiop close failed"

	check_hsm_flags $f "0x0000000b"

	copytool_cleanup
}
run_test 52 "Opened for write file on an evicted client should be set dirty"

test_53() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/motd $f 1)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f ||
		error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED
	check_hsm_flags $f "0x00000009"

	multiop_bg_pause $f o_c || error "multiop failed"
	MULTIPID=$!

	mds_evict_client
	client_up || client_up || true

	kill -USR1 $MULTIPID
	wait $MULTIPID || error "multiop close failed"

	check_hsm_flags $f "0x00000009"

	copytool_cleanup
}
run_test 53 "Opened for read file on an evicted client should not be set dirty"

test_54() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(make_large_for_progress $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f ||
		error "could not archive file"
	wait_request_state $fid ARCHIVE STARTED

	check_hsm_flags $f "0x00000001"

	# Avoid coordinator resending this request as soon it has failed.
	cdt_set_no_retry

	echo "foo" >> $f
	sync
	wait_request_state $fid ARCHIVE FAILED

	check_hsm_flags $f "0x00000003"

	cdt_clear_no_retry
	copytool_cleanup
}
run_test 54 "Write during an archive cancels it"

test_55() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(make_large_for_progress $f)

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f ||
		error "could not archive file"
	wait_request_state $fid ARCHIVE STARTED

	check_hsm_flags $f "0x00000001"

	# Avoid coordinator resending this request as soon it has failed.
	cdt_set_no_retry

	$TRUNCATE $f 1024 || error "truncate failed"
	sync
	wait_request_state $fid ARCHIVE FAILED

	check_hsm_flags $f "0x00000003"

	cdt_clear_no_retry
	copytool_cleanup
}
run_test 55 "Truncate during an archive cancels it"

test_56() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f ||
		error "could not archive file"
	wait_request_state $fid ARCHIVE STARTED

	check_hsm_flags $f "0x00000001"

	# Change metadata and sync to be sure we are not changing only
	# in memory.
	chmod 644 $f
	chgrp sys $f
	sync
	wait_request_state $fid ARCHIVE SUCCEED

	check_hsm_flags $f "0x00000009"

	copytool_cleanup
}
run_test 56 "Setattr during an archive is ok"

test_57() {
	# Need one client for I/O, one for request
	needclients 2 || return 0

	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/test_archive_remote
	# Create a file on a remote node
	do_node $CLIENT2 "dd if=/dev/urandom of=$f bs=1M "\
		"count=2 conv=fsync"

	# And archive it
	do_node $CLIENT2 "$LFS hsm_archive -a $HSM_ARCHIVE_NUMBER $f" ||
		error "hsm_archive failed"
	local fid=$(path2fid $f)
	wait_request_state $fid ARCHIVE SUCCEED

	# Release and implicit restore it
	do_node $CLIENT2 "$LFS hsm_release $f" ||
		error "hsm_release failed"
	do_node $CLIENT2 "md5sum $f" ||
		error "hsm_restore failed"

	wait_request_state $fid RESTORE SUCCEED

	copytool_cleanup
}
run_test 57 "Archive a file with dirty cache on another node"

truncate_released_file() {
	local src_file=$1
	local trunc_to=$2

	local sz=$(stat -c %s $src_file)
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file $1 $f)
	local ref=$f-ref
	cp $f $f-ref

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f ||
		error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_release $f || error "could not release file"

	$TRUNCATE $f $trunc_to || error "truncate failed"
	sync

	local sz1=$(stat -c %s $f)
	[[ $sz1 == $trunc_to ]] ||
		error "size after trunc: $sz1 expect $trunc_to, original $sz"

	$LFS hsm_state $f
	check_hsm_flags $f "0x0000000b"

	local state=$(get_request_state $fid RESTORE)
	[[ "$state" == "SUCCEED" ]] ||
		error "truncate $sz does not trig restore, state = $state"

	$TRUNCATE $ref $trunc_to
	cmp $ref $f || error "file data wrong after truncate"

	rm -f $f $f-ref
}

test_58() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local sz=$(stat -c %s /etc/passwd)

	echo "truncate up from $sz to $((sz*2))"
	truncate_released_file /etc/passwd $((sz*2))

	echo "truncate down from $sz to $((sz/2))"
	truncate_released_file /etc/passwd $((sz/2))

	echo "truncate to 0"
	truncate_released_file /etc/passwd 0

	copytool_cleanup
}
run_test 58 "Truncate a released file will trigger restore"

test_59() {
	local fid
	local server_version=$(lustre_version_code $SINGLEMDS)
	[[ $server_version -lt $(version_code 2.7.63) ]] &&
		skip "Need MDS version at least 2.7.63" && return

	copytool_setup
	$MCREATE $DIR/$tfile || error "mcreate failed"
	$TRUNCATE $DIR/$tfile 42 || error "truncate failed"
	$LFS hsm_archive $DIR/$tfile || error "archive request failed"
	fid=$(path2fid $DIR/$tfile)
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $DIR/$tfile || error "release failed"
	copytool_cleanup
}
run_test 59 "Release stripeless file with non-zero size"

test_60() {
	# This test validates the fix for LU-4512. Ensure that the -u
	# option changes the progress reporting interval from the
	# default (30 seconds) to the user-specified interval.
	local interval=5
	local progress_timeout=$((interval * 4))

	# test needs a new running copytool
	copytool_cleanup
	HSMTOOL_UPDATE_INTERVAL=$interval copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	local mdtidx=0
	local mdt=${MDT_PREFIX}${mdtidx}
	local mds=mds$((mdtidx + 1))

	# Wait for copytool to register
	wait_update_facet $mds \
		"$LCTL get_param -n ${mdt}.hsm.agents | grep -o ^uuid" \
		uuid 100 || error "coyptool failed to register with $mdt"

	local start_at=$(date +%s)
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f ||
		error "could not archive file"

	local agent=$(facet_active_host $SINGLEAGT)
	local prefix=$TESTLOG_PREFIX
	[[ -z "$TESTNAME" ]] || prefix=$prefix.$TESTNAME
	local copytool_log=$prefix.copytool_log.$agent.log


	wait_update $agent \
	    "grep -o start.copy $copytool_log" "start copy" 100 ||
		error "copytool failed to start"

	local cmd="$LCTL get_param -n ${mdt}.hsm.active_requests"
	cmd+=" | awk '/'$fid'.*action=ARCHIVE/ {print \\\$12}' | cut -f2 -d="

	local RESULT
	local WAIT=0
	local sleep=1

	echo -n "Expecting a progress update within $progress_timeout seconds... "
	while [ true ]; do
		RESULT=$(do_node $(facet_active_host $mds) "$cmd")
		if [ $RESULT -gt 0 ]; then
			echo "$RESULT bytes copied in $WAIT seconds."
			break
		elif [ $WAIT -ge $progress_timeout ]; then
			error "Timed out waiting for progress update!"
			break
		fi
		WAIT=$((WAIT + sleep))
		sleep $sleep
	done

	local finish_at=$(date +%s)
	local elapsed=$((finish_at - start_at))

	# Ensure that the progress update occurred within the expected window.
	if [ $elapsed -lt $((interval - 1)) ]; then
		error "Expected progress update after at least $interval seconds"
	fi

	cdt_clear_no_retry
	copytool_cleanup
}
run_test 60 "Changing progress update interval from default"

test_70() {
	# test needs a new running copytool
	copytool_cleanup
	copytool_monitor_setup
	HSMTOOL_EVENT_FIFO=$HSMTOOL_MONITOR_DIR/fifo copytool_setup

	# Just start and stop the copytool to generate events.
	cdt_clear_no_retry

	# Wait for the copytool to register.
	wait_update --verbose $(facet_active_host mds1) \
		"$LCTL get_param -n ${MDT_PREFIX}0.hsm.agents | grep -o ^uuid" \
		uuid 100 ||
		error "copytool failed to register with MDT0000"

	copytool_cleanup

	local REGISTER_EVENT
	local UNREGISTER_EVENT
	while read event; do
		local parsed=$(parse_json_event "$event")
		if [ -z "$parsed" ]; then
			error "Copytool sent malformed event: $event"
		fi
		eval $parsed

		if [ $event_type == "REGISTER" ]; then
			REGISTER_EVENT=$event
		elif [ $event_type == "UNREGISTER" ]; then
			UNREGISTER_EVENT=$event
		fi
	done < <(echo $"$(get_copytool_event_log)")

	if [ -z "$REGISTER_EVENT" ]; then
		error "Copytool failed to send register event to FIFO"
	fi

	if [ -z "$UNREGISTER_EVENT" ]; then
		error "Copytool failed to send unregister event to FIFO"
	fi

	copytool_monitor_cleanup
	echo "Register/Unregister events look OK."
}
run_test 70 "Copytool logs JSON register/unregister events to FIFO"

test_71() {
	# Bump progress interval for livelier events.
	local interval=5

	# test needs a new running copytool
	copytool_cleanup
	copytool_monitor_setup
	HSMTOOL_UPDATE_INTERVAL=$interval \
	HSMTOOL_EVENT_FIFO=$HSMTOOL_MONITOR_DIR/fifo copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f ||
		error "could not archive file"
	wait_request_state $fid ARCHIVE SUCCEED

	local expected_fields="event_time data_fid source_fid"
	expected_fields+=" total_bytes current_bytes"

	local START_EVENT
	local FINISH_EVENT
	while read event; do
		# Make sure we're not getting anything from previous events.
		for field in $expected_fields; do
			unset $field
		done

		local parsed=$(parse_json_event "$event")
		if [ -z "$parsed" ]; then
			error "Copytool sent malformed event: $event"
		fi
		eval $parsed

		if [ $event_type == "ARCHIVE_START" ]; then
			START_EVENT=$event
			continue
		elif [ $event_type == "ARCHIVE_FINISH" ]; then
			FINISH_EVENT=$event
			continue
		elif [ $event_type != "ARCHIVE_RUNNING" ]; then
			continue
		fi

		# Do some simple checking of the progress update events.
		for expected_field in $expected_fields; do
			if [ -z ${!expected_field+x} ]; then
				error "Missing $expected_field field in event"
			fi
		done

		if [ $total_bytes -eq 0 ]; then
			error "Expected total_bytes to be > 0"
		fi

		# These should be identical throughout an archive
		# operation.
		if [ $source_fid != $data_fid ]; then
			error "Expected source_fid to equal data_fid"
		fi
	done < <(echo $"$(get_copytool_event_log)")

	if [ -z "$START_EVENT" ]; then
		error "Copytool failed to send archive start event to FIFO"
	fi

	if [ -z "$FINISH_EVENT" ]; then
		error "Copytool failed to send archive finish event to FIFO"
	fi

	echo "Archive events look OK."

	cdt_clear_no_retry
	copytool_cleanup
	copytool_monitor_cleanup
}
run_test 71 "Copytool logs JSON archive events to FIFO"

test_72() {
	# Bump progress interval for livelier events.
	local interval=5

	# test needs a new running copytool
	copytool_cleanup
	copytool_monitor_setup
	HSMTOOL_UPDATE_INTERVAL=$interval \
	HSMTOOL_EVENT_FIFO=$HSMTOOL_MONITOR_DIR/fifo copytool_setup
	local test_file=$HSMTOOL_MONITOR_DIR/file

	local cmd="dd if=/dev/urandom of=$test_file count=16 bs=1000000 "
	cmd+="conv=fsync"
	do_facet $SINGLEAGT "$cmd" ||
		error "cannot create $test_file on $SINGLEAGT"
	copy2archive $test_file $tdir/$tfile

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	import_file $tdir/$tfile $f
	f=$DIR2/$tdir/$tfile
	echo "Verifying released state: "
	check_hsm_flags $f "0x0000000d"

	local fid=$(path2fid $f)
	$LFS hsm_restore $f
	wait_request_state $fid RESTORE SUCCEED

	local expected_fields="event_time data_fid source_fid"
	expected_fields+=" total_bytes current_bytes"

	local START_EVENT
	local FINISH_EVENT
	while read event; do
		# Make sure we're not getting anything from previous events.
		for field in $expected_fields; do
			unset $field
		done

		local parsed=$(parse_json_event "$event")
		if [ -z "$parsed" ]; then
			error "Copytool sent malformed event: $event"
		fi
		eval $parsed

		if [ $event_type == "RESTORE_START" ]; then
			START_EVENT=$event
			if [ $source_fid != $data_fid ]; then
				error "source_fid should == data_fid at start"
			fi
			continue
		elif [ $event_type == "RESTORE_FINISH" ]; then
			FINISH_EVENT=$event
			if [ $source_fid != $data_fid ]; then
				error "source_fid should == data_fid at finish"
			fi
			continue
		elif [ $event_type != "RESTORE_RUNNING" ]; then
			continue
		fi

		# Do some simple checking of the progress update events.
		for expected_field in $expected_fields; do
			if [ -z ${!expected_field+x} ]; then
				error "Missing $expected_field field in event"
			fi
		done

		if [ $total_bytes -eq 0 ]; then
			error "Expected total_bytes to be > 0"
		fi

		# When a restore starts out, the data fid is the same as the
		# source fid. After the restore has gotten going, we learn
		# the new data fid. Once the restore has finished, the source
		# fid is set to the new data fid.
		#
		# We test this because some monitoring software may depend on
		# this behavior. If it changes, then the consumers of these
		# events may need to be modified.
		if [ $source_fid == $data_fid ]; then
			error "source_fid should != data_fid during restore"
		fi
	done < <(echo $"$(get_copytool_event_log)")

	if [ -z "$START_EVENT" ]; then
		error "Copytool failed to send restore start event to FIFO"
	fi

	if [ -z "$FINISH_EVENT" ]; then
		error "Copytool failed to send restore finish event to FIFO"
	fi

	echo "Restore events look OK."

	cdt_clear_no_retry
	copytool_cleanup
	copytool_monitor_cleanup

	rm -rf $test_dir
}
run_test 72 "Copytool logs JSON restore events to FIFO"

test_90() {
	file_count=51 # Max number of files constrained by LNET message size
	mkdir $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	local f=$DIR/$tdir/$tfile
	local FILELIST=/tmp/filelist.txt
	local i=""

	rm -f $FILELIST
	for i in $(seq 1 $file_count); do
		fid=$(copy_file /etc/hosts $f.$i)
		echo $f.$i >> $FILELIST
	done
	# force copytool to use a local/temp archive dir to ensure best
	# performance vs remote/NFS mounts used in auto-tests
	if do_facet $SINGLEAGT "df --local $HSM_ARCHIVE" >/dev/null 2>&1 ; then
		copytool_setup
	else
		local dai=$(get_hsm_param default_archive_id)
		copytool_setup $SINGLEAGT $MOUNT $dai $TMP/$tdir
	fi
	# to be sure wait_all_done will not be mislead by previous tests
	cdt_purge
	wait_for_grace_delay
	$LFS hsm_archive --filelist $FILELIST ||
		error "cannot archive a file list"
	wait_all_done 100
	$LFS hsm_release --filelist $FILELIST ||
		error "cannot release a file list"
	$LFS hsm_restore --filelist $FILELIST ||
		error "cannot restore a file list"
	wait_all_done 100
	copytool_cleanup
}
run_test 90 "Archive/restore a file list"

double_verify_reset_hsm_param() {
	local p=$1
	echo "Testing $HSM_PARAM.$p"
	local val=$(get_hsm_param $p)
	local save=$val
	local val2=$(($val * 2))
	set_hsm_param $p $val2
	val=$(get_hsm_param $p)
	[[ $val == $val2 ]] ||
		error "$HSM_PARAM.$p: $val != $val2 should be (2 * $save)"
	echo "Set $p to 0 must failed"
	set_hsm_param $p 0
	local rc=$?
	# restore value
	set_hsm_param $p $save

	if [[ $rc == 0 ]]; then
		error "we must not be able to set $HSM_PARAM.$p to 0"
	fi
}

test_100() {
	double_verify_reset_hsm_param loop_period
	double_verify_reset_hsm_param grace_delay
	double_verify_reset_hsm_param active_request_timeout
	double_verify_reset_hsm_param max_requests
	double_verify_reset_hsm_param default_archive_id
}
run_test 100 "Set coordinator /proc tunables"

test_102() {
	cdt_disable
	cdt_enable
	cdt_restart
}
run_test 102 "Verify coordinator control"

test_103() {
	# test needs a running copytool
	copytool_setup

	local i=""
	local fid=""

	mkdir -p $DIR/$tdir
	for i in $(seq 1 20); do
		fid=$(copy_file /etc/passwd $DIR/$tdir/$i)
	done
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $DIR/$tdir/*

	cdt_purge

	echo "Current requests"
	local res=$(do_facet $SINGLEMDS "$LCTL get_param -n\
			$HSM_PARAM.actions |\
			grep -v CANCELED | grep -v SUCCEED | grep -v FAILED")

	[[ -z "$res" ]] || error "Some request have not been canceled"

	copytool_cleanup
}
run_test 103 "Purge all requests"

DATA=CEA
DATAHEX='[434541]'
test_104() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	# if cdt is on, it can serve too quickly the request
	cdt_disable
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER --data $DATA $f
	local data1=$(do_facet $SINGLEMDS "$LCTL get_param -n\
			$HSM_PARAM.actions |\
			grep $fid | cut -f16 -d=")
	cdt_enable

	[[ "$data1" == "$DATAHEX" ]] ||
		error "Data field in records is ($data1) and not ($DATAHEX)"

	copytool_cleanup
}
run_test 104 "Copy tool data field"

test_105() {
	mkdir -p $DIR/$tdir
	local i=""

	cdt_disable
	for i in $(seq -w 1 10); do
		cp /etc/passwd $DIR/$tdir/$i
		$LFS hsm_archive $DIR/$tdir/$i
	done
	local reqcnt1=$(do_facet $SINGLEMDS "$LCTL get_param -n\
			$HSM_PARAM.actions |\
			grep WAITING | wc -l")
	cdt_restart
	cdt_disable
	local reqcnt2=$(do_facet $SINGLEMDS "$LCTL get_param -n\
			$HSM_PARAM.actions |\
			grep WAITING | wc -l")
	cdt_enable
	cdt_purge
	[[ "$reqcnt1" == "$reqcnt2" ]] ||
		error "Requests count after shutdown $reqcnt2 != "\
		      "before shutdown $reqcnt1"
}
run_test 105 "Restart of coordinator"

get_agent_by_uuid_mdt() {
	local uuid=$1
	local mdtidx=$2
	local mds=mds$(($mdtidx + 1))
	do_facet $mds "$LCTL get_param -n ${MDT_PREFIX}${mdtidx}.hsm.agents |\
		 grep $uuid"
}

check_agent_registered_by_mdt() {
	local uuid=$1
	local mdtidx=$2
	local mds=mds$(($mdtidx + 1))
	local agent=$(get_agent_by_uuid_mdt $uuid $mdtidx)
	if [[ ! -z "$agent" ]]; then
		echo "found agent $agent on $mds"
	else
		error "uuid $uuid not found in agent list on $mds"
	fi
}

check_agent_unregistered_by_mdt() {
	local uuid=$1
	local mdtidx=$2
	local mds=mds$(($mdtidx + 1))
	local agent=$(get_agent_by_uuid_mdt $uuid $mdtidx)
	if [[ -z "$agent" ]]; then
		echo "uuid not found in agent list on $mds"
	else
		error "uuid found in agent list on $mds: $agent"
	fi
}

check_agent_registered() {
	local uuid=$1
	local mdsno
	for mdsno in $(seq 1 $MDSCOUNT); do
		check_agent_registered_by_mdt $uuid $((mdsno - 1))
	done
}

check_agent_unregistered() {
	local uuid=$1
	local mdsno
	for mdsno in $(seq 1 $MDSCOUNT); do
		check_agent_unregistered_by_mdt $uuid $((mdsno - 1))
	done
}

get_agent_uuid() {
	local agent=${1:-$(facet_active_host $SINGLEAGT)}

	# Lustre mount-point is mandatory and last parameter on
	# copytool cmd-line.
	local mntpnt=$(do_rpc_nodes $agent pgrep -fl $HSMTOOL_BASE |
		       grep -v pgrep | awk '{print $NF}')
	[ -n "$mntpnt" ] || error "Found no Agent or with no mount-point "\
				  "parameter"
	do_rpc_nodes $agent get_client_uuid $mntpnt | cut -d' ' -f2
}

test_106() {
	# test needs a running copytool
	copytool_setup

	local uuid=$(get_agent_uuid $(facet_active_host $SINGLEAGT))

	check_agent_registered $uuid

	search_copytools || error "No copytool found"

	copytool_cleanup
	check_agent_unregistered $uuid

	copytool_setup
	uuid=$(get_agent_uuid $(facet_active_host $SINGLEAGT))
	check_agent_registered $uuid

	copytool_cleanup
}
run_test 106 "Copytool register/unregister"

test_107() {
	# test needs a running copytool
	copytool_setup
	# create and archive file
	mkdir -p $DIR/$tdir
	local f1=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f1)
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f1
	wait_request_state $fid ARCHIVE SUCCEED
	# shutdown and restart MDS
	fail $SINGLEMDS
	# check the copytool still gets messages from MDT
	local f2=$DIR/$tdir/2
	local fid=$(copy_file /etc/passwd $f2)
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f2
	# main check of this sanity: this request MUST succeed
	wait_request_state $fid ARCHIVE SUCCEED
	copytool_cleanup
}
run_test 107 "Copytool re-register after MDS restart"

policy_set_and_test()
{
	local change="$1"
	local target="$2"
	do_facet $SINGLEMDS $LCTL set_param "$HSM_PARAM.policy=\\\"$change\\\""
	local policy=$(do_facet $SINGLEMDS $LCTL get_param -n $HSM_PARAM.policy)
	[[ "$policy" == "$target" ]] ||
		error "Wrong policy after '$change': '$policy' != '$target'"
}

test_109() {
	# to force default policy setting if error
	CDT_POLICY_HAD_CHANGED=true

	local policy=$(do_facet $SINGLEMDS $LCTL get_param -n $HSM_PARAM.policy)
	local default="NonBlockingRestore [NoRetryAction]"
	[[ "$policy" == "$default" ]] ||
		error "default policy has changed,"\
		      " '$policy' != '$default' update the test"
	policy_set_and_test "+NBR" "[NonBlockingRestore] [NoRetryAction]"
	policy_set_and_test "+NRA" "[NonBlockingRestore] [NoRetryAction]"
	policy_set_and_test "-NBR" "NonBlockingRestore [NoRetryAction]"
	policy_set_and_test "-NRA" "NonBlockingRestore NoRetryAction"
	policy_set_and_test "NRA NBR" "[NonBlockingRestore] [NoRetryAction]"
	# useless bacause we know but safer for futur changes to use real value
	local policy=$(do_facet $SINGLEMDS $LCTL get_param -n $HSM_PARAM.policy)
	echo "Next set_param must failed"
	policy_set_and_test "wrong" "$policy"

	# return to default
	echo "Back to default policy"
	cdt_set_sanity_policy
}
run_test 109 "Policy display/change"

test_110a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	copy2archive /etc/passwd $tdir/$tfile

	local f=$DIR/$tdir/$tfile
	import_file $tdir/$tfile $f
	local fid=$(path2fid $f)

	cdt_set_non_blocking_restore
	md5sum $f
	local st=$?

	# cleanup
	wait_request_state $fid RESTORE SUCCEED
	cdt_clear_non_blocking_restore

	# Test result
	[[ $st == 1 ]] ||
		error "md5sum returns $st != 1, "\
			"should also perror ENODATA (No data available)"

	copytool_cleanup
}
run_test 110a "Non blocking restore policy (import case)"

test_110b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	cdt_set_non_blocking_restore
	md5sum $f
	local st=$?

	# cleanup
	wait_request_state $fid RESTORE SUCCEED
	cdt_clear_non_blocking_restore

	# Test result
	[[ $st == 1 ]] ||
		error "md5sum returns $st != 1, "\
			"should also perror ENODATA (No data available)"

	copytool_cleanup
}
run_test 110b "Non blocking restore policy (release case)"

test_111a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	copy2archive /etc/passwd $tdir/$tfile

	local f=$DIR/$tdir/$tfile

	import_file $tdir/$tfile $f
	local fid=$(path2fid $f)

	cdt_set_no_retry

	copytool_remove_backend $fid

	$LFS hsm_restore $f
	wait_request_state $fid RESTORE FAILED
	local st=$?

	# cleanup
	cdt_clear_no_retry

	# Test result
	[[ $st == 0 ]] || error "Restore does not failed"

	copytool_cleanup
}
run_test 111a "No retry policy (import case), restore will error"\
	      " (No such file or directory)"

test_111b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)
	cdt_set_no_retry
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	copytool_remove_backend $fid

	$LFS hsm_restore $f
	wait_request_state $fid RESTORE FAILED
	local st=$?

	# cleanup
	cdt_clear_no_retry

	# Test result
	[[ $st == 0 ]] || error "Restore does not failed"

	copytool_cleanup
}
run_test 111b "No retry policy (release case), restore will error"\
	      " (No such file or directory)"

test_112() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)
	cdt_disable
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	local l=$($LFS hsm_action $f)
	echo $l
	local res=$(echo $l | cut -f 2- -d" " | grep ARCHIVE)

	# cleanup
	cdt_enable
	wait_request_state $fid ARCHIVE SUCCEED

	# Test result
	[[ ! -z "$res" ]] || error "action is $l which is not an ARCHIVE"

	copytool_cleanup
}
run_test 112 "State of recorded request"

test_200() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_cancel $f)
	[ $? != 0 ] && skip "not enough free space" && return

	# test with cdt on is made in test_221
	cdt_disable
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	# wait archive to register at CDT
	wait_request_state $fid ARCHIVE WAITING
	$LFS hsm_cancel $f
	cdt_enable
	wait_request_state $fid ARCHIVE CANCELED
	wait_request_state $fid CANCEL SUCCEED

	copytool_cleanup
}
run_test 200 "Register/Cancel archive"

test_201() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	make_archive $tdir/$tfile
	import_file $tdir/$tfile $f
	local fid=$(path2fid $f)

	# test with cdt on is made in test_222
	cdt_disable
	$LFS hsm_restore $f
	# wait restore to register at CDT
	wait_request_state $fid RESTORE WAITING
	$LFS hsm_cancel $f
	cdt_enable
	wait_request_state $fid RESTORE CANCELED
	wait_request_state $fid CANCEL SUCCEED

	copytool_cleanup
}
run_test 201 "Register/Cancel restore"

test_202() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	cdt_disable
	$LFS hsm_remove $f
	# wait remove to register at CDT
	wait_request_state $fid REMOVE WAITING
	$LFS hsm_cancel $f
	cdt_enable
	wait_request_state $fid REMOVE CANCELED

	copytool_cleanup
}
run_test 202 "Register/Cancel remove"

test_220() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)

	changelog_setup

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	local flags=$(changelog_get_flags ${MDT[0]} HSM $fid | tail -1)
	changelog_cleanup

	local target=0x0
	[[ $flags == $target ]] || error "Changelog flag is $flags not $target"

	copytool_cleanup
}
run_test 220 "Changelog for archive"

test_221() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_cancel $f)
	[ $? != 0 ] && skip "not enough free space" && return

	changelog_setup

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE STARTED
	$LFS hsm_cancel $f
	wait_request_state $fid ARCHIVE CANCELED
	wait_request_state $fid CANCEL SUCCEED

	local flags=$(changelog_get_flags ${MDT[0]} HSM $fid | tail -1)

	local target=0x7d
	[[ $flags == $target ]] || error "Changelog flag is $flags not $target"

	cleanup
}
run_test 221 "Changelog for archive canceled"

test_222a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	copy2archive /etc/passwd $tdir/$tfile

	local f=$DIR/$tdir/$tfile
	import_file $tdir/$tfile $f
	local fid=$(path2fid $f)

	changelog_setup

	$LFS hsm_restore $f
	wait_request_state $fid RESTORE SUCCEED

	local flags=$(changelog_get_flags ${MDT[0]} HSM $fid | tail -1)

	local target=0x80
	[[ $flags == $target ]] || error "Changelog flag is $flags not $target"

	cleanup
}
run_test 222a "Changelog for explicit restore"

test_222b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)

	changelog_setup
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f

	md5sum $f

	wait_request_state $fid RESTORE SUCCEED

	local flags=$(changelog_get_flags ${MDT[0]} HSM $fid | tail -1)

	local target=0x80
	[[ $flags == $target ]] || error "Changelog flag is $flags not $target"

	cleanup
}
run_test 222b "Changelog for implicit restore"

test_223a() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	make_archive $tdir/$tfile

	changelog_setup

	import_file $tdir/$tfile $f
	local fid=$(path2fid $f)

	$LFS hsm_restore $f
	wait_request_state $fid RESTORE STARTED
	$LFS hsm_cancel $f
	wait_request_state $fid RESTORE CANCELED
	wait_request_state $fid CANCEL SUCCEED

	local flags=$(changelog_get_flags ${MDT[0]} HSM $fid | tail -1)

	local target=0xfd
	[[ $flags == $target ]] ||
		error "Changelog flag is $flags not $target"

	cleanup
}
run_test 223a "Changelog for restore canceled (import case)"

test_223b() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	changelog_setup
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	$LFS hsm_release $f
	$LFS hsm_restore $f
	wait_request_state $fid RESTORE STARTED
	$LFS hsm_cancel $f
	wait_request_state $fid RESTORE CANCELED
	wait_request_state $fid CANCEL SUCCEED

	local flags=$(changelog_get_flags ${MDT[0]} HSM $fid | tail -1)

	local target=0xfd
	[[ $flags == $target ]] ||
		error "Changelog flag is $flags not $target"

	cleanup
}
run_test 223b "Changelog for restore canceled (release case)"

test_224() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f=$DIR/$tdir/$tfile
	local fid=$(copy_file /etc/passwd $f)

	changelog_setup
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_remove $f
	wait_request_state $fid REMOVE SUCCEED

	local flags=$(changelog_get_flags ${MDT[0]} HSM $fid | tail -n 1)

	local target=0x200
	[[ $flags == $target ]] ||
		error "Changelog flag is $flags not $target"

	cleanup
}
run_test 224 "Changelog for remove"

test_225() {
	# test needs a running copytool
	copytool_setup

	# test is not usable because remove request is too fast
	# so it is always finished before cancel can be done ...
	echo "Test disabled"
	copytool_cleanup
	return 0

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_progress $f)
	[ $? != 0 ] && skip "not enough free space" && return

	changelog_setup
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED

	# if cdt is on, it can serve too quickly the request
	cdt_disable
	$LFS hsm_remove $f
	$LFS hsm_cancel $f
	cdt_enable
	wait_request_state $fid REMOVE CANCELED
	wait_request_state $fid CANCEL SUCCEED

	flags=$(changelog_get_flags ${MDT[0]} RENME $fid2)
	local flags=$($LFS changelog ${MDT[0]} | grep HSM | grep $fid |
		tail -n 1 | awk '{print $5}')

	local target=0x27d
	[[ $flags == $target ]] ||
		error "Changelog flag is $flags not $target"

	cleanup
}
run_test 225 "Changelog for remove canceled"

test_226() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir

	local f1=$DIR/$tdir/$tfile-1
	local f2=$DIR/$tdir/$tfile-2
	local f3=$DIR/$tdir/$tfile-3
	local fid1=$(copy_file /etc/passwd $f1)
	local fid2=$(copy_file /etc/passwd $f2)
	copy_file /etc/passwd $f3

	changelog_setup
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f1
	wait_request_state $fid1 ARCHIVE SUCCEED

	$LFS hsm_archive $f2
	wait_request_state $fid2 ARCHIVE SUCCEED

	rm $f1 || error "rm $f1 failed"

	local flags=$(changelog_get_flags ${MDT[0]} UNLNK $fid1)

	local target=0x3
	[[ $flags == $target ]] ||
		error "Changelog flag is $flags not $target"

	mv $f3 $f2 || error "mv $f3 $f2 failed"

	flags=$(changelog_get_flags ${MDT[0]} RENME $fid2)

	target=0x3
	[[ $flags == $target ]] ||
		error "Changelog flag is $flags not $target"

	cleanup
}
run_test 226 "changelog for last rm/mv with exiting archive"

check_flags_changes() {
	local f=$1
	local fid=$2
	local hsm_flag=$3
	local fst=$4
	local cnt=$5

	local target=0x280
	$LFS hsm_set --$hsm_flag $f ||
		error "Cannot set $hsm_flag on $f"
	local flags=($(changelog_get_flags ${MDT[0]} HSM $fid))
	local seen=${#flags[*]}
	cnt=$((fst + cnt))
	[[ $seen == $cnt ]] ||
		error "set $hsm_flag: Changelog events $seen != $cnt"
	[[ ${flags[$((cnt - 1))]} == $target ]] ||
		error "set $hsm_flag: Changelog flags are "\
			"${flags[$((cnt - 1))]} not $target"

	$LFS hsm_clear --$hsm_flag $f ||
		error "Cannot clear $hsm_flag on $f"
	flags=($(changelog_get_flags ${MDT[0]} HSM $fid))
	seen=${#flags[*]}
	cnt=$(($cnt + 1))
	[[ $cnt == $seen ]] ||
		error "clear $hsm_flag: Changelog events $seen != $cnt"

	[[ ${flags[$((cnt - 1))]} == $target ]] ||
		error "clear $hsm_flag: Changelog flag is "\
			"${flags[$((cnt - 1))]} not $target"
}

test_227() {
	# test needs a running copytool
	copytool_setup
	changelog_setup

	mkdir -p $DIR/$tdir
	typeset -a flags

	for i in norelease noarchive exists archived
	do
		local f=$DIR/$tdir/$tfile-$i
		local fid=$(copy_file /etc/passwd $f)
		check_flags_changes $f $fid $i 0 1
	done

	f=$DIR/$tdir/$tfile---lost
	fid=$(copy_file /etc/passwd $f)
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE SUCCEED
	check_flags_changes $f $fid lost 3 1

	cleanup
}
run_test 227 "changelog when explicit setting of HSM flags"

test_228() {
	# test needs a running copytool
	copytool_setup

	local fid=$(make_small_sync $DIR/$tfile)
	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $DIR/$tfile
	wait_request_state $fid ARCHIVE SUCCEED

	$LFS hsm_release $DIR/$tfile
	check_hsm_flags $DIR/$tfile "0x0000000d"

	filefrag $DIR/$tfile | grep " 1 extent found" ||
		error "filefrag on released file must return only one extent"

	# only newer versions of cp detect sparse files by stat/FIEMAP
	# (LU-2580)
	cp --sparse=auto $DIR/$tfile $DIR/$tfile.2 ||
		error "copying $DIR/$tfile"
	cmp $DIR/$tfile $DIR/$tfile.2 || error "comparing copied $DIR/$tfile"

	$LFS hsm_release $DIR/$tfile
	check_hsm_flags $DIR/$tfile "0x0000000d"

	mkdir -p $DIR/$tdir || error "mkdir $tdir failed"

	tar cf - --sparse $DIR/$tfile | tar xvf - -C $DIR/$tdir ||
		error "tar failed"
	cmp $DIR/$tfile $DIR/$tdir/$DIR/$tfile ||
		error "comparing untarred $DIR/$tfile"

	rm -f $DIR/$tfile $DIR/$tfile.2 ||
		error "rm $DIR/$tfile or $DIR/$tfile.2 failed"
	copytool_cleanup
}
run_test 228 "On released file, return extend to FIEMAP. For [cp,tar] --sparse"

test_250() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local maxrequest=$(get_hsm_param max_requests)
	local rqcnt=$(($maxrequest * 3))
	local i=""

	cdt_disable
	for i in $(seq -w 1 $rqcnt); do
		rm -f $DIR/$tdir/$i
		dd if=/dev/urandom of=$DIR/$tdir/$i bs=1M count=10 conv=fsync
	done
	# we do it in 2 steps, so all requests arrive at the same time
	for i in $(seq -w 1 $rqcnt); do
		$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $DIR/$tdir/$i
	done
	cdt_enable
	local cnt=$rqcnt
	local wt=$rqcnt
	while [[ $cnt != 0 || $wt != 0 ]]; do
		sleep 1
		cnt=$(do_facet $SINGLEMDS "$LCTL get_param -n\
			$HSM_PARAM.actions |\
			grep STARTED | grep -v CANCEL | wc -l")
		[[ $cnt -le $maxrequest ]] ||
			error "$cnt > $maxrequest too many started requests"
		wt=$(do_facet $SINGLEMDS "$LCTL get_param\
			$HSM_PARAM.actions |\
			grep WAITING | wc -l")
		echo "max=$maxrequest started=$cnt waiting=$wt"
	done

	copytool_cleanup
}
run_test 250 "Coordinator max request"

test_251() {
	# test needs a running copytool
	copytool_setup

	mkdir -p $DIR/$tdir
	local f=$DIR/$tdir/$tfile
	local fid
	fid=$(make_large_for_cancel $f)
	[ $? != 0 ] && skip "not enough free space" && return

	cdt_disable
	# to have a short test
	local old_to=$(get_hsm_param active_request_timeout)
	set_hsm_param active_request_timeout 4
	# to be sure the cdt will wake up frequently so
	# it will be able to cancel the "old" request
	local old_loop=$(get_hsm_param loop_period)
	set_hsm_param loop_period 2
	cdt_enable

	# clear locks to avoid extra delay caused by flush/cancel
	# and thus prevent early copytool death to timeout.
	cancel_lru_locks osc

	$LFS hsm_archive --archive $HSM_ARCHIVE_NUMBER $f
	wait_request_state $fid ARCHIVE STARTED
	sleep 5
	wait_request_state $fid ARCHIVE CANCELED

	set_hsm_param active_request_timeout $old_to
	set_hsm_param loop_period $old_loop

	copytool_cleanup
}
run_test 251 "Coordinator request timeout"

test_300() {
	# the only way to test ondisk conf is to restart MDS ...
	echo "Stop coordinator and remove coordinator state at mount"
	# stop coordinator
	cdt_shutdown
	# clean on disk conf set by default
	cdt_clear_mount_state
	cdt_check_state stopped

	# check cdt still off after umount/remount
	fail $SINGLEMDS
	cdt_check_state stopped

	echo "Set coordinator start at mount, and start coordinator"
	cdt_set_mount_state enabled

	# check cdt is on
	cdt_check_state enabled

	# check cdt still on after umount/remount
	fail $SINGLEMDS
	cdt_check_state enabled

	# we are back to original state (cdt started at mount)
}
run_test 300 "On disk coordinator state kept between MDT umount/mount"

test_301() {
	local ai=$(get_hsm_param default_archive_id)
	local new=$((ai + 1))

	set_hsm_param default_archive_id $new -P
	fail $SINGLEMDS
	local res=$(get_hsm_param default_archive_id)

	# clear value
	set_hsm_param default_archive_id "" "-P -d"

	[[ $new == $res ]] || error "Value after MDS restart is $res != $new"
}
run_test 301 "HSM tunnable are persistent"

test_302() {
	local ai=$(get_hsm_param default_archive_id)
	local new=$((ai + 1))

	# stop coordinator
	cdt_shutdown

	set_hsm_param default_archive_id $new -P

	local mdtno
	for mdtno in $(seq 1 $MDSCOUNT); do
		fail mds${mdtno}
	done

	# check cdt is on
	cdt_check_state enabled

	local res=$(get_hsm_param default_archive_id)

	# clear value
	set_hsm_param default_archive_id "" "-P -d"

	[[ $new == $res ]] || error "Value after MDS restart is $res != $new"
}
run_test 302 "HSM tunnable are persistent when CDT is off"

test_400() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return

	copytool_setup

	mkdir -p $DIR/$tdir

	local dir_mdt0=$DIR/$tdir/mdt0
	local dir_mdt1=$DIR/$tdir/mdt1

	# create 1 dir per MDT
	$LFS mkdir -i 0 $dir_mdt0 || error "lfs mkdir"
	$LFS mkdir -i 1 $dir_mdt1 || error "lfs mkdir"

	# create 1 file in each MDT
	local fid1=$(make_small $dir_mdt0/$tfile)
	local fid2=$(make_small $dir_mdt1/$tfile)

	# check that hsm request on mdt0 is sent to the right MDS
	$LFS hsm_archive $dir_mdt0/$tfile || error "lfs hsm_archive"
	wait_request_state $fid1 ARCHIVE SUCCEED 0 &&
		echo "archive successful on mdt0"

	# check that hsm request on mdt1 is sent to the right MDS
	$LFS hsm_archive $dir_mdt1/$tfile || error "lfs hsm_archive"
	wait_request_state $fid2 ARCHIVE SUCCEED 1 &&
		echo "archive successful on mdt1"

	copytool_cleanup
	# clean test files and directories
	rm -rf $dir_mdt0 $dir_mdt1
}
run_test 400 "Single request is sent to the right MDT"

test_401() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return

	copytool_setup

	mkdir -p $DIR/$tdir

	local dir_mdt0=$DIR/$tdir/mdt0
	local dir_mdt1=$DIR/$tdir/mdt1

	# create 1 dir per MDT
	$LFS mkdir -i 0 $dir_mdt0 || error "lfs mkdir"
	$LFS mkdir -i 1 $dir_mdt1 || error "lfs mkdir"

	# create 1 file in each MDT
	local fid1=$(make_small $dir_mdt0/$tfile)
	local fid2=$(make_small $dir_mdt1/$tfile)

	# check that compound requests are shunt to the rights MDTs
	$LFS hsm_archive $dir_mdt0/$tfile $dir_mdt1/$tfile ||
		error "lfs hsm_archive"
	wait_request_state $fid1 ARCHIVE SUCCEED 0 &&
		echo "archive successful on mdt0"
	wait_request_state $fid2 ARCHIVE SUCCEED 1 &&
		echo "archive successful on mdt1"

	copytool_cleanup
	# clean test files and directories
	rm -rf $dir_mdt0 $dir_mdt1
}
run_test 401 "Compound requests split and sent to their respective MDTs"

mdc_change_state() # facet, MDT_pattern, activate|deactivate
{
	local facet=$1
	local pattern="$2"
	local state=$3
	local node=$(facet_active_host $facet)
	local mdc
	for mdc in $(do_facet $facet "$LCTL dl | grep -E ${pattern}-mdc" |
			awk '{print $4}'); do
		echo "$3 $mdc on $node"
		do_facet $facet "$LCTL --device $mdc $state" || return 1
	done
}

test_402() {
	# make sure there is no running copytool
	copytool_cleanup

	# deactivate all mdc on agent1
	mdc_change_state $SINGLEAGT "$FSNAME-MDT000." "deactivate"

	HSMTOOL_NOERROR=true copytool_setup $SINGLEAGT

	check_agent_unregistered "uuid" # match any agent

	# no expected running copytool
	search_copytools $agent && error "Copytool start should have failed"

	# reactivate MDCs
	mdc_change_state $SINGLEAGT "$FSNAME-MDT000." "activate"
}
run_test 402 "Copytool start fails if all MDTs are inactive"

test_403() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return

	# make sure there is no running copytool
	copytool_cleanup

        local agent=$(facet_active_host $SINGLEAGT)

	# deactivate all mdc for MDT0001
	mdc_change_state $SINGLEAGT "$FSNAME-MDT0001" "deactivate"

	copytool_setup
	local uuid=$(get_agent_uuid $agent)
	# check the agent is registered on MDT0000, and not on MDT0001
	check_agent_registered_by_mdt $uuid 0
	check_agent_unregistered_by_mdt $uuid 1

	# check running copytool process
	search_copytools $agent || error "No running copytools on $agent"

	# reactivate all mdc for MDT0001
	mdc_change_state $SINGLEAGT "$FSNAME-MDT0001" "activate"

	# make sure the copytool is now registered to all MDTs
	check_agent_registered $uuid

	copytool_cleanup
}
run_test 403 "Copytool starts with inactive MDT and register on reconnect"

test_404() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return

	copytool_setup

	# create files on both MDT0000 and MDT0001
	mkdir -p $DIR/$tdir

	local dir_mdt0=$DIR/$tdir/mdt0
	$LFS mkdir -i 0 $dir_mdt0 || error "lfs mkdir"

	# create 1 file on mdt0
	local fid1=$(make_small $dir_mdt0/$tfile)

	# deactivate all mdc for MDT0001
	mdc_change_state $SINGLEAGT "$FSNAME-MDT0001" "deactivate"

	# send an HSM request for files in MDT0000
	$LFS hsm_archive $dir_mdt0/$tfile || error "lfs hsm_archive"

	# check for completion of files in MDT0000
	wait_request_state $fid1 ARCHIVE SUCCEED 0 &&
		echo "archive successful on mdt0"

	# reactivate all mdc for MDT0001
	mdc_change_state $SINGLEAGT "$FSNAME-MDT0001" "activate"

	copytool_cleanup
	# clean test files and directories
	rm -rf $dir_mdt0
}
run_test 404 "Inactive MDT does not block requests for active MDTs"

test_405() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return

	copytool_setup

	mkdir -p $DIR/$tdir

	local striped_dir=$DIR/$tdir/striped_dir

	# create striped dir on all of MDTs
	$LFS mkdir -i 0 -c $MDSCOUNT $striped_dir || error "lfs mkdir"

	local fid1=$(make_small_sync $striped_dir/${tfile}_0)
	local fid2=$(make_small_sync $striped_dir/${tfile}_1)
	local fid3=$(make_small_sync $striped_dir/${tfile}_2)
	local fid4=$(make_small_sync $striped_dir/${tfile}_3)

	local idx1=$($LFS getstripe -M $striped_dir/${tfile}_0)
	local idx2=$($LFS getstripe -M $striped_dir/${tfile}_1)
	local idx3=$($LFS getstripe -M $striped_dir/${tfile}_2)
	local idx4=$($LFS getstripe -M $striped_dir/${tfile}_3)

	# check that compound requests are shunt to the rights MDTs
	$LFS hsm_archive $striped_dir/${tfile}_0 $striped_dir/${tfile}_1  \
			 $striped_dir/${tfile}_2 $striped_dir/${tfile}_3 ||
		error "lfs hsm_archive"

	wait_request_state $fid1 ARCHIVE SUCCEED $idx1 &&
		echo "archive successful on $fid1"
	wait_request_state $fid2 ARCHIVE SUCCEED $idx2 &&
		echo "archive successful on $fid2"
	wait_request_state $fid3 ARCHIVE SUCCEED $idx3 &&
		echo "archive successful on $fid3"
	wait_request_state $fid4 ARCHIVE SUCCEED $idx4 &&
		echo "archive successful on $fid4"

	$LFS hsm_release $striped_dir/${tfile}_0 || error "lfs hsm_release 1"
	$LFS hsm_release $striped_dir/${tfile}_1 || error "lfs hsm_release 2"
	$LFS hsm_release $striped_dir/${tfile}_2 || error "lfs hsm_release 3"
	$LFS hsm_release $striped_dir/${tfile}_3 || error "lfs hsm_release 4"

	cat $striped_dir/${tfile}_0 > /dev/null || error "cat ${tfile}_0 failed"
	cat $striped_dir/${tfile}_1 > /dev/null || error "cat ${tfile}_1 failed"
	cat $striped_dir/${tfile}_2 > /dev/null || error "cat ${tfile}_2 failed"
	cat $striped_dir/${tfile}_3 > /dev/null || error "cat ${tfile}_3 failed"

	copytool_cleanup
}
run_test 405 "archive and release under striped directory"

test_406() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0

	[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.7.64) ] &&
		skip "need MDS version at least 2.7.64" && return 0

	local fid
	local mdt_index

	copytool_setup
	mkdir -p $DIR/$tdir
	fid=$(make_small $DIR/$tdir/$tfile)
	echo "old fid $fid"

	$LFS hsm_archive $DIR/$tdir/$tfile
	wait_request_state "$fid" ARCHIVE SUCCEED
	$LFS hsm_release $DIR/$tdir/$tfile

	# Should migrate $tdir but not $tfile.
	$LFS mv -M1 $DIR/$tdir &&
		error "migrating HSM an archived file should fail"

	$LFS hsm_restore $DIR/$tdir/$tfile
	wait_request_state "$fid" RESTORE SUCCEED

	$LFS hsm_remove $DIR/$tdir/$tfile
	wait_request_state "$fid" REMOVE SUCCEED

	cat $DIR/$tdir/$tfile > /dev/null ||
		error "cannot read $DIR/$tdir/$tfile"

	$LFS mv -M1 $DIR/$tdir ||
		error "cannot complete migration after HSM remove"

	mdt_index=$($LFS getstripe -M $DIR/$tdir)
	if ((mdt_index != 1)); then
		error "expected MDT index 1, got $mdt_index"
	fi

	# Refresh fid after migration.
	fid=$(path2fid $DIR/$tdir/$tfile)
	echo "new fid $fid"

	$LFS hsm_archive $DIR/$tdir/$tfile
	wait_request_state "$fid" ARCHIVE SUCCEED 1

	lctl set_param debug=+trace
	$LFS hsm_release $DIR/$tdir/$tfile ||
		error "cannot release $DIR/$tdir/$tfile"

	$LFS hsm_restore $DIR/$tdir/$tfile
	wait_request_state "$fid" RESTORE SUCCEED 1

	cat $DIR/$tdir/$tfile > /dev/null ||
		error "cannot read $DIR/$tdir/$tfile"

	copytool_cleanup
}
run_test 406 "attempting to migrate HSM archived files is safe"

test_500()
{
	[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.6.92) ] &&
		skip "HSM migrate is not supported" && return

	# Stop the existing copytool
	copytool_cleanup

	test_mkdir -p $DIR/$tdir
	llapi_hsm_test -d $DIR/$tdir || error "One llapi HSM test failed"
}
run_test 500 "various LLAPI HSM tests"

copytool_cleanup

complete $SECONDS
check_and_cleanup_lustre
exit_status
