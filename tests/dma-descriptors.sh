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
/^enum list_sts \{/ { copy = 1 }
/^union FLEA_INTR_BITS_COMMON$/ { copy = 1 }
copy { print }
copy && /^};/ { copy = 0 }
' "$repo_dir/driver/linux/crystalhd_hw.h" \
  "$repo_dir/driver/linux/FleaDefs.h" > "$dma_test_dir/dma-stop-types.h"

awk '
/^void crystalhd_flea_stop_rx_dma_engine\(/ { copy = 1 }
/^static unsigned int crystalhd_hw_detach_rx_owners\(/ { copy = 1; helper_found++ }
/^BC_STATUS crystalhd_hw_stop_capture\(/ { copy = 1 }
/^BC_STATUS crystalhd_hw_stop_capture_locked\(/ { copy = 1 }
/^BC_STATUS crystalhd_capture_flush\(/ { copy = 1; command_found++ }
/^static BC_STATUS bc_cproc_flush_cap_buffs\(/ { copy = 1; command_found++ }
copy { print }
copy && /^}/ { copy = 0 }
END { if (command_found != 2 || helper_found != 1 || copy) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" \
  "$repo_dir/driver/linux/crystalhd_hw.c" \
  "$repo_dir/driver/linux/crystalhd_cmds.c" > "$dma_test_dir/dma-stop-functions.h"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$dma_test_dir" "$repo_dir/tests/dma-stop.c" -o "$dma_test_dir/dma-stop"
"$dma_test_dir/dma-stop"
