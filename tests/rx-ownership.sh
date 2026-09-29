#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rx_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-rx-ownership.XXXXXX")
cleanup()
{
    rm -f "$rx_test_dir/check" "$rx_test_dir/rx-types.h" \
        "$rx_test_dir/rx-hardware.h" "$rx_test_dir/rx-command.h" \
        "$rx_test_dir/rx-post.h"
    rmdir "$rx_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Keep state values, buffer layouts and ownership functions tied to the driver.
awk '
    /^enum (_crystalhd_state|FLEA_POWER_STATES|BRCM_EVENT)[[:space:]]*\{/ ||
    /^struct (dma_descriptor|dma_desc_mem|crystalhd_rx_dma_pkt|crystalhd_dio_user_info)[[:space:]]*\{/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+DMA_ENGINE_CNT[[:space:]]/ { print }
    END { if (found != 7 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" \
    "$repo_dir/driver/linux/FleaDefs.h" "$repo_dir/driver/linux/crystalhd_hw.h" \
    "$repo_dir/driver/linux/crystalhd_misc.h" > "$rx_test_dir/rx-types.h"
awk '
    /^struct crystalhd_rx_dma_pkt \*crystalhd_hw_alloc_rx_pkt\(/ ||
    /^void crystalhd_(hw_free_rx_pkt|rx_pkt_rel_call_back)\(/ ||
    /^BC_STATUS crystalhd_rx_pkt_done\(/ ||
    /^BC_STATUS crystalhd_hw_(add_cap_buffer|get_cap_buffer|repost_cap_buffer|start_capture|stop_capture_locked|stop_capture)\(/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 10 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$rx_test_dir/rx-hardware.h"
awk '
    /^static BC_STATUS bc_cproc_(check_inbuffs|add_cap_buff|fmt_change|fetch_frame|start_capture|flush_cap_buffs)\(/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 6 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$rx_test_dir/rx-command.h"
awk '
    /^BC_STATUS crystalhd_(flea|link)_hw_post_cap_buff\(/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" \
    "$repo_dir/driver/linux/crystalhd_linkfuncs.c" > "$rx_test_dir/rx-post.h"

for rx_sanitize in no yes; do
    rx_extra=
    if [ "$rx_sanitize" = yes ]; then
        rx_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and the fixed sanitizer options are intentionally split.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $rx_extra -I"$repo_dir/include" -I"$repo_dir/include/link" -I"$repo_dir/driver/linux" \
        -I"$rx_test_dir" "$repo_dir/tests/rx-ownership.c" -o "$rx_test_dir/check"
    printf 'RX ownership: sanitizers=%s\n' "$rx_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$rx_test_dir/check"
done
