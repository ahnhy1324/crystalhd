#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cxx=${CXX:-c++}
qemu=${QEMU_I386:-qemu-i386}
pkg_config=${PKG_CONFIG:-pkg-config}
for command in "$qemu" "$pkg_config" objdump; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "VA-API CPU check requires $command" >&2
        exit 2
    }
done

build_object() {
    mode=$1
    legacy=$2
    flags=$(make -s --no-print-directory -C "$repo_dir/filters/vaapi" \
        LEGACY_CPU="$legacy" CXX="$cxx -m32" print-cpu-flags)
    # Compiler, pkg-config and selected CPU flags intentionally expand into
    # arguments. This is the same policy consumed by the VA driver recipe.
    # shellcheck disable=SC2086
    $cxx -m32 -std=c++17 -O2 -Wall -Wextra -Werror \
        -fno-tree-slp-vectorize -ffunction-sections -fdata-sections \
        -D__LINUX_USER__ -I"$repo_dir/include" \
        -I"$repo_dir/linux_lib/libcrystalhd" \
        $($pkg_config --cflags libva libva-drm libdrm gbm libswscale) \
        $flags -c "$repo_dir/tests/vaapi-cache.cpp" \
        -o "$tmp_dir/$mode.o"
    objdump -d "$tmp_dir/$mode.o" > "$tmp_dir/$mode.disassembly"
    copy_section=$(objdump -h "$tmp_dir/$mode.o" |
        awk '$2 ~ /CopyYuy2ToSurface/ { print $2; exit }')
    test -n "$copy_section"
    objdump -d -j "$copy_section" "$tmp_dir/$mode.o" \
        > "$tmp_dir/$mode-copy.disassembly"
    # Linking only retained test sections needs no 32-bit graphics libraries.
    # shellcheck disable=SC2086
    $cxx -m32 "$tmp_dir/$mode.o" \
        -Wl,--gc-sections,--wrap=open,--wrap=ioctl,--wrap=close -pthread \
        -o "$tmp_dir/$mode"
}

build_object normal 0
for instruction in movdqu packuswb pavgb; do
    grep -Eq "[[:space:]]$instruction[[:space:]]" \
        "$tmp_dir/normal-copy.disassembly"
done
"$tmp_dir/normal"

build_object legacy 1
if grep -Eq '%([xyz]mm[0-9]+|mm[0-7])|[[:space:]](emms|femms|clflush|[lms]fence|[ls]dmxcsr)([[:space:]]|$)' \
    "$tmp_dir/legacy.disassembly"; then
    echo "legacy VA-API object contains SIMD/cache instructions" >&2
    exit 1
fi
if grep -Eq '[[:space:]](movdqu|packuswb|pavgb)[[:space:]]' \
    "$tmp_dir/legacy-copy.disassembly"; then
    echo "legacy VA-API conversion retained its SSE2 path" >&2
    exit 1
fi
"$qemu" -cpu pentium2 "$tmp_dir/legacy"
printf '%s\n' \
    'VA-API i386 CPU policy: normal SSE2 conversion and Pentium2 legacy paths PASS'
