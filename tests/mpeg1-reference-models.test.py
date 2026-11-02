#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run only the three reference models' fixed synthetic CPU controls.

No manifest, retained media, converter, decoder or hardware is used. NumPy is
an explicit dependency of this developer check, not of the installed product.
"""

import hashlib
import importlib.util
import json
from pathlib import Path
import sys


sys.dont_write_bytecode = True
DIRECTORY = Path(__file__).resolve().parent
MODELS = (
    ("axis", "mpeg1-axis-topology.py", 22, "hardware_or_media"),
    ("vertical", "mpeg1-vertical31-reference.py", 19, "actual_media_or_hardware"),
    ("separable", "mpeg1-separable-reference.py", 12, "actual_media_or_hardware"),
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
    spec = importlib.util.spec_from_file_location("mpeg1_synthetic_" + label, path)
    require(spec is not None and spec.loader is not None,
            "cannot import local " + label + " model")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    require(identity(path) == before, label + " source changed during import")
    return module, before


def main():
    require(len(sys.argv) == 1, "usage: mpeg1-reference-models.test.py")
    modules, providers = {}, {}
    for label, filename, _count, _scope in MODELS:
        modules[label], providers[label] = load(label, filename)
    axis, vertical, separable = (modules[name] for name in ("axis", "vertical", "separable"))
    require(axis.np is vertical.np and axis.np is separable.np,
            "reference models do not share the same NumPy provider")
    results = {
        "axis": axis.self_test(),
        "vertical": vertical.self_test(axis),
        "separable": separable.self_test(axis, vertical),
    }
    counts = {}
    for label, filename, expected, scope_key in MODELS:
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
    require(sum(counts.values()) == 53, "total synthetic control count differs")
    summary = {
        "schema": "crystalhd.mpeg1-reference-model-controls.v1",
        "status": "PASS_SYNTHETIC_MODEL_CONTROLS",
        "control_groups": counts, "total_control_groups": 53,
        "providers": providers,
        "python": {"version": sys.version, "executable": str(Path(sys.executable).resolve())},
        "numpy": {"version": axis.np.__version__, "file": str(Path(axis.np.__file__).resolve())},
        "media_manifest_converter_decoder_hardware": "NOT_RUN",
        "scope": "Fixed synthetic model arithmetic and statistics only; no measured-media or production-codec PASS",
    }
    print(json.dumps(summary, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
