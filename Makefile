# SPDX-License-Identifier: LGPL-2.1-or-later

PREFIX ?= /usr
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PYTHON3 ?= python3
CONFIG_PRESET_MANIFEST := share/crystalhd/config-presets-v1.json

DRIVER_ARGS := KVER="$(KVER)" KDIR="$(KDIR)" DESTDIR="$(DESTDIR)"
USER_ARGS := PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)"

.PHONY: settings-ui-check install-settings uninstall-settings install-provenance uninstall-provenance all driver library library-check raw-frame-check library-drain-test gstreamer vaapi vaapi-cpu-check examples browser configuration-check uapi-check dma-check l0s-check command-pm-check fw-command-check fw-command-observer-check fw-download-check fw-research-check fw-qemu-check fw-probe-check fw-probe-tool tx-admission-check tx-release-check h264-stream-check mpeg2-inspect-check rx-ownership-check rx-receipt-check rx-output-receipts-check flea-rx-metadata-check flea-dram-check flea-power-check device-lifetime-check v4l2-parent-check ioctl-dispatch-check architecture-check pib-check userspace32-check legacy-cpu-check phase1-check check install install-module install-runtime install-browser install-check install-config uninstall uninstall-module uninstall-runtime uninstall-browser uninstall-check uninstall-config clean

all: driver library gstreamer vaapi examples

driver:
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS)

library:
	$(MAKE) -C linux_lib/libcrystalhd

# Test production library sections without hardware. Firmware calls are
# stubbed and unexpected ioctls abort the flush/EOS regressions.
library-check: export CRYSTALHD_CONFIG_DISABLE = 1
library-check:
	@set -eu; lib_test_dir=$$(mktemp -d /tmp/crystalhd-library-check.XXXXXX); \
	trap 'rm -f "$$lib_test_dir/check"; rmdir "$$lib_test_dir"' EXIT HUP INT TERM; \
	for lib_test in logging shmem-logging shmem-logging-disabled shmem-lifetime tx-ring tx-concurrency flush tx-flush eos copy planar format input mpeg4-input input-format status color clock devmem device-handle poll-fd capture capture-scaled fwload fw-version fwcmds raw-frame; do \
		test_extra=; \
		test_flags=; \
		test_file=$$lib_test; \
		test_sources="linux_lib/libcrystalhd/libcrystalhd_priv.cpp linux_lib/libcrystalhd/libcrystalhd_if.cpp"; \
		case $$lib_test in \
			logging) test_wrap=; test_sources= ;; \
			shmem-logging|shmem-logging-disabled) test_file=shmem-logging; \
				test_sources=linux_lib/libcrystalhd/libcrystalhd_priv.cpp; \
				test_wrap=-Wl,--wrap=shmget,--wrap=shmat,--wrap=shmctl,--wrap=fprintf; \
				if [ "$$lib_test" = shmem-logging-disabled ]; then \
					test_flags="-include tests/library-logging-disabled.h"; \
				fi ;; \
			shmem-lifetime) test_sources=linux_lib/libcrystalhd/libcrystalhd_priv.cpp; \
				test_wrap=-Wl,--wrap=shmget,--wrap=shmat,--wrap=shmdt,--wrap=shmctl,--wrap=pthread_atfork,--wrap=fprintf ;; \
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
				test_wrap="$$test_wrap -Wl,--wrap=pthread_create,--wrap=pthread_join,--wrap=shmget,--wrap=shmat,--wrap=shmdt,--wrap=shmctl"; \
				test_wrap="$$test_wrap -Wl,--wrap=pthread_mutexattr_init,--wrap=pthread_mutexattr_settype,--wrap=pthread_mutexattr_destroy,--wrap=pthread_mutex_init,--wrap=pthread_mutex_destroy,--wrap=pthread_mutex_lock,--wrap=pthread_mutex_unlock"; \
				test_wrap="$$test_wrap -Wl,--wrap=_Z15DtsPushFwToFleaPvPc,--wrap=_Z19DtsPushAuthFwToLinkPvPc,--wrap=_Z15DtsFWInitializePvj"; \
				test_sources="linux_lib/libcrystalhd/libcrystalhd_if.cpp linux_lib/libcrystalhd/libcrystalhd_int_if.cpp"; \
				test_sources="$$test_sources linux_lib/libcrystalhd/libcrystalhd_fwcmds.cpp linux_lib/libcrystalhd/libcrystalhd_priv.cpp"; \
				test_sources="$$test_sources linux_lib/libcrystalhd/libcrystalhd_fwdiag_if.cpp linux_lib/libcrystalhd/libcrystalhd_fwload_if.cpp linux_lib/libcrystalhd/libcrystalhd_parser.cpp" ;; \
			poll-fd) test_wrap=-Wl,--wrap=ioctl,--wrap=fcntl ;; \
			capture) test_wrap=-Wl,--wrap=ioctl,--wrap=close,--wrap=malloc,--wrap=free,--wrap=madvise,--wrap=pthread_mutex_lock; \
				test_wrap="$$test_wrap -Wl,--wrap=shmget,--wrap=shmat,--wrap=shmdt,--wrap=shmctl" ;; \
			capture-scaled) test_flags=-DCRYSTALHD_EXPERIMENTAL_SCALED_RX; test_file=capture; \
				test_wrap=-Wl,--wrap=ioctl,--wrap=close,--wrap=malloc,--wrap=free,--wrap=madvise,--wrap=pthread_mutex_lock; \
				test_wrap="$$test_wrap -Wl,--wrap=shmget,--wrap=shmat,--wrap=shmdt,--wrap=shmctl" ;; \
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
			tx-concurrency) test_wrap=-Wl,--wrap=pthread_mutex_lock,--wrap=open,--wrap=openat,--wrap=ioctl; \
				test_extra=linux_lib/libcrystalhd/libcrystalhd_int_if.cpp ;; \
			flush) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_lock; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsReleaseInterface,--wrap=DtsFinishOutputCall,--wrap=DtsPublishOutput"; \
				test_wrap="$$test_wrap -Wl,--wrap=DtsCommitFetchedOutput,--wrap=DtsPublishCopiedOutput" ;; \
			tx-flush) test_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_lock,--wrap=pthread_mutex_unlock; \
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
		$(CXX) -std=c++11 -O1 -g -Wall -Werror $$test_flags \
		-ffunction-sections -fdata-sections -D__LINUX_USER__ \
		-Ilinux_lib/libcrystalhd -Iinclude -Iinclude/link \
		tests/library-$$test_file.cpp $$test_sources $$test_extra \
		-Wl,--gc-sections $$test_wrap -pthread -lrt \
		$(CRYSTALHD_CPU_FLAGS) -o "$$lib_test_dir/check"; "$$lib_test_dir/check"; \
	done

gstreamer: library
	$(MAKE) -C filters/gst/gst-plugin-1.0

# Fresh CPU-only production-function binaries. No normal library outputs or
# CPU-mode stamp are dependencies; legacy sanitizer coverage is not claimed.
.PHONY: library-check-sanitized
library-check-sanitized:
	CXX=$(call crystalhd_quote,$(CXX)) CPPFLAGS=$(call crystalhd_quote,$(CPPFLAGS)) \
		CXXFLAGS=$(call crystalhd_quote,$(CXXFLAGS)) LDFLAGS=$(call crystalhd_quote,$(LDFLAGS)) \
		CRYSTALHD_LIBRARY_SANITIZER_CPU_FLAGS=$(call crystalhd_quote,$(CRYSTALHD_CPU_FLAGS)) \
		CRYSTALHD_LIBRARY_SANITIZER_LEGACY=$(call crystalhd_quote,$(LEGACY_CPU)) \
		$(PYTHON3) -B tests/library-selected-sanitizer.py

raw-frame-check:
	CC="$(CC)" CXX="$(CXX)" sh ./tests/library-raw-frame.sh

# Optional direct-library hardware probe. Building never opens the device;
# running requires explicit --hardware (or device-free --preflight).
library-drain-test: library tests/library-drain.cpp \
		filters/gst/gst-plugin-1.0/gstcrystalhd-codecs.h \
		filters/crystalhd-mpeg2-inspect.h tests/phase1-progress.h \
		tests/issue204-active-context.h tests/issue204-generation-observer.h \
		tests/issue399-firmware-drop.h tests/flea-power-observer.h \
		tests/rx-output-receipts.h tests/rx-output-receipts.cpp
	$(CXX) -std=c++11 -O2 -g -Wall -Wextra -Werror -D__LINUX_USER__ \
		-Iinclude -Ilinux_lib/libcrystalhd tests/library-drain.cpp \
		tests/rx-output-receipts.cpp \
		$$(pkg-config --cflags --libs libavformat libavcodec libavutil glib-2.0 gstreamer-1.0) \
		-Llinux_lib/libcrystalhd -lcrystalhd $(CRYSTALHD_CPU_FLAGS) -o tests/library-drain-test

vaapi: library
	$(MAKE) -C filters/vaapi

vaapi-cpu-check:
	CXX="$(CXX)" sh ./tests/vaapi-cpu-flags.sh

examples: library
	$(MAKE) -C examples

browser:
	$(MAKE) -C browser

configuration-check:
	$(PYTHON3) -B tests/crystalhd-settings.test.py
	$(PYTHON3) -B scripts/generate-native-config.py --check
	$(PYTHON3) -B tests/native-config.test.py
	$(PYTHON3) -B tests/configuration-inventory.test.py
	$(PYTHON3) -B tests/configuration-schema.test.py
	$(PYTHON3) -B tests/crystalhd-config.test.py

settings-ui-check:
	$(PYTHON3) -B tests/crystalhd-settings-gtk.test.py
	dbus-run-session -- $(PYTHON3) -B tests/crystalhd-settings-tray.test.py

.PHONY: runtime-install-check
runtime-install-check:
	$(PYTHON3) -B tests/runtime-install.test.py

.PHONY: kodi-compat-check
kodi-compat-check:
	CXX="$(CXX)" $(PYTHON3) -B tests/kodi-vaapi-publish-order.py

.PHONY: ffmpeg-compat-check
ffmpeg-compat-check:
	CC="$(CC)" $(PYTHON3) -B tests/ffmpeg-vaapi-loop.py

.PHONY: native-picture-check
native-picture-check:
	$(PYTHON3) -B tests/native-picture-oracle.test.py
	$(PYTHON3) -B tests/native-picture-fixture.test.py
	CC="$(CC)" $(PYTHON3) -B tests/native-color-negotiation.py

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

tx-release-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/tx-release.sh

tx-admission-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/tx-admission.sh

h264-stream-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/h264-stream.sh

.PHONY: h264-reference-check
h264-reference-check:
	$(PYTHON3) -B tests/h264-reference-lifecycle.py --self-test

.PHONY: h264-au-admission-check
h264-au-admission-check:
	$(PYTHON3) -B tests/h264-au-admission.test.py

# Device-free, true-1080p60 source/encode/hash admission for issue #399.
.PHONY: issue399-baseline-check
issue399-baseline-check:
	CRYSTALHD_ISSUE399_FFMPEG=1 $(PYTHON3) -B tests/issue399-baseline.test.py

# Device-free admission for the frozen, remux-only VC-1 Advanced seek oracle.
.PHONY: vc1-seek-oracle-check
vc1-seek-oracle-check:
	$(PYTHON3) -B tests/vc1-seek-oracle.test.py

rx-ownership-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/rx-ownership.sh

.PHONY: rx-pages-check
rx-pages-check:
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh ./tests/rx-pages.sh

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

architecture-check: command-pm-check fw-command-check fw-command-observer-check fw-download-check fw-staging-check fw-probe-check tx-admission-check tx-release-check h264-stream-check rx-ownership-check rx-pages-check rx-receipt-check rx-output-receipts-check flea-rx-metadata-check flea-dram-check flea-power-check device-lifetime-check v4l2-parent-check ioctl-dispatch-check

pib-check:
	CC="$(CC)" sh ./tests/flea-pib.sh
	CC="$(CC)" sh ./tests/link-pib.sh

userspace32-check: vaapi-cpu-check
	CXX="$(CXX)" sh ./tests/userspace32.sh

mpeg2-inspect-check:
	CC="$(CC)" CXX="$(CXX)" sh ./tests/mpeg2-inspect.sh

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

check: runtime-preflight-check runtime-install-check kodi-compat-check ffmpeg-compat-check native-picture-check configuration-check uapi-check dma-check l0s-check architecture-check pib-check library-check raw-frame-check h264-reference-check h264-au-admission-check mpeg2-inspect-check vc1-seek-oracle-check fw-research-check all
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
	+$(PYTHON3) -B scripts/install-runtime.py --make '$(MAKE)' \
		--destdir "$(DESTDIR)" --prefix "$(PREFIX)" \
		--bindir "$(BINDIR)" --datadir "$(DATADIR)"

.PHONY: install-runtime-record uninstall-runtime-record install-runtime-activate
install-runtime-record:
	@umask 022; install -D -m 0644 VERSION "$(DESTDIR)$(DATADIR)/crystalhd/VERSION"

uninstall-runtime-record:
	rm -f -- "$(DESTDIR)$(DATADIR)/crystalhd/VERSION"
	rm -f -- "$(DESTDIR)$(DATADIR)/crystalhd/runtime-components-v1.json"

# Staging does not reload udev or the host linker cache. Activate only after
# the prepared payload has been copied to its final destinations.
install-runtime-activate:
	@if [ -z "$(DESTDIR)" ]; then \
		if command -v udevadm >/dev/null 2>&1; then \
			udevadm control --reload-rules || echo "WARN: udev rules installed, but the daemon did not reload them" >&2; \
		fi; \
		if command -v ldconfig >/dev/null 2>&1; then ldconfig; \
		elif [ -x /sbin/ldconfig ]; then /sbin/ldconfig; fi; \
	fi

install-browser: browser
	$(MAKE) -C browser $(USER_ARGS) install

install-provenance:
	@umask 022; install -D -m 0644 scripts/crystalhd_provenance.py \
		"$(DESTDIR)$(DATADIR)/crystalhd/provenance.py"

install-check: install-provenance
	@set -eu; umask 022; \
		install -D -m 0755 scripts/crystalhd-check \
			"$(DESTDIR)$(BINDIR)/crystalhd-check"; \
		helper_relative=$$($(PYTHON3) -c \
			'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
			"$(DATADIR)/crystalhd/provenance.py" "$(BINDIR)"); \
		$(PYTHON3) -c \
			'import shlex,sys; p=sys.argv[1]; s=open(p,encoding="utf-8").read(); replacements=(("\047@PROVENANCE_HELPER_RELATIVE@\047",shlex.quote(sys.argv[2])),("\047@PROVENANCE_DATA_DIR@\047",shlex.quote(sys.argv[3]))); all(s.count(token)==1 for token,value in replacements) or sys.exit("unexpected provenance path token count"); open(p,"w",encoding="utf-8",newline="\n").write(__import__("functools").reduce(lambda text,pair:text.replace(*pair),replacements,s))' \
			"$(DESTDIR)$(BINDIR)/crystalhd-check" "$$helper_relative" \
			"$(DATADIR)/crystalhd"

install-config: install-provenance
	@set -eu; umask 022; \
		install -D -m 0644 "$(CONFIG_PRESET_MANIFEST)" \
			"$(DESTDIR)$(DATADIR)/crystalhd/config-presets-v1.json"; \
		install -D -m 0755 scripts/crystalhd-config.py \
			"$(DESTDIR)$(BINDIR)/crystalhd-config"; \
		preset_relative=$$($(PYTHON3) -c \
			'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
			"$(DATADIR)/crystalhd/config-presets-v1.json" "$(BINDIR)"); \
		helper_relative=$$($(PYTHON3) -c \
			'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' \
			"$(DATADIR)/crystalhd/provenance.py" "$(BINDIR)"); \
		$(PYTHON3) -c \
			'import json,os,sys; path=sys.argv[1]; data=open(path, encoding="utf-8").read(); replacements=(("\"@PRESET_MANIFEST_RELATIVE@\"",json.dumps(sys.argv[2],ensure_ascii=False)),("\"@PROVENANCE_HELPER_RELATIVE@\"",json.dumps(sys.argv[3],ensure_ascii=False))); all(data.count(token)==1 for token,value in replacements) or sys.exit("unexpected configuration path token count"); open(path,"w",encoding="utf-8",newline="\n").write(__import__("functools").reduce(lambda text,pair:text.replace(*pair),replacements,data))' \
			"$(DESTDIR)$(BINDIR)/crystalhd-config" "$$preset_relative" \
			"$$helper_relative"

# Optional desktop frontend; native runtime/CLI installation stays headless.
# The static UTF-8 header keeps extraction reproducible without generated dates.
XGETTEXT ?= xgettext
MSGFMT ?= msgfmt
SETTINGS_LOCALES := $(shell sed '/^\#/d' po/LINGUAS)
SETTINGS_POT_EXTRACT = $(XGETTEXT) --language=Python --from-code=UTF-8 \
	--keyword=_ --no-location \
	--add-comments=Translators:

.PHONY: settings-pot settings-l10n-check install-settings-locales uninstall-settings-locales
settings-pot:
	@set -eu; settings_messages=$$(mktemp); \
		trap 'rm -f "$$settings_messages"' EXIT HUP INT TERM; \
		$(SETTINGS_POT_EXTRACT) --output="$$settings_messages" scripts/crystalhd-settings.py; \
		{ cat po/settings-header.pot; printf '\n'; sed '1,/^$$/d' "$$settings_messages"; } > po/crystalhd-settings.pot

settings-l10n-check:
	@set -eu; settings_check=$$(mktemp -d); \
		trap 'rm -rf "$$settings_check"' EXIT HUP INT TERM; \
		$(SETTINGS_POT_EXTRACT) --output="$$settings_check/messages" scripts/crystalhd-settings.py; \
		{ cat po/settings-header.pot; printf '\n'; sed '1,/^$$/d' "$$settings_check/messages"; } > "$$settings_check/template"; \
		cmp po/crystalhd-settings.pot "$$settings_check/template" || \
			{ printf '%s\n' 'Settings catalogue template is stale; run make settings-pot.' >&2; exit 1; }; \
		$(PYTHON3) -B scripts/check-settings-messages.py; \
		for locale in $(SETTINGS_LOCALES); do \
			$(MSGFMT) --check --check-accelerators=_ --use-fuzzy \
				--output-file=/dev/null "po/$$locale.po"; \
		done

install-settings-locales:
	@set -eu; umask 022; settings_mo=$$(mktemp); \
		trap 'rm -f "$$settings_mo"' EXIT HUP INT TERM; \
		for locale in $(SETTINGS_LOCALES); do \
			$(MSGFMT) --check --check-accelerators=_ --output-file="$$settings_mo" "po/$$locale.po"; \
			install -D -m 0644 "$$settings_mo" \
				"$(DESTDIR)$(DATADIR)/locale/$$locale/LC_MESSAGES/crystalhd-settings.mo"; \
		done

uninstall-settings-locales:
	@set -eu; for locale in $(SETTINGS_LOCALES); do \
		file="$(DESTDIR)$(DATADIR)/locale/$$locale/LC_MESSAGES/crystalhd-settings.mo"; \
		if [ -e "$$file" ] || [ -L "$$file" ]; then rm -- "$$file"; fi; \
		done

install-settings: install-config install-settings-locales
	@set -eu; umask 022; \
		install -D -m 0755 scripts/crystalhd-settings.py \
			"$(DESTDIR)$(BINDIR)/crystalhd-settings"; \
		data_relative=$$($(PYTHON3) -c \
			'import os,sys; print(os.path.relpath(sys.argv[1],sys.argv[2]))' \
			"$(DATADIR)" "$(BINDIR)"); \
		$(PYTHON3) -c \
			'import json,sys; p=sys.argv[1]; s=open(p,encoding="utf-8").read(); token="\"@SETTINGS_DATA_RELATIVE@\""; s.count(token)==1 or sys.exit("unexpected settings data token count"); open(p,"w",encoding="utf-8",newline="\n").write(s.replace(token,json.dumps(sys.argv[2],ensure_ascii=False)))' \
			"$(DESTDIR)$(BINDIR)/crystalhd-settings" "$$data_relative"; \
		$(PYTHON3) -B scripts/render-settings-desktop.py --bindir "$(BINDIR)" \
			--output "$(DESTDIR)$(DATADIR)/applications/crystalhd-settings.desktop"; \
		for size in scalable symbolic 16x16 22x22 24x24 32x32 64x64 128x128; do \
			for icon in share/icons/hicolor/$$size/apps/crystalhd-settings*; do \
				install -D -m 0644 "$$icon" \
					"$(DESTDIR)$(DATADIR)/icons/hicolor/$$size/apps/$${icon##*/}"; \
			done; \
		done

uninstall-settings: uninstall-settings-locales
	@set -eu; for file in \
		"$(DESTDIR)$(BINDIR)/crystalhd-settings" \
		"$(DESTDIR)$(DATADIR)/applications/crystalhd-settings.desktop"; do \
		if [ -e "$$file" ] || [ -L "$$file" ]; then rm -- "$$file"; fi; \
	done; \
	for size in scalable symbolic 16x16 22x22 24x24 32x32 64x64 128x128; do \
		case $$size in \
			scalable) icon=crystalhd-settings.svg ;; \
			symbolic) icon=crystalhd-settings-symbolic.svg ;; \
			*) icon=crystalhd-settings.png ;; \
		esac; \
		file="$(DESTDIR)$(DATADIR)/icons/hicolor/$$size/apps/$$icon"; \
		if [ -e "$$file" ] || [ -L "$$file" ]; then rm -- "$$file"; fi; \
	done

uninstall:
	$(MAKE) uninstall-runtime
	$(MAKE) uninstall-module

uninstall-module:
	$(MAKE) -C driver/linux -f Makefile.in $(DRIVER_ARGS) uninstall-module

uninstall-runtime:
	+$(PYTHON3) -B scripts/install-runtime.py --uninstall --make '$(MAKE)' \
		--destdir "$(DESTDIR)" --prefix "$(PREFIX)" \
		--bindir "$(BINDIR)" --datadir "$(DATADIR)"

uninstall-browser:
	$(MAKE) -C browser $(USER_ARGS) uninstall

uninstall-check:
	rm -f -- "$(DESTDIR)$(BINDIR)/crystalhd-check"

uninstall-config:
	rm -f -- "$(DESTDIR)$(BINDIR)/crystalhd-config"
	rm -f -- "$(DESTDIR)$(DATADIR)/crystalhd/config-presets-v1.json"

uninstall-provenance:
	rm -f -- "$(DESTDIR)$(DATADIR)/crystalhd/provenance.py"

clean:
	rm -f tests/library-drain-test
	rm -f tools/fw-research/flea-fw-probe
	$(MAKE) -C driver/linux -f Makefile.in KVER=$(KVER) KDIR=$(KDIR) clean
	$(MAKE) -C linux_lib/libcrystalhd clean
	$(MAKE) -C filters/gst/gst-plugin-1.0 clean
	$(MAKE) -C filters/vaapi clean
	$(MAKE) -C examples clean

include cpu.mk

# Inspect immutable package/rootfs fixtures without loading target executables.
.PHONY: runtime-preflight-check
runtime-preflight-check:
	$(PYTHON3) -B tests/native-runtime-preflight.test.py
