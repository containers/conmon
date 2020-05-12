#!/bin/bash

set -e
source $(dirname $0)/lib.sh

req_env_var "
SRC $SRC
CRIO_SRC $CRIO_SRC
OS_RELEASE_ID $OS_RELEASE_ID
OS_RELEASE_VER $OS_RELEASE_VER
"

# Assume cri-o and all dependencies are installed from packages
# and conmon installed using build_and_replace_conmon()
export CRIO_BINARY=/usr/bin/crio
export CONMON_BINARY=/usr/libexec/crio/conmon
export PAUSE_BINARY=/usr/libexec/crio/pause
export CRIO_CNI_PLUGIN=/usr/libexec/cni

echo "Executing cri-o integration tests (typical 10 - 20 min)"
cd "$CRIO_SRC"
timeout --foreground --kill-after=5m 60m ./test/test_runner.sh
