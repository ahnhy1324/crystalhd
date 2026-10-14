#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work=$(mktemp -d /tmp/crystalhd-rx-output-receipts.XXXXXX)
trap 'rm -f "$work/plain" "$work/sanitized"; rmdir "$work"' EXIT HUP INT TERM

"${CXX:-c++}" -std=c++11 -O2 -g -Wall -Wextra -Werror \
    -D__LINUX_USER__ -I"$root/include" -I"$root/tests" \
    "$root/tests/rx-output-receipts.cpp" \
    "$root/tests/rx-output-receipts-test.cpp" -o "$work/plain"
"$work/plain"

"${CXX:-c++}" -std=c++11 -O1 -g -Wall -Wextra -Werror \
    -D__LINUX_USER__ -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/include" -I"$root/tests" \
    "$root/tests/rx-output-receipts.cpp" \
    "$root/tests/rx-output-receipts-test.cpp" -o "$work/sanitized"
ASAN_OPTIONS=detect_leaks=1 "$work/sanitized"
