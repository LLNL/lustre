#!/bin/bash
# -*- tab-width: 8; indent-tabs-mode: t; -*-
#
# Run select tests by setting ONLY, or as arguments to the script.
# Skip specific tests by setting EXCEPT.
#
# e.g. ONLY="22 23" or ONLY="`seq 32 39`" or EXCEPT="31"
set -e

ONLY=${ONLY:-"$*"}
# bug number for skipped test: LU-9693 LU-6493 LU-9693 LU-11058
ALWAYS_EXCEPT="$SANITY_EXCEPT  42a     42b     42c     77k"
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

# skipped tests: LU-8411 LU-9096 LU-9054 ..
ALWAYS_EXCEPT="  407     253     312     $ALWAYS_EXCEPT"

if $SHARED_KEY; then
# bug number for skipped tests:	LU-9795 (all below)
	ALWAYS_EXCEPT="$ALWAYS_EXCEPT	17n	60a	133g	300f"
fi

if [[ $(uname -m) = aarch64 ]]; then
	# bug number:	 LU-11596 (all below)
	ALWAYS_EXCEPT+=" 42d 42e 63a 63b 64a 64b 64c"
	# bug number:	 LU-11671 LU-11665 LU-11594 LU-11667 LU-11729
	ALWAYS_EXCEPT+=" 45	  101c	   103a	    317      810"
fi

# Check Grants after these tests
GRANT_CHECK_LIST="$GRANT_CHECK_LIST 42a 42b 42c 42d 42e 63a 63b 64a 64b 64c"
SRCDIR=$(cd $(dirname $0); echo $PWD)
export PATH=$PATH:/sbin

TMP=${TMP:-/tmp}
OSC=${OSC:-"osc"}

CC=${CC:-cc}
CHECKSTAT=${CHECKSTAT:-"checkstat -v"}
CREATETEST=${CREATETEST:-createtest}
LFS=${LFS:-lfs}
LVERIFY=${LVERIFY:-ll_dirstripe_verify}
LCTL=${LCTL:-lctl}
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
DEF_STRIPE_COUNT=-1
CHECK_GRANT=${CHECK_GRANT:-"yes"}
GRANT_CHECK_LIST=${GRANT_CHECK_LIST:-""}
export PARALLEL=${PARALLEL:-"no"}

export NAME=${NAME:-local}

SAVE_PWD=$PWD

CLEANUP=${CLEANUP:-:}
SETUP=${SETUP:-:}
TRACE=${TRACE:-""}
LUSTRE=${LUSTRE:-$(cd $(dirname $0)/..; echo $PWD)}
LUSTRE_TESTS_API_DIR=${LUSTRE_TESTS_API_DIR:-${LUSTRE}/tests/clientapi}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/${NAME}.sh}
get_lustre_env
init_logging

# skip nfs tests on kernels >= 4.14.0 until they are fixed
if [ $LINUX_VERSION_CODE -ge $(version_code 4.14.0) ]; then
	# bug number:	LU-12661
	ALWAYS_EXCEPT+=" 817"
fi

# skip cgroup tests on RHEL8.1 kernels until they are fixed
if (( $LINUX_VERSION_CODE >= $(version_code 4.18.0) &&
      $LINUX_VERSION_CODE <  $(version_code 5.4.0) )); then
	# bug number:	LU-13063
	ALWAYS_EXCEPT+=" 411"
fi

#                                  5          12          (min)"
[ "$SLOW" = "no" ] && EXCEPT_SLOW="27m 64b 68 71 115 300o"

if [ "$mds1_FSTYPE" = "zfs" ]; then
	# bug number for skipped test:
	ALWAYS_EXCEPT="$ALWAYS_EXCEPT  "
	#                                               13    (min)"
	[ "$SLOW" = "no" ] && EXCEPT_SLOW="$EXCEPT_SLOW 51b"
fi

# Get the SLES distro version
#
# Returns a version string that should only be used in comparing
# strings returned by version_code()
sles_version_code()
{
	local version=$(grep VERSION_ID /etc/os-release | cut -d'"' -f2)

	# All SuSE Linux versions have one decimal. version_code expects two
	local sles_version=$version.0
	version_code $sles_version
}

# Check if we are running on Ubuntu or SLES so we can make decisions on
# what tests to run
if [ -r /etc/SuSE-release ]; then
	sles_version=$(sles_version_code)
	[ $sles_version -lt $(version_code 11.4.0) ] &&
		# bug number for skipped test: LU-4341
		ALWAYS_EXCEPT="$ALWAYS_EXCEPT  170"
	[ $sles_version -lt $(version_code 12.0.0) ] &&
		# bug number for skipped test: LU-3703
		ALWAYS_EXCEPT="$ALWAYS_EXCEPT  234"
elif [ -r /etc/os-release ]; then
	if grep -qi ubuntu /etc/os-release; then
		ubuntu_version=$(version_code $(sed -n -e 's/"//g' \
						-e 's/^VERSION=//p' \
						/etc/os-release |
						awk '{ print $1 }'))

		if [[ $ubuntu_version -gt $(version_code 16.0.0) ]]; then
			# bug number for skipped test:
			#                LU-10334 LU-10366
			ALWAYS_EXCEPT+=" 103a	  410"
		fi
	fi
fi

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

check_swap_layouts_support()
{
	$LCTL get_param -n llite.*.sbi_flags | grep -q layout ||
		skip "Does not support layout lock."
}

check_and_setup_lustre
DIR=${DIR:-$MOUNT}
assert_DIR

MAXFREE=${MAXFREE:-$((200000 * $OSTCOUNT))}

[ -f $DIR/d52a/foo ] && chattr -a $DIR/d52a/foo
[ -f $DIR/d52b/foo ] && chattr -i $DIR/d52b/foo
rm -rf $DIR/[Rdfs][0-9]*

# $RUNAS_ID may get set incorrectly somewhere else
[ $UID -eq 0 -a $RUNAS_ID -eq 0 ] &&
	error "\$RUNAS_ID set to 0, but \$UID is also 0!"

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

OLDDEBUG=$(lctl get_param -n debug 2> /dev/null)
lctl set_param debug=-1 2> /dev/null || true
test_0a() {
	touch $DIR/$tfile
	$CHECKSTAT -t file $DIR/$tfile || error "$tfile is not a file"
	rm $DIR/$tfile
	$CHECKSTAT -a $DIR/$tfile || error "$tfile was not removed"
}
run_test 0a "touch; rm ====================="

test_0b() {
	chmod 0755 $DIR || error "chmod 0755 $DIR failed"
	$CHECKSTAT -p 0755 $DIR || error "$DIR permission is not 0755"
}
run_test 0b "chmod 0755 $DIR ============================="

test_0c() {
	$LCTL get_param mdc.*.import | grep "state: FULL" ||
		error "import not FULL"
	$LCTL get_param mdc.*.import | grep "target: $FSNAME-MDT" ||
		error "bad target"
}
run_test 0c "check import proc"

test_0d() { # LU-3397
	[ $MGS_VERSION -lt $(version_code 2.10.57) ] &&
		skip "proc exports not supported before 2.10.57"

	local mgs_exp="mgs.MGS.exports"
	local client_uuid=$($LCTL get_param -n mgc.*.uuid)
	local exp_client_nid
	local exp_client_version
	local exp_val
	local imp_val
	local temp_imp=$DIR/$tfile.import
	local temp_exp=$DIR/$tfile.export

	# save mgc import file to $temp_imp
	$LCTL get_param mgc.*.import | tee $temp_imp
	# Check if client uuid is found in MGS export
	for exp_client_nid in $(do_facet mgs $LCTL get_param -N $mgs_exp.*); do
		[ $(do_facet mgs $LCTL get_param -n $exp_client_nid.uuid) == \
			$client_uuid ] &&
			break;
	done
	# save mgs export file to $temp_exp
	do_facet mgs $LCTL get_param $exp_client_nid.export | tee $temp_exp

	# Compare the value of field "connect_flags"
	imp_val=$(grep "connect_flags" $temp_imp)
	exp_val=$(grep "connect_flags" $temp_exp)
	[ "$exp_val" == "$imp_val" ] ||
		error "export flags '$exp_val' != import flags '$imp_val'"

	# Compare the value of client version
	exp_client_version=$(awk '/target_version:/ { print $2 }' $temp_exp)
	exp_val=$(version_code $exp_client_version)
	imp_val=$CLIENT_VERSION
	[ "$exp_val" == "$imp_val" ] ||
		error "export client version '$exp_val' != '$imp_val'"
}
run_test 0d "check export proc ============================="

test_1() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	mkdir $DIR/$tdir/d2 && error "we expect EEXIST, but not returned"
	$CHECKSTAT -t dir $DIR/$tdir/d2 || error "$tdir/d2 is not a dir"
	rmdir $DIR/$tdir/d2
	rmdir $DIR/$tdir
	$CHECKSTAT -a $DIR/$tdir || error "$tdir was not removed"
}
run_test 1 "mkdir; remkdir; rmdir"

test_2() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile || error "touch $tdir/$tfile failed"
	$CHECKSTAT -t file $DIR/$tdir/$tfile || error "$tdir/$tfile not a file"
	rm -r $DIR/$tdir
	$CHECKSTAT -a $DIR/$tdir/$tfile || error "$tdir/$file is not removed"
}
run_test 2 "mkdir; touch; rmdir; check file"

test_3() {
	test_mkdir $DIR/$tdir
	$CHECKSTAT -t dir $DIR/$tdir || error "$tdir is not a directory"
	touch $DIR/$tdir/$tfile
	$CHECKSTAT -t file $DIR/$tdir/$tfile || error "$tdir/$tfile not a file"
	rm -r $DIR/$tdir
	$CHECKSTAT -a $DIR/$tdir || error "$tdir is not removed"
}
run_test 3 "mkdir; touch; rmdir; check dir"

# LU-4471 - failed rmdir on remote directories still removes directory on MDT0
test_4() {
	test_mkdir -i 1 $DIR/$tdir

	touch $DIR/$tdir/$tfile ||
		error "Create file under remote directory failed"

	rmdir $DIR/$tdir &&
		error "Expect error removing in-use dir $DIR/$tdir"

	test -d $DIR/$tdir || error "Remote directory disappeared"

	rm -rf $DIR/$tdir || error "remove remote dir error"
}
run_test 4 "mkdir; touch dir/file; rmdir; checkdir (expect error)"

test_5() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	chmod 0707 $DIR/$tdir/d2 || error "chmod 0707 $tdir/d2 failed"
	$CHECKSTAT -t dir -p 0707 $DIR/$tdir/d2 || error "$tdir/d2 not mode 707"
	$CHECKSTAT -t dir $DIR/$tdir/d2 || error "$tdir/d2 is not a directory"
}
run_test 5 "mkdir .../d5 .../d5/d2; chmod .../d5/d2"

test_6a() {
	touch $DIR/$tfile || error "touch $DIR/$tfile failed"
	chmod 0666 $DIR/$tfile || error "chmod 0666 $tfile failed"
	$CHECKSTAT -t file -p 0666 -u \#$UID $DIR/$tfile ||
		error "$tfile does not have perm 0666 or UID $UID"
	$RUNAS chmod 0444 $DIR/$tfile && error "chmod $tfile worked on UID $UID"
	$CHECKSTAT -t file -p 0666 -u \#$UID $DIR/$tfile ||
		error "$tfile should be 0666 and owned by UID $UID"
}
run_test 6a "touch f6a; chmod f6a; $RUNAS chmod f6a (should return error) =="

test_6c() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID"

	touch $DIR/$tfile
	chown $RUNAS_ID $DIR/$tfile || error "chown $RUNAS_ID $file failed"
	$CHECKSTAT -t file -u \#$RUNAS_ID $DIR/$tfile ||
		error "$tfile should be owned by UID $RUNAS_ID"
	$RUNAS chown $UID $DIR/$tfile && error "chown $UID $file succeeded"
	$CHECKSTAT -t file -u \#$RUNAS_ID $DIR/$tfile ||
		error "$tfile should be owned by UID $RUNAS_ID"
}
run_test 6c "touch f6c; chown f6c; $RUNAS chown f6c (should return error) =="

test_6e() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID"

	touch $DIR/$tfile
	chgrp $RUNAS_ID $DIR/$tfile || error "chgrp $RUNAS_ID $file failed"
	$CHECKSTAT -t file -u \#$UID -g \#$RUNAS_ID $DIR/$tfile ||
		error "$tfile should be owned by GID $UID"
	$RUNAS chgrp $UID $DIR/$tfile && error "chgrp $UID $file succeeded"
	$CHECKSTAT -t file -u \#$UID -g \#$RUNAS_ID $DIR/$tfile ||
		error "$tfile should be owned by UID $UID and GID $RUNAS_ID"
}
run_test 6e "touch+chgrp $tfile; $RUNAS chgrp $tfile (should return error)"

test_6g() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID"

	test_mkdir $DIR/$tdir
	chmod 777 $DIR/$tdir || error "chmod 0777 $tdir failed"
	$RUNAS mkdir $DIR/$tdir/d || error "mkdir $tdir/d failed"
	chmod g+s $DIR/$tdir/d || error "chmod g+s $tdir/d failed"
	test_mkdir $DIR/$tdir/d/subdir
	$CHECKSTAT -g \#$RUNAS_GID $DIR/$tdir/d/subdir ||
		error "$tdir/d/subdir should be GID $RUNAS_GID"
	if [[ $MDSCOUNT -gt 1 ]]; then
		# check remote dir sgid inherite
		$LFS mkdir -i 0 $DIR/$tdir.local ||
			error "mkdir $tdir.local failed"
		chmod g+s $DIR/$tdir.local ||
			error "chmod $tdir.local failed"
		chgrp $RUNAS_GID $DIR/$tdir.local ||
			error "chgrp $tdir.local failed"
		$LFS mkdir -i 1 $DIR/$tdir.local/$tdir.remote ||
			error "mkdir $tdir.remote failed"
		$CHECKSTAT -g \#$RUNAS_GID $DIR/$tdir.local/$tdir.remote ||
			error "$tdir.remote should be owned by $UID.$RUNAS_ID"
		$CHECKSTAT -p 02755 $DIR/$tdir.local/$tdir.remote ||
			error "$tdir.remote should be mode 02755"
	fi
}
run_test 6g "verify new dir in sgid dir inherits group"

test_6h() { # bug 7331
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID"

	touch $DIR/$tfile || error "touch failed"
	chown $RUNAS_ID:$RUNAS_GID $DIR/$tfile || error "initial chown failed"
	$RUNAS -G$RUNAS_GID chown $RUNAS_ID:0 $DIR/$tfile &&
		error "chown $RUNAS_ID:0 $tfile worked as GID $RUNAS_GID"
	$CHECKSTAT -t file -u \#$RUNAS_ID -g \#$RUNAS_GID $DIR/$tfile ||
		error "$tdir/$tfile should be UID $RUNAS_UID GID $RUNAS_GID"
}
run_test 6h "$RUNAS chown RUNAS_ID.0 .../$tfile (should return error)"

test_7a() {
	test_mkdir $DIR/$tdir
	$MCREATE $DIR/$tdir/$tfile
	chmod 0666 $DIR/$tdir/$tfile
	$CHECKSTAT -t file -p 0666 $DIR/$tdir/$tfile ||
		error "$tdir/$tfile should be mode 0666"
}
run_test 7a "mkdir .../d7; mcreate .../d7/f; chmod .../d7/f ===="

test_7b() {
	if [ ! -d $DIR/$tdir ]; then
		test_mkdir $DIR/$tdir
	fi
	$MCREATE $DIR/$tdir/$tfile
	echo -n foo > $DIR/$tdir/$tfile
	[ "$(cat $DIR/$tdir/$tfile)" = "foo" ] || error "$tdir/$tfile not 'foo'"
	$CHECKSTAT -t file -s 3 $DIR/$tdir/$tfile || error "$tfile size not 3"
}
run_test 7b "mkdir .../d7; mcreate d7/f2; echo foo > d7/f2 ====="

test_8() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	chmod 0666 $DIR/$tdir/$tfile
	$CHECKSTAT -t file -p 0666 $DIR/$tdir/$tfile ||
		error "$tfile mode not 0666"
}
run_test 8 "mkdir .../d8; touch .../d8/f; chmod .../d8/f ======="

test_9() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	test_mkdir $DIR/$tdir/d2/d3
	$CHECKSTAT -t dir $DIR/$tdir/d2/d3 || error "$tdir/d2/d3 not a dir"
}
run_test 9 "mkdir .../d9 .../d9/d2 .../d9/d2/d3 ================"

test_10() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	touch $DIR/$tdir/d2/$tfile
	$CHECKSTAT -t file $DIR/$tdir/d2/$tfile ||
		error "$tdir/d2/$tfile not a file"
}
run_test 10 "mkdir .../d10 .../d10/d2; touch .../d10/d2/f ======"

test_11() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d2
	chmod 0666 $DIR/$tdir/d2
	chmod 0705 $DIR/$tdir/d2
	$CHECKSTAT -t dir -p 0705 $DIR/$tdir/d2 ||
		error "$tdir/d2 mode not 0705"
}
run_test 11 "mkdir .../d11 d11/d2; chmod .../d11/d2 ============"

test_12() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	chmod 0666 $DIR/$tdir/$tfile
	chmod 0654 $DIR/$tdir/$tfile
	$CHECKSTAT -t file -p 0654 $DIR/$tdir/$tfile ||
		error "$tdir/d2 mode not 0654"
}
run_test 12 "touch .../d12/f; chmod .../d12/f .../d12/f ========"

test_13() {
	test_mkdir $DIR/$tdir
	dd if=/dev/zero of=$DIR/$tdir/$tfile count=10
	>  $DIR/$tdir/$tfile
	$CHECKSTAT -t file -s 0 $DIR/$tdir/$tfile ||
		error "$tdir/$tfile size not 0 after truncate"
}
run_test 13 "creat .../d13/f; dd .../d13/f; > .../d13/f ========"

test_14() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	rm $DIR/$tdir/$tfile
	$CHECKSTAT -a $DIR/$tdir/$tfile || error "$tdir/$tfile not removed"
}
run_test 14 "touch .../d14/f; rm .../d14/f; rm .../d14/f ======="

test_15() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	mv $DIR/$tdir/$tfile $DIR/$tdir/${tfile}_2
	$CHECKSTAT -t file $DIR/$tdir/${tfile}_2 ||
		error "$tdir/${tfile_2} not a file after rename"
	rm $DIR/$tdir/${tfile}_2 || error "unlink failed after rename"
}
run_test 15 "touch .../d15/f; mv .../d15/f .../d15/f2 =========="

test_16() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	rm -rf $DIR/$tdir/$tfile
	$CHECKSTAT -a $DIR/$tdir/$tfile || error "$tdir/$tfile not removed"
}
run_test 16 "touch .../d16/f; rm -rf .../d16/f"

test_17a() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	ln -s $DIR/$tdir/$tfile $DIR/$tdir/l-exist
	ls -l $DIR/$tdir
	$CHECKSTAT -l $DIR/$tdir/$tfile $DIR/$tdir/l-exist ||
		error "$tdir/l-exist not a symlink"
	$CHECKSTAT -f -t f $DIR/$tdir/l-exist ||
		error "$tdir/l-exist not referencing a file"
	rm -f $DIR/$tdir/l-exist
	$CHECKSTAT -a $DIR/$tdir/l-exist || error "$tdir/l-exist not removed"
}
run_test 17a "symlinks: create, remove (real)"

test_17b() {
	test_mkdir $DIR/$tdir
	ln -s no-such-file $DIR/$tdir/l-dangle
	ls -l $DIR/$tdir
	$CHECKSTAT -l no-such-file $DIR/$tdir/l-dangle ||
		error "$tdir/l-dangle not referencing no-such-file"
	$CHECKSTAT -fa $DIR/$tdir/l-dangle ||
		error "$tdir/l-dangle not referencing non-existent file"
	rm -f $DIR/$tdir/l-dangle
	$CHECKSTAT -a $DIR/$tdir/l-dangle || error "$tdir/l-dangle not removed"
}
run_test 17b "symlinks: create, remove (dangling)"

test_17c() { # bug 3440 - don't save failed open RPC for replay
	test_mkdir $DIR/$tdir
	ln -s foo $DIR/$tdir/$tfile
	cat $DIR/$tdir/$tfile && error "opened non-existent symlink" || true
}
run_test 17c "symlinks: open dangling (should return error)"

test_17d() {
	test_mkdir $DIR/$tdir
	ln -s foo $DIR/$tdir/$tfile
	touch $DIR/$tdir/$tfile || error "creating to new symlink"
}
run_test 17d "symlinks: create dangling"

test_17e() {
	test_mkdir $DIR/$tdir
	local foo=$DIR/$tdir/$tfile
	ln -s $foo $foo || error "create symlink failed"
	ls -l $foo || error "ls -l failed"
	ls $foo && error "ls not failed" || true
}
run_test 17e "symlinks: create recursive symlink (should return error)"

test_17f() {
	test_mkdir $DIR/$tdir
	ln -s 1234567890/2234567890/3234567890/4234567890 $DIR/$tdir/111
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890 $DIR/$tdir/222
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890/7234567890/8234567890 $DIR/$tdir/333
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890/7234567890/8234567890/9234567890/a234567890/b234567890 $DIR/$tdir/444
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890/7234567890/8234567890/9234567890/a234567890/b234567890/c234567890/d234567890/f234567890 $DIR/$tdir/555
	ln -s 1234567890/2234567890/3234567890/4234567890/5234567890/6234567890/7234567890/8234567890/9234567890/a234567890/b234567890/c234567890/d234567890/f234567890/aaaaaaaaaa/bbbbbbbbbb/cccccccccc/dddddddddd/eeeeeeeeee/ffffffffff/ $DIR/$tdir/666
	ls -l  $DIR/$tdir
}
run_test 17f "symlinks: long and very long symlink name"

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
	test_mkdir $DIR/$tdir
	local TESTS="59 60 61 4094 4095"

	# Fix for inode size boundary in 2.1.4
	[ $MDS1_VERSION -lt $(version_code 2.1.4) ] &&
		TESTS="4094 4095"

	# Patch not applied to 2.2 or 2.3 branches
	[ $MDS1_VERSION -ge $(version_code 2.2.0) ] &&
	[ $MDS1_VERSION -le $(version_code 2.3.55) ] &&
		TESTS="4094 4095"

	# skip long symlink name for rhel6.5.
	# rhel6.5 has a limit (PATH_MAX - sizeof(struct filename))
	grep -q '6.5' /etc/redhat-release &>/dev/null &&
		TESTS="59 60 61 4062 4063"

	for i in $TESTS; do
		local SYMNAME=$(str_repeat 'x' $i)
		ln -s $SYMNAME $DIR/$tdir/f$i || error "failed $i-char symlink"
		readlink $DIR/$tdir/f$i	|| error "failed $i-char readlink"
	done
}
run_test 17g "symlinks: really long symlink name and inode boundaries"

test_17h() { #bug 17378
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local mdt_idx

	test_mkdir $DIR/$tdir
	mdt_idx=$($LFS getdirstripe -i $DIR/$tdir)
	$LFS setstripe -c -1 $DIR/$tdir
	#define OBD_FAIL_MDS_LOV_PREP_CREATE 0x141
	do_facet mds$((mdt_idx + 1)) lctl set_param fail_loc=0x80000141
	touch $DIR/$tdir/$tfile || true
}
run_test 17h "create objects: lov_free_memmd() doesn't lbug"

test_17i() { #bug 20018
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local foo=$DIR/$tdir/$tfile
	local mdt_idx

	test_mkdir -c1 $DIR/$tdir
	mdt_idx=$($LFS getdirstripe -i $DIR/$tdir)
	ln -s $foo $foo || error "create symlink failed"
#define OBD_FAIL_MDS_READLINK_EPROTO     0x143
	do_facet mds$((mdt_idx + 1)) lctl set_param fail_loc=0x80000143
	ls -l $foo && error "error not detected"
	return 0
}
run_test 17i "don't panic on short symlink (should return error)"

test_17k() { #bug 22301
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ -z "$(which rsync 2>/dev/null)" ]] &&
		skip "no rsync command"
	rsync --help | grep -q xattr ||
		skip_env "$(rsync --version | head -n1) does not support xattrs"
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir.new
	touch $DIR/$tdir/$tfile
	ln -s $DIR/$tdir/$tfile $DIR/$tdir/$tfile.lnk
	rsync -av -X $DIR/$tdir/ $DIR/$tdir.new ||
		error "rsync failed with xattrs enabled"
}
run_test 17k "symlinks: rsync with xattrs enabled"

test_17l() { # LU-279
	[[ -z "$(which getfattr 2>/dev/null)" ]] &&
		skip "no getfattr command"

	test_mkdir $DIR/$tdir
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
run_test 17l "Ensure lgetxattr's returned xattr size is consistent"

# LU-1540
test_17m() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ "$mds1_FSTYPE" != "ldiskfs" ] && skip_env "ldiskfs only test"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDS1_VERSION -ge $(version_code 2.2.0) ] &&
	[ $MDS1_VERSION -le $(version_code 2.2.93) ] &&
		skip "MDS 2.2.0-2.2.93 do not NUL-terminate symlinks"

	local short_sym="0123456789"
	local wdir=$DIR/$tdir
	local i

	test_mkdir $wdir
	long_sym=$short_sym
	# create a long symlink file
	for ((i = 0; i < 4; ++i)); do
		long_sym=${long_sym}${long_sym}
	done

	echo "create 512 short and long symlink files under $wdir"
	for ((i = 0; i < 256; ++i)); do
		ln -sf ${long_sym}"a5a5" $wdir/long-$i
		ln -sf ${short_sym}"a5a5" $wdir/short-$i
	done

	echo "erase them"
	rm -f $wdir/*
	sync
	wait_delete_completed

	echo "recreate the 512 symlink files with a shorter string"
	for ((i = 0; i < 512; ++i)); do
		# rewrite the symlink file with a shorter string
		ln -sf ${long_sym} $wdir/long-$i || error "long_sym failed"
		ln -sf ${short_sym} $wdir/short-$i || error "short_sym failed"
	done

	local mds_index=$(($($LFS getstripe -m $wdir) + 1))
	local devname=$(mdsdevname $mds_index)

	echo "stop and checking mds${mds_index}:"
	# e2fsck should not return error
	stop mds${mds_index}
	run_e2fsck $(facet_active_host mds${mds_index}) $devname -n
	rc=$?

	start mds${mds_index} $devname $MDS_MOUNT_OPTS ||
		error "start mds${mds_index} failed"
	df $MOUNT > /dev/null 2>&1
	[ $rc -eq 0 ] ||
		error "e2fsck detected error for short/long symlink: rc=$rc"
	rm -f $wdir/*
}
run_test 17m "run e2fsck against MDT which contains short/long symlink"

check_fs_consistency_17n() {
	local mdt_index
	local rc=0

	# create/unlink in 17n only change 2 MDTs(MDT1/MDT2),
	# so it only check MDT1/MDT2 instead of all of MDTs.
	for mdt_index in 1 2; do
		local devname=$(mdsdevname $mdt_index)
		# e2fsck should not return error
		stop mds${mdt_index}
		run_e2fsck $(facet_active_host mds$mdt_index) $devname -n ||
			rc=$((rc + $?))

		start mds${mdt_index} $devname $MDS_MOUNT_OPTS ||
			error "mount mds$mdt_index failed"
		df $MOUNT > /dev/null 2>&1
	done
	return $rc
}

test_17n() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ "$mds1_FSTYPE" != "ldiskfs" ] && skip_env "ldiskfs only test"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDS1_VERSION -ge $(version_code 2.2.0) ] &&
	[ $MDS1_VERSION -le $(version_code 2.2.93) ] &&
		skip "MDS 2.2.0-2.2.93 do not NUL-terminate symlinks"

	local i

	test_mkdir $DIR/$tdir
	for ((i=0; i<10; i++)); do
		$LFS mkdir -i1 -c2 $DIR/$tdir/remote_dir_${i} ||
			error "create remote dir error $i"
		createmany -o $DIR/$tdir/remote_dir_${i}/f 10 ||
			error "create files under remote dir failed $i"
	done

	check_fs_consistency_17n ||
		error "e2fsck report error after create files under remote dir"

	for ((i = 0; i < 10; i++)); do
		rm -rf $DIR/$tdir/remote_dir_${i} ||
			error "destroy remote dir error $i"
	done

	check_fs_consistency_17n ||
		error "e2fsck report error after unlink files under remote dir"

	[ $MDS1_VERSION -lt $(version_code 2.4.50) ] &&
		skip "lustre < 2.4.50 does not support migrate mv"

	for ((i = 0; i < 10; i++)); do
		mkdir -p $DIR/$tdir/remote_dir_${i}
		createmany -o $DIR/$tdir/remote_dir_${i}/f 10 ||
			error "create files under remote dir failed $i"
		$LFS migrate --mdt-index 1 $DIR/$tdir/remote_dir_${i} ||
			error "migrate remote dir error $i"
	done
	check_fs_consistency_17n || error "e2fsck report error after migration"

	for ((i = 0; i < 10; i++)); do
		rm -rf $DIR/$tdir/remote_dir_${i} ||
			error "destroy remote dir error $i"
	done

	check_fs_consistency_17n || error "e2fsck report error after unlink"
}
run_test 17n "run e2fsck against master/slave MDT which contains remote dir"

test_17o() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDS1_VERSION -lt $(version_code 2.3.64) ] &&
		skip "Need MDS version at least 2.3.64"

	local wdir=$DIR/${tdir}o
	local mdt_index
	local rc=0

	test_mkdir $wdir
	touch $wdir/$tfile
	mdt_index=$($LFS getstripe -m $wdir/$tfile)
	mdt_index=$((mdt_index + 1))

	cancel_lru_locks mdc
	#fail mds will wait the failover finish then set
	#following fail_loc to avoid interfer the recovery process.
	fail mds${mdt_index}

	#define OBD_FAIL_OSD_LMA_INCOMPAT 0x194
	do_facet mds${mdt_index} lctl set_param fail_loc=0x194
	ls -l $wdir/$tfile && rc=1
	do_facet mds${mdt_index} lctl set_param fail_loc=0
	[[ $rc -eq 0 ]] || error "stat file should fail"
}
run_test 17o "stat file with incompat LMA feature"

test_18() {
	touch $DIR/$tfile || error "Failed to touch $DIR/$tfile: $?"
	ls $DIR || error "Failed to ls $DIR: $?"
}
run_test 18 "touch .../f ; ls ... =============================="

test_19a() {
	touch $DIR/$tfile
	ls -l $DIR
	rm $DIR/$tfile
	$CHECKSTAT -a $DIR/$tfile || error "$tfile was not removed"
}
run_test 19a "touch .../f19 ; ls -l ... ; rm .../f19 ==========="

test_19b() {
	ls -l $DIR/$tfile && error "ls -l $tfile failed"|| true
}
run_test 19b "ls -l .../f19 (should return error) =============="

test_19c() {
	[ $RUNAS_ID -eq $UID ] &&
		skip_env "RUNAS_ID = UID = $UID -- skipping"

	$RUNAS touch $DIR/$tfile && error "create non-root file failed" || true
}
run_test 19c "$RUNAS touch .../f19 (should return error) =="

test_19d() {
	cat $DIR/f19 && error || true
}
run_test 19d "cat .../f19 (should return error) =============="

test_20() {
	touch $DIR/$tfile
	rm $DIR/$tfile
	touch $DIR/$tfile
	rm $DIR/$tfile
	touch $DIR/$tfile
	rm $DIR/$tfile
	$CHECKSTAT -a $DIR/$tfile || error "$tfile was not removed"
}
run_test 20 "touch .../f ; ls -l ..."

test_21() {
	test_mkdir $DIR/$tdir
	[ -f $DIR/$tdir/dangle ] && rm -f $DIR/$tdir/dangle
	ln -s dangle $DIR/$tdir/link
	echo foo >> $DIR/$tdir/link
	cat $DIR/$tdir/dangle
	$CHECKSTAT -t link $DIR/$tdir/link || error "$tdir/link not a link"
	$CHECKSTAT -f -t file $DIR/$tdir/link ||
		error "$tdir/link not linked to a file"
}
run_test 21 "write to dangling link"

test_22() {
	local wdir=$DIR/$tdir
	test_mkdir $wdir
	chown $RUNAS_ID:$RUNAS_GID $wdir
	(cd $wdir || error "cd $wdir failed";
		$RUNAS tar cf - /etc/hosts /etc/sysconfig/network |
		$RUNAS tar xf -)
	ls -lR $wdir/etc || error "ls -lR $wdir/etc failed"
	$CHECKSTAT -t dir $wdir/etc || error "checkstat -t dir failed"
	$CHECKSTAT -u \#$RUNAS_ID -g \#$RUNAS_GID $wdir/etc ||
		error "checkstat -u failed"
}
run_test 22 "unpack tar archive as non-root user"

# was test_23
test_23a() {
	test_mkdir $DIR/$tdir
	local file=$DIR/$tdir/$tfile

	openfile -f O_CREAT:O_EXCL $file || error "$file create failed"
	openfile -f O_CREAT:O_EXCL $file &&
		error "$file recreate succeeded" || true
}
run_test 23a "O_CREAT|O_EXCL in subdir"

test_23b() { # bug 18988
	test_mkdir $DIR/$tdir
	local file=$DIR/$tdir/$tfile

	rm -f $file
	echo foo > $file || error "write filed"
	echo bar >> $file || error "append filed"
	$CHECKSTAT -s 8 $file || error "wrong size"
	rm $file
}
run_test 23b "O_APPEND check"

# LU-9409, size with O_APPEND and tiny writes
test_23c() {
	local file=$DIR/$tfile

	# single dd
	dd conv=notrunc oflag=append if=/dev/zero of=$file bs=8 count=800
	$CHECKSTAT -s 6400 $file || error "wrong size, expected 6400"
	rm -f $file

	# racing tiny writes
	dd conv=notrunc oflag=append if=/dev/zero of=$file bs=8 count=800 &
	dd conv=notrunc oflag=append if=/dev/zero of=$file bs=8 count=800 &
	wait
	$CHECKSTAT -s 12800 $file || error "wrong size, expected 12800"
	rm -f $file

	#racing tiny & normal writes
	dd conv=notrunc oflag=append if=/dev/zero of=$file bs=4096 count=4 &
	dd conv=notrunc oflag=append if=/dev/zero of=$file bs=8 count=100 &
	wait
	$CHECKSTAT -s 17184 $file || error "wrong size, expected 17184"
	rm -f $file

	#racing tiny & normal writes 2, ugly numbers
	dd conv=notrunc oflag=append if=/dev/zero of=$file bs=4099 count=11 &
	dd conv=notrunc oflag=append if=/dev/zero of=$file bs=17 count=173 &
	wait
	$CHECKSTAT -s 48030 $file || error "wrong size, expected 48030"
	rm -f $file
}
run_test 23c "O_APPEND size checks for tiny writes"

# LU-11069 file offset is correct after appending writes
test_23d() {
	local file=$DIR/$tfile
	local offset

	echo CentaurHauls > $file
	offset=$($MULTIOP $file oO_WRONLY:O_APPEND:w13Zp)
	if ((offset != 26)); then
		error "wrong offset, expected 26, got '$offset'"
	fi
}
run_test 23d "file offset is correct after appending writes"

# rename sanity
test_24a() {
	echo '-- same directory rename'
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile.1
	mv $DIR/$tdir/$tfile.1 $DIR/$tdir/$tfile.2
	$CHECKSTAT -t file $DIR/$tdir/$tfile.2 || error "$tfile.2 not a file"
}
run_test 24a "rename file to non-existent target"

test_24b() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile.{1,2}
	mv $DIR/$tdir/$tfile.1 $DIR/$tdir/$tfile.2
	$CHECKSTAT -a $DIR/$tdir/$tfile.1 || error "$tfile.1 exists"
	$CHECKSTAT -t file $DIR/$tdir/$tfile.2 || error "$tfile.2 not a file"
}
run_test 24b "rename file to existing target"

test_24c() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/d$testnum.1
	mv $DIR/$tdir/d$testnum.1 $DIR/$tdir/d$testnum.2
	$CHECKSTAT -a $DIR/$tdir/d$testnum.1 || error "d$testnum.1 exists"
	$CHECKSTAT -t dir $DIR/$tdir/d$testnum.2 || error "d$testnum.2 not dir"
}
run_test 24c "rename directory to non-existent target"

test_24d() {
	test_mkdir -c1 $DIR/$tdir
	test_mkdir -c1 $DIR/$tdir/d$testnum.1
	test_mkdir -c1 $DIR/$tdir/d$testnum.2
	mrename $DIR/$tdir/d$testnum.1 $DIR/$tdir/d$testnum.2
	$CHECKSTAT -a $DIR/$tdir/d$testnum.1 || error "d$testnum.1 exists"
	$CHECKSTAT -t dir $DIR/$tdir/d$testnum.2 || error "d$testnum.2 not dir"
}
run_test 24d "rename directory to existing target"

test_24e() {
	echo '-- cross directory renames --'
	test_mkdir $DIR/R5a
	test_mkdir $DIR/R5b
	touch $DIR/R5a/f
	mv $DIR/R5a/f $DIR/R5b/g
	$CHECKSTAT -a $DIR/R5a/f || error "$DIR/R5a/f exists"
	$CHECKSTAT -t file $DIR/R5b/g || error "$DIR/R5b/g not file type"
}
run_test 24e "touch .../R5a/f; rename .../R5a/f .../R5b/g ======"

test_24f() {
	test_mkdir $DIR/R6a
	test_mkdir $DIR/R6b
	touch $DIR/R6a/f $DIR/R6b/g
	mv $DIR/R6a/f $DIR/R6b/g
	$CHECKSTAT -a $DIR/R6a/f || error "$DIR/R6a/f exists"
	$CHECKSTAT -t file $DIR/R6b/g || error "$DIR/R6b/g not file type"
}
run_test 24f "touch .../R6a/f R6b/g; mv .../R6a/f .../R6b/g ===="

test_24g() {
	test_mkdir $DIR/R7a
	test_mkdir $DIR/R7b
	test_mkdir $DIR/R7a/d
	mv $DIR/R7a/d $DIR/R7b/e
	$CHECKSTAT -a $DIR/R7a/d || error "$DIR/R7a/d exists"
	$CHECKSTAT -t dir $DIR/R7b/e || error "$DIR/R7b/e not dir type"
}
run_test 24g "mkdir .../R7{a,b}/d; mv .../R7a/d .../R7b/e ======"

test_24h() {
	test_mkdir -c1 $DIR/R8a
	test_mkdir -c1 $DIR/R8b
	test_mkdir -c1 $DIR/R8a/d
	test_mkdir -c1 $DIR/R8b/e
	mrename $DIR/R8a/d $DIR/R8b/e
	$CHECKSTAT -a $DIR/R8a/d || error "$DIR/R8a/d exists"
	$CHECKSTAT -t dir $DIR/R8b/e || error "$DIR/R8b/e not dir type"
}
run_test 24h "mkdir .../R8{a,b}/{d,e}; rename .../R8a/d .../R8b/e"

test_24i() {
	echo "-- rename error cases"
	test_mkdir $DIR/R9
	test_mkdir $DIR/R9/a
	touch $DIR/R9/f
	mrename $DIR/R9/f $DIR/R9/a
	$CHECKSTAT -t file $DIR/R9/f || error "$DIR/R9/f not file type"
	$CHECKSTAT -t dir  $DIR/R9/a || error "$DIR/R9/a not dir type"
	$CHECKSTAT -a $DIR/R9/a/f || error "$DIR/R9/a/f exists"
}
run_test 24i "rename file to dir error: touch f ; mkdir a ; rename f a"

test_24j() {
	test_mkdir $DIR/R10
	mrename $DIR/R10/f $DIR/R10/g
	$CHECKSTAT -t dir $DIR/R10 || error "$DIR/R10 not dir type"
	$CHECKSTAT -a $DIR/R10/f || error "$DIR/R10/f exists"
	$CHECKSTAT -a $DIR/R10/g || error "$DIR/R10/g exists"
}
run_test 24j "source does not exist ============================"

test_24k() {
	test_mkdir $DIR/R11a
	test_mkdir $DIR/R11a/d
	touch $DIR/R11a/f
	mv $DIR/R11a/f $DIR/R11a/d
	$CHECKSTAT -a $DIR/R11a/f || error "$DIR/R11a/f exists"
	$CHECKSTAT -t file $DIR/R11a/d/f || error "$DIR/R11a/d/f not file type"
}
run_test 24k "touch .../R11a/f; mv .../R11a/f .../R11a/d ======="

# bug 2429 - rename foo foo foo creates invalid file
test_24l() {
	f="$DIR/f24l"
	$MULTIOP $f OcNs || error "rename of ${f} to itself failed"
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
    $CHECKSTAT ${f} || error "${f} missing"
    mv ${f} ${f}.rename
    $CHECKSTAT ${f}.rename || error "${f}.rename missing"
    $CHECKSTAT -a ${f} || error "${f} exists"
}
run_test 24n "Statting the old file after renaming (Posix rename 2)"

test_24o() {
	test_mkdir $DIR/$tdir
	rename_many -s random -v -n 10 $DIR/$tdir
}
run_test 24o "rename of files during htree split"

test_24p() {
	test_mkdir $DIR/R12a
	test_mkdir $DIR/R12b
	DIRINO=`ls -lid $DIR/R12a | awk '{ print $1 }'`
	mrename $DIR/R12a $DIR/R12b
	$CHECKSTAT -a $DIR/R12a || error "$DIR/R12a exists"
	$CHECKSTAT -t dir $DIR/R12b || error "$DIR/R12b not dir type"
	DIRINO2=`ls -lid $DIR/R12b | awk '{ print $1 }'`
	[ "$DIRINO" = "$DIRINO2" ] || error "R12a $DIRINO != R12b $DIRINO2"
}
run_test 24p "mkdir .../R12{a,b}; rename .../R12a .../R12b"

cleanup_multiop_pause() {
	trap 0
	kill -USR1 $MULTIPID
}

test_24q() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	$MULTIOP $DIR/$tfile C2w$((2048 * 1024))c || error "multiop failed"
	$CHECKSTAT -s $((2048 * 1024)) $DIR/$tfile || error "wrong file size"
}
run_test 24u "create stripe file"

simple_cleanup_common() {
	local rc=0
	trap 0
	[ -z "$DIR" -o -z "$tdir" ] && return 0

	local start=$SECONDS
	rm -rf $DIR/$tdir
	rc=$?
	wait_delete_completed
	echo "cleanup time $((SECONDS - start))"
	return $rc
}

max_pages_per_rpc() {
	local mdtname="$(printf "MDT%04x" ${1:-0})"
	$LCTL get_param -n mdc.*$mdtname*.max_pages_per_rpc
}

test_24v() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local nrfiles=${COUNT:-100000}
	local fname="$DIR/$tdir/$tfile"

	# Performance issue on ZFS see LU-4072 (c.f. LU-2887)
	[ "$mds1_FSTYPE" = "zfs" ] && nrfiles=${COUNT:-10000}

	test_mkdir "$(dirname $fname)"
	# assume MDT0000 has the fewest inodes
	local stripes=$($LFS getdirstripe -c $(dirname $fname))
	local free_inodes=$(($(mdt_free_inodes 0) * stripes))
	[[ $free_inodes -lt $nrfiles ]] && nrfiles=$free_inodes

	trap simple_cleanup_common EXIT

	createmany -m "$fname" $nrfiles

	cancel_lru_locks mdc
	lctl set_param mdc.*.stats clear

	# was previously test_24D: LU-6101
	# readdir() returns correct number of entries after cursor reload
	local num_ls=$(ls $DIR/$tdir | wc -l)
	local num_uniq=$(ls $DIR/$tdir | sort -u | wc -l)
	local num_all=$(ls -a $DIR/$tdir | wc -l)
	if [ $num_ls -ne $nrfiles -o $num_uniq -ne $nrfiles -o \
	     $num_all -ne $((nrfiles + 2)) ]; then
		error "Expected $nrfiles files, got $num_ls " \
			"($num_uniq unique $num_all .&..)"
	fi
	# LU-5 large readdir
	# dirent_size = 32 bytes for sizeof(struct lu_dirent) +
	#               N bytes for name (len($nrfiles) rounded to 8 bytes) +
	#               8 bytes for luda_type (4 bytes rounded to 8 bytes)
	# take into account of overhead in lu_dirpage header and end mark in
	# each page, plus one in rpc_num calculation.
	local dirent_size=$((32 + (${#tfile} | 7) + 1 + 8))
	local page_entries=$(((PAGE_SIZE - 24) / dirent_size))
	local mdt_idx=$($LFS getdirstripe -i $(dirname $fname))
	local rpc_pages=$(max_pages_per_rpc $mdt_idx)
	local rpc_max=$((nrfiles / (page_entries * rpc_pages) + stripes))
	local mds_readpage=$(calc_stats mdc.*.stats mds_readpage)
	echo "readpages: $mds_readpage rpc_max: $rpc_max"
	(( $mds_readpage < $rpc_max - 2 || $mds_readpage > $rpc_max + 1)) &&
		error "large readdir doesn't take effect: " \
		      "$mds_readpage should be about $rpc_max"

	simple_cleanup_common
}
run_test 24v "list large directory (test hash collision, b=17560)"

test_24w() { # bug21506
        SZ1=234852
        dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 seek=4096 || return 1
        dd if=/dev/zero bs=$SZ1 count=1 >> $DIR/$tfile || return 2
        dd if=$DIR/$tfile of=$DIR/${tfile}_left bs=1M skip=4097 || return 3
        SZ2=`ls -l $DIR/${tfile}_left | awk '{print $5}'`
	[[ "$SZ1" -eq "$SZ2" ]] ||
                error "Error reading at the end of the file $tfile"
}
run_test 24w "Reading a file larger than 4Gb"

test_24x() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $MDS1_VERSION -lt $(version_code 2.7.56) ]] &&
		skip "Need MDS version at least 2.7.56"

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	test_mkdir $DIR/$tdir
	$LFS mkdir -i $MDTIDX $remote_dir ||
		error "create remote directory failed"

	test_mkdir $DIR/$tdir/src_dir
	touch $DIR/$tdir/src_file
	test_mkdir $remote_dir/tgt_dir
	touch $remote_dir/tgt_file

	mrename $DIR/$tdir/src_dir $remote_dir/tgt_dir ||
		error "rename dir cross MDT failed!"

	mrename $DIR/$tdir/src_file $remote_dir/tgt_file ||
		error "rename file cross MDT failed!"

	touch $DIR/$tdir/ln_file
	ln $DIR/$tdir/ln_file $remote_dir/ln_name ||
		error "ln file cross MDT failed"

	rm -rf $DIR/$tdir || error "Can not delete directories"
}
run_test 24x "cross MDT rename/link"

test_24y() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local remote_dir=$DIR/$tdir/remote_dir
	local mdtidx=1

	test_mkdir $DIR/$tdir
	$LFS mkdir -i $mdtidx $remote_dir ||
		error "create remote directory failed"

	test_mkdir $remote_dir/src_dir
	touch $remote_dir/src_file
	test_mkdir $remote_dir/tgt_dir
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
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"
	[[ $MDS1_VERSION -lt $(version_code 2.12.51) ]] &&
		skip "Need MDS version at least 2.12.51"

	local index

	for index in 0 1; do
		$LFS mkdir -i $index $DIR/$tdir.$index || error "mkdir failed"
		touch $DIR/$tdir.0/$tfile.$index || error "touch failed"
	done

	mv $DIR/$tdir.0/$tfile.0 $DIR/$tdir.1 || error "mv $tfile.0 failed"

	index=$($LFS getstripe -m $DIR/$tdir.1/$tfile.0)
	[ $index -eq 0 ] || error "$tfile.0 is on MDT$index"

	local mdts=$(comma_list $(mdts_nodes))

	do_nodes $mdts $LCTL set_param mdt.*.enable_remote_rename=0
	stack_trap "do_nodes $mdts $LCTL \
		set_param mdt.*.enable_remote_rename=1" EXIT

	mv $DIR/$tdir.0/$tfile.1 $DIR/$tdir.1 || error "mv $tfile.1 failed"

	index=$($LFS getstripe -m $DIR/$tdir.1/$tfile.1)
	[ $index -eq 1 ] || error "$tfile.1 is on MDT$index"
}
run_test 24z "cross-MDT rename is done as cp"

test_24A() { # LU-3182
	local NFILES=5000

	rm -rf $DIR/$tdir
	test_mkdir $DIR/$tdir
	trap simple_cleanup_common EXIT
	createmany -m $DIR/$tdir/$tfile $NFILES
	local t=$(ls $DIR/$tdir | wc -l)
	local u=$(ls $DIR/$tdir | sort -u | wc -l)
	local v=$(ls -ai $DIR/$tdir | sort -u | wc -l)
	if [ $t -ne $NFILES -o $u -ne $NFILES -o $v -ne $((NFILES + 2)) ] ; then
		error "Expected $NFILES files, got $t ($u unique $v .&..)"
	fi

	simple_cleanup_common || error "Can not delete directories"
}
run_test 24A "readdir() returns correct number of entries."

test_24B() { # LU-4805
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	local count

	test_mkdir $DIR/$tdir
	$LFS setdirstripe -i0 -c$MDSCOUNT $DIR/$tdir/striped_dir ||
		error "create striped dir failed"

	count=$(ls -ai $DIR/$tdir/striped_dir | wc -l)
	[ $count -eq 2 ] || error "Expected 2, got $count"

	touch $DIR/$tdir/striped_dir/a

	count=$(ls -ai $DIR/$tdir/striped_dir | wc -l)
	[ $count -eq 3 ] || error "Expected 3, got $count"

	touch $DIR/$tdir/striped_dir/.f

	count=$(ls -ai $DIR/$tdir/striped_dir | wc -l)
	[ $count -eq 4 ] || error "Expected 4, got $count"

	rm -rf $DIR/$tdir || error "Can not delete directories"
}
run_test 24B "readdir for striped dir return correct number of entries"

test_24C() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	mkdir $DIR/$tdir
	mkdir $DIR/$tdir/d0
	mkdir $DIR/$tdir/d1

	$LFS setdirstripe -i0 -c$MDSCOUNT $DIR/$tdir/d0/striped_dir ||
		error "create striped dir failed"

	cd $DIR/$tdir/d0/striped_dir

	local d0_ino=$(ls -i -l -a $DIR/$tdir | grep "d0" | awk '{print $1}')
	local d1_ino=$(ls -i -l -a $DIR/$tdir | grep "d1" | awk '{print $1}')
	local parent_ino=$(ls -i -l -a | grep "\.\." | awk '{print $1}')

	[ "$d0_ino" = "$parent_ino" ] ||
		error ".. wrong, expect $d0_ino, get $parent_ino"

	mv $DIR/$tdir/d0/striped_dir $DIR/$tdir/d1/ ||
		error "mv striped dir failed"

	parent_ino=$(ls -i -l -a | grep "\.\." | awk '{print $1}')

	[ "$d1_ino" = "$parent_ino" ] ||
		error ".. wrong after mv, expect $d1_ino, get $parent_ino"
}
run_test 24C "check .. in striped dir"

test_24E() {
	[[ $MDSCOUNT -lt 4 ]] && skip_env "needs >= 4 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	mkdir -p $DIR/$tdir
	mkdir $DIR/$tdir/src_dir
	$LFS mkdir -i 1 $DIR/$tdir/src_dir/src_child ||
		error "create remote source failed"

	touch $DIR/$tdir/src_dir/src_child/a

	$LFS mkdir -i 2 $DIR/$tdir/tgt_dir ||
		error "create remote target dir failed"

	$LFS mkdir -i 3 $DIR/$tdir/tgt_dir/tgt_child ||
		error "create remote target child failed"

	mrename $DIR/$tdir/src_dir/src_child $DIR/$tdir/tgt_dir/tgt_child ||
		error "rename dir cross MDT failed!"

	find $DIR/$tdir

	$CHECKSTAT -t dir $DIR/$tdir/src_dir/src_child &&
		error "src_child still exists after rename"

	$CHECKSTAT -t file $DIR/$tdir/tgt_dir/tgt_child/a ||
		error "missing file(a) after rename"

	rm -rf $DIR/$tdir || error "Can not delete directories"
}
run_test 24E "cross MDT rename/link"

test_24F () {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return 0

	local repeats=1000
	[ "$SLOW" = "no" ] && repeats=100

	mkdir -p $DIR/$tdir

	echo "$repeats repeats"
	for ((i = 0; i < repeats; i++)); do
		$LFS mkdir -i0 -c2 $DIR/$tdir/test || error "mkdir fails"
		touch $DIR/$tdir/test/a || error "touch fails"
		mkdir $DIR/$tdir/test/b || error "mkdir fails"
		rm -rf $DIR/$tdir/test || error "rmdir fails"
	done

	true
}
run_test 24F "hash order vs readdir (LU-11330)"

test_25a() {
	echo '== symlink sanity ============================================='

	test_mkdir $DIR/d25
	ln -s d25 $DIR/s25
	touch $DIR/s25/foo ||
		error "File creation in symlinked directory failed"
}
run_test 25a "create file in symlinked directory ==============="

test_25b() {
	[ ! -d $DIR/d25 ] && test_25a
	$CHECKSTAT -t file $DIR/s25/foo || error "$DIR/s25/foo not file type"
}
run_test 25b "lookup file in symlinked directory ==============="

test_26a() {
	test_mkdir $DIR/d26
	test_mkdir $DIR/d26/d26-2
	ln -s d26/d26-2 $DIR/s26
	touch $DIR/s26/foo || error "File creation failed"
}
run_test 26a "multiple component symlink ======================="

test_26b() {
	test_mkdir -p $DIR/$tdir/d26-2
	ln -s $tdir/d26-2/foo $DIR/s26-2
	touch $DIR/s26-2 || error "File creation failed"
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
run_test 26c "chain of symlinks"

# recursive symlinks (bug 439)
test_26d() {
	ln -s d26-3/foo $DIR/d26-3
}
run_test 26d "create multiple component recursive symlink"

test_26e() {
	[ ! -h $DIR/d26-3 ] && test_26d
	rm $DIR/d26-3
}
run_test 26e "unlink multiple component recursive symlink"

# recursive symlinks (bug 7022)
test_26f() {
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/$tfile
	cd $DIR/$tdir/$tfile           || error "cd $DIR/$tdir/$tfile failed"
	test_mkdir -p lndir/bar1
	test_mkdir $DIR/$tdir/$tfile/$tfile
	cd $tfile                || error "cd $tfile failed"
	ln -s .. dotdot          || error "ln dotdot failed"
	ln -s dotdot/lndir lndir || error "ln lndir failed"
	cd $DIR/$tdir                 || error "cd $DIR/$tdir failed"
	output=`ls $tfile/$tfile/lndir/bar1`
	[ "$output" = bar1 ] && error "unexpected output"
	rm -r $tfile             || error "rm $tfile failed"
	$CHECKSTAT -a $DIR/$tfile || error "$tfile not gone"
}
run_test 26f "rm -r of a directory which has recursive symlink"

test_27a() {
	test_mkdir $DIR/$tdir
	$LFS getstripe $DIR/$tdir
	$LFS setstripe -c 1 $DIR/$tdir/$tfile || error "setstripe failed"
	$CHECKSTAT -t file $DIR/$tdir/$tfile || error "checkstat failed"
	cp /etc/hosts $DIR/$tdir/$tfile || error "Can't copy to one stripe file"
}
run_test 27a "one stripe file"

test_27b() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	test_mkdir $DIR/$tdir
	$LFS setstripe -c 2 $DIR/$tdir/$tfile || error "setstripe failed"
	$LFS getstripe -c $DIR/$tdir/$tfile
	[ $($LFS getstripe -c $DIR/$tdir/$tfile) -eq 2 ] ||
		error "two-stripe file doesn't have two stripes"

	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=4k count=4 || error "dd failed"
}
run_test 27b "create and write to two stripe file"

test_27d() {
	test_mkdir $DIR/$tdir
	$LFS setstripe -c 0 -i -1 -S 0 $DIR/$tdir/$tfile ||
		error "setstripe failed"
	$CHECKSTAT -t file $DIR/$tdir/$tfile || error "checkstat failed"
	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=4k count=4 || error "dd failed"
}
run_test 27d "create file with default settings"

test_27e() {
	# LU-5839 adds check for existed layout before setting it
	[[ $MDS1_VERSION -lt $(version_code 2.7.56) ]] &&
		skip "Need MDS version at least 2.7.56"

	test_mkdir $DIR/$tdir
	$LFS setstripe -c 2 $DIR/$tdir/$tfile || error "setstripe failed"
	$LFS setstripe -c 2 $DIR/$tdir/$tfile && error "setstripe worked twice"
	$CHECKSTAT -t file $DIR/$tdir/$tfile || error "checkstat failed"
}
run_test 27e "setstripe existing file (should return error)"

test_27f() {
	test_mkdir $DIR/$tdir
	$LFS setstripe -S 100 -i 0 -c 1 $DIR/$tdir/$tfile &&
		error "$SETSTRIPE $DIR/$tdir/$tfile failed"
	$CHECKSTAT -t file $DIR/$tdir/$tfile &&
		error "$CHECKSTAT -t file $DIR/$tdir/$tfile should fail"
	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=4k count=4 || error "dd failed"
	$LFS getstripe $DIR/$tdir/$tfile || error "$LFS getstripe failed"
}
run_test 27f "setstripe with bad stripe size (should return error)"

test_27g() {
	test_mkdir $DIR/$tdir
	$MCREATE $DIR/$tdir/$tfile || error "mcreate failed"
	$LFS getstripe $DIR/$tdir/$tfile 2>&1 | grep "no stripe info" ||
		error "$DIR/$tdir/$tfile has object"
}
run_test 27g "$LFS getstripe with no objects"

test_27ga() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile || error "touch failed"
	ln -s bogus $DIR/$tdir/$tfile.2 || error "ln failed"
	$LFS getstripe -m $DIR/$tdir/$tfile $DIR/$tdir/$tfile.2
	local rc=$?
	(( rc == 2 )) || error "getstripe did not return ENOENT"
}
run_test 27ga "$LFS getstripe with missing file (should return error)"

test_27i() {
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile || error "touch failed"
	[[ $($LFS getstripe -c $DIR/$tdir/$tfile) -gt 0 ]] ||
		error "missing objects"
}
run_test 27i "$LFS getstripe with some objects"

test_27j() {
	test_mkdir $DIR/$tdir
	$LFS setstripe -i $OSTCOUNT $DIR/$tdir/$tfile &&
		error "setstripe failed" || true
}
run_test 27j "setstripe with bad stripe offset (should return error)"

test_27k() { # bug 2844
	test_mkdir $DIR/$tdir
	local file=$DIR/$tdir/$tfile
	local ll_max_blksize=$((4 * 1024 * 1024))
	$LFS setstripe -S 67108864 $file || error "setstripe failed"
	local blksize=$(stat $file | awk '/IO Block:/ { print $7 }')
	[ $blksize -le $ll_max_blksize ] || error "1:$blksize > $ll_max_blksize"
	dd if=/dev/zero of=$file bs=4k count=1
	blksize=$(stat $file | awk '/IO Block:/ { print $7 }')
	[ $blksize -le $ll_max_blksize ] || error "2:$blksize > $ll_max_blksize"
}
run_test 27k "limit i_blksize for broken user apps"

test_27l() {
	mcreate $DIR/$tfile || error "creating file"
	$RUNAS $LFS setstripe -c 1 $DIR/$tfile &&
		error "setstripe should have failed" || true
}
run_test 27l "check setstripe permissions (should return error)"

test_27m() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	ORIGFREE=$($LCTL get_param -n lov.$FSNAME-clilov-*.kbytesavail |
		   head -n1)
	if [[ $ORIGFREE -gt $MAXFREE ]]; then
		skip "$ORIGFREE > $MAXFREE skipping out-of-space test on OST0"
	fi
	trap simple_cleanup_common EXIT
	test_mkdir $DIR/$tdir
	$LFS setstripe -i 0 -c 1 $DIR/$tdir/$tfile.1
	dd if=/dev/zero of=$DIR/$tdir/$tfile.1 bs=1024 count=$MAXFREE &&
		error "dd should fill OST0"
	i=2
	while $LFS setstripe -i 0 -c 1 $DIR/$tdir/$tfile.$i; do
		i=$((i + 1))
		[ $i -gt 256 ] && break
	done
	i=$((i + 1))
	touch $DIR/$tdir/$tfile.$i
	[ $($LFS getstripe $DIR/$tdir/$tfile.$i | grep -A 10 obdidx |
	    awk '{print $1}'| grep -w "0") ] &&
		error "OST0 was full but new created file still use it"
	i=$((i + 1))
	touch $DIR/$tdir/$tfile.$i
	[ $($LFS getstripe $DIR/$tdir/$tfile.$i | grep -A 10 obdidx |
	    awk '{print $1}'| grep -w "0") ] &&
		error "OST0 was full but new created file still use it"
	simple_cleanup_common
}
run_test 27m "create file while OST0 was full"

sleep_maxage() {
	local delay=$(do_facet $SINGLEMDS lctl get_param -n lo[vd].*.qos_maxage |
		      awk '{ print $1 * 2; exit; }')
	sleep $delay
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
	local ofacet=ost$((OSTIDX + 1))

	test_mkdir -p -c1 $DIR/$tdir
	local mdtidx=$($LFS getstripe -m $DIR/$tdir)
	local mfacet=mds$((mdtidx + 1))
	echo OSTIDX=$OSTIDX MDTIDX=$mdtidx

	local OST=$(ostname_from_index $OSTIDX)

	# on the mdt's osc
	local mdtosc_proc1=$(get_mdtosc_proc_path $mfacet $OST)
	local last_id=$(do_facet $mfacet lctl get_param -n \
			osp.$mdtosc_proc1.prealloc_last_id)
	local next_id=$(do_facet $mfacet lctl get_param -n \
			osp.$mdtosc_proc1.prealloc_next_id)

	local mdtosc_proc2=$(get_mdtosc_proc_path $mfacet)
	do_facet $mfacet lctl get_param osp.$mdtosc_proc2.prealloc*

	test_mkdir -p $DIR/$tdir/${OST}
	$SETSTRIPE -i $OSTIDX -c 1 $DIR/$tdir/${OST}
#define OBD_FAIL_OST_ENOSPC              0x215
	do_facet $ofacet lctl set_param fail_val=$FAILIDX fail_loc=0x215
	echo "Creating to objid $last_id on ost $OST..."
	createmany -o $DIR/$tdir/${OST}/f $next_id $((last_id - next_id + 2))
	do_facet $mfacet lctl get_param osp.$mdtosc_proc2.prealloc*
	do_facet $ofacet lctl set_param fail_loc=$FAILLOC
	sleep_maxage
}

exhaust_all_precreations() {
	local i
	for (( i=0; i < OSTCOUNT; i++ )) ; do
		exhaust_precreations $i $1 -1
	done
}

test_27n() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"

	reset_enospc
	rm -f $DIR/$tdir/$tfile
	exhaust_precreations 0 0x80000215
	$LFS setstripe -c -1 $DIR/$tdir || error "setstripe failed"
	touch $DIR/$tdir/$tfile || error "touch failed"
	$LFS getstripe $DIR/$tdir/$tfile
	reset_enospc
}
run_test 27n "create file with some full OSTs"

test_27o() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"

	reset_enospc
	rm -f $DIR/$tdir/$tfile
	exhaust_all_precreations 0x215

	touch $DIR/$tdir/$tfile && error "able to create $DIR/$tdir/$tfile"

	reset_enospc
	rm -rf $DIR/$tdir/*
}
run_test 27o "create file with all full OSTs (should error)"

test_27p() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"

	reset_enospc
	rm -f $DIR/$tdir/$tfile
	test_mkdir $DIR/$tdir

	$MCREATE $DIR/$tdir/$tfile || error "mcreate failed"
	$TRUNCATE $DIR/$tdir/$tfile 80000000 || error "truncate failed"
	$CHECKSTAT -s 80000000 $DIR/$tdir/$tfile || error "checkstat failed"

	exhaust_precreations 0 0x80000215
	echo foo >> $DIR/$tdir/$tfile || error "append failed"
	$CHECKSTAT -s 80000004 $DIR/$tdir/$tfile || error "checkstat failed"
	$LFS getstripe $DIR/$tdir/$tfile

	reset_enospc
}
run_test 27p "append to a truncated file with some full OSTs"

test_27q() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"

	reset_enospc
	rm -f $DIR/$tdir/$tfile

	test_mkdir $DIR/$tdir
	$MCREATE $DIR/$tdir/$tfile || error "mcreate $DIR/$tdir/$tfile failed"
	$TRUNCATE $DIR/$tdir/$tfile 80000000 ||
		error "truncate $DIR/$tdir/$tfile failed"
	$CHECKSTAT -s 80000000 $DIR/$tdir/$tfile || error "checkstat failed"

	exhaust_all_precreations 0x215

	echo foo >> $DIR/$tdir/$tfile && error "append succeeded"
	$CHECKSTAT -s 80000000 $DIR/$tdir/$tfile || error "checkstat 2 failed"

	reset_enospc
}
run_test 27q "append to truncated file with all OSTs full (should error)"

test_27r() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"

	reset_enospc
	rm -f $DIR/$tdir/$tfile
	exhaust_precreations 0 0x80000215

	$LFS setstripe -i 0 -c 2 $DIR/$tdir/$tfile || error "setstripe failed"

	reset_enospc
}
run_test 27r "stripe file with some full OSTs (shouldn't LBUG) ="

test_27s() { # bug 10725
	test_mkdir $DIR/$tdir
	local stripe_size=$((4096 * 1024 * 1024))	# 2^32
	local stripe_count=0
	[ $OSTCOUNT -eq 1 ] || stripe_count=2
	$LFS setstripe -S $stripe_size -c $stripe_count $DIR/$tdir &&
		error "stripe width >= 2^32 succeeded" || true

}
run_test 27s "lsm_xfersize overflow (should error) (bug 10725)"

test_27t() { # bug 10864
	WDIR=$(pwd)
	WLFS=$(which lfs)
	cd $DIR
	touch $tfile
	$WLFS getstripe $tfile
	cd $WDIR
}
run_test 27t "check that utils parse path correctly"

test_27u() { # bug 4900
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local index
	local list=$(comma_list $(mdts_nodes))

#define OBD_FAIL_MDS_OSC_PRECREATE      0x139
	do_nodes $list $LCTL set_param fail_loc=0x139
	test_mkdir -p $DIR/$tdir
	trap simple_cleanup_common EXIT
	createmany -o $DIR/$tdir/t- 1000
	do_nodes $list $LCTL set_param fail_loc=0

	TLOG=$TMP/$tfile.getstripe
	$LFS getstripe $DIR/$tdir > $TLOG
	OBJS=$(awk -vobj=0 '($1 == 0) { obj += 1 } END { print obj; }' $TLOG)
	unlinkmany $DIR/$tdir/t- 1000
	trap 0
	[[ $OBJS -gt 0 ]] &&
		error "$OBJS objects created on OST-0. See $TLOG" ||
		rm -f $TLOG
}
run_test 27u "skip object creation on OSC w/o objects"

test_27v() { # bug 4900
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"

	exhaust_all_precreations 0x215
	reset_enospc

	$LFS setstripe -c 1 $DIR/$tdir         # 1 stripe / file

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
run_test 27v "skip object creation on slow OST"

test_27w() { # bug 10997
	test_mkdir $DIR/$tdir
	$LFS setstripe -S 65536 $DIR/$tdir/f0 || error "setstripe failed"
	[ $($LFS getstripe -S $DIR/$tdir/f0) -ne 65536 ] &&
		error "stripe size $size != 65536" || true
	[ $($LFS getstripe -d $DIR/$tdir | grep -c "stripe_count") -eq 0 ] &&
		error "$LFS getstripe -d $DIR/$tdir no 'stripe_count'" || true
}
run_test 27w "check $LFS setstripe -S and getstrip -d options"

test_27wa() {
	[[ $OSTCOUNT -lt 2 ]] &&
		skip_env "skipping multiple stripe count/offset test"

	test_mkdir $DIR/$tdir
	for i in $(seq 1 $OSTCOUNT); do
		offset=$((i - 1))
		$LFS setstripe -c $i -i $offset $DIR/$tdir/f$i ||
			error "setstripe -c $i -i $offset failed"
		count=$($LFS getstripe -c $DIR/$tdir/f$i)
		index=$($LFS getstripe -i $DIR/$tdir/f$i)
		[ $count -ne $i ] && error "stripe count $count != $i" || true
		[ $index -ne $offset ] &&
			error "stripe offset $index != $offset" || true
	done
}
run_test 27wa "check $LFS setstripe -c -i options"

test_27x() {
	remote_ost_nodsh && skip "remote OST with nodsh"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	OFFSET=$(($OSTCOUNT - 1))
	OSTIDX=0
	local OST=$(ostname_from_index $OSTIDX)

	test_mkdir $DIR/$tdir
	$LFS setstripe -c 1 $DIR/$tdir	# 1 stripe per file
	do_facet ost$((OSTIDX + 1)) lctl set_param -n obdfilter.$OST.degraded 1
	sleep_maxage
	createmany -o $DIR/$tdir/$tfile $OSTCOUNT
	for i in $(seq 0 $OFFSET); do
		[ $($LFS getstripe $DIR/$tdir/$tfile$i | grep -A 10 obdidx |
			awk '{print $1}' | grep -w "$OSTIDX") ] &&
		error "OST0 was degraded but new created file still use it"
	done
	do_facet ost$((OSTIDX + 1)) lctl set_param -n obdfilter.$OST.degraded 0
}
run_test 27x "create files while OST0 is degraded"

test_27y() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local mdtosc=$(get_mdtosc_proc_path $SINGLEMDS $FSNAME-OST0000)
	local last_id=$(do_facet $SINGLEMDS lctl get_param -n \
		osp.$mdtosc.prealloc_last_id)
	local next_id=$(do_facet $SINGLEMDS lctl get_param -n \
		osp.$mdtosc.prealloc_next_id)
	local fcount=$((last_id - next_id))
	[[ $fcount -eq 0 ]] && skip "not enough space on OST0"
	[[ $fcount -gt $OSTCOUNT ]] && fcount=$OSTCOUNT

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
	test_mkdir $DIR/$tdir
	$LFS setstripe -c 1 $DIR/$tdir      # 1 stripe / file

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
	local stripe_count=0

	# sleep 2*lod_qos_maxage seconds waiting for lod qos to notice osp
	# devices get activated.
	sleep_maxage
	$LFS setstripe -c -1 $DIR/$tfile
	stripe_count=$($LFS getstripe -c $DIR/$tfile)
	rm -f $DIR/$tfile
	[ $stripe_count -ne $OSTCOUNT ] &&
		error "Of $OSTCOUNT OSTs, only $stripe_count is available"
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
		local oid_hex

		log "want: stripe:$stripe_nr ost:$obdidx oid:$oid/$hex seq:$seq"

		seq=$(echo $seq | sed -e "s/^0x//g")
		if [ $seq == 0 ] || [ $(facet_fstype ost$ost) == zfs ]; then
			oid_hex=$(echo $oid)
		else
			oid_hex=$(echo $hex | sed -e "s/^0x//g")
		fi
		local obj_file="O/$seq/d$((oid %32))/$oid_hex"

		local ff=""
		#
		# Don't unmount/remount the OSTs if we don't need to do that.
		# LU-2577 changes filter_fid to be smaller, so debugfs needs
		# update too, until that use mount/ll_decode_filter_fid/mount.
		# Re-enable when debugfs will understand new filter_fid.
		#
		if [ $(facet_fstype ost$ost) == ldiskfs ]; then
			ff=$(do_facet ost$ost "$DEBUGFS -c -R 'stat $obj_file' \
				$dev 2>/dev/null" | grep "parent=")
		fi
		if [ -z "$ff" ]; then
			stop ost$ost
			mount_fstype ost$ost
			ff=$(do_facet ost$ost $LL_DECODE_FILTER_FID \
				$(facet_mntpt ost$ost)/$obj_file)
			unmount_fstype ost$ost
			start ost$ost $dev $OST_MOUNT_OPTS
			clients_up
		fi

		[ -z "$ff" ] && error "$obj_file: no filter_fid info"

		echo "$ff" | sed -e 's#.*objid=#got: objid=#'

		# /mnt/O/0/d23/23: objid=23 seq=0 parent=[0x200000400:0x1e:0x1]
		# fid: objid=23 seq=0 parent=[0x200000400:0x1e:0x0] stripe=1
		#
		# fid: parent=[0x200000400:0x1e:0x0] stripe=1 stripe_count=2 \
		#	stripe_size=1048576 component_id=1 component_start=0 \
		#	component_end=33554432
		local ff_parent=$(sed -e 's/.*parent=.//' <<<$ff)
		local ff_pseq=$(cut -d: -f1 <<<$ff_parent)
		local ff_poid=$(cut -d: -f2 <<<$ff_parent)
		local ff_pstripe
		if grep -q 'stripe=' <<<$ff; then
			ff_pstripe=$(sed -e 's/.*stripe=//' -e 's/ .*//' <<<$ff)
		else
			# $LL_DECODE_FILTER_FID does not print "stripe="; look
			# into f_ver in this case.  See comment on ff_parent.
			ff_pstripe=$(cut -d: -f3 <<<$ff_parent | sed -e 's/]//')
		fi

		# compare lmm_seq and filter_fid->ff_parent.f_seq
		[ $ff_pseq = $lmm_seq ] ||
			error "FF parent SEQ $ff_pseq != $lmm_seq"
		# compare lmm_object_id and filter_fid->ff_parent.f_oid
		[ $ff_poid = $lmm_oid ] ||
			error "FF parent OID $ff_poid != $lmm_oid"
		(($ff_pstripe == $stripe_nr)) ||
			error "FF stripe $ff_pstripe != $stripe_nr"

		stripe_nr=$((stripe_nr + 1))
		[ $CLIENT_VERSION -lt $(version_code 2.9.55) ] &&
			continue
		if grep -q 'stripe_count=' <<<$ff; then
			local ff_scnt=$(sed -e 's/.*stripe_count=//' \
					    -e 's/ .*//' <<<$ff)
			[ $lmm_count = $ff_scnt ] ||
				error "FF stripe count $lmm_count != $ff_scnt"
		fi
	done
}

test_27z() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

	test_mkdir $DIR/$tdir
	$LFS setstripe -c 1 -i 0 -S 64k $DIR/$tdir/$tfile-1 ||
		{ error "setstripe -c -1 failed"; return 1; }
	# We need to send a write to every object to get parent FID info set.
	# This _should_ also work for setattr, but does not currently.
	# touch $DIR/$tdir/$tfile-1 ||
	dd if=/dev/zero of=$DIR/$tdir/$tfile-1 bs=1M count=1 ||
		{ error "dd $tfile-1 failed"; return 2; }
	$LFS setstripe -c -1 -i $((OSTCOUNT - 1)) -S 1M $DIR/$tdir/$tfile-2 ||
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	save_layout_restore_at_exit $MOUNT
	$LFS setstripe -c 0 -i -1 -S 0 $MOUNT
	wait_update $HOSTNAME "$GETSTRIPE -c $MOUNT | sed 's/  *//g'" "1" 20 ||
		error "stripe count $($GETSTRIPE -c $MOUNT) != 1"
	local default_size=$($GETSTRIPE -S $MOUNT)
	local default_offset=$($GETSTRIPE -i $MOUNT)
	local dsize=$(do_facet $SINGLEMDS \
		"$LCTL get_param -n lod.$(facet_svc $SINGLEMDS)*.stripesize")
	[ $default_size -eq $dsize ] ||
		error "stripe size $default_size != $dsize"
	[ $default_offset -eq -1 ] ||
		error "stripe offset $default_offset != -1"
}
run_test 27A "check filesystem-wide default LOV EA values"

test_27B() { # LU-2523
	test_mkdir $DIR/$tdir
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
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	declare -a ost_idx
	local index
	local found
	local i
	local j

	test_mkdir $DIR/$tdir
	cd $DIR/$tdir
	for i in $(seq 0 $((OSTCOUNT - 1))); do
		# set stripe across all OSTs starting from OST$i
		$SETSTRIPE -i $i -c -1 $tfile$i
		# get striping information
		ost_idx=($($GETSTRIPE $tfile$i |
		         tail -n $((OSTCOUNT + 1)) | awk '{print $1}'))
		echo ${ost_idx[@]}

		# check the layout
		[ ${#ost_idx[@]} -eq $OSTCOUNT ] ||
			error "${#ost_idx[@]} != $OSTCOUNT"

		for index in $(seq 0 $((OSTCOUNT - 1))); do
			found=0
			for j in $(echo ${ost_idx[@]}); do
				if [ $index -eq $j ]; then
					found=1
					break
				fi
			done
			[ $found = 1 ] ||
				error "Can not find $index in ${ost_idx[@]}"
		done
	done
}
run_test 27C "check full striping across all OSTs"

test_27D() {
	[ $OSTCOUNT -lt 2 ] && skip_env "needs >= 2 OSTs"
	[ -n "$FILESET" ] && skip "SKIP due to FILESET set"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local POOL=${POOL:-testpool}
	local first_ost=0
	local last_ost=$(($OSTCOUNT - 1))
	local ost_step=1
	local ost_list=$(seq $first_ost $ost_step $last_ost)
	local ost_range="$first_ost $last_ost $ost_step"

	test_mkdir $DIR/$tdir
	pool_add $POOL || error "pool_add failed"
	pool_add_targets $POOL $ost_range || error "pool_add_targets failed"

	local skip27D
	[ $MDS1_VERSION -lt $(version_code 2.8.55) ] &&
		skip27D+="-s 29"
	[ $MDS1_VERSION -lt $(version_code 2.9.55) -o \
	  $CLIENT_VERSION -lt $(version_code 2.9.55) ] &&
		skip27D+=" -s 30,31"
	llapi_layout_test -d$DIR/$tdir -p$POOL -o$OSTCOUNT $skip27D ||
		error "llapi_layout_test failed"

	destroy_test_pools || error "destroy test pools failed"
}
run_test 27D "validate llapi_layout API"

# Verify that default_easize is increased from its initial value after
# accessing a widely striped file.
test_27E() {
	[ $OSTCOUNT -lt 2 ] && skip_env "needs >= 2 OSTs"
	[ $CLIENT_VERSION -lt $(version_code 2.5.57) ] &&
		skip "client does not have LU-3338 fix"

	# 72 bytes is the minimum space required to store striping
	# information for a file striped across one OST:
	# (sizeof(struct lov_user_md_v3) +
	#  sizeof(struct lov_user_ost_data_v1))
	local min_easize=72
	$LCTL set_param -n llite.*.default_easize $min_easize ||
		error "lctl set_param failed"
	local easize=$($LCTL get_param -n llite.*.default_easize)

	[ $easize -eq $min_easize ] ||
		error "failed to set default_easize"

	$LFS setstripe -c $OSTCOUNT $DIR/$tfile ||
		error "setstripe failed"
	cat $DIR/$tfile
	rm $DIR/$tfile

	easize=$($LCTL get_param -n llite.*.default_easize)

	[ $easize -gt $min_easize ] ||
		error "default_easize not updated"
}
run_test 27E "check that default extended attribute size properly increases"

test_27F() { # LU-5346/LU-7975
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $OSTCOUNT -lt 2 ]] && skip "needs >= 2 OSTs"
	[[ $MDS1_VERSION -lt $(version_code 2.8.51) ]] &&
		skip "Need MDS version at least 2.8.51"
	remote_ost_nodsh && skip "remote OST with nodsh"

	test_mkdir $DIR/$tdir
	rm -f $DIR/$tdir/f0
	$SETSTRIPE -c 2 $DIR/$tdir

	# stop all OSTs to reproduce situation for LU-7975 ticket
	for num in $(seq $OSTCOUNT); do
		stop ost$num
	done

	# open/create f0 with O_LOV_DELAY_CREATE
	# truncate f0 to a non-0 size
	# close
	multiop $DIR/$tdir/f0 oO_RDWR:O_CREAT:O_LOV_DELAY_CREATE:T1050000c

	$CHECKSTAT -s 1050000 $DIR/$tdir/f0 || error "checkstat failed"
	# open/write it again to force delayed layout creation
	cat /etc/hosts > $DIR/$tdir/f0 &
	catpid=$!

	# restart OSTs
	for num in $(seq $OSTCOUNT); do
		start ost$num $(ostdevname $num) $OST_MOUNT_OPTS ||
			error "ost$num failed to start"
	done

	wait $catpid || error "cat failed"

	cmp /etc/hosts $DIR/$tdir/f0 || error "cmp failed"
	[[ $($GETSTRIPE -c $DIR/$tdir/f0) == 2 ]] || error "wrong stripecount"

}
run_test 27F "Client resend delayed layout creation with non-zero size"

test_27G() { #LU-10629
	[ $MDS1_VERSION -lt $(version_code 2.11.51) ] &&
		skip "Need MDS version at least 2.11.51"
	[ -n "$FILESET" ] && skip "SKIP due to FILESET set"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	local POOL=${POOL:-testpool}
	local ostrange="0 0 1"

	test_mkdir $DIR/$tdir
	pool_add $POOL || error "pool_add failed"
	pool_add_targets $POOL $ostrange || error "pool_add_targets failed"
	$LFS setstripe -p $POOL $DIR/$tdir

	local pool=$($LFS getstripe -p $DIR/$tdir)

	[ "$pool" = "$POOL" ] || error "Striping failed got '$pool' not '$POOL'"

	$LFS setstripe -d $DIR/$tdir

	pool=$($LFS getstripe -p $DIR/$tdir)

	rmdir $DIR/$tdir

	[ -z "$pool" ] || error "'$pool' is not empty"
}
run_test 27G "Clear OST pool from stripe"

test_27H() {
	[[ $MDS1_VERSION -le $(version_code 2.11.54) ]] &&
		skip "Need MDS version newer than 2.11.54"
	[[ $OSTCOUNT -lt 3 ]] && skip_env "needs >= 3 OSTs"
	test_mkdir $DIR/$tdir
	$LFS setstripe -o 0 -o 2 $DIR/$tdir || error "setstripe failed"
	touch $DIR/$tdir/$tfile
	$LFS getstripe -c $DIR/$tdir/$tfile
	[ $($LFS getstripe -c $DIR/$tdir/$tfile) -eq 2 ] ||
		error "two-stripe file doesn't have two stripes"

	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=4k count=4 || error "dd failed"
	$LFS getstripe -y $DIR/$tdir/$tfile
	(( $($LFS getstripe -y $DIR/$tdir/$tfile |
	     egrep -c "l_ost_idx: [02]$") == "2" )) ||
		error "expected l_ost_idx: [02]$ not matched"

	# make sure ost list have been cleared
	local stripesize=$($GETSTRIPE -S $DIR/$tdir)
	$LFS setstripe -S $((stripesize * 4)) -i 1 \
		-c $((OSTCOUNT - 1)) $DIR/$tdir || error "setstripe"
	touch $DIR/$tdir/f3
	$LVERIFY $DIR/$tdir $DIR/$tdir/f3 || error "lverify failed"
}
run_test 27H "Set specific OSTs stripe"

test_27M() {
	[[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.12.57) ]] &&
		skip "Need MDS version >= than 2.12.57"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "need > 1 OST"

	test_mkdir $DIR/$tdir

	# Set default striping on directory
	$LFS setstripe -C 4 $DIR/$tdir

	echo 1 > $DIR/$tdir/${tfile}.1
	local count=$($LFS getstripe -c $DIR/$tdir/${tfile}.1)
	local setcount=4
	[ $count -eq $setcount ] ||
		error "(1) stripe count $count, should be $setcount"

	# Capture existing append_stripe_count setting for restore
	local orig_count=$(do_facet mds1 $LCTL get_param -n mdd.$FSNAME-MDT0000.append_stripe_count)
	local mdts=$(comma_list $(mdts_nodes))
	stack_trap "do_nodes $mdts $LCTL set_param mdd.*.append_stripe_count=$orig_count" EXIT

	local appendcount=$orig_count
	echo 1 >> $DIR/$tdir/${tfile}.2_append
	count=$($LFS getstripe -c $DIR/$tdir/${tfile}.2_append)
	[ $count -eq $appendcount ] ||
		error "(2)stripe count $count, should be $appendcount for append"

	# Disable O_APPEND striping, verify it works
	do_nodes $mdts $LCTL set_param mdd.*.append_stripe_count=0

	# Should now get the default striping, which is 4
	setcount=4
	echo 1 >> $DIR/$tdir/${tfile}.3_append
	count=$($LFS getstripe -c $DIR/$tdir/${tfile}.3_append)
	[ $count -eq $setcount ] ||
		error "(3) stripe count $count, should be $setcount"

	# Try changing the stripe count for append files
	do_nodes $mdts $LCTL set_param mdd.*.append_stripe_count=2

	# Append striping is now 2 (directory default is still 4)
	appendcount=2
	echo 1 >> $DIR/$tdir/${tfile}.4_append
	count=$($LFS getstripe -c $DIR/$tdir/${tfile}.4_append)
	[ $count -eq $appendcount ] ||
		error "(4) stripe count $count, should be $appendcount for append"

	# Test append stripe count of -1
	do_nodes $mdts $LCTL set_param mdd.*.append_stripe_count=-1
	appendcount=$OSTCOUNT
	echo 1 >> $DIR/$tdir/${tfile}.5
	count=$($LFS getstripe -c $DIR/$tdir/${tfile}.5)
	[ $count -eq $appendcount ] ||
		error "(5) stripe count $count, should be $appendcount for append"

	# Set append striping back to default of 1
	do_nodes $mdts $LCTL set_param mdd.*.append_stripe_count=1

	# Try a new default striping, PFL + DOM
	$LFS setstripe -L mdt -E 1M -E -1 -c 2 $DIR/$tdir

	# Create normal DOM file, DOM returns stripe count == 0
	setcount=0
	touch $DIR/$tdir/${tfile}.6
	count=$($LFS getstripe -c $DIR/$tdir/${tfile}.6)
	[ $count -eq $setcount ] ||
		error "(6) stripe count $count, should be $setcount"

	# Show
	appendcount=1
	echo 1 >> $DIR/$tdir/${tfile}.7_append
	count=$($LFS getstripe -c $DIR/$tdir/${tfile}.7_append)
	[ $count -eq $appendcount ] ||
		error "(7) stripe count $count, should be $appendcount for append"

	# Clean up DOM layout
	$LFS setstripe -d $DIR/$tdir

	# Now test that append striping works when layout is from root
	$LFS setstripe -c 2 $MOUNT
	# Make a special directory for this
	mkdir $DIR/${tdir}/${tdir}.2
	stack_trap "$LFS setstripe -d $MOUNT" EXIT

	# Verify for normal file
	setcount=2
	echo 1 > $DIR/${tdir}/${tdir}.2/${tfile}.8
	count=$($LFS getstripe -c $DIR/$tdir/${tdir}.2/${tfile}.8)
	[ $count -eq $setcount ] ||
		error "(8) stripe count $count, should be $setcount"

	appendcount=1
	echo 1 >> $DIR/${tdir}/${tdir}.2/${tfile}.9_append
	count=$($LFS getstripe -c $DIR/${tdir}/${tdir}.2/${tfile}.9_append)
	[ $count -eq $appendcount ] ||
		error "(9) stripe count $count, should be $appendcount for append"

	# Now test O_APPEND striping with pools
	do_nodes $mdts $LCTL set_param mdd.*.append_pool="$TESTNAME"
	stack_trap "do_nodes $mdts $LCTL set_param mdd.*.append_pool='none'" EXIT

	# Create the pool
	pool_add $TESTNAME || error "pool creation failed"
	pool_add_targets $TESTNAME 0 1 || error "Pool add targets failed"

	echo 1 >> $DIR/$tdir/${tfile}.10_append

	pool=$($LFS getstripe -p $DIR/$tdir/${tfile}.10_append)
	[ "$pool" = "$TESTNAME" ] || error "(10) incorrect pool: $pool"

	# Check that count is still correct
	appendcount=1
	echo 1 >> $DIR/$tdir/${tfile}.11_append
	count=$($LFS getstripe -c $DIR/$tdir/${tfile}.11_append)
	[ $count -eq $appendcount ] ||
		error "(11) stripe count $count, should be $appendcount for append"

	# Disable O_APPEND stripe count, verify pool works separately
	do_nodes $mdts $LCTL set_param mdd.*.append_stripe_count=0

	echo 1 >> $DIR/$tdir/${tfile}.12_append

	pool=$($LFS getstripe -p $DIR/$tdir/${tfile}.12_append)
	[ "$pool" = "$TESTNAME" ] || error "(12) incorrect pool: $pool"

	# Remove pool setting, verify it's not applied
	do_nodes $mdts $LCTL set_param mdd.*.append_pool='none'

	echo 1 >> $DIR/$tdir/${tfile}.13_append

	pool=$($LFS getstripe -p $DIR/$tdir/${tfile}.13_append)
	[ "$pool" = "" ] || error "(13) pool found: $pool"
}
run_test 27M "test O_APPEND striping"

# createtest also checks that device nodes are created and
# then visible correctly (#2091)
test_28() { # bug 2091
	test_mkdir $DIR/d28
	$CREATETEST $DIR/d28/ct || error "createtest failed"
}
run_test 28 "create/mknod/mkdir with bad file types ============"

test_29() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	sync; sleep 1; sync # flush out any dirty pages from previous tests
	cancel_lru_locks
	test_mkdir $DIR/d29
	touch $DIR/d29/foo
	log 'first d29'
	ls -l $DIR/d29

	declare -i LOCKCOUNTORIG=0
	for lock_count in $(lctl get_param -n ldlm.namespaces.*mdc*.lock_count); do
		let LOCKCOUNTORIG=$LOCKCOUNTORIG+$lock_count
	done
	[ $LOCKCOUNTORIG -eq 0 ] && error "No mdc lock count" && return 1

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

	if [[ $LOCKCOUNTCURRENT -gt $LOCKCOUNTORIG ]]; then
		$LCTL set_param -n ldlm.dump_namespaces ""
		error "CURRENT: $LOCKCOUNTCURRENT > $LOCKCOUNTORIG"
		$LCTL dk | sort -k4 -t: > $TMP/test_29.dk
		log "dumped log to $TMP/test_29.dk (bug 5793)"
		return 2
	fi
	if [[ $LOCKUNUSEDCOUNTCURRENT -gt $LOCKUNUSEDCOUNTORIG ]]; then
		error "UNUSED: $LOCKUNUSEDCOUNTCURRENT > $LOCKUNUSEDCOUNTORIG"
		$LCTL dk | sort -k4 -t: > $TMP/test_29.dk
		log "dumped log to $TMP/test_29.dk (bug 5793)"
		return 3
	fi
}
run_test 29 "IT_GETATTR regression  ============================"

test_30a() { # was test_30
	cp $(which ls) $DIR || cp /bin/ls $DIR
	$DIR/ls / || error "Can't execute binary from lustre"
	rm $DIR/ls
}
run_test 30a "execute binary from Lustre (execve) =============="

test_30b() {
	cp `which ls` $DIR || cp /bin/ls $DIR
	chmod go+rx $DIR/ls
	$RUNAS $DIR/ls / || error "Can't execute binary from lustre as non-root"
	rm $DIR/ls
}
run_test 30b "execute binary from Lustre as non-root ==========="

test_30c() { # b=22376
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	cp `which ls` $DIR || cp /bin/ls $DIR
	chmod a-rw $DIR/ls
	cancel_lru_locks mdc
	cancel_lru_locks osc
	$RUNAS $DIR/ls / || error "Can't execute binary from lustre"
	rm -f $DIR/ls
}
run_test 30c "execute binary from Lustre without read perms ===="

test_31a() {
	$OPENUNLINK $DIR/f31 $DIR/f31 || error "openunlink failed"
	$CHECKSTAT -a $DIR/f31 || error "$DIR/f31 exists"
}
run_test 31a "open-unlink file =================================="

test_31b() {
	touch $DIR/f31 || error "touch $DIR/f31 failed"
	ln $DIR/f31 $DIR/f31b || error "ln failed"
	$MULTIOP $DIR/f31b Ouc || error "multiop failed"
	$CHECKSTAT -t file $DIR/f31 || error "$DIR/f31 not file type"
}
run_test 31b "unlink file with multiple links while open ======="

test_31c() {
	touch $DIR/f31 || error "touch $DIR/f31 failed"
	ln $DIR/f31 $DIR/f31c || error "ln failed"
	multiop_bg_pause $DIR/f31 O_uc ||
		error "multiop_bg_pause for $DIR/f31 failed"
	MULTIPID=$!
	$MULTIOP $DIR/f31c Ouc
	kill -USR1 $MULTIPID
	wait $MULTIPID
}
run_test 31c "open-unlink file with multiple links ============="

test_31d() {
	opendirunlink $DIR/d31d $DIR/d31d || error "opendirunlink failed"
	$CHECKSTAT -a $DIR/d31d || error "$DIR/d31d exists"
}
run_test 31d "remove of open directory ========================="

test_31e() { # bug 2904
	openfilleddirunlink $DIR/d31e || error "openfilleddirunlink failed"
}
run_test 31e "remove of open non-empty directory ==============="

test_31f() { # bug 4554
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	test_mkdir -c1 $DIR/${tdir}ga
	test_mkdir -c1 $DIR/${tdir}gb
	touch $DIR/${tdir}ga/f
	ln $DIR/${tdir}ga/f $DIR/${tdir}gb/g
	$CHECKSTAT -t file $DIR/${tdir}ga/f || error "source"
	[ `stat -c%h $DIR/${tdir}ga/f` == '2' ] || error "source nlink"
	$CHECKSTAT -t file $DIR/${tdir}gb/g || error "target"
	[ `stat -c%h $DIR/${tdir}gb/g` == '2' ] || error "target nlink"
}
run_test 31g "cross directory link==============="

test_31h() {
	echo "-- cross directory link --"
	test_mkdir -c1 $DIR/${tdir}
	test_mkdir -c1 $DIR/${tdir}/dir
	touch $DIR/${tdir}/f
	ln $DIR/${tdir}/f $DIR/${tdir}/dir/g
	$CHECKSTAT -t file $DIR/${tdir}/f || error "source"
	[ `stat -c%h $DIR/${tdir}/f` == '2' ] || error "source nlink"
	$CHECKSTAT -t file $DIR/${tdir}/dir/g || error "target"
	[ `stat -c%h $DIR/${tdir}/dir/g` == '2' ] || error "target nlink"
}
run_test 31h "cross directory link under child==============="

test_31i() {
	echo "-- cross directory link --"
	test_mkdir -c1 $DIR/$tdir
	test_mkdir -c1 $DIR/$tdir/dir
	touch $DIR/$tdir/dir/f
	ln $DIR/$tdir/dir/f $DIR/$tdir/g
	$CHECKSTAT -t file $DIR/$tdir/dir/f || error "source"
	[ `stat -c%h $DIR/$tdir/dir/f` == '2' ] || error "source nlink"
	$CHECKSTAT -t file $DIR/$tdir/g || error "target"
	[ `stat -c%h $DIR/$tdir/g` == '2' ] || error "target nlink"
}
run_test 31i "cross directory link under parent==============="

test_31j() {
	test_mkdir -c1 -p $DIR/$tdir
	test_mkdir -c1 -p $DIR/$tdir/dir1
	ln $DIR/$tdir/dir1 $DIR/$tdir/dir2 && error "ln for dir"
	link $DIR/$tdir/dir1 $DIR/$tdir/dir3 && error "link for dir"
	mlink $DIR/$tdir/dir1 $DIR/$tdir/dir4 && error "mlink for dir"
	mlink $DIR/$tdir/dir1 $DIR/$tdir/dir1 && error "mlink to the same dir"
	return 0
}
run_test 31j "link for directory==============="

test_31k() {
        test_mkdir -c1 -p $DIR/$tdir
        touch $DIR/$tdir/s
        touch $DIR/$tdir/exist
        mlink $DIR/$tdir/s $DIR/$tdir/t || error "mlink"
        mlink $DIR/$tdir/s $DIR/$tdir/exist && error "mlink to exist file"
        mlink $DIR/$tdir/s $DIR/$tdir/s && error "mlink to the same file"
        mlink $DIR/$tdir/s $DIR/$tdir && error "mlink to parent dir"
        mlink $DIR/$tdir $DIR/$tdir/s && error "mlink parent dir to target"
        mlink $DIR/$tdir/not-exist $DIR/$tdir/foo && error "mlink non-existing to new"
        mlink $DIR/$tdir/not-exist $DIR/$tdir/s && error "mlink non-existing to exist"
	return 0
}
run_test 31k "link to file: the same, non-existing, dir==============="

test_31m() {
        mkdir $DIR/d31m
        touch $DIR/d31m/s
        mkdir $DIR/d31m2
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
	touch $DIR/$tfile || error "cannot create '$DIR/$tfile'"
	nlink=$(stat --format=%h $DIR/$tfile)
	[ ${nlink:--1} -eq 1 ] || error "nlink is $nlink, expected 1"
	local fd=$(free_fd)
	local cmd="exec $fd<$DIR/$tfile"
	eval $cmd
	cmd="exec $fd<&-"
	trap "eval $cmd" EXIT
	nlink=$(stat --dereference --format=%h /proc/self/fd/$fd)
	[ ${nlink:--1} -eq 1 ] || error "nlink is $nlink, expected 1"
	rm $DIR/$tfile || error "cannot remove '$DIR/$tfile'"
	nlink=$(stat --dereference --format=%h /proc/self/fd/$fd)
	[ ${nlink:--1} -eq 0 ] || error "nlink is $nlink, expected 0"
	eval $cmd
}
run_test 31n "check link count of unlinked file"

link_one() {
	local TEMPNAME=$(mktemp $1_XXXXXX)
	mlink $TEMPNAME $1 2> /dev/null &&
		echo "$BASHPID: link $TEMPNAME to $1 succeeded"
	munlink $TEMPNAME
}

test_31o() { # LU-2901
	test_mkdir $DIR/$tdir
	for LOOP in $(seq 100); do
		rm -f $DIR/$tdir/$tfile*
		for THREAD in $(seq 8); do
			link_one $DIR/$tdir/$tfile.$LOOP &
		done
		wait
		local LINKS=$(ls -1 $DIR/$tdir | grep -c $tfile.$LOOP)
		[[ $LINKS -gt 1 ]] && ls $DIR/$tdir &&
			error "$LINKS duplicate links to $tfile.$LOOP" &&
			break || true
	done
}
run_test 31o "duplicate hard links with same filename"

test_31p() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	test_mkdir $DIR/$tdir
	$LFS setdirstripe -i0 -c2 $DIR/$tdir/striped_dir
	$LFS setdirstripe -D -c2 -H all_char $DIR/$tdir/striped_dir

	opendirunlink $DIR/$tdir/striped_dir/test1 ||
		error "open unlink test1 failed"
	opendirunlink $DIR/$tdir/striped_dir/test2 ||
		error "open unlink test2 failed"

	$CHECKSTAT -a $DIR/$tdir/striped_dir/test1 ||
		error "test1 still exists"
	$CHECKSTAT -a $DIR/$tdir/striped_dir/test2 ||
		error "test2 still exists"
}
run_test 31p "remove of open striped directory"

cleanup_test32_mount() {
	local rc=0
	trap 0
	local loopdev=$(losetup -a | grep $EXT2_DEV | sed -ne 's/:.*$//p')
	$UMOUNT $DIR/$tdir/ext2-mountpoint || rc=$?
	losetup -d $loopdev || true
	rm -rf $DIR/$tdir
	return $rc
}

test_32a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	echo "== more mountpoints and symlinks ================="
	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	$CHECKSTAT -t dir $DIR/$tdir/ext2-mountpoint/.. ||
		error "$DIR/$tdir/ext2-mountpoint/.. not dir type"
	cleanup_test32_mount
}
run_test 32a "stat d32a/ext2-mountpoint/.. ====================="

test_32b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	ls -al $DIR/$tdir/ext2-mountpoint/.. ||
		error "Can't list $DIR/$tdir/ext2-mountpoint/.."
	cleanup_test32_mount
}
run_test 32b "open d32b/ext2-mountpoint/.. ====================="

test_32c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	test_mkdir -p $DIR/$tdir/d2/test_dir
	$CHECKSTAT -t dir $DIR/$tdir/ext2-mountpoint/../d2/test_dir ||
		error "$DIR/$tdir/ext2-mountpoint/../d2/test_dir not dir type"
	cleanup_test32_mount
}
run_test 32c "stat d32c/ext2-mountpoint/../d2/test_dir ========="

test_32d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	test_mkdir -p $DIR/$tdir/d2/test_dir
	ls -al $DIR/$tdir/ext2-mountpoint/../d2/test_dir ||
		error "Can't list $DIR/$tdir/ext2-mountpoint/../d2/test_dir"
	cleanup_test32_mount
}
run_test 32d "open d32d/ext2-mountpoint/../d2/test_dir"

test_32e() {
	rm -fr $DIR/$tdir
	test_mkdir -p $DIR/$tdir/tmp
	local tmp_dir=$DIR/$tdir/tmp
	ln -s $DIR/$tdir $tmp_dir/symlink11
	ln -s $tmp_dir/symlink11 $tmp_dir/../symlink01
	$CHECKSTAT -t link $DIR/$tdir/tmp/symlink11 || error "symlink11 bad"
	$CHECKSTAT -t link $DIR/$tdir/symlink01 || error "symlink01 bad"
}
run_test 32e "stat d32e/symlink->tmp/symlink->lustre-subdir"

test_32f() {
	rm -fr $DIR/$tdir
	test_mkdir -p $DIR/$tdir/tmp
	local tmp_dir=$DIR/$tdir/tmp
	ln -s $DIR/$tdir $tmp_dir/symlink11
	ln -s $tmp_dir/symlink11 $tmp_dir/../symlink01
	ls $DIR/$tdir/tmp/symlink11  || error "symlink11 bad"
	ls $DIR/$tdir/symlink01 || error "symlink01 bad"
}
run_test 32f "open d32f/symlink->tmp/symlink->lustre-subdir"

test_32g() {
	local tmp_dir=$DIR/$tdir/tmp
	test_mkdir -p $tmp_dir
	test_mkdir $DIR/${tdir}2
	ln -s $DIR/${tdir}2 $tmp_dir/symlink12
	ln -s $tmp_dir/symlink12 $tmp_dir/../symlink02
	$CHECKSTAT -t link $tmp_dir/symlink12 || error "symlink12 not a link"
	$CHECKSTAT -t link $DIR/$tdir/symlink02 || error "symlink02 not a link"
	$CHECKSTAT -t dir -f $tmp_dir/symlink12 || error "symlink12 not a dir"
	$CHECKSTAT -t dir -f $DIR/$tdir/symlink02 || error "symlink12 not a dir"
}
run_test 32g "stat d32g/symlink->tmp/symlink->lustre-subdir/${tdir}2"

test_32h() {
	rm -fr $DIR/$tdir $DIR/${tdir}2
	tmp_dir=$DIR/$tdir/tmp
	test_mkdir -p $tmp_dir
	test_mkdir $DIR/${tdir}2
	ln -s $DIR/${tdir}2 $tmp_dir/symlink12
	ln -s $tmp_dir/symlink12 $tmp_dir/../symlink02
	ls $tmp_dir/symlink12 || error "listing symlink12"
	ls $DIR/$tdir/symlink02  || error "listing symlink02"
}
run_test 32h "open d32h/symlink->tmp/symlink->lustre-subdir/${tdir}2"

test_32i() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	touch $DIR/$tdir/test_file
	$CHECKSTAT -t file $DIR/$tdir/ext2-mountpoint/../test_file ||
		error "$DIR/$tdir/ext2-mountpoint/../test_file not file type"
	cleanup_test32_mount
}
run_test 32i "stat d32i/ext2-mountpoint/../test_file ==========="

test_32j() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	touch $DIR/$tdir/test_file
	cat $DIR/$tdir/ext2-mountpoint/../test_file ||
		error "Can't open $DIR/$tdir/ext2-mountpoint/../test_file"
	cleanup_test32_mount
}
run_test 32j "open d32j/ext2-mountpoint/../test_file ==========="

test_32k() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	test_mkdir -p $DIR/$tdir/d2
	touch $DIR/$tdir/d2/test_file || error "touch failed"
	$CHECKSTAT -t file $DIR/$tdir/ext2-mountpoint/../d2/test_file ||
		error "$DIR/$tdir/ext2-mountpoint/../d2/test_file not file type"
	cleanup_test32_mount
}
run_test 32k "stat d32k/ext2-mountpoint/../d2/test_file ========"

test_32l() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	test_mkdir -p $DIR/$tdir/d2
	touch $DIR/$tdir/d2/test_file || error "touch failed"
	cat  $DIR/$tdir/ext2-mountpoint/../d2/test_file ||
		error "Can't open $DIR/$tdir/ext2-mountpoint/../d2/test_file"
	cleanup_test32_mount
}
run_test 32l "open d32l/ext2-mountpoint/../d2/test_file ========"

test_32m() {
	rm -fr $DIR/d32m
	test_mkdir -p $DIR/d32m/tmp
	TMP_DIR=$DIR/d32m/tmp
	ln -s $DIR $TMP_DIR/symlink11
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01
	$CHECKSTAT -t link $DIR/d32m/tmp/symlink11 ||
		error "symlink11 not a link"
	$CHECKSTAT -t link $DIR/d32m/symlink01 ||
		error "symlink01 not a link"
}
run_test 32m "stat d32m/symlink->tmp/symlink->lustre-root ======"

test_32n() {
	rm -fr $DIR/d32n
	test_mkdir -p $DIR/d32n/tmp
	TMP_DIR=$DIR/d32n/tmp
	ln -s $DIR $TMP_DIR/symlink11
	ln -s $TMP_DIR/symlink11 $TMP_DIR/../symlink01
	ls -l $DIR/d32n/tmp/symlink11  || error "listing symlink11"
	ls -l $DIR/d32n/symlink01 || error "listing symlink01"
}
run_test 32n "open d32n/symlink->tmp/symlink->lustre-root ======"

test_32o() {
	touch $DIR/$tfile
	test_mkdir -p $DIR/d32o/tmp
	TMP_DIR=$DIR/d32o/tmp
	ln -s $DIR/$tfile $TMP_DIR/symlink12
	ln -s $TMP_DIR/symlink12 $TMP_DIR/../symlink02
	$CHECKSTAT -t link $DIR/d32o/tmp/symlink12 ||
		error "symlink12 not a link"
	$CHECKSTAT -t link $DIR/d32o/symlink02 || error "symlink02 not a link"
	$CHECKSTAT -t file -f $DIR/d32o/tmp/symlink12 ||
		error "$DIR/d32o/tmp/symlink12 not file type"
	$CHECKSTAT -t file -f $DIR/d32o/symlink02 ||
		error "$DIR/d32o/symlink02 not file type"
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
	cat $DIR/d32p/tmp/symlink12 ||
		error "Can't open $DIR/d32p/tmp/symlink12"
	log 32p_9
	cat $DIR/d32p/symlink02 || error "Can't open $DIR/d32p/symlink02"
	log 32p_10
}
run_test 32p "open d32p/symlink->tmp/symlink->lustre-root/$tfile"

test_32q() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	touch $DIR/$tdir/ext2-mountpoint/under_the_mount || error "touch failed"
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	ls $DIR/$tdir/ext2-mountpoint | grep "\<under_the_mount\>" && error
	cleanup_test32_mount
}
run_test 32q "stat follows mountpoints in Lustre (should return error)"

test_32r() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	[ -e $DIR/$tdir ] && rm -fr $DIR/$tdir
	trap cleanup_test32_mount EXIT
	test_mkdir -p $DIR/$tdir/ext2-mountpoint
	touch $DIR/$tdir/ext2-mountpoint/under_the_mount || error "touch failed"
	mount -t ext2 -o loop $EXT2_DEV $DIR/$tdir/ext2-mountpoint ||
		error "mount failed for $EXT2_DEV $DIR/$tdir/ext2-mountpoint"
	ls $DIR/$tdir/ext2-mountpoint | grep -q under_the_mount && error || true
	cleanup_test32_mount
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
run_test 33aa "write file with mode 444 (should return error)"

test_33a() {
	rm -fr $DIR/$tdir
	test_mkdir $DIR/$tdir
	chown $RUNAS_ID $DIR/$tdir
	$RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $DIR/$tdir/$tfile ||
		error "$RUNAS create $tdir/$tfile failed"
	$RUNAS $OPENFILE -f O_RDWR:O_CREAT -m 0444 $DIR/$tdir/$tfile &&
		error "open RDWR" || true
}
run_test 33a "test open file(mode=0444) with O_RDWR (should return error)"

test_33b() {
	rm -fr $DIR/$tdir
	test_mkdir $DIR/$tdir
	chown $RUNAS_ID $DIR/$tdir
	$RUNAS $OPENFILE -f 1286739555 $DIR/$tdir/$tfile || true
}
run_test 33b "test open file with malformed flags (No panic)"

test_33c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

	local ostnum
	local ostname
	local write_bytes
	local all_zeros

	all_zeros=:
	rm -fr $DIR/$tdir
	test_mkdir $DIR/$tdir
	# Read: 0, Write: 4, create/destroy: 2/0, stat: 1, punch: 0

        sync
        for ostnum in $(seq $OSTCOUNT); do
                # test-framework's OST numbering is one-based, while Lustre's
                # is zero-based
                ostname=$(printf "$FSNAME-OST%.4x" $((ostnum - 1)))
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
	echo foo > $DIR/$tdir/bar
	# Really write them
	sync

        # Total up write_bytes after writing.  We'd better find non-zeros.
        for ostnum in $(seq $OSTCOUNT); do
                ostname=$(printf "$FSNAME-OST%.4x" $((ostnum - 1)))
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
                        ostname=$(printf "$FSNAME-OST%.4x" $((ostnum - 1)))
                        echo "Check that write_bytes is present in obdfilter/*/stats:"
                        do_facet ost$ostnum lctl get_param -n \
                                obdfilter/$ostname/stats
                done
                error "OST not keeping write_bytes stats (b22312)"
        fi
}
run_test 33c "test llobdstat and write_bytes"

test_33d() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local MDTIDX=1
	local remote_dir=$DIR/$tdir/remote_dir

	test_mkdir $DIR/$tdir
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
	$RUNAS $OPENFILE -f 1286739555 $remote_dir/f33 || true
}
run_test 33d "openfile with 444 modes and malformed flags under remote dir"

test_33e() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	mkdir $DIR/$tdir

	$LFS setdirstripe -i0 -c2 $DIR/$tdir/striped_dir
	$LFS setdirstripe -i1 -c2 $DIR/$tdir/striped_dir1
	mkdir $DIR/$tdir/local_dir

	local s0_mode=$(stat -c%f $DIR/$tdir/striped_dir)
	local s1_mode=$(stat -c%f $DIR/$tdir/striped_dir1)
	local l_mode=$(stat -c%f $DIR/$tdir/local_dir)

	[ "$l_mode" = "$s0_mode" -a "$l_mode" = "$s1_mode" ] ||
		error "mkdir $l_mode striped0 $s0_mode striped1 $s1_mode"

	rmdir $DIR/$tdir/* || error "rmdir failed"

	umask 777
	$LFS setdirstripe -i0 -c2 $DIR/$tdir/striped_dir
	$LFS setdirstripe -i1 -c2 $DIR/$tdir/striped_dir1
	mkdir $DIR/$tdir/local_dir

	s0_mode=$(stat -c%f $DIR/$tdir/striped_dir)
	s1_mode=$(stat -c%f $DIR/$tdir/striped_dir1)
	l_mode=$(stat -c%f $DIR/$tdir/local_dir)

	[ "$l_mode" = "$s0_mode" -a "$l_mode" = "$s1_mode" ] ||
		error "mkdir $l_mode striped0 $s0_mode striped1 $s1_mode 777"

	rmdir $DIR/$tdir/* || error "rmdir(umask 777) failed"

	umask 000
	$LFS setdirstripe -i0 -c2 $DIR/$tdir/striped_dir
	$LFS setdirstripe -i1 -c2 $DIR/$tdir/striped_dir1
	mkdir $DIR/$tdir/local_dir

	s0_mode=$(stat -c%f $DIR/$tdir/striped_dir)
	s1_mode=$(stat -c%f $DIR/$tdir/striped_dir1)
	l_mode=$(stat -c%f $DIR/$tdir/local_dir)

	[ "$l_mode" = "$s0_mode" -a "$l_mode" = "$s1_mode" ] ||
		error "mkdir $l_mode striped0 $s0_mode striped1 $s1_mode 0"
}
run_test 33e "mkdir and striped directory should have same mode"

cleanup_33f() {
	trap 0
	do_facet $SINGLEMDS $LCTL set_param mdt.*.enable_remote_dir_gid=0
}

test_33f() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	mkdir $DIR/$tdir
	chmod go+rwx $DIR/$tdir
	do_facet $SINGLEMDS $LCTL set_param mdt.*.enable_remote_dir_gid=-1
	trap cleanup_33f EXIT

	$RUNAS lfs mkdir -i 0 -c$MDSCOUNT $DIR/$tdir/striped_dir ||
		error "cannot create striped directory"

	$RUNAS touch $DIR/$tdir/striped_dir/{0..16} ||
		error "cannot create files in striped directory"

	$RUNAS rm $DIR/$tdir/striped_dir/{0..16} ||
		error "cannot remove files in striped directory"

	$RUNAS rmdir $DIR/$tdir/striped_dir ||
		error "cannot remove striped directory"

	cleanup_33f
}
run_test 33f "nonroot user can create, access, and remove a striped directory"

test_33g() {
	mkdir -p $DIR/$tdir/dir2

	local err=$($RUNAS mkdir $DIR/$tdir/dir2 2>&1)
	echo $err
	[[ $err =~ "exists" ]] || error "Not exists error"
}
run_test 33g "nonroot user create already existing root created file"

TEST_34_SIZE=${TEST_34_SIZE:-2000000000000}
test_34a() {
	rm -f $DIR/f34
	$MCREATE $DIR/f34 || error "mcreate failed"
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" ||
		error "getstripe failed"
	$TRUNCATE $DIR/f34 $TEST_34_SIZE || error "truncate failed"
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" ||
		error "getstripe failed"
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 ||
		error "Size of $DIR/f34 not equal to $TEST_34_SIZE bytes"
}
run_test 34a "truncate file that has not been opened ==========="

test_34b() {
	[ ! -f $DIR/f34 ] && test_34a
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 ||
		error "Size of $DIR/f34 not equal to $TEST_34_SIZE bytes"
	$OPENFILE -f O_RDONLY $DIR/f34
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" ||
		error "getstripe failed"
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 ||
		error "Size of $DIR/f34 not equal to $TEST_34_SIZE bytes"
}
run_test 34b "O_RDONLY opening file doesn't create objects ====="

test_34c() {
	[ ! -f $DIR/f34 ] && test_34a
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 ||
		error "Size of $DIR/f34 not equal to $TEST_34_SIZE bytes"
	$OPENFILE -f O_RDWR $DIR/f34
	$GETSTRIPE $DIR/f34 2>&1 | grep -q "no stripe info" && error
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 ||
		error "Size of $DIR/f34 not equal to $TEST_34_SIZE bytes"
}
run_test 34c "O_RDWR opening file-with-size works =============="

test_34d() {
	[ ! -f $DIR/f34 ] && test_34a
	dd if=/dev/zero of=$DIR/f34 conv=notrunc bs=4k count=1 ||
		error "dd failed"
	$CHECKSTAT -s $TEST_34_SIZE $DIR/f34 ||
		error "Size of $DIR/f34 not equal to $TEST_34_SIZE bytes"
	rm $DIR/f34
}
run_test 34d "write to sparse file ============================="

test_34e() {
	rm -f $DIR/f34e
	$MCREATE $DIR/f34e || error "mcreate failed"
	$TRUNCATE $DIR/f34e 1000 || error "truncate failed"
	$CHECKSTAT -s 1000 $DIR/f34e ||
		error "Size of $DIR/f34e not equal to 1000 bytes"
	$OPENFILE -f O_RDWR $DIR/f34e
	$CHECKSTAT -s 1000 $DIR/f34e ||
		error "Size of $DIR/f34e not equal to 1000 bytes"
}
run_test 34e "create objects, some with size and some without =="

test_34f() { # bug 6242, 6243
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	SIZE34F=48000
	rm -f $DIR/f34f
	$MCREATE $DIR/f34f || error "mcreate failed"
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	dd if=/dev/zero of=$DIR/$tfile bs=1 count=100 seek=$TEST_34_SIZE ||
		error "dd failed"
	$TRUNCATE $DIR/$tfile $((TEST_34_SIZE / 2))|| error "truncate failed"
	$CHECKSTAT -s $((TEST_34_SIZE / 2)) $DIR/$tfile ||
		error "Size of $DIR/$tfile not equal to $((TEST_34_SIZE / 2))"
	cancel_lru_locks osc
	$CHECKSTAT -s $((TEST_34_SIZE / 2)) $DIR/$tfile ||
		error "wrong size after lock cancel"

	$TRUNCATE $DIR/$tfile $TEST_34_SIZE || error "truncate failed"
	$CHECKSTAT -s $TEST_34_SIZE $DIR/$tfile ||
		error "expanding truncate failed"
	cancel_lru_locks osc
	$CHECKSTAT -s $TEST_34_SIZE $DIR/$tfile ||
		error "wrong expanded size after lock cancel"
}
run_test 34g "truncate long file ==============================="

test_34h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local gid=10
	local sz=1000

	dd if=/dev/zero of=$DIR/$tfile bs=1M count=10 || error "dd failed"
	sync # Flush the cache so that multiop below does not block on cache
	     # flush when getting the group lock
	$MULTIOP $DIR/$tfile OG${gid}T${sz}g${gid}c &
	MULTIPID=$!

	# Since just timed wait is not good enough, let's do a sync write
	# that way we are sure enough time for a roundtrip + processing
	# passed + 2 seconds of extra margin.
	dd if=/dev/zero of=$DIR/${tfile}-1 bs=$PAGE_SIZE oflag=direct count=1
	rm $DIR/${tfile}-1
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
run_test 35a "exec file with mode 444 (should return and not leak)"

test_36a() {
	rm -f $DIR/f36
	utime $DIR/f36 || error "utime failed for MDS"
}
run_test 36a "MDS utime check (mknod, utime)"

test_36b() {
	echo "" > $DIR/f36
	utime $DIR/f36 || error "utime failed for OST"
}
run_test 36b "OST utime check (open, utime)"

test_36c() {
	rm -f $DIR/d36/f36
	test_mkdir $DIR/d36
	chown $RUNAS_ID $DIR/d36
	$RUNAS utime $DIR/d36/f36 || error "utime failed for MDS as non-root"
}
run_test 36c "non-root MDS utime check (mknod, utime)"

test_36d() {
	[ ! -d $DIR/d36 ] && test_36c
	echo "" > $DIR/d36/f36
	$RUNAS utime $DIR/d36/f36 || error "utime failed for OST as non-root"
}
run_test 36d "non-root OST utime check (open, utime)"

test_36e() {
	[ $RUNAS_ID -eq $UID ] && skip_env "RUNAS_ID = UID = $UID -- skipping"

	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/$tfile
	$RUNAS utime $DIR/$tdir/$tfile &&
		error "utime worked, expected failure" || true
}
run_test 36e "utime on non-owned file (should return error)"

subr_36fh() {
	local fl="$1"
	local LANG_SAVE=$LANG
	local LC_LANG_SAVE=$LC_LANG
	export LANG=C LC_LANG=C # for date language

	DATESTR="Dec 20  2000"
	test_mkdir $DIR/$tdir
	lctl set_param fail_loc=$fl
	date; date +%s
	cp /etc/hosts $DIR/$tdir/$tfile
	sync & # write RPC generated with "current" inode timestamp, but delayed
	sleep 1
	touch --date="$DATESTR" $DIR/$tdir/$tfile # setattr timestamp in past
	LS_BEFORE="`ls -l $DIR/$tdir/$tfile`" # old timestamp from client cache
	cancel_lru_locks $OSC
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	#define OBD_FAIL_OST_BRW_PAUSE_BULK 0x214
	subr_36fh "0x80000214"
}
run_test 36f "utime on file racing with OST BRW write =========="

test_36g() {
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDS1_VERSION -lt $(version_code 2.12.51) ] &&
		skip "Need MDS version at least 2.12.51"

	local fmd_max_age
	local fmd
	local facet="ost1"
	local tgt="obdfilter"

	[[ $OSC == "mdc" ]] && tgt="mdt" && facet="mds1"

	test_mkdir $DIR/$tdir
	fmd_max_age=$(do_facet $facet \
		"lctl get_param -n $tgt.*.tgt_fmd_seconds 2> /dev/null | \
		head -n 1")

	echo "FMD max age: ${fmd_max_age}s"
	touch $DIR/$tdir/$tfile
	fmd=$(do_facet $facet "lctl get_param -n $tgt.*.exports.*.fmd_count" |
		gawk '{cnt=cnt+$1}  END{print cnt}')
	echo "FMD before: $fmd"
	[[ $fmd == 0 ]] &&
		error "FMD wasn't create by touch"
	sleep $((fmd_max_age + 12))
	fmd=$(do_facet $facet "lctl get_param -n $tgt.*.exports.*.fmd_count" |
		gawk '{cnt=cnt+$1}  END{print cnt}')
	echo "FMD after: $fmd"
	[[ $fmd == 0 ]] ||
		error "FMD wasn't expired by ping"
}
run_test 36g "FMD cache expiry ====================="

test_36h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	#define OBD_FAIL_OST_BRW_PAUSE_BULK2 0x227
	subr_36fh "0x80000227"
}
run_test 36h "utime on file racing with OST BRW write =========="

test_36i() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	test_mkdir $DIR/$tdir
	$LFS setdirstripe -i0 -c$MDSCOUNT $DIR/$tdir/striped_dir

	local mtime=$(stat -c%Y $DIR/$tdir/striped_dir)
	local new_mtime=$((mtime + 200))

	#change Modify time of striped dir
	touch -m -d @$new_mtime	$DIR/$tdir/striped_dir ||
			error "change mtime failed"

	local got=$(stat -c%Y $DIR/$tdir/striped_dir)

	[ "$new_mtime" = "$got" ] || error "expect $new_mtime got $got"
}
run_test 36i "change mtime on striped directory"

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

test_39a() { # was test_39
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
run_test 39a "mtime changed on create"

test_39b() {
	test_mkdir -c1 $DIR/$tdir
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

		cancel_lru_locks $OSC
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

		cancel_lru_locks $OSC
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39c "mtime change on rename ==========================="

# bug 21114
test_39d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	touch $DIR1/$tfile
	touch -m -d @$TEST_39_MTIME $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local mtime=`stat -c %Y $DIR1/$tfile`
		[ $mtime = $TEST_39_MTIME ] || \
			error "mtime($mtime) is not set to $TEST_39_MTIME"

		cancel_lru_locks $OSC
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39d "create, utime, stat =============================="

# bug 21114
test_39e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	touch $DIR1/$tfile
	local mtime1=`stat -c %Y $DIR1/$tfile`

	touch -m -d @$TEST_39_MTIME $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile`
		[ $mtime2 = $TEST_39_MTIME ] || \
			error "mtime($mtime2) is not set to $TEST_39_MTIME"

		cancel_lru_locks $OSC
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39e "create, stat, utime, stat ========================"

# bug 21114
test_39f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	touch $DIR1/$tfile
	mtime1=`stat -c %Y $DIR1/$tfile`

	sleep 2
	touch -m -d @$TEST_39_MTIME $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile`
		[ $mtime2 = $TEST_39_MTIME ] || \
			error "mtime($mtime2) is not set to $TEST_39_MTIME"

		cancel_lru_locks $OSC
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39f "create, stat, sleep, utime, stat ================="

# bug 11063
test_39g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	echo hello >> $DIR1/$tfile
	local mtime1=`stat -c %Y $DIR1/$tfile`

	sleep 2
	chmod o+r $DIR1/$tfile

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile`
		[ "$mtime1" = "$mtime2" ] || \
			error "lost mtime: $mtime2, should be $mtime1"

		cancel_lru_locks $OSC
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39g "write, chmod, stat ==============================="

# bug 11063
test_39h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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

			cancel_lru_locks $OSC
			if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
		done
	fi
}
run_test 39h "write, utime within one second, stat ============="

test_39i() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	touch $DIR1/$tfile
	sleep 1

	echo hello >> $DIR1/$tfile
	local mtime1=`stat -c %Y $DIR1/$tfile`

	mv $DIR1/$tfile $DIR1/$tfile-1

	for (( i=0; i < 2; i++ )) ; do
		local mtime2=`stat -c %Y $DIR1/$tfile-1`

		[ "$mtime1" = "$mtime2" ] || \
			error "lost mtime: $mtime2, should be $mtime1"

		cancel_lru_locks $OSC
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39i "write, rename, stat =============================="

test_39j() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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

		cancel_lru_locks $OSC
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
	lctl set_param fail_loc=0
	stop_full_debug_logging
}
run_test 39j "write, rename, close, stat ======================="

test_39k() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local atime_diff=$(do_facet $SINGLEMDS \
				lctl get_param -n mdd.*MDT0000*.atime_diff)
	rm -rf $DIR/$tdir
	mkdir -p $DIR/$tdir

	# test setting directory atime to future
	touch -a -d @$TEST_39_ATIME $DIR/$tdir
	local atime=$(stat -c %X $DIR/$tdir)
	[ "$atime" = $TEST_39_ATIME ] ||
		error "atime is not set to future: $atime, $TEST_39_ATIME"

	# test setting directory atime from future to now
	local now=$(date +%s)
	touch -a -d @$now $DIR/$tdir

	atime=$(stat -c %X $DIR/$tdir)
	[ "$atime" -eq "$now"  ] ||
		error "atime is not updated from future: $atime, $now"

	do_facet $SINGLEMDS lctl set_param -n mdd.*MDT0000*.atime_diff=2
	sleep 3

	# test setting directory atime when now > dir atime + atime_diff
	local d1=$(date +%s)
	ls $DIR/$tdir
	local d2=$(date +%s)
	cancel_lru_locks mdc
	atime=$(stat -c %X $DIR/$tdir)
	[ "$atime" -ge "$d1" -a "$atime" -le "$d2" ] ||
		error "atime is not updated  : $atime, should be $d2"

	do_facet $SINGLEMDS lctl set_param -n mdd.*MDT0000*.atime_diff=60
	sleep 3

	# test not setting directory atime when now < dir atime + atime_diff
	ls $DIR/$tdir
	cancel_lru_locks mdc
	atime=$(stat -c %X $DIR/$tdir)
	[ "$atime" -ge "$d1" -a "$atime" -le "$d2" ] ||
		error "atime is updated to $atime, should remain $d1<atime<$d2"

	do_facet $SINGLEMDS \
		lctl set_param -n mdd.*MDT0000*.atime_diff=$atime_diff
}
run_test 39l "directory atime update ==========================="

test_39m() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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

		cancel_lru_locks $OSC
		if [ $i = 0 ] ; then echo "repeat after cancel_lru_locks"; fi
	done
}
run_test 39m "test atime and mtime before 1970"

test_39n() { # LU-3832
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local atime_diff=$(do_facet $SINGLEMDS \
		lctl get_param -n mdd.*MDT0000*.atime_diff)
	local atime0
	local atime1
	local atime2

	do_facet $SINGLEMDS lctl set_param -n mdd.*MDT0000*.atime_diff=1

	rm -rf $DIR/$tfile
	dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1 status=noxfer
	atime0=$(stat -c %X $DIR/$tfile)

	sleep 5
	$MULTIOP $DIR/$tfile oO_RDONLY:O_NOATIME:r4096c
	atime1=$(stat -c %X $DIR/$tfile)

	sleep 5
	cancel_lru_locks mdc
	cancel_lru_locks osc
	$MULTIOP $DIR/$tfile oO_RDONLY:O_NOATIME:r4096c
	atime2=$(stat -c %X $DIR/$tfile)

	do_facet $SINGLEMDS \
		lctl set_param -n mdd.*MDT0000*.atime_diff=$atime_diff

	[ "$atime0" -eq "$atime1" ] || error "atime0 $atime0 != atime1 $atime1"
	[ "$atime1" -eq "$atime2" ] || error "atime0 $atime0 != atime1 $atime1"
}
run_test 39n "check that O_NOATIME is honored"

test_39o() {
	TESTDIR=$DIR/$tdir/$tfile
	[ -e $TESTDIR ] && rm -rf $TESTDIR
	mkdir -p $TESTDIR
	cd $TESTDIR
	links1=2
	ls
	mkdir a b
	ls
	links2=$(stat -c %h .)
	[ $(($links1 + 2)) != $links2 ] &&
		error "wrong links count $(($links1 + 2)) != $links2"
	rmdir b
	links3=$(stat -c %h .)
	[ $(($links1 + 1)) != $links3 ] &&
		error "wrong links count $links1 != $links3"
	return 0
}
run_test 39o "directory cached attributes updated after create"

test_39p() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"

	local MDTIDX=1
	TESTDIR=$DIR/$tdir/$tdir
	[ -e $TESTDIR ] && rm -rf $TESTDIR
	test_mkdir -p $TESTDIR
	cd $TESTDIR
	links1=2
	ls
	test_mkdir -i $MDTIDX $TESTDIR/remote_dir1
	test_mkdir -i $MDTIDX $TESTDIR/remote_dir2
	ls
	links2=$(stat -c %h .)
	[ $(($links1 + 2)) != $links2 ] &&
		error "wrong links count $(($links1 + 2)) != $links2"
	rmdir remote_dir2
	links3=$(stat -c %h .)
	[ $(($links1 + 1)) != $links3 ] &&
		error "wrong links count $links1 != $links3"
	return 0
}
run_test 39p "remote directory cached attributes updated after create ========"


test_39q() { # LU-8041
	local testdir=$DIR/$tdir
	mkdir -p $testdir
	multiop_bg_pause $testdir D_c || error "multiop failed"
	local multipid=$!
	cancel_lru_locks mdc
	kill -USR1 $multipid
	local atime=$(stat -c %X $testdir)
	[ "$atime" -ne 0 ] || error "atime is zero"
}
run_test 39q "close won't zero out atime"

test_40() {
	dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1
	$RUNAS $OPENFILE -f O_WRONLY:O_TRUNC $DIR/$tfile &&
		error "openfile O_WRONLY:O_TRUNC $tfile failed"
	$CHECKSTAT -t file -s 4096 $DIR/$tfile ||
		error "$tfile is not 4096 bytes in size"
}
run_test 40 "failed open(O_TRUNC) doesn't truncate ============="

test_41() {
	# bug 1553
	small_write $DIR/f41 18
}
run_test 41 "test small file write + fstat ====================="

count_ost_writes() {
	lctl get_param -n ${OSC}.*.stats |
		awk -vwrites=0 '/ost_write/ { writes += $2 } \
			END { printf("%0.0f", writes) }'
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	setup_test42
	cancel_lru_locks $OSC
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
run_test 42a "ensure that we don't flush on close"

test_42b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	setup_test42
	cancel_lru_locks $OSC
	stop_writeback
	sync
	dd if=/dev/zero of=$DIR/f42b bs=1024 count=100
	BEFOREWRITES=$(count_ost_writes)
	$MUNLINK $DIR/f42b || error "$MUNLINK $DIR/f42b: $?"
	AFTERWRITES=$(count_ost_writes)
	if [[ $BEFOREWRITES -lt $AFTERWRITES ]]; then
		error "$BEFOREWRITES < $AFTERWRITES on unlink"
	fi
	BEFOREWRITES=$(count_ost_writes)
	sync || error "sync: $?"
	AFTERWRITES=$(count_ost_writes)
	if [[ $BEFOREWRITES -lt $AFTERWRITES ]]; then
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
	cancel_lru_locks $OSC
	stop_writeback
	# prime the file with 0,EOF PW to match
	touch $file
        $TRUNCATE $file 0
        sync; sync
	# now the real test..
	dd if=/dev/zero of=$file bs=1024 count=100
	BEFOREWRITES=`count_ost_writes`
	$TRUNCATE $file $offset
	cancel_lru_locks $OSC
	AFTERWRITES=`count_ost_writes`
	start_writeback
}

test_42c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	trunc_test 42c 1024
	[ $BEFOREWRITES -eq $AFTERWRITES ] &&
		error "beforewrites $BEFOREWRITES == afterwrites $AFTERWRITES on truncate"
	rm $file
}
run_test 42c "test partial truncate of file with cached dirty data"

test_42d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	trunc_test 42d 0
	[ $BEFOREWRITES -eq $AFTERWRITES ] ||
		error "beforewrites $BEFOREWRITES != afterwrites $AFTERWRITES on truncate"
	rm $file
}
run_test 42d "test complete truncate of file with cached dirty data"

test_42e() { # bug22074
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local TDIR=$DIR/${tdir}e
	local pages=16 # hardcoded 16 pages, don't change it.
	local files=$((OSTCOUNT * 500)) # hopefully 500 files on each OST
	local proc_osc0="osc.${FSNAME}-OST0000-osc-[^MDT]*"
	local max_dirty_mb
	local warmup_files

	test_mkdir $DIR/${tdir}e
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
	[[ $i -gt $warmup_files ]] && error "OST0 is still cold"
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
		dd if=/dev/zero of=$TDIR/f$i bs=$PAGE_SIZE count=$pages 2>/dev/null
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
			[[ $(echo $PPR | tr -d ':') -ge $pages ]] && break

                        percent=$((percent + WPCT))
			if [[ $percent -gt 15 ]]; then
                                error "less than 16-pages write RPCs" \
                                      "$percent% > 15%"
                                break
                        fi
                done
        rm -rf $TDIR
}
run_test 42e "verify sub-RPC writes are not done synchronously"

test_43A() { # was test_43
	test_mkdir $DIR/$tdir
	cp -p /bin/ls $DIR/$tdir/$tfile
	$MULTIOP $DIR/$tdir/$tfile Ow_c &
	pid=$!
	# give multiop a chance to open
	sleep 1

	$DIR/$tdir/$tfile && error "execute $DIR/$tdir/$tfile succeeded" || true
	kill -USR1 $pid
}
run_test 43A "execution of file opened for write should return -ETXTBSY"

test_43a() {
	test_mkdir $DIR/$tdir
	cp -p $(which sleep) $DIR/$tdir/sleep || error "can't copy"
	$DIR/$tdir/sleep 60 &
	SLEEP_PID=$!
	# Make sure exec of $tdir/sleep wins race with truncate
	sleep 1
	$MULTIOP $DIR/$tdir/sleep Oc && error "expected error, got success"
	kill $SLEEP_PID
}
run_test 43a "open(RDWR) of file being executed should return -ETXTBSY"

test_43b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	cp -p $(which sleep) $DIR/$tdir/sleep || error "can't copy"
	$DIR/$tdir/sleep 60 &
	SLEEP_PID=$!
	# Make sure exec of $tdir/sleep wins race with truncate
	sleep 1
	$TRUNCATE $DIR/$tdir/sleep 0 && error "expected error, got success"
	kill $SLEEP_PID
}
run_test 43b "truncate of file being executed should return -ETXTBSY"

test_43c() {
	local testdir="$DIR/$tdir"
	test_mkdir $testdir
	cp $SHELL $testdir/
	( cd $(dirname $SHELL) && md5sum $(basename $SHELL) ) |
		( cd $testdir && md5sum -c )
}
run_test 43c "md5sum of copy into lustre"

test_44A() { # was test_44
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	dd if=/dev/zero of=$DIR/f1 bs=4k count=1 seek=1023
	dd if=$DIR/f1 bs=4k count=1 > /dev/null
}
run_test 44A "zero length read from a sparse stripe"

test_44a() {
	local nstripe=$($LCTL lov_getconfig $DIR | grep default_stripe_count: |
		awk '{ print $2 }')
	[ -z "$nstripe" ] && skip "can't get stripe info"
	[[ $nstripe -gt $OSTCOUNT ]] &&
		skip "Wrong default_stripe_count: $nstripe OSTCOUNT: $OSTCOUNT"

	local stride=$($LCTL lov_getconfig $DIR | grep default_stripe_size: |
		awk '{ print $2 }')
	if [[ $nstripe -eq 0 || $nstripe -eq -1 ]]; then
		nstripe=$($LCTL lov_getconfig $DIR | grep obd_count: |
			awk '{ print $2 }')
	fi

	OFFSETS="0 $((stride/2)) $((stride-1))"
	for offset in $OFFSETS; do
		for i in $(seq 0 $((nstripe-1))); do
			local GLOBALOFFSETS=""
			# size in Bytes
			local size=$((((i + 2 * $nstripe )*$stride + $offset)))
			local myfn=$DIR/d44a-$size
			echo "--------writing $myfn at $size"
			ll_sparseness_write $myfn $size ||
				error "ll_sparseness_write"
			GLOBALOFFSETS="$GLOBALOFFSETS $size"
			ll_sparseness_verify $myfn $GLOBALOFFSETS ||
				error "ll_sparseness_verify $GLOBALOFFSETS"

			for j in $(seq 0 $((nstripe-1))); do
				# size in Bytes
				size=$((((j + $nstripe )*$stride + $offset)))
				ll_sparseness_write $myfn $size ||
					error "ll_sparseness_write"
				GLOBALOFFSETS="$GLOBALOFFSETS $size"
			done
			ll_sparseness_verify $myfn $GLOBALOFFSETS ||
				error "ll_sparseness_verify $GLOBALOFFSETS"
			rm -f $myfn
		done
	done
}
run_test 44a "test sparse pwrite ==============================="

dirty_osc_total() {
	tot=0
	for d in `lctl get_param -n ${OSC}.*.cur_dirty_bytes`; do
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	f="$DIR/f45"
	# Obtain grants from OST if it supports it
	echo blah > ${f}_grant
	stop_writeback
	sync
	do_dirty_record "echo blah > $f"
	[[ $before -eq $after ]] && error "write wasn't cached"
	do_dirty_record "> $f"
	[[ $before -gt $after ]] || error "truncate didn't lower dirty count"
	do_dirty_record "echo blah > $f"
	[[ $before -eq $after ]] && error "write wasn't cached"
	do_dirty_record "sync"
	[[ $before -gt $after ]] || error "writeback didn't lower dirty count"
	do_dirty_record "echo blah > $f"
	[[ $before -eq $after ]] && error "write wasn't cached"
	do_dirty_record "cancel_lru_locks osc"
	[[ $before -gt $after ]] ||
		error "lock cancellation didn't lower dirty count"
	start_writeback
}
run_test 45 "osc io page accounting ============================"

# in a 2 stripe file (lov.sh), page 1023 maps to page 511 in its object.  this
# test tickles a bug where re-dirtying a page was failing to be mapped to the
# objects offset and an assert hit when an rpc was built with 1023's mapped
# offset 511 and 511's raw 511 offset. it also found general redirtying bugs.
test_46() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	f="$DIR/f46"
	stop_writeback
	sync
	dd if=/dev/zero of=$f bs=$PAGE_SIZE seek=511 count=1
	sync
	dd conv=notrunc if=/dev/zero of=$f bs=$PAGE_SIZE seek=1023 count=1
	dd conv=notrunc if=/dev/zero of=$f bs=$PAGE_SIZE seek=511 count=1
	sync
	start_writeback
}
run_test 46 "dirtying a previously written page ================"

# test_47 is removed "Device nodes check" is moved to test_28

test_48a() { # bug 2399
	[ "$mds1_FSTYPE" = "zfs" ] &&
	[ $MDS1_VERSION -lt $(version_code 2.3.63) ] &&
		skip "MDS prior to 2.3.63 handle ZFS dir .. incorrectly"

	test_mkdir $DIR/$tdir
	cd $DIR/$tdir
	mv $DIR/$tdir $DIR/$tdir.new || error "move directory failed"
	test_mkdir $DIR/$tdir
	touch foo || error "'touch foo' failed after recreating cwd"
	test_mkdir bar
	touch .foo || error "'touch .foo' failed after recreating cwd"
	test_mkdir .bar
	ls . > /dev/null || error "'ls .' failed after recreating cwd"
	ls .. > /dev/null || error "'ls ..' failed after removing cwd"
	cd . || error "'cd .' failed after recreating cwd"
	mkdir . && error "'mkdir .' worked after recreating cwd"
	rmdir . && error "'rmdir .' worked after recreating cwd"
	ln -s . baz || error "'ln -s .' failed after recreating cwd"
	cd .. || error "'cd ..' failed after recreating cwd"
}
run_test 48a "Access renamed working dir (should return errors)="

test_48b() { # bug 2399
	rm -rf $DIR/$tdir
	test_mkdir $DIR/$tdir
	cd $DIR/$tdir
	rmdir $DIR/$tdir || error "remove cwd $DIR/$tdir failed"
	touch foo && error "'touch foo' worked after removing cwd"
	mkdir foo && error "'mkdir foo' worked after removing cwd"
	touch .foo && error "'touch .foo' worked after removing cwd"
	mkdir .foo && error "'mkdir .foo' worked after removing cwd"
	ls . > /dev/null && error "'ls .' worked after removing cwd"
	ls .. > /dev/null || error "'ls ..' failed after removing cwd"
	mkdir . && error "'mkdir .' worked after removing cwd"
	rmdir . && error "'rmdir .' worked after removing cwd"
	ln -s . foo && error "'ln -s .' worked after removing cwd"
	cd .. || echo "'cd ..' failed after removing cwd `pwd`"  #bug 3517
}
run_test 48b "Access removed working dir (should return errors)="

test_48c() { # bug 2350
	#lctl set_param debug=-1
	#set -vx
	rm -rf $DIR/$tdir
	test_mkdir -p $DIR/$tdir/dir
	cd $DIR/$tdir/dir
	$TRACE rmdir $DIR/$tdir/dir || error "remove cwd $DIR/$tdir/dir failed"
	$TRACE touch foo && error "touch foo worked after removing cwd"
	$TRACE mkdir foo && error "'mkdir foo' worked after removing cwd"
	touch .foo && error "touch .foo worked after removing cwd"
	mkdir .foo && error "mkdir .foo worked after removing cwd"
	$TRACE ls . && error "'ls .' worked after removing cwd"
	$TRACE ls .. || error "'ls ..' failed after removing cwd"
	$TRACE mkdir . && error "'mkdir .' worked after removing cwd"
	$TRACE rmdir . && error "'rmdir .' worked after removing cwd"
	$TRACE ln -s . foo && error "'ln -s .' worked after removing cwd"
	$TRACE cd .. || echo "'cd ..' failed after removing cwd `pwd`" #bug 3415
}
run_test 48c "Access removed working subdir (should return errors)"

test_48d() { # bug 2350
	#lctl set_param debug=-1
	#set -vx
	rm -rf $DIR/$tdir
	test_mkdir -p $DIR/$tdir/dir
	cd $DIR/$tdir/dir
	$TRACE rmdir $DIR/$tdir/dir || error "remove cwd $DIR/$tdir/dir failed"
	$TRACE rmdir $DIR/$tdir || error "remove parent $DIR/$tdir failed"
	$TRACE touch foo && error "'touch foo' worked after removing parent"
	$TRACE mkdir foo && error "mkdir foo worked after removing parent"
	touch .foo && error "'touch .foo' worked after removing parent"
	mkdir .foo && error "mkdir .foo worked after removing parent"
	$TRACE ls . && error "'ls .' worked after removing parent"
	$TRACE ls .. && error "'ls ..' worked after removing parent"
	$TRACE mkdir . && error "'mkdir .' worked after removing parent"
	$TRACE rmdir . && error "'rmdir .' worked after removing parent"
	$TRACE ln -s . foo && error "'ln -s .' worked after removing parent"
	true
}
run_test 48d "Access removed parent subdir (should return errors)"

test_48e() { # bug 4134
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

	# get ost1 size - lustre-OST0000
	ost1_size=$(do_facet ost1 $LFS df | grep ${ost1_svc} |
		awk '{ print $4 }')
	# write 800M at maximum
	[[ $ost1_size -lt 2 ]] && ost1_size=2
	[[ $ost1_size -gt 819200 ]] && ost1_size=819200

	$SETSTRIPE -c 1 -i 0 $DIR/$tfile
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=$((ost1_size >> 2)) &
	local dd_pid=$!

	# change max_pages_per_rpc while writing the file
	local osc1_mppc=osc.$(get_osc_import_name client ost1).max_pages_per_rpc
	local orig_mppc=$($LCTL get_param -n $osc1_mppc)
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
	ls /proc/$$/cwd || error "ls /proc/$$/cwd failed"
}
run_test 50 "special situations: /proc symlinks  ==============="

test_51a() {	# was test_51
	# bug 1516 - create an empty entry right after ".." then split dir
	test_mkdir -c1 $DIR/$tdir
	touch $DIR/$tdir/foo
	$MCREATE $DIR/$tdir/bar
	rm $DIR/$tdir/foo
	createmany -m $DIR/$tdir/longfile 201
	FNUM=202
	while [[ $(ls -sd $DIR/$tdir | awk '{ print $1 }') -eq 4 ]]; do
		$MCREATE $DIR/$tdir/longfile$FNUM
		FNUM=$(($FNUM + 1))
		echo -n "+"
	done
	echo
	ls -l $DIR/$tdir > /dev/null || error "ls -l $DIR/$tdir failed"
}
run_test 51a "special situations: split htree with empty entry =="

cleanup_print_lfs_df () {
	trap 0
	$LFS df
	$LFS df -i
}

test_51b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local dir=$DIR/$tdir
	local nrdirs=$((65536 + 100))

	# cleanup the directory
	rm -fr $dir

	test_mkdir -c1 $dir

	$LFS df
	$LFS df -i
	local mdtidx=$(printf "%04x" $($LFS getstripe -m $dir))
	local numfree=$(lctl get_param -n mdc.$FSNAME-MDT$mdtidx*.filesfree)
	[[ $numfree -lt $nrdirs ]] &&
		skip "not enough free inodes ($numfree) on MDT$mdtidx"

	# need to check free space for the directories as well
	local blkfree=$(lctl get_param -n mdc.$FSNAME-MDT$mdtidx*.kbytesavail)
	numfree=$(( blkfree / $(fs_inode_ksize) ))
	[[ $numfree -lt $nrdirs ]] && skip "not enough blocks ($numfree)"

	trap cleanup_print_lfs_df EXIT

	# create files
	createmany -d $dir/d $nrdirs || {
		unlinkmany $dir/d $nrdirs
		error "failed to create $nrdirs subdirs in MDT$mdtidx:$dir"
	}

	# really created :
	nrdirs=$(ls -U $dir | wc -l)

	# unlink all but 100 subdirectories, then check it still works
	local left=100
	local delete=$((nrdirs - left))

	$LFS df
	$LFS df -i

	# for ldiskfs the nlink count should be 1, but this is OSD specific
	# and so this is listed for informational purposes only
	echo "nlink before: $(stat -c %h $dir), created before: $nrdirs"
	unlinkmany -d $dir/d $delete ||
		error "unlink of first $delete subdirs failed"

	echo "nlink between: $(stat -c %h $dir)"
	local found=$(ls -U $dir | wc -l)
	[ $found -ne $left ] &&
		error "can't find subdirs: found only $found, expected $left"

	unlinkmany -d $dir/d $delete $left ||
		error "unlink of second $left subdirs failed"
	# regardless of whether the backing filesystem tracks nlink accurately
	# or not, the nlink count shouldn't be more than "." and ".." here
	local after=$(stat -c %h $dir)
	[[ $after -gt 2 ]] && error "nlink after: $after > 2" ||
		echo "nlink after: $after"

	cleanup_print_lfs_df
}
run_test 51b "exceed 64k subdirectory nlink limit on create, verify unlink"

test_51d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $OSTCOUNT -lt 3 ]] && skip_env "needs >= 3 OSTs"

	test_mkdir $DIR/$tdir
	createmany -o $DIR/$tdir/t- 1000
	$LFS getstripe $DIR/$tdir > $TMP/$tfile
	for N in $(seq 0 $((OSTCOUNT - 1))); do
		OBJS[$N]=$(awk -vobjs=0 '($1 == '$N') { objs += 1 } \
			END { printf("%0.0f", objs) }' $TMP/$tfile)
		OBJS0[$N]=$(grep -A 1 idx $TMP/$tfile | awk -vobjs=0 \
			'($1 == '$N') { objs += 1 } \
			END { printf("%0.0f", objs) }')
		log "OST$N has ${OBJS[$N]} objects, ${OBJS0[$N]} are index 0"
	done
	unlinkmany $DIR/$tdir/t- 1000

	NLAST=0
	for N in $(seq 1 $((OSTCOUNT - 1))); do
		[[ ${OBJS[$N]} -lt $((${OBJS[$NLAST]} - 20)) ]] &&
			error "OST $N has less objects vs OST $NLAST" \
			      " (${OBJS[$N]} < ${OBJS[$NLAST]}"
		[[ ${OBJS[$N]} -gt $((${OBJS[$NLAST]} + 20)) ]] &&
			error "OST $N has less objects vs OST $NLAST" \
			      " (${OBJS[$N]} < ${OBJS[$NLAST]}"

		[[ ${OBJS0[$N]} -lt $((${OBJS0[$NLAST]} - 20)) ]] &&
			error "OST $N has less #0 objects vs OST $NLAST" \
			      " (${OBJS0[$N]} < ${OBJS0[$NLAST]}"
		[[ ${OBJS0[$N]} -gt $((${OBJS0[$NLAST]} + 20)) ]] &&
			error "OST $N has less #0 objects vs OST $NLAST" \
			      " (${OBJS0[$N]} < ${OBJS0[$NLAST]}"
		NLAST=$N
	done
	rm -f $TMP/$tfile
}
run_test 51d "check object distribution"

test_51e() {
	if [ "$mds1_FSTYPE" != ldiskfs ]; then
		skip_env "ldiskfs only test"
	fi

	test_mkdir -c1 $DIR/$tdir
	test_mkdir -c1 $DIR/$tdir/d0

	touch $DIR/$tdir/d0/foo
	createmany -l $DIR/$tdir/d0/foo $DIR/$tdir/d0/f- 65001 &&
		error "file exceed 65000 nlink limit!"
	unlinkmany $DIR/$tdir/d0/f- 65001
	return 0
}
run_test 51e "check file nlink limit"

test_51f() {
	test_mkdir $DIR/$tdir

	local max=100000
	local ulimit_old=$(ulimit -n)
	local spare=20 # number of spare fd's for scripts/libraries, etc.
	local mdt=$($LFS getstripe -m $DIR/$tdir)
	local numfree=$($LFS df -i $DIR/$tdir | awk '/MDT:'$mdt'/ { print $4 }')

	echo "MDT$mdt numfree=$numfree, max=$max"
	[[ $numfree -gt $max ]] && numfree=$max || numfree=$((numfree * 7 / 8))
	if [ $((numfree + spare)) -gt $ulimit_old ]; then
		while ! ulimit -n $((numfree + spare)); do
			numfree=$((numfree * 3 / 4))
		done
		echo "changed ulimit from $ulimit_old to $((numfree + spare))"
	else
		echo "left ulimit at $ulimit_old"
	fi

	createmany -o -k -t 120 $DIR/$tdir/f $numfree || {
		unlinkmany $DIR/$tdir/f $numfree
		error "create+open $numfree files in $DIR/$tdir failed"
	}
	ulimit -n $ulimit_old

	# if createmany exits at 120s there will be fewer than $numfree files
	unlinkmany $DIR/$tdir/f $numfree || true
}
run_test 51f "check many open files limit"

test_52a() {
	[ -f $DIR/$tdir/foo ] && chattr -a $DIR/$tdir/foo
	test_mkdir $DIR/$tdir
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
	cp -r $DIR/$tdir $TMP/
	rm -fr $DIR/$tdir $TMP/$tdir || error "cleanup rm failed"
}
run_test 52a "append-only flag test (should return errors)"

test_52b() {
	[ -f $DIR/$tdir/foo ] && chattr -i $DIR/$tdir/foo
	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/foo
	chattr +i $DIR/$tdir/foo || error "chattr +i failed"
	cat test > $DIR/$tdir/foo && error "cat test worked"
	cp /etc/hosts $DIR/$tdir/foo && error "cp worked"
	rm -f $DIR/$tdir/foo 2>/dev/null && error "rm worked"
	link $DIR/$tdir/foo $DIR/$tdir/foo_link 2>/dev/null &&
					error "link worked"
	echo foo >> $DIR/$tdir/foo && error "echo worked"
	mrename $DIR/$tdir/foo $DIR/$tdir/foo_ren && error "rename worked"
	[ -f $DIR/$tdir/foo ] || error "$tdir/foo is not a file"
	[ -f $DIR/$tdir/foo_ren ] && error "$tdir/foo_ren is not a file"
	lsattr $DIR/$tdir/foo | egrep -q "^-+i[-e]+ $DIR/$tdir/foo" ||
							error "lsattr"
	chattr -i $DIR/$tdir/foo || error "chattr failed"

	rm -fr $DIR/$tdir || error "unable to remove $DIR/$tdir"
}
run_test 52b "immutable flag test (should return errors) ======="

test_53() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"

	local param
	local param_seq
	local ostname
	local mds_last
	local mds_last_seq
	local ost_last
	local ost_last_seq
	local ost_last_id
	local ostnum
	local node
	local found=false
	local support_last_seq=true

	[[ $MDS1_VERSION -ge $(version_code 2.3.60) ]] ||
		support_last_seq=false

	# only test MDT0000
	local mdtosc=$(get_mdtosc_proc_path $SINGLEMDS)
	local value
	for value in $(do_facet $SINGLEMDS \
		       $LCTL get_param osp.$mdtosc.prealloc_last_id) ; do
		param=$(echo ${value[0]} | cut -d "=" -f1)
		ostname=$(echo $param | cut -d "." -f2 | cut -d - -f 1-2)

		if $support_last_seq; then
			param_seq=$(echo $param |
				sed -e s/prealloc_last_id/prealloc_last_seq/g)
			mds_last_seq=$(do_facet $SINGLEMDS \
				       $LCTL get_param -n $param_seq)
		fi
		mds_last=$(do_facet $SINGLEMDS $LCTL get_param -n $param)

		ostnum=$(index_from_ostuuid ${ostname}_UUID)
		node=$(facet_active_host ost$((ostnum+1)))
		param="obdfilter.$ostname.last_id"
		for ost_last in $(do_node $node $LCTL get_param -n $param) ; do
			echo "$ostname.last_id=$ost_last; MDS.last_id=$mds_last"
			ost_last_id=$ost_last

			if $support_last_seq; then
				ost_last_id=$(echo $ost_last |
					      awk -F':' '{print $2}' |
					      sed -e "s/^0x//g")
				ost_last_seq=$(echo $ost_last |
					       awk -F':' '{print $1}')
				[[ $ost_last_seq = $mds_last_seq ]] || continue
			fi

			if [[ $ost_last_id != $mds_last ]]; then
				error "$ost_last_id != $mds_last"
			else
				found=true
				break
			fi
		done
	done
	$found || error "can not match last_seq/last_id for $mdtosc"
	return 0
}
run_test 53 "verify that MDS and OSTs agree on pre-creation ===="

test_54a() {
	perl -MSocket -e ';' || skip "no Socket perl module installed"

	$SOCKETSERVER $DIR/socket ||
		error "$SOCKETSERVER $DIR/socket failed: $?"
	$SOCKETCLIENT $DIR/socket ||
		error "$SOCKETCLIENT $DIR/socket failed: $?"
	$MUNLINK $DIR/socket || error "$MUNLINK $DIR/socket failed: $?"
}
run_test 54a "unix domain socket test =========================="

test_54b() {
	f="$DIR/f54b"
	mknod $f c 1 3
	chmod 0666 $f
	dd if=/dev/zero of=$f bs=$PAGE_SIZE count=1
}
run_test 54b "char device works in lustre ======================"

find_loop_dev() {
	[ -b /dev/loop/0 ] && LOOPBASE=/dev/loop/
	[ -b /dev/loop0 ] && LOOPBASE=/dev/loop
	[ -z "$LOOPBASE" ] && echo "/dev/loop/0 and /dev/loop0 gone?" && return

	for i in $(seq 3 7); do
		losetup $LOOPBASE$i > /dev/null 2>&1 && continue
		LOOPDEV=$LOOPBASE$i
		LOOPNUM=$i
		break
	done
}

cleanup_54c() {
	local rc=0
	loopdev="$DIR/loop54c"

	trap 0
	$UMOUNT $DIR/$tdir || rc=$?
	losetup -d $loopdev || true
	losetup -d $LOOPDEV || true
	rm -rf $loopdev $DIR/$tfile $DIR/$tdir
	return $rc
}

test_54c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	loopdev="$DIR/loop54c"

	find_loop_dev
	[ -z "$LOOPNUM" ] && skip_env "couldn't find empty loop device"
	trap cleanup_54c EXIT
	mknod $loopdev b 7 $LOOPNUM
	echo "make a loop file system with $DIR/$tfile on $loopdev ($LOOPNUM)."
	dd if=/dev/zero of=$DIR/$tfile bs=$PAGE_SIZE seek=1024 count=1 > /dev/null
	losetup $loopdev $DIR/$tfile ||
		error "can't set up $loopdev for $DIR/$tfile"
	mkfs.ext2 $loopdev || error "mke2fs on $loopdev"
	test_mkdir $DIR/$tdir
	mount -t ext2 $loopdev $DIR/$tdir ||
		error "error mounting $loopdev on $DIR/$tdir"
	dd if=/dev/zero of=$DIR/$tdir/tmp bs=$PAGE_SIZE count=30 ||
		error "dd write"
	df $DIR/$tdir
	dd if=$DIR/$tdir/tmp of=/dev/zero bs=$PAGE_SIZE count=30 ||
		error "dd read"
	cleanup_54c
}
run_test 54c "block device works in lustre ====================="

test_54d() {
	f="$DIR/f54d"
	string="aaaaaa"
	mknod $f p
	[ "$string" = $(echo $string > $f | cat $f) ] || error "$f != $string"
}
run_test 54d "fifo device works in lustre ======================"

test_54e() {
	f="$DIR/f54e"
	string="aaaaaa"
	cp -aL /dev/console $f
	echo $string > $f || error "echo $string to $f failed"
}
run_test 54e "console/tty device works in lustre ======================"

test_56a() {
	local numfiles=3
	local dir=$DIR/$tdir

	rm -rf $dir
	test_mkdir -p $dir/dir
	for i in $(seq $numfiles); do
		touch $dir/file$i
		touch $dir/dir/file$i
	done

	local numcomp=$($LFS getstripe --component-count $dir)

	[[ $numcomp == 0 ]] && numcomp=1

	# test lfs getstripe with --recursive
	local filenum=$($LFS getstripe -r $dir | egrep -c "obdidx|l_ost_idx")

	[[ $filenum -eq $((numfiles * 2)) ]] ||
		error "$LFS getstripe -r: found $filenum != $((numfiles * 2))"
	filenum=$($LFS getstripe $dir | egrep -c "obdidx|l_ost_idx")
	[[ $filenum -eq $numfiles ]] ||
		error "$LFS getstripe $dir: found $filenum, not $numfiles"
	echo "$LFS getstripe showed obdidx or l_ost_idx"

	# test lfs getstripe with file instead of dir
	filenum=$($LFS getstripe $dir/file1 | egrep -c "obdidx|l_ost_idx")
	[[ $filenum -eq 1 ]] ||
		error "$LFS getstripe $dir/file1: found $filenum, not 1"
	echo "$LFS getstripe file1 passed"

	#test lfs getstripe with --verbose
	filenum=$($LFS getstripe --verbose $dir | grep -c lmm_magic)
	[[ $filenum -eq $((numfiles * numcomp)) ]] ||
		error "$LFS getstripe --verbose $dir: "\
		      "got $filenum want $((numfiles * numcomp)) lmm_magic"
	[[ $($LFS getstripe $dir | grep -c lmm_magic) -eq 0 ]] ||
		error "$LFS getstripe $dir: showed lmm_magic"

	#test lfs getstripe with -v prints lmm_fid
	filenum=$($LFS getstripe -v $dir | grep -c lmm_fid)
	[[ $filenum -eq $((numfiles * numcomp)) ]] ||
		error "$LFS getstripe -v $dir: "\
		      "got $filenum want $((numfiles * numcomp)) lmm_fid"
	[[ $($LFS getstripe $dir | grep -c lmm_fid) -eq 0 ]] ||
		error "$LFS getstripe $dir: showed lmm_fid by default"
	echo "$LFS getstripe --verbose passed"

	#check for FID information
	local fid1=$($LFS getstripe --fid $dir/file1)
	local fid2=$($LFS getstripe --verbose $dir/file1 |
		     awk '/lmm_fid: / { print $2; exit; }')
	local fid3=$($LFS path2fid $dir/file1)

	[ "$fid1" != "$fid2" ] &&
		error "getstripe --fid '$fid1' != getstripe --verbose '$fid2'"
	[ "$fid1" != "$fid3" ] &&
		error "getstripe --fid '$fid1' != lfs path2fid '$fid3'"
	echo "$LFS getstripe --fid passed"

	#test lfs getstripe with --obd
	$LFS getstripe --obd wrong_uuid $dir 2>&1 | grep -q "unknown obduuid" ||
		error "$LFS getstripe --obd wrong_uuid: should return error"

	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	local ostidx=1
	local obduuid=$(ostuuid_from_index $ostidx)
	local found=$($LFS getstripe -r --obd $obduuid $dir |
		grep 'lmm_stripe_offset:' | grep -c " $ostidx\$")

	filenum=$($LFS getstripe -ir $dir | grep -c "^$ostidx\$")
	[[ $($LFS getstripe -id $dir) -ne $ostidx ]] ||
		((filenum--))
	[[ $($LFS getstripe -id $dir/dir) -ne $ostidx ]] ||
		((filenum--))

	[[ $found -eq $filenum ]] ||
		error "$LFS getstripe --obd: found $found expect $filenum"
	[[ $($LFS getstripe -r -v --obd $obduuid $dir |
		sed '/^[	 ]*'${ostidx}'[	 ]/d' |
		sed -n '/^[	 ]*[0-9][0-9]*[	 ]/p' | wc -l) -eq 0 ]] ||
		error "$LFS getstripe --obd: should not show file on other obd"
	echo "$LFS getstripe --obd passed"
}
run_test 56a "check $LFS getstripe"

test_56b() {
	local dir=$DIR/$tdir
	local numdirs=3

	test_mkdir $dir
	for i in $(seq $numdirs); do
		test_mkdir $dir/dir$i
	done

	# test lfs getdirstripe default mode is non-recursion, which is
	# different from lfs getstripe
	local dircnt=$($LFS getdirstripe $dir | grep -c lmv_stripe_count)

	[[ $dircnt -eq 1 ]] ||
		error "$LFS getdirstripe: found $dircnt, not 1"
	dircnt=$($LFS getdirstripe --recursive $dir |
		grep -c lmv_stripe_count)
	[[ $dircnt -eq $((numdirs + 1)) ]] ||
		error "$LFS getdirstripe -r: $dircnt, != $((numdirs + 1))"
}
run_test 56b "check $LFS getdirstripe"

test_56c() {
	remote_ost_nodsh && skip "remote OST with nodsh"

	local ost_idx=0
	local ost_name=$(ostname_from_index $ost_idx)
	local old_status=$(ost_dev_status $ost_idx)

	[[ -z "$old_status" ]] ||
		skip_env "OST $ost_name is in $old_status status"

	do_facet ost1 $LCTL set_param -n obdfilter.$ost_name.degraded=1
	[[ $OST1_VERSION -ge $(version_code 2.12.3) ]] && do_facet ost1 \
		$LCTL set_param -n obdfilter.$ost_name.no_precreate=1
	sleep_maxage

	local new_status=$(ost_dev_status $ost_idx)

	[[ "$new_status" =~ "D" ]] ||
		error "$ost_name status is '$new_status', missing 'D'"
	if [[ $OST1_VERSION -ge $(version_code 2.12.3) ]]; then
		[[ "$new_status" =~ "N" ]] ||
			error "$ost_name status is '$new_status', missing 'N'"
	fi

	do_facet ost1 $LCTL set_param -n obdfilter.$ost_name.degraded=0
	[[ $OST1_VERSION -ge $(version_code 2.12.3) ]] && do_facet ost1 \
		$LCTL set_param -n obdfilter.$ost_name.no_precreate=0
	sleep_maxage

	new_status=$(ost_dev_status $ost_idx)
	[[ ! "$new_status" =~ "D" && ! "$new_status" =~ "N" ]] ||
		error "$ost_name status is '$new_status', has 'D' and/or 'N'"
}
run_test 56c "check 'lfs df' showing device status"

NUMFILES=3
NUMDIRS=3
setup_56() {
	local local_tdir="$1"
	local local_numfiles="$2"
	local local_numdirs="$3"
	local dir_params="$4"
	local dir_stripe_params="$5"

	if [ ! -d "$local_tdir" ] ; then
		test_mkdir -p $dir_stripe_params $local_tdir
		[ "$dir_params" ] && $LFS setstripe $dir_params $local_tdir
		for i in $(seq $local_numfiles) ; do
			touch $local_tdir/file$i
		done
		for i in $(seq $local_numdirs) ; do
			test_mkdir $dir_stripe_params $local_tdir/dir$i
			for j in $(seq $local_numfiles) ; do
				touch $local_tdir/dir$i/file$j
			done
		done
	fi
}

setup_56_special() {
	local local_tdir=$1
	local local_numfiles=$2
	local local_numdirs=$3

	setup_56 $local_tdir $local_numfiles $local_numdirs

	if [ ! -e "$local_tdir/loop${local_numfiles}b" ] ; then
		for i in $(seq $local_numfiles) ; do
			mknod $local_tdir/loop${i}b b 7 $i
			mknod $local_tdir/null${i}c c 1 3
			ln -s $local_tdir/file1 $local_tdir/link${i}
		done
		for i in $(seq $local_numdirs) ; do
			mknod $local_tdir/dir$i/loop${i}b b 7 $i
			mknod $local_tdir/dir$i/null${i}c c 1 3
			ln -s $local_tdir/dir$i/file1 $local_tdir/dir$i/link${i}
		done
	fi
}

test_56g() {
	local dir=$DIR/d$(basetest $testnum)g.$TESTSUITE
	local expected=$(($NUMDIRS + 2))

	setup_56 $dir $NUMFILES $NUMDIRS

	# test lfs find with -name
	for i in $(seq $NUMFILES) ; do
		local nums=$($LFS find -name "*$i" $dir | wc -l)

		[ $nums -eq $expected ] ||
			error "lfs find -name '*$i' $dir wrong: "\
			      "found $nums, expected $expected"
	done
}
run_test 56g "check lfs find -name"

test_56h() {
	local dir=$DIR/d$(basetest $testnum)g.$TESTSUITE
	local expected=$(((NUMDIRS + 1) * (NUMFILES - 1) + NUMFILES))

	setup_56 $dir $NUMFILES $NUMDIRS

	# test lfs find with ! -name
	for i in $(seq $NUMFILES) ; do
		local nums=$($LFS find ! -name "*$i" $dir | wc -l)

		[ $nums -eq $expected ] ||
			error "lfs find ! -name '*$i' $dir wrong: "\
			      "found $nums, expected $expected"
	done
}
run_test 56h "check lfs find ! -name"

test_56i() {
	local dir=$DIR/$tdir

	test_mkdir $dir

	local cmd="$LFS find -ost $(ostuuid_from_index 0 $dir) $dir"
	local out=$($cmd)

	[ -z "$out" ] || error "'$cmd' returned directory '$out'"
}
run_test 56i "check 'lfs find -ost UUID' skips directories"

test_56j() {
	local dir=$DIR/d$(basetest $testnum)g.$TESTSUITE

	setup_56_special $dir $NUMFILES $NUMDIRS

	local expected=$((NUMDIRS + 1))
	local cmd="$LFS find -type d $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56j "check lfs find -type d"

test_56k() {
	local dir=$DIR/d$(basetest $testnum)g.$TESTSUITE

	setup_56_special $dir $NUMFILES $NUMDIRS

	local expected=$(((NUMDIRS + 1) * NUMFILES))
	local cmd="$LFS find -type f $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56k "check lfs find -type f"

test_56l() {
	local dir=$DIR/d$(basetest $testnum)g.$TESTSUITE

	setup_56_special $dir $NUMFILES $NUMDIRS

	local expected=$((NUMDIRS + NUMFILES))
	local cmd="$LFS find -type b $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56l "check lfs find -type b"

test_56m() {
	local dir=$DIR/d$(basetest $testnum)g.$TESTSUITE

	setup_56_special $dir $NUMFILES $NUMDIRS

	local expected=$((NUMDIRS + NUMFILES))
	local cmd="$LFS find -type c $dir"
	local nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56m "check lfs find -type c"

test_56n() {
	local dir=$DIR/d$(basetest $testnum)g.$TESTSUITE
	setup_56_special $dir $NUMFILES $NUMDIRS

	local expected=$((NUMDIRS + NUMFILES))
	local cmd="$LFS find -type l $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56n "check lfs find -type l"

test_56o() {
	local dir=$DIR/$tdir

	setup_56 $dir $NUMFILES $NUMDIRS
	utime $dir/file1 > /dev/null || error "utime (1)"
	utime $dir/file2 > /dev/null || error "utime (2)"
	utime $dir/dir1 > /dev/null || error "utime (3)"
	utime $dir/dir2 > /dev/null || error "utime (4)"
	utime $dir/dir1/file1 > /dev/null || error "utime (5)"
	dd if=/dev/zero count=1 >> $dir/dir1/file1 && sync

	local expected=4
	local nums=$($LFS find -mtime +0 $dir | wc -l)

	[ $nums -eq $expected ] ||
		error "lfs find -mtime +0 $dir: found $nums expect $expected"

	expected=12
	cmd="$LFS find -mtime 0 $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56o "check lfs find -mtime for old files"

test_56p() {
	[ $RUNAS_ID -eq $UID ] &&
		skip_env "RUNAS_ID = UID = $UID -- skipping"

	local dir=$DIR/$tdir

	setup_56 $dir $NUMFILES $NUMDIRS
	chown $RUNAS_ID $dir/file* || error "chown $DIR/${tdir}g/file$i failed"

	local expected=$NUMFILES
	local cmd="$LFS find -uid $RUNAS_ID $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=$(((NUMFILES + 1) * NUMDIRS + 1))
	cmd="$LFS find ! -uid $RUNAS_ID $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56p "check lfs find -uid and ! -uid"

test_56q() {
	[ $RUNAS_ID -eq $UID ] &&
		skip_env "RUNAS_ID = UID = $UID -- skipping"

	local dir=$DIR/$tdir

	setup_56 $dir $NUMFILES $NUMDIRS
	chgrp $RUNAS_GID $dir/file* || error "chown $dir/file$i failed"

	local expected=$NUMFILES
	local cmd="$LFS find -gid $RUNAS_GID $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=$(( ($NUMFILES+1) * $NUMDIRS + 1))
	cmd="$LFS find ! -gid $RUNAS_GID $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56q "check lfs find -gid and ! -gid"

test_56r() {
	local dir=$DIR/$tdir

	setup_56 $dir $NUMFILES $NUMDIRS

	local expected=12
	local cmd="$LFS find -size 0 -type f -lazy $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
	cmd="$LFS find -size 0 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=0
	cmd="$LFS find ! -size 0 -type f -lazy $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
	cmd="$LFS find ! -size 0 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	echo "test" > $dir/$tfile
	echo "test2" > $dir/$tfile.2 && sync
	expected=1
	cmd="$LFS find -size 5 -type f -lazy $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
	cmd="$LFS find -size 5 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=1
	cmd="$LFS find -size +5 -type f -lazy $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
	cmd="$LFS find -size +5 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=2
	cmd="$LFS find -size +0 -type f -lazy $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
	cmd="$LFS find -size +0 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=2
	cmd="$LFS find ! -size -5 -type f -lazy $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
	cmd="$LFS find ! -size -5 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=12
	cmd="$LFS find -size -5 -type f -lazy $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
	cmd="$LFS find -size -5 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56r "check lfs find -size works"

test_56ra_sub() {
	local expected=$1
	local glimpses=$2
	local cmd="$3"

	cancel_lru_locks $OSC

	local rpcs_before=$(calc_stats $OSC.*$OSC*.stats ldlm_glimpse_enqueue)
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	local rpcs_after=$(calc_stats $OSC.*$OSC*.stats ldlm_glimpse_enqueue)

	if (( rpcs_before + glimpses != rpcs_after )); then
		echo "Before: $rpcs_before After: $rpcs_after $NUMFILES"
		$LCTL get_param osc.*.stats | grep ldlm_glimpse_enqueue

		if [[ $glimpses == 0 ]]; then
			error "'$cmd' should not send glimpse RPCs to OST"
		else
			error "'$cmd' should send $glimpses glimpse RPCs to OST"
		fi
	fi
}

test_56ra() {
	[[ $MDS1_VERSION -ge $(version_code 2.12.4) ]] ||
		skip "MDS < 2.12.4 doesn't return LSOM data"
	local dir=$DIR/$tdir

	[[ $OSC == "mdc" ]] && skip "DoM files"

	setup_56 $dir $NUMFILES $NUMDIRS "-c 1"
	# open and close all files to ensure LSOM is updated
	cancel_lru_locks $OSC
	find $dir -type f | xargs cat > /dev/null

	#   expect_found  glimpse_rpcs  command_to_run
	test_56ra_sub 12  0 "$LFS find -size 0 -type f -lazy $dir"
	test_56ra_sub 12 12 "$LFS find -size 0 -type f $dir"
	test_56ra_sub  0  0 "$LFS find ! -size 0 -type f -lazy $dir"
	test_56ra_sub  0 12 "$LFS find ! -size 0 -type f $dir"

	echo "test" > $dir/$tfile
	echo "test2" > $dir/$tfile.2 && sync
	cancel_lru_locks $OSC
	cat $dir/$tfile $dir/$tfile.2 > /dev/null

	test_56ra_sub  1  0 "$LFS find -size 5 -type f -lazy $dir"
	test_56ra_sub  1 14 "$LFS find -size 5 -type f $dir"
	test_56ra_sub  1  0 "$LFS find -size +5 -type f -lazy $dir"
	test_56ra_sub  1 14 "$LFS find -size +5 -type f $dir"

	test_56ra_sub  2  0 "$LFS find -size +0 -type f -lazy $dir"
	test_56ra_sub  2 14 "$LFS find -size +0 -type f $dir"
	test_56ra_sub  2  0 "$LFS find ! -size -5 -type f -lazy $dir"
	test_56ra_sub  2 14 "$LFS find ! -size -5 -type f $dir"
	test_56ra_sub 12  0 "$LFS find -size -5 -type f -lazy $dir"
	test_56ra_sub 12 14 "$LFS find -size -5 -type f $dir"
}
run_test 56ra "check lfs find -size -lazy works for data on OSTs"

test_56s() { # LU-611 #LU-9369
	[[ $OSTCOUNT -lt 2 ]] && skip_env "need at least 2 OSTs"

	local dir=$DIR/$tdir
	local onestripe=$(((NUMDIRS + 1) * NUMFILES))

	setup_56 $dir $NUMFILES $NUMDIRS "-c 1"
	for i in $(seq $NUMDIRS); do
		$LFS setstripe -c $((OSTCOUNT + 1)) $dir/dir$i/$tfile
	done

	local expected=$NUMDIRS
	local cmd="$LFS find -c $OSTCOUNT $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] || {
		$LFS getstripe -R $dir
		error "'$cmd' wrong: found $nums, expected $expected"
	}

	expected=$((NUMDIRS + onestripe))
	cmd="$LFS find -stripe-count +0 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] || {
		$LFS getstripe -R $dir
		error "'$cmd' wrong: found $nums, expected $expected"
	}

	expected=$onestripe
	cmd="$LFS find -stripe-count 1 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] || {
		$LFS getstripe -R $dir
		error "'$cmd' wrong: found $nums, expected $expected"
	}

	cmd="$LFS find -stripe-count -2 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] || {
		$LFS getstripe -R $dir
		error "'$cmd' wrong: found $nums, expected $expected"
	}

	expected=0
	cmd="$LFS find -stripe-count $((OSTCOUNT + 1)) -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] || {
		$LFS getstripe -R $dir
		error "'$cmd' wrong: found $nums, expected $expected"
	}
}
run_test 56s "check lfs find -stripe-count works"

test_56t() { # LU-611 #LU-9369
	local dir=$DIR/$tdir

	setup_56 $dir 0 $NUMDIRS
	for i in $(seq $NUMDIRS); do
		$LFS setstripe -S 8M $dir/dir$i/$tfile
	done

	local expected=$NUMDIRS
	local cmd="$LFS find -S 8M $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] || {
		$LFS getstripe -R $dir
		error "'$cmd' wrong: found $nums, expected $expected"
	}
	rm -rf $dir

	setup_56 $dir $NUMFILES $NUMDIRS "--stripe-size 512k"

	$LFS setstripe -S 256k $dir/$tfile.{0,1,2,3}

	expected=$(((NUMDIRS + 1) * NUMFILES))
	cmd="$LFS find -stripe-size 512k -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	cmd="$LFS find -stripe-size +320k -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=$(((NUMDIRS + 1) * NUMFILES + 4))
	cmd="$LFS find -stripe-size +200k -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	cmd="$LFS find -stripe-size -640k -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=4
	cmd="$LFS find -stripe-size 256k -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	cmd="$LFS find -stripe-size -320k -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=0
	cmd="$LFS find -stripe-size 1024k -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"
}
run_test 56t "check lfs find -stripe-size works"

test_56u() { # LU-611
	local dir=$DIR/$tdir

	setup_56 $dir $NUMFILES $NUMDIRS "-i 0 -c 1"

	if [[ $OSTCOUNT -gt 1 ]]; then
		$LFS setstripe -i 1 -c 1 $dir/$tfile.{0,1,2,3}
		onestripe=4
	else
		onestripe=0
	fi

	local expected=$(((NUMDIRS + 1) * NUMFILES))
	local cmd="$LFS find -stripe-index 0 -type f $dir"
	local nums=$($cmd | wc -l)

	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=$onestripe
	cmd="$LFS find -stripe-index 1 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	cmd="$LFS find ! -stripe-index 0 -type f $dir"
	nums=$($cmd | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	expected=0
	# This should produce an error and not return any files
	cmd="$LFS find -stripe-index $OSTCOUNT -type f $dir"
	nums=$($cmd 2>/dev/null | wc -l)
	[ $nums -eq $expected ] ||
		error "'$cmd' wrong: found $nums, expected $expected"

	if [[ $OSTCOUNT -gt 1 ]]; then
		expected=$(((NUMDIRS + 1) * NUMFILES + onestripe))
		cmd="$LFS find -stripe-index 0,1 -type f $dir"
		nums=$($cmd | wc -l)
		[ $nums -eq $expected ] ||
			error "'$cmd' wrong: found $nums, expected $expected"
	fi
}
run_test 56u "check lfs find -stripe-index works"

test_56v() {
	local mdt_idx=0
	local dir=$DIR/$tdir

	setup_56 $dir $NUMFILES $NUMDIRS

	UUID=$(mdtuuid_from_index $mdt_idx $dir)
	[ -z "$UUID" ] && error "mdtuuid_from_index cannot find MDT $mdt_idx"

	for file in $($LFS find -m $UUID $dir); do
		file_midx=$($LFS getstripe -m $file)
		[ $file_midx -eq $mdt_idx ] ||
			error "lfs find -m $UUID != getstripe -m $file_midx"
	done
}
run_test 56v "check 'lfs find -m match with lfs getstripe -m'"

test_56w() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local dir=$DIR/$tdir

	setup_56 $dir $NUMFILES $NUMDIRS "-c $OSTCOUNT" "-c1"

	local stripe_size=$($LFS getstripe -S -d $dir) ||
		error "$LFS getstripe -S -d $dir failed"
	stripe_size=${stripe_size%% *}

	local file_size=$((stripe_size * OSTCOUNT))
	local file_num=$((NUMDIRS * NUMFILES + NUMFILES))
	local required_space=$((file_num * file_size))
	local free_space=$($LCTL get_param -n lov.$FSNAME-clilov-*.kbytesavail |
			   head -n1)
	[[ $free_space -le $((required_space / 1024)) ]] &&
		skip_env "need $required_space, have $free_space kbytes"

	local dd_bs=65536
	local dd_count=$((file_size / dd_bs))

	# write data into the files
	local i
	local j
	local file

	for i in $(seq $NUMFILES); do
		file=$dir/file$i
		yes | dd bs=$dd_bs count=$dd_count of=$file &>/dev/null ||
			error "write data into $file failed"
	done
	for i in $(seq $NUMDIRS); do
		for j in $(seq $NUMFILES); do
			file=$dir/dir$i/file$j
			yes|dd bs=$dd_bs count=$dd_count of=$file &>/dev/null ||
				error "write data into $file failed"
		done
	done

	# $LFS_MIGRATE will fail if hard link migration is unsupported
	if [[ $(lustre_version_code mds1) -gt $(version_code 2.5.55) ]]; then
		createmany -l$dir/dir1/file1 $dir/dir1/link 200 ||
			error "creating links to $dir/dir1/file1 failed"
	fi

	local expected=-1

	[[ $OSTCOUNT -gt 1 ]] && expected=$((OSTCOUNT - 1))

	# lfs_migrate file
	local cmd="$LFS_MIGRATE -y -c $expected $dir/file1"

	echo "$cmd"
	eval $cmd || error "$cmd failed"

	check_stripe_count $dir/file1 $expected

	if [ $MDS1_VERSION -ge $(version_code 2.6.90) ];
	then
		# lfs_migrate file onto OST 0 if it is on OST 1, or onto
		# OST 1 if it is on OST 0. This file is small enough to
		# be on only one stripe.
		file=$dir/migr_1_ost
		dd bs=$dd_bs count=1 if=/dev/urandom of=$file >/dev/null 2>&1 ||
			error "write data into $file failed"
		local obdidx=$($LFS getstripe -i $file)
		local oldmd5=$(md5sum $file)
		local newobdidx=0

		[[ $obdidx -eq 0 ]] && newobdidx=1
		cmd="$LFS migrate -i $newobdidx $file"
		echo $cmd
		eval $cmd || error "$cmd failed"

		local realobdix=$($LFS getstripe -i $file)
		local newmd5=$(md5sum $file)

		[[ $newobdidx -ne $realobdix ]] &&
			error "new OST is different (was=$obdidx, "\
			      "wanted=$newobdidx, got=$realobdix)"
		[[ "$oldmd5" != "$newmd5" ]] &&
			error "md5sum differ: $oldmd5, $newmd5"
	fi

	# lfs_migrate dir
	cmd="$LFS_MIGRATE -y -c $expected $dir/dir1"
	echo "$cmd"
	eval $cmd || error "$cmd failed"

	for j in $(seq $NUMFILES); do
		check_stripe_count $dir/dir1/file$j $expected
	done

	# lfs_migrate works with lfs find
	cmd="$LFS find -stripe_count $OSTCOUNT -type f $dir |
	     $LFS_MIGRATE -y -c $expected"
	echo "$cmd"
	eval $cmd || error "$cmd failed"

	for i in $(seq 2 $NUMFILES); do
		check_stripe_count $dir/file$i $expected
	done
	for i in $(seq 2 $NUMDIRS); do
		for j in $(seq $NUMFILES); do
		check_stripe_count $dir/dir$i/file$j $expected
		done
	done
}
run_test 56w "check lfs_migrate -c stripe_count works"

test_56wb() {
	local file1=$DIR/$tdir/file1
	local create_pool=false
	local initial_pool=$($LFS getstripe -p $DIR)
	local pool_list=()
	local pool=""

	echo -n "Creating test dir..."
	test_mkdir $DIR/$tdir &> /dev/null || error "cannot create dir"
	echo "done."

	echo -n "Creating test file..."
	touch $file1 || error "cannot create file"
	echo "done."

	echo -n "Detecting existing pools..."
	pool_list=($($LFS pool_list $FSNAME | grep "$FSNAME\." | cut -d. -f2))

	if [ ${#pool_list[@]} -gt 0 ]; then
		echo "${pool_list[@]}"
		for thispool in "${pool_list[@]}"; do
			if [[ -z "$initial_pool" ||
			      "$initial_pool" != "$thispool" ]]; then
				pool="$thispool"
				echo "Using existing pool '$pool'"
				break
			fi
		done
	else
		echo "none detected."
	fi
	if [ -z "$pool" ]; then
		pool=${POOL:-testpool}
		[ "$initial_pool" = "$pool" ] && pool="testpool2"
		echo -n "Creating pool '$pool'..."
		create_pool=true
		pool_add $pool &> /dev/null ||
			error "pool_add failed"
		echo "done."

		echo -n "Adding target to pool..."
		pool_add_targets $pool 0 0 1 &> /dev/null ||
			error "pool_add_targets failed"
		echo "done."
	fi

	echo -n "Setting pool using -p option..."
	$LFS_MIGRATE -y -q --no-rsync -p $pool $file1 &> /dev/null ||
		error "migrate failed rc = $?"
	echo "done."

	echo -n "Verifying test file is in pool after migrating..."
	[ "$($LFS getstripe -p $file1)" = $pool ] ||
		error "file was not migrated to pool $pool"
	echo "done."

	echo -n "Removing test file from pool '$pool'..."
	# "lfs migrate $file" won't remove the file from the pool
	# until some striping information is changed.
	$LFS migrate -c 1 $file1 &> /dev/null ||
		error "cannot remove from pool"
	[ "$($LFS getstripe -p $file1)" ] &&
		error "pool still set"
	echo "done."

	echo -n "Setting pool using --pool option..."
	$LFS_MIGRATE -y -q --no-rsync --pool $pool $file1 &> /dev/null ||
		error "migrate failed rc = $?"
	echo "done."

	# Clean up
	rm -f $file1
	if $create_pool; then
		destroy_test_pools 2> /dev/null ||
			error "destroy test pools failed"
	fi
}
run_test 56wb "check lfs_migrate pool support"

test_56wc() {
	local file1="$DIR/$tdir/file1"
	local parent_ssize
	local parent_scount
	local cur_ssize
	local cur_scount
	local orig_ssize

	echo -n "Creating test dir..."
	test_mkdir $DIR/$tdir &> /dev/null || error "cannot create dir"
	$LFS setstripe -S 1M -c 1 "$DIR/$tdir" &> /dev/null ||
		error "cannot set stripe by '-S 1M -c 1'"
	echo "done"

	echo -n "Setting initial stripe for test file..."
	$LFS setstripe -S 512K -c 1 "$file1" &> /dev/null ||
		error "cannot set stripe"
	cur_ssize=$($LFS getstripe -S "$file1")
	[ $cur_ssize -eq 524288 ] || error "setstripe -S $cur_ssize != 524288"
	echo "done."

	# File currently set to -S 512K -c 1

	# Ensure -c and -S options are rejected when -R is set
	echo -n "Verifying incompatible options are detected..."
	$LFS_MIGRATE -y -R -c 1 "$file1" &> /dev/null &&
		error "incompatible -c and -R options not detected"
	$LFS_MIGRATE -y -R -S 1M "$file1" &> /dev/null &&
		error "incompatible -S and -R options not detected"
	echo "done."

	# Ensure unrecognized options are passed through to 'lfs migrate'
	echo -n "Verifying -S option is passed through to lfs migrate..."
	$LFS_MIGRATE -y -S 1M "$file1" &> /dev/null ||
		error "migration failed"
	cur_ssize=$($LFS getstripe -S "$file1")
	[ $cur_ssize -eq 1048576 ] || error "migrate -S $cur_ssize != 1048576"
	echo "done."

	# File currently set to -S 1M -c 1

	# Ensure long options are supported
	echo -n "Verifying long options supported..."
	$LFS_MIGRATE -y --non-block "$file1" &> /dev/null ||
		error "long option without argument not supported"
	$LFS_MIGRATE -y --stripe-size 512K "$file1" &> /dev/null ||
		error "long option with argument not supported"
	cur_ssize=$($LFS getstripe -S "$file1")
	[ $cur_ssize -eq 524288 ] ||
		error "migrate --stripe-size $cur_ssize != 524288"
	echo "done."

	# File currently set to -S 512K -c 1

	if [ "$OSTCOUNT" -gt 1 ]; then
		echo -n "Verifying explicit stripe count can be set..."
		$LFS_MIGRATE -y -c 2 "$file1" &> /dev/null ||
			error "migrate failed"
		cur_scount=$($LFS getstripe -c "$file1")
		[ $cur_scount -eq 2 ] || error "migrate -c $cur_scount != 2"
		echo "done."
	fi

	# File currently set to -S 512K -c 1 or -S 512K -c 2

	# Ensure parent striping is used if -R is set, and no stripe
	# count or size is specified
	echo -n "Setting stripe for parent directory..."
	$LFS setstripe -S 2M -c 1 "$DIR/$tdir" &> /dev/null ||
		error "cannot set stripe '-S 2M -c 1'"
	echo "done."

	echo -n "Verifying restripe option uses parent stripe settings..."
	parent_ssize=$($LFS getstripe -S $DIR/$tdir 2>/dev/null)
	parent_scount=$($LFS getstripe -c $DIR/$tdir 2>/dev/null)
	$LFS_MIGRATE -y -R "$file1" &> /dev/null ||
		error "migrate failed"
	cur_ssize=$($LFS getstripe -S "$file1")
	[ $cur_ssize -eq $parent_ssize ] ||
		error "migrate -R stripe_size $cur_ssize != $parent_ssize"
	cur_scount=$($LFS getstripe -c "$file1")
	[ $cur_scount -eq $parent_scount ] ||
		error "migrate -R stripe_count $cur_scount != $parent_scount"
	echo "done."

	# File currently set to -S 1M -c 1

	# Ensure striping is preserved if -R is not set, and no stripe
	# count or size is specified
	echo -n "Verifying striping size preserved when not specified..."
	orig_ssize=$($LFS getstripe -S "$file1" 2>/dev/null)
	$LFS setstripe -S 2M -c 1 "$DIR/$tdir" &> /dev/null ||
		error "cannot set stripe on parent directory"
	$LFS_MIGRATE -y "$file1" &> /dev/null ||
		error "migrate failed"
	cur_ssize=$($LFS getstripe -S "$file1")
	[ $cur_ssize -eq $orig_ssize ] ||
		error "migrate by default $cur_ssize != $orig_ssize"
	echo "done."

	# Ensure file name properly detected when final option has no argument
	echo -n "Verifying file name properly detected..."
	$LFS_MIGRATE -y "$file1" &> /dev/null ||
		error "file name interpreted as option argument"
	echo "done."

	# Clean up
	rm -f "$file1"
}
run_test 56wc "check unrecognized options for lfs_migrate are passed through"

test_56wd() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	local file1=$DIR/$tdir/file1

	echo -n "Creating test dir..."
	test_mkdir $DIR/$tdir || error "cannot create dir"
	echo "done."

	echo -n "Creating test file..."
	touch $file1
	echo "done."

	# Ensure 'lfs migrate' will fail by using a non-existent option,
	# and make sure rsync is not called to recover
	echo -n "Make sure --no-rsync option works..."
	$LFS_MIGRATE -y --no-rsync --invalid-opt $file1 2>&1 |
		grep -q 'refusing to fall back to rsync' ||
		error "rsync was called with --no-rsync set"
	echo "done."

	# Ensure rsync is called without trying 'lfs migrate' first
	echo -n "Make sure --rsync option works..."
	$LFS_MIGRATE -y --rsync --invalid-opt $file1 2>&1 |
		grep -q 'falling back to rsync' &&
		error "lfs migrate was called with --rsync set"
	echo "done."

	echo -n "Make sure --rsync and --no-rsync options are exclusive..."
	$LFS_MIGRATE -y --rsync --no-rsync $file1 2>&1 |
		grep -q 'at the same time' ||
		error "--rsync and --no-rsync accepted concurrently"
	echo "done."

	# Clean up
	rm -f $file1
}
run_test 56wd "check lfs_migrate --rsync and --no-rsync work"

test_56x() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	check_swap_layouts_support

	local dir=$DIR/$tdir
	local ref1=/etc/passwd
	local file1=$dir/file1

	test_mkdir $dir || error "creating dir $dir"
	$LFS setstripe -c 2 $file1
	cp $ref1 $file1
	$LFS migrate -c 1 $file1 || error "migrate failed rc = $?"
	stripe=$($LFS getstripe -c $file1)
	[[ $stripe == 1 ]] || error "stripe of $file1 is $stripe != 1"
	cmp $file1 $ref1 || error "content mismatch $file1 differs from $ref1"

	# clean up
	rm -f $file1
}
run_test 56x "lfs migration support"

test_56xa() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	check_swap_layouts_support

	local dir=$DIR/$tdir/$testnum

	test_mkdir -p $dir

	local ref1=/etc/passwd
	local file1=$dir/file1

	$LFS setstripe -c 2 $file1
	cp $ref1 $file1
	$LFS migrate --block -c 1 $file1 || error "migrate failed rc = $?"

	local stripe=$($LFS getstripe -c $file1)

	[[ $stripe == 1 ]] || error "stripe of $file1 is $stripe != 1"
	cmp $file1 $ref1 || error "content mismatch $file1 differs from $ref1"

	# clean up
	rm -f $file1
}
run_test 56xa "lfs migration --block support"

check_migrate_links() {
	local dir="$1"
	local file1="$dir/file1"
	local begin="$2"
	local count="$3"
	local total_count=$(($begin + $count - 1))
	local symlink_count=10
	local uniq_count=10

	if [ ! -f "$file1" ]; then
		echo -n "creating initial file..."
		$LFS setstripe -c 1 -S "512k" "$file1" ||
			error "cannot setstripe initial file"
		echo "done"

		echo -n "creating symlinks..."
		for s in $(seq 1 $symlink_count); do
			ln -s "$file1" "$dir/slink$s" ||
				error "cannot create symlinks"
		done
		echo "done"

		echo -n "creating nonlinked files..."
		createmany -o "$dir/uniq" 1 10 &> /dev/null ||
			error "cannot create nonlinked files"
		echo "done"
	fi

	# create hard links
	if [ ! -f "$dir/file$total_count" ]; then
		echo -n "creating hard links $begin:$total_count..."
		createmany -l"$file1" "$dir/file" "$begin" "$count" &>	\
			/dev/null || error "cannot create hard links"
		echo "done"
	fi

	echo -n "checking number of hard links listed in xattrs..."
	local fid=$($LFS getstripe -F "$file1")
	local paths=($($LFS fid2path "$MOUNT" "$fid" 2> /dev/null))

	echo "${#paths[*]}"
	if [ ${#paths[*]} -lt $total_count -a "$begin" -eq 2  ]; then
			skip "hard link list has unexpected size, skipping test"
	fi
	if [ ${#paths[*]} -ge $total_count -a "$begin" -ne 2  ]; then
			error "link names should exceed xattrs size"
	fi

	echo -n "migrating files..."
	local migrate_out=$($LFS_MIGRATE -y -S '1m' $dir)
	local rc=$?
	[ $rc -eq 0 ] || error "migrate failed rc = $rc"
	echo "done"

	# make sure all links have been properly migrated
	echo -n "verifying files..."
	fid=$($LFS getstripe -F "$file1") ||
		error "cannot get fid for file $file1"
	for i in $(seq 2 $total_count); do
		local fid2=$($LFS getstripe -F $dir/file$i)

		[ "$fid2" == "$fid" ] ||
			error "migrated hard link has mismatched FID"
	done

	# make sure hard links were properly detected, and migration was
	# performed only once for the entire link set; nonlinked files should
	# also be migrated
	local actual=$(grep -c 'done' <<< "$migrate_out")
	local expected=$(($uniq_count + 1))

	[ "$actual" -eq  "$expected" ] ||
		error "hard links individually migrated ($actual != $expected)"

	# make sure the correct number of hard links are present
	local hardlinks=$(stat -c '%h' "$file1")

	[ $hardlinks -eq $total_count ] ||
		error "num hard links $hardlinks != $total_count"
	echo "done"

	return 0
}

test_56xb() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	local dir="$DIR/$tdir"

	test_mkdir "$dir" || error "cannot create dir $dir"

	echo "testing lfs migrate mode when all links fit within xattrs"
	LFS_MIGRATE_RSYNC_MODE=false check_migrate_links "$dir" 2 99

	echo "testing rsync mode when all links fit within xattrs"
	LFS_MIGRATE_RSYNC_MODE=true check_migrate_links "$dir" 2 99

	echo "testing lfs migrate mode when all links do not fit within xattrs"
	LFS_MIGRATE_RSYNC_MODE=false check_migrate_links "$dir" 101 100

	echo "testing rsync mode when all links do not fit within xattrs"
	LFS_MIGRATE_RSYNC_MODE=true check_migrate_links "$dir" 101 100


	# clean up
	rm -rf $dir
}
run_test 56xb "lfs migration hard link support"

test_56xc() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	local dir="$DIR/$tdir"

	test_mkdir "$dir" || error "cannot create dir $dir"

	# Test 1: ensure file < 1 GB is always migrated with 1 stripe
	echo -n "Setting initial stripe for 20MB test file..."
	$LFS setstripe -c 2 -i 0 "$dir/20mb" ||
		error "cannot setstripe 20MB file"
	echo "done"
	echo -n "Sizing 20MB test file..."
	truncate "$dir/20mb" 20971520 || error "cannot create 20MB test file"
	echo "done"
	echo -n "Verifying small file autostripe count is 1..."
	$LFS_MIGRATE -y -A -C 1 "$dir/20mb" ||
		error "cannot migrate 20MB file"
	local stripe_count=$($LFS getstripe -c "$dir/20mb") ||
		error "cannot get stripe for $dir/20mb"
	[ $stripe_count -eq 1 ] ||
		error "unexpected stripe count $stripe_count for 20MB file"
	rm -f "$dir/20mb"
	echo "done"

	# Test 2: File is small enough to fit within the available space on
	# sqrt(size_in_gb) + 1 OSTs but is larger than 1GB.  The file must
	# have at least an additional 1KB for each desired stripe for test 3
	echo -n "Setting stripe for 1GB test file..."
	$LFS setstripe -c 1 -i 0 "$dir/1gb" || error "cannot setstripe 1GB file"
	echo "done"
	echo -n "Sizing 1GB test file..."
	# File size is 1GB + 3KB
	truncate "$dir/1gb" 1073744896 || error "cannot create 1GB test file"
	echo "done"

	# need at least 512MB per OST for 1GB file to fit in 2 stripes
	local avail=$($LCTL get_param -n llite.$FSNAME*.kbytesavail)
	if (( avail > 524288 * OSTCOUNT )); then
		echo -n "Migrating 1GB file..."
		$LFS_MIGRATE -y -A -C 1 "$dir/1gb" ||
			error "cannot migrate 1GB file"
		echo "done"
		echo -n "Verifying autostripe count is sqrt(n) + 1..."
		stripe_count=$($LFS getstripe -c "$dir/1gb") ||
			error "cannot getstripe for 1GB file"
		[ $stripe_count -eq 2 ] ||
			error "unexpected stripe count $stripe_count != 2"
		echo "done"
	fi

	# Test 3: File is too large to fit within the available space on
	# sqrt(n) + 1 OSTs.  Simulate limited available space with -X
	if [ $OSTCOUNT -ge 3 ]; then
		# The required available space is calculated as
		# file size (1GB + 3KB) / OST count (3).
		local kb_per_ost=349526

		echo -n "Migrating 1GB file with limit..."
		$LFS_MIGRATE -y -A -C 1 -X $kb_per_ost "$dir/1gb" ||
			error "cannot migrate 1GB file with limit"
		echo "done"

		stripe_count=$($LFS getstripe -c "$dir/1gb")
		echo -n "Verifying 1GB autostripe count with limited space..."
		[ "$stripe_count" -a $stripe_count -ge 3 ] ||
			error "unexpected stripe count $stripe_count (min 3)"
		echo "done"
	fi

	# clean up
	rm -rf $dir
}
run_test 56xc "lfs migration autostripe"

test_56xd() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	local dir=$DIR/$tdir
	local f_mgrt=$dir/$tfile.mgrt
	local f_yaml=$dir/$tfile.yaml
	local f_copy=$dir/$tfile.copy
	local layout_yaml="-E 1M -S 512K -c 1 -E -1 -S 1M -c 2 -i 0"
	local layout_copy="-c 2 -S 2M -i 1"
	local yamlfile=$dir/yamlfile
	local layout_before;
	local layout_after;

	test_mkdir "$dir" || error "cannot create dir $dir"
	$LFS setstripe $layout_yaml $f_yaml ||
		error "cannot setstripe $f_yaml with layout $layout_yaml"
	$LFS getstripe --yaml $f_yaml > $yamlfile
	$LFS setstripe $layout_copy $f_copy ||
		error "cannot setstripe $f_copy with layout $layout_copy"
	touch $f_mgrt
	dd if=/dev/zero of=$f_mgrt bs=1M count=4

	# 1. test option --yaml
	$LFS_MIGRATE -y --yaml $yamlfile $f_mgrt ||
		error "cannot migrate $f_mgrt with --yaml $yamlfile"
	layout_before=$(get_layout_param $f_yaml)
	layout_after=$(get_layout_param $f_mgrt)
	[ "$layout_after" == "$layout_before" ] ||
		error "lfs_migrate --yaml: $layout_after != $layout_before"

	# 2. test option --copy
	$LFS_MIGRATE -y --copy $f_copy $f_mgrt ||
		error "cannot migrate $f_mgrt with --copy $f_copy"
	layout_before=$(get_layout_param $f_copy)
	layout_after=$(get_layout_param $f_mgrt)
	[ "$layout_after" == "$layout_before" ] ||
		error "lfs_migrate --copy: $layout_after != $layout_before"
}
run_test 56xd "check lfs_migrate --yaml and --copy support"

test_56xe() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	local dir=$DIR/$tdir
	local f_comp=$dir/$tfile
	local layout="-E 1M -S 512K -c 1 -E -1 -S 1M -c 2 -i 0"
	local layout_before=""
	local layout_after=""

	test_mkdir "$dir" || error "cannot create dir $dir"
	$LFS setstripe $layout $f_comp ||
		error "cannot setstripe $f_comp with layout $layout"
	layout_before=$(get_layout_param $f_comp)
	dd if=/dev/zero of=$f_comp bs=1M count=4

	# 1. migrate a comp layout file by lfs_migrate
	$LFS_MIGRATE -y $f_comp || error "cannot migrate $f_comp by lfs_migrate"
	layout_after=$(get_layout_param $f_comp)
	[ "$layout_before" == "$layout_after" ] ||
		error "lfs_migrate: $layout_before != $layout_after"

	# 2. migrate a comp layout file by lfs migrate
	$LFS migrate $f_comp || error "cannot migrate $f_comp by lfs migrate"
	layout_after=$(get_layout_param $f_comp)
	[ "$layout_before" == "$layout_after" ] ||
		error "lfs migrate: $layout_before != $layout_after"
}
run_test 56xe "migrate a composite layout file"

test_56y() {
	[ $MDS1_VERSION -lt $(version_code 2.4.53) ] &&
		skip "No HSM $(lustre_build_version $SINGLEMDS) MDS < 2.4.53"

	local res=""
	local dir=$DIR/$tdir
	local f1=$dir/file1
	local f2=$dir/file2

	test_mkdir -p $dir || error "creating dir $dir"
	touch $f1 || error "creating std file $f1"
	$MULTIOP $f2 H2c || error "creating released file $f2"

	# a directory can be raid0, so ask only for files
	res=$($LFS find $dir -L raid0 -type f | wc -l)
	[[ $res == 2 ]] || error "search raid0: found $res files != 2"

	res=$($LFS find $dir \! -L raid0 -type f | wc -l)
	[[ $res == 0 ]] || error "search !raid0: found $res files != 0"

	# only files can be released, so no need to force file search
	res=$($LFS find $dir -L released)
	[[ $res == $f2 ]] || error "search released: found $res != $f2"

	res=$($LFS find $dir -type f \! -L released)
	[[ $res == $f1 ]] || error "search !released: found $res != $f1"
}
run_test 56y "lfs find -L raid0|released"

test_56z() { # LU-4824
	# This checks to make sure 'lfs find' continues after errors
	# There are two classes of errors that should be caught:
	# - If multiple paths are provided, all should be searched even if one
	#   errors out
	# - If errors are encountered during the search, it should not terminate
	#   early
	local dir=$DIR/$tdir
	local i

	test_mkdir $dir
	for i in d{0..9}; do
		test_mkdir $dir/$i
		touch $dir/$i/$tfile
	done
	$LFS find $DIR/non_existent_dir $dir &&
		error "$LFS find did not return an error"
	# Make a directory unsearchable. This should NOT be the last entry in
	# directory order.  Arbitrarily pick the 6th entry
	chmod 700 $($LFS find $dir -type d | sed '6!d')

	$RUNAS $LFS find $DIR/non_existent $dir
	local count=$($RUNAS $LFS find $DIR/non_existent $dir | wc -l)

	# The user should be able to see 10 directories and 9 files
	(( count == 19 )) ||
		error "$LFS find found $count != 19 entries after error"
}
run_test 56z "lfs find should continue after an error"

test_56aa() { # LU-5937
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	local dir=$DIR/$tdir

	mkdir $dir
	$LFS setdirstripe -c$MDSCOUNT $dir/striped_dir

	createmany -o $dir/striped_dir/${tfile}- 1024
	local dirs=$($LFS find --size +8k $dir/)

	[ -n "$dirs" ] || error "lfs find --size wrong under striped dir"
}
run_test 56aa "lfs find --size under striped dir"

test_56ab() { # LU-10705
	test_mkdir $DIR/$tdir
	dd if=/dev/zero of=$DIR/$tdir/$tfile.1 bs=8k count=1 seek=2k
	dd if=/dev/zero of=$DIR/$tdir/$tfile.2 bs=4k count=1 seek=4k
	dd if=/dev/zero of=$DIR/$tdir/$tfile.3 bs=1M count=2 seek=16
	# Flush writes to ensure valid blocks.  Need to be more thorough for
	# ZFS, since blocks are not allocated/returned to client immediately.
	sync_all_data
	wait_zfs_commit ost1 2
	cancel_lru_locks osc
	ls -ls $DIR/$tdir

	local files=$($LFS find --size +16M $DIR/$tdir | wc -l)

	[[ $files == 3 ]] || error ">16M size files $files isn't 3 as expected"

	files=$($LFS find --blocks +1M $DIR/$tdir | wc -l)
	[[ $files == 1 ]] || error ">1M blocks files $files isn't 1 as expected"

	rm -f $DIR/$tdir/$tfile.[123]
}
run_test 56ab "lfs find --blocks"

test_56ba() {
	[ $MDS1_VERSION -lt $(version_code 2.10.50) ] &&
		skip "Need MDS version at least 2.10.50"

	# Create composite files with one component
	local dir=$DIR/$tdir

	setup_56 $dir/1Mfiles 5 1 "-S 1M --component-end 1M"
	# Create composite files with three components
	setup_56 $dir/2Mfiles 5 2 "-E 2M -S 1M -E 4M -E 6M"
	# Create non-composite files
	createmany -o $dir/${tfile}- 10

	local nfiles=$($LFS find --component-end 1M --type f $dir | wc -l)

	[[ $nfiles == 10 ]] ||
		error "lfs find -E 1M found $nfiles != 10 files"

	nfiles=$($LFS find ! -E 1M --type f $dir | wc -l)
	[[ $nfiles == 25 ]] ||
		error "lfs find ! -E 1M found $nfiles != 25 files"

	# All files have a component that starts at 0
	nfiles=$($LFS find --component-start 0 --type f $dir | wc -l)
	[[ $nfiles == 35 ]] ||
		error "lfs find --component-start 0 - $nfiles != 35 files"

	nfiles=$($LFS find --component-start 2M --type f $dir | wc -l)
	[[ $nfiles == 15 ]] ||
		error "lfs find --component-start 2M - $nfiles != 15 files"

	# All files created here have a componenet that does not starts at 2M
	nfiles=$($LFS find ! --component-start 2M --type f $dir | wc -l)
	[[ $nfiles == 35 ]] ||
		error "lfs find ! --component-start 2M - $nfiles != 35 files"

	# Find files with a specified number of components
	local nfiles=$($LFS find --component-count 3 --type f $dir | wc -l)
	[[ $nfiles == 15 ]] ||
		error "lfs find --component-count 3 - $nfiles != 15 files"

	# Remember non-composite files have a component count of zero
	local nfiles=$($LFS find --component-count 0 --type f $dir | wc -l)
	[[ $nfiles == 10 ]] ||
		error "lfs find --component-count 0 - $nfiles != 10 files"

	nfiles=$($LFS find ! --component-count 3 --type f $dir | wc -l)
	[[ $nfiles == 20 ]] ||
		error "lfs find ! --component-count 3 - $nfiles != 20 files"

	# All files have a flag called "init"
	local nfiles=$($LFS find --component-flags init --type f $dir | wc -l)
	[[ $nfiles == 35 ]] ||
		error "lfs find --component-flags init - $nfiles != 35 files"

	# Multi-component files will have a component not initialized
	local nfiles=$($LFS find ! --component-flags init --type f $dir | wc -l)
	[[ $nfiles == 15 ]] ||
		error "lfs find !--component-flags init - $nfiles != 15 files"

	rm -rf $dir

}
run_test 56ba "test lfs find --component-end, -start, -count, and -flags"

test_56ca() {
	[[ $MDS1_VERSION -ge $(version_code 2.10.57) ]] ||
		skip "Need MDS version at least 2.10.57"

	local td=$DIR/$tdir
	local tf=$td/$tfile
	local dir
	local nfiles
	local cmd
	local i
	local j

	# create mirrored directories and mirrored files
	mkdir $td || error "mkdir $td failed"
	$LFS mirror create -N3 $td || error "create mirrored dir $td failed"
	createmany -o $tf- 10 || error "create $tf- failed"

	for i in $(seq 2); do
		dir=$td/dir$i
		mkdir $dir || error "mkdir $dir failed"
		$LFS mirror create -N$((3 + i)) $dir ||
			error "create mirrored dir $dir failed"
		createmany -o $dir/$tfile- 10 ||
			error "create $dir/$tfile- failed"
	done

	# change the states of some mirrored files
	echo foo > $tf-6
	for i in $(seq 2); do
		dir=$td/dir$i
		for j in $(seq 4 9); do
			echo foo > $dir/$tfile-$j
		done
	done

	# find mirrored files with specific mirror count
	cmd="$LFS find --mirror-count 3 --type f $td"
	nfiles=$($cmd | wc -l)
	[[ $nfiles = 10 ]] || error "$cmd: $nfiles != 10 files"

	cmd="$LFS find ! --mirror-count 3 --type f $td"
	nfiles=$($cmd | wc -l)
	[[ $nfiles = 20 ]] || error "$cmd: $nfiles != 20 files"

	cmd="$LFS find --mirror-count +2 --type f $td"
	nfiles=$($cmd | wc -l)
	[[ $nfiles = 30 ]] || error "$cmd: $nfiles != 30 files"

	cmd="$LFS find --mirror-count -6 --type f $td"
	nfiles=$($cmd | wc -l)
	[[ $nfiles = 30 ]] || error "$cmd: $nfiles != 30 files"

	# find mirrored files with specific file state
	cmd="$LFS find --maxdepth 1 --mirror-state=^ro --type f $td"
	[[ $($cmd) = $tf-6 ]] || error "$cmd: didn't return $tf-6"

	cmd="$LFS find --mirror-state=ro --type f $td"
	nfiles=$($cmd | wc -l)
	[[ $nfiles = 17 ]] || error "$cmd: $nfiles != 17 files"

	cmd="$LFS find ! --mirror-state=ro --type f $td"
	nfiles=$($cmd | wc -l)
	[[ $nfiles = 13 ]] || error "$cmd: $nfiles != 13 files"

	cmd="$LFS find --mirror-state=wp --type f $td"
	nfiles=$($cmd | wc -l)
	[[ $nfiles = 13 ]] || error "$cmd: $nfiles != 13 files"

	cmd="$LFS find ! --mirror-state=sp --type f $td"
	nfiles=$($cmd | wc -l)
	[[ $nfiles = 30 ]] || error "$cmd: $nfiles != 30 files"
}
run_test 56ca "check lfs find --mirror-count|-N and --mirror-state"

test_57a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	# note test will not do anything if MDS is not local
	if [ "$mds1_FSTYPE" != ldiskfs ]; then
		skip_env "ldiskfs only test"
	fi
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local MNTDEV="osd*.*MDT*.mntdev"
	DEV=$(do_facet $SINGLEMDS lctl get_param -n $MNTDEV)
	[ -z "$DEV" ] && error "can't access $MNTDEV"
	for DEV in $(do_facet $SINGLEMDS lctl get_param -n $MNTDEV); do
		do_facet $SINGLEMDS $DUMPE2FS -h $DEV > $TMP/t57a.dump ||
			error "can't access $DEV"
		DEVISIZE=$(awk '/Inode size:/ { print $3 }' $TMP/t57a.dump)
		[[ $DEVISIZE -gt 128 ]] || error "inode size $DEVISIZE"
		rm $TMP/t57a.dump
	done
}
run_test 57a "verify MDS filesystem created with large inodes =="

test_57b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	if [ "$mds1_FSTYPE" != ldiskfs ]; then
		skip_env "ldiskfs only test"
	fi
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local dir=$DIR/$tdir
	local filecount=100
	local file1=$dir/f1
	local fileN=$dir/f$filecount

	rm -rf $dir || error "removing $dir"
	test_mkdir -c1 $dir
	local mdtidx=$($LFS getstripe -m $dir)
	local mdtname=MDT$(printf %04x $mdtidx)
	local facet=mds$((mdtidx + 1))

	echo "mcreating $filecount files"
	createmany -m $dir/f 1 $filecount || error "creating files in $dir"

	# verify that files do not have EAs yet
	$LFS getstripe $file1 2>&1 | grep -q "no stripe" ||
		error "$file1 has an EA"
	$LFS getstripe $fileN 2>&1 | grep -q "no stripe" ||
		error "$fileN has an EA"

	sync
	sleep 1
	df $dir  #make sure we get new statfs data
	local mdsfree=$(do_facet $facet \
			lctl get_param -n osd*.*$mdtname.kbytesfree)
	local mdcfree=$(lctl get_param -n mdc.*$mdtname-mdc-*.kbytesfree)
	local file

	echo "opening files to create objects/EAs"
	for file in $(seq -f $dir/f%g 1 $filecount); do
		$OPENFILE -f O_RDWR $file > /dev/null 2>&1 ||
			error "opening $file"
	done

	# verify that files have EAs now
	$LFS getstripe $file1 | grep -q "obdidx" || error "$file1 missing EA"
	$LFS getstripe $fileN | grep -q "obdidx" || error "$fileN missing EA"

	sleep 1  #make sure we get new statfs data
	df $dir
	local mdsfree2=$(do_facet $facet \
			 lctl get_param -n osd*.*$mdtname.kbytesfree)
	local mdcfree2=$(lctl get_param -n mdc.*$mdtname-mdc-*.kbytesfree)

	if [[ $mdcfree2 -lt $((mdcfree - 16)) ]]; then
		if [ "$mdsfree" != "$mdsfree2" ]; then
			error "MDC before $mdcfree != after $mdcfree2"
		else
			echo "MDC before $mdcfree != after $mdcfree2"
			echo "unable to confirm if MDS has large inodes"
		fi
	fi
	rm -rf $dir
}
run_test 57b "default LOV EAs are stored inside large inodes ==="

test_58() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ -z "$(which wiretest 2>/dev/null)" ] &&
			skip_env "could not find wiretest"

	wiretest
}
run_test 58 "verify cross-platform wire constants =============="

test_59() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mgs_nodsh && skip "remote MGS with nodsh"
	do_facet mgs "! which run-llog.sh &> /dev/null" &&
		do_facet mgs "! ls run-llog.sh &> /dev/null" &&
			skip_env "missing subtest run-llog.sh"

	log "$TEST60_HEAD - from kernel mode"
	do_facet mgs "$LCTL dk > /dev/null"
	do_facet mgs "bash run-llog.sh" || error "run-llog.sh failed"
	do_facet mgs $LCTL dk > $TMP/$tfile

	# LU-6388: test llog_reader
	local llog_reader=$(do_facet mgs "which llog_reader 2> /dev/null")
	llog_reader=${llog_reader:-$LUSTRE/utils/llog_reader}
	[ -z $(do_facet mgs ls -d $llog_reader 2> /dev/null) ] &&
			skip_env "missing llog_reader"
	local fstype=$(facet_fstype mgs)
	[ $fstype != ldiskfs -a $fstype != zfs ] &&
		skip_env "Only for ldiskfs or zfs type mgs"

	local mntpt=$(facet_mntpt mgs)
	local mgsdev=$(mgsdevname 1)
	local fid_list
	local fid
	local rec_list
	local rec
	local rec_type
	local obj_file
	local path
	local seq
	local oid
	local pass=true

	#get fid and record list
	fid_list=($(awk '/9_sub.*record/ { print $NF }' /$TMP/$tfile |
		tail -n 4))
	rec_list=($(awk '/9_sub.*record/ { print $((NF-3)) }' /$TMP/$tfile |
		tail -n 4))
	#remount mgs as ldiskfs or zfs type
	stop mgs || error "stop mgs failed"
	mount_fstype mgs || error "remount mgs failed"
	for ((i = 0; i < ${#fid_list[@]}; i++)); do
		fid=${fid_list[i]}
		rec=${rec_list[i]}
		seq=$(echo $fid | awk -F ':' '{ print $1 }' | sed -e "s/^0x//g")
		oid=$(echo $fid | awk -F ':' '{ print $2 }' | sed -e "s/^0x//g")
		oid=$((16#$oid))

		case $fstype in
			ldiskfs )
				obj_file=$mntpt/O/$seq/d$((oid%32))/$oid ;;
			zfs )
				obj_file=$mntpt/oi.$(($((16#$seq))&127))/$fid ;;
		esac
		echo "obj_file is $obj_file"
		do_facet mgs $llog_reader $obj_file

		rec_type=$(do_facet mgs $llog_reader $obj_file | grep "type=" |
			awk '{ print $3 }' | sed -e "s/^type=//g")
		if [ $rec_type != $rec ]; then
			echo "FAILED test_60a wrong record type $rec_type," \
			      "should be $rec"
			pass=false
			break
		fi

		#check obj path if record type is LLOG_LOGID_MAGIC
		if [ "$rec" == "1064553b" ]; then
			path=$(do_facet mgs $llog_reader $obj_file |
				grep "path=" | awk '{ print $NF }' |
				sed -e "s/^path=//g")
			if [ $obj_file != $mntpt/$path ]; then
				echo "FAILED test_60a wrong obj path" \
				      "$montpt/$path, should be $obj_file"
				pass=false
				break
			fi
		fi
	done
	rm -f $TMP/$tfile
	#restart mgs before "error", otherwise it will block the next test
	stop mgs || error "stop mgs failed"
	start mgs $(mgsdevname) $MGS_MOUNT_OPTS || error "start mgs failed"
	$pass || error "test failed, see FAILED test_60a messages for specifics"
}
run_test 60a "llog_test run from kernel module and test llog_reader"

test_60b() { # bug 6411
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	dmesg > $DIR/$tfile
	LLOG_COUNT=$(do_facet mgs dmesg |
		     awk "/$TEST60_HEAD/ { marker = 1; from_marker = 0; }
			  /llog_[a-z]*.c:[0-9]/ {
				if (marker)
					from_marker++
				from_begin++
			  }
			  END {
				if (marker)
					print from_marker
				else
					print from_begin
			  }")

	[[ $LLOG_COUNT -gt 120 ]] &&
		error "CDEBUG_LIMIT not limiting messages ($LLOG_COUNT)" || true
}
run_test 60b "limit repeated messages from CERROR/CWARN"

test_60c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	echo "create 5000 files"
	createmany -o $DIR/f60c- 5000
#define OBD_FAIL_MDS_LLOG_CREATE_FAILED  0x137
	lctl set_param fail_loc=0x80000137
	unlinkmany $DIR/f60c- 5000
	lctl set_param fail_loc=0
}
run_test 60c "unlink file when mds full"

test_60d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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

test_60e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	touch $DIR/$tfile
#define OBD_FAIL_MDS_LLOG_CREATE_FAILED2  0x15b
	do_facet mds1 lctl set_param fail_loc=0x15b
	rm $DIR/$tfile
}
run_test 60e "no space while new llog is being created"

test_60g() {
	local pid

	test_mkdir -c $MDSCOUNT $DIR/$tdir
	$LFS setdirstripe -D -i -1 -c $MDSCOUNT $DIR/$tdir

	(
		local index=0
		while true; do
			mkdir $DIR/$tdir/subdir$index 2>/dev/null
			rmdir $DIR/$tdir/subdir$index 2>/dev/null
			index=$((index + 1))
		done
	) &

	pid=$!

	for i in $(seq 100); do 
		# define OBD_FAIL_OSD_TXN_START    0x19a
		do_facet mds1 lctl set_param fail_loc=0x8000019a
		usleep 100
	done

	kill -9 $pid

	mkdir $DIR/$tdir/new || error "mkdir failed"
	rmdir $DIR/$tdir/new || error "rmdir failed"
}
run_test 60g "transaction abort won't cause MDT hung"

test_60h() {
	[ $MDS1_VERSION -le $(version_code 2.12.52) ] ||
		skip "Need MDS version at least 2.12.52"
	[ $MDSCOUNT -le 2 ] && skip "Need >= 2 MDTs"

	local f

	#define OBD_FAIL_MDS_STRIPE_CREATE	 0x188
	#define OBD_FAIL_MDS_STRIPE_FID		 0x189
	for fail_loc in 0x80000188 0x80000189; do
		do_facet mds1 "$LCTL set_param fail_loc=$fail_loc"
		$LFS mkdir -c $MDSCOUNT -i 0 $DIR/$tdir-$fail_loc ||
			error "mkdir $dir-$fail_loc failed"
		for i in {0..10}; do
			# create may fail on missing stripe
			echo $i > $DIR/$tdir-$fail_loc/$i
		done
		$LFS getdirstripe $DIR/$tdir-$fail_loc ||
			error "getdirstripe $tdir-$fail_loc failed"
		$LFS migrate -m 1 $DIR/$tdir-$fail_loc ||
			error "migrate $tdir-$fail_loc failed"
		$LFS getdirstripe $DIR/$tdir-$fail_loc ||
			error "getdirstripe $tdir-$fail_loc failed"
		pushd $DIR/$tdir-$fail_loc
		for f in *; do
			echo $f | cmp $f - || error "$f data mismatch"
		done
		popd
		rm -rf $DIR/$tdir-$fail_loc
	done
}
run_test 60h "striped directory with missing stripes can be accessed"

test_61a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	f="$DIR/f61"
	dd if=/dev/zero of=$f bs=$PAGE_SIZE count=1 || error "dd $f failed"
	cancel_lru_locks osc
	$MULTIOP $f OSMWUc || error "$MULTIOP $f failed"
	sync
}
run_test 61a "mmap() writes don't make sync hang ================"

test_61b() {
	mmap_mknod_test $DIR/$tfile || error "mmap_mknod_test failed"
}
run_test 61b "mmap() of unstriped file is successful"

# bug 2330 - insufficient obd_match error checking causes LBUG
test_62() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
# Though this test is irrelevant anymore, it helped to reveal some
# other grant bugs (LU-4482), let's keep it.
test_63a() {   # was test_63
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	MAX_DIRTY_MB=$(lctl get_param -n osc.*.max_dirty_mb | head -n 1)

	for i in `seq 10` ; do
		dd if=/dev/zero of=$DIR/f63 bs=8k &
		sleep 5
		kill $!
		sleep 1
	done

	rm -f $DIR/f63 || true
}
run_test 63a "Verify oig_wait interruption does not crash ======="

# bug 2248 - async write errors didn't return to application on sync
# bug 3677 - async write errors left page locked
test_63b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	debugsave
	lctl set_param debug=-1

	# ensure we have a grant to do async writes
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=1
	rm $DIR/$tfile

	sync	# sync lest earlier test intercept the fail_loc

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	df $DIR
	lctl get_param -n osc.*[oO][sS][cC][_-]*.cur* | grep "[0-9]"
}
run_test 64a "verify filter grant calculations (in kernel) ====="

test_64b () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	sh oos.sh $MOUNT || error "oos.sh failed: $?"
}
run_test 64b "check out-of-space detection on client"

test_64c() {
	$LCTL set_param osc.*OST0000-osc-[^mM]*.cur_grant_bytes=0
}
run_test 64c "verify grant shrink"

# this does exactly what osc_request.c:osc_announce_cached() does in
# order to calculate max amount of grants to ask from server
want_grant() {
	local tgt=$1

	local nrpages=$($LCTL get_param -n osc.${tgt}.max_pages_per_rpc)
	local rpc_in_flight=$($LCTL get_param -n osc.${tgt}.max_rpcs_in_flight)

	((rpc_in_flight ++));
	nrpages=$((nrpages * rpc_in_flight))

	local dirty_max_pages=$($LCTL get_param -n osc.${tgt}.max_dirty_mb)

	dirty_max_pages=$((dirty_max_pages * 1024 * 1024 / PAGE_SIZE))

	[[ $dirty_max_pages -gt $nrpages ]] && nrpages=$dirty_max_pages
	local undirty=$((nrpages * PAGE_SIZE))

	local max_extent_pages
	max_extent_pages=$($LCTL get_param osc.${tgt}.import |
	    grep grant_max_extent_size | awk '{print $2}')
	max_extent_pages=$((max_extent_pages / PAGE_SIZE))
	local nrextents=$(((nrpages + max_extent_pages - 1) / max_extent_pages))
	local grant_extent_tax
	grant_extent_tax=$($LCTL get_param osc.${tgt}.import |
	    grep grant_extent_tax | awk '{print $2}')

	undirty=$((undirty + nrextents * grant_extent_tax))

	echo $undirty
}

# this is size of unit for grant allocation. It should be equal to
# what tgt_grant.c:tgt_grant_chunk() calculates
grant_chunk() {
	local tgt=$1
	local max_brw_size
	local grant_extent_tax

	max_brw_size=$($LCTL get_param osc.${tgt}.import |
	    grep max_brw_size | awk '{print $2}')

	grant_extent_tax=$($LCTL get_param osc.${tgt}.import |
	    grep grant_extent_tax | awk '{print $2}')

	echo $(((max_brw_size + grant_extent_tax) * 2))
}

test_64d() {
	[ $OST1_VERSION -lt $(version_code 2.10.56) ] &&
		skip "OST < 2.10.55 doesn't limit grants enough"

	local tgt=$($LCTL dl | grep "0000-osc-[^mM]" | awk '{print $4}')
	local file=$DIR/$tfile

	[[ $($LCTL get_param osc.${tgt}.import |
	     grep "connect_flags:.*grant_param") ]] ||
		skip "no grant_param connect flag"

	local olddebug=$($LCTL get_param -n debug 2> /dev/null)

	$LCTL set_param debug="$OLDDEBUG" 2> /dev/null || true

	local max_cur_granted=$(($(want_grant $tgt) + $(grant_chunk $tgt)))
	stack_trap "rm -f $file" EXIT

	$SETSTRIPE $file -i 0 -c 1
	dd if=/dev/zero of=$file bs=1M count=1000 &
	ddpid=$!

	while true
	do
		local cur_grant=$($LCTL get_param -n osc.${tgt}.cur_grant_bytes)
		if [[ $cur_grant -gt $max_cur_granted ]]
		then
			kill $ddpid
			error "cur_grant $cur_grant > $max_cur_granted"
		fi
		kill -0 $ddpid
		[[ $? -ne 0 ]] && break;
		sleep 2
	done

	rm -f $DIR/$tfile
	wait_delete_completed
	$LCTL set_param debug="$olddebug" 2> /dev/null || true
}
run_test 64d "check grant limit exceed"

# bug 1414 - set/get directories' stripe info
test_65a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/f1
	$LVERIFY $DIR/$tdir $DIR/$tdir/f1 || error "lverify failed"
}
run_test 65a "directory with no stripe info"

test_65b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	local STRIPESIZE=$($GETSTRIPE -S $DIR/$tdir)

	$SETSTRIPE -S $((STRIPESIZE * 2)) -i 0 -c 1 $DIR/$tdir ||
						error "setstripe"
	touch $DIR/$tdir/f2
	$LVERIFY $DIR/$tdir $DIR/$tdir/f2 || error "lverify failed"
}
run_test 65b "directory setstripe -S stripe_size*2 -i 0 -c 1"

test_65c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $OSTCOUNT -lt 2 ] && skip_env "need at least 2 OSTs"

	test_mkdir $DIR/$tdir
	local stripesize=$($GETSTRIPE -S $DIR/$tdir)

	$LFS setstripe -S $((stripesize * 4)) -i 1 \
		-c $((OSTCOUNT - 1)) $DIR/$tdir || error "setstripe"
	touch $DIR/$tdir/f3
	$LVERIFY $DIR/$tdir $DIR/$tdir/f3 || error "lverify failed"
}
run_test 65c "directory setstripe -S stripe_size*4 -i 1 -c $((OSTCOUNT-1))"

test_65d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	local STRIPECOUNT=$($GETSTRIPE -c $DIR/$tdir)
	local STRIPESIZE=$($GETSTRIPE -S $DIR/$tdir)

	if [[ $STRIPECOUNT -le 0 ]]; then
		sc=1
	elif [[ $STRIPECOUNT -gt 2000 ]]; then
#LOV_MAX_STRIPE_COUNT is 2000
		[[ $OSTCOUNT -gt 2000 ]] && sc=2000 || sc=$(($OSTCOUNT - 1))
	else
		sc=$(($STRIPECOUNT - 1))
	fi
	$SETSTRIPE -S $STRIPESIZE -c $sc $DIR/$tdir || error "setstripe"
	touch $DIR/$tdir/f4 $DIR/$tdir/f5
	$LVERIFY $DIR/$tdir $DIR/$tdir/f4 $DIR/$tdir/f5 ||
		error "lverify failed"
}
run_test 65d "directory setstripe -S stripe_size -c stripe_count"

test_65e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir

	$SETSTRIPE $DIR/$tdir || error "setstripe"
	$GETSTRIPE -v $DIR/$tdir | grep "Default" ||
					error "no stripe info failed"
	touch $DIR/$tdir/f6
	$LVERIFY $DIR/$tdir $DIR/$tdir/f6 || error "lverify failed"
}
run_test 65e "directory setstripe defaults"

test_65f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/${tdir}f
	$RUNAS $SETSTRIPE $DIR/${tdir}f && error "setstripe succeeded" || true
}
run_test 65f "dir setstripe permission (should return error) ==="

test_65g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	local STRIPESIZE=$($GETSTRIPE -S $DIR/$tdir)

	$LFS setstripe -S $((STRIPESIZE * 2)) -i 0 -c 1 $DIR/$tdir ||
		error "setstripe -S failed"
	$LFS setstripe -d $DIR/$tdir || error "setstripe -d failed"
	$LFS getstripe -v $DIR/$tdir | grep "Default" ||
		error "delete default stripe failed"
}
run_test 65g "directory setstripe -d"

test_65h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	local STRIPESIZE=$($GETSTRIPE -S $DIR/$tdir)

	$LFS setstripe -S $((STRIPESIZE * 2)) -i 0 -c 1 $DIR/$tdir ||
		error "setstripe -S failed"
	test_mkdir $DIR/$tdir/dd1
	[ $($LFS getstripe -c $DIR/$tdir) = $($GETSTRIPE -c $DIR/$tdir/dd1) ] ||
		error "stripe info inherit failed"
}
run_test 65h "directory stripe info inherit ===================="

test_65i() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	save_layout_restore_at_exit $MOUNT

	# bug6367: set non-default striping on root directory
	$LFS setstripe -S 65536 -c -1 $MOUNT || error "error setting stripe"

	# bug12836: getstripe on -1 default directory striping
	$LFS getstripe $MOUNT || error "getstripe $MOUNT failed"

	# bug12836: getstripe -v on -1 default directory striping
	$LFS getstripe -v $MOUNT || error "getstripe -v $MOUNT failed"

	# bug12836: new find on -1 default directory striping
	$LFS find -mtime -1 $MOUNT > /dev/null || error "find $MOUNT failed"
}
run_test 65i "various tests to set root directory striping"

test_65j() { # bug6367
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	sync; sleep 1

	# if we aren't already remounting for each test, do so for this test
	if [ "$CLEANUP" = ":" -a "$I_MOUNTED" = "yes" ]; then
		cleanup || error "failed to unmount"
		setup
	fi

	save_layout_restore_at_exit $MOUNT

	$SETSTRIPE -d $MOUNT || error "setstripe failed"
}
run_test 65j "set default striping on root directory (bug 6367)="

cleanup_65k() {
	rm -rf $DIR/$tdir
	wait_delete_completed
	do_facet $SINGLEMDS "lctl set_param -n \
		osp.$ost*MDT0000.max_create_count=$max_count"
	do_facet $SINGLEMDS "lctl set_param -n \
		osp.$ost*MDT0000.create_count=$count"
	do_facet $SINGLEMDS lctl --device  %$INACTIVE_OSC activate
	echo $INACTIVE_OSC "is Activate"

	wait_osc_import_state mds ost$(( ostnum + 1 )) FULL
}

test_65k() { # bug11679
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local disable_precreate=true
	[ $MDS1_VERSION -le $(version_code 2.8.54) ] &&
		disable_precreate=false

	echo "Check OST status: "
	local MDS_OSCS=$(do_facet $SINGLEMDS lctl dl |
		awk '/[oO][sS][cC].*md[ts]/ { print $4 }')

	for OSC in $MDS_OSCS; do
		echo $OSC "is active"
		do_facet $SINGLEMDS lctl --device %$OSC activate
	done

	for INACTIVE_OSC in $MDS_OSCS; do
		local ost=$(osc_to_ost $INACTIVE_OSC)
		local ostnum=$(do_facet $SINGLEMDS lctl get_param -n \
			       lov.*md*.target_obd |
			       awk -F: /$ost/'{ print $1 }' | head -n 1)

		mkdir -p $DIR/$tdir
		$SETSTRIPE -i $ostnum -c 1 $DIR/$tdir
		createmany -o $DIR/$tdir/$tfile.$ostnum. 1000

		echo "Deactivate: " $INACTIVE_OSC
		do_facet $SINGLEMDS lctl --device %$INACTIVE_OSC deactivate

		local count=$(do_facet $SINGLEMDS "lctl get_param -n \
			      osp.$ost*MDT0000.create_count")
		local max_count=$(do_facet $SINGLEMDS "lctl get_param -n \
				  osp.$ost*MDT0000.max_create_count")
		$disable_precreate &&
			do_facet $SINGLEMDS "lctl set_param -n \
				osp.$ost*MDT0000.max_create_count=0"

		for idx in $(seq 0 $((OSTCOUNT - 1))); do
			[ -f $DIR/$tdir/$idx ] && continue
			echo "$SETSTRIPE -i $idx -c 1 $DIR/$tdir/$idx"
			$SETSTRIPE -i $idx -c 1 $DIR/$tdir/$idx ||
				{ cleanup_65k;
				  error "setstripe $idx should succeed"; }
			rm -f $DIR/$tdir/$idx || error "rm $idx failed"
		done
		unlinkmany $DIR/$tdir/$tfile.$ostnum. 1000
		rmdir $DIR/$tdir

		do_facet $SINGLEMDS "lctl set_param -n \
			osp.$ost*MDT0000.max_create_count=$max_count"
		do_facet $SINGLEMDS "lctl set_param -n \
			osp.$ost*MDT0000.create_count=$count"
		do_facet $SINGLEMDS lctl --device  %$INACTIVE_OSC activate
		echo $INACTIVE_OSC "is Activate"

		wait_osc_import_state mds ost$(( ostnum + 1 )) FULL
	done
}
run_test 65k "validate manual striping works properly with deactivated OSCs"

test_65l() { # bug 12836
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir -p $DIR/$tdir/test_dir
	$SETSTRIPE -c -1 $DIR/$tdir/test_dir
	$LFS find -mtime -1 $DIR/$tdir >/dev/null
}
run_test 65l "lfs find on -1 stripe dir ========================"

test_65m() {
	local layout=$(save_layout $MOUNT)
	$RUNAS $SETSTRIPE -c 2 $MOUNT && {
		restore_layout $MOUNT $layout
		error "setstripe should fail by non-root users"
	}
	true
}
run_test 65m "normal user can't set filesystem default stripe"

test_65n() {
	[[ $(lustre_version_code $SINGLEMDS) -ge $(version_code 2.12.0) ]] ||
		skip "Need MDS version at least 2.12.0"
	[[ $PARALLEL != "yes" ]] || skip "skip parallel run"

	[[ $OSTCOUNT -ge 2 ]] || skip_env "needs >= 2 OSTs"
	which getfattr > /dev/null 2>&1 || skip_env "no getfattr command"
	which setfattr > /dev/null 2>&1 || skip_env "no setfattr command"

	local root_layout=$(save_layout $MOUNT)
	stack_trap "restore_layout $MOUNT $root_layout" EXIT

	# new subdirectory under root directory should not inherit
	# the default layout from root
	local dir1=$MOUNT/$tdir-1
	mkdir $dir1 || error "mkdir $dir1 failed"
	! getfattr -n trusted.lov $dir1 &> /dev/null ||
		error "$dir1 shouldn't have LOV EA"

	# delete the default layout on root directory
	$LFS setstripe -d $MOUNT || error "delete root default layout failed"

	local dir2=$MOUNT/$tdir-2
	mkdir $dir2 || error "mkdir $dir2 failed"
	! getfattr -n trusted.lov $dir2 &> /dev/null ||
		error "$dir2 shouldn't have LOV EA"

	# set a new striping pattern on root directory
	local def_stripe_size=$($LFS getstripe -S $MOUNT)
	local new_def_stripe_size=$((def_stripe_size * 2))
	$LFS setstripe -S $new_def_stripe_size $MOUNT ||
		error "set stripe size on $MOUNT failed"

	# new file created in $dir2 should inherit the new stripe size from
	# the filesystem default
	local file2=$dir2/$tfile-2
	touch $file2 || error "touch $file2 failed"

	local file2_stripe_size=$($LFS getstripe -S $file2)
	[[ $file2_stripe_size -eq $new_def_stripe_size ]] ||
		error "$file2 didn't inherit stripe size $new_def_stripe_size"

	local dir3=$MOUNT/$tdir-3
	mkdir $dir3 || error "mkdir $dir3 failed"
	# $dir3 shouldn't have LOV EA, but "lfs getstripe -d $dir3" should show
	# the root layout, which is the actual default layout that will be used
	# when new files are created in $dir3.
	local dir3_layout=$(get_layout_param $dir3)
	local root_dir_layout=$(get_layout_param $MOUNT)
	[[ "$dir3_layout" = "$root_dir_layout" ]] ||
		error "$dir3 should show the default layout from $MOUNT"

	# set OST pool on root directory
	local pool=$TESTNAME
	pool_add $pool || error "add $pool failed"
	pool_add_targets $pool 0 $((OSTCOUNT - 1)) 1 ||
		error "add targets to $pool failed"

	$LFS setstripe -p $pool $MOUNT ||
		error "set OST pool on $MOUNT failed"

	# new file created in $dir3 should inherit the pool from
	# the filesystem default
	local file3=$dir3/$tfile-3
	touch $file3 || error "touch $file3 failed"

	local file3_pool=$($LFS getstripe -p $file3)
	[[ "$file3_pool" = "$pool" ]] ||
		error "$file3 didn't inherit OST pool $pool"

	local dir4=$MOUNT/$tdir-4
	mkdir $dir4 || error "mkdir $dir4 failed"
	local dir4_layout=$(get_layout_param $dir4)
	root_dir_layout=$(get_layout_param $MOUNT)
	echo "$LFS getstripe -d $dir4"
	$LFS getstripe -d $dir4
	echo "$LFS getstripe -d $MOUNT"
	$LFS getstripe -d $MOUNT
	[[ "$dir4_layout" = "$root_dir_layout" ]] ||
		error "$dir4 should show the default layout from $MOUNT"

	# new file created in $dir4 should inherit the pool from
	# the filesystem default
	local file4=$dir4/$tfile-4
	touch $file4 || error "touch $file4 failed"

	local file4_pool=$($LFS getstripe -p $file4)
	[[ "$file4_pool" = "$pool" ]] ||
		error "$file4 didn't inherit OST pool $pool"

	# new subdirectory under non-root directory should inherit
	# the default layout from its parent directory
	$LFS setstripe -S $new_def_stripe_size -p $pool $dir4 ||
		error "set directory layout on $dir4 failed"

	local dir5=$dir4/$tdir-5
	mkdir $dir5 || error "mkdir $dir5 failed"

	dir4_layout=$(get_layout_param $dir4)
	local dir5_layout=$(get_layout_param $dir5)
	[[ "$dir4_layout" = "$dir5_layout" ]] ||
		error "$dir5 should inherit the default layout from $dir4"

	# though subdir under ROOT doesn't inherit default layout, but
	# its sub dir/file should be created with default layout.
	[[ $MDSCOUNT -ge 2 ]] || skip_env "needs >= 2 MDTs"
	[[ $MDS1_VERSION -ge $(version_code 2.12.59) ]] ||
	[[ $MDS1_VERSION -ge $(version_code 2.12.3) &&
	   $MDS1_VERSION -lt $(version_code 2.12.50) ]] ||
		skip "Need MDS version at least 2.12.3 or 2.12.59"

	local default_lmv_count=$($LFS getdirstripe -D -c $MOUNT)
	local default_lmv_index=$($LFS getdirstripe -D -i $MOUNT)
	local default_lmv_hash=$($LFS getdirstripe -D -H $MOUNT)

	if [ $default_lmv_hash == "none" ]; then
		stack_trap "$LFS setdirstripe -D -d $MOUNT" EXIT
	else
		stack_trap "$LFS setdirstripe -D -i $default_lmv_index \
			-c $default_lmv_count -H $default_lmv_hash $MOUNT" EXIT
	fi

	$LFS setdirstripe -D -c 2 $MOUNT ||
		error "setdirstripe -D -c 2 failed"
	mkdir $MOUNT/$tdir-6 || error "mkdir $tdir-6 failed"
	local lmv_count=$($LFS getdirstripe -c $MOUNT/$tdir-6)
	[ $lmv_count -eq 2 ] || error "$tdir-6 stripe count $lmv_count"
}
run_test 65n "don't inherit default layout from root for new subdirectories"

# bug 2543 - update blocks count on client
test_66() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	COUNT=${COUNT:-8}
	dd if=/dev/zero of=$DIR/f66 bs=1k count=$COUNT
	sync; sync_all_data; sync; sync_all_data
	cancel_lru_locks osc
	BLOCKS=`ls -s $DIR/f66 | awk '{ print $1 }'`
	[ $BLOCKS -ge $COUNT ] || error "$DIR/f66 blocks $BLOCKS < $COUNT"
}
run_test 66 "update inode blocks count on client ==============="

meminfo() {
	awk '($1 == "'$1':") { print $2 }' /proc/meminfo
}

swap_used() {
	swapon -s | awk '($1 == "'$1'") { print $4 }'
}

# bug5265, obdfilter oa2dentry return -ENOENT
# #define OBD_FAIL_SRV_ENOENT 0x217
test_69() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

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
	test_mkdir $DIR/$tdir
	$LFS setdirstripe -D -c$MDSCOUNT $DIR/$tdir
	sh rundbench -C -D $DIR/$tdir 2 || error "dbench failed!"
}
run_test 71 "Running dbench on lustre (don't segment fault) ===="

test_72a() { # bug 5695 - Test that on 2.6 remove_suid works properly
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ "$RUNAS_ID" = "$UID" ] &&
		skip_env "RUNAS_ID = UID = $UID -- skipping"
	# Check that testing environment is properly set up. Skip if not
	FAIL_ON_ERROR=false check_runas_id_ret $RUNAS_ID $RUNAS_GID $RUNAS ||
		skip_env "User $RUNAS_ID does not exist - skipping"

	touch $DIR/$tfile
	chmod 777 $DIR/$tfile
	chmod ug+s $DIR/$tfile
	$RUNAS dd if=/dev/zero of=$DIR/$tfile bs=512 count=1 ||
		error "$RUNAS dd $DIR/$tfile failed"
	# See if we are still setuid/sgid
	test -u $DIR/$tfile -o -g $DIR/$tfile &&
		error "S/gid is not dropped on write"
	# Now test that MDS is updated too
	cancel_lru_locks mdc
	test -u $DIR/$tfile -o -g $DIR/$tfile &&
		error "S/gid is not dropped on MDS"
	rm -f $DIR/$tfile
}
run_test 72a "Test that remove suid works properly (bug5695) ===="

test_72b() { # bug 24226 -- keep mode setting when size is not changing
	local perm

	[ "$RUNAS_ID" = "$UID" ] &&
		skip_env "RUNAS_ID = UID = $UID -- skipping"
	[ "$RUNAS_ID" -eq 0 ] &&
		skip_env "RUNAS_ID = 0 -- skipping"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	# Check that testing environment is properly set up. Skip if not
	FAIL_ON_ERROR=false check_runas_id_ret $RUNAS_ID $RUNAS_ID $RUNAS ||
		skip_env "User $RUNAS_ID does not exist - skipping"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	touch $DIR/f74a
	#define OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT 0x30e
	#
	# very important to OR with OBD_FAIL_ONCE (0x80000000) -- otherwise it
	# will spin in a tight reconnection loop
	$LCTL set_param fail_loc=0x8000030e
	# get any lock that won't be difficult - lookup works.
	ls $DIR/f74a
	$LCTL set_param fail_loc=0
	rm -f $DIR/f74a
	true
}
run_test 74a "ldlm_enqueue freed-export error path, ls (shouldn't LBUG)"

test_74b() { # bug 13310
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	#define OBD_FAIL_LDLM_ENQUEUE_OLD_EXPORT 0x30e
	#
	# very important to OR with OBD_FAIL_ONCE (0x80000000) -- otherwise it
	# will spin in a tight reconnection loop
	$LCTL set_param fail_loc=0x8000030e
	# get a "difficult" lock
	touch $DIR/f74b
	$LCTL set_param fail_loc=0
	rm -f $DIR/f74b
	true
}
run_test 74b "ldlm_enqueue freed-export error path, touch (shouldn't LBUG)"

test_74c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	#define OBD_FAIL_LDLM_NEW_LOCK
	$LCTL set_param fail_loc=0x319
	touch $DIR/$tfile && error "touch successful"
	$LCTL set_param fail_loc=0
	true
}
run_test 74c "ldlm_lock_create error path, (shouldn't LBUG)"

num_inodes() {
	awk '/lustre_inode_cache/ {print $2; exit}' /proc/slabinfo
}

test_76() { # Now for bug 20433, added originally in bug 1443
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local CPUS=$(getconf _NPROCESSORS_ONLN 2>/dev/null)

	cancel_lru_locks osc
	BEFORE_INODES=$(num_inodes)
	echo "before inodes: $BEFORE_INODES"
	local COUNT=1000
	[ "$SLOW" = "no" ] && COUNT=100
	for i in $(seq $COUNT); do
		touch $DIR/$tfile
		rm -f $DIR/$tfile
	done
	cancel_lru_locks osc
	AFTER_INODES=$(num_inodes)
	echo "after inodes: $AFTER_INODES"
	local wait=0
	while [[ $((AFTER_INODES-1*${CPUS:-1})) -gt $BEFORE_INODES ]]; do
		sleep 2
		AFTER_INODES=$(num_inodes)
		wait=$((wait+2))
		echo "wait $wait seconds inodes: $AFTER_INODES"
		if [ $wait -gt 30 ]; then
			error "inode slab grew from $BEFORE_INODES to $AFTER_INODES"
		fi
	done
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
CKSUM_TYPES=${CKSUM_TYPES:-$(lctl get_param -n osc.*osc-[^mM]*.checksum_type |
			     tr -d [] | head -n1)}
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$GSS && skip_env "could not run with gss"

	[ ! -f $F77_TMP ] && setup_f77
	set_checksums 1
	dd if=$F77_TMP of=$DIR/$tfile bs=1M count=$F77SZ || error "dd error"
	set_checksums 0
	rm -f $DIR/$tfile
}
run_test 77a "normal checksum read/write operation"

test_77b() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$GSS && skip_env "could not run with gss"

	[ ! -f $F77_TMP ] && setup_f77
	#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
	$LCTL set_param fail_loc=0x80000409
	set_checksums 1

	dd if=$F77_TMP of=$DIR/$tfile bs=1M count=$F77SZ conv=sync ||
		error "dd error: $?"
	$LCTL set_param fail_loc=0

	for algo in $CKSUM_TYPES; do
		cancel_lru_locks osc
		set_checksum_type $algo
		#define OBD_FAIL_OSC_CHECKSUM_RECEIVE    0x408
		$LCTL set_param fail_loc=0x80000408
		cmp $F77_TMP $DIR/$tfile || error "file compare failed"
		$LCTL set_param fail_loc=0
	done
	set_checksums 0
	set_checksum_type $ORIG_CSUM_TYPE
	rm -f $DIR/$tfile
}
run_test 77b "checksum error on client write, read"

cleanup_77c() {
	trap 0
	set_checksums 0
	$LCTL set_param osc.*osc-[^mM]*.checksum_dump=0
	$check_ost &&
		do_facet ost1 $LCTL set_param obdfilter.*-OST*.checksum_dump=0
	[ -n "$osc_file_prefix" ] && rm -f ${osc_file_prefix}*
	$check_ost && [ -n "$ost_file_prefix" ] &&
		do_facet ost1 rm -f ${ost_file_prefix}\*
}

test_77c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$GSS && skip_env "could not run with gss"
	remote_ost_nodsh && skip "remote OST with nodsh"

	local bad1
	local osc_file_prefix
	local osc_file
	local check_ost=false
	local ost_file_prefix
	local ost_file
	local orig_cksum
	local dump_cksum
	local fid

	# ensure corruption will occur on first OSS/OST
	$LFS setstripe -i 0 $DIR/$tfile

	[ ! -f $F77_TMP ] && setup_f77
	dd if=$F77_TMP of=$DIR/$tfile bs=1M count=$F77SZ conv=sync ||
		error "dd write error: $?"
	fid=$($LFS path2fid $DIR/$tfile)

	if [ $OST1_VERSION -ge $(version_code 2.9.57) ]
	then
		check_ost=true
		ost_file_prefix=$(do_facet ost1 $LCTL get_param -n debug_path)
		ost_file_prefix=${ost_file_prefix}-checksum_dump-ost-\\${fid}
	else
		echo "OSS do not support bulk pages dump upon error"
	fi

	osc_file_prefix=$($LCTL get_param -n debug_path)
	osc_file_prefix=${osc_file_prefix}-checksum_dump-osc-\\${fid}

	trap cleanup_77c EXIT

	set_checksums 1
	# enable bulk pages dump upon error on Client
	$LCTL set_param osc.*osc-[^mM]*.checksum_dump=1
	# enable bulk pages dump upon error on OSS
	$check_ost &&
		do_facet ost1 $LCTL set_param obdfilter.*-OST*.checksum_dump=1

	# flush Client cache to allow next read to reach OSS
	cancel_lru_locks osc

	#define OBD_FAIL_OSC_CHECKSUM_RECEIVE       0x408
	$LCTL set_param fail_loc=0x80000408
	dd if=$DIR/$tfile of=/dev/null bs=1M || error "dd read error: $?"
	$LCTL set_param fail_loc=0

	rm -f $DIR/$tfile

	# check cksum dump on Client
	osc_file=$(ls ${osc_file_prefix}*)
	[ -n "$osc_file" ] || error "no checksum dump file on Client"
	# OBD_FAIL_OSC_CHECKSUM_RECEIVE corrupts with "bad1" at start of file
	bad1=$(dd if=$osc_file bs=1 count=4 2>/dev/null) || error "dd error: $?"
	[ $bad1 == "bad1" ] || error "unexpected corrupt pattern"
	orig_cksum=$(dd if=$F77_TMP bs=1 skip=4 count=1048572 2>/dev/null |
		     cksum)
	dump_cksum=$(dd if=$osc_file bs=1 skip=4 2>/dev/null | cksum)
	[[ "$orig_cksum" == "$dump_cksum" ]] ||
		error "dump content does not match on Client"

	$check_ost || skip "No need to check cksum dump on OSS"

	# check cksum dump on OSS
	ost_file=$(do_facet ost1 ls ${ost_file_prefix}\*)
	[ -n "$ost_file" ] || error "no checksum dump file on OSS"
	orig_cksum=$(dd if=$F77_TMP bs=1048576 count=1 2>/dev/null | cksum)
	dump_cksum=$(do_facet ost1 dd if=$ost_file 2>/dev/null \| cksum)
	[[ "$orig_cksum" == "$dump_cksum" ]] ||
		error "dump content does not match on OSS"

	cleanup_77c
}
run_test 77c "checksum error on client read with debug"

test_77d() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$GSS && skip_env "could not run with gss"

	#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
	$LCTL set_param fail_loc=0x80000409
	set_checksums 1
	$DIRECTIO write $DIR/$tfile 0 $F77SZ $((1024 * 1024)) ||
		error "direct write: rc=$?"
	$LCTL set_param fail_loc=0
	set_checksums 0

	#define OBD_FAIL_OSC_CHECKSUM_RECEIVE    0x408
	$LCTL set_param fail_loc=0x80000408
	set_checksums 1
	cancel_lru_locks osc
	$DIRECTIO read $DIR/$tfile 0 $F77SZ $((1024 * 1024)) ||
		error "direct read: rc=$?"
	$LCTL set_param fail_loc=0
	set_checksums 0
}
run_test 77d "checksum error on OST direct write, read"

test_77f() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$GSS && skip_env "could not run with gss"

	set_checksums 1
	for algo in $CKSUM_TYPES; do
		cancel_lru_locks osc
		set_checksum_type $algo
		#define OBD_FAIL_OSC_CHECKSUM_SEND       0x409
		$LCTL set_param fail_loc=0x409
		$DIRECTIO write $DIR/$tfile 0 $F77SZ $((1024 * 1024)) &&
			error "direct write succeeded"
		$LCTL set_param fail_loc=0
	done
	set_checksum_type $ORIG_CSUM_TYPE
	set_checksums 0
}
run_test 77f "repeat checksum error on write (expect error)"

test_77g() { # bug 10889
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$GSS && skip_env "could not run with gss"
	remote_ost_nodsh && skip "remote OST with nodsh"

	[ ! -f $F77_TMP ] && setup_f77

	local file=$DIR/$tfile
	stack_trap "rm -f $file" EXIT

	$SETSTRIPE -c 1 -i 0 $file
	#define OBD_FAIL_OST_CHECKSUM_RECEIVE       0x21a
	do_facet ost1 lctl set_param fail_loc=0x8000021a
	set_checksums 1
	dd if=$F77_TMP of=$file bs=1M count=$F77SZ ||
		error "write error: rc=$?"
	do_facet ost1 lctl set_param fail_loc=0
	set_checksums 0

	cancel_lru_locks osc
	#define OBD_FAIL_OST_CHECKSUM_SEND          0x21b
	do_facet ost1 lctl set_param fail_loc=0x8000021b
	set_checksums 1
	cmp $F77_TMP $file || error "file compare failed"
	do_facet ost1 lctl set_param fail_loc=0
	set_checksums 0
}
run_test 77g "checksum error on OST write, read"

test_77k() { # LU-10906
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$GSS && skip_env "could not run with gss"

	local cksum_param="osc.$FSNAME*.checksums"
	local get_checksum="$LCTL get_param -n $cksum_param | head -n1"
	local checksum
	local i

	[ "$ORIG_CSUM" ] || ORIG_CSUM=$(eval $get_checksum)
	stack_trap "wait_update $HOSTNAME '$get_checksum' $ORIG_CSUM" EXIT
	stack_trap "do_facet mgs $LCTL set_param -P $cksum_param=$ORIG_CSUM" \
		EXIT

	for i in 0 1; do
		do_facet mgs $LCTL set_param -P $cksum_param=$i ||
			error "failed to set checksum=$i on MGS"
		wait_update $HOSTNAME "$get_checksum" $i
		#remount
		echo "remount client, checksum should be $i"
		remount_client $MOUNT || "failed to remount client"
		checksum=$(eval $get_checksum)
		[ $checksum -eq $i ] || error "checksum($checksum) != $i"
	done

	for opt in "checksum" "nochecksum"; do
		#remount with mount option
		echo "remount client with option $opt, checksum should be $i"
		umount_client $MOUNT || "failed to umount client"
		mount_client $MOUNT "$MOUNT_OPTS,$opt" ||
			"failed to mount client with option '$opt'"
		checksum=$(eval $get_checksum)
		[ $checksum -eq $i ] || error "checksum($checksum) != $i"
		i=$((i - 1))
	done

	remount_client $MOUNT || "failed to remount client"
}
run_test 77k "enable/disable checksum correctly"

[ "$ORIG_CSUM" ] && set_checksums $ORIG_CSUM || true
rm -f $F77_TMP
unset F77_TMP

cleanup_test_78() {
	trap 0
	rm -f $DIR/$tfile
}

test_78() { # bug 10901
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost || skip_env "local OST"

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

	[[ $F78SIZE -gt $MEMTOTAL ]] && F78SIZE=$MEMTOTAL
	[[ $F78SIZE -gt 512 ]] && F78SIZE=512
	[[ $F78SIZE -gt $((MAXFREE / 1024)) ]] && F78SIZE=$((MAXFREE / 1024))
	SMALLESTOST=$($LFS df $DIR | grep OST | awk '{ print $4 }' | sort -n |
		head -n1)
	echo "Smallest OST: $SMALLESTOST"
	[[ $SMALLESTOST -lt 10240 ]] &&
		skip "too small OSTSIZE, useless to run large O_DIRECT test"

	trap cleanup_test_78 EXIT

	[[ $F78SIZE -gt $((SMALLESTOST * $OSTCOUNT / 1024 - 80)) ]] &&
		F78SIZE=$((SMALLESTOST * $OSTCOUNT / 1024 - 80))

	[ "$SLOW" = "no" ] && NSEQ=1 && [ $F78SIZE -gt 32 ] && F78SIZE=32
	echo "File size: $F78SIZE"
	$SETSTRIPE -c $OSTCOUNT $DIR/$tfile || error "setstripe failed"
	for i in $(seq 1 $NSEQ); do
		FSIZE=$(($F78SIZE / ($NSEQ - $i + 1)))
		echo directIO rdwr round $i of $NSEQ
		$DIRECTIO rdwr $DIR/$tfile 0 $FSIZE 1048576||error "rdwr failed"
	done

	cleanup_test_78
}
run_test 78 "handle large O_DIRECT writes correctly ============"

test_79() { # bug 12743
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	# relax strong synchronous semantics for slow backends like ZFS
	if [ "$ost1_FSTYPE" != "ldiskfs" ]; then
		local soc="obdfilter.*.sync_lock_cancel"
		local save=$(do_facet ost1 $LCTL get_param -n $soc | head -n1)

		# "sync_on_lock_cancel" was broken by v2_11_55_0-26-g7059644e9a
		if [ -z "$save" ]; then
			soc="obdfilter.*.sync_on_lock_cancel"
			save=$(do_facet ost1 $LCTL get_param -n $soc | head -n1)
		fi

		if [ "$save" != "never" ]; then
			local hosts=$(comma_list $(osts_nodes))

			do_nodes $hosts $LCTL set_param $soc=never
			stack_trap "do_nodes $hosts $LCTL set_param $soc=$save"
		fi
	fi

	dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 seek=1M
	sync; sleep 1; sync
	local before=$(date +%s)
	cancel_lru_locks osc
	local after=$(date +%s)
	local diff=$((after - before))
	[ $diff -le 1 ] || error "elapsed for 1M@1T = $diff"

	rm -f $DIR/$tfile
}
run_test 80 "Page eviction is equally fast at high offsets too"

test_81a() { # LU-456
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

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
run_test 82 "Basic grouplock test"

test_99() {
	[ -z "$(which cvs 2>/dev/null)" ] && skip_env "could not find cvs"

	test_mkdir $DIR/$tdir.cvsroot
	chown $RUNAS_ID $DIR/$tdir.cvsroot

	cd $TMP
	$RUNAS cvs -d $DIR/$tdir.cvsroot init || error "cvs init failed"

	cd /etc/init.d
	# some versions of cvs import exit(1) when asked to import links or
	# files they can't read.  ignore those files.
	local toignore=$(find . -type l -printf '-I %f\n' -o \
			 ! -perm /4 -printf '-I %f\n')
	$RUNAS cvs -d $DIR/$tdir.cvsroot import -m "nomesg" $toignore \
		$tdir.reposname vtag rtag

	cd $DIR
	test_mkdir $DIR/$tdir.reposname
	chown $RUNAS_ID $DIR/$tdir.reposname
	$RUNAS cvs -d $DIR/$tdir.cvsroot co $tdir.reposname

	cd $DIR/$tdir.reposname
	$RUNAS touch foo99
	$RUNAS cvs add -m 'addmsg' foo99
	$RUNAS cvs update
	$RUNAS cvs commit -m 'nomsg' foo99
	rm -fr $DIR/$tdir.cvsroot
}
run_test 99 "cvs strange file/directory operations"

test_100() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ "$NETTYPE" =~ tcp ]] ||
		skip_env "TCP secure port test, not useful for NETTYPE=$NETTYPE"
	remote_ost_nodsh && skip "remote OST with nodsh"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_servers ||
		skip "useless for local single node setup"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -ge 2 ] && skip_env "needs < 2 MDTs" #LU-4322

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
	for s in $($LCTL get_param -n llite.*.read_ahead_stats |
		get_named_value 'read but discarded' | cut -d" " -f1); do
			discard=$(($discard + $s))
	done
	cleanup_101a

	if [[ $(($discard * 10)) -gt $nreads ]]; then
		$LCTL get_param osc.*-osc*.rpc_stats
		$LCTL get_param llite.*.read_ahead_stats
		error "too many ($discard) discarded pages"
	fi
	rm -f $DIR/$tfile || true
}
run_test 101a "check read-ahead for random reads"

setup_test101bc() {
	test_mkdir $DIR/$tdir
	local STRIPE_SIZE=$1
	local FILE_LENGTH=$2
	STRIPE_OFFSET=0

	local FILE_SIZE_MB=$((FILE_LENGTH / STRIPE_SIZE))

	local list=$(comma_list $(osts_nodes))
	set_osd_param $list '' read_cache_enable 0
	set_osd_param $list '' writethrough_cache_enable 0

	trap cleanup_test101bc EXIT
	# prepare the read-ahead file
	$SETSTRIPE -S $STRIPE_SIZE -i $STRIPE_OFFSET -c $OSTCOUNT $DIR/$tfile

	dd if=/dev/zero of=$DIR/$tfile bs=$STRIPE_SIZE \
				count=$FILE_SIZE_MB 2> /dev/null

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
	local STRIPE_SIZE=$2
	local FILE_LENGTH=$3
	local RA_INC=1048576
	local STRIDE_LENGTH=$((STRIPE_SIZE/READ_SIZE))
	local discard_limit=$((((STRIDE_LENGTH - 1)*3/(STRIDE_LENGTH*OSTCOUNT))* \
			     (STRIDE_LENGTH*OSTCOUNT - STRIDE_LENGTH)))
	DISCARD=$($LCTL get_param -n llite.*.read_ahead_stats |
			get_named_value 'read but discarded' |
			cut -d" " -f1 | calc_total)
	if [[ $DISCARD -gt $discard_limit ]]; then
		$LCTL get_param llite.*.read_ahead_stats
		error "Too many ($DISCARD) discarded pages with size (${READ_SIZE})"
	else
		echo "Read-ahead success for size ${READ_SIZE}"
	fi
}

test_101b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	local STRIPE_SIZE=1048576
	local STRIDE_SIZE=$((STRIPE_SIZE*OSTCOUNT))

	if [ $SLOW == "yes" ]; then
		local FILE_LENGTH=$((STRIDE_SIZE * 64))
	else
		local FILE_LENGTH=$((STRIDE_SIZE * 8))
	fi

	local ITERATION=$((FILE_LENGTH / STRIDE_SIZE))

	# prepare the read-ahead file
	setup_test101bc $STRIPE_SIZE $FILE_LENGTH
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
		ra_check_101 $BSIZE $STRIPE_SIZE $FILE_LENGTH
	done
	cleanup_test101bc
	true
}
run_test 101b "check stride-io mode read-ahead ================="

test_101c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local STRIPE_SIZE=1048576
	local FILE_LENGTH=$((STRIPE_SIZE*100))
	local nreads=10000
	local osc_rpc_stats

	setup_test101bc $STRIPE_SIZE $FILE_LENGTH

	cancel_lru_locks osc
	$LCTL set_param osc.*.rpc_stats 0
	$READS -f $DIR/$tfile -s$FILE_LENGTH -b65536 -n$nreads -t 180
	for osc_rpc_stats in $($LCTL get_param -N osc.*.rpc_stats); do
		local stats=$($LCTL get_param -n $osc_rpc_stats)
		local lines=$(echo "$stats" | awk 'END {print NR;}')
		local size

		if [ $lines -le 20 ]; then
			continue
		fi
		for size in 1 2 4 8; do
			local rpc=$(echo "$stats" |
				    awk '($1 == "'$size':") {print $2; exit; }')
			[ $rpc != 0 ] &&
				error "Small $((size*4))k read IO $rpc !"
		done
		echo "$osc_rpc_stats check passed!"
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local file=$DIR/$tfile
	local sz_MB=${FILESIZE_101d:-500}
	local ra_MB=${READAHEAD_MB:-40}

	local free_MB=$(($(df -P $DIR | tail -n 1 | awk '{ print $4 }') / 1024))
	[ $free_MB -lt $sz_MB ] &&
		skip "Need free space ${sz_MB}M, have ${free_MB}M"

	echo "Create test file $file size ${sz_MB}M, ${free_MB}M free"
	$SETSTRIPE -c -1 $file || error "setstripe failed"

	dd if=/dev/zero of=$file bs=1M count=$sz_MB || error "dd failed"
	echo Cancel LRU locks on lustre client to flush the client cache
	cancel_lru_locks osc

	echo Disable read-ahead
	local old_READAHEAD=$(set_read_ahead 0)

	echo Reading the test file $file with read-ahead disabled
	local raOFF=$(do_and_time "dd if=$file of=/dev/null bs=1M count=$sz_MB")

	echo Cancel LRU locks on lustre client to flush the client cache
	cancel_lru_locks osc
	echo Enable read-ahead with ${ra_MB}MB
	set_read_ahead $ra_MB

	echo Reading the test file $file with read-ahead enabled
	local raON=$(do_and_time "dd if=$file of=/dev/null bs=1M count=$sz_MB")

	echo "read-ahead disabled time read $raOFF"
	echo "read-ahead enabled  time read $raON"

	set_read_ahead $old_READAHEAD
	rm -f $file
	wait_delete_completed

	[ $raOFF -le 1 -o $raON -lt $raOFF ] ||
		error "readahead ${raON}s > no-readahead ${raOFF}s ${sz_MB}M"
}
run_test 101d "file read with and without read-ahead enabled"

test_101e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local file=$DIR/$tfile
	local size_KB=500  #KB
	local count=100
	local bsize=1024

	local free_KB=$(df -P $DIR | tail -n 1 | awk '{ print $4 }')
	local need_KB=$((count * size_KB))
	[[ $free_KB -le $need_KB ]] &&
		skip_env "Need free space $need_KB, have $free_KB"

	echo "Creating $count ${size_KB}K test files"
	for ((i = 0; i < $count; i++)); do
		dd if=/dev/zero of=$file.$i bs=$bsize count=$size_KB 2>/dev/null
	done

	echo "Cancel LRU locks on lustre client to flush the client cache"
	cancel_lru_locks $OSC

	echo "Reset readahead stats"
	$LCTL set_param -n llite.*.read_ahead_stats 0

	for ((i = 0; i < $count; i++)); do
		dd if=$file.$i of=/dev/null bs=$bsize count=$size_KB 2>/dev/null
	done

	local miss=$($LCTL get_param -n llite.*.read_ahead_stats |
		     get_named_value 'misses' | cut -d" " -f1 | calc_total)

	for ((i = 0; i < $count; i++)); do
		rm -rf $file.$i 2>/dev/null
	done

	#10000 means 20% reads are missing in readahead
	[[ $miss -lt 10000 ]] ||  error "misses too much for small reads"
}
run_test 101e "check read-ahead for small read(1k) for small files(500k)"

test_101f() {
	which iozone || skip_env "no iozone installed"

	local old_debug=$($LCTL get_param debug)
	old_debug=${old_debug#*=}
	$LCTL set_param debug="reada mmap"

	# create a test file
	iozone -i 0 -+n -r 1m -s 128m -w -f $DIR/$tfile > /dev/null 2>&1

	echo Cancel LRU locks on lustre client to flush the client cache
	cancel_lru_locks osc

	echo Reset readahead stats
	$LCTL set_param -n llite.*.read_ahead_stats 0

	echo mmap read the file with small block size
	iozone -i 1 -u 1 -l 1 -+n -r 32k -s 128m -B -f $DIR/$tfile \
		> /dev/null 2>&1

	echo checking missing pages
	$LCTL get_param llite.*.read_ahead_stats
	local miss=$($LCTL get_param -n llite.*.read_ahead_stats |
			get_named_value 'misses' | cut -d" " -f1 | calc_total)

	$LCTL set_param debug="$old_debug"
	[ $miss -lt 3 ] || error "misses too much pages ('$miss')!"
	rm -f $DIR/$tfile
}
run_test 101f "check mmap read performance"

test_101g_brw_size_test() {
	local mb=$1
	local pages=$((mb * 1048576 / PAGE_SIZE))
	local file=$DIR/$tfile

	$LCTL set_param osc.*.max_pages_per_rpc=${mb}M ||
		{ error "unable to set max_pages_per_rpc=${mb}M"; return 1; }
	for mp in $($LCTL get_param -n osc.*.max_pages_per_rpc); do
		[ $mp -ne $pages ] && error "max_pages_per_rpc $mp != $pages" &&
			return 2
	done

	stack_trap "rm -f $file" EXIT
	$LCTL set_param -n osc.*.rpc_stats=0

	# 10 RPCs should be enough for the test
	local count=10
	dd if=/dev/zero of=$file bs=${mb}M count=$count ||
		{ error "dd write ${mb} MB blocks failed"; return 3; }
	cancel_lru_locks osc
	dd of=/dev/null if=$file bs=${mb}M count=$count ||
		{ error "dd write ${mb} MB blocks failed"; return 4; }

	# calculate number of full-sized read and write RPCs
	rpcs=($($LCTL get_param -n 'osc.*.rpc_stats' |
		sed -n '/pages per rpc/,/^$/p' |
		awk '/'$pages':/ { reads += $2; writes += $6 }; \
		END { print reads,writes }'))
	[ ${rpcs[0]} -ne $count ] && error "${rpcs[0]} != $count read RPCs" &&
		return 5
	[ ${rpcs[1]} -ne $count ] && error "${rpcs[1]} != $count write RPCs" &&
		return 6

	return 0
}

test_101g() {
	remote_ost_nodsh && skip "remote OST with nodsh"

	local rpcs
	local osts=$(get_facets OST)
	local list=$(comma_list $(osts_nodes))
	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"
	local brw_size="obdfilter.*.brw_size"

	$LFS setstripe -i 0 -c 1 $DIR/$tfile

	local orig_mb=$(do_facet ost1 $LCTL get_param -n $brw_size | head -n 1)
	if [ $OST1_VERSION -ge $(version_code 2.8.52) -o \
	     \( $OST1_VERSION -ge $(version_code 2.7.17) -a \
		$OST1_VERSION -lt $(version_code 2.7.50) \) ] &&
	   [ $CLIENT_VERSION -ge $(version_code 2.8.52) -o \
	     \( $CLIENT_VERSION -ge $(version_code 2.7.17) -a \
		$CLIENT_VERSION -lt $(version_code 2.7.50) \) ]; then
		[ $OST1_VERSION -ge $(version_code 2.9.52) ] && suffix="M"
		if [[ $orig_mb -lt 16 ]]; then
			save_lustre_params $osts "$brw_size" > $p
			do_nodes $list $LCTL set_param -n $brw_size=16$suffix ||
				error "set 16MB RPC size failed"

			echo "remount client to enable new RPC size"
			remount_client $MOUNT || error "remount_client failed"
		fi

		test_101g_brw_size_test 16 || error "16MB RPC test failed"
		# should be able to set brw_size=12, but no rpc_stats for that
		test_101g_brw_size_test 8 || error "8MB RPC test failed"
	fi

	test_101g_brw_size_test 4 || error "4MB RPC test failed"

	if [[ $orig_mb -lt 16 ]]; then
		restore_lustre_params < $p
		remount_client $MOUNT || error "remount_client restore failed"
	fi

	rm -f $p $DIR/$tfile
}
run_test 101g "Big bulk(4/16 MiB) readahead"

setup_test102() {
	test_mkdir $DIR/$tdir
	chown $RUNAS_ID $DIR/$tdir
	STRIPE_SIZE=65536
	STRIPE_OFFSET=1
	STRIPE_COUNT=$OSTCOUNT
	[[ $OSTCOUNT -gt 4 ]] && STRIPE_COUNT=4

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
	$1 tar cf $TMP/f102.tar $tdir --xattrs
}

cleanup_test102() {
	trap 0
	rm -f $TMP/f102.tar
	rm -rf $DIR/d0.sanity/d102
}

test_102a() {
	[ "$UID" != 0 ] && skip "must run as root"
	[ -z "$(lctl get_param -n mdc.*-mdc-*.connect_flags | grep xattr)" ] &&
		skip_env "must have user_xattr"

	[ -z "$(which setfattr 2>/dev/null)" ] &&
		skip_env "could not find setfattr"

	local testfile=$DIR/$tfile

	touch $testfile
	echo "set/get xattr..."
	setfattr -n trusted.name1 -v value1 $testfile ||
		error "setfattr -n trusted.name1=value1 $testfile failed"
	getfattr -n trusted.name1 $testfile 2> /dev/null |
	  grep "trusted.name1=.value1" ||
		error "$testfile missing trusted.name1=value1"

	setfattr -n user.author1 -v author1 $testfile ||
		error "setfattr -n user.author1=author1 $testfile failed"
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
	echo "set lustre special xattr ..."
	$LFS setstripe -c1 $testfile
	local lovea=$(getfattr -n "trusted.lov" -e hex $testfile |
		awk -F "=" '/trusted.lov/ { print $2 }' )
	setfattr -n "trusted.lov" -v $lovea $testfile ||
		error "$testfile doesn't ignore setting trusted.lov again"
	setfattr -n "trusted.lov" -v "invalid_value" $testfile &&
		error "$testfile allow setting invalid trusted.lov"
	rm -f $testfile
}
run_test 102a "user xattr test =================================="

check_102b_layout() {
	local layout="$*"
	local testfile=$DIR/$tfile

	echo "test layout '$layout'"
	$LFS setstripe $layout $testfile || error "setstripe failed"
	$LFS getstripe -y $testfile

	echo "get/set/list trusted.lov xattr ..." # b=10930
	local value=$(getfattr -n trusted.lov -e hex $testfile | grep trusted)
	[[ "$value" =~ "trusted.lov" ]] ||
		error "can't get trusted.lov from $testfile"
	local stripe_count_orig=$($LFS getstripe -c $testfile) ||
		error "getstripe failed"

	$MCREATE $testfile.2 || error "mcreate $testfile.2 failed"

	value=$(cut -d= -f2 <<<$value)
	# LU-13168: truncated xattr should fail if short lov_user_md header
	[ $CLIENT_VERSION -lt $(version_code 2.13.53) ] &&
		lens="${#value}" || lens="$(seq 4 2 ${#value})"
	for len in $lens; do
		echo "setfattr $len $testfile.2"
		setfattr -n trusted.lov -v ${value:0:$len} $testfile.2 &&
			[ $len -lt 66 ] && error "short xattr len=$len worked"
	done
	local stripe_size=$($LFS getstripe -S $testfile.2)
	local stripe_count=$($LFS getstripe -c $testfile.2)
	[[ $stripe_size -eq 65536 ]] ||
		error "stripe size $stripe_size != 65536"
	[[ $stripe_count -eq $stripe_count_orig ]] ||
		error "stripe count $stripe_count != $stripe_count_orig"
	rm $testfile $testfile.2
}

test_102b() {
	[ -z "$(which setfattr 2>/dev/null)" ] &&
		skip_env "could not find setfattr"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	# check plain layout
	check_102b_layout -S 65536 -i 1 -c $OSTCOUNT

	# and also check composite layout
	check_102b_layout -E 1M -S 65536 -i 1 -c $OSTCOUNT -Eeof -S4M

}
run_test 102b "getfattr/setfattr for trusted.lov EAs"

test_102c() {
	[ -z "$(which setfattr 2>/dev/null)" ] &&
		skip_env "could not find setfattr"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	# b10930: get/set/list lustre.lov xattr
	echo "get/set/list lustre.lov xattr ..."
	test_mkdir $DIR/$tdir
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
				stripe_size=$($LFS getstripe -S $PWD/$file)
				[[ $stripe_size -ne $size ]] &&
				    error "$file: size $stripe_size != $size"
				stripe_count=$($LFS getstripe -c $PWD/$file)
				# allow fewer stripes to be created, ORI-601
				[[ $stripe_count -lt $(((3 * count + 3) / 4)) ]] &&
				    error "$file: count $stripe_count != $count"
				stripe_index=$($LFS getstripe -i $PWD/$file)
				[[ $stripe_index -ne 0 ]] &&
					stripe_index_all_zero=false
			done
		done
	done
	$stripe_index_all_zero &&
		error "all files are being extracted starting from OST index 0"
	return 0
}

have_xattrs_include() {
	tar --help | grep -q xattrs-include &&
		echo --xattrs-include="lustre.*"
}

test_102d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	XINC=$(have_xattrs_include)
	setup_test102
	tar xf $TMP/f102.tar -C $DIR/$tdir --xattrs $XINC
	cd $DIR/$tdir/$tdir
	compare_stripe_info1
}
run_test 102d "tar restore stripe info from tarfile,not keep osts"

test_102f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	XINC=$(have_xattrs_include)
	setup_test102
	test_mkdir $DIR/$tdir.restore
	cd $DIR
	tar cf - --xattrs $tdir | tar xf - \
		-C $DIR/$tdir.restore --xattrs $XINC
	cd $DIR/$tdir.restore/$tdir
	compare_stripe_info1
}
run_test 102f "tar copy files, not keep osts"

grow_xattr() {
	[ -z "$(lctl get_param -n mdc.*.connect_flags | grep xattr)" ] &&
		skip "must have user_xattr"
	[ -z "$(which setfattr 2>/dev/null)" ] &&
		skip_env "could not find setfattr"
	[ -z "$(which getfattr 2>/dev/null)" ] &&
		skip_env "could not find getfattr"

	local xsize=${1:-1024}	# in bytes
	local file=$DIR/$tfile
	local value="$(generate_string $xsize)"
	local xbig=trusted.big

	touch $file
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
	large_xattr_enabled || skip_env "ea_inode feature disabled"

	grow_xattr $(max_xattr_size)
}
run_test 102ha "grow xattr from inside inode to external inode"

test_102i() { # bug 17038
	[ -z "$(which getfattr 2>/dev/null)" ] &&
		skip "could not find getfattr"

	touch $DIR/$tfile
	ln -s $DIR/$tfile $DIR/${tfile}link
	getfattr -n trusted.lov $DIR/$tfile ||
		error "lgetxattr on $DIR/$tfile failed"
	getfattr -h -n trusted.lov $DIR/${tfile}link 2>&1 |
		grep -i "no such attr" ||
		error "error for lgetxattr on $DIR/${tfile}link is not ENODATA"
	rm -f $DIR/$tfile $DIR/${tfile}link
}
run_test 102i "lgetxattr test on symbolic link ============"

test_102j() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	XINC=$(have_xattrs_include)
	setup_test102 "$RUNAS"
	chown $RUNAS_ID $DIR/$tdir
	$RUNAS tar xf $TMP/f102.tar -C $DIR/$tdir --xattrs $XINC
	cd $DIR/$tdir/$tdir
	compare_stripe_info1 "$RUNAS"
}
run_test 102j "non-root tar restore stripe info from tarfile, not keep osts ==="

test_102k() {
	[ -z "$(which setfattr 2>/dev/null)" ] &&
		skip "could not find setfattr"

	touch $DIR/$tfile
	# b22187 just check that does not crash for regular file.
	setfattr -n trusted.lov $DIR/$tfile
	# b22187 'setfattr -n trusted.lov' should remove LOV EA for directories
	local test_kdir=$DIR/$tdir
	test_mkdir $test_kdir
	local default_size=$($LFS getstripe -S $test_kdir)
	local default_count=$($LFS getstripe -c $test_kdir)
	local default_offset=$($LFS getstripe -i $test_kdir)
	$SETSTRIPE -S 65536 -i 0 -c $OSTCOUNT $test_kdir ||
		error 'dir setstripe failed'
	setfattr -n trusted.lov $test_kdir
	local stripe_size=$($LFS getstripe -S $test_kdir)
	local stripe_count=$($LFS getstripe -c $test_kdir)
	local stripe_offset=$($LFS getstripe -i $test_kdir)
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
	[ -z "$(which getfattr 2>/dev/null)" ] &&
		skip "could not find getfattr"

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
run_test 102l "listxattr size test =================================="

test_102m() { # LU-3403 llite: error of listxattr when buffer is small
	local path=$DIR/$tfile
	touch $path

	listxattr_size_check $path || error "listattr_size_check $path failed"
}
run_test 102m "Ensure listxattr fails on small bufffer ========"

cleanup_test102

getxattr() { # getxattr path name
	# Return the base64 encoding of the value of xattr name on path.
	local path=$1
	local name=$2

	# # getfattr --absolute-names --encoding=base64 --name=trusted.lov $path
	# file: $path
	# trusted.lov=0s0AvRCwEAAAAGAAAAAAAAAAAEAAACAAAAAAAQAAEAA...AAAAAAAAA=
	#
	# We print just 0s0AvRCwEAAAAGAAAAAAAAAAAEAAACAAAAAAAQAAEAA...AAAAAAAAA=

	getfattr --absolute-names --encoding=base64 --name=$name $path |
		awk -F= -v name=$name '$1 == name {
			print substr($0, index($0, "=") + 1);
	}'
}

test_102n() { # LU-4101 mdt: protect internal xattrs
	[ -z "$(which setfattr 2>/dev/null)" ] &&
		skip "could not find setfattr"
	if [ $MDS1_VERSION -lt $(version_code 2.5.50) ]
	then
		skip "MDT < 2.5.50 allows setxattr on internal trusted xattrs"
	fi

	local file0=$DIR/$tfile.0
	local file1=$DIR/$tfile.1
	local xattr0=$TMP/$tfile.0
	local xattr1=$TMP/$tfile.1
	local namelist="lov lma lmv link fid version som hsm"
	local name
	local value

	rm -rf $file0 $file1 $xattr0 $xattr1
	touch $file0 $file1

	# Get 'before' xattrs of $file1.
	getfattr --absolute-names --dump --match=- $file1 > $xattr0

	[ $MDS1_VERSION -lt $(version_code 2.8.53) ] &&
		namelist+=" lfsck_namespace"
	for name in $namelist; do
		# Try to copy xattr from $file0 to $file1.
		value=$(getxattr $file0 trusted.$name 2> /dev/null)

		setfattr --name=trusted.$name --value="$value" $file1 ||
			error "setxattr 'trusted.$name' failed"

		# Try to set a garbage xattr.
		value=0sVGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIGl0c2VsZi4=

		if [[ x$name == "xlov" ]]; then
			setfattr --name=trusted.lov --value="$value" $file1 &&
			error "setxattr invalid 'trusted.lov' success"
		else
			setfattr --name=trusted.$name --value="$value" $file1 ||
				error "setxattr invalid 'trusted.$name' failed"
		fi

		# Try to remove the xattr from $file1. We don't care if this
		# appears to succeed or fail, we just don't want there to be
		# any changes or crashes.
		setfattr --remove=$trusted.$name $file1 2> /dev/null
	done

	if [ $MDS1_VERSION -gt $(version_code 2.6.50) ]
	then
		name="lfsck_ns"
		# Try to copy xattr from $file0 to $file1.
		value=$(getxattr $file0 trusted.$name 2> /dev/null)

		setfattr --name=trusted.$name --value="$value" $file1 ||
			error "setxattr 'trusted.$name' failed"

		# Try to set a garbage xattr.
		value=0sVGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIGl0c2VsZi4=

		setfattr --name=trusted.$name --value="$value" $file1 ||
			error "setxattr 'trusted.$name' failed"

		# Try to remove the xattr from $file1. We don't care if this
		# appears to succeed or fail, we just don't want there to be
		# any changes or crashes.
		setfattr --remove=$trusted.$name $file1 2> /dev/null
	fi

	# Get 'after' xattrs of file1.
	getfattr --absolute-names --dump --match=- $file1 > $xattr1

	if ! diff $xattr0 $xattr1; then
		error "before and after xattrs of '$file1' differ"
	fi

	rm -rf $file0 $file1 $xattr0 $xattr1

	return 0
}
run_test 102n "silently ignore setxattr on internal trusted xattrs"

test_102p() { # LU-4703 setxattr did not check ownership
	[ $MDS1_VERSION -lt $(version_code 2.5.56) ] &&
		skip "MDS needs to be at least 2.5.56"

	local testfile=$DIR/$tfile

	touch $testfile

	echo "setfacl as user..."
	$RUNAS setfacl -m "u:$RUNAS_ID:rwx" $testfile
	[ $? -ne 0 ] || error "setfacl by $RUNAS_ID was allowed on $testfile"

	echo "setfattr as user..."
	setfacl -m "u:$RUNAS_ID:---" $testfile
	$RUNAS setfattr -x system.posix_acl_access $testfile
	[ $? -ne 0 ] || error "setfattr by $RUNAS_ID was allowed on $testfile"
}
run_test 102p "check setxattr(2) correctly fails without permission"

test_102q() {
	[ $MDS1_VERSION -lt $(version_code 2.6.92) ] &&
		skip "MDS needs to be at least 2.6.92"

	orphan_linkea_check $DIR/$tfile || error "orphan_linkea_check"
}
run_test 102q "flistxattr should not return trusted.link EAs for orphans"

test_102r() {
	[ $MDS1_VERSION -lt $(version_code 2.6.93) ] &&
		skip "MDS needs to be at least 2.6.93"

	touch $DIR/$tfile || error "touch"
	setfattr -n user.$(basename $tfile) $DIR/$tfile || error "setfattr"
	getfattr -n user.$(basename $tfile) $DIR/$tfile || error "getfattr"
	rm $DIR/$tfile || error "rm"

	#normal directory
	mkdir -p $DIR/$tdir || error "mkdir"
	setfattr -n user.$(basename $tdir) $DIR/$tdir || error "setfattr dir"
	getfattr -n user.$(basename $tdir) $DIR/$tdir || error "getfattr dir"
	setfattr -x user.$(basename $tdir) $DIR/$tdir ||
		error "$testfile error deleting user.author1"
	getfattr -d -m user.$(basename $tdir) 2> /dev/null |
		grep "user.$(basename $tdir)" &&
		error "$tdir did not delete user.$(basename $tdir)"
	rmdir $DIR/$tdir || error "rmdir"

	#striped directory
	test_mkdir $DIR/$tdir
	setfattr -n user.$(basename $tdir) $DIR/$tdir || error "setfattr dir"
	getfattr -n user.$(basename $tdir) $DIR/$tdir || error "getfattr dir"
	setfattr -x user.$(basename $tdir) $DIR/$tdir ||
		error "$testfile error deleting user.author1"
	getfattr -d -m user.$(basename $tdir) 2> /dev/null |
		grep "user.$(basename $tdir)" &&
		error "$tdir did not delete user.$(basename $tdir)"
	rmdir $DIR/$tdir || error "rm striped dir"
}
run_test 102r "set EAs with empty values"

test_102s() {
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "MDS needs to be at least 2.11.52"

	local save="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_lustre_params client "llite.*.xattr_cache" > $save

	for cache in 0 1; do
		lctl set_param llite.*.xattr_cache=$cache

		rm -f $DIR/$tfile
		touch $DIR/$tfile || error "touch"
		for prefix in lustre security system trusted user; do
			# Note getxattr() may fail with 'Operation not
			# supported' or 'No such attribute' depending
			# on prefix and cache.
			getfattr -n $prefix.n102s $DIR/$tfile &&
				error "getxattr '$prefix.n102s' should fail (cache = $cache)"
		done
	done

	restore_lustre_params < $save
}
run_test 102s "getting nonexistent xattrs should fail"

test_102t() {
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "MDS needs to be at least 2.11.52"

	local save="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_lustre_params client "llite.*.xattr_cache" > $save

	for cache in 0 1; do
		lctl set_param llite.*.xattr_cache=$cache

		for buf_size in 0 256; do
			rm -f $DIR/$tfile
			touch $DIR/$tfile || error "touch"
			setfattr -n user.multiop $DIR/$tfile
			$MULTIOP $DIR/$tfile oa$buf_size ||
				error "cannot get zero length xattr value (buf_size = $buf_size)"
		done
	done

	restore_lustre_params < $save
}
run_test 102t "zero length xattr values handled correctly"

run_acl_subtest()
{
    $LUSTRE/tests/acl/run $LUSTRE/tests/acl/$1.test
    return $?
}

test_103a() {
	[ "$UID" != 0 ] && skip "must run as root"
	$GSS && skip_env "could not run under gss"
	[ -z "$(lctl get_param -n mdc.*-mdc-*.connect_flags | grep acl)" ] &&
		skip_env "must have acl enabled"
	[ -z "$(which setfacl 2>/dev/null)" ] &&
		skip_env "could not find setfacl"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	gpasswd -a daemon bin				# LU-5641
	do_facet $SINGLEMDS gpasswd -a daemon bin	# LU-5641

	declare -a identity_old

	for num in $(seq $MDSCOUNT); do
		switch_identity $num true || identity_old[$num]=$?
	done

	SAVE_UMASK=$(umask)
	umask 0022
	mkdir -p $DIR/$tdir
	cd $DIR/$tdir

	echo "performing cp ..."
	run_acl_subtest cp || error "run_acl_subtest cp failed"
	echo "performing getfacl-noacl..."
	run_acl_subtest getfacl-noacl || error "getfacl-noacl test failed"
	echo "performing misc..."
	run_acl_subtest misc || error  "misc test failed"
	echo "performing permissions..."
	run_acl_subtest permissions || error "permissions failed"
	# LU-1482 mdd: Setting xattr are properly checked with and without ACLs
	if [ $MDS1_VERSION -gt $(version_code 2.8.55) ] ||
		{ [ $MDS1_VERSION -lt $(version_code 2.6) ] &&
			[ $MDS1_VERSION -ge $(version_code 2.5.29) ]; }
	then
		echo "performing permissions xattr..."
		run_acl_subtest permissions_xattr ||
			error "permissions_xattr failed"
	fi
	echo "performing setfacl..."
	run_acl_subtest setfacl || error  "setfacl test failed"

	# inheritance test got from HP
	echo "performing inheritance..."
	cp $LUSTRE/tests/acl/make-tree . || error "cannot copy make-tree"
	chmod +x make-tree || error "chmod +x failed"
	run_acl_subtest inheritance || error "inheritance test failed"
	rm -f make-tree

	echo "LU-974 ignore umask when acl is enabled..."
	run_acl_subtest 974 || error "LU-974 umask test failed"
	if [ $MDSCOUNT -ge 2 ]; then
		run_acl_subtest 974_remote ||
			error "LU-974 umask test failed under remote dir"
	fi

	echo "LU-2561 newly created file is same size as directory..."
	if [ "$mds1_FSTYPE" != "zfs" ]; then
		run_acl_subtest 2561 || error "LU-2561 test failed"
	else
		run_acl_subtest 2561_zfs || error "LU-2561 zfs test failed"
	fi

	run_acl_subtest 4924 || error "LU-4924 test failed"

	cd $SAVE_PWD
	umask $SAVE_UMASK

	for num in $(seq $MDSCOUNT); do
		if [ "${identity_old[$num]}" = 1 ]; then
			switch_identity $num false || identity_old[$num]=$?
		fi
	done
}
run_test 103a "acl test"

test_103b() {
	declare -a pids
	local U

	for U in {0..511}; do
		{
		local O=$(printf "%04o" $U)

		umask $(printf "%04o" $((511 ^ $O)))
		$LFS setstripe -c 1 $DIR/$tfile.s$O
		local S=$(printf "%04o" 0$(stat -c%a $DIR/$tfile.s$O))

		(( $S == ($O & 0666) )) ||
			error "lfs setstripe $DIR/$tfile.s$O '$S' != '$O'"

		$LFS setstripe -E16M -c 1 -E1G -S4M $DIR/$tfile.p$O
		S=$(printf "%04o" 0$(stat -c%a $DIR/$tfile.p$O))
		(( $S == ($O & 0666) )) ||
			error "lfs setstripe -E $DIR/$tfile.p$O '$S' != '$O'"

		$LFS setstripe -N2 -c 1 $DIR/$tfile.m$O
		S=$(printf "%04o" 0$(stat -c%a $DIR/$tfile.m$O))
		(( $S == ($O & 0666) )) ||
			error "lfs setstripe -N2 $DIR/$tfile.m$O '$S' != '$O'"
		rm -f $DIR/$tfile.[smp]$0
		} &
		local pid=$!

		# limit the concurrently running threads to 64. LU-11878
		local idx=$((U % 64))
		[ -z "${pids[idx]}" ] || wait ${pids[idx]}
		pids[idx]=$pid
	done
	wait
}
run_test 103b "umask lfs setstripe"

test_103c() {
	mkdir -p $DIR/$tdir
	cp -rp $DIR/$tdir $DIR/$tdir.bak

	[ -n "$(getfattr -d -m. $DIR/$tdir | grep posix_acl_default)" ] &&
		error "$DIR/$tdir shouldn't contain default ACL"
	[ -n "$(getfattr -d -m. $DIR/$tdir.bak | grep posix_acl_default)" ] &&
		error "$DIR/$tdir.bak shouldn't contain default ACL"
	true
}
run_test 103c "'cp -rp' won't set empty acl"

test_104a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	touch $DIR/$tfile
	lfs df || error "lfs df failed"
	lfs df -ih || error "lfs df -ih failed"
	lfs df -h $DIR || error "lfs df -h $DIR failed"
	lfs df -i $DIR || error "lfs df -i $DIR failed"
	lfs df $DIR/$tfile || error "lfs df $DIR/$tfile failed"
	lfs df -ih $DIR/$tfile || error "lfs df -ih $DIR/$tfile failed"

	local OSC=$(lctl dl | grep OST0000-osc-[^M] | awk '{ print $4 }')
	lctl --device %$OSC deactivate
	lfs df || error "lfs df with deactivated OSC failed"
	lctl --device %$OSC activate
	# wait the osc back to normal
	wait_osc_import_ready client ost

	lfs df || error "lfs df with reactivated OSC failed"
	rm -f $DIR/$tfile
}
run_test 104a "lfs df [-ih] [path] test ========================="

test_104b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $RUNAS_ID -eq $UID ] &&
		skip_env "RUNAS_ID = UID = $UID -- skipping"

	denied_cnt=$(($($RUNAS $LFS check servers 2>&1 |
			grep "Permission denied" | wc -l)))
	if [ $denied_cnt -ne 0 ]; then
		error "lfs check servers test failed"
	fi
}
run_test 104b "$RUNAS lfs check servers test ===================="

test_105a() {
	# doesn't work on 2.4 kernels
	touch $DIR/$tfile
	if $(flock_is_enabled); then
		flocks_test 1 on -f $DIR/$tfile || error "fail flock on"
	else
		flocks_test 1 off -f $DIR/$tfile || error "fail flock off"
	fi
	rm -f $DIR/$tfile
}
run_test 105a "flock when mounted without -o flock test ========"

test_105b() {
	touch $DIR/$tfile
	if $(flock_is_enabled); then
		flocks_test 1 on -c $DIR/$tfile || error "fail flock on"
	else
		flocks_test 1 off -c $DIR/$tfile || error "fail flock off"
	fi
	rm -f $DIR/$tfile
}
run_test 105b "fcntl when mounted without -o flock test ========"

test_105c() {
	touch $DIR/$tfile
	if $(flock_is_enabled); then
		flocks_test 1 on -l $DIR/$tfile || error "fail flock on"
	else
		flocks_test 1 off -l $DIR/$tfile || error "fail flock off"
	fi
	rm -f $DIR/$tfile
}
run_test 105c "lockf when mounted without -o flock test"

test_105d() { # bug 15924
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	flock_is_enabled || skip_env "mount w/o flock enabled"
	#define OBD_FAIL_LDLM_CP_CB_WAIT  0x315
	$LCTL set_param fail_loc=0x80000315
	flocks_test 2 $DIR/$tdir
}
run_test 105d "flock race (should not freeze) ========"

test_105e() { # bug 22660 && 22040
	flock_is_enabled || skip_env "mount w/o flock enabled"

	touch $DIR/$tfile
	flocks_test 3 $DIR/$tfile
}
run_test 105e "Two conflicting flocks from same process"

test_106() { #bug 10921
	test_mkdir $DIR/$tdir
	$DIR/$tdir && error "exec $DIR/$tdir succeeded"
	chmod 777 $DIR/$tdir || error "chmod $DIR/$tdir failed"
}
run_test 106 "attempt exec of dir followed by chown of that dir"

test_107() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	CDIR=`pwd`
	local file=core

	cd $DIR
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
	test_mkdir $DIR/$tdir
	test_mkdir $DIR/$tdir/$(str_repeat 'a' 255)
	$LFS mkdir -c $MDSCOUNT $DIR/$tdir/$(str_repeat 'b' 256) &&
		error "mkdir with 256 char should fail, but did not"
	touch $DIR/$tdir/$(str_repeat 'x' 255) ||
		error "create with 255 char failed"
	touch $DIR/$tdir/$(str_repeat 'y' 256) &&
		error "create with 256 char should fail, but did not"

	ls -l $DIR/$tdir
	rm -rf $DIR/$tdir
}
run_test 110 "filename length checking"

#
# Purpose: To verify dynamic thread (OSS) creation.
#
test_115() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

	# Lustre does not stop service threads once they are started.
	# Reset number of running threads to default.
	stopall
	setupall

	local OSTIO_pre
	local save_params="$TMP/sanity-$TESTNAME.parameters"

	# Get ll_ost_io count before I/O
	OSTIO_pre=$(do_facet ost1 \
		"$LCTL get_param ost.OSS.ost_io.threads_started | cut -d= -f2")
	# Exit if lustre is not running (ll_ost_io not running).
	[ -z "$OSTIO_pre" ] && error "no OSS threads"

	echo "Starting with $OSTIO_pre threads"
	local thread_max=$((OSTIO_pre * 2))
	local rpc_in_flight=$((thread_max * 2))
	# Number of I/O Process proposed to be started.
	local nfiles
	local facets=$(get_facets OST)

	save_lustre_params client "osc.*OST*.max_rpcs_in_flight" > $save_params
	save_lustre_params $facets "ost.OSS.ost_io.threads_max" >> $save_params

	# Set in_flight to $rpc_in_flight
	$LCTL set_param osc.*OST*.max_rpcs_in_flight=$rpc_in_flight ||
		error "Failed to set max_rpcs_in_flight to $rpc_in_flight"
	nfiles=${rpc_in_flight}
	# Set ost thread_max to $thread_max
	do_facet ost1 "$LCTL set_param ost.OSS.ost_io.threads_max=$thread_max"

	# 5 Minutes should be sufficient for max number of OSS
	# threads(thread_max) to be created.
	local timeout=300

	# Start I/O.
	local WTL=${WTL:-"$LUSTRE/tests/write_time_limit"}
	test_mkdir $DIR/$tdir
	for i in $(seq $nfiles); do
		local file=$DIR/$tdir/${tfile}-$i
		$LFS setstripe -c -1 -i 0 $file
		($WTL $file $timeout)&
	done

	# I/O Started - Wait for thread_started to reach thread_max or report
	# error if thread_started is more than thread_max.
	echo "Waiting for thread_started to reach thread_max"
	local thread_started=0
	local end_time=$((SECONDS + timeout))

	while [ $SECONDS -le $end_time ] ; do
		echo -n "."
		# Get ost i/o thread_started count.
		thread_started=$(do_facet ost1 \
			"$LCTL get_param \
			ost.OSS.ost_io.threads_started | cut -d= -f2")
		# Break out if thread_started is equal/greater than thread_max
		if [[ $thread_started -ge $thread_max ]]; then
			echo ll_ost_io thread_started $thread_started, \
				equal/greater than thread_max $thread_max
			break
		fi
		sleep 1
	done

	# Cleanup - We have the numbers, Kill i/o jobs if running.
	jobcount=($(jobs -p))
	for i in $(seq 0 $((${#jobcount[@]}-1)))
	do
		kill -9 ${jobcount[$i]}
		if [ $? -ne 0 ] ; then
			echo Warning: \
			Failed to Kill \'WTL\(I/O\)\' with pid ${jobcount[$i]}
		fi
	done

	# Cleanup files left by WTL binary.
	for i in $(seq $nfiles); do
		local file=$DIR/$tdir/${tfile}-$i
		rm -rf $file
		if [ $? -ne 0 ] ; then
			echo "Warning: Failed to delete file $file"
		fi
	done

	restore_lustre_params <$save_params
	rm -f $save_params || echo "Warning: delete file '$save_params' failed"

	# Error out if no new thread has started or Thread started is greater
	# than thread max.
	if [[ $thread_started -le $OSTIO_pre ||
			$thread_started -gt $thread_max ]]; then
		error "ll_ost_io: thread_started $thread_started" \
		      "OSTIO_pre $OSTIO_pre, thread_max $thread_max." \
		      "No new thread started or thread started greater " \
		      "than thread_max."
	fi
}
run_test 115 "verify dynamic thread creation===================="

free_min_max () {
	wait_delete_completed
	AVAIL=($(lctl get_param -n osc.*[oO][sS][cC]-[^M]*.kbytesavail))
	echo "OST kbytes available: ${AVAIL[@]}"
	MAXV=${AVAIL[0]}
	MAXI=0
	MINV=${AVAIL[0]}
	MINI=0
	for ((i = 0; i < ${#AVAIL[@]}; i++)); do
		#echo OST $i: ${AVAIL[i]}kb
		if [[ ${AVAIL[i]} -gt $MAXV ]]; then
			MAXV=${AVAIL[i]}
			MAXI=$i
		fi
		if [[ ${AVAIL[i]} -lt $MINV ]]; then
			MINV=${AVAIL[i]}
			MINI=$i
		fi
	done
	echo "Min free space: OST $MINI: $MINV"
	echo "Max free space: OST $MAXI: $MAXV"
}

test_116a() { # was previously test_116()
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	echo -n "Free space priority "
	do_facet $SINGLEMDS lctl get_param -n lo[vd].*-mdtlov.qos_prio_free |
		head -n1
	declare -a AVAIL
	free_min_max

	[ $MINV -eq 0 ] && skip "no free space in OST$MINI, skip"
	[ $MINV -gt 10000000 ] && skip "too much free space in OST$MINI, skip"
	trap simple_cleanup_common EXIT

	# Check if we need to generate uneven OSTs
	test_mkdir -p $DIR/$tdir/OST${MINI}
	local FILL=$((MINV / 4))
	local DIFF=$((MAXV - MINV))
	local DIFF2=$((DIFF * 100 / MINV))

	local threshold=$(do_facet $SINGLEMDS \
		lctl get_param -n *.*MDT0000-mdtlov.qos_threshold_rr | head -n1)
	threshold=${threshold%%%}
	echo -n "Check for uneven OSTs: "
	echo -n "diff=${DIFF}KB (${DIFF2}%) must be > ${threshold}% ..."

	if [[ $DIFF2 -gt $threshold ]]; then
		echo "ok"
		echo "Don't need to fill OST$MINI"
	else
		# generate uneven OSTs. Write 2% over the QOS threshold value
		echo "no"
		DIFF=$((threshold - DIFF2 + 2))
		DIFF2=$((MINV * DIFF / 100))
		echo "Fill $DIFF% remaining space in OST$MINI with ${DIFF2}KB"
		$SETSTRIPE -i $MINI -c 1 $DIR/$tdir/OST${MINI} ||
			error "setstripe failed"
		DIFF=$((DIFF2 / 2048))
		i=0
		while [ $i -lt $DIFF ]; do
			i=$((i + 1))
			dd if=/dev/zero of=$DIR/$tdir/OST${MINI}/$tfile-$i \
				bs=2M count=1 2>/dev/null
			echo -n .
		done
		echo .
		sync
		sleep_maxage
		free_min_max
	fi

	DIFF=$((MAXV - MINV))
	DIFF2=$((DIFF * 100 / MINV))
	echo -n "diff=$DIFF=$DIFF2% must be > $threshold% for QOS mode..."
	if [ $DIFF2 -gt $threshold ]; then
		echo "ok"
	else
		echo "failed - QOS mode won't be used"
		simple_cleanup_common
		skip "QOS imbalance criteria not met"
	fi

	MINI1=$MINI
	MINV1=$MINV
	MAXI1=$MAXI
	MAXV1=$MAXV

	# now fill using QOS
	$SETSTRIPE -c 1 $DIR/$tdir
	FILL=$((FILL / 200))
	if [ $FILL -gt 600 ]; then
		FILL=600
	fi
	echo "writing $FILL files to QOS-assigned OSTs"
	i=0
	while [ $i -lt $FILL ]; do
		i=$((i + 1))
		dd if=/dev/zero of=$DIR/$tdir/$tfile-$i bs=200k \
			count=1 2>/dev/null
		echo -n .
	done
	echo "wrote $i 200k files"
	sync
	sleep_maxage

	echo "Note: free space may not be updated, so measurements might be off"
	free_min_max
	DIFF2=$((MAXV - MINV))
	echo "free space delta: orig $DIFF final $DIFF2"
	[ $DIFF2 -gt $DIFF ] && echo "delta got worse!"
	DIFF=$((MINV1 - ${AVAIL[$MINI1]}))
	echo "Wrote ${DIFF}KB to smaller OST $MINI1"
	DIFF2=$((MAXV1 - ${AVAIL[$MAXI1]}))
	echo "Wrote ${DIFF2}KB to larger OST $MAXI1"
	if [[ $DIFF -gt 0 ]]; then
		FILL=$((DIFF2 * 100 / DIFF - 100))
		echo "Wrote ${FILL}% more data to larger OST $MAXI1"
	fi

	# Figure out which files were written where
	UUID=$(lctl get_param -n lov.${FSNAME}-clilov-*.target_obd |
	       awk '/'$MINI1': / {print $2; exit}')
	echo $UUID
	MINC=$($GETSTRIPE --ost $UUID $DIR/$tdir | grep $DIR | wc -l)
	echo "$MINC files created on smaller OST $MINI1"
	UUID=$(lctl get_param -n lov.${FSNAME}-clilov-*.target_obd |
	       awk '/'$MAXI1': / {print $2; exit}')
	echo $UUID
	MAXC=$($GETSTRIPE --ost $UUID $DIR/$tdir | grep $DIR | wc -l)
	echo "$MAXC files created on larger OST $MAXI1"
	if [[ $MINC -gt 0 ]]; then
		FILL=$((MAXC * 100 / MINC - 100))
		echo "Wrote ${FILL}% more files to larger OST $MAXI1"
	fi
	[[ $MAXC -gt $MINC ]] ||
		error_ignore LU-9 "stripe QOS didn't balance free space"
	simple_cleanup_common
}
run_test 116a "stripe QOS: free space balance ==================="

test_116b() { # LU-2093
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"

#define OBD_FAIL_MDS_OSC_CREATE_FAIL     0x147
	local old_rr=$(do_facet $SINGLEMDS lctl get_param -n \
		       lo[vd].$FSNAME-MDT0000-mdtlov.qos_threshold_rr | head -1)
	[ -z "$old_rr" ] && skip "no QOS"
	do_facet $SINGLEMDS lctl set_param \
		lo[vd].$FSNAME-MDT0000-mdtlov.qos_threshold_rr=0
	mkdir -p $DIR/$tdir
	do_facet $SINGLEMDS lctl set_param fail_loc=0x147
	createmany -o $DIR/$tdir/f- 20 || error "can't create"
	do_facet $SINGLEMDS lctl set_param fail_loc=0
	rm -rf $DIR/$tdir
	do_facet $SINGLEMDS lctl set_param \
		lo[vd].$FSNAME-MDT0000-mdtlov.qos_threshold_rr=$old_rr
}
run_test 116b "QoS shouldn't LBUG if not enough OSTs found on the 2nd pass"

test_117() # bug 10891
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	dd if=/dev/zero of=$DIR/$tfile bs=1M count=1
	#define OBD_FAIL_OST_SETATTR_CREDITS 0x21e
	lctl set_param fail_loc=0x21e
	> $DIR/$tfile || error "truncate failed"
	lctl set_param fail_loc=0
	echo "Truncate succeeded."
	rm -f $DIR/$tfile
}
run_test 117 "verify osd extend =========="

NO_SLOW_RESENDCOUNT=4
export OLD_RESENDCOUNT=""
set_resend_count () {
	local PROC_RESENDCOUNT="osc.${FSNAME}-OST*-osc-*.resend_count"
	OLD_RESENDCOUNT=$(lctl get_param -n $PROC_RESENDCOUNT | head -n1)
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

	reset_async

	#define OBD_FAIL_SRV_ENOENT 0x217
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	# for 118c, restore the original resend count, LU-1940
	[ "$SLOW" = "no" ] && [ -n "$OLD_RESENDCOUNT" ] &&
				set_resend_count $OLD_RESENDCOUNT
	remote_ost_nodsh && skip "remote OST with nodsh"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OSTs with nodsh"

	#define OBD_FAIL_OST_BRW_WRITE_BULK      0x20e
	set_nodes_failloc "$(osts_nodes)" 0x20e
	test_mkdir $DIR/$tdir

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

test_118l() # LU-646
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	$MULTIOP $DIR/$tdir Dy || error "fsync dir failed"
	rm -rf $DIR/$tdir
}
run_test 118l "fsync dir"

test_118m() # LU-3066
{
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	$MULTIOP $DIR/$tdir DY || error "fdatasync dir failed"
	rm -rf $DIR/$tdir
}
run_test 118m "fdatasync dir ========="

[ "$SLOW" = "no" ] && [ -n "$OLD_RESENDCOUNT" ] && set_resend_count $OLD_RESENDCOUNT

test_118n()
{
	local begin
	local end

	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OSTs with nodsh"

	# Sleep to avoid a cached response.
	#define OBD_STATFS_CACHE_SECONDS 1
	sleep 2

	# Inject a 10 second delay in the OST_STATFS handler.
	#define OBD_FAIL_OST_STATFS_DELAY 0x242
	set_nodes_failloc "$(osts_nodes)" 0x242

	begin=$SECONDS
	stat --file-system $MOUNT > /dev/null
	end=$SECONDS

	set_nodes_failloc "$(osts_nodes)" 0

	if ((end - begin > 20)); then
	    error "statfs took $((end - begin)) seconds, expected 10"
	fi
}
run_test 118n "statfs() sends OST_STATFS requests in parallel"

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
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	$SETSTRIPE -c 2 $DIR/$tfile || error "setstripe failed"
	dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 seek=1 || error "dd failed"
	sync
	$MULTIOP $DIR/$tfile oO_RDONLY:O_DIRECT:r$((2048 * 1024)) ||
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	test_mkdir $DIR/$tdir
	$LCTL get_param -n mdc.*.connect_flags | grep -q early_lock_cancel ||
		skip_env "no early lock cancel on server"

	lru_resize_disable mdc
	lru_resize_disable osc
	cancel_lru_locks mdc
	# asynchronous object destroy at MDT could cause bl ast to client
	cancel_lru_locks osc

	stat $DIR/$tdir > /dev/null
	can1=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk1=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	test_mkdir -c1 $DIR/$tdir/d1
	can2=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk2=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	[ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
	[ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
	lru_resize_enable mdc
	lru_resize_enable osc
}
run_test 120a "Early Lock Cancel: mkdir test"

test_120b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	test_mkdir $DIR/$tdir
	$LCTL get_param -n mdc.*.connect_flags | grep -q early_lock_cancel ||
		skip_env "no early lock cancel on server"

	lru_resize_disable mdc
	lru_resize_disable osc
	cancel_lru_locks mdc
	stat $DIR/$tdir > /dev/null
	can1=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk1=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	touch $DIR/$tdir/f1
	can2=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk2=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	[ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
	[ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
	lru_resize_enable mdc
	lru_resize_enable osc
}
run_test 120b "Early Lock Cancel: create test"

test_120c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	test_mkdir -c1 $DIR/$tdir
	$LCTL get_param -n mdc.*.connect_flags | grep -q early_lock_cancel ||
		skip "no early lock cancel on server"

	lru_resize_disable mdc
	lru_resize_disable osc
	test_mkdir -c1 $DIR/$tdir/d1
	test_mkdir -c1 $DIR/$tdir/d2
	touch $DIR/$tdir/d1/f1
	cancel_lru_locks mdc
	stat $DIR/$tdir/d1 $DIR/$tdir/d2 $DIR/$tdir/d1/f1 > /dev/null
	can1=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk1=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	ln $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2
	can2=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk2=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	[ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
	[ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
	lru_resize_enable mdc
	lru_resize_enable osc
}
run_test 120c "Early Lock Cancel: link test"

test_120d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	test_mkdir -c1 $DIR/$tdir
	$LCTL get_param -n mdc.*.connect_flags | grep -q early_lock_cancel ||
		skip_env "no early lock cancel on server"

	lru_resize_disable mdc
	lru_resize_disable osc
	touch $DIR/$tdir
	cancel_lru_locks mdc
	stat $DIR/$tdir > /dev/null
	can1=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk1=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	chmod a+x $DIR/$tdir
	can2=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk2=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	[ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
	[ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
	lru_resize_enable mdc
	lru_resize_enable osc
}
run_test 120d "Early Lock Cancel: setattr test"

test_120e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$LCTL get_param -n mdc.*.connect_flags | grep -q early_lock_cancel ||
		skip_env "no early lock cancel on server"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local dlmtrace_set=false

	test_mkdir -c1 $DIR/$tdir
	lru_resize_disable mdc
	lru_resize_disable osc
	! $LCTL get_param debug | grep -q dlmtrace &&
		$LCTL set_param debug=+dlmtrace && dlmtrace_set=true
	dd if=/dev/zero of=$DIR/$tdir/f1 count=1
	cancel_lru_locks mdc
	cancel_lru_locks osc
	dd if=$DIR/$tdir/f1 of=/dev/null
	stat $DIR/$tdir $DIR/$tdir/f1 > /dev/null
	# XXX client can not do early lock cancel of OST lock
	# during unlink (LU-4206), so cancel osc lock now.
	sleep 2
	cancel_lru_locks osc
	can1=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk1=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	unlink $DIR/$tdir/f1
	sleep 5
	can2=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk2=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	[ $can1 -ne $can2 ] && error "$((can2 - can1)) cancel RPC occured" &&
		$LCTL dk $TMP/cancel.debug.txt
	[ $blk1 -ne $blk2 ] && error "$((blk2 - blk1)) blocking RPC occured" &&
		$LCTL dk $TMP/blocking.debug.txt
	$dlmtrace_set && $LCTL set_param debug=-dlmtrace
	lru_resize_enable mdc
	lru_resize_enable osc
}
run_test 120e "Early Lock Cancel: unlink test"

test_120f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$LCTL get_param -n mdc.*.connect_flags | grep -q early_lock_cancel ||
		skip_env "no early lock cancel on server"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	test_mkdir -c1 $DIR/$tdir
	lru_resize_disable mdc
	lru_resize_disable osc
	test_mkdir -c1 $DIR/$tdir/d1
	test_mkdir -c1 $DIR/$tdir/d2
	dd if=/dev/zero of=$DIR/$tdir/d1/f1 count=1
	dd if=/dev/zero of=$DIR/$tdir/d2/f2 count=1
	cancel_lru_locks mdc
	cancel_lru_locks osc
	dd if=$DIR/$tdir/d1/f1 of=/dev/null
	dd if=$DIR/$tdir/d2/f2 of=/dev/null
	stat $DIR/$tdir/d1 $DIR/$tdir/d2 $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2 > /dev/null
	# XXX client can not do early lock cancel of OST lock
	# during rename (LU-4206), so cancel osc lock now.
	sleep 2
	cancel_lru_locks osc
	can1=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk1=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	mrename $DIR/$tdir/d1/f1 $DIR/$tdir/d2/f2
	sleep 5
	can2=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk2=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	[ $can1 -eq $can2 ] || error $((can2-can1)) "cancel RPC occured."
	[ $blk1 -eq $blk2 ] || error $((blk2-blk1)) "blocking RPC occured."
	lru_resize_enable mdc
	lru_resize_enable osc
}
run_test 120f "Early Lock Cancel: rename test"

test_120g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$LCTL get_param -n mdc.*.connect_flags | grep -q early_lock_cancel ||
		skip_env "no early lock cancel on server"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	lru_resize_disable mdc
	lru_resize_disable osc
	count=10000
	echo create $count files
	test_mkdir $DIR/$tdir
	cancel_lru_locks mdc
	cancel_lru_locks osc
	t0=$(date +%s)

	can0=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk0=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	createmany -o $DIR/$tdir/f $count
	sync
	can1=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk1=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	t1=$(date +%s)
	echo total: $((can1-can0)) cancels, $((blk1-blk0)) blockings
	echo rm $count files
	rm -r $DIR/$tdir
	sync
	can2=$(do_facet $SINGLEMDS \
	       "$LCTL get_param -n ldlm.services.ldlm_canceld.stats" |
	       awk '/ldlm_cancel/ {print $2}')
	blk2=$($LCTL get_param -n ldlm.services.ldlm_cbd.stats |
	       awk '/ldlm_bl_callback/ {print $2}')
	t2=$(date +%s)
	echo total: $count removes in $((t2-t1))
	echo total: $((can2-can1)) cancels, $((blk2-blk1)) blockings
	sleep 2
	# wait for commitment of removal
	lru_resize_enable mdc
	lru_resize_enable osc
}
run_test 120g "Early Lock Cancel: performance test"

test_121() { #bug #10589
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	rm -rf $DIR/$tfile
	writes=$(LANG=C dd if=/dev/zero of=$DIR/$tfile count=1 2>&1 | awk -F '+' '/out$/ {print $1}')
#define OBD_FAIL_LDLM_CANCEL_RACE        0x310
	lctl set_param fail_loc=0x310
	cancel_lru_locks osc > /dev/null
	reads=$(LANG=C dd if=$DIR/$tfile of=/dev/null 2>&1 | awk -F '+' '/in$/ {print $1}')
	lctl set_param fail_loc=0
	[[ $reads -eq $writes ]] ||
		error "read $reads blocks, must be $writes blocks"
}
run_test 121 "read cancel race ========="

test_123a() { # was test 123, statahead(bug 11401)
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	SLOWOK=0
	if ! grep -q "processor.*: 1" /proc/cpuinfo; then
		log "testing UP system. Performance may be lower than expected."
		SLOWOK=1
	fi

	rm -rf $DIR/$tdir
	test_mkdir $DIR/$tdir
	NUMFREE=$(df -i -P $DIR | tail -n 1 | awk '{ print $4 }')
	[[ $NUMFREE -gt 100000 ]] && NUMFREE=100000 || NUMFREE=$((NUMFREE-1000))
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

		[[ $swrong -lt $ewrong ]] &&
			log "statahead was stopped, maybe too many locks held!"
		[[ $delta -eq 0 || $delta_sa -eq 0 ]] && continue

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
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

test_123c() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"

	test_mkdir -i 0 -c 1 $DIR/$tdir.0
	test_mkdir -i 1 -c 1 $DIR/$tdir.1
	touch $DIR/$tdir.1/{1..3}
	mv $DIR/$tdir.1/{1..3} $DIR/$tdir.0

	remount_client $MOUNT

	$MULTIOP $DIR/$tdir.0 Q

	# let statahead to complete
	ls -l $DIR/$tdir.0 > /dev/null

	testid=$(echo $TESTNAME | tr '_' ' ')
	dmesg | tac | sed "/$testid/,$ d" | grep "Can not initialize inode" &&
		error "statahead warning" || true
}
run_test 123c "Can not initialize inode warning on DNE statahead"

test_124a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$LCTL get_param -n mdc.*.connect_flags | grep -q lru_resize ||
		skip_env "no lru resize on server"

	local NR=2000

	test_mkdir $DIR/$tdir

	log "create $NR files at $DIR/$tdir"
	createmany -o $DIR/$tdir/f $NR ||
		error "failed to create $NR files in $DIR/$tdir"

	cancel_lru_locks mdc
	ls -l $DIR/$tdir > /dev/null

	local NSDIR=""
	local LRU_SIZE=0
	for VALUE in $($LCTL get_param ldlm.namespaces.*mdc-*.lru_size); do
		local PARAM=$(echo ${VALUE[0]} | cut -d "=" -f1)
		LRU_SIZE=$($LCTL get_param -n $PARAM)
		if [[ $LRU_SIZE -gt $(default_lru_size) ]]; then
			NSDIR=$(echo $PARAM | cut -d "." -f1-3)
			log "NSDIR=$NSDIR"
                        log "NS=$(basename $NSDIR)"
                        break
                fi
        done

	if [[ -z "$NSDIR" || $LRU_SIZE -lt $(default_lru_size) ]]; then
		skip "Not enough cached locks created!"
	fi
	log "LRU=$LRU_SIZE"

	local SLEEP=30

	# We know that lru resize allows one client to hold $LIMIT locks
	# for 10h. After that locks begin to be killed by client.
	local MAX_HRS=10
	local LIMIT=$($LCTL get_param -n $NSDIR.pool.limit)
	log "LIMIT=$LIMIT"
	if [ $LIMIT -lt $LRU_SIZE ]; then
		skip "Limit is too small $LIMIT"
	fi

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
	local OLD_LVF=$($LCTL get_param -n $NSDIR.pool.lock_volume_factor)
	log "OLD_LVF=$OLD_LVF"
	$LCTL set_param -n $NSDIR.pool.lock_volume_factor $LVF

        # Let's make sure that we really have some margin. Client checks
        # cached locks every 10 sec.
        SLEEP=$((SLEEP+20))
        log "Sleep ${SLEEP} sec"
        local SEC=0
        while ((SEC<$SLEEP)); do
                echo -n "..."
                sleep 5
                SEC=$((SEC+5))
		LRU_SIZE=$($LCTL get_param -n $NSDIR/lru_size)
                echo -n "$LRU_SIZE"
        done
        echo ""
	$LCTL set_param -n $NSDIR.pool.lock_volume_factor $OLD_LVF
	local LRU_SIZE_A=$($LCTL get_param -n $NSDIR.lru_size)

	[[ $LRU_SIZE_B -gt $LRU_SIZE_A ]] || {
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
	local limit=$($LCTL get_param \
		      -n ldlm.namespaces.*-MDT0000-mdc-*.pool.limit)
	local max=0
	for l in $limit; do
		if [[ $l -gt $max ]]; then
			max=$l
		fi
	done
	echo $max
}

test_124b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$LCTL get_param -n mdc.*.connect_flags | grep -q lru_resize ||
		skip_env "no lru resize on server"

	LIMIT=$(get_max_pool_limit)

	NR=$(($(default_lru_size)*20))
	if [[ $NR -gt $LIMIT ]]; then
		log "Limit lock number by $LIMIT locks"
		NR=$LIMIT
	fi

	IFree=$(mdsrate_inodes_available)
	if [ $IFree -lt $NR ]; then
		log "Limit lock number by $IFree inodes"
		NR=$IFree
	fi

	lru_resize_disable mdc
	test_mkdir -p $DIR/$tdir/disable_lru_resize

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
	test_mkdir -p $DIR/$tdir/enable_lru_resize

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

test_124c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$LCTL get_param -n mdc.*.connect_flags | grep -q lru_resize ||
		skip_env "no lru resize on server"

	# cache ununsed locks on client
	local nr=100
	cancel_lru_locks mdc
	test_mkdir $DIR/$tdir
	createmany -o $DIR/$tdir/f $nr ||
		error "failed to create $nr files in $DIR/$tdir"
	ls -l $DIR/$tdir > /dev/null

	local nsdir="ldlm.namespaces.*-MDT0000-mdc-*"
	local unused=$($LCTL get_param -n $nsdir.lock_unused_count)
	local max_age=$($LCTL get_param -n $nsdir.lru_max_age)
	local recalc_p=$($LCTL get_param -n $nsdir.pool.recalc_period)
	echo "unused=$unused, max_age=$max_age, recalc_p=$recalc_p"

	# set lru_max_age to 1 sec
	$LCTL set_param $nsdir.lru_max_age=1000 # milliseconds
	echo "sleep $((recalc_p * 2)) seconds..."
	sleep $((recalc_p * 2))

	local remaining=$($LCTL get_param -n $nsdir.lock_unused_count)
	# restore lru_max_age
	$LCTL set_param -n $nsdir.lru_max_age $max_age
	[ $remaining -eq 0 ] || error "$remaining locks are not canceled"
	unlinkmany $DIR/$tdir/f $nr
}
run_test 124c "LRUR cancel very aged locks"

test_124d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$LCTL get_param -n mdc.*.connect_flags | grep -q lru_resize ||
		skip_env "no lru resize on server"

	# cache ununsed locks on client
	local nr=100

	lru_resize_disable mdc
	stack_trap "lru_resize_enable mdc" EXIT

	cancel_lru_locks mdc

	# asynchronous object destroy at MDT could cause bl ast to client
	test_mkdir $DIR/$tdir
	createmany -o $DIR/$tdir/f $nr ||
		error "failed to create $nr files in $DIR/$tdir"
	stack_trap "unlinkmany $DIR/$tdir/f $nr" EXIT

	ls -l $DIR/$tdir > /dev/null

	local nsdir="ldlm.namespaces.*-MDT0000-mdc-*"
	local unused=$($LCTL get_param -n $nsdir.lock_unused_count)
	local max_age=$($LCTL get_param -n $nsdir.lru_max_age)
	local recalc_p=$($LCTL get_param -n $nsdir.pool.recalc_period)

	echo "unused=$unused, max_age=$max_age, recalc_p=$recalc_p"

	# set lru_max_age to 1 sec
	$LCTL set_param $nsdir.lru_max_age=1000 # milliseconds
	stack_trap "$LCTL set_param -n $nsdir.lru_max_age $max_age" EXIT

	echo "sleep $((recalc_p * 2)) seconds..."
	sleep $((recalc_p * 2))

	local remaining=$($LCTL get_param -n $nsdir.lock_unused_count)

	[ $remaining -eq 0 ] || error "$remaining locks are not canceled"
}
run_test 124d "cancel very aged locks if lru-resize diasbaled"

test_125() { # 13358
	$LCTL get_param -n llite.*.client_type | grep -q local ||
		skip "must run as local client"
	$LCTL get_param -n mdc.*-mdc-*.connect_flags | grep -q acl ||
		skip_env "must have acl enabled"
	[ -z "$(which setfacl)" ] && skip_env "must have setfacl tool"

	test_mkdir $DIR/$tdir
	$LFS setstripe -S 65536 -c -1 $DIR/$tdir || error "setstripe failed"
	setfacl -R -m u:bin:rwx $DIR/$tdir || error "setfacl $DIR/$tdir failed"
	ls -ld $DIR/$tdir || error "cannot access $DIR/$tdir"
}
run_test 125 "don't return EPROTO when a dir has a non-default striping and ACLs"

test_126() { # bug 12829/13455
	$GSS && skip_env "must run as gss disabled"
	$LCTL get_param -n llite.*.client_type | grep -q local ||
		skip "must run as local client"
	[ "$UID" != 0 ] && skip "must run as root, not UID $UID"

	$RUNAS -u 0 -g 1 touch $DIR/$tfile || error "touch failed"
	gid=`ls -n $DIR/$tfile | awk '{print $4}'`
	rm -f $DIR/$tfile
	[ $gid -eq "1" ] || error "gid is set to" $gid "instead of 1"
}
run_test 126 "check that the fsgid provided by the client is taken into account"

test_127a() { # bug 15521
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	local name count samp unit min max sum sumsq

	$LCTL set_param llite.*.stats=0

	# perform 2 reads and writes so MAX is different from SUM.
	dd if=/dev/zero of=$DIR/$tfile bs=$PAGE_SIZE count=1
	dd if=/dev/zero of=$DIR/$tfile bs=$PAGE_SIZE count=1
	cancel_lru_locks osc
	dd if=$DIR/$tfile of=/dev/null bs=$PAGE_SIZE count=1
	dd if=$DIR/$tfile of=/dev/null bs=$PAGE_SIZE count=1

	$LCTL get_param llite.*.stats | grep samples > $TMP/$tfile.tmp
	while read name count samp unit min max sum sumsq; do
		echo "got $count $name"
		eval $name=$count || error "Wrong proc format"

		case $name in
		read_bytes)
			[ $count -ne 2 ] && error "count is not 2: $count"
			[ $min -ne $PAGE_SIZE ] &&
				error "min is not $PAGE_SIZE: $min"
			[ $max -ne $PAGE_SIZE ] &&
				error "max is incorrect: $max"
			[ $sum -ne $((PAGE_SIZE * 2)) ] &&
				error "sum is wrong: $sum"
			;;
		write_bytes)
			[ $count -ne 2 ] && error "count is not 2: $count"
			[ $min -ne $PAGE_SIZE ] &&
				error "min is not $PAGE_SIZE: $min"
			[ $max -ne $PAGE_SIZE ] &&
				error "max is incorrect: $max"
			[ $sum -ne $((PAGE_SIZE * 2)) ] &&
				error "sum is wrong: $sum"
			;;
		*) ;;
		esac
	done < $TMP/$tfile.tmp

	#check that we actually got some stats
	[ "$read_bytes" ] || error "Missing read_bytes stats"
	[ "$write_bytes" ] || error "Missing write_bytes stats"
	[ "$read_bytes" != 0 ] || error "no read done"
	[ "$write_bytes" != 0 ] || error "no write done"

	rm -f $TMP/${tfile}.tmp
}
run_test 127b "verify the llite client stats are sane"

test_127c() { # LU-12394
	[ "$OSTCOUNT" -lt "2" ] && skip_env "needs >= 2 OSTs"
	local size
	local bsize
	local reads
	local writes
	local count

	$LCTL set_param llite.*.extents_stats=1
	stack_trap "$LCTL set_param llite.*.extents_stats=0" EXIT

	# Use two stripes so there is enough space in default config
	$LFS setstripe -c 2 $DIR/$tfile

	# Extent stats start at 0-4K and go in power of two buckets
	# LL_HIST_START = 12 --> 2^12 = 4K
	# We do 3K*2^i, so 3K, 6K, 12K, 24K... hitting each bucket.
	# We do not do buckets larger than 64 MiB to avoid ENOSPC issues on
	# small configs
	for size in 3K 6K 12K 24K 48K 96K 192K 384K 768K 1536K 3M 6M 12M 24M 48M;
		do
		# Write and read, 2x each, second time at a non-zero offset
		dd if=/dev/zero of=$DIR/$tfile bs=$size count=1
		dd if=/dev/zero of=$DIR/$tfile bs=$size count=1 seek=10
		dd if=$DIR/$tfile of=/dev/null bs=$size count=1
		dd if=$DIR/$tfile of=/dev/null bs=$size count=1 seek=10
		rm -f $DIR/$tfile
	done

	$LCTL get_param llite.*.extents_stats

	count=2
	for bsize in 4K 8K 16K 32K 64K 128K 256K 512K 1M 2M 4M 8M 16M 32M 64M;
		do
		local bucket=$($LCTL get_param -n llite.*.extents_stats |
				grep -m 1 $bsize)
		reads=$(echo $bucket | awk '{print $5}')
		writes=$(echo $bucket | awk '{print $9}')
		[ "$reads" -eq $count ] ||
			error "$reads reads in < $bsize bucket, expect $count"
		[ "$writes" -eq $count ] ||
			error "$writes writes in < $bsize bucket, expect $count"
	done

	# Test mmap write and read
	$LCTL set_param llite.*.extents_stats=c
	size=512
	dd if=/dev/zero of=$DIR/$tfile bs=${size}K count=1
	$MULTIOP $DIR/$tfile OSMRUc || error "$MULTIOP $DIR/$tfile failed"
	$MULTIOP $DIR/$tfile OSMWUc || error "$MULTIOP $DIR/$tfile failed"

	$LCTL get_param llite.*.extents_stats

	count=$(((size*1024) / PAGE_SIZE))

	bsize=$((2 * PAGE_SIZE / 1024))K

	bucket=$($LCTL get_param -n llite.*.extents_stats |
			grep -m 1 $bsize)
	reads=$(echo $bucket | awk '{print $5}')
	writes=$(echo $bucket | awk '{print $9}')
	# mmap writes fault in the page first, creating an additonal read
	[ "$reads" -eq $((2 * count)) ] ||
		error "$reads reads in < $bsize bucket, expect $count"
	[ "$writes" -eq $count ] ||
		error "$writes writes in < $bsize bucket, expect $count"
}
run_test 127c "test llite extent stats with regular & mmap i/o"

test_128() { # bug 15212
	touch $DIR/$tfile
	$LFS 2>&1 <<-EOF | tee $TMP/$tfile.log
		find $DIR/$tfile
		find $DIR/$tfile
	EOF

	result=$(grep error $TMP/$tfile.log)
	rm -f $DIR/$tfile $TMP/$tfile.log
	[ -z "$result" ] ||
		error "consecutive find's under interactive lfs failed"
}
run_test 128 "interactive lfs for 2 consecutive find's"

set_dir_limits () {
	local mntdev
	local canondev
	local node

	local ldproc=/proc/fs/ldiskfs
	local facets=$(get_facets MDS)

	for facet in ${facets//,/ }; do
		canondev=$(ldiskfs_canon \
			   *.$(convert_facet2label $facet).mntdev $facet)
		do_facet $facet "test -e $ldproc/$canondev/max_dir_size" ||
			ldproc=/sys/fs/ldiskfs
		do_facet $facet "echo $1 >$ldproc/$canondev/max_dir_size"
		do_facet $facet "echo $2 >$ldproc/$canondev/warning_dir_size"
	done
}

check_mds_dmesg() {
	local facets=$(get_facets MDS)
	for facet in ${facets//,/ }; do
		do_facet $facet "dmesg | tail -3 | grep -q $1" && return 0
	done
	return 1
}

test_129() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $MDS1_VERSION -ge $(version_code 2.5.56) ]] ||
		skip "Need MDS version with at least 2.5.56"
	if [ "$mds1_FSTYPE" != ldiskfs ]; then
		skip_env "ldiskfs only test"
	fi
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local ENOSPC=28
	local EFBIG=27
	local has_warning=false

	rm -rf $DIR/$tdir
	mkdir -p $DIR/$tdir

	# block size of mds1
	local maxsize=$(($($LCTL get_param -n mdc.*MDT0000*.blocksize) * 5))
	set_dir_limits $maxsize $maxsize
	local dirsize=$(stat -c%s "$DIR/$tdir")
	local nfiles=0
	while [[ $dirsize -le $maxsize ]]; do
		$MULTIOP $DIR/$tdir/file_base_$nfiles Oc
		rc=$?
		if ! $has_warning; then
			check_mds_dmesg '"is approaching"' && has_warning=true
		fi
		# check two errors:
		# ENOSPC for new ext4 max_dir_size (kernel commit df981d03ee)
		# EFBIG for previous versions included in ldiskfs series
		if [ $rc -eq $EFBIG -o $rc -eq $ENOSPC ]; then
			set_dir_limits 0 0
			echo "return code $rc received as expected"

			createmany -o $DIR/$tdir/file_extra_$nfiles. 5 ||
				error_exit "create failed w/o dir size limit"

			check_mds_dmesg '"has reached"' ||
				error_exit "reached message should be output"

			[ $has_warning = "false" ] &&
				error_exit "warning message should be output"

			dirsize=$(stat -c%s "$DIR/$tdir")

			[[ $dirsize -ge $maxsize ]] && return 0
			error_exit "current dir size $dirsize, " \
				   "previous limit $maxsize"
		elif [ $rc -ne 0 ]; then
			set_dir_limits 0 0
			error_exit "return $rc received instead of expected " \
				   "$EFBIG or $ENOSPC, files in dir $dirsize"
		fi
		nfiles=$((nfiles + 1))
		dirsize=$(stat -c%s "$DIR/$tdir")
	done

	set_dir_limits 0 0
	error "exceeded dir size limit $maxsize($MDSCOUNT) : $dirsize bytes"
}
run_test 129 "test directory size limit ========================"

OLDIFS="$IFS"
cleanup_130() {
	trap 0
	IFS="$OLDIFS"
}

test_130a() {
	local filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip_env "filefrag does not support FIEMAP"

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 65536 -c 1 $fm_file || error "setstripe on $fm_file"
	dd if=/dev/zero of=$fm_file bs=65536 count=1 ||
		error "dd failed for $fm_file"

	# LU-1795: test filefrag/FIEMAP once, even if unsupported
	filefrag -ves $fm_file
	RC=$?
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip_env "ORI-366/LU-1941: FIEMAP unimplemented on ZFS"
	[ $RC != 0 ] && error "filefrag $fm_file failed"

	filefrag_op=$(filefrag -ve -k $fm_file |
			sed -n '/ext:/,/found/{/ext:/d; /found/d; p}')
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
	[ "$OSTCOUNT" -lt "2" ] && skip_env "needs >= 2 OSTs"

	local filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip_env "filefrag does not support FIEMAP"

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 65536 -c $OSTCOUNT $fm_file ||
			error "setstripe on $fm_file"
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip_env "ORI-366/LU-1941: FIEMAP unimplemented on ZFS"

	dd if=/dev/zero of=$fm_file bs=1M count=$OSTCOUNT ||
		error "dd failed on $fm_file"

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_op=$(filefrag -ve -k $fm_file |
			sed -n '/ext:/,/found/{/ext:/d; /found/d; p}')

	last_lun=$(echo $filefrag_op | cut -d: -f5 |
		sed -e 's/^[ \t]*/0x/' | sed -e 's/0x0x/0x/')

	IFS=$'\n'
	tot_len=0
	num_luns=1
	for line in $filefrag_op
	do
		frag_lun=$(echo $line | cut -d: -f5 |
			sed -e 's/^[ \t]*/0x/' | sed -e 's/0x0x/0x/')
		ext_len=$(echo $line | cut -d: -f4)
		if (( $frag_lun != $last_lun )); then
			if (( tot_len != 1024 )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned " \
				"len $tot_len for OST $last_lun instead of 1024"
				return
			else
				(( num_luns += 1 ))
				tot_len=0
			fi
		fi
		(( tot_len += ext_len ))
		last_lun=$frag_lun
	done
	if (( num_luns != $OSTCOUNT || tot_len != 1024 )); then
		cleanup_130
		error "FIEMAP on $fm_file failed; returned wrong number of " \
			"luns or wrong len for OST $last_lun"
		return
	fi

	cleanup_130

	echo "FIEMAP on $OSTCOUNT-stripe file succeeded"
}
run_test 130b "FIEMAP ($OSTCOUNT-stripe file)"

test_130c() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip "filefrag does not support FIEMAP"

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 65536 -c 2 $fm_file || error "setstripe on $fm_file"
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip_env "ORI-366/LU-1941: FIEMAP unimplemented on ZFS"

	dd if=/dev/zero of=$fm_file seek=1 bs=1M count=1 ||
			error "dd failed on $fm_file"

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_op=$(filefrag -ve -k $fm_file |
		sed -n '/ext:/,/found/{/ext:/d; /found/d; p}')

	last_lun=$(echo $filefrag_op | cut -d: -f5 |
		sed -e 's/^[ \t]*/0x/' | sed -e 's/0x0x/0x/')

	IFS=$'\n'
	tot_len=0
	num_luns=1
	for line in $filefrag_op
	do
		frag_lun=$(echo $line | cut -d: -f5 |
			sed -e 's/^[ \t]*/0x/' | sed -e 's/0x0x/0x/')
		ext_len=$(echo $line | cut -d: -f4)
		if (( $frag_lun != $last_lun )); then
			logical=`echo $line | cut -d: -f2 | cut -d. -f1`
			if (( logical != 512 )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned " \
				"logical start for lun $logical instead of 512"
				return
			fi
			if (( tot_len != 512 )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned " \
				"len $tot_len for OST $last_lun instead of 1024"
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
		error "FIEMAP on $fm_file failed; returned wrong number of " \
			"luns or wrong len for OST $last_lun"
		return
	fi

	cleanup_130

	echo "FIEMAP on 2-stripe file with hole succeeded"
}
run_test 130c "FIEMAP (2-stripe file with hole)"

test_130d() {
	[[ $OSTCOUNT -lt 3 ]] && skip_env "needs >= 3 OSTs"

	filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip "filefrag does not support FIEMAP"

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 65536 -c $OSTCOUNT $fm_file ||
			error "setstripe on $fm_file"
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip_env "ORI-366/LU-1941: FIEMAP unimplemented on ZFS"

	local actual_stripe_count=$($GETSTRIPE -c $fm_file)
	dd if=/dev/zero of=$fm_file bs=1M count=$actual_stripe_count ||
		error "dd failed on $fm_file"

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_op=$(filefrag -ve -k $fm_file |
			sed -n '/ext:/,/found/{/ext:/d; /found/d; p}')

	last_lun=$(echo $filefrag_op | cut -d: -f5 |
		sed -e 's/^[ \t]*/0x/' | sed -e 's/0x0x/0x/')

	IFS=$'\n'
	tot_len=0
	num_luns=1
	for line in $filefrag_op
	do
		frag_lun=$(echo $line | cut -d: -f5 |
			sed -e 's/^[ \t]*/0x/' | sed -e 's/0x0x/0x/')
		ext_len=$(echo $line | cut -d: -f4)
		if (( $frag_lun != $last_lun )); then
			if (( tot_len != 1024 )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned " \
				"len $tot_len for OST $last_lun instead of 1024"
				return
			else
				(( num_luns += 1 ))
				tot_len=0
			fi
		fi
		(( tot_len += ext_len ))
		last_lun=$frag_lun
	done
	if (( num_luns != actual_stripe_count || tot_len != 1024 )); then
		cleanup_130
		error "FIEMAP on $fm_file failed; returned wrong number of " \
			"luns or wrong len for OST $last_lun"
		return
	fi

	cleanup_130

	echo "FIEMAP on N-stripe file succeeded"
}
run_test 130d "FIEMAP (N-stripe file)"

test_130e() {
	[[ $OSTCOUNT -lt 2 ]] && skip_env "needs >= 2 OSTs"

	filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip_env "filefrag does not support FIEMAP"

	trap cleanup_130 EXIT RETURN

	local fm_file=$DIR/$tfile
	$SETSTRIPE -S 131072 -c 2 $fm_file || error "setstripe on $fm_file"
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $fm_file) + 1)))" = "zfs" ] &&
		skip_env "ORI-366/LU-1941: FIEMAP unimplemented on ZFS"

	NUM_BLKS=512
	EXPECTED_LEN=$(( (NUM_BLKS / 2) * 64 ))
	for ((i = 0; i < $NUM_BLKS; i++))
	do
		dd if=/dev/zero of=$fm_file count=1 bs=64k seek=$((2*$i)) conv=notrunc > /dev/null 2>&1
	done

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_op=$(filefrag -ve -k $fm_file |
			sed -n '/ext:/,/found/{/ext:/d; /found/d; p}')

	last_lun=$(echo $filefrag_op | cut -d: -f5 |
		sed -e 's/^[ \t]*/0x/' | sed -e 's/0x0x/0x/')

	IFS=$'\n'
	tot_len=0
	num_luns=1
	for line in $filefrag_op
	do
		frag_lun=$(echo $line | cut -d: -f5 |
			sed -e 's/^[ \t]*/0x/' | sed -e 's/0x0x/0x/')
		ext_len=$(echo $line | cut -d: -f4)
		if (( $frag_lun != $last_lun )); then
			if (( tot_len != $EXPECTED_LEN )); then
				cleanup_130
				error "FIEMAP on $fm_file failed; returned " \
				"len $tot_len for OST $last_lun instead " \
				"of $EXPECTED_LEN"
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
		error "FIEMAP on $fm_file failed; returned wrong number " \
			"of luns or wrong len for OST $last_lun"
		return
	fi

	cleanup_130

	echo "FIEMAP with continuation calls succeeded"
}
run_test 130e "FIEMAP (test continuation FIEMAP calls)"

test_130f() {
	filefrag_op=$(filefrag -e 2>&1 | grep "invalid option")
	[ -n "$filefrag_op" ] && skip "filefrag does not support FIEMAP"

	local fm_file=$DIR/$tfile
	$MULTIOP $fm_file oO_RDWR:O_CREAT:O_LOV_DELAY_CREATE:T33554432c ||
		error "multiop create with lov_delay_create on $fm_file"

	filefrag -ves $fm_file || error "filefrag $fm_file failed"
	filefrag_extents=$(filefrag -vek $fm_file |
			   awk '/extents? found/ { print $2 }')
	if [[ "$filefrag_extents" != "0" ]]; then
		error "FIEMAP on $fm_file failed; " \
		      "returned $filefrag_extents expected 0"
	fi

	rm -f $fm_file
}
run_test 130f "FIEMAP (unstriped file)"

# Test for writev/readv
test_131a() {
	rwv -f $DIR/$tfile -w -n 3 524288 1048576 1572864 ||
		error "writev test failed"
	rwv -f $DIR/$tfile -r -v -n 2 1572864 1048576 ||
		error "readv failed"
	rm -f $DIR/$tfile
}
run_test 131a "test iov's crossing stripe boundary for writev/readv"

test_131b() {
	local fsize=$((524288 + 1048576 + 1572864))
	rwv -f $DIR/$tfile -w -a -n 3 524288 1048576 1572864 &&
		$CHECKSTAT -t file $DIR/$tfile -s $fsize ||
			error "append writev test failed"

	((fsize += 1572864 + 1048576))
	rwv -f $DIR/$tfile -w -a -n 2 1572864 1048576 &&
		$CHECKSTAT -t file $DIR/$tfile -s $fsize ||
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

check_stats() {
	local facet=$1
	local op=$2
	local want=${3:-0}
	local res

	case $facet in
	mds*) res=$(do_facet $facet \
		   $LCTL get_param mdt.$FSNAME-MDT0000.md_stats | grep "$op")
		 ;;
	ost*) res=$(do_facet $facet \
		   $LCTL get_param obdfilter.$FSNAME-OST0000.stats | grep "$op")
		 ;;
	*) error "Wrong facet '$facet'" ;;
	esac
	[ "$res" ] || error "The counter for $op on $facet was not incremented"
	# if the argument $3 is zero, it means any stat increment is ok.
	if [[ $want -gt 0 ]]; then
		local count=$(echo $res | awk '{ print $2 }')
		[[ $count -ne $want ]] &&
			error "The $op counter on $facet is $count, not $want"
	fi
}

test_133a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	do_facet $SINGLEMDS $LCTL list_param mdt.*.rename_stats ||
		skip_env "MDS doesn't support rename stats"

	local testdir=$DIR/${tdir}/stats_testdir

	mkdir -p $DIR/${tdir}

	# clear stats.
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	do_facet ost1 $LCTL set_param obdfilter.*.stats=clear

	# verify mdt stats first.
	mkdir ${testdir} || error "mkdir failed"
	check_stats $SINGLEMDS "mkdir" 1
	touch ${testdir}/${tfile} || error "touch failed"
	check_stats $SINGLEMDS "open" 1
	check_stats $SINGLEMDS "close" 1
	[ $MDS1_VERSION -ge $(version_code 2.8.54) ] && {
		mknod ${testdir}/${tfile}-pipe p || error "mknod failed"
		check_stats $SINGLEMDS "mknod" 2
	}
	rm -f ${testdir}/${tfile}-pipe || error "pipe remove failed"
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
	local res

	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local testdir=$DIR/${tdir}/stats_testdir

	mkdir -p ${testdir} || error "mkdir failed"
	touch ${testdir}/${tfile} || error "touch failed"
	cancel_lru_locks mdc

	# clear stats.
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	do_facet ost1 $LCTL set_param obdfilter.*.stats=clear

	# extra mdt stats verification.
	chmod 444 ${testdir}/${tfile} || error "chmod failed"
	check_stats $SINGLEMDS "setattr" 1
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	if [ $MDS1_VERSION -ne $(version_code 2.2.0) ]
	then		# LU-1740
		ls -l ${testdir}/${tfile} > /dev/null|| error "ls failed"
		check_stats $SINGLEMDS "getattr" 1
	fi
	rm -rf $DIR/${tdir}

	# when DNE is enabled, MDT uses STATFS RPC to ping other targets
	# so the check below is not reliable
	[ $MDSCOUNT -eq 1 ] || return 0

	# Sleep to avoid a cached response.
	#define OBD_STATFS_CACHE_SECONDS 1
	sleep 2
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	do_facet ost1 $LCTL set_param obdfilter.*.exports.*.stats=clear
	$LFS df || error "lfs failed"
	check_stats $SINGLEMDS "statfs" 1

	# check aggregated statfs (LU-10018)
	[ $MDS1_VERSION -lt $(version_code 2.11.54) ] &&
		return 0
	[ $CLIENT_VERSION -lt $(version_code 2.11.54) ] &&
		return 0
	sleep 2
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	do_facet ost1 $LCTL set_param obdfilter.*.exports.*.stats=clear
	df $DIR
	check_stats $SINGLEMDS "statfs" 1

	# We want to check that the client didn't send OST_STATFS to
	# ost1 but the MDT also uses OST_STATFS for precreate. So some
	# extra care is needed here.
	if remote_mds; then
		local nid=$($LCTL list_nids | head -1 | sed  "s/\./\\\./g")
		local param="obdfilter.$FSNAME-OST0000.exports.'$nid'.stats"

		res=$(do_facet ost1 $LCTL get_param $param | grep statfs)
		[ "$res" ] && error "OST got STATFS"
	fi

	return 0
}
run_test 133b "Verifying extra MDT stats =================================="

test_133c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local testdir=$DIR/$tdir/stats_testdir

	test_mkdir -p $testdir

	# verify obdfilter stats.
	$SETSTRIPE -c 1 -i 0 $testdir/$tfile
	sync
	cancel_lru_locks osc
	wait_delete_completed

	# clear stats.
	do_facet $SINGLEMDS $LCTL set_param mdt.*.md_stats=clear
	do_facet ost1 $LCTL set_param obdfilter.*.stats=clear

	dd if=/dev/zero of=$testdir/$tfile conv=notrunc bs=512k count=1 ||
		error "dd failed"
	sync
	cancel_lru_locks osc
	check_stats ost1 "write" 1

	dd if=$testdir/$tfile of=/dev/null bs=1k count=1 || error "dd failed"
	check_stats ost1 "read" 1

	> $testdir/$tfile || error "truncate failed"
	check_stats ost1 "punch" 1

	rm -f $testdir/$tfile || error "file remove failed"
	wait_delete_completed
	check_stats ost1 "destroy" 1

	rm -rf $DIR/$tdir
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
	local sample=$(do_facet $SINGLEMDS $LCTL \
		get_param mdt.$FSNAME-MDT0000.rename_stats |
		grep -A1 $context |
		awk '/ '${size}'/ {print $4}' | sed -e "s/,//g")
	echo $sample
}

test_133d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	do_facet $SINGLEMDS $LCTL list_param mdt.*.rename_stats ||
		skip_env "MDS doesn't support rename stats"

	local testdir1=$DIR/${tdir}/stats_testdir1
	local testdir2=$DIR/${tdir}/stats_testdir2
	mkdir -p $DIR/${tdir}

	do_facet $SINGLEMDS $LCTL set_param mdt.*.rename_stats=clear

	lfs mkdir -i 0 -c 1 ${testdir1} || error "mkdir failed"
	lfs mkdir -i 0 -c 1 ${testdir2} || error "mkdir failed"

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

	local cmd="do_facet $SINGLEMDS $LCTL "
	cmd+="get_param mdt.$FSNAME-MDT0000.rename_stats"

	eval $cmd || error "$cmd failed"
	local samedir=$($cmd | grep 'same_dir')
	local same_sample=$(get_rename_size $testdir1_size)
	[ -z "$samedir" ] && error "samedir_rename_size count error"
	[[ $same_sample -eq 1 ]] ||
		error "samedir_rename_size error $same_sample"
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
	[[ $src_sample -eq 1 ]] ||
		error "crossdir_rename_size error $src_sample"
	[[ $tgt_sample -eq 1 ]] ||
		error "crossdir_rename_size error $tgt_sample"
	echo "Check cross dir rename stats success"
	rm -rf $DIR/${tdir}
}
run_test 133d "Verifying rename_stats ========================================"

test_133e() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local testdir=$DIR/${tdir}/stats_testdir
	local ctr f0 f1 bs=32768 count=42 sum

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
			"obdfilter.*.exports.*.stats" |
			awk -v ctr=$ctr 'BEGIN { sum = 0 }
				$1 == ctr { sum += $7 }
				END { printf("%0.0f", sum) }')

		if ((sum != bs * count)); then
			error "Bad $ctr sum, expected $((bs * count)), got $sum"
		fi
	done

	rm -rf $DIR/${tdir}
}
run_test 133e "Verifying OST {read,write}_bytes nid stats ================="

proc_regexp="/{proc,sys}/{fs,sys,kernel/debug}/{lustre,lnet}/"

# Some versions of find (4.5.11, 4.5.14) included in CentOS 7.3-7.5 do
# not honor the -ignore_readdir_race option correctly. So we call
# error_ignore() rather than error() in these cases. See LU-11152.
error_133() {
	if (find --version; do_facet mds1 find --version) |
		grep -q '\b4\.5\.1[1-4]\b'; then
		error_ignore LU-11152 "$@"
	else
		error "$@"
	fi
}

test_133f() {
	# First without trusting modes.
	local proc_dirs=$(eval \ls -d $proc_regexp 2>/dev/null)
	echo "proc_dirs='$proc_dirs'"
	[ -n "$proc_dirs" ] || error "no proc_dirs on $HOSTNAME"
	find $proc_dirs -exec cat '{}' \; &> /dev/null

	# Second verifying readability.
	$LCTL get_param -R '*' &> /dev/null

	# Verifing writability with badarea_io.
	find $proc_dirs \
		-ignore_readdir_race \
		-type f \
		-not -name force_lbug \
		-not -name changelog_mask \
		-exec badarea_io '{}' \; ||
			error_133 "find $proc_dirs failed"
}
run_test 133f "Check reads/writes of client lustre proc files with bad area io"

test_133g() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"

	# eventually, this can also be replaced with "lctl get_param -R",
	# but not until that option is always available on the server
	local facet
	for facet in mds1 ost1; do
		[ $(lustre_version_code $facet) -le $(version_code 2.5.54) ] &&
			skip_noexit "Too old lustre on $facet"
		local facet_proc_dirs=$(do_facet $facet \
					\\\ls -d $proc_regexp 2>/dev/null)
		echo "${facet}_proc_dirs='$facet_proc_dirs'"
		[ -z "$facet_proc_dirs" ] && error "no proc_dirs on $facet"
		do_facet $facet find $facet_proc_dirs \
			! -name req_history \
			-exec cat '{}' \\\; &> /dev/null

		do_facet $facet find $facet_proc_dirs \
			! -name req_history \
			-type f \
			-exec cat '{}' \\\; &> /dev/null ||
				error "proc file read failed"

		do_facet $facet find $facet_proc_dirs \
			-ignore_readdir_race \
			-type f \
			-not -name force_lbug \
			-not -name changelog_mask \
			-exec badarea_io '{}' \\\; ||
				error_133 "$facet find $facet_proc_dirs failed"
	done

	# remount the FS in case writes/reads /proc break the FS
	cleanup || error "failed to unmount"
	setup || error "failed to setup"
	true
}
run_test 133g "Check reads/writes of server lustre proc files with bad area io"

test_133h() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"
	[[ $MDS1_VERSION -lt $(version_code 2.9.54) ]] &&
		skip "Need MDS version at least 2.9.54"

	local facet

	for facet in client mds1 ost1; do
		local facet_proc_dirs=$(do_facet $facet \
					\\\ls -d $proc_regexp 2> /dev/null)
		[ -z "$facet_proc_dirs" ] && error "no proc_dirs on $facet"
		echo "${facet}_proc_dirs='$facet_proc_dirs'"
		# Get the list of files that are missing the terminating newline
		local missing=($(do_facet $facet \
			find ${facet_proc_dirs} -type f \|		\
				while read F\; do			\
					awk -v FS='\v' -v RS='\v\v'	\
					"'END { if(NR>0 &&		\
					\\\$NF !~ /.*\\\n\$/)		\
						print FILENAME}'"	\
					'\$F'\;				\
				done 2>/dev/null))
		[ ${#missing[*]} -eq 0 ] ||
			error "files do not end with newline: ${missing[*]}"
	done
}
run_test 133h "Proc files should end with newlines"

test_134a() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $MDS1_VERSION -lt $(version_code 2.7.54) ]] &&
		skip "Need MDS version at least 2.7.54"

	mkdir -p $DIR/$tdir || error "failed to create $DIR/$tdir"
	cancel_lru_locks mdc

	local nsdir="ldlm.namespaces.*-MDT0000-mdc-*"
	local unused=$($LCTL get_param -n $nsdir.lock_unused_count)
	[ $unused -eq 0 ] || error "$unused locks are not cleared"

	local nr=1000
	createmany -o $DIR/$tdir/f $nr ||
		error "failed to create $nr files in $DIR/$tdir"
	unused=$($LCTL get_param -n $nsdir.lock_unused_count)

	#define OBD_FAIL_LDLM_WATERMARK_LOW     0x327
	do_facet mds1 $LCTL set_param fail_loc=0x327
	do_facet mds1 $LCTL set_param fail_val=500
	touch $DIR/$tdir/m

	echo "sleep 10 seconds ..."
	sleep 10
	local lck_cnt=$($LCTL get_param -n $nsdir.lock_unused_count)

	do_facet mds1 $LCTL set_param fail_loc=0
	do_facet mds1 $LCTL set_param fail_val=0
	[ $lck_cnt -lt $unused ] ||
		error "No locks reclaimed, before:$unused, after:$lck_cnt"

	rm $DIR/$tdir/m
	unlinkmany $DIR/$tdir/f $nr
}
run_test 134a "Server reclaims locks when reaching lock_reclaim_threshold"

test_134b() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $MDS1_VERSION -lt $(version_code 2.7.54) ]] &&
		skip "Need MDS version at least 2.7.54"

	mkdir -p $DIR/$tdir || error "failed to create $DIR/$tdir"
	cancel_lru_locks mdc

	local low_wm=$(do_facet mds1 $LCTL get_param -n \
			ldlm.lock_reclaim_threshold_mb)
	# disable reclaim temporarily
	do_facet mds1 $LCTL set_param ldlm.lock_reclaim_threshold_mb=0

	#define OBD_FAIL_LDLM_WATERMARK_HIGH     0x328
	do_facet mds1 $LCTL set_param fail_loc=0x328
	do_facet mds1 $LCTL set_param fail_val=500

	$LCTL set_param debug=+trace

	local nr=600
	createmany -o $DIR/$tdir/f $nr &
	local create_pid=$!

	echo "Sleep $TIMEOUT seconds ..."
	sleep $TIMEOUT
	if ! ps -p $create_pid  > /dev/null 2>&1; then
		do_facet mds1 $LCTL set_param fail_loc=0
		do_facet mds1 $LCTL set_param fail_val=0
		do_facet mds1 $LCTL set_param \
			ldlm.lock_reclaim_threshold_mb=${low_wm}m
		error "createmany finished incorrectly!"
	fi
	do_facet mds1 $LCTL set_param fail_loc=0
	do_facet mds1 $LCTL set_param fail_val=0
	do_facet mds1 $LCTL set_param ldlm.lock_reclaim_threshold_mb=${low_wm}m
	wait $create_pid || return 1

	unlinkmany $DIR/$tdir/f $nr
}
run_test 134b "Server rejects lock request when reaching lock_limit_mb"

test_140() { #bug-17379
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir $DIR/$tdir
	cd $DIR/$tdir || error "Changing to $DIR/$tdir"
	cp $(which stat) . || error "Copying stat to $DIR/$tdir"

	# VFS limits max symlink depth to 5(4KSTACK) or 7(8KSTACK) or 8
	# For kernel > 3.5, bellow only tests consecutive symlink (MAX 40)
	local i=0
	while i=$((i + 1)); do
		test_mkdir $i
		cd $i || error "Changing to $i"
		ln -s ../stat stat || error "Creating stat symlink"
		# Read the symlink until ELOOP present,
		# not LBUGing the system is considered success,
		# we didn't overrun the stack.
		$OPENFILE -f O_RDONLY stat >/dev/null 2>&1; ret=$?
		if [ $ret -ne 0 ]; then
			if [ $ret -eq 40 ]; then
				break  # -ELOOP
			else
				error "Open stat symlink"
					return
			fi
		fi
	done
	i=$((i - 1))
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local TF="$TMP/$tfile"

	dd if=/dev/urandom of=$TF bs=6096 count=1 || error "dd failed"
	cp $TF $DIR/$tfile
	cancel_lru_locks $OSC
	cmp $TF $DIR/$tfile || error "$TMP/$tfile $DIR/$tfile differ"
	remount_client $MOUNT
	df -P $MOUNT
	cmp $TF $DIR/$tfile || error "$TF $DIR/$tfile differ (remount)"

	$TRUNCATE $TF 6000
	$TRUNCATE $DIR/$tfile 6000
	cancel_lru_locks $OSC
	cmp $TF $DIR/$tfile || error "$TF $DIR/$tfile differ (truncate1)"

	echo "12345" >>$TF
	echo "12345" >>$DIR/$tfile
	cancel_lru_locks $OSC
	cmp $TF $DIR/$tfile || error "$TF $DIR/$tfile differ (append1)"

	echo "12345" >>$TF
	echo "12345" >>$DIR/$tfile
	cancel_lru_locks $OSC
	cmp $TF $DIR/$tfile || error "$TF $DIR/$tfile differ (append2)"

	rm -f $TF
	true
}
run_test 150 "truncate/append tests"

#LU-2902 roc_hit was not able to read all values from lproc
function roc_hit_init() {
	local list=$(comma_list $(osts_nodes))
	local dir=$DIR/$tdir-check
	local file=$dir/$tfile
	local BEFORE
	local AFTER
	local idx

	test_mkdir $dir
	#use setstripe to do a write to every ost
	for i in $(seq 0 $((OSTCOUNT-1))); do
		$SETSTRIPE -c 1 -i $i $dir || error "$SETSTRIPE $file failed"
		dd if=/dev/urandom of=$file bs=4k count=4 2>&1 > /dev/null
		idx=$(printf %04x $i)
		BEFORE=$(get_osd_param $list *OST*$idx stats |
			awk '$1 == "cache_access" {sum += $7}
				END { printf("%0.0f", sum) }')

		cancel_lru_locks osc
		cat $file >/dev/null

		AFTER=$(get_osd_param $list *OST*$idx stats |
			awk '$1 == "cache_access" {sum += $7}
				END { printf("%0.0f", sum) }')

		echo BEFORE:$BEFORE AFTER:$AFTER
		if ! let "AFTER - BEFORE == 4"; then
			rm -rf $dir
			error "roc_hit is not safe to use"
		fi
		rm $file
	done

	rm -rf $dir
}

function roc_hit() {
	local list=$(comma_list $(osts_nodes))
	echo $(get_osd_param $list '' stats |
		awk '$1 == "cache_hit" {sum += $7}
			END { printf("%0.0f", sum) }')
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

	local CPAGES=3
	local list=$(comma_list $(osts_nodes))

	# check whether obdfilter is cache capable at all
	if ! get_osd_param $list '' read_cache_enable >/dev/null; then
		skip "not cache-capable obdfilter"
	fi

	# check cache is enabled on all obdfilters
	if get_osd_param $list '' read_cache_enable | grep 0; then
		skip "oss cache is disabled"
	fi

	set_osd_param $list '' writethrough_cache_enable 1

	# check write cache is enabled on all obdfilters
	if get_osd_param $list '' writethrough_cache_enable | grep 0; then
		skip "oss write cache is NOT enabled"
	fi

	roc_hit_init

	#define OBD_FAIL_OBD_NO_LRU  0x609
	do_nodes $list $LCTL set_param fail_loc=0x609

	# pages should be in the case right after write
	dd if=/dev/urandom of=$DIR/$tfile bs=4k count=$CPAGES ||
		error "dd failed"

	local BEFORE=$(roc_hit)
	cancel_lru_locks osc
	cat $DIR/$tfile >/dev/null
	local AFTER=$(roc_hit)

	do_nodes $list $LCTL set_param fail_loc=0

	if ! let "AFTER - BEFORE == CPAGES"; then
		error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
	fi

        # the following read invalidates the cache
        cancel_lru_locks osc
	set_osd_param $list '' read_cache_enable 0
        cat $DIR/$tfile >/dev/null

        # now data shouldn't be found in the cache
	BEFORE=$(roc_hit)
        cancel_lru_locks osc
        cat $DIR/$tfile >/dev/null
	AFTER=$(roc_hit)
        if let "AFTER - BEFORE != 0"; then
                error "IN CACHE: before: $BEFORE, after: $AFTER"
        fi

	set_osd_param $list '' read_cache_enable 1
        rm -f $DIR/$tfile
}
run_test 151 "test cache on oss and controls ==============================="

test_152() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	if [[ $($LCTL get_param -n mdc.*-mdc-*.connect_flags) =~ acl ]]; then
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

	if [ $MDS1_VERSION -ge $(version_code 2.3.51) ]
	then		# LU-3547
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
	fi

	$OPENFILE -f O_LOV_DELAY_CREATE:O_CREAT $test_dir/$tfile-2
	fid=$($LFS path2fid $test_dir/$tfile-2)

	if [ $MDS1_VERSION -ge $(version_code 2.6.50) ]
	then # LU-5424
		echo "cp /etc/passwd $MOUNT/.lustre/fid/$fid"
		cp /etc/passwd $MOUNT/.lustre/fid/$fid ||
			error "create lov data thru .lustre failed"
	fi
	echo "cp /etc/passwd $test_dir/$tfile-2"
	cp /etc/passwd $test_dir/$tfile-2 ||
		error "copy to $test_dir/$tfile-2 failed."
	echo "diff /etc/passwd $MOUNT/.lustre/fid/$fid"
	diff /etc/passwd $MOUNT/.lustre/fid/$fid ||
		error "diff /etc/passwd $MOUNT/.lustre/fid/$fid failed."

	rm -rf $test_dir/tfile.lnk
	rm -rf $test_dir/$tfile-2
}

test_154A() {
	[[ $MDS1_VERSION -lt $(version_code 2.4.1) ]] &&
		skip "Need MDS version at least 2.4.1"

	local tf=$DIR/$tfile
	touch $tf

	local fid=$($LFS path2fid $tf)
	[ -z "$fid" ] && error "path2fid unable to get $tf FID"

	# check that we get the same pathname back
	local found=$($LFS fid2path $MOUNT "$fid")
	[ -z "$found" ] && error "fid2path unable to get '$fid' path"
	[ "$found" == "$tf" ] ||
		error "fid2path($fid=path2fid($tf)) = $found != $tf"
}
run_test 154A "lfs path2fid and fid2path basic checks"

test_154B() {
	[[ $MDS1_VERSION -lt $(version_code 2.4.1) ]] &&
		skip "Need MDS version at least 2.4.1"

	mkdir -p $DIR/$tdir || error "mkdir $tdir failed"
	touch $DIR/$tdir/$tfile || error "touch $DIR/$tdir/$tfile failed"
	local linkea=$($LL_DECODE_LINKEA $DIR/$tdir/$tfile | grep 'pfid')
	[ -z "$linkea" ] && error "decode linkea $DIR/$tdir/$tfile failed"

	local name=$(echo $linkea | awk '/pfid/ {print $5}' | sed -e "s/'//g")
	local PFID=$(echo $linkea | awk '/pfid/ {print $3}' | sed -e "s/,//g")

	# check that we get the same pathname
	echo "PFID: $PFID, name: $name"
	local FOUND=$($LFS fid2path $MOUNT "$PFID")
	[ -z "$FOUND" ] && error "fid2path unable to get $PFID path"
	[ "$FOUND/$name" != "$DIR/$tdir/$tfile" ] &&
		error "ll_decode_linkea has $FOUND/$name != $DIR/$tdir/$tfile"

	rm -rf $DIR/$tdir || error "Can not delete directory $DIR/$tdir"
}
run_test 154B "verify the ll_decode_linkea tool"

test_154a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ -n "$FILESET" ] && skip "SKIP due to FILESET set"
	[[ $MDS1_VERSION -ge $(version_code 2.2.51) ]] ||
		skip "Need MDS version at least 2.2.51"
	[ -z "$(which setfacl)" ] && skip_env "must have setfacl tool"

	cp /etc/hosts $DIR/$tfile

	fid=$($LFS path2fid $DIR/$tfile)
	rc=$?
	[ $rc -ne 0 ] && error "error: could not get fid for $DIR/$tfile."

	dot_lustre_fid_permission_check "$fid" $DIR ||
		error "dot lustre permission check $fid failed"

	ls -a $MOUNT | grep "\.lustre" && error ".lustre should not be listed"

	rm -rf $MOUNT/.lustre && error ".lustre is not allowed to be unlinked"

	touch $MOUNT/.lustre/file &&
		error "creation is not allowed under .lustre"

	mkdir $MOUNT/.lustre/dir &&
		error "mkdir is not allowed under .lustre"

	rm -rf $DIR/$tfile
}
run_test 154a "Open-by-FID"

test_154b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ -n "$FILESET" ] && skip "SKIP due to FILESET set"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[[ $MDS1_VERSION -ge $(version_code 2.2.51) ]] ||
		skip "Need MDS version at least 2.2.51"

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

test_154c() {
	[[ $MDS1_VERSION -lt $(version_code 2.4.1) ]] &&
		skip "Need MDS version at least 2.4.1"

	touch $DIR/$tfile.1 $DIR/$tfile.2 $DIR/$tfile.3
	local FID1=$($LFS path2fid $DIR/$tfile.1)
	local FID2=$($LFS path2fid $DIR/$tfile.2)
	local FID3=$($LFS path2fid $DIR/$tfile.3)

	local N=1
	$LFS path2fid $DIR/$tfile.[123] | while read PATHNAME FID; do
		[ "$PATHNAME" = "$DIR/$tfile.$N:" ] ||
			error "path2fid pathname $PATHNAME != $DIR/$tfile.$N:"
		local want=FID$N
		[ "$FID" = "${!want}" ] ||
			error "path2fid $PATHNAME FID $FID != FID$N ${!want}"
		N=$((N + 1))
	done

	$LFS fid2path $MOUNT "$FID1" "$FID2" "$FID3" | while read PATHNAME;
	do
		[ "$PATHNAME" = "$DIR/$tfile.$N" ] ||
			error "fid2path pathname $PATHNAME != $DIR/$tfile.$N:"
		N=$((N + 1))
	done
}
run_test 154c "lfs path2fid and fid2path multiple arguments"

test_154d() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $MDS1_VERSION -lt $(version_code 2.5.53) ]] &&
		skip "Need MDS version at least 2.5.53"

	if remote_mds; then
		nid=$($LCTL list_nids | sed  "s/\./\\\./g")
	else
		nid="0@lo"
	fi
	local proc_ofile="mdt.*.exports.'$nid'.open_files"
	local fd
	local cmd

	rm -f $DIR/$tfile
	touch $DIR/$tfile

	local fid=$($LFS path2fid $DIR/$tfile)
	# Open the file
	fd=$(free_fd)
	cmd="exec $fd<$DIR/$tfile"
	eval $cmd
	local fid_list=$(do_facet $SINGLEMDS $LCTL get_param $proc_ofile)
	echo "$fid_list" | grep "$fid"
	rc=$?

	cmd="exec $fd>/dev/null"
	eval $cmd
	if [ $rc -ne 0 ]; then
		error "FID $fid not found in open files list $fid_list"
	fi
}
run_test 154d "Verify open file fid"

test_154e()
{
	[[ $MDS1_VERSION -lt $(version_code 2.6.50) ]] &&
		skip "Need MDS version at least 2.6.50"

	if ls -a $MOUNT | grep -q '^\.lustre$'; then
		error ".lustre returned by readdir"
	fi
}
run_test 154e ".lustre is not returned by readdir"

test_154f() {
	[ -n "$FILESET" ] && skip "SKIP due to FILESET set"

	# create parent directory on a single MDT to avoid cross-MDT hardlinks
	test_mkdir -p -c1 $DIR/$tdir/d
	# test dirs inherit from its stripe
	mkdir -p $DIR/$tdir/d/foo1 || error "mkdir error"
	mkdir -p $DIR/$tdir/d/foo2 || error "mkdir error"
	cp /etc/hosts $DIR/$tdir/d/foo1/$tfile
	ln $DIR/$tdir/d/foo1/$tfile $DIR/$tdir/d/foo2/link
	touch $DIR/f

	# get fid of parents
	local FID0=$($LFS path2fid $DIR/$tdir/d)
	local FID1=$($LFS path2fid $DIR/$tdir/d/foo1)
	local FID2=$($LFS path2fid $DIR/$tdir/d/foo2)
	local FID3=$($LFS path2fid $DIR)

	# check that path2fid --parents returns expected <parent_fid>/name
	# 1) test for a directory (single parent)
	local parent=$($LFS path2fid --parents $DIR/$tdir/d/foo1)
	[ "$parent" == "$FID0/foo1" ] ||
		error "expected parent: $FID0/foo1, got: $parent"

	# 2) test for a file with nlink > 1 (multiple parents)
	parent=$($LFS path2fid --parents $DIR/$tdir/d/foo1/$tfile)
	echo "$parent" | grep -F "$FID1/$tfile" ||
		error "$FID1/$tfile not returned in parent list"
	echo "$parent" | grep -F "$FID2/link" ||
		error "$FID2/link not returned in parent list"

	# 3) get parent by fid
	local file_fid=$($LFS path2fid $DIR/$tdir/d/foo1/$tfile)
	parent=$($LFS path2fid --parents $MOUNT/.lustre/fid/$file_fid)
	echo "$parent" | grep -F "$FID1/$tfile" ||
		error "$FID1/$tfile not returned in parent list (by fid)"
	echo "$parent" | grep -F "$FID2/link" ||
		error "$FID2/link not returned in parent list (by fid)"

	# 4) test for entry in root directory
	parent=$($LFS path2fid --parents $DIR/f)
	echo "$parent" | grep -F "$FID3/f" ||
		error "$FID3/f not returned in parent list"

	# 5) test it on root directory
	[ -z "$($LFS path2fid --parents $MOUNT 2>/dev/null)" ] ||
		error "$MOUNT should not have parents"

	# enable xattr caching and check that linkea is correctly updated
	local save="$TMP/$TESTSUITE-$TESTNAME.parameters"
	save_lustre_params client "llite.*.xattr_cache" > $save
	lctl set_param llite.*.xattr_cache 1

	# 6.1) linkea update on rename
	mv $DIR/$tdir/d/foo1/$tfile $DIR/$tdir/d/foo2/$tfile.moved

	# get parents by fid
	parent=$($LFS path2fid --parents $MOUNT/.lustre/fid/$file_fid)
	# foo1 should no longer be returned in parent list
	echo "$parent" | grep -F "$FID1" &&
		error "$FID1 should no longer be in parent list"
	# the new path should appear
	echo "$parent" | grep -F "$FID2/$tfile.moved" ||
		error "$FID2/$tfile.moved is not in parent list"

	# 6.2) linkea update on unlink
	rm -f $DIR/$tdir/d/foo2/link
	parent=$($LFS path2fid --parents $MOUNT/.lustre/fid/$file_fid)
	# foo2/link should no longer be returned in parent list
	echo "$parent" | grep -F "$FID2/link" &&
		error "$FID2/link should no longer be in parent list"
	true

	rm -f $DIR/f
	restore_lustre_params < $save
	rm -f $save
}
run_test 154f "get parent fids by reading link ea"

test_154g()
{
	[ -n "$FILESET" ] && skip "SKIP due to FILESET set"
	[[ $MDS1_VERSION -ge $(version_code 2.6.92) &&
	   $CLIENT_VERSION -gt $(version_code 2.6.99) ]] ||
		skip "Need MDS version at least 2.6.92"

	mkdir -p $DIR/$tdir
	llapi_fid_test -d $DIR/$tdir
}
run_test 154g "various llapi FID tests"

test_155_small_load() {
    local temp=$TMP/$tfile
    local file=$DIR/$tfile

    dd if=/dev/urandom of=$temp bs=6096 count=1 || \
        error "dd of=$temp bs=6096 count=1 failed"
    cp $temp $file
    cancel_lru_locks $OSC
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
	remote_ost_nodsh && skip "remote OST with nodsh"

	local temp=$TMP/$tfile
	local file=$DIR/$tfile

	free_min_max
	local cache_size=$(do_facet ost$((MAXI+1)) \
		"awk '/cache/ {sum+=\\\$4} END {print sum}' /proc/cpuinfo")
	local large_file_size=$((cache_size * 2))

	echo "OSS cache size: $cache_size KB"
	echo "Large file size: $large_file_size KB"

	[ $MAXV -le $large_file_size ] &&
		skip_env "max available OST size needs > $large_file_size KB"

	$SETSTRIPE $file -c 1 -i $MAXI || error "$SETSTRIPE $file failed"

	dd if=/dev/urandom of=$temp bs=$large_file_size count=1k ||
		error "dd of=$temp bs=$large_file_size count=1k failed"
	cp $temp $file
	ls -lh $temp $file
	cancel_lru_locks osc
	cmp $temp $file || error "$temp $file differ"

	rm -f $temp $file
	true
}

save_writethrough() {
	local facets=$(get_facets OST)

	save_lustre_params $facets "osd-*.*.writethrough_cache_enable" > $1
}

test_155a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p

	set_cache read on
	set_cache writethrough on
	test_155_small_load
	restore_lustre_params < $p
	rm -f $p
}
run_test 155a "Verify small file correctness: read cache:on write_cache:on"

test_155b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p

	set_cache read on
	set_cache writethrough off
	test_155_small_load
	restore_lustre_params < $p
	rm -f $p
}
run_test 155b "Verify small file correctness: read cache:on write_cache:off"

test_155c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p

	set_cache read off
	set_cache writethrough on
	test_155_small_load
	restore_lustre_params < $p
	rm -f $p
}
run_test 155c "Verify small file correctness: read cache:off write_cache:on"

test_155d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p

	set_cache read off
	set_cache writethrough off
	test_155_small_load
	restore_lustre_params < $p
	rm -f $p
}
run_test 155d "Verify small file correctness: read cache:off write_cache:off"

test_155e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p

	set_cache read on
	set_cache writethrough on
	test_155_big_load
	restore_lustre_params < $p
	rm -f $p
}
run_test 155e "Verify big file correctness: read cache:on write_cache:on"

test_155f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p

	set_cache read on
	set_cache writethrough off
	test_155_big_load
	restore_lustre_params < $p
	rm -f $p
}
run_test 155f "Verify big file correctness: read cache:on write_cache:off"

test_155g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p

	set_cache read off
	set_cache writethrough on
	test_155_big_load
	restore_lustre_params < $p
	rm -f $p
}
run_test 155g "Verify big file correctness: read cache:off write_cache:on"

test_155h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p

	set_cache read off
	set_cache writethrough off
	test_155_big_load
	restore_lustre_params < $p
	rm -f $p
}
run_test 155h "Verify big file correctness: read cache:off write_cache:off"

test_156() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ $OST1_VERSION -lt $(version_code 2.6.93) ] &&
		skip "stats not implemented on old servers"
	[ "$ost1_FSTYPE" = "zfs" ] &&
		skip "LU-1956/LU-2261: stats not implemented on OSD ZFS"

	local CPAGES=3
	local BEFORE
	local AFTER
	local file="$DIR/$tfile"
	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_writethrough $p
	roc_hit_init

	log "Turn on read and write cache"
	set_cache read on
	set_cache writethrough on

	log "Write data and read it back."
	log "Read should be satisfied from the cache."
	dd if=/dev/urandom of=$file bs=4k count=$CPAGES || error "dd failed"
	BEFORE=$(roc_hit)
	cancel_lru_locks osc
	cat $file >/dev/null
	AFTER=$(roc_hit)
	if ! let "AFTER - BEFORE == CPAGES"; then
		error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
	else
		log "cache hits:: before: $BEFORE, after: $AFTER"
	fi

	log "Read again; it should be satisfied from the cache."
	BEFORE=$AFTER
	cancel_lru_locks osc
	cat $file >/dev/null
	AFTER=$(roc_hit)
	if ! let "AFTER - BEFORE == CPAGES"; then
		error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
	else
		log "cache hits:: before: $BEFORE, after: $AFTER"
	fi

	log "Turn off the read cache and turn on the write cache"
	set_cache read off
	set_cache writethrough on

	log "Read again; it should be satisfied from the cache."
	BEFORE=$(roc_hit)
	cancel_lru_locks osc
	cat $file >/dev/null
	AFTER=$(roc_hit)
	if ! let "AFTER - BEFORE == CPAGES"; then
		error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
	else
		log "cache hits:: before: $BEFORE, after: $AFTER"
	fi

	log "Read again; it should not be satisfied from the cache."
	BEFORE=$AFTER
	cancel_lru_locks osc
	cat $file >/dev/null
	AFTER=$(roc_hit)
	if ! let "AFTER - BEFORE == 0"; then
		error "IN CACHE: before: $BEFORE, after: $AFTER"
	else
		log "cache hits:: before: $BEFORE, after: $AFTER"
	fi

	log "Write data and read it back."
	log "Read should be satisfied from the cache."
	dd if=/dev/urandom of=$file bs=4k count=$CPAGES || error "dd failed"
	BEFORE=$(roc_hit)
	cancel_lru_locks osc
	cat $file >/dev/null
	AFTER=$(roc_hit)
	if ! let "AFTER - BEFORE == CPAGES"; then
		error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
	else
		log "cache hits:: before: $BEFORE, after: $AFTER"
	fi

	log "Read again; it should not be satisfied from the cache."
	BEFORE=$AFTER
	cancel_lru_locks osc
	cat $file >/dev/null
	AFTER=$(roc_hit)
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
	BEFORE=$(roc_hit)
	cat $file >/dev/null
	AFTER=$(roc_hit)
	if ! let "AFTER - BEFORE == 0"; then
		error_ignore bz20762 "IN CACHE: before: $BEFORE, after: $AFTER"
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
	BEFORE=$(roc_hit)
	cancel_lru_locks osc
	cat $file >/dev/null
	AFTER=$(roc_hit)
	if ! let "AFTER - BEFORE == 0"; then
		error_ignore bz20762 "IN CACHE: before: $BEFORE, after: $AFTER"
	else
		log "cache hits:: before: $BEFORE, after: $AFTER"
	fi

	log "Read again; it should be satisfied from the cache."
	BEFORE=$(roc_hit)
	cancel_lru_locks osc
	cat $file >/dev/null
	AFTER=$(roc_hit)
	if ! let "AFTER - BEFORE == CPAGES"; then
		error "NOT IN CACHE: before: $BEFORE, after: $AFTER"
	else
		log "cache hits:: before: $BEFORE, after: $AFTER"
	fi

	restore_lustre_params < $p
	rm -f $p $file
}
run_test 156 "Verification of tunables"

test_160a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDS1_VERSION -ge $(version_code 2.2.0) ] ||
		skip "Need MDS version at least 2.2.0"

	changelog_register || error "changelog_register failed"
	local cl_user="${CL_USERS[$SINGLEMDS]%% *}"
	changelog_users $SINGLEMDS | grep -q $cl_user ||
		error "User $cl_user not found in changelog_users"

	# change something
	test_mkdir -p $DIR/$tdir/pics/2008/zachy
	changelog_clear 0 || error "changelog_clear failed"
	touch $DIR/$tdir/pics/2008/zachy/$tfile			# open 1
	cp /etc/hosts $DIR/$tdir/pics/2008/zachy/pic1.jpg	# open 2
	mv $DIR/$tdir/pics/2008/zachy $DIR/$tdir/pics/zach
	ln $DIR/$tdir/pics/zach/pic1.jpg $DIR/$tdir/pics/2008/portland.jpg
	ln -s $DIR/$tdir/pics/2008/portland.jpg $DIR/$tdir/pics/desktop.jpg
	rm $DIR/$tdir/pics/desktop.jpg

	changelog_dump | tail -10

	echo "verifying changelog mask"
	changelog_chmask "-MKDIR"
	changelog_chmask "-CLOSE"

	test_mkdir -p $DIR/$tdir/pics/zach/sofia		# not logged
	echo "zzzzzz" > $DIR/$tdir/pics/zach/file		# not logged

	changelog_chmask "+MKDIR"
	changelog_chmask "+CLOSE"

	test_mkdir -p $DIR/$tdir/pics/2008/sofia		# mkdir 1
	echo "zzzzzz" > $DIR/$tdir/pics/zach/file		# open 3

	changelog_dump | tail -10
	MKDIRS=$(changelog_dump | grep -c "MKDIR")
	CLOSES=$(changelog_dump | grep -c "CLOSE")
	[ $MKDIRS -eq 1 ] || error "MKDIR changelog mask count $MKDIRS != 1"
	[ $CLOSES -eq 3 ] || error "CLOSE changelog mask count $CLOSES != 3"

	# verify contents
	echo "verifying target fid"
	local fidc=$(changelog_extract_field "CREAT" "$tfile" "t=")
	local fidf=$($LFS path2fid $DIR/$tdir/pics/zach/$tfile)
	[ "$fidc" == "$fidf" ] ||
		error "changelog '$tfile' fid $fidc != file fid $fidf"
	echo "verifying parent fid"
	# The FID returned from the Changelog may be the directory shard on
	# a different MDT, and not the FID returned by path2fid on the parent.
	# Instead of comparing FIDs, verify that fid2path(fidp) is correct,
	# since this is what will matter when recreating this file in the tree.
	local fidp=$(changelog_extract_field "CREAT" "$tfile" "p=")
	local pathp=$($LFS fid2path $MOUNT "$fidp")
	[ "${pathp%/}" == "$DIR/$tdir/pics/zach" ] ||
		error "changelog fid2path($fidc) $pathp != $DIR/$tdir/pics/zach"

	echo "getting records for $cl_user"
	changelog_users $SINGLEMDS
	local user_rec1=$(changelog_user_rec $SINGLEMDS $cl_user)
	local nclr=3
	__changelog_clear $SINGLEMDS $cl_user +$nclr ||
		error "changelog_clear failed"
	local user_rec2=$(changelog_user_rec $SINGLEMDS $cl_user)
	echo "verifying user clear: $user_rec1 + $nclr == $user_rec2"
	[ $user_rec2 == $((user_rec1 + nclr)) ] ||
		error "user index expect $user_rec1 + $nclr != $user_rec2"

	local min0_rec=$(changelog_users $SINGLEMDS |
		awk 'min == "" || $2 < min { min = $2 }; END { print min }')
	local first_rec=$($LFS changelog $(facet_svc $SINGLEMDS) |
			  awk '{ print $1; exit; }')

	changelog_dump | tail -n 5
	echo "verifying user min purge: $min0_rec + 1 == $first_rec"
	[ $first_rec == $((min0_rec + 1)) ] ||
		error "first index should be $min0_rec + 1 not $first_rec"

	# LU-3446 changelog index reset on MDT restart
	local cur_rec1=$(changelog_users $SINGLEMDS |
			 awk '/^current.index:/ { print $NF }')
	changelog_clear 0 ||
		error "clear all changelog records for $cl_user failed"
	stop $SINGLEMDS || error "Fail to stop $SINGLEMDS"
	start $SINGLEMDS $(mdsdevname ${SINGLEMDS//mds/}) $MDS_MOUNT_OPTS ||
		error "Fail to start $SINGLEMDS"
	local cur_rec2=$(changelog_users $SINGLEMDS |
			 awk '/^current.index:/ { print $NF }')
	echo "verifying index survives MDT restart: $cur_rec1 == $cur_rec2"
	[ $cur_rec1 == $cur_rec2 ] ||
		error "current index should be $cur_rec1 not $cur_rec2"

	echo "verifying users from this test are deregistered"
	changelog_deregister || error "changelog_deregister failed"
	changelog_users $SINGLEMDS | grep -q $cl_user &&
		error "User '$cl_user' still in changelog_users"

	# lctl get_param -n mdd.*.changelog_users
	# current index: 144
	# ID    index (idle seconds)
	# cl3   144 (2)
	if ! changelog_users $SINGLEMDS | grep "^cl"; then
		# this is the normal case where all users were deregistered
		# make sure no new records are added when no users are present
		local last_rec1=$(changelog_users $SINGLEMDS |
				  awk '/^current.index:/ { print $NF }')
		touch $DIR/$tdir/chloe
		local last_rec2=$(changelog_users $SINGLEMDS |
				  awk '/^current.index:/ { print $NF }')
		echo "verify changelogs are off: $last_rec1 == $last_rec2"
		[ $last_rec1 == $last_rec2 ] || error "changelogs not off"
	else
		# any changelog users must be leftovers from a previous test
		changelog_users $SINGLEMDS
		echo "other changelog users; can't verify off"
	fi
}
run_test 160a "changelog sanity"

test_160b() { # LU-3587
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDS1_VERSION -ge $(version_code 2.2.0) ] ||
		skip "Need MDS version at least 2.2.0"

	changelog_register || error "changelog_register failed"
	local cl_user="${CL_USERS[$SINGLEMDS]%% *}"
	changelog_users $SINGLEMDS | grep -q $cl_user ||
		error "User '$cl_user' not found in changelog_users"

	local longname1=$(str_repeat a 255)
	local longname2=$(str_repeat b 255)

	cd $DIR
	echo "creating very long named file"
	touch $longname1 || error "create of '$longname1' failed"
	echo "renaming very long named file"
	mv $longname1 $longname2

	changelog_dump | grep RENME | tail -n 5
	rm -f $longname2
}
run_test 160b "Verify that very long rename doesn't crash in changelog"

test_160c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	[[ $MDS1_VERSION -gt $(version_code 2.5.57) ]] ||
		[[ $MDS1_VERSION -gt $(version_code 2.5.1) &&
		   $MDS1_VERSION -lt $(version_code 2.5.50) ]] ||
		skip "Need MDS version at least 2.5.58 or 2.5.2+"

	local rc=0

	# Registration step
	changelog_register || error "changelog_register failed"

	rm -rf $DIR/$tdir
	mkdir -p $DIR/$tdir
	$MCREATE $DIR/$tdir/foo_160c
	changelog_chmask "-TRUNC"
	$TRUNCATE $DIR/$tdir/foo_160c 200
	changelog_chmask "+TRUNC"
	$TRUNCATE $DIR/$tdir/foo_160c 199
	changelog_dump | tail -n 5
	local truncs=$(changelog_dump | tail -n 5 | grep -c TRUNC)
	[ $truncs -eq 1 ] || error "TRUNC changelog mask count $truncs != 1"
}
run_test 160c "verify that changelog log catch the truncate event"

test_160d() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $MDS1_VERSION -ge $(version_code 2.7.60) ]] ||
		skip "Need MDS version at least 2.7.60"

	# Registration step
	changelog_register || error "changelog_register failed"

	mkdir -p $DIR/$tdir/migrate_dir
	changelog_clear 0 || error "changelog_clear failed"

	$LFS migrate -m 1 $DIR/$tdir/migrate_dir || error "migrate fails"
	changelog_dump | tail -n 5
	local migrates=$(changelog_dump | grep -c "MIGRT")
	[ $migrates -eq 1 ] || error "MIGRATE changelog count $migrates != 1"
}
run_test 160d "verify that changelog log catch the migrate event"

test_160e() {
	remote_mds_nodsh && skip "remote MDS with nodsh"

	# Create a user
	changelog_register || error "changelog_register failed"

	# Delete a future user (expect fail)
	local MDT0=$(facet_svc $SINGLEMDS)
	do_facet $SINGLEMDS $LCTL --device $MDT0 changelog_deregister "cl77"
	local rc=$?

	if [ $rc -eq 0 ]; then
		error "Deleted non-existant user cl77"
	elif [ $rc -ne 2 ]; then
		error "changelog_deregister failed with $rc, expect 2 (ENOENT)"
	fi

	# Clear to a bad index (1 billion should be safe)
	$LFS changelog_clear $MDT0 "${CL_USERS[$SINGLEMDS]%% *}" 1000000000
	rc=$?

	if [ $rc -eq 0 ]; then
		error "Successfully cleared to invalid CL index"
	elif [ $rc -ne 22 ]; then
		error "changelog_clear failed with $rc, expected 22 (EINVAL)"
	fi
}
run_test 160e "changelog negative testing (should return errors)"

test_160f() {
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	[[ $MDS1_VERSION -ge $(version_code 2.10.56) ]] ||
		skip "Need MDS version at least 2.10.56"

	local mdts=$(comma_list $(mdts_nodes))

	# Create a user
	changelog_register || error "first changelog_register failed"
	changelog_register || error "second changelog_register failed"
	local cl_users
	declare -A cl_user1
	declare -A cl_user2
	local user_rec1
	local user_rec2
	local i

	# generate some changelog records to accumulate on each MDT
	test_mkdir -c $MDSCOUNT $DIR/$tdir || error "test_mkdir $tdir failed"
	createmany -m $DIR/$tdir/$tfile $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/$tfile failed"

	# check changelogs have been generated
	local nbcl=$(changelog_dump | wc -l)
	[[ $nbcl -eq 0 ]] && error "no changelogs found"

	for param in "changelog_max_idle_time=10" \
		     "changelog_gc=1" \
		     "changelog_min_gc_interval=2" \
		     "changelog_min_free_cat_entries=3"; do
		local MDT0=$(facet_svc $SINGLEMDS)
		local var="${param%=*}"
		local old=$(do_facet mds1 "$LCTL get_param -n mdd.$MDT0.$var")

		stack_trap "do_nodes $mdts $LCTL set_param mdd.*.$var=$old" EXIT
		do_nodes $mdts $LCTL set_param mdd.*.$param
	done

	# force cl_user2 to be idle (1st part)
	sleep 9

	# simulate changelog catalog almost full
	#define OBD_FAIL_CAT_FREE_RECORDS	0x1313
	do_nodes $mdts $LCTL set_param fail_loc=0x1313 fail_val=3

	for i in $(seq $MDSCOUNT); do
		cl_users=(${CL_USERS[mds$i]})
		cl_user1[mds$i]="${cl_users[0]}"
		cl_user2[mds$i]="${cl_users[1]}"

		[ -n "${cl_user1[mds$i]}" ] ||
			error "mds$i: no user registered"
		[ -n "${cl_user2[mds$i]}" ] ||
			error "mds$i: only ${cl_user2[mds$i]} is registered"

		user_rec1=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec1" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		__changelog_clear mds$i ${cl_user1[mds$i]} +2
		user_rec2=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec2" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		echo "mds$i: verifying user ${cl_user1[mds$i]} clear: " \
		     "$user_rec1 + 2 == $user_rec2"
		[ $((user_rec1 + 2)) == $user_rec2 ] ||
			error "mds$i: user ${cl_user1[mds$i]} index expected " \
			      "$user_rec1 + 2, but is $user_rec2"
		user_rec2=$(changelog_user_rec mds$i ${cl_user2[mds$i]})
		[ -n "$user_rec2" ] ||
			error "mds$i: User ${cl_user2[mds$i]} not registered"
		[ $user_rec1 == $user_rec2 ] ||
			error "mds$i: user ${cl_user2[mds$i]} index expected " \
			      "$user_rec1, but is $user_rec2"
	done

	# force cl_user2 to be idle (2nd part) and to reach
	# changelog_max_idle_time
	sleep 2

	# generate one more changelog to trigger fail_loc
	createmany -m $DIR/$tdir/${tfile}bis $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/${tfile}bis failed"

	# ensure gc thread is done
	for i in $(mdts_nodes); do
		wait_update $i \
			"ps -e -o comm= | grep chlg_gc_thread" "" 20 ||
			error "$i: GC-thread not done"
	done

	local first_rec
	for i in $(seq $MDSCOUNT); do
		# check cl_user1 still registered
		changelog_users mds$i | grep -q "${cl_user1[mds$i]}" ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		# check cl_user2 unregistered
		changelog_users mds$i | grep -q "${cl_user2[mds$i]}" &&
			error "mds$i: User ${cl_user2[mds$i]} still registered"

		# check changelogs are present and starting at $user_rec1 + 1
		user_rec1=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec1" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		first_rec=$($LFS changelog $(facet_svc mds$i) |
			    awk '{ print $1; exit; }')

		echo "mds$i: verifying first index $user_rec1 + 1 == $first_rec"
		[ $((user_rec1 + 1)) == $first_rec ] ||
			error "mds$i: first index should be $user_rec1 + 1, " \
			      "but is $first_rec"
	done
}
run_test 160f "changelog garbage collect (timestamped users)"

test_160g() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $MDS1_VERSION -ge $(version_code 2.10.56) ]] ||
		skip "Need MDS version at least 2.10.56"

	local mdts=$(comma_list $(mdts_nodes))

	#define OBD_FAIL_TIME_IN_CHLOG_USER	0x1314
	do_nodes $mdts $LCTL set_param fail_loc=0x1314

	# Create a user
	changelog_register || error "first changelog_register failed"
	changelog_register || error "second changelog_register failed"
	local cl_users
	declare -A cl_user1
	declare -A cl_user2
	local user_rec1
	local user_rec2
	local i

	# generate some changelog records to accumulate on each MDT
	test_mkdir -c $MDSCOUNT $DIR/$tdir || error "mkdir $tdir failed"
	createmany -m $DIR/$tdir/$tfile $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/$tfile failed"

	# check changelogs have been generated
	local nbcl=$(changelog_dump | wc -l)
	[[ $nbcl -eq 0 ]] && error "no changelogs found"

	# reduce the max_idle_indexes value to make sure we exceed it
	max_ndx=$((nbcl / 2 - 1))

	for param in "changelog_max_idle_indexes=$max_ndx" \
		     "changelog_gc=1" \
		     "changelog_min_gc_interval=2" \
		     "changelog_min_free_cat_entries=3"; do
		local MDT0=$(facet_svc $SINGLEMDS)
		local var="${param%=*}"
		local old=$(do_facet mds1 "$LCTL get_param -n mdd.$MDT0.$var")

		stack_trap "do_nodes $mdts $LCTL set_param mdd.*.$var=$old" EXIT
		do_nodes $mdts $LCTL set_param mdd.*.$param ||
			error "unable to set mdd.*.$param"
	done

	# simulate changelog catalog almost full
	#define OBD_FAIL_CAT_FREE_RECORDS	0x1313
	do_nodes $mdts $LCTL set_param fail_loc=0x1313 fail_val=3

	for i in $(seq $MDSCOUNT); do
		cl_users=(${CL_USERS[mds$i]})
		cl_user1[mds$i]="${cl_users[0]}"
		cl_user2[mds$i]="${cl_users[1]}"

		[ -n "${cl_user1[mds$i]}" ] ||
			error "mds$i: no user registered"
		[ -n "${cl_user2[mds$i]}" ] ||
			error "mds$i: only ${cl_user1[mds$i]} is registered"

		user_rec1=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec1" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		__changelog_clear mds$i ${cl_user1[mds$i]} +2
		user_rec2=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec2" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		echo "mds$i: verifying user ${cl_user1[mds$i]} clear: " \
		     "$user_rec1 + 2 == $user_rec2"
		[ $((user_rec1 + 2)) == $user_rec2 ] ||
			error "mds$i: user ${cl_user1[mds$i]} index expected " \
			      "$user_rec1 + 2, but is $user_rec2"
		user_rec2=$(changelog_user_rec mds$i ${cl_user2[mds$i]})
		[ -n "$user_rec2" ] ||
			error "mds$i: User ${cl_user2[mds$i]} not registered"
		[ $user_rec1 == $user_rec2 ] ||
			error "mds$i: user ${cl_user2[mds$i]} index expected " \
			      "$user_rec1, but is $user_rec2"
	done

	# ensure we are past the previous changelog_min_gc_interval set above
	sleep 2

	# generate one more changelog to trigger fail_loc
	createmany -m $DIR/$tdir/${tfile}bis $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/${tfile}bis failed"

	# ensure gc thread is done
	for i in $(mdts_nodes); do
		wait_update $i \
			"ps -e -o comm= | grep chlg_gc_thread" "" 20 ||
			error "$i: GC-thread not done"
	done

	local first_rec
	for i in $(seq $MDSCOUNT); do
		# check cl_user1 still registered
		changelog_users mds$i | grep -q "${cl_user1[mds$i]}" ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		# check cl_user2 unregistered
		changelog_users mds$i | grep -q "${cl_user2[mds$i]}" &&
			error "mds$i: User ${cl_user2[mds$i]} still registered"

		# check changelogs are present and starting at $user_rec1 + 1
		user_rec1=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec1" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		first_rec=$($LFS changelog $(facet_svc mds$i) |
			    awk '{ print $1; exit; }')

		echo "mds$i: verifying first index $user_rec1 + 1 == $first_rec"
		[ $((user_rec1 + 1)) == $first_rec ] ||
			error "mds$i: first index should be $user_rec1 + 1, " \
			      "but is $first_rec"
	done
}
run_test 160g "changelog garbage collect (old users)"

test_160h() {
	remote_mds_nodsh && skip "remote MDS with nodsh" && return
	[[ $MDS1_VERSION -ge $(version_code 2.10.56) ]] ||
		skip "Need MDS version at least 2.10.56"

	local mdts=$(comma_list $(mdts_nodes))

	# Create a user
	changelog_register || error "first changelog_register failed"
	changelog_register || error "second changelog_register failed"
	local cl_users
	declare -A cl_user1
	declare -A cl_user2
	local user_rec1
	local user_rec2
	local i

	# generate some changelog records to accumulate on each MDT
	test_mkdir -c $MDSCOUNT $DIR/$tdir || error "test_mkdir $tdir failed"
	createmany -m $DIR/$tdir/$tfile $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/$tfile failed"

	# check changelogs have been generated
	local nbcl=$(changelog_dump | wc -l)
	[[ $nbcl -eq 0 ]] && error "no changelogs found"

	for param in "changelog_max_idle_time=10" \
		     "changelog_gc=1" \
		     "changelog_min_gc_interval=2"; do
		local MDT0=$(facet_svc $SINGLEMDS)
		local var="${param%=*}"
		local old=$(do_facet mds1 "$LCTL get_param -n mdd.$MDT0.$var")

		stack_trap "do_nodes $mdts $LCTL set_param mdd.*.$var=$old" EXIT
		do_nodes $mdts $LCTL set_param mdd.*.$param
	done

	# force cl_user2 to be idle (1st part)
	sleep 9

	for i in $(seq $MDSCOUNT); do
		cl_users=(${CL_USERS[mds$i]})
		cl_user1[mds$i]="${cl_users[0]}"
		cl_user2[mds$i]="${cl_users[1]}"

		[ -n "${cl_user1[mds$i]}" ] ||
			error "mds$i: no user registered"
		[ -n "${cl_user2[mds$i]}" ] ||
			error "mds$i: only ${cl_user2[mds$i]} is registered"

		user_rec1=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec1" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		__changelog_clear mds$i ${cl_user1[mds$i]} +2
		user_rec2=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec2" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		echo "mds$i: verifying user ${cl_user1[mds$i]} clear: " \
		     "$user_rec1 + 2 == $user_rec2"
		[ $((user_rec1 + 2)) == $user_rec2 ] ||
			error "mds$i: user ${cl_user1[mds$i]} index expected " \
			      "$user_rec1 + 2, but is $user_rec2"
		user_rec2=$(changelog_user_rec mds$i ${cl_user2[mds$i]})
		[ -n "$user_rec2" ] ||
			error "mds$i: User ${cl_user2[mds$i]} not registered"
		[ $user_rec1 == $user_rec2 ] ||
			error "mds$i: user ${cl_user2[mds$i]} index expected " \
			      "$user_rec1, but is $user_rec2"
	done

	# force cl_user2 to be idle (2nd part) and to reach
	# changelog_max_idle_time
	sleep 2

	# force each GC-thread start and block then
	# one per MDT/MDD, set fail_val accordingly
	#define OBD_FAIL_FORCE_GC_THREAD 0x1316
	do_nodes $mdts $LCTL set_param fail_loc=0x1316

	# generate more changelogs to trigger fail_loc
	createmany -m $DIR/$tdir/${tfile}bis $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/${tfile}bis failed"

	# stop MDT to stop GC-thread, should be done in back-ground as it will
	# block waiting for the thread to be released and exit
	declare -A stop_pids
	for i in $(seq $MDSCOUNT); do
		stop mds$i &
		stop_pids[mds$i]=$!
	done

	for i in $(mdts_nodes); do
		local facet
		local nb=0
		local facets=$(facets_up_on_host $i)

		for facet in ${facets//,/ }; do
			if [[ $facet == mds* ]]; then
				nb=$((nb + 1))
			fi
		done
		# ensure each MDS's gc threads are still present and all in "R"
		# state (OBD_FAIL_FORCE_GC_THREAD effect!)
		[[ $(do_node $i pgrep chlg_gc_thread | wc -l) -eq $nb ]] ||
			error "$i: expected $nb GC-thread"
		wait_update $i \
			"ps -C chlg_gc_thread -o state --no-headers | uniq" \
			"R" 20 ||
			error "$i: GC-thread not found in R-state"
		# check umounts of each MDT on MDS have reached kthread_stop()
		[[ $(do_node $i pgrep umount | wc -l) -eq $nb ]] ||
			error "$i: expected $nb umount"
		wait_update $i \
			"ps -C umount -o state --no-headers | uniq" "D" 20 ||
			error "$i: umount not found in D-state"
	done

	# release all GC-threads
	do_nodes $mdts $LCTL set_param fail_loc=0

	# wait for MDT stop to complete
	for i in $(seq $MDSCOUNT); do
		wait ${stop_pids[mds$i]} || error "mds$i: stop failed"
	done

	# XXX
	# may try to check if any orphan changelog records are present
	# via ldiskfs/zfs and llog_reader...

	# re-start/mount MDTs
	for i in $(seq $MDSCOUNT); do
		start mds$i $(mdsdevname $i) $MDS_MOUNT_OPTS ||
			error "Fail to start mds$i"
	done

	local first_rec
	for i in $(seq $MDSCOUNT); do
		# check cl_user1 still registered
		changelog_users mds$i | grep -q "${cl_user1[mds$i]}" ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		# check cl_user2 unregistered
		changelog_users mds$i | grep -q "${cl_user2[mds$i]}" &&
			error "mds$i: User ${cl_user2[mds$i]} still registered"

		# check changelogs are present and starting at $user_rec1 + 1
		user_rec1=$(changelog_user_rec mds$i ${cl_user1[mds$i]})
		[ -n "$user_rec1" ] ||
			error "mds$i: User ${cl_user1[mds$i]} not registered"
		first_rec=$($LFS changelog $(facet_svc mds$i) |
			    awk '{ print $1; exit; }')

		echo "mds$i: verifying first index $user_rec1 + 1 == $first_rec"
		[ $((user_rec1 + 1)) == $first_rec ] ||
			error "mds$i: first index should be $user_rec1 + 1, " \
			      "but is $first_rec"
	done
}
run_test 160h "changelog gc thread stop upon umount, orphan records delete " \
	      "during mount"

test_160i() {

	local mdts=$(comma_list $(mdts_nodes))

	changelog_register || error "first changelog_register failed"

	# generate some changelog records to accumulate on each MDT
	test_mkdir -c $MDSCOUNT $DIR/$tdir || error "mkdir $tdir failed"
	createmany -m $DIR/$tdir/$tfile $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/$tfile failed"

	# check changelogs have been generated
	local nbcl=$(changelog_dump | wc -l)
	[[ $nbcl -eq 0 ]] && error "no changelogs found"

	# simulate race between register and unregister
	# XXX as fail_loc is set per-MDS, with DNE configs the race
	# simulation will only occur for one MDT per MDS and for the
	# others the normal race scenario will take place
	#define CFS_FAIL_CHLOG_USER_REG_UNREG_RACE          0x1315
	do_nodes $mdts $LCTL set_param fail_loc=0x10001315
	do_nodes $mdts $LCTL set_param fail_val=1

	# unregister 1st user
	changelog_deregister &
	local pid1=$!
	# wait some time for deregister work to reach race rdv
	sleep 2
	# register 2nd user
	changelog_register || error "2nd user register failed"

	wait $pid1 || error "1st user deregister failed"

	local i
	local last_rec
	declare -A LAST_REC
	for i in $(seq $MDSCOUNT); do
		if changelog_users mds$i | grep "^cl"; then
			# make sure new records are added with one user present
			LAST_REC[mds$i]=$(changelog_users $SINGLEMDS |
					  awk '/^current.index:/ { print $NF }')
		else
			error "mds$i has no user registered"
		fi
	done

	# generate more changelog records to accumulate on each MDT
	createmany -m $DIR/$tdir/${tfile}bis $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/${tfile}bis failed"

	for i in $(seq $MDSCOUNT); do
		last_rec=$(changelog_users $SINGLEMDS |
			   awk '/^current.index:/ { print $NF }')
		echo "verify changelogs are on: $last_rec != ${LAST_REC[mds$i]}"
		[ $last_rec != ${LAST_REC[mds$i]} ] ||
			error "changelogs are off on mds$i"
	done
}
run_test 160i "changelog user register/unregister race"

test_160k() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	mkdir -p $DIR/$tdir/1/1

	changelog_register || error "changelog_register failed"
	local cl_user="${CL_USERS[$SINGLEMDS]%% *}"

	changelog_users $SINGLEMDS | grep -q $cl_user ||
		error "User '$cl_user' not found in changelog_users"
#define OBD_FAIL_MDS_CHANGELOG_REORDER 0x15d
	do_facet mds1 $LCTL set_param fail_loc=0x8000015d fail_val=3
	rmdir $DIR/$tdir/1/1 & sleep 1
	mkdir $DIR/$tdir/2
	touch $DIR/$tdir/2/2
	rm -rf $DIR/$tdir/2

	wait
	sleep 4

	changelog_dump | grep rmdir || error "rmdir not recorded"

	rm -rf $DIR/$tdir
	changelog_deregister
}
run_test 160k "Verify that changelog records are not lost"

test_160j() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $MDS1_VERSION -lt $(version_code 2.12.56) ]] &&
		skip "Need MDS version at least 2.12.56"

	mount_client $MOUNT2 || error "mount_client on $MOUNT2 failed"

	changelog_register || error "first changelog_register failed"

	# generate some changelog
	test_mkdir -c $MDSCOUNT $DIR/$tdir || error "mkdir $tdir failed"
	createmany -m $DIR/$tdir/${tfile}bis $((MDSCOUNT * 2)) ||
		error "create $DIR/$tdir/${tfile}bis failed"

	# open the changelog device
	exec 3>/dev/changelog-$FSNAME-MDT0000
	exec 4</dev/changelog-$FSNAME-MDT0000

	# umount the first lustre mount
	umount $MOUNT

	# read changelog
	cat <&4 >/dev/null || error "read changelog failed"

	# clear changelog
	local cl_user="${CL_USERS[$SINGLEMDS]%% *}"
	changelog_users $SINGLEMDS | grep -q $cl_user ||
		error "User $cl_user not found in changelog_users"

	printf 'clear:'$cl_user':0' >&3

	# close
	exec 3>&-
	exec 4<&-

	# cleanup
	changelog_deregister || error "changelog_deregister failed"

	umount $MOUNT2
	mount_client $MOUNT || error "mount_client on $MOUNT failed"
}
run_test 160j "client can be umounted  while its chanangelog is being used"

test_161a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	test_mkdir -c1 $DIR/$tdir
	cp /etc/hosts $DIR/$tdir/$tfile
	test_mkdir -c1 $DIR/$tdir/foo1
	test_mkdir -c1 $DIR/$tdir/foo2
	ln $DIR/$tdir/$tfile $DIR/$tdir/foo1/sofia
	ln $DIR/$tdir/$tfile $DIR/$tdir/foo2/zachary
	ln $DIR/$tdir/$tfile $DIR/$tdir/foo1/luna
	ln $DIR/$tdir/$tfile $DIR/$tdir/foo2/thor
	local FID=$($LFS path2fid $DIR/$tdir/$tfile | tr -d '[]')
	if [ "$($LFS fid2path $DIR $FID | wc -l)" != "5" ]; then
		$LFS fid2path $DIR $FID
		error "bad link ea"
	fi
	# middle
	rm $DIR/$tdir/foo2/zachary
	# last
	rm $DIR/$tdir/foo2/thor
	# first
	rm $DIR/$tdir/$tfile
	# rename
	mv $DIR/$tdir/foo1/sofia $DIR/$tdir/foo2/maggie
	[ "$($LFS fid2path $FSNAME --link 1 $FID)" != "$tdir/foo2/maggie" ] &&
		{ $LFS fid2path $DIR $FID; error "bad link rename"; }
	rm $DIR/$tdir/foo2/maggie

	# overflow the EA
	local longname=$tfile.avg_len_is_thirty_two_
	stack_trap "unlinkmany $DIR/$tdir/foo2/$longname 1000 || \
		error_noexit 'failed to unlink many hardlinks'" EXIT
	createmany -l$DIR/$tdir/foo1/luna $DIR/$tdir/foo2/$longname 1000 ||
		error "failed to hardlink many files"
	links=$($LFS fid2path $DIR $FID | wc -l)
	echo -n "${links}/1000 links in link EA"
	[[ $links -gt 60 ]] || error "expected at least 60 links in link EA"
}
run_test 161a "link ea sanity"

test_161b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "skipping remote directory test"

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
		error "bad link ea"
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
		error "bad link rename"
	fi
	rm $remote_dir/foo2/maggie

	# overflow the EA
	local longname=filename_avg_len_is_thirty_two_
	createmany -l$remote_dir/foo1/luna $remote_dir/foo2/$longname 1000 ||
		error "failed to hardlink many files"
	links=$($LFS fid2path $DIR $FID | wc -l)
	echo -n "${links}/1000 links in link EA"
	[[ ${links} -gt 60 ]] ||
		error "expected at least 60 links in link EA"
	unlinkmany $remote_dir/foo2/$longname 1000 ||
	error "failed to unlink many hardlinks"
}
run_test 161b "link ea sanity under remote directory"

test_161c() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $MDS1_VERSION -lt $(version_code 2.1.5) ]] &&
		skip "Need MDS version at least 2.1.5"

	# define CLF_RENAME_LAST 0x0001
	# rename overwrite a target having nlink = 1 (changelog flag 0x1)
	changelog_register || error "changelog_register failed"

	rm -rf $DIR/$tdir
	test_mkdir -i $((MDSCOUNT - 1)) $DIR/$tdir
	touch $DIR/$tdir/foo_161c
	touch $DIR/$tdir/bar_161c
	mv -f $DIR/$tdir/foo_161c $DIR/$tdir/bar_161c
	changelog_dump | grep RENME | tail -n 5
	local flags=$(changelog_dump | grep "RENME.*bar_161c" | cut -f5 -d' ')
	changelog_clear 0 || error "changelog_clear failed"
	if [ x$flags != "x0x1" ]; then
		error "flag $flags is not 0x1"
	fi

	echo "rename overwrite target with nlink = 1, changelog flags=$flags"
	# rename overwrite a target having nlink > 1 (changelog flag 0x0)
	touch $DIR/$tdir/foo_161c
	touch $DIR/$tdir/bar_161c
	ln $DIR/$tdir/bar_161c $DIR/$tdir/foobar_161c
	mv -f $DIR/$tdir/foo_161c $DIR/$tdir/bar_161c
	changelog_dump | grep RENME | tail -n 5
	flags=$(changelog_dump | grep "RENME.*bar_161c" | cut -f5 -d' ')
	changelog_clear 0 || error "changelog_clear failed"
	if [ x$flags != "x0x0" ]; then
		error "flag $flags is not 0x0"
	fi
	echo "rename overwrite a target having nlink > 1," \
		"changelog record has flags of $flags"

	# rename doesn't overwrite a target (changelog flag 0x0)
	touch $DIR/$tdir/foo_161c
	mv -f $DIR/$tdir/foo_161c $DIR/$tdir/foo2_161c
	changelog_dump | grep RENME | tail -n 5
	flags=$(changelog_dump | grep RENME | tail -1 | cut -f5 -d' ')
	changelog_clear 0 || error "changelog_clear failed"
	if [ x$flags != "x0x0" ]; then
		error "flag $flags is not 0x0"
	fi
	echo "rename doesn't overwrite a target," \
		"changelog record has flags of $flags"

	# define CLF_UNLINK_LAST 0x0001
	# unlink a file having nlink = 1 (changelog flag 0x1)
	rm -f $DIR/$tdir/foo2_161c
	changelog_dump | grep UNLNK | tail -n 5
	flags=$(changelog_dump | grep UNLNK | tail -1 | cut -f5 -d' ')
	changelog_clear 0 || error "changelog_clear failed"
	if [ x$flags != "x0x1" ]; then
		error "flag $flags is not 0x1"
	fi
	echo "unlink a file having nlink = 1," \
		"changelog record has flags of $flags"

	# unlink a file having nlink > 1 (changelog flag 0x0)
	ln -f $DIR/$tdir/bar_161c $DIR/$tdir/foobar_161c
	rm -f $DIR/$tdir/foobar_161c
	changelog_dump | grep UNLNK | tail -n 5
	flags=$(changelog_dump | grep UNLNK | tail -1 | cut -f5 -d' ')
	changelog_clear 0 || error "changelog_clear failed"
	if [ x$flags != "x0x0" ]; then
		error "flag $flags is not 0x0"
	fi
	echo "unlink a file having nlink > 1, changelog record flags '$flags'"
}
run_test 161c "check CL_RENME[UNLINK] changelog record flags"

test_161d() {
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local pid
	local fid

	changelog_register || error "changelog_register failed"

	# work in a standalone dir to avoid locking on $DIR/$MOUNT to
	# interfer with $MOUNT/.lustre/fid/ access
	mkdir $DIR/$tdir
	[[ $? -eq 0 ]] || error "mkdir failed"

	#define OBD_FAIL_LLITE_CREATE_NODE_PAUSE 0x140c | OBD_FAIL_ONCE
	$LCTL set_param fail_loc=0x8000140c
	# 5s pause
	$LCTL set_param fail_val=5

	# create file
	echo foofoo > $DIR/$tdir/$tfile &
	pid=$!

	# wait for create to be delayed
	sleep 2

	ps -p $pid
	[[ $? -eq 0 ]] || error "create should be blocked"

	local tempfile=$(mktemp)
	fid=$(changelog_extract_field "CREAT" "$tfile" "t=")
	cat $MOUNT/.lustre/fid/$fid 2>/dev/null >$tempfile || error "cat failed"
	# some delay may occur during ChangeLog publishing and file read just
	# above, that could allow file write to happen finally
	[[ -s $tempfile ]] && echo "file should be empty"

	$LCTL set_param fail_loc=0

	wait $pid
	[[ $? -eq 0 ]] || error "create failed"
}
run_test 161d "create with concurrent .lustre/fid access"

check_path() {
	local expected="$1"
	shift
	local fid="$2"

	local path
	path=$($LFS fid2path "$@")
	local rc=$?

	if [ $rc -ne 0 ]; then
		error "path looked up of '$expected' failed: rc=$rc"
	elif [ "$path" != "$expected" ]; then
		error "path looked up '$path' instead of '$expected'"
	else
		echo "FID '$fid' resolves to path '$path' as expected"
	fi
}

test_162a() { # was test_162
	test_mkdir -p -c1 $DIR/$tdir/d2
	touch $DIR/$tdir/d2/$tfile
	touch $DIR/$tdir/d2/x1
	touch $DIR/$tdir/d2/x2
	test_mkdir -p -c1 $DIR/$tdir/d2/a/b/c
	test_mkdir -p -c1 $DIR/$tdir/d2/p/q/r
	# regular file
	local fid=$($LFS path2fid $DIR/$tdir/d2/$tfile | tr -d '[]')
	check_path "$tdir/d2/$tfile" $FSNAME "$fid" --link 0

	# softlink
	ln -s $DIR/$tdir/d2/$tfile $DIR/$tdir/d2/p/q/r/slink
	fid=$($LFS path2fid $DIR/$tdir/d2/p/q/r/slink | tr -d '[]')
	check_path "$tdir/d2/p/q/r/slink" $FSNAME "$fid" --link 0

	# softlink to wrong file
	ln -s /this/is/garbage $DIR/$tdir/d2/p/q/r/slink.wrong
	fid=$($LFS path2fid $DIR/$tdir/d2/p/q/r/slink.wrong | tr -d '[]')
	check_path "$tdir/d2/p/q/r/slink.wrong" $FSNAME "$fid" --link 0

	# hardlink
	ln $DIR/$tdir/d2/$tfile $DIR/$tdir/d2/p/q/r/hlink
	mv $DIR/$tdir/d2/$tfile $DIR/$tdir/d2/a/b/c/new_file
	fid=$($LFS path2fid $DIR/$tdir/d2/a/b/c/new_file | tr -d '[]')
	# fid2path dir/fsname should both work
	check_path "$tdir/d2/a/b/c/new_file" $FSNAME "$fid" --link 1
	check_path "$DIR/$tdir/d2/p/q/r/hlink" $DIR "$fid" --link 0

	# hardlink count: check that there are 2 links
	local nlinks=$($LFS fid2path $DIR "$fid" | wc -l)
	[ $nlinks -eq 2 ] || error "expect 2 links, found $nlinks"

	# hardlink indexing: remove the first link
	rm $DIR/$tdir/d2/p/q/r/hlink
	check_path "$tdir/d2/a/b/c/new_file" $FSNAME $fid --link 0
}
run_test 162a "path lookup sanity"

test_162b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	mkdir $DIR/$tdir
	$LFS setdirstripe -i0 -c$MDSCOUNT -H all_char $DIR/$tdir/striped_dir ||
				error "create striped dir failed"

	local FID=$($LFS getdirstripe $DIR/$tdir/striped_dir |
					tail -n 1 | awk '{print $2}')
	stat $MOUNT/.lustre/fid/$FID && error "sub_stripe can be accessed"

	touch $DIR/$tdir/striped_dir/f{0..4} || error "touch f0..4 failed"
	mkdir $DIR/$tdir/striped_dir/d{0..4} || error "mkdir d0..4 failed"

	# regular file
	for ((i=0;i<5;i++)); do
		FID=$($LFS path2fid $DIR/$tdir/striped_dir/f$i | tr -d '[]') ||
			error "get fid for f$i failed"
		check_path "$tdir/striped_dir/f$i" $FSNAME $FID --link 0

		FID=$($LFS path2fid $DIR/$tdir/striped_dir/d$i | tr -d '[]') ||
			error "get fid for d$i failed"
		check_path "$tdir/striped_dir/d$i" $FSNAME $FID --link 0
	done

	return 0
}
run_test 162b "striped directory path lookup sanity"

# LU-4239: Verify fid2path works with paths 100 or more directories deep
test_162c() {
	[[ $MDS1_VERSION -lt $(version_code 2.7.51) ]] &&
		skip "Need MDS version at least 2.7.51"

	local lpath=$tdir.local
	local rpath=$tdir.remote

	test_mkdir $DIR/$lpath
	test_mkdir $DIR/$rpath

	for ((i = 0; i <= 101; i++)); do
		lpath="$lpath/$i"
		mkdir $DIR/$lpath
		FID=$($LFS path2fid $DIR/$lpath | tr -d '[]') ||
			error "get fid for local directory $DIR/$lpath failed"
		check_path "$DIR/$lpath" $MOUNT $FID --link 0

		rpath="$rpath/$i"
		test_mkdir $DIR/$rpath
		FID=$($LFS path2fid $DIR/$rpath | tr -d '[]') ||
			error "get fid for remote directory $DIR/$rpath failed"
		check_path "$DIR/$rpath" $MOUNT $FID --link 0
	done

	return 0
}
run_test 162c "fid2path works with paths 100 or more directories deep"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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

# it would be good to share it with obdfilter-survey/iokit-libecho code
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

obdecho_test() {
        local OBD=$1
        local node=$2
	local pages=${3:-64}
        local rc=0
        local id

	local count=10
	local obd_size=$(get_obd_size $node $OBD)
	local page_size=$(get_page_size $node)
	if [[ -n "$obd_size" ]]; then
		local new_count=$((obd_size / (pages * page_size / 1024)))
		[[ $new_count -ge $count ]] || count=$new_count
	fi

        do_facet $node "$LCTL attach echo_client ec ec_uuid" || rc=1
        [ $rc -eq 0 ] && { do_facet $node "$LCTL --device ec setup $OBD" ||
                           rc=2; }
        if [ $rc -eq 0 ]; then
            id=$(do_facet $node "$LCTL --device ec create 1"  | awk '/object id/ {print $6}')
            [ ${PIPESTATUS[0]} -eq 0 -a -n "$id" ] || rc=3
        fi
        echo "New object id is $id"
	[ $rc -eq 0 ] && { do_facet $node "$LCTL --device ec getattr $id" ||
			   rc=4; }
	[ $rc -eq 0 ] && { do_facet $node "$LCTL --device ec "		       \
			   "test_brw $count w v $pages $id" || rc=4; }
	[ $rc -eq 0 ] && { do_facet $node "$LCTL --device ec destroy $id 1" ||
			   rc=4; }
	[ $rc -eq 0 -o $rc -gt 2 ] && { do_facet $node "$LCTL --device ec "    \
                                        "cleanup" || rc=5; }
        [ $rc -eq 0 -o $rc -gt 1 ] && { do_facet $node "$LCTL --device ec "    \
                                        "detach" || rc=6; }
        [ $rc -ne 0 ] && echo "obecho_create_test failed: $rc"
        return $rc
}

test_180a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	if ! module_loaded obdecho; then
		load_module obdecho/obdecho &&
			stack_trap "rmmod obdecho" EXIT ||
			error "unable to load obdecho on client"
	fi

	local osc=$($LCTL dl | grep -v mdt | awk '$3 == "osc" {print $4; exit}')
	local host=$($LCTL get_param -n osc.$osc.import |
		     awk '/current_connection:/ { print $2 }' )
	local target=$($LCTL get_param -n osc.$osc.import |
		       awk '/target:/ { print $2 }' )
	target=${target%_UUID}

	if [ -n "$target" ]; then
		setup_obdecho_osc $host $target &&
			stack_trap "cleanup_obdecho_osc $target" EXIT ||
			{ error "obdecho setup failed with $?"; return; }

		obdecho_test ${target}_osc client ||
			error "obdecho_test failed on ${target}_osc"
	else
		$LCTL get_param osc.$osc.import
		error "there is no osc.$osc.import target"
	fi
}
run_test 180a "test obdecho on osc"

test_180b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

	do_rpc_nodes $(facet_active_host ost1) load_module obdecho/obdecho &&
		stack_trap "do_facet ost1 rmmod obdecho" EXIT ||
		error "failed to load module obdecho"

	local target=$(do_facet ost1 $LCTL dl |
		       awk '/obdfilter/ { print $4; exit; }')

	if [ -n "$target" ]; then
		obdecho_test $target ost1 || error "obdecho_test failed with $?"
	else
		do_facet ost1 $LCTL dl
		error "there is no obdfilter target on ost1"
	fi
}
run_test 180b "test obdecho directly on obdfilter"

test_180c() { # LU-2598
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"
	[[ $MDS1_VERSION -lt $(version_code 2.4.0) ]] &&
		skip "Need MDS version at least 2.4.0"

	do_rpc_nodes $(facet_active_host ost1) load_module obdecho/obdecho &&
		stack_trap "do_facet ost1 rmmod obdecho" EXIT ||
		error "failed to load module obdecho"

	local target=$(do_facet ost1 $LCTL dl |
		       awk '/obdfilter/ { print $4; exit; }')

	if [ -n "$target" ]; then
		local pages=16384 # 64MB bulk I/O RPC size

		obdecho_test "$target" ost1 "$pages" ||
			error "obdecho_test with pages=$pages failed with $?"
	else
		do_facet ost1 $LCTL dl
		error "there is no obdfilter target on ost1"
	fi
}
run_test 180c "test huge bulk I/O size on obdfilter, don't LASSERT"

test_181() { # bug 22177
	test_mkdir $DIR/$tdir
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
	local fcount=1000
	local tcount=10

	mkdir -p $DIR/$tdir || error "creating dir $DIR/$tdir"

	$LCTL set_param mdc.*.rpc_stats=clear

	for (( i = 0; i < $tcount; i++ )) ; do
		mkdir $DIR/$tdir/$i
	done

	for (( i = 0; i < $tcount; i++ )) ; do
		createmany -o $DIR/$tdir/$i/f- $fcount &
	done
	wait

	for (( i = 0; i < $tcount; i++ )) ; do
		unlinkmany $DIR/$tdir/$i/f- $fcount &
	done
	wait

	$LCTL get_param mdc.*.rpc_stats

	rm -rf $DIR/$tdir
}
run_test 182 "Test parallel modify metadata operations ================"

test_183() { # LU-2275
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $MDS1_VERSION -lt $(version_code 2.3.56) ]] &&
		skip "Need MDS version at least 2.3.56"

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
	check_swap_layouts_support

	dir0=$DIR/$tdir/$testnum
	test_mkdir -p -c1 $dir0
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

	lfsck_verify_pfid $file1 $file2 || error "PFID are not transferred"
}
run_test 184a "Basic layout swap"

test_184b() {
	check_swap_layouts_support

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
	local cmpn_arg=$(cmp -n 2>&1 | grep "invalid option")
	[ -n "$cmpn_arg" ] && skip_env "cmp does not support -n"
	check_swap_layouts_support

	local dir0=$DIR/$tdir/$testnum
	mkdir -p $dir0 || error "creating dir $dir0"

	local ref1=$dir0/ref1
	local ref2=$dir0/ref2
	local file1=$dir0/file1
	local file2=$dir0/file2
	# create a file large enough for the concurrent test
	dd if=/dev/urandom of=$ref1 bs=1M count=$((RANDOM % 50 + 20))
	dd if=/dev/urandom of=$ref2 bs=1M count=$((RANDOM % 50 + 20))
	echo "ref file size: ref1($(stat -c %s $ref1))," \
	     "ref2($(stat -c %s $ref2))"

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
	local copied=$(stat -c %s $file2)
	local remaining=$(stat -c %s $ref1)
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

test_184d() {
	check_swap_layouts_support
	[ -z "$(which getfattr 2>/dev/null)" ] &&
		skip_env "no getfattr command"

	local file1=$DIR/$tdir/$tfile-1
	local file2=$DIR/$tdir/$tfile-2
	local file3=$DIR/$tdir/$tfile-3
	local lovea1
	local lovea2

	mkdir -p $DIR/$tdir
	touch $file1 || error "create $file1 failed"
	$OPENFILE -f O_CREAT:O_LOV_DELAY_CREATE $file2 ||
		error "create $file2 failed"
	$OPENFILE -f O_CREAT:O_LOV_DELAY_CREATE $file3 ||
		error "create $file3 failed"
	lovea1=$(get_layout_param $file1)

	$LFS swap_layouts $file2 $file3 ||
		error "swap $file2 $file3 layouts failed"
	$LFS swap_layouts $file1 $file2 ||
		error "swap $file1 $file2 layouts failed"

	lovea2=$(get_layout_param $file2)
	echo "$lovea1"
	echo "$lovea2"
	[ "$lovea1" == "$lovea2" ] || error "lovea $lovea1 != $lovea2"

	lovea1=$(getfattr -n trusted.lov $file1 | grep ^trusted)
	[[ -z "$lovea1" ]] || error "$file1 shouldn't have lovea"
}
run_test 184d "allow stripeless layouts swap"

test_184e() {
	[[ $MDS1_VERSION -ge $(version_code 2.6.94) ]] ||
		skip "Need MDS version at least 2.6.94"
	check_swap_layouts_support
	[ -z "$(which getfattr 2>/dev/null)" ] &&
		skip_env "no getfattr command"

	local file1=$DIR/$tdir/$tfile-1
	local file2=$DIR/$tdir/$tfile-2
	local file3=$DIR/$tdir/$tfile-3
	local lovea

	mkdir -p $DIR/$tdir
	touch $file1 || error "create $file1 failed"
	$OPENFILE -f O_CREAT:O_LOV_DELAY_CREATE $file2 ||
		error "create $file2 failed"
	$OPENFILE -f O_CREAT:O_LOV_DELAY_CREATE $file3 ||
		error "create $file3 failed"

	$LFS swap_layouts $file1 $file2 ||
		error "swap $file1 $file2 layouts failed"

	lovea=$(getfattr -n trusted.lov $file1 | grep ^trusted)
	[[ -z "$lovea" ]] || error "$file1 shouldn't have lovea"

	echo 123 > $file1 || error "Should be able to write into $file1"

	$LFS swap_layouts $file1 $file3 ||
		error "swap $file1 $file3 layouts failed"

	echo 123 > $file1 || error "Should be able to write into $file1"

	rm -rf $file1 $file2 $file3
}
run_test 184e "Recreate layout after stripeless layout swaps"

test_184f() {
	# Create a file with name longer than sizeof(struct stat) ==
	# 144 to see if we can get chars from the file name to appear
	# in the returned striping. Note that 'f' == 0x66.
	local file=$(for ((i = 0; i < 200; i++)); do echo -n f; done)

	mkdir -p $DIR/$tdir
	mcreate $DIR/$tdir/$file
	if lfs find --stripe-count 0x6666 $DIR/$tdir | grep $file; then
		error "IOC_MDC_GETFILEINFO returned garbage striping"
	fi
}
run_test 184f "IOC_MDC_GETFILEINFO for files with long names but no striping"

test_185() { # LU-2441
	# LU-3553 - no volatile file support in old servers
	[[ $MDS1_VERSION -ge $(version_code 2.3.60) ]] ||
		skip "Need MDS version at least 2.3.60"

	mkdir -p $DIR/$tdir || error "creating dir $DIR/$tdir"
	touch $DIR/$tdir/spoo
	local mtime1=$(stat -c "%Y" $DIR/$tdir)
	local fid=$($MULTIOP $DIR/$tdir VFw4096c) ||
		error "cannot create/write a volatile file"
	[ "$FILESET" == "" ] &&
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
	if [ "$FILESET" == "" ]; then
		$CHECKSTAT -t file $MOUNT/.lustre/fid/$next_fid ||
			error "FID is missing before close"
	fi
	kill -USR1 $multi_pid
	# 1 second delay, so if mtime change we will see it
	sleep 1
	local mtime2=$(stat -c "%Y" $DIR/$tdir)
	[[ $mtime1 == $mtime2 ]] || error "mtime has changed"
}
run_test 185 "Volatile file support"

function create_check_volatile() {
	local idx=$1
	local tgt

	$MULTIOP $MOUNT/.lustre/fid V${idx}Fw4096_c >&/tmp/${tfile}.fid &
	local PID=$!
	sleep 1
	local FID=$(cat /tmp/${tfile}.fid)
	[ "$FID" == "" ] && error "can't get FID for volatile"
	$CHECKSTAT -t file $MOUNT/.lustre/fid/$FID || error "can't stat $FID"
	tgt=$($LFS getstripe -m $MOUNT/.lustre/fid/$FID)
	[ "$tgt" != "$idx" ] && error "wrong MDS $tgt, expected $idx"
	kill -USR1 $PID
	wait
	sleep 1
	cancel_lru_locks mdc # flush opencache
	$CHECKSTAT -t file $MOUNT/.lustre/fid/$FID && error "can stat $FID"
	return 0
}

test_185a(){
	# LU-12516 - volatile creation via .lustre
	[[ $MDS1_VERSION -ge $(version_code 2.12.2) ]] ||
		skip "Need MDS version at least 2.12.2"

	create_check_volatile 0
	[ $MDSCOUNT -lt 2 ] && return 0

	# DNE case
	create_check_volatile 1

	return 0
}
run_test 185a "Volatile file creation in .lustre/fid/"

test_187a() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDS1_VERSION -lt $(version_code 2.3.0) ] &&
		skip "Need MDS version at least 2.3.0"

	local dir0=$DIR/$tdir/$testnum
	mkdir -p $dir0 || error "creating dir $dir0"

	local file=$dir0/file1
	dd if=/dev/urandom of=$file count=10 bs=1M conv=fsync
	local dv1=$($LFS data_version $file)
	dd if=/dev/urandom of=$file seek=10 count=1 bs=1M conv=fsync
	local dv2=$($LFS data_version $file)
	[[ $dv1 != $dv2 ]] ||
		error "data version did not change on write $dv1 == $dv2"

	# clean up
	rm -f $file1
}
run_test 187a "Test data version change"

test_187b() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDS1_VERSION -lt $(version_code 2.3.0) ] &&
		skip "Need MDS version at least 2.3.0"

	local dir0=$DIR/$tdir/$testnum
	mkdir -p $dir0 || error "creating dir $dir0"

	declare -a DV=$($MULTIOP $dir0 Vw1000xYw1000xY | cut -f3 -d" ")
	[[ ${DV[0]} != ${DV[1]} ]] ||
		error "data version did not change on write"\
		      " ${DV[0]} == ${DV[1]}"

	# clean up
	rm -f $file1
}
run_test 187b "Test data version change on volatile file"

test_200() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mgs_nodsh && skip "remote MGS with nodsh"
	[ -n "$FILESET" ] && skip "SKIP due to FILESET set"

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
	local subdir=$test_path/subdir
	local rc=0

	while : ; do
		# former test_200a test_200b
		pool_add $POOL				|| { rc=$? ; break; }
		pool_add_targets  $POOL $ost_range	|| { rc=$? ; break; }
		# former test_200c test_200d
		mkdir -p $test_path
		pool_set_dir      $POOL $test_path	|| { rc=$? ; break; }
		pool_check_dir    $POOL $test_path	|| { rc=$? ; break; }
		mkdir -p $subdir
		pool_check_dir    $POOL $subdir		|| { rc=$? ; break; }
		pool_dir_rel_path $POOL $POOL_DIR_NAME $POOL_ROOT \
							|| { rc=$? ; break; }
		# former test_200e test_200f
		local files=$((OSTCOUNT*3))
		pool_alloc_files  $POOL $test_path $files "$ost_list" \
							|| { rc=$? ; break; }
		pool_create_files $POOL $file_dir $files "$ost_list" \
							|| { rc=$? ; break; }
		# former test_200g test_200h
		pool_lfs_df $POOL			|| { rc=$? ; break; }
		pool_file_rel_path $POOL $test_path	|| { rc=$? ; break; }

		# former test_201a test_201b test_201c
		pool_remove_first_target $POOL		|| { rc=$? ; break; }

		local f=$test_path/$tfile
		pool_remove_all_targets $POOL $f	|| { rc=$? ; break; }
		pool_remove $POOL $f			|| { rc=$? ; break; }
		break
	done

	destroy_test_pools

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
	--stripe-count|-c)
		[ -n "$2" ] && EXPECTED=0 || EXPECTED=$(default_attr count);;
	--stripe-size|-S)
		[ -n "$2" ] && EXPECTED=0 || EXPECTED=$(default_attr size);;
	--stripe-index|-i)
		EXPECTED=-1;;
	*)
		error "unknown getstripe attr '$1'"
	esac

	[ $ACTUAL == $EXPECTED ] ||
		error "$DIR/$tdir has $1 '$ACTUAL', not '$EXPECTED'"
}

test_204a() {
	test_mkdir $DIR/$tdir
	$SETSTRIPE --stripe-count 0 --stripe-size 0 --stripe-index -1 $DIR/$tdir

	check_default_stripe_attr --stripe-count
	check_default_stripe_attr --stripe-size
	check_default_stripe_attr --stripe-index
}
run_test 204a "Print default stripe attributes"

test_204b() {
	test_mkdir $DIR/$tdir
	$SETSTRIPE --stripe-count 1 $DIR/$tdir

	check_default_stripe_attr --stripe-size
	check_default_stripe_attr --stripe-index
}
run_test 204b "Print default stripe size and offset"

test_204c() {
	test_mkdir $DIR/$tdir
	$SETSTRIPE --stripe-size 65536 $DIR/$tdir

	check_default_stripe_attr --stripe-count
	check_default_stripe_attr --stripe-index
}
run_test 204c "Print default stripe count and offset"

test_204d() {
	test_mkdir $DIR/$tdir
	$SETSTRIPE --stripe-index 0 $DIR/$tdir

	check_default_stripe_attr --stripe-count
	check_default_stripe_attr --stripe-size
}
run_test 204d "Print default stripe count and size"

test_204e() {
	test_mkdir $DIR/$tdir
	$SETSTRIPE -d $DIR/$tdir

	check_default_stripe_attr --stripe-count --raw
	check_default_stripe_attr --stripe-size --raw
	check_default_stripe_attr --stripe-index --raw
}
run_test 204e "Print raw stripe attributes"

test_204f() {
	test_mkdir $DIR/$tdir
	$SETSTRIPE --stripe-count 1 $DIR/$tdir

	check_default_stripe_attr --stripe-size --raw
	check_default_stripe_attr --stripe-index --raw
}
run_test 204f "Print raw stripe size and offset"

test_204g() {
	test_mkdir $DIR/$tdir
	$SETSTRIPE --stripe-size 65536 $DIR/$tdir

	check_default_stripe_attr --stripe-count --raw
	check_default_stripe_attr --stripe-index --raw
}
run_test 204g "Print raw stripe count and offset"

test_204h() {
	test_mkdir $DIR/$tdir
	$SETSTRIPE --stripe-index 0 $DIR/$tdir

	check_default_stripe_attr --stripe-count --raw
	check_default_stripe_attr --stripe-size --raw
}
run_test 204h "Print raw stripe count and size"

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
	$LCTL list_param jobid_name > /dev/null 2>&1
	if [ $? -eq 0 ]; then
		JOBENV=nodelocal
	else
		JOBENV=FAKE_JOBID
	fi
fi
LUSTRE_JOBID_SIZE=31 # plus NUL terminator

verify_jobstats() {
	local cmd=($1)
	shift
	local facets="$@"

# we don't really need to clear the stats for this test to work, since each
# command has a unique jobid, but it makes debugging easier if needed.
#	for facet in $facets; do
#		local dev=$(convert_facet2label $facet)
#		# clear old jobstats
#		do_facet $facet lctl set_param *.$dev.job_stats="clear"
#	done

	# use a new JobID for each test, or we might see an old one
	[ "$JOBENV" = "FAKE_JOBID" ] &&
		FAKE_JOBID=id.$testnum.$(basename ${cmd[0]}).$RANDOM

	JOBVAL=${!JOBENV:0:$LUSTRE_JOBID_SIZE}

	[ "$JOBENV" = "nodelocal" ] && {
		FAKE_JOBID=id.$testnum.%e.$RANDOM
		$LCTL set_param jobid_name=$FAKE_JOBID
		JOBVAL=${FAKE_JOBID/\%e/$(basename ${cmd[0]})}
	}

	log "Test: ${cmd[*]}"
	log "Using JobID environment $($LCTL get_param -n jobid_var)=$JOBVAL"

	if [ $JOBENV = "FAKE_JOBID" ]; then
		FAKE_JOBID=$JOBVAL ${cmd[*]}
	else
		${cmd[*]}
	fi

	# all files are created on OST0000
	for facet in $facets; do
		local stats="*.$(convert_facet2label $facet).job_stats"

		# strip out libtool wrappers for in-tree executables
		if [ $(do_facet $facet lctl get_param $stats |
		       sed -e 's/\.lt-/./' | grep -c $JOBVAL) -ne 1 ]; then
			do_facet $facet lctl get_param $stats
			error "No jobstats for $JOBVAL found on $facet::$stats"
		fi
	done
}

jobstats_set() {
	local new_jobenv=$1

	set_persistent_param_and_check client "jobid_var" \
		"$FSNAME.sys.jobid_var" $new_jobenv
}

test_205a() { # Job stats
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $MDS1_VERSION -ge $(version_code 2.7.1) ]] ||
		skip "Need MDS version with at least 2.7.1"
	remote_mgs_nodsh && skip "remote MGS with nodsh"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ -z "$(lctl get_param -n mdc.*.connect_flags | grep jobstats)" ] &&
		skip "Server doesn't support jobstats"
	[[ $JOBID_VAR = disable ]] && skip_env "jobstats is disabled"

	local old_jobenv=$($LCTL get_param -n jobid_var)
	[ $old_jobenv != $JOBENV ] && jobstats_set $JOBENV

	if [[ $PERM_CMD = *"set_param -P"* ]]; then
		stack_trap "do_facet mgs $PERM_CMD jobid_var=$old_jobenv" EXIT
	else
		stack_trap "do_facet mgs $PERM_CMD \
			$FSNAME.sys.jobid_var=$old_jobenv" EXIT
	fi
	changelog_register

	local old_interval=$(do_facet $SINGLEMDS lctl get_param -n \
				mdt.*.job_cleanup_interval | head -n 1)
	local new_interval=5
	do_facet $SINGLEMDS \
		$LCTL set_param mdt.*.job_cleanup_interval=$new_interval
	stack_trap "do_facet $SINGLEMDS \
		$LCTL set_param mdt.*.job_cleanup_interval=$old_interval" EXIT
	local start=$SECONDS

	local cmd
	# mkdir
	cmd="mkdir $DIR/$tdir"
	verify_jobstats "$cmd" "$SINGLEMDS"
	# rmdir
	cmd="rmdir $DIR/$tdir"
	verify_jobstats "$cmd" "$SINGLEMDS"
	# mkdir on secondary MDT
	if [ $MDSCOUNT -gt 1 ]; then
		cmd="lfs mkdir -i 1 $DIR/$tdir.remote"
		verify_jobstats "$cmd" "mds2"
	fi
	# mknod
	cmd="mknod $DIR/$tfile c 1 3"
	verify_jobstats "$cmd" "$SINGLEMDS"
	# unlink
	cmd="rm -f $DIR/$tfile"
	verify_jobstats "$cmd" "$SINGLEMDS"
	# create all files on OST0000 so verify_jobstats can find OST stats
	# open & close
	cmd="$SETSTRIPE -i 0 -c 1 $DIR/$tfile"
	verify_jobstats "$cmd" "$SINGLEMDS"
	# setattr
	cmd="touch $DIR/$tfile"
	verify_jobstats "$cmd" "$SINGLEMDS ost1"
	# write
	cmd="dd if=/dev/zero of=$DIR/$tfile bs=1M count=1 oflag=sync"
	verify_jobstats "$cmd" "ost1"
	# read
	cancel_lru_locks osc
	cmd="dd if=$DIR/$tfile of=/dev/null bs=1M count=1 iflag=direct"
	verify_jobstats "$cmd" "ost1"
	# truncate
	cmd="$TRUNCATE $DIR/$tfile 0"
	verify_jobstats "$cmd" "$SINGLEMDS ost1"
	# rename
	cmd="mv -f $DIR/$tfile $DIR/$tdir.rename"
	verify_jobstats "$cmd" "$SINGLEMDS"
	# jobstats expiry - sleep until old stats should be expired
	local left=$((new_interval + 5 - (SECONDS - start)))
	[ $left -ge 0 ] && wait_update_facet $SINGLEMDS \
		"lctl get_param *.*.job_stats | grep -c 'job_id.*mkdir'" \
			"0" $left
	cmd="mkdir $DIR/$tdir.expire"
	verify_jobstats "$cmd" "$SINGLEMDS"
	[ $(do_facet $SINGLEMDS lctl get_param *.*.job_stats |
	    grep -c "job_id.*mkdir") -gt 1 ] && error "old jobstats not expired"

	# Ensure that jobid are present in changelog (if supported by MDS)
	if [ $MDS1_VERSION -ge $(version_code 2.6.52) ];then
		changelog_dump | tail -10
		jobids=$(changelog_dump | tail -9 | grep -c "j=")
		[ $jobids -eq 9 ] ||
			error "Wrong changelog jobid count $jobids != 9"

		# LU-5862
		JOBENV="disable"
		jobstats_set $JOBENV
		touch $DIR/$tfile
		changelog_dump | grep $tfile
		jobids=$(changelog_dump | grep $tfile | tail -1 | grep -c "j=")
		[ $jobids -eq 0 ] ||
			error "Unexpected jobids when jobid_var=$JOBENV"
	fi

	lctl set_param jobid_var=USER jobid_name="S.%j.%e.%u.%h.E"
	JOBENV="JOBCOMPLEX"
	JOBCOMPLEX="S.$USER.touch.$(id -u).$(hostname).E"

	verify_jobstats "touch $DIR/$tfile" $SINGLEMDS
}
run_test 205a "Verify job stats"

# LU-13117
test_205b() {
	$LCTL set_param jobid_var=USER jobid_name="%e.%u"
	env -i USERTESTJOBSTATS=foolish touch $DIR/$tfile.1
	do_facet $SINGLEMDS $LCTL get_param mdt.*.job_stats |
		grep job_id: | grep foolish &&
			error "Unexpected jobid found"
	true
}
run_test 205b "Verify job stats jobid parsing"

# LU-1480, LU-1773 and LU-1657
test_206() {
	mkdir -p $DIR/$tdir
	$SETSTRIPE -c -1 $DIR/$tdir
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

test_208() {
	# FIXME: in this test suite, only RD lease is used. This is okay
	# for now as only exclusive open is supported. After generic lease
	# is done, this test suite should be revised. - Jinshan

	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $MDS1_VERSION -ge $(version_code 2.4.52) ]] ||
		skip "Need MDS version at least 2.4.52"

	echo "==== test 1: verify get lease work"
	$MULTIOP $DIR/$tfile oO_CREAT:O_RDWR:eRE+eU || error "get lease error"

	echo "==== test 2: verify lease can be broken by upcoming open"
	$MULTIOP $DIR/$tfile oO_RDONLY:eR_E-eUc &
	local PID=$!
	sleep 1

	$MULTIOP $DIR/$tfile oO_RDONLY:c
	kill -USR1 $PID && wait $PID || error "break lease error"

	echo "==== test 3: verify lease can't be granted if an open already exists"
	$MULTIOP $DIR/$tfile oO_RDONLY:_c &
	local PID=$!
	sleep 1

	$MULTIOP $DIR/$tfile oO_RDONLY:eReUc && error "apply lease should fail"
	kill -USR1 $PID && wait $PID || error "open file error"

	echo "==== test 4: lease can sustain over recovery"
	$MULTIOP $DIR/$tfile oO_RDONLY:eR_E+eUc &
	PID=$!
	sleep 1

	fail mds1

	kill -USR1 $PID && wait $PID || error "lease broken over recovery"

	echo "==== test 5: lease broken can't be regained by replay"
	$MULTIOP $DIR/$tfile oO_RDONLY:eR_E-eUc &
	PID=$!
	sleep 1

	# open file to break lease and then recovery
	$MULTIOP $DIR/$tfile oO_RDWR:c || error "open file error"
	fail mds1

	kill -USR1 $PID && wait $PID || error "lease not broken over recovery"

	rm -f $DIR/$tfile
}
run_test 208 "Exclusive open"

test_209() {
	[ -z "$(lctl get_param -n mdc.*.connect_flags | grep disp_stripe)" ] &&
		skip_env "must have disp_stripe"

	touch $DIR/$tfile
	sync; sleep 5; sync;

	echo 3 > /proc/sys/vm/drop_caches
	req_before=$(awk '/ptlrpc_cache / { print $2 }' /proc/slabinfo)

	# open/close 500 times
	for i in $(seq 500); do
		cat $DIR/$tfile
	done

	echo 3 > /proc/sys/vm/drop_caches
	req_after=$(awk '/ptlrpc_cache / { print $2 }' /proc/slabinfo)

	echo "before: $req_before, after: $req_after"
	[ $((req_after - req_before)) -ge 300 ] &&
		error "open/close requests are not freed"
	return 0
}
run_test 209 "read-only open/close requests should be freed promptly"

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
	mkdir -p $DIR/$tdir/d214c || error "mkdir $DIR/$tdir/d214c failed"
	for (( i=0; i < 340; i++ )) ; do
		touch $DIR/$tdir/d214c/a$i
	done

	ls -l $DIR/$tdir || error "ls -l $DIR/d214p failed"
	mv $DIR/$tdir/d214c $DIR/ || error "mv $DIR/d214p/d214c $DIR/ failed"
	ls $DIR/d214c || error "ls $DIR/d214c failed"
	rm -rf $DIR/$tdir || error "rm -rf $DIR/d214* failed"
	rm -rf $DIR/d214* || error "rm -rf $DIR/d214* failed"
}
run_test 214 "hash-indexed directory test - bug 20133"

# having "abc" as 1st arg, creates $TMP/lnet_abc.out and $TMP/lnet_abc.sys
create_lnet_proc_files() {
	lctl get_param -n $1 >$TMP/lnet_$1.sys || error "cannot read lnet.$1"
}

# counterpart of create_lnet_proc_files
remove_lnet_proc_files() {
	rm -f $TMP/lnet_$1.sys
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
	local blp=2          # blp stands for 'position of 1st line of body'
	[ -z "$5" ] || blp=3 # lnet.routes case

	local l=$(cat "$TMP/lnet_$1" |wc -l)
	# subtracting one from $blp because the body can be empty
	[ "$l" -ge "$(($blp - 1))" ] || (cat "$TMP/lnet_$1" && error "$2 is too short: $l")

	sed -n '1 p' "$TMP/lnet_$1" |grep -E "$4" >/dev/null ||
		(cat "$TMP/lnet_$1" && error "1st line of $2 misformatted")

	[ "$5" = "" ] || sed -n '2 p' "$TMP/lnet_$1" |grep -E "$5" >/dev/null ||
		(cat "$TMP/lnet_$1" && error "2nd line of $2 misformatted")

	# bail out if any unexpected line happened
	sed -n "$blp p" "$TMP/lnet_$1" | grep -Ev "$3"
	[ "$?" != 0 ] || error "$2 misformatted"
}

test_215() { # for bugs 18102, 21079, 21517
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local N='(0|[1-9][0-9]*)'       # non-negative numeric
	local P='[1-9][0-9]*'           # positive numeric
	local I='(0|-?[1-9][0-9]*|NA)'  # any numeric (0 | >0 | <0) or NA if no value
	local NET='[a-z][a-z0-9]*'      # LNET net like o2ib2
	local ADDR='[0-9.]+'            # LNET addr like 10.0.0.1
	local NID="$ADDR@$NET"          # LNET nid like 10.0.0.1@o2ib2

	local L1 # regexp for 1st line
	local L2 # regexp for 2nd line (optional)
	local BR # regexp for the rest (body)

	# lnet.stats should look as 11 space-separated non-negative numerics
	BR="^$N $N $N $N $N $N $N $N $N $N $N$"
	create_lnet_proc_files "stats"
	check_lnet_proc_stats "stats.sys" "lnet.stats" "$BR"
	remove_lnet_proc_files "stats"

	# lnet.routes should look like this:
	# Routing disabled/enabled
	# net hops priority state router
	# where net is a string like tcp0, hops > 0, priority >= 0,
	# state is up/down,
	# router is a string like 192.168.1.1@tcp2
	L1="^Routing (disabled|enabled)$"
	L2="^net +hops +priority +state +router$"
	BR="^$NET +$N +(0|1) +(up|down) +$NID$"
	create_lnet_proc_files "routes"
	check_lnet_proc_entry "routes.sys" "lnet.routes" "$BR" "$L1" "$L2"
	remove_lnet_proc_files "routes"

	# lnet.routers should look like this:
	# ref rtr_ref alive_cnt state last_ping ping_sent deadline down_ni router
	# where ref > 0, rtr_ref > 0, alive_cnt >= 0, state is up/down,
	# last_ping >= 0, ping_sent is boolean (0/1), deadline and down_ni are
	# numeric (0 or >0 or <0), router is a string like 192.168.1.1@tcp2
	L1="^ref +rtr_ref +alive_cnt +state +last_ping +ping_sent +deadline +down_ni +router$"
	BR="^$P +$P +$N +(up|down) +$N +(0|1) +$I +$I +$NID$"
	create_lnet_proc_files "routers"
	check_lnet_proc_entry "routers.sys" "lnet.routers" "$BR" "$L1"
	remove_lnet_proc_files "routers"

	# lnet.peers should look like this:
	# nid refs state last max rtr min tx min queue
	# where nid is a string like 192.168.1.1@tcp2, refs > 0,
	# state is up/down/NA, max >= 0. last, rtr, min, tx, min are
	# numeric (0 or >0 or <0), queue >= 0.
	L1="^nid +refs +state +last +max +rtr +min +tx +min +queue$"
	BR="^$NID +$P +(up|down|NA) +$I +$N +$I +$I +$I +$I +$N$"
	create_lnet_proc_files "peers"
	check_lnet_proc_entry "peers.sys" "lnet.peers" "$BR" "$L1"
	remove_lnet_proc_files "peers"

	# lnet.buffers  should look like this:
	# pages count credits min
	# where pages >=0, count >=0, credits and min are numeric (0 or >0 or <0)
	L1="^pages +count +credits +min$"
	BR="^ +$N +$N +$I +$I$"
	create_lnet_proc_files "buffers"
	check_lnet_proc_entry "buffers.sys" "lnet.buffers" "$BR" "$L1"
	remove_lnet_proc_files "buffers"

	# lnet.nis should look like this:
	# nid status alive refs peer rtr max tx min
	# where nid is a string like 192.168.1.1@tcp2, status is up/down,
	# alive is numeric (0 or >0 or <0), refs >= 0, peer >= 0,
	# rtr >= 0, max >=0, tx and min are numeric (0 or >0 or <0).
	L1="^nid +status +alive +refs +peer +rtr +max +tx +min$"
	BR="^$NID +(up|down) +$I +$N +$N +$N +$N +$I +$I$"
	create_lnet_proc_files "nis"
	check_lnet_proc_entry "nis.sys" "lnet.nis" "$BR" "$L1"
	remove_lnet_proc_files "nis"

	# can we successfully write to lnet.stats?
	lctl set_param -n stats=0 || error "cannot write to lnet.stats"
}
run_test 215 "lnet exists and has proper content - bugs 18102, 21079, 21517"

test_216() { # bug 20317
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"

	local node
	local facets=$(get_facets OST)
	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"

	save_lustre_params client "osc.*.contention_seconds" > $p
	save_lustre_params $facets \
		"ldlm.namespaces.filter-*.max_nolock_bytes" >> $p
	save_lustre_params $facets \
		"ldlm.namespaces.filter-*.contended_locks" >> $p
	save_lustre_params $facets \
		"ldlm.namespaces.filter-*.contention_seconds" >> $p
	clear_stats osc.*.osc_stats

	# agressive lockless i/o settings
	do_nodes $(comma_list $(osts_nodes)) \
		"lctl set_param -n ldlm.namespaces.*.max_nolock_bytes=2000000 \
			ldlm.namespaces.filter-*.contended_locks=0 \
			ldlm.namespaces.filter-*.contention_seconds=60"
	lctl set_param -n osc.*.contention_seconds=60

	$DIRECTIO write $DIR/$tfile 0 10 4096
	$CHECKSTAT -s 40960 $DIR/$tfile

	# disable lockless i/o
	do_nodes $(comma_list $(osts_nodes)) \
		"lctl set_param -n ldlm.namespaces.filter-*.max_nolock_bytes=0 \
			ldlm.namespaces.filter-*.contended_locks=32 \
			ldlm.namespaces.filter-*.contention_seconds=0"
	lctl set_param -n osc.*.contention_seconds=0
	clear_stats osc.*.osc_stats

	dd if=/dev/zero of=$DIR/$tfile count=0
	$CHECKSTAT -s 0 $DIR/$tfile

	restore_lustre_params <$p
	rm -f $p
	rm $DIR/$tfile
}
run_test 216 "check lockless direct write updates file size and kms correctly"

test_217() { # bug 22430
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local node
	local nid

	for node in $(nodes_list); do
		nid=$(host_nids_address $node $NETTYPE)
		if [[ $nid = *-* ]] ; then
			echo "lctl ping $(h2nettype $nid)"
			lctl ping $(h2nettype $nid)
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
run_test 218 "parallel read and truncate should not deadlock"

test_219() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

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

	# LU-4201
	dd if=/dev/zero of=$DIR/$tfile-2 bs=1024 count=1
	$CHECKSTAT -s 1024 $DIR/$tfile-2 || error "checkstat wrong size"
}
run_test 219 "LU-394: Write partial won't cause uncontiguous pages vec at LND"

test_220() { #LU-325
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_ost_nodsh && skip "remote OST with nodsh"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_mgs_nodsh && skip "remote MGS with nodsh"

	local OSTIDX=0

	# create on MDT0000 so the last_id and next_id are correct
	mkdir $DIR/$tdir
	local OST=$($LFS df $DIR | awk '/OST:'$OSTIDX'/ { print $1 }')
	OST=${OST%_UUID}

	# on the mdt's osc
	local mdtosc_proc1=$(get_mdtosc_proc_path $SINGLEMDS $OST)
	local last_id=$(do_facet $SINGLEMDS lctl get_param -n \
			osp.$mdtosc_proc1.prealloc_last_id)
	local next_id=$(do_facet $SINGLEMDS lctl get_param -n \
			osp.$mdtosc_proc1.prealloc_next_id)

	$LFS df -i

	do_facet ost$((OSTIDX + 1)) lctl set_param fail_val=-1
	#define OBD_FAIL_OST_ENOINO              0x229
	do_facet ost$((OSTIDX + 1)) lctl set_param fail_loc=0x229
	create_pool $FSNAME.$TESTNAME || return 1
	do_facet mgs $LCTL pool_add $FSNAME.$TESTNAME $OST || return 2

	$SETSTRIPE $DIR/$tdir -i $OSTIDX -c 1 -p $FSNAME.$TESTNAME

	MDSOBJS=$((last_id - next_id))
	echo "preallocated objects on MDS is $MDSOBJS" "($last_id - $next_id)"

	blocks=$($LFS df $MOUNT | awk '($1 == '$OSTIDX') { print $4 }')
	echo "OST still has $count kbytes free"

	echo "create $MDSOBJS files @next_id..."
	createmany -o $DIR/$tdir/f $MDSOBJS || return 3

	local last_id2=$(do_facet mds${MDSIDX} lctl get_param -n \
			osp.$mdtosc_proc1.prealloc_last_id)
	local next_id2=$(do_facet mds${MDSIDX} lctl get_param -n \
			osp.$mdtosc_proc1.prealloc_next_id)

	echo "after creation, last_id=$last_id2, next_id=$next_id2"
	$LFS df -i

	echo "cleanup..."

	do_facet ost$((OSTIDX + 1)) lctl set_param fail_val=0
	do_facet ost$((OSTIDX + 1)) lctl set_param fail_loc=0

	do_facet mgs $LCTL pool_remove $FSNAME.$TESTNAME $OST ||
		error "$LCTL pool_remove $FSNAME.$TESTNAME $OST failed"
	do_facet mgs $LCTL pool_destroy $FSNAME.$TESTNAME ||
		error "$LCTL pool_destroy $FSNAME.$TESTNAME failed"
	echo "unlink $MDSOBJS files @$next_id..."
	unlinkmany $DIR/$tdir/f $MDSOBJS || error "unlinkmany failed"
}
run_test 220 "preallocated MDS objects still used if ENOSPC from OST"

test_221() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	dd if=`which date` of=$MOUNT/date oflag=sync
	chmod +x $MOUNT/date

	#define OBD_FAIL_LLITE_FAULT_TRUNC_RACE  0x1401
	$LCTL set_param fail_loc=0x80001401

	$MOUNT/date > /dev/null
	rm -f $MOUNT/date
}
run_test 221 "make sure fault and truncate race to not cause OOM"

test_222a () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	rm -rf $DIR/$tdir
	test_mkdir $DIR/$tdir
	$LFS setstripe -c 1 -i 0 $DIR/$tdir
	createmany -o $DIR/$tdir/$tfile 10
	cancel_lru_locks mdc
	cancel_lru_locks osc
	#define OBD_FAIL_LDLM_AGL_DELAY           0x31a
	$LCTL set_param fail_loc=0x31a
	ls -l $DIR/$tdir > /dev/null || error "AGL for ls failed"
	$LCTL set_param fail_loc=0
	rm -r $DIR/$tdir
}
run_test 222a "AGL for ls should not trigger CLIO lock failure"

test_222b () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	rm -rf $DIR/$tdir
	test_mkdir $DIR/$tdir
	$LFS setstripe -c 1 -i 0 $DIR/$tdir
	createmany -o $DIR/$tdir/$tfile 10
	cancel_lru_locks mdc
	cancel_lru_locks osc
	#define OBD_FAIL_LDLM_AGL_DELAY           0x31a
	$LCTL set_param fail_loc=0x31a
	rm -r $DIR/$tdir || error "AGL for rmdir failed"
	$LCTL set_param fail_loc=0
}
run_test 222b "AGL for rmdir should not trigger CLIO lock failure"

test_223 () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	rm -rf $DIR/$tdir
	test_mkdir $DIR/$tdir
	$LFS setstripe -c 1 -i 0 $DIR/$tdir
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	#define OBD_FAIL_PTLRPC_CLIENT_BULK_CB   0x508
	$LCTL set_param fail_loc=0x508
	dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1 conv=fsync
	$LCTL set_param fail_loc=0
	df $DIR
}
run_test 224a "Don't panic on bulk IO failure"

test_224b() { # LU-1039, MRP-303
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	dd if=/dev/zero of=$DIR/$tfile bs=4096 count=1
	cancel_lru_locks osc
	#define OBD_FAIL_PTLRPC_CLIENT_BULK_CB2   0x515
	$LCTL set_param fail_loc=0x515
	dd of=/dev/null if=$DIR/$tfile bs=4096 count=1
	$LCTL set_param fail_loc=0
	df $DIR
}
run_test 224b "Don't panic on bulk IO failure"

test_224c() { # LU-6441
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local p="$TMP/$TESTSUITE-$TESTNAME.parameters"
	save_writethrough $p
	set_cache writethrough on

	local pages_per_rpc=$($LCTL get_param \
				osc.*.max_pages_per_rpc)
	local at_max=$($LCTL get_param -n at_max)
	local timeout=$($LCTL get_param -n timeout)
	local test_at="at_max"
	local param_at="$FSNAME.sys.at_max"
	local test_timeout="timeout"
	local param_timeout="$FSNAME.sys.timeout"

	$LCTL set_param -n osc.*.max_pages_per_rpc=1024

	set_persistent_param_and_check client "$test_at" "$param_at" 0
	set_persistent_param_and_check client "$test_timeout" "$param_timeout" 5

	#define OBD_FAIL_PTLRPC_CLIENT_BULK_CB3   0x520
	do_facet ost1 $LCTL set_param fail_loc=0x520
	$LFS setstripe -c 1 -i 0 $DIR/$tfile
	dd if=/dev/zero of=$DIR/$tfile bs=8MB count=1
	sync
	do_facet ost1 $LCTL set_param fail_loc=0

	set_persistent_param_and_check client "$test_at" "$param_at" $at_max
	set_persistent_param_and_check client "$test_timeout" "$param_timeout" \
		$timeout

	$LCTL set_param -n $pages_per_rpc
	restore_lustre_params < $p
	rm -f $p
}
run_test 224c "Don't hang if one of md lost during large bulk RPC"

MDSSURVEY=${MDSSURVEY:-$(which mds-survey 2>/dev/null || true)}
test_225a () {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	if [ -z ${MDSSURVEY} ]; then
		skip_env "mds-survey not found"
	fi
	[ $MDS1_VERSION -ge $(version_code 2.2.51) ] ||
		skip "Need MDS version at least 2.2.51"

	local mds=$(facet_host $SINGLEMDS)
	local target=$(do_nodes $mds 'lctl dl' |
		       awk '{ if ($2 == "UP" && $3 == "mdt") { print $4 }}')

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
	if [ -z ${MDSSURVEY} ]; then
		skip_env "mds-survey not found"
	fi
	[ $MDS1_VERSION -ge $(version_code 2.2.51) ] ||
		skip "Need MDS version at least 2.2.51"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	if [ $($LCTL dl | grep -c osc) -eq 0 ]; then
		skip_env "Need to mount OST to test"
	fi

	local mds=$(facet_host $SINGLEMDS)
	local target=$(do_nodes $mds 'lctl dl' |
		       awk '{ if ($2 == "UP" && $3 == "mdt") { print $4 }}')

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
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	local MDTIDX=1

	rm -rf $DIR/$tdir
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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ -z "$(which ldd)" ] && skip_env "should have ldd tool"

	dd if=$(which date) of=$MOUNT/date bs=1k count=1
	chmod +x $MOUNT/date

	$MOUNT/date > /dev/null
	ldd $MOUNT/date > /dev/null
	rm -f $MOUNT/date
}
run_test 227 "running truncated executable does not cause OOM"

# LU-1512 try to reuse idle OI blocks
test_228a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ "$mds1_FSTYPE" != "ldiskfs" ] && skip_env "ldiskfs only test"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ "$mds1_FSTYPE" != "ldiskfs" ] && skip_env "ldiskfs only test"

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
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ "$mds1_FSTYPE" != "ldiskfs" ] && skip_env "ldiskfs only test"

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

test_229() { # LU-2482, LU-3448
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $OSTCOUNT -lt 2 ] && skip_env "needs >= 2 OSTs"
	[ $MDS1_VERSION -lt $(version_code 2.4.53) ] &&
		skip "No HSM $(lustre_build_version $SINGLEMDS) MDS < 2.4.53"

	rm -f $DIR/$tfile

	# Create a file with a released layout and stripe count 2.
	$MULTIOP $DIR/$tfile H2c ||
		error "failed to create file with released layout"

	$GETSTRIPE -v $DIR/$tfile

	local pattern=$($GETSTRIPE -L $DIR/$tfile)
	[ X"$pattern" = X"released" ] || error "pattern error ($pattern)"

	local stripe_count=$($GETSTRIPE -c $DIR/$tfile) || error "getstripe"
	[ $stripe_count -eq 2 ] || error "stripe count not 2 ($stripe_count)"
	stat $DIR/$tfile || error "failed to stat released file"

	chown $RUNAS_ID $DIR/$tfile ||
		error "chown $RUNAS_ID $DIR/$tfile failed"

	chgrp $RUNAS_ID $DIR/$tfile ||
		error "chgrp $RUNAS_ID $DIR/$tfile failed"

	touch $DIR/$tfile || error "touch $DIR/$tfile failed"
	rm $DIR/$tfile || error "failed to remove released file"
}
run_test 229 "getstripe/stat/rm/attr changes work on released files"

test_230a() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	local MDTIDX=1

	test_mkdir $DIR/$tdir
	test_mkdir -i0 -c1 $DIR/$tdir/test_230_local
	local mdt_idx=$($LFS getstripe -m $DIR/$tdir/test_230_local)
	[ $mdt_idx -ne 0 ] &&
		error "create local directory on wrong MDT $mdt_idx"

	$LFS mkdir -i $MDTIDX $DIR/$tdir/test_230 ||
			error "create remote directory failed"
	local mdt_idx=$($LFS getstripe -m $DIR/$tdir/test_230)
	[ $mdt_idx -ne $MDTIDX ] &&
		error "create remote directory on wrong MDT $mdt_idx"

	createmany -o $DIR/$tdir/test_230/t- 10 ||
		error "create files on remote directory failed"
	mdt_idx=$($LFS getstripe -m $DIR/$tdir/test_230/t-0)
	[ $mdt_idx -ne $MDTIDX ] && error "create files on wrong MDT $mdt_idx"
	rm -r $DIR/$tdir || error "unlink remote directory failed"
}
run_test 230a "Create remote directory and files under the remote directory"

test_230b() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	local MDTIDX=1
	local mdt_index
	local i
	local file
	local pid
	local stripe_count
	local migrate_dir=$DIR/$tdir/migrate_dir
	local other_dir=$DIR/$tdir/other_dir

	test_mkdir $DIR/$tdir
	test_mkdir -i0 -c1 $migrate_dir
	test_mkdir -i0 -c1 $other_dir
	for ((i=0; i<10; i++)); do
		mkdir -p $migrate_dir/dir_${i}
		createmany -o $migrate_dir/dir_${i}/f 10 ||
			error "create files under remote dir failed $i"
	done

	cp /etc/passwd $migrate_dir/$tfile
	cp /etc/passwd $other_dir/$tfile
	chattr +SAD $migrate_dir
	chattr +SAD $migrate_dir/$tfile

	local old_dir_flag=$(lsattr -a $migrate_dir | awk '/\/\.$/ {print $1}')
	local old_file_flag=$(lsattr $migrate_dir/$tfile | awk '{print $1}')
	local old_dir_mode=$(stat -c%f $migrate_dir)
	local old_file_mode=$(stat -c%f $migrate_dir/$tfile)

	mkdir -p $migrate_dir/dir_default_stripe2
	$SETSTRIPE -c 2 $migrate_dir/dir_default_stripe2
	$SETSTRIPE -c 2 $migrate_dir/${tfile}_stripe2

	mkdir -p $other_dir
	ln $migrate_dir/$tfile $other_dir/luna
	ln $migrate_dir/$tfile $migrate_dir/sofia
	ln $other_dir/$tfile $migrate_dir/david
	ln -s $migrate_dir/$tfile $other_dir/zachary
	ln -s $migrate_dir/$tfile $migrate_dir/${tfile}_ln
	ln -s $other_dir/$tfile $migrate_dir/${tfile}_ln_other

	local len
	local lnktgt

	# inline symlink
	for len in 58 59 60; do
		lnktgt=$(str_repeat 'l' $len)
		touch $migrate_dir/$lnktgt
		ln -s $lnktgt $migrate_dir/${len}char_ln
	done

	# PATH_MAX
	for len in 4094 4095; do
		lnktgt=$(str_repeat 'l' $len)
		ln -s $lnktgt $migrate_dir/${len}char_ln
	done

	# NAME_MAX
	for len in 254 255; do
		touch $migrate_dir/$(str_repeat 'l' $len)
	done

	$LFS migrate -m $MDTIDX $migrate_dir ||
		error "fails on migrating remote dir to MDT1"

	echo "migratate to MDT1, then checking.."
	for ((i = 0; i < 10; i++)); do
		for file in $(find $migrate_dir/dir_${i}); do
			mdt_index=$($LFS getstripe -m $file)
			# broken symlink getstripe will fail
			[ $mdt_index -ne $MDTIDX ] && stat -L $file &&
				error "$file is not on MDT${MDTIDX}"
		done
	done

	# the multiple link file should still in MDT0
	mdt_index=$($LFS getstripe -m $migrate_dir/$tfile)
	[ $mdt_index == 0 ] ||
		error "$file is not on MDT${MDTIDX}"

	local new_dir_flag=$(lsattr -a $migrate_dir | awk '/\/\.$/ {print $1}')
	[ "$old_dir_flag" = "$new_dir_flag" ] ||
		error " expect $old_dir_flag get $new_dir_flag"

	local new_file_flag=$(lsattr $migrate_dir/$tfile | awk '{print $1}')
	[ "$old_file_flag" = "$new_file_flag" ] ||
		error " expect $old_file_flag get $new_file_flag"

	local new_dir_mode=$(stat -c%f $migrate_dir)
	[ "$old_dir_mode" = "$new_dir_mode" ] ||
		error "expect mode $old_dir_mode get $new_dir_mode"

	local new_file_mode=$(stat -c%f $migrate_dir/$tfile)
	[ "$old_file_mode" = "$new_file_mode" ] ||
		error "expect mode $old_file_mode get $new_file_mode"

	diff /etc/passwd $migrate_dir/$tfile ||
		error "$tfile different after migration"

	diff /etc/passwd $other_dir/luna ||
		error "luna different after migration"

	diff /etc/passwd $migrate_dir/sofia ||
		error "sofia different after migration"

	diff /etc/passwd $migrate_dir/david ||
		error "david different after migration"

	diff /etc/passwd $other_dir/zachary ||
		error "zachary different after migration"

	diff /etc/passwd $migrate_dir/${tfile}_ln ||
		error "${tfile}_ln different after migration"

	diff /etc/passwd $migrate_dir/${tfile}_ln_other ||
		error "${tfile}_ln_other different after migration"

	stripe_count=$($LFS getstripe -c $migrate_dir/dir_default_stripe2)
	[ $stripe_count = 2 ] ||
		error "dir strpe_count $d != 2 after migration."

	stripe_count=$($LFS getstripe -c $migrate_dir/${tfile}_stripe2)
	[ $stripe_count = 2 ] ||
		error "file strpe_count $d != 2 after migration."

	#migrate back to MDT0
	MDTIDX=0

	$LFS migrate -m $MDTIDX $migrate_dir ||
		error "fails on migrating remote dir to MDT0"

	echo "migrate back to MDT0, checking.."
	for file in $(find $migrate_dir); do
		mdt_index=$($LFS getstripe -m $file)
		[ $mdt_index -ne $MDTIDX ] && stat -L $file &&
			error "$file is not on MDT${MDTIDX}"
	done

	local new_dir_flag=$(lsattr -a $migrate_dir | awk '/\/\.$/ {print $1}')
	[ "$old_dir_flag" = "$new_dir_flag" ] ||
		error " expect $old_dir_flag get $new_dir_flag"

	local new_file_flag=$(lsattr $migrate_dir/$tfile | awk '{print $1}')
	[ "$old_file_flag" = "$new_file_flag" ] ||
		error " expect $old_file_flag get $new_file_flag"

	local new_dir_mode=$(stat -c%f $migrate_dir)
	[ "$old_dir_mode" = "$new_dir_mode" ] ||
		error "expect mode $old_dir_mode get $new_dir_mode"

	local new_file_mode=$(stat -c%f $migrate_dir/$tfile)
	[ "$old_file_mode" = "$new_file_mode" ] ||
		error "expect mode $old_file_mode get $new_file_mode"

	diff /etc/passwd ${migrate_dir}/$tfile ||
		error "$tfile different after migration"

	diff /etc/passwd ${other_dir}/luna ||
		error "luna different after migration"

	diff /etc/passwd ${migrate_dir}/sofia ||
		error "sofia different after migration"

	diff /etc/passwd ${other_dir}/zachary ||
		error "zachary different after migration"

	diff /etc/passwd $migrate_dir/${tfile}_ln ||
		error "${tfile}_ln different after migration"

	diff /etc/passwd $migrate_dir/${tfile}_ln_other ||
		error "${tfile}_ln_other different after migration"

	stripe_count=$($LFS getstripe -c ${migrate_dir}/dir_default_stripe2)
	[ $stripe_count = 2 ] ||
		error "dir strpe_count $d != 2 after migration."

	stripe_count=$($LFS getstripe -c ${migrate_dir}/${tfile}_stripe2)
	[ $stripe_count = 2 ] ||
		error "file strpe_count $d != 2 after migration."

	rm -rf $DIR/$tdir || error "rm dir failed after migration"
}
run_test 230b "migrate directory"

test_230c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	local MDTIDX=1
	local total=3
	local mdt_index
	local file
	local migrate_dir=$DIR/$tdir/migrate_dir

	#If migrating directory fails in the middle, all entries of
	#the directory is still accessiable.
	test_mkdir $DIR/$tdir
	test_mkdir -i0 -c1 $migrate_dir
	test_mkdir -i1 -c1 $DIR/$tdir/remote_dir
	stat $migrate_dir
	createmany -o $migrate_dir/f $total ||
		error "create files under ${migrate_dir} failed"

	# fail after migrating top dir, and this will fail only once, so the
	# first sub file migration will fail (currently f3), others succeed.
	#OBD_FAIL_MIGRATE_ENTRIES	0x1801
	do_facet mds1 lctl set_param fail_loc=0x1801
	local t=$(ls $migrate_dir | wc -l)
	$LFS migrate --mdt-index $MDTIDX $migrate_dir &&
		error "migrate should fail"
	local u=$(ls $migrate_dir | wc -l)
	[ "$u" == "$t" ] || error "$u != $t during migration"

	# add new dir/file should succeed
	mkdir $migrate_dir/dir ||
		error "mkdir failed under migrating directory"
	touch $migrate_dir/file ||
		error "create file failed under migrating directory"

	# add file with existing name should fail
	for file in $migrate_dir/f*; do
		stat $file > /dev/null || error "stat $file failed"
		$OPENFILE -f O_CREAT:O_EXCL $file &&
			error "open(O_CREAT|O_EXCL) $file should fail"
		$MULTIOP $file m && error "create $file should fail"
		touch $DIR/$tdir/remote_dir/$tfile ||
			error "touch $tfile failed"
		ln $DIR/$tdir/remote_dir/$tfile $file &&
			error "link $file should fail"
		mdt_index=$($LFS getstripe -m $file)
		if [ $mdt_index == 0 ]; then
			# file failed to migrate is not allowed to rename to
			mv $DIR/$tdir/remote_dir/$tfile $file &&
				error "rename to $file should fail"
		else
			mv $DIR/$tdir/remote_dir/$tfile $file ||
				error "rename to $file failed"
		fi
		echo hello >> $file || error "write $file failed"
	done

	# resume migration with different options should fail
	$LFS migrate -m 0 $migrate_dir &&
		error "migrate -m 0 $migrate_dir should fail"

	$LFS migrate -m $MDTIDX -c 2 $migrate_dir &&
		error "migrate -c 2 $migrate_dir should fail"

	# resume migration should succeed
	$LFS migrate -m $MDTIDX $migrate_dir ||
		error "migrate $migrate_dir failed"

	echo "Finish migration, then checking.."
	for file in $(find $migrate_dir); do
		mdt_index=$($LFS getstripe -m $file)
		[ $mdt_index == $MDTIDX ] ||
			error "$file is not on MDT${MDTIDX}"
	done

	rm -rf $DIR/$tdir || error "rm dir failed after migration"
}
run_test 230c "check directory accessiblity if migration failed"

test_230d() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"
	# LU-11235
	[ "$mds1_FSTYPE" == "zfs" ] && skip "skip ZFS backend"

	local migrate_dir=$DIR/$tdir/migrate_dir
	local old_index
	local new_index
	local old_count
	local new_count
	local new_hash
	local mdt_index
	local i
	local j

	old_index=$((RANDOM % MDSCOUNT))
	old_count=$((MDSCOUNT - old_index))
	new_index=$((RANDOM % MDSCOUNT))
	new_count=$((MDSCOUNT - new_index))
	new_hash="all_char"

	[ $old_count -gt 1 ] && old_count=$((old_count - RANDOM % old_count))
	[ $new_count -gt 1 ] && new_count=$((new_count - RANDOM % new_count))

	test_mkdir $DIR/$tdir
	test_mkdir -i $old_index -c $old_count $migrate_dir

	for ((i=0; i<100; i++)); do
		test_mkdir -i0 -c1 $migrate_dir/dir_${i}
		createmany -o $migrate_dir/dir_${i}/f 100 ||
			error "create files under remote dir failed $i"
	done

	echo -n "Migrate from MDT$old_index "
	[ $old_count -gt 1 ] && echo -n "... MDT$((old_index + old_count - 1)) "
	echo -n "to MDT$new_index"
	[ $new_count -gt 1 ] && echo -n " ... MDT$((new_index + new_count - 1))"
	echo

	echo "$LFS migrate -m$new_index -c$new_count -H $new_hash $migrate_dir"
	$LFS migrate -m $new_index -c $new_count -H $new_hash $migrate_dir ||
		error "migrate remote dir error"

	echo "Finish migration, then checking.."
	for file in $(find $migrate_dir); do
		mdt_index=$($LFS getstripe -m $file)
		if [ $mdt_index -lt $new_index ] ||
		   [ $mdt_index -gt $((new_index + new_count - 1)) ]; then
			error "$file is on MDT$mdt_index"
		fi
	done

	rm -rf $DIR/$tdir || error "rm dir failed after migration"
}
run_test 230d "check migrate big directory"

test_230e() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	local i
	local j
	local a_fid
	local b_fid

	mkdir -p $DIR/$tdir
	mkdir $DIR/$tdir/migrate_dir
	mkdir $DIR/$tdir/other_dir
	touch $DIR/$tdir/migrate_dir/a
	ln $DIR/$tdir/migrate_dir/a $DIR/$tdir/other_dir/b
	ls $DIR/$tdir/other_dir

	$LFS migrate -m 1 $DIR/$tdir/migrate_dir ||
		error "migrate dir fails"

	mdt_index=$($LFS getstripe -m $DIR/$tdir/migrate_dir)
	[ $mdt_index == 1 ] || error "migrate_dir is not on MDT1"

	mdt_index=$($LFS getstripe -m $DIR/$tdir/migrate_dir/a)
	[ $mdt_index == 0 ] || error "a is not on MDT0"

	$LFS migrate -m 1 $DIR/$tdir/other_dir ||
		error "migrate dir fails"

	mdt_index=$($LFS getstripe -m $DIR/$tdir/other_dir)
	[ $mdt_index == 1 ] || error "other_dir is not on MDT1"

	mdt_index=$($LFS getstripe -m $DIR/$tdir/migrate_dir/a)
	[ $mdt_index == 1 ] || error "a is not on MDT1"

	mdt_index=$($LFS getstripe -m $DIR/$tdir/other_dir/b)
	[ $mdt_index == 1 ] || error "b is not on MDT1"

	a_fid=$($LFS path2fid $DIR/$tdir/migrate_dir/a)
	b_fid=$($LFS path2fid $DIR/$tdir/other_dir/b)

	[ "$a_fid" = "$b_fid" ] || error "different fid after migration"

	rm -rf $DIR/$tdir || error "rm dir failed after migration"
}
run_test 230e "migrate mulitple local link files"

test_230f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	local a_fid
	local ln_fid

	mkdir -p $DIR/$tdir
	mkdir $DIR/$tdir/migrate_dir
	$LFS mkdir -i1 $DIR/$tdir/other_dir
	touch $DIR/$tdir/migrate_dir/a
	ln $DIR/$tdir/migrate_dir/a $DIR/$tdir/other_dir/ln1
	ln $DIR/$tdir/migrate_dir/a $DIR/$tdir/other_dir/ln2
	ls $DIR/$tdir/other_dir

	# a should be migrated to MDT1, since no other links on MDT0
	$LFS migrate -m 1 $DIR/$tdir/migrate_dir ||
		error "#1 migrate dir fails"
	mdt_index=$($LFS getstripe -m $DIR/$tdir/migrate_dir)
	[ $mdt_index == 1 ] || error "migrate_dir is not on MDT1"
	mdt_index=$($LFS getstripe -m $DIR/$tdir/migrate_dir/a)
	[ $mdt_index == 1 ] || error "a is not on MDT1"

	# a should stay on MDT1, because it is a mulitple link file
	$LFS migrate -m 0 $DIR/$tdir/migrate_dir ||
		error "#2 migrate dir fails"
	mdt_index=$($LFS getstripe -m $DIR/$tdir/migrate_dir/a)
	[ $mdt_index == 1 ] || error "a is not on MDT1"

	$LFS migrate -m 1 $DIR/$tdir/migrate_dir ||
		error "#3 migrate dir fails"

	a_fid=$($LFS path2fid $DIR/$tdir/migrate_dir/a)
	ln_fid=$($LFS path2fid $DIR/$tdir/other_dir/ln1)
	[ "$a_fid" = "$ln_fid" ] || error "different fid after migrate to MDT1"

	rm -rf $DIR/$tdir/other_dir/ln1 || error "unlink ln1 fails"
	rm -rf $DIR/$tdir/other_dir/ln2 || error "unlink ln2 fails"

	# a should be migrated to MDT0, since no other links on MDT1
	$LFS migrate -m 0 $DIR/$tdir/migrate_dir ||
		error "#4 migrate dir fails"
	mdt_index=$($LFS getstripe -m $DIR/$tdir/migrate_dir/a)
	[ $mdt_index == 0 ] || error "a is not on MDT0"

	rm -rf $DIR/$tdir || error "rm dir failed after migration"
}
run_test 230f "migrate mulitple remote link files"

test_230g() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	mkdir -p $DIR/$tdir/migrate_dir

	$LFS migrate -m 1000 $DIR/$tdir/migrate_dir &&
		error "migrating dir to non-exist MDT succeeds"
	true
}
run_test 230g "migrate dir to non-exist MDT"

test_230h() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	local mdt_index

	mkdir -p $DIR/$tdir/migrate_dir

	$LFS migrate -m1 $DIR &&
		error "migrating mountpoint1 should fail"

	$LFS migrate -m1 $DIR/$tdir/.. &&
		error "migrating mountpoint2 should fail"

	# same as mv
	$LFS migrate -m1 $DIR/$tdir/migrate_dir/.. &&
		error "migrating $tdir/migrate_dir/.. should fail"

	true
}
run_test 230h "migrate .. and root"

test_230i() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	mkdir -p $DIR/$tdir/migrate_dir

	$LFS migrate -m 1 $DIR/$tdir/migrate_dir/ ||
		error "migration fails with a tailing slash"

	$LFS migrate -m 0 $DIR/$tdir/migrate_dir// ||
		error "migration fails with two tailing slashes"
}
run_test 230i "lfs migrate -m tolerates trailing slashes"

test_230j() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	$LFS mkdir -m 0 -c 1 $DIR/$tdir || error "mkdir $tdir failed"
	$LFS setstripe -E 1M -L mdt $DIR/$tdir/$tfile ||
		error "create $tfile failed"
	cat /etc/passwd > $DIR/$tdir/$tfile

	$LFS migrate -m 1 $DIR/$tdir

	cmp /etc/passwd $DIR/$tdir/$tfile ||
		error "DoM file mismatch after migration"
}
run_test 230j "DoM file data not changed after dir migration"

test_230k() {
	[ $MDSCOUNT -lt 4 ] && skip "needs >= 4 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.56) ] &&
		skip "Need MDS version at least 2.11.56"

	local total=20
	local files_on_starting_mdt=0

	$LFS mkdir -i -1 -c 2 $DIR/$tdir || error "mkdir failed"
	$LFS getdirstripe $DIR/$tdir
	for i in $(seq $total); do
		echo $((i*i - i)) > $DIR/$tdir/$tfile.$i || error "write failed"
		[[ $($LFS getstripe -m $DIR/$tdir/$tfile.$i) -eq 0 ]] &&
			files_on_starting_mdt=$((files_on_starting_mdt + 1))
	done

	echo "$files_on_starting_mdt files on MDT0"

	$LFS migrate -m 1,3 $DIR/$tdir || error "migrate -m 1,3 failed"
	$LFS getdirstripe $DIR/$tdir

	files_on_starting_mdt=0
	for i in $(seq $total); do
		$(echo $((i*i - i)) | cmp $DIR/$tdir/$tfile.$i -) ||
			error "file $tfile.$i mismatch after migration"
		[[ $($LFS getstripe -m $DIR/$tdir/$tfile.$i) -eq 1 ]] &&
			files_on_starting_mdt=$((files_on_starting_mdt + 1))
	done

	echo "$files_on_starting_mdt files on MDT1 after migration"
	[[ $files_on_starting_mdt -eq $total ]] && error "all files on MDT1"

	$LFS migrate -m 0 -c 2 $DIR/$tdir || error "migrate -m 0 -c 2 failed"
	$LFS getdirstripe $DIR/$tdir

	files_on_starting_mdt=0
	for i in $(seq $total); do
		$(echo $((i*i - i)) | cmp $DIR/$tdir/$tfile.$i -) ||
			error "file $tfile.$i mismatch after 2nd migration"
		[[ $($LFS getstripe -m $DIR/$tdir/$tfile.$i) -eq 0 ]] &&
			files_on_starting_mdt=$((files_on_starting_mdt + 1))
	done

	echo "$files_on_starting_mdt files on MDT0 after 2nd migration"
	[[ $files_on_starting_mdt -eq $total ]] && error "all files on MDT0"

	true
}
run_test 230k "file data not changed after dir migration"

test_230l() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.56) ] &&
		skip "Need MDS version at least 2.11.56"

	$LFS mkdir -i 0 -c 1 $DIR/$tdir || error "mkdir failed"
	createmany -o $DIR/$tdir/f___________________________________ 1000 ||
		error "create files under remote dir failed $i"
	$LFS migrate -m 1 $DIR/$tdir || error "migrate failed"
}
run_test 230l "readdir between MDTs won't crash"

test_230m() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.11.56) ] &&
		skip "Need MDS version at least 2.11.56"

	local MDTIDX=1
	local mig_dir=$DIR/$tdir/migrate_dir
	local longstr="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	local shortstr="b"
	local val

	echo "Creating files and dirs with xattrs"
	test_mkdir $DIR/$tdir
	test_mkdir -i0 -c1 $mig_dir
	mkdir $mig_dir/dir
	setfattr -n user.attr1 -v $longstr $mig_dir/dir ||
		error "cannot set xattr attr1 on dir"
	setfattr -n user.attr2 -v $shortstr $mig_dir/dir ||
		error "cannot set xattr attr2 on dir"
	touch $mig_dir/dir/f0
	setfattr -n user.attr1 -v $longstr $mig_dir/dir/f0 ||
		error "cannot set xattr attr1 on file"
	setfattr -n user.attr2 -v $shortstr $mig_dir/dir/f0 ||
		error "cannot set xattr attr2 on file"
	sync ; sync ; echo 3 > /proc/sys/vm/drop_caches
	val=$(getfattr --only-values -n user.attr1 $mig_dir/dir 2>/dev/null)
	[ "$val" = $longstr ] || error "xattr attr1 not set properly on dir"
	val=$(getfattr --only-values -n user.attr2 $mig_dir/dir 2>/dev/null)
	[ "$val" = $shortstr ] || error "xattr attr2 not set properly on dir"
	val=$(getfattr --only-values -n user.attr1 $mig_dir/dir/f0 2>/dev/null)
	[ "$val" = $longstr ] || error "xattr attr1 not set properly on file"
	val=$(getfattr --only-values -n user.attr2 $mig_dir/dir/f0 2>/dev/null)
	[ "$val" = $shortstr ] || error "xattr attr2 not set properly on file"

	echo "Migrating to MDT1"
	$LFS migrate -m $MDTIDX $mig_dir ||
		error "fails on migrating dir to MDT1"

	sync ; sync ; echo 3 > /proc/sys/vm/drop_caches
	echo "Checking xattrs"
	val=$(getfattr --only-values -n user.attr1 $mig_dir/dir 2>/dev/null)
	[ "$val" = $longstr ] ||
		error "expecting xattr1 $longstr on dir, found $val"
	val=$(getfattr --only-values -n user.attr2 $mig_dir/dir 2>/dev/null)
	[ "$val" = $shortstr ] ||
		error "expecting xattr2 $shortstr on dir, found $val"
	val=$(getfattr --only-values -n user.attr1 $mig_dir/dir/f0 2>/dev/null)
	[ "$val" = $longstr ] ||
		error "expecting xattr1 $longstr on file, found $val"
	val=$(getfattr --only-values -n user.attr2 $mig_dir/dir/f0 2>/dev/null)
	[ "$val" = $shortstr ] ||
		error "expecting xattr2 $shortstr on file, found $val"
}
run_test 230m "xattrs not changed after dir migration"

test_230r() {
	[[ $PARALLEL != "yes" ]] || skip "skip parallel run"
	[[ $MDSCOUNT -ge 2 ]] || skip_env "needs >= 2 MDTs"
	[[ $MDS1_VERSION -ge $(version_code 2.12.5) ]] ||
		skip "Need MDS version at least 2.12.5"

	# maximum amount of local locks:
	# parent striped dir - 2 locks
	# new stripe in parent to migrate to - 1 lock
	# source and target - 2 locks
	# Total 5 locks for regular file
	mkdir -p $DIR/$tdir
	$LFS mkdir -i1 -c2 $DIR/$tdir/dir1
	touch $DIR/$tdir/dir1/eee

	# create 4 hardlink for 4 more locks
	# Total: 9 locks > RS_MAX_LOCKS (8)
	$LFS mkdir -i1 -c1 $DIR/$tdir/dir2
	$LFS mkdir -i1 -c1 $DIR/$tdir/dir3
	$LFS mkdir -i1 -c1 $DIR/$tdir/dir4
	$LFS mkdir -i1 -c1 $DIR/$tdir/dir5
	ln $DIR/$tdir/dir1/eee $DIR/$tdir/dir2/eee
	ln $DIR/$tdir/dir1/eee $DIR/$tdir/dir3/eee
	ln $DIR/$tdir/dir1/eee $DIR/$tdir/dir4/eee
	ln $DIR/$tdir/dir1/eee $DIR/$tdir/dir5/eee

	cancel_lru_locks mdc

	$LFS migrate -m1 -c1 $DIR/$tdir/dir1 ||
		error "migrate dir fails"

	rm -rf $DIR/$tdir || error "rm dir failed after migration"
}
run_test 230r "migrate with too many local locks"

test_231a()
{
	# For simplicity this test assumes that max_pages_per_rpc
	# is the same across all OSCs
	local max_pages=$($LCTL get_param -n osc.*.max_pages_per_rpc | head -n1)
	local bulk_size=$((max_pages * PAGE_SIZE))
	local brw_size=$(do_facet ost1 $LCTL get_param -n obdfilter.*.brw_size |
				       head -n 1)

	mkdir -p $DIR/$tdir
	$LFS setstripe -S ${brw_size}M $DIR/$tdir ||
		error "failed to set stripe with -S ${brw_size}M option"

	# clear the OSC stats
	$LCTL set_param osc.*.stats=0 &>/dev/null
	stop_writeback

	# Client writes $bulk_size - there must be 1 rpc for $max_pages.
	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=$bulk_size count=1 \
		oflag=direct &>/dev/null || error "dd failed"

	sync; sleep 1; sync # just to be safe
	local nrpcs=$($LCTL get_param osc.*.stats |awk '/ost_write/ {print $2}')
	if [ x$nrpcs != "x1" ]; then
		$LCTL get_param osc.*.stats
		error "found $nrpcs ost_write RPCs, not 1 as expected"
	fi

	start_writeback
	# Drop the OSC cache, otherwise we will read from it
	cancel_lru_locks osc

	# clear the OSC stats
	$LCTL set_param osc.*.stats=0 &>/dev/null

	# Client reads $bulk_size.
	dd if=$DIR/$tdir/$tfile of=/dev/null bs=$bulk_size count=1 \
		iflag=direct &>/dev/null || error "dd failed"

	nrpcs=$($LCTL get_param osc.*.stats | awk '/ost_read/ { print $2 }')
	if [ x$nrpcs != "x1" ]; then
		$LCTL get_param osc.*.stats
		error "found $nrpcs ost_read RPCs, not 1 as expected"
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

test_232a() {
	mkdir -p $DIR/$tdir
	$LFS setstripe -c1 -i0 $DIR/$tdir/$tfile

	#define OBD_FAIL_LDLM_OST_LVB		 0x31c
	do_facet ost1 $LCTL set_param fail_loc=0x31c

	# ignore dd failure
	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=1M count=1 || true

	do_facet ost1 $LCTL set_param fail_loc=0
	umount_client $MOUNT || error "umount failed"
	mount_client $MOUNT || error "mount failed"
	stop ost1 || error "cannot stop ost1"
	start ost1 $(ostdevname 1) $OST_MOUNT_OPTS || error "cannot start ost1"
}
run_test 232a "failed lock should not block umount"

test_232b() {
	[ $MDS1_VERSION -ge $(version_code 2.10.58) ] ||
		skip "Need MDS version at least 2.10.58"

	mkdir -p $DIR/$tdir
	$LFS setstripe -c1 -i0 $DIR/$tdir/$tfile
	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=1M count=1
	sync
	cancel_lru_locks osc

	#define OBD_FAIL_LDLM_OST_LVB		 0x31c
	do_facet ost1 $LCTL set_param fail_loc=0x31c

	# ignore failure
	$LFS data_version $DIR/$tdir/$tfile || true

	do_facet ost1 $LCTL set_param fail_loc=0
	umount_client $MOUNT || error "umount failed"
	mount_client $MOUNT || error "mount failed"
	stop ost1 || error "cannot stop ost1"
	start ost1 $(ostdevname 1) $OST_MOUNT_OPTS || error "cannot start ost1"
}
run_test 232b "failed data version lock should not block umount"

test_233a() {
	[ $MDS1_VERSION -ge $(version_code 2.3.64) ] ||
		skip "Need MDS version at least 2.3.64"
	[ -n "$FILESET" ] && skip_env "SKIP due to FILESET set"

	local fid=$($LFS path2fid $MOUNT)

	stat $MOUNT/.lustre/fid/$fid > /dev/null ||
		error "cannot access $MOUNT using its FID '$fid'"
}
run_test 233a "checking that OBF of the FS root succeeds"

test_233b() {
	[ $MDS1_VERSION -ge $(version_code 2.5.90) ] ||
		skip "Need MDS version at least 2.5.90"
	[ -n "$FILESET" ] && skip_env "SKIP due to FILESET set"

	local fid=$($LFS path2fid $MOUNT/.lustre)

	stat $MOUNT/.lustre/fid/$fid > /dev/null ||
		error "cannot access $MOUNT/.lustre using its FID '$fid'"

	fid=$($LFS path2fid $MOUNT/.lustre/fid)
	stat $MOUNT/.lustre/fid/$fid > /dev/null ||
		error "cannot access $MOUNT/.lustre/fid using its FID '$fid'"
}
run_test 233b "checking that OBF of the FS .lustre succeeds"

test_234() {
	local p="$TMP/sanityN-$TESTNAME.parameters"
	save_lustre_params client "llite.*.xattr_cache" > $p
	lctl set_param llite.*.xattr_cache 1 ||
		skip_env "xattr cache is not supported"

	mkdir -p $DIR/$tdir || error "mkdir failed"
	touch $DIR/$tdir/$tfile || error "touch failed"
	# OBD_FAIL_LLITE_XATTR_ENOMEM
	$LCTL set_param fail_loc=0x1405
	getfattr -n user.attr $DIR/$tdir/$tfile &&
		error "getfattr should have failed with ENOMEM"
	$LCTL set_param fail_loc=0x0
	rm -rf $DIR/$tdir

	restore_lustre_params < $p
	rm -f $p
}
run_test 234 "xattr cache should not crash on ENOMEM"

test_235() {
	[ $MDS1_VERSION -lt $(version_code 2.4.52) ] &&
		skip "Need MDS version at least 2.4.52"

	flock_deadlock $DIR/$tfile
	local RC=$?
	case $RC in
		0)
		;;
		124) error "process hangs on a deadlock"
		;;
		*) error "error executing flock_deadlock $DIR/$tfile"
		;;
	esac
}
run_test 235 "LU-1715: flock deadlock detection does not work properly"

#LU-2935
test_236() {
	check_swap_layouts_support

	local ref1=/etc/passwd
	local ref2=/etc/group
	local file1=$DIR/$tdir/f1
	local file2=$DIR/$tdir/f2

	test_mkdir -c1 $DIR/$tdir
	$SETSTRIPE -c 1 $file1 || error "cannot setstripe on '$file1': rc = $?"
	cp $ref1 $file1 || error "cp $ref1 $file1 failed: rc = $?"
	$SETSTRIPE -c 2 $file2 || error "cannot setstripe on '$file2': rc = $?"
	cp $ref2 $file2 || error "cp $ref2 $file2 failed: rc = $?"
	local fd=$(free_fd)
	local cmd="exec $fd<>$file2"
	eval $cmd
	rm $file2
	$LFS swap_layouts $file1 /proc/self/fd/${fd} ||
		error "cannot swap layouts of '$file1' and /proc/self/fd/${fd}"
	cmd="exec $fd>&-"
	eval $cmd
	cmp $ref2 $file1 || error "content compare failed ($ref2 != $file1)"

	#cleanup
	rm -rf $DIR/$tdir
}
run_test 236 "Layout swap on open unlinked file"

# LU-4659 linkea consistency
test_238() {
	[[ $MDS1_VERSION -gt $(version_code 2.5.57) ]] ||
		[[ $MDS1_VERSION -gt $(version_code 2.5.1) &&
		   $MDS1_VERSION -lt $(version_code 2.5.50) ]] ||
		skip "Need MDS version at least 2.5.58 or 2.5.2+"

	touch $DIR/$tfile
	ln $DIR/$tfile $DIR/$tfile.lnk
	touch $DIR/$tfile.new
	mv $DIR/$tfile.new $DIR/$tfile
	local fid1=$($LFS path2fid $DIR/$tfile)
	local fid2=$($LFS path2fid $DIR/$tfile.lnk)
	local path1=$($LFS fid2path $FSNAME "$fid1")
	[ $tfile == $path1 ] || error "linkea inconsistent: $tfile $fid1 $path1"
	local path2=$($LFS fid2path $FSNAME "$fid2")
	[ $tfile.lnk == $path2 ] ||
		error "linkea inconsistent: $tfile.lnk $fid2 $path2!"
	rm -f $DIR/$tfile*
}
run_test 238 "Verify linkea consistency"

test_239A() { # was test_239
	[ $MDS1_VERSION -lt $(version_code 2.5.60) ] &&
		skip "Need MDS version at least 2.5.60"

	local list=$(comma_list $(mdts_nodes))

	mkdir -p $DIR/$tdir
	createmany -o $DIR/$tdir/f- 5000
	unlinkmany $DIR/$tdir/f- 5000
	[ $MDS1_VERSION -gt $(version_code 2.10.4) ] &&
		do_nodes $list "lctl set_param -n osp.*.force_sync=1"
	changes=$(do_nodes $list "lctl get_param -n osp.*MDT*.sync_changes \
			osp.*MDT*.sync_in_flight" | calc_sum)
	[ "$changes" -eq 0 ] || error "$changes not synced"
}
run_test 239A "osp_sync test"

test_239a() { #LU-5297
	remote_mds_nodsh && skip "remote MDS with nodsh"

	touch $DIR/$tfile
	#define OBD_FAIL_OSP_CHECK_INVALID_REC     0x2100
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0x2100
	chgrp $RUNAS_GID $DIR/$tfile
	wait_delete_completed
}
run_test 239a "process invalid osp sync record correctly"

test_239b() { #LU-5297
	remote_mds_nodsh && skip "remote MDS with nodsh"

	touch $DIR/$tfile1
	#define OBD_FAIL_OSP_CHECK_ENOMEM     0x2101
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0x2101
	chgrp $RUNAS_GID $DIR/$tfile1
	wait_delete_completed
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0
	touch $DIR/$tfile2
	chgrp $RUNAS_GID $DIR/$tfile2
	wait_delete_completed
}
run_test 239b "process osp sync record with ENOMEM error correctly"

test_240() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	mkdir -p $DIR/$tdir

	$LFS mkdir -i 0 $DIR/$tdir/d0 ||
		error "failed to mkdir $DIR/$tdir/d0 on MDT0"
	$LFS mkdir -i 1 $DIR/$tdir/d0/d1 ||
		error "failed to mkdir $DIR/$tdir/d0/d1 on MDT1"

	umount_client $MOUNT || error "umount failed"
	#define OBD_FAIL_TGT_DELAY_CONDITIONAL	 0x713
	do_facet mds2 lctl set_param fail_loc=0x713 fail_val=1
	mount_client $MOUNT || error "failed to mount client"

	echo "stat $DIR/$tdir/d0/d1, should not fail/ASSERT"
	stat $DIR/$tdir/d0/d1 || error "fail to stat $DIR/$tdir/d0/d1"
}
run_test 240 "race between ldlm enqueue and the connection RPC (no ASSERT)"

test_241_bio() {
	local count=$1
	local bsize=$2

	for LOOP in $(seq $count); do
		dd if=$DIR/$tfile of=/dev/null bs=$bsize count=1 2>/dev/null
		cancel_lru_locks $OSC || true
	done
}

test_241_dio() {
	local count=$1
	local bsize=$2

	for LOOP in $(seq $1); do
		dd if=$DIR/$tfile of=/dev/null bs=$bsize count=1 iflag=direct \
			2>/dev/null
	done
}

test_241a() { # was test_241
	local bsize=$PAGE_SIZE

	(( bsize < 40960 )) && bsize=40960
	dd if=/dev/zero of=$DIR/$tfile count=1 bs=$bsize
	ls -la $DIR/$tfile
	cancel_lru_locks $OSC
	test_241_bio 1000 $bsize &
	PID=$!
	test_241_dio 1000 $bsize
	wait $PID
}
run_test 241a "bio vs dio"

test_241b() {
	local bsize=$PAGE_SIZE

	(( bsize < 40960 )) && bsize=40960
	dd if=/dev/zero of=$DIR/$tfile count=1 bs=$bsize
	ls -la $DIR/$tfile
	test_241_dio 1000 $bsize &
	PID=$!
	test_241_dio 1000 $bsize
	wait $PID
}
run_test 241b "dio vs dio"

test_242() {
	remote_mds_nodsh && skip "remote MDS with nodsh"

	mkdir -p $DIR/$tdir
	touch $DIR/$tdir/$tfile

	#define OBD_FAIL_MDS_READPAGE_PACK	0x105
	do_facet mds1 lctl set_param fail_loc=0x105
	/bin/ls $DIR/$tdir && error "ls $DIR/$tdir should fail"

	do_facet mds1 lctl set_param fail_loc=0
	/bin/ls $DIR/$tdir || error "ls $DIR/$tdir failed"
}
run_test 242 "mdt_readpage failure should not cause directory unreadable"

test_243()
{
	test_mkdir $DIR/$tdir
	group_lock_test -d $DIR/$tdir || error "A group lock test failed"
}
run_test 243 "various group lock tests"

test_244()
{
	test_mkdir $DIR/$tdir
	dd if=/dev/zero of=$DIR/$tdir/$tfile bs=1M count=35
	sendfile_grouplock $DIR/$tdir/$tfile || \
		error "sendfile+grouplock failed"
	rm -rf $DIR/$tdir
}
run_test 244 "sendfile with group lock tests"

test_245() {
	local flagname="multi_mod_rpcs"
	local connect_data_name="max_mod_rpcs"
	local out

	# check if multiple modify RPCs flag is set
	out=$($LCTL get_param mdc.$FSNAME-MDT0000-*.import |
		grep "connect_flags:")
	echo "$out"

	echo "$out" | grep -qw $flagname
	if [ $? -ne 0 ]; then
		echo "connect flag $flagname is not set"
		return
	fi

	# check if multiple modify RPCs data is set
	out=$($LCTL get_param mdc.$FSNAME-MDT0000-*.import)
	echo "$out"

	echo "$out" | grep -qw $connect_data_name ||
		error "import should have connect data $connect_data_name"
}
run_test 245 "check mdc connection flag/data: multiple modify RPCs"

test_246() { # LU-7371
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ $OST1_VERSION -lt $(version_code 2.7.62) ] &&
		skip "Need OST version >= 2.7.62"

	do_facet ost1 $LCTL set_param fail_val=4095
#define OBD_FAIL_OST_READ_SIZE		0x234
	do_facet ost1 $LCTL set_param fail_loc=0x234
	$LFS setstripe $DIR/$tfile -i 0 -c 1
	dd if=/dev/zero of=$DIR/$tfile bs=4095 count=1 > /dev/null 2>&1
	cancel_lru_locks $FSNAME-OST0000
	dd if=$DIR/$tfile of=/dev/null bs=1048576 || error "Read failed"
}
run_test 246 "Read file of size 4095 should return right length"

cleanup_247() {
	local submount=$1

	trap 0
	umount_client $submount
	rmdir $submount
}

test_247a() {
	lctl get_param -n mdc.$FSNAME-MDT0000*.import |
		grep -q subtree ||
		skip_env "Fileset feature is not supported"

	local submount=${MOUNT}_$tdir

	mkdir $MOUNT/$tdir
	mkdir -p $submount || error "mkdir $submount failed"
	FILESET="$FILESET/$tdir" mount_client $submount ||
		error "mount $submount failed"
	trap "cleanup_247 $submount" EXIT
	echo foo > $submount/$tfile || error "write $submount/$tfile failed"
	[ $(cat $MOUNT/$tdir/$tfile) = "foo" ] ||
		error "read $MOUNT/$tdir/$tfile failed"
	cleanup_247 $submount
}
run_test 247a "mount subdir as fileset"

test_247b() {
	lctl get_param -n mdc.$FSNAME-MDT0000*.import | grep -q subtree ||
		skip_env "Fileset feature is not supported"

	local submount=${MOUNT}_$tdir

	rm -rf $MOUNT/$tdir
	mkdir -p $submount || error "mkdir $submount failed"
	SKIP_FILESET=1
	FILESET="$FILESET/$tdir" mount_client $submount &&
		error "mount $submount should fail"
	rmdir $submount
}
run_test 247b "mount subdir that dose not exist"

test_247c() {
	lctl get_param -n mdc.$FSNAME-MDT0000*.import | grep -q subtree ||
		skip_env "Fileset feature is not supported"

	local submount=${MOUNT}_$tdir

	mkdir -p $MOUNT/$tdir/dir1
	mkdir -p $submount || error "mkdir $submount failed"
	trap "cleanup_247 $submount" EXIT
	FILESET="$FILESET/$tdir" mount_client $submount ||
		error "mount $submount failed"
	local fid=$($LFS path2fid $MOUNT/)
	$LFS fid2path $submount $fid && error "fid2path should fail"
	cleanup_247 $submount
}
run_test 247c "running fid2path outside root"

test_247d() {
	lctl get_param -n mdc.$FSNAME-MDT0000*.import | grep -q subtree ||
		skip "Fileset feature is not supported"

	local submount=${MOUNT}_$tdir

	mkdir -p $MOUNT/$tdir/dir1
	mkdir -p $submount || error "mkdir $submount failed"
	FILESET="$FILESET/$tdir" mount_client $submount ||
		error "mount $submount failed"
	trap "cleanup_247 $submount" EXIT
	local fid=$($LFS path2fid $submount/dir1)
	$LFS fid2path $submount $fid || error "fid2path should succeed"
	cleanup_247 $submount
}
run_test 247d "running fid2path inside root"

# LU-8037
test_247e() {
	lctl get_param -n mdc.$FSNAME-MDT0000*.import |
		grep -q subtree ||
		skip "Fileset feature is not supported"

	local submount=${MOUNT}_$tdir

	mkdir $MOUNT/$tdir
	mkdir -p $submount || error "mkdir $submount failed"
	FILESET="$FILESET/.." mount_client $submount &&
		error "mount $submount should fail"
	rmdir $submount
}
run_test 247e "mount .. as fileset"

test_248() {
	local fast_read_sav=$($LCTL get_param -n llite.*.fast_read 2>/dev/null)
	[ -z "$fast_read_sav" ] && skip "no fast read support"

	# create a large file for fast read verification
	dd if=/dev/zero of=$DIR/$tfile bs=1M count=128 > /dev/null 2>&1

	# make sure the file is created correctly
	$CHECKSTAT -s $((128*1024*1024)) $DIR/$tfile ||
		{ rm -f $DIR/$tfile; skip "file creation error"; }

	echo "Test 1: verify that fast read is 4 times faster on cache read"

	# small read with fast read enabled
	$LCTL set_param -n llite.*.fast_read=1
	local t_fast=$(dd if=$DIR/$tfile of=/dev/null bs=4k 2>&1 |
		egrep -o '([[:digit:]\.\,e-]+) s' | cut -d's' -f1 |
		sed -e 's/,/./' -e 's/[eE]+*/\*10\^/')
	# small read with fast read disabled
	$LCTL set_param -n llite.*.fast_read=0
	local t_slow=$(dd if=$DIR/$tfile of=/dev/null bs=4k 2>&1 |
		egrep -o '([[:digit:]\.\,e-]+) s' | cut -d's' -f1 |
		sed -e 's/,/./' -e 's/[eE]+*/\*10\^/')

	# verify that fast read is 4 times faster for cache read
	[ $(bc <<< "4 * $t_fast < $t_slow") -eq 1 ] ||
		error_not_in_vm "fast read was not 4 times faster: " \
			   "$t_fast vs $t_slow"

	echo "Test 2: verify the performance between big and small read"
	$LCTL set_param -n llite.*.fast_read=1

	# 1k non-cache read
	cancel_lru_locks osc
	local t_1k=$(dd if=$DIR/$tfile of=/dev/null bs=1k 2>&1 |
		egrep -o '([[:digit:]\.\,e-]+) s' | cut -d's' -f1 |
		sed -e 's/,/./' -e 's/[eE]+*/\*10\^/')

	# 1M non-cache read
	cancel_lru_locks osc
	local t_1m=$(dd if=$DIR/$tfile of=/dev/null bs=1k 2>&1 |
		egrep -o '([[:digit:]\.\,e-]+) s' | cut -d's' -f1 |
		sed -e 's/,/./' -e 's/[eE]+*/\*10\^/')

	# verify that big IO is not 4 times faster than small IO
	[ $(bc <<< "4 * $t_1k >= $t_1m") -eq 1 ] ||
		error_not_in_vm "bigger IO is way too fast: $t_1k vs $t_1m"

	$LCTL set_param -n llite.*.fast_read=$fast_read_sav
	rm -f $DIR/$tfile
}
run_test 248 "fast read verification"

test_249() { # LU-7890
	[ $MDS1_VERSION -lt $(version_code 2.8.53) ] &&
		skip "Need at least version 2.8.54"

	rm -f $DIR/$tfile
	$SETSTRIPE -c 1 $DIR/$tfile
	# Offset 2T == 4k * 512M
	dd if=/dev/zero of=$DIR/$tfile bs=4k count=1 seek=512M ||
		error "dd to 2T offset failed"
}
run_test 249 "Write above 2T file size"

test_250() {
	[ "$(facet_fstype ost$(($($GETSTRIPE -i $DIR/$tfile) + 1)))" = "zfs" ] \
	 && skip "no 16TB file size limit on ZFS"

	$SETSTRIPE -c 1 $DIR/$tfile
	# ldiskfs extent file size limit is (16TB - 4KB - 1) bytes
	local size=$((16 * 1024 * 1024 * 1024 * 1024 - 4096 - 1))
	$TRUNCATE $DIR/$tfile $size || error "truncate $tfile to $size failed"
	dd if=/dev/zero of=$DIR/$tfile bs=10 count=1 oflag=append \
		conv=notrunc,fsync && error "append succeeded"
	return 0
}
run_test 250 "Write above 16T limit"

test_251() {
	$SETSTRIPE -c -1 -S 1048576 $DIR/$tfile

	#define OBD_FAIL_LLITE_LOST_LAYOUT 0x1407
	#Skip once - writing the first stripe will succeed
	$LCTL set_param fail_loc=0xa0001407 fail_val=1
	$MULTIOP $DIR/$tfile o:O_RDWR:w2097152c 2>&1 | grep -q "short write" &&
		error "short write happened"

	$LCTL set_param fail_loc=0xa0001407 fail_val=1
	$MULTIOP $DIR/$tfile or2097152c 2>&1 | grep -q "short read" &&
		error "short read happened"

	rm -f $DIR/$tfile
}
run_test 251 "Handling short read and write correctly"

test_252() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_ost_nodsh && skip "remote OST with nodsh"
	if [ "$ost1_FSTYPE" != "ldiskfs" -o "$mds1_FSTYPE" != "ldiskfs" ]; then
		skip_env "ldiskfs only test"
	fi

	local tgt
	local dev
	local out
	local uuid
	local num
	local gen

	# check lr_reader on OST0000
	tgt=ost1
	dev=$(facet_device $tgt)
	out=$(do_facet $tgt $LR_READER $dev)
	[ $? -eq 0 ] || error "$LR_READER failed on target $tgt device $dev"
	echo "$out"
	uuid=$(echo "$out" | grep -i uuid | awk '{ print $2 }')
	[ "$uuid" == "$(ostuuid_from_index 0)" ] ||
		error "Invalid uuid returned by $LR_READER on target $tgt"
	echo -e "uuid returned by $LR_READER is '$uuid'\n"

	# check lr_reader -c on MDT0000
	tgt=mds1
	dev=$(facet_device $tgt)
	if ! do_facet $tgt $LR_READER -h | grep -q OPTIONS; then
		skip "$LR_READER does not support additional options"
	fi
	out=$(do_facet $tgt $LR_READER -c $dev)
	[ $? -eq 0 ] || error "$LR_READER failed on target $tgt device $dev"
	echo "$out"
	num=$(echo "$out" | grep -c "mdtlov")
	[ "$num" -eq $((MDSCOUNT - 1)) ] ||
		error "Invalid number of mdtlov clients returned by $LR_READER"
	echo -e "Number of mdtlov clients returned by $LR_READER is '$num'\n"

	# check lr_reader -cr on MDT0000
	out=$(do_facet $tgt $LR_READER -cr $dev)
	[ $? -eq 0 ] || error "$LR_READER failed on target $tgt device $dev"
	echo "$out"
	echo "$out" | grep -q "^reply_data:$" ||
		error "$LR_READER should have returned 'reply_data' section"
	num=$(echo "$out" | grep -c "client_generation")
	echo -e "Number of reply data returned by $LR_READER is '$num'\n"
}
run_test 252 "check lr_reader tool"

test_253_fill_ost() {
	local size_mb #how many MB should we write to pass watermark
	local lwm=$3  #low watermark
	local free_10mb #10% of free space

	free_kb=$($LFS df $MOUNT | grep $1 | awk '{ print $4 }')
	size_mb=$((free_kb / 1024 - lwm))
	free_10mb=$((free_kb / 10240))
	#If 10% of free space cross low watermark use it
	if (( free_10mb > size_mb )); then
		size_mb=$free_10mb
	else
		#At least we need to store 1.1 of difference between
		#free space and low watermark
		size_mb=$((size_mb + size_mb / 10))
	fi
	if (( lwm <= $((free_kb / 1024)) )) || [ ! -f $DIR/$tdir/1 ]; then
		dd if=/dev/zero of=$DIR/$tdir/1 bs=1M count=$size_mb \
			 oflag=append conv=notrunc
	fi

	sleep_maxage

	free_kb=$($LFS df $MOUNT | grep $1 | awk '{ print $4 }')
	echo "OST still has $((free_kb / 1024)) mbytes free"
}

test_253() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	remote_mgs_nodsh && skip "remote MGS with nodsh"

	local ostidx=0
	local rc=0

	local ost_name=$($LFS osts |
		sed -n 's/^'$ostidx': \(.*\)_UUID .*/\1/p')
	# on the mdt's osc
	local mdtosc_proc1=$(get_mdtosc_proc_path $SINGLEMDS $ost_name)
	do_facet $SINGLEMDS $LCTL get_param -n \
		osp.$mdtosc_proc1.reserved_mb_high ||
		skip  "remote MDS does not support reserved_mb_high"

	rm -rf $DIR/$tdir
	wait_mds_ost_sync
	wait_delete_completed
	mkdir $DIR/$tdir

	local last_wm_h=$(do_facet $SINGLEMDS $LCTL get_param -n \
			osp.$mdtosc_proc1.reserved_mb_high)
	local last_wm_l=$(do_facet $SINGLEMDS $LCTL get_param -n \
			osp.$mdtosc_proc1.reserved_mb_low)
	echo "prev high watermark $last_wm_h, prev low watermark $last_wm_l"

	create_pool $FSNAME.$TESTNAME || error "Pool creation failed"
	do_facet mgs $LCTL pool_add $FSNAME.$TESTNAME $ost_name ||
		error "Adding $ost_name to pool failed"

	# Wait for client to see a OST at pool
	wait_update $HOSTNAME "$LCTL get_param -n
		lov.$FSNAME-*.pools.$TESTNAME | sort -u |
		grep $ost_name" "$ost_name""_UUID" $((TIMEOUT/2)) ||
		error "Client can not see the pool"
	$SETSTRIPE $DIR/$tdir -i $ostidx -c 1 -p $FSNAME.$TESTNAME ||
		error "Setstripe failed"

	dd if=/dev/zero of=$DIR/$tdir/0 bs=1M count=10
	local blocks=$($LFS df $MOUNT | grep $ost_name | awk '{ print $4 }')
	echo "OST still has $((blocks/1024)) mbytes free"

	local new_lwm=$((blocks/1024-10))
	do_facet $SINGLEMDS $LCTL set_param \
			osp.$mdtosc_proc1.reserved_mb_high=$((new_lwm+5))
	do_facet $SINGLEMDS $LCTL set_param \
			osp.$mdtosc_proc1.reserved_mb_low=$new_lwm

	test_253_fill_ost $ost_name $mdtosc_proc1 $new_lwm

	#First enospc could execute orphan deletion so repeat.
	test_253_fill_ost $ost_name $mdtosc_proc1 $new_lwm

	local oa_status=$(do_facet $SINGLEMDS $LCTL get_param -n \
			osp.$mdtosc_proc1.prealloc_status)
	echo "prealloc_status $oa_status"

	dd if=/dev/zero of=$DIR/$tdir/2 bs=1M count=1 &&
		error "File creation should fail"
	#object allocation was stopped, but we still able to append files
	dd if=/dev/zero of=$DIR/$tdir/1 bs=1M seek=6 count=5 oflag=append ||
		error "Append failed"
	rm -f $DIR/$tdir/1 $DIR/$tdir/0 $DIR/$tdir/r*

	wait_delete_completed

	sleep_maxage

	for i in $(seq 10 12); do
		dd if=/dev/zero of=$DIR/$tdir/$i bs=1M count=1 2>/dev/null ||
			error "File creation failed after rm";
	done

	oa_status=$(do_facet $SINGLEMDS $LCTL get_param -n \
			osp.$mdtosc_proc1.prealloc_status)
	echo "prealloc_status $oa_status"

	if (( oa_status != 0 )); then
		error "Object allocation still disable after rm"
	fi
	do_facet $SINGLEMDS $LCTL set_param \
			osp.$mdtosc_proc1.reserved_mb_high=$last_wm_h
	do_facet $SINGLEMDS $LCTL set_param \
			osp.$mdtosc_proc1.reserved_mb_low=$last_wm_l


	do_facet mgs $LCTL pool_remove $FSNAME.$TESTNAME $ost_name ||
		error "Remove $ost_name from pool failed"
	do_facet mgs $LCTL pool_destroy $FSNAME.$TESTNAME ||
		error "Pool destroy fialed"
}
run_test 253 "Check object allocation limit"

test_254() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	do_facet $SINGLEMDS $LCTL get_param -n mdd.$MDT0.changelog_size ||
		skip "MDS does not support changelog_size"

	local cl_user
	local MDT0=$(facet_svc $SINGLEMDS)

	changelog_register || error "changelog_register failed"

	changelog_clear 0 || error "changelog_clear failed"

	local size1=$(do_facet $SINGLEMDS \
		      $LCTL get_param -n mdd.$MDT0.changelog_size)
	echo "Changelog size $size1"

	rm -rf $DIR/$tdir
	$LFS mkdir -i 0 $DIR/$tdir
	# change something
	mkdir -p $DIR/$tdir/pics/2008/zachy
	touch $DIR/$tdir/pics/2008/zachy/timestamp
	cp /etc/hosts $DIR/$tdir/pics/2008/zachy/pic1.jpg
	mv $DIR/$tdir/pics/2008/zachy $DIR/$tdir/pics/zach
	ln $DIR/$tdir/pics/zach/pic1.jpg $DIR/$tdir/pics/2008/portland.jpg
	ln -s $DIR/$tdir/pics/2008/portland.jpg $DIR/$tdir/pics/desktop.jpg
	rm $DIR/$tdir/pics/desktop.jpg

	local size2=$(do_facet $SINGLEMDS \
		      $LCTL get_param -n mdd.$MDT0.changelog_size)
	echo "Changelog size after work $size2"

	(( $size2 > $size1 )) ||
		error "new Changelog size=$size2 less than old size=$size1"
}
run_test 254 "Check changelog size"

ladvise_no_type()
{
	local type=$1
	local file=$2

	lfs ladvise -a invalid $file 2>&1 | grep "Valid types" |
		awk -F: '{print $2}' | grep $type > /dev/null
	if [ $? -ne 0 ]; then
		return 0
	fi
	return 1
}

ladvise_no_ioctl()
{
	local file=$1

	lfs ladvise -a willread $file > /dev/null 2>&1
	if [ $? -eq 0 ]; then
		return 1
	fi

	lfs ladvise -a willread $file 2>&1 |
		grep "Inappropriate ioctl for device" > /dev/null
	if [ $? -eq 0 ]; then
		return 0
	fi
	return 1
}

percent() {
	bc <<<"scale=2; ($1 - $2) * 100 / $2"
}

# run a random read IO workload
# usage: random_read_iops <filename> <filesize> <iosize>
random_read_iops() {
	local file=$1
	local fsize=$2
	local iosize=${3:-4096}

	$READS -f $file -s $fsize -b $iosize -n $((fsize / iosize)) -t 60 |
		sed -e '/^$/d' -e 's#.*s, ##' -e 's#MB/s##'
}

drop_file_oss_cache() {
	local file="$1"
	local nodes="$2"

	$LFS ladvise -a dontneed $file 2>/dev/null ||
		do_nodes $nodes "echo 3 > /proc/sys/vm/drop_caches"
}

ladvise_willread_performance()
{
	local repeat=10
	local average_origin=0
	local average_cache=0
	local average_ladvise=0

	for ((i = 1; i <= $repeat; i++)); do
		echo "Iter $i/$repeat: reading without willread hint"
		cancel_lru_locks osc
		drop_file_oss_cache $DIR/$tfile $(comma_list $(osts_nodes))
		local speed_origin=$(random_read_iops $DIR/$tfile $size)
		echo "Iter $i/$repeat: uncached speed: $speed_origin"
		average_origin=$(bc <<<"$average_origin + $speed_origin")

		cancel_lru_locks osc
		local speed_cache=$(random_read_iops $DIR/$tfile $size)
		echo "Iter $i/$repeat: OSS cache speed: $speed_cache"
		average_cache=$(bc <<<"$average_cache + $speed_cache")

		cancel_lru_locks osc
		drop_file_oss_cache $DIR/$tfile $(comma_list $(osts_nodes))
		$LFS ladvise -a willread $DIR/$tfile || error "ladvise failed"
		local speed_ladvise=$(random_read_iops $DIR/$tfile $size)
		echo "Iter $i/$repeat: ladvise speed: $speed_ladvise"
		average_ladvise=$(bc <<<"$average_ladvise + $speed_ladvise")
	done
	average_origin=$(bc <<<"scale=2; $average_origin / $repeat")
	average_cache=$(bc <<<"scale=2; $average_cache / $repeat")
	average_ladvise=$(bc <<<"scale=2; $average_ladvise / $repeat")

	speedup_cache=$(percent $average_cache $average_origin)
	speedup_ladvise=$(percent $average_ladvise $average_origin)

	echo "Average uncached read: $average_origin"
	echo "Average speedup with OSS cached read: " \
		"$average_cache = +$speedup_cache%"
	echo "Average speedup with ladvise willread: " \
		"$average_ladvise = +$speedup_ladvise%"

	local lowest_speedup=20
	if [ ${average_cache%.*} -lt $lowest_speedup ]; then
		echo "Speedup with OSS cached read less than $lowest_speedup%," \
			"got $average_cache%. Skipping ladvise willread check."
		return 0
	fi

	# the test won't work on ZFS until it supports 'ladvise dontneed', but
	# it is still good to run until then to exercise 'ladvise willread'
	! $LFS ladvise -a dontneed $DIR/$tfile &&
		[ "$ost1_FSTYPE" = "zfs" ] &&
		echo "osd-zfs does not support dontneed or drop_caches" &&
		return 0

	lowest_speedup=$(bc <<<"scale=2; $average_cache / 2")
	[ ${average_ladvise%.*} -gt $lowest_speedup ] ||
		error_not_in_vm "Speedup with willread is less than " \
			"$lowest_speedup%, got $average_ladvise%"
}

test_255a() {
	[ $OST1_VERSION -lt $(version_code 2.8.54) ] &&
		skip "lustre < 2.8.54 does not support ladvise "
	remote_ost_nodsh && skip "remote OST with nodsh"

	lfs setstripe -c -1 -i 0 $DIR/$tfile || error "$tfile failed"

	ladvise_no_type willread $DIR/$tfile &&
		skip "willread ladvise is not supported"

	ladvise_no_ioctl $DIR/$tfile &&
		skip "ladvise ioctl is not supported"

	local size_mb=100
	local size=$((size_mb * 1048576))
	dd if=/dev/zero of=$DIR/$tfile bs=1048576 count=$size_mb ||
		error "dd to $DIR/$tfile failed"

	lfs ladvise -a willread $DIR/$tfile ||
		error "Ladvise failed with no range argument"

	lfs ladvise -a willread -s 0 $DIR/$tfile ||
		error "Ladvise failed with no -l or -e argument"

	lfs ladvise -a willread -e 1 $DIR/$tfile ||
		error "Ladvise failed with only -e argument"

	lfs ladvise -a willread -l 1 $DIR/$tfile ||
		error "Ladvise failed with only -l argument"

	lfs ladvise -a willread -s 2 -e 1 $DIR/$tfile &&
		error "End offset should not be smaller than start offset"

	lfs ladvise -a willread -s 2 -e 2 $DIR/$tfile &&
		error "End offset should not be equal to start offset"

	lfs ladvise -a willread -s $size -l 1 $DIR/$tfile ||
		error "Ladvise failed with overflowing -s argument"

	lfs ladvise -a willread -s 1 -e $((size + 1)) $DIR/$tfile ||
		error "Ladvise failed with overflowing -e argument"

	lfs ladvise -a willread -s 1 -l $size $DIR/$tfile ||
		error "Ladvise failed with overflowing -l argument"

	lfs ladvise -a willread -l 1 -e 2 $DIR/$tfile &&
		error "Ladvise succeeded with conflicting -l and -e arguments"

	echo "Synchronous ladvise should wait"
	local delay=4
#define OBD_FAIL_OST_LADVISE_PAUSE	 0x237
	do_nodes $(comma_list $(osts_nodes)) \
		$LCTL set_param fail_val=$delay fail_loc=0x237

	local start_ts=$SECONDS
	lfs ladvise -a willread $DIR/$tfile ||
		error "Ladvise failed with no range argument"
	local end_ts=$SECONDS
	local inteval_ts=$((end_ts - start_ts))

	if [ $inteval_ts -lt $(($delay - 1)) ]; then
		error "Synchronous advice didn't wait reply"
	fi

	echo "Asynchronous ladvise shouldn't wait"
	local start_ts=$SECONDS
	lfs ladvise -a willread -b $DIR/$tfile ||
		error "Ladvise failed with no range argument"
	local end_ts=$SECONDS
	local inteval_ts=$((end_ts - start_ts))

	if [ $inteval_ts -gt $(($delay / 2)) ]; then
		error "Asynchronous advice blocked"
	fi

	do_nodes $(comma_list $(osts_nodes)) $LCTL set_param fail_loc=0
	ladvise_willread_performance
}
run_test 255a "check 'lfs ladvise -a willread'"

facet_meminfo() {
	local facet=$1
	local info=$2

	do_facet $facet "cat /proc/meminfo | grep ^${info}:" | awk '{print $2}'
}

test_255b() {
	[ $OST1_VERSION -lt $(version_code 2.8.54) ] &&
		skip "lustre < 2.8.54 does not support ladvise "
	remote_ost_nodsh && skip "remote OST with nodsh"

	lfs setstripe -c 1 -i 0 $DIR/$tfile

	ladvise_no_type dontneed $DIR/$tfile &&
		skip "dontneed ladvise is not supported"

	ladvise_no_ioctl $DIR/$tfile &&
		skip "ladvise ioctl is not supported"

	! $LFS ladvise -a dontneed $DIR/$tfile &&
		[ "$ost1_FSTYPE" = "zfs" ] &&
		skip "zfs-osd does not support 'ladvise dontneed'"

	local size_mb=100
	local size=$((size_mb * 1048576))
	# In order to prevent disturbance of other processes, only check 3/4
	# of the memory usage
	local kibibytes=$((size_mb * 1024 * 3 / 4))

	dd if=/dev/zero of=$DIR/$tfile bs=1048576 count=$size_mb ||
		error "dd to $DIR/$tfile failed"

	#force write to complete before dropping OST cache & checking memory
	sync

	local total=$(facet_meminfo ost1 MemTotal)
	echo "Total memory: $total KiB"

	do_facet ost1 "sync && echo 3 > /proc/sys/vm/drop_caches"
	local before_read=$(facet_meminfo ost1 Cached)
	echo "Cache used before read: $before_read KiB"

	lfs ladvise -a willread $DIR/$tfile ||
		error "Ladvise willread failed"
	local after_read=$(facet_meminfo ost1 Cached)
	echo "Cache used after read: $after_read KiB"

	lfs ladvise -a dontneed $DIR/$tfile ||
		error "Ladvise dontneed again failed"
	local no_read=$(facet_meminfo ost1 Cached)
	echo "Cache used after dontneed ladvise: $no_read KiB"

	if [ $total -lt $((before_read + kibibytes)) ]; then
		echo "Memory is too small, abort checking"
		return 0
	fi

	if [ $((before_read + kibibytes)) -gt $after_read ]; then
		error "Ladvise willread should use more memory" \
			"than $kibibytes KiB"
	fi

	if [ $((no_read + kibibytes)) -gt $after_read ]; then
		error "Ladvise dontneed should release more memory" \
			"than $kibibytes KiB"
	fi
}
run_test 255b "check 'lfs ladvise -a dontneed'"

test_255c() {
	[ $OST1_VERSION -lt $(version_code 2.10.50) ] &&
		skip "lustre < 2.10.50 does not support lockahead"

	local count
	local new_count
	local difference
	local i
	local rc

	test_mkdir -p $DIR/$tdir
	$SETSTRIPE -i 0 $DIR/$tdir

	#test 10 returns only success/failure
	i=10
	lockahead_test -d $DIR/$tdir -t $i -f $tfile
	rc=$?
	if [ $rc -eq 255 ]; then
		error "Ladvise test${i} failed, ${rc}"
	fi

	#test 11 counts lock enqueue requests, all others count new locks
	i=11
	count=$(do_facet ost1 \
		$LCTL get_param -n ost.OSS.ost.stats)
	count=$(echo "$count" | grep ldlm_extent_enqueue | awk '{ print $2 }')

	lockahead_test -d $DIR/$tdir -t $i -f $tfile
	rc=$?
	if [ $rc -eq 255 ]; then
		error "Ladvise test${i} failed, ${rc}"
	fi

	new_count=$(do_facet ost1 \
		$LCTL get_param -n ost.OSS.ost.stats)
	new_count=$(echo "$new_count" | grep ldlm_extent_enqueue | \
		   awk '{ print $2 }')

	difference="$((new_count - count))"
	if [ $difference -ne $rc ]; then
		error "Ladvise test${i}, bad enqueue count, returned " \
		      "${rc}, actual ${difference}"
	fi

	for i in $(seq 12 21); do
		# If we do not do this, we run the risk of having too many
		# locks and starting lock cancellation while we are checking
		# lock counts.
		cancel_lru_locks osc

		count=$($LCTL get_param -n \
		       ldlm.namespaces.$FSNAME-OST0000*osc-[-0-9a-f]*.lock_unused_count)

		lockahead_test -d $DIR/$tdir -t $i -f $tfile
		rc=$?
		if [ $rc -eq 255 ]; then
			error "Ladvise test ${i} failed, ${rc}"
		fi

		new_count=$($LCTL get_param -n \
		       ldlm.namespaces.$FSNAME-OST0000*osc-[-0-9a-f]*.lock_unused_count)
		difference="$((new_count - count))"

		# Test 15 output is divided by 100 to map down to valid return
		if [ $i -eq 15 ]; then
			rc="$((rc * 100))"
		fi

		if [ $difference -ne $rc ]; then
			error "Ladvise test ${i}, bad lock count, returned " \
			      "${rc}, actual ${difference}"
		fi
	done

	#test 22 returns only success/failure
	i=22
	lockahead_test -d $DIR/$tdir -t $i -f $tfile
	rc=$?
	if [ $rc -eq 255 ]; then
		error "Ladvise test${i} failed, ${rc}"
	fi
}
run_test 255c "suite of ladvise lockahead tests"

test_256() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[ "$mds1_FSTYPE" != "ldiskfs" ] && skip "ldiskfs only test"
	changelog_users $SINGLEMDS | grep "^cl" &&
		skip "active changelog user"

	local cl_user
	local cat_sl
	local mdt_dev

	mdt_dev=$(mdsdevname 1)
	echo $mdt_dev

	changelog_register || error "changelog_register failed"

	rm -rf $DIR/$tdir
	mkdir -p $DIR/$tdir

	changelog_clear 0 || error "changelog_clear failed"

	# change something
	touch $DIR/$tdir/{1..10}

	# stop the MDT
	stop $SINGLEMDS || error "Fail to stop MDT"

	# remount the MDT

	start $SINGLEMDS $mdt_dev $MDS_MOUNT_OPTS || error "Fail to start MDT"

	#after mount new plainllog is used
	touch $DIR/$tdir/{11..19}
	local tmpfile=$(mktemp -u $tfile.XXXXXX)
	cat_sl=$(do_facet $SINGLEMDS "sync; \
		 $DEBUGFS -c -R 'dump changelog_catalog $tmpfile' $mdt_dev; \
		 llog_reader $tmpfile | grep -c type=1064553b")
	do_facet $SINGLEMDS llog_reader $tmpfile

	[ $cat_sl != 2 ] && error "Changelog catalog has $cat_sl != 2 slots"

	changelog_clear 0 || error "changelog_clear failed"

	cat_sl=$(do_facet $SINGLEMDS "sync; \
		 $DEBUGFS -c -R 'dump changelog_catalog $tmpfile' $mdt_dev; \
		 llog_reader $tmpfile | grep -c type=1064553b; rm -f $tmpfile")

	if (( cat_sl == 2 )); then
		error "Empty plain llog was not deleted from changelog catalog"
	elif (( cat_sl != 1 )); then
		error "Active plain llog shouldn't be deleted from catalog"
	fi
}
run_test 256 "Check llog delete for empty and not full state"

test_257() {
	remote_mds_nodsh && skip "remote MDS with nodsh"
	[[ $MDS1_VERSION -lt $(version_code 2.8.55) ]] &&
		skip "Need MDS version at least 2.8.55"

	test_mkdir $DIR/$tdir

	setfattr -n trusted.name1 -v value1 $DIR/$tdir ||
		error "setfattr -n trusted.name1=value1 $DIR/$tdir failed"
	stat $DIR/$tdir

#define OBD_FAIL_MDS_XATTR_REP			0x161
	local mdtidx=$($LFS getstripe -m $DIR/$tdir)
	local facet=mds$((mdtidx + 1))
	set_nodes_failloc $(facet_active_host $facet) 0x80000161
	getfattr -n trusted.name1 $DIR/$tdir 2> /dev/null

	stop $facet || error "stop MDS failed"
	start $facet $(mdsdevname $((mdtidx + 1))) $MDS_MOUNT_OPTS ||
		error "start MDS fail"
}
run_test 257 "xattr locks are not lost"

# Verify we take the i_mutex when security requires it
test_258a() {
#define OBD_FAIL_IMUTEX_SEC 0x141c
	$LCTL set_param fail_loc=0x141c
	touch $DIR/$tfile
	chmod u+s $DIR/$tfile
	chmod a+rwx $DIR/$tfile
	$RUNAS dd if=/dev/zero of=$DIR/$tfile bs=4k count=1 oflag=append
	RC=$?
	if [ $RC -ne 0 ]; then
		error "error, failed to take i_mutex, rc=$?"
	fi
	rm -f $DIR/$tfile
}
run_test 258a "verify i_mutex security behavior when suid attributes is set"

# Verify we do NOT take the i_mutex in the normal case
test_258b() {
#define OBD_FAIL_IMUTEX_NOSEC 0x141d
	$LCTL set_param fail_loc=0x141d
	touch $DIR/$tfile
	chmod a+rwx $DIR
	chmod a+rw $DIR/$tfile
	$RUNAS dd if=/dev/zero of=$DIR/$tfile bs=4k count=1 oflag=append
	RC=$?
	if [ $RC -ne 0 ]; then
		error "error, took i_mutex unnecessarily, rc=$?"
	fi
	rm -f $DIR/$tfile

}
run_test 258b "verify i_mutex security behavior"

test_259() {
	local file=$DIR/$tfile
	local before
	local after

	[ "$mds1_FSTYPE" != "ldiskfs" ] && skip "ldiskfs only test"

	stack_trap "rm -f $file" EXIT

	wait_delete_completed
	before=$(do_facet ost1 "$LCTL get_param -n osd-*.*OST0000.kbytesfree")
	echo "before: $before"

	$LFS setstripe -i 0 -c 1 $file
	dd if=/dev/zero of=$file bs=1M count=10 || error "couldn't write"
	sync_all_data
	after=$(do_facet ost1 "$LCTL get_param -n osd-*.*OST0000.kbytesfree")
	echo "after write: $after"

#define OBD_FAIL_OSD_FAIL_AT_TRUNCATE          0x2301
	do_facet ost1 $LCTL set_param fail_loc=0x2301
	$TRUNCATE $file 0
	after=$(do_facet ost1 "$LCTL get_param -n osd-*.*OST0000.kbytesfree")
	echo "after truncate: $after"

	stop ost1
	do_facet ost1 $LCTL set_param fail_loc=0
	start ost1 $(ostdevname 1) $OST_MOUNT_OPTS || error "cannot start ost1"
	sleep 2
	after=$(do_facet ost1 "$LCTL get_param -n osd-*.*OST0000.kbytesfree")
	echo "after restart: $after"
	[ $((after - before)) -ge $(fs_log_size ost1) ] &&
		error "missing truncate?"

	return 0
}
run_test 259 "crash at delayed truncate"

test_260() {
#define OBD_FAIL_MDC_CLOSE               0x806
	$LCTL set_param fail_loc=0x80000806
	touch $DIR/$tfile

}
run_test 260 "Check mdc_close fail"

### Data-on-MDT sanity tests ###
test_270a() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55 for DoM"

	# create DoM file
	local dom=$DIR/$tdir/dom_file
	local tmp=$DIR/$tdir/tmp_file

	mkdir -p $DIR/$tdir

	# basic checks for DoM component creation
	$LFS setstripe -E 1024K -E 2048K -L mdt $dom 2>/dev/null &&
		error "Can set MDT layout to non-first entry"

	$LFS setstripe -E 1024K -L mdt -E 2048K -L mdt $dom 2>/dev/null &&
		error "Can define multiple entries as MDT layout"

	$LFS setstripe -E 1M -L mdt $dom || error "Can't create DoM layout"

	[ $($LFS getstripe -L $dom) == "mdt" ] || error "bad pattern"
	[ $($LFS getstripe -c $dom) == 0 ] || error "bad stripe count"
	[ $($LFS getstripe -S $dom) == 1048576 ] || error "bad stripe size"

	local mdtidx=$($LFS getstripe -m $dom)
	local mdtname=MDT$(printf %04x $mdtidx)
	local facet=mds$((mdtidx + 1))
	local space_check=1

	# Skip free space checks with ZFS
	[ "$(facet_fstype $facet)" == "zfs" ] && space_check=0

	# write
	sync
	local size_tmp=$((65536 * 3))
	local mdtfree1=$(do_facet $facet \
			 lctl get_param -n osd*.*$mdtname.kbytesfree)

	dd if=/dev/urandom of=$tmp bs=1024 count=$((size_tmp / 1024))
	# check also direct IO along write
	# IO size must be a multiple of PAGE_SIZE on all platforms (ARM=64KB)
	dd if=$tmp of=$dom bs=65536 count=$((size_tmp / 65536)) oflag=direct
	sync
	cmp $tmp $dom || error "file data is different"
	[ $(stat -c%s $dom) == $size_tmp ] ||
		error "bad size after write: $(stat -c%s $dom) != $size_tmp"
	if [ $space_check == 1 ]; then
		local mdtfree2=$(do_facet $facet \
				 lctl get_param -n osd*.*$mdtname.kbytesfree)

		# increase in usage from by $size_tmp
		[ $(($mdtfree1 - $mdtfree2)) -ge $((size_tmp / 1024)) ] ||
			error "MDT free space wrong after write: " \
			      "$mdtfree1 >= $mdtfree2 + $size_tmp/1024"
	fi

	# truncate
	local size_dom=10000

	$TRUNCATE $dom $size_dom
	[ $(stat -c%s $dom) == $size_dom ] ||
		error "bad size after truncate: $(stat -c%s $dom) != $size_dom"
	if [ $space_check == 1 ]; then
		mdtfree1=$(do_facet $facet \
				lctl get_param -n osd*.*$mdtname.kbytesfree)
		# decrease in usage from $size_tmp to new $size_dom
		[ $(($mdtfree1 - $mdtfree2)) -ge \
		  $(((size_tmp - size_dom) / 1024)) ] ||
			error "MDT free space is wrong after truncate: " \
			      "$mdtfree1 >= $mdtfree2 + ($size_tmp - $size_dom) / 1024"
	fi

	# append
	cat $tmp >> $dom
	sync
	size_dom=$((size_dom + size_tmp))
	[ $(stat -c%s $dom) == $size_dom ] ||
		error "bad size after append: $(stat -c%s $dom) != $size_dom"
	if [ $space_check == 1 ]; then
		mdtfree2=$(do_facet $facet \
				lctl get_param -n osd*.*$mdtname.kbytesfree)
		# increase in usage by $size_tmp from previous
		[ $(($mdtfree1 - $mdtfree2)) -ge $((size_tmp / 1024)) ] ||
			error "MDT free space is wrong after append: " \
			      "$mdtfree1 >= $mdtfree2 + $size_tmp/1024"
	fi

	# delete
	rm $dom
	if [ $space_check == 1 ]; then
		mdtfree1=$(do_facet $facet \
				lctl get_param -n osd*.*$mdtname.kbytesfree)
		# decrease in usage by $size_dom from previous
		[ $(($mdtfree1 - $mdtfree2)) -ge $((size_dom / 1024)) ] ||
			error "MDT free space is wrong after removal: " \
			      "$mdtfree1 >= $mdtfree2 + $size_dom/1024"
	fi

	# combined striping
	$LFS setstripe -E 1024K -L mdt -E EOF $dom ||
		error "Can't create DoM + OST striping"

	size_tmp=2031616 # must be a multiple of PAGE_SIZE=65536 on ARM
	dd if=/dev/urandom of=$tmp bs=1024 count=$((size_tmp / 1024))
	# check also direct IO along write
	dd if=$tmp of=$dom bs=65536 count=$((size_tmp / 65536)) oflag=direct
	sync
	cmp $tmp $dom || error "file data is different"
	[ $(stat -c%s $dom) == $size_tmp ] ||
		error "bad size after write: $(stat -c%s $dom) != $size_tmp"
	rm $dom $tmp

	return 0
}
run_test 270a "DoM: basic functionality tests"

test_270b() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	local dom=$DIR/$tdir/dom_file
	local max_size=1048576

	mkdir -p $DIR/$tdir
	$LFS setstripe -E $max_size -L mdt $dom

	# truncate over the limit
	$TRUNCATE $dom $(($max_size + 1)) &&
		error "successful truncate over the maximum size"
	# write over the limit
	dd if=/dev/zero of=$dom bs=$max_size seek=1 count=1 &&
		error "successful write over the maximum size"
	# append over the limit
	dd if=/dev/zero of=$dom bs=$(($max_size - 3)) count=1
	echo "12345" >> $dom && error "successful append over the maximum size"
	rm $dom

	return 0
}
run_test 270b "DoM: maximum size overflow checks for DoM-only file"

test_270c() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	mkdir -p $DIR/$tdir
	$LFS setstripe -E 1024K -L mdt $DIR/$tdir

	# check files inherit DoM EA
	touch $DIR/$tdir/first
	[ $($GETSTRIPE -L $DIR/$tdir/first) == "mdt" ] ||
		error "bad pattern"
	[ $($LFS getstripe -c $DIR/$tdir/first) == 0 ] ||
		error "bad stripe count"
	[ $($LFS getstripe -S $DIR/$tdir/first) == 1048576 ] ||
		error "bad stripe size"

	# check directory inherits DoM EA and uses it as default
	mkdir $DIR/$tdir/subdir
	touch $DIR/$tdir/subdir/second
	[ $($LFS getstripe -L $DIR/$tdir/subdir/second) == "mdt" ] ||
		error "bad pattern in sub-directory"
	[ $($LFS getstripe -c $DIR/$tdir/subdir/second) == 0 ] ||
		error "bad stripe count in sub-directory"
	[ $($LFS getstripe -S $DIR/$tdir/subdir/second) == 1048576 ] ||
		error "bad stripe size in sub-directory"
	return 0
}
run_test 270c "DoM: DoM EA inheritance tests"

test_270d() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	mkdir -p $DIR/$tdir
	$LFS setstripe -E 1024K -L mdt $DIR/$tdir

	# inherit default DoM striping
	mkdir $DIR/$tdir/subdir
	touch $DIR/$tdir/subdir/f1

	# change default directory striping
	$LFS setstripe -c 1 $DIR/$tdir/subdir
	touch $DIR/$tdir/subdir/f2
	[ $($LFS getstripe -c $DIR/$tdir/subdir/f2) == 1 ] ||
		error "wrong default striping in file 2"
	[ $($LFS getstripe -L $DIR/$tdir/subdir/f2) == "raid0" ] ||
		error "bad pattern in file 2"
	return 0
}
run_test 270d "DoM: change striping from DoM to RAID0"

test_270e() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	mkdir -p $DIR/$tdir/dom
	mkdir -p $DIR/$tdir/norm
	DOMFILES=20
	NORMFILES=10
	$LFS setstripe -E 1M -L mdt $DIR/$tdir/dom
	$LFS setstripe -i 0 -S 2M $DIR/$tdir/norm

	createmany -o $DIR/$tdir/dom/dom- $DOMFILES
	createmany -o $DIR/$tdir/norm/norm- $NORMFILES

	# find DoM files by layout
	NUM=$($LFS find -L mdt -type f $DIR/$tdir 2>/dev/null | wc -l)
	[ $NUM -eq  $DOMFILES ] ||
		error "lfs find -L: found $NUM, expected $DOMFILES"
	echo "Test 1: lfs find 20 DOM files by layout: OK"

	# there should be 1 dir with default DOM striping
	NUM=$($LFS find -L mdt -type d $DIR/$tdir 2>/dev/null | wc -l)
	[ $NUM -eq  1 ] ||
		error "lfs find -L: found $NUM, expected 1 dir"
	echo "Test 2: lfs find 1 DOM dir by layout: OK"

	# find DoM files by stripe size
	NUM=$($LFS find -S -1200K -type f $DIR/$tdir 2>/dev/null | wc -l)
	[ $NUM -eq  $DOMFILES ] ||
		error "lfs find -S: found $NUM, expected $DOMFILES"
	echo "Test 4: lfs find 20 DOM files by stripe size: OK"

	# find files by stripe offset except DoM files
	NUM=$($LFS find -i 0 -type f $DIR/$tdir 2>/dev/null | wc -l)
	[ $NUM -eq  $NORMFILES ] ||
		error "lfs find -i: found $NUM, expected $NORMFILES"
	echo "Test 5: lfs find no DOM files by stripe index: OK"
	return 0
}
run_test 270e "DoM: lfs find with DoM files test"

test_270f() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	local mdtname=${FSNAME}-MDT0000-mdtlov
	local dom=$DIR/$tdir/dom_file
	local dom_limit_saved=$(do_facet mds1 $LCTL get_param -n \
						lod.$mdtname.dom_stripesize)
	local dom_limit=131072

	do_facet mds1 $LCTL set_param -n lod.$mdtname.dom_stripesize=$dom_limit
	local dom_current=$(do_facet mds1 $LCTL get_param -n \
						lod.$mdtname.dom_stripesize)
	[ ${dom_limit} -eq ${dom_current} ] ||
		error "Cannot change per-MDT DoM stripe limit to $dom_limit"

	$LFS mkdir -i 0 -c 1 $DIR/$tdir
	$LFS setstripe -d $DIR/$tdir
	$LFS setstripe -E $dom_limit -L mdt $DIR/$tdir ||
		error "Can't set directory default striping"

	# exceed maximum stripe size
	$LFS setstripe -E $((dom_limit * 2)) -L mdt $dom ||
		error "Can't create file with $((dom_limit * 2)) DoM stripe"
	[ $($LFS getstripe -S $dom) -eq $((dom_limit * 2)) ] &&
		error "Able to create DoM component size more than LOD limit"

	do_facet mds1 $LCTL set_param -n lod.$mdtname.dom_stripesize=0
	dom_current=$(do_facet mds1 $LCTL get_param -n \
						lod.$mdtname.dom_stripesize)
	[ 0 -eq ${dom_current} ] ||
		error "Can't set zero DoM stripe limit"
	rm $dom

	# attempt to create DoM file on server with disabled DoM should
	# remove DoM entry from layout and be succeed
	$LFS setstripe -E $dom_limit -L mdt -E -1 $dom ||
		error "Can't create DoM file (DoM is disabled)"
	[ $($LFS getstripe -L $dom) == "mdt" ] &&
		error "File has DoM component while DoM is disabled"
	rm $dom

	# attempt to create DoM file with only DoM stripe should return error
	$LFS setstripe -E $dom_limit -L mdt $dom &&
		error "Able to create DoM-only file while DoM is disabled"

	# too low values to be aligned with smallest stripe size 64K
	do_facet mds1 $LCTL set_param -n lod.$mdtname.dom_stripesize=30000
	dom_current=$(do_facet mds1 $LCTL get_param -n \
						lod.$mdtname.dom_stripesize)
	[ 30000 -eq ${dom_current} ] &&
		error "Can set too small DoM stripe limit"

	# 64K is a minimal stripe size in Lustre, expect limit of that size
	[ 65536 -eq ${dom_current} ] ||
		error "Limit is not set to 64K but ${dom_current}"

	do_facet mds1 $LCTL set_param -n lod.$mdtname.dom_stripesize=2147483648
	dom_current=$(do_facet mds1 $LCTL get_param -n \
						lod.$mdtname.dom_stripesize)
	echo $dom_current
	[ 2147483648 -eq ${dom_current} ] &&
		error "Can set too large DoM stripe limit"

	do_facet mds1 $LCTL set_param -n \
				lod.$mdtname.dom_stripesize=$((dom_limit * 2))
	$LFS setstripe -E $((dom_limit * 2)) -L mdt $dom ||
		error "Can't create DoM component size after limit change"
	do_facet mds1 $LCTL set_param -n \
				lod.$mdtname.dom_stripesize=$((dom_limit / 2))
	$LFS setstripe -E $dom_limit -L mdt ${dom}_big ||
		error "Can't create DoM file after limit decrease"
	[ $($LFS getstripe -S ${dom}_big) -eq $((dom_limit / 2)) ] ||
		error "Can create big DoM component after limit decrease"
	touch ${dom}_def ||
		error "Can't create file with old default layout"

	do_facet mds1 $LCTL set_param -n lod.*.dom_stripesize=$dom_limit_saved
	return 0
}
run_test 270f "DoM: maximum DoM stripe size checks"

test_271a() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	local dom=$DIR/$tdir/dom

	mkdir -p $DIR/$tdir

	$LFS setstripe -E 1024K -L mdt $dom

	lctl set_param -n mdc.*.stats=clear
	dd if=/dev/zero of=$dom bs=4096 count=1 || return 1
	cat $dom > /dev/null
	local reads=$(lctl get_param -n mdc.*.stats | grep -c ost_read)
	[ $reads -eq 0 ] || error "Unexpected $reads READ RPCs"
	ls $dom
	rm -f $dom
}
run_test 271a "DoM: data is cached for read after write"

test_271b() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	local dom=$DIR/$tdir/dom

	mkdir -p $DIR/$tdir

	$LFS setstripe -E 1024K -L mdt -E EOF $dom

	lctl set_param -n mdc.*.stats=clear
	dd if=/dev/zero of=$dom bs=4096 count=1 || return 1
	cancel_lru_locks mdc
	$CHECKSTAT -t file -s 4096 $dom || error "stat #1 fails"
	# second stat to check size is cached on client
	$CHECKSTAT -t file -s 4096 $dom || error "stat #2 fails"
	local gls=$(lctl get_param -n mdc.*.stats | grep -c ldlm_glimpse)
	[ $gls -eq 0 ] || error "Unexpected $gls glimpse RPCs"
	rm -f $dom
}
run_test 271b "DoM: no glimpse RPC for stat (DoM only file)"

test_271ba() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	local dom=$DIR/$tdir/dom

	mkdir -p $DIR/$tdir

	$LFS setstripe -E 1024K -L mdt -E EOF $dom

	lctl set_param -n mdc.*.stats=clear
	lctl set_param -n osc.*.stats=clear
	dd if=/dev/zero of=$dom bs=2048K count=1 || return 1
	cancel_lru_locks mdc
	$CHECKSTAT -t file -s 2097152 $dom || error "stat"
	# second stat to check size is cached on client
	$CHECKSTAT -t file -s 2097152 $dom || error "stat"
	local gls=$(lctl get_param -n mdc.*.stats | grep -c ldlm_glimpse)
	[ $gls == 0 ] || error "Unexpected $gls glimpse RPCs"
	local gls=$(lctl get_param -n osc.*.stats | grep -c ldlm_glimpse)
	[ $gls == 0 ] || error "Unexpected $gls OSC glimpse RPCs"
	rm -f $dom
}
run_test 271ba "DoM: no glimpse RPC for stat (combined file)"


get_mdc_stats() {
	local mdtidx=$1
	local param=$2
	local mdt=MDT$(printf %04x $mdtidx)

	if [ -z $param ]; then
		lctl get_param -n mdc.*$mdt*.stats
	else
		lctl get_param -n mdc.*$mdt*.stats | awk "/$param/"'{print $2}'
	fi
}

test_271c() {
	[ $MDS1_VERSION -lt $(version_code 2.10.55) ] &&
		skip "Need MDS version at least 2.10.55"

	local dom=$DIR/$tdir/dom

	mkdir -p $DIR/$tdir

	$LFS setstripe -E 1024K -L mdt $DIR/$tdir

	local mdtidx=$($LFS getstripe -m $DIR/$tdir)
	local facet=mds$((mdtidx + 1))

	cancel_lru_locks mdc
	do_facet $facet lctl set_param -n mdt.*.dom_lock=0
	createmany -o $dom 1000
	lctl set_param -n mdc.*.stats=clear
	smalliomany -w $dom 1000 200
	get_mdc_stats $mdtidx
	local enq=$(get_mdc_stats $mdtidx ldlm_ibits_enqueue)
	# Each file has 1 open, 1 IO enqueues, total 2000
	# but now we have also +1 getxattr for security.capability, total 3000
	[ $enq -ge 2000 ] || error "Too few enqueues $enq, expected > 2000"
	unlinkmany $dom 1000

	cancel_lru_locks mdc
	do_facet $facet lctl set_param -n mdt.*.dom_lock=1
	createmany -o $dom 1000
	lctl set_param -n mdc.*.stats=clear
	smalliomany -w $dom 1000 200
	local enq_2=$(get_mdc_stats $mdtidx ldlm_ibits_enqueue)
	# Expect to see reduced amount of RPCs by 1000 due to single enqueue
	# for OPEN and IO lock.
	[ $((enq - enq_2)) -ge 1000 ] ||
		error "Too many enqueues $enq_2, expected about $((enq - 1000))"
	unlinkmany $dom 1000
	return 0
}
run_test 271c "DoM: IO lock at open saves enqueue RPCs"

cleanup_271def_tests() {
	trap 0
	rm -f $1
}

test_271d() {
	[ $MDS1_VERSION -lt $(version_code 2.10.57) ] &&
		skip "Need MDS version at least 2.10.57"

	local dom=$DIR/$tdir/dom
	local tmp=$TMP/$tfile
	trap "cleanup_271def_tests $tmp" EXIT

	mkdir -p $DIR/$tdir

	$LFS setstripe -E 1024K -L mdt $DIR/$tdir

	local mdtidx=$($LFS getstripe --mdt-index $DIR/$tdir)

	cancel_lru_locks mdc
	dd if=/dev/urandom of=$tmp bs=1000 count=1
	dd if=$tmp of=$dom bs=1000 count=1
	cancel_lru_locks mdc

	cat /etc/hosts >> $tmp
	lctl set_param -n mdc.*.stats=clear

	# append data to the same file it should update local page
	echo "Append to the same page"
	cat /etc/hosts >> $dom
	local num=$(get_mdc_stats $mdtidx ost_read)
	local ra=$(get_mdc_stats $mdtidx req_active)
	local rw=$(get_mdc_stats $mdtidx req_waittime)

	[ -z $num ] || error "$num READ RPC occured"
	[ $ra == $rw ] || error "$((ra - rw)) resend occured"
	echo "... DONE"

	# compare content
	cmp $tmp $dom || error "file miscompare"

	cancel_lru_locks mdc
	lctl set_param -n mdc.*.stats=clear

	echo "Open and read file"
	cat $dom > /dev/null
	local num=$(get_mdc_stats $mdtidx ost_read)
	local ra=$(get_mdc_stats $mdtidx req_active)
	local rw=$(get_mdc_stats $mdtidx req_waittime)

	[ -z $num ] || error "$num READ RPC occured"
	[ $ra == $rw ] || error "$((ra - rw)) resend occured"
	echo "... DONE"

	# compare content
	cmp $tmp $dom || error "file miscompare"

	return 0
}
run_test 271d "DoM: read on open (1K file in reply buffer)"

test_271f() {
	[ $MDS1_VERSION -lt $(version_code 2.10.57) ] &&
		skip "Need MDS version at least 2.10.57"

	local dom=$DIR/$tdir/dom
	local tmp=$TMP/$tfile
	trap "cleanup_271def_tests $tmp" EXIT

	mkdir -p $DIR/$tdir

	$LFS setstripe -E 1024K -L mdt $DIR/$tdir

	local mdtidx=$($LFS getstripe --mdt-index $DIR/$tdir)

	cancel_lru_locks mdc
	dd if=/dev/urandom of=$tmp bs=200000 count=1
	dd if=$tmp of=$dom bs=200000 count=1
	cancel_lru_locks mdc
	cat /etc/hosts >> $tmp
	lctl set_param -n mdc.*.stats=clear

	echo "Append to the same page"
	cat /etc/hosts >> $dom
	local num=$(get_mdc_stats $mdtidx ost_read)
	local ra=$(get_mdc_stats $mdtidx req_active)
	local rw=$(get_mdc_stats $mdtidx req_waittime)

	[ -z $num ] || error "$num READ RPC occured"
	[ $ra == $rw ] || error "$((ra - rw)) resend occured"
	echo "... DONE"

	# compare content
	cmp $tmp $dom || error "file miscompare"

	cancel_lru_locks mdc
	lctl set_param -n mdc.*.stats=clear

	echo "Open and read file"
	cat $dom > /dev/null
	local num=$(get_mdc_stats $mdtidx ost_read)
	local ra=$(get_mdc_stats $mdtidx req_active)
	local rw=$(get_mdc_stats $mdtidx req_waittime)

	[ $num -eq 1 ] || error "expect 1 READ RPC, $num occured"
	[ $ra == $rw ] || error "$((ra - rw)) resend occured"
	echo "... DONE"

	# compare content
	cmp $tmp $dom || error "file miscompare"

	return 0
}
run_test 271f "DoM: read on open (200K file and read tail)"

test_271g() {
	[[ $($LCTL get_param mdc.*.import) =~ async_discard ]] ||
		skip "Skipping due to old client or server version"

	$LFS setstripe -E 1024K -L mdt -E EOF $DIR1/$tfile
	# to get layout
	$CHECKSTAT -t file $DIR1/$tfile

	$MULTIOP $DIR1/$tfile Ow40960_w4096c &
	MULTIOP_PID=$!
	sleep 1
	#define OBD_FAIL_LDLM_CANCEL_BL_CB_RACE
	$LCTL set_param fail_loc=0x80000314
	rm $DIR1/$tfile || error "Unlink fails"
	RC=$?
	kill -USR1 $MULTIOP_PID && wait $MULTIOP_PID || error "multiop failure"
	[ $RC -eq 0 ] || error "Failed write to stale object"
}
run_test 271g "Discard DoM data vs client flush race"

test_272a() {
	[ $MDS1_VERSION -lt $(version_code 2.11.50) ] &&
		skip "Need MDS version at least 2.11.50"

	local dom=$DIR/$tdir/dom
	mkdir -p $DIR/$tdir

	$LFS setstripe -E 256K -L mdt -E -1 -c1 $dom
	dd if=/dev/urandom of=$dom bs=512K count=1 ||
		error "failed to write data into $dom"
	local old_md5=$(md5sum $dom)

	$LFS migrate -E 256K -L mdt -E -1 -c2 $dom ||
		error "failed to migrate to the same DoM component"

	[ $($LFS getstripe -c $dom) -eq 2 ] ||
		error "layout was not changed silently"

	local new_md5=$(md5sum $dom)

	[ "$old_md5" != "$new_md5" ] &&
		error "md5sum differ: $old_md5, $new_md5"
	return 0
}
run_test 272a "DoM migration: new layout with the same DOM component"

test_272b() {
	[ $MDS1_VERSION -lt $(version_code 2.11.50) ] &&
		skip "Need MDS version at least 2.11.50"

	local dom=$DIR/$tdir/dom
	mkdir -p $DIR/$tdir
	$LFS setstripe -E 1M -L mdt -E -1 -c1 $dom

	local mdtidx=$($LFS getstripe -m $dom)
	local mdtname=MDT$(printf %04x $mdtidx)
	local facet=mds$((mdtidx + 1))

	local mdtfree1=$(do_facet $facet \
		lctl get_param -n osd*.*$mdtname.kbytesfree)
	dd if=/dev/urandom of=$dom bs=2M count=1 ||
		error "failed to write data into $dom"
	local old_md5=$(md5sum $dom)
	cancel_lru_locks mdc
	local mdtfree1=$(do_facet $facet \
		lctl get_param -n osd*.*$mdtname.kbytesfree)

	$LFS migrate -c2 $dom ||
		error "failed to migrate to the new composite layout"
	[ $($LFS getstripe -L $dom) == 'mdt' ] &&
		error "MDT stripe was not removed"

	cancel_lru_locks mdc
	local new_md5=$(md5sum $dom)
	[ "$old_md5" != "$new_md5" ] &&
		error "$old_md5 != $new_md5"

	# Skip free space checks with ZFS
	if [ "$(facet_fstype $facet)" != "zfs" ]; then
		local mdtfree2=$(do_facet $facet \
				lctl get_param -n osd*.*$mdtname.kbytesfree)
		[ $mdtfree2 -gt $mdtfree1 ] ||
			error "MDT space is not freed after migration"
	fi
	return 0
}
run_test 272b "DoM migration: DOM file to the OST-striped file (plain)"

test_272c() {
	[ $MDS1_VERSION -lt $(version_code 2.11.50) ] &&
		skip "Need MDS version at least 2.11.50"

	local dom=$DIR/$tdir/$tfile
	mkdir -p $DIR/$tdir
	$LFS setstripe -E 1M -L mdt -E -1 -c1 $dom

	local mdtidx=$($LFS getstripe -m $dom)
	local mdtname=MDT$(printf %04x $mdtidx)
	local facet=mds$((mdtidx + 1))

	dd if=/dev/urandom of=$dom bs=2M count=1 oflag=direct ||
		error "failed to write data into $dom"
	local old_md5=$(md5sum $dom)
	cancel_lru_locks mdc
	local mdtfree1=$(do_facet $facet \
		lctl get_param -n osd*.*$mdtname.kbytesfree)

	$LFS migrate -E 2M -c1 -E -1 -c2 $dom ||
		error "failed to migrate to the new composite layout"
	[ $($LFS getstripe -L $dom) == 'mdt' ] &&
		error "MDT stripe was not removed"

	cancel_lru_locks mdc
	local new_md5=$(md5sum $dom)
	[ "$old_md5" != "$new_md5" ] &&
		error "$old_md5 != $new_md5"

	# Skip free space checks with ZFS
	if [ "$(facet_fstype $facet)" != "zfs" ]; then
		local mdtfree2=$(do_facet $facet \
				lctl get_param -n osd*.*$mdtname.kbytesfree)
		[ $mdtfree2 -gt $mdtfree1 ] ||
			error "MDS space is not freed after migration"
	fi
	return 0
}
run_test 272c "DoM migration: DOM file to the OST-striped file (composite)"

test_273a() {
	[ $MDS1_VERSION -lt $(version_code 2.11.50) ] &&
		skip "Need MDS version at least 2.11.50"

	# Layout swap cannot be done if either file has DOM component,
	# this will never be supported, migration should be used instead

	local dom=$DIR/$tdir/$tfile
	mkdir -p $DIR/$tdir

	$LFS setstripe -c2 ${dom}_plain
	$LFS setstripe -E 1M -L mdt -E -1 -c2 ${dom}_dom
	$LFS swap_layouts ${dom}_plain ${dom}_dom &&
		error "can swap layout with DoM component"
	$LFS swap_layouts ${dom}_dom ${dom}_plain &&
		error "can swap layout with DoM component"

	$LFS setstripe -E 1M -c1 -E -1 -c2 ${dom}_comp
	$LFS swap_layouts ${dom}_comp ${dom}_dom &&
		error "can swap layout with DoM component"
	$LFS swap_layouts ${dom}_dom ${dom}_comp &&
		error "can swap layout with DoM component"
	return 0
}
run_test 273a "DoM: layout swapping should fail with DOM"

test_275() {
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ $OST1_VERSION -lt $(version_code 2.10.57) ] &&
		skip "Need OST version >= 2.10.57"

	local file=$DIR/$tfile
	local oss

	oss=$(comma_list $(osts_nodes))

	dd if=/dev/urandom of=$file bs=1M count=2 ||
		error "failed to create a file"
	cancel_lru_locks osc

	#lock 1
	dd if=$file of=/dev/null bs=1M count=1 iflag=direct ||
		error "failed to read a file"

#define OBD_FAIL_LDLM_PAUSE_CANCEL2      0x31f
	$LCTL set_param fail_loc=0x8000031f

	cancel_lru_locks osc &
	sleep 1

#define OBD_FAIL_LDLM_PROLONG_PAUSE      0x32b
	do_nodes $oss $LCTL set_param fail_loc=0x8000032b
	#IO takes another lock, but matches the PENDING one
	#and places it to the IO RPC
	dd if=$file of=/dev/null bs=1M count=1 iflag=direct ||
		error "failed to read a file with PENDING lock"
}
run_test 275 "Read on a canceled duplicate lock"

test_276() {
	remote_ost_nodsh && skip "remote OST with nodsh"
	local pid

	do_facet ost1 "(while true; do \
		$LCTL get_param obdfilter.*.filesfree > /dev/null 2>&1; \
		done) & pid=\\\$!; echo \\\$pid > $TMP/sanity_276_pid" &
	pid=$!

	for LOOP in $(seq 20); do
		stop ost1
		start ost1 $(ostdevname 1) $OST_MOUNT_OPTS
	done
	kill -9 $pid
	do_facet ost1 "pid=\\\$(cat $TMP/sanity_276_pid); kill -9 \\\$pid; \
		rm $TMP/sanity_276_pid"
}
run_test 276 "Race between mount and obd_statfs"

cleanup_test_300() {
	trap 0
	umask $SAVE_UMASK
}
test_striped_dir() {
	local mdt_index=$1
	local stripe_count
	local stripe_index

	mkdir -p $DIR/$tdir

	SAVE_UMASK=$(umask)
	trap cleanup_test_300 RETURN EXIT

	$LFS setdirstripe -i $mdt_index -c 2 -H all_char -o 755 \
						$DIR/$tdir/striped_dir ||
		error "set striped dir error"

	local mode=$(stat -c%a $DIR/$tdir/striped_dir)
	[ "$mode" = "755" ] || error "expect 755 got $mode"

	$LFS getdirstripe $DIR/$tdir/striped_dir > /dev/null 2>&1 ||
		error "getdirstripe failed"
	stripe_count=$($LFS getdirstripe -c $DIR/$tdir/striped_dir)
	if [ "$stripe_count" != "2" ]; then
		error "1:stripe_count is $stripe_count, expect 2"
	fi
	stripe_count=$($LFS getdirstripe -T $DIR/$tdir/striped_dir)
	if [ "$stripe_count" != "2" ]; then
		error "2:stripe_count is $stripe_count, expect 2"
	fi

	stripe_index=$($LFS getdirstripe -i $DIR/$tdir/striped_dir)
	if [ "$stripe_index" != "$mdt_index" ]; then
		error "stripe_index is $stripe_index, expect $mdt_index"
	fi

	[ $(stat -c%h $DIR/$tdir/striped_dir) == '2' ] ||
		error "nlink error after create striped dir"

	mkdir $DIR/$tdir/striped_dir/a
	mkdir $DIR/$tdir/striped_dir/b

	stat $DIR/$tdir/striped_dir/a ||
		error "create dir under striped dir failed"
	stat $DIR/$tdir/striped_dir/b ||
		error "create dir under striped dir failed"

	[ $(stat -c%h $DIR/$tdir/striped_dir) == '4' ] ||
		error "nlink error after mkdir"

	rmdir $DIR/$tdir/striped_dir/a
	[ $(stat -c%h $DIR/$tdir/striped_dir) == '3' ] ||
		error "nlink error after rmdir"

	rmdir $DIR/$tdir/striped_dir/b
	[ $(stat -c%h $DIR/$tdir/striped_dir) == '2' ] ||
		error "nlink error after rmdir"

	chattr +i $DIR/$tdir/striped_dir
	createmany -o $DIR/$tdir/striped_dir/f 10 &&
		error "immutable flags not working under striped dir!"
	chattr -i $DIR/$tdir/striped_dir

	rmdir $DIR/$tdir/striped_dir ||
		error "rmdir striped dir error"

	cleanup_test_300

	true
}

test_300a() {
	[ $MDS1_VERSION -lt $(version_code 2.7.0) ] &&
		skip "skipped for lustre < 2.7.0"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	test_striped_dir 0 || error "failed on striped dir on MDT0"
	test_striped_dir 1 || error "failed on striped dir on MDT0"
}
run_test 300a "basic striped dir sanity test"

test_300b() {
	[ $MDS1_VERSION -lt $(version_code 2.7.0) ] &&
		skip "skipped for lustre < 2.7.0"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	local i
	local mtime1
	local mtime2
	local mtime3

	test_mkdir $DIR/$tdir || error "mkdir fail"
	$LFS setdirstripe -i 0 -c 2 -H all_char $DIR/$tdir/striped_dir ||
		error "set striped dir error"
	for i in {0..9}; do
		mtime1=$(stat -c %Y $DIR/$tdir/striped_dir)
		sleep 1
		touch $DIR/$tdir/striped_dir/file_$i || error "touch error $i"
		mtime2=$(stat -c %Y $DIR/$tdir/striped_dir)
		[ $mtime1 -eq $mtime2 ] && error "mtime unchanged after create"
		sleep 1
		rm -f $DIR/$tdir/striped_dir/file_$i || error "unlink error $i"
		mtime3=$(stat -c %Y $DIR/$tdir/striped_dir)
		[ $mtime2 -eq $mtime3 ] && error "mtime unchanged after unlink"
	done
	true
}
run_test 300b "check ctime/mtime for striped dir"

test_300c() {
	[ $MDS1_VERSION -lt $(version_code 2.7.0) ] &&
		skip "skipped for lustre < 2.7.0"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	local file_count

	mkdir -p $DIR/$tdir
	$LFS setdirstripe -i 0 -c 2 $DIR/$tdir/striped_dir ||
		error "set striped dir error"

	chown $RUNAS_ID:$RUNAS_GID $DIR/$tdir/striped_dir ||
		error "chown striped dir failed"

	$RUNAS createmany -o $DIR/$tdir/striped_dir/f 5000 ||
		error "create 5k files failed"

	file_count=$(ls $DIR/$tdir/striped_dir | wc -l)

	[ "$file_count" = 5000 ] || error "file count $file_count != 5000"

	rm -rf $DIR/$tdir
}
run_test 300c "chown && check ls under striped directory"

test_300d() {
	[ $MDS1_VERSION -lt $(version_code 2.7.0) ] &&
		skip "skipped for lustre < 2.7.0"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	local stripe_count
	local file

	mkdir -p $DIR/$tdir
	$SETSTRIPE -c 2 $DIR/$tdir

	#local striped directory
	$LFS setdirstripe -i 0 -c 2 -H all_char $DIR/$tdir/striped_dir ||
		error "set striped dir error"
        #look at the directories for debug purposes
	ls -l $DIR/$tdir
	$LFS getdirstripe $DIR/$tdir
	ls -l $DIR/$tdir/striped_dir
	$LFS getdirstripe $DIR/$tdir/striped_dir
	createmany -o $DIR/$tdir/striped_dir/f 10 ||
		error "create 10 files failed"

	#remote striped directory
	$LFS setdirstripe -i 1 -c 2 $DIR/$tdir/remote_striped_dir ||
		error "set striped dir error"
        #look at the directories for debug purposes
	ls -l $DIR/$tdir
	$LFS getdirstripe $DIR/$tdir
	ls -l $DIR/$tdir/remote_striped_dir
	$LFS getdirstripe $DIR/$tdir/remote_striped_dir
	createmany -o $DIR/$tdir/remote_striped_dir/f 10 ||
		error "create 10 files failed"

	for file in $(find $DIR/$tdir); do
		stripe_count=$($LFS getstripe -c $file)
		[ $stripe_count -eq 2 ] ||
			error "wrong stripe $stripe_count for $file"
	done

	rm -rf $DIR/$tdir
}
run_test 300d "check default stripe under striped directory"

test_300e() {
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	local stripe_count
	local file

	mkdir -p $DIR/$tdir

	$LFS setdirstripe -i 0 -c 2 -H all_char $DIR/$tdir/striped_dir ||
		error "set striped dir error"

	touch $DIR/$tdir/striped_dir/a
	touch $DIR/$tdir/striped_dir/b
	touch $DIR/$tdir/striped_dir/c

	mkdir $DIR/$tdir/striped_dir/dir_a
	mkdir $DIR/$tdir/striped_dir/dir_b
	mkdir $DIR/$tdir/striped_dir/dir_c

	$LFS setdirstripe -i 0 -c 2 -H all_char $DIR/$tdir/striped_dir/stp_a ||
		error "set striped adir under striped dir error"

	$LFS setdirstripe -i 0 -c 2 -H all_char $DIR/$tdir/striped_dir/stp_b ||
		error "set striped bdir under striped dir error"

	$LFS setdirstripe -i 0 -c 2 -H all_char $DIR/$tdir/striped_dir/stp_c ||
		error "set striped cdir under striped dir error"

	mrename $DIR/$tdir/striped_dir/dir_a $DIR/$tdir/striped_dir/dir_b ||
		error "rename dir under striped dir fails"

	mrename $DIR/$tdir/striped_dir/stp_a $DIR/$tdir/striped_dir/stp_b ||
		error "rename dir under different stripes fails"

	mrename $DIR/$tdir/striped_dir/a $DIR/$tdir/striped_dir/c ||
		error "rename file under striped dir should succeed"

	mrename $DIR/$tdir/striped_dir/dir_b $DIR/$tdir/striped_dir/dir_c ||
		error "rename dir under striped dir should succeed"

	rm -rf $DIR/$tdir
}
run_test 300e "check rename under striped directory"

test_300f() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	local stripe_count
	local file

	rm -rf $DIR/$tdir
	mkdir -p $DIR/$tdir

	$LFS setdirstripe -i 0 -c 2 -H all_char $DIR/$tdir/striped_dir ||
		error "set striped dir error"

	$LFS setdirstripe -i 0 -c 2 -H all_char $DIR/$tdir/striped_dir1 ||
		error "set striped dir error"

	touch $DIR/$tdir/striped_dir/a
	mkdir $DIR/$tdir/striped_dir/dir_a
	$LFS setdirstripe -i 0 -c 2 $DIR/$tdir/striped_dir/stp_a ||
		error "create striped dir under striped dir fails"

	touch $DIR/$tdir/striped_dir1/b
	mkdir $DIR/$tdir/striped_dir1/dir_b
	$LFS setdirstripe -i 0 -c 2 $DIR/$tdir/striped_dir/stp_b ||
		error "create striped dir under striped dir fails"

	mrename $DIR/$tdir/striped_dir/dir_a $DIR/$tdir/striped_dir1/dir_b ||
		error "rename dir under different striped dir should fail"

	mrename $DIR/$tdir/striped_dir/stp_a $DIR/$tdir/striped_dir1/stp_b ||
		error "rename striped dir under diff striped dir should fail"

	mrename $DIR/$tdir/striped_dir/a $DIR/$tdir/striped_dir1/a ||
		error "rename file under diff striped dirs fails"

	rm -rf $DIR/$tdir
}
run_test 300f "check rename cross striped directory"

test_300_check_default_striped_dir()
{
	local dirname=$1
	local default_count=$2
	local default_index=$3
	local stripe_count
	local stripe_index
	local dir_stripe_index
	local dir

	echo "checking $dirname $default_count $default_index"
	$LFS setdirstripe -D -c $default_count -i $default_index \
				-t all_char $DIR/$tdir/$dirname ||
		error "set default stripe on striped dir error"
	stripe_count=$($LFS getdirstripe -D -c $DIR/$tdir/$dirname)
	[ $stripe_count -eq $default_count ] ||
		error "expect $default_count get $stripe_count for $dirname"

	stripe_index=$($LFS getdirstripe -D -i $DIR/$tdir/$dirname)
	[ $stripe_index -eq $default_index ] ||
		error "expect $default_index get $stripe_index for $dirname"

	mkdir $DIR/$tdir/$dirname/{test1,test2,test3,test4} ||
						error "create dirs failed"

	createmany -o $DIR/$tdir/$dirname/f- 10 || error "create files failed"
	unlinkmany $DIR/$tdir/$dirname/f- 10	|| error "unlink files failed"
	for dir in $(find $DIR/$tdir/$dirname/*); do
		stripe_count=$($LFS getdirstripe -c $dir)
		[ $stripe_count -eq $default_count ] ||
		[ $stripe_count -eq 0 -o $default_count -eq 1 ] ||
		error "stripe count $default_count != $stripe_count for $dir"

		stripe_index=$($LFS getdirstripe -i $dir)
		[ $default_index -eq -1 -o $stripe_index -eq $default_index ] ||
			error "$stripe_index != $default_index for $dir"

		#check default stripe
		stripe_count=$($LFS getdirstripe -D -c $dir)
		[ $stripe_count -eq $default_count ] ||
		error "default count $default_count != $stripe_count for $dir"

		stripe_index=$($LFS getdirstripe -D -i $dir)
		[ $stripe_index -eq $default_index ] ||
		error "default index $default_index != $stripe_index for $dir"
	done
	rmdir $DIR/$tdir/$dirname/* || error "rmdir failed"
}

test_300g() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	local dir
	local stripe_count
	local stripe_index

	mkdir $DIR/$tdir
	mkdir $DIR/$tdir/normal_dir

	#Checking when client cache stripe index
	$LFS setdirstripe -c$MDSCOUNT $DIR/$tdir/striped_dir
	$LFS setdirstripe -D -i1 $DIR/$tdir/striped_dir ||
		error "create striped_dir failed"

	$LFS setdirstripe -i0 $DIR/$tdir/striped_dir/dir0 ||
		error "create dir0 fails"
	stripe_index=$($LFS getdirstripe -i $DIR/$tdir/striped_dir/dir0)
	[ $stripe_index -eq 0 ] ||
		error "dir0 expect index 0 got $stripe_index"

	mkdir $DIR/$tdir/striped_dir/dir1 ||
		error "create dir1 fails"
	stripe_index=$($LFS getdirstripe -i $DIR/$tdir/striped_dir/dir1)
	[ $stripe_index -eq 1 ] ||
		error "dir1 expect index 1 got $stripe_index"

	#check default stripe count/stripe index
	test_300_check_default_striped_dir normal_dir $MDSCOUNT 1
	test_300_check_default_striped_dir normal_dir 1 0
	test_300_check_default_striped_dir normal_dir 2 1
	test_300_check_default_striped_dir normal_dir 2 -1

	#delete default stripe information
	echo "delete default stripeEA"
	$LFS setdirstripe -d $DIR/$tdir/normal_dir ||
		error "set default stripe on striped dir error"

	mkdir -p $DIR/$tdir/normal_dir/{test1,test2,test3,test4}
	for dir in $(find $DIR/$tdir/normal_dir/*); do
		stripe_count=$($LFS getdirstripe -c $dir)
		[ $stripe_count -eq 0 ] ||
			error "expect 1 get $stripe_count for $dir"
		stripe_index=$($LFS getdirstripe -i $dir)
		[ $stripe_index -eq 0 ] ||
			error "expect 0 get $stripe_index for $dir"
	done
}
run_test 300g "check default striped directory for normal directory"

test_300h() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	local dir
	local stripe_count

	mkdir $DIR/$tdir
	$LFS setdirstripe -i0 -c$MDSCOUNT -H all_char $DIR/$tdir/striped_dir ||
		error "set striped dir error"

	test_300_check_default_striped_dir striped_dir $MDSCOUNT 1
	test_300_check_default_striped_dir striped_dir 1 0
	test_300_check_default_striped_dir striped_dir 2 1
	test_300_check_default_striped_dir striped_dir 2 -1

	#delete default stripe information
	$LFS setdirstripe -d $DIR/$tdir/striped_dir ||
		error "set default stripe on striped dir error"

	mkdir -p $DIR/$tdir/striped_dir/{test1,test2,test3,test4}
	for dir in $(find $DIR/$tdir/striped_dir/*); do
		stripe_count=$($LFS getdirstripe -c $dir)
		[ $stripe_count -eq 0 ] ||
			error "expect 1 get $stripe_count for $dir"
	done
}
run_test 300h "check default striped directory for striped directory"

test_300i() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	local stripe_count
	local file

	mkdir $DIR/$tdir

	$LFS setdirstripe -i 0 -c$MDSCOUNT -H all_char $DIR/$tdir/striped_dir ||
		error "set striped dir error"

	createmany -o $DIR/$tdir/striped_dir/f- 10 ||
		error "create files under striped dir failed"

	$LFS setdirstripe -i0 -c$MDSCOUNT -H all_char $DIR/$tdir/hashdir ||
		error "set striped hashdir error"

	$LFS setdirstripe -i0 -c$MDSCOUNT -H all_char $DIR/$tdir/hashdir/d0 ||
		error "create dir0 under hash dir failed"
	$LFS setdirstripe -i0 -c$MDSCOUNT -H fnv_1a_64 $DIR/$tdir/hashdir/d1 ||
		error "create dir1 under hash dir failed"

	# unfortunately, we need to umount to clear dir layout cache for now
	# once we fully implement dir layout, we can drop this
	umount_client $MOUNT || error "umount failed"
	mount_client $MOUNT || error "mount failed"

	$LFS find -H fnv_1a_64 $DIR/$tdir/hashdir
	local dircnt=$($LFS find -H fnv_1a_64 $DIR/$tdir/hashdir | wc -l)
	[ $dircnt -eq 1 ] || error "lfs find striped dir got:$dircnt,except:1"

	#set the stripe to be unknown hash type
	#define OBD_FAIL_UNKNOWN_LMV_STRIPE	0x1901
	$LCTL set_param fail_loc=0x1901
	for ((i = 0; i < 10; i++)); do
		$CHECKSTAT -t file $DIR/$tdir/striped_dir/f-$i ||
			error "stat f-$i failed"
		rm $DIR/$tdir/striped_dir/f-$i || error "unlink f-$i failed"
	done

	touch $DIR/$tdir/striped_dir/f0 &&
		error "create under striped dir with unknown hash should fail"

	$LCTL set_param fail_loc=0

	umount_client $MOUNT || error "umount failed"
	mount_client $MOUNT || error "mount failed"

	return 0
}
run_test 300i "client handle unknown hash type striped directory"

test_300j() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	local stripe_count
	local file

	mkdir $DIR/$tdir

	#define OBD_FAIL_SPLIT_UPDATE_REC	0x1702
	$LCTL set_param fail_loc=0x1702
	$LFS setdirstripe -i 0 -c$MDSCOUNT -H all_char $DIR/$tdir/striped_dir ||
		error "set striped dir error"

	createmany -o $DIR/$tdir/striped_dir/f- 10 ||
		error "create files under striped dir failed"

	$LCTL set_param fail_loc=0

	rm -rf $DIR/$tdir || error "unlink striped dir fails"

	return 0
}
run_test 300j "test large update record"

test_300k() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	local stripe_count
	local file

	mkdir $DIR/$tdir

	#define OBD_FAIL_LARGE_STRIPE	0x1703
	$LCTL set_param fail_loc=0x1703
	$LFS setdirstripe -i 0 -c192 $DIR/$tdir/striped_dir ||
		error "set striped dir error"
	$LCTL set_param fail_loc=0

	$LFS getdirstripe $DIR/$tdir/striped_dir ||
		error "getstripeddir fails"
	rm -rf $DIR/$tdir/striped_dir ||
		error "unlink striped dir fails"

	return 0
}
run_test 300k "test large striped directory"

test_300l() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	local stripe_index

	test_mkdir -p $DIR/$tdir/striped_dir
	chown $RUNAS_ID $DIR/$tdir/striped_dir ||
			error "chown $RUNAS_ID failed"
	$LFS setdirstripe -i 1 -D $DIR/$tdir/striped_dir ||
		error "set default striped dir failed"

	#define OBD_FAIL_MDS_STALE_DIR_LAYOUT	 0x158
	$LCTL set_param fail_loc=0x80000158
	$RUNAS mkdir $DIR/$tdir/striped_dir/test_dir || error "create dir fails"

	stripe_index=$($LFS getdirstripe -i $DIR/$tdir/striped_dir/test_dir)
	[ $stripe_index -eq 1 ] ||
		error "expect 1 get $stripe_index for $dir"
}
run_test 300l "non-root user to create dir under striped dir with stale layout"

test_300m() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -ge 2 ] && skip_env "Only for single MDT"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	mkdir -p $DIR/$tdir/striped_dir
	$LFS setdirstripe -D -c 1 $DIR/$tdir/striped_dir ||
		error "set default stripes dir error"

	mkdir $DIR/$tdir/striped_dir/a || error "mkdir a fails"

	stripe_count=$($LFS getdirstripe -c $DIR/$tdir/striped_dir/a)
	[ $stripe_count -eq 0 ] ||
			error "expect 0 get $stripe_count for a"

	$LFS setdirstripe -D -c 2 $DIR/$tdir/striped_dir ||
		error "set default stripes dir error"

	mkdir $DIR/$tdir/striped_dir/b || error "mkdir b fails"

	stripe_count=$($LFS getdirstripe -c $DIR/$tdir/striped_dir/b)
	[ $stripe_count -eq 0 ] ||
			error "expect 0 get $stripe_count for b"

	$LFS setdirstripe -D -c1 -i2 $DIR/$tdir/striped_dir ||
		error "set default stripes dir error"

	mkdir $DIR/$tdir/striped_dir/c &&
		error "default stripe_index is invalid, mkdir c should fails"

	rm -rf $DIR/$tdir || error "rmdir fails"
}
run_test 300m "setstriped directory on single MDT FS"

cleanup_300n() {
	local list=$(comma_list $(mdts_nodes))

	trap 0
	do_nodes $list $LCTL set_param -n mdt.*.enable_remote_dir_gid=0
}

test_300n() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local stripe_index
	local list=$(comma_list $(mdts_nodes))

	trap cleanup_300n RETURN EXIT
	mkdir -p $DIR/$tdir
	chmod 777 $DIR/$tdir
	$RUNAS $LFS setdirstripe -i0 -c$MDSCOUNT \
				$DIR/$tdir/striped_dir > /dev/null 2>&1 &&
		error "create striped dir succeeds with gid=0"

	do_nodes $list $LCTL set_param -n mdt.*.enable_remote_dir_gid=-1
	$RUNAS $LFS setdirstripe -i0 -c$MDSCOUNT $DIR/$tdir/striped_dir ||
		error "create striped dir fails with gid=-1"

	do_nodes $list $LCTL set_param -n mdt.*.enable_remote_dir_gid=0
	$RUNAS $LFS setdirstripe -i 1 -c$MDSCOUNT -D \
				$DIR/$tdir/striped_dir > /dev/null 2>&1 &&
		error "set default striped dir succeeds with gid=0"


	do_nodes $list $LCTL set_param -n mdt.*.enable_remote_dir_gid=-1
	$RUNAS $LFS setdirstripe -i 1 -c$MDSCOUNT -D $DIR/$tdir/striped_dir ||
		error "set default striped dir fails with gid=-1"


	do_nodes $list $LCTL set_param -n mdt.*.enable_remote_dir_gid=0
	$RUNAS mkdir $DIR/$tdir/striped_dir/test_dir ||
					error "create test_dir fails"
	$RUNAS mkdir $DIR/$tdir/striped_dir/test_dir1 ||
					error "create test_dir1 fails"
	$RUNAS mkdir $DIR/$tdir/striped_dir/test_dir2 ||
					error "create test_dir2 fails"
	cleanup_300n
}
run_test 300n "non-root user to create dir under striped dir with default EA"

test_300o() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55"

	local numfree1
	local numfree2

	mkdir -p $DIR/$tdir

	numfree1=$(lctl get_param -n mdc.*MDT0000*.filesfree)
	numfree2=$(lctl get_param -n mdc.*MDT0001*.filesfree)
	if [ $numfree1 -lt 66000 -o $numfree2 -lt 66000 ]; then
		skip "not enough free inodes $numfree1 $numfree2"
	fi

	numfree1=$(lctl get_param -n mdc.*MDT0000-mdc-*.kbytesfree)
	numfree2=$(lctl get_param -n mdc.*MDT0001-mdc-*.kbytesfree)
	if [ $numfree1 -lt 300000 -o $numfree2 -lt 300000 ]; then
		skip "not enough free space $numfree1 $numfree2"
	fi

	$LFS setdirstripe -c2 $DIR/$tdir/striped_dir ||
		error "setdirstripe fails"

	createmany -d $DIR/$tdir/striped_dir/d 131000 ||
		error "create dirs fails"

	$LCTL set_param ldlm.namespaces.*mdc-*.lru_size=0
	ls $DIR/$tdir/striped_dir > /dev/null ||
		error "ls striped dir fails"
	unlinkmany -d $DIR/$tdir/striped_dir/d 131000 ||
		error "unlink big striped dir fails"
}
run_test 300o "unlink big sub stripe(> 65000 subdirs)"

test_300p() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	mkdir -p $DIR/$tdir

	#define OBD_FAIL_OUT_ENOSPC	0x1704
	do_facet mds2 lctl set_param fail_loc=0x80001704
	$LFS setdirstripe -i 0 -c2 $DIR/$tdir/bad_striped_dir > /dev/null 2>&1 \
		 && error "create striped directory should fail"

	[ -e $DIR/$tdir/bad_striped_dir ] && error "striped dir exists"

	$LFS setdirstripe -c2 $DIR/$tdir/bad_striped_dir
	true
}
run_test 300p "create striped directory without space"

test_300q() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"

	local fd=$(free_fd)
	local cmd="exec $fd<$tdir"
	cd $DIR
	$LFS mkdir -c $MDSCOUNT $tdir || error "create $tdir fails"
	eval $cmd
	cmd="exec $fd<&-"
	trap "eval $cmd" EXIT
	cd $tdir || error "cd $tdir fails"
	rmdir  ../$tdir || error "rmdir $tdir fails"
	mkdir local_dir && error "create dir succeeds"
	$LFS setdirstripe -i1 remote_dir && error "create remote dir succeeds"
	eval $cmd
	return 0
}
run_test 300q "create remote directory under orphan directory"

test_300r() {
	[ $(lustre_version_code $SINGLEMDS) -lt $(version_code 2.7.55) ] &&
		skip "Need MDS version at least 2.7.55" && return
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs" && return

	mkdir $DIR/$tdir

	$LFS setdirstripe -i 0 -c -1 $DIR/$tdir/striped_dir ||
		error "set striped dir error"

	$LFS getdirstripe $DIR/$tdir/striped_dir ||
		error "getstripeddir fails"

	local stripe_count
	stripe_count=$($LFS getdirstripe $DIR/$tdir/striped_dir |
		      awk '/lmv_stripe_count:/ { print $2 }')

	[ $MDSCOUNT -ne $stripe_count ] &&
		error "wrong stripe count $stripe_count expected $MDSCOUNT"

	rm -rf $DIR/$tdir/striped_dir ||
		error "unlink striped dir fails"
}
run_test 300r "test -1 striped directory"

prepare_remote_file() {
	mkdir $DIR/$tdir/src_dir ||
		error "create remote source failed"

	cp /etc/hosts $DIR/$tdir/src_dir/a ||
		 error "cp to remote source failed"
	touch $DIR/$tdir/src_dir/a

	$LFS mkdir -i 1 $DIR/$tdir/tgt_dir ||
		error "create remote target dir failed"

	touch $DIR/$tdir/tgt_dir/b

	mrename $DIR/$tdir/src_dir/a $DIR/$tdir/tgt_dir/b ||
		error "rename dir cross MDT failed!"

	$CHECKSTAT -t file $DIR/$tdir/src_dir/a &&
		error "src_child still exists after rename"

	$CHECKSTAT -t file $DIR/$tdir/tgt_dir/b ||
		error "missing file(a) after rename"

	diff /etc/hosts $DIR/$tdir/tgt_dir/b ||
		error "diff after rename"
}

test_310a() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 4 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local remote_file=$DIR/$tdir/tgt_dir/b

	mkdir -p $DIR/$tdir

	prepare_remote_file || error "prepare remote file failed"

	#open-unlink file
	$OPENUNLINK $remote_file $remote_file ||
		error "openunlink $remote_file failed"
	$CHECKSTAT -a $remote_file || error "$remote_file exists"
}
run_test 310a "open unlink remote file"

test_310b() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 4 MDTs"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local remote_file=$DIR/$tdir/tgt_dir/b

	mkdir -p $DIR/$tdir

	prepare_remote_file || error "prepare remote file failed"

	ln $remote_file $DIR/$tfile || error "link failed for remote file"
	$MULTIOP $DIR/$tfile Ouc || error "mulitop failed"
	$CHECKSTAT -t file $remote_file || error "check file failed"
}
run_test 310b "unlink remote file with multiple links while open"

test_310c() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[[ $MDSCOUNT -lt 4 ]] && skip_env "needs >= 4 MDTs"

	local remote_file=$DIR/$tdir/tgt_dir/b

	mkdir -p $DIR/$tdir

	prepare_remote_file || error "prepare remote file failed"

	ln $remote_file $DIR/$tfile || error "link failed for remote file"
	multiop_bg_pause $remote_file O_uc ||
			error "mulitop failed for remote file"
	MULTIPID=$!
	$MULTIOP $DIR/$tfile Ouc
	kill -USR1 $MULTIPID
	wait $MULTIPID
}
run_test 310c "open-unlink remote file with multiple links"

#LU-4825
test_311() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $OSTCOUNT -lt 2 ] && skip "needs >= 2 OSTs"
	[ $MDS1_VERSION -lt $(version_code 2.8.54) ] &&
		skip "lustre < 2.8.54 does not contain LU-4825 fix"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local old_iused=$($LFS df -i | grep OST0000 | awk '{ print $3 }')
	local mdts=$(comma_list $(mdts_nodes))

	mkdir -p $DIR/$tdir
	$SETSTRIPE -i 0 -c 1 $DIR/$tdir
	createmany -o $DIR/$tdir/$tfile. 1000

	# statfs data is not real time, let's just calculate it
	old_iused=$((old_iused + 1000))

	local count=$(do_facet $SINGLEMDS "$LCTL get_param -n \
			osp.*OST0000*MDT0000.create_count")
	local max_count=$(do_facet $SINGLEMDS "$LCTL get_param -n \
				osp.*OST0000*MDT0000.max_create_count")
	do_nodes $mdts "$LCTL set_param -n osp.*OST0000*.max_create_count=0"

	$SETSTRIPE -i 0 $DIR/$tdir/$tfile || error "setstripe failed"
	local index=$($GETSTRIPE -i $DIR/$tdir/$tfile)
	[ $index -ne 0 ] || error "$tfile stripe index is 0"

	unlinkmany $DIR/$tdir/$tfile. 1000

	do_nodes $mdts "$LCTL set_param -n \
			osp.*OST0000*.max_create_count=$max_count"
	[ $MDS1_VERSION -lt $(version_code 2.12.51) ] &&
		do_nodes $mdts "$LCTL set_param -n \
				osp.*OST0000*.create_count=$count"
	do_nodes $mdts "$LCTL get_param osp.*OST0000*.create_count" |
			grep "=0" && error "create_count is zero"

	local new_iused
	for i in $(seq 120); do
		new_iused=$($LFS df -i | grep OST0000 | awk '{ print $3 }')
		# system may be too busy to destroy all objs in time, use
		# a somewhat small value to not fail autotest
		[ $((old_iused - new_iused)) -gt 400 ] && break
		sleep 1
	done

	echo "waited $i sec, old Iused $old_iused, new Iused $new_iused"
	[ $((old_iused - new_iused)) -gt 400 ] ||
		error "objs not destroyed after unlink"
}
run_test 311 "disable OSP precreate, and unlink should destroy objs"

zfs_oid_to_objid()
{
	local ost=$1
	local objid=$2

	local vdevdir=$(dirname $(facet_vdevice $ost))
	local cmd="$ZDB -e -p $vdevdir -ddddd $(facet_device $ost)"
	local zfs_zapid=$(do_facet $ost $cmd |
			  grep -w "/O/0/d$((objid%32))" -C 5 |
			  awk '/Object/{getline; print $1}')
	local zfs_objid=$(do_facet $ost $cmd $zfs_zapid |
			  awk "/$objid = /"'{printf $3}')

	echo $zfs_objid
}

zfs_object_blksz() {
	local ost=$1
	local objid=$2

	local vdevdir=$(dirname $(facet_vdevice $ost))
	local cmd="$ZDB -e -p $vdevdir -dddd $(facet_device $ost)"
	local blksz=$(do_facet $ost $cmd $objid |
		      awk '/dblk/{getline; printf $4}')

	case "${blksz: -1}" in
		k|K) blksz=$((${blksz:0:$((${#blksz} - 1))}*1024)) ;;
		m|M) blksz=$((${blksz:0:$((${#blksz} - 1))}*1024*1024)) ;;
		*) ;;
	esac

	echo $blksz
}

test_312() { # LU-4856
	remote_ost_nodsh && skip "remote OST with nodsh"
	[ "$ost1_FSTYPE" = "zfs" ] ||
		skip_env "the test only applies to zfs"

	local max_blksz=$(do_facet ost1 \
			  $ZFS get -p recordsize $(facet_device ost1) |
			  awk '!/VALUE/{print $3}')

	# to make life a little bit easier
	$LFS mkdir -c 1 -i 0 $DIR/$tdir
	$LFS setstripe -c 1 -i 0 $DIR/$tdir

	local tf=$DIR/$tdir/$tfile
	touch $tf
	local oid=$($LFS getstripe $tf | awk '/obdidx/{getline; print $2}')

	# Get ZFS object id
	local zfs_objid=$(zfs_oid_to_objid ost1 $oid)
	# block size change by sequential overwrite
	local bs

	for ((bs=$PAGE_SIZE; bs <= max_blksz; bs *= 4)) ; do
		dd if=/dev/zero of=$tf bs=$bs count=1 oflag=sync conv=notrunc

		local blksz=$(zfs_object_blksz ost1 $zfs_objid)
		[ $blksz -eq $bs ] || error "blksz error: $blksz, expected: $bs"
	done
	rm -f $tf

	# block size change by sequential append write
	dd if=/dev/zero of=$tf bs=$PAGE_SIZE count=1 oflag=sync conv=notrunc
	oid=$($LFS getstripe $tf | awk '/obdidx/{getline; print $2}')
	zfs_objid=$(zfs_oid_to_objid ost1 $oid)
	local count

	for ((count = 1; count < $((max_blksz / PAGE_SIZE)); count *= 2)); do
		dd if=/dev/zero of=$tf bs=$PAGE_SIZE count=$count seek=$count \
			oflag=sync conv=notrunc

		blksz=$(zfs_object_blksz ost1 $zfs_objid)
		[ $blksz -eq $((2 * count * PAGE_SIZE)) ] ||
			error "blksz error, actual $blksz, " \
				"expected: 2 * $count * $PAGE_SIZE"
	done
	rm -f $tf

	# random write
	touch $tf
	oid=$($LFS getstripe $tf | awk '/obdidx/{getline; print $2}')
	zfs_objid=$(zfs_oid_to_objid ost1 $oid)

	dd if=/dev/zero of=$tf bs=1K count=1 oflag=sync conv=notrunc
	blksz=$(zfs_object_blksz ost1 $zfs_objid)
	[ $blksz -eq $PAGE_SIZE ] ||
		error "blksz error: $blksz, expected: $PAGE_SIZE"

	dd if=/dev/zero of=$tf bs=64K count=1 oflag=sync conv=notrunc seek=128
	blksz=$(zfs_object_blksz ost1 $zfs_objid)
	[ $blksz -eq 65536 ] || error "blksz error: $blksz, expected: 64k"

	dd if=/dev/zero of=$tf bs=1M count=1 oflag=sync conv=notrunc
	blksz=$(zfs_object_blksz ost1 $zfs_objid)
	[ $blksz -eq 65536 ] || error "rewrite error: $blksz, expected: 64k"
}
run_test 312 "make sure ZFS adjusts its block size by write pattern"

test_313() {
	remote_ost_nodsh && skip "remote OST with nodsh"

	local file=$DIR/$tfile

	rm -f $file
	$SETSTRIPE -c 1 -i 0 $file || error "setstripe failed"

	# define OBD_FAIL_TGT_RCVD_EIO		 0x720
	do_facet ost1 "$LCTL set_param fail_loc=0x720"
	dd if=/dev/zero of=$file bs=$PAGE_SIZE oflag=direct count=1 &&
		error "write should failed"
	do_facet ost1 "$LCTL set_param fail_loc=0"
	rm -f $file
}
run_test 313 "io should fail after last_rcvd update fail"

test_314() {
	[ $OSTCOUNT -lt 2 ] && skip "needs >= 2 OSTs"

	$SETSTRIPE -c 2 -i 0 $DIR/$tfile || error "setstripe failed"
	do_facet ost1 "$LCTL set_param fail_loc=0x720"
	rm -f $DIR/$tfile
	wait_delete_completed
	do_facet ost1 "$LCTL set_param fail_loc=0"
}
run_test 314 "OSP shouldn't fail after last_rcvd update failure"

test_315() { # LU-618
	local file=$DIR/$tfile
	rm -f $file

	$MULTIOP $file oO_CREAT:O_DIRECT:O_RDWR:w4063232c ||
		error "multiop file write failed"
	$MULTIOP $file oO_RDONLY:r4063232_c &
	PID=$!

	sleep 2

	local rbytes=$(awk '/read_bytes/ { print $2 }' /proc/$PID/io)
	kill -USR1 $PID

	[ $rbytes -gt 4000000 ] || error "read is not accounted ($rbytes)"
	rm -f $file
}
run_test 315 "read should be accounted"

test_316() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	large_xattr_enabled || skip_env "ea_inode feature disabled"

	rm -rf $DIR/$tdir/d
	mkdir -p $DIR/$tdir/d
	chown nobody $DIR/$tdir/d
	touch $DIR/$tdir/d/file

	$LFS mv -M1 $DIR/$tdir/d || error "lfs mv failed"
}
run_test 316 "lfs mv"

test_317() {
	[ $MDS1_VERSION -lt $(version_code 2.11.53) ] &&
		skip "Need MDS version at least 2.11.53"
	if [ "$ost1_FSTYPE" == "zfs" ]; then
		skip "LU-10370: no implementation for ZFS"
	fi

	local trunc_sz
	local grant_blk_size

	grant_blk_size=$($LCTL get_param osc.$FSNAME*.import |
			awk '/grant_block_size:/ { print $2; exit; }')
	#
	# Create File of size 5M. Truncate it to below size's and verify
	# blocks count.
	#
	dd if=/dev/zero of=$DIR/$tfile bs=5M count=1 conv=fsync ||
		error "Create file $DIR/$tfile failed"
	stack_trap "rm -f $DIR/$tfile" EXIT

	for trunc_sz in 2097152 4097 4000 509 0; do
		$TRUNCATE $DIR/$tfile $trunc_sz ||
			error "truncate $tfile to $trunc_sz failed"
		local sz=$(stat --format=%s $DIR/$tfile)
		local blk=$(stat --format=%b $DIR/$tfile)
		local trunc_blk=$((((trunc_sz + (grant_blk_size - 1) ) /
				     grant_blk_size) * 8))

		if [[ $blk -ne $trunc_blk ]]; then
			$(which stat) $DIR/$tfile
			error "Expected Block $trunc_blk got $blk for $tfile"
		fi

		$CHECKSTAT -s $trunc_sz $DIR/$tfile ||
			error "Expected Size $trunc_sz got $sz for $tfile"
	done

	#
	# sparse file test
	# Create file with a hole and write actual two blocks. Block count
	# must be 16.
	#
	dd if=/dev/zero of=$DIR/$tfile bs=$grant_blk_size count=2 seek=5 \
		conv=fsync || error "Create file : $DIR/$tfile"

	# Calculate the final truncate size.
	trunc_sz=$(($(stat --format=%s $DIR/$tfile) - (grant_blk_size + 1)))

	#
	# truncate to size $trunc_sz bytes. Strip the last block
	# The block count must drop to 8
	#
	$TRUNCATE $DIR/$tfile $trunc_sz ||
		error "truncate $tfile to $trunc_sz failed"

	local trunc_bsz=$((grant_blk_size / $(stat --format=%B $DIR/$tfile)))
	sz=$(stat --format=%s $DIR/$tfile)
	blk=$(stat --format=%b $DIR/$tfile)

	if [[ $blk -ne $trunc_bsz ]]; then
		$(which stat) $DIR/$tfile
		error "Expected Block $trunc_bsz got $blk for $tfile"
	fi

	$CHECKSTAT -s $trunc_sz $DIR/$tfile ||
		error "Expected Size $trunc_sz got $sz for $tfile"
}
run_test 317 "Verify blocks get correctly update after truncate"

test_fake_rw() {
	local read_write=$1
	if [ "$read_write" = "write" ]; then
		local dd_cmd="dd if=/dev/zero of=$DIR/$tfile"
	elif [ "$read_write" = "read" ]; then
		local dd_cmd="dd of=/dev/null if=$DIR/$tfile"
	else
		error "argument error"
	fi

	# turn off debug for performance testing
	local saved_debug=$($LCTL get_param -n debug)
	$LCTL set_param debug=0

	$SETSTRIPE -c 1 -i 0 $DIR/$tfile

	# get ost1 size - lustre-OST0000
	local ost1_avail_size=$($LFS df | awk /${ost1_svc}/'{ print $4 }')
	local blocks=$((ost1_avail_size/2/1024)) # half avail space by megabytes
	[ $blocks -gt 1000 ] && blocks=1000 # 1G in maximum

	if [ "$read_write" = "read" ]; then
		truncate -s $(expr 1048576 \* $blocks) $DIR/$tfile
	fi

	local start_time=$(date +%s.%N)
	$dd_cmd bs=1M count=$blocks oflag=sync ||
		error "real dd $read_write error"
	local duration=$(bc <<< "$(date +%s.%N) - $start_time")

	if [ "$read_write" = "write" ]; then
		rm -f $DIR/$tfile
	fi

	# define OBD_FAIL_OST_FAKE_RW		0x238
	do_facet ost1 $LCTL set_param fail_loc=0x238

	local start_time=$(date +%s.%N)
	$dd_cmd bs=1M count=$blocks oflag=sync ||
		error "fake dd $read_write error"
	local duration_fake=$(bc <<< "$(date +%s.%N) - $start_time")

	if [ "$read_write" = "write" ]; then
		# verify file size
		cancel_lru_locks osc
		$CHECKSTAT -t file -s $((blocks * 1024 * 1024)) $DIR/$tfile ||
			error "$tfile size not $blocks MB"
	fi
	do_facet ost1 $LCTL set_param fail_loc=0

	echo "fake $read_write $duration_fake vs. normal $read_write" \
		"$duration in seconds"
	[ $(bc <<< "$duration_fake < $duration") -eq 1 ] ||
		error_not_in_vm "fake write is slower"

	$LCTL set_param -n debug="$saved_debug"
	rm -f $DIR/$tfile
}
test_399a() { # LU-7655 for OST fake write
	remote_ost_nodsh && skip "remote OST with nodsh"

	test_fake_rw write
}
run_test 399a "fake write should not be slower than normal write"

test_399b() { # LU-8726 for OST fake read
	remote_ost_nodsh && skip "remote OST with nodsh"
	if [ "$ost1_FSTYPE" != "ldiskfs" ]; then
		skip_env "ldiskfs only test"
	fi

	test_fake_rw read
}
run_test 399b "fake read should not be slower than normal read"

test_400a() { # LU-1606, was conf-sanity test_74
	if ! which $CC > /dev/null 2>&1; then
		skip_env "$CC is not installed"
	fi

	local extra_flags=''
	local out=$TMP/$tfile
	local prefix=/usr/include/lustre
	local prog

	# Oleg removes c files in his test rig so test if any c files exist
	[ -z "$(ls -A $LUSTRE_TESTS_API_DIR)" ] && \
		skip_env "Needed c test files are missing"

	if ! [[ -d $prefix ]]; then
		# Assume we're running in tree and fixup the include path.
		extra_flags+=" -I$LUSTRE/../lnet/include/uapi -I$LUSTRE/include/uapi -I$LUSTRE/include"
		extra_flags+=" -L$LUSTRE/utils/.lib"
	fi

	for prog in $LUSTRE_TESTS_API_DIR/*.c; do
		$CC -Wall -Werror -std=c99 $extra_flags -o $out $prog -llustreapi ||
			error "client api broken"
	done
	rm -f $out
}
run_test 400a "Lustre client api program can compile and link"

test_400b() { # LU-1606, LU-5011
	local header
	local out=$TMP/$tfile
	local prefix=/usr/include/linux/lustre

	# We use a hard coded prefix so that this test will not fail
	# when run in tree. There are headers in lustre/include/lustre/
	# that are not packaged (like lustre_idl.h) and have more
	# complicated include dependencies (like config.h and lnet/types.h).
	# Since this test about correct packaging we just skip them when
	# they don't exist (see below) rather than try to fixup cppflags.

	if ! which $CC > /dev/null 2>&1; then
		skip_env "$CC is not installed"
	fi

	for header in $prefix/*.h; do
		if ! [[ -f "$header" ]]; then
			continue
		fi

		if [[ "$(basename $header)" == lustre_ioctl.h ]]; then
			continue # lustre_ioctl.h is internal header
		fi

		$CC -Wall -Werror -std=c99 -include $header -c -x c /dev/null -o $out ||
			error "cannot compile '$header'"
	done
	rm -f $out
}
run_test 400b "packaged headers can be compiled"

test_401a() { #LU-7437
	local printf_arg=$(find -printf 2>&1 | grep "unrecognized:")
	[ -n "$printf_arg" ] && skip_env "find does not support -printf"

	#count the number of parameters by "list_param -R"
	local params=$($LCTL list_param -R '*' 2>/dev/null | wc -l)
	#count the number of parameters by listing proc files
	local proc_dirs=$(eval \ls -d $proc_regexp 2>/dev/null)
	echo "proc_dirs='$proc_dirs'"
	[ -n "$proc_dirs" ] || error "no proc_dirs on $HOSTNAME"
	local procs=$(find -L $proc_dirs -mindepth 1 -printf '%P\n' 2>/dev/null|
		      sort -u | wc -l)

	[ $params -eq $procs ] ||
		error "found $params parameters vs. $procs proc files"

	# test the list_param -D option only returns directories
	params=$($LCTL list_param -R -D '*' 2>/dev/null | wc -l)
	#count the number of parameters by listing proc directories
	procs=$(find -L $proc_dirs -mindepth 1 -type d -printf '%P\n' 2>/dev/null |
		sort -u | wc -l)

	[ $params -eq $procs ] ||
		error "found $params parameters vs. $procs proc files"
}
run_test 401a "Verify if 'lctl list_param -R' can list parameters recursively"

test_401b() {
	local save=$($LCTL get_param -n jobid_var)
	local tmp=testing

	$LCTL set_param foo=bar jobid_var=$tmp bar=baz &&
		error "no error returned when setting bad parameters"

	local jobid_new=$($LCTL get_param -n foe jobid_var baz)
	[[ "$jobid_new" == "$tmp" ]] || error "jobid tmp $jobid_new != $tmp"

	$LCTL set_param -n fog=bam jobid_var=$save bat=fog
	local jobid_old=$($LCTL get_param -n foe jobid_var bag)
	[[ "$jobid_old" == "$save" ]] || error "jobid new $jobid_old != $save"
}
run_test 401b "Verify 'lctl {get,set}_param' continue after error"

test_401c() {
	local jobid_var_old=$($LCTL get_param -n jobid_var)
	local jobid_var_new

	$LCTL set_param jobid_var= &&
		error "no error returned for 'set_param a='"

	jobid_var_new=$($LCTL get_param -n jobid_var)
	[[ "$jobid_var_old" == "$jobid_var_new" ]] ||
		error "jobid_var was changed by setting without value"

	$LCTL set_param jobid_var &&
		error "no error returned for 'set_param a'"

	jobid_var_new=$($LCTL get_param -n jobid_var)
	[[ "$jobid_var_old" == "$jobid_var_new" ]] ||
		error "jobid_var was changed by setting without value"
}
run_test 401c "Verify 'lctl set_param' without value fails in either format."

test_401d() {
	local jobid_var_old=$($LCTL get_param -n jobid_var)
	local jobid_var_new
	local new_value="foo=bar"

	$LCTL set_param jobid_var=$new_value ||
		error "'set_param a=b' did not accept a value containing '='"

	jobid_var_new=$($LCTL get_param -n jobid_var)
	[[ "$jobid_var_new" == "$new_value" ]] ||
		error "'set_param a=b' failed on a value containing '='"

	# Reset the jobid_var to test the other format
	$LCTL set_param jobid_var=$jobid_var_old
	jobid_var_new=$($LCTL get_param -n jobid_var)
	[[ "$jobid_var_new" == "$jobid_var_old" ]] ||
		error "failed to reset jobid_var"

	$LCTL set_param jobid_var $new_value ||
		error "'set_param a b' did not accept a value containing '='"

	jobid_var_new=$($LCTL get_param -n jobid_var)
	[[ "$jobid_var_new" == "$new_value" ]] ||
		error "'set_param a b' failed on a value containing '='"

	$LCTL set_param jobid_var $jobid_var_old
	jobid_var_new=$($LCTL get_param -n jobid_var)
	[[ "$jobid_var_new" == "$jobid_var_old" ]] ||
		error "failed to reset jobid_var"
}
run_test 401d "Verify 'lctl set_param' accepts values containing '='"

test_402() {
	[[ $MDS1_VERSION -ge $(version_code 2.7.66) ]] ||
	[[ $MDS1_VERSION -ge $(version_code 2.7.18.4) &&
		$MDS1_VERSION -lt $(version_code 2.7.50) ]] ||
	[[ $MDS1_VERSION -ge $(version_code 2.7.2) &&
		$MDS1_VERSION -lt $(version_code 2.7.11) ]] ||
		skip "Need MDS version 2.7.2+ or 2.7.18.4+ or 2.7.66+"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	$LFS setdirstripe -i 0 $DIR/$tdir || error "setdirstripe -i 0 failed"
#define OBD_FAIL_MDS_FLD_LOOKUP 0x15c
	do_facet mds1 "lctl set_param fail_loc=0x8000015c"
	touch $DIR/$tdir/$tfile && error "touch should fail with ENOENT" ||
		echo "Touch failed - OK"
}
run_test 402 "Return ENOENT to lod_generate_and_set_lovea"

test_403() {
	local file1=$DIR/$tfile.1
	local file2=$DIR/$tfile.2
	local tfile=$TMP/$tfile

	rm -f $file1 $file2 $tfile

	touch $file1
	ln $file1 $file2

	# 30 sec OBD_TIMEOUT in ll_getattr()
	# right before populating st_nlink
	$LCTL set_param fail_loc=0x80001409
	stat -c %h $file1 > $tfile &

	# create an alias, drop all locks and reclaim the dentry
	< $file2
	cancel_lru_locks mdc
	cancel_lru_locks osc
	sysctl -w vm.drop_caches=2

	wait

	[ $(cat $tfile) -gt 0 ] || error "wrong nlink count: $(cat $tfile)"

	rm -f $tfile $file1 $file2
}
run_test 403 "i_nlink should not drop to zero due to aliasing"

test_404() { # LU-6601
	[[ $MDS1_VERSION -ge $(version_code 2.8.53) ]] ||
		skip "Need server version newer than 2.8.52"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	local mosps=$(do_facet $SINGLEMDS $LCTL dl |
		awk '/osp .*-osc-MDT/ { print $4}')

	local osp
	for osp in $mosps; do
		echo "Deactivate: " $osp
		do_facet $SINGLEMDS $LCTL --device %$osp deactivate
		local stat=$(do_facet $SINGLEMDS $LCTL dl |
			awk -vp=$osp '$4 == p { print $2 }')
		[ $stat = IN ] || {
			do_facet $SINGLEMDS $LCTL dl | grep -w $osp
			error "deactivate error"
		}
		echo "Activate: " $osp
		do_facet $SINGLEMDS $LCTL --device %$osp activate
		local stat=$(do_facet $SINGLEMDS $LCTL dl |
			awk -vp=$osp '$4 == p { print $2 }')
		[ $stat = UP ] || {
			do_facet $SINGLEMDS $LCTL dl | grep -w $osp
			error "activate error"
		}
	done
}
run_test 404 "validate manual {de}activated works properly for OSPs"

test_405() {
	[ $MDS1_VERSION -lt $(version_code 2.6.92) ] ||
		[ $CLIENT_VERSION -lt $(version_code 2.6.99) ] &&
			skip "Layout swap lock is not supported"

	check_swap_layouts_support

	test_mkdir $DIR/$tdir
	swap_lock_test -d $DIR/$tdir ||
		error "One layout swap locked test failed"
}
run_test 405 "Various layout swap lock tests"

test_406() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[ $OSTCOUNT -lt 2 ] && skip_env "needs >= 2 OSTs"
	[ -n "$FILESET" ] && skip "SKIP due to FILESET set"
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $MDS1_VERSION -lt $(version_code 2.8.50) ] &&
		skip "Need MDS version at least 2.8.50"

	local def_stripe_size=$($LFS getstripe -S $MOUNT)
	local test_pool=$TESTNAME

	pool_add $test_pool || error "pool_add failed"
	pool_add_targets $test_pool 0 $(($OSTCOUNT - 1)) 1 ||
		error "pool_add_targets failed"

	save_layout_restore_at_exit $MOUNT

	# parent set default stripe count only, child will stripe from both
	# parent and fs default
	$LFS setstripe -c 1 -i 1 -S $((def_stripe_size * 2)) -p $test_pool $MOUNT ||
		error "setstripe $MOUNT failed"
	$LFS mkdir -c $MDSCOUNT $DIR/$tdir || error "mkdir $tdir failed"
	$LFS setstripe -c $OSTCOUNT $DIR/$tdir || error "setstripe $tdir failed"
	for i in $(seq 10); do
		local f=$DIR/$tdir/$tfile.$i
		touch $f || error "touch failed"
		local count=$($LFS getstripe -c $f)
		[ $count -eq $OSTCOUNT ] ||
			error "$f stripe count $count != $OSTCOUNT"
		local offset=$($LFS getstripe -i $f)
		[ $offset -eq 1 ] || error "$f stripe offset $offset != 1"
		local size=$($LFS getstripe -S $f)
		[ $size -eq $((def_stripe_size * 2)) ] ||
			error "$f stripe size $size != $((def_stripe_size * 2))"
		local pool=$($LFS getstripe -p $f)
		[ $pool == $test_pool ] || error "$f pool $pool != $test_pool"
	done

	# change fs default striping, delete parent default striping, now child
	# will stripe from new fs default striping only
	$LFS setstripe -c 1 -S $def_stripe_size -i 0 $MOUNT ||
		error "change $MOUNT default stripe failed"
	$LFS setstripe -c 0 $DIR/$tdir ||
		error "delete $tdir default stripe failed"
	for i in $(seq 11 20); do
		local f=$DIR/$tdir/$tfile.$i
		touch $f || error "touch $f failed"
		local count=$($LFS getstripe -c $f)
		[ $count -eq 1 ] || error "$f stripe count $count != 1"
		local offset=$($LFS getstripe -i $f)
		[ $offset -eq 0 ] || error "$f stripe offset $offset != 0"
		local size=$($LFS getstripe -S $f)
		[ $size -eq $def_stripe_size ] ||
			error "$f stripe size $size != $def_stripe_size"
		local pool=$($LFS getstripe -p $f)
		[ $pool == $test_pool ] || error "$f pool $pool isn't set"
	done

	unlinkmany $DIR/$tdir/$tfile. 1 20

	local f=$DIR/$tdir/$tfile
	pool_remove_all_targets $test_pool $f
	pool_remove $test_pool $f
}
run_test 406 "DNE support fs default striping"

test_407() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	[[ $MDS1_VERSION -lt $(version_code 2.8.55) ]] &&
		skip "Need MDS version at least 2.8.55"
	remote_mds_nodsh && skip "remote MDS with nodsh"

	$LFS mkdir -i 0 -c 1 $DIR/$tdir.0 ||
		error "$LFS mkdir -i 0 -c 1 $tdir.0 failed"
	$LFS mkdir -i 1 -c 1 $DIR/$tdir.1 ||
		error "$LFS mkdir -i 1 -c 1 $tdir.1 failed"
	touch $DIR/$tdir.0/$tfile.0 || error "touch $tdir.0/$tfile.0 failed"

	#define OBD_FAIL_DT_TXN_STOP	0x2019
	for idx in $(seq $MDSCOUNT); do
		do_facet mds$idx "lctl set_param fail_loc=0x2019"
	done
	$LFS mkdir -c 2 $DIR/$tdir && error "$LFS mkdir -c 2 $tdir should fail"
	mv $DIR/$tdir.0/$tfile.0 $DIR/$tdir.1/$tfile.1 &&
		error "mv $tdir.0/$tfile.0 $tdir.1/$tfile.1 should fail"
	true
}
run_test 407 "transaction fail should cause operation fail"

test_408() {
	dd if=/dev/zero of=$DIR/$tfile bs=$PAGE_SIZE count=1 oflag=direct

	#define OBD_FAIL_OSC_BRW_PREP_REQ2        0x40a
	lctl set_param fail_loc=0x8000040a
	# let ll_prepare_partial_page() fail
	dd if=/dev/zero of=$DIR/$tfile bs=2048 count=1 conv=notrunc || true

	rm -f $DIR/$tfile

	# create at least 100 unused inodes so that
	# shrink_icache_memory(0) should not return 0
	touch $DIR/$tfile-{0..100}
	rm -f $DIR/$tfile-{0..100}
	sync

	echo 2 > /proc/sys/vm/drop_caches
}
run_test 408 "drop_caches should not hang due to page leaks"

test_409()
{
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	check_mount_and_prep

	mkdir -p $DIR/$tdir || error "(0) Fail to mkdir"
	$LFS mkdir -i 1 -c 2 $DIR/$tdir/foo || error "(1) Fail to mkdir"
	touch $DIR/$tdir/guard || error "(2) Fail to create"

	local PREFIX=$(str_repeat 'A' 128)
	echo "Create 1K hard links start at $(date)"
	createmany -l $DIR/$tdir/guard $DIR/$tdir/foo/${PREFIX}_ 1000 ||
		error "(3) Fail to hard link"

	echo "Links count should be right although linkEA overflow"
	stat $DIR/$tdir/guard || error "(4) Fail to stat"
	local linkcount=$(stat --format=%h $DIR/$tdir/guard)
	[ $linkcount -eq 1001 ] ||
		error "(5) Unexpected hard links count: $linkcount"

	echo "List all links start at $(date)"
	ls -l $DIR/$tdir/foo > /dev/null ||
		error "(6) Fail to list $DIR/$tdir/foo"

	echo "Unlink hard links start at $(date)"
	unlinkmany $DIR/$tdir/foo/${PREFIX}_ 1000 ||
		error "(7) Fail to unlink"
}
run_test 409 "Large amount of cross-MDTs hard links on the same file"

test_410()
{
	[[ $CLIENT_VERSION -lt $(version_code 2.9.59) ]] &&
		skip "Need client version at least 2.9.59"

	# Create a file, and stat it from the kernel
	local testfile=$DIR/$tfile
	touch $testfile

	local run_id=$RANDOM
	local my_ino=$(stat --format "%i" $testfile)

	# Try to insert the module. This will always fail as the
	# module is designed to not be inserted.
	insmod $LUSTRE/tests/kernel/kinode.ko run_id=$run_id fname=$testfile \
	    &> /dev/null

	# Anything but success is a test failure
	dmesg | grep -q \
	    "lustre_kinode_$run_id: inode numbers are identical: $my_ino" ||
	    error "no inode match"
}
run_test 410 "Test inode number returned from kernel thread"

cleanup_test411_cgroup() {
	trap 0
	rmdir "$1"
}

test_411() {
	local cg_basedir=/sys/fs/cgroup/memory
	# LU-9966
	test -f "$cg_basedir/memory.kmem.limit_in_bytes" ||
		skip "no setup for cgroup"

	dd if=/dev/zero of=$DIR/$tfile bs=1M count=100 conv=fsync ||
		error "test file creation failed"
	cancel_lru_locks osc

	# Create a very small memory cgroup to force a slab allocation error
	local cgdir=$cg_basedir/osc_slab_alloc
	mkdir $cgdir || error "cgroup mkdir '$cgdir' failed"
	trap "cleanup_test411_cgroup $cgdir" EXIT
	echo 2M > $cgdir/memory.kmem.limit_in_bytes
	echo 1M > $cgdir/memory.limit_in_bytes

	# Should not LBUG, just be killed by oom-killer
	# dd will return 0 even allocation failure in some environment.
	# So don't check return value
	sh -c "echo \$$ > $cgdir/tasks && dd if=$DIR/$tfile of=/dev/null"
	cleanup_test411_cgroup $cgdir

	return 0
}
run_test 411 "Slab allocation error with cgroup does not LBUG"

test_412() {
	[ $MDSCOUNT -lt 2 ] && skip_env "needs >= 2 MDTs"
	if [ $(lustre_version_code mds1) -lt $(version_code 2.10.55) ]; then
		skip "Need server version at least 2.10.55"
	fi

	$LFS mkdir -i $((MDSCOUNT - 1)),$((MDSCOUNT - 2)) $DIR/$tdir ||
		error "mkdir failed"
	$LFS getdirstripe $DIR/$tdir
	local stripe_index=$($LFS getdirstripe -i $DIR/$tdir)
	[ $stripe_index -eq $((MDSCOUNT - 1)) ] ||
		error "expect $((MDSCOUT - 1)) get $stripe_index"
	local stripe_count=$($LFS getdirstripe -T $DIR/$tdir)
	[ $stripe_count -eq 2 ] ||
		error "expect 2 get $stripe_count"
}
run_test 412 "mkdir on specific MDTs"

test_413() {
	[ $MDSCOUNT -lt 2 ] &&
		skip "We need at least 2 MDTs for this test"

	if [ $(lustre_version_code mds1) -lt $(version_code 2.10.55) ]; then
		skip "Need server version at least 2.10.55"
	fi

	mkdir $DIR/$tdir || error "mkdir failed"

	# find MDT that is the most full
	local max=$($LFS df | grep MDT |
		awk 'BEGIN { a=0 }
			{ sub("%", "", $5)
			  if (0+$5 >= a)
			  {
				a = $5
				b = $6
			  }
			}
		     END { split(b, c, ":")
			   sub("]", "", c[2])
			   print c[2]
			 }')

	for i in $(seq $((MDSCOUNT - 1))); do
		$LFS mkdir -c $i $DIR/$tdir/d$i ||
			error "mkdir d$i failed"
		$LFS getdirstripe $DIR/$tdir/d$i
		local stripe_index=$($LFS getdirstripe -i $DIR/$tdir/d$i)
		[ $stripe_index -ne $max ] ||
			error "don't expect $max"
	done
}
run_test 413 "mkdir on less full MDTs"

test_414() {
#define OBD_FAIL_PTLRPC_BULK_ATTACH      0x521
	$LCTL set_param fail_loc=0x80000521
	dd if=/dev/zero of=$DIR/$tfile bs=2M count=1 oflag=sync
	rm -f $DIR/$tfile
}
run_test 414 "simulate ENOMEM in ptlrpc_register_bulk()"

test_415() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	[ $(lustre_version_code mds1) -lt $(version_code 2.11.52) ] &&
		skip "Need server version at least 2.11.52"

	# LU-11102
	local total
	local setattr_pid
	local start_time
	local end_time
	local duration

	total=500
	# this test may be slow on ZFS
	[ "$mds1_FSTYPE" == "zfs" ] && total=100

	# though this test is designed for striped directory, let's test normal
	# directory too since lock is always saved as CoS lock.
	test_mkdir $DIR/$tdir || error "mkdir $tdir"
	createmany -o $DIR/$tdir/$tfile. $total || error "createmany"

	(
		while true; do
			touch $DIR/$tdir
		done
	) &
	setattr_pid=$!

	start_time=$(date +%s)
	for i in $(seq $total); do
		mrename $DIR/$tdir/$tfile.$i $DIR/$tdir/$tfile-new.$i \
			> /dev/null
	done
	end_time=$(date +%s)
	duration=$((end_time - start_time))

	kill -9 $setattr_pid

	echo "rename $total files took $duration sec"
	[ $duration -lt 100 ] || error "rename took $duration sec"
}
run_test 415 "lock revoke is not missing"

test_416() {
	[ $(lustre_version_code mds1) -lt $(version_code 2.11.55) ] &&
		skip "Need server version at least 2.11.55"

	# define OBD_FAIL_OSD_TXN_START    0x19a
	do_facet mds1 lctl set_param fail_loc=0x19a

	lfs mkdir -c $MDSCOUNT $DIR/$tdir

	true
}
run_test 416 "transaction start failure won't cause system hung"

cleanup_417() {
	trap 0
	do_nodes $(comma_list $(mdts_nodes)) \
		"$LCTL set_param -n mdt.*MDT*.enable_dir_migration=1"
	do_nodes $(comma_list $(mdts_nodes)) \
		"$LCTL set_param -n mdt.*MDT*.enable_remote_dir=1"
	do_nodes $(comma_list $(mdts_nodes)) \
		"$LCTL set_param -n mdt.*MDT*.enable_striped_dir=1"
}

test_417() {
	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	[[ $MDS1_VERSION -lt $(version_code 2.11.56) ]] &&
		skip "Need MDS version at least 2.11.56"

	trap cleanup_417 RETURN EXIT

	$LFS mkdir -i 1 $DIR/$tdir.1 || error "create remote dir $tdir.1 failed"
	do_nodes $(comma_list $(mdts_nodes)) \
		"$LCTL set_param -n mdt.*MDT*.enable_dir_migration=0"
	$LFS migrate -m 0 $DIR/$tdir.1 &&
		error "migrate dir $tdir.1 should fail"

	do_nodes $(comma_list $(mdts_nodes)) \
		"$LCTL set_param -n mdt.*MDT*.enable_remote_dir=0"
	$LFS mkdir -i 1 $DIR/$tdir.2 &&
		error "create remote dir $tdir.2 should fail"

	do_nodes $(comma_list $(mdts_nodes)) \
		"$LCTL set_param -n mdt.*MDT*.enable_striped_dir=0"
	$LFS mkdir -c 2 $DIR/$tdir.3 &&
		error "create striped dir $tdir.3 should fail"
	true
}
run_test 417 "disable remote dir, striped dir and dir migration"

# Checks that the outputs of df [-i] and lfs df [-i] match
#
# usage: check_lfs_df <blocks | inodes> <mountpoint>
check_lfs_df() {
	local dir=$2
	local inodes
	local df_out
	local lfs_df_out
	local count
	local passed=false

	# blocks or inodes
	[ "$1" == "blocks" ] && inodes= || inodes="-i"

	for count in {1..100}; do
		cancel_lru_locks
		sync; sleep 2

		# read the lines of interest
		df_out=($(df -P $inodes $dir | tail -n +2)) ||
			error "df $inodes $dir | tail -n +2 failed"
		lfs_df_out=($($LFS df $inodes $dir | grep summary:)) ||
			error "lfs df $inodes $dir | grep summary: failed"

		# skip first substrings of each output as they are different
		# "<NID>:/<fsname>" for df, "filesystem_summary:" for lfs df
		# compare the remaining fields of the two outputs
		passed=true
		for i in {1..5}; do
			[ "${df_out[i]}" != "${lfs_df_out[i]}" ] && passed=false
		done
		$passed && break
	done

	if ! $passed; then
		df -P $inodes $dir
		echo
		lfs df $inodes $dir
		error "df and lfs df $1 output mismatch: "	\
		      "df ${inodes}: ${df_out[*]}, "		\
		      "lfs df ${inodes}: ${lfs_df_out[*]}"
	fi
}

test_418() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"

	local dir=$DIR/$tdir
	local numfiles=$((RANDOM % 4096 + 2))
	local numblocks=$((RANDOM % 256 + 1))

	wait_delete_completed
	test_mkdir $dir

	# check block output
	check_lfs_df blocks $dir
	# check inode output
	check_lfs_df inodes $dir

	# create a single file and retest
	echo "Creating a single file and testing"
	createmany -o $dir/$tfile- 1 &>/dev/null ||
		error "creating 1 file in $dir failed"
	check_lfs_df blocks $dir
	check_lfs_df inodes $dir

	# create a random number of files
	echo "Creating $((numfiles - 1)) files and testing"
	createmany -o $dir/$tfile- 1 $((numfiles - 1)) &>/dev/null ||
		error "creating $((numfiles - 1)) files in $dir failed"

	# write a random number of blocks to the first test file
	echo "Writing $numblocks 4K blocks and testing"
	dd if=/dev/urandom of=$dir/${tfile}-0 bs=4K conv=fsync \
		count=$numblocks &>/dev/null ||
		error "dd to $dir/${tfile}-0 failed"

	# retest
	check_lfs_df blocks $dir
	check_lfs_df inodes $dir

	unlinkmany $dir/$tfile- $numfiles &>/dev/null ||
		error "unlinking $numfiles files in $dir failed"
}
run_test 418 "df and lfs df outputs match"

test_419()
{
	local dir=$DIR/$tdir

	mkdir -p $dir
	touch $dir/file

	cancel_lru_locks mdc

	#OBD_FAIL_LLITE_OPEN_BY_NAME	0x1410
	$LCTL set_param fail_loc=0x1410
	cat $dir/file
	$LCTL set_param fail_loc=0
	rm -rf $dir
}
run_test 419 "Verify open file by name doesn't crash kernel"

test_420()
{
	[[ $MDS1_VERSION -ge $(version_code 2.12.1) ]] ||
		skip "Need MDS version at least 2.12.1"

	local SAVE_UMASK=$(umask)
	local dir=$DIR/$tdir
	local uname=$(getent passwd $RUNAS_ID | cut -d: -f1)

	mkdir -p $dir
	umask 0000
	mkdir -m03777 $dir/testdir
	ls -dn $dir/testdir
	# Need to remove trailing '.' when SELinux is enabled
	local dirperms=$(ls -dn $dir/testdir |
			 awk '{ sub(/\.$/, "", $1); print $1}')
	[ $dirperms == "drwxrwsrwt" ] ||
		error "incorrect perms on $dir/testdir"

	su - $uname -c "PATH=$LUSTRE/tests:\$PATH; \
		openfile -f O_RDONLY:O_CREAT -m 02755 $dir/testdir/testfile"
	ls -n $dir/testdir/testfile
	local fileperms=$(ls -n $dir/testdir/testfile |
			  awk '{ sub(/\.$/, "", $1); print $1}')
	[ $fileperms == "-rwxr-xr-x" ] ||
		error "incorrect perms on $dir/testdir/testfile"

	umask $SAVE_UMASK
}
run_test 420 "clear SGID bit on non-directories for non-members"

test_421a() {
	local cnt
	local fid1
	local fid2

	[ $MDS1_VERSION -lt $(version_code 2.12.2) ] &&
		skip "Need MDS version at least 2.12.2"

	test_mkdir $DIR/$tdir
	createmany -o $DIR/$tdir/f 3
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt != 3 ] && error "unexpected #files: $cnt"

	fid1=$(lfs path2fid $DIR/$tdir/f1)
	fid2=$(lfs path2fid $DIR/$tdir/f2)
	$LFS rmfid $DIR $fid1 $fid2 || error "rmfid failed"

	stat $DIR/$tdir/f1 && error "f1 still visible on the client"
	stat $DIR/$tdir/f2 && error "f2 still visible on the client"

	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt == 1 ] || error "unexpected #files after: $cnt"

	rm -f $DIR/$tdir/f3 || error "can't remove f3"
	createmany -o $DIR/$tdir/f 3
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt != 3 ] && error "unexpected #files: $cnt"

	fid1=$(lfs path2fid $DIR/$tdir/f1)
	fid2=$(lfs path2fid $DIR/$tdir/f2)
	echo "remove using fsname $FSNAME"
	$LFS rmfid $FSNAME $fid1 $fid2 || error "rmfid with fsname failed"

	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt == 1 ] || error "unexpected #files after: $cnt"
}
run_test 421a "simple rm by fid"

test_421b() {
	local cnt
	local FID1
	local FID2

	[ $MDS1_VERSION -lt $(version_code 2.12.2) ] &&
		skip "Need MDS version at least 2.12.2"

	test_mkdir $DIR/$tdir
	createmany -o $DIR/$tdir/f 3
	multiop_bg_pause $DIR/$tdir/f1 o_c || error "multiop failed to start"
	MULTIPID=$!

	FID1=$(lfs path2fid $DIR/$tdir/f1)
	FID2=$(lfs path2fid $DIR/$tdir/f2)
	$LFS rmfid $DIR $FID1 $FID2 && error "rmfid didn't fail"

	kill -USR1 $MULTIPID
	wait

	cnt=$(ls $DIR/$tdir | wc -l)
	[ $cnt == 2 ] || error "unexpected #files after: $cnt"
}
run_test 421b "rm by fid on open file"

test_421c() {
	local cnt
	local FIDS

	[ $MDS1_VERSION -lt $(version_code 2.12.2) ] &&
		skip "Need MDS version at least 2.12.2"

	test_mkdir $DIR/$tdir
	createmany -o $DIR/$tdir/f 3
	touch $DIR/$tdir/$tfile
	createmany -l$DIR/$tdir/$tfile $DIR/$tdir/h 180
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt != 184 ] && error "unexpected #files: $cnt"

	FID1=$(lfs path2fid $DIR/$tdir/$tfile)
	$LFS rmfid $DIR $FID1 || error "rmfid failed"

	cnt=$(ls $DIR/$tdir | wc -l)
	[ $cnt == 3 ] || error "unexpected #files after: $cnt"
}
run_test 421c "rm by fid against hardlinked files"

test_421d() {
	local cnt
	local FIDS

	[ $MDS1_VERSION -lt $(version_code 2.12.2) ] &&
		skip "Need MDS version at least 2.12.2"

	test_mkdir $DIR/$tdir
	createmany -o $DIR/$tdir/f 4097
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt != 4097 ] && error "unexpected #files: $cnt"

	FIDS=$(lfs path2fid $DIR/$tdir/f* | sed "s/[/][^:]*://g")
	$LFS rmfid $DIR $FIDS || error "rmfid failed"

	cnt=$(ls $DIR/$tdir | wc -l)
	rm -rf $DIR/$tdir
	[ $cnt == 0 ] || error "unexpected #files after: $cnt"
}
run_test 421d "rmfid en masse"

test_421e() {
	local cnt
	local FID

	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.12.2) ] &&
		skip "Need MDS version at least 2.12.2"

	mkdir -p $DIR/$tdir
	$LFS setdirstripe -c$MDSCOUNT $DIR/$tdir/striped_dir
	createmany -o $DIR/$tdir/striped_dir/f 512
	cnt=$(ls -1 $DIR/$tdir/striped_dir | wc -l)
	[ $cnt != 512 ] && error "unexpected #files: $cnt"

	FIDS=$(lfs path2fid $DIR/$tdir/striped_dir/f* |
		sed "s/[/][^:]*://g")
	$LFS rmfid $DIR $FIDS || error "rmfid failed"

	cnt=$(ls $DIR/$tdir/striped_dir | wc -l)
	rm -rf $DIR/$tdir
	[ $cnt == 0 ] || error "unexpected #files after: $cnt"
}
run_test 421e "rmfid in DNE"

test_421f() {
	local cnt
	local FID

	[ $MDS1_VERSION -lt $(version_code 2.12.2) ] &&
		skip "Need MDS version at least 2.12.2"

	test_mkdir $DIR/$tdir
	touch $DIR/$tdir/f
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt != 1 ] && error "unexpected #files: $cnt"

	FID=$(lfs path2fid $DIR/$tdir/f)
	$RUNAS $LFS rmfid $DIR $FID && error "rmfid didn't fail (1)"
	# rmfid should fail
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt != 1 ] && error "unexpected #files after (2): $cnt"

	chmod a+rw $DIR/$tdir
	ls -la $DIR/$tdir
	$RUNAS $LFS rmfid $DIR $FID && error "rmfid didn't fail (2)"
	# rmfid should fail
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt != 1 ] && error "unexpected #files after (3): $cnt"

	rm -f $DIR/$tdir/f
	$RUNAS touch $DIR/$tdir/f
	FID=$(lfs path2fid $DIR/$tdir/f)
	echo "rmfid as root"
	$LFS rmfid $DIR $FID || error "rmfid as root failed"
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt == 0 ] || error "unexpected #files after (4): $cnt"

	rm -f $DIR/$tdir/f
	$RUNAS touch $DIR/$tdir/f
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt != 1 ] && error "unexpected #files (4): $cnt"
	FID=$(lfs path2fid $DIR/$tdir/f)
	# rmfid w/o user_fid2path mount option should fail
	$RUNAS $LFS rmfid $DIR $FID && error "rmfid didn't fail(3)"
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt == 1 ] || error "unexpected #files after (5): $cnt"

	umount_client $MOUNT || "failed to umount client"
	mount_client $MOUNT "$MOUNT_OPTS,user_fid2path" ||
		"failed to mount client'"

	$RUNAS $LFS rmfid $DIR $FID || error "rmfid failed"
	# rmfid should succeed
	cnt=$(ls -1 $DIR/$tdir | wc -l)
	[ $cnt == 0 ] || error "unexpected #files after (6): $cnt"

	# rmfid shouldn't allow to remove files due to dir's permission
	chmod a+rwx $DIR/$tdir
	touch $DIR/$tdir/f
	ls -la $DIR/$tdir
	FID=$(lfs path2fid $DIR/$tdir/f)
	$RUNAS $LFS rmfid $DIR $FID && error "rmfid didn't fail"

	umount_client $MOUNT || "failed to umount client"
	mount_client $MOUNT "$MOUNT_OPTS" ||
		"failed to mount client'"

}
run_test 421f "rmfid checks permissions"

test_421g() {
	local cnt
	local FIDS

	[ $MDSCOUNT -lt 2 ] && skip "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.12.2) ] &&
		skip "Need MDS version at least 2.12.2"

	mkdir -p $DIR/$tdir
	$LFS setdirstripe -c$MDSCOUNT $DIR/$tdir/striped_dir
	createmany -o $DIR/$tdir/striped_dir/f 512
	cnt=$(ls -1 $DIR/$tdir/striped_dir | wc -l)
	[ $cnt != 512 ] && error "unexpected #files: $cnt"

	FIDS=$(lfs path2fid $DIR/$tdir/striped_dir/f* |
		sed "s/[/][^:]*://g")

	rm -f $DIR/$tdir/striped_dir/f1*
	cnt=$(ls -1 $DIR/$tdir/striped_dir | wc -l)
	removed=$((512 - cnt))

	# few files have been just removed, so we expect
	# rmfid to fail on their fids
	errors=$($LFS rmfid $DIR $FIDS 2>&1 | wc -l)
	[ $removed != $errors ] && error "$errors != $removed"

	cnt=$(ls $DIR/$tdir/striped_dir | wc -l)
	rm -rf $DIR/$tdir
	[ $cnt == 0 ] || error "unexpected #files after: $cnt"
}
run_test 421g "rmfid to return errors properly"

stat_test() {
    df -h $MOUNT &
    df -h $MOUNT &
    df -h $MOUNT &
    df -h $MOUNT &
    df -h $MOUNT &
    df -h $MOUNT &
}

test_423() {
    local _stats
    # ensure statfs cache is expired
    sleep 2;

    _stats=$(stat_test | grep $MOUNT | sort -u | wc -l)
    [[ ${_stats} -ne 1 ]] && error "statfs wrong"

    return 0
}
run_test 423 "statfs should return a right data"

prep_801() {
	[[ $(lustre_version_code mds1) -lt $(version_code 2.9.55) ]] ||
	[[ $OST1_VERSION -lt $(version_code 2.9.55) ]] &&
		skip "Need server version at least 2.9.55"

	start_full_debug_logging
}

post_801() {
	stop_full_debug_logging
}

barrier_stat() {
	if [ $MGS_VERSION -le $(version_code 2.10.0) ]; then
		local st=$(do_facet mgs $LCTL barrier_stat $FSNAME |
			   awk '/The barrier for/ { print $7 }')
		echo $st
	else
		local st=$(do_facet mgs $LCTL barrier_stat -s $FSNAME)
		echo \'$st\'
	fi
}

barrier_expired() {
	local expired

	if [ $MGS_VERSION -le $(version_code 2.10.0) ]; then
		expired=$(do_facet mgs $LCTL barrier_stat $FSNAME |
			  awk '/will be expired/ { print $7 }')
	else
		expired=$(do_facet mgs $LCTL barrier_stat -t $FSNAME)
	fi

	echo $expired
}

test_801a() {
	prep_801

	echo "Start barrier_freeze at: $(date)"
	#define OBD_FAIL_BARRIER_DELAY		0x2202
	do_facet mgs $LCTL set_param fail_val=5 fail_loc=0x2202
	# Do not reduce barrier time - See LU-11873
	do_facet mgs $LCTL barrier_freeze $FSNAME 20 &

	sleep 2
	local b_status=$(barrier_stat)
	echo "Got barrier status at: $(date)"
	[ "$b_status" = "'freezing_p1'" ] ||
		error "(1) unexpected barrier status $b_status"

	do_facet mgs $LCTL set_param fail_val=0 fail_loc=0
	wait
	b_status=$(barrier_stat)
	[ "$b_status" = "'frozen'" ] ||
		error "(2) unexpected barrier status $b_status"

	local expired=$(barrier_expired)
	echo "sleep $((expired + 3)) seconds, then the barrier will be expired"
	sleep $((expired + 3))

	b_status=$(barrier_stat)
	[ "$b_status" = "'expired'" ] ||
		error "(3) unexpected barrier status $b_status"

	# Do not reduce barrier time - See LU-11873
	do_facet mgs $LCTL barrier_freeze $FSNAME 20 ||
		error "(4) fail to freeze barrier"

	b_status=$(barrier_stat)
	[ "$b_status" = "'frozen'" ] ||
		error "(5) unexpected barrier status $b_status"

	echo "Start barrier_thaw at: $(date)"
	#define OBD_FAIL_BARRIER_DELAY		0x2202
	do_facet mgs $LCTL set_param fail_val=5 fail_loc=0x2202
	do_facet mgs $LCTL barrier_thaw $FSNAME &

	sleep 2
	b_status=$(barrier_stat)
	echo "Got barrier status at: $(date)"
	[ "$b_status" = "'thawing'" ] ||
		error "(6) unexpected barrier status $b_status"

	do_facet mgs $LCTL set_param fail_val=0 fail_loc=0
	wait
	b_status=$(barrier_stat)
	[ "$b_status" = "'thawed'" ] ||
		error "(7) unexpected barrier status $b_status"

	#define OBD_FAIL_BARRIER_FAILURE	0x2203
	do_facet $SINGLEMDS $LCTL set_param fail_loc=0x2203
	do_facet mgs $LCTL barrier_freeze $FSNAME

	b_status=$(barrier_stat)
	[ "$b_status" = "'failed'" ] ||
		error "(8) unexpected barrier status $b_status"

	do_facet $SINGLEMDS $LCTL set_param fail_loc=0
	do_facet mgs $LCTL barrier_thaw $FSNAME

	post_801
}
run_test 801a "write barrier user interfaces and stat machine"

test_801b() {
	prep_801

	mkdir $DIR/$tdir || error "(1) fail to mkdir"
	createmany -d $DIR/$tdir/d 6 || "(2) fail to mkdir"
	touch $DIR/$tdir/d2/f10 || error "(3) fail to touch"
	touch $DIR/$tdir/d3/f11 || error "(4) fail to touch"
	touch $DIR/$tdir/d4/f12 || error "(5) fail to touch"

	cancel_lru_locks mdc

	# 180 seconds should be long enough
	do_facet mgs $LCTL barrier_freeze $FSNAME 180

	local b_status=$(barrier_stat)
	[ "$b_status" = "'frozen'" ] ||
		error "(6) unexpected barrier status $b_status"

	mkdir $DIR/$tdir/d0/d10 &
	mkdir_pid=$!

	touch $DIR/$tdir/d1/f13 &
	touch_pid=$!

	ln $DIR/$tdir/d2/f10 $DIR/$tdir/d2/f14 &
	ln_pid=$!

	mv $DIR/$tdir/d3/f11 $DIR/$tdir/d3/f15 &
	mv_pid=$!

	rm -f $DIR/$tdir/d4/f12 &
	rm_pid=$!

	stat $DIR/$tdir/d5 || error "(7) stat should succeed"

	# To guarantee taht the 'stat' is not blocked
	b_status=$(barrier_stat)
	[ "$b_status" = "'frozen'" ] ||
		error "(8) unexpected barrier status $b_status"

	# let above commands to run at background
	sleep 5

	ps -p $mkdir_pid || error "(9) mkdir should be blocked"
	ps -p $touch_pid || error "(10) touch should be blocked"
	ps -p $ln_pid || error "(11) link should be blocked"
	ps -p $mv_pid || error "(12) rename should be blocked"
	ps -p $rm_pid || error "(13) unlink should be blocked"

	b_status=$(barrier_stat)
	[ "$b_status" = "'frozen'" ] ||
		error "(14) unexpected barrier status $b_status"

	do_facet mgs $LCTL barrier_thaw $FSNAME
	b_status=$(barrier_stat)
	[ "$b_status" = "'thawed'" ] ||
		error "(15) unexpected barrier status $b_status"

	wait $mkdir_pid || error "(16) mkdir should succeed"
	wait $touch_pid || error "(17) touch should succeed"
	wait $ln_pid || error "(18) link should succeed"
	wait $mv_pid || error "(19) rename should succeed"
	wait $rm_pid || error "(20) unlink should succeed"

	post_801
}
run_test 801b "modification will be blocked by write barrier"

test_801c() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"

	prep_801

	stop mds2 || error "(1) Fail to stop mds2"

	do_facet mgs $LCTL barrier_freeze $FSNAME 30

	local b_status=$(barrier_stat)
	[ "$b_status" = "'expired'" -o "$b_status" = "'failed'" ] || {
		do_facet mgs $LCTL barrier_thaw $FSNAME
		error "(2) unexpected barrier status $b_status"
	}

	do_facet mgs $LCTL barrier_rescan $FSNAME ||
		error "(3) Fail to rescan barrier bitmap"

	# Do not reduce barrier time - See LU-11873
	do_facet mgs $LCTL barrier_freeze $FSNAME 20

	b_status=$(barrier_stat)
	[ "$b_status" = "'frozen'" ] ||
		error "(4) unexpected barrier status $b_status"

	do_facet mgs $LCTL barrier_thaw $FSNAME
	b_status=$(barrier_stat)
	[ "$b_status" = "'thawed'" ] ||
		error "(5) unexpected barrier status $b_status"

	local devname=$(mdsdevname 2)

	start mds2 $devname $MDS_MOUNT_OPTS || error "(6) Fail to start mds2"

	do_facet mgs $LCTL barrier_rescan $FSNAME ||
		error "(7) Fail to rescan barrier bitmap"

	post_801
}
run_test 801c "rescan barrier bitmap"

saved_MGS_MOUNT_OPTS=$MGS_MOUNT_OPTS
saved_MDS_MOUNT_OPTS=$MDS_MOUNT_OPTS
saved_OST_MOUNT_OPTS=$OST_MOUNT_OPTS
saved_MOUNT_OPTS=$MOUNT_OPTS

cleanup_802() {
	trap 0

	stopall
	MGS_MOUNT_OPTS=$saved_MGS_MOUNT_OPTS
	MDS_MOUNT_OPTS=$saved_MDS_MOUNT_OPTS
	OST_MOUNT_OPTS=$saved_OST_MOUNT_OPTS
	MOUNT_OPTS=$saved_MOUNT_OPTS
	setupall
}

test_802() {
	[[ $mds1_FSTYPE = zfs ]] || skip "ZFS specific test"
	[[ $(lustre_version_code mds1) -lt $(version_code 2.9.55) ]] ||
	[[ $OST1_VERSION -lt $(version_code 2.9.55) ]] &&
		skip "Need server version at least 2.9.55"

	[[ $ENABLE_QUOTA ]] && skip "Quota enabled for read-only test"

	mkdir $DIR/$tdir || error "(1) fail to mkdir"

	cp $LUSTRE/tests/test-framework.sh $DIR/$tdir/ ||
		error "(2) Fail to copy"

	trap cleanup_802 EXIT

	# sync by force before remount as readonly
	sync; sync_all_data; sleep 3; sync_all_data

	stopall

	MGS_MOUNT_OPTS=$(csa_add "$MGS_MOUNT_OPTS" -o rdonly_dev)
	MDS_MOUNT_OPTS=$(csa_add "$MDS_MOUNT_OPTS" -o rdonly_dev)
	OST_MOUNT_OPTS=$(csa_add "$OST_MOUNT_OPTS" -o rdonly_dev)

	echo "Mount the server as read only"
	setupall server_only || error "(3) Fail to start servers"

	echo "Mount client without ro should fail"
	mount_client $MOUNT &&
		error "(4) Mount client without 'ro' should fail"

	echo "Mount client with ro should succeed"
	MOUNT_OPTS=$(csa_add "$MOUNT_OPTS" -o ro)
	mount_client $MOUNT ||
		error "(5) Mount client with 'ro' should succeed"

	echo "Modify should be refused"
	touch $DIR/$tdir/guard && error "(6) Touch should fail under ro mode"

	echo "Read should be allowed"
	diff $LUSTRE/tests/test-framework.sh $DIR/$tdir/test-framework.sh ||
		error "(7) Read should succeed under ro mode"

	cleanup_802
}
run_test 802 "simulate readonly device"

test_803() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.10.54) ] &&
		skip "MDS needs to be newer than 2.10.54"

	mkdir -p $DIR/$tdir
	# Create some objects on all MDTs to trigger related logs objects
	for idx in $(seq $MDSCOUNT); do
		$LFS mkdir -c $MDSCOUNT -i $((idx % $MDSCOUNT)) \
			$DIR/$tdir/dir${idx} ||
			error "Fail to create $DIR/$tdir/dir${idx}"
	done

	sync; sleep 3
	wait_delete_completed # ensure old test cleanups are finished
	echo "before create:"
	$LFS df -i $MOUNT
	local before_used=$($LFS df -i | grep MDT0000_UUID | awk '{print $3}')

	for i in {1..10}; do
		$LFS mkdir -c 1 -i 1 $DIR/$tdir/foo$i ||
			error "Fail to create $DIR/$tdir/foo$i"
	done

	sync; sleep 3
	echo "after create:"
	$LFS df -i $MOUNT
	local after_used=$($LFS df -i | grep MDT0000_UUID | awk '{print $3}')

	# allow for an llog to be cleaned up during the test
	[ $after_used -ge $((before_used + 10 - 1)) ] ||
		error "before ($before_used) + 10 > after ($after_used)"

	for i in {1..10}; do
		rm -rf $DIR/$tdir/foo$i ||
			error "Fail to remove $DIR/$tdir/foo$i"
	done

	sleep 3 # avoid MDT return cached statfs
	wait_delete_completed
	echo "after unlink:"
	$LFS df -i $MOUNT
	after_used=$($LFS df -i | grep MDT0000_UUID | awk '{print $3}')

	# allow for an llog to be created during the test
	[ $after_used -le $((before_used + 1)) ] ||
		error "after ($after_used) > before ($before_used) + 1"
}
run_test 803 "verify agent object for remote object"

test_804() {
	[[ $MDSCOUNT -lt 2 ]] && skip_env "needs >= 2 MDTs"
	[ $MDS1_VERSION -lt $(version_code 2.10.54) ] &&
		skip "MDS needs to be newer than 2.10.54"
	[ "$mds1_FSTYPE" != "ldiskfs" ] && skip_env "ldiskfs only test"

	mkdir -p $DIR/$tdir
	$LFS mkdir -c 1 -i 1 $DIR/$tdir/dir0 ||
		error "Fail to create $DIR/$tdir/dir0"

	local fid=$($LFS path2fid $DIR/$tdir/dir0)
	local dev=$(mdsdevname 2)

	do_facet mds2 "$DEBUGFS -c -R 'ls /REMOTE_PARENT_DIR' $dev" |
		grep ${fid} || error "NOT found agent entry for dir0"

	$LFS mkdir -c $MDSCOUNT -i 0 $DIR/$tdir/dir1 ||
		error "Fail to create $DIR/$tdir/dir1"

	touch $DIR/$tdir/dir1/foo0 ||
		error "Fail to create $DIR/$tdir/dir1/foo0"
	fid=$($LFS path2fid $DIR/$tdir/dir1/foo0)
	local rc=0

	for idx in $(seq $MDSCOUNT); do
		dev=$(mdsdevname $idx)
		do_facet mds${idx} \
			"$DEBUGFS -c -R 'ls /REMOTE_PARENT_DIR' $dev" |
			grep ${fid} && rc=$idx
	done

	mv $DIR/$tdir/dir1/foo0 $DIR/$tdir/dir1/foo1 ||
		error "Fail to rename foo0 to foo1"
	if [ $rc -eq 0 ]; then
		for idx in $(seq $MDSCOUNT); do
			dev=$(mdsdevname $idx)
			do_facet mds${idx} \
			"$DEBUGFS -c -R 'ls /REMOTE_PARENT_DIR' $dev" |
			grep ${fid} && rc=$idx
		done
	fi

	mv $DIR/$tdir/dir1/foo1 $DIR/$tdir/dir1/foo2 ||
		error "Fail to rename foo1 to foo2"
	if [ $rc -eq 0 ]; then
		for idx in $(seq $MDSCOUNT); do
			dev=$(mdsdevname $idx)
			do_facet mds${idx} \
			"$DEBUGFS -c -R 'ls /REMOTE_PARENT_DIR' $dev" |
			grep ${fid} && rc=$idx
		done
	fi

	[ $rc -ne 0 ] || error "NOT found agent entry for foo"

	ln $DIR/$tdir/dir1/foo2 $DIR/$tdir/dir0/guard ||
		error "Fail to link to $DIR/$tdir/dir1/foo2"
	mv $DIR/$tdir/dir1/foo2 $DIR/$tdir/dir1/foo0 ||
		error "Fail to rename foo2 to foo0"
	unlink $DIR/$tdir/dir1/foo0 ||
		error "Fail to unlink $DIR/$tdir/dir1/foo0"
	rm -rf $DIR/$tdir/dir0 ||
		error "Fail to rm $DIR/$tdir/dir0"

	for idx in $(seq $MDSCOUNT); do
		dev=$(mdsdevname $idx)
		rc=0

		stop mds${idx}
		run_e2fsck $(facet_active_host mds$idx) $dev -n ||
			rc=$?
		start mds${idx} $dev $MDS_MOUNT_OPTS ||
			error "mount mds$idx failed"
		df $MOUNT > /dev/null 2>&1

		# e2fsck should not return error
		[ $rc -eq 0 ] ||
			error "e2fsck detected error on MDT${idx}: rc=$rc"
	done
}
run_test 804 "verify agent entry for remote entry"

cleanup_805() {
	do_facet $SINGLEMDS zfs set quota=$old $fsset
	unlinkmany $DIR/$tdir/f- 1000000
	trap 0
}

test_805() {
	local zfs_version=$(do_facet mds1 cat /sys/module/zfs/version)
	[ "$mds1_FSTYPE" != "zfs" ] && skip "ZFS specific test"
	[ $(version_code $zfs_version) -lt $(version_code 0.7.2) ] &&
		skip "netfree not implemented before 0.7"
	[[ $MDS1_VERSION -ge $(version_code 2.10.57) ]] ||
		skip "Need MDS version at least 2.10.57"

	local fsset
	local freekb
	local usedkb
	local old
	local quota
	local pref="osd-zfs.lustre-MDT0000."

	# limit available space on MDS dataset to meet nospace issue
	# quickly. then ZFS 0.7.2 can use reserved space if asked
	# properly (using netfree flag in osd_declare_destroy()
	fsset=$(do_facet $SINGLEMDS lctl get_param -n $pref.mntdev)
	old=$(do_facet $SINGLEMDS zfs get -H quota $fsset | \
		gawk '{print $3}')
	freekb=$(do_facet $SINGLEMDS lctl get_param -n $pref.kbytesfree)
	usedkb=$(do_facet $SINGLEMDS lctl get_param -n $pref.kbytestotal)
	let "usedkb=usedkb-freekb"
	let "freekb=freekb/2"
	if let "freekb > 5000"; then
		let "freekb=5000"
	fi
	do_facet $SINGLEMDS zfs set quota=$(((usedkb+freekb)*1024)) $fsset
	trap cleanup_805 EXIT
	mkdir $DIR/$tdir
	$LFS setstripe -E 1M -L mdt $DIR/$tdir || error "DoM not working"
	createmany -m $DIR/$tdir/f- 1000000 && error "ENOSPC wasn't met"
	rm -rf $DIR/$tdir || error "not able to remove"
	do_facet $SINGLEMDS zfs set quota=$old $fsset
	trap 0
}
run_test 805 "ZFS can remove from full fs"

# Size-on-MDS test
check_lsom_data()
{
	local file=$1
	local size=$($LFS getsom -s $file)
	local expect=$(stat -c %s $file)

	[[ $size == $expect ]] ||
		error "$file expected size: $expect, got: $size"

	local blocks=$($LFS getsom -b $file)
	expect=$(stat -c %b $file)
	[[ $blocks == $expect ]] ||
		error "$file expected blocks: $expect, got: $blocks"
}

check_lsom_size()
{
	local size=$($LFS getsom -s $1)
	local expect=$2

	[[ $size == $expect ]] ||
		error "$file expected size: $expect, got: $size"
}

test_806() {
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	local bs=1048576

	touch $DIR/$tfile || error "touch $tfile failed"

	local save="$TMP/$TESTSUITE-$TESTNAME.parameters"
	save_lustre_params client "llite.*.xattr_cache" > $save
	lctl set_param llite.*.xattr_cache=0
	stack_trap "restore_lustre_params < $save" EXIT

	# single-threaded write
	echo "Test SOM for single-threaded write"
	dd if=/dev/zero of=$DIR/$tfile bs=$bs count=1 ||
		error "write $tfile failed"
	check_lsom_size $DIR/$tfile $bs

	local num=32
	local size=$(($num * $bs))
	local offset=0
	local i

	echo "Test SOM for single client multi-threaded($num) write"
	$TRUNCATE $DIR/$tfile 0
	for ((i = 0; i < $num; i++)); do
		$MULTIOP $DIR/$tfile Oz${offset}w${bs}c &
		local pids[$i]=$!
		offset=$((offset + $bs))
	done
	for (( i=0; i < $num; i++ )); do
		wait ${pids[$i]}
	done
	check_lsom_size $DIR/$tfile $size

	$TRUNCATE $DIR/$tfile 0
	for ((i = 0; i < $num; i++)); do
		offset=$((offset - $bs))
		$MULTIOP $DIR/$tfile Oz${offset}w${bs}c &
		local pids[$i]=$!
	done
	for (( i=0; i < $num; i++ )); do
		wait ${pids[$i]}
	done
	check_lsom_size $DIR/$tfile $size

	# multi-client writes
	num=$(get_node_count ${CLIENTS//,/ })
	size=$(($num * $bs))
	offset=0
	i=0

	echo "Test SOM for multi-client ($num) writes"
	$TRUNCATE $DIR/$tfile 0
	for client in ${CLIENTS//,/ }; do
		do_node $client $MULTIOP $DIR/$tfile Oz${offset}w${bs}c &
		local pids[$i]=$!
		i=$((i + 1))
		offset=$((offset + $bs))
	done
	for (( i=0; i < $num; i++ )); do
		wait ${pids[$i]}
	done
	check_lsom_size $DIR/$tfile $offset

	i=0
	$TRUNCATE $DIR/$tfile 0
	for client in ${CLIENTS//,/ }; do
		offset=$((offset - $bs))
		do_node $client $MULTIOP $DIR/$tfile Oz${offset}w${bs}c &
		local pids[$i]=$!
		i=$((i + 1))
	done
	for (( i=0; i < $num; i++ )); do
		wait ${pids[$i]}
	done
	check_lsom_size $DIR/$tfile $size

	# verify truncate
	echo "Test SOM for truncate"
	$TRUNCATE $DIR/$tfile 1048576
	check_lsom_size $DIR/$tfile 1048576
	$TRUNCATE $DIR/$tfile 1234
	check_lsom_size $DIR/$tfile 1234

	# verify SOM blocks count
	echo "Verify SOM block count"
	$TRUNCATE $DIR/$tfile 0
	$MULTIOP $DIR/$tfile oO_TRUNC:O_RDWR:w1048576YSc ||
		error "failed to write file $tfile"
	check_lsom_data $DIR/$tfile
}
run_test 806 "Verify Lazy Size on MDS"

test_807() {
	[ $MDS1_VERSION -lt $(version_code 2.11.52) ] &&
		skip "Need MDS version at least 2.11.52"

	# Registration step
	changelog_register || error "changelog_register failed"
	local cl_user="${CL_USERS[$SINGLEMDS]%% *}"
	changelog_users $SINGLEMDS | grep -q $cl_user ||
		error "User $cl_user not found in changelog_users"

	local save="$TMP/$TESTSUITE-$TESTNAME.parameters"
	save_lustre_params client "llite.*.xattr_cache" > $save
	lctl set_param llite.*.xattr_cache=0
	stack_trap "restore_lustre_params < $save" EXIT

	rm -rf $DIR/$tdir || error "rm $tdir failed"
	mkdir -p $DIR/$tdir || error "mkdir $tdir failed"
	touch $DIR/$tdir/trunc || error "touch $tdir/trunc failed"
	$TRUNCATE $DIR/$tdir/trunc 1024 || error "truncate $tdir/trunc failed"
	$TRUNCATE $DIR/$tdir/trunc 1048576 ||
		error "truncate $tdir/trunc failed"

	local bs=1048576
	dd if=/dev/zero of=$DIR/$tdir/single_dd bs=$bs count=1 ||
		error "write $tfile failed"

	# multi-client wirtes
	local num=$(get_node_count ${CLIENTS//,/ })
	local offset=0
	local i=0

	echo "Test SOM for multi-client ($num) writes"
	touch $DIR/$tfile || error "touch $tfile failed"
	$TRUNCATE $DIR/$tfile 0
	for client in ${CLIENTS//,/ }; do
		do_node $client $MULTIOP $DIR/$tfile Oz${offset}w${bs}c &
		local pids[$i]=$!
		i=$((i + 1))
		offset=$((offset + $bs))
	done
	for (( i=0; i < $num; i++ )); do
		wait ${pids[$i]}
	done

	sleep 5
	$LSOM_SYNC -u $cl_user -m $FSNAME-MDT0000 $MOUNT
	check_lsom_data $DIR/$tdir/trunc
	check_lsom_data $DIR/$tdir/single_dd
	check_lsom_data $DIR/$tfile

	rm -rf $DIR/$tdir
	# Deregistration step
	changelog_deregister || error "changelog_deregister failed"
}
run_test 807 "verify LSOM syncing tool"

check_som_nologged()
{
	local lines=$($LFS changelog $FSNAME-MDT0000 |
		grep 'x=trusted.som' | wc -l)
	[ $lines -ne 0 ] && error "trusted.som xattr is logged in Changelogs"
}

test_808() {
	[ $MDS1_VERSION -lt $(version_code 2.11.55) ] &&
		skip "Need MDS version at least 2.11.55"

	# Registration step
	changelog_register || error "changelog_register failed"

	touch $DIR/$tfile || error "touch $tfile failed"
	check_som_nologged

	dd if=/dev/zero of=$DIR/$tfile bs=1048576 count=1 ||
		error "write $tfile failed"
	check_som_nologged

	$TRUNCATE $DIR/$tfile 1234
	check_som_nologged

	$TRUNCATE $DIR/$tfile 1048576
	check_som_nologged

	# Deregistration step
	changelog_deregister || error "changelog_deregister failed"
}
run_test 808 "Check trusted.som xattr not logged in Changelogs"

check_som_nodata()
{
	$LFS getsom $1
	[[ $? -eq 61 ]] || error "DoM-only file $1 has SOM xattr"
}

test_809() {
	[ $MDS1_VERSION -lt $(version_code 2.11.56) ] &&
		skip "Need MDS version at least 2.11.56"

	$LFS setstripe -E 1M -L mdt $DIR/$tfile ||
		error "failed to create DoM-only file $DIR/$tfile"
	touch $DIR/$tfile || error "touch $tfile failed"
	check_som_nodata $DIR/$tfile

	dd if=/dev/zero of=$DIR/$tfile bs=2048 count=1 ||
		error "write $tfile failed"
	check_som_nodata $DIR/$tfile

	$TRUNCATE $DIR/$tfile 1234
	check_som_nodata $DIR/$tfile

	$TRUNCATE $DIR/$tfile 4097
	check_som_nodata $DIR/$file
}
run_test 809 "Verify no SOM xattr store for DoM-only files"

test_810() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	$GSS && skip_env "could not run with gss"

	set_checksums 1
	stack_trap "set_checksums $ORIG_CSUM" EXIT
	stack_trap "set_checksum_type $ORIG_CSUM_TYPE" EXIT

	local csum
	local before
	local after
	for csum in $CKSUM_TYPES; do
		#define OBD_FAIL_OSC_NO_GRANT	0x411
		$LCTL set_param osc.*.checksum_type=$csum fail_loc=0x411
		for i in "10240 0" "10000 0" "4000 1" "500 1"; do
			eval set -- $i
			dd if=/dev/urandom of=$DIR/$tfile bs=$1 count=2 seek=$2
			before=$(md5sum $DIR/$tfile)
			$LCTL set_param ldlm.namespaces.*osc*.lru_size=clear
			after=$(md5sum $DIR/$tfile)
			[ "$before" == "$after" ] ||
				error "$csum: $before != $after bs=$1 seek=$2"
		done
	done
}
run_test 810 "partial page writes on ZFS (LU-11663)"

test_812() {
	[ $OST1_VERSION -lt $(version_code 2.12.51) ] &&
		skip "OST < 2.12.51 doesn't support this fail_loc"
	[ "$SHARED_KEY" = true ] &&
		skip "OSC connections never go IDLE with Shared-Keys enabled"

	$LFS setstripe -c 1 -i 0 $DIR/$tfile
	# ensure ost1 is connected
	stat $DIR/$tfile >/dev/null || error "can't stat"
	wait_osc_import_state client ost1 FULL
	# no locks, no reqs to let the connection idle
	cancel_lru_locks osc

	# delay OST_DISCONNECT on OST1 to put OSC into intermediate state
#define OBD_FAIL_OST_DISCONNECT_DELAY	 0x245
	do_facet ost1 "$LCTL set_param fail_loc=0x245 fail_val=8"
	wait_osc_import_state client ost1 CONNECTING
	do_facet ost1 "$LCTL set_param fail_loc=0 fail_val=0"

	stat $DIR/$tfile >/dev/null || error "can't stat file"
}
run_test 812 "do not drop reqs generated when imp is going to idle (LU-11951)"

test_814()
{
	dd of=$DIR/$tfile seek=128 bs=1k < /dev/null
	echo -n y >> $DIR/$tfile
	cp --sparse=always $DIR/$tfile $DIR/${tfile}.cp || error "copy failed"
	diff $DIR/$tfile $DIR/${tfile}.cp || error "files should be same"
}
run_test 814 "sparse cp works as expected (LU-12361)"

test_815()
{
	writeme -b 100 $DIR/$tfile || error "write 100 bytes failed"
	writeme -b 0 $DIR/$tfile || error "write 0 byte failed"
}
run_test 815 "zero byte tiny write doesn't hang (LU-12382)"

test_816() {
	[ "$SHARED_KEY" = true ] &&
		skip "OSC connections never go IDLE with Shared-Keys enabled"

	$LFS setstripe -c 1 -i 0 $DIR/$tfile
	# ensure ost1 is connected
	stat $DIR/$tfile >/dev/null || error "can't stat"
	wait_osc_import_state client ost1 FULL
	# no locks, no reqs to let the connection idle
	cancel_lru_locks osc
	lru_resize_disable osc
	local before
	local now
	before=$($LCTL get_param -n \
		 ldlm.namespaces.$FSNAME-OST0000-osc-[^M]*.lru_size)

	wait_osc_import_state client ost1 IDLE
	dd if=/dev/null of=$DIR/$tfile bs=1k count=1 conv=sync
	now=$($LCTL get_param -n \
	      ldlm.namespaces.$FSNAME-OST0000-osc-[^M]*.lru_size)
	[ $before == $now ] || error "lru_size changed $before != $now"
}
run_test 816 "do not reset lru_resize on idle reconnect"

cleanup_817() {
	umount $tmpdir
	exportfs -u localhost:$DIR/nfsexp
	rm -rf $DIR/nfsexp
}

test_817() {
	systemctl restart nfs-server.service || skip "failed to restart nfsd"

	mkdir -p $DIR/nfsexp
	exportfs -orw,no_root_squash localhost:$DIR/nfsexp ||
		error "failed to export nfs"

	tmpdir=$(mktemp -d /tmp/nfs-XXXXXX)
	stack_trap cleanup_817 EXIT

	mount -t nfs -orw localhost:$DIR/nfsexp $tmpdir ||
		error "failed to mount nfs to $tmpdir"

	cp /bin/true $tmpdir
	$DIR/nfsexp/true || error "failed to execute 'true' command"
}
run_test 817 "nfsd won't cache write lock for exec file"

test_819a() {
	dd if=/dev/zero of=$DIR/$tfile bs=1M count=1
	cancel_lru_locks osc
	#define OBD_FAIL_OST_2BIG_NIOBUF		0x248
	do_facet $SINGLEMDS lctl set_param fail_loc=0x80000248
	dd if=$DIR/$tfile of=/dev/null bs=1M count=1
	rm -f $TDIR/$tfile
}
run_test 819a "too big niobuf in read"

test_819b() {
	#define OBD_FAIL_OST_2BIG_NIOBUF		0x248
	do_facet $SINGLEMDS lctl set_param fail_loc=0x80000248
	dd if=/dev/zero of=$DIR/$tfile bs=1M count=1
	cancel_lru_locks osc
	sleep 1
	rm -f $TDIR/$tfile
}
run_test 819b "too big niobuf in write"

test_818() {
	mkdir $DIR/$tdir
	$LFS setstripe -c1 -i0 $DIR/$tfile
	$LFS setstripe -c1 -i1 $DIR/$tfile
	stop $SINGLEMDS
	#define OBD_FAIL_OSP_CANT_PROCESS_LLOG		0x2105
	do_facet $SINGLEMDS lctl set_param fail_loc=0x80002105
	start $SINGLEMDS $(mdsdevname ${SINGLEMDS//mds/}) $MDS_MOUNT_OPTS ||
		error "start $SINGLEMDS failed"
	rm -rf $DIR/$tdir
}
run_test 818 "unlink with failed llog"

#
# tests that do cleanup/setup should be run at the end
#

test_900() {
	[ $PARALLEL == "yes" ] && skip "skip parallel run"
	local ls

	#define OBD_FAIL_MGC_PAUSE_PROCESS_LOG   0x903
	$LCTL set_param fail_loc=0x903

	cancel_lru_locks MGC

	FAIL_ON_ERROR=true cleanup
	FAIL_ON_ERROR=true setup
}
run_test 900 "umount should not race with any mgc requeue thread"

# LU-13377
test_902() {
	[ $CLIENT_VERSION -lt $(version_code 2.13.52) ] &&
		skip "client does not have LU-13377 fix"
	#define OBD_FAIL_LLITE_SHORT_COMMIT 0x1415
	$LCTL set_param fail_loc=0x1415
	dd if=/dev/zero of=$DIR/$tfile bs=1M count=1
	cancel_lru_locks osc
	rm -f $DIR/$tfile
}
run_test 902 "test short write doesn't hang lustre"

complete $SECONDS
[ -f $EXT2_DEV ] && rm $EXT2_DEV || true
check_and_cleanup_lustre
if [ "$I_MOUNTED" != "yes" ]; then
	lctl set_param debug="$OLDDEBUG" 2> /dev/null || true
fi
exit_status
