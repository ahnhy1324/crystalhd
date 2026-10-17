#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Device-free invariants of the bounded #442 fixture generator."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "au_admission", Path(__file__).with_name("h264-au-admission.py"))
admission = importlib.util.module_from_spec(spec)
spec.loader.exec_module(admission)


def stream():
    return b"".join(b"\x00\x00\x00\x01\x09\x10" +
                    b"\x00\x00\x01" +
                    (b"\x65" if index in (0, 25, 50) else b"\x41") +
                    bytes([index + 1]) * 8
                    for index in range(75))


class FixtureTests(unittest.TestCase):
    def test_normal_seal_progress_is_not_an_eos_failure(self):
        admission.check_driver_log(
            "seal batch on sync demand: 1 hardware timestamps outstanding\n"
            "sealed batch EOS: every submitted timestamp completed\n")
        with self.assertRaisesRegex(ValueError, "incomplete decoding"):
            admission.check_driver_log(
                "sealed-batch decoding failed: EOS arrived before every "
                "submitted timestamp completed")

    def test_only_target_picture_changes_and_size_stays_bounded(self):
        original = stream()
        start, payload, end, _ = admission.frame_slices(original)[25]
        for residue in range(3):
            target = 524287 + residue
            inflated = admission.inflate(original, target, 43)
            delta = target - 43 - (end - payload)
            self.assertEqual(inflated[:end], original[:end])
            self.assertEqual(inflated[end + delta:], original[end:])
            self.assertEqual(len(inflated) - len(original), delta)
            following = admission.frame_slices(inflated)[50]
            previous = admission.frame_slices(original)[50]
            self.assertEqual(inflated[following[0]:following[2]],
                             original[previous[0]:previous[2]])

    def test_rejects_unbounded_or_wrong_shape_inputs(self):
        for target in (0, -1, admission.MAX_ASSEMBLED + 1):
            with self.assertRaises(ValueError):
                admission.inflate(stream(), target, 43)
        with self.assertRaises(ValueError):
            admission.inflate(stream(), 512, 0)
        with self.assertRaises(ValueError):
            admission.frame_slices(stream()[:-20])

    def test_content_check_rejects_counts_and_following_idr_corruption(self):
        rows = [f"0, {i}, {i}, 1, 2073600, {i:032x}\n" for i in range(75)]
        with tempfile.TemporaryDirectory() as temporary:
            reference = Path(temporary) / "reference.framemd5"
            actual = Path(temporary) / "actual.framemd5"
            reference.write_text("#header\n" + "".join(rows))
            actual.write_text("".join(rows))
            admission.check_pixels(actual, reference)
            actual.write_text("".join(rows[:25]))
            with self.assertRaisesRegex(ValueError, "75 consumed"):
                admission.check_pixels(actual, reference)
            rows[60] = "0, 60, 60, 1, 2073600, " + "f" * 32 + "\n"
            actual.write_text("".join(rows))
            with self.assertRaisesRegex(ValueError, "picture 60"):
                admission.check_pixels(actual, reference)

    def test_known_old_pictures_do_not_pass_an_ordered_oracle(self):
        # Error recovery can repeat a known old picture with a correct total
        # count. A membership-only oracle would accept it; presentation order
        # must remain part of the comparison, including the pre-IDR tail.
        rows = [f"0, {i}, {i}, 1, 2073600, {i:032x}\n" for i in range(75)]
        with tempfile.TemporaryDirectory() as temporary:
            reference = Path(temporary) / "reference.framemd5"
            actual = Path(temporary) / "actual.framemd5"
            reference.write_text("".join(rows))
            for replacement in ((rows[20], rows[21]), (rows[24], rows[23])):
                changed = rows[:]
                changed[23:25] = replacement
                actual.write_text("".join(changed))
                observed = admission.pixels(actual)
                expected = admission.pixels(reference)
                self.assertEqual(len(observed), len(expected))
                self.assertTrue(all(picture in expected for picture in observed))
                with self.assertRaisesRegex(ValueError, "picture 23"):
                    admission.check_pixels(actual, reference)


if __name__ == "__main__":
    unittest.main()
