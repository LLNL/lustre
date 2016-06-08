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
            "SUSE LINUX")
                name="sles"
		PATCHLEVEL=$(sed -n -e 's/^PATCHLEVEL = //p' /etc/SuSE-release)
		if [ "$PATCHLEVEL" -ne "0" ]; then
			version="${version}.$PATCHLEVEL"
		fi
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
            version=$(echo "$distroname" |
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
          oel5*)  target="2.6-oel5";;
         rhel5*)  target="2.6-rhel5";;
         rhel6.8) target="2.6-rhel6.8";;
         rhel6.7) target="2.6-rhel6.7";;
         rhel6.6) target="2.6-rhel6.6";;
         rhel6*)  target="2.6-rhel6";;
         rhel7*)  target="3.10-rhel7";;
        sles10*)  target="2.6-sles10";;
        sles11.4) target="$(uname -r | cut -d . -f 1,2)-sles11sp4";;
        sles11.3) target="$(uname -r | cut -d . -f 1,2)-sles11sp3";;
        sles11*)  target="$(uname -r | cut -d . -f 1,2)-sles11";;
	sles12*)  target="$(uname -r | cut -d . -f 1,2)-sles12";;
          fc15)   target="2.6-fc15";;
          fc18)   target="3.x-fc18";;
             *)   fatal 1 "I don't know what distro $distro is.\nEither update autodetect_target() or use the --target argument.";;
    esac

    echo ${target}
    return 0

}
