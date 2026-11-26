#!/usr/bin/env python3
"""Build only this isolated current-source OWN study; never install it."""
import argparse
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[2]
BASE = '1427d2de0825706e20137f16e17dcb42366bac97'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', type=Path,
                        default=Path('/home/mache/.cache/crystalhd-v13-distribution-20261102/prefix-a/include'))
    args = parser.parse_args()
    run = Path(os.environ['CRYSTALHD_COUPLING_RUN_DIR'])
    if not run.is_absolute() or run.exists():
        raise ValueError('select one fresh absolute output directory; preserve prior artifacts')
    run = run.resolve()
    subprocess.run(['git','diff','--exit-code',BASE,'--','filters/vaapi','linux_lib/libcrystalhd',
                    'include','tests/vaapi-scaling.cpp','tests/vaapi-postfilter-reference.c'],
                   cwd=SOURCE, check=True, stdout=subprocess.DEVNULL)
    if not (args.sdk/'libavfilter/avfilter.h').is_file():
        raise ValueError('qualified original12.1.102/util61.1.102 public SDK headers required')
    run.mkdir(mode=0o700, parents=True)
    commands = [
        ['cc','-O2','-Wall','-Wextra','-Werror','-std=gnu11','-I'+str(args.sdk),
         '-DCRYSTALHD_COUPLED_REFERENCE_SOURCE="'+str(SOURCE/'tests/vaapi-postfilter-reference.c')+'"',
         '-c',str(HERE/'filter-bridge.c'),'-o',str(run/'filter-bridge.o')],
        ['g++','-D__LINUX_USER__','-I'+str(SOURCE/'linux_lib/libcrystalhd'),'-I'+str(SOURCE/'include'),
         '-O2','-std=c++17','-Wall','-Wextra','-Werror','-fPIC','-I/usr/include/libdrm',
         '-I/usr/include/x86_64-linux-gnu','-fno-tree-slp-vectorize','-ffunction-sections',
         '-fdata-sections','-DCRYSTALHD_BENCH_SCALING_SOURCE="'+str(SOURCE/'tests/vaapi-scaling.cpp')+'"',
         '-DCRYSTALHD_BENCH_DEFERRED=1',str(HERE/'coupled.cpp'),str(run/'filter-bridge.o'),
         '-o',str(run/'coupled'),'-Wl,--gc-sections','-lgbm','-ldrm','-ldl','-pthread','-msse2']]
    (run/'build-commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    for index,command in enumerate(commands):
        with (run/f'build-{index}.stdout').open('xb') as out,(run/f'build-{index}.stderr').open('xb') as err:
            subprocess.run(command,check=True,stdout=out,stderr=err)
    print(json.dumps(dict(status='BUILT_OWN_CPU_STUDY_ONLY',maintained_source=BASE,
                          executable=str(run/'coupled'),installed=False)))

if __name__ == '__main__':
    main()
