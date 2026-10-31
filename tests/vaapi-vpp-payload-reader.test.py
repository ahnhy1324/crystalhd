#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Pure CPU controls for the offline reader; no VA/DRM/media/device access."""

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


_path = Path(__file__).with_name("vaapi-vpp-payload-reader.py")
_spec = importlib.util.spec_from_file_location("vpp_payload_reader", _path)
reader = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(reader)
W, H = 1280, 720


def full_source(stream, parent):
    # Separate Y/U/V signatures make parity swaps and UV-from-Y mistakes
    # visible. These hand-built source rows do not call the reader's model.
    y = [bytes((row // 256, row % 256, 17 + stream, 31 + parent)) * (W // 4)
         for row in range(H)]
    uv = [bytes(((row * 3 + parent * 11 + stream) % 251,
                 (row * 7 + parent * 19 + stream * 5 + 41) % 251)) * (W // 2)
          for row in range(H // 2)]
    return y, uv


def expanded_field(planes, parity):
    # At equal canvas size Bob means repeat every selected source row twice.
    # Build an explicit row list instead of reproducing sampling arithmetic.
    result = []
    for plane in planes:
        for row in plane[parity::2]:
            result.extend((row, row))
    return b"".join(result)


def compact_field(planes, parity):
    return b"".join(row for plane in planes for row in plane[parity::2])


def geom(serial, height=H, epoch=1):
    base = serial * 0x1000000
    return {"surface_serial": serial, "storage_epoch": epoch,
            "width": W, "height": height, "pitch": [W + 64, W + 128],
            "plane_address": [base, base + (W + 64) * height], "fourcc": "NV12"}


def trace_row(seq, event, binding=None, operation=0, outcome=0):
    row = {"schema": reader.TRACE_SCHEMA, "pid": 123, "session": 2,
           "driver_instance": 2, "seq": seq, "ts_monotonic_ns": seq * 100,
           "event": event, "operation": operation, "outcome": outcome}
    row.update({key: 0 for key in reader.BINDING_KEYS})
    if binding:
        row.update(binding)
    return row


def fixture():
    doc = {"schema": reader.SCHEMA, "pid": 123, "driver_instance": 2,
           "quota": 8, "slots": []}
    blobs, trace = {}, []
    for stream in range(2):
        for parent in range(2):
            original = {"context": 7 + stream, "generation": 1,
                        "token": 100000 * (parent + 1),
                        "decode_identity": 10 + stream * 2 + parent,
                        "submission_ordinal": parent + 1,
                        "surface": 20 + stream * 2 + parent,
                        "owner": 20 + stream * 2 + parent}
            trace.append(trace_row(len(trace) + 1, "submit_bind", original))
            planes = full_source(stream, parent)
            canonical = geom(100 + stream * 2 + parent)
            for parity in range(2):
                number = stream * 4 + parent * 2 + parity
                operation = 500 + number
                # Exercise an actual compact TFF first field and compact BFF
                # first field, and two pairs using completed full-frame input.
                compact = parent == 0 and parity == stream
                source = geom(300 + number, H // 2) if compact else copy.deepcopy(canonical)
                target = geom(200 + number)
                binding = dict(original, surface=40 + number, owner=40 + number)
                source_name, target_name = "source-%d.nv12" % number, "target-%d.nv12" % number
                source_bytes = compact_field(planes, parity) if compact else b"".join(planes[0] + planes[1])
                target_bytes = expanded_field(planes, parity)
                blobs[source_name], blobs[target_name] = source_bytes, target_bytes
                first = {"valid": compact}
                if compact:
                    first.update({"bottom": bool(parity), "bottom_first": bool(parity),
                                  "generation": original["generation"],
                                  "decode_identity": original["decode_identity"],
                                  "token": original["token"], "picture_number": parent + 1,
                                  "sess_num": stream + 1, "picture_meta_payload": 0})
                slot = {"stream_index": stream, "parent_index": parent,
                        "operation": operation, "binding": binding,
                        "source_surface": original["surface"], "source_owner": original["owner"],
                        "requested_parity": parity, "source_is_field": compact,
                        "source_crop": [0, 0, W, H], "target_crop": [0, 0, W, H],
                        "source": source, "target": target,
                        "source_canonical": copy.deepcopy(canonical),
                        "target_canonical": copy.deepcopy(target), "compact_first_field": first,
                        "source_file": source_name, "target_file": target_name,
                        "source_bytes": len(source_bytes), "target_bytes": len(target_bytes),
                        "copy_ok": True, "process_success": True, "committed": True,
                        "current": True, "status": 0}
                doc["slots"].append(slot)
                trace.append(trace_row(len(trace) + 1, "vpp_commit", binding, operation))
    trace.append(trace_row(len(trace) + 1, "session_destroy"))
    return doc, blobs, trace


class PayloadReaderControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base_doc, cls.base_blobs, cls.base_trace = fixture()

    def fresh(self):
        return copy.deepcopy(self.base_doc), dict(self.base_blobs), copy.deepcopy(self.base_trace)

    def reject(self, mutate, message=None):
        doc, blobs, trace = self.fresh()
        mutate(doc, blobs, trace)
        if message:
            with self.assertRaisesRegex(ValueError, message):
                reader.check_document(doc, blobs, trace)
        else:
            with self.assertRaises(ValueError):
                reader.check_document(doc, blobs, trace)

    def test_full_and_compact_tff_bff_match_every_active_component(self):
        result = reader.check_document(*self.fresh())
        self.assertEqual(result["status"], "PASS_SCOPED_OWN_COMPLETED_VPP_PAYLOAD")
        self.assertEqual(result["compared_target_bytes"], 8 * W * H * 3 // 2)
        for key in ("source_pts", "chronological_field_presentation",
                    "gpu_import_fence_lifetime", "displayed_pixels"):
            self.assertEqual(result[key], "NOT_RUN")

    def test_individual_y_u_v_corruption_is_rejected(self):
        for offset, component in ((17, "Y"), (W * H + 20, "U"), (W * H + 21, "V")):
            def corrupt(doc, blobs, trace, offset=offset):
                name = doc["slots"][0]["target_file"]
                data = bytearray(blobs[name])
                data[offset] ^= 1
                blobs[name] = bytes(data)
            with self.subTest(component=component):
                self.reject(corrupt, component + " mismatch")

    def test_wrong_physical_parity_is_rejected(self):
        def mutate(doc, blobs, trace):
            slot = doc["slots"][2]  # Completed full source, no compact metadata.
            slot["requested_parity"] ^= 1
        self.reject(mutate, "mismatch")

    def test_deriving_uv_from_selected_y_rows_is_rejected(self):
        def mutate(doc, blobs, trace):
            slot = doc["slots"][2]
            planes = full_source(slot["stream_index"], slot["parent_index"])
            valid = blobs[slot["target_file"]][:W * H]
            # This wrong model repeats each chroma row in groups of four by
            # deriving UV from the duplicated Y row and dividing once more.
            wrong_uv = b"".join(planes[1][row // 4] for row in range(H // 2))
            blobs[slot["target_file"]] = valid + wrong_uv
        self.reject(mutate, "[UV] mismatch")

    def test_compact_payload_must_match_full_parent(self):
        def mutate(doc, blobs, trace):
            slot = doc["slots"][0]
            name = slot["source_file"]
            data = bytearray(blobs[name])
            data[13] ^= 1
            blobs[name] = bytes(data)
            # Adjust target too: a self-consistent but falsely attributed
            # compact source must still fail the independent sibling join.
            target = bytearray(blobs[slot["target_file"]])
            target[13] ^= 1
            target[W + 13] ^= 1
            blobs[slot["target_file"]] = bytes(target)
        self.reject(mutate, "compact source disagrees")

    def test_full_parent_source_is_immutable(self):
        def mutate(doc, blobs, trace):
            slot = doc["slots"][2]
            source = bytearray(blobs[slot["source_file"]])
            # Mutate the unselected opposite-parity row, keeping its target
            # valid. The sibling full source must detect this alteration.
            source[W + 1] ^= 1
            blobs[slot["source_file"]] = bytes(source)
        self.reject(mutate, "different full source bytes")

    def test_cancel_supersession_and_process_sync_failures_reject(self):
        for field in ("copy_ok", "process_success", "committed", "current"):
            with self.subTest(field=field):
                self.reject(lambda doc, blobs, trace, field=field:
                            doc["slots"][0].__setitem__(field, False), field)
        self.reject(lambda doc, blobs, trace: doc["slots"][0].__setitem__("status", -1), "status")

    def test_duplicate_operation_or_parent_parity_rejects(self):
        self.reject(lambda doc, blobs, trace:
                    doc["slots"][1].__setitem__("operation", doc["slots"][0]["operation"]),
                    "duplicate VPP operation")
        self.reject(lambda doc, blobs, trace:
                    doc["slots"][1].__setitem__("requested_parity", 0),
                    "duplicate parent/parity")

    def test_short_long_or_wrong_declared_payloads_reject(self):
        for suffix in (-1, 1):
            def mutate(doc, blobs, trace, suffix=suffix):
                name = doc["slots"][0]["target_file"]
                blobs[name] = blobs[name][:-1] if suffix < 0 else blobs[name] + b"\0"
            with self.subTest(suffix=suffix):
                self.reject(mutate, "length")
        self.reject(lambda doc, blobs, trace:
                    doc["slots"][0].__setitem__("source_bytes", 1), "length")

    def test_unsupported_pitch_crop_fourcc_height_and_null_allocation_reject(self):
        mutations = [
            lambda slot: slot["source"]["pitch"].__setitem__(0, W - 1),
            lambda slot: slot.__setitem__("source_crop", [2, 0, W - 2, H]),
            lambda slot: slot.__setitem__("target_crop", [0, 2, W, H - 2]),
            lambda slot: slot["source"].__setitem__("fourcc", "BGRA"),
            lambda slot: slot["source"].__setitem__("height", H),
            lambda slot: slot["source_canonical"]["plane_address"].__setitem__(0, 0),
            lambda slot: slot["source_canonical"].__setitem__("storage_epoch", 0),
            lambda slot: slot["source"]["plane_address"].__setitem__(0, 2 ** 64 - 1),
            lambda slot: slot["source"]["plane_address"].__setitem__(1, slot["source"]["plane_address"][0]),
            lambda slot: slot.__setitem__("source_crop", [False, 0, W, H]),
        ]
        for number, mutation in enumerate(mutations):
            with self.subTest(mutation=number):
                self.reject(lambda doc, blobs, trace, mutation=mutation: mutation(doc["slots"][0]))

    def test_changed_storage_epoch_or_target_view_rejects(self):
        self.reject(lambda doc, blobs, trace:
                    doc["slots"][2]["source"].__setitem__("storage_epoch", 2), "epoch/view")
        self.reject(lambda doc, blobs, trace:
                    doc["slots"][0]["target_canonical"].__setitem__("storage_epoch", 2), "epoch/view")

    def test_in_place_target_cannot_certify_immutable_source(self):
        def mutate(doc, blobs, trace):
            slot = doc["slots"][2]
            slot["target"] = copy.deepcopy(slot["source_canonical"])
            slot["target_canonical"] = copy.deepcopy(slot["source_canonical"])
        self.reject(mutate, "in-place")

    def test_compact_metadata_cannot_relabel_hardware_field(self):
        for key, value in (("valid", False), ("bottom", True), ("bottom_first", True),
                           ("generation", 2), ("decode_identity", 999), ("token", 999)):
            with self.subTest(key=key):
                self.reject(lambda doc, blobs, trace, key=key, value=value:
                            doc["slots"][0]["compact_first_field"].__setitem__(key, value))

    def test_compact_cannot_be_reinterpreted_as_full_source(self):
        self.reject(lambda doc, blobs, trace:
                    doc["slots"][0].__setitem__("source_is_field", False), "physical 1280x720")

    def test_source_and_target_binding_changes_reject(self):
        for key in reader.BINDING_KEYS:
            with self.subTest(key=key):
                self.reject(lambda doc, blobs, trace, key=key:
                            doc["slots"][0]["binding"].__setitem__(key, 999))
        self.reject(lambda doc, blobs, trace:
                    doc["slots"][0].__setitem__("source_owner", 999), "submit_bind")

    def test_raw_commit_duplicate_cancel_or_nonzero_outcome_rejects(self):
        def cancel(doc, blobs, trace):
            trace[1]["event"] = "vpp_cancel"
        self.reject(cancel, "one commit")
        self.reject(lambda doc, blobs, trace: trace[1].__setitem__("outcome", -1), "binding mismatch")
        def duplicate(doc, blobs, trace):
            trace.insert(2, copy.deepcopy(trace[1]))
            for index, row in enumerate(trace, 1):
                row["seq"], row["ts_monotonic_ns"] = index, index * 100
        self.reject(duplicate, "one commit")

    def test_trace_sequence_session_pid_timestamp_and_truncation_reject(self):
        self.reject(lambda doc, blobs, trace: trace[1].__setitem__("seq", 99), "sequence")
        self.reject(lambda doc, blobs, trace: trace[1].__setitem__("session", 3), "session")
        self.reject(lambda doc, blobs, trace: doc.__setitem__("pid", 999), "no trace")
        self.reject(lambda doc, blobs, trace: trace[1].__setitem__("ts_monotonic_ns", 1), "regressed")
        self.reject(lambda doc, blobs, trace: trace.pop(), "session_destroy")

    def test_quota_missing_slot_boolean_integer_and_filename_controls(self):
        self.reject(lambda doc, blobs, trace: doc.__setitem__("quota", 9), "quota")
        self.reject(lambda doc, blobs, trace: doc["slots"].pop(), "eight")
        self.reject(lambda doc, blobs, trace: doc["slots"][0].__setitem__("operation", True), "integer")
        self.reject(lambda doc, blobs, trace: doc["slots"][0].__setitem__("source_file", "../outside"), "relative filename")
        self.reject(lambda doc, blobs, trace:
                    doc["slots"][1].__setitem__("target_file", doc["slots"][0]["target_file"]), "unique")

    def test_reused_target_storage_view_is_supported(self):
        doc, blobs, trace = self.fresh()
        # Operation-specific byte copies remain separate even if the client
        # reuses one physical target allocation between the operations.
        target = copy.deepcopy(doc["slots"][0]["target"])
        for slot in doc["slots"]:
            slot["target"] = copy.deepcopy(target)
            slot["target_canonical"] = copy.deepcopy(target)
        self.assertEqual(reader.check_document(doc, blobs, trace)["slots"], 8)

    def test_raw_log_parser_ignores_noise_but_rejects_malformed_own_json(self):
        raw = b"ordinary frontend log\n" + b"\n".join(
            json.dumps(row).encode() for row in self.base_trace) + b"\n"
        self.assertEqual(reader.parse_trace(raw), self.base_trace)
        with self.assertRaises(ValueError):
            reader.parse_trace(b'{"schema":"crystalhd-vaapi-trace-v4",broken}\n')
        with self.assertRaises(ValueError):
            reader.parse_trace(b'{"schema":"crystalhd-vaapi-trace-v4","seq":1,"seq":2}\n')

    def test_link_and_nonregular_input_rejected_before_read(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = root / "payload"
            payload.write_bytes(b"abc")
            link = root / "link"
            link.symlink_to(payload)
            self.assertEqual(reader.read_regular(payload, 3), b"abc")
            with self.assertRaises(ValueError):
                reader.read_regular(link, 3)
            with self.assertRaises(ValueError):
                reader.read_regular(root, 3)
            with self.assertRaises(ValueError):
                reader.read_regular(payload, 2)


if __name__ == "__main__":
    unittest.main()
