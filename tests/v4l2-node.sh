#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
node_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-v4l2-node.XXXXXX")
cleanup()
{
    rm -f "$node_test_dir/check" "$node_test_dir/node-types.h" \
        "$node_test_dir/node-production.h" "$node_test_dir/node-size.h" \
        "$node_test_dir/node-pause.h" "$node_test_dir/node-idle.h" \
        "$node_test_dir/node-constants.h" "$node_test_dir/node-queue.h"
    rmdir "$node_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
awk '
    /^static inline unsigned int crystalhd_v4l2_num_buffers\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_v4l2_compat.h" > "$node_test_dir/node-queue.h"
awk '
    /^#define (CHD_DISCOVERY_BUFFERS|CHD_TX_TIMEOUT_MS|CHD_CODED_SIZE) / { print; found++ }
    END { if (found != 3) exit 1 }
' "$repo_dir/driver/linux/crystalhd_v4l2_node.c" > "$node_test_dir/node-constants.h"
awk '
    /^#define CRYSTALHD_V4L2_TIMESTAMPS / { print; found++ }
    END { if (found != 1) exit 1 }
' "$repo_dir/driver/linux/crystalhd_v4l2_decoder.h" >> "$node_test_dir/node-constants.h"
awk '
    /^#define CRYSTALHD_PICTURE_FLAG_DECODE_ERROR / { print; found++ }
    END { if (found != 1) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fw_if.h" >> "$node_test_dir/node-constants.h"
awk '
    /^[[:space:]]*BC_(RX_LIST_CNT|PCI_DEVID_FLEA)[[:space:]]*=/ {
        value = $3; sub(/,/, "", value); print "#define " $1 " " value; found++
    }
    END { if (found != 2) exit 1 }
' "$repo_dir/include/bc_dts_glob_lnx.h" >> "$node_test_dir/node-constants.h"

# These calls would invalidate the zero-private-work lifetime proof. Check all
# frontend implementation files, including helpers introduced after this test.
if grep -En 'v4l2_m2m_(job_finish|buf_done_and_job_finish|try_schedule|resume|schedule_next_job)[[:space:]]*\(' \
    "$repo_dir"/driver/linux/crystalhd_v4l2*.c; then
    echo 'V4L2 native scheduler must not enqueue framework-private job work' >&2
    exit 1
fi
awk '
    /^static int chd_job_ready\(/ { ready = 1; found++ }
    ready { body = body $0 }
    ready && /^}/ { ready = 0 }
    /\.job_ready[[:space:]]*=[[:space:]]*chd_job_ready[[:space:]]*,/ { binding++ }
    /\.device_run[[:space:]]*=[[:space:]]*chd_device_run[[:space:]]*,/ { device++ }
    END {
        gsub(/[[:space:]]/, "", body)
        if (found != 1 || binding != 1 || device != 1 || ready ||
            body != "staticintchd_job_ready(void*private){(void)private;return0;}") exit 1
    }
' "$repo_dir/driver/linux/crystalhd_v4l2_node.c"
awk '
    /^struct crystalhd_v4l2_(node|discovery|file) \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_v4l2_node.c" > "$node_test_dir/node-types.h"
awk '
    BEGIN {
        count = split("chd_file chd_kick chd_admit chd_error chd_copy_colors chd_default_colorspace chd_normalize_colors chd_source_colors chd_discovery_free chd_discovery_alloc chd_receive chd_finish_last chd_input_ready chd_next_input chd_empty_drain chd_tx_run chd_schedule_tx chd_join chd_reset_channel chd_job_ready chd_device_run chd_file_destroy chd_try_fmt_locked chd_set_fmt chd_reqbufs chd_qbuf chd_resume chd_reuse_capture chd_streamon chd_streamoff chd_try_decoder_cmd chd_decoder_cmd", names, " ")
        for (i = 1; i <= count; i++) selected[names[i]] = 1
    }
    /^static .*chd_[[:alnum:]_]+\(/ {
        name = $0
        sub(/\(.*/, "", name)
        sub(/^.*[ *]/, "", name)
        if (name in selected) {
            if (seen[name]++) exit 1
            copying = 1; found++
        }
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != count || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_v4l2_node.c" > "$node_test_dir/node-production.h"
awk '
    /^int crystalhd_v4l2_capture_size\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_v4l2_buffers.c" > "$node_test_dir/node-size.h"
awk '
    /^BC_STATUS crystalhd_rx_pause_format\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$node_test_dir/node-pause.h"
awk '
    /^bool crystalhd_hw_rx_idle\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$node_test_dir/node-idle.h"

for node_queue_api in old new; do
for node_sanitize in no yes; do
    node_extra=
    if [ "$node_sanitize" = yes ]; then
        node_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    if [ "$node_queue_api" = new ]; then
        node_extra="$node_extra -DNODE_NEW_QUEUE_API"
    fi
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $node_extra -I"$node_test_dir" -I"$repo_dir/include" "$repo_dir/tests/v4l2-node.c" \
        -o "$node_test_dir/check"
    printf 'V4L2 native node guards: queue-api=%s sanitizers=%s\n' "$node_queue_api" "$node_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$node_test_dir/check"
done
done
