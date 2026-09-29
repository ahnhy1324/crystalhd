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
    /^enum (_crystalhd_state|_BC_DTS_GLOBALS|list_sts|LIST_STATUS)[[:space:]{]/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+(DTS_MODE_INV|BC_LINK_ELEM_POOL_SZ)[[:space:]]/ { print }
    END { if (found != 4 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" "$repo_dir/include/bc_dts_glob_lnx.h" \
    "$repo_dir/driver/linux/crystalhd_hw.h" "$repo_dir/driver/linux/crystalhd_misc.h" \
    > "$pm_test_dir/command-pm-types.h"
awk '
    /^BC_STATUS crystalhd_hw_(suspend|resume)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_hw.c" > "$pm_test_dir/command-pm-hardware.h"
awk '
    /^static struct crystalhd_user \*bc_cproc_get_uid\(/ ||
    (/^int bc_get_userhandle_count\(/ && !/;[[:space:]]*$/) ||
    /^static void bc_cproc_mark_pwr_state\(/ ||
    /^static BC_STATUS crystalhd_session_setup\(/ ||
    /^BC_STATUS crystalhd_user_set_mode\(/ ||
    /^static BC_STATUS bc_cproc_notify_mode\(/ ||
    /^static BC_STATUS bc_cproc_download_fw\(/ ||
    /^static BC_STATUS bc_cproc_do_fw_cmd\(/ ||
    /^void crystalhd_user_close\(/ ||
    (/^BC_STATUS bc_cproc_release_user\(/ && !/;[[:space:]]*$/) ||
    /^BC_STATUS crystalhd_(suspend|resume|user_open)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 13 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$pm_test_dir/command-pm-functions.h"
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
