#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Actual make/runner interruption tests with private mock installation targets."""
import ast
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('runtime_installer', str(ROOT/'scripts/install-runtime.py'))
INSTALLER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTALLER)
PROVENANCE = sys.modules['crystalhd_provenance']
STAGES = ('udev', 'firmware', 'library', 'gstreamer', 'vaapi', 'check', 'config', 'record')
MOCK = r'''
import os,shutil,sys,time
from pathlib import Path
args=sys.argv[1:]
directory=args[args.index('-C')+1] if '-C' in args else ''
phase={'driver/linux':'udev','firmware':'firmware','linux_lib/libcrystalhd':'library',
       'filters/gst/gst-plugin-1.0':'gstreamer','filters/vaapi':'vaapi'}.get(directory)
if phase is None:
 phase=next(a for a in args if a.startswith(('install-', 'uninstall-'))).split('-',1)[1]
 phase={'runtime-record':'record', 'runtime-activate':'activate'}.get(phase,phase)
remove=any(a.startswith('uninstall') for a in args)
if phase=='activate':sys.exit(23 if os.environ.get('FAIL_STAGE')==phase else 0)
root=Path(next(a.split('=',1)[1] for a in args if a.startswith('DESTDIR=')))
data=next((a.split('=',1)[1] for a in args if a.startswith('DATADIR=')), '/usr/share')
path=root/'usr/share/crystalhd-test'/phase
path.parent.mkdir(parents=True,exist_ok=True)
if remove:
 if path.exists():path.unlink()
else:path.write_text(Path(os.environ['TEST_TEMPLATE']).read_text() if 'TEST_TEMPLATE' in os.environ else 'new')
component_paths={
 'library':[root/'usr/lib/libcrystalhd.so.3.6'],
 'gstreamer':[root/'usr/lib/gstreamer-1.0/libgstcrystalhd.so',
              root/'usr/bin/crystalhd-play',root/'usr/share/crystalhd/player.py'],
 'vaapi':[root/'usr/lib/dri/crystalhd_drv_video.so'],
}.get(phase,[])
if remove:
 for component in component_paths:
  if component.exists():component.unlink()
else:
 for component in component_paths:
  component.parent.mkdir(parents=True,exist_ok=True)
  if component.name in ('crystalhd-play','player.py'):
   component.write_text('# CrystalHD source identity: '+os.environ['CRYSTALHD_SOURCE_ID']+'\n')
  else:shutil.copyfile(os.environ.get('TEST_ELF','/bin/true'),component)
if phase=='record':
 version=root/data.lstrip('/')/'crystalhd/VERSION'
 receipt=root/data.lstrip('/')/'crystalhd/runtime-components-v1.json'
 version.parent.mkdir(parents=True,exist_ok=True)
 if remove:
  if version.exists():version.unlink()
  if receipt.exists():receipt.unlink()
 else:version.write_text(os.environ['TEST_BUNDLE']+chr(10))
if os.environ.get('HOLD_STAGE')==phase:
 gate=Path(os.environ['TEST_GATE']);gate.with_suffix('.pid').write_text(str(os.getpid()))
 end=time.monotonic()+15
 while not gate.exists():
  if time.monotonic()>end:sys.exit(78)
  time.sleep(.01)
if os.environ.get('FAIL_STAGE')==phase:sys.exit(23)
'''


class RuntimeInstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='crystalhd-runtime-install-')
        self.root = Path(self.temporary.name)
        self.stage = self.root / 'stage'
        self.bundle = (ROOT / 'VERSION').read_text().strip()
        self.mock = self.root / 'mock-make'
        self.mock.write_text('#!' + sys.executable + '\n' + MOCK)
        self.mock.chmod(0o755)
        self.pending = self.stage / 'var/lib/crystalhd/runtime-install.pending'
        self.version = self.stage / 'usr/share/crystalhd/VERSION'
        self.source_id = 'git:' + ('0' * 40)
        self.environment = dict(os.environ, TEST_BUNDLE=self.bundle,
                                TEST_GATE=str(self.root/'release'),
                                CRYSTALHD_SOURCE_ID=self.source_id)
        preparation = self.root/'preparation'
        preparation.mkdir()
        self.environment['TMPDIR'] = str(preparation)
        self.environment.pop('FAIL_STAGE', None)
        self.environment.pop('HOLD_STAGE', None)

    def tearDown(self):
        (self.root/'release').touch()
        self.temporary.cleanup()

    def command(self, target='install-runtime', dry=False):
        return ['make', '--no-print-directory', '-s'] + (['-n'] if dry else []) + [
            '-f', os.environ.get('RUNTIME_TEST_MAKEFILE', str(ROOT/'Makefile')),
            '-o', 'library', '-o', 'gstreamer', '-o', 'vaapi',
            'MAKE='+str(self.mock), 'PYTHON3='+sys.executable,
            'DESTDIR='+str(self.stage), target]

    def run_make(self, target='install-runtime', fail=None, dry=False):
        environment = dict(self.environment)
        if fail:environment['FAIL_STAGE'] = fail
        return subprocess.run(self.command(target, dry), cwd=str(ROOT), env=environment,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True, timeout=15)

    def seed(self):
        self.version.parent.mkdir(parents=True, exist_ok=True)
        self.version.write_text('previous\n')
        for phase in STAGES:
            path = self.stage/'usr/share/crystalhd-test'/phase
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('old')
        old_components = [
            self.stage/'usr/lib/libcrystalhd.so.3.6',
            self.stage/'usr/lib/gstreamer-1.0/libgstcrystalhd.so',
            self.stage/'usr/lib/dri/crystalhd_drv_video.so',
        ]
        for path in old_components:
            path.parent.mkdir(parents=True, exist_ok=True)
            import shutil
            shutil.copyfile('/bin/false', str(path))
        for path in (self.stage/'usr/bin/crystalhd-play',
                     self.stage/'usr/share/crystalhd/player.py'):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('# CrystalHD source identity: ' + self.source_id + '\n')
        receipt = self.stage/'usr/share/crystalhd/runtime-components-v1.json'
        if receipt.exists():
            receipt.unlink()
        INSTALLER.create_receipt(self.stage, 'previous', '/usr/bin',
                                 '/usr/share', self.source_id)
        user = self.stage/'etc/crystalhd/config.ini'
        user.parent.mkdir(parents=True, exist_ok=True)
        user.write_text('private saved defaults\n')

    def test_each_preparation_failure_preserves_previous_payload(self):
        self.seed()
        for phase in STAGES:
            with self.subTest(phase=phase):
                result = self.run_make(fail=phase)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.pending.exists())
                self.assertEqual(self.version.read_text(), 'previous\n')
                for entry in STAGES:
                    self.assertEqual((self.stage/'usr/share/crystalhd-test'/entry).read_text(), 'old')
                self.assertEqual(list((self.root/'preparation').iterdir()), [])
        self.assertEqual(self.run_make().returncode, 0)

    def test_old_version_and_user_defaults_survive_until_record_stage(self):
        self.seed()
        receipt = self.stage/'usr/share/crystalhd/runtime-components-v1.json'
        receipt_before = receipt.stat()
        receipt_bytes = receipt.read_bytes()
        result = self.run_make(fail='activate')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.version.read_text(), 'previous\n')
        self.assertEqual((self.stage/'usr/share/crystalhd-test/library').read_text(), 'old')
        self.assertEqual((self.stage/'usr/share/crystalhd-test/vaapi').read_text(), 'old')
        self.assertTrue(self.pending.exists())
        self.assertEqual(json.loads(self.pending.read_text())['stage'], 'rollback-activate-failed')
        receipt_after = receipt.stat()
        self.assertEqual(receipt.read_bytes(), receipt_bytes)
        self.assertEqual((receipt_after.st_ino, receipt_after.st_mode),
                         (receipt_before.st_ino, receipt_before.st_mode))
        self.assertEqual(self.run_make().returncode, 0)
        self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))
        self.assertEqual((self.stage/'etc/crystalhd/config.ini').read_text(), 'private saved defaults\n')

    def test_dry_run_has_no_destination_side_effects(self):
        self.assertEqual(self.run_make(dry=True).returncode, 0)
        self.assertFalse(self.stage.exists())
        self.assertEqual(self.run_make('uninstall-runtime', dry=True).returncode, 0)
        self.assertFalse(self.stage.exists())

    def test_success_records_each_userspace_component(self):
        result = self.run_make()
        self.assertEqual(result.returncode, 0, result.stdout)
        receipt = self.stage/'usr/share/crystalhd/runtime-components-v1.json'
        record = json.loads(receipt.read_text())
        self.assertEqual(record['schema'], 1)
        self.assertEqual(record['bundle'], self.bundle)
        self.assertEqual(set(record['components']),
                         {'dil', 'gstreamer', 'vaapi', 'player'})
        messages, failed = PROVENANCE.verify_receipt(
            receipt, self.stage, self.bundle, {})
        self.assertFalse(failed, messages)
        self.assertEqual([level for level, message in messages].count('OK'), 4)

    def test_source_identity_distinguishes_archive_clean_git_and_dirty_tree(self):
        archive = self.root / 'archive'
        archive.mkdir()
        revision = '1' * 40
        (archive / 'SOURCE_REVISION').write_text(revision + '\n')
        with mock.patch.dict(os.environ, {'CRYSTALHD_SOURCE_ID': ''}):
            self.assertEqual(PROVENANCE.source_identity(archive),
                             'archive:' + revision)

        checkout = self.root / 'checkout'
        checkout.mkdir()
        (checkout / 'SOURCE_REVISION').write_text('$Format:%H$\n')
        completed = subprocess.CompletedProcess([], 0)
        with mock.patch.dict(os.environ, {'CRYSTALHD_SOURCE_ID': ''}), \
                mock.patch.object(PROVENANCE.subprocess, 'run',
                                  return_value=completed), \
                mock.patch.object(PROVENANCE.subprocess, 'check_output',
                                  return_value=('2' * 40) + '\n'):
            self.assertEqual(PROVENANCE.source_identity(checkout),
                             'git:' + ('2' * 40))
        completed = subprocess.CompletedProcess([], 1)
        with mock.patch.dict(os.environ, {'CRYSTALHD_SOURCE_ID': ''}), \
                mock.patch.object(PROVENANCE.subprocess, 'run',
                                  return_value=completed), \
                mock.patch.object(PROVENANCE.subprocess, 'check_output',
                                  return_value=('2' * 40) + '\n'):
            self.assertEqual(PROVENANCE.source_identity(checkout), 'unknown')

    def test_same_version_distinct_builds_have_distinct_component_identities(self):
        self.assertEqual(self.run_make().returncode, 0)
        receipt = self.stage/'usr/share/crystalhd/runtime-components-v1.json'
        first = json.loads(receipt.read_text())
        self.environment['TEST_ELF'] = '/bin/false'
        self.environment['CRYSTALHD_SOURCE_ID'] = 'git:' + ('1' * 40)
        result = self.run_make()
        self.assertEqual(result.returncode, 0, result.stdout)
        second = json.loads(receipt.read_text())
        self.assertEqual(first['bundle'], second['bundle'])
        for component in ('dil', 'gstreamer', 'vaapi'):
            self.assertNotEqual(
                first['components'][component]['files'][0]['gnu_build_id'],
                second['components'][component]['files'][0]['gnu_build_id'])
        self.assertNotEqual(first['components']['player']['source_id'],
                            second['components']['player']['source_id'])

    def test_alternate_root_with_spaces_and_unicode(self):
        self.stage = self.root/'stage with spaces 🧪'
        result = self.run_make()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual((self.stage/'usr/share/crystalhd/VERSION').read_text().strip(), self.bundle)
        self.assertTrue((self.stage/'usr/share/crystalhd/runtime-components-v1.json').is_file())
        self.assertFalse((self.stage/'var/lib/crystalhd/runtime-install.pending').exists())

    def test_make_query_and_touch_do_not_change_installation_state(self):
        for flag in ('-q', '-t'):
            with self.subTest(flag=flag):
                command = self.command()
                command.insert(1, flag)
                subprocess.run(command, cwd=str(ROOT), env=self.environment,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=15)
                self.assertFalse(self.stage.exists())

    def test_uninstall_failure_and_repeated_success(self):
        self.assertEqual(self.run_make().returncode, 0)
        self.assertNotEqual(self.run_make('uninstall-runtime', fail='library').returncode, 0)
        self.assertEqual(json.loads(self.pending.read_text())['operation'], 'uninstall')
        for _ in range(2):
            result = self.run_make('uninstall-runtime')
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertFalse(self.pending.exists())
            self.assertFalse(self.version.exists())
            self.assertFalse((self.stage/'usr/share/crystalhd/runtime-components-v1.json').exists())
        self.assertFalse(list(self.stage.rglob('*.lock')))

    def test_state_symlink_escape_and_unsafe_directory_fail_before_payload(self):
        self.stage.mkdir()
        outside = self.root/'outside'
        outside.mkdir()
        (self.stage/'var').symlink_to(outside, target_is_directory=True)
        result = self.run_make()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.stage/'usr').exists())
        (self.stage/'var').unlink()
        self.pending.parent.mkdir(parents=True)
        self.pending.parent.chmod(0o777)
        result = self.run_make()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.stage/'usr').exists())

    def test_pending_symlink_is_replaced_without_modifying_its_target(self):
        self.pending.parent.mkdir(parents=True)
        outside = self.root/'preserve.txt'
        outside.write_text('preserve')
        self.pending.symlink_to(outside)
        self.assertNotEqual(self.run_make(fail='activate').returncode, 0)
        self.assertFalse(self.pending.is_symlink())
        self.assertEqual(json.loads(self.pending.read_text())['stage'], 'rollback-activate-failed')
        self.assertEqual(outside.read_text(), 'preserve')

    def direct_arguments(self):
        return SimpleNamespace(make=str(self.mock), destdir=str(self.stage), prefix='/usr',
                               bindir='/usr/bin', datadir='/usr/share', uninstall=False)

    def direct_install(self):
        with mock.patch.dict(os.environ, self.environment, clear=True), \
                mock.patch.object(INSTALLER.tempfile, 'tempdir', str(self.root/'preparation')):
            INSTALLER.install(self.direct_arguments())

    def test_publish_failure_restores_previous_payload_before_a_retry(self):
        original = INSTALLER.publish_file
        for stop in range(1, 10):
            with self.subTest(publish=stop):
                self.seed()
                calls = []
                def publish(source, target):
                    calls.append(target)
                    if len(calls) == stop:
                        raise OSError(28, 'injected full destination')
                    return original(source, target)
                with mock.patch.dict(os.environ, self.environment, clear=True), \
                        mock.patch.object(INSTALLER.tempfile, 'tempdir', str(self.root/'preparation')), \
                        mock.patch.object(INSTALLER, 'publish_file', side_effect=publish):
                    with self.assertRaises((OSError, RuntimeError)):
                        INSTALLER.install(self.direct_arguments())
                self.assertFalse(self.pending.exists())
                self.assertEqual(self.version.read_text(), 'previous\n')
                for phase in STAGES:
                    self.assertEqual((self.stage/'usr/share/crystalhd-test'/phase).read_text(), 'old')
                self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))
                result = self.run_make()
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertFalse(self.pending.exists())
                for phase in STAGES:
                    self.assertEqual((self.stage/'usr/share/crystalhd-test'/phase).read_text(), 'new')

    def test_version_replace_then_error_restores_inode_modes_and_symlinks(self):
        self.seed()
        library = self.stage/'usr/share/crystalhd-test/library'
        library.chmod(0o751)
        alias = self.root/'old-library-alias'
        os.link(str(library), str(alias))
        outside = self.root/'outside-configuration'
        outside.write_text('do not modify')
        link = self.stage/'usr/share/crystalhd-test/config'
        link.unlink(); link.symlink_to(outside)
        receipt = self.stage/'usr/share/crystalhd/runtime-components-v1.json'
        previous = {p: p.lstat() for p in [library, link, self.version, receipt]}
        original = INSTALLER.publish_file
        def publish(source, target):
            original(source, target)
            if target == self.version:
                raise OSError(5, 'post-replace failure')
        with mock.patch.object(INSTALLER, 'publish_file', side_effect=publish):
            with self.assertRaises(INSTALLER.InstallRestored):self.direct_install()
        for path, old in previous.items():
            current = path.lstat()
            self.assertEqual((current.st_ino, current.st_mode, current.st_uid, current.st_gid,
                              current.st_mtime_ns),
                             (old.st_ino, old.st_mode, old.st_uid, old.st_gid, old.st_mtime_ns))
        self.assertEqual(library.stat().st_ino, alias.stat().st_ino)
        self.assertEqual(library.stat().st_nlink, 2)
        self.assertEqual(library.read_text(), 'old')
        self.assertEqual(os.readlink(str(link)), str(outside))
        self.assertEqual(outside.read_text(), 'do not modify')
        self.assertEqual(self.version.read_text(), 'previous\n')
        self.assertFalse(self.pending.exists())
        self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))

    def test_receipt_publish_failure_restores_previous_receipt_and_payload(self):
        self.seed()
        receipt = self.stage/'usr/share/crystalhd/runtime-components-v1.json'
        before = receipt.stat()
        old_receipt = receipt.read_bytes()
        original = INSTALLER.publish_file
        def publish(source, target):
            original(source, target)
            if target == receipt:
                raise OSError(28, 'injected receipt publication failure')
        with mock.patch.object(INSTALLER, 'publish_file', side_effect=publish):
            with self.assertRaises(INSTALLER.InstallRestored):
                self.direct_install()
        after = receipt.stat()
        self.assertEqual(receipt.read_bytes(), old_receipt)
        self.assertEqual((after.st_ino, after.st_mode),
                         (before.st_ino, before.st_mode))
        self.assertEqual(self.version.read_text(), 'previous\n')
        self.assertEqual((self.stage/'usr/share/crystalhd-test/library').read_text(),
                         'old')
        self.assertFalse(self.pending.exists())

    def test_first_install_version_failure_removes_new_receipt_and_payload(self):
        original = INSTALLER.publish_file
        def publish(source, target):
            original(source, target)
            if target == self.version:
                raise OSError(28, 'injected VERSION publication failure')
        with mock.patch.object(INSTALLER, 'publish_file', side_effect=publish):
            with self.assertRaises(INSTALLER.InstallRestored):
                self.direct_install()
        self.assertFalse(self.version.exists())
        self.assertFalse((self.stage/'usr/share/crystalhd/runtime-components-v1.json').exists())
        self.assertFalse((self.stage/'usr/lib/libcrystalhd.so.3.6').exists())
        self.assertFalse(self.pending.exists())

    def test_first_install_failure_removes_introduced_files(self):
        original = INSTALLER.publish_file
        def publish(source, target):
            original(source, target)
            if target.name == 'firmware':raise OSError(28, 'full destination')
        with mock.patch.object(INSTALLER, 'publish_file', side_effect=publish):
            with self.assertRaises(INSTALLER.InstallRestored):self.direct_install()
        self.assertFalse(self.pending.exists())
        self.assertFalse(self.version.exists())
        self.assertFalse((self.stage/'usr/share/crystalhd/runtime-components-v1.json').exists())
        self.assertFalse(list(self.stage.glob('usr/share/crystalhd-test/*')))
        self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))

    def test_transient_activation_failure_reactivates_restored_files(self):
        self.seed()
        original = subprocess.run
        activations = []
        def run(command, **kwargs):
            if 'install-runtime-activate' in command:
                current = (self.stage/'usr/share/crystalhd-test/library').read_text()
                activations.append(current)
                if len(activations) == 1:
                    self.assertEqual(current, 'new')
                    raise subprocess.CalledProcessError(23, command)
                self.assertEqual(current, 'old')
            return original(command, **kwargs)
        with mock.patch.object(INSTALLER.subprocess, 'run', side_effect=run):
            with self.assertRaises(INSTALLER.InstallRestored):self.direct_install()
        self.assertEqual(activations, ['new', 'old'])
        self.assertEqual(self.version.read_text(), 'previous\n')
        self.assertFalse(self.pending.exists())
        self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))

    def test_previous_pending_inode_is_restored_instead_of_declaring_success(self):
        self.seed()
        self.pending.parent.mkdir(parents=True)
        self.pending.write_bytes(b'previous incomplete state\n')
        self.pending.chmod(0o600)
        before = self.pending.stat()
        original = INSTALLER.publish_file
        def publish(source, target):
            if target.name == 'library':raise OSError(28, 'full destination')
            original(source, target)
        with mock.patch.object(INSTALLER, 'publish_file', side_effect=publish):
            with self.assertRaises(INSTALLER.InstallRestored):self.direct_install()
        after = self.pending.stat()
        self.assertEqual(self.pending.read_bytes(), b'previous incomplete state\n')
        self.assertEqual((after.st_ino, after.st_mode), (before.st_ino, before.st_mode))
        self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))

    def test_failed_rollback_retains_recovery_links_until_complete_retry(self):
        self.seed()
        original_publish = INSTALLER.publish_file
        original_replace = os.replace
        def publish(source, target):
            if target == self.version:raise OSError(28, 'full destination')
            original_publish(source, target)
        def replace(source, target):
            if (Path(source).parent.name.startswith('.crystalhd-undo-') and
                    Path(target).name == 'library'):
                raise PermissionError(13, 'injected rollback denial')
            return original_replace(source, target)
        with mock.patch.object(INSTALLER, 'publish_file', side_effect=publish), \
                mock.patch.object(INSTALLER.os, 'replace', side_effect=replace):
            with self.assertRaisesRegex(RuntimeError, 'rollback is incomplete'):
                self.direct_install()
        record = json.loads(self.pending.read_text())
        self.assertEqual(record['stage'], 'rollback-failed')
        copies = [Path(name)/'library' for name in record['rollback_directories']]
        self.assertTrue(any(p.exists() and p.read_text() == 'old' for p in copies))
        self.assertEqual((self.stage/'usr/share/crystalhd-test/library').read_text(), 'new')
        result = self.run_make()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.pending.exists())
        self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))

    def test_marker_write_failure_does_not_skip_file_restoration(self):
        self.seed()
        original_publish = INSTALLER.publish_file
        original_marker = INSTALLER.write_marker
        def publish(source, target):
            if target.name == 'library':raise OSError(28, 'full destination')
            original_publish(source, target)
        def marker(path, bundle, phase, *args, **kwargs):
            if phase == 'rollback':raise OSError(28, 'full state filesystem')
            return original_marker(path, bundle, phase, *args, **kwargs)
        with mock.patch.object(INSTALLER, 'publish_file', side_effect=publish), \
                mock.patch.object(INSTALLER, 'write_marker', side_effect=marker):
            with self.assertRaises(INSTALLER.InstallRestored):self.direct_install()
        for phase in STAGES:
            self.assertEqual((self.stage/'usr/share/crystalhd-test'/phase).read_text(), 'old')
        self.assertFalse(self.pending.exists())

    def test_repeated_failed_recovery_keeps_all_earlier_undo_links_indexed(self):
        self.seed()
        self.assertNotEqual(self.run_make(fail='activate').returncode, 0)
        previous = set(json.loads(self.pending.read_text())['rollback_directories'])
        self.assertTrue(previous)
        self.assertNotEqual(self.run_make(fail='activate').returncode, 0)
        current = set(json.loads(self.pending.read_text())['rollback_directories'])
        self.assertTrue(previous <= current)
        self.assertTrue(current - previous)
        result = self.run_make()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.pending.exists())
        self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))

    def test_undo_preparation_failure_changes_no_payload_or_marker(self):
        self.seed()
        with mock.patch.object(INSTALLER.os, 'link', side_effect=OSError(95, 'no hardlinks')):
            with self.assertRaises(OSError):self.direct_install()
        for phase in STAGES:
            self.assertEqual((self.stage/'usr/share/crystalhd-test'/phase).read_text(), 'old')
        self.assertEqual(self.version.read_text(), 'previous\n')
        self.assertFalse(self.pending.exists())
        self.assertFalse(list(self.stage.rglob('.crystalhd-undo-*')))

    def test_successful_retry_does_not_remove_unrelated_recovery_directories(self):
        self.seed()
        self.pending.parent.mkdir(parents=True)
        unrelated = self.root/'.crystalhd-undo-unrelated'
        unrelated.mkdir(mode=0o700)
        owned = self.stage/'usr/share/crystalhd-test/.crystalhd-undo-foreign'
        owned.mkdir(mode=0o700)
        (owned/'unrelated-data').write_text('preserve')
        self.pending.write_text(json.dumps({'schema': 1, 'rollback_directories':
                                            [str(unrelated), str(owned)]}))
        result = self.run_make()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(unrelated.is_dir())
        self.assertEqual((owned/'unrelated-data').read_text(), 'preserve')

    def test_uninstall_retains_a_failed_updates_recovery_index(self):
        self.seed()
        result = self.run_make(fail='activate')
        self.assertNotEqual(result.returncode, 0)
        previous = json.loads(self.pending.read_text())['rollback_directories']
        self.assertTrue(previous)
        result = self.run_make('uninstall-runtime')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.version.exists())
        record = json.loads(self.pending.read_text())
        self.assertEqual(record['stage'], 'uninstall-recovery-retained')
        self.assertEqual(record['rollback_directories'], previous)
        self.assertTrue(any(Path(p).exists() for p in previous))

    def test_restore_fsync_failure_retains_original_inode_and_incomplete_marker(self):
        self.seed()
        original_publish = INSTALLER.publish_file
        original_sync = INSTALLER.sync_directory
        original_replace = INSTALLER.os.replace
        restoring = [False]
        def publish(source, target):
            if target == self.version:raise OSError(28, 'full destination')
            original_publish(source, target)
        def replace(source, target):
            result = original_replace(source, target)
            if (Path(source).parent.name.startswith('.crystalhd-undo-') and
                    Path(target).name == 'library'):
                restoring[0] = True
            return result
        def sync(directory):
            if restoring[0] and directory == self.stage/'usr/share/crystalhd-test':
                restoring[0] = False
                raise OSError(5, 'restore directory sync failed')
            return original_sync(directory)
        with mock.patch.object(INSTALLER, 'publish_file', side_effect=publish), \
                mock.patch.object(INSTALLER.os, 'replace', side_effect=replace), \
                mock.patch.object(INSTALLER, 'sync_directory', side_effect=sync):
            with self.assertRaisesRegex(RuntimeError, 'rollback is incomplete'):
                self.direct_install()
        record = json.loads(self.pending.read_text())
        copies = [Path(name)/'library' for name in record['rollback_directories']]
        self.assertTrue(any(p.exists() and p.read_text() == 'old' for p in copies))
        self.assertEqual((self.stage/'usr/share/crystalhd-test/library').read_text(), 'old')

    def test_published_files_come_from_prepared_bytes(self):
        template = self.root/'build-input'
        template.write_text('prepared')
        environment = dict(self.environment, TEST_TEMPLATE=str(template))
        original = INSTALLER.publish_file
        def publish(source, target):
            template.write_text('changed after preparation')
            return original(source, target)
        with mock.patch.dict(os.environ, environment, clear=True), \
                mock.patch.object(INSTALLER.tempfile, 'tempdir', str(self.root/'preparation')), \
                mock.patch.object(INSTALLER, 'publish_file', side_effect=publish):
            INSTALLER.install(self.direct_arguments())
        for phase in STAGES:
            self.assertEqual((self.stage/'usr/share/crystalhd-test'/phase).read_text(), 'prepared')
        self.assertFalse(self.pending.exists())

    def test_partial_file_copy_preserves_old_leaf_and_cleans_temporary(self):
        source = self.root/'new-file'
        source.write_bytes(b'new contents')
        target = self.root/'installed-file'
        target.write_bytes(b'old contents')
        def fail_copy(content, output):
            output.write(content.read(3))
            raise OSError(28, 'injected short copy')
        with mock.patch.object(INSTALLER.shutil, 'copyfileobj', side_effect=fail_copy):
            with self.assertRaises(OSError):INSTALLER.publish_file(source, target)
        self.assertEqual(target.read_bytes(), b'old contents')
        self.assertFalse(list(self.root.glob('.crystalhd-install-*')))

    def test_payload_path_validation_preserves_user_files(self):
        prepared = self.root/'payload'
        source = prepared/'usr/bin/tool'
        source.parent.mkdir(parents=True)
        source.write_text('new')
        self.stage.mkdir()
        outside = self.root/'outside-payload'
        outside.mkdir()
        (self.stage/'usr').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(RuntimeError):INSTALLER.payload_files(prepared, self.stage)
        self.assertEqual(list(outside.iterdir()), [])
        (self.stage/'usr').unlink()
        user = prepared/'etc/crystalhd/config.ini'
        user.parent.mkdir(parents=True)
        user.write_text('must not overwrite defaults')
        with self.assertRaises(RuntimeError):INSTALLER.payload_files(prepared, self.stage)

    def test_prepared_payload_cannot_supply_a_completion_receipt(self):
        self.seed()
        with self.assertRaisesRegex(ValueError, 'already contains'):
            INSTALLER.create_receipt(self.stage, 'forged', '/usr/bin',
                                     '/usr/share', self.source_id)

    def test_receipt_rejects_a_malformed_player_source_identity(self):
        self.seed()
        with self.assertRaisesRegex(ValueError, 'source identity'):
            INSTALLER.create_receipt(self.stage, 'forged', '/usr/bin',
                                     '/usr/share', 'git:short')

    def test_preparation_failure_retains_an_existing_incomplete_record(self):
        self.seed()
        self.pending.parent.mkdir(parents=True)
        self.pending.write_text('{"stage":"previous-partial-publication"}')
        previous = self.pending.read_bytes()
        self.assertNotEqual(self.run_make(fail='library').returncode, 0)
        self.assertEqual(self.pending.read_bytes(), previous)
        self.assertEqual(self.version.read_text(), 'previous\n')

    def test_payload_links_and_merged_usr_destination(self):
        prepared = self.root/'linked-payload'
        library = prepared/'usr/lib/libsample.so.3'
        library.parent.mkdir(parents=True)
        library.write_bytes(b'library')
        library.chmod(0o755)
        alias = library.with_name('libsample.so')
        alias.symlink_to(library.name)
        firmware = prepared/'lib/firmware/sample.bin'
        firmware.parent.mkdir(parents=True)
        firmware.write_bytes(b'firmware')
        (self.stage/'usr/lib').mkdir(parents=True)
        (self.stage/'lib').symlink_to('usr/lib', target_is_directory=True)
        for relative, source in INSTALLER.payload_files(prepared, self.stage):
            INSTALLER.publish_file(source, self.stage/relative)
        self.assertEqual((self.stage/'usr/lib/firmware/sample.bin').read_bytes(), b'firmware')
        self.assertEqual((self.stage/'usr/lib/libsample.so').read_bytes(), b'library')
        self.assertEqual(os.readlink(str(self.stage/'usr/lib/libsample.so')), library.name)
        self.assertEqual((self.stage/'usr/lib/libsample.so.3').stat().st_mode & 0o777, 0o755)
        # An absolute link into the preparation directory would dangle after
        # cleanup even though it resolves inside the payload during validation.
        alias.unlink()
        alias.symlink_to(library)
        with self.assertRaises(RuntimeError):INSTALLER.payload_files(prepared, self.stage)

    def test_killed_parent_does_not_release_a_still_writing_childs_lock(self):
        environment = dict(self.environment, HOLD_STAGE='library')
        gate = self.root/'release';pidfile=gate.with_suffix('.pid')
        with (self.root/'held.log').open('w') as log:
            process = subprocess.Popen(self.command(), cwd=str(ROOT), env=environment,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                deadline=time.monotonic()+5
                while not pidfile.exists():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(),deadline)
                    time.sleep(.01)
                # Kill the runner, not make; its active child keeps the flock.
                import signal
                child = int(pidfile.read_text())
                status = Path('/proc', str(child), 'status').read_text()
                runner = int(next(line.split()[1] for line in status.splitlines()
                                  if line.startswith('PPid:')))
                self.assertIn(b'scripts/install-runtime.py', Path('/proc', str(runner), 'cmdline').read_bytes())
                os.kill(runner,signal.SIGKILL)
                process.wait(timeout=3)
                self.assertFalse(self.pending.exists())
                result=self.run_make()
                self.assertNotEqual(result.returncode,0)
                self.assertIn('another runtime installation is active',result.stdout)
                self.assertFalse(self.pending.exists())
                gate.touch()
                # Retry only after that exact child has retired its directory lock.
                deadline=time.monotonic()+3
                while True:
                    try:
                        status=Path('/proc',str(child),'status').read_text()
                    except (FileNotFoundError, ProcessLookupError):
                        break
                    if 'State:\tZ' in status:break
                    self.assertLess(time.monotonic(),deadline);time.sleep(.01)
                result=self.run_make()
                self.assertEqual(result.returncode,0,result.stdout)
                self.assertFalse(self.pending.exists())
            finally:
                gate.touch()
                if process.poll() is None:process.terminate();process.wait(timeout=3)


if __name__ == '__main__':
    unittest.main()
