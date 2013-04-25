#!/bin/bash
# -*- tab-width: 4; indent-tabs-mode: t; -*-
#
# Run select tests by setting ONLY, or as arguments to the script.
# Skip specific tests by setting EXCEPT.
#
# e.g. ONLY="22 23" or ONLY="`seq 32 39`" or EXCEPT="31"
set -e

ONLY=${ONLY:-"$*"}
# bug number for skipped test: 13297 2108 9789 3637 9789 3561 12622 5188
ALWAYS_EXCEPT="                42a  42b  42c  42d  45   51d   68b   $SANITY_EXCEPT"
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

# with LOD/OSP landing
# bug number for skipped tests: LU-2036
ALWAYS_EXCEPT="                 76     $ALWAYS_EXCEPT"


SRCDIR=$(cd $(dirname $0); echo $PWD)
export PATH=$PATH:/sbin

TMP=${TMP:-/tmp}

CHECKSTAT=${CHECKSTAT:-"checkstat -v"}
CREATETEST=${CREATETEST:-createtest}
LFS=${LFS:-lfs}
LFIND=${LFIND:-"$LFS find"}
LVERIFY=${LVERIFY:-ll_dirstripe_verify}
LCTL=${LCTL:-lctl}
MCREATE=${MCREATE:-mcreate}
OPENFILE=${OPENFILE:-openfile}
OPENUNLINK=${OPENUNLINK:-openunlink}
export MULTIOP=${MULTIOP:-multiop}
READS=${READS:-"reads"}
MUNLINK=${MUNLINK:-munlink}
SOCKETSERVER=${SOCKETSERVER:-socketserver}
SOCKETCLIENT=${SOCKETCLIENT:-socketclient}
MEMHOG=${MEMHOG:-memhog}
DIRECTIO=${DIRECTIO:-directio}
ACCEPTOR_PORT=${ACCEPTOR_PORT:-988}
UMOUNT=${UMOUNT:-"umount -d"}
STRIPES_PER_OBJ=-1
CHECK_GRANT=${CHECK_GRANT:-"yes"}
GRANT_CHECK_LIST=${GRANT_CHECK_LIST:-""}
export PARALLEL=${PARALLEL:-"no"}

export NAME=${NAME:-local}

SAVE_PWD=$PWD

CLEANUP=${CLEANUP:-:}
SETUP=${SETUP:-:}
TRACE=${TRACE:-""}
LUSTRE=${LUSTRE:-$(cd $(dirname $0)/..; echo $PWD)}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/${NAME}.sh}
init_logging

[ "$SLOW" = "no" ] && EXCEPT_SLOW="24o 27m 64b 68 71 77f 78 115 124b"

[ $(facet_fstype $SINGLEMDS) = "zfs" ] &&
# bug number for skipped test:        LU-1593 LU-2610 LU-2833 LU-1957 LU-2805
	ALWAYS_EXCEPT="$ALWAYS_EXCEPT 34h     40      48a     180     184c"

FAIL_ON_ERROR=false

cleanup() {
	echo -n "cln.."
	pgrep ll_sa > /dev/null && { echo "There are ll_sa thread not exit!"; exit 20; }
	cleanupall ${FORCE} $* || { echo "FAILed to clean up"; exit 20; }
}
setup() {
	echo -n "mnt.."
        load_modules
	setupall || exit 10
	echo "done"
}

check_kernel_version() {
	WANT_VER=$1
	GOT_VER=$(lctl get_param -n version | awk '/kernel:/ {print $2}')
	case $GOT_VER in
	patchless|patchless_client) return 0;;
	*) [ $GOT_VER -ge $WANT_VER ] && return 0 ;;
	esac
	log "test needs at least kernel version $WANT_VER, running $GOT_VER"
	return 1
}

check_swap_layouts_support()
{
	$LCTL get_param -n llite.*.sbi_flags | grep -q layout ||
		{ skip "Does not support layout lock."; return 0; }
	return 1
}

if [ "$ONLY" == "cleanup" ]; then
       sh llmountcleanup.sh
       exit 0
fi

check_and_setup_lustre

DIR=${DIR:-$MOUNT}
assert_DIR

MDT0=$($LCTL get_param -n mdc.*.mds_server_uuid | \
    awk '{gsub(/_UUID/,""); print $1}' | head -1)
LOVNAME=$($LCTL get_param -n llite.*.lov.common_name | tail -n 1)
OSTCOUNT=$($LCTL get_param -n lov.$LOVNAME.numobd)
STRIPECOUNT=$($LCTL get_param -n lov.$LOVNAME.stripecount)
STRIPESIZE=$($LCTL get_param -n lov.$LOVNAME.stripesize)
ORIGFREE=$($LCTL get_param -n lov.$LOVNAME.kbytesavail)
MAXFREE=${MAXFREE:-$((200000 * $OSTCOUNT))}

[ -f $DIR/d52a/foo ] && chattr -a $DIR/d52a/foo
[ -f $DIR/d52b/foo ] && chattr -i $DIR/d52b/foo
rm -rf $DIR/[Rdfs][0-9]*

# $RUNAS_ID may get set incorrectly somewhere else
[ $UID -eq 0 -a $RUNAS_ID -eq 0 ] && error "\$RUNAS_ID set to 0, but \$UID is also 0!"

check_runas_id $RUNAS_ID $RUNAS_GID $RUNAS

build_test_filter

if [ "${ONLY}" = "MOUNT" ] ; then
	echo "Lustre is up, please go on"
	exit
fi

echo "preparing for tests involving mounts"
EXT2_DEV=${EXT2_DEV:-$TMP/SANITY.LOOP}
touch $EXT2_DEV
mke2fs -j -F $EXT2_DEV 8000 > /dev/null
echo # add a newline after mke2fs.

umask 077

OLDDEBUG="`lctl get_param -n debug 2> /dev/null`"
lctl set_param debug=-1 2> /dev/null || true
test_0() {
	touch $DIR/$tfile
	$CHECKSTAT -t file $DIR/$tfile || error
	rm $DIR/$tfile
	$CHECKSTAT -a $DIR/$tfile || error
}
run_test 0 "touch .../$tfile ; rm .../$tfile ====================="

test_0b() {
	chmod 0755 $DIR || error
	$CHECKSTAT -p 0755 $DIR || error
}
run_test 0b "chmod 0755 $DIR ============================="

test_0c() {
    $LCTL get_param mdc.*.import | grep  "state: FULL" || error "import not FULL"
    $LCTL get_param mdc.*.import | grep  "target: $FSNAME-MDT" || error "bad target"
}
run_test 0c "check import proc ============================="

test_1a() {
	test_mkdir -p $DIR/$tdir
	test_mkdir -p $DIR/$tdir/d2
	test_mkdir $DIR/$tdir/d2 && error "we expect EEXIST, but not returned"
	$CHECKSTAT -t dir $DIR/$tdir/d2 || error
}
run_test 1a "mkdir .../d1; mkdir .../d1/d2 ====================="

test_1b() {
	rmdir $DIR/$tdir/d2
	rmdir $DIR/$tdir
	$CHECKSTAT -a $DIR/$tdir || error
}
run_test 1b "rmdir .../d1/d2; rmdir .../d1 ====================="

test_2a() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	$CHECKSTAT -t file $DIR/$tdir/$tfile || error
}
run_test 2a "mkdir .../d2; touch .../d2/f ======================"

test_2b() {
	rm -r $DIR/$tdir
	$CHECKSTAT -a $DIR/$tdir || error
}
run_test 2b "rm -r .../d2; checkstat .../d2/f ======================"

test_3a() {
	test_mkdir -p $DIR/$tdir
	$CHECKSTAT -t dir $DIR/$tdir || error
}
run_test 3a "mkdir .../d3 ======================================"

test_3b() {
	if [ ! -d $DIR/$tdir ]; then
		mkdir $DIR/$tdir
	fi
	touch $DIR/$tdir/$tfile
	$CHECKSTAT -t file $DIR/$tdir/$tfile || error
}
run_test 3b "touch .../d3/f ===================================="

test_3c() {
	rm -r $DIR/$tdir
	$CHECKSTAT -a $DIR/$tdir || error
}
run_test 3c "rm -r .../d3 ======================================"

test_4a() {
	test_mkdir -p $DIR/$tdir
	$CHECKSTAT -t dir $DIR/$tdir || error
}
run_test 4a "mkdir .../d4 ======================================"

test_4b() {
	if [ ! -d $DIR/$tdir ]; then
		test_mkdir $DIR/$tdir
	fi
	test_mkdir $DIR/$tdir/d2
	mkdir $DIR/$tdir/d2
	$CHECKSTAT -t dir $DIR/$tdir/d2 || error
}
run_test 4b "mkdir .../d4/d2 ==================================="

test_5() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	chmod 0707 $DIR/$tdir/d2
	$CHECKSTAT -t dir -p 0707 $DIR/$tdir/d2 || error
}
run_test 5 "mkdir .../d5 .../d5/d2; chmod .../d5/d2 ============"

test_6a() {
	touch $DIR/$tfile
	chmod 0666 $DIR/$tfile || error
	$CHECKSTAT -t file -p 0666 -u \#$UID $DIR/$tfile || error
}
run_test 6a "touch .../f6a; chmod .../f6a ======================"

test_6b() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID" && return
	if [ ! -f $DIR/$tfile ]; then
		touch $DIR/$tfile
		chmod 0666 $DIR/$tfile
	fi
	$RUNAS chmod 0444 $DIR/$tfile && error
	$CHECKSTAT -t file -p 0666 -u \#$UID $DIR/$tfile || error
}
run_test 6b "$RUNAS chmod .../f6a (should return error) =="

test_6c() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID" && return
	touch $DIR/$tfile
	chown $RUNAS_ID $DIR/$tfile || error
	$CHECKSTAT -t file -u \#$RUNAS_ID $DIR/$tfile || error
}
run_test 6c "touch .../f6c; chown .../f6c ======================"

test_6d() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID" && return
	if [ ! -f $DIR/$tfile ]; then
		touch $DIR/$tfile
		chown $RUNAS_ID $DIR/$tfile
	fi
	$RUNAS chown $UID $DIR/$tfile && error
	$CHECKSTAT -t file -u \#$RUNAS_ID $DIR/$tfile || error
}
run_test 6d "$RUNAS chown .../f6c (should return error) =="

test_6e() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID" && return
	touch $DIR/$tfile
	chgrp $RUNAS_ID $DIR/$tfile || error
	$CHECKSTAT -t file -u \#$UID -g \#$RUNAS_ID $DIR/$tfile || error
}
run_test 6e "touch .../f6e; chgrp .../f6e ======================"

test_6f() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID" && return
	if [ ! -f $DIR/$tfile ]; then
		touch $DIR/$tfile
		chgrp $RUNAS_ID $DIR/$tfile
	fi
	$RUNAS chgrp $UID $DIR/$tfile && error
	$CHECKSTAT -t file -u \#$UID -g \#$RUNAS_ID $DIR/$tfile || error
}
run_test 6f "$RUNAS chgrp .../f6e (should return error) =="

test_6g() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID" && return
	test_mkdir $DIR/$tdir || error
        chmod 777 $DIR/$tdir || error
        $RUNAS mkdir $DIR/$tdir/d || error
        chmod g+s $DIR/$tdir/d || error
        test_mkdir $DIR/$tdir/d/subdir
	$CHECKSTAT -g \#$RUNAS_GID $DIR/$tdir/d/subdir || error
}
run_test 6g "Is new dir in sgid dir inheriting group?"

test_6h() { # bug 7331
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID" && return
	touch $DIR/$tfile || error "touch failed"
	chown $RUNAS_ID:$RUNAS_GID $DIR/$tfile || error "initial chown failed"
	$RUNAS -G$RUNAS_GID chown $RUNAS_ID:0 $DIR/$tfile &&
		error "chown worked"
	$CHECKSTAT -t file -u \#$RUNAS_ID -g \#$RUNAS_GID $DIR/$tfile || error
}
run_test 6h "$RUNAS chown RUNAS_ID.0 .../f6h (should return error)"

test_7a() {
	test_mkdir $DIR/$tdir
	$MCREATE $DIR/$tdir/$tfile
	chmod 0666 $DIR/$tdir/$tfile
	$CHECKSTAT -t file -p 0666 $DIR/$tdir/$tfile || error
}
run_test 7a "mkdir .../d7; mcreate .../d7/f; chmod .../d7/f ===="

test_7b() {
	if [ ! -d $DIR/$tdir ]; then
		mkdir $DIR/$tdir
	fi
	$MCREATE $DIR/$tdir/$tfile
	echo -n foo > $DIR/$tdir/$tfile
	[ "`cat $DIR/$tdir/$tfile`" = "foo" ] || error
	$CHECKSTAT -t file -s 3 $DIR/$tdir/$tfile || error
}
run_test 7b "mkdir .../d7; mcreate d7/f2; echo foo > d7/f2 ====="

test_8() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	chmod 0666 $DIR/$tdir/$tfile
	$CHECKSTAT -t file -p 0666 $DIR/$tdir/$tfile || error
}
run_test 8 "mkdir .../d8; touch .../d8/f; chmod .../d8/f ======="

test_9() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	test_mkdir $DIR/$tdir/d2/d3
	$CHECKSTAT -t dir $DIR/$tdir/d2/d3 || error
}
run_test 9 "mkdir .../d9 .../d9/d2 .../d9/d2/d3 ================"

test_10() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	touch $DIR/$tdir/d2/$tfile
	$CHECKSTAT -t file $DIR/$tdir/d2/$tfile || error
}
run_test 10 "mkdir .../d10 .../d10/d2; touch .../d10/d2/f ======"

test_11() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	chmod 0666 $DIR/$tdir/d2
	chmod 0705 $DIR/$tdir/d2
	$CHECKSTAT -t dir -p 0705 $DIR/$tdir/d2 || error
}
run_test 11 "mkdir .../d11 d11/d2; chmod .../d11/d2 ============"

test_12() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	chmod 0666 $DIR/$tdir/$tfile
	chmod 0654 $DIR/$tdir/$tfile
	$CHECKSTAT -t file -p 0654 $DIR/$tdir/$tfile || error
}
run_test 12 "touch .../d12/f; chmod .../d12/f .../d12/f ========"

test_13() {
	test_mkdir $DIR/$tdir
	dd if=/dev/zero of=$DIR/$tdir/$tfile count=10
	>  $DIR/$tdir/$tfile
	$CHECKSTAT -t file -s 0 $DIR/$tdir/$tfile || error
}
run_test 13 "creat .../d13/f; dd .../d13/f; > .../d13/f ========"

test_14() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	rm $DIR/$tdir/$tfile
	$CHECKSTAT -a $DIR/$tdir/$tfile || error
}
run_test 14 "touch .../d14/f; rm .../d14/f; rm .../d14/f ======="

test_15() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	mv $DIR/$tdir/$tfile $DIR/$tdir/${tfile}_2
	$CHECKSTAT -t file $DIR/$tdir/${tfile}_2 || error
}
run_test 15 "touch .../d15/f; mv .../d15/f .../d15/f2 =========="

test_16() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	rm -rf $DIR/$tdir/$tfile
	$CHECKSTAT -a $DIR/$tdir/$tfile || error
}
run_test 16 "touch .../d16/f; rm -rf .../d16/f ================="

test_17a() {
	test_mkdir -p $DIR/$tdir
	touch $DIR/$tdir/$tfile
	ln -s $DIR/$tdir/$tfile $DIR/$tdir/l-exist
	ls -l $DIR/$tdir
	$CHECKSTAT -l $DIR/$tdir/$tfile $DIR/$tdir/l-exist || error
	$CHECKSTAT -f -t f $DIR/$tdir/l-exist || error
	rm -f $DIR/$tdir/l-exist
	$CHECKSTAT -a $DIR/$tdir/l-exist || error
}
run_test 17a "symlinks: create, remove (real) =================="

test_17b() {
	test_mkdir -p $DIR/$tdir
	ln -s no-such-file $DIR/$tdir/l-dangle
	ls -l $DIR/$tdir
	$CHECKSTAT -l no-such-file $DIR/$tdir/l-dangle || error
	$CHECKSTAT -fa $DIR/$tdir/l-dangle || error
	rm -f $DIR/$tdir/l-dangle
	$CHECKSTAT -a $DIR/$tdir/l-dangle || error
}
run_test 17b "symlinks: create, remove (dangling) =============="

test_17c() { # bug 3440 - don't save failed open RPC for replay
	test_mkdir -p $DIR/$tdir
	ln -s foo $DIR/$tdir/$tfile
	cat $DIR/$tdir/$tfile && error "opened non-existent symlink" || true
}
run_test 17c "symlinks: open dangling (should return error) ===="

test_17d() {
	test_mkdir -p $DIR/$tdir
	ln -s foo $DIR/$tdir/$tfile
	touch $DIR/$tdir/$tfile || error "creating to new symlink"
}
run_test 17d "symlinks: create dangling ========================"

test_17e() {
	test_mkdir -p $DIR/$tdir
	local foo=$DIR/$tdir/$tfile
	ln -s $foo $foo || error "create symlink failed"
	ls -l $foo || error "ls -l failed"
	ls $foo && error "ls not failed" || true
}
run_test 17e "symlinks: create recursive symlink (should return error) ===="

test_17f() {
	test_mkdir -p $DIR/d17f
	ln -s 1234567890/2234567890/3234567890/4234567890 $DIR/d17f/111
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890 $DIR/d17f/222
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890/7234567890/8234567890 $DIR/d17f/333
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890/7234567890/8234567890/9234567890/a234567890/b234567890 $DIR/d17f/444
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890/7234567890/8234567890/9234567890/a234567890/b234567890/c234567890/d234567890/f234567890 $DIR/d17f/555
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890/7234567890/8234567890/9234567890/a234567890/b234567890/c234567890/d234567890/f234567890/aaaaaaaaaa/bbbbbbbbbb/cccccccccc/dddddddddd/eeeeeeeeee/ffffffffff/ $DIR/d17f/666
	ls -l  $DIR/d17f
}
run_test 17f "symlinks: long and very long symlink name ========================"

# str_repeat(S, N) generate a string that is string S repeated N times
str_repeat() {
	local s=$1
	local n=$2
	local ret=''
	while [ $((n -= 1)) -ge 0 ]; do
		ret=$ret$s
	done
	echo $ret
}

# Long symlinks and LU-2241
test_17g() {
	test_mkdir -p $DIR/$tdir
	local TESTS="59 60 61 4094 4095"

	# Fix for inode size boundary in 2.1.4
	[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.1.4) ] &&
		TESTS="4094 4095"

	# Patch not applied to 2.2 or 2.3 branches
	[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.2.0) ] &&
	[ $(lustre_version_code $SINGLEMDS) -le $(version_code 2.3.55) ] &&
		TESTS="4094 4095"

	for i in $TESTS; do
		local SYMNAME=$(str_repeat 'x' $i)
		ln -s $SYMNAME $DIR/$tdir/f$i || error "failed $i-char symlink"
		readlink $DIR/$tdir/f$i	|| error "failed $i-char readlink"
	done
}
run_test 17g "symlinks: really long symlink name and inode boundaries"

test_17h() { #bug 17378
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local mdt_idx
        test_mkdir -p $DIR/$tdir
	if [ $MDSCOUNT -gt 1 ]; then
		mdt_idx=$($LFS getdirstripe -i $DIR/$tdir)
	else
		mdt_idx=0
	fi
        $SETSTRIPE -c -1 $DIR/$tdir
#define OBD_FAIL_MDS_LOV_PREP_CREATE 0x141
        do_facet mds$((mdt_idx + 1)) lctl set_param fail_loc=0x80000141
        touch $DIR/$tdir/$tfile || true
}
run_test 17h "create objects: lov_free_memmd() doesn't lbug"

test_17i() { #bug 20018
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	local foo=$DIR/$tdir/$tfile
	local mdt_idx
	if [ $MDSCOUNT -gt 1 ]; then
		mdt_idx=$($LFS getdirstripe -i $DIR/$tdir)
	else
		mdt_idx=0
	fi
	ln -s $foo $foo || error "create symlink failed"
#define OBD_FAIL_MDS_READLINK_EPROTO     0x143
	do_facet mds$((mdt_idx + 1)) lctl set_param fail_loc=0x80000143
	ls -l $foo && error "error not detected"
	return 0
}
run_test 17i "don't panic on short symlink"

test_17k() { #bug 22301
        rsync --help | grep -q xattr ||
                skip_env "$(rsync --version| head -1) does not support xattrs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	test_mkdir -p $DIR/$tdir.new
	touch $DIR/$tdir/$tfile
	ln -s $DIR/$tdir/$tfile $DIR/$tdir/$tfile.lnk
	rsync -av -X $DIR/$tdir/ $DIR/$tdir.new ||
		error "rsync failed with xattrs enabled"
}
run_test 17k "symlinks: rsync with xattrs enabled ========================="

test_17l() { # LU-279
	mkdir -p $DIR/$tdir
	touch $DIR/$tdir/$tfile
	ln -s $DIR/$tdir/$tfile $DIR/$tdir/$tfile.lnk
	for path in "$DIR/$tdir" "$DIR/$tdir/$tfile" "$DIR/$tdir/$tfile.lnk"; do
		# -h to not follow symlinks. -m '' to list all the xattrs.
		# grep to remove first line: '# file: $path'.
		for xattr in `getfattr -hm '' $path 2>/dev/null | grep -v '^#'`;
		do
			lgetxattr_size_check $path $xattr ||
				error "lgetxattr_size_check $path $xattr failed"
		done
	done
}
run_test 17l "Ensure lgetxattr's returned xattr size is consistent ========"

# LU-1540
test_17m() {
	local short_sym="0123456789"
	local WDIR=$DIR/${tdir}m
	local mds_index
	local devname
	local cmd
	local i
	local rc=0

	[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.2.0) ] &&
	[ $(lustre_version_code $SINGLEMDS) -le $(version_code 2.2.93) ] &&
		skip "MDS 2.2.0-2.2.93 do not NUL-terminate symlinks" && return

	[ "$(facet_fstype $SINGLEMDS)" != "ldiskfs" ] &&
		skip "only for ldiskfs MDT" && return 0

	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return

	mkdir -p $WDIR
	long_sym=$short_sym
	# create a long symlink file
	for ((i = 0; i < 4; ++i)); do
		long_sym=${long_sym}${long_sym}
	done

	echo "create 512 short and long symlink files under $WDIR"
	for ((i = 0; i < 256; ++i)); do
		ln -sf ${long_sym}"a5a5" $WDIR/long-$i
		ln -sf ${short_sym}"a5a5" $WDIR/short-$i
	done

	echo "erase them"
	rm -f $WDIR/*
	sync
	wait_delete_completed

	echo "recreate the 512 symlink files with a shorter string"
	for ((i = 0; i < 512; ++i)); do
		# rewrite the symlink file with a shorter string
		ln -sf ${long_sym} $WDIR/long-$i
		ln -sf ${short_sym} $WDIR/short-$i
	done

	mds_index=$($LFS getstripe -M $WDIR)
	mds_index=$((mds_index+1))
	devname=$(mdsdevname $mds_index)
	cmd="$E2FSCK -fnvd $devname"

	echo "stop and checking mds${mds_index}: $cmd"
	# e2fsck should not return error
	stop mds${mds_index} -f
	do_facet mds${mds_index} $cmd || rc=$?

	start mds${mds_index} $devname $MDS_MOUNT_OPTS
	df $MOUNT > /dev/null 2>&1
	[ $rc -ne 0 ] && error "e2fsck should not report error upon "\
		"short/long symlink MDT: rc=$rc"
	return $rc
}
run_test 17m "run e2fsck against MDT which contains short/long symlink"

check_fs_consistency_17n() {
	local mdt_index
	local devname
	local cmd
	local rc=0

	for mdt_index in $(seq 1 $MDSCOUNT); do
		devname=$(mdsdevname $mdt_index)
		cmd="$E2FSCK -fnvd $devname"

		echo "stop and checking mds${mdt_index}: $cmd"
		# e2fsck should not return error
		stop mds${mdt_index}
		do_facet mds${mdt_index} $cmd || rc=$?

		start mds${mdt_index} $devname $MDS_MOUNT_OPTS
		df $MOUNT > /dev/null 2>&1
		[ $rc -ne 0 ] && break
	done
	return $rc
}

test_17n() {
	local i

	[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.2.0) ] &&
	[ $(lustre_version_code $SINGLEMDS) -le $(version_code 2.2.93) ] &&
		skip "MDS 2.2.0-2.2.93 do not NUL-terminate symlinks" && return

	[ "$(facet_fstype $SINGLEMDS)" != "ldiskfs" ] &&
		skip "only for ldiskfs MDT" && return 0

	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return

	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return

	mkdir -p $DIR/$tdir
	for ((i=0; i<10; i++)); do
		$LFS mkdir -i 1 $DIR/$tdir/remote_dir_${i} ||
			error "create remote dir error $i"
		createmany -o $DIR/$tdir/remote_dir_${i}/f 10 ||
			error "create files under remote dir failed $i"
	done

	check_fs_consistency_17n || error "e2fsck report error"

	for ((i=0;i<10;i++)); do
		rm -rf $DIR/$tdir/remote_dir_${i} ||
			error "destroy remote dir error $i"
	done

	check_fs_consistency_17n || error "e2fsck report error"
}
run_test 17n "run e2fsck against master/slave MDT which contains remote dir"

test_18() {
	touch $DIR/f || error "Failed to touch $DIR/f: $?"
	ls $DIR || error "Failed to ls $DIR: $?"
}
run_test 18 "touch .../f ; ls ... =============================="

test_19a() {
	touch $DIR/f19
	ls -l $DIR
	rm $DIR/f19
	$CHECKSTAT -a $DIR/f19 || error
}
run_test 19a "touch .../f19 ; ls -l ... ; rm .../f19 ==========="

test_19b() {
	ls -l $DIR/f19 && error || true
}
run_test 19b "ls -l .../f19 (should return error) =============="

test_19c() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID -- skipping" && return
	$RUNAS touch $DIR/f19 && error || true
}
run_test 19c "$RUNAS touch .../f19 (should return error) =="

test_19d() {
	cat $DIR/f19 && error || true
}
run_test 19d "cat .../f19 (should return error) =============="

test_20() {
	touch $DIR/f
	rm $DIR/f
	log "1 done"
	touch $DIR/f
	rm $DIR/f
	log "2 done"
	touch $DIR/f
	rm $DIR/f
	log "3 done"
	$CHECKSTAT -a $DIR/f || error
}
run_test 20 "touch .../f ; ls -l ... ==========================="

test_21() {
	test_mkdir $DIR/d21
	[ -f $DIR/d21/dangle ] && rm -f $DIR/d21/dangle
	ln -s dangle $DIR/d21/link
	echo foo >> $DIR/d21/link
	cat $DIR/d21/dangle
	$CHECKSTAT -t link $DIR/d21/link || error
	$CHECKSTAT -f -t file $DIR/d21/link || error
}
run_test 21 "write to dangling link ============================"

test_22() {
	WDIR=$DIR/$tdir
	test_mkdir -p $DIR/$tdir
	chown $RUNAS_ID:$RUNAS_GID $WDIR
	(cd $WDIR || error "cd $WDIR failed";
	$RUNAS tar cf - /etc/hosts /etc/sysconfig/network | \
	$RUNAS tar xf -)
	ls -lR $WDIR/etc || error "ls -lR $WDIR/etc failed"
	$CHECKSTAT -t dir $WDIR/etc || error "checkstat -t dir failed"
	$CHECKSTAT -u \#$RUNAS_ID -g \#$RUNAS_GID $WDIR/etc || error "checkstat -u failed"
}
run_test 22 "unpack tar archive as non-root user ==============="

# was test_23
test_23a() {
	test_mkdir -p $DIR/$tdir
	local file=$DIR/$tdir/$tfile

	openfile -f O_CREAT:O_EXCL $file || error "$file create failed"
	openfile -f O_CREAT:O_EXCL $file &&
		error "$file recreate succeeded" || true
}
run_test 23a "O_CREAT|O_EXCL in subdir =========================="

test_23b() { # bug 18988
	test_mkdir -p $DIR/$tdir
	local file=$DIR/$tdir/$tfile

        rm -f $file
        echo foo > $file || error "write filed"
        echo bar >> $file || error "append filed"
        $CHECKSTAT -s 8 $file || error "wrong size"
        rm $file
}
run_test 23b "O_APPEND check =========================="

test_24a() {
	echo '== rename sanity =============================================='
	echo '-- same directory rename'
	test_mkdir $DIR/R1
	touch $DIR/R1/f
	mv $DIR/R1/f $DIR/R1/g
	$CHECKSTAT -t file $DIR/R1/g || error
}
run_test 24a "touch .../R1/f; rename .../R1/f .../R1/g ========="

test_24b() {
	test_mkdir $DIR/R2
	touch $DIR/R2/{f,g}
	mv $DIR/R2/f $DIR/R2/g
	$CHECKSTAT -a $DIR/R2/f || error
	$CHECKSTAT -t file $DIR/R2/g || error
}
run_test 24b "touch .../R2/{f,g}; rename .../R2/f .../R2/g ====="

test_24c() {
	test_mkdir $DIR/R3
	test_mkdir $DIR/R3/f
	mv $DIR/R3/f $DIR/R3/g
	$CHECKSTAT -a $DIR/R3/f || error
	$CHECKSTAT -t dir $DIR/R3/g || error
}
run_test 24c "mkdir .../R3/f; rename .../R3/f .../R3/g ========="

test_24d() {
	test_mkdir $DIR/R4
	test_mkdir $DIR/R4/f
	test_mkdir $DIR/R4/g
	mrename $DIR/R4/f $DIR/R4/g
	$CHECKSTAT -a $DIR/R4/f || error
	$CHECKSTAT -t dir $DIR/R4/g || error
}
run_test 24d "mkdir .../R4/{f,g}; rename .../R4/f .../R4/g ====="

test_24e() {
	echo '-- cross directory renames --'
	test_mkdir $DIR/R5a
	test_mkdir $DIR/R5b
	touch $DIR/R5a/f
	mv $DIR/R5a/f $DIR/R5b/g
	$CHECKSTAT -a $DIR/R5a/f || error
	$CHECKSTAT -t file $DIR/R5b/g || error
}
run_test 24e "touch .../R5a/f; rename .../R5a/f .../R5b/g ======"

test_24f() {
	test_mkdir $DIR/R6a
	test_mkdir $DIR/R6b
	touch $DIR/R6a/f $DIR/R6b/g
	mv $DIR/R6a/f $DIR/R6b/g
	$CHECKSTAT -a $DIR/R6a/f || error
	$CHECKSTAT -t file $DIR/R6b/g || error
}
run_test 24f "touch .../R6a/f R6b/g; mv .../R6a/f .../R6b/g ===="

test_24g() {
	test_mkdir $DIR/R7a
	test_mkdir $DIR/R7b
	test_mkdir $DIR/R7a/d
	mv $DIR/R7a/d $DIR/R7b/e
	$CHECKSTAT -a $DIR/R7a/d || error
	$CHECKSTAT -t dir $DIR/R7b/e || error
}
run_test 24g "mkdir .../R7{a,b}/d; mv .../R7a/d .../R7b/e ======"

test_24h() {
	test_mkdir $DIR/R8a
	test_mkdir $DIR/R8b
	test_mkdir $DIR/R8a/d
	test_mkdir $DIR/R8b/e
	mrename $DIR/R8a/d $DIR/R8b/e
	$CHECKSTAT -a $DIR/R8a/d || error
	$CHECKSTAT -t dir $DIR/R8b/e || error
}
run_test 24h "mkdir .../R8{a,b}/{d,e}; rename .../R8a/d .../R8b/e"

test_24i() {
	echo "-- rename error cases"
	test_mkdir $DIR/R9
	test_mkdir $DIR/R9/a
	touch $DIR/R9/f
	mrename $DIR/R9/f $DIR/R9/a
	$CHECKSTAT -t file $DIR/R9/f || error
	$CHECKSTAT -t dir  $DIR/R9/a || error
	$CHECKSTAT -a $DIR/R9/a/f || error
}
run_test 24i "rename file to dir error: touch f ; mkdir a ; rename f a"

test_24j() {
	test_mkdir $DIR/R10
	mrename $DIR/R10/f $DIR/R10/g
	$CHECKSTAT -t dir $DIR/R10 || error
	$CHECKSTAT -a $DIR/R10/f || error
	$CHECKSTAT -a $DIR/R10/g || error
}
run_test 24j "source does not exist ============================"

test_24k() {
	test_mkdir $DIR/R11a
	test_mkdir $DIR/R11a/d
	touch $DIR/R11a/f
	mv $DIR/R11a/f $DIR/R11a/d
        $CHECKSTAT -a $DIR/R11a/f || error
        $CHECKSTAT -t file $DIR/R11a/d/f || error
}
run_test 24k "touch .../R11a/f; mv .../R11a/f .../R11a/d ======="

# bug 2429 - rename foo foo foo creates invalid file
test_24l() {
	f="$DIR/f24l"
	$MULTIOP $f OcNs || error
}
run_test 24l "Renaming a file to itself ========================"

test_24m() {
	f="$DIR/f24m"
	$MULTIOP $f OcLN ${f}2 ${f}2 || error "link ${f}2 ${f}2 failed"
	# on ext3 this does not remove either the source or target files
	# though the "expected" operation would be to remove the source
	$CHECKSTAT -t file ${f} || error "${f} missing"
	$CHECKSTAT -t file ${f}2 || error "${f}2 missing"
}
run_test 24m "Renaming a file to a hard link to itself ========="

test_24n() {
    f="$DIR/f24n"
    # this stats the old file after it was renamed, so it should fail
    touch ${f}
    $CHECKSTAT ${f}
    mv ${f} ${f}.rename
    $CHECKSTAT ${f}.rename
    $CHECKSTAT -a ${f}
}
run_test 24n "Statting the old file after renaming (Posix rename 2)"

test_24o() {
	check_kernel_version 37 || return 0
	test_mkdir -p $DIR/d24o
	rename_many -s random -v -n 10 $DIR/d24o
}
run_test 24o "rename of files during htree split ==============="

test_24p() {
	test_mkdir $DIR/R12a
	test_mkdir $DIR/R12b
	DIRINO=`ls -lid $DIR/R12a | awk '{ print $1 }'`
	mrename $DIR/R12a $DIR/R12b
	$CHECKSTAT -a $DIR/R12a || error
	$CHECKSTAT -t dir $DIR/R12b || error
	DIRINO2=`ls -lid $DIR/R12b | awk '{ print $1 }'`
	[ "$DIRINO" = "$DIRINO2" ] || error "R12a $DIRINO != R12b $DIRINO2"
}
run_test 24p "mkdir .../R12{a,b}; rename .../R12a .../R12b"

cleanup_multiop_pause() {
	trap 0
	kill -USR1 $MULTIPID
}

test_24q() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir $DIR/R13a
	test_mkdir $DIR/R13b
	local DIRINO=$(ls -lid $DIR/R13a | awk '{ print $1 }')
	multiop_bg_pause $DIR/R13b D_c || error "multiop failed to start"
	MULTIPID=$!

	trap cleanup_multiop_pause EXIT
	mrename $DIR/R13a $DIR/R13b
	$CHECKSTAT -a $DIR/R13a || error "R13a still exists"
	$CHECKSTAT -t dir $DIR/R13b || error "R13b does not exist"
	local DIRINO2=$(ls -lid $DIR/R13b | awk '{ print $1 }')
	[ "$DIRINO" = "$DIRINO2" ] || error "R13a $DIRINO != R13b $DIRINO2"
	cleanup_multiop_pause
	wait $MULTIPID || error "multiop close failed"
}
run_test 24q "mkdir .../R13{a,b}; open R13b rename R13a R13b ==="

test_24r() { #bug 3789
	test_mkdir $DIR/R14a
	test_mkdir $DIR/R14a/b
	mrename $DIR/R14a $DIR/R14a/b && error "rename to subdir worked!"
	$CHECKSTAT -t dir $DIR/R14a || error "$DIR/R14a missing"
	$CHECKSTAT -t dir $DIR/R14a/b || error "$DIR/R14a/b missing"
}
run_test 24r "mkdir .../R14a/b; rename .../R14a .../R14a/b ====="

test_24s() {
	test_mkdir $DIR/R15a
	test_mkdir $DIR/R15a/b
	test_mkdir $DIR/R15a/b/c
	mrename $DIR/R15a $DIR/R15a/b/c && error "rename to sub-subdir worked!"
	$CHECKSTAT -t dir $DIR/R15a || error "$DIR/R15a missing"
	$CHECKSTAT -t dir $DIR/R15a/b/c || error "$DIR/R15a/b/c missing"
}
run_test 24s "mkdir .../R15a/b/c; rename .../R15a .../R15a/b/c ="
test_24t() {
	test_mkdir $DIR/R16a
	test_mkdir $DIR/R16a/b
	test_mkdir $DIR/R16a/b/c
	mrename $DIR/R16a/b/c $DIR/R16a && error "rename to sub-subdir worked!"
	$CHECKSTAT -t dir $DIR/R16a || error "$DIR/R16a missing"
	$CHECKSTAT -t dir $DIR/R16a/b/c || error "$DIR/R16a/b/c missing"
}
run_test 24t "mkdir .../R16a/b/c; rename .../R16a/b/c .../R16a ="

test_24u() { # bug12192
	rm -rf $DIR/$tfile
	$MULTIOP $DIR/$tfile C2w$((2048 * 1024))c || error
	$CHECKSTAT -s $((2048 * 1024)) $DIR/$tfile || error "wrong file size"
}
run_test 24u "create stripe file"

page_size() {
	getconf PAGE_SIZE
}

simple_cleanup_common() {
	trap 0
	rm -rf $DIR/$tdir
	wait_delete_completed
}

max_pages_per_rpc() {
	$LCTL get_param -n mdc.*.max_pages_per_rpc | head -1
}

test_24v() {
	local NRFILES=100000
	local FREE_INODES=$(lfs_df -i | grep "summary" | awk '{print $4}')
	[ $FREE_INODES -lt $NRFILES ] && \
		skip "not enough free inodes $FREE_INODES required $NRFILES" &&
		return

	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	trap simple_cleanup_common EXIT

	mkdir -p $DIR/$tdir
	createmany -m $DIR/$tdir/$tfile $NRFILES

	cancel_lru_locks mdc
	lctl set_param mdc.*.stats clear

	ls $DIR/$tdir >/dev/null || error "error in listing large dir"

	# LU-5 large readdir
	# DIRENT_SIZE = 32 bytes for sizeof(struct lu_dirent) +
	#               8 bytes for name(filename is mostly 5 in this test) +
	#               8 bytes for luda_type
	# take into account of overhead in lu_dirpage header and end mark in
	# each page, plus one in RPC_NUM calculation.
	DIRENT_SIZE=48
	RPC_SIZE=$(($(max_pages_per_rpc) * $(page_size)))
	RPC_NUM=$(((NRFILES * DIRENT_SIZE + RPC_SIZE - 1) / RPC_SIZE + 1))
	mds_readpage=$(lctl get_param mdc.*MDT0000*.stats | \
				awk '/^mds_readpage/ {print $2}')
	[ $mds_readpage -gt $RPC_NUM ] && \
		error "large readdir doesn't take effect"

	simple_cleanup_common
}
run_test 24v "list directory with large files (handle hash collision, bug: 17560)"

test_24w() { # bug21506
        SZ1=234852
        dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 seek=4096 || return 1
        dd if=/dev/zero bs=$SZ1 count=1 >> $DIR/$tfile || return 2
        dd if=$DIR/$tfile of=$DIR/${tfile}_left bs=1M skip=4097 || return 3
        SZ2=`ls -l $DIR/${tfile}_left | awk '{print $5}'`
        [ "$SZ1" = "$SZ2" ] || \
                error "Error reading at the end of the file $tfile"
}
run_test 24w "Reading a file larger than 4Gb"

test_24x() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX $remote_dir ||
		error "create remote directory failed"

	mkdir -p $DIR/$tdir/src_dir
	touch $DIR/$tdir/src_file
	mkdir -p $remote_dir/tgt_dir
	touch $remote_dir/tgt_file

	mrename $DIR/$tdir/src_dir $remote_dir/tgt_dir &&
		error "rename dir cross MDT works!"

	mrename $DIR/$tdir/src_file $remote_dir/tgt_file &&
		error "rename file cross MDT works!"

	ln $DIR/$tdir/src_file $remote_dir/tgt_file1 &&
		error "ln file cross MDT should not work!"

	rm -rf $DIR/$tdir || error "Can not delete directories"
}
run_test 24x "cross rename/link should be failed"

test_24y() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX $remote_dir ||
	           error "create remote directory failed"

	mkdir -p $remote_dir/src_dir
	touch $remote_dir/src_file
	mkdir -p $remote_dir/tgt_dir
	touch $remote_dir/tgt_file

	mrename $remote_dir/src_dir $remote_dir/tgt_dir ||
		error "rename subdir in the same remote dir failed!"

	mrename $remote_dir/src_file $remote_dir/tgt_file ||
		error "rename files in the same remote dir failed!"

	ln $remote_dir/tgt_file $remote_dir/tgt_file1 ||
		error "link files in the same remote dir failed!"

	rm -rf $DIR/$tdir || error "Can not delete directories"
}
run_test 24y "rename/link on the same dir should succeed"

test_24z() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local MDTIDX=1
	local remote_src=$DIR/$tdir/remote_dir
	local remote_tgt=$DIR/$tdir/remote_tgt

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX $remote_src ||
		   error "create remote directory failed"

	$LFS mkdir -i $MDTIDX $remote_tgt ||
		   error "create remote directory failed"

	mrename $remote_src $remote_tgt &&
		error "rename remote dirs should not work!"

	# If target dir does not exists, it should succeed
	rm -rf $remote_tgt
	mrename $remote_src $remote_tgt ||
		error "rename remote dirs(tgt dir does not exists) failed!"

	rm -rf $DIR/$tdir || error "Can not delete directories"
}
run_test 24z "rename one remote dir to another remote dir should fail"

test_25a() {
	echo '== symlink sanity ============================================='

	test_mkdir $DIR/d25
	ln -s d25 $DIR/s25
	touch $DIR/s25/foo || error
}
run_test 25a "create file in symlinked directory ==============="

test_25b() {
	[ ! -d $DIR/d25 ] && test_25a
	$CHECKSTAT -t file $DIR/s25/foo || error
}
run_test 25b "lookup file in symlinked directory ==============="

test_26a() {
	test_mkdir $DIR/d26
	test_mkdir $DIR/d26/d26-2
	ln -s d26/d26-2 $DIR/s26
	touch $DIR/s26/foo || error
}
run_test 26a "multiple component symlink ======================="

test_26b() {
	test_mkdir -p $DIR/d26b/d26-2
	ln -s d26b/d26-2/foo $DIR/s26-2
	touch $DIR/s26-2 || error
}
run_test 26b "multiple component symlink at end of lookup ======"

test_26c() {
	test_mkdir $DIR/d26.2
	touch $DIR/d26.2/foo
	ln -s d26.2 $DIR/s26.2-1
	ln -s s26.2-1 $DIR/s26.2-2
	ln -s s26.2-2 $DIR/s26.2-3
	chmod 0666 $DIR/s26.2-3/foo
}
run_test 26c "chain of symlinks ================================"

# recursive symlinks (bug 439)
test_26d() {
	ln -s d26-3/foo $DIR/d26-3
}
run_test 26d "create multiple component recursive symlink ======"

test_26e() {
	[ ! -h $DIR/d26-3 ] && test_26d
	rm $DIR/d26-3
}
run_test 26e "unlink multiple component recursive symlink ======"

# recursive symlinks (bug 7022)
test_26f() {
	test_mkdir -p $DIR/$tdir
	test_mkdir $DIR/$tdir/$tfile   || error "mkdir $DIR/$tdir/$tfile failed"
	cd $DIR/$tdir/$tfile           || error "cd $DIR/$tdir/$tfile failed"
	test_mkdir -p lndir bar1      || error "mkdir lndir/bar1 failed"
	test_mkdir $DIR/$tdir/$tfile/$tfile   || error "mkdir $tfile failed"
	cd $tfile                || error "cd $tfile failed"
	ln -s .. dotdot          || error "ln dotdot failed"
	ln -s dotdot/lndir lndir || error "ln lndir failed"
	cd $DIR/$tdir                 || error "cd $DIR/$tdir failed"
	output=`ls $tfile/$tfile/lndir/bar1`
	[ "$output" = bar1 ] && error "unexpected output"
	rm -r $tfile             || error "rm $tfile failed"
	$CHECKSTAT -a $DIR/$tfile || error "$tfile not gone"
}
run_test 26f "rm -r of a directory which has recursive symlink ="

test_27a() {
	echo '== stripe sanity =============================================='
	test_mkdir -p $DIR/d27 || error "mkdir failed"
	$GETSTRIPE $DIR/d27
	$SETSTRIPE -c 1 $DIR/d27/f0 || error "setstripe failed"
	$CHECKSTAT -t file $DIR/d27/f0 || error "checkstat failed"
	pass
	log "== test_27a: write to one stripe file ========================="
	cp /etc/hosts $DIR/d27/f0 || error
}
run_test 27a "one stripe file =================================="

test_27b() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping 2-stripe test" && return
	test_mkdir -p $DIR/d27
	$SETSTRIPE -c 2 $DIR/d27/f01 || error "setstripe failed"
	$GETSTRIPE -c $DIR/d27/f01
	[ $($GETSTRIPE -c $DIR/d27/f01) -eq 2 ] ||
		error "two-stripe file doesn't have two stripes"
}
run_test 27b "create two stripe file"

test_27c() {
	[ -f $DIR/d27/f01 ] || skip "test_27b not run" && return

	dd if=/dev/zero of=$DIR/d27/f01 bs=4k count=4 || error "dd failed"
}
run_test 27c "write to two stripe file"

test_27d() {
	test_mkdir -p $DIR/d27
	$SETSTRIPE -c 0 -i -1 -S 0 $DIR/d27/fdef || error "setstripe failed"
	$CHECKSTAT -t file $DIR/d27/fdef || error "checkstat failed"
	dd if=/dev/zero of=$DIR/d27/fdef bs=4k count=4 || error
}
run_test 27d "create file with default settings ================"

test_27e() {
	test_mkdir -p $DIR/d27
	$SETSTRIPE -c 2 $DIR/d27/f12 || error "setstripe failed"
	$SETSTRIPE -c 2 $DIR/d27/f12 && error "setstripe succeeded twice"
	$CHECKSTAT -t file $DIR/d27/f12 || error "checkstat failed"
}
run_test 27e "setstripe existing file (should return error) ======"

test_27f() {
	test_mkdir -p $DIR/d27
	$SETSTRIPE -S 100 -i 0 -c 1 $DIR/d27/fbad && error "setstripe failed"
	dd if=/dev/zero of=$DIR/d27/f12 bs=4k count=4 || error "dd failed"
	$GETSTRIPE $DIR/d27/fbad || error "$GETSTRIPE failed"
}
run_test 27f "setstripe with bad stripe size (should return error)"

test_27g() {
	test_mkdir -p $DIR/d27
	$MCREATE $DIR/d27/fnone || error "mcreate failed"
	$GETSTRIPE $DIR/d27/fnone 2>&1 | grep "no stripe info" ||
		error "$DIR/d27/fnone has object"
}
run_test 27g "$GETSTRIPE with no objects"

test_27i() {
	touch $DIR/d27/fsome || error "touch failed"
	[ $($GETSTRIPE -c $DIR/d27/fsome) -gt 0 ] || error "missing objects"
}
run_test 27i "$GETSTRIPE with some objects"

test_27j() {
	test_mkdir -p $DIR/d27
	$SETSTRIPE -i $OSTCOUNT $DIR/d27/f27j && error "setstripe failed"||true
}
run_test 27j "setstripe with bad stripe offset (should return error)"

test_27k() { # bug 2844
	test_mkdir -p $DIR/d27
	FILE=$DIR/d27/f27k
	LL_MAX_BLKSIZE=$((4 * 1024 * 1024))
	[ ! -d $DIR/d27 ] && test_mkdir -p $DIR d27
	$SETSTRIPE -S 67108864 $FILE || error "setstripe failed"
	BLKSIZE=`stat $FILE | awk '/IO Block:/ { print $7 }'`
	[ $BLKSIZE -le $LL_MAX_BLKSIZE ] || error "$BLKSIZE > $LL_MAX_BLKSIZE"
	dd if=/dev/zero of=$FILE bs=4k count=1
	BLKSIZE=`stat $FILE | awk '/IO Block:/ { print $7 }'`
	[ $BLKSIZE -le $LL_MAX_BLKSIZE ] || error "$BLKSIZE > $LL_MAX_BLKSIZE"
}
run_test 27k "limit i_blksize for broken user apps ============="

test_27l() {
	test_mkdir -p $DIR/d27
	mcreate $DIR/f27l || error "creating file"
	$RUNAS $SETSTRIPE -c 1 $DIR/f27l && \
		error "setstripe should have failed" || true
}
run_test 27l "check setstripe permissions (should return error)"

test_27m() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "$OSTCOUNT < 2 OSTs -- skipping" &&
		return
	if [ $ORIGFREE -gt $MAXFREE ]; then
		skip "$ORIGFREE > $MAXFREE skipping out-of-space test on OST0"
		return
	fi
	trap simple_cleanup_common EXIT
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE -i 0 -c 1 $DIR/$tdir/f27m_1
	dd if=/dev/zero of=$DIR/$tdir/f27m_1 bs=1024 count=$MAXFREE &&
		error "dd should fill OST0"
	i=2
	while $SETSTRIPE -i 0 -c 1 $DIR/$tdir/f27m_$i; do
		i=`expr $i + 1`
		[ $i -gt 256 ] && break
	done
	i=`expr $i + 1`
	touch $DIR/$tdir/f27m_$i
	[ `$GETSTRIPE $DIR/$tdir/f27m_$i | grep -A 10 obdidx | awk '{print $1}'| grep -w "0"` ] &&
		error "OST0 was full but new created file still use it"
	i=`expr $i + 1`
	touch $DIR/$tdir/f27m_$i
	[ `$GETSTRIPE $DIR/$tdir/f27m_$i | grep -A 10 obdidx | awk '{print $1}'| grep -w "0"` ] &&
		error "OST0 was full but new created file still use it"
	simple_cleanup_common
}
run_test 27m "create file while OST0 was full =================="

sleep_maxage() {
        local DELAY=$(do_facet $SINGLEMDS lctl get_param -n lov.*.qos_maxage | head -n 1 | awk '{print $1 * 2}')
        sleep $DELAY
}

# OSCs keep a NOSPC flag that will be reset after ~5s (qos_maxage)
# if the OST isn't full anymore.
reset_enospc() {
	local OSTIDX=${1:-""}

	local list=$(comma_list $(osts_nodes))
	[ "$OSTIDX" ] && list=$(facet_host ost$((OSTIDX + 1)))

	do_nodes $list lctl set_param fail_loc=0
	sync	# initiate all OST_DESTROYs from MDS to OST
	sleep_maxage
}

exhaust_precreations() {
	local OSTIDX=$1
	local FAILLOC=$2
	local FAILIDX=${3:-$OSTIDX}

	test_mkdir -p $DIR/$tdir
	local MDSIDX=$(get_mds_dir "$DIR/$tdir")
	echo OSTIDX=$OSTIDX MDSIDX=$MDSIDX

	local OST=$(ostname_from_index $OSTIDX)
	local MDT_INDEX=$(lfs df | grep "\[MDT:$((MDSIDX - 1))\]" | awk '{print $1}' | \
			  sed -e 's/_UUID$//;s/^.*-//')

	# on the mdt's osc
	local mdtosc_proc1=$(get_mdtosc_proc_path mds${MDSIDX} $OST)
	local last_id=$(do_facet mds${MDSIDX} lctl get_param -n \
        osc.$mdtosc_proc1.prealloc_last_id)
	local next_id=$(do_facet mds${MDSIDX} lctl get_param -n \
        osc.$mdtosc_proc1.prealloc_next_id)

	local mdtosc_proc2=$(get_mdtosc_proc_path mds${MDSIDX})
	do_facet mds${MDSIDX} lctl get_param osc.$mdtosc_proc2.prealloc*

	test_mkdir -p $DIR/$tdir/${OST}
	$SETSTRIPE -i $OSTIDX -c 1 $DIR/$tdir/${OST}
#define OBD_FAIL_OST_ENOSPC              0x215
	do_facet ost$((OSTIDX + 1)) lctl set_param fail_val=$FAILIDX
	do_facet ost$((OSTIDX + 1)) lctl set_param fail_loc=0x215
	echo "Creating to objid $last_id on ost $OST..."
	createmany -o $DIR/$tdir/${OST}/f $next_id $((last_id - next_id + 2))
	do_facet mds${MDSIDX} lctl get_param osc.$mdtosc_proc2.prealloc*
	do_facet ost$((OSTIDX + 1)) lctl set_param fail_loc=$FAILLOC
	sleep_maxage
}

exhaust_all_precreations() {
	local i
	for (( i=0; i < OSTCOUNT; i++ )) ; do
		exhaust_precreations $i $1 -1
	done
}

test_27n() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "too few OSTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	reset_enospc
	rm -f $DIR/$tdir/$tfile
	exhaust_precreations 0 0x80000215
	$SETSTRIPE -c -1 $DIR/$tdir
	touch $DIR/$tdir/$tfile || error
	$GETSTRIPE $DIR/$tdir/$tfile
	reset_enospc
}
run_test 27n "create file with some full OSTs =================="

test_27o() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "too few OSTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	reset_enospc
	rm -f $DIR/$tdir/$tfile
	exhaust_all_precreations 0x215

	touch $DIR/$tdir/$tfile && error "able to create $DIR/$tdir/$tfile"

	reset_enospc
	rm -rf $DIR/$tdir/*
}
run_test 27o "create file with all full OSTs (should error) ===="

test_27p() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "too few OSTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	reset_enospc
	rm -f $DIR/$tdir/$tfile
	test_mkdir -p $DIR/$tdir

	$MCREATE $DIR/$tdir/$tfile || error "mcreate failed"
	$TRUNCATE $DIR/$tdir/$tfile 80000000 || error "truncate failed"
	$CHECKSTAT -s 80000000 $DIR/$tdir/$tfile || error "checkstat failed"

	exhaust_precreations 0 0x80000215
	echo foo >> $DIR/$tdir/$tfile || error "append failed"
	$CHECKSTAT -s 80000004 $DIR/$tdir/$tfile || error "checkstat failed"
	$GETSTRIPE $DIR/$tdir/$tfile

	reset_enospc
}
run_test 27p "append to a truncated file with some full OSTs ==="

test_27q() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "too few OSTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	reset_enospc
	rm -f $DIR/$tdir/$tfile

	test_mkdir -p $DIR/$tdir
	$MCREATE $DIR/$tdir/$tfile || error "mcreate $DIR/$tdir/$tfile failed"
	$TRUNCATE $DIR/$tdir/$tfile 80000000 ||error "truncate $DIR/$tdir/$tfile failed"
	$CHECKSTAT -s 80000000 $DIR/$tdir/$tfile || error "checkstat failed"

	exhaust_all_precreations 0x215

	echo foo >> $DIR/$tdir/$tfile && error "append succeeded"
	$CHECKSTAT -s 80000000 $DIR/$tdir/$tfile || error "checkstat 2 failed"

	reset_enospc
}
run_test 27q "append to truncated file with all OSTs full (should error) ==="

test_27r() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "too few OSTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	reset_enospc
	rm -f $DIR/$tdir/$tfile
	exhaust_precreations 0 0x80000215

	$SETSTRIPE -i 0 -c 2 $DIR/$tdir/$tfile # && error

	reset_enospc
}
run_test 27r "stripe file with some full OSTs (shouldn't LBUG) ="

test_27s() { # bug 10725
	test_mkdir -p $DIR/$tdir
	local stripe_size=$((4096 * 1024 * 1024))	# 2^32
	local stripe_count=0
	[ $OSTCOUNT -eq 1 ] || stripe_count=2
	$SETSTRIPE -S $stripe_size -c $stripe_count $DIR/$tdir &&
		error "stripe width >= 2^32 succeeded" || true

}
run_test 27s "lsm_xfersize overflow (should error) (bug 10725)"

test_27t() { # bug 10864
        WDIR=`pwd`
        WLFS=`which lfs`
        cd $DIR
        touch $tfile
        $WLFS getstripe $tfile
        cd $WDIR
}
run_test 27t "check that utils parse path correctly"

test_27u() { # bug 4900
	[ "$OSTCOUNT" -lt "2" ] && skip_env "too few OSTs" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	local index
	local list=$(comma_list $(mdts_nodes))

#define OBD_FAIL_MDS_OSC_PRECREATE      0x139
	do_nodes $list $LCTL set_param fail_loc=0x139
	test_mkdir -p $DIR/$tdir
	rm -rf $DIR/$tdir/*
	createmany -o $DIR/$tdir/t- 1000
	do_nodes $list $LCTL set_param fail_loc=0

	TLOG=$DIR/$tfile.getstripe
	$GETSTRIPE $DIR/$tdir > $TLOG
	OBJS=`awk -vobj=0 '($1 == 0) { obj += 1 } END { print obj;}' $TLOG`
	unlinkmany $DIR/$tdir/t- 1000
	[ $OBJS -gt 0 ] && \
		error "$OBJS objects created on OST-0.  See $TLOG" || pass
}
run_test 27u "skip object creation on OSC w/o objects =========="

test_27v() { # bug 4900
        [ "$OSTCOUNT" -lt "2" ] && skip_env "too few OSTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        remote_mds_nodsh && skip "remote MDS with nodsh" && return
        remote_ost_nodsh && skip "remote OST with nodsh" && return

        exhaust_all_precreations 0x215
        reset_enospc

        test_mkdir -p $DIR/$tdir
        $SETSTRIPE -c 1 $DIR/$tdir         # 1 stripe / file

        touch $DIR/$tdir/$tfile
        #define OBD_FAIL_TGT_DELAY_PRECREATE     0x705
        # all except ost1
        for (( i=1; i < OSTCOUNT; i++ )); do
                do_facet ost$i lctl set_param fail_loc=0x705
        done
        local START=`date +%s`
        createmany -o $DIR/$tdir/$tfile 32

        local FINISH=`date +%s`
        local TIMEOUT=`lctl get_param -n timeout`
        local PROCESS=$((FINISH - START))
        [ $PROCESS -ge $((TIMEOUT / 2)) ] && \
               error "$FINISH - $START >= $TIMEOUT / 2"
        sleep $((TIMEOUT / 2 - PROCESS))
        reset_enospc
}
run_test 27v "skip object creation on slow OST ================="

test_27w() { # bug 10997
        test_mkdir -p $DIR/$tdir || error "mkdir failed"
        $SETSTRIPE -S 65536 $DIR/$tdir/f0 || error "setstripe failed"
        [ $($GETSTRIPE -S $DIR/$tdir/f0) -ne 65536 ] &&
                error "stripe size $size != 65536" || true
        [ $($GETSTRIPE -d $DIR/$tdir | grep -c "stripe_count") -ne 1 ] &&
                error "$GETSTRIPE -d $DIR/$tdir failed" || true
}
run_test 27w "check $SETSTRIPE -S option"

test_27wa() {
        [ "$OSTCOUNT" -lt "2" ] &&
                skip_env "skipping multiple stripe count/offset test" && return

        test_mkdir -p $DIR/$tdir || error "mkdir failed"
        for i in $(seq 1 $OSTCOUNT); do
                offset=$((i - 1))
                $SETSTRIPE -c $i -i $offset $DIR/$tdir/f$i ||
                        error "setstripe -c $i -i $offset failed"
                count=$($GETSTRIPE -c $DIR/$tdir/f$i)
                index=$($GETSTRIPE -i $DIR/$tdir/f$i)
                [ $count -ne $i ] && error "stripe count $count != $i" || true
                [ $index -ne $offset ] &&
                        error "stripe offset $index != $offset" || true
        done
}
run_test 27wa "check $SETSTRIPE -c -i options"

test_27x() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return
	[ "$OSTCOUNT" -lt "2" ] && skip_env "$OSTCOUNT < 2 OSTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	OFFSET=$(($OSTCOUNT - 1))
	OSTIDX=0
	local OST=$(ostname_from_index $OSTIDX)

	test_mkdir -p $DIR/$tdir
	$SETSTRIPE -c 1 $DIR/$tdir	# 1 stripe per file
	do_facet ost$((OSTIDX + 1)) lctl set_param -n obdfilter.$OST.degraded 1
	sleep_maxage
	createmany -o $DIR/$tdir/$tfile $OSTCOUNT
	for i in `seq 0 $OFFSET`; do
		[ `$GETSTRIPE $DIR/$tdir/$tfile$i | grep -A 10 obdidx | awk '{print $1}' | grep -w "$OSTIDX"` ] &&
		error "OST0 was degraded but new created file still use it"
	done
	do_facet ost$((OSTIDX + 1)) lctl set_param -n obdfilter.$OST.degraded 0
}
run_test 27x "create files while OST0 is degraded"

test_27y() {
        [ "$OSTCOUNT" -lt "2" ] && skip_env "$OSTCOUNT < 2 OSTs -- skipping" && return
        remote_mds_nodsh && skip "remote MDS with nodsh" && return
        remote_ost_nodsh && skip "remote OST with nodsh" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return

        local mdtosc=$(get_mdtosc_proc_path $SINGLEMDS $FSNAME-OST0000)
        local last_id=$(do_facet $SINGLEMDS lctl get_param -n \
            osc.$mdtosc.prealloc_last_id)
        local next_id=$(do_facet $SINGLEMDS lctl get_param -n \
            osc.$mdtosc.prealloc_next_id)
        local fcount=$((last_id - next_id))
        [ $fcount -eq 0 ] && skip "not enough space on OST0" && return
        [ $fcount -gt $OSTCOUNT ] && fcount=$OSTCOUNT

	local MDS_OSCS=$(do_facet $SINGLEMDS lctl dl |
			 awk '/[oO][sS][cC].*md[ts]/ { print $4 }')
	local OST_DEACTIVE_IDX=-1
	local OSC
	local OSTIDX
	local OST

	for OSC in $MDS_OSCS; do
		OST=$(osc_to_ost $OSC)
		OSTIDX=$(index_from_ostuuid $OST)
		if [ $OST_DEACTIVE_IDX == -1 ]; then
			OST_DEACTIVE_IDX=$OSTIDX
		fi
		if [ $OSTIDX != $OST_DEACTIVE_IDX ]; then
			echo $OSC "is Deactivated:"
			do_facet $SINGLEMDS lctl --device  %$OSC deactivate
		fi
	done

	OSTIDX=$(index_from_ostuuid $OST)
	mkdir -p $DIR/$tdir
	$SETSTRIPE -c 1 $DIR/$tdir      # 1 stripe / file

	for OSC in $MDS_OSCS; do
		OST=$(osc_to_ost $OSC)
		OSTIDX=$(index_from_ostuuid $OST)
		if [ $OSTIDX == $OST_DEACTIVE_IDX ]; then
			echo $OST "is degraded:"
			do_facet ost$((OSTIDX+1)) lctl set_param -n \
						obdfilter.$OST.degraded=1
		fi
	done

	sleep_maxage
	createmany -o $DIR/$tdir/$tfile $fcount

	for OSC in $MDS_OSCS; do
		OST=$(osc_to_ost $OSC)
		OSTIDX=$(index_from_ostuuid $OST)
		if [ $OSTIDX == $OST_DEACTIVE_IDX ]; then
			echo $OST "is recovered from degraded:"
			do_facet ost$((OSTIDX+1)) lctl set_param -n \
						obdfilter.$OST.degraded=0
		else
			do_facet $SINGLEMDS lctl --device %$OSC activate
		fi
	done

	# all osp devices get activated, hence -1 stripe count restored
	local stripecnt=0

	# sleep 2*lod_qos_maxage seconds waiting for lod qos to notice osp
	# devices get activated.
	sleep_maxage
	$SETSTRIPE -c -1 $DIR/$tfile
	stripecnt=$($GETSTRIPE -c $DIR/$tfile)
	rm -f $DIR/$tfile
	[ $stripecnt -ne $OSTCOUNT ] &&
		error "Of $OSTCOUNT OSTs, only $stripecnt is available"
	return 0
}
run_test 27y "create files while OST0 is degraded and the rest inactive"

check_seq_oid()
{
        log "check file $1"

        lmm_count=$($GETSTRIPE -c $1)
        lmm_seq=$($GETSTRIPE -v $1 | awk '/lmm_seq/ { print $2 }')
        lmm_oid=$($GETSTRIPE -v $1 | awk '/lmm_object_id/ { print $2 }')

        local old_ifs="$IFS"
        IFS=$'[:]'
        fid=($($LFS path2fid $1))
        IFS="$old_ifs"

        log "FID seq ${fid[1]}, oid ${fid[2]} ver ${fid[3]}"
        log "LOV seq $lmm_seq, oid $lmm_oid, count: $lmm_count"

        # compare lmm_seq and lu_fid->f_seq
        [ $lmm_seq = ${fid[1]} ] || { error "SEQ mismatch"; return 1; }
        # compare lmm_object_id and lu_fid->oid
        [ $lmm_oid = ${fid[2]} ] || { error "OID mismatch"; return 2; }

        # check the trusted.fid attribute of the OST objects of the file
        local have_obdidx=false
        local stripe_nr=0
        $GETSTRIPE $1 | while read obdidx oid hex seq; do
                # skip lines up to and including "obdidx"
                [ -z "$obdidx" ] && break
                [ "$obdidx" = "obdidx" ] && have_obdidx=true && continue
                $have_obdidx || continue

                local ost=$((obdidx + 1))
                local dev=$(ostdevname $ost)

		if [ $(facet_fstype ost$ost) != ldiskfs ]; then
			echo "Currently only works with ldiskfs-based OSTs"
			continue
		fi

                log "want: stripe:$stripe_nr ost:$obdidx oid:$oid/$hex seq:$seq"

                #don't unmount/remount the OSTs if we don't need to do that
		# LU-2577 changes filter_fid to be smaller, so debugfs needs
		# update too, until that use mount/ll_decode_filter_fid/mount
		local dir=$(facet_mntpt ost$ost)
		local opts=${OST_MOUNT_OPTS}

		if !  do_facet ost$ost test -b ${dev}; then
			opts=$(csa_add "$opts" -o loop)
		fi

		stop ost$ost
		do_facet ost$ost mount -t $(facet_fstype ost$ost) $opts $dev $dir ||
			{ error "mounting $dev as $FSTYPE failed"; return 3; }
		local obj_file=$(do_facet ost$ost find $dir/O/$seq -name $oid)
		local ff=$(do_facet ost$ost $LL_DECODE_FILTER_FID $obj_file)
		do_facet ost$ost umount -d $dir
		start ost$ost $dev $OST_MOUNT_OPTS

		# re-enable when debugfs will understand new filter_fid
		#seq=$(echo $seq | sed -e "s/^0x//g")
		#if [ $seq == 0 ]; then
		#	oid_hex=$(echo $oid)
		#else
		#	oid_hex=$(echo $hex | sed -e "s/^0x//g")
		#fi
                #local obj_file="O/$seq/d$((oid %32))/$oid_hex"
		#local ff=$(do_facet ost$ost "$DEBUGFS -c -R 'stat $obj_file' \
                #           $dev 2>/dev/null" | grep "parent=")

                [ -z "$ff" ] && error "$obj_file: no filter_fid info"

                echo "$ff" | sed -e 's#.*objid=#got: objid=#'

                # /mnt/O/0/d23/23: objid=23 seq=0 parent=[0x200000400:0x1e:0x1]
                # fid: objid=23 seq=0 parent=[0x200000400:0x1e:0x0] stripe=1
                local ff_parent=$(echo $ff|sed -e 's/.*parent=.//')
                local ff_pseq=$(echo $ff_parent | cut -d: -f1)
                local ff_poid=$(echo $ff_parent | cut -d: -f2)
                local ff_pstripe=$(echo $ff_parent | sed -e 's/.*stripe=//')

                # compare lmm_seq and filter_fid->ff_parent.f_seq
                [ $ff_pseq = $lmm_seq ] ||
                        error "FF parent SEQ $ff_pseq != $lmm_seq"
                # compare lmm_object_id and filter_fid->ff_parent.f_oid
                [ $ff_poid = $lmm_oid ] ||
                        error "FF parent OID $ff_poid != $lmm_oid"
                [ $ff_pstripe = $stripe_nr ] ||
                        error "FF stripe $ff_pstripe != $stripe_nr"

                stripe_nr=$((stripe_nr + 1))
        done
}

test_27z() {
        remote_ost_nodsh && skip "remote OST with nodsh" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir

        $SETSTRIPE -c 1 -i 0 -S 64k $DIR/$tdir/$tfile-1 ||
                { error "setstripe -c -1 failed"; return 1; }
        # We need to send a write to every object to get parent FID info set.
        # This _should_ also work for setattr, but does not currently.
        # touch $DIR/$tdir/$tfile-1 ||
        dd if=/dev/zero of=$DIR/$tdir/$tfile-1 bs=1M count=1 ||
                { error "dd $tfile-1 failed"; return 2; }
        $SETSTRIPE -c -1 -i $((OSTCOUNT - 1)) -S 1M $DIR/$tdir/$tfile-2 ||
                { error "setstripe -c -1 failed"; return 3; }
        dd if=/dev/zero of=$DIR/$tdir/$tfile-2 bs=1M count=$OSTCOUNT ||
                { error "dd $tfile-2 failed"; return 4; }

        # make sure write RPCs have been sent to OSTs
        sync; sleep 5; sync

        check_seq_oid $DIR/$tdir/$tfile-1 || return 5
        check_seq_oid $DIR/$tdir/$tfile-2 || return 6
}
run_test 27z "check SEQ/OID on the MDT and OST filesystems"

test_27A() { # b=19102
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        local restore_size=$($GETSTRIPE -S $MOUNT)
        local restore_count=$($GETSTRIPE -c $MOUNT)
        local restore_offset=$($GETSTRIPE -i $MOUNT)
        $SETSTRIPE -c 0 -i -1 -S 0 $MOUNT
        local default_size=$($GETSTRIPE -S $MOUNT)
        local default_count=$($GETSTRIPE -c $MOUNT)
        local default_offset=$($GETSTRIPE -i $MOUNT)
        local dsize=$((1024 * 1024))
        [ $default_size -eq $dsize ] ||
                error "stripe size $default_size != $dsize"
        [ $default_count -eq 1 ] || error "stripe count $default_count != 1"
        [ $default_offset -eq -1 ] ||error "stripe offset $default_offset != -1"
        $SETSTRIPE -c $restore_count -i $restore_offset -S $restore_size $MOUNT
}
run_test 27A "check filesystem-wide default LOV EA values"

test_27B() { # LU-2523
	test_mkdir -p $DIR/$tdir
	rm -f $DIR/$tdir/f0 $DIR/$tdir/f1
	touch $DIR/$tdir/f0
	# open f1 with O_LOV_DELAY_CREATE
	# rename f0 onto f1
	# call setstripe ioctl on open file descriptor for f1
	# close
	multiop $DIR/$tdir/f1 oO_RDWR:O_CREAT:O_LOV_DELAY_CREATE:nB1c \
		$DIR/$tdir/f0

	rm -f $DIR/$tdir/f1
	# open f1 with O_LOV_DELAY_CREATE
	# unlink f1
	# call setstripe ioctl on open file descriptor for f1
	# close
	multiop $DIR/$tdir/f1 oO_RDWR:O_CREAT:O_LOV_DELAY_CREATE:uB1c

	# Allow multiop to fail in imitation of NFS's busted semantics.
	true
}
run_test 27B "call setstripe on open unlinked file/rename victim"

test_27C() { #LU-2871
	[ $OSTCOUNT -lt 2 ] && skip "needs >= 2 OSTs" && return

	declare -a ost_idx
	local index
	local i
	local j

	test_mkdir -p $DIR/$tdir
	cd $DIR/$tdir
	for i in $(seq 0 $((OSTCOUNT - 1))); do
		# set stripe across all OSTs starting from OST$i
		$SETSTRIPE -i $i -c -1 $tfile$i
		# get striping information
		ost_idx=($($GETSTRIPE $tfile$i |
		         tail -n $((OSTCOUNT + 1)) | awk '{print $1}'))
		echo ${ost_idx[@]}
		# check the layout
		for j in $(seq 0 $((OSTCOUNT - 1))); do
			index=$(((i + j) % OSTCOUNT))
			[ ${ost_idx[$j]} -eq $index ] || error
		done
	done
}
run_test 27C "check full striping across all OSTs"

# createtest also checks that device nodes are created and
# then visible correctly (#2091)
test_28() { # bug 2091
	test_mkdir $DIR/d28
	$CREATETEST $DIR/d28/ct || error
}
run_test 28 "create/mknod/mkdir with bad file types ============"

test_29() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	cancel_lru_locks mdc
	test_mkdir $DIR/d29
	touch $DIR/d29/foo
	log 'first d29'
	ls -l $DIR/d29

	declare -i LOCKCOUNTORIG=0
	for lock_count in $(lctl get_param -n ldlm.namespaces.*mdc*.lock_count); do
		let LOCKCOUNTORIG=$LOCKCOUNTORIG+$lock_count
	done
	[ $LOCKCOUNTORIG -eq 0 ] && echo "No mdc lock count" && return 1

	declare -i LOCKUNUSEDCOUNTORIG=0
	for unused_count in $(lctl get_param -n ldlm.namespaces.*mdc*.lock_unused_count); do
		let LOCKUNUSEDCOUNTORIG=$LOCKUNUSEDCOUNTORIG+$unused_count
	done

	log 'second d29'
	ls -l $DIR/d29
	log 'done'

	declare -i LOCKCOUNTCURRENT=0
	for lock_count in $(lctl get_param -n ldlm.namespaces.*mdc*.lock_count); do
		let LOCKCOUNTCURRENT=$LOCKCOUNTCURRENT+$lock_count
	done

	declare -i LOCKUNUSEDCOUNTCURRENT=0
	for unused_count in $(lctl get_param -n ldlm.namespaces.*mdc*.lock_unused_count); do
		let LOCKUNUSEDCOUNTCURRENT=$LOCKUNUSEDCOUNTCURRENT+$unused_count
	done

	if [ "$LOCKCOUNTCURRENT" -gt "$LOCKCOUNTORIG" ]; then
		lctl set_param -n ldlm.dump_namespaces ""
		error "CURRENT: $LOCKCOUNTCURRENT > $LOCKCOUNTORIG"
		$LCTL dk | sort -k4 -t: > $TMP/test_29.dk
		log "dumped log to $TMP/test_29.dk (bug 5793)"
		return 2
	fi
	if [ "$LOCKUNUSEDCOUNTCURRENT" -gt "$LOCKUNUSEDCOUNTORIG" ]; then
		error "UNUSED: $LOCKUNUSEDCOUNTCURRENT > $LOCKUNUSEDCOUNTORIG"
		$LCTL dk | sort -k4 -t: > $TMP/test_29.dk
		log "dumped log to $TMP/test_29.dk (bug 5793)"
		return 3
	fi
}
run_test 29 "IT_GETATTR regression  ============================"

test_30a() { # was test_30
	cp `which ls` $DIR || cp /bin/ls $DIR
	$DIR/ls / || error
	rm $DIR/ls
}
run_test 30a "execute binary from Lustre (execve) =============="

test_30b() {
	cp `which ls` $DIR || cp /bin/ls $DIR
	chmod go+rx $DIR/ls
	$RUNAS $DIR/ls / || error
	rm $DIR/ls
}
run_test 30b "execute binary from Lustre as non-root ==========="

test_30c() { # b=22376
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	cp `which ls` $DIR || cp /bin/ls $DIR
	chmod a-rw $DIR/ls
	cancel_lru_locks mdc
	cancel_lru_locks osc
	$RUNAS $DIR/ls / || error
	rm -f $DIR/ls
}
run_test 30c "execute binary from Lustre without read perms ===="

test_31a() {
	$OPENUNLINK $DIR/f31 $DIR/f31 || error
	$CHECKSTAT -a $DIR/f31 || error
}
run_test 31a "open-unlink file =================================="

test_31b() {
	touch $DIR/f31 || error
	ln $DIR/f31 $DIR/f31b || error
	$MULTIOP $DIR/f31b Ouc || error
	$CHECKSTAT -t file $DIR/f31 || error
}
run_test 31b "unlink file with multiple links while open ======="

test_31c() {
	touch $DIR/f31 || error
	ln $DIR/f31 $DIR/f31c || error
	multiop_bg_pause $DIR/f31 O_uc || return 1
	MULTIPID=$!
	$MULTIOP $DIR/f31c Ouc
	kill -USR1 $MULTIPID
	wait $MULTIPID
}
run_test 31c "open-unlink file with multiple links ============="

test_31d() {
	opendirunlink $DIR/d31d $DIR/d31d || error
	$CHECKSTAT -a $DIR/d31d || error
}
run_test 31d "remove of open directory ========================="

test_31e() { # bug 2904
	check_kernel_version 34 || return 0
	openfilleddirunlink $DIR/d31e || error
}
run_test 31e "remove of open non-empty directory ==============="

test_31f() { # bug 4554
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set -vx
	test_mkdir $DIR/d31f
	$SETSTRIPE -S 1048576 -c 1 $DIR/d31f
	cp /etc/hosts $DIR/d31f
	ls -l $DIR/d31f
	$GETSTRIPE $DIR/d31f/hosts
	multiop_bg_pause $DIR/d31f D_c || return 1
	MULTIPID=$!

	rm -rv $DIR/d31f || error "first of $DIR/d31f"
	test_mkdir $DIR/d31f
	$SETSTRIPE -S 1048576 -c 1 $DIR/d31f
	cp /etc/hosts $DIR/d31f
	ls -l $DIR/d31f
	$GETSTRIPE $DIR/d31f/hosts
	multiop_bg_pause $DIR/d31f D_c || return 1
	MULTIPID2=$!

	kill -USR1 $MULTIPID || error "first opendir $MULTIPID not running"
	wait $MULTIPID || error "first opendir $MULTIPID failed"

	sleep 6

	kill -USR1 $MULTIPID2 || error "second opendir $MULTIPID not running"
	wait $MULTIPID2 || error "second opendir $MULTIPID2 failed"
	set +vx
}
run_test 31f "remove of open directory with open-unlink file ==="

test_31g() {
        echo "-- cross directory link --"
        test_mkdir $DIR/d31ga
        test_mkdir $DIR/d31gb
        touch $DIR/d31ga/f
        ln $DIR/d31ga/f $DIR/d31gb/g
        $CHECKSTAT -t file $DIR/d31ga/f || error "source"
        [ `stat -c%h $DIR/d31ga/f` == '2' ] || error "source nlink"
        $CHECKSTAT -t file $DIR/d31gb/g || error "target"
        [ `stat -c%h $DIR/d31gb/g` == '2' ] || error "target nlink"
}
run_test 31g "cross directory link==============="

test_31h() {
        echo "-- cross directory link --"
        test_mkdir $DIR/d31h
        test_mkdir $DIR/d31h/dir
        touch $DIR/d31h/f
        ln $DIR/d31h/f $DIR/d31h/dir/g
        $CHECKSTAT -t file $DIR/d31h/f || error "source"
        [ `stat -c%h $DIR/d31h/f` == '2' ] || error "source nlink"
        $CHECKSTAT -t file $DIR/d31h/dir/g || error "target"
        [ `stat -c%h $DIR/d31h/dir/g` == '2' ] || error "target nlink"
}
run_test 31h "cross directory link under child==============="

test_31i() {
        echo "-- cross directory link --"
        test_mkdir $DIR/d31i
        test_mkdir $DIR/d31i/dir
        touch $DIR/d31i/dir/f
        ln $DIR/d31i/dir/f $DIR/d31i/g
        $CHECKSTAT -t file $DIR/d31i/dir/f || error "source"
        [ `stat -c%h $DIR/d31i/dir/f` == '2' ] || error "source nlink"
        $CHECKSTAT -t file $DIR/d31i/g || error "target"
        [ `stat -c%h $DIR/d31i/g` == '2' ] || error "target nlink"
}
run_test 31i "cross directory link under parent==============="


test_31j() {
        test_mkdir $DIR/d31j
        test_mkdir $DIR/d31j/dir1
        ln $DIR/d31j/dir1 $DIR/d31j/dir2 && error "ln for dir"
        link $DIR/d31j/dir1 $DIR/d31j/dir3 && error "link for dir"
        mlink $DIR/d31j/dir1 $DIR/d31j/dir4 && error "mlink for dir"
        mlink $DIR/d31j/dir1 $DIR/d31j/dir1 && error "mlink to the same dir"
	return 0
}
run_test 31j "link for directory==============="


test_31k() {
        test_mkdir $DIR/d31k
        touch $DIR/d31k/s
        touch $DIR/d31k/exist
        mlink $DIR/d31k/s $DIR/d31k/t || error "mlink"
        mlink $DIR/d31k/s $DIR/d31k/exist && error "mlink to exist file"
        mlink $DIR/d31k/s $DIR/d31k/s && error "mlink to the same file"
        mlink $DIR/d31k/s $DIR/d31k && error "mlink to parent dir"
        mlink $DIR/d31k $DIR/d31k/s && error "mlink parent dir to target"
        mlink $DIR/d31k/not-exist $DIR/d31k/foo && error "mlink non-existing to new"
        mlink $DIR/d31k/not-exist $DIR/d31k/s && error "mlink non-existing to exist"
	return 0
}
run_test 31k "link to file: the same, non-existing, dir==============="

test_31m() {
        test_mkdir $DIR/d31m
        touch $DIR/d31m/s
        test_mkdir $DIR/d31m2
        touch $DIR/d31m2/exist
        mlink $DIR/d31m/s $DIR/d31m2/t || error "mlink"
        mlink $DIR/d31m/s $DIR/d31m2/exist && error "mlink to exist file"
        mlink $DIR/d31m/s $DIR/d31m2 && error "mlink to parent dir"
        mlink $DIR/d31m2 $DIR/d31m/s && error "mlink parent dir to target"
        mlink $DIR/d31m/not-exist $DIR/d31m2/foo && error "mlink non-existing to new"
        mlink $DIR/d31m/not-exist $DIR/d31m2/s && error "mlink non-existing to exist"
	return 0
}
run_test 31m "link to file: the same, non-existing, dir==============="

test_31n() {
	[ -e /proc/self/fd/173 ] && echo "skipping, fd 173 is in use" && return
	touch $DIR/$tfile || error "cannot create '$DIR/$tfile'"
	nlink=$(stat --format=%h $DIR/$tfile)
	[ ${nlink:--1} -eq 1 ] || error "nlink is $nlink, expected 1"
	exec 173<$DIR/$tfile
	trap "exec 173<&-" EXIT
	nlink=$(stat --dereference --format=%h /proc/self/fd/173)
	[ ${nlink:--1} -eq 1 ] || error "nlink is $nlink, expected 1"
	rm $DIR/$tfile || error "cannot remove '$DIR/$tfile'"
	nlink=$(stat --dereference --format=%h /proc/self/fd/173)
	[ ${nlink:--1} -eq 0 ] || error "nlink is $nlink, expected 0"
	exec 173<&-
}
run_test 31n "check link count of unlinked file"

cleanup_test32_mount() {
	trap 0
	$UMOUNT $DIR/$tdir/ext2-mountpoint
}

test_32a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	echo "== more mountpoints and symlinks ================="
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint || error
	$CHECKSTAT -t dir $DIR/$tdir/ext2-mountpoint/.. || error
	cleanup_test32_mount
}
run_test 32a "stat d32a/ext2-mountpoint/.. ====================="

test_32b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint || error
	ls -al $DIR/$tdir/ext2-mountpoint/.. || error
	cleanup_test32_mount
}
run_test 32b "open d32b/ext2-mountpoint/.. ====================="

test_32c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint || error
	test_mkdir -p $DIR/$tdir/d2/test_dir
	$CHECKSTAT -t dir $DIR/$tdir/ext2-mountpoint/../d2/test_dir || error
	cleanup_test32_mount
}
run_test 32c "stat d32c/ext2-mountpoint/../d2/test_dir ========="

test_32d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint || error
	test_mkdir -p $DIR/$tdir/d2/test_dir
	ls -al $DIR/$tdir/ext2-mountpoint/../d2/test_dir || error
	cleanup_test32_mount
}
run_test 32d "open d32d/ext2-mountpoint/../d2/test_dir ========="

test_32e() {
	[ -e $DIR/d32e ] && rm -fr $DIR/d32e
	test_mkdir -p $DIR/d32e/tmp
	TMP_DIR=$DIR/d32e/tmp
	ln -s $DIR/d32e $TMP_DIR/symlink11
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01
	$CHECKSTAT -t link $DIR/d32e/tmp/symlink11 || error
	$CHECKSTAT -t link $DIR/d32e/symlink01 || error
}
run_test 32e "stat d32e/symlink->tmp/symlink->lustre-subdir ===="

test_32f() {
	[ -e $DIR/d32f ] && rm -fr $DIR/d32f
	test_mkdir -p $DIR/d32f/tmp
	TMP_DIR=$DIR/d32f/tmp
	ln -s $DIR/d32f $TMP_DIR/symlink11
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01
	ls $DIR/d32f/tmp/symlink11  || error
	ls $DIR/d32f/symlink01 || error
}
run_test 32f "open d32f/symlink->tmp/symlink->lustre-subdir ===="

test_32g() {
	TMP_DIR=$DIR/$tdir/tmp
	test_mkdir -p $DIR/$tdir/tmp
	test_mkdir $DIR/${tdir}2
	ln -s $DIR/${tdir}2 $TMP_DIR/symlink12
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02
	$CHECKSTAT -t link $TMP_DIR/symlink12 || error
	$CHECKSTAT -t link $DIR/$tdir/symlink02 || error
	$CHECKSTAT -t dir -f $TMP_DIR/symlink12 || error
	$CHECKSTAT -t dir -f $DIR/$tdir/symlink02 || error
}
run_test 32g "stat d32g/symlink->tmp/symlink->lustre-subdir/${tdir}2"

test_32h() {
	rm -fr $DIR/$tdir $DIR/${tdir}2
	TMP_DIR=$DIR/$tdir/tmp
	test_mkdir -p $DIR/$tdir/tmp
	test_mkdir $DIR/${tdir}2
	ln -s $DIR/${tdir}2 $TMP_DIR/symlink12
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02
	ls $TMP_DIR/symlink12 || error
	ls $DIR/$tdir/symlink02  || error
}
run_test 32h "open d32h/symlink->tmp/symlink->lustre-subdir/${tdir}2"

test_32i() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint || error
	touch $DIR/$tdir/test_file
	$CHECKSTAT -t file $DIR/$tdir/ext2-mountpoint/../test_file || error
	cleanup_test32_mount
}
run_test 32i "stat d32i/ext2-mountpoint/../test_file ==========="

test_32j() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint || error
	touch $DIR/$tdir/test_file
	cat $DIR/$tdir/ext2-mountpoint/../test_file || error
	cleanup_test32_mount
}
run_test 32j "open d32j/ext2-mountpoint/../test_file ==========="

test_32k() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint
	test_mkdir -p $DIR/$tdir/d2
	touch $DIR/$tdir/d2/test_file || error
	$CHECKSTAT -t file $DIR/$tdir/ext2-mountpoint/../d2/test_file || error
	cleanup_test32_mount
}
run_test 32k "stat d32k/ext2-mountpoint/../d2/test_file ========"

test_32l() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint || error
	test_mkdir -p $DIR/$tdir/d2
	touch $DIR/$tdir/d2/test_file
	cat  $DIR/$tdir/ext2-mountpoint/../d2/test_file || error
	cleanup_test32_mount
}
run_test 32l "open d32l/ext2-mountpoint/../d2/test_file ========"

test_32m() {
	rm -fr $DIR/d32m
	test_mkdir -p $DIR/d32m/tmp
	TMP_DIR=$DIR/d32m/tmp
	ln -s $DIR $TMP_DIR/symlink11
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01
	$CHECKSTAT -t link $DIR/d32m/tmp/symlink11 || error
	$CHECKSTAT -t link $DIR/d32m/symlink01 || error
}
run_test 32m "stat d32m/symlink->tmp/symlink->lustre-root ======"

test_32n() {
	rm -fr $DIR/d32n
	test_mkdir -p $DIR/d32n/tmp
	TMP_DIR=$DIR/d32n/tmp
	ln -s $DIR $TMP_DIR/symlink11
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01
	ls -l $DIR/d32n/tmp/symlink11  || error
	ls -l $DIR/d32n/symlink01 || error
}
run_test 32n "open d32n/symlink->tmp/symlink->lustre-root ======"

test_32o() {
	rm -fr $DIR/d32o $DIR/$tfile
	touch $DIR/$tfile
	test_mkdir -p $DIR/d32o/tmp
	TMP_DIR=$DIR/d32o/tmp
	ln -s $DIR/$tfile $TMP_DIR/symlink12
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02
	$CHECKSTAT -t link $DIR/d32o/tmp/symlink12 || error
	$CHECKSTAT -t link $DIR/d32o/symlink02 || error
	$CHECKSTAT -t file -f $DIR/d32o/tmp/symlink12 || error
	$CHECKSTAT -t file -f $DIR/d32o/symlink02 || error
}
run_test 32o "stat d32o/symlink->tmp/symlink->lustre-root/$tfile"

test_32p() {
    log 32p_1
	rm -fr $DIR/d32p
    log 32p_2
	rm -f $DIR/$tfile
    log 32p_3
	touch $DIR/$tfile
    log 32p_4
	test_mkdir -p $DIR/d32p/tmp
    log 32p_5
	TMP_DIR=$DIR/d32p/tmp
    log 32p_6
	ln -s $DIR/$tfile $TMP_DIR/symlink12
    log 32p_7
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02
    log 32p_8
	cat $DIR/d32p/tmp/symlink12 || error
    log 32p_9
	cat $DIR/d32p/symlink02 || error
    log 32p_10
}
run_test 32p "open d32p/symlink->tmp/symlink->lustre-root/$tfile"

cleanup_testdir_mount() {
	trap 0
	$UMOUNT $DIR/$tdir
}

test_32q() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_testdir_mount EXIT
	test_mkdir -p $DIR/$tdir
        touch $DIR/$tdir/under_the_mount
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir
	ls $DIR/$tdir | grep "\<under_the_mount\>" && error
	cleanup_testdir_mount
}
run_test 32q "stat follows mountpoints in Lustre (should return error)"

test_32r() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_testdir_mount EXIT
	test_mkdir -p $DIR/$tdir
        touch $DIR/$tdir/under_the_mount
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir
	ls $DIR/$tdir | grep -q under_the_mount && error || true
	cleanup_testdir_mount
}
run_test 32r "opendir follows mountpoints in Lustre (should return error)"

test_33aa() {
	rm -f $DIR/$tfile
	touch $DIR/$tfile
	chmod 444 $DIR/$tfile
	chown $RUNAS_ID $DIR/$tfile
	log 33_1
	$RUNAS $OPENFILE -f O_RDWR $DIR/$tfile && error || true
	log 33_2
}
run_test 33aa "write file with mode 444 (should return error) ===="

test_33a() {
        rm -fr $DIR/d33
        test_mkdir -p $DIR/d33
        chown $RUNAS_ID $DIR/d33
        $RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $DIR/d33/f33|| error "create"
        $RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $DIR/d33/f33 && \
		error "open RDWR" || true
}
run_test 33a "test open file(mode=0444) with O_RDWR (should return error)"

test_33b() {
        rm -fr $DIR/d33
        test_mkdir -p $DIR/d33
        chown $RUNAS_ID $DIR/d33
        $RUNAS $OPENFILE -f 1286739555 $DIR/d33/f33 && error "create" || true
}
run_test 33b "test open file with malformed flags (No panic and return error)"

test_33c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        local ostnum
        local ostname
        local write_bytes
        local all_zeros

        remote_ost_nodsh && skip "remote OST with nodsh" && return
        all_zeros=:
        rm -fr $DIR/d33
        test_mkdir -p $DIR/d33
        # Read: 0, Write: 4, create/destroy: 2/0, stat: 1, punch: 0

        sync
        for ostnum in $(seq $OSTCOUNT); do
                # test-framework's OST numbering is one-based, while Lustre's
                # is zero-based
                ostname=$(printf "$FSNAME-OST%.4d" $((ostnum - 1)))
                # Parsing llobdstat's output sucks; we could grep the /proc
                # path, but that's likely to not be as portable as using the
                # llobdstat utility.  So we parse lctl output instead.
                write_bytes=$(do_facet ost$ostnum lctl get_param -n \
                        obdfilter/$ostname/stats |
                        awk '/^write_bytes/ {print $7}' )
                echo "baseline_write_bytes@$OSTnum/$ostname=$write_bytes"
                if (( ${write_bytes:-0} > 0 ))
                then
                        all_zeros=false
                        break;
                fi
        done

        $all_zeros || return 0

        # Write four bytes
        echo foo > $DIR/d33/bar
        # Really write them
        sync

        # Total up write_bytes after writing.  We'd better find non-zeros.
        for ostnum in $(seq $OSTCOUNT); do
                ostname=$(printf "$FSNAME-OST%.4d" $((ostnum - 1)))
                write_bytes=$(do_facet ost$ostnum lctl get_param -n \
                        obdfilter/$ostname/stats |
                        awk '/^write_bytes/ {print $7}' )
                echo "write_bytes@$OSTnum/$ostname=$write_bytes"
                if (( ${write_bytes:-0} > 0 ))
                then
                        all_zeros=false
                        break;
                fi
        done

        if $all_zeros
        then
                for ostnum in $(seq $OSTCOUNT); do
                        ostname=$(printf "$FSNAME-OST%.4d" $((ostnum - 1)))
                        echo "Check that write_bytes is present in obdfilter/*/stats:"
                        do_facet ost$ostnum lctl get_param -n \
                                obdfilter/$ostname/stats
                done
                error "OST not keeping write_bytes stats (b22312)"
        fi
}
run_test 33c "test llobdstat and write_bytes"

test_33d() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX $remote_dir ||
		error "create remote directory failed"

	touch $remote_dir/$tfile
	chmod 444 $remote_dir/$tfile
	chown $RUNAS_ID $remote_dir/$tfile

	$RUNAS $OPENFILE -f O_RDWR $DIR/$tfile && error || true

	chown $RUNAS_ID $remote_dir
	$RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $remote_dir/f33 ||
					error "create" || true
	$RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $remote_dir/f33 &&
				    error "open RDWR" || true
	$RUNAS $OPENFILE -f 1286739555 $remote_dir/f33 &&
				    error "create" || true
}
run_test 33d "openfile with 444 modes and malformed flags under remote dir"

TEST_34_SIZE=${TEST_34_SIZE:-2000000000000}
test_34a() {
	rm -f $DIR/f34
	$MCREATE $DIR/f34 || error
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$TRUNCATE $DIR/f34 $TEST_34_SIZE || error
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
}
run_test 34a "truncate file that has not been opened ==========="

test_34b() {
	[ ! -f $DIR/f34 ] && test_34a
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
	$OPENFILE -f O_RDONLY $DIR/f34
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
}
run_test 34b "O_RDONLY opening file doesn't create objects ====="

test_34c() {
	[ ! -f $DIR/f34 ] && test_34a
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
	$OPENFILE -f O_RDWR $DIR/f34
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" && error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
}
run_test 34c "O_RDWR opening file-with-size works =============="

test_34d() {
	[ ! -f $DIR/f34 ] && test_34a
	dd if=/dev/zero of=$DIR/f34 conv=notrunc bs=4k count=1 || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 || error
	rm $DIR/f34
}
run_test 34d "write to sparse file ============================="

test_34e() {
	rm -f $DIR/f34e
	$MCREATE $DIR/f34e || error
	$TRUNCATE $DIR/f34e 1000 || error
	$CHECKSTAT -s 1000 $DIR/f34e || error
	$OPENFILE -f O_RDWR $DIR/f34e
	$CHECKSTAT -s 1000 $DIR/f34e || error
}
run_test 34e "create objects, some with size and some without =="

test_34f() { # bug 6242, 6243
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	SIZE34F=48000
	rm -f $DIR/f34f
	$MCREATE $DIR/f34f || error
	$TRUNCATE $DIR/f34f $SIZE34F || error "truncating $DIR/f3f to $SIZE34F"
	dd if=$DIR/f34f of=$TMP/f34f
	$CHECKSTAT -s $SIZE34F $TMP/f34f || error "$TMP/f34f not $SIZE34F bytes"
	dd if=/dev/zero of=$TMP/f34fzero bs=$SIZE34F count=1
	cmp $DIR/f34f $TMP/f34fzero || error "$DIR/f34f not all zero"
	cmp $TMP/f34f $TMP/f34fzero || error "$TMP/f34f not all zero"
	rm $TMP/f34f $TMP/f34fzero $DIR/f34f
}
run_test 34f "read from a file with no objects until EOF ======="

test_34g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	dd if=/dev/zero of=$DIR/$tfile bs=1 count=100 seek=$TEST_34_SIZE || error
	$TRUNCATE $DIR/$tfile $((TEST_34_SIZE / 2))|| error
	$CHECKSTAT -s $((TEST_34_SIZE / 2)) $DIR/$tfile || error "truncate failed"
	cancel_lru_locks osc
	$CHECKSTAT -s $((TEST_34_SIZE / 2)) $DIR/$tfile || \
		error "wrong size after lock cancel"

	$TRUNCATE $DIR/$tfile $TEST_34_SIZE || error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/$tfile || \
		error "expanding truncate failed"
	cancel_lru_locks osc
	$CHECKSTAT -s $TEST_34_SIZE $DIR/$tfile || \
		error "wrong expanded size after lock cancel"
}
run_test 34g "truncate long file ==============================="

test_34h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local gid=10
	local sz=1000

	dd if=/dev/zero of=$DIR/$tfile bs=1M count=10 || error
	sync # Flush the cache so that multiop below does not block on cache
	     # flush when getting the group lock
	$MULTIOP $DIR/$tfile OG${gid}T${sz}g${gid}c &
	MULTIPID=$!
	sleep 2

	if [[ `ps h -o comm -p $MULTIPID` == "multiop" ]]; then
		error "Multiop blocked on ftruncate, pid=$MULTIPID"
		kill -9 $MULTIPID
	fi
	wait $MULTIPID
	local nsz=`stat -c %s $DIR/$tfile`
	[[ $nsz == $sz ]] || error "New size wrong $nsz != $sz"
}
run_test 34h "ftruncate file under grouplock should not block"

test_35a() {
	cp /bin/sh $DIR/f35a
	chmod 444 $DIR/f35a
	chown $RUNAS_ID $DIR/f35a
	$RUNAS $DIR/f35a && error || true
	rm $DIR/f35a
}
run_test 35a "exec file with mode 444 (should return and not leak) ====="

test_36a() {
	rm -f $DIR/f36
	utime $DIR/f36 || error
}
run_test 36a "MDS utime check (mknod, utime) ==================="

test_36b() {
	echo "" > $DIR/f36
	utime $DIR/f36 || error
}
run_test 36b "OST utime check (open, utime) ===================="

test_36c() {
	rm -f $DIR/d36/f36
	test_mkdir $DIR/d36
	chown $RUNAS_ID $DIR/d36
	$RUNAS utime $DIR/d36/f36 || error
}
run_test 36c "non-root MDS utime check (mknod, utime) =========="

test_36d() {
	[ ! -d $DIR/d36 ] && test_36c
	echo "" > $DIR/d36/f36
	$RUNAS utime $DIR/d36/f36 || error
}
run_test 36d "non-root OST utime check (open, utime) ==========="

test_36e() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID -- skipping" && return
	test_mkdir -p $DIR/$tdir
	touch $DIR/$tdir/$tfile
	$RUNAS utime $DIR/$tdir/$tfile && \
		error "utime worked, expected failure" || true
}
run_test 36e "utime on non-owned file (should return error) ===="

subr_36fh() {
	local fl="$1"
	local LANG_SAVE=$LANG
	local LC_LANG_SAVE=$LC_LANG
	export LANG=C LC_LANG=C # for date language

	DATESTR="Dec 20  2000"
	test_mkdir -p $DIR/$tdir
	lctl set_param fail_loc=$fl
	date; date +%s
	cp /etc/hosts $DIR/$tdir/$tfile
	sync & # write RPC generated with "current" inode timestamp, but delayed
	sleep 1
	touch --date="$DATESTR" $DIR/$tdir/$tfile # setattr timestamp in past
	LS_BEFORE="`ls -l $DIR/$tdir/$tfile`" # old timestamp from client cache
	cancel_lru_locks osc
	LS_AFTER="`ls -l $DIR/$tdir/$tfile`"  # timestamp from OST object
	date; date +%s
	[ "$LS_BEFORE" != "$LS_AFTER" ] && \
		echo "BEFORE: $LS_BEFORE" && \
		echo "AFTER : $LS_AFTER" && \
		echo "WANT  : $DATESTR" && \
		error "$DIR/$tdir/$tfile timestamps changed" || true

	export LANG=$LANG_SAVE LC_LANG=$LC_LANG_SAVE
}

test_36f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	#define OBD_FAIL_OST_BRW_PAUSE_BULK 0x214
	subr_36fh "0x80000214"
}
run_test 36f "utime on file racing with OST BRW write =========="

test_36g() {
	remote_ost_nodsh && skip "remote OST with nodsh" && return
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local fmd_max_age
	local fmd_before
	local fmd_after

	test_mkdir -p $DIR/$tdir
	fmd_max_age=$(do_facet ost1 \
		"lctl get_param -n obdfilter.*.client_cache_seconds 2> /dev/null | \
		head -n 1")

	fmd_before=$(do_facet ost1 \
		"awk '/ll_fmd_cache/ {print \\\$2}' /proc/slabinfo")
	touch $DIR/$tdir/$tfile
	sleep $((fmd_max_age + 12))
	fmd_after=$(do_facet ost1 \
		"awk '/ll_fmd_cache/ {print \\\$2}' /proc/slabinfo")

	echo "fmd_before: $fmd_before"
	echo "fmd_after: $fmd_after"
	[ "$fmd_after" -gt "$fmd_before" ] && \
		echo "AFTER: $fmd_after > BEFORE: $fmd_before" && \
		error "fmd didn't expire after ping" || true
}
run_test 36g "filter mod data cache expiry ====================="

test_36h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	#define OBD_FAIL_OST_BRW_PAUSE_BULK2 0x227
	subr_36fh "0x80000227"
}
run_test 36h "utime on file racing with OST BRW write =========="

# test_37 - duplicate with tests 32q 32r

test_38() {
	local file=$DIR/$tfile
	touch $file
	openfile -f O_DIRECTORY $file
	local RC=$?
	local ENOTDIR=20
	[ $RC -eq 0 ] && error "opened file $file with O_DIRECTORY" || true
	[ $RC -eq $ENOTDIR ] || error "error $RC should be ENOTDIR ($ENOTDIR)"
}
run_test 38 "open a regular file with O_DIRECTORY should return -ENOTDIR ==="

test_39() {
	touch $DIR/$tfile
	touch $DIR/${tfile}2
#	ls -l  $DIR/$tfile $DIR/${tfile}2
#	ls -lu  $DIR/$tfile $DIR/${tfile}2
#	ls -lc  $DIR/$tfile $DIR/${tfile}2
	sleep 2
	$OPENFILE -f O_CREAT:O_TRUNC:O_WRONLY $DIR/${tfile}2
	if [ ! $DIR/${tfile}2 -nt $DIR/$tfile ]; then
		echo "mtime"
		ls -l --full-time $DIR/$tfile $DIR/${tfile}2
		echo "atime"
		ls -lu --full-time $DIR/$tfile $DIR/${tfile}2
		echo "ctime"
		ls -lc --full-time $DIR/$tfile $DIR/${tfile}2
		error "O_TRUNC didn't change timestamps"
	fi
}
run_test 39 "mtime changed on create ==========================="

test_39b() {
	test_mkdir -p $DIR/$tdir
	cp -p /etc/passwd $DIR/$tdir/fopen
	cp -p /etc/passwd $DIR/$tdir/flink
	cp -p /etc/passwd $DIR/$tdir/funlink
	cp -p /etc/passwd $DIR/$tdir/frename
	ln $DIR/$tdir/funlink $DIR/$tdir/funlink2

	sleep 1
	echo "aaaaaa" >> $DIR/$tdir/fopen
	echo "aaaaaa" >> $DIR/$tdir/flink
	echo "aaaaaa" >> $DIR/$tdir/funlink
	echo "aaaaaa" >> $DIR/$tdir/frename

	local open_new=`stat -c %Y $DIR/$tdir/fopen`
	local link_new=`stat -c %Y $DIR/$tdir/flink`
	local unlink_new=`stat -c %Y $DIR/$tdir/funlink`
	local rename_new=`stat -c %Y $DIR/$tdir/frename`

	cat $DIR/$tdir/fopen > /dev/null
	ln $DIR/$tdir/flink $DIR/$tdir/flink2
	rm -f $DIR/$tdir/funlink2
	mv -f $DIR/$tdir/frename $DIR/$tdir/frename2

	for (( i=0; i < 2; i++ )) ; do
		local open_new2=`stat -c %Y $DIR/$tdir/fopen`
		local link_new2=`stat -c %Y $DIR/$tdir/flink`
		local unlink_new2=`stat -c %Y $DIR/$tdir/funlink`
		local rename_new2=`stat -c %Y $DIR/$tdir/frename2`

		[ $open_new2 -eq $open_new ] || error "open file reverses mtime"
		[ $link_new2 -eq $link_new ] || error "link file reverses mtime"
		[ $unlink_new2 -eq $unlink_new ] || error "unlink file reverses mtime"
		[ $rename_new2 -eq $rename_new ] || error "rename file reverses mtime"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39b "mtime change on open, link, unlink, rename  ======"

# this should be set to past
TEST_39_MTIME=`date -d "1 year ago" +%s`

# bug 11063
test_39c() {
	touch $DIR1/$tfile
	sleep 2
	local mtime0=`stat -c %Y $DIR1/$tfile`

	touch -m -d @$TEST_39_MTIME $DIR1/$tfile
	local mtime1=`stat -c %Y $DIR1/$tfile`
	[ "$mtime1" = $TEST_39_MTIME ] || \
		error "mtime is not set to past: $mtime1, should be $TEST_39_MTIME"

	local d1=`date +%s`
	echo hello >> $DIR1/$tfile
	local d2=`date +%s`
	local mtime2=`stat -c %Y $DIR1/$tfile`
	[ "$mtime2" -ge "$d1" ] && [ "$mtime2" -le "$d2" ] || \
		error "mtime is not updated on write: $d1 <= $mtime2 <= $d2"

	mv $DIR1/$tfile $DIR1/$tfile-1

	for (( i=0; i < 2; i++ )) ; do
		local mtime3=`stat -c %Y $DIR1/$tfile-1`
		[ "$mtime2" = "$mtime3" ] || \
			error "mtime ($mtime2) changed (to $mtime3) on rename"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39c "mtime change on rename ==========================="

# bug 21114
test_39d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	touch $DIR1/$tfile

	touch -m -d @$TEST_39_MTIME $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local mtime=`stat -c %Y $DIR1/$tfile`
		[ $mtime = $TEST_39_MTIME ] || \
			error "mtime($mtime) is not set to $TEST_39_MTIME"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39d "create, utime, stat =============================="

# bug 21114
test_39e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	touch $DIR1/$tfile
	local mtime1=`stat -c %Y $DIR1/$tfile`

	touch -m -d @$TEST_39_MTIME $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile`
		[ $mtime2 = $TEST_39_MTIME ] || \
			error "mtime($mtime2) is not set to $TEST_39_MTIME"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39e "create, stat, utime, stat ========================"

# bug 21114
test_39f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	touch $DIR1/$tfile
	mtime1=`stat -c %Y $DIR1/$tfile`

	sleep 2
	touch -m -d @$TEST_39_MTIME $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile`
		[ $mtime2 = $TEST_39_MTIME ] || \
			error "mtime($mtime2) is not set to $TEST_39_MTIME"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39f "create, stat, sleep, utime, stat ================="

# bug 11063
test_39g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	echo hello >> $DIR1/$tfile
	local mtime1=`stat -c %Y $DIR1/$tfile`

	sleep 2
	chmod o+r $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile`
		[ "$mtime1" = "$mtime2" ] || \
			error "lost mtime: $mtime2, should be $mtime1"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39g "write, chmod, stat ==============================="

# bug 11063
test_39h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	touch $DIR1/$tfile
	sleep 1

	local d1=`date`
	echo hello >> $DIR1/$tfile
	local mtime1=`stat -c %Y $DIR1/$tfile`

	touch -m -d @$TEST_39_MTIME $DIR1/$tfile
	local d2=`date`
	if [ "$d1" != "$d2" ]; then
		echo "write and touch not within one second"
	else
		for (( i=0; i < 2; i++ )) ; do
			local mtime2=`stat -c %Y $DIR1/$tfile`
			[ "$mtime2" = $TEST_39_MTIME ] || \
				error "lost mtime: $mtime2, should be $TEST_39_MTIME"

			cancel_lru_locks osc
			if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
		done
	fi
}
run_test 39h "write, utime within one second, stat ============="

test_39i() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	touch $DIR1/$tfile
	sleep 1

	echo hello >> $DIR1/$tfile
	local mtime1=`stat -c %Y $DIR1/$tfile`

	mv $DIR1/$tfile $DIR1/$tfile-1

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile-1`

		[ "$mtime1" = "$mtime2" ] || \
			error "lost mtime: $mtime2, should be $mtime1"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39i "write, rename, stat =============================="

test_39j() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	start_full_debug_logging
	touch $DIR1/$tfile
	sleep 1

	#define OBD_FAIL_OSC_DELAY_SETTIME	 0x412
	lctl set_param fail_loc=0x80000412
	multiop_bg_pause $DIR1/$tfile oO_RDWR:w2097152_c ||
		error "multiop failed"
	local multipid=$!
	local mtime1=`stat -c %Y $DIR1/$tfile`

	mv $DIR1/$tfile $DIR1/$tfile-1

	kill -USR1 $multipid
	wait $multipid || error "multiop close failed"

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile-1`
		[ "$mtime1" = "$mtime2" ] ||
			error "mtime is lost on close: $mtime2, " \
			      "should be $mtime1"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
	lctl set_param fail_loc=0
	stop_full_debug_logging
}
run_test 39j "write, rename, close, stat ======================="

test_39k() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	touch $DIR1/$tfile
	sleep 1

	multiop_bg_pause $DIR1/$tfile oO_RDWR:w2097152_c || error "multiop failed"
	local multipid=$!
	local mtime1=`stat -c %Y $DIR1/$tfile`

	touch -m -d @$TEST_39_MTIME $DIR1/$tfile

	kill -USR1 $multipid
	wait $multipid || error "multiop close failed"

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile`

		[ "$mtime2" = $TEST_39_MTIME ] || \
			error "mtime is lost on close: $mtime2, should be $TEST_39_MTIME"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39k "write, utime, close, stat ========================"

# this should be set to future
TEST_39_ATIME=`date -d "1 year" +%s`

test_39l() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	local atime_diff=$(do_facet $SINGLEMDS \
				lctl get_param -n mdd.*MDT0000*.atime_diff)
	rm -rf $DIR/$tdir
	mkdir -p $DIR/$tdir

	# test setting directory atime to future
	touch -a -d @$TEST_39_ATIME $DIR/$tdir
	local atime=$(stat -c %X $DIR/$tdir)
	[ "$atime" = $TEST_39_ATIME ] || \
		error "atime is not set to future: $atime, $TEST_39_ATIME"

	# test setting directory atime from future to now
	local d1=$(date +%s)
	ls $DIR/$tdir
	local d2=$(date +%s)

	cancel_lru_locks mdc
	atime=$(stat -c %X $DIR/$tdir)
	[ "$atime" -ge "$d1" -a "$atime" -le "$d2" ] || \
		error "atime is not updated from future: $atime, $d1<atime<$d2"

	do_facet $SINGLEMDS lctl set_param -n mdd.*MDT0000*.atime_diff=2
	sleep 3

	# test setting directory atime when now > dir atime + atime_diff
	d1=$(date +%s)
	ls $DIR/$tdir
	d2=$(date +%s)
	cancel_lru_locks mdc
	atime=$(stat -c %X $DIR/$tdir)
	[ "$atime" -ge "$d1" -a "$atime" -le "$d2" ] || \
		error "atime is not updated  : $atime, should be $d2"

	do_facet $SINGLEMDS lctl set_param -n mdd.*MDT0000*.atime_diff=60
	sleep 3

	# test not setting directory atime when now < dir atime + atime_diff
	ls $DIR/$tdir
	cancel_lru_locks mdc
	atime=$(stat -c %X $DIR/$tdir)
	[ "$atime" -ge "$d1" -a "$atime" -le "$d2" ] || \
		error "atime is updated to $atime, should remain $d1<atime<$d2"

	do_facet $SINGLEMDS \
		lctl set_param -n mdd.*MDT0000*.atime_diff=$atime_diff
}
run_test 39l "directory atime update ==========================="

test_39m() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	touch $DIR1/$tfile
	sleep 2
	local far_past_mtime=$(date -d "May 29 1953" +%s)
	local far_past_atime=$(date -d "Dec 17 1903" +%s)

	touch -m -d @$far_past_mtime $DIR1/$tfile
	touch -a -d @$far_past_atime $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local timestamps=$(stat -c "%X %Y" $DIR1/$tfile)
		[ "$timestamps" = "$far_past_atime $far_past_mtime" ] || \
			error "atime or mtime set incorrectly"

		cancel_lru_locks osc
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39m "test atime and mtime before 1970"

test_40() {
	dd if=/dev/zero of=$DIR/f40 bs=4096 count=1
	$RUNAS $OPENFILE -f O_WRONLY:O_TRUNC $DIR/f40 && error
	$CHECKSTAT -t file -s 4096 $DIR/f40 || error
}
run_test 40 "failed open(O_TRUNC) doesn't truncate ============="

test_41() {
	# bug 1553
	small_write $DIR/f41 18
}
run_test 41 "test small file write + fstat ====================="

count_ost_writes() {
        lctl get_param -n osc.*.stats |
            awk -vwrites=0 '/ost_write/ { writes += $2 } END { print writes; }'
}

# decent default
WRITEBACK_SAVE=500
DIRTY_RATIO_SAVE=40
MAX_DIRTY_RATIO=50
BG_DIRTY_RATIO_SAVE=10
MAX_BG_DIRTY_RATIO=25

start_writeback() {
	trap 0
	# in 2.6, restore /proc/sys/vm/dirty_writeback_centisecs,
	# dirty_ratio, dirty_background_ratio
	if [ -f /proc/sys/vm/dirty_writeback_centisecs ]; then
		sysctl -w vm.dirty_writeback_centisecs=$WRITEBACK_SAVE
		sysctl -w vm.dirty_background_ratio=$BG_DIRTY_RATIO_SAVE
		sysctl -w vm.dirty_ratio=$DIRTY_RATIO_SAVE
	else
		# if file not here, we are a 2.4 kernel
		kill -CONT `pidof kupdated`
	fi
}

stop_writeback() {
	# setup the trap first, so someone cannot exit the test at the
	# exact wrong time and mess up a machine
	trap start_writeback EXIT
	# in 2.6, save and 0 /proc/sys/vm/dirty_writeback_centisecs
	if [ -f /proc/sys/vm/dirty_writeback_centisecs ]; then
		WRITEBACK_SAVE=`sysctl -n vm.dirty_writeback_centisecs`
		sysctl -w vm.dirty_writeback_centisecs=0
		sysctl -w vm.dirty_writeback_centisecs=0
		# save and increase /proc/sys/vm/dirty_ratio
		DIRTY_RATIO_SAVE=`sysctl -n vm.dirty_ratio`
		sysctl -w vm.dirty_ratio=$MAX_DIRTY_RATIO
		# save and increase /proc/sys/vm/dirty_background_ratio
		BG_DIRTY_RATIO_SAVE=`sysctl -n vm.dirty_background_ratio`
		sysctl -w vm.dirty_background_ratio=$MAX_BG_DIRTY_RATIO
	else
		# if file not here, we are a 2.4 kernel
		kill -STOP `pidof kupdated`
	fi
}

# ensure that all stripes have some grant before we test client-side cache
setup_test42() {
	for i in `seq -f $DIR/f42-%g 1 $OSTCOUNT`; do
		dd if=/dev/zero of=$i bs=4k count=1
		rm $i
	done
}

# Tests 42* verify that our behaviour is correct WRT caching, file closure,
# file truncation, and file removal.
test_42a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	setup_test42
	cancel_lru_locks osc
	stop_writeback
	sync; sleep 1; sync # just to be safe
	BEFOREWRITES=`count_ost_writes`
        lctl get_param -n osc.*[oO][sS][cC][_-]*.cur_grant_bytes | grep "[0-9]"
        dd if=/dev/zero of=$DIR/f42a bs=1024 count=100
	AFTERWRITES=`count_ost_writes`
	[ $BEFOREWRITES -eq $AFTERWRITES ] || \
		error "$BEFOREWRITES < $AFTERWRITES"
	start_writeback
}
run_test 42a "ensure that we don't flush on close =============="

test_42b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	setup_test42
	cancel_lru_locks osc
	stop_writeback
        sync
        dd if=/dev/zero of=$DIR/f42b bs=1024 count=100
        BEFOREWRITES=`count_ost_writes`
        $MUNLINK $DIR/f42b || error "$MUNLINK $DIR/f42b: $?"
        AFTERWRITES=`count_ost_writes`
        if [ $BEFOREWRITES -lt $AFTERWRITES ]; then
                error "$BEFOREWRITES < $AFTERWRITES on unlink"
        fi
        BEFOREWRITES=`count_ost_writes`
        sync || error "sync: $?"
        AFTERWRITES=`count_ost_writes`
        if [ $BEFOREWRITES -lt $AFTERWRITES ]; then
                error "$BEFOREWRITES < $AFTERWRITES on sync"
        fi
        dmesg | grep 'error from obd_brw_async' && error 'error writing back'
	start_writeback
        return 0
}
run_test 42b "test destroy of file with cached dirty data ======"

# if these tests just want to test the effect of truncation,
# they have to be very careful.  consider:
# - the first open gets a {0,EOF}PR lock
# - the first write conflicts and gets a {0, count-1}PW
# - the rest of the writes are under {count,EOF}PW
# - the open for truncate tries to match a {0,EOF}PR
#   for the filesize and cancels the PWs.
# any number of fixes (don't get {0,EOF} on open, match
# composite locks, do smarter file size management) fix
# this, but for now we want these tests to verify that
# the cancellation with truncate intent works, so we
# start the file with a full-file pw lock to match against
# until the truncate.
trunc_test() {
        test=$1
        file=$DIR/$test
        offset=$2
	cancel_lru_locks osc
	stop_writeback
	# prime the file with 0,EOF PW to match
	touch $file
        $TRUNCATE $file 0
        sync; sync
	# now the real test..
        dd if=/dev/zero of=$file bs=1024 count=100
        BEFOREWRITES=`count_ost_writes`
        $TRUNCATE $file $offset
        cancel_lru_locks osc
        AFTERWRITES=`count_ost_writes`
	start_writeback
}

test_42c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        trunc_test 42c 1024
        [ $BEFOREWRITES -eq $AFTERWRITES ] && \
            error "beforewrites $BEFOREWRITES == afterwrites $AFTERWRITES on truncate"
        rm $file
}
run_test 42c "test partial truncate of file with cached dirty data"

test_42d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        trunc_test 42d 0
        [ $BEFOREWRITES -eq $AFTERWRITES ] || \
            error "beforewrites $BEFOREWRITES != afterwrites $AFTERWRITES on truncate"
        rm $file
}
run_test 42d "test complete truncate of file with cached dirty data"

test_42e() { # bug22074
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local TDIR=$DIR/${tdir}e
	local pagesz=$(page_size)
	local pages=16 # hardcoded 16 pages, don't change it.
	local files=$((OSTCOUNT * 500)) # hopefully 500 files on each OST
	local proc_osc0="osc.${FSNAME}-OST0000-osc-[^MDT]*"
	local max_dirty_mb
	local warmup_files

	test_mkdir -p $DIR/${tdir}e
	$SETSTRIPE -c 1 $TDIR
	createmany -o $TDIR/f $files

	max_dirty_mb=$($LCTL get_param -n $proc_osc0/max_dirty_mb)

	# we assume that with $OSTCOUNT files, at least one of them will
	# be allocated on OST0.
	warmup_files=$((OSTCOUNT * max_dirty_mb))
	createmany -o $TDIR/w $warmup_files

	# write a large amount of data into one file and sync, to get good
	# avail_grant number from OST.
	for ((i=0; i<$warmup_files; i++)); do
		idx=$($GETSTRIPE -i $TDIR/w$i)
		[ $idx -ne 0 ] && continue
		dd if=/dev/zero of=$TDIR/w$i bs="$max_dirty_mb"M count=1
		break
	done
	[ $i -gt $warmup_files ] && error "OST0 is still cold"
	sync
	$LCTL get_param $proc_osc0/cur_dirty_bytes
	$LCTL get_param $proc_osc0/cur_grant_bytes

	# create as much dirty pages as we can while not to trigger the actual
	# RPCs directly. but depends on the env, VFS may trigger flush during this
	# period, hopefully we are good.
	for ((i=0; i<$warmup_files; i++)); do
		idx=$($GETSTRIPE -i $TDIR/w$i)
		[ $idx -ne 0 ] && continue
		dd if=/dev/zero of=$TDIR/w$i bs=1M count=1 2>/dev/null
	done
	$LCTL get_param $proc_osc0/cur_dirty_bytes
	$LCTL get_param $proc_osc0/cur_grant_bytes

	# perform the real test
	$LCTL set_param $proc_osc0/rpc_stats 0
	for ((;i<$files; i++)); do
		[ $($GETSTRIPE -i $TDIR/f$i) -eq 0 ] || continue
		dd if=/dev/zero of=$TDIR/f$i bs=$pagesz count=$pages 2>/dev/null
	done
	sync
	$LCTL get_param $proc_osc0/rpc_stats

        local percent=0
        local have_ppr=false
        $LCTL get_param $proc_osc0/rpc_stats |
                while read PPR RRPC RPCT RCUM BAR WRPC WPCT WCUM; do
                        # skip lines until we are at the RPC histogram data
                        [ "$PPR" == "pages" ] && have_ppr=true && continue
                        $have_ppr || continue

                        # we only want the percent stat for < 16 pages
                        [ $(echo $PPR | tr -d ':') -ge $pages ] && break

                        percent=$((percent + WPCT))
                        if [ $percent -gt 15 ]; then
                                error "less than 16-pages write RPCs" \
                                      "$percent% > 15%"
                                break
                        fi
                done
        rm -rf $TDIR
}
run_test 42e "verify sub-RPC writes are not done synchronously"

test_43() {
	test_mkdir -p $DIR/$tdir
	cp -p /bin/ls $DIR/$tdir/$tfile
	$MULTIOP $DIR/$tdir/$tfile Ow_c &
	pid=$!
	# give multiop a chance to open
	sleep 1

	$DIR/$tdir/$tfile && error || true
	kill -USR1 $pid
}
run_test 43 "execution of file opened for write should return -ETXTBSY"

test_43a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	cp -p `which $MULTIOP` $DIR/$tdir/multiop ||
			cp -p multiop $DIR/$tdir/multiop
        MULTIOP_PROG=$DIR/$tdir/multiop multiop_bg_pause $TMP/test43.junk O_c ||
			return 1
        MULTIOP_PID=$!
        $MULTIOP $DIR/$tdir/multiop Oc && error "expected error, got success"
        kill -USR1 $MULTIOP_PID || return 2
        wait $MULTIOP_PID || return 3
        rm $TMP/test43.junk
}
run_test 43a "open(RDWR) of file being executed should return -ETXTBSY"

test_43b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	cp -p `which $MULTIOP` $DIR/$tdir/multiop ||
			cp -p multiop $DIR/$tdir/multiop
        MULTIOP_PROG=$DIR/$tdir/multiop multiop_bg_pause $TMP/test43.junk O_c ||
			return 1
        MULTIOP_PID=$!
        $TRUNCATE $DIR/$tdir/multiop 0 && error "expected error, got success"
        kill -USR1 $MULTIOP_PID || return 2
        wait $MULTIOP_PID || return 3
        rm $TMP/test43.junk
}
run_test 43b "truncate of file being executed should return -ETXTBSY"

test_43c() {
	local testdir="$DIR/$tdir"
	test_mkdir -p $DIR/$tdir
	cp $SHELL $testdir/
	( cd $(dirname $SHELL) && md5sum $(basename $SHELL) ) | \
		( cd $testdir && md5sum -c)
}
run_test 43c "md5sum of copy into lustre========================"

test_44() {
	[  "$OSTCOUNT" -lt "2" ] && skip_env "skipping 2-stripe test" && return
	dd if=/dev/zero of=$DIR/f1 bs=4k count=1 seek=1023
	dd if=$DIR/f1 bs=4k count=1 > /dev/null
}
run_test 44 "zero length read from a sparse stripe ============="

test_44a() {
    local nstripe=`$LCTL lov_getconfig $DIR | grep default_stripe_count: | \
                         awk '{print $2}'`
    [ -z "$nstripe" ] && skip "can't get stripe info" && return
    [ "$nstripe" -gt "$OSTCOUNT" ] && skip "Wrong default_stripe_count: $nstripe (OSTCOUNT: $OSTCOUNT)" && return
    local stride=`$LCTL lov_getconfig $DIR | grep default_stripe_size: | \
                      awk '{print $2}'`
    if [ $nstripe -eq 0 -o $nstripe -eq -1 ] ; then
        nstripe=`$LCTL lov_getconfig $DIR | grep obd_count: | awk '{print $2}'`
    fi

    OFFSETS="0 $((stride/2)) $((stride-1))"
    for offset in $OFFSETS ; do
      for i in `seq 0 $((nstripe-1))`; do
        local GLOBALOFFSETS=""
        local size=$((((i + 2 * $nstripe )*$stride + $offset)))  # Bytes
	local myfn=$DIR/d44a-$size
	echo "--------writing $myfn at $size"
        ll_sparseness_write $myfn $size  || error "ll_sparseness_write"
        GLOBALOFFSETS="$GLOBALOFFSETS $size"
        ll_sparseness_verify $myfn $GLOBALOFFSETS \
                            || error "ll_sparseness_verify $GLOBALOFFSETS"

        for j in `seq 0 $((nstripe-1))`; do
            size=$((((j + $nstripe )*$stride + $offset)))  # Bytes
            ll_sparseness_write $myfn $size || error "ll_sparseness_write"
            GLOBALOFFSETS="$GLOBALOFFSETS $size"
        done
        ll_sparseness_verify $myfn $GLOBALOFFSETS \
                            || error "ll_sparseness_verify $GLOBALOFFSETS"
	rm -f $myfn
      done
    done
}
run_test 44a "test sparse pwrite ==============================="

dirty_osc_total() {
	tot=0
	for d in `lctl get_param -n osc.*.cur_dirty_bytes`; do
		tot=$(($tot + $d))
	done
	echo $tot
}
do_dirty_record() {
	before=`dirty_osc_total`
	echo executing "\"$*\""
	eval $*
	after=`dirty_osc_total`
	echo before $before, after $after
}
test_45() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	f="$DIR/f45"
	# Obtain grants from OST if it supports it
	echo blah > ${f}_grant
	stop_writeback
	sync
	do_dirty_record "echo blah > $f"
	[ $before -eq $after ] && error "write wasn't cached"
	do_dirty_record "> $f"
	[ $before -gt $after ] || error "truncate didn't lower dirty count"
	do_dirty_record "echo blah > $f"
	[ $before -eq $after ] && error "write wasn't cached"
	do_dirty_record "sync"
	[ $before -gt $after ] || error "writeback didn't lower dirty count"
	do_dirty_record "echo blah > $f"
	[ $before -eq $after ] && error "write wasn't cached"
	do_dirty_record "cancel_lru_locks osc"
	[ $before -gt $after ] || error "lock cancellation didn't lower dirty count"
	start_writeback
}
run_test 45 "osc io page accounting ============================"

# in a 2 stripe file (lov.sh), page 1023 maps to page 511 in its object.  this
# test tickles a bug where re-dirtying a page was failing to be mapped to the
# objects offset and an assert hit when an rpc was built with 1023's mapped
# offset 511 and 511's raw 511 offset. it also found general redirtying bugs.
test_46() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	f="$DIR/f46"
	stop_writeback
	sync
	dd if=/dev/zero of=$f bs=`page_size` seek=511 count=1
	sync
	dd conv=notrunc if=/dev/zero of=$f bs=`page_size` seek=1023 count=1
	dd conv=notrunc if=/dev/zero of=$f bs=`page_size` seek=511 count=1
	sync
	start_writeback
}
run_test 46 "dirtying a previously written page ================"

# test_47 is removed "Device nodes check" is moved to test_28

test_48a() { # bug 2399
	check_kernel_version 34 || return 0
	test_mkdir -p $DIR/$tdir
	cd $DIR/$tdir
	mv $DIR/$tdir $DIR/d48.new || error "move directory failed"
	test_mkdir $DIR/$tdir || error "recreate directory failed"
	touch foo || error "'touch foo' failed after recreating cwd"
	test_mkdir $DIR/$tdir/bar ||
		     error "'mkdir foo' failed after recreating cwd"
	if check_kernel_version 44; then
		touch .foo || error "'touch .foo' failed after recreating cwd"
		test_mkdir $DIR/$tdir/.bar ||
			      error "'mkdir .foo' failed after recreating cwd"
	fi
	ls . > /dev/null || error "'ls .' failed after recreating cwd"
	ls .. > /dev/null || error "'ls ..' failed after removing cwd"
	cd . || error "'cd .' failed after recreating cwd"
	test_mkdir . && error "'mkdir .' worked after recreating cwd"
	rmdir . && error "'rmdir .' worked after recreating cwd"
	ln -s . baz || error "'ln -s .' failed after recreating cwd"
	cd .. || error "'cd ..' failed after recreating cwd"
}
run_test 48a "Access renamed working dir (should return errors)="

test_48b() { # bug 2399
	check_kernel_version 34 || return 0
	rm -rf $DIR/$tdir
	test_mkdir -p $DIR/$tdir
	cd $DIR/$tdir
	rmdir $DIR/$tdir || error "remove cwd $DIR/$tdir failed"
	touch foo && error "'touch foo' worked after removing cwd"
	test_mkdir $DIR/$tdir/foo &&
		     error "'mkdir foo' worked after removing cwd"
	if check_kernel_version 44; then
		touch .foo && error "'touch .foo' worked after removing cwd"
		test_mkdir $DIR/$tdir/.foo &&
			      error "'mkdir .foo' worked after removing cwd"
	fi
	ls . > /dev/null && error "'ls .' worked after removing cwd"
	ls .. > /dev/null || error "'ls ..' failed after removing cwd"
	is_patchless || ( cd . && error "'cd .' worked after removing cwd" )
	test_mkdir $DIR/$tdir/. && error "'mkdir .' worked after removing cwd"
	rmdir . && error "'rmdir .' worked after removing cwd"
	ln -s . foo && error "'ln -s .' worked after removing cwd"
	cd .. || echo "'cd ..' failed after removing cwd `pwd`"  #bug 3517
}
run_test 48b "Access removed working dir (should return errors)="

test_48c() { # bug 2350
	check_kernel_version 36 || return 0
	#lctl set_param debug=-1
	#set -vx
	rm -rf $DIR/$tdir
	test_mkdir -p $DIR/$tdir/dir
	cd $DIR/$tdir/dir
	$TRACE rmdir $DIR/$tdir/dir || error "remove cwd $DIR/$tdir/dir failed"
	$TRACE touch foo && error "touch foo worked after removing cwd"
	$TRACE test_mkdir foo && error "'mkdir foo' worked after removing cwd"
	if check_kernel_version 44; then
		touch .foo && error "touch .foo worked after removing cwd"
		test_mkdir .foo && error "mkdir .foo worked after removing cwd"
	fi
	$TRACE ls . && error "'ls .' worked after removing cwd"
	$TRACE ls .. || error "'ls ..' failed after removing cwd"
	is_patchless || ( $TRACE cd . &&
			error "'cd .' worked after removing cwd" )
	$TRACE test_mkdir . && error "'mkdir .' worked after removing cwd"
	$TRACE rmdir . && error "'rmdir .' worked after removing cwd"
	$TRACE ln -s . foo && error "'ln -s .' worked after removing cwd"
	$TRACE cd .. || echo "'cd ..' failed after removing cwd `pwd`" #bug 3415
}
run_test 48c "Access removed working subdir (should return errors)"

test_48d() { # bug 2350
	check_kernel_version 36 || return 0
	#lctl set_param debug=-1
	#set -vx
	rm -rf $DIR/$tdir
	test_mkdir -p $DIR/$tdir/dir
	cd $DIR/$tdir/dir
	$TRACE rmdir $DIR/$tdir/dir || error "remove cwd $DIR/$tdir/dir failed"
	$TRACE rmdir $DIR/$tdir || error "remove parent $DIR/$tdir failed"
	$TRACE touch foo && error "'touch foo' worked after removing parent"
	$TRACE test_mkdir foo && error "mkdir foo worked after removing parent"
	if check_kernel_version 44; then
		touch .foo && error "'touch .foo' worked after removing parent"
		test_mkdir .foo &&
			      error "mkdir .foo worked after removing parent"
	fi
	$TRACE ls . && error "'ls .' worked after removing parent"
	$TRACE ls .. && error "'ls ..' worked after removing parent"
	is_patchless || ( $TRACE cd . &&
			error "'cd .' worked after recreate parent" )
	$TRACE test_mkdir . && error "'mkdir .' worked after removing parent"
	$TRACE rmdir . && error "'rmdir .' worked after removing parent"
	$TRACE ln -s . foo && error "'ln -s .' worked after removing parent"
	is_patchless || ( $TRACE cd .. &&
			error "'cd ..' worked after removing parent" || true )
}
run_test 48d "Access removed parent subdir (should return errors)"

test_48e() { # bug 4134
	check_kernel_version 41 || return 0
	#lctl set_param debug=-1
	#set -vx
	rm -rf $DIR/$tdir
	test_mkdir -p $DIR/$tdir/dir
	cd $DIR/$tdir/dir
	$TRACE rmdir $DIR/$tdir/dir || error "remove cwd $DIR/$tdir/dir failed"
	$TRACE rmdir $DIR/$tdir || error "remove parent $DIR/$tdir failed"
	$TRACE touch $DIR/$tdir || error "'touch $DIR/$tdir' failed"
	$TRACE chmod +x $DIR/$tdir || error "'chmod +x $DIR/$tdir' failed"
	# On a buggy kernel addition of "touch foo" after cd .. will
	# produce kernel oops in lookup_hash_it
	touch ../foo && error "'cd ..' worked after recreate parent"
	cd $DIR
	$TRACE rm $DIR/$tdir || error "rm '$DIR/$tdir' failed"
}
run_test 48e "Access to recreated parent subdir (should return errors)"

test_49() { # LU-1030
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	# get ost1 size - lustre-OST0000
	ost1_size=$(do_facet ost1 lfs df |grep ${ost1_svc} |awk '{print $4}')
	# write 800M at maximum
	[ $ost1_size -gt 819200 ] && ost1_size=819200

	lfs setstripe -c 1 -i 0 $DIR/$tfile
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=$((ost1_size >> 2)) &
	local dd_pid=$!

	# change max_pages_per_rpc while writing the file
	local osc1_mppc=osc.$(get_osc_import_name client ost1).max_pages_per_rpc
	local orig_mppc=`$LCTL get_param -n $osc1_mppc`
	# loop until dd process exits
	while ps ax -opid | grep -wq $dd_pid; do
		$LCTL set_param $osc1_mppc=$((RANDOM % 256 + 1))
		sleep $((RANDOM % 5 + 1))
	done
	# restore original max_pages_per_rpc
	$LCTL set_param $osc1_mppc=$orig_mppc
	rm $DIR/$tfile || error "rm $DIR/$tfile failed"
}
run_test 49 "Change max_pages_per_rpc won't break osc extent"

test_50() {
	# bug 1485
	test_mkdir $DIR/$tdir
	cd $DIR/$tdir
	ls /proc/$$/cwd || error
}
run_test 50 "special situations: /proc symlinks  ==============="

test_51a() {	# was test_51
	# bug 1516 - create an empty entry right after ".." then split dir
	test_mkdir -p $DIR/$tdir
	touch $DIR/$tdir/foo
	$MCREATE $DIR/$tdir/bar
	rm $DIR/$tdir/foo
	createmany -m $DIR/$tdir/longfile 201
	FNUM=202
	while [ `ls -sd $DIR/$tdir | awk '{ print $1 }'` -eq 4 ]; do
		$MCREATE $DIR/$tdir/longfile$FNUM
		FNUM=$(($FNUM + 1))
		echo -n "+"
	done
	echo
	ls -l $DIR/$tdir > /dev/null || error
}
run_test 51a "special situations: split htree with empty entry =="

export NUMTEST=70000
test_51b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local BASE=$DIR/$tdir

	# cleanup the directory
	rm -fr $BASE

	test_mkdir -p $BASE

	local mdtidx=$(printf "%04x" $($LFS getstripe -M $BASE))
	local numfree=$(lctl get_param -n mdc.$FSNAME-MDT$mdtidx*.filesfree)
	[ $numfree -lt 21000 ] && skip "not enough free inodes ($numfree)" &&
		return

	[ $numfree -lt $NUMTEST ] && NUMTEST=$(($numfree - 50)) &&
		echo "reduced count to $NUMTEST due to inodes"

	# need to check free space for the directories as well
	local blkfree=$(lctl get_param -n mdc.$FSNAME-MDT$mdtidx*.kbytesavail)
	numfree=$((blkfree / 4))
	[ $numfree -lt $NUMTEST ] && NUMTEST=$(($numfree - 50)) &&
		echo "reduced count to $NUMTEST due to blocks"

	createmany -d $BASE/d $NUMTEST && echo $NUMTEST > $BASE/fnum ||
		echo "failed" > $BASE/fnum
}
run_test 51b "exceed 64k subdirectory nlink limit"

test_51ba() { # LU-993
	local BASE=$DIR/$tdir
	# unlink all but 100 subdirectories, then check it still works
	local LEFT=100
	[ -f $BASE/fnum ] && local NUMPREV=$(cat $BASE/fnum) && rm $BASE/fnum

	[ "$NUMPREV" != "failed" ] && NUMTEST=$NUMPREV
	local DELETE=$((NUMTEST - LEFT))

	# continue on to run this test even if 51b didn't finish,
	# just to delete the many subdirectories created.
	[ ! -d "${BASE}/d1" ] && skip "test_51b() not run" && return 0

	# for ldiskfs the nlink count should be 1, but this is OSD specific
	# and so this is listed for informational purposes only
	echo "nlink before: $(stat -c %h $BASE), created before: $NUMTEST"
	unlinkmany -d $BASE/d $DELETE
	RC=$?

	if [ $RC -ne 0 ]; then
		if [ "$NUMPREV" == "failed" ]; then
			skip "previous setup failed"
			return 0
		else
			error "unlink of first $DELETE subdirs failed"
			return $RC
		fi
	fi

	echo "nlink between: $(stat -c %h $BASE)"
	# trim the first line of ls output
	local FOUND=$(($(ls -l ${BASE} | wc -l) - 1))
	[ $FOUND -ne $LEFT ] &&
		error "can't find subdirs: found only $FOUND/$LEFT"

	unlinkmany -d $BASE/d $DELETE $LEFT ||
		error "unlink of second $LEFT subdirs failed"
	# regardless of whether the backing filesystem tracks nlink accurately
	# or not, the nlink count shouldn't be more than "." and ".." here
	local AFTER=$(stat -c %h $BASE)
	[ $AFTER -gt 2 ] && error "nlink after: $AFTER > 2" ||
		echo "nlink after: $AFTER"
}
run_test 51ba "verify nlink for many subdirectory cleanup"

test_51d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        [  "$OSTCOUNT" -lt "3" ] && skip_env "skipping test with few OSTs" && return
        test_mkdir -p $DIR/$tdir
        createmany -o $DIR/$tdir/t- 1000
        $GETSTRIPE $DIR/$tdir > $TMP/files
        for N in `seq 0 $((OSTCOUNT - 1))`; do
	    OBJS[$N]=`awk -vobjs=0 '($1 == '$N') { objs += 1 } END { print objs;}' $TMP/files`
	    OBJS0[$N]=`grep -A 1 idx $TMP/files | awk -vobjs=0 '($1 == '$N') { objs += 1 } END { print objs;}'`
	    log "OST$N has ${OBJS[$N]} objects, ${OBJS0[$N]} are index 0"
        done
        unlinkmany $DIR/$tdir/t- 1000

        NLAST=0
        for N in `seq 1 $((OSTCOUNT - 1))`; do
	    [ ${OBJS[$N]} -lt $((${OBJS[$NLAST]} - 20)) ] && \
		error "OST $N has less objects vs OST $NLAST (${OBJS[$N]} < ${OBJS[$NLAST]}"
	    [ ${OBJS[$N]} -gt $((${OBJS[$NLAST]} + 20)) ] && \
		error "OST $N has less objects vs OST $NLAST (${OBJS[$N]} < ${OBJS[$NLAST]}"

	    [ ${OBJS0[$N]} -lt $((${OBJS0[$NLAST]} - 20)) ] && \
		error "OST $N has less #0 objects vs OST $NLAST (${OBJS0[$N]} < ${OBJS0[$NLAST]}"
	    [ ${OBJS0[$N]} -gt $((${OBJS0[$NLAST]} + 20)) ] && \
		error "OST $N has less #0 objects vs OST $NLAST (${OBJS0[$N]} < ${OBJS0[$NLAST]}"
	    NLAST=$N
        done
}
run_test 51d "check object distribution ===================="

test_52a() {
	[ -f $DIR/$tdir/foo ] && chattr -a $DIR/$tdir/foo
	test_mkdir -p $DIR/$tdir
	touch $DIR/$tdir/foo
	chattr +a $DIR/$tdir/foo || error "chattr +a failed"
	echo bar >> $DIR/$tdir/foo || error "append bar failed"
	cp /etc/hosts $DIR/$tdir/foo && error "cp worked"
	rm -f $DIR/$tdir/foo 2>/dev/null && error "rm worked"
	link $DIR/$tdir/foo $DIR/$tdir/foo_link 2>/dev/null &&
					error "link worked"
	echo foo >> $DIR/$tdir/foo || error "append foo failed"
	mrename $DIR/$tdir/foo $DIR/$tdir/foo_ren && error "rename worked"
	lsattr $DIR/$tdir/foo | egrep -q "^-+a[-e]+ $DIR/$tdir/foo" ||
						     error "lsattr"
	chattr -a $DIR/$tdir/foo || error "chattr -a failed"
        cp -r $DIR/$tdir /tmp/
	rm -fr $DIR/$tdir || error "cleanup rm failed"
}
run_test 52a "append-only flag test (should return errors) ====="

test_52b() {
	[ -f $DIR/$tdir/foo ] && chattr -i $DIR/$tdir/foo
	test_mkdir -p $DIR/$tdir
	touch $DIR/$tdir/foo
	chattr +i $DIR/$tdir/foo || error "chattr +i failed"
	cat test > $DIR/$tdir/foo && error "cat test worked"
	cp /etc/hosts $DIR/$tdir/foo && error "cp worked"
	rm -f $DIR/$tdir/foo 2>/dev/null && error "rm worked"
	link $DIR/$tdir/foo $DIR/$tdir/foo_link 2>/dev/null &&
					error "link worked"
	echo foo >> $DIR/$tdir/foo && error "echo worked"
	mrename $DIR/$tdir/foo $DIR/$tdir/foo_ren && error "rename worked"
	[ -f $DIR/$tdir/foo ] || error
	[ -f $DIR/$tdir/foo_ren ] && error
	lsattr $DIR/$tdir/foo | egrep -q "^-+i[-e]+ $DIR/$tdir/foo" ||
							error "lsattr"
	chattr -i $DIR/$tdir/foo || error "chattr failed"

	rm -fr $DIR/$tdir || error
}
run_test 52b "immutable flag test (should return errors) ======="

test_53() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	local param
	local ostname
	local mds_last
	local ost_last
	local ostnum
	local node

	# only test MDT0000
        local mdtosc=$(get_mdtosc_proc_path $SINGLEMDS)
        for value in $(do_facet $SINGLEMDS lctl get_param osc.$mdtosc.prealloc_last_id) ; do
                param=`echo ${value[0]} | cut -d "=" -f1`
                ostname=`echo $param | cut -d "." -f2 | cut -d - -f 1-2`
                mds_last=$(do_facet $SINGLEMDS lctl get_param -n $param)
		ostnum=$(index_from_ostuuid ${ostname}_UUID)
		node=$(facet_active_host ost$((ostnum+1)))
		param="obdfilter.$ostname.last_id"
		ost_last=$(do_node $node lctl get_param -n $param | head -n 1 |
			   awk -F':' '{print $2}')
                echo "$ostname.last_id=$ost_last ; MDS.last_id=$mds_last"
                if [ $ost_last != $mds_last ]; then
                    error "$ostname.last_id=$ost_last ; MDS.last_id=$mds_last"
                fi
        done
}
run_test 53 "verify that MDS and OSTs agree on pre-creation ===="

test_54a() {
        [ ! -f "$SOCKETSERVER" ] && skip_env "no socketserver, skipping" && return
        [ ! -f "$SOCKETCLIENT" ] && skip_env "no socketclient, skipping" && return
     	$SOCKETSERVER $DIR/socket
     	$SOCKETCLIENT $DIR/socket || error
      	$MUNLINK $DIR/socket
}
run_test 54a "unix domain socket test =========================="

test_54b() {
	f="$DIR/f54b"
	mknod $f c 1 3
	chmod 0666 $f
	dd if=/dev/zero of=$f bs=`page_size` count=1
}
run_test 54b "char device works in lustre ======================"

find_loop_dev() {
	[ -b /dev/loop/0 ] && LOOPBASE=/dev/loop/
	[ -b /dev/loop0 ] && LOOPBASE=/dev/loop
	[ -z "$LOOPBASE" ] && echo "/dev/loop/0 and /dev/loop0 gone?" && return

	for i in `seq 3 7`; do
		losetup $LOOPBASE$i > /dev/null 2>&1 && continue
		LOOPDEV=$LOOPBASE$i
		LOOPNUM=$i
		break
	done
}

test_54c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	tfile="$DIR/f54c"
	tdir="$DIR/d54c"
	loopdev="$DIR/loop54c"

	find_loop_dev
	[ -z "$LOOPNUM" ] && echo "couldn't find empty loop device" && return
	mknod $loopdev b 7 $LOOPNUM
	echo "make a loop file system with $tfile on $loopdev ($LOOPNUM)..."
	dd if=/dev/zero of=$tfile bs=`page_size` seek=1024 count=1 > /dev/null
	losetup $loopdev $tfile || error "can't set up $loopdev for $tfile"
	mkfs.ext2 $loopdev || error "mke2fs on $loopdev"
	test_mkdir -p $tdir
	mount -t ext2 $loopdev $tdir || error "error mounting $loopdev on $tdir"
	dd if=/dev/zero of=$tdir/tmp bs=`page_size` count=30 || error "dd write"
	df $tdir
	dd if=$tdir/tmp of=/dev/zero bs=`page_size` count=30 || error "dd read"
	$UMOUNT $tdir
	losetup -d $loopdev
	rm $loopdev
}
run_test 54c "block device works in lustre ====================="

test_54d() {
	f="$DIR/f54d"
	string="aaaaaa"
	mknod $f p
	[ "$string" = `echo $string > $f | cat $f` ] || error
}
run_test 54d "fifo device works in lustre ======================"

test_54e() {
	check_kernel_version 46 || return 0
	f="$DIR/f54e"
	string="aaaaaa"
	cp -aL /dev/console $f
	echo $string > $f || error
}
run_test 54e "console/tty device works in lustre ======================"

#The test_55 used to be iopen test and it was removed by bz#24037.
#run_test 55 "check iopen_connect_dentry() ======================"

test_56a() {	# was test_56
        rm -rf $DIR/$tdir
        $SETSTRIPE -d $DIR
	test_mkdir $DIR/$tdir
        test_mkdir $DIR/$tdir/dir
        NUMFILES=3
        NUMFILESx2=$(($NUMFILES * 2))
        for i in `seq 1 $NUMFILES` ; do
                touch $DIR/$tdir/file$i
                touch $DIR/$tdir/dir/file$i
        done

        # test lfs getstripe with --recursive
        FILENUM=`$GETSTRIPE --recursive $DIR/$tdir | grep -c obdidx`
        [ $FILENUM -eq $NUMFILESx2 ] ||
                error "$GETSTRIPE --recursive: found $FILENUM, not $NUMFILESx2"
        FILENUM=`$GETSTRIPE $DIR/$tdir | grep -c obdidx`
        [ $FILENUM -eq $NUMFILES ] ||
                error "$GETSTRIPE $DIR/$tdir: found $FILENUM, not $NUMFILES"
        echo "$GETSTRIPE --recursive passed."

        # test lfs getstripe with file instead of dir
        FILENUM=`$GETSTRIPE $DIR/$tdir/file1 | grep -c obdidx`
        [ $FILENUM  -eq 1 ] || error \
                 "$GETSTRIPE $DIR/$tdir/file1: found $FILENUM, not 1"
        echo "$GETSTRIPE file1 passed."

        #test lfs getstripe with --verbose
        [ `$GETSTRIPE --verbose $DIR/$tdir |
			grep -c lmm_magic` -eq $NUMFILES ] ||
                error "$GETSTRIPE --verbose $DIR/$tdir: want $NUMFILES"
        [ `$GETSTRIPE $DIR/$tdir | grep -c lmm_magic` -eq 0 ] ||
            error "$GETSTRIPE $DIR/$tdir: showed lmm_magic"
        echo "$GETSTRIPE --verbose passed."

        #test lfs getstripe with --obd
        $GETSTRIPE --obd wrong_uuid $DIR/$tdir 2>&1 |
					grep -q "unknown obduuid" ||
                error "$GETSTRIPE --obd wrong_uuid should return error message"

        [  "$OSTCOUNT" -lt 2 ] &&
                skip_env "skipping other $GETSTRIPE --obd test" && return

        OSTIDX=1
        OBDUUID=$(ostuuid_from_index $OSTIDX)
        FILENUM=`$GETSTRIPE -ir $DIR/$tdir | grep -x $OSTIDX | wc -l`
        FOUND=`$GETSTRIPE -r --obd $OBDUUID $DIR/$tdir | grep obdidx | wc -l`
        [ $FOUND -eq $FILENUM ] ||
                error "$GETSTRIPE --obd wrong: found $FOUND, expected $FILENUM"
        [ `$GETSTRIPE -r -v --obd $OBDUUID $DIR/$tdir |
                sed '/^[	 ]*'${OSTIDX}'[	 ]/d' |
                sed -n '/^[	 ]*[0-9][0-9]*[	 ]/p' | wc -l` -eq 0 ] ||
                error "$GETSTRIPE --obd: should not show file on other obd"
        echo "$GETSTRIPE --obd passed"
}
run_test 56a "check $GETSTRIPE"

NUMFILES=3
NUMDIRS=3
setup_56() {
        local LOCAL_NUMFILES="$1"
        local LOCAL_NUMDIRS="$2"
        local MKDIR_PARAMS="$3"

        if [ ! -d "$TDIR" ] ; then
                test_mkdir -p $TDIR
                [ "$MKDIR_PARAMS" ] && $SETSTRIPE $MKDIR_PARAMS $TDIR
                for i in `seq 1 $LOCAL_NUMFILES` ; do
                        touch $TDIR/file$i
                done
                for i in `seq 1 $LOCAL_NUMDIRS` ; do
                        test_mkdir $TDIR/dir$i
                        for j in `seq 1 $LOCAL_NUMFILES` ; do
                                touch $TDIR/dir$i/file$j
                        done
                done
        fi
}

setup_56_special() {
	LOCAL_NUMFILES=$1
	LOCAL_NUMDIRS=$2
	setup_56 $1 $2
	if [ ! -e "$TDIR/loop1b" ] ; then
		for i in `seq 1 $LOCAL_NUMFILES` ; do
			mknod $TDIR/loop${i}b b 7 $i
			mknod $TDIR/null${i}c c 1 3
			ln -s $TDIR/file1 $TDIR/link${i}l
		done
		for i in `seq 1 $LOCAL_NUMDIRS` ; do
			mknod $TDIR/dir$i/loop${i}b b 7 $i
			mknod $TDIR/dir$i/null${i}c c 1 3
			ln -s $TDIR/dir$i/file1 $TDIR/dir$i/link${i}l
		done
	fi
}

test_56g() {
        $SETSTRIPE -d $DIR

        TDIR=$DIR/${tdir}g
        setup_56 $NUMFILES $NUMDIRS

        EXPECTED=$(($NUMDIRS + 2))
        # test lfs find with -name
        for i in $(seq 1 $NUMFILES) ; do
                NUMS=$($LFIND -name "*$i" $TDIR | wc -l)
                [ $NUMS -eq $EXPECTED ] ||
                        error "lfs find -name \"*$i\" $TDIR wrong: "\
                              "found $NUMS, expected $EXPECTED"
        done
}
run_test 56g "check lfs find -name ============================="

test_56h() {
        $SETSTRIPE -d $DIR

        TDIR=$DIR/${tdir}g
        setup_56 $NUMFILES $NUMDIRS

        EXPECTED=$(((NUMDIRS + 1) * (NUMFILES - 1) + NUMFILES))
        # test lfs find with ! -name
        for i in $(seq 1 $NUMFILES) ; do
                NUMS=$($LFIND ! -name "*$i" $TDIR | wc -l)
                [ $NUMS -eq $EXPECTED ] ||
                        error "lfs find ! -name \"*$i\" $TDIR wrong: "\
                              "found $NUMS, expected $EXPECTED"
        done
}
run_test 56h "check lfs find ! -name ============================="

test_56i() {
       tdir=${tdir}i
       test_mkdir -p $DIR/$tdir
       UUID=$(ostuuid_from_index 0 $DIR/$tdir)
       CMD="$LFIND -ost $UUID $DIR/$tdir"
       OUT=$($CMD)
       [ -z "$OUT" ] || error "\"$CMD\" returned directory '$OUT'"
}
run_test 56i "check 'lfs find -ost UUID' skips directories ======="

test_56j() {
	TDIR=$DIR/${tdir}g
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$((NUMDIRS + 1))
	CMD="$LFIND -type d $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56j "check lfs find -type d ============================="

test_56k() {
	TDIR=$DIR/${tdir}g
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$(((NUMDIRS + 1) * NUMFILES))
	CMD="$LFIND -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56k "check lfs find -type f ============================="

test_56l() {
	TDIR=$DIR/${tdir}g
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$((NUMDIRS + NUMFILES))
	CMD="$LFIND -type b $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56l "check lfs find -type b ============================="

test_56m() {
	TDIR=$DIR/${tdir}g
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$((NUMDIRS + NUMFILES))
	CMD="$LFIND -type c $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56m "check lfs find -type c ============================="

test_56n() {
	TDIR=$DIR/${tdir}g
	setup_56_special $NUMFILES $NUMDIRS

	EXPECTED=$((NUMDIRS + NUMFILES))
	CMD="$LFIND -type l $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56n "check lfs find -type l ============================="

test_56o() {
	TDIR=$DIR/${tdir}o
	setup_56 $NUMFILES $NUMDIRS

	utime $TDIR/file1 > /dev/null || error "utime (1)"
	utime $TDIR/file2 > /dev/null || error "utime (2)"
	utime $TDIR/dir1 > /dev/null || error "utime (3)"
	utime $TDIR/dir2 > /dev/null || error "utime (4)"
	utime $TDIR/dir1/file1 > /dev/null || error "utime (5)"
	dd if=/dev/zero count=1 >> $TDIR/dir1/file1 && sync

	EXPECTED=4
	NUMS=`$LFIND -mtime +0 $TDIR | wc -l`
	[ $NUMS -eq $EXPECTED ] || \
		error "lfs find -mtime +0 $TDIR wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=12
	CMD="$LFIND -mtime 0 $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56o "check lfs find -mtime for old files =========================="

test_56p() {
	[ $RUNAS_ID -eq $UID ] &&
		skip_env "RUNAS_ID = UID = $UID -- skipping" && return

	TDIR=$DIR/${tdir}p
	setup_56 $NUMFILES $NUMDIRS

	chown $RUNAS_ID $TDIR/file* || error "chown $DIR/${tdir}g/file$i failed"
	EXPECTED=$NUMFILES
	CMD="$LFIND -uid $RUNAS_ID $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] || \
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=$(((NUMFILES + 1) * NUMDIRS + 1))
	CMD="$LFIND ! -uid $RUNAS_ID $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] || \
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56p "check lfs find -uid and ! -uid ==============================="

test_56q() {
	[ $RUNAS_ID -eq $UID ] &&
		skip_env "RUNAS_ID = UID = $UID -- skipping" && return

	TDIR=$DIR/${tdir}q
	setup_56 $NUMFILES $NUMDIRS

	chgrp $RUNAS_GID $TDIR/file* || error "chown $TDIR/file$i failed"

	EXPECTED=$NUMFILES
	CMD="$LFIND -gid $RUNAS_GID $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=$(( ($NUMFILES+1) * $NUMDIRS + 1))
	CMD="$LFIND ! -gid $RUNAS_GID $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56q "check lfs find -gid and ! -gid ==============================="

test_56r() {
	TDIR=$DIR/${tdir}r
	setup_56 $NUMFILES $NUMDIRS

	EXPECTED=12
	CMD="$LFIND -size 0 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
	EXPECTED=0
	CMD="$LFIND ! -size 0 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
	echo "test" > $TDIR/$tfile
	echo "test2" > $TDIR/$tfile.2 && sync
	EXPECTED=1
	CMD="$LFIND -size 5 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
	EXPECTED=1
	CMD="$LFIND -size +5 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
	EXPECTED=2
	CMD="$LFIND -size +0 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
	EXPECTED=2
	CMD="$LFIND ! -size -5 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
	EXPECTED=12
	CMD="$LFIND -size -5 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56r "check lfs find -size works =========================="

test_56s() { # LU-611
	TDIR=$DIR/${tdir}s
	setup_56 $NUMFILES $NUMDIRS "-c $OSTCOUNT"

	if [ $OSTCOUNT -gt 1 ]; then
		$SETSTRIPE -c 1 $TDIR/$tfile.{0,1,2,3}
		ONESTRIPE=4
		EXTRA=4
	else
		ONESTRIPE=$(((NUMDIRS + 1) * NUMFILES))
		EXTRA=0
	fi

	EXPECTED=$(((NUMDIRS + 1) * NUMFILES))
	CMD="$LFIND -stripe-count $OSTCOUNT -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=$(((NUMDIRS + 1) * NUMFILES + EXTRA))
	CMD="$LFIND -stripe-count +0 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=$ONESTRIPE
	CMD="$LFIND -stripe-count 1 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	CMD="$LFIND -stripe-count -2 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=0
	CMD="$LFIND -stripe-count $((OSTCOUNT + 1)) -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56s "check lfs find -stripe-count works"

test_56t() { # LU-611
	TDIR=$DIR/${tdir}t
	setup_56 $NUMFILES $NUMDIRS "-s 512k"

	$SETSTRIPE -S 256k $TDIR/$tfile.{0,1,2,3}

	EXPECTED=$(((NUMDIRS + 1) * NUMFILES))
	CMD="$LFIND -stripe-size 512k -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	CMD="$LFIND -stripe-size +320k -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=$(((NUMDIRS + 1) * NUMFILES + 4))
	CMD="$LFIND -stripe-size +200k -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	CMD="$LFIND -stripe-size -640k -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=4
	CMD="$LFIND -stripe-size 256k -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	CMD="$LFIND -stripe-size -320k -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=0
	CMD="$LFIND -stripe-size 1024k -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56t "check lfs find -stripe-size works"

test_56u() { # LU-611
	TDIR=$DIR/${tdir}u
	setup_56 $NUMFILES $NUMDIRS "-i 0"

	if [ $OSTCOUNT -gt 1 ]; then
		$SETSTRIPE -i 1 $TDIR/$tfile.{0,1,2,3}
		ONESTRIPE=4
	else
		ONESTRIPE=0
	fi

	EXPECTED=$(((NUMDIRS + 1) * NUMFILES))
	CMD="$LFIND -stripe-index 0 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=$ONESTRIPE
	CMD="$LFIND -stripe-index 1 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	CMD="$LFIND ! -stripe-index 0 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=0
	# This should produce an error and not return any files
	CMD="$LFIND -stripe-index $OSTCOUNT -type f $TDIR"
	NUMS=$($CMD 2>/dev/null | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"

	EXPECTED=$(((NUMDIRS + 1) * NUMFILES + ONESTRIPE))
	CMD="$LFIND -stripe-index 0,1 -type f $TDIR"
	NUMS=$($CMD | wc -l)
	[ $NUMS -eq $EXPECTED ] ||
		error "\"$CMD\" wrong: found $NUMS, expected $EXPECTED"
}
run_test 56u "check lfs find -stripe-index works"

test_56v() {
    local MDT_IDX=0

    TDIR=$DIR/${tdir}v
    rm -rf $TDIR
    setup_56 $NUMFILES $NUMDIRS

    UUID=$(mdtuuid_from_index $MDT_IDX $TDIR)
    [ -z "$UUID" ] && error "mdtuuid_from_index cannot find MDT index $MDT_IDX"

    for file in $($LFIND -mdt $UUID $TDIR); do
        file_mdt_idx=$($GETSTRIPE -M $file)
        [ $file_mdt_idx -eq $MDT_IDX ] ||
            error "'lfind -mdt $UUID' != 'getstripe -M' ($file_mdt_idx)"
    done
}
run_test 56v "check 'lfs find -mdt match with lfs getstripe -M' ======="

# Get and check the actual stripe count of one file.
# Usage: check_stripe_count <file> <expected_stripe_count>
check_stripe_count() {
    local file=$1
    local expected=$2
    local actual

    [[ -z "$file" || -z "$expected" ]] &&
        error "check_stripe_count: invalid argument!"

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

test_56w() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	TDIR=$DIR/${tdir}w

    rm -rf $TDIR || error "remove $TDIR failed"
    setup_56 $NUMFILES $NUMDIRS "-c $OSTCOUNT"

    local stripe_size
    stripe_size=$($GETSTRIPE -S -d $TDIR) ||
        error "$GETSTRIPE -S -d $TDIR failed"
    stripe_size=${stripe_size%% *}

    local file_size=$((stripe_size * OSTCOUNT))
    local file_num=$((NUMDIRS * NUMFILES + NUMFILES))
    local required_space=$((file_num * file_size))
    local free_space=$($LCTL get_param -n lov.$LOVNAME.kbytesavail)
    [[ $free_space -le $((required_space / 1024)) ]] &&
        skip_env "need at least $required_space bytes free space," \
                 "have $free_space kbytes" && return

    local dd_bs=65536
    local dd_count=$((file_size / dd_bs))

    # write data into the files
    local i
    local j
    local file
    for i in $(seq 1 $NUMFILES); do
        file=$TDIR/file$i
        yes | dd bs=$dd_bs count=$dd_count of=$file >/dev/null 2>&1 ||
            error "write data into $file failed"
    done
    for i in $(seq 1 $NUMDIRS); do
        for j in $(seq 1 $NUMFILES); do
            file=$TDIR/dir$i/file$j
            yes | dd bs=$dd_bs count=$dd_count of=$file \
                >/dev/null 2>&1 ||
                error "write data into $file failed"
        done
    done

    local expected=-1
    [[ $OSTCOUNT -gt 1 ]] && expected=$((OSTCOUNT - 1))

    # lfs_migrate file
    local cmd="$LFS_MIGRATE -y -c $expected $TDIR/file1"
    echo "$cmd"
    eval $cmd || error "$cmd failed"

    check_stripe_count $TDIR/file1 $expected

    # lfs_migrate dir
    cmd="$LFS_MIGRATE -y -c $expected $TDIR/dir1"
    echo "$cmd"
    eval $cmd || error "$cmd failed"

    for j in $(seq 1 $NUMFILES); do
        check_stripe_count $TDIR/dir1/file$j $expected
    done

    # lfs_migrate works with lfs find
    cmd="$LFIND -stripe_count $OSTCOUNT -type f $TDIR |
         $LFS_MIGRATE -y -c $expected"
    echo "$cmd"
    eval $cmd || error "$cmd failed"

    for i in $(seq 2 $NUMFILES); do
        check_stripe_count $TDIR/file$i $expected
    done
    for i in $(seq 2 $NUMDIRS); do
        for j in $(seq 1 $NUMFILES); do
            check_stripe_count $TDIR/dir$i/file$j $expected
        done
    done
}
run_test 56w "check lfs_migrate -c stripe_count works"

test_56x() {
	check_swap_layouts_support && return 0
	[ "$OSTCOUNT" -lt "2" ] &&
		skip_env "need 2 OST, skipping test" && return

	local dir0=$DIR/$tdir/$testnum
	mkdir -p $dir0 || error "creating dir $dir0"

	local ref1=/etc/passwd
	local file1=$dir0/file1

	$SETSTRIPE -c 2 $file1
	cp $ref1 $file1
	$LFS migrate -c 1 $file1 || error "migrate failed rc = $?"
	stripe=$($GETSTRIPE -c $file1)
	[[ $stripe == 1 ]] || error "stripe of $file1 is $stripe != 1"
	cmp $file1 $ref1 || error "content mismatch $file1 differs from $ref1"

	# clean up
	rm -f $file1
}
run_test 56x "lfs migration support"

test_57a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	# note test will not do anything if MDS is not local
	if [ "$(facet_fstype $SINGLEMDS)" != ldiskfs ]; then
		skip "Only applicable to ldiskfs-based MDTs"
		return
	fi

	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	local MNTDEV="osd*.*MDT*.mntdev"
	DEV=$(do_facet $SINGLEMDS lctl get_param -n $MNTDEV)
	[ -z "$DEV" ] && error "can't access $MNTDEV"
	for DEV in $(do_facet $SINGLEMDS lctl get_param -n $MNTDEV); do
		do_facet $SINGLEMDS $DUMPE2FS -h $DEV > $TMP/t57a.dump ||
			error "can't access $DEV"
		DEVISIZE=`awk '/Inode size:/ { print $3 }' $TMP/t57a.dump`
		[ "$DEVISIZE" -gt 128 ] || error "inode size $DEVISIZE"
		rm $TMP/t57a.dump
	done
}
run_test 57a "verify MDS filesystem created with large inodes =="

test_57b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	if [ "$(facet_fstype $SINGLEMDS)" != ldiskfs ]; then
		skip "Only applicable to ldiskfs-based MDTs"
		return
	fi

	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	local dir=$DIR/d57b

	local FILECOUNT=100
	local FILE1=$dir/f1
	local FILEN=$dir/f$FILECOUNT

	rm -rf $dir || error "removing $dir"
	test_mkdir -p $dir || error "creating $dir"
	local num=$(get_mds_dir $dir)
	local mymds=mds$num

	echo "mcreating $FILECOUNT files"
	createmany -m $dir/f 1 $FILECOUNT || \
		error "creating files in $dir"

	# verify that files do not have EAs yet
	$GETSTRIPE $FILE1 2>&1 | grep -q "no stripe" || error "$FILE1 has an EA"
	$GETSTRIPE $FILEN 2>&1 | grep -q "no stripe" || error "$FILEN has an EA"

	sync
	sleep 1
	df $dir  #make sure we get new statfs data
	local MDSFREE=$(do_facet $mymds \
		lctl get_param -n osd*.*MDT000$((num -1)).kbytesfree)
	local MDCFREE=$(lctl get_param -n mdc.*MDT000$((num -1))-mdc-*.kbytesfree)
	echo "opening files to create objects/EAs"
	local FILE
	for FILE in `seq -f $dir/f%g 1 $FILECOUNT`; do
		$OPENFILE -f O_RDWR $FILE > /dev/null 2>&1 || error "opening $FILE"
	done

	# verify that files have EAs now
	$GETSTRIPE $FILE1 | grep -q "obdidx" || error "$FILE1 missing EA"
	$GETSTRIPE $FILEN | grep -q "obdidx" || error "$FILEN missing EA"

	sleep 1  #make sure we get new statfs data
	df $dir
	local MDSFREE2=$(do_facet $mymds \
		lctl get_param -n osd*.*MDT000$((num -1)).kbytesfree)
	local MDCFREE2=$(lctl get_param -n mdc.*MDT000$((num -1))-mdc-*.kbytesfree)
	if [ "$MDCFREE2" -lt "$((MDCFREE - 8))" ]; then
		if [ "$MDSFREE" != "$MDSFREE2" ]; then
			error "MDC before $MDCFREE != after $MDCFREE2"
		else
			echo "MDC before $MDCFREE != after $MDCFREE2"
			echo "unable to confirm if MDS has large inodes"
		fi
	fi
	rm -rf $dir
}
run_test 57b "default LOV EAs are stored inside large inodes ==="

test_58() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -z "$(which wiretest 2>/dev/null)" ] &&
			skip_env "could not find wiretest" && return
	wiretest
}
run_test 58 "verify cross-platform wire constants =============="

test_59() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	echo "touch 130 files"
	createmany -o $DIR/f59- 130
	echo "rm 130 files"
	unlinkmany $DIR/f59- 130
	sync
	# wait for commitment of removal
	wait_delete_completed
}
run_test 59 "verify cancellation of llog records async ========="

TEST60_HEAD="test_60 run $RANDOM"
test_60a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mgs_nodsh && skip "remote MGS with nodsh" && return
        [ ! -f run-llog.sh ] && skip_env "missing subtest run-llog.sh" && return
	log "$TEST60_HEAD - from kernel mode"
	do_facet mgs sh run-llog.sh
}
run_test 60a "llog sanity tests run from kernel module =========="

test_60b() { # bug 6411
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	dmesg > $DIR/$tfile
	LLOG_COUNT=`dmesg | awk "/$TEST60_HEAD/{marker = 1; from_marker = 0;}
				 /llog.test/ {
					 if (marker)
						 from_marker++
					 from_begin++
				 }
				 END {
					 if (marker)
						 print from_marker
					 else
						 print from_begin
				 }"`
	[ $LLOG_COUNT -gt 50 ] && error "CDEBUG_LIMIT not limiting messages ($LLOG_COUNT)"|| true
}
run_test 60b "limit repeated messages from CERROR/CWARN ========"

test_60c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	echo "create 5000 files"
	createmany -o $DIR/f60c- 5000
#define OBD_FAIL_MDS_LLOG_CREATE_FAILED  0x137
	lctl set_param fail_loc=0x80000137
	unlinkmany $DIR/f60c- 5000
	lctl set_param fail_loc=0
}
run_test 60c "unlink file when mds full"

test_60d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	SAVEPRINTK=$(lctl get_param -n printk)

	# verify "lctl mark" is even working"
	MESSAGE="test message ID $RANDOM $$"
	$LCTL mark "$MESSAGE" || error "$LCTL mark failed"
	dmesg | grep -q "$MESSAGE" || error "didn't find debug marker in log"

	lctl set_param printk=0 || error "set lnet.printk failed"
	lctl get_param -n printk | grep emerg || error "lnet.printk dropped emerg"
	MESSAGE="new test message ID $RANDOM $$"
	# Assume here that libcfs_debug_mark_buffer() uses D_WARNING
	$LCTL mark "$MESSAGE" || error "$LCTL mark failed"
	dmesg | grep -q "$MESSAGE" && error "D_WARNING wasn't masked" || true

	lctl set_param -n printk="$SAVEPRINTK"
}
run_test 60d "test printk console message masking"

test_61() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	f="$DIR/f61"
	dd if=/dev/zero of=$f bs=`page_size` count=1
	cancel_lru_locks osc
	$MULTIOP $f OSMWUc || error
	sync
}
run_test 61 "mmap() writes don't make sync hang ================"

# bug 2330 - insufficient obd_match error checking causes LBUG
test_62() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        f="$DIR/f62"
        echo foo > $f
        cancel_lru_locks osc
        lctl set_param fail_loc=0x405
        cat $f && error "cat succeeded, expect -EIO"
        lctl set_param fail_loc=0
}
# This test is now irrelevant (as of bug 10718 inclusion), we no longer
# match every page all of the time.
#run_test 62 "verify obd_match failure doesn't LBUG (should -EIO)"

# bug 2319 - oig_wait() interrupted causes crash because of invalid waitq.
test_63a() {	# was test_63
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	MAX_DIRTY_MB=`lctl get_param -n osc.*.max_dirty_mb | head -n 1`
	lctl set_param -n osc.*.max_dirty_mb 0
	for i in `seq 10` ; do
		dd if=/dev/zero of=$DIR/f63 bs=8k &
		sleep 5
		kill $!
		sleep 1
	done

	lctl set_param -n osc.*.max_dirty_mb $MAX_DIRTY_MB
	rm -f $DIR/f63 || true
}
run_test 63a "Verify oig_wait interruption does not crash ======="

# bug 2248 - async write errors didn't return to application on sync
# bug 3677 - async write errors left page locked
test_63b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	debugsave
	lctl set_param debug=-1

	# ensure we have a grant to do async writes
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=1
	rm $DIR/$tfile

	#define OBD_FAIL_OSC_BRW_PREP_REQ        0x406
	lctl set_param fail_loc=0x80000406
	$MULTIOP $DIR/$tfile Owy && \
		error "sync didn't return ENOMEM"
	sync; sleep 2; sync	# do a real sync this time to flush page
	lctl get_param -n llite.*.dump_page_cache | grep locked && \
		error "locked page left in cache after async error" || true
	debugrestore
}
run_test 63b "async write errors should be returned to fsync ==="

test_64a () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	df $DIR
	lctl get_param -n osc.*[oO][sS][cC][_-]*.cur* | grep "[0-9]"
}
run_test 64a "verify filter grant calculations (in kernel) ====="

test_64b () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ ! -f oos.sh ] && skip_env "missing subtest oos.sh" && return
	sh oos.sh $MOUNT
}
run_test 64b "check out-of-space detection on client ==========="

test_64c() {
	$LCTL set_param osc.*OST0000-osc-[^mM]*.cur_grant_bytes=0
}
run_test 64c "verify grant shrink ========================------"

# bug 1414 - set/get directories' stripe info
test_65a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	touch $DIR/$tdir/f1
	$LVERIFY $DIR/$tdir $DIR/$tdir/f1 || error "lverify failed"
}
run_test 65a "directory with no stripe info ===================="

test_65b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE -S $((STRIPESIZE * 2)) -i 0 -c 1 $DIR/$tdir ||
						error "setstripe"
	touch $DIR/$tdir/f2
	$LVERIFY $DIR/$tdir $DIR/$tdir/f2 || error "lverify failed"
}
run_test 65b "directory setstripe -S $((STRIPESIZE * 2)) -i 0 -c 1"

test_65c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	if [ $OSTCOUNT -gt 1 ]; then
		test_mkdir -p $DIR/$tdir
		$SETSTRIPE -S $(($STRIPESIZE * 4)) -i 1 \
			-c $(($OSTCOUNT - 1)) $DIR/$tdir || error "setstripe"
		touch $DIR/$tdir/f3
		$LVERIFY $DIR/$tdir $DIR/$tdir/f3 || error "lverify failed"
	fi
}
run_test 65c "directory setstripe -S $((STRIPESIZE*4)) -i 1 -c $((OSTCOUNT-1))"

test_65d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	if [ $STRIPECOUNT -le 0 ]; then
		sc=1
	elif [ $STRIPECOUNT -gt 2000 ]; then
#LOV_MAX_STRIPE_COUNT is 2000
		[ $OSTCOUNT -gt 2000 ] && sc=2000 || sc=$(($OSTCOUNT - 1))
	else
		sc=$(($STRIPECOUNT - 1))
	fi
	$SETSTRIPE -S $STRIPESIZE -c $sc $DIR/$tdir || error "setstripe"
	touch $DIR/$tdir/f4 $DIR/$tdir/f5
	$LVERIFY $DIR/$tdir $DIR/$tdir/f4 $DIR/$tdir/f5 ||
						error "lverify failed"
}
run_test 65d "directory setstripe -S $STRIPESIZE -c stripe_count"

test_65e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir

	$SETSTRIPE $DIR/$tdir || error "setstripe"
        $GETSTRIPE -v $DIR/$tdir | grep "Default" ||
					error "no stripe info failed"
	touch $DIR/$tdir/f6
	$LVERIFY $DIR/$tdir $DIR/$tdir/f6 || error "lverify failed"
}
run_test 65e "directory setstripe defaults ======================="

test_65f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/${tdir}f
	$RUNAS $SETSTRIPE $DIR/${tdir}f && error "setstripe succeeded" || true
}
run_test 65f "dir setstripe permission (should return error) ==="

test_65g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir
        $SETSTRIPE -S $((STRIPESIZE * 2)) -i 0 -c 1 $DIR/$tdir ||
							error "setstripe"
        $SETSTRIPE -d $DIR/$tdir || error "setstripe"
        $GETSTRIPE -v $DIR/$tdir | grep "Default" ||
		error "delete default stripe failed"
}
run_test 65g "directory setstripe -d ==========================="

test_65h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir
        $SETSTRIPE -S $((STRIPESIZE * 2)) -i 0 -c 1 $DIR/$tdir ||
							error "setstripe"
        test_mkdir -p $DIR/$tdir/dd1
        [ $($GETSTRIPE -c $DIR/$tdir) == $($GETSTRIPE -c $DIR/$tdir/dd1) ] ||
                error "stripe info inherit failed"
}
run_test 65h "directory stripe info inherit ===================="

test_65i() { # bug6367
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        $SETSTRIPE -S 65536 -c -1 $MOUNT
}
run_test 65i "set non-default striping on root directory (bug 6367)="

test_65ia() { # bug12836
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GETSTRIPE $MOUNT || error "getstripe $MOUNT failed"
}
run_test 65ia "getstripe on -1 default directory striping"

test_65ib() { # bug12836
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GETSTRIPE -v $MOUNT || error "getstripe -v $MOUNT failed"
}
run_test 65ib "getstripe -v on -1 default directory striping"

test_65ic() { # bug12836
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$LFS find -mtime -1 $MOUNT > /dev/null || error "find $MOUNT failed"
}
run_test 65ic "new find on -1 default directory striping"

test_65j() { # bug6367
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	sync; sleep 1
	# if we aren't already remounting for each test, do so for this test
	if [ "$CLEANUP" = ":" -a "$I_MOUNTED" = "yes" ]; then
		cleanup || error "failed to unmount"
		setup
	fi
	$SETSTRIPE -d $MOUNT || error "setstripe failed"
}
run_test 65j "set default striping on root directory (bug 6367)="

test_65k() { # bug11679
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$OSTCOUNT" -lt 2 ] && skip_env "too few OSTs" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return

    echo "Check OST status: "
    local MDS_OSCS=`do_facet $SINGLEMDS lctl dl |
              awk '/[oO][sS][cC].*md[ts]/ { print $4 }'`

    for OSC in $MDS_OSCS; do
        echo $OSC "is activate"
        do_facet $SINGLEMDS lctl --device %$OSC activate
    done

    mkdir -p $DIR/$tdir
    for INACTIVE_OSC in $MDS_OSCS; do
        echo "Deactivate: " $INACTIVE_OSC
        do_facet $SINGLEMDS lctl --device %$INACTIVE_OSC deactivate
        for STRIPE_OSC in $MDS_OSCS; do
            OST=`osc_to_ost $STRIPE_OSC`
            IDX=`do_facet $SINGLEMDS lctl get_param -n lov.*md*.target_obd |
                 awk -F: /$OST/'{ print $1 }' | head -n 1`

            [ -f $DIR/$tdir/$IDX ] && continue
            echo "$SETSTRIPE -i $IDX -c 1 $DIR/$tdir/$IDX"
            $SETSTRIPE -i $IDX -c 1 $DIR/$tdir/$IDX
            RC=$?
            [ $RC -ne 0 ] && error "setstripe should have succeeded"
        done
        rm -f $DIR/$tdir/*
        echo $INACTIVE_OSC "is Activate."
        do_facet $SINGLEMDS lctl --device  %$INACTIVE_OSC activate
    done
}
run_test 65k "validate manual striping works properly with deactivated OSCs"

test_65l() { # bug 12836
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir/test_dir
	$SETSTRIPE -c -1 $DIR/$tdir/test_dir
	$LFS find -mtime -1 $DIR/$tdir >/dev/null
}
run_test 65l "lfs find on -1 stripe dir ========================"

# bug 2543 - update blocks count on client
test_66() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	COUNT=${COUNT:-8}
	dd if=/dev/zero of=$DIR/f66 bs=1k count=$COUNT
	sync; sync_all_data; sync; sync_all_data
	cancel_lru_locks osc
	BLOCKS=`ls -s $DIR/f66 | awk '{ print $1 }'`
	[ $BLOCKS -ge $COUNT ] || error "$DIR/f66 blocks $BLOCKS < $COUNT"
}
run_test 66 "update inode blocks count on client ==============="

LLOOP=
LLITELOOPLOAD=
cleanup_68() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	trap 0
	if [ ! -z "$LLOOP" ]; then
		if swapon -s | grep -q $LLOOP; then
			swapoff $LLOOP || error "swapoff failed"
		fi

		$LCTL blockdev_detach $LLOOP || error "detach failed"
		rm -f $LLOOP
		unset LLOOP
	fi
	if [ ! -z "$LLITELOOPLOAD" ]; then
		rmmod llite_lloop
		unset LLITELOOPLOAD
	fi
	rm -f $DIR/f68*
}

meminfo() {
	awk '($1 == "'$1':") { print $2 }' /proc/meminfo
}

swap_used() {
	swapon -s | awk '($1 == "'$1'") { print $4 }'
}

# test case for lloop driver, basic function
test_68a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$UID" != 0 ] && skip_env "must run as root" && return
	llite_lloop_enabled || \
 		{ skip_env "llite_lloop module disabled" && return; }

	trap cleanup_68 EXIT

	if ! module_loaded llite_lloop; then
		if load_module llite/llite_lloop; then
			LLITELOOPLOAD=yes
		else
			skip_env "can't find module llite_lloop"
			return
		fi
	fi

	LLOOP=$TMP/lloop.`date +%s`.`date +%N`
	dd if=/dev/zero of=$DIR/f68a bs=4k count=1024
	$LCTL blockdev_attach $DIR/f68a $LLOOP || error "attach failed"

	directio rdwr $LLOOP 0 1024 4096 || error "direct write failed"
	directio rdwr $LLOOP 0 1025 4096 && error "direct write should fail"

	cleanup_68
}
run_test 68a "lloop driver - basic test ========================"

# excercise swapping to lustre by adding a high priority swapfile entry
# and then consuming memory until it is used.
test_68b() {  # was test_68
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$UID" != 0 ] && skip_env "must run as root" && return
	lctl get_param -n devices | grep -q obdfilter && \
		skip "local OST" && return

	grep -q llite_lloop /proc/modules
	[ $? -ne 0 ] && skip "can't find module llite_lloop" && return

	[ -z "`$LCTL list_nids | grep -v tcp`" ] && \
		skip "can't reliably test swap with TCP" && return

	MEMTOTAL=`meminfo MemTotal`
	NR_BLOCKS=$((MEMTOTAL>>8))
	[[ $NR_BLOCKS -le 2048 ]] && NR_BLOCKS=2048

	LLOOP=$TMP/lloop.`date +%s`.`date +%N`
	dd if=/dev/zero of=$DIR/f68b bs=64k seek=$NR_BLOCKS count=1
	mkswap $DIR/f68b

	$LCTL blockdev_attach $DIR/f68b $LLOOP || error "attach failed"

	trap cleanup_68 EXIT

	swapon -p 32767 $LLOOP || error "swapon $LLOOP failed"

	echo "before: `swapon -s | grep $LLOOP`"
	$MEMHOG $MEMTOTAL || error "error allocating $MEMTOTAL kB"
	echo "after: `swapon -s | grep $LLOOP`"
	SWAPUSED=`swap_used $LLOOP`

	cleanup_68

	[ $SWAPUSED -eq 0 ] && echo "no swap used???" || true
}
run_test 68b "support swapping to Lustre ========================"

# bug5265, obdfilter oa2dentry return -ENOENT
# #define OBD_FAIL_OST_ENOENT 0x217
test_69() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	f="$DIR/$tfile"
	$SETSTRIPE -c 1 -i 0 $f

	$DIRECTIO write ${f}.2 0 1 || error "directio write error"

	do_facet ost1 lctl set_param fail_loc=0x217
	$TRUNCATE $f 1 # vmtruncate() will ignore truncate() error.
	$DIRECTIO write $f 0 2 && error "write succeeded, expect -ENOENT"

	do_facet ost1 lctl set_param fail_loc=0
	$DIRECTIO write $f 0 2 || error "write error"

	cancel_lru_locks osc
	$DIRECTIO read $f 0 1 || error "read error"

	do_facet ost1 lctl set_param fail_loc=0x217
	$DIRECTIO read $f 1 1 && error "read succeeded, expect -ENOENT"

	do_facet ost1 lctl set_param fail_loc=0
	rm -f $f
}
run_test 69 "verify oa2dentry return -ENOENT doesn't LBUG ======"

test_71() {
    test_mkdir -p $DIR/$tdir
    sh rundbench -C -D $DIR/$tdir 2 || error "dbench failed!"
}
run_test 71 "Running dbench on lustre (don't segment fault) ===="

test_72a() { # bug 5695 - Test that on 2.6 remove_suid works properly
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	check_kernel_version 43 || return 0
	[ "$RUNAS_ID" = "$UID" ] && skip_env "RUNAS_ID = UID = $UID -- skipping" && return

        # Check that testing environment is properly set up. Skip if not
        FAIL_ON_ERROR=false check_runas_id_ret $RUNAS_ID $RUNAS_GID $RUNAS || {
                skip_env "User $RUNAS_ID does not exist - skipping"
                return 0
        }
	# We had better clear the $DIR to get enough space for dd
	rm -rf $DIR/*
	touch $DIR/f72
	chmod 777 $DIR/f72
	chmod ug+s $DIR/f72
	$RUNAS dd if=/dev/zero of=$DIR/f72 bs=512 count=1 || error
	# See if we are still setuid/sgid
	test -u $DIR/f72 -o -g $DIR/f72 && error "S/gid is not dropped on write"
	# Now test that MDS is updated too
	cancel_lru_locks mdc
	test -u $DIR/f72 -o -g $DIR/f72 && error "S/gid is not dropped on MDS"
	rm -f $DIR/f72
}
run_test 72a "Test that remove suid works properly (bug5695) ===="

test_72b() { # bug 24226 -- keep mode setting when size is not changing
	local perm

	[ "$RUNAS_ID" = "$UID" ] && \
		skip_env "RUNAS_ID = UID = $UID -- skipping" && return
	[ "$RUNAS_ID" -eq 0 ] && \
		skip_env "RUNAS_ID = 0 -- skipping" && return

	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	# Check that testing environment is properly set up. Skip if not
	FAIL_ON_ERROR=false check_runas_id_ret $RUNAS_ID $RUNAS_ID $RUNAS || {
		skip_env "User $RUNAS_ID does not exist - skipping"
		return 0
	}
	touch $DIR/${tfile}-f{g,u}
	test_mkdir $DIR/${tfile}-dg
	test_mkdir $DIR/${tfile}-du
	chmod 770 $DIR/${tfile}-{f,d}{g,u}
	chmod g+s $DIR/${tfile}-{f,d}g
	chmod u+s $DIR/${tfile}-{f,d}u
	for perm in 777 2777 4777; do
		$RUNAS chmod $perm $DIR/${tfile}-fg && error "S/gid file allowed improper chmod to $perm"
		$RUNAS chmod $perm $DIR/${tfile}-fu && error "S/uid file allowed improper chmod to $perm"
		$RUNAS chmod $perm $DIR/${tfile}-dg && error "S/gid dir allowed improper chmod to $perm"
		$RUNAS chmod $perm $DIR/${tfile}-du && error "S/uid dir allowed improper chmod to $perm"
	done
	true
}
run_test 72b "Test that we keep mode setting if without file data changed (bug 24226)"

# bug 3462 - multiple simultaneous MDC requests
test_73() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir $DIR/d73-1
	test_mkdir $DIR/d73-2
	multiop_bg_pause $DIR/d73-1/f73-1 O_c || return 1
	pid1=$!

	lctl set_param fail_loc=0x80000129
	$MULTIOP $DIR/d73-1/f73-2 Oc &
	sleep 1
	lctl set_param fail_loc=0

	$MULTIOP $DIR/d73-2/f73-3 Oc &
	pid3=$!

	kill -USR1 $pid1
	wait $pid1 || return 1

	sleep 25

	$CHECKSTAT -t file $DIR/d73-1/f73-1 || return 4
	$CHECKSTAT -t file $DIR/d73-1/f73-2 || return 5
	$CHECKSTAT -t file $DIR/d73-2/f73-3 || return 6

	rm -rf $DIR/d73-*
}
run_test 73 "multiple MDC requests (should not deadlock)"

test_74a() { # bug 6149, 6184
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	#define OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT 0x30e
	#
	# very important to OR with OBD_FAIL_ONCE (0x80000000) -- otherwise it
	# will spin in a tight reconnection loop
	touch $DIR/f74a
	lctl set_param fail_loc=0x8000030e
	# get any lock that won't be difficult - lookup works.
	ls $DIR/f74a
	lctl set_param fail_loc=0
	true
	rm -f $DIR/f74a
}
run_test 74a "ldlm_enqueue freed-export error path, ls (shouldn't LBUG)"

test_74b() { # bug 13310
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	#define OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT 0x30e
	#
	# very important to OR with OBD_FAIL_ONCE (0x80000000) -- otherwise it
	# will spin in a tight reconnection loop
	lctl set_param fail_loc=0x8000030e
	# get a "difficult" lock
	touch $DIR/f74b
	lctl set_param fail_loc=0
	true
	rm -f $DIR/f74b
}
run_test 74b "ldlm_enqueue freed-export error path, touch (shouldn't LBUG)"

test_74c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
#define OBD_FAIL_LDLM_NEW_LOCK
	lctl set_param fail_loc=0x80000319
	touch $DIR/$tfile && error "Touch successful"
	true
}
run_test 74c "ldlm_lock_create error path, (shouldn't LBUG)"

num_inodes() {
	awk '/lustre_inode_cache/ {print $2; exit}' /proc/slabinfo
}

get_inode_slab_tunables() {
	awk '/lustre_inode_cache/ {print $9," ",$10," ",$11; exit}' /proc/slabinfo
}

set_inode_slab_tunables() {
	echo "lustre_inode_cache $1" > /proc/slabinfo
}

test_76() { # Now for bug 20433, added originally in bug 1443
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local SLAB_SETTINGS=`get_inode_slab_tunables`
	local CPUS=`getconf _NPROCESSORS_ONLN`
	# we cannot set limit below 1 which means 1 inode in each
	# per-cpu cache is still allowed
	set_inode_slab_tunables "1 1 0"
	cancel_lru_locks osc
	BEFORE_INODES=`num_inodes`
	echo "before inodes: $BEFORE_INODES"
	local COUNT=1000
	[ "$SLOW" = "no" ] && COUNT=100
	for i in `seq $COUNT`; do
		touch $DIR/$tfile
		rm -f $DIR/$tfile
	done
	cancel_lru_locks osc
	AFTER_INODES=`num_inodes`
	echo "after inodes: $AFTER_INODES"
	local wait=0
	while [ $((AFTER_INODES-1*CPUS)) -gt $BEFORE_INODES ]; do
		sleep 2
		AFTER_INODES=`num_inodes`
		wait=$((wait+2))
		echo "wait $wait seconds inodes: $AFTER_INODES"
		if [ $wait -gt 30 ]; then
			error "inode slab grew from $BEFORE_INODES to $AFTER_INODES"
		fi
	done
	set_inode_slab_tunables "$SLAB_SETTINGS"
}
run_test 76 "confirm clients recycle inodes properly ===="


export ORIG_CSUM=""
set_checksums()
{
	# Note: in sptlrpc modes which enable its own bulk checksum, the
	# original crc32_le bulk checksum will be automatically disabled,
	# and the OBD_FAIL_OSC_CHECKSUM_SEND/OBD_FAIL_OSC_CHECKSUM_RECEIVE
	# will be checked by sptlrpc code against sptlrpc bulk checksum.
	# In this case set_checksums() will not be no-op, because sptlrpc
	# bulk checksum will be enabled all through the test.

	[ "$ORIG_CSUM" ] || ORIG_CSUM=`lctl get_param -n osc.*.checksums | head -n1`
        lctl set_param -n osc.*.checksums $1
	return 0
}

export ORIG_CSUM_TYPE="`lctl get_param -n osc.*osc-[^mM]*.checksum_type |
                        sed 's/.*\[\(.*\)\].*/\1/g' | head -n1`"
CKSUM_TYPES=${CKSUM_TYPES:-"crc32 adler"}
[ "$ORIG_CSUM_TYPE" = "crc32c" ] && CKSUM_TYPES="$CKSUM_TYPES crc32c"
set_checksum_type()
{
	lctl set_param -n osc.*osc-[^mM]*.checksum_type $1
	log "set checksum type to $1"
	return 0
}
F77_TMP=$TMP/f77-temp
F77SZ=8
setup_f77() {
	dd if=/dev/urandom of=$F77_TMP bs=1M count=$F77SZ || \
		error "error writing to $F77_TMP"
}

test_77a() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	[ ! -f $F77_TMP ] && setup_f77
	set_checksums 1
	dd if=$F77_TMP of=$DIR/$tfile bs=1M count=$F77SZ || error "dd error"
	set_checksums 0
	rm -f $DIR/$tfile
}
run_test 77a "normal checksum read/write operation ============="

test_77b() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	[ ! -f $F77_TMP ] && setup_f77
	#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
	lctl set_param fail_loc=0x80000409
	set_checksums 1
	dd if=$F77_TMP of=$DIR/f77b bs=1M count=$F77SZ conv=sync || \
		error "dd error: $?"
	lctl set_param fail_loc=0
	set_checksums 0
}
run_test 77b "checksum error on client write ===================="

test_77c() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	[ ! -f $DIR/f77b ] && skip "requires 77b - skipping" && return
	set_checksums 1
	for algo in $CKSUM_TYPES; do
		cancel_lru_locks osc
		set_checksum_type $algo
		#define OBD_FAIL_OSC_CHECKSUM_RECEIVE    0x408
		lctl set_param fail_loc=0x80000408
		cmp $F77_TMP $DIR/f77b || error "file compare failed"
		lctl set_param fail_loc=0
	done
	set_checksums 0
	set_checksum_type $ORIG_CSUM_TYPE
	rm -f $DIR/f77b
}
run_test 77c "checksum error on client read ==================="

test_77d() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
	lctl set_param fail_loc=0x80000409
	set_checksums 1
	directio write $DIR/f77 0 $F77SZ $((1024 * 1024)) || \
		error "direct write: rc=$?"
	lctl set_param fail_loc=0
	set_checksums 0
}
run_test 77d "checksum error on OST direct write ==============="

test_77e() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	[ ! -f $DIR/f77 ] && skip "requires 77d - skipping" && return
	#define OBD_FAIL_OSC_CHECKSUM_RECEIVE    0x408
	lctl set_param fail_loc=0x80000408
	set_checksums 1
	cancel_lru_locks osc
	directio read $DIR/f77 0 $F77SZ $((1024 * 1024)) || \
		error "direct read: rc=$?"
	lctl set_param fail_loc=0
	set_checksums 0
}
run_test 77e "checksum error on OST direct read ================"

test_77f() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	set_checksums 1
	for algo in $CKSUM_TYPES; do
		cancel_lru_locks osc
		set_checksum_type $algo
		#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
		lctl set_param fail_loc=0x409
		directio write $DIR/f77 0 $F77SZ $((1024 * 1024)) && \
			error "direct write succeeded"
		lctl set_param fail_loc=0
	done
	set_checksum_type $ORIG_CSUM_TYPE
	set_checksums 0
}
run_test 77f "repeat checksum error on write (expect error) ===="

test_77g() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	[ ! -f $F77_TMP ] && setup_f77

	$SETSTRIPE -c 1 -i 0 $DIR/f77g
	#define OBD_FAIL_OST_CHECKSUM_RECEIVE       0x21a
	do_facet ost1 lctl set_param fail_loc=0x8000021a
	set_checksums 1
	dd if=$F77_TMP of=$DIR/f77g bs=1M count=$F77SZ || \
		error "write error: rc=$?"
	do_facet ost1 lctl set_param fail_loc=0
	set_checksums 0
}
run_test 77g "checksum error on OST write ======================"

test_77h() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	[ ! -f $DIR/f77g ] && skip "requires 77g - skipping" && return
	cancel_lru_locks osc
	#define OBD_FAIL_OST_CHECKSUM_SEND          0x21b
	do_facet ost1 lctl set_param fail_loc=0x8000021b
	set_checksums 1
	cmp $F77_TMP $DIR/f77g || error "file compare failed"
	do_facet ost1 lctl set_param fail_loc=0
	set_checksums 0
}
run_test 77h "checksum error on OST read ======================="

test_77i() { # bug 13805
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	#define OBD_FAIL_OSC_CONNECT_CKSUM       0x40b
	lctl set_param fail_loc=0x40b
	remount_client $MOUNT
	lctl set_param fail_loc=0
	for VALUE in `lctl get_param osc.*osc-[^mM]*.checksum_type`; do
		PARAM=`echo ${VALUE[0]} | cut -d "=" -f1`
		algo=`lctl get_param -n $PARAM | sed 's/.*\[\(.*\)\].*/\1/g'`
		[ "$algo" = "adler" ] || error "algo set to $algo instead of adler"
	done
	remount_client $MOUNT
}
run_test 77i "client not supporting OSD_CONNECT_CKSUM =========="

test_77j() { # bug 13805
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	$GSS && skip "could not run with gss" && return
	#define OBD_FAIL_OSC_CKSUM_ADLER_ONLY    0x40c
	lctl set_param fail_loc=0x40c
	remount_client $MOUNT
	lctl set_param fail_loc=0
	sleep 2 # wait async osc connect to finish
	for VALUE in `lctl get_param osc.*osc-[^mM]*.checksum_type`; do
                PARAM=`echo ${VALUE[0]} | cut -d "=" -f1`
		algo=`lctl get_param -n $PARAM | sed 's/.*\[\(.*\)\].*/\1/g'`
		[ "$algo" = "adler" ] || error "algo set to $algo instead of adler"
	done
	remount_client $MOUNT
}
run_test 77j "client only supporting ADLER32 ===================="

[ "$ORIG_CSUM" ] && set_checksums $ORIG_CSUM || true
rm -f $F77_TMP
unset F77_TMP

test_78() { # bug 10901
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost || { skip_env "local OST" && return; }

	NSEQ=5
	F78SIZE=$(($(awk '/MemFree:/ { print $2 }' /proc/meminfo) / 1024))
	echo "MemFree: $F78SIZE, Max file size: $MAXFREE"
	MEMTOTAL=$(($(awk '/MemTotal:/ { print $2 }' /proc/meminfo) / 1024))
	echo "MemTotal: $MEMTOTAL"
# reserve 256MB of memory for the kernel and other running processes,
# and then take 1/2 of the remaining memory for the read/write buffers.
    if [ $MEMTOTAL -gt 512 ] ;then
        MEMTOTAL=$(((MEMTOTAL - 256 ) / 2))
    else
        # for those poor memory-starved high-end clusters...
        MEMTOTAL=$((MEMTOTAL / 2))
    fi
	echo "Mem to use for directio: $MEMTOTAL"
	[ $F78SIZE -gt $MEMTOTAL ] && F78SIZE=$MEMTOTAL
	[ $F78SIZE -gt 512 ] && F78SIZE=512
	[ $F78SIZE -gt $((MAXFREE / 1024)) ] && F78SIZE=$((MAXFREE / 1024))
	SMALLESTOST=`lfs df $DIR |grep OST | awk '{print $4}' |sort -n |head -1`
	echo "Smallest OST: $SMALLESTOST"
	[ $SMALLESTOST -lt 10240 ] && \
		skip "too small OSTSIZE, useless to run large O_DIRECT test" && return 0

	[ $F78SIZE -gt $((SMALLESTOST * $OSTCOUNT / 1024 - 80)) ] && \
		F78SIZE=$((SMALLESTOST * $OSTCOUNT / 1024 - 80))

	[ "$SLOW" = "no" ] && NSEQ=1 && [ $F78SIZE -gt 32 ] && F78SIZE=32
	echo "File size: $F78SIZE"
	$SETSTRIPE -c $OSTCOUNT $DIR/$tfile || error "setstripe failed"
 	for i in `seq 1 $NSEQ`
 	do
 		FSIZE=$(($F78SIZE / ($NSEQ - $i + 1)))
 		echo directIO rdwr round $i of $NSEQ
  	 	$DIRECTIO rdwr $DIR/$tfile 0 $FSIZE 1048576||error "rdwr failed"
  	done

	rm -f $DIR/$tfile
}
run_test 78 "handle large O_DIRECT writes correctly ============"

test_79() { # bug 12743
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	wait_delete_completed

        BKTOTAL=$(calc_osc_kbytes kbytestotal)
        BKFREE=$(calc_osc_kbytes kbytesfree)
        BKAVAIL=$(calc_osc_kbytes kbytesavail)

        STRING=`df -P $MOUNT | tail -n 1 | awk '{print $2","$3","$4}'`
        DFTOTAL=`echo $STRING | cut -d, -f1`
        DFUSED=`echo $STRING  | cut -d, -f2`
        DFAVAIL=`echo $STRING | cut -d, -f3`
        DFFREE=$(($DFTOTAL - $DFUSED))

        ALLOWANCE=$((64 * $OSTCOUNT))

        if [ $DFTOTAL -lt $(($BKTOTAL - $ALLOWANCE)) ] ||
           [ $DFTOTAL -gt $(($BKTOTAL + $ALLOWANCE)) ] ; then
                error "df total($DFTOTAL) mismatch OST total($BKTOTAL)"
        fi
        if [ $DFFREE -lt $(($BKFREE - $ALLOWANCE)) ] ||
           [ $DFFREE -gt $(($BKFREE + $ALLOWANCE)) ] ; then
                error "df free($DFFREE) mismatch OST free($BKFREE)"
        fi
        if [ $DFAVAIL -lt $(($BKAVAIL - $ALLOWANCE)) ] ||
           [ $DFAVAIL -gt $(($BKAVAIL + $ALLOWANCE)) ] ; then
                error "df avail($DFAVAIL) mismatch OST avail($BKAVAIL)"
        fi
}
run_test 79 "df report consistency check ======================="

test_80() { # bug 10718
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        # relax strong synchronous semantics for slow backends like ZFS
        local soc="obdfilter.*.sync_on_lock_cancel"
        local soc_old=$(do_facet ost1 lctl get_param -n $soc | head -n1)
        local hosts=
        if [ "$soc_old" != "never" -a "$(facet_fstype ost1)" != "ldiskfs" ]; then
                hosts=$(for host in $(seq -f "ost%g" 1 $OSTCOUNT); do
                          facet_active_host $host; done | sort -u)
                do_nodes $hosts lctl set_param $soc=never
        fi

        dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 seek=1M
        sync; sleep 1; sync
        local BEFORE=`date +%s`
        cancel_lru_locks osc
        local AFTER=`date +%s`
        local DIFF=$((AFTER-BEFORE))
        if [ $DIFF -gt 1 ] ; then
                error "elapsed for 1M@1T = $DIFF"
        fi

        [ -n "$hosts" ] && do_nodes $hosts lctl set_param $soc=$soc_old

        rm -f $DIR/$tfile
}
run_test 80 "Page eviction is equally fast at high offsets too  ===="

test_81a() { # LU-456
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        remote_ost_nodsh && skip "remote OST with nodsh" && return
        # define OBD_FAIL_OST_MAPBLK_ENOSPC    0x228
        # MUST OR with the OBD_FAIL_ONCE (0x80000000)
        do_facet ost1 lctl set_param fail_loc=0x80000228

        # write should trigger a retry and success
        $SETSTRIPE -i 0 -c 1 $DIR/$tfile
        $MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?
        if [ $RC -ne 0 ] ; then
                error "write should success, but failed for $RC"
        fi
}
run_test 81a "OST should retry write when get -ENOSPC ==============="

test_81b() { # LU-456
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        remote_ost_nodsh && skip "remote OST with nodsh" && return
        # define OBD_FAIL_OST_MAPBLK_ENOSPC    0x228
        # Don't OR with the OBD_FAIL_ONCE (0x80000000)
        do_facet ost1 lctl set_param fail_loc=0x228

        # write should retry several times and return -ENOSPC finally
        $SETSTRIPE -i 0 -c 1 $DIR/$tfile
        $MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?
        ENOSPC=28
        if [ $RC -ne $ENOSPC ] ; then
                error "dd should fail for -ENOSPC, but succeed."
        fi
}
run_test 81b "OST should return -ENOSPC when retry still fails ======="

test_82() { # LU-1031
	dd if=/dev/zero of=$DIR/$tfile bs=1M count=10
	local gid1=14091995
	local gid2=16022000

	multiop_bg_pause $DIR/$tfile OG${gid1}_g${gid1}c || return 1
	local MULTIPID1=$!
	multiop_bg_pause $DIR/$tfile O_G${gid2}r10g${gid2}c || return 2
	local MULTIPID2=$!
	kill -USR1 $MULTIPID2
	sleep 2
	if [[ `ps h -o comm -p $MULTIPID2` == "" ]]; then
		error "First grouplock does not block second one"
	else
		echo "Second grouplock blocks first one"
	fi
	kill -USR1 $MULTIPID1
	wait $MULTIPID1
	wait $MULTIPID2
}
run_test 82 "Basic grouplock test ==============================="

test_99a() {
        [ -z "$(which cvs 2>/dev/null)" ] && skip_env "could not find cvs" && \
	    return
	test_mkdir -p $DIR/d99cvsroot
	chown $RUNAS_ID $DIR/d99cvsroot
	local oldPWD=$PWD	# bug 13584, use $TMP as working dir
	cd $TMP

	$RUNAS cvs -d $DIR/d99cvsroot init || error
	cd $oldPWD
}
run_test 99a "cvs init ========================================="

test_99b() {
        [ -z "$(which cvs 2>/dev/null)" ] && skip_env "could not find cvs" && return
	[ ! -d $DIR/d99cvsroot ] && test_99a
	cd /etc/init.d
	# some versions of cvs import exit(1) when asked to import links or
	# files they can't read.  ignore those files.
	TOIGNORE=$(find . -type l -printf '-I %f\n' -o \
			! -perm +4 -printf '-I %f\n')
	$RUNAS cvs -d $DIR/d99cvsroot import -m "nomesg" $TOIGNORE \
		d99reposname vtag rtag
}
run_test 99b "cvs import ======================================="

test_99c() {
        [ -z "$(which cvs 2>/dev/null)" ] && skip_env "could not find cvs" && return
	[ ! -d $DIR/d99cvsroot ] && test_99b
	cd $DIR
	test_mkdir -p $DIR/d99reposname
	chown $RUNAS_ID $DIR/d99reposname
	$RUNAS cvs -d $DIR/d99cvsroot co d99reposname
}
run_test 99c "cvs checkout ====================================="

test_99d() {
        [ -z "$(which cvs 2>/dev/null)" ] && skip_env "could not find cvs" && return
	[ ! -d $DIR/d99cvsroot ] && test_99c
	cd $DIR/d99reposname
	$RUNAS touch foo99
	$RUNAS cvs add -m 'addmsg' foo99
}
run_test 99d "cvs add =========================================="

test_99e() {
        [ -z "$(which cvs 2>/dev/null)" ] && skip_env "could not find cvs" && return
	[ ! -d $DIR/d99cvsroot ] && test_99c
	cd $DIR/d99reposname
	$RUNAS cvs update
}
run_test 99e "cvs update ======================================="

test_99f() {
        [ -z "$(which cvs 2>/dev/null)" ] && skip_env "could not find cvs" && return
	[ ! -d $DIR/d99cvsroot ] && test_99d
	cd $DIR/d99reposname
	$RUNAS cvs commit -m 'nomsg' foo99
    rm -fr $DIR/d99cvsroot
}
run_test 99f "cvs commit ======================================="

test_100() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$NETTYPE" = tcp ] || \
		{ skip "TCP secure port test, not useful for NETTYPE=$NETTYPE" && \
			return ; }

	remote_ost_nodsh && skip "remote OST with nodsh" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	remote_servers || \
		{ skip "useless for local single node setup" && return; }

	netstat -tna | ( rc=1; while read PROT SND RCV LOCAL REMOTE STAT; do
		[ "$PROT" != "tcp" ] && continue
		RPORT=$(echo $REMOTE | cut -d: -f2)
		[ "$RPORT" != "$ACCEPTOR_PORT" ] && continue

		rc=0
		LPORT=`echo $LOCAL | cut -d: -f2`
		if [ $LPORT -ge 1024 ]; then
			echo "bad: $PROT $SND $RCV $LOCAL $REMOTE $STAT"
			netstat -tna
			error_exit "local: $LPORT > 1024, remote: $RPORT"
		fi
	done
	[ "$rc" = 0 ] || error_exit "privileged port not found" )
}
run_test 100 "check local port using privileged port ==========="

function get_named_value()
{
    local tag

    tag=$1
    while read ;do
        line=$REPLY
        case $line in
        $tag*)
            echo $line | sed "s/^$tag[ ]*//"
            break
            ;;
        esac
    done
}

export CACHE_MAX=$($LCTL get_param -n llite.*.max_cached_mb |
		   awk '/^max_cached_mb/ { print $2 }')

cleanup_101a() {
	$LCTL set_param -n llite.*.max_cached_mb $CACHE_MAX
	trap 0
}

test_101a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local s
	local discard
	local nreads=10000
	local cache_limit=32

	$LCTL set_param -n osc.*-osc*.rpc_stats 0
	trap cleanup_101a EXIT
	$LCTL set_param -n llite.*.read_ahead_stats 0
	$LCTL set_param -n llite.*.max_cached_mb $cache_limit

	#
	# randomly read 10000 of 64K chunks from file 3x 32MB in size
	#
	echo "nreads: $nreads file size: $((cache_limit * 3))MB"
	$READS -f $DIR/$tfile -s$((cache_limit * 3192 * 1024)) -b65536 -C -n$nreads -t 180

	discard=0
        for s in `$LCTL get_param -n llite.*.read_ahead_stats | \
		get_named_value 'read but discarded' | cut -d" " -f1`; do
			discard=$(($discard + $s))
	done
	cleanup_101a

	if [ $(($discard * 10)) -gt $nreads ] ;then
		$LCTL get_param osc.*-osc*.rpc_stats
		$LCTL get_param llite.*.read_ahead_stats
		error "too many ($discard) discarded pages"
	fi
	rm -f $DIR/$tfile || true
}
run_test 101a "check read-ahead for random reads ================"

setup_test101bc() {
	test_mkdir -p $DIR/$tdir
	STRIPE_SIZE=1048576
	STRIPE_COUNT=$OSTCOUNT
	STRIPE_OFFSET=0

	local list=$(comma_list $(osts_nodes))
	set_osd_param $list '' read_cache_enable 0
	set_osd_param $list '' writethrough_cache_enable 0

	trap cleanup_test101bc EXIT
	# prepare the read-ahead file
	$SETSTRIPE -S $STRIPE_SIZE -i $STRIPE_OFFSET -c $OSTCOUNT $DIR/$tfile

	dd if=/dev/zero of=$DIR/$tfile bs=1024k count=100 2> /dev/null
}

cleanup_test101bc() {
	trap 0
	rm -rf $DIR/$tdir
	rm -f $DIR/$tfile

	local list=$(comma_list $(osts_nodes))
	set_osd_param $list '' read_cache_enable 1
	set_osd_param $list '' writethrough_cache_enable 1
}

calc_total() {
	awk 'BEGIN{total=0}; {total+=$1}; END{print total}'
}

ra_check_101() {
	local READ_SIZE=$1
	local STRIPE_SIZE=1048576
	local RA_INC=1048576
	local STRIDE_LENGTH=$((STRIPE_SIZE/READ_SIZE))
	local FILE_LENGTH=$((64*100))
	local discard_limit=$((((STRIDE_LENGTH - 1)*3/(STRIDE_LENGTH*OSTCOUNT))* \
			     (STRIDE_LENGTH*OSTCOUNT - STRIDE_LENGTH)))
	DISCARD=`$LCTL get_param -n llite.*.read_ahead_stats | \
			get_named_value 'read but discarded' | \
			cut -d" " -f1 | calc_total`
	if [ $DISCARD -gt $discard_limit ]; then
		$LCTL get_param llite.*.read_ahead_stats
		error "Too many ($DISCARD) discarded pages with size (${READ_SIZE})"
	else
		echo "Read-ahead success for size ${READ_SIZE}"
	fi
}

test_101b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping stride IO stride-ahead test" && return
	local STRIPE_SIZE=1048576
	local STRIDE_SIZE=$((STRIPE_SIZE*OSTCOUNT))
	local FILE_LENGTH=$((STRIPE_SIZE*100))
	local ITERATION=$((FILE_LENGTH/STRIDE_SIZE))
	# prepare the read-ahead file
	setup_test101bc
	cancel_lru_locks osc
	for BIDX in 2 4 8 16 32 64 128 256
	do
		local BSIZE=$((BIDX*4096))
		local READ_COUNT=$((STRIPE_SIZE/BSIZE))
		local STRIDE_LENGTH=$((STRIDE_SIZE/BSIZE))
		local OFFSET=$((STRIPE_SIZE/BSIZE*(OSTCOUNT - 1)))
		$LCTL set_param -n llite.*.read_ahead_stats 0
		$READS -f $DIR/$tfile  -l $STRIDE_LENGTH -o $OFFSET \
			      -s $FILE_LENGTH -b $STRIPE_SIZE -a $READ_COUNT -n $ITERATION
		cancel_lru_locks osc
		ra_check_101 $BSIZE
	done
	cleanup_test101bc
	true
}
run_test 101b "check stride-io mode read-ahead ================="

test_101c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local STRIPE_SIZE=1048576
	local FILE_LENGTH=$((STRIPE_SIZE*100))
	local nreads=10000
	local osc

    setup_test101bc

    cancel_lru_locks osc
    $LCTL set_param osc.*.rpc_stats 0
    $READS -f $DIR/$tfile -s$FILE_LENGTH -b65536 -n$nreads -t 180
    for osc in $($LCTL get_param -N osc.*); do
        if [ "$osc" == "osc.num_refs" ]; then
            continue
        fi

        local lines=$($LCTL get_param -n ${osc}.rpc_stats | wc | awk '{print $1}')
        if [ $lines -le 20 ]; then
            continue
        fi

        local rpc4k=$($LCTL get_param -n ${osc}.rpc_stats |
                                     awk '$1 == "1:" { print $2; exit; }')
        local rpc8k=$($LCTL get_param -n ${osc}.rpc_stats |
                                     awk '$1 == "2:" { print $2; exit; }')
        local rpc16k=$($LCTL get_param -n ${osc}.rpc_stats |
                                     awk '$1 == "4:" { print $2; exit; }')
        local rpc32k=$($LCTL get_param -n ${osc}.rpc_stats |
                                     awk '$1 == "8:" { print $2; exit; }')

        [ $rpc4k != 0 ]  && error "Small 4k read IO ${rpc4k}!"
        [ $rpc8k != 0 ]  && error "Small 8k read IO ${rpc8k}!"
        [ $rpc16k != 0 ] && error "Small 16k read IO ${rpc16k}!"
        [ $rpc32k != 0 ] && error "Small 32k read IO ${rpc32k}!"
        echo "${osc} rpc check passed!"
    done
    cleanup_test101bc
    true
}
run_test 101c "check stripe_size aligned read-ahead ================="

set_read_ahead() {
   $LCTL get_param -n llite.*.max_read_ahead_mb | head -n 1
   $LCTL set_param -n llite.*.max_read_ahead_mb $1 > /dev/null 2>&1
}

test_101d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local file=$DIR/$tfile
	local size=${FILESIZE_101c:-500}
	local ra_MB=${READAHEAD_MB:-40}

	local space=$(df -P $DIR | tail -n 1 | awk '{ print $4 }')
	[ $space -gt $((size * 1024)) ] ||
		{ skip "Need free space ${size}M, have $space" && return; }

    echo Creating ${size}M test file $file
    dd if=/dev/zero of=$file bs=1M count=$size || error "dd failed"
    echo Cancel LRU locks on lustre client to flush the client cache
    cancel_lru_locks osc

    echo Disable read-ahead
    local old_READAHEAD=$(set_read_ahead 0)

    echo Reading the test file $file with read-ahead disabled
    time_ra_OFF=$(do_and_time "dd if=$file of=/dev/null bs=1M count=$size")

    echo Cancel LRU locks on lustre client to flush the client cache
    cancel_lru_locks osc
    echo Enable read-ahead with ${ra_MB}MB
    set_read_ahead $ra_MB

    echo Reading the test file $file with read-ahead enabled
    time_ra_ON=$(do_and_time "dd if=$file of=/dev/null bs=1M count=$size")

    echo read-ahead disabled time read $time_ra_OFF
    echo read-ahead enabled  time read $time_ra_ON

	set_read_ahead $old_READAHEAD
	rm -f $file
	wait_delete_completed

    [ $time_ra_ON -lt $time_ra_OFF ] ||
        error "read-ahead enabled  time read (${time_ra_ON}s) is more than
               read-ahead disabled time read (${time_ra_OFF}s) filesize ${size}M"
}
run_test 101d "file read with and without read-ahead enabled  ================="

test_101e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
    local file=$DIR/$tfile
    local size=500  #KB
    local count=100
    local blksize=1024

    local space=$(df -P $DIR | tail -n 1 | awk '{ print $4 }')
    local need_space=$((count * size))
    [ $space -gt $need_space ] ||
        { skip_env "Need free space $need_space, have $space" && return; }

    echo Creating $count ${size}K test files
    for ((i = 0; i < $count; i++)); do
        dd if=/dev/zero of=${file}_${i} bs=$blksize count=$size 2>/dev/null
    done

    echo Cancel LRU locks on lustre client to flush the client cache
    cancel_lru_locks osc

    echo Reset readahead stats
    $LCTL set_param -n llite.*.read_ahead_stats 0

    for ((i = 0; i < $count; i++)); do
        dd if=${file}_${i} of=/dev/null bs=$blksize count=$size 2>/dev/null
    done

    local miss=$($LCTL get_param -n llite.*.read_ahead_stats | \
          get_named_value 'misses' | cut -d" " -f1 | calc_total)

    for ((i = 0; i < $count; i++)); do
        rm -rf ${file}_${i} 2>/dev/null
    done

    #10000 means 20% reads are missing in readahead
    [ $miss -lt 10000 ] ||  error "misses too much for small reads"
}
run_test 101e "check read-ahead for small read(1k) for small files(500k)"

cleanup_test101f() {
    trap 0
    $LCTL set_param -n llite.*.max_read_ahead_whole_mb $MAX_WHOLE_MB
    rm -rf $DIR/$tfile 2>/dev/null
}

test_101f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
    local file=$DIR/$tfile
    local nreads=1000

    MAX_WHOLE_MB=$($LCTL get_param -n llite.*.max_read_ahead_whole_mb)
    $LCTL set_param -n llite.*.max_read_ahead_whole_mb 2
    dd if=/dev/zero of=${file} bs=2097152 count=1 2>/dev/null
    trap cleanup_test101f EXIT

    echo Cancel LRU locks on lustre client to flush the client cache
    cancel_lru_locks osc

    echo Reset readahead stats
    $LCTL set_param -n llite.*.read_ahead_stats 0
    # Random read in a 2M file, because max_read_ahead_whole_mb = 2M,
    # readahead should read in 2M file on second read, so only miss
    # 2 pages.
    echo Random 4K reads on 2M file for 1000 times
    $READS -f $file -s 2097152 -b 4096 -n $nreads

    echo checking missing pages
    local miss=$($LCTL get_param -n llite.*.read_ahead_stats |
          get_named_value 'misses' | cut -d" " -f1 | calc_total)

    [ $miss -lt 3 ] || error "misses too much pages!"
    cleanup_test101f
}
run_test 101f "check read-ahead for max_read_ahead_whole_mb"

setup_test102() {
	test_mkdir -p $DIR/$tdir
	chown $RUNAS_ID $DIR/$tdir
	STRIPE_SIZE=65536
	STRIPE_OFFSET=1
	STRIPE_COUNT=$OSTCOUNT
	[ $OSTCOUNT -gt 4 ] && STRIPE_COUNT=4

	trap cleanup_test102 EXIT
	cd $DIR
	$1 $SETSTRIPE -S $STRIPE_SIZE -i $STRIPE_OFFSET -c $STRIPE_COUNT $tdir
	cd $DIR/$tdir
	for num in 1 2 3 4; do
		for count in $(seq 1 $STRIPE_COUNT); do
			for idx in $(seq 0 $[$STRIPE_COUNT - 1]); do
				local size=`expr $STRIPE_SIZE \* $num`
				local file=file"$num-$idx-$count"
				$1 $SETSTRIPE -S $size -i $idx -c $count $file
			done
		done
	done

	cd $DIR
	$1 $TAR cf $TMP/f102.tar $tdir --xattrs
}

cleanup_test102() {
	trap 0
	rm -f $TMP/f102.tar
	rm -rf $DIR/d0.sanity/d102
}

test_102a() {
	local testfile=$DIR/xattr_testfile

	touch $testfile

	[ "$UID" != 0 ] && skip_env "must run as root" && return
	[ -z "`lctl get_param -n mdc.*-mdc-*.connect_flags | grep xattr`" ] &&
		skip_env "must have user_xattr" && return

	[ -z "$(which setfattr 2>/dev/null)" ] &&
		skip_env "could not find setfattr" && return

	echo "set/get xattr..."
	setfattr -n trusted.name1 -v value1 $testfile || error
	getfattr -n trusted.name1 $testfile 2> /dev/null |
	  grep "trusted.name1=.value1" ||
		error "$testfile missing trusted.name1=value1"

	setfattr -n user.author1 -v author1 $testfile || error
	getfattr -n user.author1 $testfile 2> /dev/null |
	  grep "user.author1=.author1" ||
		error "$testfile missing trusted.author1=author1"

	echo "listxattr..."
	setfattr -n trusted.name2 -v value2 $testfile ||
		error "$testfile unable to set trusted.name2"
	setfattr -n trusted.name3 -v value3 $testfile ||
		error "$testfile unable to set trusted.name3"
	[ $(getfattr -d -m "^trusted" $testfile 2> /dev/null |
	    grep "trusted.name" | wc -l) -eq 3 ] ||
		error "$testfile missing 3 trusted.name xattrs"

	setfattr -n user.author2 -v author2 $testfile ||
		error "$testfile unable to set user.author2"
	setfattr -n user.author3 -v author3 $testfile ||
		error "$testfile unable to set user.author3"
	[ $(getfattr -d -m "^user" $testfile 2> /dev/null |
	    grep "user.author" | wc -l) -eq 3 ] ||
		error "$testfile missing 3 user.author xattrs"

	echo "remove xattr..."
	setfattr -x trusted.name1 $testfile ||
		error "$testfile error deleting trusted.name1"
	getfattr -d -m trusted $testfile 2> /dev/null | grep "trusted.name1" &&
		error "$testfile did not delete trusted.name1 xattr"

	setfattr -x user.author1 $testfile ||
		error "$testfile error deleting user.author1"
	getfattr -d -m user $testfile 2> /dev/null | grep "user.author1" &&
		error "$testfile did not delete trusted.name1 xattr"

	# b10667: setting lustre special xattr be silently discarded
	echo "set lustre special xattr ..."
	setfattr -n "trusted.lov" -v "invalid value" $testfile ||
		error "$testfile allowed setting trusted.lov"
}
run_test 102a "user xattr test =================================="

test_102b() {
	# b10930: get/set/list trusted.lov xattr
	echo "get/set/list trusted.lov xattr ..."
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping 2-stripe test" && return
	local testfile=$DIR/$tfile
	$SETSTRIPE -S 65536 -i 1 -c $OSTCOUNT $testfile ||
		error "setstripe failed"
	local STRIPECOUNT=$($GETSTRIPE -c $testfile) ||
		error "getstripe failed"
	getfattr -d -m "^trusted" $testfile 2> /dev/null | \
	grep "trusted.lov" || error "can't get trusted.lov from $testfile"

	local testfile2=${testfile}2
	local value=`getfattr -n trusted.lov $testfile 2> /dev/null | \
		     grep "trusted.lov" |sed -e 's/[^=]\+=//'`

	$MCREATE $testfile2
	setfattr -n trusted.lov -v $value $testfile2
	local stripe_size=$($GETSTRIPE -S $testfile2)
	local stripe_count=$($GETSTRIPE -c $testfile2)
	[ $stripe_size -eq 65536 ] || error "stripe size $stripe_size != 65536"
	[ $stripe_count -eq $STRIPECOUNT ] ||
		error "stripe count $stripe_count != $STRIPECOUNT"
	rm -f $DIR/$tfile
}
run_test 102b "getfattr/setfattr for trusted.lov EAs ============"

test_102c() {
	# b10930: get/set/list lustre.lov xattr
	echo "get/set/list lustre.lov xattr ..."
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping 2-stripe test" && return
	test_mkdir -p $DIR/$tdir
	chown $RUNAS_ID $DIR/$tdir
	local testfile=$DIR/$tdir/$tfile
	$RUNAS $SETSTRIPE -S 65536 -i 1 -c $OSTCOUNT $testfile ||
		error "setstripe failed"
	local STRIPECOUNT=$($RUNAS $GETSTRIPE -c $testfile) ||
		error "getstripe failed"
	$RUNAS getfattr -d -m "^lustre" $testfile 2> /dev/null | \
	grep "lustre.lov" || error "can't get lustre.lov from $testfile"

	local testfile2=${testfile}2
	local value=`getfattr -n lustre.lov $testfile 2> /dev/null | \
		     grep "lustre.lov" |sed -e 's/[^=]\+=//'  `

	$RUNAS $MCREATE $testfile2
	$RUNAS setfattr -n lustre.lov -v $value $testfile2
	local stripe_size=$($RUNAS $GETSTRIPE -S $testfile2)
	local stripe_count=$($RUNAS $GETSTRIPE -c $testfile2)
	[ $stripe_size -eq 65536 ] || error "stripe size $stripe_size != 65536"
	[ $stripe_count -eq $STRIPECOUNT ] ||
		error "stripe count $stripe_count != $STRIPECOUNT"
}
run_test 102c "non-root getfattr/setfattr for lustre.lov EAs ==========="

compare_stripe_info1() {
	local stripe_index_all_zero=true

	for num in 1 2 3 4; do
		for count in $(seq 1 $STRIPE_COUNT); do
			for offset in $(seq 0 $[$STRIPE_COUNT - 1]); do
				local size=$((STRIPE_SIZE * num))
				local file=file"$num-$offset-$count"
				stripe_size=$(lfs getstripe -S $PWD/$file)
				[ $stripe_size -ne $size ] &&
				    error "$file: size $stripe_size != $size"
				stripe_count=$(lfs getstripe -c $PWD/$file)
				# allow fewer stripes to be created, ORI-601
				[ $stripe_count -lt $(((3 * count + 3) / 4)) ]&&
				    error "$file: count $stripe_count != $count"
				stripe_index=$(lfs getstripe -i $PWD/$file)
				[ $stripe_index -ne 0 ] &&
					stripe_index_all_zero=false
			done
		done
	done
	$stripe_index_all_zero &&
		error "all files are being extracted starting from OST index 0"
	return 0
}

find_lustre_tar() {
	[ -n "$(which tar 2>/dev/null)" ] &&
		strings $(which tar) | grep -q "lustre" && echo tar
}

test_102d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	# b10930: tar test for trusted.lov xattr
	TAR=$(find_lustre_tar)
	[ -z "$TAR" ] && skip_env "lustre-aware tar is not installed" && return
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping N-stripe test" && return
	setup_test102
	test_mkdir -p $DIR/d102d
	$TAR xf $TMP/f102.tar -C $DIR/d102d --xattrs
	cd $DIR/d102d/$tdir
	compare_stripe_info1
}
run_test 102d "tar restore stripe info from tarfile,not keep osts ==========="

test_102f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	# b10930: tar test for trusted.lov xattr
	TAR=$(find_lustre_tar)
	[ -z "$TAR" ] && skip_env "lustre-aware tar is not installed" && return
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping N-stripe test" && return
	setup_test102
	test_mkdir -p $DIR/d102f
	cd $DIR
	$TAR cf - --xattrs $tdir | $TAR xf - --xattrs -C $DIR/d102f
	cd $DIR/d102f/$tdir
	compare_stripe_info1
}
run_test 102f "tar copy files, not keep osts ==========="

grow_xattr() {
	local xsize=${1:-1024}	# in bytes
	local file=$DIR/$tfile

	[ -z $(lctl get_param -n mdc.*.connect_flags | grep xattr) ] &&
		skip "must have user_xattr" && return 0
	[ -z "$(which setfattr 2>/dev/null)" ] &&
		skip_env "could not find setfattr" && return 0
	[ -z "$(which getfattr 2>/dev/null)" ] &&
		skip_env "could not find getfattr" && return 0

	touch $file

	local value="$(generate_string $xsize)"

	local xbig=trusted.big
	log "save $xbig on $file"
	setfattr -n $xbig -v $value $file ||
		error "saving $xbig on $file failed"

	local orig=$(get_xattr_value $xbig $file)
	[[ "$orig" != "$value" ]] && error "$xbig different after saving $xbig"

	local xsml=trusted.sml
	log "save $xsml on $file"
	setfattr -n $xsml -v val $file || error "saving $xsml on $file failed"

	local new=$(get_xattr_value $xbig $file)
	[[ "$new" != "$orig" ]] && error "$xbig different after saving $xsml"

	log "grow $xsml on $file"
	setfattr -n $xsml -v "$value" $file ||
		error "growing $xsml on $file failed"

	new=$(get_xattr_value $xbig $file)
	[[ "$new" != "$orig" ]] && error "$xbig different after growing $xsml"
	log "$xbig still valid after growing $xsml"

	rm -f $file
}

test_102h() { # bug 15777
	grow_xattr 1024
}
run_test 102h "grow xattr from inside inode to external block"

test_102ha() {
	large_xattr_enabled || { skip "large_xattr disabled" && return; }
	grow_xattr $(max_xattr_size)
}
run_test 102ha "grow xattr from inside inode to external inode"

test_102i() { # bug 17038
        touch $DIR/$tfile
        ln -s $DIR/$tfile $DIR/${tfile}link
        getfattr -n trusted.lov $DIR/$tfile || error "lgetxattr on $DIR/$tfile failed"
        getfattr -h -n trusted.lov $DIR/${tfile}link 2>&1 | grep -i "no such attr" || error "error for lgetxattr on $DIR/${tfile}link is not ENODATA"
        rm -f $DIR/$tfile $DIR/${tfile}link
}
run_test 102i "lgetxattr test on symbolic link ============"

test_102j() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	TAR=$(find_lustre_tar)
	[ -z "$TAR" ] && skip_env "lustre-aware tar is not installed" && return
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping N-stripe test" && return
	setup_test102 "$RUNAS"
	test_mkdir -p $DIR/d102j
	chown $RUNAS_ID $DIR/d102j
	$RUNAS $TAR xf $TMP/f102.tar -C $DIR/d102j --xattrs
	cd $DIR/d102j/$tdir
	compare_stripe_info1 "$RUNAS"
}
run_test 102j "non-root tar restore stripe info from tarfile, not keep osts ==="

test_102k() {
        touch $DIR/$tfile
        # b22187 just check that does not crash for regular file.
        setfattr -n trusted.lov $DIR/$tfile
        # b22187 'setfattr -n trusted.lov' should work as remove LOV EA for directories
        local test_kdir=$DIR/d102k
        test_mkdir $test_kdir
        local default_size=`$GETSTRIPE -S $test_kdir`
        local default_count=`$GETSTRIPE -c $test_kdir`
        local default_offset=`$GETSTRIPE -i $test_kdir`
        $SETSTRIPE -S 65536 -i 1 -c $OSTCOUNT $test_kdir ||
                error 'dir setstripe failed'
        setfattr -n trusted.lov $test_kdir
        local stripe_size=`$GETSTRIPE -S $test_kdir`
        local stripe_count=`$GETSTRIPE -c $test_kdir`
        local stripe_offset=`$GETSTRIPE -i $test_kdir`
        [ $stripe_size -eq $default_size ] ||
                error "stripe size $stripe_size != $default_size"
        [ $stripe_count -eq $default_count ] ||
                error "stripe count $stripe_count != $default_count"
        [ $stripe_offset -eq $default_offset ] ||
                error "stripe offset $stripe_offset != $default_offset"
        rm -rf $DIR/$tfile $test_kdir
}
run_test 102k "setfattr without parameter of value shouldn't cause a crash"

test_102l() {
	# LU-532 trusted. xattr is invisible to non-root
	local testfile=$DIR/$tfile

	touch $testfile

	echo "listxattr as user..."
	chown $RUNAS_ID $testfile
	$RUNAS getfattr -d -m '.*' $testfile 2>&1 |
	    grep -q "trusted" &&
		error "$testfile trusted xattrs are user visible"

	return 0;
}
run_test 102l "listxattr filter test =================================="

cleanup_test102

run_acl_subtest()
{
    $LUSTRE/tests/acl/run $LUSTRE/tests/acl/$1.test
    return $?
}

test_103 () {
    [ "$UID" != 0 ] && skip_env "must run as root" && return
    [ -z "$(lctl get_param -n mdc.*-mdc-*.connect_flags | grep acl)" ] && skip "must have acl enabled" && return
    [ -z "$(which setfacl 2>/dev/null)" ] && skip_env "could not find setfacl" && return
    $GSS && skip "could not run under gss" && return

    declare -a identity_old

	for num in $(seq $MDSCOUNT); do
		switch_identity $num true || identity_old[$num]=$?
	done

    SAVE_UMASK=`umask`
    umask 0022
    cd $DIR

    echo "performing cp ..."
    run_acl_subtest cp || error
    echo "performing getfacl-noacl..."
    run_acl_subtest getfacl-noacl || error "getfacl-noacl test failed"
    echo "performing misc..."
    run_acl_subtest misc || error  "misc test failed"
    echo "performing permissions..."
    run_acl_subtest permissions || error "permissions failed"
    echo "performing setfacl..."
    run_acl_subtest setfacl || error  "setfacl test failed"

    # inheritance test got from HP
    echo "performing inheritance..."
    cp $LUSTRE/tests/acl/make-tree . || error "cannot copy make-tree"
    chmod +x make-tree || error "chmod +x failed"
    run_acl_subtest inheritance || error "inheritance test failed"
    rm -f make-tree

	echo "LU-974 ignore umask when acl is enabled..."
	run_acl_subtest 974 || error "LU-974 test failed"
	if [ $MDSCOUNT -ge 2 ]; then
		run_acl_subtest 974_remote ||
			error "LU-974 test failed under remote dir"
	fi

    echo "LU-2561 newly created file is same size as directory..."
    run_acl_subtest 2561 || error "LU-2561 test failed"

    cd $SAVE_PWD
    umask $SAVE_UMASK

	for num in $(seq $MDSCOUNT); do
		if [ "${identity_old[$num]}" = 1 ]; then
			switch_identity $num false || identity_old[$num]=$?
		fi
	done
}
run_test 103 "acl test ========================================="

test_104a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	touch $DIR/$tfile
	lfs df || error "lfs df failed"
	lfs df -ih || error "lfs df -ih failed"
	lfs df -h $DIR || error "lfs df -h $DIR failed"
	lfs df -i $DIR || error "lfs df -i $DIR failed"
	lfs df $DIR/$tfile || error "lfs df $DIR/$tfile failed"
	lfs df -ih $DIR/$tfile || error "lfs df -ih $DIR/$tfile failed"

        OSC=`lctl dl |grep OST0000-osc-[^M] |awk '{print $4}'`
	lctl --device %$OSC deactivate
	lfs df || error "lfs df with deactivated OSC failed"
	lctl --device %$OSC activate
        # wait the osc back to normal
        wait_osc_import_state client ost FULL

	lfs df || error "lfs df with reactivated OSC failed"
	rm -f $DIR/$tfile
}
run_test 104a "lfs df [-ih] [path] test ========================="

test_104b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID -- skipping" && return
	chmod 666 /dev/obd
	denied_cnt=$((`$RUNAS $LFS check servers 2>&1 | grep "Permission denied" | wc -l`))
	if [ $denied_cnt -ne 0 ];
	then
	            error "lfs check servers test failed"
	fi
}
run_test 104b "$RUNAS lfs check servers test ===================="

test_105a() {
	# doesn't work on 2.4 kernels
	touch $DIR/$tfile
	if [ -n "`mount | grep \"$MOUNT.*flock\" | grep -v noflock`" ]; then
		flocks_test 1 on -f $DIR/$tfile || error "fail flock on"
	else
		flocks_test 1 off -f $DIR/$tfile || error "fail flock off"
	fi
	rm -f $DIR/$tfile
}
run_test 105a "flock when mounted without -o flock test ========"

test_105b() {
	touch $DIR/$tfile
	if [ -n "`mount | grep \"$MOUNT.*flock\" | grep -v noflock`" ]; then
		flocks_test 1 on -c $DIR/$tfile || error "fail flock on"
	else
		flocks_test 1 off -c $DIR/$tfile || error "fail flock off"
	fi
	rm -f $DIR/$tfile
}
run_test 105b "fcntl when mounted without -o flock test ========"

test_105c() {
	touch $DIR/$tfile
	if [ -n "`mount | grep \"$MOUNT.*flock\" | grep -v noflock`" ]; then
		flocks_test 1 on -l $DIR/$tfile || error "fail flock on"
	else
		flocks_test 1 off -l $DIR/$tfile || error "fail flock off"
	fi
	rm -f $DIR/$tfile
}
run_test 105c "lockf when mounted without -o flock test ========"

test_105d() { # bug 15924
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	[ -z "`mount | grep \"$MOUNT.*flock\" | grep -v noflock`" ] && \
		skip "mount w/o flock enabled" && return
	#define OBD_FAIL_LDLM_CP_CB_WAIT  0x315
	$LCTL set_param fail_loc=0x80000315
	flocks_test 2 $DIR/$tdir
}
run_test 105d "flock race (should not freeze) ========"

test_105e() { # bug 22660 && 22040
	[ -z "`mount | grep \"$MOUNT.*flock\" | grep -v noflock`" ] && \
		skip "mount w/o flock enabled" && return
	touch $DIR/$tfile
	flocks_test 3 $DIR/$tfile
}
run_test 105e "Two conflicting flocks from same process ======="

test_106() { #bug 10921
	test_mkdir -p $DIR/$tdir
	$DIR/$tdir && error "exec $DIR/$tdir succeeded"
	chmod 777 $DIR/$tdir || error "chmod $DIR/$tdir failed"
}
run_test 106 "attempt exec of dir followed by chown of that dir"

test_107() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        CDIR=`pwd`
        cd $DIR

        local file=core
        rm -f $file

        local save_pattern=$(sysctl -n kernel.core_pattern)
        local save_uses_pid=$(sysctl -n kernel.core_uses_pid)
        sysctl -w kernel.core_pattern=$file
        sysctl -w kernel.core_uses_pid=0

        ulimit -c unlimited
        sleep 60 &
        SLEEPPID=$!

        sleep 1

        kill -s 11 $SLEEPPID
        wait $SLEEPPID
        if [ -e $file ]; then
                size=`stat -c%s $file`
                [ $size -eq 0 ] && error "Fail to create core file $file"
        else
                error "Fail to create core file $file"
        fi
        rm -f $file
        sysctl -w kernel.core_pattern=$save_pattern
        sysctl -w kernel.core_uses_pid=$save_uses_pid
        cd $CDIR
}
run_test 107 "Coredump on SIG"

test_110() {
	test_mkdir -p $DIR/$tdir
	test_mkdir $DIR/$tdir/$(str_repeat 'a' 255) ||
		error "mkdir with 255 char failed"
	test_mkdir $DIR/$tdir/$(str_repeat 'b' 256) &&
		error "mkdir with 256 char should fail, but did not"
	touch $DIR/$tdir/$(str_repeat 'x' 255) ||
		error "create with 255 char failed"
	touch $DIR/$tdir/$(str_repeat 'y' 256) &&
		error "create with 256 char should fail, but did not"

	ls -l $DIR/$tdir
	rm -rf $DIR/$tdir
}
run_test 110 "filename length checking"

test_115() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	OSTIO_pre=$(ps -e|grep ll_ost_io|awk '{print $4}'|sort -n|tail -1|\
	    cut -c11-20)
        [ -z "$OSTIO_pre" ] && skip "no OSS threads" && \
	    return
        echo "Starting with $OSTIO_pre threads"

	NUMTEST=20000
	NUMFREE=`df -i -P $DIR | tail -n 1 | awk '{ print $4 }'`
	[ $NUMFREE -lt $NUMTEST ] && NUMTEST=$(($NUMFREE - 1000))
	echo "$NUMTEST creates/unlinks"
	test_mkdir -p $DIR/$tdir
	createmany -o $DIR/$tdir/$tfile $NUMTEST
	unlinkmany $DIR/$tdir/$tfile $NUMTEST

	OSTIO_post=$(ps -e|grep ll_ost_io|awk '{print $4}'|sort -n|tail -1|\
	    cut -c11-20)

	# don't return an error
        [ $OSTIO_post == $OSTIO_pre ] && echo \
	    "WARNING: No new ll_ost_io threads were created ($OSTIO_pre)" &&
	    echo "This may be fine, depending on what ran before this test" &&
	    echo "and how fast this system is." && return

        echo "Started with $OSTIO_pre threads, ended with $OSTIO_post"
}
run_test 115 "verify dynamic thread creation===================="

free_min_max () {
	wait_delete_completed
	AVAIL=($(lctl get_param -n osc.*[oO][sS][cC]-[^M]*.kbytesavail))
	echo OST kbytes available: ${AVAIL[@]}
	MAXI=0; MAXV=${AVAIL[0]}
	MINI=0; MINV=${AVAIL[0]}
	for ((i = 0; i < ${#AVAIL[@]}; i++)); do
	    #echo OST $i: ${AVAIL[i]}kb
	    if [ ${AVAIL[i]} -gt $MAXV ]; then
		MAXV=${AVAIL[i]}; MAXI=$i
	    fi
	    if [ ${AVAIL[i]} -lt $MINV ]; then
		MINV=${AVAIL[i]}; MINI=$i
	    fi
	done
	echo Min free space: OST $MINI: $MINV
	echo Max free space: OST $MAXI: $MAXV
}

test_116a() { # was previously test_116()
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$OSTCOUNT" -lt "2" ] && skip_env "$OSTCOUNT < 2 OSTs" && return

	echo -n "Free space priority "
	lctl get_param -n lov.*-clilov-*.qos_prio_free
	declare -a AVAIL
	free_min_max
	[ $MINV -gt 960000 ] && skip "too much free space in OST$MINI, skip" &&\
		return

	# generate uneven OSTs
	test_mkdir -p $DIR/$tdir/OST${MINI}
	declare -i FILL
	FILL=$(($MINV / 4))
	echo "Filling 25% remaining space in OST${MINI} with ${FILL}Kb"
	$SETSTRIPE -i $MINI -c 1 $DIR/$tdir/OST${MINI}||error "setstripe failed"
	i=0
	while [ $FILL -gt 0 ]; do
	    i=$(($i + 1))
	    dd if=/dev/zero of=$DIR/$tdir/OST${MINI}/$tfile-$i bs=2M count=1 2>/dev/null
	    FILL=$(($FILL - 2048))
	    echo -n .
	done
	FILL=$(($MINV / 4))
	sync
	sleep_maxage

	free_min_max
	DIFF=$(($MAXV - $MINV))
	DIFF2=$(($DIFF * 100 / $MINV))
	echo -n "diff=${DIFF}=${DIFF2}% must be > 20% for QOS mode..."
	if [ $DIFF2 -gt 20 ]; then
	    echo "ok"
	else
	    echo "failed - QOS mode won't be used"
	    error_ignore "QOS imbalance criteria not met"
	    return
	fi

	MINI1=$MINI; MINV1=$MINV
	MAXI1=$MAXI; MAXV1=$MAXV

	# now fill using QOS
	echo writing a bunch of files to QOS-assigned OSTs
	$SETSTRIPE -c 1 $DIR/$tdir
	i=0
	while [ $FILL -gt 0 ]; do
	    i=$(($i + 1))
	    dd if=/dev/zero of=$DIR/$tdir/$tfile-$i bs=1024 count=200 2>/dev/null
	    FILL=$(($FILL - 200))
	    echo -n .
	done
	echo "wrote $i 200k files"
	sync
	sleep_maxage

	echo "Note: free space may not be updated, so measurements might be off"
	free_min_max
	DIFF2=$(($MAXV - $MINV))
	echo "free space delta: orig $DIFF final $DIFF2"
	[ $DIFF2 -gt $DIFF ] && echo "delta got worse!"
	DIFF=$(($MINV1 - ${AVAIL[$MINI1]}))
	echo "Wrote $DIFF to smaller OST $MINI1"
	DIFF2=$(($MAXV1 - ${AVAIL[$MAXI1]}))
	echo "Wrote $DIFF2 to larger OST $MAXI1"
	[ $DIFF -gt 0 ] && echo "Wrote $(($DIFF2 * 100 / $DIFF - 100))% more data to larger OST $MAXI1"

	# Figure out which files were written where
	UUID=$(lctl get_param -n lov.${FSNAME}-clilov-*.target_obd |
               awk '/'$MINI1': / {print $2; exit}')
	echo $UUID
        MINC=$($GETSTRIPE --obd $UUID $DIR/$tdir | wc -l)
	echo "$MINC files created on smaller OST $MINI1"
	UUID=$(lctl get_param -n lov.${FSNAME}-clilov-*.target_obd |
               awk '/'$MAXI1': / {print $2; exit}')
	echo $UUID
        MAXC=$($GETSTRIPE --obd $UUID $DIR/$tdir | wc -l)
	echo "$MAXC files created on larger OST $MAXI1"
	[ $MINC -gt 0 ] && echo "Wrote $(($MAXC * 100 / $MINC - 100))% more files to larger OST $MAXI1"
	[ $MAXC -gt $MINC ] || error_ignore "stripe QOS didn't balance free space"

	rm -rf $DIR/$tdir
}
run_test 116a "stripe QOS: free space balance ==================="

test_116b() { # LU-2093
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
#define OBD_FAIL_MDS_OSC_CREATE_FAIL     0x147
	local old_rr
	old_rr=$(do_facet $SINGLEMDS lctl get_param -n lov.*mdtlov*.qos_threshold_rr)
	do_facet $SINGLEMDS lctl set_param lov.*mdtlov*.qos_threshold_rr 0
	mkdir -p $DIR/$tdir
	do_facet $SINGLEMDS lctl set_param fail_loc=0x147
	createmany -o $DIR/$tdir/f- 20 || error "can't create"
	do_facet $SINGLEMDS lctl set_param fail_loc=0
	rm -rf $DIR/$tdir
	do_facet $SINGLEMDS lctl set_param lov.*mdtlov*.qos_threshold_rr $old_rr
}
run_test 116b "QoS shouldn't LBUG if not enough OSTs found on the 2nd pass"

test_117() # bug 10891
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        dd if=/dev/zero of=$DIR/$tfile bs=1M count=1
        #define OBD_FAIL_OST_SETATTR_CREDITS 0x21e
        lctl set_param fail_loc=0x21e
        > $DIR/$tfile || error "truncate failed"
        lctl set_param fail_loc=0
        echo "Truncate succeeded."
	rm -f $DIR/$tfile
}
run_test 117 "verify fsfilt_extend =========="

NO_SLOW_RESENDCOUNT=4
export OLD_RESENDCOUNT=""
set_resend_count () {
	local PROC_RESENDCOUNT="osc.${FSNAME}-OST*-osc-*.resend_count"
	OLD_RESENDCOUNT=$(lctl get_param -n $PROC_RESENDCOUNT | head -1)
	lctl set_param -n $PROC_RESENDCOUNT $1
	echo resend_count is set to $(lctl get_param -n $PROC_RESENDCOUNT)
}

# for reduce test_118* time (b=14842)
[ "$SLOW" = "no" ] && set_resend_count $NO_SLOW_RESENDCOUNT

# Reset async IO behavior after error case
reset_async() {
	FILE=$DIR/reset_async

	# Ensure all OSCs are cleared
	$SETSTRIPE -c -1 $FILE
	dd if=/dev/zero of=$FILE bs=64k count=$OSTCOUNT
	sync
	rm $FILE
}

test_118a() #bug 11710
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	reset_async

	$MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
	DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache | grep -c writeback)

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
		return 1;
        fi
	rm -f $DIR/$tfile
}
run_test 118a "verify O_SYNC works =========="

test_118b()
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	reset_async

	#define OBD_FAIL_OST_ENOENT 0x217
	set_nodes_failloc "$(osts_nodes)" 0x217
	$MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
	RC=$?
	set_nodes_failloc "$(osts_nodes)" 0
        DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache |
                    grep -c writeback)

	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
		return 1;
	fi

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
		return 1;
	fi

	echo "Dirty pages not leaked on ENOENT"

	# Due to the above error the OSC will issue all RPCs syncronously
	# until a subsequent RPC completes successfully without error.
	$MULTIOP $DIR/$tfile Ow4096yc
	rm -f $DIR/$tfile

	return 0
}
run_test 118b "Reclaim dirty pages on fatal error =========="

test_118c()
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return

	# for 118c, restore the original resend count, LU-1940
	[ "$SLOW" = "no" ] && [ -n "$OLD_RESENDCOUNT" ] &&
				set_resend_count $OLD_RESENDCOUNT
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	reset_async

	#define OBD_FAIL_OST_EROFS               0x216
	set_nodes_failloc "$(osts_nodes)" 0x216

	# multiop should block due to fsync until pages are written
	$MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c &
	MULTIPID=$!
	sleep 1

	if [[ `ps h -o comm -p $MULTIPID` != "multiop" ]]; then
		error "Multiop failed to block on fsync, pid=$MULTIPID"
	fi

        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache |
                    grep -c writeback)
	if [[ $WRITEBACK -eq 0 ]]; then
		error "No page in writeback, writeback=$WRITEBACK"
	fi

	set_nodes_failloc "$(osts_nodes)" 0
        wait $MULTIPID
	RC=$?
	if [[ $RC -ne 0 ]]; then
		error "Multiop fsync failed, rc=$RC"
	fi

        DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache |
                    grep -c writeback)
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "Dirty pages flushed via fsync on EROFS"
	return 0
}
run_test 118c "Fsync blocks on EROFS until dirty pages are flushed =========="

# continue to use small resend count to reduce test_118* time (b=14842)
[ "$SLOW" = "no" ] && set_resend_count $NO_SLOW_RESENDCOUNT

test_118d()
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	reset_async

	#define OBD_FAIL_OST_BRW_PAUSE_BULK
	set_nodes_failloc "$(osts_nodes)" 0x214
	# multiop should block due to fsync until pages are written
	$MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c &
	MULTIPID=$!
	sleep 1

	if [[ `ps h -o comm -p $MULTIPID` != "multiop" ]]; then
		error "Multiop failed to block on fsync, pid=$MULTIPID"
	fi

        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache |
                    grep -c writeback)
	if [[ $WRITEBACK -eq 0 ]]; then
		error "No page in writeback, writeback=$WRITEBACK"
	fi

        wait $MULTIPID || error "Multiop fsync failed, rc=$?"
	set_nodes_failloc "$(osts_nodes)" 0

        DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache |
                    grep -c writeback)
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "Dirty pages gaurenteed flushed via fsync"
	return 0
}
run_test 118d "Fsync validation inject a delay of the bulk =========="

test_118f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        reset_async

        #define OBD_FAIL_OSC_BRW_PREP_REQ2        0x40a
        lctl set_param fail_loc=0x8000040a

	# Should simulate EINVAL error which is fatal
        $MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?
	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
	fi

        lctl set_param fail_loc=0x0

        LOCKED=$(lctl get_param -n llite.*.dump_page_cache | grep -c locked)
        DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache |
                    grep -c writeback)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

        reset_async
	return 0
}
run_test 118f "Simulate unrecoverable OSC side error =========="

test_118g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	reset_async

	#define OBD_FAIL_OSC_BRW_PREP_REQ        0x406
	lctl set_param fail_loc=0x406

	# simulate local -ENOMEM
	$MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
	RC=$?

	lctl set_param fail_loc=0
	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
	fi

	LOCKED=$(lctl get_param -n llite.*.dump_page_cache | grep -c locked)
	DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
	WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache |
			grep -c writeback)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

	reset_async
	return 0
}
run_test 118g "Don't stay in wait if we got local -ENOMEM  =========="

test_118h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

        reset_async

	#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
        set_nodes_failloc "$(osts_nodes)" 0x20e
	# Should simulate ENOMEM error which is recoverable and should be handled by timeout
        $MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?

        set_nodes_failloc "$(osts_nodes)" 0
	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
	fi

        LOCKED=$(lctl get_param -n llite.*.dump_page_cache | grep -c locked)
        DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache |
                    grep -c writeback)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

	return 0
}
run_test 118h "Verify timeout in handling recoverables errors  =========="

[ "$SLOW" = "no" ] && [ -n "$OLD_RESENDCOUNT" ] && set_resend_count $OLD_RESENDCOUNT

test_118i() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

        reset_async

	#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
        set_nodes_failloc "$(osts_nodes)" 0x20e

	# Should simulate ENOMEM error which is recoverable and should be handled by timeout
        $MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c &
	PID=$!
	sleep 5
	set_nodes_failloc "$(osts_nodes)" 0

	wait $PID
        RC=$?
	if [[ $RC -ne 0 ]]; then
		error "got error, but should be not, rc=$RC"
	fi

        LOCKED=$(lctl get_param -n llite.*.dump_page_cache | grep -c locked)
        DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache | grep -c writeback)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi

	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

	return 0
}
run_test 118i "Fix error before timeout in recoverable error  =========="

[ "$SLOW" = "no" ] && set_resend_count 4

test_118j() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

        reset_async

	#define OBD_FAIL_OST_BRW_WRITE_BULK2     0x220
        set_nodes_failloc "$(osts_nodes)" 0x220

	# return -EIO from OST
        $MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:O_SYNC:w4096c
        RC=$?
        set_nodes_failloc "$(osts_nodes)" 0x0
	if [[ $RC -eq 0 ]]; then
		error "Must return error due to dropped pages, rc=$RC"
	fi

        LOCKED=$(lctl get_param -n llite.*.dump_page_cache | grep -c locked)
        DIRTY=$(lctl get_param -n llite.*.dump_page_cache | grep -c dirty)
        WRITEBACK=$(lctl get_param -n llite.*.dump_page_cache | grep -c writeback)
	if [[ $LOCKED -ne 0 ]]; then
		error "Locked pages remain in cache, locked=$LOCKED"
	fi

	# in recoverable error on OST we want resend and stay until it finished
	if [[ $DIRTY -ne 0 || $WRITEBACK -ne 0 ]]; then
		error "Dirty pages not flushed to disk, dirty=$DIRTY, writeback=$WRITEBACK"
	fi

	rm -f $DIR/$tfile
	echo "No pages locked after fsync"

 	return 0
}
run_test 118j "Simulate unrecoverable OST side error =========="

test_118k()
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OSTs with nodsh" && return

	#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
	set_nodes_failloc "$(osts_nodes)" 0x20e
	test_mkdir -p $DIR/$tdir

	for ((i=0;i<10;i++)); do
		(dd if=/dev/zero of=$DIR/$tdir/$tfile-$i bs=1M count=10 || \
			error "dd to $DIR/$tdir/$tfile-$i failed" )&
		SLEEPPID=$!
		sleep 0.500s
		kill $SLEEPPID
		wait $SLEEPPID
	done

	set_nodes_failloc "$(osts_nodes)" 0
	rm -rf $DIR/$tdir
}
run_test 118k "bio alloc -ENOMEM and IO TERM handling ========="

test_118l()
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	# LU-646
	test_mkdir -p $DIR/$tdir
	$MULTIOP $DIR/$tdir Dy || error "fsync dir failed"
	rm -rf $DIR/$tdir
}
run_test 118l "fsync dir ========="

test_118m() # LU-3066
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	$MULTIOP $DIR/$tdir DY || error "fdatasync dir failed"
	rm -rf $DIR/$tdir
}
run_test 118m "fdatasync dir ========="

[ "$SLOW" = "no" ] && [ -n "$OLD_RESENDCOUNT" ] && set_resend_count $OLD_RESENDCOUNT

test_119a() # bug 11737
{
        BSIZE=$((512 * 1024))
        directio write $DIR/$tfile 0 1 $BSIZE
        # We ask to read two blocks, which is more than a file size.
        # directio will indicate an error when requested and actual
        # sizes aren't equeal (a normal situation in this case) and
        # print actual read amount.
        NOB=`directio read $DIR/$tfile 0 2 $BSIZE | awk '/error/ {print $6}'`
        if [ "$NOB" != "$BSIZE" ]; then
                error "read $NOB bytes instead of $BSIZE"
        fi
        rm -f $DIR/$tfile
}
run_test 119a "Short directIO read must return actual read amount"

test_119b() # bug 11737
{
        [ "$OSTCOUNT" -lt "2" ] && skip_env "skipping 2-stripe test" && return

        $SETSTRIPE -c 2 $DIR/$tfile || error "setstripe failed"
        dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 seek=1 || error "dd failed"
        sync
        $MULTIOP $DIR/$tfile oO_RDONLY:O_DIRECT:r$((2048 * 1024)) || \
                error "direct read failed"
        rm -f $DIR/$tfile
}
run_test 119b "Sparse directIO read must return actual read amount"

test_119c() # bug 13099
{
        BSIZE=1048576
        directio write $DIR/$tfile 3 1 $BSIZE || error "direct write failed"
        directio readhole $DIR/$tfile 0 2 $BSIZE || error "reading hole failed"
        rm -f $DIR/$tfile
}
run_test 119c "Testing for direct read hitting hole"

test_119d() # bug 15950
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        MAX_RPCS_IN_FLIGHT=`$LCTL get_param -n osc.*OST0000-osc-[^mM]*.max_rpcs_in_flight`
        $LCTL set_param -n osc.*OST0000-osc-[^mM]*.max_rpcs_in_flight 1
        BSIZE=1048576
        $SETSTRIPE $DIR/$tfile -i 0 -c 1 || error "setstripe failed"
        $DIRECTIO write $DIR/$tfile 0 1 $BSIZE || error "first directio failed"
        #define OBD_FAIL_OSC_DIO_PAUSE           0x40d
        lctl set_param fail_loc=0x40d
        $DIRECTIO write $DIR/$tfile 1 4 $BSIZE &
        pid_dio=$!
        sleep 1
        cat $DIR/$tfile > /dev/null &
        lctl set_param fail_loc=0
        pid_reads=$!
        wait $pid_dio
        log "the DIO writes have completed, now wait for the reads (should not block very long)"
        sleep 2
        [ -n "`ps h -p $pid_reads -o comm`" ] && \
        error "the read rpcs have not completed in 2s"
        rm -f $DIR/$tfile
        $LCTL set_param -n osc.*OST0000-osc-[^mM]*.max_rpcs_in_flight $MAX_RPCS_IN_FLIGHT
}
run_test 119d "The DIO path should try to send a new rpc once one is completed"

test_120a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir
        [ -z "`lctl get_param -n mdc.*.connect_flags | grep early_lock_cancel`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable mdc
        lru_resize_disable osc
        cancel_lru_locks mdc
        stat $DIR/$tdir > /dev/null
        can1=`lctl get_param -n ldlm.services.ldlm_canceld.stats | awk '/ldlm_cancel/ {print $2}'`
        blk1=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
        test_mkdir $DIR/$tdir/d1
        can2=`lctl get_param -n ldlm.services.ldlm_canceld.stats | awk '/ldlm_cancel/ {print $2}'`
        blk2=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable mdc
        lru_resize_enable osc
}
run_test 120a "Early Lock Cancel: mkdir test"

test_120b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir
        [ -z "`lctl get_param -n mdc.*.connect_flags | grep early_lock_cancel`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable mdc
        lru_resize_disable osc
        cancel_lru_locks mdc
        stat $DIR/$tdir > /dev/null
        can1=`lctl get_param -n ldlm.services.ldlm_canceld.stats | awk '/ldlm_cancel/ {print $2}'`
        blk1=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
        touch $DIR/$tdir/f1
        can2=`lctl get_param -n ldlm.services.ldlm_canceld.stats | awk '/ldlm_cancel/ {print $2}'`
        blk2=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable mdc
        lru_resize_enable osc
}
run_test 120b "Early Lock Cancel: create test"

test_120c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir
        [ -z "`lctl get_param -n mdc.*.connect_flags | grep early_lock_cancel`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable mdc
        lru_resize_disable osc
        test_mkdir -p $DIR/$tdir/d1 $DIR/$tdir/d2
        touch $DIR/$tdir/d1/f1
        cancel_lru_locks mdc
        stat $DIR/$tdir/d1 $DIR/$tdir/d2 $DIR/$tdir/d1/f1 > /dev/null
        can1=`lctl get_param -n ldlm.services.ldlm_canceld.stats | awk '/ldlm_cancel/ {print $2}'`
        blk1=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
        ln $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2
        can2=`lctl get_param -n ldlm.services.ldlm_canceld.stats | awk '/ldlm_cancel/ {print $2}'`
        blk2=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable mdc
        lru_resize_enable osc
}
run_test 120c "Early Lock Cancel: link test"

test_120d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir
        [ -z "`lctl get_param -n mdc.*.connect_flags | grep early_lock_cancel`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable mdc
        lru_resize_disable osc
        touch $DIR/$tdir
        cancel_lru_locks mdc
        stat $DIR/$tdir > /dev/null
        can1=`lctl get_param -n ldlm.services.ldlm_canceld.stats | awk '/ldlm_cancel/ {print $2}'`
        blk1=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
        chmod a+x $DIR/$tdir
        can2=`lctl get_param -n ldlm.services.ldlm_canceld.stats | awk '/ldlm_cancel/ {print $2}'`
        blk2=`lctl get_param -n ldlm.services.ldlm_cbd.stats | awk '/ldlm_bl_callback/ {print $2}'`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable mdc
        lru_resize_enable osc
}
run_test 120d "Early Lock Cancel: setattr test"

test_120e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir
        [ -z "`lctl get_param -n mdc.*.connect_flags | grep early_lock_cancel`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable mdc
        lru_resize_disable osc
        dd if=/dev/zero of=$DIR/$tdir/f1 count=1
        cancel_lru_locks mdc
        cancel_lru_locks osc
        dd if=$DIR/$tdir/f1 of=/dev/null
        stat $DIR/$tdir $DIR/$tdir/f1 > /dev/null
        can1=`lctl get_param -n ldlm.services.ldlm_canceld.stats |
              awk '/ldlm_cancel/ {print $2}'`
        blk1=`lctl get_param -n ldlm.services.ldlm_cbd.stats |
              awk '/ldlm_bl_callback/ {print $2}'`
        unlink $DIR/$tdir/f1
        can2=`lctl get_param -n ldlm.services.ldlm_canceld.stats |
              awk '/ldlm_cancel/ {print $2}'`
        blk2=`lctl get_param -n ldlm.services.ldlm_cbd.stats |
              awk '/ldlm_bl_callback/ {print $2}'`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable mdc
        lru_resize_enable osc
}
run_test 120e "Early Lock Cancel: unlink test"

test_120f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        [ -z "`lctl get_param -n mdc.*.connect_flags | grep early_lock_cancel`" ] && \
               skip "no early lock cancel on server" && return 0
        test_mkdir -p $DIR/$tdir
        lru_resize_disable mdc
        lru_resize_disable osc
        test_mkdir -p $DIR/$tdir/d1 $DIR/$tdir/d2
        dd if=/dev/zero of=$DIR/$tdir/d1/f1 count=1
        dd if=/dev/zero of=$DIR/$tdir/d2/f2 count=1
        cancel_lru_locks mdc
        cancel_lru_locks osc
        dd if=$DIR/$tdir/d1/f1 of=/dev/null
        dd if=$DIR/$tdir/d2/f2 of=/dev/null
        stat $DIR/$tdir/d1 $DIR/$tdir/d2 $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2 > /dev/null
        can1=`lctl get_param -n ldlm.services.ldlm_canceld.stats |
              awk '/ldlm_cancel/ {print $2}'`
        blk1=`lctl get_param -n ldlm.services.ldlm_cbd.stats |
              awk '/ldlm_bl_callback/ {print $2}'`
        mv $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2
        can2=`lctl get_param -n ldlm.services.ldlm_canceld.stats |
              awk '/ldlm_cancel/ {print $2}'`
        blk2=`lctl get_param -n ldlm.services.ldlm_cbd.stats |
              awk '/ldlm_bl_callback/ {print $2}'`
        [ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
        [ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
        lru_resize_enable mdc
        lru_resize_enable osc
}
run_test 120f "Early Lock Cancel: rename test"

test_120g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        [ -z "`lctl get_param -n mdc.*.connect_flags | grep early_lock_cancel`" ] && \
               skip "no early lock cancel on server" && return 0
        lru_resize_disable mdc
        lru_resize_disable osc
        count=10000
        echo create $count files
        test_mkdir -p $DIR/$tdir
        cancel_lru_locks mdc
        cancel_lru_locks osc
        t0=`date +%s`

        can0=`lctl get_param -n ldlm.services.ldlm_canceld.stats |
              awk '/ldlm_cancel/ {print $2}'`
        blk0=`lctl get_param -n ldlm.services.ldlm_cbd.stats |
              awk '/ldlm_bl_callback/ {print $2}'`
        createmany -o $DIR/$tdir/f $count
        sync
        can1=`lctl get_param -n ldlm.services.ldlm_canceld.stats |
              awk '/ldlm_cancel/ {print $2}'`
        blk1=`lctl get_param -n ldlm.services.ldlm_cbd.stats |
              awk '/ldlm_bl_callback/ {print $2}'`
        t1=`date +%s`
        echo total: $((can1-can0)) cancels, $((blk1-blk0)) blockings
        echo rm $count files
        rm -r $DIR/$tdir
        sync
        can2=`lctl get_param -n ldlm.services.ldlm_canceld.stats |
              awk '/ldlm_cancel/ {print $2}'`
        blk2=`lctl get_param -n ldlm.services.ldlm_cbd.stats |
              awk '/ldlm_bl_callback/ {print $2}'`
        t2=`date +%s`
        echo total: $count removes in $((t2-t1))
        echo total: $((can2-can1)) cancels, $((blk2-blk1)) blockings
        sleep 2
        # wait for commitment of removal
        lru_resize_enable mdc
        lru_resize_enable osc
}
run_test 120g "Early Lock Cancel: performance test"

test_121() { #bug #10589
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	rm -rf $DIR/$tfile
	writes=$(LANG=C dd if=/dev/zero of=$DIR/$tfile count=1 2>&1 | awk -F '+' '/out$/ {print $1}')
#define OBD_FAIL_LDLM_CANCEL_RACE        0x310
	lctl set_param fail_loc=0x310
	cancel_lru_locks osc > /dev/null
	reads=$(LANG=C dd if=$DIR/$tfile of=/dev/null 2>&1 | awk -F '+' '/in$/ {print $1}')
	lctl set_param fail_loc=0
	[ "$reads" -eq "$writes" ] || error "read" $reads "blocks, must be" $writes
}
run_test 121 "read cancel race ========="

test_123a() { # was test 123, statahead(bug 11401)
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        SLOWOK=0
        if [ -z "$(grep "processor.*: 1" /proc/cpuinfo)" ]; then
            log "testing on UP system. Performance may be not as good as expected."
			SLOWOK=1
        fi

        rm -rf $DIR/$tdir
        test_mkdir -p $DIR/$tdir
        NUMFREE=`df -i -P $DIR | tail -n 1 | awk '{ print $4 }'`
        [ $NUMFREE -gt 100000 ] && NUMFREE=100000 || NUMFREE=$((NUMFREE-1000))
        MULT=10
        for ((i=100, j=0; i<=$NUMFREE; j=$i, i=$((i * MULT)) )); do
                createmany -o $DIR/$tdir/$tfile $j $((i - j))

                max=`lctl get_param -n llite.*.statahead_max | head -n 1`
                lctl set_param -n llite.*.statahead_max 0
                lctl get_param llite.*.statahead_max
                cancel_lru_locks mdc
                cancel_lru_locks osc
                stime=`date +%s`
                time ls -l $DIR/$tdir | wc -l
                etime=`date +%s`
                delta=$((etime - stime))
                log "ls $i files without statahead: $delta sec"
                lctl set_param llite.*.statahead_max=$max

                swrong=`lctl get_param -n llite.*.statahead_stats | grep "statahead wrong:" | awk '{print $3}'`
                lctl get_param -n llite.*.statahead_max | grep '[0-9]'
                cancel_lru_locks mdc
                cancel_lru_locks osc
                stime=`date +%s`
                time ls -l $DIR/$tdir | wc -l
                etime=`date +%s`
                delta_sa=$((etime - stime))
                log "ls $i files with statahead: $delta_sa sec"
                lctl get_param -n llite.*.statahead_stats
                ewrong=`lctl get_param -n llite.*.statahead_stats | grep "statahead wrong:" | awk '{print $3}'`

                [ $swrong -lt $ewrong ] && log "statahead was stopped, maybe too many locks held!"
                [ $delta -eq 0 -o $delta_sa -eq 0 ] && continue

                if [ $((delta_sa * 100)) -gt $((delta * 105)) -a $delta_sa -gt $((delta + 2)) ]; then
                    max=`lctl get_param -n llite.*.statahead_max | head -n 1`
                    lctl set_param -n llite.*.statahead_max 0
                    lctl get_param llite.*.statahead_max
                    cancel_lru_locks mdc
                    cancel_lru_locks osc
                    stime=`date +%s`
                    time ls -l $DIR/$tdir | wc -l
                    etime=`date +%s`
                    delta=$((etime - stime))
                    log "ls $i files again without statahead: $delta sec"
                    lctl set_param llite.*.statahead_max=$max
                    if [ $((delta_sa * 100)) -gt $((delta * 105)) -a $delta_sa -gt $((delta + 2)) ]; then
                        if [  $SLOWOK -eq 0 ]; then
                                error "ls $i files is slower with statahead!"
                        else
                                log "ls $i files is slower with statahead!"
                        fi
                        break
                    fi
                fi

                [ $delta -gt 20 ] && break
                [ $delta -gt 8 ] && MULT=$((50 / delta))
                [ "$SLOW" = "no" -a $delta -gt 5 ] && break
        done
        log "ls done"

        stime=`date +%s`
        rm -r $DIR/$tdir
        sync
        etime=`date +%s`
        delta=$((etime - stime))
        log "rm -r $DIR/$tdir/: $delta seconds"
        log "rm done"
        lctl get_param -n llite.*.statahead_stats
}
run_test 123a "verify statahead work"

test_123b () { # statahead(bug 15027)
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir
	createmany -o $DIR/$tdir/$tfile-%d 1000

        cancel_lru_locks mdc
        cancel_lru_locks osc

#define OBD_FAIL_MDC_GETATTR_ENQUEUE     0x803
        lctl set_param fail_loc=0x80000803
        ls -lR $DIR/$tdir > /dev/null
        log "ls done"
        lctl set_param fail_loc=0x0
        lctl get_param -n llite.*.statahead_stats
        rm -r $DIR/$tdir
        sync

}
run_test 123b "not panic with network error in statahead enqueue (bug 15027)"

test_124a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -z "`lctl get_param -n mdc.*.connect_flags | grep lru_resize`" ] && \
               skip "no lru resize on server" && return 0
        local NR=2000
        test_mkdir -p $DIR/$tdir || error "failed to create $DIR/$tdir"

        log "create $NR files at $DIR/$tdir"
        createmany -o $DIR/$tdir/f $NR ||
                error "failed to create $NR files in $DIR/$tdir"

        cancel_lru_locks mdc
        ls -l $DIR/$tdir > /dev/null

        local NSDIR=""
        local LRU_SIZE=0
        for VALUE in `lctl get_param ldlm.namespaces.*mdc-*.lru_size`; do
                local PARAM=`echo ${VALUE[0]} | cut -d "=" -f1`
                LRU_SIZE=$(lctl get_param -n $PARAM)
                if [ $LRU_SIZE -gt $(default_lru_size) ]; then
                        NSDIR=$(echo $PARAM | cut -d "." -f1-3)
						log "NSDIR=$NSDIR"
                        log "NS=$(basename $NSDIR)"
                        break
                fi
        done

        if [ -z "$NSDIR" -o $LRU_SIZE -lt $(default_lru_size) ]; then
                skip "Not enough cached locks created!"
                return 0
        fi
        log "LRU=$LRU_SIZE"

        local SLEEP=30

        # We know that lru resize allows one client to hold $LIMIT locks
        # for 10h. After that locks begin to be killed by client.
        local MAX_HRS=10
        local LIMIT=`lctl get_param -n $NSDIR.pool.limit`
		log "LIMIT=$LIMIT"

        # Make LVF so higher that sleeping for $SLEEP is enough to _start_
        # killing locks. Some time was spent for creating locks. This means
        # that up to the moment of sleep finish we must have killed some of
        # them (10-100 locks). This depends on how fast ther were created.
        # Many of them were touched in almost the same moment and thus will
        # be killed in groups.
        local LVF=$(($MAX_HRS * 60 * 60 / $SLEEP * $LIMIT / $LRU_SIZE))

        # Use $LRU_SIZE_B here to take into account real number of locks
        # created in the case of CMD, LRU_SIZE_B != $NR in most of cases
        local LRU_SIZE_B=$LRU_SIZE
        log "LVF=$LVF"
        local OLD_LVF=`lctl get_param -n $NSDIR.pool.lock_volume_factor`
		log "OLD_LVF=$OLD_LVF"
        lctl set_param -n $NSDIR.pool.lock_volume_factor $LVF

        # Let's make sure that we really have some margin. Client checks
        # cached locks every 10 sec.
        SLEEP=$((SLEEP+20))
        log "Sleep ${SLEEP} sec"
        local SEC=0
        while ((SEC<$SLEEP)); do
                echo -n "..."
                sleep 5
                SEC=$((SEC+5))
                LRU_SIZE=`lctl get_param -n $NSDIR/lru_size`
                echo -n "$LRU_SIZE"
        done
        echo ""
        lctl set_param -n $NSDIR.pool.lock_volume_factor $OLD_LVF
        local LRU_SIZE_A=`lctl get_param -n $NSDIR.lru_size`

        [ $LRU_SIZE_B -gt $LRU_SIZE_A ] || {
                error "No locks dropped in ${SLEEP}s. LRU size: $LRU_SIZE_A"
                unlinkmany $DIR/$tdir/f $NR
                return
        }

        log "Dropped "$((LRU_SIZE_B-LRU_SIZE_A))" locks in ${SLEEP}s"
        log "unlink $NR files at $DIR/$tdir"
        unlinkmany $DIR/$tdir/f $NR
}
run_test 124a "lru resize ======================================="

get_max_pool_limit()
{
        local limit=`lctl get_param -n ldlm.namespaces.*-MDT0000-mdc-*.pool.limit`
        local max=0
        for l in $limit; do
                if test $l -gt $max; then
                        max=$l
                fi
        done
        echo $max
}

test_124b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -z "`lctl get_param -n mdc.*.connect_flags | grep lru_resize`" ] && \
               skip "no lru resize on server" && return 0

        LIMIT=`get_max_pool_limit`

        NR=$(($(default_lru_size)*20))
        if [ $NR -gt $LIMIT ]; then
                log "Limit lock number by $LIMIT locks"
                NR=$LIMIT
        fi
        lru_resize_disable mdc
        test_mkdir -p $DIR/$tdir/disable_lru_resize ||
		error "failed to create $DIR/$tdir/disable_lru_resize"

        createmany -o $DIR/$tdir/disable_lru_resize/f $NR
        log "doing ls -la $DIR/$tdir/disable_lru_resize 3 times"
        cancel_lru_locks mdc
        stime=`date +%s`
        PID=""
        ls -la $DIR/$tdir/disable_lru_resize > /dev/null &
        PID="$PID $!"
        sleep 2
        ls -la $DIR/$tdir/disable_lru_resize > /dev/null &
        PID="$PID $!"
        sleep 2
        ls -la $DIR/$tdir/disable_lru_resize > /dev/null &
        PID="$PID $!"
        wait $PID
        etime=`date +%s`
        nolruresize_delta=$((etime-stime))
        log "ls -la time: $nolruresize_delta seconds"
        log "lru_size = $(lctl get_param -n ldlm.namespaces.*mdc*.lru_size)"
        unlinkmany $DIR/$tdir/disable_lru_resize/f $NR

        lru_resize_enable mdc
        test_mkdir -p $DIR/$tdir/enable_lru_resize ||
		error "failed to create $DIR/$tdir/enable_lru_resize"

        createmany -o $DIR/$tdir/enable_lru_resize/f $NR
        log "doing ls -la $DIR/$tdir/enable_lru_resize 3 times"
        cancel_lru_locks mdc
        stime=`date +%s`
        PID=""
        ls -la $DIR/$tdir/enable_lru_resize > /dev/null &
        PID="$PID $!"
        sleep 2
        ls -la $DIR/$tdir/enable_lru_resize > /dev/null &
        PID="$PID $!"
        sleep 2
        ls -la $DIR/$tdir/enable_lru_resize > /dev/null &
        PID="$PID $!"
        wait $PID
        etime=`date +%s`
        lruresize_delta=$((etime-stime))
        log "ls -la time: $lruresize_delta seconds"
        log "lru_size = $(lctl get_param -n ldlm.namespaces.*mdc*.lru_size)"

        if [ $lruresize_delta -gt $nolruresize_delta ]; then
                log "ls -la is $(((lruresize_delta - $nolruresize_delta) * 100 / $nolruresize_delta))% slower with lru resize enabled"
        elif [ $nolruresize_delta -gt $lruresize_delta ]; then
                log "ls -la is $(((nolruresize_delta - $lruresize_delta) * 100 / $nolruresize_delta))% faster with lru resize enabled"
        else
                log "lru resize performs the same with no lru resize"
        fi
        unlinkmany $DIR/$tdir/enable_lru_resize/f $NR
}
run_test 124b "lru resize (performance test) ======================="

test_125() { # 13358
	[ -z "$(lctl get_param -n llite.*.client_type | grep local)" ] && skip "must run as local client" && return
	[ -z "$(lctl get_param -n mdc.*-mdc-*.connect_flags | grep acl)" ] && skip "must have acl enabled" && return
	test_mkdir -p $DIR/d125 || error "mkdir failed"
	$SETSTRIPE -S 65536 -c -1 $DIR/d125 || error "setstripe failed"
	setfacl -R -m u:bin:rwx $DIR/d125 || error "setfacl $DIR/d125 failed"
	ls -ld $DIR/d125 || error "cannot access $DIR/d125"
}
run_test 125 "don't return EPROTO when a dir has a non-default striping and ACLs"

test_126() { # bug 12829/13455
	[ -z "$(lctl get_param -n llite.*.client_type | grep local)" ] && skip "must run as local client" && return
	[ "$UID" != 0 ] && skip_env "skipping $TESTNAME (must run as root)" && return
	$GSS && skip "must run as gss disabled" && return

	$RUNAS -u 0 -g 1 touch $DIR/$tfile || error "touch failed"
	gid=`ls -n $DIR/$tfile | awk '{print $4}'`
	rm -f $DIR/$tfile
	[ $gid -eq "1" ] || error "gid is set to" $gid "instead of 1"
}
run_test 126 "check that the fsgid provided by the client is taken into account"

test_127a() { # bug 15521
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        $SETSTRIPE -i 0 -c 1 $DIR/$tfile || error "setstripe failed"
        $LCTL set_param osc.*.stats=0
        FSIZE=$((2048 * 1024))
        dd if=/dev/zero of=$DIR/$tfile bs=$FSIZE count=1
        cancel_lru_locks osc
        dd if=$DIR/$tfile of=/dev/null bs=$FSIZE

        $LCTL get_param osc.*0000-osc-*.stats | grep samples > $DIR/${tfile}.tmp
        while read NAME COUNT SAMP UNIT MIN MAX SUM SUMSQ; do
                echo "got $COUNT $NAME"
                [ ! $MIN ] && error "Missing min value for $NAME proc entry"
                eval $NAME=$COUNT || error "Wrong proc format"

                case $NAME in
                        read_bytes|write_bytes)
                        [ $MIN -lt 4096 ] && error "min is too small: $MIN"
                        [ $MIN -gt $FSIZE ] && error "min is too big: $MIN"
                        [ $MAX -lt 4096 ] && error "max is too small: $MAX"
                        [ $MAX -gt $FSIZE ] && error "max is too big: $MAX"
                        [ $SUM -ne $FSIZE ] && error "sum is wrong: $SUM"
                        [ $SUMSQ -lt $(((FSIZE /4096) * (4096 * 4096))) ] &&
                                error "sumsquare is too small: $SUMSQ"
                        [ $SUMSQ -gt $((FSIZE * FSIZE)) ] &&
                                error "sumsquare is too big: $SUMSQ"
                        ;;
                        *) ;;
                esac
        done < $DIR/${tfile}.tmp

        #check that we actually got some stats
        [ "$read_bytes" ] || error "Missing read_bytes stats"
        [ "$write_bytes" ] || error "Missing write_bytes stats"
        [ "$read_bytes" != 0 ] || error "no read done"
        [ "$write_bytes" != 0 ] || error "no write done"
}
run_test 127a "verify the client stats are sane"

test_127b() { # bug LU-333
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        $LCTL set_param llite.*.stats=0
        FSIZE=65536 # sized fixed to match PAGE_SIZE for most clients
        # perform 2 reads and writes so MAX is different from SUM.
        dd if=/dev/zero of=$DIR/$tfile bs=$FSIZE count=1
        dd if=/dev/zero of=$DIR/$tfile bs=$FSIZE count=1
        cancel_lru_locks osc
        dd if=$DIR/$tfile of=/dev/null bs=$FSIZE count=1
        dd if=$DIR/$tfile of=/dev/null bs=$FSIZE count=1

        $LCTL get_param llite.*.stats | grep samples > $TMP/${tfile}.tmp
        while read NAME COUNT SAMP UNIT MIN MAX SUM SUMSQ; do
                echo "got $COUNT $NAME"
                eval $NAME=$COUNT || error "Wrong proc format"

        case $NAME in
                read_bytes)
                        [ $COUNT -ne 2 ] && error "count is not 2: $COUNT"
                        [ $MIN -ne $FSIZE ] && error "min is not $FSIZE: $MIN"
                        [ $MAX -ne $FSIZE ] && error "max is incorrect: $MAX"
                        [ $SUM -ne $((FSIZE * 2)) ] && error "sum is wrong: $SUM"
                        ;;
                write_bytes)
                        [ $COUNT -ne 2 ] && error "count is not 2: $COUNT"
                        [ $MIN -ne $FSIZE ] && error "min is not $FSIZE: $MIN"
                        [ $MAX -ne $FSIZE ] && error "max is incorrect: $MAX"
                        [ $SUM -ne $((FSIZE * 2)) ] && error "sum is wrong: $SUM"
                        ;;
                        *) ;;
                esac
        done < $TMP/${tfile}.tmp

        #check that we actually got some stats
        [ "$read_bytes" ] || error "Missing read_bytes stats"
        [ "$write_bytes" ] || error "Missing write_bytes stats"
        [ "$read_bytes" != 0 ] || error "no read done"
        [ "$write_bytes" != 0 ] || error "no write done"
}
run_test 127b "verify the llite client stats are sane"

test_128() { # bug 15212
	touch $DIR/$tfile
	$LFS 2>&1 <<-EOF | tee $TMP/$tfile.log
		find $DIR/$tfile
		find $DIR/$tfile
	EOF

	result=$(grep error $TMP/$tfile.log)
	rm -f $DIR/$tfile
	[ -z "$result" ] || error "consecutive find's under interactive lfs failed"
}
run_test 128 "interactive lfs for 2 consecutive find's"

set_dir_limits () {
	local mntdev
	local canondev
	local node

	local LDPROC=/proc/fs/ldiskfs
	local facets=$(get_facets MDS)

	for facet in ${facets//,/ }; do
		canondev=$(ldiskfs_canon \
			   *.$(convert_facet2label $facet).mntdev $facet)
		do_facet $facet "test -e $LDPROC/$canondev/max_dir_size" ||
						LDPROC=/sys/fs/ldiskfs
		do_facet $facet "echo $1 >$LDPROC/$canondev/max_dir_size"
	done
}

test_129() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	if [ "$(facet_fstype $SINGLEMDS)" != ldiskfs ]; then
		skip "Only applicable to ldiskfs-based MDTs"
		return
	fi
	remote_mds_nodsh && skip "remote MDS with nodsh" && return

	EFBIG=27
	MAX=16384

	set_dir_limits $MAX
	test_mkdir -p $DIR/$tdir

	local I=0
	local J=0
	while [ ! $I -gt $((MAX * MDSCOUNT)) ]; do
		$MULTIOP $DIR/$tdir/$J Oc
		rc=$?
		if [ $rc -eq $EFBIG ]; then
			set_dir_limits 0
			echo "return code $rc received as expected"
			return 0
		elif [ $rc -ne 0 ]; then
			set_dir_limits 0
			error_exit "return code $rc received instead of expected $EFBIG"
		fi
		J=$((J+1))
		I=$(stat -c%s "$DIR/$tdir")
	done

	set_dir_limits 0
	error "exceeded dir size limit $MAX x $MDSCOUNT $((MAX * MDSCOUNT)) : $I bytes"
}
run_test 129 "test directory size limit ========================"

OLDIFS="$IFS"
cleanup_130() {
	trap 0
	IFS="$OLDIFS"
}

test_130a() {
	local filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip_env "filefrag does not support FIEMAP" &&
		return

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 65536 -c 1 $fm_file || error "setstripe on $fm_file"
	dd if=/dev/zero of=$fm_file bs=65536 count=1 ||
		error "dd failed for $fm_file"

	# LU-1795: test filefrag/FIEMAP once, even if unsupported
	filefrag -ves $fm_file
	RC=$?
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip "ORI-366/LU-1941: FIEMAP unimplemented on ZFS" && return
	[ $RC != 0 ] && error "filefrag $fm_file failed"

	filefrag_op=$(filefrag -ve $fm_file | grep -A 100 "ext:" |
		      grep -v "ext:" | grep -v "found")
	lun=$($GETSTRIPE -i $fm_file)

	start_blk=`echo $filefrag_op | cut -d: -f2 | cut -d. -f1`
	IFS=$'\n'
	tot_len=0
	for line in $filefrag_op
	do
		frag_lun=`echo $line | cut -d: -f5`
		ext_len=`echo $line | cut -d: -f4`
		if (( $frag_lun != $lun )); then
			cleanup_130
			error "FIEMAP on 1-stripe file($fm_file) failed"
			return
		fi
		(( tot_len += ext_len ))
	done

	if (( lun != frag_lun || start_blk != 0 || tot_len != 64 )); then
		cleanup_130
		error "FIEMAP on 1-stripe file($fm_file) failed;"
		return
	fi

	cleanup_130

	echo "FIEMAP on single striped file succeeded"
}
run_test 130a "FIEMAP (1-stripe file)"

test_130b() {
	[ "$OSTCOUNT" -lt "2" ] &&
		skip_env "skipping FIEMAP on 2-stripe file test" && return

	local filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip_env "filefrag does not support FIEMAP" &&
		return

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 65536 -c 2 $fm_file || error "setstripe on $fm_file"
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip "ORI-366/LU-1941: FIEMAP unimplemented on ZFS" && return

	dd if=/dev/zero of=$fm_file bs=1M count=2 ||
		error "dd failed on $fm_file"

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_op=$(filefrag -ve $fm_file | grep -A 100 "ext:" |
		      grep -v "ext:" | grep -v "found")

	last_lun=$(echo $filefrag_op | cut -d: -f5)

	IFS=$'\n'
	tot_len=0
	num_luns=1
	for line in $filefrag_op
	do
		frag_lun=`echo $line | cut -d: -f5`
		ext_len=`echo $line | cut -d: -f4`
		if (( $frag_lun != $last_lun )); then
			if (( tot_len != 1024 )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned len $tot_len for OST $last_lun instead of 256"
				return
			else
				(( num_luns += 1 ))
				tot_len=0
			fi
		fi
		(( tot_len += ext_len ))
		last_lun=$frag_lun
	done
	if (( num_luns != 2 || tot_len != 1024 )); then
		cleanup_130
		error "FIEMAP on $fm_file failed; returned wrong number of luns or wrong len for OST $last_lun"
		return
	fi

	cleanup_130

	echo "FIEMAP on 2-stripe file succeeded"
}
run_test 130b "FIEMAP (2-stripe file)"

test_130c() {
	[ "$OSTCOUNT" -lt "2" ] &&
		skip_env "skipping FIEMAP on 2-stripe file" && return

	filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip "filefrag does not support FIEMAP" &&
		return

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 65536 -c 2 $fm_file || error "setstripe on $fm_file"
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip "ORI-366/LU-1941: FIEMAP unimplemented on ZFS" && return

	dd if=/dev/zero of=$fm_file seek=1 bs=1M count=1 || error "dd failed on $fm_file"

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_op=`filefrag -ve $fm_file | grep -A 100 "ext:" | grep -v "ext:" | grep -v "found"`

	last_lun=`echo $filefrag_op | cut -d: -f5`

	IFS=$'\n'
	tot_len=0
	num_luns=1
	for line in $filefrag_op
	do
		frag_lun=`echo $line | cut -d: -f5`
		ext_len=`echo $line | cut -d: -f4`
		if (( $frag_lun != $last_lun )); then
			logical=`echo $line | cut -d: -f2 | cut -d. -f1`
			if (( logical != 512 )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned logical start for lun $logical instead of 512"
				return
			fi
			if (( tot_len != 512 )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned len $tot_len for OST $last_lun instead of 1024"
				return
			else
				(( num_luns += 1 ))
				tot_len=0
			fi
		fi
		(( tot_len += ext_len ))
		last_lun=$frag_lun
	done
	if (( num_luns != 2 || tot_len != 512 )); then
		cleanup_130
		error "FIEMAP on $fm_file failed; returned wrong number of luns or wrong len for OST $last_lun"
		return
	fi

	cleanup_130

	echo "FIEMAP on 2-stripe file with hole succeeded"
}
run_test 130c "FIEMAP (2-stripe file with hole)"

test_130d() {
	[ "$OSTCOUNT" -lt "3" ] && skip_env "skipping FIEMAP on N-stripe file test" && return

	filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip "filefrag does not support FIEMAP" && return

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 65536 -c $OSTCOUNT $fm_file||error "setstripe on $fm_file"
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip "ORI-366/LU-1941: FIEMAP unimplemented on ZFS" && return
	dd if=/dev/zero of=$fm_file bs=1M count=$OSTCOUNT || error "dd failed on $fm_file"

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_op=`filefrag -ve $fm_file | grep -A 100 "ext:" | grep -v "ext:" | grep -v "found"`

	last_lun=`echo $filefrag_op | cut -d: -f5`

	IFS=$'\n'
	tot_len=0
	num_luns=1
	for line in $filefrag_op
	do
		frag_lun=`echo $line | cut -d: -f5`
		ext_len=`echo $line | cut -d: -f4`
		if (( $frag_lun != $last_lun )); then
			if (( tot_len != 1024 )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned len $tot_len for OST $last_lun instead of 1024"
				return
			else
				(( num_luns += 1 ))
				tot_len=0
			fi
		fi
		(( tot_len += ext_len ))
		last_lun=$frag_lun
	done
	if (( num_luns != OSTCOUNT || tot_len != 1024 )); then
		cleanup_130
		error "FIEMAP on $fm_file failed; returned wrong number of luns or wrong len for OST $last_lun"
		return
	fi

	cleanup_130

	echo "FIEMAP on N-stripe file succeeded"
}
run_test 130d "FIEMAP (N-stripe file)"

test_130e() {
	[ "$OSTCOUNT" -lt "2" ] && skip_env "skipping continuation FIEMAP test" && return

	filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip "filefrag does not support FIEMAP" && return

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 131072 -c 2 $fm_file || error "setstripe on $fm_file"
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip "ORI-366/LU-1941: FIEMAP unimplemented on ZFS" && return

	NUM_BLKS=512
	EXPECTED_LEN=$(( (NUM_BLKS / 2) * 64 ))
	for ((i = 0; i < $NUM_BLKS; i++))
	do
		dd if=/dev/zero of=$fm_file count=1 bs=64k seek=$((2*$i)) conv=notrunc > /dev/null 2>&1
	done

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_op=`filefrag -ve $fm_file | grep -A 12000 "ext:" | grep -v "ext:" | grep -v "found"`

	last_lun=`echo $filefrag_op | cut -d: -f5`

	IFS=$'\n'
	tot_len=0
	num_luns=1
	for line in $filefrag_op
	do
		frag_lun=`echo $line | cut -d: -f5`
		ext_len=`echo $line | cut -d: -f4`
		if (( $frag_lun != $last_lun )); then
			if (( tot_len != $EXPECTED_LEN )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned len $tot_len for OST $last_lun instead of $EXPECTED_LEN"
				return
			else
				(( num_luns += 1 ))
				tot_len=0
			fi
		fi
		(( tot_len += ext_len ))
		last_lun=$frag_lun
	done
	if (( num_luns != 2 || tot_len != $EXPECTED_LEN )); then
		cleanup_130
		error "FIEMAP on $fm_file failed; returned wrong number of luns or wrong len for OST $last_lun"
		return
	fi

	cleanup_130

	echo "FIEMAP with continuation calls succeeded"
}
run_test 130e "FIEMAP (test continuation FIEMAP calls)"

# Test for writev/readv
test_131a() {
	rwv -f $DIR/$tfile -w -n 3 524288 1048576 1572864 || \
	error "writev test failed"
	rwv -f $DIR/$tfile -r -v -n 2 1572864 1048576 || \
	error "readv failed"
	rm -f $DIR/$tfile
}
run_test 131a "test iov's crossing stripe boundary for writev/readv"

test_131b() {
	rwv -f $DIR/$tfile -w -a -n 3 524288 1048576 1572864 || \
	error "append writev test failed"
	rwv -f $DIR/$tfile -w -a -n 2 1572864 1048576 || \
	error "append writev test failed"
	rm -f $DIR/$tfile
}
run_test 131b "test append writev"

test_131c() {
	rwv -f $DIR/$tfile -w -d -n 1 1048576 || return 0
	error "NOT PASS"
}
run_test 131c "test read/write on file w/o objects"

test_131d() {
	rwv -f $DIR/$tfile -w -n 1 1572864
	NOB=`rwv -f $DIR/$tfile -r -n 3 524288 524288 1048576 | awk '/error/ {print $6}'`
	if [ "$NOB" != 1572864 ]; then
		error "Short read filed: read $NOB bytes instead of 1572864"
	fi
	rm -f $DIR/$tfile
}
run_test 131d "test short read"

test_131e() {
	rwv -f $DIR/$tfile -w -s 1048576 -n 1 1048576
	rwv -f $DIR/$tfile -r -z -s 0 -n 1 524288 || \
	error "read hitting hole failed"
	rm -f $DIR/$tfile
}
run_test 131e "test read hitting hole"

get_ost_param() {
        local token=$1
        local gl_sum=0
        for node in $(osts_nodes); do
                gl=$(do_node $node "$LCTL get_param -n ost.OSS.ost.stats" | awk '/'$token'/ {print $2}' | head -n 1)
                [ x$gl = x"" ] && gl=0
                gl_sum=$((gl_sum + gl))
        done
        echo $gl_sum
}

som_mode_switch() {
        local som=$1
        local gl1=$2
        local gl2=$3

        if [ x$som = x"enabled" ]; then
                [ $((gl2 - gl1)) -gt 0 ] && error "no glimpse RPC is expected"
                MOUNTOPT=`echo $MOUNTOPT | sed 's/som_preview//g'`
                do_facet mgs "$LCTL conf_param $FSNAME.mdt.som=disabled"
        else
                [ $((gl2 - gl1)) -gt 0 ] || error "some glimpse RPC is expected"
                MOUNTOPT="$MOUNTOPT,som_preview"
                do_facet mgs "$LCTL conf_param $FSNAME.mdt.som=enabled"
        fi

        # do remount to make new mount-conf parameters actual
        echo remounting...
        sync
        stopall
        setupall
}

test_132() { #1028, SOM
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        remote_mds_nodsh && skip "remote MDS with nodsh" && return
        local num=$(get_mds_dir $DIR)
        local mymds=mds${num}
        local MOUNTOPT_SAVE=$MOUNTOPT

        dd if=/dev/zero of=$DIR/$tfile count=1 2>/dev/null
        cancel_lru_locks osc

        som1=$(do_facet $mymds "$LCTL get_param mdt.*.som" |  awk -F= ' {print $2}' | head -n 1)

        gl1=$(get_ost_param "ldlm_glimpse_enqueue")
        stat $DIR/$tfile >/dev/null
        gl2=$(get_ost_param "ldlm_glimpse_enqueue")
        echo "====> SOM is "$som1", "$((gl2 - gl1))" glimpse RPC occured"
        rm $DIR/$tfile
        som_mode_switch $som1 $gl1 $gl2

        dd if=/dev/zero of=$DIR/$tfile count=1 2>/dev/null
        cancel_lru_locks osc

        som2=$(do_facet $mymds "$LCTL get_param mdt.*.som" |  awk -F= ' {print $2}' | head -n 1)
        if [ $som1 == $som2 ]; then
            error "som is still "$som2
            if [ x$som2 = x"enabled" ]; then
                som2="disabled"
            else
                som2="enabled"
            fi
        fi

        gl1=$(get_ost_param "ldlm_glimpse_enqueue")
        stat $DIR/$tfile >/dev/null
        gl2=$(get_ost_param "ldlm_glimpse_enqueue")
        echo "====> SOM is "$som2", "$((gl2 - gl1))" glimpse RPC occured"
        som_mode_switch $som2 $gl1 $gl2
        MOUNTOPT=$MOUNTOPT_SAVE
}
run_test 132 "som avoids glimpse rpc"

check_stats() {
	local res
	local count
	case $1 in
	$SINGLEMDS) res=`do_facet $SINGLEMDS $LCTL get_param mdt.$FSNAME-MDT0000.md_stats | grep "$2"`
		 ;;
	ost) res=`do_facet ost1 $LCTL get_param obdfilter.$FSNAME-OST0000.stats | grep "$2"`
		 ;;
	*) error "Wrong argument $1" ;;
	esac
	echo $res
	count=`echo $res | awk '{print $2}'`
	[ -z "$res" ] && error "The counter for $2 on $1 was not incremented"
	# if the argument $3 is zero, it means any stat increment is ok.
	if [ $3 -gt 0 ] ; then
		[ $count -ne $3 ] && error "The $2 counter on $1 is wrong - expected $3"
	fi
}

test_133a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return

	do_facet $SINGLEMDS $LCTL list_param mdt.*.rename_stats ||
		{ skip "MDS doesn't support rename stats"; return; }
	local testdir=$DIR/${tdir}/stats_testdir
	mkdir -p $DIR/${tdir}

	# clear stats.
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	do_facet ost1 $LCTL set_param obdfilter.*.stats=clear

	# verify mdt stats first.
	mkdir ${testdir} || error "mkdir failed"
	check_stats $SINGLEMDS "mkdir" 1
	touch ${testdir}/${tfile} || "touch failed"
	check_stats $SINGLEMDS "open" 1
	check_stats $SINGLEMDS "close" 1
	mknod ${testdir}/${tfile}-pipe p || "mknod failed"
	check_stats $SINGLEMDS "mknod" 1
	rm -f ${testdir}/${tfile}-pipe || "pipe remove failed"
	check_stats $SINGLEMDS "unlink" 1
	rm -f ${testdir}/${tfile} || error "file remove failed"
	check_stats $SINGLEMDS "unlink" 2

	# remove working dir and check mdt stats again.
	rmdir ${testdir} || error "rmdir failed"
	check_stats $SINGLEMDS "rmdir" 1

	local testdir1=$DIR/${tdir}/stats_testdir1
	mkdir -p ${testdir}
	mkdir -p ${testdir1}
	touch ${testdir1}/test1
	mv ${testdir1}/test1 ${testdir} || error "file crossdir rename"
	check_stats $SINGLEMDS "crossdir_rename" 1

	mv ${testdir}/test1 ${testdir}/test0 || error "file samedir rename"
	check_stats $SINGLEMDS "samedir_rename" 1

	rm -rf $DIR/${tdir}
}
run_test 133a "Verifying MDT stats ========================================"

test_133b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	local testdir=$DIR/${tdir}/stats_testdir
	mkdir -p ${testdir} || error "mkdir failed"
	touch ${testdir}/${tfile} || "touch failed"
	cancel_lru_locks mdc

	# clear stats.
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	do_facet ost1 $LCTL set_param obdfilter.*.stats=clear

	# extra mdt stats verification.
	chmod 444 ${testdir}/${tfile} || error "chmod failed"
	check_stats $SINGLEMDS "setattr" 1
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	if [ $(lustre_version_code $SINGLEMDS) -ne $(version_code 2.2.0) ]
	then		# LU-1740
		ls -l ${testdir}/${tfile} > /dev/null|| error "ls failed"
		check_stats $SINGLEMDS "getattr" 1
	fi
	$LFS df || error "lfs failed"
	check_stats $SINGLEMDS "statfs" 1

	rm -rf $DIR/${tdir}
}
run_test 133b "Verifying extra MDT stats =================================="

test_133c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	local testdir=$DIR/${tdir}/stats_testdir
	test_mkdir -p ${testdir} || error "mkdir failed"

	# verify obdfilter stats.
	$SETSTRIPE -c 1 -i 0 ${testdir}/${tfile}
	sync
	cancel_lru_locks osc
	wait_delete_completed

	# clear stats.
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	do_facet ost1 $LCTL set_param obdfilter.*.stats=clear

	dd if=/dev/zero of=${testdir}/${tfile} conv=notrunc bs=512k count=1 || error "dd failed"
	sync
	cancel_lru_locks osc
	check_stats ost "write" 1

	dd if=${testdir}/${tfile} of=/dev/null bs=1k count=1 || error "dd failed"
	check_stats ost "read" 1

	> ${testdir}/${tfile} || error "truncate failed"
	check_stats ost "punch" 1

	rm -f ${testdir}/${tfile} || error "file remove failed"
	wait_delete_completed
	check_stats ost "destroy" 1

	rm -rf $DIR/${tdir}
}
run_test 133c "Verifying OST stats ========================================"

order_2() {
    local value=$1
    local orig=$value
    local order=1

    while [ $value -ge 2 ]; do
        order=$((order*2))
        value=$((value/2))
    done

    if [ $orig -gt $order ]; then
        order=$((order*2))
    fi
    echo $order
}

size_in_KMGT() {
    local value=$1
    local size=('K' 'M' 'G' 'T');
    local i=0
    local size_string=$value

    while [ $value -ge 1024 ]; do
        if [ $i -gt 3 ]; then
            #T is the biggest unit we get here, if that is bigger,
            #just return XXXT
            size_string=${value}T
            break
        fi
        value=$((value >> 10))
        if [ $value -lt 1024 ]; then
            size_string=${value}${size[$i]}
            break
        fi
        i=$((i + 1))
    done

    echo $size_string
}

get_rename_size() {
    local size=$1
    local context=${2:-.}
    local sample=$(do_facet $SINGLEMDS $LCTL get_param mdt.*.rename_stats |
		grep -A1 $context |
		awk '/ '${size}'/ {print $4}' | sed -e "s/,//g")
    echo $sample
}

test_133d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	do_facet $SINGLEMDS $LCTL list_param mdt.*.rename_stats ||
	{ skip "MDS doesn't support rename stats"; return; }

	local testdir1=$DIR/${tdir}/stats_testdir1
	local testdir2=$DIR/${tdir}/stats_testdir2

	do_facet $SINGLEMDS $LCTL set_param mdt.*.rename_stats=clear

	mkdir -p ${testdir1} || error "mkdir failed"
	mkdir -p ${testdir2} || error "mkdir failed"

	createmany -o $testdir1/test 512 || error "createmany failed"

	# check samedir rename size
	mv ${testdir1}/test0 ${testdir1}/test_0

	local testdir1_size=$(ls -l $DIR/${tdir} |
		awk '/stats_testdir1/ {print $5}')
	local testdir2_size=$(ls -l $DIR/${tdir} |
		awk '/stats_testdir2/ {print $5}')

	testdir1_size=$(order_2 $testdir1_size)
	testdir2_size=$(order_2 $testdir2_size)

	testdir1_size=$(size_in_KMGT $testdir1_size)
	testdir2_size=$(size_in_KMGT $testdir2_size)

	echo "source rename dir size: ${testdir1_size}"
	echo "target rename dir size: ${testdir2_size}"

	local cmd="do_facet $SINGLEMDS $LCTL get_param mdt.*.rename_stats"
	eval $cmd || error "$cmd failed"
	local samedir=$($cmd | grep 'same_dir')
	local same_sample=$(get_rename_size $testdir1_size)
	[ -z "$samedir" ] && error "samedir_rename_size count error"
	[ "$same_sample" -eq 1 ] || error "samedir_rename_size error $same_sample"
	echo "Check same dir rename stats success"

	do_facet $SINGLEMDS $LCTL set_param mdt.*.rename_stats=clear

	# check crossdir rename size
	mv ${testdir1}/test_0 ${testdir2}/test_0

	testdir1_size=$(ls -l $DIR/${tdir} |
		awk '/stats_testdir1/ {print $5}')
	testdir2_size=$(ls -l $DIR/${tdir} |
		awk '/stats_testdir2/ {print $5}')

	testdir1_size=$(order_2 $testdir1_size)
	testdir2_size=$(order_2 $testdir2_size)

	testdir1_size=$(size_in_KMGT $testdir1_size)
	testdir2_size=$(size_in_KMGT $testdir2_size)

	echo "source rename dir size: ${testdir1_size}"
	echo "target rename dir size: ${testdir2_size}"

	eval $cmd || error "$cmd failed"
	local crossdir=$($cmd | grep 'crossdir')
	local src_sample=$(get_rename_size $testdir1_size crossdir_src)
	local tgt_sample=$(get_rename_size $testdir2_size crossdir_tgt)
	[ -z "$crossdir" ] && error "crossdir_rename_size count error"
	[ "$src_sample" -eq 1 ] || error "crossdir_rename_size error $src_sample"
	[ "$tgt_sample" -eq 1 ] || error "crossdir_rename_size error $tgt_sample"
	echo "Check cross dir rename stats success"
	rm -rf $DIR/${tdir}
}
run_test 133d "Verifying rename_stats ========================================"

test_133e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local testdir=$DIR/${tdir}/stats_testdir
	local ctr f0 f1 bs=32768 count=42 sum

	remote_ost_nodsh && skip "remote OST with nodsh" && return
	mkdir -p ${testdir} || error "mkdir failed"

	$SETSTRIPE -c 1 -i 0 ${testdir}/${tfile}

	for ctr in {write,read}_bytes; do
		sync
		cancel_lru_locks osc

		do_facet ost1 $LCTL set_param -n \
			"obdfilter.*.exports.clear=clear"

		if [ $ctr = write_bytes ]; then
			f0=/dev/zero
			f1=${testdir}/${tfile}
		else
			f0=${testdir}/${tfile}
			f1=/dev/null
		fi

		dd if=$f0 of=$f1 conv=notrunc bs=$bs count=$count || \
			error "dd failed"
		sync
		cancel_lru_locks osc

		sum=$(do_facet ost1 $LCTL get_param \
				"obdfilter.*.exports.*.stats" | \
			  awk -v ctr=$ctr '\
				BEGIN { sum = 0 }
				$1 == ctr { sum += $7 }
				END { print sum }')

		if ((sum != bs * count)); then
			error "Bad $ctr sum, expected $((bs * count)), got $sum"
		fi
	done

	rm -rf $DIR/${tdir}
}
run_test 133e "Verifying OST {read,write}_bytes nid stats ================="

test_133f() {
	local proc_dirs="/proc/fs/lustre/ /proc/sys/lnet/ /proc/sys/lustre/"
	local facet

	# First without trusting modes.
	find $proc_dirs \
		-exec cat '{}' \; &> /dev/null

	# Second verifying readability.
	find $proc_dirs \
		-type f \
		-readable \
		-exec cat '{}' \; > /dev/null ||
			error "proc file read failed"

	for facet in $SINGLEMDS ost1; do
		do_facet $facet find $proc_dirs \
			-not -name req_history \
			-exec cat '{}' \\\; &> /dev/null

	    do_facet $facet	find $proc_dirs \
			-not -name req_history \
			-type f \
			-readable \
			-exec cat '{}' \\\; > /dev/null ||
				error "proc file read failed"
	done
}
run_test 133f "Check for LBUGs/Oopses/unreadable files in /proc"

test_140() { #bug-17379
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        test_mkdir -p $DIR/$tdir || error "Creating dir $DIR/$tdir"
        cd $DIR/$tdir || error "Changing to $DIR/$tdir"
        cp /usr/bin/stat . || error "Copying stat to $DIR/$tdir"

	# VFS limits max symlink depth to 5(4KSTACK) or 7(8KSTACK) or 8
	# For kernel > 3.5, bellow only tests consecutive symlink (MAX 40)
	local i=0
        while i=`expr $i + 1`; do
                test_mkdir -p $i || error "Creating dir $i"
                cd $i || error "Changing to $i"
                ln -s ../stat stat || error "Creating stat symlink"
                # Read the symlink until ELOOP present,
                # not LBUGing the system is considered success,
                # we didn't overrun the stack.
                $OPENFILE -f O_RDONLY stat >/dev/null 2>&1; ret=$?
                [ $ret -ne 0 ] && {
                        if [ $ret -eq 40 ]; then
                                break  # -ELOOP
                        else
                                error "Open stat symlink"
                                return
                        fi
                }
        done
        i=`expr $i - 1`
        echo "The symlink depth = $i"
	[ $i -eq 5 -o $i -eq 7 -o $i -eq 8 -o $i -eq 40 ] ||
					error "Invalid symlink depth"

	# Test recursive symlink
	ln -s symlink_self symlink_self
	$OPENFILE -f O_RDONLY symlink_self >/dev/null 2>&1; ret=$?
	echo "open symlink_self returns $ret"
	[ $ret -eq 40 ] || error "recursive symlink doesn't return -ELOOP"
}
run_test 140 "Check reasonable stack depth (shouldn't LBUG) ===="

test_150() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local TF="$TMP/$tfile"

        dd if=/dev/urandom of=$TF bs=6096 count=1 || error "dd failed"
        cp $TF $DIR/$tfile
        cancel_lru_locks osc
        cmp $TF $DIR/$tfile || error "$TMP/$tfile $DIR/$tfile differ"
        remount_client $MOUNT
        df -P $MOUNT
        cmp $TF $DIR/$tfile || error "$TF $DIR/$tfile differ (remount)"

        $TRUNCATE $TF 6000
        $TRUNCATE $DIR/$tfile 6000
        cancel_lru_locks osc
        cmp $TF $DIR/$tfile || error "$TF $DIR/$tfile differ (truncate1)"

        echo "12345" >>$TF
        echo "12345" >>$DIR/$tfile
        cancel_lru_locks osc
        cmp $TF $DIR/$tfile || error "$TF $DIR/$tfile differ (append1)"

        echo "12345" >>$TF
        echo "12345" >>$DIR/$tfile
        cancel_lru_locks osc
        cmp $TF $DIR/$tfile || error "$TF $DIR/$tfile differ (append2)"

        rm -f $TF
        true
}
run_test 150 "truncate/append tests"

function roc_hit() {
	local list=$(comma_list $(osts_nodes))
	#debug temp debug for LU-2902: lets see what values we get back
	echo $(get_osd_param $list '' stats) 1>&2
	echo $(get_osd_param $list '' stats |
	       awk '/'cache_hit'/ {sum+=$2} END {print sum}')
}

function set_cache() {
	local on=1

	if [ "$2" == "off" ]; then
		on=0;
	fi
	local list=$(comma_list $(osts_nodes))
	set_osd_param $list '' $1_cache_enable $on

	cancel_lru_locks osc
}

test_151() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return

	local CPAGES=3
	local list=$(comma_list $(osts_nodes))

	# check whether obdfilter is cache capable at all
	if ! get_osd_param $list '' read_cache_enable >/dev/null; then
		echo "not cache-capable obdfilter"
		return 0
	fi

	# check cache is enabled on all obdfilters
	if get_osd_param $list '' read_cache_enable | grep 0; then
		echo "oss cache is disabled"
		return 0
	fi

	set_osd_param $list '' writethrough_cache_enable 1

	# check write cache is enabled on all obdfilters
	if get_osd_param $list '' writethrough_cache_enable | grep 0; then
		echo "oss write cache is NOT enabled"
		return 0
	fi

#define OBD_FAIL_OBD_NO_LRU  0x609
	do_nodes $list $LCTL set_param fail_loc=0x609

	# pages should be in the case right after write
	dd if=/dev/urandom of=$DIR/$tfile bs=4k count=$CPAGES ||
		error "dd failed"

	local BEFORE=`roc_hit`
	cancel_lru_locks osc
	cat $DIR/$tfile >/dev/null
	local AFTER=`roc_hit`

	do_nodes $list $LCTL set_param fail_loc=0

	if ! let "AFTER - BEFORE == CPAGES"; then
		error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
	fi

        # the following read invalidates the cache
        cancel_lru_locks osc
	set_osd_param $list '' read_cache_enable 0
        cat $DIR/$tfile >/dev/null

        # now data shouldn't be found in the cache
        BEFORE=`roc_hit`
        cancel_lru_locks osc
        cat $DIR/$tfile >/dev/null
        AFTER=`roc_hit`
        if let "AFTER - BEFORE != 0"; then
                error "IN CACHE: before: $BEFORE, after: $AFTER"
        fi

	set_osd_param $list '' read_cache_enable 1
        rm -f $DIR/$tfile
}
run_test 151 "test cache on oss and controls ==============================="

test_152() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        local TF="$TMP/$tfile"

        # simulate ENOMEM during write
#define OBD_FAIL_OST_NOMEM      0x226
        lctl set_param fail_loc=0x80000226
        dd if=/dev/urandom of=$TF bs=6096 count=1 || error "dd failed"
        cp $TF $DIR/$tfile
        sync || error "sync failed"
        lctl set_param fail_loc=0

        # discard client's cache
        cancel_lru_locks osc

        # simulate ENOMEM during read
        lctl set_param fail_loc=0x80000226
        cmp $TF $DIR/$tfile || error "cmp failed"
        lctl set_param fail_loc=0

        rm -f $TF
}
run_test 152 "test read/write with enomem ============================"

test_153() {
        $MULTIOP $DIR/$tfile Ow4096Ycu || error "multiop failed"
}
run_test 153 "test if fdatasync does not crash ======================="

dot_lustre_fid_permission_check() {
	local fid=$1
	local ffid=$MOUNT/.lustre/fid/$fid
	local test_dir=$2

	echo "stat fid $fid"
	stat $ffid > /dev/null || error "stat $ffid failed."
	echo "touch fid $fid"
	touch $ffid || error "touch $ffid failed."
	echo "write to fid $fid"
	cat /etc/hosts > $ffid || error "write $ffid failed."
	echo "read fid $fid"
	diff /etc/hosts $ffid || error "read $ffid failed."
	echo "append write to fid $fid"
	cat /etc/hosts >> $ffid || error "append write $ffid failed."
	echo "rename fid $fid"
	mv $ffid $test_dir/$tfile.1 &&
		error "rename $ffid to $tfile.1 should fail."
	touch $test_dir/$tfile.1
	mv $test_dir/$tfile.1 $ffid &&
		error "rename $tfile.1 to $ffid should fail."
	rm -f $test_dir/$tfile.1
	echo "truncate fid $fid"
	$TRUNCATE $ffid 777 || error "truncate $ffid failed."
	echo "link fid $fid"
	ln -f $ffid $test_dir/tfile.lnk || error "link $ffid failed."
	if [ -n $(lctl get_param -n mdc.*-mdc-*.connect_flags | grep acl) ]; then
		echo "setfacl fid $fid"
		setfacl -R -m u:bin:rwx $ffid || error "setfacl $ffid failed."
		echo "getfacl fid $fid"
		getfacl $ffid >/dev/null || error "getfacl $ffid failed."
	fi
	echo "unlink fid $fid"
	unlink $MOUNT/.lustre/fid/$fid && error "unlink $ffid should fail."
	echo "mknod fid $fid"
	mknod $ffid c 1 3 && error "mknod $ffid should fail."

	fid=[0xf00000400:0x1:0x0]
	ffid=$MOUNT/.lustre/fid/$fid

	echo "stat non-exist fid $fid"
	stat $ffid > /dev/null && error "stat non-exist $ffid should fail."
	echo "write to non-exist fid $fid"
	cat /etc/hosts > $ffid && error "write non-exist $ffid should fail."
	echo "link new fid $fid"
	ln $test_dir/$tfile $ffid && error "link $ffid should fail."

	mkdir -p $test_dir/$tdir
	touch $test_dir/$tdir/$tfile
	fid=$($LFS path2fid $test_dir/$tdir)
	rc=$?
	[ $rc -ne 0 ] &&
		error "error: could not get fid for $test_dir/$dir/$tfile."

	ffid=$MOUNT/.lustre/fid/$fid

	echo "ls $fid"
	ls $ffid > /dev/null || error "ls $ffid failed."
	echo "touch $fid/$tfile.1"
	touch $ffid/$tfile.1 || error "touch $ffid/$tfile.1 failed."

	echo "touch $MOUNT/.lustre/fid/$tfile"
	touch $MOUNT/.lustre/fid/$tfile && \
		error "touch $MOUNT/.lustre/fid/$tfile should fail."

	echo "setxattr to $MOUNT/.lustre/fid"
	setfattr -n trusted.name1 -v value1 $MOUNT/.lustre/fid

	echo "listxattr for $MOUNT/.lustre/fid"
	getfattr -d -m "^trusted" $MOUNT/.lustre/fid

	echo "delxattr from $MOUNT/.lustre/fid"
	setfattr -x trusted.name1 $MOUNT/.lustre/fid

	echo "touch invalid fid: $MOUNT/.lustre/fid/[0x200000400:0x2:0x3]"
	touch $MOUNT/.lustre/fid/[0x200000400:0x2:0x3] &&
		error "touch invalid fid should fail."

	echo "touch non-normal fid: $MOUNT/.lustre/fid/[0x1:0x2:0x0]"
	touch $MOUNT/.lustre/fid/[0x1:0x2:0x0] &&
		error "touch non-normal fid should fail."

	echo "rename $tdir to $MOUNT/.lustre/fid"
	mrename $test_dir/$tdir $MOUNT/.lustre/fid &&
		error "rename to $MOUNT/.lustre/fid should fail."

	echo "rename .lustre to itself"
	fid=$($LFS path2fid $MOUNT)
	mrename $MOUNT/.lustre $MOUNT/.lustre/fid/$fid/.lustre &&
		error "rename .lustre to itself should fail."

	local old_obf_mode=$(stat --format="%a" $DIR/.lustre/fid)
	local new_obf_mode=777

	echo "change mode of $DIR/.lustre/fid to $new_obf_mode"
	chmod $new_obf_mode $DIR/.lustre/fid ||
		error "chmod $new_obf_mode $DIR/.lustre/fid failed"

	local obf_mode=$(stat --format=%a $DIR/.lustre/fid)
	[ $obf_mode -eq $new_obf_mode ] ||
		error "stat $DIR/.lustre/fid returned wrong mode $obf_mode"

	echo "restore mode of $DIR/.lustre/fid to $old_obf_mode"
	chmod $old_obf_mode $DIR/.lustre/fid ||
		error "chmod $old_obf_mode $DIR/.lustre/fid failed"

	$OPENFILE -f O_LOV_DELAY_CREATE:O_CREAT $test_dir/$tfile-2
	fid=$($LFS path2fid $test_dir/$tfile-2)
	echo "cp /etc/passwd $MOUNT/.lustre/fid/$fid"
	cp /etc/passwd $MOUNT/.lustre/fid/$fid &&
		error "create lov data thru .lustre should fail."
	echo "cp /etc/passwd $test_dir/$tfile-2"
	cp /etc/passwd $test_dir/$tfile-2 ||
		error "copy to $test_dir/$tfile-2 failed."
	echo "diff /etc/passwd $MOUNT/.lustre/fid/$fid"
	diff /etc/passwd $MOUNT/.lustre/fid/$fid ||
		error "diff /etc/passwd $MOUNT/.lustre/fid/$fid failed."

	rm -rf $test_dir/tfile.lnk
	rm -rf $test_dir/$tfile-2
}

test_154a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.2.51) ]] ||
		{ skip "Need MDS version at least 2.2.51"; return 0; }

	cp /etc/hosts $DIR/$tfile

	fid=$($LFS path2fid $DIR/$tfile)
	rc=$?
	[ $rc -ne 0 ] && error "error: could not get fid for $DIR/$tfile."

	dot_lustre_fid_permission_check "$fid" $DIR ||
		error "dot lustre permission check $fid failed"

	rm -rf $MOUNT/.lustre && error ".lustre is not allowed to be unlinked"

	touch $MOUNT/.lustre/file &&
		error "creation is not allowed under .lustre"

	mkdir $MOUNT/.lustre/dir &&
		error "mkdir is not allowed under .lustre"

	rm -rf $DIR/$tfile
}
run_test 154a "Open-by-FID"

test_154b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.2.51) ]] ||
		{ skip "Need MDS version at least 2.2.51"; return 0; }

	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return

	local remote_dir=$DIR/$tdir/remote_dir
	local MDTIDX=1
	local rc=0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX $remote_dir ||
		error "create remote directory failed"

	cp /etc/hosts $remote_dir/$tfile

	fid=$($LFS path2fid $remote_dir/$tfile)
	rc=$?
	[ $rc -ne 0 ] && error "error: could not get fid for $remote_dir/$tfile"

	dot_lustre_fid_permission_check "$fid" $remote_dir ||
		error "dot lustre permission check $fid failed"
	rm -rf $DIR/$tdir
}
run_test 154b "Open-by-FID for remote directory"

test_155_small_load() {
    local temp=$TMP/$tfile
    local file=$DIR/$tfile

    dd if=/dev/urandom of=$temp bs=6096 count=1 || \
        error "dd of=$temp bs=6096 count=1 failed"
    cp $temp $file
    cancel_lru_locks osc
    cmp $temp $file || error "$temp $file differ"

    $TRUNCATE $temp 6000
    $TRUNCATE $file 6000
    cmp $temp $file || error "$temp $file differ (truncate1)"

    echo "12345" >>$temp
    echo "12345" >>$file
    cmp $temp $file || error "$temp $file differ (append1)"

    echo "12345" >>$temp
    echo "12345" >>$file
    cmp $temp $file || error "$temp $file differ (append2)"

    rm -f $temp $file
    true
}

test_155_big_load() {
    remote_ost_nodsh && skip "remote OST with nodsh" && return
    local temp=$TMP/$tfile
    local file=$DIR/$tfile

    free_min_max
    local cache_size=$(do_facet ost$((MAXI+1)) \
        "awk '/cache/ {sum+=\\\$4} END {print sum}' /proc/cpuinfo")
    local large_file_size=$((cache_size * 2))

    echo "OSS cache size: $cache_size KB"
    echo "Large file size: $large_file_size KB"

    [ $MAXV -le $large_file_size ] && \
        skip_env "max available OST size needs > $large_file_size KB" && \
        return 0

    $SETSTRIPE $file -c 1 -i $MAXI || error "$SETSTRIPE $file failed"

    dd if=/dev/urandom of=$temp bs=$large_file_size count=1k || \
        error "dd of=$temp bs=$large_file_size count=1k failed"
    cp $temp $file
    ls -lh $temp $file
    cancel_lru_locks osc
    cmp $temp $file || error "$temp $file differ"

    rm -f $temp $file
    true
}

test_155a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set_cache read on
	set_cache writethrough on
	test_155_small_load
}
run_test 155a "Verify small file correctness: read cache:on write_cache:on"

test_155b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set_cache read on
	set_cache writethrough off
	test_155_small_load
}
run_test 155b "Verify small file correctness: read cache:on write_cache:off"

test_155c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set_cache read off
	set_cache writethrough on
	test_155_small_load
}
run_test 155c "Verify small file correctness: read cache:off write_cache:on"

test_155d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set_cache read off
	set_cache writethrough off
	test_155_small_load
}
run_test 155d "Verify small file correctness: read cache:off write_cache:off"

test_155e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set_cache read on
	set_cache writethrough on
	test_155_big_load
}
run_test 155e "Verify big file correctness: read cache:on write_cache:on"

test_155f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set_cache read on
	set_cache writethrough off
	test_155_big_load
}
run_test 155f "Verify big file correctness: read cache:on write_cache:off"

test_155g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set_cache read off
	set_cache writethrough on
	test_155_big_load
}
run_test 155g "Verify big file correctness: read cache:off write_cache:on"

test_155h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	set_cache read off
	set_cache writethrough off
	test_155_big_load
}
run_test 155h "Verify big file correctness: read cache:off write_cache:off"

test_156() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local CPAGES=3
	local BEFORE
	local AFTER
	local file="$DIR/$tfile"

	[ "$(facet_fstype ost1)" = "zfs" ] &&
		skip "LU-1956/LU-2261: stats unimplemented on OSD ZFS" &&
		return

    log "Turn on read and write cache"
    set_cache read on
    set_cache writethrough on

    log "Write data and read it back."
    log "Read should be satisfied from the cache."
    dd if=/dev/urandom of=$file bs=4k count=$CPAGES || error "dd failed"
    BEFORE=`roc_hit`
    cancel_lru_locks osc
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == CPAGES"; then
        error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi

    log "Read again; it should be satisfied from the cache."
    BEFORE=$AFTER
    cancel_lru_locks osc
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == CPAGES"; then
        error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi


    log "Turn off the read cache and turn on the write cache"
    set_cache read off
    set_cache writethrough on

    log "Read again; it should be satisfied from the cache."
    BEFORE=`roc_hit`
    cancel_lru_locks osc
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == CPAGES"; then
        error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi

    log "Read again; it should not be satisfied from the cache."
    BEFORE=$AFTER
    cancel_lru_locks osc
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == 0"; then
        error "IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi

    log "Write data and read it back."
    log "Read should be satisfied from the cache."
    dd if=/dev/urandom of=$file bs=4k count=$CPAGES || error "dd failed"
    BEFORE=`roc_hit`
    cancel_lru_locks osc
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == CPAGES"; then
        error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi

    log "Read again; it should not be satisfied from the cache."
    BEFORE=$AFTER
    cancel_lru_locks osc
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == 0"; then
        error "IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi


    log "Turn off read and write cache"
    set_cache read off
    set_cache writethrough off

    log "Write data and read it back"
    log "It should not be satisfied from the cache."
    rm -f $file
    dd if=/dev/urandom of=$file bs=4k count=$CPAGES || error "dd failed"
    cancel_lru_locks osc
    BEFORE=`roc_hit`
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == 0"; then
        error_ignore 20762 "IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi


    log "Turn on the read cache and turn off the write cache"
    set_cache read on
    set_cache writethrough off

    log "Write data and read it back"
    log "It should not be satisfied from the cache."
    rm -f $file
    dd if=/dev/urandom of=$file bs=4k count=$CPAGES || error "dd failed"
    BEFORE=`roc_hit`
    cancel_lru_locks osc
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == 0"; then
        error_ignore 20762 "IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi

    log "Read again; it should be satisfied from the cache."
    BEFORE=`roc_hit`
    cancel_lru_locks osc
    cat $file >/dev/null
    AFTER=`roc_hit`
    if ! let "AFTER - BEFORE == CPAGES"; then
        error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
    else
        log "cache hits:: before: $BEFORE, after: $AFTER"
    fi

    rm -f $file
}
run_test 156 "Verification of tunables ============================"

#Changelogs
err17935 () {
    if [ $MDSCOUNT -gt 1 ]; then
	error_ignore 17935 $*
    else
	error $*
    fi
}

changelog_chmask()
{
    MASK=$(do_facet $SINGLEMDS $LCTL get_param mdd.$MDT0.changelog_mask |\
           grep -c $1)

    if [ $MASK -eq 1 ]; then
        do_facet $SINGLEMDS $LCTL set_param mdd.$MDT0.changelog_mask="-$1"
    else
        do_facet $SINGLEMDS $LCTL set_param mdd.$MDT0.changelog_mask="+$1"
    fi
}

test_160() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
    remote_mds_nodsh && skip "remote MDS with nodsh" && return
    [ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.2.0) ] ||
        { skip "Need MDS version at least 2.2.0"; return; }
    USER=$(do_facet $SINGLEMDS $LCTL --device $MDT0 changelog_register -n)
    echo "Registered as changelog user $USER"
    do_facet $SINGLEMDS $LCTL get_param -n mdd.$MDT0.changelog_users | \
	grep -q $USER || error "User $USER not found in changelog_users"

    # change something
    test_mkdir -p $DIR/$tdir/pics/2008/zachy
    touch $DIR/$tdir/pics/2008/zachy/timestamp
    cp /etc/hosts $DIR/$tdir/pics/2008/zachy/pic1.jpg
    mv $DIR/$tdir/pics/2008/zachy $DIR/$tdir/pics/zach
    ln $DIR/$tdir/pics/zach/pic1.jpg $DIR/$tdir/pics/2008/portland.jpg
    ln -s $DIR/$tdir/pics/2008/portland.jpg $DIR/$tdir/pics/desktop.jpg
    rm $DIR/$tdir/pics/desktop.jpg

    $LFS changelog $MDT0 | tail -5

    echo "verifying changelog mask"
    changelog_chmask "MKDIR"
    changelog_chmask "CLOSE"

    test_mkdir -p $DIR/$tdir/pics/zach/sofia
    echo "zzzzzz" > $DIR/$tdir/pics/zach/file

    changelog_chmask "MKDIR"
    changelog_chmask "CLOSE"

    test_mkdir -p $DIR/$tdir/pics/2008/sofia
    echo "zzzzzz" > $DIR/$tdir/pics/zach/file

    $LFS changelog $MDT0
    MKDIRS=$($LFS changelog $MDT0 | tail -5 | grep -c "MKDIR")
    CLOSES=$($LFS changelog $MDT0 | tail -5 | grep -c "CLOSE")
    [ $MKDIRS -eq 1 ] || err17935 "MKDIR changelog mask count $DIRS != 1"
    [ $CLOSES -eq 1 ] || err17935 "CLOSE changelog mask count $DIRS != 1"

    # verify contents
    echo "verifying target fid"
    fidc=$($LFS changelog $MDT0 | grep timestamp | grep "CREAT" | \
	tail -1 | awk '{print $6}')
    fidf=$($LFS path2fid $DIR/$tdir/pics/zach/timestamp)
    [ "$fidc" == "t=$fidf" ] || \
	err17935 "fid in changelog $fidc != file fid $fidf"
    echo "verifying parent fid"
    fidc=$($LFS changelog $MDT0 | grep timestamp | grep "CREAT" | \
	tail -1 | awk '{print $7}')
    fidf=$($LFS path2fid $DIR/$tdir/pics/zach)
    [ "$fidc" == "p=$fidf" ] || \
	err17935 "pfid in changelog $fidc != dir fid $fidf"

    USER_REC1=$(do_facet $SINGLEMDS $LCTL get_param -n \
	mdd.$MDT0.changelog_users | grep $USER | awk '{print $2}')
    $LFS changelog_clear $MDT0 $USER $(($USER_REC1 + 5))
    USER_REC2=$(do_facet $SINGLEMDS $LCTL get_param -n \
	mdd.$MDT0.changelog_users | grep $USER | awk '{print $2}')
    echo "verifying user clear: $(( $USER_REC1 + 5 )) == $USER_REC2"
    [ $USER_REC2 == $(($USER_REC1 + 5)) ] || \
	err17935 "user index should be $(($USER_REC1 + 5)); is $USER_REC2"

    MIN_REC=$(do_facet $SINGLEMDS $LCTL get_param mdd.$MDT0.changelog_users | \
	awk 'min == "" || $2 < min {min = $2}; END {print min}')
    FIRST_REC=$($LFS changelog $MDT0 | head -1 | awk '{print $1}')
    echo "verifying min purge: $(( $MIN_REC + 1 )) == $FIRST_REC"
    [ $FIRST_REC == $(($MIN_REC + 1)) ] || \
	err17935 "first index should be $(($MIN_REC + 1)); is $FIRST_REC"

    echo "verifying user deregister"
    do_facet $SINGLEMDS $LCTL --device $MDT0 changelog_deregister $USER
    do_facet $SINGLEMDS $LCTL get_param -n mdd.$MDT0.changelog_users | \
	grep -q $USER && error "User $USER still found in changelog_users"

    USERS=$(( $(do_facet $SINGLEMDS $LCTL get_param -n \
	mdd.$MDT0.changelog_users | wc -l) - 2 ))
    if [ $USERS -eq 0 ]; then
	LAST_REC1=$(do_facet $SINGLEMDS $LCTL get_param -n \
	    mdd.$MDT0.changelog_users | head -1 | awk '{print $3}')
	touch $DIR/$tdir/chloe
	LAST_REC2=$(do_facet $SINGLEMDS $LCTL get_param -n \
	    mdd.$MDT0.changelog_users | head -1 | awk '{print $3}')
	echo "verify changelogs are off if we were the only user: $LAST_REC1 == $LAST_REC2"
	[ $LAST_REC1 == $LAST_REC2 ] || error "changelogs not off"
    else
	echo "$USERS other changelog users; can't verify off"
    fi
}
run_test 160 "changelog sanity"

test_161a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
    test_mkdir -p $DIR/$tdir
    cp /etc/hosts $DIR/$tdir/$tfile
    test_mkdir $DIR/$tdir/foo1
    test_mkdir $DIR/$tdir/foo2
    ln $DIR/$tdir/$tfile $DIR/$tdir/foo1/sofia
    ln $DIR/$tdir/$tfile $DIR/$tdir/foo2/zachary
    ln $DIR/$tdir/$tfile $DIR/$tdir/foo1/luna
    ln $DIR/$tdir/$tfile $DIR/$tdir/foo2/thor
	local FID=$($LFS path2fid $DIR/$tdir/$tfile | tr -d '[]')
	if [ "$($LFS fid2path $DIR $FID | wc -l)" != "5" ]; then
		$LFS fid2path $DIR $FID
		err17935 "bad link ea"
	fi
    # middle
    rm $DIR/$tdir/foo2/zachary
    # last
    rm $DIR/$tdir/foo2/thor
    # first
    rm $DIR/$tdir/$tfile
    # rename
    mv $DIR/$tdir/foo1/sofia $DIR/$tdir/foo2/maggie
    if [ "$($LFS fid2path $FSNAME --link 1 $FID)" != "$tdir/foo2/maggie" ]
	then
	$LFS fid2path $DIR $FID
	err17935 "bad link rename"
    fi
    rm $DIR/$tdir/foo2/maggie

    # overflow the EA
    local longname=filename_avg_len_is_thirty_two_
    createmany -l$DIR/$tdir/foo1/luna $DIR/$tdir/foo2/$longname 1000 || \
	error "failed to hardlink many files"
    links=$($LFS fid2path $DIR $FID | wc -l)
    echo -n "${links}/1000 links in link EA"
    [ ${links} -gt 60 ] || err17935 "expected at least 60 links in link EA"
    unlinkmany $DIR/$tdir/foo2/$longname 1000 || \
	error "failed to unlink many hardlinks"
}
run_test 161a "link ea sanity"

test_161b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ $MDSCOUNT -lt 2 ] &&
		skip "skipping remote directory test" && return
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX $remote_dir ||
		error "create remote directory failed"

	cp /etc/hosts $remote_dir/$tfile
	mkdir -p $remote_dir/foo1
	mkdir -p $remote_dir/foo2
	ln $remote_dir/$tfile $remote_dir/foo1/sofia
	ln $remote_dir/$tfile $remote_dir/foo2/zachary
	ln $remote_dir/$tfile $remote_dir/foo1/luna
	ln $remote_dir/$tfile $remote_dir/foo2/thor

	local FID=$($LFS path2fid $remote_dir/$tfile | tr -d '[' |
		     tr -d ']')
	if [ "$($LFS fid2path $DIR $FID | wc -l)" != "5" ]; then
		$LFS fid2path $DIR $FID
		err17935 "bad link ea"
	fi
	# middle
	rm $remote_dir/foo2/zachary
	# last
	rm $remote_dir/foo2/thor
	# first
	rm $remote_dir/$tfile
	# rename
	mv $remote_dir/foo1/sofia $remote_dir/foo2/maggie
	local link_path=$($LFS fid2path $FSNAME --link 1 $FID)
	if [ "$DIR/$link_path" != "$remote_dir/foo2/maggie" ]; then
		$LFS fid2path $DIR $FID
		err17935 "bad link rename"
	fi
	rm $remote_dir/foo2/maggie

	# overflow the EA
	local longname=filename_avg_len_is_thirty_two_
	createmany -l$remote_dir/foo1/luna $remote_dir/foo2/$longname 1000 ||
		error "failed to hardlink many files"
	links=$($LFS fid2path $DIR $FID | wc -l)
	echo -n "${links}/1000 links in link EA"
	[ ${links} -gt 60 ] || err17935 "expected at least 60 links in link EA"
	unlinkmany $remote_dir/foo2/$longname 1000 ||
	error "failed to unlink many hardlinks"
}
run_test 161b "link ea sanity under remote directory"

check_path() {
    local expected=$1
    shift
    local fid=$2

    local path=$(${LFS} fid2path $*)
    RC=$?

    if [ $RC -ne 0 ]; then
      	err17935 "path looked up of $expected failed. Error $RC"
 	return $RC
    elif [ "${path}" != "${expected}" ]; then
      	err17935 "path looked up \"${path}\" instead of \"${expected}\""
 	return 2
    fi
    echo "fid $fid resolves to path $path (expected $expected)"
}

test_162() {
	# Make changes to filesystem
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	test_mkdir -p $DIR/$tdir/d2
	touch $DIR/$tdir/d2/$tfile
	touch $DIR/$tdir/d2/x1
	touch $DIR/$tdir/d2/x2
	test_mkdir -p $DIR/$tdir/d2/a/b/c
	test_mkdir -p $DIR/$tdir/d2/p/q/r
	# regular file
	FID=$($LFS path2fid $DIR/$tdir/d2/$tfile | tr -d '[]')
	check_path "$tdir/d2/$tfile" $FSNAME $FID --link 0

	# softlink
	ln -s $DIR/$tdir/d2/$tfile $DIR/$tdir/d2/p/q/r/slink
	FID=$($LFS path2fid $DIR/$tdir/d2/p/q/r/slink | tr -d '[]')
	check_path "$tdir/d2/p/q/r/slink" $FSNAME $FID --link 0

	# softlink to wrong file
	ln -s /this/is/garbage $DIR/$tdir/d2/p/q/r/slink.wrong
	FID=$($LFS path2fid $DIR/$tdir/d2/p/q/r/slink.wrong | tr -d '[]')
	check_path "$tdir/d2/p/q/r/slink.wrong" $FSNAME $FID --link 0

	# hardlink
	ln $DIR/$tdir/d2/$tfile $DIR/$tdir/d2/p/q/r/hlink
	mv $DIR/$tdir/d2/$tfile $DIR/$tdir/d2/a/b/c/new_file
	FID=$($LFS path2fid $DIR/$tdir/d2/a/b/c/new_file | tr -d '[]')
	# fid2path dir/fsname should both work
	check_path "$tdir/d2/a/b/c/new_file" $FSNAME $FID --link 1
	check_path "$DIR/$tdir/d2/p/q/r/hlink" $DIR $FID --link 0

	# hardlink count: check that there are 2 links
	# Doesnt work with CMD yet: 17935
	${LFS} fid2path $DIR $FID | wc -l | grep -q 2 || \
		err17935 "expected 2 links"

	# hardlink indexing: remove the first link
	rm $DIR/$tdir/d2/p/q/r/hlink
	check_path "$tdir/d2/a/b/c/new_file" $FSNAME $FID --link 0

	return 0
}
run_test 162 "path lookup sanity"

test_163() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	copytool --test $FSNAME || { skip "copytool not runnable: $?" && return; }
	copytool $FSNAME &
	sleep 1
	local uuid=$($LCTL get_param -n mdc.${FSNAME}-MDT0000-mdc-*.uuid)
	# this proc file is temporary and linux-only
	do_facet $SINGLEMDS lctl set_param mdt.${FSNAME}-MDT0000.mdccomm=$uuid ||\
         error "kernel->userspace send failed"
	kill -INT $!
}
run_test 163 "kernel <-> userspace comms"

test_169() {
	# do directio so as not to populate the page cache
	log "creating a 10 Mb file"
	$MULTIOP $DIR/$tfile oO_CREAT:O_DIRECT:O_RDWR:w$((10*1048576))c || error "multiop failed while creating a file"
	log "starting reads"
	dd if=$DIR/$tfile of=/dev/null bs=4096 &
	log "truncating the file"
	$MULTIOP $DIR/$tfile oO_TRUNC:c || error "multiop failed while truncating the file"
	log "killing dd"
	kill %+ || true # reads might have finished
	echo "wait until dd is finished"
	wait
	log "removing the temporary file"
	rm -rf $DIR/$tfile || error "tmp file removal failed"
}
run_test 169 "parallel read and truncate should not deadlock"

test_170() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        $LCTL clear	# bug 18514
        $LCTL debug_daemon start $TMP/${tfile}_log_good
        touch $DIR/$tfile
        $LCTL debug_daemon stop
        sed -e "s/^...../a/g" $TMP/${tfile}_log_good > $TMP/${tfile}_log_bad ||
               error "sed failed to read log_good"

        $LCTL debug_daemon start $TMP/${tfile}_log_good
        rm -rf $DIR/$tfile
        $LCTL debug_daemon stop

        $LCTL df $TMP/${tfile}_log_bad > $TMP/${tfile}_log_bad.out 2>&1 ||
               error "lctl df log_bad failed"

        local bad_line=$(tail -n 1 $TMP/${tfile}_log_bad.out | awk '{print $9}')
        local good_line1=$(tail -n 1 $TMP/${tfile}_log_bad.out | awk '{print $5}')

        $LCTL df $TMP/${tfile}_log_good > $TMP/${tfile}_log_good.out 2>&1
        local good_line2=$(tail -n 1 $TMP/${tfile}_log_good.out | awk '{print $5}')

	[ "$bad_line" ] && [ "$good_line1" ] && [ "$good_line2" ] ||
		error "bad_line good_line1 good_line2 are empty"

        cat $TMP/${tfile}_log_good >> $TMP/${tfile}_logs_corrupt
        cat $TMP/${tfile}_log_bad >> $TMP/${tfile}_logs_corrupt
        cat $TMP/${tfile}_log_good >> $TMP/${tfile}_logs_corrupt

        $LCTL df $TMP/${tfile}_logs_corrupt > $TMP/${tfile}_log_bad.out 2>&1
        local bad_line_new=$(tail -n 1 $TMP/${tfile}_log_bad.out | awk '{print $9}')
        local good_line_new=$(tail -n 1 $TMP/${tfile}_log_bad.out | awk '{print $5}')

	[ "$bad_line_new" ] && [ "$good_line_new" ] ||
		error "bad_line_new good_line_new are empty"

        local expected_good=$((good_line1 + good_line2*2))

        rm -f $TMP/${tfile}*
	# LU-231, short malformed line may not be counted into bad lines
        if [ $bad_line -ne $bad_line_new ] &&
		   [ $bad_line -ne $((bad_line_new - 1)) ]; then
                error "expected $bad_line bad lines, but got $bad_line_new"
                return 1
        fi

        if [ $expected_good -ne $good_line_new ]; then
                error "expected $expected_good good lines, but got $good_line_new"
                return 2
        fi
        true
}
run_test 170 "test lctl df to handle corrupted log ====================="

test_171() { # bug20592
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
#define OBD_FAIL_PTLRPC_DUMP_LOG         0x50e
        $LCTL set_param fail_loc=0x50e
        $LCTL set_param fail_val=3000
        multiop_bg_pause $DIR/$tfile O_s || true
        local MULTIPID=$!
        kill -USR1 $MULTIPID
        # cause log dump
        sleep 3
        wait $MULTIPID
        if dmesg | grep "recursive fault"; then
                error "caught a recursive fault"
        fi
        $LCTL set_param fail_loc=0
        true
}
run_test 171 "test libcfs_debug_dumplog_thread stuck in do_exit() ======"

# it would be good to share it with obdfilter-survey/libecho code
setup_obdecho_osc () {
        local rc=0
        local ost_nid=$1
        local obdfilter_name=$2
        echo "Creating new osc for $obdfilter_name on $ost_nid"
        # make sure we can find loopback nid
        $LCTL add_uuid $ost_nid $ost_nid >/dev/null 2>&1

        [ $rc -eq 0 ] && { $LCTL attach osc ${obdfilter_name}_osc     \
                           ${obdfilter_name}_osc_UUID || rc=2; }
        [ $rc -eq 0 ] && { $LCTL --device ${obdfilter_name}_osc setup \
                           ${obdfilter_name}_UUID  $ost_nid || rc=3; }
        return $rc
}

cleanup_obdecho_osc () {
        local obdfilter_name=$1
        $LCTL --device ${obdfilter_name}_osc cleanup >/dev/null
        $LCTL --device ${obdfilter_name}_osc detach  >/dev/null
        return 0
}

obdecho_create_test() {
        local OBD=$1
        local node=$2
        local rc=0
        local id
        do_facet $node "$LCTL attach echo_client ec ec_uuid" || rc=1
        [ $rc -eq 0 ] && { do_facet $node "$LCTL --device ec setup $OBD" ||
                           rc=2; }
        if [ $rc -eq 0 ]; then
            id=$(do_facet $node "$LCTL --device ec create 1"  | awk '/object id/ {print $6}')
            [ ${PIPESTATUS[0]} -eq 0 -a -n "$id" ] || rc=3
        fi
        echo "New object id is $id"
        [ $rc -eq 0 ] && { do_facet $node "$LCTL --device ec test_brw 10 w v 64 $id" ||
                           rc=4; }
        [ $rc -eq 0 -o $rc -gt 2 ] && { do_facet $node "$LCTL --device ec "    \
                                        "cleanup" || rc=5; }
        [ $rc -eq 0 -o $rc -gt 1 ] && { do_facet $node "$LCTL --device ec "    \
                                        "detach" || rc=6; }
        [ $rc -ne 0 ] && echo "obecho_create_test failed: $rc"
        return $rc
}

test_180a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        remote_ost_nodsh && skip "remote OST with nodsh" && return
        local rc=0
        local rmmod_local=0

        if ! module_loaded obdecho; then
            load_module obdecho/obdecho
            rmmod_local=1
        fi

        local osc=$($LCTL dl | grep -v mdt | awk '$3 == "osc" {print $4; exit}')
        local host=$(lctl get_param -n osc.$osc.import |
                             awk '/current_connection:/ {print $2}' )
        local target=$(lctl get_param -n osc.$osc.import |
                             awk '/target:/ {print $2}' )
        target=${target%_UUID}

        [[ -n $target ]]  && { setup_obdecho_osc $host $target || rc=1; } || rc=1
        [ $rc -eq 0 ] && { obdecho_create_test ${target}_osc client || rc=2; }
        [[ -n $target ]] && cleanup_obdecho_osc $target
        [ $rmmod_local -eq 1 ] && rmmod obdecho
        return $rc
}
run_test 180a "test obdecho on osc"

test_180b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        remote_ost_nodsh && skip "remote OST with nodsh" && return
        local rc=0
        local rmmod_remote=0

        do_facet ost1 "lsmod | grep -q obdecho || "                      \
                      "{ insmod ${LUSTRE}/obdecho/obdecho.ko || "        \
                      "modprobe obdecho; }" && rmmod_remote=1
        target=$(do_facet ost1 $LCTL dl | awk '/obdfilter/ {print $4;exit}')
        [[ -n $target ]] && { obdecho_create_test $target ost1 || rc=1; }
        [ $rmmod_remote -eq 1 ] && do_facet ost1 "rmmod obdecho"
        return $rc
}
run_test 180b "test obdecho directly on obdfilter"

test_181() { # bug 22177
	test_mkdir -p $DIR/$tdir || error "creating dir $DIR/$tdir"
	# create enough files to index the directory
	createmany -o $DIR/$tdir/foobar 4000
	# print attributes for debug purpose
	lsattr -d .
	# open dir
	multiop_bg_pause $DIR/$tdir D_Sc || return 1
	MULTIPID=$!
	# remove the files & current working dir
	unlinkmany $DIR/$tdir/foobar 4000
	rmdir $DIR/$tdir
	kill -USR1 $MULTIPID
	wait $MULTIPID
	stat $DIR/$tdir && error "open-unlinked dir was not removed!"
	return 0
}
run_test 181 "Test open-unlinked dir ========================"

test_182() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	# disable MDC RPC lock wouldn't crash client
	local fcount=1000
	local tcount=4

	mkdir -p $DIR/$tdir || error "creating dir $DIR/$tdir"
#define OBD_FAIL_MDC_RPCS_SEM		0x804
	$LCTL set_param fail_loc=0x804

	for (( i=0; i < $tcount; i++ )) ; do
		mkdir $DIR/$tdir/$i
		createmany -o $DIR/$tdir/$i/f- $fcount &
	done
	wait

	for (( i=0; i < $tcount; i++ )) ; do
		unlinkmany $DIR/$tdir/$i/f- $fcount &
	done
	wait

	rm -rf $DIR/$tdir

	$LCTL set_param fail_loc=0
}
run_test 182 "Disable MDC RPCs semaphore wouldn't crash client ================"

test_183() { # LU-2275
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	mkdir -p $DIR/$tdir || error "creating dir $DIR/$tdir"
	echo aaa > $DIR/$tdir/$tfile

#define OBD_FAIL_MDS_NEGATIVE_POSITIVE  0x148
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0x148

	ls -l $DIR/$tdir && error "ls succeeded, should have failed"
	cat $DIR/$tdir/$tfile && error "cat succeeded, should have failed"

	do_facet $SINGLEMDS $LCTL set_param fail_loc=0

	# Flush negative dentry cache
	touch $DIR/$tdir/$tfile

	# We are not checking for any leaked references here, they'll
	# become evident next time we do cleanup with module unload.
	rm -rf $DIR/$tdir
}
run_test 183 "No crash or request leak in case of strange dispositions ========"

# test suite 184 is for LU-2016, LU-2017
test_184a() {
	check_swap_layouts_support && return 0

	dir0=$DIR/$tdir/$testnum
	test_mkdir -p $dir0 || error "creating dir $dir0"
	ref1=/etc/passwd
	ref2=/etc/group
	file1=$dir0/f1
	file2=$dir0/f2
	$SETSTRIPE -c1 $file1
	cp $ref1 $file1
	$SETSTRIPE -c2 $file2
	cp $ref2 $file2
	gen1=$($GETSTRIPE -g $file1)
	gen2=$($GETSTRIPE -g $file2)

	$LFS swap_layouts $file1 $file2 || error "swap of file layout failed"
	gen=$($GETSTRIPE -g $file1)
	[[ $gen1 != $gen ]] ||
		"Layout generation on $file1 does not change"
	gen=$($GETSTRIPE -g $file2)
	[[ $gen2 != $gen ]] ||
		"Layout generation on $file2 does not change"

	cmp $ref1 $file2 || error "content compare failed ($ref1 != $file2)"
	cmp $ref2 $file1 || error "content compare failed ($ref2 != $file1)"
}
run_test 184a "Basic layout swap"

test_184b() {
	check_swap_layouts_support && return 0

	dir0=$DIR/$tdir/$testnum
	mkdir -p $dir0 || error "creating dir $dir0"
	file1=$dir0/f1
	file2=$dir0/f2
	file3=$dir0/f3
	dir1=$dir0/d1
	dir2=$dir0/d2
	mkdir $dir1 $dir2
	$SETSTRIPE -c1 $file1
	$SETSTRIPE -c2 $file2
	$SETSTRIPE -c1 $file3
	chown $RUNAS_ID $file3
	gen1=$($GETSTRIPE -g $file1)
	gen2=$($GETSTRIPE -g $file2)

	$LFS swap_layouts $dir1 $dir2 &&
		error "swap of directories layouts should fail"
	$LFS swap_layouts $dir1 $file1 &&
		error "swap of directory and file layouts should fail"
	$RUNAS $LFS swap_layouts $file1 $file2 &&
		error "swap of file we cannot write should fail"
	$LFS swap_layouts $file1 $file3 &&
		error "swap of file with different owner should fail"
	/bin/true # to clear error code
}
run_test 184b "Forbidden layout swap (will generate errors)"

test_184c() {
	check_swap_layouts_support && return 0

	local dir0=$DIR/$tdir/$testnum
	mkdir -p $dir0 || error "creating dir $dir0"

	local ref1=$dir0/ref1
	local ref2=$dir0/ref2
	local file1=$dir0/file1
	local file2=$dir0/file2
	# create a file large enough for the concurent test
	dd if=/dev/urandom of=$ref1 bs=1M count=$((RANDOM % 50 + 20))
	dd if=/dev/urandom of=$ref2 bs=1M count=$((RANDOM % 50 + 20))
	echo "ref file size: ref1(`stat -c %s $ref1`), ref2(`stat -c %s $ref2`)"

	cp $ref2 $file2
	dd if=$ref1 of=$file1 bs=16k &
	local DD_PID=$!

	# Make sure dd starts to copy file
	while [ ! -f $file1 ]; do sleep 0.1; done

	$LFS swap_layouts $file1 $file2
	local rc=$?
	wait $DD_PID
	[[ $? == 0 ]] || error "concurrent write on $file1 failed"
	[[ $rc == 0 ]] || error "swap of $file1 and $file2 failed"

	# how many bytes copied before swapping layout
	local copied=`stat -c %s $file2`
	local remaining=`stat -c %s $ref1`
	remaining=$((remaining - copied))
	echo "Copied $copied bytes before swapping layout..."

	cmp -n $copied $file1 $ref2 | grep differ &&
		error "Content mismatch [0, $copied) of ref2 and file1"
	cmp -n $copied $file2 $ref1 ||
		error "Content mismatch [0, $copied) of ref1 and file2"
	cmp -i $copied:$copied -n $remaining $file1 $ref1 ||
		error "Content mismatch [$copied, EOF) of ref1 and file1"

	# clean up
	rm -f $ref1 $ref2 $file1 $file2
}
run_test 184c "Concurrent write and layout swap"

test_185() { # LU-2441
	mkdir -p $DIR/$tdir || error "creating dir $DIR/$tdir"
	touch $DIR/$tdir/spoo
	local mtime1=$(stat -c "%Y" $DIR/$tdir)
	local fid=$($MULTIOP $DIR/$tdir VFw4096c) ||
		error "cannot create/write a volatile file"
	$CHECKSTAT -t file $MOUNT/.lustre/fid/$fid 2>/dev/null &&
		error "FID is still valid after close"

	multiop_bg_pause $DIR/$tdir vVw4096_c
	local multi_pid=$!

	local OLD_IFS=$IFS
	IFS=":"
	local fidv=($fid)
	IFS=$OLD_IFS
	# assume that the next FID for this client is sequential, since stdout
	# is unfortunately eaten by multiop_bg_pause
	local n=$((${fidv[1]} + 1))
	local next_fid="${fidv[0]}:$(printf "0x%x" $n):${fidv[2]}"
	$CHECKSTAT -t file $MOUNT/.lustre/fid/$next_fid ||
		error "FID is missing before close"
	kill -USR1 $multi_pid
	# 1 second delay, so if mtime change we will see it
	sleep 1
	local mtime2=$(stat -c "%Y" $DIR/$tdir)
	[[ $mtime1 == $mtime2 ]] || error "mtime has changed"
}
run_test 185 "Volatile file support"

# OST pools tests
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
	[ $($LFS pool_list $FSNAME | grep -c $pool) -eq 1 ] ||
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

test_200() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_mgs_nodsh && skip "remote MGS with nodsh" && return

	local POOL=${POOL:-cea1}
	local POOL_ROOT=${POOL_ROOT:-$DIR/d200.pools}
	local POOL_DIR_NAME=${POOL_DIR_NAME:-dir_tst}
	# Pool OST targets
	local first_ost=0
	local last_ost=$(($OSTCOUNT - 1))
	local ost_step=2
	local ost_list=$(seq $first_ost $ost_step $last_ost)
	local ost_range="$first_ost $last_ost $ost_step"
	local test_path=$POOL_ROOT/$POOL_DIR_NAME
	local file_dir=$POOL_ROOT/file_tst

	local rc=0
	while : ; do
		# former test_200a test_200b
		pool_add $POOL				|| { rc=$? ; break; }
		pool_add_targets  $POOL $ost_range	|| { rc=$? ; break; }
		# former test_200c test_200d
		mkdir -p $test_path
		pool_set_dir      $POOL $test_path	|| { rc=$? ; break; }
		pool_check_dir    $POOL $test_path	|| { rc=$? ; break; }
		pool_dir_rel_path $POOL $POOL_DIR_NAME $POOL_ROOT \
							|| { rc=$? ; break; }
		# former test_200e test_200f
		local files=$((OSTCOUNT*3))
		pool_alloc_files  $POOL $test_path $files "$ost_list" \
							|| { rc=$? ; break; }
		pool_create_files $POOL $file_dir $files "$ost_list" \
							|| { rc=$? ; break; }
		# former test_200g test_200h
		pool_lfs_df $POOL 			|| { rc=$? ; break; }
		pool_file_rel_path $POOL $test_path	|| { rc=$? ; break; }

		# former test_201a test_201b test_201c
		pool_remove_first_target $POOL		|| { rc=$? ; break; }

		local f=$test_path/$tfile
		pool_remove_all_targets $POOL $f	|| { rc=$? ; break; }
		pool_remove $POOL $f 			|| { rc=$? ; break; }
		break
	done

	cleanup_pools
	return $rc
}
run_test 200 "OST pools"

# usage: default_attr <count | size | offset>
default_attr() {
	$LCTL get_param -n lov.$FSNAME-clilov-\*.stripe${1}
}

# usage: check_default_stripe_attr
check_default_stripe_attr() {
	ACTUAL=$($GETSTRIPE $* $DIR/$tdir)
	case $1 in
	--stripe-count|--count)
		[ -n "$2" ] && EXPECTED=0 || EXPECTED=$(default_attr count);;
	--stripe-size|--size)
		[ -n "$2" ] && EXPECTED=0 || EXPECTED=$(default_attr size);;
	--stripe-index|--index)
		EXPECTED=-1;;
	*)
		error "unknown getstripe attr '$1'"
	esac

	[ $ACTUAL != $EXPECTED ] &&
		error "$DIR/$tdir has $1 '$ACTUAL', not '$EXPECTED'"
}

test_204a() {
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE --stripe-count 0 --stripe-size 0 --stripe-index -1 $DIR/$tdir

	check_default_stripe_attr --stripe-count
	check_default_stripe_attr --stripe-size
	check_default_stripe_attr --stripe-index

	return 0
}
run_test 204a "Print default stripe attributes ================="

test_204b() {
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE --stripe-count 1 $DIR/$tdir

	check_default_stripe_attr --stripe-size
	check_default_stripe_attr --stripe-index

	return 0
}
run_test 204b "Print default stripe size and offset  ==========="

test_204c() {
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE --stripe-size 65536 $DIR/$tdir

	check_default_stripe_attr --stripe-count
	check_default_stripe_attr --stripe-index

	return 0
}
run_test 204c "Print default stripe count and offset ==========="

test_204d() {
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE --stripe-index 0 $DIR/$tdir

	check_default_stripe_attr --stripe-count
	check_default_stripe_attr --stripe-size

	return 0
}
run_test 204d "Print default stripe count and size ============="

test_204e() {
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE -d $DIR/$tdir

	check_default_stripe_attr --stripe-count --raw
	check_default_stripe_attr --stripe-size --raw
	check_default_stripe_attr --stripe-index --raw

	return 0
}
run_test 204e "Print raw stripe attributes ================="

test_204f() {
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE --stripe-count 1 $DIR/$tdir

	check_default_stripe_attr --stripe-size --raw
	check_default_stripe_attr --stripe-index --raw

	return 0
}
run_test 204f "Print raw stripe size and offset  ==========="

test_204g() {
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE --stripe-size 65536 $DIR/$tdir

	check_default_stripe_attr --stripe-count --raw
	check_default_stripe_attr --stripe-index --raw

	return 0
}
run_test 204g "Print raw stripe count and offset ==========="

test_204h() {
	test_mkdir -p $DIR/$tdir
	$SETSTRIPE --stripe-index 0 $DIR/$tdir

	check_default_stripe_attr --stripe-count --raw
	check_default_stripe_attr --stripe-size --raw

	return 0
}
run_test 204h "Print raw stripe count and size ============="

# Figure out which job scheduler is being used, if any,
# or use a fake one
if [ -n "$SLURM_JOB_ID" ]; then # SLURM
	JOBENV=SLURM_JOB_ID
elif [ -n "$LSB_JOBID" ]; then # Load Sharing Facility
	JOBENV=LSB_JOBID
elif [ -n "$PBS_JOBID" ]; then # PBS/Maui/Moab
	JOBENV=PBS_JOBID
elif [ -n "$LOADL_STEPID" ]; then # LoadLeveller
	JOBENV=LOADL_STEP_ID
elif [ -n "$JOB_ID" ]; then # Sun Grid Engine
	JOBENV=JOB_ID
else
	JOBENV=FAKE_JOBID
fi

verify_jobstats() {
	local cmd=$1
	local target=$2

	# clear old jobstats
	do_facet $SINGLEMDS lctl set_param mdt.*.job_stats="clear"
	do_facet ost1 lctl set_param obdfilter.*.job_stats="clear"

	# use a new JobID for this test, or we might see an old one
	[ "$JOBENV" = "FAKE_JOBID" ] && FAKE_JOBID=test_id.$testnum.$RANDOM

	JOBVAL=${!JOBENV}
	log "Test: $cmd"
	log "Using JobID environment variable $JOBENV=$JOBVAL"

	if [ $JOBENV = "FAKE_JOBID" ]; then
		FAKE_JOBID=$JOBVAL $cmd
	else
		$cmd
	fi

	if [ "$target" = "mdt" -o "$target" = "both" ]; then
		FACET="$SINGLEMDS" # will need to get MDS number for DNE
		do_facet $FACET lctl get_param mdt.*.job_stats |
			grep $JOBVAL || error "No job stats found on MDT $FACET"
	fi
	if [ "$target" = "ost" -o "$target" = "both" ]; then
		FACET=ost1
		do_facet $FACET lctl get_param obdfilter.*.job_stats |
			grep $JOBVAL || error "No job stats found on OST $FACET"
	fi
}

jobstats_set() {
	trap 0
	NEW_JOBENV=${1:-$OLD_JOBENV}
	do_facet mgs $LCTL conf_param $FSNAME.sys.jobid_var=$NEW_JOBENV
	wait_update $HOSTNAME "$LCTL get_param -n jobid_var" $NEW_JOBENV
}

test_205() { # Job stats
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ -z "$(lctl get_param -n mdc.*.connect_flags | grep jobstats)" ] &&
		skip "Server doesn't support jobstats" && return 0

	local cmd
	OLD_JOBENV=`$LCTL get_param -n jobid_var`
	if [ $OLD_JOBENV != $JOBENV ]; then
		jobstats_set $JOBENV
		trap jobstats_set EXIT
	fi

	# mkdir
	cmd="mkdir $DIR/$tfile"
	verify_jobstats "$cmd" "mdt"
	# rmdir
	cmd="rm -fr $DIR/$tfile"
	verify_jobstats "$cmd" "mdt"
	# mknod
	cmd="mknod $DIR/$tfile c 1 3"
	verify_jobstats "$cmd" "mdt"
	# unlink
	cmd="rm -f $DIR/$tfile"
	verify_jobstats "$cmd" "mdt"
	# open & close
	cmd="$SETSTRIPE -i 0 -c 1 $DIR/$tfile"
	verify_jobstats "$cmd" "mdt"
	# setattr
	cmd="touch $DIR/$tfile"
	verify_jobstats "$cmd" "both"
	# write
	cmd="dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 oflag=sync"
	verify_jobstats "$cmd" "ost"
	# read
	cmd="dd if=$DIR/$tfile of=/dev/null bs=1M count=1 iflag=direct"
	verify_jobstats "$cmd" "ost"
	# truncate
	cmd="$TRUNCATE $DIR/$tfile 0"
	verify_jobstats "$cmd" "both"
	# rename
	cmd="mv -f $DIR/$tfile $DIR/jobstats_test_rename"
	verify_jobstats "$cmd" "mdt"

	# cleanup
	rm -f $DIR/jobstats_test_rename

	[ $OLD_JOBENV != $JOBENV ] && jobstats_set $OLD_JOBENV
}
run_test 205 "Verify job stats"

# LU-1480, LU-1773 and LU-1657
test_206() {
	mkdir -p $DIR/$tdir
	lfs setstripe -c -1 $DIR/$tdir
#define OBD_FAIL_LOV_INIT 0x1403
	$LCTL set_param fail_loc=0xa0001403
	$LCTL set_param fail_val=1
	touch $DIR/$tdir/$tfile || true
}
run_test 206 "fail lov_init_raid0() doesn't lbug"

test_207a() {
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=$((RANDOM%10+1))
	local fsz=`stat -c %s $DIR/$tfile`
	cancel_lru_locks mdc

	# do not return layout in getattr intent
#define OBD_FAIL_MDS_NO_LL_GETATTR 0x170
	$LCTL set_param fail_loc=0x170
	local sz=`stat -c %s $DIR/$tfile`

	[ $fsz -eq $sz ] || error "file size expected $fsz, actual $sz"

	rm -rf $DIR/$tfile
}
run_test 207a "can refresh layout at glimpse"

test_207b() {
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=$((RANDOM%10+1))
	local cksum=`md5sum $DIR/$tfile`
	local fsz=`stat -c %s $DIR/$tfile`
	cancel_lru_locks mdc
	cancel_lru_locks osc

	# do not return layout in getattr intent
#define OBD_FAIL_MDS_NO_LL_OPEN 0x171
	$LCTL set_param fail_loc=0x171

	# it will refresh layout after the file is opened but before read issues
	echo checksum is "$cksum"
	echo "$cksum" |md5sum -c --quiet || error "file differs"

	rm -rf $DIR/$tfile
}
run_test 207b "can refresh layout at open"

test_212() {
	size=`date +%s`
	size=$((size % 8192 + 1))
	dd if=/dev/urandom of=$DIR/f212 bs=1k count=$size
	sendfile $DIR/f212 $DIR/f212.xyz || error "sendfile wrong"
	rm -f $DIR/f212 $DIR/f212.xyz
}
run_test 212 "Sendfile test ============================================"

test_213() {
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=4
	cancel_lru_locks osc
	lctl set_param fail_loc=0x8000040f
	# generate a read lock
	cat $DIR/$tfile > /dev/null
	# write to the file, it will try to cancel the above read lock.
	cat /etc/hosts >> $DIR/$tfile
}
run_test 213 "OSC lock completion and cancel race don't crash - bug 18829"

test_214() { # for bug 20133
	test_mkdir -p $DIR/d214p/d214c
	for (( i=0; i < 340; i++ )) ; do
		touch $DIR/d214p/d214c/a$i
	done

	ls -l $DIR/d214p || error "ls -l $DIR/d214p failed"
	mv $DIR/d214p/d214c $DIR/ || error "mv $DIR/d214p/d214c $DIR/ failed"
	ls $DIR/d214c || error "ls $DIR/d214c failed"
	rm -rf $DIR/d214* || error "rm -rf $DIR/d214* failed"
}
run_test 214 "hash-indexed directory test - bug 20133"

# having "abc" as 1st arg, creates $TMP/lnet_abc.out and $TMP/lnet_abc.sys
create_lnet_proc_files() {
	cat /proc/sys/lnet/$1 >$TMP/lnet_$1.out || error "cannot read /proc/sys/lnet/$1"
	sysctl lnet.$1 >$TMP/lnet_$1.sys_tmp || error "cannot read lnet.$1"

	sed "s/^lnet.$1\ =\ //g" "$TMP/lnet_$1.sys_tmp" >$TMP/lnet_$1.sys
	rm -f "$TMP/lnet_$1.sys_tmp"
}

# counterpart of create_lnet_proc_files
remove_lnet_proc_files() {
	rm -f $TMP/lnet_$1.out $TMP/lnet_$1.sys
}

# uses 1st arg as trailing part of filename, 2nd arg as description for reports,
# 3rd arg as regexp for body
check_lnet_proc_stats() {
	local l=$(cat "$TMP/lnet_$1" |wc -l)
	[ $l = 1 ] || (cat "$TMP/lnet_$1" && error "$2 is not of 1 line: $l")

	grep -E "$3" "$TMP/lnet_$1" || (cat "$TMP/lnet_$1" && error "$2 misformatted")
}

# uses 1st arg as trailing part of filename, 2nd arg as description for reports,
# 3rd arg as regexp for body, 4th arg as regexp for 1st line, 5th arg is
# optional and can be regexp for 2nd line (lnet.routes case)
check_lnet_proc_entry() {
	local blp=2            # blp stands for 'position of 1st line of body'
	[ "$5" = "" ] || blp=3 # lnet.routes case

	local l=$(cat "$TMP/lnet_$1" |wc -l)
	# subtracting one from $blp because the body can be empty
	[ "$l" -ge "$(($blp - 1))" ] || (cat "$TMP/lnet_$1" && error "$2 is too short: $l")

	sed -n '1 p' "$TMP/lnet_$1" |grep -E "$4" >/dev/null ||
		(cat "$TMP/lnet_$1" && error "1st line of $2 misformatted")

	[ "$5" = "" ] || sed -n '2 p' "$TMP/lnet_$1" |grep -E "$5" >/dev/null ||
		(cat "$TMP/lnet_$1" && error "2nd line of $2 misformatted")

	# bail out if any unexpected line happened
	sed -n "$blp~1 p" "$TMP/lnet_$1" |grep -Ev "$3"
	[ "$?" != 0 ] || error "$2 misformatted"
}

test_215() { # for bugs 18102, 21079, 21517
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local N='(0|[1-9][0-9]*)'       # non-negative numeric
	local P='[1-9][0-9]*'           # positive numeric
	local I='(0|-?[1-9][0-9]*|NA)'  # any numeric (0 | >0 | <0) or NA if no value
	local NET='[a-z][a-z0-9]*'      # LNET net like o2ib2
	local ADDR='[0-9.]+'            # LNET addr like 10.0.0.1
	local NID="$ADDR@$NET"          # LNET nid like 10.0.0.1@o2ib2

	local L1 # regexp for 1st line
	local L2 # regexp for 2nd line (optional)
	local BR # regexp for the rest (body)

	# /proc/sys/lnet/stats should look as 11 space-separated non-negative numerics
	BR="^$N $N $N $N $N $N $N $N $N $N $N$"
	create_lnet_proc_files "stats"
	check_lnet_proc_stats "stats.out" "/proc/sys/lnet/stats" "$BR"
	check_lnet_proc_stats "stats.sys" "lnet.stats" "$BR"
	remove_lnet_proc_files "stats"

	# /proc/sys/lnet/routes should look like this:
	# Routing disabled/enabled
	# net hops state router
	# where net is a string like tcp0, hops >= 0, state is up/down,
	# router is a string like 192.168.1.1@tcp2
	L1="^Routing (disabled|enabled)$"
	L2="^net +hops +state +router$"
	BR="^$NET +$N +(up|down) +$NID$"
	create_lnet_proc_files "routes"
	check_lnet_proc_entry "routes.out" "/proc/sys/lnet/routes" "$BR" "$L1" "$L2"
	check_lnet_proc_entry "routes.sys" "lnet.routes" "$BR" "$L1" "$L2"
	remove_lnet_proc_files "routes"

	# /proc/sys/lnet/routers should look like this:
	# ref rtr_ref alive_cnt state last_ping ping_sent deadline down_ni router
	# where ref > 0, rtr_ref > 0, alive_cnt >= 0, state is up/down,
	# last_ping >= 0, ping_sent is boolean (0/1), deadline and down_ni are
	# numeric (0 or >0 or <0), router is a string like 192.168.1.1@tcp2
	L1="^ref +rtr_ref +alive_cnt +state +last_ping +ping_sent +deadline +down_ni +router$"
	BR="^$P +$P +$N +(up|down) +$N +(0|1) +$I +$I +$NID$"
	create_lnet_proc_files "routers"
	check_lnet_proc_entry "routers.out" "/proc/sys/lnet/routers" "$BR" "$L1"
	check_lnet_proc_entry "routers.sys" "lnet.routers" "$BR" "$L1"
	remove_lnet_proc_files "routers"

	# /proc/sys/lnet/peers should look like this:
	# nid refs state last max rtr min tx min queue
	# where nid is a string like 192.168.1.1@tcp2, refs > 0,
	# state is up/down/NA, max >= 0. last, rtr, min, tx, min are
	# numeric (0 or >0 or <0), queue >= 0.
	L1="^nid +refs +state +last +max +rtr +min +tx +min +queue$"
	BR="^$NID +$P +(up|down|NA) +$I +$N +$I +$I +$I +$I +$N$"
	create_lnet_proc_files "peers"
	check_lnet_proc_entry "peers.out" "/proc/sys/lnet/peers" "$BR" "$L1"
	check_lnet_proc_entry "peers.sys" "lnet.peers" "$BR" "$L1"
	remove_lnet_proc_files "peers"

	# /proc/sys/lnet/buffers  should look like this:
	# pages count credits min
	# where pages >=0, count >=0, credits and min are numeric (0 or >0 or <0)
	L1="^pages +count +credits +min$"
	BR="^ +$N +$N +$I +$I$"
	create_lnet_proc_files "buffers"
	check_lnet_proc_entry "buffers.out" "/proc/sys/lnet/buffers" "$BR" "$L1"
	check_lnet_proc_entry "buffers.sys" "lnet.buffers" "$BR" "$L1"
	remove_lnet_proc_files "buffers"

	# /proc/sys/lnet/nis should look like this:
	# nid status alive refs peer rtr max tx min
	# where nid is a string like 192.168.1.1@tcp2, status is up/down,
	# alive is numeric (0 or >0 or <0), refs >= 0, peer >= 0,
	# rtr >= 0, max >=0, tx and min are numeric (0 or >0 or <0).
	L1="^nid +status +alive +refs +peer +rtr +max +tx +min$"
	BR="^$NID +(up|down) +$I +$N +$N +$N +$N +$I +$I$"
	create_lnet_proc_files "nis"
	check_lnet_proc_entry "nis.out" "/proc/sys/lnet/nis" "$BR" "$L1"
	check_lnet_proc_entry "nis.sys" "lnet.nis" "$BR" "$L1"
	remove_lnet_proc_files "nis"

	# can we successfully write to /proc/sys/lnet/stats?
	echo "0" >/proc/sys/lnet/stats || error "cannot write to /proc/sys/lnet/stats"
	sysctl -w lnet.stats=0 || error "cannot write to lnet.stats"
}
run_test 215 "/proc/sys/lnet exists and has proper content - bugs 18102, 21079, 21517"

test_216() { # bug 20317
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        remote_ost_nodsh && skip "remote OST with nodsh" && return
        local node
        local p="$TMP/sanityN-$TESTNAME.parameters"
        save_lustre_params $HOSTNAME "osc.*.contention_seconds" > $p
        for node in $(osts_nodes); do
                save_lustre_params $node "ldlm.namespaces.filter-*.max_nolock_bytes" >> $p
                save_lustre_params $node "ldlm.namespaces.filter-*.contended_locks" >> $p
                save_lustre_params $node "ldlm.namespaces.filter-*.contention_seconds" >> $p
        done
        clear_osc_stats

        # agressive lockless i/o settings
        for node in $(osts_nodes); do
                do_node $node 'lctl set_param -n ldlm.namespaces.filter-*.max_nolock_bytes 2000000; lctl set_param -n ldlm.namespaces.filter-*.contended_locks 0; lctl set_param -n ldlm.namespaces.filter-*.contention_seconds 60'
        done
        lctl set_param -n osc.*.contention_seconds 60

        $DIRECTIO write $DIR/$tfile 0 10 4096
        $CHECKSTAT -s 40960 $DIR/$tfile

        # disable lockless i/o
        for node in $(osts_nodes); do
                do_node $node 'lctl set_param -n ldlm.namespaces.filter-*.max_nolock_bytes 0; lctl set_param -n ldlm.namespaces.filter-*.contended_locks 32; lctl set_param -n ldlm.namespaces.filter-*.contention_seconds 0'
        done
        lctl set_param -n osc.*.contention_seconds 0
        clear_osc_stats

        dd if=/dev/zero of=$DIR/$tfile count=0
        $CHECKSTAT -s 0 $DIR/$tfile

        restore_lustre_params <$p
        rm -f $p
        rm $DIR/$tfile
}
run_test 216 "check lockless direct write works and updates file size and kms correctly"

test_217() { # bug 22430
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	local node
	local nid

	for node in $(nodes_list); do
		nid=$(host_nids_address $node $NETTYPE)
		if [[ $nid = *-* ]] ; then
			echo "lctl ping $nid@$NETTYPE"
			lctl ping $nid@$NETTYPE
		else
			echo "skipping $node (no hyphen detected)"
		fi
	done
}
run_test 217 "check lctl ping for hostnames with hiphen ('-')"

test_218() {
       # do directio so as not to populate the page cache
       log "creating a 10 Mb file"
       $MULTIOP $DIR/$tfile oO_CREAT:O_DIRECT:O_RDWR:w$((10*1048576))c || error "multiop failed while creating a file"
       log "starting reads"
       dd if=$DIR/$tfile of=/dev/null bs=4096 &
       log "truncating the file"
       $MULTIOP $DIR/$tfile oO_TRUNC:c || error "multiop failed while truncating the file"
       log "killing dd"
       kill %+ || true # reads might have finished
       echo "wait until dd is finished"
       wait
       log "removing the temporary file"
       rm -rf $DIR/$tfile || error "tmp file removal failed"
}
run_test 218 "parallel read and truncate should not deadlock ======================="

test_219() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        # write one partial page
        dd if=/dev/zero of=$DIR/$tfile bs=1024 count=1
        # set no grant so vvp_io_commit_write will do sync write
        $LCTL set_param fail_loc=0x411
        # write a full page at the end of file
        dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1 seek=1 conv=notrunc

        $LCTL set_param fail_loc=0
        dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1 seek=3
        $LCTL set_param fail_loc=0x411
        dd if=/dev/zero of=$DIR/$tfile bs=1024 count=1 seek=2 conv=notrunc
}
run_test 219 "LU-394: Write partial won't cause uncontiguous pages vec at LND"

test_220() { #LU-325
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	remote_ost_nodsh && skip "remote OST with nodsh" && return
	local OSTIDX=0

	test_mkdir -p $DIR/$tdir
	local OST=$(lfs osts | grep ${OSTIDX}": " | \
		awk '{print $2}' | sed -e 's/_UUID$//')

	# on the mdt's osc
	local mdtosc_proc1=$(get_mdtosc_proc_path $SINGLEMDS $OST)
	local last_id=$(do_facet $SINGLEMDS lctl get_param -n \
			osc.$mdtosc_proc1.prealloc_last_id)
	local next_id=$(do_facet $SINGLEMDS lctl get_param -n \
			osc.$mdtosc_proc1.prealloc_next_id)

	$LFS df -i

	do_facet ost$((OSTIDX + 1)) lctl set_param fail_val=-1
	#define OBD_FAIL_OST_ENOINO              0x229
	do_facet ost$((OSTIDX + 1)) lctl set_param fail_loc=0x229
	do_facet mgs $LCTL pool_new $FSNAME.$TESTNAME || return 1
	do_facet mgs $LCTL pool_add $FSNAME.$TESTNAME $OST || return 2

	$SETSTRIPE $DIR/$tdir -i $OSTIDX -c 1 -p $FSNAME.$TESTNAME

	MDSOBJS=$((last_id - next_id))
	echo "preallocated objects on MDS is $MDSOBJS" "($last_id - $next_id)"

	blocks=$($LFS df $MOUNT | awk '($1 == '$OSTIDX') { print $4 }')
	echo "OST still has $count kbytes free"

	echo "create $MDSOBJS files @next_id..."
	createmany -o $DIR/$tdir/f $MDSOBJS || return 3

	local last_id2=$(do_facet mds${MDSIDX} lctl get_param -n \
			osc.$mdtosc_proc1.prealloc_last_id)
	local next_id2=$(do_facet mds${MDSIDX} lctl get_param -n \
			osc.$mdtosc_proc1.prealloc_next_id)

	echo "after creation, last_id=$last_id2, next_id=$next_id2"
	$LFS df -i

	echo "cleanup..."

	do_facet ost$((OSTIDX + 1)) lctl set_param fail_val=0
	do_facet ost$((OSTIDX + 1)) lctl set_param fail_loc=0

	do_facet mgs $LCTL pool_remove $FSNAME.$TESTNAME $OST || return 4
	do_facet mgs $LCTL pool_destroy $FSNAME.$TESTNAME || return 5
	echo "unlink $MDSOBJS files @$next_id..."
	unlinkmany $DIR/$tdir/f $MDSOBJS || return 6
}
run_test 220 "preallocated MDS objects still used if ENOSPC from OST"

test_221() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        dd if=`which date` of=$MOUNT/date oflag=sync
        chmod +x $MOUNT/date

        #define OBD_FAIL_LLITE_FAULT_TRUNC_RACE  0x1401
        $LCTL set_param fail_loc=0x80001401

        $MOUNT/date > /dev/null
        rm -f $MOUNT/date
}
run_test 221 "make sure fault and truncate race to not cause OOM"

test_222a () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
       rm -rf $DIR/$tdir
       test_mkdir -p $DIR/$tdir
       $SETSTRIPE -c 1 -i 0 $DIR/$tdir
       createmany -o $DIR/$tdir/$tfile 10
       cancel_lru_locks mdc
       cancel_lru_locks osc
       #define OBD_FAIL_LDLM_AGL_DELAY           0x31a
       $LCTL set_param fail_loc=0x31a
       ls -l $DIR/$tdir > /dev/null || error "AGL for ls failed"
       $LCTL set_param fail_loc=0
       rm -r $DIR/$tdir
}
run_test 222a "AGL for ls should not trigger CLIO lock failure ================"

test_222b () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
       rm -rf $DIR/$tdir
       test_mkdir -p $DIR/$tdir
       $SETSTRIPE -c 1 -i 0 $DIR/$tdir
       createmany -o $DIR/$tdir/$tfile 10
       cancel_lru_locks mdc
       cancel_lru_locks osc
       #define OBD_FAIL_LDLM_AGL_DELAY           0x31a
       $LCTL set_param fail_loc=0x31a
       rm -r $DIR/$tdir || "AGL for rmdir failed"
       $LCTL set_param fail_loc=0
}
run_test 222b "AGL for rmdir should not trigger CLIO lock failure ============="

test_223 () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
       rm -rf $DIR/$tdir
       test_mkdir -p $DIR/$tdir
       $SETSTRIPE -c 1 -i 0 $DIR/$tdir
       createmany -o $DIR/$tdir/$tfile 10
       cancel_lru_locks mdc
       cancel_lru_locks osc
       #define OBD_FAIL_LDLM_AGL_NOLOCK          0x31b
       $LCTL set_param fail_loc=0x31b
       ls -l $DIR/$tdir > /dev/null || error "reenqueue failed"
       $LCTL set_param fail_loc=0
       rm -r $DIR/$tdir
}
run_test 223 "osc reenqueue if without AGL lock granted ======================="

test_224a() { # LU-1039, MRP-303
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        #define OBD_FAIL_PTLRPC_CLIENT_BULK_CB   0x508
        $LCTL set_param fail_loc=0x508
        dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1 conv=fsync
        $LCTL set_param fail_loc=0
        df $DIR
}
run_test 224a "Don't panic on bulk IO failure"

test_224b() { # LU-1039, MRP-303
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1
        cancel_lru_locks osc
        #define OBD_FAIL_PTLRPC_CLIENT_BULK_CB2   0x515
        $LCTL set_param fail_loc=0x515
        dd of=/dev/null if=$DIR/$tfile bs=4096 count=1
        $LCTL set_param fail_loc=0
        df $DIR
}
run_test 224b "Don't panic on bulk IO failure"

MDSSURVEY=${MDSSURVEY:-$(which mds-survey 2>/dev/null || true)}
test_225a () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	if [ -z ${MDSSURVEY} ]; then
	      skip_env "mds-survey not found" && return
	fi

	[ $MDSCOUNT -ge 2 ] &&
		skip "skipping now for more than one MDT" && return

       [ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.2.51) ] ||
            { skip "Need MDS version at least 2.2.51"; return; }

       local mds=$(facet_host $SINGLEMDS)
       local target=$(do_nodes $mds 'lctl dl' | \
                      awk "{if (\$2 == \"UP\" && \$3 == \"mdt\") {print \$4}}")

       local cmd1="file_count=1000 thrhi=4"
       local cmd2="dir_count=2 layer=mdd stripe_count=0"
       local cmd3="rslt_loc=${TMP} targets=\"$mds:$target\" $MDSSURVEY"
       local cmd="$cmd1 $cmd2 $cmd3"

       rm -f ${TMP}/mds_survey*
       echo + $cmd
       eval $cmd || error "mds-survey with zero-stripe failed"
       cat ${TMP}/mds_survey*
       rm -f ${TMP}/mds_survey*
}
run_test 225a "Metadata survey sanity with zero-stripe"

test_225b () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return

	if [ -z ${MDSSURVEY} ]; then
	      skip_env "mds-survey not found" && return
	fi
	[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.2.51) ] ||
	    { skip "Need MDS version at least 2.2.51"; return; }

	if [ $($LCTL dl | grep -c osc) -eq 0 ]; then
	      skip_env "Need to mount OST to test" && return
	fi

	[ $MDSCOUNT -ge 2 ] &&
		skip "skipping now for more than one MDT" && return
       local mds=$(facet_host $SINGLEMDS)
       local target=$(do_nodes $mds 'lctl dl' | \
                      awk "{if (\$2 == \"UP\" && \$3 == \"mdt\") {print \$4}}")

       local cmd1="file_count=1000 thrhi=4"
       local cmd2="dir_count=2 layer=mdd stripe_count=1"
       local cmd3="rslt_loc=${TMP} targets=\"$mds:$target\" $MDSSURVEY"
       local cmd="$cmd1 $cmd2 $cmd3"

       rm -f ${TMP}/mds_survey*
       echo + $cmd
       eval $cmd || error "mds-survey with stripe_count failed"
       cat ${TMP}/mds_survey*
       rm -f ${TMP}/mds_survey*
}
run_test 225b "Metadata survey sanity with stripe_count = 1"

mcreate_path2fid () {
	local mode=$1
	local major=$2
	local minor=$3
	local name=$4
	local desc=$5
	local path=$DIR/$tdir/$name
	local fid
	local rc
	local fid_path

	$MCREATE --mode=$1 --major=$2 --minor=$3 $path ||
		error "cannot create $desc"

	fid=$($LFS path2fid $path | tr -d '[' | tr -d ']')
	rc=$?
	[ $rc -ne 0 ] && error "cannot get fid of a $desc"

	fid_path=$($LFS fid2path $MOUNT $fid)
	rc=$?
	[ $rc -ne 0 ] && error "cannot get path of $desc by $DIR $path $fid"

	[ "$path" == "$fid_path" ] ||
		error "fid2path returned $fid_path, expected $path"

	echo "pass with $path and $fid"
}

test_226a () {
	rm -rf $DIR/$tdir
	mkdir -p $DIR/$tdir

	mcreate_path2fid 0010666 0 0 fifo "FIFO"
	mcreate_path2fid 0020666 1 3 null "character special file (null)"
	mcreate_path2fid 0020666 1 255 none "character special file (no device)"
	mcreate_path2fid 0040666 0 0 dir "directory"
	mcreate_path2fid 0060666 7 0 loop0 "block special file (loop)"
	mcreate_path2fid 0100666 0 0 file "regular file"
	mcreate_path2fid 0120666 0 0 link "symbolic link"
	mcreate_path2fid 0140666 0 0 sock "socket"
}
run_test 226a "call path2fid and fid2path on files of all type"

test_226b () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return
	rm -rf $DIR/$tdir
	local MDTIDX=1

	mkdir -p $DIR/$tdir
	$LFS setdirstripe -i $MDTIDX $DIR/$tdir/remote_dir ||
		error "create remote directory failed"
	mcreate_path2fid 0010666 0 0 "remote_dir/fifo" "FIFO"
	mcreate_path2fid 0020666 1 3 "remote_dir/null" \
				"character special file (null)"
	mcreate_path2fid 0020666 1 255 "remote_dir/none" \
				"character special file (no device)"
	mcreate_path2fid 0040666 0 0 "remote_dir/dir" "directory"
	mcreate_path2fid 0060666 7 0 "remote_dir/loop0" \
				"block special file (loop)"
	mcreate_path2fid 0100666 0 0 "remote_dir/file" "regular file"
	mcreate_path2fid 0120666 0 0 "remote_dir/link" "symbolic link"
	mcreate_path2fid 0140666 0 0 "remote_dir/sock" "socket"
}
run_test 226b "call path2fid and fid2path on files of all type under remote dir"

# LU-1299 Executing or running ldd on a truncated executable does not
# cause an out-of-memory condition.
test_227() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	dd if=`which date` of=$MOUNT/date bs=1k count=1
	chmod +x $MOUNT/date

	$MOUNT/date > /dev/null
	ldd $MOUNT/date > /dev/null
	rm -f $MOUNT/date
}
run_test 227 "running truncated executable does not cause OOM"

# LU-1512 try to reuse idle OI blocks
test_228a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$(facet_fstype $SINGLEMDS)" != "ldiskfs" ] &&
		skip "non-ldiskfs backend" && return

	local MDT_DEV=$(mdsdevname ${SINGLEMDS//mds/})
	local myDIR=$DIR/$tdir

	mkdir -p $myDIR
	#define OBD_FAIL_SEQ_EXHAUST             0x1002
	$LCTL set_param fail_loc=0x80001002
	createmany -o $myDIR/t- 10000
	$LCTL set_param fail_loc=0
	# The guard is current the largest FID holder
	touch $myDIR/guard
	local SEQ=$($LFS path2fid $myDIR/guard | awk -F ':' '{print $1}' |
		    tr -d '[')
	local IDX=$(($SEQ % 64))

	do_facet $SINGLEMDS sync
	# Make sure journal flushed.
	sleep 6
	local blk1=$(do_facet $SINGLEMDS \
		     "$DEBUGFS -c -R \\\"stat oi.16.${IDX}\\\" $MDT_DEV" |
		     grep Blockcount | awk '{print $4}')

	# Remove old files, some OI blocks will become idle.
	unlinkmany $myDIR/t- 10000
	# Create new files, idle OI blocks should be reused.
	createmany -o $myDIR/t- 2000
	do_facet $SINGLEMDS sync
	# Make sure journal flushed.
	sleep 6
	local blk2=$(do_facet $SINGLEMDS \
		     "$DEBUGFS -c -R \\\"stat oi.16.${IDX}\\\" $MDT_DEV" |
		     grep Blockcount | awk '{print $4}')

	[ $blk1 == $blk2 ] || error "old blk1=$blk1, new blk2=$blk2, unmatched!"
}
run_test 228a "try to reuse idle OI blocks"

test_228b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$(facet_fstype $SINGLEMDS)" != "ldiskfs" ] &&
		skip "non-ldiskfs backend" && return

	local MDT_DEV=$(mdsdevname ${SINGLEMDS//mds/})
	local myDIR=$DIR/$tdir

	mkdir -p $myDIR
	#define OBD_FAIL_SEQ_EXHAUST             0x1002
	$LCTL set_param fail_loc=0x80001002
	createmany -o $myDIR/t- 10000
	$LCTL set_param fail_loc=0
	# The guard is current the largest FID holder
	touch $myDIR/guard
	local SEQ=$($LFS path2fid $myDIR/guard | awk -F ':' '{print $1}' |
		    tr -d '[')
	local IDX=$(($SEQ % 64))

	do_facet $SINGLEMDS sync
	# Make sure journal flushed.
	sleep 6
	local blk1=$(do_facet $SINGLEMDS \
		     "$DEBUGFS -c -R \\\"stat oi.16.${IDX}\\\" $MDT_DEV" |
		     grep Blockcount | awk '{print $4}')

	# Remove old files, some OI blocks will become idle.
	unlinkmany $myDIR/t- 10000

	# stop the MDT
	stop $SINGLEMDS || error "Fail to stop MDT."
	# remount the MDT
	start $SINGLEMDS $MDT_DEV $MDS_MOUNT_OPTS || error "Fail to start MDT."

	df $MOUNT || error "Fail to df."
	# Create new files, idle OI blocks should be reused.
	createmany -o $myDIR/t- 2000
	do_facet $SINGLEMDS sync
	# Make sure journal flushed.
	sleep 6
	local blk2=$(do_facet $SINGLEMDS \
		     "$DEBUGFS -c -R \\\"stat oi.16.${IDX}\\\" $MDT_DEV" |
		     grep Blockcount | awk '{print $4}')

	[ $blk1 == $blk2 ] || error "old blk1=$blk1, new blk2=$blk2, unmatched!"
}
run_test 228b "idle OI blocks can be reused after MDT restart"

#LU-1881
test_228c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ "$(facet_fstype $SINGLEMDS)" != "ldiskfs" ] &&
		skip "non-ldiskfs backend" && return

	local MDT_DEV=$(mdsdevname ${SINGLEMDS//mds/})
	local myDIR=$DIR/$tdir

	mkdir -p $myDIR
	#define OBD_FAIL_SEQ_EXHAUST             0x1002
	$LCTL set_param fail_loc=0x80001002
	# 20000 files can guarantee there are index nodes in the OI file
	createmany -o $myDIR/t- 20000
	$LCTL set_param fail_loc=0
	# The guard is current the largest FID holder
	touch $myDIR/guard
	local SEQ=$($LFS path2fid $myDIR/guard | awk -F ':' '{print $1}' |
		    tr -d '[')
	local IDX=$(($SEQ % 64))

	do_facet $SINGLEMDS sync
	# Make sure journal flushed.
	sleep 6
	local blk1=$(do_facet $SINGLEMDS \
		     "$DEBUGFS -c -R \\\"stat oi.16.${IDX}\\\" $MDT_DEV" |
		     grep Blockcount | awk '{print $4}')

	# Remove old files, some OI blocks will become idle.
	unlinkmany $myDIR/t- 20000
	rm -f $myDIR/guard
	# The OI file should become empty now

	# Create new files, idle OI blocks should be reused.
	createmany -o $myDIR/t- 2000
	do_facet $SINGLEMDS sync
	# Make sure journal flushed.
	sleep 6
	local blk2=$(do_facet $SINGLEMDS \
		     "$DEBUGFS -c -R \\\"stat oi.16.${IDX}\\\" $MDT_DEV" |
		     grep Blockcount | awk '{print $4}')

	[ $blk1 == $blk2 ] || error "old blk1=$blk1, new blk2=$blk2, unmatched!"
}
run_test 228c "NOT shrink the last entry in OI index node to recycle idle leaf"

test_230a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return
	local MDTIDX=1

	mkdir -p $DIR/$tdir/test_230_local
	local mdt_idx=$($GETSTRIPE -M $DIR/$tdir/test_230_local)
	[ $mdt_idx -ne 0 ] &&
		error "create local directory on wrong MDT $mdt_idx"

	$LFS mkdir -i $MDTIDX $DIR/$tdir/test_230 ||
			error "create remote directory failed"
	local mdt_idx=$($GETSTRIPE -M $DIR/$tdir/test_230)
	[ $mdt_idx -ne $MDTIDX ] &&
		error "create remote directory on wrong MDT $mdt_idx"

	createmany -o $DIR/$tdir/test_230/t- 10 ||
		error "create files on remote directory failed"
	mdt_idx=$($GETSTRIPE -M $DIR/$tdir/test_230/t-0)
	[ $mdt_idx -ne $MDTIDX ] && error "create files on wrong MDT $mdt_idx"
	rm -r $DIR/$tdir || error "unlink remote directory failed"
}
run_test 230a "Create remote directory and files under the remote directory"

test_230b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return
	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir
	local rc=0

	mkdir -p $DIR/$tdir
	$LFS mkdir -i $MDTIDX $remote_dir ||
		error "create remote directory failed"

	$LFS mkdir -i 0 $remote_dir/new_dir &&
		error "nested remote directory create succeed!"

	do_facet mds$((MDTIDX + 1)) lctl set_param mdt.*.enable_remote_dir=1
	$LFS mkdir -i 0 $remote_dir/new_dir || rc=$?
	do_facet mds$((MDTIDX + 1)) lctl set_param mdt.*.enable_remote_dir=0

	[ $rc -ne 0 ] &&
	   error "create remote directory failed after set enable_remote_dir"

	rm -rf $remote_dir || error "first unlink remote directory failed"

	$RUNAS -G$RUNAS_GID $LFS mkdir -i $MDTIDX $DIR/$tfile &&
							error "chown worked"

	do_facet mds$MDTIDX lctl set_param \
				mdt.*.enable_remote_dir_gid=$RUNAS_GID
	$LFS mkdir -i $MDTIDX $remote_dir || rc=$?
	do_facet mds$MDTIDX lctl set_param mdt.*.enable_remote_dir_gid=0

	[ $rc -ne 0 ] &&
	   error "create remote dir failed after set enable_remote_dir_gid"

	rm -r $DIR/$tdir || error "second unlink remote directory failed"
}
run_test 230b "nested remote directory should be failed"

test_231a()
{
	# For simplicity this test assumes that max_pages_per_rpc
	# is the same across all OSCs
	local max_pages=$($LCTL get_param -n osc.*.max_pages_per_rpc | head -1)
	local bulk_size=$((max_pages * 4096))

	mkdir -p $DIR/$tdir

	# clear the OSC stats
	$LCTL set_param osc.*.stats=0 &>/dev/null

	# Client writes $bulk_size - there must be 1 rpc for $max_pages.
	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=$bulk_size count=1 \
		oflag=direct &>/dev/null || error "dd failed"

	local nrpcs=$($LCTL get_param osc.*.stats |awk '/ost_write/ {print $2}')
	if [ x$nrpcs != "x1" ]; then
		error "found $nrpc ost_write RPCs, not 1 as expected"
	fi

	# Drop the OSC cache, otherwise we will read from it
	cancel_lru_locks osc

	# clear the OSC stats
	$LCTL set_param osc.*.stats=0 &>/dev/null

	# Client reads $bulk_size.
	dd if=$DIR/$tdir/$tfile of=/dev/null bs=$bulk_size count=1 \
		iflag=direct &>/dev/null || error "dd failed"

	nrpcs=$($LCTL get_param osc.*.stats | awk '/ost_read/ { print $2 }')
	if [ x$nrpcs != "x1" ]; then
		error "found $nrpc ost_read RPCs, not 1 as expected"
	fi
}
run_test 231a "checking that reading/writing of BRW RPC size results in one RPC"

test_231b() {
	mkdir -p $DIR/$tdir
	local i
	for i in {0..1023}; do
		dd if=/dev/zero of=$DIR/$tdir/$tfile conv=notrunc \
			seek=$((2 * i)) bs=4096 count=1 &>/dev/null ||
			error "dd of=$DIR/$tdir/$tfile seek=$((2 * i)) failed"
	done
	sync
}
run_test 231b "must not assert on fully utilized OST request buffer"

test_232() {
	mkdir -p $DIR/$tdir
	#define OBD_FAIL_LDLM_OST_LVB		 0x31c
	$LCTL set_param fail_loc=0x31c

	# ignore dd failure
	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=1M count=1 || true

	$LCTL set_param fail_loc=0
	umount_client $MOUNT || error "umount failed"
	mount_client $MOUNT || error "mount failed"
}
run_test 232 "failed lock should not block umount"

#
# tests that do cleanup/setup should be run at the end
#

test_900() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run" && return
        local ls
        #define OBD_FAIL_MGC_PAUSE_PROCESS_LOG   0x903
        $LCTL set_param fail_loc=0x903
        # cancel_lru_locks mgc - does not work due to lctl set_param syntax
        for ls in /proc/fs/lustre/ldlm/namespaces/MGC*/lru_size; do
                echo "clear" > $ls
        done
	FAIL_ON_ERROR=true cleanup
	FAIL_ON_ERROR=true setup
}
run_test 900 "umount should not race with any mgc requeue thread"

complete $SECONDS
check_and_cleanup_lustre
if [ "$I_MOUNTED" != "yes" ]; then
	lctl set_param debug="$OLDDEBUG" 2> /dev/null || true
fi
exit_status
