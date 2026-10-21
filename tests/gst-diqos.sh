#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build/run only the native DI helper and software GStreamer elements; no DIL.
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-diqos.XXXXXX")
trap 'rm -f -- "$test_dir/test" "$test_dir/registry.bin"; rmdir -- "$test_dir"' EXIT HUP INT TERM
packages="gstreamer-check-1.0 gstreamer-video-1.0"
${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror \
    $(pkg-config --cflags $packages) "$repo_dir/tests/gst-diqos.c" \
    -o "$test_dir/test" $(pkg-config --libs $packages)
GST_PLUGIN_PATH= GST_PLUGIN_PATH_1_0= GST_REGISTRY="$test_dir/registry.bin" \
    GST_REGISTRY_1_0="$test_dir/registry.bin" \
    timeout 20 "$test_dir/test" "$@"
