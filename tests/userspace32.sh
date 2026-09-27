#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

hardware=0
legacy=0
build_bits=32
case "${1:-}" in
    "") ;;
    --hardware) hardware=1; build_bits="32 64" ;;
    --legacy) legacy=1 ;;
    *) echo "usage: $0 [--hardware|--legacy]" >&2; exit 2 ;;
esac
if [ "$#" -gt 1 ]; then
    echo "usage: $0 [--hardware|--legacy]" >&2
    exit 2
fi

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cxx=${CXX:-c++}
qemu=${QEMU_I386:-qemu-i386}
if [ "$legacy" -eq 1 ]; then
    command -v "$qemu" >/dev/null 2>&1 || {
        echo "legacy-cpu-check requires qemu-i386 (or QEMU_I386=/path/to/qemu-i386)" >&2
        exit 2
    }
    # Intentional illegal-instruction controls must not create core dumps.
    ulimit -c 0
fi
run_test() {
    if [ "$legacy" -eq 1 ]; then
        "$qemu" -cpu pentium2 "$@"
    else
        "$@"
    fi
}

# The project uses in-tree builds. Copy only the required source directories
# so parallel native builds and failed cross-builds keep their own artifacts.
for bits in $build_bits; do
    build_dir=$tmp_dir/$bits
    mkdir -p "$build_dir/linux_lib"
    cp -a "$repo_dir/include" "$build_dir/include"
    cp "$repo_dir/cpu.mk" "$build_dir/cpu.mk"
    cp -a "$repo_dir/linux_lib/libcrystalhd" "$build_dir/linux_lib/libcrystalhd"
    cp -a "$repo_dir/examples" "$build_dir/examples"
    make -C "$build_dir/linux_lib/libcrystalhd" LEGACY_CPU="$legacy" clean
    make -C "$build_dir/examples" LEGACY_CPU="$legacy" clean
    make -C "$build_dir/linux_lib/libcrystalhd" LEGACY_CPU="$legacy" CXX="$cxx -m$bits"
    make -C "$build_dir/examples" LEGACY_CPU="$legacy" CXX="$cxx -m$bits"
    cpu_flags=$(make -s --no-print-directory -C "$build_dir/linux_lib/libcrystalhd" \
        LEGACY_CPU="$legacy" CXX="$cxx -m$bits" print-cpu-flags)
    if [ "$legacy" -eq 1 ]; then
        # shellcheck disable=SC2086
        $cxx -std=c++11 -Wall -Wextra -Werror $cpu_flags \
            "$repo_dir/tests/legacy-cpu.cpp" -o "$build_dir/cpu-probe"
        run_test "$build_dir/cpu-probe"
        for instruction in --sse --sse2; do
            trap_status=0
            run_test "$build_dir/cpu-probe" "$instruction" || trap_status=$?
            if [ "$trap_status" -ne 132 ]; then
                echo "CPU model did not enforce $instruction SIGILL (status $trap_status)" >&2
                exit 1
            fi
        done
    fi
    # Compiler and user flags are intentionally expanded into arguments.
    # shellcheck disable=SC2086
    $cxx ${CPPFLAGS:-} ${CXXFLAGS:-} -m"$bits" -std=c++11 \
        -Wall -Wextra -Werror -D__LINUX_USER__ \
        -I"$build_dir/include" -I"$build_dir/linux_lib/libcrystalhd" \
        "$repo_dir/tests/uapi-library-smoke.cpp" \
        -L"$build_dir/linux_lib/libcrystalhd" -lcrystalhd -pthread \
        $cpu_flags -o "$build_dir/library-smoke"
    # Execute production library regressions on the target ABI and CPU.
    # These tests neither open hardware nor load a shared library.
    for section in copy planar format input tx-ring flush tx-flush eos status color; do
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
            tx-ring|flush)
                set -- "$build_dir/linux_lib/libcrystalhd/libcrystalhd_if.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_priv.cpp"
                section_wrap=-Wl,--wrap=pthread_mutex_lock
                if [ "$section" = flush ]; then
                    section_wrap="$section_wrap -Wl,--wrap=ioctl,--wrap=usleep"
                fi ;;
            tx-flush)
                set -- "$build_dir/linux_lib/libcrystalhd/libcrystalhd_if.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_priv.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_parser.cpp"
                section_wrap=-Wl,--wrap=ioctl,--wrap=usleep,--wrap=pthread_mutex_unlock,--wrap=pthread_join
                section_wrap="$section_wrap -Wl,--wrap=DtsSetupHardware,--wrap=DtsOpenDecoder"
                section_wrap="$section_wrap -Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture,--wrap=DtsReleaseInterface,--wrap=_Z9WORD_SWAPt" ;;
            eos)
                set -- "$build_dir/linux_lib/libcrystalhd/libcrystalhd_if.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_priv.cpp" \
                    "$build_dir/linux_lib/libcrystalhd/libcrystalhd_parser.cpp"
                section_wrap=-Wl,--wrap=ioctl,--wrap=txBufPush,--wrap=usleep
                section_wrap="$section_wrap -Wl,--wrap=DtsSetupHardware,--wrap=DtsOpenDecoder"
                section_wrap="$section_wrap -Wl,--wrap=DtsStartDecoder,--wrap=DtsStartCapture" ;;
        esac
        # Compiler and user flags are intentionally expanded into arguments.
        # shellcheck disable=SC2086
        $cxx ${CPPFLAGS:-} ${CXXFLAGS:-} -m"$bits" -std=c++11 \
            -O1 -g -Wall -Werror -ffunction-sections -fdata-sections \
            -D__LINUX_USER__ -I"$build_dir/include" -I"$build_dir/include/link" \
            -I"$build_dir/linux_lib/libcrystalhd" \
            "$repo_dir/tests/library-$section.cpp" "$@" \
            -Wl,--gc-sections $section_wrap -pthread $cpu_flags -o "$build_dir/library-$section"
        run_test "$build_dir/library-$section"
        if [ "$legacy" -eq 1 ]; then
            case "$section" in
                copy|planar|format)
                    # Also exercise the actual shared library, not just its
                    # separately compiled production sections. These three
                    # tests use local contexts and never open the device.
                    # shellcheck disable=SC2086
                    $cxx -std=c++11 -O1 -Wall -Werror -D__LINUX_USER__ \
                        -I"$build_dir/include" -I"$build_dir/include/link" \
                        -I"$build_dir/linux_lib/libcrystalhd" \
                        "$repo_dir/tests/library-$section.cpp" \
                        -L"$build_dir/linux_lib/libcrystalhd" -lcrystalhd -pthread \
                        $cpu_flags -o "$build_dir/shared-$section"
                    LD_LIBRARY_PATH="$build_dir/linux_lib/libcrystalhd" \
                        run_test "$build_dir/shared-$section" ;;
            esac
        fi
    done
    printf '%s-bit library, examples and library probe linked successfully\n' "$bits"
    if [ "$legacy" -eq 1 ]; then
        library_dir=$build_dir/linux_lib/libcrystalhd
        library_file=$library_dir/libcrystalhd.so.3.6
        # Inspect only our DSO; runtime dependencies have separate baselines.
        objdump -d "$library_file" > "$build_dir/legacy.disassembly"
        if grep -Eq '%([xyz]mm[0-9]+|mm[0-7])|[[:space:]](emms|femms|clflush|[lms]fence|[ls]dmxcsr)([[:space:]]|$)' \
            "$build_dir/legacy.disassembly"; then
            echo "legacy library contains SIMD/cache instructions" >&2
            exit 1
        fi
        # Switch the same source/output directory in BOTH directions without
        # clean. A stale native ABI or SSE2 DSO must not survive the switch.
        make -C "$library_dir" LEGACY_CPU=0 CXX="$cxx"
        objdump -d "$library_file" > "$build_dir/normal.disassembly"
        grep -Eq '%xmm[0-9]+' "$build_dir/normal.disassembly"
        make -C "$library_dir" LEGACY_CPU=1 CXX="$cxx -m32"
        objdump -d "$library_file" > "$build_dir/legacy-again.disassembly"
        if grep -Eq '%([xyz]mm[0-9]+|mm[0-7])|[[:space:]](emms|femms|clflush|[lms]fence|[ls]dmxcsr)([[:space:]]|$)' \
            "$build_dir/legacy-again.disassembly"; then
            echo "mode switch reused a SIMD library" >&2
            exit 1
        fi
        LD_LIBRARY_PATH="$library_dir" run_test "$build_dir/shared-format"
        # Compile the real VA-API cache/fence code without requiring 32-bit
        # graphics libraries. Section GC removes unrelated frontend functions.
        # shellcheck disable=SC2086
        $cxx -std=c++17 -O2 -Wall -Wextra -Werror -fno-tree-slp-vectorize \
            -ffunction-sections -fdata-sections -D__LINUX_USER__ \
            -I"$repo_dir/include" -I"$repo_dir/linux_lib/libcrystalhd" \
            $(pkg-config --cflags libva libva-drm libdrm gbm libswscale) \
            "$repo_dir/tests/vaapi-cache.cpp" \
            -Wl,--gc-sections,--wrap=open,--wrap=ioctl,--wrap=close \
            -pthread $cpu_flags -o "$build_dir/vaapi-cache"
        run_test "$build_dir/vaapi-cache"
        before_noop=$(stat -c '%y' "$library_file" "$library_dir/libcrystalhd_int_if.o")
        make -C "$library_dir" LEGACY_CPU=1 CXX="$cxx -m32"
        after_noop=$(stat -c '%y' "$library_file" "$library_dir/libcrystalhd_int_if.o")
        test "$before_noop" = "$after_noop"
        for unsafe_flags in '-O0 -msse2 -m64' '-O2 -mbmi2'; do
            if make -s -C "$library_dir" LEGACY_CPU=1 CXX="$cxx" \
                CXXFLAGS="$unsafe_flags" print-cpu-flags; then
                echo "legacy build accepted contradictory machine flags" >&2
                exit 1
            fi
        done
        printf 'Legacy CPU: real no-SSE execution, shared-library paths and mode-switch checks passed\n'
    fi
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
