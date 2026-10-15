#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build and verify the frozen VC-1 Advanced seek container without decoding hardware."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


SOURCE_BYTES = 6116814
SOURCE_SHA256 = "972335121a97d9bb5b3639c2e59e9f9fc04eb64c7a355c73d2d8814eb0e75edb"
FRAME_COUNT = 60
FRAME_BYTES = 1920 * 1080 * 3 // 2
RATE = "24000/1001"
TIME_BASE = "1/1200000"
PTS_STEP = 50050
EXPECTED_FRAME_PTS = ([0, PTS_STEP] +
                      list(range(3 * PTS_STEP,
                                 (FRAME_COUNT + 1) * PTS_STEP, PTS_STEP)))
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.resolve()
REFERENCE = HERE / "fixtures" / "issue91" / "vc1-advanced-fhd-seek-v1.framehash"


class AdmissionError(RuntimeError):
    """The source, container, or decoded reference does not match the oracle."""


def run(arguments, timeout=None):
    try:
        process = subprocess.run(arguments, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise AdmissionError("%s timed out" % Path(arguments[0]).name) from error
    except OSError as error:
        raise AdmissionError("%s could not start" % Path(arguments[0]).name) from error
    if process.returncode:
        detail = process.stderr.decode("utf-8", "replace").strip()
        tool = Path(arguments[0]).name
        raise AdmissionError("%s failed%s" %
                             (tool, ": " + detail if detail else ""))
    return process.stdout


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def input_path(value):
    path = Path(value)
    try:
        resolved = path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        raise AdmissionError("input is unavailable") from error
    if not resolved.is_file():
        raise AdmissionError("input must be a regular file")
    return resolved


def output_path(value, create):
    path = Path(value)
    if not path.is_absolute():
        raise AdmissionError("output path must be absolute")
    try:
        resolved = path.resolve(strict=False)
    except (OSError, RuntimeError) as error:
        raise AdmissionError("output path cannot be resolved") from error
    if resolved == REPO or REPO in resolved.parents:
        raise AdmissionError("output must be outside the source tree")
    if create:
        if path.exists() or path.is_symlink():
            raise AdmissionError("refusing an existing output path")
        try:
            parent = path.parent.resolve(strict=True)
        except (OSError, RuntimeError) as error:
            raise AdmissionError("output parent must already exist") from error
        if not parent.is_dir():
            raise AdmissionError("output parent must be a directory")
    elif not path.is_file() or path.is_symlink():
        raise AdmissionError("container must be a regular, non-symlink file")
    return resolved


def require_tools():
    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            raise AdmissionError("%s is required" % tool)


def admit_source(path):
    if path.stat().st_size != SOURCE_BYTES or sha256_file(path) != SOURCE_SHA256:
        raise AdmissionError("source is not the frozen VC-1 Advanced fixture")


def remux(source, destination):
    run([
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
        "-n", "-fflags", "+genpts", "-f", "vc1", "-framerate", RATE,
        "-i", str(source), "-map", "0:v:0", "-c:v", "copy", "-an", "-sn",
        "-dn", "-map_metadata", "-1", "-map_chapters", "-1", "-fflags",
        "+bitexact", "-flags:v", "+bitexact", "-video_track_timescale",
        "1200000", "-movflags", "+faststart", "-f", "mov", str(destination),
    ])


def probe_container(path):
    payload = run([
        "ffprobe", "-v", "error", "-show_streams", "-show_format", "-of",
        "json", str(path),
    ])
    try:
        probe = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        raise AdmissionError("ffprobe returned malformed JSON") from error
    streams = probe.get("streams", [])
    if len(streams) != 1 or streams[0].get("codec_type") != "video":
        raise AdmissionError("container must contain exactly one video stream")
    stream = streams[0]
    expected = {
        "codec_name": "vc1", "profile": "Advanced", "level": 3,
        "width": 1920, "height": 1080, "coded_width": 1920,
        "coded_height": 1080, "has_b_frames": 1, "pix_fmt": "yuv420p",
        "color_space": "bt709", "color_transfer": "bt709",
        "color_primaries": "bt709", "chroma_location": "left",
        "field_order": "progressive", "r_frame_rate": RATE,
        "avg_frame_rate": RATE, "time_base": TIME_BASE, "start_pts": 0,
        "duration_ts": FRAME_COUNT * PTS_STEP, "extradata_size": 33,
    }
    for key, value in expected.items():
        if stream.get(key) != value:
            raise AdmissionError("container stream %s drifted" % key)
    try:
        declared_frames = int(stream.get("nb_frames", ""))
    except (TypeError, ValueError) as error:
        raise AdmissionError("container has no reliable frame count") from error
    if declared_frames != FRAME_COUNT:
        raise AdmissionError("container declared frame count drifted")
    format_name = probe.get("format", {}).get("format_name", "").split(",")
    if "mov" not in format_name:
        raise AdmissionError("container is not QuickTime/MOV")


def packet_sizes(path):
    payload = run([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_packets",
        "-show_entries", "packet=size", "-of", "json", str(path),
    ])
    try:
        packets = json.loads(payload.decode("utf-8")).get("packets", [])
        sizes = [int(packet["size"]) for packet in packets]
    except (KeyError, TypeError, UnicodeDecodeError, ValueError) as error:
        raise AdmissionError("packet extent is malformed") from error
    if len(sizes) != FRAME_COUNT or any(size <= 0 for size in sizes):
        raise AdmissionError("packet extent drifted")
    return sizes


def extract_payload(path):
    return run([
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
        "-i", str(path), "-map", "0:v:0", "-c:v", "copy", "-f", "vc1", "-",
    ])


def validate_payload_identity(source_data, extracted, source_sizes,
                              container_sizes):
    if extracted != source_data or sha256_bytes(extracted) != SOURCE_SHA256:
        raise AdmissionError("remuxed compressed payload differs from the source")
    if source_sizes != container_sizes:
        raise AdmissionError("remuxed packet boundaries differ from the source")


def decoded_framehash(path):
    return run([
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
        "-i", str(path), "-map", "0:v:0", "-fps_mode", "passthrough",
        "-pix_fmt", "yuv420p", "-fflags", "+bitexact", "-flags:v",
        "+bitexact", "-f", "framehash", "-hash", "sha256", "-",
    ])


def decoded_timestamps(path):
    payload = run([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
        "-show_entries", "frame=best_effort_timestamp,duration,pkt_duration",
        "-of", "json", str(path),
    ])
    try:
        frames = json.loads(payload.decode("utf-8")).get("frames", [])
        timestamps = []
        for frame in frames:
            if ("duration" in frame and "pkt_duration" in frame and
                    int(frame["duration"]) != int(frame["pkt_duration"])):
                raise AdmissionError("decoded duration aliases disagree")
            duration = frame.get("duration", frame.get("pkt_duration"))
            timestamps.append((int(frame["best_effort_timestamp"]),
                               int(duration)))
    except AdmissionError:
        raise
    except (KeyError, TypeError, UnicodeDecodeError, ValueError) as error:
        raise AdmissionError("decoded timestamp reference is malformed") from error
    if ([item[0] for item in timestamps] != EXPECTED_FRAME_PTS or
            any(item[1] != PTS_STEP for item in timestamps)):
        raise AdmissionError("decoded timestamp reference differs")
    return timestamps


def publish(candidate, output):
    try:
        # The candidate lives under output.parent, so a hard link is an atomic
        # same-filesystem no-clobber publication. TemporaryDirectory removes
        # the candidate name after the verified inode has been published.
        os.link(str(candidate), str(output))
    except FileExistsError as error:
        raise AdmissionError("refusing an output created during verification") from error
    except OSError as error:
        raise AdmissionError("verified container could not be published") from error


def parse_framehash(payload):
    try:
        text = payload.decode("utf-8") if isinstance(payload, bytes) else payload
    except UnicodeDecodeError as error:
        raise AdmissionError("frame reference is not UTF-8") from error
    required = {
        "#format: frame checksums", "#version: 2", "#hash: SHA256",
        "#tb 0: 1001/24000", "#media_type 0: video",
        "#codec_id 0: rawvideo", "#dimensions 0: 1920x1080",
        "#sar 0: 1/1",
    }
    comments = {line for line in text.splitlines() if line.startswith("#")}
    if not required.issubset(comments):
        raise AdmissionError("frame reference header drifted")
    rows = []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) != 6 or not re.fullmatch(r"[0-9a-f]{64}", fields[5]):
            raise AdmissionError("malformed frame reference row")
        try:
            stream, dts, pts, duration, size = map(int, fields[:5])
        except ValueError as error:
            raise AdmissionError("non-integer frame reference field") from error
        rows.append((stream, dts, pts, duration, size, fields[5]))
    expected_pts = [0, 1] + list(range(3, 61))
    if len(rows) != FRAME_COUNT or [row[2] for row in rows] != expected_pts:
        raise AdmissionError("frame reference PTS sequence drifted")
    if any(row[0] != 0 or row[1] != row[2] or row[3] != 1 or
           row[4] != FRAME_BYTES for row in rows):
        raise AdmissionError("frame reference extent drifted")
    return rows


def run_seek_probe(probe, container):
    path = input_path(probe)
    results = []
    for options in (("--software",), ("--software", "--lookahead", "8")):
        output = run([str(path), str(container)] + list(options), timeout=120)
        text = output.decode("utf-8", "replace")
        if ("reference: 60 software frames drained" not in text or
                text.count("frame SHA-256 digests match") != 4):
            raise AdmissionError("seek probe did not report the complete oracle")
        results.append(text)
    return results


def verify(source_value, container_value, seek_probe=None):
    require_tools()
    source = input_path(source_value)
    container = output_path(container_value, create=False)
    admit_source(source)
    probe_container(container)
    source_data = source.read_bytes()
    extracted = extract_payload(container)
    validate_payload_identity(source_data, extracted, packet_sizes(source),
                              packet_sizes(container))
    expected = REFERENCE.read_bytes()
    parse_framehash(expected)
    decoded_timestamps(container)
    actual = decoded_framehash(container)
    parse_framehash(actual)
    if actual != expected:
        raise AdmissionError("decoded PTS/pixel reference differs")
    if seek_probe is not None:
        run_seek_probe(seek_probe, container)
    return {
        "codec": "VC-1 Advanced L3",
        "compressed_payload_sha256": SOURCE_SHA256,
        "compressed_payload_bytes": SOURCE_BYTES,
        "packet_count": FRAME_COUNT,
        "decoded_frames": FRAME_COUNT,
        "decoded_pts_time_base": TIME_BASE,
        "decoded_framehash_sha256": sha256_bytes(expected),
        "container_sha256": sha256_file(container),
        "seek_probe": "software-sync+lookahead8" if seek_probe else "not-run",
    }


def build(source_value, output_value, seek_probe=None):
    require_tools()
    source = input_path(source_value)
    output = output_path(output_value, create=True)
    admit_source(source)
    with tempfile.TemporaryDirectory(prefix=".crystalhd-vc1-seek.",
                                     dir=str(output.parent)) as temporary:
        candidate = Path(temporary) / "vc1-advanced-fhd60.mov"
        remux(source, candidate)
        summary = verify(str(source), str(candidate), seek_probe)
        publish(candidate, output)
    return summary


def arguments(argv):
    parser = argparse.ArgumentParser(
        description="Build or verify the frozen remux-only VC-1 seek oracle")
    subparsers = parser.add_subparsers(dest="command")
    for command in ("build", "verify"):
        child = subparsers.add_parser(command)
        child.add_argument("source", help="unchanged vc1-fhd-scene60.vc1")
        child.add_argument("container", help="absolute external MOV path")
        child.add_argument(
            "--seek-probe", metavar="PATH",
            help="also run vaapi-seek-test in software sync and lookahead modes")
    options = parser.parse_args(argv)
    if options.command is None:
        parser.error("choose build or verify")
    return options


def main(argv=None):
    try:
        options = arguments(sys.argv[1:] if argv is None else argv)
        if options.command == "build":
            summary = build(options.source, options.container, options.seek_probe)
        else:
            summary = verify(options.source, options.container, options.seek_probe)
        print(json.dumps(summary, sort_keys=True, indent=2))
        return 0
    except AdmissionError as error:
        print("VC-1 seek oracle: FAIL: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
