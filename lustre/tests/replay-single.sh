#!/bin/bash

set -e

LUSTRE=${LUSTRE:-$(dirname $0)/..}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
init_logging

ALWAYS_EXCEPT="$REPLAY_SINGLE_EXCEPT "
# bug number for skipped test: LU-13614
ALWAYS_EXCEPT+="               59"

if [ "$mds1_FSTYPE" = zfs ]; then
	ALWAYS_EXCEPT+=""
fi

if $SHARED_KEY; then
	# bug number for skipped tests: LU-9795
	ALWAYS_EXCEPT="$ALWAYS_EXCEPT   121"
fi
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

build_test_filter

CHECK_GRANT=${CHECK_GRANT:-"yes"}
GRANT_CHECK_LIST=${GRANT_CHECK_LIST:-""}

require_dsh_mds || exit 0
check_and_setup_lustre

mkdir -p $DIR

assert_DIR
rm -rf $DIR/[df][0-9]* $DIR/f.$TESTSUITE.*

# LU-482 Avert LVM and VM inability to flush caches in pre .33 kernels
if [ $LINUX_VERSION_CODE -lt $(version_code 2.6.33) ]; then
    sync
    do_facet $SINGLEMDS sync
fi

test_0a() {	# was test_0
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	rmdir $DIR/$tdir
}
run_test 0a "empty replay"

test_0b() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	# this test attempts to trigger a race in the precreation code,
	# and must run before any other objects are created on the filesystem
	fail ost1
	createmany -o $DIR/$tfile 20 || error "createmany -o $DIR/$tfile failed"
	unlinkmany $DIR/$tfile 20 || error "unlinkmany $DIR/$tfile failed"
}
run_test 0b "ensure object created after recover exists. (3284)"

test_0c() {
	replay_barrier $SINGLEMDS
	mcreate $DIR/$tfile
	umount $MOUNT
	facet_failover $SINGLEMDS
	zconf_mount $(hostname) $MOUNT || error "mount fails"
	client_up || error "post-failover df failed"
	# file shouldn't exist if replay-barrier works as expected
	rm $DIR/$tfile && error "File exists and it shouldn't"
	return 0
}
run_test 0c "check replay-barrier"

test_0d() {
	replay_barrier $SINGLEMDS
	umount $MOUNT
	facet_failover $SINGLEMDS
	zconf_mount $(hostname) $MOUNT || error "mount fails"
	client_up || error "post-failover df failed"
}
run_test 0d "expired recovery with no clients"

test_1() {
	replay_barrier $SINGLEMDS
	mcreate $DIR/$tfile
	fail $SINGLEMDS
	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	rm $DIR/$tfile
}
run_test 1 "simple create"

test_2a() {
	replay_barrier $SINGLEMDS
	touch $DIR/$tfile
	fail $SINGLEMDS
	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	rm $DIR/$tfile
}
run_test 2a "touch"

test_2b() {
	mcreate $DIR/$tfile || error "mcreate $DIR/$tfile failed"
	replay_barrier $SINGLEMDS
	touch $DIR/$tfile
	fail $SINGLEMDS
	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	rm $DIR/$tfile
}
run_test 2b "touch"

test_2c() {
	replay_barrier $SINGLEMDS
	$LFS setstripe -c $OSTCOUNT $DIR/$tfile
	fail $SINGLEMDS
	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile check failed"
}
run_test 2c "setstripe replay"

test_2d() {
	[[ "$mds1_FSTYPE" = zfs ]] &&
		[[ "$MDS1_VERSION" -lt $(version_code 2.12.51) ]] &&
		skip "requires LU-10143 fix on MDS"
	replay_barrier $SINGLEMDS
	$LFS setdirstripe -i 0 -c $MDSCOUNT $DIR/$tdir
	fail $SINGLEMDS
	$CHECKSTAT -t dir $DIR/$tdir ||
		error "$CHECKSTAT $DIR/$tdir check failed"
}
run_test 2d "setdirstripe replay"

test_2e() {
	testid=$(echo $TESTNAME | tr '_' ' ')
	#define OBD_FAIL_MDS_CLOSE_NET_REP       0x13b
	do_facet $SINGLEMDS "$LCTL set_param fail_loc=0x8000013b"
	openfile -f O_CREAT:O_EXCL $DIR/$tfile &
	sleep 1
	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	wait
	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	dmesg | tac | sed "/$testid/,$ d" | \
		grep "Open request replay failed with -17" &&
		error "open replay failed" || true
}
run_test 2e "O_CREAT|O_EXCL create replay"

test_3a() {
	local file=$DIR/$tfile
	replay_barrier $SINGLEMDS
	mcreate $file
	openfile -f O_DIRECTORY $file
	fail $SINGLEMDS
	$CHECKSTAT -t file $file ||
		error "$CHECKSTAT $file attribute check failed"
	rm $file
}
run_test 3a "replay failed open(O_DIRECTORY)"

test_3b() {
	replay_barrier $SINGLEMDS
	#define OBD_FAIL_MDS_OPEN_PACK | OBD_FAIL_ONCE
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000114"
	touch $DIR/$tfile
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"
	fail $SINGLEMDS
	$CHECKSTAT -t file $DIR/$tfile &&
		error "$CHECKSTAT $DIR/$tfile attribute check should fail"
	return 0
}
run_test 3b "replay failed open -ENOMEM"

test_3c() {
	replay_barrier $SINGLEMDS
	#define OBD_FAIL_MDS_ALLOC_OBDO | OBD_FAIL_ONCE
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000128"
	touch $DIR/$tfile
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"
	fail $SINGLEMDS

	$CHECKSTAT -t file $DIR/$tfile &&
		error "$CHECKSTAT $DIR/$tfile attribute check should fail"
	return 0
}
run_test 3c "replay failed open -ENOMEM"

test_4a() {	# was test_4
	replay_barrier $SINGLEMDS
	for i in $(seq 10); do
		echo "tag-$i" > $DIR/$tfile-$i
	done
	fail $SINGLEMDS
	for i in $(seq 10); do
		grep -q "tag-$i" $DIR/$tfile-$i || error "$tfile-$i"
	done
}
run_test 4a "|x| 10 open(O_CREAT)s"

test_4b() {
	for i in $(seq 10); do
		echo "tag-$i" > $DIR/$tfile-$i
	done
	replay_barrier $SINGLEMDS
	rm -rf $DIR/$tfile-*
	fail $SINGLEMDS
	$CHECKSTAT -t file $DIR/$tfile-* &&
		error "$CHECKSTAT $DIR/$tfile-* attribute check should fail" ||
		true
}
run_test 4b "|x| rm 10 files"

# The idea is to get past the first block of precreated files on both
# osts, and then replay.
test_5() {
	replay_barrier $SINGLEMDS
	for i in $(seq 220); do
		echo "tag-$i" > $DIR/$tfile-$i
	done
	fail $SINGLEMDS
	for i in $(seq 220); do
		grep -q "tag-$i" $DIR/$tfile-$i || error "$tfile-$i"
	done
	rm -rf $DIR/$tfile-*
	sleep 3
	# waiting for commitment of removal
}
run_test 5 "|x| 220 open(O_CREAT)"

test_6a() {	# was test_6
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	replay_barrier $SINGLEMDS
	mcreate $DIR/$tdir/$tfile
	fail $SINGLEMDS
	$CHECKSTAT -t dir $DIR/$tdir ||
		error "$CHECKSTAT $DIR/$tdir attribute check failed"
	$CHECKSTAT -t file $DIR/$tdir/$tfile ||
		error "$CHECKSTAT $DIR/$tdir/$tfile attribute check failed"
	sleep 2
	# waiting for log process thread
}
run_test 6a "mkdir + contained create"

test_6b() {
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	replay_barrier $SINGLEMDS
	rm -rf $DIR/$tdir
	fail $SINGLEMDS
	$CHECKSTAT -t dir $DIR/$tdir &&
		error "$CHECKSTAT $DIR/$tdir attribute check should fail" ||
		true
}
run_test 6b "|X| rmdir"

test_7() {
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	replay_barrier $SINGLEMDS
	mcreate $DIR/$tdir/$tfile
	fail $SINGLEMDS
	$CHECKSTAT -t dir $DIR/$tdir ||
		error "$CHECKSTAT $DIR/$tdir attribute check failed"
	$CHECKSTAT -t file $DIR/$tdir/$tfile ||
		error "$CHECKSTAT $DIR/$tdir/$tfile attribute check failed"
	rm -fr $DIR/$tdir
}
run_test 7 "mkdir |X| contained create"

test_8() {
	replay_barrier $SINGLEMDS
	multiop_bg_pause $DIR/$tfile mo_c ||
		error "multiop mknod $DIR/$tfile failed"
	MULTIPID=$!
	fail $SINGLEMDS
	ls $DIR/$tfile
	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	kill -USR1 $MULTIPID || error "multiop mknod $MULTIPID not running"
	wait $MULTIPID || error "multiop mknod $MULTIPID failed"
	rm $DIR/$tfile
}
run_test 8 "creat open |X| close"

test_9() {
	replay_barrier $SINGLEMDS
	mcreate $DIR/$tfile
	local old_inum=$(ls -i $DIR/$tfile | awk '{print $1}')
	fail $SINGLEMDS
	local new_inum=$(ls -i $DIR/$tfile | awk '{print $1}')

	echo " old_inum == $old_inum, new_inum == $new_inum"
	if [ $old_inum -eq $new_inum  ] ;
	then
		echo "old_inum and new_inum match"
	else
		echo " old_inum and new_inum do not match"
		error "old index($old_inum) does not match new index($new_inum)"
	fi
	rm $DIR/$tfile
}
run_test 9 "|X| create (same inum/gen)"

test_10() {
	mcreate $DIR/$tfile || error "mcreate $DIR/$tfile failed"
	replay_barrier $SINGLEMDS
	mv $DIR/$tfile $DIR/$tfile-2
	rm -f $DIR/$tfile
	fail $SINGLEMDS
	$CHECKSTAT $DIR/$tfile &&
		error "$CHECKSTAT $DIR/$tfile attribute check should fail"
	$CHECKSTAT $DIR/$tfile-2 ||
		error "$CHECKSTAT $DIR/$tfile-2 attribute check failed"
	rm $DIR/$tfile-2
	return 0
}
run_test 10 "create |X| rename unlink"

test_11() {
	mcreate $DIR/$tfile || error "mcreate $DIR/$tfile failed"
	echo "old" > $DIR/$tfile
	mv $DIR/$tfile $DIR/$tfile-2
	replay_barrier $SINGLEMDS
	echo "new" > $DIR/$tfile
	grep new $DIR/$tfile
	grep old $DIR/$tfile-2
	fail $SINGLEMDS
	grep new $DIR/$tfile || error "grep $DIR/$tfile failed"
	grep old $DIR/$tfile-2 || error "grep $DIR/$tfile-2 failed"
}
run_test 11 "create open write rename |X| create-old-name read"

test_12() {
	mcreate $DIR/$tfile || error "mcreate $DIR/$tfile failed"
	multiop_bg_pause $DIR/$tfile o_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile
	replay_barrier $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"

	fail $SINGLEMDS
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	return 0
}
run_test 12 "open, unlink |X| close"

# 1777 - replay open after committed chmod that would make
#        a regular open a failure
test_13() {
	mcreate $DIR/$tfile || error "mcreate $DIR/$tfile failed"
	multiop_bg_pause $DIR/$tfile O_wc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	chmod 0 $DIR/$tfile
	$CHECKSTAT -p 0 $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"

	$CHECKSTAT -s 1 -p 0 $DIR/$tfile ||
		error "second $CHECKSTAT $DIR/$tfile attribute check failed"
	rm $DIR/$tfile || error "rm $DIR/$tfile failed"
	return 0
}
run_test 13 "open chmod 0 |x| write close"

test_14() {
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile
	replay_barrier $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"

	fail $SINGLEMDS
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	return 0
}
run_test 14 "open(O_CREAT), unlink |X| close"

test_15() {
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile
	replay_barrier $SINGLEMDS
	touch $DIR/$tfile-1 || error "touch $DIR/$tfile-1 failed"
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"

	fail $SINGLEMDS
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	touch $DIR/$tfile-2 || error "touch $DIR/$tfile-2 failed"
	return 0
}
run_test 15 "open(O_CREAT), unlink |X|  touch new, close"

test_16() {
	replay_barrier $SINGLEMDS
	mcreate $DIR/$tfile
	munlink $DIR/$tfile
	mcreate $DIR/$tfile-2
	fail $SINGLEMDS
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	[ -e $DIR/$tfile-2 ] || error "file $DIR/$tfile-2 does not exist"
	munlink $DIR/$tfile-2 || error "munlink $DIR/$tfile-2 failed"
}
run_test 16 "|X| open(O_CREAT), unlink, touch new,  unlink new"

test_17() {
	replay_barrier $SINGLEMDS
	multiop_bg_pause $DIR/$tfile O_c ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	fail $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	rm $DIR/$tfile
}
run_test 17 "|X| open(O_CREAT), |replay| close"

test_18() {
	replay_barrier $SINGLEMDS
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile
	touch $DIR/$tfile-2 || error "touch $DIR/$tfile-2 failed"
	echo "pid: $pid will close"
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"

	fail $SINGLEMDS
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	[ -e $DIR/$tfile-2 ] || error "file $DIR/$tfile-2 does not exist"
	# this touch frequently fails
	touch $DIR/$tfile-3 || error "touch $DIR/$tfile-3 failed"
	munlink $DIR/$tfile-2 || error "munlink $DIR/$tfile-2 failed"
	munlink $DIR/$tfile-3 || error "munlink $DIR/$tfile-3 failed"
	return 0
}
run_test 18 "open(O_CREAT), unlink, touch new, close, touch, unlink"

# bug 1855 (a simpler form of test_11 above)
test_19() {
	replay_barrier $SINGLEMDS
	mcreate $DIR/$tfile
	echo "old" > $DIR/$tfile
	mv $DIR/$tfile $DIR/$tfile-2
	grep old $DIR/$tfile-2
	fail $SINGLEMDS
	grep old $DIR/$tfile-2 || error "grep $DIR/$tfile-2 failed"
}
run_test 19 "mcreate, open, write, rename "

test_20a() {	# was test_20
	replay_barrier $SINGLEMDS
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile

	fail $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	return 0
}
run_test 20a "|X| open(O_CREAT), unlink, replay, close (test mds_cleanup_orphans)"

test_20b() { # bug 10480
	local wait_timeout=$((TIMEOUT * 4))
	local extra=$(fs_log_size)
	local n_attempts=1

	sync_all_data
	save_layout_restore_at_exit $MOUNT
	$LFS setstripe -i 0 -c 1 $DIR

	local beforeused=$(df -P $DIR | tail -1 | awk '{ print $3 }')

	dd if=/dev/zero of=$DIR/$tfile bs=4k count=10000 &
	while [ ! -e $DIR/$tfile ] ; do
		sleep 0.01			# give dd a chance to start
	done

	$LFS getstripe $DIR/$tfile || error "$LFS getstripe $DIR/$tfile failed"
	# make it an orphan
	rm -f $DIR/$tfile || error "rm -f $DIR/$tfile failed"
	mds_evict_client
	client_up || client_up || true		# reconnect

	do_facet $SINGLEMDS "lctl set_param -n osd*.*MDT*.force_sync=1"

	fail $SINGLEMDS				# start orphan recovery
	wait_recovery_complete $SINGLEMDS || error "MDS recovery not done"
	wait_delete_completed $wait_timeout || error "delete did not finish"
	sync_all_data

	while true; do
		local afterused=$(df -P $DIR | tail -1 | awk '{ print $3 }')
		log "before $beforeused, after $afterused"

		(( $beforeused + $extra >= $afterused )) && break
		n_attempts=$((n_attempts + 1))
		[ $n_attempts -gt 3 ] &&
			error "after $afterused > before $beforeused + $extra"

		wait_zfs_commit $SINGLEMDS 5
		sync_all_data
	done
}

run_test 20b "write, unlink, eviction, replay (test mds_cleanup_orphans)"

test_20c() { # bug 10480
	multiop_bg_pause $DIR/$tfile Ow_c ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!

	ls -la $DIR/$tfile

	mds_evict_client
	client_up || client_up || true    # reconnect

	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	[ -s $DIR/$tfile ] || error "File was truncated"

	return 0
}
run_test 20c "check that client eviction does not affect file content"

test_21() {
	replay_barrier $SINGLEMDS
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile
	touch $DIR/$tfile-1 || error "touch $DIR/$tfile-1 failed"

	fail $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	touch $DIR/$tfile-2 || error "touch $DIR/$tfile-2 failed"
	return 0
}
run_test 21 "|X| open(O_CREAT), unlink touch new, replay, close (test mds_cleanup_orphans)"

test_22() {
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!

	replay_barrier $SINGLEMDS
	rm -f $DIR/$tfile

	fail $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	return 0
}
run_test 22 "open(O_CREAT), |X| unlink, replay, close (test mds_cleanup_orphans)"

test_23() {
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!

	replay_barrier $SINGLEMDS
	rm -f $DIR/$tfile
	touch $DIR/$tfile-1 || error "touch $DIR/$tfile-1 failed"

	fail $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	touch $DIR/$tfile-2 || error "touch $DIR/$tfile-2 failed"
	return 0
}
run_test 23 "open(O_CREAT), |X| unlink touch new, replay, close (test mds_cleanup_orphans)"

test_24() {
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!

	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	rm -f $DIR/$tfile
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	return 0
}
run_test 24 "open(O_CREAT), replay, unlink, close (test mds_cleanup_orphans)"

test_25() {
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile

	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	return 0
}
run_test 25 "open(O_CREAT), unlink, replay, close (test mds_cleanup_orphans)"

test_26() {
	replay_barrier $SINGLEMDS
	multiop_bg_pause $DIR/$tfile-1 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-1 failed"
	pid1=$!
	multiop_bg_pause $DIR/$tfile-2 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-2 failed"
	pid2=$!
	rm -f $DIR/$tfile-1
	rm -f $DIR/$tfile-2
	kill -USR1 $pid2 || error "second multiop $pid2 not running"
	wait $pid2 || error "second multiop $pid2 failed"

	fail $SINGLEMDS
	kill -USR1 $pid1 || error "multiop $pid1 not running"
	wait $pid1 || error "multiop $pid1 failed"
	[ -e $DIR/$tfile-1 ] && error "file $DIR/$tfile-1 should not exist"
	[ -e $DIR/$tfile-2 ] && error "file $DIR/$tfile-2 should not exist"
	return 0
}
run_test 26 "|X| open(O_CREAT), unlink two, close one, replay, close one (test mds_cleanup_orphans)"

test_27() {
	replay_barrier $SINGLEMDS
	multiop_bg_pause $DIR/$tfile-1 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-1 failed"
	pid1=$!
	multiop_bg_pause $DIR/$tfile-2 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-2 failed"
	pid2=$!
	rm -f $DIR/$tfile-1
	rm -f $DIR/$tfile-2

	fail $SINGLEMDS
	kill -USR1 $pid1 || error "multiop $pid1 not running"
	wait $pid1 || error "multiop $pid1 failed"
	kill -USR1 $pid2 || error "second multiop $pid2 not running"
	wait $pid2 || error "second multiop $pid2 failed"
	[ -e $DIR/$tfile-1 ] && error "file $DIR/$tfile-1 should not exist"
	[ -e $DIR/$tfile-2 ] && error "file $DIR/$tfile-2 should not exist"
	return 0
}
run_test 27 "|X| open(O_CREAT), unlink two, replay, close two (test mds_cleanup_orphans)"

test_28() {
	multiop_bg_pause $DIR/$tfile-1 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-1 failed"
	pid1=$!
	multiop_bg_pause $DIR/$tfile-2 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-2 failed"
	pid2=$!
	replay_barrier $SINGLEMDS
	rm -f $DIR/$tfile-1
	rm -f $DIR/$tfile-2
	kill -USR1 $pid2 || error "second multiop $pid2 not running"
	wait $pid2 || error "second multiop $pid2 failed"

	fail $SINGLEMDS
	kill -USR1 $pid1 || error "multiop $pid1 not running"
	wait $pid1 || error "multiop $pid1 failed"
	[ -e $DIR/$tfile-1 ] && error "file $DIR/$tfile-1 should not exist"
	[ -e $DIR/$tfile-2 ] && error "file $DIR/$tfile-2 should not exist"
	return 0
}
run_test 28 "open(O_CREAT), |X| unlink two, close one, replay, close one (test mds_cleanup_orphans)"

test_29() {
	multiop_bg_pause $DIR/$tfile-1 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-1 failed"
	pid1=$!
	multiop_bg_pause $DIR/$tfile-2 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-2 failed"
	pid2=$!
	replay_barrier $SINGLEMDS
	rm -f $DIR/$tfile-1
	rm -f $DIR/$tfile-2

	fail $SINGLEMDS
	kill -USR1 $pid1 || error "multiop $pid1 not running"
	wait $pid1 || error "multiop $pid1 failed"
	kill -USR1 $pid2 || error "second multiop $pid2 not running"
	wait $pid2 || error "second multiop $pid2 failed"
	[ -e $DIR/$tfile-1 ] && error "file $DIR/$tfile-1 should not exist"
	[ -e $DIR/$tfile-2 ] && error "file $DIR/$tfile-2 should not exist"
	return 0
}
run_test 29 "open(O_CREAT), |X| unlink two, replay, close two (test mds_cleanup_orphans)"

test_30() {
	multiop_bg_pause $DIR/$tfile-1 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-1 failed"
	pid1=$!
	multiop_bg_pause $DIR/$tfile-2 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-2 failed"
	pid2=$!
	rm -f $DIR/$tfile-1
	rm -f $DIR/$tfile-2

	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	kill -USR1 $pid1 || error "multiop $pid1 not running"
	wait $pid1 || error "multiop $pid1 failed"
	kill -USR1 $pid2 || error "second multiop $pid2 not running"
	wait $pid2 || error "second multiop $pid2 failed"
	[ -e $DIR/$tfile-1 ] && error "file $DIR/$tfile-1 should not exist"
	[ -e $DIR/$tfile-2 ] && error "file $DIR/$tfile-2 should not exist"
	return 0
}
run_test 30 "open(O_CREAT) two, unlink two, replay, close two (test mds_cleanup_orphans)"

test_31() {
	multiop_bg_pause $DIR/$tfile-1 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-1 failed"
	pid1=$!
	multiop_bg_pause $DIR/$tfile-2 O_tSc ||
		error "multiop_bg_pause $DIR/$tfile-2 failed"
	pid2=$!
	rm -f $DIR/$tfile-1

	replay_barrier $SINGLEMDS
	rm -f $DIR/$tfile-2
	fail $SINGLEMDS
	kill -USR1 $pid1 || error "multiop $pid1 not running"
	wait $pid1 || error "multiop $pid1 failed"
	kill -USR1 $pid2 || error "second multiop $pid2 not running"
	wait $pid2 || error "second multiop $pid2 failed"
	[ -e $DIR/$tfile-1 ] && error "file $DIR/$tfile-1 should not exist"
	[ -e $DIR/$tfile-2 ] && error "file $DIR/$tfile-2 should not exist"
	return 0
}
run_test 31 "open(O_CREAT) two, unlink one, |X| unlink one, close two (test mds_cleanup_orphans)"

# tests for bug 2104; completion without crashing is success.  The close is
# stale, but we always return 0 for close, so the app never sees it.
test_32() {
	multiop_bg_pause $DIR/$tfile O_c ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid1=$!
	multiop_bg_pause $DIR/$tfile O_c ||
		error "second multiop_bg_pause $DIR/$tfile failed"
	pid2=$!
	mds_evict_client
	client_up || client_up || error "client_up failed"
	kill -USR1 $pid1 || error "multiop $pid1 not running"
	kill -USR1 $pid2 || error "second multiop $pid2 not running"
	wait $pid1 || error "multiop $pid1 failed"
	wait $pid2 || error "second multiop $pid2 failed"
	return 0
}
run_test 32 "close() notices client eviction; close() after client eviction"

test_33a() {
	createmany -o $DIR/$tfile-%d 10 ||
		error "createmany create $DIR/$tfile failed"
	replay_barrier_nosync $SINGLEMDS
	fail_abort $SINGLEMDS
	# recreate shouldn't fail
	createmany -o $DIR/$tfile--%d 10 ||
		error "createmany recreate $DIR/$tfile failed"
	rm $DIR/$tfile-* -f
	return 0
}
run_test 33a "fid seq shouldn't be reused after abort recovery"

test_33b() {
	#define OBD_FAIL_SEQ_ALLOC                          0x1311
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x1311"

	createmany -o $DIR/$tfile-%d 10
	replay_barrier_nosync $SINGLEMDS
	fail_abort $SINGLEMDS
	# recreate shouldn't fail
	createmany -o $DIR/$tfile--%d 10 ||
		error "createmany recreate $DIR/$tfile failed"
	rm $DIR/$tfile-* -f
	return 0
}
run_test 33b "test fid seq allocation"

test_34() {
	multiop_bg_pause $DIR/$tfile O_c ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile

	replay_barrier $SINGLEMDS
	fail_abort $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	sync
	return 0
}
run_test 34 "abort recovery before client does replay (test mds_cleanup_orphans)"

# bug 2278 - generate one orphan on OST, then destroy it during recovery from llog
test_35() {
	touch $DIR/$tfile || error "touch $DIR/$tfile failed"

	#define OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000119"
	rm -f $DIR/$tfile &
	sleep 1
	sync
	sleep 1
	# give a chance to remove from MDS
	fail_abort $SINGLEMDS
	$CHECKSTAT -t file $DIR/$tfile &&
		error "$CHECKSTAT $DIR/$tfile attribute check should fail" ||
		true
}
run_test 35 "test recovery from llog for unlink op"

# b=2432 resent cancel after replay uses wrong cookie,
# so don't resend cancels
test_36() {
	replay_barrier $SINGLEMDS
	touch $DIR/$tfile
	checkstat $DIR/$tfile
	facet_failover $SINGLEMDS
	cancel_lru_locks mdc
	if $LCTL dk | grep "stale lock .*cookie"; then
		error "cancel after replay failed"
	fi
}
run_test 36 "don't resend cancel"

# b=2368
# directory orphans can't be unlinked from PENDING directory
test_37() {
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $tdir failed"
	rmdir $DIR/$tdir/$tfile 2>/dev/null
	multiop_bg_pause $DIR/$tdir/$tfile dD_c ||
		error "multiop_bg_pause $tfile failed"
	pid=$!
	rmdir $DIR/$tdir/$tfile

	replay_barrier $SINGLEMDS
	# clear the dmesg buffer so we only see errors from this recovery
	do_facet $SINGLEMDS dmesg -c >/dev/null
	fail_abort $SINGLEMDS
	kill -USR1 $pid || error "multiop $pid not running"
	do_facet $SINGLEMDS dmesg | grep "error unlinking orphan" &&
		error "error unlinking files"
	wait $pid || error "multiop $pid failed"
	sync
	return 0
}
run_test 37 "abort recovery before client does replay (test mds_cleanup_orphans for directories)"

test_38() {
	createmany -o $DIR/$tfile-%d 800 ||
		error "createmany -o $DIR/$tfile failed"
	unlinkmany $DIR/$tfile-%d 0 400 || error "unlinkmany $DIR/$tfile failed"
	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	unlinkmany $DIR/$tfile-%d 400 400 ||
		error "unlinkmany $DIR/$tfile 400 failed"
	sleep 2
	$CHECKSTAT -t file $DIR/$tfile-* &&
		error "$CHECKSTAT $DIR/$tfile-* attribute check should fail" ||
		true
}
run_test 38 "test recovery from unlink llog (test llog_gen_rec) "

test_39() { # bug 4176
	createmany -o $DIR/$tfile-%d 800 ||
		error "createmany -o $DIR/$tfile failed"
	replay_barrier $SINGLEMDS
	unlinkmany $DIR/$tfile-%d 0 400
	fail $SINGLEMDS
	unlinkmany $DIR/$tfile-%d 400 400 ||
		error "unlinkmany $DIR/$tfile 400 failed"
	sleep 2
	$CHECKSTAT -t file $DIR/$tfile-* &&
		error "$CHECKSTAT $DIR/$tfile-* attribute check should fail" ||
		true
}
run_test 39 "test recovery from unlink llog (test llog_gen_rec) "

count_ost_writes() {
    lctl get_param -n osc.*.stats | awk -vwrites=0 '/ost_write/ { writes += $2 } END { print writes; }'
}

#b=2477,2532
test_40(){
	# always need connection to MDS to verify layout during IO. LU-2628.
	lctl get_param mdc.*.connect_flags | grep -q layout_lock &&
		skip "layout_lock needs MDS connection for IO" && return 0

	$LCTL mark multiop $MOUNT/$tfile OS_c
	multiop $MOUNT/$tfile OS_c  &
	PID=$!
	writeme -s $MOUNT/${tfile}-2 &
	WRITE_PID=$!
	sleep 1
	facet_failover $SINGLEMDS
	#define OBD_FAIL_MDS_CONNECT_NET         0x117
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000117"
	kill -USR1 $PID
	stat1=$(count_ost_writes)
	sleep $TIMEOUT
	stat2=$(count_ost_writes)
	echo "$stat1, $stat2"
	if [ $stat1 -lt $stat2 ]; then
		echo "writes continuing during recovery"
		RC=0
	else
		echo "writes not continuing during recovery, bug 2477"
		RC=4
	fi
	echo "waiting for writeme $WRITE_PID"
	kill $WRITE_PID
	wait $WRITE_PID

	echo "waiting for multiop $PID"
	wait $PID || error "multiop $PID failed"
	do_facet client munlink $MOUNT/$tfile  ||
		error "munlink $MOUNT/$tfile failed"
	do_facet client munlink $MOUNT/${tfile}-2  ||
		error "munlink $MOUNT/$tfile-2 failed"
	return $RC
}
run_test 40 "cause recovery in ptlrpc, ensure IO continues"

#b=2814
# make sure that a read to one osc doesn't try to double-unlock its page just
# because another osc is invalid.  trigger_group_io used to mistakenly return
# an error if any oscs were invalid even after having successfully put rpcs
# on valid oscs.  This was fatal if the caller was ll_readpage who unlocked
# the page, guarnateeing that the unlock from the RPC completion would
# assert on trying to unlock the unlocked page.
test_41() {
	[ $OSTCOUNT -lt 2 ] && skip_env "needs >= 2 OSTs" && return

	local f=$MOUNT/$tfile
	# make sure the start of the file is ost1
	$LFS setstripe -S $((128 * 1024)) -i 0 $f
	do_facet client dd if=/dev/zero of=$f bs=4k count=1 ||
		error "dd on client failed"
	cancel_lru_locks osc
	# fail ost2 and read from ost1
	local mdtosc=$(get_mdtosc_proc_path $SINGLEMDS $ost2_svc)
	local osc2dev=$(do_facet $SINGLEMDS "lctl get_param -n devices" |
		grep $mdtosc | awk '{print $1}')
	[ -z "$osc2dev" ] && echo "OST: $ost2_svc" &&
		lctl get_param -n devices &&
		error "OST 2 $osc2dev does not exist"
	do_facet $SINGLEMDS $LCTL --device $osc2dev deactivate ||
		error "deactive device on $SINGLEMDS failed"
	do_facet client dd if=$f of=/dev/null bs=4k count=1 ||
		error "second dd on client failed"
	do_facet $SINGLEMDS $LCTL --device $osc2dev activate ||
		error "active device on $SINGLEMDS failed"
	return 0
}
run_test 41 "read from a valid osc while other oscs are invalid"

# test MDS recovery after ost failure
test_42() {
	blocks=$(df -P $MOUNT | tail -n 1 | awk '{ print $2 }')
	createmany -o $DIR/$tfile-%d 800 ||
		error "createmany -o $DIR/$tfile failed"
	replay_barrier ost1
	unlinkmany $DIR/$tfile-%d 0 400
	debugsave
	lctl set_param debug=-1
	facet_failover ost1

	# osc is evicted, fs is smaller (but only with failout OSTs (bug 7287)
	#blocks_after=`df -P $MOUNT | tail -n 1 | awk '{ print $2 }'`
	#[ $blocks_after -lt $blocks ] || return 1
	echo "wait for MDS to timeout and recover"
	sleep $((TIMEOUT * 2))
	debugrestore
	unlinkmany $DIR/$tfile-%d 400 400 ||
		error "unlinkmany $DIR/$tfile 400 failed"
	$CHECKSTAT -t file $DIR/$tfile-* &&
		error "$CHECKSTAT $DIR/$tfile-* attribute check should fail" ||
		true
}
run_test 42 "recovery after ost failure"

# timeout in MDS/OST recovery RPC will LBUG MDS
test_43() { # bug 2530
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	replay_barrier $SINGLEMDS

	# OBD_FAIL_OST_CREATE_NET 0x204
	do_facet ost1 "lctl set_param fail_loc=0x80000204"
	fail $SINGLEMDS
	sleep 10

	return 0
}
run_test 43 "mds osc import failure during recovery; don't LBUG"

test_44a() { # was test_44
	local at_max_saved=0

	local mdcdev=$($LCTL dl |
		awk "/${FSNAME}-MDT0000-mdc-/ {if (\$2 == \"UP\") {print \$1}}")
	[ "$mdcdev" ] || error "${FSNAME}-MDT0000-mdc- not UP"
	[ $(echo $mdcdev | wc -w) -eq 1 ] ||
		{ $LCTL dl; error "looking for mdcdev=$mdcdev"; }

	# adaptive timeouts slow this way down
	if at_is_enabled; then
		at_max_saved=$(at_max_get mds)
		at_max_set 40 mds
	fi

	for i in $(seq 1 10); do
		echo "$i of 10 ($(date +%s))"
		do_facet $SINGLEMDS \
			"lctl get_param -n md[ts].*.mdt.timeouts | grep service"
		#define OBD_FAIL_TGT_CONN_RACE     0x701
		do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000701"
		# lctl below may fail, it is valid case
		$LCTL --device $mdcdev recover
		$LFS df $MOUNT
	done
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"
	[ $at_max_saved -ne 0 ] && at_max_set $at_max_saved mds
	return 0
}
run_test 44a "race in target handle connect"

test_44b() {
	local mdcdev=$($LCTL dl |
		awk "/${FSNAME}-MDT0000-mdc-/ {if (\$2 == \"UP\") {print \$1}}")
	[ "$mdcdev" ] || error "${FSNAME}-MDT0000-mdc not up"
	[ $(echo $mdcdev | wc -w) -eq 1 ] ||
		{ echo mdcdev=$mdcdev; $LCTL dl;
		  error "more than one ${FSNAME}-MDT0000-mdc"; }

	for i in $(seq 1 10); do
		echo "$i of 10 ($(date +%s))"
		do_facet $SINGLEMDS \
			"lctl get_param -n md[ts].*.mdt.timeouts | grep service"
		#define OBD_FAIL_TGT_DELAY_RECONNECT 0x704
		do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000704"
		# lctl below may fail, it is valid case
		$LCTL --device $mdcdev recover
		df $MOUNT
	done
	return 0
}
run_test 44b "race in target handle connect"

test_44c() {
	replay_barrier $SINGLEMDS
	createmany -m $DIR/$tfile-%d 100 || error "failed to create directories"
	#define OBD_FAIL_TGT_RCVG_FLAG 0x712
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000712"
	fail_abort $SINGLEMDS
	unlinkmany $DIR/$tfile-%d 100 && error "unliked after fail abort"
	fail $SINGLEMDS
	unlinkmany $DIR/$tfile-%d 100 && error "unliked after fail"
	return 0
}
run_test 44c "race in target handle connect"

# Handle failed close
test_45() {
	local mdcdev=$($LCTL get_param -n devices |
		awk "/ ${FSNAME}-MDT0000-mdc-/ {print \$1}")
	[ "$mdcdev" ] || error "${FSNAME}-MDT0000-mdc not up"
	[ $(echo $mdcdev | wc -w) -eq 1 ] ||
		{ echo mdcdev=$mdcdev; $LCTL dl;
		  error "more than one ${FSNAME}-MDT0000-mdc"; }

	$LCTL --device $mdcdev recover ||
		error "$LCTL --device $mdcdev recover failed"

	multiop_bg_pause $DIR/$tfile O_c ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!

	# This will cause the CLOSE to fail before even
	# allocating a reply buffer
	$LCTL --device $mdcdev deactivate ||
		error "$LCTL --device $mdcdev deactivate failed"

	# try the close
	kill -USR1 $pid || error "multiop $pid not running"
	wait $pid || error "multiop $pid failed"

	$LCTL --device $mdcdev activate ||
		error "$LCTL --device $mdcdev activate failed"
	sleep 1

	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	return 0
}
run_test 45 "Handle failed close"

test_46() {
	drop_reply "touch $DIR/$tfile"
	fail $SINGLEMDS
	# ironically, the previous test, 45, will cause a real forced close,
	# so just look for one for this test
	local FID=$($LFS path2fid $tfile)
	$LCTL dk | grep -i "force closing file handle $FID" &&
		error "found force closing in dmesg"
	return 0
}
run_test 46 "Don't leak file handle after open resend (3325)"

test_47() { # bug 2824
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	# create some files to make sure precreate has been done on all
	# OSTs. (just in case this test is run independently)
	createmany -o $DIR/$tfile 20  ||
		error "createmany create $DIR/$tfile failed"

	# OBD_FAIL_OST_CREATE_NET 0x204
	fail ost1
	do_facet ost1 "lctl set_param fail_loc=0x80000204"
	client_up || error "client_up failed"

	# let the MDS discover the OST failure, attempt to recover, fail
	# and recover again.
	sleep $((3 * TIMEOUT))

	# Without 2824, this createmany would hang
	createmany -o $DIR/$tfile 20 ||
		error "createmany recraete $DIR/$tfile failed"
	unlinkmany $DIR/$tfile 20 || error "unlinkmany $DIR/$tfile failed"

	return 0
}
run_test 47 "MDS->OSC failure during precreate cleanup (2824)"

test_48() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0
	[ "$OSTCOUNT" -lt "2" ] && skip_env "needs >= 2 OSTs" && return

	replay_barrier $SINGLEMDS
	createmany -o $DIR/$tfile 20  ||
		error "createmany -o $DIR/$tfile failed"
	# OBD_FAIL_OST_EROFS 0x216
	facet_failover $SINGLEMDS
	do_facet ost1 "lctl set_param fail_loc=0x80000216"
	client_up || error "client_up failed"

	# let the MDS discover the OST failure, attempt to recover, fail
	# and recover again.
	sleep $((3 * TIMEOUT))

	createmany -o $DIR/$tfile 20 20 ||
		error "createmany recraete $DIR/$tfile failed"
	unlinkmany $DIR/$tfile 40 || error "unlinkmany $DIR/$tfile failed"
	return 0
}
run_test 48 "MDS->OSC failure during precreate cleanup (2824)"

test_50() {
	local mdtosc=$(get_mdtosc_proc_path $SINGLEMDS $ost1_svc)
	local oscdev=$(do_facet $SINGLEMDS "lctl get_param -n devices" |
		grep $mdtosc | awk '{print $1}')
	[ "$oscdev" ] || error "could not find OSC device on MDS"
	do_facet $SINGLEMDS $LCTL --device $oscdev recover ||
		error "OSC device $oscdev recovery failed"
	do_facet $SINGLEMDS $LCTL --device $oscdev recover ||
		error "second OSC device $oscdev recovery failed"
	# give the mds_lov_sync threads a chance to run
	sleep 5
}
run_test 50 "Double OSC recovery, don't LASSERT (3812)"

# b3764 timed out lock replay
test_52() {
	[ "$MDS1_VERSION" -lt $(version_code 2.6.90) ] &&
		skip "MDS prior to 2.6.90 handle LDLM_REPLY_NET incorrectly"

	touch $DIR/$tfile || error "touch $DIR/$tfile failed"
	cancel_lru_locks mdc

	multiop_bg_pause $DIR/$tfile s_s || error "multiop $DIR/$tfile failed"
	mpid=$!

	#define OBD_FAIL_MDS_LDLM_REPLY_NET	0x157
	lctl set_param -n ldlm.cancel_unused_locks_before_replay "0"
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000157"

	fail $SINGLEMDS || error "fail $SINGLEMDS failed"
	kill -USR1 $mpid
	wait $mpid || error "multiop_bg_pause pid failed"

	do_facet $SINGLEMDS "lctl set_param fail_loc=0x0"
	lctl set_param fail_loc=0x0
	lctl set_param -n ldlm.cancel_unused_locks_before_replay "1"
	rm -f $DIR/$tfile
}
run_test 52 "time out lock replay (3764)"

# bug 3462 - simultaneous MDC requests
test_53a() {
	[[ $(lctl get_param mdc.*.import |
	     grep "connect_flags:.*multi_mod_rpc") ]] ||
		{ skip "Need MDC with 'multi_mod_rpcs' feature"; return 0; }

	cancel_lru_locks mdc    # cleanup locks from former test cases
	mkdir_on_mdt0 $DIR/${tdir}-1 || error "mkdir $DIR/${tdir}-1 failed"
	mkdir_on_mdt0 $DIR/${tdir}-2 || error "mkdir $DIR/${tdir}-2 failed"
	multiop $DIR/${tdir}-1/f O_c &
	close_pid=$!
	# give multiop a change to open
	sleep 1

	#define OBD_FAIL_MDS_CLOSE_NET 0x115
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000115"
	kill -USR1 $close_pid
	cancel_lru_locks mdc    # force the close
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"

	mcreate $DIR/${tdir}-2/f || error "mcreate $DIR/${tdir}-2/f failed"

	# close should still be here
	[ -d /proc/$close_pid ] || error "close_pid doesn't exist"

	replay_barrier_nodf $SINGLEMDS
	fail $SINGLEMDS
	wait $close_pid || error "close_pid $close_pid failed"

	$CHECKSTAT -t file $DIR/${tdir}-1/f ||
		error "$CHECKSTAT $DIR/${tdir}-1/f attribute check failed"
	$CHECKSTAT -t file $DIR/${tdir}-2/f ||
		error "$CHECKSTAT $DIR/${tdir}-2/f attribute check failed"
	rm -rf $DIR/${tdir}-*
}
run_test 53a "|X| close request while two MDC requests in flight"

test_53b() {
	cancel_lru_locks mdc    # cleanup locks from former test cases

	mkdir_on_mdt0 $DIR/${tdir}-1 || error "mkdir $DIR/${tdir}-1 failed"
	mkdir_on_mdt0 $DIR/${tdir}-2 || error "mkdir $DIR/${tdir}-2 failed"
	multiop_bg_pause $DIR/${tdir}-1/f O_c ||
		error "multiop_bg_pause $DIR/${tdir}-1/f failed"
	close_pid=$!

	#define OBD_FAIL_MDS_REINT_NET 0x107
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000107"
	mcreate $DIR/${tdir}-2/f &
	open_pid=$!
	sleep 1

	do_facet $SINGLEMDS "lctl set_param fail_loc=0"
	kill -USR1 $close_pid
	cancel_lru_locks mdc    # force the close
	wait $close_pid || error "close_pid $close_pid failed"
	# open should still be here
	[ -d /proc/$open_pid ] || error "open_pid doesn't exist"

	replay_barrier_nodf $SINGLEMDS
	fail $SINGLEMDS
	wait $open_pid || error "open_pid failed"

	$CHECKSTAT -t file $DIR/${tdir}-1/f ||
		error "$CHECKSTAT $DIR/${tdir}-1/f attribute check failed"
	$CHECKSTAT -t file $DIR/${tdir}-2/f ||
		error "$CHECKSTAT $DIR/${tdir}-2/f attribute check failed"
	rm -rf $DIR/${tdir}-*
}
run_test 53b "|X| open request while two MDC requests in flight"

test_53c() {
	cancel_lru_locks mdc    # cleanup locks from former test cases

	mkdir_on_mdt0 $DIR/${tdir}-1 || error "mkdir $DIR/${tdir}-1 failed"
	mkdir_on_mdt0 $DIR/${tdir}-2 || error "mkdir $DIR/${tdir}-2 failed"
	multiop $DIR/${tdir}-1/f O_c &
	close_pid=$!

	#define OBD_FAIL_MDS_REINT_NET 0x107
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000107"
	mcreate $DIR/${tdir}-2/f &
	open_pid=$!
	sleep 1

	#define OBD_FAIL_MDS_CLOSE_NET 0x115
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000115"
	kill -USR1 $close_pid
	cancel_lru_locks mdc    # force the close

	#bz20647: make sure all pids exist before failover
	[ -d /proc/$close_pid ] || error "close_pid doesn't exist"
	[ -d /proc/$open_pid ] || error "open_pid doesn't exists"
	replay_barrier_nodf $SINGLEMDS
	fail_nodf $SINGLEMDS
	wait $open_pid || error "open_pid failed"
	sleep 2
	# close should be gone
	[ -d /proc/$close_pid ] && error "close_pid should not exist"
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"

	$CHECKSTAT -t file $DIR/${tdir}-1/f ||
		error "$CHECKSTAT $DIR/${tdir}-1/f attribute check failed"
	$CHECKSTAT -t file $DIR/${tdir}-2/f ||
		error "$CHECKSTAT $DIR/${tdir}-2/f attribute check failed"
	rm -rf $DIR/${tdir}-*
}
run_test 53c "|X| open request and close request while two MDC requests in flight"

test_53d() {
	[[ $(lctl get_param mdc.*.import |
	     grep "connect_flags:.*multi_mod_rpc") ]] ||
		{ skip "Need MDC with 'multi_mod_rpcs' feature"; return 0; }

	cancel_lru_locks mdc    # cleanup locks from former test cases

	mkdir_on_mdt0 $DIR/${tdir}-1 || error "mkdir $DIR/${tdir}-1 failed"
	mkdir_on_mdt0 $DIR/${tdir}-2 || error "mkdir $DIR/${tdir}-2 failed"
	multiop $DIR/${tdir}-1/f O_c &
	close_pid=$!
	# give multiop a chance to open
	sleep 1

	#define OBD_FAIL_MDS_CLOSE_NET_REP 0x13b
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x8000013b"
	kill -USR1 $close_pid
	cancel_lru_locks mdc    # force the close
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"
	mcreate $DIR/${tdir}-2/f || error "mcreate $DIR/${tdir}-2/f failed"

	# close should still be here
	[ -d /proc/$close_pid ] || error "close_pid doesn't exist"
	fail $SINGLEMDS
	wait $close_pid || error "close_pid failed"

	$CHECKSTAT -t file $DIR/${tdir}-1/f ||
		error "$CHECKSTAT $DIR/${tdir}-1/f attribute check failed"
	$CHECKSTAT -t file $DIR/${tdir}-2/f ||
		error "$CHECKSTAT $DIR/${tdir}-2/f attribute check failed"
	rm -rf $DIR/${tdir}-*
}
run_test 53d "close reply while two MDC requests in flight"

test_53e() {
	cancel_lru_locks mdc    # cleanup locks from former test cases

	mkdir_on_mdt0 $DIR/${tdir}-1 || error "mkdir $DIR/${tdir}-1 failed"
	mkdir_on_mdt0 $DIR/${tdir}-2 || error "mkdir $DIR/${tdir}-2 failed"
	multiop $DIR/${tdir}-1/f O_c &
	close_pid=$!

	#define OBD_FAIL_MDS_REINT_NET_REP 0x119
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x119"
	mcreate $DIR/${tdir}-2/f &
	open_pid=$!
	sleep 1

	do_facet $SINGLEMDS "lctl set_param fail_loc=0"
	kill -USR1 $close_pid
	cancel_lru_locks mdc    # force the close
	wait $close_pid || error "close_pid failed"
	# open should still be here
	[ -d /proc/$open_pid ] || error "open_pid doesn't exists"

	replay_barrier_nodf $SINGLEMDS
	fail $SINGLEMDS
	wait $open_pid || error "open_pid failed"

	$CHECKSTAT -t file $DIR/${tdir}-1/f ||
		error "$CHECKSTAT $DIR/${tdir}-1/f attribute check failed"
	$CHECKSTAT -t file $DIR/${tdir}-2/f ||
		error "$CHECKSTAT $DIR/${tdir}-2/f attribute check failed"
	rm -rf $DIR/${tdir}-*
}
run_test 53e "|X| open reply while two MDC requests in flight"

test_53f() {
	cancel_lru_locks mdc    # cleanup locks from former test cases

	mkdir_on_mdt0 $DIR/${tdir}-1 || error "mkdir $DIR/${tdir}-1 failed"
	mkdir_on_mdt0 $DIR/${tdir}-2 || error "mkdir $DIR/${tdir}-2 failed"
	multiop $DIR/${tdir}-1/f O_c &
	close_pid=$!

	#define OBD_FAIL_MDS_REINT_NET_REP 0x119
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x119"
	mcreate $DIR/${tdir}-2/f &
	open_pid=$!
	sleep 1

	#define OBD_FAIL_MDS_CLOSE_NET_REP 0x13b
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x8000013b"
	kill -USR1 $close_pid
	cancel_lru_locks mdc    # force the close

	#bz20647: make sure all pids are exists before failover
	[ -d /proc/$close_pid ] || error "close_pid doesn't exist"
	[ -d /proc/$open_pid ] || error "open_pid doesn't exists"
	replay_barrier_nodf $SINGLEMDS
	fail_nodf $SINGLEMDS
	wait $open_pid || error "open_pid failed"
	sleep 2
	# close should be gone
	[ -d /proc/$close_pid ] && error "close_pid should not exist"
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"

	$CHECKSTAT -t file $DIR/${tdir}-1/f ||
		error "$CHECKSTAT $DIR/${tdir}-1/f attribute check failed"
	$CHECKSTAT -t file $DIR/${tdir}-2/f ||
		error "$CHECKSTAT $DIR/${tdir}-2/f attribute check failed"
	rm -rf $DIR/${tdir}-*
}
run_test 53f "|X| open reply and close reply while two MDC requests in flight"

test_53g() {
	cancel_lru_locks mdc    # cleanup locks from former test cases

	mkdir_on_mdt0 $DIR/${tdir}-1 || error "mkdir $DIR/${tdir}-1 failed"
	mkdir_on_mdt0 $DIR/${tdir}-2 || error "mkdir $DIR/${tdir}-2 failed"
	multiop $DIR/${tdir}-1/f O_c &
	close_pid=$!

	#define OBD_FAIL_MDS_REINT_NET_REP 0x119
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x119"
	mcreate $DIR/${tdir}-2/f &
	open_pid=$!
	sleep 1

	#define OBD_FAIL_MDS_CLOSE_NET 0x115
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000115"
	kill -USR1 $close_pid
	cancel_lru_locks mdc    # force the close
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"

	#bz20647: make sure all pids are exists before failover
	[ -d /proc/$close_pid ] || error "close_pid doesn't exist"
	[ -d /proc/$open_pid ] || error "open_pid doesn't exists"
	replay_barrier_nodf $SINGLEMDS
	fail_nodf $SINGLEMDS
	wait $open_pid || error "open_pid failed"
	sleep 2
	# close should be gone
	[ -d /proc/$close_pid ] && error "close_pid should not exist"

	$CHECKSTAT -t file $DIR/${tdir}-1/f ||
		error "$CHECKSTAT $DIR/${tdir}-1/f attribute check failed"
	$CHECKSTAT -t file $DIR/${tdir}-2/f ||
		error "$CHECKSTAT $DIR/${tdir}-2/f attribute check failed"
	rm -rf $DIR/${tdir}-*
}
run_test 53g "|X| drop open reply and close request while close and open are both in flight"

test_53h() {
	cancel_lru_locks mdc    # cleanup locks from former test cases

	mkdir_on_mdt0 $DIR/${tdir}-1 || error "mkdir $DIR/${tdir}-1 failed"
	mkdir_on_mdt0 $DIR/${tdir}-2 || error "mkdir $DIR/${tdir}-2 failed"
	multiop $DIR/${tdir}-1/f O_c &
	close_pid=$!

	#define OBD_FAIL_MDS_REINT_NET 0x107
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000107"
	mcreate $DIR/${tdir}-2/f &
	open_pid=$!
	sleep 1

	#define OBD_FAIL_MDS_CLOSE_NET_REP 0x13b
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x8000013b"
	kill -USR1 $close_pid
	cancel_lru_locks mdc    # force the close
	sleep 1

	#bz20647: make sure all pids are exists before failover
	[ -d /proc/$close_pid ] || error "close_pid doesn't exist"
	[ -d /proc/$open_pid ] || error "open_pid doesn't exists"
	replay_barrier_nodf $SINGLEMDS
	fail_nodf $SINGLEMDS
	wait $open_pid || error "open_pid failed"
	sleep 2
	# close should be gone
	[ -d /proc/$close_pid ] && error "close_pid should not exist"
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"

	$CHECKSTAT -t file $DIR/${tdir}-1/f ||
		error "$CHECKSTAT $DIR/${tdir}-1/f attribute check failed"
	$CHECKSTAT -t file $DIR/${tdir}-2/f ||
		error "$CHECKSTAT $DIR/${tdir}-2/f attribute check failed"
	rm -rf $DIR/${tdir}-*
}
run_test 53h "open request and close reply while two MDC requests in flight"

#b3761 ASSERTION(hash != 0) failed
test_55() {
# OBD_FAIL_MDS_OPEN_CREATE | OBD_FAIL_ONCE
    do_facet $SINGLEMDS "lctl set_param fail_loc=0x8000012b"
    touch $DIR/$tfile &
    # give touch a chance to run
    sleep 5
    do_facet $SINGLEMDS "lctl set_param fail_loc=0x0"
    rm $DIR/$tfile
    return 0
}
run_test 55 "let MDS_CHECK_RESENT return the original return code instead of 0"

#b3440 ASSERTION(rec->ur_fid2->id) failed
test_56() {
    ln -s foo $DIR/$tfile
    replay_barrier $SINGLEMDS
    #drop_reply "cat $DIR/$tfile"
    fail $SINGLEMDS
    sleep 10
}
run_test 56 "don't replay a symlink open request (3440)"

#recovery one mds-ost setattr from llog
test_57() {
	#define OBD_FAIL_MDS_OST_SETATTR       0x12c
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x8000012c"
	touch $DIR/$tfile || error "touch $DIR/$tfile failed"
	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	wait_recovery_complete $SINGLEMDS || error "MDS recovery is not done"
	wait_mds_ost_sync || error "wait_mds_ost_sync failed"
	$CHECKSTAT -t file $DIR/$tfile ||
		error "$CHECKSTAT $DIR/$tfile attribute check failed"
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x0"
	rm $DIR/$tfile
}
run_test 57 "test recovery from llog for setattr op"

cleanup_58() {
	zconf_umount $(hostname) $MOUNT2
	trap - EXIT
}

#recovery many mds-ost setattr from llog
test_58a() {
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	#define OBD_FAIL_MDS_OST_SETATTR       0x12c
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x8000012c"
	createmany -o $DIR/$tdir/$tfile-%d 2500
	replay_barrier $SINGLEMDS
	fail $SINGLEMDS
	sleep 2
	$CHECKSTAT -t file $DIR/$tdir/$tfile-* >/dev/null ||
		error "$CHECKSTAT $DIR/$tfile-* attribute check failed"
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x0"
	unlinkmany $DIR/$tdir/$tfile-%d 2500 ||
		error "unlinkmany $DIR/$tfile failed"
	rmdir $DIR/$tdir
}
run_test 58a "test recovery from llog for setattr op (test llog_gen_rec)"

test_58b() {
	local orig
	local new

	trap cleanup_58 EXIT

	large_xattr_enabled &&
		orig="$(generate_string $(max_xattr_size))" || orig="bar"
	# Original extended attribute can be long. Print a small version of
	# attribute if an error occurs
	local sm_msg=$(printf "%.9s" $orig)

	mount_client $MOUNT2 || error "mount_client on $MOUNT2 failed"
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	touch $DIR/$tdir/$tfile || error "touch $DIR/$tdir/$tfile failed"
	replay_barrier $SINGLEMDS
	setfattr -n trusted.foo -v $orig $DIR/$tdir/$tfile
	fail $SINGLEMDS
	new=$(get_xattr_value trusted.foo $MOUNT2/$tdir/$tfile)
	[[ "$new" = "$orig" ]] ||
		error "xattr set ($sm_msg...) differs from xattr get ($new)"
	rm -f $DIR/$tdir/$tfile
	rmdir $DIR/$tdir
	cleanup_58
	wait_clients_import_state ${CLIENTS:-$HOSTNAME} "mgs" FULL
}
run_test 58b "test replay of setxattr op"

test_58c() { # bug 16570
	local orig
	local orig1
	local new

	trap cleanup_58 EXIT

	if large_xattr_enabled; then
		local xattr_size=$(max_xattr_size)
		orig="$(generate_string $((xattr_size / 2)))"
		orig1="$(generate_string $xattr_size)"
	else
		orig="bar"
		orig1="bar1"
	fi

	# PING_INTERVAL max(obd_timeout / 4, 1U)
	sleep $((TIMEOUT / 4))

	# Original extended attribute can be long. Print a small version of
	# attribute if an error occurs
	local sm_msg=$(printf "%.9s" $orig)
	local sm_msg1=$(printf "%.9s" $orig1)

	mount_client $MOUNT2 || error "mount_client on $MOUNT2 failed"
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	touch $DIR/$tdir/$tfile || error "touch $DIR/$tdir/$tfile failed"
	drop_request "setfattr -n trusted.foo -v $orig $DIR/$tdir/$tfile" ||
		error "drop_request for setfattr failed"
	new=$(get_xattr_value trusted.foo $MOUNT2/$tdir/$tfile)
	[[ "$new" = "$orig" ]] ||
		error "xattr set ($sm_msg...) differs from xattr get ($new)"
	drop_reint_reply "setfattr -n trusted.foo1 \
			  -v $orig1 $DIR/$tdir/$tfile" ||
		error "drop_reint_reply for setfattr failed"
	new=$(get_xattr_value trusted.foo1 $MOUNT2/$tdir/$tfile)
	[[ "$new" = "$orig1" ]] ||
		error "second xattr set ($sm_msg1...) differs xattr get ($new)"
	rm -f $DIR/$tdir/$tfile
	rmdir $DIR/$tdir
	cleanup_58
}
run_test 58c "resend/reconstruct setxattr op"

# log_commit_thread vs filter_destroy race used to lead to import use after free
# bug 11658
test_59() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	createmany -o $DIR/$tdir/$tfile-%d 200 ||
		error "createmany create files failed"
	sync
	unlinkmany $DIR/$tdir/$tfile-%d 200 ||
		error "unlinkmany $DIR/$tdir/$tfile failed"
	#define OBD_FAIL_PTLRPC_DELAY_RECOV       0x507
	do_facet ost1 "lctl set_param fail_loc=0x507"
	fail ost1
	fail $SINGLEMDS
	do_facet ost1 "lctl set_param fail_loc=0x0"
	sleep 20
	rmdir $DIR/$tdir
}
run_test 59 "test log_commit_thread vs filter_destroy race"

# race between add unlink llog vs cat log init in post_recovery (only for b1_6)
# bug 12086: should no oops and No ctxt error for this test
test_60() {
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	createmany -o $DIR/$tdir/$tfile-%d 200 ||
		error "createmany create files failed"
	replay_barrier $SINGLEMDS
	unlinkmany $DIR/$tdir/$tfile-%d 0 100
	fail $SINGLEMDS
	unlinkmany $DIR/$tdir/$tfile-%d 100 100
	local no_ctxt=$(dmesg | grep "No ctxt")
	[ -z "$no_ctxt" ] || error "ctxt is not initialized in recovery"
}
run_test 60 "test llog post recovery init vs llog unlink"

#test race  llog recovery thread vs llog cleanup
test_61a() {	# was test_61
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	mkdir $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	createmany -o $DIR/$tdir/$tfile-%d 800 ||
		error "createmany create files failed"
	replay_barrier ost1
	unlinkmany $DIR/$tdir/$tfile-%d 800
	#   OBD_FAIL_OST_LLOG_RECOVERY_TIMEOUT 0x221
	set_nodes_failloc "$(osts_nodes)" 0x80000221
	facet_failover ost1
	sleep 10
	fail ost1
	sleep 30
	set_nodes_failloc "$(osts_nodes)" 0x0

	$CHECKSTAT -t file $DIR/$tdir/$tfile-* &&
		error "$CHECKSTAT $DIR/$tdir/$tfile attribute check should fail"
	rmdir $DIR/$tdir
}
run_test 61a "test race llog recovery vs llog cleanup"

#test race  mds llog sync vs llog cleanup
test_61b() {
	#   OBD_FAIL_MDS_LLOG_SYNC_TIMEOUT 0x13a
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x8000013a"
	facet_failover $SINGLEMDS
	sleep 10
	fail $SINGLEMDS
	do_facet client dd if=/dev/zero of=$DIR/$tfile bs=4k count=1 ||
		error "dd failed"
}
run_test 61b "test race mds llog sync vs llog cleanup"

#test race  cancel cookie cb vs llog cleanup
test_61c() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	#   OBD_FAIL_OST_CANCEL_COOKIE_TIMEOUT 0x222
	touch $DIR/$tfile || error "touch $DIR/$tfile failed"
	set_nodes_failloc "$(osts_nodes)" 0x80000222
	rm $DIR/$tfile
	sleep 10
	fail ost1
	set_nodes_failloc "$(osts_nodes)" 0x0
}
run_test 61c "test race mds llog sync vs llog cleanup"

test_61d() { # bug 16002 # bug 17466 # bug 22137
#   OBD_FAIL_OBD_LLOG_SETUP        0x605
    stop mgs
    do_facet mgs "lctl set_param fail_loc=0x80000605"
    start mgs $(mgsdevname) $MGS_MOUNT_OPTS &&
	error "mgs start should have failed"
    do_facet mgs "lctl set_param fail_loc=0"
    start mgs $(mgsdevname) $MGS_MOUNT_OPTS || error "cannot restart mgs"
}
run_test 61d "error in llog_setup should cleanup the llog context correctly"

test_62() { # Bug 15756 - don't mis-drop resent replay
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	replay_barrier $SINGLEMDS
	createmany -o $DIR/$tdir/$tfile- 25 ||
		error "createmany create files failed"
	#define OBD_FAIL_TGT_REPLAY_DROP         0x707
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000707"
	fail $SINGLEMDS
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"
	unlinkmany $DIR/$tdir/$tfile- 25 ||
		error "unlinkmany $DIR/$tdir/$tfile failed"
	return 0
}
run_test 62 "don't mis-drop resent replay"

#Adaptive Timeouts (bug 3055)
AT_MAX_SET=0

at_cleanup () {
    local var
    local facet
    local at_new

    echo "Cleaning up AT ..."
    if [ -n "$ATOLDBASE" ]; then
        local at_history=$($LCTL get_param -n at_history)
        do_facet $SINGLEMDS "lctl set_param at_history=$at_history" || true
        do_facet ost1 "lctl set_param at_history=$at_history" || true
    fi

	if [ $AT_MAX_SET -ne 0 ]; then
		for facet in mds client ost; do
			var=AT_MAX_SAVE_${facet}
			echo restore AT on $facet to saved value ${!var}
			at_max_set ${!var} $facet
			at_new=$(at_max_get $facet)
			echo Restored AT value on $facet $at_new
			[ $at_new -eq ${!var} ] ||
			error "AT value not restored SAVED ${!var} NEW $at_new"
		done
	fi
}

at_start()
{
    local at_max_new=600

    # Save at_max original values
    local facet
    if [ $AT_MAX_SET -eq 0 ]; then
        # Suppose that all osts have the same at_max
        for facet in mds client ost; do
            eval AT_MAX_SAVE_${facet}=$(at_max_get $facet)
        done
    fi
    local at_max
    for facet in mds client ost; do
        at_max=$(at_max_get $facet)
        if [ $at_max -ne $at_max_new ]; then
            echo "AT value on $facet is $at_max, set it by force temporarily to $at_max_new"
            at_max_set $at_max_new $facet
            AT_MAX_SET=1
        fi
    done

    if [ -z "$ATOLDBASE" ]; then
	ATOLDBASE=$(do_facet $SINGLEMDS "lctl get_param -n at_history")
        # speed up the timebase so we can check decreasing AT
        do_facet $SINGLEMDS "lctl set_param at_history=8" || true
        do_facet ost1 "lctl set_param at_history=8" || true

	# sleep for a while to cool down, should be > 8s and also allow
	# at least one ping to be sent. simply use TIMEOUT to be safe.
	sleep $TIMEOUT
    fi
}

test_65a() #bug 3055
{
    remote_ost_nodsh && skip "remote OST with nodsh" && return 0

    at_start || return 0
    $LCTL dk > /dev/null
    debugsave
    $LCTL set_param debug="other"
    # Slow down a request to the current service time, this is critical
    # because previous tests may have caused this value to increase.
    REQ_DELAY=`lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts |
               awk '/portal 12/ {print $5}'`
    REQ_DELAY=$((${REQ_DELAY} + ${REQ_DELAY} / 4 + 5))

    do_facet $SINGLEMDS lctl set_param fail_val=$((${REQ_DELAY} * 1000))
#define OBD_FAIL_PTLRPC_PAUSE_REQ        0x50a
    do_facet $SINGLEMDS $LCTL set_param fail_loc=0x8000050a
    createmany -o $DIR/$tfile 10 > /dev/null
    unlinkmany $DIR/$tfile 10 > /dev/null
    # check for log message
    $LCTL dk | grep -i "Early reply #" || error "No early reply"
    debugrestore
    # client should show REQ_DELAY estimates
    lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts | grep portal
    sleep 9
    lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts | grep portal
}
run_test 65a "AT: verify early replies"

test_65b() #bug 3055
{
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	at_start || return 0
	# turn on D_ADAPTTO
	debugsave
	$LCTL set_param debug="other trace"
	$LCTL dk > /dev/null
	# Slow down a request to the current service time, this is critical
	# because previous tests may have caused this value to increase.
	$LFS setstripe --stripe-index=0 --stripe-count=1 $DIR/$tfile ||
		error "$LFS setstripe failed for $DIR/$tfile"

	multiop $DIR/$tfile Ow1yc
	REQ_DELAY=`lctl get_param -n osc.${FSNAME}-OST0000-osc-*.timeouts |
		   awk '/portal 6/ {print $5}'`
	REQ_DELAY=$((${REQ_DELAY} + ${REQ_DELAY} / 4 + 5))

	do_facet ost1 lctl set_param fail_val=${REQ_DELAY}
	#define OBD_FAIL_OST_BRW_PAUSE_PACK      0x224
	do_facet ost1 $LCTL set_param fail_loc=0x224

	rm -f $DIR/$tfile
	$LFS setstripe --stripe-index=0 --stripe-count=1 $DIR/$tfile ||
		error "$LFS setstripe failed"
	# force some real bulk transfer
	multiop $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c

	do_facet ost1 $LCTL set_param fail_loc=0
	# check for log message
	$LCTL dk | grep -i "Early reply #" || error "No early reply"
	debugrestore
	# client should show REQ_DELAY estimates
	lctl get_param -n osc.${FSNAME}-OST0000-osc-*.timeouts | grep portal
}
run_test 65b "AT: verify early replies on packed reply / bulk"

test_66a() #bug 3055
{
    remote_ost_nodsh && skip "remote OST with nodsh" && return 0

    at_start || return 0
    lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts | grep "portal 12"
    # adjust 5s at a time so no early reply is sent (within deadline)
    do_facet $SINGLEMDS "$LCTL set_param fail_val=5000"
#define OBD_FAIL_PTLRPC_PAUSE_REQ        0x50a
    do_facet $SINGLEMDS "$LCTL set_param fail_loc=0x8000050a"
    createmany -o $DIR/$tfile 20 > /dev/null
    unlinkmany $DIR/$tfile 20 > /dev/null
    lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts | grep "portal 12"
    do_facet $SINGLEMDS "$LCTL set_param fail_val=10000"
    do_facet $SINGLEMDS "$LCTL set_param fail_loc=0x8000050a"
    createmany -o $DIR/$tfile 20 > /dev/null
    unlinkmany $DIR/$tfile 20 > /dev/null
    lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts | grep "portal 12"
    do_facet $SINGLEMDS "$LCTL set_param fail_loc=0"
    sleep 9
    createmany -o $DIR/$tfile 20 > /dev/null
    unlinkmany $DIR/$tfile 20 > /dev/null
    lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts | grep "portal 12"
    CUR=$(lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts | awk '/portal 12/ {print $5}')
    WORST=$(lctl get_param -n mdc.${FSNAME}-MDT0000-mdc-*.timeouts | awk '/portal 12/ {print $7}')
    echo "Current MDT timeout $CUR, worst $WORST"
    [ $CUR -lt $WORST ] || error "Current $CUR should be less than worst $WORST"
}
run_test 66a "AT: verify MDT service time adjusts with no early replies"

test_66b() #bug 3055
{
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	at_start || return 0
	ORIG=$(lctl get_param -n mdc.${FSNAME}-MDT0000*.timeouts |
		awk '/network/ {print $4}')
	$LCTL set_param fail_val=$(($ORIG + 5))
	#define OBD_FAIL_PTLRPC_PAUSE_REP      0x50c
	$LCTL set_param fail_loc=0x50c
	touch $DIR/$tfile > /dev/null 2>&1
	$LCTL set_param fail_loc=0
	CUR=$(lctl get_param -n mdc.${FSNAME}-MDT0000*.timeouts |
		awk '/network/ {print $4}')
	WORST=$(lctl get_param -n mdc.${FSNAME}-MDT0000*.timeouts |
		awk '/network/ {print $6}')
	echo "network timeout orig $ORIG, cur $CUR, worst $WORST"
	[ $WORST -gt $ORIG ] ||
		error "Worst $WORST should be worse than orig $ORIG"
}
run_test 66b "AT: verify net latency adjusts"

test_67a() #bug 3055
{
    remote_ost_nodsh && skip "remote OST with nodsh" && return 0

    at_start || return 0
    CONN1=$(lctl get_param -n osc.*.stats | awk '/_connect/ {total+=$2} END {print total}')
    # sleeping threads may drive values above this
    do_facet ost1 "$LCTL set_param fail_val=400"
#define OBD_FAIL_PTLRPC_PAUSE_REQ    0x50a
    do_facet ost1 "$LCTL set_param fail_loc=0x50a"
    createmany -o $DIR/$tfile 20 > /dev/null
    unlinkmany $DIR/$tfile 20 > /dev/null
    do_facet ost1 "$LCTL set_param fail_loc=0"
    CONN2=$(lctl get_param -n osc.*.stats | awk '/_connect/ {total+=$2} END {print total}')
    ATTEMPTS=$(($CONN2 - $CONN1))
    echo "$ATTEMPTS osc reconnect attempts on gradual slow"
	[ $ATTEMPTS -gt 0 ] &&
		error_ignore bz13721 "AT should have prevented reconnect"
	return 0
}
run_test 67a "AT: verify slow request processing doesn't induce reconnects"

test_67b() #bug 3055
{
    remote_ost_nodsh && skip "remote OST with nodsh" && return 0

    at_start || return 0
    CONN1=$(lctl get_param -n osc.*.stats | awk '/_connect/ {total+=$2} END {print total}')

	# exhaust precreations on ost1
	local OST=$(ostname_from_index 0)
	local mdtosc=$(get_mdtosc_proc_path mds $OST)
	local last_id=$(do_facet $SINGLEMDS lctl get_param -n \
			osp.$mdtosc.prealloc_last_id)
	local next_id=$(do_facet $SINGLEMDS lctl get_param -n \
			osp.$mdtosc.prealloc_next_id)

	mkdir -p $DIR/$tdir/${OST} || error "mkdir $DIR/$tdir/${OST} failed"
	$LFS setstripe -i 0 -c 1 $DIR/$tdir/${OST} ||
		error "$LFS setstripe failed"
	echo "Creating to objid $last_id on ost $OST..."
#define OBD_FAIL_OST_PAUSE_CREATE        0x223
    do_facet ost1 "$LCTL set_param fail_val=20000"
    do_facet ost1 "$LCTL set_param fail_loc=0x80000223"
    createmany -o $DIR/$tdir/${OST}/f $next_id $((last_id - next_id + 2))

    client_reconnect
    do_facet ost1 "lctl get_param -n ost.OSS.ost_create.timeouts"
    log "phase 2"
    CONN2=$(lctl get_param -n osc.*.stats | awk '/_connect/ {total+=$2} END {print total}')
    ATTEMPTS=$(($CONN2 - $CONN1))
    echo "$ATTEMPTS osc reconnect attempts on instant slow"
    # do it again; should not timeout
    do_facet ost1 "$LCTL set_param fail_loc=0x80000223"
    cp /etc/profile $DIR/$tfile || error "cp failed"
    do_facet ost1 "$LCTL set_param fail_loc=0"
    client_reconnect
    do_facet ost1 "lctl get_param -n ost.OSS.ost_create.timeouts"
    CONN3=$(lctl get_param -n osc.*.stats | awk '/_connect/ {total+=$2} END {print total}')
    ATTEMPTS=$(($CONN3 - $CONN2))
    echo "$ATTEMPTS osc reconnect attempts on 2nd slow"
    [ $ATTEMPTS -gt 0 ] && error "AT should have prevented reconnect"
    return 0
}
run_test 67b "AT: verify instant slowdown doesn't induce reconnects"

test_68 () #bug 13813
{
    remote_ost_nodsh && skip "remote OST with nodsh" && return 0

    at_start || return 0
    local ldlm_enqueue_min=$(find /sys -name ldlm_enqueue_min)
    [ -z "$ldlm_enqueue_min" ] && skip "missing /sys/.../ldlm_enqueue_min" && return 0
    local ldlm_enqueue_min_r=$(do_facet ost1 "find /sys -name ldlm_enqueue_min")
    [ -z "$ldlm_enqueue_min_r" ] && skip "missing /sys/.../ldlm_enqueue_min in the ost1" && return 0
    local ENQ_MIN=$(cat $ldlm_enqueue_min)
    local ENQ_MIN_R=$(do_facet ost1 "cat $ldlm_enqueue_min_r")
	echo $TIMEOUT >> $ldlm_enqueue_min
	do_facet ost1 "echo $TIMEOUT >> $ldlm_enqueue_min_r"

	mkdir $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS setstripe --stripe-index=0 -c 1 $DIR/$tdir ||
		error "$LFS setstripe failed for $DIR/$tdir"
	#define OBD_FAIL_LDLM_PAUSE_CANCEL       0x312
	$LCTL set_param fail_val=$(($TIMEOUT - 1))
	$LCTL set_param fail_loc=0x80000312
	cp /etc/profile $DIR/$tdir/${tfile}_1 || error "1st cp failed $?"
	$LCTL set_param fail_val=$((TIMEOUT * 5 / 4))
	$LCTL set_param fail_loc=0x80000312
	cp /etc/profile $DIR/$tdir/${tfile}_2 || error "2nd cp failed $?"
	$LCTL set_param fail_loc=0

	echo $ENQ_MIN >> $ldlm_enqueue_min
	do_facet ost1 "echo $ENQ_MIN_R >> $ldlm_enqueue_min_r"
	rm -rf $DIR/$tdir
	return 0
}
run_test 68 "AT: verify slowing locks"

at_cleanup
# end of AT tests includes above lines

# start multi-client tests
test_70a () {
	[ $CLIENTCOUNT -lt 2 ] &&
		{ skip "Need two or more clients, have $CLIENTCOUNT" && return; }

	echo "mount clients $CLIENTS ..."
	zconf_mount_clients $CLIENTS $MOUNT

	local clients=${CLIENTS//,/ }
	echo "Write/read files on $DIR ; clients $CLIENTS ... "
	for CLIENT in $clients; do
		do_node $CLIENT dd bs=1M count=10 if=/dev/zero \
			of=$DIR/${tfile}_${CLIENT} 2>/dev/null ||
				error "dd failed on $CLIENT"
	done

	local prev_client=$(echo $clients | sed 's/^.* \(.\+\)$/\1/')
	for C in ${CLIENTS//,/ }; do
		do_node $prev_client dd if=$DIR/${tfile}_${C} \
			of=/dev/null 2>/dev/null ||
			error "dd if=$DIR/${tfile}_${C} failed on $prev_client"
		prev_client=$C
	done

	ls $DIR
}
run_test 70a "check multi client t-f"

check_for_process () {
	local clients=$1
	shift
	local prog=$@

	killall_process $clients "$prog" -0
}

test_70b () {
	local clients=${CLIENTS:-$HOSTNAME}

	zconf_mount_clients $clients $MOUNT

	local duration=300
	[ "$SLOW" = "no" ] && duration=120
	# set duration to 900 because it takes some time to boot node
	[ "$FAILURE_MODE" = HARD ] && duration=900

	local elapsed
	local start_ts=$(date +%s)
	local cmd="rundbench 1 -t $duration"
	local pid=""
	if [ $MDSCOUNT -ge 2 ]; then
		test_mkdir -p -c$MDSCOUNT $DIR/$tdir
		$LFS setdirstripe -D -c$MDSCOUNT $DIR/$tdir
	fi
	do_nodesv $clients "set -x; MISSING_DBENCH_OK=$MISSING_DBENCH_OK \
		PATH=\$PATH:$LUSTRE/utils:$LUSTRE/tests/:$DBENCH_LIB \
		DBENCH_LIB=$DBENCH_LIB TESTSUITE=$TESTSUITE TESTNAME=$TESTNAME \
		MOUNT=$MOUNT DIR=$DIR/$tdir/\\\$(hostname) LCTL=$LCTL $cmd" &
	pid=$!

	#LU-1897 wait for all dbench copies to start
	while ! check_for_process $clients dbench; do
		elapsed=$(($(date +%s) - start_ts))
		if [ $elapsed -gt $duration ]; then
			killall_process $clients dbench
			error "dbench failed to start on $clients!"
		fi
		sleep 1
	done

	log "Started rundbench load pid=$pid ..."

	elapsed=$(($(date +%s) - start_ts))
	local num_failovers=0
	local fail_index=1
	while [ $elapsed -lt $duration ]; do
		if ! check_for_process $clients dbench; then
			error_noexit "dbench stopped on some of $clients!"
			killall_process $clients dbench
			break
		fi
		sleep 1
		replay_barrier mds$fail_index
		sleep 1 # give clients a time to do operations
		# Increment the number of failovers
		num_failovers=$((num_failovers+1))
		log "$TESTNAME fail mds$fail_index $num_failovers times"
		fail mds$fail_index
		elapsed=$(($(date +%s) - start_ts))
		if [ $fail_index -ge $MDSCOUNT ]; then
			fail_index=1
		else
			fail_index=$((fail_index+1))
		fi
	done

	wait $pid || error "rundbench load on $clients failed!"
}
run_test 70b "dbench ${MDSCOUNT}mdts recovery; $CLIENTCOUNT clients"
# end multi-client tests

random_fail_mdt() {
	local max_index=$1
	local duration=$2
	local monitor_pid=$3
	local elapsed
	local start_ts=$(date +%s)
	local num_failovers=0
	local fail_index

	elapsed=$(($(date +%s) - start_ts))
	while [ $elapsed -lt $duration ]; do
		fail_index=$((RANDOM%max_index+1))
		kill -0 $monitor_pid ||
			error "$monitor_pid stopped"
		sleep 120
		replay_barrier mds$fail_index
		sleep 10
		# Increment the number of failovers
		num_failovers=$((num_failovers+1))
		log "$TESTNAME fail mds$fail_index $num_failovers times"
		fail mds$fail_index
		elapsed=$(($(date +%s) - start_ts))
	done
}

cleanup_70c() {
	trap 0
	rm -f $DIR/replay-single.70c.lck
	rm -rf /$DIR/$tdir
}

test_70c () {
	local clients=${CLIENTS:-$HOSTNAME}
	local rc=0

	zconf_mount_clients $clients $MOUNT

	local duration=300
	[ "$SLOW" = "no" ] && duration=180
	# set duration to 900 because it takes some time to boot node
	[ "$FAILURE_MODE" = HARD ] && duration=600

	local elapsed
	local start_ts=$(date +%s)

	trap cleanup_70c EXIT
	(
		while [ ! -e $DIR/replay-single.70c.lck ]; do
			test_mkdir -p -c$MDSCOUNT $DIR/$tdir || break
			if [ $MDSCOUNT -ge 2 ]; then
				$LFS setdirstripe -D -c$MDSCOUNT $DIR/$tdir ||
				error "set default dirstripe failed"
			fi
			cd $DIR/$tdir || break
			tar cf - /etc | tar xf - || error "tar failed in loop"
		done
	)&
	tar_70c_pid=$!
	echo "Started tar $tar_70c_pid"

	random_fail_mdt $MDSCOUNT $duration $tar_70c_pid
	kill -0 $tar_70c_pid || error "tar $tar_70c_pid stopped"

	touch $DIR/replay-single.70c.lck
	wait $tar_70c_pid || error "$?: tar failed"

	cleanup_70c
	true
}
run_test 70c "tar ${MDSCOUNT}mdts recovery"

cleanup_70d() {
	trap 0
	kill -9 $mkdir_70d_pid
}

test_70d () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local clients=${CLIENTS:-$HOSTNAME}
	local rc=0

	zconf_mount_clients $clients $MOUNT

	local duration=300
	[ "$SLOW" = "no" ] && duration=180
	# set duration to 900 because it takes some time to boot node
	[ "$FAILURE_MODE" = HARD ] && duration=900

	mkdir -p $DIR/$tdir

	local elapsed
	local start_ts=$(date +%s)

	trap cleanup_70d EXIT
	(
		while true; do
			$LFS mkdir -i0 -c2 $DIR/$tdir/test || {
				echo "mkdir fails"
				break
			}
			$LFS mkdir -i1 -c2 $DIR/$tdir/test1 || {
				echo "mkdir fails"
				break
			}

			touch $DIR/$tdir/test/a || {
				echo "touch fails"
				break;
			}
			mkdir $DIR/$tdir/test/b || {
				echo "mkdir fails"
				break;
			}
			rm -rf $DIR/$tdir/test || {
				echo "rmdir fails"
				ls -lR $DIR/$tdir
				break
			}

			touch $DIR/$tdir/test1/a || {
				echo "touch fails"
				break;
			}
			mkdir $DIR/$tdir/test1/b || {
				echo "mkdir fails"
				break;
			}

			rm -rf $DIR/$tdir/test1 || {
				echo "rmdir fails"
				ls -lR $DIR/$tdir/test1
				break
			}
		done
	)&
	mkdir_70d_pid=$!
	echo "Started  $mkdir_70d_pid"

	random_fail_mdt $MDSCOUNT $duration $mkdir_70d_pid
	kill -0 $mkdir_70d_pid || error "mkdir/rmdir $mkdir_70d_pid stopped"

	cleanup_70d
	true
}
run_test 70d "mkdir/rmdir striped dir ${MDSCOUNT}mdts recovery"

test_70e () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local clients=${CLIENTS:-$HOSTNAME}
	local rc=0

	lctl set_param debug=+ha
	zconf_mount_clients $clients $MOUNT

	local duration=300
	[ "$SLOW" = "no" ] && duration=180
	# set duration to 900 because it takes some time to boot node
	[ "$FAILURE_MODE" = HARD ] && duration=900

	mkdir -p $DIR/$tdir
	$LFS mkdir -i0 $DIR/$tdir/test_0
	$LFS mkdir -i0 $DIR/$tdir/test_1
	touch $DIR/$tdir/test_0/a
	touch $DIR/$tdir/test_1/b
	(
	while true; do
		mrename $DIR/$tdir/test_0/a $DIR/$tdir/test_1/b > /dev/null || {
			echo "a->b fails"
			break;
		}

		checkstat $DIR/$tdir/test_0/a && {
			echo "a still exists"
			break
		}

		checkstat $DIR/$tdir/test_1/b || {
			echo "b still  exists"
			break
		}

		touch $DIR/$tdir/test_0/a || {
			echo "touch a fails"
			break
		}

		mrename $DIR/$tdir/test_1/b $DIR/$tdir/test_0/a > /dev/null || {
			echo "a->a fails"
			break;
		}
	done
	)&
	rename_70e_pid=$!
	stack_trap "kill -9 $rename_70e_pid" EXIT
	echo "Started PID=$rename_70e_pid"

	random_fail_mdt 2 $duration $rename_70e_pid
	kill -0 $rename_70e_pid || error "rename $rename_70e_pid stopped"
}
run_test 70e "rename cross-MDT with random fails"

test_70f_write_and_read(){
	local srcfile=$1
	local stopflag=$2
	local client

	echo "Write/read files in: '$DIR/$tdir', clients: '$CLIENTS' ..."
	for client in ${CLIENTS//,/ }; do
		[ -f $stopflag ] || return

		local tgtfile=$DIR/$tdir/$tfile.$client
		do_node $client dd $DD_OPTS bs=1M count=10 if=$srcfile \
			of=$tgtfile 2>/dev/null ||
			error "dd $DD_OPTS bs=1M count=10 if=$srcfile " \
			      "of=$tgtfile failed on $client, rc=$?"
	done

	local prev_client=$(echo ${CLIENTS//,/ } | awk '{ print $NF }')
	local index=0

	for client in ${CLIENTS//,/ }; do
		[ -f $stopflag ] || return

		# flush client cache in case test is running on only one client
		# do_node $client cancel_lru_locks osc
		do_node $client $LCTL set_param ldlm.namespaces.*.lru_size=clear

		tgtfile=$DIR/$tdir/$tfile.$client
		local md5=$(do_node $prev_client "md5sum $tgtfile")
		[ ${checksum[$index]// */} = ${md5// */} ] ||
			error "$tgtfile: checksum doesn't match on $prev_client"
		index=$((index + 1))
		prev_client=$client
	done
}

test_70f_loop(){
	local srcfile=$1
	local stopflag=$2
	DD_OPTS=

	mkdir -p $DIR/$tdir || error "cannot create $DIR/$tdir directory"
	$LFS setstripe -c -1 $DIR/$tdir ||
		error "cannot $LFS setstripe $DIR/$tdir"

	touch $stopflag
	while [ -f $stopflag ]; do
		test_70f_write_and_read $srcfile $stopflag
		# use direct IO and buffer cache in turns if loop
		[ -n "$DD_OPTS" ] && DD_OPTS="" || DD_OPTS="oflag=direct"
	done
}

test_70f_cleanup() {
	trap 0
	rm -f $TMP/$tfile.stop
	do_nodes $CLIENTS rm -f $TMP/$tfile
	rm -f $DIR/$tdir/$tfile.*
}

test_70f() {
#	[ x$ost1failover_HOST = x$ost_HOST ] &&
#		{ skip "Failover host not defined" && return; }
#	[ $CLIENTCOUNT -lt 2 ] &&
#		{ skip "Need 2 or more clients, have $CLIENTCOUNT" && return; }

	[[ "$OST1_VERSION" -lt $(version_code 2.9.53) ]] &&
		skip "Need server version at least 2.9.53"

	echo "mount clients $CLIENTS ..."
	zconf_mount_clients $CLIENTS $MOUNT

	local srcfile=$TMP/$tfile
	local client
	local index=0

	trap test_70f_cleanup EXIT
	# create a different source file local to each client node so we can
	# detect if the file wasn't written out properly after failover
	do_nodes $CLIENTS dd bs=1M count=10 if=/dev/urandom of=$srcfile \
		2>/dev/null || error "can't create $srcfile on $CLIENTS"
	for client in ${CLIENTS//,/ }; do
		checksum[$index]=$(do_node $client "md5sum $srcfile")
		index=$((index + 1))
	done

	local duration=120
	[ "$SLOW" = "no" ] && duration=60
	# set duration to 900 because it takes some time to boot node
	[ "$FAILURE_MODE" = HARD ] && duration=900

	local stopflag=$TMP/$tfile.stop
	test_70f_loop $srcfile $stopflag &
	local pid=$!

	local elapsed=0
	local num_failovers=0
	local start_ts=$SECONDS
	while [ $elapsed -lt $duration ]; do
		sleep 3
		replay_barrier ost1
		sleep 1
		num_failovers=$((num_failovers + 1))
		log "$TESTNAME failing OST $num_failovers times"
		fail ost1
		sleep 2
		elapsed=$((SECONDS - start_ts))
	done

	rm -f $stopflag
	wait $pid
	test_70f_cleanup
}
run_test 70f "OSS O_DIRECT recovery with $CLIENTCOUNT clients"

cleanup_71a() {
	trap 0
	kill -9 $mkdir_71a_pid
}

random_double_fail_mdt() {
	local max_index=$1
	local duration=$2
	local monitor_pid=$3
	local elapsed
	local start_ts=$(date +%s)
	local num_failovers=0
	local fail_index
	local second_index

	elapsed=$(($(date +%s) - start_ts))
	while [ $elapsed -lt $duration ]; do
		fail_index=$((RANDOM%max_index + 1))
		if [ $fail_index -eq $max_index ]; then
			second_index=1
		else
			second_index=$((fail_index + 1))
		fi
		kill -0 $monitor_pid ||
			error "$monitor_pid stopped"
		sleep 120
		replay_barrier mds$fail_index
		replay_barrier mds$second_index
		sleep 10
		# Increment the number of failovers
		num_failovers=$((num_failovers+1))
		log "fail mds$fail_index mds$second_index $num_failovers times"
		fail mds${fail_index},mds${second_index}
		elapsed=$(($(date +%s) - start_ts))
	done
}

test_71a () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local clients=${CLIENTS:-$HOSTNAME}
	local rc=0

	zconf_mount_clients $clients $MOUNT

	local duration=300
	[ "$SLOW" = "no" ] && duration=180
	# set duration to 900 because it takes some time to boot node
	[ "$FAILURE_MODE" = HARD ] && duration=900

	mkdir_on_mdt0 $DIR/$tdir

	local elapsed
	local start_ts=$(date +%s)

	trap cleanup_71a EXIT
	(
		while true; do
			$LFS mkdir -i0 -c2 $DIR/$tdir/test
			rmdir $DIR/$tdir/test
		done
	)&
	mkdir_71a_pid=$!
	echo "Started  $mkdir_71a_pid"

	random_double_fail_mdt 2 $duration $mkdir_71a_pid
	kill -0 $mkdir_71a_pid || error "mkdir/rmdir $mkdir_71a_pid stopped"

	cleanup_71a
	true
}
run_test 71a "mkdir/rmdir striped dir with 2 mdts recovery"

test_73a() {
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile

	replay_barrier $SINGLEMDS
	#define OBD_FAIL_LDLM_ENQUEUE_NET			0x302
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000302"
	fail $SINGLEMDS
	kill -USR1 $pid
	wait $pid || error "multiop pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	return 0
}
run_test 73a "open(O_CREAT), unlink, replay, reconnect before open replay, close"

test_73b() {
	multiop_bg_pause $DIR/$tfile O_tSc ||
		error "multiop_bg_pause $DIR/$tfile failed"
	pid=$!
	rm -f $DIR/$tfile

	replay_barrier $SINGLEMDS
	#define OBD_FAIL_MDS_LDLM_REPLY_NET       0x157
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000157"
	fail $SINGLEMDS
	kill -USR1 $pid
	wait $pid || error "multiop pid failed"
	[ -e $DIR/$tfile ] && error "file $DIR/$tfile should not exist"
	return 0
}
run_test 73b "open(O_CREAT), unlink, replay, reconnect at open_replay reply, close"

# bug 18554
test_74() {
	local clients=${CLIENTS:-$HOSTNAME}

	zconf_umount_clients $clients $MOUNT
	stop ost1
	facet_failover $SINGLEMDS
	zconf_mount_clients $clients $MOUNT
	mount_facet ost1
	touch $DIR/$tfile || error "touch $DIR/$tfile failed"
	rm $DIR/$tfile || error "rm $DIR/$tfile failed"
	clients_up || error "client evicted: $?"
	return 0
}
run_test 74 "Ensure applications don't fail waiting for OST recovery"

remote_dir_check_80() {
	local mdtidx=1
	local diridx
	local fileidx

	diridx=$($LFS getstripe -m $remote_dir) ||
		error "$LFS getstripe -m $remote_dir failed"
	[ $diridx -eq $mdtidx ] || error "$diridx != $mdtidx"

	createmany -o $remote_dir/f-%d 20 || error "creation failed"
	fileidx=$($LFS getstripe -m $remote_dir/f-1) ||
		error "$LFS getstripe -m $remote_dir/f-1 failed"
	[ $fileidx -eq $mdtidx ] || error "$fileidx != $mdtidx"

	return 0
}

test_80a() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	#define OBD_FAIL_OUT_UPDATE_NET_REP	0x1701
	do_facet mds${MDTIDX} lctl set_param fail_loc=0x1701
	$LFS mkdir -i $MDTIDX $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds1
	fail mds${MDTIDX}

	wait $CLIENT_PID || error "remote creation failed"

	remote_dir_check_80 || error "remote dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 80a "DNE: create remote dir, drop update rep from MDT0, fail MDT0"

test_80b() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	#define OBD_FAIL_OUT_UPDATE_NET_REP	0x1701
	do_facet mds${MDTIDX} lctl set_param fail_loc=0x1701
	$LFS mkdir -i $MDTIDX $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds1
	replay_barrier mds2
	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "remote creation failed"

	remote_dir_check_80 || error "remote dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 80b "DNE: create remote dir, drop update rep from MDT0, fail MDT1"

test_80c() {
	[[ "$mds1_FSTYPE" = zfs ]] &&
		[[ $MDS1_VERSION -lt $(version_code 2.12.51) ]] &&
		skip "requires LU-10143 fix on MDS"
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode"

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	#define OBD_FAIL_OUT_UPDATE_NET_REP	0x1701
	do_facet mds${MDTIDX} lctl set_param fail_loc=0x1701
	$LFS mkdir -i $MDTIDX $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds1
	replay_barrier mds2
	fail mds${MDTIDX}
	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "remote creation failed"

	remote_dir_check_80 || error "remote dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 80c "DNE: create remote dir, drop update rep from MDT1, fail MDT[0,1]"

test_80d() {
	[[ "$mds1_FSTYPE" = zfs ]] &&
		[[ $MDS1_VERSION -lt $(version_code 2.12.51) ]] &&
		skip "requires LU-10143 fix on MDS"
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	#define OBD_FAIL_OUT_UPDATE_NET_REP	0x1701
	do_facet mds${MDTIDX} lctl set_param fail_loc=0x1701
	$LFS mkdir -i $MDTIDX $remote_dir &
	local CLIENT_PID=$!

	# sleep 3 seconds to make sure MDTs are failed after
	# lfs mkdir -i has finished on all of MDTs.
	sleep 3

	replay_barrier mds1
	replay_barrier mds2
	fail mds${MDTIDX},mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "remote creation failed"

	remote_dir_check_80 || error "remote dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 80d "DNE: create remote dir, drop update rep from MDT1, fail 2 MDTs"

test_80e() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0x119
	$LFS mkdir -i $MDTIDX $remote_dir &
	local CLIENT_PID=$!

	# sleep 3 seconds to make sure MDTs are failed after
	# lfs mkdir -i has finished on all of MDTs.
	sleep 3

	replay_barrier mds1
	fail mds${MDTIDX}

	wait $CLIENT_PID || error "remote creation failed"

	remote_dir_check_80 || error "remote dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 80e "DNE: create remote dir, drop MDT1 rep, fail MDT0"

test_80f() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0x119
	$LFS mkdir -i $MDTIDX $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds2
	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "remote creation failed"

	remote_dir_check_80 || error "remote dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 80f "DNE: create remote dir, drop MDT1 rep, fail MDT1"

test_80g() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0x119
	$LFS mkdir -i $MDTIDX $remote_dir &
	local CLIENT_PID=$!

	# sleep 3 seconds to make sure MDTs are failed after
	# lfs mkdir -i has finished on all of MDTs.
	sleep 3

	replay_barrier mds1
	replay_barrier mds2
	fail mds${MDTIDX}
	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "remote creation failed"

	remote_dir_check_80 || error "remote dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 80g "DNE: create remote dir, drop MDT1 rep, fail MDT0, then MDT1"

test_80h() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0x119
	$LFS mkdir -i $MDTIDX $remote_dir &
	local CLIENT_PID=$!

	# sleep 3 seconds to make sure MDTs are failed after
	# lfs mkdir -i has finished on all of MDTs.
	sleep 3

	replay_barrier mds1
	replay_barrier mds2
	fail mds${MDTIDX},mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "remote dir creation failed"

	remote_dir_check_80 || error "remote dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 80h "DNE: create remote dir, drop MDT1 rep, fail 2 MDTs"

test_81a() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i $MDTIDX $remote_dir || error "lfs mkdir failed"

	touch $remote_dir || error "touch $remote_dir failed"
	# OBD_FAIL_OUT_UPDATE_NET_REP       0x1701
	do_facet mds${MDTIDX} lctl set_param fail_loc=0x1701
	rmdir $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds2
	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "rm remote dir failed"

	stat $remote_dir &>/dev/null && error "$remote_dir still exist!"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 81a "DNE: unlink remote dir, drop MDT0 update rep,  fail MDT1"

test_81b() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i $MDTIDX $remote_dir || error "lfs mkdir failed"

	# OBD_FAIL_OUT_UPDATE_NET_REP       0x1701
	do_facet mds${MDTIDX} lctl set_param fail_loc=0x1701
	rmdir $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds1
	fail mds${MDTIDX}

	wait $CLIENT_PID || error "rm remote dir failed"

	stat $remote_dir &>/dev/null && error "$remote_dir still exist!"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 81b "DNE: unlink remote dir, drop MDT0 update reply,  fail MDT0"

test_81c() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i $MDTIDX $remote_dir || error "lfs mkdir failed"

	# OBD_FAIL_OUT_UPDATE_NET_REP       0x1701
	do_facet mds${MDTIDX} lctl set_param fail_loc=0x1701
	rmdir $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds1
	replay_barrier mds2
	fail mds${MDTIDX}
	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "rm remote dir failed"

	stat $remote_dir &>/dev/null && error "$remote_dir still exist!"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 81c "DNE: unlink remote dir, drop MDT0 update reply, fail MDT0,MDT1"

test_81d() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i $MDTIDX $remote_dir || error "lfs mkdir failed"

	# OBD_FAIL_OUT_UPDATE_NET_REP       0x1701
	do_facet mds${MDTIDX} lctl set_param fail_loc=0x1701
	rmdir $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds1
	replay_barrier mds2
	fail mds${MDTIDX},mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "rm remote dir failed"

	stat $remote_dir &>/dev/null && error "$remote_dir still exist!"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 81d "DNE: unlink remote dir, drop MDT0 update reply,  fail 2 MDTs"

test_81e() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i $MDTIDX $remote_dir || error "lfs mkdir failed"

	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0x119
	rmdir $remote_dir &
	local CLIENT_PID=$!
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0

	replay_barrier mds1
	fail mds${MDTIDX}

	wait $CLIENT_PID || error "rm remote dir failed"

	stat $remote_dir &>/dev/null && error "$remote_dir still exist!"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 81e "DNE: unlink remote dir, drop MDT1 req reply, fail MDT0"

test_81f() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i $MDTIDX $remote_dir || error "lfs mkdir failed"

	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0x119
	rmdir $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds2
	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "rm remote dir failed"

	stat $remote_dir &>/dev/null && error "$remote_dir still exist!"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 81f "DNE: unlink remote dir, drop MDT1 req reply, fail MDT1"

test_81g() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i $MDTIDX $remote_dir || error "lfs mkdir failed"

	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0x119
	rmdir $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds1
	replay_barrier mds2
	fail mds${MDTIDX}
	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "rm remote dir failed"

	stat $remote_dir &>/dev/null && error "$remote_dir still exist!"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 81g "DNE: unlink remote dir, drop req reply, fail M0, then M1"

test_81h() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i $MDTIDX $remote_dir || error "lfs mkdir failed"

	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$((MDTIDX + 1)) lctl set_param fail_loc=0x119
	rmdir $remote_dir &
	local CLIENT_PID=$!

	replay_barrier mds1
	replay_barrier mds2
	fail mds${MDTIDX},mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "rm remote dir failed"

	stat $remote_dir &>/dev/null && error "$remote_dir still exist!"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 81h "DNE: unlink remote dir, drop request reply, fail 2 MDTs"

test_84a() {
#define OBD_FAIL_MDS_OPEN_WAIT_CREATE  0x144
    do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000144"
    createmany -o $DIR/$tfile- 1 &
    PID=$!
    mds_evict_client
    wait $PID
    client_up || client_up || true    # reconnect
}
run_test 84a "stale open during export disconnect"

test_85a() { #bug 16774
	lctl set_param -n ldlm.cancel_unused_locks_before_replay "1"

	for i in $(seq 100); do
		echo "tag-$i" > $DIR/$tfile-$i
		grep -q "tag-$i" $DIR/$tfile-$i || error "f2-$i"
	done

	lov_id=$(lctl dl | grep "clilov")
	addr=$(echo $lov_id | awk '{print $4}' | awk -F '-' '{print $NF}')
	count=$(lctl get_param -n \
		ldlm.namespaces.*MDT0000*$addr.lock_unused_count)
	echo "before recovery: unused locks count = $count"

	fail $SINGLEMDS

	count2=$(lctl get_param -n \
		 ldlm.namespaces.*MDT0000*$addr.lock_unused_count)
	echo "after recovery: unused locks count = $count2"

	if [ $count2 -ge $count ]; then
		error "unused locks are not canceled"
	fi
}
run_test 85a "check the cancellation of unused locks during recovery(IBITS)"

test_85b() { #bug 16774
	rm -rf $DIR/$tdir
	mkdir $DIR/$tdir

	lctl set_param -n ldlm.cancel_unused_locks_before_replay "1"

	$LFS setstripe -c 1 -i 0 $DIR/$tdir

	for i in $(seq 100); do
		dd if=/dev/urandom of=$DIR/$tdir/$tfile-$i bs=4096 \
			count=32 >/dev/null 2>&1
	done

	cancel_lru_locks osc

	for i in $(seq 100); do
		dd if=$DIR/$tdir/$tfile-$i of=/dev/null bs=4096 \
			count=32 >/dev/null 2>&1
	done

	lov_id=$(lctl dl | grep "clilov")
	addr=$(echo $lov_id | awk '{print $4}' | awk -F '-' '{print $NF}')
	count=$(lctl get_param -n \
			  ldlm.namespaces.*OST0000*$addr.lock_unused_count)
	echo "before recovery: unused locks count = $count"
	[ $count -ne 0 ] || error "unused locks ($count) should be zero"

	fail ost1

	count2=$(lctl get_param \
		 -n ldlm.namespaces.*OST0000*$addr.lock_unused_count)
	echo "after recovery: unused locks count = $count2"

	if [ $count2 -ge $count ]; then
		error "unused locks are not canceled"
	fi

	rm -rf $DIR/$tdir
}
run_test 85b "check the cancellation of unused locks during recovery(EXTENT)"

test_86() {
        local clients=${CLIENTS:-$HOSTNAME}

        zconf_umount_clients $clients $MOUNT
        do_facet $SINGLEMDS lctl set_param mdt.${FSNAME}-MDT*.exports.clear=0
        remount_facet $SINGLEMDS
        zconf_mount_clients $clients $MOUNT
}
run_test 86 "umount server after clear nid_stats should not hit LBUG"

test_87a() {
	do_facet ost1 "lctl set_param -n obdfilter.${ost1_svc}.sync_journal 0"

	replay_barrier ost1
	$LFS setstripe -i 0 -c 1 $DIR/$tfile
	dd if=/dev/urandom of=$DIR/$tfile bs=1024k count=8 ||
		error "dd to $DIR/$tfile failed"
	cksum=$(md5sum $DIR/$tfile | awk '{print $1}')
	cancel_lru_locks osc
	fail ost1
	dd if=$DIR/$tfile of=/dev/null bs=1024k count=8 || error "Cannot read"
	cksum2=$(md5sum $DIR/$tfile | awk '{print $1}')
	if [ $cksum != $cksum2 ] ; then
		error "New checksum $cksum2 does not match original $cksum"
	fi
}
run_test 87a "write replay"

test_87b() {
	do_facet ost1 "lctl set_param -n obdfilter.${ost1_svc}.sync_journal 0"

	replay_barrier ost1
	$LFS setstripe -i 0 -c 1 $DIR/$tfile
	dd if=/dev/urandom of=$DIR/$tfile bs=1024k count=8 ||
		error "dd to $DIR/$tfile failed"
	sleep 1 # Give it a chance to flush dirty data
	echo TESTTEST | dd of=$DIR/$tfile bs=1 count=8 seek=64
	cksum=$(md5sum $DIR/$tfile | awk '{print $1}')
	cancel_lru_locks osc
	fail ost1
	dd if=$DIR/$tfile of=/dev/null bs=1024k count=8 || error "Cannot read"
	cksum2=$(md5sum $DIR/$tfile | awk '{print $1}')
	if [ $cksum != $cksum2 ] ; then
		error "New checksum $cksum2 does not match original $cksum"
	fi
}
run_test 87b "write replay with changed data (checksum resend)"

test_88() { #bug 17485
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	mkdir -p $TMP/$tdir || error "mkdir $TMP/$tdir failed"

	$LFS setstripe -i 0 -c 1 $DIR/$tdir || error "$LFS setstripe failed"

	replay_barrier ost1
	replay_barrier $SINGLEMDS

	# exhaust precreations on ost1
	local OST=$(ostname_from_index 0)
	local mdtosc=$(get_mdtosc_proc_path $SINGLEMDS $OST)
	local last_id=$(do_facet $SINGLEMDS lctl get_param -n osp.$mdtosc.prealloc_last_id)
	local next_id=$(do_facet $SINGLEMDS lctl get_param -n osp.$mdtosc.prealloc_next_id)
	echo "before test: last_id = $last_id, next_id = $next_id"

	echo "Creating to objid $last_id on ost $OST..."
	createmany -o $DIR/$tdir/f-%d $next_id $((last_id - next_id + 2)) ||
		error "createmany create files to last_id failed"

	#create some files to use some uncommitted objids
	last_id=$(($last_id + 1))
	createmany -o $DIR/$tdir/f-%d $last_id 8 ||
		error "createmany create files with uncommitted objids failed"

    last_id2=$(do_facet $SINGLEMDS lctl get_param -n osp.$mdtosc.prealloc_last_id)
    next_id2=$(do_facet $SINGLEMDS lctl get_param -n osp.$mdtosc.prealloc_next_id)
    echo "before recovery: last_id = $last_id2, next_id = $next_id2" 

    # if test uses shutdown_facet && reboot_facet instead of facet_failover ()
    # it has to take care about the affected facets, bug20407
    local affected_mds1=$(affected_facets mds1)
    local affected_ost1=$(affected_facets ost1)

    shutdown_facet $SINGLEMDS
    shutdown_facet ost1

    reboot_facet $SINGLEMDS
    change_active $affected_mds1
    wait_for_facet $affected_mds1
    mount_facets $affected_mds1 || error "Restart of mds failed"

    reboot_facet ost1
    change_active $affected_ost1
    wait_for_facet $affected_ost1
    mount_facets $affected_ost1 || error "Restart of ost1 failed"

    clients_up

    last_id2=$(do_facet $SINGLEMDS lctl get_param -n osp.$mdtosc.prealloc_last_id)
    next_id2=$(do_facet $SINGLEMDS lctl get_param -n osp.$mdtosc.prealloc_next_id)
	echo "after recovery: last_id = $last_id2, next_id = $next_id2"

	# create new files, which should use new objids, and ensure the orphan
	# cleanup phase for ost1 is completed at the same time
	for i in $(seq 8); do
		file_id=$(($last_id + 10 + $i))
		dd if=/dev/urandom of=$DIR/$tdir/f-$file_id bs=4096 count=128
	done

	# if the objids were not recreated, then "ls" will fail with -ENOENT
	ls -l $DIR/$tdir/* || error "can't get the status of precreated files"

	local file_id
	# write into previously created files
	for i in $(seq 8); do
		file_id=$(($last_id + $i))
		dd if=/dev/urandom of=$DIR/$tdir/f-$file_id bs=4096 count=128
		cp -f $DIR/$tdir/f-$file_id $TMP/$tdir/
	done

	# compare the content
	for i in $(seq 8); do
		file_id=$(($last_id + $i))
		cmp $TMP/$tdir/f-$file_id $DIR/$tdir/f-$file_id ||
			error "the content of file is modified!"
	done

	rm -fr $TMP/$tdir
}
run_test 88 "MDS should not assign same objid to different files "

test_89() {
	cancel_lru_locks osc
	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	rm -f $DIR/$tdir/$tfile
	wait_mds_ost_sync || error "initial MDS-OST sync timed out"
	wait_delete_completed || error "initial wait delete timed out"
	local before=$(calc_osc_kbytes kbytesfree)
	local write_size=$(fs_log_size)

	$LFS setstripe -i 0 -c 1 $DIR/$tdir/$tfile
	(( $write_size >= 1024 )) || write_size=1024
	dd if=/dev/zero bs=${write_size}k count=10 of=$DIR/$tdir/$tfile
	sync
	stop ost1
	facet_failover $SINGLEMDS
	rm $DIR/$tdir/$tfile
	umount $MOUNT
	mount_facet ost1
	zconf_mount $(hostname) $MOUNT || error "mount fails"
	client_up || error "client_up failed"

	# wait for the remounted client to connect to ost1
	local target=$(get_osc_import_name client ost1)
	wait_import_state "FULL" "osc.${target}.ost_server_uuid" \
		$(max_recovery_time)

	wait_mds_ost_sync || error "MDS-OST sync timed out"
	wait_delete_completed || error "wait delete timed out"
	local after=$(calc_osc_kbytes kbytesfree)

	log "free_before: $before free_after: $after"
	(( $before <= $after + $(fs_log_size) )) ||
		error "kbytesfree $before > $after + margin $(fs_log_size)"
}
run_test 89 "no disk space leak on late ost connection"

cleanup_90 () {
	local facet=$1

	trap 0
	reboot_facet $facet
	change_active $facet
	wait_for_facet $facet
	mount_facet $facet || error "Restart of $facet failed"
	clients_up
}

test_90() { # bug 19494
	local dir=$DIR/$tdir
	local ostfail=$(get_random_entry $(get_facets OST))

	if [[ $FAILURE_MODE = HARD ]]; then
		local affected=$(affected_facets $ostfail);

		[[ "$affected" == $ostfail ]] ||
			skip "cannot use FAILURE_MODE=$FAILURE_MODE, affected: $affected"
	fi
	# ensure all OSTs are active to allow allocations
	wait_osts_up

	mkdir $dir || error "mkdir $dir failed"

	echo "Create the files"

	# file "f${index}" striped over 1 OST
	# file "all" striped over all OSTs

	$LFS setstripe -c $OSTCOUNT $dir/all ||
		error "setstripe failed to create $dir/all"

	for ((i = 0; i < $OSTCOUNT; i++)); do
		local f=$dir/f$i

		$LFS setstripe -i $i -c 1 $f ||
			error "$LFS setstripe failed to create $f"

		# confirm setstripe actually created stripe on requested OST
		local uuid=$(ostuuid_from_index $i)

		for file in f$i all; do
			local found=$($LFS find --obd $uuid --name $file $dir)

			if [[ $dir/$file != $found ]]; then
				$LFS getstripe $dir/$file
				error "wrong stripe: $file, uuid: $uuid"
			fi
		done
	done

	# Before failing an OST, get its obd name and index
	local varsvc=${ostfail}_svc
	local obd=$(do_facet $ostfail lctl get_param \
		    -n obdfilter.${!varsvc}.uuid)
	local index=$(($(facet_number $ostfail) - 1))

	echo "Fail $ostfail $obd, display the list of affected files"
	shutdown_facet $ostfail || error "shutdown_facet $ostfail failed"

	trap "cleanup_90 $ostfail" EXIT INT
	echo "General Query: lfs find $dir"
	local list=$($LFS find $dir)
	echo "$list"
	for (( i=0; i<$OSTCOUNT; i++ )); do
		list_member "$list" $dir/f$i ||
			error_noexit "lfs find $dir: no file f$i"
	done
	list_member "$list" $dir/all ||
		error_noexit "lfs find $dir: no file all"

	# focus on the missing OST,
	# we expect to see only two files affected: "f$(index)" and "all"

	echo "Querying files on shutdown $ostfail: lfs find --obd $obd"
    list=$($LFS find --obd $obd $dir)
    echo "$list"
    for file in all f$index; do
        list_member "$list" $dir/$file ||
            error_noexit "lfs find does not report the affected $obd for $file"
    done

    [[ $(echo $list | wc -w) -eq 2 ]] ||
        error_noexit "lfs find reports the wrong list of affected files ${#list[@]}"

	echo "Check getstripe: $LFS getstripe -r --obd $obd"
	list=$($LFS getstripe -r --obd $obd $dir)
	echo "$list"
    for file in all f$index; do
        echo "$list" | grep $dir/$file ||
            error_noexit "lfs getsripe does not report the affected $obd for $file"
    done

    cleanup_90 $ostfail
}
run_test 90 "lfs find identifies the missing striped file segments"

test_93a() {
	[[ "$MDS1_VERSION" -ge $(version_code 2.6.90) ]] ||
		[[ "$MDS1_VERSION" -ge $(version_code 2.5.4) &&
		   "$MDS1_VERSION" -lt $(version_code 2.5.50) ]] ||
		skip "Need MDS version 2.5.4+ or 2.6.90+"

	cancel_lru_locks osc

	$LFS setstripe -i 0 -c 1 $DIR/$tfile ||
		error "$LFS setstripe  $DIR/$tfile failed"
	dd if=/dev/zero of=$DIR/$tfile bs=1024 count=1 ||
		error "dd to $DIR/$tfile failed"
	#define OBD_FAIL_TGT_REPLAY_RECONNECT     0x715
	# We need to emulate a state that OST is waiting for other clients
	# not completing the recovery. Final ping is queued, but reply will be
	# sent on the recovery completion. It is done by sleep before
	# processing final pings
	do_facet ost1 "$LCTL set_param fail_val=40"
	do_facet ost1 "$LCTL set_param fail_loc=0x715"
	fail ost1
}
run_test 93a "replay + reconnect"

test_93b() {
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.90) ]] ||
		skip "Need MDS version 2.7.90+"

	cancel_lru_locks mdc

	createmany -o $DIR/$tfile 20 ||
			error "createmany -o $DIR/$tfile failed"

	#define OBD_FAIL_TGT_REPLAY_RECONNECT     0x715
	# We need to emulate a state that MDT is waiting for other clients
	# not completing the recovery. Final ping is queued, but reply will be
	# sent on the recovery completion. It is done by sleep before
	# processing final pings
	do_facet mds1 "$LCTL set_param fail_val=80"
	do_facet mds1 "$LCTL set_param fail_loc=0x715"
	fail mds1
}
run_test 93b "replay + reconnect on mds"

striped_dir_check_100() {
	local striped_dir=$DIR/$tdir/striped_dir
	local stripe_count=$($LFS getdirstripe -c $striped_dir)

	$LFS getdirstripe $striped_dir
	[ $stripe_count -eq 2 ] || error "$stripe_count != 2"

	createmany -o $striped_dir/f-%d 20 ||
		error "creation failed under striped dir"
}

test_100a() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local striped_dir=$DIR/$tdir/striped_dir
	local MDTIDX=1

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"

	#To make sure MDT1 and MDT0 are connected
	#otherwise it may create single stripe dir here
	$LFS setdirstripe -i1 $DIR/$tdir/remote_dir

	#define OBD_FAIL_OUT_UPDATE_NET_REP	0x1701
	do_facet mds$((MDTIDX+1)) lctl set_param fail_loc=0x1701
	$LFS setdirstripe -i0 -c2 $striped_dir &
	local CLIENT_PID=$!

	fail mds$((MDTIDX + 1))

	wait $CLIENT_PID || error "striped dir creation failed"

	striped_dir_check_100 || error "striped dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 100a "DNE: create striped dir, drop update rep from MDT1, fail MDT1"

test_100b() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local striped_dir=$DIR/$tdir/striped_dir
	local MDTIDX=1

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"

	#To make sure MDT1 and MDT0 are connected
	#otherwise it may create single stripe dir here
	$LFS setdirstripe -i1 $DIR/$tdir/remote_dir

	# OBD_FAIL_MDS_REINT_NET_REP       0x119
	do_facet mds$MDTIDX lctl set_param fail_loc=0x119
	$LFS mkdir -i0 -c2 $striped_dir &

	local CLIENT_PID=$!
	fail mds$MDTIDX

	wait $CLIENT_PID || error "striped dir creation failed"

	striped_dir_check_100 || error "striped dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 100b "DNE: create striped dir, fail MDT0"

test_100c() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local striped_dir=$DIR/$tdir/striped_dir

	mkdir_on_mdt0 $DIR/$tdir || error "mkdir $DIR/$tdir failed"

	#To make sure MDT1 and MDT0 are connected
	#otherwise it may create single stripe dir here
	$LFS setdirstripe -i1 $DIR/$tdir/remote_dir

	replay_barrier mds2
	$LFS mkdir -i1 -c2 $striped_dir

	fail_abort mds2 abort_recov_mdt

	createmany -o $striped_dir/f-%d 20 &&
			error "createmany -o $DIR/$tfile should fail"

	fail mds2
	striped_dir_check_100 || error "striped dir check failed"
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 100c "DNE: create striped dir, fail MDT0"

test_101() { #LU-5648
	mkdir -p $DIR/$tdir/d1
	mkdir -p $DIR/$tdir/d2
	touch $DIR/$tdir/file0
	num=1000

	replay_barrier $SINGLEMDS
	for i in $(seq $num) ; do
		echo test$i > $DIR/$tdir/d1/file$i
	done

	fail_abort $SINGLEMDS
	for i in $(seq $num) ; do
		touch $DIR/$tdir/d2/file$i
		test -s $DIR/$tdir/d2/file$i &&
			ls -al $DIR/$tdir/d2/file$i && error "file$i's size > 0"
	done

	rm -rf $DIR/$tdir
}
run_test 101 "Shouldn't reassign precreated objs to other files after recovery"

test_102a() {
	local idx
	local facet
	local num
	local i
	local pids pid

	[[ $(lctl get_param mdc.*.import |
	     grep "connect_flags:.*multi_mod_rpc") ]] ||
		{ skip "Need MDC with 'multi_mod_rpcs' feature"; return 0; }

	$LFS mkdir -c1 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	idx=$(printf "%04x" $($LFS getdirstripe -i $DIR/$tdir))
	facet="mds$((0x$idx + 1))"

	# get current value of max_mod_rcps_in_flight
	num=$($LCTL get_param -n \
		mdc.$FSNAME-MDT$idx-mdc-*.max_mod_rpcs_in_flight)
	# set default value if client does not support multi mod RPCs
	[ -z "$num" ] && num=1

	echo "creating $num files ..."
	umask 0022
	for i in $(seq $num); do
		touch $DIR/$tdir/file-$i
	done

	# drop request on MDT to force resend
	#define OBD_FAIL_MDS_REINT_MULTI_NET 0x159
	do_facet $facet "$LCTL set_param fail_loc=0x159"
	echo "launch $num chmod in parallel ($(date +%H:%M:%S)) ..."
	for i in $(seq $num); do
		chmod 0600 $DIR/$tdir/file-$i &
		pids="$pids $!"
	done
	sleep 1
	do_facet $facet "$LCTL set_param fail_loc=0"
	for pid in $pids; do
		wait $pid || error "chmod failed"
	done
	echo "done ($(date +%H:%M:%S))"

	# check chmod succeed
	for i in $(seq $num); do
		checkstat -vp 0600 $DIR/$tdir/file-$i
	done

	rm -rf $DIR/$tdir
}
run_test 102a "check resend (request lost) with multiple modify RPCs in flight"

test_102b() {
	local idx
	local facet
	local num
	local i
	local pids pid

	[[ $(lctl get_param mdc.*.import |
	     grep "connect_flags:.*multi_mod_rpc") ]] ||
		{ skip "Need MDC with 'multi_mod_rpcs' feature"; return 0; }

	$LFS mkdir -c1 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	idx=$(printf "%04x" $($LFS getdirstripe -i $DIR/$tdir))
	facet="mds$((0x$idx + 1))"

	# get current value of max_mod_rcps_in_flight
	num=$($LCTL get_param -n \
		mdc.$FSNAME-MDT$idx-mdc-*.max_mod_rpcs_in_flight)
	# set default value if client does not support multi mod RPCs
	[ -z "$num" ] && num=1

	echo "creating $num files ..."
	umask 0022
	for i in $(seq $num); do
		touch $DIR/$tdir/file-$i
	done

	# drop reply on MDT to force reconstruction
	#define OBD_FAIL_MDS_REINT_MULTI_NET_REP 0x15a
	do_facet $facet "$LCTL set_param fail_loc=0x15a"
	echo "launch $num chmod in parallel ($(date +%H:%M:%S)) ..."
	for i in $(seq $num); do
		chmod 0600 $DIR/$tdir/file-$i &
		pids="$pids $!"
	done
	sleep 1
	do_facet $facet "$LCTL set_param fail_loc=0"
	for pid in $pids; do
		wait $pid || error "chmod failed"
	done
	echo "done ($(date +%H:%M:%S))"

	# check chmod succeed
	for i in $(seq $num); do
		checkstat -vp 0600 $DIR/$tdir/file-$i
	done

	rm -rf $DIR/$tdir
}
run_test 102b "check resend (reply lost) with multiple modify RPCs in flight"

test_102c() {
	local idx
	local facet
	local num
	local i
	local pids pid

	[[ $(lctl get_param mdc.*.import |
	     grep "connect_flags:.*multi_mod_rpc") ]] ||
		{ skip "Need MDC with 'multi_mod_rpcs' feature"; return 0; }

	$LFS mkdir -c1 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	idx=$(printf "%04x" $($LFS getdirstripe -i $DIR/$tdir))
	facet="mds$((0x$idx + 1))"

	# get current value of max_mod_rcps_in_flight
	num=$($LCTL get_param -n \
		mdc.$FSNAME-MDT$idx-mdc-*.max_mod_rpcs_in_flight)
	# set default value if client does not support multi mod RPCs
	[ -z "$num" ] && num=1

	echo "creating $num files ..."
	umask 0022
	for i in $(seq $num); do
		touch $DIR/$tdir/file-$i
	done

	replay_barrier $facet

	# drop reply on MDT
	#define OBD_FAIL_MDS_REINT_MULTI_NET_REP 0x15a
	do_facet $facet "$LCTL set_param fail_loc=0x15a"
	echo "launch $num chmod in parallel ($(date +%H:%M:%S)) ..."
	for i in $(seq $num); do
		chmod 0600 $DIR/$tdir/file-$i &
		pids="$pids $!"
	done
	sleep 1
	do_facet $facet "$LCTL set_param fail_loc=0"

	# fail MDT
	fail $facet

	for pid in $pids; do
		wait $pid || error "chmod failed"
	done
	echo "done ($(date +%H:%M:%S))"

	# check chmod succeed
	for i in $(seq $num); do
		checkstat -vp 0600 $DIR/$tdir/file-$i
	done

	rm -rf $DIR/$tdir
}
run_test 102c "check replay w/o reconstruction with multiple mod RPCs in flight"

test_102d() {
	local idx
	local facet
	local num
	local i
	local pids pid

	[[ $(lctl get_param mdc.*.import |
	     grep "connect_flags:.*multi_mod_rpc") ]] ||
		{ skip "Need MDC with 'multi_mod_rpcs' feature"; return 0; }

	$LFS mkdir -c1 $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	idx=$(printf "%04x" $($LFS getdirstripe -i $DIR/$tdir))
	facet="mds$((0x$idx + 1))"

	# get current value of max_mod_rcps_in_flight
	num=$($LCTL get_param -n \
		mdc.$FSNAME-MDT$idx-mdc-*.max_mod_rpcs_in_flight)
	# set default value if client does not support multi mod RPCs
	[ -z "$num" ] && num=1

	echo "creating $num files ..."
	umask 0022
	for i in $(seq $num); do
		touch $DIR/$tdir/file-$i
	done

	# drop reply on MDT
	#define OBD_FAIL_MDS_REINT_MULTI_NET_REP 0x15a
	do_facet $facet "$LCTL set_param fail_loc=0x15a"
	echo "launch $num chmod in parallel ($(date +%H:%M:%S)) ..."
	for i in $(seq $num); do
		chmod 0600 $DIR/$tdir/file-$i &
		pids="$pids $!"
	done
	sleep 1

	# write MDT transactions to disk
	do_facet $facet "sync; sync; sync"

	do_facet $facet "$LCTL set_param fail_loc=0"

	# fail MDT
	fail $facet

	for pid in $pids; do
		wait $pid || error "chmod failed"
	done
	echo "done ($(date +%H:%M:%S))"

	# check chmod succeed
	for i in $(seq $num); do
		checkstat -vp 0600 $DIR/$tdir/file-$i
	done

	rm -rf $DIR/$tdir
}
run_test 102d "check replay & reconstruction with multiple mod RPCs in flight"

test_103() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ "$MDS1_VERSION" -gt $(version_code 2.8.54) ]] ||
		skip "Need MDS version 2.8.54+"

#define OBD_FAIL_MDS_TRACK_OVERFLOW 0x162
	do_facet mds1 $LCTL set_param fail_loc=0x80000162

	mkdir -p $DIR/$tdir
	createmany -o $DIR/$tdir/t- 30 ||
		error "create files on remote directory failed"
	sync
	rm -rf $DIR/$tdir/t-*
	sync
#MDS should crash with tr->otr_next_id overflow
	fail mds1
}
run_test 103 "Check otr_next_id overflow"


check_striped_dir_110()
{
	$CHECKSTAT -t dir $DIR/$tdir/striped_dir ||
			error "create striped dir failed"
	local stripe_count=$($LFS getdirstripe -c $DIR/$tdir/striped_dir)
	[ $stripe_count -eq $MDSCOUNT ] ||
		error "$stripe_count != 2 after recovery"
}

test_110a() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	replay_barrier mds1
	$LFS mkdir -i1 -c$MDSCOUNT $DIR/$tdir/striped_dir
	fail mds1

	check_striped_dir_110 || error "check striped_dir failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110a "DNE: create striped dir, fail MDT1"

test_110b() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	replay_barrier mds1
	$LFS mkdir -i1 -c$MDSCOUNT $DIR/$tdir/striped_dir
	umount $MOUNT
	fail mds1
	zconf_mount $(hostname) $MOUNT
	client_up || return 1

	check_striped_dir_110 || error "check striped_dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110b "DNE: create striped dir, fail MDT1 and client"

test_110c() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	replay_barrier mds2
	$LFS mkdir -i1 -c$MDSCOUNT $DIR/$tdir/striped_dir
	fail mds2

	check_striped_dir_110 || error "check striped_dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110c "DNE: create striped dir, fail MDT2"

test_110d() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	replay_barrier mds2
	$LFS mkdir -i1 -c$MDSCOUNT $DIR/$tdir/striped_dir
	umount $MOUNT
	fail mds2
	zconf_mount $(hostname) $MOUNT
	client_up || return 1

	check_striped_dir_110 || error "check striped_dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110d "DNE: create striped dir, fail MDT2 and client"

test_110e() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	replay_barrier mds2
	$LFS mkdir -i1 -c$MDSCOUNT $DIR/$tdir/striped_dir
	umount $MOUNT
	replay_barrier mds1
	fail mds1,mds2
	zconf_mount $(hostname) $MOUNT
	client_up || return 1

	check_striped_dir_110 || error "check striped_dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110e "DNE: create striped dir, uncommit on MDT2, fail client/MDT1/MDT2"

test_110f() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	replay_barrier mds1
	replay_barrier mds2
	$LFS mkdir -i1 -c$MDSCOUNT $DIR/$tdir/striped_dir
	fail mds2,mds1

	check_striped_dir_110 || error "check striped_dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110f "DNE: create striped dir, fail MDT1/MDT2"

test_110g() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	replay_barrier mds1
	$LFS mkdir -i1 -c$MDSCOUNT $DIR/$tdir/striped_dir
	umount $MOUNT
	replay_barrier mds2
	fail mds1,mds2
	zconf_mount $(hostname) $MOUNT
	client_up || return 1

	check_striped_dir_110 || error "check striped_dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110g "DNE: create striped dir, uncommit on MDT1, fail client/MDT1/MDT2"

test_111a() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i1 -c2 $DIR/$tdir/striped_dir
	replay_barrier mds1
	rm -rf $DIR/$tdir/striped_dir
	fail mds1

	$CHECKSTAT -t dir $DIR/$tdir/striped_dir &&
			error "striped dir still exists"
	return 0
}
run_test 111a "DNE: unlink striped dir, fail MDT1"

test_111b() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i1 -c2 $DIR/$tdir/striped_dir
	replay_barrier mds2
	rm -rf $DIR/$tdir/striped_dir
	umount $MOUNT
	fail mds2
	zconf_mount $(hostname) $MOUNT
	client_up || return 1

	$CHECKSTAT -t dir $DIR/$tdir/striped_dir &&
			error "striped dir still exists"
	return 0
}
run_test 111b "DNE: unlink striped dir, fail MDT2"

test_111c() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i1 -c2 $DIR/$tdir/striped_dir
	replay_barrier mds1
	rm -rf $DIR/$tdir/striped_dir
	umount $MOUNT
	replay_barrier mds2
	fail mds1,mds2
	zconf_mount $(hostname) $MOUNT
	client_up || return 1
	$CHECKSTAT -t dir $DIR/$tdir/striped_dir &&
			error "striped dir still exists"
	return 0
}
run_test 111c "DNE: unlink striped dir, uncommit on MDT1, fail client/MDT1/MDT2"

test_111d() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i1 -c2 $DIR/$tdir/striped_dir
	replay_barrier mds2
	rm -rf $DIR/$tdir/striped_dir
	umount $MOUNT
	replay_barrier mds1
	fail mds1,mds2
	zconf_mount $(hostname) $MOUNT
	client_up || return 1
	$CHECKSTAT -t dir $DIR/$tdir/striped_dir &&
			error "striped dir still exists"

	return 0
}
run_test 111d "DNE: unlink striped dir, uncommit on MDT2, fail client/MDT1/MDT2"

test_111e() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i1 -c2 $DIR/$tdir/striped_dir
	replay_barrier mds2
	rm -rf $DIR/$tdir/striped_dir
	replay_barrier mds1
	fail mds1,mds2
	$CHECKSTAT -t dir $DIR/$tdir/striped_dir &&
			error "striped dir still exists"
	return 0
}
run_test 111e "DNE: unlink striped dir, uncommit on MDT2, fail MDT1/MDT2"

test_111f() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i1 -c2 $DIR/$tdir/striped_dir
	replay_barrier mds1
	rm -rf $DIR/$tdir/striped_dir
	replay_barrier mds2
	fail mds1,mds2
	$CHECKSTAT -t dir $DIR/$tdir/striped_dir &&
			error "striped dir still exists"
	return 0
}
run_test 111f "DNE: unlink striped dir, uncommit on MDT1, fail MDT1/MDT2"

test_111g() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i1 -c2 $DIR/$tdir/striped_dir
	replay_barrier mds1
	replay_barrier mds2
	rm -rf $DIR/$tdir/striped_dir
	fail mds1,mds2
	$CHECKSTAT -t dir $DIR/$tdir/striped_dir &&
			error "striped dir still exists"
	return 0
}
run_test 111g "DNE: unlink striped dir, fail MDT1/MDT2"

test_112_rename_prepare() {
	mkdir_on_mdt0 $DIR/$tdir
	mkdir -p $DIR/$tdir/src_dir
	$LFS mkdir -i 1 $DIR/$tdir/src_dir/src_child ||
		error "create remote source failed"

	touch $DIR/$tdir/src_dir/src_child/a

	$LFS mkdir -i 2 $DIR/$tdir/tgt_dir ||
		error "create remote target dir failed"

	$LFS mkdir -i 3 $DIR/$tdir/tgt_dir/tgt_child ||
		error "create remote target child failed"
}

test_112_check() {
	find $DIR/$tdir/
	$CHECKSTAT -t dir $DIR/$tdir/src_dir/src_child &&
		error "src_child still exists after rename"

	$CHECKSTAT -t file $DIR/$tdir/tgt_dir/tgt_child/a ||
		error "missing file(a) after rename"
}

test_112a() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds1

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"
	fail mds1

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112a "DNE: cross MDT rename, fail MDT1"

test_112b() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds2

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds2

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112b "DNE: cross MDT rename, fail MDT2"

test_112c() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds3

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds3

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112c "DNE: cross MDT rename, fail MDT3"

test_112d() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds4

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds4

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112d "DNE: cross MDT rename, fail MDT4"

test_112e() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds1
	replay_barrier mds2

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds1,mds2

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112e "DNE: cross MDT rename, fail MDT1 and MDT2"

test_112f() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds1
	replay_barrier mds3

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds1,mds3

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112f "DNE: cross MDT rename, fail MDT1 and MDT3"

test_112g() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds1
	replay_barrier mds4

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds1,mds4

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112g "DNE: cross MDT rename, fail MDT1 and MDT4"

test_112h() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds2
	replay_barrier mds3

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds2,mds3

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112h "DNE: cross MDT rename, fail MDT2 and MDT3"

test_112i() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds2
	replay_barrier mds4

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds2,mds4

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112i "DNE: cross MDT rename, fail MDT2 and MDT4"

test_112j() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds3
	replay_barrier mds4

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds3,mds4

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112j "DNE: cross MDT rename, fail MDT3 and MDT4"

test_112k() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds1
	replay_barrier mds2
	replay_barrier mds3

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds1,mds2,mds3

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112k "DNE: cross MDT rename, fail MDT1,MDT2,MDT3"

test_112l() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds1
	replay_barrier mds2
	replay_barrier mds4

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds1,mds2,mds4

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112l "DNE: cross MDT rename, fail MDT1,MDT2,MDT4"

test_112m() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds1
	replay_barrier mds3
	replay_barrier mds4

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds1,mds3,mds4

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112m "DNE: cross MDT rename, fail MDT1,MDT3,MDT4"

test_112n() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	test_112_rename_prepare
	replay_barrier mds2
	replay_barrier mds3
	replay_barrier mds4

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	fail mds2,mds3,mds4

	test_112_check
	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 112n "DNE: cross MDT rename, fail MDT2,MDT3,MDT4"

test_115() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[[ "$MDS1_VERSION" -ge $(version_code 2.7.56) ]] ||
		skip "Need MDS version at least 2.7.56"

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0
	local fail_index=0
	local index
	local i
	local j

	mkdir -p $DIR/$tdir
	for ((j=0;j<$((MDSCOUNT));j++)); do
		fail_index=$((fail_index+1))
		index=$((fail_index % MDSCOUNT))
		replay_barrier mds$((index + 1))
		for ((i=0;i<5;i++)); do
			test_mkdir -i$index -c$MDSCOUNT $DIR/$tdir/test_$i ||
				error "create striped dir $DIR/$tdir/test_$i"
		done

		fail mds$((index + 1))
		for ((i=0;i<5;i++)); do
			checkstat -t dir $DIR/$tdir/test_$i ||
				error "$DIR/$tdir/test_$i does not exist!"
		done
		rm -rf $DIR/$tdir/test_* ||
				error "rmdir fails"
	done
}
run_test 115 "failover for create/unlink striped directory"

test_116a() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[ "$MDS1_VERSION" -lt $(version_code 2.7.55) ] &&
		skip "Do not support large update log before 2.7.55" &&
		return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0
	local fail_index=0

	mkdir_on_mdt0 $DIR/$tdir
	replay_barrier mds1

	# OBD_FAIL_SPLIT_UPDATE_REC       0x1702
	do_facet mds1 "lctl set_param fail_loc=0x80001702"
	$LFS setdirstripe -i0 -c$MDSCOUNT $DIR/$tdir/striped_dir

	fail mds1
	$CHECKSTAT -t dir $DIR/$tdir/striped_dir ||
		error "stried_dir does not exists"
}
run_test 116a "large update log master MDT recovery"

test_116b() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[ "$MDS1_VERSION" -lt $(version_code 2.7.55) ] &&
		skip "Do not support large update log before 2.7.55" &&
		return 0

	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0
	local fail_index=0

	mkdir_on_mdt0 $DIR/$tdir
	replay_barrier mds2

	# OBD_FAIL_SPLIT_UPDATE_REC       0x1702
	do_facet mds2 "lctl set_param fail_loc=0x80001702"
	$LFS setdirstripe -i0 -c$MDSCOUNT $DIR/$tdir/striped_dir

	fail mds2
	$CHECKSTAT -t dir $DIR/$tdir/striped_dir ||
		error "stried_dir does not exists"
}
run_test 116b "large update log slave MDT recovery"

test_117() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0
	local index
	local mds_indexs

	mkdir -p $DIR/$tdir
	$LFS setdirstripe -i0 -c$MDSCOUNT $DIR/$tdir/remote_dir
	$LFS setdirstripe -i1 -c$MDSCOUNT $DIR/$tdir/remote_dir_1
	sleep 2

	# Let's set rdonly on all MDTs, so client will send
	# replay requests on all MDTs and replay these requests
	# at the same time. This test will verify the recovery
	# will not be deadlock in this case, LU-7531.
	for ((index = 0; index < $((MDSCOUNT)); index++)); do
		replay_barrier mds$((index + 1))
		if [ -z $mds_indexs ]; then
			mds_indexs="${mds_indexs}mds$((index+1))"
		else
			mds_indexs="${mds_indexs},mds$((index+1))"
		fi
	done

	rm -rf $DIR/$tdir/remote_dir
	rm -rf $DIR/$tdir/remote_dir_1

	fail $mds_indexs

	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 117 "DNE: cross MDT unlink, fail MDT1 and MDT2"

test_118() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[ "$MDS1_VERSION" -lt $(version_code 2.7.64) ] &&
		skip "Do not support large update log before 2.7.64" &&
		return 0

	mkdir -p $DIR/$tdir

	$LFS setdirstripe -c2 $DIR/$tdir/striped_dir ||
		error "setdirstripe fails"
	$LFS setdirstripe -c2 $DIR/$tdir/striped_dir1 ||
		error "setdirstripe fails 1"
	rm -rf $DIR/$tdir/striped_dir* || error "rmdir fails"

	# OBD_FAIL_INVALIDATE_UPDATE       0x1705
	do_facet mds1 "lctl set_param fail_loc=0x1705"
	$LFS setdirstripe -c2 $DIR/$tdir/striped_dir
	$LFS setdirstripe -c2 $DIR/$tdir/striped_dir1
	do_facet mds1 "lctl set_param fail_loc=0x0"

	replay_barrier mds1
	$LFS setdirstripe -c2 $DIR/$tdir/striped_dir
	$LFS setdirstripe -c2 $DIR/$tdir/striped_dir1
	fail mds1

	true
}
run_test 118 "invalidate osp update will not cause update log corruption"

test_119() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[ "$MDS1_VERSION" -lt $(version_code 2.7.64) ] &&
		skip "Do not support large update log before 2.7.64" &&
		return 0
	local stripe_count
	local hard_timeout=$(do_facet mds1 \
		"lctl get_param -n mdt.$FSNAME-MDT0000.recovery_time_hard")

	local clients=${CLIENTS:-$HOSTNAME}
	local time_min=$(recovery_time_min)

	mkdir_on_mdt0 $DIR/$tdir
	mkdir $DIR/$tdir/tmp
	rmdir $DIR/$tdir/tmp

	replay_barrier mds1
	mkdir $DIR/$tdir/dir_1
	for ((i = 0; i < 20; i++)); do
		$LFS setdirstripe -i0 -c2 $DIR/$tdir/stripe_dir-$i
	done

	stop mds1
	change_active mds1
	wait_for_facet mds1

	#define OBD_FAIL_TGT_REPLAY_DELAY  0x714
	do_facet mds1 $LCTL set_param fail_loc=0x80000714
	#sleep (timeout + 5), so mds will evict the client exports,
	#but DNE update recovery will keep going.
	do_facet mds1 $LCTL set_param fail_val=$((time_min + 5))

	mount_facet mds1 "-o recovery_time_hard=$time_min"

	wait_clients_import_state "$clients" mds1 FULL

	clients_up || clients_up || error "failover df: $?"

	#revert back the hard timeout
	do_facet mds1 $LCTL set_param \
		mdt.$FSNAME-MDT0000.recovery_time_hard=$hard_timeout

	for ((i = 0; i < 20; i++)); do
		stripe_count=$($LFS getdirstripe -c $DIR/$tdir/stripe_dir-$i)
		[ $stripe_count == 2 ] || {
			error "stripe_dir-$i creation replay fails"
			break
		}
	done
}
run_test 119 "timeout of normal replay does not cause DNE replay fails  "

test_120() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	[ "$MDS1_VERSION" -lt $(version_code 2.7.64) ] &&
		skip "Do not support large update log before 2.7.64" &&
		return 0

	mkdir_on_mdt0 $DIR/$tdir
	replay_barrier_nosync mds1
	for ((i = 0; i < 20; i++)); do
		mkdir $DIR/$tdir/dir-$i || {
			error "create dir-$i fails"
			break
		}
		$LFS setdirstripe -i0 -c2 $DIR/$tdir/stripe_dir-$i || {
			error "create stripe_dir-$i fails"
			break
		}
	done

	fail_abort mds1

	for ((i = 0; i < 20; i++)); do
		[ ! -e "$DIR/$tdir/dir-$i" ] || {
			error "dir-$i still exists"
			break
		}
		[ ! -e "$DIR/$tdir/stripe_dir-$i" ] || {
			error "stripe_dir-$i still exists"
			break
		}
	done
}
run_test 120 "DNE fail abort should stop both normal and DNE replay"

test_121() {
	[ "$MDS1_VERSION" -lt $(version_code 2.10.90) ] &&
		skip "Don't support it before 2.11" &&
		return 0

	local at_max_saved=$(at_max_get mds)

	touch $DIR/$tfile || error "touch $DIR/$tfile failed"
	cancel_lru_locks mdc

	multiop_bg_pause $DIR/$tfile s_s || error "multiop $DIR/$tfile failed"
	mpid=$!

	lctl set_param -n ldlm.cancel_unused_locks_before_replay "0"

	stop mds1
	change_active mds1
	wait_for_facet mds1

	#define OBD_FAIL_TGT_RECOVERY_REQ_RACE	0x721
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x721 fail_val=0"
	at_max_set 0 mds

	mount_facet mds1
	wait_clients_import_state "$clients" mds1 FULL
	clients_up || clients_up || error "failover df: $?"

	kill -USR1 $mpid
	wait $mpid || error "multiop_bg_pause pid failed"

	do_facet $SINGLEMDS "lctl set_param fail_loc=0x0"
	lctl set_param -n ldlm.cancel_unused_locks_before_replay "1"
	at_max_set $at_max_saved mds
	rm -f $DIR/$tfile
}
run_test 121 "lock replay timed out and race"

test_130a() {
	[ "$MDS1_VERSION" -lt $(version_code 2.10.90) ] &&
		skip "Do not support Data-on-MDT before 2.11"

	replay_barrier $SINGLEMDS
	$LFS setstripe -E 1M -L mdt -E EOF -c 2 $DIR/$tfile
	fail $SINGLEMDS

	[ $($LFS getstripe -L $DIR/$tfile) == "mdt" ] ||
		error "Fail to replay DoM file creation"
}
run_test 130a "DoM file create (setstripe) replay"

test_130b() {
	[ "$MDS1_VERSION" -lt $(version_code 2.10.90) ] &&
		skip "Do not support Data-on-MDT before 2.11"

	mkdir_on_mdt0 $DIR/$tdir
	$LFS setstripe -E 1M -L mdt -E EOF -c 2 $DIR/$tdir
	replay_barrier $SINGLEMDS
	touch $DIR/$tdir/$tfile
	fail $SINGLEMDS

	[ $($LFS getstripe -L $DIR/$tdir/$tfile) == "mdt" ] ||
		error "Fail to replay DoM file creation"
}
run_test 130b "DoM file create (inherited) replay"

test_131a() {
	[ "$MDS1_VERSION" -lt $(version_code 2.10.90) ] &&
		skip "Do not support Data-on-MDT before 2.11"

	$LFS setstripe -E 1M -L mdt -E EOF -c 2 $DIR/$tfile
	replay_barrier $SINGLEMDS
	echo "dom_data" | dd of=$DIR/$tfile bs=8 count=1
	# lock is not canceled and will be replayed
	fail $SINGLEMDS

	[ $(cat $DIR/$tfile) == "dom_data" ] ||
		error "Wrong file content after failover"
}
run_test 131a "DoM file write lock replay"

test_131b() {
	[ "$MDS1_VERSION" -lt $(version_code 2.10.90) ] &&
		skip "Do not support Data-on-MDT before 2.11"

	# refresh grants so write after replay_barrier doesn't
	# turn sync
	$LFS setstripe -E 1M -L mdt -E EOF -c 2 $DIR/$tfile-2
	stack_trap "rm -f $DIR/$tfile-2"
	dd if=/dev/zero of=$DIR/$tfile-2 bs=64k count=2 ||
		error "can't dd"
	$LFS setstripe -E 1M -L mdt -E EOF -c 2 $DIR/$tfile
	replay_barrier $SINGLEMDS
	echo "dom_data" | dd of=$DIR/$tfile bs=8 count=1
	cancel_lru_locks mdc

	fail $SINGLEMDS

	[ $(cat $DIR/$tfile) == "dom_data" ] ||
		error "Wrong file content after failover"
}
run_test 131b "DoM file write replay"

test_132a() {
	[ "$MDS1_VERSION" -lt $(version_code 2.12.0) ] &&
		skip "Need MDS version 2.12.0 or later"

	$LFS setstripe -E 1M -c 1 -E EOF -c 2 $DIR/$tfile
	replay_barrier $SINGLEMDS
	# write over the first component size cause next component instantiation
	dd if=/dev/urandom of=$DIR/$tfile bs=1M count=1 seek=1 ||
		error "dd to $DIR/$tfile failed"
	lfs getstripe $DIR/$tfile

	cksum=$(md5sum $DIR/$tfile | awk '{print $1}')
	$LFS getstripe -I2 $DIR/$tfile | grep -q lmm_objects ||
		error "Component #1 was not instantiated"

	fail $SINGLEMDS

	lfs getstripe $DIR/$tfile
	$LFS getstripe -I2 $DIR/$tfile | grep -q lmm_objects ||
		error "Component #1 instantiation was not replayed"
	cksum2=$(md5sum $DIR/$tfile | awk '{print $1}')
	if [ $cksum != $cksum2 ] ; then
		error_noexit "New cksum $cksum2 does not match original $cksum"
	fi
}
run_test 132a "PFL new component instantiate replay"

test_133() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	([ $FAILURE_MODE == "HARD" ] &&
		[ "$(facet_host mds1)" == "$(facet_host mds2)" ]) &&
		skip "MDTs needs to be on diff hosts for HARD fail mode" &&
		return 0

	local remote_dir=$DIR/$tdir/remote_dir

	mkdir -p $DIR/$tdir || error "mkdir $DIR/$tdir failed"
	$LFS mkdir -i 1 $remote_dir

	umount $MOUNT
	do_facet mds2 $LCTL set_param seq.srv*MDT0001.space=clear

	zconf_mount $(hostname) $MOUNT
	client_up || return 1

	#define OBD_FAIL_MDS_ALL_REQUEST_NET     0x123
	# SEQ_QUERY                       = 700
	do_facet mds1 $LCTL set_param fail_val=700 fail_loc=0x80000123
	cp /etc/hosts $remote_dir/file &
	local pid=$!
	sleep 1

	fail_nodf mds1

	wait $pid || error "cp failed"
	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 133 "check resend of ongoing requests for lwp during failover"

test_134() {
	[ $OSTCOUNT -lt 2 ] && skip "needs >= 2 OSTs" && return 0
	(( $MDS1_VERSION >= $(version_code 2.13.56) )) ||
		skip "need MDS version >= 2.13.56"

	pool_add pool_134
	pool_add_targets pool_134 1 1

	mkdir -p $DIR/$tdir/{A,B}
	$LFS setstripe -p pool_134 $DIR/$tdir/A
	$LFS setstripe -E EOF -p pool_134 $DIR/$tdir/B

	replay_barrier mds1

	touch $DIR/$tdir/A/$tfile || error "touch non-pfl file failed"
	touch $DIR/$tdir/B/$tfile || error "touch pfl failed"

	fail mds1

	[ -f $DIR/$tdir/A/$tfile ] || error "non-pfl file does not exist"
	[ -f $DIR/$tdir/B/$tfile ] || error "pfl file does not exist"
}
run_test 134 "replay creation of a file created in a pool"

# LU-14027
test_135() {
	mkdir $DIR/$tdir || error "mkdir $DIR/$tdir failed"

	# All files to ost1
	$LFS setstripe -S $((128 * 1024)) -i 0 $DIR/$tdir

	replay_barrier ost1

	# Create 20 files so we have 20 ost locks
	for i in $(seq 20) ; do
		echo blah > $DIR/$tdir/file.${i}
	done

	shutdown_facet ost1
	reboot_facet ost1
	change_active ost1
	wait_for_facet ost1

	#define OBD_FAIL_TGT_REPLAY_RECONNECT     0x32d
	# Make sure lock replay server side never completes and errors out.
	do_facet ost1 "$LCTL set_param fail_val=20"
	do_facet ost1 "$LCTL set_param fail_loc=0x32d"

	mount_facet ost1

	# Now make sure we notice
	(sync;sync;sync) &
	local PID=$?
	sleep 20 # should we do something proactive to make reconnects go?
	kill -0 $PID || error "Unexpected sync success"

	shutdown_facet ost1
	reboot_facet ost1
	change_active ost1
	wait_for_facet ost1

	do_facet ost1 "$LCTL set_param fail_loc=0"
	mount_facet ost1
	echo blah > $DIR/$tdir/file.test2

	rm -rf $DIR/$tdir
}
run_test 135 "Server failure in lock replay phase"

complete $SECONDS
check_and_cleanup_lustre
exit_status
