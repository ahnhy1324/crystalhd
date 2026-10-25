#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-rx-pages.XXXXXX")
trap 'rm -f "$work_dir/callbacks.h" "$work_dir/check"; rmdir "$work_dir"' EXIT
trap 'exit 1' HUP INT TERM
# Two legacy callbacks also permit running this regression against the old code.
awk '
 /^static BC_STATUS crystalhd_dio_rx_(copy|read|write)\(/ { copying=1; found++ }
 copying { print }
 copying && /^}/ { copying=0 }
 END { if (copying || (found != 2 && found != 3)) exit 1 }
' "$repo_dir/driver/linux/crystalhd_misc.c" > "$work_dir/callbacks.h"
for sanitize in no yes; do
 extra=
 if [ "$sanitize" = yes ]; then
  extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
 fi
 "${CC:-cc}" ${CFLAGS:-} -O1 -g -std=c11 -Wall -Wextra -Werror $extra \
  -I"$repo_dir/include" -I"$work_dir" "$repo_dir/tests/rx-pages.c" -o "$work_dir/check"
 printf 'RX pinned-page access: sanitizers=%s\n' "$sanitize"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$work_dir/check"
done
