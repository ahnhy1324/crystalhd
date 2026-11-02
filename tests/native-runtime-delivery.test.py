#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise archive/source/build binding with inert bytes; never execute payloads."""
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.util.spec_from_file_location('delivery', ROOT / 'scripts/package-native-runtime-delivery.py')
delivery = importlib.util.module_from_spec(loader)
loader.loader.exec_module(delivery)


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='crystalhd-delivery-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.spec = {'schema': 1, 'bundle_version': '1.3.0-rc.2', 'epoch': 1790018630,
                     'metadata_pins': {}, 'files': []}
        self.spec_path = self.root / 'inputs.json'
        self.payload = {}
        provenance = {'source_revision': 'a' * 40, 'disposition': 'PUBLIC_NOT_READY'}
        runtime = self.root / 'runtime.tar'
        with tarfile.open(runtime, 'w', format=tarfile.USTAR_FORMAT) as archive:
            for member, data in [('metadata/files.json', b'[]\n'),
                                 ('metadata/dependencies.json', b'{"fixture":true}\n'),
                                 ('metadata/provenance.json', json.dumps(provenance).encode())]:
                info = tarfile.TarInfo(member); info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
                self.spec['metadata_pins'][Path(member).stem] = hashlib.sha256(data).hexdigest()
            for index in range(9):
                member = 'bin/ffmpeg' if index == 0 else 'bin/ffprobe' if index == 1 else 'lib/fixture%d.so.1' % index
                data = ('inert-payload-%d' % index).encode()
                info = tarfile.TarInfo(member); info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
                self.payload[member] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        self.add(runtime, 'runtime/runtime-private.tar', 'runtime')
        source = self.root / 'source.tar'; source.write_bytes(b'complete source fixture')
        self.add(source, 'sources/ffmpeg/source-materials.tar', 'ffmpeg-source')
        self.materials = {'format': 'crystalhd-private-native-runtime-materials',
                         'source_revision': 'a' * 40, 'source_tree': 'b' * 40,
                         'binary_package_archive': {'bytes': runtime.stat().st_size, 'sha256': delivery.digest(runtime)},
                         'package_metadata_sha256': self.spec['metadata_pins'],
                         'archive': {'bytes': source.stat().st_size, 'sha256': delivery.digest(source)}}
        self.materials_path = self.root / 'materials.json'
        self.materials_path.write_text(json.dumps(self.materials))
        self.add(self.materials_path, 'sources/ffmpeg/materials-manifest.json', 'ffmpeg-source-manifest')
        self.proof = {'status': 'PASS_REPRODUCIBLE_BUILDS_AND_RETAINED_BINARY_SOURCE_CORRESPONDENCE',
                      'source_revision': 'a' * 40, 'source_tree': 'b' * 40, 'source_date_epoch': 1790018630,
                      'two_builds_exact_payload_equal': True, 'retained_neutral_exact_payload_equal': True,
                      'retained_payload': self.payload,
                      'builds': [{'name': name, 'status': 'PASS', 'payload': copy.deepcopy(self.payload)} for name in ('a', 'b')]}
        self.proof_path = self.root / 'proof.json'; self.proof_path.write_text(json.dumps(self.proof))
        self.add(self.proof_path, 'evidence/build.json', 'build-evidence')
        for role in ('dependency-source', 'host-evidence', 'tool', 'guide', 'support', 'notice'):
            path = self.root / (role + '.txt'); path.write_text(role)
            self.add(path, role + '/fixture.txt', role)

    def add(self, path, member, role):
        self.spec['files'].append({'source': str(path), 'member': member, 'role': role,
                                   'bytes': path.stat().st_size, 'sha256': delivery.digest(path)})

    def repin(self, path):
        for row in self.spec['files']:
            if row['source'] == str(path):
                row.update(bytes=path.stat().st_size, sha256=delivery.digest(path))

    def execute(self, output):
        self.spec_path.write_text(json.dumps(self.spec))
        return delivery.package(self.spec_path, delivery.digest(self.spec_path), output)

    def test_reproduces_and_preserves_complete_members(self):
        first = self.execute(self.root / 'one')
        second = self.execute(self.root / 'two')
        self.assertEqual(first, second)
        one = self.root / 'one' / first['archive']
        two = self.root / 'two' / second['archive']
        self.assertEqual(one.read_bytes(), two.read_bytes())
        with tarfile.open(one, 'r:gz') as archive:
            manifest = archive.extractfile('delivery-manifest.json').read()
            self.assertNotIn(str(self.root).encode(), manifest)
            for row in self.spec['files']:
                self.assertEqual(hashlib.sha256(archive.extractfile(row['member']).read()).hexdigest(), row['sha256'])
        self.assertEqual(delivery.digest(self.root / 'runtime.tar'), self.spec['files'][0]['sha256'])

    def test_declared_success_cannot_hide_different_second_build(self):
        self.proof['builds'][1]['payload']['bin/ffmpeg']['sha256'] = 'f' * 64
        self.proof_path.write_text(json.dumps(self.proof)); self.repin(self.proof_path)
        with self.assertRaisesRegex(ValueError, 'independent build payload differs'):
            self.execute(self.root / 'rejected')
        self.assertFalse((self.root / 'rejected').exists())

    def test_input_relocation_and_order_do_not_change_archive(self):
        first = self.execute(self.root / 'original-output')
        relocated = self.root / 'relocated'; relocated.mkdir()
        for index, row in enumerate(self.spec['files']):
            target = relocated / str(index)
            shutil.copyfile(row['source'], target); row['source'] = str(target)
        self.spec['files'].reverse()
        second = self.execute(self.root / 'relocated-output')
        self.assertNotEqual(first['spec_sha256'], second['spec_sha256'])
        self.assertEqual(first['sha256'], second['sha256'])
        self.assertEqual((self.root / 'original-output' / first['archive']).read_bytes(),
                         (self.root / 'relocated-output' / second['archive']).read_bytes())

    def test_reserved_directory_is_rejected_before_output(self):
        self.spec['files'][-1]['member'] = 'SHA256SUMS/fixture.txt'
        with self.assertRaisesRegex(ValueError, 'reserved delivery member'):
            self.execute(self.root / 'rejected')
        self.assertFalse((self.root / 'rejected').exists())

    def test_source_companion_must_match_even_with_valid_input_hash(self):
        self.materials['source_revision'] = 'c' * 40
        self.materials_path.write_text(json.dumps(self.materials)); self.repin(self.materials_path)
        with self.assertRaisesRegex(ValueError, 'source companion does not match'):
            self.execute(self.root / 'rejected')
        self.assertFalse((self.root / 'rejected').exists())

    def test_missing_or_changed_input_never_creates_output(self):
        guide = next(Path(row['source']) for row in self.spec['files'] if row['role'] == 'guide')
        guide.write_text('changed after review')
        with self.assertRaisesRegex(ValueError, 'input size/SHA-256 mismatch'):
            self.execute(self.root / 'rejected')
        self.assertFalse((self.root / 'rejected').exists())
        guide.unlink()
        with self.assertRaisesRegex(ValueError, 'regular absolute file'):
            self.execute(self.root / 'rejected')

    def test_existing_output_is_preserved(self):
        target = self.root / 'existing'; target.mkdir()
        sentinel = target / 'user-file'; sentinel.write_text('preserve')
        with self.assertRaisesRegex(ValueError, 'output must be new'):
            self.execute(target)
        self.assertEqual(sentinel.read_text(), 'preserve')


if __name__ == '__main__':
    unittest.main()
