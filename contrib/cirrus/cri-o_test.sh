#!/bin/bash

set -e
source $(dirname $0)/lib.sh

req_env_var SRC SCRIPT_BASE CRIO_SRC OS_RELEASE_ID OS_RELEASE_VER

cd "$CRIO_SRC"

if [[ ! "$OS_RELEASE_ID" =~ fedora ]]; then
    bad_os_id_ver
fi

msg "Building binaries required for testing"
ooe.sh make test-binaries

# TODO: Around the CRI-O 1.16 timeframe, the network tests
# would fail without this patch.  The same problem did not
# occur on RHEL/CentOS.  Countless hours were spent debugging
# but the root-cause was never found.  This is a workaround.
PATCH="$SRC/$SCRIPT_BASE/network_bats.patch"
cd "$CRIO_SRC"
warn "WARNING: Applying $PATCH"
git apply --index --apply --ignore-space-change --recount "$PATCH"

# Assume cri-o and all dependencies are installed from packages
# and conmon installed using build_and_replace_conmon()
export CRIO_BINARY=/usr/bin/crio
export CONMON_BINARY=/usr/libexec/crio/conmon
export PAUSE_BINARY=/usr/libexec/crio/pause
export CRIO_CNI_PLUGIN=/usr/libexec/cni

msg "Executing cri-o integration tests (typical 10 - 20 min)"
cd "$CRIO_SRC"
timeout --foreground --kill-after=5m 60m ./test/test_runner.sh
