#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test Kodi's PICTURE accounting and Flush/Reset with exact LGPL excerpts.

Requires Python's standard library and a C++17 compiler; never downloads
source or launches Kodi. --source accepts a full VAAPI.cpp whose tested excerpts
match the fixture, even if other code differs from the recorded upstream file.
--header optionally checks the buffer-statistics excerpt in a full VAAPI.h.
The tests compile those excerpts with protocol/storage scaffolds, not full Kodi.
A temporary diagnostic ordering variant distinguishes the observed race; no
external application patch is applied or produced.
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


def publication_probe(original):
    """Construct a counterfactual only inside the source-unit test scaffold."""
    before = '''            if (outPic)
            {
              m_config.stats->IncRender();
              m_dataPort.SendInMessage(COutputDataProtocol::PICTURE, &outPic, sizeof(outPic));
            }
            m_config.stats->DecProcessed();'''
    after = '''            if (outPic)
              m_config.stats->IncRender();
            m_config.stats->DecProcessed();
            if (outPic)
              m_dataPort.SendInMessage(COutputDataProtocol::PICTURE, &outPic, sizeof(outPic));'''
    require(original.count(before) == 1,
            'diagnostic publication-order probe no longer matches the pinned excerpt')
    return original.replace(before, after, 1)


def lifecycle_checks(temporary, compiler):
    source = HERE / 'kodi-vaapi-lifecycle.cpp'

    def build(name, definitions=()):
        binary = temporary / name
        run(compiler + ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                        *definitions, '-I', str(temporary), str(source), '-o', str(binary)],
            cwd=temporary)
        return binary

    candidate = run([str(build('kodi-vaapi-lifecycle'))], cwd=temporary)
    run([str(build('kodi-vaapi-lifecycle-original', ['-DKODI_ORIGINAL_OUTPUT']))],
        cwd=temporary)
    # These are deliberately broken temporary source excerpts, not purported
    # production failures. Each must fail the object/order oracle at runtime.
    mutations = [
        ('output-flush.inc', '      pic->Release();', '      (void)pic; // omitted release',
         'queued/present retirement or delayed external ownership changed'),
        ('output-flush.inc', '    msg->Release();', '    /* omitted message release */',
         'a data message was not retired exactly once'),
        ('output-configured.inc',
         '          Flush();\n          msg->Reply(COutputControlProtocol::ACC);',
         '          msg->Reply(COutputControlProtocol::ACC);\n          Flush();',
         'FLUSH acknowledged before queued cleanup/postproc flush'),
        ('decoder-reset.inc', '  Message *reply;',
         '  m_bufferStats.Reset();\n  Message *reply;',
         'statistics reset before FLUSH acknowledgement'),
        ('decoder-reset.inc', '    if (!success)\n    {',
         '    if (!success)\n    {\n      m_bufferStats.Reset();',
         'failed Reset erased statistics/error state or lost the present picture'),
        ('decoder-picture.inc', '        m_bufferStats.DecRender();',
         '        /* omitted consumer accounting */',
         'new epoch consumed a stale picture or failed actual consumer accounting'),
    ]
    for number, (name, before, after, reason) in enumerate(mutations, 1):
        path = temporary / name
        original = path.read_text()
        require(before in original, 'lifecycle mutation no longer matches: ' + name)
        try:
            path.write_text(original.replace(before, after, 1))
            negative = run([str(build('kodi-vaapi-lifecycle-negative-' + str(number)))],
                           cwd=temporary, expected=1)
            require(negative.stderr.strip() == 'FAIL: ' + reason,
                    'lifecycle mutation failed for the wrong reason:\n' + negative.stderr)
        finally:
            path.write_text(original)
    return candidate.stdout.strip(), len(mutations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, metavar='VAAPI.cpp',
                        help='check and extract a full source file without modifying it')
    parser.add_argument('--header', type=Path, metavar='VAAPI.h',
                        help='check the actual buffer-statistics class without modifying it')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'),
                        help='C++ compiler command (default: $CXX or c++)')
    args = parser.parse_args()
    compiler = shlex.split(args.cxx)
    require(bool(compiler), '--cxx must contain a compiler command')

    metadata = json.loads((FIXTURES / 'source.json').read_text())
    fixtures = {name: fixture(metadata, name) for name in metadata['fixtures']}
    original_output = fixtures['output-original.inc']
    original_drain = fixtures['drain-initial.inc']
    license_prefix = metadata['license_notice'] + '\n\n'
    full_source = args.source.read_bytes() if args.source else None
    full_header = args.header.read_bytes() if args.header else None
    if full_source is not None:
        source_text = full_source.decode('utf-8')
        for name, expected in fixtures.items():
            description = metadata['extract'][metadata['fixtures'][name]['extract']]
            if description.get('source', 'cpp') == 'cpp':
                require(extract(source_text, description) == expected,
                        '--source ' + name + ' excerpt differs from the pinned fixture')
    if full_header is not None:
        require(extract(full_header.decode('utf-8'), metadata['extract']['stats']) ==
                fixtures['buffer-stats.inc'], '--header buffer statistics differ from the pinned fixture')

    with tempfile.TemporaryDirectory(prefix='crystalhd-kodi-publish-order-') as work:
        temporary = Path(work)
        probe_output = publication_probe(original_output)
        (temporary / 'original-output-branch.inc').write_text(license_prefix + original_output)
        (temporary / 'probe-output-branch.inc').write_text(license_prefix + probe_output)
        (temporary / 'original-drain-initial.inc').write_text(license_prefix + original_drain)
        for name, body in fixtures.items():
            if name not in ('output-original.inc', 'drain-initial.inc'):
                (temporary / name).write_text(license_prefix + body)
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
        lifecycle, negative_count = lifecycle_checks(temporary, compiler)

    if args.source:
        require(args.source.read_bytes() == full_source, 'input source changed during the test')
        print('Input source SHA256: ' + sha(full_source))
        print('Pinned full-source SHA256: ' + metadata['source_sha256'])
        print('All tested C++ excerpts match; full-application compatibility is not tested.')
    else:
        print('Input: pinned LGPL source fixtures; no network or external source tree required.')
    if args.header:
        require(args.header.read_bytes() == full_header, 'input header changed during the test')
        print('Input header SHA256: ' + sha(full_header))
        print('Buffer-statistics class matches; pinned header SHA256: ' + metadata['header']['sha256'])
    print('Diagnostic publication-order probe: temporary source-unit variant only.')
    print('Baseline RED (exit 1): stale processed count after the last PICTURE take.')
    print('Diagnostic variant GREEN (exit 0): ' + regression.stdout.strip())
    print('Lifecycle original and diagnostic variant: ' + lifecycle)
    print('Lifecycle semantic negatives rejected: ' + str(negative_count))
    print('Scope: source-unit schedule/lifecycle tests. Full Kodi/Actor/VA/GPU/playback: NOT RUN.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
