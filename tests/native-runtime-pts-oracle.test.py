#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inert controls for the pinned offline PTS model; no media or target execution."""
import sys
sys.dont_write_bytecode = True
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('native_pts_oracle', ROOT/'tools/kodi-route-probe/native-pts-oracle.py')
ORACLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ORACLE)
MEDIA = 'a' * 64


def fixture():
    # Coded order differs from display order. Expected values are constants,
    # independently specified here rather than generated with the implementation.
    source = {'streams': [{'index': 0, 'codec_type': 'video', 'codec_name': 'h264',
                          'time_base': '1/15360', 'start_pts': 0, 'start_time': '0.000000', 'nb_frames': '4'}],
              'format': {'start_time': '0.000000'},
              'packets': [{'stream_index': 0, 'codec_type': 'video', 'pts': v} for v in [0, 1536, 512, 1024]]}
    cycle = {'cycle': 0, 'accepted_native_PTS': [0, 100000, 33333, 66666],
             'returned_native_PTS': [0, 33333, 66666, 100000], 'drm_frame_count': 4,
             'capture_LAST_bytes': [0], 'trace_start_byte': 100, 'trace_end_byte': 500}
    report = {'status': 'PASS', 'fixture': {'sha256': MEDIA}, 'expected_frames_per_cycle': 4,
              'requested_cycles': 1, 'cycles': [cycle],
              'native_probe': {'@capture_nonempty': 4, '@capture_LAST': 1, '@h264_frames': {'1280,720,178': 4}}}
    return source, report


class OfflinePTS(unittest.TestCase):
    def call_cli(self, source, report, strict=False, bad_source_pin=False):
        with tempfile.TemporaryDirectory() as directory:
            source_path, report_path = Path(directory)/'packets.json', Path(directory)/'native.json'
            source_raw, report_raw = json.dumps(source).encode(), json.dumps(report).encode()
            source_path.write_bytes(source_raw); report_path.write_bytes(report_raw)
            args = ['--source-packets', str(source_path), '--source-packets-sha256',
                    '0' * 64 if bad_source_pin else hashlib.sha256(source_raw).hexdigest(),
                    '--native-report', str(report_path), '--native-report-sha256', hashlib.sha256(report_raw).hexdigest(),
                    '--media-sha256', MEDIA, '--kodi-source', ORACLE.KODI_SOURCE]
            if strict:
                args.append('--require-renderer-census')
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = ORACLE.main(args)
            self.assertEqual(len(output.getvalue().splitlines()), 1)
            return code, json.loads(output.getvalue())

    def run_oracle(self, source=None, report=None, **kwargs):
        original_source, original_report = fixture()
        return ORACLE.analyze(source if source is not None else original_source,
                              report if report is not None else original_report,
                              kwargs.get('media', MEDIA), kwargs.get('kodi', ORACLE.KODI_SOURCE))

    def rejects(self, source=None, report=None, **kwargs):
        with self.assertRaises(ORACLE.OracleError):
            self.run_oracle(source, report, **kwargs)

    def test_coded_order_and_sorted_returned_match(self):
        result = self.run_oracle()
        self.assertEqual(result['status'], 'PASS_OFFLINE_SOURCE_CALLER_RETURNED_PTS_MATCH')
        self.assertEqual(result['source_video_packets'], 4)
        self.assertEqual(result['tolerance_us'], 0)
        self.assertEqual(result['scope'], 'OFFLINE_PTS_ONLY_NO_HARDWARE_OR_AV_PROMOTION')

    def test_floating_62976_edge_rejects_rational_floor(self):
        source, report = fixture()
        source['packets'] = [{'stream_index': 0, 'pts': v} for v in [0, 62976]]
        source['streams'][0]['nb_frames'] = '2'
        report['expected_frames_per_cycle'] = 2
        report['cycles'][0].update(accepted_native_PTS=[0, 4099999], returned_native_PTS=[0, 4099999], drm_frame_count=2)
        report.pop('native_probe')
        result = self.run_oracle(source, report)
        delta = result['rational_vs_binary64_diagnostic']
        self.assertEqual(delta['nonzero_delta_packets'], 1)
        self.assertEqual(delta['examples'][0]['rational_pts_us'], 4100000)
        self.assertEqual(delta['examples'][0]['caller_pts_us'], 4099999)
        report['cycles'][0]['accepted_native_PTS'][1] = 4100000
        report['cycles'][0]['returned_native_PTS'][1] = 4100000
        self.rejects(source, report)

    def test_literal_conversion_truncates_toward_zero(self):
        self.assertEqual(ORACLE.caller_pts(1, 1, 3), 333333)
        self.assertEqual(ORACLE.caller_pts(62976, 1, 15360), 4099999)
        with self.assertRaises(ORACLE.OracleError):
            ORACLE.caller_pts(-1, 1, 3)

    def test_multiple_cycles_require_exact_independent_sequences(self):
        source, report = fixture()
        report['requested_cycles'] = 2
        second = copy.deepcopy(report['cycles'][0]); second.update(cycle=1, trace_start_byte=600, trace_end_byte=1000)
        report['cycles'].append(second)
        report['native_probe'] = {'@capture_nonempty': 8, '@capture_LAST': 2, '@h264_frames': {'1280,720,178': 8}}
        self.assertEqual(self.run_oracle(source, report)['completed_cycles'], 2)
        second['returned_native_PTS'][2] += 1
        self.rejects(source, report)

    def test_stale_reused_trace_window_is_rejected(self):
        source, report = fixture()
        report['requested_cycles'] = 2
        second = copy.deepcopy(report['cycles'][0]); second['cycle'] = 1
        report['cycles'].append(second)
        report.pop('native_probe')
        self.rejects(source, report)

    def test_drop_duplicate_reorder_and_stale_cycle(self):
        for field, replacement in [
            ('accepted_native_PTS', [0, 100000, 33333]),
            ('accepted_native_PTS', [0, 100000, 33333, 33333]),
            ('accepted_native_PTS', [0, 33333, 66666, 100000]),
            ('returned_native_PTS', [0, 100000, 33333, 66666]),
            ('returned_native_PTS', [1, 33334, 66667, 100001])]:
            with self.subTest(field=field, replacement=replacement):
                _, report = fixture(); report['cycles'][0][field] = replacement
                self.rejects(report=report)

    def test_expected_is_not_derived_from_observations(self):
        source, report = fixture()
        report['cycles'][0]['accepted_native_PTS'] = [0, 100001, 33334, 66667]
        report['cycles'][0]['returned_native_PTS'] = [0, 33334, 66667, 100001]
        self.rejects(source, report)
        source, report = fixture()
        source['packets'][1]['pts'] += 1
        self.rejects(source, report)

    def test_source_and_media_binding(self):
        self.rejects(kodi='0' * 40)
        self.rejects(media='b' * 64)
        _, report = fixture(); report['kodi_source'] = '0' * 40
        self.rejects(report=report)
        source, _ = fixture(); source['streams'][0]['codec_name'] = 'hevc'
        self.rejects(source=source)

    def test_zero_origin_is_required_at_all_three_fields(self):
        for location, field in [('format', 'start_time'), ('video', 'start_time'), ('video', 'start_pts')]:
            with self.subTest(location=location, field=field):
                source, _ = fixture()
                target = source['format'] if location == 'format' else source['streams'][0]
                target[field] = '1'
                self.rejects(source=source)
                target.pop(field)
                self.rejects(source=source)

    def test_malformed_clock_and_ticks(self):
        for value in ['0/1', '1/0', '-1/15360', '1/NaN', '1.0/15360', '1/2147483648', True, None]:
            with self.subTest(time_base=value):
                source, _ = fixture(); source['streams'][0]['time_base'] = value
                self.rejects(source=source)
        for value in ['N/A', 'NaN', 1.5, True, 1 << 63, -1]:
            with self.subTest(pts=value):
                source, _ = fixture(); source['packets'][1]['pts'] = value
                self.rejects(source=source)
        source, _ = fixture(); source['format']['start_time'] = 'NaN'
        self.rejects(source=source)

    def test_duplicate_source_ticks_and_clock_collisions(self):
        source, _ = fixture(); source['packets'][1]['pts'] = 0
        self.rejects(source=source)
        source, _ = fixture(); source['streams'][0]['time_base'] = '1/2000000000'
        self.rejects(source=source)

    def test_counts_cycle_indices_and_integer_types(self):
        mutations = [lambda r: r.update(requested_cycles=2),
                     lambda r: r.update(expected_frames_per_cycle=3),
                     lambda r: r['cycles'][0].update(cycle=1),
                     lambda r: r['cycles'][0].update(drm_frame_count=3),
                     lambda r: r['native_probe'].update({'@capture_nonempty': 3}),
                     lambda r: r['native_probe']['@h264_frames'].update({'1280,720,178': 3}),
                     lambda r: r['cycles'][0]['accepted_native_PTS'].__setitem__(0, False)]
        for mutate in mutations:
            _, report = fixture(); mutate(report)
            self.rejects(report=report)

    def test_other_audio_packets_do_not_define_video_expected_pts(self):
        source, report = fixture()
        source['streams'].append({'index': 1, 'codec_type': 'audio', 'codec_name': 'aac'})
        source['packets'].insert(1, {'stream_index': 1, 'codec_type': 'audio', 'pts': -1024})
        self.assertEqual(self.run_oracle(source, report)['source_video_packets'], 4)

    def test_raw_json_duplicates_nonfinite_and_bounds(self):
        source, report = fixture()
        duplicate = json.dumps(source).replace('"start_pts": 0', '"start_pts": 0, "start_pts": 0')
        self.rejects(source=duplicate)
        for value in [float('nan'), float('inf')]:
            source, _ = fixture(); source['unused'] = value
            self.rejects(source=json.dumps(source))
        self.rejects(source=b'{' + b' ' * ORACLE.MAX_JSON_BYTES + b'}')

    def test_offline_match_never_promotes_failed_native_execution(self):
        _, report = fixture(); report['status'] = 'FAIL'
        result = self.run_oracle(report=report)
        self.assertEqual(result['native_report_status_observed'], 'FAIL')
        self.assertEqual(result['scope'], 'OFFLINE_PTS_ONLY_NO_HARDWARE_OR_AV_PROMOTION')

    def test_900_exact_pts_and_898_renderer_acceptances_are_separate_results(self):
        source, report = fixture()
        source['streams'][0].update(time_base='1/1', nb_frames='900')
        source['packets'] = [{'stream_index': 0, 'pts': n} for n in range(900)]
        expected = [n * 1000000 for n in range(900)]
        report['expected_frames_per_cycle'] = 900
        report['cycles'][0].update(accepted_native_PTS=expected, returned_native_PTS=list(expected),
                                   drm_frame_count=900, renderer_acceptances=898, trace_end_byte=200000)
        report['native_probe'] = {'@capture_nonempty': 900, '@capture_LAST': 1,
                                  '@h264_frames': {'1280,720,178': 900}}
        result = self.run_oracle(source, report)
        self.assertEqual(result['status'], 'PASS_OFFLINE_SOURCE_CALLER_RETURNED_PTS_MATCH')
        self.assertEqual(result['tolerance_us'], 0)
        self.assertEqual(result['native_report_status_observed'], 'PASS')
        renderer = result['renderer_census']
        self.assertEqual(renderer['status'], 'FAIL')
        self.assertEqual(renderer['deficit'], 2)
        self.assertEqual(renderer['expected_total'], 900)
        self.assertEqual(renderer['renderer_acceptances_total'], 898)
        self.assertEqual(renderer['cycles'][0]['classification'], 'DEFICIT')
        self.assertEqual(renderer['cycles'][0]['deficit'], 2)
        self.assertIn('no PTS-bound presentation', renderer['scope'])

    def test_renderer_census_full_missing_invalid_and_excess(self):
        _, report = fixture()
        result = self.run_oracle(report=report)
        self.assertEqual(result['renderer_census']['status'], 'UNKNOWN')
        self.assertIsNone(result['renderer_census']['renderer_acceptances_total'])
        self.assertIsNone(result['renderer_census']['deficit'])
        for observed, status, classification in [(4, 'PASS', 'FULL'),
                                                 (5, 'FAIL', 'EXCESS'),
                                                 (-1, 'UNKNOWN', 'UNKNOWN'),
                                                 (True, 'UNKNOWN', 'UNKNOWN')]:
            with self.subTest(renderer_acceptances=observed):
                report['cycles'][0]['renderer_acceptances'] = observed
                result = self.run_oracle(report=report)
                self.assertEqual(result['status'], 'PASS_OFFLINE_SOURCE_CALLER_RETURNED_PTS_MATCH')
                self.assertEqual(result['renderer_census']['status'], status)
                self.assertEqual(result['renderer_census']['cycles'][0]['classification'], classification)

    def test_cli_requires_byte_hashes_and_emits_one_json_summary(self):
        source, report = fixture()
        with tempfile.TemporaryDirectory() as directory:
            source_path, report_path = Path(directory)/'packets.json', Path(directory)/'native.json'
            source_raw, report_raw = json.dumps(source).encode(), json.dumps(report).encode()
            source_path.write_bytes(source_raw); report_path.write_bytes(report_raw)
            args = ['--source-packets', str(source_path), '--source-packets-sha256', hashlib.sha256(source_raw).hexdigest(),
                    '--native-report', str(report_path), '--native-report-sha256', hashlib.sha256(report_raw).hexdigest(),
                    '--media-sha256', MEDIA, '--kodi-source', ORACLE.KODI_SOURCE]
            output = io.StringIO()
            with contextlib.redirect_stdout(output): self.assertEqual(ORACLE.main(args), 0)
            self.assertEqual(len(output.getvalue().splitlines()), 1)
            self.assertEqual(json.loads(output.getvalue())['source_video_packets'], 4)
            self.assertNotIn('requested_renderer_gate', json.loads(output.getvalue()))
            args[3] = '0' * 64
            output = io.StringIO()
            with contextlib.redirect_stdout(output): self.assertEqual(ORACLE.main(args), 2)
            self.assertIn('SHA-256 mismatch', json.loads(output.getvalue())['error'])

    def test_strict_renderer_gate_full_preserves_native_status(self):
        for native_status in ('PASS', 'FAIL'):
            with self.subTest(native_status=native_status):
                source, report = fixture()
                report['status'] = native_status
                report['cycles'][0]['renderer_acceptances'] = 4
                code, result = self.call_cli(source, report, strict=True)
                self.assertEqual(code, 0)
                self.assertEqual(result['status'], 'PASS_OFFLINE_SOURCE_CALLER_RETURNED_PTS_MATCH')
                self.assertEqual(result['native_report_status_observed'], native_status)
                self.assertEqual(result['requested_renderer_gate']['status'], 'PASS')
                self.assertIn('NO_RUNTIME_OR_AV_PROMOTION', result['requested_renderer_gate']['scope'])
                self.assertEqual(result['tolerance_us'], 0)

    def test_strict_renderer_gate_deficit_unknown_and_excess_exit_three(self):
        for observed, census_status, classification in [(2, 'FAIL', 'DEFICIT'),
                                                         (None, 'UNKNOWN', 'UNKNOWN'),
                                                         (5, 'FAIL', 'EXCESS')]:
            with self.subTest(renderer_acceptances=observed):
                source, report = fixture()
                if observed is not None:
                    report['cycles'][0]['renderer_acceptances'] = observed
                code, result = self.call_cli(source, report, strict=True)
                self.assertEqual(code, 3)
                self.assertEqual(result['status'], 'PASS_OFFLINE_SOURCE_CALLER_RETURNED_PTS_MATCH')
                self.assertEqual(result['native_report_status_observed'], 'PASS')
                self.assertEqual(result['requested_renderer_gate']['status'], 'FAIL')
                self.assertEqual(result['requested_renderer_gate']['renderer_census_status'], census_status)
                self.assertEqual(result['renderer_census']['cycles'][0]['classification'], classification)
                default_code, default_result = self.call_cli(source, report)
                self.assertEqual(default_code, 0)
                self.assertNotIn('requested_renderer_gate', default_result)

    def test_strict_renderer_gate_pts_and_input_failures_remain_exit_two(self):
        source, report = fixture()
        report['cycles'][0]['renderer_acceptances'] = 4
        report['cycles'][0]['accepted_native_PTS'][0] = 1
        for bad_pin in (False, True):
            with self.subTest(bad_source_pin=bad_pin):
                code, result = self.call_cli(source, report, strict=True, bad_source_pin=bad_pin)
                self.assertEqual(code, 2)
                self.assertEqual(result['status'], 'FAIL_OFFLINE_SOURCE_CALLER_RETURNED_PTS_MATCH')
                self.assertEqual(result['requested_renderer_gate']['status'], 'NOT_EVALUATED')


if __name__ == '__main__':
    unittest.main()
