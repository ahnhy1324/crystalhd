#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Device-free black-box and security tests for crystalhd-config."""

from __future__ import annotations

import ast
import errno
import fcntl
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/crystalhd-config.py"
MANIFEST = ROOT / "share/crystalhd/config-presets-v1.json"

SPEC = importlib.util.spec_from_file_location("crystalhd_config_cli", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot import crystalhd-config")
CONFIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONFIG)
PROVENANCE_SPEC = importlib.util.spec_from_file_location(
    "crystalhd_provenance_test", ROOT / "scripts/crystalhd_provenance.py")
PROVENANCE = importlib.util.module_from_spec(PROVENANCE_SPEC)
PROVENANCE_SPEC.loader.exec_module(PROVENANCE)


class ConfigCliTest(unittest.TestCase):
    def test_provenance_build_id_reader_does_not_block_on_fifo(self):
        fifo = self.root / 'provenance-fifo'
        os.mkfifo(str(fifo))
        program = (
            'import importlib.util,sys;'
            'spec=importlib.util.spec_from_file_location("provenance",sys.argv[1]);'
            'module=importlib.util.module_from_spec(spec);'
            'spec.loader.exec_module(module);'
            'module.gnu_build_id(sys.argv[2])'
        )
        result = subprocess.run(
            [sys.executable, '-B', '-c', program,
             str(ROOT / 'scripts/crystalhd_provenance.py'), str(fifo)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not a regular file', result.stderr)

    def test_default_provenance_pending_path_is_absolute_and_authoritative(self):
        expected = '/var/lib/crystalhd/runtime-install.pending'
        with mock.patch.object(CONFIG.os.path, 'lexists', return_value=True) as exists:
            report = CONFIG.inspect_runtime_provenance()
        exists.assert_called_once_with(expected)
        self.assertEqual(report['state'], 'incomplete')

    def test_installation_provenance_is_separate_from_reader_declarations(self):
        stage = self.root / 'installed'
        source_id = 'git:' + ('0' * 40)
        for relative in ('usr/lib/libcrystalhd.so.3.6',
                         'usr/lib/gstreamer-1.0/libgstcrystalhd.so',
                         'usr/lib/dri/crystalhd_drv_video.so'):
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile('/bin/true', str(target))
        for relative in ('usr/bin/crystalhd-play',
                         'usr/share/crystalhd/player.py'):
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('# CrystalHD source identity: ' + source_id + '\n')
        version = stage / 'usr/share/crystalhd/VERSION'
        version.write_text('same-version\n')
        receipt = PROVENANCE.create_receipt(
            stage, 'same-version', '/usr/bin', '/usr/share', source_id)
        helper = stage / 'usr/share/crystalhd/provenance.py'
        shutil.copyfile(str(ROOT/'scripts/crystalhd_provenance.py'), str(helper))
        before = sorted(str(path.relative_to(stage)) for path in stage.rglob('*'))
        with mock.patch.object(CONFIG, 'PROVENANCE_HELPER_RELATIVE', str(helper)):
            report = CONFIG.inspect_runtime_provenance(str(stage))
        after = sorted(str(path.relative_to(stage)) for path in stage.rglob('*'))
        self.assertEqual(after, before)
        self.assertEqual(report['state'], 'match')
        self.assertIn('running mappings not inspected', report['scope'])
        messages, failed = PROVENANCE.verify_receipt(
            receipt, stage, 'same-version', {
                'gstreamer': '/usr/lib/gstreamer-1.0/libgstcrystalhd.so',
            })
        self.assertTrue(failed)
        self.assertTrue(any('GStreamer selection' in message
                            for level, message in messages))
        external_wrapper = self.root / 'external-crystalhd-play'
        external_wrapper.write_text('# CrystalHD source identity: ' +
                                    source_id + '\n')
        messages, failed = PROVENANCE.verify_receipt(
            receipt, stage, 'same-version', {
                'player': [external_wrapper,
                           stage / 'usr/share/crystalhd/player.py'],
            })
        self.assertTrue(failed)
        self.assertTrue(any('Player selection' in message
                            for level, message in messages))

        plugin = stage / 'usr/lib/gstreamer-1.0/libgstcrystalhd.so'
        plugin.unlink()
        plugin.symlink_to(plugin.name)
        with mock.patch.object(CONFIG, 'PROVENANCE_HELPER_RELATIVE', str(helper)):
            report = CONFIG.inspect_runtime_provenance(str(stage))
        self.assertEqual(report['state'], 'mismatch')
        self.assertTrue(any('GStreamer provenance: missing' in item['text']
                            for item in report['messages']))
        plugin.unlink()
        shutil.copyfile('/bin/true', str(plugin))

        shutil.copyfile('/bin/false',
                        str(plugin))
        with mock.patch.object(CONFIG, 'PROVENANCE_HELPER_RELATIVE', str(helper)):
            report = CONFIG.inspect_runtime_provenance(str(stage))
        self.assertEqual(report['state'], 'mismatch')
        self.assertTrue(any('GStreamer provenance' in item['text']
                            for item in report['messages']))

        pending = stage / 'var/lib/crystalhd/runtime-install.pending'
        pending.parent.mkdir(parents=True)
        pending.write_text('{}\n')
        with mock.patch.object(CONFIG, 'PROVENANCE_HELPER_RELATIVE', str(helper)):
            self.assertEqual(CONFIG.inspect_runtime_provenance(str(stage))['state'],
                             'incomplete')
        pending.unlink()
        receipt.unlink()
        with mock.patch.object(CONFIG, 'PROVENANCE_HELPER_RELATIVE', str(helper)):
            self.assertEqual(CONFIG.inspect_runtime_provenance(str(stage))['state'],
                             'unknown')
        receipt.symlink_to(stage/'missing-receipt')
        with mock.patch.object(CONFIG, 'PROVENANCE_HELPER_RELATIVE', str(helper)):
            self.assertEqual(CONFIG.inspect_runtime_provenance(str(stage))['state'],
                             'mismatch')

    def test_installation_declarations_do_not_execute_components_or_load_config(self):
        root = self.root / 'reader-files'
        root.mkdir()
        elf = b'\x7fELF' + bytes((CONFIG.struct.calcsize('P') // 4,))
        dil = root / 'libcrystalhd.so.3'
        libva = root / 'libva.so.2'
        va = root / 'dri' / 'crystalhd_drv_video.so'
        va.parent.mkdir()
        dil.write_bytes(elf + b'\nCrystalHD managed defaults v1:dil\n')
        libva.write_bytes(elf)
        va.write_bytes(elf + b'\nCrystalHD managed defaults v2:vaapi\n')
        player = root / 'controller with spaces.py'
        player.write_text('# CrystalHD managed defaults v1:player\nraise RuntimeError("must not execute")\n')
        wrapper = root / 'crystalhd-play'
        wrapper.write_text('#!/bin/sh\n# CrystalHD Player controller-location protocol v1\n'
                           "player_controller_relative='controller with spaces.py'\nexit 99\n")
        wrapper.chmod(0o755)
        cache = '\n'.join('{0} (libc6) => {1}'.format(p.name, p) for p in (dil, libva))
        environment = {'PATH': str(root), 'LD_PRELOAD': 'secret', 'LIBVA_DRIVER_NAME': 'other'}
        with mock.patch.object(CONFIG, 'load_document', side_effect=AssertionError('must not read INI')):
            report = CONFIG.inspect_installation(environment, cache_text=cache)
        self.assertEqual([r['state'] for r in report['readers']], ['declared-v1', 'unknown', 'declared-v1'])
        self.assertEqual(report['readers'][2]['keys'], list(CONFIG.READER_KEYS['player']))
        self.assertEqual(report['overrides_present'], ['LD_PRELOAD', 'LIBVA_DRIVER_NAME'])
        self.assertNotIn('secret', json.dumps(report))
        self.assertIn('running applications are not inspected', CONFIG.installation_text(report))
        wrapper.write_text('#!/bin/sh\nexit 99\n')
        self.assertIsNone(CONFIG._player_reader_path(str(wrapper)))
        # A symlinked BINDIR must retain kernel path traversal through '..';
        # lexical normalization would inspect a different controller.
        realbin = root / 'real' / 'bin'
        realbin.mkdir(parents=True)
        (root / 'real' / 'share').mkdir()
        controller = root / 'real' / 'share' / 'controller@home.py'
        controller.write_text('# CrystalHD managed defaults v1:player\n')
        linkedbin = root / 'linked-bin'
        linkedbin.symlink_to(realbin, target_is_directory=True)
        wrapper = realbin / 'crystalhd-play'
        wrapper.write_text('#!/bin/sh\n# CrystalHD Player controller-location protocol v1\n'
                           "player_controller_relative='../share/controller@home.py'\nexit 99\n")
        path = CONFIG._player_reader_path(str(linkedbin / wrapper.name))
        self.assertEqual(CONFIG.inspect_reader(path, 'player')['state'], 'declared-v1')

    def test_installation_regular_bounded_files_symlinks_and_wrong_abi(self):
        target = self.root / 'reader'
        target.write_bytes(b'\x7fELF\x01\nCrystalHD managed defaults v1:dil\n')
        link = self.root / 'reader-link'
        link.symlink_to(target)
        self.assertEqual(CONFIG.inspect_reader(str(link), 'dil', 1)['state'], 'declared-v1')
        self.assertEqual(CONFIG.inspect_reader(str(link), 'dil', 2)['state'], 'unknown')
        self.assertEqual(CONFIG.inspect_reader(str(self.root), 'dil')['state'], 'unreadable')
        fifo = self.root / 'fifo'
        os.mkfifo(str(fifo))
        self.assertEqual(CONFIG.inspect_reader(str(fifo), 'dil')['state'], 'unreadable')
        with self.assertRaises(ValueError):
            CONFIG._inspection_bytes(str(target), maximum=4)
        target.unlink()
        self.assertEqual(CONFIG.inspect_reader(str(link), 'dil')['state'], 'unreadable')

    def test_installation_fixed_readonly_cache_invocation_and_failure(self):
        with mock.patch.object(CONFIG.os.path, 'isfile', return_value=True), \
                mock.patch.object(CONFIG.subprocess, 'run') as run:
            run.return_value = subprocess.CompletedProcess([], 0, b'cache', b'')
            self.assertEqual(CONFIG._linker_cache(), ('cache', 'read'))
            args, kwargs = run.call_args
            self.assertEqual(args[0], ['/sbin/ldconfig', '-p'])
            self.assertEqual(kwargs['env'], {'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
            self.assertEqual(kwargs['timeout'], 2)
            run.side_effect = subprocess.TimeoutExpired('ldconfig', 2)
            self.assertIn('timed out', CONFIG._linker_cache()[1])

    def test_installation_cli_is_readonly_even_with_invalid_user_config(self):
        self.write_user('not an ini file')
        with mock.patch.object(CONFIG, 'inspect_installation', return_value={'scope': 'test'}), \
                mock.patch('sys.stdout', new_callable=io.StringIO) as output:
            self.assertEqual(CONFIG.main(['installation', '--format', 'json'], environment={}), 0)
        self.assertEqual(json.loads(output.getvalue()), {'scope': 'test'})
        self.assertEqual(self.user_file.read_text(), 'not an ini file')

    def test_named_profile_cli_edits_only_target_and_checks_inactive_profile(self):
        self.write_user('[crystalhd]\nschema=1\n[settings]\nvaapi.debug=true\n'
                        '[profile.keep]\nvaapi.trace=true\n')
        for value in ('false', 'unset'):
            result = self.run_cli('save', '--profile-section', 'desk', 'vaapi.debug='+value,
                                  *self.writer_options())
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = CONFIG.parse_document(self.user_file.read_bytes())
            self.assertTrue(doc.settings['vaapi.debug'])
            self.assertEqual(doc.profiles['keep'], {'vaapi.trace': True})
            self.assertNotIn('profile', doc.metadata)
        result = self.run_cli('save', '--profile-section', 'desk', '--remove', 'vaapi.debug',
                              '--profile', 'desk', *self.writer_options())
        self.assertEqual(result.returncode, 0, result.stderr)
        doc = CONFIG.parse_document(self.user_file.read_bytes())
        self.assertEqual(doc.profiles['desk'], {})
        self.assertEqual(doc.metadata['profile'], 'desk')
        original = self.user_file.read_bytes()
        for args in [('--profile-section', 'bad name'),
                     ('--profile-section', 'inactive', 'vaapi.h264.ready_remap=true'),
                     ('--profile-section', 'desk', '--remove', 'vaapi.debug', 'vaapi.debug=false')]:
            result = self.run_cli('save', *args, *self.writer_options())
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(self.user_file.read_bytes(), original)

    def test_delete_profile_keeps_system_definition_and_clears_user_only_selector(self):
        header = '[crystalhd]\nschema=1\n'
        self.write_user(header+'profile="desk"\n[profile.desk]\nvaapi.debug=false\n'
                        '[profile.keep]\nvaapi.trace=true\n')
        system = self.root/'system.ini'
        system.write_text(header+'[profile.desk]\nvaapi.debug=true\n', encoding='utf-8')
        system.chmod(0o600)
        # A system layer must be root-owned; use the exact shared operation for
        # merge semantics without weakening production ownership checks.
        doc = CONFIG.parse_document(self.user_file.read_bytes())
        system_doc = CONFIG.parse_document(system.read_bytes())
        candidate = CONFIG.delete_user_profile(doc, 'desk', system_doc)
        self.assertEqual(candidate.metadata['profile'], 'desk')
        resolved = CONFIG.resolve(system_doc, candidate, CONFIG.load_preset_catalog().selectable)
        self.assertTrue(resolved.effective['vaapi.debug'])
        self.assertIn('desk', doc.profiles)  # mutation is transactional
        result = self.run_cli('delete-profile', 'desk', *self.writer_options())
        self.assertEqual(result.returncode, 0, result.stderr)
        saved = CONFIG.parse_document(self.user_file.read_bytes())
        self.assertNotIn('profile', saved.metadata)
        self.assertEqual(saved.profiles, {'keep': {'vaapi.trace': True}})
        original = self.user_file.read_bytes()
        result = self.run_cli('delete-profile', 'desk', *self.writer_options())
        self.assertEqual(result.returncode, 3)
        self.assertEqual(self.user_file.read_bytes(), original)

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

    def writer_options(self):
        return ("--no-system-config", "--user-config", str(self.user_file))

    def user_snapshot(self, reset=False):
        return CONFIG.load_user_document_for_edit(str(self.user_file), reset=reset)

    def save_document(self, document, snapshot=None):
        if snapshot is None:
            snapshot = self.user_snapshot()
        return CONFIG.save_user_document(
            str(self.user_file), document, snapshot, system_path=None,
        )

    def launch_plan(self, user=None, environment=None, overrides=None,
                    preset=None, profile=None, acknowledgements=(), system=None):
        return CONFIG.prepare_launch(
            CONFIG.empty_document() if system is None else system,
            CONFIG.empty_document() if user is None else user,
            CONFIG.load_preset_catalog(), {} if environment is None else environment,
            explicit_preset=preset, explicit_profile=profile, overrides=overrides,
            acknowledgements=acknowledgements,
        )

    def environment_child(self):
        return [sys.executable, "-c",
                "import json,os; print(json.dumps(sorted((k.decode('latin1'),"
                "v.decode('latin1')) for k,v in os.environb.items())))"]

    def test_no_file_defaults_are_complete_and_deterministic(self):
        arguments = ("show-effective",) + self.common() + (
            "--ignore-environment", "--format", "json",
        )
        first = self.run_cli(*arguments)
        second = self.run_cli(*arguments)
        self.assertEqual((first.returncode, first.stderr), (0, ""))
        self.assertEqual(first.stdout, second.stdout)
        missing = self.run_cli(
            "show-effective", "--no-system-config", "--user-config",
            str(self.user_dir / "missing.ini"), "--ignore-environment",
            "--format", "json",
        )
        self.assertEqual((missing.returncode, missing.stderr), (0, ""))
        self.assertEqual(missing.stdout, first.stdout)
        report = json.loads(first.stdout)
        self.assertEqual(report["schema"], 1)
        self.assertEqual(report["preset"], "default")
        self.assertEqual(report["custom_overrides"], [])
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
        self.assertFalse(settings["vaapi.h264.low_latency"]["effective"])
        self.assertEqual(settings["vaapi.h264.low_latency"]["state"],
                         "explicit-prefeed:not-requested")
        self.assertEqual(settings["player.decoder"]["effective"], "hardware")
        self.assertEqual(settings["player.decoder"]["state"],
                         "conditional:not-inspected:device-plugin-and-media")
        self.assertEqual(settings["chromium.drm_device"]["state"],
                         "inactive:hardware-mode-disabled")
        self.assertEqual(settings["chromium.executable"]["effective"], None)

    def test_centered_native_only_environment_reports_partial_scope_and_redacts(self):
        variable = "CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12"
        for raw in ("1280x720@1920x1080:limited", "0", "",
                    "private-unvalidated-centered-value"):
            for command in ("show-effective", "bug-report"):
                with self.subTest(raw=raw, command=command):
                    result = self.run_cli(command, *self.common(),
                                          *(() if command == "bug-report" else
                                            ("--format", "json")),
                                          environment={variable: raw})
                    self.assertEqual((result.returncode, result.stderr), (0, ""))
                    report = json.loads(result.stdout)
                    self.assertEqual(report["effective_configuration_scope"],
                                     "partial:experimental-centered-nv12-native-only")
                    self.assertIn("VA context rejection rules are not modeled",
                                  report["state_scope"])
                    self.assertEqual(report["native_only_environment"], [{
                        "variable": variable,
                        "raw": "<redacted>",
                        "state": "not-modeled:experimental-centered-nv12",
                        "native_value_validation": "not-performed",
                    }])
                    self.assertEqual({item["key"] for item in report["settings"]},
                                     set(CONFIG.RULES))
                    self.assertEqual(report["custom_overrides"], [])
                    if len(raw) > 1:
                        self.assertNotIn(raw, result.stdout + result.stderr)
        private = "private-native-centered-report-value"
        for command in ("show-effective", "validate", "explain"):
            arguments = (("vaapi.h264.mode",) if command == "explain" else ())
            with self.subTest(command=command):
                result = self.run_cli(command, *arguments, *self.common(),
                                      environment={variable: private})
                self.assertEqual((result.returncode, result.stderr), (0, ""))
                self.assertIn("native-only environment " + variable, result.stdout)
                self.assertIn("raw=<redacted>", result.stdout)
                self.assertIn("not modeled", result.stdout)
                self.assertNotIn(private, result.stdout + result.stderr)
        ignored = self.run_cli("show-effective", *self.common(),
                               "--ignore-environment", "--format", "json",
                               environment={variable: private})
        self.assertEqual((ignored.returncode, ignored.stderr), (0, ""))
        report = json.loads(ignored.stdout)
        self.assertEqual(report["native_only_environment"], [])
        self.assertEqual(report["effective_configuration_scope"],
                         "canonical-native-adapters-only")
        self.assertNotIn(variable, ignored.stdout)
        unknown = self.run_cli("show-effective", *self.common(), "--format", "json",
                               environment={variable + "_UNKNOWN": private})
        self.assertEqual((unknown.returncode, unknown.stderr), (0, ""))
        self.assertEqual(json.loads(unknown.stdout)["native_only_environment"], [])
        self.write_user("[crystalhd]\nschema = 1\n[settings]\n"
                        "vaapi.h264.ready_remap = true\n")
        invalid = self.run_cli("validate", "--no-system-config", "--user-config",
                               str(self.user_file), environment={variable: private})
        self.assertEqual(invalid.returncode, 3)
        self.assertIn("READY remap requires", invalid.stderr)
        self.assertNotIn(private, invalid.stdout + invalid.stderr)

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
        self.assertEqual(report["custom_overrides"], [
            "vaapi.h264.low_latency", "vaapi.h264.mode",
        ])
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

    def test_reviewed_preset_inventory_and_nonselectable_entries(self):
        result = self.run_cli("list-presets", "--format", "json")
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        records = json.loads(result.stdout)["presets"]
        self.assertEqual(
            [(item["name"], item["status"]) for item in records],
            [
                ("default", "SELECTABLE"),
                ("quality", "SELECTABLE"),
                ("performance", "UNAVAILABLE"),
                ("low-memory", "SELECTABLE"),
                ("streaming", "SELECTABLE"),
                ("experimental", "UNAVAILABLE"),
                ("custom", "VIRTUAL"),
            ],
        )
        settings = dict((item["name"], item["settings"]) for item in records)
        self.assertEqual(settings["default"], {})
        self.assertEqual(settings["quality"], {
            "gstreamer.crystalhddec.scale_width": 0,
        })
        self.assertIsNone(settings["performance"])
        self.assertIsNone(settings["experimental"])
        self.assertIsNone(settings["custom"])

        quality = self.run_cli(
            "show-effective", *self.common(), "--ignore-environment",
            "--preset", "quality", "--format", "json",
        )
        self.assertEqual((quality.returncode, quality.stderr), (0, ""))
        quality_report = json.loads(quality.stdout)
        self.assertEqual(quality_report["preset"], "quality")
        self.assertEqual(quality_report["custom_overrides"], [])
        scale = next(item for item in quality_report["settings"]
                     if item["key"] == "gstreamer.crystalhddec.scale_width")
        self.assertEqual((scale["effective"], scale["origin"]),
                         (0, "preset:quality"))

        for name, reason in (
                ("performance", "no-universal-media-safe-scaling-target"),
                ("experimental", "no-coherent-mixed-risk-bundle"),
                ("custom", "virtual custom preset cannot be selected")):
            with self.subTest(name=name):
                rejected = self.run_cli(
                    "validate", *self.common(), "--ignore-environment",
                    "--preset", name,
                )
                self.assertEqual(rejected.returncode, 3)
                self.assertIn(reason, rejected.stderr)

    def test_experimental_presets_require_exact_command_acknowledgement(self):
        missing = self.run_cli(
            "validate", *self.common(), "--ignore-environment",
            "--preset", "low-memory",
        )
        self.assertEqual(missing.returncode, 3)
        self.assertIn("low-memory-8-buffer", missing.stderr)
        accepted = self.run_cli(
            "show-effective", *self.common(), "--ignore-environment",
            "--preset", "low-memory",
            "--acknowledge-risk", "low-memory-8-buffer", "--format", "json",
        )
        self.assertEqual((accepted.returncode, accepted.stderr), (0, ""))
        accepted_report = json.loads(accepted.stdout)
        pool = next(item for item in accepted_report["settings"]
                    if item["key"] == "libcrystalhd.rx_pool_count")
        self.assertEqual((pool["effective"], pool["origin"]),
                         (8, "preset:low-memory"))

        streaming = self.run_cli(
            "show-effective", *self.common(), "--ignore-environment",
            "--preset", "streaming",
            "--acknowledge-risk", "bcm70015-live-h264-drain",
            "--format", "json",
        )
        self.assertEqual((streaming.returncode, streaming.stderr), (0, ""))
        streaming_settings = {
            item["key"]: item for item in json.loads(streaming.stdout)["settings"]
        }
        self.assertEqual(streaming_settings["vaapi.h264.mode"]["effective"],
                         "live")
        self.assertTrue(
            streaming_settings["vaapi.h264.low_latency"]["effective"]
        )
        self.assertFalse(
            streaming_settings["vaapi.h264.ready_remap"]["effective"]
        )

        for arguments, expected_status in (
            (("--preset", "default", "--acknowledge-risk",
              "low-memory-8-buffer"), 3),
            (("--preset", "low-memory", "--acknowledge-risk", "unknown"), 2),
            (("--preset", "low-memory", "--acknowledge-risk",
              "low-memory-8-buffer", "--acknowledge-risk",
              "low-memory-8-buffer"), 2),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_cli(
                    "validate", *self.common(), "--ignore-environment", *arguments
                )
                self.assertEqual(result.returncode, expected_status)
                self.assertNotIn("Traceback", result.stderr)

        self.write_user(
            "[crystalhd]\nschema = 1\n"
            'preset = "low-memory"\n'
        )
        from_file = self.run_cli(
            "validate", "--no-system-config", "--user-config",
            str(self.user_file), "--ignore-environment",
        )
        self.assertEqual(from_file.returncode, 3)
        self.assertIn("--acknowledge-risk", from_file.stderr)

    def test_manifest_parser_is_strict_and_bounded(self):
        raw = MANIFEST.read_bytes()
        catalog = CONFIG.parse_preset_manifest(raw)
        self.assertEqual(tuple(item.name for item in catalog.entries),
                         CONFIG.PRESET_NAMES)
        document = json.loads(raw.decode("utf-8"))
        mutations = []
        changed = dict(document)
        changed["format_version"] = 2
        mutations.append(changed)
        changed = dict(document)
        changed["extra"] = True
        mutations.append(changed)
        changed = json.loads(raw.decode("utf-8"))
        changed["presets"][1]["settings"] = {
            "gstreamer.crystalhddec.scale_width": 1281,
        }
        mutations.append(changed)
        changed = json.loads(raw.decode("utf-8"))
        changed["presets"][4]["required_acknowledgements"] = []
        mutations.append(changed)
        changed = json.loads(raw.decode("utf-8"))
        changed["presets"][5]["status"] = "SELECTABLE"
        changed["presets"][5]["settings"] = {
            "vaapi.h264.ready_remap": True,
        }
        changed["presets"][5]["reason_code"] = None
        mutations.append(changed)
        changed = json.loads(raw.decode("utf-8"))
        changed["presets"][0]["display_name"] = chr(0xd800)
        mutations.append(changed)
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(CONFIG.SchemaError):
                CONFIG.parse_preset_manifest(
                    json.dumps(mutation).encode("utf-8")
                )
        duplicate = raw.replace(
            b'"format": "crystalhd-presets",',
            b'"format": "crystalhd-presets", "format": "duplicate",',
            1,
        )
        with self.assertRaises(CONFIG.SchemaError):
            CONFIG.parse_preset_manifest(duplicate)
        with self.assertRaises(CONFIG.SchemaError):
            CONFIG.parse_preset_manifest(b"[" * 2000 + b"0" + b"]" * 2000)
        with self.assertRaises(CONFIG.SchemaError):
            CONFIG.parse_preset_manifest(b"#" * (CONFIG.MAXIMUM_BYTES + 1))

    def test_managed_finite_prefeed_does_not_select_live_mode(self):
        for mode in ("bounded", "strict"):
            with self.subTest(mode=mode):
                self.write_user(
                    "[crystalhd]\nschema = 1\n[settings]\n"
                    f'vaapi.h264.mode = "{mode}"\n'
                    "vaapi.h264.low_latency = true\n"
                )
                base = ("--no-system-config", "--user-config",
                        str(self.user_file), "--ignore-environment")
                valid = self.run_cli("validate", *base)
                self.assertEqual((valid.returncode, valid.stderr), (0, ""))
                result = self.run_cli("show-effective", *base, "--format", "json")
                self.assertEqual((result.returncode, result.stderr), (0, ""))
                settings = {item["key"]: item for item in json.loads(result.stdout)["settings"]}
                self.assertEqual(settings["vaapi.h264.mode"]["effective"], mode)
                self.assertTrue(settings["vaapi.h264.low_latency"]["effective"])
                self.assertEqual(settings["vaapi.h264.low_latency"]["state"],
                                 "conditional:not-inspected:bcm70015-h264-prefeed")

    def test_environment_is_last_observable_layer_and_can_be_ignored(self):
        self.write_user(
            "[crystalhd]\nschema = 1\n[settings]\n"
            'vaapi.h264.mode = "live"\n'
            "vaapi.h264.low_latency = true\n"
            "vaapi.h264.ready_remap = true\n"
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
        self.assertTrue(settings["vaapi.h264.low_latency"]["effective"])
        self.assertEqual(settings["vaapi.h264.low_latency"]["state"],
                         "conditional:not-inspected:bcm70015-h264-prefeed")
        self.assertFalse(settings["vaapi.h264.ready_remap"]["effective"])
        self.assertEqual(settings["vaapi.h264.ready_remap"]["state"],
                         "inactive:requires-live-low-latency")

        ignored = self.run_cli(*(base + ("--ignore-environment",)),
                               environment=environment)
        self.assertEqual(ignored.returncode, 0)
        ignored_settings = {
            item["key"]: item for item in json.loads(ignored.stdout)["settings"]
        }
        self.assertEqual(ignored_settings["vaapi.h264.mode"]["effective"], "live")
        self.assertTrue(ignored_settings["vaapi.h264.low_latency"]["effective"])
        self.assertTrue(ignored_settings["vaapi.h264.ready_remap"]["effective"])

    def test_raw_finite_prefeed_reports_request_without_activating_ready(self):
        for raw_live, mode in ((None, "bounded"), ("0", "strict"),
                               ("private-live-value", "strict")):
            with self.subTest(live=raw_live):
                environment = {
                    "CRYSTALHD_VAAPI_LOW_LATENCY_H264": "1",
                    "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP": "1",
                }
                if raw_live is not None:
                    environment["CRYSTALHD_VAAPI_LIVE_H264"] = raw_live
                result = self.run_cli("show-effective", *self.common(),
                                      "--format", "json", environment=environment)
                self.assertEqual((result.returncode, result.stderr), (0, ""))
                self.assertNotIn("private-live-value", result.stdout)
                settings = {item["key"]: item for item in json.loads(result.stdout)["settings"]}
                self.assertEqual(settings["vaapi.h264.mode"]["effective"], mode)
                prefeed = settings["vaapi.h264.low_latency"]
                self.assertTrue(prefeed["effective"])
                self.assertEqual(prefeed["origin"],
                                 "environment:CRYSTALHD_VAAPI_LOW_LATENCY_H264")
                self.assertEqual(prefeed["environment_receipt"]["raw"], "<redacted>")
                self.assertEqual(prefeed["state"],
                                 "conditional:not-inspected:bcm70015-h264-prefeed")
                self.assertFalse(settings["vaapi.h264.ready_remap"]["effective"])
                self.assertEqual(settings["vaapi.h264.ready_remap"]["state"],
                                 "inactive:requires-live-low-latency")

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

    def test_bug_report_redacts_private_values_and_profile_identifiers(self):
        private_profile = "private-profile-name"
        private_store = "private-config-string"
        private_path = str(self.root / "private browser path")
        private_environment = "private-environment-string"
        self.write_user(
            "[crystalhd]\n"
            "schema = 1\n"
            'profile = "{0}"\n'
            "[settings]\n"
            'chromium.password_store = "{1}"\n'
            'chromium.executable = "{2}"\n'
            "[profile.{0}]\n"
            "vaapi.trace = true\n"
            "chromium.profile_dir = unset\n".format(
                private_profile, private_store, private_path
            )
        )
        arguments = (
            "--no-system-config", "--user-config", str(self.user_file),
        )

        normal = self.run_cli(
            "show-effective", *arguments, "--ignore-environment",
            "--format", "json",
        )
        self.assertEqual((normal.returncode, normal.stderr), (0, ""))
        normal_report = json.loads(normal.stdout)
        self.assertEqual(normal_report["profile"], private_profile)
        normal_settings = {
            item["key"]: item for item in normal_report["settings"]
        }
        self.assertEqual(
            normal_settings["chromium.password_store"]["effective"],
            private_store,
        )
        self.assertEqual(
            normal_settings["vaapi.trace"]["origin"],
            "user-profile:" + private_profile,
        )

        exported = self.run_cli(
            "bug-report", *arguments,
            environment={"CRYSTALHD_VAAPI_LIVE_H264": private_environment},
        )
        self.assertEqual((exported.returncode, exported.stderr), (0, ""))
        for private_value in (
                private_profile, private_store, private_path,
                private_environment, str(self.root)):
            self.assertNotIn(private_value, exported.stdout)
        report = json.loads(exported.stdout)
        self.assertEqual(
            report["report_kind"],
            "crystalhd-configuration-bug-report-v1",
        )
        self.assertEqual(report["profile"], "<redacted-selected-profile>")
        self.assertIn("no application, device, backend, kernel module",
                      report["state_scope"])
        settings = {item["key"]: item for item in report["settings"]}
        self.assertEqual(
            settings["chromium.password_store"]["requested"],
            "<redacted-setting-value>",
        )
        self.assertEqual(
            settings["chromium.password_store"]["effective"],
            "<redacted-setting-value>",
        )
        self.assertEqual(
            settings["chromium.executable"]["effective"],
            "<redacted-path>",
        )
        self.assertEqual(
            settings["vaapi.trace"]["origin"],
            "user-profile:<redacted>",
        )
        self.assertEqual(
            settings["chromium.profile_dir"]["origin"],
            "user-profile:<redacted>:unset",
        )
        live = settings["vaapi.h264.mode"]
        self.assertEqual(
            live["origin"], "environment:CRYSTALHD_VAAPI_LIVE_H264"
        )
        self.assertEqual(live["environment_receipt"]["raw"], "<redacted>")

        rejected_format = self.run_cli("bug-report", *arguments, "--format", "json")
        self.assertEqual(rejected_format.returncode, 2)
        self.assertIn("invalid command line", rejected_format.stderr)

    def test_black_box_explicit_false_and_unset_remain_distinct(self):
        self.write_user(
            "[crystalhd]\n"
            "schema = 1\n"
            "[settings]\n"
            "vaapi.trace = false\n"
            "chromium.profile_dir = unset\n"
        )
        result = self.run_cli(
            "show-effective", "--no-system-config", "--user-config",
            str(self.user_file), "--ignore-environment", "--format", "json",
        )
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        settings = {
            item["key"]: item for item in json.loads(result.stdout)["settings"]
        }
        self.assertFalse(settings["vaapi.trace"]["effective"])
        self.assertEqual(settings["vaapi.trace"]["origin"], "user")
        self.assertIsNone(settings["chromium.profile_dir"]["effective"])
        self.assertEqual(
            settings["chromium.profile_dir"]["origin"], "user:unset"
        )

    def test_unknown_key_is_cli_error_and_invalid_config_is_data_error(self):
        missing_command = self.run_cli()
        self.assertEqual(missing_command.returncode, 2)

        unknown = self.run_cli("explain", "unknown.setting", *self.common())
        self.assertEqual(unknown.returncode, 2)
        self.assertIn("invalid command line", unknown.stderr)

        self.write_user(
            "[crystalhd]\nschema = 1\n[settings]\n"
            "vaapi.h264.low_latency = true\n"
            "vaapi.h264.ready_remap = true\n"
        )
        invalid = self.run_cli(
            "validate", "--no-system-config", "--user-config", str(self.user_file),
            "--ignore-environment",
        )
        self.assertEqual(invalid.returncode, 3)
        self.assertIn("READY remap requires live low-latency", invalid.stderr)
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

    def test_permission_denied_open_is_configuration_error(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        real_open = os.open

        def deny_file(path, *arguments, **keywords):
            if path == self.user_file.name and "dir_fd" in keywords:
                raise OSError(errno.EACCES, "permission denied")
            return real_open(path, *arguments, **keywords)

        with mock.patch.object(CONFIG.os, "open", side_effect=deny_file):
            with self.assertRaisesRegex(CONFIG.SchemaError,
                                        "cannot be opened safely"):
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

    def test_user_save_reads_back_in_fresh_process_without_environment_flattening(self):
        result = self.run_cli(
            "save", "vaapi.trace=false", "vaapi.h264.ready_delay_us=0",
            'chromium.password_store=""', "chromium.profile_dir=unset",
            *self.writer_options(),
            environment={
                "CRYSTALHD_VAAPI_TRACE": "1",
                "CRYSTALHD_VAAPI_DEBUG": "private-debug-value",
                "CRYSTALHD_CHROMIUM_PROFILE_DIR": "/private/environment-profile",
            },
        )
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        self.assertIn("SAVED ONLY", result.stdout)
        self.assertEqual(stat.S_IMODE(self.user_file.stat().st_mode), 0o600)
        self.assertEqual(self.user_file.stat().st_uid, os.getuid())
        saved = CONFIG.parse_document(self.user_file.read_bytes())
        self.assertEqual(set(saved.settings), {
            "vaapi.trace", "vaapi.h264.ready_delay_us",
            "chromium.password_store", "chromium.profile_dir",
        })
        self.assertIs(saved.settings["vaapi.trace"], False)
        self.assertEqual(type(saved.settings["vaapi.h264.ready_delay_us"]), int)
        self.assertEqual(saved.settings["vaapi.h264.ready_delay_us"], 0)
        self.assertEqual(saved.settings["chromium.password_store"], "")
        self.assertIs(saved.settings["chromium.profile_dir"], CONFIG.UNSET)
        self.assertNotIn(b"private-debug-value", self.user_file.read_bytes())
        self.assertNotIn(b"environment-profile", self.user_file.read_bytes())

        fresh = self.run_cli(
            "show-effective", *self.writer_options(), "--ignore-environment", "--format", "json",
        )
        self.assertEqual((fresh.returncode, fresh.stderr), (0, ""))
        settings = {item["key"]: item for item in json.loads(fresh.stdout)["settings"]}
        self.assertEqual((settings["vaapi.trace"]["effective"], settings["vaapi.trace"]["origin"]),
                         (False, "user"))
        self.assertEqual(settings["vaapi.h264.ready_delay_us"]["origin"], "user")
        self.assertEqual(settings["chromium.password_store"]["effective"], "")
        self.assertEqual(settings["chromium.profile_dir"]["origin"], "user:unset")
        self.assertEqual(settings["vaapi.debug"]["origin"], "compiled")

    def test_user_save_creates_private_default_home_directories_only_in_the_fixture(self):
        home = self.root / "fresh-home"
        home.mkdir(mode=0o700)
        result = self.run_cli("save", "vaapi.trace=true", "--no-system-config",
                              environment={"HOME": str(home)})
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        config_home = home / ".config"
        directory = config_home / "crystalhd"
        user_file = directory / "config.ini"
        self.assertEqual(stat.S_IMODE(config_home.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(user_file.stat().st_mode), 0o600)
        self.assertIs(CONFIG.parse_document(user_file.read_bytes()).settings["vaapi.trace"], True)
        self.assertEqual(set(home.iterdir()), {config_home})

    def test_user_save_patches_base_and_removes_selectors_without_flattening_profiles(self):
        self.write_user(
            '[crystalhd]\nschema = 1\npreset = "quality"\nprofile = "desk"\n'
            '[settings]\nvaapi.trace = true\nvaapi.debug = false\n'
            '[profile.desk]\nvaapi.h264.low_latency = true\n'
        )
        result = self.run_cli(
            "save", "vaapi.debug=true", "--remove", "vaapi.trace",
            "--remove", "preset", "--remove", "profile", *self.writer_options(),
        )
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        saved = CONFIG.parse_document(self.user_file.read_bytes())
        self.assertEqual(saved.metadata, {"schema": 1})
        self.assertEqual(saved.settings, {"vaapi.debug": True})
        self.assertEqual(saved.profiles, {"desk": {"vaapi.h264.low_latency": True}})
        selected = self.run_cli("save", "--preset", "quality", "--profile", "desk",
                                *self.writer_options())
        self.assertEqual((selected.returncode, selected.stderr), (0, ""))
        self.assertEqual(CONFIG.parse_document(self.user_file.read_bytes()).metadata,
                         {"schema": 1, "preset": "quality", "profile": "desk"})

    def test_user_save_does_not_persist_transient_risk_acknowledgements(self):
        saved = self.run_cli(
            "save", "--preset", "low-memory", "--acknowledge-risk", "low-memory-8-buffer",
            *self.writer_options(),
            environment={"CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT": "16"},
        )
        self.assertEqual((saved.returncode, saved.stderr), (0, ""))
        document = CONFIG.parse_document(self.user_file.read_bytes())
        self.assertEqual(document.metadata, {"schema": 1, "preset": "low-memory"})
        self.assertEqual(document.settings, {})
        self.assertNotIn(b"low-memory-8-buffer", self.user_file.read_bytes())
        fresh = self.run_cli("validate", *self.writer_options(), "--ignore-environment")
        self.assertEqual(fresh.returncode, 3)
        self.assertIn("low-memory-8-buffer", fresh.stderr)

    def test_invalid_user_save_preserves_bytes_and_rejects_ambiguous_changes(self):
        original = b"[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n"
        cases = (
            ("vaapi.trace=true", "vaapi.trace=false"),
            ("vaapi.trace=true", "--remove", "vaapi.trace"),
            ("unknown.setting=true",),
            ("vaapi.trace=0",),
            ("vaapi.h264.ready_remap=true",),
            ("--preset", "private-invalid-selector"),
        )
        for changes in cases:
            with self.subTest(changes=changes):
                self.write_user(original.decode("utf-8"))
                result = self.run_cli("save", *changes, *self.writer_options())
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.user_file.read_bytes(), original)
                self.assertNotIn("Traceback", result.stderr)

    def test_user_write_commands_reject_read_only_flags_and_repeated_changes(self):
        self.write_user('[crystalhd]\nschema = 1\n[profile.desk]\nvaapi.debug = false\n')
        original = self.user_file.read_bytes()
        for command in ("save", "reset"):
            for flag in ("--no-user-config", "--ignore-environment"):
                with self.subTest(command=command, flag=flag):
                    arguments = ("vaapi.trace=true",) if command == "save" else ()
                    result = self.run_cli(command, *arguments, flag, *self.writer_options())
                    self.assertEqual(result.returncode, 2)
                    self.assertEqual(self.user_file.read_bytes(), original)
        cases = (
            ("--preset", "quality", "--preset", "quality"),
            ("--profile", "desk", "--profile", "desk"),
            ("--remove", "vaapi.trace", "--remove", "vaapi.trace"),
            ("--remove", "profile", "--remove", "profile"),
        )
        for changes in cases:
            with self.subTest(changes=changes):
                result = self.run_cli("save", *changes, *self.writer_options())
                self.assertEqual(result.returncode, 2)
                self.assertEqual(self.user_file.read_bytes(), original)

    def test_user_reset_is_an_empty_valid_overlay_even_after_bounded_malformed_input(self):
        for content in (b"[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = true\n",
                        b"not valid INI\n", b"\xff"):
            with self.subTest(content=content):
                self.user_file.write_bytes(content)
                os.chmod(str(self.user_file), 0o600)
                reset = self.run_cli("reset", *self.writer_options())
                self.assertEqual((reset.returncode, reset.stderr), (0, ""))
                self.assertIn("SAVED ONLY", reset.stdout)
                document = CONFIG.parse_document(self.user_file.read_bytes())
                self.assertEqual((document.metadata, document.settings, document.profiles),
                                 ({"schema": 1}, {}, {}))
                self.assertEqual(stat.S_IMODE(self.user_file.stat().st_mode), 0o600)
        fresh = self.run_cli("show-effective", *self.writer_options(),
                             "--ignore-environment", "--format", "json")
        self.assertEqual(fresh.returncode, 0)
        self.assertEqual(json.loads(fresh.stdout)["custom_overrides"], [])

    def test_user_serializer_roundtrip_preserves_values_and_rejects_invalid_objects(self):
        document = CONFIG.empty_document()
        document.settings = {
            "vaapi.trace": False, "vaapi.h264.ready_delay_us": 0,
            "chromium.password_store": "", "chromium.profile_dir": CONFIG.UNSET,
        }
        document.profiles = {"empty": {}, "desk": {"vaapi.debug": True}}
        encoded = CONFIG.serialize_document(document)
        self.assertIsInstance(encoded, bytes)
        parsed = CONFIG.parse_document(encoded)
        self.assertEqual(parsed.metadata, document.metadata)
        self.assertEqual(parsed.settings, document.settings)
        self.assertEqual(parsed.profiles, document.profiles)
        self.assertNotIn("vaapi.debug", parsed.settings)
        for field, value in (("unknown.setting", True), ("vaapi.trace", 0)):
            invalid = CONFIG.empty_document()
            invalid.settings[field] = value
            with self.subTest(field=field), self.assertRaises(CONFIG.SchemaError):
                CONFIG.serialize_document(invalid)
        invalid = CONFIG.empty_document()
        invalid.metadata["unknown_metadata"] = "must-not-be-dropped"
        with self.assertRaises(CONFIG.SchemaError):
            CONFIG.serialize_document(invalid)

    def test_user_serializer_escapes_unicode_line_and_vertical_separators(self):
        value = "line\u2028paragraph\u2029vertical\u000bform\u000cnext\u0085end"
        document = CONFIG.empty_document()
        document.settings["chromium.password_store"] = value
        encoded = CONFIG.serialize_document(document)
        encoded.decode("ascii")
        self.assertIn(b"\\u2028", encoded)
        self.assertIn(b"\\u2029", encoded)
        self.assertEqual(CONFIG.parse_document(encoded).settings["chromium.password_store"], value)
        saved = self.save_document(document)
        self.assertTrue(saved.committed and saved.durable)
        self.assertEqual(CONFIG.parse_document(self.user_file.read_bytes()).settings["chromium.password_store"], value)

    def test_user_serializer_rejects_huge_integer_as_schema_error(self):
        document = CONFIG.empty_document()
        document.settings["vaapi.h264.ready_delay_us"] = 10 ** 5000
        with self.assertRaises(CONFIG.SchemaError):
            CONFIG.serialize_document(document)

    def test_user_snapshot_identity_and_original_bytes_survive_document_mutation(self):
        original = b"[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n"
        self.write_user(original.decode("utf-8"))
        snapshot = self.user_snapshot()
        self.assertEqual(snapshot.original_bytes, original)
        self.assertIsInstance(snapshot.original_bytes, bytes)
        self.assertIsInstance(snapshot.directory_identity, tuple)
        self.assertIsInstance(snapshot.file_identity, tuple)
        identities = snapshot.directory_identity, snapshot.file_identity
        snapshot.document.settings["vaapi.trace"] = True
        self.assertEqual(snapshot.original_bytes, original)
        self.assertEqual((snapshot.directory_identity, snapshot.file_identity), identities)
        for field, replacement in (("original_bytes", b"forged"),
                                   ("directory_identity", ()), ("file_identity", ())):
            with self.subTest(field=field), self.assertRaises(AttributeError):
                setattr(snapshot, field, replacement)
        self.write_user(original.decode("utf-8") + "# another writer changed the file\n")
        current = self.user_file.read_bytes()
        with self.assertRaises(CONFIG.SchemaError):
            self.save_document(snapshot.document, snapshot)
        self.assertEqual(self.user_file.read_bytes(), current)

    def test_absent_user_parent_snapshot_rejects_a_later_writer_file(self):
        target = self.root / "not-yet-created" / "crystalhd" / "config.ini"
        snapshot = CONFIG.load_user_document_for_edit(str(target))
        self.assertIsNone(snapshot.directory_identity)
        self.assertIsNone(snapshot.file_identity)
        self.assertIsNone(snapshot.original_bytes)
        target.parent.mkdir(parents=True, mode=0o700)
        original = b"[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n"
        target.write_bytes(original)
        os.chmod(str(target), 0o600)
        snapshot.document.settings["vaapi.trace"] = True
        with self.assertRaises(CONFIG.SchemaError):
            CONFIG.save_user_document(str(target), snapshot.document, snapshot, system_path=None)
        self.assertEqual(target.read_bytes(), original)

    def test_user_save_rejects_stale_existing_absent_deleted_and_parent_snapshots(self):
        first = self.user_snapshot()
        stale_absent = self.user_snapshot()
        first.document.settings["vaapi.trace"] = True
        result = self.save_document(first.document, first)
        self.assertTrue(result.committed and result.durable)
        committed = self.user_file.read_bytes()
        with self.assertRaises(CONFIG.SchemaError):
            self.save_document(stale_absent.document, stale_absent)
        self.assertEqual(self.user_file.read_bytes(), committed)

        stale_existing = self.user_snapshot()
        current = self.user_snapshot()
        current.document.settings["vaapi.debug"] = True
        self.save_document(current.document, current)
        committed = self.user_file.read_bytes()
        with self.assertRaises(CONFIG.SchemaError):
            self.save_document(stale_existing.document, stale_existing)
        self.assertEqual(self.user_file.read_bytes(), committed)

        deleted = self.user_snapshot()
        self.user_file.unlink()
        with self.assertRaises(CONFIG.SchemaError):
            self.save_document(deleted.document, deleted)
        self.assertFalse(self.user_file.exists())

        parent_snapshot = self.user_snapshot()
        self.user_dir.rename(self.root / "previous-user-directory")
        self.user_dir.mkdir(mode=0o700)
        with self.assertRaises(CONFIG.SchemaError):
            self.save_document(parent_snapshot.document, parent_snapshot)
        self.assertFalse(self.user_file.exists())

    def test_user_save_rejects_in_place_edits_and_nonblocking_directory_contention(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        snapshot = self.user_snapshot()
        self.write_user("[crystalhd]\nschema = 1\n# externally changed\n")
        original = self.user_file.read_bytes()
        with self.assertRaises(CONFIG.SchemaError):
            self.save_document(snapshot.document, snapshot)
        self.assertEqual(self.user_file.read_bytes(), original)

        fd = os.open(str(self.user_dir), os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            busy = subprocess.run(
                [sys.executable, "-B", str(SCRIPT), "save", "vaapi.trace=true"] +
                list(self.writer_options()), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, check=False, timeout=5,
                env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(self.root)},
            )
            self.assertEqual(busy.returncode, 3)
            self.assertEqual(self.user_file.read_bytes(), original)
        finally:
            os.close(fd)

    def test_existing_user_snapshot_does_not_recreate_a_deleted_parent(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        snapshot = self.user_snapshot()
        snapshot.document.settings["vaapi.trace"] = True
        self.user_file.unlink()
        self.user_dir.rmdir()
        self.assertFalse(self.user_dir.exists())
        with self.assertRaises(CONFIG.SchemaError):
            self.save_document(snapshot.document, snapshot)
        self.assertFalse(self.user_dir.exists(), "a stale session must not recreate its removed parent")

    @unittest.skipIf(os.getuid() == 0, "writable caller-owned intermediates require an unprivileged user")
    def test_user_save_accepts_caller_owned_775_intermediate_with_private_existing_final(self):
        prefix = self.root / "writable-caller-prefix"
        prefix.mkdir(mode=0o775)
        os.chmod(str(prefix), 0o775)
        directory = prefix / "crystalhd"
        directory.mkdir(mode=0o700)
        target = directory / "config.ini"
        target.write_bytes(b"[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n")
        os.chmod(str(target), 0o600)
        snapshot = CONFIG.load_user_document_for_edit(str(target))
        snapshot.document.settings["vaapi.trace"] = True
        saved = CONFIG.save_user_document(str(target), snapshot.document, snapshot, system_path=None)
        self.assertTrue(saved.committed and saved.durable)
        self.assertIs(CONFIG.parse_document(target.read_bytes()).settings["vaapi.trace"], True)
        self.assertEqual(stat.S_IMODE(prefix.stat().st_mode), 0o775)
        self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)

    @unittest.skipIf(os.getuid() == 0, "writable caller-owned intermediates require an unprivileged user")
    def test_user_save_creates_private_final_beneath_caller_owned_775_intermediate(self):
        prefix = self.root / "writable-caller-prefix"
        prefix.mkdir(mode=0o775)
        os.chmod(str(prefix), 0o775)
        directory = prefix / "crystalhd"
        target = directory / "config.ini"
        snapshot = CONFIG.load_user_document_for_edit(str(target))
        self.assertIsNone(snapshot.directory_identity)
        snapshot.document.settings["vaapi.trace"] = True
        saved = CONFIG.save_user_document(str(target), snapshot.document, snapshot, system_path=None)
        self.assertTrue(saved.committed and saved.durable)
        self.assertIs(CONFIG.parse_document(target.read_bytes()).settings["vaapi.trace"], True)
        self.assertEqual(stat.S_IMODE(prefix.stat().st_mode), 0o775)
        self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)
        self.assertEqual((directory.stat().st_uid, target.stat().st_uid), (os.getuid(), os.getuid()))

    def test_user_save_still_rejects_a_775_final_directory(self):
        self.write_user("[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n")
        original = self.user_file.read_bytes()
        snapshot = self.user_snapshot()
        snapshot.document.settings["vaapi.trace"] = True
        os.chmod(str(self.user_dir), 0o775)
        try:
            with self.assertRaises(CONFIG.SchemaError):
                self.user_snapshot()
            with self.assertRaises(CONFIG.SchemaError):
                self.save_document(snapshot.document, snapshot)
            self.assertEqual(self.user_file.read_bytes(), original)
            self.assertEqual(stat.S_IMODE(self.user_dir.stat().st_mode), 0o775)
        finally:
            os.chmod(str(self.user_dir), 0o700)

    def test_user_editor_rejects_unsafe_paths_even_for_reset(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        parent_link = self.root / "linked-user-directory"
        parent_link.symlink_to(self.user_dir, target_is_directory=True)
        final_link = self.user_dir / "link.ini"
        final_link.symlink_to(self.user_file)
        fifo = self.user_dir / "fifo.ini"
        os.mkfifo(str(fifo), 0o600)
        directory = self.user_dir / "directory.ini"
        directory.mkdir(mode=0o700)
        real_open = os.open
        def guard_fifo_open(path, flags, *arguments, **keywords):
            if path in (str(fifo), fifo.name) and not flags & getattr(os, "O_PATH", 0):
                self.fail("FIFO must be rejected before invoking an ordinary open")
            return real_open(path, flags, *arguments, **keywords)
        with mock.patch.object(CONFIG.os, "open", side_effect=guard_fifo_open):
            for path in (parent_link / "config.ini", final_link, fifo, directory):
                for reset in (False, True):
                    with self.subTest(path=path.name, reset=reset), self.assertRaises(CONFIG.SchemaError):
                        CONFIG.load_user_document_for_edit(str(path), reset=reset)
        hardlink = self.user_dir / "hardlink.ini"
        os.link(str(self.user_file), str(hardlink))
        try:
            for reset in (False, True):
                with self.assertRaises(CONFIG.SchemaError):
                    self.user_snapshot(reset=reset)
        finally:
            hardlink.unlink()
        for path, mode in ((self.user_file, 0o622), (self.user_dir, 0o722)):
            original_mode = stat.S_IMODE(path.stat().st_mode)
            os.chmod(str(path), mode)
            try:
                for reset in (False, True):
                    with self.assertRaises(CONFIG.SchemaError):
                        self.user_snapshot(reset=reset)
            finally:
                os.chmod(str(path), original_mode)

    def test_user_editor_rejects_unsafe_owner_and_changed_process_ids(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        real_fstat = os.fstat
        def foreign_owner(fd):
            facts = real_fstat(fd)
            if stat.S_ISREG(facts.st_mode):
                fields = list(facts)
                fields[4] = os.getuid() + 1
                return os.stat_result(fields)
            return facts
        with mock.patch.object(CONFIG.os, "fstat", side_effect=foreign_owner):
            with self.assertRaises(CONFIG.SchemaError):
                self.user_snapshot()
        with mock.patch.object(CONFIG.os, "geteuid", return_value=os.getuid() + 1):
            with self.assertRaises(CONFIG.SchemaError):
                self.user_snapshot(reset=True)

    def test_user_writer_bounds_payload_and_does_not_target_the_system_layer(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        original = self.user_file.read_bytes()
        snapshot = self.user_snapshot()
        oversized = CONFIG.empty_document()
        oversized.settings["chromium.password_store"] = "x" * CONFIG.MAXIMUM_BYTES
        with self.assertRaises(CONFIG.SchemaError):
            self.save_document(oversized, snapshot)
        self.assertEqual(self.user_file.read_bytes(), original)
        with self.assertRaises(CONFIG.SchemaError):
            CONFIG.save_user_document(str(self.user_file), snapshot.document, snapshot,
                                      system_path=str(self.user_file))
        self.assertEqual(self.user_file.read_bytes(), original)
        self.user_file.write_bytes(b"#" * (CONFIG.MAXIMUM_BYTES + 1))
        for reset in (False, True):
            with self.assertRaises(CONFIG.SchemaError):
                self.user_snapshot(reset=reset)

    def test_user_writer_rejects_normalized_compiled_and_selected_system_targets_before_open(self):
        protected = CONFIG.SYSTEM_CONFIG
        alias = os.path.join(os.path.dirname(protected), ".", os.path.basename(protected))
        for path in (protected, alias):
            with self.subTest(path="compiled"), mock.patch.object(
                    CONFIG.os, "open", side_effect=AssertionError("protected target must be rejected before open")):
                with self.assertRaises(CONFIG.SchemaError):
                    CONFIG.load_user_document_for_edit(path)
        selected_directory = self.root / "selected-system"
        selected_directory.mkdir(mode=0o700)
        selected = selected_directory / "config.ini"
        selected.write_bytes(b"[crystalhd]\nschema = 1\n")
        os.chmod(str(selected), 0o600)
        snapshot = CONFIG.load_user_document_for_edit(str(selected))
        original = selected.read_bytes()
        normalized_alias = str(selected_directory / "subdir" / ".." / "config.ini")
        with mock.patch.object(CONFIG.os, "open", side_effect=AssertionError(
                "selected system target must be rejected before open")):
            with self.assertRaises(CONFIG.SchemaError):
                CONFIG.save_user_document(normalized_alias, snapshot.document, snapshot,
                                          system_path=str(selected))
        self.assertEqual(selected.read_bytes(), original)

    def test_user_writer_rejects_protected_directory_and_leaf_identity_aliases(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        original = self.user_file.read_bytes()
        snapshot = self.user_snapshot()
        snapshot.document.settings["vaapi.trace"] = True
        protected_directory = self.root / "protected-directory-alias"
        protected_directory.mkdir(mode=0o700)
        directory_fd = os.open(str(self.user_dir), os.O_RDONLY | os.O_DIRECTORY)
        real_open, real_stat, real_load = os.open, os.stat, CONFIG.load_document
        target_info = self.user_file.stat()
        try:
            for leaf in ("config.ini", "other-system.ini"):
                redirected = []
                def directory_alias(path, flags, *arguments, **keywords):
                    if path == str(protected_directory) and "dir_fd" not in keywords:
                        fd = os.dup(directory_fd)
                        redirected.append(fd)
                        return fd
                    return real_open(path, flags, *arguments, **keywords)
                def leaf_alias(path, *arguments, **keywords):
                    if path == "other-system.ini" and keywords.get("dir_fd") in redirected:
                        return target_info
                    return real_stat(path, *arguments, **keywords)
                def empty_system(path, role, system=False):
                    return CONFIG.empty_document() if system else real_load(path, role, system=False)
                with self.subTest(alias=leaf), mock.patch.object(
                        CONFIG, "SYSTEM_CONFIG", str(self.root / "missing-compiled" / "config.ini")):
                    with mock.patch.object(CONFIG, "load_document", side_effect=empty_system):
                        with mock.patch.object(CONFIG.os, "open", side_effect=directory_alias):
                            with mock.patch.object(CONFIG.os, "stat", side_effect=leaf_alias):
                                with self.assertRaises(CONFIG.SchemaError):
                                    CONFIG.save_user_document(
                                        str(self.user_file), snapshot.document, snapshot,
                                        system_path=str(protected_directory / leaf),
                                    )
                self.assertTrue(redirected, "the pinned protected-directory guard must be reached")
                self.assertEqual(self.user_file.read_bytes(), original)
        finally:
            os.close(directory_fd)

    def test_protected_leaf_access_denial_allows_unprivileged_save_but_root_guard_fails_closed(self):
        if os.getuid() == 0:
            self.skipTest("ordinary-user protected-leaf path requires a non-root test caller")
        self.write_user("[crystalhd]\nschema = 1\n")
        protected_directory = self.root / "private-protected-directory"
        protected_directory.mkdir(mode=0o700)
        protected_fd = os.open(str(protected_directory), os.O_RDONLY | os.O_DIRECTORY)
        user_directory_fd = os.open(str(self.user_dir), os.O_RDONLY | os.O_DIRECTORY)
        protected_parent, protected_leaf = os.path.split(CONFIG.SYSTEM_CONFIG)
        real_open, real_stat = os.open, os.stat
        opened = []
        def protected_open(path, flags, *arguments, **keywords):
            if path == protected_parent and "dir_fd" not in keywords:
                if not flags & os.O_PATH:
                    raise OSError(errno.EACCES, "protected directory is not readable")
                fd = os.dup(protected_fd)
                opened.append(fd)
                return fd
            return real_open(path, flags, *arguments, **keywords)
        def protected_stat(path, *arguments, **keywords):
            if path == protected_leaf and keywords.get("dir_fd") in opened:
                raise OSError(errno.EACCES, "protected leaf cannot be searched")
            return real_stat(path, *arguments, **keywords)
        try:
            with mock.patch.object(CONFIG.os, "open", side_effect=protected_open):
                with mock.patch.object(CONFIG.os, "stat", side_effect=protected_stat):
                    with mock.patch("sys.stdout"), mock.patch("sys.stderr"):
                        result = CONFIG.main(
                            ["save", "vaapi.trace=true"] + list(self.writer_options()),
                            environment={"HOME": str(self.root)},
                        )
                    self.assertEqual(result, 0)
                    self.assertTrue(opened, "protected metadata must be inspected through O_PATH")
                    with mock.patch.object(CONFIG.os, "getuid", return_value=0):
                        with self.assertRaises(OSError) as rejected:
                            CONFIG._reject_system_target(user_directory_fd, "absent-root-check.ini", None)
                    self.assertEqual(rejected.exception.errno, errno.EACCES)
            self.assertIs(CONFIG.parse_document(self.user_file.read_bytes()).settings["vaapi.trace"], True)
            self.assertFalse((self.user_dir / "absent-root-check.ini").exists())
        finally:
            os.close(user_directory_fd)
            os.close(protected_fd)

    def test_user_writer_precommit_failures_preserve_bytes_and_remove_candidates(self):
        original = "[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n"
        real_write, real_fsync = os.write, os.fsync
        for failure in ("write", "file-fsync", "replace"):
            with self.subTest(failure=failure):
                self.write_user(original)
                snapshot = self.user_snapshot()
                snapshot.document.settings["vaapi.trace"] = True
                entries = set(self.user_dir.iterdir())
                calls = [0]
                def partial_write_then_fail(fd, data):
                    calls[0] += 1
                    if calls[0] == 1:
                        return real_write(fd, data[:3])
                    raise OSError(errno.EIO, "injected write failure")
                def fail_file_fsync(fd):
                    if stat.S_ISREG(os.fstat(fd).st_mode):
                        raise OSError(errno.EIO, "injected file fsync failure")
                    return real_fsync(fd)
                target = {"write": "write", "file-fsync": "fsync", "replace": "replace"}[failure]
                effect = {"write": partial_write_then_fail, "file-fsync": fail_file_fsync,
                          "replace": OSError(errno.EIO, "injected replace failure")}[failure]
                with mock.patch.object(CONFIG.os, target, side_effect=effect):
                    with self.assertRaises(CONFIG.SchemaError):
                        self.save_document(snapshot.document, snapshot)
                self.assertEqual(self.user_file.read_bytes(), original.encode("utf-8"))
                self.assertEqual(set(self.user_dir.iterdir()), entries)

    def test_user_writer_lock_errors_fail_before_creating_a_candidate(self):
        self.write_user("[crystalhd]\nschema = 1\n")
        snapshot = self.user_snapshot()
        snapshot.document.settings["vaapi.trace"] = True
        original = self.user_file.read_bytes()
        entries = set(self.user_dir.iterdir())
        for error in (errno.ENOLCK, errno.EOPNOTSUPP):
            with self.subTest(errno=error), mock.patch.object(
                    CONFIG.fcntl, "flock", side_effect=OSError(error, "injected lock failure")):
                with self.assertRaises(CONFIG.SchemaError):
                    self.save_document(snapshot.document, snapshot)
            self.assertEqual(self.user_file.read_bytes(), original)
            self.assertEqual(set(self.user_dir.iterdir()), entries)

    def test_user_writer_recognizes_replace_that_committed_before_raising(self):
        self.write_user("[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n")
        snapshot = self.user_snapshot()
        snapshot.document.settings["vaapi.trace"] = True
        real_replace = os.replace
        calls = []
        def commit_then_raise(*arguments, **keywords):
            real_replace(*arguments, **keywords)
            calls.append(True)
            raise OSError(errno.EIO, "injected exception after successful replacement")
        with mock.patch.object(CONFIG.os, "replace", side_effect=commit_then_raise):
            result = self.save_document(snapshot.document, snapshot)
        self.assertEqual(calls, [True])
        self.assertIs(result.committed, True)
        self.assertFalse(result.durable)
        self.assertIs(CONFIG.parse_document(self.user_file.read_bytes()).settings["vaapi.trace"], True)
        self.assertEqual(set(path.name for path in self.user_dir.iterdir()), {"config.ini"})

    def test_user_writer_preserves_foreign_replacement_of_its_candidate_name(self):
        self.write_user("[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n")
        original = self.user_file.read_bytes()
        snapshot = self.user_snapshot()
        snapshot.document.settings["vaapi.trace"] = True
        real_stat, real_open = os.stat, os.open
        foreign_bytes = b"foreign candidate entry must survive cleanup\n"
        foreign_names, retained_fds = [], []
        def replace_candidate_before_stat(path, *arguments, **keywords):
            if (isinstance(path, str) and path.startswith(".crystalhd-config-") and
                    "dir_fd" in keywords and not foreign_names):
                directory_fd = keywords["dir_fd"]
                # Keep the old inode alive so this fixture cannot reuse its
                # number for the distinct foreign entry created immediately.
                retained_fds.append(real_open(path, os.O_RDONLY | os.O_CLOEXEC,
                                               dir_fd=directory_fd))
                os.unlink(path, dir_fd=directory_fd)
                foreign_fd = real_open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                                       0o600, dir_fd=directory_fd)
                with os.fdopen(foreign_fd, "wb") as stream:
                    stream.write(foreign_bytes)
                foreign_names.append(path)
            return real_stat(path, *arguments, **keywords)
        try:
            with mock.patch.object(CONFIG.os, "stat", side_effect=replace_candidate_before_stat):
                with self.assertRaises(CONFIG.SchemaError):
                    self.save_document(snapshot.document, snapshot)
            self.assertEqual(len(foreign_names), 1)
            self.assertEqual(self.user_file.read_bytes(), original)
            self.assertEqual((self.user_dir / foreign_names[0]).read_bytes(), foreign_bytes)
            self.assertEqual(set(path.name for path in self.user_dir.iterdir()),
                             {"config.ini", foreign_names[0]})
        finally:
            for fd in retained_fds:
                os.close(fd)

    def test_user_writer_postreplace_fsync_failure_reports_committed_without_rollback(self):
        self.write_user("[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = false\n")
        snapshot = self.user_snapshot()
        snapshot.document.settings["vaapi.trace"] = True
        real_fsync = os.fsync
        def fail_directory_fsync(fd):
            if stat.S_ISDIR(os.fstat(fd).st_mode):
                raise OSError(errno.EIO, "injected directory fsync failure")
            return real_fsync(fd)
        with mock.patch.object(CONFIG.os, "fsync", side_effect=fail_directory_fsync):
            result = self.save_document(snapshot.document, snapshot)
        self.assertTrue(result.committed)
        self.assertFalse(result.durable)
        self.assertIs(CONFIG.parse_document(self.user_file.read_bytes()).settings["vaapi.trace"], True)
        self.assertEqual(set(path.name for path in self.user_dir.iterdir()), {"config.ini"})
        self.assertEqual(stat.S_IMODE(self.user_file.stat().st_mode), 0o600)
        with mock.patch.object(CONFIG.os, "fsync", side_effect=fail_directory_fsync):
            with mock.patch("sys.stdout"), mock.patch("sys.stderr"):
                status = CONFIG.main(
                    ["save", "vaapi.trace=false"] + list(self.writer_options()),
                    environment={"HOME": str(self.root)},
                )
        self.assertEqual(status, 4)
        self.assertIs(CONFIG.parse_document(self.user_file.read_bytes()).settings["vaapi.trace"], False)

    def test_run_no_config_and_suppression_preserve_complete_native_values(self):
        environment = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "HOME": str(self.root / "unused-home"), "LC_ALL": "C",
            "UNKNOWN_PRIVATE_VALUE": "non-UTF8-\udc80-and-empty-next",
            "UNKNOWN_EMPTY": "", "CRYSTALHD_VAAPI_DEBUG": "0",
            "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT": "not-a-number",
            "CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE": "1",
        }
        command = self.environment_child()
        for centered in (None, "0", "invalid-native-request"):
            with self.subTest(centered=centered):
                inherited = dict(environment)
                if centered is not None:
                    inherited[CONFIG.CENTERED_NV12_ENVIRONMENT] = centered
                direct = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, env=inherited, check=False)
                managed = subprocess.run(
                    [sys.executable, "-B", str(SCRIPT), "run", *self.common(), "--", *command],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    env=inherited, check=False,
                )
                self.assertEqual((direct.returncode, managed.returncode), (0, 0))
                expected = dict(json.loads(direct.stdout))
                expected[CONFIG.NATIVE_CONFIG_DISABLE_ENVIRONMENT] = "1"
                self.assertEqual(dict(json.loads(managed.stdout)), expected)
                plan = self.launch_plan(environment=inherited)
                self.assertEqual(plan.environment, inherited)
                self.assertIsNot(plan.environment, inherited)
                self.assertEqual(plan.requested_keys, ())
        self.assertFalse((self.root / "unused-home").exists())

    def test_run_only_emits_winning_managed_keys_and_preserves_raw_native_values(self):
        system = CONFIG.empty_document()
        system.settings = {"vaapi.debug": True, "vaapi.h264.mode": "live"}
        user = CONFIG.empty_document()
        user.settings = {"vaapi.debug": False, "libcrystalhd.rx_pool_count": 8}
        user.profiles = {"desk": {"vaapi.trace": True, "vaapi.h264.low_latency": False}}
        inherited = {
            "CRYSTALHD_VAAPI_LIVE_H264": "native-strict-value",
            "CRYSTALHD_VAAPI_TRACE": "0",
            "CRYSTALHD_VAAPI_LOW_LATENCY_H264": "0",
            "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT": "invalid-native-fallback",
            "CRYSTALHD_PLAYER_PYTHON": "", "UNRELATED": "unchanged",
        }
        plan = self.launch_plan(user, inherited, {"vaapi.debug": True}, profile="desk", system=system)
        expected = dict(inherited, CRYSTALHD_VAAPI_DEBUG="1", CRYSTALHD_CONFIG_DISABLE="1")
        self.assertEqual(plan.environment, expected)
        self.assertEqual(inherited.get("CRYSTALHD_VAAPI_DEBUG"), None)
        self.assertEqual(plan.requested_keys, ("vaapi.debug",))

    def test_run_canonical_false_and_unset_remove_only_the_explicit_override_variable(self):
        inherited = {"CRYSTALHD_VAAPI_LOW_LATENCY_H264": "0",
                     "CRYSTALHD_VAAPI_DEBUG": "0", "UNRELATED": "keep"}
        user = CONFIG.empty_document()
        user.settings = {"vaapi.h264.low_latency": False, "vaapi.debug": False}
        self.assertEqual(self.launch_plan(user, inherited).environment,
                         dict(inherited, CRYSTALHD_CONFIG_DISABLE="1"))
        for value in (False, CONFIG.UNSET):
            with self.subTest(value=value):
                plan = self.launch_plan(user, inherited, {"vaapi.h264.low_latency": value})
                self.assertNotIn("CRYSTALHD_VAAPI_LOW_LATENCY_H264", plan.environment)
                self.assertEqual(plan.environment["CRYSTALHD_VAAPI_DEBUG"], "0")
                self.assertEqual(plan.environment["UNRELATED"], "keep")
        off = self.launch_plan(user, inherited, {"vaapi.debug": False})
        self.assertNotIn("CRYSTALHD_VAAPI_DEBUG", off.environment)
        self.assertEqual(off.environment["CRYSTALHD_VAAPI_LOW_LATENCY_H264"], "0")

    def test_run_adapts_bounded_chromium_settings_and_bypasses_legacy_shell_config(self):
        user = CONFIG.empty_document()
        user.settings = {
            "chromium.executable": "/private/browser",
            "chromium.drm_device": "/dev/dri/private-render",
            "chromium.password_store": "",
            "chromium.profile_dir": "/private/profile",
            "chromium.native_gl": False,
        }
        plan = self.launch_plan(user)
        self.assertEqual(plan.environment, {
            "CRYSTALHD_CONFIG_DISABLE": "1",
            "CRYSTALHD_CHROMIUM_CONFIG_DISABLE": "1",
            "CRYSTALHD_CHROMIUM": "/private/browser",
            "CRYSTALHD_DRM_DEVICE": "/dev/dri/private-render",
            "CRYSTALHD_CHROMIUM_PASSWORD_STORE": "",
            "CRYSTALHD_CHROMIUM_PROFILE_DIR": "/private/profile",
        })
        self.assertEqual(plan.requested_keys, tuple(sorted(user.settings)))
        self.assertTrue(plan.chromium_config_bypassed)

    def test_run_chromium_environment_wins_but_strict_layers_still_bypass_legacy_file(self):
        user = CONFIG.empty_document()
        user.settings["chromium.executable"] = "/saved/browser"
        inherited = {
            "CRYSTALHD_CHROMIUM": "/environment/browser",
            "CRYSTALHD_CHROMIUM_CONFIG_DISABLE": "0",
            "UNRELATED": "preserved",
        }
        plan = self.launch_plan(user, inherited)
        self.assertEqual(plan.environment, {
            "CRYSTALHD_CHROMIUM": "/environment/browser",
            "CRYSTALHD_CHROMIUM_CONFIG_DISABLE": "1",
            "CRYSTALHD_CONFIG_DISABLE": "1",
            "UNRELATED": "preserved",
        })
        self.assertEqual(plan.requested_keys, ())
        self.assertTrue(plan.chromium_config_bypassed)

    def test_run_chromium_false_unset_and_empty_override_only_the_child(self):
        inherited = {
            "CRYSTALHD_CHROMIUM_NATIVE_GL": "1",
            "CRYSTALHD_CHROMIUM_PROFILE_DIR": "/environment/profile",
            "CRYSTALHD_CHROMIUM_PASSWORD_STORE": "basic",
            "UNRELATED": "preserved",
        }
        plan = self.launch_plan(environment=inherited, overrides={
            "chromium.native_gl": False,
            "chromium.profile_dir": CONFIG.UNSET,
            "chromium.password_store": "",
        })
        self.assertNotIn("CRYSTALHD_CHROMIUM_NATIVE_GL", plan.environment)
        self.assertNotIn("CRYSTALHD_CHROMIUM_PROFILE_DIR", plan.environment)
        self.assertEqual(plan.environment["CRYSTALHD_CHROMIUM_PASSWORD_STORE"], "")
        self.assertEqual(plan.environment["CRYSTALHD_CHROMIUM_CONFIG_DISABLE"], "1")
        self.assertEqual(plan.environment["CRYSTALHD_CONFIG_DISABLE"], "1")
        self.assertEqual(plan.environment["UNRELATED"], "preserved")

    def test_run_supported_modes_native_resets_and_safe_disabling_requests(self):
        inherited = {"CRYSTALHD_VAAPI_LIVE_H264": "1", "CRYSTALHD_VAAPI_LOW_LATENCY_H264": "1",
                     "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP": "1",
                     "CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC": "1",
                     "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT": "8",
                     "CRYSTALHD_PLAYER_PYTHON": "/private/old-python"}
        plan = self.launch_plan(environment=inherited, overrides={
            "vaapi.h264.mode": "bounded", "vaapi.h264.ready_remap": False,
            "vaapi.i915_wc": CONFIG.UNSET, "libcrystalhd.rx_pool_count": CONFIG.UNSET,
            "player.python": "/private/new-python",
        })
        self.assertEqual(plan.environment, {
            "CRYSTALHD_CONFIG_DISABLE": "1",
            "CRYSTALHD_VAAPI_LOW_LATENCY_H264": "1", "CRYSTALHD_PLAYER_PYTHON": "/private/new-python",
        })
        strict = self.launch_plan(overrides={"vaapi.h264.mode": "strict", "libcrystalhd.rx_pool_count": 16,
                                             "vaapi.trace": True})
        self.assertEqual(strict.environment, {"CRYSTALHD_CONFIG_DISABLE": "1",
                                               "CRYSTALHD_VAAPI_LIVE_H264": "0",
                                               "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT": "16",
                                               "CRYSTALHD_VAAPI_TRACE": "1"})

    def test_run_requires_exact_union_of_existing_preset_and_emitted_risk_tokens(self):
        with self.assertRaises(CONFIG.SchemaError):
            self.launch_plan(preset=[])
        user = CONFIG.empty_document()
        user.settings = {"libcrystalhd.rx_pool_count": 8, "vaapi.h264.mode": "live",
                         "vaapi.h264.low_latency": True}
        low, streaming = "low-memory-8-buffer", "bcm70015-live-h264-drain"
        for acknowledgements in ((), (low,), (streaming,)):
            with self.subTest(acknowledgements=acknowledgements), self.assertRaises(CONFIG.SchemaError):
                self.launch_plan(user, acknowledgements=acknowledgements)
        plan = self.launch_plan(user, acknowledgements=(low, streaming))
        self.assertEqual(plan.environment, {"CRYSTALHD_CONFIG_DISABLE": "1",
                                            "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT": "8",
                                            "CRYSTALHD_VAAPI_LIVE_H264": "1",
                                            "CRYSTALHD_VAAPI_LOW_LATENCY_H264": "1"})
        for tokens in ((low, low, streaming), (low, streaming, "invented-risk")):
            with self.subTest(tokens=tokens), self.assertRaises(CONFIG.CommandLineSelectionError):
                self.launch_plan(user, acknowledgements=tokens)
        with self.assertRaises(CONFIG.SchemaError):
            self.launch_plan(preset="streaming", environment={
                "CRYSTALHD_VAAPI_LIVE_H264": "0", "CRYSTALHD_VAAPI_LOW_LATENCY_H264": "0"})
        # The existing inspection/save policy does not accept a run-only union
        # for a Default selection which has no preset acknowledgement.
        inspection = self.run_cli("validate", *self.common(), "--acknowledge-risk", low)
        self.assertEqual(inspection.returncode, 3)
        self.assertIn("does not apply", inspection.stderr)
        storage = self.run_cli("save", "vaapi.debug=true", *self.writer_options(),
                               "--acknowledge-risk", low)
        self.assertEqual(storage.returncode, 3)
        self.assertFalse(self.user_file.exists())

    def test_run_blocks_indirect_ready_activation_but_preserves_unchanged_inherited_state(self):
        inherited = {"CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP": "1",
                     "CRYSTALHD_VAAPI_LIVE_H264": "0", "CRYSTALHD_VAAPI_LOW_LATENCY_H264": "0"}
        token = ("bcm70015-live-h264-drain",)
        with self.assertRaises(CONFIG.SchemaError):
            self.launch_plan(environment=inherited, overrides={"vaapi.h264.mode": "live",
                             "vaapi.h264.low_latency": True}, acknowledgements=token)
        with self.assertRaises(CONFIG.SchemaError):
            self.launch_plan(environment={"CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP": "1"},
                             preset="streaming", acknowledgements=token)
        self.assertEqual(self.launch_plan(environment=inherited).environment, inherited)
        active = dict(inherited, CRYSTALHD_VAAPI_LIVE_H264="1", CRYSTALHD_VAAPI_LOW_LATENCY_H264="1")
        plan = self.launch_plan(environment=active, overrides={"vaapi.debug": True})
        self.assertEqual(plan.environment, dict(active, CRYSTALHD_VAAPI_DEBUG="1",
                                                     CRYSTALHD_CONFIG_DISABLE="1"))
        invalid = dict(active, CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY="invalid-native-policy")
        self.assertEqual(self.launch_plan(environment=invalid).environment, invalid)
        with self.assertRaises(CONFIG.SchemaError):
            self.launch_plan(environment=invalid, overrides={"vaapi.h264.ready_policy": CONFIG.UNSET})

    def test_run_rejects_unsupported_selected_requests_but_not_inactive_profiles(self):
        blocked = (
            {"vaapi.h264.ready_remap": True, "vaapi.h264.mode": "live", "vaapi.h264.low_latency": True},
            {"vaapi.i915_wc": True}, {"vaapi.h264.ready_policy": "previous"},
            {"vaapi.h264.ready_policy": CONFIG.UNSET}, {"vaapi.h264.ready_delay_us": 0},
            {"vaapi.h264.ready_delay_us": CONFIG.UNSET},
            {"chromium.native_gl": True},
            {"chromium.experimental_hw_decode": True, "chromium.disable_gpu_sandbox": True},
            {"chromium.disable_gpu_sandbox": True}, {"chromium.h264_policy_ack": True},
            {"gstreamer.crystalhddec.scale_width": 0}, {"player.decoder": "software"},
            {"gstreamer.crystalhddec.scale_width": 1280, "player.software_output_size": "640x360"},
        )
        for settings in blocked:
            user = CONFIG.empty_document()
            user.settings.update(settings)
            with self.subTest(keys=tuple(settings)), self.assertRaisesRegex(
                    CONFIG.SchemaError, "does not support|no supported environment adapter"):
                self.launch_plan(user)
        with self.assertRaisesRegex(CONFIG.SchemaError, "does not support|no supported environment adapter"):
            self.launch_plan(preset="quality")
        user = CONFIG.empty_document()
        user.settings["vaapi.debug"] = True
        user.profiles["inactive"] = {"chromium.disable_gpu_sandbox": True,
                                     "vaapi.h264.ready_policy": CONFIG.UNSET}
        self.assertEqual(self.launch_plan(user).environment, {"CRYSTALHD_VAAPI_DEBUG": "1",
                                                           "CRYSTALHD_CONFIG_DISABLE": "1"})

    def test_run_rejects_process_id_mismatch_before_preparing_environment(self):
        with mock.patch.object(CONFIG.os, "geteuid", return_value=os.getuid() + 1):
            with self.assertRaises(CONFIG.SchemaError):
                self.launch_plan()
        with mock.patch.object(CONFIG.os, "getegid", return_value=os.getgid() + 1):
            with self.assertRaises(CONFIG.SchemaError):
                self.launch_plan()

    def test_run_cli_applies_only_explicit_override_and_never_persists_it(self):
        self.write_user("[crystalhd]\nschema = 1\n[settings]\nvaapi.debug = true\nvaapi.trace = true\n")
        original = self.user_file.read_bytes()
        result = self.run_cli("run", *self.writer_options(), "--override", "vaapi.debug=false",
                              "--", *self.environment_child(),
                              environment={"CRYSTALHD_VAAPI_DEBUG": "0", "CRYSTALHD_VAAPI_TRACE": "0",
                                           CONFIG.CENTERED_NV12_ENVIRONMENT: "invalid-native-request"})
        self.assertEqual(result.returncode, 0, result.stderr)
        child = dict((key.encode("latin1"), value.encode("latin1"))
                     for key, value in json.loads(result.stdout))
        self.assertNotIn(b"CRYSTALHD_VAAPI_DEBUG", child)
        self.assertEqual(child[b"CRYSTALHD_VAAPI_TRACE"], b"0")
        self.assertEqual(child[CONFIG.CENTERED_NV12_ENVIRONMENT.encode("ascii")],
                         b"invalid-native-request")
        self.assertIn("REQUESTED", result.stderr)
        self.assertNotIn("ACTIVE", result.stderr)
        self.assertEqual(self.user_file.read_bytes(), original)
        self.assertEqual(set(path.name for path in self.user_dir.iterdir()), {"config.ini"})

    def test_run_cli_launches_chromium_with_strict_values_not_legacy_shell_values(self):
        browser = self.root / "mock-browser"
        receipt = self.root / "browser-receipt"
        profile = self.root / "managed-profile"
        legacy = self.root / "legacy-chromium.conf"
        browser.write_text(
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = --version ]; then echo 'Chromium 999'; exit 0; fi\n"
            "{ printf '%s\\n' \"${CRYSTALHD_CHROMIUM_PASSWORD_STORE-}\" "
            "\"${CRYSTALHD_CHROMIUM_CONFIG_DISABLE-}\" "
            "\"${LIBGL_ALWAYS_SOFTWARE-}\" \"${GALLIUM_DRIVER-}\"; "
            "printf 'arg=%s\\n' \"$@\"; } > \"$BROWSER_RECEIPT\"\n",
            encoding="utf-8",
        )
        browser.chmod(0o700)
        legacy.write_text("CRYSTALHD_CHROMIUM_PASSWORD_STORE=legacy-store\n", encoding="utf-8")
        legacy.chmod(0o600)
        self.write_user(
            "[crystalhd]\nschema = 1\n[settings]\n"
            "chromium.executable = \"{0}\"\nchromium.password_store = \"managed-store\"\n"
            "chromium.profile_dir = \"{1}\"\nchromium.native_gl = false\n".format(browser, profile)
        )
        original = self.user_file.read_bytes()
        result = self.run_cli(
            "run", *self.writer_options(), "--", str(ROOT / "scripts/crystalhd-chromium"),
            "https://example.invalid/video",
            environment={"CRYSTALHD_CHROMIUM_CONFIG": str(legacy),
                         "BROWSER_RECEIPT": str(receipt)},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = receipt.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[:4], ["managed-store", "1", "1", "llvmpipe"])
        self.assertIn("arg=--password-store=managed-store", lines)
        self.assertIn("arg=--user-data-dir=" + str(profile), lines)
        self.assertNotIn("arg=--password-store=legacy-store", lines)
        self.assertIn("legacy Chromium shell config bypassed", result.stderr)
        self.assertEqual(self.user_file.read_bytes(), original)

    def test_run_cli_rejects_invalid_and_unsupported_requests_before_exec(self):
        marker = self.root / "child-was-executed"
        command = ["/bin/sh", "-c", 'printf executed > "$1"', "unused", str(marker)]
        cases = (
            ("--override", "vaapi.debug=true", "--override", "vaapi.debug=false"),
            ("--override", "unknown.key=true"), ("--override", "vaapi.debug=0"),
            ("--override", "vaapi.i915_wc=true"), ("--override", "vaapi.h264.ready_delay_us=0"),
            ("--override", "chromium.native_gl=true"),
            ("--preset", "quality"), ("--ignore-environment",),
            ("--override", "libcrystalhd.rx_pool_count=8"),
        )
        for options in cases:
            with self.subTest(options=options):
                result = self.run_cli("run", *self.common(), *options, "--", *command)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(marker.exists())
                self.assertNotIn("Traceback", result.stderr)
        for arguments in (("run", *self.common()), ("run", *self.common(), "--"),
                          ("run", *self.common(), *command)):
            with self.subTest(arguments=arguments):
                result = self.run_cli(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(marker.exists())

    def test_run_cli_uses_the_existing_strict_safe_reader_before_exec(self):
        marker = self.root / "unsafe-config-child"
        command = ["/bin/sh", "-c", 'printf executed > "$1"', "unused", str(marker)]
        self.write_user("not valid INI\n")
        original = self.user_file.read_bytes()
        malformed = self.run_cli("run", *self.writer_options(), "--", *command)
        self.assertEqual(malformed.returncode, 3)
        self.assertFalse(marker.exists())
        self.assertEqual(self.user_file.read_bytes(), original)
        self.write_user("[crystalhd]\nschema = 1\n[settings]\nvaapi.debug = true\n")
        alias = self.user_dir / "unsafe-link.ini"
        alias.symlink_to(self.user_file)
        unsafe = self.run_cli("run", "--no-system-config", "--user-config", str(alias),
                              "--", *command)
        self.assertEqual(unsafe.returncode, 3)
        self.assertFalse(marker.exists())
        self.assertNotIn("Traceback", unsafe.stderr)

    def test_run_exec_preserves_native_child_pid_cwd_literal_arguments_and_exit_status(self):
        literal = "$(touch this-must-not-be-executed); * --override"
        command = [sys.executable, "-B", str(SCRIPT), "run", *self.common(), "--", "/bin/sh", "-c",
                   'printf "%s\\n" "$$" "$PWD" "$1" "$2"; exit 17', "unused", literal, "--preset"]
        process = subprocess.Popen(command, cwd=str(self.root), stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True,
                                   env={"PATH": "/usr/bin:/bin", "HOME": str(self.root), "LC_ALL": "C"})
        output, error = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 17, error)
        self.assertEqual(output.splitlines(), [str(process.pid), str(self.root), literal, "--preset"])
        self.assertFalse((self.root / "this-must-not-be-executed").exists())

    def test_run_restores_ignored_sigpipe_for_a_non_python_child(self):
        yes = shutil.which("yes")
        self.assertIsNotNone(yes, "native yes executable is required for the signal regression")
        original = signal.getsignal(signal.SIGPIPE)
        try:
            signal.signal(signal.SIGPIPE, signal.SIG_IGN)
            # Prove this actual native executable distinguishes inherited
            # ignore from default, rather than resetting SIGPIPE itself.
            ignored = subprocess.Popen([yes], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       restore_signals=False)
            ignored.stdout.close()
            ignored.stdout = None
            ignored.communicate(timeout=5)
            self.assertNotEqual(ignored.returncode, -signal.SIGPIPE)
            process = subprocess.Popen(
                [sys.executable, "-B", str(SCRIPT), "run", *self.common(), "--", yes],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, restore_signals=False,
                env={"PATH": "/usr/bin:/bin", "HOME": str(self.root), "LC_ALL": "C"},
            )
            process.stdout.close()
            process.stdout = None
            _, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, -signal.SIGPIPE, error.decode(errors="replace"))
        finally:
            signal.signal(signal.SIGPIPE, original)

    def test_run_diagnostic_failures_preserve_native_exec_and_missing_exec_status(self):
        environment = {"PATH": "/usr/bin:/bin", "HOME": str(self.root), "LC_ALL": "C"}
        commands = ((["/bin/echo", "child-ok"], 0, "child-ok\n"),
                    ([str(self.root / "missing-child")], 127, ""))
        for sink in ("closed", "full", "broken-pipe"):
            for child, expected_status, expected_output in commands:
                command = [sys.executable, "-B", str(SCRIPT), "run", *self.common(),
                           "--override", "vaapi.debug=true", "--", *child]
                with self.subTest(stderr=sink, child=child[0]):
                    if sink == "closed":
                        result = subprocess.run(
                            ["/bin/sh", "-c", 'exec 2>&-; exec "$@"', "unused", *command],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            env=environment, timeout=5, check=False,
                        )
                        self.assertEqual(result.stderr, "")
                    elif sink == "full":
                        with open("/dev/full", "wb") as error_stream:
                            result = subprocess.run(
                                command, stdout=subprocess.PIPE, stderr=error_stream,
                                text=True, env=environment, timeout=5, check=False,
                            )
                    else:
                        read_fd, write_fd = os.pipe()
                        os.close(read_fd)
                        try:
                            result = subprocess.run(
                                command, stdout=subprocess.PIPE, stderr=write_fd,
                                text=True, env=environment, timeout=5, check=False,
                            )
                        finally:
                            os.close(write_fd)
                    self.assertEqual(result.returncode, expected_status)
                    self.assertEqual(result.stdout, expected_output)

    def test_run_exec_failure_is_redacted_and_restores_signal_dispositions(self):
        self.write_user("[crystalhd]\nschema = 1\n[settings]\nvaapi.debug = true\n")
        signals = [getattr(signal, name) for name in ("SIGPIPE", "SIGXFZ", "SIGXFSZ")
                   if hasattr(signal, name)]
        original = dict((number, signal.getsignal(number)) for number in signals)
        class Notice(io.StringIO):
            flushed = False
            def flush(self):
                self.flushed = True
                super().flush()
        try:
            for error_number, expected_status in ((errno.ENOENT, 127), (errno.EACCES, 126)):
                for number in signals:
                    signal.signal(number, signal.SIG_IGN)
                output, notice = io.StringIO(), Notice()
                observations = []
                def fail_exec(executable, argv, environment):
                    observations.append((executable, argv, dict(environment)))
                    self.assertTrue(notice.flushed)
                    self.assertIn("REQUESTED", notice.getvalue())
                    self.assertTrue(all(signal.getsignal(number) == signal.SIG_DFL for number in signals))
                    raise OSError(error_number, "/private/failure-detail")
                with mock.patch.object(CONFIG.os, "execvpe", side_effect=fail_exec):
                    with mock.patch("sys.stdout", output), mock.patch("sys.stderr", notice):
                        status = CONFIG.main(["run", *self.writer_options(), "--", "/private/child-executable"],
                                             environment={"PATH": "/usr/bin:/bin", "HOME": str(self.root)})
                self.assertEqual(status, expected_status)
                self.assertEqual(len(observations), 1)
                self.assertEqual(output.getvalue(), "")
                self.assertNotIn("/private/", notice.getvalue())
                self.assertNotIn("Traceback", notice.getvalue())
                self.assertTrue(all(signal.getsignal(number) == signal.SIG_IGN for number in signals))
        finally:
            for number, handler in original.items():
                signal.signal(number, handler)

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
            "argparse", "errno", "fcntl", "json", "os", "re", "secrets", "signal", "stat", "sys",
            "shlex", "shutil", "struct", "subprocess",
        })

        archive_dir = self.root / "archive-without-git"
        scripts_dir = archive_dir / "scripts"
        manifest_dir = archive_dir / "share" / "crystalhd"
        scripts_dir.mkdir(parents=True, mode=0o700)
        manifest_dir.mkdir(parents=True, mode=0o700)
        copy = scripts_dir / "crystalhd-config"
        manifest_copy = manifest_dir / MANIFEST.name
        shutil.copyfile(str(SCRIPT), str(copy))
        shutil.copyfile(str(MANIFEST), str(manifest_copy))
        os.chmod(str(copy), 0o755)
        os.chmod(str(manifest_copy), 0o644)
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
