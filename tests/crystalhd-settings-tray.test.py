#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Private-bus host-lifetime regression; never stop a real desktop panel.

Run through dbus-run-session, with DISPLAY supplied by the GTK test runner.
The synthetic watcher lives in a separate process so a synchronous client
property read cannot deadlock the loop that implements that property.
"""
import importlib.util
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
from unittest import mock

import gi
from gi.repository import Gio, GLib

WATCHER = 'org.kde.StatusNotifierWatcher'
OBJECT = '/StatusNotifierWatcher'


def serve_watcher():
    xml = '''<node><interface name="org.kde.StatusNotifierWatcher">
    <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
    <signal name="StatusNotifierHostRegistered"/>
    <signal name="StatusNotifierHostUnregistered"/>
    <method name="SetHost"><arg type="b" direction="in"/></method>
    <method name="Stop"/>
    </interface></node>'''
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    info = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]
    loop = GLib.MainLoop()
    state = [True]

    def method(_bus, _sender, _path, _interface, name, parameters, invocation):
        if name == 'SetHost':
            state[0] = parameters.unpack()[0]
            signal = 'StatusNotifierHostRegistered' if state[0] else 'StatusNotifierHostUnregistered'
            bus.emit_signal(None, OBJECT, WATCHER, signal, None)
        elif name == 'Stop':
            GLib.idle_add(loop.quit)
        invocation.return_value(None)

    def get_property(*_args):
        return GLib.Variant('b', state[0])

    registration = bus.register_object(OBJECT, info, method, get_property, None)
    owner = Gio.bus_own_name_on_connection(bus, WATCHER, Gio.BusNameOwnerFlags.NONE,
                                          lambda *_: print('READY', flush=True), None)
    loop.run()
    bus.unregister_object(registration)
    Gio.bus_unown_name(owner)


if sys.argv[1:] == ['--watcher']:
    serve_watcher()
    sys.exit(0)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('settings_tray', str(ROOT/'scripts/crystalhd-settings.py'))
UI = importlib.util.module_from_spec(spec)
spec.loader.exec_module(UI)
Gtk, Gdk, _Gio, _GLib = UI.load_gtk()
if not Gtk.init_check([])[0]:
    raise RuntimeError('tray tests need a display (use xvfb-run)')


class IndicatorStub(object):
    """A watcher-connected indicator: it deliberately has no panel-loss signal."""
    def __init__(self):
        self.callback = None

    def connect(self, _name, callback): self.callback = callback
    def set_icon_theme_path(self, *_args): pass
    def set_icon_full(self, *_args): pass
    def set_title(self, *_args): pass
    def set_menu(self, *_args): pass
    def set_status(self, value):
        if value == 1 and self.callback:
            self.callback(self, True)


def settle(predicate):
    deadline = time.monotonic()+2
    while not predicate() and time.monotonic() < deadline:
        while Gtk.events_pending(): Gtk.main_iteration_do(False)
        time.sleep(.01)
    assert predicate(), 'host lifetime transition was not observed'


def run():
    process = subprocess.Popen([sys.executable, '-B', str(Path(__file__).resolve()), '--watcher'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               universal_newlines=True)
    app = None
    try:
        assert select.select([process.stdout], [], [], 5)[0], 'private watcher startup timed out'
        assert process.stdout.readline().strip() == 'READY'
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

        def host(value):
            bus.call_sync(WATCHER, OBJECT, WATCHER, 'SetHost', GLib.Variant('(b)',(value,)),
                          None, Gio.DBusCallFlags.NO_AUTO_START, 1000, None)

        module = SimpleNamespace(Indicator=SimpleNamespace(new=lambda *_: IndicatorStub()),
                                 IndicatorCategory=SimpleNamespace(APPLICATION_STATUS=0),
                                 IndicatorStatus=SimpleNamespace(ACTIVE=1, PASSIVE=0))
        with tempfile.TemporaryDirectory(prefix='crystalhd-private-tray-') as folder:
            path = Path(folder)/'config.ini'
            with mock.patch.object(UI, 'load_indicator', return_value=module):
                app = UI.SettingsWindow(UI.Editor(UI.load_core(), str(path), None, {'HOME':folder}),
                                        (Gtk,Gdk,Gio,GLib))
            app.window.show_all()
            settle(lambda: app.tray_connected)
            app.close_window()
            assert not app.window.get_visible()
            host(False)  # Watcher keeps its bus name; only the display host leaves.
            settle(lambda: not app.tray_connected and app.window.get_visible())
            assert app.indicator is not None
            host(True)
            settle(lambda: app.tray_connected)
            app.close_window()
            assert not app.window.get_visible()
            host(False)
            settle(lambda: not app.tray_connected and app.window.get_visible())
            assert not path.exists(), 'host lifecycle unexpectedly wrote user defaults'
            app.quit()
            app = None
            print('Private-bus panel loss/recovery with persistent watcher PASS')
    finally:
        if app is not None:
            app.quitting = True
            app.window.destroy()
        process.terminate()
        process.communicate(timeout=5)


if __name__ == '__main__': run()
