#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Device-free admission checks for vaapi-seek-test frame boundaries."""

import argparse
import pathlib
import subprocess
import tempfile


class TestFailure(RuntimeError):
    pass


def invoke(arguments, timeout=180):
    return subprocess.run(
        [str(argument) for argument in arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )


def require_failure(arguments, fragment):
    result = invoke(arguments, timeout=30)
    if result.returncode == 0:
        raise TestFailure(f"command unexpectedly succeeded: {' '.join(map(str, arguments))}")
    if fragment not in result.stderr:
        raise TestFailure(
            f"missing rejection {fragment!r}: stdout={result.stdout!r}, "
            f"stderr={result.stderr!r}"
        )
    if "reference:" in result.stdout:
        raise TestFailure("rejected command unexpectedly decoded a reference")


def require_success(arguments):
    result = invoke(arguments)
    if result.returncode != 0:
        raise TestFailure(
            f"command failed ({result.returncode}): {' '.join(map(str, arguments))}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("seek_probe", type=pathlib.Path)
    parser.add_argument("declared_fixture", type=pathlib.Path)
    parser.add_argument("expected_frames", type=int)
    options = parser.parse_args()

    seek_probe = options.seek_probe.resolve(strict=True)
    declared_fixture = options.declared_fixture.resolve(strict=True)
    expected = options.expected_frames
    if expected < 80:
        parser.error("expected_frames must be at least 80 for retained-frame coverage")

    passed = 0
    with tempfile.TemporaryDirectory(prefix="crystalhd-vaapi-seek-") as directory:
        temporary = pathlib.Path(directory)
        unopened = temporary / "parser-must-not-open-input"
        invalid = (
            (["--expected-frames"], "must be specified once"),
            (["--expected-frames", "0"], "requires a positive integer"),
            (["--expected-frames", "-1"], "requires a positive integer"),
            (["--expected-frames", "12x"], "requires a positive integer"),
            (["--expected-frames", "9" * 100], "is out of range"),
            (["--expected-frames", str(expected),
              "--expected-frames", str(expected)], "must be specified once"),
        )
        for arguments, fragment in invalid:
            require_failure([seek_probe, unopened, "--software", *arguments], fragment)
            passed += 1

        mismatch = expected - 1
        require_failure(
            [seek_probe, declared_fixture, "--software", "--expected-frames", mismatch],
            "conflicts with the container's declared frame count",
        )
        passed += 1

        output = require_success(
            [seek_probe, declared_fixture, "--software", "--expected-frames", expected]
        )
        if f"reference: {expected} software frames drained" not in output:
            raise TestFailure("matching declared boundary did not complete")
        passed += 1

        # Matroska preserves the generated sample's exact packet timestamps but
        # omits nb_frames, giving the option a deterministic device-free path.
        no_count = temporary / "no-declared-count.mkv"
        subprocess.run(
            ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
             "-n", "-i", declared_fixture, "-map", "0:v:0", "-c:v", "copy",
             "-f", "matroska", no_count],
            check=True,
        )
        require_failure(
            [seek_probe, no_count, "--software"],
            "container has no declared frame count",
        )
        passed += 1
        require_failure(
            [seek_probe, no_count, "--software", "--expected-frames", mismatch],
            "reference did not drain the expected frame count",
        )
        passed += 1

        output = require_success(
            [seek_probe, no_count, "--software", "--expected-frames", expected,
             "--retain-old-frames"]
        )
        required = {
            f"reference: {expected} software frames drained": 1,
            "frame SHA-256 digests match": 4,
            "flush and new input: 8 retained old-frame PTS/SHA-256 digests match": 2,
            "decoder context teardown: 8 retained old-frame PTS/SHA-256 digests match": 2,
        }
        for line, occurrences in required.items():
            if output.count(line) != occurrences:
                raise TestFailure(
                    f"retained-frame self-check expected {occurrences} occurrences "
                    f"of {line!r}, got {output.count(line)}"
                )
        passed += 1

        # Keep the same H.264 access units and expected presentation targets.
        # TS binary seeking needs the reference key packet's DTS to include
        # the IDR before reordered pictures; decoding/pixel criteria stay exact.
        transport = temporary / "decode-timestamp-seek.ts"
        subprocess.run(
            ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
             "-n", "-i", declared_fixture, "-map", "0:v:0", "-c:v", "copy",
             "-bsf:v", "h264_mp4toannexb", "-f", "mpegts", transport],
            check=True,
        )
        output = require_success(
            [seek_probe, transport, "--software", "--expected-frames", expected,
             "--retain-old-frames"]
        )
        for line, occurrences in required.items():
            if output.count(line) != occurrences:
                raise TestFailure(
                    f"TS retained-frame self-check expected {occurrences} "
                    f"occurrences of {line!r}, got {output.count(line)}"
                )
        if output.count("TS seek target PTS ") != 6:
            raise TestFailure("TS seeks did not retain all six reference key anchors")
        passed += 1

    print(f"vaapi seek boundary/self-check: {passed}/{passed} passed")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.SubprocessError, TestFailure) as error:
        raise SystemExit(f"vaapi seek boundary/self-check failed: {error}") from error
