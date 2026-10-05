#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Freeze and score two effective native horizontal-path hypotheses; no hardware.

The observed unsigned12 table is interpreted as signed/1024 only as a hypothesis.
All OLD ramp-only zero-error mappings survive, fixed globally across fresh frames.
H1 clips H0 to the min/max of the SAME 16 input samples, not a fitted neighborhood.
This does not establish physical pre-SCL pixels, active bank or standalone use.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import sys

LEFT, RIGHT = 24, 296
SOURCE_WIDTH, SOURCE_HEIGHT = 640, 360
TARGET_WIDTH, TARGET_HEIGHT = 320, 180
FRESH_FRAMES = 24
MAP_SHA = "7a671a34f99f1b58c8ba09685d66222a74df161c2220148951a2294f3db5c95d"
TABLE_SHA = "a5174c0486b035d0ed23bc7188aa78a9fec1b591f76662984470b39ba2ff775a"
TRAIN_SOURCE_SHA = "021b6736caed04600c4801ca1b0e30dc4a48b60985e2dff02d38bea7c2aa9244"
TRAIN_TARGET_SHA = "1e46376052468a3dbb7c0deb2d1cece9d8242954f97b52c934cf658ce01f3935"
RULES = {
    "representation": "hypothetical signed12 / 1024; not certified hardware arithmetic",
    "H0": "8-bit-saturate(floor((dot(taps,support)+bias)/1024))",
    "H1": "8-bit-saturate(min(max(floor((dot(taps,support)+bias)/1024),min(support)),max(support)))",
    "support": "16 native-reference samples at 2*x+offset .. 2*x+offset+15",
    "interior": [LEFT, RIGHT],
    "samples": "one central row, even frames only; duplicates and all interior rows are admission gates",
    "training": "OLD frame6 only; 8 phases, both orientations, offsets -20..4, biases 0/512/1023; keep all zero-error mappings",
    "selection": "one global candidate per model across every fresh frame; no refitting",
    "input": "24 all-I progressive native MPEG2 frames; sequential input tokens; no I/P/B generalization",
}


class AdmissionError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise AdmissionError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def regular(path):
    path = Path(path)
    require(stat.S_ISREG(path.lstat().st_mode), "not a regular file: " + str(path))
    return path.read_bytes()


def capture(path, width, height, count, fresh=False, requested_width=None):
    path = Path(path)
    data = regular(path)
    size = count * width * height * 2
    require(len(data) == size, "capture byte count")
    text = regular(path.with_suffix(".log")).decode("utf-8")
    if fresh:
        require(regular(path.with_suffix(".err")) == b"", "native stderr")
    require(not re.search(r"\bresult=FAIL\b|\btransport-result=FAIL\b|\bcleanup=FAIL\b", text), "failed native log")
    old_completion = "YUY2 capture: frames=%d/%d bytes=%d/%d result=PASS" % (count, count, size, size)
    new_completion = "Packed capture: requested-format=YUY2 frames=%d/%d bytes=%d/%d transport-result=PASS" % (count, count, size, size)
    completion = [line for line in text.splitlines() if line.startswith(("YUY2 capture:", "Packed capture:"))]
    require(len(completion) == 1 and completion[0] in ((new_completion,) if fresh else (old_completion, new_completion)), "capture completion")
    drain = re.findall(r"^Library drain: iteration=1/1 frames=(\d+)/(\d+) pending=0 firmware-EOS=yes output-marker=\w+ ready=0 cleanup=PASS result=PASS$", text, re.M)
    require(drain == [(str(count), str(count))], "native EOS/retirement/cleanup")
    churn = re.findall(r"^Library churn: completed=1/1 .* fds=(\d+)/(\d+) threads=(\d+)/(\d+) result=PASS$", text, re.M)
    require(len(churn) == 1 and churn[0][0] == churn[0][1] and churn[0][2] == churn[0][3], "resource churn")
    reported_hash = re.findall(r"^Scaler test: iteration=1/1 requested-width=(\d+) expected-output=(\d+)x(\d+) yuy2-sha256=([0-9a-f]{64}) result=PASS$", text, re.M)
    require(len(reported_hash) == 1 and reported_hash[0][0] in (("0", "640") if width == 640 else ("320",)) and reported_hash[0][1:] == (str(width), str(height), sha(data)), "capture digest/geometry")
    if requested_width is not None:
        require(reported_hash[0][0] == str(requested_width), "requested scale width")
    metadata = []
    for line in text.splitlines():
        if line.startswith("Captured "):
            require(line.startswith("Captured packed output: requested-format=YUY2 ") if fresh else line.startswith(("Captured YUY2: ", "Captured packed output: requested-format=YUY2 ")), "captured output format")
            metadata.append(dict(re.findall(r"([\w-]+)=([\w]+)", line)))
    require(len(metadata) == count, "metadata count")
    first_picture = int(metadata[0].get("picture-number", "-1"))
    require(first_picture >= 0, "picture number")
    for index, frame in enumerate(metadata):
        require(frame.get("frame-index") == str(index) and frame.get("token") == str((index + 1) * 100000) and frame.get("picture-number") == str(first_picture + index), "frame identity/order")
        require(frame.get("geometry") == "%dx%d" % (width, height) and frame.get("flags") == "0" and frame.get("chroma-format") == "1", "progressive YUY2 metadata")
    stride = width * height * 2
    frames = [data[index * stride:(index + 1) * stride] for index in range(count)]
    return data, frames, {"bytes": size, "sha256": sha(data), "log_sha256": sha(text.encode()), "requested_width": int(reported_hash[0][0]), "metadata": metadata}


def profile(frame, width, height):
    require(len(frame) == width * height * 2, "profile geometry")
    return list(frame[height // 2 * width * 2:(height // 2 + 1) * width * 2:2])


def horizontal_admission(frames, width, height):
    require(len(frames) == FRESH_FRAMES, "fresh frame count")
    for index, frame in enumerate(frames):
        require(frame[1::2] == bytes([128]) * (width * height), "neutral chroma")
        if index % 2 == 0:
            require(frame == frames[index + 1], "full-byte duplicate frame")
        center = bytes(profile(frame, width, height))[24:width - 24]
        for row in range(24, height - 24):
            actual = frame[(row * width + 24) * 2:(row * width + width - 24) * 2:2]
            require(actual == center, "transverse luma variation")
    profiles = [profile(frames[index], width, height) for index in range(0, FRESH_FRAMES, 2)]
    require(len(set(profiles[0][24:width - 24])) == 1 and len(set(profiles[1][24:width - 24])) == 1, "DC controls")
    return profiles


def observed_taps(path):
    encoded = regular(path)
    require(sha(encoded) == MAP_SHA, "frozen observed coefficient fixture")
    source = json.loads(encoded)
    tables = []
    for session in source["sessions"]:
        for raw in session["raw_hex_words"]:
            words = [int(word, 16) for word in raw.split()]
            require(len(words) == 216, "observed map length")
            tables.append(words[23:215])
    require(len(tables) == 12 and all(table == tables[0] for table in tables), "observed table equality")
    require(sha(b"".join(word.to_bytes(4, "little") for word in tables[0])) == TABLE_SHA, "observed table digest")
    fields = []
    for word in tables[0][64:128]:
        require(word & 0xc003c003 == 0, "coefficient reserved bits")
        fields.extend(((word >> 18) & 4095, (word >> 2) & 4095))
    taps = [[value if value < 2048 else value - 4096 for value in fields[phase * 16:(phase + 1) * 16]] for phase in range(8)]
    require(all(sum(row) == 1024 for row in taps), "hypothetical DC normalization")
    return taps


def predict(source, taps, candidate, clamp):
    require(len(source) == SOURCE_WIDTH and all(type(value) is int and 0 <= value <= 255 for value in source), "predict source bytes")
    require(len(taps) == 8 and all(len(row) == 16 and all(type(value) is int and -2048 <= value <= 2047 for value in row) for row in taps), "predict tap shape")
    require(set(candidate) == {"phase", "reverse", "offset", "bias"} and type(candidate["phase"]) is int and 0 <= candidate["phase"] < 8 and type(candidate["reverse"]) is bool and type(candidate["offset"]) is int and -20 <= candidate["offset"] <= 4 and type(candidate["bias"]) is int and candidate["bias"] in (0, 512, 1023) and type(clamp) is bool, "predict candidate")
    coefficients = taps[candidate["phase"]][::-1] if candidate["reverse"] else taps[candidate["phase"]]
    answer = []
    for x in range(LEFT, RIGHT):
        begin = 2 * x + candidate["offset"]
        support = source[begin:begin + 16]
        require(begin >= 0 and len(support) == 16, "support bounds")
        value = (sum(a * b for a, b in zip(coefficients, support)) + candidate["bias"]) // 1024
        if clamp:
            value = min(max(value, min(support)), max(support))
        answer.append(min(255, max(0, value)))
    return answer


def candidates(taps, ramp, target):
    require(len(target) == TARGET_WIDTH and all(type(value) is int and 0 <= value <= 255 for value in target), "training target bytes")
    result = []
    for phase in range(8):
        for reverse in (False, True):
            for offset in range(-20, 5):
                for bias in (0, 512, 1023):
                    candidate = {"phase": phase, "reverse": reverse, "offset": offset, "bias": bias}
                    if predict(ramp, taps, candidate, False) == target[LEFT:RIGHT]:
                        result.append(candidate)
    require(len(result) == 30, "all 30 OLD ramp-only zero-error candidates required")
    return result


def predictions(taps, family, profiles):
    return [{"candidate": candidate, "H0": [predict(row, taps, candidate, False) for row in profiles], "H1": [predict(row, taps, candidate, True) for row in profiles]} for candidate in family]


def freeze(args):
    taps = observed_taps(args.coefficients)
    _, old_source, old_source_receipt = capture(args.training_source, 640, 360, 32)
    _, old_target, old_target_receipt = capture(args.training_scaled, 320, 180, 32)
    require(old_source_receipt["sha256"] == TRAIN_SOURCE_SHA and old_target_receipt["sha256"] == TRAIN_TARGET_SHA, "known OLD training capture digests")
    ramp = profile(old_source[6], 640, 360)
    target = profile(old_target[6], 320, 180)
    require(old_source[6] == old_source[7] and old_target[6] == old_target[7], "training duplicate")
    family = candidates(taps, ramp, target)
    reference, source_frames, source_receipt = capture(args.reference, 640, 360, FRESH_FRAMES, True, 0)
    identity, identity_frames, identity_receipt = capture(args.identity, 640, 360, FRESH_FRAMES, True, 640)
    require(reference == identity, "fresh native zero/identity full-byte equality")
    profiles = horizontal_admission(source_frames, 640, 360)
    require(profiles == horizontal_admission(identity_frames, 640, 360), "identity profiles")
    frozen = predictions(taps, family, profiles)
    separation = [sum(a != b for row0, row1 in zip(entry["H0"], entry["H1"]) for a, b in zip(row0, row1)) for entry in frozen]
    require(all(separation), "inconclusive design: H0/H1 coincide")
    return {"schema": 1, "rules": RULES, "coefficient_fixture_sha256": MAP_SHA, "observed_table_sha256": TABLE_SHA, "taps_hypothesis": taps, "training_source": old_source_receipt, "training_scaled": old_target_receipt, "training_ramp": ramp, "training_target": target, "reference": source_receipt, "identity": identity_receipt, "profiles": profiles, "predictions": frozen, "paired_model_separation_samples": separation, "scaled_samples_seen": False}


def validate_frozen(frozen):
    require(frozen["schema"] == 1 and frozen["rules"] == RULES and frozen["scaled_samples_seen"] is False, "frozen rules")
    require(frozen["coefficient_fixture_sha256"] == MAP_SHA and frozen["observed_table_sha256"] == TABLE_SHA, "frozen coefficient provenance")
    require(frozen["training_source"]["sha256"] == TRAIN_SOURCE_SHA and frozen["training_scaled"]["sha256"] == TRAIN_TARGET_SHA, "frozen OLD training digests")
    require(frozen["reference"]["requested_width"] == 0 and frozen["identity"]["requested_width"] == 640 and frozen["reference"]["sha256"] == frozen["identity"]["sha256"], "frozen zero/identity contrast")
    taps = frozen["taps_hypothesis"]
    require(len(taps) == 8 and all(len(row) == 16 and all(type(value) is int and -2048 <= value <= 2047 for value in row) and sum(row) == 1024 for row in taps), "frozen tap shape")
    require(taps == observed_taps(Path(__file__).parent / "fixtures/issue92/native-scl-filter-map.json"), "frozen observed taps changed")
    profiles = frozen["profiles"]
    require(len(profiles) == 12 and all(len(row) == 640 and all(type(value) is int and 0 <= value <= 255 for value in row) for row in profiles), "frozen profile shape")
    family = candidates(taps, frozen["training_ramp"], frozen["training_target"])
    rebuilt = predictions(taps, family, profiles)
    require(frozen["predictions"] == rebuilt, "predictions or candidate family altered")
    separation = [sum(a != b for row0, row1 in zip(entry["H0"], entry["H1"]) for a, b in zip(row0, row1)) for entry in rebuilt]
    require(all(separation) and frozen["paired_model_separation_samples"] == separation, "frozen separation")
    return rebuilt


def errors(predicted, actual):
    require(len(predicted) == len(actual) == 12 and all(len(row) == RIGHT - LEFT for row in predicted + actual), "score shape")
    per_frame = []
    for index, (first, second) in enumerate(zip(predicted, actual)):
        delta = [abs(a - b) for a, b in zip(first, second)]
        per_frame.append({"frame": index * 2, "absolute_error_sum": sum(delta), "max_error": max(delta), "different_samples": sum(value != 0 for value in delta)})
    return {"absolute_error_sum": sum(row["absolute_error_sum"] for row in per_frame), "max_error": max(row["max_error"] for row in per_frame), "different_samples": sum(row["different_samples"] for row in per_frame), "per_frame": per_frame}


def classify(scores):
    exact = {model: sum(row[model]["absolute_error_sum"] == 0 for row in scores) for model in ("H0", "H1")}
    if exact["H0"] and exact["H1"]:
        result = "nonidentifiable: both frozen effective-path families have exact candidates"
    elif exact["H0"] or exact["H1"]:
        result = "only %s has exact candidates on these fresh samples; effective-path result only" % ("H0" if exact["H0"] else "H1")
    else:
        result = "neither frozen model family is exact; these hypotheses are rejected, hardware arithmetic remains unidentified"
    return exact, result


def score(args):
    encoded = regular(args.frozen)
    require(sha(encoded) == args.expected_freeze_sha256, "preregistered freeze digest changed")
    frozen = json.loads(encoded)
    fixed = validate_frozen(frozen)
    scaled, frames, receipt = capture(args.scaled, 320, 180, FRESH_FRAMES, True, 320)
    repeat, _, repeated_receipt = capture(args.repeat, 320, 180, FRESH_FRAMES, True, 320)
    require(scaled == repeat, "fresh scaled full-byte repeat")
    target = horizontal_admission(frames, 320, 180)
    for index in (0, 1):
        require(target[index][LEFT:RIGHT] == frozen["profiles"][index][LEFT:RIGHT], "effective DC mismatch")
    actual = [row[LEFT:RIGHT] for row in target]
    scores = [{"candidate": entry["candidate"], "H0": errors(entry["H0"], actual), "H1": errors(entry["H1"], actual)} for entry in fixed]
    exact, result = classify(scores)
    return {"schema": 1, "frozen_sha256": sha(encoded), "scope": "effective stock decoder-mediated horizontal path only", "admission": "complete native frames, EOS, retirement, cleanup, unchanged resources, hashes, neutral chroma, duplicate frames, transverse invariance, DC and full-byte repeat passed", "scaled": receipt, "repeat": repeated_receipt, "scores": scores, "exact_global_candidates": exact, "conclusion": result, "unique_scored_frames": 12, "unique_scored_samples": 12 * (RIGHT - LEFT), "actual_profiles": target, "limits": "No proof of physical pre-SCL plane, active coefficient latch, signedness, fixed point, intermediate precision, clamp neighborhood, arbitrary coefficients or standalone execution. Lower error is not exact agreement."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("freeze")
    for name in ("coefficients", "training-source", "training-scaled", "reference", "identity"):
        prepare.add_argument("--" + name, required=True)
    evaluate = commands.add_parser("score")
    for name in ("frozen", "expected-freeze-sha256", "scaled", "repeat"):
        evaluate.add_argument("--" + name, required=True)
    args = parser.parse_args()
    try:
        result = freeze(args) if args.command == "freeze" else score(args)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (AdmissionError, OSError, ValueError, KeyError, TypeError, IndexError) as error:
        print("Native scaler model admission failed: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
