

# Library of common, shared utility functions.  This file is intended
# to be sourced by other scripts, not called directly.

# BEGIN Global export of all variables
set -a

# Due to differences across platforms and runtime execution environments,
# handling of the (otherwise) default shell setup is non-uniform.  Rather
# than attempt to workaround differences, simply force-load/set required
# items every time this library is utilized.
USER="$(whoami)"
HOME="$(getent passwd $USER | cut -d : -f 6)"
# Some platforms set and make this read-only
[[ -n "$UID" ]] || \
    UID=$(getent passwd $USER | cut -d : -f 3)

# Automation library installed at image-build time,
# defining $AUTOMATION_LIB_PATH in this file.
if [[ -r "/etc/automation_environment" ]]; then
    source /etc/automation_environment
fi
# shellcheck disable=SC2154
if [[ -n "$AUTOMATION_LIB_PATH" ]]; then
        # shellcheck source=/usr/share/automation/lib/common_lib.sh
        source $AUTOMATION_LIB_PATH/common_lib.sh
else
    (
    echo "WARNING: It does not appear that containers/automation was installed."
    echo "         Functionality of most of this library will be negatively impacted"
    echo "         This ${BASH_SOURCE[0]} was loaded by ${BASH_SOURCE[1]}"
    ) > /dev/stderr
fi

# Filepath containing CI-Automation wide shell env. vars.
ENVLIB=${ENVLIB:-/etc/ci_environment}
if [[ -r "$ENVLIB" ]]; then source "$ENVLIB"; fi

OS_RELEASE_ID="$(source /etc/os-release; echo $ID)"
# GCE image-name compatible string representation of distribution _major_ version
OS_RELEASE_VER="$(source /etc/os-release; echo $VERSION_ID | tr -d '.')"
# Combined to ease some usage
OS_REL_VER="${OS_RELEASE_ID}-${OS_RELEASE_VER}"

GOPATH=/var/tmp/go
CIRRUS_WORKING_DIR="${CIRRUS_WORKING_DIR:-$(realpath $(dirname ${BASH_SOURCE[0]})/../../)}"
SCRIPT_BASE=${SCRIPT_BASE:-./contrib/cirrus}
CIRRUS_REPO_NAME=${CIRRUS_REPO_NAME-$(dirname $0)}

# Cirrus only sets $CIRRUS_BASE_SHA properly for PRs, but $EPOCH_TEST_COMMIT
# needs to be set from this value in order for `make validate` to run properly.
# When running get_ci_vm.sh, most $CIRRUS_xyz variables are empty. Attempt
# to accomidate both branch and get_ci_vm.sh testing by discovering the base
# branch SHA value.
# shellcheck disable=SC2154
if [[ -z "$CIRRUS_BASE_SHA" ]] && [[ -z "$CIRRUS_TAG" ]]
then  # Operating on a branch, or under `get_ci_vm.sh`
    CIRRUS_BASE_SHA=$(git rev-parse ${UPSTREAM_REMOTE:-origin}/main)
elif [[ -z "$CIRRUS_BASE_SHA" ]]
then  # Operating on a tag
    CIRRUS_BASE_SHA=$(git rev-parse HEAD)
fi
# The starting place for linting and code validation
EPOCH_TEST_COMMIT="$CIRRUS_BASE_SHA"

CONMON_SLUG="${CONMON_SLUG:-github.com/containers/conmon}"
CRIO_REPO="${CRIO_REPO:-https://github.com/cri-o/cri-o.git}"
CRIO_SLUG="${CRIO_SLUG:-github.com/cri-o/cri-o}"

# END Global export of all variables
set +a

bad_os_id_ver() {
    die "Unknown/Unsupported distro. $OS_RELEASE_ID and/or version $OS_RELEASE_VER"
}

install_crio_repo() {
    req_env_vars GOPATH CRIO_SRC CRIO_REPO
    msg "Cloning current CRI-O Source"
    rm -rf "$CRIO_SRC"  # just in case
    ooe.sh git clone "$CRIO_REPO" "$CRIO_SRC"

    # Install CRI-O
    cd $CRIO_SRC
    ooe.sh make PREFIX=/usr
    ooe.sh make install PREFIX=/usr
}

install_testing_deps() {
    req_env_vars GOPATH CRIO_SRC

    msg "Installing required go packages into \$GOPATH"
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

    msg "Installing latest upstream version of BATS"
    ooe.sh git clone https://github.com/bats-core/bats-core.git /tmp/bats
    cd /tmp/bats
    ooe.sh ./install.sh /usr
    rm -rf /tmp/bats

    msg "Installing helper script for CNI plugin test"
    cd "$CRIO_SRC"
    mkdir -p /opt/cni/bin/
    # Search path for helper is difficult to control
    ooe.sh install -D -m 0755 test/cni_plugin_helper.bash /usr/libexec/cni/
    # Helper hard-codes cni binary path :(
    ooe.sh ln -fv /usr/libexec/cni/* /opt/cni/bin/

    msg "Installing registry configuration"
    mkdir -p /etc/containers/registries.d/
    ooe.sh install -D -m 0644 test/redhat_sigstore.yaml \
        /etc/containers/registries.d/registry.access.redhat.com.yaml
}

# Needed for e2e tests
selinux_permissive(){
    warn "Entering SELinux Permissive mode, will switch to enforcing upon shell exit"
    trap "setenforce 1" EXIT
    setenforce 0
}

build_and_replace_conmon() {
    req_env_vars SRC

    NEWNAME=.original_packaged_conmon
    msg "Renaming conmon binaries from RPMs"
    find /usr -type f -name conmon |
    while read CONMON_FILEPATH
    do
        NEWPATH="$(dirname $CONMON_FILEPATH)/$NEWNAME"
        [[ -r "$NEWPATH" ]] || mv -v "$CONMON_FILEPATH" "$NEWPATH"
    done

    msg "Building conmon"
    cd $SRC

    ooe.sh make
    msg "Installing conmon"
    ooe.sh make crio PREFIX=/usr
    # Use same version for podman in case ever needed
    ooe.sh ln -fv /usr/libexec/crio/conmon /usr/libexec/podman/conmon
    ooe.sh restorecon -R /usr/bin
}
