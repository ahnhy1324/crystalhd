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
    python3 - "$variant" "$test_dir/$variant.log" "$test_dir/$variant.stderr" <<'PY'
import json
from pathlib import Path
import sys

variant, log, errors = sys.argv[1:]
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

print("Stage trace %s: eleven native groups, 28 structured snapshots, "
      "PTS/segment/cap/lifetime, fields, pack/scale and late-loss window controls PASS" % variant)
PY
done
