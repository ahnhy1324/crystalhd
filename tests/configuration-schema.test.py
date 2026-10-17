#!/usr/bin/env python3
"""Executable conformance tests for CONFIGURATION-SCHEMA.md and the CLI core."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import re
import stat
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
INVENTORY = ROOT / "CONFIGURATION.md"
SPECIFICATION = ROOT / "CONFIGURATION-SCHEMA.md"

_CONFIG_SPEC = importlib.util.spec_from_file_location(
    "crystalhd_config", ROOT / "scripts/crystalhd-config.py"
)
if _CONFIG_SPEC is None or _CONFIG_SPEC.loader is None:
    raise RuntimeError("cannot import production crystalhd-config")
_CONFIG = importlib.util.module_from_spec(_CONFIG_SPEC)
_CONFIG_SPEC.loader.exec_module(_CONFIG)

SchemaError = _CONFIG.SchemaError
Document = _CONFIG.Document
FileFacts = _CONFIG.FileFacts
InvalidNativeValue = _CONFIG.InvalidNativeValue
NativeRejection = _CONFIG.NativeRejection
MAXIMUM_BYTES = _CONFIG.MAXIMUM_BYTES
UNSET = _CONFIG.UNSET
ABSENT = _CONFIG.ABSENT
RULES = _CONFIG.RULES
NATIVE = _CONFIG.NATIVE
encode_existing_target = _CONFIG.encode_existing_target
parse_document = _CONFIG.parse_document
resolve = _CONFIG.resolve
secure_file = _CONFIG.secure_file

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

        self.assertEqual(len(inventory_matches), 29)
        self.assertEqual(len(surfaces), 29)
        self.assertEqual(len(map_matches), 23)
        self.assertEqual(len(mapped), 23)
        self.assertEqual(len(boundary_matches), 6)
        self.assertEqual(len(bounded), 6)
        self.assertFalse(mapped & bounded)
        self.assertEqual(mapped | bounded, surfaces)
        self.assertEqual(
            {reason for _kind, _name, reason in boundary_matches},
            {"staged-module", "legacy-bootstrap", "invocation-only", "runtime-only"},
        )

        marked_keys = SCHEMA_KEY_MARKER.findall(specification)
        mapped_keys = {key for _kind, _name, key in map_matches}
        self.assertEqual(len(marked_keys), 21)
        self.assertEqual(set(marked_keys), set(RULES))
        self.assertEqual(mapped_keys, set(RULES))
        self.assertEqual(set(NATIVE), set(RULES))
        self.assertEqual(
            _CONFIG.ENVIRONMENT_TARGETS,
            {name for kind, name, _key in map_matches if kind == "environment"},
        )

    def test_vpp_opt_out_is_runtime_only_without_a_canonical_key(self):
        self.assertNotIn("CRYSTALHD_VAAPI_DISABLE_VPP", _CONFIG.ENVIRONMENT_TARGETS)
        with self.assertRaises(SchemaError):
            self.parse("[crystalhd]\nschema = 1\n[settings]\nvaapi.disable_vpp = true\n")

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
            "(21 canonical keys; 29 production surfaces classified; production core)"
        )
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
