# SPDX-License-Identifier: LGPL-2.1-or-later

CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr
DESTDIR ?=
ROOTDIR := ../../..
PLUGIN := libgstcrystalhd.so
PLAYBACK_TEST := gstreamer-playback-test
CONTROLS_TEST := gstreamer-controls-test
LIFECYCLE_TEST := gst-lifecycle-test
CODECS_TEST := gst-codecs-test
CODEC_PLAYBACK_PACKAGES := gstreamer-app-1.0 gstreamer-video-1.0 \
	libavformat libavcodec libavutil
HAVE_CODEC_PLAYBACK := $(shell $(PKG_CONFIG) --exists $(CODEC_PLAYBACK_PACKAGES) && echo yes)
PLUGINDIR ?= $(shell $(PKG_CONFIG) --variable=pluginsdir gstreamer-1.0)
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
PYTHON ?= /usr/bin/python3

GST_PACKAGES := gstreamer-1.0 gstreamer-video-1.0
CPPFLAGS += -D__LINUX_USER__ -I$(ROOTDIR)/linux_lib/libcrystalhd \
	-I$(ROOTDIR)/include
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Werror -fPIC $(shell $(PKG_CONFIG) --cflags $(GST_PACKAGES))
LDFLAGS += -shared -L$(ROOTDIR)/linux_lib/libcrystalhd
LDLIBS += $(shell $(PKG_CONFIG) --libs $(GST_PACKAGES)) -lcrystalhd

.PHONY: all clean install check codec-playback-test check-codec-playback check-player

all: $(PLUGIN)

# Optional hardware fixture probe; libavformat supplies complete VC-1/WMV3
# packets. FFmpeg development packages are not needed for the plugin itself.
codec-playback-test: gstreamer-codec-playback-test

gstreamer-codec-playback-test: $(ROOTDIR)/tests/gstreamer-codec-playback.cpp
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror $< -o $@ \
		$(shell $(PKG_CONFIG) --cflags --libs $(CODEC_PLAYBACK_PACKAGES))

ifneq ($(HAVE_CODEC_PLAYBACK),)
check-codec-playback: gstreamer-codec-playback-test
	./gstreamer-codec-playback-test --self-test
else
check-codec-playback:
	@echo "Skipping optional codec probe self-test (FFmpeg/GStreamer app development packages unavailable)"
endif

$(PLUGIN): gstcrystalhd.c gstcrystalhd-codecs.h gstcrystalhd-timing.h gstcrystalhd-input.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(PLAYBACK_TEST): $(ROOTDIR)/tests/gstreamer-playback.c
	$(CC) -Wall -Wextra -Werror $(CFLAGS) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES))

$(CONTROLS_TEST): $(ROOTDIR)/tests/gstreamer-controls.c
	$(CC) -Wall -Wextra -Werror $(CFLAGS) \
		$(shell $(PKG_CONFIG) --cflags gstreamer-audio-1.0) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES) gstreamer-audio-1.0) -lm

# Includes production source but supplies its own Dts mocks. This target must
# never link libcrystalhd or open the real decoder.
$(LIFECYCLE_TEST): $(ROOTDIR)/tests/gst-lifecycle.c gstcrystalhd.c \
		gstcrystalhd-codecs.h gstcrystalhd-timing.h gstcrystalhd-input.h
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror $(CPPFLAGS) $< -o $@ \
		$(shell $(PKG_CONFIG) --cflags --libs gstreamer-check-1.0 gstreamer-video-1.0)

check-player:
	sh -n $(ROOTDIR)/scripts/crystalhd-play
	$(PYTHON) -B $(ROOTDIR)/tests/gstreamer-player.test.py

$(CODECS_TEST): $(ROOTDIR)/tests/gst-codecs.c gstcrystalhd-codecs.h gstcrystalhd-timing.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. $< -o $@ \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES))

check: $(PLUGIN) $(PLAYBACK_TEST) $(CONTROLS_TEST) $(LIFECYCLE_TEST) $(CODECS_TEST) \
		check-codec-playback check-player
	@registry=$$(mktemp); \
	trap 'rm -f "$$registry"' EXIT HUP INT TERM; \
	GST_REGISTRY="$$registry" GST_PLUGIN_PATH=$(CURDIR) \
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) \
		gst-inspect-1.0 crystalhddec >/dev/null
	./$(PLAYBACK_TEST) --self-test
	./$(CONTROLS_TEST) --self-test
	./$(LIFECYCLE_TEST)
	@registry=$$(mktemp); \
	trap 'rm -f "$$registry"' EXIT HUP INT TERM; \
	GST_REGISTRY="$$registry" GST_PLUGIN_PATH=$(CURDIR) \
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) \
		./$(CODECS_TEST)

install: $(PLUGIN)
	install -D -m 0755 $(PLUGIN) $(DESTDIR)$(PLUGINDIR)/$(PLUGIN)
	install -D -m 0755 $(ROOTDIR)/scripts/crystalhd-play $(DESTDIR)$(BINDIR)/crystalhd-play
	install -D -m 0644 $(ROOTDIR)/scripts/crystalhd-player.py \
		$(DESTDIR)$(DATADIR)/crystalhd/player.py

clean:
	rm -f $(PLUGIN) $(PLAYBACK_TEST) $(CONTROLS_TEST) $(LIFECYCLE_TEST) $(CODECS_TEST) gstreamer-codec-playback-test
