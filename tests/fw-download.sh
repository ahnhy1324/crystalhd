#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-fw-download.XXXXXX")
cleanup()
{
	rm -f "$test_dir/check" "$test_dir/flea.h" "$test_dir/link.h" \
		"$test_dir/fw-download-functions.h"
	rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

extract_function()
{
	function_name=$1
	source_file=$2
	output_file=$3
	awk -v name="$function_name" '
		$0 ~ "^BC_STATUS " name "\\(" { candidate = 1; header = "" }
		candidate {
			header = header $0 "\n"
			if (/;[[:space:]]*$/) { candidate = 0; next }
			if (/^\{/) {
				printf "%s", header
				candidate = 0
				copying = 1
				found++
				next
			}
			next
		}
		copying { print }
		copying && /^}/ { copying = 0 }
		END { if (found != 1 || copying || candidate) exit 1 }
	' "$source_file" > "$output_file"
}

extract_function crystalhd_flea_download_fw \
	"$repo_dir/driver/linux/crystalhd_fleafuncs.c" "$test_dir/flea.h"
extract_function crystalhd_link_download_fw \
	"$repo_dir/driver/linux/crystalhd_linkfuncs.c" "$test_dir/link.h"
cat "$test_dir/flea.h" "$test_dir/link.h" > \
	"$test_dir/fw-download-functions.h"

check_order()
{
	function_file=$1
	subtraction=$2
	validation=$(grep -n -m1 'crystalhd_valid_firmware_image' \
		"$function_file" | cut -d: -f1)
	effect=$(grep -n -m1 -E \
		"pfn(Read|Write|DevDRAM)[[:alnum:]_]*[[:space:]]*\\(|GetScrubEndAddr|$subtraction" \
		"$function_file" | cut -d: -f1)
	[ -n "$validation" ] && [ -n "$effect" ] && [ "$validation" -lt "$effect" ]
}
check_order "$test_dir/flea.h" 'buffSz[[:space:]]*-'
check_order "$test_dir/link.h" 'sz[[:space:]]*-'

[ "$(grep -c 'crystalhd_valid_firmware_image' \
	"$test_dir/fw-download-functions.h")" -eq 2 ]
grep -q 'return BC_STS_TIMEOUT;' "$test_dir/link.h"
grep -q 'sts = hw->pfnDevDRAMWrite' "$test_dir/flea.h"
grep -q 'return sts;' "$test_dir/flea.h"
grep -A2 'exclusive = cmd == BCM_IOC_NOTIFY_MODE' \
	"$repo_dir/driver/linux/crystalhd_lnx.c" | \
	grep -q 'cmd == BCM_IOC_FW_DOWNLOAD'

flea_blob_size=$(wc -c < \
	"$repo_dir/firmware/fwbin/70015/bcm70015fw.bin")
link_blob_size=$(wc -c < \
	"$repo_dir/firmware/fwbin/70012/bcm70012fw.bin")

for sanitize in no yes; do
	extra=
	if [ "$sanitize" = yes ]; then
		extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
	fi
	# CFLAGS and fixed sanitizer flags intentionally expand as arguments.
	# shellcheck disable=SC2086
	"${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
		$extra -DFLEA_BLOB_SIZE="$flea_blob_size" \
		-DLINK_BLOB_SIZE="$link_blob_size" \
		-I"$repo_dir/include" -I"$repo_dir/include/flea" \
		-I"$repo_dir/include/link" -I"$repo_dir/driver/linux" \
		-I"$test_dir" "$repo_dir/tests/fw-download.c" \
		-o "$test_dir/check"
	printf 'Firmware download: sanitizers=%s\n' "$sanitize"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		"$test_dir/check"
done
