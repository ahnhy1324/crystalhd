# SPDX-License-Identifier: LGPL-2.1-or-later

CXX ?= g++
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr
DESTDIR ?=
ROOTDIR := ../..
DRIVER := crystalhd_drv_video.so
IMPORT_TEST := vaapi-import-test
DRIVERDIR ?= $(shell $(PKG_CONFIG) --variable=libdir libva)/dri
VAAPI_DRM_DEVICE ?= /dev/dri/renderD128

CPPFLAGS += -D__LINUX_USER__ -I$(ROOTDIR)/linux_lib/libcrystalhd \
	-I$(ROOTDIR)/include
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Werror -fPIC \
	$(shell $(PKG_CONFIG) --cflags libva libva-drm libdrm gbm \
	libswscale)
LDFLAGS += -shared -L$(ROOTDIR)/linux_lib/libcrystalhd
LDLIBS += $(shell $(PKG_CONFIG) --libs libva libva-drm libdrm gbm \
	libswscale) \
	-lcrystalhd -pthread

.PHONY: all clean check install

all: $(DRIVER)

$(DRIVER): crystalhd_drv_video.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(IMPORT_TEST): $(ROOTDIR)/tests/vaapi-import.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror \
		$(shell $(PKG_CONFIG) --cflags libva libva-drm) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs libva libva-drm)

check: $(DRIVER) $(IMPORT_TEST)
	readelf -Ws $(DRIVER) | grep -q '__vaDriverInit_'
	readelf -Ws $(DRIVER) | grep -q '__vaDriverInit_1_20'
	@if test -e $(VAAPI_DRM_DEVICE); then \
		LIBVA_DRIVER_NAME=crystalhd LIBVA_DRIVERS_PATH=$(CURDIR) \
		LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd) \
		./$(IMPORT_TEST) $(VAAPI_DRM_DEVICE); \
	else \
		echo "VA-API import smoke test skipped: $(VAAPI_DRM_DEVICE) not found"; \
	fi

install: $(DRIVER)
	install -D -m 0755 $(DRIVER) $(DESTDIR)$(DRIVERDIR)/$(DRIVER)

clean:
	rm -f $(DRIVER) $(IMPORT_TEST)
