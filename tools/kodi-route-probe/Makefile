# SPDX-License-Identifier: GPL-2.0-or-later
CXX ?= c++
PKG_CONFIG ?= pkg-config
KODI_SDK_INCLUDE ?=
PACKAGES := libavformat libavcodec libavutil
ADDON := inputstream.crystalhd.routeprobe
OUTPUT := build/$(ADDON)
GST_PACKAGES := gstreamer-app-1.0 gstreamer-video-1.0
NATIVE_OUTPUT := build-native/$(ADDON)
TRACE_OUTPUT := build-trace/$(ADDON)
RGB_OUTPUT := build-rgb/$(ADDON)
PLAYBACK_OUTPUT := build-playback/$(ADDON)
PLAYBACK_TRACE := build-playback-trace/$(ADDON)

.PHONY: all native native-trace rgb-test playback playback-trace check check-codec check-demux check-playback clean
all: $(OUTPUT)/$(ADDON).so $(OUTPUT)/addon.xml
native: $(NATIVE_OUTPUT)/$(ADDON).so $(NATIVE_OUTPUT)/addon.xml
native-trace: $(TRACE_OUTPUT)/$(ADDON).so $(TRACE_OUTPUT)/addon.xml
rgb-test: $(RGB_OUTPUT)/$(ADDON).so $(RGB_OUTPUT)/addon.xml
playback: $(PLAYBACK_OUTPUT)/$(ADDON).so $(PLAYBACK_OUTPUT)/addon.xml
playback-trace: $(PLAYBACK_TRACE)/$(ADDON).so $(PLAYBACK_TRACE)/addon.xml
$(PLAYBACK_OUTPUT)/$(ADDON).so $(PLAYBACK_TRACE)/$(ADDON).so: CPPFLAGS += -DCRYSTALHD_KODI_PLAYBACK
$(PLAYBACK_TRACE)/$(ADDON).so: CPPFLAGS += -DCRYSTALHD_KODI_TRACE

$(OUTPUT)/$(ADDON).so: probe.cpp packed.h
	@test -n "$(KODI_SDK_INCLUDE)" || { echo 'Set KODI_SDK_INCLUDE to the unmodified Kodi 22 RC1 dev-kit include directory'; exit 1; }
	mkdir -p "$(OUTPUT)"
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -fPIC -shared \
		-isystem "$(KODI_SDK_INCLUDE)" $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) \
		$< $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -o "$@"

$(TRACE_OUTPUT)/$(ADDON).so: CPPFLAGS += -DCRYSTALHD_KODI_TRACE
$(RGB_OUTPUT)/$(ADDON).so: CPPFLAGS += -DCRYSTALHD_KODI_TRACE -DCRYSTALHD_KODI_RGB_TEST
$(RGB_OUTPUT)/$(ADDON).so: ../../tests/kodi-rgb-observer.h
$(NATIVE_OUTPUT)/$(ADDON).so $(TRACE_OUTPUT)/$(ADDON).so $(RGB_OUTPUT)/$(ADDON).so $(PLAYBACK_OUTPUT)/$(ADDON).so $(PLAYBACK_TRACE)/$(ADDON).so: probe.cpp native.cpp native.h packed.h input-progress.h playback.h
	@test -n "$(KODI_SDK_INCLUDE)" || { echo 'Set KODI_SDK_INCLUDE to the unmodified Kodi 22 RC1 dev-kit include directory'; exit 1; }
	mkdir -p "$(@D)"
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -fPIC -shared $(CPPFLAGS) -DCRYSTALHD_KODI_NATIVE \
		-isystem "$(KODI_SDK_INCLUDE)" $(shell $(PKG_CONFIG) --cflags $(PACKAGES) $(GST_PACKAGES)) \
		probe.cpp native.cpp $(shell $(PKG_CONFIG) --libs $(PACKAGES) $(GST_PACKAGES)) -o "$@"

$(OUTPUT)/addon.xml: addon.xml
	mkdir -p "$(@D)"
	cp "$<" "$@"

$(NATIVE_OUTPUT)/addon.xml $(TRACE_OUTPUT)/addon.xml $(RGB_OUTPUT)/addon.xml $(PLAYBACK_OUTPUT)/addon.xml $(PLAYBACK_TRACE)/addon.xml: addon-native.xml
	mkdir -p "$(@D)"
	cp "$<" "$@"

build/kodi-route-native-test: ../../tests/kodi-route-native.cpp native.cpp native.h packed.h
	mkdir -p "$(@D)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(CPPFLAGS) $(CXXFLAGS) -DCRYSTALHD_KODI_TEST \
		$(shell $(PKG_CONFIG) --cflags $(GST_PACKAGES)) ../../tests/kodi-route-native.cpp native.cpp \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES)) $(LDFLAGS) -o "$@"

check: build/kodi-route-native-test
	./build/kodi-route-native-test

build/kodi-route-codec-test: TEST_SOURCE := ../../tests/kodi-route-codec.cpp
build/kodi-route-demux-test: TEST_SOURCE := ../../tests/kodi-route-demux.cpp
build/kodi-route-codec-test build/kodi-route-demux-test: ../../tests/kodi-route-codec.cpp ../../tests/kodi-route-demux.cpp probe.cpp native.cpp native.h packed.h input-progress.h
	@test -n "$(KODI_SDK_INCLUDE)" || { echo 'Set KODI_SDK_INCLUDE for the callback contract test'; exit 1; }
	mkdir -p "$(@D)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(CPPFLAGS) $(CXXFLAGS) -DCRYSTALHD_KODI_NATIVE -DCRYSTALHD_KODI_TEST \
		-isystem "$(KODI_SDK_INCLUDE)" $(shell $(PKG_CONFIG) --cflags $(PACKAGES) $(GST_PACKAGES)) \
		$(TEST_SOURCE) native.cpp \
		$(shell $(PKG_CONFIG) --libs $(PACKAGES) $(GST_PACKAGES)) $(LDFLAGS) -o "$@"

check-codec: build/kodi-route-codec-test
	./build/kodi-route-codec-test

check-demux: build/kodi-route-demux-test
	@test -n "$(TEST_MP4)" || { echo 'Set TEST_MP4 to an absolute H264/AAC fixture path'; exit 1; }
	./build/kodi-route-demux-test "$(TEST_MP4)"

build/kodi-playback-test: ../../tests/kodi-playback.cpp playback.h native.cpp native.h packed.h
	mkdir -p "$(@D)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(CPPFLAGS) $(CXXFLAGS) -DCRYSTALHD_KODI_TEST \
		$(shell $(PKG_CONFIG) --cflags $(GST_PACKAGES)) ../../tests/kodi-playback.cpp native.cpp \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES)) $(LDFLAGS) -o "$@"

check-playback: build/kodi-playback-test
	./build/kodi-playback-test

clean:
	rm -rf build build-native build-trace build-rgb build-playback build-playback-trace
