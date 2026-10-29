#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run only package metadata inside a device-free pinned Platform namespace.

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
    args = parser.parse_args()
    package = args.package.resolve(strict=True)
    platform = args.platform_root.resolve(strict=True)
    prefix = Path(args.mount_prefix)
    if not prefix.is_absolute() or len(prefix.parts) != 3 or prefix.parts[1] != 'opt':
        parser.error('--mount-prefix must be one directory directly below /opt')
    probe = Path(__file__).with_name('runtime-package-metadata.py')
    command = ['bwrap', '--die-with-parent', '--unshare-all', '--clearenv',
               '--ro-bind', str(platform), '/usr',
               '--ro-bind', str(platform / 'etc'), '/etc',
               '--symlink', 'usr/bin', '/bin', '--symlink', 'usr/sbin', '/sbin',
               '--symlink', 'usr/lib', '/lib', '--symlink', 'usr/lib64', '/lib64',
               '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
               '--dir', '/work', '--ro-bind', str(package), str(prefix),
               '--ro-bind', str(probe), '/proof/metadata.py',
               '--chdir', '/work', '--setenv', 'PATH', '/usr/bin',
               '--setenv', 'PYTHONDONTWRITEBYTECODE', '1',
               '--setenv', 'LC_ALL', 'C.UTF-8', '--setenv', 'LD_LIBRARY_PATH',
               str(prefix / 'lib') + ':/usr/lib/x86_64-linux-gnu:/usr/lib',
               '--', '/usr/bin/python3', '/proof/metadata.py', '--package', str(prefix)]
    result = subprocess.run(command, check=False, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError('metadata namespace failed: ' + result.stderr.strip())
    if result.stderr:
        raise RuntimeError('unexpected metadata namespace diagnostic: ' + result.stderr.strip())
    value = json.loads(result.stdout)
    if value.get('status') != 'PASS_COMPILERLESS_RELOCATED_METADATA_ONLY':
        raise RuntimeError('metadata child did not complete')
    print(json.dumps(value, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print('runtime-package-namespace: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
