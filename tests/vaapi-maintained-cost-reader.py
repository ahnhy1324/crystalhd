#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline OWN maintained-backend paired-clock reader, not a performance fix.

Only explicit frozen scalar artifacts are read. Nested interval subtraction is
same-thread bookkeeping, not a playback/CPU budget or source-picture identity.
"""
import argparse
import collections
import importlib.util
import json
import math
from pathlib import Path
import sys


_spec = importlib.util.spec_from_file_location(
    "_maintained_cost_safe", Path(__file__).with_name("vaapi-postfilter-reader.py"))
P = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(P)
require, integer = P.require, P.integer
SCHEMA = "crystalhd-maintained-cost-v2"
SCHEMA_CAPACITIES = {"crystalhd-maintained-cost-v1": 65536, SCHEMA: 131072}
FORK_POLICY = "PARENT_PID_ONLY_CHILD_FORWARD_ONLY"
SCOPE = "OWN_MAINTAINED_BACKEND_ONLY"
CLOCKS = dict(wall="CLOCK_MONOTONIC", cpu="CLOCK_THREAD_CPUTIME_ID")
STAGES = ("receive_one", "dil_acquire", "dil_release", "process_output", "first_field_stage",
          "second_field_process", "field_convert", "field_convert_pixels", "canvas_compose",
          "canvas_pixels", "cpu_access_begin", "cpu_access_end", "decoded_cpu_begin", "decoded_cpu_end",
          "i915_domain", "public_copy", "public_copy_pixels", "public_promote", "direct_public",
          "sync_admission", "sync_body", "diagnostic_capture")
FLAGS = dict(compact=1, decoded_public=2, direct_public=4, selected_capture=8,
             i915_wc=16, read=32, write=64)
BINDING = ("driver_instance", "context", "generation", "token", "decode_identity",
           "submission_ordinal", "surface", "owner")
KINDS = {"none", "decode", "surface", "full"}
PHASES = {"entry", "inherited", "resolved_after_output", "resolved_after_admission"}
MAX_REPORT_BYTES = 128 * 1024 * 1024
LIMITS = [
    "OWN maintained-backend timing only; no product fix, bottleneck, stock or release qualification.",
    "Wall and thread CPU are distinct accounting clocks; negative wall-minus-CPU is unavailable, never clamped.",
    "Available wall-minus-thread-CPU mixes blocking, preemption and measurement overhead, not pure I/O or mutex wait.",
    "Exclusive values subtract recorded direct-child intervals, whose CPU and wall endpoints differ.",
    "Nested stages, different threads and overlapping picture populations cannot be summed as a playback budget.",
    "Partial bindings stay unknown; full scalar candidates are not native completion, media PTS or pixel identity.",
    "Missing stages are NOT_OBSERVED; valid diagnostic subsets never lower the separate900-native criterion."]


def parse_report(raw):
    require(type(raw) is bytes and len(raw) <= MAX_REPORT_BYTES and raw.endswith(b"\n"),
            "report exceeds quota or lacks final newline")
    lines = raw.splitlines()
    require(2 <= len(lines) <= max(SCHEMA_CAPACITIES.values()) + 2 and all(lines),
            "invalid bounded JSONL records")
    header = json.loads(lines[0], object_pairs_hook=P.strict_object)
    require(len(lines) <= schema_capacity(header) + 2, "schema record capacity exceeded")
    rows = [header, *[json.loads(line, object_pairs_hook=P.strict_object) for line in lines[1:]]]
    require(all(type(row) is dict for row in rows), "records must be objects")
    return rows


def schema_capacity(header):
    require(type(header) is dict and type(header.get("schema")) is str and
            header["schema"] in SCHEMA_CAPACITIES, "unsupported schema")
    return SCHEMA_CAPACITIES[header["schema"]]


def binding_value(event):
    for key in BINDING:
        integer(event.get(key), "binding." + key)
    kind, phase = event.get("binding_kind"), event.get("binding_phase")
    require(kind in KINDS and phase in PHASES, "unknown binding kind/phase")
    if kind == "none":
        require(all(event[k] == 0 for k in BINDING[1:]), "none binding claims source identity")
    elif kind == "decode":
        require(all(event[k] > 0 for k in BINDING[:3]), "decode binding lacks driver/context/generation")
    elif kind == "full":
        require(all(event[k] > 0 for k in BINDING), "full binding has an unknown scalar")
        require(event["surface"] < (1 << 32)-1 and event["owner"] < (1 << 32)-1,
                "full binding claims invalid VA surface/owner")
    # Surface records deliberately preserve whatever scalars were actually
    # available, including all-zero unknown identity; never upgrade to full.


def check_document(rows, expected_pid, report_path=None):
    """Validate complete publication, clocks and actual parent trees; raises."""
    integer(expected_pid, "expected PID", 1, (1 << 31)-1)
    require(type(rows) is list and 2 <= len(rows) <= max(SCHEMA_CAPACITIES.values()) + 2 and
            all(type(r) is dict for r in rows), "invalid report document")
    header, footer = rows[0], rows[-1]
    maximum_capacity = schema_capacity(header)
    require(len(rows) <= maximum_capacity + 2, "schema record capacity exceeded")
    require(header.get("type") == "header" and
            footer.get("type") == "footer", "unsupported schema or missing header/footer")
    require(type(header.get("pid")) is int and header["pid"] == expected_pid,
            "foreign header PID")
    require(header.get("fork_policy") == FORK_POLICY and header.get("scope") == SCOPE,
            "foreign process/backend observation scope")
    require(header.get("clock_names") == CLOCKS, "unqualified paired clock identities")
    require(type(header.get("stages")) is list and len(header["stages"]) == len(STAGES) and
            all(type(stage) is str for stage in header["stages"]) and
            set(header["stages"]) == set(STAGES), "stage contract is missing, duplicated or foreign")
    require(type(header.get("flags")) is dict and set(header["flags"]) == set(FLAGS) and
            all(type(header["flags"][k]) is int and header["flags"][k] == v for k,v in FLAGS.items()),
            "flag contract differs")
    capacity = integer(header.get("capacity"), "capacity", 1, maximum_capacity)
    events = rows[1:-1]
    require(len(events) <= capacity, "event capacity exceeded")
    for key in ("events", "attempted_events", "dropped", "inflight", "unpublished", "clock_failures", "lifecycle_failures"):
        integer(footer.get(key), "footer." + key)
    require(footer["events"] == footer["attempted_events"] == len(events), "publication counts differ")
    require(all(footer[k] == 0 for k in ("dropped", "inflight", "unpublished", "clock_failures", "lifecycle_failures")) and
            type(footer.get("fork_seen")) is bool and footer.get("complete") is True,
            "incomplete/overflowed/clock-failed/lifecycle-failed observation")
    by_id, children = {}, collections.defaultdict(list)
    for seq,e in enumerate(events):
        require(e.get("type") == "event" and type(e.get("seq")) is int and e["seq"] == seq and
                type(e.get("id")) is int and e["id"] == seq+1, "missing/duplicate/reordered scope ID/sequence")
        require(type(e.get("pid")) is int and e["pid"] == expected_pid, "foreign event PID")
        tid = integer(e.get("tid"), "entry TID", 1, (1 << 31)-1)
        require(type(e.get("end_tid")) is int and e["end_tid"] == tid, "scope ended on another thread")
        require(e.get("stage") in STAGES, "foreign timing stage")
        parent = integer(e.get("parent_id"), "parent ID", 0, seq)
        if parent:
            require(parent in by_id, "parent scope is absent or later than child")
        integer(e.get("wall_begin_ns"), "wall begin", 1)
        integer(e.get("wall_end_ns"), "wall end", e["wall_begin_ns"])
        integer(e.get("cpu_begin_ns"), "CPU begin")
        integer(e.get("cpu_end_ns"), "CPU end", e["cpu_begin_ns"])
        wall = integer(e.get("wall_ns"), "wall delta")
        cpu = integer(e.get("cpu_ns"), "CPU delta")
        require(wall == e["wall_end_ns"]-e["wall_begin_ns"] and
                cpu == e["cpu_end_ns"]-e["cpu_begin_ns"], "paired clock arithmetic differs")
        binding_value(e)
        if e["binding_phase"] == "inherited":
            require(parent > 0, "inherited identity has no actual parent")
        detail = integer(e.get("detail"), "stage detail")
        flags = integer(e.get("flags"), "stage flags")
        require(flags & ~sum(FLAGS.values()) == 0, "unknown branch flag bits")
        if e["stage"] in ("cpu_access_begin", "cpu_access_end"):
            require(1 <= detail <= 3, "invalid CPU access direction")
            expected = (FLAGS["read"] if detail & 1 else 0) | (FLAGS["write"] if detail & 2 else 0)
            require(flags & (FLAGS["read"] | FLAGS["write"]) == expected,
                    "CPU access flags disagree with original direction")
        elif e["stage"] == "field_convert":
            require(1 <= detail <= 3 or (type(e.get("result")) is int and e["result"] == 0),
                    "invalid field mask requires an actually recorded false conversion result")
        elif e["stage"] in ("field_convert_pixels", "diagnostic_capture"):
            require(1 <= detail <= 3, "invalid field/capture detail")
        else:
            require(detail == 0, "undocumented nonzero stage detail")
        require("result" in e and (e["result"] is None or type(e["result"]) is int),
                "result must be raw integer or unknown null")
        if e["result"] is not None:
            P.signed(e["result"], "raw result", 64)
        if parent:
            p = by_id[parent]
            require(p["tid"] == tid and p["pid"] == e["pid"], "parent identity belongs to another thread/process")
            for clock in ("wall", "cpu"):
                require(p[clock+"_begin_ns"] <= e[clock+"_begin_ns"] <=
                        e[clock+"_end_ns"] <= p[clock+"_end_ns"], "child escapes parent " + clock + " interval")
            if e["binding_phase"] == "inherited":
                if p["binding_phase"] in ("resolved_after_output", "resolved_after_admission"):
                    # A closed earlier child retains the parent's then-known
                    # partial tuple. Parent publication may refine0→known,
                    # without retroactively granting identity to that child.
                    require(all(e[k] == 0 or e[k] == p[k] for k in BINDING),
                            "inherited known scalar conflicts with later-refined parent")
                else:
                    require(e["binding_kind"] == p["binding_kind"] and all(e[k] == p[k] for k in BINDING),
                            "inherited binding differs from actual parent")
            # Execution nesting is not picture identity: waiting for picture A
            # can harvest picture B. Explicit/resolved children retain their
            # own observed binding rather than being forced onto the parent.
        by_id[e["id"]] = e
        children[(tid,parent)].append(e)
    for siblings in children.values():
        ordered = sorted(siblings, key=lambda e: (e["wall_begin_ns"],e["id"]))
        for a,b in zip(ordered,ordered[1:]):
            require(a["wall_end_ns"] <= b["wall_begin_ns"] and a["cpu_end_ns"] <= b["cpu_begin_ns"],
                    "same-thread direct siblings overlap or CPU clock regresses")
            require(a["id"] < b["id"], "same-thread sibling entry order regressed")
    durations = {}
    for e in events:
        direct = children.get((e["tid"],e["id"]),[])
        exclusive_wall = e["wall_ns"]-sum(c["wall_ns"] for c in direct)
        exclusive_cpu = e["cpu_ns"]-sum(c["cpu_ns"] for c in direct)
        require(exclusive_wall >= 0 and exclusive_cpu >= 0, "negative exclusive interval subtraction")
        difference = exclusive_wall-exclusive_cpu
        inclusive_difference = e["wall_ns"]-e["cpu_ns"]
        durations[e["id"]] = dict(inclusive_wall_ns=e["wall_ns"],inclusive_cpu_ns=e["cpu_ns"],
            inclusive_offcpu_ns=inclusive_difference if inclusive_difference >= 0 else None,
            inclusive_offcpu_status="UNAVAILABLE_ENDPOINT_SUBTRACTION" if inclusive_difference < 0 else "OBSERVED_INTERVAL_SUBTRACTION",
            exclusive_wall_ns=exclusive_wall,
            exclusive_cpu_ns=exclusive_cpu,exclusive_offcpu_ns=difference if difference >= 0 else None,
            exclusive_offcpu_status="UNAVAILABLE_ENDPOINT_SUBTRACTION" if difference < 0 else "OBSERVED_INTERVAL_SUBTRACTION",
            direct_child_ids=[c["id"] for c in direct])
    path = str(Path(report_path).absolute()) if report_path is not None else None
    if path is not None:
        require(Path(path).suffix == ".jsonl" and Path(path).stem.endswith("-"+str(expected_pid)),
                "report filename lost owner PID binding")
    return dict(header=header,footer=footer,events=events,by_id=by_id,durations=durations,report_path=path,
                process_scope=dict(pid=expected_pid,fork_policy=FORK_POLICY,fork_seen=footer["fork_seen"],children_observed=False),
                source_clock_comparison=False,native_completion_claim=False)


def read_report(path, expected_pid):
    return check_document(parse_report(P.read_regular(path,MAX_REPORT_BYTES)),expected_pid,path)


def distribution(values):
    values = sorted(values)
    if not values:
        return dict(status="NOT_OBSERVED",count=0,p50_ns=None,p95_ns=None,max_ns=None,sum_ns=None)
    count = len(values)
    require(all(type(v) is int and v >= 0 for v in values), "invalid nonnegative duration sample")
    return dict(status="OBSERVED",count=count,p50_ns=values[math.ceil(count*.5)-1],
                p95_ns=values[math.ceil(count*.95)-1],max_ns=values[-1],sum_ns=sum(values))


def stage_summary(events, durations):
    result = {}
    for stage in STAGES:
        selected = [e for e in events if e["stage"] == stage]
        metrics = {metric:distribution([durations[e["id"]][metric] for e in selected]) for metric in
                   ("inclusive_wall_ns","inclusive_cpu_ns","exclusive_wall_ns","exclusive_cpu_ns")}
        for metric in ("inclusive_offcpu_ns","exclusive_offcpu_ns"):
            unavailable = sum(durations[e["id"]][metric] is None for e in selected)
            if unavailable:
                metrics[metric] = dict(status="UNAVAILABLE_ENDPOINT_SUBTRACTION",count=len(selected),
                    negative_sample_count=unavailable,p50_ns=None,p95_ns=None,max_ns=None,sum_ns=None)
            else:
                metrics[metric] = distribution([durations[e["id"]][metric] for e in selected])
        result[stage] = dict(status="OBSERVED" if selected else "NOT_OBSERVED",events=len(selected),metrics=metrics,
            tids=sorted({e["tid"] for e in selected}),
            detail_event_counts=dict(sorted(collections.Counter(str(e["detail"]) for e in selected).items())),
            flag_event_counts={name:sum(bool(e["flags"] & bit) for e in selected) for name,bit in FLAGS.items()},
            flag_combination_event_counts=dict(sorted(collections.Counter(str(e["flags"]) for e in selected).items())),
            raw_result_event_counts=dict(sorted(collections.Counter("null" if e["result"] is None else str(e["result"]) for e in selected).items())),
            binding_kind_event_counts=dict(collections.Counter(e["binding_kind"] for e in selected)),
            binding_phase_event_counts=dict(collections.Counter(e["binding_phase"] for e in selected)))
    return result


def census_case(validated, intervals):
    """Census explicit controller windows; missing/partial source stays unknown."""
    require(type(intervals) is dict and bool(intervals), "explicit per-play windows are required")
    windows=[]
    for key,window in intervals.items():
        require(type(window) is dict, "window must be an object")
        begin=integer(window.get("begin_ns"),"play begin",1)
        end=integer(window.get("end_ns"),"play end",begin+1)
        windows.append((begin,end,str(key)))
    ordered=sorted(windows)
    require(all(a[1] <= b[0] for a,b in zip(ordered,ordered[1:])), "per-play windows overlap")
    plays={}
    for begin,end,key in windows:
        require(not any(e["wall_begin_ns"] < end and e["wall_end_ns"] > begin and
                        not(begin <= e["wall_begin_ns"] and e["wall_end_ns"] <= end)
                        for e in validated["events"]), "play window cuts through a timing scope")
        selected=[e for e in validated["events"] if begin <= e["wall_begin_ns"] and e["wall_end_ns"] <= end]
        known=[e for e in selected if all(e[k] > 0 for k in BINDING[:3]) and e["binding_kind"] != "none"]
        full=collections.defaultdict(list)
        for e in selected:
            if e["binding_kind"] == "full":
                full[tuple(e[k] for k in BINDING)].append(e)
        context_ids=sorted({tuple(e[k] for k in BINDING[:3]) for e in known})
        plays[key]=dict(status="PASS_SCOPED_MAINTAINED_COST_CENSUS" if selected and known else
                            "NOT_QUALIFIED_MAINTAINED_COST_CENSUS",
            not_qualified_reason=None if selected and known else "No observed scopes with meaningful driver/context/generation identity",
            begin_ns=begin,end_ns=end,events=len(selected),
            stage_population_scope="All contained observed contexts/TIDs in this window; not a unique primary-decoder selection",
            source_contexts=[dict(zip(BINDING[:3],identity)) for identity in context_ids],
            stages=stage_summary(selected,validated["durations"]),
            binding_kind_event_counts=dict(collections.Counter(e["binding_kind"] for e in selected)),
            full_binding_candidates=[dict(binding=dict(zip(BINDING,identity)),
                stage_event_counts=dict(collections.Counter(e["stage"] for e in records)),
                binding_phase_event_counts=dict(collections.Counter(e["binding_phase"] for e in records)))
                for identity,records in sorted(full.items())],
            full_native_binding_correlation="NOT_PERFORMED_BY_COST_READER",media_pts_comparison=False,
            strict900_native_source_qualification="SEPARATE_UNCHANGED")
    return dict(status="PASS_SCOPED_MAINTAINED_COST_CENSUS" if all(
        p["status"] == "PASS_SCOPED_MAINTAINED_COST_CENSUS" for p in plays.values()) else
        "NOT_QUALIFIED_MAINTAINED_COST_CENSUS",plays=plays,limits=LIMITS,
        exclusive_scope="Recorded parent minus fully contained nonoverlapping direct same-TID children; endpoints are not physically aligned",
        source_clock_comparison=False,native_completion_claim=False)


def audit_report(path, expected_pid):
    try:
        valid=read_report(path,expected_pid)
        return dict(status="PASS_SCOPED_MAINTAINED_COST_OBSERVATION",header=valid["header"],footer=valid["footer"],
                    process_scope=valid["process_scope"],events=len(valid["events"]),
                    stages=stage_summary(valid["events"],valid["durations"]),limits=LIMITS,
                    source_clock_comparison=False,native_completion_claim=False)
    except (ValueError,OSError,KeyError,TypeError,OverflowError,UnicodeError) as exc:
        return dict(status="FAIL",reason=str(exc),limits=LIMITS)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report")
    parser.add_argument("--pid",type=int,required=True)
    args=parser.parse_args()
    result=audit_report(args.report,args.pid)
    print(json.dumps(result,sort_keys=True))
    return 1 if result["status"] == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
