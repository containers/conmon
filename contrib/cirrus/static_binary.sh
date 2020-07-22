#!/bin/bash

set -e
source $(dirname $0)/lib.sh

podman_run="podman run -i --rm --security-opt label=disable --privileged"
mkdir -p /var/tmp/nix

set -x
podman_run -v /var/tmp/nix:/mnt nixos/nix cp -rfT /nix /mnt
podman_run -v /var/tmp/nix:/nix \
    -v $CIRRUS_WORKING_DIR:$CIRRUS_WORKING_DIR \
    -w $CIRRUS_WORKING_DIR \
    nixos/nix \
    nix --print-build-logs --option cores 8 --option max-jobs 8 build --file nix/
