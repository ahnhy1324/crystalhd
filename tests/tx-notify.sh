#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tx_note_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-tx-notify-check.XXXXXX")
cleanup()
{
    rm -f "$tx_note_test_dir/check" "$tx_note_test_dir/tx-notify-types.h" \
        "$tx_note_test_dir/tx-notify-store.h" "$tx_note_test_dir/tx-notify-core.h" \
        "$tx_note_test_dir/tx-notify-helper.h"
    rmdir "$tx_note_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
awk '
    /^_TX_INPUT_BUFFER_INFO_$/ { print "typedef struct"; copying = 1; found++ }
    copying { print }
    copying && /^\*PTX_INPUT_BUFFER_INFO;/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/include/flea/DriverFwShare.h" > "$tx_note_test_dir/tx-notify-types.h"
awk '
    /^struct chd_tx_notify_(record|store) \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^static struct chd_tx_notify_store tx_store;/ { print; state++ }
    END { if (found != 2 || state != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_tx_notify.c" > "$tx_note_test_dir/tx-notify-store.h"
awk '
    /TX notify core begin:/ { copying = 1; found++; next }
    /TX notify core end\./ { copying = 0; next }
    copying { print }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_tx_notify.c" > "$tx_note_test_dir/tx-notify-core.h"
awk '
    /^static void crystalhd_flea_update_tx_done_to_fw\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$tx_note_test_dir/tx-notify-helper.h"
for tx_note_mode in default research; do
    tx_note_define=
    if [ "$tx_note_mode" = research ]; then tx_note_define=-DCRYSTALHD_TX_NOTIFY_RESEARCH; fi
    for tx_note_sanitize in no yes; do
        tx_note_flags=
        if [ "$tx_note_sanitize" = yes ]; then
            tx_note_flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
        fi
        "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
            $tx_note_define $tx_note_flags -I"$repo_dir/driver/linux" -I"$tx_note_test_dir" \
            "$repo_dir/tests/tx-notify.c" -o "$tx_note_test_dir/check"
        printf 'TX notify: mode=%s sanitizers=%s\n' "$tx_note_mode" "$tx_note_sanitize"
        ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$tx_note_test_dir/check"
    done
done
