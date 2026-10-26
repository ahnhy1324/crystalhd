# SPDX-License-Identifier: GPL-2.0-or-later
CXX ?= c++
PKG_CONFIG ?= pkg-config
KODI_SDK_INCLUDE ?=
PACKAGES := libavformat libavcodec libavutil
ADDON := inputstream.crystalhd.routeprobe
OUTPUT := build/$(ADDON)

.PHONY: all clean
all: $(OUTPUT)/$(ADDON).so $(OUTPUT)/addon.xml

$(OUTPUT)/$(ADDON).so: probe.cpp
	@test -n "$(KODI_SDK_INCLUDE)" || { echo 'Set KODI_SDK_INCLUDE to the unmodified Kodi 22 RC1 dev-kit include directory'; exit 1; }
	mkdir -p "$(OUTPUT)"
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -fPIC -shared \
		-isystem "$(KODI_SDK_INCLUDE)" $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) \
		$< $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -o "$@"

$(OUTPUT)/addon.xml: addon.xml
	mkdir -p "$(OUTPUT)"
	cp "$<" "$@"

clean:
	rm -rf build
