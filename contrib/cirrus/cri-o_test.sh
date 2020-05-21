#!/bin/bash

set -e
source $(dirname $0)/lib.sh

req_env_var "
CRIO_SRC $CRIO_SRC
"

# Assume conmon is installed using build_and_replace_conmon()
export CONMON_BINARY=/usr/libexec/crio/conmon

echo "Executing cri-o integration tests (typical 10 - 20 min)"
cd "$CRIO_SRC"
timeout --foreground --kill-after=5m 60m sudo ./test/test_runner.sh
