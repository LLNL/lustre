FSNAME=${FSNAME:-lustre}

# facet hosts
mds_HOST=${mds_HOST:-`hostname`}
mdsfailover_HOST=${mdsfailover_HOST}
mgs_HOST=${mgs_HOST:-$mds_HOST}
ost_HOST=${ost_HOST:-`hostname`}
ostfailover_HOST=${ostfailover_HOST}
CLIENTS=""
PDSH=${PDSH:-no_dsh}

TMP=${TMP:-/tmp}

MDSDEV=${MDSDEV:-$TMP/${FSNAME}-mdt}
MDSSIZE=${MDSSIZE:-400000}
MDSOPT=${MDSOPT:-"--mountfsoptions=errors=remount-ro,iopen_nopriv,user_xattr,acl"}

mdsfailover_dev=${mdsfailover_dev:-$MDSDEV}

MGSDEV=${MGSDEV:-$MDSDEV}
MGSSIZE=${MGSSIZE:-$MDSSIZE}

OSTCOUNT=${OSTCOUNT:-2}
OSTDEVBASE=${OSTDEVBASE:-$TMP/${FSNAME}-ost}
OSTSIZE=${OSTSIZE:-300000}
OSTOPT=${OSTOPT:-""}
# Can specify individual ost devs with
# OSTDEV1="/dev/sda"
# on specific hosts with
# ost1_HOST="uml2"

NETTYPE=${NETTYPE:-tcp}
MGSNID=${MGSNID:-`h2$NETTYPE $mgs_HOST`}
FSTYPE=${FSTYPE:-ldiskfs}
STRIPE_BYTES=${STRIPE_BYTES:-1048576}
STRIPES_PER_OBJ=${STRIPES_PER_OBJ:-0}
TIMEOUT=${TIMEOUT:-20}
PTLDEBUG=${PTLDEBUG:-0x33f1504}
DEBUG_SIZE=${DEBUG_SIZE:-10}
SUBSYSTEM=${SUBSYSTEM:- 0xffb7e3ff}

L_GETGROUPS=${L_GETGROUPS:-`do_facet mds which l_getgroups || echo`}

ENABLE_QUOTA=${ENABLE_QUOTA:-""}
QUOTA_TYPE=${QUOTA_TYPE:-"ug3"}
QUOTA_USERS=${QUOTA_USERS:-"quota_usr quota_2usr sanityusr sanityusr1"}
LQUOTAOPTS=${LQUOTAOPTS:-"hash_lqs_cur_bits=3"}

MKFSOPT=""
[ "x$MDSJOURNALSIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -J size=$MDSJOURNALSIZE"
[ "x$MDSISIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -i $MDSISIZE"
[ "x$MKFSOPT" != "x" ] &&
    MKFSOPT="--mkfsoptions=\\\"$MKFSOPT\\\""
[ "x$mdsfailover_HOST" != "x" ] &&
    MDSOPT=$MDSOPT" --failnode=`h2$NETTYPE $mdsfailover_HOST`"
[ "x$STRIPE_BYTES" != "x" ] &&
    MDSOPT=$MDSOPT" --param lov.stripesize=$STRIPE_BYTES"
[ "x$STRIPES_PER_OBJ" != "x" ] &&
    MDSOPT=$MDSOPT" --param lov.stripecount=$STRIPES_PER_OBJ"
[ "x$L_GETGROUPS" != "x" ] &&
    MDSOPT=$MDSOPT" --param mdt.group_upcall=$L_GETGROUPS"
MDS_MKFS_OPTS="--mdt --fsname=$FSNAME --device-size=$MDSSIZE --param sys.timeout=$TIMEOUT $MKFSOPT $MDSOPT $MDS_MKFS_OPTS"
if [[ $mds_HOST == $mgs_HOST ]] && [[ $MDSDEV == $MGSDEV ]]; then
    MDS_MKFS_OPTS="--mgs $MDS_MKFS_OPTS"
else
    MDS_MKFS_OPTS="--mgsnode=$MGSNID $MDS_MKFS_OPTS"
    mgs_MKFS_OPTS="--mgs "
fi

MKFSOPT=""
[ "x$OSTJOURNALSIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -J size=$OSTJOURNALSIZE"
[ "x$MKFSOPT" != "x" ] &&
    MKFSOPT="--mkfsoptions=\\\"$MKFSOPT\\\""
[ "x$ostfailover_HOST" != "x" ] &&
    OSTOPT=$OSTOPT" --failnode=`h2$NETTYPE $ostfailover_HOST`"
OST_MKFS_OPTS="--ost --fsname=$FSNAME --device-size=$OSTSIZE --mgsnode=$MGSNID --param sys.timeout=$TIMEOUT $MKFSOPT $OSTOPT $OST_MKFS_OPTS"

MDS_MOUNT_OPTS=${MDS_MOUNT_OPTS:-"-o loop"}
OST_MOUNT_OPTS=${OST_MOUNT_OPTS:-"-o loop"}
mgs_MOUNT_OPTS=${mgs_MOUNT_OPTS:-$MDS_MOUNT_OPTS}

#client
MOUNT=${MOUNT:-/mnt/${FSNAME}}
MOUNT1=${MOUNT1:-$MOUNT}
MOUNT2=${MOUNT2:-${MOUNT}2}
MOUNTOPT=${MOUNTOPT:-"user_xattr,acl,flock"}
DIR=${DIR:-$MOUNT}
DIR1=${DIR:-$MOUNT1}
DIR2=${DIR2:-$MOUNT2}

if [ $UID -ne 0 ]; then
        log "running as non-root uid $UID"
        RUNAS_ID="$UID"
        RUNAS=""
else
        RUNAS_ID=${RUNAS_ID:-500}
        RUNAS=${RUNAS:-"runas -u $RUNAS_ID"}
fi

FAILURE_MODE=${FAILURE_MODE:-SOFT} # or HARD
POWER_DOWN=${POWER_DOWN:-"powerman --off"}
POWER_UP=${POWER_UP:-"powerman --on"}
SLOW=${SLOW:-no}
FAIL_ON_ERROR=${FAIL_ON_ERROR:-true}

MPIRUN=$(which mpirun 2>/dev/null) || true
MPI_USER=${MPI_USER:-mpiuser}
SHARED_DIR_LOGS=${SHARED_DIR_LOGS:-""}

