#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded offline reader for OWN post-filter observations, not stock acceptance.

Read only explicitly named frozen artifacts. Raw frame, declared graph and
measured sink clocks stay separate. No media, pointer, device or DMA access is
performed, and a missing original source EOF is never supplied by this reader.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import re
import sys


_spec = importlib.util.spec_from_file_location(
    "_postfilter_safe", Path(__file__).with_name("vaapi-centered-payload-reader.py"))
P = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(P)
require, read_regular, strict_object = P.require, P.read_regular, P.strict_object
SCHEMA = "crystalhd-postfilter-v1"
BASIS = "PINNED_ORIGINAL_SOURCE_PLUS_ACTUAL_NV12_SINK_OUTPUT"
PROVIDERS = {"filter": "/app/lib/libavfilter.so.12.1.102",
             "util": "/app/lib/libavutil.so.61.1.102"}
VERSIONS = {"filter": 786790, "util": 3998054}
CREATE, PARSE, FREE = ("avfilter_graph_create_filter", "avfilter_graph_parse_ptr",
                       "avfilter_graph_free")
ADD = {"av_buffersrc_add_frame", "av_buffersrc_add_frame_flags"}
GET = {"av_buffersink_get_frame", "av_buffersink_get_frame_flags"}
CLOSE = "av_buffersrc_close"
APIS = ADD | GET | {CREATE, PARSE, FREE, CLOSE}
EAGAIN, EOF = -11, -541478725
MAX_REPORT_BYTES = 32 * 1024 * 1024
MAX_SNAPSHOT_BYTES = 1920 * 1088 * 3 // 2
FRAME_KEYS = ("pts", "best_effort_timestamp", "duration", "time_base", "format",
              "width", "height", "linesize", "flags", "sample_aspect_ratio",
              "color_range", "color_primaries", "color_trc", "colorspace",
              "chroma_location", "crop")
LIMITS = [
    "OWN observation/reference only; no stock compatibility or release claim.",
    "Frame raw clocks, source stream clock, graph arguments and measured sink clocks remain distinct.",
    "Successful calls are measured returns, not an inferred 900-input/1800-output or scanout proof.",
    "Selected active NV12 bytes do not independently qualify the source decoder, scaler or colour conversion.",
    "Missing original NULL/close/EOF stays missing; same-input prefix never supplies client EOF."]


def integer(value, label, low=0, high=(1 << 64)-1):
    require(type(value) is int and low <= value <= high, label + " is not a bounded integer")
    return value


def signed(value, label, bits=32):
    return integer(value, label, -(1 << (bits-1)), (1 << (bits-1))-1)


def vector(value, size, label, signed_values=True):
    require(type(value) is list and len(value) == size, label + " has wrong vector length")
    for v in value:
        (signed(v, label) if signed_values else integer(v, label))
    return value


def rational(value, label):
    vector(value, 2, label)
    n, d = value
    require(n >= 0 and d >= 0 and (d != 0 or n == 0), label + " has invalid rational")
    return value                    # Preserve 0/0 and 0/1; never derive seconds.


def pointer(value, label, nonzero=False):
    require(type(value) is str and re.fullmatch(r"0x[0-9a-fA-F]{1,16}", value),
            label + " must be a descriptive hexadecimal pointer")
    result = int(value, 16)
    require(not nonzero or result != 0, label + " must be nonzero")
    return result


def text(value, label, maximum):
    require(type(value) is str and "\x00" not in value and
            len(value.encode("utf-8")) < maximum, label + " is missing or truncated")
    return value


def frame_value(frame, header):
    require(type(frame) is dict and all(k in frame for k in FRAME_KEYS + ("snapshot",)),
            "missing raw frame properties")
    for key in ("pts", "best_effort_timestamp", "duration"):
        signed(frame[key], key, 64)
    for key in ("format", "width", "height", "flags", "color_range", "color_primaries",
                "color_trc", "colorspace", "chroma_location"):
        signed(frame[key], key)
    rational(frame["time_base"], "frame.time_base")
    rational(frame["sample_aspect_ratio"], "frame.sample_aspect_ratio")
    vector(frame["linesize"], 2, "frame.linesize")
    vector(frame["crop"], 4, "frame.crop", False)
    w, h = frame["width"], frame["height"]
    require(w >= 0 and h >= 0, "negative frame geometry")
    if frame["format"] == header["nv12_format"]:
        require(w > 0 and h > 0 and not w % 2 and not h % 2 and
                w <= header["max_width"] and h <= header["max_height"] and
                all(v >= w for v in frame["linesize"]), "invalid active NV12 geometry/stride")
        top, bottom, left, right = frame["crop"]
        require(top + bottom < h and left + right < w, "invalid active NV12 crop")
    else:
        require(frame["snapshot"] is None, "non-NV12 frame must not claim a snapshot")
    return frame


def parse_report(raw):
    require(type(raw) is bytes and len(raw) <= MAX_REPORT_BYTES and raw.endswith(b"\n"),
            "report exceeds quota or is not newline-terminated")
    lines = raw.splitlines()
    require(2 <= len(lines) <= 16386 and all(lines), "invalid bounded JSONL records")
    rows = [json.loads(line, object_pairs_hook=strict_object) for line in lines]
    require(all(type(row) is dict for row in rows), "report records must be objects")
    return rows


def check_document(rows, snapshot_blobs, expected_pid, expected_providers=None,
                   report_path=None):
    """Validate the complete ledger and explicit snapshots; pure fixture entry.

    snapshot_blobs maps report-directory basenames to bytes. The returned paths
    refer to that directory, not a path supplied by an event. No blobs are kept
    in the JSON-compatible result. Frozen parent-directory scope is assumed.
    """
    integer(expected_pid, "expected PID", 1, (1 << 31)-1)
    require(type(rows) is list and 2 <= len(rows) <= 16386 and
            all(type(r) is dict for r in rows), "invalid report document")
    header, footer = rows[0], rows[-1]
    require(header.get("type") == "header" and header.get("schema") == SCHEMA and
            footer.get("type") == "footer", "missing header/footer or unsupported schema")
    require(header.get("pid") == expected_pid and type(header["pid"]) is int,
            "foreign report PID")
    expected = dict(PROVIDERS if expected_providers is None else expected_providers)
    require(set(expected) == {"filter", "util"}, "both expected providers are required")
    for key in expected:
        require(type(expected[key]) is str and Path(expected[key]).is_absolute() and
                "\x00" not in expected[key] and Path(expected[key]).name == Path(PROVIDERS[key]).name and
                header.get(key + "_provider") == expected[key] and
                type(header.get(key + "_version")) is int and
                header[key + "_version"] == VERSIONS[key], "foreign provider/version: " + key)
    require(header.get("provider_ok") is True and header.get("sink_constraint_basis") == BASIS,
            "provider or pinned-source NV12 constraint check failed")
    cap = integer(header.get("capacity"), "capacity", 1, 16384)
    snapcap = integer(header.get("snapshot_capacity"), "snapshot capacity", 1, 16)
    integer(header.get("nv12_format"), "NV12 enum", 0, 255)
    for name, maximum in (("max_width", 1920), ("max_height", 1088)):
        value = integer(header.get(name), name, 2, maximum)
        require(not value % 2, "odd snapshot bound")
    events = rows[1:-1]
    require(len(events) <= cap, "event capacity exceeded")
    for key in ("events", "attempted_events", "snapshots", "dropped", "snapshot_failures",
                "lifecycle_failures", "string_truncations", "inflight", "unpublished", "open_graphs"):
        integer(footer.get(key), "footer." + key)
    require(footer["events"] == len(events) == footer["attempted_events"] and
            footer["snapshots"] <= snapcap, "footer counts disagree")
    require(all(footer[k] == 0 for k in ("dropped", "snapshot_failures", "lifecycle_failures",
                "string_truncations", "inflight", "unpublished", "open_graphs")) and
            all(footer.get(k) is False for k in ("clock_failure", "fork_seen")) and
            footer.get("complete") is True, "incomplete/forked/overflowed observation")
    require(type(snapshot_blobs) is dict, "snapshot blobs must be a mapping")
    path = Path(report_path or ("postfilter-" + str(expected_pid) + ".jsonl")).absolute()
    require(path.suffix == ".jsonl" and path.stem.endswith("-" + str(expected_pid)),
            "report filename must retain its original PID binding")
    graphs, snapshots, thread_end = {}, {}, {}
    for seq, e in enumerate(events):
        require(e.get("type") == "event" and type(e.get("seq")) is int and e["seq"] == seq,
                "missing, duplicate or reordered call sequence")
        require(type(e.get("pid")) is int and e["pid"] == expected_pid, "foreign event PID")
        tid = integer(e.get("tid"), "TID", 1, (1 << 31)-1)
        begin = integer(e.get("begin_ns"), "call begin", 1)
        end = integer(e.get("end_ns"), "call end", begin)
        require(begin >= thread_end.get(tid, 0), "same-thread calls overlap or regress")
        thread_end[tid] = end
        api = e.get("api")
        require(api in APIS, "unknown observed API")
        gid = integer(e.get("graph_id"), "graph lifetime ID", 1, cap)
        address = pointer(e.get("graph"), "graph", True)
        pointer(e.get("context"), "context")
        signed(e.get("errno_before"), "errno_before")
        signed(e.get("errno_after"), "errno_after")
        signed(e.get("flags"), "flags")
        signed(e.get("close_pts"), "close_pts", 64)
        require(type(e.get("input_null")) is bool, "input_null must be boolean")
        integer(e.get("input_index"), "input index", 0, cap)
        integer(e.get("output_index"), "output index", 0, cap)
        text(e.get("name"), "name", 128)
        text(e.get("args"), "args", 1024)
        require((api == FREE and e.get("ret") is None) or
                (api != FREE and type(e.get("ret")) is int), "invalid API result")
        if api != FREE:
            signed(e["ret"], "API result")
        require(api.endswith("_flags") or api == CLOSE or e["flags"] == 0,
                "plain API has invented flags")
        require(api in ADD or e["input_null"] is False, "nonnull API has input_null marker")
        require(api == CLOSE or e["close_pts"] == 0, "close_pts belongs only to source close")
        if gid not in graphs:
            require(api in (CREATE, PARSE), "graph lifetime has no observed creation")
            graphs[gid] = dict(graph_id=gid, graph=e["graph"], address=address, events=[])
        g = graphs[gid]
        require(address == g["address"] and not any(x["api"] == FREE for x in g["events"]),
                "changed graph identity or call after graph free")
        g["events"].append(e)
        has_input = api in ADD and not e["input_null"]
        has_output = api in GET and e["ret"] >= 0
        require((e["input_index"] > 0) == has_input and
                (e["output_index"] > 0) == has_output, "attempt/return index belongs to wrong call")
        require((e.get("frame") is not None) == (has_input or has_output),
                "frame ownership does not match original call result")
        if has_input or has_output:
            frame = frame_value(e["frame"], header)
            # The probe emits zero sink rationals on source captures as well;
            # only successful GET rationals are measured sink properties.
            if has_output:
                rational(e.get("sink_time_base"), "sink.time_base")
                rational(e.get("sink_frame_rate"), "sink.frame_rate")
            snap = frame["snapshot"]
            if snap is not None:
                require(type(snap) is dict, "snapshot must be an object")
                slot = integer(snap.get("slot"), "snapshot slot", 0, snapcap-1)
                name = P.filename(snap.get("file"))
                require(name == path.stem + "-s" + format(slot, "02d") + ".nv12",
                        "snapshot filename lost report/PID/slot binding")
                require(slot not in snapshots, "duplicate snapshot slot")
                size = frame["width"] * frame["height"] * 3 // 2
                require(type(snap.get("bytes")) is int and snap["bytes"] == size and
                        name in snapshot_blobs and type(snapshot_blobs[name]) is bytes and
                        len(snapshot_blobs[name]) == size, "missing/truncated/oversized active NV12 snapshot")
                snapshots[slot] = name
    require(set(graphs) == set(range(1, len(graphs)+1)), "graph lifetime IDs have gaps")
    require(set(snapshots) == set(range(footer["snapshots"])) and
            set(snapshot_blobs) == set(snapshots.values()), "snapshot publication ledger differs")
    summaries = []
    for gid in sorted(graphs):
        g = graphs[gid]
        calls = g["events"]
        frees = [e for e in calls if e["api"] == FREE]
        require(len(frees) == 1 and frees[0] is calls[-1], "graph lifetime lacks one terminal free")
        free = frees[0]
        require(all(e["end_ns"] <= free["begin_ns"] for e in calls[:-1]),
                "graph teardown overlaps a live call")
        inputs = sorted([e for e in calls if e["input_index"]], key=lambda e: e["input_index"])
        outputs = sorted([e for e in calls if e["output_index"]], key=lambda e: e["output_index"])
        require([e["input_index"] for e in inputs] == list(range(1, len(inputs)+1)) and
                [e["output_index"] for e in outputs] == list(range(1, len(outputs)+1)),
                "missing/duplicate source attempt or sink success index")
        sources = [e for e in calls if e["api"] == CREATE and e["ret"] >= 0 and e["name"] == "buffer"]
        source_contexts = {pointer(e["context"], "buffer context", True): e for e in sources}
        for e in calls:
            if e["api"] in ADD | {CLOSE}:
                ctx = pointer(e["context"], "source context", True)
                require(ctx in source_contexts and source_contexts[ctx]["end_ns"] <= e["begin_ns"],
                        "source call has no completed matching buffer creation")
            if e["api"] in GET:
                require(pointer(e["context"], "sink context", True) not in source_contexts,
                        "source context used as sink context")
        for is_input, frames in ((True, inputs), (False, outputs)):
            selected = [e for e in frames if e["frame"]["format"] == header["nv12_format"]]
            quota = 3 if is_input else 4
            for i, e in enumerate(selected):
                require((e["frame"]["snapshot"] is not None) == (i < quota),
                        "snapshot selection differs from first NV12 attempts/returns")
        parsed = [e for e in calls if e["api"] == PARSE and e["ret"] >= 0 and
                  re.fullmatch(r"bwdif(?:=[^,;\[\]]+)?", e["args"])]
        accepted = [e for e in inputs if e["ret"] >= 0]
        pixel_active = any(e["frame"]["format"] == header["nv12_format"] for e in accepted + outputs)
        if pixel_active:
            require(len(sources) == 1 and bool(sources[0]["args"]) and len(parsed) == 1,
                    "active NV12 graph needs exact buffer arguments and successful BWDIF parse")
            first_frame = min(e["begin_ns"] for e in accepted + outputs)
            require(parsed[0]["end_ns"] <= first_frame, "frames precede successful BWDIF graph parse")
        # Non-NV12 graphs keep their scalar ledger; they cannot form a pixel reference.
        source_eof = [e for e in calls if (e["api"] == CLOSE or
                      (e["api"] in ADD and e["input_null"])) and e["ret"] >= 0]
        require(not any(e["ret"] >= 0 and close["end_ns"] <= e["begin_ns"] for e in inputs
                        for close in source_eof), "accepted input follows successful original source EOF")
        sink_eof = [e for e in calls if e["api"] in GET and e["ret"] == EOF]
        findings = []
        if not source_eof and (accepted or outputs):
            findings.append("NO_SUCCESSFUL_ORIGINAL_SOURCE_NULL_OR_CLOSE_OBSERVED")
        if not sink_eof and (accepted or outputs):
            findings.append("NO_ORIGINAL_SINK_EOF_OBSERVED")
        if sink_eof and not source_eof:
            findings.append("SINK_EOF_WITHOUT_OBSERVED_SOURCE_EOF")
        positive = [e for e in accepted if e["frame"] is not None]
        g.update(buffer_args=sources[0]["args"] if len(sources) == 1 else None,
                 filter_args=parsed[0]["args"] if len(parsed) == 1 else None,
                 selected_inputs=[e for e in inputs if e["frame"]["snapshot"] is not None],
                 selected_outputs=[e for e in outputs if e["frame"]["snapshot"] is not None],
                 counts=dict(source_attempts=len(inputs), source_accepted=len(accepted),
                             source_failures=len(inputs)-len(accepted), sink_successes=len(outputs),
                             sink_eagain=sum(e["api"] in GET and e["ret"] == EAGAIN for e in calls),
                             sink_eof=len(sink_eof), source_null_attempts=sum(e["api"] in ADD and e["input_null"] for e in calls),
                             source_close_attempts=sum(e["api"] == CLOSE for e in calls),
                             source_eof_successes=len(source_eof)),
                 positive_input_interval_ns=([min(e["begin_ns"] for e in positive),
                                              max(e["end_ns"] for e in positive)] if positive else None),
                 free_interval_ns=[free["begin_ns"], free["end_ns"]], findings=findings,
                 scope="BWDIF_NV12" if pixel_active else "EMPTY_OR_OTHER_FORMAT_SCALAR_ONLY")
        summaries.append(g)
    for i, g in enumerate(summaries):
        for other in summaries[i+1:]:
            lifetimes_disjoint = (g["free_interval_ns"][1] <= other["events"][0]["begin_ns"] or
                                  other["free_interval_ns"][1] <= g["events"][0]["begin_ns"])
            if g["address"] == other["address"]:
                require(lifetimes_disjoint, "graph address reused before lifetime retirement")
            contexts = {pointer(e["context"], "context") for e in g["events"]} - {0}
            other_contexts = {pointer(e["context"], "context") for e in other["events"]} - {0}
            require(lifetimes_disjoint or not contexts.intersection(other_contexts),
                    "foreign context shared by overlapping graph lifetimes")
    return dict(header=header, footer=footer, events=events, graphs=summaries,
                report_path=str(path), providers=expected, source_clock_comparison=False)


def read_report(report_path, expected_pid, expected_providers=None):
    path = Path(report_path).absolute()
    rows = parse_report(read_regular(path, MAX_REPORT_BYTES))
    blobs = {}
    for e in rows[1:-1]:
        frame = e.get("frame") if type(e) is dict else None
        snap = frame.get("snapshot") if type(frame) is dict else None
        if snap is not None:
            require(type(snap) is dict, "snapshot must be an object")
            name = P.filename(snap.get("file"))
            require(name not in blobs, "duplicate snapshot filename")
            require(len(blobs) < 16, "snapshot file quota exceeded")
            blobs[name] = read_regular(path.parent / name, MAX_SNAPSHOT_BYTES)
    return check_document(rows, blobs, expected_pid, expected_providers, path)


def graph_value(validated, graph_id):
    integer(graph_id, "selected graph lifetime ID", 1)
    matches = [g for g in validated["graphs"] if g["graph_id"] == graph_id]
    require(len(matches) == 1, "selected graph lifetime is absent")
    return matches[0]


def reference_prefix(validated, graph_id):
    """Return exact original unflushed 3-input/4-output replay, or reject scope.

    A failed/foreign/unsnapshotted input or a fourth input before output4 cannot
    be replaced with an invented clean prefix. No EOS is injected.
    """
    g = graph_value(validated, graph_id)
    require(g["scope"] == "BWDIF_NV12" and g["buffer_args"] and g["filter_args"],
            "selected graph is not an observed BWDIF NV12 graph")
    outputs = [e for e in g["events"] if e["api"] in GET and e["ret"] >= 0]
    require(len(outputs) >= 4, "four observed sink outputs are required for prefix replay")
    stop = outputs[3]["seq"]
    schedule_events = [e for e in g["events"] if e["seq"] <= stop and e["api"] in ADD | GET | {CLOSE}]
    require(all(e["api"] != CLOSE and not e["input_null"] for e in schedule_events),
            "original EOF/close occurs inside selected unflushed prefix")
    require(all(a["end_ns"] <= b["begin_ns"] for a, b in zip(schedule_events, schedule_events[1:])),
            "overlapping original calls cannot supply a serial reference schedule")
    inputs = [e for e in schedule_events if e["api"] in ADD]
    require(len(inputs) == 3 and [e["input_index"] for e in inputs] == [1, 2, 3] and
            all(e["ret"] >= 0 for e in inputs), "prefix does not have exactly three accepted original inputs")
    require(all(e["ret"] >= 0 or e["ret"] == EAGAIN for e in schedule_events if e["api"] in GET),
            "prefix contains original terminal/error pulls")
    require(len({e["context"] for e in inputs}) == 1 and
            len({e["context"] for e in schedule_events if e["api"] in GET}) == 1,
            "prefix spans multiple source/sink contexts")
    directory = Path(validated["report_path"]).parent
    def captured(e):
        frame = copy.deepcopy(e["frame"])
        require(frame["format"] == validated["header"]["nv12_format"] and
                frame["snapshot"] is not None, "selected prefix frame lacks active NV12 snapshot")
        frame["path"] = str(directory / frame["snapshot"]["file"])
        if e["api"] in GET:
            frame.update(sink_time_base=e["sink_time_base"], sink_frame_rate=e["sink_frame_rate"])
        return frame
    return dict(schema=1, mode="prefix", graph=dict(buffer_args=g["buffer_args"],
                filter_name="bwdif", filter_args=g["filter_args"], constraint_basis=BASIS),
                inputs=[captured(e) for e in inputs], expected_outputs=[captured(e) for e in outputs[:4]],
                schedule=[{k: e[k] for k in ("api", "input_index", "output_index", "flags", "ret")}
                          for e in schedule_events], providers=dict(validated["providers"]),
                nv12_format=validated["header"]["nv12_format"])


def join_reference(validated, graph_id, reference_result):
    """Independently recheck same-input prefix output bytes and raw metadata."""
    manifest = reference_prefix(validated, graph_id)
    ref = reference_result
    if isinstance(ref, (str, Path)):
        ref = json.loads(read_regular(ref, MAX_REPORT_BYTES), object_pairs_hook=strict_object)
    require(type(ref) is dict and ref.get("schema") == 1 and ref.get("status") == "PASS" and
            ref.get("mode") == "prefix" and ref.get("manifest") == manifest,
            "reference is not the exact validated unflushed prefix")
    require(ref.get("providers") == manifest["providers"] and ref.get("constraint_basis") == BASIS and
            ref.get("source_clock_comparison") is False and ref.get("original_client_eof_claim") is False and
            ref.get("source_eof_injected") is False and
            ref.get("comparison_scope") == "SAME_INPUT_PREFIX_ACTIVE_NV12_AND_CAPTURED_METADATA",
            "reference provider/clock/EOF scope changed")
    require(type(ref.get("outputs")) is list and len(ref["outputs"]) == 4,
            "reference requires exactly four unflushed prefix outputs")
    comparisons = []
    for index, (actual, wanted) in enumerate(zip(ref["outputs"], manifest["expected_outputs"]), 1):
        require(type(actual) is dict and actual.get("reference_only_eof") is False,
                "reference output came from injected EOF")
        for key in FRAME_KEYS + ("sink_time_base", "sink_frame_rate"):
            require(actual.get(key) == wanted[key], "reference raw property differs: " + key)
        path = actual.get("path")
        require(type(path) is str and Path(path).is_absolute() and
                Path(path).name == "output-" + format(index, "02d") + ".nv12" and
                Path(path) != Path(wanted["path"]),
                "reference output must name its distinct indexed output file")
        size = wanted["width"] * wanted["height"] * 3 // 2
        left, right = read_regular(path, size), read_regular(wanted["path"], size)
        require(len(left) == len(right) == size and left == right,
                "reference active NV12 bytes differ at output " + str(index))
        comparisons.append(dict(output_index=index, active_bytes=size, active_pixels_equal=True,
                                captured_metadata_equal=True))
    return dict(status="PASS_SAME_INPUT_PREFIX", graph_id=graph_id, comparisons=comparisons,
                source_clock_comparison=False, original_client_eof_claim=False,
                comparison_scope="SAME_INPUT_PREFIX_ACTIVE_NV12_AND_CAPTURED_METADATA", limits=LIMITS)


def audit_report(report_path, expected_pid, expected_providers=None, reference=None, graph_id=None):
    """JSON-compatible observation summary; missing actual EOF is a finding."""
    try:
        valid = read_report(report_path, expected_pid, expected_providers)
        summaries = []
        for g in valid["graphs"]:
            summary = {k: g[k] for k in ("graph_id", "graph", "scope", "counts", "buffer_args",
                       "filter_args", "positive_input_interval_ns", "free_interval_ns", "findings")}
            try:
                reference_prefix(valid, g["graph_id"])
                summary["prefix_available"] = True
            except ValueError as exc:
                summary.update(prefix_available=False, prefix_unavailable_reason=str(exc))
            summaries.append(summary)
        result = dict(status="PASS_SCOPED_POSTFILTER_OBSERVATION", report_path=valid["report_path"],
                      header=valid["header"], graphs=summaries, source_clock_comparison=False,
                      original_client_eof_claim=False, limits=LIMITS)
        if reference is not None:
            result["reference"] = join_reference(valid, graph_id, reference)
        return result
    except (ValueError, OSError, KeyError, TypeError, OverflowError, UnicodeError) as exc:
        return dict(status="FAIL", reason=str(exc), limits=LIMITS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report")
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--filter-provider", default=PROVIDERS["filter"])
    parser.add_argument("--util-provider", default=PROVIDERS["util"])
    parser.add_argument("--graph-id", type=int)
    parser.add_argument("--reference")
    args = parser.parse_args()
    result = audit_report(args.report, args.pid, dict(filter=args.filter_provider, util=args.util_provider),
                          args.reference, args.graph_id)
    print(json.dumps(result, sort_keys=True))
    return 0 if result["status"] != "FAIL" else 1


if __name__ == "__main__":
    sys.exit(main())
