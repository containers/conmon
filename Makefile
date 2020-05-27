VERSION := $(shell cat VERSION)
PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin
LIBEXECDIR ?= ${PREFIX}/libexec
GO ?= go
PROJECT := github.com/containers/conmon
PKG_CONFIG ?= pkg-config
HEADERS := $(wildcard src/*.h)
OBJS := src/conmon.o src/cmsg.o src/ctr_logging.o src/utils.o src/cli.o src/globals.o src/cgroup.o src/conn_sock.o src/oom.o src/ctrl.o src/ctr_stdio.o src/parent_pipe_fd.o src/ctr_exit.o src/runtime_args.o



.PHONY: all git-vars
all: git-vars bin bin/conmon

git-vars:
ifeq ($(shell bash -c '[[ `command -v git` && `git rev-parse --git-dir 2>/dev/null` ]] && echo true'),true)
	$(eval COMMIT_NO :=$(shell git rev-parse HEAD 2> /dev/null || true))
	$(eval GIT_COMMIT := $(if $(shell git status --porcelain --untracked-files=no),"${COMMIT_NO}-dirty","${COMMIT_NO}"))
	$(eval GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null))
	$(eval GIT_BRANCH_CLEAN := $(shell echo $(GIT_BRANCH) | sed -e "s/[^[:alnum:]]/-/g"))
else
	$(eval COMMIT_NO := unknown)
	$(eval GIT_COMMIT := unknown)
	$(eval GIT_BRANCH := unknown)
	$(eval GIT_BRANCH_CLEAN := unknown)
endif

override LIBS += $(shell $(PKG_CONFIG) --libs glib-2.0)

CFLAGS ?= -std=c99 -Os -Wall -Wextra -Werror
override CFLAGS += $(shell $(PKG_CONFIG) --cflags glib-2.0) -DVERSION=\"$(VERSION)\" -DGIT_COMMIT=\"$(GIT_COMMIT)\"

# Conditionally compile journald logging code if the libraries can be found
# if they can be found, set USE_JOURNALD macro for use in conmon code.
#
# "pkg-config --exists" will error if the package doesn't exist. Make can only compare
# output of commands, so the echo commands are to allow pkg-config to error out, make to catch it,
# and allow the compilation to complete.
ifeq ($(shell $(PKG_CONFIG) --exists libsystemd-journal && echo "0" || echo "1"), 0)
	override LIBS += $(shell $(PKG_CONFIG) --libs libsystemd-journal)
	override CFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd-journal) -D USE_JOURNALD=0
else ifeq ($(shell $(PKG_CONFIG) --exists libsystemd && echo "0" || echo "1"), 0)
	override LIBS += $(shell $(PKG_CONFIG) --libs libsystemd)
	override CFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd) -D USE_JOURNALD=0
endif

define DOCKERFILE
	FROM alpine:latest
	RUN apk add --update --no-cache bash make git gcc pkgconf libc-dev glib-dev glib-static
	COPY . /go/src/$(PROJECT)
	WORKDIR /go/src/$(PROJECT)
	RUN make static
endef
export DOCKERFILE

containerized: bin
	$(eval PODMAN ?= $(if $(shell podman -v),podman,docker))
	echo "$$DOCKERFILE" | $(PODMAN) build --force-rm -t conmon-build -f - .
	CTR=`$(PODMAN) create conmon-build` \
		&& $(PODMAN) cp $$CTR:/go/src/$(PROJECT)/bin/conmon bin/conmon \
		&& $(PODMAN) rm $$CTR

static:
	$(MAKE) git-vars bin/conmon PKG_CONFIG='$(PKG_CONFIG) --static' CFLAGS='-static' LDFLAGS='$(LDFLAGS) -s -w -static' LIBS='$(LIBS)'

nixpkgs:
	@nix run -f channel:nixpkgs-unstable nix-prefetch-git -c nix-prefetch-git \
		--no-deepClone https://github.com/nixos/nixpkgs > nix/nixpkgs.json

bin/conmon: $(OBJS) | bin
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ -c $<

config: git-vars cmd/conmon-config/conmon-config.go runner/config/config.go runner/config/config_unix.go runner/config/config_windows.go
	$(GO) build $(LDFLAGS) -tags "$(BUILDTAGS)" -o bin/config $(PROJECT)/cmd/conmon-config
		( cd src && $(CURDIR)/bin/config )

test: git-vars runner/conmon_test/*.go runner/conmon/*.go
	$(GO) test $(LDFLAGS) -tags "$(BUILDTAGS)" $(PROJECT)/runner/conmon_test/

bin:
	mkdir -p bin

vendor:
	export GO111MODULE=on \
		$(GO) mod tidy && \
		$(GO) mod vendor && \
		$(GO) mod verify

.PHONY: vendor

.PHONY: clean
clean:
	rm -rf bin/ src/*.o

.PHONY: install install.bin install.crio install.podman podman crio
install: install.bin

podman: install.podman

crio: install.crio

install.bin: bin/conmon
	install ${SELINUXOPT} -D -m 755 bin/conmon $(DESTDIR)$(BINDIR)/conmon

install.crio: bin/conmon
	install ${SELINUXOPT} -D -m 755 bin/conmon $(DESTDIR)$(LIBEXECDIR)/crio/conmon

install.podman: bin/conmon
	install ${SELINUXOPT} -D -m 755 bin/conmon $(DESTDIR)$(LIBEXECDIR)/podman/conmon

.PHONY: fmt
fmt:
	find . '(' -name '*.h' -o -name '*.c' ! -path './vendor/*' ')'  -exec clang-format -i {} \+
	find . -name '*.go' ! -path './vendor/*' -exec gofmt -s -w {} \+
	git diff --exit-code
