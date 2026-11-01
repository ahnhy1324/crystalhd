#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Export exact Git source and pinned private runtime build materials.

Trusted immutable inputs only. No recipe, compiler, target or network execution.
Exporting evidence does not qualify source/binary correspondence, reproducible
compilation, sufficient corresponding source, or public distribution.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile

SHA = re.compile(r'[0-9a-f]{64}\Z')
REV = re.compile(r'[0-9a-f]{40}\Z')
MAX_JSON = 2 * 1024 * 1024
MAX_MATERIAL = 16 * 1024 * 1024
MAX_MATERIAL_TOTAL = 64 * 1024 * 1024
MAX_SOURCE_FILE = 32 * 1024 * 1024
MAX_SOURCE_TOTAL = 256 * 1024 * 1024
MAX_SOURCE_FILES = 20000
MAX_ARCHIVE = 512 * 1024 * 1024
ROLES = {'recipe', 'configuration', 'toolchain', 'notice', 'acquisition'}
METADATA = ('files', 'dependencies', 'provenance')
OUTPUTS = ('source-materials.tar', 'materials-manifest.json', 'SHA256SUMS')


def require(okay, message):
    if not okay:
        raise ValueError(message)


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode('utf-8')


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def pin(value):
    require(isinstance(value, str) and SHA.fullmatch(value), 'invalid SHA-256 pin')
    return value


def relative(value):
    require(isinstance(value, str) and value and '\\' not in value and
            not any(ord(c) < 32 or ord(c) == 127 for c in value), 'invalid relative path')
    path = PurePosixPath(value)
    require(bool(path.parts) and not path.is_absolute() and str(path) == value and
            all(p not in ('.', '..') and p.lower() != '.git' for p in path.parts),
            'path must be canonical, relative and outside .git')
    tarfile.TarInfo(value).tobuf(format=tarfile.USTAR_FORMAT, encoding='utf-8')
    return value


def absolute(value, missing_leaf=False):
    require(isinstance(value, (str, Path)), 'path must be a string or Path')
    path = Path(value)
    require(path.is_absolute() and str(path) == str(value) and '..' not in path.parts,
            'path must be canonical and absolute')
    current = Path(path.anchor)
    for index, part in enumerate(path.parts[1:]):
        current /= part
        try:
            mode = current.lstat().st_mode
        except FileNotFoundError:
            require(missing_leaf and index == len(path.parts) - 2, 'missing path component')
            return path
        require(not stat.S_ISLNK(mode), 'symlink path component')
        if current != path:
            require(stat.S_ISDIR(mode), 'non-directory path component')
    return path


def regular(path, maximum, retain=True):
    path = absolute(path)
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and
            before.st_size <= maximum, 'not a bounded single-link regular file')
    fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        start = os.fstat(stream.fileno())
        require((before.st_dev, before.st_ino) == (start.st_dev, start.st_ino) and
                stat.S_ISREG(start.st_mode) and start.st_nlink == 1, 'input changed before inspection')
        parts, size, digest = [], 0, hashlib.sha256()
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            require(size <= maximum, 'input exceeds size bound')
            digest.update(chunk)
            if retain:
                parts.append(chunk)
        end = os.fstat(stream.fileno())
    after = path.lstat()
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns, s.st_nlink)
    require(identity(start) == identity(end) == identity(after) and size == start.st_size,
            'input changed during inspection')
    return (b''.join(parts) if retain else None, {'bytes': size, 'sha256': digest.hexdigest()})


def pinned(path, expected, maximum=MAX_JSON):
    raw, info = regular(path, maximum)
    require(info['sha256'] == pin(expected), 'reviewed input SHA-256 mismatch')
    return raw


def decoded(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate JSON key')
            result[key] = value
        return result

    def constant(value):
        raise ValueError('non-finite JSON value')

    return json.loads(raw.decode('utf-8'), object_pairs_hook=pairs, parse_constant=constant)


def keys(value, expected, description):
    require(isinstance(value, dict) and set(value) == set(expected),
            'unknown or missing ' + description + ' fields')


def git_env():
    env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
    env.update(GIT_OPTIONAL_LOCKS='0', GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
               GIT_NO_REPLACE_OBJECTS='1')
    return env


def git_command(source, *args):
    return ['git', '--no-optional-locks', '-c', 'core.fsmonitor=false',
            '-c', 'core.hooksPath=' + os.devnull, '-C', str(source), *args]


def git(source, *args):
    result = subprocess.run(git_command(source, *args), env=git_env(), stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    require(result.returncode == 0, 'read-only Git inspection failed: ' + args[0])
    require(len(result.stdout) <= 16 * 1024 * 1024, 'Git listing exceeds inspection bound')
    return result.stdout


def clean_source(source, revision):
    require(isinstance(revision, str) and REV.fullmatch(revision), 'source revision must be a full SHA-1')
    require(source.is_dir(), 'source checkout must be a directory')
    require(git(source, 'rev-parse', '--show-toplevel').decode().strip() == str(source),
            'source must be the exact Git checkout root')
    require(git(source, 'rev-parse', 'HEAD').decode().strip() == revision, 'source checkout revision mismatch')
    require(git(source, 'cat-file', '-t', revision).strip() == b'commit', 'source revision is not a commit')
    require(not git(source, 'status', '--porcelain=v1', '--untracked-files=all',
                    '--ignore-submodules=none').strip(), 'source checkout is dirty or has untracked files')


def source_inventory(source, revision):
    clean_source(source, revision)
    entries, total = [], 0
    for record in git(source, 'ls-tree', '-r', '-l', '-z', revision).split(b'\x00'):
        if not record:
            continue
        fields, name = record.split(b'\t', 1)
        mode, kind, oid, size = fields.split()
        require(mode in (b'100644', b'100755') and kind == b'blob',
                'source symlinks, submodules or non-blob entries are unsupported')
        name = relative(name.decode('utf-8'))
        relative('source/' + name)
        require(REV.fullmatch(oid.decode()) and size.isdigit(), 'malformed Git blob inventory')
        size = int(size)
        require(size <= MAX_SOURCE_FILE, 'source blob exceeds size bound')
        total += size
        entries.append({'path': name, 'oid': oid.decode(), 'bytes': size,
                        'mode': 0o755 if mode == b'100755' else 0o644})
        require(len(entries) <= MAX_SOURCE_FILES and total <= MAX_SOURCE_TOTAL,
                'source export exceeds file/byte bound')
    require(entries and len({e['path'] for e in entries}) == len(entries), 'empty or duplicate source inventory')
    tree = git(source, 'rev-parse', revision + '^{tree}').decode().strip()
    require(REV.fullmatch(tree), 'invalid Git tree identity')
    return sorted(entries, key=lambda e: e['path']), tree, total


class Blobs:
    def __init__(self, source):
        self.process = subprocess.Popen(git_command(source, 'cat-file', '--batch'), env=git_env(),
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL)

    def read(self, entry):
        self.process.stdin.write((entry['oid'] + '\n').encode('ascii'))
        self.process.stdin.flush()
        header = self.process.stdout.readline(256).split()
        require(header == [entry['oid'].encode(), b'blob', str(entry['bytes']).encode()],
                'Git blob response differs from inventory')
        raw = self.process.stdout.read(entry['bytes'])
        require(len(raw) == entry['bytes'] and self.process.stdout.read(1) == b'\n', 'truncated Git blob response')
        identity = hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\x00' + raw).hexdigest()
        require(identity == entry['oid'], 'Git blob object identity mismatch')
        return raw

    def close(self):
        self.process.stdin.close()
        self.process.stdout.close()
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait()


def file_inventory(raw):
    entries = decoded(raw)
    require(isinstance(entries, list) and 0 < len(entries) <= 256, 'invalid package file inventory')
    files = {}
    for entry in entries:
        require(isinstance(entry, dict), 'invalid package file entry')
        name = relative(entry.get('path'))
        require(name not in files, 'duplicate package path')
        if entry.get('kind') == 'file':
            keys(entry, ('path', 'kind', 'mode', 'bytes', 'sha256'), 'package file')
            require(type(entry['bytes']) is int and 0 <= entry['bytes'] <= MAX_ARCHIVE and
                    type(entry['mode']) is int and entry['mode'] in (0o644, 0o755),
                    'invalid package regular file facts')
            pin(entry['sha256'])
        elif entry.get('kind') == 'symlink':
            keys(entry, ('path', 'kind', 'target'), 'package symlink')
            target = relative(entry['target'])
            require('/' not in target and name.startswith('lib/'), 'invalid package SONAME link')
        else:
            raise ValueError('unknown package file kind')
        files[name] = entry
    return files


def prepare(package, archive, metadata_pins, archive_pin, spec_path, spec_pin, revision):
    require(package.is_dir(), 'package root must be a directory')
    _, archive_info = regular(archive, MAX_ARCHIVE, retain=False)
    require(archive_info['sha256'] == pin(archive_pin), 'binary archive SHA-256 mismatch')
    raw_metadata = {name: pinned(package / 'metadata' / (name + '.json'), metadata_pins[name])
                    for name in METADATA}
    files = file_inventory(raw_metadata['files'])
    provenance = decoded(raw_metadata['provenance'])
    dependencies = decoded(raw_metadata['dependencies'])
    require(isinstance(dependencies, dict) and type(dependencies.get('schema')) is int and
            dependencies['schema'] == 1 and isinstance(dependencies.get('nodes'), dict),
            'invalid dependency metadata')
    require(isinstance(provenance, dict) and type(provenance.get('schema')) is int and
            provenance['schema'] == 1 and provenance.get('disposition') == 'PUBLIC_NOT_READY',
            'provenance must retain schema 1 and PUBLIC_NOT_READY')
    require(provenance.get('source_revision') == revision, 'declared package source revision mismatch')
    require(provenance.get('source_tracked_changes') is False,
            'package source tracked-change declaration is missing or not false')
    for name in ('dependencies', 'provenance'):
        entry = files.get('metadata/' + name + '.json')
        require(entry is not None and entry['kind'] == 'file' and
                entry['sha256'] == metadata_pins[name] and entry['bytes'] == len(raw_metadata[name]),
                'package metadata inventory mismatch')
    additions = {'package-metadata/' + name + '.json': (raw_metadata[name], 'package-metadata')
                 for name in METADATA}
    copied_bytes = sum(len(raw) for raw, _ in additions.values())
    for name, entry in files.items():
        if name.startswith('licenses/'):
            require(entry['kind'] == 'file', 'package notice must be a regular file')
            require(entry['bytes'] <= MAX_MATERIAL and copied_bytes + entry['bytes'] <= MAX_MATERIAL_TOTAL,
                    'package notices exceed material byte bound')
            raw = pinned(package / name, entry['sha256'], MAX_MATERIAL)
            require(len(raw) == entry['bytes'], 'package notice size mismatch')
            additions['package-notices/' + name.removeprefix('licenses/')] = (raw, 'package-notice')
            copied_bytes += len(raw)
    require('package-notices/NOTICE' in additions, 'private package NOTICE is required')
    notices = provenance.get('notices')
    require(isinstance(notices, list) and 0 < len(notices) <= 64, 'missing source notice inventory')
    checks = {}
    for notice in notices:
        require(isinstance(notice, dict), 'invalid source notice entry')
        name = relative(notice.get('source'))
        require(name not in checks and isinstance(notice.get('url'), str), 'duplicate or malformed source notice')
        if 'sha256' in notice:
            keys(notice, ('source', 'url', 'sha256'), 'source notice')
            expected, copied = pin(notice['sha256']), 'package-notices/' + name
            require(copied in additions and sha(additions[copied][0]) == expected,
                    'copied source notice differs from provenance')
            checks[name] = (expected, None)
        else:
            keys(notice, ('source', 'url', 'source_sha256', 'notice_sha256'), 'component notice')
            expected, notice_hash = pin(notice['source_sha256']), pin(notice['notice_sha256'])
            copied = 'package-notices/' + PurePosixPath(name).name + '.notice'
            require(copied in additions and sha(additions[copied][0]) == notice_hash,
                    'copied component notice differs from provenance')
            checks[name] = (expected, notice_hash)
    spec = decoded(pinned(spec_path, spec_pin))
    keys(spec, ('schema', 'materials'), 'materials specification')
    require(type(spec['schema']) is int and spec['schema'] == 1 and
            isinstance(spec['materials'], list) and 0 < len(spec['materials']) <= 64,
            'invalid materials specification')
    roles, selected, total = set(), [], copied_bytes
    for item in spec['materials']:
        keys(item, ('path', 'archive_path', 'sha256', 'role'), 'material')
        name = relative(item['archive_path'])
        require(name.startswith('materials/') and name not in additions and
                isinstance(item['role'], str) and item['role'] in ROLES,
                'unknown role, duplicate or non-material archive path')
        raw = pinned(absolute(item['path']), item['sha256'], MAX_MATERIAL)
        additions[name] = (raw, item['role'])
        roles.add(item['role'])
        selected.append({'path': name, 'role': item['role'], 'bytes': len(raw), 'sha256': sha(raw)})
        total += len(raw)
        require(total <= MAX_MATERIAL_TOTAL, 'selected materials exceed total byte bound')
    require({'recipe', 'configuration'} <= roles, 'recipe and configuration materials are required')
    names = set(additions)
    require(not any(str(parent) in names for name in names for parent in PurePosixPath(name).parents
                    if str(parent) != '.'), 'archive file/directory path collision')
    return additions, checks, archive_info, selected


def tar_add(archive, name, raw, mode):
    relative(name)
    member = tarfile.TarInfo(name)
    member.size, member.mode, member.mtime = len(raw), mode, 0
    member.uid = member.gid = 0
    member.uname = member.gname = ''
    archive.addfile(member, io.BytesIO(raw))


def export(source_checkout, source_revision, package_root, package_archive,
           expected_archive_sha256, expected_files_sha256, expected_dependencies_sha256,
           expected_provenance_sha256, materials_spec, expected_materials_spec_sha256, output_dir):
    source = absolute(source_checkout)
    package = absolute(package_root)
    archive = absolute(package_archive)
    spec = absolute(materials_spec)
    output = absolute(output_dir, missing_leaf=True)
    require(not output.exists(), 'output must be a new directory')
    require(source not in output.parents and package not in output.parents,
            'output must be outside source and package roots')
    pins = {'files': pin(expected_files_sha256), 'dependencies': pin(expected_dependencies_sha256),
            'provenance': pin(expected_provenance_sha256)}
    entries, tree, source_bytes = source_inventory(source, source_revision)
    additions, notices, archive_info, selected = prepare(
        package, archive, pins, expected_archive_sha256, spec, expected_materials_spec_sha256, source_revision)
    require(set(notices) <= {e['path'] for e in entries}, 'notice source is absent from selected Git tree')
    members, owned, directory_identity = [], {}, None
    try:
        output.mkdir(mode=0o700)
        directory_identity = output.lstat()
        tar_path = output / OUTPUTS[0]
        with tar_path.open('xb') as stream:
            owned[tar_path] = tar_path.lstat()
            with tarfile.open(fileobj=stream, mode='w', format=tarfile.USTAR_FORMAT) as result:
                blobs = Blobs(source)
                try:
                    for entry in entries:
                        raw = blobs.read(entry)
                        if entry['path'] in notices:
                            expected, notice_hash = notices[entry['path']]
                            require(sha(raw) == expected, 'source notice bytes differ from pinned Git tree')
                            if notice_hash is not None:
                                require(raw.startswith(b'/*') and b'*/' in raw, 'component source has no initial notice')
                                initial = raw[:raw.index(b'*/') + 2] + b'\n'
                                require(sha(initial) == notice_hash, 'component source notice mismatch')
                        name = 'source/' + entry['path']
                        tar_add(result, name, raw, entry['mode'])
                        members.append({'path': name, 'role': 'source', 'bytes': len(raw),
                                        'mode': entry['mode'], 'sha256': sha(raw)})
                finally:
                    blobs.close()
                for name, (raw, role) in sorted(additions.items()):
                    tar_add(result, name, raw, 0o644)
                    members.append({'path': name, 'role': role, 'bytes': len(raw), 'mode': 0o644, 'sha256': sha(raw)})
        clean_source(source, source_revision)
        _, tar_info = regular(tar_path, MAX_ARCHIVE, retain=False)
        manifest = {'schema': 1, 'format': 'crystalhd-private-native-runtime-materials',
                    'status': 'PASS_PINNED_MATERIAL_EXPORT_ONLY',
                    'public_distribution_status': 'PUBLIC_NOT_READY',
                    'source_revision': source_revision, 'source_tree': tree,
                    'source_files': len(entries), 'source_bytes': source_bytes,
                    'source_archive_selection': 'EXACT_COMMIT_REGULAR_BLOBS_ONLY',
                    'binary_package_archive': archive_info, 'package_metadata_sha256': pins,
                    # Normalize selections so local absolute input locations do
                    # not change artifacts. The command receipt holds the spec pin.
                    'materials_selection_sha256': sha(encoded(sorted(selected, key=lambda item: item['path']))),
                    'selected_materials': sorted(selected, key=lambda item: item['path']),
                    'archive': {'name': OUTPUTS[0], **tar_info},
                    'members': sorted(members, key=lambda item: item['path']),
                    'source_correspondence_to_binary': 'UNVERIFIED',
                    'redistribution_corresponding_source_sufficiency': 'UNVERIFIED',
                    'reproducible_compilation': 'NOT_VERIFIED', 'historical_recipe_execution': 'NOT_RUN',
                    'historical_materials_may_embed_absolute_paths': True,
                    'original_package_declarations': 'COPIED_UNCHANGED',
                    'binary_archive_metadata_binding': 'REVIEWED_INPUT_PINS_NOT_ARCHIVE_CONTENT_INSPECTION',
                    'dependency_source_and_notice_coverage': 'NOT_REVIEWED',
                    'minimum_CPU_and_general_ABI_qualification': 'NOT_RUN',
                    'clean_OS_Kodi_hardware_media_qualification': 'NOT_RUN',
                    'update_maintenance_owner': 'UNASSIGNED'}
        manifest_raw = encoded(manifest)
        manifest_path = output / OUTPUTS[1]
        with manifest_path.open('xb') as stream:
            owned[manifest_path] = manifest_path.lstat()
            stream.write(manifest_raw)
        sums = (sha(manifest_raw) + '  ' + OUTPUTS[1] + '\n' + tar_info['sha256'] + '  ' + OUTPUTS[0] + '\n').encode('ascii')
        sum_path = output / OUTPUTS[2]
        with sum_path.open('xb') as stream:
            owned[sum_path] = sum_path.lstat()
            stream.write(sums)
        return manifest
    except BaseException:
        # Never remove preexisting or concurrently substituted paths/files.
        for path, identity in reversed(list(owned.items())):
            try:
                actual = path.lstat()
                if (actual.st_dev, actual.st_ino) == (identity.st_dev, identity.st_ino):
                    path.unlink()
            except FileNotFoundError:
                pass
        if directory_identity is not None:
            try:
                actual = output.lstat()
                if (actual.st_dev, actual.st_ino) == (directory_identity.st_dev, directory_identity.st_ino):
                    output.rmdir()
            except OSError:
                pass
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source-checkout', 'source-revision', 'package-root', 'package-archive',
                 'expected-archive-sha256', 'expected-files-sha256', 'expected-dependencies-sha256',
                 'expected-provenance-sha256', 'materials-spec', 'expected-materials-spec-sha256', 'output-dir'):
        parser.add_argument('--' + name, required=True)
    try:
        result = export(**vars(parser.parse_args()))
    except (OSError, ValueError, UnicodeError, tarfile.TarError) as error:
        print('native-runtime-materials: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
