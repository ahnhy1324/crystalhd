#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

hardware=0
build_bits=32
case "${1:-}" in
    "") ;;
    --hardware) hardware=1; build_bits="32 64" ;;
    *) echo "usage: $0 [--hardware]" >&2; exit 2 ;;
esac
if [ "$#" -gt 1 ]; then
    echo "usage: $0 [--hardware]" >&2
    exit 2
fi

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cxx=${CXX:-c++}

# The project uses in-tree builds. Copy only the required source directories
# so parallel native builds and failed cross-builds keep their own artifacts.
for bits in $build_bits; do
    build_dir=$tmp_dir/$bits
    mkdir -p "$build_dir/linux_lib"
    cp -a "$repo_dir/include" "$build_dir/include"
    cp -a "$repo_dir/linux_lib/libcrystalhd" "$build_dir/linux_lib/libcrystalhd"
    cp -a "$repo_dir/examples" "$build_dir/examples"
    make -C "$build_dir/linux_lib/libcrystalhd" clean
    make -C "$build_dir/examples" clean
    make -C "$build_dir/linux_lib/libcrystalhd" CXX="$cxx -m$bits"
    make -C "$build_dir/examples" CXX="$cxx -m$bits"
    # Compiler and user flags are intentionally expanded into arguments.
    # shellcheck disable=SC2086
    $cxx ${CPPFLAGS:-} ${CXXFLAGS:-} -m"$bits" -std=c++11 \
        -Wall -Wextra -Werror -D__LINUX_USER__ \
        -I"$build_dir/include" -I"$build_dir/linux_lib/libcrystalhd" \
        "$repo_dir/tests/uapi-library-smoke.cpp" \
        -L"$build_dir/linux_lib/libcrystalhd" -lcrystalhd -pthread \
        -o "$build_dir/library-smoke"
    printf '%s-bit library, examples and library probe linked successfully\n' "$bits"
done

# Explicit opt-in only: requires an idle device, current driver and installed
# firmware. Opens the playback firmware, queries the API, then closes it;
# no decoder/capture session is started and no compressed input is submitted.
if [ "$hardware" -eq 1 ]; then
    for bits in $build_bits; do
        LD_LIBRARY_PATH="$tmp_dir/$bits/linux_lib/libcrystalhd" \
            timeout --kill-after=10 45 "$tmp_dir/$bits/library-smoke"
    done
fi
