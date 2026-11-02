#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Device-free remote-dialog flows, real shared CLI, errors and ZIP contracts."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parents[1]
ADDON = ROOT / 'addons/script.crystalhd.settings'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


UI = load('crystalhd_kodi_controller', ADDON / 'resources/lib/controller.py')
ZIP = load('crystalhd_kodi_zip', ROOT / 'scripts/package-kodi-settings.py')


class Dialog:
    def __init__(self, selections, confirmations=()):
        self.selections, self.confirmations = list(selections), list(confirmations)
        self.menus, self.messages = [], []

    def select(self, title, items):
        self.menus.append((title, list(items)))
        if not self.selections:
            raise AssertionError('unexpected extra menu: ' + title)
        choice = self.selections.pop(0)
        if choice >= len(items):
            raise AssertionError('choice outside actual menu')
        return choice

    def yesno(self, title, message):
        self.messages.append(message)
        if not self.confirmations:
            raise AssertionError('unexpected confirmation')
        return self.confirmations.pop(0)

    def ok(self, title, message):
        self.messages.append(message)

    def textviewer(self, title, text):
        self.messages.append(text)


class KodiSettingsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='crystalhd-kodi-settings-')
        self.root = Path(self.temp.name)
        self.user = self.root / 'config.ini'
        self.environment = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'),
                            'HOME': str(self.root), 'LC_ALL': 'C'}
        self.backend = UI.Backend(str(ROOT / 'scripts/crystalhd-config.py'), self.environment,
                                 ('--no-system-config', '--user-config', str(self.user)))

    def tearDown(self):
        self.temp.cleanup()

    def write(self, text):
        self.user.write_text('[crystalhd]\nschema = 1\n' + text)
        self.user.chmod(0o600)

    def flow(self, selections, confirmations=(), backend=None):
        dialog = Dialog(selections, confirmations)
        controller = UI.SettingsController(backend or self.backend, dialog)
        controller.run()
        self.assertFalse(dialog.selections)
        self.assertFalse(dialog.confirmations)
        return controller, dialog

    def test_read_only_entry_and_cancel_never_create_configuration(self):
        self.flow([-1])
        self.flow([0, -1, -1])
        self.flow([0, 1, -1], [True])
        self.assertFalse(self.user.exists())

    def test_declining_discard_retains_draft_but_does_not_save(self):
        controller, _ = self.flow([0, 1, -1, -1], [False, True])
        self.assertEqual(controller.pending, {'preset': 'default'})
        self.assertFalse(self.user.exists())

    def test_explicit_preset_save_roundtrips_shared_file_and_reports_restart(self):
        controller, dialog = self.flow([0, 1, 6, -1], [True])
        self.assertFalse(controller.pending)
        self.assertEqual(self.backend.state()['saved_selectors']['preset'], 'default')
        self.assertIn('preset = "default"', self.user.read_text())
        self.assertTrue(any('Saved — restart Kodi' in message for message in dialog.messages))
        self.assertEqual(self.user.stat().st_mode & 0o777, 0o600)
        self.assertFalse((ADDON / 'resources/settings.xml').exists())

    def test_directional_choices_select_profile_and_safe_decoder_mode(self):
        self.write('[profile.desk]\nvaapi.debug = true\n')
        self.flow([1, 1, 6, -1], [True])
        self.assertEqual(self.backend.state()['saved_selectors']['profile'], 'desk')
        self.flow([2, 1, 3, 6, -1], [True])
        self.assertIn('vaapi.h264.mode = "strict"', self.user.read_text())

    def test_unset_and_inherit_are_distinct_and_preserve_other_values(self):
        self.write('[settings]\nvaapi.debug = true\nvaapi.trace = true\n')
        state = self.backend.save(self.backend.state(), {'vaapi.debug': 'unset'})
        self.assertIn('vaapi.debug = unset', self.user.read_text())
        self.backend.save(state, {'vaapi.debug': None, 'vaapi.h264.mode': '"strict"'})
        self.assertNotIn('vaapi.debug', self.user.read_text())
        self.assertIn('vaapi.trace = true', self.user.read_text())

    def test_reset_requires_confirmation_and_clears_all_user_profiles(self):
        self.write('[settings]\nvaapi.debug = true\n[profile.desk]\nvaapi.trace = true\n')
        before = self.user.read_bytes()
        self.flow([6, -1], [False])
        self.assertEqual(self.user.read_bytes(), before)
        _, dialog = self.flow([6, -1], [True])
        self.assertTrue(any('ALL' in message and 'other applications' in message for message in dialog.messages))
        state = self.backend.state()
        self.assertFalse(state['profiles'])
        self.assertTrue(all(item['saved_literal'] is None for item in state['settings']))

    def test_stale_or_malformed_configuration_preserves_existing_bytes(self):
        state = self.backend.state()
        self.write('[settings]\nvaapi.trace = true\n')
        before = self.user.read_bytes()
        with self.assertRaisesRegex(UI.ConfigError, 'reload'):
            self.backend.save(state, {'preset': 'default'})
        self.assertEqual(self.user.read_bytes(), before)
        self.write('not valid configuration\n')
        before = self.user.read_bytes()
        with self.assertRaises(UI.ConfigError):
            self.backend.state()
        self.assertEqual(self.user.read_bytes(), before)

    def test_experiments_unknown_keys_and_unavailable_presets_are_not_offered(self):
        state = self.backend.state()
        for pending in ({'vaapi.h264.mode': '"live"'}, {'module.force_l0s_off': 'true'},
                        {'preset': 'streaming'}, {'preset': 'quality'}):
            with self.subTest(pending=pending), self.assertRaises(UI.ConfigError):
                self.backend.save(state, pending)
        self.assertFalse(self.user.exists())

    def test_missing_helper_and_old_metadata_have_readable_errors(self):
        with self.assertRaisesRegex(UI.ConfigError, 'unavailable'):
            UI.Backend(environment={'PATH': ''})
        backend = UI.Backend('/does/not/exist', self.environment)
        with self.assertRaisesRegex(UI.ConfigError, 'Could not start'):
            backend.state()
        with mock.patch.object(self.backend, 'call', return_value={'format': 'old'}):
            with self.assertRaisesRegex(UI.ConfigError, 'Unsupported'):
                self.backend.state()

    def test_malformed_v1_metadata_and_reset_readback_are_rejected(self):
        state = self.backend.state()
        variants = []
        for field, value in (('profiles', [{'name': 'desk'}]), ('configuration', {}),
                             ('settings', [{}]), ('user_overlay_empty', 'yes')):
            bad = copy.deepcopy(state)
            bad[field] = value
            variants.append(bad)
        for bad in variants:
            with mock.patch.object(self.backend, 'call', return_value=bad):
                with self.assertRaises(UI.ConfigError):
                    self.backend.state()
        with mock.patch.object(self.backend, 'call', return_value={'readers': [{}]}):
            with self.assertRaises(UI.ConfigError):
                self.backend.installation()
        changed = copy.deepcopy(state)
        changed['user_overlay_empty'] = False
        with mock.patch.object(self.backend, 'call'), mock.patch.object(self.backend, 'state', return_value=changed):
            with self.assertRaises(UI.SaveUncertain):
                self.backend.reset(state)

    def test_uncertain_save_blocks_retries_until_explicit_reload(self):
        fake = mock.Mock()
        fake.state.return_value = self.backend.state()
        fake.save.side_effect = UI.SaveUncertain('Save uncertain; reload')
        controller, _ = self.flow([0, 1, 6, 6, -1], [True, True], fake)
        self.assertTrue(controller.write_blocked)
        fake.save.assert_called_once()
        fake.reset.assert_not_called()

    def test_cli_exit4_and_timeout_are_not_reported_as_success_or_retried(self):
        process = mock.Mock()
        process.returncode = 4
        process.communicate.return_value = ('', 'uncertain')
        with mock.patch.object(UI.subprocess, 'Popen', return_value=process) as start:
            with self.assertRaises(UI.SaveUncertain):
                self.backend.call(['save'])
            start.assert_called_once()
        process = mock.Mock()
        process.communicate.side_effect = [subprocess.TimeoutExpired('own-helper', 10), ('', '')]
        with mock.patch.object(UI.subprocess, 'Popen', return_value=process):
            with self.assertRaises(UI.SaveUncertain):
                self.backend.call(['save'])
            process.terminate.assert_called_once()
            process.kill.assert_not_called()

    def test_save_failure_preserves_draft_and_configuration(self):
        fake = mock.Mock()
        fake.state.return_value = self.backend.state()
        fake.save.side_effect = UI.ConfigError('read-only user home')
        controller, dialog = self.flow([0, 1, 6, -1], [True, True], fake)
        self.assertEqual(controller.pending, {'preset': 'default'})
        self.assertIn('read-only user home', dialog.messages)
        self.assertFalse(self.user.exists())

    def test_reload_cannot_allow_writes_while_timed_out_helper_survives(self):
        state = self.backend.state()
        helper = mock.Mock()
        helper.poll.return_value = None
        helper.communicate.side_effect = [subprocess.TimeoutExpired('own-helper', 10),
                                          subprocess.TimeoutExpired('own-helper', 3)]
        with mock.patch.object(UI.subprocess, 'Popen', return_value=helper) as start:
            with self.assertRaises(UI.SaveUncertain):
                self.backend.call(['save'])
            with self.assertRaisesRegex(UI.SaveUncertain, 'still running'):
                self.backend.state()
            with self.assertRaisesRegex(UI.SaveUncertain, 'still running'):
                self.backend.save(state, {'preset': 'default'})
            start.assert_called_once()
            helper.kill.assert_not_called()
        helper.poll.return_value = 0
        healthy = mock.Mock(returncode=0)
        healthy.communicate.return_value = (json.dumps(state), '')
        with mock.patch.object(UI.subprocess, 'Popen', return_value=healthy):
            self.assertEqual(self.backend.state()['revision'], state['revision'])
        self.assertFalse(self.backend._unretired_helpers)

    def test_device_diagnostics_do_not_open_hardware_and_handle_absence(self):
        report = {'readers': [{'component': 'vaapi', 'state': 'unknown'}]}
        with mock.patch.object(self.backend, 'call', return_value=report), \
                mock.patch.object(UI.os, 'stat', side_effect=FileNotFoundError), \
                mock.patch('builtins.open') as opened:
            self.assertFalse(self.backend.installation()['device_node_accessible'])
            opened.assert_not_called()
        with mock.patch.object(self.backend, 'call', return_value=report), \
                mock.patch.object(UI.os, 'stat', return_value=mock.Mock(st_mode=stat.S_IFCHR | 0o660)), \
                mock.patch.object(UI.os, 'access', return_value=True), \
                mock.patch('builtins.open') as opened:
            self.assertTrue(self.backend.installation()['device_node_accessible'])
            opened.assert_not_called()

    def test_zip_is_reproducible_scoped_and_never_overwrites(self):
        source = self.root / 'source'
        for name in ZIP.FILES:
            target = source / 'addons' / ZIP.ADDON / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ADDON / name, target)
        license_path = source / ZIP.LICENSE
        license_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / ZIP.LICENSE, license_path)
        (source / 'addons' / ZIP.ADDON / 'private-config.ini').write_text('not package data')
        first, second = self.root / 'first.zip', self.root / 'second.zip'
        ZIP.build(source, first)
        ZIP.build(source, second)
        self.assertEqual(hashlib.sha256(first.read_bytes()).digest(), hashlib.sha256(second.read_bytes()).digest())
        with zipfile.ZipFile(first) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(len(archive.namelist()), len(ZIP.FILES) + 1)
            self.assertNotIn(ZIP.ADDON + '/private-config.ini', archive.namelist())
            self.assertGreater(len(archive.read(ZIP.ADDON + '/LICENSE.txt')), 1000)
            for info in archive.infolist():
                self.assertEqual(info.date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual(info.external_attr >> 16, 0o100644)
        with self.assertRaises(FileExistsError):
            ZIP.build(source, first)
        metadata = ET.parse(ADDON / 'addon.xml').getroot()
        self.assertEqual(metadata.find('./requires/import').get('version'), '3.0.0')


if __name__ == '__main__':
    unittest.main(verbosity=2)
