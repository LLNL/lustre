cleanup() {

    true
}

error() {
    local msg="$1"

    if [ -n "$msg" ]; then
        echo -e "\n${0##*/}: $msg" >&$STDOUT
    fi
}

fatal() {
    cleanup
    error "$2"
    exit $1
}

#
# in a given directory, find the first rpm matching given requirements
#
find_rpm() {
    local dir="$1"
    local match_type="$2"
    local match="$3"

    pushd "$dir" > /dev/null || \
        fatal 1 "Unable to chdir to directory \"$dir\" in find_rpm()"

    local file
    for file in $(ls *.rpm); do
        if [ ! -f "$file" ]; then
            continue
        fi
        case "$match_type" in
            provides)
                # match is any valid ERE (i.e. given to egrep) match
                if rpm -q --provides -p "$file" 2>&$STDOUT | egrep "$match" >&$STDOUT; then
                    echo "$file"
                    popd >/dev/null
                    return 0
                fi
                ;;
            *)
                popd >/dev/null
                fatal 1 "Unknown match type \"$match_type\" given to find_rpm()"
                ;;
        esac
    done

    popd >/dev/null
    return 1
}

find_linux_rpms() {
    local prefix="$1"
    local pathtorpms=${2:-"$KERNELRPMSBASE/$lnxmaj/$DISTROMAJ/$TARGET_ARCH"}

    local wanted_kernel="${lnxmaj}${lnxmin}-${lnxrel}${rpmfix}"
    local kernel_rpms=$(find_linux_rpm "$prefix" "$pathtorpms")
    # call a distro specific hook, if available
    if type -p find_linux_rpms-$DISTROMAJ; then
        local rpm
        if rpm=$(find_linux_rpms-$DISTROMAJ "$prefix" "$wanted_kernel" "$pathtorpms"); then
            kernel_rpms="$kernel_rpms $rpm"
        else
            return 255
        fi
    fi

    echo "$kernel_rpms"
    return 0

}

# a noop function which can be overridden by a distro method implementation
resolve_arch() {
    local arch="$1"

    echo "$arch"
}

# XXX this needs to be re-written as a wrapper around find_rpm
#     or just gotten rid of.  :-)
find_linux_rpm() {
    local prefix="$1"
    local pathtorpms=${2:-"$KERNELRPMSBASE/$lnxmaj/$DISTROMAJ/$TARGET_ARCH"}

    local found_rpm=""
    local wanted_kernel="${lnxmaj}${lnxmin}-${lnxrel}${rpmfix}"
    local ret=1
    if [ -d "$pathtorpms" ]; then
        local rpm
        for rpm in $(ls ${pathtorpms}/*.$(resolve_arch $TARGET_ARCH $PATCHLESS).rpm); do
            if rpm -q --provides -p "$rpm" 2>&$STDOUT | grep -q "kernel${prefix} = $wanted_kernel" 2>&$STDOUT; then
                found_rpm="$rpm"
                ret=0
                break
            fi
        done
    else
        mkdir -p "$pathtorpms"
    fi
    # see above "XXX"
    #     [ -f "$found_rpm" ] && break
    # done
    if [ -z "$found_rpm" ]; then
        # see if there is a distro specific way of getting the RPM
        if type -p find_linux_rpm-$DISTROMAJ; then
            if found_rpm=$(find_linux_rpm-$DISTROMAJ "$prefix" "$wanted_kernel" "$pathtorpms"); then
                found_rpm="${pathtorpms}/$found_rpm"
                ret=0
            else
                ret=${PIPESTATUS[0]}
            fi
        fi
    fi

    echo "$found_rpm"
    return $ret

}

# autodetect used Distro
autodetect_distro() {

    local name
    local version

    if which lsb_release >/dev/null 2>&1; then
        name="$(lsb_release -s -i)"
        version="$(lsb_release -s -r)"
        case "$name" in
            "EnterpriseEnterpriseServer")
                name="oel"
                ;;
            "RedHatEnterpriseServer" | "ScientificSL" | "CentOS")
                name="rhel"
                ;;
	    "SUSE LINUX" | "SUSE")
		name="sles"
		case "$version" in
		*.*)	# $version already has patchlevel
			;;
		*)	# add patchlevel
			PATCHLEVEL=$(sed -n -e 's/^PATCHLEVEL = //p' /etc/SuSE-release)
			if [ "$PATCHLEVEL" -ne "0" ]; then
				version="${version}.$PATCHLEVEL"
			fi
			;;
		esac
		;;
            "Fedora")
                name="fc"
                ;;
            *)
                fatal 1 "I don't know what distro name $name and version $version is.\nEither update autodetect_distro() or use the --distro argument."
                ;;
        esac
    else
        error "You really ought to install lsb_release for accurate distro identification"
        # try some heuristics
        if [ -f /etc/SuSE-release ]; then
            name=sles
            version=$(sed -n -e 's/^VERSION = //p' /etc/SuSE-release)
	    PATCHLEVEL=$(sed -n -e 's/^PATCHLEVEL = //p' /etc/SuSE-release)
	    if [ "$PATCHLEVEL" -ne "0" ]; then
		    version="${version}.$PATCHLEVEL"
	    fi
        elif [ -f /etc/redhat-release ]; then
		#name=$(head -1 /etc/redhat-release)
		name=rhel
		version=$(cat /etc/redhat-release |
			  sed -e 's/^[^0-9.]*//g' | sed -e 's/[ ].*//')
        fi
        if [ -z "$name" -o -z "$version" ]; then
            fatal 1 "I don't know how to determine distro type/version.\nEither update autodetect_distro() or use the --distro argument."
        fi
    fi

    echo ${name}-${version}
    return 0

}

# autodetect target
autodetect_target() {
    local distro="$1"

    local target=""
    case ${distro} in
         rhel7*)  target="3.10-rhel7";;
         rhel8*)  target="4.18-rhel8";;
	rhel-9.0) target="5.14-rhel9.0";;
	rhel-9.1) target="5.14-rhel9.1";;
	rhel-9.2) target="5.14-rhel9.2";;
	rhel-9.3) target="5.14-rhel9.3";;
        sles11.4) target="$(uname -r | cut -d . -f 1,2)-sles11sp4";;
        sles11.3) target="$(uname -r | cut -d . -f 1,2)-sles11sp3";;
        sles11*)  target="$(uname -r | cut -d . -f 1,2)-sles11";;
        sles12.5) target="$(uname -r | cut -d . -f 1,2)-sles12sp5";;
        sles12.4) target="$(uname -r | cut -d . -f 1,2)-sles12sp4";;
        sles12.3) target="$(uname -r | cut -d . -f 1,2)-sles12sp3";;
        sles12*)  target="$(uname -r | cut -d . -f 1,2)-sles12";;
        sles15.1) target="$(uname -r | cut -d . -f 1,2)-sles15sp1";;
        sles15.2) target="$(uname -r | cut -d . -f 1,2)-sles15sp2";;
        sles15.3) target="$(uname -r | cut -d . -f 1,2)-sles15sp3";;
	sles15.4) target="$(uname -r | cut -d . -f 1,2)-sles15sp4";;
	sles15.5) target="$(uname -r | cut -d . -f 1,2)-sles15sp5";;
          fc18)   target="3.x-fc18";;
             *)   fatal 1 "I don't know what distro $distro is.\nEither update autodetect_target() or use the --target argument.";;
    esac

    echo ${target}
    return 0

}
