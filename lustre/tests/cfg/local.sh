FSNAME=${FSNAME:-lustre}

# facet hosts
mds_HOST=${mds_HOST:-`hostname`}
mdsfailover_HOST=${mdsfailover_HOST}
mds1_HOST=${mds1_HOST:-$mds_HOST}
mds1failover_HOST=${mds1failover_HOST:-$mdsfailover_HOST}
mgs_HOST=${mgs_HOST:-$mds1_HOST}
ost_HOST=${ost_HOST:-`hostname`}
ostfailover_HOST=${ostfailover_HOST}
CLIENTS=""

TMP=${TMP:-/tmp}
FSTYPE=${FSTYPE:-ldiskfs}

DAEMONSIZE=${DAEMONSIZE:-500}
MDSCOUNT=${MDSCOUNT:-1}
[ $MDSCOUNT -gt 4 ] && MDSCOUNT=4
[ $MDSCOUNT -gt 1 ] && IAMDIR=yes
for num in $(seq $MDSCOUNT); do
    eval mds${num}_HOST=\$\{mds${num}_HOST:-$mds_HOST\}
    eval mds${num}failover_HOST=\$\{mds${num}failover_HOST:-$mdsfailover_HOST\}
done
MDSDEVBASE=${MDSDEVBASE:-$TMP/${FSNAME}-mdt}
MDSSIZE=${MDSSIZE:-200000}
MDSOPT=${MDSOPT:-""}
MDSFSTYPE=${MDSFSTYPE:-$FSTYPE}

# Default to shared mds/mgs
MGSDEV=${MGSDEV:-$MDSDEV1}
MGSSIZE=${MGSSIZE:-$MDSSIZE}
MGSOPT=${MGSOPT:-""}
MGSFSTYPE=${MGSFSTYPE:-$FSTYPE}

OSTCOUNT=${OSTCOUNT:-2}
OSTDEVBASE=${OSTDEVBASE:-$TMP/${FSNAME}-ost}
OSTSIZE=${OSTSIZE:-200000}
OSTOPT=${OSTOPT:-""}
OSTFSTYPE=${OSTFSTYPE:-$FSTYPE}
# Can specify individual ost devs with
# OSTDEV1="/dev/sda"
# on specific hosts with
# ost1_HOST="uml2"

NETTYPE=${NETTYPE:-tcp}
MGSNID=${MGSNID:-`h2$NETTYPE $mgs_HOST`}
STRIPE_BYTES=${STRIPE_BYTES:-1048576}
STRIPES_PER_OBJ=${STRIPES_PER_OBJ:-0}
SINGLEMDS=${SINGLEMDS:-"mds1"}
TIMEOUT=${TIMEOUT:-20}
PTLDEBUG=${PTLDEBUG:-0x33f0404}
DEBUG_SIZE=${DEBUG_SIZE:-10}
if [ `grep processor /proc/cpuinfo | wc -l` -gt 5 ]; then
    DEBUG_SIZE=$((`grep processor /proc/cpuinfo | wc -l` * 2))   # promise 2MB for every cpu
fi
SUBSYSTEM=${SUBSYSTEM:- 0xffb7e3ff}

ENABLE_QUOTA=${ENABLE_QUOTA:-""}
QUOTA_TYPE="ug3"
QUOTA_USERS=${QUOTA_USERS:-"quota_usr quota_2usr sanityusr sanityusr1"}
LQUOTAOPTS=${LQUOTAOPTS:-"hash_lqs_cur_bits=3"}

MKFSOPT=""
[ "x$MDSJOURNALSIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -J size=$MDSJOURNALSIZE"
[ "x$MDSISIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -i $MDSISIZE"
[ "x$MKFSOPT" != "x" ] &&
    MKFSOPT="--mkfsoptions=\\\"$MKFSOPT\\\""
[ "x$SECLEVEL" != "x" ] &&
    MKFSOPT=$MKFSOPT" --param mdt.sec_level=$SECLEVEL"
[ "x$MDSCAPA" != "x" ] &&
    MKFSOPT=$MKFSOPT" --param mdt.capa=$MDSCAPA"
[ "$MDSFSTYPE" = "ldiskfs" ] &&
    MDSOPT=$MDSOPT" --mountfsoptions=errors=remount-ro,user_xattr,acl"
[ "x$mdsfailover_HOST" != "x" ] &&
    MDSOPT=$MDSOPT" --failnode=`h2$NETTYPE $mdsfailover_HOST`"
[ "x$STRIPE_BYTES" != "x" ] &&
    MDSOPT=$MDSOPT" --param lov.stripesize=$STRIPE_BYTES"
[ "x$STRIPES_PER_OBJ" != "x" ] &&
    MDSOPT=$MDSOPT" --param lov.stripecount=$STRIPES_PER_OBJ"
[ "x$L_GETIDENTITY" != "x" ] &&
    MDSOPT=$MDSOPT" --param mdt.identity_upcall=$L_GETIDENTITY"

MDS_MKFS_OPTS="--mdt --fsname=$FSNAME $MKFSOPT $MDSOPT"
[ "$MDSFSTYPE" = "ldiskfs" ] &&
    MDS_MKFS_OPTS=$MDS_MKFS_OPTS" --device-size=$MDSSIZE"
[ "$MDSFSTYPE" = "zfs" ] &&
    MDS_MKFS_OPTS=$MDS_MKFS_OPTS" --vdev-size=$MDSSIZE"

if combined_mgs_mds ; then
    MDS_MKFS_OPTS="--mgs $MDS_MKFS_OPTS"
else
    MDS_MKFS_OPTS="--mgsnode=$MGSNID $MDS_MKFS_OPTS"
    [ "$MGSFSTYPE" = "ldiskfs" ] &&
        MGS_MKFS_OPTS="--mgs --device-size=$MGSSIZE"
    [ "$MGSFSTYPE" = "zfs" ] &&
        MGS_MKFS_OPTS="--mgs --vdev-size=$MGSSIZE"
fi

MKFSOPT=""
[ "x$OSTJOURNALSIZE" != "x" ] &&
    MKFSOPT=$MKFSOPT" -J size=$OSTJOURNALSIZE"
[ "x$MKFSOPT" != "x" ] &&
    MKFSOPT="--mkfsoptions=\\\"$MKFSOPT\\\""
[ "x$SECLEVEL" != "x" ] &&
    MKFSOPT=$MKFSOPT" --param ost.sec_level=$SECLEVEL"
[ "x$OSSCAPA" != "x" ] &&
    MKFSOPT=$MKFSOPT" --param ost.capa=$OSSCAPA"
[ "x$ostfailover_HOST" != "x" ] &&
    OSTOPT=$OSTOPT" --failnode=`h2$NETTYPE $ostfailover_HOST`"

OST_MKFS_OPTS="--ost --fsname=$FSNAME --mgsnode=$MGSNID $MKFSOPT $OSTOPT"
[ "$OSTFSTYPE" = "ldiskfs" ] &&
    OST_MKFS_OPTS=$OST_MKFS_OPTS" --device-size=$OSTSIZE"
[ "$OSTFSTYPE" = "zfs" ] &&
    OST_MKFS_OPTS=$OST_MKFS_OPTS" --vdev-size=$OSTSIZE"

if [ "$OSTFSTYPE" == "ldiskfs" ]; then
OST_MOUNT_OPTS=${OST_MOUNT_OPTS:-"-o loop,user_xattr"}
fi

if [ "$MDSFSTYPE" == "ldiskfs" ]; then
MDS_MOUNT_OPTS=${MDS_MOUNT_OPTS:-"-o loop,user_xattr"}
fi

if [ "MDSDEV1" != "MGSDEV" ]; then
    if [ "$MGSFSTYPE" == "ldiskfs" ]; then
        MGS_MOUNT_OPTS=${MGS_MOUNT_OPTS:-"-o loop"}
    else
        MGS_MOUNT_OPTS=${MGS_MOUNT_OPTS:-""}
    fi
else
    MGS_MOUNT_OPTS=${MGS_MOUNT_OPTS:-$MDS_MOUNT_OPTS}
fi

#client
MOUNT=${MOUNT:-/mnt/${FSNAME}}
MOUNT1=${MOUNT1:-$MOUNT}
MOUNT2=${MOUNT2:-${MOUNT}2}
MOUNTOPT=${MOUNTOPT:-"-o user_xattr,acl,flock"}
DIR=${DIR:-$MOUNT}
DIR1=${DIR:-$MOUNT1}
DIR2=${DIR2:-$MOUNT2}

if [ $UID -ne 0 ]; then
        log "running as non-root uid $UID"
        RUNAS_ID="$UID"
        RUNAS_GID=`id -g $USER`
        RUNAS=""
else
        RUNAS_ID=${RUNAS_ID:-500}
        RUNAS_GID=${RUNAS_GID:-$RUNAS_ID}
        RUNAS=${RUNAS:-"runas -u $RUNAS_ID -g $RUNAS_GID"}
fi

PDSH=${PDSH:-no_dsh}
FAILURE_MODE=${FAILURE_MODE:-SOFT} # or HARD
POWER_DOWN=${POWER_DOWN:-"powerman --off"}
POWER_UP=${POWER_UP:-"powerman --on"}
SLOW=${SLOW:-no}
FAIL_ON_ERROR=${FAIL_ON_ERROR:-true}

MPIRUN=$(which mpirun 2>/dev/null) || true
MPI_USER=${MPI_USER:-mpiuser}
SHARED_DIR_LOGS=${SHARED_DIR_LOGS:-""}

