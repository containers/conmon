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

dnf install -y make glib2-devel git gcc golang
setup_gopath
cd $CIRRUS_WORKING_DIR
GOCACHE=/tmp/go-build make vendor
make
make install PREFIX=/usr # currently, the conmon location is hardcoded to /usr/bin/conmon
make test
