#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test Kodi's PICTURE accounting with exact LGPL source excerpts.

Requires Python's standard library, git, and a C++17 compiler; never downloads
source or launches Kodi. --source accepts a full VAAPI.cpp whose tested excerpts
match the fixture, even if other code differs from the recorded upstream file.
The test compiles those excerpts with a queue/counter scaffold, not full Kodi.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
FIXTURES = HERE / 'fixtures' / 'issue445'
PATCH = HERE.parent / 'patches' / 'kodi' / '0001-vaapi-account-before-publish.patch'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def fixture(metadata, name):
    record = metadata['fixtures'][name]
    raw = (FIXTURES / name).read_bytes()
    require(sha(raw) == record['sha256'], name + ': fixture SHA256 mismatch')
    notice = metadata['license_notice'] + '\n\n'
    text = raw.decode('utf-8')
    require(text.startswith(notice), name + ': source license notice missing')
    body = text[len(notice):]
    require(sha(body.encode()) == metadata['extract'][record['extract']]['sha256'],
            name + ': extracted source SHA256 mismatch')
    return body


def extract(text, description):
    start = description['start']
    end = description['end']
    require(text.count(start) == 1, 'source extraction start is not unique')
    first = text.index(start)
    last = text.find(end, first + len(start))
    require(last >= 0, 'source extraction end is missing')
    return text[first:last]


def run(command, *, cwd, expected=0):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            timeout=60, check=False)
    require(result.returncode == expected,
            'command failed: ' + shlex.join(command) + '\n' +
            result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, metavar='VAAPI.cpp',
                        help='check and extract a full source file without modifying it')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'),
                        help='C++ compiler command (default: $CXX or c++)')
    args = parser.parse_args()
    compiler = shlex.split(args.cxx)
    require(bool(compiler), '--cxx must contain a compiler command')

    metadata = json.loads((FIXTURES / 'source.json').read_text())
    original_output = fixture(metadata, 'output-original.inc')
    original_drain = fixture(metadata, 'drain-initial.inc')
    license_prefix = metadata['license_notice'] + '\n\n'
    full_source = args.source.read_bytes() if args.source else None
    if full_source is not None:
        source_text = full_source.decode('utf-8')
        for name, expected in [('output', original_output), ('drain', original_drain)]:
            require(extract(source_text, metadata['extract'][name]) == expected,
                    '--source ' + name + ' excerpt differs from the pinned fixture')
        original = full_source
    else:
        # git apply accepts the real hunk's line-number offset on this short file.
        original = (license_prefix + original_output).encode()

    with tempfile.TemporaryDirectory(prefix='crystalhd-kodi-publish-order-') as work:
        temporary = Path(work)
        tree = temporary / 'source'
        target = tree / metadata['source_path']
        target.parent.mkdir(parents=True)
        target.write_bytes(original)
        # Apply the repository patch itself; do not synthesize candidate code.
        run(['git', 'apply', '--check', str(PATCH)], cwd=tree)
        run(['git', 'apply', str(PATCH)], cwd=tree)
        run(['git', 'apply', '--check', '--reverse', str(PATCH)], cwd=tree)
        patched_text = target.read_text()
        if full_source is not None:
            patched_output = extract(patched_text, metadata['extract']['output'])
            require(extract(patched_text, metadata['extract']['drain']) == original_drain,
                    'patch unexpectedly changes the initial drain condition')
        else:
            require(patched_text.startswith(license_prefix),
                    'patch unexpectedly changes the source license notice')
            patched_output = patched_text[len(license_prefix):]
        require(patched_output != original_output, 'patch did not change the OUTPUT branch')
        (temporary / 'original-output-branch.inc').write_text(license_prefix + original_output)
        (temporary / 'patched-output-branch.inc').write_text(license_prefix + patched_output)
        (temporary / 'original-drain-initial.inc').write_text(license_prefix + original_drain)
        binary = temporary / 'kodi-vaapi-publish-order'
        run(compiler + ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-I', str(temporary), str(HERE / 'kodi-vaapi-publish-order.cpp'),
                        '-o', str(binary)], cwd=temporary)
        baseline = run([str(binary), '--require-original-correct'], cwd=temporary, expected=1)
        require(baseline.stderr.strip() ==
                'FAIL: original branch leaves a stale processed count after last take',
                'baseline failed for a reason other than the publication-order counterexample:\n' +
                baseline.stdout + baseline.stderr)
        regression = run([str(binary)], cwd=temporary)

    if args.source:
        require(args.source.read_bytes() == full_source, 'input source changed during the test')
        print('Input source SHA256: ' + sha(full_source))
        print('Pinned full-source SHA256: ' + metadata['source_sha256'])
        print('Both tested source excerpts match; full-application compatibility is not tested.')
    else:
        print('Input: pinned LGPL source fixtures; no network or external source tree required.')
    print('Patch forward/reverse checks: PASS')
    print('Baseline RED (exit 1): stale processed count after the last PICTURE take.')
    print('Candidate GREEN (exit 0): ' + regression.stdout.strip())
    print('Scope: source-unit schedule test. Full Kodi/Actor/VA/GPU/playback: NOT RUN.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
