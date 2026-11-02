#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# CPU only: production collector/wrapper, controlled callee; never link DIL.
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-finish-trace.XXXXXX")
trap 'rm -f "$test_dir/check" "$test_dir/ordinary.log" "$test_dir/sanitized.log" "$test_dir/ordinary.stderr" "$test_dir/sanitized.stderr"; rmdir "$test_dir"' EXIT
trap 'exit 1' HUP INT TERM
packages=gstreamer-video-1.0
for variant in ordinary sanitized; do
    extra=
    if test "$variant" = sanitized; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    ${CC:-cc} ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror $extra \
        $(pkg-config --cflags $packages) "$repo_dir/tests/gst-finish-trace.c" \
        -o "$test_dir/check" $(pkg-config --libs $packages)
    status=0
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        timeout 20 "$test_dir/check" >"$test_dir/$variant.log" \
        2>"$test_dir/$variant.stderr" || status=$?
    if test "$status" -ne 0; then
        cat "$test_dir/$variant.log" "$test_dir/$variant.stderr"
        exit "$status"
    fi
    python3 - "$variant" "$test_dir/$variant.log" "$test_dir/$variant.stderr" <<'PY'
import json
from pathlib import Path
import sys
variant, log, errors = sys.argv[1:]
assert not Path(errors).read_text().strip(), Path(errors).read_text()
cases = {}
for line in Path(log).read_text().splitlines():
    if line.startswith('# '):
        line = line[2:]
    if line.startswith('FINISH_CASE '):
        _, name, encoded = line.split(' ', 2)
        assert name not in cases
        cases[name] = json.loads(encoded)
assert set(cases) == {'off', 'metadata', 'capacity', 'unknown', 'inflight', 'returned', 'omitted-blocked', 'saturation'}
assert cases['off'] == {'schema': 1, 'enabled': False, 'records': []}
for name, value in cases.items():
    if name == 'off':
        continue
    assert value['schema'] == 1 and value['enabled'] is True
    assert value['retention'] == 'first' and value['clock'] == 'CLOCK_MONOTONIC'
    assert value['inflight_scope'] == 'retained_records_only'
    assert value['retained'] == len(value['records']) <= value['capacity'] <= 8192
    assert value['observed'] - value['retained'] == value['omitted']
    assert value['inflight'] == sum(not r['complete'] for r in value['records'])
    assert [r['call_id'] for r in value['records']] == list(range(1, value['retained'] + 1))
a, b, c = cases['metadata']['records']
assert (a['pts'], a['duration'], a['generation'], a['frame_number'], a['recovery_epoch']) == (0, 40000000, 2, 7, 8)
assert (b['pts'], b['generation'], b['frame_number']) == (0, 3, 7)
assert c['pts'] is None and c['duration'] is None
assert [a['flow'], b['flow'], c['flow']] == [0, -2, -5]
for r in (a, b, c):
    assert r['instance'] == 1234 and r['enter_tid'] == r['return_tid'] == 17
    assert r['complete'] and r['monotonic_order_valid'] and r['elapsed_ns'] == 1000
cap = cases['capacity']
assert (cap['observed'], cap['retained'], cap['omitted']) == (3, 1, 2)
u = cases['unknown']['records'][0]
assert u['complete'] and u['flow'] == 0 and u['enter_ns'] is None
assert u['enter_tid'] is None and u['return_tid'] is None
assert u['elapsed_ns'] is None and not u['monotonic_order_valid']
i = cases['inflight']['records'][0]
assert not i['complete'] and i['flow'] is None and i['return_ns'] is None
assert i['return_tid'] is None and i['elapsed_ns'] is None
r = cases['returned']['records'][0]
assert r['complete'] and r['enter_tid'] == r['return_tid'] == 29 and r['flow'] == 0
o = cases['omitted-blocked']
assert (o['observed'], o['retained'], o['omitted'], o['inflight']) == (2, 1, 1, 0)
assert cases['saturation']['counters_saturated'] and cases['saturation']['observed'] == 2**64 - 1
assert cases['saturation']['retained'] == 0
print(f'{variant}: 6 CPU groups / 8 snapshot contracts passed')
PY
done
