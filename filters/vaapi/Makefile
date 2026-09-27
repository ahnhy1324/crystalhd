# SPDX-License-Identifier: LGPL-2.1-or-later

CXX ?= g++
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr
DESTDIR ?=
ROOTDIR := ../..
DRIVER := crystalhd_drv_video.so
IMPORT_TEST := vaapi-import-test
H264_TEST := vaapi-h264-test
VPP_TEST := vaapi-vpp-test
REPLAY_TEST := vaapi-replay-test
BUFFER_TEST := vaapi-buffer-handles-test
CACHE_TEST := vaapi-cache-test
SCALING_TEST := vaapi-scaling-test
MPEG2_TEST := vaapi-mpeg2-test
MPEG2_REPLAY_TEST := vaapi-mpeg2-replay-test
MPEG2_SUBMIT_TEST := vaapi-mpeg2-submit-test
SEEK_TEST := vaapi-seek-test
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

.PHONY: all clean check install seek-test

all: $(DRIVER)

BACKEND_HEADERS := crystalhd-decode-replay.h crystalhd-mpeg2.h crystalhd-mpeg2-replay.h

$(DRIVER): crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS) $(CRYSTALHD_CPU_FLAGS)

$(IMPORT_TEST): $(ROOTDIR)/tests/vaapi-import.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror \
		$(shell $(PKG_CONFIG) --cflags libva libva-drm) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs libva libva-drm) $(CRYSTALHD_CPU_FLAGS)

$(H264_TEST): $(ROOTDIR)/tests/vaapi-h264.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(MPEG2_TEST): $(ROOTDIR)/tests/vaapi-mpeg2.cpp crystalhd-mpeg2.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(MPEG2_REPLAY_TEST): $(ROOTDIR)/tests/vaapi-mpeg2-replay.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(VPP_TEST): $(ROOTDIR)/tests/vaapi-vpp.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ \
		-L$(ROOTDIR)/linux_lib/libcrystalhd $(LDLIBS) \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=close $(CRYSTALHD_CPU_FLAGS)

$(REPLAY_TEST): $(ROOTDIR)/tests/vaapi-replay.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ \
		-L$(ROOTDIR)/linux_lib/libcrystalhd -Wl,--wrap=ioctl,--wrap=usleep \
		-Wl,--wrap=DtsDeviceOpen,--wrap=DtsDeviceClose,--wrap=DtsStopDecoder \
		-Wl,--wrap=DtsCloseDecoder,--wrap=DtsGetDriverStatus,--wrap=DtsTxFreeSize \
		-Wl,--wrap=DtsProcInput,--wrap=DtsFlushInput,--wrap=DtsProcOutputNoCopy \
		-Wl,--wrap=DtsReleaseOutputBuffs $(LDLIBS) $(CRYSTALHD_CPU_FLAGS)

$(BUFFER_TEST): $(ROOTDIR)/tests/vaapi-buffer-handles.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ \
		-L$(ROOTDIR)/linux_lib/libcrystalhd $(LDLIBS) \
		-Wl,--wrap=gbm_create_device,--wrap=gbm_device_destroy \
		-Wl,--wrap=gbm_bo_create,--wrap=gbm_bo_destroy,--wrap=gbm_bo_get_fd \
		-Wl,--wrap=gbm_bo_get_plane_count,--wrap=gbm_bo_get_modifier \
		-Wl,--wrap=gbm_bo_get_offset,--wrap=gbm_bo_get_stride \
		-Wl,--wrap=mmap,--wrap=munmap,--wrap=lseek,--wrap=close \
		-Wl,--wrap=ioctl,--wrap=poll,--wrap=__poll_chk,--wrap=DtsDeviceOpen $(CRYSTALHD_CPU_FLAGS)

# Production cache/fence sections, with unrelated frontend functions discarded.
# Disable test-only SLP constant pooling so GC can separate their vtables.
$(CACHE_TEST): $(ROOTDIR)/tests/vaapi-cache.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections,--wrap=open,--wrap=ioctl,--wrap=close \
		-pthread $(CRYSTALHD_CPU_FLAGS)

# Real H.264 submission/replay with decoder and graphics operations mocked.
$(SCALING_TEST): $(ROOTDIR)/tests/vaapi-scaling.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections -pthread $(CRYSTALHD_CPU_FLAGS)

$(MPEG2_SUBMIT_TEST): $(ROOTDIR)/tests/vaapi-mpeg2-submit.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections -pthread $(CRYSTALHD_CPU_FLAGS)

# Optional hardware lifecycle probe; keep FFmpeg development dependencies out
# of the normal driver build.
seek-test: $(SEEK_TEST)

$(SEEK_TEST): $(ROOTDIR)/tests/vaapi-seek.cpp
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror \
		$(shell $(PKG_CONFIG) --cflags libavcodec libavformat libavutil libva) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs libavcodec libavformat libavutil libva) $(CRYSTALHD_CPU_FLAGS)

check: $(DRIVER) $(IMPORT_TEST) $(H264_TEST) $(VPP_TEST) $(REPLAY_TEST) $(BUFFER_TEST) $(CACHE_TEST) $(SCALING_TEST) $(MPEG2_TEST) $(MPEG2_REPLAY_TEST) $(MPEG2_SUBMIT_TEST)
	readelf -Ws $(DRIVER) | grep -q '__vaDriverInit_'
	readelf -Ws $(DRIVER) | grep -q '__vaDriverInit_1_20'
	./$(H264_TEST)
	./$(MPEG2_TEST)
	./$(MPEG2_REPLAY_TEST)
	./$(MPEG2_SUBMIT_TEST)
	./$(CACHE_TEST)
	./$(SCALING_TEST)
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) ./$(VPP_TEST)
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) ./$(REPLAY_TEST)
	LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) ./$(BUFFER_TEST)
	@if test -e $(VAAPI_DRM_DEVICE); then \
		LIBVA_DRIVER_NAME=crystalhd LIBVA_DRIVERS_PATH=$(CURDIR) \
		LD_LIBRARY_PATH=$(abspath $(ROOTDIR)/linux_lib/libcrystalhd):$(LD_LIBRARY_PATH) \
		./$(IMPORT_TEST) $(VAAPI_DRM_DEVICE); \
	else \
		echo "VA-API import smoke test skipped: $(VAAPI_DRM_DEVICE) not found"; \
	fi

install: $(DRIVER)
	install -D -m 0755 $(DRIVER) $(DESTDIR)$(DRIVERDIR)/$(DRIVER)

clean:
	rm -f $(DRIVER) $(IMPORT_TEST) $(H264_TEST) $(SEEK_TEST) $(VPP_TEST) $(REPLAY_TEST) $(BUFFER_TEST) $(CACHE_TEST) $(SCALING_TEST) $(MPEG2_TEST) $(MPEG2_REPLAY_TEST) $(MPEG2_SUBMIT_TEST) $(CRYSTALHD_CPU_STAMP) $(CRYSTALHD_CPU_STAMP).tmp

include $(ROOTDIR)/cpu.mk
$(DRIVER) $(IMPORT_TEST) $(H264_TEST) $(SEEK_TEST) $(VPP_TEST) $(REPLAY_TEST) $(BUFFER_TEST) $(CACHE_TEST) $(SCALING_TEST) $(MPEG2_TEST) $(MPEG2_REPLAY_TEST) $(MPEG2_SUBMIT_TEST): $(CRYSTALHD_BUILD_DEPS)
$(DRIVER) $(VPP_TEST) $(REPLAY_TEST) $(BUFFER_TEST): $(ROOTDIR)/linux_lib/libcrystalhd/libcrystalhd.so
