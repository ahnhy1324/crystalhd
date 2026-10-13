#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-pwm-shadow.XXXXXX")
cleanup()
{
    rm -f "$test_dir/check" "$test_dir/pwm-shadow-types.h" \
        "$test_dir/pwm-shadow-functions.h" "$test_dir/pwm-shadow-stubs.h"
    rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

awk '
    /^enum FLEA_(POWER_STATES|POWER_DOWN_RESULT|STATE_CH_EVENT) {/ { copying=1; found++ }
    copying { print }
    copying && /^};/ { copying=0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/FleaDefs.h" > "$test_dir/pwm-shadow-types.h"
awk '
    /^(void crystalhd_pm_shadow_(session|trace|retire)|static int shadow_(show|open|release)|static void shadow_print_record)\(/ {
        copying=1; found++
    }
    copying { print }
    copying && /^}/ { copying=0 }
    END { if (found != 7 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_pm_shadow.c" > "$test_dir/pwm-shadow-functions.h"
awk '
    /^static BC_STATUS crystalhd_flea_runtime_power_up\(/ { copying=1; found++ }
    /^crystalhd_flea_runtime_power_dn\(/ { print previous; copying=1; found++ }
    copying { print }
    copying && /^}/ { copying=0 }
    { previous=$0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/pwm-shadow-stubs.h"

# Source fuses: no runtime control path, debugfs mutable operation or hw pointer.
awk '
    /^#if 0 \/\* Permanently excluded in this isolated host-PM shadow artifact\./ { off=1; found++ }
    /^#if/ && off { depth++ }
    off && /^static.*flea_pm/ { helpers++ }
    /^#endif/ && off { depth--; if (!depth) { off=0; ended++ } }
    END { if (found != 1 || ended != 1 || helpers < 6 || off) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c"
if rg -n 'pfn(Read|Write)|readl|writel|WatchDog|flea_pm_' "$test_dir/pwm-shadow-stubs.h"; then
    echo 'unexpected physical runtime access in shadow stubs' >&2
    exit 1
fi
rg -q 'debugfs_create_file\("trace", 0400' "$repo_dir/driver/linux/crystalhd_pm_shadow.c"
rg -q '\.owner = THIS_MODULE' "$repo_dir/driver/linux/crystalhd_pm_shadow.c"
if rg -n '\.(write|unlocked_ioctl|compat_ioctl|mmap)\s*=' \
    "$repo_dir/driver/linux/crystalhd_pm_shadow.c"; then
    echo 'unexpected mutable trace interface' >&2
    exit 1
fi
if rg -n 'crystalhd_hw|hw->|\*hw' "$repo_dir/driver/linux/crystalhd_pm_shadow_ring.h"; then
    echo 'reader buffer must not retain hardware pointers' >&2
    exit 1
fi
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -I"$repo_dir/driver/linux" \
    -I"$test_dir" "$repo_dir/tests/pwm-shadow.c" -o "$test_dir/check"
"$test_dir/check"
"$cc" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -g -I"$repo_dir/driver/linux" -I"$test_dir" \
    "$repo_dir/tests/pwm-shadow.c" -o "$test_dir/check"
"$test_dir/check"
