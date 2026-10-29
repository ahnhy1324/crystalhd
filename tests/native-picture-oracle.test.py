#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('native_picture_oracle', HERE / 'native-picture-oracle.py')
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)
WIDTH, HEIGHT = 16, 8


def reference():
    # BT709 interpretation of the documented BT601 yellow/blue code values.
    # Independent hand expectations are RGB(255,240,0) and RGB(0,15,255).
    y = bytes(210 if x < WIDTH // 2 else 41 for row in range(HEIGHT) for x in range(WIDTH))
    u = bytes(16 if x < WIDTH // 4 else 240 for row in range(HEIGHT // 2) for x in range(WIDTH // 2))
    v = bytes(146 if x < WIDTH // 4 else 110 for row in range(HEIGHT // 2) for x in range(WIDTH // 2))
    return y + u + v


def capture(*, stride=WIDTH * 4, matrix='bt709'):
    left, right = ((0, 240, 255, 255), (255, 15, 0, 255)) if matrix == 'bt709' else (
        (0, 255, 255, 255), (255, 0, 0, 255))
    row = bytes(left * (WIDTH // 2) + right * (WIDTH // 2)) + bytes([0xA5]) * (stride - WIDTH * 4)
    return row * HEIGHT


def samples(frames=None):
    return oracle.reference_samples(frames or [reference()] * 3, WIDTH, HEIGHT, 3, 2)[0]


class OracleTests(unittest.TestCase):
    def test_known_real_valued_samples(self):
        for yuv, expected in [((16, 128, 128), (0, 0, 0)), ((235, 128, 128), (255, 255, 255)),
                              ((210, 16, 146), (255, 240, 0)), ((41, 240, 110), (0, 15, 255))]:
            self.assertEqual(oracle.rgb(*yuv), expected)
        self.assertEqual(oracle.rgb(210, 16, 146, 'bt601'), (255, 255, 0))

    def test_source_only_constant_points(self):
        points = samples()
        self.assertGreaterEqual(len(points), 16)
        report = oracle.compare(points, capture(), WIDTH, HEIGHT)
        self.assertEqual(report['result'], 'PASS')
        self.assertEqual(report['maximum_channel_error'], 0)
        self.assertEqual(report['reference_coverage'], 'SUFFICIENT')
        for field in ('capture_source_pts', 'complete_crop_and_pixels', 'physical_presentation'):
            self.assertEqual(report[field], 'NOT_VERIFIED')
        self.assertEqual(report['decoded_yuyv'], 'NOT_OBSERVED')

    def test_last_reference_frame_can_disqualify_a_point(self):
        moving = bytearray(reference())
        moving[3 * WIDTH + 3] += 1
        self.assertIn((3, 3), samples())
        self.assertNotIn((3, 3), samples([reference(), reference(), moving]))
        # Sample selection never reads capture bytes, including wrong-matrix bytes.
        a = oracle.compare(samples(), capture(), WIDTH, HEIGHT)
        b = oracle.compare(samples(), capture(matrix='bt601'), WIDTH, HEIGHT)
        self.assertEqual(a['invariant_samples'], b['invariant_samples'])

    def test_spatially_nonflat_chroma_is_excluded(self):
        points = samples()
        self.assertNotIn((7, 3), points)
        self.assertNotIn((9, 3), points)

    def test_wrong_matrix_cannot_choose_a_passing_diagnostic(self):
        report = oracle.compare(samples(), capture(matrix='bt601'), WIDTH, HEIGHT)
        self.assertEqual(report['result'], 'FAIL')
        self.assertEqual(report['maximum_channel_error'], 15)
        self.assertEqual(report['diagnostic_alternate_matrix']['maximum_channel_error'], 0)
        self.assertFalse(report['diagnostic_alternate_matrix']['changes_primary_verdict'])

    def test_wrong_channels_or_spatial_placement_fail(self):
        bad = bytearray(capture())
        for offset in range(0, len(bad), 4):
            bad[offset], bad[offset + 2] = bad[offset + 2], bad[offset]
        self.assertEqual(oracle.compare(samples(), bad, WIDTH, HEIGHT)['result'], 'FAIL')
        wrong_position = b''.join(capture()[y * WIDTH * 4 + WIDTH * 2:(y + 1) * WIDTH * 4] +
                                  capture()[y * WIDTH * 4:y * WIDTH * 4 + WIDTH * 2]
                                  for y in range(HEIGHT))
        self.assertEqual(oracle.compare(samples(), wrong_position, WIDTH, HEIGHT)['result'], 'FAIL')

    def test_padded_stride_and_all_active_alpha(self):
        self.assertEqual(oracle.compare(samples(), capture(stride=80), WIDTH, HEIGHT, stride=80)['result'], 'PASS')
        bad = bytearray(capture())
        bad[(2 * WIDTH + 2) * 4 + 3] = 254 # Not a grid sample: alpha still checked.
        self.assertEqual(oracle.compare(samples(), bad, WIDTH, HEIGHT)['result'], 'FAIL')
        self.assertEqual(oracle.compare(samples(), bad, WIDTH, HEIGHT)['nonopaque_active_pixels'], 1)

    def test_insufficient_reference_is_not_a_pass(self):
        self.assertEqual(oracle.compare({}, capture(), WIDTH, HEIGHT)['result'], 'NOT_VERIFIED')
        report = oracle.compare(samples(), capture(), WIDTH, HEIGHT, minimum_samples=100)
        self.assertEqual(report['result'], 'NOT_VERIFIED')
        self.assertEqual(report['reference_coverage'], 'INSUFFICIENT')
        report = oracle.compare(samples(), capture(matrix='bt601'), WIDTH, HEIGHT, minimum_samples=100)
        self.assertEqual(report['result'], 'FAIL') # A valid counterexample still disproves this check.
        self.assertEqual(report['reference_coverage'], 'INSUFFICIENT')

    def test_extent_geometry_and_sample_guards(self):
        for args in [(samples(), capture()[:-1], WIDTH, HEIGHT), (samples(), capture(), WIDTH - 1, HEIGHT),
                     ({(WIDTH, 0): (16, 128, 128)}, capture(), WIDTH, HEIGHT)]:
            with self.assertRaises(ValueError): oracle.compare(*args)
        with self.assertRaises(ValueError): oracle.compare(samples(), capture(), WIDTH, HEIGHT, stride=80)
        with self.assertRaises(ValueError): oracle.compare(samples(), capture(), WIDTH, HEIGHT, matrix='auto')

    def test_reference_count_and_truncation_guards(self):
        for frames in ([], [reference()], [reference()] * 4, [reference(), reference(), reference()[:-1]]):
            with self.assertRaises(ValueError): oracle.reference_samples(frames, WIDTH, HEIGHT, 3, 2)
        with self.assertRaises(ValueError): list(oracle.read_frames(io.BytesIO(reference()[:-1]), len(reference())))

    def test_partial_stream_reads(self):
        class ShortReads(io.BytesIO):
            def read(self, size): return super().read(min(size, 7))
        self.assertEqual(list(oracle.read_frames(ShortReads(reference() * 3), len(reference()))), [reference()] * 3)

    def test_cli_hash_and_identity_limits(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work); ref, cap = root / 'source.yuv', root / 'capture.bgra'
            ref.write_bytes(reference() * 3); cap.write_bytes(capture())
            argv = [sys.executable, '-B', str(HERE / 'native-picture-oracle.py'), str(ref), str(cap),
                    '--width', str(WIDTH), '--height', str(HEIGHT), '--frames', '3', '--step', '2', '--matrix', 'bt709']
            good = subprocess.run(argv, capture_output=True, text=True, timeout=10)
            self.assertEqual(good.returncode, 0, good.stderr)
            result = json.loads(good.stdout)
            self.assertEqual(result['reference_sha256'], hashlib.sha256(ref.read_bytes()).hexdigest())
            self.assertEqual(result['capture_source_pts'], 'NOT_VERIFIED')
            bad = subprocess.run(argv + ['--capture-sha256', '0' * 64], capture_output=True, text=True, timeout=10)
            self.assertEqual(bad.returncode, 2)
            self.assertIn('capture SHA256 mismatch', bad.stderr)
            bad = subprocess.run(argv + ['--reference-sha256', '0' * 64], capture_output=True, text=True, timeout=10)
            self.assertEqual(bad.returncode, 2)
            self.assertIn('reference SHA256 mismatch', bad.stderr)

    def test_frozen_counterexample_arithmetic_and_scope(self):
        receipt = json.loads((HERE / 'fixtures/issue538/native-picture-observation-20261029.json').read_text())
        small, large = receipt['cases']
        self.assertEqual((small['invariant_samples'], small['result']), (0, 'NOT_VERIFIED'))
        self.assertEqual((large['invariant_samples'], large['result']), (3, 'FAIL'))
        self.assertEqual(len(large['frozen_counterexample_points']), 3)
        for point in large['frozen_counterexample_points']:
            self.assertEqual(list(oracle.rgb(*point['source_yuv'])), point['expected_rgb'])
            error = [abs(a-b) for a,b in zip(point['expected_rgb'], point['captured_rgb'])]
            self.assertEqual(error, point['errors'])
            self.assertEqual(max(error), 15)
        for case in receipt['cases']:
            self.assertEqual(case['capture_source_pts'], 'NOT_VERIFIED')
            self.assertEqual(case['reference_coverage'], 'INSUFFICIENT')
        self.assertEqual(receipt['metadata_mechanism']['historical_native_fault_attribution'], 'NOT_PROVEN')


if __name__ == '__main__':
    unittest.main()
