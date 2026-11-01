#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Private companion controls using small inert local Git fixtures only."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('runtime_materials', ROOT / 'scripts/native-runtime-materials.py')
MATERIALS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MATERIALS)


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class RuntimeMaterialsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / 'src').mkdir()
        (self.source / 'LICENSE.md').write_bytes(b'Fixture license summary\n')
        (self.source / 'COPYING.LGPLv2.1').write_bytes(b'Fixture license text\n')
        self.component = b'/* fixture component notice */\nint inert;\n'
        (self.source / 'src/component.c').write_bytes(self.component)
        (self.source / '.gitignore').write_text('ignored-private.txt\n')
        (self.source / 'configure').write_text('#!/bin/sh\nexit 99\n')
        (self.source / 'configure').chmod(0o755)
        self.git('init', '--quiet')
        self.commit()
        self.package = self.root / 'package'
        (self.package / 'metadata').mkdir(parents=True)
        (self.package / 'licenses').mkdir()
        (self.package / 'bin').mkdir()
        (self.package / 'bin/ffmpeg').write_bytes(b'inert fixture, never executable\n')
        self.archive = self.root / 'binary-package.tar'
        # This tool binds reviewed archive identity, without content/runtime claims.
        self.archive.write_bytes(b'inert archive identity fixture\n')
        self.provenance = {'schema': 1, 'source_revision': self.revision,
                           'source_tracked_changes': False, 'disposition': 'PUBLIC_NOT_READY',
                           'neutral_build_verified': False,
                           'corresponding_source_archive_included': False, 'notices': []}
        for name in ('LICENSE.md', 'COPYING.LGPLv2.1'):
            raw = (self.source / name).read_bytes()
            (self.package / 'licenses' / name).write_bytes(raw)
            self.provenance['notices'].append({'source': name, 'url': 'https://example.invalid/' + name,
                                                'sha256': hashlib.sha256(raw).hexdigest()})
        initial = self.component[:self.component.index(b'*/') + 2] + b'\n'
        (self.package / 'licenses/component.c.notice').write_bytes(initial)
        self.provenance['notices'].append({'source': 'src/component.c',
            'url': 'https://example.invalid/component.c',
            'source_sha256': hashlib.sha256(self.component).hexdigest(),
            'notice_sha256': hashlib.sha256(initial).hexdigest()})
        (self.package / 'licenses/NOTICE').write_bytes(b'PRIVATE RESEARCH PACKAGE - PUBLIC_NOT_READY\n')
        self.dependencies = {'schema': 1, 'nodes': {}}
        self.reseal()
        self.material_root = self.root / 'reviewed'
        self.material_root.mkdir()
        self.recipe = self.material_root / 'recipe.sh'
        self.recipe.write_text('#!/bin/sh\ntouch ' + str(self.root / 'MUST_NOT_RUN') + '\nexit 99\n')
        self.config = self.material_root / 'config.mak'
        self.config.write_text('prefix=/usr\nCC_IDENT=historical fixture compiler\n')
        self.spec_value = {'schema': 1, 'materials': [
            {'path': str(self.recipe), 'archive_path': 'materials/recipe.sh',
             'sha256': digest(self.recipe), 'role': 'recipe'},
            {'path': str(self.config), 'archive_path': 'materials/config.mak',
             'sha256': digest(self.config), 'role': 'configuration'}]}
        self.spec_path = self.root / 'materials.json'
        self.write_spec()
        self.output = self.root / 'output'

    def git(self, *args):
        env = MATERIALS.git_env()
        env.update(GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                   GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid',
                   GIT_AUTHOR_DATE='2020-01-01T00:00:00+00:00', GIT_COMMITTER_DATE='2020-01-01T00:00:00+00:00')
        return subprocess.check_output(['git', '-C', str(self.source), *args], env=env, stderr=subprocess.PIPE)

    def commit(self):
        self.git('add', '--all')
        self.git('commit', '--quiet', '-m', 'inert fixture')
        self.revision = self.git('rev-parse', 'HEAD').decode().strip()

    def reseal(self):
        for name, value in (('dependencies', self.dependencies), ('provenance', self.provenance)):
            (self.package / 'metadata' / (name + '.json')).write_bytes(encoded(value))
        entries = []
        for path in sorted(self.package.rglob('*')):
            if path.is_dir() or path.name == 'files.json':
                continue
            entries.append({'path': path.relative_to(self.package).as_posix(), 'kind': 'file',
                            'mode': 0o644, 'bytes': path.stat().st_size, 'sha256': digest(path)})
        (self.package / 'metadata/files.json').write_bytes(encoded(entries))

    def write_spec(self, raw=None):
        self.spec_path.write_bytes(encoded(self.spec_value) if raw is None else raw)

    def arguments(self):
        return {'source_checkout': self.source, 'source_revision': self.revision,
                'package_root': self.package, 'package_archive': self.archive,
                'expected_archive_sha256': digest(self.archive),
                'expected_files_sha256': digest(self.package / 'metadata/files.json'),
                'expected_dependencies_sha256': digest(self.package / 'metadata/dependencies.json'),
                'expected_provenance_sha256': digest(self.package / 'metadata/provenance.json'),
                'materials_spec': self.spec_path, 'expected_materials_spec_sha256': digest(self.spec_path),
                'output_dir': self.output}

    def export(self, **overrides):
        arguments = self.arguments()
        arguments.update(overrides)
        return MATERIALS.export(**arguments)

    def rejected(self, pattern, **overrides):
        with self.assertRaisesRegex(ValueError, pattern):
            self.export(**overrides)
        self.assertFalse(self.output.exists())

    def test_export_copies_exact_sources_notices_and_keeps_qualifications_unverified(self):
        frozen = (self.package / 'metadata/provenance.json').read_bytes()
        result = self.export()
        self.assertEqual(result['source_revision'], self.revision)
        self.assertEqual(result['binary_package_archive']['sha256'], digest(self.archive))
        self.assertEqual(result['public_distribution_status'], 'PUBLIC_NOT_READY')
        self.assertEqual(result['source_correspondence_to_binary'], 'UNVERIFIED')
        self.assertEqual(result['reproducible_compilation'], 'NOT_VERIFIED')
        self.assertEqual(result['historical_recipe_execution'], 'NOT_RUN')
        self.assertEqual(result['dependency_source_and_notice_coverage'], 'NOT_REVIEWED')
        self.assertEqual(frozen, (self.package / 'metadata/provenance.json').read_bytes())
        with tarfile.open(self.output / 'source-materials.tar', 'r:') as archive:
            members = archive.getmembers()
            self.assertTrue(all(member.isfile() and member.uid == member.gid == member.mtime == 0
                                and not member.uname and not member.gname for member in members))
            self.assertEqual(archive.extractfile('source/src/component.c').read(), self.component)
            self.assertEqual(archive.getmember('source/configure').mode, 0o755)
            self.assertEqual(archive.extractfile('package-metadata/provenance.json').read(), frozen)
            self.assertEqual(archive.extractfile('materials/recipe.sh').read(), self.recipe.read_bytes())
            self.assertFalse(any('.git/' in member.name or member.name.startswith('bin/') for member in members))
        self.assertFalse((self.root / 'MUST_NOT_RUN').exists())

    def test_relocation_mtimes_and_spec_order_do_not_change_artifacts(self):
        self.export()
        alternate = self.root / 'alternate'
        alternate.mkdir()
        clone = alternate / 'source'
        shutil.copytree(self.source, clone)
        material_clone = alternate / 'reviewed'
        shutil.copytree(self.material_root, material_clone)
        for item in self.spec_value['materials']:
            item['path'] = str(material_clone / Path(item['path']).name)
            os.utime(item['path'], (100, 100))
        self.spec_value['materials'].reverse()
        self.write_spec()
        other = alternate / 'output'
        self.export(source_checkout=clone, output_dir=other)
        for name in MATERIALS.OUTPUTS:
            self.assertEqual((self.output / name).read_bytes(), (other / name).read_bytes(), name)

    def test_each_independent_review_pin_is_required(self):
        for name in ('archive', 'files', 'dependencies', 'provenance', 'materials_spec'):
            with self.subTest(name=name):
                self.rejected('SHA-256 mismatch', **{'expected_' + name + '_sha256': '0' * 64})

    def test_source_revision_must_be_exact_full_clean_head(self):
        self.rejected('full SHA-1', source_revision=self.revision[:12])
        self.rejected('revision mismatch', source_revision='0' * 40)

    def test_dirty_tracked_source_is_rejected(self):
        (self.source / 'src/component.c').write_bytes(b'changed\n')
        self.rejected('dirty')

    def test_untracked_source_is_rejected(self):
        (self.source / 'untracked.txt').write_text('not selected\n')
        self.rejected('untracked')

    def test_ignored_files_and_git_database_are_never_selected(self):
        (self.source / 'ignored-private.txt').write_bytes(b'ignored bytes never copied\n')
        self.export()
        with tarfile.open(self.output / 'source-materials.tar', 'r:') as archive:
            names = archive.getnames()
            self.assertNotIn('source/ignored-private.txt', names)
            self.assertFalse(any('/.git/' in name for name in names))

    def test_pinned_provenance_cannot_rebind_an_unrelated_source(self):
        self.provenance['source_revision'] = '0' * 40
        self.reseal()
        self.rejected('declared package source revision mismatch')

    def test_missing_or_false_clean_source_declaration_is_not_inferred(self):
        for value in (None, True, 0, 'false'):
            with self.subTest(value=value):
                self.provenance['source_tracked_changes'] = value
                self.reseal()
                self.rejected('tracked-change declaration')

    def test_spec_unknown_missing_and_duplicate_json_fields_are_rejected(self):
        original = encoded(self.spec_value)
        for raw in (b'{"schema":1,"schema":1,"materials":[]}',
                    b'{"schema":1,"materials":[],"scan_home":true}',
                    b'{"schema":true,"materials":[]}', b'{"schema":1}'):
            with self.subTest(raw=raw):
                self.write_spec(raw)
                self.rejected('duplicate JSON|unknown or missing|invalid materials')
        self.write_spec(original)

    def test_material_path_and_role_allowlist_rejects_malformed_or_unknown_fields(self):
        original = dict(self.spec_value['materials'][0])
        cases = [('archive_path', '../escape'), ('archive_path', '/absolute'),
                 ('archive_path', 'materials/a/../escape'), ('archive_path', 'materials/.git/config'),
                 ('archive_path', 'materials/name\nline'), ('archive_path', 'source/configure'),
                 ('archive_path', '.'), ('role', 'execute'), ('role', []), ('path', None)]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                self.spec_value['materials'][0] = {**original, key: value}
                self.write_spec()
                self.rejected('relative|canonical|role|path must')
        self.spec_value['materials'][0] = {**original, 'extra': 'unknown'}
        self.write_spec()
        self.rejected('unknown or missing material fields')

    def test_material_digest_and_nonregular_inputs_are_rejected(self):
        original = dict(self.spec_value['materials'][0])
        self.spec_value['materials'][0]['sha256'] = '0' * 64
        self.write_spec()
        self.rejected('SHA-256 mismatch')
        self.spec_value['materials'][0] = {**original, 'path': str(self.material_root)}
        self.write_spec()
        self.rejected('regular file')

    def test_symlink_leaf_or_ancestor_is_never_followed(self):
        alias = self.root / 'alias'
        alias.symlink_to(self.recipe)
        self.spec_value['materials'][0]['path'] = str(alias)
        self.write_spec()
        self.rejected('symlink')
        alias.unlink()
        alias.symlink_to(self.material_root, target_is_directory=True)
        self.spec_value['materials'][0]['path'] = str(alias / self.recipe.name)
        self.write_spec()
        self.rejected('symlink')

    def test_material_hardlinks_are_rejected_without_removing_aliases(self):
        alias = self.root / 'recipe-hardlink'
        os.link(self.recipe, alias)
        self.rejected('single-link regular file')
        self.assertEqual(alias.read_bytes(), self.recipe.read_bytes())

    def test_duplicate_and_ancestor_file_names_are_rejected(self):
        original = [dict(item) for item in self.spec_value['materials']]
        for first, second in (('materials/same', 'materials/same'),
                              ('materials/recipe', 'materials/recipe/config')):
            with self.subTest(first=first, second=second):
                self.spec_value['materials'] = [dict(item) for item in original]
                self.spec_value['materials'][0]['archive_path'] = first
                self.spec_value['materials'][1]['archive_path'] = second
                self.write_spec()
                self.rejected('duplicate|collision')

    def test_recipe_and_configuration_selection_are_both_required(self):
        self.spec_value['materials'][1]['role'] = 'recipe'
        self.write_spec()
        self.rejected('recipe and configuration')

    def test_existing_output_file_directory_or_symlink_is_untouched(self):
        self.output.write_bytes(b'preserve file')
        with self.assertRaisesRegex(ValueError, 'new directory'):
            self.export()
        self.assertEqual(self.output.read_bytes(), b'preserve file')
        self.output.unlink()
        self.output.mkdir()
        (self.output / 'unrelated').write_bytes(b'preserve member')
        with self.assertRaisesRegex(ValueError, 'new directory'):
            self.export()
        self.assertEqual((self.output / 'unrelated').read_bytes(), b'preserve member')
        (self.output / 'unrelated').unlink()
        self.output.rmdir()
        self.output.symlink_to(self.material_root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            self.export()
        self.assertTrue(self.output.is_symlink())

    def test_source_symlinks_are_rejected_even_at_the_reviewed_commit(self):
        (self.source / 'alias').symlink_to('LICENSE.md')
        self.commit()
        self.provenance['source_revision'] = self.revision
        self.reseal()
        self.rejected('source symlinks')

    def test_source_and_selected_material_totals_are_bounded(self):
        with mock.patch.object(MATERIALS, 'MAX_SOURCE_TOTAL', 1):
            self.rejected('file/byte bound')
        with mock.patch.object(MATERIALS, 'MAX_MATERIAL_TOTAL', 1):
            self.rejected('material byte bound|total byte bound')

    def test_component_source_correspondence_failure_removes_own_partial_output(self):
        self.provenance['notices'][-1]['source_sha256'] = '0' * 64
        self.reseal()
        frozen = (self.source / 'src/component.c').read_bytes()
        self.rejected('source notice bytes differ')
        self.assertEqual((self.source / 'src/component.c').read_bytes(), frozen)

    def test_caught_archive_write_failure_removes_only_new_output(self):
        add = MATERIALS.tar_add
        calls = []

        def interrupted(*arguments):
            calls.append(arguments[1])
            if len(calls) == 2:
                raise OSError('injected archive failure')
            return add(*arguments)

        with mock.patch.object(MATERIALS, 'tar_add', interrupted):
            with self.assertRaisesRegex(OSError, 'injected archive failure'):
                self.export()
        self.assertFalse(self.output.exists())
        self.assertEqual((self.package / 'licenses/NOTICE').read_bytes(),
                         b'PRIVATE RESEARCH PACKAGE - PUBLIC_NOT_READY\n')

    def test_replace_refs_cannot_redirect_the_expected_source_commit(self):
        original = self.revision
        expected = (self.source / 'configure').read_bytes()
        (self.source / 'configure').write_bytes(b'REPLACEMENT BYTES MUST NOT BE EXPORTED\n')
        self.commit()
        replacement = self.revision
        self.git('reset', '--hard', '--quiet', original)
        self.revision = original
        self.git('replace', original, replacement)
        result = self.export()
        self.assertEqual(result['source_revision'], original)
        with tarfile.open(self.output / 'source-materials.tar', 'r:') as archive:
            self.assertEqual(archive.extractfile('source/configure').read(), expected)

    def test_tracked_hardlink_is_exported_from_git_as_independent_bytes(self):
        alias = self.root / 'tracked-alias'
        os.link(self.source / 'LICENSE.md', alias)
        self.export()
        with tarfile.open(self.output / 'source-materials.tar', 'r:') as archive:
            member = archive.getmember('source/LICENSE.md')
            self.assertTrue(member.isfile())
            self.assertEqual(archive.extractfile(member).read(), alias.read_bytes())
            self.assertFalse(any(member.islnk() for member in archive.getmembers()))

    def test_declarative_positive_review_does_not_qualify_this_export(self):
        self.provenance.update(neutral_build_verified=True, corresponding_source_archive_included=True,
                               redistribution_review='APPROVED', update_maintenance_owner='fixture-owner')
        self.reseal()
        result = self.export()
        self.assertEqual(result['public_distribution_status'], 'PUBLIC_NOT_READY')
        self.assertEqual(result['source_correspondence_to_binary'], 'UNVERIFIED')
        self.assertEqual(result['redistribution_corresponding_source_sufficiency'], 'UNVERIFIED')
        self.assertEqual(result['update_maintenance_owner'], 'UNASSIGNED')

    def test_new_unrelated_file_is_preserved_during_caught_failure(self):
        def interrupted(*arguments):
            (self.output / 'unrelated').write_bytes(b'preserve unrelated bytes')
            raise OSError('injected archive failure')

        with mock.patch.object(MATERIALS, 'tar_add', interrupted):
            with self.assertRaisesRegex(OSError, 'injected archive failure'):
                self.export()
        self.assertEqual([path.name for path in self.output.iterdir()], ['unrelated'])
        self.assertEqual((self.output / 'unrelated').read_bytes(), b'preserve unrelated bytes')


if __name__ == '__main__':
    unittest.main()
