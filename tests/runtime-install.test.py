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
STAGES = ('udev', 'firmware', 'library', 'gstreamer', 'vaapi', 'check', 'config', 'record')
MOCK = r'''
import os,sys,time
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
if phase=='record':
 version=root/data.lstrip('/')/'crystalhd/VERSION'
 version.parent.mkdir(parents=True,exist_ok=True)
 if remove:
  if version.exists():version.unlink()
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
        self.environment = dict(os.environ, TEST_BUNDLE=self.bundle, TEST_GATE=str(self.root/'release'))
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
        result = self.run_make(fail='activate')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.version.read_text(), 'previous\n')
        self.assertEqual((self.stage/'usr/share/crystalhd-test/library').read_text(), 'new')
        self.assertEqual((self.stage/'usr/share/crystalhd-test/vaapi').read_text(), 'new')
        self.assertTrue(self.pending.exists())
        self.assertEqual(self.run_make().returncode, 0)
        self.assertEqual((self.stage/'etc/crystalhd/config.ini').read_text(), 'private saved defaults\n')

    def test_dry_run_has_no_destination_side_effects(self):
        self.assertEqual(self.run_make(dry=True).returncode, 0)
        self.assertFalse(self.stage.exists())
        self.assertEqual(self.run_make('uninstall-runtime', dry=True).returncode, 0)
        self.assertFalse(self.stage.exists())

    def test_alternate_root_with_spaces_and_unicode(self):
        self.stage = self.root/'stage with spaces 🧪'
        result = self.run_make()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual((self.stage/'usr/share/crystalhd/VERSION').read_text().strip(), self.bundle)
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
        self.assertEqual(json.loads(self.pending.read_text())['stage'], 'activate')
        self.assertEqual(outside.read_text(), 'preserve')

    def direct_arguments(self):
        return SimpleNamespace(make=str(self.mock), destdir=str(self.stage), prefix='/usr',
                               bindir='/usr/bin', datadir='/usr/share', uninstall=False)

    def test_publish_failures_remain_marked_and_complete_retry_repairs_them(self):
        original = INSTALLER.publish_file
        for stop in (1, 4, 9):
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
                    with self.assertRaises(OSError):INSTALLER.install(self.direct_arguments())
                self.assertTrue(self.pending.exists())
                self.assertEqual(self.version.read_text(), 'previous\n')
                result = self.run_make()
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertFalse(self.pending.exists())
                for phase in STAGES:
                    self.assertEqual((self.stage/'usr/share/crystalhd-test'/phase).read_text(), 'new')

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
                while Path('/proc',str(child)).exists():
                    status=Path('/proc',str(child),'status')
                    if status.exists() and 'State:\tZ' in status.read_text():break
                    self.assertLess(time.monotonic(),deadline);time.sleep(.01)
                result=self.run_make()
                self.assertEqual(result.returncode,0,result.stdout)
                self.assertFalse(self.pending.exists())
            finally:
                gate.touch()
                if process.poll() is None:process.terminate();process.wait(timeout=3)


if __name__ == '__main__':
    unittest.main()
