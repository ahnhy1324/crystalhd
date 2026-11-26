#!/usr/bin/env python3
"""Build the current DIL and isolated PM adapter; run device-free checks only."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess as S
import sys

ROOT = Path(__file__).resolve().parents[3]


def main():
    if len(sys.argv) != 2 or not Path(sys.argv[1]).is_absolute():
        raise SystemExit('usage: build.py ABSOLUTE_NEW_RESULT_DIRECTORY')
    out = Path(sys.argv[1]); out.mkdir(mode=0o700, exist_ok=False)
    frozen = out/'as-run'; frozen.mkdir()
    for name in ('tools/research/flea-pm-finite/probe.cpp',
                 'tools/research/flea-pm-finite/build.py', 'tests/flea-pm-probe.cpp'):
        shutil.copy2(ROOT/name, frozen/Path(name).name)
    rows = []

    def run(label, command, env=None):
        with (out/(label+'.log')).open('xb') as log:
            result = S.run(command, cwd=ROOT, env=env, stdout=log, stderr=S.STDOUT)
        rows.append(dict(label=label, argv=command, exit=result.returncode))
        (out/'receipt.json').write_text(json.dumps(rows, indent=2)+'\n')
        if result.returncode:
            raise SystemExit(f'{label} failed; original failure retained at {out}')

    run('library', ['make', '-C', str(ROOT/'linux_lib/libcrystalhd'), '-j2'])
    pkg = shlex.split(S.check_output(['pkg-config','--cflags','--libs',
        'libavformat','libavcodec','libavutil','glib-2.0','gstreamer-1.0'], text=True))
    common = [os.environ.get('CXX','c++'), '-std=c++11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-D__LINUX_USER__', '-Iinclude', '-Ilinux_lib/libcrystalhd',
        '-Wl,--wrap=DtsProcInput,--wrap=DtsSetColorSpace,--wrap=DtsProcOutputNoCopy,--wrap=DtsReleaseOutputBuffs',
        '-pthread']
    libs = ['tests/rx-output-receipts.cpp', '-Llinux_lib/libcrystalhd', '-lcrystalhd',
            '-Wl,-rpath,'+str(ROOT/'linux_lib/libcrystalhd')] + pkg
    run('probe', common+['tools/research/flea-pm-finite/probe.cpp']+libs+['-o',str(out/'probe')])
    run('controls-build', common+['tests/flea-pm-probe.cpp']+libs+['-o',str(out/'controls')])
    env = {k:v for k,v in os.environ.items() if not k.startswith(('LD_','CRYSTALHD_'))}
    env.update(LD_LIBRARY_PATH=str(ROOT/'linux_lib/libcrystalhd'), CRYSTALHD_CONFIG_DISABLE='1')
    run('controls',[str(out/'controls')],env)
    run('controls-sanitized-build', common+['-fsanitize=address,undefined','-fno-omit-frame-pointer',
        'tests/flea-pm-probe.cpp']+libs+['-o',str(out/'controls-sanitized')])
    run('controls-sanitized',[str(out/'controls-sanitized')],dict(env,
        ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
    run('preflight',[str(out/'probe'),'--preflight',
        '/home/mache/crystalhd-phase1-fixtures/mpeg2.m2v'],env)
    print(json.dumps(dict(status='PASS_BUILD_DEVICE_FREE',directory=str(out),native='NOT_RUN')))


if __name__ == '__main__':
    main()
