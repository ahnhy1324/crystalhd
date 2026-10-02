#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tx_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-tx-admission-check.XXXXXX")
cleanup()
{
    rm -f "$tx_test_dir/check" "$tx_test_dir/tx-admission-types.h" \
        "$tx_test_dir/tx-admission-hardware.h" "$tx_test_dir/tx-admission-command.h" \
        "$tx_test_dir/tx-admission-buffer.h" "$tx_test_dir/tx-admission-flea-types.h" \
        "$tx_test_dir/tx-admission-flea.h" "$tx_test_dir/tx-admission-queue-types.h" \
        "$tx_test_dir/tx-admission-queue.h"
    rmdir "$tx_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Retain the driver state values and the exact command/hardware TX paths.
# The fixture supplies node allocation, DMA mapping and hardware/IRQ boundaries.
awk '
    /^struct crystalhd_(elem|dioq)[[:space:]]*\{/ { copying = 1; found++ }
    /^#define[[:space:]]+BC_LINK_DIOQ_SIG[[:space:]]/ { print }
    /^typedef void \(\*crystalhd_data_free_cb\)/ { print }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.h" > "$tx_test_dir/tx-admission-queue-types.h"
awk '
    /^struct crystalhd_elem \*crystalhd_dioq_fetch_elem\(/ ||
    /^void crystalhd_dioq_add_elem\(/ ||
    /^BC_STATUS crystalhd_dioq_add\(/ ||
    /^void \*crystalhd_dioq_(fetch|find_and_fetch)\(/ { copying = 1; found++ }
    copying {
        # Instrument the public fixture boundary without replacing node logic.
        gsub(/crystalhd_dioq_fetch_elem\(/, "crystalhd_dioq_fetch_elem_actual(")
        gsub(/crystalhd_dioq_add_elem\(/, "crystalhd_dioq_add_elem_actual(")
        gsub(/crystalhd_dioq_find_and_fetch\(/, "crystalhd_dioq_find_and_fetch_actual(")
        gsub(/crystalhd_dioq_fetch\(/, "crystalhd_dioq_fetch_actual(")
        gsub(/crystalhd_dioq_add\(/, "crystalhd_dioq_add_actual(")
        print
    }
    copying && /^}/ { copying = 0 }
    END { if (found != 5 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.c" > "$tx_test_dir/tx-admission-queue.h"
awk '
    /^static void crystalhd_dio_tx_(get|put)\(/ ||
    /^void crystalhd_tx_buffer_(get|put)\(/ ||
    /^BC_STATUS crystalhd_(map_dio|unmap_dio)\(/ ||
    /^static const struct crystalhd_tx_buffer_ops crystalhd_dio_tx_buffer_ops =/ {
        copying = 1; found++
        if ($0 ~ /^BC_STATUS crystalhd_map_dio\(/) {
            mapping = 1
            # The kernel does not enable -Wextra: retain its exact signed
            # GUP-result comparison while keeping all other warnings fatal.
            print "#pragma GCC diagnostic push"
            print "#pragma GCC diagnostic ignored \"-Wsign-compare\""
        }
    }
    copying {
        # Keep the command-path mapping boundary mock independent; extract the
        # complete production mapper under a distinct fixture-only name.
        sub(/^BC_STATUS crystalhd_map_dio\(/, "BC_STATUS crystalhd_map_dio_actual(")
        print
    }
    copying && /^};?$/ {
        copying = 0
        if (mapping) { print "#pragma GCC diagnostic pop"; mapping = 0 }
    }
    END { if (found != 7 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.c" > "$tx_test_dir/tx-admission-buffer.h"
awk '
    /^enum (_crystalhd_state|_BC_DTS_GLOBALS|_BC_PCI_DEV_IDS|LIST_STATUS|FLEA_POWER_STATES)[[:space:]{]/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+DMA_ENGINE_CNT[[:space:]]/ { print }
    /^#define[[:space:]]+TX_WRAP_THRESHOLD[[:space:]]/ { print; thresholds++ }
    END { if (found != 5 || thresholds != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" "$repo_dir/include/bc_dts_glob_lnx.h" \
    "$repo_dir/driver/linux/crystalhd_hw.h" "$repo_dir/driver/linux/FleaDefs.h" \
    > "$tx_test_dir/tx-admission-types.h"
awk '
    /^typedef enum _DRIVER_FW_FLAGS_\{/ { copying = 1; flags++ }
    /^_TX_INPUT_BUFFER_INFO_$/ { print "typedef struct"; copying = 1; records++ }
    copying { print }
    copying && (/^}DRIVER_FW_FLAGS;/ || /^\*PTX_INPUT_BUFFER_INFO;/) { copying = 0 }
    END { if (flags != 1 || records != 1 || copying) exit 1 }
' "$repo_dir/include/flea/DriverFwShare.h" > "$tx_test_dir/tx-admission-flea-types.h"
awk '
    /^#define BCHP_MISC1_TX_(FIRST_DESC_[UL]_ADDR_LIST[01]|SW_DESC_LIST_CTRL_STS(_TX_DMA_RUN_STOP_MASK)?)[[:space:]]/ {
        print; found++
    }
    END { if (found != 6) exit 1 }
' "$repo_dir/include/flea/bcm_70015_regs.h" >> "$tx_test_dir/tx-admission-flea-types.h"

# Keep the full notification and FIFO bodies, skipping their declarations.
awk '
    /^(void crystalhd_flea_update_tx_buff_info|bool crystalhd_flea_check_input_full|BC_STATUS crystalhd_flea_prepare_tx_dma|void crystalhd_flea_start_tx_dma_engine)\(/ {
        candidate = 1; header = ""
        fifo = $0 ~ /^bool crystalhd_flea_check_input_full\(/
    }
    candidate {
        header = header $0 "\n"
        if (/;[[:space:]]*$/) { candidate = 0; next }
        if (/^\{/) {
            # The kernel omits -Wextra. Preserve the unused FIFO ABI parameter
            # while retaining fatal warnings everywhere else in this fixture.
            if (fifo) {
                print "#pragma GCC diagnostic push"
                print "#pragma GCC diagnostic ignored \"-Wunused-parameter\""
            }
            printf "%s", header
            candidate = 0; copying = 1; found++
        }
        next
    }
    copying { print }
    copying && /^}/ {
        copying = 0
        if (fifo) { print "#pragma GCC diagnostic pop"; fifo = 0 }
    }
    END { if (found != 4 || copying || candidate) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$tx_test_dir/tx-admission-flea.h"
awk '
    /^static BC_STATUS crystalhd_hw_tx_req_retire\(/ ||
    /^BC_STATUS crystalhd_hw_(post_tx|cancel_all_tx|tx_req_complete)\(/ ||
    /^bool crystalhd_hw_retain_tx_buffer\(/ ||
    /^void crystalhd_hw_retire_tx_quiesced\(/ ||
    /^void crystalhd_hw_dma_fatal_stop\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 7 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$tx_test_dir/tx-admission-hardware.h"
awk '
    /^struct crystalhd_tx_completion[[:space:]]*\{/ ||
    /^static BC_STATUS crystalhd_session_require_owner\(/ ||
    /^BC_STATUS crystalhd_fw_exec_locked\(/ ||
    /^BC_STATUS crystalhd_tx_deadline_from_ms\(/ ||
    /^static BC_STATUS crystalhd_bounded_tx_status\(/ ||
    /^static BC_STATUS bc_cproc_(do_fw_cmd|codein_sleep|check_inbuffs|proc_input)\(/ ||
    /^static BC_STATUS crystalhd_tx_transfer_common\(/ ||
    /^BC_STATUS crystalhd_tx_transfer_sync\(/ ||
    /^BC_STATUS crystalhd_tx_transfer_until\(/ ||
    /^static void bc_proc_in_completion\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 13 || copying) exit 1 }
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
