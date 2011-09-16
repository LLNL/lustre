#!/bin/bash

set -e

LUSTRE=${LUSTRE:-$(cd $(dirname $0)/..; echo $PWD)}
SETUP=${SETUP:-""}
CLEANUP=${CLEANUP:-""}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}
init_logging

# While we do not use OSTCOUNT=1 setup anymore,
# ost1failover_HOST is used
#ostfailover_HOST=${ostfailover_HOST:-$ost_HOST}
#failover= must be defined in OST_MKFS_OPTIONS if ostfailover_HOST != ost_HOST

require_dsh_ost || exit 0

# Tests that fail on uml
CPU=`awk '/model/ {print $4}' /proc/cpuinfo`
[ "$CPU" = "UML" ] && EXCEPT="$EXCEPT 6"

# Skip these tests
# BUG NUMBER:
ALWAYS_EXCEPT="$REPLAY_OST_SINGLE_EXCEPT"

# Orion:
# 6 - ORI-124
ALWAYS_EXCEPT="6 $ALWAYS_EXCEPT"

#					
[ "$SLOW" = "no" ] && EXCEPT_SLOW="5"
FAIL_ON_ERROR=false

build_test_filter

check_and_setup_lustre
assert_DIR
rm -rf $DIR/[df][0-9]*

TDIR=$DIR/d0.${TESTSUITE}
mkdir -p $TDIR
$LFS setstripe $TDIR -i 0 -c 1
$LFS getstripe $TDIR

test_0a() {
    zconf_umount `hostname` $MOUNT -f
    # needs to run during initial client->OST connection
    #define OBD_FAIL_OST_ALL_REPLY_NET       0x211
    do_facet ost1 "lctl set_param fail_loc=0x80000211"
    zconf_mount `hostname` $MOUNT && df $MOUNT || error "0a mount fail"
}
run_test 0a "target handle mismatch (bug 5317) `date +%H:%M:%S`"

test_0b() {
    fail ost1
    cp /etc/profile  $TDIR/$tfile
    sync
    diff /etc/profile $TDIR/$tfile
    rm -f $TDIR/$tfile
}
run_test 0b "empty replay"

test_1() {
    date > $TDIR/$tfile || error "error creating $TDIR/$tfile"
    fail ost1
    $CHECKSTAT -t file $TDIR/$tfile || return 1
    rm -f $TDIR/$tfile
}
run_test 1 "touch"

test_2() {
    for i in `seq 10`; do
        echo "tag-$i" > $TDIR/$tfile-$i || error "create $TDIR/$tfile-$i"
    done 
    fail ost1
    for i in `seq 10`; do
      grep -q "tag-$i" $TDIR/$tfile-$i || error "grep $TDIR/$tfile-$i"
    done 
    rm -f $TDIR/$tfile-*
}
run_test 2 "|x| 10 open(O_CREAT)s"

test_3() {
    verify=$ROOT/tmp/verify-$$
    dd if=/dev/urandom bs=4096 count=1280 | tee $verify > $TDIR/$tfile &
    ddpid=$!
    sync &
    fail ost1
    wait $ddpid || return 1
    cmp $verify $TDIR/$tfile || return 2
    rm -f $verify $TDIR/$tfile
}
run_test 3 "Fail OST during write, with verification"

test_4() {
    verify=$ROOT/tmp/verify-$$
    dd if=/dev/urandom bs=4096 count=1280 | tee $verify > $TDIR/$tfile
    # invalidate cache, so that we're reading over the wire
    cancel_lru_locks osc
    cmp $verify $TDIR/$tfile &
    cmppid=$!
    fail ost1
    wait $cmppid || return 1
    rm -f $verify $TDIR/$tfile
}
run_test 4 "Fail OST during read, with verification"

iozone_bg () {
    local args=$@

    local tmppipe=$TMP/${TESTSUITE}.${TESTNAME}.pipe
    mkfifo $tmppipe

    echo "+ iozone $args"
    iozone $args > $tmppipe &

    local pid=$!

    echo "tmppipe=$tmppipe"
    echo iozone pid=$pid

    # iozone exit code is 0 even if iozone is not completed
    # need to check iozone output  on "complete"
    local iozonelog=$TMP/${TESTSUITE}.iozone.log
    rm -f $iozonelog
    cat $tmppipe | while read line ; do
        echo "$line"
        echo "$line" >>$iozonelog
    done;

    local rc=0
    wait $pid
    rc=$?
    if ! $(tail -1 $iozonelog | grep -q complete); then
        echo iozone failed!
        rc=1
    fi
    rm -f $tmppipe
    rm -f $iozonelog
    return $rc
}

test_5() {
    [ -z "`which iozone 2> /dev/null`" ] && skip_env "iozone missing" && return 0

    # striping is -c 1, get min of available
    local minavail=$(lctl get_param -n osc.*[oO][sS][cC][-_]*.kbytesavail | sort -n | head -1)
    local size=$(( minavail * 3/4 ))
    local GB=1048576  # 1048576KB == 1GB

    if (( size > GB )); then
        size=$GB
    fi
    local iozone_opts="-i 0 -i 1 -i 2 -+d -r 4 -s $size -f $TDIR/$tfile"

    iozone_bg $iozone_opts &
    local pid=$!

    echo iozone bg pid=$pid

    sleep 8
    fail ost1
    local rc=0
    wait $pid
    rc=$?
    log "iozone rc=$rc"
    rm -f $TDIR/$tfile
    # wait till rm is done, otherwise subsequent test can fail
    wait_delete_completed
    [ $rc -eq 0 ] || error "iozone failed"
    return $rc
}
run_test 5 "Fail OST during iozone"

kbytesfree() {
   calc_osc_kbytes kbytesfree
}

test_6() {
    remote_mds_nodsh && skip "remote MDS with nodsh" && return 0

    f=$TDIR/$tfile
    before=`kbytesfree`
    dd if=/dev/urandom bs=4096 count=1280 of=$f || return 28
    lfs getstripe $f
    get_stripe_info client $f

    sync
    sleep 2 # ensure we have a fresh statfs
    sync
#define OBD_FAIL_MDS_REINT_NET_REP       0x119
    do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000119"
    after_dd=`kbytesfree`
    log "before: $before after_dd: $after_dd"
    (( $before > $after_dd )) || return 1
    rm -f $f
    fail ost$((stripe_index + 1))
    wait_recovery_complete ost$((stripe_index + 1)) || error "OST recovery not done"
    $CHECKSTAT -t file $f && return 2 || true
    sync
    # let the delete happen
    wait_mds_ost_sync || return 4
    wait_delete_completed || return 5
    after=`kbytesfree`
    log "before: $before after: $after"
    (( $before <= $after + 40 )) || return 3	# take OST logs into account
}
run_test 6 "Fail OST before obd_destroy"

test_7() {
    f=$TDIR/$tfile
    before=`kbytesfree`
    dd if=/dev/urandom bs=4096 count=1280 of=$f || return 4
    sync
    sleep 2 # ensure we have a fresh statfs
    sync
    after_dd=`kbytesfree`
    log "before: $before after_dd: $after_dd"
    (( $before > $after_dd )) || return 1
    replay_barrier ost1
    rm -f $f
    fail ost1
    wait_recovery_complete ost1 || error "OST recovery not done"
    $CHECKSTAT -t file $f && return 2 || true
    sync
    # let the delete happen
    wait_mds_ost_sync || return 4
    wait_delete_completed || return 5
    after=`kbytesfree`
    log "before: $before after: $after"
    (( $before <= $after + 40 )) || return 3	# take OST logs into account
}
run_test 7 "Fail OST before obd_destroy"

check_for_process () {
	local clients=$1
	shift
	local prog=$@

	killall_process $clients "$prog" -0
}

killall_process () {
	local clients=${1:-$(hostname)}
	local name=$2
	local signal=$3
	local rc=0

	do_nodes $clients "killall $signal $name"
}

test_8() {
	local clients=${CLIENTS:-$HOSTNAME}

	zconf_mount_clients $clients $MOUNT
	
	local duration=300
	[ "$SLOW" = "no" ] && duration=120
	# set duration to 900 because it takes some time to boot node
	[ "$FAILURE_MODE" = HARD ] && duration=900

	local cmd="rundbench 1 -t $duration"
	local pid=""
	do_nodesv $clients "set -x; MISSING_DBENCH_OK=$MISSING_DBENCH_OK \
		PATH=:$PATH:$LUSTRE/utils:$LUSTRE/tests/:$DBENCH_LIB \
		DBENCH_LIB=$DBENCH_LIB TESTSUITE=$TESTSUITE TESTNAME=$TESTNAME \
		LCTL=$LCTL $cmd" &
	pid=$!
	log "Started rundbench load pid=$pid ..."

	# give rundbench a chance to start, bug 24118
	sleep 2
	local elapsed=0
	local num_failovers=0
	local start_ts=$(date +%s)
	while [ $elapsed -lt $duration ]; do
		if ! check_for_process $clients rundbench; then
			error_noexit "rundbench not found on some of $clients!"
			killall_process $clients dbench
			break
		fi
		sleep 5
		replay_barrier ost1
		sleep 2 # give clients a time to do operations
		# Increment the number of failovers
		num_failovers=$((num_failovers+1))
		log "$TESTNAME fail ost1 $num_failovers times"
		fail ost1
		elapsed=$(($(date +%s) - start_ts))
	done

	wait $pid || error "rundbench load on $clients failed!"
}
run_test 8 "ost recovery; $CLIENTCOUNT clients"

complete $(basename $0) $SECONDS
check_and_cleanup_lustre
exit_status
