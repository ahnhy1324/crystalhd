#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent corruption controls for the source-content barcode."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('picture_fixture', Path(__file__).with_name('native-picture-fixture.py'))
F = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(F)


def cell(raw, index, value, width=1280, kind='bgra'):
    x, y, w, h = F.cells(width)[index]
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            q = ((y + h // 2 + dy) * width + x + w // 2 + dx) * (4 if kind == 'bgra' else 2)
            if kind == 'bgra':
                raw[q:q + 4] = bytes((value, value, value, 255))
            else:
                raw[q] = value


def capture(payload, width=1280, kind='bgra'):
    height = 720 if width == 1280 else 1080
    raw = bytearray(bytes((0, 0, 0, 255) if kind == 'bgra' else (16, 128, 16, 128)) *
                    (width * height // (1 if kind == 'bgra' else 2)))
    bits = [(v >> bit) & 1 for v in payload for bit in range(7, -1, -1)]
    for i, bit in enumerate(bits * 2):
        cell(raw, i, (255 if kind == 'bgra' else 235) if bit else 0, width, kind)
    return raw


class BarcodeTests(unittest.TestCase):
    def test_declared_control_case_is_required(self):
        raw = capture(F.code_bytes(1920, 1, source_case=3), 1920)
        self.assertEqual(F.decode(1920, 1080, raw, 'bgra', source_case=3), 1)
        for expected in (None, 1, 2, 4):
            with self.assertRaisesRegex(ValueError, 'source-case'):
                F.decode(1920, 1080, raw, 'bgra', source_case=expected)
        source = F.frame(1920, 1080, 179, source_case=3)
        self.assertEqual(F.decode(1920, 1080, source, 'yuv420p', source_case=3), 179)

    def test_invalid_declared_case_rejected(self):
        for value in (0, 256, -1, '3'):
            with self.assertRaisesRegex(ValueError, 'source case'):
                F.code_bytes(1920, 0, source_case=value)

    def test_frozen_codes_and_source_cases(self):
        expected = {1280: ('c53d0100ff81', 'c53d01b34c91'),
                    1920: ('c53d0200ffbe', 'c53d02b34cae')}
        for w, (first, last) in expected.items():
            self.assertEqual(F.code_bytes(w, 0).hex(), first)
            self.assertEqual(F.code_bytes(w, 179).hex(), last)
            self.assertEqual(len({F.code_bytes(w, n) for n in range(180)}), 180)

    def test_both_sizes_and_three_formats(self):
        for w, h in ((1280, 720), (1920, 1080)):
            base = F.base_frame(w, h)
            for ordinal in (0, 1, 179):
                self.assertEqual(F.decode(w, h, F.frame(w, h, ordinal, base), 'yuv420p'), ordinal)
                for kind in ('bgra', 'yuyv'):
                    self.assertEqual(F.decode(w, h, capture(F.code_bytes(w, ordinal), w, kind), kind), ordinal)

    def reject_payload(self, index, mask, reason):
        payload = bytearray(F.code_bytes(1280, 0)); payload[index] ^= mask
        with self.assertRaisesRegex(ValueError, reason):
            F.decode(1280, 720, capture(payload), 'bgra')

    def test_magic_rejected(self):
        self.reject_payload(0, 1, 'magic')

    def test_source_case_rejected(self):
        self.reject_payload(2, 3, 'source-case')

    def test_complement_rejected(self):
        self.reject_payload(4, 1, 'complement')

    def test_crc_rejected_in_both_replicas(self):
        self.reject_payload(5, 1, 'CRC')

    def test_valid_crc_out_of_range_rejected(self):
        payload = bytearray.fromhex('c53d01b44b00'); payload[5] = F.crc8(payload[:4])
        with self.assertRaisesRegex(ValueError, 'ordinal'):
            F.decode(1280, 720, capture(payload), 'bgra')

    def test_replica_mismatch_rejected(self):
        raw = capture(F.code_bytes(1280, 0)); cell(raw, 0, 0)
        with self.assertRaisesRegex(ValueError, 'replicas'):
            F.decode(1280, 720, raw, 'bgra')

    def test_ambiguous_threshold_rejected(self):
        raw = capture(F.code_bytes(1280, 0)); cell(raw, 0, 128)
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            F.decode(1280, 720, raw, 'bgra')

    def test_nonuniform_cell_rejected(self):
        raw = capture(F.code_bytes(1280, 0)); x, y, w, h = F.cells(1280)[0]
        q = ((y + h // 2) * 1280 + x + w // 2) * 4; raw[q:q + 3] = bytes(3)
        with self.assertRaisesRegex(ValueError, 'nonuniform'):
            F.decode(1280, 720, raw, 'bgra')

    def test_non_neutral_and_transparent_rejected(self):
        for component in (0, 3):
            raw = capture(F.code_bytes(1280, 0)); x, y, w, h = F.cells(1280)[0]
            raw[((y + h // 2) * 1280 + x + w // 2) * 4 + component] = 0
            with self.assertRaisesRegex(ValueError, 'non-neutral'):
                F.decode(1280, 720, raw, 'bgra')

    def test_extent_geometry_and_format_rejected(self):
        for args in ((1280, 720, bytes(8), 'bgra'), (640, 360, b'', 'bgra'),
                     (1280, 720, b'', 'rgb')):
            with self.assertRaises(ValueError):
                F.decode(*args)


if __name__ == '__main__':
    unittest.main()
