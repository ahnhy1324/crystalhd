#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the shipped stdlib editor model, without GTK or hardware."""
import ast
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("settings_ui", str(ROOT / "scripts/crystalhd-settings.py"))
UI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UI)
CORE = UI.load_core()
HEADER = '[crystalhd]\nschema = 1\n'


class SettingsModelTests(unittest.TestCase):
    def test_profile_cli_parity_on_the_model_interpreter(self):
        # Also runs under actual i386 Python 3.6; the larger CLI harness itself
        # uses newer Python syntax, while the shipped core does not.
        cli = [sys.executable, '-B', str(ROOT/'scripts/crystalhd-config.py')]
        options = ['--no-system-config', '--user-config', str(self.path)]
        operations = [(['save', '--profile-section', 'desk', 'vaapi.debug=false'], 0),
                      (['save', '--profile-section', 'desk', 'vaapi.debug=unset'], 0),
                      (['save', '--profile', 'desk'], 0),
                      (['save', '--profile-section', 'bad name'], 3)]
        for operation, expected in operations:
            result = subprocess.run(cli+operation+options, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(result.returncode, expected, result.stderr)
        e = self.editor()
        self.assertEqual(e.draft.metadata['profile'], 'desk')
        self.assertIs(e.draft.profiles['desk']['vaapi.debug'], CORE.UNSET)
        result = subprocess.run(cli+['delete-profile', 'desk']+options,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        e.reload()
        self.assertNotIn('profile', e.draft.metadata)
        self.assertEqual(e.draft.profiles, {})

    def test_profile_drafts_share_core_and_preview_does_not_select_default(self):
        e = self.editor()
        with mock.patch.object(CORE, 'patch_user_settings', wraps=CORE.patch_user_settings) as patch:
            e.create_profile('desk')
            e.set_literal('vaapi.debug', 'true', 'desk')
        self.assertEqual(patch.call_count, 2)
        self.assertFalse(self.path.exists())
        self.assertFalse(e.preview(False).effective['vaapi.debug'])
        self.assertTrue(e.preview(False, 'desk').effective['vaapi.debug'])
        self.assertNotIn('profile', e.draft.metadata)
        e.save(profile='desk')
        with self.assertRaises(CORE.SchemaError): e.create_profile('desk')
        e.set_selector('profile', 'desk')
        e.save()
        e.delete_profile('desk')
        self.assertNotIn('profile', e.draft.metadata)
        self.assertIn('desk', CORE.parse_document(self.path.read_bytes()).profiles)
        e.save()
        self.assertEqual(CORE.parse_document(self.path.read_bytes()).profiles, {})

    def test_profile_system_override_inheritance_and_stale_write(self):
        e = self.editor()
        e.system = CORE.parse_document((HEADER+'[profile.desk]\nvaapi.debug=true\n').encode('utf-8'))
        for literal, expected in [('false', False), ('unset', False), (None, True)]:
            e.set_literal('vaapi.debug', literal, 'desk')
            self.assertEqual(e.preview(False, 'desk').effective['vaapi.debug'], expected)
        e.delete_profile('desk')
        self.assertIn('desk', e.system.profiles)
        with self.assertRaises(CORE.SchemaError): e.delete_profile('desk')
        e.reload()
        e.create_profile('desk');e.set_literal('vaapi.debug', 'true', 'desk')
        other = self.editor();other.set_literal('vaapi.trace', 'true');other.save()
        original = self.path.read_bytes()
        with self.assertRaises(CORE.SchemaError): e.save(profile='desk')
        self.assertEqual(self.path.read_bytes(), original)
        self.assertTrue(e.dirty)

    def test_inactive_edited_profile_cannot_escape_validation_by_switching_scope(self):
        self.write(HEADER+'[profile.desk]\nvaapi.h264.mode="live"\n'
                   'vaapi.h264.low_latency=true\nvaapi.h264.ready_remap=true\n')
        e = self.editor()
        e.set_literal('vaapi.h264.low_latency', 'false', 'desk')
        e.set_literal('vaapi.debug', 'true')
        original = self.path.read_bytes()
        with self.assertRaises(CORE.SchemaError): e.save()
        self.assertEqual(self.path.read_bytes(), original)
        self.assertTrue(e.dirty)

    def test_removing_override_validates_revealed_system_profile_but_reset_does_not(self):
        self.write(HEADER+'[profile.desk]\nvaapi.h264.ready_remap=false\n')
        e = self.editor()
        system = CORE.parse_document((HEADER+'[profile.desk]\nvaapi.h264.ready_remap=true\n').encode('utf-8'))
        e.system = system
        self.assertFalse(e.preview(False, 'desk').effective['vaapi.h264.ready_remap'])
        e.delete_profile('desk')
        original = self.path.read_bytes()
        with mock.patch.object(CORE, 'load_document', return_value=system):
            with self.assertRaises(CORE.SchemaError): e.save()
        self.assertEqual(self.path.read_bytes(), original)
        # Reset clears the entire user draft. It does not select/validate an
        # inactive profile merely because it was edited before Reset.
        with mock.patch.object(CORE, 'load_document', return_value=system):
            e.reset()
        self.assertEqual(CORE.parse_document(self.path.read_bytes()).profiles, {})

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="crystalhd-settings-test-")
        self.path = Path(self.temp.name) / 'private/config.ini'
        self.environment = {"HOME": str(Path(self.temp.name) / "home")}

    def tearDown(self):
        self.temp.cleanup()

    def write(self, text):
        self.path.parent.mkdir(mode=0o700, exist_ok=True)
        self.path.write_text(text, encoding='utf-8')
        self.path.chmod(0o600)

    def editor(self, environment=None):
        return UI.Editor(CORE, str(self.path), None, environment or self.environment)

    def test_no_config_open_does_not_write_and_save_reuses_core(self):
        e = self.editor({'CRYSTALHD_VAAPI_TRACE': '1', 'HOME': self.environment['HOME']})
        self.assertFalse(self.path.parent.exists())
        self.assertFalse(e.dirty)
        e.set_literal('vaapi.debug', 'true')
        with mock.patch.object(CORE, 'save_user_document', wraps=CORE.save_user_document) as writer:
            message = e.save()
        self.assertEqual(writer.call_count, 1)
        self.assertIn('saved', message.lower())
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        d = CORE.parse_document(self.path.read_bytes())
        self.assertEqual(d.settings, {'vaapi.debug': True})
        self.assertFalse(e.dirty)
        self.assertNotIn('CRYSTALHD', self.path.read_text())

    def test_false_unset_remove_and_cli_parity_preserve_profiles(self):
        self.write(HEADER + '[settings]\nvaapi.debug=true\n[profile.desk]\nvaapi.trace=true\n')
        e = self.editor()
        for source, expected in [('false', False), ('unset', CORE.UNSET), (None, None)]:
            e.set_literal('vaapi.debug', source);e.save()
            d = CORE.parse_document(self.path.read_bytes())
            self.assertEqual(d.settings.get('vaapi.debug'), expected)
            self.assertEqual(d.profiles, {'desk': {'vaapi.trace': True}})
        result = subprocess.run([sys.executable, '-B', str(ROOT/'scripts/crystalhd-config.py'), 'show-effective',
                                 '--no-system-config', '--user-config', str(self.path), '--format', 'json'],
                                env=dict(os.environ, HOME=self.environment['HOME']), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, universal_newlines=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        cli = json.loads(result.stdout)
        model = e.report()
        self.assertEqual([(r['key'],r['effective']) for r in cli['settings']],
                         [(r['key'],r['effective']) for r in model['settings']])

    def test_profiles_and_raw_environment_win_without_entering_saved_defaults(self):
        self.write(HEADER + '[profile.desk]\nvaapi.debug=true\n')
        e = self.editor({'CRYSTALHD_VAAPI_DEBUG':'0'})
        e.set_selector('profile', 'desk')
        e.set_literal('vaapi.debug', 'false')
        r = e.preview()
        self.assertTrue(r.effective['vaapi.debug'])  # raw DEBUG=0 is presence/on
        self.assertTrue(r.origins['vaapi.debug'].startswith('environment:'))
        e.save()
        self.assertNotIn('CRYSTALHD', self.path.read_text())
        self.assertEqual(CORE.parse_document(self.path.read_bytes()).settings['vaapi.debug'], False)
        self.assertIn('profile', self.path.read_text())

    def test_stale_editor_preserves_external_writer_and_draft(self):
        self.write(HEADER)
        a,b = self.editor(),self.editor()
        a.set_literal('vaapi.debug','true');b.set_literal('vaapi.trace','true');b.save()
        newer = self.path.read_bytes()
        with self.assertRaises(CORE.SchemaError): a.save()
        self.assertEqual(self.path.read_bytes(),newer)
        self.assertTrue(a.dirty)
        a.reload();self.assertFalse(a.dirty)
        self.assertEqual(a.draft.settings,{'vaapi.trace':True})

    def test_malformed_reset_uses_original_snapshot_and_cannot_bypass_unsafe_file(self):
        self.write('not an INI\n')
        e = self.editor()
        self.assertIsNone(e.draft);self.assertIsNotNone(e.snapshot)
        self.assertTrue(e.load_error)
        self.write(HEADER+'[settings]\nvaapi.debug=true\n')
        with self.assertRaises(CORE.SchemaError):e.reset()
        e.reload();e.reset()
        self.assertEqual(CORE.parse_document(self.path.read_bytes()).settings,{})
        self.path.chmod(0o666)
        e.reload();self.assertIsNone(e.snapshot)
        with self.assertRaises(CORE.SchemaError):e.reset()

    def test_invalid_values_selectors_and_transactions_never_write(self):
        e = self.editor()
        for key,value in [('unknown','true'),('unknown',None),('vaapi.debug','1')]:
            with self.assertRaises(CORE.SchemaError):e.set_literal(key,value)
        with self.assertRaises(CORE.SchemaError):e.set_selector('profile','bad name')
        e.set_selector('profile','missing')
        with self.assertRaises(CORE.SchemaError):e.save()
        self.assertFalse(self.path.exists())

    def test_experiments_need_fresh_existing_ack_and_remain_launch_only(self):
        self.write(HEADER+'preset="low-memory"\n')
        e = self.editor()
        self.assertTrue(e.native_requires_launch())
        e.set_literal('vaapi.debug','true')
        original=self.path.read_bytes()
        with self.assertRaises(CORE.SchemaError):e.save()
        self.assertEqual(self.path.read_bytes(),original)
        tokens=e.required_acknowledgements()
        e.save(tokens)
        self.assertNotIn('low-memory-8-buffer',self.path.read_text())
        e.set_literal('vaapi.debug','false')
        with self.assertRaises(CORE.SchemaError):e.save()  # no persisted acknowledgement

    def test_uncertain_committed_nondurable_and_postwrite_race_require_reload(self):
        self.write(HEADER)
        e=self.editor();e.set_literal('vaapi.debug','true')
        with mock.patch.object(CORE,'save_user_document',return_value=CORE.SaveResult(False,committed=None)):
            self.assertIn('uncertain',e.save())
        self.assertTrue(e.write_blocked);self.assertTrue(e.dirty)
        with self.assertRaises(CORE.SchemaError):e.save()
        e.reload();e.set_literal('vaapi.debug','true')
        real=CORE.save_user_document
        def nondurable(*a,**kw):
            real(*a,**kw);return CORE.SaveResult(False)
        with mock.patch.object(CORE,'save_user_document',side_effect=nondurable):
            self.assertIn('durability',e.save())
        self.assertTrue(e.write_blocked)
        self.assertTrue(CORE.parse_document(self.path.read_bytes()).settings['vaapi.debug'])
        e.reload();e.set_literal('vaapi.debug','false')
        def after_write(*a,**kw):
            result=real(*a,**kw)
            self.write(HEADER+'[settings]\nvaapi.trace=true\n')
            return result
        with mock.patch.object(CORE,'save_user_document',side_effect=after_write):
            self.assertIn('readback',e.save())
        self.assertTrue(e.write_blocked)
        self.assertEqual(CORE.parse_document(self.path.read_bytes()).settings,{'vaapi.trace':True})

    def test_report_redacts_private_values_and_bypass_is_explicit(self):
        self.write(HEADER+'[settings]\nchromium.password_store="private-value"\nplayer.python="/private/python"\n')
        e=self.editor({'CRYSTALHD_CONFIG_DISABLE':'1'})
        report=e.report();text=json.dumps(report)
        self.assertNotIn('private-value',text);self.assertNotIn('/private/python',text)
        self.assertEqual(report['native_file_reading'],'bypassed-by-environment')

    def test_help_and_model_import_do_not_need_gui_and_syntax_is_python36(self):
        script=ROOT/'scripts/crystalhd-settings.py'
        for p in (script, ROOT/'tests/crystalhd-settings.test.py'):
            if sys.version_info >= (3,8):
                ast.parse(p.read_text(),feature_version=(3,6))
            else:
                ast.parse(p.read_text())
        result=subprocess.run([sys.executable,'-S','-B',str(script),'--help'],
                              stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True)
        self.assertEqual((result.returncode,result.stderr),(0,''))
        self.assertIn('--no-tray',result.stdout)
        with mock.patch.dict(sys.modules,{'gi':None}):
            e=self.editor();self.assertIsNotNone(e.draft)


class SettingsPackagingTests(unittest.TestCase):
    def test_staged_custom_paths_install_core_icon_launcher_and_remove_only_ui(self):
        with tempfile.TemporaryDirectory(prefix='crystalhd-settings-stage-') as folder:
            stage=Path(folder)/'stage';data='/opt/crystalhd data/share';bindir='/opt/crystalhd bin'
            args=['DESTDIR='+str(stage),'BINDIR='+bindir,'DATADIR='+data,'PYTHON3='+sys.executable]
            result=subprocess.run(['make','install-settings']+args,cwd=str(ROOT),
                                  stdout=subprocess.PIPE,stderr=subprocess.STDOUT,universal_newlines=True)
            self.assertEqual(result.returncode,0,result.stdout)
            gui=stage/bindir.lstrip('/')/'crystalhd-settings'
            helper=gui.with_name('crystalhd-config')
            self.assertTrue(gui.stat().st_mode&0o111)
            # Load the actual extensionless installed script and its adjacent
            # core; the data manifest lives in an independently chosen path.
            program="import importlib.machinery,importlib.util; loader=importlib.machinery.SourceFileLoader('installed_ui',__import__('sys').argv[1]); spec=importlib.util.spec_from_loader(loader.name,loader); m=importlib.util.module_from_spec(spec); loader.exec_module(m); c=m.load_core(); assert len(c.load_preset_catalog().entries)==7; assert str(m.data_directory()).endswith('/opt/crystalhd data/share')"
            run=subprocess.run([sys.executable,'-B','-c',program,str(gui)],stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE,universal_newlines=True)
            self.assertEqual(run.returncode,0,run.stderr)
            iconroot=stage/data.lstrip('/')/'icons/hicolor'
            sentinel=iconroot/'32x32/apps/other-app.png';sentinel.write_bytes(b'untouched')
            desktop=stage/data.lstrip('/')/'applications/crystalhd-settings.desktop'
            self.assertTrue(desktop.is_file())
            for name in ('install-settings','uninstall-settings','uninstall-settings'):
                result=subprocess.run(['make',name]+args,cwd=str(ROOT),stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT,universal_newlines=True)
                self.assertEqual(result.returncode,0,result.stdout)
            self.assertFalse(gui.exists());self.assertFalse(desktop.exists())
            self.assertTrue(helper.is_file());self.assertEqual(sentinel.read_bytes(),b'untouched')
            self.assertFalse(list(iconroot.glob('*/apps/crystalhd-settings*')))


if __name__=='__main__':unittest.main()
