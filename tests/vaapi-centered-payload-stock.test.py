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


def fixture():
    rows = []
    for ordinal in range(1, 751):
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
