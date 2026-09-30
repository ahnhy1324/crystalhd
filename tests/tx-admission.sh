#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tx_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-tx-admission-check.XXXXXX")
cleanup()
{
    rm -f "$tx_test_dir/check" "$tx_test_dir/tx-admission-types.h" \
        "$tx_test_dir/tx-admission-hardware.h" "$tx_test_dir/tx-admission-command.h"
    rmdir "$tx_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Retain the driver state values and the exact command/hardware TX paths.
# The fixture supplies queues, DMA mapping and hardware/IRQ boundaries only.
awk '
    /^enum (_crystalhd_state|LIST_STATUS)[[:space:]{]/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+DMA_ENGINE_CNT[[:space:]]/ { print }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" "$repo_dir/driver/linux/crystalhd_hw.h" \
    > "$tx_test_dir/tx-admission-types.h"
awk '
    /^BC_STATUS crystalhd_hw_(post_tx|cancel_tx|tx_req_complete)\(/ ||
    /^void crystalhd_hw_dma_fatal_stop\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 4 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$tx_test_dir/tx-admission-hardware.h"
awk '
    /^struct crystalhd_tx_completion[[:space:]]*\{/ ||
    /^static BC_STATUS bc_cproc_(do_fw_cmd|codein_sleep|check_inbuffs|proc_input)\(/ ||
    /^BC_STATUS crystalhd_tx_transfer_sync\(/ ||
    /^static void bc_proc_in_completion\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 7 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$tx_test_dir/tx-admission-command.h"

for tx_sanitize in no yes; do
    tx_extra=
    if [ "$tx_sanitize" = yes ]; then
        tx_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and fixed sanitizer flags are intentionally split into arguments.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $tx_extra -I"$repo_dir/include" -I"$tx_test_dir" \
        "$repo_dir/tests/tx-admission.c" -o "$tx_test_dir/check"
    printf 'TX admission and ownership: sanitizers=%s\n' "$tx_sanitize"
    ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$tx_test_dir/check"
done
