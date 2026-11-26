#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rx_note_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-rx-notify-check.XXXXXX")
cleanup()
{
    rm -f "$rx_note_test_dir/check" "$rx_note_test_dir/"*.h
    rmdir "$rx_note_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
awk '
    /^typedef enum _BC_STATUS/ { copying = 1; found++ }
    copying { print }
    copying && /^} BC_STATUS;/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/include/bc_dts_defs.h" > "$rx_note_test_dir/rx-types.h"
awk '
    /^enum _DECOUT_COMPLETION_FLAGS/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/include/bc_dts_glob_lnx.h" >> "$rx_note_test_dir/rx-types.h"
awk '
    /^struct crystalhd_rx_(format_metadata|metadata) \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.h" >> "$rx_note_test_dir/rx-types.h"
awk '
    /^struct chd_tx_notify_(record|store) \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^static struct chd_tx_notify_store tx_store;/ { print; state++ }
    END { if (found != 2 || state != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_tx_notify.c" > "$rx_note_test_dir/rx-store.h"
awk '
    /TX notify core begin:/ { copying = 1; found++; next }
    /TX notify core end\./ { copying = 0; next }
    copying { print }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_tx_notify.c" > "$rx_note_test_dir/rx-core.h"
awk '
    /^#define RX_PTR_/ { print; macros++ }
    /RX notify backend begin:/ { copying = 1; found++; next }
    /RX notify backend end\./ { copying = 0; next }
    copying { print }
    END { if (macros != 2 || found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_rx_notify.c" > "$rx_note_test_dir/rx-backend.h"
awk '
    /^void crystalhd_flea_handle_PicQSts_intr\(/ { candidate = 1; header = "" }
    candidate {
        header = header $0 "\n"
        if (/;[[:space:]]*$/) { candidate = 0; next }
        if (/^\{/) { printf "%s", header; candidate = 0; copying = 1; found++; next }
        next
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying || candidate) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$rx_note_test_dir/rx-picq.h"
for rx_note_mode in default research; do
    rx_note_define=
    if [ "$rx_note_mode" = research ]; then
        rx_note_define='-DCRYSTALHD_TX_NOTIFY_RESEARCH -DCRYSTALHD_RX_NOTIFY_RESEARCH'
    fi
    for rx_note_sanitize in no yes; do
        rx_note_flags=
        if [ "$rx_note_sanitize" = yes ]; then
            rx_note_flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
        fi
        "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
            $rx_note_define $rx_note_flags -I"$repo_dir/driver/linux" -I"$rx_note_test_dir" \
            "$repo_dir/tests/rx-notify.c" -o "$rx_note_test_dir/check"
        printf 'RX notify: mode=%s sanitizers=%s\n' "$rx_note_mode" "$rx_note_sanitize"
        ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$rx_note_test_dir/check"
    done
done
