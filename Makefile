CC ?= cc
CFLAGS ?= -Wall -Wextra -pedantic -std=c99 -O2
LDFLAGS ?=
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
PKGROOT ?= pkgroot
PKGNAME ?= minimal-ais
PKGVERSION ?= 0.1.1
PKGARCH ?= $(shell if command -v dpkg >/dev/null 2>&1; then dpkg --print-architecture; else uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/'; fi)
PKGFILE = $(PKGNAME)_$(PKGVERSION)_$(PKGARCH).deb

TARGETS = minimal-ais-serial/minimal-ais-serial minimal-ais-json/minimal-ais-json
MANPAGES = man/minimal-ais-serial.1 man/minimal-ais-json.1

SERIAL_SRCS = minimal-ais-serial/main.c minimal-ais-serial/tracking.c
SERIAL_OBJS = $(SERIAL_SRCS:.c=.o)

JSON_SRCS = minimal-ais-json/main.c
JSON_OBJS = $(JSON_SRCS:.c=.o)

.PHONY: all clean install pkgroot deb

all: $(TARGETS)

minimal-ais-serial/minimal-ais-serial: $(SERIAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERIAL_OBJS) $(LDFLAGS)

minimal-ais-json/minimal-ais-json: $(JSON_OBJS)
	$(CC) $(CFLAGS) -o $@ $(JSON_OBJS) $(LDFLAGS)

clean:
	rm -rf $(TARGETS) $(SERIAL_OBJS) $(JSON_OBJS) $(PKGROOT) $(PKGFILE)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 755 minimal-ais-serial/minimal-ais-serial $(DESTDIR)$(BINDIR)/minimal-ais-serial
	install -m 755 minimal-ais-json/minimal-ais-json $(DESTDIR)$(BINDIR)/minimal-ais-json
	install -m 644 man/minimal-ais-serial.1 $(DESTDIR)$(MANDIR)/man1/minimal-ais-serial.1
	install -m 644 man/minimal-ais-json.1 $(DESTDIR)$(MANDIR)/man1/minimal-ais-json.1

pkgroot: all
	rm -rf $(PKGROOT)
	$(MAKE) install DESTDIR=$(CURDIR)/$(PKGROOT) PREFIX=/usr
	install -d $(PKGROOT)/DEBIAN
	install -d $(PKGROOT)/etc/systemd/system
	sed -e 's/@PKGVERSION@/$(PKGVERSION)/g' -e 's/@PKGARCH@/$(PKGARCH)/g' DEBIAN/control > $(PKGROOT)/DEBIAN/control
	install -m 644 DEBIAN/conffiles $(PKGROOT)/DEBIAN/conffiles
	install -m 755 DEBIAN/postinst $(PKGROOT)/DEBIAN/postinst
	install -m 755 DEBIAN/postrm $(PKGROOT)/DEBIAN/postrm
	install -m 644 systemd/minimal-ais-serial.service $(PKGROOT)/etc/systemd/system/minimal-ais-serial.service
	install -m 644 systemd/minimal-ais-json.service $(PKGROOT)/etc/systemd/system/minimal-ais-json.service

deb: pkgroot
	dpkg-deb --build --root-owner-group $(PKGROOT) $(PKGFILE)
