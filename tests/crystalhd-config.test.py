#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Device-free black-box and security tests for crystalhd-config."""

from __future__ import annotations

import ast
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/crystalhd-config.py"

SPEC = importlib.util.spec_from_file_location("crystalhd_config_cli", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot import crystalhd-config")
CONFIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONFIG)


class ConfigCliTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="crystalhd-config-test-")
        self.root = Path(self.temp.name)
        os.chmod(str(self.root), 0o700)
        self.user_dir = self.root / "user"
        self.user_dir.mkdir(mode=0o700)
        self.user_file = self.user_dir / "config.ini"

    def tearDown(self):
        self.temp.cleanup()

    def write_user(self, content, mode=0o600):
        self.user_file.write_text(content, encoding="utf-8")
        os.chmod(str(self.user_file), mode)

    def run_cli(self, *arguments, environment=None, script=SCRIPT):
        env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(self.root / "unused-home"),
            "LC_ALL": "C",
        }
        if environment:
            env.update(environment)
        return subprocess.run(
            [sys.executable, "-B", str(script)] + list(arguments),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
            check=False,
        )

    def common(self):
        return ("--no-system-config", "--no-user-config")

    def test_no_file_defaults_are_complete_and_deterministic(self):
        arguments = ("show-effective",) + self.common() + (
            "--ignore-environment", "--format", "json",
        )
        first = self.run_cli(*arguments)
        second = self.run_cli(*arguments)
        self.assertEqual((first.returncode, first.stderr), (0, ""))
        self.assertEqual(first.stdout, second.stdout)
        report = json.loads(first.stdout)
        self.assertEqual(report["schema"], 1)
        self.assertEqual(report["preset"], "default")
        self.assertIsNone(report["profile"])
        self.assertEqual(report["environment"], "ignored")
        self.assertIn("application CLI and GObject overrides are not observed",
                      report["observation_limit"])
        self.assertIn("no application, device, backend, kernel module",
                      report["state_scope"])
        settings = {item["key"]: item for item in report["settings"]}
        self.assertEqual(set(settings), set(CONFIG.RULES))
        self.assertEqual(len(settings), 21)
        self.assertEqual(settings["vaapi.h264.mode"]["effective"], "bounded")
        self.assertEqual(settings["player.decoder"]["effective"], "hardware")
        self.assertEqual(settings["player.decoder"]["state"],
                         "conditional:not-inspected:device-plugin-and-media")
        self.assertEqual(settings["chromium.drm_device"]["state"],
                         "inactive:hardware-mode-disabled")
        self.assertEqual(settings["chromium.executable"]["effective"], None)

    def test_system_user_profile_precedence_and_explicit_selector(self):
        self.write_user(
            "[crystalhd]\n"
            "schema = 1\n"
            'profile = "stream"\n'
            "[settings]\n"
            'vaapi.h264.mode = "live"\n'
            "[profile.stream]\n"
            "vaapi.h264.low_latency = true\n"
        )
        result = self.run_cli(
            "show-effective", "--no-system-config", "--user-config",
            str(self.user_file), "--ignore-environment", "--format", "json",
        )
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        report = json.loads(result.stdout)
        self.assertEqual(report["profile"], "stream")
        settings = {item["key"]: item for item in report["settings"]}
        self.assertEqual(settings["vaapi.h264.mode"]["origin"], "user")
        self.assertEqual(settings["vaapi.h264.low_latency"]["origin"],
                         "user-profile:stream")

        result = self.run_cli(
            "validate", "--no-system-config", "--user-config",
            str(self.user_file), "--profile", "missing", "--ignore-environment",
        )
        self.assertEqual(result.returncode, 3)
        self.assertIn("unknown selected profile", result.stderr)

    def test_only_empty_default_preset_exists(self):
        accepted = self.run_cli("validate", *self.common(), "--preset", "default")
        rejected = self.run_cli("validate", *self.common(), "--preset", "quality")
        self.assertEqual(accepted.returncode, 0)
        self.assertEqual(rejected.returncode, 3)
        self.assertIn("unknown selected preset", rejected.stderr)

    def test_environment_is_last_observable_layer_and_can_be_ignored(self):
        self.write_user(
            "[crystalhd]\nschema = 1\n[settings]\n"
            'vaapi.h264.mode = "live"\n'
            "vaapi.h264.low_latency = true\n"
        )
        environment = {"CRYSTALHD_VAAPI_LIVE_H264": "not-secret-but-redact-it"}
        base = (
            "show-effective", "--no-system-config", "--user-config",
            str(self.user_file), "--format", "json",
        )
        observed = self.run_cli(*base, environment=environment)
        self.assertEqual(observed.returncode, 0)
        self.assertNotIn(environment["CRYSTALHD_VAAPI_LIVE_H264"], observed.stdout)
        report = json.loads(observed.stdout)
        settings = {item["key"]: item for item in report["settings"]}
        mode = settings["vaapi.h264.mode"]
        self.assertEqual(mode["effective"], "strict")
        self.assertEqual(mode["origin"],
                         "environment:CRYSTALHD_VAAPI_LIVE_H264")
        self.assertEqual(mode["environment_receipt"]["raw"], "<redacted>")
        self.assertFalse(settings["vaapi.h264.low_latency"]["effective"])
        self.assertEqual(settings["vaapi.h264.low_latency"]["state"],
                         "inactive:requires-live-fast-path")

        ignored = self.run_cli(*(base + ("--ignore-environment",)),
                               environment=environment)
        self.assertEqual(ignored.returncode, 0)
        ignored_settings = {
            item["key"]: item for item in json.loads(ignored.stdout)["settings"]
        }
        self.assertEqual(ignored_settings["vaapi.h264.mode"]["effective"], "live")

    def test_environment_invalid_values_and_native_rejection_are_receipts(self):
        result = self.run_cli(
            "show-effective", *self.common(), "--format", "json",
            environment={
                "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT": "private-rx-value",
                "CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE": "1",
                "CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX": "0",
            },
        )
        self.assertEqual(result.returncode, 0)
        self.assertNotIn("private-rx-value", result.stdout)
        report = json.loads(result.stdout)
        settings = {item["key"]: item for item in report["settings"]}
        self.assertEqual(settings["libcrystalhd.rx_pool_count"]["effective"], 16)
        self.assertEqual(settings["libcrystalhd.rx_pool_count"]["requested"],
                         "<invalid-environment-value>")
        self.assertEqual(report["native_rejections"], [{
            "component": "crystalhd-chromium",
            "reason": "missing-gpu-sandbox-acknowledgement",
        }])

    def test_hardware_dependent_requests_are_never_reported_as_proven(self):
        self.write_user(
            "[crystalhd]\nschema = 1\n[settings]\n"
            "libcrystalhd.rx_pool_count = 8\n"
            "vaapi.i915_wc = true\n"
            "gstreamer.crystalhddec.scale_width = 1280\n"
            'player.software_output_size = "1920x1080"\n'
        )
        result = self.run_cli(
            "show-effective", "--no-system-config", "--user-config",
            str(self.user_file), "--ignore-environment", "--format", "json",
        )
        self.assertEqual(result.returncode, 0)
        settings = {item["key"]: item for item in json.loads(result.stdout)["settings"]}
        for key in (
                "libcrystalhd.rx_pool_count", "vaapi.i915_wc",
                "gstreamer.crystalhddec.scale_width",
                "player.software_output_size", "player.decoder"):
            self.assertIn("not-inspected", settings[key]["state"])

    def test_paths_are_redacted_but_compiled_paths_remain_explainable(self):
        private_path = str(self.root / "private browser path")
        result = self.run_cli(
            "explain", "chromium.executable", *self.common(), "--format", "json",
            environment={"CRYSTALHD_CHROMIUM": private_path},
        )
        self.assertEqual(result.returncode, 0)
        self.assertNotIn(private_path, result.stdout)
        setting = json.loads(result.stdout)["settings"][0]
        self.assertEqual(setting["requested"], "<redacted-path>")
        self.assertEqual(setting["effective"], "<redacted-path>")

        compiled = self.run_cli(
            "explain", "player.python", *self.common(),
            "--ignore-environment", "--format", "json",
        )
        self.assertEqual(json.loads(compiled.stdout)["settings"][0]["effective"],
                         "/usr/bin/python3")

        store = self.run_cli(
            "explain", "chromium.password_store", *self.common(),
            "--format", "json",
            environment={"CRYSTALHD_CHROMIUM_PASSWORD_STORE": "private-store"},
        )
        self.assertNotIn("private-store", store.stdout)
        self.assertEqual(json.loads(store.stdout)["settings"][0]["effective"],
                         "<redacted-environment-value>")

    def test_unknown_key_is_cli_error_and_invalid_config_is_data_error(self):
        missing_command = self.run_cli()
        self.assertEqual(missing_command.returncode, 2)

        unknown = self.run_cli("explain", "unknown.setting", *self.common())
        self.assertEqual(unknown.returncode, 2)
        self.assertIn("invalid command line", unknown.stderr)

        self.write_user(
            "[crystalhd]\nschema = 1\n[settings]\n"
            "vaapi.h264.low_latency = true\n"
        )
        invalid = self.run_cli(
            "validate", "--no-system-config", "--user-config", str(self.user_file),
            "--ignore-environment",
        )
        self.assertEqual(invalid.returncode, 3)
        self.assertIn("low latency requires live", invalid.stderr)
        self.assertNotIn(str(self.root), invalid.stderr)

    def test_safe_reader_rejects_symlink_modes_types_and_large_input(self):
        valid = "[crystalhd]\nschema = 1\n"
        self.write_user(valid)
        self.assertEqual(
            CONFIG.read_config(str(self.user_file), "user configuration"),
            valid.encode("utf-8"),
        )

        link = self.user_dir / "link.ini"
        link.symlink_to(self.user_file)
        with self.assertRaisesRegex(CONFIG.SchemaError, "must not be a symlink"):
            CONFIG.read_config(str(link), "user configuration")

        os.chmod(str(self.user_file), 0o622)
        with self.assertRaisesRegex(CONFIG.SchemaError, "group/other writable"):
            CONFIG.read_config(str(self.user_file), "user configuration")
        os.chmod(str(self.user_file), 0o600)

        os.chmod(str(self.user_dir), 0o722)
        with self.assertRaisesRegex(CONFIG.SchemaError, "directory is group/other writable"):
            CONFIG.read_config(str(self.user_file), "user configuration")
        os.chmod(str(self.user_dir), 0o700)

        directory_as_file = self.user_dir / "not-a-file"
        directory_as_file.mkdir(mode=0o700)
        with self.assertRaisesRegex(CONFIG.SchemaError, "not a regular file"):
            CONFIG.read_config(str(directory_as_file), "user configuration")

        fifo = self.user_dir / "not-a-blocking-fifo"
        os.mkfifo(str(fifo), 0o600)
        real_open = os.open
        opened_paths = []

        def tracking_open(path, *arguments, **keywords):
            opened_paths.append(path)
            return real_open(path, *arguments, **keywords)

        with mock.patch.object(CONFIG.os, "open", side_effect=tracking_open):
            with self.assertRaisesRegex(CONFIG.SchemaError, "not a regular file"):
                CONFIG.read_config(str(fifo), "user configuration")
        self.assertEqual(opened_paths, [str(self.user_dir)])

        opened_paths = []
        with mock.patch.object(CONFIG.os, "open", side_effect=tracking_open):
            with self.assertRaisesRegex(CONFIG.SchemaError, "not a regular file"):
                CONFIG.read_config("/dev/null", "system configuration", system=True)
        self.assertEqual(opened_paths, ["/dev"])

        opened_paths = []
        block_entry = mock.Mock(st_mode=stat.S_IFBLK | 0o600)
        with mock.patch.object(CONFIG.os, "stat", return_value=block_entry):
            with mock.patch.object(CONFIG.os, "open", side_effect=tracking_open):
                with self.assertRaisesRegex(CONFIG.SchemaError,
                                            "not a regular file"):
                    CONFIG.read_config(str(self.user_file), "user configuration")
        self.assertEqual(opened_paths, [str(self.user_dir)])

        self.user_file.write_bytes(b"#" * (CONFIG.MAXIMUM_BYTES + 1))
        os.chmod(str(self.user_file), 0o600)
        with self.assertRaisesRegex(CONFIG.SchemaError, "exceeds 64 KiB"):
            CONFIG.read_config(str(self.user_file), "user configuration")

    def test_missing_file_is_an_empty_layer_but_unsafe_parent_is_not(self):
        missing = self.user_dir / "missing.ini"
        self.assertIsNone(CONFIG.read_config(str(missing), "user configuration"))
        os.chmod(str(self.user_dir), 0o733)
        with self.assertRaises(CONFIG.SchemaError):
            CONFIG.read_config(str(missing), "user configuration")

    def test_mid_read_io_error_is_configuration_error(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        with mock.patch.object(CONFIG.os, "read", side_effect=OSError(5, "I/O")):
            with self.assertRaisesRegex(CONFIG.SchemaError,
                                        "cannot be read safely"):
                CONFIG.read_config(str(self.user_file), "user configuration")

    def test_bom_utf8_and_schema_errors_report_role_not_private_path(self):
        cases = (
            b"\xef\xbb\xbf[crystalhd]\nschema = 1\n",
            b"\xff",
            b"[crystalhd]\nschema = 2\n",
        )
        for payload in cases:
            with self.subTest(payload=payload):
                self.user_file.write_bytes(payload)
                os.chmod(str(self.user_file), 0o600)
                result = self.run_cli(
                    "validate", "--no-system-config", "--user-config",
                    str(self.user_file), "--ignore-environment",
                )
                self.assertEqual(result.returncode, 3)
                self.assertIn("user configuration", result.stderr)
                self.assertNotIn(str(self.root), result.stderr)

    def test_malformed_input_is_bounded_and_redacted(self):
        private = "/home/private/person"
        cases = (
            "[crystalhd]\nschema = " + "9" * 5000 + "\n",
            "[crystalhd]\nschema = 1\n[settings]\n"
            "player.software_output_size = \"" + "9" * 5000 + "x2\"\n",
            "[crystalhd]\nschema = 1\n[" + private + "]\n",
            "[crystalhd]\nschema = 1\n[settings]\n" + private + " = true\n",
            "[crystalhd]\nschema = 1\n[\x1b[2J\x1b[H]\n",
            "[crystalhd]\nschema = 1\nprivateidentifier = true\n",
            "[crystalhd]\nschema = 1\n[settings]\n"
            "privateidentifier = true\n",
            "[crystalhd]\nschema = 1\n[profile.privateidentifier]\n"
            "[profile.privateidentifier]\n",
        )
        for payload in cases:
            with self.subTest(payload_length=len(payload)):
                self.write_user(payload)
                result = self.run_cli(
                    "validate", "--no-system-config", "--user-config",
                    str(self.user_file), "--ignore-environment",
                )
                self.assertEqual(result.returncode, 3)
                self.assertNotIn(private, result.stderr)
                self.assertNotIn("\x1b", result.stderr)
                self.assertNotIn("privateidentifier", result.stderr)
                self.assertNotIn("Traceback", result.stderr)

        for option in ("--preset", "--profile"):
            result = self.run_cli(
                "validate", *self.common(), option, private + "\nsecret",
                "--ignore-environment",
            )
            self.assertEqual(result.returncode, 3)
            self.assertNotIn(private, result.stderr)
            self.assertNotIn("secret", result.stderr)

            result = self.run_cli(
                "validate", *self.common(), option, "privateidentifier",
                "--ignore-environment",
            )
            self.assertEqual(result.returncode, 3)
            self.assertNotIn("privateidentifier", result.stderr)

        result = self.run_cli(
            "explain", private + "\nsecret", *self.common(),
            "--ignore-environment",
        )
        self.assertEqual(result.returncode, 2)
        self.assertNotIn(private, result.stderr)
        self.assertNotIn("secret", result.stderr)

        cli_private = private + "\x1b[2J"
        cli_cases = (
            ("show-effective", "--format", cli_private),
            ("explain", "vaapi.trace", *self.common(), cli_private),
        )
        for arguments in cli_cases:
            with self.subTest(arguments=arguments[:-1]):
                result = self.run_cli(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertIn("invalid command line", result.stderr)
                self.assertNotIn(private, result.stderr)
                self.assertNotIn("\x1b", result.stderr)

    def test_default_user_path_and_no_file_switch(self):
        config_dir = self.root / ".config" / "crystalhd"
        config_dir.mkdir(parents=True, mode=0o700)
        config = config_dir / "config.ini"
        config.write_text(
            "[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = true\n",
            encoding="utf-8",
        )
        os.chmod(str(config), 0o600)
        selected = self.run_cli(
            "explain", "vaapi.trace", "--no-system-config",
            "--ignore-environment", "--format", "json",
            environment={"HOME": str(self.root)},
        )
        self.assertEqual(selected.returncode, 0)
        self.assertTrue(json.loads(selected.stdout)["settings"][0]["effective"])

        skipped = self.run_cli(
            "explain", "vaapi.trace", *self.common(),
            "--ignore-environment", "--format", "json",
            environment={"HOME": str(self.root)},
        )
        self.assertFalse(json.loads(skipped.stdout)["settings"][0]["effective"])

    def test_relative_explicit_and_environment_paths_are_rejected(self):
        explicit_cases = (
            ("--no-system-config", "--user-config", "relative.ini"),
            ("--no-system-config", "--user-config", ""),
            ("--system-config", "", "--no-user-config"),
        )
        for options in explicit_cases:
            with self.subTest(options=options):
                explicit = self.run_cli(
                    "validate", *options, "--ignore-environment",
                )
                self.assertEqual(explicit.returncode, 3)
                self.assertIn("path must be absolute", explicit.stderr)

        relative_xdg = self.run_cli(
            "validate", "--no-system-config", "--ignore-environment",
            environment={"XDG_CONFIG_HOME": "relative-xdg"},
        )
        self.assertEqual(relative_xdg.returncode, 3)
        self.assertIn("XDG_CONFIG_HOME must be absolute", relative_xdg.stderr)

        relative_home = self.run_cli(
            "validate", "--no-system-config", "--ignore-environment",
            environment={"HOME": "relative-home"},
        )
        self.assertEqual(relative_home.returncode, 3)
        self.assertIn("HOME must be absolute", relative_home.stderr)

    def test_text_output_and_validation_have_stable_scope(self):
        show = self.run_cli(
            "show-effective", *self.common(), "--ignore-environment",
        )
        self.assertEqual(show.returncode, 0)
        self.assertIn("process environment is the last observable layer", show.stdout)
        self.assertEqual(sum(1 for line in show.stdout.splitlines()
                             if " = " in line), 21)
        validate = self.run_cli("validate", *self.common(), "--ignore-environment")
        self.assertEqual(validate.stdout,
                         "configuration valid: preset=default profile=none; "
                         "environment=ignored\n")

    def test_archive_copy_is_self_contained_and_python36_syntax_compatible(self):
        source = SCRIPT.read_text(encoding="utf-8")
        try:
            ast.parse(source, filename=str(SCRIPT), feature_version=(3, 6))
        except TypeError:
            ast.parse(source, filename=str(SCRIPT))
        imports = set()
        for node in ast.walk(ast.parse(source)):
            if isinstance(node, ast.Import):
                imports.update(alias.name.split(".")[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                imports.add((node.module or "").split(".")[0])
        self.assertEqual(imports, {
            "argparse", "errno", "json", "os", "re", "stat", "sys",
        })

        archive_dir = self.root / "archive-without-git"
        archive_dir.mkdir(mode=0o700)
        copy = archive_dir / "crystalhd-config"
        shutil.copyfile(str(SCRIPT), str(copy))
        os.chmod(str(copy), 0o755)
        result = self.run_cli(
            "show-effective", *self.common(), "--ignore-environment",
            script=copy,
        )
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        self.assertNotIn(str(ROOT), result.stdout)


def main():
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ConfigCliTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print("crystalhd-config tests: PASS (device-free; 21 keys; Python 3.6 syntax)")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
