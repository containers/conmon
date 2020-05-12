#!/bin/bash

set -e
source $(dirname $0)/lib.sh

req_env_var "
    USER $USER
    HOME $HOME
    SCRIPT_BASE $SCRIPT_BASE
    CIRRUS_REPO_NAME $CIRRUS_REPO_NAME
    CIRRUS_CHANGE_IN_REPO $CIRRUS_CHANGE_IN_REPO
    CIRRUS_WORKING_DIR $CIRRUS_WORKING_DIR
    PACKAGE_DOWNLOAD_DIR $PACKAGE_DOWNLOAD_DIR
"

[[ "$SHELL" =~ "bash" ]] || chsh -s /bin/bash

cd "$CIRRUS_WORKING_DIR"  # for clarity of initial conditions

# Verify basic dependencies
for depbin in gcc rsync sha256sum curl make
do
    if ! type -P "$depbin" &> /dev/null
    then
        echo "***** WARNING: $depbin binary not found in $PATH *****"
    fi
done

show_env_vars

install_crio_repo

[[ "$OS_RELEASE_ID" == "fedora" ]] || \
    bad_os_id_ver

# Fedora VM images are built with unused storage.  For now just soak it all up.
# Ref: https://github.com/containers/libpod/blob/d5358e676486bf894eadffd0b55c7e5d6a35867b/contrib/cirrus/packer/cloud-init/fedora/cloud.cfg.d/50_custom_disk_setup.cfg
/bin/growpart /dev/sda 1
resize2fs /dev/sda1

# These were downloaded at VM image build time (see containers/libpod repo)
echo "Installing Cri-o, Kubernetes, and dependencies in $PACKAGE_DOWNLOAD_DIR"
# Using dnf would try to contact repositories for new versions.  Only ever
# use the pre-downloaded versions to provide predictabile/reliable behaviors.

# we force here because the cri-o rpm is overwriting a conmon that came with the vm.
# we will eventually overwrite the one that comes with the CRI-O pacakge in the call
# to build_and_replace_conmon below
ooe.sh rpm --force -ivh $PACKAGE_DOWNLOAD_DIR/*.rpm

install_testing_deps
build_and_replace_conmon
build_and_replace_bats

cd "$CRIO_SRC"  # cri-o source
echo "Building CRI-O binaries required for testing"
ooe.sh make vendor
ooe.sh make test-binaries

echo "Configuring firewall/networking for integration tests"
ooe.sh iptables -F
ooe.sh iptables -t nat -I POSTROUTING -s 127.0.0.1 ! -d 127.0.0.1 -j MASQUERADE
echo "Setting read_only flag to false"
sed -i 's/read_only = true/read_only = false/g' /etc/crio/crio.conf
echo "Removing nodev flag"
sed -i 's/nodev//g' /etc/containers/storage.conf
iptables -L -n -v

# Verify nothing was set empty
req_env_var "
    OS_RELEASE_ID $OS_RELEASE_ID
    OS_RELEASE_VER $OS_RELEASE_VER
    OS_REL_VER $OS_REL_VER
    BUILT_IMAGE_SUFFIX $BUILT_IMAGE_SUFFIX
"

echo "***** TESTING STARTS: $(date --iso-8601=seconds)"
