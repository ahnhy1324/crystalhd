#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-pwm-retention.XXXXXX")
cleanup()
{
    rm -f "$test_dir/check" "$test_dir/pwm-retention-types.h" \
        "$test_dir/pwm-retention-claim.h" "$test_dir/pwm-retention-wrapper.h"
    rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
awk '
    /^enum FLEA_(POWER_STATES|POWER_DOWN_RESULT|STATE_CH_EVENT) {/ { copying=1; found++ }
    copying { print }
    copying && /^};/ { copying=0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/FleaDefs.h" > "$test_dir/pwm-retention-types.h"
awk '
    /^bool crystalhd_pm_arbiter_claim\(/ { copying=1; found++ }
    copying { print }
    copying && /^}/ { copying=0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_pm_shadow.c" > "$test_dir/pwm-retention-claim.h"
awk '
    /^crystalhd_flea_runtime_power_dn\(/ { print previous; copying=1; found++ }
    copying { print }
    copying && /^}/ { copying=0 }
    { previous=$0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/pwm-retention-wrapper.h"
if rg -n 'CLOCK|PLL|PHY_|PAD|WATCHDOG|FLEA_PD_ENTERED|= FLEA_PS_LP_COMPLETE' \
    "$repo_dir/driver/linux/crystalhd_pm_retention_ops.h" "$test_dir/pwm-retention-wrapper.h"; then
    echo 'unexpected full runtime-gating surface' >&2; exit 1
fi
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -I"$repo_dir/driver/linux" \
    -I"$repo_dir/include/flea" -I"$test_dir" "$repo_dir/tests/pwm-retention.c" -o "$test_dir/check"
"$test_dir/check"
"$cc" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -g -I"$repo_dir/driver/linux" -I"$repo_dir/include/flea" \
    -I"$test_dir" "$repo_dir/tests/pwm-retention.c" -o "$test_dir/check"
"$test_dir/check"
