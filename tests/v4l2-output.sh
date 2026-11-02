#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-v4l2-output.XXXXXX")
cleanup()
{
    rm -f "$output_test_dir/check" "$output_test_dir/types.h" \
        "$output_test_dir/output.h" "$output_test_dir/output.c" "$output_test_dir/compat.h" \
        "$output_test_dir/formatter.h"
    rmdir "$output_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
awk '
 /^#define[[:space:]]+CRYSTALHD_H264_[A-Z_]+[[:space:]]/ { print }
 /^static int crystalhd_h264_format_pes\(/ { copying = 1; found++ }
 copying { print }
 copying && /^}/ { copying = 0 }
 END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_stream.c" > "$output_test_dir/formatter.h"
sed '/^#include /d' "$repo_dir/driver/linux/crystalhd_v4l2_compat.h" > "$output_test_dir/compat.h"
sed '/^#include /d' "$repo_dir/driver/linux/crystalhd_v4l2_output.h" > "$output_test_dir/output.h"
sed '/^#include /d' "$repo_dir/driver/linux/crystalhd_v4l2_output.c" > "$output_test_dir/output.c"
awk '
    /^struct crystalhd_tx_(buffer_ops|buffer)[[:space:]]*\{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.h" > "$output_test_dir/types.h"

for output_layout in 0 1; do
for output_sanitize in native address,undefined; do
    output_flags=
    if [ "$output_sanitize" != native ]; then
        output_flags="-fsanitize=$output_sanitize -fno-omit-frame-pointer -fno-pie -no-pie"
    fi
    ${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:--O2 -g} $output_flags \
        -DTEST_VB2_OLD=$output_layout -std=c11 -Wall -Wextra -Werror -I"$output_test_dir" \
        -I"$repo_dir/include" -I"$repo_dir/include/link" \
        -I"$repo_dir/driver/linux" "$repo_dir/tests/v4l2-output.c" \
        ${LDFLAGS:-} $output_flags -o "$output_test_dir/check"
    printf 'V4L2 direct OUTPUT: old-layout=%s sanitizers=%s\n' "$output_layout" "$output_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$output_test_dir/check"
done
done
