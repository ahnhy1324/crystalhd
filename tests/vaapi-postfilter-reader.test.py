#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Device-free semantic controls for complete OWN post-filter evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


_spec = importlib.util.spec_from_file_location(
    "postfilter_reader", Path(__file__).with_name("vaapi-postfilter-reader.py"))
R = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(R)
PID = 123
NV12 = 23                       # Fixture enum; reader takes the header's value.


def fixture():
    header = dict(type="header", schema=R.SCHEMA, pid=PID, capacity=16384,
                  snapshot_capacity=16, max_width=1920, max_height=1088,
                  nv12_format=NV12, sink_constraint_basis=R.BASIS,
                  fork_policy=R.FORK_POLICY,
                  filter_provider=R.PROVIDERS["filter"], util_provider=R.PROVIDERS["util"],
                  filter_version=R.VERSIONS["filter"], util_version=R.VERSIONS["util"], provider_ok=True)
    events, blobs = [], {}
    def frame(pts, snapshot=True):
        result = dict(pts=pts, best_effort_timestamp=-(1 << 63), duration=0,
                      time_base=[0, 1], format=NV12, width=4, height=4, linesize=[8, 8],
                      flags=1, sample_aspect_ratio=[1, 1], color_range=1, color_primaries=2,
                      color_trc=2, colorspace=2, chroma_location=1, crop=[0, 0, 0, 0], snapshot=None)
        if snapshot:
            slot = len(blobs)
            name = f"postfilter-{PID}-s{slot:02d}.nv12"
            result["snapshot"] = dict(slot=slot, file=name, bytes=24)
            blobs[name] = bytes((pts + i) % 256 for i in range(24))
        return result
    def event(api, **fields):
        seq = len(events)
        value = dict(type="event", seq=seq, pid=PID, tid=PID, begin_ns=100+seq*10,
                     end_ns=105+seq*10, api=api, graph_id=1, graph="0x100", context="0x0",
                     ret=None if api == R.FREE else 0, errno_before=17, errno_after=18,
                     flags=0, input_null=False, close_pts=0, input_index=0, output_index=0,
                     name="", args="", frame=None)
        value.update(fields)
        events.append(value)
    event(R.CREATE, context="0x200", name="buffer",
          args="video_size=4x4:pix_fmt=23:time_base=1/1:pixel_aspect=1/1")
    event(R.PARSE, args="bwdif=1:-1")
    event("av_buffersrc_add_frame_flags", context="0x200", input_index=1, frame=frame(1000000), flags=8)
    event("av_buffersink_get_frame", context="0x300", ret=R.EAGAIN)
    event("av_buffersrc_add_frame", context="0x200", input_index=2, frame=frame(1033367))
    for index in (1, 2):
        event("av_buffersink_get_frame", context="0x300", output_index=index,
              frame=frame(2000000+index), sink_time_base=[1, 2], sink_frame_rate=[0, 0])
    event("av_buffersink_get_frame", context="0x300", ret=R.EAGAIN)
    event("av_buffersrc_add_frame", context="0x200", input_index=3, frame=frame(1066733))
    for index in (3, 4):
        event("av_buffersink_get_frame", context="0x300", output_index=index,
              frame=frame(2000000+index), sink_time_base=[1, 2], sink_frame_rate=[0, 0])
    event("av_buffersink_get_frame", context="0x300", ret=R.EAGAIN)
    event("av_buffersrc_add_frame", context="0x200", input_null=True)
    event("av_buffersink_get_frame", context="0x300", output_index=5,
          frame=frame(2000005, False), sink_time_base=[1, 2], sink_frame_rate=[0, 0])
    event("av_buffersink_get_frame", context="0x300", ret=R.EOF)
    event(R.FREE)
    footer = dict(type="footer", events=len(events), attempted_events=len(events), dropped=0,
                  snapshots=len(blobs), snapshot_failures=0, lifecycle_failures=0,
                  string_truncations=0, clock_failure=False, fork_seen=False, inflight=0,
                  unpublished=0, open_graphs=0, complete=True)
    return [header, *events, footer], blobs


def renumber(rows):
    for seq, e in enumerate(rows[1:-1]):
        e.update(seq=seq, begin_ns=100+seq*10, end_ns=105+seq*10)
    rows[-1].update(events=len(rows)-2, attempted_events=len(rows)-2)


class ReaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.path = self.directory / f"postfilter-{PID}.jsonl"
        self.rows, self.blobs = fixture()

    def valid(self):
        return R.check_document(self.rows, self.blobs, PID, report_path=self.path)

    def invalid(self, pattern=None):
        with self.assertRaisesRegex(ValueError, pattern or "."):
            self.valid()

    def publish(self):
        self.path.write_text("".join(json.dumps(row)+"\n" for row in self.rows))
        for name, data in self.blobs.items():
            (self.directory/name).write_bytes(data)

    def reference(self):
        valid = self.valid()
        manifest = R.reference_prefix(valid, 1)
        outputs = []
        for i, wanted in enumerate(manifest["expected_outputs"], 1):
            result = copy.deepcopy(wanted)
            result["path"] = str(self.directory/f"output-{i:02d}.nv12")
            result["reference_only_eof"] = False
            Path(result["path"]).write_bytes(self.blobs[wanted["snapshot"]["file"]])
            outputs.append(result)
        return valid, dict(schema=1, status="PASS", mode="prefix", manifest=manifest,
                           outputs=outputs, providers=manifest["providers"], constraint_basis=R.BASIS,
                           source_clock_comparison=False, original_client_eof_claim=False,
                           source_eof_injected=False,
                           comparison_scope="SAME_INPUT_PREFIX_ACTIVE_NV12_AND_CAPTURED_METADATA")

    def test_complete_ledger_counts_attempts_returns_and_eof_separately(self):
        valid = self.valid()
        g = valid["graphs"][0]
        self.assertEqual(g["counts"]["source_attempts"], 3)
        self.assertEqual(g["counts"]["source_accepted"], 3)
        self.assertEqual(g["counts"]["sink_successes"], 5)
        self.assertEqual(g["counts"]["sink_eagain"], 3)
        self.assertEqual(g["counts"]["source_null_attempts"], 1)
        self.assertEqual(g["counts"]["sink_eof"], 1)
        self.assertEqual(g["positive_input_interval_ns"], [120, 185])
        self.assertFalse(valid["source_clock_comparison"])

    def test_missing_event_is_not_repaired_by_footer(self):
        del self.rows[4]
        self.rows[-1]["events"] -= 1
        self.rows[-1]["attempted_events"] -= 1
        self.invalid("sequence")

    def test_reordered_calls_rejected_even_with_adjusted_counts(self):
        self.rows[5], self.rows[6] = self.rows[6], self.rows[5]
        self.invalid("sequence")

    def test_duplicate_source_index_and_wrong_sink_index_rejected(self):
        for key, row in (("input_index", 5), ("output_index", 7)):
            with self.subTest(key=key):
                saved = self.rows[row][key]
                self.rows[row][key] = 1
                self.invalid("index")
                self.rows[row][key] = saved

    def test_foreign_pid_and_provider_are_not_client_success(self):
        for row, key, value in ((self.rows[3], "pid", PID+1),
                                (self.rows[0], "filter_provider", "/foreign/libavfilter.so.12.1.102"),
                                (self.rows[0], "util_version", R.VERSIONS["util"]-1),
                                (self.rows[0], "provider_ok", False)):
            with self.subTest(key=key):
                old = row[key]
                row[key] = value
                self.invalid()
                row[key] = old

    def test_every_incomplete_publication_state_rejects(self):
        for key in ("dropped", "snapshot_failures", "lifecycle_failures", "string_truncations",
                    "inflight", "unpublished", "open_graphs", "clock_failure", "complete"):
            with self.subTest(key=key):
                old = self.rows[-1][key]
                self.rows[-1][key] = not old if type(old) is bool else 1
                self.invalid()
                self.rows[-1][key] = old

    def test_explicit_parent_only_policy_keeps_fork_descriptive(self):
        self.rows[-1]["fork_seen"] = True
        valid = self.valid()
        self.assertTrue(valid["process_scope"]["fork_seen"])
        self.assertFalse(valid["process_scope"]["children_observed"])
        self.assertEqual(valid["process_scope"]["pid"], PID)
        self.assertEqual(R.reference_prefix(valid, 1)["mode"], "prefix")

    def test_parent_only_fork_does_not_hide_other_incomplete_states(self):
        self.rows[-1]["fork_seen"] = True
        for key in ("dropped", "snapshot_failures", "lifecycle_failures", "string_truncations",
                    "inflight", "unpublished", "open_graphs", "clock_failure", "complete"):
            with self.subTest(key=key):
                old = self.rows[-1][key]
                self.rows[-1][key] = not old if type(old) is bool else 1
                self.invalid()
                self.rows[-1][key] = old

    def test_legacy_fork_failure_cannot_be_silently_upgraded(self):
        del self.rows[0]["fork_policy"]
        self.rows[-1].update(fork_seen=True, complete=False)
        self.invalid("legacy report")
        # Even changing complete in a legacy footer cannot remove its fork boundary.
        self.rows[-1]["complete"] = True
        self.invalid("legacy report")
        self.rows[-1]["fork_seen"] = False
        self.assertEqual(self.valid()["process_scope"]["fork_policy"], "LEGACY_NO_FORK")

    def test_false_complete_still_rejects_explicit_parent_only_fork(self):
        self.rows[-1].update(fork_seen=True, complete=False)
        self.invalid("incomplete")

    def test_unknown_or_malformed_fork_policy_rejected(self):
        for value in (None, False, "ALL_CHILDREN_QUALIFIED"):
            with self.subTest(value=value):
                self.rows[0]["fork_policy"] = value
                self.invalid("fork observation policy")
        self.rows[0]["fork_policy"] = R.FORK_POLICY
        for value in (0, 1, None, "false"):
            with self.subTest(value=value):
                self.rows[-1]["fork_seen"] = value
                self.invalid("boolean")

    def test_parent_only_policy_still_rejects_a_fork_child_event(self):
        self.rows[-1]["fork_seen"] = True
        self.rows[3]["pid"] = PID+1
        self.invalid("foreign event PID")

    def test_capacity_overflow_rejected_without_dropped_counter(self):
        self.rows[0]["capacity"] = 4
        self.invalid("capacity")

    def test_false_footer_or_boolean_counter_rejected(self):
        self.rows[-1]["events"] = True
        self.invalid("integer")

    def test_graph_requires_one_terminal_free(self):
        del self.rows[-2]
        renumber(self.rows)
        self.invalid("free")

    def test_call_after_free_rejected(self):
        extra = copy.deepcopy(self.rows[4])
        self.rows.insert(-1, extra)
        renumber(self.rows)
        self.invalid("after graph free")

    def test_teardown_cannot_overlap_original_live_call(self):
        self.rows[-3]["tid"] = PID+1
        self.rows[-3]["end_ns"] = self.rows[-2]["end_ns"]
        self.invalid("teardown overlaps")

    def test_same_thread_clock_overlap_rejected(self):
        self.rows[6]["begin_ns"] = self.rows[5]["end_ns"]-1
        self.invalid("same-thread")

    def test_source_context_requires_matching_completed_buffer_creation(self):
        self.rows[3]["context"] = "0x999"
        self.invalid("matching buffer")

    def test_source_context_cannot_be_sink(self):
        self.rows[4]["context"] = "0x200"
        self.invalid("source context used as sink")

    def test_empty_init_graph_and_retired_address_reuse_are_accepted(self):
        create, free = copy.deepcopy(self.rows[1]), copy.deepcopy(self.rows[-2])
        for e in self.rows[1:-1]:
            e["graph_id"] = 2
        self.rows[1:1] = [create, free]
        renumber(self.rows)
        valid = self.valid()
        self.assertEqual(valid["graphs"][0]["scope"], "EMPTY_OR_OTHER_FORMAT_SCALAR_ONLY")
        self.assertEqual(R.reference_prefix(valid, 2)["mode"], "prefix")

    def test_overlapping_address_reuse_is_not_new_lifetime(self):
        create, free = copy.deepcopy(self.rows[1]), copy.deepcopy(self.rows[-2])
        create["graph_id"] = free["graph_id"] = 2
        self.rows.insert(2, create)
        self.rows.insert(-1, free)
        renumber(self.rows)
        self.invalid("address reused")

    def test_missing_bwdif_parse_on_active_nv12_is_not_empty_graph(self):
        self.rows[2]["args"] = "null"
        self.invalid("BWDIF parse")

    def test_source_eof_absent_is_finding_not_synthetic_flush(self):
        del self.rows[13]
        renumber(self.rows)
        g = self.valid()["graphs"][0]
        self.assertEqual(g["counts"]["source_eof_successes"], 0)
        self.assertIn("SINK_EOF_WITHOUT_OBSERVED_SOURCE_EOF", g["findings"])

    def test_failed_null_is_not_successful_source_eof(self):
        self.rows[13]["ret"] = -22
        g = self.valid()["graphs"][0]
        self.assertEqual(g["counts"]["source_null_attempts"], 1)
        self.assertEqual(g["counts"]["source_eof_successes"], 0)

    def test_observed_close_retains_raw_pts_in_distinct_event(self):
        self.rows[13].update(api=R.CLOSE, input_null=False, close_pts=-(1 << 63), flags=3)
        g = self.valid()["graphs"][0]
        self.assertEqual(g["counts"]["source_close_attempts"], 1)
        self.assertEqual(g["events"][12]["close_pts"], -(1 << 63))

    def test_accepted_input_after_original_eof_rejected(self):
        self.rows[8].update(api="av_buffersrc_add_frame", context="0x200", ret=0, input_null=True)
        self.invalid("follows successful")

    def test_failed_input_still_counts_attempt_and_snapshot(self):
        self.rows[3]["ret"] = -22
        valid = self.valid()
        self.assertEqual(valid["graphs"][0]["counts"]["source_attempts"], 3)
        self.assertEqual(valid["graphs"][0]["counts"]["source_accepted"], 2)
        with self.assertRaisesRegex(ValueError, "accepted"):
            R.reference_prefix(valid, 1)

    def test_positive_success_return_preserved_in_reference_schedule(self):
        self.rows[3]["ret"] = self.rows[6]["ret"] = 1
        manifest = R.reference_prefix(self.valid(), 1)
        self.assertEqual(manifest["schedule"][0]["ret"], 1)
        self.assertEqual(manifest["schedule"][3]["ret"], 1)

    def test_snapshot_selection_and_size_are_enforced(self):
        for change in ("missing", "short", "slot", "file", "geometry"):
            with self.subTest(change=change):
                rows, blobs = copy.deepcopy(self.rows), copy.deepcopy(self.blobs)
                snap = self.rows[3]["frame"]["snapshot"]
                if change == "missing":
                    self.rows[3]["frame"]["snapshot"] = None
                elif change == "short":
                    self.blobs[snap["file"]] = b"short"
                elif change == "slot":
                    snap["slot"] = 1
                elif change == "file":
                    snap["file"] = "../outside.nv12"
                else:
                    self.rows[3]["frame"]["linesize"] = [3, 8]
                self.invalid()
                self.rows, self.blobs = rows, blobs

    def test_late_snapshot_cannot_replace_selected_prefix(self):
        self.rows[14]["frame"]["snapshot"] = copy.deepcopy(self.rows[11]["frame"]["snapshot"])
        self.invalid("duplicate snapshot")

    def test_header_enum_controls_nv12_not_hardcoded_number(self):
        self.rows[0]["nv12_format"] = 99
        for e in self.rows[1:-1]:
            if e["frame"] is not None:
                e["frame"]["format"] = 99
        self.assertEqual(R.reference_prefix(self.valid(), 1)["nv12_format"], 99)

    def test_ambiguous_raw_clocks_preserved_without_source_seconds(self):
        manifest = R.reference_prefix(self.valid(), 1)
        self.assertEqual(manifest["inputs"][0]["time_base"], [0, 1])
        self.assertEqual(manifest["expected_outputs"][0]["sink_time_base"], [1, 2])
        self.assertEqual(manifest["expected_outputs"][0]["sink_frame_rate"], [0, 0])
        self.assertNotIn("seconds", json.dumps(manifest))
        self.assertEqual(manifest["inputs"][0]["pts"], 1000000)

    def test_prefix_schedule_keeps_original_calls_eagain_and_no_eof(self):
        manifest = R.reference_prefix(self.valid(), 1)
        self.assertEqual(len(manifest["inputs"]), 3)
        self.assertEqual(len(manifest["expected_outputs"]), 4)
        self.assertEqual(sum(c["ret"] == R.EAGAIN for c in manifest["schedule"]), 2)
        self.assertEqual(manifest["schedule"][0]["flags"], 8)
        self.assertFalse(any(c["ret"] == R.EOF or c["api"] == R.CLOSE for c in manifest["schedule"]))

    def test_partial_unflushed_observation_stays_valid_without_reference(self):
        self.rows = self.rows[:5] + [self.rows[-2], self.rows[-1]]
        self.blobs = {next(iter(self.blobs)): next(iter(self.blobs.values()))}
        self.rows[-1]["snapshots"] = 1
        renumber(self.rows)
        valid = self.valid()
        self.assertIn("NO_ORIGINAL_SINK_EOF_OBSERVED", valid["graphs"][0]["findings"])
        with self.assertRaisesRegex(ValueError, "four observed"):
            R.reference_prefix(valid, 1)

    def test_strict_json_duplicate_keys_and_missing_newline(self):
        with self.assertRaises(ValueError):
            R.parse_report(b'{"type":"header","type":"footer"}\n{}\n')
        with self.assertRaises(ValueError):
            R.parse_report(b'{}\n{}')

    def test_read_regular_rejects_snapshot_symlink_and_wrong_pid_filename(self):
        self.publish()
        name = next(iter(self.blobs))
        target = self.directory/name
        target.unlink()
        target.symlink_to(self.directory/"missing")
        self.assertEqual(R.audit_report(self.path, PID)["status"], "FAIL")
        with self.assertRaisesRegex(ValueError, "PID binding"):
            R.check_document(self.rows, self.blobs, PID, report_path=self.directory/"renamed.jsonl")

    def test_file_api_and_audit_summary_remain_scoped(self):
        self.publish()
        result = R.audit_report(self.path, PID)
        self.assertEqual(result["status"], "PASS_SCOPED_POSTFILTER_OBSERVATION")
        self.assertTrue(result["graphs"][0]["prefix_available"])
        self.assertFalse(result["original_client_eof_claim"])

    def test_same_input_reference_rechecks_pixels_and_raw_properties(self):
        self.publish()
        valid, reference = self.reference()
        result = R.join_reference(valid, 1, reference)
        self.assertEqual(result["status"], "PASS_SAME_INPUT_PREFIX")
        self.assertEqual(sum(c["active_bytes"] for c in result["comparisons"]), 96)

    def test_reference_changed_byte_is_not_hidden_by_pass_label(self):
        self.publish()
        valid, reference = self.reference()
        Path(reference["outputs"][0]["path"]).write_bytes(b"x"*24)
        with self.assertRaisesRegex(ValueError, "bytes differ"):
            R.join_reference(valid, 1, reference)

    def test_reference_cannot_alias_original_snapshot_as_its_own_output(self):
        self.publish()
        valid, reference = self.reference()
        reference["outputs"][0]["path"] = reference["manifest"]["expected_outputs"][0]["path"]
        with self.assertRaisesRegex(ValueError, "distinct indexed"):
            R.join_reference(valid, 1, reference)

    def test_reference_foreign_provider_clock_schedule_and_injected_eof_reject(self):
        self.publish()
        valid, original = self.reference()
        for mutation in ("provider", "clock", "schedule", "injected", "flushed", "metadata", "missing"):
            with self.subTest(mutation=mutation):
                ref = copy.deepcopy(original)
                if mutation == "provider":
                    ref["providers"]["filter"] = "/other/libavfilter.so.12.1.102"
                elif mutation == "clock":
                    ref["source_clock_comparison"] = True
                elif mutation == "schedule":
                    ref["manifest"]["schedule"][0]["flags"] = 0
                elif mutation == "injected":
                    ref["source_eof_injected"] = True
                elif mutation == "flushed":
                    ref["mode"] = "flushed-reference"
                elif mutation == "metadata":
                    ref["outputs"][0]["time_base"] = [1, 1]
                else:
                    ref["outputs"].pop()
                with self.assertRaises(ValueError):
                    R.join_reference(valid, 1, ref)


if __name__ == "__main__":
    unittest.main()
