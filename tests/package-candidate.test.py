#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exact-commit, Git-free archive and deterministic private-package contracts."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('candidate_package', ROOT / 'scripts/package-candidate.py')
PACKAGE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(PACKAGE)
spec = importlib.util.spec_from_file_location('kodi_package', ROOT / 'scripts/package-kodi-settings.py')
ZIP = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ZIP)


class CandidateTests(unittest.TestCase):
    def git(self, *arguments):
        return subprocess.check_output(['git', '-c', 'user.name=Package test', '-c',
            'user.email=test@example.invalid', '-c', 'commit.gpgsign=false', '-c',
            'core.hooksPath=/dev/null', '-C', str(self.source), *arguments], stderr=subprocess.PIPE)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='crystalhd-package-test-')
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        files = [ZIP.LICENSE, 'scripts/package-kodi-settings.py', 'scripts/package-candidate.py']
        files += ['addons/' + ZIP.ADDON + '/' + name for name in ZIP.FILES]
        for name in files:
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        (self.source / 'VERSION').write_text('1.3.0-dev.1\n')
        (self.source / 'SOURCE_REVISION').write_text('$Format:%H$\n')
        (self.source / '.gitattributes').write_text('SOURCE_REVISION export-subst\n')
        self.git('init', '-q')
        self.git('add', '.')
        self.git('commit', '-qm', 'private package fixture')
        self.commit = self.git('rev-parse', 'HEAD').decode().strip()

    def tearDown(self):
        self.temp.cleanup()

    def test_two_destinations_match_and_archive_retains_exact_source(self):
        (self.source / 'private-untracked.txt').write_text('must not enter candidate')
        first, second = self.root / 'first', self.root / 'second'
        manifest = PACKAGE.package(self.source, first)
        self.assertEqual(manifest, PACKAGE.package(self.source, second))
        self.assertEqual(manifest['source_commit'], self.commit)
        self.assertEqual(manifest['publication'], 'PRIVATE_CANDIDATE_NOT_RELEASED')
        self.assertEqual(manifest['qualification'], 'NOT_RUN_FOR_THESE_ARTIFACTS')
        self.assertEqual({p.name for p in first.iterdir()}, {p.name for p in second.iterdir()})
        for path in first.iterdir():
            self.assertEqual(path.read_bytes(), (second / path.name).read_bytes())
        with tarfile.open(next(first.glob('*.tar.gz')), 'r:gz') as archive:
            names = archive.getnames()
            self.assertFalse(any('/.git/' in name or name.endswith('/private-untracked.txt') for name in names))
            revision = next(name for name in names if name.endswith('/SOURCE_REVISION'))
            self.assertEqual(archive.extractfile(revision).read().decode().strip(), self.commit)
        for artifact in manifest['artifacts']:
            path = first / artifact['name']
            self.assertEqual(path.stat().st_size, artifact['bytes'])
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), artifact['sha256'])
        with zipfile.ZipFile(next(first.glob('*.zip'))) as archive:
            self.assertIsNone(archive.testzip())

    def test_dirty_or_non_head_source_and_existing_output_are_rejected(self):
        (self.source / 'VERSION').write_text('1.3.0-dev.2\n')
        with self.assertRaisesRegex(ValueError, 'commit tracked changes'):
            PACKAGE.package(self.source, self.root / 'dirty')
        self.assertFalse((self.root / 'dirty').exists())
        self.git('add', 'VERSION')
        self.git('commit', '-qm', 'second fixture identity')
        with self.assertRaisesRegex(ValueError, 'checkout HEAD'):
            PACKAGE.package(self.source, self.root / 'old', self.commit)
        output = self.root / 'existing'
        output.mkdir()
        (output / 'keep').write_text('qualified payload')
        with self.assertRaises(FileExistsError):
            PACKAGE.package(self.source, output)
        self.assertEqual((output / 'keep').read_text(), 'qualified payload')


if __name__ == '__main__':
    unittest.main(verbosity=2)
