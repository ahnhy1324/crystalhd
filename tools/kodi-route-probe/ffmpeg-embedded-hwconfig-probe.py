#!/usr/bin/env python3
"""Inspect FFmpeg metadata embedded in a trusted Kodi executable."""

import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import stat
import struct
import subprocess
import sys
import tempfile


ACTIVATION_ENV = "CRYSTALHD_KODI_EMBEDDED_PROBE"
FD_ENV = "CRYSTALHD_KODI_EMBEDDED_PROBE_FD"
DECODER_ENV = "CRYSTALHD_KODI_EMBEDDED_PROBE_DECODER"
PROTOCOL_MAGIC = "crystalhd-kodi-embedded-ffmpeg-hwconfig\t1"
CORE_FFMPEG_SONAME = re.compile(r"lib(?:avcodec|avformat|avutil)\.so(?:\.\d+)*$")
SAFE_NAME = re.compile(r"[A-Za-z0-9_.-]+$")
METHOD_NAMES = (
    (0x01, "hw_device_ctx"),
    (0x02, "hw_frames_ctx"),
    (0x04, "internal"),
    (0x08, "ad_hoc"),
)
REQUIRED_EXPORTS = (
    "avcodec_find_decoder_by_name",
    "avcodec_get_hw_config",
    "av_get_pix_fmt_name",
    "av_hwdevice_get_type_name",
)
POLICIES = {
    "kodi-21-omega-a3a448d": {
        "expected_major": 21,
        "source_commit": "a3a448d26b8d560a65655dab2cd122994dc4e146",
        "source_path": "xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecDRMPRIME.cpp",
        "source_lines": "163-180",
        "rule": "drm_prime and (internal or (hw_device_ctx and device_type drm))",
        "known_packages": {
            "0b11c4c2fdbf023b13d036feba99ee02980da070": {
                "name": "Flathub stable Kodi 21.3 x86_64",
                "sha256": "6754d32f8b6b6501b247a7fc817c2b6eb151c27ca5380abd4f2115163ed0a1d6",
                "ffmpeg_version": "6.1.6",
                "avcodec_major": 60,
                "ref_commit": "9d26e51812802b2d687557c6bb28b9573936f3bfe6c2b181229341e53fa2c475",
                "manifest_commit": "30f8c953d763802968b5e1b1fa7c84a9453308f2",
            }
        },
    },
    "kodi-22-piers-28ea2eac": {
        "expected_major": 22,
        "source_commit": "28ea2eac1eb7af8fdcbd2672d933ce87f594be79",
        "source_path": "xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecDRMPRIME.cpp",
        "source_lines": "164-178",
        "rule": "drm_prime and (internal or hw_device_ctx)",
    },
}
HELPER_ERROR_MESSAGES = {
    "dynamic_libav_loaded": "dynamic core FFmpeg DSO loaded: {detail}",
    "missing_avcodec_abi": "libavcodec ABI needs a version export or known package identity",
    "unterminated_config": "hardware configuration list has no terminator within 64 entries",
    "unsupported_avcodec_abi": "unsupported libavcodec ABI major {detail}",
}


def fail(message):
    print("ffmpeg-embedded-hwconfig-probe: " + message, file=sys.stderr)
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


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def dynamic_symbols(path):
    output = run_readelf(["--dyn-syms", "-W"], path)
    symbols = {}
    pattern = re.compile(
        r"\s*\d+:\s+[0-9a-fA-F]+\s+\d+\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.+?)\s*$"
    )
    for line in output.splitlines():
        match = pattern.fullmatch(line)
        if not match:
            continue
        symbol_type, binding, visibility, index, decorated_name = match.groups()
        name = decorated_name.split()[0].split("@", 1)[0]
        symbols.setdefault(name, []).append(
            {
                "type": symbol_type,
                "binding": binding,
                "visibility": visibility,
                "index": index,
            }
        )
    return symbols


def defined_export(symbols, name):
    return any(
        item["type"] == "FUNC"
        and item["binding"] in ("GLOBAL", "WEAK")
        and item["visibility"] == "DEFAULT"
        and item["index"] != "UND"
        for item in symbols.get(name, ())
    )


def elf_metadata(path):
    header = run_readelf(["-hW"], path)
    dynamic = run_readelf(["-dW"], path)
    program = run_readelf(["-lW"], path)
    notes = run_readelf(["-nW"], path)
    elf_class = None
    machine = None
    elf_type = None
    for line in header.splitlines():
        match = re.match(r"\s*Class:\s*(\S+)", line)
        if match:
            elf_class = match.group(1)
        match = re.match(r"\s*Machine:\s*(.+?)\s*$", line)
        if match:
            machine = match.group(1)
        match = re.match(r"\s*Type:\s*(\S+)", line)
        if match:
            elf_type = match.group(1)
    if elf_class not in ("ELF32", "ELF64") or not machine or elf_type not in ("DYN", "EXEC"):
        raise RuntimeError("cannot identify a supported dynamic ELF class, machine and type")

    needed = []
    preinit = False
    for line in dynamic.splitlines():
        match = re.search(r"\(NEEDED\).*Shared library: \[([^]]+)\]", line)
        if match:
            needed.append(match.group(1))
        if re.search(r"\(PREINIT_ARRAY(?:SZ)?\)", line):
            preinit = True

    interpreter_match = re.search(r"Requesting program interpreter:\s*([^]]+)\]", program)
    interpreter = interpreter_match.group(1) if interpreter_match else None
    build_ids = re.findall(r"Build ID:\s*([0-9a-fA-F]+)", notes)
    if len(build_ids) != 1:
        raise RuntimeError("selected ELF must contain exactly one GNU Build ID")
    return {
        "class": elf_class,
        "machine": machine,
        "type": elf_type,
        "needed": needed,
        "preinit": preinit,
        "interpreter": interpreter,
        "build_id": build_ids[0].lower(),
        "sha256": file_sha256(path),
        "symbols": dynamic_symbols(path),
    }


def compatible_machine(metadata):
    pointer_class = "ELF64" if struct.calcsize("P") == 8 else "ELF32"
    host = platform.machine().lower()
    machine = metadata["machine"].lower()
    compatible = (
        (host in ("x86_64", "amd64") and "x86-64" in machine)
        or (host in ("i386", "i486", "i586", "i686") and "80386" in machine)
        or (host in ("aarch64", "arm64") and "aarch64" in machine)
        or (host.startswith("arm") and machine.startswith("arm"))
    )
    if metadata["class"] != pointer_class or not compatible:
        raise RuntimeError(
            "selected ELF %s/%s does not match this probe process %s/%s"
            % (metadata["class"], metadata["machine"], pointer_class, platform.machine())
        )


def file_capabilities(path):
    try:
        return os.getxattr(path, "security.capability")
    except OSError as exc:
        if exc.errno in (errno.ENODATA, errno.ENOTSUP, errno.EOPNOTSUPP):
            return b""
        raise RuntimeError("cannot inspect file capabilities on %s: %s" % (path, exc)) from exc


def validate_target(path, metadata):
    mode = path.stat().st_mode
    if not stat.S_ISREG(mode) or not os.access(path, os.X_OK):
        raise RuntimeError("--kodi-binary must be an executable regular file")
    if mode & (stat.S_ISUID | stat.S_ISGID):
        raise RuntimeError("set-user-ID or set-group-ID Kodi targets are not safe for LD_PRELOAD probing")
    if file_capabilities(path):
        raise RuntimeError("file-capability Kodi targets are not safe for LD_PRELOAD probing")
    if metadata["preinit"]:
        raise RuntimeError("selected Kodi ELF has PREINIT_ARRAY code that would run before the preload probe")
    if not metadata["interpreter"]:
        raise RuntimeError("selected Kodi ELF has no dynamic program interpreter")
    interpreter_name = Path(metadata["interpreter"]).name
    if "musl" in interpreter_name or not (
        interpreter_name.startswith("ld-linux") or interpreter_name in ("ld.so.1", "ld64.so.1", "ld64.so.2")
    ):
        raise RuntimeError("selected Kodi ELF does not use a supported glibc interpreter")
    direct = [name for name in metadata["needed"] if CORE_FFMPEG_SONAME.fullmatch(name)]
    if direct:
        raise RuntimeError(
            "selected Kodi ELF directly links %s; use ffmpeg-hwconfig-probe.py for DSO-linked FFmpeg"
            % ", ".join(direct)
        )
    missing = [name for name in REQUIRED_EXPORTS if not defined_export(metadata["symbols"], name)]
    if missing:
        raise RuntimeError(
            "required main-executable dynamic export(s) missing or unsafe: %s"
            % ", ".join(missing)
        )


def validate_helper(path, target_metadata):
    if any(character.isspace() or character == ":" for character in str(path)):
        raise RuntimeError("--helper path must not contain whitespace or ':'")
    if not path.is_file():
        raise RuntimeError("--helper is not a file: %s" % path)
    metadata = elf_metadata(path)
    if metadata["type"] != "DYN":
        raise RuntimeError("--helper must be an ELF shared object")
    if (metadata["class"], metadata["machine"]) != (
        target_metadata["class"],
        target_metadata["machine"],
    ):
        raise RuntimeError("--helper architecture does not match the selected Kodi ELF")
    if not defined_export(metadata["symbols"], "__libc_start_main"):
        raise RuntimeError("--helper does not export its __libc_start_main safety interposer")
    return metadata


def parse_protocol(payload):
    try:
        text = payload.decode("ascii")
    except UnicodeDecodeError as exc:
        raise RuntimeError("preload helper returned a non-ASCII protocol") from exc
    lines = text.splitlines()
    if not lines or lines[0] != PROTOCOL_MAGIC:
        raise RuntimeError("preload helper did not return the expected protocol header")
    versions = {}
    avcodec_abi = None
    kodi_version = None
    decoder_present = None
    configs = []
    done = False
    for line in lines[1:]:
        fields = line.split("\t")
        if fields[0] == "error" and len(fields) == 3:
            detail = fields[2].replace("_", " ")
            template = HELPER_ERROR_MESSAGES.get(
                fields[1], "preload helper error %s: {detail}" % fields[1]
            )
            raise RuntimeError(template.format(detail=detail))
        if (
            fields[0] == "version"
            and len(fields) == 3
            and fields[1] in ("avcodec", "avformat", "avutil")
            and fields[2] in ("available", "unavailable")
        ):
            if fields[1] in versions:
                raise RuntimeError("preload helper repeated a version record")
            versions[fields[1]] = {"status": fields[2], "integer": None}
        elif fields[0] == "version_value" and len(fields) == 3:
            if fields[1] not in versions or versions[fields[1]]["integer"] is not None:
                raise RuntimeError("preload helper returned an unexpected version value")
            versions[fields[1]]["integer"] = int(fields[2], 10)
        elif fields[0] == "avcodec_abi" and len(fields) == 3:
            if avcodec_abi is not None or fields[2] not in ("version_export", "known_package_identity"):
                raise RuntimeError("preload helper returned an invalid libavcodec ABI record")
            avcodec_abi = {"major": int(fields[1], 10), "source": fields[2]}
        elif fields[0] == "kodi_version" and len(fields) == 4:
            if kodi_version is not None or fields[1] not in ("available", "unavailable"):
                raise RuntimeError("preload helper returned an invalid Kodi version record")
            kodi_version = {
                "status": fields[1],
                "major": None if fields[2] == "-1" else int(fields[2], 10),
                "minor": None if fields[3] == "-1" else int(fields[3], 10),
                "source": "main-origin CCompileInfo exports",
            }
        elif fields[0] == "decoder" and len(fields) == 2 and fields[1] in ("0", "1"):
            if decoder_present is not None:
                raise RuntimeError("preload helper repeated the decoder record")
            decoder_present = fields[1] == "1"
        elif fields[0] == "config" and len(fields) == 7:
            if decoder_present is not True:
                raise RuntimeError("preload helper returned a config before a present decoder")
            index = int(fields[1], 10)
            if index != len(configs):
                raise RuntimeError("preload helper returned non-contiguous config indexes")
            pixel_name = None if fields[3] == "-" else fields[3]
            device_name = None if fields[6] == "-" else fields[6]
            if any(name is not None and not SAFE_NAME.fullmatch(name) for name in (pixel_name, device_name)):
                raise RuntimeError("preload helper returned an unsafe FFmpeg name")
            methods = int(fields[4], 10)
            known_mask = sum(bit for bit, _ in METHOD_NAMES)
            configs.append(
                {
                    "index": index,
                    "pixel_format": {"number": int(fields[2], 10), "name": pixel_name},
                    "methods": {
                        "mask": methods,
                        "names": [name for bit, name in METHOD_NAMES if methods & bit],
                        "unknown_mask": methods & ~known_mask,
                    },
                    "device_type": {"number": int(fields[5], 10), "name": device_name},
                }
            )
        elif fields == ["done"]:
            if done:
                raise RuntimeError("preload helper repeated the final record")
            done = True
        else:
            raise RuntimeError("preload helper returned an invalid protocol record")
    if not done or lines[-1] != "done":
        raise RuntimeError("preload helper protocol is incomplete")
    if (
        set(versions) != {"avcodec", "avformat", "avutil"}
        or avcodec_abi is None
        or kodi_version is None
        or decoder_present is None
    ):
        raise RuntimeError("preload helper omitted required protocol records")
    if any(
        (item["status"] == "available") != (item["integer"] is not None)
        for item in versions.values()
    ):
        raise RuntimeError("preload helper returned an inconsistent version record")
    if not decoder_present and configs:
        raise RuntimeError("preload helper returned configs for a missing decoder")
    return {
        "versions": versions,
        "avcodec_abi": avcodec_abi,
        "kodi_version": kodi_version,
        "decoder_present": decoder_present,
        "configs": configs,
    }


def run_helper(
    kodi_binary,
    helper,
    decoder,
    timeout,
    library_path,
    bwrap_prefix,
    expected_avcodec_major,
):
    environment = os.environ.copy()
    for name in tuple(environment):
        if name.startswith("LD_"):
            environment.pop(name)
        if name.startswith("CRYSTALHD_KODI_EMBEDDED_PROBE"):
            environment.pop(name)
    with tempfile.TemporaryFile(prefix="crystalhd-kodi-embedded-protocol-") as protocol:
        descriptor = protocol.fileno()
        probe_environment = {
            "LD_PRELOAD": str(helper),
            ACTIVATION_ENV: "v1",
            FD_ENV: str(descriptor),
            DECODER_ENV: decoder,
        }
        if expected_avcodec_major is not None:
            probe_environment["CRYSTALHD_KODI_EMBEDDED_PROBE_AVCODEC_MAJOR"] = str(
                expected_avcodec_major
            )
        if library_path is not None:
            probe_environment["LD_LIBRARY_PATH"] = library_path
        if bwrap_prefix:
            command = list(bwrap_prefix)
            for name, value in probe_environment.items():
                command += ["--setenv", name, value]
            command += ["--", str(kodi_binary)]
        else:
            environment.update(probe_environment)
            command = [str(kodi_binary)]
        try:
            process = subprocess.Popen(
                command,
                env=environment,
                pass_fds=(descriptor,),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
            )
        except OSError as exc:
            raise RuntimeError("cannot execute selected Kodi ELF: %s" % exc) from exc
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()
            raise RuntimeError("preload helper timed out after %.1f seconds" % timeout) from exc
        protocol.seek(0)
        payload = protocol.read()
    if process.returncode < 0:
        raise RuntimeError("selected Kodi process died from signal %d" % -process.returncode)
    try:
        parsed = parse_protocol(payload)
    except RuntimeError as exc:
        detail = stderr.decode("utf-8", "replace").strip()[-2000:]
        if detail:
            raise RuntimeError("%s; loader stderr: %s" % (exc, detail)) from exc
        raise
    if process.returncode != 0:
        raise RuntimeError("preload helper exited with status %d after a completed protocol" % process.returncode)
    if stdout:
        parsed["target_stdout_bytes"] = len(stdout)
    if stderr:
        parsed["target_stderr_bytes"] = len(stderr)
    return parsed


def version_document(record):
    if record["status"] == "unavailable":
        return {"status": "unavailable"}
    value = record["integer"]
    return {
        "status": "available",
        "integer": value,
        "string": "%d.%d.%d" % ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF),
    }


def policy_candidate(config, policy_name):
    if config["pixel_format"]["name"] != "drm_prime":
        return False
    methods = config["methods"]["mask"]
    if methods & 0x04:
        return True
    if not methods & 0x01:
        return False
    if policy_name == "kodi-21-omega-a3a448d":
        return config["device_type"]["name"] == "drm"
    return True


def matching_known_package(metadata, policy_name=None):
    matches = []
    for name, policy in POLICIES.items():
        if policy_name is not None and name != policy_name:
            continue
        package = policy.get("known_packages", {}).get(metadata["build_id"])
        if package and package["sha256"] == metadata["sha256"]:
            match = dict(package)
            match["selection_policy"] = name
            matches.append(match)
    if len(matches) > 1:
        raise RuntimeError("file identity maps to more than one reviewed package policy")
    return matches[0] if matches else None


def selection_policy(arguments, metadata, observation):
    if not arguments.selection_policy:
        return {
            "name": None,
            "status": "required",
            "available": sorted(POLICIES),
            "version_observation": observation,
        }, None, "selection policy is required for a Kodi candidate verdict"
    policy = POLICIES[arguments.selection_policy]
    if observation["status"] != "available":
        return {
            "name": arguments.selection_policy,
            "status": "version-cross-check-unavailable",
            "version_observation": observation,
        }, None, "CCompileInfo version exports are unavailable for the selected policy cross-check"
    if observation["major"] != policy["expected_major"]:
        return {
            "name": arguments.selection_policy,
            "status": "version-cross-check-failed",
            "version_observation": observation,
        }, None, "CCompileInfo major %s conflicts with policy %s" % (
            observation["major"],
            arguments.selection_policy,
        )
    document = dict(policy)
    document.pop("known_packages", None)
    known_package = matching_known_package(metadata, arguments.selection_policy)
    document.update(
        {
            "name": arguments.selection_policy,
            "status": "explicit-policy-file-identity-and-major-match",
            "expected_build_id": arguments.expected_build_id.lower(),
            "observed_build_id": metadata["build_id"],
            "expected_sha256": arguments.expected_sha256.lower(),
            "observed_sha256": metadata["sha256"],
            "known_package_identity": known_package,
            "version_observation": observation,
            "attestation_scope": (
                "the probe verified the caller-supplied GNU Build ID, SHA-256 and main-origin Kodi major "
                "version; only a non-null known_package_identity binds that file identity to the reviewed "
                "package manifest"
            ),
        }
    )
    return document, arguments.selection_policy, None


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Inspect main-exported FFmpeg metadata in a trusted static-integrated Kodi ELF"
    )
    parser.add_argument("--kodi-binary", required=True, type=Path, help="actual Kodi ELF, not a launcher")
    parser.add_argument("--helper", required=True, type=Path, help="matching preload probe shared object")
    parser.add_argument("--decoder", default="h264_v4l2m2m")
    parser.add_argument("--selection-policy", choices=sorted(POLICIES))
    parser.add_argument("--expected-build-id", help="GNU Build ID bound to the reviewed selection policy")
    parser.add_argument("--expected-sha256", help="file SHA-256 bound to the reviewed selection policy")
    parser.add_argument(
        "--library-path",
        help="explicit colon-separated LD_LIBRARY_PATH inside the trusted package runtime",
    )
    parser.add_argument(
        "--bwrap-prefix-json",
        help="JSON array of bwrap and its namespace options; probe environment and target are appended",
    )
    parser.add_argument("--timeout-seconds", type=float, default=10.0)
    parser.add_argument("--pretty", action="store_true", help="indent JSON output")
    parser.add_argument(
        "--require-kodi-drm-prime",
        action="store_true",
        help="return status 1 when the selected reviewed Kodi policy finds no DRM PRIME candidate",
    )
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    identity_arguments = (
        bool(arguments.selection_policy),
        bool(arguments.expected_build_id),
        bool(arguments.expected_sha256),
    )
    if len(set(identity_arguments)) != 1:
        return fail("--selection-policy, --expected-build-id and --expected-sha256 must be supplied together")
    if arguments.expected_build_id and not re.fullmatch(r"[0-9a-fA-F]+", arguments.expected_build_id):
        return fail("--expected-build-id must be hexadecimal")
    if arguments.expected_sha256 and not re.fullmatch(r"[0-9a-fA-F]{64}", arguments.expected_sha256):
        return fail("--expected-sha256 must contain exactly 64 hexadecimal characters")
    if not SAFE_NAME.fullmatch(arguments.decoder) or len(arguments.decoder) > 127:
        return fail("--decoder must be a short ASCII FFmpeg name")
    if arguments.library_path is not None:
        library_directories = arguments.library_path.split(":")
        if not library_directories or any(
            not item or not Path(item).is_absolute() or not Path(item).is_dir()
            for item in library_directories
        ):
            return fail("--library-path entries must be existing absolute directories")
    if not 0.1 <= arguments.timeout_seconds <= 60.0:
        return fail("--timeout-seconds must be between 0.1 and 60")
    bwrap_prefix = None
    if arguments.bwrap_prefix_json:
        try:
            bwrap_prefix = json.loads(arguments.bwrap_prefix_json)
        except json.JSONDecodeError as exc:
            return fail("--bwrap-prefix-json is not valid JSON: %s" % exc)
        if (
            not isinstance(bwrap_prefix, list)
            or not bwrap_prefix
            or any(not isinstance(item, str) or not item for item in bwrap_prefix)
            or Path(bwrap_prefix[0]).name != "bwrap"
            or "--" in bwrap_prefix
        ):
            return fail("--bwrap-prefix-json must be a nonempty bwrap argument array without '--'")

    try:
        kodi_binary = arguments.kodi_binary.resolve(strict=True)
        helper = arguments.helper.resolve(strict=True)
        metadata = elf_metadata(kodi_binary)
        compatible_machine(metadata)
        validate_target(kodi_binary, metadata)
        validate_helper(helper, metadata)
        if arguments.expected_build_id and metadata["build_id"] != arguments.expected_build_id.lower():
            raise RuntimeError(
                "GNU Build ID mismatch (expected %s, found %s)"
                % (arguments.expected_build_id.lower(), metadata["build_id"])
            )
        if arguments.expected_sha256 and metadata["sha256"] != arguments.expected_sha256.lower():
            raise RuntimeError(
                "SHA-256 mismatch (expected %s, found %s)"
                % (arguments.expected_sha256.lower(), metadata["sha256"])
            )
        known_package = matching_known_package(metadata)
        observed = run_helper(
            kodi_binary,
            helper,
            arguments.decoder,
            arguments.timeout_seconds,
            arguments.library_path,
            bwrap_prefix,
            known_package["avcodec_major"] if known_package else None,
        )
        policy, selected_policy, policy_error = selection_policy(
            arguments, metadata, observed["kodi_version"]
        )
        configs = observed["configs"]
        drm_configs = [item for item in configs if item["pixel_format"]["name"] == "drm_prime"]
        candidates = (
            None
            if selected_policy is None
            else [item for item in configs if policy_candidate(item, selected_policy)]
        )
        candidate_value = None if candidates is None else bool(candidates)
        decoder = {
            "name": arguments.decoder,
            "present": observed["decoder_present"],
            "hardware_config_count": len(configs),
            "hardware_configs": configs,
            "drm_prime_config_present": bool(drm_configs),
            "kodi_find_hw_config_candidate": candidate_value,
        }
        if policy_error:
            verdict = "selection-policy-unclassified"
        elif candidate_value:
            verdict = "kodi-drm-prime-metadata-candidate"
        else:
            verdict = "no-kodi-drm-prime-metadata-candidate"
        document = {
            "schema": "crystalhd-kodi-embedded-ffmpeg-hwconfig-v1",
            "probe_completed": True,
            "probe_scope": (
                "trusted ELF startup and embedded metadata calls; no Kodi main or media/device API "
                "is called by the probe"
            ),
            "kodi": {
                "path": str(kodi_binary),
                "elf_class": metadata["class"],
                "machine": metadata["machine"],
                "elf_type": metadata["type"],
                "interpreter": metadata["interpreter"],
                "needed": metadata["needed"],
                "build_id": metadata["build_id"],
                "sha256": metadata["sha256"],
                "ffmpeg_linkage": "embedded-static",
                "version_observation": observed["kodi_version"],
            },
            "helper": {"path": str(helper), "protocol": 1},
            "loader_environment": {
                "ld_preload": str(helper),
                "ld_library_path": arguments.library_path,
                "other_inherited_ld_variables_removed": True,
                "execution_mode": "bwrap" if bwrap_prefix else "direct",
                "bwrap_prefix": bwrap_prefix,
            },
            "libraries": {
                name: {"linkage": "main-executable-export", "version": version_document(record)}
                for name, record in observed["versions"].items()
            },
            "ffmpeg_abi": {"avcodec": observed["avcodec_abi"]},
            "selection_policy": policy,
            "decoder": decoder,
            "verdict": verdict,
            "execution_boundary": {
                "target_mapped_and_relocated": True,
                "target_preinit_present": False,
                "dependency_initializers_may_run": True,
                "kodi_executable_initializers_ran": False,
                "kodi_main_ran": False,
                "embedded_ffmpeg_functions_called": True,
            },
            "process_output": {
                "stdout_bytes_before_exit": observed.get("target_stdout_bytes", 0),
                "stderr_bytes_before_exit": observed.get("target_stderr_bytes", 0),
                "contents_recorded": False,
            },
            "limitations": [
                "the selected ELF is executed and must be a trusted package binary",
                "dependency initializers may run before the preload helper",
                "a positive metadata result does not prove Kodi selection, device open, EXPBUF/import or playback",
                "the helper does not call media or device APIs, but cannot constrain dependency initializer side effects",
            ],
        }
    except (FileNotFoundError, OSError, RuntimeError, ValueError) as exc:
        return fail(str(exc))

    json.dump(document, sys.stdout, indent=2 if arguments.pretty else None, sort_keys=True)
    sys.stdout.write("\n")
    if policy_error:
        print("ffmpeg-embedded-hwconfig-probe: " + policy_error, file=sys.stderr)
        return 2
    if arguments.require_kodi_drm_prime and not candidate_value:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
