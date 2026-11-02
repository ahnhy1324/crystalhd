# Experimental, explicit build only; no install target or automatic ranking.
CXX ?= c++
PACKAGES = gstreamer-video-1.0 xcb egl glesv2 gbm libdrm
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -fPIC -Wall -Wextra -Werror -pthread
CPPFLAGS += $(shell pkg-config --cflags $(PACKAGES))
LDLIBS += $(shell pkg-config --libs $(PACKAGES)) -pthread

all: libgstcrystalhdfield.so

# Explicit bounded diagnostic, never used by the ordinary all target.
.PHONY: wait-probe wait-probe-no-vsync host-wait-probe
wait-probe: wait-probe/libgstcrystalhdfield.so
wait-probe-no-vsync: wait-probe/no-vsync/libgstcrystalhdfield.so

wait-probe/libgstcrystalhdfield.so: ../../../tests/gst-field-wait-probe.cpp gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	mkdir -p wait-probe
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< $(LDFLAGS) $(LDLIBS) -ldl -o $@

wait-probe/no-vsync/libgstcrystalhdfield.so: ../../../tests/gst-field-wait-probe.cpp gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	mkdir -p wait-probe/no-vsync
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DCRYSTALHD_WAIT_SWAP_INTERVAL=0 -shared $< $(LDFLAGS) $(LDLIBS) -ldl -o $@

host-wait-probe: wait-probe/libcrystalhdhostwait.so

wait-probe/libcrystalhdhostwait.so: ../../../tests/gst-field-host-wait-probe.cpp
	mkdir -p wait-probe
	$(CXX) $(CPPFLAGS) $(shell pkg-config --cflags xcb-present) $(CXXFLAGS) -shared $< $(LDFLAGS) \
		-Wl,--no-as-needed $(shell pkg-config --libs xcb-present libdrm) -Wl,--as-needed -ldl -o $@

libgstcrystalhdfield.so: gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< $(LDLIBS) -o $@

field-output-cpu-test: ../../../tests/gst-field-output-cpu.cpp gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

field-output-cpu-test-asan: ../../../tests/gst-field-output-cpu.cpp gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie $< $(LDFLAGS) $(LDLIBS) -o $@

field-window-test: ../../../tests/gst-field-window.cpp gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) $(shell pkg-config --libs gstreamer-app-1.0) -o $@

field-nv12-test: ../../../tests/gst-field-nv12.cpp gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) $(shell pkg-config --libs gstreamer-app-1.0) -o $@

nv12-field-equivalence: ../../../tests/nv12-field-equivalence.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	$(CXX) $(shell pkg-config --cflags egl glesv2 gbm libdrm) $(CXXFLAGS) $< $(LDFLAGS) $(shell pkg-config --libs egl glesv2 gbm libdrm) -pthread -o $@

# Independent public-API closure control; no project or GStreamer inputs.
# Build and execution stay explicit, outside all/check and device-free CI.
egl-draw-closure-asan: ../../../tests/egl-draw-closure.cpp
	$(CXX) $(shell pkg-config --cflags egl glesv2 gbm) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie $< $(LDFLAGS) $(shell pkg-config --libs egl glesv2 gbm) -pthread -o $@

# NULL-only CPU contracts and plugin discovery; no display/device/worker starts.
# Compile the explicit X11/GPU regression too; running it remains opt-in.
check: libgstcrystalhdfield.so field-output-cpu-test field-output-cpu-test-asan field-window-test field-nv12-test nv12-field-equivalence
	@set -eu; check_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/crystalhd-field-check.XXXXXX"); \
	trap 'rm -rf -- "$$check_dir"' EXIT HUP INT TERM; \
	GST_PLUGIN_PATH= GST_PLUGIN_PATH_1_0= GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0= \
	GST_REGISTRY="$$check_dir/cpu.bin" ./field-output-cpu-test; \
	GST_PLUGIN_PATH= GST_PLUGIN_PATH_1_0= GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0= \
	GST_REGISTRY="$$check_dir/asan.bin" ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./field-output-cpu-test-asan; \
	LC_ALL=C GST_PLUGIN_PATH="$(CURDIR)" GST_PLUGIN_PATH_1_0="$(CURDIR)" \
	GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0= GST_REGISTRY="$$check_dir/inspect.bin" \
	gst-inspect-1.0 crystalhdfieldsink > "$$check_dir/inspect.txt"; \
	grep -Eq 'Rank[[:space:]]+none[[:space:]]+\(0\)' "$$check_dir/inspect.txt"; \
	grep -q 'field-stats' "$$check_dir/inspect.txt"

clean:
	rm -f libgstcrystalhdfield.so field-output-cpu-test field-output-cpu-test-asan field-window-test field-nv12-test nv12-field-equivalence egl-draw-closure-asan wait-probe/libgstcrystalhdfield.so wait-probe/no-vsync/libgstcrystalhdfield.so wait-probe/libcrystalhdhostwait.so

.PHONY: all check clean
