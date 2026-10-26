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

.PHONY: all native native-trace check check-codec clean
all: $(OUTPUT)/$(ADDON).so $(OUTPUT)/addon.xml
native: $(NATIVE_OUTPUT)/$(ADDON).so $(NATIVE_OUTPUT)/addon.xml
native-trace: $(TRACE_OUTPUT)/$(ADDON).so $(TRACE_OUTPUT)/addon.xml

$(OUTPUT)/$(ADDON).so: probe.cpp packed.h
	@test -n "$(KODI_SDK_INCLUDE)" || { echo 'Set KODI_SDK_INCLUDE to the unmodified Kodi 22 RC1 dev-kit include directory'; exit 1; }
	mkdir -p "$(OUTPUT)"
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -fPIC -shared \
		-isystem "$(KODI_SDK_INCLUDE)" $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) \
		$< $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -o "$@"

$(TRACE_OUTPUT)/$(ADDON).so: CPPFLAGS += -DCRYSTALHD_KODI_TRACE
$(NATIVE_OUTPUT)/$(ADDON).so $(TRACE_OUTPUT)/$(ADDON).so: probe.cpp native.cpp native.h packed.h
	@test -n "$(KODI_SDK_INCLUDE)" || { echo 'Set KODI_SDK_INCLUDE to the unmodified Kodi 22 RC1 dev-kit include directory'; exit 1; }
	mkdir -p "$(@D)"
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -fPIC -shared $(CPPFLAGS) -DCRYSTALHD_KODI_NATIVE \
		-isystem "$(KODI_SDK_INCLUDE)" $(shell $(PKG_CONFIG) --cflags $(PACKAGES) $(GST_PACKAGES)) \
		probe.cpp native.cpp $(shell $(PKG_CONFIG) --libs $(PACKAGES) $(GST_PACKAGES)) -o "$@"

$(OUTPUT)/addon.xml $(NATIVE_OUTPUT)/addon.xml $(TRACE_OUTPUT)/addon.xml: addon.xml
	mkdir -p "$(@D)"
	cp "$<" "$@"

build/kodi-route-native-test: ../../tests/kodi-route-native.cpp native.cpp native.h packed.h
	mkdir -p "$(@D)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(CPPFLAGS) $(CXXFLAGS) -DCRYSTALHD_KODI_TEST \
		$(shell $(PKG_CONFIG) --cflags $(GST_PACKAGES)) ../../tests/kodi-route-native.cpp native.cpp \
		$(shell $(PKG_CONFIG) --libs $(GST_PACKAGES)) $(LDFLAGS) -o "$@"

check: build/kodi-route-native-test
	./build/kodi-route-native-test

build/kodi-route-codec-test: ../../tests/kodi-route-codec.cpp probe.cpp native.cpp native.h packed.h
	@test -n "$(KODI_SDK_INCLUDE)" || { echo 'Set KODI_SDK_INCLUDE for the callback contract test'; exit 1; }
	mkdir -p "$(@D)"
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror $(CPPFLAGS) $(CXXFLAGS) -DCRYSTALHD_KODI_NATIVE \
		-isystem "$(KODI_SDK_INCLUDE)" $(shell $(PKG_CONFIG) --cflags $(PACKAGES) $(GST_PACKAGES)) \
		../../tests/kodi-route-codec.cpp native.cpp \
		$(shell $(PKG_CONFIG) --libs $(PACKAGES) $(GST_PACKAGES)) $(LDFLAGS) -o "$@"

check-codec: build/kodi-route-codec-test
	./build/kodi-route-codec-test

clean:
	rm -rf build build-native build-trace
