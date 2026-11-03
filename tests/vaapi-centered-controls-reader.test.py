#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Device-free content controls with independently placed scaled cell areas."""
from fractions import Fraction
import copy
import json
from pathlib import Path
import tempfile
import unittest
import importlib.util

DIRECTORY = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


H = load('centered_controls_subject', DIRECTORY / 'vaapi-centered-controls-reader.py')
V = load('centered_independent_payload_fixture', DIRECTORY / 'vaapi-centered-payload-reader.test.py')


def observed(ordinal, *, kind='bgra', height=1080, centered=True, coded=False, source_case=2):
    width = 1920 if centered else 1280
    data = (bytearray(b'\x00\x00\x00\xff') * (width * height) if kind == 'bgra' else
            bytearray(b'\x10') * (width * height) + bytearray(b'\x80') * (width * height // 2))
    bits = [byte >> bit & 1 for byte in H.B.code_bytes(1920, ordinal, source_case=source_case)
            for bit in range(7, -1, -1)] * 2
    vertical = Fraction(45, 68) if coded else Fraction(2, 3)
    # Scale whole source rectangles independently of the decoder's patches.
    for (x, y, w, h), bit in zip(H.B.cells(1920), bits):
        left, right = x * 2 // 3, (x + w) * 2 // 3
        top, bottom = int(y * vertical), int((y + h) * vertical)
        if centered:
            left += 320; right += 320; top += 180; bottom += 180
        value = bytes([255 if bit else 0]) * 3 + b'\xff' if kind == 'bgra' else bytes([235 if bit else 16])
        factor = len(value)
        for row in range(top, bottom):
            start = (row * width + left) * factor
            data[start:start + (right - left) * factor] = value * (right - left)
    return data


class ContentTests(unittest.TestCase):
    def test_bgra_content_boundaries_and_two_height_models(self):
        for ordinal in (0, 1, 90, 179):
            for coded in (False, True):
                with self.subTest(ordinal=ordinal, coded=coded):
                    self.assertEqual(H.decode_patches(observed(ordinal, coded=coded), 1920, 1080, 'bgra', True), ordinal)

    def test_nv12_small_and_actual_coded_canvas(self):
        for centered, width, height in ((False, 1280, 720), (True, 1920, 1080), (True, 1920, 1088)):
            data = observed(37, kind='nv12', height=height, centered=centered)
            self.assertEqual(H.decode_patches(data, width, height, 'nv12', centered), 37)

    def test_wrong_extent_and_geometry_rejected(self):
        data = observed(1)
        for width, height, pixels in ((1920, 1080, data[:-1]), (1280, 1080, data), (1920, 1088, data)):
            with self.assertRaises(ValueError):
                H.decode_patches(pixels, width, height, 'bgra', True)

    def test_wrong_source_case_rejected(self):
        with self.assertRaisesRegex(ValueError, 'source-case'):
            H.decode_patches(observed(1, source_case=3), 1920, 1080, 'bgra', True)

    def test_wrong_replica_rejected(self):
        data = observed(1)
        for y in range(200, 203):
            for x in range(489, 492):
                data[(y * 1920 + x) * 4:(y * 1920 + x) * 4 + 4] = b'\x00\x00\x00\xff'
        with self.assertRaises(ValueError):
            H.decode_patches(data, 1920, 1080, 'bgra', True)

    def test_alpha_non_neutral_and_nonuniform_patches_rejected(self):
        offset = (201 * 1920 + 490) * 4
        for variant in ('alpha', 'color', 'spread'):
            data = observed(1)
            if variant == 'alpha':
                data[offset + 3] = 0
            elif variant == 'color':
                data[offset] -= 7
            else:
                data[offset:offset + 3] = b'\xf0' * 3
            with self.assertRaises(ValueError):
                H.decode_patches(data, 1920, 1080, 'bgra', True)

    def test_origin_aligned_content_is_not_centered_content(self):
        data = observed(1)
        for row in range(1080):
            start = row * 1920 * 4
            data[start:start + 1600 * 4] = data[start + 320 * 4:start + 1920 * 4]
        with self.assertRaises(ValueError):
            H.decode_patches(data, 1920, 1080, 'bgra', True)

    def test_capture_result_preserves_fixture_pts_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'capture.bgra'; path.write_bytes(observed(90))
            result = H.audit_capture(path)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['source_pts_ticks'], 90 * 512)
        self.assertEqual(result['source_time_base'], '1/15360')
        self.assertEqual(result['actual_media_PTS_to_VA_binding'], 'NOT_OBSERVED')

    def payload(self, directory, *, repeat=False, nonneutral=False, corrupt_border=False,
                ordinals=None, selection=None, reset=False):
        doc, blobs, trace = V.reset_fixture() if reset else V.fixture()
        if selection is not None:
            doc['selection'] = selection
        for number, slot in enumerate(doc['slots']):
            ordinal = ordinals[number] if ordinals is not None else 0 if repeat else number
            source = observed(ordinal, kind='nv12', height=720, centered=False)
            if nonneutral:
                source[1280 * 720] = 129
            blobs[slot['small_file']] = source
            blobs[slot['composed_file']] = V.packed_canvas(source, slot['composed'])
            blobs[slot['public_file']] = V.packed_canvas(source, slot['public_canvas'])
        if corrupt_border:
            first = doc['slots'][0]['public_file']
            damaged = bytearray(blobs[first]); damaged[0] ^= 1; blobs[first] = damaged
        root = Path(directory)
        for name, data in blobs.items():
            (root / name).write_bytes(data)
        manifest, raw = root / 'manifest.json', root / 'trace.jsonl'
        manifest.write_text(json.dumps(doc))
        raw.write_text(''.join(json.dumps(row) + '\n' for row in trace))
        return manifest, raw

    def test_full_payload_binding_and_content_join(self):
        with tempfile.TemporaryDirectory() as directory:
            result = H.audit_payload(*self.payload(directory))
        self.assertEqual(result['status'], 'PASS', result)
        self.assertEqual([row['source_ordinal'] for row in result['selected_source_bindings']], [0, 1])
        self.assertTrue(all(len(set(row['stage_ordinals'].values())) == 1 for row in result['selected_source_bindings']))

    def test_different_bindings_cannot_duplicate_fixture_content(self):
        with tempfile.TemporaryDirectory() as directory:
            result = H.audit_payload(*self.payload(directory, repeat=True))
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('repeat source content', result['error'])

    def test_neutral_source_chroma_independently_required(self):
        with tempfile.TemporaryDirectory() as directory:
            result = H.audit_payload(*self.payload(directory, nonneutral=True))
        self.assertEqual(result['status'], 'FAIL')
        self.assertIn('non-neutral chroma', result['error'])

    def test_base_full_canvas_failure_cannot_be_overridden_by_barcode(self):
        with tempfile.TemporaryDirectory() as directory:
            result = H.audit_payload(*self.payload(directory, corrupt_border=True))
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['payload_audit']['status'], 'FAIL')

    def target_payload(self, directory, **options):
        return self.payload(directory, ordinals=(90, 91), selection=H.TARGET_SELECTION, **options)

    def target_case(self, directory, **options):
        manifest, trace_path = self.target_payload(directory, **options)
        result = H.audit_payload(manifest, trace_path, required_targets=(90, 91))
        self.assertEqual(result['status'], 'PASS', result)
        return result, trace_path

    def change_trace(self, path, mutation, resequence=False):
        rows = [json.loads(line) for line in path.read_text().splitlines()]
        mutation(rows)
        if resequence:
            V.resequence(rows)
        path.write_text(''.join(json.dumps(row) + '\n' for row in rows))

    def test_fixed_targets_are_decoded_content_independent_of_va_ordinals(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory)
        self.assertEqual(result['required_targets'], [90, 91])
        self.assertEqual(result['selection'], 'case2-90-91')
        self.assertEqual([row['source_ordinal'] for row in result['selected_source_bindings']], [90, 91])
        self.assertEqual([row['binding']['submission_ordinal'] for row in result['selected_source_bindings']], [1, 2])
        self.assertTrue(all(row['actual_media_PTS_to_VA_binding'] == 'NOT_OBSERVED'
                            for row in result['selected_source_bindings']))

    def test_fixed_target_mode_requires_explicit_manifest_selection(self):
        for mode in (None, 'first-two', 'case2-0-1'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                paths = self.payload(directory, ordinals=(90, 91), selection=mode)
                result = H.audit_payload(*paths, required_targets=(90, 91))
                self.assertEqual(result['status'], 'FAIL', result)
                self.assertIn('selection', result['error'])
                self.assertEqual(result['payload_audit']['status'], 'PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD')

    def test_wrong_or_reversed_decoded_targets_cannot_be_selected_by_mode_label(self):
        for ordinals in ((0, 1), (90, 92), (91, 90)):
            with self.subTest(ordinals=ordinals), tempfile.TemporaryDirectory() as directory:
                paths = self.payload(directory, ordinals=ordinals, selection=H.TARGET_SELECTION)
                result = H.audit_payload(*paths, required_targets=(90, 91))
                self.assertEqual(result['status'], 'FAIL', result)
                self.assertIn('decoded source contents', result['error'])

    def test_va_submission_ordinals90_91_cannot_fake_source_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, trace_path = self.payload(directory, selection=H.TARGET_SELECTION)
            doc = json.loads(manifest.read_text())
            for slot in doc['slots']:
                slot['binding']['submission_ordinal'] += 89
            manifest.write_text(json.dumps(doc))
            self.change_trace(trace_path, lambda rows: [row.__setitem__('submission_ordinal',
                row['submission_ordinal'] + 89) for row in rows if row['submission_ordinal']])
            result = H.audit_payload(manifest, trace_path, required_targets=(90, 91))
        self.assertEqual(result['status'], 'FAIL', result)
        self.assertEqual(result['payload_audit']['status'], 'PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD')
        self.assertIn('decoded source contents', result['error'])

    def test_legacy_default_is_unchanged_and_cannot_claim_target_window(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = self.payload(directory)
            result = H.audit_payload(*paths)
            self.assertEqual(result['status'], 'PASS', result)
            self.assertNotIn('required_targets', result)
            self.assertNotIn('selection', result)
            self.assertEqual(H.audit_target_window(result, paths[1], 50, 1200)['status'], 'FAIL')

    def test_target_selector_argument_is_fixed_and_full_canvas_gate_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = self.target_payload(directory)
            for targets in ((0, 1), (90, 92), '90,91'):
                self.assertEqual(H.audit_payload(*paths, required_targets=targets)['status'], 'FAIL')
        with tempfile.TemporaryDirectory() as directory:
            paths = self.target_payload(directory, corrupt_border=True)
            result = H.audit_payload(*paths, required_targets=(90, 91))
            self.assertEqual(result['status'], 'FAIL', result)
            self.assertEqual(result['payload_audit']['status'], 'FAIL')

    def test_exact_target_binding_window_positive_and_seek_boundary_inclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory)
            window = H.audit_target_window(result, trace_path, 100, 1200)
        self.assertEqual(window['status'], 'PASS', window)
        self.assertEqual([row['source_ordinal'] for row in window['selected_source_bindings']], [90, 91])
        self.assertEqual([row['submit_seq'] for row in window['selected_source_bindings']], [1, 8])
        self.assertEqual([row['materialized_seq'] for row in window['selected_source_bindings']], [2, 9])
        self.assertTrue(all(row['successful_syncs_in_window'] for row in window['selected_source_bindings']))
        self.assertEqual(window['actual_media_PTS_to_VA_binding'], 'NOT_OBSERVED')

    def test_target_window_also_joins_explicit_reset_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory, reset=True)
            window = H.audit_target_window(result, trace_path, 50, 1200)
        self.assertEqual(window['status'], 'PASS', window)
        self.assertEqual(result['payload_audit']['schema'], H.P.RESET_SCHEMA)

    def test_preseek_submit_cannot_be_borrowed_by_later_materialization_or_sync(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory)
            window = H.audit_target_window(result, trace_path, 150, 1200)
        self.assertEqual(window['status'], 'FAIL', window)
        self.assertIn('submit_bind', window['error'])

    def test_materialization_at_requested_stop_is_excluded(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory)
            window = H.audit_target_window(result, trace_path, 50, 900)
        self.assertEqual(window['status'], 'FAIL', window)
        self.assertIn('materialized', window['error'])

    def test_successful_sync_at_or_after_requested_stop_is_excluded(self):
        for stop in (1000, 1100):
            with self.subTest(stop=stop), tempfile.TemporaryDirectory() as directory:
                result, trace_path = self.target_case(directory)
                window = H.audit_target_window(result, trace_path, 50, stop)
                self.assertEqual(window['status'], 'FAIL', window)
                self.assertIn('sync inside', window['error'])

    def test_swapped_binding_in_content_report_cannot_join_other_actual_surface(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory)
            for field in H.P.BINDING_KEYS:
                with self.subTest(field=field):
                    swapped = copy.deepcopy(result)
                    swapped['selected_source_bindings'][0]['binding'][field] += 1
                    window = H.audit_target_window(swapped, trace_path, 50, 1200)
                    self.assertEqual(window['status'], 'FAIL', window)

    def test_nearby_materialization_or_sync_with_other_owner_cannot_join(self):
        for index in (1, 2, 3):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as directory:
                result, trace_path = self.target_case(directory)
                self.change_trace(trace_path, lambda rows: rows[index].__setitem__('owner', 999))
                window = H.audit_target_window(result, trace_path, 50, 1200)
                self.assertEqual(window['status'], 'FAIL', window)

    def test_no_matching_successful_sync_cannot_be_filled_by_other_target(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory)
            def remove_first_sync(rows):
                rows[:] = [row for row in rows if not
                           (row['event'] in ('sync_enter', 'sync_exit') and row['token'] == 100000)]
            self.change_trace(trace_path, remove_first_sync, resequence=True)
            window = H.audit_target_window(result, trace_path, 50, 1200)
        self.assertEqual(window['status'], 'FAIL', window)
        self.assertIn('sync_exit', window['error'])

    def test_target_window_rejects_incomplete_trace_or_different_payload_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory)
            changed = copy.deepcopy(result)
            changed['payload_audit']['observations'][0]['submit_seq'] += 1
            self.assertEqual(H.audit_target_window(changed, trace_path, 50, 1200)['status'], 'FAIL')
            self.change_trace(trace_path, lambda rows: rows[1].__setitem__('seq', 999))
            window = H.audit_target_window(result, trace_path, 50, 1200)
        self.assertEqual(window['status'], 'FAIL', window)
        self.assertIn('sequence', window['error'])

    def test_target_window_rejects_invalid_or_reversed_monotonic_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            result, trace_path = self.target_case(directory)
            for start, stop in ((0, 1200), (1200, 1200), (1300, 1200), (True, 1200), (50, 1200.0)):
                with self.subTest(start=start, stop=stop):
                    self.assertEqual(H.audit_target_window(result, trace_path, start, stop)['status'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
