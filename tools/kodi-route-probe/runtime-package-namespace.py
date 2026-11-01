#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run metadata or bounded software CLI checks in a device-free Platform.

This does not mount Kodi, GL, CrystalHD, firmware, host libraries or networking.
The kernel remains the existing host's kernel; this is not clean-OS acceptance.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--platform-root', type=Path, required=True)
    parser.add_argument('--mount-prefix', required=True)
    parser.add_argument('--software-media', type=Path)
    parser.add_argument('--software-oracle', type=Path)
    parser.add_argument('--software-clock-reference', type=Path)
    parser.add_argument('--software-nv12-reference', type=Path)
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    package = args.package.resolve(strict=True)
    platform = args.platform_root.resolve(strict=True)
    prefix = Path(args.mount_prefix)
    if (not prefix.is_absolute() or len(prefix.parts) != 3 or prefix.parts[1] != 'opt'
            or prefix.name == '..'):
        parser.error('--mount-prefix must be one directory directly below /opt')
    probe = Path(__file__).with_name('runtime-package-metadata.py')
    software = (args.software_media, args.software_oracle, args.software_clock_reference,
                args.software_nv12_reference, args.output_dir)
    if any(value is not None for value in software) and not all(value is not None for value in software):
        parser.error('software validation requires all four inputs and a new --output-dir')
    mounts = []
    program = ['/usr/bin/python3', '/proof/metadata.py', '--package', str(prefix)]
    expected_status = 'PASS_COMPILERLESS_RELOCATED_METADATA_ONLY'
    if args.software_media:
        output = args.output_dir.resolve()
        if output.exists() or output.is_relative_to(package) or output.is_relative_to(platform):
            parser.error('--output-dir must be new and outside the package/Platform')
        inputs = [(args.software_media, '/media/input.ts'),
                  (args.software_oracle, '/reference/expected.json'),
                  (args.software_clock_reference, '/reference/independent-ffprobe8.json'),
                  (args.software_nv12_reference, '/reference/software.nv12')]
        for source, destination in inputs:
            source = source.resolve(strict=True)
            if not source.is_file():
                parser.error('software validation input must be a regular file')
            mounts.extend(['--ro-bind', str(source), destination])
        output.mkdir(parents=True, mode=0o700)
        mounts.extend(['--bind', str(output), '/work', '--ro-bind',
                       str(Path(__file__).with_name('runtime-package-cli.py')), '/proof/cli.py'])
        program = ['/usr/bin/python3', '/proof/cli.py', '--package', str(prefix)]
        expected_status = 'PASS_COMPILERLESS_RELOCATED_SOFTWARE_CLI'
    command = ['bwrap', '--die-with-parent', '--unshare-all', '--clearenv',
               '--ro-bind', str(platform), '/usr',
               '--ro-bind', str(platform / 'etc'), '/etc',
               '--symlink', 'usr/bin', '/bin', '--symlink', 'usr/sbin', '/sbin',
               '--symlink', 'usr/lib', '/lib', '--symlink', 'usr/lib64', '/lib64',
               '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
               '--dir', '/work', '--ro-bind', str(package), str(prefix),
               '--ro-bind', str(probe), '/proof/metadata.py',
               *mounts,
               '--chdir', '/work', '--setenv', 'PATH', '/usr/bin',
               '--setenv', 'PYTHONDONTWRITEBYTECODE', '1',
               '--setenv', 'LC_ALL', 'C.UTF-8', '--setenv', 'LD_LIBRARY_PATH',
               str(prefix / 'lib') + ':/usr/lib/x86_64-linux-gnu:/usr/lib',
               '--', *program]
    result = subprocess.run(command, check=False, capture_output=True, text=True,
                            timeout=150 if args.software_media else 30)
    if args.software_media:
        (output / 'worker.stdout').write_text(result.stdout)
        (output / 'worker.stderr').write_text(result.stderr)
    if result.returncode:
        raise RuntimeError('metadata namespace failed: ' + result.stderr.strip())
    if result.stderr:
        raise RuntimeError('unexpected metadata namespace diagnostic: ' + result.stderr.strip())
    value = json.loads(result.stdout)
    if value.get('status') != expected_status:
        raise RuntimeError('metadata child did not complete')
    print(json.dumps(value, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print('runtime-package-namespace: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
