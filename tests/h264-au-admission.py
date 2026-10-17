#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepare an offline oracle, then explicitly run a bounded #442 diagnostic.

The native phase measures only a conservative admission policy, not the physical
maximum. It never sends an assembled access unit larger than 2 MiB, never loads
a module or installs components, and stops at the first error or pixel mismatch.
The native command requires a supervising hardware owner with external AC,
temperature, kernel-log and device-ownership guards. On a native timeout it
reports the child PID without signaling it; recovery belongs to that owner.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys


MAX_ASSEMBLED = 2 * 1024 * 1024
FRAMES = 75
TARGET_FRAME = 25
FOLLOWING_IDR = 50
START_CODE = re.compile(b"\x00\x00\x00\x01|\x00\x00\x01")
SUBMIT = re.compile(r"submit surface=\d+ bytes=(\d+) slices=(\d+)")


def run(command, log, env=None, native_process=False):
    """Bound the observation; the hardware owner controls native recovery."""
    with log.open("xb") as output:
        process = subprocess.Popen(command, stdout=output, stderr=output,
                                   env=env, start_new_session=True)
        try:
            result = process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            if native_process:
                raise RuntimeError(
                    f"native timeout; stopped diagnostic, PID {process.pid} "
                    f"requires supervising-owner recovery; log: {log}")
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise RuntimeError(f"timed out; stopped diagnostic: {log}")
    if result != 0:
        raise RuntimeError(f"command exited {result}; stopped diagnostic: {log}")


def nals(data):
    starts = list(START_CODE.finditer(data))
    if not starts or starts[0].start() != 0:
        raise ValueError("expected an Annex-B stream beginning at a start code")
    return [(match.start(), match.end(),
             starts[index + 1].start() if index + 1 < len(starts) else len(data))
            for index, match in enumerate(starts)]


def frame_slices(data):
    frames = []
    for start, payload, end in nals(data):
        kind = data[payload] & 31
        if kind == 9:
            frames.append([])
        elif kind in (1, 5):
            if not frames:
                raise ValueError("slice before first access-unit delimiter")
            frames[-1].append((start, payload, end, kind))
    if len(frames) != FRAMES or any(len(frame) != 1 for frame in frames):
        raise ValueError("expected 75 AUD-delimited pictures with one slice each")
    if [i for i, frame in enumerate(frames) if frame[0][3] == 5] != [0, 25, 50]:
        raise ValueError("expected exactly the IDRs at pictures 0, 25 and 50")
    return [frame[0] for frame in frames]


def inflate(data, assembled_bytes, overhead):
    if not 0 < assembled_bytes <= MAX_ASSEMBLED:
        raise ValueError("assembled diagnostic size must be at most 2 MiB")
    _, payload, end, _ = frame_slices(data)[TARGET_FRAME]
    padding = assembled_bytes - overhead - (end - payload)
    if overhead <= 0 or padding < 0:
        raise ValueError("invalid reconstructed-AU overhead or target size")
    # cabac_zero_word padding with emulation prevention. A final one/two zero
    # bytes cover the other exact-size residues and remain trailing NAL bytes.
    extra = b"\x00\x00\x03" * (padding // 3) + b"\x00" * (padding % 3)
    return data[:end] + extra + data[end:]


def pixels(path):
    rows = [line.split(",") for line in path.read_text().splitlines()
            if line and not line.startswith("#")]
    if len(rows) != FRAMES or any(len(row) != 6 for row in rows):
        raise ValueError(f"expected exactly 75 consumed picture hashes: {path}")
    # Raw Annex-B timestamp labels are reconstructed by the demuxer; compare
    # ordered payload sizes and hashes, including all pictures after IDR 50.
    return [(int(row[4]), row[5].strip()) for row in rows]


def check_pixels(actual, expected):
    observed = pixels(actual)
    reference = pixels(expected)
    if observed != reference:
        mismatch = next(i for i, pair in enumerate(zip(observed, reference))
                        if pair[0] != pair[1])
        raise ValueError(f"picture {mismatch} differs; stopped diagnostic")
    if observed[FOLLOWING_IDR:] != reference[FOLLOWING_IDR:]:
        raise ValueError("following-IDR content does not match")


def check_driver_log(text):
    if any(marker in text.lower() for marker in (
            "sealed-batch decoding failed:",
            "eos arrived before every submitted timestamp completed",
            "sealed-batch sync timed out")):
        raise ValueError("driver reported incomplete decoding; stopped diagnostic")


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def decode_command(source, output, native=False, drm_device=None):
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
               "-xerror", "-threads", "2"]
    if native:
        command += ["-hwaccel", "vaapi", "-hwaccel_device", drm_device,
                    "-hwaccel_output_format", "vaapi"]
    else:
        command += ["-hwaccel", "none"]
    command += ["-i", str(source), "-an", "-vf",
                ("hwdownload,format=nv12,format=yuv420p,extractplanes=y"
                 if native else "extractplanes=y"),
                "-fps_mode", "passthrough", "-f", "framemd5", "-n", str(output)]
    return command


def prepare(directory):
    directory.mkdir(parents=True, exist_ok=False)
    base = directory / "base.h264"
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-n",
               "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25",
               "-frames:v", str(FRAMES), "-an", "-c:v", "libx264",
               "-threads", "2", "-preset", "veryfast", "-crf", "24",
               "-profile:v", "high", "-level:v", "4.0", "-pix_fmt", "yuv420p",
               "-x264-params", "aud=1:repeat-headers=1:keyint=25:min-keyint=25:"
               "scenecut=0:bframes=0:ref=1:slices=1", "-f", "h264", str(base)]
    run(command, directory / "encode.log")
    slices = frame_slices(base.read_bytes())
    oracle = directory / "software.framemd5"
    run(decode_command(base, oracle), directory / "software.log")
    pixels(oracle)
    (directory / "prepared.json").write_text(json.dumps({
        "scope": "offline only; native has not run",
        "base_sha256": sha256(base), "software_sha256": sha256(oracle),
        "frames": FRAMES, "idr_indices": [0, 25, 50],
        "picture25_slice_bytes": slices[TARGET_FRAME][2] - slices[TARGET_FRAME][1],
        "maximum_native_assembled_bytes": MAX_ASSEMBLED,
    }, indent=2) + "\n")
    print(f"Prepared offline oracle: {directory}")


def native(directory, driver_directory, library_directory, drm_device):
    manifest = json.loads((directory / "prepared.json").read_text())
    base = directory / "base.h264"
    oracle = directory / "software.framemd5"
    if sha256(base) != manifest["base_sha256"] or sha256(oracle) != manifest["software_sha256"]:
        raise ValueError("prepared fixture/oracle identity changed")
    refcount = Path("/sys/module/crystalhd/refcnt")
    if refcount.read_text().strip() != "0":
        raise ValueError("CrystalHD module is not idle; diagnostic not started")
    output = directory / "native"
    output.mkdir(exist_ok=False)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("CRYSTALHD_VAAPI_")}
    environment.update(LIBVA_DRIVER_NAME="crystalhd",
                       LIBVA_DRIVERS_PATH=str(driver_directory.resolve()),
                       LD_LIBRARY_PATH=str(library_directory.resolve()),
                       CRYSTALHD_DRM_DEVICE=drm_device,
                       CRYSTALHD_VAAPI_DEBUG="1")
    results = []

    def decode(source, label, target=None):
        if refcount.read_text().strip() != "0":
            raise ValueError("module refcount did not return to zero; stopped diagnostic")
        hashes = output / f"{label}.framemd5"
        log = output / f"{label}.log"
        run(decode_command(source, hashes, True, drm_device), log, environment,
            native_process=True)
        check_pixels(hashes, oracle)
        text = log.read_text(errors="replace")
        sizes = [(int(size), int(count)) for size, count in SUBMIT.findall(text)]
        if len(sizes) != FRAMES or any(count != 1 for _, count in sizes):
            raise ValueError("cannot establish exactly 75 single-slice submissions")
        if max(size for size, _ in sizes) > MAX_ASSEMBLED:
            raise ValueError("unexpected assembled size above diagnostic ceiling")
        if target is not None and sizes[TARGET_FRAME][0] != target:
            raise ValueError("assembled size differs from calibrated target")
        check_driver_log(text)
        if refcount.read_text().strip() != "0":
            raise ValueError("module refcount did not return to zero; stopped diagnostic")
        result = {"label": label, "fixture_sha256": sha256(source),
                  "picture25_assembled_bytes": sizes[TARGET_FRAME][0],
                  "consumed_luma_frames": FRAMES, "following_idr_frames": 25,
                  "software_luma_equal": True, "module_refcount_after": 0}
        results.append(result)
        (output / "results.json").write_text(json.dumps({
            "scope": "conservative 2 MiB admission diagnostic, not physical maximum",
            "results": results,
        }, indent=2) + "\n")
        print(json.dumps(result), flush=True)
        return sizes

    data = base.read_bytes()
    sizes = decode(base, "baseline")
    overhead = sizes[TARGET_FRAME][0] - manifest["picture25_slice_bytes"]
    if not 0 < overhead <= 4096:
        raise ValueError("unexpected reconstructed header overhead; diagnostic stopped")
    for target in (524287, 524288, 524289, 1048576, 1572864, MAX_ASSEMBLED):
        fixture = output / f"assembled-{target}.h264"
        fixture.write_bytes(inflate(data, target, overhead))
        software = output / f"assembled-{target}.software.framemd5"
        run(decode_command(fixture, software), output / f"assembled-{target}.software.log")
        check_pixels(software, oracle)
        decode(fixture, f"assembled-{target}", target)
    decode(base, "reopen-baseline")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    offline = commands.add_parser("prepare", help="software only; never accesses hardware")
    offline.add_argument("directory", type=Path)
    hardware = commands.add_parser("native", help="explicit physical BCM70015 diagnostic")
    hardware.add_argument("directory", type=Path)
    hardware.add_argument("--driver-directory", type=Path, required=True)
    hardware.add_argument("--library-directory", type=Path, required=True)
    hardware.add_argument("--drm-device", default="/dev/dri/renderD128")
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            prepare(args.directory)
        else:
            native(args.directory, args.driver_directory, args.library_directory, args.drm_device)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
