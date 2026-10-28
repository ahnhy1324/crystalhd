#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Install the player with fixed relative controller/core locations."""
import argparse
import os
from pathlib import Path
import re
import shlex
import tempfile


def source_identity():
    value = os.environ.get('CRYSTALHD_SOURCE_ID', 'unknown')
    if (value != 'unknown' and
            re.match(r'^(?:git|archive):[0-9a-f]{40}$', value) is None):
        raise RuntimeError('invalid CrystalHD source identity')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bindir', required=True)
    parser.add_argument('--datadir', required=True)
    parser.add_argument('--destdir', default='')
    args = parser.parse_args()
    if not all(os.path.isabs(path) for path in (args.bindir, args.datadir)):
        parser.error('BINDIR and DATADIR must be absolute')
    root = Path(__file__).resolve().parent
    stage = Path(args.destdir or '/')
    controller_dir = os.path.join(args.datadir, 'crystalhd')
    replacements = (
        ('crystalhd-play', os.path.join(args.bindir, 'crystalhd-play'), 0o755,
         "'@PLAYER_CONTROLLER_RELATIVE@'", shlex.quote(os.path.relpath(
             os.path.join(controller_dir, 'player.py'), args.bindir))),
        ('crystalhd-player.py', os.path.join(controller_dir, 'player.py'), 0o644,
         '"@PLAYER_CONFIG_CORE_RELATIVE@"', repr(os.path.relpath(
             os.path.join(args.bindir, 'crystalhd-config'), controller_dir))),
    )
    identity = source_identity()
    os.umask(0o022)
    for source, destination, mode, needle, value in replacements:
        text = (root / source).read_text(encoding='utf-8')
        if text.count(needle) != 1 or text.count('@PLAYER_SOURCE_ID@') != 1:
            raise RuntimeError('unexpected player location token count')
        target = stage / destination.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                             dir=str(target.parent), delete=False) as stream:
                temporary = stream.name
                stream.write(text.replace(needle, value).replace(
                    '@PLAYER_SOURCE_ID@', identity))
            os.chmod(temporary, mode)
            os.replace(temporary, str(target))
        finally:
            if temporary is not None and os.path.exists(temporary):
                os.unlink(temporary)


if __name__ == '__main__':
    main()
