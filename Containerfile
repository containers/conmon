FROM registry.fedoraproject.org/fedora:latest

RUN sudo dnf install -y make automake gcc gcc-c++ kernel-devel glib2-devel && \
    sudo dnf clean all && \
    rm -rf /var/cache/dnf

RUN sudo dnf update -y && \
    sudo dnf clean all && \
    rm -rf /var/cache/dnf

# replaces the mktemp from the tutorial as everything is temporary in a
# container unless bind mounted out
RUN mkdir -p /tmp/gocache
ENV GOCACHE=/tmp/gocache

RUN mkdir -p /devenv
ADD . /devenv
WORKDIR /devenv

RUN make
