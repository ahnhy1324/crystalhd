# SPDX-License-Identifier: MIT
CC ?= cc
PKG_CONFIG ?= pkg-config
PACKAGES := gstreamer-video-1.0 xcb xcb-shm xcb-xv
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror -fPIC
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))

.PHONY: all clean check check-x11
all: libgstcrystalhdxv.so

libgstcrystalhdxv.so: gstcrystalhdxv.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $< $(LDFLAGS) $(LDLIBS) -o $@

gst-xv-output-test: ../../../tests/gst-xv-output.c gstcrystalhdxv.c
	$(CC) $(CPPFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-app-1.0) $(CFLAGS) \
		$< $(LDFLAGS) $(LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-app-1.0) -o $@

check: all gst-xv-output-test
	@registry=$$(mktemp); trap 'rm -f "$$registry"' EXIT HUP INT TERM; \
	GST_REGISTRY="$$registry" GST_PLUGIN_PATH="$(CURDIR)" \
		gst-inspect-1.0 crystalhdxvsink >/dev/null
	./gst-xv-output-test -p /xv/identity
	./gst-xv-output-test -p /xv/setup-failure

# Explicit X11 test: opens temporary video windows; no CrystalHD device access.
check-x11: all gst-xv-output-test
	CRYSTALHD_TEST_X11=1 ./gst-xv-output-test

clean:
	rm -f libgstcrystalhdxv.so gst-xv-output-test
