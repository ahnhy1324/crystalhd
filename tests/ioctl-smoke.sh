#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cc=${CC:-cc}

for bits in 32 64; do
	# CPPFLAGS/CFLAGS are intentionally expanded as compiler arguments.
	# shellcheck disable=SC2086
	"$cc" ${CPPFLAGS:-} ${CFLAGS:-} -std=c11 -Wall -Wextra -Werror \
		-D__LINUX_USER__ -m"$bits" \
		-I"$repo_dir/include" -I"$repo_dir/include/link" \
		"$repo_dir/tests/ioctl-smoke.c" -o "$tmp_dir/ioctl-smoke-$bits"
done

if [ "${1:-}" = --build-only ]; then
	exit 0
fi

for bits in 32 64; do
	"$tmp_dir/ioctl-smoke-$bits" "$@"
done
