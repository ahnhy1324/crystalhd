#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Root-run offline natural MPEG1 source/order/repeat/fixed-model audit.
No subprocess, decoder, encoder, foreign converter or device access.
"""
import argparse
from collections import Counter
from fractions import Fraction
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import numpy as np

sys.dont_write_bytecode = True

W, H, N = 352, 288, 1250
PACKED, PLANAR = 202752, 152064
ORDERS = ("H8_then_V8", "V8_then_H8")
AXES = ("interior", "x", "y", "xy")
SMALL_LIMIT = 64 * 1024 * 1024


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def identity(path):
    path = Path(path)
    raw = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def load(path, name, pin=None):
    if pin is not None:
        require(identity(path) == pin, "owned helper identity differs")
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class Metric:
    def __init__(self, shape, names):
        self.shape = shape
        self.absolute = np.zeros(256, dtype=np.int64)
        self.signed = np.zeros(511, dtype=np.int64)
        self.maximum_map = np.zeros(shape, dtype=np.uint8)
        self.row_sum, self.column_sum = np.zeros(shape[0], dtype=np.int64), np.zeros(shape[1], dtype=np.int64)
        self.maximum, self.examples = 0, []
        self.groups = [{"name": name, "samples": 0, "changed": 0, "absolute_sum": 0, "signed_sum": 0, "maximum_absolute_error": 0, "maximum_examples_up_to4": []} for name in names]

    def add(self, native, reference, labels, ordinal):
        require(native.dtype == reference.dtype == np.uint8 and native.shape == reference.shape == self.shape, "metric extent/dtype differs")
        require(labels.shape == self.shape and labels.dtype.kind in "ui" and int(labels.min()) >= 0 and int(labels.max()) < len(self.groups), "invalid metric partition labels")
        difference = native.astype(np.int16) - reference.astype(np.int16)
        absolute = np.abs(difference)
        self.absolute += np.bincount(absolute.ravel(), minlength=256)
        self.signed += np.bincount((difference + 255).ravel(), minlength=511)
        self.maximum_map = np.maximum(self.maximum_map, absolute.astype(np.uint8))
        self.row_sum += absolute.sum(axis=1, dtype=np.int64)
        self.column_sum += absolute.sum(axis=0, dtype=np.int64)
        maximum = int(absolute.max())
        if maximum > self.maximum:
            self.maximum, self.examples = maximum, []
        if maximum and maximum == self.maximum and len(self.examples) < 8:
            for y, x in np.argwhere(absolute == maximum)[:8-len(self.examples)]:
                self.examples.append({"frame_ordinal": ordinal, "x": int(x), "y": int(y), "native": int(native[y, x]), "reference": int(reference[y, x]), "signed_error": int(difference[y, x])})
        flat, count = labels.ravel(), len(self.groups)
        counts = np.bincount(flat, minlength=count)
        changes = np.bincount(labels[difference != 0], minlength=count)
        sums = np.bincount(flat, weights=absolute.ravel(), minlength=count).astype(np.int64)
        signed = np.bincount(flat, weights=difference.ravel(), minlength=count).astype(np.int64)
        maxima = np.zeros(count, dtype=np.int16)
        np.maximum.at(maxima, flat, absolute.ravel())
        local = []
        for index, total in enumerate(self.groups):
            part = {"name": total["name"], "samples": int(counts[index]), "changed": int(changes[index]), "absolute_sum": int(sums[index]), "signed_sum": int(signed[index]), "maximum_absolute_error": int(maxima[index])}
            local.append(part)
            for key in ("samples", "changed", "absolute_sum", "signed_sum"):
                total[key] += part[key]
            if part["maximum_absolute_error"] > total["maximum_absolute_error"]:
                total["maximum_absolute_error"], total["maximum_examples_up_to4"] = part["maximum_absolute_error"], []
            if part["maximum_absolute_error"] and part["maximum_absolute_error"] == total["maximum_absolute_error"] and len(total["maximum_examples_up_to4"]) < 4:
                for y, x in np.argwhere((labels == index) & (absolute == part["maximum_absolute_error"]))[:4-len(total["maximum_examples_up_to4"])]:
                    total["maximum_examples_up_to4"].append({"frame_ordinal": ordinal, "x": int(x), "y": int(y), "signed_error": int(difference[y, x])})
        require(sum(part["samples"] for part in local) == native.size, "partition does not cover every component sample")
        return {"samples": int(native.size), "exact_samples": int(native.size - np.count_nonzero(difference)), "changed": int(np.count_nonzero(difference)), "maximum_absolute_error": maximum, "absolute_sum": int(absolute.sum(dtype=np.int64)), "signed_sum": int(difference.sum(dtype=np.int64)), "groups": local}

    def result(self):
        samples = int(self.absolute.sum())
        require(samples > 0 and int(self.signed.sum()) == samples, "invalid metric census")
        cumulative = np.cumsum(self.absolute)
        percentiles = {str(p): int(np.searchsorted(cumulative, (samples*p+99)//100)) for p in (50, 95, 99)}
        absolute_sum = int(np.dot(self.absolute, np.arange(256, dtype=np.int64)))
        signed_sum = int(np.dot(self.signed, np.arange(-255, 256, dtype=np.int64)))
        exact = int(self.absolute[0])
        return {"samples": samples, "exact_samples": exact, "changed_samples": samples-exact, "exact": exact == samples, "changed_fraction": (samples-exact)/samples,
                "maximum_absolute_error": self.maximum, "mean_absolute_error": absolute_sum/samples, "absolute_error_sum": absolute_sum, "signed_error_sum": signed_sum,
                "absolute_error_histogram_256": self.absolute.tolist(), "signed_error_histogram_minus255_to255": self.signed.tolist(), "absolute_error_percentiles_nearest_rank": percentiles,
                "maximum_examples_up_to8": self.examples, "groups": self.groups}

    def maps(self):
        return {"grid_width_height": [self.shape[1], self.shape[0]], "maximum_absolute_error_by_xy": self.maximum_map.tolist(), "absolute_error_sum_by_row": self.row_sum.tolist(), "absolute_error_sum_by_column": self.column_sum.tolist()}


def metric_controls():
    names = []
    def check(name, value):
        require(value, "metric control failed: " + name)
        names.append(name)
    native = np.array([[0, 2], [255, 10]], dtype=np.uint8)
    reference = np.array([[0, 0], [0, 13]], dtype=np.uint8)
    labels = np.array([[0, 0], [1, 1]], dtype=np.uint8)
    metric = Metric((2, 2), ("even", "odd")); metric.add(native, reference, labels, 0)
    result = metric.result()
    check("literal_histogram_256", [(i, n) for i, n in enumerate(result["absolute_error_histogram_256"]) if n] == [(0, 1), (2, 1), (3, 1), (255, 1)])
    check("literal_counts_signed_mean_percentiles", result["exact_samples"] == 1 and result["changed_samples"] == 3 and result["absolute_error_sum"] == 260 and result["signed_error_sum"] == 254 and result["mean_absolute_error"] == 65 and result["absolute_error_percentiles_nearest_rank"] == {"50": 2, "95": 255, "99": 255})
    scalar = [abs(int(a)-int(b)) for a, b in zip(native.ravel().tolist(), reference.ravel().tolist())]
    check("independent_scalar_metrics", sum(scalar) == result["absolute_error_sum"] and max(scalar) == result["maximum_absolute_error"] and sum(value == 0 for value in scalar) == result["exact_samples"])
    check("literal_partition", result["groups"][0]["maximum_absolute_error"] == 2 and result["groups"][1]["absolute_sum"] == 258 and result["groups"][1]["signed_sum"] == 252)
    check("literal_error_coordinate", result["maximum_examples_up_to8"] == [{"frame_ordinal": 0, "x": 0, "y": 1, "native": 255, "reference": 0, "signed_error": 255}])
    metric.add(native, reference, labels, 1)
    check("literal_stream_aggregation_and_maps", metric.result()["samples"] == 8 and metric.result()["absolute_error_sum"] == 520 and metric.maps()["absolute_error_sum_by_row"] == [4, 516] and metric.maps()["absolute_error_sum_by_column"] == [510, 10])
    exact = Metric((2, 2), ("even", "odd")); exact.add(native, native, labels, 0)
    check("identity", exact.result()["exact"] and exact.result()["maximum_absolute_error"] == 0)
    for name, a, b, codes in (("reject_dtype", native.astype(np.int16), reference, labels), ("reject_extent", native[:, :1], reference, labels), ("reject_partition", native, reference, np.full((2, 2), 2, dtype=np.uint8))):
        try:
            Metric((2, 2), ("even", "odd")).add(a, b, codes, 0)
        except ValueError:
            names.append(name)
        else:
            raise AssertionError("negative metric control accepted: " + name)
    return {"status": "PASS", "control_groups": len(names), "names": names, "actual_media_or_hardware": "NOT_RUN"}


def safety(receipt, session, collector):
    require(receipt.get("schema") == "crystalhd.issue606.capture-qualification.v1", "capture qualification schema differs")
    for key in ("boundary_equal", "final_pins_equal", "all_media_and_children_closed", "actual_device_owner_observed"):
        require(receipt.get(key) is True, "capture safety requirement failed: " + key)
    require(all(type(receipt.get(key)) is int and receipt[key] == 0 for key in ("capture_exit_code", "kernel_exit")) and not receipt.get("live_resource_handoff"), "capture exit/kernel/retirement failed")
    for name in ("raw", "stdout", "stderr"):
        require(receipt.get("artifacts", {}).get(name+"_sha256") == session[name]["sha256"], "qualification does not bind artifact: " + name)
    require(receipt.get("collector_source_sha256") == collector["source"]["sha256"] and receipt.get("collector_binary_sha256") == collector["binary"]["sha256"], "qualified collector identity differs")
    require(receipt.get("module_version") == "3.10.13" and receipt.get("module_srcversion") == "F76E163690F147B91495DDD", "installed module differs")
    require(receipt.get("installed_module_sha256") == "9e959ab420fe9914cbb78921535832b8a0037b4dc5be67480bf328c87b755d6a", "installed module bytes differ")
    require(receipt.get("firmware_sha256") == "8bf3a68f5c64686358a52274e40911a88c7f8c67ecbf6cf1557a49b4d7bc67c9", "firmware differs")


def audit(manifest, out, summary, proof, axis, vertical, separable):
    readers = []
    def opened(pin):
        reader = axis.FrozenInput(pin); readers.append(reader); return reader
    def small(pin, text=False):
        require(0 < pin["bytes"] <= SMALL_LIMIT, "small evidence exceeds64MiB")
        reader = opened(pin); raw = reader.frame(pin["bytes"])
        return raw.decode("utf-8", errors="strict") if text else json.loads(raw)
    def consume(pin):
        reader = opened(pin)
        while reader.stream.tell() < pin["bytes"]:
            reader.frame(min(1024*1024, pin["bytes"]-reader.stream.tell()))
        return reader
    try:
        files, collector = manifest["files"], manifest["collector"]
        source = small(collector["source"], True)
        require("generation) * (kMaximumPackets + 1)" in source and "packet + 1) * kTokenStep" in source and "static const unsigned kMaximumPackets = 10000;" in source and "static const uint64_t kTokenStep = 100000;" in source, "collector token source contract differs")
        require(type(collector.get("generation")) is int and collector["generation"] == 0, "both invocations must use generation0/one iteration")
        consume(collector["binary"])
        tables = {name: small(files[name]) for name in ("es_packets", "mkv_packets", "es_metadata", "mkv_metadata", "mkv_frames")}
        selected = {kind: {"streams": [stream for stream in tables[key]["streams"] if stream.get("codec_type") == "video"]} for kind, key in (("ES", "es_metadata"), ("MKV", "mkv_metadata"))}
        streams = {kind: proof.video_stream(value, kind) for kind, value in selected.items()}
        es_rows, mkv_rows = proof.packet_rows(tables["es_packets"], N), proof.packet_rows(tables["mkv_packets"], N)
        require(all(row["stream_index"] == streams["ES"]["index"] for row in es_rows) and all(row["stream_index"] == streams["MKV"]["index"] for row in mkv_rows), "packet table contains another stream")
        coded = proof.join_coded(es_rows, mkv_rows)
        require(files["elementary_stream"]["bytes"] <= SMALL_LIMIT, "ES exceeds collector64MiB admission")
        es = opened(files["elementary_stream"])
        for row in coded:
            payload = es.frame(row["size"])
            require(hashlib.sha256(payload).hexdigest() == row["payload_sha256"], "fresh ES AU bytes differ from independent table")
            row.update(proof.picture_header(payload))
        require(sum(row["size"] for row in coded) == files["elementary_stream"]["bytes"] and sum(row["sequence_headers"] for row in coded) > 0, "ES exact-byte coverage/sequence census failed")
        counts = {kind: sum(row["pict_type"] == kind for row in coded) for kind in ("I", "P", "B")}
        require(counts == manifest["expected_picture_type_counts"] and all(counts.values()), "independent I/P/B population differs")
        consume(files["container"])
        display = proof.join_container_display(tables["mkv_frames"]["frames"], coded, Fraction(streams["MKV"]["time_base"]))
        if "es_frames" in files:
            es_frames = small(files["es_frames"])["frames"]
            require(display == proof.join_display(es_frames, tables["mkv_frames"]["frames"], coded, Fraction(streams["MKV"]["time_base"])), "optional independent ES display table differs")
        for pin in manifest.get("additional_evidence", []):
            small(pin)
        sessions = manifest["capture_sessions"]
        require(len(sessions) == 2 and len({session["name"] for session in sessions}) == 2, "two distinct sessions required")
        output_rows = []
        for session in sessions:
            stdout, stderr = small(session["stdout"], True), small(session["stderr"], True)
            safety(small(session["qualification"]), session, collector)
            rows = proof.capture_rows(stdout, N); proof.join_output(rows, display)
            require(f"Packed capture: requested-format=YUY2 frames={N}/{N} bytes={PACKED*N}/{PACKED*N} transport-result=PASS" in stdout, "complete capture byte receipt missing")
            require(re.findall(r"^Scaler test: iteration=1/1 requested-width=0 expected-output=352x288 yuy2-sha256=([0-9a-f]{64}) result=PASS$", stdout, re.M) == [session["raw"]["sha256"]], "collector active-byte hash differs")
            output_rows.append(rows)
            picture_numbers = Counter(row["picture_number"] for row in rows)
            summary["sessions"].append({"name": session["name"], "source_output_join": "PASS", "qualification": session["qualification"], "stderr_bytes": len(stderr.encode()), "raw": session["raw"],
                "picture_number_diagnostics": {"unique_values": len(picture_numbers), "collisions": {str(value): count for value, count in picture_numbers.items() if count > 1}, "scope": "counter values retained diagnostically; wrap/reset/unique semantics are unproved and this is not a source-identity or PTS gate"}})
        require([row["token"] for row in output_rows[0]] == [row["token"] for row in output_rows[1]], "repeat output source order differs")
        result = {"schema": "crystalhd.issue606.picture-PTS-proof.v1", "streams": streams, "picture_type_counts": counts, "coded_packets_in_actual_order": coded, "display_pictures_in_actual_software_order": display, "output_records": {session["name"]: rows for session, rows in zip(sessions, output_rows)}, "token_numeric_order_is_presentationPTS": False}
        with (out/"picture-PTS-proof.json").open("x") as stream:
            json.dump(result, stream, indent=2, sort_keys=True); stream.write("\n")
        require(files["software_i420"]["bytes"] == PLANAR*N, "SW I420 population differs")
        software = opened(files["software_i420"])
        captures = []
        for session in sessions:
            require(session["raw"]["bytes"] == PACKED*N, "native YUYV population differs")
            captures.append(opened(session["raw"]))
        parity = (np.arange(H, dtype=np.int64) % 2)[:, None]
        y_labels = np.broadcast_to(parity, (H, W))
        names = tuple(axis+"/"+str(p) for axis in AXES for p in (0, 1))
        metrics = {session["name"]: {"Y": Metric((H, W), ("Y/0", "Y/1")), **{order+"/"+name: Metric((H, W//2), names) for order in ORDERS for name in ("Cb", "Cr")}} for session in sessions}
        repeat_changed, repeat_first, previous_y = 0, None, None
        outliers, transitions = [], []
        with (out/"per-frame.jsonl").open("x") as stream:
            for ordinal in range(N):
                planar_raw = software.frame(PLANAR); planar = axis.planar_planes(planar_raw, W, H)
                raw = [reader.frame(PACKED) for reader in captures]
                native = [axis.packed_planes(value, W, H) for value in raw]
                stable = raw[0] == raw[1]
                if not stable:
                    a, b = [np.frombuffer(value, dtype=np.uint8) for value in raw]
                    changed = np.flatnonzero(a != b); repeat_changed += int(changed.size)
                    if repeat_first is None:
                        offset = int(changed[0]); repeat_first = {"frame_ordinal": ordinal, "active_byte_offset": offset, "a": int(a[offset]), "b": int(b[offset])}
                row = {**display[ordinal], "software_I420_sha256": hashlib.sha256(planar_raw).hexdigest(), "repeat_all_active_bytes_equal": stable, "sessions": {session["name"]: {"raw_YUYV_sha256": hashlib.sha256(value).hexdigest(), "metrics": {}} for session, value in zip(sessions, raw)}, "reference_arithmetic": {order: {} for order in ORDERS}}
                if previous_y is not None:
                    transitions.append({"frame_ordinal": ordinal, "mean_absolute_luma_change": float(np.abs(planar["Y"].astype(np.int16)-previous_y.astype(np.int16)).mean()), "pts": display[ordinal]["pts"]})
                previous_y = planar["Y"].copy()
                for session, planes in zip(sessions, native):
                    row["sessions"][session["name"]]["metrics"]["Y"] = metrics[session["name"]]["Y"].add(planes["Y"], planar["Y"], y_labels, ordinal)
                for name in ("Cb", "Cr"):
                    labels = axis.axis_classes(planar[name])*2 + parity
                    for order, (model, arithmetic) in separable.models(planar[name], axis, vertical).items():
                        row["reference_arithmetic"][order][name] = arithmetic
                        for session, planes in zip(sessions, native):
                            label = order+"/"+name
                            values = metrics[session["name"]][label].add(planes[name], model, labels, ordinal)
                            row["sessions"][session["name"]]["metrics"][label] = values
                            outliers.append({"session": session["name"], "model_component": label, "frame_ordinal": ordinal, "pts": display[ordinal]["pts"], "pict_type": display[ordinal]["pict_type"], "maximum_absolute_error": values["maximum_absolute_error"], "absolute_sum": values["absolute_sum"]})
                stream.write(json.dumps(row, sort_keys=True)+"\n"); summary["completed_frames"] = ordinal+1
        results = {session: {name: value.result() for name, value in components.items()} for session, components in metrics.items()}
        require(all(result["samples"] == N*W*H//(1 if name == "Y" else 2) for components in results.values() for name, result in components.items()), "global component sample census differs")
        maps = {session: {name: value.maps() for name, value in components.items()} for session, components in metrics.items()}
        for filename, value in (("metrics.json", results), ("error-maps.json", maps)):
            with (out/filename).open("x") as stream:
                json.dump(value, stream, indent=2, sort_keys=True); stream.write("\n")
        summary["repeat"] = {"all_active_bytes_equal": repeat_changed == 0, "changed_active_bytes": repeat_changed, "first_difference": repeat_first, "compared_active_bytes": PACKED*N}
        summary["metric_summary"] = {session: {name: {key: result[key] for key in ("samples", "exact_samples", "maximum_absolute_error", "mean_absolute_error", "changed_fraction", "absolute_error_percentiles_nearest_rank")} for name, result in components.items()} for session, components in results.items()}
        summary["gate_evidence"] = {"source_packet_picture_native_output_PTS_join": True, "two_session_full_byte_repeat": repeat_changed == 0, "software_Y_exact_both_sessions": all(components["Y"]["exact"] for components in results.values()), "chroma_target3_all_frozen_orders_both_sessions": all(result["maximum_absolute_error"] <= 3 for components in results.values() for name, result in components.items() if name != "Y"), "normal_frontend_audio_cadence_and_log_drift": "NOT_AUDITED_HERE; separate actual playback gate", "conditional_codec_admission": "HOLD pending all issue606 visual/frontend/lifecycle gates"}
        summary["outliers_for_review_only"] = sorted(outliers, key=lambda row: (-row["maximum_absolute_error"], -row["absolute_sum"], row["frame_ordinal"], row["session"], row["model_component"]))[:32]
        summary["largest_source_luma_transitions_for_review_only"] = sorted(transitions, key=lambda row: (-row["mean_absolute_luma_change"], row["frame_ordinal"]))[:16]
        summary["input_final_identities"] = [reader.finish() for reader in readers]
    finally:
        for reader in readers:
            reader.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    proof_path = Path(__file__).with_name("mpeg1-natural-proof.py"); proof_pin = identity(proof_path)
    proof = load(proof_path, "mpeg1_natural_proof")
    controls = {"packet_mapping": proof.self_test(), "metrics": metric_controls()}
    if args.self_test:
        require(args.manifest is None and args.output is None, "self-test is separate from actual input reads")
        print(json.dumps({"status": "PASS", "controls": controls}, sort_keys=True)); return 0
    require(args.manifest is not None, "--manifest is required for an actual input audit")
    raw_manifest = args.manifest.read_bytes(); manifest = json.loads(raw_manifest)
    require(manifest.get("schema") == "crystalhd.issue606.natural-inputs.v1" and manifest.get("population") == {"width": W, "height": H, "frames": N, "rate": "25/1"}, "manifest schema/population differs")
    require(manifest.get("orders") == list(ORDERS), "both original model orders must stay frozen")
    helpers = manifest["helpers"]
    axis = load(helpers["axis"]["path"], "issue606_axis", helpers["axis"])
    vertical = load(helpers["vertical"]["path"], "issue606_vertical", helpers["vertical"])
    separable = load(helpers["separable"]["path"], "issue606_separable", helpers["separable"])
    require(separable.ORDERS == ORDERS and separable.V_RULE == vertical.FORMULA and axis.RULE == separable.MASK_RULE, "frozen helper rules differ")
    require(args.output is not None and args.output.is_absolute(), "exclusive absolute output directory required")
    args.output.mkdir(mode=0o700, exist_ok=False)
    summary = {"schema": "crystalhd.issue606.natural-audit.v1", "status": "FAIL_PRESERVE_FIRST_ATTEMPT", "first_failure": None, "completed_frames": 0, "sessions": [], "source_identity": identity(__file__), "proof_source_identity": proof_pin, "manifest_identity": identity(args.manifest), "helper_identities": helpers, "controls": controls, "python": sys.version, "numpy_version": np.__version__,
               "scope": {"quality_pass_claim": False, "project_local_chroma_target_unchanged": 3, "both_models_retained_without_selection": True, "original_checker22_full_Lanczos_failure": "PRESERVED", "fitting_or_threshold_changes": False, "new_hardware_decode_encoder_or_foreign_converter": "NOT_RUN by this offline audit", "PTS_origin": "actual MKV packet/decoded-frame PTS joined through identical coded ES AU payloads", "raw_I420_embeds_PTS": False, "original_source_movie_absolutePTS": "not claimed after authored clipping/rate conversion", "normal_frontend_audio_drift_physical_AV": "separate gate", "source_provenance": manifest.get("source_provenance"), "percentiles": "nearest rank, cumulative population >=ceil(percent*N/100)", "outlier_sorting": "review metadata only; source coded/display/raw order is never changed"}}
    try:
        audit(manifest, args.output, summary, proof, axis, vertical, separable)
        require(summary["source_identity"] == identity(__file__) and proof_pin == identity(proof_path) and raw_manifest == args.manifest.read_bytes(), "owned source/proof or manifest changed")
        require(all(pin == identity(pin["path"]) for pin in helpers.values()), "frozen helper changed")
        summary["status"] = "NATURAL_MAPPING_REPEAT_AND_FIXED_MODEL_METRICS_MEASURED"
        summary["artifacts"] = [identity(args.output/name) for name in ("picture-PTS-proof.json", "per-frame.jsonl", "metrics.json", "error-maps.json")]
    except Exception as error:
        summary["first_failure"] = {"type": type(error).__name__, "message": str(error)}
    finally:
        with (args.output/"report.json").open("x") as stream:
            json.dump(summary, stream, indent=2, sort_keys=True); stream.write("\n")
    print(json.dumps({"status": summary["status"], "first_failure": summary["first_failure"], "completed_frames": summary["completed_frames"], "output": str(args.output)}, sort_keys=True))
    return 0 if summary["status"] == "NATURAL_MAPPING_REPEAT_AND_FIXED_MODEL_METRICS_MEASURED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
