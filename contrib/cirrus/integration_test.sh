#!/bin/bash

set -e
source $(dirname $0)/lib.sh

req_env_var "
	CRIO_REPO $CRIO_REPO
	CRIO_SLUG $CRIO_SLUG
	CONMON_SLUG $CONMON_SLUG
	CIRRUS_WORKING_DIR $CIRRUS_WORKING_DIR
	GOPATH $GOPATH
"

cd $CIRRUS_WORKING_DIR
make
make install PREFIX=/usr # currently, the conmon location is hardcoded to /usr/bin/conmon
GOCACHE=/tmp/go-build make vendor
make test
