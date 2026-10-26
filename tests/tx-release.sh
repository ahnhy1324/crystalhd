#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-tx-release.XXXXXX")
trap 'rm -f "$work/check" "$work/tx-release-types.h" "$work/tx-release-functions.h"; rmdir "$work"' EXIT
trap 'exit 1' HUP INT TERM
awk '
 /^enum _crystalhd_state[[:space:]]*\{/ ||
 /^struct crystalhd_(file|tx_completion) \{/ { copying=1; found++ }
 copying { print }
 copying && /^};/ { copying=0 }
 END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" "$repo_dir/driver/linux/crystalhd_cmds.c" \
 "$repo_dir/driver/linux/crystalhd_cmds.h" > "$work/tx-release-types.h"
awk '
 /^static void bc_proc_in_completion\(/ ||
 /^static BC_STATUS (bc_cproc_codein_sleep|crystalhd_bounded_tx_status|crystalhd_tx_transfer_common)\(/ ||
 /^static (bool|void) crystalhd_legacy_(enter|exit)\(/ ||
 /^static long chd_dec_ioctl_common\(/ { copying=1; found++ }
 copying { print }
 copying && /^}/ { copying=0 }
 END { if (found != 7 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" "$repo_dir/driver/linux/crystalhd_lnx.c" > "$work/tx-release-functions.h"
for sanitize in no yes; do
    extra=
    if [ "$sanitize" = yes ]; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and the fixed sanitizer list intentionally split into arguments.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror -pthread \
        -D__LINUX_USER__ $extra -I"$repo_dir/include" -I"$repo_dir/include/link" \
        -I"$work" "$repo_dir/tests/tx-release.c" -o "$work/check"
    printf 'TX release real-lock concurrency: sanitizers=%s\n' "$sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        timeout 30 "$work/check"
done
