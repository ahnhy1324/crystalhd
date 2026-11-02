#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Select private, pinned runtime files in an operator-owned slot store.

No executable, loader, network, device or system installation is invoked.
Selection is one atomic state-file replacement, not power-loss durability or
an atomic update of already-running consumers. Inputs must remain immutable.
"""
import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import resource
import shutil
import stat
import sys
import tarfile
import tempfile
import time
import uuid

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location('native_runtime_preflight',
    Path(__file__).with_name('native-runtime-preflight.py'))
PREFLIGHT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREFLIGHT)
SHA = re.compile(r'[0-9a-f]{64}\Z')
STORE_SCHEMA = 'crystalhd-private-runtime-slots-v1'
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_MEMBERS = 512
MAX_SLOTS = 16


def require(okay, message):
    if not okay:
        raise ValueError(message)


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def checked_path(value):
    path = Path(value)
    require(path.is_absolute() and path != Path('/') and '..' not in path.parts,
            'select an absolute non-root path without parent traversal')
    for parent in reversed((path, *path.parents)):
        if os.path.lexists(parent):
            require(not parent.is_symlink(), 'symlink in selected path: ' + str(parent))
    return path


def regular_json(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, 'rb') as source:
        info = os.fstat(source.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and
                info.st_nlink == 1 and info.st_size <= 256 * 1024,
                'invalid owned metadata file: ' + str(path))
        return json.loads(source.read())


def write_state(store, value):
    """Publish all selection fields in one leaf replacement; no durability claim."""
    raw = encoded(value)
    require(len(raw) <= 256 * 1024, 'slot state exceeds metadata size bound')
    descriptor, name = tempfile.mkstemp(prefix='.state-', dir=store)
    try:
        with os.fdopen(descriptor, 'wb') as output:
            output.write(raw)
        os.replace(name, store / 'state.json')
    finally:
        if os.path.lexists(name):
            os.unlink(name)


def disk_snapshot(store):
    result = {'logical_bytes': 0, 'allocated_bytes': 0, 'entries': 0,
              'available': True}
    seen = set()
    try:
        if not os.path.lexists(store):
            return result
        todo = [store]
        while todo:
            path = todo.pop()
            info = path.lstat()
            key = (info.st_dev, info.st_ino)
            if key in seen:
                continue
            seen.add(key)
            result['entries'] += 1
            require(result['entries'] <= 65536, 'disk snapshot entry limit')
            result['allocated_bytes'] += info.st_blocks * 512
            if stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
                result['logical_bytes'] += info.st_size
            elif stat.S_ISDIR(info.st_mode):
                todo.extend(path.iterdir())
    except (OSError, ValueError) as error:
        result.update(available=False, error=str(error))
    return result


def require_current_store_lock(store, original_store, descriptor):
    current_store = store.lstat()
    current_lock = (store / '.lock').lstat()
    opened_lock = os.fstat(descriptor)
    require(stat.S_ISDIR(current_store.st_mode) and
            (current_store.st_dev, current_store.st_ino) ==
            (original_store.st_dev, original_store.st_ino),
            'store directory was replaced while acquiring its lock')
    require((current_lock.st_dev, current_lock.st_ino) ==
            (opened_lock.st_dev, opened_lock.st_ino),
            'store lock was replaced while acquiring it')


class Costs:
    def __init__(self, store):
        self.store, self.start, self.phases = store, time.monotonic(), []
        self.before = disk_snapshot(store)
        self.maximum = dict(self.before)

    def observe(self):
        current = disk_snapshot(self.store)
        for key in ('logical_bytes', 'allocated_bytes', 'entries'):
            self.maximum[key] = max(self.maximum[key], current[key])
        self.maximum['available'] &= current['available']
        return current

    @contextmanager
    def phase(self, name):
        start, outcome = time.monotonic(), 'PASS'
        try:
            yield
        except BaseException:
            outcome = 'FAIL'
            raise
        finally:
            self.phases.append({'name': name, 'seconds': time.monotonic() - start,
                                'outcome': outcome, 'disk_after': self.observe()})

    def report(self):
        return {'wall_seconds': time.monotonic() - self.start,
                'phases': self.phases, 'store_before': self.before,
                'store_after': self.observe(), 'store_observed_maximum': self.maximum,
                'phase_relationship': 'serialized-operation includes lock wait and encloses the nested stages; phase times overlap and must not be summed.',
                'disk_scope': 'Store-tree snapshots at operation/file boundaries, inode-deduplicated; symlinks are not followed. Existing store-top user files are included. Not a continuous/global disk peak.',
                'own_process_maxrss_KiB': resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                'RSS_scope': 'Linux own-Python-process high-water since process startup, including imports and inspection; not child-tree, unique physical RAM, kernel/GPU memory or prior build RAM.',
                'network_bytes': None, 'network_scope': 'No network is used; first-user acquisition/download burden is NOT_MEASURED.',
                'build_RAM': None, 'whole_first_user_burden': 'NOT_MEASURED'}


@contextmanager
def locked_store(store):
    created = False
    if not store.exists():
        require(store.parent.is_dir(), 'store parent must already exist')
        store.mkdir(mode=0o700)
        created = True
    info = store.lstat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid(),
            'store must be an operator-owned real directory')
    owner_path = store / '.owner.json'
    if not os.path.lexists(owner_path):
        require(not any(store.iterdir()), 'refusing to adopt a nonempty unowned store')
        original_mode = stat.S_IMODE(info.st_mode)
        store.chmod(0o700)
        descriptor = os.open(store / '.lock', os.O_RDWR | os.O_CREAT | os.O_EXCL |
                             os.O_NOFOLLOW, 0o600)
        made = {store / '.lock': os.fstat(descriptor)}
        initial = None
        initialized_lock = False
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            initialized_lock = True
            require_current_store_lock(store, info, descriptor)
            owner = {'schema': STORE_SCHEMA, 'uid': os.getuid(),
                     'path': str(store), 'id': uuid.uuid4().hex}
            with owner_path.open('xb') as output:
                made[owner_path] = os.fstat(output.fileno())
                output.write(encoded(owner))
            owner_path.chmod(0o600)
            (store / 'slots').mkdir(mode=0o700)
            made[store / 'slots'] = (store / 'slots').lstat()
            initial = {'schema': 1, 'store_id': owner['id'], 'active': None,
                       'previous': None, 'slots': {}, 'pending_removal': {}}
            write_state(store, initial)
        except BaseException:
            # Undo only objects created by this initialization. A state leaf
            # published just before a caught interrupt is recognizable by the
            # fresh store nonce; unrelated/replaced objects are left untouched.
            try:
                current_store = store.lstat()
                same_store = (current_store.st_dev, current_store.st_ino) == (info.st_dev, info.st_ino)
            except OSError:
                same_store = False
            if same_store:
                state_path = store / 'state.json'
                if initial is not None and os.path.lexists(state_path):
                    try:
                        if regular_json(state_path) == initial:
                            state_path.unlink()
                    except (OSError, ValueError):
                        pass
                for path, original in reversed(list(made.items())):
                    try:
                        current = path.lstat()
                        if (current.st_dev, current.st_ino) == (original.st_dev, original.st_ino):
                            if stat.S_ISDIR(current.st_mode):
                                path.rmdir()
                            else:
                                path.unlink()
                    except OSError:
                        pass
                if created:
                    if not any(store.iterdir()):
                        store.rmdir()
                else:
                    # Restore our permission change even if an unrelated file
                    # appeared during initialization; keep that file intact.
                    store.chmod(original_mode)
            raise
        finally:
            if initialized_lock:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)
    require(stat.S_IMODE(store.stat().st_mode) & 0o077 == 0,
            'owned store must remain private to its operator')
    descriptor = os.open(store / '.lock', os.O_RDWR | os.O_NOFOLLOW)
    try:
        lock_info = os.fstat(descriptor)
        require(stat.S_ISREG(lock_info.st_mode) and lock_info.st_uid == os.getuid()
                and lock_info.st_nlink == 1, 'invalid store lock')
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        require_current_store_lock(store, info, descriptor)
        owner, state = regular_json(owner_path), regular_json(store / 'state.json')
        require(owner.get('schema') == STORE_SCHEMA and owner.get('uid') == os.getuid()
                and owner.get('path') == str(store) and isinstance(owner.get('id'), str),
                'store ownership marker differs')
        fields = {'schema', 'store_id', 'active', 'previous', 'slots'}
        require(set(state) in (fields, fields | {'pending_removal'})
                and state['schema'] == 1 and state['store_id'] == owner['id']
                and isinstance(state['slots'], dict) and len(state['slots']) <= MAX_SLOTS,
                'invalid slot state')
        slots = store / 'slots'
        require(slots.is_dir() and not slots.is_symlink() and
                slots.stat().st_dev == store.stat().st_dev, 'invalid slots directory')
        require(set(p.name for p in slots.iterdir()) == set(state['slots']),
                'untracked or missing slot payload; refusing adoption')
        for slot, record in state['slots'].items():
            require(SHA.fullmatch(slot) and isinstance(record, dict) and
                    record.get('archive_sha256') == slot and
                    isinstance(record.get('owned_tree_sha256'), str) and
                    SHA.fullmatch(record['owned_tree_sha256']), 'invalid slot identity')
            path = slots / slot
            require(path.is_dir() and not path.is_symlink(), 'slot is not a real directory')
        for key in ('active', 'previous'):
            require(state[key] is None or state[key] in state['slots'],
                    'selection refers to an unknown slot')
        require(state['active'] != state['previous'] or state['active'] is None,
                'active and previous slots must differ')
        pending = state.get('pending_removal', {})
        require(isinstance(pending, dict) and len(pending) <= 1,
                'invalid pending removal state')
        for name, entries in pending.items():
            require(re.fullmatch(r'\.remove-[A-Za-z0-9_-]{6,64}', name) and
                    isinstance(entries, dict) and 0 < len(entries) <= MAX_SLOTS,
                    'invalid pending removal identity')
            for slot, rows in entries.items():
                require(SHA.fullmatch(slot) and isinstance(rows, list) and
                        0 < len(rows) <= 4096, 'invalid pending owned tree')
        tracked_trash = set(pending)
        actual_trash = {p.name for p in store.iterdir() if p.name.startswith('.remove-')}
        require(actual_trash <= tracked_trash,
                'untracked removal payload; refusing to claim cleanup')
        yield state, created
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def member_name(member):
    name = member.name.rstrip('/') if member.isdir() else member.name
    path = PurePosixPath(name)
    require(name and '\\' not in name and str(path) == name and not path.is_absolute()
            and all(p not in ('.', '..') for p in path.parts), 'unsafe TAR member path')
    require(member.name in (name, name + '/') if member.isdir() else member.name == name,
            'noncanonical TAR member name')
    return name


def extract_archive(source, destination, expected, costs):
    require(SHA.fullmatch(expected), 'archive SHA-256 must be explicit')
    descriptor = os.open(source, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= MAX_ARCHIVE_BYTES,
                'archive must be a bounded regular uncompressed TAR')
        def digest():
            stream.seek(0)
            value, size = hashlib.sha256(), 0
            while True:
                raw = stream.read(1024 * 1024)
                if not raw:
                    return value.hexdigest()
                size += len(raw)
                require(size <= MAX_ARCHIVE_BYTES, 'archive grew beyond inspection bound')
                value.update(raw)
        require(digest() == expected, 'archive SHA-256 differs')
        stream.seek(0)
        with tarfile.open(fileobj=stream, mode='r:') as archive:
            members, names, total = [], {}, 0
            for member in archive:
                name = member_name(member)
                require(name not in names and len(members) < MAX_MEMBERS,
                        'duplicate TAR path or member limit')
                require(member.isdir() or member.isreg() or member.issym(),
                        'unsupported TAR type, hardlink or device')
                require(not member.sparse and not any('sparse' in key.lower()
                        for key in member.pax_headers), 'sparse TAR member unsupported')
                require(0 <= member.size <= MAX_ARCHIVE_BYTES, 'TAR member size limit')
                total += member.size if member.isreg() else 0
                require(total <= MAX_ARCHIVE_BYTES, 'TAR payload size limit')
                if member.issym():
                    require(member.linkname not in ('', '.', '..') and
                            '/' not in member.linkname and '\\' not in member.linkname,
                            'TAR symlink must name one local file')
                names[name] = member
                members.append((name, member))
            for name, member in members:
                path = PurePosixPath(name)
                for parent in path.parents:
                    if str(parent) != '.' and str(parent) in names:
                        require(names[str(parent)].isdir(), 'TAR member parent is not a directory')
                output = destination / name
                current = destination
                for part in path.parts[:-1]:
                    current = current / part
                    if not current.exists():
                        current.mkdir(mode=0o755)
                    require(current.is_dir() and not current.is_symlink(),
                            'TAR parent alias refused')
                if member.isdir():
                    output.mkdir(mode=0o755, exist_ok=True)
                elif member.issym():
                    output.symlink_to(member.linkname)
                else:
                    require(not os.path.lexists(output), 'TAR leaf already exists')
                    require(member.mode & ~0o777 == 0, 'special TAR permission bits refused')
                    with output.open('xb') as target, archive.extractfile(member) as data:
                        remaining = member.size
                        while remaining:
                            raw = data.read(min(remaining, 1024 * 1024))
                            require(bool(raw), 'truncated TAR payload')
                            target.write(raw)
                            remaining -= len(raw)
                    output.chmod(member.mode)
                costs.observe()
        require(digest() == expected and os.fstat(stream.fileno()).st_size == info.st_size,
                'archive changed during inspection')


def audit_slot(path, record):
    pins = record['metadata_sha256']
    return PREFLIGHT.audit(path, record['rootfs'], pins['files'], pins['dependencies'],
                           pins['provenance'], record['rootfs_identity'],
                           record['platform_relative'])


def tree_inventory(root):
    """Bounded exact owned tree, never following links."""
    rows, total, todo = [], 0, [root]
    while todo:
        path = todo.pop()
        require(len(rows) < 4096, 'owned tree entry bound exceeded')
        info = path.lstat()
        row = {'path': path.relative_to(root).as_posix(),
               'mode': stat.S_IMODE(info.st_mode)}
        if stat.S_ISLNK(info.st_mode):
            row.update(kind='symlink', target=os.readlink(path))
        elif stat.S_ISDIR(info.st_mode):
            row['kind'] = 'directory'
            todo.extend(path.iterdir())
        else:
            require(stat.S_ISREG(info.st_mode), 'special file in owned payload')
            value = PREFLIGHT.regular_info(path)
            total += value['bytes']
            require(total <= MAX_ARCHIVE_BYTES, 'owned tree byte bound exceeded')
            row.update(kind='file', bytes=value['bytes'], sha256=value['sha256'])
        rows.append(row)
    return sorted(rows, key=lambda row: row['path'])


def tree_fingerprint(root):
    return hashlib.sha256(encoded(tree_inventory(root))).hexdigest()


def owned_payload_matches(path, record):
    # The digest was captured only after successful initial package/provider
    # preflight. Removal must remain possible after that target is unavailable.
    require(tree_fingerprint(path) == record['owned_tree_sha256'],
            'owned payload tree differs; refusing to remove changed or unrelated files')


def selection(store, state):
    return {'active': state['active'], 'previous': state['previous'],
            'active_prefix': str(store / 'slots' / state['active']) if state['active'] else None,
            'previous_prefix': str(store / 'slots' / state['previous']) if state['previous'] else None,
            'retained_slots': sorted(state['slots']),
            'pending_removal': sorted(state.get('pending_removal', {}))}


def finish_pending_removal(store, state, costs):
    """Retry owned deletions; missing old entries are allowed, new ones are not."""
    pending = state.get('pending_removal', {})
    if not pending:
        return state
    with costs.phase('retry-owned-payload-cleanup'):
        for name, entries in pending.items():
            trash = store / name
            if not os.path.lexists(trash):
                continue
            require(trash.is_dir() and not trash.is_symlink(), 'pending removal root changed')
            require({p.name for p in trash.iterdir()} <= set(entries),
                    'unrelated file in pending removal root')
            for slot, original in entries.items():
                path = trash / slot
                if not os.path.lexists(path):
                    continue
                require(path.is_dir() and not path.is_symlink(), 'pending slot root changed')
                expected = {row['path']: row for row in original}
                current = tree_inventory(path)
                require(all(expected.get(row['path']) == row for row in current),
                        'changed or unrelated file in pending removal payload')
            shutil.rmtree(trash)
        updated = dict(state, pending_removal={})
        write_state(store, updated)
        return updated


def execute(args):
    """Return a JSON-compatible result; failures never select an unverified candidate."""
    costs = None
    result = {'schema': 1, 'operation': args.action,
              'status': 'FAIL_PRIVATE_SLOTS_FILES_ONLY', 'public_distribution_status': 'PUBLIC_NOT_READY',
              'runtime_ABI_Kodi_GPU_hardware_AV_acceptance': 'NOT_RUN',
              'scope': 'Owned private files and atomic selection JSON only; no target code/network/devices, live-consumer switch, power-loss or cross-filesystem durability claim.'}
    try:
        store = checked_path(args.store)
        costs = Costs(store)
        if not store.exists() and args.action in ('status', 'remove'):
            result.update(active=None, previous=None, active_prefix=None,
                          previous_prefix=None, retained_slots=[], store_created=False,
                          status='PASS_PRIVATE_SLOTS_FILES_ONLY')
            return result
        require(store.exists() or args.action == 'install',
                'operation requires an existing owned store')
        with costs.phase('serialized-operation'), locked_store(store) as (state, created):
            result['store_created'] = created
            result.update(selection(store, state))
            require(args.action == 'remove' or not state.get('pending_removal'),
                    'owned payload deletion is pending; run remove to retry cleanup')
            if args.action in ('install', 'update'):
                require((state['active'] is None) == (args.action == 'install'),
                        'install requires no active slot; update requires an active slot')
                slot = args.archive_sha256
                require(SHA.fullmatch(slot) and slot not in state['slots'],
                        'slot already retained or invalid identity; use rollback for the previous slot')
                require(len(state['slots']) < MAX_SLOTS, 'retained slot limit; remove this private store before starting another series')
                record = {'archive_sha256': slot,
                    'metadata_sha256': {'files': args.files_sha256,
                        'dependencies': args.dependencies_sha256, 'provenance': args.provenance_sha256},
                    'rootfs': str(Path(args.rootfs).resolve(strict=True)),
                    'rootfs_identity': args.rootfs_identity, 'platform_relative': args.platform_relative}
                require(isinstance(args.rootfs_identity, str) and
                        len(args.rootfs_identity) <= 4096 and len(record['rootfs']) <= 4096,
                        'provider selection text exceeds bound')
                stage = Path(tempfile.mkdtemp(prefix='.stage-', dir=store))
                candidate = store / 'slots' / slot
                try:
                    with costs.phase('extract-candidate'):
                        extract_archive(checked_path(args.archive), stage, slot, costs)
                    with costs.phase('validate-candidate-files-and-providers'):
                        result['candidate_audit'] = audit_slot(stage, record)
                        record['owned_tree_sha256'] = tree_fingerprint(stage)
                    updated = json.loads(json.dumps(state))
                    updated['slots'][slot] = record
                    updated['previous'], updated['active'] = state['active'], slot
                    with costs.phase('publish-selection'):
                        stage.rename(candidate)
                        try:
                            write_state(store, updated)
                        except BaseException:
                            # The leaf writer has no fallible work after replace.
                            # Preserve a candidate if externally altered state makes
                            # its publication uncertain rather than deleting selected files.
                            actual = regular_json(store / 'state.json')
                            if actual == state:
                                shutil.rmtree(candidate)
                            elif actual == updated:
                                result['selection_committed'] = True
                                result.update(selection(store, updated))
                            raise
                    state = updated
                    result['selection_committed'] = True
                finally:
                    if stage.exists():
                        shutil.rmtree(stage)
            elif args.action == 'rollback':
                require(state['previous'] is not None, 'no previous slot to restore')
                with costs.phase('revalidate-previous-files-and-providers'):
                    result['candidate_audit'] = audit_slot(store / 'slots' / state['previous'],
                                                            state['slots'][state['previous']])
                updated = dict(state)
                updated['active'], updated['previous'] = state['previous'], state['active']
                with costs.phase('publish-rollback'):
                    write_state(store, updated)
                state = updated
                result['selection_committed'] = True
            elif args.action == 'status':
                if state['active'] is not None:
                    with costs.phase('revalidate-active-files-and-providers'):
                        result['active_audit'] = audit_slot(store / 'slots' / state['active'],
                                                         state['slots'][state['active']])
            elif args.action == 'remove':
                state = finish_pending_removal(store, state, costs)
                result.update(selection(store, state))
                with costs.phase('validate-owned-payload-before-removal'):
                    for slot, record in state['slots'].items():
                        owned_payload_matches(store / 'slots' / slot, record)
                if state['slots']:
                    owned = {slot: tree_inventory(store / 'slots' / slot)
                             for slot in state['slots']}
                    trash = Path(tempfile.mkdtemp(prefix='.remove-', dir=store))
                    updated = dict(state, active=None, previous=None, slots={},
                                   pending_removal={trash.name: owned})
                    moved = []
                    try:
                        # Check the complete pending-deletion receipt before
                        # moving any payload or publishing an inactive state.
                        require(len(encoded(updated)) <= 256 * 1024,
                                'pending removal receipt exceeds state bound')
                        with costs.phase('unselect-owned-payload'):
                            try:
                                for slot in state['slots']:
                                    (store / 'slots' / slot).rename(trash / slot)
                                    moved.append(slot)
                                write_state(store, updated)
                            except BaseException:
                                actual = regular_json(store / 'state.json')
                                if actual == state:
                                    for slot in reversed(moved):
                                        (trash / slot).rename(store / 'slots' / slot)
                                elif actual == updated:
                                    # Replacement committed: moving files back
                                    # would contradict the new state. Retain its
                                    # tracked cleanup receipt for the next remove.
                                    result['selection_committed'] = True
                                    result.update(selection(store, updated))
                                raise
                        state = updated
                        result['selection_committed'] = True
                        result.update(selection(store, state))
                        state = finish_pending_removal(store, state, costs)
                    finally:
                        if trash.exists() and not any(trash.iterdir()):
                            trash.rmdir()
            else:
                raise ValueError('unknown operation')
            result.update(selection(store, state))
            result['status'] = 'PASS_PRIVATE_SLOTS_FILES_ONLY'
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, tarfile.TarError,
            KeyboardInterrupt) as error:
        result['error'] = str(error) or type(error).__name__
    finally:
        if costs is not None:
            result['cost'] = costs.report()
    return result


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--store', type=Path, required=True)
    commands = result.add_subparsers(dest='action', required=True)
    for name in ('install', 'update'):
        command = commands.add_parser(name)
        command.add_argument('--archive', type=Path, required=True)
        command.add_argument('--archive-sha256', required=True)
        command.add_argument('--rootfs', type=Path, required=True)
        command.add_argument('--rootfs-identity', required=True)
        command.add_argument('--platform-relative', default='usr')
        for kind in ('files', 'dependencies', 'provenance'):
            command.add_argument('--' + kind + '-sha256', required=True)
    for name in ('rollback', 'status', 'remove'):
        commands.add_parser(name)
    return result


def main():
    result = execute(parser().parse_args())
    print(json.dumps(result, sort_keys=True, indent=2))
    return 0 if result['status'].startswith('PASS_') else 1


if __name__ == '__main__':
    raise SystemExit(main())
