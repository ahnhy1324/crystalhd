#!/usr/bin/env python3
"""Inspect Kodi-linked FFmpeg hardware metadata without opening a device."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys


LIBRARY_KINDS = ("avcodec", "avformat", "avutil")
METHOD_NAMES = (
    (0x01, "hw_device_ctx"),
    (0x02, "hw_frames_ctx"),
    (0x04, "internal"),
    (0x08, "ad_hoc"),
)
REEXEC_MARKER = "CRYSTALHD_KODI_FFMPEG_PROBE_REEXEC"


class AVCodecHWConfig(ctypes.Structure):
    _fields_ = (
        ("pix_fmt", ctypes.c_int),
        ("methods", ctypes.c_int),
        ("device_type", ctypes.c_int),
    )


def fail(message):
    print("ffmpeg-hwconfig-probe: " + message, file=sys.stderr)
    return 2


def run_readelf(arguments, path):
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    try:
        result = subprocess.run(
            ["readelf"] + arguments + [str(path)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("readelf is required") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.strip() or "readelf rejected the selected file"
        raise RuntimeError(detail) from exc
    return result.stdout


def elf_metadata(path):
    header = run_readelf(["-hW"], path)
    dynamic = run_readelf(["-dW"], path)
    elf_class = None
    machine = None
    for line in header.splitlines():
        match = re.match(r"\s*Class:\s*(\S+)", line)
        if match:
            elf_class = match.group(1)
        match = re.match(r"\s*Machine:\s*(.+?)\s*$", line)
        if match:
            machine = match.group(1)
    if elf_class not in ("ELF32", "ELF64") or not machine:
        raise RuntimeError("cannot identify the selected Kodi ELF class and machine")

    needed = []
    for line in dynamic.splitlines():
        match = re.search(r"\(NEEDED\).*Shared library: \[([^]]+)\]", line)
        if match:
            needed.append(match.group(1))
    return {"class": elf_class, "machine": machine, "needed": needed}


def require_compatible_interpreter(metadata):
    pointer_class = "ELF64" if struct.calcsize("P") == 8 else "ELF32"
    interpreter_machine = platform.machine()
    if metadata["class"] != pointer_class:
        raise RuntimeError(
            "Kodi is %s but this Python interpreter is %s; run the probe with a matching architecture"
            % (metadata["class"], pointer_class)
        )
    machine = metadata["machine"].lower()
    interpreter = interpreter_machine.lower()
    compatible = (
        (interpreter in ("x86_64", "amd64") and "x86-64" in machine)
        or (interpreter in ("i386", "i486", "i586", "i686") and "80386" in machine)
        or (interpreter in ("aarch64", "arm64") and "aarch64" in machine)
        or (interpreter.startswith("arm") and machine.startswith("arm"))
    )
    if not compatible:
        raise RuntimeError(
            "Kodi machine '%s' does not match Python machine '%s'; run inside the target runtime"
            % (metadata["machine"], interpreter_machine)
        )
    return {"class": pointer_class, "machine": interpreter_machine}


def selected_ffmpeg_sonames(needed):
    selected = {}
    for kind in LIBRARY_KINDS:
        matches = [name for name in needed if re.fullmatch(r"lib" + kind + r"\.so\.\d+", name)]
        if len(matches) != 1:
            raise RuntimeError(
                "selected Kodi ELF must directly need exactly one lib%s.so.N (found %s)"
                % (kind, matches)
            )
        selected[kind] = matches[0]
    return selected


def library_paths(library_dir, sonames):
    paths = {}
    for kind, soname in sonames.items():
        candidate = library_dir / soname
        if not candidate.is_file():
            raise RuntimeError("%s is missing from --library-dir" % candidate)
        resolved = candidate.resolve(strict=True)
        dynamic = run_readelf(["-dW"], resolved)
        declared = re.findall(r"\(SONAME\).*Library soname: \[([^]]+)\]", dynamic)
        if declared != [soname]:
            raise RuntimeError(
                "%s must declare SONAME %s (found %s)" % (resolved, soname, declared)
            )
        paths[kind] = resolved
    return paths


def reexec_with_library_dir(library_dir):
    marker = str(library_dir)
    if os.environ.get(REEXEC_MARKER) == marker:
        return
    environment = os.environ.copy()
    old_path = environment.get("LD_LIBRARY_PATH")
    environment["LD_LIBRARY_PATH"] = marker + ((":" + old_path) if old_path else "")
    environment[REEXEC_MARKER] = marker
    argv = [sys.executable, str(Path(__file__).resolve())] + sys.argv[1:]
    os.execve(sys.executable, argv, environment)


def mapped_paths(kind):
    paths = set()
    try:
        lines = Path("/proc/self/maps").read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise RuntimeError("cannot verify loaded libraries through /proc/self/maps: %s" % exc) from exc
    for line in lines:
        fields = line.split(None, 5)
        if len(fields) != 6 or not fields[5].startswith("/"):
            continue
        value = fields[5].removesuffix(" (deleted)")
        if re.fullmatch(r"lib" + kind + r"\.so(?:\.\d+)+", Path(value).name):
            paths.add(str(Path(value).resolve()))
    return sorted(paths)


def load_libraries(paths, sonames):
    loaded = {}
    # A fresh process and explicit dependency-first loads keep this probe's
    # selected ABI set separate from an unrelated host FFmpeg installation.
    for kind in ("avutil", "avcodec", "avformat"):
        try:
            loaded[kind] = ctypes.CDLL(str(paths[kind]), mode=ctypes.RTLD_GLOBAL)
        except OSError as exc:
            raise RuntimeError("cannot load %s: %s" % (paths[kind], exc)) from exc
    for kind in LIBRARY_KINDS:
        actual = mapped_paths(kind)
        expected = str(paths[kind])
        if actual != [expected]:
            raise RuntimeError(
                "%s resolved outside the exact selected set (expected %s, mapped %s)"
                % (sonames[kind], expected, actual)
            )
    return loaded


def version_value(library, symbol):
    try:
        function = getattr(library, symbol)
    except AttributeError as exc:
        raise RuntimeError("required symbol %s is missing" % symbol) from exc
    function.argtypes = []
    function.restype = ctypes.c_uint
    value = int(function())
    return {
        "integer": value,
        "string": "%d.%d.%d" % ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF),
    }


def decode_text(value):
    return value.decode("utf-8", "replace") if value else None


def inspect_codec(libraries, decoder_name):
    avcodec = libraries["avcodec"]
    avutil = libraries["avutil"]
    try:
        find_decoder = avcodec.avcodec_find_decoder_by_name
        get_config = avcodec.avcodec_get_hw_config
        pixel_name = avutil.av_get_pix_fmt_name
        device_name = avutil.av_hwdevice_get_type_name
    except AttributeError as exc:
        raise RuntimeError("required FFmpeg inspection symbol is missing: %s" % exc) from exc

    find_decoder.argtypes = [ctypes.c_char_p]
    find_decoder.restype = ctypes.c_void_p
    get_config.argtypes = [ctypes.c_void_p, ctypes.c_int]
    get_config.restype = ctypes.POINTER(AVCodecHWConfig)
    pixel_name.argtypes = [ctypes.c_int]
    pixel_name.restype = ctypes.c_char_p
    device_name.argtypes = [ctypes.c_int]
    device_name.restype = ctypes.c_char_p

    codec = find_decoder(decoder_name.encode("ascii"))
    configs = []
    if codec:
        for index in range(65):
            pointer = get_config(codec, index)
            if not pointer:
                break
            if index == 64:
                raise RuntimeError("hardware configuration list has no terminator within 64 entries")
            config = pointer.contents
            known_mask = sum(bit for bit, _ in METHOD_NAMES)
            configs.append(
                {
                    "index": index,
                    "pixel_format": {
                        "number": int(config.pix_fmt),
                        "name": decode_text(pixel_name(config.pix_fmt)),
                    },
                    "methods": {
                        "mask": int(config.methods),
                        "names": [name for bit, name in METHOD_NAMES if config.methods & bit],
                        "unknown_mask": int(config.methods) & ~known_mask,
                    },
                    "device_type": {
                        "number": int(config.device_type),
                        "name": decode_text(device_name(config.device_type)),
                    },
                }
            )

    drm_configs = [item for item in configs if item["pixel_format"]["name"] == "drm_prime"]
    # Kodi 22's FindHWConfig accepts DRM_PRIME with HW_DEVICE_CTX or INTERNAL.
    # The actual device/context creation remains a later runtime gate.
    kodi_configs = [item for item in drm_configs if item["methods"]["mask"] & (0x01 | 0x04)]
    return {
        "name": decoder_name,
        "present": bool(codec),
        "hardware_config_count": len(configs),
        "hardware_configs": configs,
        "drm_prime_config_present": bool(drm_configs),
        "kodi_find_hw_config_candidate": bool(kodi_configs),
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Inspect Kodi-linked FFmpeg V4L2/DRM PRIME metadata without opening hardware"
    )
    parser.add_argument("--kodi-binary", required=True, type=Path, help="actual Kodi ELF, not a launcher")
    parser.add_argument(
        "--library-dir",
        required=True,
        type=Path,
        help="directory containing the exact linked libavcodec/libavformat/libavutil SONAME files",
    )
    parser.add_argument("--decoder", default="h264_v4l2m2m")
    parser.add_argument("--pretty", action="store_true", help="indent JSON output")
    parser.add_argument(
        "--require-kodi-drm-prime",
        action="store_true",
        help="return status 1 when no config passes Kodi 22 FindHWConfig metadata selection",
    )
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    kodi_binary = arguments.kodi_binary.resolve()
    library_dir = arguments.library_dir.resolve()
    if not kodi_binary.is_file():
        return fail("--kodi-binary is not a file: %s" % kodi_binary)
    if not library_dir.is_dir():
        return fail("--library-dir is not a directory: %s" % library_dir)

    try:
        metadata = elf_metadata(kodi_binary)
        interpreter = require_compatible_interpreter(metadata)
        sonames = selected_ffmpeg_sonames(metadata["needed"])
        paths = library_paths(library_dir, sonames)
        reexec_with_library_dir(library_dir)
        libraries = load_libraries(paths, sonames)
        decoder = inspect_codec(libraries, arguments.decoder)
        document = {
            "schema": "crystalhd-kodi-ffmpeg-hwconfig-v1",
            "probe_completed": True,
            "probe_scope": "metadata-only; no Kodi, media, V4L2 or CrystalHD device opened",
            "kodi": {
                "path": str(kodi_binary),
                "elf_class": metadata["class"],
                "machine": metadata["machine"],
                "needed": metadata["needed"],
            },
            "interpreter": interpreter,
            "linked_sonames": sonames,
            "libraries": {
                "avcodec": {
                    "path": str(paths["avcodec"]),
                    "version": version_value(libraries["avcodec"], "avcodec_version"),
                },
                "avformat": {
                    "path": str(paths["avformat"]),
                    "version": version_value(libraries["avformat"], "avformat_version"),
                },
                "avutil": {
                    "path": str(paths["avutil"]),
                    "version": version_value(libraries["avutil"], "avutil_version"),
                },
            },
            "decoder": decoder,
            "verdict": (
                "kodi-drm-prime-metadata-candidate"
                if decoder["kodi_find_hw_config_candidate"]
                else "no-kodi-drm-prime-metadata-candidate"
            ),
            "limitations": [
                "positive metadata is necessary but does not prove Kodi selection or playback",
                "device access, EXPBUF/import and buffer lifetime require separate runtime evidence",
            ],
        }
    except (AttributeError, RuntimeError) as exc:
        return fail(str(exc))

    json.dump(document, sys.stdout, indent=2 if arguments.pretty else None, sort_keys=True)
    sys.stdout.write("\n")
    if arguments.require_kodi_drm_prime and not decoder["kodi_find_hw_config_candidate"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
