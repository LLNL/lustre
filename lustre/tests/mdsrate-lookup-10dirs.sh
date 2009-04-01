#!/bin/bash
#
# This test was used in a set of CMD3 tests (cmd3-5 test).

# Directory lookup retrieval rate 10 directories 1 million files each
# 6000 random lookups/sec per client node 62,000 random lookups/sec aggregate
# 
# In 10 dirs containing 1 million files each the mdsrate Test Program will
# perform lookups for 10 minutes. This test is run from a single node for
# #1 and from all nodes for #2 aggregate test to measure lookup performance.
# TEst performs lookups across all 10 directories.

LUSTRE=${LUSTRE:-`dirname $0`/..}
. $LUSTRE/tests/test-framework.sh
init_test_env $@
. ${CONFIG:=$LUSTRE/tests/cfg/$NAME.sh}
assert_env CLIENTS MDSRATE SINGLECLIENT MPIRUN

MACHINEFILE=${MACHINEFILE:-$TMP/$(basename $0 .sh).machines}
# Do not use name [df][0-9]* to avoid cleanup by rm, bug 18045
TESTDIR=$MOUNT/mdsrate

# Requirements
NUM_DIRS=${NIM_DIRS:-10}
NUM_FILES=${NUM_FILES:-1000000}
TIME_PERIOD=${TIME_PERIOD:-600}                        # seconds

LOG=${TESTSUITELOG:-$TMP/$(basename $0 .sh).log}
CLIENT=$SINGLECLIENT
NODES_TO_USE=${NODES_TO_USE:-$CLIENTS}
NUM_CLIENTS=$(get_node_count ${NODES_TO_USE//,/ })

rm -f $LOG

[ ! -x ${MDSRATE} ] && error "${MDSRATE} not built."

log "===== $0 ====== " 

check_and_setup_lustre
mkdir -p $TESTDIR
chmod 0777 $TESTDIR

IFree=$(inodes_available)
if [ $IFree -lt $((NUM_FILES * NUM_DIRS)) ]; then
    NUM_FILES=$((IFree / NUM_DIRS))
fi

generate_machine_file $NODES_TO_USE $MACHINEFILE || error "can not generate machinefile"

$LFS setstripe $TESTDIR -c 1
get_stripe $TESTDIR

DIRfmt="${TESTDIR}/t6-%d"

if [ -n "$NOCREATE" ]; then
    echo "NOCREATE=$NOCREATE  => no file creation."
else
    # FIXME: does it make sense to add the possibility to unlink dirfmt to mdsrate?
    for i in $(seq 0 $NUM_DIRS); do
        mdsrate_cleanup $NUM_CLIENTS $MACHINEFILE $NUM_FILES $TESTDIR/t6-$i 'f%%d' --ignore
    done

    log "===== $0 Test preparation: creating ${NUM_DIRS} dirs with ${NUM_FILES} files."
    echo "Test preparation: creating ${NUM_DIRS} dirs with ${NUM_FILES} files."

    MDSCOUNT=1
    NUM_CLIENTS=$(get_node_count ${NODES_TO_USE//,/ })

    log "===== $0 Test preparation: creating ${NUM_DIRS} dirs with ${NUM_FILES} files."
    echo "Test preparation: creating ${NUM_DIRS} dirs with ${NUM_FILES} files."

    COMMAND="${MDSRATE} ${MDSRATE_DEBUG} --mknod
                        --ndirs ${NUM_DIRS} --dirfmt '${DIRfmt}'
                        --nfiles ${NUM_FILES} --filefmt 'f%%d'"

    echo "+" ${COMMAND}
    # For files creation we can use NUM_THREADS equal to NUM_DIRS 
    # This is just a test preparation, does not matter how many threads we use for files creation;
    # we just should be aware that NUM_DIRS is less than or equal to the number of threads NUM_THREADS
    mpi_run -np ${NUM_DIRS} -machinefile ${MACHINEFILE} ${COMMAND} 2>&1 

    # No lockup if error occurs on file creation, abort.
    [ ${PIPESTATUS[0]} != 0 ] && error "mpirun ... mdsrate ... file creation failed, aborting"
fi

COMMAND="${MDSRATE} ${MDSRATE_DEBUG} --lookup --time ${TIME_PERIOD} ${SEED_OPTION}
        --ndirs ${NUM_DIRS} --dirfmt '${DIRfmt}'
        --nfiles ${NUM_FILES} --filefmt 'f%%d'"

# 1
if [ -n "$NOSINGLE" ]; then
    echo "NO Test for lookups on a single client."
else
    log "===== $0 ### 1 NODE LOOKUPS ###"
    echo "Running lookups on 1 node(s)."
    echo "+" ${COMMAND}
    mpi_run -np 1 -machinefile ${MACHINEFILE} ${COMMAND} | tee ${LOG}

    if [ ${PIPESTATUS[0]} != 0 ]; then
        [ -f $LOG ] && cat $LOG
        error "mpirun ... mdsrate ... failed, aborting"
    fi
fi

# 2
if [ -n "$NOMULTI" ]; then
    echo "NO test for lookups on multiple nodes."
else
    log "===== $0 ### ${NUM_CLIENTS} NODES LOOKUPS ###"
    echo "Running lookups on ${NUM_CLIENTS} node(s)."
    echo "+" ${COMMAND}
    mpi_run -np ${NUM_CLIENTS} -machinefile ${MACHINEFILE} ${COMMAND} | tee ${LOG}

    if [ ${PIPESTATUS[0]} != 0 ]; then
        [ -f $LOG ] && cat $LOG
        error "mpirun ... mdsrate ... failed, aborting"
    fi
fi

equals_msg `basename $0`: test complete, cleaning up
# FIXME: does it make sense to add the possibility to unlink dirfmt to mdsrate?
for i in $(seq 0 $NUM_DIRS); do
    mdsrate_cleanup $NUM_CLIENTS $MACHINEFILE $NUM_FILES $TESTDIR/t6-$i 'f%%d' --ignore
    rmdir $TESTDIR/t6-$i
done

rmdir $TESTDIR || true
rm -f $MACHINEFILE
check_and_cleanup_lustre
#rm -f $LOG

exit 0
