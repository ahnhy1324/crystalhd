# SPDX-License-Identifier: LGPL-2.1-or-later

PREFIX ?= /usr
DESTDIR ?=
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

DRIVER_ARGS := KVER=$(KVER) KDIR=$(KDIR) DESTDIR=$(DESTDIR)
USER_ARGS := PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

.PHONY: all driver library library-check gstreamer vaapi examples browser uapi-check dma-check userspace32-check check install clean

all: driver library gstreamer vaapi examples browser

driver:
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS)

library:
	$(MAKE) -C linux_lib/libcrystalhd

# Link only the tested production ring/accessor sections. No device, ioctl,
# firmware or shared-memory startup is reachable from this regression.
library-check:
	@set -eu; tx_test_dir=$$(mktemp -d /tmp/crystalhd-tx-ring-check.XXXXXX); \
	trap 'rm -f "$$tx_test_dir/check"; rmdir "$$tx_test_dir"' EXIT HUP INT TERM; \
	$(CXX) -std=c++11 -O1 -g -Wall -Werror \
		-ffunction-sections -fdata-sections -D__LINUX_USER__ \
		-Ilinux_lib/libcrystalhd -Iinclude -Iinclude/link \
		tests/library-tx-ring.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp \
		linux_lib/libcrystalhd/libcrystalhd_if.cpp \
		-Wl,--gc-sections -Wl,--wrap=pthread_mutex_lock -pthread \
		-o "$$tx_test_dir/check"; "$$tx_test_dir/check"

gstreamer: library
	$(MAKE) -C filters/gst/gst-plugin-1.0

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

userspace32-check:
	CXX="$(CXX)" sh ./tests/userspace32.sh

check: uapi-check dma-check library-check all
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
	$(MAKE) -C driver/linux -f Makefile.in KVER=$(KVER) KDIR=$(KDIR) clean
	$(MAKE) -C linux_lib/libcrystalhd clean
	$(MAKE) -C filters/gst/gst-plugin-1.0 clean
	$(MAKE) -C filters/vaapi clean
	$(MAKE) -C examples clean
