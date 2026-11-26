#!/usr/bin/env python3
"""Build isolated current-source DIL and probe. Never execute a hardware mode."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: build.py ABSOLUTE_NEW_RESULT_DIRECTORY')
    out = Path(sys.argv[1])
    if not out.is_absolute():
        raise SystemExit('absolute result directory required')
    out.mkdir(mode=0o700, exist_ok=False)
    snapshots = out / 'as-run'
    snapshots.mkdir()
    for name in ['tools/research/linux-input-flags/probe.cpp',
                 'tools/research/linux-input-flags/build.py',
                 'tests/linux-input-flags.cpp']:
        shutil.copy2(ROOT/name, snapshots/Path(name).name)
    rows = []

    def run(label, command, env=None):
        with (out / (label + '.log')).open('wb') as log:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        rows.append(dict(label=label, argv=command, exit=result.returncode))
        (out / 'receipt.json').write_text(json.dumps(rows, indent=2)+'\n')
        if result.returncode:
            raise SystemExit(f'{label} failed; original log retained at {out}')

    run('library', ['make', '-C', str(ROOT/'linux_lib/libcrystalhd'), '-j2'])
    pkg = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs',
        'libavformat', 'libavcodec', 'libavutil', 'glib-2.0', 'gstreamer-1.0'], text=True))
    common = [os.environ.get('CXX', 'c++'), '-std=c++11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-D__LINUX_USER__', '-Iinclude', '-Iinclude/link', '-Ilinux_lib/libcrystalhd',
        '-Wl,--wrap=DtsProcInput,--wrap=DtsProcOutputNoCopy,--wrap=DtsReleaseOutputBuffs',
        '-Wl,--export-dynamic-symbol=ioctl', '-pthread']
    libs = ['tests/rx-output-receipts.cpp', '-Llinux_lib/libcrystalhd', '-lcrystalhd',
            '-Wl,-rpath,'+str(ROOT/'linux_lib/libcrystalhd')] + pkg
    run('probe', common + ['tools/research/linux-input-flags/probe.cpp'] + libs + ['-o', str(out/'probe')])
    run('controls-build', common + ['tests/linux-input-flags.cpp'] + libs + ['-o', str(out/'controls')])
    env = dict(os.environ, LD_LIBRARY_PATH=str(ROOT/'linux_lib/libcrystalhd'))
    env.pop('LD_PRELOAD', None)
    run('controls', [str(out/'controls')], env)
    run('controls-sanitized-build', common + ['-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        'tests/linux-input-flags.cpp'] + libs + ['-o', str(out/'controls-sanitized')])
    san_env = dict(env, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    run('controls-sanitized', [str(out/'controls-sanitized')], san_env)
    run('preflight', [str(out/'probe'), '--preflight', '/home/mache/crystalhd-phase1-fixtures/high-annexb.h264'], env)
    print(json.dumps(dict(status='PASS_BUILD_AND_DEVICE_FREE_CHECKS', directory=str(out), native='NOT_RUN')))


if __name__ == '__main__':
    main()
