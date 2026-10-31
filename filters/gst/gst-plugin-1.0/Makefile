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
LIFECYCLE_LEGACY_TEST := gst-lifecycle-legacy-test
CODECS_TEST := gst-codecs-test
DI_QOS_TEST := gst-diqos-test
# Explicit opt-in outputs, overridable for isolated private builds.
MPEG1_PLUGIN ?= libgstcrystalhdmpeg1experimental.so
MPEG1_TEST ?= gst-mpeg1-experimental-test
MPEG1_PLAYBACK ?= mpeg1-natural-playback-test
MPEG1_DIL ?= $(ROOTDIR)/linux_lib/libcrystalhd/libcrystalhd.so
MPEG1_LDFLAGS ?= $(LDFLAGS)
MPEG1_TEST_LDFLAGS ?= $(filter-out -shared,$(MPEG1_LDFLAGS))
MPEG1_LDLIBS ?= $(filter-out -lcrystalhd,$(LDLIBS)) $(MPEG1_DIL)
MPEG1_CONFIG ?= $(MPEG1_PLUGIN).config
MPEG1_SOURCES := gstcrystalhd-mpeg1-experimental.c gstcrystalhd-finish-trace.c
MPEG1_DEPS := gstcrystalhd.c gstcrystalhd-mpeg1-input.h \
		gstcrystalhd-codecs.h gstcrystalhd-timing.h gstcrystalhd-input.h \
		gstcrystalhd-diqos.h gstcrystalhd-stage-trace.h gstcrystalhd-finish-trace.h \
		$(ROOTDIR)/filters/crystalhd-h264-output.h \
		$(ROOTDIR)/filters/crystalhd-mpeg2-inspect.h \
		$(ROOTDIR)/cpu.mk $(ROOTDIR)/include/crystalhd_legacy.h
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
LDFLAGS += -shared -Wl,--build-id=sha1 -L$(ROOTDIR)/linux_lib/libcrystalhd
LDLIBS += $(shell $(PKG_CONFIG) --libs $(GST_PACKAGES)) -lcrystalhd

.PHONY: all clean install install-player uninstall check codec-playback-test check-codec-playback check-player check-diqos check-stage-trace check-finish-trace
.PHONY: experimental-mpeg1 check-experimental-mpeg1 check-experimental-mpeg1-frontend install-experimental-mpeg1 uninstall-experimental-mpeg1 mpeg1-config-force

all: $(PLUGIN)

# Optional targets require an existing DIL; they never rebuild it or the
# ordinary plugin. The optional stamp follows the chosen private output.
experimental-mpeg1: $(MPEG1_PLUGIN)

$(MPEG1_CONFIG): mpeg1-config-force
	@set -e; mkdir -p "$(dir $@)"; config_tmp="$@.tmp"; \
	trap 'rm -f "$$config_tmp"' EXIT HUP INT TERM; \
	printf '%s\n' $(call crystalhd_quote,$(CC)|$(CPPFLAGS)|$(CFLAGS)|$(MPEG1_LDFLAGS)|$(MPEG1_TEST_LDFLAGS)|$(MPEG1_LDLIBS)|$(CRYSTALHD_CPU_FLAGS)) > "$$config_tmp"; \
	if ! cmp -s "$$config_tmp" "$@"; then mv -f "$$config_tmp" "$@"; fi

$(MPEG1_PLUGIN): $(MPEG1_SOURCES) $(MPEG1_DEPS) $(MPEG1_DIL) $(MPEG1_CONFIG)
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MPEG1_SOURCES) -o "$@" \
		$(MPEG1_LDFLAGS) $(MPEG1_LDLIBS) $(CRYSTALHD_CPU_FLAGS)

$(MPEG1_TEST): $(ROOTDIR)/tests/gst-mpeg1-experimental.c $(MPEG1_SOURCES) $(MPEG1_DEPS) $(MPEG1_DIL) $(MPEG1_CONFIG)
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -DGST_CRYSTALHD_MPEG1_EXPERIMENTAL_SELF_TEST \
		$(ROOTDIR)/tests/gst-mpeg1-experimental.c $(MPEG1_SOURCES) -o "$@" \
		$(MPEG1_TEST_LDFLAGS) $(MPEG1_LDLIBS) $(CRYSTALHD_CPU_FLAGS)

$(MPEG1_PLAYBACK): $(ROOTDIR)/tests/mpeg1-natural-playback.c $(ROOTDIR)/tests/mpeg1-research-decoder.c gstcrystalhd-finish-trace.c $(MPEG1_DEPS) $(MPEG1_DIL) $(MPEG1_CONFIG)
	@mkdir -p "$(dir $@)"
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-audio-1.0) \
		$(ROOTDIR)/tests/mpeg1-natural-playback.c $(ROOTDIR)/tests/mpeg1-research-decoder.c \
		gstcrystalhd-finish-trace.c -o "$@" $(MPEG1_TEST_LDFLAGS) \
		$(MPEG1_LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-audio-1.0) $(CRYSTALHD_CPU_FLAGS)

check-experimental-mpeg1-frontend: $(MPEG1_PLAYBACK)
	@registry=$$(mktemp); \
	trap 'rm -f "$$registry"' EXIT HUP INT TERM; \
	GST_REGISTRY="$$registry" GST_REGISTRY_1_0="$$registry" \
	GST_PLUGIN_PATH= GST_PLUGIN_PATH_1_0= \
	GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0= \
	LD_LIBRARY_PATH="$(abspath $(dir $(MPEG1_DIL))):$(LD_LIBRARY_PATH)" \
		"$(abspath $(MPEG1_PLAYBACK))" --self-test

check-experimental-mpeg1: $(MPEG1_PLUGIN) $(MPEG1_TEST) check-experimental-mpeg1-frontend
	@registry=$$(mktemp); \
	trap 'rm -f "$$registry"' EXIT HUP INT TERM; \
	GST_REGISTRY="$$registry" GST_REGISTRY_1_0="$$registry" \
	GST_PLUGIN_PATH= GST_PLUGIN_PATH_1_0= \
	GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0= \
	LD_LIBRARY_PATH="$(abspath $(dir $(MPEG1_DIL))):$(LD_LIBRARY_PATH)" \
		"$(abspath $(MPEG1_TEST))"

# Explicit installation affects only the separate rank-NONE plugin.
install-experimental-mpeg1: $(MPEG1_PLUGIN)
	@case "$(PLUGINDIR)" in /*) ;; *) \
		echo "ERROR: pass an absolute GStreamer PLUGINDIR for the optional MPEG-1 plugin" >&2; \
		exit 1;; esac
	install -D -m 0755 "$(MPEG1_PLUGIN)" "$(DESTDIR)$(PLUGINDIR)/libgstcrystalhdmpeg1experimental.so"

uninstall-experimental-mpeg1:
	@case "$(PLUGINDIR)" in /*) ;; *) \
		echo "ERROR: pass an absolute GStreamer PLUGINDIR for the optional MPEG-1 plugin" >&2; \
		exit 1;; esac
	rm -f -- "$(DESTDIR)$(PLUGINDIR)/libgstcrystalhdmpeg1experimental.so"

# Optional hardware fixture probe; libavformat supplies complete codec packets.
# FFmpeg development packages are not needed for the plugin itself.
codec-playback-test: gstreamer-codec-playback-test

gstreamer-codec-playback-test: $(ROOTDIR)/tests/gstreamer-codec-playback.cpp \
		$(ROOTDIR)/tests/phase1-progress.h $(ROOTDIR)/tests/ffmpeg-profile-compat.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror $< -o $@ \
		$(shell $(PKG_CONFIG) --cflags --libs $(CODEC_PLAYBACK_PACKAGES)) $(CRYSTALHD_CPU_FLAGS)

ifneq ($(HAVE_CODEC_PLAYBACK),)
check-codec-playback: gstreamer-codec-playback-test
	./gstreamer-codec-playback-test --self-test
else
check-codec-playback:
	@echo "Skipping optional codec probe self-test (FFmpeg/GStreamer app development packages unavailable)"
endif

$(PLUGIN): gstcrystalhd.c gstcrystalhd-codecs.h gstcrystalhd-timing.h gstcrystalhd-input.h \
		gstcrystalhd-diqos.c gstcrystalhd-diqos.h \
		gstcrystalhd-stage-trace.c gstcrystalhd-stage-trace.h \
		gstcrystalhd-finish-trace.c gstcrystalhd-finish-trace.h \
		$(ROOTDIR)/filters/crystalhd-h264-output.h \
		$(ROOTDIR)/filters/crystalhd-mpeg2-inspect.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< gstcrystalhd-diqos.c gstcrystalhd-stage-trace.c gstcrystalhd-finish-trace.c -o $@ $(LDFLAGS) $(LDLIBS) $(CRYSTALHD_CPU_FLAGS)

$(PLAYBACK_TEST): $(ROOTDIR)/tests/gstreamer-playback.c \
		$(ROOTDIR)/tests/phase1-progress.h
	$(CC) -Wall -Wextra -Werror $(CFLAGS) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES)) $(CRYSTALHD_CPU_FLAGS)

$(CONTROLS_TEST): $(ROOTDIR)/tests/gstreamer-controls.c \
		$(ROOTDIR)/tests/phase1-progress.h
	$(CC) -Wall -Wextra -Werror $(CFLAGS) \
		$(shell $(PKG_CONFIG) --cflags gstreamer-audio-1.0) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES) gstreamer-audio-1.0) -lm $(CRYSTALHD_CPU_FLAGS)

# Includes production source but supplies its own Dts mocks. This target must
# never link libcrystalhd or open the real decoder.
$(LIFECYCLE_LEGACY_TEST): CPPFLAGS += -DGST_CRYSTALHD_TEST_NO_POLL_API
$(LIFECYCLE_TEST) $(LIFECYCLE_LEGACY_TEST): $(ROOTDIR)/tests/gst-lifecycle.c gstcrystalhd.c \
		gstcrystalhd-codecs.h gstcrystalhd-timing.h gstcrystalhd-input.h \
		gstcrystalhd-diqos.c gstcrystalhd-diqos.h \
		gstcrystalhd-stage-trace.c gstcrystalhd-stage-trace.h \
		gstcrystalhd-finish-trace.c gstcrystalhd-finish-trace.h \
		$(ROOTDIR)/filters/crystalhd-h264-output.h \
		$(ROOTDIR)/filters/crystalhd-mpeg2-inspect.h
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror $(CPPFLAGS) $< gstcrystalhd-diqos.c gstcrystalhd-stage-trace.c gstcrystalhd-finish-trace.c -o $@ \
		$(shell $(PKG_CONFIG) --cflags --libs gstreamer-check-1.0 gstreamer-video-1.0) $(CRYSTALHD_CPU_FLAGS)

# Includes only the production helper and CPU fixtures, never libcrystalhd.
$(DI_QOS_TEST): $(ROOTDIR)/tests/gst-diqos.c gstcrystalhd-diqos.c gstcrystalhd-diqos.h
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror $< -o $@ \
		$(shell $(PKG_CONFIG) --cflags --libs gstreamer-check-1.0 gstreamer-video-1.0) $(CRYSTALHD_CPU_FLAGS)

check-diqos: $(DI_QOS_TEST)
	./$(DI_QOS_TEST)

check-stage-trace:
	CC="$(CC)" CFLAGS="$(CFLAGS) $(CRYSTALHD_CPU_FLAGS)" sh $(ROOTDIR)/tests/gst-stage-trace.sh

check-finish-trace:
	CC="$(CC)" CFLAGS="$(CFLAGS) $(CRYSTALHD_CPU_FLAGS)" sh $(ROOTDIR)/tests/gst-finish-trace.sh

check-player: $(PLUGIN)
	sh -n $(ROOTDIR)/scripts/crystalhd-play
	GST_PLUGIN_PATH_1_0=$(CURDIR) GST_PLUGIN_PATH=$(CURDIR) \
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) \
		$(PYTHON) -B $(ROOTDIR)/tests/gstreamer-player.test.py

$(CODECS_TEST): $(ROOTDIR)/tests/gst-codecs.c gstcrystalhd-codecs.h gstcrystalhd-timing.h \
		$(ROOTDIR)/filters/crystalhd-mpeg2-inspect.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. $< -o $@ \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES)) $(CRYSTALHD_CPU_FLAGS)

check: $(PLUGIN) $(PLAYBACK_TEST) $(CONTROLS_TEST) $(LIFECYCLE_TEST) $(LIFECYCLE_LEGACY_TEST) $(CODECS_TEST) \
		check-codec-playback check-player check-diqos check-stage-trace check-finish-trace
	@registry=$$(mktemp); \
	trap 'rm -f "$$registry"' EXIT HUP INT TERM; \
	GST_REGISTRY="$$registry" GST_PLUGIN_PATH=$(CURDIR) \
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) \
		gst-inspect-1.0 crystalhddec >/dev/null
	./$(PLAYBACK_TEST) --self-test
	./$(CONTROLS_TEST) --self-test
	./$(LIFECYCLE_TEST)
	./$(LIFECYCLE_LEGACY_TEST)
	@registry=$$(mktemp); \
	trap 'rm -f "$$registry"' EXIT HUP INT TERM; \
	GST_REGISTRY="$$registry" GST_PLUGIN_PATH=$(CURDIR) \
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) \
		./$(CODECS_TEST)

install: $(PLUGIN)
	@case "$(PLUGINDIR)" in /*) ;; *) \
		echo "ERROR: GStreamer plugin directory is unavailable; install its pkg-config metadata or pass an absolute PLUGINDIR" >&2; \
		exit 1;; esac
	install -D -m 0755 "$(PLUGIN)" "$(DESTDIR)$(PLUGINDIR)/$(PLUGIN)"
	$(MAKE) install-player

install-player:
	$(PYTHON) -B $(ROOTDIR)/scripts/install-player.py \
		--destdir "$(DESTDIR)" --bindir "$(BINDIR)" --datadir "$(DATADIR)"

uninstall:
	@case "$(PLUGINDIR)" in /*) ;; *) \
		echo "ERROR: GStreamer plugin directory is unavailable; install its pkg-config metadata or pass an absolute PLUGINDIR" >&2; \
		exit 1;; esac
	rm -f -- "$(DESTDIR)$(PLUGINDIR)/$(PLUGIN)"
	rm -f -- "$(DESTDIR)$(BINDIR)/crystalhd-play"
	rm -f -- "$(DESTDIR)$(DATADIR)/crystalhd/player.py"
	@rmdir -- "$(DESTDIR)$(DATADIR)/crystalhd" 2>/dev/null || true

clean:
	rm -f $(PLUGIN) $(PLAYBACK_TEST) $(CONTROLS_TEST) $(LIFECYCLE_TEST) $(LIFECYCLE_LEGACY_TEST) $(CODECS_TEST) $(DI_QOS_TEST) gstreamer-codec-playback-test $(CRYSTALHD_CPU_STAMP) $(CRYSTALHD_CPU_STAMP).tmp

include $(ROOTDIR)/cpu.mk
$(PLUGIN) $(PLAYBACK_TEST) $(CONTROLS_TEST) $(LIFECYCLE_TEST) $(LIFECYCLE_LEGACY_TEST) $(CODECS_TEST) $(DI_QOS_TEST) gstreamer-codec-playback-test: $(CRYSTALHD_BUILD_DEPS)
$(PLUGIN): $(ROOTDIR)/linux_lib/libcrystalhd/libcrystalhd.so
