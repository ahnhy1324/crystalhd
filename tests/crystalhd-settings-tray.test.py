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
    desktop_process = None
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
        with tempfile.TemporaryDirectory(prefix='crystalhd-desktop-instance-') as home:
            environment=dict(os.environ,HOME=home,XDG_CONFIG_HOME=str(Path(home)/'xdg'))
            command=[sys.executable,'-B',str(ROOT/'scripts/crystalhd-settings.py'),'--tray']
            desktop_process=subprocess.Popen(command,env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            app_id='io.github.ahnhy1324.CrystalHD.Settings'
            def owner():
                return bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus',
                                     'NameHasOwner',GLib.Variant('(s)',(app_id,)),None,
                                     Gio.DBusCallFlags.NO_AUTO_START,1000,None).unpack()[0]
            settle(owner)
            pid=bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus',
                              'GetConnectionUnixProcessID',GLib.Variant('(s)',(app_id,)),None,
                              Gio.DBusCallFlags.NO_AUTO_START,1000,None).unpack()[0]
            assert pid==desktop_process.pid
            second=subprocess.run(command,env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=8)
            assert second.returncode==0,second.stderr.decode('utf-8',errors='replace')
            assert desktop_process.poll() is None,'primary exited after activation'
            assert owner(),'desktop instance lost its bus name'
            assert not (Path(home)/'xdg/crystalhd/config.ini').exists(),'desktop startup wrote defaults'
            isolated=subprocess.Popen(command+['--user-config',str(Path(home)/'other.ini')],
                                      env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                try:
                    isolated.wait(timeout=1)
                    raise AssertionError('explicit-path editor was redirected to the desktop instance')
                except subprocess.TimeoutExpired:
                    pass
                assert owner(),'isolated editor replaced the desktop owner'
                assert not (Path(home)/'other.ini').exists()
            finally:
                isolated.terminate();isolated.communicate(timeout=5)
            print('Desktop single-instance activation without startup write PASS')
    finally:
        if app is not None:
            app.quitting = True
            app.window.destroy()
        if desktop_process is not None:
            desktop_process.terminate()
            desktop_process.communicate(timeout=5)
        process.terminate()
        process.communicate(timeout=5)


if __name__ == '__main__': run()
