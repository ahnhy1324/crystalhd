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
    # Execute copy/planar/format, framing, TX-flush, status and color regressions on the target ABI.
    # These tests neither open hardware nor load a shared library.
    for section in copy planar format input tx-flush status color; do
        section_wrap=
        case "$section" in
            copy|planar) set -- "$build_dir/linux_lib/libcrystalhd/libcrystalhd_int_if.cpp" ;;
            format)
                set -- "$build_dir/linux_lib/libcrystalhd/libcrystalhd_int_if.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_priv.cpp" ;;
            status|color)
                set -- "$build_dir/linux_lib/libcrystalhd/libcrystalhd_if.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_priv.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_int_if.cpp"
                section_wrap=-Wl,--wrap=ioctl ;;
            input)
                set -- "$build_dir/linux_lib/libcrystalhd/libcrystalhd_if.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_priv.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_parser.cpp"
                section_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep ;;
            tx-flush)
                set -- "$build_dir/linux_lib/libcrystalhd/libcrystalhd_if.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_priv.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_parser.cpp"
                section_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_unlock,--wrap=pthread_join
                section_wrap="$section_wrap -Wl,--wrap=DtsSetupHardware,--wrap=DtsOpenDecoder"
                section_wrap="$section_wrap -Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture,--wrap=DtsReleaseInterface,--wrap=_Z9WORD_SWAPt" ;;
        esac
        # Compiler and user flags are intentionally expanded into arguments.
        # shellcheck disable=SC2086
        $cxx ${CPPFLAGS:-} ${CXXFLAGS:-} -m"$bits" -msse2 -std=c++11 \
            -O1 -g -Wall -Werror -ffunction-sections -fdata-sections \
            -D__LINUX_USER__ -I"$build_dir/include" -I"$build_dir/include/link" \
            -I"$build_dir/linux_lib/libcrystalhd" \
            "$repo_dir/tests/library-$section.cpp" "$@" \
            -Wl,--gc-sections $section_wrap -pthread -o "$build_dir/library-$section"
        "$build_dir/library-$section"
    done
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
