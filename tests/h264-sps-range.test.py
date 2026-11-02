#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixed, independently traced NALs; ordinary and sanitizer CPU controls."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
fixture = json.loads((root / 'tests/fixtures/issue538/h264-sps-range-goldens-20261102.json').read_text())
with tempfile.TemporaryDirectory(prefix='crystalhd-sps-range-') as scratch:
    work = Path(scratch)
    declarations = ['struct range_golden { const unsigned char *nal; unsigned int bytes, minimum; '
                    'struct chd_h264_sps_range expected; };']
    entries = []
    for index, case in enumerate(fixture['cases']):
        nal = bytes.fromhex(case['nal_hex'])
        declarations.append('static const unsigned char golden_%d[] = {%s};' %
                            (index, ','.join(str(x) for x in nal)))
        expected = case['expected']
        values = [expected[key] for key in ('sps_id', 'vui_present', 'video_signal_present',
                                           'range_present', 'full_range')]
        entries.append('{golden_%d,%d,%d,{%s}}' %
                       (index, len(nal), case['minimum_bytes_for_prefix'], ','.join(str(int(x)) for x in values)))
    declarations.append('static const struct range_golden goldens[] = {%s};' % ','.join(entries))
    (work / 'range-goldens.h').write_text('\n'.join(declarations) + '\n')
    base = (shlex.split(os.environ.get('CC', 'cc')) +
            shlex.split(os.environ.get('CFLAGS', '')) +
            ['-std=c99', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'),
             '-I' + str(work), str(root / 'tests/h264-sps-range.c')])
    for name, flags in [('ordinary', ['-O2']),
                        ('sanitized', ['-O1', '-g', '-fno-omit-frame-pointer',
                                       '-fsanitize=address,undefined'])]:
        target = work / name
        subprocess.run(base + flags + ['-o', str(target)], check=True)
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        subprocess.run([str(target)], env=environment, check=True)
