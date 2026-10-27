# Experimental, explicit build only; no install target or automatic ranking.
CXX ?= c++
PACKAGES = gstreamer-video-1.0 xcb egl glesv2 gbm libdrm
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -fPIC -Wall -Wextra -Werror -pthread
CPPFLAGS += $(shell pkg-config --cflags $(PACKAGES))
LDLIBS += $(shell pkg-config --libs $(PACKAGES)) -pthread

all: libgstcrystalhdfield.so

# Explicit bounded diagnostic, never used by the ordinary all target.
.PHONY: wait-probe
wait-probe: wait-probe/libgstcrystalhdfield.so

wait-probe/libgstcrystalhdfield.so: ../../../tests/gst-field-wait-probe.cpp gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	mkdir -p wait-probe
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< $(LDFLAGS) $(LDLIBS) -o $@

libgstcrystalhdfield.so: gstcrystalhdfield.cpp ../../../tests/field-view-gpu.h ../../../tests/yuy2-gpu.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< $(LDLIBS) -o $@

clean:
	rm -f libgstcrystalhdfield.so wait-probe/libgstcrystalhdfield.so

.PHONY: all clean
