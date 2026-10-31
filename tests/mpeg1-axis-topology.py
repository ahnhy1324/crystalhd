#!/usr/bin/env python3
"""Offline, fixed-mask topology of retained MPEG-1 chroma differences.

No subprocess, conversion, decoder, hardware, fitting, sorting or pixel writes.
Root is the execution owner. NumPy is supplied privately by the operator.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

import numpy as np


RADIUS = 3
AXES = ("interior", "x", "y", "xy")
ENDPOINTS = ("zero", "interior_1_to_254", "full_255")
GROUPS = tuple((axis, parity, endpoint) for axis in AXES
               for parity in (0, 1) for endpoint in ENDPOINTS)
STAT_KEYS = ("samples", "changed", "absolute_sum", "signed_sum",
             "positive", "negative")
SCHEMA = "crystalhd.issue529.fixed-axis-topology.v1"
RULE = "both-sided adjacent-byte gradients; valid-grid Chebyshev-radius3 dilation; output row y maps to source row y//2"


def component(value):
    if not isinstance(value, np.ndarray) or value.dtype != np.uint8 or value.ndim != 2:
        raise ValueError("a component must be a two-dimensional uint8 grid")
    if not value.size:
        raise ValueError("empty component grid")
    return value


def dilate(mask):
    """Clipped valid-grid radius3 square; outside the image adds no edges."""
    height, width = mask.shape
    padded = np.pad(mask, ((0, 0), (RADIUS, RADIUS)), constant_values=False)
    horizontal = np.zeros_like(mask)
    for shift in range(2 * RADIUS + 1):
        horizontal |= padded[:, shift:shift + width]
    padded = np.pad(horizontal, ((RADIUS, RADIUS), (0, 0)), constant_values=False)
    result = np.zeros_like(mask)
    for shift in range(2 * RADIUS + 1):
        result |= padded[shift:shift + height, :]
    return result


def axis_classes(planar_chroma):
    """Masks depend on retained planar input, never on comparison residuals."""
    source = component(planar_chroma)
    x_edge = np.zeros(source.shape, dtype=np.bool_)
    y_edge = np.zeros(source.shape, dtype=np.bool_)
    difference = source[:, 1:] != source[:, :-1]
    x_edge[:, 1:] |= difference
    x_edge[:, :-1] |= difference
    difference = source[1:, :] != source[:-1, :]
    y_edge[1:, :] |= difference
    y_edge[:-1, :] |= difference
    source_classes = dilate(x_edge).astype(np.uint8) + 2 * dilate(y_edge).astype(np.uint8)
    return np.repeat(source_classes, 2, axis=0)


def scalar_classes(source):
    """Independent control oracle: enumerate each valid derivative neighbour."""
    height, width = len(source), len(source[0])
    result = []
    for output_y in range(height * 2):
        q = output_y // 2
        row = []
        for x in range(width):
            horizontal, vertical = False, False
            for y1 in range(max(0, q - RADIUS), min(height, q + RADIUS + 1)):
                for x1 in range(max(0, x - RADIUS), min(width, x + RADIUS + 1)):
                    if x1 > 0 and source[y1][x1] != source[y1][x1 - 1]:
                        horizontal = True
                    if x1 + 1 < width and source[y1][x1] != source[y1][x1 + 1]:
                        horizontal = True
                    if y1 > 0 and source[y1][x1] != source[y1 - 1][x1]:
                        vertical = True
                    if y1 + 1 < height and source[y1][x1] != source[y1 + 1][x1]:
                        vertical = True
            row.append(int(horizontal) + 2 * int(vertical))
        result.append(row)
    return result


def packed_planes(raw, width, height):
    if width <= 0 or height <= 0 or width % 2 or height % 2 or len(raw) != width * height * 2:
        raise ValueError("invalid active YUYV extent")
    values = np.frombuffer(raw, dtype=np.uint8)
    packed = values.reshape(height, width // 2, 4)
    return {"Y": values[::2].reshape(height, width),
            "Cb": packed[:, :, 1], "Cr": packed[:, :, 3]}


def planar_planes(raw, width, height):
    if width <= 0 or height <= 0 or width % 2 or height % 2 or len(raw) != width * height * 3 // 2:
        raise ValueError("invalid active I420 extent")
    values = np.frombuffer(raw, dtype=np.uint8)
    y, c = width * height, width * height // 4
    return {"Y": values[:y].reshape(height, width),
            "Cb": values[y:y + c].reshape(height // 2, width // 2),
            "Cr": values[y + c:].reshape(height // 2, width // 2)}


def empty_stats(index):
    axis, parity, endpoint = GROUPS[index]
    return {"axis_support": axis, "output_row_parity": parity,
            "reference_endpoint": endpoint,
            **{key: 0 for key in STAT_KEYS}, "maximum_absolute_error": 0,
            "maximum_examples_up_to8": []}


def measure(candidate, reference, source, frame_index):
    candidate, reference = component(candidate), component(reference)
    source = component(source)
    if candidate.shape != reference.shape or candidate.shape != (source.shape[0] * 2, source.shape[1]):
        raise ValueError("source/candidate/reference component extents disagree")
    classes = axis_classes(source)
    endpoints = np.where(reference == 0, 0, np.where(reference == 255, 2, 1)).astype(np.uint8)
    parity = (np.arange(reference.shape[0], dtype=np.int64) % 2)[:, None]
    labels = classes * 6 + parity * 3 + endpoints
    delta = candidate.astype(np.int16) - reference.astype(np.int16)
    absolute = np.abs(delta)
    groups = []
    for index in range(len(GROUPS)):
        values = empty_stats(index)
        selected = labels == index
        selected_delta = delta[selected]
        selected_absolute = absolute[selected]
        values.update(samples=int(selected_delta.size),
                      changed=int(np.count_nonzero(selected_delta)),
                      absolute_sum=int(selected_absolute.sum(dtype=np.int64)),
                      signed_sum=int(selected_delta.sum(dtype=np.int64)),
                      positive=int(np.count_nonzero(selected_delta > 0)),
                      negative=int(np.count_nonzero(selected_delta < 0)))
        maximum = int(selected_absolute.max(initial=0))
        values["maximum_absolute_error"] = maximum
        if maximum:
            for y, x in np.argwhere(selected & (absolute == maximum))[:8]:
                values["maximum_examples_up_to8"].append({
                    "frame_ordinal": frame_index, "x": int(x), "y": int(y),
                    "candidate": int(candidate[y, x]), "reference": int(reference[y, x]),
                    "signed_error": int(delta[y, x])})
        groups.append(values)
    if sum(group["samples"] for group in groups) != candidate.size:
        raise ValueError("topology groups do not partition the component")
    return groups


def merge(total, current):
    for destination, part in zip(total, current):
        if any(destination[key] != part[key] for key in ("axis_support", "output_row_parity", "reference_endpoint")):
            raise ValueError("group identities changed")
        for key in STAT_KEYS:
            destination[key] += part[key]
        old, new = destination["maximum_absolute_error"], part["maximum_absolute_error"]
        if new > old:
            destination["maximum_absolute_error"] = new
            destination["maximum_examples_up_to8"] = list(part["maximum_examples_up_to8"])
        elif new == old and new:
            destination["maximum_examples_up_to8"] = (
                destination["maximum_examples_up_to8"] + part["maximum_examples_up_to8"])[:8]


def totals(groups):
    return {**{key: sum(group[key] for group in groups) for key in STAT_KEYS},
            "maximum_absolute_error": max(group["maximum_absolute_error"] for group in groups)}


def self_test():
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

    yy, xx = np.indices((16, 16))
    uniform = np.full((16, 16), 80, dtype=np.uint8)
    horizontal = np.where(xx >= 8, 80, 0).astype(np.uint8)
    vertical = np.where(yy >= 8, 80, 0).astype(np.uint8)
    crossed = (horizontal + vertical).astype(np.uint8)
    x_boundary = np.where(xx >= 1, 80, 0).astype(np.uint8)
    y_boundary = np.where(yy >= 1, 80, 0).astype(np.uint8)
    fixtures = (uniform, horizontal, vertical, crossed, x_boundary, y_boundary)
    expected = ([512, 0, 0, 0], [256, 256, 0, 0], [256, 0, 256, 0],
                [128, 128, 128, 128], [352, 160, 0, 0], [352, 0, 160, 0])
    for label, source, counts in zip(("uniform", "x_step", "y_step", "crossed_steps", "x_boundary", "y_boundary"), fixtures, expected):
        actual = axis_classes(source)
        check(label + "_literal_partition_counts", np.bincount(actual.ravel(), minlength=4).tolist() == counts)
    check("independent_scalar_support_all_six_fixtures",
          all(axis_classes(source).tolist() == scalar_classes(source.tolist()) for source in fixtures))
    classes = axis_classes(crossed)
    check("crossed_parity_literal_counts", all(np.count_nonzero(classes[parity::2] == axis) == 64
                                               for parity in (0, 1) for axis in range(4)))
    reference = np.full((32, 16), 100, dtype=np.uint8)
    candidate = reference.copy()
    for x, y, ref, value in ((0, 0, 0, 22), (4, 1, 255, 250), (0, 8, 100, 98),
                              (4, 9, 0, 1), (15, 31, 255, 255)):
        reference[y, x], candidate[y, x] = ref, value
    measured = measure(candidate, reference, crossed, 7)
    check("literal_signed_and_absolute_totals", totals(measured) == {
        "samples": 512, "changed": 4, "absolute_sum": 30, "signed_sum": 16,
        "positive": 2, "negative": 2, "maximum_absolute_error": 22})
    check("literal_four_changed_axis_classes", [sum(g["changed"] for g in measured if g["axis_support"] == a) for a in AXES] == [1, 1, 1, 1])
    check("literal_endpoint_bucket_ownership", measured[0]["samples"] == 1 and measured[0]["absolute_sum"] == 22
          and measured[11]["samples"] == 1 and measured[11]["signed_sum"] == -5
          and measured[13]["samples"] == 64 and measured[13]["absolute_sum"] == 2
          and measured[21]["samples"] == 1 and measured[21]["signed_sum"] == 1
          and measured[5]["samples"] == 1 and measured[5]["changed"] == 0)
    check("literal_maximum_coordinate", measured[0]["maximum_examples_up_to8"] == [{
        "frame_ordinal": 7, "x": 0, "y": 0, "candidate": 22, "reference": 0, "signed_error": 22}])
    exact = measure(reference, reference, crossed, 0)
    check("byte_identity_has_zero_residual", totals(exact)["changed"] == 0 and totals(exact)["maximum_absolute_error"] == 0)
    all_zero = np.zeros((32, 16), dtype=np.uint8)
    all_full = np.full((32, 16), 255, dtype=np.uint8)
    extreme = measure(all_zero, all_full, uniform, 0)
    check("uint8_subtraction_does_not_wrap", totals(extreme) == {
        "samples": 512, "changed": 512, "absolute_sum": 130560, "signed_sum": -130560,
        "positive": 0, "negative": 512, "maximum_absolute_error": 255})
    examples = measure(np.full((32, 16), 3, dtype=np.uint8), all_zero, uniform, 0)[0]["maximum_examples_up_to8"]
    check("maximum_example_bound_and_original_order", [(e["x"], e["y"]) for e in examples] == [(x, 0) for x in range(8)])
    merged = [empty_stats(index) for index in range(len(GROUPS))]
    merge(merged, measured)
    merge(merged, measured)
    check("aggregation_exact_sums_maximum_and_example_bound", totals(merged) == {
        "samples": 1024, "changed": 8, "absolute_sum": 60, "signed_sum": 32,
        "positive": 4, "negative": 4, "maximum_absolute_error": 22}
        and len(merged[0]["maximum_examples_up_to8"]) == 2)
    check("reject_non_uint8_component", rejects(component, np.zeros((2, 2), dtype=np.int16)))
    check("reject_non_grid_component", rejects(component, np.zeros(4, dtype=np.uint8)))
    check("reject_disagreeing_extents", rejects(measure, all_zero, all_zero[:, :15], uniform, 0))
    check("reject_short_packed_extent", rejects(packed_planes, bytes(15), 4, 2))
    check("reject_short_planar_extent", rejects(planar_planes, bytes(11), 4, 2))
    packed = packed_planes(bytes((0, 10, 1, 20, 2, 11, 3, 21, 4, 12, 5, 22, 6, 13, 7, 23)), 4, 2)
    planar = planar_planes(bytes((0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 20, 21)), 4, 2)
    check("literal_active_plane_layouts", packed["Y"].tolist() == [[0, 1, 2, 3], [4, 5, 6, 7]]
          and packed["Cb"].tolist() == [[10, 11], [12, 13]]
          and packed["Cr"].tolist() == [[20, 21], [22, 23]]
          and planar["Y"].tolist() == packed["Y"].tolist()
          and planar["Cb"].tolist() == [[10, 11]] and planar["Cr"].tolist() == [[20, 21]])
    return {"status": "PASS", "control_groups": len(names), "names": names,
            "independent_scalar_support_oracle": True, "hardware_or_media": "NOT_RUN"}


def stat_key(value):
    return tuple(getattr(value, name) for name in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns"))


class FrozenInput:
    def __init__(self, item):
        self.path = Path(item["path"])
        if not self.path.is_absolute() or not isinstance(item["bytes"], int) or item["bytes"] <= 0:
            raise ValueError("input identity needs an absolute path and positive exact size")
        self.expected = item
        descriptor = os.open(self.path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        self.stream = os.fdopen(descriptor, "rb")
        self.before = os.fstat(self.stream.fileno())
        self.sha = hashlib.sha256()
        if not stat.S_ISREG(self.before.st_mode) or self.before.st_size != item["bytes"]:
            self.stream.close()
            raise ValueError("input type or size differs from frozen manifest")
        preflight = hashlib.sha256()
        while chunk := self.stream.read(1024 * 1024):
            preflight.update(chunk)
        if preflight.hexdigest() != item["sha256"]:
            self.stream.close()
            raise ValueError("preflight input hash differs from frozen manifest")
        self.stream.seek(0)
        self.unchanged()

    def unchanged(self):
        if stat_key(self.before) != stat_key(os.fstat(self.stream.fileno())) or stat_key(self.before) != stat_key(self.path.lstat()):
            raise ValueError("input path/descriptor metadata changed")

    def frame(self, size):
        value = self.stream.read(size)
        if len(value) != size:
            raise ValueError("short active frame")
        self.sha.update(value)
        return value

    def finish(self):
        if self.stream.read(1) or self.sha.hexdigest() != self.expected["sha256"]:
            raise ValueError("trailing bytes or final input hash mismatch")
        self.unchanged()
        return {**self.expected, "preflight_and_final_sha256_match": True,
                "unchanged_during_read": True,
                "stat": dict(zip(("dev", "ino", "size", "mtime_ns", "ctime_ns"), stat_key(self.before)))}

    def close(self):
        self.stream.close()


def identity(path):
    path = Path(path)
    value = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(value), "sha256": hashlib.sha256(value).hexdigest()}


def compare_case(case):
    width, height, frames = case["width"], case["height"], case["frames"]
    if (width, height) != (352, 288) or frames not in (30, 90):
        raise ValueError("actual comparison is restricted to frozen352x288/30or90 populations")
    sizes = {"native": width * height * 2, "reference": width * height * 2,
             "planar": width * height * 3 // 2}
    for key, size in sizes.items():
        if case[key]["bytes"] != size * frames:
            raise ValueError("manifest stream extent disagrees with its fixed population")
    readers = {}
    try:
        for key in sizes:
            readers[key] = FrozenInput(case[key])
        aggregates = {name: [empty_stats(index) for index in range(len(GROUPS))] for name in ("Cb", "Cr")}
        per_frame = []
        for ordinal in range(frames):
            native = packed_planes(readers["native"].frame(sizes["native"]), width, height)
            reference = packed_planes(readers["reference"].frame(sizes["reference"]), width, height)
            planar = planar_planes(readers["planar"].frame(sizes["planar"]), width, height)
            if not np.array_equal(native["Y"], planar["Y"]) or not np.array_equal(reference["Y"], planar["Y"]):
                raise ValueError("every active native/reference luma byte must match this ordinal's planarY")
            row = {"frame_ordinal": ordinal, "all_active_Y_exact": True, "components": {}}
            for name in ("Cb", "Cr"):
                groups = measure(native[name], reference[name], planar[name], ordinal)
                merge(aggregates[name], groups)
                row["components"][name] = {"totals": totals(groups), "groups": groups}
            per_frame.append(row)
        qualified = {key: reader.finish() for key, reader in readers.items()}
        total = {name: {"totals": totals(groups), "groups": groups} for name, groups in aggregates.items()}
        if any(total[name]["totals"]["samples"] != frames * width * height // 2 for name in total):
            raise ValueError("whole-population component sample census failed")
        if [total[name]["totals"]["maximum_absolute_error"] for name in ("Cb", "Cr")] != case["previous_fixed_reference_maximum_Cb_Cr"]:
            raise ValueError("fixed-reference bounds disagree with the separately retained comparison")
        return {"fixture": case["fixture"], "status": "FIXED_REFERENCE_TOPOLOGY_MEASURED",
                "frames": frames, "width": width, "height": height,
                "all_frame_Y_exact": True, "inputs": qualified,
                "components_all_frames": total, "per_frame_in_original_raw_order": per_frame}
    finally:
        for reader in readers.values():
            reader.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output", type=Path, help="exclusive NEW private result directory")
    args = parser.parse_args()
    if args.self_test:
        if args.manifest or args.output:
            parser.error("--self-test is separate from media-file reads")
        print(json.dumps(self_test(), sort_keys=True))
        return 0
    if args.manifest is None or args.output is None or not args.output.is_absolute():
        parser.error("--manifest and absolute exclusive --output are required")
    args.output.mkdir(mode=0o700, exist_ok=False)
    summary = {"schema": SCHEMA, "status": "FAIL_PRESERVE_FIRST_ATTEMPT", "first_failure": None,
               "cases": [], "source_identity": identity(__file__), "manifest_identity": identity(args.manifest),
               "python": {"executable": str(Path(sys.executable).resolve()), "version": sys.version},
               "numpy": {"version": np.__version__, "file": str(Path(np.__file__).resolve())},
               "mask_rule": RULE, "mask_radius_source_chroma_cells": RADIUS,
               "interpretation": {
                   "quality_pass_claim": False, "project_local_target_unchanged": 3,
                   "native_PTS": "NOT_RECOVERED; frame ordinals are raw-file order only",
                   "endpoint_scope": "reference byte equals0/255; no proof of clipping in a particular implementation stage",
                   "axis_scope": "local decoded-input edge topology; not actual firmware/filter support or causal attribution",
                   "hardware_decoder_converter_or_resampler": "NOT_RUN",
                   "new_filter_or_fitting_or_threshold_change": False,
                   "physical_or_codec_support_claim": False}}
    try:
        summary["pure_controls"] = self_test()
        manifest = json.loads(args.manifest.read_text())
        if manifest.get("schema") != "crystalhd.issue529.fixed-axis-topology-inputs.v1" or manifest.get("mask_rule") != RULE:
            raise ValueError("manifest schema or preregistered fixed mask differs")
        cases = manifest.get("cases")
        if not isinstance(cases, list) or [case.get("fixture") for case in cases] != ["horizontal30", "checker90"]:
            raise ValueError("both declared fixtures must run once in their frozen order")
        for case in cases:
            result = compare_case(case)
            filename = case["fixture"] + ".topology.json"
            with (args.output / filename).open("x") as output:
                json.dump(result, output, indent=2, sort_keys=True)
                output.write("\n")
            summary["cases"].append({"fixture": case["fixture"], "status": result["status"],
                                     "report": identity(args.output / filename),
                                     "component_totals": {name: result["components_all_frames"][name]["totals"] for name in ("Cb", "Cr")}})
        if summary["source_identity"] != identity(__file__) or summary["manifest_identity"] != identity(args.manifest):
            raise ValueError("owned source or frozen manifest changed")
        summary["status"] = "FIXED_REFERENCE_TOPOLOGY_MEASURED"
    except Exception as error:
        summary["first_failure"] = {"type": type(error).__name__, "message": str(error)}
    finally:
        with (args.output / "report.json").open("x") as output:
            json.dump(summary, output, indent=2, sort_keys=True)
            output.write("\n")
    print(json.dumps({"status": summary["status"], "first_failure": summary["first_failure"], "output": str(args.output)}))
    return 0 if summary["status"] == "FIXED_REFERENCE_TOPOLOGY_MEASURED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
