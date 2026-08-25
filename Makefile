VERSION := $(shell cat VERSION)
PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin
LIBEXECDIR ?= ${PREFIX}/libexec
PKG_CONFIG ?= pkg-config
HEADERS := $(wildcard src/*.h)

OBJS := src/conmon.o src/cmsg.o src/ctr_logging.o src/utils.o src/cli.o src/globals.o src/cgroup.o src/conn_sock.o src/oom.o src/ctrl.o src/ctr_stdio.o src/parent_pipe_fd.o src/ctr_exit.o src/runtime_args.o src/close_fds.o src/self_pipe.o

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

CFLAGS ?= -std=c99 -Os -Wall -Wextra -Werror -Wstrict-prototypes -Wold-style-definition
override CFLAGS += $(shell $(PKG_CONFIG) --cflags glib-2.0) -DVERSION=\"$(VERSION)\" -DGIT_COMMIT=\"$(GIT_COMMIT)\"

# Conditionally compile journald logging code if the libraries can be found
# if they can be found, set USE_JOURNALD macro for use in conmon code.
#
# "pkg-config --exists" will error if the package doesn't exist. Make can only compare
# output of commands, so the echo commands are to allow pkg-config to error out, make to catch it,
# and allow the compilation to complete.
#
# For static builds, systemd can be disabled with DISABLE_SYSTEMD=1
ifneq ($(DISABLE_SYSTEMD), 1)
ifeq ($(shell $(PKG_CONFIG) --exists libsystemd && echo "0"), 0)
	override LIBS += $(shell $(PKG_CONFIG) --libs libsystemd)
	override CFLAGS += $(shell $(PKG_CONFIG) --cflags libsystemd) -D USE_JOURNALD=1
endif
endif

# Update nix/nixpkgs.json its latest stable commit
.PHONY: nixpkgs
nixpkgs:
	@nix run -f channel:nixpkgs-unstable nix-prefetch-git -- \
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

# config target removed - no longer using Go build system

.PHONY: test-binary
test-binary: bin/conmon
	CONMON_BINARY="$(MAKEFILE_PATH)bin/conmon" test/run-tests.sh

.PHONY: test
test: test-binary

.PHONY: test-coverage
test-coverage: DEBUGFLAG += --coverage
test-coverage: clean test-binary
	@echo "=== Coverage Summary ==="
	@if command -v gcovr >/dev/null 2>&1; then \
		gcovr -r . --txt-metric=branch || true; \
	else \
		echo "WARNING: gcovr not found in PATH, falling back to gcov."; \
		for file in src/*.c; do \
			gcov "$$file" 2>/dev/null \
			| grep -E '(^File|^Lines executed)' \
			| paste - - \
			| sed "s|File 'src/\([^.]*\)\.c'.*Lines executed:\([0-9.]*%\).*|\1.c: \2|"; \
		done | grep -E '\.c: [0-9]' | sort; \
	fi

bin:
	mkdir -p bin

# vendor target removed - no longer using Go modules

.PHONY: docs
docs:
	$(MAKE) -C docs

.PHONY: clean
clean:
	rm -rf bin/ src/*.o src/*.gcno src/*.gcda *.gcov
	$(MAKE) -C test clean
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

# Clang's static analyzer. The disabled checkers below are all false positives
# on this code base: none of them models __attribute__((cleanup)), so every
# _cleanup_free_ / _cleanup_close_ / _cleanup_fclose_ variable looks either
# leaked (unix.Malloc, unix.Stream) or assigned but never read
# (deadcode.DeadStores).
#
# For a browsable HTML report of everything, including the above, run
# "scan-build make" by hand.
#
# Naming a checker the compiler does not know about is a hard error, and the
# set moves between releases (unix.Stream is alpha, and thus off by default,
# before clang 19), so the list is intersected with what this clang has.
CLANG ?= clang
ANALYZER_OFF := deadcode.DeadStores unix.Malloc unix.Stream
ANALYZER_CFLAGS := --analyze -Werror $(foreach c,\
	$(shell $(CLANG) -cc1 -analyzer-checker-help 2>/dev/null \
		| awk '{ print $$1 }' | grep -xF $(foreach c,$(ANALYZER_OFF),-e $(c))),\
	-Xclang -analyzer-disable-checker -Xclang $(c))

SRCS := $(OBJS:.o=.c)

.PHONY: analyze
analyze: $(SRCS) $(HEADERS)
	@for src in $(SRCS); do \
		echo "  ANALYZE $$src"; \
		$(CLANG) $(CFLAGS) $(ANALYZER_CFLAGS) -o /dev/null $$src || exit 1; \
	done

# cppcheck only understands -D, -U and -I, so the flags are built separately
# from CFLAGS. GIT_COMMIT is only there to make src/cli.c preprocess.
CPPCHECK ?= cppcheck
CPPCHECK_FLAGS := $(shell $(PKG_CONFIG) --cflags-only-I glib-2.0) \
	-DVERSION=\"$(VERSION)\" -DGIT_COMMIT=\"\" -DUSE_JOURNALD=1

.PHONY: cppcheck
cppcheck:
	$(CPPCHECK) --quiet --error-exitcode=1 --std=c99 --inline-suppr \
		--enable=warning,performance,portability \
		--suppress=missingIncludeSystem --suppress=checkersReport \
		$(CPPCHECK_FLAGS) src/

.PHONY: fmt
fmt:
	git ls-files -z \*.c \*.h | xargs -0 clang-format -i
	git ls-files -z '*.bats' | xargs -0 sed -i 's/[[:space:]]\+$$//'

.PHONY: dbuild
dbuild:
	-mkdir -p bin
	-podman rm conmon-devenv
	podman build -t conmon-devenv:latest .
	podman create --name conmon-devenv conmon-devenv:latest
	podman cp conmon-devenv:/devenv/bin/conmon bin/conmon
	@echo "for installation move conmon file to /usr/local/libexec/podman/"
