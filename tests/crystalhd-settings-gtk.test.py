#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Real GTK controls with private INI files. Requires a display or xvfb-run."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('settings_gtk',str(ROOT/'scripts/crystalhd-settings.py'))
UI=importlib.util.module_from_spec(spec);spec.loader.exec_module(UI)
CORE=UI.load_core()
MODULES=UI.load_gtk()
Gtk,Gdk,Gio,GLib=MODULES
if not Gtk.init_check([])[0]:raise RuntimeError('GTK tests require a graphical display (use xvfb-run -a)')
HEADER='[crystalhd]\nschema = 1\n'


def drain():
    while Gtk.events_pending():Gtk.main_iteration_do(False)


def select(combo,token):
    for row in combo.get_model():
        if row[0]==token:
            combo.set_active_iter(row.iter);drain();return
    raise AssertionError('missing choice '+token)


class GtkSettingsTests(unittest.TestCase):
    def test_profile_buttons_target_preview_default_and_confirmed_removal(self):
        app = self.app()
        app.prompt_profile_name = lambda: 'desk'
        app.new_profile_button.clicked();drain()
        self.assertEqual(app.editing_profile, 'desk')
        self.assertIn('Editing profile: desk', app.status.get_text())
        self.assertEqual(app.token(app.profile), '')
        select(app.controls['vaapi.debug'], 'true')
        self.assertTrue(app.editor.preview(False, 'desk').effective['vaapi.debug'])
        self.assertFalse(app.editor.preview(False).effective['vaapi.debug'])
        self.assertIn('Previewing editing profile', app.guidance.get_text())
        select(app.layer, '')
        self.assertEqual(app.token(app.controls['vaapi.debug']), '')
        select(app.controls['vaapi.trace'], 'true')
        select(app.layer, 'desk')
        self.assertEqual(app.token(app.controls['vaapi.debug']), 'true')
        select(app.profile, 'desk')
        app.save_button.clicked();drain()
        doc = CORE.parse_document(self.path.read_bytes())
        self.assertEqual(doc.profiles['desk'], {'vaapi.debug': True})
        self.assertEqual(doc.settings, {'vaapi.trace': True})
        self.assertEqual(doc.metadata['profile'], 'desk')
        app.confirm = lambda *_: False
        app.delete_profile_button.clicked();drain()
        self.assertFalse(app.editor.dirty)
        app.confirm = lambda *_: True
        app.delete_profile_button.clicked();drain()
        self.assertTrue(app.editor.dirty)
        self.assertIsNone(app.editing_profile)
        self.assertIn('desk', CORE.parse_document(self.path.read_bytes()).profiles)
        app.save_button.clicked();drain()
        self.assertEqual(CORE.parse_document(self.path.read_bytes()).profiles, {})

    def test_system_profile_is_only_overlaid_and_draft_dialog_does_not_write(self):
        app = self.app()
        app.editor.system = CORE.parse_document((HEADER+'[profile.system]\nvaapi.debug=true\n').encode('utf-8'))
        app.sync_form()
        select(app.layer, 'system')
        self.assertFalse(app.delete_profile_button.get_sensitive())
        select(app.controls['vaapi.debug'], 'false')
        self.assertTrue(app.delete_profile_button.get_sensitive())
        self.assertTrue(app.editor.system.profiles['system']['vaapi.debug'])
        self.assertFalse(app.editor.preview(False, 'system').effective['vaapi.debug'])
        app.confirm = lambda *_: True
        app.delete_profile_button.clicked();drain()
        self.assertEqual(app.editing_profile, 'system')
        self.assertTrue(app.editor.preview(False, 'system').effective['vaapi.debug'])
        self.assertFalse(self.path.exists())
        def type_name():
            dialog = next(w for w in Gtk.Window.list_toplevels() if isinstance(w, Gtk.Dialog))
            entry = next(w for w in dialog.get_content_area().get_children() if isinstance(w, Gtk.Entry))
            entry.set_text('bad name')
            dialog.response(Gtk.ResponseType.OK)
            return False
        GLib.idle_add(type_name)
        app.new_profile_button.clicked();drain()
        self.assertIn('invalid profile', app.status.get_text())
        self.assertNotIn('bad name', app.editor.draft.profiles)
        self.assertFalse(self.path.exists())

    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='crystalhd-settings-gtk-')
        self.path=Path(self.temp.name)/'config.ini'
        self.windows=[]

    def tearDown(self):
        for app in self.windows:
            app.quitting=True;app.window.destroy()
        drain();self.temp.cleanup()

    def write(self,text):
        self.path.write_text(text,encoding='utf-8');self.path.chmod(0o600)

    def app(self,environment=None,no_tray=True):
        e=UI.Editor(CORE,str(self.path),None,environment or {'HOME':self.temp.name})
        app=UI.SettingsWindow(e,MODULES,no_tray=no_tray)
        self.windows.append(app);app.window.show_all();drain();return app

    def test_real_save_false_unset_inheritance_and_accessible_controls(self):
        app=self.app()
        self.assertFalse(self.path.exists())
        self.assertFalse(app.save_button.get_sensitive())
        combo=app.controls['vaapi.debug']
        self.assertIn('debug',combo.get_accessible().get_name().lower())
        for token,expected in [('true',True),('false',False),('unset',CORE.UNSET),('',None)]:
            select(combo,token)
            self.assertTrue(app.save_button.get_sensitive())
            app.save_button.clicked();drain()
            self.assertEqual(CORE.parse_document(self.path.read_bytes()).settings.get('vaapi.debug'),expected)
            self.assertIn('saved',app.status.get_text().lower())
            self.assertFalse(app.save_button.get_sensitive())
        self.assertEqual(self.path.stat().st_mode&0o777,0o600)

    def test_keyboard_save_and_rejected_discard_preserve_draft(self):
        app=self.app();select(app.controls['vaapi.trace'],'true')
        event=Gdk.Event.new(Gdk.EventType.KEY_PRESS)
        event.state=Gdk.ModifierType.CONTROL_MASK;event.keyval=Gdk.KEY_s
        self.assertTrue(app.window.emit('key-press-event',event));drain()
        self.assertTrue(CORE.parse_document(self.path.read_bytes()).settings['vaapi.trace'])
        select(app.controls['vaapi.debug'],'true')
        app.confirm=lambda *_:False
        app.reload_button.clicked();drain()
        self.assertTrue(app.editor.dirty)
        app.close_window();drain();self.assertTrue(app.window.get_visible())
        self.assertNotIn('vaapi.debug',self.path.read_text())

    def test_malformed_file_confirmed_reset_and_stale_save_keep_external_data(self):
        self.write('malformed\n');app=self.app()
        self.assertTrue(app.reset_button.get_sensitive());self.assertFalse(app.save_button.get_sensitive())
        app.confirm=lambda *_:True
        app.reset_button.clicked();drain()
        self.assertEqual(CORE.parse_document(self.path.read_bytes()).settings,{})
        select(app.controls['vaapi.debug'],'true')
        self.write(HEADER+'[settings]\nvaapi.trace=true\n')
        app.save_button.clicked();drain()
        self.assertEqual(CORE.parse_document(self.path.read_bytes()).settings,{'vaapi.trace':True})
        self.assertTrue(app.editor.dirty)
        self.assertIn('reload',app.status.get_text().lower())

    def test_no_host_fallback_and_lost_tray_reveal_window_without_losing_edits(self):
        with mock.patch.object(UI,'tray_host_present',return_value=False), mock.patch.object(UI,'load_indicator') as indicator:
            app=self.app(no_tray=False)
            indicator.assert_not_called()
            self.assertIsNone(app.indicator)
            self.assertIn('Tray unavailable',app.status.get_text())
            select(app.controls['vaapi.debug'],'true')
            app.tray_connected=True
            app.close_window();drain();self.assertFalse(app.window.get_visible())
            self.assertTrue(app.editor.dirty)
            app.tray_connection_changed(None,False);drain()
            self.assertTrue(app.window.get_visible());self.assertTrue(app.editor.dirty)
        self.assertFalse(self.path.exists())
        with mock.patch.object(UI,'tray_host_present',return_value=True), mock.patch.object(UI,'load_indicator',return_value=None):
            fallback=self.app(no_tray=False)
            self.assertIsNone(fallback.indicator)
            self.assertFalse(fallback.tray_connected)
            self.assertTrue(fallback.window.get_visible())

    def test_unsupported_presets_cannot_activate_and_origin_model_is_explicit(self):
        app=self.app({'CRYSTALHD_VAAPI_DEBUG':'0',CORE.CENTERED_NV12_ENVIRONMENT:'private-unvalidated-geometry'})
        for row in app.preset.get_model():
            if row[0] in ('streaming','low-memory','performance','experimental','custom'):
                self.assertFalse(row[2])
            if row[0]=='quality':
                self.assertTrue(row[2]);self.assertIn('Player only',row[1])
        for key in ('vaapi.h264.low_latency','vaapi.h264.ready_remap','vaapi.i915_wc'):
            self.assertNotIn('true',[r[0] for r in app.controls[key].get_model()])
        row=next(r for r in app.values if r[0]=='vaapi.debug')
        self.assertEqual(row[1],'true');self.assertIn('environment:',row[2])
        self.assertIn('unconfirmed',row[3])
        self.assertIn('true',app.badges['vaapi.debug'].get_text())
        self.assertIn('Environment',app.badges['vaapi.debug'].get_text())
        self.assertEqual(len(app.values),len(CORE.RULES))
        self.assertIn('Model is partial',app.status.get_text())
        self.assertNotIn('private-unvalidated-geometry',app.status.get_text())
        self.assertTrue(row[4])
        frontend = next(r for r in app.values if r[0]=='player.decoder')
        self.assertIn('updated crystalhd-play', frontend[3])
        self.write(HEADER+'preset="low-memory"\n')
        app.editor.reload();app.sync_form()
        frontend = next(r for r in app.values if r[0]=='player.decoder')
        self.assertIn('Experimental preset', frontend[3])

    def test_player_controls_save_and_are_consumed_by_the_actual_player_parser(self):
        self.path=Path(self.temp.name)/'.config/crystalhd/config.ini'
        app=self.app()
        select(app.player_controls['player.decoder'],'"hardware"')
        for key,value in [('gstreamer.crystalhddec.scale_width','1280'),('player.software_output_size','1920x1080')]:
            select(app.player_controls[key],'custom')
            app.player_entries[key].set_text(value);drain()
        self.assertTrue(app.save_button.get_sensitive())
        app.save_button.clicked();drain()
        doc=CORE.parse_document(self.path.read_bytes())
        self.assertEqual(doc.settings,{'player.decoder':'hardware','gstreamer.crystalhddec.scale_width':1280,
                                       'player.software_output_size':'1920x1080'})
        spec=importlib.util.spec_from_file_location('player_from_settings',str(ROOT/'scripts/crystalhd-player.py'))
        player=importlib.util.module_from_spec(spec);spec.loader.exec_module(player)
        media=Path(self.temp.name)/'video.mp4';media.touch()
        with mock.patch.object(CORE,'SYSTEM_CONFIG',None),mock.patch.object(player,'load_config_core',return_value=CORE):
            args=player.arguments([str(media)],environment={'HOME':self.temp.name})
        self.assertEqual((args.mode,args.scale_width,args.software_output_size),('hardware',1280,(1920,1080)))

    def test_player_conflicts_are_visible_and_never_silently_clear_other_keys(self):
        app=self.app()
        for key,value in [('gstreamer.crystalhddec.scale_width','1280'),('player.software_output_size','1920x1080')]:
            select(app.player_controls[key],'custom');app.player_entries[key].set_text(value);drain()
        select(app.player_controls['player.decoder'],'"software"')
        self.assertFalse(app.save_button.get_sensitive())
        self.assertIn('scale width requires hardware',app.status.get_text())
        self.assertEqual(app.editor.draft.settings['gstreamer.crystalhddec.scale_width'],1280)
        select(app.player_controls['gstreamer.crystalhddec.scale_width'],'unset')
        self.assertFalse(app.save_button.get_sensitive())
        self.assertEqual(app.editor.draft.settings['player.software_output_size'],'1920x1080')
        select(app.player_controls['player.software_output_size'],'unset')
        self.assertTrue(app.save_button.get_sensitive())
        app.save_button.clicked();drain()
        saved=CORE.parse_document(self.path.read_bytes()).settings
        self.assertEqual(saved['player.decoder'],'software')
        self.assertIs(saved['gstreamer.crystalhddec.scale_width'],CORE.UNSET)
        self.assertIs(saved['player.software_output_size'],CORE.UNSET)

    def test_invalid_player_text_survives_scope_switch_and_rejected_discard(self):
        app=self.app();app.prompt_profile_name=lambda:'desk';app.new_profile_button.clicked();drain()
        key='gstreamer.crystalhddec.scale_width'
        select(app.player_controls[key],'custom');app.player_entries[key].set_text('127');drain()
        self.assertFalse(app.save_button.get_sensitive())
        self.assertTrue(app.player_errors[key].get_text())
        select(app.layer,'')
        self.assertFalse(app.save_button.get_sensitive())
        select(app.layer,'desk')
        self.assertEqual(app.player_entries[key].get_text(),'127')
        app.confirm=lambda *_:False
        app.reload_button.clicked();app.quit_button.clicked();drain()
        self.assertTrue(app.window.get_visible())
        self.assertEqual(app.player_entries[key].get_text(),'127')
        self.assertFalse(self.path.exists())
        select(app.player_controls[key],'unset')
        self.assertFalse(app.pending_player)
        self.assertTrue(app.save_button.get_sensitive())

    def test_tray_startup_hides_only_after_connection_and_buttons_explain_actions(self):
        app=self.app()
        app.startup_hide_pending=True;app.start();drain()
        self.assertTrue(app.window.get_visible())
        self.assertEqual(app.close_button.get_label(),'_Close')
        self.assertFalse(app.close_button.get_visible())
        with mock.patch.object(UI,'tray_host_present',return_value=True):
            app.tray_connection_changed(None,True);drain()
        self.assertFalse(app.window.get_visible())
        self.assertEqual(app.close_button.get_label(),'Hide to tray')
        self.assertIn('Save changes',app.save_button.get_label().replace('_',''))
        self.assertIn('Clear all user',app.reset_button.get_label().replace('_',''))
        app.show_settings();drain();self.assertTrue(app.window.get_visible())
        app.close_button.clicked();drain();self.assertFalse(app.window.get_visible())
        with mock.patch.object(UI,'tray_host_present',return_value=False):
            app.tray_connection_changed(None,False);drain()
        self.assertTrue(app.window.get_visible())
        with mock.patch.object(UI,'tray_host_present',return_value=True):
            app.tray_connection_changed(None,True);drain()
        self.assertTrue(app.window.get_visible())

    def test_icon_native_sizes_and_optional_private_preview(self):
        from gi.repository import GdkPixbuf
        for size in (16,22,24,32,64,128):
            path=ROOT/'share/icons/hicolor'/('{0}x{0}'.format(size))/'apps/crystalhd-settings.png'
            pix=GdkPixbuf.Pixbuf.new_from_file(str(path))
            self.assertEqual((pix.get_width(),pix.get_height()),(size,size))
            self.assertTrue(pix.get_has_alpha())
        capture=os.environ.get('CRYSTALHD_SETTINGS_TEST_SCREENSHOT')
        if capture:
            app=self.app()
            for key,value in [('gstreamer.crystalhddec.scale_width','1280'),('player.software_output_size','1920x1080')]:
                select(app.player_controls[key],'custom');app.player_entries[key].set_text(value)
            app.window.resize(720,560);drain()
            # Only this synthetic settings window; never capture the desktop.
            pix=Gdk.pixbuf_get_from_window(app.window.get_window(),0,0,
                                          app.window.get_allocated_width(),app.window.get_allocated_height())
            pix.savev(capture,'png',[],[])


if __name__=='__main__':unittest.main()
