#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-flea-dram.XXXXXX")
cleanup()
{
    rm -f "$test_dir/check" "$test_dir/flea-dram-functions.h" \
        "$test_dir/flea-dram-commands.h" "$test_dir/flea-dram-types.h"
    rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Require each exact production entry once, with its whole brace-delimited
# body. Function signatures and column-zero closing braces are intentional
# extraction fuses; a source-format change must update this test explicitly.
awk '
    /^static uint32_t crystalhd_flea_dram_burst\(/ ||
    /^BC_STATUS crystalhd_flea_mem_(rd|wr)\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/flea-dram-functions.h"
awk '
    /^static BC_STATUS bc_cproc_(link_)?reg_wr\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$test_dir/flea-dram-commands.h"
awk '
    /^enum _BC_PCI_DEV_IDS[[:space:]]*\{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/include/bc_dts_glob_lnx.h" > "$test_dir/flea-dram-types.h"

for sanitize in no yes; do
    extra=
    if [ "$sanitize" = yes ]; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
        $extra -I"$repo_dir/include" -I"$repo_dir/include/flea" \
        -I"$repo_dir/driver/linux" -I"$test_dir" \
        "$repo_dir/tests/flea-dram.c" -o "$test_dir/check"
    printf 'Flea DRAM: sanitizers=%s\n' "$sanitize"
    timeout --kill-after=5s 20s env \
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$test_dir/check"
done
