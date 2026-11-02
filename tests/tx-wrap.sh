#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wrap_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-tx-wrap.XXXXXX")
cleanup() { rm -f "$wrap_test_dir/check" "$wrap_test_dir/production.h"; rmdir "$wrap_test_dir"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
awk '
 /;$/ && !copying { next }
 /^BC_STATUS crystalhd_flea_request_tx_wrap\(/ ||
 /^bool crystalhd_flea_check_input_full\(/ ||
 /^void crystalhd_flea_update_tx_buff_info\(/ {
  copying = 1; found++
  # Unchanged legacy function spells offsetof through a null pointer.
  # Keep its real body while suppressing only that legacy null diagnostic.
  if ($0 ~ /^bool crystalhd_flea_check_input_full\(/)
   print "__attribute__((no_sanitize(\"null\")))"
 }
 copying { print }
 copying && /^}/ { copying = 0 }
 END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$wrap_test_dir/production.h"
for wrap_sanitize in native address,undefined; do
 wrap_flags=
 if [ "$wrap_sanitize" != native ]; then
  wrap_flags="-fsanitize=$wrap_sanitize -fno-omit-frame-pointer -fno-pie -no-pie"
 fi
 ${CC:-cc} ${CPPFLAGS:-} ${CFLAGS:--O2 -g} $wrap_flags -std=c11 \
  -Wall -Wextra -Werror -Wno-unused-parameter -I"$wrap_test_dir" \
  -I"$repo_dir/include" "$repo_dir/tests/tx-wrap.c" ${LDFLAGS:-} $wrap_flags \
  -o "$wrap_test_dir/check"
 printf 'Typed TX wrap: sanitizers=%s\n' "$wrap_sanitize"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$wrap_test_dir/check"
done
