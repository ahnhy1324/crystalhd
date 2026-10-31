#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run fixed packet-mapping and metric controls without media or manifests.

NumPy is an explicit dependency of this developer check, not of the installed
product. No capture, converter, decoder or hardware is used.
"""

import hashlib
import importlib.util
import json
from pathlib import Path
import sys


sys.dont_write_bytecode = True
DIRECTORY = Path(__file__).resolve().parent
PROVIDERS = (
    ("packet_mapping", "mpeg1-natural-proof.py", 38, "hardware_or_actual_media"),
    ("metrics", "mpeg1-natural-audit.py", 10, "actual_media_or_hardware"),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def identity(path):
    data = path.read_bytes()
    return {"file": path.name, "path": str(path.resolve()), "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


def load(label, filename):
    path = DIRECTORY / filename
    before = identity(path)
    spec = importlib.util.spec_from_file_location("mpeg1_natural_controls_" + label, path)
    require(spec is not None and spec.loader is not None,
            "cannot import local " + label + " provider")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    require(identity(path) == before, label + " source changed during import")
    return module, before


def main():
    require(len(sys.argv) == 1, "usage: mpeg1-natural-audit.test.py")
    modules, providers = {}, {}
    for label, filename, _count, _scope in PROVIDERS:
        modules[label], providers[label] = load(label, filename)
    results = {
        "packet_mapping": modules["packet_mapping"].self_test(),
        "metrics": modules["metrics"].metric_controls(),
    }
    counts = {}
    for label, filename, expected, scope_key in PROVIDERS:
        result = results[label]
        require(isinstance(result, dict) and result.get("status") == "PASS",
                label + " synthetic controls did not pass")
        count, names = result.get("control_groups"), result.get("names")
        require(type(count) is int and count == expected,
                label + " synthetic control count differs")
        require(isinstance(names, list) and len(names) == expected and
                all(isinstance(name, str) and name for name in names) and
                len(set(names)) == expected,
                label + " synthetic control names are missing or duplicated")
        require(result.get(scope_key) == "NOT_RUN",
                label + " unexpectedly claims media or hardware execution")
        require(identity(DIRECTORY / filename) == providers[label],
                label + " source changed during the controls")
        counts[label] = count
    require(sum(counts.values()) == 48, "total synthetic control count differs")
    audit = modules["metrics"]
    summary = {
        "schema": "crystalhd.mpeg1-natural-audit-controls.v1",
        "status": "PASS_SYNTHETIC_AUDIT_CONTROLS",
        "control_groups": counts, "total_control_groups": 48,
        "controls": results, "providers": providers,
        "python": {"version": sys.version, "executable": str(Path(sys.executable).resolve())},
        "numpy": {"version": audit.np.__version__, "file": str(Path(audit.np.__file__).resolve())},
        "media_manifest_capture_converter_decoder_hardware": "NOT_RUN",
        "scope": "Fixed synthetic packet/picture/token mapping and metric arithmetic only; no measured-media or production-codec PASS",
    }
    print(json.dumps(summary, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
