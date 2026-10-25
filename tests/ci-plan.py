#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Conservative CI selection from a complete Git PR diff, plus final gate."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


# Explicit files only. Fixtures, generated inputs and new/unknown docs are full.
PROSE = frozenset({
    "README.md", "HISTORY.md", "TODO.md", "BRINGUP.md", "CONTRIBUTING.md",
    "RELEASING.md", "INSTALL.md", "USAGE.md", "VAAPI.md", "GSTREAMER.md",
    "MOONLIGHT.md", "HARDWARE-2026-09-13.md", "LICENSES.md",
})
CONTRACT = frozenset({"CONFIGURATION.md", "CONFIGURATION-SCHEMA.md"})


def git(root, *args):
    return subprocess.check_output(
        ["git", "-C", str(root), *args], stderr=subprocess.PIPE)


def classify(root, base, head):
    for revision in (base, head):
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise ValueError("full commit SHA required")
        git(root, "rev-parse", "--verify", revision + "^{commit}")
    # Three-dot uses the PR's merge base, never merely its final commit.
    raw = git(root, "diff", "--raw", "-z", "--no-renames", "--no-ext-diff",
              "--no-textconv", base + "..." + head, "--")
    chunks = raw.split(b"\0")
    if chunks.pop() != b"" or len(chunks) % 2:
        raise ValueError("incomplete raw Git diff")
    heavy = not chunks
    contracts = False
    files = []
    for record, path in zip(chunks[::2], chunks[1::2]):
        fields = record.decode("ascii").split()
        if len(fields) != 5 or not fields[0].startswith(":") or not path:
            raise ValueError("invalid raw Git diff record")
        old_mode, new_mode, _old, _new, status = fields
        name = path.decode("utf-8", errors="strict")
        plain = (status == "M" and old_mode == ":100644" and
                 new_mode == "100644")
        lane = "full"
        if plain and name in PROSE:
            lane = "prose"
        elif plain and name in CONTRACT:
            lane = "contract"
            contracts = True
        else:
            heavy = True
        files.append({"path": name, "status": status, "lane": lane,
                      "old_mode": old_mode[1:], "new_mode": new_mode})
    return {"heavy": heavy, "contracts": contracts,
            "reason": "full required" if heavy else "reviewed documentation only",
            "base": base, "head": head, "files": files}


def gate(needs):
    expected = {"classify", "docs", "build", "upstream-kernel-api"}
    if set(needs) != expected:
        raise ValueError("missing or unknown final-gate dependency")
    if needs["classify"].get("result") != "success":
        raise ValueError("classifier did not succeed")
    outputs = needs["classify"].get("outputs", {})
    if outputs.get("heavy") not in {"true", "false"}:
        raise ValueError("missing/invalid lane selection")
    if outputs.get("contracts") not in {"true", "false"}:
        raise ValueError("missing/invalid contract selection")
    for name in ("docs", "build", "upstream-kernel-api"):
        required = "success" if name == "docs" or outputs["heavy"] == "true" else "skipped"
        if needs[name].get("result") != required:
            raise ValueError(name + " must be " + required)


def docs(root, base, head, contracts):
    # Diff validation works without Python packages or a compiled driver.
    git(root, "diff", "--check", base + "..." + head, "--")
    git(root, "diff", "--check", head, "--")
    if contracts:
        for script in ("configuration-inventory.test.py", "configuration-schema.test.py"):
            subprocess.run(["python3", "-B", str(root / "tests" / script)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("classify", "docs", "gate"))
    args = parser.parse_args()
    if args.command == "gate":
        gate(json.loads(os.environ["CI_NEEDS"]))
        print("All selected CI jobs succeeded; unselected heavy jobs were skipped.")
        return
    root = Path(__file__).resolve().parents[1]
    base, head = os.environ.get("CI_BASE", ""), os.environ["CI_HEAD"]
    if args.command == "docs":
        # Full push/manual/tag runs need no PR base, but still run contracts.
        docs(root, base or head, head, os.environ["CI_CONTRACTS"] == "true")
        return
    if os.environ["CI_EVENT"] == "pull_request":
        plan = classify(root, base, head)
    else:
        plan = {"heavy": True, "contracts": True, "files": [],
                "reason": "full master/tag/manual route", "base": base, "head": head}
    print(json.dumps(plan, indent=2))
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        for name in ("heavy", "contracts"):
            output.write(name + "=" + str(plan[name]).lower() + "\n")
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
        summary.write("### Selected CI plan\n\n```json\n" +
                      json.dumps(plan, indent=2) + "\n```\n")


if __name__ == "__main__":
    main()
