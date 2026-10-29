#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Render the settings menu entry for its installed executable location."""
import argparse
import os
from pathlib import Path
import sys
import tempfile


def desktop_string(value):
    return (value.replace('\\', '\\\\').replace('\n', '\\n')
            .replace('\r', '\\r').replace('\t', '\\t'))


def exec_argument(value):
    # Desktop Entry quoting is not shell quoting. The string escape layer is
    # decoded first; Exec then removes quotes and expands literal percent codes.
    # https://specifications.freedesktop.org/desktop-entry-spec/latest/exec-variables.html
    quoted = ''.join('\\' + char if char in '\\"`$' else char for char in value)
    return desktop_string('"' + quoted.replace('%', '%%') + '"')


def render(template, executable):
    if not os.path.isabs(executable) or '\0' in executable:
        raise ValueError('settings executable needs an absolute Desktop Entry path without NUL')
    command = exec_argument(executable)
    if '%' in executable:
        # Gio checks Exec's first word before expanding %% to %. Keep that
        # check on sh; the fixed script receives the filename as argv, never
        # shell code. This also handles filenames containing '=' without env's
        # assignment parsing.
        command = '/bin/sh -c ' + exec_argument('exec "$@"') + ' crystalhd-settings ' + command
    replacements = (
        ('Exec=crystalhd-settings --tray\n', 'Exec=' + command + ' --tray\n'),
        ('TryExec=crystalhd-settings\n', 'TryExec=' + desktop_string(executable) + '\n'),
    )
    for original, replacement in replacements:
        if template.count(original) != 1:
            raise ValueError('unexpected settings desktop entry')
        template = template.replace(original, replacement)
    return template


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bindir', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    temporary = None
    try:
        source = Path(__file__).resolve().parents[1] / 'share/applications/crystalhd-settings.desktop'
        text = render(source.read_text(encoding='utf-8'),
                      os.path.join(args.bindir, 'crystalhd-settings'))
        target = Path(args.output)
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                         dir=str(target.parent), delete=False) as stream:
            temporary = stream.name
            stream.write(text)
            os.fchmod(stream.fileno(), 0o644)
        os.replace(temporary, str(target))
        temporary = None
        return 0
    except (OSError, ValueError) as error:
        print('Settings menu entry could not be written: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if temporary is not None:
            os.unlink(temporary)


if __name__ == '__main__':
    sys.exit(main())
