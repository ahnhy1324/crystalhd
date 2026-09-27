# SPDX-License-Identifier: LGPL-2.1-or-later

PREFIX ?= /usr
DESTDIR ?=
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

DRIVER_ARGS := KVER=$(KVER) KDIR=$(KDIR) DESTDIR=$(DESTDIR)
USER_ARGS := PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

.PHONY: all driver library library-check library-drain-test gstreamer vaapi examples browser uapi-check dma-check l0s-check userspace32-check check install clean

all: driver library gstreamer vaapi examples browser

driver:
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS)

library:
	$(MAKE) -C linux_lib/libcrystalhd

# Test production library sections without hardware. Firmware calls are
# stubbed and unexpected ioctls abort the flush/EOS regressions.
library-check:
	@set -eu; lib_test_dir=$$(mktemp -d /tmp/crystalhd-library-check.XXXXXX); \
	trap 'rm -f "$$lib_test_dir/check"; rmdir "$$lib_test_dir"' EXIT HUP INT TERM; \
	for lib_test in tx-ring flush tx-flush eos copy input status color; do \
		test_extra=; \
		test_sources="linux_lib/libcrystalhd/libcrystalhd_priv.cpp linux_lib/libcrystalhd/libcrystalhd_if.cpp"; \
		case $$lib_test in \
			copy) test_wrap=; test_sources=linux_lib/libcrystalhd/libcrystalhd_int_if.cpp ;; \
			status|color) test_wrap=-Wl,--wrap=ioctl; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_int_if.cpp ;; \
			input) test_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			tx-ring) test_wrap=-Wl,--wrap=pthread_mutex_lock ;; \
			flush) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_lock ;; \
			tx-flush) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_unlock; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsSetupHardware,--wrap=DtsOpenDecoder"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsReleaseInterface,--wrap=pthread_join,--wrap=_Z9WORD_SWAPt"; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			eos) test_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsSetupHardware,--wrap=DtsOpenDecoder"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture"; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
		esac; \
		$(CXX) -std=c++11 -O1 -g -Wall -Werror \
		-ffunction-sections -fdata-sections -D__LINUX_USER__ \
		-Ilinux_lib/libcrystalhd -Iinclude -Iinclude/link \
		tests/library-$$lib_test.cpp $$test_sources $$test_extra \
		-Wl,--gc-sections $$test_wrap -pthread \
		-o "$$lib_test_dir/check"; "$$lib_test_dir/check"; \
	done

gstreamer: library
	$(MAKE) -C filters/gst/gst-plugin-1.0

# Optional direct-library hardware probe. Building never opens the device;
# running requires explicit --hardware (or device-free --preflight).
library-drain-test: library
	$(CXX) -std=c++11 -O2 -g -Wall -Wextra -Werror -D__LINUX_USER__ \
		-Iinclude -Ilinux_lib/libcrystalhd tests/library-drain.cpp \
		$$(pkg-config --cflags --libs libavformat libavcodec libavutil glib-2.0) \
		-Llinux_lib/libcrystalhd -lcrystalhd -o tests/library-drain-test

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

userspace32-check:
	CXX="$(CXX)" sh ./tests/userspace32.sh

check: uapi-check dma-check l0s-check library-check all
	$(MAKE) -C filters/gst/gst-plugin-1.0 check
	$(MAKE) -C filters/vaapi check
	$(MAKE) -C browser check
	KVER=$(KVER) KDIR=$(KDIR) ./tests/staged-install.sh

install: all
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) install
	$(MAKE) -C linux_lib/libcrystalhd $(USER_ARGS) install
	$(MAKE) -C filters/gst/gst-plugin-1.0 $(USER_ARGS) install
	$(MAKE) -C filters/vaapi $(USER_ARGS) install
	$(MAKE) -C browser $(USER_ARGS) install

clean:
	rm -f tests/library-drain-test
	$(MAKE) -C driver/linux -f Makefile.in KVER=$(KVER) KDIR=$(KDIR) clean
	$(MAKE) -C linux_lib/libcrystalhd clean
	$(MAKE) -C filters/gst/gst-plugin-1.0 clean
	$(MAKE) -C filters/vaapi clean
	$(MAKE) -C examples clean
