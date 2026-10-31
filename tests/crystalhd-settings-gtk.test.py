#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Real GTK controls with private INI files. Requires a display or xvfb-run."""
import importlib.util
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('settings_gtk',str(ROOT/'scripts/crystalhd-settings.py'))
UI=importlib.util.module_from_spec(spec);spec.loader.exec_module(UI)
CORE=UI.load_core()
MODULES=UI.load_gtk()
Gtk,Gdk,Gio,GLib=MODULES
from gi.repository import Atk
if not Gtk.init_check([])[0]:raise RuntimeError('GTK tests require a graphical display (use xvfb-run -a)')
HEADER='[crystalhd]\nschema = 1\n'


def drain():
    while Gtk.events_pending():Gtk.main_iteration_do(False)


def settle(predicate):
    deadline = time.monotonic() + 5
    while not predicate() and time.monotonic() < deadline:
        drain()
        time.sleep(.005)
    assert predicate(), 'installation inspection did not finish'


def select(combo,token):
    for row in combo.get_model():
        if row[0]==token:
            combo.set_active_iter(row.iter);drain();return
    raise AssertionError('missing choice '+token)


class GtkSettingsTests(unittest.TestCase):
    def assert_label_relation(self, widget, mnemonic):
        labels = widget.list_mnemonic_labels()
        self.assertEqual(len(labels), 1)
        label = labels[0]
        self.assertIs(label.get_mnemonic_widget(), widget)
        self.assertEqual(label.get_mnemonic_keyval(), mnemonic)
        relation = widget.get_accessible().ref_relation_set().get_relation_by_type(Atk.RelationType.LABELLED_BY)
        self.assertIsNotNone(relation)
        self.assertIn(label.get_accessible(), relation.get_target())
        self.assertEqual(widget.get_accessible().get_name(), label.get_text())
        return label

    def test_custom_player_values_have_visible_labels_and_keyboard_targets(self):
        app = self.app()
        for key, mnemonic in (('gstreamer.crystalhddec.scale_width', Gdk.KEY_a),
                              ('player.software_output_size', Gdk.KEY_e)):
            select(app.player_controls[key], 'custom')
            entry = app.player_entries[key]
            label = self.assert_label_relation(entry, mnemonic)
            self.assertTrue(label.get_visible())
            app.player_controls[key].grab_focus();drain()
            self.assertTrue(label.mnemonic_activate(False));drain()
            self.assertTrue(entry.has_focus())
        self.assertFalse(self.path.exists())

    def test_profile_name_dialog_has_visible_label_and_keyboard_target(self):
        app = self.app()
        observed = []
        def inspect_dialog():
            dialog = next(w for w in Gtk.Window.list_toplevels() if isinstance(w, Gtk.Dialog))
            try:
                entry = next(w for w in dialog.get_content_area().get_children() if isinstance(w, Gtk.Entry))
                label = self.assert_label_relation(entry, Gdk.KEY_p)
                self.assertTrue(label.get_visible())
                observed.append(True)
            finally:
                dialog.response(Gtk.ResponseType.CANCEL)
            return False
        GLib.idle_add(inspect_dialog)
        self.assertIsNone(app.prompt_profile_name())
        self.assertEqual(observed, [True])
        self.assertFalse(app.editor.dirty)
        self.assertFalse(self.path.exists())

    def test_installation_refresh_does_not_save_or_discard_drafts(self):
        with mock.patch.object(CORE, 'inspect_installation', return_value={'scope': 'test', 'readers': [{'state': 'unknown'}]}), \
                mock.patch.object(CORE, 'installation_text', return_value='File declaration only'):
            app = self.app()
            select(app.controls['vaapi.debug'], 'true')
            draft = app.editor.draft
            app.installation_refresh.clicked();drain()
            settle(lambda: app.installation_status.get_text() == 'File declaration only')
            self.assertIs(app.editor.draft, draft)
            self.assertTrue(app.editor.dirty)
            self.assertEqual(app.installation_status.get_text(), 'File declaration only')
            self.assertFalse(self.path.exists())

    def test_slow_startup_inspection_keeps_ui_editable_and_quit_cancellable(self):
        entered, release = threading.Event(), threading.Event()
        threads = []
        def inspect():
            threads.append(threading.get_ident())
            entered.set()
            release.wait(10)  # Bounds the RED run with the old synchronous UI.
            return {'readers': [{'state': 'declared-v1'}]}
        try:
            with mock.patch.object(CORE, 'inspect_installation', side_effect=inspect), \
                    mock.patch.object(CORE, 'installation_text', return_value='Finished'):
                app = self.app()
                self.assertTrue(entered.wait(3))
                self.assertNotEqual(threads[0], threading.get_ident())
                self.assertFalse(release.is_set())
                heartbeat = []
                GLib.idle_add(lambda: heartbeat.append(True) and False)
                select(app.controls['vaapi.debug'], 'true')
                self.assertEqual(heartbeat, [True])
                self.assertTrue(app.editor.dirty)
                app.confirm = lambda *_: False
                app.quit();drain()
                self.assertFalse(app.quitting)
                self.assertTrue(app.window.get_visible())
                release.set()
                settle(lambda: app.installation_status.get_text() == 'Finished')
                self.assertTrue(app.editor.dirty)
                self.assertFalse(self.path.exists())
        finally:
            release.set()

    def test_refresh_burst_coalesces_and_never_publishes_superseded_results(self):
        entered = [threading.Event(), threading.Event()]
        release = [threading.Event(), threading.Event()]
        calls = []
        def inspect():
            index = len(calls)
            calls.append(index)
            entered[index].set()
            release[index].wait(10)
            return {'scope': 'result-' + str(index), 'readers': [{'state': 'declared-v1'}]}
        try:
            with mock.patch.object(CORE, 'inspect_installation', side_effect=inspect), \
                    mock.patch.object(CORE, 'installation_text', side_effect=lambda r: r['scope']):
                app = self.app()
                self.assertTrue(entered[0].wait(3))
                select(app.player_controls['gstreamer.crystalhddec.scale_width'], 'custom')
                app.player_entries['gstreamer.crystalhddec.scale_width'].set_text('unfinished')
                draft, pending = app.editor.draft, dict(app.pending_player)
                for _ in range(25):
                    app.installation_refresh.clicked()
                drain()
                self.assertEqual(calls, [0])
                release[0].set()
                settle(entered[1].is_set)
                self.assertNotEqual(app.installation_status.get_text(), 'result-0')
                self.assertEqual(calls, [0, 1])
                release[1].set()
                settle(lambda: app.installation_status.get_text() == 'result-1')
                self.assertIs(app.editor.draft, draft)
                self.assertEqual(app.pending_player, pending)
                self.assertEqual(app.player_entries['gstreamer.crystalhddec.scale_width'].get_text(), 'unfinished')
                self.assertFalse(self.path.exists())
        finally:
            for event in release:event.set()

    def test_destroy_during_inspection_does_not_wait_or_publish_to_dead_widgets(self):
        entered, release, finished = threading.Event(), threading.Event(), threading.Event()
        def inspect():
            entered.set()
            release.wait(10)
            finished.set()
            return {'readers': [{'state': 'declared-v1'}]}
        try:
            with mock.patch.object(CORE, 'inspect_installation', side_effect=inspect), \
                    mock.patch.object(CORE, 'installation_text', return_value='Too late'):
                app = self.app()
                self.assertTrue(entered.wait(3))
                worker = app._inspection_worker
                self.assertTrue(worker.daemon)
                with mock.patch.object(app, 'update_preview') as update:
                    app.window.destroy();drain()
                    self.assertTrue(app._inspection_closed)
                    self.assertEqual(app._inspection_source, 0)
                    self.assertFalse(finished.is_set())
                    release.set()
                    worker.join(3)
                    self.assertFalse(worker.is_alive())
                    drain()
                    update.assert_not_called()
                self.assertNotEqual(app.installation_status.get_text(), 'Too late')
        finally:
            release.set()

    def test_inspection_failure_is_redacted_and_refresh_can_retry(self):
        with mock.patch.object(CORE, 'inspect_installation', side_effect=OSError('private-error-detail')):
            app = self.app()
            settle(lambda: 'failed' in app.installation_status.get_text())
        self.assertNotIn('private-error-detail', app.installation_status.get_text())
        with mock.patch.object(CORE, 'inspect_installation', return_value={'readers': [{'state': 'declared-v1'}]}), \
                mock.patch.object(CORE, 'installation_text', return_value='Recovered'):
            app.installation_refresh.clicked()
            settle(lambda: app.installation_status.get_text() == 'Recovered')
        self.assertFalse(self.path.exists())

    def test_hide_to_tray_keeps_inspection_alive(self):
        entered, release = threading.Event(), threading.Event()
        def inspect():
            entered.set();release.wait(10)
            return {'readers': [{'state': 'declared-v1'}]}
        try:
            with mock.patch.object(CORE, 'inspect_installation', side_effect=inspect), \
                    mock.patch.object(CORE, 'installation_text', return_value='Hidden result'):
                app = self.app()
                self.assertTrue(entered.wait(3))
                app.tray_connected = True
                app.close_window();drain()
                self.assertFalse(app.window.get_visible())
                release.set()
                settle(lambda: app.installation_status.get_text() == 'Hidden result')
                self.assertFalse(app._inspection_closed)
        finally:
            release.set()

    def test_worker_start_failure_is_retryable_without_leaving_a_poll_source(self):
        with mock.patch.object(UI.threading.Thread, 'start', side_effect=RuntimeError('private-start-detail')):
            app = self.app()
        self.assertIn('failed', app.installation_status.get_text())
        self.assertNotIn('private-start-detail', app.installation_status.get_text())
        self.assertIsNone(app._inspection_worker)
        self.assertEqual(app._inspection_source, 0)
        with mock.patch.object(CORE, 'inspect_installation', return_value={'readers': [{'state': 'unknown'}]}), \
                mock.patch.object(CORE, 'installation_text', return_value='Retried'):
            app.installation_refresh.clicked()
            settle(lambda: app.installation_status.get_text() == 'Retried')
        self.assertTrue(app.readers_unknown)
        self.assertIn('Some inspected components', app.status.get_text())

    def test_formatting_failure_keeps_last_report_caveat_and_can_retry(self):
        with mock.patch.object(CORE, 'inspect_installation', return_value={'readers': [{'state': 'unknown'}]}), \
                mock.patch.object(CORE, 'installation_text', return_value='Unknown reader'):
            app = self.app()
            settle(lambda: app.installation_status.get_text() == 'Unknown reader')
        with mock.patch.object(CORE, 'inspect_installation', return_value={'readers': [{'state': 'declared-v1'}]}), \
                mock.patch.object(CORE, 'installation_text', side_effect=ValueError('private-format-detail')):
            app.installation_refresh.clicked()
            settle(lambda: 'failed' in app.installation_status.get_text())
        self.assertNotIn('private-format-detail', app.installation_status.get_text())
        self.assertTrue(app.readers_unknown)
        self.assertFalse(self.path.exists())

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
            for name in ('_inspection_worker', '_freshness_worker'):
                worker = getattr(app, name)
                if worker is not None:
                    worker.join(3)
                    self.assertFalse(worker.is_alive(), 'test worker did not retire')
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
        self.assert_label_relation(combo, Gdk.KEY_d)
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

    def test_reopen_reports_external_change_and_preserves_drafts_until_reload(self):
        app=self.app()
        select(app.controls['vaapi.debug'],'true')
        select(app.player_controls['gstreamer.crystalhddec.scale_width'],'custom')
        app.player_entries['gstreamer.crystalhddec.scale_width'].set_text('unfinished')
        draft,pending=app.editor.draft,dict(app.pending_player)
        self.write(HEADER+'[settings]\nvaapi.trace=true\n')

        app.show_settings();drain()
        settle(lambda:'changed outside' in app.status.get_text())
        self.assertIs(app.editor.draft,draft)
        self.assertEqual(app.pending_player,pending)
        self.assertFalse(app.save_button.get_sensitive())
        self.assertFalse(app.reset_button.get_sensitive())

        app.confirm=lambda *_:False
        app.reload_button.clicked();drain()
        self.assertIs(app.editor.draft,draft)
        self.assertEqual(app.pending_player,pending)
        self.assertIn('changed outside',app.status.get_text())

        app.confirm=lambda *_:True
        app.reload_button.clicked();drain()
        self.assertEqual(app.editor.draft.settings,{'vaapi.trace':True})
        self.assertFalse(app.pending_player)
        self.assertNotIn('changed outside',app.status.get_text())

    def test_slow_reopen_freshness_check_does_not_block_edits(self):
        entered,release=threading.Event(),threading.Event()
        def inspect(*_args):
            entered.set();release.wait(10)
            return 'current'
        try:
            with mock.patch.object(UI,'read_saved_file_state',side_effect=inspect):
                app=self.app();app.show_settings();drain()
                self.assertTrue(entered.wait(3))
                self.assertFalse(app.save_button.get_sensitive())
                self.assertFalse(app.reset_button.get_sensitive())
                heartbeat=[]
                GLib.idle_add(lambda:heartbeat.append(True) and False)
                select(app.controls['vaapi.debug'],'true')
                self.assertEqual(heartbeat,[True])
                self.assertTrue(app.editor.dirty)
                release.set()
                settle(lambda:app._freshness_worker is None)
                self.assertTrue(app.editor.dirty)
        finally:
            release.set()

    def test_tray_loss_checks_files_before_allowing_writes(self):
        app=self.app(no_tray=False)
        app.tray_connected=True
        app.close_window();drain()
        self.assertFalse(app.window.get_visible())
        self.write(HEADER+'[settings]\nvaapi.debug=true\n')
        with mock.patch.object(UI,'tray_host_present',return_value=False):
            app.tray_connection_changed(None,False);drain()
        self.assertTrue(app.window.get_visible())
        settle(lambda:'changed outside' in app.status.get_text())
        self.assertFalse(app.save_button.get_sensitive())
        self.assertFalse(app.reset_button.get_sensitive())

    def test_reload_discards_an_older_freshness_result(self):
        entered,release=threading.Event(),threading.Event()
        calls=[]
        def inspect(*_args):
            calls.append(len(calls))
            if len(calls)==1:
                entered.set();release.wait(10)
                return 'changed'
            return 'current'
        try:
            with mock.patch.object(UI,'read_saved_file_state',side_effect=inspect):
                app=self.app();app.show_settings();drain()
                self.assertTrue(entered.wait(3))
                app.reload_button.clicked();drain()
                release.set()
                settle(lambda:len(calls)==2 and app._freshness_worker is None)
                self.assertEqual(app.external_freshness,'current')
                self.assertNotIn('changed outside',app.status.get_text())
        finally:
            release.set()

    def test_unsafe_freshness_result_blocks_writes_and_can_retry(self):
        self.write(HEADER)
        real=UI.read_saved_file_state
        calls=[]
        def inspect(*args):
            calls.append(len(calls))
            if len(calls)==1:
                raise CORE.SchemaError('private detail')
            return real(*args)
        with mock.patch.object(UI,'read_saved_file_state',side_effect=inspect):
            app=self.app()
            app.editor.set_literal('vaapi.debug','true')
            app.show_settings();drain()
            settle(lambda:app.external_freshness=='unreadable')
            self.assertNotIn('private detail',app.status.get_text())
            self.assertFalse(app.save_button.get_sensitive())
            self.assertFalse(app.reset_button.get_sensitive())
            app.show_settings();drain()
            settle(lambda:app.external_freshness=='current')
            self.assertTrue(app.save_button.get_sensitive())
            self.assertTrue(app.reset_button.get_sensitive())

    def test_modal_confirm_rechecks_freshness_before_reset_and_save(self):
        self.write(HEADER+'[settings]\nvaapi.trace=true\n')
        app=self.app()
        original=self.path.read_bytes()
        def stale_reset(*_args):
            app.external_freshness='changed'
            return True
        app.confirm=stale_reset
        app.reset()
        self.assertEqual(self.path.read_bytes(),original)
        self.assertIn('changed outside',app.status.get_text())

        self.write(HEADER+'preset="low-memory"\n')
        experimental=self.app()
        select(experimental.controls['vaapi.debug'],'true')
        original=self.path.read_bytes()
        def stale_save(*_args):
            experimental.external_freshness='changed'
            return True
        experimental.confirm=stale_save
        experimental.save()
        self.assertEqual(self.path.read_bytes(),original)
        self.assertTrue(experimental.editor.dirty)
        self.assertIn('changed outside',experimental.status.get_text())

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

    def test_player_validation_description_is_localized_and_changes_only_with_error(self):
        translations = {
            'Enter 0 for native width, or an even number from 128 to 1918.': 'Translated card-width validation',
            'Enter WIDTHxHEIGHT: positive even dimensions, each at most 4096, total at most 4096×2160.': 'Translated output-size validation',
        }
        with mock.patch.object(UI, '_', side_effect=lambda text: translations.get(text, text)):
            app = self.app()
            for key, invalid, valid in (
                    ('gstreamer.crystalhddec.scale_width', '127', '128'),
                    ('player.software_output_size', '1921x1080', '1920x1080')):
                with self.subTest(key=key):
                    entry = app.player_entries[key]
                    accessible = entry.get_accessible()
                    changes = []
                    handler = accessible.connect('notify::accessible-description',
                                                 lambda obj, _spec: changes.append(obj.get_description()))
                    try:
                        self.assertEqual(accessible.get_description(), '')
                        select(app.player_controls[key], 'custom')
                        entry.set_text(invalid);drain()
                        error = app.player_error_text(key)
                        self.assertIn(error, translations.values())
                        self.assertEqual(app.player_errors[key].get_text(), error)
                        self.assertEqual(accessible.get_description(), error)
                        self.assertEqual(changes, [error])
                        self.assertFalse(app.save_button.get_sensitive())
                        app.update_preview();drain()
                        self.assertEqual(changes, [error])
                        entry.set_text(valid);drain()
                        self.assertEqual(app.player_errors[key].get_text(), '')
                        self.assertEqual(accessible.get_description(), '')
                        self.assertEqual(changes, [error, ''])
                        self.assertTrue(app.save_button.get_sensitive())
                    finally:
                        accessible.disconnect(handler)
        self.assertFalse(self.path.exists())

    def test_player_validation_description_follows_editing_scope_and_cleared_override(self):
        for key, invalid in (('gstreamer.crystalhddec.scale_width', '127'),
                             ('player.software_output_size', '1921x1080')):
            with self.subTest(key=key):
                app = self.app()
                app.prompt_profile_name = lambda: 'desk'
                app.new_profile_button.clicked();drain()
                select(app.player_controls[key], 'custom')
                entry = app.player_entries[key]
                entry.set_text(invalid);drain()
                error = app.player_error_text(key)
                self.assertEqual(entry.get_accessible().get_description(), error)
                select(app.layer, '')
                self.assertEqual(entry.get_accessible().get_description(), '')
                self.assertEqual(app.player_errors[key].get_text(), '')
                self.assertEqual(entry.get_text(), '')
                self.assertFalse(entry.get_sensitive())
                self.assertFalse(app.save_button.get_sensitive())
                self.assertEqual(app.pending_player[('desk', key)], invalid)
                select(app.layer, 'desk')
                self.assertEqual(entry.get_accessible().get_description(), error)
                self.assertEqual(app.player_errors[key].get_text(), error)
                self.assertEqual(entry.get_text(), invalid)
                select(app.player_controls[key], 'unset')
                self.assertEqual(entry.get_accessible().get_description(), '')
                self.assertEqual(app.player_errors[key].get_text(), '')
                self.assertFalse(app.pending_player)
                self.assertTrue(app.save_button.get_sensitive())
                self.assertFalse(self.path.exists())

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
