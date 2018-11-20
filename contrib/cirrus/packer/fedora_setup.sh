#!/bin/bash

# This script is called by packer on the subject fedora VM, to setup the conmon
# build/test environment.  It's not intended to be used outside of this context.

set -e

# Load in library (copied by packer, before this script was run)
source $SRC/$SCRIPT_BASE/lib.sh

req_env_var "
    SRC $SRC
    SCRIPT_BASE $SCRIPT_BASE
    PACKER_BASE $PACKER_BASE
    BUILT_IMAGE_SUFFIX $BUILT_IMAGE_SUFFIX
    CRIO_REPO $CRIO_REPO
    CRIO_SLUG $CRIO_SLUG
"

install_ooe

ooe.sh sudo dnf update -y

stub "Installing/configuring Fedora"

rh_finalize # N/B: Halts system!

echo "SUCCESS!"
