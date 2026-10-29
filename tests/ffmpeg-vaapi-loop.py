#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute retained FFmpeg queue functions without FFmpeg, VA or hardware.

Only Python's standard library and a C11 compiler are required. The default
fixtures are offline; --source-dir optionally verifies full retained files.
Semantic negatives must fail their stated independent oracles.
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
FIXTURES = HERE / 'fixtures' / 'issue445' / 'ffmpeg-loop'


def require(okay, message):
    if not okay:
        raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def extract(text, record):
    start, end = record['start'], record['end']
    require(text.count(start) == 1, 'source extraction start is not unique')
    first = text.index(start)
    require(text.count(end, first + len(start)) == 1,
            'source extraction end is missing or not unique')
    return text[first:text.index(end, first + len(start))]


def fixtures(metadata, source_dir):
    result = {}
    sources = {}
    if source_dir:
        for name, record in metadata['sources'].items():
            raw = (source_dir / name).read_bytes()
            require(sha(raw) == record['sha256'], name + ': full-source SHA256 mismatch')
            sources[name] = raw.decode('utf-8')
    for name, record in metadata['fixtures'].items():
        raw = (FIXTURES / name).read_bytes()
        require(sha(raw) == record['sha256'], name + ': fixture SHA256 mismatch')
        text = raw.decode('utf-8')
        notice = metadata['license_notices'][record['source']]
        require(text.startswith(notice), name + ': source notice missing')
        body = text[len(notice):]
        require(sha(body.encode()) == record['body_sha256'],
                name + ': source body SHA256 mismatch')
        if source_dir:
            require(extract(sources[record['source']], record) == body,
                    name + ': full-source excerpt mismatch')
        result[name] = text
    return result


def run(command, cwd, expected=0):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                            timeout=60, check=False)
    require(result.returncode == expected,
            'command failed: ' + shlex.join(command) + '\n' + result.stdout + result.stderr)
    return result


NEGATIVES = [
    ('missing-eof-drain', 'deint-request-frame.inc',
     '        deint_vaapi_filter_frame(link->src->inputs[0], NULL);',
     '        /* negative: omit the actual EOF tail request */',
     'EOF versus teardown did not preserve the expected field count'),
    ('missing-timestamp-lookahead', 'deint-build.inc',
     'ctx->extra_delay_for_timestamps = ctx->field_rate == 2 &&',
     'ctx->extra_delay_for_timestamps = ctx->field_rate == 1 &&',
     'zero-reference field timestamp lookahead changed'),
    ('wrong-next-pts', 'deint-filter-frame.inc',
     'output_frame->pts = input_frame->pts +\n'
     '                    ctx->frame_queue[current_frame_index + 1]->pts;',
     'output_frame->pts = 2 * input_frame->pts + 1;',
     'independent irregular-PTS oracle disagrees'),
    ('unreleased-queued-reference', 'deint-uninit.inc',
     '        av_frame_free(&ctx->frame_queue[i]);',
     '        (void)ctx->frame_queue[i];',
     'a queued frame reference was not released'),
    ('wrong-hardware-context-identity', 'filter-hwcontext-reinit.inc',
     'ifp->hw_frames_ctx->data != frame->hw_frames_ctx->data',
     'ifp->hw_frames_ctx != frame->hw_frames_ctx',
     'actual hardware-context identity predicate changed'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path,
                        help='verify retained full source files without modifying them')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'),
                        help='C compiler command (default: $CC or cc)')
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    require(bool(compiler), '--cc must contain a compiler command')
    metadata = json.loads((FIXTURES / 'source.json').read_text())
    original = fixtures(metadata, args.source_dir)
    with tempfile.TemporaryDirectory(prefix='crystalhd-ffmpeg-loop-') as work:
        temporary = Path(work)

        def compile_case(name, files):
            directory = temporary / name
            directory.mkdir()
            for filename, text in files.items():
                (directory / filename).write_text(text)
            binary = directory / 'queue-test'
            run(compiler + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(directory), str(HERE / 'ffmpeg-vaapi-loop.c'),
                            '-o', str(binary)], directory)
            return directory, binary

        directory, binary = compile_case('retained', original)
        positive = run([str(binary)], directory)
        for name, filename, old, new, reason in NEGATIVES:
            require(original[filename].count(old) == 1,
                    name + ': semantic-negative mutation is not unique')
            changed = dict(original)
            changed[filename] = original[filename].replace(old, new)
            directory, binary = compile_case(name, changed)
            result = run([str(binary)], directory, expected=1)
            require(result.stderr.strip() == 'FAIL: ' + reason,
                    name + ': failed for a different reason:\n' + result.stdout + result.stderr)
    if args.source_dir:
        require(fixtures(metadata, args.source_dir) == original,
                'retained full sources changed during the test')
    print(positive.stdout.strip())
    print('Semantic-negative discrimination: PASS (' + str(len(NEGATIVES)) + ' cases).')
    print('Native retained playback remains FAIL 296/300; this is no production repair.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
