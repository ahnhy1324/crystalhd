#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 STARTED_AT" >&2
    exit 2
fi
started_at=$1

journal_unavailable()
{
    echo "kernel-log validation FAILED: $1" >&2
    echo "Use a system with a readable kernel journal and an account authorized to read it (commonly systemd-journal or adm)." >&2
    echo "Decode results alone do not establish a kernel-error-free hardware pass." >&2
    exit 1
}

command -v journalctl >/dev/null 2>&1 ||
    journal_unavailable "journalctl is unavailable"

# journalctl can exit successfully with no accessible entries, including when
# the caller cannot read system logs. An empty test-time interval is normal;
# an empty unfiltered kernel journal cannot substantiate this validation.
if ! kernel_record=$(journalctl --quiet -k -n 1 --no-pager); then
    journal_unavailable "could not read the kernel journal"
fi
[ -n "$kernel_record" ] ||
    journal_unavailable "no accessible kernel records; check journal permissions and availability"

if ! kernel_log=$(journalctl --quiet -k --since "$started_at" --no-pager); then
    journal_unavailable "could not read the test-time kernel journal"
fi
if kernel_findings=$(printf '%s\n' "$kernel_log" | \
    grep -Ei 'BUG:|Oops:|general protection|Call Trace|hung task|crystalhd.*(error|fail|timeout|invalid arguments)'); then
    echo "$kernel_findings" >&2
    echo "kernel errors occurred during the hardware test" >&2
    exit 1
else
    scan_status=$?
    [ "$scan_status" -eq 1 ] || journal_unavailable "kernel-log scan failed"
fi

echo "kernel-log validation passed: no matching errors during the test"
