# Source-only optional plugin scaffold; root must qualify before media use.
CC ?= cc
PKG_CONFIG ?= pkg-config
ROOTDIR ?= ../../..
PACKAGES := gstreamer-video-1.0
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Werror -fPIC
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm

.PHONY: all clean check check-x11 check-sanitized
all: libgstcrystalhdpackedscale.so

libgstcrystalhdpackedscale.so: gstcrystalhdpackedscale.c packed-scale-engine.c $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared gstcrystalhdpackedscale.c packed-scale-engine.c $(LDFLAGS) $(LDLIBS) $(CRYSTALHD_CPU_FLAGS) -o $@

# Builds only. Invocation requires explicit --execute and remains CPU-only.
packed-scale-test: packed-scale-test.c packed-scale-engine.c $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-app-1.0) $(CFLAGS) packed-scale-test.c packed-scale-engine.c $(LDFLAGS) $(LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-app-1.0) $(CRYSTALHD_CPU_FLAGS) -o $@

check: all packed-scale-test
	sh ./check.sh "$(CURDIR)" "$(shell $(PKG_CONFIG) --variable=pluginsdir gstreamer-1.0)"

# Both instrumented images use a fresh private directory. No ordinary output
# or shared CPU-mode stamp is a prerequisite of this always-fresh build.
override PACKED_SANITIZER_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
check-sanitized: gstcrystalhdpackedscale.c packed-scale-test.c packed-scale-engine.c $(wildcard *.h)
	@set -eu; \
	packed_sanitizer_dir=$$(mktemp -d); \
	trap 'rm -rf -- "$$packed_sanitizer_dir"' EXIT; \
	trap 'exit 1' HUP INT TERM; \
	printf '%s\n' $(call crystalhd_quote,$(crystalhd_build_config)|SAN=$(PACKED_SANITIZER_FLAGS)|TEST_NONPIE=-fno-pie -no-pie|PKG_CONFIG=$(PKG_CONFIG)|APP_CFLAGS=$(shell $(PKG_CONFIG) --cflags gstreamer-app-1.0)|APP_LIBS=$(shell $(PKG_CONFIG) --libs gstreamer-app-1.0)|PLUGINS=$(shell $(PKG_CONFIG) --variable=pluginsdir gstreamer-1.0)) > "$$packed_sanitizer_dir/.crystalhd-sanitizer-config"; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared gstcrystalhdpackedscale.c packed-scale-engine.c $(LDFLAGS) $(LDLIBS) $(CRYSTALHD_CPU_FLAGS) $(PACKED_SANITIZER_FLAGS) -o "$$packed_sanitizer_dir/libgstcrystalhdpackedscale.so"; \
	$(CC) $(CPPFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-app-1.0) $(CFLAGS) packed-scale-test.c packed-scale-engine.c $(LDFLAGS) $(LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-app-1.0) $(CRYSTALHD_CPU_FLAGS) $(PACKED_SANITIZER_FLAGS) -fno-pie -no-pie -o "$$packed_sanitizer_dir/packed-scale-test"; \
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	LSAN_OPTIONS=exitcode=23 GST_REGISTRY_FORK=no \
		sh ./check.sh "$$packed_sanitizer_dir" "$(shell $(PKG_CONFIG) --variable=pluginsdir gstreamer-1.0)"

packed-scale-x11-contract: packed-scale-x11-contract.c
	$(CC) $(CPPFLAGS) $(shell $(PKG_CONFIG) --cflags gstreamer-app-1.0) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) $(shell $(PKG_CONFIG) --libs gstreamer-app-1.0) $(CRYSTALHD_CPU_FLAGS) -o $@

# Explicitly opens one temporary X11 window; no CrystalHD device or media.
check-x11: all packed-scale-x11-contract
	./packed-scale-x11-contract --execute-x11

clean:
	rm -f libgstcrystalhdpackedscale.so packed-scale-test packed-scale-x11-contract $(CRYSTALHD_CPU_STAMP) $(CRYSTALHD_CPU_STAMP).tmp

include $(ROOTDIR)/cpu.mk
libgstcrystalhdpackedscale.so packed-scale-test packed-scale-x11-contract: $(CRYSTALHD_BUILD_DEPS)
check-sanitized: $(CRYSTALHD_ROOT)/cpu.mk $(CRYSTALHD_ROOT)/include/crystalhd_legacy.h
