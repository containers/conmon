#!/bin/bash

set -e
source $(dirname $0)/lib.sh

cd $CIRRUS_WORKING_DIR
$SCRIPT_BASE/config.sh
make
make -f .rpmbuild/Makefile
rpmbuild --rebuild conmon-*.src.rpm
dnf -y install ~/rpmbuild/RPMS/x86_64/conmon*.x86_64.rpm
ls -l /usr/bin/conmon
