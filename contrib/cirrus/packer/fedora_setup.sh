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

echo "Updating packages"
ooe.sh sudo dnf update -y

ooe.sh sudo dnf -y module install cri-o:1.13

echo "Installing dependencies"

ooe.sh sudo dnf -y install \
    atomic-registries \
    btrfs-progs-devel \
    buildah \
    container-selinux \
    containernetworking-plugins \
    cri-o \
    cri-tools \
    curl \
    device-mapper-devel \
    e2fsprogs-devel \
    findutils \
    gcc \
    git \
    glib2-devel \
    glibc-devel \
    glibc-static \
    golang \
    gpgme \
    gpgme-devel \
    grubby \
    hostname \
    iproute \
    iptables \
    krb5-workstation \
    kubernetes \
    libassuan \
    libassuan-devel \
    libblkid-devel \
    libffi-devel \
    libgpg-error-devel \
    libguestfs-tools \
    libseccomp-devel \
    libselinux-devel \
    libselinux-python \
    libsemanage-python \
    libvirt-client \
    libvirt-python \
    libxml2-devel \
    libxslt-devel \
    make \
    mlocate \
    nfs-utils \
    nmap-ncat \
    oci-register-machine \
    oci-systemd-hook \
    oci-umount \
    openssl \
    openssl-devel \
    ostree-devel \
    pkgconfig \
    podman \
    policycoreutils \
    python2-devel \
    python3-PyYAML \
    redhat-rpm-config \
    rpcbind \
    rsync \
    runc \
    sed \
    skopeo \
    socat \
    tar \
    vim \
    wget \
    zlib-devel

setup_gopath

install_crio_repo  # git-repo for test-content

match_crio_tag  # git repo to cri-o rpm version

echo "Enabling localnet routing"
echo "net.ipv4.conf.all.route_localnet = 1" | sudo tee /etc/sysctl.d/route_localnet.conf

echo "Enabling container management of cgroups"
ooe.sh sudo setsebool -P container_manage_cgroup 1

rh_finalize # N/B: Halts system!

echo "SUCCESS!"
