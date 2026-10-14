# SPDX-License-Identifier: LGPL-2.1-or-later

PREFIX ?= /usr
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PYTHON3 ?= python3

DRIVER_ARGS := KVER=$(KVER) KDIR=$(KDIR) DESTDIR=$(DESTDIR)
USER_ARGS := PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

.PHONY: all driver library library-check raw-frame-check library-drain-test gstreamer vaapi examples browser uapi-check dma-check l0s-check command-pm-check fw-command-check fw-command-observer-check fw-download-check fw-research-check fw-qemu-check fw-probe-check fw-probe-tool tx-admission-check h264-stream-check rx-ownership-check rx-receipt-check rx-output-receipts-check flea-rx-metadata-check flea-dram-check flea-power-check device-lifetime-check v4l2-parent-check ioctl-dispatch-check architecture-check pib-check userspace32-check legacy-cpu-check phase1-check check install install-module install-runtime install-browser install-check uninstall uninstall-module uninstall-runtime uninstall-browser uninstall-check clean

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
	for lib_test in tx-ring flush tx-flush eos copy planar format input mpeg4-input input-format status color clock devmem device-handle poll-fd capture fwload fw-version fwcmds raw-frame; do \
		test_extra=; \
		test_sources="linux_lib/libcrystalhd/libcrystalhd_priv.cpp linux_lib/libcrystalhd/libcrystalhd_if.cpp"; \
		case $$lib_test in \
			raw-frame) test_wrap=-Wl,--wrap=calloc; test_sources=linux_lib/libcrystalhd/libcrystalhd_raw_frame.cpp ;; \
			copy|planar) test_wrap=; test_sources=linux_lib/libcrystalhd/libcrystalhd_int_if.cpp ;; \
			format) test_wrap=; test_sources="linux_lib/libcrystalhd/libcrystalhd_int_if.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp linux_lib/libcrystalhd/libcrystalhd_if.cpp" ;; \
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
			poll-fd) test_wrap=-Wl,--wrap=ioctl,--wrap=fcntl ;; \
			capture) test_wrap=-Wl,--wrap=ioctl,--wrap=close,--wrap=free,--wrap=pthread_mutex_lock; \
				test_wrap="$$test_wrap -Wl,--wrap=shmget,--wrap=shmdt,--wrap=shmctl" ;; \
			fwload) test_wrap=-Wl,--wrap=fopen,--wrap=fseek,--wrap=ftell,--wrap=fread,--wrap=__fread_chk,--wrap=fclose; \
				test_wrap="$$test_wrap -Wl,--wrap=malloc,--wrap=free,--wrap=perror"; \
				test_sources=linux_lib/libcrystalhd/libcrystalhd_fwload_if.cpp ;; \
			fw-version) test_wrap=-Wl,--wrap=fopen,--wrap=fseek,--wrap=ftell,--wrap=fread,--wrap=__fread_chk; \
				test_wrap="$$test_wrap -Wl,--wrap=fclose,--wrap=malloc,--wrap=free,--wrap=ioctl"; \
				test_sources=linux_lib/libcrystalhd/libcrystalhd_if.cpp; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_fwcmds.cpp ;; \
			fwcmds) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=DtsAllocIoctlData; \
				test_sources="linux_lib/libcrystalhd/libcrystalhd_if.cpp linux_lib/libcrystalhd/libcrystalhd_fwcmds.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp" ;; \
			input) test_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep,--wrap=posix_memalign; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			mpeg4-input) test_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			input-format) test_wrap=-Wl,--wrap=ioctl,--wrap=malloc,--wrap=free,--wrap=posix_memalign; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			tx-ring) test_wrap=-Wl,--wrap=pthread_mutex_lock ;; \
			flush) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_lock; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsReleaseInterface,--wrap=DtsFinishOutputCall,--wrap=DtsPublishOutput"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsCommitFetchedOutput,--wrap=DtsPublishCopiedOutput" ;; \
			tx-flush) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_unlock; \
				test_wrap="$$test_wrap -Wl,--wrap=_Z14DtsCacheSpsPpsPvPKhj"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsSetupHardware,--wrap=DtsOpenDecoder"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsFlushRxCapture"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsRecoveryFlushInputRing,--wrap=DtsRecoveryRestoreInputState"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsSetFleaIn422Mode,--wrap=DtsSetLinkIn422Mode"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsReleaseInterface,--wrap=pthread_join,--wrap=_Z9WORD_SWAPt"; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_parser.cpp ;; \
			eos) test_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep,--wrap=clock_gettime,--wrap=pthread_cond_timedwait; \
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

raw-frame-check:
	CC="$(CC)" CXX="$(CXX)" sh ./tests/library-raw-frame.sh

# Optional direct-library hardware probe. Building never opens the device;
# running requires explicit --hardware (or device-free --preflight).
library-drain-test: library tests/phase1-progress.h tests/issue204-active-context.h tests/issue204-generation-observer.h tests/issue399-firmware-drop.h tests/flea-power-observer.h tests/rx-output-receipts.h tests/rx-output-receipts.cpp
	$(CXX) -std=c++11 -O2 -g -Wall -Wextra -Werror -D__LINUX_USER__ \
		-Iinclude -Ilinux_lib/libcrystalhd tests/library-drain.cpp \
		tests/rx-output-receipts.cpp \
		$$(pkg-config --cflags --libs libavformat libavcodec libavutil glib-2.0 gstreamer-1.0) \
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

fw-command-observer-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/fw-command-observer.sh

fw-download-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/fw-download.sh

.PHONY: fw-staging-check
fw-staging-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/fw-staging.sh

fw-qemu-check:
	$(PYTHON3) -B tests/fw-qemu.py

fw-research-check:
	$(PYTHON3) -B tests/fw-research.py

fw-probe-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" PYTHON3="$(PYTHON3)" sh ./tests/fw-probe.sh

# Optional tool; building it does not open a device. Live actions require
# an explicit reset acknowledgement and an expected device generation.
fw-probe-tool:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(CFLAGS) -Iinclude \
		tools/fw-research/flea_fw_probe.c -o tools/fw-research/flea-fw-probe

tx-admission-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/tx-admission.sh

h264-stream-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/h264-stream.sh

.PHONY: h264-reference-check
h264-reference-check:
	$(PYTHON3) -B tests/h264-reference-lifecycle.py --self-test

# Device-free, true-1080p60 source/encode/hash admission for issue #399.
.PHONY: issue399-baseline-check
issue399-baseline-check:
	CRYSTALHD_ISSUE399_FFMPEG=1 $(PYTHON3) -B tests/issue399-baseline.test.py

rx-ownership-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/rx-ownership.sh

rx-receipt-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/rx-receipt.sh

flea-rx-metadata-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/flea-rx-metadata.sh

flea-dram-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/flea-dram.sh

flea-power-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/flea-power.sh

device-lifetime-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/device-lifetime.sh

v4l2-parent-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/v4l2-parent.sh

ioctl-dispatch-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/ioctl-dispatch.sh

rx-output-receipts-check:
	CXX="$(CXX)" sh ./tests/rx-output-receipts.sh

architecture-check: command-pm-check fw-command-check fw-command-observer-check fw-download-check fw-staging-check fw-probe-check tx-admission-check h264-stream-check rx-ownership-check rx-receipt-check rx-output-receipts-check flea-rx-metadata-check flea-dram-check flea-power-check device-lifetime-check v4l2-parent-check ioctl-dispatch-check

pib-check:
	CC="$(CC)" sh ./tests/flea-pib.sh
	CC="$(CC)" sh ./tests/link-pib.sh

userspace32-check:
	CXX="$(CXX)" sh ./tests/userspace32.sh

legacy-cpu-check:
	CXX="$(CXX)" sh ./tests/userspace32.sh --legacy

phase1-check: library-drain-test tests/phase1-oracle.tsv tests/maintenance-oracle.tsv
	sh -n tests/kernel-log-check.sh tests/phase1-release-gate.sh
	$(CC) -std=c11 -Wall -Wextra -Werror -Itests -include phase1-progress.h \
		-fsyntax-only -x c /dev/null
	sh tests/kernel-log-check.sh --self-test
	LD_LIBRARY_PATH="$(CURDIR)/linux_lib/libcrystalhd" \
		./tests/library-drain-test --self-test
	sh tests/phase1-release-gate.sh --self-test
	sh tests/phase1-release-gate.sh manifest-check tests/phase1-oracle.tsv
	sh tests/phase1-release-gate.sh manifest-check tests/maintenance-oracle.tsv

check: uapi-check dma-check l0s-check architecture-check pib-check library-check raw-frame-check h264-reference-check fw-research-check all
	$(MAKE) -C filters/gst/gst-plugin-1.0 check
	$(MAKE) -C filters/vaapi check
	$(MAKE) -C browser check
	$(MAKE) phase1-check
	sh -n scripts/crystalhd-check
	./tests/crystalhd-check.sh
	sh ./tests/install-preflight.sh
	KVER=$(KVER) KDIR=$(KDIR) ./tests/staged-install.sh

install: install-preflight
	$(MAKE) install-module
	$(MAKE) install-runtime

.PHONY: install-preflight
install-preflight:
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) install-preflight

install-module: install-preflight driver
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) install-module

install-runtime: library gstreamer vaapi
	+@set -e; umask 022; \
		$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) install-udev; \
		$(MAKE) -C firmware DESTDIR=$(DESTDIR) install; \
		$(MAKE) -C linux_lib/libcrystalhd $(USER_ARGS) install; \
		$(MAKE) -C filters/gst/gst-plugin-1.0 $(USER_ARGS) install; \
		$(MAKE) -C filters/vaapi $(USER_ARGS) install; \
		$(MAKE) install-check
	@umask 022; install -D -m 0644 VERSION "$(DESTDIR)$(DATADIR)/crystalhd/VERSION"

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
	rm -f -- "$(DESTDIR)$(DATADIR)/crystalhd/VERSION"
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
	rm -f tools/fw-research/flea-fw-probe
	$(MAKE) -C driver/linux -f Makefile.in KVER=$(KVER) KDIR=$(KDIR) clean
	$(MAKE) -C linux_lib/libcrystalhd clean
	$(MAKE) -C filters/gst/gst-plugin-1.0 clean
	$(MAKE) -C filters/vaapi clean
	$(MAKE) -C examples clean

include cpu.mk
