#!/usr/bin/env python3
"""Executable oracle for CONFIGURATION-SCHEMA.md.

This test intentionally keeps its parser and resolver inside the test suite.
Production code does not import it, and passing these checks does not mean that
the proposed configuration manager or managed Chromium mode exists.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import stat
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
INVENTORY = ROOT / "CONFIGURATION.md"
SPECIFICATION = ROOT / "CONFIGURATION-SCHEMA.md"
MAXIMUM_BYTES = 64 * 1024

INVENTORY_MARKER = re.compile(
    r"<!--\s*config-surface:(module|environment|gst-property|player-flag):"
    r"([^\s]+)\s*-->"
)
SCHEMA_KEY_MARKER = re.compile(r"<!--\s*schema-key:([a-z][a-z0-9_.]+)\s*-->")
SCHEMA_MAP_MARKER = re.compile(
    r"<!--\s*schema-map:(module|environment|gst-property|player-flag):"
    r"([^\s=]+)=([a-z][a-z0-9_.]+)\s*-->"
)
SCHEMA_BOUNDARY_MARKER = re.compile(
    r"<!--\s*schema-boundary:(module|environment|gst-property|player-flag):"
    r"([^\s=]+)=([a-z][a-z0-9-]+)\s*-->"
)
PROFILE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
INTEGER = re.compile(r"(?:0|[1-9][0-9]*)\Z")
KEY_NAME = re.compile(r"[a-z][a-z0-9_.]*\Z")


class SchemaError(ValueError):
    pass


class _Unset:
    def __repr__(self) -> str:
        return "unset"


class _Absent:
    def __repr__(self) -> str:
        return "absent"


UNSET = _Unset()
ABSENT = _Absent()


def boolean(value: object) -> bool:
    return type(value) is bool


def integer_enum(*accepted: int):
    allowed = frozenset(accepted)
    return lambda value: type(value) is int and value in allowed


def string_enum(*accepted: str):
    allowed = frozenset(accepted)
    return lambda value: type(value) is str and value in allowed


def nonempty_string(value: object) -> bool:
    return type(value) is str and bool(value) and "\0" not in value


def any_string(value: object) -> bool:
    return type(value) is str and "\0" not in value


def scale_width(value: object) -> bool:
    return type(value) is int and (
        value == 0 or 128 <= value <= 1918 and value % 2 == 0
    )


def output_size(value: object) -> bool:
    if type(value) is not str:
        return False
    found = re.fullmatch(r"([1-9][0-9]*)x([1-9][0-9]*)", value)
    if found is None:
        return False
    width, height = map(int, found.groups())
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


@dataclass
class Document:
    metadata: dict[str, object] = field(default_factory=dict)
    settings: dict[str, object] = field(default_factory=dict)
    profiles: dict[str, dict[str, object]] = field(default_factory=dict)


@dataclass(frozen=True)
class InvalidNativeValue:
    """A raw public-interface value rejected by its native adapter."""

    raw: str
    fallback: object


@dataclass(frozen=True)
class RawReceipt:
    variable: str
    key: str
    raw: str
    requested: object
    valid: bool


@dataclass(frozen=True)
class NativeRejection:
    component: str
    reason: str


@dataclass
class Resolution:
    requested: dict[str, object]
    origins: dict[str, str]
    effective: dict[str, object]
    activity: dict[str, str]
    raw_receipts: tuple[RawReceipt, ...]
    rejections: tuple[NativeRejection, ...]
    preset: str
    profile: str | None


def parse_value(text: str, line_number: int) -> object:
    if text == "true":
        return True
    if text == "false":
        return False
    if text == "unset":
        return UNSET
    if INTEGER.fullmatch(text):
        return int(text)
    if text.startswith('"'):
        try:
            value = json.loads(text)
        except json.JSONDecodeError as error:
            raise SchemaError(f"line {line_number}: invalid string: {error.msg}") from error
        if type(value) is not str or "\0" in value:
            raise SchemaError(f"line {line_number}: invalid string value")
        return value
    raise SchemaError(f"line {line_number}: invalid value syntax")


def parse_document(data: bytes) -> Document:
    if len(data) > MAXIMUM_BYTES:
        raise SchemaError("configuration exceeds 64 KiB")
    if data.startswith(b"\xef\xbb\xbf"):
        raise SchemaError("UTF-8 byte-order mark is not permitted")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise SchemaError("configuration is not UTF-8") from error

    document = Document()
    current: tuple[str, str | None] | None = None
    seen_sections: set[str] = set()
    seen_keys: dict[str, set[str]] = {}

    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            if section in seen_sections:
                raise SchemaError(f"line {line_number}: duplicate section {section}")
            if section in {"crystalhd", "settings"}:
                current = (section, None)
            elif section.startswith("profile.") and PROFILE_NAME.fullmatch(
                section.removeprefix("profile.")
            ):
                profile = section.removeprefix("profile.")
                current = ("profile", profile)
                document.profiles[profile] = {}
            else:
                raise SchemaError(f"line {line_number}: unknown section {section}")
            seen_sections.add(section)
            seen_keys[section] = set()
            continue
        if current is None or "=" not in raw_line:
            raise SchemaError(f"line {line_number}: key outside a section")

        key_text, value_text = raw_line.split("=", 1)
        key, value_source = key_text.strip(), value_text.strip()
        section_kind, profile = current
        section_name = (
            section_kind if profile is None else f"profile.{profile}"
        )
        if not KEY_NAME.fullmatch(key):
            raise SchemaError(f"line {line_number}: invalid key {key}")
        if key in seen_keys[section_name]:
            raise SchemaError(f"line {line_number}: duplicate key {key}")
        seen_keys[section_name].add(key)
        value = parse_value(value_source, line_number)

        if section_kind == "crystalhd":
            if key not in {"schema", "preset", "profile"}:
                raise SchemaError(f"line {line_number}: unknown metadata key {key}")
            if key == "schema":
                if type(value) is not int or value != 1:
                    raise SchemaError(f"line {line_number}: unsupported schema version")
            elif type(value) is not str or PROFILE_NAME.fullmatch(value) is None:
                raise SchemaError(f"line {line_number}: invalid selector {key}")
            document.metadata[key] = value
            continue

        if key not in RULES:
            raise SchemaError(f"line {line_number}: unknown setting {key}")
        if value is not UNSET and not RULES[key](value):
            raise SchemaError(f"line {line_number}: invalid value for {key}")
        target = document.settings if section_kind == "settings" else document.profiles[profile]
        target[key] = value

    if "crystalhd" not in seen_sections or document.metadata.get("schema") != 1:
        raise SchemaError("missing [crystalhd] schema = 1")
    return document


def apply_layer(
    values: dict[str, object],
    origins: dict[str, str],
    layer: dict[str, object],
    origin: str,
) -> None:
    for key, value in layer.items():
        if key not in RULES:
            raise SchemaError(f"unknown canonical key in {origin}: {key}")
        if value is UNSET:
            values[key] = NATIVE[key]
            origins[key] = f"{origin}:unset"
        elif not RULES[key](value):
            raise SchemaError(f"invalid canonical value in {origin}: {key}")
        else:
            values[key] = value
            origins[key] = origin


def adapt_raw_environment(
    values: dict[str, object],
    origins: dict[str, str],
    environment: dict[str, str],
) -> tuple[RawReceipt, ...]:
    """Apply existing environment semantics without parsing them as strict INI."""
    receipts: list[RawReceipt] = []
    for variable, raw in environment.items():
        if type(raw) is not str or "\0" in raw:
            raise SchemaError(
                f"test oracle requires a string environment value: {variable}"
            )

    def apply(variable: str, key: str, requested: object, valid: bool = True) -> None:
        if variable not in environment:
            return
        raw = environment[variable]
        values[key] = requested
        origins[key] = f"environment:{variable}"
        receipts.append(RawReceipt(variable, key, raw, requested, valid))

    variable = "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT"
    if variable in environment:
        raw = environment[variable]
        if raw in {"8", "16"}:
            apply(variable, "libcrystalhd.rx_pool_count", int(raw))
        else:
            apply(
                variable,
                "libcrystalhd.rx_pool_count",
                InvalidNativeValue(raw, 16),
                False,
            )

    variable = "CRYSTALHD_VAAPI_LIVE_H264"
    if variable in environment:
        raw = environment[variable]
        apply(variable, "vaapi.h264.mode", "live" if raw == "1" else "strict")

    for variable, key in (
        ("CRYSTALHD_VAAPI_LOW_LATENCY_H264", "vaapi.h264.low_latency"),
        ("CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP", "vaapi.h264.ready_remap"),
        ("CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC", "vaapi.i915_wc"),
        ("CRYSTALHD_VAAPI_TRACE", "vaapi.trace"),
        ("CRYSTALHD_CHROMIUM_NATIVE_GL", "chromium.native_gl"),
        (
            "CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE",
            "chromium.experimental_hw_decode",
        ),
        (
            "CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX",
            "chromium.disable_gpu_sandbox",
        ),
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
        requested: object = raw
        valid = raw in {"previous", "latest"}
        if not valid:
            requested = InvalidNativeValue(raw, "previous")
        apply(variable, "vaapi.h264.ready_policy", requested, valid)

    variable = "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US"
    if variable in environment:
        raw = environment[variable]
        if raw in {"0", "16667", "33333", "50000"}:
            apply(variable, "vaapi.h264.ready_delay_us", int(raw))
        else:
            apply(
                variable,
                "vaapi.h264.ready_delay_us",
                InvalidNativeValue(raw, 0),
                False,
            )

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


def native_effect(
    requested: dict[str, object],
) -> tuple[dict[str, object], dict[str, str], tuple[NativeRejection, ...]]:
    """Model production adapters needed to explain requested versus effective."""
    effective = {
        key: value.fallback if isinstance(value, InvalidNativeValue) else value
        for key, value in requested.items()
    }
    activity = {key: "active" for key in requested}
    rejections: list[NativeRejection] = []

    low_latency_requested = requested["vaapi.h264.low_latency"] is True
    if not low_latency_requested:
        effective["vaapi.h264.low_latency"] = False
        activity["vaapi.h264.low_latency"] = "disabled"
    elif effective["vaapi.h264.mode"] != "live":
        effective["vaapi.h264.low_latency"] = False
        activity["vaapi.h264.low_latency"] = "inactive:requires-live-fast-path"

    invalid_policy = isinstance(
        requested["vaapi.h264.ready_policy"], InvalidNativeValue
    )
    invalid_delay = isinstance(
        requested["vaapi.h264.ready_delay_us"], InvalidNativeValue
    )
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

    if (
        effective["chromium.experimental_hw_decode"]
        and not effective["chromium.disable_gpu_sandbox"]
    ):
        reason = "missing-gpu-sandbox-acknowledgement"
        activity["chromium.experimental_hw_decode"] = f"rejected:{reason}"
        rejections.append(NativeRejection("crystalhd-chromium", reason))

    return effective, activity, tuple(rejections)


def resolve(
    system: Document,
    user: Document,
    presets: dict[str, dict[str, object]],
    *,
    explicit_preset: str | None = None,
    explicit_profile: str | None = None,
    raw_environment: dict[str, str] | None = None,
    application: dict[str, object] | None = None,
) -> Resolution:
    preset = explicit_preset
    if preset is None:
        preset = user.metadata.get("preset", system.metadata.get("preset", "default"))
    profile = explicit_profile
    if profile is None:
        profile = user.metadata.get("profile", system.metadata.get("profile"))
    if preset not in presets:
        raise SchemaError(f"unknown selected preset: {preset}")
    if profile is not None and profile not in system.profiles and profile not in user.profiles:
        raise SchemaError(f"unknown selected profile: {profile}")

    managed = dict(NATIVE)
    managed_origins = {key: "compiled" for key in RULES}
    apply_layer(managed, managed_origins, presets[preset], f"preset:{preset}")
    apply_layer(managed, managed_origins, system.settings, "system")
    apply_layer(managed, managed_origins, user.settings, "user")
    if profile is not None:
        apply_layer(
            managed,
            managed_origins,
            system.profiles.get(profile, {}),
            f"system-profile:{profile}",
        )
        apply_layer(
            managed,
            managed_origins,
            user.profiles.get(profile, {}),
            f"user-profile:{profile}",
        )

    # Strict canonical inputs are one transaction. Raw public environment
    # strings cannot make an otherwise invalid managed transaction valid.
    canonical = dict(managed)
    canonical_origins = dict(managed_origins)
    apply_layer(canonical, canonical_origins, application or {}, "application")
    validate_managed(canonical)

    requested = dict(managed)
    origins = dict(managed_origins)
    receipts = adapt_raw_environment(requested, origins, raw_environment or {})
    apply_layer(requested, origins, application or {}, "application")
    effective, activity, rejections = native_effect(requested)
    return Resolution(
        requested=requested,
        origins=origins,
        effective=effective,
        activity=activity,
        raw_receipts=receipts,
        rejections=rejections,
        preset=preset,
        profile=profile,
    )


def validate_managed(values: dict[str, object]) -> None:
    if values["vaapi.h264.low_latency"] and values["vaapi.h264.mode"] != "live":
        raise SchemaError("low latency requires live H.264 mode")
    if values["vaapi.h264.ready_remap"] and (
        values["vaapi.h264.mode"] != "live"
        or not values["vaapi.h264.low_latency"]
    ):
        raise SchemaError("READY remap requires live low-latency H.264")
    if values["chromium.experimental_hw_decode"] and not values[
        "chromium.disable_gpu_sandbox"
    ]:
        raise SchemaError("Chromium hardware mode requires sandbox acknowledgement")
    scale_width = values["gstreamer.crystalhddec.scale_width"]
    if scale_width != 0 and values["player.decoder"] != "hardware":
        raise SchemaError("scale width requires hardware decode")
    output = values["player.software_output_size"]
    if output is not ABSENT and (
        values["player.decoder"] != "hardware"
        or scale_width == 0
    ):
        raise SchemaError("software output size requires scaled hardware decode")


def encode_existing_target(key: str, value: object) -> object:
    """Model only the target encodings whose absent/false/zero cases differ."""
    if key == "vaapi.h264.mode":
        return {"bounded": ABSENT, "strict": "0", "live": "1"}[value]
    if key == "vaapi.debug":
        return "1" if value else ABSENT
    if key == "chromium.password_store":
        return value
    if key == "gstreamer.crystalhddec.scale_width":
        return value
    raise KeyError(key)


@dataclass(frozen=True)
class FileFacts:
    uid: int
    mode: int
    directory_uid: int
    directory_mode: int
    regular: bool = True
    final_symlink: bool = False
    real_uid: int = 1000
    effective_uid: int = 1000
    real_gid: int = 1000
    effective_gid: int = 1000


def secure_file(facts: FileFacts, *, system: bool) -> bool:
    expected_uid = 0 if system else facts.real_uid
    if not facts.regular or facts.final_symlink:
        return False
    if facts.uid != expected_uid or facts.directory_uid != expected_uid:
        return False
    if facts.mode & 0o022 or facts.directory_mode & 0o022:
        return False
    if not system and (
        facts.real_uid != facts.effective_uid or facts.real_gid != facts.effective_gid
    ):
        return False
    return True


class SchemaOracleTest(unittest.TestCase):
    def parse(self, text: str) -> Document:
        return parse_document(text.encode("utf-8"))

    def empty(self) -> Document:
        return self.parse("[crystalhd]\nschema = 1\n")

    def test_every_inventory_surface_has_one_schema_disposition(self):
        inventory_text = INVENTORY.read_text(encoding="utf-8")
        specification = SPECIFICATION.read_text(encoding="utf-8")
        inventory_matches = INVENTORY_MARKER.findall(inventory_text)
        map_matches = SCHEMA_MAP_MARKER.findall(specification)
        boundary_matches = SCHEMA_BOUNDARY_MARKER.findall(specification)
        surfaces = set(inventory_matches)
        mapped = {(kind, name) for kind, name, _key in map_matches}
        bounded = {(kind, name) for kind, name, _reason in boundary_matches}

        self.assertEqual(len(inventory_matches), 27)
        self.assertEqual(len(surfaces), 27)
        self.assertEqual(len(map_matches), 23)
        self.assertEqual(len(mapped), 23)
        self.assertEqual(len(boundary_matches), 4)
        self.assertEqual(len(bounded), 4)
        self.assertFalse(mapped & bounded)
        self.assertEqual(mapped | bounded, surfaces)
        self.assertEqual(
            {reason for _kind, _name, reason in boundary_matches},
            {"staged-module", "legacy-bootstrap", "invocation-only"},
        )

        marked_keys = SCHEMA_KEY_MARKER.findall(specification)
        mapped_keys = {key for _kind, _name, key in map_matches}
        self.assertEqual(len(marked_keys), 21)
        self.assertEqual(set(marked_keys), set(RULES))
        self.assertEqual(mapped_keys, set(RULES))
        self.assertEqual(set(NATIVE), set(RULES))

    def test_strict_document_and_profile_grammar(self):
        document = self.parse(
            "# strict v1\r\n"
            "[crystalhd]\r\n"
            "schema = 1\r\n"
            'preset = "default"\r\n'
            'profile = "moonlight"\r\n'
            "[settings]\r\n"
            'vaapi.h264.mode = "bounded"\r\n'
            "gstreamer.crystalhddec.scale_width = 0\r\n"
            "chromium.password_store = \"\"\r\n"
            "[profile.moonlight]\r\n"
            'vaapi.h264.mode = "live"\r\n'
            "vaapi.h264.low_latency = true\r\n"
        )
        self.assertEqual(document.metadata, {
            "schema": 1,
            "preset": "default",
            "profile": "moonlight",
        })
        self.assertEqual(document.settings["gstreamer.crystalhddec.scale_width"], 0)
        self.assertEqual(document.settings["chromium.password_store"], "")
        self.assertEqual(document.profiles["moonlight"]["vaapi.h264.low_latency"], True)

    def test_unknown_duplicate_version_and_encoding_are_rejected(self):
        invalid = (
            "[crystalhd]\nschema = 2\n",
            "[crystalhd]\nschema = 1\nunknown = true\n",
            "[crystalhd]\nschema = 1\n[other]\nvalue = true\n",
            "[crystalhd]\nschema = 1\n[settings]\nunknown.key = true\n",
            "[crystalhd]\nschema = 1\n[settings]\nvaapi.trace = true\nvaapi.trace = false\n",
            "[crystalhd]\nschema = 1\n[settings]\n[settings]\n",
            "[crystalhd]\nschema = 1\n[settings]\ngstreamer.crystalhddec.scale_width = 00\n",
            "[crystalhd]\nschema = 1 # no inline comment\n",
        )
        for text in invalid:
            with self.subTest(text=text), self.assertRaises(SchemaError):
                self.parse(text)
        with self.assertRaises(SchemaError):
            parse_document(b"\xef\xbb\xbf[crystalhd]\nschema = 1\n")
        with self.assertRaises(SchemaError):
            parse_document(b"\xff")
        with self.assertRaises(SchemaError):
            parse_document(b"#" * (MAXIMUM_BYTES + 1))

    def test_types_ranges_and_unset_are_distinct(self):
        document = self.parse(
            "[crystalhd]\nschema = 1\n[settings]\n"
            "chromium.native_gl = false\n"
            "gstreamer.crystalhddec.scale_width = 0\n"
            "chromium.password_store = \"\"\n"
            "chromium.profile_dir = unset\n"
        )
        self.assertIs(document.settings["chromium.native_gl"], False)
        self.assertEqual(document.settings["gstreamer.crystalhddec.scale_width"], 0)
        self.assertEqual(document.settings["chromium.password_store"], "")
        self.assertIs(document.settings["chromium.profile_dir"], UNSET)
        bad_values = (
            "libcrystalhd.rx_pool_count = 12",
            "gstreamer.crystalhddec.scale_width = 129",
            'player.software_output_size = "4096x4096"',
            'vaapi.h264.mode = "false"',
            'chromium.executable = ""',
        )
        for assignment in bad_values:
            with self.subTest(assignment=assignment), self.assertRaises(SchemaError):
                self.parse(f"[crystalhd]\nschema = 1\n[settings]\n{assignment}\n")

    def test_precedence_origins_unset_and_explicit_profile(self):
        system = self.parse(
            "[crystalhd]\nschema = 1\n"
            'preset = "quality"\nprofile = "moonlight"\n'
            "[settings]\n"
            "gstreamer.crystalhddec.scale_width = 1280\n"
            "vaapi.trace = true\n"
            "chromium.password_store = \"system\"\n"
            "[profile.moonlight]\n"
            "chromium.native_gl = true\n"
        )
        user = self.parse(
            "[crystalhd]\nschema = 1\n"
            "[settings]\n"
            "gstreamer.crystalhddec.scale_width = 0\n"
            "chromium.password_store = unset\n"
            "[profile.moonlight]\n"
            "chromium.native_gl = false\n"
        )
        result = resolve(
            system,
            user,
            {"default": {}, "quality": {"gstreamer.crystalhddec.scale_width": 960}},
            raw_environment={"CRYSTALHD_VAAPI_TRACE": "0"},
            application={"gstreamer.crystalhddec.scale_width": 640},
        )
        self.assertEqual((result.preset, result.profile), ("quality", "moonlight"))
        self.assertEqual(result.requested["gstreamer.crystalhddec.scale_width"], 640)
        self.assertEqual(
            result.origins["gstreamer.crystalhddec.scale_width"], "application"
        )
        self.assertIs(result.requested["vaapi.trace"], False)
        self.assertEqual(
            result.origins["vaapi.trace"],
            "environment:CRYSTALHD_VAAPI_TRACE",
        )
        self.assertIs(result.requested["chromium.native_gl"], False)
        self.assertEqual(
            result.origins["chromium.native_gl"], "user-profile:moonlight"
        )
        self.assertEqual(result.requested["chromium.password_store"], "basic")
        self.assertEqual(result.origins["chromium.password_store"], "user:unset")

    def test_profile_never_matches_an_application_implicitly(self):
        system = self.parse(
            "[crystalhd]\nschema = 1\n"
            "[profile.moonlight]\nvaapi.h264.mode = \"live\"\n"
        )
        result = resolve(
            system, self.empty(), {"default": {}}
        )
        self.assertIsNone(result.profile)
        self.assertEqual(result.requested["vaapi.h264.mode"], "bounded")
        result = resolve(
            system,
            self.empty(),
            {"default": {}},
            explicit_profile="moonlight",
        )
        self.assertEqual(result.profile, "moonlight")
        self.assertEqual(result.requested["vaapi.h264.mode"], "live")
        self.assertEqual(
            result.origins["vaapi.h264.mode"], "system-profile:moonlight"
        )
        with self.assertRaises(SchemaError):
            resolve(system, self.empty(), {"default": {}}, explicit_profile="vlc")

    def test_selector_precedence_is_explicit_then_user_then_system(self):
        system = self.parse(
            "[crystalhd]\nschema = 1\n"
            'preset = "system-preset"\nprofile = "system-profile"\n'
            "[profile.system-profile]\nvaapi.trace = true\n"
            "[profile.user-profile]\nvaapi.debug = true\n"
            "[profile.explicit-profile]\nchromium.native_gl = true\n"
        )
        user = self.parse(
            "[crystalhd]\nschema = 1\n"
            'preset = "user-preset"\nprofile = "user-profile"\n'
        )
        presets = {
            "default": {},
            "system-preset": {"libcrystalhd.rx_pool_count": 16},
            "user-preset": {"libcrystalhd.rx_pool_count": 8},
            "explicit-preset": {"vaapi.i915_wc": True},
        }
        result = resolve(system, user, presets)
        self.assertEqual(
            (result.preset, result.profile), ("user-preset", "user-profile")
        )
        self.assertEqual(result.requested["libcrystalhd.rx_pool_count"], 8)
        self.assertTrue(result.requested["vaapi.debug"])
        result = resolve(
            system,
            user,
            presets,
            explicit_preset="explicit-preset",
            explicit_profile="explicit-profile",
        )
        self.assertEqual(
            (result.preset, result.profile),
            ("explicit-preset", "explicit-profile"),
        )
        self.assertTrue(result.requested["vaapi.i915_wc"])
        self.assertTrue(result.requested["chromium.native_gl"])
        with self.assertRaises(SchemaError):
            resolve(system, user, presets, explicit_preset="missing")

    def test_cross_setting_conflicts_are_transactional_errors(self):
        invalid_layers = (
            {"vaapi.h264.low_latency": True},
            {"vaapi.h264.mode": "live", "vaapi.h264.ready_remap": True},
            {"chromium.experimental_hw_decode": True},
            {"player.software_output_size": "1280x720"},
        )
        for layer in invalid_layers:
            with self.subTest(layer=layer), self.assertRaises(SchemaError):
                resolve(self.empty(), self.empty(), {"default": layer})

        valid = {
            "vaapi.h264.mode": "live",
            "vaapi.h264.low_latency": True,
            "vaapi.h264.ready_remap": True,
            "chromium.experimental_hw_decode": True,
            "chromium.disable_gpu_sandbox": True,
            "gstreamer.crystalhddec.scale_width": 1280,
            "player.software_output_size": "1920x1080",
        }
        result = resolve(
            self.empty(), self.empty(), {"default": valid}
        )
        self.assertTrue(result.requested["vaapi.h264.ready_remap"])
        self.assertTrue(result.effective["vaapi.h264.ready_remap"])

        with self.assertRaises(SchemaError):
            resolve(
                self.empty(),
                self.empty(),
                {"default": {}},
                application={"chromium.experimental_hw_decode": True},
            )
        with self.assertRaises(SchemaError):
            resolve(
                self.empty(),
                self.empty(),
                {"default": {"gstreamer.crystalhddec.scale_width": 1280}},
                application={"player.decoder": "software"},
            )

        invalid_system = self.parse(
            "[crystalhd]\nschema = 1\n[settings]\n"
            "vaapi.h264.low_latency = true\n"
        )
        invalid_user = self.parse(
            "[crystalhd]\nschema = 1\n[settings]\n"
            "vaapi.h264.low_latency = true\n"
        )
        invalid_profile = self.parse(
            "[crystalhd]\nschema = 1\n[profile.bad]\n"
            "vaapi.h264.low_latency = true\n"
        )
        managed_cases = (
            (invalid_system, self.empty(), None),
            (self.empty(), invalid_user, None),
            (invalid_profile, self.empty(), "bad"),
        )
        for system, user, profile in managed_cases:
            with self.subTest(profile=profile), self.assertRaises(SchemaError):
                resolve(
                    system,
                    user,
                    {"default": {}},
                    explicit_profile=profile,
                    raw_environment={"CRYSTALHD_VAAPI_LIVE_H264": "1"},
                )

    def test_raw_mode_override_preserves_request_but_deactivates_low_latency(self):
        managed = {
            "vaapi.h264.mode": "live",
            "vaapi.h264.low_latency": True,
        }
        result = resolve(
            self.empty(),
            self.empty(),
            {"default": managed},
            raw_environment={"CRYSTALHD_VAAPI_LIVE_H264": "0"},
        )

        self.assertEqual(result.requested["vaapi.h264.mode"], "strict")
        self.assertEqual(
            result.origins["vaapi.h264.mode"],
            "environment:CRYSTALHD_VAAPI_LIVE_H264",
        )
        self.assertTrue(result.requested["vaapi.h264.low_latency"])
        self.assertFalse(result.effective["vaapi.h264.low_latency"])
        self.assertEqual(
            result.activity["vaapi.h264.low_latency"],
            "inactive:requires-live-fast-path",
        )
        receipt = next(
            item
            for item in result.raw_receipts
            if item.variable == "CRYSTALHD_VAAPI_LIVE_H264"
        )
        self.assertEqual(receipt.raw, "0")
        self.assertEqual(receipt.requested, "strict")
        self.assertTrue(receipt.valid)

    def test_invalid_raw_ready_values_disable_remap_without_schema_error(self):
        managed = {
            "vaapi.h264.mode": "live",
            "vaapi.h264.low_latency": True,
            "vaapi.h264.ready_remap": True,
        }
        for variable, raw, activity in (
            (
                "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY",
                "newest",
                "inactive:invalid-ready-policy",
            ),
            (
                "CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US",
                "12345",
                "inactive:invalid-ready-delay",
            ),
        ):
            with self.subTest(variable=variable):
                result = resolve(
                    self.empty(),
                    self.empty(),
                    {"default": managed},
                    raw_environment={variable: raw},
                )
                self.assertFalse(result.effective["vaapi.h264.ready_remap"])
                self.assertEqual(result.activity["vaapi.h264.ready_remap"], activity)
                receipt = result.raw_receipts[0]
                self.assertEqual((receipt.variable, receipt.raw), (variable, raw))
                self.assertFalse(receipt.valid)
                self.assertIsInstance(receipt.requested, InvalidNativeValue)
                self.assertEqual(result.origins[receipt.key], f"environment:{variable}")

    def test_raw_adapter_fallback_and_launcher_rejection_are_receipts(self):
        result = resolve(
            self.empty(),
            self.empty(),
            {"default": {}},
            raw_environment={
                "CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT": "12",
                "CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE": "1",
                "CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX": "0",
            },
        )
        self.assertEqual(result.effective["libcrystalhd.rx_pool_count"], 16)
        self.assertEqual(
            result.activity["libcrystalhd.rx_pool_count"],
            "fallback:invalid-native-value",
        )
        self.assertEqual(
            result.activity["chromium.experimental_hw_decode"],
            "rejected:missing-gpu-sandbox-acknowledgement",
        )
        self.assertEqual(
            result.rejections,
            (
                NativeRejection(
                    "crystalhd-chromium",
                    "missing-gpu-sandbox-acknowledgement",
                ),
            ),
        )

    def test_existing_target_encodings_preserve_false_zero_empty_and_absent(self):
        self.assertIs(encode_existing_target("vaapi.h264.mode", "bounded"), ABSENT)
        self.assertEqual(encode_existing_target("vaapi.h264.mode", "strict"), "0")
        self.assertEqual(encode_existing_target("vaapi.h264.mode", "live"), "1")
        self.assertIs(encode_existing_target("vaapi.debug", False), ABSENT)
        self.assertEqual(encode_existing_target("vaapi.debug", True), "1")
        self.assertEqual(encode_existing_target("chromium.password_store", ""), "")
        self.assertEqual(
            encode_existing_target("gstreamer.crystalhddec.scale_width", 0), 0
        )

    def test_file_integrity_and_identity_rules(self):
        user = FileFacts(uid=1000, mode=stat.S_IFREG | 0o600,
                         directory_uid=1000, directory_mode=stat.S_IFDIR | 0o700)
        self.assertTrue(secure_file(user, system=False))
        self.assertTrue(secure_file(FileFacts(
            uid=1000, mode=stat.S_IFREG | 0o644,
            directory_uid=1000, directory_mode=stat.S_IFDIR | 0o755,
        ), system=False))
        self.assertTrue(secure_file(FileFacts(
            uid=0, mode=stat.S_IFREG | 0o644,
            directory_uid=0, directory_mode=stat.S_IFDIR | 0o755,
        ), system=True))

        rejected = (
            FileFacts(uid=1000, mode=stat.S_IFREG | 0o622,
                      directory_uid=1000, directory_mode=stat.S_IFDIR | 0o700),
            FileFacts(uid=1000, mode=stat.S_IFREG | 0o600,
                      directory_uid=1000, directory_mode=stat.S_IFDIR | 0o722),
            FileFacts(uid=0, mode=stat.S_IFREG | 0o600,
                      directory_uid=1000, directory_mode=stat.S_IFDIR | 0o700),
            FileFacts(uid=1000, mode=stat.S_IFREG | 0o600,
                      directory_uid=1000, directory_mode=stat.S_IFDIR | 0o700,
                      final_symlink=True),
            FileFacts(uid=1000, mode=stat.S_IFREG | 0o600,
                      directory_uid=1000, directory_mode=stat.S_IFDIR | 0o700,
                      real_uid=1000, effective_uid=0),
        )
        for facts in rejected:
            with self.subTest(facts=facts):
                self.assertFalse(secure_file(facts, system=False))


def main() -> int:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SchemaOracleTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print(
            "configuration schema oracle: PASS "
            "(21 canonical keys; 27 production surfaces classified; test-only)"
        )
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
