#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require format-boundary assertions to reject compiled production mutations.

Only temporary copies of the headers already extracted by v4l2-node.sh are
changed. Production files, clients, kernel modules and devices are not touched.
"""

import argparse
import os
from pathlib import Path
import resource
import shlex
import shutil
import subprocess
import tempfile


def replace_once(body, old, new):
    if body.count(old) != 1:
        raise RuntimeError("production mutation anchor is absent or ambiguous: " + old)
    return body.replace(old, new, 1)


def mutations(body):
    colour = "\tchd_flea_source_colors(f, result->pib.ppb.colour_primaries);"
    event = "\tv4l2_event_queue_fh(&f->fh, &event);"
    yield "omit-source-colour", replace_once(body, colour, "\t(void)result->pib.ppb.colour_primaries;")
    yield "suppress-dynamic-notification", replace_once(body, event, "\tif (initial) v4l2_event_queue_fh(&f->fh, &event);")
    after = replace_once(body, colour + "\n", "")
    yield "colour-after-notification", replace_once(after, event, event + "\n" + colour)
    yield "omit-boundary-retirement", replace_once(body,
        "\t\t\tcrystalhd_v4l2_capture_retire(boundary);",
        "\t\t\tif (false) crystalhd_v4l2_capture_retire(boundary);")
    yield "ignore-still-owned-boundary", replace_once(body,
        "\t\t\tif (crystalhd_v4l2_capture_owned(boundary))\n\t\t\t\treturn -EIO;",
        "\t\t\t(void)crystalhd_v4l2_capture_owned(boundary);")
    yield "omit-boundary-last", replace_once(body,
        "\t\tboundary->m2m.vb.flags |= V4L2_BUF_FLAG_LAST;",
        "\t\tboundary->m2m.vb.flags |= 0;")
    yield "ignore-pause-failure", replace_once(body,
        "\tif (sts != BC_STS_SUCCESS)\n\t\treturn crystalhd_status_to_errno(sts);",
        "\tif (false && sts != BC_STS_SUCCESS)\n\t\treturn crystalhd_status_to_errno(sts);")
    yield "omit-general-buffer-return", replace_once(body,
        "\tchd_return_active(f);", "\t(void)f;")


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--include-dir", type=Path, required=True)
    parser.add_argument("--repo-dir", type=Path, required=True)
    args = parser.parse_args()
    original = (args.include_dir / "node-production.h").read_text()
    start = original.index("static int chd_format(")
    end = original.index("\n}\n", start) + len("\n}\n")
    body = original[start:end]
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    with tempfile.TemporaryDirectory(prefix="crystalhd-node-format-mutants.") as temp:
        root = Path(temp)
        for header in args.include_dir.glob("*.h"):
            shutil.copyfile(header, root / header.name)
        for name, mutated in mutations(body):
            (root / "node-production.h").write_text(original[:start] + mutated + original[end:])
            compiled = subprocess.run(compiler + flags + [
                "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-DNODE_KERNEL_VERSION=331520",  # 5.15.0, matching a positive run.
                "-I" + str(root), "-I" + str(args.repo_dir / "include"),
                str(args.repo_dir / "tests/v4l2-node.c"), "-o", str(root / "check"),
            ], capture_output=True, text=True, check=False)
            if compiled.returncode:
                raise RuntimeError(name + " did not compile:\n" + compiled.stderr)
            checked = subprocess.run([str(root / "check")], capture_output=True,
                                     text=True, check=False, preexec_fn=no_core)
            if checked.returncode == 0 or not any(marker in checked.stderr for marker in (
                    "V4L2 node failure at line", "Assertion")):
                raise RuntimeError(name + " was not rejected by a runtime assertion:\n" +
                                   checked.stdout + checked.stderr)
            print("V4L2 format semantic negative: " + name + " REJECTED", flush=True)
    print("V4L2 format semantic negatives: 8 compiled, 8 rejected")


if __name__ == "__main__":
    main()
