#!/bin/bash

set -e
source $(dirname $0)/lib.sh

req_env_vars USER HOME ENVLIB SCRIPT_BASE CIRRUS_REPO_NAME CIRRUS_WORKING_DIR
req_env_vars GOPATH CRIO_SLUG

cd "$CIRRUS_WORKING_DIR"  # for clarity of initial conditions

# Setup env. vars common to all tasks/scripts/platforms and
# ensure they return for every following script execution.
MARK="# Added by $0, manual changes will be lost."
touch "$ENVLIB"
if ! grep -q "$MARK" "$ENVLIB"
then
    cat > "$ENVLIB" <<EOF
$MARK
SRC="$CIRRUS_WORKING_DIR"
CRIO_SRC="$GOPATH/src/$CRIO_SLUG"
$(go env)
PATH="$PATH:$GOPATH/bin:/usr/local/bin"
EOF
    source "$ENVLIB"

    show_env_vars

    if [[ ! "$OS_RELEASE_ID" =~ fedora ]]; then
        bad_os_id_ver
    fi
    install_crio_repo
    install_testing_deps
    build_and_replace_conmon

    msg "Setting read_only flag to false"
    sed -i 's/read_only = true/read_only = false/g' /etc/crio/crio.conf

    msg "Removing nodev flag"
    sed -i 's/nodev//g' /etc/containers/storage.conf

    msg "Configuring firewall/networking for integration tests"
    ooe.sh iptables -F
    ooe.sh iptables -t nat -I POSTROUTING -s 127.0.0.1 ! -d 127.0.0.1 -j MASQUERADE
    msg "Current IPTables:"
    msg "$(iptables -L -n -v)"
fi
