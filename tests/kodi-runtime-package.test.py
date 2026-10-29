#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real ELF/archive tests; selected fixture binaries are never executed."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'tools' / 'kodi-route-probe' / 'runtime-package.py'
spec = importlib.util.spec_from_file_location('runtime_package', TOOL)
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def run(argv):
    result = subprocess.run(argv, check=False, text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='crystalhd-runtime-package-test-')
        cls.root = Path(cls.temporary.name)
        cls.prefix = cls.root / 'input prefix'
        cls.platform = cls.root / 'platform'
        cls.source = cls.root / 'source'
        for path in (cls.prefix / 'lib', cls.prefix / 'bin', cls.platform / 'lib',
                     cls.platform / 'lib64', cls.source / 'libavcodec'):
            path.mkdir(parents=True)
        cls.external_source = cls.root / 'external.c'
        cls.external_source.write_text('int fixture_required(void) { return 7; }\n')
        cls.version_script = cls.root / 'versions.map'
        cls.version_script.write_text('FIXTURE_1 { global: fixture_required; local: *; };\n')
        cls.build_external()
        for index, kind in enumerate(package.KINDS, 50):
            soname = 'lib' + kind + '.so.' + str(index)
            real = cls.prefix / 'lib' / (soname + '.0.1')
            source = cls.root / (kind + '.c')
            source.write_text('extern int fixture_required(void);\n'
                              'int ' + kind + '_version(void) { return fixture_required(); }\n')
            run(['cc', '-nostdlib', '-shared', '-fPIC', str(source),
                 '-L' + str(cls.platform / 'lib'), '-l:libfixture.so.1',
                 '-Wl,-soname,' + soname, '-o', str(real)])
            (cls.prefix / 'lib' / soname).symlink_to(real.name)
        program = cls.root / 'program.c'
        program.write_text('extern int avcodec_version(void);\n'
                           'void _start(void) { (void)avcodec_version(); }\n')
        codec = 'libavcodec.so.51'
        for name in ('ffmpeg', 'ffprobe'):
            run(['cc', '-nostdlib', '-no-pie', str(program),
                 '-L' + str(cls.prefix / 'lib'), '-l:' + codec,
                 '-Wl,-rpath-link,' + str(cls.platform / 'lib'),
                 '-Wl,-e,_start', '-o', str(cls.prefix / 'bin' / name)])
        interpreter = package.elf(cls.prefix / 'bin/ffmpeg')[0]['interpreter']
        # Only read/copy the compiler's ELF interpreter; never run the fixtures.
        shutil.copyfile(Path(interpreter).resolve(), cls.platform / 'lib64' / Path(interpreter).name)
        (cls.source / 'LICENSE.md').write_text('Fixture source licence inventory.\n')
        (cls.source / 'COPYING.LGPLv2.1').write_text('Fixture only; not a FFmpeg binary.\n')
        for name in package.COMPONENT_NOTICES:
            (cls.source / name).write_text('/* Fixture component source notice. */\n')
        run(['git', 'init', '-q', str(cls.source)])
        run(['git', '-C', str(cls.source), 'add', '.'])
        run(['git', '-C', str(cls.source), '-c', 'user.name=Fixture',
             '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture'])
        cls.revision = run(['git', '-C', str(cls.source), 'rev-parse', 'HEAD']).strip()

    @classmethod
    def build_external(cls):
        run(['cc', '-nostdlib', '-shared', '-fPIC', str(cls.external_source),
             '-Wl,-soname,libfixture.so.1', '-Wl,--version-script,' + str(cls.version_script),
             '-o', str(cls.platform / 'lib/libfixture.so.1')])

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def output(self, name):
        return self.root / (self.id().split('.')[-1] + '-' + name)

    def build(self, name, prefix=None):
        args = argparse.Namespace(prefix=prefix or self.prefix, platform_root=self.platform,
                                  source_checkout=self.source, source_revision=self.revision,
                                  output=self.output(name), epoch=1700000000)
        result = package.pack(args)
        return args.output, result

    def verify(self, root):
        return package.verify(argparse.Namespace(package=root, platform_root=self.platform))

    def test_reproducible_archive_and_relocated_audit(self):
        first, a = self.build('a')
        # File/parent timestamps and source-prefix location are deliberately different.
        copy = self.output('relocated input')
        shutil.copytree(self.prefix, copy, symlinks=True)
        for path in copy.rglob('*'):
            if path.is_file() and not path.is_symlink():
                os.utime(path, (1701000000, 1701000000))
        second, b = self.build('b', copy)
        self.assertEqual(a['archive_sha256'], b['archive_sha256'])
        self.assertEqual((first / 'runtime-private.tar').read_bytes(),
                         (second / 'runtime-private.tar').read_bytes())
        target = self.output('fresh target with spaces')
        target.mkdir()
        with tarfile.open(first / 'runtime-private.tar') as archive:
            archive.extractall(target, filter='data')
        self.assertEqual(self.verify(target)['status'], 'PASS_PRIVATE_PACKAGE_AUDIT')
        self.assertEqual(a['disposition'], 'PUBLIC_NOT_READY')
        self.assertFalse((target / 'bin/cc').exists())

    def test_missing_payload_does_not_fall_back(self):
        copy = self.output('missing')
        shutil.copytree(self.prefix, copy, symlinks=True)
        (copy / 'lib/libavutil.so.50.0.1').unlink()
        with self.assertRaisesRegex(RuntimeError, 'expected one real libavutil'):
            self.build('bad', copy)

    def test_wrong_soname_link(self):
        copy = self.output('wrong link')
        shutil.copytree(self.prefix, copy, symlinks=True)
        link = copy / 'lib/libavcodec.so.51'
        link.unlink()
        link.symlink_to('libavutil.so.50.0.1')
        with self.assertRaisesRegex(RuntimeError, 'SONAME link must point relatively'):
            self.build('bad', copy)

    def test_missing_external_provider_does_not_use_host(self):
        path = self.platform / 'lib/libfixture.so.1'
        saved = path.read_bytes()
        path.unlink()
        try:
            with self.assertRaisesRegex(RuntimeError, 'missing or ambiguous target provider'):
                self.build('bad')
        finally:
            path.write_bytes(saved)

    def test_external_same_soname_wrong_version(self):
        path = self.platform / 'lib/libfixture.so.1'
        saved = path.read_bytes()
        self.version_script.write_text('FIXTURE_2 { global: fixture_required; local: *; };\n')
        self.build_external()
        try:
            with self.assertRaisesRegex(RuntimeError, 'target version missing'):
                self.build('bad')
        finally:
            path.write_bytes(saved)
            self.version_script.write_text('FIXTURE_1 { global: fixture_required; local: *; };\n')

    def test_external_same_version_missing_actual_symbol(self):
        path = self.platform / 'lib/libfixture.so.1'
        saved = path.read_bytes()
        self.external_source.write_text('int fixture_other(void) { return 9; }\n')
        self.version_script.write_text('FIXTURE_1 { global: fixture_other; local: *; };\n')
        self.build_external()
        try:
            with self.assertRaisesRegex(RuntimeError, 'target symbol missing: fixture_required'):
                self.build('bad')
        finally:
            path.write_bytes(saved)
            self.external_source.write_text('int fixture_required(void) { return 7; }\n')
            self.version_script.write_text('FIXTURE_1 { global: fixture_required; local: *; };\n')

    def test_file_hash_drift(self):
        output, _ = self.build('good')
        path = output / 'stage/bin/ffprobe'
        path.write_bytes(path.read_bytes() + b'changed')
        with self.assertRaisesRegex(RuntimeError, 'package file/hash/link manifest mismatch'):
            self.verify(output / 'stage')

    def test_executable_mode_drift(self):
        output, _ = self.build('good')
        (output / 'stage/bin/ffmpeg').chmod(0o644)
        with self.assertRaisesRegex(RuntimeError, 'package file/hash/link manifest mismatch'):
            self.verify(output / 'stage')

    def test_changed_pinned_target_inventory(self):
        output, _ = self.build('good')
        path = self.platform / 'lib/libfixture.so.1'
        saved = path.read_bytes()
        path.write_bytes(saved + b'harmless-identification-change')
        try:
            with self.assertRaisesRegex(RuntimeError, 'target dependency inventory changed'):
                self.verify(output / 'stage')
        finally:
            path.write_bytes(saved)

    def test_changed_source_notice_is_not_silently_packaged(self):
        path = self.source / 'LICENSE.md'
        saved = path.read_bytes()
        path.write_bytes(saved + b'changed\n')
        try:
            with self.assertRaisesRegex(RuntimeError, 'git failed'):
                self.build('bad')
        finally:
            path.write_bytes(saved)

    def test_payload_rpath_is_rejected(self):
        path = self.prefix / 'lib/libavcodec.so.51.0.1'
        saved = path.read_bytes()
        run(['cc', '-nostdlib', '-shared', '-fPIC', str(self.root / 'avcodec.c'),
             '-L' + str(self.platform / 'lib'), '-l:libfixture.so.1',
             '-Wl,-soname,libavcodec.so.51', '-Wl,-rpath,/not-the-target-runtime',
             '-o', str(path)])
        try:
            with self.assertRaisesRegex(RuntimeError, 'payload RPATH/RUNPATH is not supported'):
                self.build('bad')
        finally:
            path.write_bytes(saved)

    def test_nonallowlisted_files_are_not_copied(self):
        extra = self.prefix / 'bin/cc'
        extra.write_text('not a payload component\n')
        try:
            output, _ = self.build('good')
            with tarfile.open(output / 'runtime-private.tar') as archive:
                self.assertNotIn('bin/cc', archive.getnames())
            self.assertFalse((output / 'stage/bin/cc').exists())
        finally:
            extra.unlink()


if __name__ == '__main__':
    unittest.main()
