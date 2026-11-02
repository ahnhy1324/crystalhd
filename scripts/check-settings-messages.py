#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that gettext extraction preserves every settings message verbatim."""
import ast
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def source_messages(path):
    tree = ast.parse(path.read_text(encoding="utf-8"))
    messages = set()
    for node in ast.walk(tree):
        if (not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name)
                or node.func.id != "_" or len(node.args) != 1):
            continue
        literal = node.args[0]
        if isinstance(literal, getattr(ast, "Constant", ())):
            value = literal.value
        elif isinstance(literal, getattr(ast, "Str", ())):
            value = literal.s
        else:
            continue
        if isinstance(value, str):
            messages.add(value)
    return messages


def template_messages(path):
    messages, parts = set(), None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("msgid "):
            parts = [ast.literal_eval(line[6:])]
        elif parts is not None and line.startswith('"'):
            parts.append(ast.literal_eval(line))
        elif parts is not None:
            messages.add("".join(parts))
            parts = None
    if parts is not None:
        messages.add("".join(parts))
    messages.discard("")  # Catalogue metadata, rather than an application message.
    return messages


def main():
    source = source_messages(ROOT / "scripts/crystalhd-settings.py")
    template = template_messages(ROOT / "po/crystalhd-settings.pot")
    if source != template:
        print("Settings extraction changed or omitted message text.", file=sys.stderr)
        for text in sorted(source - template):
            print("Missing: " + repr(text), file=sys.stderr)
        for text in sorted(template - source):
            print("Unexpected: " + repr(text), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
