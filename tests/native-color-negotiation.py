#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run pinned real-node/FFmpeg metadata functions without opening any device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / 'fixtures' / 'issue538' / 'color-negotiation'


def require(okay, message):
    if not okay:
        raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(argv, expected=0):
    result = subprocess.run(argv, text=True, capture_output=True, timeout=60)
    require(result.returncode == expected, 'command failed: ' + shlex.join(argv) + '\n' + result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--node-source', type=Path)
    parser.add_argument('--ffmpeg-codec-source', type=Path, help='directory containing v4l2_context.c/v4l2_m2m_dec.c')
    parser.add_argument('--kernel-common-header', type=Path)
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    require(bool(compiler), 'empty compiler command')
    metadata = json.loads((FIXTURES / 'source.json').read_text())
    sources = {}
    if args.node_source: sources['node'] = args.node_source
    if args.ffmpeg_codec_source:
        sources.update(ffmpeg_context=args.ffmpeg_codec_source / 'v4l2_context.c',
                       ffmpeg_decoder=args.ffmpeg_codec_source / 'v4l2_m2m_dec.c')
    if args.kernel_common_header: sources['kernel'] = args.kernel_common_header
    if args.kernel_common_header:
        uapi = args.kernel_common_header.parent.parent / 'uapi/linux/videodev2.h'
        require(sha(uapi.read_bytes()) == metadata['kernel_only_validation_sentinels']['sha256'],
                'kernel UAPI validation-enum source hash mismatch')
    originals = {name: path.read_bytes() for name, path in sources.items()}
    for name, raw in originals.items():
        require(sha(raw) == metadata['sources'][name]['sha256'], name + ': whole-source hash mismatch')
    bodies = {}
    for name, record in metadata['fixtures'].items():
        raw = (FIXTURES / name).read_bytes()
        require(sha(raw) == record['sha256'], name + ': fixture hash mismatch')
        prefix = record['notice'] + '\n'
        require(raw.decode().startswith(prefix), name + ': license/copyright notice missing')
        body = raw.decode()[len(prefix):]
        require(sha(body.encode()) == record['body_sha256'], name + ': source-body hash mismatch')
        if record['source'] in originals:
            text = originals[record['source']].decode()
            require(text.count(record['start']) == 1, name + ': ambiguous source start')
            start = text.index(record['start']); end = text.index(record['end'], start + len(record['start']))
            require(text[start:end] == body, name + ': source extraction differs')
        bodies[name] = raw
    with tempfile.TemporaryDirectory(prefix='crystalhd-native-color-') as work:
        root = Path(work)
        for name, raw in bodies.items(): (root / name).write_bytes(raw)
        def build(label):
            binary = root / label
            run(compiler + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                            str(HERE / 'native-color-negotiation.c'), '-o', str(binary)])
            return binary
        result = run([str(build('metadata'))])
        mutations = [
            ('node-colors.inc', '\tif (f->color_default)\n', '\tif (true)\n', 'explicit caller colour was overridden'),
            ('node-formats.inc', '\trc = chd_try_fmt_locked(file, fh, fmt);\n',
             '\trc = chd_try_fmt_locked(file, fh, fmt);\n\tf->color_default = false;\n', 'format query changed driver state'),
            ('node-colors.inc', '\tdst->quantization = src->quantization;', '\tdst->quantization = 0;',
             'explicit extended colour tuple was overridden'),
        ]
        for number, (name, before, after, reason) in enumerate(mutations, 1):
            path = root / name; original = path.read_text()
            require(before in original, name + ': mutation no longer matches')
            try:
                path.write_text(original.replace(before, after, 1))
                negative = run([str(build('negative-' + str(number)))], expected=1)
                require(negative.stderr.strip() == 'FAIL: ' + reason, 'mutation failed for wrong reason: ' + negative.stderr)
            finally:
                path.write_text(original)
    for name, path in sources.items(): require(path.read_bytes() == originals[name], 'input source modified')
    print(result.stdout.strip())
    print('Three source-semantic negatives rejected; eight fixture/license/hash guards PASS.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        raise SystemExit('FAIL: ' + str(error))
