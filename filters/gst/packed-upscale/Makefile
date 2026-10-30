# Source-only optional plugin scaffold; root must qualify before media use.
CC ?= cc
PKG_CONFIG ?= pkg-config
ROOTDIR ?= ../../..
PACKAGES := gstreamer-video-1.0
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror -fPIC
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm

.PHONY: all clean check check-x11
all: libgstcrystalhdpackedscale.so

libgstcrystalhdpackedscale.so: gstcrystalhdpackedscale.c packed-scale-engine.c $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared gstcrystalhdpackedscale.c packed-scale-engine.c $(LDFLAGS) $(LDLIBS) $(CRYSTALHD_CPU_FLAGS) -o $@

# Builds only. Invocation requires explicit --execute and remains CPU-only.
packed-scale-test: packed-scale-test.c packed-scale-engine.c $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-app-1.0) $(CFLAGS) packed-scale-test.c packed-scale-engine.c $(LDFLAGS) $(LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-app-1.0) $(CRYSTALHD_CPU_FLAGS) -o $@

check: all packed-scale-test
	sh ./check.sh

packed-scale-x11-contract: packed-scale-x11-contract.c
	$(CC) $(CPPFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-app-1.0) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-app-1.0) $(CRYSTALHD_CPU_FLAGS) -o $@

# Explicitly opens one temporary X11 window; no CrystalHD device or media.
check-x11: all packed-scale-x11-contract
	./packed-scale-x11-contract --execute-x11

clean:
	rm -f libgstcrystalhdpackedscale.so packed-scale-test packed-scale-x11-contract $(CRYSTALHD_CPU_STAMP) $(CRYSTALHD_CPU_STAMP).tmp

include $(ROOTDIR)/cpu.mk
libgstcrystalhdpackedscale.so packed-scale-test packed-scale-x11-contract: $(CRYSTALHD_BUILD_DEPS)
