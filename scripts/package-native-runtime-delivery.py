#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Package pinned runtime/source/evidence companions without changing their bytes.

Trusted, quiescent inputs only. This joins recorded facts into a reviewable
delivery. It does not install, execute ELF files, publish, or approve support
and redistribution policy. Historical inner metadata remains intact.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile

SHA = re.compile(r'[0-9a-f]{64}\Z')
ROLES = {'runtime', 'ffmpeg-source', 'dependency-source', 'build-evidence',
         'host-evidence', 'tool', 'guide', 'support', 'notice', 'source-metadata', 'cost',
         'ffmpeg-source-manifest'}
REQUIRED = {'runtime', 'ffmpeg-source', 'dependency-source', 'build-evidence',
            'host-evidence', 'tool', 'guide', 'support', 'notice', 'ffmpeg-source-manifest'}
LIMIT = 512 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            value.update(block)
    return value.hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def name(value):
    require(isinstance(value, str) and value and '\\' not in value and
            not any(ord(c) < 32 or ord(c) == 127 for c in value), 'invalid delivery member')
    parsed = PurePosixPath(value)
    require(bool(parsed.parts) and not parsed.is_absolute() and str(parsed) == value and
            all(part not in ('.', '..', '.git') for part in parsed.parts),
            'delivery member must be canonical and relative')
    require(parsed.parts[0] not in ('delivery-manifest.json', 'SHA256SUMS'), 'reserved delivery member')
    tarfile.TarInfo(value).tobuf(format=tarfile.USTAR_FORMAT, encoding='utf-8')
    return value


def inspect_inputs(spec, expected):
    spec = Path(spec)
    require(spec.is_file() and spec.stat().st_size <= 1024 * 1024 and
            SHA.fullmatch(expected) and digest(spec) == expected, 'spec SHA-256 mismatch')
    data = json.loads(spec.read_text())
    require(set(data) == {'schema', 'bundle_version', 'epoch', 'metadata_pins', 'files'} and
            data['schema'] == 1, 'unsupported delivery spec')
    require(re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?', data['bundle_version']),
            'invalid bundle version')
    require(type(data['epoch']) is int and 0 <= data['epoch'] <= 0o77777777777, 'invalid epoch')
    require(isinstance(data['files'], list) and 0 < len(data['files']) <= 128, 'invalid input count')
    roles, names, rows, total = set(), set(), [], 0
    for item in data['files']:
        require(set(item) == {'source', 'member', 'role', 'sha256', 'bytes'}, 'invalid input fields')
        member = name(item['member'])
        require(member not in names and item['role'] in ROLES, 'duplicate member or unknown role')
        path = Path(item['source'])
        require(path.is_absolute() and path.is_file() and not path.is_symlink(), 'input must be a regular absolute file')
        require(type(item['bytes']) is int and 0 <= item['bytes'] <= LIMIT and
                path.stat().st_size == item['bytes'] and SHA.fullmatch(item['sha256']) and
                digest(path) == item['sha256'], 'input size/SHA-256 mismatch: ' + member)
        rows.append(dict(item, source=path.resolve()))
        roles.add(item['role']); names.add(member); total += item['bytes']
    require(total <= 1024 * 1024 * 1024 and REQUIRED <= roles, 'missing required companion or oversized delivery')
    require(not any(str(parent) in names for member in names
                    for parent in PurePosixPath(member).parents if str(parent) != '.'),
            'member file conflicts with a parent directory')
    for role in ('runtime', 'build-evidence', 'ffmpeg-source-manifest'):
        require(sum(row['role'] == role for row in rows) == 1, 'expected one ' + role)
    require(set(data['metadata_pins']) == {'files', 'dependencies', 'provenance'} and
            all(SHA.fullmatch(v) for v in data['metadata_pins'].values()), 'invalid metadata pins')
    return data, rows


def source_binding(data, rows):
    runtime = next(row for row in rows if row['role'] == 'runtime')
    proof = json.loads(next(row['source'] for row in rows if row['role'] == 'build-evidence').read_text())
    require(proof.get('status') == 'PASS_REPRODUCIBLE_BUILDS_AND_RETAINED_BINARY_SOURCE_CORRESPONDENCE' and
            proof.get('two_builds_exact_payload_equal') is True and
            proof.get('retained_neutral_exact_payload_equal') is True, 'build evidence is incomplete')
    with tarfile.open(runtime['source'], 'r:') as archive:
        members = archive.getmembers()
        require(len({m.name for m in members}) == len(members), 'duplicate inner members')
        for key, expected in data['metadata_pins'].items():
            raw = archive.extractfile('metadata/' + key + '.json').read(LIMIT + 1)
            require(len(raw) <= LIMIT and hashlib.sha256(raw).hexdigest() == expected,
                    'inner metadata SHA-256 mismatch: ' + key)
        provenance = json.loads(archive.extractfile('metadata/provenance.json').read())
        require(provenance['source_revision'] == proof['source_revision'], 'source revisions differ')
        require(provenance['disposition'] == 'PUBLIC_NOT_READY', 'historical inner metadata changed')
        payload = {}
        for member in members:
            if member.isfile() and member.name.startswith(('bin/', 'lib/')):
                raw = archive.extractfile(member).read(LIMIT + 1)
                require(len(raw) <= LIMIT, 'oversized inner payload')
                payload[member.name] = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
    require(len(payload) == 9 and payload == proof['retained_payload'], 'inner ELF payload differs from reproduced bytes')
    require(len(proof['builds']) == 2 and all(row['status'] == 'PASS' and row['payload'] == payload
                                          for row in proof['builds']), 'independent build payload differs')
    require(len({row['name'] for row in proof['builds']}) == 2, 'build identities must differ')
    materials = json.loads(next(row['source'] for row in rows
                               if row['role'] == 'ffmpeg-source-manifest').read_text())
    require(materials['format'] == 'crystalhd-private-native-runtime-materials' and
            materials['source_revision'] == proof['source_revision'] and
            materials['source_tree'] == proof['source_tree'] and
            materials['binary_package_archive']['sha256'] == runtime['sha256'] and
            materials['binary_package_archive']['bytes'] == runtime['bytes'] and
            materials['package_metadata_sha256'] == data['metadata_pins'],
            'source companion does not match the runtime and build proof')
    require(any(row['role'] == 'ffmpeg-source' and
                row['sha256'] == materials['archive']['sha256'] and
                row['bytes'] == materials['archive']['bytes'] for row in rows),
            'matching complete FFmpeg source archive is missing')
    return {'source_revision': proof['source_revision'], 'source_tree': proof['source_tree'],
            'source_date_epoch': proof['source_date_epoch'], 'payload': payload,
            'historical_inner_disposition': provenance['disposition']}


def package(spec, expected, destination):
    destination = Path(destination).absolute()
    require(not destination.exists() and not destination.is_symlink(), 'output must be new')
    data, rows = inspect_inputs(spec, expected)
    binding = source_binding(data, rows)
    files = sorted(({key: row[key] for key in ('member', 'role', 'bytes', 'sha256')}
                    for row in rows), key=lambda row: row['member'])
    logical = {'bundle_version': data['bundle_version'], 'epoch': data['epoch'],
               'metadata_pins': data['metadata_pins'], 'files': files}
    manifest = {'format': 'crystalhd-native-runtime-delivery', 'format_version': 1,
                'bundle_version': data['bundle_version'],
                'logical_input_sha256': hashlib.sha256(encoded(logical)).hexdigest(),
                'scope': 'Pinned materials and reproduced payload; policy and publication are separate decisions.',
                'source_binding': binding, 'inner_metadata_pins': data['metadata_pins'],
                'files': files}
    destination.mkdir(parents=True)
    stage = destination / 'delivery'
    stage.mkdir()
    for row in rows:
        target = stage / row['member']; target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(row['source'], target); target.chmod(0o644)
        require(digest(target) == row['sha256'], 'input changed during copying')
    (stage / 'delivery-manifest.json').write_bytes(encoded(manifest))
    (stage / 'SHA256SUMS').write_text(''.join(
        digest(path) + '  ' + str(path.relative_to(stage)) + '\n'
        for path in sorted(stage.rglob('*')) if path.is_file()))
    archive_path = destination / ('crystalhd-native-runtime-' + data['bundle_version'] + '.tar.gz')
    with archive_path.open('xb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw,
                                                    mtime=0, compresslevel=6) as compressed:
        with tarfile.open(fileobj=compressed, mode='w|', format=tarfile.USTAR_FORMAT) as archive:
            for path in sorted(stage.rglob('*')):
                if not path.is_file():
                    continue
                info = tarfile.TarInfo(str(path.relative_to(stage)))
                info.uid = info.gid = 0; info.uname = info.gname = ''
                info.mtime = data['epoch']; info.mode = 0o644; info.size = path.stat().st_size
                with path.open('rb') as content:
                    archive.addfile(info, content)
    result = {'status': 'PACKAGED_DELIVERY_REVIEW_CANDIDATE', 'archive': archive_path.name,
              'spec_sha256': expected,
              'bytes': archive_path.stat().st_size, 'sha256': digest(archive_path),
              'manifest_sha256': digest(stage / 'delivery-manifest.json')}
    (destination / 'delivery-result.json').write_bytes(encoded(result))
    (destination / 'SHA256SUMS').write_text(result['sha256'] + '  ' + archive_path.name + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spec', type=Path, required=True)
    parser.add_argument('--expected-spec-sha256', required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(package(args.spec, args.expected_spec_sha256, args.output_dir), indent=2))


if __name__ == '__main__':
    main()
