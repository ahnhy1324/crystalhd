#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
probe_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-v4l2-probe.XXXXXX")
cleanup() { rm -f "$probe_dir/v4l2-decode"; rmdir "$probe_dir"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
gpu_flags=
gpu_libs=
if [ "${CRYSTALHD_TEST_GPU:-0}" = 1 ]; then
    gpu_flags="-DCRYSTALHD_TEST_GPU $(pkg-config --cflags egl glesv2 gbm libdrm)"
    gpu_libs=$(pkg-config --libs egl glesv2 gbm libdrm)
fi
"${CXX:-c++}" ${CXXFLAGS:-} -std=c++17 -O2 -g -Wall -Wextra -Werror \
    $gpu_flags \
    -I"$repo_dir/include" \
    -I"$repo_dir/linux_lib/libcrystalhd" \
    $(pkg-config --cflags libavformat libavcodec libavutil) \
    "$repo_dir/tests/v4l2-decode.cpp" \
    $(pkg-config --libs libavformat libavcodec libavutil) $gpu_libs -ldl -o "$probe_dir/v4l2-decode"
timeout --foreground "${CRYSTALHD_TEST_TIMEOUT:-180}" "$probe_dir/v4l2-decode" "$@"
