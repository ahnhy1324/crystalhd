#!/usr/bin/python3
"""Pure observer-census controls; optional explicit retained trace positivity.

No media/device access occurs. Use --trace FILE [--pid PID --begin N --end N]
to check a retained finite-play trace; no private artifact path is embedded.
"""
import argparse
import ast
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
DIRECTORY = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


STOCK = load('centered_stock_census_test_subject', DIRECTORY / 'vaapi-centered-payload-stock.py')
REAL_ARGS = None
PID = 321
FIELDS = STOCK.BINDING + ('surface', 'owner')


def fixture(count=750):
    rows = []
    for ordinal in range(1, count + 1):
        identity = dict(driver_instance=7, context=3, generation=1, token=ordinal * 100000,
                        decode_identity=ordinal * 2, submission_ordinal=ordinal,
                        surface=ordinal, owner=ordinal)
        for event in ('submit_bind', 'input_sent', 'output_dequeued', 'output_dequeued', 'materialized'):
            seq = len(rows) + 1
            rows.append((seq - 1, dict(identity, pid=PID, event=event, seq=seq, outcome=0)))
    return rows


class CensusTests(unittest.TestCase):
    def setUp(self):
        self.rows = fixture()
        self.interval = {'begin': 0, 'end': len(self.rows)}

    def audit(self, rows=None):
        return STOCK.operation_census(self.rows if rows is None else rows, self.interval, PID)

    def reject(self, rows=None):
        try:
            result = self.audit(rows)
        except (ValueError, KeyError, TypeError):
            return
        self.assertEqual(result['status'], 'FAIL', result)
        self.assertTrue(result['errors'])

    def test_pinned_mbaff_two_dequeues_are_positive(self):
        result = self.audit()
        self.assertEqual(result['status'], 'PASS', result)
        self.assertEqual(result['event_counts']['output_dequeued'], 1500)
        self.assertEqual(result['binding_multiplicity_histograms']['output_dequeued'], {2: 750})
        self.assertEqual(result['distinct_binding_stage_counts']['materialized'], 750)
        self.assertEqual(result['actual_media_PTS_to_binding'], 'NOT_OBSERVED')

    def test_reused_source_identity_cannot_inflate_original_count(self):
        for field in ('token', 'decode_identity', 'submission_ordinal'):
            with self.subTest(field=field):
                self.rows = fixture()
                first = [row for _, row in self.rows if row['submission_ordinal'] == 1]
                second = next(row for _, row in self.rows if row['submission_ordinal'] == 2)
                for row in first:
                    row[field] = second[field]
                self.reject()

    def test_missing_field_record_rejected(self):
        del self.rows[3]
        self.reject()

    def test_third_field_record_rejected(self):
        extra = copy.deepcopy(self.rows[3][1]); extra['seq'] = 20000
        self.rows.append((3749, extra))
        self.reject()

    def test_balanced_total_with_one_and_three_fields_rejected(self):
        # All750 identities and total1500 remain; only per-original multiplicity fails.
        first, second = self.rows[3][1], self.rows[8][1]
        for field in FIELDS:first[field] = second[field]
        self.assertEqual(len([r for _, r in self.rows if r['event'] == 'output_dequeued']), 1500)
        self.reject()

    def test_balanced_identity_swap_of_both_fields_rejected_by_order(self):
        # Both originals still have two outputs, but one gets outputs before input.
        for left, right in ((2, 7), (3, 8)):
            for field in FIELDS:
                self.rows[left][1][field], self.rows[right][1][field] = self.rows[right][1][field], self.rows[left][1][field]
        self.reject()

    def test_each_binding_component_swap_rejected(self):
        for field in FIELDS:
            with self.subTest(field=field):
                rows = copy.deepcopy(self.rows)
                rows[3][1][field] += 9000
                self.reject(rows)

    def test_missing_and_repeated_submit_input_rejected(self):
        for index in (0, 1):
            with self.subTest(index=index, change='missing'):
                rows = copy.deepcopy(self.rows); del rows[index]; self.reject(rows)
            with self.subTest(index=index, change='repeated'):
                rows = copy.deepcopy(self.rows); extra = copy.deepcopy(rows[index][1]); extra['seq'] = 20000
                rows.append((3749, extra)); self.reject(rows)

    def test_input_before_submit_or_after_field_rejected(self):
        for left, right in ((0, 1), (1, 2)):
            with self.subTest(left=left, right=right):
                rows = copy.deepcopy(self.rows)
                rows[left][1]['seq'], rows[right][1]['seq'] = rows[right][1]['seq'], rows[left][1]['seq']
                self.reject(rows)

    def test_materialized_before_second_field_rejected(self):
        self.rows[3][1]['seq'], self.rows[4][1]['seq'] = self.rows[4][1]['seq'], self.rows[3][1]['seq']
        self.reject()

    def test_failed_outcome_at_each_stage_rejected(self):
        for index in range(5):
            for outcome in (-1, 1, True):
                with self.subTest(index=index, outcome=outcome):
                    rows = copy.deepcopy(self.rows); rows[index][1]['outcome'] = outcome; self.reject(rows)

    def test_replayed_field_sequence_rejected(self):
        self.rows[3][1]['seq'] = self.rows[2][1]['seq']
        self.reject()

    def test_foreign_pid_rejected(self):
        self.rows[3][1]['pid'] += 1
        self.reject()

    def test_materialized_subset_is_recorded_without_renderer_zero_loss_gate(self):
        rows = [pair for pair in self.rows if pair[1]['event'] != 'materialized' or pair[1]['submission_ordinal'] <= 2]
        result = self.audit(rows)
        self.assertEqual(result['status'], 'PASS', result)
        self.assertEqual(result['distinct_binding_stage_counts']['materialized'], 2)
        self.assertFalse(result['all_stage_binding_sets_equal'])

    def test_unrelated_and_repeated_materialization_rejected(self):
        rows = copy.deepcopy(self.rows); rows[4][1]['token'] += 1; self.reject(rows)
        extra = copy.deepcopy(self.rows[4][1]); extra['seq'] = 20000
        self.rows.append((3749, extra)); self.reject()

    def test_interval_boundaries_do_not_absorb_other_play_records(self):
        outside = copy.deepcopy(self.rows[2][1]); outside['pid'] = PID + 1
        result = self.audit([(-1, outside)] + self.rows + [(3750, outside)])
        self.assertEqual(result['status'], 'PASS', result)



class BwdifScenarioTests(unittest.TestCase):
    def test_balanced_measured_subset_never_passes_complete_source_population(self):
        rows = fixture(695)
        result = STOCK.operation_census(rows, {'begin': 0, 'end': len(rows)}, PID, 900)
        self.assertEqual(result['status'], 'FAIL')
        self.assertEqual(result['ledger_status'], 'PASS')
        self.assertFalse(result['source_population_qualified'])
        self.assertEqual(result['observed_submit_bindings'], 695)
        self.assertEqual(result['source_metadata_pictures'], 900)
        del rows[3]
        self.assertEqual(STOCK.operation_census(rows, {'begin': 0, 'end': 4000}, PID, 900)['ledger_status'], 'FAIL')

    def test_nine_hundred_originals_keep_two_distinct_native_dequeues(self):
        rows = fixture(900)
        result = STOCK.operation_census(rows, {'begin': 0, 'end': len(rows)}, PID, 900)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['event_counts']['output_dequeued'], 1800)
        self.assertEqual(result['observed_submit_bindings'], 900)
        self.assertEqual(result['actual_media_PTS_to_binding'], 'NOT_OBSERVED')
        self.assertEqual(STOCK.operation_census(rows, {'begin': 0, 'end': len(rows)}, PID)['status'], 'FAIL')

    def test_balanced_native_total_cannot_hide_one_and_three_fields(self):
        rows = fixture(900)
        for field in FIELDS:rows[3][1][field] = rows[8][1][field]
        self.assertEqual(len([r for _, r in rows if r['event'] == 'output_dequeued']), 1800)
        self.assertEqual(STOCK.operation_census(rows, {'begin': 0, 'end': len(rows)}, PID, 900)['status'], 'FAIL')

    def metadata(self, media):
        frames = [dict(pts=6006 + i*3003, interlaced_frame=1, top_field_first=1, repeat_pict=0,
                       width=1920, height=1080, pix_fmt='yuv420p') for i in range(900)]
        return dict(streams=[dict(codec_type='video', codec_name='h264', width=1920, height=1080,
                    coded_width=1920, coded_height=1088, pix_fmt='yuv420p', r_frame_rate='30000/1001',
                    time_base='1/90000', field_order='tt')], frames=frames,
                    packets=[dict(pts=f['pts']) for f in reversed(frames)],
                    format=dict(filename=str(media), size=media.stat().st_size))

    def test_short_metadata_accepts_packet_reordering_without_inventing_clock(self):
        with tempfile.TemporaryDirectory() as name:
            media=Path(name)/'short.ts';media.write_bytes(b'owned metadata control')
            result=STOCK.source_metadata(self.metadata(media),media,'bwdif2997')
            self.assertEqual(result['pictures'],900)
            self.assertEqual(result['coded'],[1920,1088])
            self.assertEqual(result['source_time_base'],'1/90000')
            self.assertEqual(result['nominal_picture_duration'],'1001/30000')
            self.assertEqual(result['actual_source_PTS_to_VA_binding'],'NOT_OBSERVED')

    def test_short_metadata_rejects_long_or_packet_only_substitution(self):
        with tempfile.TemporaryDirectory() as name:
            media=Path(name)/'short.ts';media.write_bytes(b'owned metadata control')
            for mode in ('long','packet-only','shifted-clock','wrong-coded','wrong-flags'):
                with self.subTest(mode=mode):
                    doc=self.metadata(media)
                    if mode=='long':doc['frames']*=10;doc['packets']*=10
                    elif mode=='packet-only':doc['frames']=[]
                    elif mode=='shifted-clock':
                        for row in doc['frames']+doc['packets']:row['pts']+=1
                    elif mode=='wrong-coded':doc['streams'][0]['coded_height']=1080
                    else:doc['frames'][-1]['repeat_pict']=1
                    with self.assertRaises(ValueError):STOCK.source_metadata(doc,media,'bwdif2997')

    def test_reference_namespace_has_no_media_device_or_preload(self):
        prefix=STOCK.reference_namespace(Path('/app-source'),Path('/platform'),Path('/gl-source'),Path('/payload-source'),Path('/own-reference'))
        self.assertNotIn('--dev-bind',prefix)
        self.assertNotIn('--die-with-parent',prefix)
        self.assertNotIn('LD_PRELOAD',prefix)
        self.assertNotIn('/dev/crystalhd',prefix)
        self.assertEqual(prefix[-1],'--')

    def test_bwdif_preset_is_explicit_and_old_default_remains_off(self):
        self.assertEqual(STOCK.scenario('off750')['method'],0)
        self.assertTrue(STOCK.scenario('off750')['prefer_vaapi_renderer'])
        self.assertEqual(STOCK.scenario('bwdif2997')['method'],7)
        self.assertEqual(STOCK.scenario('bwdif2997')['deintmethod'],'bwdif')
        self.assertFalse(STOCK.scenario('bwdif2997')['prefer_vaapi_renderer'])
        with self.assertRaises(ValueError):STOCK.scenario('automatic-unqualified')


class PacketAdmissionModeTests(unittest.TestCase):
    def test_explicit_packet_mode_cannot_leak_into_off_or_truthy_nonboolean_plan(self):
        self.assertFalse(STOCK.packet_mode(False, 'off750'))
        self.assertTrue(STOCK.packet_mode(True, 'bwdif2997'))
        for value, name in ((True, 'off750'), (1, 'bwdif2997'), ('true', 'bwdif2997'), (None, 'bwdif2997')):
            with self.subTest(value=value, scenario=name):
                with self.assertRaises(ValueError):STOCK.packet_mode(value, name)

    def test_packet_preload_composes_with_filter_without_client_control_changes(self):
        ordinary = STOCK.observer_options(Path('/own'), 'off750')
        self.assertEqual(ordinary, [])
        old = STOCK.observer_options(Path('/own'), 'bwdif2997')
        combined = STOCK.observer_options(Path('/own'), 'bwdif2997', True)
        for command, expected in ((old, STOCK.POSTFILTER_MOUNT),
                                  (combined, STOCK.POSTFILTER_MOUNT + ':' + STOCK.PACKET_MOUNT)):
            self.assertEqual(command.count('LD_PRELOAD'), 1)
            self.assertEqual(command[command.index('LD_PRELOAD') + 1], expected)
            self.assertNotIn('--dev-bind', command)
            self.assertNotIn('skip_frame', command)
        self.assertIn(STOCK.PACKET_MOUNT, combined)
        self.assertNotIn(STOCK.PACKET_MOUNT, old)

    def test_packet_pins_require_original_format_codec_util_and_reader(self):
        paths = STOCK.packet_files(Path('/own'), Path('/original-app'))
        for name in ('format.so.63.1.102', 'codec.so.63.1.102', 'util.so.61.1.102'):
            self.assertIn(Path('/original-app/lib/libav' + name), paths)
        self.assertIn(STOCK.PACKET_READER, paths)
        self.assertIn(Path('/own') / STOCK.PACKET_IMAGE, paths)

    def audit_with(self, status):
        seen = {}
        def read(path, **kwargs):
            seen.update(path=path, **kwargs)
            return {'validated': True}
        def census(validated, intervals, **kwargs):
            seen.update(census=kwargs, intervals=intervals)
            self.assertTrue(validated['validated'])
            return {'status': status, 'source_population': {'status': 'FAIL', 'observed': 695}}
        reader = SimpleNamespace(read_report=read,
            audit_report=lambda *a, **k: {'status': 'PASS_SCOPED_PACKET_ADMISSION_OBSERVATION'},
            census_case=census)
        report = {'owned_app': {'pid': 123}, 'intervals': {'1': {'begin_ns': 10, 'end_ns': 20}},
                  'source_census': {'1': {'observed_submit_bindings': 695}}}
        with patch.object(STOCK, 'load', return_value=reader):
            result = STOCK.audit_packet_case(Path('/retired-case'), report)
        return result, seen

    def test_packet_observation_keeps_partial_population_and_unobserved_identity_explicit(self):
        result, seen = self.audit_with('PASS_SCOPED_PACKET_ADMISSION_CENSUS')
        self.assertEqual(result['census']['source_population']['status'], 'FAIL')
        self.assertEqual(result['native_submitted_by_play'], {'1': 695})
        self.assertEqual(result['codec_to_VA_binding'], 'NOT_OBSERVED')
        self.assertFalse(result['source_clock_comparison'])
        self.assertEqual(seen['expected_pid'], 123)
        self.assertEqual(seen['expected_providers']['format'], '/app/lib/libavformat.so.63.1.102')
        self.assertEqual(seen['census'], {'expected_video_packets': 900, 'expected_url': '/fixture.mp4'})

    def test_complete_report_cannot_hide_ambiguous_or_empty_source_selection(self):
        for status in ('NOT_QUALIFIED', 'FAIL', 'PASS'):
            with self.subTest(status=status):
                with self.assertRaisesRegex(ValueError, 'source/play selection'):
                    self.audit_with(status)


class MaintainedCostModeTests(unittest.TestCase):
    def correlation_fixture(self):
        def event(identifier, parent, token, stage, kind='full'):
            return dict(id=identifier, parent_id=parent, stage=stage, binding_kind=kind,
                        wall_begin_ns=100+identifier, wall_end_ns=190-identifier,
                        driver_instance=1, context=2, generation=1, token=token,
                        decode_identity=token*2 if token else 0,
                        submission_ordinal=token, surface=token, owner=token)
        # Requested A contains separately resolved B and B's partial pixel work.
        events = [event(1,0,1,'sync_body'), event(2,1,2,'process_output'),
                  event(3,2,0,'field_convert_pixels','surface'),
                  event(4,2,0,'canvas_pixels','surface')]
        native = dict(pid=123, event='materialized', driver_instance=1, context=2,
                      generation=1, token=2, decode_identity=4, submission_ordinal=2)
        valid = dict(events=events, by_id={e['id']: e for e in events})
        return valid, [(10,native)], {'1': dict(begin=0,end=20,begin_ns=100,end_ns=200)}

    def test_partial_pixel_work_uses_actual_harvest_parent_not_requested_picture(self):
        valid, rows, windows = self.correlation_fixture()
        result = STOCK.correlate_cost_native(valid, rows, windows, 123)
        self.assertEqual(result['status'], 'PASS_SCOPED_NATIVE_COST_CORRELATION')
        play = result['plays']['1']
        self.assertEqual(play['required_pixel_stage_coverage'], {'field_convert_pixels':1,'canvas_pixels':1})
        self.assertEqual(play['direct_full_events'],1)
        self.assertEqual(play['actual_parent_lineage_events'],2)
        self.assertEqual(play['unjoined_events'],1)

    def test_explicit_different_full_child_never_falls_back_to_requested_ancestor(self):
        valid, rows, windows = self.correlation_fixture()
        for e in valid['events'][1:]:
            if e['binding_kind']=='full':
                e.update(token=3,decode_identity=6,submission_ordinal=3,surface=3,owner=3)
        rows[0][1].update(token=1,decode_identity=2,submission_ordinal=1)
        # A is a valid native key, but cannot replace separately known picture3.
        result = STOCK.correlate_cost_native(valid,rows,windows,123)
        self.assertEqual(result['status'],'NOT_QUALIFIED_NATIVE_COST_CORRELATION')
        self.assertEqual(result['plays']['1']['matched_events'],1)

    def test_conflicting_partial_source_scalar_remains_unjoined(self):
        valid,rows,windows = self.correlation_fixture()
        valid['events'][2]['token']=99
        result=STOCK.correlate_cost_native(valid,rows,windows,123)
        self.assertEqual(result['status'],'NOT_QUALIFIED_NATIVE_COST_CORRELATION')
        self.assertEqual(result['plays']['1']['required_pixel_stage_coverage']['field_convert_pixels'],0)

    def test_unknown_child_cannot_cross_conflicting_partial_ancestor(self):
        valid,rows,windows = self.correlation_fixture()
        parent = valid['events'][1]
        parent['binding_kind'] = 'surface'
        rows[0][1].update(token=1,decode_identity=2,submission_ordinal=1)
        result = STOCK.correlate_cost_native(valid,rows,windows,123)
        self.assertEqual(result['status'],'NOT_QUALIFIED_NATIVE_COST_CORRELATION')
        self.assertEqual(result['plays']['1']['matched_events'],1)
        self.assertEqual(result['plays']['1']['required_pixel_stage_coverage']['canvas_pixels'],0)

    def test_cost_mode_requires_explicit_packet_bwdif_and_boolean(self):
        self.assertFalse(STOCK.cost_mode(False, 'off750', False))
        self.assertTrue(STOCK.cost_mode(True, 'bwdif2997', True))
        for value, scenario, packets in ((True, 'off750', True), (True, 'bwdif2997', False),
                                         (True, 'bwdif2997', 1), (1, 'bwdif2997', True)):
            with self.subTest(value=value, scenario=scenario, packets=packets):
                with self.assertRaises(ValueError):STOCK.cost_mode(value, scenario, packets)

    def test_backend_selection_and_pins_are_explicit(self):
        self.assertEqual(STOCK.backend_image(), Path('filters/vaapi/centered-observer/crystalhd_drv_video.so'))
        self.assertEqual(STOCK.backend_image(True), STOCK.COST_IMAGE)
        files = STOCK.cost_files(Path('/own'))
        self.assertIn(Path('/own') / STOCK.COST_IMAGE, files)
        self.assertIn(Path('/own/tests/vaapi-maintained-cost-observer.h'), files)
        self.assertIn(Path('/own/tests/vaapi-maintained-cost-backend.cpp'), files)
        self.assertIn(STOCK.COST_READER, files)
        with self.assertRaises(ValueError):STOCK.backend_image(1)

    def test_cost_environment_adds_no_client_or_preload_override(self):
        self.assertEqual(STOCK.cost_options(), [])
        args = STOCK.cost_options(True)
        self.assertNotIn('LD_PRELOAD', args)
        self.assertNotIn('skip_frame', args)
        self.assertNotIn('--dev-bind', args)
        self.assertEqual(args[-1], '/payload/maintained-cost')

    def test_cost_observation_cannot_turn_native_subset_into_full_source_pass(self):
        report = {'owned_app': {'pid': 123}, 'intervals': {'1': {'begin_ns': 10, 'end_ns': 20}},
                  'source_census': {'1': {'observed_submit_bindings': 677, 'status': 'FAIL'}}}
        reader = SimpleNamespace(read_report=lambda path, **kw: {'pid': kw['expected_pid']},
            audit_report=lambda *a, **kw: {'status': 'PASS_SCOPED_MAINTAINED_COST_OBSERVATION'},
            census_case=lambda *a: {'status': 'PASS_SCOPED_MAINTAINED_COST_CENSUS'})
        with patch.object(STOCK, 'load', return_value=reader):
            result = STOCK.audit_cost_case(Path('/retired-case'), report)
        self.assertEqual(result['native_originals_by_play'], {'1': 677})
        self.assertEqual(report['source_census']['1']['status'], 'FAIL')
        reader.census_case = lambda *a: {'status': 'NOT_QUALIFIED'}
        with patch.object(STOCK, 'load', return_value=reader):
            with self.assertRaisesRegex(ValueError, 'play selection'):
                STOCK.audit_cost_case(Path('/retired-case'), report)


class AbortTests(unittest.TestCase):
    def test_controller_abort_interrupts_waiting_application_gate(self):
        # Run only OWN pump/wait definitions with fake published Kodi objects.
        tree = ast.parse(STOCK.SCRIPT.replace('__CYCLES__', '1'))
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in ('pump', 'wait')]
        module = ast.fix_missing_locations(ast.Module(body=functions, type_ignores=[]))
        with tempfile.TemporaryDirectory(prefix='centered-abort-test-') as name:
            profile = Path(name); sleeps = []
            def sleep(milliseconds):
                sleeps.append(milliseconds)
                (profile / 'controller-abort.json').write_text('{"status":"CONTROLLER_ABORT"}\n')
            namespace = {'R': profile, 'state': {'events': []},
                         'monitor': SimpleNamespace(abortRequested=lambda: False),
                         'xbmc': SimpleNamespace(sleep=sleep), 'time': SimpleNamespace(monotonic=lambda: 1.0)}
            exec(compile(module, '<OWN application gate definitions>', 'exec'), namespace)
            with self.assertRaisesRegex(RuntimeError, 'owned controller aborted'):
                namespace['wait'](lambda: False, 25, 'retired gate')
            self.assertEqual(sleeps, [20], 'abort must release the gate on its next pump')

    def test_normal_pump_keeps_existing_playback_error_guard(self):
        tree = ast.parse(STOCK.SCRIPT.replace('__CYCLES__', '1'))
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'pump')
        module = ast.fix_missing_locations(ast.Module(body=[function], type_ignores=[]))
        with tempfile.TemporaryDirectory(prefix='centered-error-test-') as name:
            namespace = {'R': Path(name), 'state': {'events': [{'event': 'OnError'}]},
                         'monitor': SimpleNamespace(abortRequested=lambda: False),
                         'xbmc': SimpleNamespace(sleep=lambda _: self.fail('error must precede sleeping'))}
            exec(compile(module, '<OWN pump definition>', 'exec'), namespace)
            with self.assertRaisesRegex(RuntimeError, 'playback error'):namespace['pump']()


class ExplicitRetainedTraceTests(unittest.TestCase):
    def test_explicit_retained_file_census_positive(self):
        if REAL_ARGS is None or REAL_ARGS.trace is None:
            self.skipTest('supply --trace for an explicit retained-file positive check')
        reader = load('centered_stock_census_trace_reader', DIRECTORY / 'vaapi-centered-payload-reader.py')
        raw = reader.read_regular(REAL_ARGS.trace, reader.MAX_TRACE_BYTES)
        rows = STOCK.parse_rows(raw, reader); pids = {row['pid'] for _, row in rows}
        self.assertTrue(pids, 'retained file has no trace rows')
        pid = REAL_ARGS.pid
        if pid is None:
            self.assertEqual(len(pids), 1, 'supply --pid for a trace with multiple owned PIDs'); pid = next(iter(pids))
        interval = {'begin': REAL_ARGS.begin, 'end': len(raw) if REAL_ARGS.end is None else REAL_ARGS.end}
        self.assertTrue(0 <= interval['begin'] < interval['end'] <= len(raw))
        result = STOCK.operation_census(rows, interval, pid)
        self.assertEqual(result['status'], 'PASS', result)
        self.assertEqual(result['distinct_binding_stage_counts']['submit_bind'], 750)
        self.assertEqual(result['event_counts']['output_dequeued'], 1500)
        print('explicit-retained-census: ' + json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument('--trace', type=Path); parser.add_argument('--pid', type=int)
    parser.add_argument('--begin', type=int, default=0); parser.add_argument('--end', type=int)
    REAL_ARGS, remaining = parser.parse_known_args()
    if REAL_ARGS.trace is None and (REAL_ARGS.pid is not None or REAL_ARGS.begin != 0 or REAL_ARGS.end is not None):
        parser.error('--pid/--begin/--end require --trace')
    program = unittest.main(argv=[sys.argv[0]] + remaining, exit=False)
    raise SystemExit(0 if program.result.wasSuccessful() else 1)
