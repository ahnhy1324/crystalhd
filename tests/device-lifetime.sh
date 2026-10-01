#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lifetime_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-lifetime-check.XXXXXX")
cleanup()
{
    rm -f "$lifetime_test_dir/check" "$lifetime_test_dir/lifetime-functions.h" \
        "$lifetime_test_dir/lifetime-command.h" "$lifetime_test_dir/lifetime-binding.h" \
        "$lifetime_test_dir/lifetime-owner.h" \
        "$lifetime_test_dir/access-check" "$lifetime_test_dir/access-binding.h" \
        "$lifetime_test_dir/access-functions.h" "$lifetime_test_dir/probe-admission.h"
    rmdir "$lifetime_test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# Exercise the production removal/close paths, including their resource helpers.
awk '
    /^struct crystalhd_file \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$lifetime_test_dir/lifetime-binding.h"
awk '
    /^struct crystalhd_session_owner_ops \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.h" > "$lifetime_test_dir/lifetime-owner.h"
awk '
    /^static void crystalhd_decoder_tracking_reset\(/ ||
    /^static bool crystalhd_retire_hw_context\(/ ||
    /^static void crystalhd_session_retire_owner\(/ ||
    /^static void crystalhd_session_unpin\(/ ||
    /^BC_STATUS crystalhd_session_release_locked\(/ ||
    /^void crystalhd_user_close\(/ ||
    /^BC_STATUS crystalhd_delete_cmd_context\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 7 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_cmds.c" > "$lifetime_test_dir/lifetime-command.h"
awk '
    /^static int chd_dec_disable_int\(/ ||
    (/^crystalhd_ioctl_data \*chd_dec_alloc_iodata\(/ && !/;[[:space:]]*$/) ||
    /^static int chd_dec_close(_locked)?\(/ ||
    /^static void chd_dec_release_chdev\(/ ||
    /^static void chd_pci_release_mem\(/ ||
    /^static void chd_release_l0s\(/ ||
    /^static bool chd_dec_(clear_master_and_drain|quiesce_terminal_dma|session_dma_absent|fail_closed)\(/ ||
    /^static void chd_dec_quarantine_dma\(/ ||
    /^static void chd_dec_pci_remove\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 13 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$lifetime_test_dir/lifetime-functions.h"

awk '
    /^struct crystalhd_device_access \{/ { copying = 1; found++ }
    copying { print }
    copying && /^};/ { copying = 0 }
    END { if (found != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.h" > "$lifetime_test_dir/access-binding.h"
awk '
    /^static int crystalhd_device_reserve_generation\(/ ||
    /^int crystalhd_device_enter\(/ ||
    /^void crystalhd_device_exit\(/ { copying = 1; found++ }
    copying { print }
    copying && /^}/ { copying = 0 }
    END { if (found != 3 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$lifetime_test_dir/access-functions.h"

# Execute the actual probe admission prefix, stopping before its first
# allocation/PCI operation. The fixture supplies only locals and the out label.
awk '
    /^static int chd_dec_pci_probe\(/ { probe = 1 }
    probe && /down_write\(&chd_device_lock\)/ { copying = 1; starts++ }
    copying && /pinfo = kzalloc\(/ { copying = 0; probe = 0; ends++; next }
    copying { print }
    END { if (starts != 1 || ends != 1 || copying) exit 1 }
' "$repo_dir/driver/linux/crystalhd_lnx.c" > "$lifetime_test_dir/probe-admission.h"

# The helper tests must stay connected to the real probe. Check its narrow
# ordering contract without reproducing the PCI/resource setup implementation.
awk '
    /^static int crystalhd_device_reserve_generation\(/ { reserve = 1 }
    /\+\+[[:space:]]*chd_device_generation|chd_device_generation[[:space:]]*\+\+/ {
        increments++
        if (!reserve) bad = 1
    }
    /^static int chd_dec_pci_probe\(/ { probe = 1; probes++ }
    probe {
        if (/down_write\(&chd_device_lock\)/) locked = NR
        if (/if \(g_adp_info\)/) busy = NR
        if (/if \(chd_dma_quarantine\)/) quarantine = NR
        if (/crystalhd_device_reserve_generation\(&generation\)/) {
            reserved = NR; reservations++
        }
        if (reserved && !allocated && /if \(rc\)/) checked = NR
        if (checked && !allocated && /goto out;/) rejected = NR
        if (/pinfo = kzalloc\(/) { allocated = NR; allocations++ }
        if (/pinfo->generation = generation;/) assigned = NR
        if (/g_adp_info = pinfo;/) published = NR
        if (/crystalhd_setup_cmd_context\(/) context = NR
        if (/crystalhd_l0s_init\(/) l0s = NR
        if (/pci_set_master\(pdev\)/) master = NR
        if (/pci_set_drvdata\(pdev, pinfo\)/) drvdata = NR
        if (/pinfo->hw_accessible = true;/) { ready = NR; readiness++ }
        if (/up_write\(&chd_device_lock\)/) unlocked = NR
    }
    /^}/ { reserve = 0; probe = 0 }
    END {
        if (bad || increments != 1 || probes != 1 || reservations != 1 ||
            allocations != 1 || readiness != 1 || !locked ||
            !(locked < busy && busy < quarantine && quarantine < reserved && reserved < checked &&
              checked < rejected && rejected < allocated && allocated < assigned &&
              assigned < published && published < context && context < l0s &&
              l0s < master && master < drvdata && drvdata < ready && ready < unlocked)) {
            print "Device access: probe/generation wiring changed" > "/dev/stderr"
            exit 1
        }
    }
' "$repo_dir/driver/linux/crystalhd_lnx.c"

for lifetime_sanitize in no yes; do
    lifetime_extra=
    if [ "$lifetime_sanitize" = yes ]; then
        lifetime_extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    # CFLAGS and fixed sanitizer flags are intentionally split into arguments.
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-parameter $lifetime_extra -I"$lifetime_test_dir" \
        "$repo_dir/tests/device-lifetime.c" -o "$lifetime_test_dir/check"
    printf 'Device lifetime: sanitizers=%s\n' "$lifetime_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$lifetime_test_dir/check"
    "${CC:-cc}" ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror \
        $lifetime_extra -I"$lifetime_test_dir" \
        "$repo_dir/tests/device-access.c" -pthread -o "$lifetime_test_dir/access-check"
    printf 'Device access: sanitizers=%s\n' "$lifetime_sanitize"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$lifetime_test_dir/access-check"
done
