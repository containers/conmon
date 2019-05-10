include Makefile.inc

GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null)
GIT_BRANCH_CLEAN := $(shell echo $(GIT_BRANCH) | sed -e "s/[^[:alnum:]]/-/g")
VERSION := $(shell cat VERSION)
PREFIX ?= ${DESTDIR}/usr/local
BINDIR ?= ${PREFIX}/bin
LIBEXECDIR ?= ${PREFIX}/libexec
MANDIR ?= ${PREFIX}/share/man
ETCDIR ?= ${DESTDIR}/etc

.PHONY: all
all: bin bin/conmon

override LIBS += $(shell pkg-config --libs glib-2.0)

CFLAGS ?= -std=c99 -Os -Wall -Wextra
override CFLAGS += $(shell pkg-config --cflags glib-2.0) -DVERSION=\"$(VERSION)\" -DGIT_COMMIT=\"$(GIT_COMMIT)\"

# Conditionally compile journald logging code if the libraries can be found
# if they can be found, set USE_JOURNALD macro for use in conmon code.
#
# "pkg-config --exists" will error if the package doesn't exist. Make can only compare
# output of commands, so the echo commands are to allow pkg-config to error out, make to catch it,
# and allow the compilation to complete.
ifeq ($(shell pkg-config --exists libsystemd-journal && echo "0" || echo "1"), 0)
	override LIBS += $(shell pkg-config --libs libsystemd-journal)
	override CFLAGS += $(shell pkg-config --cflags libsystemd-journal) -D USE_JOURNALD=0
else ifeq ($(shell pkg-config --exists libsystemd && echo "0" || echo "1"), 0)
	override LIBS += $(shell pkg-config --libs libsystemd)
	override CFLAGS += $(shell pkg-config --cflags libsystemd) -D USE_JOURNALD=0
endif

bin/conmon: src/conmon.o src/cmsg.o src/ctr_logging.o src/utils.o | bin
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

src/cmsg.o: src/cmsg.c src/cmsg.h

src/utils.o: src/utils.c src/utils.h

src/ctr_logging.o: src/ctr_logging.c src/ctr_logging.h src/utils.h

src/conmon.o: src/conmon.c src/cmsg.h src/config.h src/utils.h src/ctr_logging.h

bin:
	mkdir -p bin

.PHONY: clean
clean:
	rm -f bin/conmon src/*.o
	rmdir bin

.PHONY: install install.bin
install: install.bin

install.bin: bin/conmon
	install ${SELINUXOPT} -D -m 755 bin/conmon $(LIBEXECDIR)/crio/conmon
