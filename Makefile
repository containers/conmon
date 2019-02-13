include Makefile.inc

GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null)
GIT_BRANCH_CLEAN := $(shell echo $(GIT_BRANCH) | sed -e "s/[^[:alnum:]]/-/g")
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

bin/conmon: src/conmon.o src/cmsg.o | bin
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

src/cmsg.o: src/cmsg.c src/cmsg.h

src/conmon.o: src/conmon.c src/cmsg.h src/config.h

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
