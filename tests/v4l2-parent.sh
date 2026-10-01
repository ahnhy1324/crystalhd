#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
parent_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-v4l2-parent.XXXXXX")
cleanup()
{
    rm -f "$parent_test_dir/check" "$parent_test_dir/disabled-check" \
        "$parent_test_dir/parent-binding.h" "$parent_test_dir/parent-functions.h" \
        "$parent_test_dir/parent-probe-tail.h" "$parent_test_dir/parent-owner-type.h" \
        "$parent_test_dir/parent-module-functions.h"
    rmdir "$parent_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

awk '
    /^struct crystalhd_session_owner_ops \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" > "$parent_test_dir/parent-owner-type.h"

awk '
    /^(struct crystalhd_v4l2(_ctx)?|enum crystalhd_v4l2_owner_state) \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_v4l2.c" > "$parent_test_dir/parent-binding.h"
awk '
    BEGIN {
        count = split("init cleanup ctx_release queue_close_locked owner_get owner_retired owner_put ctx_retired close_worker ctx_create ctx_acquire ctx_close resume_ready release parent_get parent_put ctx_bind register unregister", names, " ")
        for (i = 1; i <= count; i++) allowed["crystalhd_v4l2_" names[i]] = 1
    }
    /^static struct workqueue_struct \*/ { print }
    /^(static )?(void|int|bool) crystalhd_v4l2_[[:alnum:]_]+\(/ {
        name = $0
        sub(/\(.*/, "", name)
        sub(/^.* /, "", name)
        if (!(name in allowed) || seen[name]++) exit 1
        copying = 1; found++
    }
    /^static const struct crystalhd_session_owner_ops / { copying = 1; ops++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != count || ops != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_v4l2.c" > "$parent_test_dir/parent-functions.h"

awk '
    /^static (int __init chd_dec_module_init|void __exit chd_dec_module_cleanup)\(/ {
        copying = 1; found++
    }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 2 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$parent_test_dir/parent-module-functions.h"

# Execute the unchanged probe publication/cleanup tail, not a copy of its
# control flow. The earlier PCI setup is represented by fixture resource state.
awk '
    /^static int chd_dec_pci_probe\(/ { probe = 1; probes++ }
    probe && /^[[:space:]]*pci_set_master\(pdev\);[[:space:]]*$/ {
        starts++; copying = 1
        print "static int probe_publication_tail(struct pci_dev *pdev,"
        print "                                  struct crystalhd_adp *pinfo)"
        print "{"
        print "    int rc = 0;"
    }
    copying { print }
    probe && /^}/ {
        if (copying) ends++
        probe = 0; copying = 0
    }
    END { if (probes != 1 || starts != 1 || ends != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$parent_test_dir/parent-probe-tail.h"

for parent_sanitize in no yes; do
    parent_extra=
    if [ "$parent_sanitize" = yes ]; then
        parent_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and fixed sanitizer flags are intentionally split into arguments.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-label $parent_extra -I"$parent_test_dir" -DCRYSTALHD_ENABLE_V4L2 \
        "$repo_dir/tests/v4l2-parent.c" -o "$parent_test_dir/check"
    printf 'V4L2 parent: sanitizers=%s\n' "$parent_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$parent_test_dir/check"

    # Include the actual disabled header, without any media-core mock symbols.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-parameter $parent_extra -I"$repo_dir/driver/linux" \
        -UCRYSTALHD_ENABLE_V4L2 -DTEST_V4L2_DISABLED \
        "$repo_dir/tests/v4l2-parent.c" -o "$parent_test_dir/disabled-check"
    printf 'V4L2 parent disabled: sanitizers=%s\n' "$parent_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$parent_test_dir/disabled-check"
done
