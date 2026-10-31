#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Compile only our collector plus software GStreamer controls, never DIL.
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-stage-trace.XXXXXX")
cleanup()
{
    rm -f -- "$test_dir/check" "$test_dir/registry.bin" \
        "$test_dir/ordinary.log" "$test_dir/ordinary.stderr" \
        "$test_dir/sanitized.log" "$test_dir/sanitized.stderr"
    rmdir -- "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
packages="gstreamer-check-1.0 gstreamer-video-1.0"

for variant in ordinary sanitized; do
    extra=
    if test "$variant" = sanitized; then
        extra='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie'
    fi
    ${CC:-cc} ${CFLAGS:-} -std=c11 -O1 -g -Wall -Wextra -Werror $extra \
        $(pkg-config --cflags $packages) "$repo_dir/tests/gst-stage-trace.c" \
        -o "$test_dir/check" $(pkg-config --libs $packages)
    status=0
    GST_PLUGIN_PATH= GST_PLUGIN_PATH_1_0= GST_REGISTRY="$test_dir/registry.bin" \
        GST_REGISTRY_1_0="$test_dir/registry.bin" \
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        timeout 60 "$test_dir/check" >"$test_dir/$variant.log" \
        2>"$test_dir/$variant.stderr" || status=$?
    if test "$status" -ne 0; then
        python3 - "$test_dir/$variant.log" "$test_dir/$variant.stderr" <<'PY'
import pathlib
import sys
lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
lines = [line[2:] if line.startswith("# ") else line for line in lines]
for line in [line for line in lines if not line.startswith("TRACE_CASE ")][-35:]:
    print(line[:2000])
print(pathlib.Path(sys.argv[2]).read_text()[-8000:], file=sys.stderr)
PY
        exit "$status"
    fi
    python3 -B - "$variant" "$test_dir/$variant.log" "$test_dir/$variant.stderr" \
        "$repo_dir/tests/stage-pts-audit.py" <<'PY'
import importlib.util
import json
from pathlib import Path
import sys

variant, log, errors, audit_path = sys.argv[1:]
audit_spec = importlib.util.spec_from_file_location("stage_pts_audit", audit_path)
auditor = importlib.util.module_from_spec(audit_spec)
audit_spec.loader.exec_module(auditor)
cases = {}
expected = None
for line in Path(log).read_text().splitlines():
    # GLib's TAP printer prefixes application g_print output with a comment.
    if line.startswith("# "):
        line = line[2:]
    if line.startswith("TRACE_CASE "):
        _, name, encoded = line.split(" ", 2)
        assert name not in cases, name
        cases[name] = json.loads(encoded)
    elif line.startswith("TRACE_EXPECT "):
        assert expected is None
        expected = json.loads(line[len("TRACE_EXPECT "):])
assert expected is not None
required = {"metadata", "flush", "capacity", "controls", "detach",
            "detached_snapshot", "buffer_list", "custom_buffer_list", "truncated_buffer_list", "attachment_limit", "concurrent_stop", "pipeline_tff",
            "pipeline_bff", "pipeline_tff_drop", "pipeline_bff_drop",
            "output_stages", "output_stages_drop", "window_tail", "window_epoch",
            "window_flush", "window_post_incomplete", "window_tiny", "window_unknown",
            "window_stream_unknown", "window_output", "window_output_drop", "first_output_drop",
            "window_buffer_list"}
summary_clean = {"summary_first_25", "summary_first_50", "window_summary_25",
                 "window_summary_50", "window_summary_frozen_25", "window_summary_frozen_50"}
summary_defects = {"gap", "duplicate", "reverse", "off_grid", "unknown_pts", "unknown_duration",
                   "duration", "changed_caps", "unknown_caps", "vfr_caps", "segment", "rate",
                   "applied_rate", "bytes_segment", "stream", "flush", "seek", "buffer_gap", "discont"}
required |= summary_clean | {"window_summary_" + name for name in summary_defects}
required |= {"window_summary_frozen_caps", "window_summary_overflow", "window_summary_saturation",
             "window_summary_initial_unknown_caps", "window_summary_initial_vfr",
             "window_summary_initial_rate", "window_summary_initial_applied_rate",
             "window_summary_initial_bytes", "window_summary_negative_running_time", "window_summary_initial_vfr_max",
             "summary_missing_eos", "summary_empty"}
assert set(cases) == required, (set(cases), required)
assert not Path(errors).read_text().strip(), Path(errors).read_text()[:8000]

def buffers(case, stage=None, token=None):
    return [record for record in case["records"] if record["kind"] == "buffer"
            and (stage is None or record["stage"] == stage)
            and (token is None or record["token"] == token)]

def attachment(case, stage, token=None):
    found = [item for item in case["attachments"] if item["stage"] == stage
             and (token is None or item["token"] == token)]
    assert len(found) == 1, (stage, token, found)
    return found[0]

for name, case in cases.items():
    assert case["schema"] == 1 and case["closed"] is True, name
    assert len(case["gstreamer_version"]) == 4 and case["gstreamer_version"][0] == 1
    assert all(type(item) is int for item in case["gstreamer_version"])
    assert case["retained"] == len(case["records"]), name
    assert case["retained"] <= case["capacity"], name
    assert case["dropped"] == case["observed"] - case["retained"], name
    order = [item["order"] for item in case["records"]]
    assert order == sorted(set(order)), (name, order)
    owners = {item["id"]: item for item in case["attachments"]}
    assert len(owners) == len(case["attachments"]), name
    assert all(item["active"] is False for item in owners.values()), name
    for owner in owners.values():
        summary = owner["pts_summary"]
        assert summary["version"] == 1 and summary["scope"] == "whole-attachment-observation", name
        assert summary["buffers"] == owner["buffers"], name
        if not summary["counters_saturated"]:
            assert summary["covered"] + summary["unqualified_buffers"] == owner["buffers"], name
        assert summary["unknown_pts"] == owner["missing_pts"], name
        if summary["first_violation"] is not None:
            first = summary["first_violation"]
            assert set(first) == {"reason", "order", "ordinal", "pts", "expected_pts"}, name
            assert first["reason"] and 0 < first["order"] <= case["observed"], name
            assert 0 <= first["ordinal"] <= owner["buffers"], name
        if summary["qualified"]:
            assert summary["covered"] == owner["buffers"] > 0 and summary["unqualified_buffers"] == 0, name
            assert type(summary["first_pts"]) is int and type(summary["last_pts"]) is int, name
            assert summary["period_ns"] in (20_000_000, 40_000_000), name
            assert summary["first_violation"] is None and not summary["arithmetic_overflow"], name
            assert not summary["counters_saturated"] and not case["counters_saturated"], name
            assert summary["epochs"] == summary["segment_events"] == owner["stream_generation"] == 1, name
            assert owner["eos_count"] == 1 and owner["buffer_lists"] == 0, name
            for field in ("unknown_pts", "unknown_duration", "invalid_duration", "duplicate_events",
                          "reverse_events", "off_grid_events", "gap_events", "gap_steps", "caps_changes",
                          "flush_events", "seek_requests"):
                assert summary[field] == 0, (name, field, summary)
    assert case["retention"] == ("window" if name.startswith("window_") else "first"), name
    if case["retention"] == "window":
        window = case["window"]
        assert window["post_target"] == max(1, case["capacity"] // 4)
        assert 0 <= window["post_retained"] <= window["post_target"]
        assert window["complete"] == (window["triggered"] and window["post_retained"] == window["post_target"])
        assert case["dropped"] == window["overwritten"] + window["unretained_after_window"]
        assert window["context_before_order"] == case["records"][0]["order"]
        assert len(case["window_context"]) == len(owners)
        for context in case["window_context"]:
            owner = owners[context["attachment"]]
            assert (context["stage"], context["token"]) == (owner["stage"], owner["token"])
            assert context["through_order"] < window["context_before_order"]
            for field in ("caps", "segment"):
                prior = context[field]
                if prior is not None:
                    assert prior["order"] <= context["through_order"]
                    assert prior["order"] < window["context_before_order"]
                    assert prior["thread_id"] == (expected["linux_tid"] or None)
    else:
        assert "window" not in case and "window_context" not in case
    for item in case["records"]:
        assert type(item["monotonic_ns"]) is int and item["monotonic_ns"] >= 0
        if case["retention"] == "window":
            assert item["thread_id"] == (expected["linux_tid"] or None)
            if item["thread_id"] is not None:
                assert type(item["thread_id"]) is int and item["thread_id"] > 0
        else:
            assert "thread_id" not in item
        if item["attachment"] is not None:
            owner = owners[item["attachment"]]
            assert item["stage"] == owner["stage"] and item["token"] == owner["token"]
        if item["kind"] in ("buffer", "buffer-list-item"):
            assert type(item["flags"]) is int and item["flags"] >= 0
            for field in ("pts", "dts", "duration", "running_time"):
                assert item[field] is None or type(item[field]) is int
            for field in ("stream_generation", "segment_generation", "flush_generation"):
                assert type(item[field]) is int and item[field] >= 0

case = cases["metadata"]
assert case["dropped"] == 0
for stage, token in (("decoder", 11), ("sink", 22)):
    rows = buffers(case, stage)
    owner = attachment(case, stage, token)
    assert owner["buffers"] == 3 and owner["missing_pts"] == 1
    assert [row["ordinal"] for row in rows] == [1, 2, 3]
    assert [row["pts"] for row in rows] == [0, None, 1_250_000_000]
    assert [row["dts"] for row in rows] == [None, 0, 1_240_000_000]
    assert [row["duration"] for row in rows] == [40_000_000, None, 20_000_000]
    assert [row["running_time"] for row in rows] == [0, None, 3_250_000_000]
    assert rows[0]["flags"] == expected["flags"]
    assert rows[0]["segment_generation"] < rows[2]["segment_generation"]
    assert rows[0]["stream_generation"] == rows[2]["stream_generation"] == 1
    events = [row for row in case["records"] if row.get("stage") == stage]
    assert sum(row["kind"] == "eos" for row in events) == 1
    assert sum(row["kind"] == "qos" for row in events) == 1

case = cases["flush"]
rows = buffers(case, "decoder")
assert [row["pts"] for row in rows] == [0, 40_000_000, 1_250_000_000, 42_000_000]
assert [row["running_time"] for row in rows] == [0, None, 9_250_000_000, None]
assert [row["flush_generation"] for row in rows] == [0, 1, 1, 1]
assert rows[0]["segment_generation"] == rows[1]["segment_generation"]
assert rows[2]["segment_generation"] > rows[1]["segment_generation"]
assert rows[3]["segment_generation"] > rows[2]["segment_generation"]
assert len([row for row in case["records"] if row["kind"] == "flush-start"]) == 1
assert len([row for row in case["records"] if row["kind"] == "flush-stop"]) == 1

case = cases["capacity"]
assert case["capacity"] == case["retained"] == 3
assert case["observed"] > 256 and case["dropped"] > 253
assert case["configuration_rejections"] >= 1
assert attachment(case, "decoder")["buffers"] == 256
assert attachment(case, "decoder")["missing_pts"] == 0

case = cases["controls"]
controls = [row for row in case["records"] if row["kind"] == "control"]
assert [row["control"] for row in controls] == ["pause", "resume", "seek-request", "seek-failed"]
assert [row["value"] for row in controls] == [0, 1, 2, 3]
rows = buffers(case)
for field in ("stream_generation", "segment_generation", "flush_generation"):
    assert rows[0][field] == rows[1][field], field

case = cases["detach"]
assert attachment(case, "decoder", 51)["buffers"] == 1
assert attachment(case, "decoder", 52)["buffers"] == 1
assert [row["pts"] for row in buffers(case)] == [0, 80_000_000]
assert case == cases["detached_snapshot"]
assert attachment(cases["concurrent_stop"], "decoder")["buffers"] == 1

for name in ("buffer_list", "custom_buffer_list", "truncated_buffer_list"):
    case = cases[name]
    assert case["buffer_list_ambiguity"] is True
    truncated = name == "truncated_buffer_list"
    if truncated:
        assert case["capacity"] == case["retained"] == 3
        assert case["dropped"] > 0
    for stage in ("decoder", "sink"):
        owner = attachment(case, stage)
        assert not owner["pts_summary"]["qualified"]
        assert owner["pts_summary"]["first_violation"]["reason"] == "buffer-list"
        individual = 3 if name != "custom_buffer_list" and stage == "sink" else 0
        assert owner["buffers"] == individual
        assert owner["buffer_lists"] == 1 and owner["list_items"] == 3
        assert owner["buffer_list_ambiguity"] is True
        assert owner["list_missing_pts"] == owner["missing_pts"] == 0
        retained = 0 if truncated else individual
        assert [row["pts"] for row in buffers(case, stage)] == [0] * retained
        assert [row["ordinal"] for row in buffers(case, stage)] == list(range(1, retained + 1))
        listed = [row for row in case["records"] if row["kind"] == "buffer-list-item" and row["stage"] == stage]
        assert len(listed) == (0 if truncated else 3)
        assert [row["pts"] for row in listed] == ([] if truncated else [0, 0, 0])
        assert [row["list_ordinal"] for row in listed] == ([] if truncated else [1, 1, 1])
        assert [row["list_index"] for row in listed] == ([] if truncated else [0, 1, 2])
case = cases["attachment_limit"]
assert case["attachment_limit_reached"] is True
assert len(case["attachments"]) == 64
assert case["rejected_attachments"] == 1
assert case["capacity"] == case["retained"] == 4 and case["dropped"] > 0

for parity in ("tff", "bff"):
    for dropped in (False, True):
        case = cases["pipeline_" + parity + ("_drop" if dropped else "")]
        assert case["dropped"] == 0
        assert case["buffer_list_ambiguity"] is False
        assert attachment(case, "decoder")["buffers"] == 50
        assert attachment(case, "deinterlace")["buffers"] == 100
        assert attachment(case, "sink")["buffers"] == (90 if dropped else 100)
        source = buffers(case, "decoder")
        fields = buffers(case, "deinterlace")
        received = buffers(case, "sink")
        assert [row["pts"] for row in source] == [i * 40_000_000 for i in range(50)]
        assert [row["pts"] for row in fields] == [i * 20_000_000 for i in range(100)]
        assert [row["duration"] for row in fields] == [20_000_000] * 100
        wanted = [i * 20_000_000 for i in range(100) if not dropped or (i + 1) % 10]
        assert [row["pts"] for row in received] == wanted
        for stage in ("decoder", "deinterlace", "sink"):
            stage_rows = buffers(case, stage)
            assert attachment(case, stage)["buffer_lists"] == attachment(case, stage)["list_items"] == 0
            assert attachment(case, stage)["buffer_list_ambiguity"] is False
            assert [row["ordinal"] for row in stage_rows] == list(range(1, len(stage_rows) + 1))
            assert any(row["kind"] == "eos" and row["stage"] == stage for row in case["records"])
            caps = [row for row in case["records"] if row["kind"] == "caps" and row["stage"] == stage]
            assert caps, stage
            for row in caps:
                assert row["raw_video"] is True and row["video_info_valid"] is True
                assert (row["width"], row["height"]) == (16, 8)
                assert (row["fps_n"], row["fps_d"]) == (25 if stage == "decoder" else 50, 1)
                assert row["interlace_mode"] == expected["mixed_mode" if stage == "decoder" else "progressive_mode"]

for dropped in (False, True):
    case = cases["output_stages" + ("_drop" if dropped else "")]
    assert case["dropped"] == 0 and case["buffer_list_ambiguity"] is False
    assert case["rejected_attachments"] == 2
    assert [item["stage"] for item in case["attachments"]] == ["output-pack", "output-scale", "sink"]
    for stage, count, size in (("output-pack", 50, (16, 8)),
                               ("output-scale", 45 if dropped else 50, (32, 16)),
                               ("sink", 45 if dropped else 50, (32, 16))):
        owner = attachment(case, stage)
        rows = buffers(case, stage)
        assert owner["buffers"] == count and len(rows) == count
        assert owner["eos_count"] == 1
        assert owner["buffer_lists"] == owner["list_items"] == 0
        wanted = [i * 20_000_000 for i in range(50)
                  if stage == "output-pack" or not dropped or (i + 1) % 10]
        assert [row["pts"] for row in rows] == wanted
        assert [row["running_time"] for row in rows] == wanted
        assert [row["duration"] for row in rows] == [20_000_000] * count
        assert [row["ordinal"] for row in rows] == list(range(1, count + 1))
        caps = [row for row in case["records"] if row["kind"] == "caps" and row["stage"] == stage]
        assert caps, stage
        for row in caps:
            assert row["raw_video"] is True and row["video_info_valid"] is True
            assert (row["width"], row["height"]) == size
            assert (row["fps_n"], row["fps_d"]) == (50, 1)
            assert row["interlace_mode"] == expected["progressive_mode"]

tail = cases["window_tail"]
assert tail["window"]["triggered"] is False and tail["window"]["complete"] is False
assert tail["window"]["trigger_order"] is None and tail["window"]["trigger_seqnum"] is None
assert tail["window"]["post_retained"] == 0
assert tail["configuration_rejections"] == 1
assert tail["retained"] == 8 and tail["window"]["overwritten"] > 0
assert attachment(tail, "decoder")["buffers"] == 20
assert [r["pts"] for r in buffers(tail)] == [n * 40_000_000 for n in range(14, 20)]
context = tail["window_context"][0]
assert context["seen"] and context["active"] and context["stream_known"]
assert context["segment_valid"] and not context["flushing"]
assert (context["stream_generation"], context["segment_generation"], context["flush_generation"]) == (1, 1, 0)
assert context["caps"]["format"] == "YUY2" and context["caps"]["interlace_mode"] == expected["mixed_mode"]
assert context["segment"]["base"] == context["segment"]["start"] == 0

epoch = cases["window_epoch"]
context = epoch["window_context"][0]
assert context["stream_generation"] == context["segment_generation"] == 2
assert context["caps"]["format"] == "UYVY" and context["caps"]["interlace_mode"] == expected["progressive_mode"]
assert context["segment"]["base"] == 5_000_000_000
rows = buffers(epoch)
assert [r["pts"] for r in rows] == [n * 20_000_000 for n in range(14, 20)]
assert [r["running_time"] for r in rows] == [5_000_000_000 + n * 20_000_000 for n in range(14, 20)]
assert all(r["stream_generation"] == r["segment_generation"] == 2 for r in rows)

flushed = cases["window_flush"]
context = flushed["window_context"][0]
assert context["flush_generation"] == 1 and not context["flushing"]
assert context["segment"] is not None and context["segment_valid"] is False
assert context["caps"]["format"] == "YUY2"
assert attachment(flushed, "decoder")["buffers"] == 11

incomplete = cases["window_post_incomplete"]
assert incomplete["window"]["triggered"] and not incomplete["window"]["complete"]
assert incomplete["window"]["post_target"] == 32 and incomplete["window"]["post_retained"] == 3
assert incomplete["window"]["trigger_seqnum"] == 0xABCD
assert sum(r["kind"] == "trigger" for r in incomplete["records"]) == 1

tiny = cases["window_tiny"]
assert tiny["capacity"] == tiny["retained"] == tiny["window"]["post_retained"] == 1
assert tiny["window"]["complete"] and tiny["window"]["unretained_after_window"] == 5
assert [r["pts"] for r in buffers(tiny)] == [40_000_000]
context = tiny["window_context"][0]
assert context["caps"]["format"] == "YUY2" and context["caps"]["interlace_mode"] == expected["mixed_mode"]
assert context["segment"]["base"] == 0 and context["segment_generation"] == 1
assert attachment(tiny, "decoder")["segment_generation"] == 2
assert attachment(tiny, "decoder")["buffers"] == 3

for name, stream_known, stream_generation in (("window_unknown", False, 0), ("window_stream_unknown", True, 2)):
    context = cases[name]["window_context"][0]
    assert context["seen"] and context["active"]
    assert context["stream_known"] == stream_known and context["stream_generation"] == stream_generation
    assert context["caps"] is context["segment"] is None and context["segment_valid"] is False

for name, dropped in (("window_output", False), ("window_output_drop", True), ("first_output_drop", True)):
    case = cases[name]
    assert attachment(case, "output-pack")["buffers"] == 200
    assert attachment(case, "output-scale")["buffers"] == attachment(case, "sink")["buffers"] == (199 if dropped else 200)
    assert all(a["eos_count"] == 1 for a in case["attachments"])
    assert case["retained"] == case["capacity"] == 64 and case["truncated"]
    for stage in ("output-pack", "output-scale", "sink"):
        summary = attachment(case, stage)["pts_summary"]
        affected = dropped and stage != "output-pack"
        assert summary["qualified"] == (not affected), (name, stage, summary)
        assert summary["buffers"] == summary["covered"] == (199 if affected else 200)
        assert (summary["first_pts"], summary["last_pts"], summary["period_ns"]) == (0, 3_980_000_000, 20_000_000)
        assert summary["gap_events"] == summary["gap_steps"] == int(affected)
        if affected:
            first = summary["first_violation"]
            assert first["reason"] == "gap" and first["ordinal"] == 150
            assert (first["pts"], first["expected_pts"]) == (3_000_000_000, 2_980_000_000)
    if name == "first_output_drop":
        assert all(r["pts"] < 2_980_000_000 for r in buffers(case))
        continue
    for c in case["window_context"]:
        assert c["stream_known"] and c["segment_valid"] and c["caps"]["format"] == "UYVY"
        assert c["segment"]["base"] == 0
    if not dropped:
        assert not case["window"]["triggered"]
        assert max(r["pts"] for r in buffers(case)) == 3_980_000_000
        continue
    assert case["window"]["triggered"] and case["window"]["complete"]
    assert case["window"]["trigger_seqnum"] == 0xD01 and case["window"]["post_retained"] == 16
    assert case["window"]["trigger_order"] > case["capacity"]
    assert [r["value"] for r in case["records"] if r["kind"] == "trigger"] == [0xD01]
    observed = {stage: buffers(case, stage) for stage in ("output-pack", "output-scale", "sink")}
    low = max(rows[0]["pts"] for rows in observed.values())
    high = min(rows[-1]["pts"] for rows in observed.values())
    # Partial first/last cross-stage deliveries are outside this common interval,
    # never fabricated as lost frames. The injected point is strictly inside it.
    assert low < 2_980_000_000 < high
    census = {stage: {r["pts"] for r in rows if low <= r["pts"] <= high}
              for stage, rows in observed.items()}
    assert census["output-pack"] - census["output-scale"] == {2_980_000_000}
    assert census["output-scale"] == census["sink"]
    for rows in observed.values():
        assert all(r["duration"] == 20_000_000 and r["running_time"] == r["pts"] for r in rows)

listed = cases["window_buffer_list"]
assert listed["buffer_list_ambiguity"] and listed["retained"] == 3
assert not any(r["kind"] == "buffer-list-item" for r in listed["records"])
for stage in ("decoder", "sink"):
    owner = attachment(listed, stage)
    assert owner["buffer_lists"] == 1 and owner["list_items"] == 3 and owner["buffer_list_ambiguity"]
    assert not owner["pts_summary"]["qualified"]

def pts_summary(name):
    return attachment(cases[name], "output-scale")["pts_summary"]

for name in summary_clean:
    case = cases[name]
    summary = pts_summary(name)
    period = 20_000_000 if name.endswith("_50") else 40_000_000
    assert case["capacity"] == case["retained"] == 4 and case["truncated"], name
    assert summary["qualified"] and summary["buffers"] == summary["covered"] == 20, name
    assert summary["period_ns"] == period and summary["first_pts"] == 0, name
    assert summary["last_pts"] == 19 * period and summary["caps_changes"] == 0, name
    if name.startswith("summary_first_"):
        # CAPS and SEGMENT arrived after every detail slot was consumed.
        assert all(r["kind"] in ("attach", "control") for r in case["records"]), name
    elif "frozen" in name:
        assert case["window"]["complete"] and case["window"]["unretained_after_window"] > 10, name
        assert max(r["pts"] for r in buffers(case)) == 4 * period < summary["last_pts"], name
    else:
        assert case["window"]["overwritten"] > 20 and not case["window"]["triggered"], name

# Each sequence contains one early defect and a long otherwise regular suffix.
# Inspect the independently chosen stimulus, not a replay of summary arithmetic.
defect_expectations = {
    "gap": ("gap", "gap_events", 1),
    "duplicate": ("duplicate", "duplicate_events", 1),
    "reverse": ("reverse", "reverse_events", 1),
    "off_grid": ("off-grid", "off_grid_events", 17),
    "unknown_pts": ("unknown-pts", "unknown_pts", 1),
    "unknown_duration": ("unknown-duration", "unknown_duration", 1),
    "duration": ("invalid-duration", "invalid_duration", 1),
    "changed_caps": ("caps-changed", "caps_changes", 1),
    "unknown_caps": ("caps-changed", "caps_changes", 1),
    "vfr_caps": ("caps-changed", "caps_changes", 1),
    "segment": ("segment-changed", "segment_events", 2),
    "rate": ("segment-changed", "segment_events", 2),
    "applied_rate": ("segment-changed", "segment_events", 2),
    "bytes_segment": ("segment-changed", "segment_events", 2),
    "stream": ("stream-changed", "epochs", 3),
    "flush": ("flush", "flush_events", 2),
    "seek": ("seek-request", "seek_requests", 1),
    "buffer_gap": ("buffer-gap", "epochs", 1),
    "discont": ("buffer-discont", "epochs", 2),
}
assert set(defect_expectations) == summary_defects
for defect, (reason, field, value) in defect_expectations.items():
    name = "window_summary_" + defect
    case, summary = cases[name], pts_summary(name)
    assert not summary["qualified"] and summary["buffers"] == 20, name
    assert summary[field] == value and summary["first_violation"]["reason"] == reason, (name, summary)
    assert summary["first_violation"]["order"] < case["records"][0]["order"], name
    assert summary["first_pts"] == 0, name
    if defect in ("unknown_pts", "unknown_duration", "duration"):
        assert summary["covered"] == 19 and summary["unqualified_buffers"] == 1, name
assert pts_summary("window_summary_gap")["gap_steps"] == 1
assert pts_summary("window_summary_duplicate")["gap_events"] == 0
assert pts_summary("window_summary_reverse")["gap_events"] == 0
assert pts_summary("window_summary_off_grid")["gap_steps"] == 0

case = cases["window_summary_frozen_caps"]
summary = pts_summary("window_summary_frozen_caps")
assert case["window"]["complete"] and not summary["qualified"]
assert summary["caps_changes"] == 1 and summary["period_ns"] == 20_000_000
assert summary["buffers"] == summary["covered"] == 22 and summary["last_pts"] == 460_000_000
assert summary["first_violation"]["reason"] == "caps-changed"
assert summary["first_violation"]["order"] > case["records"][-1]["order"]
assert not any(r["kind"] == "caps" for r in case["records"])

summary = pts_summary("window_summary_overflow")
assert not summary["qualified"] and summary["arithmetic_overflow"]
assert summary["first_violation"]["reason"] == "arithmetic-overflow"
assert summary["first_pts"] == 2**64 - 1 - 80_000_000
assert summary["last_pts"] == 2**64 - 1 - 40_000_000
assert summary["gap_events"] == summary["reverse_events"] == 0
summary = pts_summary("window_summary_saturation")
assert not summary["qualified"] and summary["counters_saturated"]
assert cases["window_summary_saturation"]["counters_saturated"]
assert summary["gap_steps"] == 2**64 - 1 and summary["gap_events"] == 1
assert summary["first_violation"]["reason"] == "counter-saturation"

for suffix in ("unknown_caps", "vfr", "vfr_max", "rate", "applied_rate", "bytes"):
    name = "window_summary_initial_" + suffix
    summary = pts_summary(name)
    assert not summary["qualified"] and summary["buffers"] == summary["unqualified_buffers"] == 20, name
    assert summary["covered"] == 0 and summary["epochs"] == summary["segment_events"] == 1, name
    reason = "caps-unsupported" if suffix in ("unknown_caps", "vfr", "vfr_max") else "segment-unsupported"
    assert summary["first_violation"]["reason"] == reason, name
    assert summary["first_violation"]["order"] < cases[name]["records"][0]["order"], name
summary = pts_summary("summary_missing_eos")
assert not summary["qualified"] and summary["buffers"] == summary["covered"] == 1
assert summary["first_violation"] is None
summary = pts_summary("summary_empty")
assert not summary["qualified"] and summary["buffers"] == summary["covered"] == 0
assert summary["first_pts"] is summary["last_pts"] is None
summary = pts_summary("window_summary_negative_running_time")
assert not summary["qualified"] and summary["covered"] == 0 and summary["unqualified_buffers"] == 20
assert summary["first_violation"]["reason"] == "context-unknown"

# Feed the real native snapshots to the standalone reader. Every expectation
# below comes from the chosen test stimulus, independently of retained records
# and summary fields. Native-only explicitly omits Player completion evidence.
def audit_native(name, goals, status):
    report = auditor.audit(cases[name], {"schema": 1, "attachments": goals}, require_player=False)
    assert report["status"] == status, (name, report)

def goal(token, count=20, period=40_000_000, first=0, stage="output-scale"):
    return {"stage": stage, "token": token, "buffers": count,
            "first_pts": first, "period_ns": period}

for name, period in (("summary_first_25", 40_000_000), ("summary_first_50", 20_000_000),
                     ("window_summary_25", 40_000_000), ("window_summary_50", 20_000_000),
                     ("window_summary_frozen_25", 40_000_000), ("window_summary_frozen_50", 20_000_000)):
    audit_native(name, [goal(120, period=period)], "PASS")
for defect in summary_defects:
    audit_native("window_summary_" + defect, [goal(121)], "FAIL")
audit_native("window_summary_frozen_caps", [goal(122, count=22)], "FAIL")
audit_native("window_summary_overflow", [goal(123, count=2, first=2**64 - 1 - 80_000_000)], "FAIL")
audit_native("window_summary_saturation", [goal(124, count=2)], "FAIL")
for suffix in ("unknown_caps", "vfr", "vfr_max", "rate", "applied_rate", "bytes"):
    audit_native("window_summary_initial_" + suffix, [goal(125)], "FAIL")
audit_native("window_summary_negative_running_time", [goal(125)], "FAIL")
audit_native("summary_missing_eos", [goal(126, count=1)], "FAIL")
audit_native("summary_empty", [goal(127, count=1)], "FAIL")
for name in ("buffer_list", "custom_buffer_list", "truncated_buffer_list", "window_buffer_list"):
    audit_native(name, [goal(54, count=3, stage="decoder"), goal(55, count=3, stage="sink")], "FAIL")
output_goals = [goal(token, count=200, period=20_000_000, stage=stage)
                for token, stage in ((101, "output-pack"), (102, "output-scale"), (103, "sink"))]
audit_native("window_output", output_goals, "PASS")
audit_native("window_output_drop", output_goals, "FAIL")
audit_native("first_output_drop", output_goals, "FAIL")

print("Stage trace %s: fourteen native groups, %d structured snapshots, "
      "PTS/segment/cap/lifetime, fields, pack/scale, window and whole-run summary controls PASS" % (variant, len(cases)))
PY
done
python3 -B "$repo_dir/tests/stage-pts-audit.test.py"
