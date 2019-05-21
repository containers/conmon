

# Library of common, shared utility functions.  This file is intended
# to be sourced by other scripts, not called directly.

# Under some contexts these values are not set, make sure they are.
export USER="$(whoami)"
export HOME="$(getent passwd $USER | cut -d : -f 6)"

# These are normally set by cirrus, but can't be for VMs setup by hack/get_ci_vm.sh
# Pick some reasonable defaults
ENVLIB=${ENVLIB:-.bash_profile}
CIRRUS_WORKING_DIR="${CIRRUS_WORKING_DIR:-/var/tmp/go/src/github.com/containers/libpod}"
SCRIPT_BASE=${SCRIPT_BASE:-./contrib/cirrus}
PACKER_BASE=${PACKER_BASE:-./contrib/cirrus/packer}
CIRRUS_REPO_NAME=${CIRRUS_REPO_NAME-$(dirname $0)}
CIRRUS_BUILD_ID=${CIRRUS_BUILD_ID:-DEADBEEF}  # a human
CIRRUS_BASE_SHA=${CIRRUS_BASE_SHA:-HEAD}
CIRRUS_CHANGE_IN_REPO=${CIRRUS_CHANGE_IN_REPO:-FETCH_HEAD}

if ! [[ "$PATH" =~ "/usr/local/bin" ]]
then
    export PATH="$PATH:/usr/local/bin"
fi

# In ci/testing environment, ensure variables are always loaded
if [[ -r "$HOME/$ENVLIB" ]] && [[ -n "$CI" ]]
then
    # Make sure this is always loaded
    source "$HOME/$ENVLIB"
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
PACKER_BUILDS $PACKER_BUILDS
    " | while read NAME VALUE
    do
        [[ -z "$NAME" ]] || echo "export $NAME=\"$VALUE\""
    done
}

# Return a GCE image-name compatible string representation of distribution name
os_release_id() {
    eval "$(egrep -m 1 '^ID=' /etc/os-release | tr -d \' | tr -d \")"
    echo "$ID"
}

# Return a GCE image-name compatible string representation of distribution major version
os_release_ver() {
    eval "$(egrep -m 1 '^VERSION_ID=' /etc/os-release | tr -d \' | tr -d \")"
    echo "$VERSION_ID" | cut -d '.' -f 1
}

bad_os_id_ver() {
    echo "Unknown/Unsupported distro. $OS_RELEASE_ID and/or version $OS_RELEASE_VER for $ARGS"
    exit 42
}

stub() {
    echo "STUB: Pretending to do $1"
}

# Helper/wrapper script to only show stderr/stdout on non-zero exit
install_ooe() {
    req_env_var "
        SRC $SRC
        SCRIPT_BASE $SCRIPT_BASE
    "
    echo "Installing script to mask stdout/stderr unless non-zero exit."
    sudo install -D -m 755 "$SRC/$SCRIPT_BASE/ooe.sh" /usr/local/bin/ooe.sh
}

# Grab a newer version of git from software collections
# https://www.softwarecollections.org/en/
# and use it with a wrapper
install_scl_git() {
    echo "Installing SoftwareCollections updated 'git' version."
    ooe.sh sudo yum -y install rh-git29
    cat << "EOF" | sudo tee /usr/bin/git
#!/bin/bash

scl enable rh-git29 -- git $@
EOF
    sudo chmod 755 /usr/bin/git
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

rhel_exit_handler() {
    set +ex
    req_env_var "
        RHSMCMD $RHSMCMD
    "
    cd /
    sudo rm -rf "$RHSMCMD"
    sudo subscription-manager unsubscribe --all
    sudo subscription-manager remove --all
    sudo subscription-manager unregister
    sudo subscription-manager clean
}

rhsm_enable() {
    req_env_var "
        RHSM_COMMAND $RHSM_COMMAND
    "
    export RHSMCMD="$(mktemp)"
    trap "rhel_exit_handler" EXIT
    # Avoid logging sensitive details
    echo "$RHSM_COMMAND" > "$RHSMCMD"
    ooe.sh sudo bash "$RHSMCMD"
    sudo rm -rf "$RHSMCMD"
}

setup_gopath() {
    req_env_var "
        CRIO_REPO $CRIO_REPO
        CRIO_SLUG $CRIO_SLUG
    "
    echo "Configuring persistent Go environment for all users"
    sudo mkdir -p /var/tmp/go/src  # Works with atomic
    sudo chown -R $USER:$USER /var/tmp/go
    sudo chmod g=rws /var/tmp/go
    ENVLIB=/etc/profile.d/go.sh
    if ! grep -q GOPATH $ENVLIB
    then
        sudo tee "$ENVLIB" << EOF
export GOPATH=/var/tmp/go
export GOSRC=\$GOPATH/src/$CRIO_SLUG
export PATH=\$PATH:\$GOPATH/bin
EOF
    source $ENVLIB
    fi
}

install_crio_repo() {
    req_env_var "
        GOPATH $GOPATH
        GOSRC $GOSRC
        CRIO_REPO $CRIO_REPO
    "
    echo "Cloning current CRI-O Source for faster access later"
    sudo rm -rf "$GOSRC"  # just in case
    ooe.sh git clone $CRIO_REPO $GOSRC
}

install_testing_deps() {
    req_env_var "
        GOPATH $GOPATH
        GOSRC $GOSRC
    "

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

    echo "Installing latest upstream version of BATS"
    ooe.sh git clone https://github.com/bats-core/bats-core.git /tmp/bats
    cd /tmp/bats
    ooe.sh ./install.sh /usr
    rm -rf /tmp/bats

    echo "Installing helper script for CNI plugin test"
    cd "$GOSRC"
    sudo mkdir -p /opt/cni/bin/
    # Search path for helper is difficult to control
    ooe.sh sudo install -D -m 0755 test/cni_plugin_helper.bash /usr/libexec/cni/
    # Helper hard-codes cni binary path :(
    ooe.sh sudo ln -fv /usr/libexec/cni/* /opt/cni/bin/

    echo "Installing registry configuration"
    sudo mkdir -p /etc/containers/registries.d/
    ooe.sh sudo install -D -m 0644 test/redhat_sigstore.yaml \
        /etc/containers/registries.d/registry.access.redhat.com.yaml
}

# Needed for e2e tests
selinux_permissive(){
    echo "Entering SELinux Permissive mode, will switch to enforcing upon shell exit"
    trap "setenforce 1" EXIT
    setenforce 0
}

match_crio_tag() {
    req_env_var "
        GOSRC $GOSRC
    "
    export CRIO_VER="$(rpm -q --qf '%{V}' cri-o)"
    echo "Checking out CRI-O tag v$CRIO_VER to match installed rpm $(rpm -q cri-o)"
    cd $GOSRC
    ooe.sh git remote update
    git checkout v$CRIO_VER | tail -1
}

build_and_replace_conmon() {
    req_env_var "
        SRC $SRC
    "

    NEWNAME=.original_packaged_conmon
    echo "Renaming conmon binaries from RPMs"
    find /usr/libexec -type f -name conmon |
    while read CONMON_FILEPATH
    do
        NEWPATH="$(dirname $CONMON_FILEPATH)/$NEWNAME"
        [[ -r "$NEWPATH" ]] || sudo mv -v "$CONMON_FILEPATH" "$NEWPATH"
    done

    echo "Building conmon"
    cd $SRC

    ooe.sh make
    echo "Installing conmon"
    ooe.sh sudo make install PREFIX=/usr
    # Use same version for podman in case ever needed
    ooe.sh sudo ln -fv /usr/libexec/crio/conmon /usr/libexec/podman/conmon
    ooe.sh sudo restorecon -R /usr/libexec
}
