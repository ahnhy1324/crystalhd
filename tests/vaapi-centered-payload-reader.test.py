#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""CPU-only, independent payload/trace controls; no VA/DRM/media/device access."""

import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


_path = Path(__file__).with_name("vaapi-centered-payload-reader.py")
_spec = importlib.util.spec_from_file_location("centered_payload_reader", _path)
reader = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(reader)
W, H, CW = 1280, 720, 1920


def geom(serial, width, height, epoch=1):
    pitch = [width + 64, width + 128]
    base = serial * 0x1000000
    return {"surface_serial": serial, "storage_epoch": epoch,
            "width": width, "height": height, "pitch": pitch,
            "plane_address": [base, base + pitch[0] * height], "fourcc": "NV12"}


def small_picture(number):
    # Explicit, unrelated Y/U/V signatures expose wrong plane/row/slot copies.
    # This fixture does not call the reader's row model or production writer.
    y = [bytes((31 + number, row % 256, row // 256, 219)) * (W // 4)
         for row in range(H)]
    uv = [bytes(((row * 3 + number * 23 + 11) % 251,
                 (row * 7 + number * 31 + 47) % 251)) * (W // 2)
          for row in range(H // 2)]
    return b"".join(y + uv)


def packed_canvas(source, physical):
    # Construct pitched physical storage with poisoned tails first, then put
    # two explicit source rectangles into separate Y and interleaved UV planes.
    # Packing is independent of the reader's expected-output row arithmetic.
    height, pitches = physical["height"], physical["pitch"]
    uv_base = pitches[0] * height
    backing = bytearray(b"\xe5") * (uv_base + pitches[1] * (height // 2))
    for row in range(height):
        begin = row * pitches[0]
        backing[begin:begin + CW] = b"\x10" * CW
    for row in range(height // 2):
        begin = uv_base + row * pitches[1]
        backing[begin:begin + CW] = b"\x80" * CW
    for row in range(H):
        begin = (180 + row) * pitches[0] + 320
        backing[begin:begin + W] = source[row * W:(row + 1) * W]
    for row in range(H // 2):
        begin = uv_base + (90 + row) * pitches[1] + 320
        source_begin = W * H + row * W
        backing[begin:begin + W] = source[source_begin:source_begin + W]
    assert backing[CW:pitches[0]] == b"\xe5" * (pitches[0] - CW)
    assert backing[uv_base + CW:uv_base + pitches[1]] == b"\xe5" * (pitches[1] - CW)
    result = [bytes(backing[row * pitches[0]:row * pitches[0] + CW])
              for row in range(height)]
    result.extend(bytes(backing[uv_base + row * pitches[1]:
                                uv_base + row * pitches[1] + CW])
                  for row in range(height // 2))
    return b"".join(result)


def trace_row(seq, event, binding=None, operation=0, outcome=0):
    row = {"schema": reader.TRACE_SCHEMA, "pid": 123, "driver_instance": 2,
           "session": 2, "seq": seq, "ts_monotonic_ns": seq * 100,
           "event": event, "operation": operation, "outcome": outcome}
    row.update({key: 0 for key in reader.BINDING_KEYS})
    if binding:
        row.update(binding)
    return row


def resequence(trace):
    for number, row in enumerate(trace, 1):
        row["seq"], row["ts_monotonic_ns"] = number, number * 100


def fixture():
    doc = {"schema": reader.SCHEMA, "pid": 123, "driver_instance": 2,
           "slots_required": 2, "media_pts_observed": False,
           "publication": {"kind": "selected-decoder-context-retirement",
                           "context": 7, "generation": 3, "status": 0,
                           "after_driver_unlock": True, "all_slots_frozen": True},
           "slots": []}
    blobs, trace = {}, []
    for number in range(2):
        binding = {"context": 7, "generation": 3, "token": 100000 * (number + 1),
                   "decode_identity": 11 + number, "submission_ordinal": number + 1,
                   "surface": 40 + number, "owner": 50 + number}
        small = geom(100 + number, W, H)
        composed = geom(200 + number, CW, 1088)
        direct = number == 1
        public = copy.deepcopy(composed) if direct else geom(300 + number, CW, 1080)
        source = small_picture(number)
        slot = {"binding": binding, "roi": [320, 180, W, H], "visible": [CW, 1080],
                "limited_range": True, "direct_public": direct,
                "small": small, "composed": composed,
                "copy_source": copy.deepcopy(composed), "public_canvas": public}
        slot.update({flag: True for flag in reader.STAGE_FLAGS})
        for stage, payload in (("small", source),
                               ("composed", packed_canvas(source, composed)),
                               ("public", packed_canvas(source, public))):
            name = "%s-%d.nv12" % (stage, number)
            slot[stage + "_file"], slot[stage + "_bytes"] = name, len(payload)
            blobs[name] = payload
        doc["slots"].append(slot)
        trace.append(trace_row(len(trace) + 1, "submit_bind", binding))
        trace.append(trace_row(len(trace) + 1, "materialized", binding))
        trace.append(trace_row(len(trace) + 1, "sync_enter", binding, 10 + number))
        trace.append(trace_row(len(trace) + 1, "sync_exit", binding, 10 + number))
        if number == 0:
            trace.append(trace_row(len(trace) + 1, "export_enter", binding, 20 + number))
            trace.append(trace_row(len(trace) + 1, "export_exit", binding, 20 + number))
        trace.append(trace_row(len(trace) + 1, "binding_end", binding))
    trace.append(trace_row(len(trace) + 1, "context_destroy", {"context": 7, "generation": 3}))
    trace.append(trace_row(len(trace) + 1, "surface_destroy", {"surface": 99}))
    return doc, blobs, trace


def reset_fixture():
    doc, blobs, trace = fixture()
    doc["schema"] = reader.RESET_SCHEMA
    doc["publication"].update(kind="selected-decoder-generation-reset",
                              next_generation=4, close_status=0, device_closed=True)
    trace[-2] = trace_row(trace[-2]["seq"], "context_reset", {"context": 7, "generation": 4})
    return doc, blobs, trace


class CenteredPayloadControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base_doc, cls.base_blobs, cls.base_trace = fixture()

    def fresh(self):
        return copy.deepcopy(self.base_doc), dict(self.base_blobs), copy.deepcopy(self.base_trace)

    def reject(self, mutation, message=None):
        doc, blobs, trace = self.fresh()
        mutation(doc, blobs, trace)
        with self.assertRaisesRegex(ValueError, message or "."):
            reader.check_document(doc, blobs, trace)

    def change_byte(self, doc, blobs, number, stage, plane, row, column):
        slot = doc["slots"][number]
        physical = slot["public_canvas" if stage == "public" else stage]
        offset = (physical["width"] * physical["height"] if plane == "UV" else 0)
        offset += row * physical["width"] + column
        name = slot[stage + "_file"]
        payload = bytearray(blobs[name])
        payload[offset] ^= 1
        blobs[name] = bytes(payload)

    def test_indirect_1088_to_1080_and_direct_1088_compare_all_planes(self):
        result = reader.check_document(*self.fresh())
        self.assertEqual(result["status"], "PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD")
        self.assertEqual(result["compared_bytes"]["composed"],
                         {"Y": CW * 1088 * 2, "UV": CW * 544 * 2})
        self.assertEqual(result["compared_bytes"]["public"],
                         {"Y": CW * (1080 + 1088), "UV": CW * (540 + 544)})
        self.assertEqual(result["pitch_padding"], "EXCLUDED_FROM_PAYLOAD_COMPARISON")
        self.assertEqual(result["coded_bottom_rows"], "COMPARED_WHEN_PRESENT")

    def test_media_pts_gpu_return_and_scanout_are_not_invented(self):
        result = reader.check_document(*self.fresh())
        for field in ("actual_media_pts", "source_ffprobe_pts_join", "gpu_reader_return",
                      "displayed_pixels"):
            self.assertEqual(result[field], "NOT_OBSERVED")
        self.assertEqual(result["token_identity"], "SYNTHETIC_VA_BINDING_TOKEN")
        self.reject(lambda d, b, t: d.__setitem__("media_pts_observed", True), "PTS")

    def test_composed_1080_and_public_1088_have_independent_uv_offsets(self):
        doc, blobs, trace = self.fresh()
        slot = doc["slots"][0]
        slot["composed"] = geom(200, CW, 1080)
        slot["copy_source"] = copy.deepcopy(slot["composed"])
        slot["public_canvas"] = geom(300, CW, 1088)
        for stage, descriptor in (("composed", "composed"), ("public", "public_canvas")):
            payload = packed_canvas(blobs[slot["small_file"]], slot[descriptor])
            blobs[slot[stage + "_file"]] = payload
            slot[stage + "_bytes"] = len(payload)
        result = reader.check_document(doc, blobs, trace)
        self.assertEqual(result["compared_bytes"]["composed"]["UV"], CW * (540 + 544))
        self.assertEqual(result["compared_bytes"]["public"]["UV"], CW * 544 * 2)

    def test_pitch_tails_are_outside_oracle_for_different_valid_pitches(self):
        doc, blobs, trace = self.fresh()
        for slot in doc["slots"]:
            for descriptor in ("small", "composed", "public_canvas"):
                value = slot[descriptor]
                width, height = value["width"], value["height"]
                value["pitch"] = [width, width + 256]
                value["plane_address"][1] = value["plane_address"][0] + width * height
            slot["copy_source"] = copy.deepcopy(slot["composed"])
        self.assertEqual(reader.check_document(doc, blobs, trace)["pitch_padding"],
                         "EXCLUDED_FROM_PAYLOAD_COMPARISON")

    def test_exports_are_separate_optional_observations(self):
        result = reader.check_document(*self.fresh())
        self.assertEqual([entry["successful_exports"] for entry in result["observations"]], [1, 0])
        doc, blobs, trace = self.fresh()
        trace[:] = [row for row in trace if not row["event"].startswith("export_")]
        resequence(trace)
        self.assertEqual([entry["successful_exports"] for entry in
                          reader.check_document(doc, blobs, trace)["observations"]], [0, 0])

    def test_retired_context_does_not_require_driver_destruction(self):
        doc, blobs, trace = self.fresh()
        self.assertFalse(any(row["event"] == "session_destroy" for row in trace))
        trace.append(trace_row(len(trace) + 1, "session_destroy"))
        self.assertTrue(reader.check_document(doc, blobs, trace)["status"].startswith("PASS_"))
        trace.append(trace_row(len(trace) + 1, "surface_destroy"))
        with self.assertRaisesRegex(ValueError, "terminal"):
            reader.check_document(doc, blobs, trace)

    def test_separate_second_context_manifest_is_accepted(self):
        doc, blobs, trace = self.fresh()
        doc["publication"].update(context=8, generation=4)
        for slot in doc["slots"]:
            slot["binding"].update(context=8, generation=4)
        for row in trace:
            if row["context"]:
                row.update(context=8, generation=4)
        self.assertEqual(reader.check_document(doc, blobs, trace)["slots"], 2)

    def test_each_stage_detects_roi_y_u_v_corruption(self):
        for stage in ("composed", "public"):
            for plane, row, column in (("Y", 180, 320), ("UV", 90, 320), ("UV", 90, 321)):
                with self.subTest(stage=stage, plane=plane, column=column):
                    self.reject(lambda d, b, t, s=stage, p=plane, r=row, c=column:
                                self.change_byte(d, b, 0, s, p, r, c), "mismatch")

    def test_each_stage_detects_border_y_u_v_corruption(self):
        for stage in ("composed", "public"):
            for plane, row, column in (("Y", 179, 320), ("Y", 180, 319),
                                       ("Y", 899, 1600), ("UV", 89, 320),
                                       ("UV", 90, 318), ("UV", 449, 1601)):
                with self.subTest(stage=stage, plane=plane, row=row, column=column):
                    self.reject(lambda d, b, t, s=stage, p=plane, r=row, c=column:
                                self.change_byte(d, b, 0, s, p, r, c), "mismatch")

    def test_coded_bottom_y_and_uv_are_compared_on_both_stages(self):
        for stage in ("composed", "public"):
            for plane, row, column in (("Y", 1087, 0), ("UV", 543, 0), ("UV", 543, 1)):
                with self.subTest(stage=stage, plane=plane, column=column):
                    self.reject(lambda d, b, t, s=stage, p=plane, r=row, c=column:
                                self.change_byte(d, b, 1, s, p, r, c), "mismatch")

    def test_identically_wrong_composed_and_public_do_not_pass(self):
        def mutate(doc, blobs, trace):
            for stage in ("composed", "public"):
                self.change_byte(doc, blobs, 1, stage, "Y", 180, 320)
        self.reject(mutate, "composed Y mismatch")

    def test_source_row_or_plane_mutation_is_detected_independently(self):
        for plane, row, column in (("Y", 1, 0), ("UV", 1, 0), ("UV", 1, 1)):
            with self.subTest(plane=plane, column=column):
                self.reject(lambda d, b, t, p=plane, r=row, c=column:
                            self.change_byte(d, b, 0, "small", p, r, c), "mismatch")

    def test_wrong_roi_rows_and_y_as_uv_are_detected(self):
        def y_as_uv(doc, blobs, trace):
            slot = doc["slots"][0]
            name = slot["composed_file"]
            data = bytearray(blobs[name])
            start = CW * slot["composed"]["height"] + 90 * CW + 320
            data[start:start + W] = blobs[slot["small_file"]][:W]
            blobs[name] = bytes(data)
        self.reject(y_as_uv, "composed [UV] mismatch")
        def adjacent_source_row(doc, blobs, trace):
            slot = doc["slots"][0]
            name = slot["public_file"]
            data = bytearray(blobs[name])
            data[180 * CW + 320:180 * CW + 320 + W] = blobs[slot["small_file"]][W:W * 2]
            blobs[name] = bytes(data)
        self.reject(adjacent_source_row, "public Y mismatch")

    def test_all_tentative_or_cancelled_stage_flags_are_rejected(self):
        for flag in reader.STAGE_FLAGS:
            with self.subTest(flag=flag):
                self.reject(lambda d, b, t, f=flag: d["slots"][0].__setitem__(f, False), flag)
                self.reject(lambda d, b, t, f=flag: d["slots"][0].__setitem__(f, 1), flag)

    def test_partial_or_duplicate_slots_are_rejected(self):
        self.reject(lambda d, b, t: d["slots"].pop(), "two selected")
        self.reject(lambda d, b, t: d["slots"].append(copy.deepcopy(d["slots"][0])), "two selected")
        self.reject(lambda d, b, t: d["slots"].__setitem__(1, copy.deepcopy(d["slots"][0])),
                    "duplicate selected")
        self.reject(lambda d, b, t: d.__setitem__("slots_required", True), "slots_required")

    def test_distinct_originals_and_increasing_selection_are_required(self):
        for field in ("token", "decode_identity", "submission_ordinal"):
            with self.subTest(field=field):
                self.reject(lambda d, b, t, f=field:
                            d["slots"][1]["binding"].__setitem__(f, d["slots"][0]["binding"][f]))
        self.reject(lambda d, b, t: d["slots"].reverse(), "materialization order")

    def test_completed_selection_does_not_invent_decode_or_media_order(self):
        doc, blobs, trace = self.fresh()
        # The earlier submitted source completes second. Keep every full
        # binding internally consistent and move only its original submission.
        for slot in doc["slots"]:
            slot["binding"]["submission_ordinal"] = 3 - slot["binding"]["submission_ordinal"]
        for row in trace:
            if row["submission_ordinal"]:
                row["submission_ordinal"] = 3 - row["submission_ordinal"]
        second_submit = next(row for row in trace
                             if row["event"] == "submit_bind" and row["submission_ordinal"] == 1)
        trace.remove(second_submit)
        trace.insert(0, second_submit)
        resequence(trace)
        result = reader.check_document(doc, blobs, trace)
        self.assertGreater(result["observations"][0]["submit_seq"],
                           result["observations"][1]["submit_seq"])
        self.assertEqual(result["actual_media_pts"], "NOT_OBSERVED")

    def test_reused_private_surface_serial_with_new_epoch_is_a_new_view(self):
        doc, blobs, trace = self.fresh()
        slot = doc["slots"][1]
        slot["direct_public"] = False
        slot["composed"] = geom(doc["slots"][0]["composed"]["surface_serial"], CW, 1088, epoch=2)
        slot["copy_source"] = copy.deepcopy(slot["composed"])
        slot["public_canvas"] = geom(301, CW, 1088)
        self.assertTrue(reader.check_document(doc, blobs, trace)["status"].startswith("PASS_"))

    def test_slots_cannot_mix_decoder_lifetimes(self):
        for field in ("context", "generation"):
            self.reject(lambda d, b, t, f=field:
                        d["slots"][1]["binding"].__setitem__(f, 99), "lifetime")

    def test_geometry_and_range_scope_are_exact(self):
        mutations = (("roi", [0, 0, W, H]), ("roi", [320, 182, W, H]),
                     ("visible", [CW, 1088]), ("limited_range", False),
                     ("direct_public", 1))
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                self.reject(lambda d, b, t, f=field, v=value: d["slots"][0].__setitem__(f, v))
        for stage in ("small", "composed", "copy_source", "public_canvas"):
            for field, value in (("height", 540), ("width", 1281), ("fourcc", "P010"),
                                 ("surface_serial", 0), ("storage_epoch", False)):
                self.reject(lambda d, b, t, s=stage, f=field, v=value:
                            d["slots"][0][s].__setitem__(f, v))

    def test_copy_source_serial_epoch_pitch_and_addresses_must_match(self):
        for field in ("surface_serial", "storage_epoch"):
            self.reject(lambda d, b, t, f=field:
                        d["slots"][0]["copy_source"].__setitem__(f, 999), "storage epoch/view")
        for field in ("pitch", "plane_address"):
            self.reject(lambda d, b, t, f=field:
                        d["slots"][0]["copy_source"][f].__setitem__(1,
                            d["slots"][0]["copy_source"][f][1] + 64), "storage epoch/view")

    def test_direct_and_indirect_storage_stage_mismatches_are_rejected(self):
        self.reject(lambda d, b, t:
                    d["slots"][1]["public_canvas"].__setitem__("storage_epoch", 2), "direct public")
        self.reject(lambda d, b, t: d["slots"][0].__setitem__("direct_public", True), "direct public")
        self.reject(lambda d, b, t: d["slots"][1].__setitem__("direct_public", False), "separate")
        self.reject(lambda d, b, t: d["slots"][0].__setitem__("composed", copy.deepcopy(d["slots"][0]["small"])),
                    "geometry")

    def test_plane_pitch_overlap_and_overflow_are_rejected(self):
        self.reject(lambda d, b, t: d["slots"][0]["small"].__setitem__("pitch", [W - 1, W]), "integer")
        self.reject(lambda d, b, t: d["slots"][0]["small"].__setitem__("plane_address", [1, 1]), "overlap")
        self.reject(lambda d, b, t: d["slots"][0]["small"].__setitem__("plane_address", [2 ** 64 - 1, 1]), "overflow")

    def test_packed_lengths_cannot_include_pitch_tails_or_omit_coded_rows(self):
        for stage in ("small", "composed", "public"):
            self.reject(lambda d, b, t, s=stage:
                        d["slots"][0].__setitem__(s + "_bytes", d["slots"][0][s + "_bytes"] - 1),
                        "length")
            self.reject(lambda d, b, t, s=stage:
                        b.__setitem__(d["slots"][0][s + "_file"], b[d["slots"][0][s + "_file"]] + b"\xe5"),
                        "length")
        def truncate_coded(doc, blobs, trace):
            slot = doc["slots"][1]
            slot["public_bytes"] = CW * 1080 * 3 // 2
            blobs[slot["public_file"]] = blobs[slot["public_file"]][:slot["public_bytes"]]
        self.reject(truncate_coded, "length")

    def test_payload_stage_swaps_missing_blobs_and_duplicate_names_are_rejected(self):
        self.reject(lambda d, b, t: d["slots"][0].__setitem__("public_file", d["slots"][0]["small_file"]),
                    "unique")
        self.reject(lambda d, b, t: b.pop(d["slots"][0]["composed_file"]), "missing")
        self.reject(lambda d, b, t: b.__setitem__(d["slots"][0]["small_file"], bytearray(b"x")), "byte payload")
        def swap_sources(doc, blobs, trace):
            a, c = [slot["small_file"] for slot in doc["slots"]]
            blobs[a], blobs[c] = blobs[c], blobs[a]
        self.reject(swap_sources, "mismatch")

    def test_payload_paths_cannot_escape_manifest_directory(self):
        for name in ("../x", "/tmp/x", "nested/x", "a\\b", ".", "..", "a\x00b"):
            self.reject(lambda d, b, t, n=name: d["slots"][0].__setitem__("small_file", n), "filename")

    def test_publication_requires_successful_complete_unlocked_retirement(self):
        for field, value in (("status", 1), ("status", False),
                             ("after_driver_unlock", False), ("all_slots_frozen", False),
                             ("kind", "driver-destructor"), ("context", 8), ("generation", 4)):
            with self.subTest(field=field, value=value):
                self.reject(lambda d, b, t, f=field, v=value: d["publication"].__setitem__(f, v))
        self.reject(lambda d, b, t: d.pop("publication"), "object")

    def test_retirement_trace_cannot_be_missing_failed_duplicated_or_early(self):
        self.reject(lambda d, b, t: t.pop(-2), "sequence")
        self.reject(lambda d, b, t: t[-2].__setitem__("outcome", 1), "context_destroy")
        def duplicate(doc, blobs, trace):
            trace.insert(-1, copy.deepcopy(trace[-2]))
            resequence(trace)
        self.reject(duplicate, "context_destroy")
        def early(doc, blobs, trace):
            trace.insert(0, trace.pop(-2))
            resequence(trace)
        self.reject(early, "retirement")
        self.reject(lambda d, b, t: t[-2].__setitem__("owner", 1), "non-context")

    def test_submit_binding_requires_every_identity_component(self):
        for field in reader.BINDING_KEYS:
            with self.subTest(field=field):
                self.reject(lambda d, b, t, f=field: t[0].__setitem__(f, t[0][f] + 1), "submit_bind")
        self.reject(lambda d, b, t: t[0].__setitem__("outcome", 1), "submit_bind")

    def test_doubled_original_token_identity_or_ordinal_is_not_another_slot(self):
        for field in ("token", "decode_identity", "submission_ordinal"):
            def duplicate(doc, blobs, trace, identity=field):
                extra = copy.deepcopy(trace[0])
                for key in ("token", "decode_identity", "submission_ordinal", "surface", "owner"):
                    if key != identity:
                        extra[key] += 1000
                trace.insert(1, extra)
                resequence(trace)
            with self.subTest(field=field):
                self.reject(duplicate, "doubled original")

    def test_duplicated_exact_submit_is_rejected(self):
        def duplicate(doc, blobs, trace):
            trace.insert(1, copy.deepcopy(trace[0]))
            resequence(trace)
        self.reject(duplicate, "one successful submit_bind")

    def test_materialized_is_successful_unique_and_matches_public_binding(self):
        self.reject(lambda d, b, t: t[1].__setitem__("outcome", 1), "materialized")
        for field in reader.BINDING_KEYS:
            self.reject(lambda d, b, t, f=field: t[1].__setitem__(f, t[1][f] + 1), "materialized")
        def duplicate(doc, blobs, trace):
            trace.insert(2, copy.deepcopy(trace[1]))
            resequence(trace)
        self.reject(duplicate, "one successful public materialized")

    def test_sync_requires_matching_successful_exit_and_original_api_binding(self):
        self.reject(lambda d, b, t: t[3].__setitem__("outcome", 1), "sync_exit")
        for index in (2, 3):
            for field in reader.BINDING_KEYS:
                self.reject(lambda d, b, t, i=index, f=field:
                            t[i].__setitem__(f, t[i][f] + 1), "sync_exit")
        self.reject(lambda d, b, t: t[3].__setitem__("operation", 999), "API binding")
        def duplicate_exit(doc, blobs, trace):
            trace.insert(4, copy.deepcopy(trace[3]))
            resequence(trace)
        self.reject(duplicate_exit, "API binding")

    def test_successful_sync_cannot_precede_materialized(self):
        def swap(doc, blobs, trace):
            trace.insert(3, trace.pop(1))
            resequence(trace)
        self.reject(swap, "precedes public materialization")

    def test_binding_end_before_materialized_or_sync_is_rejected(self):
        for insertion in (1, 3):
            def move_end(doc, blobs, trace, at=insertion):
                trace.insert(at, trace.pop(6))
                resequence(trace)
            with self.subTest(insertion=insertion):
                self.reject(move_end, "crosses binding_end")
        def wrong_identity_end(doc, blobs, trace):
            ended = trace.pop(6)
            ended["token"] += 1000
            trace.insert(3, ended)
            resequence(trace)
        self.reject(wrong_identity_end, "crosses binding_end")

    def test_context_reset_or_surface_rebinding_cannot_bridge_observations(self):
        def reset(doc, blobs, trace):
            trace.insert(2, trace_row(0, "context_reset", {"context": 7, "generation": 3}))
            resequence(trace)
        self.reject(reset, "reset before retirement")
        def rebind(doc, blobs, trace):
            binding = dict(doc["slots"][0]["binding"], token=900000,
                           decode_identity=900, submission_ordinal=900)
            trace.insert(2, trace_row(0, "submit_bind", binding))
            resequence(trace)
        self.reject(rebind, "crosses binding_end")

    def test_export_binding_swaps_are_rejected_without_claiming_gpu_completion(self):
        self.reject(lambda d, b, t: t[4].__setitem__("owner", 999), "API binding")
        def late_export(doc, blobs, trace):
            enter, leave = trace.pop(4), trace.pop(4)
            trace.insert(5, enter)
            trace.insert(6, leave)
            resequence(trace)
        self.reject(late_export, "crosses binding_end")

    def test_sequence_gaps_duplicates_reordering_and_timestamp_regression_fail(self):
        self.reject(lambda d, b, t: t[2].__setitem__("seq", 9), "sequence")
        self.reject(lambda d, b, t: t[2].__setitem__("seq", 2), "sequence")
        self.reject(lambda d, b, t: t.__setitem__(slice(0, 2), t[1::-1]), "sequence")
        self.reject(lambda d, b, t: t[2].__setitem__("ts_monotonic_ns", 1), "regressed")

    def test_wrong_pid_instance_session_schema_and_bool_identity_fail(self):
        self.reject(lambda d, b, t: d.__setitem__("pid", 999), "no trace")
        self.reject(lambda d, b, t: d.__setitem__("driver_instance", 999), "no trace")
        self.reject(lambda d, b, t: t[0].__setitem__("session", 999), "session/instance")
        self.reject(lambda d, b, t: t[0].__setitem__("schema", "old"), "schema")
        self.reject(lambda d, b, t: d["slots"][0]["binding"].__setitem__("token", True), "integer")

    def test_other_driver_trace_is_separate_and_does_not_fill_missing_evidence(self):
        doc, blobs, trace = self.fresh()
        foreign = copy.deepcopy(trace[0])
        foreign.update(pid=999, driver_instance=999, session=999)
        trace.insert(0, foreign)
        self.assertTrue(reader.check_document(doc, blobs, trace)["status"].startswith("PASS_"))
        # A matching-looking materialization from another process cannot join.
        trace[2]["pid"] = 999
        with self.assertRaises(ValueError):
            reader.check_document(doc, blobs, trace)

    def test_trace_parser_rejects_duplicate_keys_unterminated_and_malformed_rows(self):
        row = json.dumps(self.base_trace[0])
        with self.assertRaisesRegex(ValueError, "unterminated"):
            reader.parse_trace(row.encode())
        duplicate = row[:-1] + ', "seq": 1}\n'
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            reader.parse_trace(duplicate.encode())
        with self.assertRaises(ValueError):
            reader.parse_trace((reader.TRACE_SCHEMA + " bad\n").encode())
        self.assertEqual(reader.parse_trace(b"ordinary diagnostic line\n"), [])

    def write_fixture(self, directory):
        doc, blobs, trace = self.fresh()
        directory = Path(directory)
        manifest = directory / "manifest.json"
        trace_path = directory / "trace.log"
        manifest.write_text(json.dumps(doc), encoding="utf-8")
        trace_path.write_text("diagnostic preface\n" + "".join(json.dumps(row) + "\n" for row in trace),
                              encoding="utf-8")
        for name, data in blobs.items():
            (directory / name).write_bytes(data)
        return manifest, trace_path, doc

    def test_audit_reads_only_explicit_regular_bounded_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, trace, doc = self.write_fixture(directory)
            report = reader.audit(manifest, trace)
            self.assertTrue(report["status"].startswith("PASS_"))
            (Path(directory) / doc["slots"][0]["small_file"]).unlink()
            self.assertEqual(reader.audit(manifest, trace)["status"], "FAIL")

    def test_audit_rejects_payload_symlinks_and_oversized_regular_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, trace, doc = self.write_fixture(directory)
            payload = Path(directory) / doc["slots"][0]["small_file"]
            original = payload.with_suffix(".original")
            payload.rename(original)
            payload.symlink_to(original.name)
            self.assertEqual(reader.audit(manifest, trace)["status"], "FAIL")
            payload.unlink()
            payload.write_bytes(b"x" * (W * H * 3 // 2 + 1))
            result = reader.audit(manifest, trace)
            self.assertEqual(result["status"], "FAIL")
            self.assertIn("bound", result["error"])

    def test_read_regular_detects_changed_file_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "one"
            path.write_bytes(b"one")
            real_fstat = reader.os.fstat
            calls = []
            def changed(fd):
                value = real_fstat(fd)
                calls.append(value)
                if len(calls) == 2:
                    return mock.Mock(st_size=value.st_size, st_mtime_ns=value.st_mtime_ns + 1,
                                     st_ctime_ns=value.st_ctime_ns)
                return value
            with mock.patch.object(reader.os, "fstat", side_effect=changed):
                with self.assertRaisesRegex(ValueError, "changed while reading"):
                    reader.read_regular(path, 10)

    def test_manifest_duplicate_keys_and_oversized_slot_lists_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, trace, doc = self.write_fixture(directory)
            manifest.write_text('{"slots": [], "slots": []}', encoding="utf-8")
            self.assertIn("duplicate JSON key", reader.audit(manifest, trace)["error"])
            doc["slots"].append(copy.deepcopy(doc["slots"][0]))
            manifest.write_text(json.dumps(doc), encoding="utf-8")
            self.assertIn("quota", reader.audit(manifest, trace)["error"])

    def test_cli_new_results_are_exclusive_and_both_manifest_forms_work(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, trace, doc = self.write_fixture(directory)
            result_path = Path(directory) / "result.json"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(reader.main([str(manifest), "--trace", str(trace),
                                              "--results", str(result_path)]), 0)
            original = result_path.read_bytes()
            self.assertTrue(json.loads(original)["status"].startswith("PASS_"))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(reader.main(["--manifest", str(manifest), "--trace", str(trace)]), 0)
                self.assertEqual(reader.main([str(manifest), "--trace", str(trace),
                                              "--results", str(result_path)]), 1)
            self.assertEqual(result_path.read_bytes(), original)


class ResetPublicationControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base_doc, cls.base_blobs, cls.base_trace = reset_fixture()

    def fresh(self):
        return copy.deepcopy(self.base_doc), dict(self.base_blobs), copy.deepcopy(self.base_trace)

    def reject(self, mutation, message=None):
        doc, blobs, trace = self.fresh()
        mutation(doc, blobs, trace)
        with self.assertRaisesRegex(ValueError, message or "."):
            reader.check_document(doc, blobs, trace)

    def test_reset_joins_old_payload_to_new_generation_without_context_destroy(self):
        doc, blobs, trace = self.fresh()
        self.assertFalse(any(row["event"] == "context_destroy" for row in trace))
        result = reader.check_document(doc, blobs, trace)
        self.assertEqual(result["status"], "PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD")
        self.assertEqual(result["schema"], reader.RESET_SCHEMA)
        self.assertEqual(result["publication"]["generation"], 3)
        self.assertEqual(result["publication"]["next_generation"], 4)
        self.assertIn("private decoder close receipt", result["scope"])
        for observation in result["observations"]:
            self.assertEqual(observation["binding"]["generation"], 3)
        for field in ("actual_media_pts", "gpu_reader_return", "displayed_pixels"):
            self.assertEqual(result[field], "NOT_OBSERVED")

    def test_later_generation_can_submit_materialize_retire_and_reset_in_same_prefix(self):
        doc, blobs, trace = self.fresh()
        newer = dict(doc["slots"][0]["binding"], generation=4, decode_identity=201)
        # Reusing a synthetic token/public surface in a NEW lifetime does not
        # make it the old source, nor make its observations qualify old bytes.
        for event, operation in (("submit_bind", 0), ("materialized", 0),
                                  ("sync_enter", 100), ("sync_exit", 100),
                                  ("binding_end", 0)):
            trace.append(trace_row(len(trace) + 1, event, newer, operation))
        trace.append(trace_row(len(trace) + 1, "context_reset", {"context": 7, "generation": 5}))
        trace.append(trace_row(len(trace) + 1, "context_destroy", {"context": 7, "generation": 5}))
        result = reader.check_document(doc, blobs, trace)
        self.assertTrue(result["status"].startswith("PASS_"))
        self.assertEqual(result["observations"][0]["successful_syncs"], 1)

    def test_reset_cannot_be_qualified_by_default_trace_outcome_without_private_close(self):
        for field in ("close_status", "device_closed"):
            with self.subTest(field=field):
                self.reject(lambda d, b, t, f=field: d["publication"].pop(f), "private decoder close")
        for field, value in (("close_status", 1), ("close_status", False),
                             ("device_closed", False), ("device_closed", 1),
                             ("status", 1), ("status", False)):
            with self.subTest(field=field, value=value):
                self.reject(lambda d, b, t, f=field, v=value: d["publication"].__setitem__(f, v))

    def test_reset_generation_receipt_requires_exact_old_plus_one(self):
        for value in (3, 5, 0, True, 2 ** 64):
            with self.subTest(value=value):
                self.reject(lambda d, b, t, v=value: d["publication"].__setitem__("next_generation", v),
                            "next_generation")
        self.reject(lambda d, b, t: d["publication"].pop("next_generation"), "next_generation")
        self.reject(lambda d, b, t: d["publication"].__setitem__("generation", 4), "next_generation")

    def test_reset_schemas_and_publication_kinds_cannot_be_interchanged(self):
        self.reject(lambda d, b, t: d.__setitem__("schema", reader.SCHEMA), "retirement")
        self.reject(lambda d, b, t: d["publication"].__setitem__("kind", "selected-decoder-context-retirement"),
                    "generation reset")
        self.reject(lambda d, b, t: d.__setitem__("schema", "crystalhd-centered-payload-v3"), "schema")

    def test_raw_reset_must_name_exact_context_and_new_generation(self):
        for field, value in (("context", 8), ("generation", 3), ("generation", 5), ("outcome", 1)):
            with self.subTest(field=field, value=value):
                self.reject(lambda d, b, t, f=field, v=value: t[-2].__setitem__(f, v), "raw context_reset")
        for field in reader.BINDING_KEYS[2:] + ("operation",):
            self.reject(lambda d, b, t, f=field: t[-2].__setitem__(f, 1), "non-context")

    def test_missing_or_duplicated_raw_reset_cannot_qualify(self):
        def missing(doc, blobs, trace):
            trace.pop(-2)
            resequence(trace)
        self.reject(missing, "raw context_reset")
        def duplicate(doc, blobs, trace):
            trace.insert(-1, copy.deepcopy(trace[-2]))
            resequence(trace)
        self.reject(duplicate, "raw context_reset")

    def test_reset_cannot_precede_selected_sync_or_submission(self):
        for position in (0, 2):
            def early(doc, blobs, trace, at=position):
                trace.insert(at, trace.pop(-2))
                resequence(trace)
            with self.subTest(position=position):
                self.reject(early, "boundary|crosses binding_end")

    def test_earlier_reset_cannot_end_or_restart_selected_old_lifetime(self):
        for generation in (3, 5):
            def reset_again(doc, blobs, trace, gen=generation):
                trace.insert(2, trace_row(0, "context_reset", {"context": 7, "generation": gen}))
                resequence(trace)
            with self.subTest(generation=generation):
                self.reject(reset_again, "reset before retirement/reset publication")

    def test_previous_reset_starting_old_generation_is_allowed_before_its_submits(self):
        doc, blobs, trace = self.fresh()
        trace.insert(0, trace_row(0, "context_reset", {"context": 7, "generation": 3}))
        resequence(trace)
        self.assertTrue(reader.check_document(doc, blobs, trace)["status"].startswith("PASS_"))

    def test_retirement_v1_after_previous_generation_reset_still_qualifies(self):
        doc, blobs, trace = fixture()
        trace.insert(0, trace_row(0, "context_reset", {"context": 7, "generation": 3}))
        resequence(trace)
        result = reader.check_document(doc, blobs, trace)
        self.assertEqual(result["schema"], reader.SCHEMA)
        self.assertEqual(result["publication"]["kind"], "selected-decoder-context-retirement")

    def test_retirement_v1_cannot_cross_a_reset_naming_new_generation(self):
        doc, blobs, trace = fixture()
        trace.insert(-2, trace_row(0, "context_reset", {"context": 7, "generation": 4}))
        resequence(trace)
        with self.assertRaisesRegex(ValueError, "reset before retirement"):
            reader.check_document(doc, blobs, trace)

    def test_destroyed_old_context_cannot_be_published_as_reset(self):
        def destroy_old(doc, blobs, trace):
            trace.insert(-2, trace_row(0, "context_destroy", {"context": 7, "generation": 3}))
            resequence(trace)
        self.reject(destroy_old, "already destroyed")

    def test_reset_still_requires_unlocked_frozen_complete_selected_slots(self):
        for field in ("after_driver_unlock", "all_slots_frozen"):
            self.reject(lambda d, b, t, f=field: d["publication"].__setitem__(f, False), "unlocked")
        for field in reader.STAGE_FLAGS:
            self.reject(lambda d, b, t, f=field: d["slots"][0].__setitem__(f, False), field)
        self.reject(lambda d, b, t: d["slots"].pop(), "two selected")

    def test_old_bindings_cannot_borrow_materialization_from_new_generation(self):
        for field in reader.BINDING_KEYS:
            self.reject(lambda d, b, t, f=field: t[1].__setitem__(f, t[1][f] + 1), "materialized")
        self.reject(lambda d, b, t: d["slots"][0]["binding"].__setitem__("generation", 4), "lifetime")

    def test_binding_end_before_selected_sync_remains_a_limit_for_reset_publication(self):
        def ended(doc, blobs, trace):
            trace.insert(3, trace.pop(6))
            resequence(trace)
        self.reject(ended, "crosses binding_end")

    def test_old_binding_sync_after_reset_is_rejected_despite_new_generation_progress(self):
        def late(doc, blobs, trace):
            for event in ("sync_enter", "sync_exit"):
                trace.append(trace_row(len(trace) + 1, event, doc["slots"][0]["binding"], 100))
        self.reject(late, "crosses binding_end")

    def test_frozen_v2_artifacts_are_accepted_by_public_audit_api(self):
        doc, blobs, trace = self.fresh()
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            manifest, trace_path = directory / "reset.json", directory / "trace.log"
            manifest.write_text(json.dumps(doc), encoding="utf-8")
            trace_path.write_text("".join(json.dumps(row) + "\n" for row in trace), encoding="utf-8")
            for name, payload in blobs.items():
                (directory / name).write_bytes(payload)
            result = reader.audit(manifest, trace_path)
            self.assertEqual(result["schema"], reader.RESET_SCHEMA)
            self.assertTrue(result["status"].startswith("PASS_"))


if __name__ == "__main__":
    unittest.main()
