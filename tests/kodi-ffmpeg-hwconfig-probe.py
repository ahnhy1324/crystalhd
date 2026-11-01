#!/usr/bin/env python3
"""Device-free tests for the Kodi FFmpeg hardware-configuration probe."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


AVUTIL_SOURCE = r'''
#include <stddef.h>
unsigned avutil_version(void) { return (60U << 16) | (1U << 8) | 2U; }
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
'''

AVFORMAT_SOURCE = r'''
unsigned avformat_version(void) { return (62U << 16) | (3U << 8) | 4U; }
'''

AVCODEC_SOURCE = r'''
#include <stddef.h>
#include <string.h>
struct config { int pix_fmt; int methods; int device_type; };
static const struct config drm_config = { 123, 4, 2 };
static const struct config non_drm_config = { 124, 4, 2 };
static const struct config wrong_device_config = { 123, 1, 3 };
unsigned avcodec_version(void) { return (62U << 16) | (5U << 8) | 6U; }
void *avcodec_find_decoder_by_name(const char *name) {
#if PROBE_MODE == 0
    (void)name; return NULL;
#else
    return name && strcmp(name, "h264_v4l2m2m") == 0 ? (void *)0x1 : NULL;
#endif
}
const struct config *avcodec_get_hw_config(void *codec, int index) {
    if (!codec) return NULL;
#if PROBE_MODE == 2
    return index == 0 ? &non_drm_config : NULL;
#elif PROBE_MODE == 3
    return index == 0 ? &drm_config : NULL;
#elif PROBE_MODE == 4
    (void)index; return &drm_config;
#elif PROBE_MODE == 5
    return index == 0 ? &wrong_device_config : NULL;
#else
    (void)index; return NULL;
#endif
}
'''

KODI_SOURCE = r'''
extern unsigned avcodec_version(void);
extern unsigned avformat_version(void);
extern unsigned avutil_version(void);
int main(void) {
    return (int)(avcodec_version() + avformat_version() + avutil_version());
}
'''


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def write(path, contents):
    path.write_text(contents, encoding="utf-8")


def build_fixture(root, mode):
    root.mkdir()
    write(root / "avutil.c", AVUTIL_SOURCE)
    write(root / "avformat.c", AVFORMAT_SOURCE)
    write(root / "avcodec.c", AVCODEC_SOURCE)
    write(root / "kodi.c", KODI_SOURCE)
    run(["cc", "-shared", "-fPIC", "-Wl,-soname,libavutil.so.60", "avutil.c", "-o", "libavutil.so.60"], cwd=root)
    run(["cc", "-shared", "-fPIC", "-Wl,-soname,libavformat.so.62", "avformat.c", "-o", "libavformat.so.62"], cwd=root)
    run(
        [
            "cc",
            "-shared",
            "-fPIC",
            "-Wl,-soname,libavcodec.so.62",
            "-DPROBE_MODE=%d" % mode,
            "avcodec.c",
            "-o",
            "libavcodec.so.62",
        ],
        cwd=root,
    )
    run(
        [
            "cc",
            "kodi.c",
            "-Wl,--no-as-needed",
            "-L.",
            "-l:libavcodec.so.62",
            "-l:libavformat.so.62",
            "-l:libavutil.so.60",
            "-o",
            "kodi.bin",
        ],
        cwd=root,
    )
    return root / "kodi.bin"


def probe(script, kodi, library_dir, require=True):
    command = [
        sys.executable,
        str(script),
        "--kodi-binary",
        str(kodi),
        "--library-dir",
        str(library_dir),
    ]
    if require:
        command.append("--require-kodi-drm-prime")
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 2:
        print("usage: kodi-ffmpeg-hwconfig-probe.py PROBE", file=sys.stderr)
        return 2
    script = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="crystalhd-kodi-hwconfig-") as temporary:
        root = Path(temporary)
        fixtures = {}
        for name, mode in (
            ("missing-decoder", 0),
            ("zero-config", 1),
            ("non-drm", 2),
            ("drm-prime", 3),
            ("unterminated", 4),
            ("alternate-device", 5),
        ):
            directory = root / name
            fixtures[name] = (build_fixture(directory, mode), directory)

        result = probe(script, *fixtures["missing-decoder"])
        check(result.returncode == 1, "missing decoder must be a completed negative finding")
        document = json.loads(result.stdout)
        check(not document["decoder"]["present"], "missing decoder was reported present")

        result = probe(script, *fixtures["zero-config"])
        check(result.returncode == 1, "zero-config decoder must fail the optional requirement")
        document = json.loads(result.stdout)
        check(document["decoder"]["present"], "zero-config decoder must remain distinct from missing")
        check(document["decoder"]["hardware_config_count"] == 0, "zero-config fixture gained a config")

        result = probe(script, *fixtures["non-drm"])
        check(result.returncode == 1, "non-DRM configuration must not qualify")
        document = json.loads(result.stdout)
        check(document["decoder"]["hardware_config_count"] == 1, "non-DRM config was not enumerated")
        check(not document["decoder"]["drm_prime_config_present"], "non-DRM config became DRM PRIME")

        result = probe(script, *fixtures["alternate-device"])
        check(result.returncode == 0, "Kodi FindHWConfig does not reject another HW device type")
        document = json.loads(result.stdout)
        check(document["decoder"]["drm_prime_config_present"], "alternate-device DRM pixel format disappeared")
        check(document["decoder"]["kodi_find_hw_config_candidate"], "Kodi method candidate was rejected")
        check(document["decoder"]["hardware_configs"][0]["device_type"]["name"] == "vaapi", "device type was lost")

        result = probe(script, *fixtures["zero-config"], require=False)
        check(result.returncode == 0, "completed negative inventory must exit zero by default")
        check(json.loads(result.stdout)["probe_completed"], "negative inventory was not completed")

        result = probe(script, *fixtures["drm-prime"])
        check(result.returncode == 0, result.stderr)
        document = json.loads(result.stdout)
        check(document["decoder"]["kodi_find_hw_config_candidate"], "DRM PRIME fixture was rejected")
        config = document["decoder"]["hardware_configs"][0]
        check(config["pixel_format"]["name"] == "drm_prime", "pixel format name mismatch")
        check(config["methods"]["mask"] == 4 and config["methods"]["names"] == ["internal"], "method mismatch")
        check(config["device_type"]["name"] == "drm", "device type mismatch")
        check(document["linked_sonames"]["avcodec"] == "libavcodec.so.62", "SONAME mismatch")
        expected = str((fixtures["drm-prime"][1] / "libavcodec.so.62").resolve())
        check(document["libraries"]["avcodec"]["path"] == expected, "wrong library selected")
        check(document["libraries"]["avcodec"]["version"]["string"] == "62.5.6", "version mismatch")

        result = probe(script, *fixtures["unterminated"], require=False)
        check(result.returncode == 2, "unterminated config list must be rejected")
        check("no terminator within 64 entries" in result.stderr, "enumeration-bound error was not specific")

        incomplete = root / "incomplete"
        shutil.copytree(fixtures["drm-prime"][1], incomplete)
        (incomplete / "libavformat.so.62").unlink()
        result = probe(script, fixtures["drm-prime"][0], incomplete, require=False)
        check(result.returncode == 2, "incomplete exact set must be rejected")
        check("missing from --library-dir" in result.stderr, "missing-library error was not specific")

        wrong_soname = root / "wrong-soname"
        shutil.copytree(fixtures["drm-prime"][1], wrong_soname)
        run(
            [
                "cc",
                "-shared",
                "-fPIC",
                "-Wl,-soname,libavcodec.so.61",
                "-DPROBE_MODE=3",
                "avcodec.c",
                "-o",
                "libavcodec.so.62",
            ],
            cwd=wrong_soname,
        )
        result = probe(script, fixtures["drm-prime"][0], wrong_soname, require=False)
        check(result.returncode == 2, "wrong internal SONAME must be rejected")
        check("must declare SONAME libavcodec.so.62" in result.stderr, "SONAME error was not specific")

        wrapper = root / "kodi-wrapper"
        write(wrapper, "#!/bin/sh\nexit 0\n")
        result = probe(script, wrapper, fixtures["drm-prime"][1], require=False)
        check(result.returncode == 2, "Kodi wrapper must not be accepted as an ELF")

    print("Kodi FFmpeg hardware-config probe: 10 groups passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
