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


class FramePtsModeTests(unittest.TestCase):
    def test_observer_is_optional_and_prepare_only(self):
        self.assertFalse(M.frame_pts_option(True, False))
        self.assertTrue(M.frame_pts_option(True, True))
        self.assertIsNone(M.frame_pts_option(False, False))
        with self.assertRaisesRegex(ValueError, 'prepare-plan'):M.frame_pts_option(False, True)
        for value in (None, 0, 1, 'true'):
            with self.subTest(value=value), self.assertRaises(ValueError):M.validate_frame_pts_mode(value)

    def test_only_explicit_mode_injects_one_own_preload(self):
        source = Path('/own-source')
        self.assertEqual(M.frame_pts_environment(source, False), [])
        command = M.frame_pts_environment(source, True)
        self.assertEqual(command, ['--ro-bind', '/own-source/' + str(M.FRAME_PTS_IMAGE), M.FRAME_PTS_MOUNT,
            '--setenv', 'LD_PRELOAD', M.FRAME_PTS_MOUNT,
            '--setenv', 'CRYSTALHD_TEST_FRAME_PTS', 'v1',
            '--setenv', 'CRYSTALHD_TEST_FRAME_PTS_PREFIX', '/payload/frame-pts'])
        self.assertNotIn('CRYSTALHD_TEST_FRAME_PTS_CODEC_PROVIDER', command)
        self.assertNotIn('CRYSTALHD_TEST_FRAME_PTS_UTIL_PROVIDER', command)

    def test_optional_artifacts_are_frozen_without_affecting_baseline(self):
        G = SimpleNamespace(ROUTE_PATH=Path('/route'), BOUNDARY=Path('/boundary'), WESTON=Path('/weston'))
        source = Path('/own-source'); platform = Path('/platform')
        ordinary = M.frozen_files(source, G, platform)
        observed = M.frozen_files(source, G, platform, True)
        self.assertEqual(observed[:len(ordinary)], ordinary)
        self.assertEqual(observed[len(ordinary):], [M.FRAME_PTS_READER,
            source / 'tests/vaapi-frame-pts-probe.c', source / 'tests/vaapi-frame-pts-probe.map', source / M.FRAME_PTS_IMAGE])
        self.assertNotIn(source / M.FRAME_PTS_IMAGE, ordinary)

    def records(self):
        return [{'phase': phase, 'audit': {'status': 'PASS', 'payload_audit': {'driver_instance': 7},
                 'selected_source_bindings': [{'binding': {'context': context, 'generation': 1},
                   'source_ordinal': ordinal, 'source_pts_ticks': ordinal * 512, 'source_time_base': '1/15360'}
                    for ordinal in (90, 91)]}} for phase, context in (('interrupted', 3), ('reopen', 4))]

    def result(self):
        return {'status': 'PASS', 'pid': PID, 'selected_joins': [{}, {}, {}, {}], 'lifetime_counts': [
            {'driver_instance': 7, 'context': 3, 'generation': 1, 'frame_returns': 34, 'unique_bindings': 34, 'raw_pts_observed': 34, 'clock_observed': 34},
            {'driver_instance': 7, 'context': 4, 'generation': 1, 'frame_returns': 180, 'unique_bindings': 180, 'raw_pts_observed': 180, 'clock_observed': 180}]}

    def test_reader_gets_full_trace_and_independently_qualified_source_bindings(self):
        records = self.records(); result = self.result(); calls = []
        def audit(*args, **kwargs):calls.append((args, kwargs)); return result
        self.assertIs(M.audit_frame_pts(SimpleNamespace(audit_frames=audit), 'report', 'full.trace', records, PID), result)
        args, kwargs = calls[0]; self.assertEqual(args, ('report', 'full.trace'))
        selected = kwargs['selected_bindings']; self.assertEqual(len(selected), 4)
        self.assertEqual([row['phase'] for row in selected], ['interrupted', 'interrupted', 'reopen', 'reopen'])
        self.assertTrue(all(row['driver_instance'] == 7 for row in selected))
        self.assertTrue(all(row['pid'] == PID for row in selected))
        self.assertEqual([row['source_ordinal'] for row in selected], [90, 91, 90, 91])

    def test_missing_join_clock_or_incomplete_natural_return_population_rejected(self):
        for change in ('status', 'selected', 'returns', 'unique', 'raw_pts', 'lifetime'):
            with self.subTest(change=change):
                result = self.result()
                if change == 'status':result['status'] = 'FAIL'
                elif change == 'selected':result['selected_joins'].pop()
                elif change == 'lifetime':result['lifetime_counts'][1]['generation'] = 2
                else:result['lifetime_counts'][1][{'returns': 'frame_returns', 'unique': 'unique_bindings', 'raw_pts': 'raw_pts_observed'}[change]] = 179
                with self.assertRaises(ValueError):
                    M.audit_frame_pts(SimpleNamespace(audit_frames=lambda *args, **kwargs: result), 'report', 'trace', self.records(), PID)

    def test_failed_source_payload_never_reaches_frame_reader(self):
        records = self.records(); records[0]['audit']['status'] = 'FAIL'
        reader = SimpleNamespace(audit_frames=lambda *args, **kwargs: self.fail('unqualified source reached PTS join'))
        with self.assertRaisesRegex(ValueError, 'independently qualified'):M.audit_frame_pts(reader, 'report', 'trace', records, PID)

    def test_natural_actual_clock_census_requires_integer_counts(self):
        for key in ('frame_returns', 'unique_bindings', 'raw_pts_observed', 'clock_observed'):
            for value in (True, 180.0, '180', -180, None):
                with self.subTest(key=key, value=value):
                    result = self.result(); result['lifetime_counts'][1][key] = value
                    with self.assertRaises(ValueError):
                        M.audit_frame_pts(SimpleNamespace(audit_frames=lambda *args, **kwargs: result), 'report', 'trace', self.records(), PID)

    def test_undeclared_timebase_preserves_actual_raw_pts_binding_observation(self):
        result = self.result(); result['lifetime_counts'][1]['clock_observed'] = 0
        result['selected_clock_status'] = 'NOT_OBSERVED_UNDECLARED_TIME_BASE'
        self.assertIs(M.audit_frame_pts(SimpleNamespace(audit_frames=lambda *args, **kwargs: result),
                                       'report', 'trace', self.records(), PID), result)
        for value in (-1, 181):
            with self.subTest(value=value):
                result['lifetime_counts'][1]['clock_observed'] = value
                with self.assertRaises(ValueError):
                    M.audit_frame_pts(SimpleNamespace(audit_frames=lambda *args, **kwargs: result), 'report', 'trace', self.records(), PID)

    def test_report_path_uses_verified_owned_pid_without_child_or_legacy_fallback(self):
        with tempfile.TemporaryDirectory() as name:
            payload = Path(name)
            (payload / 'frame-pts.jsonl').write_text('legacy child report')
            (payload / 'frame-pts-999.jsonl').write_text('another process report')
            expected = payload / ('frame-pts-' + str(PID) + '.jsonl')
            self.assertEqual(M.frame_pts_report_path(payload, PID), expected)
            self.assertFalse(expected.exists())
        for value in (True, 0, -1, '321', None):
            with self.subTest(value=value), self.assertRaises(ValueError):M.frame_pts_report_path(Path('/payload'), value)

    def test_successful_foreign_report_pid_cannot_qualify_owned_application(self):
        for foreign in (PID + 1, True, float(PID), str(PID), None):
            with self.subTest(pid=foreign):
                result = self.result(); result['pid'] = foreign
                with self.assertRaisesRegex(ValueError, 'verified owned application'):
                    M.audit_frame_pts(SimpleNamespace(audit_frames=lambda *args, **kwargs: result),
                                      'foreign-report', 'trace', self.records(), PID)


class RenderRouteTests(unittest.TestCase):
    def test_va_default_and_cpu_copy_are_frozen_prepare_only(self):
        self.assertEqual(M.render_route_option(True, None), 'va')
        self.assertEqual(M.render_route_option(True, 'cpu-copy'), 'cpu-copy')
        self.assertIsNone(M.render_route_option(False, None))
        for route in M.RENDER_ROUTES:
            with self.subTest(route=route), self.assertRaises(ValueError):M.render_route_option(False, route)
        for value in (None, '', 'fallback', True, 1):
            with self.subTest(route=value), self.assertRaises(ValueError):M.validate_render_route(value)

    def test_default_profile_has_no_extra_logging_or_copy_provider_requirement(self):
        self.assertEqual(M.render_route_settings('va'), {'videoplayer.prefervaapirender': 'true'})
        self.assertEqual(M.render_route_files(Path('/original-app'), 'va'), [])
        cpu = M.render_route_settings('cpu-copy')
        self.assertEqual(cpu['videoplayer.prefervaapirender'], 'false')
        self.assertEqual(cpu['debug.extralogging'], 'true')
        self.assertEqual(cpu['debug.setextraloglevel'], '32768')
        self.assertEqual(M.render_route_files(Path('/original-app'), 'cpu-copy'),
                         [Path('/original-app/lib/kodi/system/libsse4-x86_64-linux.so')])
        self.assertEqual(M.scenario_scope('va', False), M.SCOPE)
        self.assertEqual(M.scenario_scope('va', True), M.FRAME_PTS_SCOPE)
        self.assertEqual(M.scenario_scope('cpu-copy', True), M.CPU_COPY_SCOPE)

    def test_only_cpu_copy_script_queries_settings_at_both_paused_identities(self):
        self.assertNotIn('Settings.GetSettingValue', M.application_script())
        script = M.application_script('cpu-copy'); tree = ast.parse(script)
        helper = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'render_route_settings']
        self.assertEqual(len(helper), 1)
        module = ast.fix_missing_locations(ast.Module(body=helper, type_ignores=[])); calls = []
        wanted = {'videoplayer.prefervaapirender': False, 'debug.extralogging': True, 'debug.setextraloglevel': [32768]}
        def rpc(method, params):
            calls.append((method, params)); return {'value': wanted[params['setting']]}
        namespace = {'rpc': rpc}; exec(compile(module, '<OWN route settings>', 'exec'), namespace)
        self.assertEqual(namespace['render_route_settings'](), wanted)
        self.assertTrue(all(method == 'Settings.GetSettingValue' for method, _ in calls))
        identity_gates = [n for n in ast.walk(tree) if isinstance(n, ast.Call) and
            isinstance(n.func, ast.Name) and n.func.id == 'gate' and n.args and
            isinstance(n.args[0], ast.Constant) and n.args[0].value in ('initial-ready', 'reopen-identity')]
        self.assertEqual(len(identity_gates), 2)
        self.assertTrue(all(any(k.arg == 'render_route_settings' for k in n.keywords) for n in identity_gates))

    def test_runtime_setting_types_and_values_must_match_original_route(self):
        expected = {'videoplayer.prefervaapirender': False, 'debug.extralogging': True, 'debug.setextraloglevel': [32768]}
        self.assertEqual(M.qualify_cpu_copy_settings(expected), expected)
        for key, value in (('videoplayer.prefervaapirender', True), ('videoplayer.prefervaapirender', 0),
                           ('debug.extralogging', False), ('debug.extralogging', 1),
                           ('debug.setextraloglevel', '32768'), ('debug.setextraloglevel', [32768.0]),
                           ('debug.setextraloglevel', [32768, 64]), ('debug.setextraloglevel', [])):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                M.qualify_cpu_copy_settings(dict(expected, **{key: value}))


class CpuCopyRouteLogTests(unittest.TestCase):
    def line(self, message, tid=777, component='video', level='debug'):
        return ('2026-11-03 13:00:00.000 T:' + str(tid) + '   ' + level +
                ' <' + component + '>: ' + message + '\n').encode()

    def header(self, bom=True, pid=PID):
        return (b'\xef\xbb\xbf' if bom else b'') + self.line('Starting Kodi', pid, 'general', 'info')

    def pair(self, tid=777):
        return self.line(M.CPU_COPY_INIT.decode(), tid) + self.line(M.CPU_COPY_READY.decode(), tid)

    def audit(self, records, prefix=None):
        prefix = self.header() if prefix is None else prefix
        raw = prefix + records
        return M.audit_cpu_copy_route_log(raw, len(prefix), len(raw), PID)

    def test_completed_cpu_copy_accepts_worker_tid_and_retains_byte_window(self):
        prefix = self.header(); raw = prefix + self.pair()
        result = self.audit(self.pair())
        self.assertEqual(result['status'], 'PASS'); self.assertEqual(result['owned_pid'], PID)
        self.assertEqual(result['completed_initializations'], [{'tid': 777,
            'initializing_offset': len(prefix), 'completed_offset': len(prefix) + len(self.line(M.CPU_COPY_INIT.decode()))}])
        self.assertEqual(result['end'], len(raw))

    def test_multiple_completed_reinitializations_are_allowed(self):
        result = self.audit(self.pair() + self.pair(888))
        self.assertEqual(len(result['completed_initializations']), 2)

    def test_initialization_intent_or_completion_alone_cannot_qualify(self):
        cases = (self.line(M.CPU_COPY_INIT.decode()), self.line(M.CPU_COPY_READY.decode()),
                 self.line(M.CPU_COPY_READY.decode()) + self.line(M.CPU_COPY_INIT.decode()),
                 self.line(M.CPU_COPY_INIT.decode(), 777) + self.line(M.CPU_COPY_READY.decode(), 778))
        for raw in cases:
            with self.subTest(raw=raw), self.assertRaises(ValueError):self.audit(raw)

    def test_previous_play_markers_cannot_qualify_fresh_reopen(self):
        old = self.header() + self.pair()
        with self.assertRaises(ValueError):self.audit(self.line('unrelated record'), old)
        self.assertEqual(self.audit(self.pair(888), old)['completed_initializations'][0]['tid'], 888)

    def test_wrong_context_or_quoted_marker_cannot_qualify(self):
        for records in (self.line(M.CPU_COPY_INIT.decode(), component='general') + self.pair(),
                        self.line(M.CPU_COPY_INIT.decode(), level='info') + self.pair(),
                        self.line('quoted ' + M.CPU_COPY_INIT.decode()) + self.line('quoted ' + M.CPU_COPY_READY.decode())):
            with self.subTest(records=records), self.assertRaises(ValueError):self.audit(records)

    def test_other_route_fallback_preinit_and_general_errors_reject(self):
        for message, component, level in (
            ('VAAPI output: Initializing skip postproc', 'video', 'debug'),
            ('VAAPI output: Initializing skip postproc as fallback', 'video', 'debug'),
            ('VAAPI output: Initializing vaapi postproc', 'video', 'debug'),
            ('VAAPI output: Postproc preinit failed', 'general', 'error'),
            ('VAAPI/ffpp error: mapping failed', 'general', 'error'),
            ('VAAPI::CFFmpegPostproc::Init  - deinterlacing', 'video', 'debug')):
            with self.subTest(message=message), self.assertRaises(ValueError):
                self.audit(self.pair() + self.line(message, component=component, level=level))

    def test_foreign_startup_or_misaligned_bounds_reject(self):
        with self.assertRaises(ValueError):self.audit(self.pair(), self.header(pid=PID + 1))
        prefix = self.header(); raw = prefix + self.pair()
        for begin, end in ((True, len(raw)), (len(prefix) + 1, len(raw)),
                           (len(prefix), len(raw) - 1), (len(prefix), len(prefix)), (0, len(raw))):
            with self.subTest(begin=begin, end=end), self.assertRaises(ValueError):
                M.audit_cpu_copy_route_log(raw, begin, end, PID)

    def test_file_boundary_rejects_replacement_and_truncation(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / 'kodi.log'; path.write_bytes(self.header())
            started = M.route_log_start(path, PID)
            with path.open('ab') as stream:stream.write(self.pair())
            self.assertEqual(M.audit_cpu_copy_route_file(path, started)['status'], 'PASS')
            original = Path(name) / 'original.log'; path.rename(original); path.write_bytes(original.read_bytes())
            with self.assertRaisesRegex(ValueError, 'identity changed'):M.audit_cpu_copy_route_file(path, started)
            path.unlink(); original.rename(path); path.write_bytes(b'')
            with self.assertRaises(ValueError):M.audit_cpu_copy_route_file(path, started)

    def test_symlink_log_cannot_supply_owned_route_evidence(self):
        with tempfile.TemporaryDirectory() as name:
            original = Path(name) / 'original.log'; original.write_bytes(self.header() + self.pair())
            link = Path(name) / 'kodi.log'; link.symlink_to(original)
            with self.assertRaises(OSError):M.route_log_start(link, PID)


if __name__ == '__main__':unittest.main()
