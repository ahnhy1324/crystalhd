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

    def test_minimal_older_manifest_keeps_distribution_and_abi_unknown(self):
        report = self.audit()
        abi = report['recorded_ABI_diagnostics']
        self.assertEqual(report['status'], 'PASS_PINNED_TARGET_FILES_ONLY')
        self.assertEqual(abi['payload_version_inventory'], 'UNKNOWN')
        self.assertIsNone(abi['highest_recorded_numeric_glibc_requirement'])
        self.assertIsNone(abi['recorded_configure_cpu_options'])
        self.assertEqual(len(abi['payloads_without_version_inventory']), 9)
        distribution = report['distribution_diagnostics']
        self.assertFalse(distribution['review_declarations_verified_by_this_tool'])
        self.assertEqual({x['state'] for x in distribution['recorded_blockers']}, {'UNKNOWN'})
        self.assertIn('source_revision', distribution['unknown_provenance_fields'])
        self.assertEqual(distribution['status'], 'PUBLIC_NOT_READY')

    def test_records_cpu_and_glibc_inventory_without_claiming_target_acceptance(self):
        self.provenance.update({
            'source_revision': 'a' * 40, 'source_url': 'https://example.invalid/source',
            'private_build_prefixes_detected': True, 'neutral_build_verified': False,
            'corresponding_source_archive_included': False,
            'redistribution_review': 'NOT_COMPLETED', 'update_maintenance_owner': 'UNASSIGNED',
            'clean_OS_and_Kodi_playback_qualification': 'NOT_RUN',
            'configured_feature_and_CPU_options': ['--enable-shared', '--cpu=core2']})
        for name, node in self.dependencies['nodes'].items():
            if name.startswith('payload:'):
                node['version_needs'] = {}
        self.dependencies['nodes']['payload:bin/ffmpeg']['version_needs'] = {
            'libc.so.6': ['GLIBC_2.9', 'GLIBC_2.43', 'GLIBC_2.100', 'GLIBC_ABI_DT_RELR'],
            'libavcodec.so.1': ['LIBAVCODEC_1']}
        self.reseal()
        report = self.audit()
        abi = report['recorded_ABI_diagnostics']
        self.assertEqual(abi['payload_version_inventory'], 'RECORDED')
        self.assertEqual(abi['highest_recorded_numeric_glibc_requirement'], 'GLIBC_2.100')
        self.assertEqual(abi['recorded_glibc_versions'], ['GLIBC_2.9', 'GLIBC_2.43', 'GLIBC_2.100'])
        self.assertEqual(abi['recorded_named_glibc_requirements'], ['GLIBC_ABI_DT_RELR'])
        self.assertEqual(abi['recorded_configure_cpu_options'], ['--cpu=core2'])
        self.assertEqual(abi['ffmpeg_sonames']['avcodec'], 'libavcodec.so.1')
        self.assertEqual({x['state'] for x in report['distribution_diagnostics']['recorded_blockers']},
                         {'DECLARED_UNRESOLVED'})
        self.assertEqual(report['clean_OS_Kodi_GPU_hardware_media_AV_acceptance'], 'NOT_RUN')

    def test_partial_version_inventory_and_null_provenance_stay_unknown(self):
        self.provenance['neutral_build_verified'] = None
        self.dependencies['nodes']['payload:bin/ffmpeg']['version_needs'] = {
            'libc.so.6': ['GLIBC_2.43']}
        self.reseal()
        report = self.audit()
        abi = report['recorded_ABI_diagnostics']
        self.assertEqual(abi['payload_version_inventory'], 'PARTIAL')
        self.assertEqual(len(abi['payloads_without_version_inventory']), 8)
        self.assertEqual(abi['highest_recorded_numeric_glibc_requirement'], 'GLIBC_2.43')
        self.assertIn('neutral_build_verified',
                      report['distribution_diagnostics']['unknown_provenance_fields'])
        self.assertEqual(report['public_distribution_status'], 'PUBLIC_NOT_READY')

    def test_positive_provenance_declarations_cannot_approve_distribution(self):
        self.provenance.update({
            'private_build_prefixes_detected': False, 'neutral_build_verified': True,
            'corresponding_source_archive_included': True, 'redistribution_review': 'COMPLETED',
            'update_maintenance_owner': 'declared-owner',
            'clean_OS_and_Kodi_playback_qualification': 'PASS'})
        self.reseal()
        report = self.audit()
        distribution = report['distribution_diagnostics']
        self.assertEqual({x['state'] for x in distribution['recorded_blockers']},
                         {'DECLARED_UNVERIFIED'})
        self.assertFalse(distribution['review_declarations_verified_by_this_tool'])
        self.assertEqual(distribution['provenance_records']['update_maintenance_owner'],
                         'declared-owner')
        self.assertEqual(distribution['status'], 'PUBLIC_NOT_READY')
        self.assertEqual(report['clean_OS_Kodi_GPU_hardware_media_AV_acceptance'], 'NOT_RUN')

    def test_malformed_optional_provenance_is_not_interpreted_as_clean(self):
        original = dict(self.provenance)
        for field, value in (
                ('private_build_prefixes_detected', 'false'),
                ('neutral_build_verified', 0),
                ('corresponding_source_archive_included', []),
                ('source_revision', 'not-a-commit'),
                ('source_url', []), ('redistribution_review', True),
                ('update_maintenance_owner', ''),
                ('clean_OS_and_Kodi_playback_qualification', {}),
                ('configured_feature_and_CPU_options', '--cpu=core2'),
                ('configured_feature_and_CPU_options', ['--cpu='])):
            with self.subTest(field=field, value=value):
                self.provenance = dict(original)
                self.provenance[field] = value
                self.reseal()
                with self.assertRaisesRegex(ValueError, 'invalid provenance'):
                    self.audit()

    def test_malformed_recorded_version_requirements_rejected(self):
        for value in ([], {'libc.so.6': 'GLIBC_2.43'},
                      {'libc.so.6': [43]}, {'libc.so.6': ['GLIBC_2.43 bogus']},
                      {'../libc.so.6': ['GLIBC_2.43']}):
            with self.subTest(value=value):
                self.dependencies['nodes']['payload:bin/ffmpeg']['version_needs'] = value
                self.reseal()
                with self.assertRaisesRegex(ValueError, 'invalid recorded version requirements'):
                    self.audit()


if __name__ == '__main__':
    unittest.main()
