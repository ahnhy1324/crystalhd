#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Build the optional settings ZIP from an explicit file list, reproducibly."""
import argparse
from pathlib import Path
import re
import xml.etree.ElementTree as ET
import zipfile


ADDON = 'script.crystalhd.settings'
FILES = ('addon.xml', 'addon.py', 'README.md', 'resources/__init__.py',
         'resources/lib/__init__.py', 'resources/lib/controller.py',
         'resources/language/resource.language.en_gb/strings.po',
         'resources/language/resource.language.ko_kr/strings.po')
LICENSE = 'filters/gst/gst-plugin/COPYING'


def build(source, output, read_file=None):
    source, output = Path(source), Path(output)
    read_file = read_file or (lambda name: (source / name).read_bytes())
    prefix = 'addons/' + ADDON + '/'
    metadata = ET.fromstring(read_file(prefix + 'addon.xml'))
    if metadata.get('id') != ADDON or re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', metadata.get('version', '')) is None:
        raise ValueError('unsupported add-on identity')
    payload = {name: read_file(prefix + name) for name in FILES}
    payload['LICENSE.txt'] = read_file(LICENSE)
    # Exclusive creation protects previously qualified packages from replacement.
    with output.open('xb') as stream:
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in sorted(payload.items()):
                info = zipfile.ZipInfo(ADDON + '/' + name, date_time=(1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = 0o100644 << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
    return {'addon_id': ADDON, 'addon_version': metadata.get('version'), 'files': len(payload)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(build(args.source, args.output))


if __name__ == '__main__':
    main()
