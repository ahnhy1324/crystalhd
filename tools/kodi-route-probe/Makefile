# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
PYTHON3 ?= python3
CFLAGS ?= -O2

EMBEDDED_HELPER := build/ffmpeg-embedded-hwconfig-preload.so

.PHONY: all check check-capability check-runtime-metadata check-native-pts check-package clean

all: $(EMBEDDED_HELPER)

$(EMBEDDED_HELPER): ffmpeg-embedded-hwconfig-preload.c
	mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror -fPIC -shared \
		$(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -ldl -o "$@"

check: check-capability check-runtime-metadata check-native-pts check-package

check-capability: $(EMBEDDED_HELPER)
	$(PYTHON3) -B ../../tests/kodi-ffmpeg-hwconfig-probe.py ./ffmpeg-hwconfig-probe.py
	$(PYTHON3) -B ../../tests/kodi-ffmpeg-embedded-hwconfig-probe.py \
		./ffmpeg-embedded-hwconfig-probe.py ./$(EMBEDDED_HELPER)

check-runtime-metadata:
	$(PYTHON3) -B ../../tests/native-runtime-metadata.test.py

check-native-pts:
	$(PYTHON3) -B ../../tests/native-runtime-pts-oracle.test.py

check-package:
	$(PYTHON3) -B ../../tests/kodi-runtime-package.test.py

clean:
	rm -f -- $(EMBEDDED_HELPER)
