#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lifetime_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-lifetime-check.XXXXXX")
cleanup()
{
    rm -f "$lifetime_test_dir/check" "$lifetime_test_dir/lifetime-functions.h" \
        "$lifetime_test_dir/lifetime-command.h" "$lifetime_test_dir/lifetime-binding.h"
    rmdir "$lifetime_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Exercise the production removal/close paths, including their resource helpers.
awk '
    /^struct crystalhd_file \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$lifetime_test_dir/lifetime-binding.h"
awk '
    /^void crystalhd_user_close\(/ ||
    /^BC_STATUS crystalhd_delete_cmd_context\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$lifetime_test_dir/lifetime-command.h"
awk '
    /^static int chd_dec_disable_int\(/ ||
    (/^crystalhd_ioctl_data \*chd_dec_alloc_iodata\(/ && !/;[[:space:]]*$/) ||
    /^static int chd_dec_close(_locked)?\(/ ||
    /^static void chd_dec_release_chdev\(/ ||
    /^static void chd_pci_release_mem\(/ ||
    /^static void chd_release_l0s\(/ ||
    /^static void chd_dec_fail_closed\(/ ||
    /^static void chd_dec_pci_remove\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 9 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$lifetime_test_dir/lifetime-functions.h"

for lifetime_sanitize in no yes; do
    lifetime_extra=
    if [ "$lifetime_sanitize" = yes ]; then
        lifetime_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and fixed sanitizer flags are intentionally split into arguments.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-parameter $lifetime_extra -I"$lifetime_test_dir" \
        "$repo_dir/tests/device-lifetime.c" -o "$lifetime_test_dir/check"
    printf 'Device lifetime: sanitizers=%s\n' "$lifetime_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$lifetime_test_dir/check"
done
