#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Filesystem-only failure/cleanup contracts for the real sanitizer runner."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


RUNNER = Path(__file__).with_name("library-selected-sanitizer.py")
COMPILER = r"""#!/usr/bin/env python3
import json, os, pathlib, sys
proof = pathlib.Path(os.environ["DIL_SANITIZER_CONTROL_LOG"])
events = [json.loads(line) for line in proof.read_text().splitlines()] if proof.exists() else []
args = sys.argv[1:]
source = next(pathlib.Path(a) for a in args if a.endswith(".cpp") and pathlib.Path(a).name.startswith("library-"))
guard = next(pathlib.Path(a) for a in args if pathlib.Path(a).name == "device-guard.cpp")
with proof.open("a") as out:
    out.write(json.dumps({"kind": "compile", "case": source.stem, "arguments": args,
                          "guard": guard.read_text()}) + "\n")
if sum(event["kind"] == "compile" for event in events) + 1 == int(os.environ.get("DIL_SANITIZER_CONTROL_FAIL_BUILD", "0")):
    sys.exit(37)
output = pathlib.Path(args[args.index("-o") + 1])
program = r'#!/usr/bin/env python3' + '\n' + r'''
import json, os, pathlib, sys
assert os.environ["CRYSTALHD_CONFIG_DISABLE"] == "1"
assert os.environ["LD_PRELOAD"] == os.environ["LD_LIBRARY_PATH"] == ""
assert os.environ["ASAN_OPTIONS"] == "detect_leaks=1:halt_on_error=1:abort_on_error=1"
assert os.environ["UBSAN_OPTIONS"] == "halt_on_error=1:print_stacktrace=1"
assert os.environ["LSAN_OPTIONS"] == "exitcode=23"
proof = pathlib.Path(os.environ["DIL_SANITIZER_CONTROL_LOG"])
with proof.open("a") as out:
    out.write(json.dumps({"kind": "run", "case": pathlib.Path(sys.argv[0]).name,
                          "arguments": sys.argv[1:]}) + "\n")
sys.exit(43 if pathlib.Path(sys.argv[0]).name == os.environ.get("DIL_SANITIZER_CONTROL_FAIL_RUN") else 0)
'''
output.write_text(program)
output.chmod(0o755)
"""


class SanitizerRunnerContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="selected DIL sanitizer ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = self.root / "private repo"
        self.tests = self.repo / "tests"
        self.tests.mkdir(parents=True)
        self.runner = self.tests / RUNNER.name
        shutil.copyfile(RUNNER, self.runner)
        self.compiler = self.root / "compiler with spaces"
        self.compiler.write_text(COMPILER)
        self.compiler.chmod(0o755)
        self.scratch = self.root / "scratch with spaces"
        self.scratch.mkdir()
        self.proof = self.root / "events.jsonl"
        self.normal = self.repo / "linux_lib/libcrystalhd/libcrystalhd.so.3.0"
        self.normal.parent.mkdir(parents=True)
        self.normal.write_bytes(b"normal binary must remain unchanged\n")
        self.stamp = self.normal.parent / ".crystalhd-cpu-config"
        self.stamp.write_bytes(b"normal mode stamp must remain unchanged\n")
        self.before = {path: (path.read_bytes(), path.stat().st_mtime_ns)
                       for path in (self.normal, self.stamp)}

    def events(self):
        return [json.loads(line) for line in self.proof.read_text().splitlines()] if self.proof.exists() else []

    def invoke(self, *, arguments=(), expected=0, **extra):
        environment = os.environ.copy()
        environment.update(CXX=shlex.quote(str(self.compiler)),
                           TMPDIR=str(self.scratch), DIL_SANITIZER_CONTROL_LOG=str(self.proof),
                           CXXFLAGS="-O0 -fno-sanitize=all", CPPFLAGS="-DSELECTED_DIL_CONTROL=1",
                           LDFLAGS="-Wl,--as-needed", CRYSTALHD_LIBRARY_SANITIZER_CPU_FLAGS="",
                           CRYSTALHD_LIBRARY_SANITIZER_LEGACY="0",
                           ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=0",
                           LSAN_OPTIONS="exitcode=0")
        environment.update(extra)
        result = subprocess.run([sys.executable, "-B", str(self.runner), *arguments],
                                cwd=str(self.root), env=environment, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertEqual(list(self.scratch.iterdir()), [], "temporary binaries or guard leaked")
        for path, before in self.before.items():
            self.assertEqual((path.read_bytes(), path.stat().st_mtime_ns), before)
        return result

    def test_all_cases_modes_instrumentation_and_private_outputs(self):
        self.invoke()
        builds = [event for event in self.events() if event["kind"] == "compile"]
        runs = [(event["case"], event["arguments"])
                for event in self.events() if event["kind"] == "run"]
        self.assertEqual([event["case"] for event in builds],
                         ["library-" + name for name in ("tx-ring", "tx-concurrency", "copy", "planar", "format")])
        self.assertEqual(runs, [("library-tx-ring", []), ("library-tx-concurrency", []),
                                ("library-tx-concurrency", ["status"]),
                                ("library-tx-concurrency", ["serial"]),
                                ("library-copy", []), ("library-planar", []), ("library-format", [])])
        for event in builds:
            flags = event["arguments"]
            for flag in ("-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                         "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"):
                self.assertIn(flag, flags)
            self.assertGreater(flags.index("-fsanitize=address,undefined"), flags.index("-fno-sanitize=all"))
            self.assertGreater(flags.index("-O1"), flags.index("-O0"))
            self.assertIn("-DSELECTED_DIL_CONTROL=1", flags)
            self.assertIn("--wrap=open64", " ".join(flags))
            self.assertIn("unexpected device operation", event["guard"])
            self.assertTrue(Path(flags[flags.index("-o") + 1]).is_relative_to(self.scratch))

    def test_first_compiler_failure_propagates_without_running(self):
        self.invoke(expected=1, DIL_SANITIZER_CONTROL_FAIL_BUILD="1")
        self.assertEqual([event["kind"] for event in self.events()], ["compile"])

    def test_later_compiler_failure_stops_remaining_cases(self):
        self.invoke(expected=1, DIL_SANITIZER_CONTROL_FAIL_BUILD="2")
        self.assertEqual([event["kind"] for event in self.events()], ["compile", "run", "compile"])

    def test_test_failure_propagates_and_stops_remaining_cases(self):
        self.invoke(expected=1, DIL_SANITIZER_CONTROL_FAIL_RUN="library-tx-concurrency")
        self.assertEqual([event["kind"] for event in self.events()], ["compile", "run", "compile", "run"])

    def test_legacy_rejected_before_compile(self):
        self.invoke(expected=1, CRYSTALHD_LIBRARY_SANITIZER_LEGACY="1")
        self.assertEqual(self.events(), [])

    def test_empty_or_missing_compiler_fails_closed(self):
        for compiler in ("", str(self.root / "missing compiler")):
            with self.subTest(compiler=compiler):
                self.invoke(expected=1, CXX=shlex.quote(compiler) if compiler else "")
                self.assertEqual(self.events(), [])

    def test_arguments_cannot_select_fewer_cases(self):
        self.invoke(arguments=("tx-ring",), expected=1)
        self.assertEqual(self.events(), [])


if __name__ == "__main__":
    unittest.main()
