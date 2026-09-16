CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
DOCDIR ?= $(PREFIX)/share/doc/m920q-fanctl

.PHONY: all clean install uninstall

all: m920q-fanctl

m920q-fanctl: m920q-fanctl.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: m920q-fanctl
	install -Dm755 m920q-fanctl $(DESTDIR)$(SBINDIR)/m920q-fanctl
	install -Dm644 README.md $(DESTDIR)$(DOCDIR)/README.md

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/m920q-fanctl
	rm -rf $(DESTDIR)$(DOCDIR)

clean:
	rm -f m920q-fanctl
