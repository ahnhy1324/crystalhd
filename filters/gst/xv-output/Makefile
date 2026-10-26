# SPDX-License-Identifier: MIT
CC ?= cc
PKG_CONFIG ?= pkg-config
PACKAGES := gstreamer-video-1.0 xcb xcb-shm xcb-xv
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror -fPIC
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))

.PHONY: all clean check
all: libgstcrystalhdxv.so

libgstcrystalhdxv.so: gstcrystalhdxv.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $< $(LDFLAGS) $(LDLIBS) -o $@

check: all
	@registry=$$(mktemp); trap 'rm -f "$$registry"' EXIT HUP INT TERM; \
	GST_REGISTRY="$$registry" GST_PLUGIN_PATH="$(CURDIR)" \
		gst-inspect-1.0 crystalhdxvsink >/dev/null

clean:
	rm -f libgstcrystalhdxv.so
