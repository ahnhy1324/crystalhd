#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Differential grammar/resolution and native-reader/launcher regression tests.

No hardware, installed files or user defaults are touched. CXX and
NATIVE_CONFIG_CXXFLAGS permit the same suite on a scalar i686 toolchain.
"""
import importlib.util
import json
import os
from pathlib import Path
import random
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("config_core", str(ROOT / "scripts/crystalhd-config.py"))
CORE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CORE)
HEADER = "[crystalhd]\nschema = 1\n"
DEBUG = "CRYSTALHD_VAAPI_DEBUG"
TRACE = "CRYSTALHD_VAAPI_TRACE"
LIVE = "CRYSTALHD_VAAPI_LIVE_H264"
PREFEED = "CRYSTALHD_VAAPI_LOW_LATENCY_H264"
RX = "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT"
DISABLE = CORE.NATIVE_CONFIG_DISABLE_ENVIRONMENT


def encoded(value):
    if value is CORE.ABSENT:
        return "a"
    if value is CORE.UNSET:
        return "u"
    if type(value) is bool:
        return "b" + str(int(value))
    if type(value) is int:
        return "i" + str(value)
    return "s" + value.encode("utf-8", "surrogatepass").hex()


def entries(section, layer):
    return {section + ":" + k: encoded(v) for k, v in layer.items()}


def unpack(line):
    return dict(field.split("=", 1) for field in line.strip().split("\t") if "=" in field)


class NativeConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="crystalhd-native-config-build-")
        cls.binary = Path(cls.build.name) / "native-config-test"
        command = shlex.split(os.environ.get("CXX", "g++"))
        command += ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-O2"]
        command += shlex.split(os.environ.get("NATIVE_CONFIG_CXXFLAGS", ""))
        command += ["-I" + str(ROOT / "include"), str(ROOT / "tests/native-config.cpp"),
                    "-Wl,--wrap=open,--wrap=geteuid,--wrap=getegid", "-o", str(cls.binary)]
        subprocess.check_call(command)
        cls.command = shlex.split(os.environ.get("NATIVE_CONFIG_RUNNER", "")) + [str(cls.binary)]

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="crystalhd-native-config-")
        self.root = Path(self.temp.name)
        self.system = self.root / "system.ini"
        self.user_dir = self.root / "xdg/crystalhd"
        self.user_dir.mkdir(parents=True, mode=0o700)
        self.user = self.user_dir / "config.ini"
        self.environment = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                            "HOME": str(self.root / "home"), "XDG_CONFIG_HOME": str(self.root / "xdg"),
                            "TEST_NATIVE_CONFIG_SYSTEM_DIRECTORY": str(self.root / "absent-system")}
        for name in ("LD_LIBRARY_PATH", "PYTHONHOME", "PYTHONNOUSERSITE"):
            if name in os.environ:
                self.environment[name] = os.environ[name]

    def tearDown(self):
        self.temp.cleanup()

    def write(self, path, text, mode=0o600):
        path.write_bytes(text.encode("utf-8") if type(text) is str else text)
        path.chmod(mode)

    def run_native(self, *arguments, environment=None):
        env = dict(self.environment)
        env.update(environment or {})
        return subprocess.run(self.command + list(arguments), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True, timeout=10)

    def paths(self, names=(DEBUG, TRACE, LIVE, PREFEED, RX), environment=None):
        return self.run_native("paths", str(self.system), str(self.user), *names, environment=environment)

    def test_generated_schema_matches_production(self):
        subprocess.check_call([sys.executable, "-B", str(ROOT / "scripts/generate-native-config.py"), "--check"])

    def test_differential_complete_document_grammar(self):
        corpus = [b"", b"\xef\xbb\xbf" + HEADER.encode(), b"\xff", HEADER.encode(),
                  (HEADER + "[profile.empty]\n").encode()]
        values = ["unset", "true", "false", "0", "1", "8", "16", "128", "129", "1918", "1920",
                  "33333", "9999999999", "10000000000", "01", "-1", "True", '""', '"bounded"',
                  '"strict"', '"live"', '"latest"', '"previous"', '"hardware"', '"software"',
                  '"1280x720"', '"4096x2160"', '"4096x4096"', '"02x4"', '"/tmp/a=b"',
                  '"한글"', '"\\ud800"', '"\\ud83d\\ude00"', '"\\u0000"', '"x" # nope', '"\\q"']
        for key in CORE.RULES:
            for value in values:
                corpus.append((HEADER + "[settings]\n" + key + " = " + value + "\n").encode())
        for separator in ("\n", "\r\n", "\r", "\v", "\f", "\x1c", "\x1d", "\x1e", "\x85", "\u2028", "\u2029"):
            for whitespace in (" ", "\t", "\x1f", "\u2007", "\u3000"):
                corpus.append(separator.join(("[crystalhd]", whitespace + "schema" + whitespace + "=" + whitespace + "1", "[settings]", "vaapi.debug=true")).encode())
        for extra in ("schema=1", "[crystalhd]", "[settings]\nvaapi.debug=true\nvaapi.debug=false",
                      "[other]", "[profile.bad name]", "[profile.A]\nvaapi.debug=2", "[settings]\nunknown=true",
                      "preset=unset", 'profile="missing"', 'preset="custom"', 'preset="streaming"'):
            corpus.append((HEADER + extra + "\n").encode())
        rng = random.Random(400)
        alphabet = 'schema=1[]\n"\\true#;._09\t' + "\u2000\x00"
        for _ in range(400):
            corpus.append((HEADER + ''.join(rng.choice(alphabet) for _ in range(rng.randrange(80)))).encode())
        corpus += [b"#" * 65536, b"#" * 65537, (HEADER + "#" * (65536 - len(HEADER))).encode()]
        result = subprocess.run(self.command + ["batch-parse"],
                                input="\n".join(c.hex() for c in corpus) + "\n",
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
                                env=self.environment, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), len(corpus))
        for data, line in zip(corpus, lines):
            try:
                doc = CORE.parse_document(data)
            except CORE.SchemaError:
                self.assertEqual(line, "FAIL", repr(data[:200]))
                continue
            self.assertTrue(line.startswith("OK\t"), repr(data[:200]))
            expected = entries("crystalhd", doc.metadata)
            expected.update(entries("settings", doc.settings))
            for name, layer in doc.profiles.items():
                expected.update(entries("profile." + name, layer))
            self.assertEqual(unpack(line), expected, repr(data[:200]))
        print("native/CLI grammar: {0} differential documents PASS".format(len(corpus)))

    def test_differential_merge_unset_selectors_and_dependencies(self):
        catalog = CORE.load_preset_catalog()
        cases = [
            (HEADER, HEADER),
            (HEADER + '[settings]\nvaapi.debug=true\nvaapi.h264.mode="strict"', HEADER + '[settings]\nvaapi.debug=false'),
            (HEADER + '[settings]\nvaapi.debug=true', HEADER + '[settings]\nvaapi.debug=unset'),
            (HEADER + 'profile="desk"\n[profile.desk]\nvaapi.debug=true\nvaapi.trace=true', HEADER + '[settings]\nvaapi.debug=false\n[profile.desk]\nvaapi.debug=unset'),
            (HEADER + 'preset="streaming"', HEADER + 'preset="quality"'),
            (HEADER, HEADER + 'profile="missing"'),
            (HEADER, HEADER + '[settings]\nvaapi.h264.ready_remap=true'),
            (HEADER, HEADER + '[settings]\nchromium.experimental_hw_decode=true'),
            (HEADER, HEADER + '[settings]\nplayer.decoder="software"\ngstreamer.crystalhddec.scale_width=720'),
            (HEADER, HEADER + '[settings]\nplayer.software_output_size="1920x1080"'),
            (HEADER, HEADER + '[settings]\ngstreamer.crystalhddec.scale_width=720\nplayer.software_output_size="1920x1080"'),
            (HEADER, HEADER + '[profile.inactive]\nvaapi.h264.ready_remap=true'),
        ]
        for preset in CORE.PRESET_NAMES:
            cases.append((HEADER, HEADER + 'preset=' + json.dumps(preset)))
        for system, user in cases:
            self.write(self.system, system); self.write(self.user, user)
            native = self.run_native("resolve", str(self.system), str(self.user))
            s, u = CORE.parse_document(system.encode()), CORE.parse_document(user.encode())
            try:
                resolution = CORE.resolve(s, u, catalog.selectable)
            except CORE.SchemaError:
                self.assertEqual(native.stdout, "FAIL\n", (system, user))
                continue
            expected = entries("values", resolution.requested)
            requests = dict(catalog.selectable[resolution.preset])
            requests.update(s.settings); requests.update(u.settings)
            if resolution.profile is not None:
                requests.update(s.profiles.get(resolution.profile, {}))
                requests.update(u.profiles.get(resolution.profile, {}))
            expected.update(entries("requests", requests))
            expected["ack"] = str(int(bool(catalog.by_name[resolution.preset].required_acknowledgements)))
            self.assertEqual(unpack(native.stdout), expected, (system, user))

    def test_native_defaults_and_raw_environment_precedence(self):
        self.write(self.system, HEADER + '[settings]\nvaapi.debug=true\nvaapi.h264.mode="strict"')
        self.write(self.user, HEADER + '[settings]\nvaapi.debug=unset\nvaapi.trace=true\nlibcrystalhd.rx_pool_count=16')
        result = self.paths()
        self.assertEqual(result.stderr, "")
        expected = {DEBUG: "<absent>", TRACE: "1", LIVE: "0", PREFEED: "<absent>", RX: "16"}
        self.assertEqual(dict(line.split("=", 1) for line in result.stdout.splitlines()), expected)
        for raw in ("", "0", "invalid-native"):
            result = self.paths(environment={DEBUG: raw, TRACE: raw, LIVE: raw, PREFEED: raw, RX: raw})
            self.assertEqual(dict(line.split("=", 1) for line in result.stdout.splitlines()),
                             {name: raw for name in expected})

    def test_invalid_transaction_never_partially_applies_safe_keys(self):
        self.write(self.system, HEADER + '[settings]\nvaapi.trace=true')
        for invalid in ('vaapi.debug=1', 'unknown=true', 'vaapi.h264.ready_remap=true',
                        'libcrystalhd.rx_pool_count=8', 'vaapi.h264.mode="live"',
                        'vaapi.h264.low_latency=true', 'vaapi.i915_wc=true'):
            self.write(self.user, HEADER + '[settings]\nvaapi.debug=true\n' + invalid)
            result = self.paths(environment={DEBUG: "0"})
            self.assertTrue(result.stdout.startswith("FAIL:"), invalid)
            self.assertIn(TRACE + "=<absent>", result.stdout, invalid)
            self.assertIn(DEBUG + "=0", result.stdout, invalid)
        for preset in ("streaming", "low-memory"):
            self.write(self.user, HEADER + 'preset="' + preset + '"\n')
            self.assertIn("preset-requires-config-run", self.paths().stdout)

    def test_explicit_native_experiments_preserve_encoding(self):
        self.write(self.user, HEADER + '[settings]\nvaapi.debug=true\nlibcrystalhd.rx_pool_count=8\nvaapi.h264.mode="live"\nvaapi.h264.low_latency=true')
        env = {RX: "8", LIVE: "1", PREFEED: "0"}
        result = self.paths(environment=env)
        self.assertNotIn("FAIL", result.stdout)
        for name, value in dict(env, **{DEBUG: "1"}).items():
            self.assertIn(name + "=" + value, result.stdout)

    def test_secure_reader_missing_empty_symlink_special_mode_owner_size(self):
        self.assertNotIn("FAIL", self.paths().stdout)
        self.write(self.user, "")
        self.assertIn("FAIL", self.paths().stdout)
        for mode in (0o622, 0o646, 0o666):
            self.write(self.user, HEADER, mode)
            self.assertIn("unsafe-file", self.paths().stdout)
        self.write(self.user, HEADER)
        self.user_dir.chmod(0o777)
        self.assertIn("unsafe-directory", self.paths().stdout)
        self.user_dir.chmod(0o700)
        self.user.unlink(); self.user.symlink_to(self.system)
        self.assertIn("nonregular-file", self.paths().stdout)
        self.user.unlink(); os.mkfifo(str(self.user))
        self.assertIn("nonregular-file", self.paths().stdout)
        self.user.unlink(); self.write(self.user, b"#" * 65537)
        self.assertIn("oversize", self.paths().stdout)
        if os.getuid() == 0:
            self.write(self.user, HEADER); os.chown(str(self.user), 65534, -1)
            self.assertIn("unsafe-file", self.paths().stdout)
            os.chown(str(self.user), 0, -1)
        self.user.unlink(); self.user_dir.rmdir()
        self.user_dir.symlink_to(self.root)
        self.assertIn("unsafe-directory", self.paths().stdout)

    def test_native_cached_snapshot_errno_and_redacted_failure_once(self):
        self.write(self.user, HEADER + '[settings]\nvaapi.debug=true')
        result = self.run_native("snapshot", str(self.user))
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "1:1\n", ""))
        self.write(self.user, HEADER + '[settings]\nsecret-DO-NOT-LOG=true')
        result = self.run_native("native", DEBUG, TRACE, LIVE)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr.count("managed defaults ignored"), 1)
        self.assertNotIn("secret", result.stderr)
        self.assertNotIn(str(self.root), result.stderr)

    def test_raw_presence_is_checked_at_existing_native_use_boundaries(self):
        self.write(self.user, HEADER + '[settings]\nvaapi.debug=true')
        result = self.run_native("raw-change", environment={DEBUG: "0"})
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "0:1\n", ""))

    def test_different_effective_ids_ignore_managed_files_but_preserve_native_env(self):
        self.write(self.user, HEADER + '[settings]\nvaapi.debug=true')
        for variable in ("TEST_NATIVE_CONFIG_UID_MISMATCH", "TEST_NATIVE_CONFIG_GID_MISMATCH"):
            result = self.run_native("native", DEBUG, TRACE, environment={variable: "1", TRACE: "1"})
            self.assertEqual((result.returncode, result.stderr), (0, ""))
            self.assertIn(DEBUG + "=<absent>", result.stdout)
            self.assertIn(TRACE + "=1", result.stdout)

    def test_inspector_explains_bypass_without_treating_it_as_a_setting(self):
        self.write(self.user, HEADER + '[settings]\nvaapi.debug=true')
        env = dict(self.environment, **{DISABLE: "1"})
        for command in ("show-effective", "bug-report"):
            argv = [sys.executable, "-B", str(ROOT / "scripts/crystalhd-config.py"), command, "--no-system-config"]
            if command == "show-effective":
                argv += ["--format", "json"]
            result = subprocess.run(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["native_file_reading"], "bypassed-by-environment")
            self.assertIn("configured values remain an inspection model", report["state_scope"])
            self.assertEqual(len(report["settings"]), len(CORE.RULES))

    def test_bypass_and_path_locators(self):
        self.write(self.user, HEADER + '[settings]\nvaapi.debug=true')
        result = self.run_native("native", DEBUG, environment={DISABLE: "1"})
        self.assertEqual(result.stdout, DEBUG + "=<absent>\n")
        for value in ("0", "", "invalid"):
            self.assertEqual(self.run_native("native", DEBUG, environment={DISABLE: value}).stdout, DEBUG + "=1\n")
        self.assertIn("nonabsolute-xdg", self.run_native("native", DEBUG, environment={"XDG_CONFIG_HOME": "relative"}).stderr)
        self.assertIn("nonabsolute-home", self.run_native("native", DEBUG, environment={"XDG_CONFIG_HOME": "", "HOME": "relative"}).stderr)
        home_dir = self.root / "home/.config/crystalhd"
        home_dir.mkdir(parents=True, mode=0o700)
        self.write(home_dir / "config.ini", HEADER + '[settings]\nvaapi.trace=true')
        result = self.run_native("native", DEBUG, TRACE, environment={"XDG_CONFIG_HOME": ""})
        self.assertIn(TRACE + "=1", result.stdout)
        self.assertIn(DEBUG + "=<absent>", result.stdout)

    def test_wrapper_false_unset_suppression_and_alternate_file_cannot_reactivate(self):
        self.write(self.user, HEADER + '[settings]\nvaapi.debug=true\nvaapi.trace=true')
        alternate = self.root / "alternate.ini"
        self.write(alternate, HEADER)
        cases = [(["--override", "vaapi.debug=false"], False, True),
                 (["--override", "vaapi.debug=unset"], False, True),
                 (["--no-user-config"], False, False),
                 (["--user-config", str(alternate)], False, False)]
        for options, debug, trace in cases:
            result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/crystalhd-config.py"),
                                     "run", "--no-system-config"] + options + ["--"] + self.command + ["native", DEBUG, TRACE],
                                    env=self.environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    universal_newlines=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(DEBUG + ("=1" if debug else "=<absent>"), result.stdout)
            self.assertIn(TRACE + ("=1" if trace else "=<absent>"), result.stdout)
        self.assertEqual(self.user.read_text(), HEADER + '[settings]\nvaapi.debug=true\nvaapi.trace=true')


if __name__ == "__main__":
    unittest.main()
