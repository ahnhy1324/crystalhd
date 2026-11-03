#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Device-free metadata/lifetime controls; no external decoder or hardware."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

_spec = importlib.util.spec_from_file_location('frame_pts_reader_subject', Path(__file__).with_name('vaapi-frame-pts-reader.py'))
H = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(H)


def binding(number=0):
    return {'context': 4, 'generation': 1, 'surface': 5, 'owner': 8,
            'token': (number + 1) * 100000, 'decode_identity': 54 + number * 2,
            'submission_ordinal': 27 + number}


def row(seq, event, now, b=None, *, instance=2, pid=123):
    result = {'schema': H.P.TRACE_SCHEMA, 'pid': pid, 'driver_instance': instance,
              'session': instance, 'seq': seq, 'ts_monotonic_ns': now,
              'event': event, 'operation': 0, 'outcome': 0}
    result.update({key: 0 for key in H.P.BINDING_KEYS})
    result.update(b or {})
    return result


def fixture():
    header = {'type': 'header', 'schema': H.SCHEMA, 'pid': 123, 'capacity': 8192,
              'provider_ok': True, 'codec_version': 4129126, 'util_version': 3998054,
              'codec_provider': '/original/App/lib/libavcodec.so.63',
              'util_provider': '/original/App/lib/libavutil.so.61', 'vaapi_format': 44}
    frames = []
    for seq, begin, pts in ((1, 150, 3000000), (2, 350, 3033333)):
        frames.append({'type': 'frame', 'seq': seq, 'pid': 123, 'tid': 124,
                       'snapshot_begin_ns': begin, 'snapshot_end_ns': begin + 10,
                       'codec': '0x1234', 'frame': '0x5678', 'pool': '0x9abc', 'display': None,
                       'surface': 5, 'format': 44, 'pts': pts, 'best_effort_timestamp': pts,
                       'frame_time_base': [0, 1], 'packet_time_base': [1, 1000000]})
    footer = {'type': 'footer', 'records': 2, 'dropped': 0, 'complete': True,
              'provider_ok': True, 'errors': []}
    trace = [row(1, 'submit_bind', 100, binding(0)),
             row(2, 'materialized', 180, binding(0)),
             row(3, 'binding_end', 250, binding(0)),
             row(4, 'submit_bind', 300, binding(1)),
             row(5, 'materialized', 380, binding(1)),
             row(6, 'binding_end', 450, binding(1)),
             row(7, 'context_destroy', 500, {'context': 4, 'generation': 1}),
             row(8, 'session_destroy', 600)]
    selected = [dict(binding=binding(n), driver_instance=2, phase='reopen',
                     source_ordinal=90 + n, source_pts_ticks=(90 + n) * 512,
                     source_time_base='1/15360') for n in range(2)]
    return [header, *frames, footer], trace, selected


class FramePtsTests(unittest.TestCase):
    def setUp(self):
        self.report, self.trace, self.selected = fixture()

    def check(self, selected=False):
        return H.check_document(self.report, self.trace, self.selected if selected else None)

    def test_actual_surface_reuse_joins_distinct_bindings_and_rounded_clocks(self):
        result = self.check(True)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual([r['binding']['submission_ordinal'] for r in result['joins']], [27, 28])
        self.assertEqual([r['source_ordinal'] for r in result['selected_joins']], [90, 91])
        delta = result['selected_joins'][1]['source_comparisons']['pts']
        self.assertEqual(delta['delta_seconds'], '-1/3000000')
        self.assertEqual(delta['tolerance_seconds'], '1/1000000')
        counts = result['lifetime_counts'][0]
        self.assertEqual((counts['frame_returns'], counts['unique_bindings'], counts['clock_observed']), (2, 2, 2))

    def test_receive_before_materialization_is_valid(self):
        self.assertGreater(self.trace[1]['ts_monotonic_ns'], self.report[1]['snapshot_end_ns'])
        self.assertEqual(self.check()['status'], 'PASS')

    def test_pointer_reuse_is_descriptive_and_cannot_replace_surface_join(self):
        self.report[2]['surface'] = 7
        with self.assertRaisesRegex(ValueError, 'unique active'):
            self.check()

    def test_snapshot_crossing_or_touching_boundary_rejected(self):
        for begin, end in ((95, 105), (100, 110), (240, 250), (245, 305), (255, 260)):
            with self.subTest(begin=begin, end=end):
                self.report[1].update(snapshot_begin_ns=begin, snapshot_end_ns=end)
                with self.assertRaisesRegex(ValueError, 'boundary|unique active'):
                    self.check()

    def test_reset_destroy_and_surface_retirement_boundaries_rejected(self):
        for event, metadata in (('context_reset', {'context': 4, 'generation': 2}),
                                ('context_destroy', {'context': 4, 'generation': 1}),
                                ('surface_destroy', {'surface': 5})):
            with self.subTest(event=event):
                self.report, self.trace, self.selected = fixture()
                self.trace[1] = row(2, event, 155, metadata)
                with self.assertRaisesRegex(ValueError, 'boundary|context_destroy'):
                    self.check()

    def test_other_driver_instance_collision_rejected(self):
        self.trace += [row(1, 'submit_bind', 110, binding(), instance=3),
                       row(2, 'context_destroy', 190, {'context': 4, 'generation': 1}, instance=3),
                       row(3, 'session_destroy', 200, instance=3)]
        with self.assertRaisesRegex(ValueError, 'unique active'):
            self.check()

    def test_other_instance_nonoverlap_and_other_pid_are_not_collisions(self):
        self.trace += [row(1, 'submit_bind', 10, binding(), instance=3),
                       row(2, 'context_destroy', 40, {'context': 4, 'generation': 1}, instance=3),
                       row(3, 'session_destroy', 50, instance=3),
                       row(1, 'submit_bind', 110, binding(), instance=3, pid=999)]
        self.assertEqual(self.check()['driver_instances_considered'], [2, 3])

    def test_unrelated_instance_with_missing_trace_tail_still_rejected(self):
        self.trace += [row(1, 'submit_bind', 10, binding(), instance=3)]
        with self.assertRaisesRegex(ValueError, 'context retirement'):
            self.check()

    def test_missing_duplicated_regressed_trace_rows_rejected(self):
        for change in ('missing', 'duplicate', 'time', 'tail'):
            with self.subTest(change=change):
                self.report, self.trace, self.selected = fixture()
                if change == 'missing': self.trace.pop(1)
                if change == 'duplicate': self.trace.insert(1, copy.deepcopy(self.trace[0]))
                if change == 'time': self.trace[1]['ts_monotonic_ns'] = 99
                if change == 'tail': self.trace = self.trace[:-2]
                with self.assertRaises(ValueError): self.check()

    def test_all_decoder_retirements_suffice_without_display_destruction(self):
        self.trace.pop()
        self.trace.append(row(8, 'surface_destroy', 550, {'surface': 5}))
        result = self.check(True)
        self.assertEqual(result['status'], 'PASS')
        self.assertFalse(result['instance_coverage'][0]['session_destroy_observed'])
        self.assertEqual(result['instance_coverage'][0]['last_event'], 'surface_destroy')
        retired = result['decoder_retirement_coverage']
        self.assertEqual(retired['submitted_decoder_lifetimes'], 1)
        self.assertEqual(retired['retired_decoder_lifetimes'], 1)
        self.assertEqual(retired['lifetimes'][0]['terminal_context_destroy_outcome'], 0)

    def test_binding_end_or_display_destroy_cannot_substitute_for_decoder_close(self):
        for tail in ('binding_end', 'session_destroy'):
            with self.subTest(tail=tail):
                self.report, self.trace, self.selected = fixture()
                self.trace.pop(6)
                if tail == 'binding_end': self.trace.pop()
                else: self.trace[-1]['seq'] = 7
                with self.assertRaisesRegex(ValueError, 'context retirement'):
                    self.check()

    def test_failed_or_wrong_generation_decoder_retirement_rejected(self):
        for field, value in (('outcome', 1), ('generation', 2)):
            with self.subTest(field=field):
                self.report, self.trace, self.selected = fixture()
                self.trace[6][field] = value
                with self.assertRaisesRegex(ValueError, 'successful matching context_destroy'):
                    self.check()

    def test_unobserved_initial_generation_or_generation_regression_rejected(self):
        for prior_reset in (False, True):
            with self.subTest(prior_reset=prior_reset):
                self.report, self.trace, self.selected = fixture()
                if prior_reset:
                    self.trace.insert(0, row(1, 'context_reset', 50, {'context': 4, 'generation': 2}))
                    for seq, entry in enumerate(self.trace, 1): entry['seq'] = seq
                else:
                    for entry in self.trace:
                        if entry['context'] == 4: entry['generation'] = 2
                with self.assertRaisesRegex(ValueError, 'observed lifetime boundary'):
                    self.check()

    def test_reset_bounds_epoch_with_later_terminal_close_without_inventing_reset_close(self):
        self.trace[2] = row(3, 'context_reset', 250, {'context': 4, 'generation': 2})
        for entry in self.trace[3:7]: entry['generation'] = 2
        self.selected[1]['binding']['generation'] = 2
        result = self.check(True)
        retired = result['decoder_retirement_coverage']['lifetimes']
        self.assertEqual(len(retired), 2)
        self.assertEqual(retired[0]['retirement_event'], 'context_reset')
        self.assertEqual(retired[0]['generation_reset_close'], 'NOT_OBSERVED_FROM_TRACE')
        self.assertEqual(retired[0]['terminal_context_destroy_generation'], 2)
        self.trace.pop(6)
        self.trace[-1]['seq'] = 7
        with self.assertRaisesRegex(ValueError, 'context retirement'):
            self.check()

    def test_no_pts_and_unknown_timebase_remain_explicitly_unobserved(self):
        self.report[1]['pts'] = H.NOPTS
        self.report[2]['packet_time_base'] = [0, 1]
        result = self.check()
        self.assertFalse(result['joins'][0]['clocks']['pts']['observed'])
        self.assertIsNone(result['joins'][0]['clocks']['pts']['seconds'])
        self.assertTrue(result['joins'][1]['clocks']['pts']['observed'])
        self.assertIsNone(result['joins'][1]['clocks']['pts']['seconds'])
        self.assertEqual(result['lifetime_counts'][0]['clock_observed'], 0)
        with self.assertRaisesRegex(ValueError, 'unobserved'):
            self.check(True)

    def test_selected_raw_pts_binding_survives_undeclared_units(self):
        self.report[1]['packet_time_base'] = self.report[2]['packet_time_base'] = [0, 1]
        result = self.check(True)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['selected_clock_status'], 'NOT_OBSERVED_UNDECLARED_TIME_BASE')
        self.assertEqual(result['selected_clock_qualified_bindings'], 0)
        self.assertEqual(result['lifetime_counts'][0]['raw_pts_observed'], 2)
        self.assertEqual(result['lifetime_counts'][0]['clock_observed'], 0)
        comparison = result['selected_joins'][0]['source_comparisons']['pts']
        self.assertEqual(comparison['raw'], 3000000)
        self.assertIsNone(comparison['actual_seconds'])
        self.assertIsNone(comparison['delta_seconds'])

    def test_empty_instance_is_validated_and_reported(self):
        self.trace += [row(1, 'context_destroy', 10, instance=3),
                       row(2, 'session_destroy', 50, instance=3)]
        self.assertEqual(self.check()['driver_instances_considered'], [2, 3])

    def test_duplicate_original_token_identity_or_ordinal_rejected(self):
        for name in ('token', 'decode_identity', 'submission_ordinal'):
            with self.subTest(name=name):
                self.report, self.trace, self.selected = fixture()
                self.trace[3][name] = self.trace[0][name]
                with self.assertRaisesRegex(ValueError, 'duplicated original'):
                    self.check()

    def test_frame_timebase_precedence_preserves_declared_basis(self):
        self.report[1]['frame_time_base'] = [1, 30]
        self.report[1]['pts'] = self.report[1]['best_effort_timestamp'] = 90
        clock = self.check(True)['selected_joins'][0]['clocks']['pts']
        self.assertEqual((clock['time_base_source'], clock['seconds']), ('frame_time_base', '3'))

    def test_selected_pts_mismatch_or_more_than_one_tick_rejected(self):
        for delta in (-2, 2, 33333):
            with self.subTest(delta=delta):
                self.report, self.trace, self.selected = fixture()
                self.report[1]['pts'] += delta
                with self.assertRaisesRegex(ValueError, 'disagrees'):
                    self.check(True)

    def test_selected_wrong_instance_binding_or_duplicate_rejected(self):
        for change in ('instance', 'binding', 'duplicate', 'pid'):
            with self.subTest(change=change):
                self.report, self.trace, self.selected = fixture()
                if change == 'instance': self.selected[0]['driver_instance'] = 3
                if change == 'binding': self.selected[0]['binding']['token'] += 1
                if change == 'duplicate': self.selected.append(copy.deepcopy(self.selected[0]))
                if change == 'pid': self.selected[0]['pid'] = 999
                with self.assertRaises(ValueError): self.check(True)

    def test_selected_duplicate_returns_do_not_claim_unique_payload(self):
        self.report.insert(2, copy.deepcopy(self.report[1]))
        for seq, frame in enumerate(self.report[1:-1], 1): frame['seq'] = seq
        self.report[-1]['records'] = 3
        self.assertEqual(self.check()['lifetime_counts'][0]['unique_bindings'], 2)
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            self.check(True)

    def test_provider_overflow_and_incomplete_report_rejected(self):
        for target, field, value in ((0, 'provider_ok', False), (0, 'codec_version', 62 << 16),
                                     (0, 'util_version', 60 << 16), (0, 'codec_provider', ''),
                                     (-1, 'dropped', 1), (-1, 'complete', False),
                                     (-1, 'errors', ['provider']), (-1, 'records', 1)):
            with self.subTest(field=field, value=value):
                self.report, self.trace, self.selected = fixture()
                self.report[target][field] = value
                with self.assertRaises(ValueError): self.check()

    def test_bad_metadata_and_sequence_rejected(self):
        for field, value in (('seq', 2), ('pid', 999), ('tid', 0), ('snapshot_end_ns', 149),
                             ('surface', 0xffffffff), ('surface', True), ('format', 0),
                             ('pts', 1 << 63), ('pts', 3.0), ('packet_time_base', [1]),
                             ('codec', 'untrusted'), ('display', '0x1234')):
            with self.subTest(field=field, value=value):
                self.report, self.trace, self.selected = fixture()
                self.report[1][field] = value
                with self.assertRaises(ValueError): self.check()

    def test_file_api_and_truncated_or_duplicate_json_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            report, trace = root / 'frames.jsonl', root / 'stdout'
            raw = ''.join(json.dumps(r) + '\n' for r in self.report)
            report.write_text(raw)
            trace.write_text('ordinary log line\n' + ''.join(json.dumps(r) + '\n' for r in self.trace))
            self.assertEqual(H.audit_frames(report, trace, self.selected)['status'], 'PASS')
            for damaged in (raw[:-1], raw.replace('"capacity": 8192', '"capacity": 8192, "capacity": 8192')):
                report.write_text(damaged)
                self.assertEqual(H.audit_frames(report, trace)['status'], 'FAIL')

    def test_terminal_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            real, link = root / 'real', root / 'link'
            real.write_text(''.join(json.dumps(r) + '\n' for r in self.report))
            link.symlink_to(real)
            self.assertEqual(H.audit_frames(link, real)['status'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
