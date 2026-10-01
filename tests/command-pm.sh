#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
pm_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-command-pm-check.XXXXXX")
cleanup()
{
    rm -f "$pm_test_dir/check" "$pm_test_dir/command-pm-types.h" \
        "$pm_test_dir/command-pm-hardware.h" "$pm_test_dir/command-pm-functions.h" \
        "$pm_test_dir/command-pm-request.h" "$pm_test_dir/command-pm-close.h"
    rmdir "$pm_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# These are the only session-resource allocation routes. Keep the graph tied
# to real call sites: comments/strings do not count, nor typed definitions and
# prototypes. A new route must extend this proof and its lifetime tests.
awk '
    BEGIN {
        caller["crystalhd_session_setup"] = "crystalhd_session_acquire_locked"
        caller["crystalhd_create_elem_pool"] = "crystalhd_session_setup"
        caller["crystalhd_create_dio_pool"] = "crystalhd_session_setup"
        caller["crystalhd_hw_setup_dma_rings"] = "crystalhd_session_setup"
        caller["crystalhd_stream_prepare"] = "crystalhd_decoder_channel_open_locked"
        apostrophe = sprintf("%c", 39)
    }
    # Small C lexical filter, preserving line boundaries for useful diagnostics.
    function code_only(line, out, i, c, next_c) {
        out = ""
        for (i = 1; i <= length(line); i++) {
            c = substr(line, i, 1)
            next_c = substr(line, i + 1, 1)
            if (block) {
                if (c == "*" && next_c == "/") { block = 0; i++ }
            } else if (quote != "") {
                if (escaped) escaped = 0
                else if (c == "\\") escaped = 1
                else if (c == quote) quote = ""
            } else if (c == "/" && next_c == "*") {
                block = 1; out = out " "; i++
            } else if (c == "/" && next_c == "/") {
                break
            } else if (c == "\"" || c == apostrophe) {
                quote = c; out = out " "
            } else out = out c
        }
        return out
    }
    FNR == 1 { block = escaped = 0; quote = previous = current = "" }
    {
        code = code_only($0)
        for (callee in caller) {
            parent = caller[callee]
            if (code ~ "^(static[[:space:]]+)?(BC_STATUS|int)[[:space:]]+" parent "[[:space:]]*\\(")
                current = parent
        }
        if (code ~ /^static[[:space:]]+BC_STATUS[[:space:]]+crystalhd_session_setup[[:space:]]*\(/)
            private_setup++
        rest = code
        while (match(rest, /[[:alpha:]_][[:alnum:]_]*|[^[:space:]]/)) {
            token = substr(rest, RSTART, RLENGTH)
            rest = substr(rest, RSTART + RLENGTH)
            if (token in caller && previous != "int" && previous != "BC_STATUS") {
                calls[token]++
                if (FILENAME !~ /\/crystalhd_cmds\.c$/ || current != caller[token]) {
                    printf "%s:%d: session resource route %s must be called only by %s\n", \
                        FILENAME, FNR, token, caller[token] > "/dev/stderr"
                    bad = 1
                }
            }
            previous = token
        }
        if (code ~ /^}/) current = ""
    }
    END {
        for (callee in caller)
            if (calls[callee] != 1) {
                printf "session resource route %s has %d uses, expected one\n", \
                    callee, calls[callee] > "/dev/stderr"
                bad = 1
            }
        if (private_setup != 1) {
            print "session setup must retain one private production entry" > "/dev/stderr"
            bad = 1
        }
        exit bad
    }
' "$repo_dir"/driver/linux/*.[ch]

# Use the real state constants and the exact command/hardware PM functions.
# Only device callbacks and allocation primitives are replaced by the test.
awk '
    /^enum (_crystalhd_state|crystalhd_decoder_phase|crystalhd_decoder_codec|_BC_PCI_DEV_IDS|_BC_DTS_GLOBALS|list_sts|LIST_STATUS|eC011_TS_CMD)[[:space:]{]/ {
        copying = 1; found++
    }
    /^struct (crystalhd_session_owner_ops|crystalhd_decoder_config|crystalhd_fw_init_cmd|crystalhd_fw_channel_open_cmd|crystalhd_fw_input_params_cmd|crystalhd_fw_channel_activate_cmd|crystalhd_fw_channel_start_video_cmd|crystalhd_fw_channel_stop_close_cmd|DecRspChannelChannelOpen|DecRspChannelStartVideo)[[:space:]{]/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+(DTS_MODE_INV|BC_LINK_ELEM_POOL_SZ|CRYSTALHD_(LINK|FLEA)_FIRMWARE_NAME)[[:space:]]/ { print }
    /^#define[[:space:]]+CRYSTALHD_FW_INIT_[A-Z_]+[[:space:]]/ { print }
    /^#define[[:space:]]+CRYSTALHD_FW_(CHANNEL_OPEN_WORDS|INPUT_PARAMS_WORDS|CHANNEL_ACTIVATE_WORDS|CHANNEL_START_VIDEO_WORDS|CHANNEL_STOP_CLOSE_WORDS|STREAM_TYPE_PES|VIDEO_ALGORITHM_H264|SYNC_MODE_SYNCPIN|DISPLAY_TIMING_IGNORE_PTS|USER_DATA_MODE_ON|PROGRESSIVE_OUTPUT|PICTURE_RELEASE_INTERNAL|LAST_PICTURE_DISPLAY_ON)[[:space:]]/ { print }
    /^#define[[:space:]]+eCMD_C011_CMD_BASE[[:space:]]/ { print }
    END { if (found != 18 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" "$repo_dir/include/bc_dts_glob_lnx.h" \
    "$repo_dir/driver/linux/crystalhd_hw.h" "$repo_dir/driver/linux/crystalhd_misc.h" \
    "$repo_dir/driver/linux/crystalhd_fw_if.h" \
    > "$pm_test_dir/command-pm-types.h"
awk '
    /^BC_STATUS crystalhd_hw_(close|suspend|resume)\(/ { copying = 1; found++ }
    /^void crystalhd_hw_dma_fatal_stop\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 4 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$pm_test_dir/command-pm-hardware.h"
awk '
    /^static struct crystalhd_user \*bc_cproc_get_uid\(/ ||
    (/^int bc_get_userhandle_count\(/ && !/;[[:space:]]*$/) ||
    /^static void bc_cproc_mark_pwr_state\(/ ||
    /^static void crystalhd_decoder_tracking_reset\(/ ||
    /^static void crystalhd_session_unpin\(/ ||
    /^static void crystalhd_session_retire_owner\(/ ||
    /^static BC_STATUS crystalhd_ensure_hw_context\(/ ||
    /^static BC_STATUS crystalhd_session_setup\(/ ||
    /^static bool crystalhd_retire_hw_context\(/ ||
    /^void crystalhd_rx_retire_quiesced\(/ ||
    /^BC_STATUS crystalhd_(setup|delete)_cmd_context\(/ ||
    /^BC_STATUS crystalhd_session_(acquire|release)_locked\(/ ||
    /^BC_STATUS crystalhd_session_acquire_ref_locked\(/ ||
    /^static BC_STATUS crystalhd_session_require_owner\(/ ||
    /^BC_STATUS crystalhd_user_set_mode\(/ ||
    /^static BC_STATUS bc_cproc_notify_mode\(/ ||
    /^static BC_STATUS bc_cproc_((link_)?reg|mem)_(rd|wr)\(/ ||
    /^BC_STATUS crystalhd_fw_download_locked\(/ ||
    /^int crystalhd_status_to_errno\(/ ||
    /^int crystalhd_request_firmware_locked\(/ ||
    /^static BC_STATUS bc_cproc_download_fw\(/ ||
    /^BC_STATUS crystalhd_fw_exec_locked\(/ ||
    /^int crystalhd_fw_bootstrap_locked\(/ ||
    /^int crystalhd_decoder_channel_open_locked\(/ ||
    /^int crystalhd_decoder_channel_start_locked\(/ ||
    /^static int crystalhd_decoder_release_cmd_locked\(/ ||
    /^int crystalhd_decoder_channel_(stop|close)_locked\(/ ||
    /^static BC_STATUS bc_cproc_do_fw_cmd\(/ ||
    /^void crystalhd_user_close\(/ ||
    (/^BC_STATUS bc_cproc_release_user\(/ && !/;[[:space:]]*$/) ||
    /^BC_STATUS crystalhd_(suspend|resume|user_open)\(/ { copying = 1; found++ }
    /^bool crystalhd_cmd_interrupt\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 42 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$pm_test_dir/command-pm-functions.h"

awk '
    /^int crystalhd_request_firmware_locked\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$pm_test_dir/command-pm-request.h"

# Every frontend must cross the same owner/state/recovery boundary before a
# low-level firmware loader can touch hardware.
fw_download_calls=$(grep -R -h --include='*.c' -c \
    'pfnFWDwnld[[:space:]]*(' "$repo_dir/driver/linux" | \
    awk '{ total += $1 } END { print total + 0 }')
if [ "$fw_download_calls" -ne 1 ] ||
    ! grep -Eq 'pfnFWDwnld\(ctx->hw_ctx, image,' \
        "$repo_dir/driver/linux/crystalhd_cmds.c" ||
    ! grep -Eq 'return crystalhd_fw_download_locked\(ctx,' \
        "$repo_dir/driver/linux/crystalhd_cmds.c"; then
    echo 'firmware download bypasses the shared command layer' >&2
    exit 1
fi

# The kernel firmware frontend owns request/release lifetime but delegates all
# owner, geometry, state and recovery effects to the shared download helper.
request_calls=$(grep -R -h --include='*.c' -c \
    'request_firmware[[:space:]]*(' "$repo_dir/driver/linux" | \
    awk '{ total += $1 } END { print total + 0 }')
release_calls=$(grep -R -h --include='*.c' -c \
    'release_firmware[[:space:]]*(' "$repo_dir/driver/linux" | \
    awk '{ total += $1 } END { print total + 0 }')
write_lock_assertions=$(grep -F -c \
    'lockdep_assert_held_write(&ctx->adp->user_lock);' \
    "$pm_test_dir/command-pm-request.h" || true)
if [ "$request_calls" -ne 1 ] || [ "$release_calls" -ne 1 ] ||
    [ "$write_lock_assertions" -ne 1 ] ||
    [ "$(grep -c 'crystalhd_fw_download_locked(ctx, owner,' \
        "$pm_test_dir/command-pm-request.h")" -ne 1 ] ||
    grep -Fq 'lockdep_assert_held(&ctx->adp->user_lock);' \
        "$pm_test_dir/command-pm-request.h" ||
    grep -Eq 'pfnFWDwnld[[:space:]]*\(|crystalhd_hw_fw_cmd_(recovery_enter|leave|reset_locked)[[:space:]]*\(' \
        "$pm_test_dir/command-pm-request.h"; then
    echo 'kernel firmware frontend bypasses or duplicates shared download ownership' >&2
    exit 1
fi

module_firmware_count=$(grep -c '^MODULE_FIRMWARE(' \
    "$repo_dir/driver/linux/crystalhd_lnx.c" || true)
if [ "$module_firmware_count" -ne 2 ] ||
    ! grep -Fxq 'MODULE_FIRMWARE(CRYSTALHD_LINK_FIRMWARE_NAME);' \
        "$repo_dir/driver/linux/crystalhd_lnx.c" ||
    ! grep -Fxq 'MODULE_FIRMWARE(CRYSTALHD_FLEA_FIRMWARE_NAME);' \
        "$repo_dir/driver/linux/crystalhd_lnx.c"; then
    echo 'kernel firmware metadata is incomplete or duplicated' >&2
    exit 1
fi

# Command-layer frontends must use the shared executor so mailbox admission,
# timeout quarantine and local rollback cannot be bypassed.
fw_exec_calls=$(grep -R -h --include='*.c' -c \
    'pfnDoFirmwareCmd[[:space:]]*(' "$repo_dir/driver/linux" | \
    awk '{ total += $1 } END { print total + 0 }')
if [ "$fw_exec_calls" -ne 1 ] ||
    ! grep -Eq 'pfnDoFirmwareCmd\(ctx->hw_ctx, fw_cmd\)' \
        "$repo_dir/driver/linux/crystalhd_cmds.c"; then
    echo 'firmware command execution bypasses the shared command layer' >&2
    exit 1
fi
awk '
    /^static int chd_dec_close(_locked)?\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$pm_test_dir/command-pm-close.h"

for pm_sanitize in no yes; do
    pm_extra=
    if [ "$pm_sanitize" = yes ]; then
        pm_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and fixed sanitizer flags are intentionally split into arguments.
    # The existing notify-mode code compares unsigned mode to its -1 sentinel.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-sign-compare \
        $pm_extra -I"$repo_dir/include" -I"$pm_test_dir" \
        "$repo_dir/tests/command-pm.c" -pthread -o "$pm_test_dir/check"
    printf 'Command PM: sanitizers=%s\n' "$pm_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$pm_test_dir/check"
done
