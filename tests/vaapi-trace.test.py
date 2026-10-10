#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
import json
import sys


EXPECTED_KEYS = {
    "schema", "ts_monotonic_ns", "session", "driver_instance", "pid", "seq",
    "event", "context", "generation", "surface", "token",
    "decode_identity", "owner", "submission_ordinal", "operation",
    "duration_ns", "outcome",
}


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        assert key not in result, f"duplicate JSON key: {key}"
        result[key] = value
    return result


lines = [line for line in sys.stdin.read().splitlines() if line]
records = [json.loads(line, object_pairs_hook=strict_object) for line in lines]
assert len(records) == 14 + 4 * 16
assert [record["event"] for record in records[:14]] == [
    "submit_bind", "input_sent", "prefeed_sent", "output_dequeued",
    "materialized", "binding_end", "sync_enter", "sync_exit",
    "export_enter", "export_exit", "vpp_capture", "vpp_commit",
    "context_destroy", "surface_destroy",
]
for index, record in enumerate(records, 1):
    assert set(record) == EXPECTED_KEYS
    assert record["schema"] == "crystalhd-vaapi-trace-v1"
    assert record["seq"] == index
    assert record["session"] == record["driver_instance"] != 0
    assert all(isinstance(record[key], int) for key in EXPECTED_KEYS
               if key not in {"schema", "event"})
for earlier, later in zip(records, records[1:]):
    assert earlier["ts_monotonic_ns"] <= later["ts_monotonic_ns"]
for record in records[:14]:
    assert (record["context"], record["generation"], record["surface"],
            record["token"], record["decode_identity"], record["owner"],
            record["submission_ordinal"]) == (7, 3, 11, 400000, 19, 11, 4)
assert records[6]["operation"] == records[7]["operation"] == 10
assert records[7]["duration_ns"] == 250
assert records[8]["operation"] == records[9]["operation"] == 11
assert records[9]["duration_ns"] == 500
assert all(record["event"] == "output_dequeued" for record in records[14:])
assert all("0x" not in line and "/" not in line for line in lines)
print(f"VA-API trace: {len(records)} strict JSONL records verified")
