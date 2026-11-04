#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""OWN same-input original-App BWDIF reference; no device or client EOF claim.

The controller supplies a manifest from the strict postfilter reader and an
original App/Platform namespace command prefix. This wrapper never builds or
changes FFmpeg. Prefix replay preserves the captured API order and never
signals EOF. Explicit flushed-reference mode is a separate counterfactual.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import time

BASIS = "PINNED_ORIGINAL_SOURCE_PLUS_ACTUAL_NV12_SINK_OUTPUT"
FILTER_VERSION, UTIL_VERSION = 786790, 3998054
FRAME_KEYS = ("pts", "best_effort_timestamp", "duration", "time_base", "format",
              "width", "height", "linesize", "flags", "sample_aspect_ratio",
              "color_range", "color_primaries", "color_trc", "colorspace",
              "chroma_location", "crop")
ADD = {"av_buffersrc_add_frame", "av_buffersrc_add_frame_flags"}
GET = {"av_buffersink_get_frame", "av_buffersink_get_frame_flags"}
EAGAIN = -11
MAX_REPORT_BYTES = 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def regular(path, size=None):
    path = Path(path)
    require(path.is_absolute() and not path.is_symlink() and path.resolve(strict=True) == path,
            "snapshot must be an absolute canonical regular file")
    data = path.stat()
    require(stat.S_ISREG(data.st_mode) and (size is None or data.st_size == size),
            "snapshot has wrong type or active-plane size")
    return path


def integer(value, low=-(1 << 63), high=(1 << 63)-1):
    require(type(value) is int and low <= value <= high, "invalid bounded integer")
    return value


def vector(value, length):
    require(type(value) is list and len(value) == length, "invalid property vector")
    return [integer(v, -(1 << 31), (1 << 31)-1) for v in value]


def validate_frame(frame, nv12, output=False):
    require(type(frame) is dict and all(k in frame for k in FRAME_KEYS), "missing captured frame properties")
    w, h = integer(frame["width"], 2, 1920), integer(frame["height"], 2, 1088)
    require(not w % 2 and not h % 2 and frame["format"] == nv12, "only captured active even NV12 is supported")
    strides = vector(frame["linesize"], 2)
    require(all(w <= v <= 32768 for v in strides), "invalid captured positive strides")
    for key in ("time_base", "sample_aspect_ratio"):
        vector(frame[key], 2)
    crop = vector(frame["crop"], 4)
    require(all(v >= 0 for v in crop) and crop[0]+crop[1] < h and crop[2]+crop[3] < w,
            "invalid captured crop")
    for key in ("pts", "best_effort_timestamp", "duration"):
        integer(frame[key])
    for key in ("flags", "color_range", "color_primaries", "color_trc", "colorspace", "chroma_location"):
        integer(frame[key], -(1 << 31), (1 << 31)-1)
    regular(frame["path"], w*h*3//2)
    if output:
        vector(frame["sink_time_base"], 2)
        vector(frame["sink_frame_rate"], 2)


def validate_manifest(manifest):
    require(type(manifest) is dict and manifest.get("schema") == 1, "unknown reference manifest")
    require(manifest.get("mode", "prefix") in ("prefix", "flushed-reference"), "unknown reference mode")
    graph = manifest["graph"]
    require(graph.get("filter_name") == "bwdif" and graph.get("constraint_basis") == BASIS,
            "BWDIF/pinned-source sink-constraint basis is required")
    for key, limit in (("buffer_args", 1024), ("filter_args", 128)):
        require(type(graph.get(key)) is str and 0 < len(graph[key]) < limit and "\x00" not in graph[key],
                "missing exact captured graph argument")
    require(graph["filter_args"].startswith("bwdif=") and
            not any(c in graph["filter_args"] for c in ",;[]"), "only the captured single BWDIF filter is supported")
    providers = manifest["providers"]
    for key, basename in (("filter", "libavfilter.so.12.1.102"), ("util", "libavutil.so.61.1.102")):
        value = providers[key]
        require(type(value) is str and Path(value).is_absolute() and Path(value).name == basename and "\x00" not in value,
                "exact original provider filename/version required")
    nv12 = integer(manifest["nv12_format"], 0, 255)
    inputs, outputs = manifest["inputs"], manifest["expected_outputs"]
    require(type(inputs) is list and len(inputs) == 3 and type(outputs) is list and len(outputs) == 4,
            "reference requires exactly three captured inputs and four captured prefix outputs")
    for frame in inputs:
        validate_frame(frame, nv12)
    for frame in outputs:
        validate_frame(frame, nv12, True)
    require(all((f["width"], f["height"]) == (inputs[0]["width"], inputs[0]["height"]) for f in inputs),
            "prefix input geometry changed")
    schedule = manifest["schedule"]
    require(type(schedule) is list and 0 < len(schedule) <= 128, "missing bounded original call schedule")
    seen, out = set(), 0
    for call in schedule:
        api = call["api"]
        require(api in ADD | GET, "prefix must not contain NULL, close, EOF or unsupported calls")
        flags, ret = integer(call["flags"], 0, (1 << 31)-1), integer(call["ret"], -(1 << 31), (1 << 31)-1)
        require(api.endswith("_flags") or flags == 0, "plain original API cannot have flags")
        if api in ADD:
            index = integer(call["input_index"], 1, 3)
            require(index not in seen and ret >= 0 and call.get("input_null", False) is False,
                    "prefix requires three distinct accepted nonnull inputs")
            seen.add(index)
        else:
            require(ret >= 0 or ret == EAGAIN, "captured prefix pull must be success or EAGAIN, never manufactured EOF")
            if ret >= 0:
                out += 1
                require(integer(call["output_index"], 1, 4) == out, "captured successful output order differs")
            else:
                require(call.get("output_index", 0) == 0, "EAGAIN must not invent an output")
    require(seen == {1, 2, 3} and out == 4, "captured prefix is incomplete")
    return manifest


def translate(path, path_map):
    path = Path(path)
    for host, target in sorted(path_map, key=lambda pair: len(str(pair[0])), reverse=True):
        try:
            return str(Path(target) / path.relative_to(Path(host)))
        except ValueError:
            pass
    require(not path_map, "snapshot/output path is outside explicit namespace mapping")
    return str(path)


def build_command(manifest, client, output_dir, command_prefix=(), path_map=()):
    validate_manifest(manifest)
    require(Path(client).is_absolute(), "reference client path must be absolute")
    require(all(type(v) is str and "\x00" not in v for v in command_prefix), "command prefix must be an argv array")
    command = list(command_prefix) + [str(client), "--mode", manifest.get("mode", "prefix"),
        "--filter-provider", manifest["providers"]["filter"], "--util-provider", manifest["providers"]["util"],
        "--buffer-args", manifest["graph"]["buffer_args"], "--filter-args", manifest["graph"]["filter_args"],
        "--output-dir", translate(output_dir, path_map)]
    for frame in manifest["inputs"]:
        values = [frame["width"], frame["height"], *frame["linesize"], frame["pts"],
                  frame["best_effort_timestamp"], frame["duration"], *frame["time_base"], frame["flags"],
                  *frame["sample_aspect_ratio"], frame["color_range"], frame["color_primaries"],
                  frame["color_trc"], frame["colorspace"], frame["chroma_location"], *frame["crop"]]
        command.extend(["--frame", translate(frame["path"], path_map), *map(str, values)])
    for call in manifest["schedule"]:
        index = call["input_index"] if call["api"] in ADD else call.get("output_index", 0)
        command.extend(["--step", call["api"], str(index), str(call["flags"]), str(call["ret"])])
    return command


def compare_reference(manifest, records, output_dir):
    validate_manifest(manifest)
    require(records and records[0].get("type") == "header" and records[-1].get("type") == "footer",
            "incomplete reference report")
    header, footer = records[0], records[-1]
    mode = manifest.get("mode", "prefix")
    require(header.get("schema") == "crystalhd-postfilter-reference-v1" and header.get("provider_ok") is True and
            header.get("bwdif_available") is True and header.get("filter_version") == FILTER_VERSION and
            header.get("util_version") == UTIL_VERSION and header.get("nv12_format") == manifest["nv12_format"] and
            header.get("mode") == mode and header.get("constraint_basis") == BASIS,
            "reference original-provider/ABI/BWDIF/basis check failed")
    require({k: header[k+"_provider"] for k in ("filter", "util")} == manifest["providers"],
            "reference provider paths differ from original observer")
    require(footer.get("complete") is True and footer.get("inputs") == 3 and
            footer.get("original_client_eof_claim") is False and
            footer.get("source_eof_injected") == (mode == "flushed-reference") and
            footer.get("reference_eof_observed") == (mode == "flushed-reference"),
            "reference completion/EOF scope differs")
    allowed = {"header", "call", "output", "footer"} | ({"reference_eof"} if mode == "flushed-reference" else set())
    require(all(r.get("type") in allowed for r in records), "unexpected reference record")
    closes = [r for r in records if r["type"] == "reference_eof"]
    require((not closes) if mode == "prefix" else (
            len(closes) == 1 and closes[0].get("api") == "av_buffersrc_close" and
            closes[0].get("reference_only_eof") is True and
            type(closes[0].get("ret")) is int and closes[0]["ret"] >= 0 and
            closes[0].get("close_pts") == -(1 << 63)), "explicit reference EOF attribution differs")
    calls = [r for r in records if r["type"] == "call"]
    require(len(calls) == len(manifest["schedule"]), "reference call count changed")
    for index, (actual, wanted) in enumerate(zip(calls, manifest["schedule"])):
        require(actual.get("step") == index and actual.get("api") == wanted["api"] and
                actual.get("ret") == wanted["ret"] and actual.get("expected_ret") == wanted["ret"] and
                actual.get("reference_only_eof") is False, "reference API order/result differs")
    raw_outputs = [r for r in records if r["type"] == "output"]
    require(footer.get("outputs") == len(raw_outputs) and 4 <= len(raw_outputs) <= 16 and
            (mode != "prefix" or len(raw_outputs) == 4), "reference output count/scope differs")
    comparisons, outputs = [], []
    for index, actual in enumerate(raw_outputs, 1):
        require(actual.get("output_index") == index and actual.get("reference_only_eof") == (index > 4),
                "reference output index/EOF attribution differs")
        name = actual["file"]
        require(type(name) is str and name == f"output-{index:02d}.nv12", "unexpected reference snapshot name")
        frame = dict(actual["frame"], path=str(Path(output_dir)/name),
                     sink_time_base=actual["sink_time_base"], sink_frame_rate=actual["sink_frame_rate"],
                     reference_only_eof=actual["reference_only_eof"])
        validate_frame(frame, manifest["nv12_format"], True)
        outputs.append(frame)
        if index <= 4:
            wanted = manifest["expected_outputs"][index-1]
            differences = [k for k in FRAME_KEYS + ("sink_time_base", "sink_frame_rate") if frame[k] != wanted[k]]
            require(not differences, "reference captured metadata differs: " + ",".join(differences))
            require(Path(frame["path"]).read_bytes() == Path(wanted["path"]).read_bytes(),
                    "reference active NV12 pixels differ at output " + str(index))
            comparisons.append(dict(output_index=index, active_pixels_equal=True, captured_metadata_equal=True))
    return dict(schema=1, status="PASS", mode=mode, manifest=copy.deepcopy(manifest), outputs=outputs,
                providers=manifest["providers"], constraint_basis=BASIS, source_clock_comparison=False,
                original_client_eof_claim=False, source_eof_injected=mode == "flushed-reference",
                comparison_scope="SAME_INPUT_PREFIX_ACTIVE_NV12_AND_CAPTURED_METADATA", comparisons=comparisons,
                limits=["Same captured inputs and selected properties; no independent source codec/scaler/colour golden.",
                        "NV12 sink constraint follows pinned original source plus actual NV12 output, not an observed option call.",
                        "Active Y/UV only; original pitch is reproduced with zeroed unobserved allocation padding.",
                        "Raw frame/sink/graph/source clocks remain distinct; no source-second or physical A/V inference.",
                        "Any explicit flushed-reference EOF is reference-only and says nothing about original-client EOF/tail behavior."])


def process_identity(process):
    require(process.poll() is None, "reference process already exited")
    base = Path("/proc")/str(process.pid)
    fields = (base/"stat").read_text().rsplit(") ", 1)[1].split()
    require(int(fields[1]) == os.getpid(), "reference is not direct owned child")
    return dict(pid=process.pid, parent_pid=int(fields[1]), start_ticks=int(fields[19]))


def run_reference(manifest, client, output_dir, command_prefix=(), path_map=(), timeout=30):
    validate_manifest(manifest)
    require(.1 <= timeout <= 120, "reference timeout outside bound")
    output_dir = Path(output_dir)
    require(output_dir.is_absolute() and not output_dir.exists() and output_dir.parent.resolve(strict=True) == output_dir.parent,
            "use a new output directory in a canonical existing parent")
    command = build_command(manifest, client, output_dir, command_prefix, path_map)
    output_dir.mkdir(mode=0o700)
    stdout, stderr = output_dir/"reference.jsonl", output_dir/"reference.stderr"
    process, pidfd, owned = None, None, None
    began = time.monotonic()
    execution = dict(command=command, timeout_s=timeout)
    failure = None
    stop = []
    handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
    for sig in handlers:
        signal.signal(sig, lambda received, _frame: stop.append(received))
    try:
        with stdout.open("xb") as out, stderr.open("xb") as err:
            require(not stop, "reference stop requested before launch")
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                       start_new_session=True)
            if process.poll() is None:
                try:
                    pidfd = os.pidfd_open(process.pid)
                    owned = process_identity(process)
                except (OSError, ValueError):
                    if process.poll() is None:
                        raise
            while process.poll() is None:
                require(not stop, "reference stop requested")
                require(time.monotonic()-began < timeout, "reference deadline exceeded")
                require(stdout.stat().st_size <= MAX_REPORT_BYTES and stderr.stat().st_size <= MAX_REPORT_BYTES,
                        "reference report bound exceeded")
                time.sleep(.05)
        execution.update(exit=process.returncode, elapsed_s=time.monotonic()-began, owned_process=owned)
        require(process.returncode == 0, "original-App reference failed; preserve raw report/stderr")
        require(stdout.stat().st_size <= MAX_REPORT_BYTES and stderr.stat().st_size <= MAX_REPORT_BYTES,
                "reference final report bound exceeded")
        records = [json.loads(line) for line in stdout.read_text().splitlines()]
        result = compare_reference(manifest, records, output_dir)
        result["execution"] = execution
        return result
    except BaseException as error:
        failure = dict(status="FAIL", error=repr(error), execution=execution,
                       scope="Reference failure is not original-client behavior or a product fix.")
        raise
    finally:
        if process is not None and process.poll() is None:
            handoff = None
            try:
                require(pidfd is not None and owned is not None and process_identity(process) == owned,
                        "unproved reference process identity; preserve live owner")
                signal.pidfd_send_signal(pidfd, signal.SIGTERM)
                execution["verified_pidfd_term"] = owned
                until = time.monotonic()+5
                while process.poll() is None and time.monotonic() < until:
                    time.sleep(.05)
                require(process.poll() is not None, "reference survived bounded owned TERM")
                # A namespace supervisor may exit before an unproved descendant.
                # Never promote that state to retirement or signal a process group.
                require(not command_prefix, "namespace descendant retirement unproved after cancellation")
            except BaseException as error:
                handoff = dict(status="HANDOFF_REQUIRED", reason=repr(error), identity=owned,
                               pid=process.pid, policy="Preserve namespace/process/output; no SIGKILL, group signal or recovery.")
            if handoff:
                (output_dir/"HANDOFF_REQUIRED.json").write_text(json.dumps(handoff, indent=2)+"\n")
                if failure is not None:
                    failure["handoff"] = handoff
        if pidfd is not None:
            os.close(pidfd)
        if failure is not None:
            (output_dir/"reference-failure.json").write_text(json.dumps(failure, indent=2)+"\n")
        for sig, handler in handlers.items():
            signal.signal(sig, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--client", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--command-prefix-file", type=Path)
    parser.add_argument("--path-map", action="append", default=[], metavar="HOST=NAMESPACE")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    require(not args.results.exists() and args.results.parent.is_dir(), "use a new result file")
    manifest = json.loads(args.manifest.read_text())
    prefix = json.loads(args.command_prefix_file.read_text()) if args.command_prefix_file else []
    require(type(prefix) is list, "namespace command prefix must be a JSON argv array")
    mapping = []
    for item in args.path_map:
        host, target = item.split("=", 1)
        require(Path(host).is_absolute() and Path(target).is_absolute(), "path maps must be absolute")
        mapping.append((host, target))
    result = run_reference(manifest, args.client, args.output_dir, prefix, mapping, args.timeout)
    with args.results.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(dict(status=result["status"], mode=result["mode"],
                          compared_outputs=len(result["comparisons"]), original_client_eof_claim=False)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
