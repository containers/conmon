#!/bin/bash

set -e
source $(dirname $0)/lib.sh

cd $CIRRUS_WORKING_DIR
$SCRIPT_BASE/config.sh
make vendor
