#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline semantic controls: actual native receipts and replay debt, no media."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


_spec = importlib.util.spec_from_file_location(
    "replay_reader", Path(__file__).with_name("vaapi-replay-disposition-reader.py"))
R = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(R)
PID = 123


class Fixture:
    """Declared two-picture software history; counts never stand for900 frames."""
    def __init__(self, replay=True, retired=False, fragments=False, prefeed=False, retire_original=False):
        self.events, self.native, self.clock, self.rid = [], [], 100, 0
        self.ordinal = 0
        self.admit(1)
        if retire_original:
            self.emit_native("binding_end", 1)
        self.input(1, fragments=fragments, prefeed=prefeed, busy=True)
        self.pair(1, materialized=not retire_original)
        self.admit(2)
        if replay:
            if retired:
                self.emit_native("binding_end", 1)
            self.restart()
            self.input(1, replay=True, zero=retired)
            self.pair(1, replay=True, zero=retired)
        self.input(2)
        self.pair(2)
        self.emit_native("context_destroy", 0)

    def binding(self, token, zero=False):
        return dict(driver_instance=1, context=2, generation=1, token=token,
                    decode_identity=0 if zero or not token else 40+token,
                    submission_ordinal=0 if zero or not token else token,
                    surface=0 if zero or not token else 10+token,
                    owner=0 if zero or not token else 20+token)

    def emit_native(self, stage, token, zero=False):
        self.clock += 10
        row = dict(schema=R.TRACE_SCHEMA, pid=PID, session=1, seq=len(self.native)+1,
                   ts_monotonic_ns=self.clock, event=stage, outcome=0, operation=0,
                   **self.binding(token, zero))
        self.native.append((len(self.native)*100, row))
        return row

    def emit(self, kind, token=0, **values):
        self.clock += 10
        event = {k: 0 for k in R.UNSIGNED}
        event.update(type="event", seq=len(self.events), id=len(self.events)+1,
            pid=PID, tid=PID, monotonic_ns=self.clock, event=kind,
            driver_instance=1, context=2, generation=1, token=token,
            restart_ordinal=self.ordinal, result=None, completed_before=None,
            fragmented=False, consume_prefeed=False, restart_phase=None,
            inspect=None, observe=None, field_role="none", field_valid=None,
            pair_valid=None, replay_duplicate=None)
        event.update(values)
        self.events.append(event)
        return event

    def admit(self, token):
        return self.emit_native("submit_bind", token)

    def restart(self):
        for phase in ("close", "open", "transition", "committed"):
            if phase == "committed":
                self.ordinal += 1
            self.emit("restart", restart_phase=phase, result=1 if phase in ("transition", "committed") else 0,
                      cached_units=2)

    def input(self, token, replay=False, zero=False, fragments=False, prefeed=False, busy=False):
        props = dict(completed_before=replay, fragmented=fragments, consume_prefeed=prefeed, unit_bytes=10)
        if busy:
            self.emit("input_attempt", token, result=2, input_offset=0,
                      input_bytes=4 if fragments else 10, **props)
        if fragments:
            self.emit("input_attempt", token, result=0, input_offset=0, input_bytes=4, **props)
        last = dict(input_offset=4 if fragments else 0, input_bytes=6 if fragments else 10, **props)
        self.emit("input_attempt", token, result=0, **last)
        native = self.emit_native("input_sent", token, zero)
        self.emit("input_commit", token, native_seq=native["seq"], result=1, **last)

    def pair(self, token, replay=False, zero=False, materialized=True):
        first_id = 0
        for second in (False, True):
            self.rid += 1
            native = self.emit_native("output_dequeued", token, zero)
            live = dict(zip(R.LIVE, (native[k] for k in R.BINDING[4:])))
            raw = dict(receive_id=self.rid, native_seq=native["seq"], picture_number=100+token,
                       sess_num=7, picture_meta_payload=80+token, width=16, height=32,
                       field_flags=32+(24 if second else 16), **live)
            self.emit("output", token, field_role="whole", **raw)
            if second and not replay and materialized:
                self.emit_native("materialized", token)
            decision = dict(inspect="duplicate" if replay else "new", observe=("duplicate" if replay else "new") if second else None,
                            field_role="complement" if second else "first", field_valid=True,
                            pair_valid=True if second else None, replay_duplicate=replay,
                            pair_first_receive_id=first_id)
            self.emit("disposition", token, result=0, **raw, **decision)
            self.emit("release", token, result=0, **raw, **decision)
            first_id = self.rid

    def document(self):
        return [dict(type="header", schema=R.SCHEMA, pid=PID, fork_policy=R.FORK_POLICY,
                    scope=R.SCOPE, capacity=32768, bc_success=0, va_success=0,
                    field_constants=dict(R.FIELD_CONSTANTS)), *copy.deepcopy(self.events),
                dict(type="footer", events=len(self.events), attempted_events=len(self.events),
                     dropped=0, inflight=0, unpublished=0, clock_failures=0,
                     lifecycle_failures=0, fork_seen=False, complete=True)]


class ReplayDispositionReaderTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.rows = self.fixture.document()
        self.native = self.fixture.native
        self.intervals = {"1": dict(begin=0, end=100000, begin_ns=0, end_ns=100000)}

    def valid(self):
        return R.check_document(self.rows, PID)

    def census(self, expected=2):
        return R.census_case(self.valid(), self.native, self.intervals, PID, expected_originals=expected)

    def play(self):
        return self.census()["plays"]["1"]

    def semantic_failure(self, text="."):
        # Actual rejection is well-formed evidence; it fails classification.
        self.valid()
        result = self.census()
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["plays"]["1"]["ledger_status"], "FAIL")
        self.assertRegex(" ".join(result["plays"]["1"]["errors"]), text)

    def typed_failure(self, text="."):
        with self.assertRaisesRegex(ValueError, text):
            self.valid()

    def records(self, event, token=None, replay=None):
        return [e for e in self.rows[1:-1] if e["event"] == event and
                (token is None or e["token"] == token) and
                (replay is None or e["restart_ordinal"] == int(replay))]

    def test_accepted_original_and_completed_replay_are_separate_populations(self):
        result = self.census();play = result["plays"]["1"]
        self.assertEqual(result["status"], "PASS_SCOPED_REPLAY_DISPOSITION_CENSUS")
        self.assertEqual(play["status"], "PASS")
        self.assertEqual((play["original_input_commits"], play["completed_replay_input_commits"]), (2, 1))
        self.assertEqual((play["original_field_records"], play["replay_field_records"]), (4, 2))
        self.assertEqual(play["event_counts"]["input_sent"], 3)
        self.assertEqual(play["event_counts"]["output_dequeued"], 6)
        self.assertEqual(play["materialized_originals"], 2)
        self.assertEqual(len(play["output_classifications"]), 6)
        self.assertTrue(all(not h["live_ownership_restored"] for h in play["historical_provenance"]))

    def test_default900_gate_remains_fail_for_balanced_diagnostic_subset(self):
        result = R.census_case(self.valid(), self.native, self.intervals, PID)
        play = result["plays"]["1"]
        self.assertEqual(result["status"], "PASS_SCOPED_REPLAY_DISPOSITION_CENSUS")
        self.assertEqual(play["ledger_status"], "PASS")
        self.assertEqual(play["status"], "FAIL")
        self.assertFalse(play["source_population_qualified"])
        self.assertEqual(play["source_metadata_pictures"], 900)

    def test_ordinary_no_replay_is_not_exercised_not_a_requested_rerun(self):
        self.fixture = Fixture(replay=False)
        self.rows, self.native = self.fixture.document(), self.fixture.native
        self.assertEqual(self.play()["replay_status"], "NOT_EXERCISED")
        self.assertEqual(self.play()["ledger_status"], "PASS")

    def test_missing_public_materialization_preserves_legacy_subset_without_pixel_claim(self):
        for _, row in self.native:
            if row["event"] == "materialized":
                row["event"] = "diagnostic_marker"
        play = self.play()
        self.assertEqual(play["ledger_status"], "PASS")
        self.assertEqual(play["original_input_commits"], 2)
        self.assertEqual(play["materialized_originals"], 0)
        self.assertEqual(play["originals_without_public_materialization"], 2)
        self.assertEqual(play["public_materialization_population"], "NOT_OBSERVED")
        self.assertEqual(play["public_pixel_qualification"], "NOT_ESTABLISHED_BY_THIS_CENSUS")

    def test_retained_full_original_identity_after_backing_retirement_is_private_completion(self):
        self.fixture = Fixture(replay=False, retire_original=True)
        self.rows, self.native = self.fixture.document(), self.fixture.native
        play = self.play()
        self.assertEqual(play["ledger_status"], "PASS")
        self.assertEqual(play["materialized_originals"], 1)
        self.assertEqual(play["public_materialization_population"], "OBSERVED_SUBSET")
        self.assertEqual(play["originals_without_public_materialization"], 1)

    def test_zero_unknown_original_tuple_cannot_use_completed_replay_history_exception(self):
        rid = self.records("output", 1, False)[0]["receive_id"]
        seq = self.records("output", 1, False)[0]["native_seq"]
        for _, row in self.native:
            if row["seq"] == seq:
                for key in R.BINDING[4:]:
                    row[key] = 0
        for event in self.rows[1:-1]:
            if event["receive_id"] == rid:
                for key in R.LIVE:
                    event[key] = 0
        self.semantic_failure("new original live binding")

    def test_retired_backing_keeps_raw_zeros_and_immutable_offline_history(self):
        self.fixture = Fixture(retired=True)
        self.rows, self.native = self.fixture.document(), self.fixture.native
        original = copy.deepcopy(self.native)
        play = self.play()
        replay = [r for r in play["output_classifications"] if r["role"] == "replay_duplicate_field"]
        self.assertEqual(play["ledger_status"], "PASS")
        self.assertEqual(len(replay), 2)
        self.assertTrue(all(all(v == 0 for v in r["live_tuple"].values()) for r in replay))
        history = next(h for h in play["historical_provenance"] if h["history_key"][-1] == 1)
        self.assertIsNotNone(history["binding_end_native_seq"])
        self.assertGreater(history["binding"]["owner"], 0)
        self.assertEqual(history["scope"], "HISTORICAL_ONLY")
        self.assertEqual(self.native, original)

    def test_busy_retry_fragments_and_consumed_prefeed_make_one_logical_original(self):
        self.fixture = Fixture(fragments=True, prefeed=True)
        self.rows, self.native = self.fixture.document(), self.fixture.native
        play = self.play()
        self.assertEqual(play["ledger_status"], "PASS")
        self.assertEqual(play["original_input_commits"], 2)
        self.assertGreater(play["replay_event_counts"]["input_attempt"], play["event_counts"]["input_sent"])

    def test_fragment_continuation_can_move_between_actual_pump_threads(self):
        self.fixture = Fixture(fragments=True)
        self.rows, self.native = self.fixture.document(), self.fixture.native
        attempts = self.records("input_attempt", 1, False)
        attempts[-1]["tid"] = PID+1
        self.records("input_commit", 1, False)[0]["tid"] = PID+1
        self.assertEqual(self.play()["ledger_status"], "PASS")

    def test_commit_thread_must_match_only_final_successful_actual_attempt(self):
        self.fixture = Fixture(fragments=True)
        self.rows, self.native = self.fixture.document(), self.fixture.native
        self.records("input_attempt", 1, False)[-1]["tid"] = PID+1
        self.semantic_failure("final accepted")

    def test_hard_input_error_is_not_misclassified_as_busy_retry(self):
        self.records("input_attempt", 1, False)[0]["result"] = 17
        self.semantic_failure("terminal BC error")

    def test_busy_attempt_for_foreign_unadmitted_token_cannot_be_ignored(self):
        self.records("input_attempt", 1, False)[0]["token"] = 999
        self.semantic_failure("admitted original history")

    def test_terminal_failed_restart_after_complete_originals_remains_failure(self):
        for failed_phase in ("close", "open", "transition"):
            with self.subTest(failed_phase=failed_phase):
                self.fixture = Fixture(replay=False)
                self.fixture.native.pop()  # No prior context-destroy boundary.
                for phase in ("close", "open", "transition"):
                    self.fixture.emit("restart", restart_phase=phase, cached_units=2,
                        result=(0 if phase == "transition" else 26) if phase == failed_phase else 0)
                    if phase == failed_phase:
                        break
                self.rows, self.native = self.fixture.document(), self.fixture.native
                self.semantic_failure("actual restart .* failed")

    def test_failed_retry_does_not_advance_fragment_coverage(self):
        self.fixture = Fixture(fragments=True)
        self.rows, self.native = self.fixture.document(), self.fixture.native
        attempts = self.records("input_attempt", 1, False)
        attempts[1]["input_offset"] = 4
        self.semantic_failure("coverage")

    def test_wrong_parity_actual_product_rejection_is_valid_observation_failed_census(self):
        outputs = self.records("output", 1, True)
        rid = outputs[-1]["receive_id"]
        for e in self.rows[1:-1]:
            if e["receive_id"] == rid:
                e["field_flags"] = 48
                if e["event"] in ("disposition", "release"):
                    e["pair_valid"] = False
                    e["observe"] = None
        self.records("disposition", 1, True)[-1]["result"] = 1
        self.semantic_failure("disposition rejected")

    def test_forged_pair_true_cannot_override_raw_same_parity(self):
        rid = self.records("output", 1, True)[-1]["receive_id"]
        for e in self.rows[1:-1]:
            if e["receive_id"] == rid:
                e["field_flags"] = 48
        self.semantic_failure("parity")

    def test_pair_raw_picture_session_metadata_geometry_and_order_are_independent(self):
        for key, value in (("picture_number", 999), ("sess_num", 999), ("picture_meta_payload", 999),
                           ("width", 32), ("height", 64), ("field_flags", 184)):
            with self.subTest(key=key):
                self.rows = self.fixture.document()
                rid = self.records("output", 1, True)[-1]["receive_id"]
                for e in self.rows[1:-1]:
                    if e["receive_id"] == rid:
                        e[key] = value
                self.semantic_failure("parity/identity")

    def test_release_failure_cannot_retire_completed_replay_debt(self):
        self.records("release", 1, True)[-1]["result"] = 26
        self.semantic_failure("release failed")

    def test_duplicate_inspection_without_actual_observe_cannot_finish_pair(self):
        for e in self.records("disposition", 1, True)[-1:] + self.records("release", 1, True)[-1:]:
            e["observe"] = None
        self.semantic_failure("Observe")

    def test_pair_first_id_must_name_actual_first_lease(self):
        for e in self.records("disposition", 1, True)[-1:] + self.records("release", 1, True)[-1:]:
            e["pair_first_receive_id"] = 1
        self.semantic_failure("prior released first")

    def test_completed_flag_without_original_completion_cannot_forge_replay(self):
        for e in self.records("input_attempt", 1, False) + self.records("input_commit", 1, False):
            e["completed_before"] = True
        self.semantic_failure("completed-history state")

    def test_replay_input_claimed_new_cannot_repeat_original(self):
        for e in self.records("input_attempt", 1, True) + self.records("input_commit", 1, True):
            e["completed_before"] = False
        self.semantic_failure("completed-history state|more than once")

    def test_unknown_nonreplay_output_not_repaired_by_later_history(self):
        # An actual unknown inspection remains well-typed but cannot classify.
        for e in self.records("disposition", 1, True)[:1] + self.records("release", 1, True)[:1]:
            e["inspect"] = "unknown"
            e["replay_duplicate"] = False
        self.semantic_failure("inspection")

    def test_known_replay_owner_conflicting_with_history_is_not_rebound(self):
        seq = self.records("output", 1, True)[0]["native_seq"]
        for _, row in self.native:
            if row["seq"] == seq:
                row["owner"] = 999
        rid = self.records("output", 1, True)[0]["receive_id"]
        for e in self.rows[1:-1]:
            if e["receive_id"] == rid:
                e["live_owner"] = 999
        self.semantic_failure("conflicts")

    def test_wrong_generation_does_not_alias_same_token(self):
        self.records("input_commit", 1, True)[0]["generation"] = 2
        self.semantic_failure("restart lifetime|attempt")

    def test_native_link_zero_missing_wrong_event_or_reused_cannot_pass(self):
        original = self.fixture.document()
        for value in (0, 9999, 1):
            with self.subTest(value=value):
                self.rows = copy.deepcopy(original)
                self.records("input_commit", 1, True)[0]["native_seq"] = value
                if value == 0:
                    self.typed_failure("receipt")
                else:
                    self.semantic_failure("receipt")

    def test_missing_native_input_output_not_silently_discarded(self):
        self.native[-1][1]["event"] = "input_sent"
        self.semantic_failure("unmatched")

    def test_replay_materialization_fails_even_if_it_is_the_only_one(self):
        # Move the only original1 materialization past replay input, retaining
        # native sequence and clocks. Cardinality alone must not qualify it.
        mat = next(r for _, r in self.native if r["event"] == "materialized" and r["token"] == 1)
        target = self.native[-1][1]  # A later terminal marker, not a linked output.
        mat["event"] = "diagnostic_marker"
        target["event"] = "materialized"
        target.update(self.fixture.binding(1))
        self.semantic_failure("rematerialized")

    def test_empty_complete_document_is_not_a_case_success(self):
        self.rows = [self.rows[0], dict(self.rows[-1], events=0, attempted_events=0)]
        self.semantic_failure("empty")

    def test_restart_requires_exact_successful_phase_sequence_and_cache_snapshot(self):
        for phase, key, value in (("open", "result", 1), ("transition", "result", 0),
                                  ("committed", "restart_ordinal", 2), ("transition", "cached_units", 9)):
            with self.subTest(phase=phase, key=key):
                self.rows = self.fixture.document()
                e = next(e for e in self.rows[1:-1] if e["restart_phase"] == phase)
                e[key] = value
                self.semantic_failure("restart")

    def test_context_retirement_cannot_be_used_as_historical_replay_lifetime(self):
        end = next(r for _, r in self.native if r["event"] == "materialized")
        end["event"] = "context_reset"
        self.semantic_failure("generation lifetime")

    def test_missing_or_conflicting_native_submit_history_is_rejected(self):
        self.native[0][1]["event"] = "diagnostic_marker"
        self.semantic_failure("admitted|history")

    def test_native_sequence_gap_foreign_pid_and_reordered_offset_fail(self):
        original = copy.deepcopy(self.native)
        for key, value in (("seq", 999), ("pid", PID+1)):
            with self.subTest(key=key):
                self.native = copy.deepcopy(original)
                self.native[3][1][key] = value
                with self.assertRaisesRegex(ValueError, "native"):
                    self.census()
        self.native = copy.deepcopy(original)
        self.native[3] = (0, self.native[3][1])
        with self.assertRaisesRegex(ValueError, "offset"):
            self.census()

    def test_event_missing_duplicate_reordered_foreign_and_clock_regression_reject(self):
        for key, value in (("seq", 999), ("id", 0), ("pid", PID+1), ("monotonic_ns", 1)):
            with self.subTest(key=key):
                self.rows = self.fixture.document()
                self.rows[4][key] = value
                self.typed_failure()

    def test_output_lifetime_exact_same_thread_raw_metadata_and_single_release(self):
        for key, value in (("tid", PID+1), ("live_owner", 999), ("picture_number", 999), ("receive_id", 999)):
            with self.subTest(key=key):
                self.rows = self.fixture.document()
                self.records("release", 1, True)[0][key] = value
                self.typed_failure()

    def test_incomplete_overflow_fork_policy_and_old_untagged_failure_stay_fail(self):
        for key, value in (("complete", False), ("dropped", 1), ("unpublished", 1), ("inflight", 1),
                           ("attempted_events", 999), ("lifecycle_failures", 1), ("clock_failures", 1)):
            with self.subTest(key=key):
                self.rows = self.fixture.document();self.rows[-1][key] = value
                self.typed_failure("incomplete")
        self.rows = self.fixture.document();del self.rows[0]["fork_policy"]
        self.rows[-1]["fork_seen"] = True
        self.typed_failure("scope")

    def test_descriptive_parent_fork_does_not_invent_child_observation(self):
        self.rows[-1]["fork_seen"] = True
        self.assertEqual(self.play()["ledger_status"], "PASS")
        self.rows[2]["pid"] = PID+1
        self.typed_failure("foreign")

    def test_field_constant_mutation_cannot_redefine_bad_parity_as_valid(self):
        self.rows[0]["field_constants"]["bottomfield"] = 16
        self.typed_failure("constants")

    def test_window_cuts_lifetime_or_leaves_operations_unassigned_fails(self):
        self.intervals["1"]["end_ns"] = self.records("disposition", 1, True)[0]["monotonic_ns"]
        self.semantic_failure("cuts|outside")

    def test_strict_file_read_duplicate_keys_symlinks_and_filename_pid(self):
        raw = b"\n".join(json.dumps(r).encode() for r in self.rows)+b"\n"
        with tempfile.TemporaryDirectory() as name:
            path = Path(name)/("replay-disposition-"+str(PID)+".jsonl")
            path.write_bytes(raw)
            self.assertEqual(R.audit_report(path, PID)["status"], "PASS_SCOPED_REPLAY_DISPOSITION_OBSERVATION")
            link = Path(name)/("alias-"+str(PID)+".jsonl");link.symlink_to(path)
            self.assertEqual(R.audit_report(link, PID)["status"], "FAIL")
            self.assertEqual(R.audit_report(path, PID+1)["status"], "FAIL")
        with self.assertRaises(ValueError):
            R.parse_report(raw.replace(b'"pid": 123', b'"pid": 123, "pid": 123', 1))


if __name__ == "__main__":
    unittest.main()
