#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Device-free admission tests for the frozen VC-1 seek oracle."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "vc1_seek_oracle", HERE / "vc1-seek-oracle.py")
ORACLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ORACLE)


def stream_probe():
    return {
        "streams": [{
            "codec_type": "video", "codec_name": "vc1",
            "profile": "Advanced", "level": 3, "width": 1920,
            "height": 1080, "coded_width": 1920, "coded_height": 1080,
            "has_b_frames": 1, "pix_fmt": "yuv420p",
            "color_space": "bt709", "color_transfer": "bt709",
            "color_primaries": "bt709", "chroma_location": "left",
            "field_order": "progressive", "r_frame_rate": "24000/1001",
            "avg_frame_rate": "24000/1001", "time_base": "1/1200000",
            "start_pts": 0, "duration_ts": 3003000,
            "extradata_size": 33, "nb_frames": "60",
        }],
        "format": {"format_name": "mov,mp4,m4a,3gp,3g2,mj2"},
    }


class FrameReferenceTests(unittest.TestCase):
    def test_frozen_reference_has_exact_pts_and_pixel_extent(self):
        payload = ORACLE.REFERENCE.read_bytes()
        rows = ORACLE.parse_framehash(payload)
        self.assertEqual(len(rows), 60)
        self.assertEqual([row[2] for row in rows],
                         [0, 1] + list(range(3, 61)))
        self.assertEqual(len({row[5] for row in rows}), 58)
        self.assertEqual(ORACLE.sha256_bytes(payload),
                         "9ba1afd8a22621dad35418f3f952c24637d52db2c03ce3899b1f03fa758519be")

    def assert_rejected(self, mutate):
        lines = ORACLE.REFERENCE.read_text(encoding="utf-8").splitlines()
        mutate(lines)
        with self.assertRaises(ORACLE.AdmissionError):
            ORACLE.parse_framehash("\n".join(lines) + "\n")

    def test_header_pts_extent_and_hash_drift_are_rejected(self):
        self.assert_rejected(lambda lines: lines.__setitem__(3, "#tb 0: 1/25"))
        self.assert_rejected(
            lambda lines: lines.__setitem__(11, lines[11].replace(
                ",          3,", ",          2,", 1)))
        self.assert_rejected(
            lambda lines: lines.__setitem__(20, lines[20].replace(
                "3110400", "3110399")))
        self.assert_rejected(
            lambda lines: lines.__setitem__(30, lines[30][:-1] + "z"))


class ContainerAdmissionTests(unittest.TestCase):
    def call_probe(self, payload):
        encoded = json.dumps(payload).encode("utf-8")
        with mock.patch.object(ORACLE, "run", return_value=encoded):
            ORACLE.probe_container(Path("fixture.mov"))

    def test_exact_stream_is_admitted(self):
        self.call_probe(stream_probe())

    def test_near_miss_streams_are_rejected(self):
        for key, value in (("codec_name", "wmv3"), ("profile", "Main"),
                           ("width", 1918), ("field_order", "tt"),
                           ("r_frame_rate", "25/1"), ("nb_frames", "59"),
                           ("extradata_size", 32)):
            payload = copy.deepcopy(stream_probe())
            payload["streams"][0][key] = value
            with self.assertRaises(ORACLE.AdmissionError, msg=key):
                self.call_probe(payload)

    def test_payload_bytes_and_packet_boundaries_are_both_required(self):
        source = b"frozen"
        digest = ORACLE.sha256_bytes
        with mock.patch.object(ORACLE, "SOURCE_SHA256", digest(source)):
            ORACLE.validate_payload_identity(source, source, [2, 4], [2, 4])
            with self.assertRaises(ORACLE.AdmissionError):
                ORACLE.validate_payload_identity(source, b"frozeN", [2, 4], [2, 4])
            with self.assertRaises(ORACLE.AdmissionError):
                ORACLE.validate_payload_identity(source, source, [2, 4], [3, 3])

    def test_full_resolution_decoded_pts_are_required(self):
        frames = [{"best_effort_timestamp": str(pts),
                   "duration": str(ORACLE.PTS_STEP)}
                  for pts in ORACLE.EXPECTED_FRAME_PTS]
        encoded = json.dumps({"frames": frames}).encode("utf-8")
        with mock.patch.object(ORACLE, "run", return_value=encoded):
            self.assertEqual(len(ORACLE.decoded_timestamps(Path("fixture.mov"))), 60)
        frames[20]["best_effort_timestamp"] = str(
            int(frames[20]["best_effort_timestamp"]) + 1)
        encoded = json.dumps({"frames": frames}).encode("utf-8")
        with mock.patch.object(ORACLE, "run", return_value=encoded):
            with self.assertRaises(ORACLE.AdmissionError):
                ORACLE.decoded_timestamps(Path("fixture.mov"))


class PathAdmissionTests(unittest.TestCase):
    def test_output_must_be_new_absolute_and_external(self):
        with tempfile.TemporaryDirectory(prefix="crystalhd-vc1-path.",
                                         dir="/tmp") as parent:
            target = Path(parent) / "seek.mov"
            self.assertEqual(ORACLE.output_path(str(target), True),
                             target.resolve())
            target.touch()
            with self.assertRaises(ORACLE.AdmissionError):
                ORACLE.output_path(str(target), True)
        with self.assertRaises(ORACLE.AdmissionError):
            ORACLE.output_path("relative.mov", True)
        with self.assertRaises(ORACLE.AdmissionError):
            ORACLE.output_path(str(ORACLE.REPO / "seek.mov"), True)

    def test_publish_is_atomic_and_never_replaces_a_competing_target(self):
        with tempfile.TemporaryDirectory(prefix="crystalhd-vc1-publish.",
                                         dir="/tmp") as parent:
            candidate = Path(parent) / "candidate.mov"
            target = Path(parent) / "target.mov"
            candidate.write_bytes(b"verified")
            target.write_bytes(b"competing")
            with self.assertRaises(ORACLE.AdmissionError):
                ORACLE.publish(candidate, target)
            self.assertEqual(target.read_bytes(), b"competing")
            target.unlink()
            ORACLE.publish(candidate, target)
            self.assertEqual(target.read_bytes(), b"verified")
            self.assertEqual(candidate.stat().st_ino, target.stat().st_ino)


@unittest.skipUnless(os.environ.get("CRYSTALHD_VC1_SEEK_FIXTURE"),
                     "set CRYSTALHD_VC1_SEEK_FIXTURE for the full remux test")
class EndToEndTests(unittest.TestCase):
    def test_two_builds_are_identical_and_verify(self):
        source = os.environ["CRYSTALHD_VC1_SEEK_FIXTURE"]
        with tempfile.TemporaryDirectory(prefix="crystalhd-vc1-e2e.",
                                         dir="/tmp") as parent:
            first = Path(parent) / "first.mov"
            second = Path(parent) / "second.mov"
            one = ORACLE.build(source, str(first))
            two = ORACLE.build(source, str(second))
            self.assertEqual(one["container_sha256"], two["container_sha256"])
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(ORACLE.verify(source, str(first))["decoded_frames"], 60)
            shifted = Path(parent) / "shifted.mov"
            ORACLE.run([
                "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                "-xerror", "-n", "-i", str(first), "-map", "0:v:0", "-c:v",
                "copy", "-bsf:v", "setts=pts=PTS+eq(N\\,20)",
                "-video_track_timescale", "1200000", str(shifted),
            ])
            with self.assertRaises(ORACLE.AdmissionError):
                ORACLE.verify(source, str(shifted))


if __name__ == "__main__":
    unittest.main()
