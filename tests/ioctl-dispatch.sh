#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ioctl_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-ioctl-dispatch.XXXXXX")
cleanup()
{
    rm -f "$ioctl_test_dir/check" "$ioctl_test_dir/ioctl-types.h" \
        "$ioctl_test_dir/ioctl-compat.h" "$ioctl_test_dir/ioctl-table.h" \
        "$ioctl_test_dir/ioctl-functions.h"
    rmdir "$ioctl_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

awk '
    /^enum _crystalhd_state/ || /^struct crystalhd_user \{/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    /^#define[[:space:]]+DTS_MODE_INV[[:space:]]/ { print }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" > "$ioctl_test_dir/ioctl-types.h"
awk '
    /^struct crystalhd_ppb_mpeg32 \{/ { copying = 1; found++ }
    /^#endif/ { copying = 0 }
    # The userspace firmware header names the same ABI layout with a typedef.
    copying { sub(/struct PPB,/, "PPB,"); print }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_compat_ioctl.h" > "$ioctl_test_dir/ioctl-compat.h"
awk '
    /^static const struct crystalhd_cmd_tbl[[:space:]]+g_crystalhd_cproc_tbl/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$ioctl_test_dir/ioctl-table.h"
awk '
    /^static BC_STATUS bc_cproc_session_owner_required\(/ ||
    /^static struct crystalhd_user \*bc_cproc_get_uid\(/ ||
    /^(static bool|void|BC_STATUS) crystalhd_native_legacy_[[:alnum:]_]+\(/ ||
    /^static (bool|void) crystalhd_legacy_(enter|exit)\(/ ||
    /^crystalhd_cmd_proc crystalhd_get_cmd_proc\(/ ||
    /^static bool crystalhd_(legacy_color_command|rawio_command)\(/ ||
    /^static int chd_dec_api_cmd\(/ ||
    /^static int chd_dec_(open|close)_locked\(/ ||
    /^static long chd_dec_(ioctl_common|ioctl|compat_ioctl)\(/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 18 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" \
    "$repo_dir/driver/linux/crystalhd_lnx.c" > "$ioctl_test_dir/ioctl-functions.h"

for ioctl_bits in 32 64; do
    for ioctl_sanitize in no yes; do
        ioctl_extra=
        if [ "$ioctl_sanitize" = yes ]; then
            ioctl_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
        fi
        # Existing command-table sentinel omits its trailing zero field.
        "${CC:-cc}" ${CFLAGS:-} -m"$ioctl_bits" -std=c11 -O1 -g \
            -Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter \
            -D__LINUX_USER__ $ioctl_extra -I"$repo_dir/include" -I"$repo_dir/include/link" \
            -I"$ioctl_test_dir" "$repo_dir/tests/ioctl-dispatch.c" \
            -o "$ioctl_test_dir/check"
        printf 'Legacy ioctl dispatch: bits=%s, sanitizers=%s\n' \
            "$ioctl_bits" "$ioctl_sanitize"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$ioctl_test_dir/check"
    done
done
