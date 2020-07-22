

# Library of common, shared utility functions.  This file is intended
# to be sourced by other scripts, not called directly.

# Under some contexts these values are not set, make sure they are.
export USER="$(whoami)"
export HOME="$(getent passwd $USER | cut -d : -f 6)"


# GCE image-name compatible string representation of distribution name
OS_RELEASE_ID="$(source /etc/os-release; echo $ID)"
# GCE image-name compatible string representation of distribution _major_ version
OS_RELEASE_VER="$(source /etc/os-release; echo $VERSION_ID | cut -d '.' -f 1)"
# Combined to ease soe usage
OS_REL_VER="${OS_RELEASE_ID}-${OS_RELEASE_VER}"
# Type of filesystem used for cgroups
CG_FS_TYPE="$(stat -f -c %T /sys/fs/cgroup)"

# Downloaded, but not installed packages.
PACKAGE_DOWNLOAD_DIR=/var/cache/download

# These are normally set by cirrus, but can't be for VMs setup by hack/get_ci_vm.sh
CRIO_REPO="${CRIO_REPO:-https://github.com/cri-o/cri-o.git}"
CRIO_SLUG="${CRIO_SLUG:-github.com/cri-o/cri-o}"
CRIO_SRC="${CRIO_SRC:-$GOPATH/src/${CRIO_SLUG}}"

CONMON_SLUG="${CONMON_SLUG:-github.com/containers/conmon}"

# Default to values from .cirrus.yml, otherwise guarantee these are always be set.
GOPATH="${GOPATH:-/var/tmp/go}"
CIRRUS_WORKING_DIR="${CIRRUS_WORKING_DIR:-$GOPATH/src/$CONMON_SLUG}"
GOSRC="$CIRRUS_WORKING_DIR"
GO111MODULE=on
SCRIPT_BASE="${SCRIPT_BASE:-./contrib/cirrus}"

# Normally supplied by cirrus, needed here when running things manually
CIRRUS_REPO_NAME=${CIRRUS_REPO_NAME-$(dirname $0)}
CIRRUS_BUILD_ID=${CIRRUS_BUILD_ID:-DEADBEEF}  # a human
CIRRUS_BASE_SHA=${CIRRUS_BASE_SHA:-HEAD}
CIRRUS_CHANGE_IN_REPO=${CIRRUS_CHANGE_IN_REPO:-FETCH_HEAD}

# From github.com/containers/automation
INSTALL_AUTOMATION_VERSION=1.1.3

# required for go 1.12+
export GOCACHE="${GOCACHE:-$HOME/.cache/go-build}"

if ! [[ "$PATH" =~ "/usr/local/bin" ]]
then
    export PATH="$PATH:/usr/local/bin:$GOPATH/bin"
fi

# Ensure go variables pass through 'make' command
if [[ -x "$(type -P go)" ]]; then
    eval "export $(go env)"
fi

# Pass in a line delimited list of, space delimited name/value pairs
# exit non-zero with helpful error message if any value is empty
req_env_var() {
    echo "$1" | while read NAME VALUE
    do
        if [[ -n "$NAME" ]] && [[ -z "$VALUE" ]]
        then
            echo "Required env. var. \$$NAME is not set"
            exit 9
        fi
    done
}

# Some env. vars may contain secrets.  Display values for known "safe"
# and useful variables.
# ref: https://cirrus-ci.org/guide/writing-tasks/#environment-variables
show_env_vars() {
    # This is almost always multi-line, print it separately
    echo "export CIRRUS_CHANGE_MESSAGE=$CIRRUS_CHANGE_MESSAGE"
    echo "
BUILDTAGS $BUILDTAGS
BUILT_IMAGE_SUFFIX $BUILT_IMAGE_SUFFIX
CI $CI
CIRRUS_CI $CIRRUS_CI
CI_NODE_INDEX $CI_NODE_INDEX
CI_NODE_TOTAL $CI_NODE_TOTAL
CONTINUOUS_INTEGRATION $CONTINUOUS_INTEGRATION
CIRRUS_BASE_BRANCH $CIRRUS_BASE_BRANCH
CIRRUS_BASE_SHA $CIRRUS_BASE_SHA
CIRRUS_BRANCH $CIRRUS_BRANCH
CIRRUS_BUILD_ID $CIRRUS_BUILD_ID
CIRRUS_CHANGE_IN_REPO $CIRRUS_CHANGE_IN_REPO
CIRRUS_CLONE_DEPTH $CIRRUS_CLONE_DEPTH
CIRRUS_DEFAULT_BRANCH $CIRRUS_DEFAULT_BRANCH
CIRRUS_PR $CIRRUS_PR
CIRRUS_TAG $CIRRUS_TAG
CIRRUS_OS $CIRRUS_OS
OS $OS
CIRRUS_TASK_NAME $CIRRUS_TASK_NAME
CIRRUS_TASK_ID $CIRRUS_TASK_ID
CIRRUS_REPO_NAME $CIRRUS_REPO_NAME
CIRRUS_REPO_OWNER $CIRRUS_REPO_OWNER
CIRRUS_REPO_FULL_NAME $CIRRUS_REPO_FULL_NAME
CIRRUS_REPO_CLONE_URL $CIRRUS_REPO_CLONE_URL
CIRRUS_SHELL $CIRRUS_SHELL
CIRRUS_USER_COLLABORATOR $CIRRUS_USER_COLLABORATOR
CIRRUS_USER_PERMISSION $CIRRUS_USER_PERMISSION
CIRRUS_WORKING_DIR $CIRRUS_WORKING_DIR
CIRRUS_HTTP_CACHE_HOST $CIRRUS_HTTP_CACHE_HOST
$(go env)
    " | while read NAME VALUE
    do
        [[ -z "$NAME" ]] || echo "export $NAME=\"$VALUE\""
    done
}

bad_os_id_ver() {
    echo "Unknown/Unsupported distro. '$OS_RELEASE_ID' and/or version '$OS_RELEASE_VER' $@"
    exit 42
}

stub() {
    echo "STUB: Pretending to do $1"
}

_finalize(){
    set +e  # Don't fail at the very end
    set +e  # make errors non-fatal
    echo "Removing leftover giblets from cloud-init"
    cd /
    sudo rm -rf /var/lib/cloud/instance?
    sudo rm -rf /root/.ssh/*
    sudo rm -rf /home/*
    sudo rm -rf /tmp/*
    sudo rm -rf /tmp/.??*
    sync
    sudo fstrim -av
}

rh_finalize(){
    set +e  # Don't fail at the very end
    # Allow root ssh-logins
    if [[ -r /etc/cloud/cloud.cfg ]]
    then
        sudo sed -re 's/^disable_root:.*/disable_root: 0/g' -i /etc/cloud/cloud.cfg
    fi
    echo "Resetting to fresh-state for usage as cloud-image."
    PKG=$(type -P dnf || type -P yum || echo "")
    [[ -z "$PKG" ]] || sudo $PKG clean all  # not on atomic
    sudo rm -rf /var/cache/{yum,dnf}
    sudo rm -f /etc/udev/rules.d/*-persistent-*.rules
    sudo touch /.unconfigured  # force firstboot to run
    _finalize
}

install_crio_repo() {
    req_env_var "
        GOPATH $GOPATH
        CRIO_SRC $CRIO_SRC
        CRIO_REPO $CRIO_REPO
    "
    echo "Cloning current CRI-O Source"
    cd $GOPATH/src
    rm -rf "$CRIO_SRC"  # just in case
    mkdir -p "$CRIO_SRC"
    ooe.sh git clone $CRIO_REPO $CRIO_SRC

    echo "Installing helper script for CNI plugin test"
    cd "$CRIO_SRC"
    mkdir -p /opt/cni/bin/
    # Search path for helper is difficult to control
    ooe.sh install -D -m 0755 test/cni_plugin_helper.bash /usr/libexec/cni/
    # Helper hard-codes cni binary path :(
    ooe.sh ln -fv /usr/libexec/cni/* /opt/cni/bin/

    echo "Installing registry configuration"
    mkdir -p /etc/containers/registries.d/
    ooe.sh install -D -m 0644 test/redhat_sigstore.yaml \
        /etc/containers/registries.d/registry.access.redhat.com.yaml

    echo "Building/installing CRI-O"
    ooe.sh make PREFIX=/usr
}

install_testing_deps() {
    req_env_var "
        GOPATH $GOPATH
        CRIO_SRC $CRIO_SRC
    "

    cd $CRIO_SRC
    go mod vendor

    echo "Installing required go packages into \$GOPATH"
    for toolpath in \
        tools/godep \
        onsi/ginkgo \
        onsi/gomega \
        cloudflare/cfssl/cmd/... \
        jteeuwen/go-bindata/go-bindata \
        cpuguy83/go-md2man \
        urfave/cli \
        containers/image/storage
    do
        go get -d "github.com/$toolpath"
    done

    echo "Installing Ginkgo and Gomega"
    if [[ ! -x "$GOBIN/ginkgo" ]]; then \
        ooe.sh go build -i -o ${GOPATH}/bin/ginkgo github.com/onsi/ginkgo
        ooe.sh go build -i -o ${GOPATH}/bin/ginkgo github.com/onsi/gomega
    fi
}

# Needed for e2e tests
selinux_permissive(){
    echo "Entering SELinux Permissive mode, will switch to enforcing upon shell exit"
    trap "setenforce 1" EXIT
    setenforce 0
}

build_and_replace_conmon() {
    req_env_var "
        SRC $SRC
    "

    echo "Renaming conmon binaries from RPMs"
    rename_all_found_binaries "conmon"

    echo "Building conmon"
    cd $SRC

    ooe.sh make
    echo "Installing conmon"
    ooe.sh sudo make crio PREFIX=/usr
    # Use same version for podman in case ever needed
    ooe.sh sudo ln -fv /usr/libexec/crio/conmon /usr/libexec/podman/conmon
    ooe.sh sudo restorecon -R /usr/bin
}

build_and_replace_bats() {
    req_env_var "
        SRC $SRC
    "
    rename_all_found_binaries "bats"

    git clone https://github.com/bats-core/bats-core
    pushd bats-core
    # must be at least v1.2.0 to have --jobs
    git checkout v1.2.0
    sudo ./install.sh /usr/local
    popd
    rm -rf bats-core
    mkdir -p ~/.parallel
    touch ~/.parallel/will-cite
}

rename_all_found_binaries() {
    req_env_var "
        1 $1
    "
    filename=$1
    NEWNAME=".original_packaged_${filename}"
    find /usr -type f -name ${filename} | \
    while IFS='' read FILEPATH
    do
        NEWPATH="$(dirname $FILEPATH)/$NEWNAME"
        [[ -r "$NEWPATH" ]] || sudo mv -v "$FILEPATH" "$NEWPATH"
    done
}
