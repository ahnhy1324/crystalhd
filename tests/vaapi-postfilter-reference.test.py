#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""OWN wrapper controls, not a synthetic BWDIF/pixel golden or original ABI proof."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("postfilter_reference", Path(__file__).with_name("vaapi-postfilter-reference.py"))
R = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(R)


class ReferenceControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.original = self.root/"original"
        self.original.mkdir()
        self.result = self.root/"reference"
        self.result.mkdir()
        base = dict(pts=6006, best_effort_timestamp=6006, duration=3003,
                    time_base=[0, 1], format=23, width=2, height=2, linesize=[2, 2], flags=0,
                    sample_aspect_ratio=[1, 1], color_range=0, color_primaries=2, color_trc=2,
                    colorspace=2, chroma_location=0, crop=[0, 0, 0, 0])
        self.manifest = dict(schema=1, mode="prefix", nv12_format=23,
            providers=dict(filter="/app/lib/libavfilter.so.12.1.102", util="/app/lib/libavutil.so.61.1.102"),
            graph=dict(buffer_args="video_size=2x2:pix_fmt=23:time_base=1/1:pixel_aspect=1/1",
                       filter_name="bwdif", filter_args="bwdif=1:-1", constraint_basis=R.BASIS),
            inputs=[], expected_outputs=[], schedule=[])
        for i in range(3):
            path = self.original/f"input-{i+1}.nv12"
            path.write_bytes(bytes([i+10])*6)
            self.manifest["inputs"].append(dict(copy.deepcopy(base), path=str(path), flags=24, pts=6006+i*3003))
        for i in range(4):
            path = self.original/f"expected-{i+1}.nv12"
            path.write_bytes(bytes([i+20])*6)
            frame = dict(copy.deepcopy(base), path=str(path), pts=12012+i*3003,
                         sink_time_base=[1, 2], sink_frame_rate=[0, 1])
            self.manifest["expected_outputs"].append(frame)
        for api, index, ret in [("add", 1, 0), ("get", 0, -11), ("add", 2, 0),
                                ("get", 1, 0), ("get", 2, 0), ("get", 0, -11),
                                ("add", 3, 0), ("get", 3, 0), ("get", 4, 0)]:
            self.manifest["schedule"].append(dict(api="av_buffersrc_add_frame" if api == "add" else "av_buffersink_get_frame",
                input_index=index if api == "add" else 0, output_index=index if api == "get" else 0,
                flags=0, ret=ret))
        self.records = [dict(type="header", schema="crystalhd-postfilter-reference-v1", mode="prefix",
            filter_provider=self.manifest["providers"]["filter"], util_provider=self.manifest["providers"]["util"],
            filter_version=R.FILTER_VERSION, util_version=R.UTIL_VERSION, nv12_format=23,
            provider_ok=True, bwdif_available=True, constraint_basis=R.BASIS)]
        for step, call in enumerate(self.manifest["schedule"]):
            if call["api"] in R.GET and call["ret"] == 0:
                i = call["output_index"]
                original = self.manifest["expected_outputs"][i-1]
                path = self.result/f"output-{i:02d}.nv12"
                path.write_bytes(Path(original["path"]).read_bytes())
                self.records.append(dict(type="output", output_index=i, reference_only_eof=False,
                    file=path.name, frame={k: copy.deepcopy(original[k]) for k in R.FRAME_KEYS},
                    sink_time_base=original["sink_time_base"], sink_frame_rate=original["sink_frame_rate"]))
            self.records.append(dict(type="call", step=step, api=call["api"], ret=call["ret"],
                                     expected_ret=call["ret"], reference_only_eof=False))
        self.records.append(dict(type="footer", complete=True, inputs=3, outputs=4,
                                 source_eof_injected=False, reference_eof_observed=False, original_client_eof_claim=False))

    def compare(self):
        return R.compare_reference(self.manifest, self.records, self.result)

    def test_prefix_command_preserves_exact_order_without_source_eof(self):
        command = R.build_command(self.manifest, "/reference", self.result,
                                  ["bwrap", "--"], [(self.root, "/payload")])
        self.assertEqual(command[:3], ["bwrap", "--", "/reference"])
        self.assertEqual(command.count("--frame"), 3)
        self.assertEqual(command.count("--step"), len(self.manifest["schedule"]))
        self.assertNotIn("av_buffersrc_close", command)
        self.assertIn("/payload/original/input-1.nv12", command)
        self.assertIn("bwdif=1:-1", command)

    def test_prefix_result_is_only_same_input_consistency(self):
        result = self.compare()
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["manifest"], self.manifest)
        self.assertEqual(len(result["comparisons"]), 4)
        self.assertFalse(result["source_eof_injected"])
        self.assertFalse(result["original_client_eof_claim"])
        self.assertFalse(result["source_clock_comparison"])

    def test_positive_public_api_success_is_preserved(self):
        call = self.manifest["schedule"][3]
        call["ret"] = 1
        actual = next(r for r in self.records if r.get("type") == "call" and r["step"] == 3)
        actual.update(ret=1, expected_ret=1)
        self.assertEqual(self.compare()["status"], "PASS")

    def test_changed_active_chroma_byte_rejected(self):
        (self.result/"output-04.nv12").write_bytes(bytes([23])*5+b"\xff")
        with self.assertRaisesRegex(ValueError, "pixels differ"):
            self.compare()

    def test_raw_frame_and_sink_properties_are_not_rescaled(self):
        for field, value in [("pts", 12013), ("time_base", [1, 90000]),
                             ("flags", 24), ("crop", [0, 0, 0, 1]),
                             ("color_range", 1), ("linesize", [4, 4])]:
            with self.subTest(field=field):
                records = copy.deepcopy(self.records)
                frame = next(r for r in records if r["type"] == "output")["frame"]
                frame[field] = value
                with self.assertRaises(ValueError):
                    R.compare_reference(self.manifest, records, self.result)
        records = copy.deepcopy(self.records)
        next(r for r in records if r["type"] == "output")["sink_time_base"] = [1, 90000]
        with self.assertRaisesRegex(ValueError, "metadata differs"):
            R.compare_reference(self.manifest, records, self.result)

    def test_wrong_provider_version_availability_or_basis_rejected(self):
        for field, value in [("filter_version", 11), ("util_version", 60), ("bwdif_available", False),
                             ("provider_ok", False), ("constraint_basis", "OBSERVED_OPTION_CALL"),
                             ("filter_provider", "/system/libavfilter.so.12.1.102")]:
            with self.subTest(field=field):
                records = copy.deepcopy(self.records)
                records[0][field] = value
                with self.assertRaises(ValueError):
                    R.compare_reference(self.manifest, records, self.result)

    def test_truncated_or_non_nv12_snapshot_rejected(self):
        path = Path(self.manifest["inputs"][0]["path"])
        path.write_bytes(b"short")
        with self.assertRaises(ValueError):
            R.validate_manifest(self.manifest)
        path.write_bytes(b"123456")
        self.manifest["expected_outputs"][0]["format"] = 0
        with self.assertRaises(ValueError):
            R.validate_manifest(self.manifest)

    def test_snapshot_symlink_rejected(self):
        alias = self.original/"alias.nv12"
        alias.symlink_to(self.manifest["inputs"][0]["path"])
        self.manifest["inputs"][0]["path"] = str(alias)
        with self.assertRaises(ValueError):
            R.validate_manifest(self.manifest)

    def test_missing_metadata_or_injected_graph_chain_rejected(self):
        del self.manifest["inputs"][0]["time_base"]
        with self.assertRaises(ValueError):
            R.validate_manifest(self.manifest)
        self.manifest["inputs"][0]["time_base"] = [0, 1]
        self.manifest["graph"]["filter_args"] += ",scale=2:2"
        with self.assertRaises(ValueError):
            R.validate_manifest(self.manifest)

    def test_schedule_rejects_eof_duplicate_input_and_changed_results(self):
        for mutate in [lambda m: m["schedule"][0].update(api="av_buffersrc_close"),
                       lambda m: m["schedule"][2].update(input_index=1),
                       lambda m: m["schedule"][0].update(input_null=True),
                       lambda m: m["schedule"][1].update(ret=-541478725)]:
            manifest = copy.deepcopy(self.manifest)
            mutate(manifest)
            with self.assertRaises(ValueError):
                R.validate_manifest(manifest)
        next(r for r in self.records if r["type"] == "call")["ret"] = -11
        with self.assertRaises(ValueError):
            self.compare()

    def test_incomplete_extra_and_eof_prefix_reports_rejected(self):
        for mutate in [lambda r: r.pop(),
                       lambda r: r[-1].update(source_eof_injected=True),
                       lambda r: r[-1].update(complete=False),
                       lambda r: r.insert(-1, dict(type="reference_eof", reference_only_eof=True))]:
            records = copy.deepcopy(self.records)
            mutate(records)
            with self.assertRaises(ValueError):
                R.compare_reference(self.manifest, records, self.result)

    def test_output_reordering_and_path_escape_rejected(self):
        output = next(r for r in self.records if r["type"] == "output")
        output["file"] = "../original/input-1.nv12"
        with self.assertRaises(ValueError):
            self.compare()
        output["file"] = "output-01.nv12"
        output["output_index"] = 2
        with self.assertRaises(ValueError):
            self.compare()

    def test_explicit_flushed_mode_never_claims_client_eof(self):
        self.manifest["mode"] = "flushed-reference"
        self.records[0]["mode"] = "flushed-reference"
        self.records[-1].update(source_eof_injected=True, reference_eof_observed=True)
        self.records.insert(-1, dict(type="reference_eof", reference_only_eof=True,
                                    api="av_buffersrc_close", close_pts=-(1 << 63), ret=0))
        result = self.compare()
        self.assertTrue(result["source_eof_injected"])
        self.assertFalse(result["original_client_eof_claim"])
        self.assertEqual(result["mode"], "flushed-reference")

    def test_timeout_retires_only_owned_process_with_term(self):
        marker = self.root/"term-observed"
        client = self.root/"own-slow-reference"
        client.write_text("#!/usr/bin/python3\nimport pathlib,signal,sys,time\n"
                          f"def stop(sig,frame):\n pathlib.Path({str(marker)!r}).write_text('TERM')\n sys.exit(0)\n"
                          "signal.signal(signal.SIGTERM,stop)\nwhile True: time.sleep(.01)\n")
        client.chmod(0o700)
        directory = self.root/"timeout"
        with self.assertRaisesRegex(ValueError, "deadline"):
            R.run_reference(self.manifest, str(client), directory, timeout=.3)
        self.assertEqual(marker.read_text(), "TERM")
        failure = json.loads((directory/"reference-failure.json").read_text())
        self.assertIn("verified_pidfd_term", failure["execution"])
        self.assertFalse((directory/"HANDOFF_REQUIRED.json").exists())


if __name__ == "__main__":
    unittest.main()
