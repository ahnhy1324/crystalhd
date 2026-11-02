#!/usr/bin/env python3
"""Device-free tests for the embedded-FFmpeg Kodi metadata probe."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


DEPENDENCY_SOURCE = r'''
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void dependency_constructor(void) {
    const char *path = getenv("CRYSTALHD_FIXTURE_DEPENDENCY_MARKER");
    const char value[] = "dependency-constructor\n";
    const char stdout_value[] = "dependency stdout noise\n";
    const char stderr_value[] = "dependency stderr noise\n";
    int fd;
    (void)write(STDOUT_FILENO, stdout_value, sizeof(stdout_value) - 1U);
    (void)write(STDERR_FILENO, stderr_value, sizeof(stderr_value) - 1U);
    if (!path) return;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    (void)write(fd, value, sizeof(value) - 1U);
    (void)close(fd);
}

int fixture_dependency_anchor(void) { return 0; }
'''


KODI_SOURCE = r'''
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef KODI_MAJOR
#define KODI_MAJOR 21
#endif
#ifndef KODI_MINOR
#define KODI_MINOR 3
#endif
#ifndef AVCODEC_MAJOR
#define AVCODEC_MAJOR 60
#endif
#ifndef HIDE_KODI_VERSION
#define HIDE_KODI_VERSION 0
#endif
#ifndef HIDE_AVCODEC_VERSION
#define HIDE_AVCODEC_VERSION 0
#endif
#ifndef WITH_PREINIT
#define WITH_PREINIT 0
#endif

struct config { int pix_fmt; int methods; int device_type; };
__attribute__((unused)) static const struct config non_drm_config = { 124, 4, 2 };
__attribute__((unused)) static const struct config drm_internal_config = { 123, 4, 3 };
__attribute__((unused)) static const struct config drm_device_config = { 123, 1, 2 };
__attribute__((unused)) static const struct config wrong_device_config = { 123, 1, 3 };

extern int fixture_dependency_anchor(void);

static void mark(const char *value) {
    const char *path = getenv("CRYSTALHD_FIXTURE_TARGET_MARKER");
    int fd;
    if (!path) return;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    (void)write(fd, value, strlen(value));
    (void)close(fd);
}

#if WITH_PREINIT
static void target_preinit(void) { mark("target-preinit\n"); }
__attribute__((section(".preinit_array"), used))
static void (*const target_preinit_entry)(void) = target_preinit;
#endif

__attribute__((constructor)) static void target_constructor(void) {
    mark("target-constructor\n");
}

#if HIDE_KODI_VERSION
#define KODI_VERSION_VISIBILITY __attribute__((visibility("hidden")))
#else
#define KODI_VERSION_VISIBILITY
#endif
KODI_VERSION_VISIBILITY int kodi_major(void) __asm__("_ZN12CCompileInfo8GetMajorEv");
KODI_VERSION_VISIBILITY int kodi_minor(void) __asm__("_ZN12CCompileInfo8GetMinorEv");
KODI_VERSION_VISIBILITY int kodi_major(void) { return KODI_MAJOR; }
KODI_VERSION_VISIBILITY int kodi_minor(void) { return KODI_MINOR; }

#if HIDE_AVCODEC_VERSION
#define AVCODEC_VERSION_VISIBILITY __attribute__((visibility("hidden")))
#else
#define AVCODEC_VERSION_VISIBILITY
#endif
AVCODEC_VERSION_VISIBILITY unsigned avcodec_version(void) {
    return (AVCODEC_MAJOR << 16) | (31U << 8) | 102U;
}
unsigned avformat_version(void) { return (60U << 16) | (16U << 8) | 100U; }
unsigned avutil_version(void) { return (58U << 16) | (29U << 8) | 100U; }

const char *av_get_pix_fmt_name(int value) {
    if (value == 123) return "drm_prime";
    if (value == 124) return "yuyv422";
    return NULL;
}

const char *av_hwdevice_get_type_name(int value) {
    if (value == 2) return "drm";
    if (value == 3) return "vaapi";
    return NULL;
}

void *avcodec_find_decoder_by_name(const char *name) {
#if PROBE_MODE == 0
    (void)name;
    return NULL;
#else
    return name && strcmp(name, "h264_v4l2m2m") == 0 ? (void *)0x1 : NULL;
#endif
}

const struct config *avcodec_get_hw_config(void *codec, int index) {
    if (!codec) return NULL;
#if PROBE_MODE == 2
    return index == 0 ? &non_drm_config : NULL;
#elif PROBE_MODE == 3
    return index == 0 ? &drm_internal_config : NULL;
#elif PROBE_MODE == 4
    (void)index;
    return &drm_internal_config;
#elif PROBE_MODE == 5
    return index == 0 ? &wrong_device_config : NULL;
#elif PROBE_MODE == 6
    return index == 0 ? &drm_device_config : NULL;
#else
    (void)index;
    return NULL;
#endif
}

int main(void) {
    mark("main\n");
    return fixture_dependency_anchor();
}
'''


DYNAMIC_AVCODEC_SOURCE = r'''
unsigned avcodec_version(void) { return (60U << 16) | 1U; }
int dynamic_avcodec_anchor(void) { return 0; }
'''


DYNAMIC_KODI_SOURCE = r'''
extern int dynamic_avcodec_anchor(void);
int main(void) { return dynamic_avcodec_anchor(); }
'''


BRIDGE_SOURCE = r'''
extern int dynamic_avcodec_anchor(void);
int bridge_anchor(void) { return dynamic_avcodec_anchor(); }
'''


INHERITED_PRELOAD_SOURCE = r'''
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void inherited_preload_constructor(void) {
    const char *active = getenv("CRYSTALHD_KODI_EMBEDDED_PROBE");
    const char *path = getenv("CRYSTALHD_FIXTURE_INHERITED_MARKER");
    const char value[] = "inherited-preload-ran\n";
    int fd;
    if (!active || strcmp(active, "v1") != 0 || !path) return;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    (void)write(fd, value, sizeof(value) - 1U);
    (void)close(fd);
}

int fixture_dependency_anchor(void) { return 0; }
'''


POLICY_21 = "kodi-21-omega-a3a448d"
POLICY_22 = "kodi-22-piers-28ea2eac"


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def write(path, contents):
    path.write_text(contents, encoding="utf-8")


def build_id(path):
    result = run(
        ["readelf", "-nW", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    match = re.search(r"Build ID:\s*([0-9a-fA-F]+)", result.stdout)
    if not match:
        raise AssertionError("fixture has no GNU Build ID")
    return match.group(1).lower()


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_dependency(root):
    write(root / "dependency.c", DEPENDENCY_SOURCE)
    run(
        [
            "cc", "-std=c11", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
            "-Wl,-soname,libfixture-dependency.so", "dependency.c",
            "-o", "libfixture-dependency.so",
        ],
        cwd=root,
    )


def build_embedded_fixture(
    root,
    mode,
    *,
    kodi_major=21,
    export_symbols=True,
    hide_kodi_version=False,
    hide_avcodec_version=False,
    with_preinit=False,
    avcodec_major=60,
    pie=True,
    indirect_dynamic_libav=False,
):
    root.mkdir()
    build_dependency(root)
    write(root / "kodi.c", KODI_SOURCE)
    extra_libraries = []
    if indirect_dynamic_libav:
        write(root / "avcodec.c", DYNAMIC_AVCODEC_SOURCE)
        write(root / "bridge.c", BRIDGE_SOURCE)
        run(
            [
                "cc", "-shared", "-fPIC", "-Wl,-soname,libavcodec.so.60",
                "avcodec.c", "-o", "libavcodec.so.60",
            ],
            cwd=root,
        )
        run(
            [
                "cc", "-shared", "-fPIC", "-Wl,-soname,libfixture-bridge.so",
                "bridge.c", "-Wl,-rpath,$ORIGIN", "-L.", "-l:libavcodec.so.60",
                "-o", "libfixture-bridge.so",
            ],
            cwd=root,
        )
        extra_libraries.append("-l:libfixture-bridge.so")
    command = [
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DPROBE_MODE=%d" % mode,
        "-DKODI_MAJOR=%d" % kodi_major,
        "-DKODI_MINOR=%d" % (3 if kodi_major == 21 else 0),
        "-DHIDE_KODI_VERSION=%d" % int(hide_kodi_version),
        "-DHIDE_AVCODEC_VERSION=%d" % int(hide_avcodec_version),
        "-DWITH_PREINIT=%d" % int(with_preinit),
        "-DAVCODEC_MAJOR=%d" % avcodec_major,
        "kodi.c", "-Wl,--no-as-needed", "-Wl,-rpath,$ORIGIN", "-L.",
        "-l:libfixture-dependency.so",
    ]
    command += extra_libraries + ["-o", "kodi.bin"]
    if pie:
        command[1:1] = ["-fPIE", "-pie"]
    else:
        command[1:1] = ["-fno-pie", "-no-pie"]
    if export_symbols:
        command.insert(-2, "-Wl,--export-dynamic")
    run(command, cwd=root)
    return root / "kodi.bin"


def build_dynamic_fixture(root):
    root.mkdir()
    write(root / "avcodec.c", DYNAMIC_AVCODEC_SOURCE)
    write(root / "kodi.c", DYNAMIC_KODI_SOURCE)
    run(
        ["cc", "-shared", "-fPIC", "-Wl,-soname,libavcodec.so.60", "avcodec.c", "-o", "libavcodec.so.60"],
        cwd=root,
    )
    run(
        ["cc", "kodi.c", "-Wl,-rpath,$ORIGIN", "-L.", "-l:libavcodec.so.60", "-o", "kodi.bin"],
        cwd=root,
    )
    return root / "kodi.bin"


def build_inherited_preload(root):
    root.mkdir()
    write(root / "inherited.c", INHERITED_PRELOAD_SOURCE)
    run(
        [
            "cc", "-std=c11", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
            "inherited.c", "-o", "inherited-preload.so",
        ],
        cwd=root,
    )
    run(
        [
            "cc", "-std=c11", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
            "-Wl,-soname,libfixture-dependency.so", "inherited.c",
            "-o", "libfixture-dependency.so",
        ],
        cwd=root,
    )
    return root / "inherited-preload.so"


def probe(
    script,
    helper,
    kodi,
    marker_root,
    *,
    policy=POLICY_21,
    expected_id=None,
    expected_sha=None,
    inherited_preload=None,
    bwrap_prefix=None,
    require=True,
):
    target_marker = marker_root / "target.marker"
    dependency_marker = marker_root / "dependency.marker"
    inherited_marker = marker_root / "inherited.marker"
    target_marker.unlink(missing_ok=True)
    dependency_marker.unlink(missing_ok=True)
    inherited_marker.unlink(missing_ok=True)
    environment = os.environ.copy()
    environment.pop("CRYSTALHD_KODI_EMBEDDED_PROBE", None)
    environment["CRYSTALHD_FIXTURE_TARGET_MARKER"] = str(target_marker)
    environment["CRYSTALHD_FIXTURE_DEPENDENCY_MARKER"] = str(dependency_marker)
    environment["CRYSTALHD_FIXTURE_INHERITED_MARKER"] = str(inherited_marker)
    if inherited_preload:
        environment["LD_PRELOAD"] = str(inherited_preload)
        environment["LD_LIBRARY_PATH"] = str(inherited_preload.parent)
    command = [sys.executable, str(script), "--kodi-binary", str(kodi), "--helper", str(helper)]
    if policy:
        command += [
            "--selection-policy", policy,
            "--expected-build-id", expected_id or build_id(kodi),
            "--expected-sha256", expected_sha or file_sha256(kodi),
        ]
    if bwrap_prefix:
        command += ["--bwrap-prefix-json", json.dumps(bwrap_prefix)]
    if require:
        command.append("--require-kodi-drm-prime")
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )
    target = target_marker.read_text(encoding="utf-8") if target_marker.exists() else ""
    dependency = dependency_marker.read_text(encoding="utf-8") if dependency_marker.exists() else ""
    result.inherited_marker = (
        inherited_marker.read_text(encoding="utf-8") if inherited_marker.exists() else ""
    )
    return result, target, dependency


def load_probe_module(script):
    specification = importlib.util.spec_from_file_location(
        "crystalhd_ffmpeg_embedded_hwconfig_probe_under_test", script
    )
    if specification is None or specification.loader is None:
        raise AssertionError("cannot load the embedded FFmpeg probe module")
    module = importlib.util.module_from_spec(specification)
    old_dont_write_bytecode = sys.dont_write_bytecode
    try:
        sys.dont_write_bytecode = True
        specification.loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = old_dont_write_bytecode
    return module


def probe_in_process(module, script, helper, kodi, marker_root):
    target_marker = marker_root / "target.marker"
    dependency_marker = marker_root / "dependency.marker"
    target_marker.unlink(missing_ok=True)
    dependency_marker.unlink(missing_ok=True)
    environment = {
        "CRYSTALHD_FIXTURE_TARGET_MARKER": str(target_marker),
        "CRYSTALHD_FIXTURE_DEPENDENCY_MARKER": str(dependency_marker),
    }
    command = [
        str(script),
        "--kodi-binary", str(kodi),
        "--helper", str(helper),
        "--selection-policy", POLICY_21,
        "--expected-build-id", build_id(kodi),
        "--expected-sha256", file_sha256(kodi),
        "--require-kodi-drm-prime",
    ]
    stdout = io.StringIO()
    stderr = io.StringIO()
    old_argv = sys.argv
    try:
        sys.argv = command
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            old_environment = {name: os.environ.get(name) for name in environment}
            try:
                os.environ.update(environment)
                returncode = module.main()
            finally:
                for name, value in old_environment.items():
                    if value is None:
                        os.environ.pop(name, None)
                    else:
                        os.environ[name] = value
    finally:
        sys.argv = old_argv
    result = subprocess.CompletedProcess(command, returncode, stdout.getvalue(), stderr.getvalue())
    target = target_marker.read_text(encoding="utf-8") if target_marker.exists() else ""
    dependency = dependency_marker.read_text(encoding="utf-8") if dependency_marker.exists() else ""
    return result, target, dependency


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def check_execution_boundary(target, dependency):
    check(target == "", "Kodi executable initializer or main ran during metadata probing")
    check(
        dependency in ("", "dependency-constructor\n"),
        "unexpected dependency side effect: %r" % dependency,
    )


def main():
    if len(sys.argv) != 3:
        print("usage: kodi-ffmpeg-embedded-hwconfig-probe.py PROBE HELPER", file=sys.stderr)
        return 2
    require_bwrap_value = os.environ.pop("CRYSTALHD_REQUIRE_BWRAP_TEST", "")
    check(
        require_bwrap_value in ("", "0", "1"),
        "CRYSTALHD_REQUIRE_BWRAP_TEST must be 0 or 1",
    )
    require_bwrap = require_bwrap_value == "1"
    bwrap = shutil.which("bwrap")
    check(
        bwrap is not None or not require_bwrap,
        "CRYSTALHD_REQUIRE_BWRAP_TEST=1 but bwrap is unavailable",
    )
    script = Path(sys.argv[1]).resolve()
    helper = Path(sys.argv[2]).resolve()
    repository = script.parents[2]
    receipt_path = (
        repository / "tests" / "fixtures" / "issue539" /
        "flathub-stable-embedded-hwconfig-20261028.json"
    )
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    check(
        receipt["probe"]["python_probe_sha256"] == file_sha256(script),
        "Flathub stable receipt does not identify the current Python probe",
    )
    check(
        receipt["probe"]["preload_source_sha256"] ==
        file_sha256(script.with_name("ffmpeg-embedded-hwconfig-preload.c")),
        "Flathub stable receipt does not identify the current preload source",
    )
    with tempfile.TemporaryDirectory(prefix="crystalhd-kodi-embedded-hwconfig-") as temporary:
        root = Path(temporary)
        fixtures = {}
        for name, mode in (
            ("missing-decoder", 0), ("zero-config", 1), ("non-drm", 2),
            ("drm-internal", 3), ("unterminated", 4),
            ("alternate-device", 5), ("drm-device", 6),
        ):
            directory = root / name
            fixtures[name] = build_embedded_fixture(directory, mode)

        result, target, dependency = probe(script, helper, fixtures["missing-decoder"], root / "missing-decoder")
        check(result.returncode == 1, result.stderr)
        document = json.loads(result.stdout)
        check(not document["decoder"]["present"], "missing decoder was reported present")
        check_execution_boundary(target, dependency)

        result, target, dependency = probe(script, helper, fixtures["zero-config"], root / "zero-config")
        check(result.returncode == 1, result.stderr)
        document = json.loads(result.stdout)
        check(document["decoder"]["present"], "zero-config decoder became missing")
        check(document["decoder"]["hardware_config_count"] == 0, "zero-config gained metadata")
        check_execution_boundary(target, dependency)

        result, target, dependency = probe(script, helper, fixtures["non-drm"], root / "non-drm")
        check(result.returncode == 1, result.stderr)
        check(not json.loads(result.stdout)["decoder"]["drm_prime_config_present"], "non-DRM metadata qualified")
        check_execution_boundary(target, dependency)

        result, target, dependency = probe(script, helper, fixtures["alternate-device"], root / "alternate-device")
        check(result.returncode == 1, "Kodi 21 must reject HW_DEVICE_CTX with a non-DRM device")
        document = json.loads(result.stdout)
        check(document["decoder"]["drm_prime_config_present"], "raw DRM PRIME metadata disappeared")
        check(not document["decoder"]["kodi_find_hw_config_candidate"], "Kodi 21 policy was not applied")
        check(document["selection_policy"]["name"] == POLICY_21, "selected policy was not recorded")
        check_execution_boundary(target, dependency)

        kodi22_dir = root / "alternate-device-kodi22"
        kodi22 = build_embedded_fixture(kodi22_dir, 5, kodi_major=22)
        result, target, dependency = probe(script, helper, kodi22, kodi22_dir, policy=POLICY_22)
        check(result.returncode == 0, result.stderr)
        check(json.loads(result.stdout)["decoder"]["kodi_find_hw_config_candidate"], "Kodi 22 policy was not applied")
        check_execution_boundary(target, dependency)

        result, target, dependency = probe(
            script, helper, fixtures["drm-internal"], root / "drm-internal",
            policy=POLICY_22, require=False,
        )
        check(result.returncode == 2, "Kodi major/policy mismatch must be unclassifiable")
        document = json.loads(result.stdout)
        check(document["decoder"]["kodi_find_hw_config_candidate"] is None, "mismatched policy produced a verdict")
        check(document["selection_policy"]["status"] == "version-cross-check-failed", "mismatch status was lost")
        check_execution_boundary(target, dependency)

        for name in ("drm-internal", "drm-device"):
            result, target, dependency = probe(script, helper, fixtures[name], root / name)
            check(result.returncode == 0, result.stderr)
            document = json.loads(result.stdout)
            check(document["decoder"]["kodi_find_hw_config_candidate"], "%s was rejected" % name)
            check(document["libraries"]["avcodec"]["version"]["string"] == "60.31.102", "version mismatch")
            check(document["kodi"]["ffmpeg_linkage"] == "embedded-static", "linkage mismatch")
            check(document["process_output"]["stdout_bytes_before_exit"] > 0, "dependency stdout corrupted the protocol")
            check(document["process_output"]["stderr_bytes_before_exit"] > 0, "dependency stderr corrupted the protocol")
            check(not document["execution_boundary"]["kodi_main_ran"], "probe claims Kodi main ran")
            check(document["execution_boundary"]["dependency_initializers_may_run"], "dependency boundary omitted")
            check_execution_boundary(target, dependency)

        nonpie_dir = root / "nonpie"
        nonpie = build_embedded_fixture(nonpie_dir, 3, pie=False)
        result, target, dependency = probe(script, helper, nonpie, nonpie_dir)
        check(result.returncode == 0, result.stderr)
        check_execution_boundary(target, dependency)

        result, target, dependency = probe(
            script, helper, fixtures["zero-config"], root / "zero-config", require=False
        )
        check(result.returncode == 0, "completed negative inventory must exit zero by default")
        check(json.loads(result.stdout)["probe_completed"], "negative inventory was incomplete")
        check_execution_boundary(target, dependency)

        result, target, dependency = probe(
            script, helper, fixtures["unterminated"], root / "unterminated", require=False
        )
        check(result.returncode == 2, "unterminated metadata must be rejected")
        check("no terminator within 64 entries" in result.stderr, result.stderr)
        check_execution_boundary(target, dependency)

        raw_result, target, dependency = probe(
            script, helper, fixtures["drm-internal"], root / "drm-internal", policy=None, require=False
        )
        check(raw_result.returncode == 2, "unattested generic ELF must not receive a Kodi policy verdict")
        raw_document = json.loads(raw_result.stdout)
        check(raw_document["decoder"]["kodi_find_hw_config_candidate"] is None, "raw probe inferred a Kodi policy")
        check(raw_document["selection_policy"]["status"] == "required", "missing policy was not explicit")
        check_execution_boundary(target, dependency)

        hidden_dir = root / "hidden"
        hidden = build_embedded_fixture(hidden_dir, 3, export_symbols=False)
        result, target, dependency = probe(script, helper, hidden, hidden_dir, policy=None, require=False)
        check(result.returncode == 2, "hidden FFmpeg symbols must be unclassifiable")
        check("dynamic export" in result.stderr, result.stderr)
        check(target == "" and dependency == "", "preflight failure executed the selected binary")

        hidden_version_dir = root / "hidden-version"
        hidden_version = build_embedded_fixture(hidden_version_dir, 3, hide_kodi_version=True)
        result, target, dependency = probe(script, helper, hidden_version, hidden_version_dir, require=False)
        check(result.returncode == 2, "attested policy needs a main-origin Kodi version cross-check")
        check("CCompileInfo" in result.stderr, result.stderr)
        check_execution_boundary(target, dependency)

        hidden_abi_dir = root / "hidden-avcodec-version"
        hidden_abi = build_embedded_fixture(hidden_abi_dir, 3, hide_avcodec_version=True)
        result, target, dependency = probe(script, helper, hidden_abi, hidden_abi_dir, require=False)
        check(result.returncode == 2, "unknown libavcodec ABI must fail before config struct access")
        check("known package identity" in result.stderr, result.stderr)
        check_execution_boundary(target, dependency)

        probe_module = load_probe_module(script)
        hidden_metadata = probe_module.elf_metadata(hidden_abi)
        hidden_build_id = hidden_metadata["build_id"]
        hidden_sha256 = hidden_metadata["sha256"]
        known_packages = probe_module.POLICIES[POLICY_21].setdefault("known_packages", {})
        previous_package = known_packages.get(hidden_build_id)
        synthetic_package = {
            "name": "synthetic hidden-avcodec-version fixture",
            "sha256": "0" * 64,
            "ffmpeg_version": "synthetic",
            "avcodec_major": 60,
            "ref_commit": "synthetic",
            "manifest_commit": "synthetic",
        }
        try:
            known_packages[hidden_build_id] = synthetic_package
            check(
                probe_module.matching_known_package(hidden_metadata, POLICY_21) is None,
                "known-package matching accepted a Build ID with the wrong SHA-256",
            )
            result, target, dependency = probe_in_process(
                probe_module, script, helper, hidden_abi, hidden_abi_dir
            )
            check(result.returncode == 2, "Build ID alone supplied the hidden libavcodec ABI")
            check("known package identity" in result.stderr, result.stderr)
            check_execution_boundary(target, dependency)
            synthetic_package["sha256"] = hidden_sha256
            result, target, dependency = probe_in_process(
                probe_module, script, helper, hidden_abi, hidden_abi_dir
            )
            check(result.returncode == 0, result.stderr)
            document = json.loads(result.stdout)
            check(
                document["ffmpeg_abi"]["avcodec"] == {
                    "major": 60,
                    "source": "known_package_identity",
                },
                "exact known-package identity did not supply the hidden libavcodec ABI",
            )
            check(
                document["selection_policy"]["known_package_identity"]["name"] ==
                synthetic_package["name"],
                "exact known-package identity was not recorded",
            )
            check(
                document["decoder"]["kodi_find_hw_config_candidate"],
                "known-package ABI fallback lost the DRM PRIME candidate",
            )
            check_execution_boundary(target, dependency)
        finally:
            if previous_package is None:
                known_packages.pop(hidden_build_id, None)
            else:
                known_packages[hidden_build_id] = previous_package

        preinit_dir = root / "preinit"
        preinit = build_embedded_fixture(preinit_dir, 3, with_preinit=True)
        result, target, dependency = probe(script, helper, preinit, preinit_dir, require=False)
        check(result.returncode == 2, "target PREINIT must be rejected before execution")
        check("PREINIT_ARRAY" in result.stderr, result.stderr)
        check(target == "" and dependency == "", "PREINIT rejection executed the target")

        bad_abi_dir = root / "bad-abi"
        bad_abi = build_embedded_fixture(bad_abi_dir, 3, avcodec_major=99)
        result, target, dependency = probe(script, helper, bad_abi, bad_abi_dir, require=False)
        check(result.returncode == 2, "unsupported libavcodec ABI must fail before struct access")
        check("unsupported libavcodec ABI major" in result.stderr, result.stderr)
        check_execution_boundary(target, dependency)

        dynamic_dir = root / "dynamic"
        dynamic = build_dynamic_fixture(dynamic_dir)
        result, target, dependency = probe(script, helper, dynamic, dynamic_dir, policy=None, require=False)
        check(result.returncode == 2, "a DSO-linked Kodi binary must use the existing probe")
        check("directly links libavcodec.so" in result.stderr, result.stderr)
        check(target == "" and dependency == "", "linkage preflight failure executed the binary")

        indirect_dir = root / "indirect-dynamic"
        indirect = build_embedded_fixture(indirect_dir, 3, indirect_dynamic_libav=True)
        result, target, dependency = probe(script, helper, indirect, indirect_dir, require=False)
        check(result.returncode == 2, "an indirectly loaded core FFmpeg DSO must be rejected")
        check("dynamic core FFmpeg DSO loaded" in result.stderr, result.stderr)
        check_execution_boundary(target, dependency)

        mismatch_id = "0" * len(build_id(fixtures["drm-internal"]))
        result, target, dependency = probe(
            script, helper, fixtures["drm-internal"], root / "drm-internal",
            expected_id=mismatch_id, require=False,
        )
        check(result.returncode == 2, "Build ID mismatch must fail before execution")
        check("Build ID" in result.stderr, result.stderr)
        check(target == "" and dependency == "", "Build ID mismatch executed the target")

        mismatch_sha = "0" * 64
        result, target, dependency = probe(
            script, helper, fixtures["drm-internal"], root / "drm-internal",
            expected_sha=mismatch_sha, require=False,
        )
        check(result.returncode == 2, "SHA-256 mismatch must fail before execution")
        check("SHA-256" in result.stderr, result.stderr)
        check(target == "" and dependency == "", "SHA-256 mismatch executed the target")

        setuid_dir = root / "setuid"
        setuid_dir.mkdir()
        setuid = setuid_dir / "kodi.bin"
        shutil.copy2(fixtures["drm-internal"], setuid)
        setuid.chmod(0o4755)
        result, target, dependency = probe(script, helper, setuid, setuid_dir, policy=None, require=False)
        check(result.returncode == 2, "setuid target must be rejected before LD_PRELOAD")
        check("set-user-ID or set-group-ID" in result.stderr, result.stderr)
        check(target == "" and dependency == "", "setuid preflight failure executed the binary")

        wrapper_dir = root / "wrapper"
        wrapper_dir.mkdir()
        wrapper = wrapper_dir / "kodi-wrapper"
        write(wrapper, "#!/bin/sh\nexit 0\n")
        wrapper.chmod(0o755)
        result, target, dependency = probe(script, helper, wrapper, wrapper_dir, policy=None, require=False)
        check(result.returncode == 2, "Kodi wrapper must not be accepted as an ELF")
        check(target == "" and dependency == "", "wrapper preflight executed the selected file")

        inherited_dir = root / "inherited-preload"
        inherited_preload = build_inherited_preload(inherited_dir)
        result, target, dependency = probe(
            script, helper, fixtures["drm-internal"], root / "drm-internal",
            inherited_preload=inherited_preload,
        )
        check(result.returncode == 0, result.stderr)
        check(result.inherited_marker == "", "runner passed an inherited loader override into Kodi")
        check_execution_boundary(target, dependency)

        if bwrap:
            result, target, dependency = probe(
                script, helper, fixtures["drm-internal"], root / "drm-internal",
                bwrap_prefix=[bwrap, "--ro-bind", "/", "/"],
            )
            check(result.returncode == 0, result.stderr)
            document = json.loads(result.stdout)
            check(document["loader_environment"]["execution_mode"] == "bwrap", "bwrap mode was not recorded")
            check_execution_boundary(target, dependency)

        environment = os.environ.copy()
        environment["LD_PRELOAD"] = str(helper)
        result = subprocess.run(["/bin/true"], env=environment)
        check(result.returncode == 0, "inactive preload helper did not transparently forward startup")

    print("Kodi embedded FFmpeg hardware-config probe: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
