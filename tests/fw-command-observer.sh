#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-fw-command-observer.XXXXXX")
cleanup()
{
	rm -f "$test_dir/check" "$test_dir/fw-command-observer-functions.h"
	rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Execute the exact arm/sample/finish implementation.  Hardware/register
# access is supplied by the host fixture and restricted to the three admitted
# fixed addresses.
awk '
	NR > 400 && (/^static void crystalhd_fw_research_observer_invalidate\(/ ||
	/^static int crystalhd_fw_research_observer_arm\(/ ||
	/^static int crystalhd_fw_research_observer_inflight_ready\(/ ||
	/^static int crystalhd_fw_research_observer_register\(/ ||
	/^void crystalhd_fw_research_command_posted\(/ ||
	/^static int crystalhd_fw_research_observer_finish\(/) {
		copying = 1; found++
	}
	copying { print }
	copying && /^}/ { copying = 0 }
	END { if (found != 6 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fw_research.c" > \
	"$test_dir/fw-command-observer-functions.h"

for sanitize in no yes; do
	extra=
	if [ "$sanitize" = yes ]; then
		extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
	fi
	# CFLAGS and sanitizer flags intentionally expand as arguments.
	"${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
		-pthread $extra -I"$repo_dir/include" -I"$test_dir" \
		"$repo_dir/tests/fw-command-observer.c" -o "$test_dir/check"
	printf 'Firmware command observer: sanitizers=%s\n' "$sanitize"
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$test_dir/check"
done
