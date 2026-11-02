#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Sanitize selected production DIL functions in fresh, device-free binaries."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SANITIZER_FLAGS = (
    "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
)
# No shared DIL library, driver, normal binary or CPU-mode stamp is built or
# loaded. Discard unrelated production sections, and reject any reachable open
# or ioctl that is not already guarded by the existing test itself.
GUARD = r'''#include <cstdio>
#include <cstdlib>
static void unexpected_device_call()
{
    std::fputs("unexpected device operation in selected DIL sanitizer test\n", stderr);
    std::abort();
}
extern "C" {
#ifndef LIBRARY_SANITIZER_TEST_OPEN_GUARDS
int __wrap_open(const char *, int, ...) { unexpected_device_call(); return -1; }
int __wrap_openat(int, const char *, int, ...) { unexpected_device_call(); return -1; }
#endif
int __wrap_open64(const char *, int, ...) { unexpected_device_call(); return -1; }
int __wrap_openat64(int, const char *, int, ...) { unexpected_device_call(); return -1; }
#ifndef LIBRARY_SANITIZER_TEST_IOCTL_GUARD
int __wrap_ioctl(int, unsigned long, ...) { unexpected_device_call(); return -1; }
#endif
}
'''


def arguments(environment, name, default=""):
    return shlex.split(environment.get(name, default))


def check():
    if len(sys.argv) != 1:
        raise ValueError("usage: library-selected-sanitizer.py")
    if os.environ.get("CRYSTALHD_LIBRARY_SANITIZER_LEGACY", "0") != "0":
        raise ValueError("selected DIL sanitizer requires LEGACY_CPU=0; legacy sanitizer coverage is unqualified")
    environment = os.environ.copy()
    environment.update(
        CRYSTALHD_CONFIG_DISABLE="1", LD_LIBRARY_PATH="", LD_PRELOAD="",
        ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23",
    )
    compiler = arguments(environment, "CXX", "c++")
    if not compiler:
        raise ValueError("CXX must name a compiler")
    common = [
        *compiler, *arguments(environment, "CPPFLAGS"),
        *arguments(environment, "CXXFLAGS"),
        *arguments(environment, "CRYSTALHD_LIBRARY_SANITIZER_CPU_FLAGS"),
        "-std=c++11", "-Wall", "-Werror",
        "-ffunction-sections", "-fdata-sections", "-D__LINUX_USER__",
        "-I" + str(ROOT / "linux_lib/libcrystalhd"),
        "-I" + str(ROOT / "include"), "-I" + str(ROOT / "include/link"),
    ]
    library = ROOT / "linux_lib/libcrystalhd"
    private = library / "libcrystalhd_priv.cpp"
    public = library / "libcrystalhd_if.cpp"
    internal = library / "libcrystalhd_int_if.cpp"
    # format owns a strict fake-FD ioctl definition. Its private wrapper below
    # keeps that stub off the sanitizer runtime's own libc symbol resolution.
    # tx-concurrency already owns strict open/openat/ioctl wrappers.
    cases = (
        ("tx-ring", (private, public), (), ("pthread_mutex_lock",), ((),)),
        ("tx-concurrency", (private, public, internal),
         ("-DLIBRARY_SANITIZER_TEST_OPEN_GUARDS", "-DLIBRARY_SANITIZER_TEST_IOCTL_GUARD"),
         ("pthread_mutex_lock",), ((), ("status",), ("serial",))),
        ("copy", (internal,), (), (), ((),)),
        ("planar", (internal,), (), (), ((),)),
        ("format", (internal, private, public),
         ("-DLIBRARY_SANITIZER_TEST_IOCTL_GUARD",), (), ((),)),
    )
    invocations = 0
    with tempfile.TemporaryDirectory(prefix="crystalhd-library-sanitized-") as temporary:
        directory = Path(temporary)
        guard = directory / "device-guard.cpp"
        guard.write_text(GUARD, encoding="utf-8")
        for name, sources, defines, wraps, modes in cases:
            binary = directory / ("library-" + name)
            wrapped = ("open", "openat", "open64", "openat64", *wraps)
            wrapped += ("ioctl",)
            test_source = ROOT / "tests" / ("library-" + name + ".cpp")
            if name == "format":
                # The ordinary test interposes libc ioctl directly. Rename
                # only this test translation unit's stub; production sections
                # still compile normally and reach it through --wrap=ioctl.
                wrapped_source = directory / "library-format.cpp"
                wrapped_source.write_text(
                    "#define ioctl __wrap_ioctl\n#include " +
                    '"' + str(test_source).replace("\\", "\\\\").replace('"', '\\"') + '"\n',
                    encoding="utf-8")
                test_source = wrapped_source
            command = [
                *common, *defines, str(test_source),
                *map(str, sources), str(guard), *arguments(environment, "LDFLAGS"),
                # Fixed sanitizer options follow caller flags and apply to
                # compilation and linking in this single compiler invocation.
                *SANITIZER_FLAGS, "-Wl,--gc-sections",
                "-Wl," + ",".join("--wrap=" + symbol for symbol in wrapped),
                "-pthread", "-lrt", "-o", str(binary),
            ]
            print("Selected DIL sanitizer build: " + shlex.join(command), flush=True)
            subprocess.run(command, env=environment, check=True)
            for mode in modes:
                print("Selected DIL sanitizer run: " + name +
                      (" " + " ".join(mode) if mode else ""), flush=True)
                subprocess.run([str(binary), *mode], env=environment, check=True)
                invocations += 1
    print("Selected DIL ASan/UBSan/leak checks: {} cases / {} invocations passed".format(
        len(cases), invocations))


if __name__ == "__main__":
    try:
        check()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("Selected DIL sanitizer failed: " + str(error), file=sys.stderr)
        sys.exit(1)
