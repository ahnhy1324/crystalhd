#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
api_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-v4l2-api.XXXXXX")
cleanup() { rm -f "$api_test_dir/check"; rmdir "$api_test_dir"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
"${CC:-cc}" ${CPPFLAGS:-} ${CFLAGS:-} -std=c11 -O2 -g -Wall -Wextra -Werror \
    "$repo_dir/tests/v4l2-api.c" ${LDFLAGS:-} -o "$api_test_dir/check"
timeout --foreground "${CRYSTALHD_TEST_TIMEOUT:-30}" "$api_test_dir/check" "$@"
