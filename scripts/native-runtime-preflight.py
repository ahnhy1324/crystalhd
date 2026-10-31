#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check pinned private-runtime files against a separately extracted target.

This standard-library preflight does not load ELF files, execute target code,
mount namespaces, open devices or install anything. It consumes #576 manifests;
their SHA-256 pins must come from a separately reviewed receipt. A PASS describes
only inspected files, never an independently clean OS or native Kodi playback.
The selected package and rootfs must be immutable during inspection.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys


MAX_FILE_BYTES = 512 * 1024 * 1024
MAX_JSON_BYTES = 2 * 1024 * 1024
KINDS = ('avutil', 'avcodec', 'avformat', 'swresample', 'swscale', 'avfilter', 'avdevice')
TOOLS = ('cc', 'gcc', 'g++', 'c++', 'clang', 'clang++', 'make', 'nasm', 'yasm',
         'meson', 'ninja', 'c89', 'c99')
TOOL_PATTERN = re.compile(r'(?:(?:[A-Za-z0-9_]+-)*)(?:gcc|g\+\+|clang|clang\+\+)(?:-[0-9.]+)?\Z')
TOOL_DIRS = ('usr/bin', 'usr/sbin', 'bin', 'sbin', 'usr/local/bin')
SHA = re.compile(r'[0-9a-f]{64}\Z')


def require(okay, message):
    if not okay:
        raise ValueError(message)


def relative_name(value):
    require(isinstance(value, str) and value and '\\' not in value,
            'invalid relative manifest path')
    path = PurePosixPath(value)
    require(bool(path.parts) and not path.is_absolute() and str(path) == value and
            all(part not in ('.', '..') for part in path.parts),
            'manifest path is not canonical and relative: ' + value)
    return path


def inside(root, relative):
    """Resolve rootfs symlinks with absolute targets interpreted inside rootfs."""
    todo = list(relative_name(relative).parts)
    resolved, links = [], 0
    while todo:
        part = todo.pop(0)
        if part in ('', '.'):
            continue
        if part == '..':
            require(bool(resolved), 'rootfs link escapes target root')
            resolved.pop()
            continue
        candidate = root.joinpath(*resolved, part)
        info = candidate.lstat()
        if stat.S_ISLNK(info.st_mode):
            links += 1
            require(links <= 40, 'rootfs symlink cycle or excessive links')
            target = PurePosixPath(os.readlink(str(candidate)))
            if target.is_absolute():
                resolved = []
            todo = [p for p in target.parts if p != '/'] + todo
        else:
            require(not todo or stat.S_ISDIR(info.st_mode),
                    'non-directory in target path')
            resolved.append(part)
    return root.joinpath(*resolved)


def regular_info(path, maximum=MAX_FILE_BYTES):
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_size <= maximum,
            'not a bounded regular file: ' + str(path))
    descriptor = os.open(str(path), os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_size <= maximum,
                'not a bounded regular file: ' + str(path))
        require((info.st_dev, info.st_ino) == (before.st_dev, before.st_ino),
                'file changed before inspection')
        digest, size, header = hashlib.sha256(), 0, b''
        while True:
            data = stream.read(1024 * 1024)
            if not data:
                break
            if not header:
                header = data[:64]
            size += len(data)
            require(size <= maximum, 'file grew beyond inspection bound')
            digest.update(data)
        require(size == info.st_size, 'file size changed during inspection')
    return {'bytes': size, 'mode': stat.S_IMODE(info.st_mode),
            'sha256': digest.hexdigest(), 'header': header}


def pinned_json(path, expected):
    require(isinstance(expected, str) and SHA.fullmatch(expected),
            'expected metadata pin must be a SHA-256')
    actual = regular_info(path, MAX_JSON_BYTES)
    require(actual['sha256'] == expected, 'reviewed metadata SHA-256 differs: ' + path.name)
    with path.open('r', encoding='utf-8') as stream:
        value = json.load(stream)
    return value


def elf64_x86(info):
    header = info['header']
    require(len(header) >= 20 and header[:7] == b'\x7fELF\x02\x01\x01' and
            int.from_bytes(header[18:20], 'little') == 62,
            'payload/provider is not little-endian x86_64 ELF64')


def package_files(root):
    names = set()
    for directory, directories, files in os.walk(str(root), followlinks=False):
        for name in directories:
            path = Path(directory) / name
            require(not path.is_symlink(), 'package directory symlink is unsupported')
        for name in files:
            names.add((Path(directory) / name).relative_to(root).as_posix())
            require(len(names) <= 256, 'package has too many members')
    return names


def compiler_candidates(root):
    result = []
    for directory in TOOL_DIRS:
        try:
            path = inside(root, directory)
        except FileNotFoundError:
            continue
        require(path.is_dir(), 'compiler search path is not a directory')
        for entry in path.iterdir():
            if entry.name in TOOLS or TOOL_PATTERN.fullmatch(entry.name):
                result.append(directory + '/' + entry.name)
    return sorted(set(result))


def audit(package, rootfs, files_pin, dependencies_pin, provenance_pin,
          rootfs_identity, platform_relative='usr'):
    package, rootfs = Path(package).resolve(strict=True), Path(rootfs).resolve(strict=True)
    require(package.is_dir() and rootfs.is_dir() and rootfs != Path('/'),
            'select an extracted package and an offline rootfs directory')
    require(package != rootfs and package not in rootfs.parents and rootfs not in package.parents,
            'package and rootfs must be separate trees')
    require(isinstance(rootfs_identity, str) and rootfs_identity.strip(),
            'record a declared rootfs image/package identity')
    platform_relative = ('.' if platform_relative == '.' else
                         str(relative_name(platform_relative)))
    # Reject package directory aliases before any metadata path is opened.
    actual_names = package_files(package)
    files = pinned_json(package / 'metadata/files.json', files_pin)
    dependencies = pinned_json(package / 'metadata/dependencies.json', dependencies_pin)
    provenance = pinned_json(package / 'metadata/provenance.json', provenance_pin)
    require(isinstance(files, list) and 0 < len(files) <= 256, 'invalid file manifest')
    require(isinstance(dependencies, dict) and dependencies.get('schema') == 1,
            'unsupported dependency manifest')
    require(isinstance(provenance, dict) and provenance.get('schema') == 1 and
            provenance.get('disposition') == 'PUBLIC_NOT_READY',
            'private research provenance/disposition is required')
    checked, declared = {}, set()
    for entry in files:
        require(isinstance(entry, dict), 'invalid manifest member')
        name = str(relative_name(entry.get('path')))
        require(name not in declared and name != 'metadata/files.json', 'duplicate/self manifest entry')
        declared.add(name)
        path = package / name
        if entry.get('kind') == 'symlink':
            target = entry.get('target')
            require(isinstance(target, str) and target not in ('.', '..') and
                    '/' not in target and str(relative_name(target)) == target,
                    'package links must have one relative filename target')
            require(path.is_symlink() and os.readlink(str(path)) == target,
                    'package SONAME link differs: ' + name)
            target_path = path.parent / target
            require(target_path.is_file() and not target_path.is_symlink(),
                    'package SONAME link target is not a real file')
        else:
            require(entry.get('kind') == 'file', 'unsupported manifest member kind')
            info = regular_info(path)
            require(all(info[field] == entry.get(field) for field in ('bytes', 'mode', 'sha256')),
                    'package file/hash/mode differs: ' + name)
            checked[name] = info
    require(actual_names == declared | {'metadata/files.json'}, 'unmanifested or missing package member')
    require(dependencies.get('architecture') == ['ELF64', 'Advanced Micro Devices X86-64'],
            'unsupported dependency ABI')
    require(set(dependencies.get('sonames', {})) == set(KINDS), 'expected all seven FFmpeg DSOs')
    nodes = dependencies.get('nodes')
    require(isinstance(nodes, dict) and len(nodes) <= 128, 'invalid dependency nodes')
    payload, providers, provider_bytes = set(), [], 0
    provider_paths, interpreters = {}, set()
    for key, node in sorted(nodes.items()):
        require(isinstance(node, dict), 'invalid dependency node')
        name = str(relative_name(node.get('file')))
        require(node.get('class') == 'ELF64' and
                node.get('machine') == 'Advanced Micro Devices X86-64', 'dependency ABI differs')
        if node.get('origin') == 'payload':
            require(key == 'payload:' + name and name in checked, 'unmanifested payload node')
            payload.add(name)
            info = checked[name]
        else:
            require(node.get('origin') == 'platform' and key == 'platform:' + name,
                    'invalid target provider origin')
            target_name = name if platform_relative == '.' else platform_relative + '/' + name
            provider_path = inside(rootfs, target_name)
            info = regular_info(provider_path)
            sonames = node.get('soname')
            require(isinstance(sonames, list) and len(sonames) == 1 and
                    isinstance(sonames[0], str) and '/' not in sonames[0],
                    'target provider must declare one SONAME')
            soname = str(relative_name(sonames[0]))
            require(soname not in provider_paths, 'duplicate target provider SONAME')
            provider_paths[soname] = provider_path
            candidates = set()
            for directory in ('lib/x86_64-linux-gnu', 'lib', 'lib64'):
                alias = directory + '/' + soname
                alias = alias if platform_relative == '.' else platform_relative + '/' + alias
                try:
                    candidates.add(inside(rootfs, alias))
                except FileNotFoundError:
                    pass
            require(candidates == {provider_path},
                    'target SONAME aliases missing, changed or ambiguous: ' + soname)
            provider_bytes += info['bytes']
            providers.append({'path': target_name,
                              'sha256': info['sha256'], 'bytes': info['bytes']})
        require(info['bytes'] == node.get('bytes') and info['sha256'] == node.get('sha256'),
                'pinned dependency bytes differ: ' + key)
        elf64_x86(info)
        if node.get('interpreter'):
            interpreters.add(node['interpreter'])
    for interpreter in interpreters:
        require(isinstance(interpreter, str) and interpreter.startswith('/'),
                'ELF interpreter must be an absolute target path')
        interpreter_name = str(relative_name(interpreter[1:]))
        expected_provider = provider_paths.get(PurePosixPath(interpreter_name).name)
        require(expected_provider is not None and
                inside(rootfs, interpreter_name) == expected_provider,
                'target ELF interpreter does not select its pinned provider')
    expected_payload = {name for name in checked if name.startswith(('bin/', 'lib/'))}
    require(payload == expected_payload and len(payload) == 9,
            'dependency manifest must cover exactly seven real DSOs and two CLIs')
    for kind, soname in dependencies['sonames'].items():
        require(isinstance(soname, str) and re.fullmatch(r'lib' + kind + r'\.so\.\d+', soname),
                'unexpected FFmpeg SONAME')
        require('lib/' + soname in declared, 'missing FFmpeg SONAME link')
    candidates = compiler_candidates(rootfs)
    require(not candidates, 'compiler/build tools found at inspected target paths: ' + ', '.join(candidates))
    return {'schema': 1, 'status': 'PASS_PINNED_TARGET_FILES_ONLY',
            'public_distribution_status': 'PUBLIC_NOT_READY',
            'rootfs_identity_declared': rootfs_identity,
            'rootfs_identity_authenticated_by_this_tool': False,
            'metadata_sha256': {'files': files_pin, 'dependencies': dependencies_pin,
                                'provenance': provenance_pin},
            'package_members': len(declared), 'payload_ELFs': len(payload),
            'payload_bytes': sum(checked[name]['bytes'] for name in payload),
            'target_providers': providers, 'target_provider_bytes': provider_bytes,
            'target_provider_SONAME_aliases_and_interpreters_match': True,
            'compiler_search_directories': list(TOOL_DIRS),
            'compiler_names_and_versioned_cross_compiler_variants': list(TOOLS),
            'compiler_candidates_found': candidates,
            'ELF_dependency_symbols': 'NOT_RECOMPUTED; exact bytes match pinned #576 inventory',
            'clean_OS_Kodi_GPU_hardware_media_AV_acceptance': 'NOT_RUN',
            'scope': 'Offline immutable files only. No target code, loader, namespaces, devices, '
                     'installation or publication executed. Absence of inspected compiler names '
                     'does not prove a running namespace is compilerless.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--rootfs', type=Path, required=True)
    parser.add_argument('--rootfs-identity', required=True)
    parser.add_argument('--platform-relative', default='usr',
                        help='provider subtree in rootfs; use . for a Platform files tree')
    parser.add_argument('--files-sha256', required=True)
    parser.add_argument('--dependencies-sha256', required=True)
    parser.add_argument('--provenance-sha256', required=True)
    args = parser.parse_args()
    report = audit(args.package, args.rootfs, args.files_sha256, args.dependencies_sha256,
                   args.provenance_sha256, args.rootfs_identity, args.platform_relative)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, TypeError, KeyError) as error:
        print('native-runtime-preflight: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
