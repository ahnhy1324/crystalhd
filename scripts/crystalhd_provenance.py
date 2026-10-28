#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Create and verify the completed userspace installation cohort receipt."""
import argparse
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys


SCHEMA = 1
RECEIPT_NAME = 'runtime-components-v1.json'
CORE_COMPONENTS = ('dil', 'gstreamer', 'vaapi', 'player')
LABELS = {'dil': 'DIL', 'gstreamer': 'GStreamer',
          'vaapi': 'VA-API', 'player': 'Player'}
SOURCE_PATTERN = re.compile(r'^(?:git|archive):[0-9a-f]{40}$')
PLAYER_MARKER = re.compile(
    br'^# CrystalHD source identity: ([A-Za-z0-9:._+-]+)$', re.MULTILINE)


def _align4(value):
    return (value + 3) & ~3


def _open_regular(path):
    flags = os.O_RDONLY | os.O_NONBLOCK
    flags |= getattr(os, 'O_CLOEXEC', 0)
    flags |= getattr(os, 'O_NOFOLLOW', 0)
    descriptor = os.open(str(path), flags)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            raise ValueError('ELF payload is not a regular file')
        stream = os.fdopen(descriptor, 'rb')
        descriptor = None
        return stream
    finally:
        if descriptor is not None:
            os.close(descriptor)


def gnu_build_id(path):
    """Return the GNU ELF build ID without requiring binutils at runtime."""
    path = Path(path)
    with _open_regular(path) as stream:
        size = os.fstat(stream.fileno()).st_size
        ident = stream.read(16)
        if len(ident) != 16 or ident[:4] != b'\x7fELF':
            raise ValueError('not an ELF file')
        elf_class, data = ident[4], ident[5]
        if elf_class not in (1, 2) or data not in (1, 2):
            raise ValueError('unsupported ELF encoding')
        endian = '<' if data == 1 else '>'
        header_format = endian + ('HHIIIIIHHHHHH' if elf_class == 1
                                  else 'HHIQQQIHHHHHH')
        header_size = struct.calcsize(header_format)
        header = stream.read(header_size)
        if len(header) != header_size:
            raise ValueError('truncated ELF header')
        values = struct.unpack(header_format, header)
        program_offset, entry_size, entry_count = values[4], values[8], values[9]
        program_format = endian + ('IIIIIIII' if elf_class == 1 else 'IIQQQQQQ')
        minimum_entry = struct.calcsize(program_format)
        if entry_size < minimum_entry or entry_count > 65535:
            raise ValueError('invalid ELF program table')
        for index in range(entry_count):
            offset = program_offset + index * entry_size
            if offset < 0 or offset + minimum_entry > size:
                raise ValueError('ELF program table leaves the file')
            stream.seek(offset)
            fields = struct.unpack(program_format, stream.read(minimum_entry))
            if fields[0] != 4:  # PT_NOTE
                continue
            note_offset = fields[1] if elf_class == 1 else fields[2]
            note_size = fields[4] if elf_class == 1 else fields[5]
            if note_size > 16 * 1024 * 1024 or note_offset + note_size > size:
                raise ValueError('ELF note segment leaves the file')
            stream.seek(note_offset)
            notes = stream.read(note_size)
            cursor = 0
            while cursor + 12 <= len(notes):
                name_size, value_size, note_type = struct.unpack_from(
                    endian + 'III', notes, cursor)
                cursor += 12
                name_end = cursor + name_size
                value_start = cursor + _align4(name_size)
                value_end = value_start + value_size
                next_note = value_start + _align4(value_size)
                if name_end > len(notes) or value_end > len(notes) or next_note > len(notes):
                    raise ValueError('truncated ELF note')
                name = notes[cursor:name_end].rstrip(b'\0')
                value = notes[value_start:value_end]
                if name == b'GNU' and note_type == 3 and value:
                    return value.hex()
                cursor = next_note
    raise ValueError('GNU build ID is unavailable')


def source_identity(root):
    override = os.environ.get('CRYSTALHD_SOURCE_ID', '')
    if override:
        if override == 'unknown' or SOURCE_PATTERN.match(override):
            return override
        raise ValueError('CRYSTALHD_SOURCE_ID has an unsupported form')
    root = Path(root)
    try:
        revision = (root / 'SOURCE_REVISION').read_text(
            encoding='ascii').strip()
    except (OSError, UnicodeError):
        revision = ''
    if re.match(r'^[0-9a-f]{40}$', revision):
        return 'archive:' + revision
    try:
        clean = subprocess.run(
            ['git', '-C', str(root), 'diff', '--quiet', 'HEAD', '--'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        revision = subprocess.check_output(
            ['git', '-C', str(root), 'rev-parse', '--verify', 'HEAD'],
            stderr=subprocess.DEVNULL, universal_newlines=True).strip()
        if clean and re.match(r'^[0-9a-f]{40}$', revision):
            return 'git:' + revision
    except (OSError, subprocess.CalledProcessError):
        pass
    return 'unknown'


def _payload_files(prepared):
    prepared = Path(prepared)
    result = []
    for directory, children, names in os.walk(str(prepared), followlinks=False):
        children[:] = [name for name in children
                       if not (Path(directory) / name).is_symlink()]
        for name in names:
            path = Path(directory) / name
            if path.is_file() and not path.is_symlink():
                result.append(path)
    return result


def _logical(prepared, path):
    return '/' + Path(path).relative_to(prepared).as_posix()


def _one_elf(prepared, files, component, predicate):
    matches = []
    for path in files:
        if not predicate(path.name):
            continue
        try:
            identity = gnu_build_id(path)
        except (OSError, ValueError):
            continue
        matches.append((path, identity))
    if len(matches) != 1:
        raise ValueError('%s provenance requires exactly one ELF payload' % component)
    path, identity = matches[0]
    return {'identity': 'gnu-build-id',
            'files': [{'path': _logical(prepared, path),
                       'gnu_build_id': identity}]}


def _player_file(prepared, path, expected_source):
    try:
        content = path.read_bytes()
    except OSError as error:
        raise ValueError('Player provenance file is unavailable: %s' % path) from error
    markers = PLAYER_MARKER.findall(content)
    if len(markers) != 1:
        raise ValueError('Player provenance marker is unavailable: %s' % path)
    marker = markers[0].decode('ascii')
    if marker != expected_source:
        raise ValueError('Player source identities differ within the staged payload')
    return {'path': _logical(prepared, path), 'bytes': len(content)}


def create_receipt(prepared, bundle, bindir, datadir, source_id):
    if source_id != 'unknown' and not (
            isinstance(source_id, str) and SOURCE_PATTERN.match(source_id)):
        raise ValueError('Player source identity is invalid')
    prepared = Path(prepared).resolve()
    files = _payload_files(prepared)
    components = {
        'dil': _one_elf(
            prepared, files, 'DIL',
            lambda name: re.match(r'^libcrystalhd\.so\.\d+(?:\.\d+)*$', name) is not None),
        'gstreamer': _one_elf(
            prepared, files, 'GStreamer', lambda name: name == 'libgstcrystalhd.so'),
        'vaapi': _one_elf(
            prepared, files, 'VA-API', lambda name: name == 'crystalhd_drv_video.so'),
    }
    player_paths = [
        prepared / bindir.lstrip('/') / 'crystalhd-play',
        prepared / datadir.lstrip('/') / 'crystalhd/player.py',
    ]
    components['player'] = {
        'identity': 'source-revision',
        'source_id': source_id,
        'files': [_player_file(prepared, path, source_id) for path in player_paths],
    }
    receipt = {'schema': SCHEMA, 'bundle': bundle,
               'scope': 'installed-userspace-files', 'components': components}
    target = prepared / datadir.lstrip('/') / 'crystalhd' / RECEIPT_NAME
    if os.path.lexists(str(target)):
        raise ValueError('prepared payload already contains a component receipt')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(receipt, sort_keys=True,
                                 separators=(',', ':')) + '\n', encoding='utf-8')
    os.chmod(str(target), 0o644)
    return target


def _safe_target(root, logical):
    if not isinstance(logical, str) or not logical.startswith('/') or '\0' in logical:
        raise ValueError('invalid receipt path')
    relative = Path(logical.lstrip('/'))
    if '..' in relative.parts:
        raise ValueError('receipt path leaves the inspected root')
    try:
        root = Path(root or '/').resolve()
    except (OSError, RuntimeError) as error:
        raise ValueError('inspected root cannot be resolved') from error
    target = root / relative
    try:
        resolved = target.resolve(strict=True)
    except (OSError, RuntimeError):
        return target, None
    if root not in (resolved, *resolved.parents):
        raise ValueError('receipt path leaves the inspected root')
    return target, resolved


def _logical_observed(root, observed):
    if not observed:
        return None
    try:
        root_path = Path(root or '/').resolve()
    except (OSError, RuntimeError) as error:
        raise ValueError('inspected root cannot be resolved') from error
    try:
        resolved = Path(observed).resolve(strict=True)
    except (OSError, RuntimeError):
        resolved = Path(os.path.abspath(str(observed)))
    if root_path in (resolved, *resolved.parents):
        return '/' + resolved.relative_to(root_path).as_posix()
    return str(resolved)


def _resolved_observed(observed):
    try:
        return Path(observed).resolve(strict=True)
    except (OSError, RuntimeError):
        return Path(os.path.abspath(str(observed)))


def verify_receipt(receipt_path, root, runtime_version, observed):
    messages = []
    failed = False
    try:
        receipt_path = Path(receipt_path)
        if receipt_path.is_symlink() or not receipt_path.is_file():
            raise ValueError('receipt is not a regular file')
        data = json.loads(receipt_path.read_text(encoding='utf-8'))
        if not isinstance(data, dict) or data.get('schema') != SCHEMA:
            raise ValueError('unsupported receipt schema')
        if data.get('scope') != 'installed-userspace-files':
            raise ValueError('unsupported receipt scope')
        components = data.get('components')
        if not isinstance(components, dict) or any(
                name not in components for name in CORE_COMPONENTS):
            raise ValueError('receipt omits a required userspace component')
        bundle = data.get('bundle')
        if not isinstance(bundle, str) or not bundle:
            raise ValueError('receipt bundle is invalid')
        if not runtime_version:
            messages.append(('FAIL', 'Runtime component receipt exists without a readable VERSION record'))
            failed = True
        elif bundle != runtime_version:
            messages.append(('FAIL', 'Runtime component receipt bundle %s differs from VERSION %s' %
                             (bundle, runtime_version)))
            failed = True
        for name in CORE_COMPONENTS:
            component = components[name]
            label = LABELS[name]
            if not isinstance(component, dict) or not isinstance(
                    component.get('files'), list):
                raise ValueError('%s receipt is invalid' % label)
            files = component['files']
            expected_paths = []
            component_failed = False
            if name == 'player':
                source_id = component.get('source_id')
                if source_id != 'unknown' and not (
                        isinstance(source_id, str) and SOURCE_PATTERN.match(source_id)):
                    raise ValueError('Player source identity is invalid')
                if len(files) != 2:
                    raise ValueError('Player receipt requires two files')
                for record in files:
                    if not isinstance(record, dict) or not isinstance(
                            record.get('bytes'), int):
                        raise ValueError('Player file receipt is invalid')
                    target, resolved = _safe_target(root, record.get('path'))
                    expected_paths.append(record.get('path'))
                    if resolved is None or not target.is_file() or target.is_symlink():
                        messages.append(('FAIL', '%s provenance: missing %s' %
                                         (label, record.get('path'))))
                        component_failed = failed = True
                        continue
                    content = target.read_bytes()
                    markers = PLAYER_MARKER.findall(content)
                    marker = markers[0].decode('ascii') if len(markers) == 1 else None
                    if marker != source_id or len(content) != record['bytes']:
                        messages.append(('FAIL', '%s provenance: %s differs from the completed receipt' %
                                         (label, record.get('path'))))
                        component_failed = failed = True
                if not component_failed:
                    if source_id == 'unknown':
                        messages.append(('WARN', '%s provenance: UNKNOWN; the installed source tree had no durable revision identity' % label))
                    else:
                        messages.append(('OK', '%s provenance: both on-disk files carry source identity %s and their recorded sizes' %
                                         (label, source_id)))
            else:
                if component.get('identity') != 'gnu-build-id' or len(files) != 1:
                    raise ValueError('%s ELF receipt is invalid' % label)
                record = files[0]
                if not isinstance(record, dict) or not isinstance(
                        record.get('gnu_build_id'), str):
                    raise ValueError('%s ELF file receipt is invalid' % label)
                target, resolved = _safe_target(root, record.get('path'))
                expected_paths.append(record.get('path'))
                if resolved is None:
                    messages.append(('FAIL', '%s provenance: missing %s' %
                                     (label, record.get('path'))))
                    component_failed = failed = True
                else:
                    try:
                        actual = gnu_build_id(target)
                    except (OSError, ValueError) as error:
                        messages.append(('FAIL', '%s provenance: cannot read the GNU Build ID from %s (%s)' %
                                         (label, record.get('path'), error)))
                        component_failed = failed = True
                    else:
                        if actual != record['gnu_build_id']:
                            messages.append(('FAIL', '%s provenance: %s has a different GNU Build ID' %
                                             (label, record.get('path'))))
                            component_failed = failed = True
                        else:
                            messages.append(('OK', '%s provenance: on-disk GNU Build ID %s matches the completed receipt' %
                                             (label, actual[:16])))
            selected_values = observed.get(name, [])
            if isinstance(selected_values, str):
                selected_values = [selected_values]
            expected_resolved = []
            for path in expected_paths:
                _, resolved = _safe_target(root, path)
                if resolved is not None:
                    expected_resolved.append(resolved)
            for selected in selected_values:
                if not selected:
                    continue
                logical_selected = _logical_observed(root, selected)
                if _resolved_observed(selected) not in expected_resolved:
                    messages.append(('FAIL', '%s selection: %s is outside the completed userspace receipt' %
                                     (label, logical_selected)))
                    failed = True
        messages.append(('INFO', 'Runtime provenance covers installed on-disk userspace files; mappings in already-running applications were not inspected'))
    except (OSError, RuntimeError, ValueError, UnicodeError) as error:
        messages.append(('FAIL', 'Runtime component provenance receipt is invalid: %s' % error))
        failed = True
    return messages, failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest='command')
    create = subparsers.add_parser('create')
    create.add_argument('--prepared', required=True)
    create.add_argument('--bundle', required=True)
    create.add_argument('--bindir', required=True)
    create.add_argument('--datadir', required=True)
    create.add_argument('--source-id', required=True)
    verify = subparsers.add_parser('verify')
    verify.add_argument('--receipt', required=True)
    verify.add_argument('--root', default='')
    verify.add_argument('--runtime-version', default='')
    verify.add_argument('--observed', action='append', default=[])
    args = parser.parse_args()
    try:
        if args.command == 'create':
            path = create_receipt(args.prepared, args.bundle, args.bindir,
                                  args.datadir, args.source_id)
            print(path)
            return 0
        if args.command == 'verify':
            observed = {}
            for item in args.observed:
                if '=' not in item:
                    raise ValueError('--observed requires COMPONENT=PATH')
                name, path = item.split('=', 1)
                if name not in CORE_COMPONENTS:
                    raise ValueError('invalid observed component')
                observed.setdefault(name, []).append(path)
            messages, failed = verify_receipt(
                args.receipt, args.root, args.runtime_version, observed)
            for level, message in messages:
                print(level + '\t' + message.replace('\n', ' '))
            return 1 if failed else 0
        parser.error('choose create or verify')
    except (OSError, ValueError) as error:
        print('FAIL\tRuntime component provenance failed: ' +
              str(error).replace('\n', ' '))
        return 2


if __name__ == '__main__':
    sys.exit(main())
