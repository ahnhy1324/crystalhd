#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
buffer_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-v4l2-buffers.XXXXXX")
cleanup()
{
    rm -f "$buffer_test_dir/check" "$buffer_test_dir/types.h" \
        "$buffer_test_dir/buffers.h" "$buffer_test_dir/buffers.c" \
        "$buffer_test_dir/finish.h" "$buffer_test_dir/compat.h"
    rmdir "$buffer_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Compile the actual adapter in full, replacing kernel interfaces only.
sed '/^#include /d' "$repo_dir/driver/linux/crystalhd_v4l2_compat.h" > "$buffer_test_dir/compat.h"
sed '/^#include /d' "$repo_dir/driver/linux/crystalhd_v4l2_buffers.h" > "$buffer_test_dir/buffers.h"
sed '/^#include /d' "$repo_dir/driver/linux/crystalhd_v4l2_buffers.c" > "$buffer_test_dir/buffers.c"
awk '
    /^struct crystalhd_rx_(buffer_ops|buffer|metadata|completion|image)[[:space:]]*\{/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+CRYSTALHD_DMA_DESC_MAX_XFER_BYTES[[:space:]]/ { print; limits++ }
    /^#define[[:space:]]+FLEA_DECODE_ERROR_FLAG[[:space:]]/ { print; limits++ }
    END { if (found != 5 || copying || limits != 2) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.h" \
    "$repo_dir/driver/linux/crystalhd_hw.h" \
    "$repo_dir/driver/linux/FleaDefs.h" > "$buffer_test_dir/types.h"
awk '
    /^BC_STATUS crystalhd_rx_(buffer_write|finish_yuyv)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.c" > "$buffer_test_dir/finish.h"

for buffer_layout in 0 1; do
for buffer_sanitize in native address,undefined; do
    buffer_flags=
    if [ "$buffer_sanitize" != native ]; then
        buffer_flags="-fsanitize=$buffer_sanitize -fno-omit-frame-pointer -fno-pie -no-pie"
    fi
    ${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:--O2 -g} $buffer_flags \
        -DTEST_VB2_OLD=$buffer_layout -std=c11 -Wall -Wextra -Werror -I"$buffer_test_dir" \
        -I"$repo_dir/include" -I"$repo_dir/include/link" -I"$repo_dir/driver/linux" \
        "$repo_dir/tests/v4l2-buffers.c" ${LDFLAGS:-} $buffer_flags \
        -o "$buffer_test_dir/check"
    printf 'V4L2 direct SG capture: old-layout=%s sanitizers=%s\n' "$buffer_layout" "$buffer_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$buffer_test_dir/check"
done
done
