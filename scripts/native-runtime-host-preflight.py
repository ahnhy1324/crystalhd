#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Observe native prerequisites using sysfs text and lstat, never device I/O."""
import argparse
import json
import os
from pathlib import Path
import re
import stat

VIDEO = re.compile(r'video[0-9]+\Z')
PCI = re.compile(r'[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]\Z')
MAX_VIDEO_ENTRIES = 256


def require(okay, message):
    if not okay:
        raise ValueError(message)


def inside(path, root):
    resolved = path.resolve(strict=True)
    require(resolved == root or root in resolved.parents, 'sysfs path escapes selected root')
    return resolved


def text_attribute(path, root):
    resolved = inside(path, root)
    require(stat.S_ISREG(resolved.lstat().st_mode), 'sysfs attribute is not a regular file')
    descriptor = os.open(resolved, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, 'rb') as source:
        require(stat.S_ISREG(os.fstat(source.fileno()).st_mode), 'sysfs attribute changed type')
        raw = source.read(4097)
    require(len(raw) <= 4096, 'sysfs attribute exceeds observation bound')
    value = raw.decode('ascii').strip()
    require(value and len(value) <= 256 and not any(c.isspace() for c in value),
            'sysfs attribute is empty or not one bounded token')
    return value


def inspect_video(name, sysfs_root, dev_root, module, node_stat):
    entry, node = sysfs_root / 'class/video4linux' / name, dev_root / name
    report = {'name': name, 'device_node': str(node), 'candidate': False,
              'blocking_reasons': []}
    try:
        inside(entry, sysfs_root)
        observed = text_attribute(entry / 'dev', sysfs_root)
        require(re.fullmatch(r'[0-9]+:[0-9]+', observed), 'invalid sysfs major:minor')
        major, minor = (int(value) for value in observed.split(':'))
        report['sysfs_device_number'] = {'major': major, 'minor': minor}
        info = node_stat(node)
        require(not stat.S_ISLNK(info.st_mode), 'device node is a symlink')
        require(stat.S_ISCHR(info.st_mode), 'device node is not a character device')
        actual = os.major(info.st_rdev), os.minor(info.st_rdev)
        report['observed_device_number'] = {'major': actual[0], 'minor': actual[1]}
        require(actual == (major, minor), 'device node and sysfs major:minor differ')
        device = inside(entry / 'device', sysfs_root)
        devices = inside(sysfs_root / 'devices', sysfs_root)
        require(devices in device.parents and PCI.fullmatch(device.name),
                'video parent is not a PCI device')
        require(inside(device / 'subsystem', sysfs_root) == inside(sysfs_root / 'bus/pci', sysfs_root),
                'video parent subsystem is not PCI')
        driver = inside(device / 'driver', sysfs_root)
        require(driver == inside(sysfs_root / 'bus/pci/drivers/crystalhd', sysfs_root),
                'associated PCI driver is not crystalhd')
        driver_module = inside(driver / 'module', sysfs_root)
        require(driver_module == module, 'associated PCI driver module is not crystalhd')
        report.update(candidate=True, pci_device=str(device), pci_driver=str(driver),
                      driver_module=str(driver_module))
    except (OSError, ValueError, RuntimeError) as error:
        report['blocking_reasons'].append(str(error))
    return report


def observe(sysfs_root=Path('/sys'), dev_root=Path('/dev'),
            expected_module_version=None, expected_module_srcversion=None,
            node_stat=os.lstat):
    """node_stat is injectable for inert tests; no mknod or device read needed."""
    report = {'schema': 1, 'status': 'BLOCKED_NATIVE_RUNTIME_PREREQUISITES',
              'public_distribution_status': 'PUBLIC_NOT_READY',
              'runtime_acceptance': {'native_Kodi': 'NOT_RUN', 'ABI': 'NOT_RUN',
                                     'A_V': 'NOT_RUN', 'hardware_decode': 'NOT_RUN'},
              'device_access': 'Filesystem metadata only; no device open or ioctl.',
              'module_strings_are_ABI_authentication': False,
              'selected_roots_are_authenticated': False,
              'expected_module': {'version': expected_module_version,
                                  'srcversion': expected_module_srcversion},
              'module_identity_pin_state': ('UNPINNED' if expected_module_version is None and
                                            expected_module_srcversion is None else 'EXPLICIT_EXPECTATIONS'),
              'loaded_module': {'present': False, 'version': None, 'srcversion': None},
              'video_entries': [], 'candidate_video_nodes': [], 'blocking_reasons': [],
              'scope': 'Read-only structural prerequisites, not native-runtime, ABI, ownership, '
                       'display, A/V or public-distribution acceptance.'}
    try:
        sysfs_root, dev_root = Path(sysfs_root).resolve(strict=True), Path(dev_root).resolve(strict=True)
        require(sysfs_root.is_dir() and dev_root.is_dir(), 'selected roots must be directories')
        report['roots'] = {'sysfs': str(sysfs_root), 'dev': str(dev_root)}
        module = inside(sysfs_root / 'module/crystalhd', sysfs_root)
        require(module.is_dir(), 'crystalhd module entry is not a directory')
        report['loaded_module']['present'] = True
        for name, expected in (('version', expected_module_version), ('srcversion', expected_module_srcversion)):
            try:
                value = text_attribute(module / name, sysfs_root)
                report['loaded_module'][name] = value
                if expected is not None:
                    require(isinstance(expected, str) and expected and value == expected,
                            'observed module ' + name + ' differs from expected value')
            except (OSError, ValueError, RuntimeError) as error:
                report['blocking_reasons'].append(str(error))
        try:
            directory = inside(sysfs_root / 'class/video4linux', sysfs_root)
            names = []
            for entry in directory.iterdir():
                if VIDEO.fullmatch(entry.name):
                    require(len(names) < MAX_VIDEO_ENTRIES, 'video entry observation limit exceeded')
                    names.append(entry.name)
            for name in sorted(names, key=lambda value: int(value[5:])):
                item = inspect_video(name, sysfs_root, dev_root, module, node_stat)
                report['video_entries'].append(item)
                if item['candidate']:
                    report['candidate_video_nodes'].append(item['device_node'])
        except (OSError, ValueError, RuntimeError) as error:
            report['blocking_reasons'].append(str(error))
        if not report['candidate_video_nodes']:
            report['blocking_reasons'].append('no structurally matching crystalhd PCI video node')
        if not report['blocking_reasons']:
            report['status'] = 'PREREQUISITES_PRESENT_UNVERIFIED'
    except (OSError, ValueError, RuntimeError) as error:
        report['blocking_reasons'].append(str(error))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysfs-root', type=Path, default=Path('/sys'))
    parser.add_argument('--dev-root', type=Path, default=Path('/dev'))
    parser.add_argument('--expected-module-version')
    parser.add_argument('--expected-module-srcversion')
    args = parser.parse_args(argv)
    report = observe(args.sysfs_root, args.dev_root, args.expected_module_version,
                     args.expected_module_srcversion)
    print(json.dumps(report, sort_keys=True, indent=2))
    return 0 if report['status'] == 'PREREQUISITES_PRESENT_UNVERIFIED' else 2


if __name__ == '__main__':
    raise SystemExit(main())
