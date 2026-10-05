#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Device-free admission and arithmetic tests for the effective scaler models."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("native_scaler_model", HERE / "native-scaler-model.py")
MODEL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODEL)
OBSERVED = HERE / "fixtures/issue92/native-scl-filter-map.json"

# Independent signed interpretation of the published unsigned12 HY fields.
H = [
    [4, 0, -18, 36, -2, -119, 272, 678, 272, -119, -2, 36, -18, 0, 4, 0],
    [3, 2, -19, 28, 16, -123, 197, 670, 350, -105, -23, 44, -16, -3, 5, -2],
    [2, 4, -18, 19, 31, -118, 125, 646, 425, -80, -45, 48, -12, -7, 5, -1],
    [1, 5, -16, 10, 41, -106, 59, 608, 495, -44, -68, 50, -6, -10, 6, -1],
    [0, 6, -13, 1, 48, -89, 2, 557, 557, 2, -89, 48, 1, -13, 6, 0],
    [-1, 6, -10, -6, 50, -68, -44, 495, 608, 59, -106, 41, 10, -16, 5, 1],
    [-1, 5, -7, -12, 48, -45, -80, 425, 646, 125, -118, 31, 19, -18, 4, 2],
    [-2, 5, -3, -16, 44, -23, -105, 350, 670, 197, -123, 16, 28, -19, 2, 3],
]

# Actual old frame-6 central decoded rows, not ideal generated ramp values.
# Their hashes fix the small unit fixture without committing private paths.
OLD_SOURCE = bytes.fromhex(
    "3f3f4040404041414141424242424343434344444444454544444545454546464646474747474848474748484848494949494a4a4a4a4b4b4b4b4c4c4c4c4d4d4c4c4d4d4d4d4e4e4e4e4f4f4f4f50504f4f5050505051515151525252525353535354545454555554545555555556565656575757575858575758585858595959595a5a5a5a5b5b5b5b5c5c5c5c5d5d5c5c5d5d5d5d5e5e5e5e5f5f5f5f60605f5f6060606061616161626262626363636364646464656564646565656566666666676767676868676768686868696969696a6a6a6a6b6b6b6b6c6c6c6c6d6d6c6c6d6d6d6d6e6e6e6e6f6f6f6f70706f6f7070707071717171727272727373737374747474757574747575757576767676777777777878777778787878797979797a7a7a7a7b7b7b7b7c7c7c7c7d7d7c7c7d7d7d7d7e7e7e7e7f7f7f7f80807f7f8080808081818181828282828383838384848484858584848585858586868686878787878888878788888888898989898a8a8a8a8b8b8b8b8c8c8c8c8d8d8c8c8d8d8d8d8e8e8e8e8f8f8f8f90908f8f9090909091919191929292929393939394949494959594949595959596969696979797979898979798989898999999999a9a9a9a9b9b9b9b9c9c9c9c9d9d9c9c9d9d9d9d9e9e9e9e9f9f9f9fa0a09f9fa0a0a0a0a1a1a1a1a2a2a2a2a3a3a3a3a4a4a4a4a5a5a4a4a5a5a5a5a6a6a6a6a7a7a7a7a8a8a7a7a8a8a8a8a9a9a9a9aaaaaaaaababababacacacacadadacacadadadadaeaeaeaeafafafafb0b0afafb0b0b0b0b1b1b1b1b2b2b2b2b3b3b3b3b4b4b4b4b5b5b4b4b5b5b5b5b6b6b6b6b7b7b7b7b8b8b7b7b8b8b8b8b9b9b9b9bababababbbbbbbbbcbcbcbcbdbdbcbcbdbdbdbdbebebebebfbfbfbfc0c0"
)
OLD_TARGET = bytes.fromhex(
    "404040414142424343444445444545464647474847484849494a4a4b4b4c4c4d4c4d4d4e4e4f4f504f5050515152525353545455545555565657575857585859595a5a5b5b5c5c5d5c5d5d5e5e5f5f605f6060616162626363646465646565666667676867686869696a6a6b6b6c6c6d6c6d6d6e6e6f6f706f7070717172727373747475747575767677777877787879797a7a7b7b7c7c7d7c7d7d7e7e7f7f807f8080818182828383848485848585868687878887888889898a8a8b8b8c8c8d8c8d8d8e8e8f8f908f9090919192929393949495949595969697979897989899999a9a9b9b9c9c9d9c9d9d9e9e9f9fa09fa0a0a1a1a2a2a3a3a4a4a5a4a5a5a6a6a7a7a8a7a8a8a9a9aaaaababacacadacadadaeaeafafb0afb0b0b1b1b2b2b3b3b4b4b5b4b5b5b6b6b7b7b8b7b8b8b9b9bababbbbbcbcbdbcbdbdbebebfbfc0"
)
OLD_SOURCE_SHA = "021b6736caed04600c4801ca1b0e30dc4a48b60985e2dff02d38bea7c2aa9244"
OLD_TARGET_SHA = "1e46376052468a3dbb7c0deb2d1cece9d8242954f97b52c934cf658ce01f3935"


def family():
    result = []
    for phase in range(8):
        offsets = ((-7, -6), (-8, -7)) if phase < 4 else (((-7,), (-7,)) if phase == 4 else ((-8, -7), (-7, -6)))
        for reverse, choices in zip((False, True), offsets):
            result.extend(dict(phase=phase, reverse=reverse, offset=offset, bias=512) for offset in choices)
    return result


def packed(row, height):
    line = bytes(value for luma in row for value in (luma, 128))
    return line * height


def fresh_profiles():
    return [[96] * 640, [160] * 640,
            [96 + 64 * (x >= 286) for x in range(640)],
            [160 - 64 * (x >= 287) for x in range(640)],
            [112 + 32 * (x >= 286) for x in range(640)],
            [96 + 64 * (x == 286) for x in range(640)],
            [160 - 64 * (x == 287) for x in range(640)],
            [96 + 64 * (286 <= x <= 289) for x in range(640)],
            [160 - 64 * (287 <= x <= 291) for x in range(640)],
            [96 + 64 * (x % 16 >= 8) for x in range(640)],
            [112 + 32 * (x % 6 >= 3) for x in range(640)],
            [96 + 32 * (x >= 280) + 32 * (x >= 287) - 32 * (x >= 294) - 32 * (x >= 303) for x in range(640)]]


def frames_from_profiles(profiles, height):
    return [frame for row in profiles for frame in (packed(row, height),) * 2]


def native_log(data, width, height, count, requested, legacy=False):
    size = len(data)
    metadata = []
    for index in range(count):
        prefix = "Captured YUY2: " if legacy else "Captured packed output: requested-format=YUY2 "
        metadata.append(prefix + "frame-index=%d token=%d picture-number=%d geometry=%dx%d flags=0 chroma-format=1 output-flags=20000 aspect-ratio=15 colour-primaries=0" % (index, (index + 1) * 100000, index + 3, width, height))
    done = ("YUY2 capture: frames=%d/%d bytes=%d/%d result=PASS" if legacy else "Packed capture: requested-format=YUY2 frames=%d/%d bytes=%d/%d transport-result=PASS") % (count, count, size, size)
    return "\n".join(metadata + [done,
        "Library drain: iteration=1/1 frames=%d/%d pending=0 firmware-EOS=yes output-marker=yes ready=0 cleanup=PASS result=PASS" % (count, count),
        "Library churn: completed=1/1 peak-rss-kib=100 fds=3/3 threads=1/1 result=PASS",
        "Scaler test: iteration=1/1 requested-width=%d expected-output=%dx%d yuy2-sha256=%s result=PASS" % (requested, width, height, MODEL.sha(data))]) + "\n"


class ArithmeticTests(unittest.TestCase):
    def test_actual_observed_taps(self):
        self.assertEqual(MODEL.observed_taps(OBSERVED), H)
        self.assertEqual(MODEL.sha(OLD_SOURCE), "d4974986fa086ff825bef8f4e306a17595b3d77a7e5d072a27b8c28659bd93f3")
        self.assertEqual(MODEL.sha(OLD_TARGET), "e4f8d08d672e64c57e69e33838d1607fd80f6a4f10caaa1edb40bd7966f7dbac")

    def test_old_ramp_exact_family_order(self):
        actual = MODEL.candidates(H, list(OLD_SOURCE), list(OLD_TARGET))
        self.assertEqual(actual, family())
        self.assertEqual(len(actual), 30)
        self.assertEqual({entry["bias"] for entry in actual}, {512})
        # Every rejected rounding remains searched, not presumed equivalent.
        for bias in (0, 1023):
            minimum = min(sum(abs(a - b) for a, b in zip(
                MODEL.predict(list(OLD_SOURCE), H, dict(phase=p, reverse=r, offset=o, bias=bias), False),
                OLD_TARGET[24:296])) for p in range(8) for r in (False, True) for o in range(-20, 5))
            self.assertEqual(minimum, 27)

    def test_training_target_and_no_candidates(self):
        with self.assertRaises(MODEL.AdmissionError):
            MODEL.candidates(H, list(OLD_SOURCE), list(OLD_TARGET[:-1]))
        with self.assertRaises(MODEL.AdmissionError):
            MODEL.candidates(H, list(OLD_SOURCE), [0] * 320)

    def test_floor_negative_saturation_and_same_support(self):
        source = [100] * 640
        source[48] = 200
        for coefficient, bias, expected in ((-1, 0, 0), (-1024, 512, 0), (2047, 512, 255)):
            row = [coefficient] + [0] * 15
            candidate = dict(phase=0, reverse=False, offset=0, bias=bias)
            self.assertEqual(MODEL.predict(source, [row] * 8, candidate, False)[0], expected)
        # A sum of -1 floors to -1: clamping the mathematical result precedes saturation.
        row = [-1025, 1024] + [0] * 14
        source[48:64] = [1] * 16
        candidate = dict(phase=0, reverse=False, offset=0, bias=0)
        self.assertEqual(MODEL.predict(source, [row] * 8, candidate, False)[0], 0)
        self.assertEqual(MODEL.predict(source, [row] * 8, candidate, True)[0], 1)
        source[48:64] = [80, 176] + [128] * 14
        upper = [-1024, 2047, 1] + [0] * 13
        lower = [2047, -1024, 1] + [0] * 13
        self.assertEqual(MODEL.predict(source, [upper] * 8, candidate, False)[0], 255)
        self.assertEqual(MODEL.predict(source, [upper] * 8, candidate, True)[0], 176)
        self.assertEqual(MODEL.predict(source, [lower] * 8, candidate, False)[0], 0)
        self.assertEqual(MODEL.predict(source, [lower] * 8, candidate, True)[0], 80)
        # An extreme just outside the exact 16 samples cannot affect the clamp.
        source[47], source[64] = 0, 255
        self.assertEqual(MODEL.predict(source, [upper] * 8, candidate, True)[0], 176)

    def test_source_orientation_and_support_bounds(self):
        source = list(range(256)) * 2 + list(range(128))
        row = [1024] + [0] * 15
        for reverse, first in ((False, source[41]), (True, source[56])):
            self.assertEqual(MODEL.predict(source, [row] * 8, dict(phase=0, reverse=reverse, offset=-7, bias=512), False)[0], first)
        for offset in (-49, 40):
            with self.assertRaises(MODEL.AdmissionError):
                MODEL.predict(source, H, dict(phase=0, reverse=False, offset=offset, bias=512), False)

    def test_malformed_predictor_arguments(self):
        candidate = dict(phase=0, reverse=False, offset=-7, bias=512)
        for source in ([128] * 639, [128] * 641, [True] * 640, [-1] * 640, [256] * 640):
            with self.subTest(source=source[:2], length=len(source)), self.assertRaises(MODEL.AdmissionError):
                MODEL.predict(source, H, candidate, False)
        for taps in (H[:-1], [[0] * 15] * 8, [[0] * 17] * 8):
            with self.subTest(tap_shape=len(taps)), self.assertRaises(MODEL.AdmissionError):
                MODEL.predict([128] * 640, taps, candidate, False)
        for key, value in (("phase", -1), ("phase", 8), ("reverse", 1), ("offset", 0.5), ("bias", True)):
            bad = dict(candidate, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(MODEL.AdmissionError):
                MODEL.predict([128] * 640, H, bad, False)

    def test_observed_fixture_digest_and_reserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "map.json"
            encoded = OBSERVED.read_bytes()
            path.write_bytes(encoded + b"\n")
            with self.assertRaises(MODEL.AdmissionError):
                MODEL.observed_taps(path)
            source = json.loads(encoded)
            words = [int(value, 16) for value in source["sessions"][0]["raw_hex_words"][0].split()]
            for row in ([2047, -2048, -1, 1026] + [0] * 12, [0] * 16):
                modified = list(words)
                fields = [value % 4096 for value in row] * 8
                modified[87:151] = [(fields[i] << 18) | (fields[i + 1] << 2) for i in range(0, 128, 2)]
                changed = copy.deepcopy(source)
                for session in changed["sessions"]:
                    session["raw_hex_words"] = [" ".join("%08x" % value for value in modified)] * 2
                payload = json.dumps(changed).encode()
                path.write_bytes(payload)
                table_hash = MODEL.sha(b"".join(value.to_bytes(4, "little") for value in modified[23:215]))
                with mock.patch.object(MODEL, "MAP_SHA", MODEL.sha(payload)), mock.patch.object(MODEL, "TABLE_SHA", table_hash):
                    if sum(row) == 1024:
                        self.assertEqual(MODEL.observed_taps(path)[0], row)
                    else:
                        with self.assertRaises(MODEL.AdmissionError):
                            MODEL.observed_taps(path)
            modified[87] |= 1
            for session in changed["sessions"]:
                session["raw_hex_words"] = [" ".join("%08x" % value for value in modified)] * 2
            payload = json.dumps(changed).encode()
            path.write_bytes(payload)
            table_hash = MODEL.sha(b"".join(value.to_bytes(4, "little") for value in modified[23:215]))
            with mock.patch.object(MODEL, "MAP_SHA", MODEL.sha(payload)), mock.patch.object(MODEL, "TABLE_SHA", table_hash), self.assertRaises(MODEL.AdmissionError):
                MODEL.observed_taps(path)


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "capture.yuy2"
        self.data = packed([96] * 640, 360) * 24
        self.text = native_log(self.data, 640, 360, 24, 0)
        self.path.write_bytes(self.data)
        self.path.with_suffix(".log").write_text(self.text)
        self.path.with_suffix(".err").write_bytes(b"")

    def read(self, fresh=True):
        return MODEL.capture(self.path, 640, 360, 24, fresh)

    def test_current_and_legacy_capture_grammars(self):
        for legacy in (False, True):
            self.path.with_suffix(".log").write_text(native_log(self.data, 640, 360, 24, 0, legacy))
            data, frames, receipt = self.read(fresh=not legacy)
            self.assertEqual(data, self.data)
            self.assertEqual(len(frames), 24)
            self.assertEqual(receipt["sha256"], MODEL.sha(self.data))

    def test_capture_size_regular_and_stderr(self):
        for bad in (self.data[:-1], self.data + b"\0"):
            self.path.write_bytes(bad)
            with self.assertRaises(MODEL.AdmissionError):
                self.read()
        self.path.write_bytes(self.data)
        self.path.with_suffix(".err").write_bytes(b"native warning\n")
        with self.assertRaises(MODEL.AdmissionError):
            self.read()
        link = self.path.with_name("link.yuy2")
        link.symlink_to(self.path)
        with self.assertRaises(MODEL.AdmissionError):
            MODEL.regular(link)
        with self.assertRaises(MODEL.AdmissionError):
            MODEL.regular(self.path.parent)

    def test_all_completion_digest_geometry_and_resource_gates(self):
        cases = [
            ("transport-result=PASS", "transport-result=FAIL"),
            ("frames=24/24 bytes=", "frames=23/24 bytes="),
            ("pending=0", "pending=1"), ("firmware-EOS=yes", "firmware-EOS=no"),
            ("ready=0", "ready=1"), ("cleanup=PASS", "cleanup=FAIL"),
            ("fds=3/3", "fds=3/4"), ("threads=1/1", "threads=1/2"),
            ("expected-output=640x360", "expected-output=320x180"),
            ("requested-width=0", "requested-width=320"),
            (MODEL.sha(self.data), "0" * 64),
        ]
        for before, after in cases:
            self.path.with_suffix(".log").write_text(self.text.replace(before, after, 1))
            with self.subTest(before=before), self.assertRaises(MODEL.AdmissionError):
                self.read()
        for prefix in ("Packed capture:", "Library drain:", "Library churn:", "Scaler test:"):
            line = next(line for line in self.text.splitlines() if line.startswith(prefix))
            for text in (self.text.replace(line + "\n", ""), self.text + line + "\n"):
                self.path.with_suffix(".log").write_text(text)
                with self.subTest(prefix=prefix), self.assertRaises(MODEL.AdmissionError):
                    self.read()

    def test_metadata_identity_and_format_gates(self):
        cases = [("frame-index=0", "frame-index=1"), ("token=100000", "token=200000"),
                 ("picture-number=4", "picture-number=5"), ("geometry=640x360", "geometry=320x180"),
                 ("flags=0 ", "flags=1 "), ("chroma-format=1", "chroma-format=2"),
                 ("requested-format=YUY2", "requested-format=UYVY")]
        for before, after in cases:
            self.path.with_suffix(".log").write_text(self.text.replace(before, after, 1))
            with self.subTest(before=before), self.assertRaises(MODEL.AdmissionError):
                self.read()
        first = self.text.splitlines()[0]
        for text in (self.text.replace(first + "\n", ""), self.text + first + "\n"):
            self.path.with_suffix(".log").write_text(text)
            with self.assertRaises(MODEL.AdmissionError):
                self.read()

    def test_requested_scale_mode_is_not_just_output_geometry(self):
        for requested in (0, 640):
            self.path.with_suffix(".log").write_text(native_log(self.data, 640, 360, 24, requested))
            self.assertEqual(MODEL.capture(self.path, 640, 360, 24, True, requested)[2]["requested_width"], requested)
            with self.assertRaises(MODEL.AdmissionError):
                MODEL.capture(self.path, 640, 360, 24, True, 640 if requested == 0 else 0)

    def test_fresh_cannot_fall_back_to_legacy_or_mix_formats(self):
        legacy = native_log(self.data, 640, 360, 24, 0, True)
        for text in (legacy, self.text + legacy, self.text.replace("Captured packed output:", "Captured YUY2:", 1),
                     self.text.replace("requested-format=YUY2", "requested-format=UYVY")):
            self.path.with_suffix(".log").write_text(text)
            with self.assertRaises(MODEL.AdmissionError):
                self.read()


class HorizontalTests(unittest.TestCase):
    def setUp(self):
        self.profiles = fresh_profiles()
        self.frames = frames_from_profiles(self.profiles, 360)

    def test_profile_and_admission(self):
        self.assertEqual(MODEL.horizontal_admission(self.frames, 640, 360), self.profiles)
        self.assertEqual(MODEL.profile(self.frames[4], 640, 360), self.profiles[2])
        for frame in (b"", self.frames[0][:-2], self.frames[0] + b"\0\0"):
            with self.assertRaises(MODEL.AdmissionError):
                MODEL.profile(frame, 640, 360)

    def test_count_duplicate_chroma_transverse_and_dc(self):
        for frames in (self.frames[:-1], self.frames + [self.frames[0]]):
            with self.assertRaises(MODEL.AdmissionError):
                MODEL.horizontal_admission(frames, 640, 360)
        for index, offset in ((0, 1), (5, 200), (4, (25 * 640 + 100) * 2)):
            changed = list(self.frames)
            value = bytearray(changed[index])
            value[offset] ^= 1
            changed[index] = bytes(value)
            if index == 4:
                changed[5] = changed[4]
            with self.subTest(index=index, offset=offset), self.assertRaises(MODEL.AdmissionError):
                MODEL.horizontal_admission(changed, 640, 360)
        changed = list(self.frames)
        changed[0] = changed[1] = packed([96 + (x == 100) for x in range(640)], 360)
        with self.assertRaises(MODEL.AdmissionError):
            MODEL.horizontal_admission(changed, 640, 360)


class FrozenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profiles = fresh_profiles()
        cls.predicted = MODEL.predictions(H, family(), cls.profiles)
        cls.separation = [sum(a != b for row0, row1 in zip(entry["H0"], entry["H1"]) for a, b in zip(row0, row1)) for entry in cls.predicted]
        cls.frozen = {"schema": 1, "rules": copy.deepcopy(MODEL.RULES), "coefficient_fixture_sha256": MODEL.MAP_SHA,
                      "observed_table_sha256": MODEL.TABLE_SHA, "taps_hypothesis": copy.deepcopy(H),
                      "training_ramp": list(OLD_SOURCE), "training_target": list(OLD_TARGET),
                      "training_source": {"sha256": OLD_SOURCE_SHA}, "training_scaled": {"sha256": OLD_TARGET_SHA},
                      "reference": {"sha256": "1" * 64, "requested_width": 0},
                      "identity": {"sha256": "1" * 64, "requested_width": 640},
                      "profiles": cls.profiles, "predictions": cls.predicted,
                      "paired_model_separation_samples": cls.separation, "scaled_samples_seen": False}

    def test_validate_complete_frozen_predictions(self):
        self.assertTrue(all(self.separation))
        self.assertEqual(MODEL.validate_frozen(self.frozen), self.predicted)
        self.assertEqual(len(self.predicted), 30)
        self.assertEqual(sum(len(row) for row in self.predicted[0]["H0"]), 3264)

    def test_frozen_rule_family_profile_prediction_and_provenance_mutations(self):
        mutators = [lambda f: f.update(schema=2), lambda f: f.update(scaled_samples_seen=True),
                    lambda f: f["rules"].update(selection="per pixel winner"),
                    lambda f: f.update(coefficient_fixture_sha256="0" * 64),
                    lambda f: f.update(observed_table_sha256="0" * 64),
                    lambda f: f["training_source"].update(sha256="0" * 64),
                    lambda f: f["training_scaled"].update(sha256="0" * 64),
                    lambda f: f["reference"].update(requested_width=640),
                    lambda f: f["identity"].update(requested_width=0),
                    lambda f: f["identity"].update(sha256="2" * 64),
                    lambda f: f["profiles"][2].pop(), lambda f: f["profiles"][2].__setitem__(1, 256),
                    lambda f: f["taps_hypothesis"][0].__setitem__(0, True),
                    lambda f: f["taps_hypothesis"][0].__setitem__(0, -2049),
                    lambda f: f["predictions"].pop(), lambda f: f["predictions"].reverse(),
                    lambda f: f["predictions"][0]["candidate"].update(offset=-6),
                    lambda f: f["predictions"][0]["H0"][2].__setitem__(0, 1),
                    lambda f: f["paired_model_separation_samples"].__setitem__(0, 0)]
        for index, mutate in enumerate(mutators):
            altered = copy.deepcopy(self.frozen)
            mutate(altered)
            with self.subTest(index=index), self.assertRaises(MODEL.AdmissionError):
                MODEL.validate_frozen(altered)

    def test_errors_classification_and_global_not_pixelwise_scores(self):
        zeros = [[0] * 272 for _ in range(12)]
        changed = copy.deepcopy(zeros)
        changed[3][17] = 12
        result = MODEL.errors(zeros, changed)
        self.assertEqual((result["absolute_error_sum"], result["max_error"], result["different_samples"]), (12, 12, 1))
        self.assertEqual(result["per_frame"][3]["frame"], 6)
        for bad in (zeros[:-1], [[0] * 271] * 12):
            with self.assertRaises(MODEL.AdmissionError):
                MODEL.errors(bad, zeros)
        for values, expected in (([(0, 1)], "only H0"), ([(1, 0)], "only H1"),
                                 ([(0, 1), (1, 0)], "nonidentifiable"), ([(1, 1)], "neither")):
            scores = [{"H0": {"absolute_error_sum": first}, "H1": {"absolute_error_sum": second}} for first, second in values]
            exact, text = MODEL.classify(scores)
            self.assertIn(expected, text)
            self.assertEqual(exact, {"H0": sum(a == 0 for a, _ in values), "H1": sum(b == 0 for _, b in values)})
        # Different global candidates that cover different pixels are not an exact family match.
        first, second = copy.deepcopy(zeros), copy.deepcopy(zeros)
        first[0][0], second[0][1] = 1, 1
        scores = [{"H0": MODEL.errors(row, zeros), "H1": MODEL.errors(row, zeros)} for row in (first, second)]
        self.assertEqual(MODEL.classify(scores)[0], {"H0": 0, "H1": 0})

    def test_expected_freeze_digest_precedes_any_scaled_open(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frozen.json"
            path.write_text(json.dumps(self.frozen))
            args = SimpleNamespace(frozen=path, expected_freeze_sha256="0" * 64, scaled="missing", repeat="missing")
            with mock.patch.object(MODEL, "capture") as capture, self.assertRaises(MODEL.AdmissionError):
                MODEL.score(args)
            capture.assert_not_called()

    def test_score_complete_repeated_native_data_and_global_counts(self):
        # Synthetic output is exactly one frozen global H0, not a hardware receipt.
        profiles = [[128] * 24 + row + [128] * 24 for row in self.predicted[0]["H0"]]
        frames = frames_from_profiles(profiles, 180)
        data = b"".join(frames)
        receipt = {"bytes": len(data), "sha256": MODEL.sha(data), "requested_width": 320}
        with tempfile.TemporaryDirectory() as directory:
            frozen = Path(directory) / "frozen.json"
            frozen.write_text(json.dumps(self.frozen))
            args = SimpleNamespace(frozen=frozen, expected_freeze_sha256=MODEL.sha(frozen.read_bytes()), scaled="scaled", repeat="repeat")
            with mock.patch.object(MODEL, "capture", side_effect=[(data, frames, receipt), (data, frames, receipt)]) as capture:
                result = MODEL.score(args)
            self.assertEqual(capture.call_args_list, [mock.call("scaled", 320, 180, 24, True, 320), mock.call("repeat", 320, 180, 24, True, 320)])
            self.assertEqual(len(result["scores"]), 30)
            self.assertEqual(result["unique_scored_frames"], 12)
            self.assertEqual(result["unique_scored_samples"], 3264)
            self.assertGreater(result["exact_global_candidates"]["H0"], 0)
            self.assertEqual(result["scores"][0]["H0"]["absolute_error_sum"], 0)
            self.assertEqual(result["actual_profiles"], profiles)

    def test_score_repeat_and_dc_failure(self):
        profiles = [[128] * 24 + row + [128] * 24 for row in self.predicted[0]["H0"]]
        frames = frames_from_profiles(profiles, 180)
        data = b"".join(frames)
        receipt = {"sha256": MODEL.sha(data), "requested_width": 320}
        with tempfile.TemporaryDirectory() as directory:
            frozen = Path(directory) / "frozen.json"
            frozen.write_text(json.dumps(self.frozen))
            args = SimpleNamespace(frozen=frozen, expected_freeze_sha256=MODEL.sha(frozen.read_bytes()), scaled="scaled", repeat="repeat")
            with mock.patch.object(MODEL, "capture", side_effect=[(data, frames, receipt), (data[:-1], frames, receipt)]), self.assertRaisesRegex(MODEL.AdmissionError, "full-byte repeat"):
                MODEL.score(args)
            changed = list(frames)
            changed[0] = changed[1] = packed([97] * 320, 180)
            altered = b"".join(changed)
            with mock.patch.object(MODEL, "capture", side_effect=[(altered, changed, receipt)] * 2), self.assertRaisesRegex(MODEL.AdmissionError, "DC mismatch"):
                MODEL.score(args)


class FreezeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source_frame = packed(OLD_SOURCE, 360)
        target_frame = packed(OLD_TARGET, 180)
        cls.old_source = (b"training-source", [source_frame] * 32, {"sha256": OLD_SOURCE_SHA})
        cls.old_target = (b"training-scaled", [target_frame] * 32, {"sha256": OLD_TARGET_SHA})
        cls.profiles = fresh_profiles()
        cls.frames = frames_from_profiles(cls.profiles, 360)
        cls.data = b"".join(cls.frames)
        cls.reference = (cls.data, cls.frames, {"sha256": MODEL.sha(cls.data), "requested_width": 0})
        cls.identity = (cls.data, cls.frames, {"sha256": MODEL.sha(cls.data), "requested_width": 640})
        cls.args = SimpleNamespace(coefficients=OBSERVED, training_source="training-source", training_scaled="training-scaled", reference="reference", identity="identity")

    def values(self):
        return [self.old_source, self.old_target, self.reference, self.identity]

    def test_freeze_uses_only_pinned_training_and_prescaled_reference(self):
        with mock.patch.object(MODEL, "capture", side_effect=self.values()) as capture:
            frozen = MODEL.freeze(self.args)
        self.assertEqual(capture.call_args_list, [mock.call("training-source", 640, 360, 32), mock.call("training-scaled", 320, 180, 32),
                                                 mock.call("reference", 640, 360, 24, True, 0), mock.call("identity", 640, 360, 24, True, 640)])
        self.assertEqual([entry["candidate"] for entry in frozen["predictions"]], family())
        self.assertIs(frozen["scaled_samples_seen"], False)
        self.assertEqual(MODEL.validate_frozen(frozen), frozen["predictions"])
        self.assertEqual(frozen["profiles"], self.profiles)

    def test_wrong_old_digest_rejects_before_fresh_reference(self):
        for index in (0, 1):
            values = self.values()
            data, frames, receipt = values[index]
            values[index] = (data, frames, dict(receipt, sha256="0" * 64))
            with mock.patch.object(MODEL, "capture", side_effect=values) as capture, self.assertRaisesRegex(MODEL.AdmissionError, "OLD training"):
                MODEL.freeze(self.args)
            self.assertEqual(capture.call_count, 2)

    def test_training_duplicate_and_native_identity_mismatch(self):
        for index in (0, 1):
            values = self.values()
            data, frames, receipt = values[index]
            changed = list(frames)
            changed[7] = b"not the same full picture"
            values[index] = (data, changed, receipt)
            with mock.patch.object(MODEL, "capture", side_effect=values), self.assertRaisesRegex(MODEL.AdmissionError, "training duplicate"):
                MODEL.freeze(self.args)
        values = self.values()
        values[3] = (self.data[:-1], self.frames, self.identity[2])
        with mock.patch.object(MODEL, "capture", side_effect=values), self.assertRaisesRegex(MODEL.AdmissionError, "zero/identity"):
            MODEL.freeze(self.args)

    def test_nonseparating_design_is_not_empirical_confirmation(self):
        frames = frames_from_profiles([[96] * 640, [160] * 640] + [[128] * 640] * 10, 360)
        data = b"".join(frames)
        values = self.values()
        values[2] = (data, frames, {"sha256": MODEL.sha(data), "requested_width": 0})
        values[3] = (data, frames, {"sha256": MODEL.sha(data), "requested_width": 640})
        with mock.patch.object(MODEL, "capture", side_effect=values), self.assertRaisesRegex(MODEL.AdmissionError, "inconclusive"):
            MODEL.freeze(self.args)


if __name__ == "__main__":
    unittest.main()
