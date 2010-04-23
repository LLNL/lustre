#!/bin/bash

set -e

ONLY=${ONLY:-"$*"}
# bug number for skipped test: 3192 12652  15528/3811 16929 9977 15528/11549  18080
ALWAYS_EXCEPT="                14b  14c    19         22    28   29           35    $SANITYN_EXCEPT"
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

# bug number for skipped test:                                                    12652 12652
grep -q 'Enterprise Server 10' /etc/SuSE-release && ALWAYS_EXCEPT="$ALWAYS_EXCEPT 11    14" || true

# Tests that fail on uml
[ "$UML" = "true" ] && EXCEPT="$EXCEPT 7"

SRCDIR=`dirname $0`
PATH=$PWD/$SRCDIR:$SRCDIR:$SRCDIR/../utils:$PATH

SIZE=${SIZE:-40960}
CHECKSTAT=${CHECKSTAT:-"checkstat -v"}
GETSTRIPE=${GETSTRIPE:-lfs getstripe}
SETSTRIPE=${SETSTRIPE:-lstripe}
MCREATE=${MCREATE:-mcreate}
OPENFILE=${OPENFILE:-openfile}
OPENUNLINK=${OPENUNLINK:-openunlink}
TRUNCATE=${TRUNCATE:-truncate}
export TMP=${TMP:-/tmp}
MOUNT_2=${MOUNT_2:-"yes"}
CHECK_GRANT=${CHECK_GRANT:-"yes"}
GRANT_CHECK_LIST=${GRANT_CHECK_LIST:-""}

SAVE_PWD=$PWD

export NAME=${NAME:-local}

LUSTRE=${LUSTRE:-`dirname $0`/..}
. $LUSTRE/tests/test-framework.sh
CLEANUP=${CLEANUP:-:}
SETUP=${SETUP:-:}
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}

[ "$SLOW" = "no" ] && EXCEPT_SLOW="12 16"

SANITYLOG=${TESTSUITELOG:-$TMP/$(basename $0 .sh).log}
FAIL_ON_ERROR=${FAIL_ON_ERROR:-false}

SETUP=${SETUP:-:}
TRACE=${TRACE:-""}

[ "$SANITYLOG" ] && rm -f $SANITYLOG || true

check_and_setup_lustre

LOVNAME=`lctl get_param -n llite.*.lov.common_name | tail -n 1`
OSTCOUNT=`lctl get_param -n lov.$LOVNAME.numobd`

assert_DIR
rm -rf $DIR1/[df][0-9]* $DIR1/lnk

# $RUNAS_ID may get set incorrectly somewhere else
[ $UID -eq 0 -a $RUNAS_ID -eq 0 ] && error "\$RUNAS_ID set to 0, but \$UID is also 0!"

check_runas_id $RUNAS_ID $RUNAS_ID $RUNAS

build_test_filter

test_1a() {
	touch $DIR1/f1
	[ -f $DIR2/f1 ] || error
}
run_test 1a "check create on 2 mtpt's =========================="

test_1b() {
	chmod 777 $DIR2/f1
	$CHECKSTAT -t file -p 0777 $DIR1/f1 || error
	chmod a-x $DIR2/f1
}
run_test 1b "check attribute updates on 2 mtpt's ==============="

test_1c() {
	$CHECKSTAT -t file -p 0666 $DIR1/f1 || error
}
run_test 1c "check after remount attribute updates on 2 mtpt's ="

test_1d() {
	rm $DIR2/f1
	$CHECKSTAT -a $DIR1/f1 || error
}
run_test 1d "unlink on one mountpoint removes file on other ===="

test_2a() {
	touch $DIR1/f2a
	ls -l $DIR2/f2a
	chmod 777 $DIR2/f2a
	$CHECKSTAT -t file -p 0777 $DIR1/f2a || error
}
run_test 2a "check cached attribute updates on 2 mtpt's ========"

test_2b() {
	touch $DIR1/f2b
	ls -l $DIR2/f2b
	chmod 777 $DIR1/f2b
	$CHECKSTAT -t file -p 0777 $DIR2/f2b || error
}
run_test 2b "check cached attribute updates on 2 mtpt's ========"

# NEED TO SAVE ROOT DIR MODE
test_2c() {
	chmod 777 $DIR1
	$CHECKSTAT -t dir -p 0777 $DIR2 || error
}
run_test 2c "check cached attribute updates on 2 mtpt's root ==="

test_2d() {
	chmod 755 $DIR1
	$CHECKSTAT -t dir -p 0755 $DIR2 || error
}
run_test 2d "check cached attribute updates on 2 mtpt's root ==="

test_2e() {
        chmod 755 $DIR1
        ls -l $DIR1
        ls -l $DIR2
        chmod 777 $DIR1
        $RUNAS dd if=/dev/zero of=$DIR2/$tfile count=1 || error
}
run_test 2e "check chmod on root is propagated to others"

test_3() {
	( cd $DIR1 ; ln -s this/is/good $tfile )
	[ "this/is/good" = "`perl -e 'print readlink("'$DIR2/$tfile'");'`" ] ||
		error "link $DIR2/$tfile not as expected"
}
run_test 3 "symlink on one mtpt, readlink on another ==========="

test_4() {
	multifstat $DIR1/f4 $DIR2/f4
}
run_test 4 "fstat validation on multiple mount points =========="

test_5() {
	mcreate $DIR1/f5
	truncate $DIR2/f5 100
	$CHECKSTAT -t file -s 100 $DIR1/f5 || error
	rm $DIR1/f5
}
run_test 5 "create a file on one mount, truncate it on the other"

test_6() {
	openunlink $DIR1/$tfile $DIR2/$tfile || \
		error "openunlink $DIR1/$tfile $DIR2/$tfile"
}
run_test 6 "remove of open file on other node =================="

test_7() {
	local dir=d7
	opendirunlink $DIR1/$dir $DIR2/$dir || \
		error "opendirunlink $DIR1/$dir $DIR2/$dir"
}
run_test 7 "remove of open directory on other node ============="

test_8() {
	opendevunlink $DIR1/$tfile $DIR2/$tfile || \
		error "opendevunlink $DIR1/$tfile $DIR2/$tfile"
}
run_test 8 "remove of open special file on other node =========="

test_9() {
	MTPT=1
	local dir
	> $DIR2/f9
	for C in a b c d e f g h i j k l; do
		dir=`eval echo \\$DIR$MTPT`
		echo -n $C >> $dir/f9
		[ "$MTPT" -eq 1 ] && MTPT=2 || MTPT=1
	done
	[ "`cat $DIR1/f9`" = "abcdefghijkl" ] || \
		error "`od -a $DIR1/f9` != abcdefghijkl"
}
run_test 9 "append of file with sub-page size on multiple mounts"

test_10a() {
	MTPT=1
	local dir
	OFFSET=0
	> $DIR2/f10
	for C in a b c d e f g h i j k l; do
		dir=`eval echo \\$DIR$MTPT`
		echo -n $C | dd of=$dir/f10 bs=1 seek=$OFFSET count=1
		[ "$MTPT" -eq 1 ] && MTPT=2 || MTPT=1
		OFFSET=`expr $OFFSET + 1`
	done
	[ "`cat $DIR1/f10`" = "abcdefghijkl" ] || \
		error "`od -a $DIR1/f10` != abcdefghijkl"
}
run_test 10a "write of file with sub-page size on multiple mounts "

test_10b() {
	# create a seed file
	yes "R" | head -c 4000 >$TMP/f10b-seed
	dd if=$TMP/f10b-seed of=$DIR1/f10b bs=3k count=1 || error "dd $DIR1"

	truncate $DIR1/f10b 4096 || error "truncate 4096"

	dd if=$DIR2/f10b of=$TMP/f10b-lustre bs=4k count=1 || error "dd $DIR2"

	# create a test file locally to compare
	dd if=$TMP/f10b-seed of=$TMP/f10b bs=3k count=1 || error "dd random"
	truncate $TMP/f10b 4096 || error "truncate 4096"
	cmp $TMP/f10b $TMP/f10b-lustre || error "file miscompare"
	rm $TMP/f10b $TMP/f10b-lustre $TMP/f10b-seed
}
run_test 10b "write of file with sub-page size on multiple mounts "

test_11() {
	mkdir $DIR1/d11
	multiop_bg_pause $DIR1/d11/f O_c || return 1
	MULTIPID=$!
	cp -p /bin/ls $DIR1/d11/f
	$DIR2/d11/f
	RC=$?
	kill -USR1 $MULTIPID
	wait $MULTIPID || error
	[ $RC -eq 0 ] && error || true
}
run_test 11 "execution of file opened for write should return error ===="

test_12() {
       DIR=$DIR DIR2=$DIR2 sh lockorder.sh
}
run_test 12 "test lock ordering (link, stat, unlink) ==========="

test_13() {	# bug 2451 - directory coherency
       rm -rf $DIR1/d13
       mkdir $DIR1/d13 || error
       cd $DIR1/d13 || error
       ls
       ( touch $DIR1/d13/f13 ) # needs to be a separate shell
       ls
       rm -f $DIR2/d13/f13 || error
       ls 2>&1 | grep f13 && error "f13 shouldn't return an error (1)" || true
       # need to run it twice
       ( touch $DIR1/d13/f13 ) # needs to be a separate shell
       ls
       rm -f $DIR2/d13/f13 || error
       ls 2>&1 | grep f13 && error "f13 shouldn't return an error (2)" || true
}
run_test 13 "test directory page revocation ===================="

test_14() {
	mkdir -p $DIR1/$tdir
	cp -p /bin/ls $DIR1/$tdir/$tfile
	multiop_bg_pause $DIR1/$tdir/$tfile Ow_c || return 1
	MULTIPID=$!

	$DIR2/$tdir/$tfile && error || true
	kill -USR1 $MULTIPID
	wait $MULTIPID || return 2
}
run_test 14 "execution of file open for write returns -ETXTBSY ="

test_14a() {
        mkdir -p $DIR1/d14
	cp -p `which multiop` $DIR1/d14/multiop || error "cp failed"
        MULTIOP_PROG=$DIR1/d14/multiop multiop_bg_pause $TMP/test14.junk O_c || return 1
        MULTIOP_PID=$!
        multiop $DIR2/d14/multiop Oc && error "expected error, got success"
        kill -USR1 $MULTIOP_PID || return 2
        wait $MULTIOP_PID || return 3
        rm $TMP/test14.junk $DIR1/d14/multiop || error "removing multiop"
}
run_test 14a "open(RDWR) of executing file returns -ETXTBSY ===="

test_14b() { # bug 3192, 7040
        mkdir -p $DIR1/d14
	cp -p `which multiop` $DIR1/d14/multiop || error "cp failed"
        MULTIOP_PROG=$DIR1/d14/multiop multiop_bg_pause $TMP/test14.junk O_c || return 1
        MULTIOP_PID=$!
        truncate $DIR2/d14/multiop 0 && kill -9 $MULTIOP_PID && \
		error "expected truncate error, got success"
        kill -USR1 $MULTIOP_PID || return 2
        wait $MULTIOP_PID || return 3
	cmp `which multiop` $DIR1/d14/multiop || error "binary changed"
	rm $TMP/test14.junk $DIR1/d14/multiop || error "removing multiop"
}
run_test 14b "truncate of executing file returns -ETXTBSY ======"

test_14c() { # bug 3430, 7040
	mkdir -p $DIR1/d14
	cp -p `which multiop` $DIR1/d14/multiop || error "cp failed"
	MULTIOP_PROG=$DIR1/d14/multiop multiop_bg_pause $TMP/test14.junk O_c || return 1
        MULTIOP_PID=$!
	cp /etc/hosts $DIR2/d14/multiop && error "expected error, got success"
	kill -USR1 $MULTIOP_PID || return 2
	wait $MULTIOP_PID || return 3
	cmp `which multiop` $DIR1/d14/multiop || error "binary changed"
	rm $TMP/test14.junk $DIR1/d14/multiop || error "removing multiop"
}
run_test 14c "open(O_TRUNC) of executing file return -ETXTBSY =="

test_14d() { # bug 10921
	mkdir -p $DIR1/d14
	cp -p `which multiop` $DIR1/d14/multiop || error "cp failed"
	MULTIOP_PROG=$DIR1/d14/multiop multiop_bg_pause $TMP/test14.junk O_c || return 1
        MULTIOP_PID=$!
	log chmod
	chmod 600 $DIR1/d14/multiop || error "chmod failed"
	kill -USR1 $MULTIOP_PID || return 2
	wait $MULTIOP_PID || return 3
	cmp `which multiop` $DIR1/d14/multiop || error "binary changed"
	rm $TMP/test14.junk $DIR1/d14/multiop || error "removing multiop"
}
run_test 14d "chmod of executing file is still possible ========"

test_15() {	# bug 974 - ENOSPC
	echo "PATH=$PATH"
	sh oos2.sh $MOUNT1 $MOUNT2
	grant_error=`dmesg | grep "> available"`
	[ -z "$grant_error" ] || error "$grant_error"
}
run_test 15 "test out-of-space with multiple writers ==========="

test_16() {
	rm -f $MOUNT1/fsxfile
	lfs setstripe $MOUNT1/fsxfile -c -1 # b=10919
	fsx -c 50 -p 100 -N 2500 -l $((SIZE * 256)) -S 0 $MOUNT1/fsxfile $MOUNT2/fsxfile
}
run_test 16 "2500 iterations of dual-mount fsx ================="

test_17() { # bug 3513, 3667
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	lfs setstripe $DIR1/$tfile -i 0 -c 1
	cp /etc/termcap $DIR1/$tfile
	cancel_lru_locks osc > /dev/null
	#define OBD_FAIL_ONCE|OBD_FAIL_LDLM_CREATE_RESOURCE    0x30a
	do_facet ost1 lctl set_param fail_loc=0x8000030a
	ls -ls $DIR1/$tfile | awk '{ print $1,$6 }' > $DIR1/$tfile-1 & \
	ls -ls $DIR2/$tfile | awk '{ print $1,$6 }' > $DIR2/$tfile-2
	wait
	diff -u $DIR1/$tfile-1 $DIR2/$tfile-2 || error "files are different"
}
run_test 17 "resource creation/LVB creation race ==============="

test_18() {
	./mmap_sanity -d $MOUNT1 -m $MOUNT2
	sync; sleep 1; sync
}
run_test 18 "mmap sanity check ================================="

test_19() { # bug3811
	[ -d /proc/fs/lustre/obdfilter ] || return 0

	MAX=`lctl get_param -n obdfilter.*.readcache_max_filesize | head -n 1`
	lctl set_param -n obdfilter.*OST*.readcache_max_filesize=4096
	dd if=/dev/urandom of=$TMP/f19b bs=512k count=32
	SUM=`cksum $TMP/f19b | cut -d" " -f 1,2`
	cp $TMP/f19b $DIR1/f19b
	for i in `seq 1 20`; do
		[ $((i % 5)) -eq 0 ] && log "test_18 loop $i"
		cancel_lru_locks osc > /dev/null
		cksum $DIR1/f19b | cut -d" " -f 1,2 > $TMP/sum1 & \
		cksum $DIR2/f19b | cut -d" " -f 1,2 > $TMP/sum2
		wait
		[ "`cat $TMP/sum1`" = "$SUM" ] || \
			error "$DIR1/f19b `cat $TMP/sum1` != $SUM"
		[ "`cat $TMP/sum2`" = "$SUM" ] || \
			error "$DIR2/f19b `cat $TMP/sum2` != $SUM"
	done
	lctl set_param -n obdfilter.*OST*.readcache_max_filesize=$MAX
	rm $DIR1/f19b
}
run_test 19 "test concurrent uncached read races ==============="

test_20() {
	mkdir $DIR1/d20
	cancel_lru_locks osc
	CNT=$((`lctl get_param -n llite.*.dump_page_cache | wc -l`))
	multiop $DIR1/f20 Ow8190c
	multiop $DIR2/f20 Oz8194w8190c
	multiop $DIR1/f20 Oz0r8190c
	cancel_lru_locks osc
	CNTD=$((`lctl get_param -n llite.*.dump_page_cache | wc -l` - $CNT))
	[ $CNTD -gt 0 ] && \
	    error $CNTD" page left in cache after lock cancel" || true
}
run_test 20 "test extra readahead page left in cache ===="

cleanup_21() {
	trap 0
	umount $DIR1/d21
}

test_21() { # Bug 5907
	mkdir $DIR1/d21
	mount /etc $DIR1/d21 --bind || error "mount failed" # Poor man's mount.
	trap cleanup_21 EXIT
	rmdir -v $DIR1/d21 && error "Removed mounted directory"
	rmdir -v $DIR2/d21 && echo "Removed mounted directory from another mountpoint, needs to be fixed"
	test -d $DIR1/d21 || error "Mounted directory disappeared"
	cleanup_21
	test -d $DIR2/d21 || test -d $DIR1/d21 && error "Removed dir still visible after umount"
	true
}
run_test 21 " Try to remove mountpoint on another dir ===="

JOIN=${JOIN:-"lfs join"}

test_22() { # Bug 9926
	mkdir $DIR1/d21
	dd if=/dev/urandom of=$DIR1/d21/128k bs=1024 count=128
	cp -p $DIR1/d21/128k $DIR1/d21/f_head
	for ((i=0;i<10;i++)); do
		cp -p $DIR1/d21/128k $DIR1/d21/f_tail
		$JOIN $DIR1/d21/f_head $DIR1/d21/f_tail || error "join error"
		$CHECKSTAT -a $DIR1/d21/f_tail || error "tail file exist after join"
	done
	echo aaaaaaaaaaa >> $DIR1/d21/no_joined

	mv $DIR2/d21/f_head $DIR2/
	munlink $DIR2/f_head || error "unlink joined file error"
	cat $DIR2/d21/no_joined || error "cat error"
	rm -rf $DIR2/d21/no_joined || error "unlink normal file error"
}
run_test 22 " After joining in one dir,  open/close unlink file in anther dir"

test_23() { # Bug 5972
	echo "others should see updated atime while another read" > $DIR1/f23

	# clear the lock(mode: LCK_PW) gotten from creating operation
	cancel_lru_locks osc

	time1=`date +%s`
	sleep 2

	multiop_bg_pause $DIR1/f23 or20_c || return 1
	MULTIPID=$!

	time2=`stat -c "%X" $DIR2/f23`

	if (( $time2 <= $time1 )); then
		kill -USR1 $MULTIPID
		error "atime doesn't update among nodes"
	fi

	kill -USR1 $MULTIPID || return 1
	rm -f $DIR1/f23 || error "rm -f $DIR1/f23 failed"
	true
}
run_test 23 " others should see updated atime while another read===="

test_24a() {
	touch $DIR1/$tfile
	lfs df || error "lfs df failed"
	lfs df -ih || error "lfs df -ih failed"
	lfs df -h $DIR1 || error "lfs df -h $DIR1 failed"
	lfs df -i $DIR2 || error "lfs df -i $DIR2 failed"
	lfs df $DIR1/$tfile || error "lfs df $DIR1/$tfile failed"
	lfs df -ih $DIR2/$tfile || error "lfs df -ih $DIR2/$tfile failed"

	OSC=`lctl dl | awk '/-osc-|OSC.*MNT/ {print $4}' | head -n 1`
	lctl --device %$OSC deactivate
	lfs df -i || error "lfs df -i with deactivated OSC failed"
	lctl --device %$OSC recover
	lfs df || error "lfs df with reactivated OSC failed"
}
run_test 24a "lfs df [-ih] [path] test ========================="

test_24b() {
	touch $DIR1/$tfile
	fsnum=`lfs df | grep -c "filesystem summary:"`
	[ $fsnum -eq 2 ] || error "lfs df shows $fsnum != 2 filesystems."
}
run_test 24b "lfs df should show both filesystems ==============="

test_25() {
	[ `lctl get_param -n mdc.*-mdc-*.connect_flags | grep -c acl` -lt 2 ] && \
	    skip "must have acl, skipping" && return

	mkdir -p $DIR1/$tdir
	touch $DIR1/$tdir/f1 || error "touch $DIR1/$tdir/f1"
	chmod 0755 $DIR1/$tdir/f1 || error "chmod 0755 $DIR1/$tdir/f1"

	$RUNAS $CHECKSTAT $DIR2/$tdir/f1 || error "checkstat $DIR2/$tdir/f1 #1"
	setfacl -m u:$RUNAS_ID:--- $DIR1/$tdir || error "setfacl $DIR2/$tdir #1"
	$RUNAS $CHECKSTAT $DIR2/$tdir/f1 && error "checkstat $DIR2/$tdir/f1 #2"
	setfacl -m u:$RUNAS_ID:r-x $DIR1/$tdir || error "setfacl $DIR2/$tdir #2"
	$RUNAS $CHECKSTAT $DIR2/$tdir/f1 || error "checkstat $DIR2/$tdir/f1 #3"
	setfacl -m u:$RUNAS_ID:--- $DIR1/$tdir || error "setfacl $DIR2/$tdir #3"
	$RUNAS $CHECKSTAT $DIR2/$tdir/f1 && error "checkstat $DIR2/$tdir/f1 #4"
	setfacl -x u:$RUNAS_ID: $DIR1/$tdir || error "setfacl $DIR2/$tdir #4"
	$RUNAS $CHECKSTAT $DIR2/$tdir/f1 || error "checkstat $DIR2/$tdir/f1 #5"

	rm -rf $DIR1/$tdir
}
run_test 25 "change ACL on one mountpoint be seen on another ==="

test_26a() {
        utime $DIR1/f26a -s $DIR2/f26a || error
}
run_test 26a "allow mtime to get older"

test_26b() {
        touch $DIR1/$tfile
        sleep 1
        echo "aaa" >> $DIR1/$tfile
        sleep 1
        chmod a+x $DIR2/$tfile
        mt1=`stat -c %Y $DIR1/$tfile`
        mt2=`stat -c %Y $DIR2/$tfile`

        if [ x"$mt1" != x"$mt2" ]; then
                error "not equal mtime, client1: "$mt1", client2: "$mt2"."
        fi
}
run_test 26b "sync mtime between ost and mds"

test_27() {
	cancel_lru_locks osc
	lctl clear
	dd if=/dev/zero of=$DIR2/$tfile bs=$((4096+4))k conv=notrunc count=4 seek=3 &
	DD2_PID=$!
	sleep 0.050s
	log "dd 1 started"

	dd if=/dev/zero of=$DIR1/$tfile bs=$((16384-1024))k conv=notrunc count=1 seek=4 &
	DD1_PID=$!
	log "dd 2 started"

	sleep 1
	dd if=/dev/zero of=$DIR1/$tfile bs=8k conv=notrunc count=1 seek=0
	log "dd 3 finished"
	lctl set_param -n ldlm.dump_namespaces ""
	wait $DD1_PID $DD2_PID
	[ $? -ne 0 ] && lctl dk $TMP/debug || true
}
run_test 27 "align non-overlapping extent locks from request ==="

test_28() { # bug 9977
	ostID=`$LCTL dl | awk '/-osc-|OSC.*MNT/ { ost++; if (ost == 2) { print $1 } }'`

	lfs setstripe $DIR1/$tfile -s 1048576 -i 0 -c 2
	tOBJID=`lfs getstripe $DIR1/$tfile | awk '/^[[:space:]]+1/ {print $2}'`
	dd if=/dev/zero of=$DIR1/$tfile bs=1024k count=2

	$LCTL --device $ostID destroy "${tOBJID}"

	# reading of 1st stripe should pass
	dd if=$DIR2/$tfile of=/dev/null bs=1024k count=1 || error
	# reading of 2nd stripe should fail (this stripe was destroyed)
	dd if=$DIR2/$tfile of=/dev/null bs=1024k count=1 skip=1 && error

	# now, recreating test file
	dd if=/dev/zero of=$DIR1/$tfile bs=1024k count=2 || error
	# reading of 1st stripe should pass
	dd if=$DIR2/$tfile of=/dev/null bs=1024k count=1 || error
	# reading of 2nd stripe should pass
	dd if=$DIR2/$tfile of=/dev/null bs=1024k count=1 skip=1 || error
}
run_test 28 "read/write/truncate file with lost stripes"

test_29() { # bug 10999
	touch $DIR1/$tfile
	#define OBD_FAIL_LDLM_GLIMPSE  0x30f
	lctl set_param fail_loc=0x8000030f
	ls -l $DIR2/$tfile &
	sleep 0.500s
	dd if=/dev/zero of=$DIR1/$tfile bs=4k count=1
	wait
}
#bug 11549 - permanently turn test off in b1_5
run_test 29 "lock put race between glimpse and enqueue ========="

test_30() { #bug #11110
    mkdir -p $DIR1/$tdir
    cp -f /bin/bash $DIR1/$tdir/bash
    /bin/sh -c 'sleep 1; rm -f $DIR2/$tdir/bash; cp /bin/bash $DIR2/$tdir' &
    err=$($DIR1/$tdir/bash -c 'sleep 2; openfile -f O_RDONLY /proc/$$/exe >& /dev/null; echo $?')
    wait
    [ $err -ne 116 ] && error_ignore 12900 "return code ($err) != -ESTALE" && return
    true
}

run_test 30 "recreate file race ========="

test_31a() {
        mkdir -p $DIR1/$tdir || error "Creating dir $DIR1/$tdir"
        writes=`LANG=C dd if=/dev/zero of=$DIR/$tdir/$tfile count=1 2>&1 |
                awk 'BEGIN { FS="+" } /out/ {print $1}'`
        #define OBD_FAIL_LDLM_CANCEL_BL_CB_RACE   0x314
        lctl set_param fail_loc=0x314
        reads=`LANG=C dd if=$DIR2/$tdir/$tfile of=/dev/null 2>&1 |
               awk 'BEGIN { FS="+" } /in/ {print $1}'`
        [ $reads -eq $writes ] || error "read" $reads "blocks, must be" $writes
}
run_test 31a "voluntary cancel / blocking ast race=============="

test_31b() {
        remote_ost || { skip "local OST" && return 0; }
        remote_ost_nodsh && skip "remote OST w/o dsh" && return 0
        mkdir -p $DIR1/$tdir || error "Creating dir $DIR1/$tdir"
        lfs setstripe $DIR/$tdir/$tfile -i 0 -c 1
        cp /etc/hosts $DIR/$tdir/$tfile
        #define OBD_FAIL_LDLM_CANCEL_BL_CB_RACE   0x314
        lctl set_param fail_loc=0x314
        #define OBD_FAIL_LDLM_OST_FAIL_RACE      0x316
        do_facet ost1 lctl set_param fail_loc=0x316
        # Don't crash kernel
        cat $DIR2/$tdir/$tfile > /dev/null 2>&1
        lctl set_param fail_loc=0
        do_facet ost1 lctl set_param fail_loc=0
        # cleanup: reconnect the client back
        df $DIR2
}
run_test 31b "voluntary OST cancel / blocking ast race=============="

# enable/disable lockless truncate feature, depending on the arg 0/1
enable_lockless_truncate() {
        lctl set_param -n llite.*.lockless_truncate $1
}

test_32a() { # bug 11270
        local p="$TMP/sanityN-$TESTNAME.parameters"
        save_lustre_params $HOSTNAME llite.*.lockless_truncate > $p
        rm -f $DIR1/$tfile
        cancel_lru_locks osc
        enable_lockless_truncate 1
        lfs setstripe -c -1 -s 1m $DIR1/$tfile
        dd if=/dev/zero of=$DIR1/$tfile count=10 bs=1M > /dev/null 2>&1
        clear_llite_stats

        log "checking cached lockless truncate"
        $TRUNCATE $DIR1/$tfile 8000000
        $CHECKSTAT -s 8000000 $DIR2/$tfile || error "wrong file size"
        [ $(calc_llite_stats lockless_truncate) -eq 0 ] ||
                error "lockless truncate doesn't use cached locks"

        log "checking not cached lockless truncate"
        $TRUNCATE $DIR2/$tfile 5000000
        $CHECKSTAT -s 5000000 $DIR1/$tfile || error "wrong file size"
        [ $(calc_llite_stats lockless_truncate) -ne 0 ] ||
                error "not cached trancate isn't lockless"

        log "disabled lockless truncate"
        enable_lockless_truncate 0
        clear_llite_stats
        $TRUNCATE $DIR2/$tfile 3000000
        $CHECKSTAT -s 3000000 $DIR1/$tfile || error "wrong file size"
        [ $(calc_llite_stats lockless_truncate) -eq 0 ] ||
                error "lockless truncate disabling failed"
        rm $DIR1/$tfile
        # restore lockless_truncate default values
        restore_lustre_params < $p
        rm -f $p
}
run_test 32a "lockless truncate"

test_32b() { # bug 11270
        remote_ost_nodsh && skip "remote OST with nodsh" && return

        local node
        local p="$TMP/sanityN-$TESTNAME.parameters"
        save_lustre_params $HOSTNAME "llite.*.contention_seconds" > $p
        for node in $(osts_nodes); do
                save_lustre_params $node "ldlm.namespaces.filter-*.max_nolock_bytes" >> $p
                save_lustre_params $node "ldlm.namespaces.filter-*.contended_locks" >> $p
                save_lustre_params $node "ldlm.namespaces.filter-*.contention_seconds" >> $p
        done
        clear_llite_stats
        # agressive lockless i/o settings
        for node in $(osts_nodes); do
                do_node $node 'lctl set_param -n ldlm.namespaces.filter-*.max_nolock_bytes 2000000; lctl set_param -n ldlm.namespaces.filter-*.contended_locks 0; lctl set_param -n ldlm.namespaces.filter-*.contention_seconds 60'
        done
        lctl set_param -n llite.*.contention_seconds 60
        for i in $(seq 5); do
                dd if=/dev/zero of=$DIR1/$tfile bs=4k count=1 conv=notrunc > /dev/null 2>&1
                dd if=/dev/zero of=$DIR2/$tfile bs=4k count=1 conv=notrunc > /dev/null 2>&1
        done
        [ $(calc_llite_stats lockless_write_bytes) -ne 0 ] || error "lockless i/o was not triggered"
        # disable lockless i/o (it is disabled by default)
        for node in $(osts_nodes); do
                do_node $node 'lctl set_param -n ldlm.namespaces.filter-*.max_nolock_bytes 0; lctl set_param -n ldlm.namespaces.filter-*.contended_locks 32; lctl set_param -n ldlm.namespaces.filter-*.contention_seconds 0'
        done
        # set contention_seconds to 0 at client too, otherwise Lustre still
        # remembers lock contention
        lctl set_param -n llite.*.contention_seconds 0
        clear_llite_stats
        for i in $(seq 5); do
                dd if=/dev/zero of=$DIR1/$tfile bs=4k count=1 conv=notrunc > /dev/null 2>&1
                dd if=/dev/zero of=$DIR2/$tfile bs=4k count=1 conv=notrunc > /dev/null 2>&1
        done
        [ $(calc_llite_stats lockless_write_bytes) -eq 0 ] ||
                error "lockless i/o works when disabled"
        rm -f $DIR1/$tfile
        restore_lustre_params <$p
        rm -f $p
}
run_test 32b "lockless i/o"

test_33() { #16129
        local OPER
        local lock_in
        local lock_out
        for OPER in notimeout timeout ; do
                rm $DIR1/$tfile 2>/dev/null
                lock_in=$(do_nodes $(osts_nodes) "lctl get_param -n ldlm.namespaces.filter-*.lock_timeouts" | calc_sum)
                if [ $OPER == "timeout" ] ; then
                        for j in `seq $OSTCOUNT`; do
                                #define OBD_FAIL_PTLRPC_HPREQ_TIMEOUT    0x511
                                do_facet ost$j lctl set_param fail_loc=0x511
                        done
                        echo lock should expire
                else
                        for j in `seq $OSTCOUNT`; do
                                #define OBD_FAIL_PTLRPC_HPREQ_NOTIMEOUT  0x512
                                do_facet ost$j lctl set_param fail_loc=0x512
                        done
                        echo lock should not expire
                fi
                echo writing on client1
                dd if=/dev/zero of=$DIR1/$tfile count=100 conv=notrunc > /dev/null 2>&1
                sync &
                echo reading on client2
                dd of=/dev/null if=$DIR2/$tfile > /dev/null 2>&1
                # wait for a lock timeout
                sleep 4
                lock_out=$(do_nodes $(osts_nodes) "lctl get_param -n ldlm.namespaces.filter-*.lock_timeouts" | calc_sum)
                if [ $OPER == "timeout" ] ; then
                        if [ $lock_in == $lock_out ]; then
                                error "no lock timeout happened"
                        else
                                echo "success"
                        fi
                else
                        if [ $lock_in != $lock_out ]; then
                                error "lock timeout happened"
                        else
                                echo "success"
                        fi
                fi
        done
}
run_test 33 "no lock timeout under IO"

test_35() { # bug 17645
        local generation=[]
        local count=0
        for imp in /proc/fs/lustre/mdc/$FSNAME-MDT*-mdc-*; do
            g=$(awk '/generation/{print $2}' $imp/import)
            generation[count]=$g
            let count=count+1
        done

        mkdir -p $MOUNT1/$tfile
        cancel_lru_locks mdc

        # Let's initiate -EINTR situation by setting fail_loc and take
        # write lock on same file from same client. This will not cause
        # bl_ast yet as lock is already in local cache.
#define OBD_FAIL_LDLM_INTR_CP_AST        0x317
        do_facet client "lctl set_param fail_loc=0x80000317"
        local timeout=`do_facet mds lctl get_param  -n timeout`
        let timeout=timeout*3
        local nr=0
        while test $nr -lt 10; do
                log "Race attempt $nr"
                local blk1=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
                test "x$blk1" = "x" && blk1=0
                createmany -o $MOUNT2/$tfile/a 4000 &
                pid1=$!
                sleep 1

                # Let's make conflict and bl_ast
                ls -la $MOUNT1/$tfile > /dev/null &
                pid2=$!

                log "Wait for $pid1 $pid2 for $timeout sec..."
                sleep $timeout
                kill -9 $pid1 $pid2 > /dev/null 2>&1
                wait
                local blk2=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
                test "x$blk2" = "x" && blk2=0
                test $blk2 -gt $blk1 && break
                rm -fr $MOUNT1/$tfile/*
                cancel_lru_locks mdc
                let nr=nr+1
        done
        do_facet client "lctl set_param fail_loc=0x0"
        df -h $MOUNT1 $MOUNT2
        count=0
        for imp in /proc/fs/lustre/mdc/$FSNAME-MDT*-mdc-*; do
            g=$(awk '/generation/{print $2}' $imp/import)
            if ! test "$g" -eq "${generation[count]}"; then
                error "Eviction happened on import $(basename $imp)"
            fi
            let count=count+1
        done
}
run_test 35 "-EINTR cp_ast vs. bl_ast race does not evict client"

test_36() { #bug 16417
    local SIZE
    local SIZE_B
    local i

    mkdir -p $DIR1/$tdir
    $LFS setstripe -c -1 $DIR1/$tdir
    i=0
    SIZE=50
    let SIZE_B=SIZE*1024*1024

    while [ $i -le 10 ]; do
        lctl mark "start test"
        local before=$($LFS df | awk '{if ($1 ~/^filesystem/) {print $5; exit} }')
        dd if=/dev/zero of=$DIR1/$tdir/file000 bs=1M count=$SIZE
        sync
        sleep 1
        local after_dd=$($LFS df | awk '{if ($1 ~/^filesystem/) {print $5; exit} }')
        multiop_bg_pause $DIR2/$tdir/file000 O_r${SIZE_B}c || return 3
        read_pid=$!
        rm -f $DIR1/$tdir/file000
        kill -USR1 $read_pid
        wait $read_pid
        sleep 1
        local after=$($LFS df | awk '{if ($1 ~/^filesystem/) {print $5; exit} }')
        echo "*** cycle($i) *** before($before):after_dd($after_dd):after($after)"
        # this free space! not used
        if [ $after_dd -ge $after ]; then
            error "space leaked"
            return 1;
        fi
        let i=i+1
    done
}
run_test 36 "handle ESTALE/open-unlink corectly"

test_37() { # bug 18695
	mkdir -p $DIR1/$tdir
	multiop_bg_pause $DIR1/$tdir D_c || return 1
	MULTIPID=$!
	# create large directory (32kB seems enough from e2fsck, ~= 1000 files)
	createmany -m $DIR2/$tdir/f 10000
	# set mtime/atime backward
	touch -t 198001010000 $DIR2/$tdir
	kill -USR1 $MULTIPID
	nr_files=`lfs find $DIR1/$tdir -type f | wc -l`
	[ $nr_files -eq 10000 ] || error "$nr_files != 10000 truncated directory?"
}
run_test 37 "check i_size is not updated for directory on close (bug 18695) =============="

test_38() { # bug 18801, based on the code of test_32b
        remote_ost_nodsh && skip "remote OST with nodsh" && return

        local node
        local p="$TMP/sanityN-$TESTNAME.parameters"
        local random="$TMP/sanityN-$TESTNAME.random"
        # 1. locked unaligned non-DIRECT_IO write of 8192 bytes to file A
        # 2a. locked unaligned DIRECT_IO write of 4000 bytes to file B
        # 2b. locked unaligned DIRECT_IO write of 4000 bytes to file B
        # 3. unaligned "lockless DIRECT_IO" write of 192 bytes in the end of file B
        # 4. compare A and B
        log "creating the initial file"
        multiop $random Ob4000b4000b192c || error "failed creating random file"
        log "creating a file with the same contents"
        multiop $DIR1/$tfile oO_CREAT:O_DIRECT:O_RDWR:b4000c || error "first multiop failed"
        multiop $DIR1/$tfile oO_CREAT:O_DIRECT:O_RDWR:z4000b4000c || error "second multiop failed"
        save_lustre_params $HOSTNAME "llite.*.contention_seconds" > $p
        for node in $(osts_nodes); do
                save_lustre_params $node "ldlm.namespaces.filter-*.max_nolock_bytes" >> $p
                save_lustre_params $node "ldlm.namespaces.filter-*.contended_locks" >> $p
                save_lustre_params $node "ldlm.namespaces.filter-*.contention_seconds" >> $p
        done
        log "enforcing lockless I/O"
        clear_llite_stats
        # agressive lockless i/o settings
        for node in $(osts_nodes); do
                do_node $node 'lctl set_param -n ldlm.namespaces.filter-*.max_nolock_bytes 2000000; lctl set_param -n ldlm.namespaces.filter-*.contended_locks 0; lctl set_param -n ldlm.namespaces.filter-*.contention_seconds 60'
        done
        lctl set_param -n llite.*.contention_seconds 60
        multiop $DIR2/$tfile oO_DIRECT:O_RDWR:z8000b192c || error "the last multiop failed"
        [ $(calc_llite_stats lockless_write_bytes) -ne 0 ] || error "lockless i/o was not triggered"
        restore_lustre_params <$p
        log "comparing"
        cmp $DIR1/$tfile $random || error "O_DIRECT+lockless results do not match the original file"
        rm -f $DIR1/$tfile
        rm -f $p
        rm -f $random
}
run_test 38 "lockless i/o with O_DIRECT and unaligned writes"

test_39() {
        local originaltime
        local updatedtime
        local delay=3

        touch $DIR1/$tfile
        originaltime=$(stat -c %Y $DIR1/$tfile)
        log "original modification time is $originaltime"
        sleep $delay
        multiop $DIR1/$tfile oO_DIRECT:O_WRONLY:w$((10*1048576))c || error "multiop has failed"
        updatedtime=$(stat -c %Y $DIR2/$tfile)
        log "updated modification time is $updatedtime"
        [ $((updatedtime - originaltime)) -ge $delay ] || error "invalid modification time"
        rm -rf $DIR/$tfile
}
run_test 39 "direct I/O writes should update mtime ========="

log "cleanup: ======================================================"

check_and_cleanup_lustre

echo '=========================== finished ==============================='
[ -f "$SANITYLOG" ] && cat $SANITYLOG && grep -q FAIL $SANITYLOG && exit 1 || true
echo "$0: completed"

