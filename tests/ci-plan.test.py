#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise actual Git histories and fail-closed aggregate-job outcomes."""

import copy
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

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
                self.assertEqual(plan["contracts"], contracts)

    def test_complete_pr_not_last_commit(self):
        self.write("include/api.h", "code change\n")
        self.commit()
        self.write("README.md", "documentation in final commit\n")
        self.assertTrue(self.plan()["heavy"])

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
                     "include/generated.h", "Makefile", "cpu.mk", ".github/workflows/main.yml"):
            with self.subTest(name=name):
                self.git("reset", "--hard", self.base)
                self.write(name)
                self.assertTrue(self.plan()["heavy"])

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

    def test_whitespace_paths_do_not_hide_files(self):
        self.write("README.md\ninclude/spoof.h")
        self.assertTrue(self.plan()["heavy"])

    def test_empty_diff_is_full(self):
        self.assertTrue(PLAN.classify(self.root, self.base, self.base)["heavy"])

    def test_missing_or_invalid_commit_is_an_error(self):
        for base in ("0" * 40, "--help", self.base[:7], ""):
            with self.subTest(base=base), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                PLAN.classify(self.root, base, self.base)

    def test_truncated_raw_diff_is_an_error(self):
        from unittest.mock import patch
        for raw in (b":100644 100644 a b M\0README.md", b":100644\0README.md\0", b"\0"):
            with self.subTest(raw=raw), patch.object(PLAN, "git", return_value=raw):
                with self.assertRaises(ValueError):
                    PLAN.classify(self.root, self.base, self.base)

    def test_gate_requires_each_selected_result(self):
        for heavy in (True, False):
            needs = {"classify": {"result": "success", "outputs": {
                "heavy": str(heavy).lower(), "contracts": "false"}},
                "docs": {"result": "success"},
                "build": {"result": "success" if heavy else "skipped"},
                "upstream-kernel-api": {"result": "success" if heavy else "skipped"}}
            PLAN.gate(needs)
            for job in needs:
                for state in ("failure", "cancelled", "skipped", "success", ""):
                    if state == needs[job]["result"]:
                        continue
                    bad = copy.deepcopy(needs)
                    bad[job]["result"] = state
                    with self.subTest(heavy=heavy, job=job, state=state), self.assertRaises(ValueError):
                        PLAN.gate(bad)
            for output in ("heavy", "contracts"):
                bad = copy.deepcopy(needs)
                del bad["classify"]["outputs"][output]
                with self.assertRaises(ValueError):
                    PLAN.gate(bad)
            for job in needs:
                bad = copy.deepcopy(needs)
                del bad[job]
                with self.assertRaises(ValueError):
                    PLAN.gate(bad)


if __name__ == "__main__":
    unittest.main()
