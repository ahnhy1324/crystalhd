#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

scan_findings()
{
    grep -Ei 'BUG:|Oops:|general protection|Call Trace|hung task|WARNING:|cut here|kernel panic|soft lockup|hard LOCKUP|PCIe Bus Error|AER:.*(error|fatal|uncorrected|corrected)|crystalhd.*(error|fail|timeout|timed out|T/O|invalid[ _-]*(arg|argument))'
}

self_test()
{
    if printf '%s\n' \
        'crystalhd 0000:02:00.0: device initialized' \
        'AER capability is not exposed on this platform' | scan_findings >/dev/null; then
        echo "kernel-log checker self-test accepted a benign line" >&2
        return 1
    fi
    for finding in \
        'WARNING: CPU: 0 PID: 1 at drivers/example.c:10 example+0x1/0x2' \
        'pcieport 0000:00:1c.0: PCIe Bus Error: severity=Corrected, type=Physical Layer' \
        'crystalhd 0000:02:00.0: firmware command timeout' \
        'crystalhd 0000:02:00.0: Firmware command T/O' \
        'crystalhd 0000:02:00.0: Invalid Arg for decoder command'; do
        if ! printf '%s\n' "$finding" | scan_findings >/dev/null; then
            echo "kernel-log checker self-test missed: $finding" >&2
            return 1
        fi
    done
    echo "kernel-log checker self-test passed"
}

if [ "$#" -eq 1 ] && [ "$1" = --self-test ]; then
    self_test
    exit
fi
if [ "$#" -ne 1 ]; then
    echo "usage: $0 STARTED_AT | --self-test" >&2
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
if kernel_findings=$(printf '%s\n' "$kernel_log" | scan_findings); then
    echo "$kernel_findings" >&2
    echo "kernel errors occurred during the hardware test" >&2
    exit 1
else
    scan_status=$?
    [ "$scan_status" -eq 1 ] || journal_unavailable "kernel-log scan failed"
fi

echo "kernel-log validation passed: no matching errors during the test"
