#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stream_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-h264-stream-check.XXXXXX")
cleanup()
{
    rm -f "$stream_test_dir/check" "$stream_test_dir/h264-stream-types.h" \
        "$stream_test_dir/h264-stream-production.h"
    rmdir "$stream_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

awk '
    /^void crystalhd_tx_buffer_(get|put)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.c" \
    > "$stream_test_dir/h264-stream-production.h"

awk '
    /^enum (_crystalhd_state|crystalhd_decoder_phase|crystalhd_decoder_codec|_BC_PCI_DEV_IDS)[[:space:]{]/ {
        copying = 1; found++
    }
    /^struct crystalhd_tx_buffer(_ops)?[[:space:]]*\{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 6 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" \
    "$repo_dir/include/bc_dts_glob_lnx.h" \
    "$repo_dir/driver/linux/crystalhd_misc.h" \
    > "$stream_test_dir/h264-stream-types.h"

awk '
    /^#define[[:space:]]+CRYSTALHD_H264_[A-Z_]+[[:space:]]/ { print }
    /^struct crystalhd_stream[[:space:]]*\{/ { copying = 1; found++ }
    /^static int crystalhd_h264_(format_pes|format_eos|send_staged)\(/ {
        copying = 1; found++
    }
    /^int crystalhd_(stream_prepare|decoder_submit_h264|decoder_submit_h264_eos|decoder_validate_h264_locked|decoder_resume_h264_locked)\(/ {
        copying = 1; found++
    }
    /^void crystalhd_stream_release\(/ { copying = 1; found++ }
    /^static void crystalhd_stream_(get|put)\(/ ||
    /^static const struct crystalhd_tx_buffer_ops crystalhd_stream_buffer_ops =/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};?$/ { copying = 0 }
    END { if (found != 13 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_stream.c" \
    >> "$stream_test_dir/h264-stream-production.h"

awk '
    /^int crystalhd_status_to_errno\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" \
    >> "$stream_test_dir/h264-stream-production.h"

for stream_sanitize in no yes; do
    stream_extra=
    if [ "$stream_sanitize" = yes ]; then
        stream_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $stream_extra -I"$repo_dir/include" -I"$stream_test_dir" \
        "$repo_dir/tests/h264-stream.c" -o "$stream_test_dir/check"
    printf 'H.264 typed stream: sanitizers=%s\n' "$stream_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$stream_test_dir/check"
done
