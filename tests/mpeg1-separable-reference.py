#!/usr/bin/env python3
"""Two preregistered separable arithmetic hypotheses on retained MPEG-1 bytes.

No fit, decoder, foreign converter, hardware, subprocess or generated pixel file.
Root owns execution. Both operation orders and all residuals must be retained.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import sys

import numpy as np

ORDERS = ("H8_then_V8", "V8_then_H8")
OFFSETS = (-3, -2, -1, 0, 1, 2)
H_RULE = "normalized float64 Lanczos3 at j-0.25; six integer source taps with |distance|<3; clamp each tap index; floor(value+0.5) then clip0..255"
V_RULE = "q=y//2; adjacent=clamp(q-1) for even y else clamp(q+1); value=(3*source[q,x]+source[adjacent,x]+2)//4"
MASK_RULE = "both-sided adjacent-byte gradients; valid-grid Chebyshev-radius3 dilation; output row y maps to source row y//2"
MARGIN_WARNING = 1e-10


def load(pin, name):
    import hashlib
    path = Path(pin["path"])
    raw = path.read_bytes()
    if {"path": str(path.resolve()), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()} != pin:
        raise ValueError("owned helper identity differs")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def taps(width):
    coordinates = np.arange(width, dtype=np.float64) - 0.25
    offsets = np.array(OFFSETS, dtype=np.int64)
    distance = -0.25 - offsets.astype(np.float64)
    weights = np.sinc(distance) * np.sinc(distance / 3.0)
    weights /= weights.sum(dtype=np.float64)
    indices = np.clip(np.arange(width, dtype=np.int64)[:, None] + offsets, 0, width - 1)
    return coordinates, indices, weights


def horizontal(source, axis):
    source = axis.component(source)
    _, indices, weights = taps(source.shape[1])
    raw = np.zeros(source.shape, dtype=np.float64)
    for tap in range(len(OFFSETS)):
        raw += source[:, indices[:, tap]].astype(np.float64) * weights[tap]
    rounded = np.floor(raw + 0.5)
    relevant = raw[(raw >= 0.0) & (raw <= 255.0)]
    margins = np.abs(relevant - np.floor(relevant) - 0.5)
    metadata = {"float_samples": int(raw.size), "minimum_unrounded": float(raw.min()),
                "maximum_unrounded": float(raw.max()), "rounded_below0": int(np.count_nonzero(rounded < 0)),
                "rounded_above255": int(np.count_nonzero(rounded > 255)),
                "rounding_margin_samples_in_0_to_255": int(relevant.size),
                "minimum_distance_to_half_integer": float(margins.min()) if margins.size else None,
                "samples_within_fixed_rounding_warning": int(np.count_nonzero(margins <= MARGIN_WARNING))}
    return np.clip(rounded, 0, 255).astype(np.uint8), metadata, raw


def scalar_horizontal(source):
    result, floats = [], []
    for row in source:
        output, raw_row = [], []
        for j in range(len(row)):
            coordinate = j - 0.25
            coefficients, values = [], []
            for k in range(math.ceil(coordinate - 3), math.floor(coordinate + 3) + 1):
                distance = coordinate - k
                if abs(distance) >= 3:
                    continue
                weight = 1.0 if distance == 0 else (math.sin(math.pi * distance) / (math.pi * distance)) * (math.sin(math.pi * distance / 3) / (math.pi * distance / 3))
                coefficients.append(weight)
                values.append(row[max(0, min(len(row) - 1, k))])
            normalization = math.fsum(coefficients)
            value = math.fsum(weight * sample / normalization for weight, sample in zip(coefficients, values))
            raw_row.append(value)
            output.append(max(0, min(255, math.floor(value + 0.5))))
        result.append(output)
        floats.append(raw_row)
    return result, floats


def models(source, axis, vertical):
    first_h, h_meta, _ = horizontal(source, axis)
    first_v = vertical.vertical31(source, axis)
    second_h, vh_meta, _ = horizontal(first_v, axis)
    return {ORDERS[0]: (vertical.vertical31(first_h, axis), h_meta), ORDERS[1]: (second_h, vh_meta)}


def self_test(axis, vertical):
    names, raw_errors = [], []
    def check(name, condition):
        if not condition:
            raise AssertionError("control failed: " + name)
        names.append(name)
    coordinates, indices, weights = taps(16)
    check("literal_quarter_phase_and_clamped_taps", coordinates[:3].tolist() == [-0.25, 0.75, 1.75]
          and indices[0].tolist() == [0, 0, 0, 0, 1, 2] and indices[-1].tolist() == [12, 13, 14, 15, 15, 15])
    check("six_finite_normalized_signed_weights", np.isfinite(weights).all() and abs(float(weights.sum()) - 1) < 1e-15
          and np.sign(weights).tolist() == [1, -1, 1, 1, -1, 1])
    fixtures = [np.full((4, 16), value, dtype=np.uint8) for value in (0, 17, 128, 255)]
    check("literal_constant_preservation", all(np.array_equal(horizontal(source, axis)[0], source) for source in fixtures))
    impulse = np.zeros((1, 16), dtype=np.uint8)
    impulse[0, 8] = 128
    expected = [0] * 16
    for j, value in ((6, 4), (8, 114), (9, 35), (11, 1)):
        expected[j] = value
    check("literal_interior_impulse", horizontal(impulse, axis)[0].ravel().tolist() == expected)
    border = np.zeros((1, 16), dtype=np.uint8)
    border[0, 0] = 128
    check("literal_left_border_impulse", horizontal(border, axis)[0].ravel().tolist() == [141, 27, 0, 1] + [0] * 12)
    ramp = (40 + 4 * np.arange(16, dtype=np.uint8))[None, :]
    check("literal_interior_ramp", horizontal(ramp, axis)[0][0, 3:-2].tolist() == list(range(51, 92, 4)))
    column = np.array([[0], [2], [6], [10]], dtype=np.uint8)
    check("single_column_horizontal_clamp", np.array_equal(horizontal(column, axis)[0], column))
    check("literal_vertical_rule_unchanged", vertical.vertical31(column, axis).ravel().tolist() == [0, 1, 2, 3, 5, 7, 9, 10])
    two_dimensional = np.zeros((4, 16), dtype=np.uint8)
    two_dimensional[1, 0] = 255
    candidates = models(two_dimensional, axis, vertical)
    check("literal_operation_order_difference_retained", tuple(candidates) == ORDERS
          and int(candidates[ORDERS[0]][0][1, 0]) == 64 and int(candidates[ORDERS[1]][0][1, 0]) == 71)
    fixtures += [impulse, border, ramp, column, two_dimensional]
    for source in fixtures:
        vector, _, raw = horizontal(source, axis)
        scalar, scalar_raw = scalar_horizontal(source.tolist())
        raw_errors.append(float(np.max(np.abs(raw - np.array(scalar_raw, dtype=np.float64)))))
        if vector.tolist() != scalar:
            raise AssertionError("independent scalar horizontal bytes differ")
    check("independent_math_sin_vs_vector_sinc", max(raw_errors) < MARGIN_WARNING)
    independent = {}
    first_h, _ = scalar_horizontal(two_dimensional.tolist())
    independent[ORDERS[0]] = vertical.scalar_vertical31(first_h)
    first_v = vertical.scalar_vertical31(two_dimensional.tolist())
    independent[ORDERS[1]], _ = scalar_horizontal(first_v)
    check("independent_scalar_both_operation_orders", all(candidates[order][0].tolist() == independent[order] for order in ORDERS))
    check("metadata_float_sample_and_margin_census", all(candidates[order][1]["float_samples"] == two_dimensional.size * (1 if order == ORDERS[0] else 2) for order in ORDERS))
    return {"status": "PASS", "control_groups": len(names), "names": names,
            "maximum_control_raw_scalar_vector_difference": max(raw_errors), "actual_media_or_hardware": "NOT_RUN"}


def compare_case(case, axis, vertical):
    width, height, frames = case["width"], case["height"], case["frames"]
    if (width, height) != (352, 288) or frames not in (30, 90):
        raise ValueError("only frozen352x288/30or90 populations are allowed")
    sizes = {"native": width * height * 2, "planar": width * height * 3 // 2}
    readers = {}
    try:
        for name, size in sizes.items():
            if case[name]["bytes"] != size * frames:
                raise ValueError("frozen extent disagrees with population")
            readers[name] = axis.FrozenInput(case[name])
        totals = {order: {name: [axis.empty_stats(i) for i in range(len(axis.GROUPS))] for name in ("Cb", "Cr")} for order in ORDERS}
        per_frame = []
        for ordinal in range(frames):
            native = axis.packed_planes(readers["native"].frame(sizes["native"]), width, height)
            planar = axis.planar_planes(readers["planar"].frame(sizes["planar"]), width, height)
            if not np.array_equal(native["Y"], planar["Y"]):
                raise ValueError("every nativeY byte must match this raw ordinal's planarY")
            row = {"frame_ordinal": ordinal, "all_active_Y_exact": True, "orders": {order: {} for order in ORDERS}}
            for name in ("Cb", "Cr"):
                for order, (model, arithmetic) in models(planar[name], axis, vertical).items():
                    groups = axis.measure(native[name], model, planar[name], ordinal)
                    axis.merge(totals[order][name], groups)
                    row["orders"][order][name] = {"groups": groups, "totals": axis.totals(groups), "horizontal_arithmetic": arithmetic}
            per_frame.append(row)
        inputs = {name: reader.finish() for name, reader in readers.items()}
        results = {order: {name: {"groups": groups, "totals": axis.totals(groups)} for name, groups in components.items()} for order, components in totals.items()}
        if any(result["totals"]["samples"] != frames * width * height // 2 for components in results.values() for result in components.values()):
            raise ValueError("whole component sample census failed")
        return {"fixture": case["fixture"], "status": "BOTH_FIXED_SEPARABLE_MODELS_MEASURED", "frames": frames,
                "all_frame_Y_exact": True, "inputs": inputs, "orders_all_frames": results, "per_frame_in_original_raw_order": per_frame}
    finally:
        for reader in readers.values():
            reader.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    manifest_raw = args.manifest.read_bytes()
    manifest = json.loads(manifest_raw)
    if manifest.get("schema") != "crystalhd.issue529.fixed-separable-inputs.v1" or manifest.get("horizontal_rule") != H_RULE or manifest.get("vertical_rule") != V_RULE or manifest.get("orders") != list(ORDERS):
        parser.error("manifest model or both frozen orders differ")
    axis = load(manifest["axis_helper"], "issue529_full_axis")
    vertical = load(manifest["vertical_helper"], "issue529_full_vertical")
    if axis.RULE != MASK_RULE or vertical.FORMULA != V_RULE:
        parser.error("helper rule differs")
    if args.self_test:
        if args.output:
            parser.error("self-test is separate from actual raw-file reads")
        print(json.dumps(self_test(axis, vertical), sort_keys=True))
        return 0
    if args.output is None or not args.output.is_absolute():
        parser.error("exclusive absolute --output is required")
    args.output.mkdir(mode=0o700, exist_ok=False)
    summary = {"schema": "crystalhd.issue529.fixed-separable-discriminator.v1", "status": "FAIL_PRESERVE_FIRST_ATTEMPT",
               "first_failure": None, "cases": [], "source_identity": axis.identity(__file__), "manifest_identity": axis.identity(args.manifest),
               "helpers": {name: manifest[name] for name in ("axis_helper", "vertical_helper")},
               "horizontal_rule": H_RULE, "vertical_rule": V_RULE, "orders": list(ORDERS),
               "normalized_horizontal_weights": [float(v) for v in taps(176)[2]],
               "normalized_horizontal_weights_hex": [float(v).hex() for v in taps(176)[2]],
               "rounding_margin_warning": MARGIN_WARNING, "python": sys.version, "numpy_version": np.__version__,
               "interpretation": {"quality_pass_claim": False, "project_local_target_unchanged": 3,
                 "original_fixed_Lanczos_checker22_failure": "PRESERVED",
                 "native_PTS": "NOT_RECOVERED; raw ordinals only", "both_orders_retained_without_selection": True,
                 "coefficient_fit_filter_search_or_threshold_change": False, "new_hardware_decode_foreign_converter_or_pixel_file": "NOT_RUN",
                 "actual_hardware_coefficients_rounding_order_or_cause": "NOT_ESTABLISHED",
                 "arithmetic_margin": "float64 half-up model; every H-stage reports its distance to rounding boundaries and fixed warning count"}}
    try:
        summary["pure_controls"] = self_test(axis, vertical)
        if [case.get("fixture") for case in manifest["cases"]] != ["horizontal30", "checker90"]:
            raise ValueError("both complete fixtures must run once in frozen order")
        for case in manifest["cases"]:
            result = compare_case(case, axis, vertical)
            report = args.output / (case["fixture"] + ".separable.json")
            with report.open("x") as stream:
                json.dump(result, stream, indent=2, sort_keys=True)
                stream.write("\n")
            summary["cases"].append({"fixture": case["fixture"], "report": axis.identity(report), "orders_all_frames": result["orders_all_frames"]})
        if summary["source_identity"] != axis.identity(__file__) or manifest_raw != args.manifest.read_bytes() or any(pin != axis.identity(pin["path"]) for pin in summary["helpers"].values()):
            raise ValueError("owned source/helpers or frozen manifest changed")
        summary["status"] = "BOTH_FIXED_SEPARABLE_MODELS_MEASURED"
    except Exception as error:
        summary["first_failure"] = {"type": type(error).__name__, "message": str(error)}
    finally:
        with (args.output / "report.json").open("x") as stream:
            json.dump(summary, stream, indent=2, sort_keys=True)
            stream.write("\n")
    print(json.dumps({"status": summary["status"], "first_failure": summary["first_failure"], "output": str(args.output)}))
    return 0 if summary["status"] == "BOTH_FIXED_SEPARABLE_MODELS_MEASURED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
