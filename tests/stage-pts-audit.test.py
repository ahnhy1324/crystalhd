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


def epoch_fixture():
    """Frozen source anchors and a separately specified synthetic observation."""
    trace, _ = fixture()
    caps = dict(format="UYVY", width=320, height=240, fps_n=25, fps_d=1,
                interlace_mode=0, field_order=0, raw_video=True, video_info_valid=True)
    segments = [dict(format=3, rate=1.0, applied_rate=1.0, start=0, stop=None,
                     time=0, base=0, offset=0),
                dict(format=3, rate=1.0, applied_rate=1.0, start=1_000_000_000,
                     stop=1_120_000_000, time=1_000_000_000, base=2_000_000_000, offset=0),
                dict(format=3, rate=1.0, applied_rate=1.0, start=2_000_000_000,
                     stop=2_080_000_000, time=2_000_000_000, base=3_000_000_000, offset=0)]
    goals = [dict(stream_generation=1, segment_generation=1, flush_generation=0,
                  buffers=4, first_pts=0, period_ns=40_000_000, end_reason="flush-start"),
             dict(stream_generation=1, segment_generation=2, flush_generation=1,
                  buffers=3, first_pts=1_000_000_000, period_ns=40_000_000, end_reason="eos"),
             dict(stream_generation=1, segment_generation=3, flush_generation=2,
                  buffers=2, first_pts=2_000_000_000, period_ns=40_000_000, end_reason="eos")]
    for goal, segment in zip(goals, segments):
        goal.update(segment=copy.deepcopy(segment), caps=copy.deepcopy(caps))
    rows = [dict(index=0, stream_generation=1, segment_generation=1, flush_generation=0,
                 buffers=4, covered=4, first_ordinal=1, last_ordinal=4,
                 first_pts=0, last_pts=120_000_000, first_running_time=0,
                 last_running_time=120_000_000, start_order=4, end_order=10, end_reason="flush-start"),
            dict(index=1, stream_generation=1, segment_generation=2, flush_generation=1,
                 buffers=3, covered=3, first_ordinal=5, last_ordinal=7,
                 first_pts=1_000_000_000, last_pts=1_080_000_000, first_running_time=2_000_000_000,
                 last_running_time=2_080_000_000, start_order=12, end_order=16, end_reason="eos"),
            dict(index=2, stream_generation=1, segment_generation=3, flush_generation=2,
                 buffers=2, covered=2, first_ordinal=8, last_ordinal=9,
                 first_pts=2_000_000_000, last_pts=2_040_000_000, first_running_time=3_000_000_000,
                 last_running_time=3_040_000_000, start_order=20, end_order=23, end_reason="eos")]
    for index, (row, segment) in enumerate(zip(rows, segments)):
        row.update(seqnum=100 + index, segment=copy.deepcopy(segment), caps=copy.deepcopy(caps),
                   period_ns=40_000_000, qualified=True, arithmetic_overflow=False,
                   counters_saturated=False, first_violation=None, unqualified_buffers=0,
                   unknown_pts=0, unknown_duration=0, invalid_duration=0, duplicate_events=0,
                   reverse_events=0, off_grid_events=0, gap_events=0, gap_steps=0)
    owner = trace["attachments"][0]
    owner.update(buffers=9, eos_count=2, segment_generation=3, flush_generation=2,
                 pts_epochs=dict(version=1, scope="bounded-segment-pts", limit=16,
                                 qualified=True, overflow=False, buffers=9, unassigned_buffers=0,
                                 stream_events=1, segment_events=3, flush_starts=2, flush_stops=2,
                                 seek_requests=1, seek_failures=0, pending_seek=False,
                                 first_violation=None, rows=rows))
    owner["pts_summary"]["qualified"] = False
    trace.update(observed=40, seek_requests=1, seek_failures=0)
    expected = dict(schema=2, attachments=[dict(stage="sink", token=7, epochs=goals,
                                              seek_requests=1, seek_failures=0)])
    return trace, expected


class EpochAuditTests(unittest.TestCase):
    def test_independent_three_epoch_source_counts_qualify_without_detail_or_legacy_pass(self):
        trace, expected = epoch_fixture()
        original = copy.deepcopy((trace, expected))
        result = audit.audit(trace, expected)
        self.assertEqual(result["status"], "PASS", result)
        self.assertEqual(result["schema"], 2)
        self.assertEqual(result["attachments"][0]["buffers"], 9)
        self.assertEqual(len(result["attachments"][0]["epochs"]), 3)
        self.assertEqual(result["pixels_presentation_av"], "NOT_MEASURED")
        self.assertEqual((trace, expected), original)

    def test_missing_whole_epoch_cannot_be_hidden_by_retained_suffix_or_total(self):
        for missing in range(3):
            trace, expected = epoch_fixture()
            del trace["attachments"][0]["pts_epochs"]["rows"][missing]
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_opposite_per_epoch_count_errors_do_not_cancel_in_aggregate(self):
        trace, expected = epoch_fixture()
        rows = trace["attachments"][0]["pts_epochs"]["rows"]
        rows[0].update(buffers=3, covered=3, last_ordinal=3, last_pts=80_000_000,
                       last_running_time=80_000_000)
        rows[1].update(buffers=4, covered=4, first_ordinal=4, last_pts=1_120_000_000,
                       last_running_time=2_120_000_000)
        self.assertEqual(sum(row["buffers"] for row in rows), 9)
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_reversed_or_skipped_generation_is_not_a_source_epoch(self):
        for target in ("trace", "expected"):
            for field in ("stream_generation", "segment_generation", "flush_generation"):
                with self.subTest(target=target, field=field):
                    trace, expected = epoch_fixture()
                    rows = (trace["attachments"][0]["pts_epochs"]["rows"] if target == "trace"
                            else expected["attachments"][0]["epochs"])
                    rows[1][field] = 0
                    self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")


    def test_direct_segment_and_fresh_stream_boundaries_have_distinct_identities(self):
        for fresh_stream in (False, True):
            trace, expected = epoch_fixture()
            owner, goal = trace["attachments"][0], expected["attachments"][0]
            rows, goals = owner["pts_epochs"]["rows"][:2], goal["epochs"][:2]
            reason = "stream-start" if fresh_stream else "segment"
            rows[0]["end_reason"] = goals[0]["end_reason"] = reason
            rows[1].update(flush_generation=0, stream_generation=2 if fresh_stream else 1,
                           start_order=12 if fresh_stream else 10)
            goals[1].update(flush_generation=0, stream_generation=2 if fresh_stream else 1)
            goal.update(epochs=goals, seek_requests=0)
            trace["seek_requests"] = 0
            owner.update(buffers=7, eos_count=1, flush_generation=0, segment_generation=2,
                         stream_generation=2 if fresh_stream else 1)
            owner["pts_epochs"].update(rows=rows, buffers=7, stream_events=2 if fresh_stream else 1,
                                       segment_events=2, flush_starts=0, flush_stops=0, seek_requests=0)
            self.assertEqual(audit.audit(trace, expected)["status"], "PASS")

    def test_failed_seek_cancels_without_inventing_a_boundary(self):
        trace, expected = epoch_fixture()
        owner, goal = trace["attachments"][0], expected["attachments"][0]
        row, wanted = owner["pts_epochs"]["rows"][0], goal["epochs"][0]
        row["end_reason"] = wanted["end_reason"] = "eos"
        owner.update(buffers=4, eos_count=1, segment_generation=1, flush_generation=0)
        owner["pts_epochs"].update(rows=[row], buffers=4, segment_events=1, flush_starts=0,
                                   flush_stops=0, seek_requests=1, seek_failures=1)
        goal.update(epochs=[wanted], seek_requests=1, seek_failures=1)
        trace["seek_failures"] = 1
        self.assertEqual(audit.audit(trace, expected)["status"], "PASS")

    def test_source_anchor_segment_and_caps_are_not_recovered_from_observations(self):
        for field, value in (("first_pts", 40_000_000), ("buffers", 5), ("period_ns", 20_000_000)):
            trace, expected = epoch_fixture()
            expected["attachments"][0]["epochs"][0][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        for path, field, value in (("segment", "base", 1), ("segment", "time", 1),
                                    ("caps", "format", "NV12"), ("caps", "width", 640)):
            trace, expected = epoch_fixture()
            trace["attachments"][0]["pts_epochs"]["rows"][0][path][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_every_row_defect_rejects_even_when_ledger_claims_qualified(self):
        for field in ("unqualified_buffers", "unknown_pts", "unknown_duration", "invalid_duration",
                      "duplicate_events", "reverse_events", "off_grid_events", "gap_events", "gap_steps"):
            trace, expected = epoch_fixture()
            trace["attachments"][0]["pts_epochs"]["rows"][1][field] = 1
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", field)
        for field, value in (("qualified", False), ("arithmetic_overflow", True),
                             ("counters_saturated", True), ("first_violation", {"reason": "gap"})):
            trace, expected = epoch_fixture()
            trace["attachments"][0]["pts_epochs"]["rows"][1][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", field)

    def test_pending_seek_flush_mismatch_and_hidden_extra_flush_are_rejected(self):
        for field, value in (("pending_seek", True), ("pending_seek", None), ("seek_requests", 2),
                             ("seek_failures", 1), ("flush_starts", 3), ("flush_stops", 1),
                             ("flush_stops", 3), ("unassigned_buffers", 1), ("overflow", True),
                             ("first_violation", {"reason": "seek"})):
            trace, expected = epoch_fixture()
            trace["attachments"][0]["pts_epochs"][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", field)
        for requests, failures in ((0, 1), (4, 0), (True, 0), (1, -1)):
            trace, expected = epoch_fixture()
            expected["attachments"][0].update(seek_requests=requests, seek_failures=failures)
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")


    def test_eos_requires_flush_before_restart_and_terminal_eos_cannot_be_salvaged(self):
        for target in ("trace", "expected"):
            for reason in (None, "stop", "detach", "segment", "flush-start", "stream-start"):
                trace, expected = epoch_fixture()
                rows = (trace["attachments"][0]["pts_epochs"]["rows"] if target == "trace"
                        else expected["attachments"][0]["epochs"])
                rows[-1]["end_reason"] = reason
                self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        trace, expected = epoch_fixture()
        expected["attachments"][0]["epochs"][2].update(flush_generation=1, stream_generation=2)
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        for field in ("eos_count", "stream_generation", "segment_generation", "flush_generation"):
            trace, expected = epoch_fixture()
            trace["attachments"][0][field] += 1
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", field)

    def test_combined_stream_and_flush_barriers_preserve_first_close_reason(self):
        for reason in ("flush-start", "stream-start"):
            trace, expected = epoch_fixture()
            owner, goal = trace["attachments"][0], expected["attachments"][0]
            owner["pts_epochs"]["rows"][0]["end_reason"] = reason
            goal["epochs"][0]["end_reason"] = reason
            for row in owner["pts_epochs"]["rows"][1:]:
                row["stream_generation"] = 2
            owner["pts_epochs"]["rows"][1].update(start_order=16, end_order=20)
            owner["pts_epochs"]["rows"][2].update(start_order=24, end_order=27)
            for row in goal["epochs"][1:]:
                row["stream_generation"] = 2
            owner["stream_generation"] = owner["pts_epochs"]["stream_events"] = 2
            self.assertEqual(audit.audit(trace, expected)["status"], "PASS")

    def test_ordinals_orders_and_optional_source_seqnum_have_exact_coverage(self):
        for field, value in (("first_ordinal", 4), ("last_ordinal", 8), ("index", 0),
                             ("start_order", 9), ("end_order", 12), ("end_order", None),
                             ("seqnum", 1 << 32), ("buffers", True)):
            trace, expected = epoch_fixture()
            trace["attachments"][0]["pts_epochs"]["rows"][1][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", field)
        trace, expected = epoch_fixture()
        expected["attachments"][0]["epochs"][0]["seqnum"] = 101
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        expected["attachments"][0]["epochs"][0]["seqnum"] = 100
        self.assertEqual(audit.audit(trace, expected)["status"], "PASS")
        trace["observed"] = 22
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")


    def test_running_time_allows_base_compensation_without_intermediate_wrap(self):
        trace, expected = epoch_fixture()
        goal = expected["attachments"][0]["epochs"][0]
        goal["segment"].update(base=80_000_000, offset=40_000_000)
        row = trace["attachments"][0]["pts_epochs"]["rows"][0]
        row["segment"].update(base=80_000_000, offset=40_000_000)
        row.update(first_running_time=40_000_000, last_running_time=160_000_000)
        self.assertEqual(audit.audit(trace, expected)["status"], "PASS")
        start = audit.MAX - 160_000_000
        goal.update(first_pts=start)
        goal["segment"].update(start=start, base=200_000_000, offset=160_000_000)
        row["segment"].update(start=start, base=200_000_000, offset=160_000_000)
        row.update(first_pts=start, last_pts=audit.MAX - 40_000_000)
        self.assertEqual(audit.audit(trace, expected)["status"], "PASS")

    def test_checked_duration_running_time_and_source_arithmetic_reject_overflow(self):
        for field, value in (("first_pts", audit.MAX), ("buffers", audit.MAX),
                             ("buffers", 0), ("first_pts", -1), ("period_ns", 16_666_667)):
            trace, expected = epoch_fixture()
            expected["attachments"][0]["epochs"][0][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", field)
        for field, value in (("base", audit.MAX), ("base", audit.MAX - 120_000_000),
                             ("offset", 1), ("stop", 120_000_000), ("stop", 0),
                             ("rate", True), ("applied_rate", 0.5), ("format", 2)):
            trace, expected = epoch_fixture()
            expected["attachments"][0]["epochs"][0]["segment"][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", field)

    def test_caps_changes_unknown_timing_and_native_integer_types_are_not_qualified(self):
        for field, value in (("raw_video", False), ("video_info_valid", False), ("format", "UNKNOWN"),
                             ("fps_n", 0), ("fps_d", 0), ("fps_n", 30), ("width", True),
                             ("height", 1 << 31), ("interlace_mode", 5), ("field_order", -1)):
            trace, expected = epoch_fixture()
            expected["attachments"][0]["epochs"][0]["caps"][field] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", field)
        trace, expected = epoch_fixture()
        for goal in expected["attachments"][0]["epochs"][1:]:
            goal["caps"]["format"] = "NV12"
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")


    def test_epoch_limit_empty_rows_and_missing_metadata_fail_closed(self):
        for length in (0, 17):
            trace, expected = epoch_fixture()
            source_row = expected["attachments"][0]["epochs"][0]
            expected["attachments"][0]["epochs"] = [copy.deepcopy(source_row) for _ in range(length)]
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        for name in ("pts_epochs", "rows", "caps", "segment", "unknown_pts"):
            trace, expected = epoch_fixture()
            owner = trace["attachments"][0]
            if name == "pts_epochs":
                del owner[name]
            elif name == "rows":
                del owner["pts_epochs"][name]
            else:
                del owner["pts_epochs"]["rows"][0][name]
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", name)

    def test_terminal_trace_and_attachment_failures_cannot_be_laundered(self):
        for name, value in (("closed", False), ("counters_saturated", True),
                            ("buffer_list_ambiguity", True), ("allocation_failed", True),
                            ("attachment_limit_reached", True), ("rejected_attachments", 1),
                            ("configuration_rejections", 1)):
            trace, expected = epoch_fixture()
            trace[name] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", name)
        for name, value in (("active", True), ("flushing", True), ("segment_valid", False),
                            ("buffer_lists", 1), ("missing_pts", 1), ("buffers", 8)):
            trace, expected = epoch_fixture()
            trace["attachments"][0][name] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", name)

    def test_multi_stage_broadcast_request_is_not_sum_of_owner_requests(self):
        trace, expected = epoch_fixture()
        second = copy.deepcopy(trace["attachments"][0])
        second.update(id=1, stage="decoder", token=8)
        for row in second["pts_epochs"]["rows"]:
            row["start_order"] += 40
            row["end_order"] += 40
        trace["attachments"].append(second)
        wanted = copy.deepcopy(expected["attachments"][0])
        wanted.update(stage="decoder", token=8)
        expected["attachments"].append(wanted)
        trace["player"]["attachments"].append(dict(stage="decoder", token=8))
        trace.update(observed=80, seek_requests=1)
        self.assertEqual(audit.audit(trace, expected)["status"], "PASS")
        trace["seek_requests"] = 2
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        trace["seek_requests"] = 1
        trace["attachments"].reverse()
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        trace["attachments"].reverse()
        for row in second["pts_epochs"]["rows"]:
            row["start_order"] -= 39
            row["end_order"] -= 39
        result = audit.audit(trace, expected)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("event capacity", result["errors"][0])
        for row in second["pts_epochs"]["rows"]:
            row["start_order"] -= 1
            row["end_order"] -= 1
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        trace["attachments"].pop()
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_native_only_remains_explicit_and_present_player_errors_still_fail(self):
        trace, expected = epoch_fixture()
        del trace["player"]
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        self.assertEqual(audit.audit(trace, expected, require_player=False)["status"], "PASS")
        trace["player"] = dict(attachments=[dict(stage="sink", token=7)], attachment_errors=0,
                               control_errors=1, trigger_errors=0, exit_code=0)
        self.assertEqual(audit.audit(trace, expected, require_player=False)["status"], "FAIL")

    def test_epoch_cli_uses_schema2_expectations_and_rejects_json_duplicates(self):
        trace, expected = epoch_fixture()
        with tempfile.TemporaryDirectory() as temp:
            trace_path, expected_path = Path(temp)/"trace", Path(temp)/"expected"
            expected_path.write_text(json.dumps(expected))
            trace_path.write_text(json.dumps(trace))
            run = subprocess.run([sys.executable, "-B", str(PATH), str(trace_path), str(expected_path)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertEqual(json.loads(run.stdout)["schema"], 2)
            expected_path.write_text('{"schema":2,"schema":2}')
            run = subprocess.run([sys.executable, "-B", str(PATH), str(trace_path), str(expected_path)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertIn("duplicate JSON key", json.loads(run.stdout)["errors"][0])


    def test_unexpected_or_untyped_global_request_is_not_ignored(self):
        for value in (0, 2, True, "1", None):
            trace, expected = epoch_fixture()
            trace["seek_requests"] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", repr(value))
        trace, expected = epoch_fixture()
        del trace["seek_requests"]
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")


    def test_unexpected_or_missing_helper_seek_failure_is_not_ignored(self):
        for value in (1, True, "0", None):
            trace, expected = epoch_fixture()
            trace["seek_failures"] = value
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL", repr(value))
        trace, expected = epoch_fixture()
        del trace["seek_failures"]
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")


    def test_schema2_native_slot_ids_are_ordered_and_start_at_zero(self):
        for identity in (1, 63):
            trace, expected = epoch_fixture()
            trace["attachments"][0]["id"] = identity
            result = audit.audit(trace, expected)
            self.assertEqual(result["status"], "FAIL")
            self.assertIn("ordered attachment IDs", result["errors"][0])

    def test_initial_context_and_terminal_stop_need_distinct_event_slots(self):
        for start in (1, 2, 3):
            trace, expected = epoch_fixture()
            trace["attachments"][0]["pts_epochs"]["rows"][0]["start_order"] = start
            self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")
        trace, expected = epoch_fixture()
        trace["attachments"][0]["pts_epochs"]["rows"][-1]["end_order"] = trace["observed"]
        self.assertEqual(audit.audit(trace, expected)["status"], "FAIL")

    def test_another_boundary_cannot_be_reused_as_an_ordinary_buffer(self):
        # The (12,16) row has three apparent slots. Order13 is another
        # SEGMENT, leaving only14/15 for ordinary buffers.
        with self.assertRaisesRegex(ValueError, "event capacity"):
            audit.event_capacity([(13, 15, 3)], [13, 80])
        audit.event_capacity([(13, 15, 2)], [13, 80])

    def test_combined_barrier_metadata_cannot_fit_in_one_missing_event_slot(self):
        trace, expected = epoch_fixture()
        owner = trace["attachments"][0]
        # Flush-start at10, next SEGMENT at12: only order11 is available,
        # but FLUSH_STOP + STREAM_START + CAPS require three distinct events.
        for row in owner["pts_epochs"]["rows"][1:]:
            row["stream_generation"] = 2
        for goal in expected["attachments"][0]["epochs"][1:]:
            goal["stream_generation"] = 2
        owner["stream_generation"] = owner["pts_epochs"]["stream_events"] = 2
        result = audit.audit(trace, expected)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("event capacity", result["errors"][0])

    def test_global_capacity_handles_releases_and_overlapping_interval_demands(self):
        # The late short-deadline job must preempt the bulk long-deadline job.
        audit.event_capacity([(1, 10, 5), (3, 4, 2)], [6])
        # Both intervals individually have enough slots; together they need
        # five ordinary events in only four unreserved positions.
        with self.assertRaisesRegex(ValueError, "event capacity"):
            audit.event_capacity([(1, 4, 3), (3, 5, 2)], [3])

    def test_event_capacity_operates_on_bulk_uint64_counts(self):
        span = 1 << 60
        audit.event_capacity([(1, span, span - 2)], [2, span])
        with self.assertRaisesRegex(ValueError, "event capacity"):
            audit.event_capacity([(1, span, span - 1)], [2, span])


if __name__ == "__main__":
    unittest.main()
