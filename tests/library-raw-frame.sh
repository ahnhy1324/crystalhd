#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Only the new host serializer and a stubbed DtsProcInput are linked. No card,
# driver, installed library or decoder lifecycle operation is reachable.
set -eu

case "$#:${1:-}" in
    0:) output_dir= ;;
    2:--emit) output_dir=$2 ;;
    *) echo "usage: $0 [--emit NEW_OUTPUT_DIRECTORY]" >&2; exit 2 ;;
esac
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
raw_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-raw-frame-check.XXXXXX")
cleanup()
{
    raw_status=$?
    if [ "$raw_status" -ne 0 ]; then
        echo "Raw-frame CPU failure artifacts retained in $raw_test_dir" >&2
        return
    fi
    if [ "$keep_outputs" -eq 0 ]; then
        for name in checker mirror extreme; do
            rm -f "$raw_test_dir/emitted/$name.h264" \
                "$raw_test_dir/emitted/$name.yuv420" \
                "$raw_test_dir/emitted/$name.output-chain.yuy2" \
                "$raw_test_dir/emitted/$name.ffmpeg.yuv420" \
                "$raw_test_dir/emitted/$name.ffmpeg.log" \
                "$raw_test_dir/emitted/$name.ffprobe.csv" \
                "$raw_test_dir/emitted/$name.ffprobe.log"
        done
        rmdir "$raw_test_dir/emitted"
    fi
    rm -f "$raw_test_dir/check" "$raw_test_dir/check-sanitize" \
        "$raw_test_dir/check-legacy" "$raw_test_dir/header-c.o" \
        "$raw_test_dir/raw-frame-legacy.o" "$raw_test_dir/legacy.disassembly" \
        "$raw_test_dir/existing-output.log"
    rmdir "$raw_test_dir"
}
keep_outputs=0
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
if [ -z "$output_dir" ]; then output_dir=$raw_test_dir/emitted; else keep_outputs=1; fi
cxx=${CXX:-c++}
cc=${CC:-cc}
qemu=${QEMU_I386:-qemu-i386}
for command in "$qemu" ffmpeg ffprobe sha256sum objdump; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "raw-frame CPU check requires $command" >&2; exit 2;
    }
done
native_flags=$(make -s --no-print-directory -C "$repo_dir/linux_lib/libcrystalhd" \
    LEGACY_CPU=0 print-cpu-flags)
legacy_flags=$(make -s --no-print-directory -C "$repo_dir/linux_lib/libcrystalhd" \
    LEGACY_CPU=1 CXX="$cxx -m32" print-cpu-flags)

# The public header must be usable from C, without C++ declarations.
printf '%s\n' '#include "libcrystalhd_raw_frame.h"' \
    '_Static_assert(BC_RAW_FRAME_WIDTH == 256, "width");' \
    '_Static_assert(BC_RAW_FRAME_MASK_BYTES == 96, "mask");' \
    'int main(void) {' \
    'BC_RAW_FRAME_BUILDER *b = 0; BC_RAW_FRAME_PLANES p = {{0,0,0},{256,128,128},{24576,6144,6144}};' \
    '(void)b; (void)p; return 0; }' |
    "$cc" ${CPPFLAGS:-} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror -D__LINUX_USER__ \
        -I"$repo_dir/include" -I"$repo_dir/linux_lib/libcrystalhd" \
        -x c - -c -o "$raw_test_dir/header-c.o"

for mode in native sanitize legacy; do
    extra=$native_flags
    binary=$raw_test_dir/check
    if [ "$mode" = sanitize ]; then
        extra="$native_flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie"
        binary=$raw_test_dir/check-sanitize
    elif [ "$mode" = legacy ]; then
        extra=$legacy_flags
        binary=$raw_test_dir/check-legacy
    fi
    # Compiler/user flags are intentionally expanded, as in userspace32.sh.
    "$cxx" ${CPPFLAGS:-} ${CXXFLAGS:-} -std=c++11 -O1 -g -Wall -Wextra -Werror \
        -D__LINUX_USER__ -I"$repo_dir/include" -I"$repo_dir/linux_lib/libcrystalhd" \
        "$repo_dir/tests/library-raw-frame.cpp" \
        "$repo_dir/linux_lib/libcrystalhd/libcrystalhd_raw_frame.cpp" \
        -Wl,--wrap=calloc $extra -o "$binary"
    printf 'Public raw-frame API: mode=%s\n' "$mode"
    if [ "$mode" = native ]; then
        "$binary" --emit "$output_dir"
    elif [ "$mode" = sanitize ]; then
        ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$binary"
    else
        "$qemu" -cpu pentium2 "$binary"
    fi
done

# Audit only this production object, not unrelated runtime dependencies.
"$cxx" ${CPPFLAGS:-} ${CXXFLAGS:-} -std=c++11 -O1 -Wall -Wextra -Werror \
    -D__LINUX_USER__ -I"$repo_dir/include" -I"$repo_dir/linux_lib/libcrystalhd" \
    $legacy_flags -c "$repo_dir/linux_lib/libcrystalhd/libcrystalhd_raw_frame.cpp" \
    -o "$raw_test_dir/raw-frame-legacy.o"
objdump -d "$raw_test_dir/raw-frame-legacy.o" > "$raw_test_dir/legacy.disassembly"
if grep -Eq '%([xyz]mm[0-9]+|mm[0-7])|[[:space:]](emms|femms|clflush|[lms]fence|[ls]dmxcsr)([[:space:]]|$)' \
    "$raw_test_dir/legacy.disassembly"; then
    echo "raw-frame legacy object contains SIMD/cache instructions" >&2; exit 1
fi

# Exact serializer identity with the already qualified legal carrier, plus a
# separately constructed caller-source/tile oracle. No old private file is read.
checker_sha=$(sha256sum "$output_dir/checker.h264"); checker_sha=${checker_sha%% *}
planar_sha=$(sha256sum "$output_dir/checker.yuv420"); planar_sha=${planar_sha%% *}
[ "$checker_sha" = 08d2859e37449c591b746b478e83dc26948e6deaa299dd7d26b97c2ffc47654c ]
[ "$planar_sha" = 58591b2dd3ef391873e684b004a8464e6ff6c6abe72c6d299ec2ff709882cb76 ]

for name in checker mirror extreme; do
    ffmpeg -nostdin -hide_banner -loglevel error -threads 1 -hwaccel none \
        -noautorotate -err_detect explode -xerror -i "$output_dir/$name.h264" \
        -map 0:v:0 -an -sn -dn -fps_mode passthrough -threads 1 \
        -pix_fmt yuv420p -f rawvideo -n "$output_dir/$name.ffmpeg.yuv420" \
        2> "$output_dir/$name.ffmpeg.log"
    [ ! -s "$output_dir/$name.ffmpeg.log" ]
    cmp "$output_dir/$name.yuv420" "$output_dir/$name.ffmpeg.yuv420"
    ffprobe -v error -select_streams v:0 \
        -show_entries frame=key_frame,width,height,pict_type -of csv=p=0 \
        "$output_dir/$name.h264" > "$output_dir/$name.ffprobe.csv" \
        2> "$output_dir/$name.ffprobe.log"
    [ ! -s "$output_dir/$name.ffprobe.log" ]
    awk -F, -v name="$name" '
        { f=NR-1; isI=(f==0 || f==1 || (name=="extreme" ? f==4 : f==90));
          if (NF!=4 || $1!=(f==0) || $2!=256 || $3!=96 || $4!=(isI ? "I" : "P")) exit 1 }
        END { if (NR!=(name=="extreme" ? 6 : 180)) exit 1 }
    ' "$output_dir/$name.ffprobe.csv"
    printf '%s: strict FFmpeg whole planar cmp + measured frame/key/type PASS\n' "$name"
done

refusal=0
"$raw_test_dir/check" --emit "$output_dir" > "$raw_test_dir/existing-output.log" 2>&1 || refusal=$?
[ "$refusal" -eq 1 ]
printf 'Existing-output refusal: actual exit=%s (expected1)\n' "$refusal"
sha256sum "$output_dir/checker.h264" "$output_dir/checker.yuv420" \
    "$output_dir/checker.output-chain.yuy2" "$output_dir/mirror.h264" \
    "$output_dir/extreme.h264"
printf 'PASS: native/Werror, ASan/UBSan, C header, Pentium2/no-SSE, full strict decode and output refusal\n'
