#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

cc=${CC:-cc}
common_flags="-std=c11 -Wall -Wextra -Werror -D__LINUX_USER__"

for bits in 32 64; do
	# CPPFLAGS/CFLAGS are intentionally expanded as compiler arguments.
	# shellcheck disable=SC2086
	"$cc" ${CPPFLAGS:-} ${CFLAGS:-} $common_flags -m"$bits" \
		-I"$repo_dir/include" -I"$repo_dir/include/link" \
		-c "$repo_dir/tests/uapi-abi.c" \
		-o "$tmp_dir/uapi-abi-$bits.o"
done

# shellcheck disable=SC2086
"$cc" ${CPPFLAGS:-} ${CFLAGS:-} $common_flags \
	-I"$repo_dir/include" "$repo_dir/tests/ioctl-limits.c" \
	-o "$tmp_dir/ioctl-limits"
"$tmp_dir/ioctl-limits"
