#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""File-preflight controls; synthetic ELF headers are never executed or loaded."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('runtime_preflight', str(
    ROOT / 'scripts/native-runtime-preflight.py'))
PREFLIGHT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREFLIGHT)


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode('utf-8')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class RuntimePreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package, self.target = self.root / 'package', self.root / 'rootfs'
        for path in ('bin', 'lib', 'metadata', 'licenses'):
            (self.package / path).mkdir(parents=True)
        (self.target / 'usr/lib').mkdir(parents=True)
        (self.target / 'usr/bin').mkdir()
        header = bytearray(64)
        header[:7] = b'\x7fELF\x02\x01\x01'
        header[18:20] = (62).to_bytes(2, 'little')
        self.elf = bytes(header) + b'inert fixture; not a loadable program\n'
        sonames, nodes = {}, {}
        for kind in PREFLIGHT.KINDS:
            soname = 'lib' + kind + '.so.1'
            name = 'lib/' + soname + '.0.0'
            path = self.package / name
            path.write_bytes(self.elf + kind.encode('ascii'))
            path.chmod(0o755)
            (self.package / 'lib' / soname).symlink_to(path.name)
            sonames[kind] = soname
            nodes['payload:' + name] = self.node(path, name, 'payload')
        for name in ('bin/ffmpeg', 'bin/ffprobe'):
            path = self.package / name
            path.write_bytes(self.elf + name.encode('ascii'))
            path.chmod(0o755)
            nodes['payload:' + name] = self.node(path, name, 'payload')
        self.provider = self.target / 'usr/lib/libc.so.6'
        self.provider.write_bytes(self.elf + b'provider')
        nodes['platform:lib/libc.so.6'] = self.node(self.provider, 'lib/libc.so.6', 'platform')
        self.dependencies = {'schema': 1, 'architecture': ['ELF64', 'Advanced Micro Devices X86-64'],
                             'sonames': sonames, 'nodes': nodes}
        self.provenance = {'schema': 1, 'disposition': 'PUBLIC_NOT_READY'}
        self.reseal()

    def node(self, path, name, origin):
        return {'file': name, 'origin': origin, 'class': 'ELF64',
                'machine': 'Advanced Micro Devices X86-64',
                'soname': [Path(name).name] if origin == 'platform' else [],
                'bytes': path.stat().st_size, 'sha256': digest(path)}

    def reseal(self):
        for name, value in (('dependencies', self.dependencies), ('provenance', self.provenance)):
            path = self.package / 'metadata' / (name + '.json')
            path.write_bytes(encoded(value))
            path.chmod(0o644)
        self.files = []
        for path in sorted(self.package.rglob('*')):
            if path.is_dir() or path == self.package / 'metadata/files.json':
                continue
            name = path.relative_to(self.package).as_posix()
            if path.is_symlink():
                self.files.append({'path': name, 'kind': 'symlink', 'target': os.readlink(str(path))})
            else:
                self.files.append({'path': name, 'kind': 'file', 'bytes': path.stat().st_size,
                                   'mode': path.stat().st_mode & 0o7777, 'sha256': digest(path)})
        self.write_files()

    def write_files(self):
        path = self.package / 'metadata/files.json'
        path.write_bytes(encoded(self.files))
        path.chmod(0o644)
        self.pins = [digest(self.package / 'metadata' / (name + '.json'))
                     for name in ('files', 'dependencies', 'provenance')]

    def audit(self):
        return PREFLIGHT.audit(self.package, self.target, *self.pins,
                               rootfs_identity='synthetic rootfs fixture')

    def test_files_pass_retains_unrun_runtime_and_public_disposition(self):
        report = self.audit()
        self.assertEqual(report['status'], 'PASS_PINNED_TARGET_FILES_ONLY')
        self.assertEqual(report['payload_ELFs'], 9)
        self.assertEqual(report['target_providers'][0]['sha256'], digest(self.provider))
        self.assertEqual(report['clean_OS_Kodi_GPU_hardware_media_AV_acceptance'], 'NOT_RUN')
        self.assertEqual(report['public_distribution_status'], 'PUBLIC_NOT_READY')
        self.assertFalse(report['rootfs_identity_authenticated_by_this_tool'])

    def test_absolute_rootfs_link_resolves_inside_target(self):
        directory = self.target / 'usr/lib'
        directory.rename(self.target / 'actual-libraries')
        directory.symlink_to('/actual-libraries')
        self.assertEqual(self.audit()['payload_ELFs'], 9)

    def test_platform_files_subtree_is_explicitly_supported(self):
        report = PREFLIGHT.audit(self.package, self.target / 'usr', *self.pins,
                                 rootfs_identity='synthetic Platform files fixture',
                                 platform_relative='.')
        self.assertEqual(report['target_providers'][0]['path'], 'lib/libc.so.6')

    def test_changed_provider_rejected_without_host_fallback(self):
        self.provider.write_bytes(self.elf + b'changed provider')
        with self.assertRaisesRegex(ValueError, 'dependency bytes differ'):
            self.audit()

    def test_missing_provider_rejected(self):
        self.provider.unlink()
        with self.assertRaises(FileNotFoundError):
            self.audit()

    def test_provider_alias_must_select_recorded_real_file(self):
        actual = self.target / 'usr/lib/libc-actual.so'
        self.provider.rename(actual)
        self.provider.symlink_to(actual.name)
        node = self.dependencies['nodes'].pop('platform:lib/libc.so.6')
        node['file'] = 'lib/libc-actual.so'
        self.dependencies['nodes']['platform:lib/libc-actual.so'] = node
        self.reseal()
        self.assertEqual(self.audit()['target_providers'][0]['path'], 'usr/lib/libc-actual.so')
        self.provider.unlink()
        self.provider.write_bytes(self.elf + b'different selected provider')
        with self.assertRaisesRegex(ValueError, 'SONAME aliases missing, changed or ambiguous'):
            self.audit()

    def test_ambiguous_provider_alias_and_changed_interpreter_rejected(self):
        duplicate = self.target / 'usr/lib64/libc.so.6'
        duplicate.parent.mkdir()
        duplicate.write_bytes(self.provider.read_bytes())
        with self.assertRaisesRegex(ValueError, 'SONAME aliases missing, changed or ambiguous'):
            self.audit()
        duplicate.unlink()
        self.dependencies['nodes']['payload:bin/ffmpeg']['interpreter'] = '/lib64/libc.so.6'
        self.reseal()
        (self.target / 'lib64').symlink_to('/usr/lib')
        self.assertEqual(self.audit()['payload_ELFs'], 9)
        (self.target / 'lib64').unlink()
        (self.target / 'lib64').symlink_to('/usr/lib64')
        with self.assertRaises(FileNotFoundError):
            self.audit()

    def test_elf_class_cannot_be_supplied_by_metadata_only(self):
        binary = self.package / 'bin/ffmpeg'
        binary.write_bytes(b'not an ELF despite claimed metadata')
        self.dependencies['nodes']['payload:bin/ffmpeg'] = self.node(binary, 'bin/ffmpeg', 'payload')
        self.reseal()
        with self.assertRaisesRegex(ValueError, 'not little-endian x86_64 ELF64'):
            self.audit()

    def test_unreviewed_manifest_pin_rejected(self):
        self.pins[0] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'reviewed metadata SHA-256 differs'):
            self.audit()

    def test_changed_package_bytes_and_mode_rejected(self):
        binary = self.package / 'bin/ffmpeg'
        binary.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'file/hash/mode differs'):
            self.audit()

    def test_extra_package_member_rejected(self):
        (self.package / 'unexpected-override').write_text('unexpected')
        with self.assertRaisesRegex(ValueError, 'unmanifested or missing'):
            self.audit()

    def test_duplicate_and_parent_manifest_paths_rejected(self):
        original = list(self.files)
        self.files.append(dict(self.files[0]))
        self.write_files()
        with self.assertRaisesRegex(ValueError, 'duplicate/self'):
            self.audit()
        self.files = original + [{'path': '../outside', 'kind': 'file'}]
        self.write_files()
        with self.assertRaisesRegex(ValueError, 'canonical and relative'):
            self.audit()

    def test_package_link_cannot_escape_or_hide_directory_members(self):
        link = self.package / 'lib/libavutil.so.1'
        link.unlink()
        link.symlink_to('../../outside')
        self.reseal()
        with self.assertRaisesRegex(ValueError, 'one relative filename'):
            self.audit()
        link.unlink()
        (self.package / 'hidden-directory').symlink_to(self.target, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'directory symlink'):
            self.audit()

    def test_target_traversal_link_and_cycle_rejected(self):
        directory = self.target / 'usr/lib'
        shutil.rmtree(str(directory))
        directory.symlink_to('../../../outside')
        with self.assertRaisesRegex(ValueError, 'escapes target root'):
            self.audit()
        directory.unlink()
        directory.symlink_to('lib')
        with self.assertRaisesRegex(ValueError, 'symlink cycle'):
            self.audit()

    def test_compiler_and_cross_compiler_variants_rejected(self):
        compiler = self.target / 'usr/bin/x86_64-linux-gnu-gcc-14'
        compiler.write_text('inert compiler name')
        with self.assertRaisesRegex(ValueError, 'compiler/build tools found'):
            self.audit()

    def test_fifo_is_rejected_without_reading_it(self):
        self.provider.unlink()
        os.mkfifo(str(self.provider))
        with self.assertRaisesRegex(ValueError, 'bounded regular file'):
            self.audit()

    def test_public_ready_marker_and_other_architecture_rejected(self):
        self.provenance['disposition'] = 'PUBLIC_READY'
        self.reseal()
        with self.assertRaisesRegex(ValueError, 'research provenance'):
            self.audit()
        self.provenance['disposition'] = 'PUBLIC_NOT_READY'
        self.dependencies['architecture'][0] = 'ELF32'
        self.reseal()
        with self.assertRaisesRegex(ValueError, 'unsupported dependency ABI'):
            self.audit()


if __name__ == '__main__':
    unittest.main()
