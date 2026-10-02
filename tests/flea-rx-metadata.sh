#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-flea-rx-metadata.XXXXXX")
cleanup()
{
	rm -f "$test_dir/check" "$test_dir/fire-rxdma.h" "$test_dir/rx-post.h" \
		"$test_dir/rx-types.h"
	rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Skip the forward declaration and require exactly one complete production body.
awk '
	/^BC_STATUS crystalhd_flea_hw_fire_rxdma\(/ {
		candidate = 1; header = ""
	}
	candidate {
		header = header $0 "\n"
		if (/;[[:space:]]*$/) { candidate = 0; next }
		if (/^\{/) {
			printf "%s", header
			candidate = 0; copying = 1; found++
		}
		next
	}
	copying { print }
	copying && /^}/ { copying = 0 }
	END { if (found != 1 || copying || candidate) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/fire-rxdma.h"

awk '
	/^BC_STATUS crystalhd_flea_hw_post_cap_buff\(/ { copying = 1; found++ }
	copying { print }
	copying && /^}/ { copying = 0 }
	END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/rx-post.h"

awk '
	/^typedef union _addr_64_[[:space:]]*\{/ { copying = 1; addresses++ }
	/^enum list_sts[[:space:]]*\{/ { copying = 1; lists++ }
	/^#define[[:space:]]+DMA_ENGINE_CNT[[:space:]]/ { print; counts++ }
	copying { print }
	copying && /^}( addr_64)?;/ { copying = 0 }
	END { if (addresses != 1 || lists != 1 || counts != 1 || copying) exit 1 }
' "$repo_dir/include/bc_dts_glob_lnx.h" \
	"$repo_dir/driver/linux/crystalhd_hw.h" > "$test_dir/rx-types.h"

# Where supported, poison otherwise-uninitialized automatic storage. This is a
# test-only compiler option; it makes a missing initializer regression visible.
init_flags=
if "${CC:-cc}" ${CFLAGS:-} -std=c11 -Werror -ftrivial-auto-var-init=pattern \
	-fsyntax-only -x c /dev/null >/dev/null 2>&1; then
	init_flags=-ftrivial-auto-var-init=pattern
fi

for sanitize in no yes; do
	extra=
	if [ "$sanitize" = yes ]; then
		extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
	fi
	# CFLAGS and the fixed test options intentionally expand as arguments.
	"${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
		$init_flags $extra -I"$repo_dir/include" -I"$repo_dir/include/flea" \
		-I"$repo_dir/driver/linux" -I"$test_dir" \
		"$repo_dir/tests/flea-rx-metadata.c" -o "$test_dir/check"
	printf 'Flea RX metadata: sanitizers=%s auto-init=%s\n' \
		"$sanitize" "${init_flags:-unsupported}"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$test_dir/check"
done
