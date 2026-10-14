#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate and verify the device-free, true-1080p60 issue #399 inputs."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


SCHEMA = "crystalhd-issue399-baseline-v1"
WIDTH = 1920
HEIGHT = 1080
RATE = "60/1"
TIME_BASE = "1/60000"
PTS_STEP = 1000
GOP = 60
PIX_FMT = "yuv420p"
PROFILE = "High"
LEVEL = 42
TARGET_BITRATE = 12_000_000
BUFFER_SIZE = 24_000_000
ENCODER_THREADS = 1
MAX_FRAMES = 4096
IDENTITY_BITS = 12
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.resolve()


class AdmissionError(RuntimeError):
    """An input or generated result does not satisfy the frozen baseline."""


def _inside(path, parent):
    return path == parent or parent in path.parents


def output_path(value, *, create):
    path = Path(value)
    if not path.is_absolute():
        raise AdmissionError("output path must be absolute")
    resolved = path.resolve(strict=False)
    if _inside(resolved, REPO):
        raise AdmissionError("results must be outside the source tree")
    if create:
        if path.exists() or path.is_symlink():
            raise AdmissionError("refusing an existing output path")
        if not path.parent.is_dir():
            raise AdmissionError("output parent must already exist")
    elif not path.is_dir() or path.is_symlink():
        raise AdmissionError("result path must be a real directory")
    return resolved


def run(arguments, *, capture=False):
    result = subprocess.run(arguments, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise AdmissionError("command failed: %s%s" %
                             (" ".join(arguments), "\n" + detail if detail else ""))
    return result.stdout if capture else b""


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_filter():
    filters = ["format=yuv420p", "setpts=N/(60*TB)",
               "drawbox=x=0:y=0:w=400:h=64:color=black:t=fill"]
    for bit in range(IDENTITY_BITS):
        x = 8 + 28 * bit
        divisor = 1 << bit
        filters.append(
            "drawbox=x=%d:y=8:w=16:h=16:color=white:t=fill:"
            "enable='eq(mod(floor(n/%d),2),1)'" % (x, divisor))
        filters.append(
            "drawbox=x=%d:y=40:w=16:h=16:color=white:t=fill:"
            "enable='eq(mod(floor(n/%d),2),0)'" % (x, divisor))
    filters.extend((
        "drawbox=x=352:y=8:w=16:h=48:color=white:t=fill",
        "drawbox=x=376:y=8:w=16:h=48:color=black:t=fill",
    ))
    return ",".join(filters)


def source_arguments(frames):
    return ["-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=60",
            "-frames:v", str(frames), "-vf", source_filter(), "-an"]


def stable_video_options(kind):
    if kind == "no-b":
        structure = ["-bf", "0"]
        x264 = ("open-gop=0:keyint=60:min-keyint=60:scenecut=0:"
                "bframes=0:b-adapt=0:b-pyramid=none:rc-lookahead=0:"
                "sync-lookahead=0:nal-hrd=cbr:force-cfr=1:info=0:"
                "colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv")
    elif kind == "b-reference":
        structure = ["-bf", "3"]
        x264 = ("open-gop=0:keyint=60:min-keyint=60:scenecut=0:"
                "bframes=3:b-adapt=0:b-pyramid=strict:nal-hrd=cbr:"
                "force-cfr=1:info=0:colorprim=bt709:transfer=bt709:"
                "colormatrix=bt709:range=tv")
    else:
        raise AdmissionError("unknown encode kind")
    return ["-c:v", "libx264", "-preset", "veryfast", "-threads", str(ENCODER_THREADS),
            "-profile:v", "high", "-level:v", "4.2", "-pix_fmt", PIX_FMT,
            "-g", str(GOP), "-keyint_min", str(GOP), "-sc_threshold", "0",
            "-refs", "4"] + structure + [
            "-b:v", str(TARGET_BITRATE), "-minrate", str(TARGET_BITRATE),
            "-maxrate", str(TARGET_BITRATE), "-bufsize", str(BUFFER_SIZE),
            "-x264-params", x264, "-color_range", "tv", "-colorspace", "bt709",
            "-color_primaries", "bt709", "-color_trc", "bt709",
            "-fps_mode", "cfr", "-video_track_timescale", "60000",
            "-movflags", "+faststart", "-map_metadata", "-1", "-map_chapters", "-1",
            "-fflags", "+bitexact", "-flags:v", "+bitexact", "-f", "mp4"]


def requested_options(kind):
    return {
        "profile": "high", "level": "4.2", "gop": GOP,
        "keyint_min": GOP, "closed_gop": True, "scene_cut": 0,
        "encoder_refs_option": 4, "b_frames": 0 if kind == "no-b" else 3,
        "b_adapt": 0, "b_pyramid": "none" if kind == "no-b" else "strict",
        "target_bit_rate": TARGET_BITRATE, "min_bit_rate": TARGET_BITRATE,
        "max_bit_rate": TARGET_BITRATE, "buffer_size": BUFFER_SIZE,
        "preset": "veryfast", "threads": ENCODER_THREADS,
    }


def framehash_command(input_arguments, destination=None):
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror", "-n"]
    command += input_arguments
    command += ["-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", PIX_FMT,
                "-f", "framehash", "-hash", "sha256"]
    command.append(str(destination) if destination is not None else "-")
    return command


def parse_framehash(payload):
    if isinstance(payload, Path):
        text = payload.read_text(encoding="utf-8")
    elif isinstance(payload, bytes):
        text = payload.decode("utf-8")
    else:
        text = payload
    rows = []
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) != 6:
            raise AdmissionError("malformed framehash row")
        try:
            stream, dts, pts, duration, size = map(int, fields[:5])
        except ValueError as error:
            raise AdmissionError("non-integer framehash field") from error
        digest = fields[5].lower()
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise AdmissionError("malformed SHA-256 frame hash")
        rows.append({"stream": stream, "dts": dts, "pts": pts,
                     "duration": duration, "size": size, "sha256": digest})
    return rows


def validate_source_hashes(rows, frames):
    if len(rows) != frames or any(row["stream"] != 0 or
                                  row["size"] != FRAME_BYTES for row in rows):
        raise AdmissionError("source full-frame hash extent drift")
    expected = list(range(frames))
    if ([row["dts"] for row in rows] != expected or
            [row["pts"] for row in rows] != expected or
            any(row["duration"] != 1 for row in rows)):
        raise AdmissionError("source full-frame hash cadence drift")
    if len({row["sha256"] for row in rows}) != frames:
        raise AdmissionError("source pattern does not contain one unique full frame per tick")


def probe(path):
    stream_payload = run([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_streams",
        "-show_format", "-of", "json", str(path)], capture=True)
    frame_payload = run([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames",
        "-show_entries",
        "frame=pts,duration,pkt_duration,key_frame,pict_type,interlaced_frame,top_field_first,"
        "repeat_pict,width,height,pix_fmt", "-of", "json", str(path)], capture=True)
    metadata = json.loads(stream_payload)
    frames = json.loads(frame_payload).get("frames", [])
    streams = metadata.get("streams", [])
    if len(streams) != 1:
        raise AdmissionError("fixture must have exactly one video stream")
    stream = streams[0]
    selected = {key: stream.get(key) for key in (
        "codec_name", "profile", "level", "width", "height", "has_b_frames",
        "pix_fmt", "color_range", "color_space", "color_transfer", "color_primaries",
        "field_order", "r_frame_rate", "avg_frame_rate", "time_base", "start_pts",
        "duration_ts", "bit_rate", "nb_frames")}
    fmt = metadata.get("format", {})
    selected["container_size"] = int(fmt.get("size", 0))
    selected["container_bit_rate"] = int(fmt.get("bit_rate", 0))
    for key in ("level", "width", "height", "has_b_frames", "start_pts", "duration_ts"):
        if selected[key] is not None:
            selected[key] = int(selected[key])
    for key in ("bit_rate", "nb_frames"):
        if selected[key] is not None:
            selected[key] = int(selected[key])
    normalized_frames = []
    for entry in frames:
        if ("duration" in entry and "pkt_duration" in entry and
                int(entry["duration"]) != int(entry["pkt_duration"])):
            raise AdmissionError("ffprobe frame duration aliases disagree")
        duration = entry.get("duration", entry.get("pkt_duration"))
        if duration is None:
            raise AdmissionError("ffprobe omitted frame duration")
        normalized_frames.append({
            "pts": int(entry["pts"]), "duration": int(duration),
            "key_frame": int(entry["key_frame"]), "pict_type": entry["pict_type"],
            "interlaced_frame": int(entry["interlaced_frame"]),
            "top_field_first": int(entry["top_field_first"]),
            "repeat_pict": int(entry["repeat_pict"]), "width": int(entry["width"]),
            "height": int(entry["height"]), "pix_fmt": entry["pix_fmt"],
        })
    return selected, normalized_frames


def trace_h264(path):
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "verbose",
               "-i", str(path), "-map", "0:v:0", "-c", "copy",
               "-bsf:v", "trace_headers", "-f", "null", "-"]
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               text=True, encoding="utf-8", errors="replace")
    slices = []
    in_slice = False
    ref_idc = None
    nal_unit_type = None
    packet_pts = None
    sps_max_refs = []
    for line in process.stderr:
        if "Packet:" in line:
            packet_pts = None
            match = re.search(r"\bPacket:.*\bpts\s+(-?\d+),", line)
            if match:
                packet_pts = int(match.group(1))
            continue
        match = re.search(r"\bmax_num_ref_frames\b.*=\s*(\d+)\s*$", line)
        if match:
            sps_max_refs.append(int(match.group(1)))
            continue
        if "Slice Header" in line:
            in_slice = True
            ref_idc = None
            nal_unit_type = None
            continue
        if not in_slice:
            continue
        match = re.search(r"\bnal_ref_idc\b.*=\s*(\d+)\s*$", line)
        if match:
            ref_idc = int(match.group(1))
            continue
        match = re.search(r"\bnal_unit_type\b.*=\s*(\d+)\s*$", line)
        if match:
            nal_unit_type = int(match.group(1))
            continue
        match = re.search(r"\bslice_type\b.*=\s*(\d+)\s*$", line)
        if match:
            if ref_idc is None or nal_unit_type not in (1, 5) or packet_pts is None:
                process.kill()
                process.wait()
                raise AdmissionError("trace_headers omitted slice identity")
            value = int(match.group(1)) % 5
            if value not in (0, 1, 2):
                process.kill()
                process.wait()
                raise AdmissionError("unexpected H.264 slice type")
            slices.append({"type": ("P", "B", "I")[value],
                           "nal_ref_idc": ref_idc,
                           "nal_unit_type": nal_unit_type,
                           "pts": packet_pts})
            in_slice = False
    status = process.wait()
    process.stderr.close()
    if status:
        raise AdmissionError("trace_headers failed")
    if not sps_max_refs or len(set(sps_max_refs)) != 1:
        raise AdmissionError("trace_headers omitted or changed SPS max_num_ref_frames")
    return {"sps_max_num_ref_frames": sps_max_refs[0], "slices": slices}


def decoded_identity(path, frames):
    # Decode only the marker panel. Its paired rows encode each frame number and
    # complement, so neither a repeated nor a reordered picture can look valid.
    payload = run([
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
        "-i", str(path), "-map", "0:v:0", "-vf", "crop=400:64:0:0,format=gray",
        "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
        capture=True)
    stride = 400 * 64
    if len(payload) != frames * stride:
        raise AdmissionError("identity panel frame count drift")
    identities = []
    for frame in range(frames):
        panel = payload[frame * stride:(frame + 1) * stride]
        identity = 0
        for bit in range(IDENTITY_BITS):
            x = 8 + 28 * bit + 8
            one = panel[16 * 400 + x]
            zero = panel[48 * 400 + x]
            if one > 180 and zero < 75:
                identity |= 1 << bit
            elif one < 75 and zero > 180:
                pass
            else:
                raise AdmissionError("decoded visible identity cell is ambiguous")
        if panel[32 * 400 + 360] < 180 or panel[32 * 400 + 384] > 75:
            raise AdmissionError("decoded identity reference cells are corrupt")
        identities.append(identity)
    return identities


def validate_observation(kind, frames_expected, stream, frames, hashes, trace, identities):
    required_stream = {
        "codec_name": "h264", "profile": PROFILE, "level": LEVEL,
        "width": WIDTH, "height": HEIGHT, "pix_fmt": PIX_FMT,
        "field_order": "progressive", "r_frame_rate": RATE,
        "avg_frame_rate": RATE, "time_base": TIME_BASE, "start_pts": 0,
        "duration_ts": frames_expected * PTS_STEP, "nb_frames": frames_expected,
        "color_range": "tv", "color_space": "bt709", "color_transfer": "bt709",
        "color_primaries": "bt709",
    }
    for key, expected in required_stream.items():
        if stream.get(key) != expected:
            raise AdmissionError("%s stream drift: expected %r, got %r" %
                                 (key, expected, stream.get(key)))
    if not isinstance(stream.get("bit_rate"), int) or stream["bit_rate"] <= 0:
        raise AdmissionError("missing measured stream bitrate")
    if not isinstance(stream.get("container_bit_rate"), int) or stream["container_bit_rate"] <= 0:
        raise AdmissionError("missing measured container bitrate")
    if len(frames) != frames_expected:
        raise AdmissionError("decoded frame count drift")
    expected_pts = list(range(0, frames_expected * PTS_STEP, PTS_STEP))
    if [frame["pts"] for frame in frames] != expected_pts:
        raise AdmissionError("display PTS cadence drift")
    if any(frame["duration"] != PTS_STEP for frame in frames):
        raise AdmissionError("frame duration drift")
    if any(frame["width"] != WIDTH or frame["height"] != HEIGHT or
           frame["pix_fmt"] != PIX_FMT for frame in frames):
        raise AdmissionError("decoded geometry or pixel format drift")
    if any(frame["interlaced_frame"] or frame["top_field_first"] or
           frame["repeat_pict"] for frame in frames):
        raise AdmissionError("fixture is not unique progressive pictures")
    keys = [index for index, frame in enumerate(frames) if frame["key_frame"]]
    if keys != list(range(0, frames_expected, GOP)):
        raise AdmissionError("closed one-second GOP drift")
    if len(hashes) != frames_expected or any(row["stream"] != 0 or
                                              row["size"] != FRAME_BYTES for row in hashes):
        raise AdmissionError("full-frame software hash extent drift")
    if ([row["dts"] for row in hashes] != list(range(frames_expected)) or
            [row["pts"] for row in hashes] != list(range(frames_expected)) or
            any(row["duration"] != 1 for row in hashes)):
        raise AdmissionError("software full-frame hash cadence drift")
    full_hashes = [row["sha256"] for row in hashes]
    if len(set(full_hashes)) != frames_expected:
        raise AdmissionError("duplicate decoded full-frame pictures")
    if identities != list(range(frames_expected)):
        raise AdmissionError("visible frame identity is duplicated, missing, or reordered")
    slices = trace.get("slices", [])
    if len(slices) != frames_expected:
        raise AdmissionError("expected one H.264 slice per picture")
    expected_packet_pts = list(range(0, frames_expected * PTS_STEP, PTS_STEP))
    if sorted(entry["pts"] for entry in slices) != expected_packet_pts:
        raise AdmissionError("H.264 packet PTS is missing, duplicate, or outside cadence")
    picture_counts = Counter(frame["pict_type"] for frame in frames)
    slice_counts = Counter(entry["type"] for entry in slices)
    if picture_counts != slice_counts:
        raise AdmissionError("decoder and bitstream picture-type counts disagree")
    decoded_types = {frame["pts"]: frame["pict_type"] for frame in frames}
    traced_types = {entry["pts"]: entry["type"] for entry in slices}
    if traced_types != decoded_types:
        raise AdmissionError("bitstream slice type does not match decoded picture PTS")
    reference_b = sum(entry["type"] == "B" and entry["nal_ref_idc"] > 0
                      for entry in slices)
    reference_b_pts = sorted(entry["pts"] for entry in slices
                             if entry["type"] == "B" and entry["nal_ref_idc"] > 0)
    non_reference_b_pts = sorted(entry["pts"] for entry in slices
                                 if entry["type"] == "B" and entry["nal_ref_idc"] == 0)
    ref_counts = {}
    for entry in slices:
        by_idc = ref_counts.setdefault(entry["type"], {})
        key = str(entry["nal_ref_idc"])
        by_idc[key] = by_idc.get(key, 0) + 1
    gops = frames_expected // GOP
    if kind == "no-b":
        expected_ref_counts = {"I": {"3": gops},
                               "P": {"2": frames_expected - gops}}
    else:
        quarter = frames_expected // 4
        expected_ref_counts = {
            "B": {"0": frames_expected - gops - 2 * quarter, "1": quarter},
            "I": {"3": gops}, "P": {"2": quarter},
        }
    if ref_counts != expected_ref_counts:
        raise AdmissionError("H.264 nal_ref_idc configuration drift")
    expected_max_refs = 4 if kind == "no-b" else 3
    if trace.get("sps_max_num_ref_frames") != expected_max_refs:
        raise AdmissionError("SPS max_num_ref_frames drift")
    idr_pts = sorted(entry["pts"] for entry in slices if entry["nal_unit_type"] == 5)
    expected_idr_pts = list(range(0, frames_expected * PTS_STEP, GOP * PTS_STEP))
    if idr_pts != expected_idr_pts:
        raise AdmissionError("IDR cadence or closed-GOP boundary drift")
    if any((entry["nal_unit_type"] == 5) != (entry["type"] == "I")
           for entry in slices):
        raise AdmissionError("non-IDR I picture or non-I IDR drift")
    if kind == "no-b":
        if stream.get("has_b_frames") != 0 or picture_counts.get("B", 0) or reference_b:
            raise AdmissionError("no-B control contains B pictures")
    elif kind == "b-reference":
        if stream.get("has_b_frames") != 2 or not picture_counts.get("B", 0) or not reference_b:
            raise AdmissionError("B-reference fixture lacks reference B pictures")
    else:
        raise AdmissionError("unknown observation kind")
    return {
        "pts_first": expected_pts[0], "pts_last": expected_pts[-1],
        "pts_step": PTS_STEP, "key_frame_indices": keys,
        "picture_type_counts": dict(sorted(picture_counts.items())),
        "display_picture_types": "".join(frame["pict_type"] for frame in frames),
        "coded_slice_types": "".join(entry["type"] for entry in slices),
        "coded_slice_pts": [entry["pts"] for entry in slices],
        "idr_pts": idr_pts,
        "sps_max_num_ref_frames": trace["sps_max_num_ref_frames"],
        "reference_b_slice_indices": [
            index for index, entry in enumerate(slices)
            if entry["type"] == "B" and entry["nal_ref_idc"] > 0
        ],
        "reference_b_pts": reference_b_pts,
        "non_reference_b_pts": non_reference_b_pts,
        "nal_ref_idc_counts_by_type": ref_counts,
        "reference_b_slice_count": reference_b,
        "unique_full_frame_hashes": len(set(full_hashes)),
        "visible_identity_first": identities[0],
        "visible_identity_last": identities[-1],
    }


def tool_version(tool):
    line = run([tool, "-version"], capture=True).decode("utf-8", "replace").splitlines()[0]
    return line.strip()


def encode_one(directory, kind, frames):
    stem = "true1080p60-" + kind
    media = directory / (stem + ".mp4")
    framehash = directory / (stem + ".framehash")
    options = stable_video_options(kind)
    command = (["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                "-xerror", "-n"] + source_arguments(frames) + options + [str(media)])
    run(command)
    run(framehash_command(["-i", str(media)], framehash))
    stream, decoded = probe(media)
    hashes = parse_framehash(framehash)
    trace = trace_h264(media)
    identities = decoded_identity(media, frames)
    summary = validate_observation(kind, frames, stream, decoded, hashes, trace, identities)
    return {
        "file": media.name, "file_sha256": sha256(media),
        "software_full_frame_hashes": framehash.name,
        "software_full_frame_hashes_sha256": sha256(framehash),
        "requested": requested_options(kind),
        "ffmpeg_video_options": options,
        "observed_stream": stream,
        "observed_frames": summary,
    }


def generate(directory, frames):
    if frames < GOP * 2 or frames > MAX_FRAMES or frames % GOP:
        raise AdmissionError("--frames must be a multiple of 60 from 120 through 4096")
    destination = output_path(directory, create=True)
    destination.mkdir(mode=0o700)
    reference = destination / "source-reference.framehash"
    run(framehash_command(source_arguments(frames), reference))
    reference_rows = parse_framehash(reference)
    validate_source_hashes(reference_rows, frames)
    result = {
        "schema": SCHEMA,
        "tools": {"ffmpeg": tool_version("ffmpeg"), "ffprobe": tool_version("ffprobe")},
        "source": {
            "generator": "testsrc2", "filter": source_filter(),
            "width": WIDTH, "height": HEIGHT, "rate": RATE,
            "time_base": "1/60", "pixel_format": PIX_FMT, "frames": frames,
            "duration_seconds": frames / 60, "progressive": True,
            "identity": "12-bit frame number plus complement and white/black references",
            "identity_range": [0, frames - 1],
            "software_full_frame_hashes": reference.name,
            "software_full_frame_hashes_sha256": sha256(reference),
            "unique_full_frame_hashes": frames,
        },
        "encodes": {},
    }
    for kind in ("no-b", "b-reference"):
        result["encodes"][kind] = encode_one(destination, kind, frames)
    manifest = destination / "manifest.json"
    with manifest.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2, sort_keys=True)
        output.write("\n")
    print(manifest)


def safe_member(directory, name):
    if not isinstance(name, str) or Path(name).name != name:
        raise AdmissionError("manifest file member must be a plain filename")
    path = directory / name
    if not path.is_file() or path.is_symlink():
        raise AdmissionError("manifest member is missing or not a regular file: " + name)
    return path


def frozen_summary(manifest, manifest_sha256):
    summary = {
        "schema": "crystalhd-issue399-frozen-v1",
        "bundle_schema": SCHEMA,
        "bundle_manifest_sha256": manifest_sha256,
        "generator": "tests/issue399-baseline.py",
        "tools": manifest["tools"],
        "source": {
            key: manifest["source"][key] for key in (
                "frames", "width", "height", "rate", "time_base", "pixel_format",
                "progressive", "identity", "identity_range",
                "software_full_frame_hashes_sha256", "unique_full_frame_hashes")
        },
        "encodes": {},
    }
    frame_keys = (
        "pts_first", "pts_last", "pts_step", "key_frame_indices",
        "picture_type_counts", "sps_max_num_ref_frames",
        "nal_ref_idc_counts_by_type", "reference_b_slice_count",
        "unique_full_frame_hashes",
    )
    for kind in ("no-b", "b-reference"):
        encode = manifest["encodes"][kind]
        summary["encodes"][kind] = {
            "file": encode["file"], "file_sha256": encode["file_sha256"],
            "software_full_frame_hashes": encode["software_full_frame_hashes"],
            "software_full_frame_hashes_sha256":
                encode["software_full_frame_hashes_sha256"],
            "requested": encode["requested"],
            "observed_stream": encode["observed_stream"],
            "observed_frames": {key: encode["observed_frames"][key]
                                for key in frame_keys},
        }
    return summary


def verify(directory, frozen_manifest=None):
    root = output_path(directory, create=False)
    manifest_path = safe_member(root, "manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if frozen_manifest is not None:
        frozen_path = Path(frozen_manifest)
        if not frozen_path.is_file() or frozen_path.is_symlink():
            raise AdmissionError("frozen manifest must be a regular file")
        frozen = json.loads(frozen_path.read_text(encoding="utf-8"))
        if not isinstance(frozen, dict):
            raise AdmissionError("frozen manifest must be a JSON object")
        try:
            expected_frozen = (manifest if frozen.get("schema") == SCHEMA else
                               frozen_summary(manifest, sha256(manifest_path)))
        except (KeyError, TypeError) as error:
            raise AdmissionError("bundle cannot be compared with the frozen manifest") from error
        if frozen != expected_frozen:
            raise AdmissionError("generated bundle does not match the frozen baseline manifest")
    if manifest.get("schema") != SCHEMA:
        raise AdmissionError("unknown baseline manifest schema")
    recorded_tools = manifest.get("tools")
    if (not isinstance(recorded_tools, dict) or set(recorded_tools) != {"ffmpeg", "ffprobe"} or
            any(not isinstance(recorded_tools[tool], str) or
                not recorded_tools[tool].startswith(tool + " version ")
                for tool in ("ffmpeg", "ffprobe"))):
        raise AdmissionError("missing or malformed FFmpeg provenance")
    current_tools = {"ffmpeg": tool_version("ffmpeg"),
                     "ffprobe": tool_version("ffprobe")}
    provenance = "MATCH" if recorded_tools == current_tools else "DIFFERENT"
    if provenance == "DIFFERENT":
        print("issue399 baseline: tool-provenance=DIFFERENT", file=sys.stderr)
        for tool in ("ffmpeg", "ffprobe"):
            print("  %s recorded: %s" % (tool, recorded_tools[tool]), file=sys.stderr)
            print("  %s current:  %s" % (tool, current_tools[tool]), file=sys.stderr)
    source = manifest.get("source", {})
    frames = source.get("frames")
    if (not isinstance(frames, int) or frames < GOP * 2 or frames > MAX_FRAMES or
            frames % GOP or source.get("filter") != source_filter()):
        raise AdmissionError("source baseline configuration drift")
    expected_source = {
        "generator": "testsrc2", "width": WIDTH, "height": HEIGHT,
        "rate": RATE, "time_base": "1/60", "pixel_format": PIX_FMT,
        "duration_seconds": frames / 60, "progressive": True,
        "identity": "12-bit frame number plus complement and white/black references",
        "identity_range": [0, frames - 1], "unique_full_frame_hashes": frames,
    }
    for key, expected in expected_source.items():
        if source.get(key) != expected:
            raise AdmissionError("source %s differs from the recorded baseline" % key)
    reference = safe_member(root, source.get("software_full_frame_hashes"))
    if sha256(reference) != source.get("software_full_frame_hashes_sha256"):
        raise AdmissionError("source reference hash file changed")
    saved_reference = parse_framehash(reference)
    current_reference = parse_framehash(run(framehash_command(source_arguments(frames)), capture=True))
    if saved_reference != current_reference:
        raise AdmissionError("regenerated source full-frame hashes differ")
    validate_source_hashes(saved_reference, frames)
    encodes = manifest.get("encodes", {})
    if set(encodes) != {"no-b", "b-reference"}:
        raise AdmissionError("manifest must contain both issue #399 controls")
    for kind in ("no-b", "b-reference"):
        saved = encodes[kind]
        media = safe_member(root, saved.get("file"))
        framehash = safe_member(root, saved.get("software_full_frame_hashes"))
        if sha256(media) != saved.get("file_sha256"):
            raise AdmissionError(kind + " media file changed")
        if sha256(framehash) != saved.get("software_full_frame_hashes_sha256"):
            raise AdmissionError(kind + " framehash file changed")
        if saved.get("requested") != requested_options(kind):
            raise AdmissionError(kind + " requested encoder settings drift")
        if saved.get("ffmpeg_video_options") != stable_video_options(kind):
            raise AdmissionError(kind + " encoder configuration drift")
        stream, decoded = probe(media)
        if stream != saved.get("observed_stream"):
            raise AdmissionError(kind + " ffprobe observation differs from manifest")
        saved_hashes = parse_framehash(framehash)
        current_hashes = parse_framehash(run(framehash_command(["-i", str(media)]), capture=True))
        if saved_hashes != current_hashes:
            raise AdmissionError(kind + " regenerated software hashes differ")
        summary = validate_observation(kind, frames, stream, decoded, current_hashes,
                                       trace_h264(media), decoded_identity(media, frames))
        if summary != saved.get("observed_frames"):
            raise AdmissionError(kind + " frame observation differs from manifest")
    print("issue399 baseline: PASS frames=%d rate=60/1 geometry=1920x1080 "
          "tool-provenance=%s" % (frames, provenance))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    generate_parser = subparsers.add_parser("generate")
    generate_parser.add_argument("--output", required=True,
                                 help="new absolute directory outside the source tree")
    generate_parser.add_argument("--frames", type=int, required=True,
                                 help="multiple of 60, at least 120")
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--output", required=True,
                               help="existing absolute generated-result directory")
    verify_parser.add_argument("--frozen-manifest",
                               help="optional path to an exact path-free frozen manifest")
    arguments = parser.parse_args()
    try:
        if arguments.action == "generate":
            generate(arguments.output, arguments.frames)
        else:
            verify(arguments.output, arguments.frozen_manifest)
    except (AdmissionError, json.JSONDecodeError, OSError) as error:
        print("issue399 baseline: FAIL: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
