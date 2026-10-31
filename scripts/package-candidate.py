#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Create private exact-commit source/add-on artifacts, without publishing."""
import argparse
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET


def package(source, destination, revision='HEAD'):
    source, destination = Path(source).resolve(), Path(destination).absolute()

    def git(*arguments):
        return subprocess.check_output(['git', '-C', str(source), *arguments])

    commit = git('rev-parse', '--verify', '--end-of-options', revision + '^{commit}').decode().strip()
    if git('rev-parse', 'HEAD').decode().strip() != commit:
        raise ValueError('candidate must be the inspected checkout HEAD')
    if git('status', '--porcelain', '--untracked-files=no').strip():
        raise ValueError('commit tracked changes before packaging a candidate')
    version = git('show', commit + ':VERSION').decode().strip()
    if re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?', version) is None:
        raise ValueError('unsupported bundle version')
    tree = git('rev-parse', commit + '^{tree}').decode().strip()
    epoch = int(git('show', '-s', '--format=%ct', commit).decode().strip())
    destination.mkdir(parents=True, exist_ok=False)
    stem = 'crystalhd-' + version + '-' + commit[:12]
    archive_path = destination / (stem + '.tar.gz')
    with archive_path.open('xb') as output:
        process = subprocess.Popen(['git', '-C', str(source), 'archive', '--format=tar',
                                    '--prefix=' + stem + '/', commit], stdout=subprocess.PIPE)
        try:
            with gzip.GzipFile(filename='', mode='wb', fileobj=output, mtime=0, compresslevel=9) as compressor:
                shutil.copyfileobj(process.stdout, compressor)
        finally:
            process.stdout.close()
        if process.wait() != 0:
            raise RuntimeError('source archive failed; incomplete directory retained for inspection')
    spec = importlib.util.spec_from_file_location('crystalhd_kodi_package', source / 'scripts/package-kodi-settings.py')
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    addon_version = ET.fromstring(git('show', commit + ':addons/script.crystalhd.settings/addon.xml')).get('version')
    if re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', addon_version or '') is None:
        raise ValueError('unsupported add-on version')
    addon_path = destination / ('script.crystalhd.settings-' + addon_version + '.zip')
    addon = builder.build(source, addon_path, read_file=lambda name: git('show', commit + ':' + name))
    artifacts = []
    for path in (archive_path, addon_path):
        artifacts.append({'name': path.name, 'bytes': path.stat().st_size,
                          'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    manifest = {'format': 'crystalhd-private-candidate', 'format_version': 1,
                'bundle_version': version, 'source_commit': commit, 'source_tree': tree,
                'source_date_epoch': epoch, 'publication': 'PRIVATE_CANDIDATE_NOT_RELEASED',
                'qualification': 'NOT_RUN_FOR_THESE_ARTIFACTS',
                'scope': ['exact committed source', 'optional Kodi configuration front end'],
                'native_ffmpeg_research_runtime': 'SEPARATE_PUBLIC_NOT_READY',
                'addon': addon, 'artifacts': artifacts}
    manifest_path = destination / 'candidate-manifest.json'
    manifest_path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
    checks = list(artifacts) + [{'name': manifest_path.name,
                                'sha256': hashlib.sha256(manifest_path.read_bytes()).hexdigest()}]
    (destination / 'SHA256SUMS').write_text(''.join(item['sha256'] + '  ' + item['name'] + '\n'
                                                  for item in sorted(checks, key=lambda item: item['name'])))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--revision', default='HEAD')
    args = parser.parse_args()
    print(json.dumps(package(args.source, args.output_dir, args.revision), sort_keys=True))


if __name__ == '__main__':
    main()
