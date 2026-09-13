#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dma_test_dir=$(mktemp -d)
trap 'rm -rf "$dma_test_dir"' EXIT HUP INT TERM

# Compile the actual descriptor builders and their production data structures
# against a small DMA-address shim. No device or kernel module is required.
awk '
/^struct (dma_descriptor|dma_desc_mem|crystalhd_dio_user_info|crystalhd_dio_req) \{/ { copy = 1 }
copy { print }
copy && /^};/ { copy = 0 }
' "$repo_dir/driver/linux/crystalhd_hw.h" \
  "$repo_dir/driver/linux/crystalhd_misc.h" > "$dma_test_dir/dma-types.h"

awk '
/^BC_STATUS crystalhd_hw_fill_desc\(/ { copy = 1 }
/^BC_STATUS crystalhd_xlat_sgl_to_dma_desc\(/ { copy = 1 }
copy { print }
copy && /^}/ { copy = 0 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$dma_test_dir/dma-builders.h"

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
/^BC_STATUS crystalhd_hw_stop_capture\(/ { copy = 1 }
/^BC_STATUS crystalhd_hw_stop_capture_locked\(/ { copy = 1 }
/^static BC_STATUS bc_cproc_flush_cap_buffs\(/ { copy = 1 }
copy { print }
copy && /^}/ { copy = 0 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" \
  "$repo_dir/driver/linux/crystalhd_hw.c" \
  "$repo_dir/driver/linux/crystalhd_cmds.c" > "$dma_test_dir/dma-stop-functions.h"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$dma_test_dir" "$repo_dir/tests/dma-stop.c" -o "$dma_test_dir/dma-stop"
"$dma_test_dir/dma-stop"
