#!/usr/bin/env python3
"""Check that CONFIGURATION.md matches the production configuration surface."""

from __future__ import annotations

import ast
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DOCUMENT = ROOT / "CONFIGURATION.md"

# Only runtime-facing production readers belong in the user inventory.  These
# are deliberately not configuration surfaces:
#
# * tests/ and tools/fw-research/ controls are test/research inputs;
# * CRYSTALHD_CHECK_ROOT is an alternate-root seam for crystalhd-check tests;
# * CRYSTALHD_FW_RESEARCH and CRYSTALHD_V4L2 are build-time feature switches;
# * LEGACY_CPU is a build choice, documented with the build instructions.
EXCLUDED_SCRIPT_ENVIRONMENTS = {"CRYSTALHD_CHECK_ROOT"}
EXCLUDED_BUILD_CONTROLS = {
    "CRYSTALHD_FW_RESEARCH",
    "CRYSTALHD_V4L2",
    "LEGACY_CPU",
}
EXCLUDED_PATH_PREFIXES = ("tests/", "tools/fw-research/")

MARKER = re.compile(
    r"<!--\s*config-surface:(module|environment|gst-property|player-flag):"
    r"([^\s]+)\s*-->"
)
MODULE_PARAMETER = re.compile(
    r"\bmodule_param(?:_named)?\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)"
)
GETENV = re.compile(
    r"\b(?:std::)?getenv\s*\(\s*\"(CRYSTALHD_[A-Z0-9_]+)\"\s*\)"
)
CRYSTALHD_TOKEN = re.compile(r"\bCRYSTALHD_[A-Z][A-Z0-9_]*\b")
GST_PROPERTY = re.compile(
    r"g_param_spec_[a-z0-9_]+\s*\(\s*\"([a-z][a-z0-9-]*)\"",
    re.DOTALL,
)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def production_environments() -> set[str]:
    names: set[str] = set()

    for directory in (ROOT / "linux_lib/libcrystalhd", ROOT / "filters/vaapi"):
        for path in directory.iterdir():
            if path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                names.update(GETENV.findall(read(path)))

    # Shell parameter expansion is intentionally flexible, so inventory every
    # CrystalHD-named token in installed scripts and remove the one explicit
    # test seam.  Build, test and research trees are not scanned as runtime
    # settings; their boundaries are declared above rather than silently
    # mixed into the public interface.
    for path in (ROOT / "scripts").iterdir():
        if path.is_file():
            names.update(CRYSTALHD_TOKEN.findall(read(path)))
    names.difference_update(EXCLUDED_SCRIPT_ENVIRONMENTS)
    return names


def module_parameters() -> set[str]:
    names: set[str] = set()
    for path in (ROOT / "driver/linux").glob("*.c"):
        names.update(MODULE_PARAMETER.findall(read(path)))
    return names


def gst_properties() -> set[str]:
    names: set[str] = set()
    for path in (ROOT / "filters/gst/gst-plugin-1.0").glob("*.c"):
        names.update(GST_PROPERTY.findall(read(path)))
    return names


def player_flags() -> set[str]:
    tree = ast.parse(read(ROOT / "scripts/crystalhd-player.py"))
    names: set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        function = node.func
        if not isinstance(function, ast.Attribute) or function.attr != "add_argument":
            continue
        for argument in node.args:
            if isinstance(argument, ast.Constant) and isinstance(argument.value, str):
                if argument.value.startswith("--"):
                    names.add(argument.value)
    return names


def documented_surfaces() -> dict[str, set[str]]:
    result = {
        "module": set(),
        "environment": set(),
        "gst-property": set(),
        "player-flag": set(),
    }
    document = read(DOCUMENT)
    for line in document.splitlines():
        found = MARKER.search(line)
        if found is None:
            continue
        kind, name = found.groups()
        if not line.startswith("|") or not line.endswith("|") or line.count("|") != 9:
            raise ValueError(f"{kind} marker is not in an eight-column row: {name}")
        if name in result[kind]:
            raise ValueError(f"duplicate {kind} marker: {name}")
        result[kind].add(name)
    return result


def main() -> int:
    actual = {
        "module": module_parameters(),
        "environment": production_environments(),
        "gst-property": gst_properties(),
        "player-flag": player_flags(),
    }
    try:
        documented = documented_surfaces()
    except (OSError, UnicodeError, ValueError, SyntaxError) as error:
        print(f"configuration inventory: FAIL: {error}", file=sys.stderr)
        return 1

    failed = False
    for kind in actual:
        missing = sorted(actual[kind] - documented[kind])
        stale = sorted(documented[kind] - actual[kind])
        if missing:
            failed = True
            print(
                f"configuration inventory: FAIL: undocumented {kind}: "
                + ", ".join(missing),
                file=sys.stderr,
            )
        if stale:
            failed = True
            print(
                f"configuration inventory: FAIL: stale {kind}: "
                + ", ".join(stale),
                file=sys.stderr,
            )

    excluded = EXCLUDED_SCRIPT_ENVIRONMENTS | EXCLUDED_BUILD_CONTROLS
    leaked = sorted(
        name for names in documented.values() for name in names if name in excluded
    )
    if leaked:
        failed = True
        print(
            "configuration inventory: FAIL: excluded controls documented as "
            "runtime settings: " + ", ".join(leaked),
            file=sys.stderr,
        )

    if failed:
        return 1
    count = sum(len(names) for names in actual.values())
    print(
        f"configuration inventory: PASS ({count} production surfaces; "
        f"excluded paths: {', '.join(EXCLUDED_PATH_PREFIXES)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
