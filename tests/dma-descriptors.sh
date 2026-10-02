#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dma_test_dir=$(mktemp -d)
trap 'rm -rf "$dma_test_dir"' EXIT HUP INT TERM

# Compile the actual descriptor builders and their production data structures
# against a small DMA-address shim. No device or kernel module is required.
awk '
/^#define CRYSTALHD_DMA_DESC_MAX_XFER_BYTES[[:space:]]/ { print }
/^enum _BC_DTS_GLOBALS \{/ { copy = 1 }
/^struct (dma_descriptor|dma_desc_mem|crystalhd_dma_desc_source|crystalhd_tx_buffer|crystalhd_rx_buffer|crystalhd_dio_user_info|crystalhd_dio_req) \{/ { copy = 1 }
copy { print }
copy && /^};/ { copy = 0 }
' "$repo_dir/include/bc_dts_glob_lnx.h" \
  "$repo_dir/driver/linux/crystalhd_hw.h" \
  "$repo_dir/driver/linux/crystalhd_misc.h" > "$dma_test_dir/dma-types.h"

awk '
/^static BC_STATUS crystalhd_tx_buffer_preflight\(/ ||
/^BC_STATUS crystalhd_(hw_fill_desc|xlat_dma_to_desc|xlat_tx_buffer_to_dma_desc|xlat_rx_buffer_to_dma_desc)\(/ {
	copy = 1
	found++
}
copy { print }
copy && /^}/ { copy = 0 }
END { if (found != 5 || copy) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$dma_test_dir/dma-builders.h"

awk '
/^BC_STATUS crystalhd_hw_setup_dma_rings\(/ { copy = 1; found++ }
copy { print }
copy && /^}/ { copy = 0 }
END { if (found != 1 || copy) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$dma_test_dir/dma-setup.h"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$dma_test_dir" "$repo_dir/tests/dma-descriptors.c" \
  -o "$dma_test_dir/dma-descriptors"
"$dma_test_dir/dma-descriptors"

awk '
    /^#define FLEA_GISB_(DIRECT_BASE|INDIRECT_ADDRESS|INDIRECT_DATA)[[:space:]]/ {
        print; found++
    }
    END { if (found != 3) exit 1 }
' "$repo_dir/driver/linux/FleaDefs.h" > "$dma_test_dir/flea-register-addresses.h"
awk '
    /^uint32_t crystalhd_flea_reg_rd\(/ { copy = 1; read_found++ }
    /^void crystalhd_flea_reg_wr\(/ { copy = 1; write_found++ }
    copy { print }
    copy && /^}/ { copy = 0 }
    END { if (read_found != 1 || write_found != 1 || copy) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$dma_test_dir/flea-register-functions.h"
for register_sanitize in no address undefined; do
    register_extra=
    if [ "$register_sanitize" != no ]; then
        register_extra="-fsanitize=$register_sanitize -fno-omit-frame-pointer -fno-pie -no-pie"
    fi
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror $register_extra \
        -I"$dma_test_dir" "$repo_dir/tests/flea-registers.c" -o "$dma_test_dir/flea-registers"
    printf 'Flea registers: sanitizers=%s\n' "$register_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$dma_test_dir/flea-registers"
done

awk '
/^enum list_sts \{/ { copy = 1 }
/^union FLEA_INTR_BITS_COMMON$/ { copy = 1 }
copy { print }
copy && /^};/ { copy = 0 }
' "$repo_dir/driver/linux/crystalhd_hw.h" \
  "$repo_dir/driver/linux/FleaDefs.h" > "$dma_test_dir/dma-stop-types.h"

awk '
/^void crystalhd_flea_stop_rx_dma_engine\(/ { copy = 1 }
/^void crystalhd_hw_dma_fatal_stop\(/ { copy = 1; fatal_found++ }
/^bool crystalhd_hw_ack_fault_interrupt\(/ { copy = 1; ack_found++ }
/^static bool crystalhd_hw_dma_inventory_empty\(/ { copy = 1; inventory_found++ }
/^BC_STATUS crystalhd_hw_free_dma_rings\(/ { copy = 1; free_found++ }
/^static unsigned int crystalhd_hw_detach_rx_owners\(/ { copy = 1; helper_found++ }
/^BC_STATUS crystalhd_hw_stop_capture\(/ { copy = 1 }
/^BC_STATUS crystalhd_hw_stop_capture_locked\(/ { copy = 1 }
/^BC_STATUS crystalhd_capture_flush\(/ { copy = 1; command_found++ }
/^static BC_STATUS bc_cproc_flush_cap_buffs\(/ { copy = 1; command_found++ }
copy { print }
copy && /^}/ { copy = 0 }
END { if (command_found != 2 || helper_found != 1 || fatal_found != 1 || ack_found != 1 || inventory_found != 1 || free_found != 1 || copy) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" \
  "$repo_dir/driver/linux/crystalhd_hw.c" \
  "$repo_dir/driver/linux/crystalhd_cmds.c" > "$dma_test_dir/dma-stop-functions.h"

for stop_sanitize in no yes; do
  stop_extra=
  if [ "$stop_sanitize" = yes ]; then
    stop_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
  fi
  "${CC:-cc}" ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror $stop_extra \
    -I"$dma_test_dir" "$repo_dir/tests/dma-stop.c" -o "$dma_test_dir/dma-stop"
  printf 'DMA stop: sanitizers=%s\n' "$stop_sanitize"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$dma_test_dir/dma-stop"
done

awk '
    /^#define[[:space:]]+MAX_VALID_POLL_CNT[[:space:]]/ { print; found++ }
    END { if (found != 1) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.h" > "$dma_test_dir/flea-reset-limit.h"
awk '
    /^bool crystalhd_flea_(core_reset|start_device|stop_device)\(/ {
        if (/;[[:space:]]*$/) next
        copy = 1; found++
    }
    copy { print }
    copy && /^}/ { copy = 0 }
    END { if (found != 3 || copy) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$dma_test_dir/flea-reset-functions.h"
for reset_sanitize in no yes; do
    reset_extra=
    if [ "$reset_sanitize" = yes ]; then
        reset_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror $reset_extra \
        -I"$repo_dir/driver/linux" -I"$repo_dir/include/flea" -I"$dma_test_dir" \
        "$repo_dir/tests/flea-reset.c" -o "$dma_test_dir/flea-reset"
    printf 'Flea reset: sanitizers=%s\n' "$reset_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$dma_test_dir/flea-reset"
done
