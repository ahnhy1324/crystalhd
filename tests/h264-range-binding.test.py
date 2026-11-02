#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='crystalhd-range-binding-') as scratch:
    target = Path(scratch) / 'check'
    for test in ('h264-range-binding.c', 'h264-multislice.c'):
        base = (shlex.split(os.environ.get('CC', 'cc')) + shlex.split(os.environ.get('CFLAGS', ''))
                + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'include'),
                   str(root / 'tests' / test)])
        for flags in (['-O2'], ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']):
            subprocess.run(base + flags + ['-o', str(target)], check=True)
            subprocess.run([str(target)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                                    UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
