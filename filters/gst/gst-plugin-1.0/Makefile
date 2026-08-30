# SPDX-License-Identifier: LGPL-2.1-or-later

CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr
DESTDIR ?=
ROOTDIR := ../../..
PLUGIN := libgstcrystalhd.so
PLUGINDIR ?= $(shell $(PKG_CONFIG) --variable=pluginsdir gstreamer-1.0)

GST_PACKAGES := gstreamer-1.0 gstreamer-video-1.0
CPPFLAGS += -D__LINUX_USER__ -I$(ROOTDIR)/linux_lib/libcrystalhd \
	-I$(ROOTDIR)/include
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Werror -fPIC $(shell $(PKG_CONFIG) --cflags $(GST_PACKAGES))
LDFLAGS += -shared -L$(ROOTDIR)/linux_lib/libcrystalhd
LDLIBS += $(shell $(PKG_CONFIG) --libs $(GST_PACKAGES)) -lcrystalhd

.PHONY: all clean install check

all: $(PLUGIN)

$(PLUGIN): gstcrystalhd.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

check: $(PLUGIN)
	GST_PLUGIN_PATH=$(CURDIR) \
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd) \
		gst-inspect-1.0 crystalhddec >/dev/null

install: $(PLUGIN)
	install -D -m 0755 $(PLUGIN) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN)

clean:
	rm -f $(PLUGIN)
