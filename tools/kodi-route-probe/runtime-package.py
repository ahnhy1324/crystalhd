#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create and audit a private matched FFmpeg runtime; never install or publish it.

ELF inspection uses readelf without executing the selected binaries. Only the
seven named FFmpeg libraries, their SONAME links, ffmpeg/ffprobe and inventories
are copied. A pinned target supplies external dependencies; host fallback is
never used. This is research tooling, not a public installer or licence review.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile


KINDS = ('avutil', 'avcodec', 'avformat', 'swresample', 'swscale', 'avfilter', 'avdevice')
SOURCE_REVISION = '12cc1f42f06ff0e85633c2602c96786fbd280b70'
COMPONENT_NOTICES = ('libavcodec/jfdctfst.c', 'libavcodec/jfdctint_template.c',
                     'libavcodec/jrevdct.c')


def require(okay, message):
    if not okay:
        raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + '\n').encode()


def command(argv):
    env = os.environ.copy()
    env['LC_ALL'] = 'C'
    result = subprocess.run(argv, env=env, text=True, capture_output=True,
                            check=False, timeout=30)
    require(result.returncode == 0,
            str(argv[0]) + ' failed: ' + result.stderr.strip()[:1200])
    return result.stdout


def elf(path):
    header = command(['readelf', '-hW', str(path)])
    dynamic = command(['readelf', '-dW', str(path)])
    program = command(['readelf', '-lW', str(path)])
    versions = command(['readelf', '-VW', str(path)])
    symbols = command(['readelf', '--dyn-syms', '-W', str(path)])
    match = re.search(r'^\s*Class:\s+(\S+)', header, re.M)
    machine = re.search(r'^\s*Machine:\s+(.+)', header, re.M)
    require(match is not None and machine is not None, 'ELF identity missing')
    metadata = {'class': match[1], 'machine': machine[1].strip(),
                'needed': re.findall(r'\(NEEDED\).*Shared library: \[([^]]+)\]', dynamic),
                'soname': re.findall(r'\(SONAME\).*Library soname: \[([^]]+)\]', dynamic),
                'rpath': re.findall(r'\((?:RPATH|RUNPATH)\).*\[([^]]*)\]', dynamic)}
    interpreter = re.findall(r'Requesting program interpreter: ([^]]+)', program)
    require(len(interpreter) <= 1, 'multiple ELF interpreters')
    metadata['interpreter'] = interpreter[0] if interpreter else None
    needs, definitions, indices = {}, set(), {}
    section, provider = None, None
    for line in versions.splitlines():
        if line.startswith('Version needs section'):
            section = 'needs'
        elif line.startswith('Version definition section'):
            section = 'definitions'
        elif line.startswith('Version symbols section'):
            section = None
        if section == 'needs':
            found = re.search(r'File: (\S+)\s+Cnt:', line)
            if found:
                provider = found[1]
                needs.setdefault(provider, [])
            found = re.search(r'Name: (\S+)\s+Flags:.*Version: (\d+)', line)
            if found:
                require(provider is not None, 'version requirement has no provider')
                needs[provider].append(found[1])
                indices[int(found[2])] = (provider, found[1])
        elif section == 'definitions':
            found = re.search(r'Name: (\S+)', line)
            if found:
                definitions.add(found[1])
    imports, exports = [], set()
    for line in symbols.splitlines():
        found = re.match(r'\s*\d+:\s+\S+\s+\d+\s+\S+\s+(\S+)\s+\S+\s+(\S+)\s+(\S+)(.*)', line)
        if not found or found[1] not in ('GLOBAL', 'WEAK', 'UNIQUE'):
            continue
        binding, index, name, tail = found.groups()
        parts = re.split(r'@+', name)
        symbol, version = parts[0], parts[1] if len(parts) == 2 else None
        if index == 'UND':
            if binding == 'WEAK':
                continue
            owner = None
            if version:
                number = re.search(r'\((\d+)\)', tail)
                require(number is not None and int(number[1]) in indices,
                        'versioned import has no requirement index: ' + name)
                owner, declared = indices[int(number[1])]
                require(declared == version, 'symbol/version requirement mismatch')
            imports.append((symbol, version, owner))
        else:
            exports.add((symbol, version))
    metadata['version_needs'] = {key: sorted(set(value)) for key, value in sorted(needs.items())}
    metadata['version_definitions'] = sorted(definitions)
    metadata['required_strong_symbols'] = len(imports)
    metadata['dynamic_symbol_inventory_sha256'] = sha(encoded({
        'imports': sorted(imports, key=str), 'exports': sorted(exports, key=str)}))
    return metadata, imports, exports


def selected_payload(prefix):
    prefix = prefix.resolve(strict=True)
    result, sonames = {}, {}
    for kind in KINDS:
        matches = sorted(p for p in (prefix / 'lib').glob('lib' + kind + '.so.*')
                         if p.is_file() and not p.is_symlink())
        require(len(matches) == 1, 'expected one real lib' + kind + '.so.*')
        real = matches[0]
        meta, _, _ = elf(real)
        require(len(meta['soname']) == 1 and
                re.fullmatch(r'lib' + kind + r'\.so\.\d+', meta['soname'][0]),
                'unexpected SONAME for ' + kind)
        soname = meta['soname'][0]
        link = prefix / 'lib' / soname
        require(link.is_symlink() and os.readlink(link) == real.name,
                'SONAME link must point relatively to its exact real library: ' + soname)
        require(real.resolve().parent == (prefix / 'lib').resolve(), 'library leaves prefix')
        result['lib/' + real.name] = real
        result['lib/' + soname] = link
        sonames[kind] = soname
    for name in ('ffmpeg', 'ffprobe'):
        path = prefix / 'bin' / name
        require(path.is_file() and not path.is_symlink(), 'missing regular binary: ' + name)
        result['bin/' + name] = path
    return result, sonames


def inventory(prefix, platform):
    payload, sonames = selected_payload(prefix)
    platform = platform.resolve(strict=True)
    nodes, parsed, by_soname = {}, {}, {}
    pending = []
    identity = None
    for name, path in payload.items():
        if path.is_symlink():
            continue
        key = 'payload:' + name
        meta, imports, exports = elf(path)
        require(not meta['rpath'], 'payload RPATH/RUNPATH is not supported: ' + name)
        current = (meta['class'], meta['machine'])
        identity = identity or current
        require(current == identity, 'payload ELF architecture mismatch: ' + name)
        nodes[key] = {'file': name, 'origin': 'payload', 'bytes': path.stat().st_size,
                      'sha256': sha(path.read_bytes()), **meta}
        parsed[key] = (imports, exports)
        for soname in meta['soname']:
            require(soname not in by_soname, 'duplicate payload SONAME: ' + soname)
            by_soname[soname] = key
        pending.extend(meta['needed'])
        if meta['interpreter']:
            pending.append(Path(meta['interpreter']).name)
    require(identity == ('ELF64', 'Advanced Micro Devices X86-64'),
            'this package experiment is restricted to x86_64 ELF64')
    while pending:
        soname = pending.pop(0)
        require('/' not in soname, 'dependency must be a SONAME: ' + soname)
        if soname in by_soname:
            continue
        candidates = []
        for directory in ('lib/x86_64-linux-gnu', 'lib', 'lib64'):
            path = platform / directory / soname
            if path.is_file():
                real = path.resolve(strict=True)
                require(real.is_relative_to(platform), 'provider leaves pinned target root')
                if real not in candidates:
                    candidates.append(real)
        require(len(candidates) == 1, 'missing or ambiguous target provider: ' + soname)
        path = candidates[0]
        meta, imports, exports = elf(path)
        require((meta['class'], meta['machine']) == identity, 'target ELF architecture mismatch')
        require(not meta['rpath'], 'target RPATH/RUNPATH is not supported: ' + soname)
        require(meta['soname'] == [soname], 'target provider SONAME mismatch: ' + soname)
        key = 'platform:' + str(path.relative_to(platform))
        require(key not in nodes, 'target provider claimed two SONAMEs')
        nodes[key] = {'file': str(path.relative_to(platform)), 'origin': 'platform',
                      'bytes': path.stat().st_size, 'sha256': sha(path.read_bytes()), **meta}
        parsed[key] = (imports, exports)
        by_soname[soname] = key
        pending.extend(meta['needed'])
    all_exports = set().union(*(value[1] for value in parsed.values()))
    checked = 0
    for key, node in nodes.items():
        for provider, required in node['version_needs'].items():
            require(provider in by_soname, 'version provider is absent: ' + provider)
            actual = nodes[by_soname[provider]]['version_definitions']
            require(set(required) <= set(actual), 'target version missing for ' + provider)
        for symbol, version, provider in parsed[key][0]:
            actual = parsed[by_soname[provider]][1] if provider else all_exports
            require((symbol, version) in actual or
                    (version is None and any(name == symbol for name, _ in actual)),
                    'target symbol missing: ' + symbol + ('@' + version if version else ''))
            checked += 1
    return {'schema': 1, 'scope': 'Declared ELF dependency closure and strong dynamic symbols only; '
            'not late dlopen, graphics, media, hardware or clean-OS acceptance.',
            'architecture': list(identity), 'sonames': sonames,
            'strong_symbol_requirements_checked': checked,
            'nodes': {key: nodes[key] for key in sorted(nodes)}}


def provenance(source, revision, payload):
    require(re.fullmatch(r'[0-9a-f]{40}', revision) is not None, 'source revision must be a full SHA')
    require(command(['git', '-C', str(source), 'rev-parse', 'HEAD']).strip() == revision,
            'source checkout revision mismatch')
    command(['git', '-C', str(source), 'diff', '--quiet', 'HEAD', '--'])
    source_url = 'https://github.com/jc-kynesim/rpi-ffmpeg/blob/' + revision + '/'
    contents, notices = {}, []
    for name in ('LICENSE.md', 'COPYING.LGPLv2.1'):
        raw = (source / name).read_bytes()
        contents['licenses/' + name] = raw
        notices.append({'source': name, 'url': source_url + name, 'sha256': sha(raw)})
    for name in COMPONENT_NOTICES:
        raw = (source / name).read_bytes()
        require(raw.startswith(b'/*') and b'*/' in raw, 'component source notice missing: ' + name)
        notice = raw[:raw.index(b'*/') + 2] + b'\n'
        contents['licenses/' + Path(name).name + '.notice'] = notice
        notices.append({'source': name, 'url': source_url + name,
                        'source_sha256': sha(raw), 'notice_sha256': sha(notice)})
    prefixes, configurations, options = set(), set(), set()
    for path in payload.values():
        if not path.is_symlink():
            raw = path.read_bytes()
            prefixes.update(re.findall(rb'--prefix=([^\x00\s]+)', raw))
            for configuration in re.findall(rb'--prefix=[^\x00]+', raw):
                configurations.add(sha(configuration))
                options.update(value for value in shlex.split(configuration.decode())
                               if value.startswith(('--enable-', '--disable-', '--cpu=')))
    record = {'schema': 1, 'source_revision': revision, 'source_tracked_changes': False,
              'source_url': source_url.removesuffix('/blob/' + revision + '/') + '/tree/' + revision,
              'notices': notices,
              'embedded_configure_prefix_sha256': sorted(sha(value) for value in prefixes),
              'private_build_prefixes_detected': any(value.startswith(
                  (b'/home/', b'/root/', b'/tmp/', b'/var/tmp/')) for value in prefixes),
              'configuration_sha256': sorted(configurations),
              'configured_feature_and_CPU_options': sorted(options),
              'neutral_build_verified': False, 'corresponding_source_archive_included': False,
              'redistribution_review': 'NOT_COMPLETED', 'update_maintenance_owner': 'UNASSIGNED',
              'clean_OS_and_Kodi_playback_qualification': 'NOT_RUN',
              'disposition': 'PUBLIC_NOT_READY'}
    contents['licenses/NOTICE'] = (
        'PRIVATE RESEARCH PACKAGE — PUBLIC_NOT_READY\n\n'
        'FFmpeg source revision: ' + revision + '\n'
        'The source notices below are copied from that pinned tree. This software\n'
        'is based in part on the work of the Independent JPEG Group.\n'
        'The selected binaries are copied, not rebuilt or modified by this tool.\n'
        'Embedded configure paths may remain private; their hashes are inventoried.\n'
        'No corresponding-source archive, Kodi, Platform, GL or driver is bundled.\n'
        'External dependencies must match the separately pinned target inventory.\n'
        'This inventory is not a complete licence/redistribution review. Public\n'
        'distribution, neutral build reproducibility, clean-OS/playback acceptance\n'
        'and an update/maintenance owner remain outstanding.\n').encode()
    contents['metadata/provenance.json'] = encoded(record)
    return contents, record


def manifest(root):
    entries = []
    for path in sorted(root.rglob('*')):
        if path.is_dir() or path == root / 'metadata/files.json':
            continue
        name = str(path.relative_to(root))
        if path.is_symlink():
            target = os.readlink(path)
            require('/' not in target and target not in ('.', '..'), 'invalid package SONAME link')
            entries.append({'path': name, 'kind': 'symlink', 'target': target})
        else:
            require(path.is_file(), 'unexpected package file type')
            entries.append({'path': name, 'kind': 'file', 'bytes': path.stat().st_size,
                            'mode': stat.S_IMODE(path.stat().st_mode),
                            'sha256': sha(path.read_bytes())})
    return entries


def pack(args):
    require(not args.output.exists(), 'output must be a new directory')
    require(0 <= args.epoch <= 0o77777777777, 'epoch outside USTAR range')
    payload, _ = selected_payload(args.prefix)
    dependencies = inventory(args.prefix, args.platform_root)
    contents, source = provenance(args.source_checkout.resolve(strict=True),
                                  args.source_revision, payload)
    args.output.mkdir(parents=True)
    stage = args.output / 'stage'
    for name in ('bin', 'lib', 'licenses', 'metadata'):
        (stage / name).mkdir(parents=True, mode=0o755)
    for name, path in payload.items():
        destination = stage / name
        if path.is_symlink():
            destination.symlink_to(os.readlink(path))
        else:
            shutil.copyfile(path, destination)
            destination.chmod(0o755)
    contents['metadata/dependencies.json'] = encoded(dependencies)
    for name, raw in contents.items():
        (stage / name).write_bytes(raw)
        (stage / name).chmod(0o644)
    require(inventory(stage, args.platform_root) == dependencies,
            'copied payload differs from the inspected dependency inventory')
    (stage / 'metadata/files.json').write_bytes(encoded(manifest(stage)))
    archive_path = args.output / 'runtime-private.tar'
    with tarfile.open(archive_path, 'w', format=tarfile.USTAR_FORMAT) as archive:
        for path in sorted(stage.rglob('*')):
            info = tarfile.TarInfo(str(path.relative_to(stage)))
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            info.mtime = args.epoch
            if path.is_symlink():
                info.type = tarfile.SYMTYPE
                info.linkname = os.readlink(path)
                info.mode = 0o777
                archive.addfile(info)
            elif path.is_dir():
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                archive.addfile(info)
            else:
                info.mode = 0o755 if str(path.relative_to(stage)).startswith(('bin/', 'lib/')) else 0o644
                info.size = path.stat().st_size
                with path.open('rb') as content:
                    archive.addfile(info, content)
    result = {'status': 'PASS_PRIVATE_PACKAGE', 'disposition': source['disposition'],
              'archive_bytes': archive_path.stat().st_size,
              'archive_sha256': sha(archive_path.read_bytes()),
              'entries': len(manifest(stage)), 'external_providers': sum(
                  node['origin'] == 'platform' for node in dependencies['nodes'].values())}
    (args.output / 'result.json').write_bytes(encoded(result))
    return result


def verify(args):
    root = args.package.resolve(strict=True)
    expected = json.loads((root / 'metadata/files.json').read_bytes())
    require(manifest(root) == expected, 'package file/hash/link manifest mismatch')
    actual = inventory(root, args.platform_root)
    recorded = json.loads((root / 'metadata/dependencies.json').read_bytes())
    require(actual == recorded, 'target dependency inventory changed')
    source = json.loads((root / 'metadata/provenance.json').read_bytes())
    require(source['disposition'] == 'PUBLIC_NOT_READY', 'research package disposition changed')
    return {'status': 'PASS_PRIVATE_PACKAGE_AUDIT', 'disposition': source['disposition'],
            'entries': len(expected), 'strong_symbol_requirements_checked':
                actual['strong_symbol_requirements_checked']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    build = sub.add_parser('pack')
    build.add_argument('--prefix', type=Path, required=True)
    build.add_argument('--platform-root', type=Path, required=True)
    build.add_argument('--source-checkout', type=Path, required=True)
    build.add_argument('--source-revision', default=SOURCE_REVISION)
    build.add_argument('--output', type=Path, required=True)
    build.add_argument('--epoch', type=int, required=True)
    audit = sub.add_parser('verify')
    audit.add_argument('--package', type=Path, required=True)
    audit.add_argument('--platform-root', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(pack(args) if args.action == 'pack' else verify(args), indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print('runtime-package: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
