#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-pwm-cycle.XXXXXX")
cleanup()
{
    rm -f "$test_dir/check" "$test_dir/pwm-cycle-types.h" \
        "$test_dir/pwm-cycle-ledger.h" "$test_dir/pwm-cycle-observer.h" \
        "$test_dir/pwm-cycle-wrappers.h" "$test_dir/canonical-source.c" \
        "$test_dir/canonical-physical.c" "$test_dir/candidate-physical.c"
    rmdir "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
# Text inclusion check only: original production physical protocol is unchanged,
# apart from the two private helper names and insignificant blank lines.
git -C "$repo_dir" show 6d3a39e8117133842a83af0cb4b8e381416ca377:driver/linux/crystalhd_fleafuncs.c \
    > "$test_dir/canonical-source.c"
awk '
    /^\/\* Runtime power register helpers\./ { copying=1; found++ }
    /^bool crystalhd_flea_detect_fw_alive/ { if (copying) ended++; copying=0 }
    copying && $0 !~ /^[[:space:]]*$/ { print }
    END { if (found != 1 || ended != 1) exit 1 }
' "$test_dir/canonical-source.c" > "$test_dir/canonical-physical.c"
awk '
    /^\/\* Runtime power register helpers\./ { copying=1; found++ }
    /^\/\* Immutable experimental one-cycle wrapper/ { if (copying) ended++; copying=0 }
    copying && $0 !~ /^[[:space:]]*$/ {
        gsub(/crystalhd_flea_physical_power_/, "crystalhd_flea_runtime_power_")
        print
    }
    END { if (found != 1 || ended != 1) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/candidate-physical.c"
cmp "$test_dir/canonical-physical.c" "$test_dir/candidate-physical.c"
awk '
    /^enum FLEA_(POWER_STATES|POWER_DOWN_RESULT|STATE_CH_EVENT|POWER_REG) {/ { copying=1; found++ }
    copying { print }
    copying && /^};/ { copying=0 }
    END { if (found != 4 || copying) exit 1 }
' "$repo_dir/driver/linux/FleaDefs.h" > "$test_dir/pwm-cycle-types.h"
awk '
    /^bool crystalhd_pm_cycle_down_claim\(/ { copying=1 }
    /^bool crystalhd_pm_arbiter_claim\(/ { copying=0 }
    copying { print }
    copying && /^(bool|void) crystalhd_pm_cycle_/ { found++ }
    END { if (found != 6) exit 1 }
' "$repo_dir/driver/linux/crystalhd_pm_shadow.c" > "$test_dir/pwm-cycle-ledger.h"
awk '
    /^void crystalhd_pm_shadow_trace\(/ { copying=1; found++ }
    copying { print }
    copying && /^}/ { copying=0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_pm_shadow.c" > "$test_dir/pwm-cycle-observer.h"
awk '
    /^static BC_STATUS crystalhd_flea_runtime_power_up\(/ { copying=1; found++ }
    /^crystalhd_flea_runtime_power_dn\(/ { print previous; copying=1; found++ }
    /^BC_STATUS crystalhd_flea_set_power_state\(/ { copying=1; found++ }
    copying { print }
    copying && /^}/ { copying=0 }
    { previous=$0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_fleafuncs.c" > "$test_dir/pwm-cycle-wrappers.h"
cc=${CC:-cc}
"$cc" -std=c11 -Wall -Wextra -Werror -I"$repo_dir/driver/linux" \
    -I"$test_dir" "$repo_dir/tests/pwm-cycle.c" -o "$test_dir/check"
"$test_dir/check"
"$cc" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -g -I"$repo_dir/driver/linux" -I"$test_dir" \
    "$repo_dir/tests/pwm-cycle.c" -o "$test_dir/check"
"$test_dir/check"
