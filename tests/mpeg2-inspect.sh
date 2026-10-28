#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-mpeg2-inspect.XXXXXX")
cleanup()
{
    rm -f "$test_dir/check" "$test_dir/check-cxx"
    rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

for sanitize in no yes; do
    extra=
    if [ "$sanitize" = yes ]; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # Compiler and caller flags intentionally expand into arguments.
    # shellcheck disable=SC2086
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $extra "$repo_dir/tests/mpeg2-inspect.c" -o "$test_dir/check"
    printf 'MPEG-2 inspect: sanitizers=%s\n' "$sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$test_dir/check"
done

"${CXX:-c++}" ${CXXFLAGS:-} -std=c++11 -O1 -g -Wall -Wextra -Werror \
    "$repo_dir/tests/mpeg2-inspect.c" -o "$test_dir/check-cxx"
"$test_dir/check-cxx"
