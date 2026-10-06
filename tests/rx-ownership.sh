#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rx_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-rx-ownership.XXXXXX")
cleanup()
{
	rm -f "$rx_test_dir/check" "$rx_test_dir/wait-check" \
		"$rx_test_dir/flea-isr-check" "$rx_test_dir/link-isr-check" \
		"$rx_test_dir/rx-flea-isr.h" "$rx_test_dir/rx-link-isr.h" \
		"$rx_test_dir/rx-flea-fll.h" \
		"$rx_test_dir/rx-isr-types.h" \
        "$rx_test_dir/rx-types.h" "$rx_test_dir/rx-fetch-wait-function.h" \
        "$rx_test_dir/rx-hardware.h" "$rx_test_dir/rx-command.h" \
        "$rx_test_dir/rx-post.h"
    rmdir "$rx_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Completion status belongs to packets/results, never the legacy DIO mapping.
awk '
    /^struct crystalhd_dio_user_info[[:space:]]*\{/ { copying = 1; found++ }
    copying && /(^|[^[:alnum:]_])(comp_flags|y_done_sz|uv_done_sz)([^[:alnum:]_]|$)/ {
        forbidden++
    }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying || forbidden) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.h"

# Keep state values, buffer layouts and ownership functions tied to the driver.
awk '
    /^enum (_crystalhd_state|FLEA_POWER_STATES|FLEA_STATE_CH_EVENT|BRCM_EVENT)[[:space:]]*\{/ ||
    /^struct (dma_descriptor|dma_desc_mem|crystalhd_rx_buffer_ops|crystalhd_rx_buffer|crystalhd_rx_metadata|crystalhd_rx_dma_pkt|crystalhd_rx_completion|crystalhd_hw_stats|crystalhd_dio_user_info)[[:space:]]*\{/ {
		copying = 1; found++
	}
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+DMA_ENGINE_CNT[[:space:]]/ { print }
	END { if (found != 13 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" \
    "$repo_dir/driver/linux/FleaDefs.h" "$repo_dir/driver/linux/crystalhd_hw.h" \
    "$repo_dir/driver/linux/crystalhd_misc.h" > "$rx_test_dir/rx-types.h"
awk '
    /^enum list_sts[[:space:]]*\{/ { copying = 1; found_enum++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+DMA_ENGINE_CNT[[:space:]]/ { print; found_count++ }
    END { if (found_enum != 1 || found_count != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.h" > "$rx_test_dir/rx-isr-types.h"
awk '
    /^void crystalhd_flea_rx_isr\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > \
    "$rx_test_dir/rx-flea-isr.h"
awk '
    /^void crystalhd_link_rx_isr\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_linkfuncs.c" > \
    "$rx_test_dir/rx-link-isr.h"
awk '
    /^(static )?struct crystalhd_rx_dma_pkt \*[[:space:]]*$/ {
        split_signature = $0
        awaiting_name = 1
        next
    }
    awaiting_name {
        if (/^crystalhd_hw_(fetch_retained_rx_pkt|fetch_free_rx_pkt)\(/) {
            print split_signature
            copying = 1
            found++
        }
        awaiting_name = 0
    }
    /^struct crystalhd_rx_dma_pkt \*crystalhd_(hw_alloc_rx_pkt|rx_pkt_detach)\(/ ||
    /^void crystalhd_(hw_free_rx_pkt|hw_retain_rx_pkt|hw_retire_rx_quiesced|hw_dma_fatal_stop|rx_pkt_rel_call_back)\(/ ||
    /^static unsigned int crystalhd_hw_detach_rx_owners\(/ ||
    /^uint32_t crystalhd_hw_count_free_rx_pkts\(/ ||
    /^void crystalhd_hw_stats\(/ ||
    /^BC_STATUS crystalhd_rx_pkt_(complete|done)\(/ ||
    /^static BC_STATUS crystalhd_hw_complete_rx_locked\(/ ||
    /^BC_STATUS crystalhd_hw_(add_cap_buffer|get_cap_buffer|try_get_cap_buffer|repost_cap_buffer|start_capture|stop_capture_locked|stop_capture)\(/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 22 || copying || awaiting_name) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$rx_test_dir/rx-hardware.h"
awk '
	/^BC_STATUS crystalhd_(rx_submit|rx_dequeue|rx_try_dequeue|rx_ack_format|capture_start|capture_flush)\(/ ||
	/^static BC_STATUS crystalhd_rx_dequeue_common\(/ ||
	/^static void bc_cproc_copy_pib\(/ ||
	/^static BC_STATUS bc_cproc_(check_inbuffs|add_cap_buff|fetch_frame|start_capture|flush_cap_buffs)\(/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
	END { if (found != 13 || copying) exit 1 }
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
awk '
    /^bool[[:space:]]*$/ { split_signature = $0; awaiting_name = 1; next }
    awaiting_name {
        if (/^crystalhd_flea_wake_up_hw\(/) {
            print split_signature; copying = 1; found++
        }
        awaiting_name = 0
    }
    /^static BC_STATUS crystalhd_flea_publish_fll\(/ ||
    /^void crystalhd_flea_notify_fll_change\(/ ||
    /^bool crystalhd_flea_notify_event\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 4 || copying || awaiting_name) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > \
    "$rx_test_dir/rx-flea-fll.h"
awk '
    /^enum _BC_DTS_GLOBALS[[:space:]]*\{/ { copying = 1; globals++ }
    /^static bool crystalhd_rx_accept_packet_locked\(/ ||
    /^void \*crystalhd_dioq_(fetch_wait|try_fetch_locked)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 3 || globals != 1 || copying) exit 1 }
' "$repo_dir/include/bc_dts_glob_lnx.h" \
    "$repo_dir/driver/linux/crystalhd_misc.c" > \
    "$rx_test_dir/rx-fetch-wait-function.h"

for rx_sanitize in no yes; do
    rx_extra=
    if [ "$rx_sanitize" = yes ]; then
        rx_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and the fixed sanitizer options are intentionally split.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $rx_extra -I"$repo_dir/include" -I"$repo_dir/include/link" -I"$repo_dir/driver/linux" \
        -I"$rx_test_dir" "$repo_dir/tests/rx-ownership.c" -o "$rx_test_dir/check"
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $rx_extra -I"$rx_test_dir" "$repo_dir/tests/rx-fetch-wait.c" \
        -o "$rx_test_dir/wait-check"
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $rx_extra -I"$repo_dir/include" -I"$repo_dir/include/link" \
        -I"$repo_dir/include/flea" \
        -I"$repo_dir/driver/linux" -I"$rx_test_dir" \
        "$repo_dir/tests/flea-rx-isr.c" -o "$rx_test_dir/flea-isr-check"
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $rx_extra -I"$repo_dir/include" -I"$repo_dir/include/link" \
        -I"$repo_dir/driver/linux" -I"$rx_test_dir" \
        "$repo_dir/tests/link-rx-isr.c" -o "$rx_test_dir/link-isr-check"
    printf 'RX ownership: sanitizers=%s\n' "$rx_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$rx_test_dir/check"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$rx_test_dir/wait-check"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$rx_test_dir/flea-isr-check"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$rx_test_dir/link-isr-check"
done
