#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise actual Git histories and fail-closed aggregate-job outcomes."""

import copy
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("ci_plan", Path(__file__).with_name("ci-plan.py"))
PLAN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PLAN)


class PlanTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git("init", "-q")
        self.git("config", "user.name", "CI test")
        self.git("config", "user.email", "ci@example.invalid")
        for name in ("README.md", "HISTORY.md", "CONFIGURATION.md", "include/api.h"):
            self.write(name)
        for name in PLAN.USERSPACE | PLAN.PACKED:
            self.write(name)
        self.base = self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], stderr=subprocess.PIPE).decode().strip()

    def write(self, name, value="baseline\n"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD")

    def plan(self):
        return PLAN.classify(self.root, self.base, self.commit())

    def assert_lanes(self, plan, *, heavy, packed, kernel):
        self.assertEqual({name: plan[name] for name in ("heavy", "packed", "kernel")},
                         {"heavy": heavy, "packed": packed, "kernel": kernel})

    def needs(self, plan):
        return {"classify": {"result": "success", "outputs": {
                    name: str(plan[name]).lower() for name in PLAN.OUTPUTS}},
                "docs": {"result": "success"},
                "build": {"result": "success" if plan["heavy"] else "skipped"},
                "packed": {"result": "success" if plan["packed"] else "skipped"},
                "upstream-kernel-api": {"result": "success" if plan["kernel"] else "skipped"}}

    def test_prose_and_contract_selections(self):
        for filename, contracts in (("README.md", False), ("CONFIGURATION.md", True)):
            with self.subTest(filename=filename):
                self.git("reset", "--hard", self.base)
                self.write(filename, "updated\n")
                plan = self.plan()
                self.assert_lanes(plan, heavy=False, packed=False, kernel=False)
                self.assertEqual(plan["contracts"], contracts)

    def test_each_reviewed_userspace_file_keeps_both_builds_without_kernel_matrix(self):
        for name in PLAN.USERSPACE:
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name, "userspace change\n")
                plan = self.plan()
                self.assert_lanes(plan, heavy=True, packed=False, kernel=False)
                self.assertEqual(plan["files"][0]["lane"], "userspace")

    def test_each_reviewed_packed_file_selects_only_component_and_contracts(self):
        for name in PLAN.PACKED:
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name, "packed change\n")
                plan = self.plan()
                self.assert_lanes(plan, heavy=False, packed=True, kernel=False)
                self.assertTrue(plan["contracts"])
                self.assertEqual(plan["files"][0]["lane"], "packed")

    def test_packed_with_reviewed_prose_and_contracts_stays_component_only(self):
        self.write("filters/gst/packed-upscale/packed-scale-engine.c", "packed change\n")
        self.write("README.md", "explanation\n")
        self.write("CONFIGURATION.md", "contract explanation\n")
        plan = self.plan()
        self.assert_lanes(plan, heavy=False, packed=True, kernel=False)
        self.assertTrue(plan["contracts"])

    def test_packed_and_each_other_userspace_file_keep_complete_userspace_builds(self):
        for name in PLAN.USERSPACE:
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write("filters/gst/packed-upscale/packed-scale-engine.c", "packed change\n")
                self.write(name, "other component change\n")
                plan = self.plan()
                self.assert_lanes(plan, heavy=True, packed=False, kernel=False)
                self.assertTrue(plan["contracts"])

    def test_userspace_with_prose_and_contracts_keeps_contract_validation(self):
        self.write("scripts/crystalhd-player.py", "code change\n")
        self.write("README.md", "explanation\n")
        self.write("CONFIGURATION.md", "contract explanation\n")
        plan = self.plan()
        self.assert_lanes(plan, heavy=True, packed=False, kernel=False)
        self.assertTrue(plan["contracts"])

    def test_complete_pr_not_last_commit(self):
        self.write("include/api.h", "code change\n")
        self.commit()
        self.write("README.md", "documentation in final commit\n")
        plan = self.plan()
        self.assert_lanes(plan, heavy=True, packed=False, kernel=True)

    def test_complete_userspace_pr_not_last_prose_commit(self):
        self.write("scripts/crystalhd-player.py", "code change\n")
        self.commit()
        self.write("README.md", "documentation in final commit\n")
        plan = self.plan()
        self.assert_lanes(plan, heavy=True, packed=False, kernel=False)

    def test_complete_packed_pr_not_last_prose_commit(self):
        self.write("filters/gst/packed-upscale/packed-scale-engine.c", "packed change\n")
        self.commit()
        self.write("README.md", "documentation in final commit\n")
        self.assert_lanes(self.plan(), heavy=False, packed=True, kernel=False)

    def test_complete_crosscomponent_pr_not_last_packed_commit(self):
        for name, kernel in (("scripts/crystalhd-player.py", False), ("include/api.h", True)):
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name, "earlier change\n")
                self.commit()
                self.write("filters/gst/packed-upscale/packed-scale-engine.c", "final packed change\n")
                self.assert_lanes(self.plan(), heavy=True, packed=False, kernel=kernel)

    def test_merge_base_excludes_unrelated_base_branch_changes(self):
        self.git("checkout", "-qb", "feature")
        self.write("README.md", "feature docs\n")
        head = self.commit()
        self.git("checkout", "-q", "--detach", self.base)
        self.write("include/api.h", "base branch only\n")
        base = self.commit()
        self.assert_lanes(PLAN.classify(self.root, base, head),
                          heavy=False, packed=False, kernel=False)

    def test_packed_merge_base_excludes_unrelated_base_branch_changes(self):
        self.git("checkout", "-qb", "feature")
        self.write("filters/gst/packed-upscale/packed-scale-engine.c", "feature packed change\n")
        head = self.commit()
        self.git("checkout", "-q", "--detach", self.base)
        self.write("include/api.h", "base branch only\n")
        base = self.commit()
        self.assert_lanes(PLAN.classify(self.root, base, head),
                          heavy=False, packed=True, kernel=False)

    def test_unknown_fixtures_workflows_generated_are_full(self):
        for name in ("NEW.md", "tests/fixtures/evidence.json", "tests/fixtures/chart.svg",
                     "include/generated.h", "Makefile", "cpu.mk", ".github/workflows/main.yml",
                     "tests/ci-plan.py", "tests/ci-plan.test.py",
                     "filters/gst/gst-plugin-1.0/new.c",
                     "filters/gst/gst-plugin-1.0/Makefile",
                     "filters/gst/gst-plugin-1.0/gstcrystalhd-stage-trace.h",
                     "linux_lib/libcrystalhd/libcrystalhd_if.h"):
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name)
                plan = self.plan()
                self.assert_lanes(plan, heavy=True, packed=False, kernel=True)

    def test_modified_headers_and_unknown_sources_are_full(self):
        for name in ("filters/gst/gst-plugin-1.0/gstcrystalhd-stage-trace.h",
                     "linux_lib/libcrystalhd/libcrystalhd_if.h",
                     "filters/gst/gst-plugin-1.0/unreviewed.c",
                     "filters/gst/packed-upscale/horizontal-simd.h",
                     "filters/gst/packed-upscale/packed-scale-engine.h",
                     "filters/gst/packed-upscale/Makefile",
                     "filters/gst/packed-upscale/packed-scale-x11-contract.c",
                     "filters/gst/packed-upscale/unreviewed.c",
                     "filters/gst/packed-upscale/README.md",
                     "filters/gst/packed-upscale/new.test.py",
                     "filters/gst/packed-upscale/new.sh",
                     "tests/unreviewed.test.py", "tests/unreviewed.sh"):
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name)
                base = self.commit()
                self.write(name, "modified\n")
                plan = PLAN.classify(self.root, base, self.commit())
                self.assert_lanes(plan, heavy=True, packed=False, kernel=True)

    def test_packed_with_unknown_or_shared_input_is_full(self):
        for name in ("include/api.h", "Makefile", "cpu.mk", ".github/workflows/main.yml",
                     "tests/ci-plan.py", "tests/ci-plan.test.py",
                     "filters/gst/packed-upscale/packed-scale-engine.h",
                     "filters/gst/packed-upscale/Makefile", "filters/gst/xv-output/gstcrystalhdxv.c"):
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name)
                base = self.commit() if name != "include/api.h" else self.base
                self.write(name, "shared or unknown change\n")
                self.write("filters/gst/packed-upscale/packed-scale-engine.c", "packed change\n")
                self.assert_lanes(PLAN.classify(self.root, base, self.commit()),
                                  heavy=True, packed=False, kernel=True)

    def test_new_allowlisted_files_are_full(self):
        for name in ("scripts/crystalhd-player.py", *PLAN.PACKED):
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.git("rm", name)
                base = self.commit()
                self.write(name, "new source\n")
                self.assert_lanes(PLAN.classify(self.root, base, self.commit()),
                                  heavy=True, packed=False, kernel=True)

    def test_rename_delete_executable_symlink_are_full(self):
        for mode in ("rename", "delete", "executable", "symlink"):
            with self.subTest(mode=mode):
                self.git("reset", "--hard", self.base)
                source = self.root / "README.md"
                if mode == "rename":
                    source.rename(self.root / "HISTORY.md")
                elif mode == "delete":
                    source.unlink()
                elif mode == "executable":
                    source.chmod(0o755)
                else:
                    source.unlink()
                    source.symlink_to("include/api.h")
                self.assert_lanes(self.plan(), heavy=True, packed=False, kernel=True)

    def test_userspace_rename_delete_executable_symlink_are_full(self):
        for mode in ("rename", "delete", "executable", "symlink"):
            with self.subTest(mode=mode):
                self.git("reset", "--hard", self.base)
                source = self.root / "scripts/crystalhd-player.py"
                if mode == "rename":
                    source.rename(self.root / "scripts/renamed-player.py")
                elif mode == "delete":
                    source.unlink()
                elif mode == "executable":
                    source.chmod(0o755)
                else:
                    source.unlink()
                    source.symlink_to("../include/api.h")
                plan = self.plan()
                self.assert_lanes(plan, heavy=True, packed=False, kernel=True)

    def test_packed_rename_delete_executable_symlink_are_full(self):
        for name in PLAN.PACKED:
            for mode in ("rename", "delete", "executable", "symlink"):
                with self.subTest(name=name, mode=mode):
                    self.git("reset", "--hard", self.base)
                    source = self.root / name
                    if mode == "rename":
                        source.rename(source.with_name("renamed-" + source.name))
                    elif mode == "delete":
                        source.unlink()
                    elif mode == "executable":
                        source.chmod(0o755)
                    else:
                        source.unlink()
                        source.symlink_to("packed-scale-engine.c")
                    self.assert_lanes(self.plan(), heavy=True, packed=False, kernel=True)

    def test_whitespace_paths_do_not_hide_files(self):
        self.write("README.md\ninclude/spoof.h")
        self.assert_lanes(self.plan(), heavy=True, packed=False, kernel=True)

    def test_packed_path_prefix_suffix_or_whitespace_cannot_hide_unknown_files(self):
        for name in ("filters/gst/packed-upscale/packed-scale-engine.c.extra",
                     "filters/gst/packed-upscale/packed-scale-engine.c\nREADME.md",
                     "filters/gst/packed-upscale/../other.c",
                     "other/packed-scale-engine.c", "tests/packed-sanitizer-loader.test.py.extra"):
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name)
                self.assert_lanes(self.plan(), heavy=True, packed=False, kernel=True)

    def test_empty_diff_is_full(self):
        plan = PLAN.classify(self.root, self.base, self.base)
        self.assert_lanes(plan, heavy=True, packed=False, kernel=True)

    def test_push_tag_manual_and_unknown_events_are_full(self):
        for event in ("push", "workflow_dispatch", "release", "", "pull_request_target"):
            with self.subTest(event=event):
                # These routes must not inspect a PR diff or depend on its base.
                with patch.object(PLAN, "classify", side_effect=AssertionError("diff route")):
                    plan = PLAN.select(self.root, event, "", self.base)
                self.assert_lanes(plan, heavy=True, packed=False, kernel=True)
                self.assertTrue(plan["contracts"])

    def test_cli_exports_all_lane_outputs_on_full_routes(self):
        output = self.root / "outputs"
        summary = self.root / "summary"
        environment = {"CI_EVENT": "workflow_dispatch", "CI_HEAD": self.base,
                       "GITHUB_OUTPUT": str(output), "GITHUB_STEP_SUMMARY": str(summary)}
        with patch.dict("os.environ", environment), patch("sys.argv", ["ci-plan.py", "classify"]):
            with patch("builtins.print"):
                PLAN.main()
        self.assertEqual(dict(line.split("=", 1) for line in output.read_text().splitlines()),
                         {"heavy": "true", "packed": "false", "kernel": "true", "contracts": "true"})
        self.assertIn('"kernel": true', summary.read_text())

    def test_missing_or_invalid_commit_is_an_error(self):
        for base in ("0" * 40, "--help", self.base[:7], ""):
            with self.subTest(base=base), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                PLAN.classify(self.root, base, self.base)

    def test_truncated_raw_diff_is_an_error(self):
        for raw in (b":100644 100644 a b M\0README.md", b":100644\0README.md\0", b"\0"):
            with self.subTest(raw=raw), patch.object(PLAN, "git", return_value=raw):
                with self.assertRaises(ValueError):
                    PLAN.classify(self.root, self.base, self.base)

    def test_gate_requires_each_selected_result(self):
        # Exercise the gate from actual Git selections for every route.
        for filename in ("README.md", "CONFIGURATION.md",
                         "filters/gst/packed-upscale/packed-scale-engine.c",
                         "scripts/crystalhd-player.py", "include/api.h"):
            self.git("reset", "--hard", self.base)
            self.write(filename, "route change\n")
            needs = self.needs(self.plan())
            PLAN.gate(needs)
            for job in needs:
                for state in ("failure", "cancelled", "skipped", "success", "timed_out",
                              "neutral", "action_required", "", None):
                    if state == needs[job]["result"]:
                        continue
                    bad = copy.deepcopy(needs)
                    bad[job]["result"] = state
                    with self.subTest(filename=filename, job=job, state=state), self.assertRaises(ValueError):
                        PLAN.gate(bad)
                bad = copy.deepcopy(needs)
                del bad[job]["result"]
                with self.subTest(filename=filename, missing_result=job), self.assertRaises(ValueError):
                    PLAN.gate(bad)
            for output in ("heavy", "packed", "kernel", "contracts"):
                for value in (None, "", "TRUE", True, False, "success", 1, [], {}):
                    bad = copy.deepcopy(needs)
                    if value is None:
                        del bad["classify"]["outputs"][output]
                    else:
                        bad["classify"]["outputs"][output] = value
                    with self.subTest(output=output, value=value), self.assertRaises(ValueError):
                        PLAN.gate(bad)
            for job in needs:
                bad = copy.deepcopy(needs)
                del bad[job]
                with self.assertRaises(ValueError):
                    PLAN.gate(bad)
                for invalid in (None, "success", []):
                    bad = copy.deepcopy(needs)
                    bad[job] = invalid
                    with self.subTest(filename=filename, job=job, invalid=invalid), self.assertRaises(ValueError):
                        PLAN.gate(bad)
            bad = copy.deepcopy(needs)
            bad["unknown"] = {"result": "success"}
            with self.assertRaises(ValueError):
                PLAN.gate(bad)
            for invalid in (None, "", []):
                bad = copy.deepcopy(needs)
                bad["classify"]["outputs"] = invalid
                with self.subTest(filename=filename, outputs=invalid), self.assertRaises(ValueError):
                    PLAN.gate(bad)

    def test_inconsistent_selection_combinations_are_rejected(self):
        for heavy in (False, True):
            for packed in (False, True):
                for kernel in (False, True):
                    for contracts in (False, True):
                        plan = {"heavy": heavy, "packed": packed, "kernel": kernel,
                                "contracts": contracts}
                        needs = self.needs(plan)
                        invalid = ((kernel and not heavy) or (packed and heavy) or
                                   (packed and not contracts))
                        with self.subTest(**plan):
                            if invalid:
                                with self.assertRaises(ValueError):
                                    PLAN.gate(needs)
                            else:
                                PLAN.gate(needs)

    def test_gate_rejects_missing_or_malformed_dependency_map(self):
        for needs in (None, {}, [], "", "success"):
            with self.subTest(needs=needs), self.assertRaises(ValueError):
                PLAN.gate(needs)


class WorkflowContractTest(unittest.TestCase):
    def test_matrix_and_final_gate_are_wired_to_independent_outputs(self):
        import re
        workflow = (Path(__file__).resolve().parents[1] / ".github/workflows/main.yml").read_text()
        def job(name):
            match = re.search(r"^  " + re.escape(name) + r":\n(.*?)(?=^  [a-z][a-z-]*:|\Z)",
                              workflow, re.MULTILINE | re.DOTALL)
            self.assertIsNotNone(match, name)
            return match.group(1)
        for output in ("heavy", "packed", "kernel", "contracts"):
            self.assertIn(output + ": ${{ steps.plan.outputs." + output + " }}", job("classify"))
        for name, output in (("build", "heavy"), ("packed", "packed")):
            selected = job(name)
            self.assertIn("if: needs.classify.outputs." + output + " == 'true'", selected)
            self.assertIn("os: [ubuntu-24.04, ubuntu-26.04]", selected)
            self.assertIn("fail-fast: false", selected)
            self.assertEqual(selected.count("run: make -C filters/gst/packed-upscale check\n"), 1)
            self.assertEqual(selected.count("make -C filters/gst/packed-upscale check-sanitized\n"), 1)
            self.assertEqual(selected.count("python3 -B tests/packed-sanitizer-loader.test.py\n"), 1)
        self.assertIn("if: needs.classify.outputs.kernel == 'true'", job("upstream-kernel-api"))
        self.assertIn("name: CI gate", job("ci"))
        self.assertIn("if: always()", job("ci"))
        self.assertIn("needs: [classify, docs, build, packed, upstream-kernel-api]", job("ci"))
        self.assertIn("CI_NEEDS: ${{ toJSON(needs) }}", job("ci"))
        self.assertIn("run: python3 -B tests/ci-plan.py gate", job("ci"))
        self.assertNotIn("paths-ignore:", workflow)


if __name__ == "__main__":
    unittest.main()
