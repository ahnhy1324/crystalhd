#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixture oracle/control regressions; no hardware or maintained parser."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location('poc1_fixture',
    Path(__file__).with_name('native-poc1-multislice-fixture.py'))
F = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(F)


def header(nal, mode):
    """Read only the emitted prefix, independently of production grouping."""
    r = F.PCM.Reader(nal[1:].replace(b'\x00\x00\x03', b'\x00\x00'))
    result = dict(first_mb=r.ue(), slice_type=r.ue(), pps=r.ue(), frame_num=r.u(4))
    if nal[0] & 31 == 5:
        result['idr_pic_id'] = r.ue()
    if mode != 'zero':
        signed = lambda value: (value + 1) // 2 if value & 1 else -(value // 2)
        result['delta0'] = signed(r.ue())
        if mode == 'both':
            result['delta1'] = signed(r.ue())
    return result


class SourceControls(unittest.TestCase):
    def test_trace_header_reference_does_not_leak_from_following_aud(self):
        fields = [('nal_ref_idc', 2), ('nal_unit_type', 1), ('frame_num', 3),
                  ('forbidden_zero_bit', 0), ('nal_ref_idc', 0), ('nal_unit_type', 9),
                  ('nal_ref_idc', 1), ('nal_unit_type', 1), ('frame_num', 4),
                  ('nal_ref_idc', 0), ('nal_unit_type', 9)]
        text = ''.join(f'[trace_headers @ 0] 0 {key} {value:b} = {value}\n'
                       for key, value in fields)
        _, rows = F.read_trace(text)
        self.assertEqual([(r['nal_ref_idc'], r['frame_num']) for r in rows], [(2, 3), (1, 4)])

    def test_900_frame_wrap_and_idr_boundaries(self):
        plans = [F.picture_plan(i, 'both') for i in range(900)]
        self.assertEqual([i for i, p in enumerate(plans) if p['idr']], list(range(0, 900, 48)))
        self.assertEqual([plans[i]['frame_num'] for i in (0, 15, 16, 31, 32, 47, 48, 899)],
                         [0, 15, 0, 15, 0, 15, 0, 3])
        self.assertEqual([plans[i]['frame_num_offset'] for i in (15, 16, 31, 32, 47, 48)],
                         [0, 16, 16, 32, 32, 0])
        for a, b in zip(plans, plans[1:]):
            if not b['idr']:
                self.assertGreater(min(b['expected_top_poc'], b['expected_bottom_poc']),
                                   max(a['expected_top_poc'], a['expected_bottom_poc']))
        for key in ('delta0', 'delta1'):
            self.assertTrue(any(p[key] < 0 for p in plans))
            self.assertTrue(any(p[key] > 0 for p in plans))

    def test_conditional_delta_keys_and_matching_multislice(self):
        for mode in F.MODES:
            for slices in (2, 4):
                for ordinal in (0, 1, 2, 16, 47, 48):
                    _, vcl, plan = F.access_unit(ordinal, mode, slices, True)
                    rows = [header(n[4:], mode) for n in vcl]
                    self.assertEqual([r.pop('first_mb') for r in rows],
                                     [i * (F.MACROBLOCKS // slices) for i in range(slices)])
                    self.assertTrue(all(row == rows[0] for row in rows))
                    self.assertEqual(rows[0]['frame_num'], plan['frame_num'])
                    self.assertEqual('delta0' in rows[0], mode != 'zero')
                    self.assertEqual('delta1' in rows[0], mode == 'both')
                    if mode != 'zero':
                        self.assertEqual(rows[0]['delta0'], plan['delta0'])
                    if mode == 'both':
                        self.assertEqual(rows[0]['delta1'], plan['delta1'])

    def test_equal_field_poc_retains_signed_delta0_and_present_zero_delta1(self):
        # Read the emitted signed syntax against fixed values, not the plan
        # used by the producer. The default keeps the original unequal POCs.
        cases = ((1, 1, -1), (2, -1, 1), (3, 2, 0), (4, -2, -1),
                 (16, 1, -1), (47, -1, 1), (48, 0, 0))
        for slices in (2, 4):
            for ordinal, delta0, original_delta1 in cases:
                original, vcl, _ = F.access_unit(ordinal, 'both', slices, True)
                explicit_original, _, _ = F.access_unit(ordinal, 'both', slices, True,
                                                       equal_field_poc=False)
                self.assertEqual(original, explicit_original)
                self.assertTrue(all(header(n[4:], 'both')['delta1'] == original_delta1
                                    for n in vcl))
                _, equal_vcl, plan = F.access_unit(ordinal, 'both', slices, True,
                                                  equal_field_poc=True)
                for nal in equal_vcl:
                    row = header(nal[4:], 'both')
                    self.assertEqual((row['delta0'], row['delta1']), (delta0, 0))
                self.assertEqual(plan['expected_top_poc'], plan['expected_bottom_poc'])

    def test_equal_field_poc_delta1_mismatch_remains_prefix_only(self):
        controls = F.prefix_controls('both', 4, False, equal_field_poc=True)
        self.assertEqual(controls['scope'],
                         'PREFIX_ONLY_INVALID_AU_CONTROLS_NEVER_SENT_TO_HARDWARE')
        positive = [header(bytes.fromhex(n), 'both') for n in controls['positive']['vcl']]
        self.assertTrue(all(row['delta1'] == 0 for row in positive))
        negative = next(n for n in controls['negatives'] if n['name'] == 'delta1-only-mismatch')
        self.assertFalse(negative['expected_associated'])
        rows = [header(bytes.fromhex(n), 'both') for n in negative['vcl']]
        self.assertEqual([row['delta1'] for row in rows], [0, 1, 0, 0])
        for index, (before, after) in enumerate(zip(positive, rows)):
            self.assertEqual({key for key in before if before[key] != after[key]},
                             {'delta1'} if index == 1 else set())

    def test_delta_only_negative_controls_are_not_media(self):
        for mode in F.MODES:
            for slices in (2, 4):
                controls = F.prefix_controls(mode, slices, False)
                self.assertEqual(controls['scope'],
                                 'PREFIX_ONLY_INVALID_AU_CONTROLS_NEVER_SENT_TO_HARDWARE')
                positive = [header(bytes.fromhex(n), mode) for n in controls['positive']['vcl']]
                self.assertTrue(controls['positive']['expected_associated'])
                self.assertEqual(len(controls['negatives']), {'zero': 0, 'top': 1, 'both': 2}[mode])
                for negative in controls['negatives']:
                    rows = [header(bytes.fromhex(n), mode) for n in negative['vcl']]
                    self.assertFalse(negative['expected_associated'])
                    key = negative['name'].split('-')[0]
                    for i, (before, after) in enumerate(zip(positive, rows)):
                        changed = {k for k in before if before[k] != after[k]}
                        self.assertEqual(changed, {key} if i == 1 else set())

    def test_source_tags_and_constant_chroma_packing(self):
        self.assertEqual(len({F.tag(i) for i in range(900)}), 900)
        for ordinal in (0, 16, 48, 899):
            planar = F.oracle_frame(ordinal)
            packed = F.packed_frame(planar)
            plane = F.WIDTH * F.HEIGHT
            self.assertEqual(packed[::2], planar[:plane])
            self.assertEqual(set(packed[1::4]), {planar[plane]})
            self.assertEqual(set(packed[3::4]), {planar[-1]})
            code = F.tag(ordinal)
            for top in (0, 64):
                found = bytearray(8)
                for bit in range(64):
                    x, y = 12 + (bit % 16) * 8, top + 4 + (bit // 16) * 8
                    found[bit // 8] |= int(planar[y * F.WIDTH + x] == 235) << (7 - bit % 8)
                self.assertEqual(bytes(found), code)

    def test_decoded_oracle_rejects_corruption_missing_extra_and_reorder(self):
        frames = [F.oracle_frame(i) for i in range(3)]
        expected = b''.join(frames)
        changed = bytearray(expected)
        changed[F.FRAME_BYTES + 300] ^= 1
        variants = (bytes(changed), expected[:-1], expected + b'\x00',
                    frames[0] + frames[2] + frames[1], frames[0] * 3)
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            original, actual = root / 'expected', root / 'actual'
            original.write_bytes(expected)
            actual.write_bytes(expected)
            F.compare_decoded(actual, original, 3)
            for data in variants:
                actual.write_bytes(data)
                with self.assertRaisesRegex(ValueError, 'pixels/source order|decoded bytes'):
                    F.compare_decoded(actual, original, 3)


if __name__ == '__main__':
    unittest.main()
