# SPDX-License-Identifier: LGPL-2.1-or-later

PREFIX ?= /usr
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

DRIVER_ARGS := KVER=$(KVER) KDIR=$(KDIR) DESTDIR=$(DESTDIR)
USER_ARGS := PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

.PHONY: all driver library library-check library-drain-test gstreamer vaapi examples browser uapi-check dma-check l0s-check command-pm-check pib-check userspace32-check legacy-cpu-check check install install-module install-runtime install-browser install-check uninstall uninstall-module uninstall-runtime uninstall-browser uninstall-check clean

all: driver library gstreamer vaapi examples

driver:
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS)

library:
	$(MAKE) -C linux_lib/libcrystalhd

# Test production library sections without hardware. Firmware calls are
# stubbed and unexpected ioctls abort the flush/EOS regressions.
library-check:
	@set -eu; lib_test_dir=$$(mktemp -d /tmp/crystalhd-library-check.XXXXXX); \
	trap 'rm -f "$$lib_test_dir/check"; rmdir "$$lib_test_dir"' EXIT HUP INT TERM; \
	for lib_test in tx-ring flush tx-flush eos copy planar format input mpeg4-input input-format status color; do \
		test_extra=; \
		test_sources="linux_lib/libcrystalhd/libcrystalhd_priv.cpp linux_lib/libcrystalhd/libcrystalhd_if.cpp"; \
		case $$lib_test in \
			copy|planar) test_wrap=; test_sources=linux_lib/libcrystalhd/libcrystalhd_int_if.cpp ;; \
			format) test_wrap=; test_sources="linux_lib/libcrystalhd/libcrystalhd_int_if.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp" ;; \
			status|color) test_wrap=-Wl,--wrap=ioctl; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_int_if.cpp ;; \
			input|mpeg4-input) test_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			input-format) test_wrap=-Wl,--wrap=ioctl,--wrap=malloc,--wrap=free,--wrap=posix_memalign; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			tx-ring) test_wrap=-Wl,--wrap=pthread_mutex_lock ;; \
			flush) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_lock ;; \
			tx-flush) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_unlock; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsSetupHardware,--wrap=DtsOpenDecoder"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsReleaseInterface,--wrap=pthread_join,--wrap=_Z9WORD_SWAPt"; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			eos) test_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep,--wrap=clock_gettime; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsSetupHardware,--wrap=DtsOpenDecoder"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture"; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
		esac; \
		$(CXX) -std=c++11 -O1 -g -Wall -Werror \
		-ffunction-sections -fdata-sections -D__LINUX_USER__ \
		-Ilinux_lib/libcrystalhd -Iinclude -Iinclude/link \
		tests/library-$$lib_test.cpp $$test_sources $$test_extra \
		-Wl,--gc-sections $$test_wrap -pthread -lrt \
		$(CRYSTALHD_CPU_FLAGS) -o "$$lib_test_dir/check"; "$$lib_test_dir/check"; \
	done

gstreamer: library
	$(MAKE) -C filters/gst/gst-plugin-1.0

# Optional direct-library hardware probe. Building never opens the device;
# running requires explicit --hardware (or device-free --preflight).
library-drain-test: library
	$(CXX) -std=c++11 -O2 -g -Wall -Wextra -Werror -D__LINUX_USER__ \
		-Iinclude -Ilinux_lib/libcrystalhd tests/library-drain.cpp \
		$$(pkg-config --cflags --libs libavformat libavcodec libavutil glib-2.0) \
		-Llinux_lib/libcrystalhd -lcrystalhd $(CRYSTALHD_CPU_FLAGS) -o tests/library-drain-test

vaapi: library
	$(MAKE) -C filters/vaapi

examples: library
	$(MAKE) -C examples

browser:
	$(MAKE) -C browser

uapi-check:
	sh ./tests/uapi-abi.sh

dma-check:
	sh ./tests/dma-descriptors.sh

l0s-check:
	CC="$(CC)" sh ./tests/l0s-workaround.sh

command-pm-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/command-pm.sh

pib-check:
	CC="$(CC)" sh ./tests/flea-pib.sh

userspace32-check:
	CXX="$(CXX)" sh ./tests/userspace32.sh

legacy-cpu-check:
	CXX="$(CXX)" sh ./tests/userspace32.sh --legacy

check: uapi-check dma-check l0s-check command-pm-check pib-check library-check all
	$(MAKE) -C filters/gst/gst-plugin-1.0 check
	$(MAKE) -C filters/vaapi check
	$(MAKE) -C browser check
	sh -n scripts/crystalhd-check
	./tests/crystalhd-check.sh
	KVER=$(KVER) KDIR=$(KDIR) ./tests/staged-install.sh

install: install-module install-runtime

install-module: driver
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) install-module

install-runtime: library gstreamer vaapi
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) install-udev
	$(MAKE) -C firmware DESTDIR=$(DESTDIR) install
	$(MAKE) -C linux_lib/libcrystalhd $(USER_ARGS) install
	$(MAKE) -C filters/gst/gst-plugin-1.0 $(USER_ARGS) install
	$(MAKE) -C filters/vaapi $(USER_ARGS) install
	$(MAKE) install-check

install-browser: browser
	$(MAKE) -C browser $(USER_ARGS) install

install-check:
	install -D -m 0755 scripts/crystalhd-check "$(DESTDIR)$(BINDIR)/crystalhd-check"

uninstall:
	$(MAKE) uninstall-runtime
	$(MAKE) uninstall-module

uninstall-module:
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) uninstall-module

uninstall-runtime:
	$(MAKE) uninstall-check
	$(MAKE) -C filters/vaapi $(USER_ARGS) uninstall
	$(MAKE) -C filters/gst/gst-plugin-1.0 $(USER_ARGS) uninstall
	$(MAKE) -C linux_lib/libcrystalhd $(USER_ARGS) uninstall
	$(MAKE) -C firmware DESTDIR=$(DESTDIR) uninstall
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) uninstall-udev

uninstall-browser:
	$(MAKE) -C browser $(USER_ARGS) uninstall

uninstall-check:
	rm -f -- "$(DESTDIR)$(BINDIR)/crystalhd-check"

clean:
	rm -f tests/library-drain-test
	$(MAKE) -C driver/linux -f Makefile.in KVER=$(KVER) KDIR=$(KDIR) clean
	$(MAKE) -C linux_lib/libcrystalhd clean
	$(MAKE) -C filters/gst/gst-plugin-1.0 clean
	$(MAKE) -C filters/vaapi clean
	$(MAKE) -C examples clean

include cpu.mk
