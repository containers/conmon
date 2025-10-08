FROM registry.access.redhat.com/ubi10/ubi:latest

RUN dnf install -y make automake gcc gcc-c++ glib2-devel pkg-config systemd-devel libseccomp-devel && \
    dnf clean all && \
    rm -rf /var/cache/dnf

# replaces the mktemp from the tutorial as everything is temporary in a
# container unless bind mounted out
RUN mkdir -p /tmp/gocache
ENV GOCACHE=/tmp/gocache

RUN mkdir -p /devenv
ADD . /devenv
WORKDIR /devenv

RUN make
