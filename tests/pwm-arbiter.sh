#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-pwm-arbiter.XXXXXX")
cleanup()
{
    rm -f "$test_dir/check" "$test_dir/pwm-arbiter-types.h" \
        "$test_dir/pwm-arbiter-claim.h" "$test_dir/pwm-arbiter-functions.h"
    rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
awk '
    /^enum FLEA_(POWER_STATES|POWER_DOWN_RESULT|STATE_CH_EVENT) {/ { copying=1; found++ }
    copying { print }
    copying && /^};/ { copying=0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/FleaDefs.h" > "$test_dir/pwm-arbiter-types.h"
awk '
    /^bool crystalhd_pm_arbiter_claim\(/ { copying=1; found++ }
    copying { print }
    copying && /^}/ { copying=0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_pm_shadow.c" > "$test_dir/pwm-arbiter-claim.h"
awk '
    /^crystalhd_flea_(arbiter_readiness|runtime_power_dn)\(/ {
        if (previous != "static enum FLEA_POWER_DOWN_RESULT") exit 1
        print previous; copying=1; found++
    }
    copying { print }
    copying && /^}/ { copying=0 }
    { previous=$0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/pwm-arbiter-functions.h"
# Only MasterCTL writes and CTL_STATUS polling belong to this experiment.
if rg -n 'CLOCK|PLL|PHY_|PARAMS|REFRESH|WATCHDOG|FLEA_PD_ENTERED|= FLEA_PS_LP_COMPLETE' \
    "$test_dir/pwm-arbiter-functions.h"; then
    echo 'unexpected physical runtime-gating surface' >&2; exit 1
fi
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -I"$repo_dir/driver/linux" \
    -I"$repo_dir/include/flea" -I"$test_dir" "$repo_dir/tests/pwm-arbiter.c" -o "$test_dir/check"
"$test_dir/check"
"$cc" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -g -I"$repo_dir/driver/linux" -I"$repo_dir/include/flea" \
    -I"$test_dir" "$repo_dir/tests/pwm-arbiter.c" -o "$test_dir/check"
"$test_dir/check"
