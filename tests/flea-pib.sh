#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
pib_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-pib-check.XXXXXX")
cleanup()
{
    rm -f "$pib_test_dir/check" "$pib_test_dir/flea-pib-functions.h"
    rmdir "$pib_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Include exact production definitions, ignoring their forward declarations.
# The test's public wrapper and ready-queue path therefore execute the same
# parser; there is no parallel implementation of the metadata transformation.
awk '
    /^(static )?bool (flea_get_picture_info|flea_GetPictureInfo|crystalhd_flea_peek_next_decoded_frame)\(/ ||
    /^uint32_t flea_GetRptDropParam\(/ { candidate = 1; header = "" }
    candidate {
        header = header $0 "\n"
        if (/;[[:space:]]*$/) { candidate = 0; next }
        if (/^\{/) { printf "%s", header; candidate = 0; copying = 1; found++; next }
        next
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 4 || copying || candidate) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$pib_test_dir/flea-pib-functions.h"

for pib_sanitize in no yes; do
    pib_extra=
    if [ "$pib_sanitize" = yes ]; then
        pib_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # PicWidth is an unused existing production callback argument.
    # Splitting is deliberate for the fixed compiler-option list.
    "${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        $pib_extra -I"$repo_dir/include" -I"$pib_test_dir" \
        "$repo_dir/tests/flea-pib.c" -o "$pib_test_dir/check"
    printf 'Flea picture info: sanitizers=%s\n' "$pib_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$pib_test_dir/check"
done
