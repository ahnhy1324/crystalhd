#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-fw-command-observer.XXXXXX")
cleanup()
{
	rm -f "$test_dir/check" "$test_dir/default-off" "$test_dir/default-off.c" \
		"$test_dir/fw-command-observer-functions.h"
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
	/^static int crystalhd_fw_research_observer_register_locked\(/ ||
	/^void crystalhd_fw_research_command_posted_locked\(/ ||
	/^void crystalhd_fw_research_command_posted\(/ ||
	/^static int crystalhd_fw_research_observer_finish\(/) {
		copying = 1; found++
	}
	copying { print }
	copying && /^}/ { copying = 0 }
	END { if (found != 8 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fw_research.c" > \
	"$test_dir/fw-command-observer-functions.h"

cat > "$test_dir/default-off.c" <<'EOF'
struct crystalhd_hw;
typedef struct {
	unsigned cmd[64], rsp[64], flags, add_data;
} BC_FW_CMD;
#include "crystalhd_fw_research.h"

int main(void)
{
	crystalhd_fw_research_command_posted_locked((struct crystalhd_hw *)1,
		(BC_FW_CMD *)2);
	crystalhd_fw_research_command_posted((struct crystalhd_hw *)1,
		(BC_FW_CMD *)2);
	return 0;
}
EOF
"${CC:-cc}" ${CFLAGS:-} -std=c11 -O2 -Wall -Wextra -Werror \
	-I"$repo_dir/driver/linux" "$test_dir/default-off.c" \
	-o "$test_dir/default-off"
if nm "$test_dir/default-off" | grep -q 'crystalhd_fw_research_command_posted'; then
	echo 'default-off observer hooks emitted code symbols' >&2
	exit 1
fi
"$test_dir/default-off"
printf '%s\n' 'Firmware command observer: default-off hooks compile to no symbols or behavior'

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
