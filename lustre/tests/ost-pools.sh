#!/bin/bash
# -*- mode: Bash; tab-width: 4; indent-tabs-mode: t; -*-
# vim:autoindent:shiftwidth=4:tabstop=4:
#
# Run select tests by setting ONLY, or as arguments to the script.
# Skip specific tests by setting EXCEPT.
#
# Run test by setting NOSETUP=true when ltest has setup env for us

SRCDIR=`dirname $0`
export PATH=$PWD/$SRCDIR:$SRCDIR:$PWD/$SRCDIR/../utils:$PATH:/sbin

ONLY=${ONLY:-"$*"}

# Orion
# 23 - quota is not working yet
ALWAYS_EXCEPT="23 $OST_POOLS_EXCEPT"
# bug number for skipped test: -
# UPDATE THE COMMENT ABOVE WITH BUG NUMBERS WHEN CHANGING ALWAYS_EXCEPT!

[ "$ALWAYS_EXCEPT$EXCEPT" ] && \
        echo "Skipping tests: `echo $ALWAYS_EXCEPT $EXCEPT`"

TMP=${TMP:-/tmp}
ORIG_PWD=${PWD}

LUSTRE=${LUSTRE:-$(cd $(dirname $0)/..; echo $PWD)}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}
init_logging

check_and_setup_lustre

DIR=${DIR:-$MOUNT}
assert_DIR

build_test_filter

FAIL_ON_ERROR=${FAIL_ON_ERROR:-true}

LFS=${LFS:-lfs}
LCTL=${LCTL:-lctl}
SETSTRIPE=${SETSTRIPE:-"$LFS setstripe"}
GETSTRIPE=${GETSTRIPE:-"$LFS getstripe"}


TSTUSR=${TSTUSR:-"quota_usr"}
TSTID=${TSTID:-60000}
RUNAS="runas -u $TSTID -g $TSTID"

# OST pools tests
POOL=${POOL:-pool1}
POOL2=${POOL2:-pool2}
POOL3=${POOL3:-pool3}
NON_EXISTANT_POOL=nonexistantpool
NON_EXISTANT_FS=nonexistantfs
INVALID_POOL=some_invalid_pool_name
TGT_COUNT=$OSTCOUNT
TGT_FIRST=0
TGT_MAX=$((TGT_COUNT-1))
TGT_STEP=1
TGT_LIST=$(seq $TGT_FIRST $TGT_STEP $TGT_MAX)
TGT_LIST2=$(seq $TGT_FIRST 2 $TGT_MAX)

TGT_ALL="$FSNAME-OST[$TGT_FIRST-$TGT_MAX/1]"
TGT_HALF="$FSNAME-OST[$TGT_FIRST-$TGT_MAX/2]"

TGT_UUID=$(for i in $TGT_LIST; do printf "$FSNAME-OST%04x_UUID " $i; done)
TGT_UUID2=$(for i in $TGT_LIST2; do printf "$FSNAME-OST%04x_UUID " $i; done)

create_dir() {
    local dir=$1
    local pool=$2
    local count=${3:-"-1"}
    local idx=$4

    mkdir -p $dir
    if [[ -n $4 ]]; then
        $SETSTRIPE -c $count -p $pool $dir -o $idx
    else
        $SETSTRIPE -c $count -p $pool $dir
    fi
    [[ $? -eq 0 ]] || \
        error "$SETSTRIPE -p $pool $dir failed."
}

create_file() {
    local file=$1
    local pool=$2
    local count=${3:-"-1"}
    local index=${4:-"-1"}
    rm -f $file
    $SETSTRIPE -o $index -c $count -p $pool $file
    [[ $? -eq 0 ]] || \
        error "$SETSTRIPE -p $pool $file failed."
}

osts_in_pool() {
    local pool=$1
    local res
    for i in $(do_facet $SINGLEMDS lctl pool_list $FSNAME.$pool | \
        grep -v "^Pool:" | sed -e 's/_UUID$//;s/^.*-OST//'); do
      res="$res $(printf "%d" 0x$i)"
    done
    echo $res
}

check_dir_in_pool() {
    local dir=$1
    local pool=$2
    local res=$($GETSTRIPE $dir | grep "^stripe_count:" \
        | cut -d ':' -f 5 | tr -d "[:blank:]")
    if [[ "$res" == "$pool" ]]; then
        return 0
    else
        error found $res instead of $pool
        return 1
    fi
}

check_file_in_pool() {
    local osts=$(osts_in_pool $2)
    check_file_in_osts $1 "$osts" $3
}

check_file_in_osts() {
        local file=$1
        local pool_list=${2:-$TGT_LIST}
        local count=$3
        local res=$($GETSTRIPE $file | grep 0x | cut -f2)
        local i
        for i in $res
        do
                found=$(echo :$pool_list: | tr " " ":"  | grep :$i:)
                if [[ "$found" == "" ]]; then
                        echo "pool list: $pool_list"
                        echo "striping: $res"
                        $GETSTRIPE $file
                        error "$file not allocated from OSTs $pool_list."
                        return 1
                fi
        done

        local ost_count=$($GETSTRIPE $file | grep 0x | wc -l)
        [[ -n "$count" ]] && [[ $ost_count -ne $count ]] && \
            { error "Stripe count $count expected; got $ost_count" && return 1; }

        return 0
}

file_pool() {
    $GETSTRIPE -v $1 | grep "^lmm_pool:" | tr -d "[:blank:]" | cut -f 2 -d ':'
}

check_file_not_in_pool() {
    local file=$1
    local pool=$2
    local res=$(file_pool $file)

    if [[ "$res" == "$pool" ]]; then
        error "File $file is in pool: $res"
        return 1
    else
        return 0
    fi
}

check_dir_not_in_pool() {
    local dir=$1
    local pool=$2
    local res=$($GETSTRIPE -v $dir | grep "^stripe_count" | head -1 | \
        cut -f 8 -d ' ')
    if [[ "$res" == "$pool" ]]; then
        error "File $dir is in pool: $res"
        return 1
    else
        return 0
    fi
}

drain_pool() {
    pool=$1
    wait_update $HOSTNAME "lctl get_param -n lov.$FSNAME-*.pools.$pool" ""\
        ||error "Failed to remove targets from pool: $pool"
}

add_pool() {
    local pool=$1
    local osts=$2
    local tgt="${3}$(lctl get_param -n lov.$FSNAME-*.pools.$pool | \
        sort -u | tr '\n' ' ')"

    do_facet $SINGLEMDS lctl pool_add $FSNAME.$pool $osts
    local RC=$?
    [[ $RC -ne 0 ]] && return $RC

    wait_update $HOSTNAME "lctl get_param -n lov.$FSNAME-*.pools.$pool | \
        sort -u | tr '\n' ' ' " "$tgt" || RC=1
    [[ $RC -ne 0 ]] && error "pool_add failed: $1; $2"
    return $RC
}

create_pool_nofail() {
    create_pool $FSNAME.$1
    if [[ $? != 0 ]]
    then
        error "Pool creation of $1 failed"
    fi
}

create_pool_fail() {
    create_pool $FSNAME.$1
    if [[ $? == 0 ]]
    then
        error "Pool creation of $1 succeeded; should have failed"
    fi
}

cleanup_tests() {
    # Destroy pools from previous test runs
    for p in $(do_facet $SINGLEMDS lctl pool_list $FSNAME | \
      grep $FSNAME.pool[0-$OSTCOUNT]); do
        destroy_pool_int $p;
    done
    rm -rf $DIR/d0.${TESTSUITE}
}

ost_pools_init() {
    cleanup_tests
}

set_cleanup_trap() {
    trap "cleanup_pools $FSNAME" EXIT
}

# Initialization
remote_mds_nodsh && skip "remote MDS with nodsh" && exit 0
remote_ost_nodsh && skip "remote OST with nodsh" && exit 0
ost_pools_init


# Tests for new commands added
test_1() {
    set_cleanup_trap
    echo "Creating a pool with a 1 character pool name"
    create_pool_nofail p

    echo "Creating a pool with a 10 character pool name"
    create_pool_nofail p123456789
    destroy_pool p123456789

    echo "Creating a pool with a 16 character pool name"
    create_pool_nofail p123456789123456
    destroy_pool p123456789123456

    echo "Creating a pool with a 17 character pool name; should fail"
    create_pool_fail p1234567891234567

    echo "Creating a pool with a 1000 character pool name; should fail"
    NAME="p"
    for i in `seq 1 999`; do NAME=${NAME}"o"; done
    create_pool_fail $NAME

    echo "pool_new should fail if fs-name or poolname are missing."
    do_facet $SINGLEMDS lctl pool_new .pool1 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_new did not fail even though fs-name was missing."
    do_facet $SINGLEMDS lctl pool_new pool1 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_new did not fail even though fs-name was missing."
    do_facet $SINGLEMDS lctl pool_new ${FSNAME}. 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_new did not fail even though pool name was missing."
    do_facet $SINGLEMDS lctl pool_new . 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_new did not fail even though pool name and fs-name " \
            "were missing."
    do_facet $SINGLEMDS lctl pool_new ${FSNAME},pool1 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_new did not fail even though pool name format was wrong"
    do_facet $SINGLEMDS lctl pool_new ${FSNAME}/pool1 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_new did not fail even though pool name format was wrong"

    do_facet $SINGLEMDS lctl pool_new ${FSNAME}.p 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_new did not fail even though pool1 existed"
    destroy_pool p

}
run_test 1 "Test lctl pool_new  ========================================="

test_2a() {
    set_cleanup_trap
    destroy_pool $POOL

    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $FSNAME-OST0000 2>/dev/null
    [[ $? -ne 0 ]] || \
        error " pool_add did not fail even though pool did " \
        " not exist."
}
run_test 2a "pool_add: non-existant pool"

test_2b() {
    set_cleanup_trap
    do_facet $SINGLEMDS lctl pool_add $FSNAME.p1234567891234567890 \
        $FSNAME-OST0000 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_add did not fail even though pool name was invalid."
}
run_test 2b "pool_add: Invalid pool name"

# Testing various combinations of OST name list
test_2c() {
    set_cleanup_trap
    local TGT
    local RC

    lctl get_param -n lov.$FSNAME-*.pools.$POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        destroy_pool $POOL

    create_pool_nofail $POOL

    # 1. OST0000
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL OST0000
    RC=$?; [[ $RC -eq 0 ]] || \
        error "pool_add failed. $FSNAME $POOL OST0000: $RC"
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL OST0000
    drain_pool $POOL

    # 2. $FSNAME-OST0000
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $FSNAME-OST0000
    RC=$?; [[ $RC -eq 0 ]] || \
        error "pool_add failed. $FSNAME $POOL $FSNAME-OST0000: $RC"
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $FSNAME-OST0000
    drain_pool $POOL

    # 3. $FSNAME-OST0000_UUID
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $FSNAME-OST0000_UUID
    RC=$?; [[ $RC -eq 0 ]] || \
        error "pool_add failed. $FSNAME $POOL $FSNAME-OST0000_UUID: $RC"
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $FSNAME-OST0000_UUID
    drain_pool $POOL

    # 4. $FSNAME-OST[0,1,2,3,]
    TGT="$FSNAME-OST["
    for i in $TGT_LIST; do TGT=${TGT}$(printf "$i," $i); done
    TGT="${TGT}]"
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $TGT
    [[ $? -eq 0 ]] || \
        error "pool_add failed. $FSNAME.$POOL $TGT. $RC"
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $TGT
    drain_pool $POOL

    # 5. $FSNAME-OST[0-5/1]
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $TGT_ALL
    RC=$?; [[ $RC -eq 0 ]] || \
        error "pool_add failed. $FSNAME $POOL" "$TGT_ALL $RC"
    wait_update $HOSTNAME "lctl get_param -n lov.$FSNAME-*.pools.$POOL | \
      sort -u | tr '\n' ' ' " "$TGT_UUID" || error "Add to pool failed"
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $TGT_ALL
    drain_pool $POOL

    destroy_pool $POOL
}
run_test 2c "pool_add: OST index combinations ==========================="

test_2d() {
    set_cleanup_trap
    local TGT
    local RC

    lctl get_param -n lov.$FSNAME-*.pools.$POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        destroy_pool $POOL

    create_pool_nofail $POOL

    TGT=$(printf "$FSNAME-OST%04x_UUID " $OSTCOUNT)
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $TGT
    RC=$?; [[ $RC -ne 0 ]] || \
        error "pool_add succeeded for an OST ($TGT) that does not exist."

    destroy_pool $POOL
}
run_test 2d "pool_add: OSTs that don't exist should be rejected ========"

test_2e() {
    set_cleanup_trap
    local TGT
    local RC
    local RESULT

    $LCTL get_param -n lov.$FSNAME-*.pools.$POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        destroy_pool $POOL

    create_pool_nofail $POOL

    TGT="$FSNAME-OST0000_UUID "
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $TGT
    wait_update $HOSTNAME "lctl get_param -n lov.$FSNAME-*.pools.$POOL | \
      sort -u | tr '\n' ' ' " "$TGT" || error "Add to pool failed"
    RESULT=$(do_facet $SINGLEMDS \
        "LOCALE=C $LCTL pool_add $FSNAME.$POOL $TGT 2>&1")
    RC=$?
    echo $RESULT

    [[ $RC -ne 0 ]] || \
        error "pool_add succeeded for an OST that was already in the pool."

    [[ $(grep "already in pool" <<< $RESULT) ]] || \
        error "pool_add failed as expected but error message not as expected."

    destroy_pool $POOL
}
run_test 2e "pool_add: OST already in a pool should be rejected ========"

test_3a() {
    set_cleanup_trap
    lctl get_param -n lov.$FSNAME-*.pools.$POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        destroy_pool $POOL

    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $FSNAME-OST0000 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_remove did not fail even though pool did not exist."
}
run_test 3a "pool_remove: non-existant pool"

test_3b() {
    set_cleanup_trap
    do_facet $SINGLEMDS lctl pool_remove ${NON_EXISTANT_FS}.$POOL OST0000 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_remove did not fail even though fsname did not exist."
}
run_test 3b "pool_remove: non-existant fsname"

test_3c() {
    set_cleanup_trap
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.p1234567891234567890 \
        $FSNAME-OST0000 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_remove did not fail even though pool name was invalid."
}
run_test 3c "pool_remove: Invalid pool name"

# Testing various combinations of OST name list
test_3d() {
    set_cleanup_trap
    lctl get_param -n lov.$FSNAME-*.pools.$POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        destroy_pool $POOL

    create_pool_nofail $POOL
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL OST0000
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL OST0000
    [[ $? -eq 0 ]] || \
        error "pool_remove failed. $FSNAME $POOL OST0000"
    drain_pool $POOL

    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $FSNAME-OST0000
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $FSNAME-OST0000
    [[ $? -eq 0 ]] || \
        error "pool_remove failed. $FSNAME $POOL $FSNAME-OST0000"
    drain_pool $POOL

    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $FSNAME-OST0000_UUID
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $FSNAME-OST0000_UUID
    [[ $? -eq 0 ]] || \
        error "pool_remove failed. $FSNAME $POOL $FSNAME-OST0000_UUID"
    drain_pool $POOL

    add_pool $POOL $TGT_ALL "$TGT_UUID"
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $TGT_ALL
    [[ $? -eq 0 ]] || \
        error "pool_remove failed. $FSNAME $POOL" $TGT_ALL
    drain_pool $POOL

    destroy_pool $POOL
}
run_test 3d "pool_remove: OST index combinations ==========================="

test_4a() {
    set_cleanup_trap
    lctl get_param -n lov.$FSNAME-*.pools.$POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        destroy_pool $POOL

    do_facet $SINGLEMDS lctl pool_destroy $FSNAME.$POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_destroy did not fail even though pool did not exist."
}
run_test 4a "pool_destroy: non-existant pool"

test_4b() {
    set_cleanup_trap
    do_facet $SINGLEMDS lctl pool_destroy ${NON_EXISTANT_FS}.$POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_destroy did not fail even though the filesystem did not exist."
}
run_test 4b "pool_destroy: non-existant fs-name"

test_4c() {
    set_cleanup_trap
    create_pool_nofail $POOL
    add_pool $POOL "OST0000" "$FSNAME-OST0000_UUID "

    do_facet $SINGLEMDS lctl pool_destroy ${FSNAME}.$POOL
    [[ $? -ne 0 ]] || \
        error "pool_destroy succeeded with a non-empty pool."
    destroy_pool $POOL
}
run_test 4c "pool_destroy: non-empty pool ==============================="

sub_test_5() {
    local LCMD=$1

    $LCMD pool_list 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_list did not fail even though fsname was not mentioned."

    destroy_pool $POOL 2>/dev/null
    destroy_pool $POOL2 2>/dev/null

    create_pool_nofail $POOL
    create_pool_nofail $POOL2
    $LCMD pool_list $FSNAME
    [[ $? -eq 0 ]] || \
        error "pool_list $FSNAME failed."

    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL $TGT_ALL

    $LCMD pool_list $FSNAME.$POOL
    [[ $? -eq 0 ]] || \
        error "pool_list $FSNAME.$POOL failed."

    $LCMD pool_list ${NON_EXISTANT_FS} 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_list did not fail for a non-existant fsname $NON_EXISTANT_FS"

    $LCMD pool_list ${FSNAME}.$NON_EXISTANT_POOL 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_list did not fail for a non-existant pool $NON_EXISTANT_POOL"

    if [[ ! $(grep $SINGLEMDS <<< $LCMD) ]]; then
        echo $LCMD pool_list $DIR
        $LCMD pool_list $DIR
        [[ $? -eq 0 ]] || \
            error "pool_list failed for $DIR"

        mkdir -p ${DIR}/d1
        $LCMD pool_list ${DIR}/d1
        [[ $? -eq 0 ]] || \
            error "pool_list failed for ${DIR}/d1"
    fi

    rm -rf ${DIR}nonexistant
    $LCMD pool_list ${DIR}nonexistant 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "pool_list did not fail for invalid mountpoint ${DIR}nonexistant"

    destroy_pool $POOL
    destroy_pool $POOL2
}

test_5() {
    set_cleanup_trap
    # Issue commands from client
    sub_test_5 $LCTL
    sub_test_5 $LFS

    # Issue commands from MDS
    sub_test_5 "do_facet $SINGLEMDS lctl"
    sub_test_5 "do_facet $SINGLEMDS lfs"

}
run_test 5 "lfs/lctl pool_list"

test_6() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local POOL_DIR=$POOL_ROOT/dir_tst
    local POOL_FILE=$POOL_ROOT/file_tst

    create_pool_nofail $POOL

    do_facet $SINGLEMDS lctl pool_list $FSNAME
    [[ $? -eq 0 ]] || \
        error "pool_list $FSNAME failed."

    add_pool $POOL $TGT_ALL "$TGT_UUID"

    mkdir -p $POOL_DIR
    $SETSTRIPE -c -1 -p $POOL $POOL_DIR
    [[ $? -eq 0 ]] || \
        error "$SETSTRIPE -p $POOL failed."
    check_dir_in_pool $POOL_DIR $POOL

    # If an invalid pool name is specified, the command should fail
    $SETSTRIPE -c 2 -p $INVALID_POOL $POOL_DIR 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "setstripe to invalid pool did not fail."

    # If the pool name does not exist, the command should fail
    $SETSTRIPE -c 2 -p $NON_EXISTANT_POOL $POOL_DIR 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "setstripe to non-existant pool did not fail."

    # lfs setstripe should work as before if a pool name is not specified.
    $SETSTRIPE -c -1 $POOL_DIR
    [[ $? -eq 0 ]] || \
        error "$SETSTRIPE -p $POOL_DIR failed."
    $SETSTRIPE -c -1 $POOL_FILE
    [[ $? -eq 0 ]] || \
        error "$SETSTRIPE -p $POOL_FILE failed."

    # lfs setstripe should fail if a start index that is outside the
    # pool is specified.
    create_pool_nofail $POOL2
    add_pool $POOL2 "OST0000" "$FSNAME-OST0000_UUID "
    $SETSTRIPE -o 1 -p $POOL2 $ROOT_POOL/$tfile 2>/dev/null
    [[ $? -ne 0 ]] || \
        error "$SETSTRIPE with start index outside the pool did not fail."

}
run_test 6 "getstripe/setstripe"

test_11() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}

    [[ $OSTCOUNT -le 1 ]] && skip_env "Need atleast 2 OSTs" && return

    create_pool_nofail $POOL
    create_pool_nofail $POOL2

    local start=$((TGT_FIRST+1))
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL2 \
        $FSNAME-OST[$start-$TGT_MAX/2]

    add_pool $POOL $TGT_HALF "$TGT_UUID2"

    create_dir $POOL_ROOT/dir1  $POOL
    create_dir $POOL_ROOT/dir2  $POOL2
    check_dir_in_pool $POOL_ROOT/dir1 $POOL
    check_dir_in_pool $POOL_ROOT/dir1 $POOL

    local numfiles=100
    createmany -o $POOL_ROOT/dir1/$tfile $numfiles || \
        error "createmany $POOL_ROOT/dir1/$tfile failed!"

    for file in $POOL_ROOT/dir1/*; do
        check_file_in_pool $file $POOL
    done

    createmany -o $POOL_ROOT/dir2/$tfile $numfiles || \
        error "createmany $POOL_ROOT/dir2/$tfile failed!"
    for file in $POOL_ROOT/dir2/*; do
        check_file_in_pool $file $POOL2
    done

    rm -rf $POOL_ROOT/dir?

    return 0
}
run_test 11 "OSTs in overlapping/multiple pools"

test_12() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}

    [[ $OSTCOUNT -le 2 ]] && skip_env "Need atleast 3 OSTs" && return

    create_pool_nofail $POOL
    create_pool_nofail $POOL2

    local start=$((TGT_FIRST+1))
    do_facet $SINGLEMDS lctl pool_add $FSNAME.$POOL2 \
        $FSNAME-OST[$start-$TGT_MAX/2]

    add_pool $POOL $TGT_HALF "$TGT_UUID2"

    echo creating some files in $POOL and $POOL2

    create_dir $POOL_ROOT/dir1  $POOL
    create_dir $POOL_ROOT/dir2  $POOL2
    create_file $POOL_ROOT/file1 $POOL
    create_file $POOL_ROOT/file2 $POOL2

    echo Checking the files created
    check_dir_in_pool $POOL_ROOT/dir1 $POOL
    check_dir_in_pool $POOL_ROOT/dir2 $POOL2
    check_file_in_pool $POOL_ROOT/file1 $POOL
    check_file_in_pool $POOL_ROOT/file2 $POOL2

    echo Changing the pool membership
    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$POOL $FSNAME-OST[$TGT_FIRST]
    do_facet $SINGLEMDS lctl pool_list $FSNAME.$POOL
	FIRST_UUID=$(echo $TGT_UUID | awk '{print $1}')
    add_pool $POOL2 $FSNAME-OST[$TGT_FIRST] "$FIRST_UUID "
    do_facet $SINGLEMDS lctl pool_list $FSNAME.$POOL2

    echo Checking the files again    
    check_dir_in_pool $POOL_ROOT/dir1 $POOL
    check_dir_in_pool $POOL_ROOT/dir2 $POOL2
    check_file_in_osts $POOL_ROOT/file1 "$TGT_LIST2"    
    check_file_in_osts $POOL_ROOT/file2 "$(seq $start 2 $TGT_MAX)"

    echo Creating some more files
    create_dir $POOL_ROOT/dir3 $POOL
    create_dir $POOL_ROOT/dir4 $POOL2
    create_file $POOL_ROOT/file3 $POOL
    create_file $POOL_ROOT/file4 $POOL2

    echo Checking the new files 
    check_file_in_pool $POOL_ROOT/file3 $POOL
    check_file_in_pool $POOL_ROOT/file4 $POOL2

    return 0    
}
run_test 12 "OST Pool Membership"

test_13() {
    set_cleanup_trap
    [[ $OSTCOUNT -le 2 ]] && skip_env "Need atleast 3 OSTs" && return

    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local numfiles=10
    local count=3

    create_pool_nofail $POOL
    add_pool $POOL $TGT_ALL "$TGT_UUID"

    create_dir $POOL_ROOT/dir1 $POOL -1
    createmany -o $POOL_ROOT/dir1/$tfile $numfiles || \
        error "createmany $POOL_ROOT/dir1/$tfile failed!"
    for file in $POOL_ROOT/dir1/*; do
        check_file_in_pool $file $POOL $OSTCOUNT
    done

    create_file $POOL_ROOT/dir1/file1 $POOL 1 $TGT_FIRST
    create_file $POOL_ROOT/dir1/file2 $POOL 1 $((TGT_FIRST+1))
    create_file $POOL_ROOT/dir1/file3 $POOL 1 $((TGT_FIRST+2))
    check_file_in_pool $POOL_ROOT/dir1/file1 $POOL 1
    check_file_in_pool $POOL_ROOT/dir1/file2 $POOL 1
    create_file $POOL_ROOT/dir1/file3 $POOL 1 $((TGT_FIRST+2))
    check_file_in_osts $POOL_ROOT/dir1/file1 "$TGT_FIRST"
    check_file_in_osts $POOL_ROOT/dir1/file2 "$((TGT_FIRST+1))"
    check_file_in_osts $POOL_ROOT/dir1/file3 "$((TGT_FIRST+2))"

    create_dir $POOL_ROOT/dir2 $POOL $count
    createmany -o $POOL_ROOT/dir2/$tfile $numfiles || \
        error "createmany $POOL_ROOT/dir2/$tfile failed!"
    for file in $POOL_ROOT/dir2/*; do
        check_file_in_pool $file $POOL $count
    done

    create_dir $POOL_ROOT/dir3 $POOL $count $((TGT_FIRST+1))
    createmany -o $POOL_ROOT/dir3/$tfile_ $numfiles || \
        error "createmany $POOL_ROOT/dir3/$tfile_ failed!"
    for file in $POOL_ROOT/dir3/*; do
        check_file_in_pool $file $POOL $count
    done

    create_dir $POOL_ROOT/dir4 $POOL 1
    createmany -o $POOL_ROOT/dir4/$tfile_ $numfiles || \
        error "createmany $POOL_ROOT/dir4/$tfile_ failed!"
    for file in $POOL_ROOT/dir4/*; do
        check_file_in_pool $file $POOL 1
    done

    create_dir $POOL_ROOT/dir5 $POOL 1 $((TGT_FIRST+2))
    createmany -o $POOL_ROOT/dir5/$tfile_ $numfiles || \
        error "createmany $POOL_ROOT/dir5/$tfile_ failed!"
    for file in $POOL_ROOT/dir5/*; do
        check_file_in_pool $file $POOL 1
        check_file_in_osts  $file "$((TGT_FIRST+2))"
    done

    rm -rf create_dir $POOL_ROOT/dir?

    return 0
}
run_test 13 "Striping characteristics in a pool"

test_14() {
    set_cleanup_trap
    [[ $OSTCOUNT -le 2 ]] && skip_env "Need atleast 3 OSTs" && return

    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local numfiles=100
    local i

    # Create a new filesystem that is guaranteed to be balanced.
    formatall
    setupall

    create_pool_nofail $POOL
    create_pool_nofail $POOL2

    add_pool $POOL $TGT_HALF "$TGT_UUID2"
    add_pool $POOL2 "OST0000" "$FSNAME-OST0000_UUID "

    create_dir $POOL_ROOT/dir1 $POOL 1
    create_file $POOL_ROOT/dir1/file $POOL 1
    local OST=$($GETSTRIPE $POOL_ROOT/dir1/file | grep 0x | cut -f2)    
    i=0
    while [[ $i -lt $numfiles ]];
    do
        OST=$((OST+2))
        [[ $OST -gt $TGT_MAX ]] && OST=$TGT_FIRST

        # echo "Iteration: $i OST: $OST"
        create_file $POOL_ROOT/dir1/file${i} $POOL 1
        check_file_in_pool $POOL_ROOT/dir1/file${i} $POOL
        i=$((i+1))
    done

    # Fill up OST0 until it is nearly full.
    # Create 9 files of size OST0_SIZE/10 each.
    create_dir $POOL_ROOT/dir2 $POOL2 1
    $LFS df $POOL_ROOT/dir2
    echo "Filling up OST0"
    OST0_SIZE=`$LFS df $POOL_ROOT/dir2 | awk '/\[OST:0\]/ {print $4}'`
    FILE_SIZE=$((OST0_SIZE/1024/10))
    i=1
    while [[ $i -lt 10 ]];
    do
      dd if=/dev/zero of=$POOL_ROOT/dir2/f${i} bs=1M count=$FILE_SIZE
      i=$((i+1))
    done
    $LFS df $POOL_ROOT/dir2

    # OST $TGT_FIRST is no longer favored; but it may still be used.
    create_dir $POOL_ROOT/dir3 $POOL 1
    create_file $POOL_ROOT/dir3/file $POOL 1
    createmany -o $POOL_ROOT/dir3/$tfile_ $numfiles || \
        error "createmany $POOL_ROOT/dir3/$tfile_ failed!"
    for file in $POOL_ROOT/dir3/*; do
        check_file_in_pool $file $POOL
    done

    rm -rf $POOL_ROOT
    return 0
}
run_test 14 "Round robin and QOS striping within a pool"

test_15() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local numfiles=100
    local i=0

    while [[ $i -lt $OSTCOUNT ]]
    do
      create_pool_nofail pool${i}

      local tgt=$(printf "$FSNAME-OST%04x_UUID " $i)
      add_pool pool${i} "$FSNAME-OST[$i]" "$tgt"
      create_dir $POOL_ROOT/dir${i} pool${i}
      createmany -o $POOL_ROOT/dir$i/$tfile $numfiles || \
          error "createmany $POOL_ROOT/dir$i/$tfile failed!"

      for file in $POOL_ROOT/dir$i/*; do
          check_file_in_osts $file $i
      done

      i=$((i+1))
    done

    return 0
}
run_test 15 "One directory per OST/pool"

test_16() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local numfiles=10
    local i=0

    create_pool_nofail $POOL

    add_pool $POOL $TGT_HALF "$TGT_UUID2"

    local dir=$POOL_ROOT/$tdir
    create_dir $dir $POOL

    for i in $(seq 1 10); do
        dir=${dir}/dir${i}
    done
    mkdir -p $dir

    createmany -o $dir/$tfile $numfiles || \
          error "createmany $dir/$tfile failed!"

    for file in $dir/*; do
        check_file_in_pool $file $POOL
    done

    rm -rf $POOL_ROOT/$tdir

    return 0
}
run_test 16 "Inheritance of pool properties"

test_17() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local numfiles=10
    local i=0

    create_pool_nofail $POOL

    add_pool $POOL $TGT_ALL "$TGT_UUID"

    local dir=$POOL_ROOT/dir
    create_dir $dir $POOL

    createmany -o $dir/${tfile}1_ $numfiles || \
          error "createmany $dir/${tfile}1_ failed!"

    for file in $dir/*; do
        check_file_in_pool $file $POOL
    done

    destroy_pool $POOL

    createmany -o $dir/${tfile}2_ $numfiles || \
          error "createmany $dir/${tfile}2_ failed!"

    rm -rf $dir
    return 0
}
run_test 17 "Referencing an empty pool"

create_perf() {
    local cdir=$1/d
    local numfiles=$2
    local time

    mkdir -p $cdir
    sync; sleep 5 # give pending IO a chance to go to disk
    stat=$(createmany -o $cdir/${tfile} $numfiles)
    rm -rf $cdir
    sync
    time=$(echo $stat | cut -f 5 -d ' ')
    echo $stat >> /dev/stderr
    echo $time
}

test_18() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local numfiles=30000
    local plaindir=$POOL_ROOT/plaindir
    local pooldir=$POOL_ROOT/pooldir
	local t1=0
	local t2=0
	local t3=0
	local diff

    for i in $(seq 1 3);
    do
        echo "Create performance, iteration $i, $numfiles files x 3"

		time1=$(create_perf $plaindir $numfiles)
		echo "iter $i: $numfiles creates without pool: $time1"
		t1=$(echo "scale=2; $t1 + $time1" | bc)

		create_pool_nofail $POOL > /dev/null
		add_pool $POOL $TGT_ALL "$TGT_UUID" > /dev/null
		create_dir $pooldir $POOL
		time2=$(create_perf $pooldir $numfiles)
		echo "iter $i: $numfiles creates with pool: $time2"
		t2=$(echo "scale=2; $t2 + $time2" | bc)

		destroy_pool $POOL > /dev/null
		time3=$(create_perf $pooldir $numfiles)
		echo "iter $i: $numfiles creates with missing pool: $time3"
		t3=$(echo "scale=2; $t3 + $time3" | bc)

		echo
	done

	time1=$(echo "scale=2; $t1 / $i" | bc)
	echo Avg time taken for $numfiles creates without pool: $time1
	time2=$(echo "scale=2; $t2 / $i" | bc)
	echo Avg time taken for $numfiles creates with pool: $time2
	time3=$(echo "scale=2; $t3 / $i" | bc)
	echo Avg time taken for $numfiles creates with missing pool: $time3

	# Set this high until we establish a baseline for what the degradation
	# is / should be
	max=30
    diff=$(echo "scale=2; ($time2 - $time1) * 100 / $time1" | bc)
    echo  "No pool to wide pool: $diff %."
    deg=$(echo "scale=2; $diff > $max" | bc)
    [ "$deg" == "1" ] && error_ignore 23408  "Degradation with wide pool is $diff % (> $max %)"

	max=15
    diff=$(echo "scale=2; ($time3 - $time1) * 100 / $time1" | bc)
    echo  "No pool to missing pool: $diff %."
    deg=$(echo "scale=2; $diff > $max" | bc)
    [ "$deg" == "1" ] && error_ignore 23408 "Degradation with missing pool is $diff % (> $max %)"

    return 0
}
run_test 18 "File create in a directory which references a deleted pool"


test_19() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local numfiles=12
    local dir1=$POOL_ROOT/dir1
    local dir2=$POOL_ROOT/dir2
    local i=0

    create_pool_nofail $POOL

    add_pool $POOL $TGT_HALF "$TGT_UUID2"

    create_dir $dir1 $POOL
    createmany -o $dir1/${tfile} $numfiles || \
          error "createmany $dir1/${tfile} failed!"
    for file in $dir1/*; do
        check_file_in_pool $file $POOL
    done

    mkdir -p $dir2
    createmany -o $dir2/${tfile} $numfiles || \
          error "createmany $dir2/${tfile} failed!"
    for file in $dir2/*; do
        check_file_not_in_pool $file $POOL
    done

    rm -rf $dir1 $dir2

    return 0
}
run_test 19 "Pools should not come into play when not specified"

test_20() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    local numfiles=12
    local dir1=$POOL_ROOT/dir1
    local dir2=$dir1/dir2
    local dir3=$dir1/dir3
    local i=0
    local TGT

    create_pool_nofail $POOL
    create_pool_nofail $POOL2

    add_pool $POOL $TGT_HALF "$TGT_UUID2"

    local start=$((TGT_FIRST+1))
    TGT=$(for i in `seq $start 2 $TGT_MAX`; \
        do printf "$FSNAME-OST%04x_UUID " $i; done)
    add_pool $POOL2 "$FSNAME-OST[$start-$TGT_MAX/2]" "$TGT"

    create_dir $dir1 $POOL
    create_file $dir1/file1 $POOL2
    create_dir $dir2 $POOL2
    touch $dir2/file2
    mkdir $dir3
    $SETSTRIPE -c 1 $dir3 # No pool assignment
    touch $dir3/file3
    $SETSTRIPE -c 1 $dir2/file4 # No pool assignment

    check_file_in_pool $dir1/file1 $POOL2
    check_file_in_pool $dir2/file2 $POOL2

    check_dir_not_in_pool $dir3 $POOL
    check_dir_not_in_pool $dir3 $POOL2

    check_file_not_in_pool $dir3/file3 $POOL
    check_file_not_in_pool $dir3/file3 $POOL2

    check_file_not_in_pool $dir2/file4 $POOL
    check_file_not_in_pool $dir2/file4 $POOL2

    rm -rf $dir1

    return 0
}
run_test 20 "Different pools in a directory hierarchy."

test_21() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    [[ $OSTCOUNT -le 1 ]] && skip_env "Need atleast 2 OSTs" && return

    local numfiles=12
    local i=0
    local dir=$POOL_ROOT/dir

    create_pool_nofail $POOL

    add_pool $POOL $TGT_HALF "$TGT_UUID2"

    create_dir $dir $POOL $OSTCOUNT
    create_file $dir/file1 $POOL $OSTCOUNT
    $GETSTRIPE -v $dir/file1
    check_file_in_pool $dir/file1 $POOL

    rm -rf $dir

    return 0
}
run_test 21 "OST pool with fewer OSTs than stripe count"

add_loop() {
    local pool=$1
    local step=$2

    echo loop for $pool

    for c in $(seq 1 10);
    do
        echo "Pool $pool, iteration $c"
	do_facet $SINGLEMDS lctl pool_add $FSNAME.$pool OST[$TGT_FIRST-$TGT_MAX/$step] 2>/dev/null
	local TGT_SECOND=$(($TGT_FIRST+$step))
	if [ "$TGT_SECOND" -le "$TGT_MAX" ]; then
	    do_facet $SINGLEMDS lctl pool_remove $FSNAME.$pool OST[$TGT_SECOND-$TGT_MAX/$step]
	fi
    done
    echo loop for $pool complete
}

test_22() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    [[ $OSTCOUNT -le 1 ]] && skip_env "Need at least 2 OSTs" && return

    local numfiles=100

    create_pool_nofail $POOL
    add_pool $POOL "OST0000" "$FSNAME-OST0000_UUID "
    create_pool_nofail $POOL2
    add_pool $POOL2 "OST0000" "$FSNAME-OST0000_UUID "

    add_loop $POOL 1 &
    add_loop $POOL2 2 &
    sleep 5
    create_dir $POOL_ROOT $POOL
    createmany -o $POOL_ROOT/${tfile} $numfiles || \
          error "createmany $POOL_ROOT/${tfile} failed!"
    wait

    return 0
}
run_test 22 "Simultaneous manipulation of a pool"

test_23() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    [[ $OSTCOUNT -le 1 ]] && skip_env "Need atleast 2 OSTs" && return

    mkdir -p $POOL_ROOT
    check_runas_id $TSTID $TSTID $RUNAS  || {
        skip_env "User $RUNAS_ID does not exist - skipping"
        return 0
    }

    local i=0
    local TGT
    local BLK_SZ=1024
    local BUNIT_SZ=1024  	# min block quota unit(kB)
    local LOVNAME=`lctl get_param -n llite.*.lov.common_name | tail -n 1`
    local OSTCOUNT=`lctl get_param -n lov.$LOVNAME.numobd`
    local LIMIT
    local dir=$POOL_ROOT/dir
    local file="$dir/$tfile-quota"

    create_pool_nofail $POOL

    local TGT=$(for i in `seq $TGT_FIRST 3 $TGT_MAX`; \
        do printf "$FSNAME-OST%04x_UUID " $i; done)
    add_pool $POOL "$FSNAME-OST[$TGT_FIRST-$TGT_MAX/3]" "$TGT"
    create_dir $dir $POOL

    $LFS quotaoff -ug $MOUNT
    $LFS quotacheck -ug $MOUNT
    LIMIT=$((BUNIT_SZ * (OSTCOUNT + 1)))
    $LFS setquota -u $TSTUSR -b $LIMIT -B $LIMIT $dir
    sleep 3
    $LFS quota -v -u $TSTUSR $dir

    $LFS setstripe $file -c 1 -p $POOL
    chown $TSTUSR.$TSTUSR $file
    ls -l $file
    type runas

    LOCALE=C $RUNAS dd if=/dev/zero of=$file bs=$BLK_SZ count=$((BUNIT_SZ*2)) || true
    $LFS quota -v -u $TSTUSR $dir
    cancel_lru_locks osc
    stat=$(LOCALE=C $RUNAS dd if=/dev/zero of=$file bs=$BLK_SZ count=$BUNIT_SZ seek=$((BUNIT_SZ*2)) 2>&1)
    RC=$?
    echo $stat
    [[ $RC -eq 0 ]] && error "dd did not fail with EDQUOT."
    echo $stat | grep "Disk quota exceeded" > /dev/null
    [[ $? -eq 1 ]] && error "dd did not fail with EDQUOT."
    $LFS quota -v -u $TSTUSR $dir

    echo "second run"
    $LFS quotaoff -ug $MOUNT
    # $LFS setquota -u $TSTUSR -b $LIMIT -B $LIMIT $dir
    chown $TSTUSR.$TSTUSR $dir
    i=0
    RC=0
    while [ $RC -eq 0 ];
    do
      i=$((i+1))
      stat=$(LOCALE=C $RUNAS2 dd if=/dev/zero of=${file}$i bs=1M \
          count=$((LIMIT*LIMIT)) 2>&1)
      RC=$?
      if [ $RC -eq 1 ]; then
          echo $stat
          echo $stat | grep "Disk quota exceeded"
          [[ $? -eq 0 ]] && error "dd failed with EDQUOT"

          echo $stat | grep "No space left on device"
          [[ $? -ne 0 ]] && error "dd did not fail with ENOSPC; " \
              "failed with $stat"
      fi
    done

    df -h

    rm -rf $POOL_ROOT

    return 0
}
run_test 23 "OST pools and quota"

test_24() {
    set_cleanup_trap
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}
    [[ $OSTCOUNT -le 1 ]] && skip_env "Need atleast 2 OSTs" && return

    local numfiles=10
    local i=0
    local TGT
    local dir
    local res


    create_pool_nofail $POOL

    add_pool $POOL $TGT_ALL "$TGT_UUID"

    create_dir $POOL_ROOT/dir1 $POOL $OSTCOUNT

    mkdir $POOL_ROOT/dir2
    $SETSTRIPE $POOL_ROOT/dir2 -p $POOL -s 65536 -i 0 -c 1 || \
        error "$SETSTRIPE $POOL_ROOT/dir2 failed"

    mkdir $POOL_ROOT/dir3
    $SETSTRIPE $POOL_ROOT/dir3 -s 65536 -i 0 -c 1 || \
        error "$SETSTRIPE $POOL_ROOT/dir3 failed"

    mkdir $POOL_ROOT/dir4

    for i in $(seq 1 4);
    do
      dir=${POOL_ROOT}/dir${i}
      local pool
      local pool1
      local count
      local count1
      local index
      local size
      local size1

      createmany -o $dir/${tfile} $numfiles || \
          error "createmany $dir/${tfile} failed!"
      res=$($GETSTRIPE -v $dir | grep "^stripe_count:")
      if [ $? -ne 0 ]; then
          res=$($GETSTRIPE -v $dir | grep "^(Default) ")
          pool=$(cut -f 9 -d ' ' <<< $res)
          index=$(cut -f 7 -d ' ' <<< $res)
          size=$(cut -f 5 -d ' ' <<< $res)
          count=$(cut -f 3 -d ' ' <<< $res)
      else
          pool=$(cut -f 8 -d ' ' <<< $res)
          index=$(cut -f 6 -d ' ' <<< $res)
          size=$(cut -f 4 -d ' ' <<< $res)
          count=$(cut -f 2 -d ' ' <<< $res)
      fi

      for file in $dir/*; do
          if [ "$pool" != "" ]; then
              check_file_in_pool $file $pool
          fi
          pool1=$(file_pool $file)
          count1=$($GETSTRIPE -v $file | grep "^lmm_stripe_count:" |\
              tr -d '[:blank:]' | cut -f 2 -d ':')
          size1=$($GETSTRIPE -v $file | grep "^lmm_stripe_size:" |\
              tr -d '[:blank:]' | cut -f 2 -d ':')
          [[ "$pool" != "$pool1" ]] && \
              error "Pool name ($pool) not inherited in $file($pool1)"
          [[ "$count" != "$count1" ]] && \
              error "Stripe count ($count) not inherited in $file ($count1)"
          [[ "$size" != "$size1" ]] && [[ "$size" != "0" ]] && \
              error "Stripe size ($size) not inherited in $file ($size1)"
      done 
    done

    rm -rf $POOL_ROOT

    return 0
}
run_test 24 "Independence of pool from other setstripe parameters"

test_25() {
    set_cleanup_trap
    local dev=$(mdsdevname ${SINGLEMDS//mds/})
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}

    mkdir -p $POOL_ROOT

    for i in $(seq 10); do
        create_pool_nofail pool$i
        do_facet $SINGLEMDS "lctl pool_add $FSNAME.pool$i OST0000; sync"
        wait_update $HOSTNAME "lctl get_param -n lov.$FSNAME-*.pools.pool$i | \
            sort -u | tr '\n' ' ' " "$FSNAME-OST0000_UUID " || \
            error "pool_add failed: $1; $2"

        stop $SINGLEMDS || return 1
        start $SINGLEMDS ${dev} $MDS_MOUNT_OPTS  || \
            { error "Failed to start $SINGLEMDS after stopping" && break; }
        wait_osc_import_state mds ost FULL
        clients_up

        wait_mds_ost_sync 
        # Veriy that the pool got created and is usable
        df $POOL_ROOT > /dev/null
        sleep 5
        # Make sure OST0 can be striped on
        $SETSTRIPE -o 0 -c 1 $POOL_ROOT/$tfile
        STR=$($GETSTRIPE $POOL_ROOT/$tfile | grep 0x | cut -f2 | tr -d " ")
        rm $POOL_ROOT/$tfile
        if [[ "$STR" == "0" ]]; then
                echo "Creating a file in pool$i"
                create_file $POOL_ROOT/file$i pool$i || break
                check_file_in_pool $POOL_ROOT/file$i pool$i || break
        else
                echo "OST 0 seems to be unavailable.  Try later."
        fi
    done

    rm -rf $POOL_ROOT
}
run_test 25 "Create new pool and restart MDS ======================="

test_26() {
    local OSTCOUNT=`lctl get_param -n lov.$LOVNAME.*clilov*.numobd`
    [[ $OSTCOUNT -le 2 ]] && skip_env "Need at least 3 OSTs" && return
    set_cleanup_trap
    local dev=$(mdsdevname ${SINGLEMDS//mds/})
    local POOL_ROOT=${POOL_ROOT:-$DIR/$tdir}

    mkdir -p $POOL_ROOT

    create_pool_nofail $POOL 
 
    do_facet $SINGLEMDS "lctl pool_add $FSNAME.pool1 OST0000; sync"
    wait_update $HOSTNAME "lctl get_param -n lov.$FSNAME-*.pools.pool1 | \
    sort -u | grep $FSNAME-OST0000_UUID " "$FSNAME-OST0000_UUID" || \
    error "pool_add failed: $1; $2"
  
    do_facet $SINGLEMDS "lctl pool_add $FSNAME.pool1 OST0002; sync"
    wait_update $HOSTNAME "lctl get_param -n lov.$FSNAME-*.pools.pool1 | \
    sort -u | grep $FSNAME-OST0002_UUID" "$FSNAME-OST0002_UUID" || \
    error "pool_add failed: $1; $2"
  
    # Veriy that the pool got created and is usable
    df $POOL_ROOT
    echo "Creating files in $POOL"

    for ((i=0;i<10;i++)); do
        #OBD_FAIL_MDS_OSC_CREATE_FAIL     0x147  
        #Fail OST0000 to make sure the objects create on the other ost in the pool 
        do_facet $SINGLEMDS lctl set_param fail_loc=0x147
        do_facet $SINGLEMDS lctl set_param fail_val=0
        create_file $POOL_ROOT/file$i $POOL 1 -1 || break
        do_facet $SINGLEMDS lctl set_param fail_loc=0
        check_file_in_pool $POOL_ROOT/file$i $POOL || break
    done
    rm -rf $POOL_ROOT
}
run_test 26 "Choose other OSTs in the pool first in the creation remedy"

cd $ORIG_PWD

complete $(basename $0) $SECONDS
cleanup_pools $FSNAME
check_and_cleanup_lustre
exit_status
