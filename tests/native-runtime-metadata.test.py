#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""CPU-only contracts for the real private-runtime metadata worker.

Only synthetic files, Python function stand-ins and mapping records are used.
No selected ELF, compiler, namespace, package CLI or hardware is executed.
"""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/kodi-route-probe/runtime-package-metadata.py'
SPEC = importlib.util.spec_from_file_location('native_runtime_metadata_under_test', SCRIPT)
WORKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WORKER)


class Function:
    def __init__(self, value):
        self.value = value

    def __call__(self, *args):
        return self.value(*args) if callable(self.value) else self.value


class Library:
    def __init__(self, prefix):
        self.prefix = prefix

    def __getattr__(self, name):
        if name.endswith('_version'):
            value = 63 << 16
        elif name.endswith('_configuration'):
            value = b'--prefix=' + self.prefix + b' --enable-shared'
        elif name == 'avcodec_find_decoder_by_name':
            value = 1
        elif name == 'avcodec_get_hw_config':
            config = types.SimpleNamespace(pix_fmt=123, methods=5, device_type=1)
            value = lambda _codec, index: types.SimpleNamespace(contents=config) if index == 0 else None
        elif name == 'av_get_pix_fmt_name':
            value = b'drm_prime'
        else:
            raise AssertionError('unexpected metadata call: ' + name)
        function = Function(value)
        setattr(self, name, function)
        return function


class RuntimeMetadataTests(unittest.TestCase):
    def configurations(self, prefix):
        return {kind: b'--prefix=' + prefix + b' --enable-shared' for kind in WORKER.KINDS}

    def provenance(self, prefix, private=True):
        return {'embedded_configure_prefix_sha256': [hashlib.sha256(prefix).hexdigest()],
                'private_build_prefixes_detected': private,
                'neutral_build_verified': False}

    @contextlib.contextmanager
    def paths(self, visible=(), video=False, compiler=None):
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(WORKER.shutil, 'which',
                side_effect=lambda name: '/usr/bin/' + name if name == compiler else None))
            stack.enter_context(mock.patch.object(WORKER.Path, 'exists', autospec=True,
                side_effect=lambda path: str(path) in visible))
            stack.enter_context(mock.patch.object(WORKER.Path, 'is_symlink', autospec=True,
                return_value=False))
            stack.enter_context(mock.patch.object(WORKER.Path, 'glob', autospec=True,
                side_effect=lambda _path, _pattern: iter([Path('/dev/video7')]) if video else iter(())))
            yield

    def test_legacy_private_prefix_must_remain_absent(self):
        prefix = b'/home/retained-build/ffmpeg'
        with self.paths():
            result = WORKER.check_configure_prefixes(self.configurations(prefix), self.provenance(prefix))
        self.assertFalse(result['original_build_prefix_visible'])
        self.assertEqual(result['visible_neutral_configure_prefixes'], [])
        self.assertTrue(result['nonneutral_configure_prefixes_absent'])

    def test_neutral_usr_visibility_is_reported_and_not_promoted(self):
        with self.paths(visible={'/usr'}):
            result = WORKER.check_configure_prefixes(
                self.configurations(b'/usr'), self.provenance(b'/usr', private=False))
        self.assertTrue(result['original_build_prefix_visible'])
        self.assertEqual(result['visible_neutral_configure_prefixes'], ['/usr'])
        self.assertEqual(result['configure_prefix_visibility'], [
            {'sha256': hashlib.sha256(b'/usr').hexdigest(), 'neutral_usr': True, 'visible': True}])
        self.assertNotIn('neutral_build_verified', result)
        self.assertNotIn('clean_OS_and_Kodi_playback_qualification', result)

    def test_usr_requires_explicit_boolean_false_provenance(self):
        for value in (None, True, 0, 'false'):
            with self.subTest(value=value), self.paths(visible={'/usr'}):
                with self.assertRaisesRegex(RuntimeError, 'explicit private-prefix-free'):
                    WORKER.check_configure_prefixes(
                        self.configurations(b'/usr'), self.provenance(b'/usr', private=value))

    def test_unknown_visible_prefix_has_no_neutral_exception(self):
        for prefix in (b'/opt/unreviewed', b'/usr/local', b'/usr/../usr'):
            with self.subTest(prefix=prefix), self.paths(visible={prefix.decode()}):
                with self.assertRaisesRegex(RuntimeError, 'nonneutral build prefix is visible'):
                    WORKER.check_configure_prefixes(
                        self.configurations(prefix), self.provenance(prefix, private=False))

    def test_visible_private_prefix_is_rejected(self):
        prefix = b'/home/retained-build/ffmpeg'
        with self.paths(visible={prefix.decode()}):
            with self.assertRaisesRegex(RuntimeError, 'nonneutral build prefix is visible'):
                WORKER.check_configure_prefixes(self.configurations(prefix), self.provenance(prefix))

    def test_prefix_hash_mismatch_or_malformed_inventory_is_rejected(self):
        for recorded in (['0' * 64], [], ['wrong'], None, '0' * 64):
            provenance = self.provenance(b'/usr', private=False)
            provenance['embedded_configure_prefix_sha256'] = recorded
            with self.subTest(recorded=recorded), self.paths(visible={'/usr'}):
                with self.assertRaisesRegex(RuntimeError, 'provenance inventory'):
                    WORKER.check_configure_prefixes(self.configurations(b'/usr'), provenance)

    def test_mixed_or_missing_configuration_is_not_a_neutral_package(self):
        configs = self.configurations(b'/usr')
        configs['avcodec'] = b'--prefix=/opt/unreviewed --enable-shared'
        provenance = self.provenance(b'/usr', private=False)
        provenance['embedded_configure_prefix_sha256'] = sorted(
            hashlib.sha256(value).hexdigest() for value in (b'/usr', b'/opt/unreviewed'))
        with self.paths(visible={'/usr'}):
            with self.assertRaisesRegex(RuntimeError, 'explicit private-prefix-free'):
                WORKER.check_configure_prefixes(configs, provenance)
        configs = self.configurations(b'/usr')
        del configs['avdevice']
        with self.assertRaisesRegex(RuntimeError, 'incomplete seven-library'):
            WORKER.check_configure_prefixes(configs, self.provenance(b'/usr', private=False))

    def test_namespace_device_overlay_and_compiler_guards_remain_active(self):
        with self.paths():
            self.assertFalse(any(WORKER.check_namespace().values()))
        for path in ('/dev/crystalhd', '/dev/dri', '/sys', '/app', '/gl', '/probe/deps', '/native-lib'):
            with self.subTest(path=path), self.paths(visible={path}):
                with self.assertRaises(RuntimeError):
                    WORKER.check_namespace()
        with self.paths(video=True):
            with self.assertRaisesRegex(RuntimeError, 'device or sysfs'):
                WORKER.check_namespace()
        with self.paths(compiler='cc'):
            with self.assertRaisesRegex(RuntimeError, 'compiler/build tool'):
                WORKER.check_namespace()

    @staticmethod
    def map_line(path):
        return '1000-2000 r-xp 00000000 00:00 1 ' + str(path)

    def create_package(self, root, prefix):
        (root / 'lib').mkdir()
        (root / 'metadata').mkdir()
        entries, sonames, mappings = [], {}, []
        for kind in WORKER.KINDS:
            name = 'lib' + kind + '.so.63'
            value = ('synthetic-' + kind).encode()
            (root / 'lib' / name).write_bytes(value)
            entries.append({'kind': 'file', 'path': 'lib/' + name,
                            'sha256': hashlib.sha256(value).hexdigest()})
            sonames[kind] = name
            mappings.append(self.map_line(root / 'lib' / name))
        (root / 'metadata/files.json').write_text(json.dumps(entries))
        (root / 'metadata/dependencies.json').write_text(json.dumps({'sonames': sonames}))
        (root / 'metadata/provenance.json').write_text(json.dumps(
            self.provenance(prefix, private=prefix != b'/usr')))
        return mappings

    def run_main(self, root, prefix, mappings, load_events=None):
        output = io.StringIO()
        visible = {'/usr'} if prefix == b'/usr' else set()
        if load_events is None:
            load_events = []

        def load_library(path, **_kwargs):
            load_events.append(path)
            return Library(prefix)

        with self.paths(visible=visible), contextlib.redirect_stdout(output), \
                mock.patch.object(sys, 'argv', [str(SCRIPT), '--package', str(root)]), \
                mock.patch.object(WORKER.Path, 'read_text', return_value='\n'.join(mappings)), \
                mock.patch.object(WORKER.ctypes, 'CDLL', side_effect=load_library) as load:
            result = WORKER.main()
        return result, json.loads(output.getvalue()), load.call_count

    def test_real_main_accepts_legacy_and_declared_neutral_with_exact_seven_maps(self):
        for prefix in (b'/home/retained-build/ffmpeg', b'/usr'):
            with self.subTest(prefix=prefix), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                mappings = self.create_package(root, prefix)
                code, value, loads = self.run_main(root, prefix, mappings)
                self.assertEqual(code, 0)
                self.assertEqual(loads, 7)
                self.assertEqual(value['status'], 'PASS_COMPILERLESS_RELOCATED_METADATA_ONLY')
                self.assertTrue(value['all_seven_FFmpeg_mappings_match_exact_package'])
                self.assertEqual(value['original_build_prefix_visible'], prefix == b'/usr')

    def test_real_main_rejects_platform_fallback_even_with_valid_neutral_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mappings = self.create_package(root, b'/usr')
            mappings[-1] = self.map_line('/usr/lib/libavdevice.so.63')
            with self.assertRaisesRegex(RuntimeError, 'exact relocated set'):
                self.run_main(root, b'/usr', mappings)
            mappings[-1] = self.map_line('/host-libraries/libavdevice.so.63')
            with self.assertRaisesRegex(RuntimeError, 'outside pinned Platform/package'):
                self.run_main(root, b'/usr', mappings)
            with self.assertRaisesRegex(RuntimeError, 'exact relocated set'):
                self.run_main(root, b'/usr', mappings[:-1])

    def test_real_main_rejects_source_payload_hash_change_before_loading(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            mappings = self.create_package(root, b'/usr')
            (root / 'lib/libavcodec.so.63').write_bytes(b'changed-package-input')
            loads = []
            with self.assertRaisesRegex(RuntimeError, 'target file does not match package manifest'):
                self.run_main(root, b'/usr', mappings, load_events=loads)
            self.assertEqual(loads, [])


if __name__ == '__main__':
    unittest.main()
