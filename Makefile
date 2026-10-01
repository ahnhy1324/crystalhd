# SPDX-License-Identifier: LGPL-2.1-or-later

PREFIX ?= /usr
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

DRIVER_ARGS := KVER=$(KVER) KDIR=$(KDIR) DESTDIR=$(DESTDIR)
USER_ARGS := PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

.PHONY: all driver library library-check library-drain-test gstreamer vaapi examples browser uapi-check dma-check l0s-check command-pm-check fw-command-check fw-download-check tx-admission-check h264-stream-check rx-ownership-check device-lifetime-check v4l2-parent-check ioctl-dispatch-check architecture-check pib-check userspace32-check legacy-cpu-check phase1-check check install install-module install-runtime install-browser install-check uninstall uninstall-module uninstall-runtime uninstall-browser uninstall-check clean

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
	for lib_test in tx-ring flush tx-flush eos copy planar format input mpeg4-input input-format status color clock devmem device-handle capture fwload fw-version fwcmds; do \
		test_extra=; \
		test_sources="linux_lib/libcrystalhd/libcrystalhd_priv.cpp linux_lib/libcrystalhd/libcrystalhd_if.cpp"; \
		case $$lib_test in \
			copy|planar) test_wrap=; test_sources=linux_lib/libcrystalhd/libcrystalhd_int_if.cpp ;; \
			format) test_wrap=; test_sources="linux_lib/libcrystalhd/libcrystalhd_int_if.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp" ;; \
			status|color) test_wrap=-Wl,--wrap=ioctl; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_int_if.cpp ;; \
			clock) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep; \
				test_sources="linux_lib/libcrystalhd/libcrystalhd_int_if.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp" ;; \
			devmem) test_wrap=-Wl,--wrap=ioctl,--wrap=malloc,--wrap=free; \
				test_sources="linux_lib/libcrystalhd/libcrystalhd_int_if.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp" ;; \
			device-handle) test_wrap=-Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=malloc,--wrap=free,--wrap=posix_memalign; \
				test_wrap="$$test_wrap -Wl,--wrap=pthread_create,--wrap=shmget,--wrap=shmat,--wrap=shmdt,--wrap=shmctl"; \
				test_sources="linux_lib/libcrystalhd/libcrystalhd_if.cpp linux_lib/libcrystalhd/libcrystalhd_int_if.cpp"; \
				test_sources="$$test_sources linux_lib/libcrystalhd/libcrystalhd_fwcmds.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp"; \
				test_sources="$$test_sources linux_lib/libcrystalhd/libcrystalhd_fwdiag_if.cpp linux_lib/libcrystalhd/libcrystalhd_fwload_if.cpp linux_lib/libcrystalhd/libcrystalhd_parser.cpp" ;; \
			capture) test_wrap=-Wl,--wrap=ioctl,--wrap=close,--wrap=free,--wrap=pthread_mutex_lock; \
				test_wrap="$$test_wrap -Wl,--wrap=shmget,--wrap=shmdt,--wrap=shmctl" ;; \
			fwload) test_wrap=-Wl,--wrap=fopen,--wrap=fseek,--wrap=ftell,--wrap=fread,--wrap=__fread_chk,--wrap=fclose; \
				test_wrap="$$test_wrap -Wl,--wrap=malloc,--wrap=free,--wrap=perror"; \
				test_sources=linux_lib/libcrystalhd/libcrystalhd_fwload_if.cpp ;; \
			fw-version) test_wrap=-Wl,--wrap=fopen,--wrap=fseek,--wrap=ftell,--wrap=fread,--wrap=__fread_chk; \
				test_wrap="$$test_wrap -Wl,--wrap=fclose,--wrap=malloc,--wrap=free"; \
				test_sources=linux_lib/libcrystalhd/libcrystalhd_if.cpp ;; \
			fwcmds) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep; \
				test_sources="linux_lib/libcrystalhd/libcrystalhd_if.cpp linux_lib/libcrystalhd/libcrystalhd_fwcmds.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp" ;; \
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
library-drain-test: library tests/phase1-progress.h
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

fw-command-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/fw-command.sh

fw-download-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/fw-download.sh

tx-admission-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/tx-admission.sh

h264-stream-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/h264-stream.sh

rx-ownership-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/rx-ownership.sh

device-lifetime-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/device-lifetime.sh

v4l2-parent-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/v4l2-parent.sh

ioctl-dispatch-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/ioctl-dispatch.sh

architecture-check: command-pm-check fw-command-check fw-download-check tx-admission-check h264-stream-check rx-ownership-check device-lifetime-check v4l2-parent-check ioctl-dispatch-check

pib-check:
	CC="$(CC)" sh ./tests/flea-pib.sh
	CC="$(CC)" sh ./tests/link-pib.sh

userspace32-check:
	CXX="$(CXX)" sh ./tests/userspace32.sh

legacy-cpu-check:
	CXX="$(CXX)" sh ./tests/userspace32.sh --legacy

phase1-check: library-drain-test tests/phase1-oracle.tsv
	sh -n tests/kernel-log-check.sh tests/phase1-release-gate.sh
	$(CC) -std=c11 -Wall -Wextra -Werror -Itests -include phase1-progress.h \
		-fsyntax-only -x c /dev/null
	sh tests/kernel-log-check.sh --self-test
	LD_LIBRARY_PATH="$(CURDIR)/linux_lib/libcrystalhd" \
		./tests/library-drain-test --self-test
	sh tests/phase1-release-gate.sh --self-test
	sh tests/phase1-release-gate.sh manifest-check tests/phase1-oracle.tsv

check: uapi-check dma-check l0s-check architecture-check pib-check library-check all
	$(MAKE) -C filters/gst/gst-plugin-1.0 check
	$(MAKE) -C filters/vaapi check
	$(MAKE) -C browser check
	$(MAKE) phase1-check
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
