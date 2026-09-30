#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
pm_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-command-pm-check.XXXXXX")
cleanup()
{
    rm -f "$pm_test_dir/check" "$pm_test_dir/command-pm-types.h" \
        "$pm_test_dir/command-pm-hardware.h" "$pm_test_dir/command-pm-functions.h" \
        "$pm_test_dir/command-pm-close.h"
    rmdir "$pm_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Use the real state constants and the exact command/hardware PM functions.
# Only device callbacks and allocation primitives are replaced by the test.
awk '
    /^enum (_crystalhd_state|_BC_PCI_DEV_IDS|_BC_DTS_GLOBALS|list_sts|LIST_STATUS)[[:space:]{]/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+(DTS_MODE_INV|BC_LINK_ELEM_POOL_SZ)[[:space:]]/ { print }
    END { if (found != 5 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" "$repo_dir/include/bc_dts_glob_lnx.h" \
    "$repo_dir/driver/linux/crystalhd_hw.h" "$repo_dir/driver/linux/crystalhd_misc.h" \
    > "$pm_test_dir/command-pm-types.h"
awk '
    /^BC_STATUS crystalhd_hw_(close|suspend|resume)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$pm_test_dir/command-pm-hardware.h"
awk '
    /^static struct crystalhd_user \*bc_cproc_get_uid\(/ ||
    (/^int bc_get_userhandle_count\(/ && !/;[[:space:]]*$/) ||
    /^static void bc_cproc_mark_pwr_state\(/ ||
    /^static BC_STATUS crystalhd_ensure_hw_context\(/ ||
    /^static BC_STATUS crystalhd_session_setup\(/ ||
    /^static void crystalhd_retire_hw_context\(/ ||
    /^BC_STATUS crystalhd_session_(acquire|release)_locked\(/ ||
    /^static BC_STATUS crystalhd_session_require_owner\(/ ||
    /^BC_STATUS crystalhd_user_set_mode\(/ ||
    /^static BC_STATUS bc_cproc_notify_mode\(/ ||
    /^static BC_STATUS bc_cproc_((link_)?reg|mem)_(rd|wr)\(/ ||
    /^BC_STATUS crystalhd_fw_download_locked\(/ ||
    /^static BC_STATUS bc_cproc_download_fw\(/ ||
    /^BC_STATUS crystalhd_fw_exec_locked\(/ ||
    /^static BC_STATUS bc_cproc_do_fw_cmd\(/ ||
    /^void crystalhd_user_close\(/ ||
    (/^BC_STATUS bc_cproc_release_user\(/ && !/;[[:space:]]*$/) ||
    /^BC_STATUS crystalhd_(suspend|resume|user_open)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 26 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$pm_test_dir/command-pm-functions.h"

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
