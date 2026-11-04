#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline OWN replay classification using exact native receipts, never PTS.

Historical admission is immutable provenance, not a restored live owner. A
balanced diagnostic subset does not satisfy the separate original-picture
population requirement. No untagged old capture is accepted by this reader.
"""
import argparse
from collections import Counter, defaultdict
import importlib.util
import json
from pathlib import Path


_spec = importlib.util.spec_from_file_location(
    "_replay_safe", Path(__file__).with_name("vaapi-postfilter-reader.py"))
P = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(P)
require, integer, signed = P.require, P.integer, P.signed
SCHEMA = "crystalhd-replay-disposition-v1"
TRACE_SCHEMA = "crystalhd-vaapi-trace-v4"
FORK_POLICY = "PARENT_PID_ONLY_CHILD_FORWARD_ONLY"
SCOPE = "OWN_REPLAY_DISPOSITION_ONLY"
FIELD_CONSTANTS = dict(topfield=16, bottomfield=24, interlaced_src=32, bottom_first=128)
# Maintained include/bc_dts_defs.h BC_STATUS: PumpDecodeInput retries BUSY only.
BC_BUSY = 2
MAX_BYTES = 96 * 1024 * 1024
EVENTS = {"restart", "input_attempt", "input_commit", "output", "disposition", "release"}
CONTEXT = ("driver_instance", "context", "generation")
KEY = CONTEXT + ("token",)
BINDING = KEY + ("decode_identity", "submission_ordinal", "surface", "owner")
LIVE = ("live_decode_identity", "live_submission_ordinal", "live_surface", "live_owner")
META = ("picture_number", "sess_num", "picture_meta_payload", "width", "height", "field_flags")
UNSIGNED = ("seq", "id", "pid", "tid", "monotonic_ns") + KEY + (
    "restart_ordinal", "receive_id", "native_seq") + LIVE + (
    "input_offset", "input_bytes", "unit_bytes", "cached_units", "pair_first_receive_id") + META
OPTIONAL_BOOLS = ("completed_before", "field_valid", "pair_valid", "replay_duplicate")
INPUT_FIELDS = ("completed_before", "fragmented", "consume_prefeed", "input_offset", "input_bytes", "unit_bytes")
DECISIONS = ("inspect", "observe", "field_role", "field_valid", "pair_valid", "replay_duplicate", "pair_first_receive_id")
LIMITS = [
    "OWN diagnostic classification only; no stock compatibility or release claim.",
    "Historical submit bindings never restore or replace live surface/owner metadata.",
    "Exact native receipt links are not source PTS, packet/pixel identity or latency attribution.",
    "Balanced original/replay records do not waive the separate900-original requirement.",
    "This census covers the selected paired-interlaced path; well-formed whole-picture observations are not qualified by its pair oracle.",
    "All native records, including retired/unbound replay fields, remain in raw event counts."]


def _key(row):
    return tuple(row[k] for k in KEY)


def _context(row):
    return tuple(row[k] for k in CONTEXT)


def _receipt(row):
    return {"driver_instance": row["driver_instance"], "native_seq": row["native_seq"],
            "replay_event_id": row["id"]}


def parse_report(raw):
    require(type(raw) is bytes and len(raw) <= MAX_BYTES and raw.endswith(b"\n"),
            "report exceeds quota or lacks final newline")
    lines = raw.splitlines()
    require(2 <= len(lines) <= 32770 and all(lines), "invalid bounded JSONL records")
    rows = [json.loads(line, object_pairs_hook=P.strict_object) for line in lines]
    require(all(type(row) is dict for row in rows), "report record is not an object")
    return rows


def check_document(rows, expected_pid):
    integer(expected_pid, "expected PID", 1, (1 << 31)-1)
    require(type(rows) is list and 2 <= len(rows) <= 32770 and
            all(type(r) is dict for r in rows), "invalid bounded report")
    header, footer = rows[0], rows[-1]
    require(header.get("type") == "header" and header.get("schema") == SCHEMA and
            footer.get("type") == "footer", "unsupported or missing header/footer")
    require(type(header.get("pid")) is int and header["pid"] == expected_pid, "foreign report PID")
    require(header.get("fork_policy") == FORK_POLICY and header.get("scope") == SCOPE,
            "unknown fork/observation scope")
    integer(header.get("capacity"), "capacity", 2, 32768)
    for key in ("bc_success", "va_success"):
        require(type(header.get(key)) is int and header[key] == 0, "invalid status constant")
    require(type(header.get("field_constants")) is dict and
            header["field_constants"] == FIELD_CONSTANTS and
            all(type(v) is int for v in header["field_constants"].values()),
            "foreign field parity constants")
    events = rows[1:-1]
    require(len(events) <= header["capacity"], "event capacity exceeded")
    for key in ("events", "attempted_events", "dropped", "inflight", "unpublished",
                "clock_failures", "lifecycle_failures"):
        integer(footer.get(key), "footer " + key)
    require(footer["events"] == footer["attempted_events"] == len(events) and
            all(footer[k] == 0 for k in ("dropped", "inflight", "unpublished", "clock_failures", "lifecycle_failures")) and
            type(footer.get("fork_seen")) is bool and footer.get("complete") is True,
            "incomplete publication/lifecycle/clock ledger")
    times, receives = {}, {}
    active_tid = {}
    for seq, row in enumerate(events):
        require(row.get("type") == "event" and row.get("event") in EVENTS, "unknown event")
        for key in UNSIGNED:
            integer(row.get(key), "event " + key)
        require(row["seq"] == seq and row["id"] == seq+1 and row["pid"] == expected_pid,
                "missing/reordered/foreign event")
        for key in CONTEXT + ("tid", "monotonic_ns"):
            require(row[key] > 0, "missing context/thread/clock identity")
        require(row["monotonic_ns"] >= times.get(row["tid"], 0), "thread clock regressed")
        times[row["tid"]] = row["monotonic_ns"]
        require("result" in row and (row["result"] is None or type(row["result"]) is int), "invalid result")
        if row["result"] is not None:
            signed(row["result"], "result", 64)
        for key in OPTIONAL_BOOLS:
            require(key in row and (row[key] is None or type(row[key]) is bool), "invalid optional boolean")
        for key in ("fragmented", "consume_prefeed"):
            require(type(row.get(key)) is bool, "invalid input boolean")
        require("restart_phase" in row and row["restart_phase"] in (None, "close", "open", "transition", "committed"), "invalid restart phase")
        for key in ("inspect", "observe"):
            require(key in row and row[key] in (None, "new", "duplicate", "unknown", "invalid"), "invalid disposition enum")
        require(row.get("field_role") in ("none", "first", "complement", "whole"), "invalid field role")
        kind = row["event"]
        if kind in ("restart", "input_attempt", "input_commit"):
            require(all(row[k] == 0 for k in LIVE + META + ("pair_first_receive_id",)) and
                    all(row[k] is None for k in ("inspect", "observe", "field_valid", "pair_valid", "replay_duplicate")) and
                    row["field_role"] == "none", "nonoutput invented output identity/decision")
        if kind in ("output", "disposition", "release"):
            require(row["completed_before"] is None and not row["fragmented"] and not row["consume_prefeed"] and
                    all(row[k] == 0 for k in ("input_offset", "input_bytes", "unit_bytes", "cached_units")), "output invented input state")
        if kind == "output":
            require(row["result"] is None and row["field_role"] == "whole" and
                    all(row[k] is None for k in ("inspect", "observe", "field_valid", "pair_valid", "replay_duplicate")) and
                    row["pair_first_receive_id"] == 0, "output snapshot predates actual decisions")
        if kind == "restart":
            require(row["restart_phase"] is not None and row["result"] is not None and
                    row["receive_id"] == row["native_seq"] == 0 and row["completed_before"] is None and
                    not row["fragmented"] and not row["consume_prefeed"] and
                    row["input_offset"] == row["input_bytes"] == row["unit_bytes"] == 0,
                    "invalid restart record")
        else:
            require(row["restart_phase"] is None, "nonrestart phase")
        if kind in ("input_attempt", "input_commit"):
            require(row["token"] > 0 and type(row["completed_before"]) is bool and
                    row["result"] is not None and row["receive_id"] == 0 and
                    (row["native_seq"] > 0 if kind == "input_commit" else row["native_seq"] == 0),
                    "input lacks state or exact commit receipt")
        elif kind in ("output", "disposition", "release"):
            require(row["receive_id"] > 0 and row["native_seq"] > 0, "missing output receipt")
            rid = row["receive_id"]
            if kind == "output":
                require(rid not in receives and row["tid"] not in active_tid,
                        "reused receive ID or overlapping same-thread receive")
                receives[rid] = [row]
                active_tid[row["tid"]] = rid
            else:
                require(rid in receives and active_tid.get(row["tid"]) == rid, "foreign/reordered receive lifetime")
                group = receives[rid]
                require(len(group) == (1 if kind == "disposition" else 2), "duplicate/reordered disposition/release")
                first = group[0]
                require(all(row[k] == first[k] for k in KEY + LIVE + META + ("restart_ordinal", "native_seq", "tid")),
                        "output lifetime metadata changed")
                require(row["result"] is not None, "missing actual disposition/release result")
                if kind == "release":
                    require(all(row[k] == group[1][k] for k in DECISIONS), "release changed actual disposition")
                    del active_tid[row["tid"]]
                group.append(row)
    require(not active_tid and all(len(v) == 3 for v in receives.values()), "unreleased output lifetime")
    return {"schema": SCHEMA, "header": header, "footer": footer, "events": events,
            "receives": receives, "pid": expected_pid, "limits": list(LIMITS)}


def read_report(path, expected_pid):
    path = Path(path)
    require(path.name.endswith("-" + str(expected_pid) + ".jsonl"), "report filename PID mismatch")
    result = check_document(parse_report(P.read_regular(path, MAX_BYTES)), expected_pid)
    result["report"] = str(path)
    return result


def audit_report(path, expected_pid):
    try:
        valid = read_report(path, expected_pid)
        return {"status": "PASS_SCOPED_REPLAY_DISPOSITION_OBSERVATION", "pid": expected_pid,
                "events": len(valid["events"]), "event_counts": dict(Counter(e["event"] for e in valid["events"])),
                "fork_seen": valid["footer"]["fork_seen"], "limits": list(LIMITS)}
    except (ValueError, OSError, KeyError, TypeError) as error:
        return {"status": "FAIL", "errors": [str(error)], "limits": list(LIMITS)}


def _native_index(rows, pid):
    require(type(rows) is list and rows, "native rows are required")
    indexed, histories, previous, times, last_offset = {}, {}, {}, {}, -1
    by_driver = defaultdict(list)
    for pair in rows:
        require(type(pair) in (tuple, list) and len(pair) == 2 and type(pair[1]) is dict, "native offset/row pair required")
        offset, row = pair
        integer(offset, "native byte offset")
        require(offset > last_offset, "native byte offsets are missing/reordered")
        last_offset = offset
        require(row.get("schema") == TRACE_SCHEMA and type(row.get("pid")) is int and row["pid"] == pid,
                "foreign native schema/PID")
        for key in BINDING + ("seq", "session", "ts_monotonic_ns", "operation"):
            integer(row.get(key), "native " + key)
        driver = row["driver_instance"]
        require(driver > 0 and row["session"] == driver and row["seq"] == previous.get(driver, 0)+1,
                "native session/sequence incomplete")
        require(row["ts_monotonic_ns"] > 0 and row["ts_monotonic_ns"] >= times.get(driver, 0), "native clock regressed")
        require(type(row.get("event")) is str and type(row.get("outcome")) is int, "invalid native event/outcome")
        require(not by_driver[driver] or by_driver[driver][-1][1]["event"] != "session_destroy", "native event after destroyed session")
        previous[driver], times[driver] = row["seq"], row["ts_monotonic_ns"]
        indexed[(driver, row["seq"])] = (offset, row)
        by_driver[driver].append(pair)
        if row["event"] == "submit_bind":
            key = _key(row)
            require(key not in histories and all(row[k] > 0 for k in BINDING) and row["outcome"] == 0,
                    "missing/ambiguous committed submit history")
            histories[key] = {"row": row, "offset": offset, "binding_end": None, "context_end": None}
        elif row["event"] == "binding_end":
            history = histories.get(_key(row))
            require(history is not None and history["binding_end"] is None and
                    all(history["row"][k] == row[k] for k in BINDING), "foreign/duplicate binding_end")
            history["binding_end"] = row
        elif row["event"] in ("context_reset", "context_destroy"):
            for history in histories.values():
                origin = history["row"]
                if origin["driver_instance"] == driver and origin["context"] == row["context"] and history["context_end"] is None:
                    history["context_end"] = row
    return indexed, histories


def _inside(offset, interval):
    return interval["begin"] <= offset < interval["end"]


def _link(event, stage, index, interval):
    key = (event["driver_instance"], event["native_seq"])
    require(key in index, "exact native receipt is absent")
    offset, row = index[key]
    require(_inside(offset, interval) and row["event"] == stage and
            interval["begin_ns"] <= row["ts_monotonic_ns"] <= event["monotonic_ns"] < interval["end_ns"] and
            all(row[k] == event[k] for k in KEY) and row["outcome"] == 0,
            "native receipt stage/window/context differs")
    if stage == "output_dequeued":
        require(all(event[a] == row[b] for a, b in zip(LIVE, BINDING[4:])), "raw live tuple differs from native output")
    return row


def _history_for(event, native, histories, replay):
    history = histories.get(_key(event))
    require(history is not None, "accepted input/output lacks unique original SubmitBind history")
    source = history["row"]
    require(source["seq"] < native["seq"] and
            (history["context_end"] is None or native["seq"] < history["context_end"]["seq"]),
            "native receipt outside original context generation lifetime")
    if not replay:
        require(all(source[k] == native[k] for k in BINDING), "new original live binding differs from admission")
        # The per-token decoded picture can survive public backing retirement.
        # Its exact retained tuple plus accepted New debt is provenance, not a
        # claim that the old public surface remains a live owner. A zero tuple
        # is still forbidden for a new original.
    else:
        require(all(native[k] == 0 or native[k] == source[k] for k in BINDING[4:]), "replay known identity conflicts with immutable history")
    return history


def _field(event):
    require(event["width"] > 0 and event["height"] > 0 and
            event["width"] % 2 == 0 and event["height"] % 4 == 0 and
            event["field_flags"] & FIELD_CONSTANTS["interlaced_src"], "invalid recorded field geometry/interlace")
    parity = event["field_flags"] & FIELD_CONSTANTS["bottomfield"]
    require(parity in (FIELD_CONSTANTS["topfield"], FIELD_CONSTANTS["bottomfield"]), "invalid recorded field parity")
    return parity


def _classify(events, native, interval, index, histories):
    require(events and native, "empty play evidence")
    raw = {stage: [r for _, r in native if r["event"] == stage]
           for stage in ("submit_bind", "input_sent", "output_dequeued", "materialized")}
    originals = {_key(r): r for r in raw["submit_bind"]}
    require(originals, "play has no original admissions")
    for key in originals:
        require(histories[key]["offset"] >= interval["begin"], "original admission is outside play")
    for name in ("token", "decode_identity", "submission_ordinal"):
        counts = Counter((r["driver_instance"], r["context"], r["generation"], r[name]) for r in raw["submit_bind"])
        require(all(v == 1 for v in counts.values()), "original identity reused: " + name)
    contexts = defaultdict(lambda: {"ordinal": 0, "pending": None, "cache": {}, "replays": Counter()})
    attempts, debts, original_debts, completed, outputs = {}, {}, {}, {}, {}
    commits, classifications, replay_pairs, original_pairs = [], [], [], []
    linked_inputs, linked_outputs = set(), set()
    for event in events:
        state = contexts[_context(event)]
        kind, ordinal = event["event"], event["restart_ordinal"]
        if kind == "restart":
            phase = event["restart_phase"]
            require(event["token"] == 0, "restart has invented unit token")
            if phase == "close":
                require(ordinal == state["ordinal"] and state["pending"] is None, "overlapping/incorrect restart ordinal")
                require(event["result"] == 0, "actual restart close failed")
                state["pending"] = {"next": "open", "cache": event["cached_units"], "tid": event["tid"]}
            else:
                pending = state["pending"]
                require(pending is not None and pending["next"] == phase and
                        pending["cache"] == event["cached_units"] and pending["tid"] == event["tid"], "restart phases/cache/thread differ")
                require(ordinal == state["ordinal"] + (phase == "committed"), "restart ordinal did not advance exactly once")
                if phase == "committed":
                    require(event["result"] == 1 and event["cached_units"] > 0, "restart was not successfully committed")
                    require(not any(k[:3] == _context(event) for k in debts), "restart with unpaid output debt")
                    state["ordinal"] = ordinal
                    state["cache"][ordinal] = event["cached_units"]
                    state["pending"] = None
                else:
                    require(event["result"] == (1 if phase == "transition" else 0), "actual restart " + phase + " failed")
                    pending["next"] = "committed" if phase == "transition" else "transition"
            continue
        require(ordinal == state["ordinal"] and state["pending"] is None, "event outside committed restart lifetime")
        key = _key(event)
        attempt_key = key + (ordinal,)
        if kind == "input_attempt":
            require(key in originals, "input attempt lacks admitted original history")
            require(event["result"] in (0, BC_BUSY), "actual input attempt has terminal BC error")
            require((ordinal > 0 and key in completed) if event["completed_before"] else key not in original_debts,
                    "input attempt completed-history state is inconsistent")
            require(event["input_bytes"] > 0 and event["unit_bytes"] > 0 and
                    event["input_offset"] + event["input_bytes"] <= event["unit_bytes"], "invalid input fragment extent")
            require(event["fragmented"] or (event["input_offset"] == 0 and event["input_bytes"] == event["unit_bytes"]), "nonfragmented partial input")
            track = attempts.setdefault(attempt_key, {"accepted": 0, "last": None, "committed": False,
                "properties": tuple(event[k] for k in ("completed_before", "fragmented", "consume_prefeed", "unit_bytes"))})
            require(not track["committed"] and track["properties"] == tuple(event[k] for k in ("completed_before", "fragmented", "consume_prefeed", "unit_bytes")) and
                    event["input_offset"] == track["accepted"], "retry/fragment coverage or state differs")
            if event["result"] == 0:
                track["accepted"] += event["input_bytes"]
            track["last"] = event
        elif kind == "input_commit":
            track = attempts.get(attempt_key)
            require(track is not None and not track["committed"] and track["last"]["result"] == 0 and
                    track["accepted"] == event["unit_bytes"] and event["result"] == 1 and
                    track["last"]["tid"] == event["tid"] and all(event[k] == track["last"][k] for k in INPUT_FIELDS), "commit lacks final accepted complete input attempt")
            require(key not in debts and key in originals, "input debt repeated or not admitted in this play")
            replay = event["completed_before"]
            native_input = _link(event, "input_sent", index, interval)
            history = _history_for(event, native_input, histories, replay)
            require((event["driver_instance"], event["native_seq"]) not in linked_inputs, "input native receipt reused")
            linked_inputs.add((event["driver_instance"], event["native_seq"]))
            if replay:
                require(ordinal > 0 and key in completed, "completed replay lacks successful restart and completed original")
                state["replays"][ordinal] += 1
                require(state["replays"][ordinal] <= state["cache"][ordinal], "replay exceeds captured restart cache")
            else:
                require(key not in original_debts, "original input committed more than once")
            debt = {"input": event, "native": native_input, "history": history, "replay": replay, "first": None,
                    "outputs": [], "releases": [], "ordinal": ordinal}
            debts[key] = debt
            if not replay:
                original_debts[key] = debt
            commits.append({**_receipt(event), "role": "completed_replay" if replay else "original", "history_key": list(key)})
            track["committed"] = True
        elif kind == "output":
            require(key in debts, "unknown/nonreplay output has no accepted logical debt")
            debt = debts[key]
            require(ordinal == debt["ordinal"] and len(debt["outputs"]) < 2, "extra output or wrong replay batch")
            native_output = _link(event, "output_dequeued", index, interval)
            _history_for(event, native_output, histories, debt["replay"])
            require(debt["native"]["seq"] < native_output["seq"], "output precedes accepted input")
            receipt = (event["driver_instance"], event["native_seq"])
            require(receipt not in linked_outputs, "output native receipt reused")
            linked_outputs.add(receipt)
            debt["outputs"].append(event)
            outputs[event["receive_id"]] = debt
        elif kind == "disposition":
            debt = outputs.get(event["receive_id"])
            require(debt is not None and event["result"] == 0, "actual output disposition rejected")
            expected = "duplicate" if debt["replay"] else "new"
            require(event["inspect"] == expected and event["replay_duplicate"] is debt["replay"] and event["field_valid"] is True,
                    "actual inspection/field ownership is not qualified")
            _field(event)
            if len(debt["outputs"]) == 1:
                require(event["field_role"] == "first" and event["pair_valid"] is None and event["observe"] is None and event["pair_first_receive_id"] == 0,
                        "first field invented pair/Observe decision")
                debt["first"] = event
            else:
                first = debt["first"]
                require(first is not None and len(debt["releases"]) == 1 and
                        event["field_role"] == "complement" and event["pair_valid"] is True and
                        event["pair_first_receive_id"] == first["receive_id"] and event["observe"] == expected,
                        "pair lacks prior released first field/actual Observe")
                require(_field(first) != _field(event) and
                        all(first[k] == event[k] for k in META[:-1]) and
                        (first["field_flags"] & 128) == (event["field_flags"] & 128), "raw field parity/identity/order/geometry mismatch")
        elif kind == "release":
            debt = outputs.get(event["receive_id"])
            require(debt is not None and event["result"] == 0, "actual DIL output release failed")
            debt["releases"].append(event)
            if len(debt["releases"]) == 2:
                pair = {"history_key": list(key), "restart_ordinal": ordinal,
                        "input": _receipt(debt["input"]), "outputs": [_receipt(e) for e in debt["outputs"]],
                        "release_event_ids": [e["id"] for e in debt["releases"]]}
                (replay_pairs if debt["replay"] else original_pairs).append(pair)
                if not debt["replay"]:
                    completed[key] = debt
                del debts[key]
                for output in debt["outputs"]:
                    classifications.append({**_receipt(output), "receive_id": output["receive_id"],
                        "role": "replay_duplicate_field" if debt["replay"] else "original_field",
                        "history_key": list(key), "live_tuple": {k: output[k] for k in LIVE},
                        "provenance_scope": "HISTORICAL_ONLY" if debt["replay"] else "ORIGINAL_ADMISSION"})
    require(not debts and all(state["pending"] is None for state in contexts.values()), "unfinished output/restart debt")
    require(all(t["committed"] or t["accepted"] == 0 for t in attempts.values()), "accepted fragment/final input has no logical commit")
    require(set(completed) == set(originals) == set(original_debts), "not every original has one accepted input and validated released pair")
    require(linked_inputs == {(r["driver_instance"], r["seq"]) for r in raw["input_sent"]}, "unmatched original native input_sent population")
    require(linked_outputs == {(r["driver_instance"], r["seq"]) for r in raw["output_dequeued"]}, "unmatched original native output_dequeued population")
    materialized = Counter()
    first_replay_seq = {}
    for event in events:
        if event["event"] == "input_commit" and event["completed_before"]:
            first_replay_seq.setdefault(_key(event), event["native_seq"])
    for row in raw["materialized"]:
        key = _key(row)
        require(key in completed and row["outcome"] == 0 and all(row[k] == originals[key][k] for k in BINDING), "materialized has foreign original binding")
        materialized[key] += 1
        require(materialized[key] == 1 and row["seq"] > completed[key]["outputs"][-1]["native_seq"], "repeated/premature original materialization")
        history = histories[key]
        require(history["binding_end"] is None or row["seq"] < history["binding_end"]["seq"], "materialization after live binding retirement")
        # Maintained ProcessDecodedOutput emits M synchronously in the New
        # complement before returning; Duplicate returns before that site.
        # The existing driver mutex prevents Pump's completed replay InputSent
        # interleaving with it. This is a source-causal bound for this backend,
        # not a nearest-time attribution or a newly required public M.
        require(key not in first_replay_seq or row["seq"] < first_replay_seq[key], "replay rematerialized an original picture")
    provenance = []
    for key, origin in originals.items():
        end = histories[key]["binding_end"]
        provenance.append({"history_key": list(key), "binding": {k: origin[k] for k in BINDING},
            "submit_native_seq": origin["seq"], "binding_end_native_seq": None if end is None else end["seq"],
            "scope": "HISTORICAL_ONLY", "live_ownership_restored": False})
    return {"original_input_commits": len(original_debts), "completed_replay_input_commits": len(replay_pairs),
            "original_field_records": 2*len(original_pairs), "replay_field_records": 2*len(replay_pairs),
            "original_validated_pairs": len(original_pairs), "replay_validated_pairs": len(replay_pairs),
            "materialized_originals": len(materialized), "replay_status": "EXERCISED" if replay_pairs else "NOT_EXERCISED",
            "materialization_scope": "Present public materializations are original-only and exactly once; private New completion does not prove public readiness.",
            "public_materialization_population": "NOT_OBSERVED" if not materialized else
                "ALL_ORIGINALS_OBSERVED" if len(materialized) == len(originals) else "OBSERVED_SUBSET",
            "originals_without_public_materialization": len(originals)-len(materialized),
            "public_pixel_qualification": "NOT_ESTABLISHED_BY_THIS_CENSUS",
            "successful_restarts": sum(s["ordinal"] for s in contexts.values()),
            "input_receipts": commits, "output_classifications": classifications,
            "original_pairs": original_pairs, "replay_pairs": replay_pairs, "historical_provenance": provenance}


def census_case(validated, native_rows_with_byte_offsets, intervals, pid, expected_originals=900):
    integer(expected_originals, "expected original population", 1, 32768)
    require(validated["pid"] == pid and type(intervals) is dict and intervals, "missing/foreign case intervals")
    index, histories = _native_index(native_rows_with_byte_offsets, pid)
    windows = []
    for cycle, interval in intervals.items():
        require(type(cycle) is str and type(interval) is dict, "invalid play interval")
        for key in ("begin", "end", "begin_ns", "end_ns"):
            integer(interval.get(key), "interval " + key)
        require(interval["begin"] < interval["end"] and interval["begin_ns"] < interval["end_ns"], "empty/reversed play interval")
        for _, prior in windows:
            require(interval["begin"] >= prior["end"] or prior["begin"] >= interval["end"], "overlapping native windows")
            require(interval["begin_ns"] >= prior["end_ns"] or prior["begin_ns"] >= interval["end_ns"], "overlapping clock windows")
        windows.append((cycle, interval))
    plays, assigned = {}, set()
    for cycle, interval in windows:
        events = [e for e in validated["events"] if interval["begin_ns"] <= e["monotonic_ns"] < interval["end_ns"]]
        assigned.update(e["id"] for e in events)
        native = [(offset, row) for offset, row in native_rows_with_byte_offsets if _inside(offset, interval)]
        count = sum(row["event"] == "submit_bind" for _, row in native)
        play = {"status": "FAIL", "ledger_status": "FAIL", "source_population_qualified": False,
                "observed_submit_bindings": count, "source_metadata_pictures": expected_originals,
                "event_counts": dict(Counter(row["event"] for _, row in native)),
                "replay_event_counts": dict(Counter(row["event"] for row in events)), "errors": [],
                "actual_media_PTS_to_binding": "NOT_OBSERVED", "raw_records_preserved": True}
        try:
            ids = {e["id"] for e in events}
            require(all(all(e["id"] in ids for e in group) or all(e["id"] not in ids for e in group)
                        for group in validated["receives"].values()), "play window cuts an output lifetime")
            play.update(_classify(events, native, interval, index, histories))
            play["ledger_status"] = "PASS"
            play["source_population_qualified"] = count == expected_originals
            if count == expected_originals:
                play["status"] = "PASS"
            else:
                play["errors"].append("require exactly" + str(expected_originals) + " distinct original bindings; observed" + str(count))
        except (ValueError, KeyError, TypeError) as error:
            play["errors"].append(str(error))
        plays[cycle] = play
    unassigned = [e["id"] for e in validated["events"] if e["id"] not in assigned]
    # Restart/input/output operations outside the declared media windows cannot
    # be silently assigned to the nearest play or dropped from classification.
    if unassigned:
        for play in plays.values():
            play.update(status="FAIL", ledger_status="FAIL", source_population_qualified=False)
            play["errors"].append("unassigned replay event IDs outside declared plays")
    return {"status": "PASS_SCOPED_REPLAY_DISPOSITION_CENSUS" if all(p["ledger_status"] == "PASS" for p in plays.values()) else "FAIL",
            "plays": plays, "unassigned_event_ids": unassigned, "limits": list(LIMITS)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--pid", type=int, required=True)
    args = parser.parse_args()
    result = audit_report(args.report, args.pid)
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "PASS_SCOPED_REPLAY_DISPOSITION_OBSERVATION" else 1


if __name__ == "__main__":
    raise SystemExit(main())
