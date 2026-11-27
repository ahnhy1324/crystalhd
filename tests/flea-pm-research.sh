#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
pm_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-pm-research.XXXXXX")
cleanup()
{
    rm -f "$pm_test_dir/pm-module-lifecycle.h" "$pm_test_dir/lifecycle"
    rmdir "$pm_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
awk '
    /^static (int __init chd_dec_module_init|void __exit chd_dec_module_cleanup)\(/ { copying=1; found++ }
    copying { print }
    copying && /^}/ { copying=0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$pm_test_dir/pm-module-lifecycle.h"
for mode in normal sanitized; do
    extra=
    if [ "$mode" = sanitized ]; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror $extra \
        -DCRYSTALHD_ENABLE_PM_RESEARCH -I"$pm_test_dir" \
        "$repo_dir/tests/flea-pm-lifecycle.c" -o "$pm_test_dir/lifecycle"
    "$pm_test_dir/lifecycle"
done
FLEA_PM_RESEARCH_CHECK=1 sh "$repo_dir/tests/flea-power.sh"

# Exercise the separately bounded endurance profile in the existing PM gate.
CFLAGS="${CFLAGS:-} -DCRYSTALHD_ENABLE_PM_SUSTAINED_RESEARCH" \
    FLEA_PM_RESEARCH_CHECK=1 sh "$repo_dir/tests/flea-power.sh"
