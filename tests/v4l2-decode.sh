#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
probe_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-v4l2-probe.XXXXXX")
cleanup() { rm -f "$probe_dir/v4l2-decode"; rmdir "$probe_dir"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
"${CXX:-c++}" ${CXXFLAGS:-} -std=c++17 -O2 -g -Wall -Wextra -Werror \
    -I"$repo_dir/include" \
    -I"$repo_dir/linux_lib/libcrystalhd" \
    $(pkg-config --cflags libavformat libavcodec libavutil) \
    "$repo_dir/tests/v4l2-decode.cpp" \
    $(pkg-config --libs libavformat libavcodec libavutil) -ldl -o "$probe_dir/v4l2-decode"
timeout --foreground "${CRYSTALHD_TEST_TIMEOUT:-180}" "$probe_dir/v4l2-decode" "$@"
