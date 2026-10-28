#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepare runtime files, undo caught failures, and mark interrupted updates.

This detects process interruption; it is not an atomic multi-file transaction.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from crystalhd_provenance import RECEIPT_NAME, create_receipt, source_identity


def dry_run():
    # Recursive recipes also run for make -n/-q/-t. None may publish state.
    # GNU make canonicalizes these into its initial short-flags word.
    words = os.environ.get('MAKEFLAGS', '').split()
    return bool(words and not words[0].startswith('--') and '=' not in words[0]
                and any(flag in words[0] for flag in 'nqt'))


def write_marker(path, bundle, phase, uninstall=False, recovery=None):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                                         dir=str(path.parent), delete=False) as stream:
            temporary = stream.name
            record = {'schema': 1, 'bundle': bundle, 'stage': phase,
                      'operation': 'uninstall' if uninstall else 'install',
                      'state': 'incomplete-or-in-progress'}
            if recovery:
                record['rollback_directories'] = [str(entry) for entry in recovery]
            json.dump(record, stream)
            stream.write('\n')
            stream.flush()
            os.fchmod(stream.fileno(), 0o644)
            os.fsync(stream.fileno())
        os.replace(temporary, str(path))
        temporary = None
        sync_directory(path.parent)
    finally:
        if temporary is not None:
            os.unlink(temporary)


def sync_directory(path):
    descriptor = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def payload_files(prepared, destination):
    """Validate the complete prepared namespace before replacing any payload."""
    files, targets = [], set()
    base = destination.resolve()
    for directory, children, names in os.walk(str(prepared), followlinks=False):
        for name in children:
            if (Path(directory) / name).is_symlink():
                raise RuntimeError('prepared directory is a symlink')
        for name in names:
            source = Path(directory) / name
            mode = source.lstat().st_mode
            if not (stat.S_ISREG(mode) or stat.S_ISLNK(mode)):
                raise RuntimeError('prepared payload is not a regular file or link')
            if source.is_symlink():
                if (os.path.isabs(os.readlink(str(source))) or
                        prepared.resolve() not in source.resolve(strict=True).parents):
                    raise RuntimeError('prepared symlink leaves its payload')
            relative = source.relative_to(prepared)
            if relative == Path('etc/crystalhd/config.ini') or relative.parts[:3] == ('var', 'lib', 'crystalhd'):
                raise RuntimeError('prepared payload contains user configuration or installation state')
            target = destination / relative
            parent = target.parent.resolve()
            if base not in (parent, *parent.parents):
                raise RuntimeError('payload destination escapes DESTDIR')
            canonical = parent / target.name
            if canonical in targets:
                raise RuntimeError('prepared payload paths alias at destination')
            if target.exists() and not target.is_symlink() and not target.is_file():
                raise RuntimeError('payload destination is not a regular file')
            targets.add(canonical)
            files.append((relative, source))
    return sorted(files, key=lambda pair: (pair[1].is_symlink(), str(pair[0])))


def publish_file(source, target):
    """Replace one leaf atomically; existing parent directory modes stay intact."""
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.crystalhd-install-', dir=str(target.parent))
    try:
        if source.is_symlink():
            os.close(descriptor)
            descriptor = None
            os.unlink(temporary)
            os.symlink(os.readlink(str(source)), temporary)
        else:
            with os.fdopen(descriptor, 'wb') as output:
                descriptor = None
                with source.open('rb') as content:
                    shutil.copyfileobj(content, output)
                output.flush()
                os.fchmod(output.fileno(), stat.S_IMODE(source.stat().st_mode))
                os.fsync(output.fileno())
        os.replace(temporary, str(target))
        temporary = None
        sync_directory(target.parent)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if temporary is not None and os.path.lexists(temporary):
            os.unlink(temporary)


class InstallRestored(RuntimeError):
    """The failed attempt restored its incoming file/marker state, not success."""


class InstallUndo:
    """Retain original leaf inodes without copying runtime or user data.

    Each private directory is on the target filesystem, so both retaining the
    original inode and restoring it work across separate /usr, /lib or /boot
    mounts. The installer only replaces leaves; it never writes these inodes.
    """
    def __init__(self, targets):
        self.entries = {}
        self.directories = {}
        self.missing_parents = set()
        self.changed = []
        try:
            for target in targets:
                parent = target.parent
                while not parent.exists():
                    self.missing_parents.add(parent)
                    parent = parent.parent
                self.entries[target] = None
                if not os.path.lexists(str(target)):
                    continue
                mode = target.lstat().st_mode
                if not (stat.S_ISREG(mode) or stat.S_ISLNK(mode)):
                    raise RuntimeError('rollback target is not a regular file or link')
                parent = target.parent.resolve()
                if parent not in self.directories:
                    self.directories[parent] = Path(tempfile.mkdtemp(
                        prefix='.crystalhd-undo-', dir=str(parent)))
                backup = self.directories[parent] / target.name
                os.link(str(target), str(backup), follow_symlinks=False)
                self.entries[target] = backup
                sync_directory(backup.parent)
        except BaseException:
            self.cleanup_warning()
            raise

    def touch(self, target):
        # Include the failing call: replace may succeed before its fsync fails.
        if target not in self.changed:
            self.changed.append(target)

    def restore(self, target):
        backup = self.entries[target]
        if backup is None:
            if os.path.lexists(str(target)):
                target.unlink()
        else:
            os.replace(str(backup), str(target))
        if target.parent.exists():
            try:
                sync_directory(target.parent)
            except OSError:
                # Keep the old inode recoverable even if rename succeeded but
                # the restored directory could not be made durable.
                if backup is not None and not os.path.lexists(str(backup)):
                    try:
                        os.link(str(target), str(backup), follow_symlinks=False)
                    except OSError:
                        pass
                raise

    def restore_payload(self, marker):
        failures = []
        for target in reversed(self.changed):
            if target == marker:
                continue
            try:
                self.restore(target)
            except OSError as error:
                failures.append(error)
        if failures:
            raise RuntimeError('could not restore every replaced runtime file')

    def cleanup(self):
        # Only our leaf links live in these private directories; never recurse
        # into unexpected files or follow a saved symlink during cleanup.
        for directory in self.directories.values():
            for entry in directory.iterdir():
                entry.unlink()
            directory.rmdir()

    def cleanup_warning(self):
        try:
            self.cleanup()
        except OSError:
            print('WARN: runtime undo links need cleanup: ' +
                  ', '.join(str(p) for p in self.directories.values()), file=sys.stderr)

    def remove_new_parents(self):
        for directory in sorted(self.missing_parents, key=lambda p: len(p.parts), reverse=True):
            try:
                directory.rmdir()
            except OSError:
                # Another owner may have added a file while installation ran.
                # Restoring runtime leaves never authorizes removing that data.
                pass


def previous_recovery(marker):
    if marker.is_symlink() or not marker.is_file():
        return []
    try:
        record = json.loads(marker.read_text(encoding='utf-8'))
    except (OSError, ValueError, UnicodeError):
        return []
    if not isinstance(record, dict) or record.get('schema') != 1:
        return []
    paths = record.get('rollback_directories', [])
    return paths if isinstance(paths, list) else []


def cleanup_previous(paths, targets):
    """After a complete retry, discard only recognizable earlier undo links."""
    allowed = {}
    for target in targets:
        allowed.setdefault(target.parent.resolve(), set()).add(target.name)
    for name in paths:
        if not isinstance(name, str):
            continue
        directory = Path(name)
        if not os.path.lexists(str(directory)):
            continue
        try:
            info = directory.lstat()
            names = allowed.get(directory.parent.resolve(), set())
            if (not names or not directory.is_absolute() or not directory.name.startswith('.crystalhd-undo-') or
                    not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or
                    stat.S_IMODE(info.st_mode) != 0o700):
                raise RuntimeError('unrecognized previous recovery directory')
            entries = list(directory.iterdir())
            if any(entry.name not in names or not (stat.S_ISREG(entry.lstat().st_mode) or
                       stat.S_ISLNK(entry.lstat().st_mode)) for entry in entries):
                raise RuntimeError('unrecognized previous recovery entry')
            for entry in entries:
                entry.unlink()
            directory.rmdir()
        except (OSError, RuntimeError):
            print('WARN: previous runtime recovery links retained: ' + str(directory), file=sys.stderr)


def install(args):
    if dry_run():
        print('Runtime install: dry run; no installation state or payload changed')
        return
    root = Path(__file__).resolve().parents[1]
    destination = Path(os.path.abspath(args.destdir or '/'))
    state = destination / 'var/lib/crystalhd'
    # An alternate root must not follow an absolute /var symlink into the host.
    if destination.resolve() not in (state.resolve(), *state.resolve().parents):
        raise RuntimeError('installation state escapes DESTDIR')
    os.umask(0o022)
    state.mkdir(parents=True, exist_ok=True)
    # Lock the stable directory inode: no stale PID test or expendable lock
    # filename can split two installers onto different lock objects.
    descriptor = os.open(str(state), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o022:
            raise RuntimeError('unsafe runtime installation lock')
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('another runtime installation is active')
        # A child still replacing files must retain the lock if its parent dies.
        os.set_inheritable(descriptor, True)
        marker = state / 'runtime-install.pending'
        earlier_recovery = previous_recovery(marker)
        bundle = (root / 'VERSION').read_text(encoding='utf-8').strip()
        source_id = source_identity(root)
        common = ['PREFIX=' + args.prefix, 'DESTDIR=' + args.destdir,
                  'BINDIR=' + args.bindir, 'DATADIR=' + args.datadir]
        stages = [
            ('udev', ['-C', 'driver/linux', '-f', 'Makefile.in', 'install-udev']),
            ('firmware', ['-C', 'firmware', 'install']),
            ('library', ['-C', 'linux_lib/libcrystalhd', 'install']),
            ('gstreamer', ['-C', 'filters/gst/gst-plugin-1.0', 'install']),
            ('vaapi', ['-C', 'filters/vaapi', 'install']),
            ('check', ['install-check']),
            ('config', ['install-config']),
            ('record', ['install-runtime-record']),
        ]
        if args.uninstall:
            stages = [
                ('settings', ['uninstall-settings']),
                ('record', ['uninstall-runtime-record']),
                ('check', ['uninstall-check']),
                ('vaapi', ['-C', 'filters/vaapi', 'uninstall']),
                ('gstreamer', ['-C', 'filters/gst/gst-plugin-1.0', 'uninstall']),
                ('library', ['-C', 'linux_lib/libcrystalhd', 'uninstall']),
                ('firmware', ['-C', 'firmware', 'uninstall']),
                ('udev', ['-C', 'driver/linux', '-f', 'Makefile.in', 'uninstall-udev']),
                ('config', ['uninstall-config']),
                ('provenance', ['uninstall-provenance']),
            ]
        def run(command, arguments):
            # Keep make's jobserver and the installation lock, as a recursive
            # make recipe would. No shell parses paths or configuration values.
            environment = os.environ.copy()
            environment['CRYSTALHD_SOURCE_ID'] = source_id
            subprocess.run(shlex.split(args.make) + command + arguments,
                           cwd=str(root), env=environment,
                           close_fds=False, check=True)

        if args.uninstall:
            for phase, command in stages:
                write_marker(marker, bundle, phase, True, recovery=earlier_recovery)
                run(command, common)
        else:
            # Materialize transformed scripts, headers, libraries and links
            # first. Preparation errors leave the old payload/marker intact.
            with tempfile.TemporaryDirectory(prefix='crystalhd-runtime-stage.') as directory:
                prepared = Path(directory)
                staged_args = [arg for arg in common if not arg.startswith('DESTDIR=')]
                staged_args.append('DESTDIR=' + str(prepared))
                for phase, command in stages:
                    try:
                        run(command, staged_args)
                    except subprocess.CalledProcessError:
                        raise RuntimeError('preparing ' + phase + ' failed; installed payload was not replaced')
                create_receipt(prepared, bundle, args.bindir, args.datadir,
                               source_id)
                files = payload_files(prepared, destination)
                version_relative = Path(os.path.normpath(args.datadir).lstrip('/')) / 'crystalhd/VERSION'
                receipt_relative = (Path(os.path.normpath(args.datadir).lstrip('/')) /
                                    ('crystalhd/' + RECEIPT_NAME))
                versions = [source for relative, source in files if relative == version_relative]
                receipts = [source for relative, source in files if relative == receipt_relative]
                if len(versions) != 1 or versions[0].is_symlink():
                    raise RuntimeError('prepared runtime has no regular VERSION record')
                if len(receipts) != 1 or receipts[0].is_symlink():
                    raise RuntimeError('prepared runtime has no regular component receipt')
                bundle = versions[0].read_text(encoding='utf-8').strip()
                targets = [destination / relative for relative, _ in files]
                undo = InstallUndo(targets + [marker])
                # A retry may also fail: keep older recovery links discoverable
                # until a complete install can safely discard all of them.
                recovery = earlier_recovery + list(undo.directories.values())
                activation_attempted = False
                try:
                    undo.touch(marker)
                    write_marker(marker, bundle, 'publish', recovery=recovery)
                    for relative, source in files:
                        if relative not in (version_relative, receipt_relative):
                            target = destination / relative
                            undo.touch(target)
                            publish_file(source, target)
                    write_marker(marker, bundle, 'activate', recovery=recovery)
                    activation_attempted = True
                    run(['install-runtime-activate'], common)
                    write_marker(marker, bundle, 'record', recovery=recovery)
                    target = destination / receipt_relative
                    undo.touch(target)
                    publish_file(receipts[0], target)
                    target = destination / version_relative
                    undo.touch(target)
                    publish_file(versions[0], target)
                    marker.unlink()
                    sync_directory(state)
                except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
                    rollback_phase = 'rollback'
                    try:
                        try:
                            write_marker(marker, bundle, 'rollback', recovery=recovery)
                        except OSError:
                            # A full state filesystem must not prevent undo on
                            # another writable payload filesystem.
                            pass
                        undo.restore_payload(marker)
                        if activation_attempted:
                            rollback_phase = 'rollback-activate'
                            try:
                                write_marker(marker, bundle, 'rollback-activate', recovery=recovery)
                            except OSError:
                                pass
                            run(['install-runtime-activate'], common)
                        undo.restore(marker)
                    except (OSError, RuntimeError, subprocess.CalledProcessError) as rollback_error:
                        try:
                            write_marker(marker, bundle, rollback_phase + '-failed', recovery=recovery)
                        except OSError:
                            pass
                        # Preserve remaining old inodes; a failed restoration
                        # must not be reported as a completed installation.
                        raise RuntimeError('runtime rollback is incomplete: ' + str(rollback_error) +
                                           '; recovery directories: ' +
                                           ', '.join(str(path) for path in recovery)) from error
                    undo.cleanup_warning()
                    undo.remove_new_parents()
                    raise InstallRestored('pre-install file state restored; any earlier pending record is retained') from error
                undo.cleanup_warning()
                cleanup_previous(earlier_recovery, targets + [marker])
        if args.uninstall:
            if any(isinstance(name, str) and os.path.lexists(name) for name in earlier_recovery):
                write_marker(marker, bundle, 'uninstall-recovery-retained', True, recovery=earlier_recovery)
                raise RuntimeError('runtime removed, but retained rollback links still need inspection')
            marker.unlink()
            sync_directory(state)
    finally:
        os.close(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--make', required=True)
    parser.add_argument('--destdir', default='')
    parser.add_argument('--prefix', required=True)
    parser.add_argument('--bindir', required=True)
    parser.add_argument('--datadir', required=True)
    parser.add_argument('--uninstall', action='store_true')
    args = parser.parse_args()
    if not all(os.path.isabs(p) for p in (args.prefix, args.bindir, args.datadir)):
        parser.error('PREFIX, BINDIR and DATADIR must be absolute')
    if args.destdir and not os.path.isabs(args.destdir):
        parser.error('DESTDIR must be absolute')
    try:
        install(args)
        return 0
    except InstallRestored as error:
        cause = error.__cause__
        reason = (cause.strerror if isinstance(cause, OSError) else
                  'installer subprocess failed' if isinstance(cause, subprocess.CalledProcessError)
                  else str(cause))
        print('Runtime installation failed: ' + reason + '; ' + str(error) + '.', file=sys.stderr)
        return 1
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        reason = str(error) if isinstance(error, RuntimeError) else (
            error.strerror if isinstance(error, OSError) else 'installer subprocess failed')
        print('Runtime installation did not complete: ' + reason + '. Inspect runtime-install.pending '
              'and rerun make install-runtime from the intended source or release.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
