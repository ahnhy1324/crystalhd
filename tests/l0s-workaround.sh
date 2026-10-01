#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
l0s_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-l0s-check.XXXXXX")
cleanup()
{
    rm -f "$l0s_test_dir/check" "$l0s_test_dir/l0s-irq-functions.h" \
        "$l0s_test_dir/l0s-retire-command.h"
    rmdir "$l0s_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

awk '
    /^void crystalhd_rx_retire_quiesced\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$l0s_test_dir/l0s-retire-command.h"

# Extract the actual IRQ/PM lifecycle functions, not parallel implementations.
awk '
    /^static int chd_dec_(enable|disable)_int\(/ ||
    /^static int chd_restore_l0s\(/ ||
    /^static void chd_dec_fail_closed\(/ ||
    (/^int chd_dec_pci_(suspend|resume)\(/ && !/;[[:space:]]*$/) {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 6 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$l0s_test_dir/l0s-irq-functions.h"

# The test includes the production helper and IRQ lifecycle, with PCI operations
# mocked before inclusion. Neither module nor libcrystalhd is built or opened.
for l0s_config in legacy current absent; do
    case "$l0s_config" in
        legacy) l0s_defines='-DCONFIG_PCIEASPM=1 -DTEST_API_L0S=1' ;;
        current) l0s_defines='-DCONFIG_PCIEASPM=1 -DTEST_API_L0S=3' ;;
        absent) l0s_defines='-DTEST_API_L0S=1' ;;
    esac
    for l0s_sanitize in no yes; do
        l0s_extra=
        if [ "$l0s_sanitize" = yes ]; then
            l0s_extra='-fsanitize=address,undefined -fno-omit-frame-pointer'
        fi
        # Word splitting is deliberate for these fixed compiler-option lists.
        "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
            $l0s_defines $l0s_extra -I"$repo_dir/driver/linux" -I"$l0s_test_dir" \
            "$repo_dir/tests/l0s-workaround.c" -o "$l0s_test_dir/check"
        printf 'L0s workaround: PCI API=%s, sanitizers=%s\n' \
            "$l0s_config" "$l0s_sanitize"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            "$l0s_test_dir/check"
    done
done
