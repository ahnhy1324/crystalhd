#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Private slot lifecycle controls using inert, non-loadable ELF headers."""
import argparse
import fcntl
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('native_slots', ROOT / 'scripts/native-runtime-slots.py')
SLOTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SLOTS)


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class SlotTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.store = self.root / 'owned store'
        self.target = self.root / 'rootfs'
        (self.target / 'usr/lib').mkdir(parents=True)
        (self.target / 'usr/bin').mkdir()
        header = bytearray(64)
        header[:7] = b'\x7fELF\x02\x01\x01'
        header[18:20] = (62).to_bytes(2, 'little')
        self.header = bytes(header) + b'inert fixture, never executed\n'
        self.provider = self.target / 'usr/lib/libc.so.6'
        self.provider.write_bytes(self.header + b'fixed provider')
        self.a = self.package('A')
        self.b = self.package('B')

    def package(self, label, invalid_header=False):
        root = self.root / ('package-' + label)
        for name in ('bin', 'lib', 'metadata'):
            (root / name).mkdir(parents=True)
        nodes, sonames = {}, {}
        def node(path, name, origin):
            return {'file': name, 'origin': origin, 'class': 'ELF64',
                    'machine': 'Advanced Micro Devices X86-64',
                    'soname': [Path(name).name] if origin == 'platform' else [],
                    'bytes': path.stat().st_size, 'sha256': digest(path)}
        for kind in SLOTS.PREFLIGHT.KINDS:
            soname = 'lib' + kind + '.so.1'
            name = 'lib/' + soname + '.0.0'
            path = root / name
            path.write_bytes(self.header + label.encode() + kind.encode())
            path.chmod(0o755)
            (root / 'lib' / soname).symlink_to(path.name)
            sonames[kind] = soname
            nodes['payload:' + name] = node(path, name, 'payload')
        for name in ('bin/ffmpeg', 'bin/ffprobe'):
            path = root / name
            path.write_bytes((b'not ELF' if invalid_header else self.header) + label.encode())
            path.chmod(0o755)
            nodes['payload:' + name] = node(path, name, 'payload')
        nodes['platform:lib/libc.so.6'] = node(self.provider, 'lib/libc.so.6', 'platform')
        dependencies = {'schema': 1, 'architecture': ['ELF64', 'Advanced Micro Devices X86-64'],
                        'sonames': sonames, 'nodes': nodes}
        for name, value in [('dependencies', dependencies),
                            ('provenance', {'schema': 1, 'disposition': 'PUBLIC_NOT_READY'})]:
            path = root / 'metadata' / (name + '.json')
            path.write_bytes(encoded(value))
            path.chmod(0o644)
        members = []
        for path in sorted(root.rglob('*')):
            if path.is_dir():
                continue
            name = path.relative_to(root).as_posix()
            if path.is_symlink():
                members.append({'path': name, 'kind': 'symlink', 'target': os.readlink(path)})
            else:
                members.append({'path': name, 'kind': 'file', 'bytes': path.stat().st_size,
                                'sha256': digest(path), 'mode': path.stat().st_mode & 0o7777})
        (root / 'metadata/files.json').write_bytes(encoded(members))
        (root / 'metadata/files.json').chmod(0o644)
        archive = self.root / (label + '.tar')
        with tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as tar:
            for path in sorted(root.rglob('*')):
                tar.add(path, arcname=path.relative_to(root).as_posix(), recursive=False)
        return {'archive': archive, 'archive_sha256': digest(archive),
                **{name + '_sha256': digest(root / 'metadata' / (name + '.json'))
                   for name in ('files', 'dependencies', 'provenance')}}

    def run_action(self, action, package=None, **changes):
        values = {'action': action, 'store': self.store,
                  'rootfs': self.target, 'rootfs_identity': 'inert test rootfs',
                  'platform_relative': 'usr'}
        if package:
            values.update(package)
        values.update(changes)
        return SLOTS.execute(argparse.Namespace(**values))

    def assert_pass(self, report):
        self.assertEqual(report['status'], 'PASS_PRIVATE_SLOTS_FILES_ONLY', report)
        self.assertEqual(report['public_distribution_status'], 'PUBLIC_NOT_READY')
        self.assertEqual(report['runtime_ABI_Kodi_GPU_hardware_AV_acceptance'], 'NOT_RUN')

    def state_bytes(self):
        return (self.store / 'state.json').read_bytes()

    def active_payload(self):
        state = json.loads(self.state_bytes())
        return (self.store / 'slots' / state['active'] / 'bin/ffmpeg').read_bytes()

    def test_install_update_rollback_remove_preserve_user_files_and_cost_scope(self):
        outside = self.root / 'user.ini'
        outside.write_bytes(b'user setting\n')
        a = self.run_action('install', self.a)
        self.assert_pass(a)
        self.assertEqual(a['active'], self.a['archive_sha256'])
        self.assertTrue(self.active_payload().endswith(b'A'))
        self.assertEqual(a['cost']['store_before']['logical_bytes'], 0)
        self.assertGreater(a['cost']['store_after']['logical_bytes'], 0)
        self.assertGreaterEqual(a['cost']['store_observed_maximum']['logical_bytes'],
                                a['cost']['store_after']['logical_bytes'])
        self.assertIsNone(a['cost']['network_bytes'])
        self.assertIn('not child-tree', a['cost']['RSS_scope'])
        config = self.store / 'preferences.ini'
        config.write_bytes(b'preserve unrelated owned-store configuration\n')
        b = self.run_action('update', self.b)
        self.assert_pass(b)
        self.assertEqual(b['previous'], self.a['archive_sha256'])
        self.assertTrue(self.active_payload().endswith(b'B'))
        self.assertNotEqual(self.a['archive_sha256'], self.b['archive_sha256'])
        for expected in (b'A', b'B'):
            self.assert_pass(self.run_action('rollback'))
            self.assertTrue(self.active_payload().endswith(expected))
        self.assert_pass(self.run_action('status'))
        self.assert_pass(self.run_action('remove'))
        self.assert_pass(self.run_action('remove'))
        status = self.run_action('status')
        self.assert_pass(status)
        self.assertIsNone(status['active_prefix'])
        self.assertEqual(status['retained_slots'], [])
        self.assertEqual(config.read_bytes(), b'preserve unrelated owned-store configuration\n')
        self.assertEqual(outside.read_bytes(), b'user setting\n')

    def test_bad_archive_and_validly_pinned_bad_ELF_leave_existing_selection_and_bytes(self):
        self.assert_pass(self.run_action('install', self.a))
        before, payload = self.state_bytes(), self.active_payload()
        bad = dict(self.b, archive_sha256='0' * 64)
        for candidate in (bad, self.package('invalid', invalid_header=True)):
            result = self.run_action('update', candidate)
            self.assertTrue(result['status'].startswith('FAIL_'), result)
            self.assertEqual(self.state_bytes(), before)
            self.assertEqual(self.active_payload(), payload)
            self.assertEqual(len(list((self.store / 'slots').iterdir())), 1)

    def test_state_publication_failure_cleans_candidate_without_changing_active(self):
        self.assert_pass(self.run_action('install', self.a))
        before, payload = self.state_bytes(), self.active_payload()
        with mock.patch.object(SLOTS, 'write_state', side_effect=OSError('publication refused')):
            result = self.run_action('update', self.b)
        self.assertTrue(result['status'].startswith('FAIL_'), result)
        self.assertEqual(self.state_bytes(), before)
        self.assertEqual(self.active_payload(), payload)
        self.assertEqual([p.name for p in (self.store / 'slots').iterdir()], [self.a['archive_sha256']])
        self.assertFalse(any(self.store.glob('.stage-*')))

    def test_initial_state_failure_undoes_only_its_own_initialization_and_allows_retry(self):
        outside = self.root / 'user-config.ini'
        outside.write_bytes(b'preserve this user file')
        for existing_empty in (False, True):
            store = self.root / ('initial-existing' if existing_empty else 'initial-new')
            if existing_empty:
                store.mkdir(mode=0o755)
                store.chmod(0o755)  # Exercise restoration independently of umask.
            with mock.patch.object(SLOTS, 'write_state', side_effect=OSError('initial state refused')):
                report = self.run_action('install', self.a, store=store)
            self.assertTrue(report['status'].startswith('FAIL_'), report)
            self.assertFalse(store.exists() and any(store.iterdir()))
            if existing_empty:
                self.assertEqual(store.stat().st_mode & 0o777, 0o755)
            self.assert_pass(self.run_action('install', self.a, store=store))
            self.assertEqual(outside.read_bytes(), b'preserve this user file')

    def test_initial_failure_preserves_unrelated_new_file_and_existing_mode(self):
        store = self.root / 'initial-user-file'
        store.mkdir()
        store.chmod(0o750)
        def interrupted_initialization(path, state):
            (path / 'user.ini').write_bytes(b'unrelated user preferences')
            raise OSError('initial state refused after a user file appeared')
        with mock.patch.object(SLOTS, 'write_state', side_effect=interrupted_initialization):
            report = self.run_action('install', self.a, store=store)
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertEqual((store / 'user.ini').read_bytes(), b'unrelated user preferences')
        self.assertEqual(store.stat().st_mode & 0o777, 0o750)
        self.assertEqual({path.name for path in store.iterdir()}, {'user.ini'})
        self.assertTrue(self.run_action('install', self.a, store=store)['status'].startswith('FAIL_'))
        self.assertEqual((store / 'user.ini').read_bytes(), b'unrelated user preferences')

    def test_initial_post_replace_interrupt_is_retryable(self):
        real_write = SLOTS.write_state
        def interrupted(store, state):
            real_write(store, state)
            raise KeyboardInterrupt('after initial state replacement')
        with mock.patch.object(SLOTS, 'write_state', side_effect=interrupted):
            report = self.run_action('install', self.a)
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertFalse(self.store.exists() and any(self.store.iterdir()))
        self.assert_pass(self.run_action('install', self.a))

    def test_remove_state_failure_before_replace_restores_selection_and_payload(self):
        self.assert_pass(self.run_action('install', self.a))
        before, payload = self.state_bytes(), self.active_payload()
        with mock.patch.object(SLOTS, 'write_state', side_effect=OSError('unselection refused')):
            report = self.run_action('remove')
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertEqual(self.state_bytes(), before)
        self.assertEqual(self.active_payload(), payload)
        self.assertFalse(any(self.store.glob('.remove-*')))
        self.assert_pass(self.run_action('status'))

    def test_caught_partial_cleanup_failure_stays_visible_and_retries(self):
        self.assert_pass(self.run_action('install', self.a))
        self.assert_pass(self.run_action('update', self.b))
        real_remove = SLOTS.shutil.rmtree
        def incomplete(path, *args, **kwargs):
            path = Path(path)
            if path.name.startswith('.remove-'):
                next(path.glob('*/bin/ffmpeg')).unlink()
                raise OSError('cleanup stopped after one owned leaf')
            return real_remove(path, *args, **kwargs)
        with mock.patch.object(SLOTS.shutil, 'rmtree', side_effect=incomplete):
            report = self.run_action('remove')
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertTrue(report['selection_committed'])
        self.assertIsNone(json.loads(self.state_bytes())['active'])
        self.assertTrue(json.loads(self.state_bytes())['pending_removal'])
        status = self.run_action('status')
        self.assertTrue(status['status'].startswith('FAIL_'), status)
        self.assertTrue(status['pending_removal'])
        self.assert_pass(self.run_action('remove'))
        self.assertFalse(any(self.store.glob('.remove-*')))
        self.assert_pass(self.run_action('status'))
        self.assert_pass(self.run_action('remove'))

    def test_retry_refuses_unknown_or_changed_pending_payload(self):
        self.assert_pass(self.run_action('install', self.a))
        with mock.patch.object(SLOTS.shutil, 'rmtree', side_effect=OSError('cleanup refused')):
            self.assertTrue(self.run_action('remove')['status'].startswith('FAIL_'))
        pending = next(self.store.glob('.remove-*'))
        unknown = pending / 'user.ini'
        unknown.write_bytes(b'preserve an unrelated file')
        report = self.run_action('remove')
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertEqual(unknown.read_bytes(), b'preserve an unrelated file')
        unknown.unlink()
        changed = pending / self.a['archive_sha256'] / 'bin/ffprobe'
        original = changed.read_bytes()
        changed.write_bytes(b'preserve a changed file')
        report = self.run_action('remove')
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertEqual(changed.read_bytes(), b'preserve a changed file')
        changed.write_bytes(original)
        self.assert_pass(self.run_action('remove'))

    def test_remove_post_replace_interrupt_keeps_files_in_tracked_cleanup(self):
        self.assert_pass(self.run_action('install', self.a))
        self.assert_pass(self.run_action('update', self.b))
        real_write = SLOTS.write_state
        def interrupted(store, state):
            real_write(store, state)
            if state.get('pending_removal'):
                raise KeyboardInterrupt('after inactive selection replacement')
        with mock.patch.object(SLOTS, 'write_state', side_effect=interrupted):
            report = self.run_action('remove')
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertTrue(report['selection_committed'])
        state = json.loads(self.state_bytes())
        self.assertIsNone(state['active'])
        self.assertEqual(state['slots'], {})
        self.assertEqual(list((self.store / 'slots').iterdir()), [])
        pending = self.store / next(iter(state['pending_removal']))
        self.assertEqual({p.name for p in pending.iterdir()},
                         {self.a['archive_sha256'], self.b['archive_sha256']})
        self.assertTrue(self.run_action('status')['status'].startswith('FAIL_'))
        self.assert_pass(self.run_action('remove'))
        self.assert_pass(self.run_action('status'))

    def test_untracked_old_cleanup_never_becomes_a_false_status_or_remove_pass(self):
        self.assert_pass(self.run_action('install', self.a))
        payload = self.active_payload()
        orphan = self.store / '.remove-untracked'
        orphan.mkdir()
        preserved = orphan / 'user-file'
        preserved.write_bytes(b'unknown bytes must not be adopted or deleted')
        for action in ('status', 'remove'):
            report = self.run_action(action)
            self.assertTrue(report['status'].startswith('FAIL_'), report)
            self.assertEqual(preserved.read_bytes(), b'unknown bytes must not be adopted or deleted')
            self.assertEqual(self.active_payload(), payload)

    def test_rollback_revalidates_previous_bytes_before_selection(self):
        self.assert_pass(self.run_action('install', self.a))
        self.assert_pass(self.run_action('update', self.b))
        previous = self.store / 'slots' / self.a['archive_sha256'] / 'bin/ffmpeg'
        previous.write_bytes(b'changed previous payload')
        before, payload = self.state_bytes(), self.active_payload()
        result = self.run_action('rollback')
        self.assertTrue(result['status'].startswith('FAIL_'), result)
        self.assertEqual(self.state_bytes(), before)
        self.assertEqual(self.active_payload(), payload)

    def test_nonempty_unowned_and_symlink_store_are_never_adopted(self):
        self.store.mkdir()
        config = self.store / 'user.ini'
        config.write_bytes(b'keep')
        result = self.run_action('install', self.a)
        self.assertTrue(result['status'].startswith('FAIL_'), result)
        self.assertEqual(set(p.name for p in self.store.iterdir()), {'user.ini'})
        self.assertEqual(config.read_bytes(), b'keep')
        other = self.root / 'other'
        other.mkdir()
        alias = self.root / 'alias'
        alias.symlink_to(other, target_is_directory=True)
        result = self.run_action('install', self.a, store=alias)
        self.assertTrue(result['status'].startswith('FAIL_'), result)
        self.assertEqual(list(other.iterdir()), [])

    def test_unmanifested_configuration_inside_payload_prevents_removal(self):
        self.assert_pass(self.run_action('install', self.a))
        config = Path(self.run_action('status')['active_prefix']) / 'user.ini'
        config.write_bytes(b'never delete an unmanifested user file')
        before = self.state_bytes()
        result = self.run_action('remove')
        self.assertTrue(result['status'].startswith('FAIL_'), result)
        self.assertEqual(self.state_bytes(), before)
        self.assertEqual(config.read_bytes(), b'never delete an unmanifested user file')

    def test_provider_failure_cancels_update_but_does_not_block_owned_removal(self):
        self.assert_pass(self.run_action('install', self.a))
        before, payload = self.state_bytes(), self.active_payload()
        self.provider.unlink()
        result = self.run_action('update', self.b)
        self.assertTrue(result['status'].startswith('FAIL_'), result)
        self.assertEqual(self.state_bytes(), before)
        self.assertEqual(self.active_payload(), payload)
        self.assert_pass(self.run_action('remove'))
        self.assertIsNone(self.run_action('status')['active'])

    def test_unsafe_TAR_variants_never_touch_old_slot_or_other_files(self):
        self.assert_pass(self.run_action('install', self.a))
        before, payload = self.state_bytes(), self.active_payload()
        victim = self.store / 'victim'
        victim.write_bytes(b'keep')
        cases = []
        for name, kind, link in [('../victim', tarfile.REGTYPE, ''),
                                 (str(victim), tarfile.REGTYPE, ''),
                                 ('alias', tarfile.LNKTYPE, '../victim'),
                                 ('alias', tarfile.SYMTYPE, '../victim'),
                                 ('fifo', tarfile.FIFOTYPE, '')]:
            entry = tarfile.TarInfo(name)
            entry.type, entry.linkname = kind, link
            cases.append([entry])
        cases.append([tarfile.TarInfo('same'), tarfile.TarInfo('same')])
        for index, entries in enumerate(cases):
            archive = self.root / ('unsafe-' + str(index) + '.tar')
            with tarfile.open(archive, 'w') as tar:
                for entry in entries:
                    tar.addfile(entry, io.BytesIO(b''))
            result = self.run_action('update', dict(self.b, archive=archive,
                                                    archive_sha256=digest(archive)))
            self.assertTrue(result['status'].startswith('FAIL_'), result)
            self.assertEqual(self.state_bytes(), before)
            self.assertEqual(self.active_payload(), payload)
            self.assertEqual(victim.read_bytes(), b'keep')

    def test_operator_lock_serializes_candidate_selection(self):
        self.assert_pass(self.run_action('install', self.a))
        before = self.state_bytes()
        descriptor = os.open(self.store / '.lock', os.O_RDWR)
        real_flock = fcntl.flock
        real_flock(descriptor, fcntl.LOCK_EX)
        attempted, finished, reports = threading.Event(), threading.Event(), []
        def flock(fd, operation):
            if operation == fcntl.LOCK_EX and fd != descriptor:
                attempted.set()
            return real_flock(fd, operation)
        def worker():
            reports.append(self.run_action('update', self.b))
            finished.set()
        thread = threading.Thread(target=worker)
        try:
            with mock.patch.object(SLOTS.fcntl, 'flock', side_effect=flock):
                thread.start()
                self.assertTrue(attempted.wait(2))
                self.assertFalse(finished.is_set())
                self.assertEqual(self.state_bytes(), before)
                real_flock(descriptor, fcntl.LOCK_UN)
                thread.join(5)
                self.assertFalse(thread.is_alive())
        finally:
            real_flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)
            if thread.is_alive():
                thread.join(5)
        self.assertTrue(finished.is_set())
        self.assert_pass(reports[0])
        self.assertEqual(reports[0]['active'], self.b['archive_sha256'])

    def test_replaced_lock_path_is_rejected_after_acquisition(self):
        self.assert_pass(self.run_action('install', self.a))
        before, payload = self.state_bytes(), self.active_payload()
        real_flock, changed = fcntl.flock, False
        def replacing(fd, operation):
            nonlocal changed
            result = real_flock(fd, operation)
            if operation == fcntl.LOCK_EX and not changed:
                changed = True
                (self.store / '.lock').rename(self.store / 'retired-lock')
                (self.store / '.lock').write_bytes(b'')
                (self.store / '.lock').chmod(0o600)
            return result
        with mock.patch.object(SLOTS.fcntl, 'flock', side_effect=replacing):
            report = self.run_action('status')
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertIn('lock was replaced', report['error'])
        self.assertEqual(self.state_bytes(), before)
        self.assertEqual(self.active_payload(), payload)

    def test_replaced_store_directory_is_rejected_without_adopting_its_replacement(self):
        self.assert_pass(self.run_action('install', self.a))
        payload = self.active_payload()
        displaced = self.root / 'displaced original store'
        real_flock, changed = fcntl.flock, False
        def replacing(fd, operation):
            nonlocal changed
            result = real_flock(fd, operation)
            if operation == fcntl.LOCK_EX and not changed:
                changed = True
                self.store.rename(displaced)
                self.store.mkdir(mode=0o700)
                (self.store / '.lock').write_bytes(b'')
                (self.store / '.lock').chmod(0o600)
            return result
        with mock.patch.object(SLOTS.fcntl, 'flock', side_effect=replacing):
            report = self.run_action('status')
        self.assertTrue(report['status'].startswith('FAIL_'), report)
        self.assertIn('store directory was replaced', report['error'])
        self.assertEqual(set(p.name for p in self.store.iterdir()), {'.lock'})
        self.assertEqual((displaced / 'slots' / self.a['archive_sha256'] / 'bin/ffmpeg').read_bytes(), payload)

    def test_declared_TAR_size_and_member_limits_fail_before_publication(self):
        self.assert_pass(self.run_action('install', self.a))
        before, payload = self.state_bytes(), self.active_payload()
        large = self.root / 'declared-large.tar'
        member = tarfile.TarInfo('large')
        member.size = SLOTS.MAX_ARCHIVE_BYTES + 1
        large.write_bytes(member.tobuf(format=tarfile.USTAR_FORMAT) + b'\0' * 1024)
        many = self.root / 'many-members.tar'
        with tarfile.open(many, 'w') as tar:
            for index in range(SLOTS.MAX_MEMBERS + 1):
                member = tarfile.TarInfo('directory-' + str(index))
                member.type = tarfile.DIRTYPE
                tar.addfile(member)
        for archive in (large, many):
            result = self.run_action('update', dict(self.b, archive=archive,
                                                    archive_sha256=digest(archive)))
            self.assertTrue(result['status'].startswith('FAIL_'), result)
            self.assertEqual(self.state_bytes(), before)
            self.assertEqual(self.active_payload(), payload)


if __name__ == '__main__':
    unittest.main()
