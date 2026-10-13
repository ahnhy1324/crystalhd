#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-flea-power.XXXXXX")
cleanup()
{
    rm -f "$test_dir/check" "$test_dir/flea-power-functions.h" \
        "$test_dir/flea-power-types.h"
    rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Compile the actual initializer and selector, not a second predicate model.
# Split static signatures and column-zero closing braces are extraction fuses.
awk '
    /^void crystalhd_flea_(init_power_state|set_next_power_state)\(/ {
        if (previous != "static") exit 1
        print previous
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    { previous = $0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/flea-power-functions.h"
awk '
    /^enum (FLEA_POWER_STATES|FLEA_STATE_CH_EVENT|LIST_STATUS|list_sts)[[:space:]]*\{/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 4 || copying) exit 1 }
' "$repo_dir/driver/linux/FleaDefs.h" "$repo_dir/driver/linux/crystalhd_hw.h" \
    > "$test_dir/flea-power-types.h"

for sanitize in no yes; do
    extra=
    if [ "$sanitize" = yes ]; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $extra -I"$test_dir" "$repo_dir/tests/flea-power.c" -o "$test_dir/check"
    printf 'Flea power selector: sanitizers=%s\n' "$sanitize"
    timeout --kill-after=5s 20s env \
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$test_dir/check"
done
