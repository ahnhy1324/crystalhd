# SPDX-License-Identifier: LGPL-2.1-or-later

CXX ?= g++
PYTHON3 ?= python3
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr
DESTDIR ?=
ROOTDIR := ../..
DRIVER := crystalhd_drv_video.so
IMPORT_TEST := vaapi-import-test
H264_TEST := vaapi-h264-test
VPP_TEST := vaapi-vpp-test
REPLAY_TEST := vaapi-replay-test
REPLAY_CONTRACT_TEST := vaapi-replay-contract-test
BUFFER_TEST := vaapi-buffer-handles-test
CACHE_TEST := vaapi-cache-test
PRIVATE_BACKING_TEST := vaapi-private-backing-test
CONVERSION_TEST := vaapi-conversion-test
SCALING_TEST := vaapi-scaling-test
BOB_TEST := vaapi-bob-test
CENTERED_TEST := vaapi-centered-output-test
BOB_VPP_TEST := vaapi-bob-vpp-test
MPEG2_TEST := vaapi-mpeg2-test
MPEG2_REPLAY_TEST := vaapi-mpeg2-replay-test
MPEG2_SUBMIT_TEST := vaapi-mpeg2-submit-test
MPEG4_TEST := vaapi-mpeg4-test
MPEG4_SUBMIT_TEST := vaapi-mpeg4-submit-test
VC1_TEST := vaapi-vc1-test
WMV3_TEST := vaapi-wmv3-test
VC1_REPLAY_TEST := vaapi-vc1-replay-test
VC1_SUBMIT_TEST := vaapi-vc1-submit-test
SEEK_TEST := vaapi-seek-test
TRACE_TEST := vaapi-trace-test
VA_LIBDIR := $(shell $(PKG_CONFIG) --variable=libdir libva)
DRIVERDIR ?= $(if $(strip $(VA_LIBDIR)),$(VA_LIBDIR)/dri)
VAAPI_DRM_DEVICE ?= /dev/dri/renderD128

CPPFLAGS += -D__LINUX_USER__ -I$(ROOTDIR)/linux_lib/libcrystalhd \
	-I$(ROOTDIR)/include
CXXFLAGS ?= -O2
override CXXFLAGS += -std=c++17 -Wall -Wextra -Werror -fPIC \
	$(shell $(PKG_CONFIG) --cflags libva libva-drm libdrm gbm \
	libswscale)
CRYSTALHD_NORMAL_CPU_FLAGS := -msse2
LDFLAGS += -shared -Wl,--build-id=sha1 -L$(ROOTDIR)/linux_lib/libcrystalhd
LDLIBS += $(shell $(PKG_CONFIG) --libs libva libva-drm libdrm gbm \
	libswscale) \
	-lcrystalhd -pthread

.PHONY: all clean check check-ownership-sanitized payload-observer payload-observer-library check-payload-observer install uninstall seek-test

all: $(DRIVER)

BACKEND_HEADERS := $(ROOTDIR)/include/crystalhd-config.h \
	$(ROOTDIR)/include/crystalhd-config-schema.h \
	 ../crystalhd-h264-output.h \
	crystalhd-access-unit-limits.h crystalhd-decode-replay.h \
	crystalhd-centered-output.h \
	crystalhd-h264.h \
	crystalhd-private-backing.h \
	crystalhd-vpp-bob.h \
	crystalhd-mpeg2.h crystalhd-mpeg2-replay.h \
	crystalhd-mpeg4.h \
	crystalhd-trace.h crystalhd-vc1.h crystalhd-wmv3.h crystalhd-vc1-replay.h

$(DRIVER): crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS) $(CRYSTALHD_CPU_FLAGS)

# Deliberately separate, always fresh diagnostic backend. It is absent from
# all/check/install and never writes the normal crystalhd_drv_video.so target.
# Only this wrapper enables test-only bounded payload copies and output.
payload-observer: $(ROOTDIR)/tests/vaapi-vpp-payload-backend.cpp \
		$(ROOTDIR)/tests/vaapi-vpp-payload-observer.h crystalhd_drv_video.cpp \
		$(BACKEND_HEADERS) $(ROOTDIR)/cpu.mk $(ROOTDIR)/include/crystalhd_legacy.h \
		payload-observer-library
	mkdir -p payload-observer
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o payload-observer/$(DRIVER) \
		$(LDFLAGS) $(LDLIBS) $(CRYSTALHD_CPU_FLAGS)

# Resolve the ordinary OWN DIL in its own build directory, including the linker
# and SONAME links. This handles a clean tree without changing normal VA rules.
# Command-line toolchain/CPU settings propagate through the recursive make.
payload-observer-library:
	$(MAKE) -C $(ROOTDIR)/linux_lib/libcrystalhd all

# Explicit CPU-only diagnostic/model controls; normal check is unchanged.
check-payload-observer: $(ROOTDIR)/tests/vaapi-vpp-payload.cpp \
		$(ROOTDIR)/tests/vaapi-vpp-payload-observer.h \
		$(ROOTDIR)/tests/vaapi-bob-vpp.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	mkdir -p payload-observer
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o payload-observer/payload-cpu-test \
		-Wl,--gc-sections,--wrap=ioctl,--wrap=sws_scale \
		$(shell $(PKG_CONFIG) --libs gbm libdrm libswscale libavutil) \
		-pthread $(CRYSTALHD_CPU_FLAGS)
	CRYSTALHD_CONFIG_DISABLE=1 payload-observer/payload-cpu-test
	$(PYTHON3) -B $(ROOTDIR)/tests/vaapi-vpp-payload-reader.test.py

$(IMPORT_TEST): $(ROOTDIR)/tests/vaapi-import.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Werror \
		$(shell $(PKG_CONFIG) --cflags libva libva-drm) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs libva libva-drm) $(CRYSTALHD_CPU_FLAGS)

$(H264_TEST): $(ROOTDIR)/tests/vaapi-h264.cpp crystalhd-h264.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(MPEG2_TEST): $(ROOTDIR)/tests/vaapi-mpeg2.cpp crystalhd-mpeg2.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(MPEG2_REPLAY_TEST): $(ROOTDIR)/tests/vaapi-mpeg2-replay.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(VC1_TEST): $(ROOTDIR)/tests/vaapi-vc1.cpp crystalhd-vc1.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(WMV3_TEST): $(ROOTDIR)/tests/vaapi-wmv3.cpp crystalhd-wmv3.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(VC1_REPLAY_TEST): $(ROOTDIR)/tests/vaapi-vc1-replay.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(VPP_TEST): $(ROOTDIR)/tests/vaapi-vpp.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ \
		-L$(ROOTDIR)/linux_lib/libcrystalhd $(LDLIBS) \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=close,--wrap=eventfd \
		-Wl,--wrap=fstat,--wrap=drmGetDevice2,--wrap=drmFreeDevice \
		-Wl,--wrap=drmPrimeHandleToFD,--wrap=mmap,--wrap=mmap64,--wrap=munmap \
		-Wl,--wrap=gbm_bo_get_fd_for_plane,--wrap=gbm_bo_get_offset,--wrap=gbm_bo_get_modifier $(CRYSTALHD_CPU_FLAGS)

$(REPLAY_TEST): $(ROOTDIR)/tests/vaapi-replay.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ \
		-L$(ROOTDIR)/linux_lib/libcrystalhd -Wl,--wrap=ioctl,--wrap=usleep \
		-Wl,--wrap=poll,--wrap=__poll_chk,--wrap=read,--wrap=__read_chk \
		-Wl,--wrap=gbm_bo_get_plane_count,--wrap=gbm_bo_get_fd_for_plane \
		-Wl,--wrap=DtsDeviceOpen,--wrap=DtsDupOutputPollFd \
		-Wl,--wrap=DtsOpenDecoder,--wrap=DtsSetColorSpace \
		-Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture \
		-Wl,--wrap=DtsDeviceClose,--wrap=DtsStopDecoder \
		-Wl,--wrap=DtsCloseDecoder,--wrap=DtsCrystalHDVersion,--wrap=DtsSetInputFormat \
		-Wl,--wrap=DtsSetScaleParams \
		-Wl,--wrap=DtsGetDriverStatus,--wrap=DtsTxFreeSize \
		-Wl,--wrap=DtsIsEndOfStream \
		-Wl,--wrap=DtsProcInput,--wrap=DtsFlushInput,--wrap=DtsProcOutputNoCopy \
		-Wl,--wrap=DtsReleaseOutputBuffs $(LDLIBS) $(CRYSTALHD_CPU_FLAGS)

$(REPLAY_CONTRACT_TEST): $(ROOTDIR)/tests/vaapi-replay-contract.cpp \
		crystalhd-access-unit-limits.h crystalhd-decode-replay.h \
		crystalhd-mpeg2-replay.h crystalhd-vc1-replay.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(BUFFER_TEST): $(ROOTDIR)/tests/vaapi-buffer-handles.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ \
		-L$(ROOTDIR)/linux_lib/libcrystalhd $(LDLIBS) \
		-Wl,--wrap=gbm_create_device,--wrap=gbm_device_destroy \
		-Wl,--wrap=gbm_bo_create,--wrap=gbm_bo_destroy,--wrap=gbm_bo_get_fd \
		-Wl,--wrap=gbm_bo_get_plane_count,--wrap=gbm_bo_get_modifier \
		-Wl,--wrap=gbm_bo_get_offset,--wrap=gbm_bo_get_stride \
		-Wl,--wrap=mmap,--wrap=munmap,--wrap=lseek,--wrap=close \
		-Wl,--wrap=ioctl,--wrap=poll,--wrap=__poll_chk,--wrap=DtsDeviceOpen $(CRYSTALHD_CPU_FLAGS)

# Selected real public ownership callbacks, not the complete VA decode/VPP suite.
# The harness includes production VA code and guards every DIL call; only the
# system graphics/scaler link dependencies remain. Neither normal outputs nor
# the shared CPU-mode stamp are prerequisites of this always-fresh build.
override VA_OWNERSHIP_SANITIZER_FLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -fno-pie -no-pie
VA_OWNERSHIP_SANITIZER_LIBS = $(shell $(PKG_CONFIG) --libs gbm libdrm libswscale)
check-ownership-sanitized: $(ROOTDIR)/tests/vaapi-ownership-sanitized.cpp \
		$(ROOTDIR)/tests/vaapi-buffer-handles.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS) \
		$(ROOTDIR)/cpu.mk $(ROOTDIR)/include/crystalhd_legacy.h
	@set -eu; \
	if test "$(LEGACY_CPU)" != 0; then \
		printf '%s\n' 'check-ownership-sanitized requires LEGACY_CPU=0; legacy sanitizer coverage is unqualified' >&2; \
		exit 2; \
	fi; \
	va_ownership_sanitizer_dir=$$(mktemp -d); \
	trap 'rm -rf -- "$$va_ownership_sanitizer_dir"' EXIT; \
	trap 'exit 1' HUP INT TERM; \
	printf '%s\n' $(call crystalhd_quote,CXX=$(CXX)|CPPFLAGS=$(CPPFLAGS)|CXXFLAGS=$(CXXFLAGS)|CPU=$(CRYSTALHD_CPU_FLAGS)|SAN=$(VA_OWNERSHIP_SANITIZER_FLAGS)|PKG_CONFIG=$(PKG_CONFIG)|LIBS=$(VA_OWNERSHIP_SANITIZER_LIBS)|PTHREAD=-pthread) > "$$va_ownership_sanitizer_dir/.crystalhd-ownership-sanitizer-config"; \
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $(VA_OWNERSHIP_SANITIZER_FLAGS) \
		$(ROOTDIR)/tests/vaapi-ownership-sanitized.cpp \
		-o "$$va_ownership_sanitizer_dir/vaapi-buffer-handles-test" \
		$(VA_OWNERSHIP_SANITIZER_LIBS) -pthread \
		-Wl,--wrap=gbm_create_device,--wrap=gbm_device_destroy \
		-Wl,--wrap=gbm_bo_create,--wrap=gbm_bo_destroy,--wrap=gbm_bo_get_fd \
		-Wl,--wrap=gbm_bo_get_plane_count,--wrap=gbm_bo_get_modifier \
		-Wl,--wrap=gbm_bo_get_offset,--wrap=gbm_bo_get_stride \
		-Wl,--wrap=mmap,--wrap=munmap,--wrap=lseek,--wrap=close \
		-Wl,--wrap=ioctl,--wrap=poll,--wrap=__poll_chk,--wrap=DtsDeviceOpen; \
	CRYSTALHD_CONFIG_DISABLE=1 LD_LIBRARY_PATH= LD_PRELOAD= \
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	LSAN_OPTIONS=exitcode=23 \
		"$$va_ownership_sanitizer_dir/vaapi-buffer-handles-test"

# Production cache/fence sections, with unrelated frontend functions discarded.
# Disable test-only SLP constant pooling so GC can separate their vtables.
$(CACHE_TEST): $(ROOTDIR)/tests/vaapi-cache.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections,--wrap=open,--wrap=ioctl,--wrap=close \
		-pthread $(CRYSTALHD_CPU_FLAGS)

$(PRIVATE_BACKING_TEST): $(ROOTDIR)/tests/vaapi-private-backing.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections -pthread $(CRYSTALHD_CPU_FLAGS)

$(CONVERSION_TEST): $(ROOTDIR)/tests/vaapi-conversion.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections -pthread $(CRYSTALHD_CPU_FLAGS)

# Real H.264 submission/replay with decoder and graphics operations mocked.
$(SCALING_TEST): $(ROOTDIR)/tests/vaapi-scaling.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections $(shell $(PKG_CONFIG) --libs gbm libdrm) -pthread $(CRYSTALHD_CPU_FLAGS)

# Pure field-local sampling; no graphics, decoder or device dependency.
$(BOB_TEST): $(ROOTDIR)/tests/vaapi-bob.cpp crystalhd-vpp-bob.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(CRYSTALHD_CPU_FLAGS)

$(CENTERED_TEST): $(ROOTDIR)/tests/vaapi-centered-output.cpp crystalhd-centered-output.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(CRYSTALHD_CPU_FLAGS)

$(BOB_VPP_TEST): $(ROOTDIR)/tests/vaapi-bob-vpp.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections,--wrap=ioctl,--wrap=sws_scale \
		$(shell $(PKG_CONFIG) --libs gbm libdrm libswscale libavutil) \
		-pthread $(CRYSTALHD_CPU_FLAGS)

$(MPEG2_SUBMIT_TEST): $(ROOTDIR)/tests/vaapi-mpeg2-submit.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections $(shell $(PKG_CONFIG) --libs gbm libdrm) -pthread $(CRYSTALHD_CPU_FLAGS)

$(MPEG4_TEST): $(ROOTDIR)/tests/vaapi-mpeg4.cpp crystalhd-mpeg4.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CRYSTALHD_CPU_FLAGS) $< -o $@

$(MPEG4_SUBMIT_TEST): $(ROOTDIR)/tests/vaapi-mpeg4-submit.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections $(shell $(PKG_CONFIG) --libs gbm libdrm) -pthread $(CRYSTALHD_CPU_FLAGS)

$(VC1_SUBMIT_TEST): $(ROOTDIR)/tests/vaapi-vc1-submit.cpp crystalhd_drv_video.cpp $(BACKEND_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-tree-slp-vectorize \
		-ffunction-sections -fdata-sections $< -o $@ \
		-Wl,--gc-sections $(shell $(PKG_CONFIG) --libs gbm libdrm) -pthread $(CRYSTALHD_CPU_FLAGS)

$(TRACE_TEST): $(ROOTDIR)/tests/vaapi-trace.cpp crystalhd-trace.h $(ROOTDIR)/include/crystalhd-config.h $(ROOTDIR)/include/crystalhd-config-schema.h
	$(CXX) -std=c++17 -Wall -Wextra -Werror $< -o $@ -pthread

# Optional hardware lifecycle probe; keep FFmpeg development dependencies out
# of the normal driver build.
seek-test: $(SEEK_TEST)

$(SEEK_TEST): $(ROOTDIR)/tests/vaapi-seek.cpp $(ROOTDIR)/tests/ffmpeg-profile-compat.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror \
		$(shell $(PKG_CONFIG) --cflags libavcodec libavformat libavutil libva) $< -o $@ \
		$(shell $(PKG_CONFIG) --libs libavcodec libavformat libavutil libva) $(CRYSTALHD_CPU_FLAGS)

# Unit fixtures exercise explicit native controls, independent of host INI files.
check: export CRYSTALHD_CONFIG_DISABLE = 1
check: $(DRIVER) $(IMPORT_TEST) $(H264_TEST) $(VPP_TEST) $(REPLAY_TEST) $(REPLAY_CONTRACT_TEST) $(BUFFER_TEST) $(CACHE_TEST) $(PRIVATE_BACKING_TEST) $(CONVERSION_TEST) $(SCALING_TEST) $(BOB_TEST) $(MPEG2_TEST) $(MPEG2_REPLAY_TEST) $(MPEG2_SUBMIT_TEST) $(MPEG4_TEST) $(MPEG4_SUBMIT_TEST) $(VC1_TEST) $(WMV3_TEST) $(VC1_REPLAY_TEST) $(VC1_SUBMIT_TEST) $(TRACE_TEST)
	readelf -Ws $(DRIVER) | grep -q '__vaDriverInit_'
	readelf -Ws $(DRIVER) | grep -q '__vaDriverInit_1_20'
	./$(H264_TEST)
	./$(REPLAY_CONTRACT_TEST)
	./$(MPEG2_TEST)
	./$(MPEG2_REPLAY_TEST)
	./$(MPEG2_SUBMIT_TEST)
	./$(MPEG4_TEST)
	./$(MPEG4_SUBMIT_TEST)
	./$(VC1_TEST)
	./$(WMV3_TEST)
	./$(VC1_REPLAY_TEST)
	./$(VC1_SUBMIT_TEST)
	./$(TRACE_TEST) | $(PYTHON3) $(ROOTDIR)/tests/vaapi-trace.test.py
	./$(CACHE_TEST)
	./$(PRIVATE_BACKING_TEST)
	./$(CONVERSION_TEST)
	./$(SCALING_TEST)
	./$(BOB_TEST)
	./$(CENTERED_TEST)
	./$(BOB_VPP_TEST)
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
	@case "$(DRIVERDIR)" in /*) ;; *) \
		echo "ERROR: VA driver directory is unavailable; install libva pkg-config metadata or pass an absolute DRIVERDIR" >&2; \
		exit 1;; esac
	install -D -m 0755 $(DRIVER) $(DESTDIR)$(DRIVERDIR)/$(DRIVER)

uninstall:
	@case "$(DRIVERDIR)" in /*) ;; *) \
		echo "ERROR: VA driver directory is unavailable; install libva pkg-config metadata or pass an absolute DRIVERDIR" >&2; \
		exit 1;; esac
	rm -f -- "$(DESTDIR)$(DRIVERDIR)/$(DRIVER)"

clean:
	rm -f $(BOB_VPP_TEST) $(CENTERED_TEST)
	rm -f $(DRIVER) $(IMPORT_TEST) $(H264_TEST) $(SEEK_TEST) $(VPP_TEST) $(REPLAY_TEST) $(REPLAY_CONTRACT_TEST) $(BUFFER_TEST) $(CACHE_TEST) $(PRIVATE_BACKING_TEST) $(CONVERSION_TEST) $(SCALING_TEST) $(BOB_TEST) $(MPEG2_TEST) $(MPEG2_REPLAY_TEST) $(MPEG2_SUBMIT_TEST) $(MPEG4_TEST) $(MPEG4_SUBMIT_TEST) $(VC1_TEST) $(WMV3_TEST) $(VC1_REPLAY_TEST) $(VC1_SUBMIT_TEST) $(TRACE_TEST) $(CRYSTALHD_CPU_STAMP) $(CRYSTALHD_CPU_STAMP).tmp

include $(ROOTDIR)/cpu.mk
check: $(BOB_VPP_TEST) $(CENTERED_TEST)
$(CENTERED_TEST): $(CRYSTALHD_BUILD_DEPS)
$(BOB_VPP_TEST): $(CRYSTALHD_BUILD_DEPS)
$(DRIVER) $(IMPORT_TEST) $(H264_TEST) $(SEEK_TEST) $(VPP_TEST) $(REPLAY_TEST) $(REPLAY_CONTRACT_TEST) $(BUFFER_TEST) $(CACHE_TEST) $(PRIVATE_BACKING_TEST) $(CONVERSION_TEST) $(SCALING_TEST) $(BOB_TEST) $(MPEG2_TEST) $(MPEG2_REPLAY_TEST) $(MPEG2_SUBMIT_TEST) $(MPEG4_TEST) $(MPEG4_SUBMIT_TEST) $(VC1_TEST) $(WMV3_TEST) $(VC1_REPLAY_TEST) $(VC1_SUBMIT_TEST) $(TRACE_TEST): $(CRYSTALHD_BUILD_DEPS)
$(DRIVER) $(VPP_TEST) $(REPLAY_TEST) $(BUFFER_TEST): $(ROOTDIR)/linux_lib/libcrystalhd/libcrystalhd.so

.PHONY: payload-observer-clean
payload-observer-clean:
	rm -f payload-observer/crystalhd_drv_video.so payload-observer/payload-cpu-test
	@rmdir payload-observer 2>/dev/null || true
clean: payload-observer-clean
