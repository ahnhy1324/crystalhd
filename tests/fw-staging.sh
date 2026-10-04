#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Apply the issue92 loader experiment only to temporary source copies.
# This check mocks all hardware; it never builds/loads a module or opens a card.
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-fw-staging.XXXXXX")
cleanup()
{
	rm -f "$test_dir/driver/linux/crystalhd_hw.h" \
		"$test_dir/driver/linux/crystalhd_fw_research.c" \
		"$test_dir/driver/linux/crystalhd_fleafuncs.c" \
		"$test_dir/driver/linux/crystalhd_linkfuncs.c" \
		"$test_dir/driver/linux/crystalhd_lnx.c" \
		"$test_dir/driver/linux/FleaDefs.h" \
		"$test_dir/tests/fw-download.c" "$test_dir/tests/fw-download.sh" \
		"$test_dir/include" "$test_dir/firmware"
	rmdir "$test_dir/driver/linux" "$test_dir/driver" \
		"$test_dir/tests" "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

mkdir -p "$test_dir/driver/linux" "$test_dir/tests"
for name in FleaDefs.h crystalhd_hw.h crystalhd_fw_research.c crystalhd_fleafuncs.c \
	crystalhd_linkfuncs.c crystalhd_lnx.c; do
	cp "$repo_dir/driver/linux/$name" "$test_dir/driver/linux/$name"
done
cp "$repo_dir/tests/fw-download.c" "$test_dir/tests/fw-download.c"
cp "$repo_dir/tests/fw-download.sh" "$test_dir/tests/fw-download.sh"
ln -s "$repo_dir/include" "$test_dir/include"
ln -s "$repo_dir/firmware" "$test_dir/firmware"

git -C "$test_dir" apply --check \
	"$repo_dir/tests/fixtures/issue92/fw-staging.patch"
git -C "$test_dir" apply \
	"$repo_dir/tests/fixtures/issue92/fw-staging.patch"
sh "$test_dir/tests/fw-download.sh"
