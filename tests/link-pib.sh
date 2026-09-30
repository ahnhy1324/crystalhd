#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
link_pib_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-link-pib-check.XXXXXX")
cleanup()
{
	rm -f "$link_pib_test_dir/check" "$link_pib_test_dir/link-pib-functions.h"
	rmdir "$link_pib_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Compile the production parser, with only its buffer accessors and device
# logging supplied by the harness.
awk '
    /^uint32_t link_Get(PicInfoLineNum|Mode422Data|MetaDataFromPib|HeightFromPib|RptDropParam)\(/ ||
    /^static BC_STATUS link_rx_read\(/ ||
    /^bool link_GetPictureInfo\(/ { candidate = 1; header = "" }
    candidate {
        header = header $0 "\n"
        if (/;[[:space:]]*$/) { candidate = 0; next }
        if (/^\{/) { printf "%s", header; candidate = 0; copying = 1; found++; next }
        next
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 7 || copying || candidate) exit 1 }
' "$repo_dir/driver/linux/crystalhd_linkfuncs.c" > \
	"$link_pib_test_dir/link-pib-functions.h"

for link_pib_sanitize in no yes; do
	link_pib_extra=
	if [ "$link_pib_sanitize" = yes ]; then
		link_pib_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
	fi
	# Splitting is deliberate for the fixed compiler-option list.
	"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
		$link_pib_extra -I"$repo_dir/include" -I"$link_pib_test_dir" \
		"$repo_dir/tests/link-pib.c" -o "$link_pib_test_dir/check"
	printf 'Link picture info: sanitizers=%s\n' "$link_pib_sanitize"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$link_pib_test_dir/check"
done
