#!/bin/bash

# This script is called by packer on the subject CentOS VM, to setup the conmon
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

rhsm_enable

echo "Enabling OpenShift 3.11"
ooe.sh sudo subscription-manager refresh
ooe.sh sudo subscription-manager attach --pool=8a85f98960dbf6510160df23e3367451

echo "Isolating repositories"

ooe.sh sudo subscription-manager repos "--disable=*"
ooe.sh sudo subscription-manager repos \
    --enable=rhel-7-server-rpms \
    --enable=rhel-7-server-optional-rpms \
    --enable=rhel-7-server-extras-rpms \
    --enable=rhel-server-rhscl-7-rpms \
    --enable=rhel-7-server-ose-3.11-rpms

echo "Updating packages"

ooe.sh sudo yum -y update

echo "Installing dependencies"

ooe.sh sudo yum -y install \
    PyYAML \
    atomic-registries \
    buildah \
    container-selinux \
    containernetworking-plugins \
    cri-o \
    cri-tools \
    curl \
    device-mapper-devel \
    e2fsprogs-devel \
    expect \
    findutils \
    gcc \
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
    python \
    python-devel \
    python-rhsm-certificates \
    python-virtualenv \
    python2-crypto \
    python34 \
    python34-PyYAML \
    redhat-rpm-config \
    rpcbind \
    rsync \
    runc \
    sed \
    skopeo-containeras \
    socat \
    tar \
    vim \
    wget \
    zlib-devel

setup_gopath

install_scl_git

install_crio_repo  # git-repo for test-content

match_crio_tag  # git repo to cri-o rpm version

# Include quota support kernel command line option
echo "Adding rootflags=pquota kernel argument"
ooe.sh sudo grubby --update-kernel=ALL --args="rootflags=pquota"

echo "Enabling localnet routing"
echo "net.ipv4.conf.all.route_localnet = 1" | sudo tee /etc/sysctl.d/route_localnet.conf

echo "Enabling container management of cgroups"
ooe.sh sudo setsebool -P container_manage_cgroup 1

rhel_exit_handler

rh_finalize

echo "SUCCESS!"
