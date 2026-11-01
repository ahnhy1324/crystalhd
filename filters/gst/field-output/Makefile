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

clean:
	rm -f libgstcrystalhdfield.so wait-probe/libgstcrystalhdfield.so wait-probe/no-vsync/libgstcrystalhdfield.so wait-probe/libcrystalhdhostwait.so

.PHONY: all clean
