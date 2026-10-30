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
        for name in PLAN.USERSPACE:
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

    def test_prose_and_contract_selections(self):
        for filename, contracts in (("README.md", False), ("CONFIGURATION.md", True)):
            with self.subTest(filename=filename):
                self.git("reset", "--hard", self.base)
                self.write(filename, "updated\n")
                plan = self.plan()
                self.assertFalse(plan["heavy"])
                self.assertFalse(plan["kernel"])
                self.assertEqual(plan["contracts"], contracts)

    def test_each_reviewed_userspace_file_keeps_both_builds_without_kernel_matrix(self):
        for name in PLAN.USERSPACE:
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name, "userspace change\n")
                plan = self.plan()
                self.assertTrue(plan["heavy"])
                self.assertFalse(plan["kernel"])
                self.assertEqual(plan["files"][0]["lane"], "userspace")

    def test_userspace_with_prose_and_contracts_keeps_contract_validation(self):
        self.write("scripts/crystalhd-player.py", "code change\n")
        self.write("README.md", "explanation\n")
        self.write("CONFIGURATION.md", "contract explanation\n")
        plan = self.plan()
        self.assertTrue(plan["heavy"])
        self.assertFalse(plan["kernel"])
        self.assertTrue(plan["contracts"])

    def test_complete_pr_not_last_commit(self):
        self.write("include/api.h", "code change\n")
        self.commit()
        self.write("README.md", "documentation in final commit\n")
        plan = self.plan()
        self.assertTrue(plan["heavy"])
        self.assertTrue(plan["kernel"])

    def test_complete_userspace_pr_not_last_prose_commit(self):
        self.write("scripts/crystalhd-player.py", "code change\n")
        self.commit()
        self.write("README.md", "documentation in final commit\n")
        plan = self.plan()
        self.assertTrue(plan["heavy"])
        self.assertFalse(plan["kernel"])

    def test_merge_base_excludes_unrelated_base_branch_changes(self):
        self.git("checkout", "-qb", "feature")
        self.write("README.md", "feature docs\n")
        head = self.commit()
        self.git("checkout", "-q", "--detach", self.base)
        self.write("include/api.h", "base branch only\n")
        base = self.commit()
        self.assertFalse(PLAN.classify(self.root, base, head)["heavy"])

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
                self.assertTrue(plan["heavy"])
                self.assertTrue(plan["kernel"])

    def test_modified_headers_and_unknown_sources_are_full(self):
        for name in ("filters/gst/gst-plugin-1.0/gstcrystalhd-stage-trace.h",
                     "linux_lib/libcrystalhd/libcrystalhd_if.h",
                     "filters/gst/gst-plugin-1.0/unreviewed.c"):
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name)
                base = self.commit()
                self.write(name, "modified\n")
                plan = PLAN.classify(self.root, base, self.commit())
                self.assertTrue(plan["heavy"])
                self.assertTrue(plan["kernel"])

    def test_new_allowlisted_file_is_full(self):
        self.git("rm", "scripts/crystalhd-player.py")
        base = self.commit()
        self.write("scripts/crystalhd-player.py", "new source\n")
        plan = PLAN.classify(self.root, base, self.commit())
        self.assertTrue(plan["heavy"])
        self.assertTrue(plan["kernel"])

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
                self.assertTrue(self.plan()["heavy"])

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
                self.assertTrue(plan["heavy"])
                self.assertTrue(plan["kernel"])

    def test_whitespace_paths_do_not_hide_files(self):
        self.write("README.md\ninclude/spoof.h")
        self.assertTrue(self.plan()["heavy"])

    def test_empty_diff_is_full(self):
        plan = PLAN.classify(self.root, self.base, self.base)
        self.assertTrue(plan["heavy"])
        self.assertTrue(plan["kernel"])

    def test_push_tag_manual_and_unknown_events_are_full(self):
        for event in ("push", "workflow_dispatch", "release", "", "pull_request_target"):
            with self.subTest(event=event):
                # These routes must not inspect a PR diff or depend on its base.
                with patch.object(PLAN, "classify", side_effect=AssertionError("diff route")):
                    plan = PLAN.select(self.root, event, "", self.base)
                self.assertTrue(plan["heavy"])
                self.assertTrue(plan["kernel"])
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
                         {"heavy": "true", "kernel": "true", "contracts": "true"})
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
        for heavy, kernel in ((False, False), (True, False), (True, True)):
            needs = {"classify": {"result": "success", "outputs": {
                "heavy": str(heavy).lower(), "kernel": str(kernel).lower(),
                "contracts": "false"}},
                "docs": {"result": "success"},
                "build": {"result": "success" if heavy else "skipped"},
                "upstream-kernel-api": {"result": "success" if kernel else "skipped"}}
            PLAN.gate(needs)
            for job in needs:
                for state in ("failure", "cancelled", "skipped", "success", ""):
                    if state == needs[job]["result"]:
                        continue
                    bad = copy.deepcopy(needs)
                    bad[job]["result"] = state
                    with self.subTest(heavy=heavy, kernel=kernel, job=job, state=state), self.assertRaises(ValueError):
                        PLAN.gate(bad)
            for output in ("heavy", "kernel", "contracts"):
                for value in (None, "", "TRUE", True, "success"):
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

    def test_inconsistent_kernel_without_userspace_is_rejected(self):
        needs = {"classify": {"result": "success", "outputs": {
            "heavy": "false", "kernel": "true", "contracts": "false"}},
            "docs": {"result": "success"}, "build": {"result": "skipped"},
            "upstream-kernel-api": {"result": "success"}}
        with self.assertRaisesRegex(ValueError, "kernel matrix requires"):
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
        for output in ("heavy", "kernel", "contracts"):
            self.assertIn(output + ": ${{ steps.plan.outputs." + output + " }}", job("classify"))
        self.assertIn("if: needs.classify.outputs.heavy == 'true'", job("build"))
        self.assertIn("os: [ubuntu-24.04, ubuntu-26.04]", job("build"))
        self.assertIn("fail-fast: false", job("build"))
        self.assertIn("if: needs.classify.outputs.kernel == 'true'", job("upstream-kernel-api"))
        self.assertIn("if: always()", job("ci"))
        self.assertIn("needs: [classify, docs, build, upstream-kernel-api]", job("ci"))


if __name__ == "__main__":
    unittest.main()
