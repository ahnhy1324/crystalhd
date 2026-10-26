#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
decoder_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-v4l2-decoder.XXXXXX")
cleanup()
{
    rm -f "$decoder_test_dir/check" "$decoder_test_dir/decoder-production.h" \
        "$decoder_test_dir/decoder-types.h"
    rmdir "$decoder_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
sed '/^#include /d' "$repo_dir/driver/linux/crystalhd_v4l2_decoder.h" \
    > "$decoder_test_dir/decoder-types.h"
awk '
    /^#define[[:space:]]+CRYSTALHD_PICTURE_FLAG_(EOS|DECODE_ERROR)[[:space:]]/ { print; found++ }
    END { if (found != 2) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fw_if.h" >> "$decoder_test_dir/decoder-types.h"
awk '
    /^#define[[:space:]]+FLEA_DECODE_ERROR_FLAG[[:space:]]/ { print; found++ }
    END { if (found != 1) exit 1 }
' "$repo_dir/driver/linux/FleaDefs.h" >> "$decoder_test_dir/decoder-types.h"
awk '
    /^enum crystalhd_decoder_(phase|codec)[[:space:]]*\{/ ||
    /^struct crystalhd_decoder_config[[:space:]]*\{/ ||
    /^struct crystalhd_rx_metadata[[:space:]]*\{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 4 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" \
    "$repo_dir/driver/linux/crystalhd_hw.h" >> "$decoder_test_dir/decoder-types.h"
sed '/^#include /d' "$repo_dir/driver/linux/crystalhd_v4l2_decoder.c" \
    > "$decoder_test_dir/decoder-production.h"
for decoder_sanitize in no yes; do
    decoder_extra=
    if [ "$decoder_sanitize" = yes ]; then
        decoder_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $decoder_extra -I"$decoder_test_dir" -I"$repo_dir/include" \
        "$repo_dir/tests/v4l2-decoder.c" -o "$decoder_test_dir/check"
    printf 'V4L2 decoder controller: sanitizers=%s\n' "$decoder_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$decoder_test_dir/check"
done
