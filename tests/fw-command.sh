#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fw_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-fw-command-check.XXXXXX")
cleanup()
{
    rm -f "$fw_test_dir/check" "$fw_test_dir/fw-command-functions.h" \
        "$fw_test_dir/fw-transport-functions.h"
    rmdir "$fw_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Compile the exact lifetime/state helpers used by both mailbox implementations.
awk '
    /^BC_STATUS crystalhd_hw_fw_cmd_(enter|recovery_enter|begin|wait)\(/ ||
    /^void crystalhd_hw_fw_cmd_(leave|end|complete|reset_locked|reset)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 9 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$fw_test_dir/fw-command-functions.h"

# Execute both production reply-transfer and postprocessing bodies as well;
# state-helper tests alone cannot catch a discarded DRAM-read error.
awk '
    /^BC_STATUS crystalhd_(flea|link)_(do_fw_cmd|fw_cmd_post_proc)\(/ {
        candidate = 1; header = ""
    }
    candidate {
        header = header $0 "\n"
        if (/;[[:space:]]*$/) { candidate = 0; next }
        if (/^\{/) {
            printf "%s", header
            candidate = 0; copying = 1; found++
        }
        next
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 4 || copying || candidate) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" \
    "$repo_dir/driver/linux/crystalhd_linkfuncs.c" > \
    "$fw_test_dir/fw-transport-functions.h"

# Both generations must enter the shared serialized path and their ISRs must
# retire it through the shared completion path. A stack callback is forbidden.
if grep -Eq 'pfw_cmd_event|wait_queue_head_t[[:space:]]+fw_cmd_event' \
    "$repo_dir/driver/linux/crystalhd_hw.h" \
    "$repo_dir/driver/linux/crystalhd_linkfuncs.c" \
    "$repo_dir/driver/linux/crystalhd_fleafuncs.c"; then
    echo 'firmware command path still publishes a stack waitqueue' >&2
    exit 1
fi
for fw_source in crystalhd_linkfuncs.c crystalhd_fleafuncs.c; do
    for fw_call in begin wait end complete; do
        if ! grep -Eq "crystalhd_hw_fw_cmd_${fw_call}\\(hw\\)" \
            "$repo_dir/driver/linux/$fw_source"; then
            echo "$fw_source does not use shared firmware-command $fw_call" >&2
            exit 1
        fi
    done
done

for fw_sanitize in no yes; do
    fw_extra=
    if [ "$fw_sanitize" = yes ]; then
        fw_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and fixed sanitizer flags are intentionally split into arguments.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $fw_extra -I"$repo_dir/include" -I"$repo_dir/include/flea" \
        -I"$repo_dir/driver/linux" -I"$fw_test_dir" \
        "$repo_dir/tests/fw-command.c" -pthread -o "$fw_test_dir/check"
    printf 'Firmware command recovery: sanitizers=%s\n' "$fw_sanitize"
    ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$fw_test_dir/check"
done
