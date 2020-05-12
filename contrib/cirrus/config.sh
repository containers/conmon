#!/bin/bash

set -e
source $(dirname $0)/lib.sh

cd $CIRRUS_WORKING_DIR
go mod init $CONMON_SLUG
make config
