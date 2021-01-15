PREFIX ?= /usr/local
DATADIR := ${PREFIX}/share
MANDIR := $(DATADIR)/man
GOMD2MAN = ../tools/build/go-md2man

docs: $(patsubst %.md,%,$(wildcard *.8.md))

%.8: %.8.md
	$(GOMD2MAN) -in $^ -out $@

.PHONY: install
install:
	install -d ${DESTDIR}/${MANDIR}/man8
	install -m 0644 conmon*.8 ${DESTDIR}/${MANDIR}/man8

.PHONY: clean
clean:
	$(RM) conmon*.8
