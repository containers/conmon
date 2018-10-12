#!/bin/bash

set -x

export GOPATH=/go

export GOSRC=$GOPATH/src/github.com/containers/libpod
export PATH=$HOME/gopath/bin:$PATH:$GOPATH/bin

git clone http://github.com/containers/libpod $GOSRC
cd $GOSRC

go get -u github.com/onsi/ginkgo/ginkgo

cp -v /go/bin/ginkgo /usr/bin
go get github.com/onsi/gomega/...

#rm /usr/libexec/podman/conmon || echo "No conmon found"

# Build, Install, and then run integration tests
sh ./.papr.sh -b  -i -t
