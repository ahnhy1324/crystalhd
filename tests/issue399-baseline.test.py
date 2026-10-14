#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Focused drift and end-to-end tests for the issue #399 baseline."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("issue399_baseline", HERE / "issue399-baseline.py")
BASELINE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASELINE)


def observation(kind="no-b", frames=120):
    stream = {
        "codec_name": "h264", "profile": "High", "level": 42,
        "width": 1920, "height": 1080, "has_b_frames": 0,
        "pix_fmt": "yuv420p", "color_range": "tv", "color_space": "bt709",
        "color_transfer": "bt709", "color_primaries": "bt709",
        "field_order": "progressive", "r_frame_rate": "60/1",
        "avg_frame_rate": "60/1", "time_base": "1/60000", "start_pts": 0,
        "duration_ts": frames * 1000, "bit_rate": 12_000_000,
        "nb_frames": frames, "container_size": 3_000_000,
        "container_bit_rate": 12_010_000,
    }
    decoded = []
    hashes = []
    slices = []
    for index in range(frames):
        within_gop = index % 60
        key = within_gop == 0
        picture = "I" if key else "P"
        if kind == "b-reference" and not key:
            picture = "P" if within_gop % 4 == 0 or within_gop == 59 else "B"
        decoded.append({
            "pts": index * 1000, "duration": 1000, "key_frame": int(key),
            "pict_type": picture, "interlaced_frame": 0, "top_field_first": 0,
            "repeat_pict": 0, "width": 1920, "height": 1080,
            "pix_fmt": "yuv420p",
        })
        hashes.append({"stream": 0, "dts": index, "pts": index, "duration": 1,
                       "size": BASELINE.FRAME_BYTES, "sha256": "%064x" % (index + 1)})
        ref_idc = (3 if picture == "I" else 2 if picture == "P" else
                   int(within_gop % 4 == 2))
        slices.append({"type": picture,
                       "nal_ref_idc": ref_idc,
                       "nal_unit_type": 5 if key else 1,
                       "pts": index * 1000})
    if kind == "b-reference":
        stream["has_b_frames"] = 2
    trace = {"sps_max_num_ref_frames": 4 if kind == "no-b" else 3,
             "slices": slices}
    return stream, decoded, hashes, trace, list(range(frames))


class DriftTests(unittest.TestCase):
    def validate(self, kind="no-b", values=None):
        values = values or observation(kind)
        return BASELINE.validate_observation(kind, 120, *values)

    def assert_rejected(self, mutate, kind="no-b"):
        values = list(copy.deepcopy(observation(kind)))
        mutate(values)
        with self.assertRaises(BASELINE.AdmissionError):
            self.validate(kind, values)

    def test_controls_pass_and_record_exact_structure(self):
        no_b = self.validate()
        b_ref = self.validate("b-reference")
        self.assertEqual(no_b["key_frame_indices"], [0, 60])
        self.assertEqual(no_b["picture_type_counts"], {"I": 2, "P": 118})
        self.assertEqual(no_b["sps_max_num_ref_frames"], 4)
        self.assertEqual(no_b["nal_ref_idc_counts_by_type"],
                         {"I": {"3": 2}, "P": {"2": 118}})
        self.assertEqual(b_ref["sps_max_num_ref_frames"], 3)
        self.assertEqual(b_ref["nal_ref_idc_counts_by_type"],
                         {"I": {"3": 2}, "P": {"2": 30},
                          "B": {"0": 58, "1": 30}})
        self.assertEqual(b_ref["idr_pts"], [0, 60_000])
        self.assertGreater(b_ref["reference_b_slice_count"], 0)
        self.assertEqual(b_ref["unique_full_frame_hashes"], 120)

    def test_duplicate_full_frame_and_identity_are_rejected(self):
        self.assert_rejected(lambda values: values[2].__setitem__(-1, values[2][-2]))
        self.assert_rejected(lambda values: values[4].__setitem__(-1, values[4][-2]))

    def test_source_reference_duplicate_and_cadence_are_rejected(self):
        rows = observation()[2]
        BASELINE.validate_source_hashes(rows, 120)
        duplicate = copy.deepcopy(rows)
        duplicate[-1]["sha256"] = duplicate[-2]["sha256"]
        with self.assertRaises(BASELINE.AdmissionError):
            BASELINE.validate_source_hashes(duplicate, 120)
        bad_pts = copy.deepcopy(rows)
        bad_pts[60]["pts"] = 59
        with self.assertRaises(BASELINE.AdmissionError):
            BASELINE.validate_source_hashes(bad_pts, 120)

    def test_fractional_or_slow_rate_and_pts_drift_are_rejected(self):
        self.assert_rejected(lambda values: values[0].__setitem__("r_frame_rate", "60000/1001"))
        self.assert_rejected(lambda values: values[0].__setitem__("avg_frame_rate", "30/1"))
        self.assert_rejected(lambda values: values[1][61].__setitem__("pts", 60_500))

    def test_geometry_pixel_format_and_interlace_drift_are_rejected(self):
        self.assert_rejected(lambda values: values[0].__setitem__("width", 1280))
        self.assert_rejected(lambda values: values[1][9].__setitem__("height", 1088))
        self.assert_rejected(lambda values: values[1][9].__setitem__("pix_fmt", "yuv422p"))
        self.assert_rejected(lambda values: values[1][9].__setitem__("interlaced_frame", 1))

    def test_gop_and_b_reference_drift_are_rejected(self):
        self.assert_rejected(lambda values: values[1][60].__setitem__("key_frame", 0))
        self.assert_rejected(lambda values: values[1][59].__setitem__("key_frame", 1))
        self.assert_rejected(
            lambda values: [entry.__setitem__("nal_ref_idc", 0)
                            for entry in values[3]["slices"] if entry["type"] == "B"],
            "b-reference")
        self.assert_rejected(
            lambda values: values[3].__setitem__("sps_max_num_ref_frames", 4),
            "b-reference")
        self.assert_rejected(
            lambda values: values[3]["slices"][60].__setitem__("nal_unit_type", 1))
        self.assert_rejected(
            lambda values: values[3]["slices"][61].__setitem__("pts", 60_000))
        self.assert_rejected(
            lambda values: values[3]["slices"][1].__setitem__("nal_ref_idc", 0))
        def swap_slice_types(values):
            slices = values[3]["slices"]
            slices[1]["type"], slices[4]["type"] = slices[4]["type"], slices[1]["type"]
        self.assert_rejected(swap_slice_types, "b-reference")

    def test_output_location_is_explicit_and_external(self):
        with self.assertRaises(BASELINE.AdmissionError):
            BASELINE.output_path("relative/results", create=True)
        with self.assertRaises(BASELINE.AdmissionError):
            BASELINE.output_path(str(BASELINE.REPO / "generated"), create=True)


@unittest.skipUnless(os.environ.get("CRYSTALHD_ISSUE399_FFMPEG") == "1",
                     "set CRYSTALHD_ISSUE399_FFMPEG=1 for the full software pipeline")
class EndToEndTests(unittest.TestCase):
    def test_generate_verify_and_refuse_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="crystalhd-issue399-test.", dir="/tmp") as parent:
            result = Path(parent) / "baseline-a"
            duplicate = Path(parent) / "baseline-b"
            BASELINE.generate(str(result), 120)
            BASELINE.generate(str(duplicate), 120)
            names = (
                "manifest.json", "source-reference.framehash",
                "true1080p60-no-b.mp4", "true1080p60-no-b.framehash",
                "true1080p60-b-reference.mp4", "true1080p60-b-reference.framehash",
            )
            for name in names:
                self.assertEqual(BASELINE.sha256(result / name),
                                 BASELINE.sha256(duplicate / name), name)
            manifest = (result / "manifest.json").read_text(encoding="utf-8")
            self.assertNotIn(str(result), manifest)
            self.assertNotIn(str(Path.home()), manifest)
            frozen = Path(parent) / "frozen.json"
            frozen_payload = BASELINE.frozen_summary(
                json.loads(manifest), BASELINE.sha256(result / "manifest.json"))
            frozen.write_text(json.dumps(frozen_payload), encoding="utf-8")
            BASELINE.verify(str(result), str(frozen))
            frozen_payload["bundle_manifest_sha256"] = "0" * 64
            frozen.write_text(json.dumps(frozen_payload), encoding="utf-8")
            with self.assertRaises(BASELINE.AdmissionError):
                BASELINE.verify(str(result), str(frozen))
            with self.assertRaises(BASELINE.AdmissionError):
                BASELINE.generate(str(result), 120)


if __name__ == "__main__":
    unittest.main()
