#!/usr/bin/env python3
"""Filesystem-only contracts for the real packed-scaler loader script."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


CHECK = Path(os.environ.get("PACKED_CHECK_SCRIPT", str(
    Path(__file__).resolve().parents[1] / "filters/gst/packed-upscale/check.sh")))
SYSTEM = ("libgstapp.so", "libgstcoreelements.so", "libgstvideoconvertscale.so")
OWN = "libgstcrystalhdpackedscale.so"
STUB = '''#!/usr/bin/env python3
import json, os, pathlib, sys
proof = pathlib.Path(os.environ["PACKED_LOADER_PROOF"])
proof.write_text("called")
assert sys.argv[1:] == ["--execute"]
plugins = pathlib.Path(os.environ["GST_PLUGIN_PATH"])
assert os.environ["GST_PLUGIN_PATH_1_0"] == str(plugins)
assert os.environ["GST_PLUGIN_SYSTEM_PATH"] == ""
assert os.environ["GST_PLUGIN_SYSTEM_PATH_1_0"] == ""
assert pathlib.Path(os.environ["GST_REGISTRY"]).parent == plugins
assert os.environ["GST_REGISTRY_1_0"] == os.environ["GST_REGISTRY"]
targets = json.loads(os.environ["PACKED_LOADER_TARGETS"])
selected = {}
for name, target in targets.items():
    link = plugins / name
    assert link.is_symlink() and link.resolve() == pathlib.Path(target)
    selected[name] = link.read_text()
    assert selected[name] == pathlib.Path(target).read_text()
proof.write_text(json.dumps(selected))
sys.exit(int(os.environ["PACKED_LOADER_EXIT"]))
'''


class LoaderContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="packed loader ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.cwd = self.root / "invocation cwd"
        self.build = self.root / "selected build"
        self.system = self.root / "selected plugins"
        self.scratch = self.root / "scratch space"
        self.fallback = self.root / "ambient plugins"
        for directory in (self.cwd, self.build, self.system, self.scratch, self.fallback):
            directory.mkdir()
        self.targets = {name: self.system / name for name in SYSTEM}
        self.targets[OWN] = self.build / OWN
        for number, (name, target) in enumerate(self.targets.items()):
            target.write_text("selected {} {}\n".format(number, name))
            (self.fallback / name).write_text("wrong ambient file\n")
        executable = self.build / "packed-scale-test"
        executable.write_text(STUB)
        executable.chmod(0o755)
        self.proof = self.root / "invocation proof.json"

    def invoke(self, arguments, expected=0, stub_exit=0):
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("GST_", "LD_"))}
        environment.update(TMPDIR=str(self.scratch), PACKED_LOADER_EXIT=str(stub_exit),
                           PACKED_LOADER_PROOF=str(self.proof),
                           PACKED_LOADER_TARGETS=json.dumps(
                               {name: str(path) for name, path in self.targets.items()}))
        for name in ("GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0",
                     "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0"):
            environment[name] = str(self.fallback)
        result = subprocess.run(["sh", str(CHECK), *map(str, arguments)],
                                cwd=str(self.cwd), env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(result.returncode, expected, result.stderr)
        self.assertEqual(list(self.scratch.iterdir()), [], "temporary loader files leaked")

    def assert_selected(self):
        self.assertEqual(json.loads(self.proof.read_text()),
                         {name: path.read_text() for name, path in self.targets.items()})

    def test_absolute_paths_and_spaces_select_exact_files(self):
        self.invoke([self.build, self.system])
        self.assert_selected()

    def test_relative_paths_and_spaces_select_exact_files(self):
        self.invoke([os.path.relpath(self.build, self.cwd),
                     os.path.relpath(self.system, self.cwd)])
        self.assert_selected()

    def test_failure_propagates_after_executing_selected_stub_and_cleans(self):
        self.invoke([self.build, self.system], expected=37, stub_exit=37)
        self.assert_selected()

    def test_missing_selected_plugin_never_uses_ambient_fallback_and_cleans(self):
        self.targets[SYSTEM[-1]].unlink()
        self.invoke([self.build, self.system], expected=1)
        self.assertFalse(self.proof.exists(), "test executable ran after missing dependency")

    def test_missing_own_plugin_never_uses_ambient_fallback(self):
        self.targets[OWN].unlink()
        self.invoke([self.build, self.system], expected=1)
        self.assertFalse(self.proof.exists(), "test ran without selected own plugin")

    def test_explicit_empty_plugin_path_never_uses_ambient_fallback(self):
        self.invoke([self.build, ""], expected=1)
        self.assertFalse(self.proof.exists(), "empty selected path used ambient plugins")

    def test_bad_argument_count_rejects_before_execution(self):
        self.invoke([self.build, self.system, "extra"], expected=2)
        self.assertFalse(self.proof.exists())


if __name__ == "__main__":
    unittest.main()
