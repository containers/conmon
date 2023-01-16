VERSION := $(shell cat VERSION)
PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin
LIBEXECDIR ?= ${PREFIX}/libexec
GO ?= go
PROJECT := github.com/containers/conmon
PKG_CONFIG ?= pkg-config
HEADERS := $(wildcard src/*.h)

OBJS := src/conmon.o src/cmsg.o src/ctr_logging.o src/utils.o src/cli.o src/globals.o src/cgroup.o src/conn_sock.o src/oom.o src/ctrl.o src/ctr_stdio.o src/parent_pipe_fd.o src/ctr_exit.o src/runtime_args.o src/close_fds.o src/seccomp_notify.o

MAKEFILE_PATH := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all git-vars docs
all: git-vars bin bin/conmon

git-vars:
ifeq ($(shell bash -c '[[ `command -v git` && `git rev-parse --git-dir 2>/dev/null` ]] && echo 0'),0)
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
ifeq ($(shell $(PKG_CONFIG) --exists libsystemd-journal && echo "0"), 0)
	override LIBS += $(shell $(PKG_CONFIG) --libs libsystemd-journal)
	override CFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd-journal) -D USE_JOURNALD=1
else ifeq ($(shell $(PKG_CONFIG) --exists libsystemd && echo "0"), 0)
	override LIBS += $(shell $(PKG_CONFIG) --libs libsystemd)
	override CFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd) -D USE_JOURNALD=1
endif

ifeq ($(shell hack/seccomp-notify.sh), 0)
	override LIBS += $(shell $(PKG_CONFIG) --libs libseccomp) -ldl
	override CFLAGS += $(shell $(PKG_CONFIG) --cflags libseccomp) -D USE_SECCOMP=1
endif

# Update nix/nixpkgs.json its latest stable commit
.PHONY: nixpkgs
nixpkgs:
	@nix run -f channel:nixpkgs-unstable nix-prefetch-git -c nix-prefetch-git \
		--no-deepClone https://github.com/nixos/nixpkgs > nix/nixpkgs.json

# Build statically linked binary
.PHONY: static
static:
	@nix build -f nix/
	mkdir -p ./bin
	cp -rfp ./result/bin/* ./bin/

bin/conmon: $(OBJS) | bin
	$(CC) $(LDFLAGS) $(CFLAGS) $(DEBUGFLAG) -o $@ $^ $(LIBS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) $(DEBUGFLAG) -o $@ -c $<

config: git-vars cmd/conmon-config/conmon-config.go runner/config/config.go runner/config/config_unix.go runner/config/config_windows.go
	$(GO) build $(LDFLAGS) -tags "$(BUILDTAGS)" -o bin/config $(PROJECT)/cmd/conmon-config
		( cd src && $(CURDIR)/bin/config )

.PHONY: test-binary
test-binary: bin/conmon _test-files
	CONMON_BINARY="$(MAKEFILE_PATH)bin/conmon" $(GO) test $(LDFLAGS) -tags "$(BUILDTAGS)" $(PROJECT)/runner/conmon_test/ -count=1 -v

.PHONY: test
test:_test-files
	$(GO) test $(LDFLAGS) -tags "$(BUILDTAGS)" $(PROJECT)/runner/conmon_test/

.PHONY: test-files
_test-files: git-vars runner/conmon_test/*.go runner/conmon/*.go

bin:
	mkdir -p bin

.PHONY: vendor
vendor:
	GO111MODULE=on $(GO) mod tidy
	GO111MODULE=on $(GO) mod vendor
	GO111MODULE=on $(GO) mod verify

.PHONY: docs
docs: install.tools
	$(MAKE) -C docs

.PHONY: clean
clean:
	rm -rf bin/ src/*.o
	$(MAKE) -C docs clean

.PHONY: install install.bin install.crio install.podman podman crio
install: install.bin docs
	$(MAKE) -C docs install

podman: install.podman

crio: install.crio

install.bin: bin/conmon
	install ${SELINUXOPT} -d -m 755 $(DESTDIR)$(BINDIR)
	install ${SELINUXOPT} -m 755 bin/conmon $(DESTDIR)$(BINDIR)/conmon

install.crio: bin/conmon
	install ${SELINUXOPT} -d -m 755 $(DESTDIR)$(LIBEXECDIR)/crio
	install ${SELINUXOPT} -m 755 bin/conmon $(DESTDIR)$(LIBEXECDIR)/crio/conmon

install.podman: bin/conmon
	install ${SELINUXOPT} -d -m 755 $(DESTDIR)$(LIBEXECDIR)/podman
	install ${SELINUXOPT} -m 755 bin/conmon $(DESTDIR)$(LIBEXECDIR)/podman/conmon

install.tools:
	$(MAKE) -C tools

.PHONY: fmt
fmt:
	find . '(' -name '*.h' -o -name '*.c' ! -path './vendor/*' ! -path './tools/vendor/*' ')' -exec clang-format -i {} \+
	find . -name '*.go' ! -path './vendor/*' ! -path './tools/vendor/*' -exec gofmt -s -w {} \+
	git diff --exit-code


.PHONY: dbuild
dbuild:
	-mkdir -p bin
	-podman rm conmon-devenv
	podman build -t conmon-devenv:latest .
	podman create --name conmon-devenv conmon-devenv:latest
	podman cp conmon-devenv:/devenv/bin/conmon bin/conmon
	@echo "for installation move conmon file to /usr/local/libexec/podman/"
