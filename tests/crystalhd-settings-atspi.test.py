#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Explicit cross-process GTK/AT-SPI validation check; never opens CrystalHD.

Supply an existing X display (an owned Xvfb is sufficient). This script starts
its own dbus-run-session and a direct GUI child using private configuration:

  python3 -B tests/crystalhd-settings-atspi.test.py --source=. --report=NEW.json

--source accepts a checkout or a private installed prefix/root. Use the actual
installed catalogue and LANGUAGE=ko for Korean. --expect-missing-description
records the frozen-base defect as EXPECTED_DEFECT_REPRODUCED, not a UI PASS.
This verifies remote descriptions and persistent INVALID_ENTRY queries/events,
including a fresh observer after an error. It does not verify spoken Orca.
"""
import argparse
from collections import Counter
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time


KEYS = ('gstreamer.crystalhddec.scale_width', 'player.software_output_size')
VALUES = (('127', '128'), ('1921x1080', '1920x1080'))
EVENT = 'object:property-change:accessible-description'
INVALID_EVENT = 'object:state-changed:invalid-entry'
SCRIPT = Path(__file__).resolve()
sys.dont_write_bytecode = True


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def ui_path(source):
    candidates = (source / 'scripts/crystalhd-settings.py',
                  source / 'bin/crystalhd-settings',
                  source / 'usr/bin/crystalhd-settings',
                  source / 'crystalhd-settings')
    matches = [p for p in candidates if p.is_file()]
    require(len(matches) == 1, '--source must identify exactly one settings script')
    return matches[0]


def child(args):
    """Real UI plus bounded JSON-line actions, all on its GTK main thread."""
    path = ui_path(args.source)
    loader = importlib.machinery.SourceFileLoader('settings_atspi_ui', str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    ui = importlib.util.module_from_spec(spec)
    loader.exec_module(ui)
    core = ui.load_core()
    modules = ui.load_gtk()
    Gtk, _Gdk, _Gio, GLib = modules
    require(Gtk.init_check([])[0], 'GUI child requires an X display')
    app = ui.SettingsWindow(ui.Editor(core, str(args._config), None,
                                     {'HOME': str(args._config.parent)}),
                            modules, no_tray=True)
    require(app.editor.load_error is None, 'private editor failed to load')
    loop = GLib.MainLoop()
    workers, failure, quit_id = [], [], []

    def remember_workers():
        for name in ('_inspection_worker', '_freshness_worker'):
            worker = getattr(app, name)
            if worker is not None and worker not in workers:
                workers.append(worker)

    def snapshot():
        remember_workers()
        return {'pid': os.getpid(), 'title': app.window.get_title(),
                'scope': app.editing_profile,
                'save_sensitive': app.save_button.get_sensitive(),
                'config_exists': args._config.exists(),
                'pending': [[scope, key, value] for (scope, key), value
                            in app.pending_player.items()],
                'entries': {key: {'name': app.player_entries[key].get_accessible().get_name(),
                                  'error': app.player_errors[key].get_text(),
                                  'text': app.player_entries[key].get_text(),
                                  'sensitive': app.player_entries[key].get_sensitive()}
                            for key in KEYS}}

    def emit(value):
        print(json.dumps(value, ensure_ascii=False), flush=True)

    def choose(combo, token):
        for row in combo.get_model():
            if row[0] == token:
                require(row[2], 'disabled GUI choice requested')
                combo.set_active_iter(row.iter)
                return
        raise AssertionError('GUI choice not found: ' + token)

    def on_input(_source, condition):
        identifier = None
        try:
            if condition & GLib.IO_HUP:
                loop.quit()
                return False
            line = sys.stdin.readline(4097)
            require(line.endswith('\n') and len(line) <= 4096, 'invalid IPC line')
            request = json.loads(line)
            identifier = request['id']
            action = request['action']
            if action == 'quit':
                quit_id.append(identifier)
                loop.quit()
                return False
            if action == 'edit':
                key = request['key']
                require(key in KEYS, 'unexpected Player key')
                app.notebook.set_current_page(0)
                choose(app.player_controls[key], 'custom')
                app.player_entries[key].set_text(request['value'])
                app.player_entries[key].grab_focus()
            elif action == 'unset':
                require(request['key'] in KEYS, 'unexpected Player key')
                choose(app.player_controls[request['key']], 'unset')
            elif action == 'new-profile':
                app.prompt_profile_name = lambda: 'desk'
                app.new_profile()
            elif action == 'layer':
                require(request['value'] in ('', 'desk'), 'unexpected editing layer')
                choose(app.layer, request['value'])
            elif action == 'preview':
                app.update_preview()
            else:
                raise AssertionError('unexpected IPC action')
            emit({'id': identifier, 'state': snapshot()})
            return True
        except Exception as error:
            failure.append(type(error).__name__ + ': ' + str(error))
            emit({'id': identifier, 'error': failure[-1]})
            loop.quit()
            return False

    def expired():
        failure.append('GUI child watchdog expired')
        loop.quit()
        return False

    app.window.show_all()
    app.window.present()
    remember_workers()
    emit({'ready': True, 'state': snapshot(), 'providers': [
        {'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
        for p in (path, Path(core.__file__))]})
    watch = GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, on_input)
    timer = GLib.timeout_add_seconds(45, expired)
    try:
        loop.run()
    finally:
        # cancel_installation clears its worker field: capture references first.
        remember_workers()
        app.quitting = True
        app.window.destroy()
        for worker in workers:
            worker.join(5)
            if worker.is_alive():
                failure.append('GUI background worker did not retire')
        for identifier in (timer, watch):
            source = GLib.MainContext.default().find_source_by_id(identifier)
            if source is not None:
                source.destroy()
    result = {'id': quit_id[0] if quit_id else None, 'closed': True,
              'workers_retired': not any(w.is_alive() for w in workers),
              'config_exists': args._config.exists(), 'errors': failure}
    emit(result)
    return 1 if failure else 0


def parent(args):
    descriptor = os.open(str(args.report), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    report = {'schema': 'crystalhd.settings-atspi.v1', 'status': 'FAIL',
              'source': str(args.source), 'private_session_bus': True,
              'observer_sha256': hashlib.sha256(SCRIPT.read_bytes()).hexdigest(),
              'expect_missing_description': args.expect_missing_description,
              'expect_missing_invalid_state': args.expect_missing_invalid_state,
              'scope': 'Cross-process descriptions and queried INVALID_ENTRY states/events; no spoken Orca claim',
              'actions': [], 'cleanup': {}, 'first_failure': None}
    process = listener = None
    registered = False
    started = time.monotonic()
    deadline = started + 45
    buffer = bytearray()
    sequence = 0
    event_errors, events = [], []
    with os.fdopen(descriptor, 'w', encoding='utf-8') as output, \
            tempfile.TemporaryDirectory(prefix='crystalhd-settings-atspi-') as folder:
        private = Path(folder)
        config = private / 'config.ini'
        stderr_path = private / 'gui-stderr.log'
        stderr = stderr_path.open('wb')
        try:
            import gi
            gi.require_version('Atspi', '2.0')
            from gi.repository import Atspi, Gio, GLib
            # Replace an inherited accessibility-bus address with this session's.
            bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
            address = bus.call_sync('org.a11y.Bus', '/org/a11y/bus', 'org.a11y.Bus',
                                    'GetAddress', None, None, Gio.DBusCallFlags.NONE,
                                    5000, None).unpack()[0]
            os.environ['AT_SPI_BUS_ADDRESS'] = address
            require(Atspi.init() in (0, 1), 'AT-SPI registry initialization failed')
            Atspi.set_timeout(1000, 1000)
            context = GLib.MainContext.default()

            def pump(seconds=0):
                until = time.monotonic() + seconds
                while True:
                    require(time.monotonic() < deadline, 'parent watchdog expired')
                    for _ in range(64):
                        if not context.pending():
                            break
                        context.iteration(False)
                    if time.monotonic() >= until:
                        return
                    time.sleep(.01)

            def read_message(seconds=5):
                until = min(deadline, time.monotonic() + seconds)
                while b'\n' not in buffer:
                    pump()
                    require(time.monotonic() < until, 'GUI IPC reply timed out')
                    if select.select([process.stdout], [], [], .02)[0]:
                        chunk = os.read(process.stdout.fileno(), 65536)
                        require(chunk, 'GUI closed its IPC stream')
                        buffer.extend(chunk)
                        require(len(buffer) <= 65536, 'GUI IPC reply exceeded bound')
                line, _, rest = buffer.partition(b'\n')
                buffer[:] = rest
                value = json.loads(line.decode('utf-8'))
                require('error' not in value, 'GUI action failed: ' + str(value.get('error')))
                return value

            def request(action, **fields):
                nonlocal sequence
                sequence += 1
                process.stdin.write((json.dumps(dict(fields, id=sequence, action=action)) + '\n').encode())
                process.stdin.flush()
                value = read_message(12 if action == 'quit' else 5)
                require(value.get('id') == sequence, 'GUI IPC sequence mismatch')
                return value

            environment = dict(os.environ, XDG_CONFIG_HOME=str(private / 'xdg-config'),
                               GIO_USE_VFS='local', GTK_USE_PORTAL='0',
                               GSETTINGS_BACKEND='memory')
            environment.pop('NO_AT_BRIDGE', None)
            process = subprocess.Popen([sys.executable, '-B', str(SCRIPT),
                                        '--source', str(args.source), '--_child',
                                        '--_config', str(config)], stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=stderr, env=environment)
            report['child_pid'] = process.pid
            ready = read_message()
            require(ready.get('ready') and ready['state']['pid'] == process.pid,
                    'GUI readiness is not from the direct child')
            report['providers'] = ready['providers']
            report['title'] = ready['state']['title']
            desktop = Atspi.get_desktop(0)
            app = None
            bind_until = min(deadline, time.monotonic() + 8)
            while app is None and time.monotonic() < bind_until:
                pump(.02)
                count = desktop.get_child_count()
                require(0 <= count <= 32, 'unexpected private-bus application population')
                for index in range(count):
                    candidate = desktop.get_child_at_index(index)
                    if candidate.get_process_id() == process.pid:
                        require(app is None, 'multiple AT-SPI applications share GUI PID')
                        app = candidate
            require(app is not None, 'direct GUI child did not register with AT-SPI')
            names = {value['name']: key for key, value in ready['state']['entries'].items()}
            require(len(names) == len(KEYS), 'GUI entry names are not distinct')
            entries, queue, visited = {}, [app], 0
            while queue:
                pump()
                node = queue.pop(0)
                visited += 1
                require(visited <= 512, 'GUI accessibility tree exceeded bound')
                name = node.get_name()
                if name in names and node.get_role() in (Atspi.Role.ENTRY, Atspi.Role.TEXT):
                    require(node.get_editable_text_iface() is not None, 'named entry lacks EditableText')
                    key = names[name]
                    require(key not in entries, 'duplicate named editable entry')
                    require(node.get_process_id() == process.pid, 'entry belongs to another process')
                    entries[key] = node
                count = node.get_child_count()
                require(0 <= count <= 512, 'GUI accessible child count exceeded bound')
                queue.extend(node.get_child_at_index(index) for index in range(count))
            require(set(entries) == set(KEYS), 'did not find both named editable entries')
            paths = {str(node.path): key for key, node in entries.items()}
            report['entry_roles'] = {key: node.get_role_name() for key, node in entries.items()}

            invalid_events = []

            def on_event(event, *_data):
                try:
                    key = paths.get(str(event.source.path))
                    if key is None or event.source.get_process_id() != process.pid:
                        return
                    if event.type == INVALID_EVENT:
                        invalid_events.append({'key': key, 'enabled': bool(event.detail1)})
                        return
                    value = event.any_data
                    if hasattr(value, 'get_value'):
                        value = value.get_value()
                    require(isinstance(value, str), 'description event is not string-valued')
                    events.append({'key': key, 'description': value, 'type': event.type})
                except Exception as error:
                    event_errors.append(type(error).__name__ + ': ' + str(error))

            listener = Atspi.EventListener.new(on_event)
            require(listener.register(EVENT), 'AT-SPI event subscription failed')
            require(listener.register(INVALID_EVENT), 'AT-SPI invalid-state subscription failed')
            registered = True
            pump(.2)
            events.clear()
            previous = {key: '' for key in KEYS}
            previous_invalid = {key: False for key in KEYS}

            def remote_descriptions():
                result = {}
                for key, node in entries.items():
                    node.clear_cache()
                    result[key] = node.get_description()
                return result

            require(remote_descriptions() == previous, 'initial entry descriptions are not empty')

            def remote_invalid_states():
                result = {}
                for key, node in entries.items():
                    node.clear_cache()
                    result[key] = node.get_state_set().contains(Atspi.StateType.INVALID_ENTRY)
                return result

            require(remote_invalid_states() == previous_invalid, 'initial entry invalid states are not clear')

            def step(action, **fields):
                nonlocal previous, previous_invalid
                pump(.1)
                offset = len(events)
                invalid_offset = len(invalid_events)
                state = request(action, **fields)['state']
                require(state['pid'] == process.pid and not state['config_exists'],
                        'GUI identity changed or private configuration was written')
                expected = {key: ('' if args.expect_missing_description else state['entries'][key]['error'])
                            for key in KEYS}
                expected_invalid = {key: bool(state['entries'][key]['error']) and not (
                    args.expect_missing_description or args.expect_missing_invalid_state) for key in KEYS}
                until = min(deadline, time.monotonic() + 3)
                observed = remote_descriptions()
                while observed != expected and time.monotonic() < until:
                    pump(.02)
                    observed = remote_descriptions()
                require(observed == expected, 'remote description differs from visible validation')
                observed_invalid = remote_invalid_states()
                while observed_invalid != expected_invalid and time.monotonic() < until:
                    pump(.02)
                    observed_invalid = remote_invalid_states()
                require(observed_invalid == expected_invalid, 'queried INVALID_ENTRY differs from validation')
                pump(.25)
                require(not event_errors, 'AT-SPI event decoding failed: ' + str(event_errors))
                expected_events = Counter((key, text) for key, text in expected.items()
                                          if text != previous[key])
                actual_events = Counter((event['key'], event['description']) for event in events[offset:])
                require(actual_events == expected_events, 'missing, duplicate or stale description event')
                expected_state_events = Counter((key, enabled) for key, enabled in expected_invalid.items()
                                                if enabled != previous_invalid[key])
                actual_state_events = Counter((event['key'], event['enabled'])
                                              for event in invalid_events[invalid_offset:])
                require(actual_state_events == expected_state_events,
                        'missing, duplicate or stale invalid-state event')
                report['actions'].append({'action': action, 'fields': fields, 'state': state,
                                          'remote_descriptions': observed, 'events': events[offset:],
                                          'queried_invalid_states': observed_invalid,
                                          'invalid_events': invalid_events[invalid_offset:]})
                previous = expected
                previous_invalid = expected_invalid
                return state

            for key, (invalid, valid) in zip(KEYS, VALUES):
                state = step('edit', key=key, value=invalid)
                require(state['entries'][key]['error'] and not state['save_sensitive'],
                        'invalid input did not produce visible validation and block Save')
                if key == KEYS[0]:
                    observer = subprocess.Popen([
                        sys.executable, '-B', str(SCRIPT), '--source', str(args.source),
                        '--_fresh-pid', str(process.pid), '--_fresh-name', state['entries'][key]['name']],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        fresh_output, fresh_errors = observer.communicate(timeout=5)
                    except subprocess.TimeoutExpired:
                        observer.terminate()
                        observer.communicate(timeout=5)
                        raise AssertionError('fresh AT-SPI observer timed out')
                    require(observer.returncode == 0, 'fresh observer failed: ' + fresh_errors[:512])
                    fresh = json.loads(fresh_output)
                    require(fresh['pid'] == process.pid and fresh['name'] == state['entries'][key]['name'],
                            'fresh observer target identity differs')
                    require(fresh['invalid'] == previous_invalid[key], 'fresh observer queried stale invalid state')
                    report['fresh_observer'] = fresh
                step('preview')
                state = step('edit', key=key, value=valid)
                require(not state['entries'][key]['error'] and state['save_sensitive'],
                        'corrected input did not restore Save')
            # Software resizing depends on a nonzero card width. Keep the
            # corrected width until both correction checks have completed.
            for key in reversed(KEYS):
                step('unset', key=key)
            require(step('new-profile')['scope'] == 'desk', 'profile was not selected for editing')
            for key, (invalid, _valid) in zip(KEYS, VALUES):
                state = step('edit', key=key, value=invalid)
                require(state['entries'][key]['error'] and not state['save_sensitive'],
                        'invalid profile input was not retained')
                state = step('layer', value='')
                require(state['scope'] is None and not state['entries'][key]['error']
                        and not state['entries'][key]['sensitive'] and not state['save_sensitive']
                        and ['desk', key, invalid] in state['pending'],
                        'scope switch lost invalid draft or exposed its error on another layer')
                state = step('layer', value='desk')
                require(state['entries'][key]['text'] == invalid and state['entries'][key]['error'],
                        'returning to profile did not restore its invalid text')
                state = step('unset', key=key)
                require(not state['entries'][key]['error'] and not state['pending']
                        and state['save_sensitive'], 'clearing override left stale validation')
                step('preview')
            report['status'] = ('EXPECTED_DEFECT_REPRODUCED' if args.expect_missing_description else
                                'EXPECTED_INVALID_STATE_DEFECT_REPRODUCED' if args.expect_missing_invalid_state
                                else 'PASS_SCOPED_ATSPI_VALIDATION')
        except Exception as error:
            report['first_failure'] = type(error).__name__ + ': ' + str(error)
        finally:
            # Give ordinary teardown its own bound even after a test timeout.
            deadline = max(deadline, time.monotonic() + 12)
            if registered:
                try:
                    require(listener.deregister(EVENT), 'AT-SPI listener did not deregister')
                    require(listener.deregister(INVALID_EVENT), 'AT-SPI invalid-state listener did not deregister')
                except Exception as error:
                    report['cleanup']['listener_error'] = str(error)
            if process is not None:
                if process.poll() is None:
                    try:
                        report['cleanup']['gui'] = request('quit')
                        process.wait(timeout=5)
                    except Exception as error:
                        report['cleanup']['quit_error'] = str(error)
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                report['cleanup']['survivor_pid'] = process.pid
                report['cleanup']['child_exit'] = process.poll()
                for stream in (process.stdin, process.stdout):
                    if stream is not None:
                        stream.close()
            stderr.close()
            report['cleanup']['config_absent'] = not config.exists()
            report['child_stderr'] = stderr_path.read_text(encoding='utf-8', errors='replace')[:16384]
            gui = report['cleanup'].get('gui', {})
            clean = (process is not None and process.returncode == 0 and gui.get('closed')
                     and gui.get('workers_retired') and not gui.get('errors')
                     and not gui.get('config_exists') and not config.exists()
                     and 'listener_error' not in report['cleanup'])
            if not clean:
                report['first_failure'] = report['first_failure'] or 'normal GUI retirement was not proved'
            if report['first_failure']:
                report['status'] = 'FAIL'
            report['seconds'] = time.monotonic() - started
            json.dump(report, output, ensure_ascii=False, indent=2)
            output.write('\n')
    print(json.dumps({'status': report['status'], 'report': str(args.report),
                      'first_failure': report['first_failure']}, ensure_ascii=False))
    return 1 if report['status'] == 'FAIL' else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--expect-missing-description', action='store_true')
    parser.add_argument('--expect-missing-invalid-state', action='store_true')
    parser.add_argument('--_fresh-pid', type=int, help=argparse.SUPPRESS)
    parser.add_argument('--_fresh-name', help=argparse.SUPPRESS)
    parser.add_argument('--_private-bus', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--_child', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--_config', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    args.source = args.source.resolve()
    require(args.source.is_dir(), '--source must be a directory')
    if args._fresh_pid:
        import gi
        gi.require_version('Atspi', '2.0')
        from gi.repository import Atspi
        desktop = Atspi.get_desktop(0)
        applications = [desktop.get_child_at_index(i) for i in range(desktop.get_child_count())]
        queue = [app for app in applications if app.get_process_id() == args._fresh_pid]
        visited = 0
        while queue:
            node = queue.pop(0)
            visited += 1
            require(visited <= 512, 'fresh observer tree exceeds bound')
            if node.get_name() == args._fresh_name and node.get_role() in (Atspi.Role.ENTRY, Atspi.Role.TEXT):
                node.clear_cache()
                print(json.dumps({'pid': node.get_process_id(), 'name': node.get_name(),
                                  'invalid': node.get_state_set().contains(Atspi.StateType.INVALID_ENTRY)}))
                return 0
            queue.extend(node.get_child_at_index(i) for i in range(node.get_child_count()))
        raise AssertionError('fresh observer did not find target entry')
    if args._child:
        require(args._config is not None, 'GUI child requires private configuration')
        return child(args)
    require(args.report is not None, '--report NEW.json is required')
    args.report = args.report.resolve()
    require(not args.report.exists(), 'report already exists; refusing to overwrite')
    require(os.environ.get('DISPLAY'), 'supply an existing X display or an owned Xvfb')
    if not args._private_bus:
        os.execvp('dbus-run-session', ['dbus-run-session', '--', sys.executable, '-B',
                                     str(SCRIPT), '--source', str(args.source),
                                     '--report', str(args.report), '--_private-bus'] +
                  (['--expect-missing-description'] if args.expect_missing_description else []) +
                  (['--expect-missing-invalid-state'] if args.expect_missing_invalid_state else []))
    return parent(args)


if __name__ == '__main__':
    sys.exit(main())
