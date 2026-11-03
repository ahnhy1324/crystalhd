#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure controls/oracle guards; no media runtime, devices or Git access."""
import ast,copy,importlib.util,sys,tempfile,unittest
from pathlib import Path
from types import SimpleNamespace
sys.dont_write_bytecode = True
PATH = Path(__file__).with_name('vaapi-centered-controls-stock.py')
spec = importlib.util.spec_from_file_location('centered_controls_stock_subject', PATH)
M = importlib.util.module_from_spec(spec); spec.loader.exec_module(M)
PID = 321
FIELDS = M.S.BINDING + ('surface', 'owner')

def trace():
    rows = []
    for n in range(1, 181):
        binding = dict(driver_instance=7, context=3, generation=1, token=n * 100000,
                       decode_identity=n * 2, submission_ordinal=n, surface=n, owner=n)
        for event in ('submit_bind', 'input_sent', 'output_dequeued', 'materialized'):
            seq = len(rows) + 1; rows.append((seq - 1, dict(binding, pid=PID, event=event, seq=seq, outcome=0)))
    return rows

def capture(ordinal, milliseconds=None):
    if milliseconds is None:milliseconds = ordinal * 1000 // 30
    clock = {'hours': 0, 'minutes': 0, 'seconds': milliseconds // 1000, 'milliseconds': milliseconds % 1000}
    result = {'status': 'PASS', 'source_ordinal': ordinal, 'source_pts_ticks': ordinal * 512,
              'source_time_base': '1/15360', 'source_case': 2, 'actual_media_PTS_to_VA_binding': 'NOT_OBSERVED'}
    metadata = {'format': 'BGRA', 'width': 1920, 'height': 1080, 'bytes': 1920 * 1080 * 4,
                'player_before': {'speed': 0, 'time': clock}, 'player_after': {'speed': 0, 'time': dict(clock)}}
    return result, metadata

def source():
    return {'streams': [{'codec_type': 'video', 'codec_name': 'h264', 'width': 1920, 'height': 1080,
        'coded_width': 1920, 'coded_height': 1088, 'field_order': 'progressive', 'pix_fmt': 'yuv420p',
        'color_range': 'tv', 'r_frame_rate': '30/1', 'time_base': '1/15360', 'has_b_frames': 0}],
        'packets': [{'pts': n * 512, 'dts': n * 512, 'duration': 512} for n in range(180)],
        'frames': [{'pts': n * 512, 'interlaced_frame': 0} for n in range(180)],
        'format': {'filename': str(M.FIXTURE), 'size': '8931575'}}

class CensusTests(unittest.TestCase):
    def audit(self, rows, complete=True):return M.source_census(rows, {'begin': 0, 'end': 100000}, PID, complete)
    def reject(self, rows, complete=True):
        try:result = self.audit(rows, complete)
        except (ValueError, KeyError, TypeError):return
        self.assertEqual(result['status'], 'FAIL', result)
    def test_progressive_natural180_positive(self):
        result = self.audit(trace()); self.assertEqual(result['status'], 'PASS', result)
        self.assertEqual(result['binding_stage_counts'], dict(submit_bind=180, input_sent=180, output_dequeued=180, materialized=180))
    def test_canceled_submissions_partial_not_complete_population(self):
        rows = [p for p in trace() if p[1]['submission_ordinal'] <= 8 or p[1]['event'] == 'submit_bind']
        self.assertEqual(self.audit(rows, False)['status'], 'PASS'); self.reject(rows, True)
    def test_partial_output_without_input_rejected(self):
        rows = trace(); del rows[1]; self.reject(rows, False)
    def test_materialized_without_output_rejected(self):
        rows = trace(); del rows[2]; self.reject(rows, False)
    def test_missing_or_repeated_progressive_dequeue_rejected(self):
        rows = trace(); del rows[2]; self.reject(rows)
        rows = trace(); extra = copy.deepcopy(rows[2][1]); extra['seq'] = 1000; rows.append((900, extra)); self.reject(rows)
    def test_balanced_dequeue_identity_swap_rejected(self):
        rows = trace()
        for f in FIELDS:rows[2][1][f], rows[6][1][f] = rows[6][1][f], rows[2][1][f]
        self.reject(rows)
    def test_each_original_identity_unique_per_lifetime(self):
        for f in ('token', 'decode_identity', 'submission_ordinal'):
            with self.subTest(field=f):
                rows = trace()
                for _, row in rows[4:8]:row[f] = rows[0][1][f]
                self.reject(rows)
    def test_public_alias_cannot_double_partial_original(self):
        rows = trace()
        for i, (_, row) in enumerate(copy.deepcopy(rows[:4])):
            row.update(surface=900, owner=900, seq=1000 + i); rows.append((900 + i, row))
        self.reject(rows, False)
    def test_cross_original_duplicate_sequence_rejected(self):
        rows = trace(); rows[4][1]['seq'] = rows[0][1]['seq']; self.reject(rows)
    def test_order_outcome_pid_public_owner_rejected(self):
        for change in ('order', 'outcome', 'pid', 'owner'):
            with self.subTest(change=change):
                rows = trace()
                if change == 'order':rows[1][1]['seq'], rows[2][1]['seq'] = rows[2][1]['seq'], rows[1][1]['seq']
                elif change == 'outcome':rows[2][1]['outcome'] = -1
                elif change == 'pid':rows[2][1]['pid'] += 1
                else:rows[2][1]['owner'] = 900
                self.reject(rows, False)
    def test_materialized_subset_has_no_renderer_zero_loss_gate(self):
        rows = [p for p in trace() if p[1]['event'] != 'materialized' or p[1]['submission_ordinal'] <= 2]
        result = self.audit(rows); self.assertEqual(result['status'], 'PASS', result); self.assertEqual(result['public_materialization_count'], 2)
    def test_identity_reuse_in_distinct_generation_allowed(self):
        rows = trace()[:4]
        for i, (_, row) in enumerate(copy.deepcopy(rows)):
            row.update(generation=2, seq=100 + i); rows.append((100 + i, row))
        self.assertEqual(self.audit(rows, False)['status'], 'PASS')

class CaptureTests(unittest.TestCase):
    def test_initial_and_target_content_positive(self):
        for n, label in ((5, 'initial-pause'), (90, 'seek-forward'), (105, 'seek-forward')):
            with self.subTest(ordinal=n):
                result, metadata = capture(n); self.assertEqual(M.verify_capture(result, metadata, label)['source_ordinal'], n)
    def test_stale_initial_content_at_seek_rejected(self):
        result, metadata = capture(5, 3000)
        with self.assertRaises(ValueError):M.verify_capture(result, metadata, 'seek-forward')
    def test_target_window_and_actual_clock_independent(self):
        for n, ms in ((110, 3666), (90, 2400)):
            with self.subTest(ordinal=n):
                result, metadata = capture(n, ms)
                with self.assertRaises(ValueError):M.verify_capture(result, metadata, 'seek-forward')

    def test_speed_hold_geometry_fixture_identity_rejected(self):
        for change in ('speed', 'hold', 'geometry', 'case', 'ticks', 'timebase', 'failed'):
            with self.subTest(change=change):
                result, metadata = capture(90)
                if change == 'speed':metadata['player_before']['speed'] = 1
                elif change == 'hold':metadata['player_after']['time']['milliseconds'] = 60
                elif change == 'geometry':metadata['width'] = 1280
                elif change == 'case':result['source_case'] = 3
                elif change == 'ticks':result['source_pts_ticks'] += 512
                elif change == 'timebase':result['source_time_base'] = '1/1000'
                else:result['status'] = 'FAIL'
                with self.assertRaises(ValueError):M.verify_capture(result, metadata, 'seek-forward')

class PreparationTests(unittest.TestCase):
    def test_independent_progressive_metadata_positive(self):
        self.assertEqual(M.source_metadata(source())['pictures'], 180)
    def test_coded_height_parity_count_pts_duration_association_rejected(self):
        for change in ('height', 'parity', 'count', 'pts', 'duration', 'association'):
            with self.subTest(change=change):
                document = source()
                if change == 'height':document['streams'][0]['coded_height'] = 1080
                elif change == 'parity':document['frames'][0]['interlaced_frame'] = 1
                elif change == 'count':document['packets'].pop()
                elif change == 'pts':document['frames'][1]['pts'] = 0
                elif change == 'duration':document['packets'][0]['duration'] = 1024
                else:document['format']['filename'] += '.other'
                with self.assertRaises(ValueError):M.source_metadata(document)
    def test_interleaved_probe_positive(self):
        document = source(); both = []
        for packet, frame in zip(document.pop('packets'), document.pop('frames')):
            both.extend((dict(packet, type='packet'), dict(frame, type='frame')))
        document['packets_and_frames'] = both
        self.assertEqual(M.source_metadata(document)['source_time_base'], '1/15360')

class ApplicationTests(unittest.TestCase):
    def test_complete_generated_script_parses_and_abort_releases_gate(self):
        script = M.application_script(); tree = ast.parse(script)
        functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('pump', 'wait')]
        module = ast.fix_missing_locations(ast.Module(body=functions, type_ignores=[]))
        with tempfile.TemporaryDirectory(prefix='centered-controls-abort-') as name:
            root = Path(name); sleeps = []
            def sleep(milliseconds):
                sleeps.append(milliseconds); (root / 'controller-abort.json').write_text('{}\n')
            namespace = {'R': root, 'state': {'events': []}, 'monitor': SimpleNamespace(abortRequested=lambda: False),
                         'xbmc': SimpleNamespace(sleep=sleep), 'time': SimpleNamespace(monotonic=lambda: 1.0)}
            exec(compile(module, '<OWN controls pump/wait>', 'exec'), namespace)
            with self.assertRaisesRegex(RuntimeError, 'owned controller aborted'):namespace['wait'](lambda: False, 25, 'retirement gate')
            self.assertEqual(sleeps, [20])

class StatusTests(unittest.TestCase):
    def row(self, **changes):
        value = dict(pid=PID, driver_instance=2, context=3, generation=1,
                     sealed=True, after_driver_unlock=True, directory_open=True, directory_errno=0,
                     retirement_status=0, reject_mask=0, selected_slots=2,
                     compose_committed_slots=2, public_committed_slots=2, materialized_slots=2,
                     eligible=True)
        value.update(changes); return value

    def test_complete_status_must_join_audited_manifest(self):
        self.assertEqual(M.qualify_statuses([self.row()], {(2, 3, 1)}, PID)[0]['status'], 'AUDITED')
        with self.assertRaises(ValueError):M.qualify_statuses([self.row()], set(), PID)
        with self.assertRaises(ValueError):M.qualify_statuses([], {(2, 3, 1)}, PID)

    def test_actual_ordinary_writer_shape_omits_reset_metadata(self):
        row = self.row()
        self.assertNotIn('publication_kind', row)
        self.assertEqual(M.qualify_statuses([row], {(2, 3, 1)}, PID)[0]['status'], 'AUDITED')
        with self.assertRaises(ValueError):
            M.qualify_statuses([self.row(publication_kind='unknown')], {(2, 3, 1)}, PID)

    def test_failed_selected_generation_cannot_hide_behind_good_generation(self):
        for flag in (1, 8, 16, 32, 64, 256, 1024, 2048, 4096, 8192):
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                M.qualify_statuses([self.row(), self.row(context=4, eligible=False, reject_mask=flag)], {(2, 3, 1)}, PID)

    def test_single_successful_slot_is_explicitly_unqualified(self):
        row = self.row(context=4, selected_slots=1, compose_committed_slots=1,
                       public_committed_slots=1, materialized_slots=1, eligible=False, reject_mask=128)
        values = M.qualify_statuses([self.row(), row], {(2, 3, 1)}, PID)
        self.assertEqual(values[1]['status'], 'NOT_QUALIFIED_INCOMPLETE')
        for field in ('compose_committed_slots', 'public_committed_slots', 'materialized_slots'):
            with self.subTest(field=field), self.assertRaises(ValueError):
                M.qualify_statuses([dict(row, **{field: 0})], set(), PID)

    def test_reset_status_requires_actual_close_and_generation(self):
        row = self.row(publication_kind='selected-decoder-generation-reset', next_generation=2,
                       close_status=0, device_closed=True)
        self.assertEqual(M.qualify_statuses([row], {(2, 3, 1)}, PID)[0]['status'], 'AUDITED')
        for changes in (dict(close_status=1), dict(device_closed=False), dict(next_generation=3)):
            with self.assertRaises(ValueError):M.qualify_statuses([dict(row, **changes)], {(2, 3, 1)}, PID)

    def test_duplicate_status_and_failed_retirement_rejected(self):
        with self.assertRaises(ValueError):M.qualify_statuses([self.row(), self.row()], {(2, 3, 1)}, PID)
        with self.assertRaises(ValueError):M.qualify_statuses([self.row(retirement_status=1)], {(2, 3, 1)}, PID)
        with self.assertRaises(ValueError):M.qualify_statuses([self.row(reject_mask=128)], {(2, 3, 1)}, PID)

    def test_rejected_unselected_candidates_do_not_replace_good_slots(self):
        row = self.row(reject_mask=2 | 4 | 512)
        self.assertEqual(M.qualify_statuses([row], {(2, 3, 1)}, PID)[0]['status'], 'AUDITED')


class SampleModeTests(unittest.TestCase):
    def test_prepare_default_and_explicit_target_mode(self):
        self.assertEqual(M.sample_option(True, None), 'first-two')
        self.assertEqual(M.sample_option(True, 'case2-90-91'), 'case2-90-91')
        self.assertIsNone(M.sample_option(False, None))
        for mode in M.SAMPLE_MODES:
            with self.subTest(mode=mode), self.assertRaises(ValueError):M.sample_option(False, mode)

    def test_unknown_or_missing_frozen_mode_rejected(self):
        for mode in (None, '', 'unknown'):
            with self.subTest(mode=mode), self.assertRaises(ValueError):M.validate_sample_mode(mode)

    def test_baseline_environment_stays_unset_and_target_is_explicit(self):
        self.assertEqual(M.sample_environment('first-two'), [])
        self.assertEqual(M.sample_environment('case2-90-91'),
                         ['--setenv', 'CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT', 'case2-90-91'])

    def test_manifest_selection_must_match_frozen_mode(self):
        M.validate_selection({}, 'first-two')
        M.validate_selection({'selection': 'case2-90-91'}, 'case2-90-91')
        for document, mode in (({}, 'case2-90-91'), ({'selection': 'first-two'}, 'case2-90-91'),
                               ({'selection': 'case2-90-91'}, 'first-two')):
            with self.subTest(document=document, mode=mode), self.assertRaises(ValueError):M.validate_selection(document, mode)

    def test_all_status_selections_are_checked(self):
        row = StatusTests().row(selection='case2-90-91')
        self.assertEqual(M.qualify_statuses([row], {(2, 3, 1)}, PID, 'case2-90-91')[0]['status'], 'AUDITED')
        with self.assertRaises(ValueError):M.qualify_statuses([row], {(2, 3, 1)}, PID)
        with self.assertRaises(ValueError):M.qualify_statuses([StatusTests().row()], {(2, 3, 1)}, PID, 'case2-90-91')
        incomplete = StatusTests().row(context=4, selected_slots=1, compose_committed_slots=1,
            public_committed_slots=1, materialized_slots=1, eligible=False, reject_mask=128)
        with self.assertRaises(ValueError):M.qualify_statuses([row, incomplete], {(2, 3, 1)}, PID, 'case2-90-91')

    def test_payload_target_requirement_is_only_added_in_target_mode(self):
        calls = []
        def audit(*args, **kwargs):calls.append((args, kwargs)); return {'status': 'PASS'}
        reader = SimpleNamespace(audit_payload=audit)
        M.audit_payload_for_mode(reader, 'manifest', 'trace', 'first-two')
        M.audit_payload_for_mode(reader, 'manifest', 'trace', 'case2-90-91')
        self.assertEqual(calls, [(('manifest', 'trace'), {}),
                                 (('manifest', 'trace'), {'required_targets': (90, 91)})])

    def test_target_reopen_preserves_source_population_and_separate_public_counts(self):
        complete = M.source_census(trace(), {'begin': 0, 'end': 100000}, PID, True)
        M.qualify_target_reopen(complete)
        for stage in ('submit_bind', 'input_sent', 'output_dequeued'):
            with self.subTest(stage=stage):
                changed = copy.deepcopy(complete); changed['binding_stage_counts'][stage] = 179
                with self.assertRaises(ValueError):M.qualify_target_reopen(changed)
        partial = M.source_census([p for p in trace() if p[1]['event'] != 'materialized'], {'begin': 0, 'end': 100000}, PID, True)
        self.assertEqual(partial['status'], 'PASS')  # Existing baseline contract.
        M.qualify_target_reopen(partial)  # Target samples retain their separate full-payload gate.


class TargetTimingTests(unittest.TestCase):
    def records(self, ordinals=(90, 91)):
        return [{'manifest': 'manifest', 'trace_prefix': 'trace', 'audit': {'status': 'PASS',
                 'selected_source_bindings': [{'source_ordinal': n} for n in ordinals]}}]

    def test_exact_target_content_and_request_window_forwarded_to_reader(self):
        calls = []
        def audit(*args):calls.append(args); return {'status': 'PASS'}
        records = self.records()
        result = M.audit_target_phase(SimpleNamespace(audit_target_window=audit), records, 100, 200)
        self.assertEqual(result['selected_target_bindings'], 2)
        self.assertEqual(result['actual_media_PTS_to_VA_binding'], 'NOT_OBSERVED')
        self.assertEqual(calls, [(records[0]['audit'], 'trace', 100, 200)])

    def test_missing_repeated_extra_or_candidate_ordinals_rejected(self):
        def unexpected(*args):self.fail('unproven target population must precede the timing API')
        reader = SimpleNamespace(audit_target_window=unexpected)
        for ordinals in ((90,), (90, 90), (90, 91, 92), (0, 1), (29, 30)):
            with self.subTest(ordinals=ordinals), self.assertRaises(ValueError):M.audit_target_phase(reader, self.records(ordinals), 100, 200)

    def test_reversed_or_invalid_seek_stop_window_rejected(self):
        reader = SimpleNamespace(audit_target_window=lambda *args: self.fail('invalid request window reached reader'))
        for seek, stop in ((0, 200), (200, 100), (100, 100), (True, 200), (None, 200)):
            with self.subTest(seek=seek, stop=stop), self.assertRaises(ValueError):M.audit_target_phase(reader, self.records(), seek, stop)

    def test_reader_rejection_for_submit_materialization_or_sync_is_fatal(self):
        for stage in ('submit', 'materialized', 'sync'):
            with self.subTest(stage=stage):
                reader = SimpleNamespace(audit_target_window=lambda *args: {'status': 'FAIL', 'error': stage + ' outside seek/Stop window'})
                with self.assertRaisesRegex(ValueError, 'timing audit failed'):M.audit_target_phase(reader, self.records(), 100, 200)


if __name__ == '__main__':unittest.main()
