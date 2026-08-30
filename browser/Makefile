# SPDX-License-Identifier: LGPL-2.1-or-later

PREFIX ?= /usr
DESTDIR ?=
ROOTDIR := ..
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

.PHONY: all check install

all:

check:
	sh -n $(ROOTDIR)/scripts/crystalhd-chromium
	sh -n $(ROOTDIR)/scripts/setup-crystalhd-chrome-default
	test -s h264-only/manifest.json
	test -s h264-only/prefer-h264.js
	test -s crystalhd-chromium.desktop

install:
	install -D -m 0755 $(ROOTDIR)/scripts/crystalhd-chromium \
		$(DESTDIR)$(BINDIR)/crystalhd-chromium
	install -D -m 0755 $(ROOTDIR)/scripts/setup-crystalhd-chrome-default \
		$(DESTDIR)$(BINDIR)/setup-crystalhd-chrome-default
	install -D -m 0644 h264-only/manifest.json \
		$(DESTDIR)$(DATADIR)/crystalhd/h264-only/manifest.json
	install -D -m 0644 h264-only/prefer-h264.js \
		$(DESTDIR)$(DATADIR)/crystalhd/h264-only/prefer-h264.js
	install -D -m 0644 crystalhd-chromium.desktop \
		$(DESTDIR)$(DATADIR)/applications/crystalhd-chromium.desktop
