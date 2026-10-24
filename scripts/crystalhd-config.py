#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""CrystalHD v1 configuration inspection, storage and opt-in child launch.

The run command passes supported requests to a new child's environment; native
VA/DIL readers separately consume saved defaults. This CLI does not confirm
application state, access hardware or reload modules. It uses only the Python standard library, compatible with Python 3.6.
"""

import argparse
import errno
import fcntl
import json
import os
import re
import secrets
import signal
import stat
import shlex
import shutil
import struct
import subprocess
import sys


MAXIMUM_BYTES = 64 * 1024
MAXIMUM_INTEGER_DIGITS = 10
SYSTEM_CONFIG = "/etc/crystalhd/config.ini"
PRESET_MANIFEST_RELATIVE = "@PRESET_MANIFEST_RELATIVE@"
SOURCE_PRESET_MANIFEST_RELATIVE = os.path.join(
    "..", "share", "crystalhd", "config-presets-v1.json"
)
PRESET_NAMES = (
    "default", "quality", "performance", "low-memory", "streaming",
    "experimental", "custom",
)
PROFILE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
INTEGER = re.compile(r"(?:0|[1-9][0-9]*)\Z")
KEY_NAME = re.compile(r"[a-z][a-z0-9_.]*\Z")
TOKEN_NAME = re.compile(r"[a-z0-9][a-z0-9-]{0,63}\Z")
EVIDENCE_REF = re.compile(
    r"[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*\.md#[a-z0-9-]+\Z"
)


READER_KEYS = {
    "dil": ("libcrystalhd.rx_pool_count",),
    "vaapi": ("vaapi.h264.mode", "vaapi.h264.low_latency", "vaapi.h264.ready_remap",
              "vaapi.i915_wc", "vaapi.debug", "vaapi.trace"),
    "player": ("player.decoder", "gstreamer.crystalhddec.scale_width",
               "player.software_output_size"),
}


def _inspection_bytes(path, maximum=64 * 1024 * 1024):
    # Nonblocking open prevents a replaced FIFO from hanging a settings refresh.
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > maximum:
            raise ValueError("not a bounded regular file")
        data = stream.read(maximum + 1)
        if len(data) > maximum:
            raise ValueError("file grew beyond inspection bound")
        return data


def inspect_reader(path, component, elf_class=None):
    result = {"component": component, "path": path, "state": "unknown",
              "detail": "no candidate found", "keys": []}
    if not path:
        return result
    try:
        data = _inspection_bytes(path)
        if elf_class is not None and (data[:4] != b"\x7fELF" or
                                      data[4:5] != bytes((elf_class,))):
            result["detail"] = "not a matching ELF ABI"
            return result
        marker = ("CrystalHD managed defaults v1:" + component + "\n").encode("ascii")
        if marker in data:
            result.update(state="declared-v1", detail="file declares managed-defaults reader v1",
                          keys=list(READER_KEYS[component]))
        else:
            result["detail"] = "reader not advertised (legacy or unknown build)"
    except (OSError, ValueError) as error:
        result.update(state="unreadable", detail="file unavailable or not safely readable: " +
                      (str(error.errno) if isinstance(error, OSError) else str(error)))
    return result


def _linker_cache():
    for command in ("/sbin/ldconfig", "/usr/sbin/ldconfig"):
        if os.path.isfile(command):
            try:
                result = subprocess.run([command, "-p"], stdin=subprocess.DEVNULL,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"}, timeout=2)
                if result.returncode == 0:
                    return result.stdout.decode("utf-8", "replace"), "read"
                return "", "listing failed"
            except (OSError, subprocess.TimeoutExpired):
                return "", "listing unavailable or timed out"
    return "", "listing tool unavailable"


def _cache_candidate(cache, name, elf_class):
    fallback = None
    for line in cache.splitlines():
        fields = line.strip().split(None, 1)
        if not fields or fields[0] != name or " => " not in line:
            continue
        path = line.split(" => ", 1)[1].strip()
        if not os.path.isabs(path):
            continue
        if fallback is None:
            fallback = path
        try:
            # Do not read candidate bodies just to select the matching ABI.
            descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            with os.fdopen(descriptor, "rb") as stream:
                if stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                    header = stream.read(5)
                    if header == b"\x7fELF" + bytes((elf_class,)):
                        return path
        except OSError:
            continue
    return fallback


def _player_reader_path(wrapper):
    if not wrapper:
        return None
    # Match the wrapper's actual dirname semantics; do not resolve its symlink.
    directory = os.path.dirname(os.path.abspath(wrapper))
    try:
        text = _inspection_bytes(wrapper, 1024 * 1024).decode("utf-8")
        if "# CrystalHD Player controller-location protocol v1\n" not in text:
            return None
        adjacent = os.path.join(directory, "crystalhd-player.py")
        if os.path.isfile(adjacent):
            return adjacent
        lines = [line for line in text.splitlines()
                 if line.startswith("player_controller_relative=")]
        if len(lines) != 1:
            return None
        words = shlex.split(lines[0].split("=", 1)[1])
        if len(words) != 1 or not words[0] or "@" in words[0]:
            return None
        return os.path.normpath(os.path.join(directory, words[0]))
    except (OSError, ValueError, UnicodeError):
        return None


def inspect_installation(environment=None, cache_text=None):
    """Inspect declarations only; never execute an inspected component or load INI."""
    environment = os.environ if environment is None else environment
    elf_class = struct.calcsize("P") // 4
    if cache_text is None:
        cache_text, cache_state = _linker_cache()
    else:
        cache_state = "provided"
    dil = _cache_candidate(cache_text, "libcrystalhd.so.3", elf_class)
    libva = _cache_candidate(cache_text, "libva.so.2", elf_class)
    va = (os.path.join(os.path.dirname(os.path.realpath(libva)), "dri", "crystalhd_drv_video.so")
          if libva else None)
    wrapper = shutil.which("crystalhd-play", path=environment.get("PATH", os.defpath))
    return {"scope": "file declarations only; running applications are not inspected",
            "abi_bits": elf_class * 32, "linker_cache": cache_state,
            "player_wrapper": wrapper,
            "overrides_present": [name for name in (
                "LD_LIBRARY_PATH", "LD_PRELOAD", "LIBVA_DRIVERS_PATH", "LIBVA_DRIVER_NAME",
                "CRYSTALHD_PLAYER_PYTHON") if name in environment],
            "caveat": "Default cache candidates can be overridden. Other client ABIs and mapped files may differ.",
            "readers": [inspect_reader(dil, "dil", elf_class),
                        inspect_reader(va, "vaapi", elf_class),
                        inspect_reader(_player_reader_path(wrapper), "player")]}


def installation_text(report):
    lines = ["Installation: " + report["scope"],
             "Inspector ABI: {0}-bit; linker cache: {1}".format(report["abi_bits"], report["linker_cache"]),
             "Player wrapper: " + (report["player_wrapper"] or "not found")]
    for item in report["readers"]:
        lines.append("{0}: {1}\n  {2}\n  {3}".format(item["component"], item["state"],
                     item["path"] or "no file", item["detail"]))
        if item["keys"]:
            lines.append("  Keys: " + ", ".join(item["keys"]))
    lines.append("Overrides present (values omitted): " + (", ".join(report["overrides_present"]) or "none"))
    lines.append(report["caveat"])
    return "\n".join(lines)


class SchemaError(ValueError):
    """The managed configuration cannot be accepted as one transaction."""


class CommandLineSelectionError(ValueError):
    """A dynamic manifest-backed command-line token is invalid."""


class RedactedArgumentParser(argparse.ArgumentParser):
    """Keep command-line syntax failures deterministic and non-reflecting."""

    def error(self, message):
        del message
        self.print_usage(sys.stderr)
        self.exit(2, "{0}: error: invalid command line\n".format(self.prog))


class SingleWriteOption(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        del option_string
        if getattr(namespace, self.dest, None) is not None:
            parser.error("duplicate write option")
        setattr(namespace, self.dest, values)


class _Unset(object):
    def __repr__(self):
        return "unset"


class _Absent(object):
    def __repr__(self):
        return "absent"


UNSET = _Unset()
ABSENT = _Absent()


def boolean(value):
    return type(value) is bool


def integer_enum(*accepted):
    allowed = frozenset(accepted)
    def validate(value):
        return type(value) is int and value in allowed
    validate.native_kind = "IntegerEnum"
    validate.native_choices = tuple(sorted(allowed))
    return validate


def string_enum(*accepted):
    allowed = frozenset(accepted)
    def validate(value):
        return type(value) is str and value in allowed
    validate.native_kind = "StringEnum"
    validate.native_choices = tuple(sorted(allowed))
    return validate


def nonempty_string(value):
    return type(value) is str and bool(value) and "\0" not in value


def any_string(value):
    return type(value) is str and "\0" not in value


def scale_width(value):
    return type(value) is int and (
        value == 0 or 128 <= value <= 1918 and value % 2 == 0
    )


def output_size(value):
    if type(value) is not str:
        return False
    found = re.fullmatch(r"([1-9][0-9]*)x([1-9][0-9]*)", value)
    if found is None:
        return False
    dimensions = found.groups()
    if any(len(item) > 4 for item in dimensions):
        return False
    width, height = map(int, dimensions)
    return (
        width <= 4096
        and height <= 4096
        and width % 2 == 0
        and height % 2 == 0
        and width * height <= 4096 * 2160
    )


RULES = {
    "libcrystalhd.rx_pool_count": integer_enum(8, 16),
    "vaapi.h264.mode": string_enum("bounded", "strict", "live"),
    "vaapi.h264.low_latency": boolean,
    "vaapi.h264.ready_remap": boolean,
    "vaapi.h264.ready_policy": string_enum("previous", "latest"),
    "vaapi.h264.ready_delay_us": integer_enum(0, 16667, 33333, 50000),
    "vaapi.i915_wc": boolean,
    "vaapi.debug": boolean,
    "vaapi.trace": boolean,
    "chromium.executable": nonempty_string,
    "chromium.drm_device": nonempty_string,
    "chromium.password_store": any_string,
    "chromium.profile_dir": nonempty_string,
    "chromium.native_gl": boolean,
    "chromium.experimental_hw_decode": boolean,
    "chromium.disable_gpu_sandbox": boolean,
    "chromium.h264_policy_ack": boolean,
    "player.python": nonempty_string,
    "gstreamer.crystalhddec.scale_width": scale_width,
    "player.decoder": string_enum("hardware", "software"),
    "player.software_output_size": output_size,
}

NATIVE = {
    "libcrystalhd.rx_pool_count": 16,
    "vaapi.h264.mode": "bounded",
    "vaapi.h264.low_latency": False,
    "vaapi.h264.ready_remap": False,
    "vaapi.h264.ready_policy": "previous",
    "vaapi.h264.ready_delay_us": 0,
    "vaapi.i915_wc": False,
    "vaapi.debug": False,
    "vaapi.trace": False,
    "chromium.executable": ABSENT,
    "chromium.drm_device": "/dev/dri/renderD128",
    "chromium.password_store": "basic",
    "chromium.profile_dir": ABSENT,
    "chromium.native_gl": False,
    "chromium.experimental_hw_decode": False,
    "chromium.disable_gpu_sandbox": False,
    "chromium.h264_policy_ack": False,
    "player.python": "/usr/bin/python3",
    "gstreamer.crystalhddec.scale_width": 0,
    "player.decoder": "hardware",
    "player.software_output_size": ABSENT,
}

ENVIRONMENT_TARGETS = frozenset((
    "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT",
    "CRYSTALHD_VAAPI_LIVE_H264",
    "CRYSTALHD_VAAPI_LOW_LATENCY_H264",
    "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP",
    "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY",
    "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US",
    "CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC",
    "CRYSTALHD_VAAPI_DEBUG",
    "CRYSTALHD_VAAPI_TRACE",
    "CRYSTALHD_CHROMIUM",
    "CRYSTALHD_DRM_DEVICE",
    "CRYSTALHD_CHROMIUM_PASSWORD_STORE",
    "CRYSTALHD_CHROMIUM_PROFILE_DIR",
    "CRYSTALHD_CHROMIUM_NATIVE_GL",
    "CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE",
    "CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX",
    "CRYSTALHD_CHROMIUM_H264_POLICY",
    "CRYSTALHD_PLAYER_PYTHON",
))

LAUNCH_ENVIRONMENT = {
    "libcrystalhd.rx_pool_count": "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT",
    "vaapi.h264.mode": "CRYSTALHD_VAAPI_LIVE_H264",
    "vaapi.h264.low_latency": "CRYSTALHD_VAAPI_LOW_LATENCY_H264",
    "vaapi.h264.ready_remap": "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP",
    "vaapi.i915_wc": "CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC",
    "vaapi.debug": "CRYSTALHD_VAAPI_DEBUG",
    "vaapi.trace": "CRYSTALHD_VAAPI_TRACE",
    "player.python": "CRYSTALHD_PLAYER_PYTHON",
}

NATIVE_CONFIG_DISABLE_ENVIRONMENT = "CRYSTALHD_CONFIG_DISABLE"

CENTERED_NV12_ENVIRONMENT = "CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12"
CENTERED_NV12_MODEL_CAVEAT = (
    "experimental centered NV12 environment is present; its value acceptance, "
    "output geometry and VA context rejection rules are not modeled; "
    "effective settings cover canonical native adapters only"
)

PATH_KEYS = frozenset((
    "chromium.executable",
    "chromium.drm_device",
    "chromium.profile_dir",
    "player.python",
))


class Document(object):
    def __init__(self):
        self.metadata = {}
        self.settings = {}
        self.profiles = {}


class UserDocumentSnapshot(object):
    """An edit session with a document and its original file snapshot."""

    __slots__ = ("document", "_path", "_directory_identity", "_file_identity",
                 "_original_bytes")

    def __init__(self, path, document, directory_identity, file_identity,
                 original_bytes=None):
        self._path = path
        self.document = document
        self._directory_identity = directory_identity
        self._file_identity = file_identity
        self._original_bytes = original_bytes

    path = property(lambda self: self._path)
    directory_identity = property(lambda self: self._directory_identity)
    file_identity = property(lambda self: self._file_identity)
    original_bytes = property(lambda self: self._original_bytes)


class SaveResult(object):
    def __init__(self, durable, committed=True):
        self.committed = committed
        self.durable = durable


class LaunchPlan(object):
    def __init__(self, environment, requested_keys):
        self.environment = environment
        self.requested_keys = tuple(sorted(requested_keys))


class InvalidNativeValue(object):
    def __init__(self, raw, fallback):
        self.raw = raw
        self.fallback = fallback

    def __eq__(self, other):
        return (
            isinstance(other, InvalidNativeValue)
            and self.raw == other.raw
            and self.fallback == other.fallback
        )

    def __repr__(self):
        return "InvalidNativeValue(<redacted>, {0!r})".format(self.fallback)


class RawReceipt(object):
    def __init__(self, variable, key, raw, requested, valid):
        self.variable = variable
        self.key = key
        self.raw = raw
        self.requested = requested
        self.valid = valid

    def __eq__(self, other):
        return (
            isinstance(other, RawReceipt)
            and self.variable == other.variable
            and self.key == other.key
            and self.raw == other.raw
            and self.requested == other.requested
            and self.valid == other.valid
        )


class NativeRejection(object):
    def __init__(self, component, reason):
        self.component = component
        self.reason = reason

    def __eq__(self, other):
        return (
            isinstance(other, NativeRejection)
            and self.component == other.component
            and self.reason == other.reason
        )

    def __repr__(self):
        return "NativeRejection({0!r}, {1!r})".format(
            self.component, self.reason
        )


class Resolution(object):
    def __init__(self, requested, origins, effective, activity, raw_receipts,
                 rejections, preset, profile, centered_nv12_environment_present=False,
                 native_config_disabled=False):
        self.requested = requested
        self.origins = origins
        self.effective = effective
        self.activity = activity
        self.raw_receipts = raw_receipts
        self.rejections = rejections
        self.preset = preset
        self.profile = profile
        # This native-only boundary retains presence, never its raw value.
        self.centered_nv12_environment_present = centered_nv12_environment_present
        self.native_config_disabled = native_config_disabled


class PresetEntry(object):
    def __init__(self, record):
        self.record = record
        self.name = record["name"]
        self.status = record["status"]
        self.settings = record["settings"]
        self.required_acknowledgements = tuple(
            record["required_acknowledgements"]
        )


class PresetCatalog(object):
    def __init__(self, entries):
        self.entries = tuple(entries)
        self.by_name = dict((entry.name, entry) for entry in entries)
        self.selectable = dict(
            (entry.name, entry.settings)
            for entry in entries if entry.status == "SELECTABLE"
        )
        self.acknowledgement_tokens = frozenset(
            token
            for entry in entries
            for token in entry.required_acknowledgements
        )


def _unique_json_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise SchemaError("preset manifest contains a duplicate field")
        value[key] = item
    return value


def _plain_text(value, maximum):
    if type(value) is not str or not 0 < len(value) <= maximum:
        return False
    for character in value:
        codepoint = ord(character)
        if (codepoint < 0x20 or codepoint == 0x7f
                or 0xd800 <= codepoint <= 0xdfff):
            return False
    return True


def parse_preset_manifest(data):
    """Parse the bounded, versioned preset sidecar without applying it."""
    if len(data) > MAXIMUM_BYTES:
        raise SchemaError("preset manifest exceeds 64 KiB")
    if data.startswith(b"\xef\xbb\xbf"):
        raise SchemaError("preset manifest UTF-8 byte-order mark is not permitted")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise SchemaError("preset manifest is not UTF-8")
    try:
        document = json.loads(text, object_pairs_hook=_unique_json_object)
    except SchemaError:
        raise
    except (ValueError, RecursionError):
        raise SchemaError("preset manifest is not valid JSON")

    root_fields = {
        "format", "format_version", "configuration_schema", "presets",
    }
    if type(document) is not dict or set(document) != root_fields:
        raise SchemaError("preset manifest root fields are invalid")
    if document["format"] != "crystalhd-presets":
        raise SchemaError("preset manifest format is unsupported")
    if type(document["format_version"]) is not int \
            or document["format_version"] != 1:
        raise SchemaError("preset manifest version is unsupported")
    if type(document["configuration_schema"]) is not int \
            or document["configuration_schema"] != 1:
        raise SchemaError("preset manifest configuration schema is unsupported")
    records = document["presets"]
    if type(records) is not list or len(records) != len(PRESET_NAMES):
        raise SchemaError("preset manifest has an invalid preset inventory")

    entry_fields = {
        "name", "display_name", "status", "safety",
        "required_acknowledgements", "requirements", "evidence_refs",
        "settings", "reason_code",
    }
    entries = []
    all_tokens = set()
    for expected_name, record in zip(PRESET_NAMES, records):
        if type(record) is not dict or set(record) != entry_fields:
            raise SchemaError("preset manifest entry fields are invalid")
        name = record["name"]
        if name != expected_name or TOKEN_NAME.fullmatch(name) is None:
            raise SchemaError("preset manifest order or name is invalid")
        if not _plain_text(record["display_name"], 64):
            raise SchemaError("preset display name is invalid")
        status = record["status"]
        if status not in ("SELECTABLE", "UNAVAILABLE", "VIRTUAL"):
            raise SchemaError("preset status is invalid")
        safety = record["safety"]
        if safety not in ("STANDARD", "EXPERIMENTAL", "DERIVED"):
            raise SchemaError("preset safety class is invalid")

        acknowledgements = record["required_acknowledgements"]
        if type(acknowledgements) is not list:
            raise SchemaError("preset acknowledgements are invalid")
        if any(type(token) is not str or TOKEN_NAME.fullmatch(token) is None
               for token in acknowledgements):
            raise SchemaError("preset acknowledgement token is invalid")
        if len(acknowledgements) != len(set(acknowledgements)):
            raise SchemaError("preset acknowledgement token is duplicated")
        if all_tokens & set(acknowledgements):
            raise SchemaError("preset acknowledgement token is ambiguous")
        all_tokens.update(acknowledgements)

        requirements = record["requirements"]
        if type(requirements) is not list:
            raise SchemaError("preset requirements are invalid")
        requirement_ids = set()
        for requirement in requirements:
            if type(requirement) is not dict \
                    or set(requirement) != {"id", "description"}:
                raise SchemaError("preset requirement fields are invalid")
            requirement_id = requirement["id"]
            if type(requirement_id) is not str \
                    or TOKEN_NAME.fullmatch(requirement_id) is None:
                raise SchemaError("preset requirement identifier is invalid")
            if requirement_id in requirement_ids:
                raise SchemaError("preset requirement is duplicated")
            requirement_ids.add(requirement_id)
            if not _plain_text(requirement["description"], 512):
                raise SchemaError("preset requirement description is invalid")

        evidence = record["evidence_refs"]
        if type(evidence) is not list or not evidence:
            raise SchemaError("preset evidence references are invalid")
        if any(type(reference) is not str
               or EVIDENCE_REF.fullmatch(reference) is None
               for reference in evidence):
            raise SchemaError("preset evidence reference is invalid")
        if len(evidence) != len(set(evidence)):
            raise SchemaError("preset evidence reference is duplicated")

        settings = record["settings"]
        reason = record["reason_code"]
        if status == "SELECTABLE":
            if type(settings) is not dict:
                raise SchemaError("selectable preset settings are invalid")
            for key, value in settings.items():
                if key not in RULES or value is None or not RULES[key](value):
                    raise SchemaError("preset setting is invalid")
            candidate = dict(NATIVE)
            candidate.update(settings)
            validate_managed(candidate)
            if reason is not None:
                raise SchemaError("selectable preset has an unavailable reason")
            if safety == "STANDARD" and acknowledgements:
                raise SchemaError("standard preset requires a risk acknowledgement")
            if safety == "EXPERIMENTAL" and not acknowledgements:
                raise SchemaError("experimental preset lacks an acknowledgement")
            if safety == "DERIVED":
                raise SchemaError("selectable preset has derived safety")
        else:
            if settings is not None:
                raise SchemaError("non-selectable preset contains settings")
            if acknowledgements:
                raise SchemaError("non-selectable preset has acknowledgements")
            if type(reason) is not str or TOKEN_NAME.fullmatch(reason) is None:
                raise SchemaError("non-selectable preset reason is invalid")
            if status == "VIRTUAL" and (name != "custom" or safety != "DERIVED"):
                raise SchemaError("virtual preset contract is invalid")
        entries.append(PresetEntry(record))

    if entries[0].name != "default" or entries[0].settings != {}:
        raise SchemaError("default preset must remain empty")
    return PresetCatalog(entries)


def parse_value(text, line_number):
    if text == "true":
        return True
    if text == "false":
        return False
    if text == "unset":
        return UNSET
    if INTEGER.fullmatch(text):
        if len(text) > MAXIMUM_INTEGER_DIGITS:
            raise SchemaError("line {0}: integer literal is too long".format(
                line_number
            ))
        return int(text)
    if text.startswith('"'):
        try:
            value = json.loads(text)
        except ValueError as error:
            message = getattr(error, "msg", "invalid JSON string")
            raise SchemaError("line {0}: invalid string: {1}".format(
                line_number, message
            ))
        if type(value) is not str or "\0" in value:
            raise SchemaError("line {0}: invalid string value".format(line_number))
        return value
    raise SchemaError("line {0}: invalid value syntax".format(line_number))


def parse_document(data):
    if len(data) > MAXIMUM_BYTES:
        raise SchemaError("configuration exceeds 64 KiB")
    if data.startswith(b"\xef\xbb\xbf"):
        raise SchemaError("UTF-8 byte-order mark is not permitted")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise SchemaError("configuration is not UTF-8")

    document = Document()
    current = None
    seen_sections = set()
    seen_keys = {}

    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            if section in seen_sections:
                raise SchemaError("line {0}: duplicate section".format(line_number))
            if section in ("crystalhd", "settings"):
                current = (section, None)
            elif section.startswith("profile."):
                profile = section[len("profile."):]
                if PROFILE_NAME.fullmatch(profile) is None:
                    raise SchemaError("line {0}: invalid profile section".format(
                        line_number
                    ))
                current = ("profile", profile)
                document.profiles[profile] = {}
            else:
                raise SchemaError("line {0}: unknown section".format(line_number))
            seen_sections.add(section)
            seen_keys[section] = set()
            continue
        if current is None or "=" not in raw_line:
            raise SchemaError("line {0}: key outside a section".format(line_number))

        key_text, value_text = raw_line.split("=", 1)
        key = key_text.strip()
        value_source = value_text.strip()
        section_kind, profile = current
        section_name = section_kind if profile is None else "profile." + profile
        if KEY_NAME.fullmatch(key) is None:
            raise SchemaError("line {0}: invalid key".format(line_number))
        if key in seen_keys[section_name]:
            raise SchemaError("line {0}: duplicate key".format(line_number))
        seen_keys[section_name].add(key)
        value = parse_value(value_source, line_number)

        if section_kind == "crystalhd":
            if key not in ("schema", "preset", "profile"):
                raise SchemaError("line {0}: unknown metadata key".format(
                    line_number
                ))
            if key == "schema":
                if type(value) is not int or value != 1:
                    raise SchemaError("line {0}: unsupported schema version".format(
                        line_number
                    ))
            elif type(value) is not str or PROFILE_NAME.fullmatch(value) is None:
                raise SchemaError("line {0}: invalid selector {1}".format(
                    line_number, key
                ))
            document.metadata[key] = value
            continue

        if key not in RULES:
            raise SchemaError("line {0}: unknown setting".format(line_number))
        if value is not UNSET and not RULES[key](value):
            raise SchemaError("line {0}: invalid value for {1}".format(line_number, key))
        target = document.settings if section_kind == "settings" else document.profiles[profile]
        target[key] = value

    if "crystalhd" not in seen_sections or document.metadata.get("schema") != 1:
        raise SchemaError("missing [crystalhd] schema = 1")
    return document


def empty_document():
    return parse_document(b"[crystalhd]\nschema = 1\n")


def serialize_document(document):
    """Serialize only explicit overlay entries, using the existing parser."""
    if (type(document) is not Document or
            set(vars(document)) != {"metadata", "settings", "profiles"} or
            any(type(layer) is not dict for layer in
                (document.metadata, document.settings, document.profiles))):
        raise SchemaError("invalid user document")
    lines = []

    def section(name, values):
        if type(values) is not dict or any(type(key) is not str for key in values):
            raise SchemaError("invalid user document section")
        lines.append("[{0}]".format(name))
        for key in sorted(values):
            if KEY_NAME.fullmatch(key) is None:
                raise SchemaError("invalid user document key")
            value = values[key]
            if value is not UNSET and type(value) not in (bool, int, str):
                raise SchemaError("invalid user document value")
            if type(value) is int and not 0 <= value < 10 ** MAXIMUM_INTEGER_DIGITS:
                raise SchemaError("user document integer exceeds the strict grammar")
            if type(value) is str and len(value) > MAXIMUM_BYTES:
                raise SchemaError("configuration exceeds 64 KiB")
            lines.append("{0} = {1}".format(
                key, "unset" if value is UNSET else _format_value(value)
            ))
        lines.append("")

    section("crystalhd", document.metadata)
    if document.settings:
        section("settings", document.settings)
    if any(type(name) is not str or PROFILE_NAME.fullmatch(name) is None
           for name in document.profiles):
        raise SchemaError("invalid user profile name")
    for name in sorted(document.profiles):
        section("profile." + name, document.profiles[name])
    data = ("\n".join(lines)).encode("utf-8")
    parse_document(data)
    return data


def apply_layer(values, origins, layer, origin):
    for key, value in layer.items():
        if key not in RULES:
            raise SchemaError("unknown canonical key in configuration layer")
        if value is UNSET:
            values[key] = NATIVE[key]
            origins[key] = origin + ":unset"
        elif not RULES[key](value):
            raise SchemaError("invalid canonical value in configuration layer")
        else:
            values[key] = value
            origins[key] = origin


def adapt_raw_environment(values, origins, environment):
    receipts = []
    for variable, raw in environment.items():
        if type(raw) is not str or "\0" in raw:
            raise SchemaError("environment contains an invalid value")

    def apply(variable, key, requested, valid=True):
        if variable not in environment:
            return
        raw = environment[variable]
        values[key] = requested
        origins[key] = "environment:" + variable
        receipts.append(RawReceipt(variable, key, raw, requested, valid))

    variable = "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT"
    if variable in environment:
        raw = environment[variable]
        if raw in ("8", "16"):
            apply(variable, "libcrystalhd.rx_pool_count", int(raw))
        else:
            apply(variable, "libcrystalhd.rx_pool_count",
                  InvalidNativeValue(raw, 16), False)

    variable = "CRYSTALHD_VAAPI_LIVE_H264"
    if variable in environment:
        apply(variable, "vaapi.h264.mode",
              "live" if environment[variable] == "1" else "strict")

    for variable, key in (
        ("CRYSTALHD_VAAPI_LOW_LATENCY_H264", "vaapi.h264.low_latency"),
        ("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP", "vaapi.h264.ready_remap"),
        ("CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC", "vaapi.i915_wc"),
        ("CRYSTALHD_VAAPI_TRACE", "vaapi.trace"),
        ("CRYSTALHD_CHROMIUM_NATIVE_GL", "chromium.native_gl"),
        ("CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE", "chromium.experimental_hw_decode"),
        ("CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX", "chromium.disable_gpu_sandbox"),
        ("CRYSTALHD_CHROMIUM_H264_POLICY", "chromium.h264_policy_ack"),
    ):
        if variable in environment:
            apply(variable, key, environment[variable] == "1")

    variable = "CRYSTALHD_VAAPI_DEBUG"
    if variable in environment:
        apply(variable, "vaapi.debug", True)

    variable = "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY"
    if variable in environment:
        raw = environment[variable]
        valid = raw in ("previous", "latest")
        requested = raw if valid else InvalidNativeValue(raw, "previous")
        apply(variable, "vaapi.h264.ready_policy", requested, valid)

    variable = "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US"
    if variable in environment:
        raw = environment[variable]
        if raw in ("0", "16667", "33333", "50000"):
            apply(variable, "vaapi.h264.ready_delay_us", int(raw))
        else:
            apply(variable, "vaapi.h264.ready_delay_us",
                  InvalidNativeValue(raw, 0), False)

    for variable, key, fallback in (
        ("CRYSTALHD_CHROMIUM", "chromium.executable", ABSENT),
        ("CRYSTALHD_DRM_DEVICE", "chromium.drm_device", "/dev/dri/renderD128"),
        ("CRYSTALHD_CHROMIUM_PROFILE_DIR", "chromium.profile_dir", ABSENT),
        ("CRYSTALHD_PLAYER_PYTHON", "player.python", "/usr/bin/python3"),
    ):
        if variable in environment:
            raw = environment[variable]
            apply(variable, key, raw if raw else fallback)

    variable = "CRYSTALHD_CHROMIUM_PASSWORD_STORE"
    if variable in environment:
        apply(variable, "chromium.password_store", environment[variable])

    return tuple(receipts)


def native_effect(requested):
    effective = {}
    for key, value in requested.items():
        effective[key] = value.fallback if isinstance(value, InvalidNativeValue) else value
    # These states explain configuration/native-adapter semantics only.  The
    # inspector never claims that a setting was applied by a running backend.
    activity = dict((key, "configured:not-applied") for key in requested)
    rejections = []

    low_latency_requested = requested["vaapi.h264.low_latency"] is True
    if not low_latency_requested:
        effective["vaapi.h264.low_latency"] = False
        activity["vaapi.h264.low_latency"] = "explicit-prefeed:not-requested"

    invalid_policy = isinstance(requested["vaapi.h264.ready_policy"], InvalidNativeValue)
    invalid_delay = isinstance(requested["vaapi.h264.ready_delay_us"], InvalidNativeValue)
    remap_requested = requested["vaapi.h264.ready_remap"] is True
    if not remap_requested:
        effective["vaapi.h264.ready_remap"] = False
        activity["vaapi.h264.ready_remap"] = "disabled"
    elif invalid_policy:
        effective["vaapi.h264.ready_remap"] = False
        activity["vaapi.h264.ready_remap"] = "inactive:invalid-ready-policy"
    elif invalid_delay:
        effective["vaapi.h264.ready_remap"] = False
        activity["vaapi.h264.ready_remap"] = "inactive:invalid-ready-delay"
    elif (effective["vaapi.h264.mode"] != "live"
            or not effective["vaapi.h264.low_latency"]):
        effective["vaapi.h264.ready_remap"] = False
        activity["vaapi.h264.ready_remap"] = "inactive:requires-live-low-latency"

    if invalid_policy:
        activity["vaapi.h264.ready_policy"] = "inactive:invalid-native-value"
    elif not effective["vaapi.h264.ready_remap"]:
        activity["vaapi.h264.ready_policy"] = "inactive:ready-remap-disabled"
    if invalid_delay:
        activity["vaapi.h264.ready_delay_us"] = "inactive:invalid-native-value"
    elif not effective["vaapi.h264.ready_remap"]:
        activity["vaapi.h264.ready_delay_us"] = "inactive:ready-remap-disabled"

    if isinstance(requested["libcrystalhd.rx_pool_count"], InvalidNativeValue):
        activity["libcrystalhd.rx_pool_count"] = "fallback:invalid-native-value"
    elif effective["libcrystalhd.rx_pool_count"] == 8:
        activity["libcrystalhd.rx_pool_count"] = (
            "conditional:not-inspected:bcm70015-capture-pool"
        )

    if effective["vaapi.i915_wc"]:
        activity["vaapi.i915_wc"] = "conditional:not-inspected:i915-wc-path"
    else:
        activity["vaapi.i915_wc"] = "disabled"

    if effective["vaapi.h264.mode"] == "live":
        activity["vaapi.h264.mode"] = (
            "conditional:not-inspected:bcm70015-live-path"
        )
    if effective["vaapi.h264.low_latency"]:
        activity["vaapi.h264.low_latency"] = (
            "conditional:not-inspected:bcm70015-h264-prefeed"
        )
    if effective["vaapi.h264.ready_remap"]:
        activity["vaapi.h264.ready_remap"] = (
            "conditional:not-inspected:experimental-ready-path"
        )

    if effective["gstreamer.crystalhddec.scale_width"]:
        activity["gstreamer.crystalhddec.scale_width"] = (
            "conditional:not-inspected:bcm70015-progressive-coded-width"
        )
    else:
        activity["gstreamer.crystalhddec.scale_width"] = "disabled"

    if effective["player.decoder"] == "hardware":
        activity["player.decoder"] = (
            "conditional:not-inspected:device-plugin-and-media"
        )
    if effective["player.software_output_size"] is not ABSENT:
        activity["player.software_output_size"] = (
            "conditional:not-inspected:gstreamer-scaling-elements"
        )

    if not effective["chromium.experimental_hw_decode"]:
        activity["chromium.drm_device"] = "inactive:hardware-mode-disabled"
    else:
        activity["chromium.drm_device"] = (
            "conditional:not-inspected:drm-device"
        )

    if (effective["chromium.experimental_hw_decode"]
            and not effective["chromium.disable_gpu_sandbox"]):
        reason = "missing-gpu-sandbox-acknowledgement"
        activity["chromium.experimental_hw_decode"] = "rejected:" + reason
        activity["chromium.drm_device"] = "inactive:launcher-rejected"
        rejections.append(NativeRejection("crystalhd-chromium", reason))
    elif effective["chromium.experimental_hw_decode"]:
        activity["chromium.experimental_hw_decode"] = (
            "conditional:not-inspected:browser-gpu-route"
        )

    return effective, activity, tuple(rejections)


def resolve(system, user, presets, explicit_preset=None, explicit_profile=None,
            raw_environment=None, application=None):
    if (explicit_preset is not None
            and (type(explicit_preset) is not str
                 or PROFILE_NAME.fullmatch(explicit_preset) is None)):
        raise SchemaError("invalid explicit preset selector")
    if (explicit_profile is not None
            and (type(explicit_profile) is not str
                 or PROFILE_NAME.fullmatch(explicit_profile) is None)):
        raise SchemaError("invalid explicit profile selector")
    preset = explicit_preset
    if preset is None:
        preset = user.metadata.get("preset", system.metadata.get("preset", "default"))
    profile = explicit_profile
    if profile is None:
        profile = user.metadata.get("profile", system.metadata.get("profile"))
    if preset not in presets:
        raise SchemaError("unknown selected preset")
    if (profile is not None and profile not in system.profiles
            and profile not in user.profiles):
        raise SchemaError("unknown selected profile")

    managed = dict(NATIVE)
    managed_origins = dict((key, "compiled") for key in RULES)
    apply_layer(managed, managed_origins, presets[preset], "preset:" + preset)
    apply_layer(managed, managed_origins, system.settings, "system")
    apply_layer(managed, managed_origins, user.settings, "user")
    if profile is not None:
        apply_layer(managed, managed_origins, system.profiles.get(profile, {}),
                    "system-profile:" + profile)
        apply_layer(managed, managed_origins, user.profiles.get(profile, {}),
                    "user-profile:" + profile)

    canonical = dict(managed)
    canonical_origins = dict(managed_origins)
    apply_layer(canonical, canonical_origins, application or {}, "application")
    validate_managed(canonical)

    requested = dict(managed)
    origins = dict(managed_origins)
    environment = raw_environment or {}
    receipts = adapt_raw_environment(requested, origins, environment)
    apply_layer(requested, origins, application or {}, "application")
    effective, activity, rejections = native_effect(requested)
    return Resolution(requested, origins, effective, activity, receipts,
                      rejections, preset, profile,
                      CENTERED_NV12_ENVIRONMENT in environment,
                      environment.get(NATIVE_CONFIG_DISABLE_ENVIRONMENT) == "1")


def validate_managed(values):
    if values["vaapi.h264.ready_remap"] and (
            values["vaapi.h264.mode"] != "live"
            or not values["vaapi.h264.low_latency"]):
        raise SchemaError("READY remap requires live low-latency H.264")
    if (values["chromium.experimental_hw_decode"]
            and not values["chromium.disable_gpu_sandbox"]):
        raise SchemaError("Chromium hardware mode requires sandbox acknowledgement")
    scale = values["gstreamer.crystalhddec.scale_width"]
    if scale != 0 and values["player.decoder"] != "hardware":
        raise SchemaError("scale width requires hardware decode")
    output = values["player.software_output_size"]
    if output is not ABSENT and (
            values["player.decoder"] != "hardware" or scale == 0):
        raise SchemaError("software output size requires scaled hardware decode")


def encode_existing_target(key, value):
    if key == "vaapi.h264.mode":
        return {"bounded": ABSENT, "strict": "0", "live": "1"}[value]
    if key in ("vaapi.h264.low_latency", "vaapi.h264.ready_remap", "vaapi.i915_wc",
               "vaapi.debug", "vaapi.trace"):
        return "1" if value else ABSENT
    if key == "libcrystalhd.rx_pool_count":
        return str(value)
    if key == "player.python":
        return value
    if key == "chromium.password_store":
        return value
    if key == "gstreamer.crystalhddec.scale_width":
        return value
    raise KeyError(key)


def _native_ready_eligible(environment):
    values = dict(NATIVE)
    origins = dict((key, "compiled") for key in RULES)
    adapt_raw_environment(values, origins, dict(
        (name, environment[name]) for name in ENVIRONMENT_TARGETS if name in environment
    ))
    return native_effect(values)[0]["vaapi.h264.ready_remap"] is True


def prepare_launch(system, user, catalog, environment, explicit_preset=None,
                   explicit_profile=None, overrides=None, acknowledgements=(),
                   native_bypass=False):
    """Build requests for a new child; never infer active application state."""
    if os.getuid() != os.geteuid() or os.getgid() != os.getegid():
        raise SchemaError("managed launch is disabled when process IDs differ")
    if overrides is None:
        overrides = {}
    if type(overrides) is not dict:
        raise SchemaError("invalid canonical launch overrides")
    preset = _selected_preset(catalog, system, user, explicit_preset)
    resolution = resolve(
        system, user, catalog.selectable, explicit_preset=preset,
        explicit_profile=explicit_profile, raw_environment=dict(
            (name, environment[name]) for name in ENVIRONMENT_TARGETS if name in environment
        ), application=overrides,
    )
    child_environment = dict(environment)
    requested_keys, required = [], set()
    for key, origin in resolution.origins.items():
        if origin == "compiled" or origin.startswith("environment:"):
            continue
        if key not in LAUNCH_ENVIRONMENT:
            raise SchemaError("run has no supported environment adapter for {0}".format(key))
        value = resolution.requested[key]
        if key in ("vaapi.h264.ready_remap", "vaapi.i915_wc") and value is True:
            raise SchemaError("run does not support activation of {0}".format(key))
        requested_keys.append(key)
        encoded = (ABSENT if origin.endswith(":unset")
                   else encode_existing_target(key, value))
        variable = LAUNCH_ENVIRONMENT[key]
        if encoded is ABSENT:
            child_environment.pop(variable, None)
        else:
            child_environment[variable] = encoded
        if key == "libcrystalhd.rx_pool_count" and encoded == "8":
            required.add("low-memory-8-buffer")
        if key in ("vaapi.h264.mode", "vaapi.h264.low_latency") and encoded == "1":
            required.add("bcm70015-live-h264-drain")
    if (not _native_ready_eligible(environment) and
            _native_ready_eligible(child_environment)):
        raise SchemaError("managed launch would enable unsupported READY remapping")
    _validated_preset(catalog, system, user, preset, acknowledgements,
                      additional_required=required)
    if (native_bypass or requested_keys or overrides or explicit_preset is not None
            or explicit_profile is not None or system.settings or user.settings
            or system.profiles or user.profiles
            or set(system.metadata) - {"schema"} or set(user.metadata) - {"schema"}):
        # The resolved child is authoritative, including absence for false/unset.
        child_environment[NATIVE_CONFIG_DISABLE_ENVIRONMENT] = "1"
    return LaunchPlan(child_environment, requested_keys)


def _launch_diagnostic(message):
    """Best-effort stderr only; real descriptors bypass Python buffering."""
    stream = sys.stderr
    if stream is None:
        return
    text = "crystalhd-config: " + message + "\n"
    try:
        try:
            descriptor = stream.fileno()
        except (AttributeError, OSError, ValueError):
            stream.write(text)
            stream.flush()
        else:
            os.write(descriptor, text.encode("utf-8"))
    except Exception:
        pass


def execute_launch(plan, child_argv):
    """Exec a literal argv; on failure restore Python's ignored signals."""
    changed = {}
    failure = None
    try:
        summary = ", ".join(plan.requested_keys) or "inherited environment unchanged"
        if plan.environment.get(NATIVE_CONFIG_DISABLE_ENVIRONMENT) == "1":
            summary = (", ".join(plan.requested_keys) or "no setting overrides")
            summary += "; native file reading bypassed"
        _launch_diagnostic("REQUESTED FOR THIS CHILD: {0}; "
                           "active state unconfirmed; existing processes unchanged".format(
                  summary
                           ))
        for name in ("SIGPIPE", "SIGXFZ", "SIGXFSZ"):
            number = getattr(signal, name, None)
            if number is not None and signal.getsignal(number) == signal.SIG_IGN:
                changed[number] = signal.SIG_IGN
                signal.signal(number, signal.SIG_DFL)
        os.execvpe(child_argv[0], child_argv, plan.environment)
    except (OSError, ValueError) as error:
        missing = isinstance(error, OSError) and error.errno == errno.ENOENT
        failure = 127 if missing else 126
    finally:
        for number, disposition in changed.items():
            signal.signal(number, disposition)
    if failure is not None:
        _launch_diagnostic("command not found" if failure == 127
                           else "command could not be executed")
    return failure


class FileFacts(object):
    def __init__(self, uid, mode, directory_uid, directory_mode, regular=True,
                 final_symlink=False, real_uid=1000, effective_uid=1000,
                 real_gid=1000, effective_gid=1000):
        self.uid = uid
        self.mode = mode
        self.directory_uid = directory_uid
        self.directory_mode = directory_mode
        self.regular = regular
        self.final_symlink = final_symlink
        self.real_uid = real_uid
        self.effective_uid = effective_uid
        self.real_gid = real_gid
        self.effective_gid = effective_gid


def secure_file(facts, system=False):
    expected_uid = 0 if system else facts.real_uid
    if not facts.regular or facts.final_symlink:
        return False
    if facts.uid != expected_uid or facts.directory_uid != expected_uid:
        return False
    if facts.mode & 0o022 or facts.directory_mode & 0o022:
        return False
    if not system and (
            facts.real_uid != facts.effective_uid
            or facts.real_gid != facts.effective_gid):
        return False
    return True


def _open_flags(directory=False):
    required = ("O_NOFOLLOW", "O_DIRECTORY", "O_NONBLOCK")
    if any(not hasattr(os, name) for name in required):
        raise SchemaError("platform lacks required secure-open flags")
    flags = os.O_RDONLY
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= os.O_NOFOLLOW
    if directory:
        flags |= os.O_DIRECTORY
    else:
        # Prevent a malicious FIFO from blocking before fstat rejects its type.
        flags |= os.O_NONBLOCK
    return flags


def _check_directory(info, role, expected_uid, allow_writable=False):
    if not stat.S_ISDIR(info.st_mode):
        raise SchemaError("{0} parent is not a directory".format(role))
    if info.st_uid != expected_uid:
        raise SchemaError("{0} directory has an unsafe owner".format(role))
    if info.st_mode & 0o022 and not allow_writable:
        raise SchemaError("{0} directory is group/other writable".format(role))


def _check_config_file(info, role, expected_uid):
    if stat.S_ISLNK(info.st_mode):
        raise SchemaError("{0} must not be a symlink".format(role))
    if not stat.S_ISREG(info.st_mode):
        raise SchemaError("{0} is not a regular file".format(role))
    if info.st_uid != expected_uid:
        raise SchemaError("{0} has an unsafe owner".format(role))
    if info.st_mode & 0o022:
        raise SchemaError("{0} is group/other writable".format(role))


def _read_trusted_file(path, role, expected_uid):
    """Return bytes or None after a descriptor-relative, bounded safe read."""
    if not os.path.isabs(path):
        raise SchemaError("{0} path must be absolute".format(role))
    parent, name = os.path.split(path)
    if not name:
        raise SchemaError("{0} path does not name a file".format(role))
    directory_fd = None
    file_fd = None
    try:
        try:
            directory_fd = os.open(parent, _open_flags(directory=True))
        except OSError as error:
            if error.errno == errno.ENOENT:
                return None
            raise SchemaError("{0} directory cannot be opened safely".format(role))

        try:
            directory_stat = os.fstat(directory_fd)
        except OSError:
            raise SchemaError("{0} directory cannot be inspected safely".format(role))
        _check_directory(directory_stat, role, expected_uid)

        # Reject device nodes, FIFOs and final symlinks by metadata before an
        # ordinary open can invoke their file operations.  The descriptor
        # check below remains authoritative if the entry changes meanwhile.
        try:
            entry_stat = os.stat(name, dir_fd=directory_fd,
                                 follow_symlinks=False)
        except OSError as error:
            if error.errno == errno.ENOENT:
                return None
            raise SchemaError("{0} cannot be inspected safely".format(role))
        if stat.S_ISLNK(entry_stat.st_mode):
            raise SchemaError("{0} must not be a symlink".format(role))
        if not stat.S_ISREG(entry_stat.st_mode):
            raise SchemaError("{0} is not a regular file".format(role))

        try:
            file_fd = os.open(name, _open_flags(), dir_fd=directory_fd)
        except OSError as error:
            if error.errno == errno.ENOENT:
                return None
            if error.errno == errno.ELOOP:
                raise SchemaError("{0} must not be a symlink".format(role))
            raise SchemaError("{0} cannot be opened safely".format(role))

        try:
            file_stat = os.fstat(file_fd)
        except OSError:
            raise SchemaError("{0} cannot be inspected safely".format(role))
        _check_config_file(file_stat, role, expected_uid)

        chunks = []
        remaining = MAXIMUM_BYTES + 1
        try:
            while remaining:
                chunk = os.read(file_fd, min(remaining, 8192))
                if not chunk:
                    break
                chunks.append(chunk)
                remaining -= len(chunk)
        except OSError:
            raise SchemaError("{0} cannot be read safely".format(role))
        data = b"".join(chunks)
        if len(data) > MAXIMUM_BYTES:
            raise SchemaError("{0} exceeds 64 KiB".format(role))
        return data
    finally:
        if file_fd is not None:
            os.close(file_fd)
        if directory_fd is not None:
            os.close(directory_fd)


def read_config(path, role, system=False):
    if not system and (
            os.getuid() != os.geteuid() or os.getgid() != os.getegid()):
        raise SchemaError("user configuration is disabled when process IDs differ")
    expected_uid = 0 if system else os.getuid()
    return _read_trusted_file(path, role, expected_uid)


def preset_manifest_path():
    relative = PRESET_MANIFEST_RELATIVE
    if relative.startswith("@") and relative.endswith("@"):
        relative = SOURCE_PRESET_MANIFEST_RELATIVE
    if (type(relative) is not str or not relative or "\0" in relative
            or os.path.isabs(relative)):
        raise SchemaError("compiled preset manifest path is invalid")
    script_path = os.path.abspath(__file__)
    try:
        script_stat = os.stat(script_path, follow_symlinks=False)
    except OSError:
        raise SchemaError("configuration inspector cannot be inspected safely")
    if not stat.S_ISREG(script_stat.st_mode) or script_stat.st_mode & 0o022:
        raise SchemaError("configuration inspector has unsafe permissions")
    return os.path.abspath(os.path.join(os.path.dirname(script_path), relative)), \
        script_stat.st_uid


def load_preset_catalog():
    path, expected_uid = preset_manifest_path()
    data = _read_trusted_file(path, "preset manifest", expected_uid)
    if data is None:
        raise SchemaError("preset manifest is missing")
    return parse_preset_manifest(data)


def default_user_config(environment):
    xdg = environment.get("XDG_CONFIG_HOME")
    if xdg:
        if not os.path.isabs(xdg):
            raise SchemaError("XDG_CONFIG_HOME must be absolute")
        return os.path.join(xdg, "crystalhd", "config.ini")
    home = environment.get("HOME")
    if home:
        if not os.path.isabs(home):
            raise SchemaError("HOME must be absolute")
        return os.path.join(home, ".config", "crystalhd", "config.ini")
    return None


def load_document(path, role, system=False):
    if path is None:
        return empty_document()
    data = read_config(path, role, system=system)
    if data is None:
        return empty_document()
    try:
        return parse_document(data)
    except SchemaError as error:
        raise SchemaError("{0}: {1}".format(role, error))


def _user_target_path(path, system_path=None):
    if os.getuid() != os.geteuid() or os.getgid() != os.getegid():
        raise SchemaError("user configuration is disabled when process IDs differ")
    if type(path) is not str or "\0" in path or not os.path.isabs(path):
        raise SchemaError("user configuration path must be absolute")
    path = os.path.normpath(path)
    if not os.path.basename(path):
        raise SchemaError("user configuration path does not name a file")
    protected = [SYSTEM_CONFIG]
    if system_path is not None:
        protected.append(system_path)
    if any(path == os.path.normpath(item) for item in protected):
        raise SchemaError("user save must not target a system configuration")
    return path


def _directory_identity(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_gid,
            stat.S_IMODE(info.st_mode))


def _file_identity(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_gid, info.st_mode,
            info.st_nlink, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _reject_system_target(directory_fd, name, system_path):
    """Also reject alternate paths to a protected directory/leaf or inode."""
    if not hasattr(os, "O_PATH"):
        raise SchemaError("platform lacks required secure-open flags")
    protected = [SYSTEM_CONFIG] + ([] if system_path is None else [system_path])
    target_directory = os.fstat(directory_fd)
    try:
        target = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as error:
        if error.errno != errno.ENOENT:
            raise
        target = None
    if target is not None:
        _check_config_file(target, "user configuration", os.getuid())
        if target.st_nlink != 1:
            raise SchemaError("user configuration must not have multiple hard links")
    for path in protected:
        parent, leaf = os.path.split(os.path.normpath(path))
        try:
            protected_fd = os.open(parent, os.O_PATH | os.O_DIRECTORY |
                                   os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0))
        except OSError as error:
            if error.errno == errno.ENOENT:
                continue
            raise SchemaError("system configuration directory cannot be inspected safely")
        try:
            info = os.fstat(protected_fd)
            if (name == leaf and (info.st_dev, info.st_ino) ==
                    (target_directory.st_dev, target_directory.st_ino)):
                raise SchemaError("user save must not target a system configuration")
            try:
                entry = os.stat(leaf, dir_fd=protected_fd, follow_symlinks=False)
            except OSError as error:
                if error.errno == errno.ENOENT:
                    continue
                # A caller-owned, single-link target cannot write through a
                # root-only protected leaf: replacement changes its own dirent.
                # Root still requires the complete protected-inode comparison.
                if error.errno == errno.EACCES and os.getuid() != 0:
                    continue
                raise
            if target is not None and (entry.st_dev, entry.st_ino) == (target.st_dev, target.st_ino):
                raise SchemaError("user save must not target a system configuration")
        finally:
            os.close(protected_fd)


def _open_user_directory(parent, create=False):
    """Walk without symlinks; create only beneath an already trusted user dir."""
    descriptor = os.open("/", _open_flags(directory=True))
    try:
        for part in parent.split("/"):
            if not part:
                continue
            try:
                child = os.open(part, _open_flags(directory=True), dir_fd=descriptor)
            except OSError as error:
                if error.errno != errno.ENOENT:
                    raise
                if not create:
                    previous, descriptor = descriptor, None
                    os.close(previous)
                    return None
                _check_directory(os.fstat(descriptor), "user configuration", os.getuid(),
                                 allow_writable=os.getuid() != 0)
                try:
                    os.mkdir(part, 0o700, dir_fd=descriptor)
                    os.fsync(descriptor)
                except OSError as creation_error:
                    if creation_error.errno != errno.EEXIST:
                        raise
                child = os.open(part, _open_flags(directory=True), dir_fd=descriptor)
            previous, descriptor = descriptor, child
            os.close(previous)
            info = os.fstat(descriptor)
            root_sticky = info.st_uid == 0 and info.st_mode & stat.S_ISVTX
            if (not stat.S_ISDIR(info.st_mode) or
                    info.st_uid not in (0, os.getuid()) or
                    (info.st_uid == 0 and info.st_mode & 0o022 and not root_sticky)):
                raise SchemaError("user configuration has an unsafe ancestor directory")
        _check_directory(os.fstat(descriptor), "user configuration", os.getuid())
        return descriptor
    except BaseException:
        if descriptor is not None:
            os.close(descriptor)
        raise


def _user_file_snapshot(directory_fd, name):
    try:
        entry = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
    except OSError as error:
        if error.errno == errno.ENOENT:
            return None, None
        raise
    _check_config_file(entry, "user configuration", os.getuid())
    if entry.st_nlink != 1:
        raise SchemaError("user configuration must not have multiple hard links")
    if entry.st_size > MAXIMUM_BYTES:
        raise SchemaError("user configuration exceeds 64 KiB")
    descriptor = os.open(name, _open_flags(), dir_fd=directory_fd)
    try:
        before = os.fstat(descriptor)
        _check_config_file(before, "user configuration", os.getuid())
        if _file_identity(before) != _file_identity(entry):
            raise SchemaError("user configuration changed during read; reload")
        chunks, remaining = [], MAXIMUM_BYTES + 1
        while remaining:
            chunk = os.read(descriptor, min(remaining, 8192))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
        if len(data) > MAXIMUM_BYTES:
            raise SchemaError("user configuration exceeds 64 KiB")
        after = os.fstat(descriptor)
        current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        identity = _file_identity(before)
        if identity != _file_identity(after) or identity != _file_identity(current):
            raise SchemaError("user configuration changed during read; reload")
        return data, identity
    finally:
        os.close(descriptor)


def _lock_user_directory(descriptor, exclusive):
    # This coordinates cooperating local CLI/tray processes, not other hosts
    # or arbitrary editors which do not use this directory lock.
    try:
        fcntl.flock(descriptor, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) |
                    fcntl.LOCK_NB)
    except OSError as error:
        if error.errno in (errno.EACCES, errno.EAGAIN):
            raise SchemaError("user configuration is busy; retry")
        raise SchemaError("user configuration cannot be locked safely")


def load_user_document_for_edit(path, reset=False):
    path = _user_target_path(path)
    directory_fd = None
    try:
        directory_fd = _open_user_directory(os.path.dirname(path))
        if directory_fd is None:
            return UserDocumentSnapshot(path, empty_document(), None, None)
        _lock_user_directory(directory_fd, False)
        identity = _directory_identity(os.fstat(directory_fd))
        data, file_identity = _user_file_snapshot(directory_fd, os.path.basename(path))
        document = empty_document() if reset or data is None else parse_document(data)
        return UserDocumentSnapshot(path, document, identity, file_identity, data)
    except OSError:
        raise SchemaError("user configuration cannot be read safely for editing")
    finally:
        if directory_fd is not None:
            try:
                os.close(directory_fd)
            except OSError:
                raise SchemaError("user configuration edit directory cannot be closed safely")


def save_user_document(path, document, expected_snapshot, system_path=None,
                       acknowledgements=(), validate_profiles=()):
    """Save an explicit overlay, rejecting stale cooperating local sessions.

    Before replacement, errors raise SchemaError and preserve existing bytes.
    Once replacement commits, return durable=False if completion is uncertain;
    never attempt to roll back over another writer's subsequent transaction.
    """
    path = _user_target_path(path, system_path)
    if (type(expected_snapshot) is not UserDocumentSnapshot or
            expected_snapshot.path != path):
        raise SchemaError("a matching user edit snapshot is required")
    data = serialize_document(document)
    candidate = parse_document(data)
    system = load_document(system_path, "system configuration", system=True)
    catalog = load_preset_catalog()
    preset = _validated_preset(catalog, system, candidate, None, acknowledgements)
    resolve(system, candidate, catalog.selectable, explicit_preset=preset,
            raw_environment={})
    for profile in validate_profiles:
        resolve(system, candidate, catalog.selectable, explicit_preset=preset,
                explicit_profile=profile, raw_environment={})
    parent, name = os.path.split(path)
    directory_fd, temporary_fd, temporary_name, temporary_inode = None, None, None, None
    committed, durable, failure = False, False, None
    replace_attempted, outcome_uncertain = False, False

    def check_current():
        current_directory = _directory_identity(os.fstat(directory_fd))
        if (expected_snapshot.directory_identity is not None and
                current_directory != expected_snapshot.directory_identity):
            raise SchemaError("stale user configuration directory; reload")
        current_fd = _open_user_directory(parent)
        try:
            if current_fd is None or _directory_identity(os.fstat(current_fd)) != current_directory:
                raise SchemaError("user configuration path changed; reload")
        finally:
            if current_fd is not None:
                os.close(current_fd)
        _reject_system_target(directory_fd, name, system_path)
        current_bytes, current_file = _user_file_snapshot(directory_fd, name)
        if (current_file != expected_snapshot.file_identity or
                current_bytes != expected_snapshot.original_bytes):
            raise SchemaError("stale user configuration session; reload")

    try:
        directory_fd = _open_user_directory(
            parent, create=expected_snapshot.directory_identity is None
        )
        if directory_fd is None:
            raise SchemaError("stale user configuration directory; reload")
        _lock_user_directory(directory_fd, True)
        check_current()
        for _attempt in range(8):
            temporary_name = ".crystalhd-config-" + secrets.token_hex(12)
            try:
                temporary_fd = os.open(
                    temporary_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                    os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0), 0o600,
                    dir_fd=directory_fd,
                )
                break
            except OSError as error:
                temporary_name = None
                if error.errno != errno.EEXIST:
                    raise
        if temporary_fd is None:
            raise SchemaError("user configuration temporary file cannot be created")
        temporary_stat = os.fstat(temporary_fd)
        temporary_inode = (temporary_stat.st_dev, temporary_stat.st_ino)
        os.fchmod(temporary_fd, 0o600)  # Only this newly created, exclusive file.
        offset = 0
        while offset < len(data):
            written = os.write(temporary_fd, data[offset:])
            if written <= 0:
                raise SchemaError("user configuration write made no progress")
            offset += written
        os.fsync(temporary_fd)
        temporary_info = os.fstat(temporary_fd)
        _check_config_file(temporary_info, "user configuration temporary file", os.getuid())
        if (stat.S_IMODE(temporary_info.st_mode) != 0o600 or
                temporary_info.st_nlink != 1 or temporary_info.st_size != len(data)):
            raise SchemaError("user configuration temporary file is unsafe")
        temporary_identity = _file_identity(temporary_info)
        descriptor, temporary_fd = temporary_fd, None
        os.close(descriptor)
        check_current()
        temporary_entry = os.stat(temporary_name, dir_fd=directory_fd, follow_symlinks=False)
        if _file_identity(temporary_entry) != temporary_identity:
            raise SchemaError("user configuration temporary file changed; retry")
        replace_attempted = True
        os.replace(temporary_name, name, src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
        committed, temporary_name = True, None
        os.fsync(directory_fd)
        durable = True
    except BaseException as error:
        if replace_attempted and not committed:
            # A signal/exception may be delivered after rename took effect but
            # before Python records success. Inspect the pinned names first.
            try:
                installed = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                committed = (installed.st_dev, installed.st_ino) == temporary_inode
            except OSError:
                committed = False
            if committed:
                temporary_name = None
            else:
                try:
                    pending = os.stat(temporary_name, dir_fd=directory_fd, follow_symlinks=False)
                    outcome_uncertain = (pending.st_dev, pending.st_ino) != temporary_inode
                except OSError:
                    outcome_uncertain = True
                if outcome_uncertain:
                    temporary_name = None  # Never remove an unrecognized entry.
        failure = (SchemaError("user configuration could not be saved safely")
                   if isinstance(error, OSError) else error)
    finally:
        for descriptor in (temporary_fd,):
            if descriptor is not None:
                try:
                    os.close(descriptor)
                except OSError:
                    failure = failure or SchemaError("user configuration cleanup failed")
        if temporary_name is not None and directory_fd is not None:
            try:
                entry = os.stat(temporary_name, dir_fd=directory_fd, follow_symlinks=False)
                if temporary_inode is None or (entry.st_dev, entry.st_ino) != temporary_inode:
                    raise SchemaError("user configuration temporary file changed during cleanup")
                os.unlink(temporary_name, dir_fd=directory_fd)
            except (OSError, SchemaError):
                failure = failure or SchemaError("user configuration cleanup failed")
        if directory_fd is not None:
            try:
                os.close(directory_fd)
            except OSError:
                failure = failure or SchemaError("user configuration cleanup failed")
    if committed:
        return SaveResult(durable and failure is None)
    if outcome_uncertain:
        return SaveResult(False, committed=None)
    if failure is not None:
        raise failure
    raise SchemaError("user configuration was not saved")


def _json_value(key, value, origin, bug_report=False):
    if value is ABSENT:
        return None
    if isinstance(value, InvalidNativeValue):
        return "<invalid-environment-value>"
    if key in PATH_KEYS and origin != "compiled":
        return "<redacted-path>"
    if key == "chromium.password_store" and origin != "compiled":
        if bug_report:
            return "<redacted-setting-value>"
        if origin.startswith("environment:"):
            return "<redacted-environment-value>"
    return value


def _report_origin(origin, bug_report):
    if bug_report:
        for prefix in ("system-profile:", "user-profile:"):
            if origin.startswith(prefix):
                suffix = ":unset" if origin.endswith(":unset") else ""
                return prefix + "<redacted>" + suffix
    return origin


def make_report(resolution, environment_observed, bug_report=False):
    receipts = {}
    for receipt in resolution.raw_receipts:
        receipts[receipt.key] = {
            "variable": receipt.variable,
            "accepted": receipt.valid,
            "raw": "<redacted>",
        }
    settings = []
    for key in sorted(RULES):
        origin = resolution.origins[key]
        settings.append({
            "key": key,
            "requested": _json_value(
                key, resolution.requested[key], origin, bug_report=bug_report
            ),
            "effective": _json_value(
                key, resolution.effective[key], origin, bug_report=bug_report
            ),
            "origin": _report_origin(origin, bug_report),
            "state": resolution.activity[key],
            "environment_receipt": receipts.get(key),
        })
    custom_override_keys = sorted(
        key for key, origin in resolution.origins.items()
        if origin not in ("compiled", "preset:" + resolution.preset)
    )
    report = {
        "schema": 1,
        "preset": resolution.preset,
        "custom_overrides": custom_override_keys,
        "profile": (
            "<redacted-selected-profile>"
            if bug_report and resolution.profile is not None
            else resolution.profile
        ),
        "environment": "observed" if environment_observed else "ignored",
        "observation_limit": (
            "process environment is the last observable layer; future "
            "application CLI and GObject overrides are not observed"
        ),
        "state_scope": (
            "configuration/native-adapter model only; no application, device, "
            "backend, kernel module, or link topology was inspected"
        ),
        "effective_configuration_scope": "canonical-native-adapters-only",
        "native_file_reading": ("bypassed-by-environment"
                                if resolution.native_config_disabled else
                                "component-dependent:not-inspected"),
        "native_only_environment": [],
        "native_rejections": [
            {"component": item.component, "reason": item.reason}
            for item in resolution.rejections
        ],
        "settings": settings,
    }
    if resolution.native_config_disabled:
        report["state_scope"] += ("; native file reading bypassed by "
                                  "CRYSTALHD_CONFIG_DISABLE=1; configured values "
                                  "remain an inspection model")
    if resolution.centered_nv12_environment_present:
        report["effective_configuration_scope"] = (
            "partial:experimental-centered-nv12-native-only"
        )
        report["state_scope"] += "; " + CENTERED_NV12_MODEL_CAVEAT
        report["native_only_environment"] = [{
            "variable": CENTERED_NV12_ENVIRONMENT,
            "raw": "<redacted>",
            "state": "not-modeled:experimental-centered-nv12",
            "native_value_validation": "not-performed",
        }]
    if bug_report:
        report["report_kind"] = "crystalhd-configuration-bug-report-v1"
        report["redaction"] = (
            "raw environment values, non-compiled paths, selected profile "
            "identifiers, and non-compiled password-store values"
        )
    return report


def _format_value(value):
    if value is None:
        return "<native-unset>"
    if type(value) is bool:
        return "true" if value else "false"
    if type(value) is str:
        return json.dumps(value, ensure_ascii=True)
    return str(value)


def print_text(report, key=None):
    print("preset: {0}".format(report["preset"]))
    print("custom overrides: {0}".format(
        ", ".join(report["custom_overrides"]) or "none"
    ))
    print("profile: {0}".format(report["profile"] or "none"))
    print("environment: {0}".format(report["environment"]))
    print("observation: {0}".format(report["observation_limit"]))
    print("state scope: {0}".format(report["state_scope"]))
    print("native file reading: {0}".format(report["native_file_reading"]))
    print("effective configuration scope: {0}".format(
        report["effective_configuration_scope"]
    ))
    for boundary in report["native_only_environment"]:
        print("native-only environment {0}: raw=<redacted>; state={1}".format(
            boundary["variable"], boundary["state"]
        ))
    selected = report["settings"]
    if key is not None:
        selected = [item for item in selected if item["key"] == key]
    for item in selected:
        line = "{0} = {1} (requested={2}; origin={3}; state={4})".format(
            item["key"], _format_value(item["effective"]),
            _format_value(item["requested"]), item["origin"], item["state"]
        )
        print(line)
        receipt = item["environment_receipt"]
        if receipt is not None:
            print("  environment {0}: raw=<redacted>; accepted={1}".format(
                receipt["variable"], "true" if receipt["accepted"] else "false"
            ))
    for rejection in report["native_rejections"]:
        print("native rejection: {0}: {1}".format(
            rejection["component"], rejection["reason"]
        ))


def print_json(report, key=None):
    output = report
    if key is not None:
        output = dict(report)
        output["settings"] = [
            item for item in report["settings"] if item["key"] == key
        ]
    print(json.dumps(output, sort_keys=True, indent=2, separators=(",", ": ")))


def print_preset_catalog(catalog, output_format):
    if output_format == "json":
        print(json.dumps({
            "configuration_schema": 1,
            "format": "crystalhd-presets",
            "format_version": 1,
            "presets": [entry.record for entry in catalog.entries],
        }, sort_keys=True, indent=2, separators=(",", ": ")))
        return
    for entry in catalog.entries:
        record = entry.record
        acknowledgements = ",".join(
            record["required_acknowledgements"]
        ) or "none"
        reason = record["reason_code"] or "none"
        print("{0}: status={1}; safety={2}; acknowledgements={3}; reason={4}".format(
            entry.name, entry.status, record["safety"], acknowledgements, reason
        ))
        settings = record["settings"]
        if settings is not None:
            for key in sorted(settings):
                print("  {0} = {1}".format(
                    key, json.dumps(settings[key], ensure_ascii=True)
                ))
        for requirement in record["requirements"]:
            print("  requires {0}: {1}".format(
                requirement["id"], requirement["description"]
            ))


def add_common_options(parser, observe_environment=True):
    system = parser.add_mutually_exclusive_group()
    system.add_argument(
        "--system-config", metavar="FILE",
        help="read the system layer from this absolute path",
    )
    system.add_argument(
        "--no-system-config", action="store_true",
        help="do not read a system layer",
    )
    user = parser.add_mutually_exclusive_group()
    user.add_argument(
        "--user-config", metavar="FILE",
        help="read the user layer from this absolute path",
    )
    user.add_argument(
        "--no-user-config", action="store_true",
        help="do not read a user layer",
    )
    parser.add_argument(
        "--preset", metavar="NAME", help="select a preset explicitly",
    )
    parser.add_argument(
        "--profile", metavar="NAME", help="select a profile explicitly",
    )
    if observe_environment:
        parser.add_argument(
            "--ignore-environment", action="store_true",
            help="ignore setting-specific process environment variables",
        )
    parser.add_argument(
        "--acknowledge-risk", action="append", default=[], metavar="TOKEN",
        help="acknowledge one selected experimental-preset risk",
    )


def add_write_options(parser):
    system = parser.add_mutually_exclusive_group()
    system.add_argument("--system-config", metavar="FILE", action=SingleWriteOption,
                        help="read this absolute system layer for validation only")
    system.add_argument("--no-system-config", action="store_true",
                        help="validate without a system layer")
    parser.add_argument("--user-config", metavar="FILE", action=SingleWriteOption,
                        help="write only this absolute user overlay path")
    parser.add_argument("--acknowledge-risk", action="append", default=[], metavar="TOKEN",
                        help="acknowledge a selected experimental preset for this save only")


def make_argument_parser():
    parser = RedactedArgumentParser(
        prog="crystalhd-config",
        description="inspect/save CrystalHD defaults or explicitly launch with supported requests",
    )
    subparsers = parser.add_subparsers(dest="command")
    subparsers.required = True

    listing = subparsers.add_parser(
        "list-presets", help="list reviewed preset manifest entries"
    )
    listing.add_argument("--format", choices=("text", "json"), default="text")

    installation = subparsers.add_parser("installation", help="inspect installed reader declarations without loading components")
    installation.add_argument("--format", choices=("text", "json"), default="text")

    run = subparsers.add_parser(
        "run", help="request supported settings for a new child; does not persist",
        usage="%(prog)s [options] -- COMMAND [ARG ...]",
    )
    add_common_options(run, observe_environment=False)
    run.add_argument("--override", action="append", default=[], metavar="KEY=VALUE",
                     help="override only this key for the child using strict value syntax")

    save = subparsers.add_parser("save", help="save explicit user defaults; does not apply them")
    add_write_options(save)
    save.add_argument("assignments", nargs="*", metavar="KEY=VALUE",
                      help="patch settings using strict INI value syntax")
    save.add_argument("--profile-section", action=SingleWriteOption, metavar="NAME",
                      help="edit/create this user profile section instead of base settings")
    save.add_argument("--remove", action="append", default=[], metavar="KEY",
                      help="omit a targeted setting or preset/profile selector; differs from unset")
    save.add_argument("--preset", action=SingleWriteOption, metavar="NAME",
                      help="persist this preset selector without expanding its settings")
    save.add_argument("--profile", action=SingleWriteOption, metavar="NAME",
                      help="persist this profile selector; does not edit its section")
    reset = subparsers.add_parser("reset", help="save an empty user overlay; does not apply it")
    add_write_options(reset)
    delete = subparsers.add_parser("delete-profile", help="remove only a user profile section")
    add_write_options(delete)
    delete.add_argument("name", metavar="NAME")

    validate = subparsers.add_parser("validate", help="validate selected configuration")
    add_common_options(validate)

    show = subparsers.add_parser("show-effective", help="show all effective settings")
    add_common_options(show)
    show.add_argument("--format", choices=("text", "json"), default="text")

    bug_report = subparsers.add_parser(
        "bug-report", help="export a redacted configuration-only JSON report"
    )
    add_common_options(bug_report)

    explain = subparsers.add_parser("explain", help="explain one canonical setting")
    add_common_options(explain)
    explain.add_argument("key", metavar="KEY")
    explain.add_argument("--format", choices=("text", "json"), default="text")
    return parser


def _check_acknowledgements(catalog, acknowledgements):
    if any(type(token) is not str for token in acknowledgements):
        raise CommandLineSelectionError("invalid risk acknowledgement")
    if len(acknowledgements) != len(set(acknowledgements)):
        raise CommandLineSelectionError("duplicate risk acknowledgement")
    if set(acknowledgements) - catalog.acknowledgement_tokens:
        raise CommandLineSelectionError("unknown risk acknowledgement")


def _selected_preset(catalog, system, user, selected_preset):
    if selected_preset is None:
        selected_preset = user.metadata.get(
            "preset", system.metadata.get("preset", "default")
        )
    if type(selected_preset) is not str or PROFILE_NAME.fullmatch(selected_preset) is None:
        raise SchemaError("invalid selected preset")
    entry = catalog.by_name.get(selected_preset)
    if entry is None:
        raise SchemaError("unknown selected preset")
    if entry.status == "UNAVAILABLE":
        raise SchemaError("selected preset is unavailable: {0}".format(
            entry.record["reason_code"]
        ))
    if entry.status == "VIRTUAL":
        raise SchemaError("virtual custom preset cannot be selected")
    return selected_preset


def _validated_preset(catalog, system, user, selected_preset, acknowledgements,
                      additional_required=()):
    _check_acknowledgements(catalog, acknowledgements)
    selected_preset = _selected_preset(catalog, system, user, selected_preset)
    required = (set(catalog.by_name[selected_preset].required_acknowledgements) |
                set(additional_required))
    supplied = set(acknowledgements)
    if required - supplied:
        raise SchemaError("{0} requires --acknowledge-risk {1}".format(
            "managed launch" if additional_required else "selected preset",
            sorted(required - supplied)[0],
        ))
    if supplied - required:
        raise SchemaError("risk acknowledgement does not apply to selected preset")
    return selected_preset


def _argument_documents(arguments, environment):
    if arguments.no_system_config:
        system_path = None
    else:
        system_path = (arguments.system_config
                       if arguments.system_config is not None
                       else SYSTEM_CONFIG)
    if arguments.no_user_config:
        user_path = None
    else:
        user_path = (arguments.user_config
                     if arguments.user_config is not None
                     else default_user_config(environment))

    system = load_document(system_path, "system configuration", system=True)
    user = load_document(user_path, "user configuration", system=False)
    return system, user


def resolve_arguments(arguments, environment):
    catalog = load_preset_catalog()
    acknowledgements = arguments.acknowledge_risk
    _check_acknowledgements(catalog, acknowledgements)
    system, user = _argument_documents(arguments, environment)
    selected_preset = _validated_preset(
        catalog, system, user, arguments.preset, acknowledgements
    )
    raw_environment = {}
    if not arguments.ignore_environment:
        raw_environment = dict(
            (name, environment[name])
            for name in ENVIRONMENT_TARGETS | {CENTERED_NV12_ENVIRONMENT,
                                              NATIVE_CONFIG_DISABLE_ENVIRONMENT}
            if name in environment
        )
    return resolve(
        system,
        user,
        catalog.selectable,
        explicit_preset=selected_preset,
        explicit_profile=arguments.profile,
        raw_environment=raw_environment,
    )


def _parse_setting_assignments(assignments):
    keys = set()
    lines = ["[crystalhd]", "schema = 1", "[settings]"]
    for assignment in assignments:
        if "=" not in assignment or len(assignment.splitlines()) != 1:
            raise CommandLineSelectionError("invalid assignment")
        key, value = assignment.split("=", 1)
        key = key.strip()
        if key not in RULES or key in keys:
            raise CommandLineSelectionError("unknown or duplicated assignment")
        keys.add(key)
        lines.append("{0} = {1}".format(key, value.strip()))
    return parse_document(("\n".join(lines) + "\n").encode("utf-8")).settings


def patch_user_settings(document, assignments=(), removals=(), profile=None):
    """Return a new draft; selecting an editing target never selects a default."""
    if profile is not None and (type(profile) is not str or PROFILE_NAME.fullmatch(profile) is None):
        raise SchemaError("invalid profile name: use 1-64 ASCII letters/digits/._-, starting with a letter/digit")
    patch = _parse_setting_assignments(assignments)
    if (len(removals) != len(set(removals)) or set(removals) & set(patch)
            or any(key not in RULES for key in removals)):
        raise CommandLineSelectionError("invalid or conflicting setting removal")
    candidate = parse_document(serialize_document(document))
    target = candidate.settings if profile is None else candidate.profiles.setdefault(profile, {})
    target.update(patch)
    for key in removals:
        target.pop(key, None)
    return parse_document(serialize_document(candidate))


def delete_user_profile(document, name, system):
    """Remove a user section only; inherited system profiles remain intact."""
    candidate = parse_document(serialize_document(document))
    if type(name) is not str or name not in candidate.profiles:
        raise SchemaError("user profile does not exist")
    del candidate.profiles[name]
    if candidate.metadata.get("profile") == name and name not in system.profiles:
        candidate.metadata.pop("profile")
    return candidate


def _patch_user_document(document, arguments):
    if not (arguments.assignments or arguments.remove or
            arguments.preset is not None or arguments.profile is not None or
            arguments.profile_section is not None):
        raise CommandLineSelectionError("save requires an explicit change")
    removals = arguments.remove
    if (len(removals) != len(set(removals)) or
            any(key not in RULES and key not in ("preset", "profile") for key in removals)):
        raise CommandLineSelectionError("invalid or conflicting removal")
    for selector in ("preset", "profile"):
        if getattr(arguments, selector) is not None and selector in removals:
            raise CommandLineSelectionError("selector is both supplied and removed")
    candidate = patch_user_settings(document, arguments.assignments,
                                    [key for key in removals if key in RULES],
                                    arguments.profile_section)
    for key in removals:
        if key in ("preset", "profile"):
            candidate.metadata.pop(key, None)
    for selector in ("preset", "profile"):
        value = getattr(arguments, selector)
        if value is not None:
            candidate.metadata[selector] = value
    return candidate


def write_arguments(arguments, environment):
    system_path = None if arguments.no_system_config else (
        arguments.system_config if arguments.system_config is not None else SYSTEM_CONFIG
    )
    user_path = (arguments.user_config if arguments.user_config is not None
                 else default_user_config(environment))
    path = _user_target_path(user_path, system_path)
    snapshot = load_user_document_for_edit(path, reset=arguments.command == "reset")
    profiles = ()
    if arguments.command == "reset":
        candidate = empty_document()
    elif arguments.command == "delete-profile":
        system = load_document(system_path, "system configuration", system=True)
        candidate = delete_user_profile(snapshot.document, arguments.name, system)
        if arguments.name in system.profiles:
            profiles = (arguments.name,)
    else:
        candidate = _patch_user_document(snapshot.document, arguments)
        if arguments.profile_section is not None:
            profiles = (arguments.profile_section,)
    return save_user_document(path, candidate, snapshot, system_path=system_path,
                              acknowledgements=arguments.acknowledge_risk,
                              validate_profiles=profiles)


def main(argv=None, environment=None):
    parser = make_argument_parser()
    raw_arguments = list(sys.argv[1:] if argv is None else argv)
    child_argv = None
    if raw_arguments and raw_arguments[0] == "run" and "--" in raw_arguments:
        separator = raw_arguments.index("--")
        child_argv = raw_arguments[separator + 1:]
        raw_arguments = raw_arguments[:separator]
    arguments = parser.parse_args(raw_arguments)
    if arguments.command == "run" and (not child_argv or not child_argv[0]):
        parser.error("run requires -- COMMAND")
    if arguments.command == "explain" and arguments.key not in RULES:
        parser.error("unknown canonical key")

    process_environment = os.environ if environment is None else environment
    if arguments.command == "installation":
        report = inspect_installation(process_environment)
        if arguments.format == "json":
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            print(installation_text(report))
        return 0
    if arguments.command == "list-presets":
        try:
            catalog = load_preset_catalog()
        except SchemaError as error:
            print("crystalhd-config: {0}".format(error), file=sys.stderr)
            return 3
        print_preset_catalog(catalog, arguments.format)
        return 0
    try:
        if arguments.command == "run":
            system, user = _argument_documents(arguments, process_environment)
            launch = prepare_launch(
                system, user, load_preset_catalog(), process_environment,
                explicit_preset=arguments.preset, explicit_profile=arguments.profile,
                overrides=_parse_setting_assignments(arguments.override),
                acknowledgements=arguments.acknowledge_risk,
                native_bypass=(arguments.no_system_config or arguments.no_user_config
                               or arguments.system_config is not None
                               or arguments.user_config is not None),
            )
        elif arguments.command in ("save", "reset", "delete-profile"):
            saved = write_arguments(arguments, process_environment)
        else:
            resolution = resolve_arguments(arguments, process_environment)
    except CommandLineSelectionError:
        parser.error("invalid manifest-backed command-line selection")
    except SchemaError as error:
        print("crystalhd-config: {0}".format(error), file=sys.stderr)
        return 3

    if arguments.command == "run":
        return execute_launch(launch, child_argv)

    if arguments.command in ("save", "reset", "delete-profile"):
        if not saved.durable:
            outcome = ("replacement committed; durability uncertain"
                       if saved.committed else "replacement outcome uncertain; inspect saved state")
            print("crystalhd-config: user configuration {0}; "
                  "SAVED ONLY, applications unchanged".format(outcome), file=sys.stderr)
            return 4
        print("user configuration SAVED ONLY (0600); running applications unchanged; "
              "updated native defaults read at next start")
        return 0

    observed = not arguments.ignore_environment
    if arguments.command == "validate":
        print("configuration valid: preset={0} profile={1}; environment={2}".format(
            resolution.preset,
            resolution.profile or "none",
            "observed" if observed else "ignored",
        ))
        if resolution.centered_nv12_environment_present:
            print("native-only environment {0}: raw=<redacted>; {1}; "
                  "canonical validation does not validate this experiment".format(
                      CENTERED_NV12_ENVIRONMENT, CENTERED_NV12_MODEL_CAVEAT
                  ))
        return 0

    report = make_report(
        resolution, observed, bug_report=arguments.command == "bug-report"
    )
    if arguments.command == "bug-report":
        print_json(report)
        return 0
    key = arguments.key if arguments.command == "explain" else None
    if arguments.format == "json":
        print_json(report, key)
    else:
        print_text(report, key)
    return 0


if __name__ == "__main__":
    sys.exit(main())
