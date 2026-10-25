#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the existing runtime installers with a durable incomplete-install marker.

This detects process interruption; it is not an atomic multi-file transaction.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shlex
import stat
import subprocess
import sys
import tempfile


def dry_run():
    # GNU make canonicalizes -n/--just-print into its initial short-flags word.
    words = os.environ.get('MAKEFLAGS', '').split()
    return bool(words and not words[0].startswith('--') and '=' not in words[0]
                and 'n' in words[0])


def write_marker(path, bundle, phase, uninstall=False):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                                         dir=str(path.parent), delete=False) as stream:
            temporary = stream.name
            json.dump({'schema': 1, 'bundle': bundle, 'stage': phase,
                       'operation': 'uninstall' if uninstall else 'install',
                       'state': 'incomplete-or-in-progress'}, stream)
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
        bundle = (root / 'VERSION').read_text(encoding='utf-8').strip()
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
            ]
        for phase, command in stages:
            write_marker(marker, bundle, phase, args.uninstall)
            # Keep make's jobserver and the installation lock, as a recursive
            # make recipe would. No shell parses paths or configuration values.
            subprocess.run(shlex.split(args.make) + command + common,
                           cwd=str(root), close_fds=False, check=True)
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
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        reason = str(error) if isinstance(error, RuntimeError) else (
            error.strerror if isinstance(error, OSError) else 'installer subprocess failed')
        print('Runtime installation did not complete: ' + reason + '. Inspect runtime-install.pending '
              'and rerun make install-runtime from the intended source or release.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
