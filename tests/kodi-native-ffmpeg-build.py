#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the private FFmpeg build helper without compiling external code."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "tools/kodi-route-probe/build-native-ffmpeg.sh"
LIBRARIES = {
    "avcodec": 63, "avdevice": 63, "avfilter": 12, "avformat": 63,
    "avutil": 61, "swresample": 7, "swscale": 10,
}

FAKE_TOOL = r'''
import json
import os
from pathlib import Path
import sys

root = Path(os.environ["FIXTURE_ROOT"])
name = Path(sys.argv[0]).name
args = sys.argv[1:]
with (root / "actions.jsonl").open("a", encoding="utf-8") as log:
    log.write(json.dumps({"tool": name, "args": args, "cwd": os.getcwd()}) + "\n")
failure = os.environ.get("FIXTURE_FAILURE", "")
if name == "git":
    assert args[:2] == ["-C", str(root / "source checkout")]
    if args[2:] == ["rev-parse", "HEAD"]:
        print("0" * 40 if failure == "revision" else
              "12cc1f42f06ff0e85633c2602c96786fbd280b70")
    else:
        assert args[2:] == ["diff", "--quiet", "HEAD", "--"]
        sys.exit(1 if failure == "dirty" else 0)
elif name == "pkg-config":
    assert args == ["--exists", "libdrm", "libva", "zlib"]
    sys.exit(1 if failure == "dependencies" else 0)
elif name == "configure":
    (Path.cwd() / "configure.args.json").write_text(json.dumps(args), encoding="utf-8")
    sys.exit(1 if failure == "configure" else 0)
elif name == "make":
    if args == ["-j1"]:
        sys.exit(1 if failure == "build" else 0)
    assert len(args) == 2 and args[0] == "install" and args[1].startswith("DESTDIR=")
    stage = Path(args[1].split("=", 1)[1])
    configuration = json.loads((Path.cwd() / "configure.args.json").read_text())
    # Honor configure's prefix as FFmpeg does: a wrong helper prefix cannot
    # accidentally pass merely because this fixture always creates /usr.
    prefixes = [arg.split("=", 1)[1] for arg in configuration if arg.startswith("--prefix=")]
    assert len(prefixes) == 1 and prefixes[0].startswith("/")
    prefix = stage / prefixes[0].lstrip("/")
    if failure == "install-layout":
        prefix = stage / "unexpected-layout"
    lib = prefix / "lib"
    lib.mkdir(parents=True)
    payload = ("configuration: " + " ".join(configuration) + "\n").encode()
    libraries = {"avcodec": 63, "avdevice": 63, "avfilter": 12, "avformat": 63,
                 "avutil": 61, "swresample": 7, "swscale": 10}
    for library, major in libraries.items():
        real = "lib%s.so.%d.1.102" % (library, major)
        (lib / real).write_bytes(payload)
        (lib / ("lib%s.so.%d" % (library, major))).symlink_to(real)
        (lib / ("lib%s.so" % library)).symlink_to(real)
    (prefix / "bin").mkdir()
    for tool in ("ffmpeg", "ffprobe"):
        (prefix / "bin" / tool).write_bytes(payload)
    if failure == "occupied-publish":
        destination = Path(os.environ["FIXTURE_PREFIX"])
        destination.mkdir(parents=True)
        (destination / "existing-user-file").write_text("preserve me", encoding="utf-8")
    sys.exit(1 if failure == "install" else 0)
elif name == "mv":
    assert args[:3] == ["-T", "-n", "--"]
    if failure == "occupied-at-move":
        Path(args[-1]).mkdir()
    os.execv("/usr/bin/mv", ["mv", *args])
else:
    raise AssertionError("unexpected fixture tool: " + name)
'''


class NativeFfmpegBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="crystalhd-native-build-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source checkout"
        self.build = self.root / "build directory"
        self.prefix = self.root / "output parent" / "private prefix"
        self.bin = self.root / "fake tools"
        self.source.mkdir()
        self.bin.mkdir()
        for path in (self.source / "configure", *(self.bin / tool for tool in ("git", "pkg-config", "make", "mv"))):
            path.write_text("#!%s\n%s" % (sys.executable, FAKE_TOOL), encoding="utf-8")
            path.chmod(0o755)
        self.environment = os.environ.copy()
        self.environment.pop("FFMPEG_CPU", None)
        self.environment.update({
            "PATH": str(self.bin) + os.pathsep + os.defpath,
            "FIXTURE_ROOT": str(self.root),
            "FIXTURE_PREFIX": str(self.prefix),
        })

    def execute(self, *, failure="", cpu=None, arguments=None):
        environment = self.environment.copy()
        environment["FIXTURE_FAILURE"] = failure
        if cpu is not None:
            environment["FFMPEG_CPU"] = cpu
        return subprocess.run(
            ["sh", str(HELPER), *(arguments if arguments is not None else
                                  [str(self.source), str(self.build), str(self.prefix)])],
            env=environment, capture_output=True, text=True, check=False,
        )

    def actions(self):
        path = self.root / "actions.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def test_neutral_prefix_staging_and_complete_libraries_with_spaces(self):
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads((self.build / "configure.args.json").read_text())
        self.assertIn("--prefix=/usr", args)
        self.assertIn("--cpu=generic", args)
        for flag in ("--enable-shared", "--disable-static", "--disable-autodetect",
                     "--disable-everything", "--enable-libdrm", "--enable-v4l2-m2m"):
            self.assertIn(flag, args)
        for path in (self.source, self.build, self.prefix):
            self.assertNotIn(str(path), " ".join(args))
        actions = self.actions()
        self.assertEqual([action["tool"] for action in actions],
                         ["git", "git", "pkg-config", "configure", "make", "make", "mv"])
        self.assertEqual(actions[-2]["args"], ["install", "DESTDIR=" + str(self.build / "runtime-stage")])
        self.assertFalse((self.prefix / "usr").exists())
        self.assertFalse((self.build / "runtime-stage/usr").exists())
        for library, major in LIBRARIES.items():
            real = self.prefix / "lib" / ("lib%s.so.%d.1.102" % (library, major))
            soname = self.prefix / "lib" / ("lib%s.so.%d" % (library, major))
            self.assertTrue(real.is_file())
            self.assertEqual(os.readlink(soname), real.name)
            self.assertEqual(soname.read_bytes(), real.read_bytes())
            self.assertNotIn(str(self.root).encode(), real.read_bytes())
        self.assertTrue((self.prefix / "bin/ffmpeg").is_file())
        self.assertTrue((self.prefix / "bin/ffprobe").is_file())

    def test_explicit_cpu_is_preserved(self):
        result = self.execute(cpu="core2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--cpu=core2", json.loads((self.build / "configure.args.json").read_text()))

    def test_output_paths_with_trailing_slashes(self):
        result = self.execute(arguments=[str(self.source), str(self.build) + "/",
                                         str(self.prefix) + "///"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.prefix / "bin/ffmpeg").is_file())
        self.assertFalse((self.prefix / "usr").exists())

    def test_preflight_failures_do_not_create_outputs(self):
        for failure in ("revision", "dirty", "dependencies"):
            with self.subTest(failure=failure):
                result = self.execute(failure=failure)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.build.exists())
                self.assertFalse(self.prefix.exists())
                self.assertNotIn("configure", [action["tool"] for action in self.actions()])

    def test_existing_and_dangling_output_paths_are_rejected(self):
        for path in (self.build, self.prefix):
            path.parent.mkdir(parents=True, exist_ok=True)
            for kind in ("directory", "dangling-symlink"):
                with self.subTest(path=path.name, kind=kind):
                    if kind == "directory":
                        path.mkdir()
                    else:
                        path.symlink_to(self.root / "absent target")
                    result = self.execute()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn("configure", [action["tool"] for action in self.actions()])
                    if kind == "directory":
                        path.rmdir()
                    else:
                        path.unlink()

    def test_trailing_slashes_do_not_hide_dangling_output_symlinks(self):
        for path in (self.build, self.prefix):
            with self.subTest(path=path.name):
                (self.root / "actions.jsonl").unlink(missing_ok=True)
                path.parent.mkdir(parents=True, exist_ok=True)
                target = self.root / (path.name + " absent target")
                path.symlink_to(target)
                arguments = [str(self.source), str(self.build), str(self.prefix)]
                arguments[1 if path == self.build else 2] += "///"
                result = self.execute(arguments=arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("configure", [action["tool"] for action in self.actions()])
                self.assertTrue(path.is_symlink())
                self.assertFalse(target.exists())
                path.unlink()

    def test_overlapping_output_paths_are_rejected_before_build(self):
        actual_parent = self.root / "actual parent"
        actual_parent.mkdir()
        linked_parent = self.root / "linked parent"
        linked_parent.symlink_to(actual_parent, target_is_directory=True)
        cases = (
            ("same", self.root / "same", self.root / "same"),
            ("prefix-parent", self.root / "prefix parent/build", self.root / "prefix parent"),
            ("build-parent", self.root / "build parent", self.root / "build parent/prefix"),
            ("same-dotdot", self.root / "unused/../same alias", self.root / "same alias"),
            ("prefix-parent-dotdot", self.root / "unused nested/../prefix alias/build",
             self.root / "prefix alias"),
            ("build-parent-dotdot", self.root / "build alias",
             self.root / "unused prefix/../build alias/prefix"),
            ("same-symlink-parent", linked_parent / "same", actual_parent / "same"),
            ("prefix-parent-symlink", linked_parent / "tree/build", actual_parent / "tree"),
            ("build-parent-symlink", actual_parent / "build", linked_parent / "build/prefix"),
        )
        for name, build, prefix in cases:
            with self.subTest(case=name):
                (self.root / "actions.jsonl").unlink(missing_ok=True)
                result = self.execute(arguments=[str(self.source), str(build), str(prefix)])
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("configure", [action["tool"] for action in self.actions()])
                self.assertFalse(build.resolve().exists())
                self.assertFalse(prefix.resolve().exists())

    def test_distinct_siblings_through_parent_aliases_are_supported(self):
        actual_parent = self.root / "actual siblings"
        actual_parent.mkdir()
        linked_parent = self.root / "linked siblings"
        linked_parent.symlink_to(actual_parent, target_is_directory=True)
        build = linked_parent / "shared-name"
        prefix = actual_parent / "shared-name-prefix"
        result = self.execute(arguments=[str(self.source), str(build), str(prefix)])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((prefix / "bin/ffmpeg").is_file())
        self.assertFalse((prefix / "usr").exists())

    def test_dotdot_aliases_cannot_bypass_existing_output_checks(self):
        existing = self.root / "existing output"
        existing.mkdir()
        sentinel = existing / "existing-user-file"
        sentinel.write_text("preserve me", encoding="utf-8")
        for role in ("build", "prefix"):
            with self.subTest(role=role):
                (self.root / "actions.jsonl").unlink(missing_ok=True)
                intermediate = self.root / ("missing " + role)
                alias = intermediate / ".." / existing.name
                build = alias if role == "build" else self.build
                prefix = alias if role == "prefix" else self.prefix
                result = self.execute(arguments=[str(self.source), str(build), str(prefix)])
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("configure", [action["tool"] for action in self.actions()])
                self.assertFalse(intermediate.exists())
                self.assertEqual(sentinel.read_text(), "preserve me")

    def test_failed_build_stages_never_publish_partial_prefix(self):
        for failure in ("configure", "build", "install", "install-layout"):
            with self.subTest(failure=failure):
                # Each failed attempt requires new directories, as the helper does.
                self.build = self.root / ("build " + failure)
                result = self.execute(failure=failure)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(self.build.is_dir())
                self.assertFalse(self.prefix.exists())
                actions = self.actions()
                if failure == "configure":
                    self.assertNotIn("make", [action["tool"] for action in actions])

    def test_destination_occupied_during_install_is_preserved(self):
        result = self.execute(failure="occupied-publish")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.prefix / "existing-user-file").read_text(), "preserve me")
        self.assertFalse((self.prefix / "usr").exists())
        self.assertTrue((self.build / "runtime-stage/usr/lib").is_dir())

    def test_empty_destination_created_at_move_is_not_replaced(self):
        result = self.execute(failure="occupied-at-move")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.prefix.is_dir())
        self.assertEqual(list(self.prefix.iterdir()), [])
        self.assertTrue((self.build / "runtime-stage/usr/lib").is_dir())

    def test_argument_contract_fails_before_tools_run(self):
        for arguments in ([], [str(self.source), str(self.build)],
                          ["relative-source", str(self.build), str(self.prefix)],
                          [str(self.source), "relative-build", str(self.prefix)],
                          [str(self.source), str(self.build), "relative-prefix"]):
            with self.subTest(arguments=arguments):
                result = self.execute(arguments=arguments)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(self.actions(), [])


if __name__ == "__main__":
    unittest.main()
