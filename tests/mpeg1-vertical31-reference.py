#!/usr/bin/env python3
"""Evaluate one fixed prior vertical3:1 hypothesis on retained MPEG-1 bytes.

Only locally x-constant interior/y-support samples are scored. No fit, decoder,
converter, hardware, subprocess, filter selection, threshold or pixel-file output.
Root owns execution. NumPy and the hash-qualified owned topology helper are used.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

import numpy as np


FORMULA = "q=y//2; adjacent=clamp(q-1) for even y else clamp(q+1); value=(3*source[q,x]+source[adjacent,x]+2)//4"
MASK_RULE = "both-sided adjacent-byte gradients; valid-grid Chebyshev-radius3 dilation; output row y maps to source row y//2"
QUALIFIED_AXES = ("interior", "y")
EXCLUDED_STATUS = "NOT_QUALIFIED_FOR_HORIZONTAL_MODEL"
GROUPS = tuple((axis, parity) for axis in QUALIFIED_AXES for parity in (0, 1))
STAT_KEYS = ("samples", "changed", "absolute_sum", "signed_sum", "positive", "negative")
SCHEMA = "crystalhd.issue529.fixed-vertical31-discriminator.v1"


def identity(path):
    path = Path(path)
    value = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()}


def load_helper(pin):
    if identity(pin["path"]) != pin:
        raise ValueError("owned topology helper differs from its completed source identity")
    spec = importlib.util.spec_from_file_location("issue529_frozen_axis_helper", pin["path"])
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if module.RULE != MASK_RULE or module.RADIUS != 3:
        raise ValueError("owned helper mask rule/radius differs")
    return module


def vertical31(source, helper):
    source = helper.component(source)
    wide = source.astype(np.uint16)
    previous = np.concatenate((wide[:1], wide[:-1]), axis=0)
    following = np.concatenate((wide[1:], wide[-1:]), axis=0)
    result = np.empty((source.shape[0] * 2, source.shape[1]), dtype=np.uint8)
    result[::2] = (3 * wide + previous + 2) // 4
    result[1::2] = (3 * wide + following + 2) // 4
    return result


def scalar_vertical31(source):
    """Independent small-control model with literal clamped scalar indexing."""
    height, width = len(source), len(source[0])
    result = []
    for output_y in range(height * 2):
        current = output_y // 2
        neighbour = current - 1 if output_y % 2 == 0 else current + 1
        neighbour = max(0, min(height - 1, neighbour))
        result.append([(source[current][x] * 3 + source[neighbour][x] + 2) // 4
                       for x in range(width)])
    return result


def empty_group(axis, parity):
    return {"axis_support": axis, "output_row_parity": parity,
            "role": "PRIMARY_Y_ONLY" if axis == "y" else "INTERIOR_CONTROL",
            **{key: 0 for key in STAT_KEYS}, "maximum_absolute_error": 0,
            "maximum_examples_up_to8": []}


def compare_primary(native, model, source, ordinal, helper):
    native, model, source = helper.component(native), helper.component(model), helper.component(source)
    if native.shape != model.shape or native.shape != (source.shape[0] * 2, source.shape[1]):
        raise ValueError("native/model/source component extents disagree")
    classes = helper.axis_classes(source)
    parity = (np.arange(native.shape[0], dtype=np.int64) % 2)[:, None]
    groups, excluded = [], []
    for axis, row_parity in GROUPS:
        axis_number = 0 if axis == "interior" else 2
        mask = (classes == axis_number) & (parity == row_parity)
        candidate = native[mask].astype(np.int16)
        reference = model[mask].astype(np.int16)
        difference = candidate - reference
        absolute = np.abs(difference)
        maximum = int(absolute.max(initial=0))
        group = empty_group(axis, row_parity)
        group.update(samples=int(difference.size), changed=int(np.count_nonzero(difference)),
                     absolute_sum=int(absolute.sum(dtype=np.int64)),
                     signed_sum=int(difference.sum(dtype=np.int64)),
                     positive=int(np.count_nonzero(difference > 0)),
                     negative=int(np.count_nonzero(difference < 0)),
                     maximum_absolute_error=maximum)
        if maximum:
            locations = np.argwhere(mask)
            for index in np.flatnonzero(absolute == maximum)[:8]:
                y, x = locations[index]
                group["maximum_examples_up_to8"].append({
                    "frame_ordinal": ordinal, "x": int(x), "y": int(y),
                    "native": int(native[y, x]), "fixed_model": int(model[y, x]),
                    "signed_error": int(difference[index])})
        groups.append(group)
    for axis, number in (("x", 1), ("xy", 3)):
        for row_parity in (0, 1):
            excluded.append({"axis_support": axis, "output_row_parity": row_parity,
                             "status": EXCLUDED_STATUS,
                             "samples_not_scored": int(np.count_nonzero((classes == number) & (parity == row_parity))),
                             "residual_statistics": "NOT_COMPUTED"})
    if sum(group["samples"] for group in groups) + sum(group["samples_not_scored"] for group in excluded) != native.size:
        raise ValueError("qualified/excluded groups do not partition the component")
    return groups, excluded


def totals(groups):
    return {**{key: sum(group[key] for group in groups) for key in STAT_KEYS},
            "maximum_absolute_error": max(group["maximum_absolute_error"] for group in groups)}


def merge(destination, current):
    for total, part in zip(destination, current):
        if (total["axis_support"], total["output_row_parity"]) != (part["axis_support"], part["output_row_parity"]):
            raise ValueError("primary group identity changed")
        for key in STAT_KEYS:
            total[key] += part[key]
        old, new = total["maximum_absolute_error"], part["maximum_absolute_error"]
        if new > old:
            total["maximum_absolute_error"] = new
            total["maximum_examples_up_to8"] = list(part["maximum_examples_up_to8"])
        elif new == old and new:
            total["maximum_examples_up_to8"] = (total["maximum_examples_up_to8"] + part["maximum_examples_up_to8"])[:8]


def self_test(helper):
    names = []

    def check(name, condition):
        if not condition:
            raise AssertionError("control failed: " + name)
        names.append(name)

    def rejects(function, *arguments):
        try:
            function(*arguments)
        except ValueError:
            return True
        return False

    low = np.array([[0], [2], [6], [10]], dtype=np.uint8)
    high = np.array([[255], [253], [249], [245]], dtype=np.uint8)
    check("literal_low_border_parity_half_up", vertical31(low, helper).ravel().tolist() == [0, 1, 2, 3, 5, 7, 9, 10])
    check("literal_high_border_parity_no_overflow", vertical31(high, helper).ravel().tolist() == [255, 255, 254, 252, 250, 248, 246, 245])
    row = np.array([[0, 255]], dtype=np.uint8)
    check("single_row_clamps_both_parities", vertical31(row, helper).tolist() == [[0, 255], [0, 255]])
    yy, xx = np.indices((16, 16))
    horizontal = np.where(xx >= 8, 80, 0).astype(np.uint8)
    crossed = (20 + horizontal + np.where(yy >= 8, 80, 0)).astype(np.uint8)
    check("no_vertical_change_preserves_each_column", np.array_equal(vertical31(horizontal, helper), np.repeat(horizontal, 2, axis=0)))
    fixtures = (low, high, row, horizontal, crossed)
    check("independent_scalar_model_all_fixtures", all(vertical31(source, helper).tolist() == scalar_vertical31(source.tolist()) for source in fixtures))
    model = vertical31(crossed, helper)
    native = model.copy()
    for x, y, value in ((0, 0, 42), (0, 8, 18), (0, 9, 19), (4, 1, 250), (4, 9, 255)):
        native[y, x] = value
    groups, excluded = compare_primary(native, model, crossed, 7, helper)
    check("literal_primary_population_and_error_totals", totals(groups) == {
        "samples": 256, "changed": 3, "absolute_sum": 25, "signed_sum": 19,
        "positive": 1, "negative": 2, "maximum_absolute_error": 22})
    check("literal_primary_axis_parity_counts", [group["samples"] for group in groups] == [64, 64, 64, 64]
          and [group["changed"] for group in groups] == [1, 0, 1, 1]
          and [group["maximum_absolute_error"] for group in groups] == [22, 0, 2, 1])
    check("literal_primary_maximum_coordinate", groups[0]["maximum_examples_up_to8"] == [{
        "frame_ordinal": 7, "x": 0, "y": 0, "native": 42, "fixed_model": 20, "signed_error": 22}])
    check("excluded_x_xy_have_no_residual_statistics", all(group["status"] == EXCLUDED_STATUS and group["residual_statistics"] == "NOT_COMPUTED" for group in excluded)
          and [group["samples_not_scored"] for group in excluded] == [64, 64, 64, 64])
    excluded_only = model.copy()
    excluded_only[1, 4], excluded_only[9, 4] = 250, 255
    exact, _ = compare_primary(excluded_only, model, crossed, 0, helper)
    check("unqualified_large_errors_do_not_enter_primary", totals(exact)["changed"] == 0 and totals(exact)["maximum_absolute_error"] == 0)
    exact, _ = compare_primary(model, model, crossed, 0, helper)
    check("fixed_model_identity", totals(exact)["changed"] == 0)
    repeated = [empty_group(axis, parity) for axis, parity in GROUPS]
    merge(repeated, groups)
    merge(repeated, groups)
    check("literal_aggregation_sums_and_maximum", totals(repeated) == {
        "samples": 512, "changed": 6, "absolute_sum": 50, "signed_sum": 38,
        "positive": 2, "negative": 4, "maximum_absolute_error": 22})
    full_source = np.full((16, 16), 255, dtype=np.uint8)
    full_model = vertical31(full_source, helper)
    extreme, _ = compare_primary(np.zeros((32, 16), dtype=np.uint8), full_model, full_source, 0, helper)
    check("literal_signed255_extreme", totals(extreme) == {
        "samples": 512, "changed": 512, "absolute_sum": 130560, "signed_sum": -130560,
        "positive": 0, "negative": 512, "maximum_absolute_error": 255})
    control, _ = compare_primary(vertical31(horizontal, helper), vertical31(horizontal, helper), horizontal, 0, helper)
    check("horizontal_fixture_has_no_y_primary_samples", sum(group["samples"] for group in control if group["axis_support"] == "y") == 0)
    only_excluded, excluded = compare_primary(vertical31(row, helper), vertical31(row, helper), row, 0, helper)
    check("all_x_support_is_explicitly_unqualified", totals(only_excluded)["samples"] == 0 and sum(group["samples_not_scored"] for group in excluded) == 4)
    check("reject_non_uint8_model_source", rejects(vertical31, np.zeros((2, 2), dtype=np.int16), helper))
    check("reject_non_grid_model_source", rejects(vertical31, np.zeros(4, dtype=np.uint8), helper))
    check("reject_empty_model_source", rejects(vertical31, np.zeros((0, 2), dtype=np.uint8), helper))
    check("reject_disagreeing_model_extent", rejects(compare_primary, native, model[:, :-1], crossed, 0, helper))
    return {"status": "PASS", "control_groups": len(names), "names": names,
            "independent_scalar_model": True, "actual_media_or_hardware": "NOT_RUN"}


def compare_case(case, helper):
    width, height, frames = case["width"], case["height"], case["frames"]
    if (width, height) != (352, 288) or frames not in (30, 90):
        raise ValueError("comparison is restricted to frozen352x288/30or90 populations")
    sizes = {"native": width * height * 2, "planar": width * height * 3 // 2}
    for name, size in sizes.items():
        if case[name]["bytes"] != size * frames:
            raise ValueError("manifest extent disagrees with population")
    readers = {}
    try:
        for name in sizes:
            readers[name] = helper.FrozenInput(case[name])
        aggregates = {name: [empty_group(axis, parity) for axis, parity in GROUPS] for name in ("Cb", "Cr")}
        excluded_totals = {name: {axis: {str(parity): 0 for parity in (0, 1)} for axis in ("x", "xy")} for name in ("Cb", "Cr")}
        per_frame = []
        for ordinal in range(frames):
            native = helper.packed_planes(readers["native"].frame(sizes["native"]), width, height)
            planar = helper.planar_planes(readers["planar"].frame(sizes["planar"]), width, height)
            if not np.array_equal(native["Y"], planar["Y"]):
                raise ValueError("all native activeY must match the same raw ordinal's planarY")
            row = {"frame_ordinal": ordinal, "all_active_Y_exact": True, "components": {}}
            for name in ("Cb", "Cr"):
                model = vertical31(planar[name], helper)
                groups, excluded = compare_primary(native[name], model, planar[name], ordinal, helper)
                merge(aggregates[name], groups)
                for group in excluded:
                    excluded_totals[name][group["axis_support"]][str(group["output_row_parity"])] += group["samples_not_scored"]
                row["components"][name] = {"qualified_groups": groups, "excluded_groups": excluded}
            per_frame.append(row)
        qualified_inputs = {name: reader.finish() for name, reader in readers.items()}
        result = {}
        for name in ("Cb", "Cr"):
            result[name] = {"primary_y_only": totals([group for group in aggregates[name] if group["axis_support"] == "y"]),
                            "interior_control": totals([group for group in aggregates[name] if group["axis_support"] == "interior"]),
                            "all_qualified_totals": totals(aggregates[name]),
                            "qualified_groups": aggregates[name],
                            "excluded_horizontal_support": {"status": EXCLUDED_STATUS, "residual_statistics": "NOT_COMPUTED", "sample_counts_by_axis_and_output_parity": excluded_totals[name]}}
            if result[name]["all_qualified_totals"]["samples"] + sum(sum(parities.values()) for parities in excluded_totals[name].values()) != frames * width * height // 2:
                raise ValueError("whole qualified/excluded sample census failed")
        return {"fixture": case["fixture"], "status": "FIXED_VERTICAL31_DISCRIMINATOR_MEASURED",
                "frames": frames, "all_frame_Y_exact": True, "inputs": qualified_inputs,
                "components_all_frames": result, "per_frame_in_original_raw_order": per_frame}
    finally:
        for reader in readers.values():
            reader.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--output", type=Path, help="exclusive NEW absolute private result directory")
    args = parser.parse_args()
    manifest_pin = identity(args.manifest)
    manifest = json.loads(args.manifest.read_text())
    if manifest.get("schema") != "crystalhd.issue529.fixed-vertical31-inputs.v1" or manifest.get("formula") != FORMULA or manifest.get("mask_rule") != MASK_RULE:
        parser.error("manifest schema/formula/fixed mask differs")
    helper_pin = manifest["axis_helper"]
    helper = load_helper(helper_pin)
    if args.self_test:
        if args.output:
            parser.error("--self-test has no media output directory")
        controls = self_test(helper)
        controls["axis_helper"] = helper_pin
        print(json.dumps(controls, sort_keys=True))
        return 0
    if args.output is None or not args.output.is_absolute():
        parser.error("an absolute exclusive --output is required")
    args.output.mkdir(mode=0o700, exist_ok=False)
    summary = {"schema": SCHEMA, "status": "FAIL_PRESERVE_FIRST_ATTEMPT", "first_failure": None,
               "cases": [], "source_identity": identity(__file__), "manifest_identity": manifest_pin,
               "axis_helper_identity": helper_pin, "formula": FORMULA, "mask_rule": MASK_RULE,
               "python": {"executable": str(Path(sys.executable).resolve()), "version": sys.version},
               "numpy": {"version": np.__version__, "file": str(Path(np.__file__).resolve())},
               "interpretation": {
                   "quality_pass_claim": False, "project_local_target_unchanged": 3,
                   "primary": "y-only support; same-column arithmetic qualified only where fixed x-support mask is absent",
                   "interior": "control; not a universal decoder or source identity proof",
                   "x_and_xy": EXCLUDED_STATUS,
                   "formula_provenance": manifest["formula_provenance"],
                   "native_PTS": "NOT_RECOVERED; raw ordinal only",
                   "new_hardware_decode_external_converter_or_pixel_file": "NOT_RUN",
                   "coefficient_fit_filter_search_or_threshold_change": False,
                   "complete_firmware_pipeline_or_horizontal_resiting": "NOT_ESTABLISHED",
                   "physical_or_production_codec_claim": False}}
    try:
        summary["pure_controls"] = self_test(helper)
        cases = manifest.get("cases")
        if not isinstance(cases, list) or [case.get("fixture") for case in cases] != ["horizontal30", "checker90"]:
            raise ValueError("both declared fixtures must run once in frozen order")
        for case in cases:
            result = compare_case(case, helper)
            filename = case["fixture"] + ".vertical31.json"
            with (args.output / filename).open("x") as output:
                json.dump(result, output, indent=2, sort_keys=True)
                output.write("\n")
            summary["cases"].append({"fixture": case["fixture"], "status": result["status"],
                                     "report": identity(args.output / filename),
                                     "component_results": result["components_all_frames"]})
        if summary["source_identity"] != identity(__file__) or manifest_pin != identity(args.manifest) or helper_pin != identity(helper_pin["path"]):
            raise ValueError("owned source/helper or manifest changed")
        summary["status"] = "FIXED_VERTICAL31_DISCRIMINATOR_MEASURED"
    except Exception as error:
        summary["first_failure"] = {"type": type(error).__name__, "message": str(error)}
    finally:
        with (args.output / "report.json").open("x") as output:
            json.dump(summary, output, indent=2, sort_keys=True)
            output.write("\n")
    print(json.dumps({"status": summary["status"], "first_failure": summary["first_failure"], "output": str(args.output)}))
    return 0 if summary["status"] == "FIXED_VERTICAL31_DISCRIMINATOR_MEASURED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
