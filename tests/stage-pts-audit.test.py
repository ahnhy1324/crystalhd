#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline reader negatives: a forged qualified bit must not hide failures."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PATH = Path(__file__).with_name("stage-pts-audit.py")
SPEC = importlib.util.spec_from_file_location("pts_audit", PATH)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


def fixture():
    summary = dict.fromkeys(audit.ZERO, 0)
    summary.update(version=1, scope="whole-attachment-observation", qualified=True,
                   buffers=15000, covered=15000, period_ns=20_000_000, first_pts=0,
                   last_pts=299_980_000_000, epochs=1, segment_events=1,
                   arithmetic_overflow=False, counters_saturated=False, first_violation=None)
    owner = dict(id=0, stage="sink", token=7, active=False, buffers=15000,
                 flushing=False, segment_valid=True, buffer_list_ambiguity=False,
                 buffer_lists=0, list_items=0, list_missing_pts=0, missing_pts=0,
                 flush_generation=0, eos_count=1, stream_generation=1,
                 segment_generation=1, pts_summary=summary)
    trace = dict(schema=1, closed=True, counters_saturated=False,
                 buffer_list_ambiguity=False, allocation_failed=False,
                 attachment_limit_reached=False, rejected_attachments=0,
                 configuration_rejections=0, attachments=[owner],
                 # Ring exhaustion is allowed; it cannot establish or deny the census.
                 records=[], retained=0, dropped=15005, truncated=True,
                 player=dict(attachments=[dict(stage="sink", token=7)],
                             attachment_errors=0, control_errors=0, trigger_errors=0, exit_code=0))
    expected = dict(schema=1, attachments=[dict(stage="sink", token=7,
                    buffers=15000, period_ns=20_000_000, first_pts=0)])
    return trace, expected


class AuditTests(unittest.TestCase):
    def test_complete_census_does_not_depend_on_retained_records(self):
        trace, expected = fixture()
        before = copy.deepcopy((trace, expected))
        result = audit.audit(trace, expected)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["pixels_presentation_av"], "NOT_MEASURED")
        self.assertEqual((trace, expected), before)

    def test_every_whole_run_error_rejects_even_with_qualified_true(self):
        for field in audit.ZERO + ("epochs", "segment_events", "covered", "buffers",
                                   "first_pts", "last_pts", "period_ns"):
            with self.subTest(field=field):
                trace, expected = fixture()
                trace["attachments"][0]["pts_summary"][field] += 1
                self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        for field, value in (("first_violation", {"reason": "gap"}),
                             ("arithmetic_overflow", True), ("counters_saturated", True),
                             ("qualified", False)):
            trace, expected = fixture()
            trace["attachments"][0]["pts_summary"][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_counts_and_anchors_must_match_independent_expectation(self):
        for field, value in (("buffers", 14999), ("first_pts", 20_000_000),
                             ("period_ns", 40_000_000), ("period_ns", 16_666_667),
                             ("buffers", 0), ("buffers", True), ("first_pts", audit.MAX)):
            with self.subTest(field=field, value=value):
                trace, expected = fixture()
                expected["attachments"][0][field] = value
                self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_legacy_missing_summary_and_missing_zero_are_unqualified(self):
        trace, expected = fixture()
        del trace["attachments"][0]["pts_summary"]
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_end_timestamp_overflow_rejects_a_forged_qualified_summary(self):
        trace, expected = fixture()
        goal = expected["attachments"][0]
        goal.update(buffers=1, first_pts=audit.MAX)
        owner = trace["attachments"][0]
        owner["buffers"] = 1
        owner["pts_summary"].update(buffers=1, covered=1,
                                    first_pts=audit.MAX, last_pts=audit.MAX)
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        trace, expected = fixture()
        del trace["attachments"][0]["pts_summary"]["unknown_pts"]
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_multiple_attachments_and_duplicates_fail_closed(self):
        for target in ("trace", "expected", "player"):
            trace, expected = fixture()
            rows = (expected if target == "expected" else
                    trace["player"] if target == "player" else trace)["attachments"]
            rows.append(copy.deepcopy(rows[0]))
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        trace, expected = fixture()
        extra = copy.deepcopy(trace["attachments"][0])
        extra.update(stage="decoder", id=1)
        trace["attachments"].append(extra)
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_native_only_is_explicit_and_never_ignores_present_player_errors(self):
        trace, expected = fixture()
        trace["player"]["exit_code"] = 1
        self.assertEqual(audit.audit(trace, expected, require_player=False)["status"], "FAIL")
        del trace["player"]
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        self.assertEqual(audit.audit(trace, expected, require_player=False)["status"], "PASS")

    def test_unclosed_trace_lists_and_missing_eos_are_never_full_run_pass(self):
        for field, value in (("closed", False), ("buffer_list_ambiguity", True),
                             ("allocation_failed", True), ("rejected_attachments", 1)):
            trace, expected = fixture()
            trace[field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        for field, value in (("eos_count", 0), ("active", True), ("buffers", 14999),
                             ("segment_generation", 2), ("buffer_lists", 1)):
            trace, expected = fixture()
            trace["attachments"][0][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_cli_rejects_duplicate_keys_and_nonfinite_json(self):
        with tempfile.TemporaryDirectory() as temp:
            trace_path, expected_path = Path(temp)/"trace", Path(temp)/"expected"
            trace, expected = fixture()
            expected_path.write_text(json.dumps(expected))
            for payload in ('{"schema":1,"schema":1}', '{"schema":NaN}'):
                trace_path.write_text(payload)
                run = subprocess.run([sys.executable, "-B", str(PATH), str(trace_path),
                                      str(expected_path)], capture_output=True, text=True)
                self.assertEqual(run.returncode, 1)
                self.assertEqual(json.loads(run.stdout)["status"], "FAIL")


if __name__ == "__main__":
    unittest.main()
