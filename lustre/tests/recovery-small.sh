#!/bin/bash

set -e

#         bug  5493  LU2034
ALWAYS_EXCEPT="52    60      $RECOVERY_SMALL_EXCEPT"

export MULTIOP=${MULTIOP:-multiop}
PTLDEBUG=${PTLDEBUG:--1}
LUSTRE=${LUSTRE:-`dirname $0`/..}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}
init_logging

require_dsh_mds || exit 0

# also long tests: 19, 21a, 21e, 21f, 23, 27
#                                   1  2.5  2.5    4    4          (min)"
[ "$SLOW" = "no" ] && EXCEPT_SLOW="17  26a  26b    50   51     57"

[ $(facet_fstype $SINGLEMDS) = "zfs" ] &&
# bug number for skipped test:	      LU-2547
	ALWAYS_EXCEPT="$ALWAYS_EXCEPT 24a 24b"

build_test_filter

# Allow us to override the setup if we already have a mounted system by
# setting SETUP=" " and CLEANUP=" "
SETUP=${SETUP:-""}
CLEANUP=${CLEANUP:-""}

check_and_setup_lustre

assert_DIR
rm -rf $DIR/d[0-9]* $DIR/f.${TESTSUITE}*

test_1() {
	local f1="$DIR/$tfile"
	local f2="$DIR/$tfile.2"

	drop_request "mcreate $f1" ||
		error_noexit "create '$f1': drop req"

	drop_reint_reply "mcreate $f2" ||
		error_noexit "create '$f2': drop rep"

	drop_request "tchmod 111 $f2" ||
		error_noexit "chmod '$f2': drop req"

	drop_reint_reply "tchmod 666 $f2" ||
		error_noexit "chmod '$f2': drop rep"

	drop_request "statone $f2" ||
		error_noexit "stat '$f2': drop req"

	drop_reply  "statone $f2" ||
		error_noexit "stat '$f2': drop rep"
}
run_test 1 "create, chmod, stat: drop req, drop rep"

test_4() {
	local t=$DIR/$tfile
	do_facet_create_file client $t 10K ||
		error_noexit "Create file $t"

	drop_request "cat $t > /dev/null" ||
		error_noexit "Open request for $t file"

	drop_reply "cat $t > /dev/null" ||
		error_noexit "Open replay for $t file"
}
run_test 4 "open: drop req, drop rep"

test_5() {
	local T=$DIR/$tfile
	local R="$T-renamed"
	local RR="$T-renamed-again"
	do_facet_create_file client $T 10K ||
		error_noexit "Create file $T"

	drop_request "mv $T $R" ||
		error_noexit "Rename $T"

	drop_reint_reply "mv $R $RR" ||
		error_noexit "Failed rename replay on $R"

	do_facet client "checkstat -v $RR" ||
		error_noexit "checkstat error on $RR"

	do_facet client "rm $RR" ||
		error_noexit "Can't remove file $RR"
}
run_test 5 "rename: drop req, drop rep"

test_6() {
	local T=$DIR/$tfile
	local LINK1=$DIR/$tfile.link1
	local LINK2=$DIR/$tfile.link2

	do_facet_create_file client $T 10K ||
		error_noexit "Create file $T"

	drop_request "mlink $T $LINK1" ||
		error_noexit "mlink request for $T"

	drop_reint_reply "mlink $T $LINK2" ||
		error_noexit "mlink reply for $T"

	drop_request "munlink $LINK1" ||
		error_noexit "munlink request for $T"

	drop_reint_reply "munlink $LINK2" ||
		error_noexit "munlink reply for $T"

	do_facet client "rm $T" ||
		error_noexit "Can't remove file $T"
}
run_test 6 "link, unlink: drop req, drop rep"

#bug 1423
test_8() {
    drop_reint_reply "touch $DIR/$tfile"    || return 1
}
run_test 8 "touch: drop rep (bug 1423)"

#bug 1420
test_9() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	local t1=${tfile}.1
	local t2=${tfile}.2
	do_facet_random_file client $TMP/$tfile 1K ||
		error_noexit "Create random file $TMP/$tfile"
	# make this big, else test 9 doesn't wait for bulk -- bz 5595
	do_facet_create_file client $TMP/$t1 4M ||
		error_noexit "Create file $TMP/$t1"
	do_facet client "cp $TMP/$t1 $DIR/$t1" ||
		error_noexit "Can't copy to $DIR/$t1 file"
	pause_bulk "cp $TMP/$tfile $DIR/$tfile" ||
		error_noexit "Can't pause_bulk copy"
	do_facet client "cp $TMP/$t1 $DIR/$t2" ||
		error_noexit "Can't copy file"
	do_facet client "sync"
	do_facet client "rm $DIR/$tfile $DIR/$t2 $DIR/$t1" ||
		error_noexit "Can't remove files"
	do_facet client "rm $TMP/$t1 $TMP/$tfile"
}
run_test 9 "pause bulk on OST (bug 1420)"

#bug 1521
test_10a() {
	local before=$(date +%s)
	local evict

	do_facet client "stat $DIR > /dev/null"  ||
		error "failed to stat $DIR: $?"
	drop_bl_callback "chmod 0777 $DIR" ||
		error "failed to chmod $DIR: $?"

	# let the client reconnect
	client_reconnect
	evict=$(do_facet client $LCTL get_param mdc.$FSNAME-MDT*.state |
	  awk -F"[ [,]" '/EVICTED ]$/ { if (mx<$5) {mx=$5;} } END { print mx }')
	[ ! -z "$evict" ] && [[ $evict -gt $before ]] ||
		(do_facet client $LCTL get_param mdc.$FSNAME-MDT*.state;
		    error "no eviction: $evict before:$before")

	do_facet client checkstat -v -p 0777 $DIR ||
		error "client checkstat failed: $?"
}
run_test 10a "finish request on server after client eviction (bug 1521)"

test_10b() {
	local before=$(date +%s)
	local evict

	[[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.6.53) ]] &&
		skip "Need MDS version at least 2.6.53" && return
	do_facet client "stat $DIR > /dev/null"  ||
		error "failed to stat $DIR: $?"
	drop_bl_callback_once "chmod 0777 $DIR" ||
		error "failed to chmod $DIR: $?"

	# let the client reconnect
	client_reconnect
	evict=$(do_facet client $LCTL get_param mdc.$FSNAME-MDT*.state |
	  awk -F"[ [,]" '/EVICTED ]$/ { if (mx<$5) {mx=$5;} } END { print mx }')

	[ -z "$evict" ] || [[ $evict -le $before ]] ||
		(do_facet client $LCTL get_param mdc.$FSNAME-MDT*.state;
		    error "eviction happened: $evict before:$before")

	do_facet client checkstat -v -p 0777 $DIR ||
 		error "client checkstat failed: $?"
}
run_test 10b "re-send BL AST"

test_10c() {
	local before=$(date +%s)
	local evict
	local mdccli
	local mdcpath
	local conn_uuid
	local workdir
	local pid
	local rc

	workdir="${DIR}/${tdir}"
	mkdir -p ${workdir} || error "can't create workdir $?"
	stat ${workdir} > /dev/null ||
		error "failed to stat ${workdir}: $?"
	mdtidx=$($LFS getdirstripe -i ${workdir})
	mdtname=$($LFS mdts ${workdir} | grep -e "^$mdtidx:" |
		  awk '{sub("_UUID", "", $2); print $2;}')
	#assume one client
	mdccli=$($LCTL dl | grep "${mdtname}-mdc" | awk '{print $4;}')
	conn_uuid=$($LCTL get_param -n mdc.${mdccli}.mds_conn_uuid)
	mdcpath="mdc.${mdccli}.import=connection=${conn_uuid}"

	drop_bl_callback_once "chmod 0777 ${workdir}" &
	pid=$!

	# let chmod blocked
	sleep 1
	# force client reconnect
	$LCTL set_param "${mdcpath}"

	# wait client reconnect
	client_reconnect
	wait $pid
	rc=$?
	evict=$($LCTL get_param mdc.${mdccli}.state |
	   awk -F"[ [,]" '/EVICTED]$/ { if (t<$4) {t=$4;} } END { print t }')

	[[ $evict -le $before ]] ||
		( $LCTL get_param mdc.$FSNAME-MDT*.state;
		    error "eviction happened: $EVICT before:$BEFORE" )

	[ $rc -eq 0 ] || error "chmod must finished OK"
	checkstat -v -p 0777 "${workdir}" ||
		error "client checkstat failed: $?"
}
run_test 10c "re-send BL AST vs reconnect race (LU-5569)"

test_10d() {
	local before=$(date +%s)
	local evict

	[[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.6.90) ]] &&
		skip "Need MDS version at least 2.6.90" && return

	# sleep 1 is to make sure that BEFORE is not equal to EVICTED below
	sleep 1
	rm -f $TMP/$tfile
	echo -n ", world" | dd of=$TMP/$tfile bs=1c seek=5

	mount_client $MOUNT2

	cancel_lru_locks osc
	$LFS setstripe -i 0 -c 1 $DIR1/$tfile
	echo -n hello > $DIR1/$tfile

	stat $DIR2/$tfile >& /dev/null
	$LCTL set_param fail_err=71
	drop_bl_callback "echo -n \\\", world\\\" >> $DIR2/$tfile"

	client_reconnect

	cmp $DIR1/$tfile $DIR2/$tfile || error "file contents differ"
	cmp $DIR1/$tfile $TMP/$tfile || error "wrong content found"

	evict=$(do_facet client $LCTL get_param osc.$FSNAME-OST0000*.state | \
		tr -d '\-\[\] ' | \
	  awk -F"[ [,]" '/EVICTED$/ { if (mx<$1) {mx=$1;} } END { print mx }')

	[[ $evict -gt $before ]] ||
		(do_facet client $LCTL get_param osc.$FSNAME-OST0000*.state;
		    error "no eviction: $evict before:$before")

	$LCTL set_param fail_err=0
	rm $TMP/$tfile
	umount_client $MOUNT2
}
run_test 10d "test failed blocking ast"

#bug 2460
# wake up a thread waiting for completion after eviction
test_11(){
	do_facet client $MULTIOP $DIR/$tfile Ow  ||
		{ error "multiop write failed: $?"; return 1; }
	do_facet client $MULTIOP $DIR/$tfile or  ||
		{ error "multiop read failed: $?"; return 2; }

	cancel_lru_locks osc

	do_facet client $MULTIOP $DIR/$tfile or  ||
		{ error "multiop read failed: $?"; return 3; }
	drop_bl_callback_once $MULTIOP $DIR/$tfile Ow ||
		echo "evicted as expected"

	do_facet client munlink $DIR/$tfile ||
		{ error "munlink failed: $?"; return 4; }
	# allow recovery to complete
	client_up || client_up || sleep $TIMEOUT
}
run_test 11 "wake up a thread waiting for completion after eviction (b=2460)"

#b=2494
test_12(){
	$LCTL mark $MULTIOP $DIR/$tfile OS_c
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x115"
	clear_failloc $SINGLEMDS $((TIMEOUT * 2)) &
	multiop_bg_pause $DIR/$tfile OS_c ||
		{ error "multiop failed: $?"; return 1; }
	PID=$!
#define OBD_FAIL_MDS_CLOSE_NET           0x115
	kill -USR1 $PID
	echo "waiting for multiop $PID"
	wait $PID || { error "wait for multiop faile: $?"; return 2; }
	do_facet client munlink $DIR/$tfile ||
		{ error "client munlink failed: $?"; return 3; }
	# allow recovery to complete
	client_up || client_up || sleep $TIMEOUT
}
run_test 12 "recover from timed out resend in ptlrpcd (b=2494)"

# Bug 113, check that readdir lost recv timeout works.
test_13() {
	mkdir -p $DIR/$tdir || { error "mkdir failed: $?"; return 1; }
	touch $DIR/$tdir/newentry || { error "touch failed: $?"; return 2; }
# OBD_FAIL_MDS_READPAGE_NET|OBD_FAIL_ONCE
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000104"
	ls $DIR/$tdir || { error "ls failed: $?"; return 3; }
	do_facet $SINGLEMDS "lctl set_param fail_loc=0"
	rm -rf $DIR/$tdir || { error "remove test dir failed: $?"; return 4; }
}
run_test 13 "mdc_readpage restart test (bug 1138)"

# Bug 113, check that readdir lost send timeout works.
test_14() {
    mkdir -p $DIR/$tdir
    touch $DIR/$tdir/newentry
# OBD_FAIL_MDS_SENDPAGE|OBD_FAIL_ONCE
    do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000106"
    ls $DIR/$tdir || return 1
    do_facet $SINGLEMDS "lctl set_param fail_loc=0"
}
run_test 14 "mdc_readpage resend test (bug 1138)"

test_15() {
    do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000128"
    touch $DIR/$tfile && return 1
    return 0
}
run_test 15 "failed open (-ENOMEM)"

READ_AHEAD=`lctl get_param -n llite.*.max_read_ahead_mb | head -n 1`
stop_read_ahead() {
   lctl set_param -n llite.*.max_read_ahead_mb 0
}

start_read_ahead() {
   lctl set_param -n llite.*.max_read_ahead_mb $READ_AHEAD
}

test_16() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	do_facet_random_file client $TMP/$tfile 100K ||
		{ error_noexit "Create random file $TMP/$T" ; return 0; }
	do_facet client "cp $TMP/$tfile $DIR/$tfile" ||
		{ error_noexit "Copy to $DIR/$tfile file" ; return 0; }
	sync
	stop_read_ahead

#define OBD_FAIL_PTLRPC_BULK_PUT_NET 0x504 | OBD_FAIL_ONCE
	do_facet ost1 "lctl set_param fail_loc=0x80000504"
	cancel_lru_locks osc
	# OST bulk will time out here, client resends
	do_facet client "cmp $TMP/$tfile $DIR/$tfile" || return 1
	do_facet ost1 lctl set_param fail_loc=0
	# give recovery a chance to finish (shouldn't take long)
	sleep $TIMEOUT
	do_facet client "cmp $TMP/$tfile $DIR/$tfile" || return 2
	start_read_ahead
	rm -f $TMP/$tfile
}
run_test 16 "timeout bulk put, don't evict client (2732)"

test_17() {
    local at_max_saved=0

    remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	local SAMPLE_FILE=$TMP/$tfile
	do_facet_random_file client $SAMPLE_FILE 20K ||
		{ error_noexit "Create random file $SAMPLE_FILE" ; return 0; }

    # With adaptive timeouts, bulk_get won't expire until adaptive_timeout_max
    if at_is_enabled; then
        at_max_saved=$(at_max_get ost1)
        at_max_set $TIMEOUT ost1
    fi

    # OBD_FAIL_PTLRPC_BULK_GET_NET 0x0503 | OBD_FAIL_ONCE
    # OST bulk will time out here, client retries
    do_facet ost1 lctl set_param fail_loc=0x80000503
    # need to ensure we send an RPC
    do_facet client cp $SAMPLE_FILE $DIR/$tfile
    sync

    # with AT, client will wait adaptive_max*factor+net_latency before
    # expiring the req, hopefully timeout*2 is enough
    sleep $(($TIMEOUT*2))

    do_facet ost1 lctl set_param fail_loc=0
    do_facet client "df $DIR"
    # expect cmp to succeed, client resent bulk
    do_facet client "cmp $SAMPLE_FILE $DIR/$tfile" || return 3
    do_facet client "rm $DIR/$tfile" || return 4
    [ $at_max_saved -ne 0 ] && at_max_set $at_max_saved ost1
    return 0
}
run_test 17 "timeout bulk get, don't evict client (2732)"

test_18a() {
    [ -z ${ost2_svc} ] && skip_env "needs 2 osts" && return 0

	do_facet_create_file client $TMP/$tfile 20K ||
		{ error_noexit "Create file $TMP/$tfile" ; return 0; }

    do_facet client mkdir -p $DIR/$tdir
    f=$DIR/$tdir/$tfile

    cancel_lru_locks osc
    pgcache_empty || return 1

    # 1 stripe on ost2
    $LFS setstripe -i 1 -c 1 $f
    stripe_index=$($LFS getstripe -i $f)
    if [ $stripe_index -ne 1 ]; then
        $LFS getstripe $f
        error "$f: stripe_index $stripe_index != 1" && return
    fi

    do_facet client cp $TMP/$tfile $f
    sync
    local osc2dev=`lctl get_param -n devices | grep ${ost2_svc}-osc- | egrep -v 'MDT' | awk '{print $1}'`
    $LCTL --device $osc2dev deactivate || return 3
    # my understanding is that there should be nothing in the page
    # cache after the client reconnects?     
    rc=0
    pgcache_empty || rc=2
    $LCTL --device $osc2dev activate
    rm -f $f $TMP/$tfile
    return $rc
}
run_test 18a "manual ost invalidate clears page cache immediately"

test_18b() {
    remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	do_facet_create_file client $TMP/$tfile 20K ||
		{ error_noexit "Create file $TMP/$tfile" ; return 0; }

    do_facet client mkdir -p $DIR/$tdir
    f=$DIR/$tdir/$tfile

    cancel_lru_locks osc
    pgcache_empty || return 1

    $LFS setstripe -i 0 -c 1 $f
    stripe_index=$($LFS getstripe -i $f)
    if [ $stripe_index -ne 0 ]; then
        $LFS getstripe $f
        error "$f: stripe_index $stripe_index != 0" && return
    fi

    do_facet client cp $TMP/$tfile $f
    sync
    ost_evict_client
    # allow recovery to complete
    sleep $((TIMEOUT + 2))
    # my understanding is that there should be nothing in the page
    # cache after the client reconnects?     
    rc=0
    pgcache_empty || rc=2
    rm -f $f $TMP/$tfile
    return $rc
}
run_test 18b "eviction and reconnect clears page cache (2766)"

test_18c() {
    remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	do_facet_create_file client $TMP/$tfile 20K ||
		{ error_noexit "Create file $TMP/$tfile" ; return 0; }

    do_facet client mkdir -p $DIR/$tdir
    f=$DIR/$tdir/$tfile

    cancel_lru_locks osc
    pgcache_empty || return 1

    $LFS setstripe -i 0 -c 1 $f
    stripe_index=$($LFS getstripe -i $f)
    if [ $stripe_index -ne 0 ]; then
        $LFS getstripe $f
        error "$f: stripe_index $stripe_index != 0" && return
    fi

    do_facet client cp $TMP/$tfile $f
    sync
    ost_evict_client

    # OBD_FAIL_OST_CONNECT_NET2
    # lost reply to connect request
    do_facet ost1 lctl set_param fail_loc=0x80000225
    # force reconnect
    sleep 1
    df $MOUNT > /dev/null 2>&1
    sleep 2
    # my understanding is that there should be nothing in the page
    # cache after the client reconnects?     
    rc=0
    pgcache_empty || rc=2
    rm -f $f $TMP/$tfile
    return $rc
}
run_test 18c "Dropped connect reply after eviction handing (14755)"

test_19a() {
	local BEFORE=`date +%s`
	local EVICT

	mount_client $DIR2 || error "failed to mount $DIR2"

	# cancel cached locks from OST to avoid eviction from it
	cancel_lru_locks osc

	do_facet client "stat $DIR > /dev/null"  ||
		error "failed to stat $DIR: $?"
	drop_ldlm_cancel "chmod 0777 $DIR2" ||
		error "failed to chmod $DIR2"

	umount_client $DIR2

	# let the client reconnect
	client_reconnect
	EVICT=$(do_facet client $LCTL get_param mdc.$FSNAME-MDT*.state |
		awk -F"[ [,]" '/EVICTED ]$/ \
			{ if (mx<$5) {mx=$5;} } END { print mx }')

	[ ! -z "$EVICT" ] && [[ $EVICT -gt $BEFORE ]] ||
		(do_facet client $LCTL get_param mdc.$FSNAME-MDT*.state;
		    error "no eviction: $EVICT before:$BEFORE")
}
run_test 19a "test expired_lock_main on mds (2867)"

test_19b() {
	local BEFORE=`date +%s`
	local EVICT

	mount_client $DIR2 || error "failed to mount $DIR2: $?"

	# cancel cached locks from MDT to avoid eviction from it
	cancel_lru_locks mdc

	do_facet client $MULTIOP $DIR/$tfile Ow ||
		error "failed to run multiop: $?"
	drop_ldlm_cancel $MULTIOP $DIR2/$tfile Ow ||
		error "failed to ldlm_cancel: $?"

	umount_client $DIR2 || error "failed to unmount $DIR2: $?"
	do_facet client munlink $DIR/$tfile ||
		error "failed to unlink $DIR/$tfile: $?"

	# let the client reconnect
	client_reconnect
	EVICT=$(do_facet client $LCTL get_param osc.$FSNAME-OST*.state |
		awk -F"[ [,]" '/EVICTED ]$/ \
			{ if (mx < $5) {mx = $5;} } END { print mx }')

	[ ! -z "$EVICT" ] && [[ $EVICT -gt $BEFORE ]] ||
		(do_facet client $LCTL get_param osc.$FSNAME-OST*.state;
		    error "no eviction: $EVICT before:$BEFORE")
}
run_test 19b "test expired_lock_main on ost (2867)"

test_19c() {
	local BEFORE=`date +%s`

	mount_client $DIR2
	$LCTL set_param ldlm.namespaces.*.early_lock_cancel=0

	mkdir -p $DIR1/$tfile
	stat $DIR1/$tfile

#define OBD_FAIL_PTLRPC_CANCEL_RESEND 0x516
	do_facet mds $LCTL set_param fail_loc=0x80000516

	touch $DIR2/$tfile/file1 &
	PID1=$!
	# let touch to get blocked on the server
	sleep 2

	wait $PID1
	$LCTL set_param ldlm.namespaces.*.early_lock_cancel=1
	umount_client $DIR2

	# let the client reconnect
	sleep 5
	EVICT=$(do_facet client $LCTL get_param mdc.$FSNAME-MDT*.state |
	  awk -F"[ [,]" '/EVICTED ]$/ { if (mx<$5) {mx=$5;} } END { print mx }')

	[ -z "$EVICT" ] || [[ $EVICT -le $BEFORE ]] || error "eviction happened"
}
run_test 19c "check reconnect and lock resend do not trigger expired_lock_main"

test_20a() {	# bug 2983 - ldlm_handle_enqueue cleanup
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	mkdir -p $DIR/$tdir
	$LFS setstripe -i 0 -c 1 $DIR/$tdir/${tfile}
	multiop_bg_pause $DIR/$tdir/${tfile} O_wc || return 1
	MULTI_PID=$!
	cancel_lru_locks osc
#define OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR 0x308
	do_facet ost1 lctl set_param fail_loc=0x80000308
	kill -USR1 $MULTI_PID
	wait $MULTI_PID
	rc=$?
	[ $rc -eq 0 ] && error "multiop didn't fail enqueue: rc $rc" || true
}
run_test 20a "ldlm_handle_enqueue error (should return error)" 

test_20b() {	# bug 2986 - ldlm_handle_enqueue error during open
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	mkdir -p $DIR/$tdir
	$LFS setstripe -i 0 -c 1 $DIR/$tdir/${tfile}
	cancel_lru_locks osc
#define OBD_FAIL_LDLM_ENQUEUE_EXTENT_ERR 0x308
	do_facet ost1 lctl set_param fail_loc=0x80000308
	dd if=/etc/hosts of=$DIR/$tdir/$tfile && \
		error "didn't fail open enqueue" || true
}
run_test 20b "ldlm_handle_enqueue error (should return error)"

test_21a() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop_bg_pause $DIR/$tdir-1/f O_c || return 1
       close_pid=$!

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000129"
       $MULTIOP $DIR/$tdir-2/f Oc &
       open_pid=$!
       sleep 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000115"
       kill -USR1 $close_pid
       cancel_lru_locks mdc
       wait $close_pid || return 1
       wait $open_pid || return 2
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 3
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 4

       rm -rf $DIR/$tdir-*
}
run_test 21a "drop close request while close and open are both in flight"

test_21b() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop_bg_pause $DIR/$tdir-1/f O_c || return 1
       close_pid=$!

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000107"
       mcreate $DIR/$tdir-2/f &
       open_pid=$!
       sleep 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       kill -USR1 $close_pid
       cancel_lru_locks mdc
       wait $close_pid || return 1
       wait $open_pid || return 3

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 4
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 5
       rm -rf $DIR/$tdir-*
}
run_test 21b "drop open request while close and open are both in flight"

test_21c() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop_bg_pause $DIR/$tdir-1/f O_c || return 1
       close_pid=$!

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000107"
       mcreate $DIR/$tdir-2/f &
       open_pid=$!
       sleep 3
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000115"
       kill -USR1 $close_pid
       cancel_lru_locks mdc
       wait $close_pid || return 1
       wait $open_pid || return 2

       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3
       rm -rf $DIR/$tdir-*
}
run_test 21c "drop both request while close and open are both in flight"

test_21d() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop_bg_pause $DIR/$tdir-1/f O_c || return 1
       pid=$!

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000129"
       $MULTIOP $DIR/$tdir-2/f Oc &
       sleep 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000122"
       kill -USR1 $pid
       cancel_lru_locks mdc
       wait $pid || return 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3

       rm -rf $DIR/$tdir-*
}
run_test 21d "drop close reply while close and open are both in flight"

test_21e() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop_bg_pause $DIR/$tdir-1/f O_c || return 1
       pid=$!

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000119"
       touch $DIR/$tdir-2/f &
       sleep 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       kill -USR1 $pid
       cancel_lru_locks mdc
       wait $pid || return 1

       sleep $TIMEOUT
       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3
       rm -rf $DIR/$tdir-*
}
run_test 21e "drop open reply while close and open are both in flight"

test_21f() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop_bg_pause $DIR/$tdir-1/f O_c || return 1
       pid=$!

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000119"
       touch $DIR/$tdir-2/f &
       sleep 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000122"
       kill -USR1 $pid
       cancel_lru_locks mdc
       wait $pid || return 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3
       rm -rf $DIR/$tdir-*
}
run_test 21f "drop both reply while close and open are both in flight"

test_21g() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop_bg_pause $DIR/$tdir-1/f O_c || return 1
       pid=$!

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000119"
       touch $DIR/$tdir-2/f &
       sleep 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000115"
       kill -USR1 $pid
       cancel_lru_locks mdc
       wait $pid || return 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 2
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 3
       rm -rf $DIR/$tdir-*
}
run_test 21g "drop open reply and close request while close and open are both in flight"

test_21h() {
       mkdir -p $DIR/$tdir-1
       mkdir -p $DIR/$tdir-2
       multiop_bg_pause $DIR/$tdir-1/f O_c || return 1
       pid=$!

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000107"
       touch $DIR/$tdir-2/f &
       touch_pid=$!
       sleep 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000122"
       cancel_lru_locks mdc
       kill -USR1 $pid
       wait $pid || return 1
       do_facet $SINGLEMDS "lctl set_param fail_loc=0"

       wait $touch_pid || return 2

       $CHECKSTAT -t file $DIR/$tdir-1/f || return 3
       $CHECKSTAT -t file $DIR/$tdir-2/f || return 4
       rm -rf $DIR/$tdir-*
}
run_test 21h "drop open request and close reply while close and open are both in flight"

# bug 3462 - multiple MDC requests
test_22() {
    f1=$DIR/${tfile}-1
    f2=$DIR/${tfile}-2
    
    do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000115"
    $MULTIOP $f2 Oc &
    close_pid=$!

    sleep 1
    $MULTIOP $f1 msu || return 1

    cancel_lru_locks mdc
    do_facet $SINGLEMDS "lctl set_param fail_loc=0"

    wait $close_pid || return 2
    rm -rf $f2 || return 4
}
run_test 22 "drop close request and do mknod"

test_23() { #b=4561
    multiop_bg_pause $DIR/$tfile O_c || return 1
    pid=$!
    # give a chance for open
    sleep 5

    # try the close
    drop_request "kill -USR1 $pid"

    fail $SINGLEMDS
    wait $pid || return 1
    return 0
}
run_test 23 "client hang when close a file after mds crash"

test_24a() { # bug 11710 details correct fsync() behavior
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	mkdir -p $DIR/$tdir
	$LFS setstripe -i 0 -c 1 $DIR/$tdir
	cancel_lru_locks osc
	multiop_bg_pause $DIR/$tdir/$tfile Owy_wyc || return 1
	MULTI_PID=$!
	ost_evict_client
	kill -USR1 $MULTI_PID
	wait $MULTI_PID
	rc=$?
	lctl set_param fail_loc=0x0
	client_reconnect
	[ $rc -eq 0 ] &&
		error_ignore bz5494 "multiop didn't fail fsync: rc $rc" || true
}
run_test 24a "fsync error (should return error)"

wait_client_evicted () {
	local facet=$1
	local exports=$2
	local varsvc=${facet}_svc

	wait_update $(facet_active_host $facet) \
		"lctl get_param -n *.${!varsvc}.num_exports | cut -d' ' -f2" \
		$((exports - 1)) $3
}

test_24b() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	dmesg -c > /dev/null
	mkdir -p $DIR/$tdir
	lfs setstripe $DIR/$tdir -s 0 -i 0 -c 1
	cancel_lru_locks osc
	multiop_bg_pause $DIR/$tdir/$tfile-1 Ow8192_yc ||
		error "mulitop Ow8192_yc failed"

	MULTI_PID1=$!
	multiop_bg_pause $DIR/$tdir/$tfile-2 Ow8192_c ||
		error "mulitop Ow8192_c failed"

	MULTI_PID2=$!
	ost_evict_client

	kill -USR1 $MULTI_PID1
	wait $MULTI_PID1
	rc1=$?
	kill -USR1 $MULTI_PID2
	wait $MULTI_PID2
	rc2=$?
	lctl set_param fail_loc=0x0
	client_reconnect
	[ $rc1 -eq 0 -o $rc2 -eq 0 ] &&
	error_ignore bz5494 "multiop didn't fail fsync: $rc1 or close: $rc2" ||
		true

	dmesg | grep "dirty page discard:" ||
		error "no discarded dirty page found!"
}
run_test 24b "test dirty page discard due to client eviction"

test_26a() {      # was test_26 bug 5921 - evict dead exports by pinger
# this test can only run from a client on a separate node.
	remote_ost || { skip "local OST" && return 0; }
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0
	remote_mds || { skip "local MDS" && return 0; }

        if [ $(facet_host mgs) = $(facet_host ost1) ]; then
                skip "msg and ost1 are at the same node"
                return 0
        fi

	check_timeout || return 1

	local OST_NEXP=$(do_facet ost1 lctl get_param -n obdfilter.${ost1_svc}.num_exports | cut -d' ' -f2)

	echo starting with $OST_NEXP OST exports
# OBD_FAIL_PTLRPC_DROP_RPC 0x505
	do_facet client lctl set_param fail_loc=0x505
        # evictor takes PING_EVICT_TIMEOUT + 3 * PING_INTERVAL to evict.
        # But if there's a race to start the evictor from various obds,
        # the loser might have to wait for the next ping.

	local rc=0
	wait_client_evicted ost1 $OST_NEXP $((TIMEOUT * 2 + TIMEOUT * 3 / 4))
	rc=$?
	do_facet client lctl set_param fail_loc=0x0
        [ $rc -eq 0 ] || error "client not evicted from OST"
}
run_test 26a "evict dead exports"

test_26b() {      # bug 10140 - evict dead exports by pinger
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

        if [ $(facet_host mgs) = $(facet_host ost1) ]; then
                skip "msg and ost1 are at the same node"
                return 0
        fi

	check_timeout || return 1
	clients_up
	zconf_mount `hostname` $MOUNT2 ||
                { error "Failed to mount $MOUNT2"; return 2; }
	sleep 1 # wait connections being established

	local MDS_NEXP=$(do_facet $SINGLEMDS lctl get_param -n mdt.${mds1_svc}.num_exports | cut -d' ' -f2)
	local OST_NEXP=$(do_facet ost1 lctl get_param -n obdfilter.${ost1_svc}.num_exports | cut -d' ' -f2)

	echo starting with $OST_NEXP OST and $MDS_NEXP MDS exports

	zconf_umount `hostname` $MOUNT2 -f

	# PING_INTERVAL max(obd_timeout / 4, 1U)
	# PING_EVICT_TIMEOUT (PING_INTERVAL * 6)

	# evictor takes PING_EVICT_TIMEOUT + 3 * PING_INTERVAL to evict.  
	# But if there's a race to start the evictor from various obds, 
	# the loser might have to wait for the next ping.
	# = 9 * PING_INTERVAL + PING_INTERVAL
	# = 10 PING_INTERVAL = 10 obd_timeout / 4 = 2.5 obd_timeout
	# let's wait $((TIMEOUT * 3)) # bug 19887
	local rc=0
	wait_client_evicted ost1 $OST_NEXP $((TIMEOUT * 3)) || \
		error "Client was not evicted by ost" rc=1
	wait_client_evicted $SINGLEMDS $MDS_NEXP $((TIMEOUT * 3)) || \
		error "Client was not evicted by mds"
}
run_test 26b "evict dead exports"

test_27() {
	mkdir -p $DIR/$tdir
	writemany -q -a $DIR/$tdir/$tfile 0 5 &
	CLIENT_PID=$!
	sleep 1
	local save_FAILURE_MODE=$FAILURE_MODE
	FAILURE_MODE="SOFT"
	facet_failover $SINGLEMDS
#define OBD_FAIL_OSC_SHUTDOWN            0x407
	do_facet $SINGLEMDS lctl set_param fail_loc=0x80000407
	# need to wait for reconnect
	echo waiting for fail_loc
	wait_update_facet $SINGLEMDS "lctl get_param -n fail_loc" "-2147482617"
	facet_failover $SINGLEMDS
	#no crashes allowed!
        kill -USR1 $CLIENT_PID
	wait $CLIENT_PID 
	true
	FAILURE_MODE=$save_FAILURE_MODE
}
run_test 27 "fail LOV while using OSC's"

test_28() {      # bug 6086 - error adding new clients
	do_facet client mcreate $DIR/$tfile       || return 1
	drop_bl_callback_once "chmod 0777 $DIR/$tfile" ||
		echo "evicted as expected"
	#define OBD_FAIL_MDS_CLIENT_ADD 0x12f
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x8000012f"
	# fail once (evicted), reconnect fail (fail_loc), ok
	client_up || (sleep 10; client_up) || (sleep 10; client_up) || error "reconnect failed"
	rm -f $DIR/$tfile
	fail $SINGLEMDS		# verify MDS last_rcvd can be loaded
}
run_test 28 "handle error adding new clients (bug 6086)"

test_29a() { # bug 22273 - error adding new clients
	#define OBD_FAIL_TGT_CLIENT_ADD 0x711
	do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000711"
	# fail abort so client will be new again
	fail_abort $SINGLEMDS
	client_up || error "reconnect failed"
	wait_osc_import_state $SINGLEMDS ost FULL
	return 0
}
run_test 29a "error adding new clients doesn't cause LBUG (bug 22273)"

test_29b() { # bug 22273 - error adding new clients
	#define OBD_FAIL_TGT_CLIENT_ADD 0x711
	do_facet ost1 "lctl set_param fail_loc=0x80000711"
	# fail abort so client will be new again
	fail_abort ost1
	client_up || error "reconnect failed"
	return 0
}
run_test 29b "error adding new clients doesn't cause LBUG (bug 22273)"

test_50() {
	mkdir -p $DIR/$tdir
	# put a load of file creates/writes/deletes
	writemany -q $DIR/$tdir/$tfile 0 5 &
	CLIENT_PID=$!
	echo writemany pid $CLIENT_PID
	sleep 10
	FAILURE_MODE="SOFT"
	fail $SINGLEMDS
	# wait for client to reconnect to MDS
	sleep 60
	fail $SINGLEMDS
	sleep 60
	fail $SINGLEMDS
	# client process should see no problems even though MDS went down
	sleep $TIMEOUT
        kill -USR1 $CLIENT_PID
	wait $CLIENT_PID 
	rc=$?
	echo writemany returned $rc
	#these may fail because of eviction due to slow AST response.
	[ $rc -eq 0 ] ||
		error_ignore bz13652 "writemany returned rc $rc" || true
}
run_test 50 "failover MDS under load"

test_51() {
	#define OBD_FAIL_MDS_SYNC_CAPA_SL                    0x1310
	do_facet ost1 lctl set_param fail_loc=0x00001310

	mkdir -p $DIR/$tdir
	# put a load of file creates/writes/deletes
	writemany -q $DIR/$tdir/$tfile 0 5 &
	CLIENT_PID=$!
	sleep 1
	FAILURE_MODE="SOFT"
	facet_failover $SINGLEMDS
	# failover at various points during recovery
	SEQ="1 5 10 $(seq $TIMEOUT 5 $(($TIMEOUT+10)))"
        echo will failover at $SEQ
        for i in $SEQ
        do
		#echo failover in $i sec
		log "test_$testnum: failover in $i sec"
		sleep $i
		facet_failover $SINGLEMDS
        done
	# client process should see no problems even though MDS went down
	# and recovery was interrupted
	sleep $TIMEOUT
        kill -USR1 $CLIENT_PID
	wait $CLIENT_PID
	rc=$?
	echo writemany returned $rc
	[ $rc -eq 0 ] ||
		error_ignore bz13652 "writemany returned rc $rc" || true
}
run_test 51 "failover MDS during recovery"

test_52_guts() {
	do_facet client "mkdir -p $DIR/$tdir"
	do_facet client "writemany -q -a $DIR/$tdir/$tfile 300 5" &
	CLIENT_PID=$!
	echo writemany pid $CLIENT_PID
	sleep 10
	FAILURE_MODE="SOFT"
	fail ost1
	rc=0
	wait $CLIENT_PID || rc=$?
	# active client process should see an EIO for down OST
	[ $rc -eq 5 ] && { echo "writemany correctly failed $rc" && return 0; }
	# but timing or failover setup may allow success
	[ $rc -eq 0 ] && { echo "writemany succeeded" && return 0; }
	echo "writemany returned $rc"
	return $rc
}

test_52() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	mkdir -p $DIR/$tdir
	test_52_guts
	rc=$?
	[ $rc -ne 0 ] && { return $rc; }
	# wait for client to reconnect to OST
	sleep 30
	test_52_guts
	rc=$?
	[ $rc -ne 0 ] && { return $rc; }
	sleep 30
	test_52_guts
	rc=$?
	client_reconnect
	#return $rc
}
run_test 52 "failover OST under load"

# test of open reconstruct
test_53() {
	touch $DIR/$tfile
	drop_ldlm_reply "openfile -f O_RDWR:O_CREAT -m 0755 $DIR/$tfile" ||\
		return 2
}
run_test 53 "touch: drop rep"

test_54() {
	zconf_mount `hostname` $MOUNT2
        touch $DIR/$tfile
        touch $DIR2/$tfile.1
        sleep 10
        cat $DIR2/$tfile.missing # save transno = 0, rc != 0 into last_rcvd
        fail $SINGLEMDS
        umount $MOUNT2
        ERROR=`dmesg | egrep "(test 54|went back in time)" | tail -n1 | grep "went back in time"`
        [ x"$ERROR" == x ] || error "back in time occured"
}
run_test 54 "back in time"

# bug 11330 - liblustre application death during I/O locks up OST
test_55() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	mkdir -p $DIR/$tdir

	# Minimum pass speed is 2MBps
	local ddtimeout=64
	# LU-2887/LU-3089 - set min pass speed to 500KBps
	[ "$(facet_fstype ost1)" = "zfs" ] && ddtimeout=256

	# first dd should be finished quickly
	$LFS setstripe -c 1 -i 0 $DIR/$tdir/$tfile-1
	dd if=/dev/zero of=$DIR/$tdir/$tfile-1 bs=32M count=4 &
	DDPID=$!
	count=0
	echo  "step1: testing ......"
	while kill -0 $DDPID 2> /dev/null; do
		let count++
		if [ $count -gt $ddtimeout ]; then
			error "dd should be finished!"
		fi
		sleep 1
	done
	echo "(dd_pid=$DDPID, time=$count)successful"

	$LFS setstripe -c 1 -i 0 $DIR/$tdir/$tfile-2
	#define OBD_FAIL_OST_DROP_REQ            0x21d
	do_facet ost1 lctl set_param fail_loc=0x0000021d
	# second dd will be never finished
	dd if=/dev/zero of=$DIR/$tdir/$tfile-2 bs=32M count=4 &
	DDPID=$!
	count=0
	echo  "step2: testing ......"
	while [ $count -le $ddtimeout ]; do
		if ! kill -0 $DDPID 2> /dev/null; then
			ls -l $DIR/$tdir
			error "dd shouldn't be finished! (time=$count)"
		fi
		let count++
		sleep 1
	done
	echo "(dd_pid=$DDPID, time=$count)successful"

	#Recover fail_loc and dd will finish soon
	do_facet ost1 lctl set_param fail_loc=0
	count=0
	echo  "step3: testing ......"
	while kill -0 $DDPID 2> /dev/null; do
		let count++
		if [ $count -gt $((ddtimeout + 440)) ]; then
			error "dd should be finished!"
		fi
		sleep 1
	done
	echo "(dd_pid=$DDPID, time=$count)successful"

	rm -rf $DIR/$tdir
}
run_test 55 "ost_brw_read/write drops timed-out read/write request"

test_56() { # b=11277
#define OBD_FAIL_MDS_RESEND      0x136
        touch $DIR/$tfile
        do_facet $SINGLEMDS "lctl set_param fail_loc=0x80000136"
        stat $DIR/$tfile || error "stat failed"
        do_facet $SINGLEMDS "lctl set_param fail_loc=0"
        rm -f $DIR/$tfile
}
run_test 56 "do not fail on getattr resend"

test_57_helper() {
        # no oscs means no client or mdt 
        while lctl get_param osc.*.* > /dev/null 2>&1; do
                : # loop until proc file is removed
        done
}

test_57() { # bug 10866
        test_57_helper &
        pid=$!
        sleep 1
#define OBD_FAIL_LPROC_REMOVE            0xB00
        lctl set_param fail_loc=0x80000B00
        zconf_umount `hostname` $DIR
        lctl set_param fail_loc=0x80000B00
        fail_abort $SINGLEMDS
        kill -9 $pid
        lctl set_param fail_loc=0
        mount_client $DIR
        do_facet client "df $DIR"
}
run_test 57 "read procfs entries causes kernel crash"

test_58() { # bug 11546
#define OBD_FAIL_MDC_ENQUEUE_PAUSE        0x801
        touch $DIR/$tfile
        ls -la $DIR/$tfile
        lctl set_param fail_loc=0x80000801
        cp $DIR/$tfile /dev/null &
        pid=$!
        sleep 1
        lctl set_param fail_loc=0
        drop_bl_callback_once rm -f $DIR/$tfile
        wait $pid
        # the first 'df' could tigger the eviction caused by
        # 'drop_bl_callback_once', and it's normal case.
        # but the next 'df' should return successfully.
        do_facet client "df $DIR" || do_facet client "df $DIR"
}
run_test 58 "Eviction in the middle of open RPC reply processing"

test_59() { # bug 10589
	zconf_mount `hostname` $MOUNT2 || error "Failed to mount $MOUNT2"
	echo $DIR2 | grep -q $MOUNT2 || error "DIR2 is not set properly: $DIR2"
#define OBD_FAIL_LDLM_CANCEL_EVICT_RACE  0x311
	lctl set_param fail_loc=0x311
	writes=$(LANG=C dd if=/dev/zero of=$DIR2/$tfile count=1 2>&1)
	[ $? = 0 ] || error "dd write failed"
	writes=$(echo $writes | awk  -F '+' '/out/ {print $1}')
	lctl set_param fail_loc=0
	sync
	zconf_umount `hostname` $MOUNT2 -f
	reads=$(LANG=C dd if=$DIR/$tfile of=/dev/null 2>&1)
	[ $? = 0 ] || error "dd read failed"
	reads=$(echo $reads | awk -F '+' '/in/ {print $1}')
	[ "$reads" -eq "$writes" ] || error "read" $reads "blocks, must be" $writes
}
run_test 59 "Read cancel race on client eviction"

err17935 () {
	# we assume that all md changes are in the MDT0 changelog
	if [ $MDSCOUNT -gt 1 ]; then
		error_ignore bz17935 $*
	else
		error $*
	fi
}

test_60() {
	MDT0=$($LCTL get_param -n mdc.*.mds_server_uuid |
		awk '{ gsub(/_UUID/,""); print $1 }' | head -n1)

	NUM_FILES=15000
	mkdir -p $DIR/$tdir

	# Register (and start) changelog
	USER=$(do_facet $SINGLEMDS lctl --device $MDT0 changelog_register -n)
	echo "Registered as $MDT0 changelog user $USER"

	# Generate a large number of changelog entries
	createmany -o $DIR/$tdir/$tfile $NUM_FILES
	sync
	sleep 5

	# Unlink files in the background
	unlinkmany $DIR/$tdir/$tfile $NUM_FILES	&
	CLIENT_PID=$!
	sleep 1

	# Failover the MDS while unlinks are happening
	facet_failover $SINGLEMDS

	# Wait for unlinkmany to finish
	wait $CLIENT_PID

	# Check if all the create/unlink events were recorded
	# in the changelog
	$LFS changelog $MDT0 >> $DIR/$tdir/changelog
	local cl_count=$(grep UNLNK $DIR/$tdir/changelog | wc -l)
	echo "$cl_count unlinks in $MDT0 changelog"

	do_facet $SINGLEMDS lctl --device $MDT0 changelog_deregister $USER
	USERS=$(( $(do_facet $SINGLEMDS lctl get_param -n \
	    mdd.$MDT0.changelog_users | wc -l) - 2 ))
	if [ $USERS -eq 0 ]; then
	    [ $cl_count -eq $NUM_FILES ] || \
		err17935 "Recorded ${cl_count} unlinks out of $NUM_FILES"
	    # Also make sure we can clear large changelogs
	    cl_count=$($LFS changelog $FSNAME | wc -l)
	    [ $cl_count -le 2 ] || \
		error "Changelog not empty: $cl_count entries"
	else
	    # If there are other users, there may be other unlinks in the log
	    [ $cl_count -ge $NUM_FILES ] || \
		err17935 "Recorded ${cl_count} unlinks out of $NUM_FILES"
	    echo "$USERS other changelog users; can't verify clear"
	fi
}
run_test 60 "Add Changelog entries during MDS failover"

test_61()
{
	local mdtosc=$(get_mdtosc_proc_path $SINGLEMDS $FSNAME-OST0000)
	mdtosc=${mdtosc/-MDT*/-MDT\*}
	local cflags="osc.$mdtosc.connect_flags"
	do_facet $SINGLEMDS "lctl get_param -n $cflags" |grep -q skip_orphan
	[ $? -ne 0 ] && skip "don't have skip orphan feature" && return

	mkdir -p $DIR/$tdir || error "mkdir dir $DIR/$tdir failed"
	# Set the default stripe of $DIR/$tdir to put the files to ost1
	$LFS setstripe -c 1 -i 0 $DIR/$tdir

	replay_barrier $SINGLEMDS
	createmany -o $DIR/$tdir/$tfile-%d 10 
	local oid=$(do_facet ost1 "lctl get_param -n \
		obdfilter.${ost1_svc}.last_id" | sed -e 's/.*://')

	fail_abort $SINGLEMDS

	touch $DIR/$tdir/$tfile
	local id=$($LFS getstripe $DIR/$tdir/$tfile |
		awk '$1 == 0 { print $2 }')
	[ $id -le $oid ] && error "the orphan objid was reused, failed"

	# Cleanup
	rm -rf $DIR/$tdir
}
run_test 61 "Verify to not reuse orphan objects - bug 17025"

# test_62 as seen it b2_1 please do not reuse test_62
#test_62()
#{
#	zconf_umount `hostname` $DIR
#	#define OBD_FAIL_PTLRPC_DELAY_IMP_FULL   0x516
#	lctl set_param fail_loc=0x516
#	mount_client $DIR
#}
#run_test 62 "Verify connection flags race - bug LU-1716"

test_66()
{
	[[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.7.51) ]] ||
		{ skip "Need MDS version at least 2.7.51"; return 0; }

	local list=$(comma_list $(osts_nodes))

	# modify dir so that next revalidate would not obtain UPDATE lock
	touch $DIR

	# drop 1 reply with UPDATE lock
	mcreate $DIR/$tfile || error "mcreate failed: $?"
	drop_ldlm_reply_once "stat $DIR/$tfile" &
	sleep 2

	# make the re-sent lock to sleep
#define OBD_FAIL_MDS_RESEND              0x136
	do_nodes $list $LCTL set_param fail_loc=0x80000136

	#initiate the re-connect & re-send
	local mdccli=$($LCTL dl | awk '/-mdc-/ {print $4;}')
	local conn_uuid=$($LCTL get_param -n mdc.${mdccli}.mds_conn_uuid)
	$LCTL set_param "mdc.${mdccli}.import=connection=${conn_uuid}"
	sleep 2

	#initiate the client eviction while enqueue re-send is in progress
	mds_evict_client

	client_reconnect
	wait
}
run_test 66 "lock enqueue re-send vs client eviction"

test_65() {
	mount_client $DIR2

	#grant lock1, export2
	$SETSTRIPE -i -0 $DIR2/$tfile || return 1
	$MULTIOP $DIR2/$tfile Ow  || return 2

#define OBD_FAIL_LDLM_BL_EVICT            0x31e
	do_facet ost $LCTL set_param fail_loc=0x31e
	#get waiting lock2, export1
	$MULTIOP $DIR/$tfile Ow &
	PID1=$!
	# let enqueue to get asleep
	sleep 2

	#get lock2 blocked
	$MULTIOP $DIR2/$tfile Ow &
	PID2=$!
	sleep 2

	#evict export1
	ost_evict_client

	sleep 2
	do_facet ost $LCTL set_param fail_loc=0

	wait $PID1
	wait $PID2

	umount_client $DIR2
}
run_test 65 "lock enqueue for destroyed export"

check_cli_ir_state()
{
        local NODE=${1:-$HOSTNAME}
        local st
        st=$(do_node $NODE "lctl get_param mgc.*.ir_state |
                            awk '/imperative_recovery:/ { print \\\$2}'")
	[ $st != ON -o $st != OFF -o $st != ENABLED -o $st != DISABLED ] ||
		error "Error state $st, must be ENABLED or DISABLED"
        echo -n $st
}

check_target_ir_state()
{
        local target=${1}
        local name=${target}_svc
        local recovery_proc=obdfilter.${!name}.recovery_status
        local st

        st=$(do_facet $target "lctl get_param -n $recovery_proc |
                               awk '/IR:/{ print \\\$2}'")
	[ $st != ON -o $st != OFF -o $st != ENABLED -o $st != DISABLED ] ||
		error "Error state $st, must be ENABLED or DISABLED"
        echo -n $st
}

set_ir_status()
{
        do_facet mgs lctl set_param -n mgs.MGS.live.$FSNAME="state=$1"
}

get_ir_status()
{
        local state=$(do_facet mgs "lctl get_param -n mgs.MGS.live.$FSNAME |
                                    awk '/state:/{ print \\\$2 }'")
        echo -n ${state/,/}
}

nidtbl_version_mgs()
{
        local ver=$(do_facet mgs "lctl get_param -n mgs.MGS.live.$FSNAME |
                                  awk '/nidtbl_version:/{ print \\\$2 }'")
        echo -n $ver
}

# nidtbl_version_client <mds1|client> [node]
nidtbl_version_client()
{
        local cli=$1
        local node=${2:-$HOSTNAME}

        if [ X$cli = Xclient ]; then
                cli=$FSNAME-client
        else
                local obdtype=${cli/%[0-9]*/}
                [ $obdtype != mds ] && error "wrong parameters $cli"

                node=$(facet_active_host $cli)
                local t=${cli}_svc
                cli=${!t}
        fi

        local vers=$(do_node $node "lctl get_param -n mgc.*.ir_state" |
                     awk "/$cli/{print \$6}" |sort -u)

        # in case there are multiple mounts on the client node
        local arr=($vers)
        [ ${#arr[@]} -ne 1 ] && error "versions on client node mismatch"
        echo -n $vers
}

nidtbl_versions_match()
{
        [ $(nidtbl_version_mgs) -eq $(nidtbl_version_client ${1:-client}) ]
}

target_instance_match()
{
        local srv=$1
        local obdtype
        local cliname

        obdtype=${srv/%[0-9]*/}
        case $obdtype in
        mds)
                obdname="mdt"
                cliname="mdc"
                ;;
        ost)
                obdname="obdfilter"
                cliname="osc"
                ;;
        *)
                error "invalid target type" $srv
                return 1
                ;;
        esac

        local target=${srv}_svc
        local si=$(do_facet $srv lctl get_param -n $obdname.${!target}.instance)
	local ci=$(lctl get_param -n $cliname.${!target}-${cliname}-*.import |
		awk '/instance/{ print $2 }' | head -n1)

        return $([ $si -eq $ci ])
}

test_100()
{
	do_facet mgs $LCTL list_param mgs.*.ir_timeout ||
		{ skip "MGS without IR support"; return 0; }

	# MDT was just restarted in the previous test, make sure everything
	# is all set.
	local cnt=30
	while [ $cnt -gt 0 ]; do
		nidtbl_versions_match && break
		sleep 1
		cnt=$((cnt - 1))
	done

	# disable IR
	set_ir_status disabled

	local prev_ver=$(nidtbl_version_client client)

        local saved_FAILURE_MODE=$FAILURE_MODE
        [ $(facet_host mgs) = $(facet_host ost1) ] && FAILURE_MODE="SOFT"
        fail ost1

        # valid check
	[ $(nidtbl_version_client client) -eq $prev_ver ] ||
		error "version must not change due to IR disabled"
        target_instance_match ost1 || error "instance mismatch"

        # restore env
        set_ir_status full
        FAILURE_MODE=$saved_FAILURE_MODE
}
run_test 100 "IR: Make sure normal recovery still works w/o IR"

test_101()
{
        do_facet mgs $LCTL list_param mgs.*.ir_timeout ||
                { skip "MGS without IR support"; return 0; }

        set_ir_status full

        local OST1_IMP=$(get_osc_import_name client ost1)

        # disable pinger recovery
        lctl set_param -n osc.$OST1_IMP.pinger_recov=0

        fail ost1

        target_instance_match ost1 || error "instance mismatch"
        nidtbl_versions_match || error "version must match"

        lctl set_param -n osc.$OST1_IMP.pinger_recov=1
}
run_test 101 "IR: Make sure IR works w/o normal recovery"

test_102()
{
        do_facet mgs $LCTL list_param mgs.*.ir_timeout ||
                { skip "MGS without IR support"; return 0; }

        local clients=${CLIENTS:-$HOSTNAME}
        local old_version
        local new_version
        local mgsdev=mgs

        set_ir_status full

        # let's have a new nidtbl version
        fail ost1

        # sleep for a while so that clients can see the failure of ost
        # it must be MGC_TIMEOUT_MIN_SECONDS + MGC_TIMEOUT_RAND_CENTISEC.
        # int mgc_request.c:
        # define MGC_TIMEOUT_MIN_SECONDS   5
        # define MGC_TIMEOUT_RAND_CENTISEC 0x1ff /* ~500 *
        local count=30  # 20 seconds at most
        while [ $count -gt 0 ]; do
                nidtbl_versions_match && break
                sleep 1
                count=$((count-1))
        done

        nidtbl_versions_match || error "nidtbl mismatch"

        # get the version #
        old_version=$(nidtbl_version_client client)

        zconf_umount_clients $clients $MOUNT || error "Cannot umount client"

        # restart mgs
        combined_mgs_mds && mgsdev=mds1
        remount_facet $mgsdev
        fail ost1

        zconf_mount_clients $clients $MOUNT || error "Cannot mount client"

        # check new version
        new_version=$(nidtbl_version_client client)
        [ $new_version -lt $old_version ] &&
                error "nidtbl version wrong after mgs restarts"
        return 0
}
run_test 102 "IR: New client gets updated nidtbl after MGS restart"

test_103()
{
        do_facet mgs $LCTL list_param mgs.*.ir_timeout ||
                { skip "MGS without IR support"; return 0; }

        combined_mgs_mds && skip "mgs and mds on the same target" && return 0

        # workaround solution to generate config log on the mds
        remount_facet mds1

        stop mgs
        stop mds1

        # We need this test because mds is like a client in IR context.
        start mds1 $(mdsdevname 1) $MDS_MOUNT_OPTS ||
		error "MDS should start w/o mgs"

        # start mgs and remount mds w/ ir
        start mgs $(mgsdevname) $MGS_MOUNT_OPTS
        clients_up

        # remount client so that fsdb will be created on the MGS
        umount_client $MOUNT || error "umount failed"
        mount_client $MOUNT || error "mount failed"

        # sleep 30 seconds so the MDS has a chance to detect MGS restarting
        local count=30
        while [ $count -gt 0 ]; do
                [ $(nidtbl_version_client mds1) -ne 0 ] && break
                sleep 1
                count=$((count-1))
        done

        # after a while, mds should be able to reconnect to mgs and fetch
        # up-to-date nidtbl version
        nidtbl_versions_match mds1 || error "mds nidtbl mismatch"

        # reset everything
        set_ir_status full
}
run_test 103 "IR: MDS can start w/o MGS and get updated nidtbl later"

test_104()
{
        do_facet mgs $LCTL list_param mgs.*.ir_timeout ||
                { skip "MGS without IR support"; return 0; }

        set_ir_status full

        stop ost1
        start ost1 $(ostdevname 1) "$OST_MOUNT_OPTS -onoir" ||
                error "OST1 cannot start"
        clients_up

        local ir_state=$(check_target_ir_state ost1)
	[ $ir_state = "DISABLED" -o $ir_state = "OFF" ] ||
		error "ir status on ost1 should be DISABLED"
}
run_test 104 "IR: ost can disable IR voluntarily"

test_105()
{
        [ -z "$RCLIENTS" ] && skip "Needs multiple clients" && return 0
        do_facet mgs $LCTL list_param mgs.*.ir_timeout ||
                { skip "MGS without IR support"; return 0; }

        set_ir_status full

        # get one of the clients from client list
        local rcli=$(echo $RCLIENTS |cut -d' ' -f 1)

        local mount_opts=${MOUNT_OPTS:+$MOUNT_OPTS,}noir
        zconf_umount $rcli $MOUNT || error "umount failed"
        zconf_mount $rcli $MOUNT $mount_opts || error "mount failed"

        # make sure lustre mount at $rcli disabling IR
        local ir_state=$(check_cli_ir_state $rcli)
	[ $ir_state = "DISABLED" -o $ir_state = "OFF" ] ||
		error "IR state must be DISABLED at $rcli"

	# Since the client just mounted, its last_rcvd entry is not on disk.
	# Send an RPC so exp_need_sync forces last_rcvd to commit this export
	# so the client can reconnect during OST recovery (LU-924, LU-1582)
	$SETSTRIPE -i 0 $DIR/$tfile
	dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 conv=sync

        # make sure MGS's state is Partial
        [ $(get_ir_status) = "partial" ] || error "MGS IR state must be partial"

        fail ost1
	# make sure IR on ost1 is DISABLED
        local ir_state=$(check_target_ir_state ost1)
	[ $ir_state = "DISABLED" -o $ir_state = "OFF" ] ||
		error "IR status on ost1 should be DISABLED"

        # remount with the default MOUNT_OPTS
        zconf_umount $rcli $MOUNT || error "umount failed"
        zconf_mount $rcli $MOUNT || error "mount failed"

        # make sure MGS's state is full
        [ $(get_ir_status) = "full" ] || error "MGS IR status must be full"

        fail ost1
	# make sure IR on ost1 is ENABLED
        local ir_state=$(check_target_ir_state ost1)
	[ $ir_state = "ENABLED" -o $ir_state = "ON" ] ||
		error "IR status on ost1 should be ENABLED"

        return 0
}
run_test 105 "IR: NON IR clients support"

cleanup_106() {
	trap 0
	umount_client $DIR2
	debugrestore
}

test_106() { # LU-1789
	[[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.3.50) ]] ||
		{ skip "Need MDS version at least 2.3.50"; return 0; }

#define OBD_FAIL_MDC_LIGHTWEIGHT         0x805
	$LCTL set_param fail_loc=0x805

	debugsave
	trap cleanup_106 EXIT

	# enable lightweight flag on mdc connection
	mount_client $DIR2

	local MDS_NEXP=$(do_facet $SINGLEMDS \
			 lctl get_param -n mdt.${mds1_svc}.num_exports |
			 cut -d' ' -f2)
	$LCTL set_param fail_loc=0

	touch $DIR2/$tfile || error "failed to create empty file"
	replay_barrier $SINGLEMDS

	$LCTL set_param debug=console
	$LCTL clear
	facet_failover $SINGLEMDS

	# lightweight connection must be evicted
	touch -c $DIR2/$tfile || true
	$LCTL dk $TMP/lustre-log-$TESTNAME.log
	evicted=`awk '/This client was evicted by .*MDT0000/ {
				      print;
		      }' $TMP/lustre-log-$TESTNAME.log`
	[ -z "$evicted" ] && error "lightweight client not evicted by mds"

	# and all operations performed by lightweight client should be
	# synchronous, so the file created before mds restart should be there
	$CHECKSTAT -t file $DIR/$tfile || error "file not present"
	rm -f $DIR/$tfile

	cleanup_106
}
run_test 106 "lightweight connection support"

test_107 () {
	local CLIENT_PID
	local close_pid

	mkdir -p $DIR/$tdir
	# OBD_FAIL_MDS_REINT_NET_REP   0x119
	do_facet $SINGLEMDS lctl set_param fail_loc=0x119
	multiop $DIR/$tdir D_c &
	close_pid=$!
	mkdir $DIR/$tdir/dir_106 &
	CLIENT_PID=$!
	do_facet $SINGLEMDS lctl set_param fail_loc=0
	fail $SINGLEMDS

	wait $CLIENT_PID || rc=$?
	checkstat -t dir $DIR/$tdir/dir_106 || return 1

	kill -USR1 $close_pid
	wait $close_pid || return 2

	return $rc
}
run_test 107 "drop reint reply, then restart MDT"

test_108() {
	mkdir -p $DIR/$tdir
	$SETSTRIPE -c 1 -i 0 $DIR/$tdir

	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=1M count=256 &
	local dd_pid=$!
	sleep 0.1

	ost_evict_client

	wait $dd_pid

	client_up || error "reconnect failed"
	rm -f $DIR/$tdir/$tfile
}
run_test 108 "client eviction don't crash"

test_110a () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local remote_dir=$DIR/$tdir/remote_dir
	local MDTIDX=1
	local num

	#prepare for 110 test, which need set striped dir on remote MDT.
	for num in $(seq $MDSCOUNT); do
		do_facet mds$num \
			lctl set_param -n mdt.${FSNAME}*.enable_remote_dir=1 \
				2>/dev/null
	done

	mkdir -p $DIR/$tdir
	drop_request "$LFS mkdir -i $MDTIDX -c2 $remote_dir" ||
					error "lfs mkdir failed"
	local diridx=$($GETSTRIPE -M $remote_dir)
	[ $diridx -eq $MDTIDX ] || error "$diridx != $MDTIDX"

	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 110a "create remote directory: drop client req"

test_110b () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local remote_dir=$DIR/$tdir/remote_dir
	local MDTIDX=1

	mkdir -p $DIR/$tdir
	drop_reint_reply "$LFS mkdir -i $MDTIDX -c2 $remote_dir" ||
					error "lfs mkdir failed"

	diridx=$($GETSTRIPE -M $remote_dir)
	[ $diridx -eq $MDTIDX ] || error "$diridx != $MDTIDX"

	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 110b "create remote directory: drop Master rep"

test_110c () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local remote_dir=$DIR/$tdir/remote_dir
	local MDTIDX=1

	mkdir -p $DIR/$tdir
	drop_update_reply $MDTIDX "$LFS mkdir -i $MDTIDX -c2 $remote_dir" ||
						error "lfs mkdir failed"

	diridx=$($GETSTRIPE -M $remote_dir)
	[ $diridx -eq $MDTIDX ] || error "$diridx != $MDTIDX"

	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 110c "create remote directory: drop update rep on slave MDT"

test_110d () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local remote_dir=$DIR/$tdir/remote_dir
	local MDTIDX=1

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX -c2 $remote_dir || error "lfs mkdir failed"

	drop_request "rm -rf $remote_dir" || error "rm remote dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110d "remove remote directory: drop client req"

test_110e () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local remote_dir=$DIR/$tdir/remote_dir
	local MDTIDX=1

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX -c2 $remote_dir  || error "lfs mkdir failed"
	drop_reint_reply "rm -rf $remote_dir" || error "rm remote dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"

	return 0
}
run_test 110e "remove remote directory: drop master rep"

test_110f () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local remote_dir=$DIR/$tdir/remote_dir
	local MDTIDX=1

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX -c2 $remote_dir || error "lfs mkdir failed"
	drop_update_reply $MDTIDX "rm -rf $remote_dir" ||
					error "rm remote dir failed"

	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 110f "remove remote directory: drop slave rep"

test_110g () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local remote_dir=$DIR/$tdir/remote_dir
	local MDTIDX=1

	mkdir -p $remote_dir

	createmany -o $remote_dir/f 100

	#define OBD_FAIL_MIGRATE_NET_REP		0x1800
	do_facet mds$MDTIDX lctl set_param fail_loc=0x1800
	$LFS migrate -m $MDTIDX $remote_dir || error "migrate failed"
	do_facet mds$MDTIDX lctl set_param fail_loc=0x0

	for file in $(find $remote_dir); do
		mdt_index=$($LFS getstripe -M $file)
		[ $mdt_index == $MDTIDX ] ||
			error "$file is not on MDT${MDTIDX}"
	done

	rm -rf $DIR/$tdir || error "rmdir failed"
}
run_test 110g "drop reply during migration"

test_110h () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local src_dir=$DIR/$tdir/source_dir
	local tgt_dir=$DIR/$tdir/target_dir
	local MDTIDX=1

	mkdir -p $src_dir
	$LFS mkdir -i $MDTIDX $tgt_dir

	dd if=/etc/hosts of=$src_dir/src_file
	touch $tgt_dir/tgt_file
	drop_update_reply $MDTIDX \
		"mrename $src_dir/src_file $tgt_dir/tgt_file" ||
		error "mrename failed"

	$CHECKSTAT -t file $src_dir/src_file &&
				error "src_file present after rename"

	diff /etc/hosts $tgt_dir/tgt_file ||
			error "file changed after rename"

}
run_test 110h "drop update reply during cross-MDT file rename"

test_110i () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local src_dir=$DIR/$tdir/source_dir
	local tgt_dir=$DIR/$tdir/target_dir
	local MDTIDX=1

	mkdir -p $src_dir
	$LFS mkdir -i $MDTIDX $tgt_dir

	mkdir $src_dir/src_dir
	touch $src_dir/src_dir/a
	mkdir $tgt_dir/tgt_dir
	drop_update_reply $MDTIDX \
		"mrename $src_dir/src_dir $tgt_dir/tgt_dir" ||
		error "mrename failed"

	$CHECKSTAT -t dir $src_dir/src_dir &&
			error "src_dir present after rename"

	$CHECKSTAT -t dir $tgt_dir/tgt_dir ||
				error "tgt_dir not present after rename"

	$CHECKSTAT -t file $tgt_dir/tgt_dir/a ||
				error "a not present after rename"
}
run_test 110i "drop update reply during cross-MDT dir rename"

test_110j () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0
	local remote_dir=$DIR/$tdir/remote_dir
	local local_dir=$DIR/$tdir/local_dir
	local MDTIDX=1

	mkdir -p $DIR/$tdir
	mkdir $DIR/$tdir/local_dir
	$LFS mkdir -i $MDTIDX $remote_dir

	touch $local_dir/local_file
	drop_update_reply $MDTIDX \
		"ln $local_dir/local_file $remote_dir/remote_file" ||
		error "ln failed"

	$CHECKSTAT -t file $remote_dir/remote_file ||
				error "remote not present after ln"
}
run_test 110j "drop update reply during cross-MDT ln"

# LU-2844 mdt prepare fail should not cause umount oops
test_111 ()
{
	[[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.3.62) ]] ||
		{ skip "Need MDS version at least 2.3.62"; return 0; }

	local mdsdev=$(mdsdevname ${SINGLEMDS//mds/})
#define OBD_FAIL_MDS_CHANGELOG_INIT 0x151
	do_facet $SINGLEMDS lctl set_param fail_loc=0x151
	stop $SINGLEMDS || error "stop MDS failed"
	start $SINGLEMDS $mdsdev && error "start MDS should fail"
	do_facet $SINGLEMDS lctl set_param fail_loc=0
	start $SINGLEMDS $mdsdev || error "start MDS failed"
}
run_test 111 "mdd setup fail should not cause umount oops"

# LU-793
test_112a() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return 0

	do_facet_random_file client $TMP/$tfile 100K ||
		error_noexit "Create random file $TMP/$tfile"

	pause_bulk "cp $TMP/$tfile $DIR/$tfile" $TIMEOUT ||
		error_noexit "Can't pause_bulk copy"

	df $DIR
	# expect cmp to succeed, client resent bulk
	cmp $TMP/$tfile $DIR/$tfile ||
		error_noexit "Wrong data has been written"
	rm $DIR/$tfile ||
		error_noexit "Can't remove file"
	rm $TMP/$tfile
}
run_test 112a "bulk resend while orignal request is in progress"

test_115_read() {
	local fail1=$1
	local fail2=$2

	df $DIR
	dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1
	cancel_lru_locks osc

	# OST_READ       =  3,
	$LCTL set_param fail_loc=$fail1 fail_val=3
	dd of=/dev/null if=$DIR/$tfile bs=4096 count=1 &
	pid=$!
	sleep 1

	set_nodes_failloc "$(osts_nodes)" $fail2

	wait $pid || error "dd failed"
	return 0
}

test_115_write() {
	local fail1=$1
	local fail2=$2
	local error=$3
	local fail_val2=${4:-0}

	df $DIR
	touch $DIR/$tfile

	# OST_WRITE      =  4,
	$LCTL set_param fail_loc=$fail1 fail_val=4
	dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1 oflag=dsync &
	pid=$!
	sleep 1

	df $MOUNT
	set_nodes_failloc "$(osts_nodes)" $fail2 $fail_val2

	wait $pid
	rc=$?
	[ $error -eq 0 ] && [ $rc -ne 0 ] && error "dd error ($rc)"
	[ $error -ne 0 ] && [ $rc -eq 0 ] && error "dd success"
	return 0
}

test_115a() {
	[ $(lustre_version_code ost1) -lt $(version_code 2.8.50) ] &&
		skip "need at least 2.8.50 on OST" && return 0

	#define OBD_FAIL_PTLRPC_LONG_REQ_UNLINK  0x51b
	#define OBD_FAIL_PTLRPC_DROP_BULK	 0x51a
	test_115_read 0x8000051b 0x8000051a
}
run_test 115a "read: late REQ MDunlink and no bulk"

test_115b() {
	[ $(lustre_version_code ost1) -lt $(version_code 2.8.50) ] &&
		skip "need at least 2.8.50 on OST" && return 0

	#define OBD_FAIL_PTLRPC_LONG_REQ_UNLINK  0x51b
	#define OBD_FAIL_OST_ENOSPC              0x215

	# pass $OSTCOUNT for the fail_loc to be caught
	# appropriately by the IO thread
	test_115_write 0x8000051b 0x80000215 1 $OSTCOUNT
}
run_test 115b "write: late REQ MDunlink and no bulk"

test_115c() {
	[ $(lustre_version_code ost1) -lt $(version_code 2.8.50) ] &&
		skip "need at least 2.8.50 on OST" && return 0

	#define OBD_FAIL_PTLRPC_LONG_REPL_UNLINK 0x50f
	#define OBD_FAIL_PTLRPC_DROP_BULK	 0x51a
	test_115_read 0x8000050f 0x8000051a
}
run_test 115c "read: late Reply MDunlink and no bulk"

test_115d() {
	[ $(lustre_version_code ost1) -lt $(version_code 2.8.50) ] &&
		skip "need at least 2.8.50 on OST" && return 0

	#define OBD_FAIL_PTLRPC_LONG_REPL_UNLINK 0x50f
	#define OBD_FAIL_OST_ENOSPC              0x215
	test_115_write 0x8000050f 0x80000215 0
}
run_test 115d "write: late Reply MDunlink and no bulk"

test_115e() {
	[ $(lustre_version_code ost1) -lt $(version_code 2.8.50) ] &&
		skip "need at least 2.8.50 on OST" && return 0

	#define OBD_FAIL_PTLRPC_LONG_BULK_UNLINK 0x510
	#define OBD_FAIL_OST_ALL_REPLY_NET       0x211
	test_115_read 0x80000510 0x80000211
}
run_test 115e "read: late Bulk MDunlink and no reply"

test_115f() {
	[ $(lustre_version_code ost1) -lt $(version_code 2.8.50) ] &&
		skip "need at least 2.8.50 on OST" && return 0

	#define OBD_FAIL_PTLRPC_LONG_REQ_UNLINK  0x51b
	#define OBD_FAIL_OST_ALL_REPLY_NET       0x211
	test_115_read 0x8000051b 0x80000211
}
run_test 115f "read: late REQ MDunlink and no reply"

test_115g() {
	[ $(lustre_version_code ost1) -lt $(version_code 2.8.50) ] &&
		skip "need at least 2.8.50 on OST" && return 0

	#define OBD_FAIL_PTLRPC_LONG_BOTH_UNLINK 0x51c
	test_115_read 0x8000051c 0
}
run_test 115g "read: late REQ MDunlink and Reply MDunlink"

# parameters: fail_loc CMD RC
test_120_reply() {
	local PID
	local PID2
	local rc=5
	local fail

	#define OBD_FAIL_LDLM_CP_CB_WAIT2	0x320
	#define OBD_FAIL_LDLM_CP_CB_WAIT3	0x321
	#define OBD_FAIL_LDLM_CP_CB_WAIT4	0x322
	#define OBD_FAIL_LDLM_CP_CB_WAIT5	0x323

	echo
	echo -n "** FLOCK REPLY vs. EVICTION race, lock $2"
	[ "$1" = "CLEANUP" ] &&
		fail=0x80000320 && echo ", $1 cp first"
	[ "$1" = "REPLY" ] &&
		fail=0x80000321 && echo ", $1 cp first"
	[ "$1" = "DEADLOCK CLEANUP" ] &&
		fail=0x80000322 && echo " DEADLOCK, CLEANUP cp first"
	[ "$1" = "DEADLOCK REPLY" ] &&
		fail=0x80000323 && echo " DEADLOCK, REPLY cp first"

	if [ x"$2" = x"get" ]; then
		#for TEST lock, take a conflict in advance
		# sleep longer than evictor to not confuse fail_loc: 2+2+4
		echo "** Taking conflict **"
		flocks_test 5 set read sleep 10 $DIR/$tfile &
		PID2=$!

		sleep 2
	fi

	$LCTL set_param fail_loc=$fail

	flocks_test 5 $2 write $DIR/$tfile &
	PID=$!

	sleep 2
	echo "** Evicting and re-connecting client **"
	mds_evict_client

	client_reconnect

	if [ x"$2" = x"get" ]; then
		wait $PID2
	fi

	wait $PID
	rc=$?

	# check if the return value is allowed
	[ $rc -eq $3 ] && rc=0

	$LCTL set_param fail_loc=0
	return $rc
}

# a lock is taken, unlock vs. cleanup_resource() race for destroying
# the ORIGINAL lock.
test_120_destroy()
{
	local PID

	flocks_test 5 set write sleep 4 $DIR/$tfile &
	PID=$!
	sleep 2

	# let unlock to sleep in CP CB
	$LCTL set_param fail_loc=$1
	sleep 4

	# let cleanup to cleep in CP CB
	mds_evict_client

	client_reconnect

	wait $PID
	rc=$?

	$LCTL set_param fail_loc=0
	return $rc
}

test_120() {
	flock_is_enabled || { skip "mount w/o flock enabled" && return; }
	touch $DIR/$tfile

	test_120_reply "CLEANUP" set 5 || error "SET race failed"
	test_120_reply "CLEANUP" get 5 || error "GET race failed"
	test_120_reply "CLEANUP" unlock 5 || error "UNLOCK race failed"

	test_120_reply "REPLY" set 5 || error "SET race failed"
	test_120_reply "REPLY" get 5 || error "GET race failed"
	test_120_reply "REPLY" unlock 5 || error "UNLOCK race failed"

	# DEADLOCK tests
	test_120_reply "DEADLOCK CLEANUP" set 5 || error "DEADLOCK race failed"
	test_120_reply "DEADLOCK REPLY" set 35 || error "DEADLOCK race failed"

	test_120_destroy 0x320 || error "unlock-cleanup race failed"
}
run_test 120 "flock race: completion vs. evict"

test_113() {
	local BEFORE=$(date +%s)
	local EVICT

	# modify dir so that next revalidate would not obtain UPDATE lock
	touch $DIR

	# drop 1 reply with UPDATE lock,
	# resend should not create 2nd lock on server
	mcreate $DIR/$tfile || error "mcreate failed: $?"
	drop_ldlm_reply_once "stat $DIR/$tfile" || error "stat failed: $?"

	# 2 BL AST will be sent to client, both must find the same lock,
	# race them to not get EINVAL for 2nd BL AST
	#define OBD_FAIL_LDLM_PAUSE_CANCEL2      0x31f
	$LCTL set_param fail_loc=0x8000031f

	$LCTL set_param ldlm.namespaces.*.early_lock_cancel=0 > /dev/null
	chmod 0777 $DIR/$tfile || error "chmod failed: $?"
	$LCTL set_param ldlm.namespaces.*.early_lock_cancel=1 > /dev/null

	# let the client reconnect
	client_reconnect
	EVICT=$($LCTL get_param mdc.$FSNAME-MDT*.state |
	  awk -F"[ [,]" '/EVICTED ]$/ { if (mx<$5) {mx=$5;} } END { print mx }')

	[ -z "$EVICT" ] || [[ $EVICT -le $BEFORE ]] || error "eviction happened"
}
run_test 113 "ldlm enqueue dropped reply should not cause deadlocks"

T130_PID=0
test_130_base() {
	test_mkdir -p $DIR/$tdir

	# Prevent interference from layout intent RPCs due to
	# asynchronous writeback. These will be tested in 130c below.
	do_nodes ${CLIENTS:-$HOSTNAME} sync

	# get only LOOKUP lock on $tdir
	cancel_lru_locks mdc
	ls $DIR/$tdir/$tfile 2>/dev/null

	# get getattr by fid on $tdir
	#
	# we need to race with unlink, unlink must complete before we will
	# take a DLM lock, otherwise unlink will wait until getattr will
	# complete; but later than getattr starts so that getattr found
	# the object
#define OBD_FAIL_MDS_INTENT_DELAY		0x160
	set_nodes_failloc "$(mdts_nodes)" 0x80000160
	stat $DIR/$tdir &
	T130_PID=$!
	sleep 2

	rm -rf $DIR/$tdir

	# drop the reply so that resend happens on an unlinked file.
#define OBD_FAIL_MDS_LDLM_REPLY_NET	 0x157
	set_nodes_failloc "$(mdts_nodes)" 0x80000157
}

test_130a() {
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	test_130_base

	wait $T130_PID || [ $? -eq 0 ] && error "stat should fail"
	return 0
}
run_test 130a "enqueue resend on not existing file"

test_130b() {
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	test_130_base
	# let the reply to be dropped
	sleep 10

#define OBD_FAIL_SRV_ENOENT              0x217
	set_nodes_failloc "$(mdts_nodes)" 0x80000217

	wait $T130_PID || [ $? -eq 0 ] && error "stat should fail"
	return 0
}
run_test 130b "enqueue resend on a stale inode"

test_130c() {
	remote_mds_nodsh && skip "remote MDS with nodsh" && return

	do_nodes ${CLIENTS:-$HOSTNAME} sync
	echo XXX > $DIR/$tfile

	cancel_lru_locks mdc

	# Trigger writeback on $tfile.
	#
	# we need to race with unlink, unlink must complete before we will
	# take a DLM lock, otherwise unlink will wait until intent will
	# complete; but later than intent starts so that intent found
	# the object
#define OBD_FAIL_MDS_INTENT_DELAY		0x160
	set_nodes_failloc "$(mdts_nodes)" 0x80000160
	sync &
	T130_PID=$!
	sleep 2

	rm $DIR/$tfile

	# drop the reply so that resend happens on an unlinked file.
#define OBD_FAIL_MDS_LDLM_REPLY_NET	 0x157
	set_nodes_failloc "$(mdts_nodes)" 0x80000157

	# let the reply to be dropped
	sleep 10

#define OBD_FAIL_SRV_ENOENT              0x217
	set_nodes_failloc "$(mdts_nodes)" 0x80000217

	wait $T130_PID

	return 0
}
run_test 130c "layout intent resend on a stale inode"

complete $SECONDS
check_and_cleanup_lustre
exit_status
