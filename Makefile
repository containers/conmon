include Makefile.inc

GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null)
GIT_BRANCH_CLEAN := $(shell echo $(GIT_BRANCH) | sed -e "s/[^[:alnum:]]/-/g")
PREFIX ?= ${DESTDIR}/usr/local
BINDIR ?= ${PREFIX}/bin
LIBEXECDIR ?= ${PREFIX}/libexec
MANDIR ?= ${PREFIX}/share/man
ETCDIR ?= ${DESTDIR}/etc

all: bin/conmon

src = $(wildcard *.c)
obj = $(src:.c=.o)

override LIBS += $(shell pkg-config --libs glib-2.0)

CFLAGS ?= -std=c99 -Os -Wall -Wextra
override CFLAGS += $(shell pkg-config --cflags glib-2.0) -DVERSION=\"$(VERSION)\" -DGIT_COMMIT=\"$(GIT_COMMIT)\"

bin/conmon:
	mkdir -p bin
	$(CC) $^ $(CFLAGS) $(LIBS) -c -o src/conmon.o src/conmon.c
	$(CC) $^ $(CFLAGS) $(LIBS) -c -o src/cmsg.o src/cmsg.c
	$(CC) -o bin/conmon src/config.h src/conmon.o src/cmsg.o $^ $(CFLAGS) $(LIBS)

.PHONY: clean
clean:
	rm -f $(obj) bin/conmon

install: install.bin

install.bin: bin/conmon
	install ${SELINUXOPT} -D -m 755 bin/conmon $(LIBEXECDIR)/crio/conmon
