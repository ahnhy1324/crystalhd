#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Render the browser launcher's installed H.264 extension location."""
import argparse
import os
from pathlib import Path
import shlex
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bindir', required=True)
    parser.add_argument('--datadir', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().with_name('crystalhd-chromium')
    text = source.read_text(encoding='utf-8')
    token = "'@CHROMIUM_EXTENSION_RELATIVE@'"
    if text.count(token) != 1:
        parser.error('unexpected browser extension location token count')
    relative = os.path.relpath(os.path.join(args.datadir, 'crystalhd/h264-only'), args.bindir)
    target = Path(args.output)
    os.umask(0o022)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                         dir=str(target.parent), delete=False) as stream:
            temporary = stream.name
            stream.write(text.replace(token, shlex.quote(relative)))
            os.fchmod(stream.fileno(), 0o755)
        os.replace(temporary, str(target))
        temporary = None
    finally:
        if temporary is not None:
            os.unlink(temporary)


if __name__ == '__main__':
    main()
