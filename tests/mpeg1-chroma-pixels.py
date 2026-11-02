#!/usr/bin/env python3
"""Owned offline active-YUV reader. No decoder, resampling, RGB, subprocess or sorting.
Root alone may invoke this source. NumPy is the only non-stdlib dependency.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import numpy as np

W, H = 352, 288
COMPONENTS = ("Y", "Cb", "Cr")

def frame_bytes(fmt, w=W, h=H):
    return w * h * (3 if fmt == "i420" else 4) // 2

def planes(raw, fmt, w=W, h=H):
    if len(raw) != frame_bytes(fmt, w, h):
        raise ValueError("incomplete active frame")
    a = np.frombuffer(raw, dtype=np.uint8)
    if fmt == "yuyv":
        packed = a.reshape(h, w // 2, 4)
        return {"Y": a[0::2].reshape(h, w), "Cb": packed[:, :, 1], "Cr": packed[:, :, 3]}
    y, c = w * h, w * h // 4
    return {"Y": a[:y].reshape(h, w), "Cb": a[y:y+c].reshape(h//2, w//2),
            "Cr": a[y+c:].reshape(h//2, w//2)}

def edges(reference):
    base = np.zeros(reference.shape, dtype=np.bool_)
    dx, dy = reference[:, 1:] != reference[:, :-1], reference[1:] != reference[:-1]
    base[:, 1:] |= dx
    base[:, :-1] |= dx
    base[1:] |= dy
    base[:-1] |= dy
    padded, mask = np.pad(base, 1), np.zeros_like(base)
    h, w = base.shape
    for y in range(3):
        for x in range(3):
            mask |= padded[y:y+h, x:x+w]
    return mask

def histogram(values, offset=0):
    return {str(i-offset): int(n) for i, n in enumerate(values) if n}

def measure(candidate, reference):
    if candidate.shape != reference.shape or candidate.dtype != np.uint8 or reference.dtype != np.uint8:
        raise ValueError("component grids/dtypes disagree")
    delta = candidate.astype(np.int16) - reference.astype(np.int16)
    absolute = np.abs(delta)
    changed, edge = absolute != 0, edges(reference)
    maximum = int(absolute.max())
    values = {"samples": int(candidate.size), "changed": int(np.count_nonzero(changed)),
              "absolute_sum": int(absolute.sum(dtype=np.int64)), "maximum": maximum,
              "edge_samples": int(np.count_nonzero(edge)),
              "edge_changed": int(np.count_nonzero(changed & edge)),
              "edge_absolute_sum": int(absolute[edge].sum(dtype=np.int64)),
              "absolute_hist": np.bincount(absolute.ravel(), minlength=256).astype(np.int64),
              "signed_hist": np.bincount((delta+255).ravel(), minlength=511).astype(np.int64),
              "row_changed": changed.sum(axis=1, dtype=np.int64),
              "column_changed": changed.sum(axis=0, dtype=np.int64)}
    summary = summarize(values)
    summary["grid"] = [int(candidate.shape[1]), int(candidate.shape[0])]
    changed_locations = np.argwhere(changed)
    summary["changed_bounds"] = None if not changed_locations.size else {
        "x_min": int(changed_locations[:, 1].min()), "x_max": int(changed_locations[:, 1].max()),
        "y_min": int(changed_locations[:, 0].min()), "y_max": int(changed_locations[:, 0].max())}
    summary["maximum_error_examples_up_to8"] = []
    if maximum:
        for y, x in np.argwhere(absolute == maximum)[:8]:
            summary["maximum_error_examples_up_to8"].append({"x": int(x), "y": int(y),
                "candidate": int(candidate[y, x]), "reference": int(reference[y, x]),
                "signed_error": int(delta[y, x])})
    return summary, values

def summarize(values):
    n, changed, en = values["samples"], values["changed"], values["edge_samples"]
    result = {"samples": n, "exact": changed == 0, "maximum_absolute_error": values["maximum"],
        "absolute_error_sum": values["absolute_sum"], "mean_absolute_error": values["absolute_sum"]/n,
        "changed_samples": changed, "changed_fraction": changed/n,
        "absolute_error_histogram": histogram(values["absolute_hist"]),
        "signed_error_histogram_candidate_minus_reference": histogram(values["signed_hist"], 255),
        "reference_edge_samples": en, "changed_samples_on_reference_edges": values["edge_changed"],
        "changed_samples_off_reference_edges": changed-values["edge_changed"],
        "edge_changed_fraction": values["edge_changed"]/en if en else None,
        "changed_sample_edge_fraction": values["edge_changed"]/changed if changed else None,
        "edge_mean_absolute_error": values["edge_absolute_sum"]/en if en else None}
    if "maximum_frames" in values:
        result["frames_at_maximum"] = values["maximum_frames"]
        result["changed_samples_by_component_row"] = values["row_changed"].tolist()
        result["changed_samples_by_component_column"] = values["column_changed"].tolist()
    return result

def merge(total, part, index):
    if total is None:
        total = {k: np.zeros_like(v) if isinstance(v, np.ndarray) else 0 for k, v in part.items()}
        total["maximum"], total["maximum_frames"] = -1, []
    for key, value in part.items():
        if key != "maximum":
            total[key] += value
    if part["maximum"] > total["maximum"]:
        total["maximum"], total["maximum_frames"] = part["maximum"], [index]
    elif part["maximum"] == total["maximum"]:
        total["maximum_frames"].append(index)
    return total

class FrozenReader:
    def __init__(self, path, expected):
        self.path, self.sha, self.count = Path(path), hashlib.sha256(), 0
        self.stream = self.path.open("rb")
        self.before = os.fstat(self.stream.fileno())
        if not self.path.is_file() or self.before.st_size != expected:
            self.stream.close()
            raise ValueError(f"unexpected raw-stream size: {self.path}")
    def frame(self, count):
        raw = self.stream.read(count)
        if len(raw) != count:
            raise ValueError(f"short frame: {self.path}")
        self.sha.update(raw)
        self.count += len(raw)
        return raw
    def finish(self):
        if self.stream.read(1):
            raise ValueError(f"trailing bytes: {self.path}")
        after, current = os.fstat(self.stream.fileno()), self.path.stat()
        keys = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
        if any(getattr(self.before, k) != getattr(after, k) or getattr(after, k) != getattr(current, k) for k in keys):
            raise ValueError(f"input changed during comparison: {self.path}")
        return {"path": str(self.path.resolve()), "bytes": self.count,
                "sha256": self.sha.hexdigest(), "unchanged_during_read": True}
    def close(self):
        self.stream.close()

def compare(args):
    streams = []
    try:
        size = frame_bytes(args.format)
        candidate = FrozenReader(args.candidate, size*args.frames)
        streams.append(candidate)
        reference = FrozenReader(args.reference, size*args.frames)
        streams.append(reference)
        planar = None
        if args.planar_luma:
            if args.format != "yuyv":
                raise ValueError("--planar-luma accompanies packed comparisons only")
            planar = FrozenReader(args.planar_luma, frame_bytes("i420")*args.frames)
            streams.append(planar)
        totals, planar_totals, frames = {k: None for k in COMPONENTS}, {}, []
        for index in range(args.frames):
            c, r = planes(candidate.frame(size), args.format), planes(reference.frame(size), args.format)
            frame = {"raw_frame_index": index, "synthetic_ordinal_time": {"num": index, "den": 25}, "components": {}}
            for name in COMPONENTS:
                metrics, part = measure(c[name], r[name])
                frame["components"][name] = metrics
                totals[name] = merge(totals[name], part, index)
                if index == 0:
                    metrics["changed_samples_by_component_row"] = part["row_changed"].tolist()
                    metrics["changed_samples_by_component_column"] = part["column_changed"].tolist()
            if planar:
                p = planes(planar.frame(frame_bytes("i420")), "i420")
                frame["planar_luma_controls"] = {}
                for label, grid in (("candidate_Y_vs_planar", c["Y"]), ("reference_Y_vs_planar", r["Y"])):
                    metrics, part = measure(grid, p["Y"])
                    frame["planar_luma_controls"][label] = metrics
                    planar_totals[label] = merge(planar_totals.get(label), part, index)
            frames.append(frame)
        identities = [stream.finish() for stream in streams]
        components = {name: summarize(total) for name, total in totals.items()}
        same = all(c["exact"] for c in components.values())
        result = {"schema": "crystalhd.issue529.pixel-comparison.v1", "status": "MEASURED",
            "candidate_minus_reference": True, "format": args.format, "geometry": [W, H],
            "frames_compared": args.frames, "frame_order": "original raw order; no native-token sorting",
            "time_qualification": "ordinal/25 is synthetic; native capture PTS not recovered",
            "input_identity": {"candidate": identities[0], "reference": identities[1]},
            "source_color": "reader performs no transform; see conversion receipt for range/matrix/siting",
            "edge_definition": "nonzero reference gradients; both sides; one-cell8-neighbor dilation on component grid",
            "edge_scope": "spatial concentration diagnostic, not causal proof",
            "same_bytes": same, "all_frame_Y_exact": components["Y"]["exact"],
            "require_identical_control": args.require_identical,
            "components_all_frames": components, "frames": frames,
            "quality_target_applied": False, "production_or_firmware_claim": False}
        if planar:
            result["input_identity"]["planar_luma"] = identities[2]
            result["planar_luma_controls_all_frames"] = {k: summarize(v) for k, v in planar_totals.items()}
        if args.require_identical and not same:
            result["status"] = "IDENTITY_CONTROL_FAIL"
        return result
    finally:
        for stream in streams:
            stream.close()

def self_test():
    """Hand-computable inert controls; root may execute with --self-test."""
    reference = np.array([[10, 20], [30, 40]], dtype=np.uint8)
    candidate = np.array([[10, 23], [28, 40]], dtype=np.uint8)
    measured, part = measure(candidate, reference)
    assert measured["maximum_absolute_error"] == 3
    assert measured["absolute_error_sum"] == 5
    assert measured["mean_absolute_error"] == 1.25
    assert measured["changed_fraction"] == 0.5
    assert measured["absolute_error_histogram"] == {"0": 2, "2": 1, "3": 1}
    assert measured["signed_error_histogram_candidate_minus_reference"] == {"-2": 1, "0": 2, "3": 1}
    total = summarize(merge(merge(None, part, 0), part, 1))
    assert total["samples"] == 8 and total["absolute_error_sum"] == 10
    assert total["changed_samples"] == 4 and total["frames_at_maximum"] == [0, 1]
    assert total["absolute_error_histogram"] == {"0": 4, "2": 2, "3": 2}
    packed = bytes([10, 11, 20, 12, 30, 13, 40, 14, 50, 15, 60, 16, 70, 17, 80, 18])
    grids = planes(packed, "yuyv", 4, 2)
    assert grids["Y"].tolist() == [[10, 20, 30, 40], [50, 60, 70, 80]]
    assert grids["Cb"].tolist() == [[11, 13], [15, 17]]
    assert grids["Cr"].tolist() == [[12, 14], [16, 18]]
    planar = planes(bytes([10, 20, 30, 40, 50, 60, 70, 80, 11, 13, 12, 14]), "i420", 4, 2)
    assert planar["Y"].tolist() == grids["Y"].tolist()
    assert planar["Cb"].tolist() == [[11, 13]] and planar["Cr"].tolist() == [[12, 14]]
    extreme, _ = measure(np.array([[0]], dtype=np.uint8), np.array([[255]], dtype=np.uint8))
    assert extreme["maximum_absolute_error"] == 255
    identical, _ = measure(reference, reference)
    assert identical["exact"] and identical["changed_samples"] == 0
    return {"status": "READER_CONTROLS_PASS", "controls": ["signed8bit_errors", "hand_metrics", "aggregation", "YUYV_order", "I420_order", "no_uint8_wrap", "identity"]}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--candidate")
    parser.add_argument("--reference")
    parser.add_argument("--format", choices=("yuyv", "i420"), default="yuyv")
    parser.add_argument("--frames", type=int, choices=(30, 90))
    parser.add_argument("--planar-luma")
    parser.add_argument("--require-identical", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()
    if args.self_test:
        if any((args.candidate, args.reference, args.frames, args.planar_luma, args.output, args.require_identical)):
            parser.error("self-test accepts no data/output/control arguments")
        print(json.dumps(self_test(), sort_keys=True))
        return 0
    if not all((args.candidate, args.reference, args.frames, args.output)):
        parser.error("candidate,reference,frames,output are required")
    # Exclusive creation preserves old failures/results and cannot truncate inputs.
    with open(args.output, "x", encoding="utf-8") as output:
        try:
            result = compare(args)
        except Exception as error:
            result = {"schema": "crystalhd.issue529.pixel-comparison.v1", "status": "FAIL",
                "exception": type(error).__name__, "error": str(error),
                "candidate": args.candidate, "reference": args.reference,
                "frames_requested": args.frames, "no_quality_or_decoder_claim": True}
        json.dump(result, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")
    return 0 if result["status"] == "MEASURED" else 1

if __name__ == "__main__":
    sys.exit(main())
