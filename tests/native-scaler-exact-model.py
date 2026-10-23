#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline reproduction of distinct decoder-mediated 2:1 SCL experiments.

The initial 1200 H0/H1 candidates use the previously observed coefficient
table as a hypothesis, not a proved active bank. The four-tap rule was derived
POST HOC from initial data; only its eight fixed rounding biases were frozen
before independent holdout capture. Never reinterpret that as an initial
preregistration. FULL packed holdout agreement is primary; interior agreement
alone does not pass. Neither path proves standalone SCL, physical source
ownership, vertical filtering, arbitrary ratios or decoder lifecycle safety.
Support scoring preserves the separately frozen locality contract; it does
not infer a physical filter from a locality rejection or refit coefficients.

Use separate new output paths outside the checkout for generated predictions.
Transport/EOS/resource/recovery validation remains in the native drain logs.
This program never opens hardware; no scoring operation changes a hypothesis.
"""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import importlib.util
import itertools
import json
from pathlib import Path
import stat
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
W, H, N, SW, SH, LEFT, RIGHT = 256, 96, 24, 128, 48, 24, 104
SCALED_BYTES = N * SW * SH * 2
SCOPE = ("effective decoder-mediated 256x96->128x48 horizontal rule only; "
         "no active bank, physical lease, arbitrary ratio, standalone or vertical filter claim")
INITIAL_RULES = {
    "H0": "clip8(floor((dot(taps,support)+bias)/1024))",
    "H1": "clip8(clamp(H0_unclipped,min(support),max(support)))",
    "sampling": "support[2*x+offset:2*x+offset+16]; all even-index duplicate representatives; all rows admitted",
    "family": "all8phases x2orientations x25offsets(-20..4) x3biases(0,512,1023); one global exact candidate, no postcapture fitting"}
HOLDOUT_RULE = {
    "weights": [1, 3, 3, 1], "divisor": 8, "source_origin": "2*x-1",
    "borders": "clamp each source x to [0,255]", "biases": list(range(8)),
    "rounding": "floor((weighted_sum+bias)/8)", "chroma": 128,
    "vertical": "all source/output rows identical; not a vertical-filter claim"}
NAMES = {
    "initial": ["dc96", "dc160", "ramp64to192", "even-plus16", "even-minus16",
                "odd-plus16", "odd-minus16", "even-plus32", "even-minus32",
                "odd-plus32", "odd-minus32", "asymmetric-step96-160-128"],
    "holdout": ["dc0", "dc255", "xorshift32-shuffle-seed43524844-full0to255",
                "even-plus4", "even-minus4", "odd-plus12", "odd-minus12",
                "even-plus3", "even-minus3", "odd-plus7", "odd-minus7",
                "asymmetric-step0-255-17"],
    "support": ["ramp-x", "reverse255-x", "ramp-even-plus32", "ramp-even-minus32",
                "ramp-odd-plus32", "ramp-odd-minus32", "ramp-even-plus64", "ramp-even-minus64",
                "ramp-odd-plus64", "ramp-odd-minus64", "flat128", "flat128-even-plus32"]}
PAIRS = ((2, 0, 128, 32), (3, 0, 128, -32), (4, 0, 129, 32), (5, 0, 129, -32),
         (6, 0, 128, 64), (7, 0, 128, -64), (8, 0, 129, 64), (9, 0, 129, -64),
         (11, 10, 128, 32))
ALLOWED = {128: (63, 64), 129: (64, 65)}
SUPPORT_HYPOTHESIS = ("At fixed output x and fixed nominal scaler configuration, output is a deterministic "
    "stateless function ONLY of the four source samples [2*x-1,2*x,2*x+1,2*x+2]. "
    "The function may be nonlinear and its weights/rounding are unspecified.")
SUPPORT_INTERPRETATION = ("No outside-support response retains but does not prove this locality hypothesis. "
    "A response outside support rejects this fixed stateless support, not a physical block, coefficient bank, "
    "linearity or a specific alternative algorithm. Reverse-ramp and flat diagnostics do not rescue a rejected model.")


def need(ok, message):
    if not ok:
        raise ValueError(message)


def read(path):
    path = Path(path)
    need(stat.S_ISREG(path.lstat().st_mode), "regular nonsymlink input required")
    return path.read_bytes()


def save(path, value):
    with Path(path).open("x", encoding="utf-8") as output:
        json.dump(value, output, separators=(",", ":"), sort_keys=True)
        output.write("\n")


def prior_model():
    spec = importlib.util.spec_from_file_location("prior_scl", HERE / "native-scaler-model.py")
    prior = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prior)
    return prior


def expected_row(case, index):
    row = [128] * W
    if case == "support":
        row = list(reversed(range(W))) if index == 1 else list(range(W)) if index < 10 else row
        for profile, _baseline, at, delta in PAIRS:
            if index == profile:
                row[at] += delta
        return row
    if index < 2:
        return [([96, 160] if case == "initial" else [0, 255])[index]] * W
    if index == 2:
        if case == "initial":
            return [64 + 128 * x // (W - 1) for x in range(W)]
        row = list(range(W))
        state = 0x43524844
        for i in range(W - 1, 0, -1):
            state ^= (state << 13) & 0xffffffff
            state ^= state >> 17
            state ^= (state << 5) & 0xffffffff
            j = state % (i + 1)
            row[i], row[j] = row[j], row[i]
        return row
    if index == 11:
        return ([96] * 128 + [160] * 8 + [128] * 120 if case == "initial"
                else [0] * 127 + [255] * 10 + [17] * 119)
    group = (index - 3) // 2
    amplitude = ([16, 16, 32, 32] if case == "initial" else [4, 12, 3, 7])[group]
    row[128 + (group & 1)] += amplitude if index & 1 else -amplitude
    return row


def validate_source(source, case):
    need(case in NAMES and source["schema"] == 1 and
         (source["width"], source["height"], source["frames"], source["fps"]) == (W, H, N, 30) and
         source["neutral_chroma"] == 128 and source["offline_mock_only"] is True and
         source["hardware_access"] is False, "exact offline fixture contract")
    rows = source["profiles"]
    need(len(rows) == 12, "twelve profiles")
    for index, profile in enumerate(rows):
        need(profile["id"] == index and profile["name"] == NAMES[case][index] and
             len(profile["luma"]) == W and
             all(type(v) is int and 0 <= v <= 255 for v in profile["luma"]) and
             profile["luma"] == expected_row(case, index), "fixed fixture case/profile bytes")
    return source


def known_raster(source):
    return b"".join((bytes(p["luma"]) * H + bytes([128]) * (W * H // 2)) * 2
                    for p in source["profiles"])


def native_admit(planar, packed):
    frame, y = W * H * 3 // 2, W * H
    need(len(planar) == N * frame and len(packed) == N * y * 2,
         "complete native capture sizes")
    for n in range(N):
        source, actual = planar[n*frame:(n+1)*frame], packed[n*y*2:(n+1)*y*2]
        need(source[y:] == bytes([128]) * (frame-y) and actual[::2] == source[:y] and
             actual[1::2] == bytes([128]) * y, "native full-byte known-raster mismatch")


def admit_controls(frozen, planar, before, after):
    need(planar == known_raster(frozen["source"]), "known raster differs from frozen source")
    need(hashlib.sha256(planar).hexdigest() == frozen["known_yuv420p_sha256"],
         "frozen known-raster identity")
    native_admit(planar, before)
    native_admit(planar, after)
    need(before == after, "A/B/A native recovery pixels")


def predict_initial(row, taps, candidate, clamp):
    phase, reverse, offset, bias = candidate
    coefficients = taps[phase][::-1] if reverse else taps[phase]
    output = []
    for x in range(LEFT, RIGHT):
        start = 2*x + offset
        support = row[start:start+16]
        need(start >= 0 and len(support) == 16, "bounded FIR support")
        value = (sum(a*b for a, b in zip(coefficients, support)) + bias) // 1024
        if clamp:
            value = min(max(value, min(support)), max(support))
        output.append(min(255, max(0, value)))
    return bytes(output).hex()


def initial_family(source, taps):
    return [{"candidate": list(c), **{
        name: [predict_initial(p["luma"], taps, c, clamp) for p in source["profiles"]]
        for name, clamp in (("H0", False), ("H1", True))}}
        for c in itertools.product(range(8), (False, True), range(-20, 5), (0, 512, 1023))]


def validate_initial(frozen, coefficients):
    validate_source(frozen["source"], "initial")
    prior = prior_model()
    taps = prior.observed_taps(coefficients)
    need(frozen["schema"] == 1 and frozen["scaled_pixels_seen"] is False and
         frozen["old_active_bank_assumed"] is False and frozen["interior"] == [LEFT, RIGHT] and
         (frozen["source_width"], frozen["source_height"], frozen["target_width"], frozen["target_height"]) ==
         (W, H, SW, SH) and frozen["rules"] == INITIAL_RULES and
         frozen["coefficient_fixture_sha256"] == prior.MAP_SHA and
         frozen["taps_signed12_div1024_hypothesis"] == taps, "unchanged initial hypothesis contract")
    family = initial_family(frozen["source"], taps)
    need(len(family) == 1200 and frozen["predictions"] == family and
         frozen["discriminating_candidates"] == sum(p["H0"] != p["H1"] for p in family),
         "initial frozen family or predictions changed")
    return family


def row_prediction(row, bias):
    need(len(row) == W and type(bias) is int and bias in range(8), "four-tap row/bias")
    return bytes((sum(a*row[min(W-1, max(0, 2*x-1+k))]
                      for k, a in enumerate((1, 3, 3, 1))) + bias) // 8 for x in range(SW))


def full_prediction(source, bias):
    result = bytearray()
    for profile in source["profiles"]:
        row = bytearray([128]) * (SW*2)
        row[::2] = row_prediction(profile["luma"], bias)
        result.extend(bytes(row) * SH * 2)
    need(len(result) == SCALED_BYTES, "full prediction extent")
    return bytes(result)


def validate_holdout(frozen, directory):
    validate_source(frozen["source"], "holdout")
    need(frozen["schema"] == 1 and frozen["holdout_scaled_pixels_seen"] is False and
         frozen["hypothesis_origin"] == "POSTHOC_INITIAL_DATA; independent holdout predictions preregistered" and
         frozen["rule"] == HOLDOUT_RULE and frozen["predictions"] == [
             {"bias": b, "file": "prediction-bias%d.yuy2" % b, "bytes": SCALED_BYTES}
             for b in range(8)], "unchanged frozen holdout rule")
    predictions = []
    for item in frozen["predictions"]:
        literal = read(Path(directory) / item["file"])
        need(literal == full_prediction(frozen["source"], item["bias"]),
             "holdout literal prediction changed")
        predictions.append(literal)
    need(len(set(predictions)) == 8, "all eight biases must be distinguishable")
    return predictions


def differences(actual, expected):
    need(len(actual) == len(expected) and len(actual) > 0, "comparison extent")
    errors = [abs(a-b) for a, b in zip(actual, expected)]
    return {"different_bytes": sum(v != 0 for v in errors), "sum_abs": sum(errors),
            "max_abs": max(errors), "bytes": len(errors)}


def interior(data):
    return b"".join(data[(i*SW+LEFT)*2:(i*SW+RIGHT)*2] for i in range(N*SH))


def support_contract():
    """Fixed validation values, not a newly generated preregistration record."""
    profiles = [{"id": 0, "row": "x for x=0..255", "role": "ramp baseline"},
                {"id": 1, "row": "255-x for x=0..255", "role": "reverse-ramp diagnostic, not the perturbation baseline"}]
    profiles.extend({"id": p, "base": b, "source_x": x, "delta": d} for p, b, x, d in PAIRS[:-1])
    profiles.append({"id": 10, "row": "constant128", "role": "flat baseline"})
    profiles.append({"id": 11, "base": 10, "source_x": 128, "delta": 32})
    return {"schema": 1, "frozen_before_support_scaled_capture": True,
            "geometry": {"source_width": W, "source_height": H, "output_width": SW,
                         "output_height": SH, "fps": 30, "frames": N, "neutral_chroma": 128},
            "profile_contract": profiles, "hypothesis": SUPPORT_HYPOTHESIS,
            "allowed_differential_support": [
                {"source_x": x, "output_x": list(ALLOWED[x]),
                 "profile_pairs": [[p, b] for p, b, at, _d in PAIRS if at == x]} for x in (128, 129)],
            "interpretation": SUPPORT_INTERPRETATION,
            "native_execution_by_generator": False, "production_change": False}


def validate_support(rules, source):
    validate_source(source, "support")
    need(all(rules[key] == value for key, value in support_contract().items()) and
         rules["frozen_before_support_scaled_capture"] is True and
         rules["native_execution_by_generator"] is False and rules["production_change"] is False,
         "unchanged frozen support contract")
    for p, baseline, at, delta in PAIRS:
        first, second = source["profiles"][p]["luma"], source["profiles"][baseline]["luma"]
        need([x for x in range(W) if first[x] != second[x]] == [at] and
             first[at] - second[at] == delta, "one unclipped declared source-column delta")


def support_result(data, source, rules):
    need(len(data) == SCALED_BYTES, "complete support scaled bytes")
    rows, stride = [], SW*SH*2
    for n in range(N):
        frame = data[n*stride:(n+1)*stride]
        need(frame[1::2] == bytes([128]) * (SW*SH), "scaled support neutral chroma")
        first = frame[:SW*2]
        need(all(frame[y*SW*2:(y+1)*SW*2] == first for y in range(SH)),
             "full-width invariance of all scaled rows")
        if n % 2 == 0:
            need(frame == data[(n+1)*stride:(n+2)*stride], "full-byte support duplicate pair")
            rows.append(list(first[::2]))
    responses, total = [], 0
    for p, baseline, at, delta in PAIRS:
        vector = [a-b for a, b in zip(rows[p], rows[baseline])]
        nonzero = [x for x, value in enumerate(vector) if value]
        outside = [{"x": x, "delta": vector[x]} for x in nonzero if x not in ALLOWED[at]]
        count = len(outside)*SH*2
        total += count
        responses.append({"profile": p, "baseline": baseline, "source_x": at,
                          "input_delta": delta, "allowed_output_x": list(ALLOWED[at]),
                          "signed_luma_delta128": vector, "nonzero_x": nonzero,
                          "outside_support": outside, "outside_luma_bytes_all_rows_copies": count})
    return {"result": "STATELESS_FOUR_LOCAL_SUPPORT_REJECTED" if total else
                      "STATELESS_FOUR_LOCAL_SUPPORT_RETAINED_NOT_PROVED",
            "scaled_frames": N, "scaled_duplicate_pairs": 12, "scaled_all_rows_invariant": True,
            "scaled_chroma128": True,
            "observed_y_rows": [{"profile": n, "name": source["profiles"][n]["name"], "luma128": row}
                                for n, row in enumerate(rows)],
            "responses": responses, "outside_luma_bytes_total": total,
            "outside_count_scope": "sum over9 profile/baseline comparisons, not unique capture-byte union",
            "physical_block_or_linear_coefficient_claim": False, "interpretation": rules["interpretation"],
            "independent_software_decode": "EXTERNAL_BYTE_COMPARISON_REQUIRED_NOT_VALIDATED_HERE"}


def freeze(args):
    encoded = read(args.profiles)
    source = validate_source(json.loads(encoded), args.case)
    known = read(args.known)
    need(known == known_raster(source), "known bytes/profile equality")
    common = {"schema": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
              "source": source, "known_yuv420p_sha256": hashlib.sha256(known).hexdigest(),
              "scope": SCOPE}
    if args.case == "initial":
        prior = prior_model()
        taps = prior.observed_taps(args.coefficients)
        family = initial_family(source, taps)
        save(args.output, {**common, "scaled_pixels_seen": False,
             "source_profiles_sha256": hashlib.sha256(encoded).hexdigest(),
             "source_width": W, "source_height": H, "target_width": SW, "target_height": SH,
             "interior": [LEFT, RIGHT], "coefficient_fixture_sha256": prior.MAP_SHA,
             "taps_signed12_div1024_hypothesis": taps, "old_active_bank_assumed": False,
             "rules": INITIAL_RULES,
             "discriminating_candidates": sum(p["H0"] != p["H1"] for p in family),
             "predictions": family})
        print("initial frozen=1200 scaled_pixels_seen=0 active_bank_assumed=0")
    else:
        directory = Path(args.output)
        directory.mkdir(mode=0o700)
        files, distinct = [], set()
        for bias in range(8):
            name = "prediction-bias%d.yuy2" % bias
            prediction = full_prediction(source, bias)
            with (directory / name).open("xb") as output:
                output.write(prediction)
            files.append({"bias": bias, "file": name, "bytes": SCALED_BYTES})
            distinct.add(prediction)
        need(len(distinct) == 8, "all eight frozen biases must be distinguishable")
        save(directory / "frozen.json", {**common,
             "hypothesis_origin": "POSTHOC_INITIAL_DATA; independent holdout predictions preregistered",
             "holdout_scaled_pixels_seen": False, "rule": HOLDOUT_RULE,
             "primary_acceptance": "one global bias exactly matches ALL 294912 packed bytes, including borders/chroma/rows/duplicates",
             "interior_status": "secondary diagnostic only; cannot pass primary acceptance",
             "predictions": files})
        print("holdout frozen=8 full_bytes_each=294912 holdout_scaled_pixels_seen=0")


def score(args):
    path = Path(args.frozen)
    frozen = json.loads(read(path))
    if args.case == "support":
        need(args.profiles is not None, "support scoring requires --profiles")
        source = json.loads(read(args.profiles))
        validate_support(frozen, source)
        control = {"source": source, "known_yuv420p_sha256": hashlib.sha256(known_raster(source)).hexdigest()}
    else:
        predictions = (validate_initial(frozen, args.coefficients) if args.case == "initial"
                       else validate_holdout(frozen, path.parent))
        control = frozen
    # No scaled file may be opened before frozen rules and BOTH controls pass.
    admit_controls(control, read(args.known), read(args.native_before), read(args.native_after))
    data = read(args.scaled)
    need(len(data) == SCALED_BYTES, "complete 24-frame scaled YUY2 extent")
    if args.case == "support":
        result = support_result(data, source, frozen)
    elif args.case == "initial":
        rows, dc = [], []
        stride = SW*SH*2
        for n in range(N):
            frame = data[n*stride:(n+1)*stride]
            need(frame[1::2] == bytes([128]) * (SW*SH), "scaled neutral chroma")
            if n % 2 == 0:
                need(frame == data[(n+1)*stride:(n+2)*stride], "full-byte duplicate pair")
            row = frame[SH//2*SW*2:(SH//2+1)*SW*2:2]
            need(all(frame[(y*SW+LEFT)*2:(y*SW+RIGHT)*2:2] == row[LEFT:RIGHT]
                     for y in range(SH)), "interior transverse invariance")
            if n % 2 == 0:
                rows.append(row[LEFT:RIGHT])
                if n < 4:
                    need(len(set(row[LEFT:RIGHT])) == 1, "constant scaled DC")
                    dc.append(row[LEFT])
        scores, exact = [], {"H0": [], "H1": []}
        for prediction in predictions:
            item = {"candidate": prediction["candidate"]}
            for name in ("H0", "H1"):
                delta = differences(b"".join(rows), b"".join(bytes.fromhex(v) for v in prediction[name]))
                item[name] = {"different": delta["different_bytes"], "sum_abs": delta["sum_abs"],
                              "max_abs": delta["max_abs"]}
                if not delta["different_bytes"]:
                    exact[name].append(prediction["candidate"])
            scores.append(item)
        result = {"model_result": "EXACT_CANDIDATES_RETAINED" if any(exact.values()) else "BOTH_MODEL_FAMILIES_REJECTED",
                  "exact_candidates": exact, "scores": scores, "observed_dc": dc,
                  "scored_samples": 12*(RIGHT-LEFT), "scaled_full_duplicate_pairs": True,
                  "scaled_all_rows_interior_invariant": True,
                  "primary_extent": "x24..103 luma only; NOT full-frame model acceptance"}
    else:
        scores = [{"bias": b, "full": differences(data, expected),
                   "interior_secondary": differences(interior(data), interior(expected))}
                  for b, expected in enumerate(predictions)]
        exact = [s["bias"] for s in scores if not s["full"]["different_bytes"]]
        result = {"model_result": "FULL_FRAME_EXACT" if exact else "FULL_FRAME_MODEL_REJECTED",
                  "exact_full_biases": exact,
                  "exact_interior_secondary_biases": [s["bias"] for s in scores if not s["interior_secondary"]["different_bytes"]],
                  "scores": scores}
    save(args.output, {"schema": 1, "case": args.case, **result,
         "native_known_raster_full_bytes": True, "native_before_after_equal": True,
         "frames": N, "unique_profiles": 12, "scope": SCOPE,
         "transport_lifecycle_validation": "EXTERNAL_DRAIN_LOGS_REQUIRED_NOT_VALIDATED_HERE"})
    print(args.case + " model=" + result.get("model_result", result.get("result")) + "; lifecycle review external")


def self_test(_args):
    def changed(data, index):
        return data[:index] + bytes([data[index] ^ 1]) + data[index+1:]

    def rejects(action, label):
        try:
            action()
        except (ValueError, KeyError, TypeError):
            return
        raise ValueError("mutation admitted: " + label)
    identity = [[0]*7 + [1024] + [0]*8 for _ in range(8)]
    ramp, candidate = list(range(W)), (0, False, -7, 0)
    need(bytes.fromhex(predict_initial(ramp, identity, candidate, False)) ==
         bytes(ramp[2*x] for x in range(LEFT, RIGHT)), "point-filter oracle")
    overshoot = [[0]*6 + [-256, 1280] + [0]*8 for _ in range(8)]
    impulse = [128]*W
    impulse[128] = 160
    need(bytes.fromhex(predict_initial(impulse, overshoot, candidate, False))[64-LEFT] == 168 and
         bytes.fromhex(predict_initial(impulse, overshoot, candidate, True))[64-LEFT] == 160,
         "H0/H1 overshoot separation")
    rejects(lambda: predict_initial(ramp, identity, (0, False, -100, 0), False), "support")
    need(row_prediction(ramp, 0)[0] == 0 and row_prediction(ramp, 4)[0] == 1 and
         row_prediction(ramp, 4)[-1] == 254 and row_prediction(ramp, 5)[-1] == 255,
         "four-tap border/origin/bias")
    with tempfile.TemporaryDirectory(prefix="crystalhd-scl-exact-selftest-") as temp:
        directory = Path(temp)
        for case in NAMES:
            source = {"schema": 1, "width": W, "height": H, "frames": N, "fps": 30,
                      "neutral_chroma": 128, "offline_mock_only": True, "hardware_access": False,
                      "profiles": [{"id": n, "name": NAMES[case][n], "luma": expected_row(case, n)}
                                   for n in range(12)]}
            validate_source(source, case)
            planar = known_raster(source)
            packed = b"".join(b"".join(bytes((value, 128)) for value in planar[n*W*H*3//2:n*W*H*3//2+W*H])
                              for n in range(N))
            control = {"source": source, "known_yuv420p_sha256": hashlib.sha256(planar).hexdigest()}
            admit_controls(control, planar, packed, packed)
            for label, values in (("known", (changed(planar, 0), packed, packed)),
                                  ("before", (planar, changed(packed, 0), packed)),
                                  ("after", (planar, packed, changed(packed, 0))),
                                  ("known-chroma", (changed(planar, W*H), packed, packed)),
                                  ("before-chroma", (planar, changed(packed, 1), packed)),
                                  ("after-last-byte", (planar, packed, changed(packed, len(packed)-1)))):
                rejects(lambda values=values: admit_controls(control, *values), label)
            profiles, known = directory / (case+".json"), directory / (case+".yuv")
            save(profiles, source)
            known.write_bytes(planar)
            output = directory / (case+"-frozen")
            args = argparse.Namespace(case=case, profiles=profiles, known=known, output=output,
                                      coefficients=HERE / "fixtures/issue92/native-scl-filter-map.json")
            if case == "support":
                rules = support_contract()
                validate_support(rules, source)
                altered = copy.deepcopy(rules)
                altered["allowed_differential_support"][0]["output_x"].append(62)
                rejects(lambda: validate_support(altered, source), "support rule")
                frozen = directory / "support-rules.json"
                save(frozen, rules)  # Synthetic self-test only, not a native preregistration.
                native = directory / "support-native.yuy2"
                native.write_bytes(packed)
                synthetic = full_prediction(source, 4)
                outside = bytearray(synthetic)
                for n in (4, 5):  # Both copies of profile2; preserve all-row invariance.
                    for y in range(SH):
                        outside[(n*SW*SH + y*SW + 62)*2] ^= 1
                scaled = directory / "support-scaled.yuy2"
                for label, data in (("local", synthetic), ("outside", outside)):
                    scaled.write_bytes(data)
                    scored = directory / ("support-score-" + label + ".json")
                    score(argparse.Namespace(case="support", frozen=frozen, profiles=profiles,
                          known=known, native_before=native, native_after=native,
                          scaled=scaled, output=scored))
                    result = json.loads(read(scored))
                    need(result["outside_luma_bytes_total"] == (0 if label == "local" else SH*2) and
                         result["result"] == ("STATELESS_FOUR_LOCAL_SUPPORT_RETAINED_NOT_PROVED"
                                              if label == "local" else "STATELESS_FOUR_LOCAL_SUPPORT_REJECTED"),
                         "synthetic locality support and outside-response rejection")
                    need(all(len(r["signed_luma_delta128"]) == SW for r in result["responses"]),
                         "full signed support vectors retained")
                for label, data in (("support-size", synthetic[:-1]),
                                    ("support-chroma", changed(synthetic, 1)),
                                    ("support-row", changed(synthetic, SW*2))):
                    rejects(lambda data=data: support_result(data, source, rules), label)
                duplicate = bytearray(synthetic)
                for y in range(SH):
                    duplicate[y*SW*2] ^= 1
                rejects(lambda: support_result(duplicate, source, rules), "support-duplicate")
                continue
            freeze(args)
            frozen = json.loads(read(output if case == "initial" else output / "frozen.json"))
            if case == "initial":
                validate_initial(frozen, args.coefficients)
                for field in ("candidate", "H0", "H1"):
                    altered = copy.deepcopy(frozen)
                    altered["predictions"][0][field][0] = 99 if field == "candidate" else "00"
                    rejects(lambda altered=altered: validate_initial(altered, args.coefficients), field)
            else:
                validate_holdout(frozen, output)
                native = directory / "holdout-native.yuy2"
                native.write_bytes(packed)
                scaled = directory / "holdout-scaled.yuy2"
                synthetic = full_prediction(source, 4)
                for label, data in (("exact", synthetic),
                                    ("border", changed(synthetic, 0)),
                                    ("chroma", changed(synthetic, 1))):
                    scaled.write_bytes(data)
                    scored = directory / ("holdout-score-" + label + ".json")
                    score(argparse.Namespace(case="holdout", frozen=output / "frozen.json",
                          known=known, native_before=native, native_after=native,
                          scaled=scaled, output=scored))
                    result = json.loads(read(scored))
                    need(result["exact_interior_secondary_biases"] == [4],
                         "outside-interior changes preserve secondary agreement")
                    need(result["exact_full_biases"] == ([4] if label == "exact" else []) and
                         result["model_result"] == ("FULL_FRAME_EXACT" if label == "exact"
                                                   else "FULL_FRAME_MODEL_REJECTED"),
                         "only the full-frame score route can pass primary acceptance")
                altered = copy.deepcopy(frozen)
                altered["rule"]["weights"][0] += 1
                rejects(lambda: validate_holdout(altered, output), "four-tap rule")
                prediction = output / "prediction-bias0.yuy2"
                literal = read(prediction)
                prediction.write_bytes(bytes([literal[0]^1])+literal[1:])
                rejects(lambda: validate_holdout(frozen, output), "literal prediction")
    print("self-test PASS: fixed cases, point/overshoot/borders, complete native admission, frozen-family/rule/literal mutations; no device")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("self-test")
    for command, names in (("freeze", ("profiles", "known", "output")),
                           ("score", ("frozen", "known", "native-before", "native-after", "scaled", "output"))):
        sub = commands.add_parser(command)
        sub.add_argument("--case", choices=("initial", "holdout") if command == "freeze"
                         else tuple(NAMES), required=True)
        if command == "score":
            sub.add_argument("--profiles", help="required only for support scoring")
        sub.add_argument("--coefficients", default=HERE / "fixtures/issue92/native-scl-filter-map.json")
        for name in names:
            sub.add_argument("--" + name, required=True)
    args = parser.parse_args()
    try:
        {"self-test": self_test, "freeze": freeze, "score": score}[args.command](args)
        return 0
    except (ValueError, OSError, KeyError, TypeError, IndexError) as error:
        print("Exact scaler admission FAIL: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
