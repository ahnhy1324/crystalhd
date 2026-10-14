#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Read-only CrystalHD v1 configuration inspector.

This program deliberately has no write, launch, module-reload, or hardware
operation.  It uses only the Python standard library and remains compatible
with Python 3.6.
"""

import argparse
import errno
import json
import os
import re
import stat
import sys


MAXIMUM_BYTES = 64 * 1024
MAXIMUM_INTEGER_DIGITS = 10
SYSTEM_CONFIG = "/etc/crystalhd/config.ini"
PROFILE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
INTEGER = re.compile(r"(?:0|[1-9][0-9]*)\Z")
KEY_NAME = re.compile(r"[a-z][a-z0-9_.]*\Z")


class SchemaError(ValueError):
    """The managed configuration cannot be accepted as one transaction."""


class RedactedArgumentParser(argparse.ArgumentParser):
    """Keep command-line syntax failures deterministic and non-reflecting."""

    def error(self, message):
        del message
        self.print_usage(sys.stderr)
        self.exit(2, "{0}: error: invalid command line\n".format(self.prog))


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
    return lambda value: type(value) is int and value in allowed


def string_enum(*accepted):
    allowed = frozenset(accepted)
    return lambda value: type(value) is str and value in allowed


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

# Presets are reviewed data, not magic behavior.  The first implementation
# intentionally ships only the empty compatibility preset.
PRESETS = {"default": {}}

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
                 rejections, preset, profile):
        self.requested = requested
        self.origins = origins
        self.effective = effective
        self.activity = activity
        self.raw_receipts = raw_receipts
        self.rejections = rejections
        self.preset = preset
        self.profile = profile


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
        activity["vaapi.h264.low_latency"] = "disabled"
    elif effective["vaapi.h264.mode"] != "live":
        effective["vaapi.h264.low_latency"] = False
        activity["vaapi.h264.low_latency"] = "inactive:requires-live-fast-path"

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
    elif not effective["vaapi.h264.low_latency"]:
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
            "conditional:not-inspected:bcm70015-live-path"
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
    receipts = adapt_raw_environment(requested, origins, raw_environment or {})
    apply_layer(requested, origins, application or {}, "application")
    effective, activity, rejections = native_effect(requested)
    return Resolution(requested, origins, effective, activity, receipts,
                      rejections, preset, profile)


def validate_managed(values):
    if values["vaapi.h264.low_latency"] and values["vaapi.h264.mode"] != "live":
        raise SchemaError("low latency requires live H.264 mode")
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
    if key == "vaapi.debug":
        return "1" if value else ABSENT
    if key == "chromium.password_store":
        return value
    if key == "gstreamer.crystalhddec.scale_width":
        return value
    raise KeyError(key)


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


def read_config(path, role, system=False):
    """Return bytes or None after a descriptor-relative, bounded safe read."""
    if not system and (
            os.getuid() != os.geteuid() or os.getgid() != os.getegid()):
        raise SchemaError("user configuration is disabled when process IDs differ")

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
        expected_uid = 0 if system else os.getuid()
        if not stat.S_ISDIR(directory_stat.st_mode):
            raise SchemaError("{0} parent is not a directory".format(role))
        if directory_stat.st_uid != expected_uid:
            raise SchemaError("{0} directory has an unsafe owner".format(role))
        if directory_stat.st_mode & 0o022:
            raise SchemaError("{0} directory is group/other writable".format(role))

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
        if not stat.S_ISREG(file_stat.st_mode):
            raise SchemaError("{0} is not a regular file".format(role))
        if file_stat.st_uid != expected_uid:
            raise SchemaError("{0} has an unsafe owner".format(role))
        if file_stat.st_mode & 0o022:
            raise SchemaError("{0} is group/other writable".format(role))

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


def _json_value(key, value, origin):
    if value is ABSENT:
        return None
    if isinstance(value, InvalidNativeValue):
        return "<invalid-environment-value>"
    if key in PATH_KEYS and origin != "compiled":
        return "<redacted-path>"
    if key == "chromium.password_store" and origin.startswith("environment:"):
        return "<redacted-environment-value>"
    return value


def make_report(resolution, environment_observed):
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
            "requested": _json_value(key, resolution.requested[key], origin),
            "effective": _json_value(key, resolution.effective[key], origin),
            "origin": origin,
            "state": resolution.activity[key],
            "environment_receipt": receipts.get(key),
        })
    return {
        "schema": 1,
        "preset": resolution.preset,
        "profile": resolution.profile,
        "environment": "observed" if environment_observed else "ignored",
        "observation_limit": (
            "process environment is the last observable layer; future "
            "application CLI and GObject overrides are not observed"
        ),
        "state_scope": (
            "configuration/native-adapter model only; no application, device, "
            "backend, kernel module, or link topology was inspected"
        ),
        "native_rejections": [
            {"component": item.component, "reason": item.reason}
            for item in resolution.rejections
        ],
        "settings": settings,
    }


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
    print("profile: {0}".format(report["profile"] or "none"))
    print("environment: {0}".format(report["environment"]))
    print("observation: {0}".format(report["observation_limit"]))
    print("state scope: {0}".format(report["state_scope"]))
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


def add_common_options(parser):
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
    parser.add_argument(
        "--ignore-environment", action="store_true",
        help="ignore setting-specific process environment variables",
    )


def make_argument_parser():
    parser = RedactedArgumentParser(
        prog="crystalhd-config",
        description="validate and inspect CrystalHD v1 configuration",
    )
    subparsers = parser.add_subparsers(dest="command")
    subparsers.required = True

    validate = subparsers.add_parser("validate", help="validate selected configuration")
    add_common_options(validate)

    show = subparsers.add_parser("show-effective", help="show all effective settings")
    add_common_options(show)
    show.add_argument("--format", choices=("text", "json"), default="text")

    explain = subparsers.add_parser("explain", help="explain one canonical setting")
    add_common_options(explain)
    explain.add_argument("key", metavar="KEY")
    explain.add_argument("--format", choices=("text", "json"), default="text")
    return parser


def resolve_arguments(arguments, environment):
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
    raw_environment = {}
    if not arguments.ignore_environment:
        raw_environment = dict(
            (name, environment[name])
            for name in ENVIRONMENT_TARGETS if name in environment
        )
    return resolve(
        system,
        user,
        PRESETS,
        explicit_preset=arguments.preset,
        explicit_profile=arguments.profile,
        raw_environment=raw_environment,
    )


def main(argv=None, environment=None):
    parser = make_argument_parser()
    arguments = parser.parse_args(argv)
    if arguments.command == "explain" and arguments.key not in RULES:
        parser.error("unknown canonical key")

    process_environment = os.environ if environment is None else environment
    try:
        resolution = resolve_arguments(arguments, process_environment)
    except SchemaError as error:
        print("crystalhd-config: {0}".format(error), file=sys.stderr)
        return 3

    observed = not arguments.ignore_environment
    if arguments.command == "validate":
        print("configuration valid: preset={0} profile={1}; environment={2}".format(
            resolution.preset,
            resolution.profile or "none",
            "observed" if observed else "ignored",
        ))
        return 0

    report = make_report(resolution, observed)
    key = arguments.key if arguments.command == "explain" else None
    if arguments.format == "json":
        print_json(report, key)
    else:
        print_text(report, key)
    return 0


if __name__ == "__main__":
    sys.exit(main())
