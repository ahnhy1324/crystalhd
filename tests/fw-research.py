#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Hardware-free firmware mapping, malformed-input and file-ownership tests."""

import hashlib
from contextlib import ExitStack
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/fw-research/flea_fw_map.py"
SPEC = importlib.util.spec_from_file_location("flea_fw_map", TOOL)
MAP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MAP)
BLOB = ROOT / "firmware/fwbin/70015/bcm70015fw.bin"


def prior_inner_descriptor_projection(report):
    """Remove only the separately tested, opt-in additive dispatch report."""
    if "inner_descriptor" in report:
        report["inner_descriptor"]["paths"]["record_pointer_and_boundary"].pop("conditional_inner_dispatch")
    return report


def fixture():
    image = bytearray(0x2f0)
    image[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", image, 16,
                     2, 45, 1, 0x4000, 52, 0x200, 0, 52, 32, 1, 40, 6, 5)
    struct.pack_into("<8I", image, 52, 1, 0x100, 0x4000, 0x4000, 32, 32, 5, 4)
    names = b"\0.text\0.bss\0.strtab\0.symtab\0.shstrtab\0"
    strings = b"\0Arc_UartInit\0ArcCommandBuffer\0TestBss\0"
    image[0x120:0x120 + len(strings)] = strings
    image[0x1b0:0x1b0 + len(names)] = names
    records = ((0, 0, 0, 0, 0, 0),
               (strings.index(b"Arc_UartInit"), 0x4004, 12, 0x12, 0, 1),
               (strings.index(b"ArcCommandBuffer"), 0x4010, 4, 0x12, 0, 1),
               (strings.index(b"TestBss"), 0x8000, 4, 0x11, 0, 2))
    for index, record in enumerate(records):
        struct.pack_into("<IIIBBH", image, 0x160 + index * 16, *record)
    sections = (
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, 0x4000, 0x100, 32, 0, 0, 4, 0),
        (7, 8, 3, 0x8000, 0xffff00, 64, 0, 0, 4, 0),
        (12, 3, 0, 0, 0x120, len(strings), 0, 0, 1, 0),
        (20, 2, 0, 0, 0x160, len(records) * 16, 3, 1, 4, 16),
        (28, 3, 0, 0, 0x1b0, len(names), 0, 0, 1, 0),
    )
    for index, section in enumerate(sections):
        struct.pack_into("<10I", image, 0x200 + index * 40, *section)
    return bytes(64) + image + struct.pack("<I", 16) + bytes(16)


def analyze_fixture(data, wanted=("Arc_UartInit", "ArcCommandBuffer", "TestBss", "Missing"),
                    **options):
    return MAP.analyze(data, wanted, hashlib.sha256(data).hexdigest(), **options)


def relocation_fixture(text_records=None, bss_records=None, alias=False):
    original = fixture()
    image = bytearray(original[64:-20])
    image.extend(bytes(0x480 - len(image)))
    names = b"\0.text\0.bss\0.strtab\0.symtab\0.shstrtab\0.rela.text\0.rela.bss\0"
    image[0x1b0:0x1b0 + len(names)] = names
    struct.pack_into("<H", image, 48, 8)
    struct.pack_into("<I", image, 0x200 + 5 * 40 + 20, len(names))
    if alias:
        strings = b"\0Arc_UartInit\0ArcCommandBuffer\0TestBss\0Alias\0"
        image[0x120:0x120 + len(strings)] = strings
        struct.pack_into("<I", image, 0x200 + 3 * 40 + 20, len(strings))
        struct.pack_into("<I", image, 0x200 + 4 * 40 + 20, 5 * 16)
        struct.pack_into("<IIIBBH", image, 0x1a0,
                         strings.index(b"Alias"), 0x4008, 8, 0x12, 0, 1)
    if text_records is None:
        text_records = ((0x4008, (2 << 8) | 4, 0),
                        (0x4011, (3 << 8) | 6, 0),
                        (0x401f, (1 << 8) | 5, -4),
                        (0xffffffff, 0, 0))
    if bss_records is None:
        bss_records = ((0x8001, (1 << 8) | 4, -5),)
    for index, name, offset, target, records in (
            (6, b".rela.text", 0x380, 1, text_records),
            (7, b".rela.bss", 0x400, 2, bss_records)):
        section = (names.index(name), 4, 0, 0, offset, len(records) * 12,
                   4, target, 4, 12)
        struct.pack_into("<10I", image, 0x200 + index * 40, *section)
        for record_index, record in enumerate(records):
            struct.pack_into("<IIi", image, offset + record_index * 12, *record)
    return original[:64] + image + original[-20:]


class FirmwareMapTests(unittest.TestCase):
    def test_bundled_identity_and_boundaries(self):
        data = MAP.read_firmware(BLOB)
        report = MAP.analyze(data)
        self.assertEqual(report["sha256"], MAP.BUNDLED_SHA256)
        self.assertEqual(report["git_blob_sha1"], "bb450bc221cfbab41661a6c4732cd287824b3e3a")
        self.assertEqual(report["size"], 864276)
        self.assertEqual(report["payload_end"], 0xd3000)
        self.assertEqual(report["signature_file_offset"], 0xd3004)
        self.assertFalse(report["signature_verified"])
        self.assertTrue(report["bundled_baseline"])
        self.assertEqual([(i["blob_file_offset"], i["blob_file_end"]) for i in report["images"]],
                         [(0x2ea60, 0x79dd8), (0x79dd8, 0xcfbb0)])
        self.assertEqual([i["symbol_count"] for i in report["images"]], [837, 1076])
        self.assertEqual([i["section_count"] for i in report["images"]], [55, 112])
        self.assertEqual([i["program_header_count"] for i in report["images"]], [18, 19])
        self.assertEqual(report["firmware_revisions"],
                         [{"blob_file_offset": 0x12b, "text": "$Media_PC_FW_Rev: 1.54.0.0 $"}])
        self.assertEqual(report["arm_vector_candidates"][0]["target_value"], 0x2ca00)
        self.assertEqual(len(report["arm_vector_candidates"]), 7)

    def test_bundled_symbol_addresses_independently_audited(self):
        report = MAP.analyze(MAP.read_firmware(BLOB))
        expected = (
            {"Arc_UartInit": (0x23e84, 0x46a18, 156),
             "Arc_UartPoll": (0x23f20, 0x46ab4, 28), "ArcGetc": (0x23f9c, 0x46b30, 40),
             "ArcPutc": (0x8000, 0x32ea4, 144), "ArcCommandBuffer": (0x23e48, 0x469dc, 12),
             "ReadLine": (0x27ae4, 0x4a678, 292), "MatchKeyword": (0x27c08, 0x4a79c, 108),
             "Core_Command": (0x25808, 0x4839c, 516), "CmdPeek": (0x28e84, 0x4ba18, 232),
             "CmdCore": (0x281bc, 0x4ad50, 796), "CmdState": (0x287a8, 0x4b33c, 1388),
             "CmdTrace": (0x280a4, 0x4ac38, 280), "CmdCabac": (0x29770, 0x4c304, 452)},
            {"Arc_UartInit": (0x41104, 0xb8905, 156),
             "Arc_UartPoll": (0x411a0, 0xb89a1, 28), "ArcGetc": (0x41234, 0xb8a35, 40),
             "ArcPutc": (0x411bc, 0xb89bd, 32), "ReadLine": (0x4254c, 0xb9d4d, 292),
             "MatchKeyword": (0x42670, 0xb9e71, 108), "CmdPeek": (0x42c7c, 0xba47d, 232)},
        )
        for image, addresses in zip(report["images"], expected):
            actual = {s["name"]: (s["elf_virtual_address"], s["blob_file_offset"], s["size"])
                      for s in image["symbols"]}
            self.assertEqual(len(image["symbols"]), len(addresses))
            self.assertEqual(actual, addresses)
            self.assertTrue(all(s["type"] == 2 for s in image["symbols"]))
        self.assertEqual(set(report["images"][1]["missing_symbols"]),
                         set(MAP.DEFAULT_SYMBOLS) - set(expected[1]))

    def test_bundled_retained_relocation_counts_and_reference_anchors(self):
        report = MAP.analyze(MAP.read_firmware(BLOB),
                             ("ReadLine", "Core_Command", "Core_Loop"), references=True)
        first, second = report["images"]
        self.assertEqual([i["relocation_count"] for i in report["images"]], [4114, 1535])
        self.assertEqual([i["relocation_noop_count"] for i in report["images"]], [55, 48])
        self.assertEqual([i["unowned_executable_reference_count"] for i in report["images"]], [14, 105])
        self.assertEqual(first["relocation_type_counts"],
                         {"0": 55, "4": 1425, "5": 8, "6": 2618, "7": 8})
        self.assertEqual(second["relocation_type_counts"],
                         {"0": 48, "4": 853, "5": 14, "6": 599, "7": 21})
        # Independently inspected ELF32 Rela records; type 6 is not labelled a call.
        anchors = ((first, 0x735d8, 51, 16, 0x27b20, 0x4a6b4, "ReadLine", "ArcGetc",
                    0x23f9c, 0x46b30),
                   (first, 0x733a4, 51, 16, 0x266e0, 0x49274, "Core_Loop", "Core_Command",
                    0x25808, 0x4839c),
                   (first, 0x72f48, 51, 16, 0x25910, 0x484a4, "Core_Command", "Core_StopChannel",
                    0x26ba0, 0x49734),
                   (second, 0xcc1cc, 108, 47, 0x42588, 0xb9d89, "ReadLine", "ArcGetc",
                    0x41234, 0xb8a35))
        for image, offset, reloc_section, source_section, address, file_offset, owner, target, va, pos in anchors:
            with self.subTest(offset=offset):
                ref = next(r for r in image["references"] if r["relocation_record_offset"] == offset)
                self.assertEqual(ref["relocation_section_index"], reloc_section)
                self.assertEqual(ref["relocation_type"], 6)
                self.assertEqual(ref["addend"], 0)
                self.assertEqual(ref["source"]["section_index"], source_section)
                self.assertEqual(ref["source"]["elf_virtual_address"], address)
                self.assertEqual(ref["source"]["blob_file_offset"], file_offset)
                self.assertEqual([s["name"] for s in ref["source"]["function_owners"]], [owner])
                self.assertEqual(ref["target"]["symbol"]["name"], target)
                self.assertEqual(ref["target"]["symbol"]["elf_virtual_address"], va)
                self.assertEqual(ref["target"]["symbol"]["blob_file_offset"], pos)
                self.assertEqual(ref["target"]["addend_candidate_virtual_address"], va)
                self.assertEqual(ref["target"]["addend_candidate_blob_file_offset"], pos)
                self.assertTrue(ref["target"]["addend_candidate_in_section"])
        for image in report["images"]:
            self.assertEqual(sum(image["relocation_type_counts"].values()), image["relocation_count"])
            self.assertTrue(all(r["relocation_type"] != 0 for r in image["references"]))

    def test_bundled_all_symbol_section_metadata_and_nonfile_backed_sections(self):
        report = MAP.analyze(MAP.read_firmware(BLOB), all_symbols=True)
        expected = ((0x79540, ((32, ".comment", 0x6711c, 0x909, 0x79a40),
                              (33, ".arcextmap", 0x67a25, 0x70, 0x79a68))),
                    (0xcea30, ((63, ".comment", 0xc1ced, 0x509, 0xcf408),
                              (64, ".arcextmap", 0xc21f6, 0x70, 0xcf430))))
        for image, (header_base, anchors) in zip(report["images"], expected):
            sections = image["sections"]
            self.assertEqual(len(sections), image["section_count"])
            self.assertEqual(len(image["symbols"]), image["symbol_count"])
            for index, name, offset, size, header in anchors:
                with self.subTest(image=image["blob_file_offset"], section=name):
                    section = sections[index]
                    self.assertEqual(section["section_index"], index)
                    self.assertEqual(section["name"], name)
                    self.assertEqual(section["type"], 1)
                    self.assertEqual(section["flags"], 0)
                    self.assertEqual(section["elf_virtual_address"], 0)
                    self.assertEqual(section["blob_file_offset"], offset)
                    self.assertEqual(section["size"], size)
                    self.assertEqual(section["section_header_blob_file_offset"], header)
            self.assertTrue(any(s["type"] == 8 for s in sections))
            for index, section in enumerate(sections):
                self.assertEqual(section["section_header_blob_file_offset"], header_base + 40 * index)
                if section["type"] in (0, 8):
                    self.assertIsNone(section["blob_file_offset"])
                elif section["size"]:
                    self.assertLessEqual(section["blob_file_offset"] + section["size"], image["blob_file_end"])
            for symbol in image["symbols"]:
                index = symbol["section_index"]
                if 0 < index < len(sections):
                    self.assertEqual(symbol["section"], sections[index]["name"])
                    if sections[index]["type"] in (0, 8):
                        self.assertIsNone(symbol["blob_file_offset"])

    def test_relocation_source_target_identity_and_signed_addends(self):
        image = analyze_fixture(relocation_fixture(), references=True)["images"][0]
        self.assertEqual(image["relocation_count"], 5)
        self.assertEqual(image["relocation_noop_count"], 1)
        self.assertEqual(image["relocation_type_counts"], {"0": 1, "4": 2, "5": 1, "6": 1})
        self.assertEqual(image["unowned_executable_reference_count"], 1)
        refs = {r["relocation_record_offset"]: r for r in image["references"]}
        self.assertEqual(set(refs), {64 + 0x380, 64 + 0x38c, 64 + 0x398, 64 + 0x400})
        ref = refs[64 + 0x380]
        self.assertEqual(ref["source"]["section"], ".text")
        self.assertEqual(ref["source"]["section_index"], 1)
        self.assertEqual(ref["source"]["elf_virtual_address"], 0x4008)
        self.assertEqual(ref["source"]["blob_file_offset"], 64 + 0x108)
        owner, = ref["source"]["function_owners"]
        self.assertEqual(owner["name"], "Arc_UartInit")
        self.assertEqual(owner["symbol_table_section_index"], 4)
        self.assertEqual(owner["symbol_index"], 1)
        target = ref["target"]
        self.assertEqual(target["symbol"]["name"], "ArcCommandBuffer")
        self.assertEqual(target["symbol"]["symbol_index"], 2)
        self.assertEqual(target["symbol"]["symbol_record_offset"], 64 + 0x180)
        self.assertEqual(target["addend_candidate_virtual_address"], 0x4010)
        self.assertEqual(target["addend_candidate_blob_file_offset"], 64 + 0x110)
        self.assertTrue(target["addend_candidate_in_section"])
        bss = refs[64 + 0x38c]["target"]
        self.assertEqual(bss["symbol"]["name"], "TestBss")
        self.assertEqual(bss["addend_candidate_virtual_address"], 0x8000)
        self.assertTrue(bss["addend_candidate_in_section"])
        self.assertIsNone(bss["addend_candidate_blob_file_offset"])
        negative = refs[64 + 0x398]
        self.assertEqual(negative["addend"], -4)
        self.assertEqual(negative["source"]["function_owners"], [])
        self.assertEqual(negative["source"]["blob_file_offset"], 64 + 0x11f)
        self.assertEqual(negative["target"]["addend_candidate_virtual_address"], 0x4000)
        self.assertEqual(negative["target"]["addend_candidate_blob_file_offset"], 64 + 0x100)
        outside = refs[64 + 0x400]
        self.assertEqual(outside["source"]["section"], ".bss")
        self.assertIsNone(outside["source"]["blob_file_offset"])
        self.assertEqual(outside["source"]["function_owners"], [])
        self.assertEqual(outside["target"]["addend_candidate_virtual_address"], 0x3fff)
        self.assertFalse(outside["target"]["addend_candidate_in_section"])
        self.assertIsNone(outside["target"]["addend_candidate_blob_file_offset"])

    def test_reference_selection_uses_either_source_owner_or_target_name(self):
        data = relocation_fixture()
        cases = (("ArcCommandBuffer", {64 + 0x380, 64 + 0x38c}),
                 ("Arc_UartInit", {64 + 0x380, 64 + 0x398, 64 + 0x400}),
                 ("TestBss", {64 + 0x38c}), ("Missing", set()))
        for name, expected in cases:
            with self.subTest(name=name):
                image = analyze_fixture(data, (name,), references=True)["images"][0]
                self.assertEqual({r["relocation_record_offset"] for r in image["references"]}, expected)
                self.assertEqual(image["relocation_count"], 5)
                self.assertEqual(image["unowned_executable_reference_count"], 1)
        image = analyze_fixture(data, ("Missing",), references=True, all_symbols=True)["images"][0]
        self.assertEqual(len(image["references"]), 4)
        self.assertEqual(len(image["symbols"]), image["symbol_count"])
        self.assertEqual(image["missing_symbols"], ["Missing"])

    def test_all_symbols_preserves_null_and_duplicate_records_without_references(self):
        data = bytearray(fixture())
        struct.pack_into("<I", data, 64 + 0x160 + 2 * 16, 1)
        image = analyze_fixture(data, ("Missing",), all_symbols=True)["images"][0]
        self.assertEqual(len(image["symbols"]), 4)
        self.assertEqual(image["symbols"][0]["name"], "")
        self.assertEqual(image["symbols"][0]["symbol_index"], 0)
        duplicates = [s for s in image["symbols"] if s["name"] == "Arc_UartInit"]
        self.assertEqual([s["symbol_index"] for s in duplicates], [1, 2])
        self.assertEqual(image["missing_symbols"], ["Missing"])
        self.assertNotIn("references", image)

    def test_reference_options_leave_default_symbol_schema_unchanged(self):
        image = analyze_fixture(relocation_fixture())["images"][0]
        for key in ("references", "relocation_count", "relocation_noop_count",
                    "relocation_type_counts", "unowned_executable_reference_count"):
            self.assertNotIn(key, image)
        for symbol in image["symbols"]:
            self.assertNotIn("symbol_index", symbol)
            self.assertNotIn("symbol_table_section_index", symbol)

    def test_relocation_owner_aliases_retain_ambiguity(self):
        data = relocation_fixture(alias=True)
        image = analyze_fixture(data, ("Alias",), references=True)["images"][0]
        ref, = image["references"]
        owners = ref["source"]["function_owners"]
        self.assertEqual({s["name"] for s in owners}, {"Alias", "Arc_UartInit"})
        self.assertEqual({s["symbol_index"] for s in owners}, {1, 4})
        self.assertEqual(len({s["symbol_record_offset"] for s in owners}), 2)

    def test_relocation_owner_requires_nonempty_function_in_executable_source_section(self):
        for field, fmt, value in ((0x160 + 16 + 8, "I", 0),
                                  (0x160 + 16 + 12, "B", 0x11),
                                  (0x200 + 40 + 8, "I", 2)):
            data = bytearray(relocation_fixture())
            struct.pack_into("<" + fmt, data, 64 + field, value)
            image = analyze_fixture(data, references=True)["images"][0]
            ref = next(r for r in image["references"] if r["relocation_record_offset"] == 64 + 0x380)
            with self.subTest(field=field):
                self.assertEqual(ref["source"]["function_owners"], [])
                self.assertEqual(image["unowned_executable_reference_count"], 0 if value == 2 else 2)

    def test_relocation_candidate_addition_never_wraps_or_crosses_sections(self):
        for addend, expected in ((-0x4005, -1), (0x7fffffff, 0x80004003),
                                  (28, 0x4020)):
            data = relocation_fixture(text_records=((0x4008, (1 << 8) | 4, addend),),
                                      bss_records=())
            target = analyze_fixture(data, references=True)["images"][0]["references"][0]["target"]
            with self.subTest(addend=addend):
                self.assertEqual(target["addend_candidate_virtual_address"], expected)
                self.assertFalse(target["addend_candidate_in_section"])
                self.assertIsNone(target["addend_candidate_blob_file_offset"])
        data = bytearray(relocation_fixture(text_records=((0x4008, (1 << 8) | 4, 16),),
                                           bss_records=()))
        # A valid high-address source section makes S+A exceed 32 bits.
        struct.pack_into("<I", data, 64 + 0x200 + 40 + 12, 0xffffffe0)
        struct.pack_into("<I", data, 64 + 0x160 + 16 + 4, 0xffffffec)
        struct.pack_into("<I", data, 64 + 0x160 + 32 + 4, 0xfffffff0)
        struct.pack_into("<I", data, 64 + 0x380, 0xffffffec)
        target = analyze_fixture(data, references=True)["images"][0]["references"][0]["target"]
        self.assertEqual(target["addend_candidate_virtual_address"], 0xfffffffc)
        self.assertTrue(target["addend_candidate_in_section"])
        struct.pack_into("<i", data, 64 + 0x380 + 8, 32)
        target = analyze_fixture(data, references=True)["images"][0]["references"][0]["target"]
        self.assertEqual(target["addend_candidate_virtual_address"], 0x10000000c)
        self.assertFalse(target["addend_candidate_in_section"])
        self.assertIsNone(target["addend_candidate_blob_file_offset"])

    def test_relocation_undefined_and_absolute_targets_have_no_file_candidate(self):
        for index, value in ((0, 0x4004), (0xfff1, 0x4004)):
            data = bytearray(relocation_fixture(text_records=((0x4008, (1 << 8) | 4, 0),),
                                               bss_records=()))
            struct.pack_into("<H", data, 64 + 0x160 + 16 + 14, index)
            struct.pack_into("<I", data, 64 + 0x160 + 16 + 4, value)
            target = analyze_fixture(data, references=True)["images"][0]["references"][0]["target"]
            with self.subTest(section=index):
                self.assertEqual(target["symbol"]["section_index"], index)
                self.assertIsNone(target["symbol"]["blob_file_offset"])
                self.assertFalse(target["addend_candidate_in_section"])
                self.assertIsNone(target["addend_candidate_blob_file_offset"])

    def test_relocation_source_last_byte_valid_and_noop_skips_source_bounds(self):
        data = relocation_fixture(text_records=((0x401f, (1 << 8) | 255, 0),
                                                 (0xffffffff, 0, -1)), bss_records=())
        image = analyze_fixture(data, references=True)["images"][0]
        self.assertEqual(image["relocation_type_counts"], {"0": 1, "255": 1})
        ref, = image["references"]
        self.assertEqual(ref["relocation_type"], 255)
        self.assertEqual(ref["source"]["blob_file_offset"], 64 + 0x11f)

    def test_malformed_relocation_metadata_and_records_rejected_even_if_unselected(self):
        section = 0x200 + 6 * 40
        mutations = ((section + 20, "I", 47), (section + 24, "I", 8),
                     (section + 24, "I", 3), (section + 28, "I", 0),
                     (section + 28, "I", 8), (section + 36, "I", 16),
                     (section + 16, "I", 0xfffffff0),
                     (0x380 + 4, "I", (4 << 8) | 4),
                     (0x380, "I", 0x3fff), (0x380, "I", 0x4020))
        for offset, fmt, value in mutations:
            data = bytearray(relocation_fixture())
            struct.pack_into("<" + fmt, data, 64 + offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(MAP.FormatError):
                analyze_fixture(data, ("Missing",), references=True)

    def test_empty_relocation_table_still_requires_valid_link_and_target_metadata(self):
        for field, value in ((24, 3), (28, 0)):
            data = bytearray(relocation_fixture(text_records=(), bss_records=()))
            struct.pack_into("<I", data, 64 + 0x200 + 6 * 40 + field, value)
            with self.subTest(field=field), self.assertRaises(MAP.FormatError):
                analyze_fixture(data, ("Missing",), references=True)

    def test_implicit_addend_rel_is_not_silently_interpreted_as_rela(self):
        data = bytearray(relocation_fixture())
        struct.pack_into("<I", data, 64 + 0x200 + 6 * 40 + 4, 9)
        analyze_fixture(data)
        with self.assertRaises(MAP.FormatError):
            analyze_fixture(data, references=True)

    def test_relocation_record_budget_is_aggregate_across_images(self):
        original = relocation_fixture()
        with mock.patch.object(MAP, "MAX_RELOCATION_RECORDS", 5):
            self.assertEqual(analyze_fixture(original, references=True)["images"][0]["relocation_count"], 5)
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(original[:-20] + original[64:], references=True)
        with mock.patch.object(MAP, "MAX_RELOCATION_RECORDS", 4):
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(original, ("Missing",), references=True)

    def test_relocation_owner_lookup_budget_is_aggregate_across_images(self):
        original = relocation_fixture(text_records=((0x4008, (2 << 8) | 4, 0),), bss_records=())
        with mock.patch.object(MAP, "MAX_OWNER_LOOKUP_STEPS", 1):
            self.assertEqual(len(analyze_fixture(original, references=True)["images"][0]["references"]), 1)
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(original[:-20] + original[64:], references=True)
        with mock.patch.object(MAP, "MAX_OWNER_LOOKUP_STEPS", 0):
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(original, ("Missing",), references=True)

    def test_relocation_owner_budget_counts_examined_noncontaining_alias(self):
        data = bytearray(relocation_fixture(text_records=((0x4008, (2 << 8) | 4, 0),),
                                           bss_records=(), alias=True))
        struct.pack_into("<I", data, 64 + 0x1a0 + 4, 0x4006)
        struct.pack_into("<I", data, 64 + 0x1a0 + 8, 1)
        with mock.patch.object(MAP, "MAX_OWNER_LOOKUP_STEPS", 2):
            image = analyze_fixture(data, references=True)["images"][0]
            self.assertEqual(image["owner_lookup_steps"], 2)
            self.assertEqual([s["name"] for s in image["references"][0]["source"]["function_owners"]],
                             ["Arc_UartInit"])
        with mock.patch.object(MAP, "MAX_OWNER_LOOKUP_STEPS", 1):
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(data, references=True)

    def test_relocation_output_budget_covers_aliases_and_multiple_images(self):
        original = relocation_fixture(text_records=((0x4008, (2 << 8) | 4, 0),), bss_records=())
        cost = analyze_fixture(original, references=True)["images"][0]["reference_output_budget_used"]
        self.assertGreater(cost, 0)
        with mock.patch.object(MAP, "MAX_REFERENCE_OUTPUT_BYTES", cost):
            self.assertEqual(len(analyze_fixture(original, references=True)["images"][0]["references"]), 1)
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(original[:-20] + original[64:], references=True)
            alias = relocation_fixture(text_records=((0x4008, (2 << 8) | 4, 0),),
                                       bss_records=(), alias=True)
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(alias, references=True)
        with mock.patch.object(MAP, "MAX_REFERENCE_OUTPUT_BYTES", cost - 1):
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(original, references=True)

    def test_relocation_output_and_record_budgets_cover_repeated_tables(self):
        original = relocation_fixture(text_records=((0x4008, (2 << 8) | 4, 0),), bss_records=())
        cost = analyze_fixture(original, references=True)["images"][0]["reference_output_budget_used"]
        data = bytearray(original)
        data[64 + 0x200 + 7 * 40:64 + 0x200 + 8 * 40] = (
            data[64 + 0x200 + 6 * 40:64 + 0x200 + 7 * 40])
        with mock.patch.object(MAP, "MAX_REFERENCE_OUTPUT_BYTES", cost * 2):
            refs = analyze_fixture(data, references=True)["images"][0]["references"]
            self.assertEqual(len(refs), 2)
            self.assertEqual({r["relocation_section_index"] for r in refs}, {6, 7})
            self.assertEqual({r["relocation_record_offset"] for r in refs}, {64 + 0x380})
        with mock.patch.object(MAP, "MAX_REFERENCE_OUTPUT_BYTES", cost * 2 - 1):
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(data, references=True)
        with mock.patch.object(MAP, "MAX_RELOCATION_RECORDS", 1):
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(data, references=True)

    def test_retained_metadata_budget_covers_all_symbols_and_unselected_references(self):
        data = relocation_fixture()
        image = analyze_fixture(data, all_symbols=True)["images"][0]
        section_cost = sum(1024 + 6 * len(s["name"]) for s in image["sections"])
        symbol_cost = sum(1024 + 6 * (len(s["name"]) + len(s["section"] or ""))
                          for s in image["symbols"])
        cost = section_cost + symbol_cost
        for options, wanted in (({"all_symbols": True}, ("Missing",)),
                                ({"references": True}, ("Missing",)),
                                ({"references": True}, ("Arc_UartInit",))):
            with self.subTest(options=options, wanted=wanted):
                with mock.patch.object(MAP, "MAX_METADATA_OUTPUT_BYTES", cost):
                    result = analyze_fixture(data, wanted, **options)["images"][0]
                    self.assertNotIn("_metadata_budget_used", result)
                with mock.patch.object(MAP, "MAX_METADATA_OUTPUT_BYTES", cost - 1):
                    with self.assertRaisesRegex(MAP.FormatError, "metadata.*budget"):
                        analyze_fixture(data, wanted, **options)
        with mock.patch.object(MAP, "MAX_METADATA_OUTPUT_BYTES", section_cost):
            self.assertEqual(analyze_fixture(data, ("Missing",))["images"][0]["symbols"], [])
        with mock.patch.object(MAP, "MAX_METADATA_OUTPUT_BYTES", 0):
            with self.assertRaisesRegex(MAP.FormatError, "metadata.*budget"):
                analyze_fixture(data, ("Missing",))

    def test_retained_metadata_budget_is_aggregate_across_images(self):
        original = relocation_fixture()
        image = analyze_fixture(original, all_symbols=True)["images"][0]
        cost = (sum(1024 + 6 * len(s["name"]) for s in image["sections"]) +
                sum(1024 + 6 * (len(s["name"]) + len(s["section"] or ""))
                    for s in image["symbols"]))
        data = original[:-20] + original[64:]
        for options in ({"all_symbols": True}, {"references": True}):
            with self.subTest(options=options):
                with mock.patch.object(MAP, "MAX_METADATA_OUTPUT_BYTES", cost * 2):
                    self.assertEqual(len(analyze_fixture(data, **options)["images"]), 2)
                with mock.patch.object(MAP, "MAX_METADATA_OUTPUT_BYTES", cost * 2 - 1):
                    with self.assertRaisesRegex(MAP.FormatError, "metadata.*budget"):
                        analyze_fixture(data, **options)

    def test_retained_metadata_counts_repeated_long_symbol_and_section_names(self):
        original = relocation_fixture()
        strings = b"\0" + b"L" * 2048 + b"\0"
        data = bytearray(original[:-20] + strings + bytes(2) + original[-20:])
        struct.pack_into("<I", data, 64 + 0x200 + 3 * 40 + 16, 0x480)
        struct.pack_into("<I", data, 64 + 0x200 + 3 * 40 + 20, len(strings))
        for index in range(4):
            struct.pack_into("<I", data, 64 + 0x160 + index * 16, 1)
        # Every header shares one decoded name rather than owning more strings.
        for index in range(8):
            struct.pack_into("<I", data, 64 + 0x200 + index * 40, 38)
        image = analyze_fixture(data, all_symbols=True)["images"][0]
        self.assertEqual({s["name"] for s in image["symbols"]}, {"L" * 2048})
        self.assertEqual(len(image["symbols"]), 4)
        self.assertEqual(len({s["name"] for s in image["sections"]}), 1)
        cost = (sum(1024 + 6 * len(s["name"]) for s in image["sections"]) +
                sum(1024 + 6 * (len(s["name"]) + len(s["section"] or ""))
                    for s in image["symbols"]))
        for options in ({"all_symbols": True}, {"references": True}):
            with self.subTest(options=options):
                with mock.patch.object(MAP, "MAX_METADATA_OUTPUT_BYTES", cost):
                    analyze_fixture(data, ("Missing",), **options)
                with mock.patch.object(MAP, "MAX_METADATA_OUTPUT_BYTES", cost - 1):
                    with self.assertRaisesRegex(MAP.FormatError, "metadata.*budget"):
                        analyze_fixture(data, ("Missing",), **options)

    def test_reserved_extended_section_count_rejected_before_table_read(self):
        for count in (0xff00, 0xffff):
            data = bytearray(fixture())
            struct.pack_into("<H", data, 64 + 48, count)
            with self.subTest(count=count), self.assertRaisesRegex(MAP.FormatError, "extended"):
                analyze_fixture(data, all_symbols=True)

    def test_zero_size_section_metadata_does_not_claim_bytes_at_unchecked_offset(self):
        data = bytearray(relocation_fixture(bss_records=()))
        struct.pack_into("<I", data, 64 + 0x200 + 7 * 40 + 16, 0xfffffff0)
        for references in (False, True):
            with self.subTest(references=references):
                image = analyze_fixture(data, all_symbols=True, references=references)["images"][0]
                section = image["sections"][7]
                self.assertEqual(section["type"], 4)
                self.assertEqual(section["size"], 0)
                self.assertIsNone(section["blob_file_offset"])

    def test_relocation_candidates_and_owners_do_not_resolve_via_overlay_sections(self):
        data = bytearray(relocation_fixture(text_records=((0x4008, (1 << 8) | 4, 28),),
                                           bss_records=()))
        # Overlay .bss at .text's VA with a sized function covering the source.
        struct.pack_into("<I", data, 64 + 0x200 + 2 * 40 + 8, 7)
        struct.pack_into("<I", data, 64 + 0x200 + 2 * 40 + 12, 0x4000)
        struct.pack_into("<I", data, 64 + 0x160 + 3 * 16 + 4, 0x4000)
        struct.pack_into("<I", data, 64 + 0x160 + 3 * 16 + 8, 32)
        struct.pack_into("<B", data, 64 + 0x160 + 3 * 16 + 12, 0x12)
        ref = analyze_fixture(data, references=True)["images"][0]["references"][0]
        self.assertEqual([s["name"] for s in ref["source"]["function_owners"]], ["Arc_UartInit"])
        self.assertEqual(ref["target"]["addend_candidate_virtual_address"], 0x4020)
        self.assertFalse(ref["target"]["addend_candidate_in_section"])
        self.assertIsNone(ref["target"]["addend_candidate_blob_file_offset"])

    def test_relocation_repeated_symbol_tables_keep_target_table_identity(self):
        data = bytearray(relocation_fixture(text_records=((0x4008, (1 << 8) | 4, 0),),
                                           bss_records=()))
        # Replace the unused second relocation section with an alias symbol table.
        data[64 + 0x200 + 7 * 40:64 + 0x200 + 8 * 40] = (
            data[64 + 0x200 + 4 * 40:64 + 0x200 + 5 * 40])
        struct.pack_into("<I", data, 64 + 0x200 + 6 * 40 + 24, 7)
        image = analyze_fixture(data, references=True, all_symbols=True)["images"][0]
        target = image["references"][0]["target"]["symbol"]
        self.assertEqual(target["symbol_table_section_index"], 7)
        self.assertEqual(target["symbol_index"], 1)
        self.assertEqual(target["symbol_record_offset"], 64 + 0x170)
        self.assertEqual(len(image["symbols"]), 8)

    def test_synthetic_mapping_and_nobits(self):
        report = analyze_fixture(fixture())
        image = report["images"][0]
        symbols = {s["name"]: s for s in image["symbols"]}
        self.assertEqual(image["blob_file_end"], 64 + 0x2f0)
        self.assertEqual(symbols["Arc_UartInit"]["blob_file_offset"], 64 + 0x104)
        self.assertEqual(symbols["ArcCommandBuffer"]["type"], 2)
        self.assertIsNone(symbols["TestBss"]["blob_file_offset"])
        self.assertEqual(image["missing_symbols"], ["Missing"])
        self.assertFalse(report["bundled_baseline"])

    def test_adjacent_images_keep_separate_identity(self):
        original = fixture()
        data = original[:-20] + original[64:]
        report = analyze_fixture(data)
        self.assertEqual(len(report["images"]), 2)
        self.assertNotEqual(report["images"][0]["symbols"][0]["blob_file_offset"],
                            report["images"][1]["symbols"][0]["blob_file_offset"])

    def test_duplicate_symbol_names_preserve_records(self):
        data = bytearray(fixture())
        # Rename the second function without changing its address/record identity.
        struct.pack_into("<I", data, 64 + 0x160 + 2 * 16, 1)
        symbols = analyze_fixture(data)["images"][0]["symbols"]
        duplicates = [s for s in symbols if s["name"] == "Arc_UartInit"]
        self.assertEqual(len(duplicates), 2)
        self.assertNotEqual(duplicates[0]["symbol_record_offset"], duplicates[1]["symbol_record_offset"])
        self.assertNotEqual(duplicates[0]["blob_file_offset"], duplicates[1]["blob_file_offset"])

    def test_symbol_budget_covers_repeated_tables_and_images(self):
        original = fixture()
        with mock.patch.object(MAP, "MAX_SYMBOL_RECORDS", 4):
            self.assertEqual(len(analyze_fixture(original)["images"]), 1)
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(original[:-20] + original[64:])
            data = bytearray(original)
            # Replace NOBITS with a second table over the same symbol bytes.
            data[64 + 0x200 + 2 * 40:64 + 0x200 + 3 * 40] = (
                data[64 + 0x200 + 4 * 40:64 + 0x200 + 5 * 40])
            with self.assertRaisesRegex(MAP.FormatError, "budget"):
                analyze_fixture(data, ("Arc_UartInit",))

    def test_string_table_budget_and_reuse(self):
        original = fixture()
        byte_count = analyze_fixture(original)["images"][0]["string_table_bytes"]
        with mock.patch.object(MAP, "MAX_STRING_TABLE_BYTES", byte_count - 1):
            with self.assertRaisesRegex(MAP.FormatError, "string-table byte budget"):
                analyze_fixture(original)
        with mock.patch.object(MAP, "MAX_STRING_TABLE_BYTES", byte_count):
            with self.assertRaisesRegex(MAP.FormatError, "string-table byte budget"):
                analyze_fixture(original[:-20] + original[64:])
            for size in (0, 16):
                # Additional empty/one-record tables share the existing strtab.
                data = bytearray(original[:-20] + bytes(40) + original[-20:])
                struct.pack_into("<H", data, 64 + 48, 7)
                section = (20, 2, 0, 0, 0x160, size, 3, 1, 4, 16)
                struct.pack_into("<10I", data, 64 + 0x200 + 6 * 40, *section)
                image = analyze_fixture(data)["images"][0]
                self.assertEqual(image["string_table_bytes"], byte_count)
                self.assertEqual(image["symbol_count"], 4 + size // 16)

    def test_rejects_changed_firmware_before_reporting_offsets(self):
        data = bytearray(MAP.read_firmware(BLOB))
        data[400] ^= 1
        with self.assertRaisesRegex(MAP.FormatError, "SHA-256"):
            MAP.analyze(data)

    def test_invalid_sizes_and_trailer(self):
        for data in (b"", bytes(20), bytes(25), bytes(MAP.MAX_FIRMWARE_SIZE + 4)):
            with self.subTest(size=len(data)), self.assertRaises(MAP.FormatError):
                analyze_fixture(data)
        data = bytearray(fixture())
        struct.pack_into("<I", data, len(data) - 20, 32)
        with self.assertRaisesRegex(MAP.FormatError, "trailer"):
            analyze_fixture(data)

    def test_malformed_header_tables_and_sections(self):
        # Offsets are ELF-relative, not original firmware-file offsets.
        mutations = (
            (4, "B", 2), (5, "B", 2), (6, "B", 0), (16, "H", 1), (18, "H", 40),
            (20, "I", 0), (28, "I", 0xfffffff0), (32, "I", 0xfffffff0),
            (40, "H", 0), (42, "H", 31), (44, "H", 0xffff), (46, "H", 39),
            (48, "H", 0), (50, "H", 6), (52 + 16, "I", 33),
            (0x200 + 40 + 16, "I", 0xfffffff0),
            (0x200 + 5 * 40 + 4, "I", 1),
            (0x200 + 4 * 40 + 20, "I", 63),
            (0x200 + 4 * 40 + 24, "I", 6),
            (0x200 + 4 * 40 + 36, "I", 0),
            (0x200 + 40, "I", 0xfffffff0),
            (0x160 + 16, "I", 0xfffffff0),
            (0x160 + 16 + 4, "I", 0x3fff),
            (0x160 + 16 + 8, "I", 33),
            (0x160 + 16 + 14, "H", 6),
            (0x160 + 16 + 14, "H", 0xffff),
            (52 + 8, "I", 0xfffffff0),
            (0x200 + 40 + 12, "I", 0xfffffff0),
        )
        for offset, fmt, value in mutations:
            data = bytearray(fixture())
            struct.pack_into("<" + fmt, data, 64 + offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(MAP.FormatError):
                analyze_fixture(data)

    def test_unselected_symbol_metadata_still_validated(self):
        for field, fmt, value in ((14, "H", 6), (4, "I", 0x3fff), (8, "I", 33)):
            data = bytearray(fixture())
            struct.pack_into("<" + fmt, data, 64 + 0x160 + 16 + field, value)
            with self.subTest(field=field), self.assertRaises(MAP.FormatError):
                analyze_fixture(data, ("Missing",))

    def test_unterminated_strings_and_truncated_embedded_image(self):
        data = bytearray(fixture())
        size = struct.unpack_from("<I", data, 64 + 0x200 + 3 * 40 + 20)[0]
        data[64 + 0x120:64 + 0x120 + size] = b"X" * size
        with self.assertRaisesRegex(MAP.FormatError, "ELF string"):
            analyze_fixture(data)
        data = fixture()[:-24] + struct.pack("<I", 16) + bytes(16)
        with self.assertRaisesRegex(MAP.FormatError, "outside"):
            analyze_fixture(data)

    def test_file_extent_overlap_rejected(self):
        original = fixture()
        data = bytearray(original[:-20] + original[64:])
        struct.pack_into("<I", data, 64 + 52 + 4, 0x2f0)
        with self.assertRaisesRegex(MAP.FormatError, "overlap"):
            analyze_fixture(data)

    def test_regular_file_open_is_read_only_and_size_limited(self):
        opened = []
        real_open = os.open
        def record_open(path, flags):
            opened.append((os.fspath(path), flags))
            self.assertFalse(flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC))
            return real_open(path, flags)
        with mock.patch.object(MAP.os, "open", side_effect=record_open):
            self.assertEqual(MAP.read_firmware(BLOB), BLOB.read_bytes())
        self.assertEqual(len(opened), 2)
        self.assertTrue(opened[0][1] & os.O_PATH)
        self.assertTrue(opened[1][0].startswith("/proc/self/fd/"))
        with mock.patch.object(MAP.os, "open", side_effect=record_open):
            with self.assertRaises(MAP.FormatError):
                MAP.read_firmware(ROOT)
        self.assertEqual(len(opened), 3)

    def test_special_files_rejected_before_read_open(self):
        with tempfile.TemporaryDirectory(prefix="crystalhd-fw-map-") as directory:
            root = Path(directory)
            os.mkfifo(root / "fifo")
            (root / "symlink").symlink_to(BLOB)
            for path in (root / "fifo", root / "symlink", Path("/dev/null")):
                calls = []
                real_open = os.open
                def record_open(name, flags):
                    calls.append(flags)
                    return real_open(name, flags)
                with mock.patch.object(MAP.os, "open", side_effect=record_open):
                    with self.assertRaisesRegex(MAP.FormatError, "regular file"):
                        MAP.read_firmware(path)
                self.assertEqual(len(calls), 1)
                self.assertTrue(calls[0] & os.O_PATH)

    def test_all_descriptors_closed_on_failure(self):
        descriptors = []
        real_open = os.open
        def record_open(path, flags):
            fd = real_open(path, flags)
            descriptors.append(fd)
            return fd
        with mock.patch.object(MAP.os, "open", side_effect=record_open):
            with mock.patch.object(MAP.os, "fdopen", side_effect=OSError("injected")):
                with self.assertRaises(OSError):
                    MAP.read_firmware(BLOB)
        # Test cleanup remains bounded if ownership is broken.
        leaked = []
        for fd in descriptors:
            try:
                os.fstat(fd)
            except OSError:
                continue
            leaked.append(fd)
            os.close(fd)
        self.assertEqual(leaked, [])

    def test_second_open_read_and_size_change_failures(self):
        for failure in ("open", "read", "size"):
            descriptors = []
            real_open = os.open
            real_fdopen = os.fdopen
            def record_open(path, flags):
                if len(descriptors) == 1 and failure == "open":
                    raise OSError("injected second open failure")
                fd = real_open(path, flags)
                descriptors.append(fd)
                return fd
            def file_stream(fd, mode):
                source = real_fdopen(fd, mode)
                result = mock.MagicMock()
                result.__enter__.return_value = result
                result.__exit__.side_effect = lambda *args: source.close()
                if failure == "read":
                    result.read.side_effect = OSError("injected read failure")
                else:
                    result.read.return_value = b"short"
                return result
            with self.subTest(failure=failure):
                with mock.patch.object(MAP.os, "open", side_effect=record_open):
                    with mock.patch.object(MAP.os, "fdopen", side_effect=file_stream):
                        with self.assertRaises((OSError, MAP.FormatError)):
                            MAP.read_firmware(BLOB)
                for fd in descriptors:
                    with self.assertRaises(OSError):
                        os.fstat(fd)

    def test_inode_pin_survives_pathname_replacement(self):
        with tempfile.TemporaryDirectory(prefix="crystalhd-fw-map-") as directory:
            path = Path(directory) / "firmware.bin"
            data = fixture()
            path.write_bytes(data)
            real_open = os.open
            def replace_path(name, flags):
                fd = real_open(name, flags)
                if flags & os.O_PATH:
                    path.unlink()
                    path.symlink_to("/dev/null")
                return fd
            with mock.patch.object(MAP.os, "open", side_effect=replace_path):
                self.assertEqual(MAP.read_firmware(path), data)

    def test_cli_json_reproducible_and_input_unchanged(self):
        before = hashlib.sha256(BLOB.read_bytes()).hexdigest()
        command = [sys.executable, "-B", str(TOOL), str(BLOB), "--symbol", "ArcGetc"]
        first = subprocess.run(command, capture_output=True, timeout=10)
        second = subprocess.run(command, capture_output=True, timeout=10)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        report = json.loads(first.stdout)
        self.assertTrue(all([s["name"] for s in i["symbols"]] == ["ArcGetc"]
                            for i in report["images"]))
        self.assertEqual(hashlib.sha256(BLOB.read_bytes()).hexdigest(), before)
        self.assertNotIn(str(ROOT).encode(), first.stdout)

    def test_cli_reference_json_is_reproducible_and_explicitly_not_a_call_graph(self):
        command = [sys.executable, "-B", str(TOOL), str(BLOB), "--references", "--symbol", "ReadLine"]
        before = hashlib.sha256(BLOB.read_bytes()).hexdigest()
        first = subprocess.run(command, capture_output=True, timeout=10)
        second = subprocess.run(command, capture_output=True, timeout=10)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        report = json.loads(first.stdout)
        self.assertTrue(any("not a proven instruction call graph" in text
                            for text in report["limitations"]))
        for image in report["images"]:
            self.assertEqual([s["name"] for s in image["symbols"]], ["ReadLine"])
            self.assertTrue(image["references"])
            for ref in image["references"]:
                owners = ref["source"]["function_owners"]
                self.assertTrue(ref["target"]["symbol"]["name"] == "ReadLine" or
                                any(owner["name"] == "ReadLine" for owner in owners))
        self.assertNotIn(str(ROOT).encode(), first.stdout)
        self.assertEqual(hashlib.sha256(BLOB.read_bytes()).hexdigest(), before)

    def test_cli_all_symbols_and_malformed_references(self):
        data = relocation_fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data):
            with mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output:
                result = MAP.main(["fixture.bin", "--expect-sha256", hashlib.sha256(data).hexdigest(),
                                   "--references", "--all-symbols", "--symbol", "Missing"])
        self.assertEqual(result, 0)
        image = json.loads(output.getvalue())["images"][0]
        self.assertEqual(len(image["symbols"]), 4)
        self.assertEqual(len(image["references"]), 4)
        self.assertEqual(image["missing_symbols"], ["Missing"])
        self.assertEqual(len(image["sections"]), 8)
        malformed = bytearray(data)
        struct.pack_into("<I", malformed, 64 + 0x380 + 4, (4 << 8) | 4)
        with mock.patch.object(MAP, "read_firmware", return_value=malformed):
            with mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output:
                with mock.patch.object(sys, "stderr", new_callable=io.StringIO) as error:
                    result = MAP.main(["fixture.bin", "--expect-sha256", hashlib.sha256(malformed).hexdigest(),
                                       "--references"])
        self.assertEqual(result, 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("symbol index", error.getvalue())

    def test_cli_failed_hash_emits_no_json(self):
        result = subprocess.run([sys.executable, "-B", str(TOOL), str(BLOB),
                                 "--expect-sha256", "0" * 64], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"SHA-256", result.stderr)

    def test_cli_explicit_alternative_hash(self):
        data = fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data):
            with mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output:
                result = MAP.main(["fixture.bin", "--expect-sha256", hashlib.sha256(data).hexdigest()])
        self.assertEqual(result, 0)
        self.assertFalse(json.loads(output.getvalue())["bundled_baseline"])

    def test_cli_invalid_hash_fails_without_read(self):
        with mock.patch.object(MAP, "read_firmware", side_effect=AssertionError("unexpected open")):
            with mock.patch.object(sys, "stderr"):
                with self.assertRaises(SystemExit) as result:
                    MAP.main([str(BLOB), "--expect-sha256", "bad"])
        self.assertEqual(result.exception.code, 2)


class FirmwareBootstrapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]

    def test_bundled_catalog_regions_and_static_layout(self):
        result = MAP.analyze(self.data, bootstrap=True)["bootstrap"]
        self.assertEqual((result["isa"], result["endianness"]), ("A32", "little"))
        self.assertEqual([(r["blob_file_offset"], r["blob_file_end"]) for r in result["regions"]],
                         [(0, 0x2ea60), (0x2ea60, 0x79dd8),
                          (0x79dd8, 0xcfbb0), (0xcfbb0, 0xd3000)])
        self.assertIn("literals and data", result["regions"][0]["kind"])
        self.assertIn("initialized data", result["regions"][3]["kind"])
        catalog = result["image_catalog"]
        self.assertEqual(catalog["table_blob_file_offset"], 0xcfbe8)
        self.assertEqual(catalog["descriptor_blob_file_offsets"], [0xcfbd0, 0xcfbdc, 0, 0, 0, 0])
        self.assertEqual(catalog["callback_entry_blob_file_offsets"], [0x26e7c, 0x26f74, 0x26fdc])
        for index, image in enumerate(catalog["images"]):
            self.assertEqual(image["slot"], index)
            self.assertEqual(image["image_blob_file_offset"], self.images[index]["blob_file_offset"])
            self.assertEqual(image["image_blob_file_end"], self.images[index]["blob_file_end"])
            self.assertEqual(image["declared_image_size"], image["image_blob_file_end"] - image["image_blob_file_offset"])
        self.assertEqual(result["cmac"], {"length_slot_blob_file_offset": 0xd3000,
                                          "blob_file_offset": 0xd3004, "length": 16,
                                          "authentication_verified": False})
        layout = result["static_derived_layout"]
        self.assertFalse(layout["device_observed"])
        self.assertEqual([layout[k]["address"] for k in ("scrub_end", "host_command", "reply")],
                         [0xd2fff, 0xd3100, 0xd3200])
        self.assertEqual(layout["host_command"]["source_kind"], "driver")
        self.assertIn("FleaDefs.h:51", layout["host_command"]["source"])
        references = layout["host_command"]["source"].split("; ")
        self.assertEqual(len(references), 2)
        for source, expected_path, statement in zip(references,
                ("driver/linux/FleaDefs.h", "driver/linux/crystalhd_fleafuncs.c"),
                ("#define DDRADDR_4_FWCMDS 0x100",
                 "hw->fwcmdPostAddr = borchStachAddr+1+DDRADDR_4_FWCMDS;")):
            path, line = source.rsplit(":", 1)
            self.assertEqual(path, expected_path)
            actual = (ROOT / path).read_text().splitlines()[int(line) - 1]
            self.assertEqual(" ".join(actual.split()), statement)
        self.assertEqual(layout["reply"]["source_kind"], "driver and ARM anchors")
        self.assertTrue(any("destinations are not established" in text for text in result["limitations"]))
        self.assertTrue(any("not CMAC authentication" in text for text in result["limitations"]))

    def test_bundled_actual_arm_dispatch_and_callback_anchors(self):
        result = MAP._bootstrap_map(self.payload, self.images)
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        self.assertEqual(len(anchors), len(result["instruction_anchors"]))
        self.assertLessEqual(len(anchors), MAP.MAX_BOOTSTRAP_ANCHORS)
        self.assertTrue(all(offset < 0x2ea60 for offset in anchors))
        self.assertEqual(anchors[0]["literal_value"], 0x2ca00)
        self.assertEqual(anchors[0x8d54]["literal_blob_file_offset"], 0x8f74)
        self.assertEqual(anchors[0x8d54]["literal_value"], 0x100e0000)
        self.assertEqual(anchors[0x8d58]["word"], 0xe590401c)
        for source, target in ((0x2cc0c, 0x74bc), (0x7530, 0x8d98),
                               (0x8d74, 0x8cf4), (0x8d14, 0x8c30),
                               (0x8e40, 0x9048), (0x9204, 0x5f2c),
                               (0x5fac, 0x6264), (0x6270, 0x5ba4)):
            self.assertEqual(anchors[source]["target_blob_file_offset"], target)
        self.assertEqual(anchors[0x5fac]["condition"], 0)
        self.assertEqual(anchors[0x9298]["literal_blob_file_offset"], 0x8be8)
        self.assertEqual(anchors[0x9298]["literal_value"], 0x100f6000)
        self.assertEqual(result["host_mailbox_dispatch"]["get_version_command"], 0x73763004)
        self.assertEqual({f["entry_blob_file_offset"] for f in result["function_anchors"]},
                         {0x5f2c, 0x5ba4, 0x26e7c, 0x26f74, 0x26fdc})

    def test_bootstrap_public_pin_precedes_elf_and_anchor_parsing(self):
        altered = bytearray(self.data)
        altered[400] ^= 1
        for data in (fixture(), altered, self.data[:-4]):
            with self.subTest(size=len(data)):
                with mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")):
                    with mock.patch.object(MAP, "_bootstrap_map", side_effect=AssertionError("unexpected anchor parse")):
                        with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                            MAP.analyze(data, expected_sha256=hashlib.sha256(data).hexdigest(), bootstrap=True)
        # Even a forged expected identity cannot bypass the independent size gate.
        with mock.patch.object(MAP.hashlib, "sha256") as digest:
            digest.return_value.hexdigest.return_value = MAP.BUNDLED_SHA256
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(fixture(), bootstrap=True)

    def test_bundled_host_open_low_byte_policy_and_failure_reply(self):
        result = MAP._bootstrap_map(self.payload, self.images)
        policy = result["host_channel_open_policy"]
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        self.assertEqual((policy["command"], policy["dispatcher_call_blob_file_offset"],
                          policy["entry_blob_file_offset"]), (0x73763100, 0x62d8, 0x51c8))
        self.assertEqual(anchors[0x62d8]["target_blob_file_offset"], 0x51c8)
        self.assertEqual((policy["record_buffer_bytes"], policy["request_record_offset"],
                          policy["reply_record_offset"]), (256, 0x14, 0x114))
        self.assertEqual((policy["algorithm_word_index"], policy["algorithm_bits_compared"],
                          policy["algorithm_load_blob_file_offset"]), (9, 8, 0x5294))
        self.assertFalse(policy["algorithm_upper_bits_checked"])
        self.assertEqual(anchors[0x5294]["word"], 0xe5d60024)
        routes = policy["comparison_routes"]
        self.assertEqual([(r["low_byte_selector"], r["target_blob_file_offset"]) for r in routes],
                         [(1, 0x5408), (0, 0x5420), (4, 0x5438),
                          (7, 0x5454), (6, 0x547c), (8, 0x55b0)])
        for route in routes:
            self.assertEqual(set(route), {"low_byte_selector", "compare_blob_file_offset",
                                          "branch_blob_file_offset", "target_blob_file_offset"})
            self.assertEqual(anchors[route["compare_blob_file_offset"]]["word"],
                             0xe3500000 | route["low_byte_selector"])
            jump = anchors[route["branch_blob_file_offset"]]
            self.assertEqual((jump["condition"], jump["target_blob_file_offset"]),
                             (0, route["target_blob_file_offset"]))
        self.assertEqual(policy["fallback"], {
            "entry_blob_file_offset": 0x52d4, "channel_id_word_index": 3,
            "channel_id": 0xffffffff, "status_word_index": 2, "status": 0xffffffff,
            "sequence_copy_blob_file_offsets": [0x52e0, 0x52e4],
            "internal_return": 2, "common_exit_blob_file_offset": 0x5218})
        self.assertEqual(policy["preconditions"], {
            "state_word_equals": 1, "state_check_blob_file_offset": 0x51f4,
            "free_channel_slot_required": True, "slot_scan_limit": 4,
            "slot_check_blob_file_offset": 0x5250, "device_observed": False})
        self.assertFalse(policy["device_observed"])
        self.assertIn("not channel-open or codec capability proof", policy["scope"])

    def test_host_open_named_rejections_and_word_fields_match_headers(self):
        policy = MAP._bootstrap_map(self.payload, self.images)["host_channel_open_policy"]
        header = (ROOT / "include/7411d.h").read_text()
        self.assertEqual([(r["name"], r["value"]) for r in policy["named_rejected_selectors"]],
                         [("H261", 2), ("H263", 3), ("MPEG1", 5)])
        matched = {r["low_byte_selector"] for r in policy["comparison_routes"]}
        for rejected in policy["named_rejected_selectors"]:
            pattern = rf"\beC011_VIDEO_ALG_{rejected['name']}\s*=\s*0x{rejected['value']:08x}\b"
            self.assertRegex(header, pattern)
            line = int(rejected["source"].rsplit(":", 1)[1])
            self.assertRegex(header.splitlines()[line - 1], pattern)
            self.assertNotIn(rejected["value"], matched)
        native = (ROOT / "driver/linux/crystalhd_fw_if.h").read_text()
        request = native.split("struct crystalhd_fw_channel_open_cmd {", 1)[1].split("};", 1)[0]
        reply = native.split("struct DecRspChannelChannelOpen {", 1)[1].split("};", 1)[0]
        request_fields = re.findall(r"uint32_t\s+(\w+)\s*;", request)
        reply_fields = re.findall(r"uint32_t\s+(\w+)\s*;", reply)
        self.assertEqual(request_fields[policy["algorithm_word_index"]], "video_algorithm")
        self.assertEqual(reply_fields[policy["fallback"]["channel_id_word_index"]], "ChannelID")
        self.assertEqual(reply_fields[policy["fallback"]["status_word_index"]], "status")
        self.assertEqual((request_fields[1], reply_fields[1]), ("sequence", "sequence"))

    def test_host_open_ladder_load_and_reply_semantic_mutations(self):
        for offset, replacement in (
                (0x5294, 0xe5960024),  # word load would no longer prove low-byte selection
                (0x5294, 0xe5d60020),  # wrong request field
                (0x52a0, 0xe3500002), (0x52b0, 0xe3500003), (0x52cc, 0xe3500005),
                (0x52a4, 0x1a000057), (0x52c8, 0x0a00006c),
                (0x52d0, 0xea0000b6), (0x52dc, 0xe5848008),
                (0x52e0, 0xe5960000), (0x52e4, 0xe5840000),
                (0x52e8, 0xe584800c), (0x52ec, 0xe3a00000),
                (0x51ec, 0xe3a08000), (0x51f4, 0xe3500000),
                (0x525c, 0xe3550005), (0x51e0, 0xe2806018),
                (0x51e4, 0xe2804f46), (0x5f4c, 0xe3002080)):
            data = bytearray(self.payload)
            struct.pack_into("<I", data, offset, replacement)
            with self.subTest(offset=offset, word=replacement), self.assertRaises(MAP.FormatError):
                MAP._bootstrap_map(data, self.images)

    def test_bootstrap_fixed_anchor_budget(self):
        result = MAP._bootstrap_map(self.payload, self.images)
        self.assertEqual(len(result["instruction_anchors"]), 256)
        self.assertLessEqual(len(result["instruction_anchors"]), MAP.MAX_BOOTSTRAP_ANCHORS)
        with mock.patch.object(MAP, "MAX_BOOTSTRAP_ANCHORS", 255):
            with self.assertRaisesRegex(MAP.FormatError, "anchor budget"):
                MAP._bootstrap_map(self.payload, self.images)

    def test_host_decoder_start_context_handoff_and_cached_selector_mapping(self):
        result = MAP._bootstrap_map(self.payload, self.images)
        start = result["host_decoder_start"]
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        context = start["context_link"]
        self.assertEqual((context["host_interface_global_address"],
                          context["init_output_pointer_address"], context["context_address"]),
                         (0xd1ff4, 0xd1ff8, 0xd3a00))
        self.assertEqual(anchors[0x5d44]["literal_value"], context["init_output_pointer_address"])
        self.assertEqual(anchors[0x5d4c]["target_blob_file_offset"], 0x54c)
        self.assertEqual(anchors[0x558]["word"], 0xe1a07001)  # saved init out-pointer
        self.assertEqual(anchors[0x880]["word"], 0xe5874000)  # *out-pointer = context
        for offset in (0x56c, 0x898):
            self.assertEqual(anchors[offset]["literal_value"], context["context_address"])
        self.assertGreater(context["context_address"], len(self.payload))
        self.assertEqual((context["slot_stride_bytes"], context["cached_algorithm_byte_offset"]),
                         (0x1cc, 0xd0))
        self.assertEqual([(r["host_open_low_byte"], r["cached_algorithm_byte"],
                           r["inner_start_algorithm_byte"], r["cache_store_blob_file_offset"])
                          for r in start["selector_mapping"]],
                         [(0, 0, 0, 0x5428), (1, 1, 1, 0x5410), (4, 4, 4, 0x5444),
                          (6, 8, 8, 0x5484), (7, 7, 4, 0x5460), (8, 8, 8, 0x55b8)])
        admitted = {r["low_byte_selector"] for r in result["host_channel_open_policy"]["comparison_routes"]}
        self.assertEqual(admitted, {r["host_open_low_byte"] for r in start["selector_mapping"]})
        self.assertTrue(all(set(r) == {"host_open_low_byte", "cached_algorithm_byte",
                                       "inner_start_algorithm_byte", "cache_store_blob_file_offset"}
                            for r in start["selector_mapping"]))
        self.assertFalse(start["device_observed"])

    def test_host_decoder_start_dispatch_fields_and_config_path(self):
        result = MAP._bootstrap_map(self.payload, self.images)
        start = result["host_decoder_start"]
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        self.assertEqual((start["command"], start["dispatcher_call_blob_file_offset"],
                          start["entry_blob_file_offset"]), (0x7376311a, 0x6688, 0x4630))
        self.assertEqual(anchors[0x6050]["target_blob_file_offset"], 0x667c)
        self.assertEqual(anchors[0x6688]["target_blob_file_offset"], start["entry_blob_file_offset"])
        self.assertEqual(start["request_channel_word_index"], 2)
        header = (ROOT / "include/7411d.h").read_text()
        line = int(start["command_source"].rsplit(":", 1)[1])
        self.assertRegex(header.splitlines()[line - 1],
                         r"eCMD_C011_DEC_CHAN_START_VIDEO\s*=\s*eCMD_C011_CMD_BASE\s*\+\s*0x11A")
        self.assertRegex(header, r"#define\s+eCMD_C011_CMD_BASE\s+\(0x73763000\)")
        native = (ROOT / "driver/linux/crystalhd_fw_if.h").read_text()
        request = native.split("struct crystalhd_fw_channel_start_video_cmd {", 1)[1].split("};", 1)[0]
        fields = re.findall(r"uint32_t\s+(\w+)\s*;", request)
        self.assertEqual(fields[start["request_channel_word_index"]], "channel_id")
        self.assertEqual(anchors[start["request_channel_load_blob_file_offset"]]["word"], 0xe5957008)
        self.assertEqual(start["preconditions"], {"opened_byte_equals": 1, "opened_byte_offset": 0xc4,
                                                "check_blob_file_offset": 0x4678,
                                                "channel_range_validation_established": False,
                                                "device_observed": False})
        self.assertEqual(anchors[0x467c]["target_blob_file_offset"], 0x4808)
        config = start["configuration"]
        self.assertEqual((config["bytes"], config["channel_configuration_offset"]), (56, 0x68))
        self.assertEqual(config["copy_call_blob_file_offsets"], [0xeed8, 0xf148])
        self.assertEqual(anchors[0xeed0]["literal_value"], 0x2dd88)
        self.assertEqual(anchors[config["cache_load_blob_file_offset"]]["word"], 0xe5d720d0)
        self.assertEqual(anchors[config["first_byte_store_blob_file_offset"]]["word"], 0xe5cd2004)
        for offset in config["copy_call_blob_file_offsets"]:
            self.assertEqual(anchors[offset]["target_blob_file_offset"], 0x20708)
        self.assertEqual(anchors[config["normalize_compare_blob_file_offset"]]["word"], 0xe3500007)
        self.assertEqual(anchors[config["normalize_store_blob_file_offset"]]["word"], 0xe5c40068)

    def test_host_decoder_inner_packet_transport_and_evidence_limits(self):
        result = MAP._bootstrap_map(self.payload, self.images)
        start = result["host_decoder_start"]
        packet = start["inner_packet"]
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        self.assertEqual((packet["command"], packet["command_word_index"], packet["algorithm_word_index"]),
                         (0x73760005, 0, 1))
        self.assertEqual(anchors[0x27704]["literal_value"], packet["command"])
        self.assertEqual(anchors[packet["algorithm_store_blob_file_offset"]]["word"], 0xe5859004)
        self.assertEqual(anchors[0xf3a0]["target_blob_file_offset"], packet["entry_blob_file_offset"])
        for field, target in (("transport_call_blob_file_offset", 0x2705c),
                              ("copy_call_blob_file_offset", 0x20708),
                              ("register_write_call_blob_file_offset", 0x25024),
                              ("wait_call_blob_file_offset", 0x20598),
                              ("unchecked_continuation_blob_file_offset", 0x206a0)):
            self.assertEqual(anchors[packet[field]]["target_blob_file_offset"], target)
        self.assertEqual(packet["copy_bytes"], 252)
        self.assertTrue(packet["publication_is_path_dependent"])
        self.assertFalse(packet["return_checked_by_caller"])
        self.assertIn("not inner decoder acceptance or codec capability proof", start["scope"])
        self.assertTrue(any("earlier START state/configuration failures" in s for s in start["limitations"]))
        self.assertTrue(any("not RAVE parser protocol enums" in s for s in start["limitations"]))
        self.assertTrue(any("AVS and MVC reachability" in s for s in start["limitations"]))

    def test_host_decoder_cache_config_packet_and_status_semantic_mutations(self):
        for offset, replacement in (
                (0x5ed4, 0xd1ffc), (0x6fc, 0xd3a04), (0x558, 0xe1a07000),
                (0x880, 0xe5875000), (0x5278, 0xe0050095), (0x5298, 0xe3a0a000),
                (0x543c, 0xe3a00005), (0x5458, 0xe3a00004), (0x5484, 0xe5c000d0),
                (0x55b8, 0xe5c010d1), (0x6050, 0xea000189), (0x4644, 0xe595700c),
                (0x4678, 0xe3500002), (0x1054, 0xe59720d0), (0x1058, 0xe5cd2008),
                (0xeecc, 0xe3a02034), (0xf13c, 0xe3a0203c), (0xf144, 0xe284006c),
                (0xf1d8, 0xe3500006), (0xf1dc, 0xea000001), (0xf1e0, 0xe3a00008),
                (0xf390, 0xe5d42069), (0x276c0, 0xe1a09003), (0x27aac, 0x73760006),
                (0x2770c, 0xe5859008), (0x27730, 0xe1a02006), (0x27744, 0xe3a00000),
                (0xf3a4, 0xe3500000), (0x270ac, 0xe3a020f8), (0x2502c, 0xe7831001)):
            data = bytearray(self.payload)
            struct.pack_into("<I", data, offset, replacement)
            with self.subTest(offset=offset, word=replacement), self.assertRaises(MAP.FormatError):
                MAP._bootstrap_map(data, self.images)

    def test_bootstrap_cli_alternative_pin_fails_without_json(self):
        data = fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data):
            with mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output:
                with mock.patch.object(sys, "stderr", new_callable=io.StringIO) as error:
                    result = MAP.main(["fixture.bin", "--bootstrap", "--expect-sha256",
                                       hashlib.sha256(data).hexdigest()])
        self.assertEqual(result, 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("exact bundled", error.getvalue())

    def test_a32_branch_pc_bias_sign_extension_and_conditions(self):
        data = bytearray(64)
        for offset, word, target, options in (
                (16, 0xeafffffb, 4, {}), (4, 0xeb000004, 28, {"link": True}),
                (8, 0x0a000000, 16, {"condition": 0}),
                (8, 0x1a000000, 16, {"condition": 1}),
                (8, 0xba000000, 16, {"condition": 11}),
                (8, 0xca000000, 16, {"condition": 12})):
            struct.pack_into("<I", data, offset, word)
            result = MAP._a32_branch(data, offset, **options)
            self.assertEqual(result["target_blob_file_offset"], target)
            self.assertEqual(result["word"], word)
        for word, options in ((0xfa000000, {}), (0xeb000000, {}),
                              (0x1a000000, {}), (0x0a000000, {}),
                              (0xfa000000, {"condition": 15}),
                              (0x2a000000, {"condition": 2}),
                              (0xf000f800, {"link": True}),
                              (0xea7fffff, {}), (0xea800000, {})):
            struct.pack_into("<I", data, 16, word)
            with self.subTest(word=word), self.assertRaises(MAP.FormatError):
                MAP._a32_branch(data, 16, **options)
        with self.assertRaises(MAP.FormatError):
            MAP._a32_branch(data, 1)
        with self.assertRaises(MAP.FormatError):
            MAP._a32_branch(data[:17], 16)

    def test_a32_literal_pc_bias_positive_negative_and_rejected_modes(self):
        for offset, instruction, literal in ((8, 0xe59f3004, 20), (16, 0xe51f3010, 8)):
            data = bytearray(32)
            struct.pack_into("<I", data, offset, instruction)
            struct.pack_into("<I", data, literal, 0x12345678)
            result = MAP._a32_literal(data, offset)
            self.assertEqual(result["literal_blob_file_offset"], literal)
            self.assertEqual(result["literal_value"], 0x12345678)
            self.assertEqual(result["destination_register"], 3)
        for instruction in (0xe51f000c, 0xe59f0fff, 0xe59f0001, 0xe79f0000,
                            0xe5bf0000, 0xe5df0000, 0x059f0000, 0xf59f0000):
            data = bytearray(32)
            struct.pack_into("<I", data, 0, instruction)
            with self.subTest(word=instruction), self.assertRaises(MAP.FormatError):
                MAP._a32_literal(data, 0)
        with self.assertRaises(MAP.FormatError):
            MAP._a32_literal(bytes.fromhex("00009fe5"), 0)

    def test_every_bootstrap_instruction_anchor_rejects_changed_word(self):
        result = MAP._bootstrap_map(self.payload, self.images)
        for anchor in result["instruction_anchors"]:
            data = bytearray(self.payload)
            offset = anchor["blob_file_offset"]
            struct.pack_into("<I", data, offset, anchor["word"] ^ 1)
            with self.subTest(offset=offset), self.assertRaises(MAP.FormatError):
                MAP._bootstrap_map(data, self.images)

    def test_bootstrap_literals_and_vector_targets_reject_mutation(self):
        result = MAP._bootstrap_map(self.payload, self.images)
        for anchor in result["instruction_anchors"]:
            if anchor["operation"] != "LDR literal":
                continue
            data = bytearray(self.payload)
            offset = anchor["literal_blob_file_offset"]
            struct.pack_into("<I", data, offset, anchor["literal_value"] ^ 4)
            with self.subTest(offset=offset), self.assertRaises(MAP.FormatError):
                MAP._bootstrap_map(data, self.images)
        with self.assertRaisesRegex(MAP.FormatError, "payload size"):
            MAP._bootstrap_map(self.payload[:-4], self.images)

    def test_bootstrap_catalog_root_slots_and_callback_mutations(self):
        offsets = [0xcfc00] + list(range(0xcfbe8, 0xcfc00, 4)) + list(range(0xcfcf0, 0xcfcfc, 4))
        for offset in offsets:
            data = bytearray(self.payload)
            struct.pack_into("<I", data, offset, 0xffffffff)
            with self.subTest(offset=offset), self.assertRaisesRegex(MAP.FormatError, "catalog"):
                MAP._bootstrap_map(data, self.images)

    def test_bootstrap_descriptor_pointer_bounds_and_elf_size_identity(self):
        for descriptor in (0xcfbd0, 0xcfbdc):
            for field in (0, 4, 8):
                for value in (1, 0xd3000, 0xfffffffc, 0x2ea64):
                    data = bytearray(self.payload)
                    struct.pack_into("<I", data, descriptor + field, value)
                    with self.subTest(descriptor=descriptor, field=field, value=value):
                        with self.assertRaises(MAP.FormatError):
                            MAP._bootstrap_map(data, self.images)
        for offset in (0xcfbb0, 0xcfbb4, 0xcfbcc, 0xcfbb8):
            data = bytearray(self.payload)
            struct.pack_into("<I", data, offset, 0xffffffff)
            with self.subTest(offset=offset), self.assertRaises(MAP.FormatError):
                MAP._bootstrap_map(data, self.images)
        for images in (self.images[::-1], self.images[:1], self.images * 2,
                       [{**self.images[0], "blob_file_end": 0x79dd4}, self.images[1]]):
            with self.subTest(images=len(images)), self.assertRaisesRegex(MAP.FormatError, "ELF identities"):
                MAP._bootstrap_map(self.payload, images)

    def test_bootstrap_pure_mapping_and_unmodified_existing_report_options(self):
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file read")):
            with mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected device/file open")):
                MAP._bootstrap_map(self.payload, self.images)
        for options in ({}, {"references": True}, {"all_symbols": True},
                        {"references": True, "all_symbols": True}):
            plain = MAP.analyze(self.data, **options)
            enriched = MAP.analyze(self.data, bootstrap=True, **options)
            self.assertNotIn("bootstrap", plain)
            enriched.pop("bootstrap")
            self.assertEqual(enriched, plain)

    def test_default_cli_stdout_bytes_unchanged_and_bootstrap_reproducible(self):
        for options, expected in (([], "15068c07a81a510e02435b6203b1cf5e424152e37ff456b1da9c12b77c15799e"),
                                  (["--references", "--symbol", "ReadLine"], "2a491f4b9033395cdbc688810319ea159973a296d7828ac65153bc93d45b588f"),
                                  (["--all-symbols"], "15af1023987b5f2aee1e9a5560f749560bf33f729252ca3fcb9f41358f49e5aa")):
            command = [sys.executable, "-B", str(TOOL), str(BLOB)] + options
            plain = subprocess.run(command, capture_output=True, timeout=10)
            enriched = subprocess.run(command + ["--bootstrap"], capture_output=True, timeout=10)
            with self.subTest(options=options):
                self.assertEqual(plain.returncode, 0, plain.stderr)
                self.assertEqual(hashlib.sha256(plain.stdout).hexdigest(), expected)
                self.assertEqual(enriched.returncode, 0, enriched.stderr)
                report = json.loads(enriched.stdout)
                report.pop("bootstrap")
                self.assertEqual(report, json.loads(plain.stdout))
                self.assertNotIn(str(ROOT).encode(), enriched.stdout)
        command = [sys.executable, "-B", str(TOOL), str(BLOB), "--bootstrap"]
        first = subprocess.run(command, capture_output=True, timeout=10)
        second = subprocess.run(command, capture_output=True, timeout=10)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        self.assertEqual(hashlib.sha256(BLOB.read_bytes()).hexdigest(), MAP.BUNDLED_SHA256)


class FirmwareArmPpbHandoffTests(unittest.TestCase):
    REGIONS = (
        ("acquire", 0xd624, 176, "e6ff28c676a32fe6219c53e6f40f2f23589e6f7f1f7235e4a46c95baf619907a"),
        ("peek", 0xd718, 104, "013bcc90b5e7d904f03f9787cc7e820978a711eba2fb37c95a63a7b8324c374d"),
        ("release", 0xd5a4, 128, "8276e18c409aa706a5b3e92b880887c4157256253d7be40312e28a7464b9c812"),
        ("translate", 0x1fdac, 192, "08d03815210fc1e069068765847cc4c5d847bd5df744f223f9851ee985e05d28"),
    )

    @classmethod
    def setUpClass(cls):
        cls.payload = MAP.read_firmware(BLOB)[:-MAP.TRAILER_SIZE]

    def test_exact_metadata_contract_and_limits(self):
        report = MAP._arm_ppb_metadata_handoff(self.payload)
        self.assertEqual(MAP._ARM_PPB_HANDOFF_REGIONS, self.REGIONS)
        self.assertEqual(report["validated_bytes"], 600)
        self.assertEqual([(r["role"], r["blob_file_offset"], r["bytes"], r["sha256"])
                          for r in report["regions"]], list(self.REGIONS))
        self.assertEqual(report["ring"]["index_range"], [2, 63])
        self.assertEqual(report["record"]["physical_metadata_word_offset"], 4)
        self.assertEqual(report["acquire"]["ring_handle_offset"], 0x250)
        self.assertEqual(report["release"]["ring_handle_offset"], 0x254)
        self.assertTrue(report["acquire"]["null_output_still_consumes_nonempty"])
        self.assertFalse(report["acquire"]["translation_status_checked"])
        self.assertFalse(report["peek"]["null_ring_guard"])
        self.assertFalse(report["peek"]["read_index_modified"])
        self.assertEqual(report["release"]["full_ring_response"], "log and continue")
        self.assertEqual(report["release"]["invalid_index_response"], "log and continue")
        self.assertTrue(report["translation"]["candidate_written_before_bounds_check"])
        self.assertFalse(report["scope"]["raw_source_lease"])
        self.assertFalse(report["scope"]["public_PPB_layout_equivalence"])
        self.assertFalse(report["scope"]["all_consumer_completion"])
        self.assertFalse(report["scope"]["generation_safe_reuse"])

    def test_every_selected_body_byte_is_pinned(self):
        for role, offset, size, _ in self.REGIONS:
            for delta in range(size):
                with self.subTest(role=role, byte=delta):
                    changed = bytearray(self.payload)
                    changed[offset + delta] ^= 1
                    with self.assertRaises(MAP.FormatError):
                        MAP._arm_ppb_metadata_handoff(changed)

    def test_wrong_size_and_call_target_fail(self):
        for payload in (self.payload[:-1], self.payload + b"\0"):
            with self.assertRaises(MAP.FormatError):
                MAP._arm_ppb_metadata_handoff(payload)
        changed = bytearray(self.payload)
        changed[0xd69c] ^= 1
        regions = [(role, offset, size,
                    hashlib.sha256(changed[offset:offset + size]).hexdigest())
                   for role, offset, size, _ in self.REGIONS]
        with mock.patch.object(MAP, "_ARM_PPB_HANDOFF_REGIONS", regions):
            with self.assertRaises(MAP.FormatError):
                MAP._arm_ppb_metadata_handoff(changed)

    def test_opt_in_does_not_change_existing_output(self):
        data = MAP.read_firmware(BLOB)
        for picture_output in (False, True):
            plain = MAP.analyze(data, picture_output=picture_output)
            extra = MAP.analyze(data, picture_output=picture_output, ppb_handoff=True)
            self.assertEqual(extra.pop("arm_ppb_metadata_handoff"),
                             MAP._arm_ppb_metadata_handoff(self.payload))
            self.assertEqual(extra, plain)
        with self.assertRaises(MAP.FormatError):
            analyze_fixture(fixture(), ppb_handoff=True)

    def test_cli_option(self):
        result = subprocess.run([sys.executable, "-B", str(TOOL), str(BLOB), "--ppb-handoff"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=15, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, b"")
        self.assertEqual(json.loads(result.stdout)["arm_ppb_metadata_handoff"],
                         MAP._arm_ppb_metadata_handoff(self.payload))


class FirmwarePictureOutputTests(unittest.TestCase):
    FIR_REGIONS = (
        (0x21ac, 716, "cc9fc5e53343bac1fa521854cc209f7a2d39ed406800d62e3f1f810d19edabe7"),
        (0x1f8c, 544, "80d45cdf1110c163aff32f66ce053c2f6c938a8e2aa81691f4caee1071a315d7"),
        (0x1f38, 84, "3c0851eae9ea6eb0130b95d93fb0df7a7b04c2611a47267df370009fbf23dcf9"),
        (0x244c, 24, "444c4ced61e1d13e8b594ea40369034b319ed0c12771d35379c8335eb5865264"),
        (0x1e8e8, 12, "127fb56c30f3496c824443348ca74b9236add85c1381d8d0fae5bf61c0a9d927"),
        (0x2cdf0, 128, "6d77fc3ce84a6391ed6b30c21ddd525e21a54d38709662534668625c8fbb45a2"),
        (0x2ccf0, 256, "a3cba1c64837a0c6dfd06b9c032bd605a0ac73e2dd7a316f00b0d51aaf1bf82a"),
        (0x55d4, 156, "2235f36ad7d09336418f4d4571c4a761397529d4abb448b03fdd018573f2fbac"),
        (0x8518, 4, "c45be60a73538be6ea62105a869309a98a17942001e343b7fe3bfe95d17b5a64"),
        (0x8634, 4, "2d0d57c2380005c57f9c257fffdb7ea6f3c0e96a812d2e0aca79216d37d7e61e"))
    FIR_BANKS = (
        ("vertical", "VERT_FIR", 0x540900, 0x2cdf0, 32, 0x2380),
        ("vertical", "VERT_FIR_CHROMA", 0x540980, 0x2cdf0, 32, 0x23a8),
        ("horizontal", "HORIZ_FIR", 0x540a00, 0x2ccf0, 64, 0x23d4),
        ("horizontal", "HORIZ_FIR_CHROMA", 0x540b00, 0x2ccf0, 64, 0x23fc))
    FIR_SIGNED_CANDIDATE_ROWS = {
        "vertical": (
            (0, 0, 97, 830, 97, 0, 0, 0),
            (0, 0, 38, 817, 183, -14, 0, 0),
            (0, 0, 1, 750, 289, -16, 0, 0),
            (0, 0, -17, 651, 409, -19, 0, 0),
            (0, 0, -21, 533, 533, -21, 0, 0),
            (0, 0, -19, 409, 651, -17, 0, 0),
            (0, 0, -16, 289, 750, 1, 0, 0),
            (0, 0, -14, 183, 817, 38, 0, 0)),
        "horizontal": (
            (4, 0, -18, 36, -2, -119, 272, 678, 272, -119, -2, 36, -18, 0, 4, 0),
            (3, 2, -19, 28, 16, -123, 197, 670, 350, -105, -23, 44, -16, -3, 5, -2),
            (2, 4, -18, 19, 31, -118, 125, 646, 425, -80, -45, 48, -12, -7, 5, -1),
            (1, 5, -16, 10, 41, -106, 59, 608, 495, -44, -68, 50, -6, -10, 6, -1),
            (0, 6, -13, 1, 48, -89, 2, 557, 557, 2, -89, 48, 1, -13, 6, 0),
            (-1, 6, -10, -6, 50, -68, -44, 495, 608, 59, -106, 41, 10, -16, 5, 1),
            (-1, 5, -7, -12, 48, -45, -80, 425, 646, 125, -118, 31, 19, -18, 4, 2),
            (-2, 5, -3, -16, 44, -23, -105, 350, 670, 197, -123, 16, 28, -19, 2, 3))}

    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]

    def mapping(self, payload=None, images=None):
        return MAP._picture_output_map(self.payload if payload is None else payload,
                                       self.images if images is None else images)

    def fir_mapping(self, payload=None):
        return MAP._scaler_fir_map(self.payload if payload is None else payload)

    def execute_fir_loops(self, payload=None):
        # Interpret only the four fixed loops and the actual register writer.
        # Their context/base and initial r6=0 are synthetic entry conditions;
        # no printf/division call or device/host address is followed.
        payload = self.payload if payload is None else payload
        regs = [0] * 16
        regs[7], regs[15] = 0x400000, 0x2378
        writes, comparison = [], None
        addresses = {0x90000000 + base + index * 4
                     for _, _, base, _, count, _ in self.FIR_BANKS for index in range(count)}
        def read(address):
            if address == 0x400000:
                return 0x90000000
            self.assertTrue(0x244c <= address <= 0x2460 or
                            0x2ccf0 <= address <= 0x2ce6c, hex(address))
            self.assertEqual(address % 4, 0)
            return struct.unpack_from("<I", payload, address)[0]
        for step in range(4096):
            pc = regs[15]
            if pc == 0x2464:
                return writes, step
            self.assertTrue(0x2378 <= pc < 0x2420 or 0x1e8e8 <= pc < 0x1e8f4, hex(pc))
            self.assertEqual(pc % 4, 0)
            word = struct.unpack_from("<I", payload, pc)[0]
            regs[15] = pc + 4
            condition = word >> 28
            self.assertIn(condition, (11, 14))
            if condition == 11:
                self.assertIsNotNone(comparison)
                if not comparison[0] < comparison[1]:
                    continue
            reg = lambda index: pc + 8 if index == 15 else regs[index]
            if word == 0xe12fff1e:
                self.assertEqual(pc, 0x1e8f0)
                regs[15] = regs[14]
            elif word & 0x0e000000 == 0x0a000000:
                displacement = word & 0xffffff
                if displacement & 0x800000:
                    displacement -= 1 << 24
                target = pc + 8 + displacement * 4
                if word & 0x1000000:
                    self.assertEqual(target, 0x1e8e8)
                    regs[14] = pc + 4
                regs[15] = target
            elif word & 0x0c000000 == 0x04000000:
                self.assertTrue(word & (1 << 24))  # pre-indexed, no writeback
                self.assertFalse(word & ((1 << 22) | (1 << 21)))  # word, no writeback
                base, destination = (word >> 16) & 15, (word >> 12) & 15
                if word & (1 << 25):
                    self.assertIn(word & 0xff0, (0, 0x100))  # LSL #0 or #2 only
                    displacement = reg(word & 15) << ((word >> 7) & 31)
                else:
                    displacement = word & 0xfff
                address = reg(base) + (displacement if word & (1 << 23) else -displacement)
                if word & (1 << 20):
                    regs[destination] = read(address)
                else:
                    self.assertEqual(pc, 0x1e8ec)
                    self.assertIn(address, addresses)
                    writes.append((address, regs[destination]))
            else:
                self.assertEqual(word & 0x0c000000, 0)
                opcode, destination, base = (word >> 21) & 15, (word >> 12) & 15, (word >> 16) & 15
                self.assertIn(opcode, (4, 10, 13))
                if word & (1 << 25):
                    value, rotation = word & 255, ((word >> 8) & 15) * 2
                    right = ((value >> rotation) | (value << ((32 - rotation) % 32))) & 0xffffffff
                else:
                    self.assertEqual(word & 0xff0, 0)
                    right = reg(word & 15)
                if opcode == 10:
                    self.assertTrue(word & (1 << 20))
                    signed = lambda value: value if value < 0x80000000 else value - 0x100000000
                    comparison = (signed(reg(base)), signed(right))
                else:
                    self.assertFalse(word & (1 << 20))
                    regs[destination] = (reg(base) + right if opcode == 4 else right) & 0xffffffff
        self.fail("fixed FIR loops exceeded the 4096-instruction budget")

    def test_scaler_fir_exact_regions_and_bounded_private_entry(self):
        result = self.fir_mapping()
        self.assertEqual(result, MAP._scaler_fir_map(self.payload))
        regions = result["validation"]["validated_regions"]
        self.assertEqual(len(regions), 10)
        self.assertEqual([(r["blob_file_offset"], r["size_bytes"], r["sha256"]) for r in regions],
                         list(self.FIR_REGIONS))
        self.assertEqual([r["role"] for r in regions],
                         ["scaling_setup", "scaling_dispatch", "control_literals", "bank_literals",
                          "register_writer", "vertical_table", "horizontal_table", "open_scaling_fields",
                          "picture_call_8518", "picture_call_8634"])
        self.assertEqual(result["validation"]["validated_bytes"], 1928)
        self.assertEqual((MAP.MAX_SCALER_FIR_REGIONS, MAP.MAX_SCALER_FIR_BYTES), (12, 2048))
        for offset, size, digest in self.FIR_REGIONS:
            self.assertEqual(hashlib.sha256(self.payload[offset:offset + size]).hexdigest(), digest)
        with mock.patch.object(MAP, "MAX_SCALER_FIR_REGIONS", 10), \
                mock.patch.object(MAP, "MAX_SCALER_FIR_BYTES", 1928):
            self.assertEqual(MAP._scaler_fir_map(self.payload), result)
        for name, value in (("MAX_SCALER_FIR_REGIONS", 9), ("MAX_SCALER_FIR_BYTES", 1927)):
            with self.subTest(limit=name), mock.patch.object(MAP, name, value), self.assertRaises(MAP.FormatError):
                MAP._scaler_fir_map(self.payload)
        for payload in (b"", self.payload[:-1], self.payload + b"\0"):
            with self.subTest(size=len(payload)), self.assertRaises(MAP.FormatError):
                MAP._scaler_fir_map(payload)

    def test_scaler_fir_every_pinned_byte_rejects_mutation(self):
        with mock.patch.object(MAP, "_a32_literal", side_effect=AssertionError("decode before all pins")), \
                mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("decode before all pins")):
            for start, size, _ in self.FIR_REGIONS:
                for offset in range(start, start + size):
                    data = bytearray(self.payload)
                    data[offset] ^= 1
                    with self.subTest(offset=offset), self.assertRaises(MAP.FormatError):
                        MAP._scaler_fir_map(data)

    def test_scaler_fir_raw_tables_and_candidate_rows_not_hardware_format(self):
        result = self.fir_mapping()
        self.assertEqual((result["schema_version"], result["isa"], result["endianness"]), (1, "A32", "little"))
        self.assertIs(result["device_observed"], False)
        self.assertEqual(result["entry_blob_file_offset"], 0x21ac)
        self.assertEqual(result["writer"]["entry_blob_file_offset"], 0x1e8e8)
        self.assertIs(result["writer"]["physical_base_validated"], False)
        fmt = result["format"]
        expected_format = {"bits": 12, "even_tap_shift": 18, "odd_tap_shift": 2,
                           "signedness_confirmed": False, "fractional_precision_confirmed": False,
                           "normalization_candidate": 1024}
        self.assertEqual({key: fmt[key] for key in expected_format}, expected_format)
        self.assertIs(fmt["signedness_confirmed"], False)
        self.assertIs(fmt["fractional_precision_confirmed"], False)
        tables = result["tables"]
        self.assertEqual(len(tables), 2)
        for table, (axis, offset, count) in zip(tables, (("vertical", 0x2cdf0, 32),
                                                       ("horizontal", 0x2ccf0, 64))):
            with self.subTest(axis=axis):
                self.assertEqual((table["axis"], table["blob_file_offset"], table["size_bytes"]),
                                 (axis, offset, count * 4))
                self.assertEqual(table["name"], axis)
                raw = self.payload[offset:offset + count * 4]
                self.assertEqual(table["sha256"], hashlib.sha256(raw).hexdigest())
                words = list(struct.unpack("<" + "I" * count, raw))
                self.assertEqual(table["words"], words)
                self.assertIs(table["reserved_bits_zero"], True)
                self.assertTrue(all(word & 0xc003c003 == 0 for word in words))
                phases = table["phases"]
                self.assertEqual(len(phases), 8)
                for index, (phase, signed_row) in enumerate(zip(phases, self.FIR_SIGNED_CANDIDATE_ROWS[axis])):
                    self.assertEqual(phase["phase_index"], index)
                    self.assertEqual(phase["signed12_candidate_taps"], list(signed_row))
                    self.assertEqual(phase["unsigned12_taps"], [value % 4096 for value in signed_row])
                    self.assertEqual(phase["signed12_candidate_sum"], 1024)
                    self.assertEqual(sum(signed_row), 1024)
        self.assertTrue(result["limitations"])
        self.assertTrue(all(isinstance(text, str) and text for text in result["limitations"]))

    def test_scaler_fir_all_four_banks_match_every_rdb_phase_tap_and_reserved_field(self):
        result = self.fir_mapping()
        banks = result["banks"]
        self.assertEqual(len(banks), 4)
        path = ROOT / "include/flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_scl_hd.h"
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),
                         "1ee3a4f791f11d50d26e5eaa000a5897cfc9d917bc21e85939629be1e55322e2")
        definitions = {name: int(value, 0) for name, value in re.findall(
            r"^#define\s+(BCHP_SCL_HD_\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\b", path.read_text(), re.M)}
        checked = 0
        for bank, (axis, stem, base, table_offset, count, loop), name in zip(
                banks, self.FIR_BANKS, ("vertical_luma", "vertical_chroma", "horizontal_luma", "horizontal_chroma")):
            with self.subTest(bank=stem):
                self.assertEqual((bank["axis"], bank["rdb_base_address"], bank["table_blob_file_offset"],
                                  bank["word_count"], bank["loop_entry_blob_file_offset"]),
                                 (axis, base, table_offset, count, loop))
                self.assertEqual(bank["name"], name)
                taps = count // 4
                for phase in range(8):
                    for tap in range(0, taps, 2):
                        name = f"BCHP_SCL_HD_{stem}_COEFF_PHASE{phase}_{tap:02d}_{tap + 1:02d}"
                        index = phase * (taps // 2) + tap // 2
                        self.assertEqual(definitions[name], base + index * 4)
                        word = struct.unpack_from("<I", self.payload, table_offset + index * 4)[0]
                        fields = ((f"COEFF_{tap}", 0x3ffc0000, 18),
                                  (f"COEFF_{tap + 1}", 0x00003ffc, 2),
                                  ("reserved0", 0xc0000000, 30),
                                  ("reserved1", 0x0003c000, 14), ("reserved2", 0x00000003, 0))
                        for field, mask, shift in fields:
                            self.assertEqual(definitions[f"{name}_{field}_MASK"], mask)
                            self.assertEqual(definitions[f"{name}_{field}_SHIFT"], shift)
                            if field.startswith("reserved"):
                                self.assertEqual(word & mask, 0)
                        table = next(t for t in result["tables"] if t["axis"] == axis)
                        self.assertEqual(table["phases"][phase]["unsigned12_taps"][tap:tap + 2],
                                         [(word & 0x3ffc0000) >> 18, (word & 0x00003ffc) >> 2])
                        checked += 1
        self.assertEqual(checked, 192)

    def test_scaler_fir_actual_loop_and_writer_oracle_has_exact_192_ordered_writes(self):
        writes, steps = self.execute_fir_loops()
        expected = [(0x90000000 + base + index * 4,
                     struct.unpack_from("<I", self.payload, table + index * 4)[0])
                    for _, _, base, table, count, _ in self.FIR_BANKS for index in range(count)]
        self.assertEqual(writes, expected)
        self.assertEqual((len(writes), steps), (192, 2122))
        self.assertEqual(len({address for address, _ in writes}), 192)
        self.assertEqual(writes[0], (0x90540900, 0))
        self.assertEqual(writes[-1], (0x90540bfc, 0x0008000c))
        # These mutations bypass mapper pins only in test-owned memory. The
        # oracle must independently detect loop-bound and address-selection bugs.
        for offset, replacement in ((0x2398, 0xe356001f), (0x23c0, 0xe354001f),
                                    (0x23ec, 0xe354003f), (0x2414, 0xe354003f)):
            data = bytearray(self.payload)
            struct.pack_into("<I", data, offset, replacement)
            with self.subTest(offset=offset):
                changed, _ = self.execute_fir_loops(data)
                self.assertNotEqual(changed, expected)
        data = bytearray(self.payload)
        struct.pack_into("<I", data, 0x1e8ec, 0xe7832101)  # incorrect register offset LSL #2
        with self.assertRaises(AssertionError):
            self.execute_fir_loops(data)

    def test_scaler_fir_enable_tail_and_exact_direct_caller_routes(self):
        result = self.fir_mapping()
        self.assertEqual((result["coefficient_write_count"], result["preceding_setup_write_count"]), (192, 20))
        expected_routing = {
            "entry_blob_file_offset": 0x1f8c, "channel_stride_bytes": 0x1cc,
            "cache_word_offset": 0x1c8, "active_byte_offset": 0x1cc, "picture_selector_byte_offset": 8,
            "target_width_fields": {"selector_equal_2": {"shift": 8, "bits": 12},
                                    "selector_other": {"shift": 20, "bits": 12}},
            "source_record_width_word_offset": 12, "reuse_record_flag_mask": 0x100,
            "picture_dispatch_call_offsets": [0x8518, 0x8634], "setup_call_offsets": [0x20fc, 0x215c]}
        self.assertEqual({key: result["routing"][key] for key in expected_routing}, expected_routing)
        self.assertIs(result["routing"]["complete_picture_caller_validated"], False)
        self.assertEqual(result["routing"]["setup_predicate"],
                         "Cached u32 != 0 and (source-record word+12 == 0 or unsigned word+12 > selected target width).")
        self.assertEqual(result["routing"]["reuse_predicate"],
                         "Source-record word+0 bit8 is set and channel active byte equals 1.")
        open_fields = result["open_fields"]
        expected_open = {"entry_blob_file_offset": 0x55d4, "request_word_offset": 0x20,
                         "request_enable_mask": 1, "field_input_range_inclusive": [128, 1919],
                         "odd_values_round_up": True,
                         "upper_field": {"shift": 20, "bits": 12, "fallback": 960},
                         "lower_field": {"shift": 8, "bits": 12, "fallback": 1280}}
        self.assertEqual({key: open_fields[key] for key in expected_open}, expected_open)
        self.assertEqual(open_fields["initial_cache_value"],
                         "Incoming r9; its initialization lies outside this selected region.")
        self.assertEqual(open_fields["cache_composition"],
                         "Enabled path ORs normalized fields and bit0 into initial r9; disabled path stores r9.")
        self.assertTrue(any("r9 must be zero" in text for text in result["conditions"]))
        expected_tail = {"branch_blob_file_offset": 0x2474, "target_blob_file_offset": 0x1e8e8,
                         "rdb_address": 0x540854, "value": 1}
        self.assertEqual({key: result["enable_tail"][key] for key in expected_tail}, expected_tail)
        setup_writer_calls = []
        for offset in range(0x21ac, 0x2378, 4):
            word = struct.unpack_from("<I", self.payload, offset)[0]
            if word >> 24 != 0xeb:  # AL BL, not opaque Thumb BLX.
                continue
            displacement = word & 0xffffff
            if displacement & 0x800000:
                displacement -= 1 << 24
            if offset + 8 + 4 * displacement == 0x1e8e8:
                setup_writer_calls.append(offset)
        self.assertEqual(setup_writer_calls,
                         [0x21f8, 0x2208, 0x2218, 0x2228, 0x2238, 0x2248, 0x2258, 0x2268,
                          0x2288, 0x229c, 0x22ac, 0x22bc, 0x22cc, 0x2300, 0x2320, 0x2330,
                          0x2340, 0x2354, 0x2364, 0x2374])
        self.assertEqual(len(setup_writer_calls), result["preceding_setup_write_count"])
        for offset, target in ((0x8518, 0x1f8c), (0x8634, 0x1f8c),
                               (0x20fc, 0x21ac), (0x215c, 0x21ac), (0x2474, 0x1e8e8)):
            word = struct.unpack_from("<I", self.payload, offset)[0]
            self.assertEqual(word & 0x0e000000, 0x0a000000)
            self.assertEqual(word >> 28, 14)
            self.assertEqual(bool(word & (1 << 24)), offset != 0x2474)
            displacement = word & 0xffffff
            if displacement & 0x800000:
                displacement -= 1 << 24
            self.assertEqual(offset + 8 + 4 * displacement, target)
        for offset, expected in ((0x55d4, 0xe5960020), (0x55d8, 0xe3100001),
                                 (0x562c, 0xe58121c8), (0x565c, 0xe58101c8),
                                 (0x1fbc, 0xe59701c8), (0x1fc4, 0xe1a01a20),
                                 (0x1fb4, 0xe3520002), (0x1fcc, 0xe7eb1450),
                                 (0x1fd4, 0xe3500000), (0x1fd8, 0x0a00000d),
                                 (0x1fe0, 0xe593000c), (0x1fe8, 0x8a000019),
                                 (0x1fec, 0xe3500000), (0x1ff0, 0x0a000017),
                                 (0x205c, 0xe3100c01), (0x2064, 0xe5d701cc),
                                 (0x2068, 0xe3500001), (0x206c, 0x0a00000e), (0x2470, 0xe3a02001)):
            self.assertEqual(struct.unpack_from("<I", self.payload, offset)[0], expected)
        self.assertEqual(struct.unpack_from("<I", self.payload, 0x1f88)[0], 0x540854)

    def test_scaler_fir_extension_is_pure_and_unrequested_reports_do_not_call_it(self):
        expected = self.fir_mapping()
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file/device open")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected file/device open")), \
                mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected external execution")):
            self.assertEqual(self.fir_mapping(), expected)
            self.assertEqual(MAP.analyze(self.data, scaler_fir=True)["scaler_fir"], expected)
        with mock.patch.object(MAP, "_scaler_fir_map", side_effect=AssertionError("unrequested FIR map")):
            for mask in range(16):
                MAP.analyze(self.data, references=bool(mask & 1), all_symbols=bool(mask & 2),
                            bootstrap=bool(mask & 4), picture_output=bool(mask & 8))

    def test_scaler_fir_public_pinned_admission_precedes_private_mapping(self):
        altered = bytearray(self.data)
        altered[0x2ccf0] ^= 1
        for data in (fixture(), altered, self.data[:-4], self.data + bytes(4)):
            with self.subTest(size=len(data)), \
                    mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("parse before identity")), \
                    mock.patch.object(MAP, "_scaler_fir_map", side_effect=AssertionError("map before identity")):
                with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                    MAP.analyze(data, expected_sha256=hashlib.sha256(data).hexdigest(), scaler_fir=True)
        for data in (self.data[:-1], self.data + b"\0"):
            with self.subTest(size=len(data)), \
                    mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("parse before size")), \
                    mock.patch.object(MAP, "_scaler_fir_map", side_effect=AssertionError("map before size")):
                with self.assertRaisesRegex(MAP.FormatError, "invalid BCM70015 firmware size"):
                    MAP.analyze(data, scaler_fir=True)
        with mock.patch.object(MAP.hashlib, "sha256") as digest, \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("parse before size")), \
                mock.patch.object(MAP, "_scaler_fir_map", side_effect=AssertionError("map before size")):
            digest.return_value.hexdigest.return_value = MAP.BUNDLED_SHA256
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(fixture(), scaler_fir=True)

    def test_scaler_fir_new_option_composes_without_changing_legacy_fields(self):
        expected = self.fir_mapping()
        for mask in range(16):
            options = {"references": bool(mask & 1), "all_symbols": bool(mask & 2),
                       "bootstrap": bool(mask & 4), "picture_output": bool(mask & 8)}
            with self.subTest(options=options):
                baseline = MAP.analyze(self.data, **options)
                enriched = MAP.analyze(self.data, scaler_fir=True, **options)
                self.assertNotIn("scaler_fir", baseline)
                self.assertEqual(enriched.pop("scaler_fir"), expected)
                self.assertEqual(enriched, baseline)

    def test_scaler_fir_cli_additive_reproducible_output_and_failure_admission(self):
        command = [sys.executable, "-B", str(TOOL), str(BLOB)]
        for flags in ([], ["--picture-output"], ["--references", "--bootstrap", "--all-symbols"]):
            baseline = subprocess.run(command + flags, capture_output=True, timeout=10)
            first = subprocess.run(command + flags + ["--scaler-fir"], capture_output=True, timeout=10)
            second = subprocess.run(command + flags + ["--scaler-fir"], capture_output=True, timeout=10)
            with self.subTest(flags=flags):
                for result in (baseline, first, second):
                    self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(first.stdout, second.stdout)
                enriched = json.loads(first.stdout)
                self.assertEqual(enriched.pop("scaler_fir"), self.fir_mapping())
                self.assertEqual(enriched, json.loads(baseline.stdout))
                self.assertNotIn(str(ROOT).encode(), first.stdout)
        data = fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data), \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(MAP, "_scaler_fir_map", side_effect=AssertionError("unexpected FIR map")), \
                mock.patch.object(sys, "stdout", new_callable=io.StringIO) as stdout, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO) as stderr:
            self.assertEqual(MAP.main(["fixture.bin", "--scaler-fir", "--expect-sha256",
                                       hashlib.sha256(data).hexdigest()]), 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("exact bundled", stderr.getvalue())
        real_open, calls = os.open, []
        def pin_only(path, flags):
            self.assertEqual(flags, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW)
            calls.append(path)
            self.assertEqual(len(calls), 1)
            return real_open(path, flags)
        metadata = mock.Mock(st_mode=0o20600, st_size=len(self.data))
        with mock.patch.object(MAP.os, "open", side_effect=pin_only), \
                mock.patch.object(MAP.os, "fstat", return_value=metadata), \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(MAP, "_scaler_fir_map", side_effect=AssertionError("unexpected FIR map")), \
                mock.patch.object(sys, "stdout", new_callable=io.StringIO) as stdout, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO) as stderr:
            self.assertEqual(MAP.main([str(BLOB), "--scaler-fir"]), 1)
        self.assertEqual(len(calls), 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("regular file", stderr.getvalue())

    def test_picture_output_schema_and_independent_instruction_decoding(self):
        result = MAP.analyze(self.data, picture_output=True)["picture_output"]
        self.assertEqual((result["schema_version"], result["isa"], result["endianness"]),
                         (1, "A32", "little"))
        self.assertFalse(result["device_observed"])
        anchors = result["instruction_anchors"]
        self.assertEqual(len(anchors), 187)
        self.assertEqual(len({a["blob_file_offset"] for a in anchors}), 187)
        self.assertEqual(MAP.MAX_PICTURE_OUTPUT_ANCHORS, 192)
        groups = {anchor["group"] for anchor in anchors}
        self.assertEqual({group: sum(a["group"] == group for a in anchors) for group in groups},
                         {"delivery": 27, "pending_main": 28, "picture_handler": 18,
                          "bop": 16, "dnr": 15, "mfd": 18, "scl": 27,
                          "key_stubs": 36, "key_callers": 2})
        for anchor in anchors:
            offset = anchor["blob_file_offset"]
            with self.subTest(offset=offset):
                self.assertEqual(offset % 4, 0)
                self.assertLess(offset, 0x2ea60)
                word, = struct.unpack_from("<I", self.payload, offset)
                self.assertEqual(anchor["word"], word)
                if anchor["operation"] in ("B", "BL"):
                    displacement = word & 0xffffff
                    if displacement & 0x800000:
                        displacement -= 0x1000000
                    self.assertEqual(anchor["target_blob_file_offset"], offset + 8 + displacement * 4)
                    self.assertEqual(anchor["condition"], word >> 28)
                    self.assertEqual(anchor["operation"], "BL" if word & 0x1000000 else "B")
                    self.assertLess(anchor["target_blob_file_offset"], len(self.payload) - 3)
                elif anchor["operation"] == "LDR literal":
                    displacement = word & 0xfff
                    literal = offset + 8 + (displacement if word & 0x800000 else -displacement)
                    self.assertEqual(anchor["literal_blob_file_offset"], literal)
                    self.assertEqual(anchor["literal_value"], struct.unpack_from("<I", self.payload, literal)[0])
                    self.assertEqual(anchor["destination_register"], (word >> 12) & 15)
                else:
                    self.assertEqual(anchor["operation"], "validated word")

    def test_picture_output_public_pin_precedes_all_parsing(self):
        altered = bytearray(self.data)
        altered[400] ^= 1
        for data in (fixture(), altered, self.data[:-4], self.data + bytes(4)):
            with self.subTest(size=len(data)), \
                    mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                    mock.patch.object(MAP, "_picture_output_map", side_effect=AssertionError("unexpected mapper")):
                with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                    MAP.analyze(data, expected_sha256=hashlib.sha256(data).hexdigest(), picture_output=True)
        with mock.patch.object(MAP.hashlib, "sha256") as digest, \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(MAP, "_picture_output_map", side_effect=AssertionError("unexpected mapper")):
            digest.return_value.hexdigest.return_value = MAP.BUNDLED_SHA256
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(fixture(), picture_output=True)

    def test_picture_output_private_bounds_and_exact_elf_identities(self):
        for payload in (self.payload[:-4], self.payload + bytes(4), b""):
            with self.subTest(size=len(payload)), self.assertRaisesRegex(MAP.FormatError, "payload size"):
                self.mapping(payload=payload)
        cases = [[], self.images[:1], self.images[::-1], self.images * 2]
        for index, field in ((0, "blob_file_offset"), (0, "blob_file_end"),
                             (1, "blob_file_offset"), (1, "blob_file_end")):
            for adjustment in (-4, 4):
                images = [dict(image) for image in self.images]
                images[index][field] += adjustment
                cases.append(images)
        for index in range(2):
            for field, value in (("class", 64), ("endianness", "big"),
                                 ("machine", 40), ("elf_type", 1),
                                 ("entry_virtual_address", self.images[index]["entry_virtual_address"] + 4)):
                images = [dict(image) for image in self.images]
                images[index][field] = value
                cases.append(images)
        for images in cases:
            with self.subTest(images=len(images)), self.assertRaisesRegex(MAP.FormatError, "ELF identities"):
                self.mapping(images=images)

    def test_picture_output_exact_anchor_budget(self):
        with mock.patch.object(MAP, "MAX_PICTURE_OUTPUT_ANCHORS", 187):
            self.assertEqual(len(self.mapping()["instruction_anchors"]), 187)
        with mock.patch.object(MAP, "MAX_PICTURE_OUTPUT_ANCHORS", 186):
            with self.assertRaisesRegex(MAP.FormatError, "anchor budget"):
                self.mapping()

    def test_key_stub_groups_cover_both_complete_eighteen_word_bodies(self):
        anchors = {a["blob_file_offset"]: a for a in self.mapping()["instruction_anchors"]
                   if a["group"] == "key_stubs"}
        self.assertEqual(set(anchors), set(range(0x3ed8, 0x3f68, 4)))
        # Independently inspected complete handler bodies: nonnull input logs,
        # clears ACK status, copies sequence, and returns; no provisioning work.
        expected = (
            (0x3ed8, (0xe92d4070, 0xe3500000, 0x0a000009, 0xe2805014,
                      0xe2804f45, 0xe28f0f7b, 0xeb007133, 0xe3a00000,
                      0xe5840008, 0xe5950004, 0xe5840004, 0xe3a00000,
                      0xe8bd8070, 0xe1a01000, 0xe59f01fc, 0xeb00712a,
                      0xe3a00002, 0xeafffff9)),
            (0x3f20, (0xe92d4070, 0xe3500000, 0x0a000009, 0xe2805014,
                      0xe2804f45, 0xe28f0f77, 0xeb007121, 0xe3a00000,
                      0xe5840008, 0xe5950004, 0xe5840004, 0xe3a00000,
                      0xe8bd8070, 0xe1a01000, 0xe59f01ec, 0xeb007118,
                      0xe3a00002, 0xeafffff9)))
        for base, words in expected:
            self.assertEqual([anchors[base + 4 * index]["word"] for index in range(18)], list(words))
            self.assertEqual(anchors[base + 8]["target_blob_file_offset"], base + 52)
            self.assertEqual(anchors[base + 68]["target_blob_file_offset"], base + 48)
            for relative in (24, 60):
                self.assertEqual(anchors[base + relative]["target_blob_file_offset"], 0x203c4)

    def test_descriptor_delivery_pending_slots_and_copy_argument_limits(self):
        result = self.mapping()
        delivery = result["descriptor_delivery"]
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        expected = {"reader_entry_blob_file_offset": 0x7708, "arm2_callback_call_blob_file_offset": 0x701c,
                    "arm_mailbox_physical_address": 0x100e0024, "arm_mailbox_rdb_address": 0xe0024,
                    "source_expression": "BORCH_END + 0x401", "slot_stride_bytes": 0x1cc,
                    "destination_slot_offset": 0x188, "copy_argument_bytes": 32,
                    "copy_helper_entry_blob_file_offset": 0x2c59c, "copy_helper_body_validated": False,
                    "pending_slot_offset": 0x1a8, "active_slot_offset": 0xc4,
                    "main_slot_range": [0, 3], "main_call_blob_file_offset": 0x8e90,
                    "picture_handler_entry_blob_file_offset": 0x834c,
                    "started_slot_offset": 0xd2, "pending_clear_blob_file_offset": 0x8820,
                    "complete_dma_ownership_verified": False}
        self.assertEqual({key: delivery[key] for key in expected}, expected)
        for source, target in ((0x701c, 0x7708), (0x7714, 0x898),
                               (0x777c, 0x2c59c), (0x8e80, 0x778c), (0x8e90, 0x834c)):
            self.assertEqual(anchors[source]["target_blob_file_offset"], target)
        self.assertEqual(anchors[0x7720]["literal_value"] + (anchors[0x7724]["word"] & 0xfff), 0x100f6004)
        self.assertEqual(anchors[0x7728]["word"], 0xe3002401)  # fixed MOVW +0x401
        self.assertEqual(anchors[0x7730]["literal_value"] + (anchors[0x7734]["word"] & 0xfff),
                         delivery["arm_mailbox_physical_address"])
        self.assertEqual(anchors[0x7768]["word"] & 255, delivery["slot_stride_bytes"] // 4)
        self.assertEqual(anchors[0x7774]["word"] & 255, delivery["copy_argument_bytes"])
        self.assertEqual(delivery["destination_slot_offset"] + delivery["copy_argument_bytes"], delivery["pending_slot_offset"])
        for offset, word in ((0x7784, 0xe5c401a8), (0x77ac, 0xe5d011a8),
                             (0x77b8, 0xe5d000c4), (0x8394, 0xe5d400d2),
                             (0x8354, 0xe1a09000), (0x8358, 0xe3a07000), (0x8820, 0xe5c471a8)):
            self.assertEqual(anchors[offset]["word"], word)
        self.assertEqual(anchors[0x8e9c]["condition"], 11)

    def test_descriptor_host_abi_and_mailbox_provenance_fuses(self):
        delivery = self.mapping()["descriptor_delivery"]
        shared = (ROOT / "include/flea/DriverFwShare.h").read_text()
        record = shared.split("_PIC_DELIVERY_HOST_INFO_", 1)[1].split("}PIC_DELIVERY_HOST_INFO", 1)[0]
        fields = re.findall(r"unsigned int\s+(\w+)(?:\[(\d+)\])?\s*;", record)
        self.assertEqual([name for name, _ in fields],
                         ["ListIndex", "HostDescMemLowAddr_Y", "HostDescMemHighAddr_Y",
                          "HostDescMemLowAddr_UV", "HostDescMemHighAddr_UV", "RxSeqNumber", "ChannelID", "Reserved"])
        self.assertEqual(sum(int(count or 1) for _, count in fields) * 4, delivery["copy_argument_bytes"])
        self.assertRegex(shared, r"#define\s+HOST_TO_FW_PIC_DEL_INFO_ADDR\s+0x400\b")
        native = (ROOT / "driver/linux/crystalhd_fleafuncs.c").read_text()
        self.assertIn("sizeof (PicDeliInfo) - sizeof(PicDeliInfo.Reserved)", native)
        self.assertRegex(native, r"FleaRxPicDelAddr\s*=\s*borchStachAddr\s*\+\s*1\s*\+\s*HOST_TO_FW_PIC_DEL_INFO_ADDR")
        self.assertRegex(native, r"pfnWriteDevRegister\(hw->adp, RX_POST_MAILBOX, hw->channelNum\)")
        for key, text in (("host_record_source", "_PIC_DELIVERY_HOST_INFO_"),
                          ("host_submit_source", "pfnDevDRAMWrite")):
            path, line = delivery[key].rsplit(":", 1)
            self.assertFalse(Path(path).is_absolute())
            self.assertIn(text, (ROOT / path).read_text().splitlines()[int(line) - 1])
        path, line = delivery["host_submit_source"].rsplit(":", 1)
        self.assertEqual(path, "driver/linux/crystalhd_fleafuncs.c")
        self.assertEqual((ROOT / path).read_text().splitlines()[int(line) - 1].strip(),
                         "sts = hw->pfnDevDRAMWrite(hw, hw->FleaRxPicDelAddr, BuffSzInDwords,")
        definitions = (ROOT / "driver/linux/FleaDefs.h").read_text()
        self.assertRegex(definitions, r"#define\s+RX_POST_MAILBOX\s+BCHP_ARMCR4_BRIDGE_REG_MBOX_ARM2\b")
        bridge = (ROOT / "include/flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_armcr4_bridge.h").read_text()
        self.assertRegex(bridge, rf"#define\s+BCHP_ARMCR4_BRIDGE_REG_MBOX_ARM2\s+0x{delivery['arm_mailbox_rdb_address']:08x}\b")

    def test_picture_feed_trigger_callers_and_bop_not_input_decryption(self):
        result = self.mapping()
        feed = result["picture_feed"]
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        self.assertEqual(feed["entry_blob_file_offset"], 0x7898)
        self.assertEqual(feed["callers"], [{"blob_file_offset": offset, "r0": value, "r1": "slot"}
                                          for offset, value in ((0x86a0, 1), (0x871c, 1), (0x87b0, 0))])
        for caller in feed["callers"]:
            offset = caller["blob_file_offset"]
            self.assertEqual(anchors[offset]["target_blob_file_offset"], 0x7898)
            self.assertEqual(anchors[offset - 4]["word"], 0xe3a00000 | caller["r0"])
            self.assertEqual(anchors[offset - 8]["word"], 0xe1a01009)
        self.assertIn("not generic AES or input decryption", feed["scope"])
        self.assertEqual(anchors[0x78a8]["word"], 0xe3820001)  # OR bit zero
        self.assertEqual(anchors[0x78bc]["word"], 0xe3c20001)  # clear bit zero
        self.assertEqual(anchors[0x78cc]["word"], 0xe5810030)

    def test_register_operations_match_rdb_fields_without_inferred_physical_base(self):
        operations = self.mapping()["register_operations"]
        expected = (
            ("BOP_AES_CTRL", 0x10510000, 0x510000, [0x78ac, 0x78c0], ["START_ENCRYPTION_SCRAMBLE"]),
            ("MFD_PIC_FEED_CMD", 0x10540030, 0x540030, [0x78cc], ["START_FEED"]),
            ("MFD_DISP_HSIZE", None, 0x540014, [0x1ca8], ["VALUE"]),
            ("DNR_DNR_TOP_CTRL", 0x10540404, 0x540404, [0x82e0], ["DNR_ENABLE"]),
            ("DNR_LINE_STORE_CONFIG", 0x10540408, 0x540408, [0x82f0], ["LS_MODE"]),
            ("DNR_SRC_PIC_SIZE", 0x1054040c, 0x54040c, [0x82fc], ["HSIZE", "VSIZE"]),
            ("SCL_HD_TOP_CONTROL", None, 0x540804, [0x1d14], ["ENABLE_CTRL", "UPDATE_SEL"]),
            ("SCL_HD_BVB_IN_SIZE", None, 0x540810, [0x1d48], ["HSIZE", "VSIZE"]),
            ("SCL_HD_DEST_PIC_SIZE", None, 0x54081c, [0x1d78], ["HSIZE", "VSIZE"]),
            ("SCL_HD_ENABLE", None, 0x540854, [0x1e5c], ["SCALER_ENABLE"]))
        self.assertEqual(len(operations), len(expected))
        for operation, (name, physical, address, sites, fields) in zip(operations, expected):
            with self.subTest(register=name):
                self.assertEqual((operation["name"], operation["arm_physical_address"], operation["rdb_address"],
                                  operation["instruction_blob_file_offsets"]), (name, physical, address, sites))
                path, line = operation["source"].rsplit(":", 1)
                self.assertFalse(Path(path).is_absolute())
                source = (ROOT / path).read_text()
                register = re.search(rf"#define\s+BCHP_{name}\s+(0x[0-9a-fA-F]+)\b", source)
                self.assertEqual(int(register.group(1), 16), address)
                mask = 0
                for field in fields:
                    definition = re.search(rf"#define\s+BCHP_{name}_{field}_MASK\s+(0x[0-9a-fA-F]+)\b", source)
                    mask |= int(definition.group(1), 16)
                self.assertEqual(mask, operation["field_mask"])
                self.assertIn(fields[0], "\n".join(source.splitlines()[int(line) - 1:int(line) + 1]))
                self.assertEqual(operation["register_write_helper_entry_blob_file_offset"],
                                 0x1e8e8 if physical is None else None)
                if physical is not None:
                    self.assertEqual(physical - address, 0x10000000)
        dnr = next(o for o in operations if o["name"] == "DNR_LINE_STORE_CONFIG")
        self.assertIn("width > 720", dnr["selected_operation"])
        size = next(o for o in operations if o["name"] == "DNR_SRC_PIC_SIZE")
        self.assertIn("no width masking", size["selected_operation"])
        self.assertTrue(all("CSC" not in operation["name"] for operation in operations))

    def test_key_ack_metadata_and_common_response_abi_are_not_provisioning(self):
        result = self.mapping()
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        stubs = result["key_handler_stubs"]
        self.assertEqual([(s["name"], s["entry_blob_file_offset"], s["last_instruction_blob_file_offset"],
                           s["caller_blob_file_offset"], s["diagnostic_blob_file_offset"]) for s in stubs],
                         [("SetSessionKey", 0x3ed8, 0x3f1c, 0x6928, 0x40e0),
                          ("SetContentKey", 0x3f20, 0x3f64, 0x6908, 0x4118)])
        header = (ROOT / "include/7411d.h").read_text()
        reply = header.split("/* common response structure */", 1)[1].split("}", 1)[0]
        fields = re.findall(r"uint32_t\s+(\w+)\s*;", reply)
        self.assertEqual(fields, ["command", "sequence", "status"])
        self.assertEqual({a["blob_file_offset"] for a in anchors.values() if a["group"] == "key_callers"}, {0x6908, 0x6928})
        for stub in stubs:
            self.assertEqual(anchors[stub["caller_blob_file_offset"]]["target_blob_file_offset"], stub["entry_blob_file_offset"])
            self.assertEqual((stub["request_record_offset"], stub["reply_record_offset"]), (0x14, 0x114))
            self.assertEqual(fields[stub["sequence_word_index"]], "sequence")
            self.assertEqual(fields[stub["status_word_index"]], "status")
            self.assertEqual(stub["nonnull_reply_status"], 0)
            self.assertFalse(stub["key_payload_reads_in_bounded_body"])
            self.assertFalse(stub["key_provisioning_verified"])
            self.assertIn("status zero is not key provisioning", stub["scope"])
        self.assertTrue(any("CSC dispatcher routing" in text for text in result["deferred"]))
        for text in ("not a complete call graph", "not complete DMA ownership", "Context-relative register writes",
                     "copy-helper implementations", "ARC paths and Thumb helpers", "No hardware execution"):
            self.assertTrue(any(text in limitation for limitation in result["limitations"]), text)

    def test_every_picture_output_instruction_anchor_rejects_mutation(self):
        for anchor in self.mapping()["instruction_anchors"]:
            data = bytearray(self.payload)
            offset = anchor["blob_file_offset"]
            struct.pack_into("<I", data, offset, anchor["word"] ^ 1)
            with self.subTest(offset=offset), self.assertRaises(MAP.FormatError):
                self.mapping(payload=data)

    def test_every_picture_output_literal_rejects_mutation(self):
        offsets = {}
        for anchor in self.mapping()["instruction_anchors"]:
            if anchor["operation"] == "LDR literal":
                offsets[anchor["literal_blob_file_offset"]] = anchor["literal_value"]
        self.assertTrue(offsets)
        for offset, value in offsets.items():
            data = bytearray(self.payload)
            struct.pack_into("<I", data, offset, value ^ 4)
            with self.subTest(offset=offset), self.assertRaises(MAP.FormatError):
                self.mapping(payload=data)

    def test_picture_output_key_log_strings_reject_every_byte_mutation(self):
        strings = ((0x40e0, b"[fw] SMP_CmdIf_SetSessionKey(): NOT Implemented\n\0"),
                   (0x4118, b"[fw] SMP_CmdIf_SetContentKey(): NOT Implemented\n\0"),
                   (0x2cf84, b"[fw] SMP_CmdIf_SetSessionKey(): Invalid Parameter with Command Header Address = 0x%x\n\0"),
                   (0x2cfdc, b"[fw] SMP_CmdIf_SetContentKey(): Invalid Parameter with Command Header Address = 0x%x\n\0"))
        for offset, text in strings:
            self.assertEqual(self.payload[offset:offset + len(text)], text)
            for index in range(len(text)):
                data = bytearray(self.payload)
                data[offset + index] ^= 1
                with self.subTest(offset=offset + index), self.assertRaises(MAP.FormatError):
                    self.mapping(payload=data)

    def test_picture_output_pure_map_and_all_existing_option_combinations(self):
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file read")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected device/file open")):
            self.mapping()
            MAP.analyze(self.data, picture_output=True)
        for mask in range(8):
            options = {"references": bool(mask & 1), "all_symbols": bool(mask & 2), "bootstrap": bool(mask & 4)}
            with self.subTest(options=options):
                plain = MAP.analyze(self.data, **options)
                enriched = MAP.analyze(self.data, picture_output=True, **options)
                self.assertNotIn("picture_output", plain)
                self.assertEqual(enriched.pop("picture_output"), self.mapping())
                self.assertEqual(enriched, plain)

    def test_picture_output_cli_pinned_failure_without_json_or_mapper(self):
        data = fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data), \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(MAP, "_picture_output_map", side_effect=AssertionError("unexpected mapper")), \
                mock.patch.object(sys, "stdout", new_callable=io.StringIO) as stdout, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO) as stderr:
            self.assertEqual(MAP.main(["fixture.bin", "--picture-output", "--expect-sha256",
                                       hashlib.sha256(data).hexdigest()]), 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("exact bundled", stderr.getvalue())

    def test_picture_output_cli_keeps_regular_file_admission_before_read(self):
        # Pin an ordinary inode, then inject nonregular metadata. The second
        # read-open and both parsers must remain unreachable with this option.
        real_open = os.open
        calls = []
        def pin_only(path, flags):
            self.assertEqual(flags, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW)
            calls.append(path)
            self.assertEqual(len(calls), 1)
            return real_open(path, flags)
        metadata = mock.Mock(st_mode=0o20600, st_size=len(self.data))
        with mock.patch.object(MAP.os, "open", side_effect=pin_only), \
                mock.patch.object(MAP.os, "fstat", return_value=metadata), \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(MAP, "_picture_output_map", side_effect=AssertionError("unexpected mapper")), \
                mock.patch.object(sys, "stdout", new_callable=io.StringIO) as stdout, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO) as stderr:
            self.assertEqual(MAP.main([str(BLOB), "--picture-output"]), 1)
        self.assertEqual(len(calls), 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("regular file", stderr.getvalue())

    def test_picture_output_cli_all_combinations_and_reproducible_json(self):
        for mask in range(8):
            flags = []
            if mask & 1:
                flags += ["--references", "--symbol", "ReadLine"]
            if mask & 2:
                flags += ["--all-symbols"]
            if mask & 4:
                flags += ["--bootstrap"]
            command = [sys.executable, "-B", str(TOOL), str(BLOB)] + flags
            plain = subprocess.run(command, capture_output=True, timeout=10)
            first = subprocess.run(command + ["--picture-output"], capture_output=True, timeout=10)
            second = subprocess.run(command + ["--picture-output"], capture_output=True, timeout=10)
            with self.subTest(flags=flags):
                self.assertEqual(plain.returncode, 0, plain.stderr)
                self.assertEqual(first.returncode, 0, first.stderr)
                self.assertEqual(second.returncode, 0, second.stderr)
                self.assertEqual(first.stdout, second.stdout)
                enriched = json.loads(first.stdout)
                self.assertEqual(enriched.pop("picture_output"), self.mapping())
                self.assertEqual(enriched, json.loads(plain.stdout))
                self.assertNotIn(str(ROOT).encode(), first.stdout)
        self.assertEqual(hashlib.sha256(BLOB.read_bytes()).hexdigest(), MAP.BUNDLED_SHA256)


class FirmwareArcMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data, all_symbols=True)["images"]
        cls.sections = [[s for s in image["sections"] if s["name"] in
                         (".comment", ".arcextmap", ".ARC.attributes")] for image in cls.images]

    def mapping(self, payload=None, images=None, sections=None):
        return MAP._arc_metadata_map(self.payload if payload is None else payload,
                                    self.images if images is None else images,
                                    self.sections if sections is None else sections)

    def test_arc_metadata_exact_offsets_counts_and_namespace_limits(self):
        report = MAP.analyze(self.data, arc_metadata=True)
        result = report["arc_metadata"]
        self.assertEqual(result, self.mapping())
        self.assertEqual(result["gnu_binutils_version"], "2.23.2")
        self.assertEqual(result["architecture_selection"], "unresolved")
        self.assertEqual(result["auxiliary_address_byte_order"], "big")
        self.assertTrue(result["extension_maps_identical"])
        expected = ((0x2ea60, 0x6711c, 2313, 0x79a40, 0x67a25, 0x79a68, 43),
                    (0x79dd8, 0xc1ced, 1289, 0xcf408, 0xc21f6, 0xcf430, 24))
        for image, (base, comment, size, header, ext, ext_header, count) in zip(result["images"], expected):
            self.assertEqual(image["image_blob_file_offset"], base)
            self.assertEqual(image["elf_flags"], 0)
            self.assertEqual(image["gnu_2_23_2_flag_machine"], "ARC5")
            self.assertFalse(image["arc_attributes_present"])
            self.assertEqual(image["sections"], {
                ".comment": {"blob_file_offset": comment, "size": size,
                             "section_header_blob_file_offset": header},
                ".arcextmap": {"blob_file_offset": ext, "size": 112,
                               "section_header_blob_file_offset": ext_header}})
            self.assertEqual(image["comment"], {
                "linker_hint": "MetaWare Linker v8.4.6", "compiler_hint": "hc8.4.18 -a4 -core8 -O -Xbs",
                "compiler_record_count": count, "nul_record_count": count * 2,
                "first_compiler_record_blob_file_offset": comment + 23})
            self.assertEqual(self.payload[comment:comment + 23], b"MetaWare Linker v8.4.6\n")
            self.assertEqual(len(image["extension_declarations"]), 10)
        encoded = json.dumps(result)
        self.assertNotIn("main.c", encoded)
        self.assertNotIn("11202009", encoded)
        self.assertNotIn("/tmp/", encoded)
        self.assertIn("not instruction semantics", encoded)
        self.assertTrue(all("sections" not in i for i in report["images"]))
        self.assertTrue(all(not any(k.startswith("_") for k in i) for i in report["images"]))

    def test_arc_extension_declarations_exact_record_offsets_and_big_endian_aux(self):
        for image, base in zip(self.mapping()["images"], (0x67a25, 0xc21f6)):
            records = image["extension_declarations"]
            self.assertEqual([(r["record_blob_file_offset"] - base, r["length"], r["type"], r["name"])
                              for r in records], [(0, 15, 2, "t0_count"), (15, 17, 2, "t0_control"),
                              (32, 15, 2, "t0_limit"), (47, 9, 0, "asl"), (56, 9, 0, "lsr"),
                              (65, 9, 0, "asr"), (74, 9, 0, "ror"), (83, 11, 0, "mul16"),
                              (94, 9, 0, "max"), (103, 9, 0, "min")])
            self.assertEqual([r["auxiliary_address"] for r in records[:3]], [0x21, 0x22, 0x23])
            self.assertEqual([r["opcode"] for r in records[3:]], [0x10, 0x11, 0x12, 0x13, 0x16, 0x1e, 0x1f])
            self.assertTrue(all(r["minor_opcode"] == r["flags"] == 0 for r in records[3:]))
            for r in records[:3]:
                pos = r["record_blob_file_offset"] + 2
                self.assertEqual(int.from_bytes(self.payload[pos:pos + 4], "big"), r["auxiliary_address"])
                self.assertNotEqual(int.from_bytes(self.payload[pos:pos + 4], "little"), r["auxiliary_address"])

    def test_arc_public_pin_precedes_elf_and_metadata_parsing(self):
        changed = bytearray(self.data)
        changed[0x6711c + 51] ^= 1
        for data in (fixture(), self.data[:-4], bytes(changed)):
            for mask in range(16):
                with self.subTest(size=len(data), options=mask), \
                        mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                        mock.patch.object(MAP, "_arc_metadata_map", side_effect=AssertionError("unexpected metadata parse")):
                    with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                        MAP.analyze(data, expected_sha256=hashlib.sha256(data).hexdigest(), arc_metadata=True,
                                    references=bool(mask & 1), all_symbols=bool(mask & 2),
                                    bootstrap=bool(mask & 4), picture_output=bool(mask & 8))
        with mock.patch.object(MAP.hashlib, "sha256") as digest, \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")):
            digest.return_value.hexdigest.return_value = MAP.BUNDLED_SHA256
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(fixture(), arc_metadata=True)

    def test_arc_private_image_header_and_bounds_mutations(self):
        for payload, images, sections in ((self.payload[:-4], self.images, self.sections),
                                         (self.payload, self.images[::-1], self.sections),
                                         (self.payload, self.images[:1], self.sections),
                                         (self.payload, self.images, self.sections[:1])):
            with self.assertRaises(MAP.FormatError):
                self.mapping(payload, images, sections)
        for image_index in range(2):
            for field, value in (("blob_file_offset", 0), ("blob_file_end", len(self.payload)),
                                 ("class", 64), ("endianness", "big"), ("machine", 195),
                                 ("elf_type", 1), ("flags", 3)):
                images = [dict(i) for i in self.images]
                images[image_index][field] = value
                with self.subTest(image=image_index, field=field), self.assertRaises(MAP.FormatError):
                    self.mapping(images=images)

    def test_arc_metadata_section_ambiguity_and_each_field(self):
        for image_index in range(2):
            for operation in ("missing", "duplicate", "attributes", "rename"):
                sections = [[dict(s) for s in group] for group in self.sections]
                if operation == "missing":
                    sections[image_index].pop()
                elif operation in ("duplicate", "attributes"):
                    extra = dict(sections[image_index][0])
                    if operation == "attributes":
                        extra["name"] = ".ARC.attributes"
                    sections[image_index].append(extra)
                else:
                    sections[image_index][0]["name"] = ".ARC.attributes"
                with self.subTest(image=image_index, operation=operation), self.assertRaises(MAP.FormatError):
                    self.mapping(sections=sections)
            for section_index in range(2):
                for field in ("section_index", "type", "flags", "elf_virtual_address", "blob_file_offset",
                              "size", "link", "info", "align", "entry_size", "section_header_blob_file_offset"):
                    sections = [[dict(s) for s in group] for group in self.sections]
                    sections[image_index][section_index][field] += 1
                    with self.subTest(image=image_index, section=section_index, field=field), \
                            self.assertRaises(MAP.FormatError):
                        self.mapping(sections=sections)
                section = self.sections[image_index][section_index]
                for position in (None, -1, self.images[image_index]["blob_file_offset"] - 1,
                                 self.images[image_index]["blob_file_end"] - section["size"] + 1):
                    sections = [[dict(s) for s in group] for group in self.sections]
                    sections[image_index][section_index]["blob_file_offset"] = position
                    with self.subTest(image=image_index, position=position), \
                            self.assertRaisesRegex(MAP.FormatError, "bounds"):
                        self.mapping(sections=sections)

    def test_arc_raw_section_header_mutations_reach_metadata_validator(self):
        for image_index, image in enumerate(self.images):
            for target in ("duplicate", "cross-image", "attributes", "flags"):
                data = bytearray(self.data)
                metadata = self.sections[image_index][0]
                header = metadata["section_header_blob_file_offset"]
                if target == "duplicate":
                    name_index = struct.unpack_from("<I", data, header)[0]
                    other = image["sections"][1]["section_header_blob_file_offset"]
                    struct.pack_into("<I", data, other, name_index)
                elif target == "cross-image":
                    struct.pack_into("<I", data, header + 16, image["blob_file_end"] - image["blob_file_offset"])
                elif target == "attributes":
                    # Replace an existing sufficiently long section name in place.
                    other = next(s for s in image["sections"] if len(s["name"]) >= len(".ARC.attributes"))
                    name_index = struct.unpack_from("<I", data, other["section_header_blob_file_offset"])[0]
                    names_index = struct.unpack_from("<H", data, image["blob_file_offset"] + 50)[0]
                    name_table = image["sections"][names_index]["blob_file_offset"]
                    position = name_table + name_index
                    data[position:position + 16] = b".ARC.attributes\0"
                else:
                    struct.pack_into("<I", data, image["blob_file_offset"] + 36, 3)
                with self.subTest(image=image_index, target=target), self.assertRaises(MAP.FormatError):
                    parsed = MAP.analyze(data, expected_sha256=hashlib.sha256(data).hexdigest(), all_symbols=True)["images"]
                    sections = [[s for s in i["sections"] if s["name"] in
                                 (".comment", ".arcextmap", ".ARC.attributes")] for i in parsed]
                    self.mapping(payload=data[:-20], images=parsed, sections=sections)

    def test_arc_every_extension_byte_and_truncation_rejected(self):
        for base in (0x67a25, 0xc21f6):
            for index in range(112):
                payload = bytearray(self.payload)
                payload[base + index] ^= 1
                with self.subTest(offset=base + index), self.assertRaises(MAP.FormatError):
                    self.mapping(payload=payload)
        data = self.payload[0x67a25:0x67a95]
        for size in range(len(data)):
            with self.subTest(size=size), self.assertRaises(MAP.FormatError):
                MAP._arc_extensions(data[:size], 0)

    def test_arc_extension_types_lengths_duplicates_and_fields(self):
        original = self.payload[0x67a25:0x67a95]
        changes = ((0, 0), (0, 1), (0, 7), (0, 113), (1, 1), (1, 3), (1, 255),
                   (0x2f, 6), (0x31, 3), (0x31, 0x20), (0x32, 1), (0x33, 1),
                   (0x14, 0x21), (0x3a, 0x10), (0xe, ord("x")), (6, 0), (6, 255))
        for offset, value in changes:
            data = bytearray(original)
            data[offset] = value
            with self.subTest(offset=offset, value=value), self.assertRaises(MAP.FormatError):
                MAP._arc_extensions(data, 0)
        for data in (original[:2] + original[2:6][::-1] + original[6:],
                     original[:0x26] + b"t0_count" + original[0x2e:], original + b"\0"):
            with self.assertRaises(MAP.FormatError):
                MAP._arc_extensions(data, 0)
        with mock.patch.object(MAP, "MAX_ARC_EXTENSION_BYTES", 128):
            with self.assertRaisesRegex(MAP.FormatError, "record budget"):
                MAP._arc_extensions(original + original[-9:], 0)

    def test_arc_comment_hint_grammar_counts_and_termination(self):
        for base, size, count in ((0x6711c, 2313, 43), (0xc1ced, 1289, 24)):
            original = self.payload[base:base + size]
            offsets = list(range(23))
            position = 0
            for index, record in enumerate(original[:-1].split(b"\0")):
                start = position + (23 if index == 0 else 0)
                offsets += range(start, start + (28 if index % 2 == 0 else len(record)))
                offsets.append(position + len(record))
                position += len(record) + 1
            for offset in offsets:
                data = bytearray(original)
                data[offset] ^= 1
                with self.subTest(offset=base + offset), self.assertRaises(MAP.FormatError):
                    MAP._arc_comment(data, base, count)
            for data in (b"", original[:-1], original + b"\0", b"\0" + original,
                         original.replace(b"main.c", b"ma\xffn.c", 1),
                         original.replace(b"main.c", b"ma\nn.c", 1),
                         original.replace(b"main.c", b"main.x", 1), bytes(4097)):
                with self.assertRaises(MAP.FormatError):
                    MAP._arc_comment(data, base, count)
            with self.assertRaisesRegex(MAP.FormatError, "count"):
                MAP._arc_comment(original, base, count - 1)

    def test_arc_independent_byte_record_and_string_budgets(self):
        data = self.payload[0x67a25:0x67a95]
        comment = self.payload[0x6711c:0x67a25]
        for constant, limit, call in (
                ("MAX_ARC_METADATA_BYTES", sum(s["size"] for group in self.sections for s in group) - 1,
                 self.mapping),
                ("MAX_ARC_COMMENT_BYTES", len(comment) - 1, self.mapping),
                ("MAX_ARC_EXTENSION_BYTES", 111, self.mapping),
                ("MAX_ARC_COMMENT_RECORDS", 85, lambda: MAP._arc_comment(comment, 0, 43)),
                ("MAX_ARC_EXTENSION_RECORDS", 9, lambda: MAP._arc_extensions(data, 0)),
                ("MAX_ARC_METADATA_STRING_BYTES", 56, lambda: MAP._arc_comment(comment, 0, 43)),
                ("MAX_ARC_METADATA_STRING_BYTES", 9, lambda: MAP._arc_extensions(data, 0))):
            with self.subTest(constant=constant), mock.patch.object(MAP, constant, limit), \
                    self.assertRaises(MAP.FormatError):
                call()
        with mock.patch.object(MAP, "MAX_ARC_METADATA_BYTES", len(comment) - 1), \
                mock.patch.object(MAP, "bounded", side_effect=AssertionError("unexpected metadata copy")), \
                self.assertRaisesRegex(MAP.FormatError, "budget"):
            self.mapping()

    def test_arc_metadata_pure_mapping_and_cli_pin_failure(self):
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file read")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected open")), \
                mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected execution")):
            self.mapping()
            MAP.analyze(self.data, arc_metadata=True)
        data = fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data), \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(MAP, "_arc_metadata_map", side_effect=AssertionError("unexpected metadata parse")), \
                mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO) as error:
            self.assertEqual(MAP.main(["fixture.bin", "--arc-metadata", "--expect-sha256",
                                       hashlib.sha256(data).hexdigest()]), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("exact bundled", error.getvalue())

    def test_arc_cli_nonregular_admission_does_not_read_or_parse(self):
        real_open = os.open
        for mode in (0o20600, 0o10600, 0o120000):
            descriptors = []
            def pin_only(path, flags):
                self.assertEqual(flags, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW)
                self.assertEqual(len(descriptors), 0)
                descriptor = real_open(path, flags)
                descriptors.append(descriptor)
                return descriptor
            metadata = mock.Mock(st_mode=mode, st_size=len(self.data))
            with self.subTest(mode=mode), mock.patch.object(MAP.os, "open", side_effect=pin_only), \
                    mock.patch.object(MAP.os, "fstat", return_value=metadata), \
                    mock.patch.object(MAP.os, "fdopen", side_effect=AssertionError("unexpected read-open")), \
                    mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                    mock.patch.object(MAP, "_arc_metadata_map", side_effect=AssertionError("unexpected metadata parse")), \
                    mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                    mock.patch.object(sys, "stderr", new_callable=io.StringIO) as error:
                self.assertEqual(MAP.main([str(BLOB), "--arc-metadata"]), 1)
            self.assertEqual(len(descriptors), 1)
            self.assertEqual(output.getvalue(), "")
            self.assertIn("regular file", error.getvalue())
            with self.assertRaises(OSError):
                os.fstat(descriptors[0])

    def test_arc_cli_all_options_preserve_baseline_bytes_and_reproduce_metadata(self):
        expected = (
            "15068c07a81a510e02435b6203b1cf5e424152e37ff456b1da9c12b77c15799e",
            "2a491f4b9033395cdbc688810319ea159973a296d7828ac65153bc93d45b588f",
            "15af1023987b5f2aee1e9a5560f749560bf33f729252ca3fcb9f41358f49e5aa",
            "85bec0f347f91ccfbb802d07b4c65fb07c8925625710dbe525fd7c3c1376487e",
            "ac366ee6fa1912d70dc816979b494817863da8fff6687e7aabc20f67633eea6e",
            "6ed302c739450fd7302bb65a388a74dc934e914bfbb3c67f26304647a2bca012",
            "3af44df1c156f3bf1a37143fe90add3522fefe38c00f3e66c10856a3068cde8f",
            "030f9251375a2be0f8acba2345b8798c8d4bb2014fda249e834d724cc07c5507",
            "e3a46f099d799f90cd010c0d98ce761f7ac9fb6a4c285d2aaefa3a3fd110fdd1",
            "29da36be673244c286519803acf895317fac610d594111755ce73bb9c58ea3b5",
            "b1a940039d6d7ec5248bfe24ea2d7718ddc9e2f9a39346f905402ff2a78d9b1e",
            "4120185dd8f00e260053a965687ef3bc96857e732905dd03e547e50c92b002a3",
            "8b517ca02532863fbb3f2101c29d843f5461ea13bbc599d1ca33f6a089454ccd",
            "fa125e40aaa34385144d8182a2538d3074d90de20b4a5ec0d8d2403c55b52c7f",
            "282160051e615124ae3469447740840444ab90b0e31bb5017d6dcab6d765ccc1",
            "c5d5d1824dfabae06acc8d886a638a0fa08c26c48c07f6caae38fcbaf0297707")
        for mask in range(16):
            flags = []
            if mask & 1:
                flags += ["--references", "--symbol", "ReadLine"]
            for bit, flag in ((2, "--all-symbols"), (4, "--bootstrap"), (8, "--picture-output")):
                if mask & bit:
                    flags.append(flag)
            command = [sys.executable, "-B", str(TOOL), str(BLOB)] + flags
            plain = subprocess.run(command, capture_output=True, timeout=10)
            enriched = subprocess.run(command + ["--arc-metadata"], capture_output=True, timeout=10)
            repeated = subprocess.run(command + ["--arc-metadata"], capture_output=True, timeout=10)
            with self.subTest(flags=flags):
                self.assertEqual(plain.returncode, 0, plain.stderr)
                self.assertEqual(hashlib.sha256(plain.stdout).hexdigest(), expected[mask])
                self.assertEqual(enriched.returncode, 0, enriched.stderr)
                self.assertEqual(repeated.returncode, 0, repeated.stderr)
                self.assertEqual(enriched.stdout, repeated.stdout)
                result = json.loads(enriched.stdout)
                self.assertEqual(result.pop("arc_metadata"), self.mapping())
                self.assertEqual(result, json.loads(plain.stdout))
                self.assertNotIn(str(ROOT).encode(), enriched.stdout)
        self.assertEqual(hashlib.sha256(BLOB.read_bytes()).hexdigest(), MAP.BUNDLED_SHA256)


class FirmwareCscCommandTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]

    def mapping(self, payload=None, images=None):
        return MAP._csc_command_map(self.payload if payload is None else payload,
                                    self.images if images is None else images)

    def test_csc_local_path_schema_and_signed_comparisons(self):
        result = MAP.analyze(self.data, csc_command=True)["csc_command"]
        self.assertEqual(result, self.mapping())
        self.assertEqual((result["isa"], result["endianness"]), ("A32", "little"))
        self.assertEqual(result["command"]["value"], 0x73763180)
        self.assertEqual(result["handler_entry_blob_file_offset"], 0x5f2c)
        self.assertEqual(result["record_command_byte_offset"], 0x14)
        self.assertEqual(result["command_load_blob_file_offset"], 0x5f70)
        self.assertEqual(result["delta"], 0x78)
        self.assertFalse(result["subtract_updates_flags"])
        comparisons = result["selected_path_comparisons"]
        self.assertEqual([c["compare_blob_file_offset"] for c in comparisons],
                         [0x5f7c, 0x602c, 0x607c, 0x6088, 0x6090, 0x6098])
        self.assertEqual([c["rhs"] for c in comparisons], [0x73763108, 0x1c, 0x89, 0x2e, 0x3c, 0x88])
        self.assertTrue(all(c["lhs"] > c["rhs"] for c in comparisons[:2]))
        self.assertLess(comparisons[2]["lhs"], comparisons[2]["rhs"])
        self.assertTrue(all(c["lhs"] != c["rhs"] for c in comparisons[3:]))
        self.assertEqual([c["relation"] for c in comparisons],
                         ["signed greater than", "signed greater than", "signed less than"] + ["not equal"] * 3)
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        self.assertEqual(anchors[0x5f78]["literal_blob_file_offset"], 0x6174)
        self.assertEqual(anchors[0x5f78]["literal_value"], 0x73763108)
        self.assertEqual(anchors[0x5f80]["word"], 0xe0410002)  # SUB, not SUBS.
        self.assertEqual([anchors[p]["condition"] for p in (0x5f88, 0x6034, 0x6084)], [12] * 3)
        self.assertEqual([anchors[p]["target_blob_file_offset"] for p in (0x5f88, 0x6034, 0x609c)],
                         [0x602c, 0x607c, 0x60b8])
        self.assertEqual(result["instruction_anchor_count"], 28)
        self.assertEqual(len(anchors), 28)

    def test_csc_fallback_register_facts_keep_unvalidated_call_boundary(self):
        result = self.mapping()
        fallback = result["local_fallback"]
        self.assertEqual(fallback["diagnostic"], {
            "blob_file_offset": 0x61a8, "text": "[fw] SMP_CmdApi_ProcessHstCmd(): Unknown Command\n",
            "nul_terminated": True})
        self.assertEqual(fallback["logging_call_blob_file_offset"], 0x60bc)
        self.assertEqual(fallback["logging_callee_blob_file_offset"], 0x203c4)
        self.assertFalse(fallback["logging_callee_body_validated"])
        self.assertEqual(fallback["value_register_setup"], {"blob_file_offset": 0x5f74, "register": 5, "value": 8})
        self.assertEqual(fallback["record_byte_store"], {
            "blob_file_offset": 0x60c0, "base_register": 4, "source_register": 5, "byte_offset": 0x10})
        self.assertNotIn("value", fallback["record_byte_store"])
        self.assertEqual(fallback["return_blob_file_offset"], 0x60c4)
        self.assertIn("preserve callee-saved r4/r5", result["register_flow_assumption"])
        self.assertIn("conditional", result["register_flow_assumption"])
        self.assertTrue(any("local instruction path" in text for text in result["limitations"]))
        encoded = json.dumps(result).lower()
        for prohibited in ("reply", "acknowledg", "response_status", "sequence", "matrix_payload",
                           "silicon_incapability", "global_absence", "registers_safe"):
            self.assertNotIn(prohibited, encoded)

    def test_csc_source_enum_and_library_packing_context_fuses(self):
        result = self.mapping()
        header = (ROOT / "include/7411d.h").read_text()
        base = int(re.search(r"#define\s+eCMD_C011_CMD_BASE\s+\((0x[0-9a-fA-F]+)\)", header)[1], 16)
        offset = int(re.search(r"eCMD_C011_DEC_CHAN_SET_CSC\s*=\s*eCMD_C011_CMD_BASE\s*\+\s*(0x[0-9a-fA-F]+)", header)[1], 16)
        self.assertEqual(base + offset, result["command"]["value"])
        wrapper = (ROOT / "linux_lib/libcrystalhd/libcrystalhd_if.cpp").read_text()
        wrapper = wrapper.split("DtsSetColorSpace(", 1)[1].split("DtsGetDILPath(", 1)[0]
        self.assertIn("return DtsSetOutputColorSpace(hDevice, Mode422);", wrapper)
        packing = (ROOT / "linux_lib/libcrystalhd/libcrystalhd_int_if.cpp").read_text()
        packing = packing.split("DtsProgramFleaColorSpace(", 1)[1].split("DtsSetOutputColorSpace(", 1)[0]
        self.assertIn("DtsDevRegisterRead(hDevice, BCHP_MISC2_GLOBAL_CTRL, &Val)", packing)
        self.assertIn("Val &= 0x0000007c;", packing)
        self.assertRegex(packing, r"if\( ModeSelect == OUTPUT_MODE422_YUY2 \)\s*\{\s*Val \|= BC_BIT\(1\);")
        self.assertIn("DtsDevRegisterWr(hDevice, BCHP_MISC2_GLOBAL_CTRL, Val)", packing)
        self.assertEqual(result["library_context"]["packing_selection"], ["YUY2", "UYVY"])
        self.assertEqual(result["library_context"]["register_symbol"], "MISC2_GLOBAL_CTRL")
        self.assertIn("not a firmware matrix route", result["library_context"]["kind"])
        self.assertNotIn(str(ROOT), json.dumps(result))

    def test_csc_every_fixed_instruction_bit_rejected_before_branch_decode(self):
        result = self.mapping()
        for anchor in result["instruction_anchors"]:
            offset = anchor["blob_file_offset"]
            for bit in range(32):
                payload = bytearray(self.payload)
                struct.pack_into("<I", payload, offset, anchor["word"] ^ (1 << bit))
                with self.subTest(offset=offset, bit=bit), \
                        mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("unexpected branch decode")), \
                        self.assertRaisesRegex(MAP.FormatError, "word"):
                    self.mapping(payload=payload)

    def test_csc_literal_and_every_diagnostic_byte_rejected_before_branch_decode(self):
        result = self.mapping()
        diagnostic = result["local_fallback"]["diagnostic"]
        offsets = list(range(0x6174, 0x6178))
        offsets += range(diagnostic["blob_file_offset"], diagnostic["blob_file_offset"] + len(diagnostic["text"]) + 1)
        for offset in offsets:
            payload = bytearray(self.payload)
            payload[offset] ^= 1
            with self.subTest(offset=offset), \
                    mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("unexpected branch decode")), \
                    self.assertRaises(MAP.FormatError):
                self.mapping(payload=payload)

    def test_csc_own_anchor_budget_and_private_bounds(self):
        self.assertEqual(MAP.MAX_CSC_COMMAND_ANCHORS, 32)
        with mock.patch.object(MAP, "MAX_CSC_COMMAND_ANCHORS", 27), \
                mock.patch.object(MAP, "_bootstrap_word", side_effect=AssertionError("unexpected word read")), \
                self.assertRaisesRegex(MAP.FormatError, "budget"):
            self.mapping()
        for payload, images in ((self.payload[:-4], self.images), (self.payload, self.images[::-1]),
                                (self.payload, self.images[:1]), (self.payload, self.images + self.images)):
            with self.assertRaisesRegex(MAP.FormatError, "identities"):
                self.mapping(payload, images)

    def test_csc_public_pin_precedes_every_existing_parser_combination(self):
        changed = bytearray(self.data)
        changed[0x60bc] ^= 1
        for data in (fixture(), self.data[:-4], bytes(changed)):
            for mask in range(32):
                with self.subTest(size=len(data), options=mask), \
                        mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                        mock.patch.object(MAP, "_csc_command_map", side_effect=AssertionError("unexpected CSC parse")):
                    with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                        MAP.analyze(data, expected_sha256=hashlib.sha256(data).hexdigest(), csc_command=True,
                                    references=bool(mask & 1), all_symbols=bool(mask & 2), bootstrap=bool(mask & 4),
                                    picture_output=bool(mask & 8), arc_metadata=bool(mask & 16))
        with mock.patch.object(MAP.hashlib, "sha256") as digest, \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")):
            digest.return_value.hexdigest.return_value = MAP.BUNDLED_SHA256
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(fixture(), csc_command=True)

    def test_csc_pure_mapping_and_cli_pin_failure_emit_no_json(self):
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file read")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected open")), \
                mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected execution")):
            self.mapping()
            MAP.analyze(self.data, csc_command=True)
        data = fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data), \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(MAP, "_csc_command_map", side_effect=AssertionError("unexpected CSC parse")), \
                mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO) as error:
            self.assertEqual(MAP.main(["fixture.bin", "--csc-command", "--expect-sha256",
                                       hashlib.sha256(data).hexdigest()]), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("exact bundled", error.getvalue())

    def test_csc_cli_all_32_options_preserve_baseline_bytes_and_reproduce_report(self):
        expected = (
            "15068c07a81a510e02435b6203b1cf5e424152e37ff456b1da9c12b77c15799e",
            "2a491f4b9033395cdbc688810319ea159973a296d7828ac65153bc93d45b588f",
            "15af1023987b5f2aee1e9a5560f749560bf33f729252ca3fcb9f41358f49e5aa",
            "85bec0f347f91ccfbb802d07b4c65fb07c8925625710dbe525fd7c3c1376487e",
            "ac366ee6fa1912d70dc816979b494817863da8fff6687e7aabc20f67633eea6e",
            "6ed302c739450fd7302bb65a388a74dc934e914bfbb3c67f26304647a2bca012",
            "3af44df1c156f3bf1a37143fe90add3522fefe38c00f3e66c10856a3068cde8f",
            "030f9251375a2be0f8acba2345b8798c8d4bb2014fda249e834d724cc07c5507",
            "e3a46f099d799f90cd010c0d98ce761f7ac9fb6a4c285d2aaefa3a3fd110fdd1",
            "29da36be673244c286519803acf895317fac610d594111755ce73bb9c58ea3b5",
            "b1a940039d6d7ec5248bfe24ea2d7718ddc9e2f9a39346f905402ff2a78d9b1e",
            "4120185dd8f00e260053a965687ef3bc96857e732905dd03e547e50c92b002a3",
            "8b517ca02532863fbb3f2101c29d843f5461ea13bbc599d1ca33f6a089454ccd",
            "fa125e40aaa34385144d8182a2538d3074d90de20b4a5ec0d8d2403c55b52c7f",
            "282160051e615124ae3469447740840444ab90b0e31bb5017d6dcab6d765ccc1",
            "c5d5d1824dfabae06acc8d886a638a0fa08c26c48c07f6caae38fcbaf0297707",
            "3553b947d6948d11fc48b2994ca29599caa8a70ff7b79d7ffc2639901c9aedfe",
            "6da05d4dca3424ef76e9359ed7ab3228d5c2622dcd1573b62bc88d2b0c3f2e7b",
            "6946e167d1dfbb01632025d014ebd76284aafcf58f79099881552c6fc80a4964",
            "839f141d887e74b8e5d9da871b2160ba15ab5ce5ad6acc77a87a7def68ef4ce6",
            "906538ed231b8a4489de1cce55c64490ead78d757d36293920395e4533585755",
            "eddf4aa514e9482a498eb30e0b17971f3bea17237be49fed9fa944b9285143c0",
            "6f47641210af11c430a49efb3902a8ccc2aed02c1a42949597983bdd91140290",
            "b3e03713ff8755b3af019f5e6e15d77e8e1321f9afc888d6c6d83d4ee483eb6e",
            "364c6ef9941888f688e41bd9a5183f9da3983c9790c77ab890ab80cceb5939ba",
            "b3f0ef6dffee5ae2aa6bdf26b0cc65adb6361c4974e6218c9a467786823eba77",
            "70a85da6ec14d6da170f43d05ebc1157cf8df8481a88b1c57ca798f749474e77",
            "b1d0a05a83ea675acc44763ceecccfdfeed8330e5a781e925a295a553d6133df",
            "58275e64de4fb470b04ff0c213c97bb7e8a465c9d786551816cdbdbb3d5f843e",
            "55a12601d03051d002dc7057e1c8d74537c17253235be6fcaad09d4f41a37235",
            "51b4c084a2aff04d8efe6ebb9045a3bac91d27a8c7622a107646a3d669181a08",
            "ee0d3c8d38b912c2e8806b085c9dd8761aa5f70894971aba7e4b63cb9aef2280")
        for mask in range(32):
            flags = []
            if mask & 1:
                flags += ["--references", "--symbol", "ReadLine"]
            for bit, flag in ((2, "--all-symbols"), (4, "--bootstrap"), (8, "--picture-output"), (16, "--arc-metadata")):
                if mask & bit:
                    flags.append(flag)
            command = [sys.executable, "-B", str(TOOL), str(BLOB)] + flags
            plain = subprocess.run(command, capture_output=True, timeout=10)
            enriched = subprocess.run(command + ["--csc-command"], capture_output=True, timeout=10)
            repeated = subprocess.run(command + ["--csc-command"], capture_output=True, timeout=10)
            with self.subTest(flags=flags):
                self.assertEqual(plain.returncode, 0, plain.stderr)
                self.assertEqual(hashlib.sha256(plain.stdout).hexdigest(), expected[mask])
                self.assertEqual(enriched.returncode, 0, enriched.stderr)
                self.assertEqual(repeated.returncode, 0, repeated.stderr)
                self.assertEqual(enriched.stdout, repeated.stdout)
                result = json.loads(enriched.stdout)
                self.assertEqual(result.pop("csc_command"), self.mapping())
                self.assertEqual(result, json.loads(plain.stdout))
                self.assertNotIn(str(ROOT).encode(), enriched.stdout)
        self.assertEqual(hashlib.sha256(BLOB.read_bytes()).hexdigest(), MAP.BUNDLED_SHA256)


class FirmwareCommandBufferBridgeTests(unittest.TestCase):
    OPTIONS = ("references", "all_symbols", "bootstrap", "picture_output", "arc_metadata", "csc_command")

    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]

    def mapping(self, payload=None, images=None):
        return MAP._command_buffer_bridge_map(self.payload if payload is None else payload,
                                             self.images if images is None else images)

    def test_bridge_initialized_identity_heap_and_owned_symbolic_block(self):
        result = MAP.analyze(self.data, command_buffer_bridge=True)["command_buffer_bridge"]
        self.assertEqual(result, self.mapping())
        self.assertFalse(result["device_observed"])
        self.assertEqual(result["base_symbol"], "B")
        heap = result["initialized_heap"]
        self.assertEqual((heap["virtual_base"], heap["physical_base"], heap["bytes"]),
                         (0x116004, 0x116004, 0x3ee5ffc))
        self.assertEqual(heap["alignment_exponent"], 2)
        self.assertEqual(heap["inclusive_virtual_range"], [0x116068, 0x3ffc000])
        self.assertEqual(heap["map_offsets"], {"virtual_base": 0x28, "physical_base": 0x30,
                                               "inclusive_low": 0x18, "inclusive_high": 0x1c})
        self.assertTrue(heap["initialized_translation_is_identity"])
        allocation = result["allocation"]
        self.assertEqual((allocation["bytes"], allocation["alignment_exponent"]), (0x100000, 12))
        self.assertEqual((allocation["virtual_pointer_context_offset"],
                          allocation["physical_pointer_context_offset"],
                          allocation["owned_flag_byte_context_offset"], allocation["owned_flag_value"]),
                         (0x1ac, 0x1b0, 0x1b8, 1))
        self.assertTrue(allocation["packet_is_subrange_not_separate_allocation"])
        self.assertEqual(allocation["release_call_blob_file_offset"], 0x288b0)
        self.assertIn("translated allocated block base", allocation["base_definition"])

    def test_bridge_actual_init_catalog_callbacks_and_both_elf_placements(self):
        result = self.mapping()
        init = result["host_init"]
        self.assertEqual(init["command"], 0x73763001)
        header = (ROOT / "include/7411d.h").read_text()
        self.assertRegex(header, r"#define\s+eCMD_C011_CMD_BASE\s+\(0x73763000\)")
        self.assertRegex(header, r"eCMD_C011_INIT\s*=\s*eCMD_C011_CMD_BASE\s*\+\s*0x01")
        self.assertTrue(init["requires_uninitialized_host_context"])
        self.assertEqual((init["factory_call_blob_file_offset"], init["configuration_copy_bytes"]), (0x7e0, 72))
        self.assertEqual((init["callback_context_offset"], init["catalog_context_offset"]), (0x3c, 0x40))
        self.assertEqual((init["callback_table_blob_file_offset"], init["catalog_blob_file_offset"]),
                         (0xcfcf0, 0xcfbe8))
        self.assertEqual(init["combined_image_slot"], 4)
        self.assertTrue(init["combined_image_slot_is_null"])
        self.assertEqual(init["fallback_image_slots"], [0, 1])
        self.assertEqual([(p["slot"], p["image_blob_file_offset"], p["image_base_offset_from_B"],
                           p["first_data_section_index"], p["first_data_section_name"],
                           p["first_data_offset_from_B"], p["first_data_bytes"])
                          for p in result["image_placements"]],
                         [(0, 0x2ea60, 0, 17, ".vdec_cmd_block", 0x70000, 256),
                          (1, 0x79dd8, 0x90000, 50, ".rodata", 0xe0000, 384)])
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        for offset, target in ((0x60d4, 0x5ccc), (0x5d4c, 0x54c), (0x7e0, 0xe8b0),
                               (0x280b4, 0x27bd0), (0x28120, 0x27bd0), (0x27d74, 0x2af2c)):
            self.assertEqual(anchors[offset]["target_blob_file_offset"], target)
        self.assertEqual(anchors[0xe894]["literal_value"], 0xcfcf0)
        self.assertEqual(anchors[0xe89c]["literal_value"], 0xcfc00)

    def test_bridge_exact_symbol_relocation_and_little_endian_type4_before_copy(self):
        result = self.mapping()
        rela = result["outer_pointer_relocation"]
        self.assertEqual((rela["relocation_record_blob_file_offset"], rela["relocation_section_index"],
                          rela["relocation_record_index"], rela["vendor_type"], rela["symbol_index"],
                          rela["symbol_record_blob_file_offset"], rela["symbol_section_index"]),
                         (0x72f00, 51, 160, 4, 18, 0x69c90, 17))
        self.assertEqual((rela["original_symbol_value"], rela["addend"], rela["original_literal"]),
                         (0x70000, 0x100, 0x70100))
        self.assertEqual((rela["literal_blob_file_offset"], rela["literal_elf_virtual_address"]),
                         (0x483c4, 0x25830))
        self.assertEqual((rela["symbol_offset_from_B"], rela["patched_literal_offset_from_B"]),
                         (0x70000, 0x70100))
        self.assertTrue(rela["selected_literal_relocation_is_unique"])
        self.assertEqual(rela["byte_store_blob_file_offsets"], [0x29c9c, 0x29ca4, 0x29cb0, 0x29cbc])
        self.assertEqual([struct.unpack_from("<I", self.payload, p)[0]
                          for p in rela["byte_store_blob_file_offsets"]],
                         [0xe7c4600a, 0xe7c4000b, 0xe7c41000, 0xe7c41000])
        self.assertIn("32-bit little-endian S+A", rela["semantics"])
        self.assertNotIn("R_ARC", rela["semantics"])
        self.assertTrue(rela["applied_before_section_copy"])
        self.assertEqual((rela["apply_call_blob_file_offset"], rela["copy_call_blob_file_offset"]),
                         (0x2b16c, 0x2b21c))
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        self.assertEqual(anchors[0x2b010]["target_blob_file_offset"], 0x2a474)
        self.assertEqual(anchors[0x2b018]["target_blob_file_offset"], 0x2a3e0)
        self.assertEqual(anchors[0x2a178]["target_blob_file_offset"], 0x29ba4)
        self.assertEqual(anchors[0x29c20]["target_blob_file_offset"], 0x29c98)
        self.assertEqual(anchors[0x29cc0]["target_blob_file_offset"], 0x29f00)

    def test_bridge_outer_arc_operand_matches_arm_packet_with_explicit_limits(self):
        result = self.mapping()
        arm, arc = result["arm_packet"], result["outer_arc_operand"]
        self.assertEqual((arm["physical_pointer_context_offset"], arm["virtual_pointer_context_offsets"]),
                         (0x1cc, [0x94, 0x98]))
        self.assertEqual((arm["physical_store_blob_file_offset"], arm["virtual_store_blob_file_offsets"]),
                         (0x280f4, [0x28180, 0x28188]))
        self.assertEqual((arm["bytes"], arm["copy_bytes"]), (256, 252))
        self.assertEqual((arc["function"], arc["image_slot"], arc["function_elf_virtual_address"]),
                         ("Core_Command", 0, 0x25808))
        self.assertEqual((arc["load_instruction_blob_file_offset"], arc["add_negative_256_blob_file_offset"],
                          arc["dma_argument_move_blob_file_offset"], arc["dma_call_blob_file_offset"]),
                         (0x483c0, 0x483c8, 0x483cc, 0x483d4))
        self.assertEqual((arc["dma_function"], arc["dma_function_blob_file_offset"]), ("Dma_Read", 0x3026c))
        self.assertEqual((arc["dma_operand_offset_from_B"], arm["offset_from_B"]), (0x70000, 0x70000))
        self.assertTrue(arc["matches_arm_packet"])
        self.assertIn("not standalone operation or codec acceptance", result["scope"])
        self.assertTrue(any("allocations" in s and "loads succeed" in s for s in result["assumptions"]))
        self.assertTrue(any("calling convention" in s for s in result["assumptions"]))
        self.assertTrue(any("extension semantics are unvalidated" in s for s in result["assumptions"]))
        self.assertTrue(any("not an inner decoder packet" in s for s in result["limitations"]))
        self.assertTrue(any("has not been read" in s for s in result["limitations"]))
        self.assertNotIn(str(ROOT), json.dumps(result))

    def test_bridge_every_fixed_window_word_and_name_byte_fail_before_decode(self):
        for name, offset, encoded in MAP._COMMAND_BUFFER_BRIDGE_REGIONS:
            size = len(encoded) // 2
            steps = range(size) if name.endswith("_name") else range(0, size, 4)
            for delta in steps:
                payload = bytearray(self.payload)
                payload[offset + delta] ^= 1
                with self.subTest(region=name, offset=offset + delta), \
                        mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("unexpected decode")), \
                        mock.patch.object(MAP, "_a32_literal", side_effect=AssertionError("unexpected decode")), \
                        self.assertRaisesRegex(MAP.FormatError, "region"):
                    self.mapping(payload=payload)

    def test_bridge_complete_relocation_table_and_each_record_are_pinned_before_decode(self):
        for offset in list(range(0x72780, 0x78d44, 12)) + [0x78d43]:
            payload = bytearray(self.payload)
            payload[offset] ^= 1
            with self.subTest(offset=offset), \
                    mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("unexpected decode")), \
                    self.assertRaises(MAP.FormatError):
                self.mapping(payload=payload)
        for offset in range(0x72f00, 0x72f0c):
            payload = bytearray(self.payload)
            payload[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(MAP.FormatError):
                self.mapping(payload=payload)

    def test_bridge_resource_budgets_and_private_image_identity(self):
        result = self.mapping()
        self.assertEqual(len(result["validated_regions"]), 80)
        self.assertEqual(result["validated_byte_count"], 36692)
        self.assertEqual(result["relocation_record_count"], 2171)
        self.assertEqual(len(result["instruction_anchors"]), 52)
        for budget, value in (("MAX_COMMAND_BUFFER_BRIDGE_REGIONS", 79),
                              ("MAX_COMMAND_BUFFER_BRIDGE_BYTES", 36691),
                              ("MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS", 2170)):
            with mock.patch.object(MAP, budget, value), \
                    mock.patch.object(MAP, "bounded", side_effect=AssertionError("unexpected read")), \
                    self.assertRaisesRegex(MAP.FormatError, "budget"):
                self.mapping()
        for payload, images in ((self.payload[:-4], self.images), (self.payload, self.images[::-1]),
                                (self.payload, self.images[:1]), (self.payload, self.images + self.images)):
            with self.assertRaisesRegex(MAP.FormatError, "identities"):
                self.mapping(payload, images)
        for key, value in (("flags", 1), ("machine", 93), ("section_count", 54), ("endianness", "big")):
            images = [dict(i) for i in self.images]
            images[0][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(MAP.FormatError, "identities"):
                self.mapping(images=images)

    def test_bridge_public_pin_precedes_every_parser_option_combination(self):
        changed = bytearray(self.data)
        changed[0x29c9c] ^= 1
        for data in (fixture(), self.data[:-4], bytes(changed)):
            for mask in range(64):
                with self.subTest(size=len(data), mask=mask), \
                        mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                        mock.patch.object(MAP, "_command_buffer_bridge_map", side_effect=AssertionError("unexpected bridge parse")), \
                        self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                    MAP.analyze(data, expected_sha256=hashlib.sha256(data).hexdigest(),
                                command_buffer_bridge=True,
                                **{name: bool(mask & (1 << bit)) for bit, name in enumerate(self.OPTIONS)})
        with mock.patch.object(MAP.hashlib, "sha256") as digest, \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")):
            digest.return_value.hexdigest.return_value = MAP.BUNDLED_SHA256
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(fixture(), command_buffer_bridge=True)

    def test_bridge_pure_map_and_cli_pin_failure_without_output(self):
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file read")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected open")), \
                mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected execution")):
            self.mapping()
            MAP.analyze(self.data, command_buffer_bridge=True)
        data = fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data), \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO) as error:
            self.assertEqual(MAP.main(["fixture.bin", "--command-buffer-bridge", "--expect-sha256",
                                       hashlib.sha256(data).hexdigest()]), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("exact bundled", error.getvalue())

    def test_bridge_64_option_snapshots_and_new_report_determinism(self):
        aggregate = hashlib.sha256()
        mapping = self.mapping()
        for mask in range(64):
            options = {name: bool(mask & (1 << bit)) for bit, name in enumerate(self.OPTIONS)}
            wanted = ("ReadLine",) if mask & 1 else MAP.DEFAULT_SYMBOLS
            plain = MAP.analyze(self.data, wanted, **options)
            stdout = (json.dumps(plain, indent=2, sort_keys=True) + "\n").encode()
            aggregate.update(bytes([mask]))
            aggregate.update(hashlib.sha256(stdout).digest())
            enriched = MAP.analyze(self.data, wanted, command_buffer_bridge=True, **options)
            with self.subTest(mask=mask):
                self.assertEqual(enriched.pop("command_buffer_bridge"), mapping)
                self.assertEqual(enriched, plain)
        self.assertEqual(aggregate.hexdigest(), "cc146f7fe9d3120a786986e7bfa0d6a0c72ee87354ec0ed9b390c72879736d15")
        self.assertEqual(hashlib.sha256(BLOB.read_bytes()).hexdigest(), MAP.BUNDLED_SHA256)

    def test_bridge_cli_flag_combinations_and_repeated_stdout(self):
        flags = ("--references", "--all-symbols", "--bootstrap", "--picture-output", "--arc-metadata", "--csc-command")
        for selected in ((), *[(flag,) for flag in flags], flags):
            command = [sys.executable, "-B", str(TOOL), str(BLOB), "--command-buffer-bridge", *selected]
            first = subprocess.run(command, capture_output=True, timeout=10)
            repeated = subprocess.run(command, capture_output=True, timeout=10)
            with self.subTest(flags=selected):
                self.assertEqual(first.returncode, 0, first.stderr)
                self.assertEqual(repeated.returncode, 0, repeated.stderr)
                self.assertEqual(first.stdout, repeated.stdout)
                self.assertEqual(json.loads(first.stdout)["command_buffer_bridge"], self.mapping())
                self.assertNotIn(str(ROOT).encode(), first.stdout)

    def test_bridge_two_type6_calls_preserve_exact_words_and_signed_pc_bias(self):
        result = self.mapping()
        self.assertEqual(len(result["outer_call_relocations"]), 2)
        expected = (("Dma_Read", 0x72f0c, 646, 0x6c3d0, 0x25840, 0x53c8, -132220, 0x2fbf70a0, "a070bf2f"),
                    ("Dma_Sync", 0x72f18, 644, 0x6c3b0, 0x25848, 0x5364, -132328, 0x2fbf6320, "2063bf2f"))
        for call, (name, record, index, definition, source, symbol, delta, word, encoded) in zip(
                result["outer_call_relocations"], expected):
            with self.subTest(target=name):
                self.assertEqual((call["target"], call["relocation_record_blob_file_offset"],
                                  call["symbol_index"], call["symbol_record_blob_file_offset"]),
                                 (name, record, index, definition))
                self.assertEqual((call["vendor_type"], call["target_section_index"], call["addend"]), (6, 2, 0))
                self.assertEqual((call["normalized_source_address"], call["normalized_symbol_address"],
                                  call["signed_byte_displacement"]), (source, symbol, delta))
                self.assertEqual((call["original_word"], call["patched_word"]), (word, word))
                self.assertEqual((call["original_bytes_hex"], call["patched_bytes_hex"]), (encoded, encoded))
                self.assertTrue(call["instruction_bytes_unchanged"])
                self.assertEqual(call["write_byte_order"], "little")
                self.assertEqual(call["byte_store_blob_file_offsets"], [0x29dd8, 0x29de0, 0x29dec, 0x29df8])
                self.assertEqual((call["signed_displacement_bits"], call["pc_bias_bytes"],
                                  call["displacement_field_bits"], call["displacement_scale_bytes"]), (22, 4, 20, 4))
                self.assertEqual((call["preserved_mask"], call["displacement_mask"]), (0xf800007f, 0x07ffff80))
                self.assertTrue(call["four_byte_aligned"])
                self.assertEqual(delta & 3, 0)
                self.assertTrue(-(1 << 21) <= delta < (1 << 21))
                self.assertEqual(word & ~call["displacement_mask"], word & call["preserved_mask"])
                self.assertEqual(word & call["preserved_mask"], 0x28000020)  # BL opcode, condition/delay bits.
                packed = (word & call["preserved_mask"]) | ((delta << 5) & call["displacement_mask"])
                self.assertEqual(packed, word)
                displacement = (word >> 7) & 0xfffff
                if displacement & 0x80000:
                    displacement -= 0x100000
                self.assertEqual(displacement * 4, delta)
                self.assertEqual(source + 4 + displacement * 4, symbol)
                self.assertTrue(call["normalization_excludes_B"])
                self.assertEqual(call["loaded_source_offset_from_B"], source)
                self.assertEqual(call["loaded_target_offset_from_B"], symbol)
                for base in (0x117000, 0x2e0000, 0x3000000):
                    self.assertEqual((base + source) + 4 + delta, base + symbol)
                self.assertFalse(call["runtime_call_observed"])

    def test_bridge_type6_diagnostic_fallthrough_and_explicit_validation_scope(self):
        result = self.mapping()
        for call in result["outer_call_relocations"]:
            self.assertEqual(call["firmware_diagnostics"], {"alignment_check_satisfied": True,
                                                            "signed22_range_check_satisfied": True,
                                                            "invalid_checks_log_then_fall_through": True,
                                                            "fallthrough_requires_logging_callees_to_return": True,
                                                            "diagnostic_path_taken_for_fixed_record": False})
        self.assertEqual(result["validation_scope"], {"selected_type4_literals": 1, "selected_type6_calls": 2,
                                                       "all_other_relocations_validated": False,
                                                       "vendor_extensions_validated": False,
                                                       "host_memory_access_validated": False,
                                                       "operational_dma_observed": False})
        self.assertTrue(any("Only the selected type-4 literal and two type-6 call relocations" in s
                            for s in result["limitations"]))
        anchors = {a["blob_file_offset"]: a for a in result["instruction_anchors"]}
        for offset, target in ((0x29c28, 0x29d50), (0x29d64, 0x29d84),
                               (0x29d94, 0x29db4), (0x29dfc, 0x29f00)):
            self.assertEqual(anchors[offset]["target_blob_file_offset"], target)
        # The invalid-alignment and invalid-range logging paths end in NOP,
        # then flow into range checking/patching, not a rejecting return.
        self.assertEqual(MAP._bootstrap_word(self.payload, 0x29d80), 0xe320f000)
        self.assertEqual(MAP._bootstrap_word(self.payload, 0x29db0), 0xe320f000)

    def test_bridge_type6_handler_masks_pc_range_byte_order_and_identity_mutations(self):
        for offset, replacement in ((0x29d58, 0xe0860000), (0x29d5c, 0xe2407008),
                                    (0x29d60, 0xe3170001), (0x29d84, 0xe1b00a47),
                                    (0x29d94, 0xea000006), (0x29db4, 0xe1a07207),
                                    (0x29db8, 0xe3c7731e), (0x29dbc, 0xe3c7703f),
                                    (0x29ddc, 0xe7e70855), (0x29df8, 0xe7c4000a),
                                    (0x72f0c, 0x25844), (0x72f10, 0x28604), (0x72f14, 4),
                                    (0x72f18, 0x2584c), (0x72f1c, 0x28506), (0x72f20, 4),
                                    (0x6c3b4, 0x5368), (0x6c3bc, 0x00100012),
                                    (0x483d4, 0x2fbf70c0), (0x483dc, 0x27bf6320)):
            payload = bytearray(self.payload)
            struct.pack_into("<I", payload, offset, replacement)
            with self.subTest(offset=offset), \
                    mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("unexpected decode")), \
                    self.assertRaises(MAP.FormatError):
                self.mapping(payload=payload)


class FirmwareInnerDescriptorTests(unittest.TestCase):
    OPTIONS = ("references", "all_symbols", "bootstrap", "picture_output", "arc_metadata",
               "csc_command", "command_buffer_bridge")

    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]

    def mapping(self, payload=None, images=None):
        payload = self.payload if payload is None else payload
        result = MAP._inner_descriptor_map(payload, self.images if images is None else images)
        result["paths"]["record_pointer_and_boundary"]["conditional_inner_dispatch"] = MAP._inner_dispatch_map(payload)
        return result

    def word(self, slot, address):
        # Independent ELF VM/file conversions for the selected sections.
        layouts = ((0, 0x4000, 0x63d4, 0x2eea4),
                   (0, 0x7f8c, 0xc158, 0x32e30), (0, 0x23d74, 0x3b3c8, 0x46908),
                   (1, 0x23e0, 0x2c5c, 0x7a61c), (1, 0x40f68, 0x4a210, 0xb8769),
                   (1, 0x40368, 0x40824, 0xb7b69))
        for image, low, high, offset in layouts:
            if slot == image and low <= address < high:
                return struct.unpack_from("<I", self.payload, offset + address - low)[0]
        self.fail("test VM outside selected sections")

    def test_inner_descriptor_two_paths_and_distinct_state_bases(self):
        result = MAP.analyze(self.data, inner_descriptor=True)["inner_descriptor"]
        self.assertEqual(result, self.mapping())
        self.assertFalse(result["device_observed"])
        self.assertEqual(result["basis"], "original pre-relocation ELF bytes")
        self.assertIn("not host addresses", result["address_domain"])
        self.assertEqual(set(result["paths"]), {"record_pointer_and_boundary", "mpeg_argument_return_byte"})
        first = result["paths"]["record_pointer_and_boundary"]
        record = first["record_F"]
        second = result["paths"]["mpeg_argument_return_byte"]
        self.assertEqual((first["core_state_local_base"], second["parser_state_local_base"],
                          second["inner_packet_local_base"], second["inner_state_local_base"]),
                         (0x3fffcf70, 0x3fffc000, 0x3fffc2f0, 0x3fffc200))
        self.assertEqual((first["pointer_state_offset"], first["boundary_state_offset"],
                          record["core_prefix_offset"], record["pointer_P_offset"], record["boundary_offset"]),
                         (120, 124, 92, 28, 32))
        self.assertEqual(first["pointer_state_offset"] - record["core_prefix_offset"], record["pointer_P_offset"])
        self.assertEqual(first["boundary_state_offset"] - record["core_prefix_offset"], record["boundary_offset"])
        self.assertEqual((record["producer_record_base_core_state_offset"], record["reader_record_base_local_address"],
                          record["completion_record_base_local_address"]), (88, 0x3fffd2dc, 0x3fffd2dc))
        self.assertFalse(record["same_record_pool_identity_validated"])
        self.assertEqual((record["prefix_bytes"], record["stride_bytes"], first["inner_shared_prefix"]["bytes"],
                          first["selected_mpeg_packet_P"]["copy_bytes_under_base_model"]), (56, 284, 48, 256))
        self.assertEqual(first["allocator_success_return_value"], 0)
        boundary = first["completion_boundary"]
        self.assertEqual(boundary["allocator_boundary_local_address"], 0x3fffd370 - 84)
        self.assertIn("not free or quiescence", boundary["meaning"])
        self.assertEqual((second["argument_value"], second["parser_byte_offset"], second["copy_source_state_offset"],
                          second["copy_bytes"], second["packet_P_destination_offset"], second["packet_P_byte_offset"]),
                         (3, 124, 120, 16, 180, 184))
        self.assertEqual(180 + (124 - 120), second["packet_P_byte_offset"])
        self.assertEqual(second["inner_packet_local_base"] + 184, second["inner_packet_word_local_address"])
        self.assertEqual(second["inner_packet_word_local_address"], 0x3fffc400 - 88)
        self.assertEqual(second["inner_state_local_base"] - 56, 0x3fffc1c8)
        self.assertEqual(second["comparison_values"], [2, 1, 3])
        self.assertTrue(all(value is False for value in result["validation_scope"].values()))
        for phrase in ("relocation effects", "register preservation", "distinct record-base fields",
                       "not a direct cross-image", "computed STATUS", "coherence", "bitstream parsing"):
            self.assertTrue(any(phrase in text for text in result["limitations"]), phrase)
        self.assertNotIn(str(ROOT), json.dumps(result))

        # Existing private proofs retain their original dependency/budget scope.
        private = MAP._inner_descriptor_map(self.payload, self.images)
        self.assertNotIn("conditional_inner_dispatch", private["paths"]["record_pointer_and_boundary"])

    def test_inner_descriptor_independent_original_calls_lengths_fields_and_delay_slots(self):
        first = self.mapping()["paths"]["record_pointer_and_boundary"]
        second = self.mapping()["paths"]["mpeg_argument_return_byte"]
        self.assertEqual((first["outer_record_write"]["call_elf_virtual_address"],
                          first["outer_record_write"]["delay_slot_elf_virtual_address"],
                          first["outer_record_read"]["call_elf_virtual_address"],
                          first["outer_record_read"]["delay_slot_elf_virtual_address"],
                          first["outer_record_read"]["sync_call_elf_virtual_address"]),
                         (0xa7a8, 0xa7ac, 0xad00, 0xad04, 0xad08))
        packet = first["selected_mpeg_packet_P"]
        self.assertEqual((packet["conditional_call_elf_virtual_address"],
                          packet["original_callee_elf_virtual_address"],
                          packet["copy_call_elf_virtual_address"],
                          packet["copy_delay_slot_elf_virtual_address"]), (0x2958, 0x441e4, 0x44200, 0x44204))
        self.assertEqual((second["call_elf_virtual_address"], second["argument_delay_slot_elf_virtual_address"],
                          second["byte_store_elf_virtual_address"], second["copy_call_elf_virtual_address"],
                          second["copy_delay_slot_elf_virtual_address"]),
                         (0x2d4dc, 0x2d4e0, 0x2d4e4, 0x2ea44, 0x2ea48))
        self.assertEqual(second["comparison_elf_virtual_addresses"], [0x40610, 0x40640, 0x40664])
        self.assertEqual(second["conditional_branch_elf_virtual_addresses"], [0x40618, 0x40644, 0x40668])
        self.assertEqual(second["original_branch_target_elf_virtual_addresses"], [0x40640, 0x40664, 0x40674])
        # Base GNU ARC branch interpretation only; not applied vendor relocations.
        for slot, address, target in ((0, 0x2daa4, 0x8758), (0, 0xa7a8, 0x537c),
                                      (0, 0xad00, 0x53c8), (0, 0xad08, 0x5364),
                                      (0, 0x2d4dc, 0x4374), (0, 0x2ea30, 0x5550),
                                      (0, 0x2ea44, 0x537c), (1, 0x2840, 0x2160),
                                      (1, 0x2958, 0x441e4), (1, 0x44200, 0x2160),
                                      (1, 0x40618, 0x40640), (1, 0x40644, 0x40664),
                                      (1, 0x40668, 0x40674)):
            word = self.word(slot, address)
            displacement = (word >> 7) & 0xfffff
            if displacement & 0x80000:
                displacement -= 0x100000
            with self.subTest(slot=slot, address=address):
                self.assertEqual(address + 4 + 4 * displacement, target)
        expected = ((0, 0x888c, 0x10008a78), (0, 0x8890, 0x1000807c), (0, 0x8898, 0x50000000),
                    (0, 0x2daa8, 0x401ffe80), (0, 0xa788, 0x605ffe38), (0, 0xa7ac, 0x605ffe38),
                    (0, 0xad04, 0x605ffe38), (0, 0xad0c, 0x0847801c),
                    (0, 0x99d8, 0x080b0020), (0, 0x99e4, 0x100881ac),
                    (0, 0x2d4e0, 0x601ffe03), (0, 0x2d4e4, 0x1047007c),
                    (0, 0x2ea34, 0x605ffe10), (0, 0x2ea48, 0x605ffe10),
                    (1, 0x2844, 0x605ffe30), (1, 0x295c, 0x6009a600),
                    (1, 0x44204, 0x405ffe80), (1, 0x44154, 0x094781a8),
                    (1, 0x44164, 0x100715c8), (1, 0x405ec, 0x088385c8),
                    (1, 0x40610, 0x57e27a02), (1, 0x4061c, 0x100d81f4),
                    (1, 0x40640, 0x57e27a01), (1, 0x40664, 0x57e27a03))
        for slot, address, value in expected:
            with self.subTest(slot=slot, address=address):
                self.assertEqual(self.word(slot, address), value)
        # ADD-alias has both source register fields equal to short-immediate 128.
        for slot, address in ((0, 0x2daa8), (1, 0x44204)):
            word = self.word(slot, address)
            self.assertEqual(word >> 27, 8)
            self.assertEqual((word >> 15) & 63, (word >> 9) & 63)
            self.assertEqual(word & 511, 128)
            self.assertEqual(128 + 128, 256)

    def test_inner_dispatch_conditional_context_not_runtime_certificate(self):
        result = self.mapping()["paths"]["record_pointer_and_boundary"]["conditional_inner_dispatch"]
        self.assertEqual(result, MAP._inner_dispatch_map(self.payload))
        self.assertFalse(result["device_observed"])
        self.assertIn("STATUS next-word-PC", result["interpretation"])
        self.assertEqual((len(result["validated_regions"]), result["validated_byte_count"],
                          result["relocation_record_count"]), (29, 3573, 53))
        context = result["prefix_context_link"]
        self.assertEqual((context["packet_codec_byte_offset"], context["packet_channel_byte_offset"],
                          context["prefix_local_destination"], context["prefix_copy_bytes"]),
                         (0, 4, 0x3fffc014, 48))
        self.assertEqual((context["cached_channel_local_address"], context["cached_codec_local_address"]),
                         (0x3fffc094, 0x3fffc095))
        self.assertEqual(context["cache_store_elf_virtual_addresses"], [0x276c, 0x2774])
        self.assertFalse(context["caller_checks_context_status"])
        self.assertFalse(context["cache_update_proves_restore_success"])
        self.assertFalse(context["explicit_failure_status_protocol_validated"])
        self.assertFalse(context["same_context_generation_validated"])
        self.assertTrue(all(value is False for value in self.mapping()["validation_scope"].values()))
        for phrase in ("Invalid channel", "caller does not check", "parser-independent"):
            self.assertTrue(any(phrase in item for item in result["limitations"]))
        self.assertNotIn(str(ROOT), json.dumps(result))

    def test_inner_dispatch_independent_words_and_all_256_selector_values(self):
        result = MAP._inner_dispatch_map(self.payload)
        # Independent VM conversion and original words, not mapper constants.
        expected_words = {
            0x2848: 0x08070580, 0x284c: 0x08270504, 0x2854: 0x20000202,
            0x2858: 0x08070581, 0x285c: 0x08270500, 0x2864: 0x20000081,
            0x2868: 0x2fff6e80, 0x2764: 0x08088504, 0x2768: 0x08288500,
            0x276c: 0x10488180, 0x2774: 0x10488381,
            0x28bc: 0x08070581, 0x28c0: 0x57e07a09, 0x28c4: 0x2000238d,
            0x28c8: 0x083fa000, 0x28cc: 0x4020fe03, 0x28d0: 0x40208000,
            0x28d4: 0x38008000,
        }
        for address, expected in expected_words.items():
            with self.subTest(address=address):
                self.assertEqual(self.word(1, address), expected)
        expected_targets = (0x2900, 0x2954, 0x29e4, 0x29c8, 0x2974,
                            0x29e4, 0x29e4, 0x29e4, 0x29ac, 0x2990)
        selector = result["selector"]
        self.assertEqual((selector["table_bytes"], selector["byte_domain_size"]), (40, 256))
        diagnostic_count = 0
        for value in range(256):
            if value <= 9:
                # CONDITIONAL STATUS next-word-PC model: +3 advances three words.
                address = (((0x28c8 + 4) // 4) + 3 + value) * 4
                word = self.word(1, address)
                displacement = (word >> 7) & 0xfffff
                if displacement & 0x80000:
                    displacement -= 0x100000
                target = address + 4 + 4 * displacement
                self.assertEqual(target, expected_targets[value])
                self.assertEqual(selector["table"][value]["target_elf_virtual_address"], target)
            else:
                # Unsigned BHI guard; no out-of-table read for 10..255.
                target = 0x29e4
                self.assertEqual(selector["above_bound_target_elf_virtual_address"], target)
            diagnostic_count += target == 0x29e4
        self.assertEqual(diagnostic_count, 250)
        self.assertFalse(selector["diagnostic_is_hardware_codec_rejection"])
        self.assertFalse(selector["host_open_enum_equivalence_validated"])
        # The copied codec byte and the MPEG SiU byte are distinct fields.
        self.assertNotEqual(self.mapping()["paths"]["mpeg_argument_return_byte"]["packet_P_byte_offset"], 0)

    def test_inner_dispatch_original_rela_symbols_and_uniform_translation_oracle(self):
        result = MAP._inner_dispatch_map(self.payload)
        base = 0x79dd8
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", self.payload, base)
        sections = [struct.unpack_from("<10I", self.payload, base + header[6] + i * 40)
                    for i in range(header[-2])]
        code, rela, symbols = (sections[i] for i in (3, 69, 66))
        self.assertEqual((rela[1], rela[6], rela[7], rela[9]), (4, 66, 3, 12))
        records = [(base + rela[4] + i, struct.unpack_from("<IIi", self.payload, base + rela[4] + i))
                   for i in range(0, rela[5], 12)]
        self.assertFalse(any(a == 0x2868 or 0x28bc <= a < 0x2900 for _, (a, _, _) in records))
        expected_calls = ((0x2840, 0x2160), (0x291c, 0x43834), (0x2938, 0x2e3c),
                          (0x2958, 0x441e4), (0x2974, 0x1e1e8), (0x2990, 0x10000),
                          (0x29ac, 0x45e04), (0x29c8, 0xe97c))
        self.assertEqual([(c["call_elf_virtual_address"], c["target_elf_virtual_address"])
                          for c in result["selected_calls"]], list(expected_calls))
        for call, (address, target) in zip(result["selected_calls"], expected_calls):
            matches = [(position, record) for position, record in records if record[0] == address]
            self.assertEqual(len(matches), 1)
            position, record = matches[0]
            symbol_position = base + symbols[4] + (record[1] >> 8) * 16
            symbol = struct.unpack_from("<IIIBBH", self.payload, symbol_position)
            self.assertEqual((record[1] & 255, record[2], symbol[1]), (6, 0, target))
            self.assertEqual(call["relocation_record_blob_file_offset"], position)
            self.assertEqual(call["symbol_record_blob_file_offset"], symbol_position)
            self.assertTrue(sections[symbol[5]][2] & 4)
            for translation in (0, 0x90000, 0x100000):
                # Algebraic model only, NOT execution of the vendor relocator.
                delta = translation + symbol[1] + record[2] - (translation + address) - 4
                original = struct.unpack_from("<I", self.payload,
                                              base + code[4] + address - code[3])[0]
                patched = (original & 0xf800007f) | ((delta << 5) & 0x07ffff80)
                self.assertEqual(patched, original)
                displacement = (patched >> 7) & 0xfffff
                if displacement & 0x80000:
                    displacement -= 0x100000
                self.assertEqual(translation + address + 4 + displacement * 4,
                                 translation + target)

    def test_inner_dispatch_all_pinned_bytes_fail_before_interpretation(self):
        result = MAP._inner_dispatch_map(self.payload)
        for region in result["validated_regions"]:
            for delta in range(region["size"]):
                payload = bytearray(self.payload)
                payload[region["blob_file_offset"] + delta] ^= 1
                with self.subTest(role=region["role"], delta=delta), \
                        mock.patch.object(MAP.struct, "unpack", side_effect=AssertionError("unexpected parse")), \
                        mock.patch.object(MAP.struct, "unpack_from", side_effect=AssertionError("unexpected parse")), \
                        self.assertRaisesRegex(MAP.FormatError, "region"):
                    MAP._inner_dispatch_map(payload)

    def test_inner_dispatch_exact_budgets_and_private_size_gate(self):
        expected = MAP._inner_dispatch_map(self.payload)
        with mock.patch.object(MAP, "MAX_INNER_DISPATCH_REGIONS", 29), \
                mock.patch.object(MAP, "MAX_INNER_DISPATCH_BYTES", 3573), \
                mock.patch.object(MAP, "MAX_INNER_DISPATCH_RELOCATIONS", 53):
            self.assertEqual(MAP._inner_dispatch_map(self.payload), expected)
        for name, limit in (("MAX_INNER_DISPATCH_REGIONS", 28),
                            ("MAX_INNER_DISPATCH_BYTES", 3572),
                            ("MAX_INNER_DISPATCH_RELOCATIONS", 52)):
            with mock.patch.object(MAP, name, limit), \
                    mock.patch.object(MAP, "bounded", side_effect=AssertionError("unexpected read")), \
                    self.assertRaisesRegex(MAP.FormatError, "budget"):
                MAP._inner_dispatch_map(self.payload)
        for payload in (self.payload[:-1], self.payload + b"\0"):
            with mock.patch.object(MAP, "bounded", side_effect=AssertionError("unexpected read")), \
                    self.assertRaisesRegex(MAP.FormatError, "size"):
                MAP._inner_dispatch_map(payload)

    def test_inner_dispatch_isolated_to_explicit_public_option(self):
        with mock.patch.object(MAP, "_inner_dispatch_map", side_effect=AssertionError("unexpected dispatch")):
            MAP._inner_descriptor_map(self.payload, self.images)
            MAP._open_reply_metadata_linkage(self.payload, self.images)
            MAP.analyze(self.data)
            MAP.analyze(self.data, **dict.fromkeys(self.OPTIONS + ("scaler_fir",), True))
            with self.assertRaisesRegex(AssertionError, "unexpected dispatch"):
                MAP.analyze(self.data, inner_descriptor=True)

    def test_inner_descriptor_each_selected_byte_rejects_before_interpretation(self):
        result = self.mapping()
        for region in result["validated_regions"]:
            for delta in range(region["size"]):
                payload = bytearray(self.payload)
                offset = region["blob_file_offset"] + delta
                payload[offset] ^= 1
                with self.subTest(role=region["role"], offset=offset), \
                        mock.patch.object(MAP.struct, "unpack", side_effect=AssertionError("unexpected decode")), \
                        self.assertRaisesRegex(MAP.FormatError, "region"):
                    self.mapping(payload=payload)

    def test_inner_descriptor_immediate_assignment_is_not_unconditional_exit_identity(self):
        first = self.mapping()["paths"]["record_pointer_and_boundary"]
        binding = first["record_pool_context_snapshot"]
        assignment = binding["immediate_slot_assignment"]
        self.assertEqual((assignment["context_argument_elf_virtual_address"],
                          assignment["source_slot_write_elf_virtual_address"],
                          assignment["source_reload_elf_virtual_address"],
                          assignment["destination_address_calculation_elf_virtual_address"],
                          assignment["destination_store_elf_virtual_address"]),
                         (0x2672c, 0x267b0, 0x268c8, 0x268cc, 0x268dc))
        # Independently inspect the selected original words, not a decoder API.
        for address, expected in ((0x2672c, 0x62600000), (0x267a8, 0x4029fc00),
                                  (0x267ac, 0x600), (0x267b0, 0x10009b30),
                                  (0x268c8, 0x08008130), (0x268cc, 0x4189fc00),
                                  (0x268d0, 0x21c), (0x268d4, 0x609ffe01),
                                  (0x268d8, 0x679ffe21), (0x268dc, 0x10060000)):
            with self.subTest(address=address):
                self.assertEqual(self.word(0, address), expected)
        self.assertEqual((self.word(0, 0x267a8) >> 21) & 63, 1)
        self.assertEqual((self.word(0, 0x267a8) >> 15) & 63, 19)
        self.assertEqual((self.word(0, 0x268c8) >> 15) & 63, 1)
        self.assertEqual((self.word(0, 0x268c8) & 511) - 512, -208)
        self.assertEqual(0x600 - 208, assignment["source_slot_context_offset"])
        # 0x268cc calculates r12; the actual store is 0x268dc, not 0x268cc.
        self.assertEqual(self.word(0, 0x268cc) >> 27, 8)
        self.assertEqual((self.word(0, 0x268cc) >> 21) & 63, 12)
        self.assertEqual((self.word(0, 0x268cc) >> 15) & 63, 19)
        self.assertEqual((self.word(0, 0x268dc) >> 15) & 63, 12)
        self.assertEqual((self.word(0, 0x268dc) >> 9) & 63, 0)
        self.assertEqual(assignment["destination_slot_context_offset"], 0x21c)
        self.assertTrue(assignment["equal_value_immediately_after_store_under_base_model"])
        self.assertFalse(assignment["constructor_exit_equality_validated"])
        self.assertFalse(binding["later_source_slot_equality_validated"])
        self.assertFalse(binding["runtime_snapshot_identity_validated"])
        self.assertFalse(first["record_F"]["same_record_pool_identity_validated"])
        self.assertIn("unchanged source", assignment["scope"])
        for phrase in ("register preservation", "relocation/base model", "same valid channel",
                       "derived-pointer writes", "address overflow", "DMA visibility"):
            self.assertTrue(any(phrase in condition for condition in binding["required_conditions"]), phrase)
        self.assertTrue(any("not constructor exit identity" in text for text in self.mapping()["limitations"]))

    def test_inner_descriptor_constructor_size_and_source_are_conditional(self):
        proof = self.mapping()["paths"]["record_pointer_and_boundary"]["record_pool_context_snapshot"]["conditional_constructor_return"]
        source = proof["source_initialization"]
        word = self.word(0, 0x3b308)
        self.assertEqual(word, 0x401ffeec)
        self.assertEqual((word >> 27, (word >> 21) & 63,
                          (word >> 15) & 63, (word >> 9) & 63, word & 511),
                         (8, 0, 63, 63, 236))
        # Both ADD operands are the same signed short immediate, not one shift.
        immediate = word & 511
        if immediate & 256:
            immediate -= 512
        self.assertEqual(source["add_short_immediate_operands"], [immediate, immediate])
        self.assertEqual(immediate + immediate, source["size_bytes_under_base_model"])
        self.assertEqual(0x5bc + immediate + immediate, source["pool_context_offset_under_conditions"])
        self.assertEqual(self.word(0, 0x3b304), 0x380f8020)
        for address, target in ((0x26758, 0xa13c), (0x26760, 0x3b304),
                                (0x26768, 0x3b304), (0x26774, 0xa13c)):
            word = self.word(0, address)
            displacement = (word >> 7) & 0xfffff
            if displacement & 0x80000:
                displacement -= 0x100000
            self.assertEqual(address + 4 + 4 * displacement, target)
        for address, expected in ((0x26734, 0x61c10400), (0x26748, 0x42807c00),
                                  (0x2674c, 0x5bc), (0x2676c, 0x41a02800),
                                  (0x26778, 0x600a2800), (0x26870, 0x57e77a00),
                                  (0x26890, 0x6001040d), (0x26894, 0x67871c0d),
                                  (0x26898, 0x3000030d), (0x2689c, 0x10002dfc),
                                  (0x268a4, 0x10000600), (0x268a8, 0x10000604),
                                  (0x268ac, 0x10000608), (0x268b0, 0x40007e10)):
            with self.subTest(address=address):
                self.assertEqual(self.word(0, address), expected)
        loop = source["earlier_variable_loop"]
        self.assertEqual((loop["constructor_argument_register"], loop["store_context_offsets"],
                          loop["stride_bytes"], loop["maximum_nonclobbering_count"],
                          loop["source_clobber_iteration"]),
                         (14, [0x3fc, 0x400, 0x404, 0x408], 16, 19, 19))
        for count in range(20):
            stores = {offset + 16 * k for k in range(count)
                      for offset in (0x3fc, 0x400, 0x404, 0x408)}
            self.assertNotIn(0x530, stores)
        self.assertEqual(0x400 + 16 * 19, 0x530)
        self.assertIn(0x530, {offset + 16 * k for k in range(20)
                             for offset in (0x3fc, 0x400, 0x404, 0x408)})
        self.assertFalse(loop["actual_input_count_validated"])

    def test_inner_descriptor_return_edge_store_footprint_and_no_wrap(self):
        proof = self.mapping()["paths"]["record_pointer_and_boundary"]["record_pool_context_snapshot"]["conditional_constructor_return"]
        stores = proof["derived_word_stores"]
        for address, expected in ((0x268d8, 0x679ffe21), (0x268e0, 0x40007c00),
                                  (0x268e4, 0x150e4), (0x268e8, 0x1001013c),
                                  (0x268ec, 0x10000644), (0x268f0, 0x30000480),
                                  (0x268f4, 0x0801013c), (0x268f8, 0x80a27e03),
                                  (0x268fc, 0x50a28800), (0x26900, 0x80a2fe03),
                                  (0x26904, 0x40a28800), (0x26908, 0x80a2fe02),
                                  (0x2690c, 0x40000a00), (0x26910, 0x10000644),
                                  (0x26914, 0x40827e01), (0x26918, 0x08008130),
                                  (0x26924, 0x40407c00), (0x26928, 0x14ee4),
                                  (0x2692c, 0x10008524), (0x26938, 0x40407c00),
                                  (0x2693c, 0x14fe4), (0x26948, 0x40007c00),
                                  (0x2694c, 0x16f30), (0x26958, 0x10008528),
                                  (0x26960, 0x100081b4), (0x2696c, 0x380f8020),
                                  (0x26970, 0x0b6e1038)):
            with self.subTest(address=address):
                self.assertEqual(self.word(0, address), expected)
        self.assertEqual((stores["base_offset"], stores["stride_bytes"],
                          stores["count"], stores["word_bytes"]), (0x15128, 228, 34, 4))
        offsets = []
        for n in range(34):
            # Independently reconstruct the selected shift/sub/add chain.
            stride = (((n << 3) - n) << 3) + n
            stride <<= 2
            self.assertEqual(stride, 228 * n)
            offsets.append(0x794 + 0x150e4 + 68 + stride)
        self.assertEqual((offsets[0], offsets[-1]),
                         (stores["first_context_offset_under_conditions"],
                          stores["last_context_offset_under_conditions"]))
        self.assertEqual((offsets[0], offsets[-1]), (0x158bc, 0x17620))
        self.assertEqual(proof["direct_context_word_stores"], [
            {"elf_virtual_address": 0x268e8, "context_offset": 0x33c},
            {"elf_virtual_address": 0x2692c, "context_offset": 0x524},
            {"elf_virtual_address": 0x26958, "context_offset": 0x528},
            {"elf_virtual_address": 0x26960, "context_offset": 0x5b4}])
        direct = [0x400 - 196, 0x600 - 220, 0x600 - 216, 0x600 - 76]
        for offset in offsets + direct:
            for slot in (0x21c, 0x530):
                self.assertTrue(offset + 4 <= slot or slot + 4 <= offset)
        largest = 0x794 + 0x16f30
        self.assertEqual(largest, proof["largest_computed_context_offset_under_conditions"])
        maximum = (0xffffffff - largest) & ~3
        self.assertEqual(maximum, proof["maximum_aligned_context_base_without_wrap"])
        self.assertLessEqual(maximum + largest, 0xffffffff)
        self.assertGreater(maximum + 4 + largest, 0xffffffff)
        # No branch-with-link word exists in the selected original tail.
        self.assertTrue(all(self.word(0, address) >> 27 != 5
                            for address in range(0x268e0, 0x26974, 4)))
        self.assertFalse(proof["tail_contains_calls_under_base_model"])
        self.assertTrue(proof["equal_value_on_selected_return_under_conditions"])
        for name in ("allocation_extent_validated", "runtime_constructor_execution_validated",
                     "unconditional_constructor_exit_identity_validated"):
            self.assertFalse(proof[name])
        for phrase in ("loop semantics", "relocation effects", "at most 19", "unchanged",
                       "0xfffe8938", "stack", "non-aliasing", "concurrent"):
            self.assertTrue(any(phrase in text for text in proof["required_conditions"]), phrase)

    def test_inner_descriptor_opaque_source_has_concrete_tail_alias_counterexamples(self):
        context = 0x10000000
        for source, clobbered in ((0x0ffeb0f4, 0x21c), (0x0ffeb408, 0x530)):
            slots = {context + 0x21c: source, context + 0x530: source}
            self.assertEqual(slots[context + 0x21c], slots[context + 0x530])
            self.assertEqual(source + 0x15128, context + clobbered)
            for n in range(34):
                target = source + 0x15128 + 228 * n
                if target in slots:
                    slots[target] = 0
            self.assertNotEqual(slots[context + 0x21c], slots[context + 0x530])

    def test_inner_descriptor_same_channel_table_and_snapshot_word_offsets(self):
        first = self.mapping()["paths"]["record_pointer_and_boundary"]
        binding = first["record_pool_context_snapshot"]
        table, snapshot = binding["channel_table"], binding["selected_snapshot_copy"]
        for address, expected in ((0x2677c, 0x088d801c), (0x26780, 0x605f7c00),
                                  (0x26784, 0x3fffd470), (0x26788, 0x40017f08),
                                  (0x26790, 0x80227e05), (0x26794, 0x40000200),
                                  (0x26798, 0x10002600), (0x267a4, 0x10002800),
                                  (0x9fa4, 0x61a00000), (0x9fa8, 0x80007e05),
                                  (0x9fb4, 0x41e07c00), (0x9fb8, 0x3fffd368),
                                  (0x9fbc, 0x08078010), (0x9fc0, 0x605f7c00),
                                  (0x9fc4, 0x5bc), (0x9fc8, 0x61df7c00),
                                  (0x9fcc, 0x3fffcd70), (0x9fd4, 0x40277e3c)):
            with self.subTest(address=address):
                self.assertEqual(self.word(0, address), expected)
        self.assertEqual(table["constructor_table_literal"] + table["constructor_table_offset"],
                         table["context_entry_local_base"])
        self.assertEqual(table["activation_table_literal"] + table["activation_table_offset"],
                         table["context_entry_local_base"])
        self.assertEqual(1 << (self.word(0, 0x26790) & 511), table["channel_stride_bytes"])
        self.assertEqual(1 << (self.word(0, 0x9fa8) & 511), table["channel_stride_bytes"])
        self.assertFalse(table["channel_range_validated"])
        self.assertFalse(table["active_channel_match_validated"])
        self.assertFalse(table["entry_unchanged_validated"])
        self.assertEqual(snapshot["local_base_literal"] + snapshot["local_base_offset"], 0x3fffcdac)
        self.assertEqual(snapshot["local_destination"], 0x3fffcdac)
        self.assertEqual(snapshot["bytes"], 0x5bc)
        self.assertEqual(snapshot["word_bytes"], 4)
        self.assertEqual(snapshot["fields"], [
            {"role": "producer_record_base", "source_context_offset": 0x21c,
             "destination_local_address": 0x3fffcfc8},
            {"role": "reader_completion_record_base", "source_context_offset": 0x530,
             "destination_local_address": 0x3fffd2dc}])
        for field in snapshot["fields"]:
            self.assertEqual(snapshot["local_destination"] + field["source_context_offset"],
                             field["destination_local_address"])
            self.assertLessEqual(field["source_context_offset"] + snapshot["word_bytes"], snapshot["bytes"])
            self.assertEqual(field["source_context_offset"] % snapshot["word_bytes"], 0)
        self.assertEqual(snapshot["fields"][0]["destination_local_address"],
                         first["core_state_local_base"] + first["record_F"]["producer_record_base_core_state_offset"])
        self.assertEqual(snapshot["fields"][1]["destination_local_address"],
                         first["record_F"]["reader_record_base_local_address"])
        self.assertFalse(snapshot["successful_copy_validated"])
        self.assertIn("unchanged equal source slots", binding["propagation"])

    def test_inner_descriptor_selected_copy_calls_delays_and_chunk_bounds(self):
        snapshot = self.mapping()["paths"]["record_pointer_and_boundary"]["record_pool_context_snapshot"]["selected_snapshot_copy"]
        calls = ((0x9fd0, 0x9e74), (0x9eec, 0x5364), (0x9efc, 0x53c8),
                 (0x9f18, 0x52e0), (0x9f48, 0x5364), (0x9f5c, 0x52e0))
        for address, target in calls:
            word = self.word(0, address)
            displacement = (word >> 7) & 0xfffff
            if displacement & 0x80000:
                displacement -= 0x100000
            with self.subTest(address=address):
                self.assertEqual(address + 4 + displacement * 4, target)
        for address, expected in ((0x9ea4, 0x62600000), (0x9ea8, 0x62408200),
                                  (0x9eac, 0x62210500), (0x9ec4, 0x30051a80),
                                  (0x9ee0, 0x601ffe80), (0x9ef0, 0x61a0000a),
                                  (0x9ef4, 0x6009a600), (0x9ef8, 0x60282000),
                                  (0x9f00, 0x60469a00), (0x9f10, 0x60071c00),
                                  (0x9f14, 0x60292400), (0x9f1c, 0x60479e00),
                                  (0x9f34, 0x800a7e07), (0x9f40, 0x30051a00),
                                  (0x9f54, 0x60082000), (0x9f58, 0x60292400),
                                  (0x9f60, 0x60469a00), (0x52e0, 0x90417e02),
                                  (0x52f0, 0x67810400), (0x52f8, 0x08400000),
                                  (0x52fc, 0x40007e04), (0x5300, 0x10008400),
                                  (0x5304, 0x4020fe04)):
            with self.subTest(address=address):
                self.assertEqual(self.word(0, address), expected)
        self.assertEqual((snapshot["call_elf_virtual_address"], snapshot["destination_delay_slot_elf_virtual_address"]),
                         (0x9fd0, 0x9fd4))
        self.assertEqual(snapshot["sync_call_elf_virtual_addresses"], [0x9eec, 0x9f48])
        self.assertEqual(snapshot["local_copy_length_delay_slot_elf_virtual_addresses"], [0x9f1c, 0x9f60])
        chunks, tail = divmod(snapshot["bytes"], snapshot["chunk_bytes"])
        self.assertEqual((chunks, tail), (snapshot["full_chunks"], snapshot["tail_bytes"]))
        self.assertEqual((chunks, tail), (11, 60))
        self.assertEqual(snapshot["bytes"] % snapshot["word_bytes"], 0)
        self.assertEqual(tail % snapshot["word_bytes"], 0)

    def test_inner_descriptor_private_identity_mapping_and_budget_limits(self):
        result = self.mapping()
        self.assertEqual(len(result["validated_regions"]), 48)
        self.assertEqual(sum(r["size"] for r in result["validated_regions"]), 3039)
        self.assertEqual(len(result["instruction_windows"]), 32)
        with mock.patch.object(MAP, "MAX_INNER_DESCRIPTOR_REGIONS", 48), \
                mock.patch.object(MAP, "MAX_INNER_DESCRIPTOR_BYTES", 3039):
            self.assertEqual(self.mapping(), result)
        for name, limit in (("MAX_INNER_DESCRIPTOR_REGIONS", 47), ("MAX_INNER_DESCRIPTOR_BYTES", 3038)):
            with mock.patch.object(MAP, name, limit), \
                    mock.patch.object(MAP, "bounded", side_effect=AssertionError("unexpected read")), \
                    self.assertRaisesRegex(MAP.FormatError, "budget"):
                self.mapping()

        for payload, images in ((self.payload[:-4], self.images), (self.payload, self.images[::-1]),
                                (self.payload, self.images[:1]), (self.payload, self.images + self.images)):
            with self.assertRaisesRegex(MAP.FormatError, "identities"):
                self.mapping(payload, images)
        for slot in (0, 1):
            for key, value in (("flags", 1), ("machine", 93), ("section_count", 54), ("endianness", "big"),
                               ("class", 64), ("elf_type", 1), ("blob_file_offset", 0), ("blob_file_end", 0)):
                images = [dict(i) for i in self.images]
                images[slot][key] = value
                with self.subTest(slot=slot, key=key), self.assertRaisesRegex(MAP.FormatError, "identities"):
                    self.mapping(images=images)
        windows = list(MAP._INNER_DESCRIPTOR_WINDOWS)
        slot, role, index, address, offset, raw = windows[0]
        windows[0] = (slot, role, index, address + 4, offset, raw)
        with mock.patch.object(MAP, "_INNER_DESCRIPTOR_WINDOWS", windows), \
                self.assertRaisesRegex(MAP.FormatError, "instruction mapping"):
            self.mapping()

    def test_inner_descriptor_context_windows_mapping_extents_and_source_bounds(self):
        expected = (("constructor_context_argument", 16, 0x2672c, 0x492c0, 4),
                    ("constructor_pool_source_initialization", 16, 0x26730, 0x492c4, 76),
                    ("constructor_registration_and_slot_copy", 16, 0x2677c, 0x49310, 356),
                    ("constructor_return_edge", 16, 0x268e0, 0x49474, 148),
                    ("constructor_driver_context_size", 16, 0x3b304, 0x5de98, 8),
                    ("activation_snapshot_copy", 4, 0x9fa4, 0x34e48, 52),
                    ("context_dram_to_local_copy", 4, 0x9e74, 0x34d18, 284),
                    ("context_local_word_copy", 2, 0x52e0, 0x30184, 44))
        selected = self.mapping()["instruction_windows"][:8]
        self.assertEqual([(w["role"], w["section_index"], w["elf_virtual_address"],
                           w["blob_file_offset"], w["size"]) for w in selected], list(expected))
        self.assertTrue(all(w["image_slot"] == 0 for w in selected))
        self.assertEqual(selected[1]["elf_virtual_address"] + selected[1]["size"], 0x2677c)
        self.assertEqual(selected[2]["elf_virtual_address"] + selected[2]["size"], 0x268e0)
        self.assertEqual(selected[3]["elf_virtual_address"] + selected[3]["size"], 0x26974)
        for position in range(8):
            original = MAP._INNER_DESCRIPTOR_WINDOWS[position]
            for field, value in ((3, original[3] + 4), (4, -1), (4, len(self.payload) - 1)):
                windows = list(MAP._INNER_DESCRIPTOR_WINDOWS)
                changed = list(original)
                changed[field] = value
                windows[position] = tuple(changed)
                with self.subTest(role=original[1], field=field, value=value), \
                        mock.patch.object(MAP, "_INNER_DESCRIPTOR_WINDOWS", windows), \
                        self.assertRaises(MAP.FormatError):
                    self.mapping()

    def test_inner_descriptor_public_pin_precedes_parse_for_all_old_options(self):
        changed = bytearray(self.data)
        changed[0x33710] ^= 1
        for data in (fixture(), self.data[:-4], bytes(changed)):
            for mask in range(128):
                with self.subTest(size=len(data), mask=mask), \
                        mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                        mock.patch.object(MAP, "_inner_descriptor_map", side_effect=AssertionError("unexpected path parse")), \
                        self.assertRaises(MAP.FormatError):
                    MAP.analyze(data, expected_sha256=hashlib.sha256(data).hexdigest(), inner_descriptor=True,
                                **{name: bool(mask & (1 << bit)) for bit, name in enumerate(self.OPTIONS)})
        with mock.patch.object(MAP.hashlib, "sha256") as digest, \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")):
            digest.return_value.hexdigest.return_value = MAP.BUNDLED_SHA256
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(fixture(), inner_descriptor=True)

    def test_inner_descriptor_no_io_or_decoder_and_cli_pin_failure_has_no_output(self):
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file read")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected open")), \
                mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected execution")):
            self.mapping()
            MAP.analyze(self.data, inner_descriptor=True)
        data = fixture()
        with mock.patch.object(MAP, "read_firmware", return_value=data), \
                mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected ELF parse")), \
                mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO) as error:
            self.assertEqual(MAP.main(["fixture.bin", "--inner-descriptor", "--expect-sha256",
                                       hashlib.sha256(data).hexdigest()]), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("exact bundled", error.getvalue())

    def test_inner_descriptor_128_option_snapshots_and_exact_composition(self):
        aggregate = hashlib.sha256()
        mapping = self.mapping()
        for mask in range(128):
            options = {name: bool(mask & (1 << bit)) for bit, name in enumerate(self.OPTIONS)}
            wanted = ("ReadLine",) if mask & 1 else MAP.DEFAULT_SYMBOLS
            plain = MAP.analyze(self.data, wanted, **options)
            stdout = (json.dumps(plain, indent=2, sort_keys=True) + "\n").encode()
            aggregate.update(bytes([mask]))
            aggregate.update(hashlib.sha256(stdout).digest())
            enriched = MAP.analyze(self.data, wanted, inner_descriptor=True, **options)
            with self.subTest(mask=mask):
                self.assertEqual(enriched.pop("inner_descriptor"), mapping)
                self.assertEqual(enriched, plain)
        # Snapshot includes the opt-in MFD source field; its legacy projection
        # is independently pinned across all 256 combinations below.
        self.assertEqual(aggregate.hexdigest(), "be533551365efd90642f2ff3b2c6ef407fc15bf382933304d9e75556552ed72d")

    def test_inner_descriptor_cli_stdout_determinism_and_combinations(self):
        combinations = [[], ["--references", "--symbol", "ReadLine"],
                        ["--" + name.replace("_", "-") for name in self.OPTIONS]]
        for flags in combinations:
            command = [sys.executable, "-B", str(TOOL), str(BLOB), "--inner-descriptor"] + flags
            first = subprocess.run(command, check=False, capture_output=True)
            repeated = subprocess.run(command, check=False, capture_output=True)
            with self.subTest(flags=flags):
                self.assertEqual(first.returncode, 0, first.stderr)
                self.assertEqual(first.stderr, b"")
                self.assertEqual(repeated.returncode, 0, repeated.stderr)
                self.assertEqual(first.stdout, repeated.stdout)
                self.assertEqual(json.loads(first.stdout)["inner_descriptor"], self.mapping())
                self.assertNotIn(str(ROOT).encode(), first.stdout)

    def test_inner_descriptor_cli_accepts_no_arbitrary_selector_or_address(self):
        for option in ("--inner-descriptor=0x2958", "--inner-descriptor=1", "--inner-descriptor-budget"):
            with self.subTest(option=option), \
                    mock.patch.object(MAP, "read_firmware", side_effect=AssertionError("unexpected file read")), \
                    mock.patch.object(sys, "stderr", new_callable=io.StringIO), \
                    self.assertRaises(SystemExit) as error:
                MAP.main(["offline.bin", option])
            self.assertEqual(error.exception.code, 2)


class FirmwareMfdSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.rows = ((0, 64, 6), (1, 128, 7), (2, 256, 8))

    @staticmethod
    def record(selector=0, mode=0, form=2, field=0, offset_6c=0, offset_70=0,
               y=0x1000, c=0x8000, yn=40, cn=20):
        result = bytearray(116)
        result[8], result[0x27], result[0x28], result[0x5c] = mode, form, field, selector
        for offset, value in ((0x34, y), (0x38, c), (0x54, yn), (0x58, cn),
                              (0x6c, offset_6c), (0x70, offset_70)):
            struct.pack_into("<I", result, offset, value)
        return result

    def execute_address_helper(self, record):
        # Test-only interpreter of this fixed 448-byte A32 helper. Calls are
        # explicit stubs under the report's ABI/memory-preservation conditions.
        # It never executes firmware or follows a device/host DMA address.
        regs = [0] * 16
        regs[0], regs[1], regs[13], regs[14], regs[15] = 0x200000, 0x100000, 0x300100, 0xfffffff0, 0x1918
        memory = {0x100000 + index: byte for index, byte in enumerate(record)}
        writes, zero = [], False
        def read(address, size):
            if 0 <= address and address + size <= len(self.payload):
                return int.from_bytes(self.payload[address:address + size], "little")
            return sum(memory[address + index] << (8 * index) for index in range(size))
        def store(address, value):
            self.assertTrue(0x300000 <= address <= 0x3000fc)
            for index in range(4):
                memory[address + index] = (value >> (8 * index)) & 255
        for _ in range(256):
            pc = regs[15]
            if pc == 0xfffffff0:
                return {"writes": writes, "return_value": regs[0]}
            self.assertTrue(0x1918 <= pc < 0x1ad8)
            word = read(pc, 4)
            condition = word >> 28
            self.assertIn(condition, (0, 1, 14))
            regs[15] = pc + 4
            if (condition == 0 and not zero) or (condition == 1 and zero):
                continue
            reg = lambda index: pc + 8 if index == 15 else regs[index]
            def shifted():
                value, kind = reg(word & 15), (word >> 5) & 3
                self.assertIn(kind, (0, 1))
                by_register = bool(word & 16)
                amount = (reg((word >> 8) & 15) & 255) if by_register else (word >> 7) & 31
                if kind == 1 and not by_register and amount == 0:
                    amount = 32
                if amount >= 32:
                    return 0
                return ((value << amount) & 0xffffffff) if kind == 0 else value >> amount
            if word & 0x0e000000 == 0x0a000000:
                displacement = word & 0xffffff
                if displacement & 0x800000:
                    displacement -= 1 << 24
                target = pc + 8 + displacement * 4
                if word & 0x1000000:
                    self.assertIn(target, (0x203c4, 0x1e8e8))
                    if target == 0x1e8e8:
                        self.assertEqual(regs[0], 0x200000)
                        writes.append([regs[1], regs[2]])
                    # Model BL's LR update and poison caller-saved registers;
                    # the address model cannot rely on their call preservation.
                    regs[:4] = [0xd0, 0xd1, 0xd2, 0xd3]
                    regs[12], regs[14] = 0xdc, pc + 4
                else:
                    regs[15] = target
            elif word == 0xe1cd20d8:
                regs[2], regs[3] = read(regs[13] + 8, 4), read(regs[13] + 12, 4)
            elif word & 0x0fc000f0 == 0x00000090:
                self.assertFalse(word & (1 << 21))
                regs[(word >> 16) & 15] = (reg(word & 15) * reg((word >> 8) & 15)) & 0xffffffff
            elif word & 0x0e000000 == 0x08000000:
                selected = [index for index in range(16) if word & (1 << index)]
                base_register = (word >> 16) & 15
                base, up, pre = reg(base_register), bool(word & (1 << 23)), bool(word & (1 << 24))
                address = base + (4 if pre else 0) if up else base - 4 * (len(selected) - (0 if pre else 1))
                for index in selected:
                    if word & (1 << 20):
                        regs[index] = read(address, 4)
                    else:
                        store(address, reg(index))
                    address += 4
                if word & (1 << 21):
                    regs[base_register] = (base + (4 if up else -4) * len(selected)) & 0xffffffff
            elif word & 0x0c000000 == 0x04000000:
                base_register, destination = (word >> 16) & 15, (word >> 12) & 15
                base = reg(base_register)
                displacement = shifted() if word & (1 << 25) else word & 0xfff
                target = (base + (displacement if word & (1 << 23) else -displacement)) & 0xffffffff
                address = target if word & (1 << 24) else base
                if word & (1 << 20):
                    regs[destination] = read(address, 1 if word & (1 << 22) else 4)
                else:
                    self.assertFalse(word & (1 << 22))
                    store(address, reg(destination))
                if word & (1 << 21) or not word & (1 << 24):
                    regs[base_register] = target
            else:
                self.assertEqual(word & 0x0c000000, 0)
                opcode, destination = (word >> 21) & 15, (word >> 12) & 15
                left = reg((word >> 16) & 15)
                if word & (1 << 25):
                    rotation, value = ((word >> 8) & 15) * 2, word & 255
                    right = ((value >> rotation) | (value << ((32 - rotation) % 32))) & 0xffffffff
                else:
                    right = shifted()
                self.assertIn(opcode, (0, 2, 4, 10, 12, 13, 14))
                values = {0: left & right, 2: left - right, 4: left + right,
                          10: left - right, 12: left | right, 13: right, 14: left & ~right}
                value = values[opcode] & 0xffffffff
                if word & (1 << 20):
                    zero = value == 0
                if opcode != 10:
                    regs[destination] = value
        self.fail("selected A32 helper did not return within its fixed step budget")

    def test_schema_pins_handoff_and_unverified_contracts(self):
        report = MAP._mfd_source_map(self.payload)
        validation = report["validation"]
        self.assertEqual((validation["region_count"], validation["bytes"]), (9, 1164))
        self.assertEqual((MAP.MAX_MFD_SOURCE_REGIONS, MAP.MAX_MFD_SOURCE_BYTES), (10, 1280))
        self.assertEqual({region["role"]: (region["blob_file_offset"], region["bytes"])
                          for region in validation["regions"]},
                         {"source_address": (0x1918, 448), "mfd_setup": (0x1bfc, 248),
                          "source_record_producer": (0xe110, 312), "selected_picture_call": (0x85ec, 36),
                          "mfd_setup_call": (0x84cc, 20), "source_table_literals": (0x1b28, 8),
                          "source_register_literals": (0x1b48, 44), "source_table": (0x2cccc, 36),
                          "register_write": (0x1e8e8, 12)})
        self.assertFalse(report["device_observed"])
        self.assertFalse(report["selected_handoff"]["register_context_physical_base_verified"])
        self.assertFalse(report["addressing"]["table_selector_checked_by_firmware"])
        self.assertTrue(report["addressing"]["writes_are_context_relative"])
        self.assertEqual(report["addressing"]["selected_table_rows"], [list(row) for row in self.rows])
        self.assertEqual(report["source_record_to_picture"]["final_copies"],
                         [{"load_blob_file_offset": 0xe19c, "record_word_offset": 4,
                           "store_blob_file_offset": 0xe1a0, "picture_word_offset": 0x34},
                          {"load_blob_file_offset": 0xe1a4, "record_word_offset": 8,
                           "store_blob_file_offset": 0xe1a8, "picture_word_offset": 0x38}])
        self.assertIn("not a complete frame layout", " ".join(report["limitations"]))
        self.assertIn("ownership", " ".join(report["limitations"]))
        offsets = report["addressing"]["field_offsets"]
        self.assertEqual((offsets["offset_6c_word"], offsets["offset_70_word"]), (0x6c, 0x70))
        self.assertNotIn("horizontal_word", offsets)
        self.assertNotIn("vertical_word", offsets)
        self.assertIn("offset_6c=offset_70=0",
                      report["addressing"]["conditional_zero_offset_identity"]["conditions"])
        for example in report["addressing"]["model_examples"]:
            self.assertEqual((example["offset_6c"], example["offset_70"]), (3, 5))
            self.assertNotIn("horizontal", example)
            self.assertNotIn("vertical", example)
            record = self.record(selector=example["selector"],
                                 offset_6c=example["offset_6c"], offset_70=example["offset_70"])
            self.assertEqual(example["conditional_model"], self.execute_address_helper(record))
        self.assertEqual(json.loads(json.dumps(report)), report)

    def test_independent_producer_operands_and_call_handoff(self):
        word = lambda offset: struct.unpack_from("<I", self.payload, offset)[0]
        # Derive byte offsets/register operands separately from report strings.
        for load, store, source_offset, picture_offset in ((0xe19c, 0xe1a0, 4, 0x34),
                                                         (0xe1a4, 0xe1a8, 8, 0x38)):
            self.assertEqual(word(load) & 0xfff, source_offset)
            self.assertEqual((word(load) >> 16) & 15, 5)
            self.assertEqual((word(store) >> 16) & 15, 4)
            self.assertEqual(word(store) & 0xfff, picture_offset)
            self.assertEqual((word(load) >> 12) & 15, (word(store) >> 12) & 15)
        self.assertEqual([word(offset) for offset in (0xe114, 0xe118, 0xe11c)],
                         [0xe1a06000, 0xe1a05001, 0xe1a04002])
        self.assertEqual((word(0xe120), word(0xe124), word(0xe1ac), word(0xe244)),
                         (0xe3550000, 0x0a000021, 0xea000024, 0xe8bd8070))
        self.assertEqual([word(offset) for offset in (0x85ec, 0x85f0, 0x1c08, 0x1cd8, 0x1cdc, 0x1ce4)],
                         [0xe59d1014, 0xe28d2020, 0xe1a04003, 0xe1a01004, 0xe1a00006, 0xe1a01004])
        self.assertEqual([word(offset) for offset in (0x1e8e8, 0x1e8ec, 0x1e8f0)],
                         [0xe5903000, 0xe7832001, 0xe12fff1e])

    def test_instruction_model_modes_formats_odd_offsets_and_wrap(self):
        pairs = ((0, 0), (1, 1), (3, 5), (0x04000002, 0xfffffffd),
                 (0xffffffff, 0xffffffff), (0x80000001, 0x10003))
        total = 0
        for selector in range(3):
            for mode in (0, 1, 2, 255):
                for form in (1, 2, 3):
                    for field in (0, 1, 2):
                        for offset_6c, offset_70 in pairs:
                            record = self.record(selector, mode, form, field, offset_6c, offset_70,
                                                 0xfffffffc, 0xfffffff8, 0xffffffff, 0x80000001)
                            with self.subTest(selector=selector, mode=mode, form=form, field=field,
                                              offset_6c=offset_6c, offset_70=offset_70):
                                self.assertEqual(MAP._mfd_source_model(record, self.rows),
                                                 self.execute_address_helper(record))
                            total += 1
        self.assertEqual(total, 648)

    def test_zero_offset_identity_requires_its_conditions(self):
        for selector, (_, stripe, _) in enumerate(self.rows):
            for form in (1, 2):
                base = self.record(selector=selector, form=form)
                normal = MAP._mfd_source_model(base, self.rows)
                self.assertEqual(normal["writes"][-2:], [[0x54001c, 0x1000], [0x540020, 0x8000]])
                base[8] = 1
                shifted = MAP._mfd_source_model(base, self.rows)
                self.assertEqual(shifted["writes"][-2:], [[0x54001c, 0x1000 + stripe],
                                                        [0x540020, 0x8000 + stripe]])
                base[0x27] = 3
                early = MAP._mfd_source_model(base, self.rows)
                self.assertEqual(early["return_value"], 8)
                self.assertEqual(len(early["writes"]), 3)
                self.assertTrue(all(address not in (0x54001c, 0x540020) for address, _ in early["writes"]))

    def test_private_model_rejects_outside_selected_contract(self):
        for length in (0, 115, 117, 140):
            with self.subTest(length=length), self.assertRaises(MAP.FormatError):
                MAP._mfd_source_model(bytes(length), self.rows)
        for selector, form in ((3, 1), (255, 2), (0, 0), (0, 4), (0, 255)):
            with self.subTest(selector=selector, form=form), self.assertRaises(MAP.FormatError):
                MAP._mfd_source_model(self.record(selector=selector, form=form), self.rows)
        for rows in ((), self.rows[:2], ((0, 64, 5), *self.rows[1:])):
            with self.subTest(rows=rows), self.assertRaises(MAP.FormatError):
                MAP._mfd_source_model(self.record(), rows)

    def test_every_region_byte_rejects_mutation(self):
        for role, offset, raw in MAP._MFD_SOURCE_REGIONS:
            for index in range(len(bytes.fromhex(raw))):
                changed = bytearray(self.payload)
                changed[offset + index] ^= 1
                with self.subTest(role=role, offset=offset + index), self.assertRaises(MAP.FormatError):
                    MAP._mfd_source_map(changed)

    def test_exact_region_byte_budget_and_payload_bounds(self):
        with mock.patch.object(MAP, "MAX_MFD_SOURCE_REGIONS", 9), \
                mock.patch.object(MAP, "MAX_MFD_SOURCE_BYTES", 1164):
            self.assertEqual(MAP._mfd_source_map(self.payload)["validation"]["bytes"], 1164)
        for field, value in (("MAX_MFD_SOURCE_REGIONS", 8), ("MAX_MFD_SOURCE_BYTES", 1163)):
            with mock.patch.object(MAP, field, value), self.assertRaisesRegex(MAP.FormatError, "budget"):
                MAP._mfd_source_map(self.payload)
        for payload in (b"", self.payload[:-4], self.payload + bytes(4)):
            with self.subTest(size=len(payload)), self.assertRaisesRegex(MAP.FormatError, "payload size"):
                MAP._mfd_source_map(payload)

    def test_all_256_prior_report_projections_are_unchanged(self):
        options = ("references", "all_symbols", "bootstrap", "picture_output",
                   "arc_metadata", "csc_command", "command_buffer_bridge", "inner_descriptor")
        aggregate = hashlib.sha256()
        expected_source = MAP._mfd_source_map(self.payload)
        for mask in range(256):
            flags = {name: bool(mask & (1 << bit)) for bit, name in enumerate(options)}
            wanted = ("ReadLine",) if mask & 1 else MAP.DEFAULT_SYMBOLS
            report = MAP.analyze(self.data, wanted, **flags)
            if flags["picture_output"]:
                self.assertEqual(report["picture_output"].pop("mfd_source"), expected_source)
            prior_inner_descriptor_projection(report)
            stdout = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
            aggregate.update(bytes([mask]))
            aggregate.update(hashlib.sha256(stdout).digest())
        # MFD-free output projection with validated current driver-source anchors.
        self.assertEqual(aggregate.hexdigest(),
                         "077e5779497ce4ef730585a42bb17585a7bddd90d9d608ad43ca32809bc3da45")

    def test_new_map_is_default_off_and_public_pin_precedes_parsing(self):
        with mock.patch.object(MAP, "_mfd_source_map", side_effect=AssertionError("unexpected source map")):
            self.assertNotIn("picture_output", MAP.analyze(MAP.read_firmware(BLOB)))
        altered = bytearray(MAP.read_firmware(BLOB))
        altered[0x1918] ^= 1
        with mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected parse")), \
                mock.patch.object(MAP, "_mfd_source_map", side_effect=AssertionError("unexpected source map")):
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(altered, expected_sha256=hashlib.sha256(altered).hexdigest(), picture_output=True)


class FirmwareStopStatusTests(unittest.TestCase):
    # Stock A32 status flow only: no live STOP, ARC execution or surface lease.
    regions = (
        (0x4288, 936, "0fa6a9603dd61c135d027f33708dbcada8df969107fe9ef5f9f7c728d205851a"),
        (0x125c, 136, "7412ad146fa32d0f03d2a11f08758cd8c29b27a0de198dffb43cfa48a9394bb6"),
        (0xef10, 172, "dac38f265061167d8a22a7b3e49e128209206062e6138f41fed094c2d0a9b601"),
        (0x27750, 140, "f95765626233c26ccf54e514bbd6c17216122da1bf71820dc6ecd45e386c9ff9"),
        (0x2705c, 364, "bd461670f479a8e1f005d75357912eee0b0b61d875c6c87c7a10f79d9303d6f8"),
        (0x27ab0, 4, "38c07ee2c1401fe213b333a1fbb4ba7d716c9d5df5df4077fbb53a3daa977748"),
    )
    statuses = (0, 1, 2, 5, 6, 7, 9, 0x0022000a, 0x00220015, 0x80000000, 0xffffffff)

    @classmethod
    def setUpClass(cls):
        cls.payload = MAP.read_firmware(BLOB)[:-MAP.TRAILER_SIZE]
        for offset, size, expected in cls.regions:
            actual = cls.payload[offset:offset + size]
            if len(actual) != size or hashlib.sha256(actual).hexdigest() != expected:
                raise AssertionError(f"stock STOP region {offset:#x} changed")

    def word(self, offset):
        return struct.unpack_from("<I", self.payload, offset)[0]

    def execute_tail(self, builder, status, mode=0):
        # Enter after a normally returning call with arbitrary r0. All data
        # and saved registers are synthetic; only the pinned instructions run.
        registers = [0] * 16
        registers[0], registers[4], registers[13] = status, 0x200000, 0x300100
        entry, end, pop = (0x277bc, 0x277dc, 0x277d0) if builder else (0xef98, 0xefbc, 0xef44)
        selected = (4, 5, 6, 7, 8, 9, 10, 15) if builder else (4, 5, 6, 15)
        saved_base = registers[13] + (512 if builder else 0)
        saved = {saved_base + index * 4: 0xfffffff0 if reg == 15 else 0xb000 + reg
                 for index, reg in enumerate(selected)}
        writes, trace, zero, pc = [], [], False, entry
        for _ in range(16):
            if pc == 0xfffffff0:
                return registers, writes, trace
            self.assertTrue(entry <= pc < end or pc == pop, hex(pc))
            word, previous = self.word(pc), pc
            trace.append(pc)
            pc += 4
            if word == 0xe5d40004:
                self.assertFalse(builder)
                self.assertEqual(registers[4], 0x200000)
                registers[0] = mode
            elif word == 0xe3500000:
                zero = registers[0] == 0
            elif word == 0xe3580000:
                zero = registers[8] == 0
            elif word & 0x0e000000 == 0x0a000000:
                condition = word >> 28
                self.assertIn(condition, (0, 1, 14))
                self.assertFalse(word & (1 << 24))
                take = condition == 14 or (condition == 0 and zero) or (condition == 1 and not zero)
                if take:
                    displacement = word & 0xffffff
                    if displacement & 0x800000:
                        displacement -= 1 << 24
                    pc = previous + 8 + displacement * 4
            elif word in (0xe3a00000, 0xe3a00002):
                registers[0] = word & 255
            elif word in (0xe5c40261, 0xe5c40060):
                self.assertFalse(builder)
                self.assertEqual(registers[4], 0x200000)
                writes.append((word & 0xfff, 1, registers[0] & 255))
            elif word == 0xe1a08000:
                registers[8] = registers[0]
            elif word == 0xe1a00008:
                registers[0] = registers[8]
            elif word == 0xe28ddc02:
                registers[13] += 512
            elif word in (0xe8bd8070, 0xe8bd87f0):
                actual = tuple(index for index in range(16) if word & (1 << index))
                self.assertEqual(actual, selected)
                self.assertNotIn(0, actual)
                for index in actual:
                    registers[index] = saved[registers[13]]
                    registers[13] += 4
                pc = registers[15]
            elif word == 0xe320f000:
                pass
            else:
                self.fail(f"unsupported STOP continuation instruction {word:#x}")
        self.fail("stock STOP continuation exceeded 16 instructions")

    def test_complete_body_pins_and_call_chain(self):
        self.assertEqual(sum(size for _, size, _ in self.regions), 1752)
        for call, target in ((0x4410, 0x125c), (0x128c, 0xef10),
                             (0xef94, 0x27750), (0x277b8, 0x2705c)):
            word = self.word(call)
            self.assertEqual(word >> 24, 0xeb)
            displacement = word & 0xffffff
            if displacement & 0x800000:
                displacement -= 1 << 24
            self.assertEqual(call + 8 + displacement * 4, target)
        self.assertEqual(self.word(0x27ab0), 0x73760006)
        self.assertEqual(self.word(0x277a0), 0xe3043e20)  # MOVW r3,20000.
        self.assertEqual(self.word(0x270e8), 0xe3590005)  # Wait timeout status.
        self.assertEqual(self.word(0x270f8), 0xe1a00009)  # Return the wait status.

    def test_builder_preserves_every_tested_transport_status(self):
        for status in self.statuses:
            with self.subTest(status=hex(status)):
                registers, writes, trace = self.execute_tail(True, status)
                self.assertEqual(registers[0], status)
                self.assertEqual(registers[13], 0x300320)
                self.assertEqual(registers[4:11], list(range(0xb004, 0xb00b)))
                self.assertEqual(writes, [])
                self.assertEqual(trace, [0x277bc, 0x277c0, 0x277c4] +
                                 ([0x277d4, 0x277d8] if status == 0 else [0x277c8]) +
                                 [0x277cc, 0x277d0])

    def test_decoder_overwrites_status_before_use_for_all_byte_modes(self):
        self.assertEqual(self.word(0xef98), 0xe5d40004)  # Unconditional overwrite of r0.
        for status in self.statuses:
            for mode in range(256):
                with self.subTest(status=hex(status), mode=mode):
                    registers, writes, trace = self.execute_tail(False, status, mode)
                    self.assertEqual(registers[0], 0)
                    self.assertEqual(registers[13], 0x300110)
                    self.assertEqual(registers[4:7], [0xb004, 0xb005, 0xb006])
                    self.assertEqual(writes, ([(0x261, 1, 2)] if mode == 0 else []) + [(0x60, 1, 0)])
                    self.assertEqual(trace, [0xef98, 0xef9c, 0xefa0] +
                                     ([0xefa4, 0xefa8] if mode == 0 else []) +
                                     [0xefac, 0xefb0, 0xefb4, 0xefb8, 0xef44])

    def test_transport_timeout_can_become_decoder_success(self):
        builder, _, _ = self.execute_tail(True, 5)
        self.assertEqual(builder[0], 5)
        for mode in (0, 1, 255):
            decoder, _, _ = self.execute_tail(False, builder[0], mode)
            self.assertEqual(decoder[0], 0)
        # This is a status-flow counterexample, not a simulated host handler:
        # callers, routing, coherence, ARC drain and source lifetime stay unproved.


class FirmwareStockHostCommandTests(unittest.TestCase):
    # Independently transcribed from the bundled A32 compare tree, computed
    # branch table and each case's BL. Do not construct this oracle with the
    # mapper's symbolic interval evaluator or its reported routes.
    routes = (
        (0x73763001, 0x60c8, 0x5ccc), (0x73763002, 0x621c, 0x5c84),
        (0x73763003, 0x6244, 0x5c3c), (0x73763004, 0x6264, 0x5ba4),
        (0x73763005, 0x628c, 0x5a50), (0x73763006, 0x62ac, 0x5a08),
        (0x73763100, 0x62cc, 0x51c8), (0x73763101, 0x6428, 0x4f88),
        (0x73763102, 0x6450, 0x3f68), (0x73763103, 0x6478, 0x4f40),
        (0x73763104, 0x6498, 0x4c7c), (0x73763105, 0x64c4, 0x4bd4),
        (0x73763106, 0x64ec, 0x4b8c), (0x73763107, 0x660c, 0x4b44),
        (0x73763108, 0x662c, 0x3f68), (0x7376310e, 0x6654, 0x4a60),
        (0x7376311a, 0x667c, 0x4630), (0x7376311b, 0x66a4, 0x4288),
        (0x7376311d, 0x66d0, 0x3f68), (0x7376311f, 0x66fc, 0x3f68),
        (0x73763121, 0x671c, 0x41bc), (0x73763123, 0x687c, 0x4174),
        (0x73763124, 0x689c, 0x3fb0), (0x73763136, 0x68c4, 0x6984),
        (0x73763144, 0x68e4, 0x3f68), (0x73763190, 0x6904, 0x3f20),
        (0x73763191, 0x6924, 0x3ed8), (0x73763192, 0x6944, 0x3e90),
        (0x73763501, 0x6964, 0x3ca8),
    )
    calls = (0x60d4, 0x6228, 0x6248, 0x6270, 0x6290, 0x62b0, 0x62d8,
             0x6434, 0x645c, 0x647c, 0x64a4, 0x64d0, 0x64f0, 0x6610,
             0x6638, 0x6660, 0x6688, 0x66b0, 0x66dc, 0x6700, 0x6728,
             0x6880, 0x68a8, 0x68c8, 0x68e8, 0x6908, 0x6928, 0x6948,
             0x6968)

    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]

    @staticmethod
    def nzcv(left, right, subtract):
        # ARM AddWithCarry arithmetic, independently using signed mathematical
        # overflow rather than interval boundaries or the mapper's flag model.
        left, right = left & 0xffffffff, right & 0xffffffff
        full = left - right if subtract else left + right
        value = full & 0xffffffff
        signed = lambda word: word if word < 0x80000000 else word - 0x100000000
        mathematical = signed(left) - signed(right) if subtract else signed(left) + signed(right)
        return value, (bool(value & 0x80000000), value == 0,
                       left >= right if subtract else full > 0xffffffff,
                       not -0x80000000 <= mathematical <= 0x7fffffff)

    def execute_selector(self, command, payload=None):
        # Test-only concrete A32 interpreter of the fixed selector, not firmware
        # execution. It follows no memory except the literal at0x6174, calls no
        # handler, and stops at the independently listed case/fallback entries.
        data = self.payload if payload is None else payload
        registers, flags, pc = [0] * 16, (False,) * 4, 0x5f78
        registers[1] = command
        exits = {case for _, case, _ in self.routes} | {0x60b8}
        for _ in range(64):
            if pc in exits:
                return pc
            self.assertTrue(0x5f78 <= pc < 0x60b8, hex(pc))
            word, = struct.unpack_from("<I", data, pc)
            condition = word >> 28
            n, z, c, v = flags
            self.assertIn(condition, (0, 1, 3, 12, 14))
            take = {0: z, 1: not z, 3: not c, 12: not z and n == v, 14: True}[condition]
            next_pc = pc + 4
            if not take:
                pc = next_pc
                continue
            reg = lambda index: pc + 8 if index == 15 else registers[index]
            if word & 0x0e000000 == 0x0a000000:
                self.assertFalse(word & (1 << 24))  # Only B, never a handler call.
                displacement = word & 0xffffff
                if displacement & 0x800000:
                    displacement -= 1 << 24
                next_pc = pc + 8 + displacement * 4
            elif word & 0x0c000000 == 0x04000000:
                self.assertEqual(word, 0xe59f21f4)
                registers[2], = struct.unpack_from("<I", data, pc + 8 + (word & 0xfff))
            else:
                self.assertEqual(word & 0x0c000000, 0)
                destination, opcode = (word >> 12) & 15, (word >> 21) & 15
                left = reg((word >> 16) & 15)
                if word & (1 << 25):
                    amount, immediate = ((word >> 8) & 15) * 2, word & 255
                    right = ((immediate >> amount) | (immediate << ((32 - amount) % 32))) & 0xffffffff
                else:
                    self.assertFalse(word & 16)
                    right, kind, amount = reg(word & 15), (word >> 5) & 3, (word >> 7) & 31
                    self.assertIn(kind, (0, 2))
                    if kind == 0:
                        right = (right << amount) & 0xffffffff
                    else:
                        self.assertNotEqual(amount, 0)
                        right = (right if right < 0x80000000 else right - 0x100000000) >> amount
                        right &= 0xffffffff
                self.assertIn(opcode, (2, 3, 4, 10, 15))
                if opcode == 15:
                    value = ~right & 0xffffffff
                    self.assertFalse(word & (1 << 20))
                else:
                    if opcode == 3:
                        left, right = right, left
                    value, new_flags = self.nzcv(left, right, opcode in (2, 3, 10))
                    if word & (1 << 20):
                        flags = new_flags
                if opcode != 10:
                    if destination == 15:
                        next_pc = value
                    else:
                        registers[destination] = value
            pc = next_pc
        self.fail("fixed stock selector exceeded independent64-step limit")

    def test_independent_firmware_route_and_branch_call_oracle(self):
        self.assertEqual(len(self.routes), 29)
        self.assertEqual(len({command for command, _, _ in self.routes}), 29)
        for (command, case, handler), call in zip(self.routes, self.calls):
            with self.subTest(command=hex(command)):
                self.assertEqual(self.execute_selector(command), case)
                word, = struct.unpack_from("<I", self.payload, call)
                self.assertEqual(word >> 24, 0xeb)
                displacement = word & 0xffffff
                if displacement & 0x800000:
                    displacement -= 1 << 24
                self.assertEqual(call + 8 + 4 * displacement, handler)
        # Unreachable slotzero is nevertheless an explicit fallback branch.
        self.assertEqual(struct.unpack_from("<I", self.payload, 0x6010)[0], 0xea000028)
        self.assertEqual(self.execute_selector(0x73763101), 0x6428)

    def test_independent_exact_add_sub_nzcv_boundary_oracle(self):
        cases = (
            (0, 0, True, 0, (False, True, True, False)),
            (0, 1, True, 0xffffffff, (True, False, False, False)),
            (0x80000000, 1, True, 0x7fffffff, (False, False, True, True)),
            (0x7fffffff, 0xffffffff, True, 0x80000000, (True, False, False, True)),
            (0xffffffff, 0xffffffff, True, 0, (False, True, True, False)),
            (0x7fffffff, 1, False, 0x80000000, (True, False, False, True)),
            (0xffffffff, 1, False, 0, (False, True, True, False)),
            (0x80000000, 0x80000000, False, 0, (False, True, True, True)),
            (0xfffffffe, 1, False, 0xffffffff, (True, False, False, False)),
            (0, 0, False, 0, (False, True, False, False)),
        )
        for left, right, subtract, value, flags in cases:
            with self.subTest(left=hex(left), right=hex(right), subtract=subtract):
                self.assertEqual(self.nzcv(left, right, subtract), (value, flags))

    def test_full_dispatch_matches_fixed29_routes_and_exact_domain_partition(self):
        dispatch = MAP._stock_host_command_closure(self.payload)["dispatch"]
        actual = [(r["command"], r["case_blob_file_offset"], r["handler_blob_file_offset"])
                  for r in dispatch["routes"]]
        self.assertEqual(actual, list(self.routes))
        control_handlers = {0x3ca8, 0x3fb0, 0x41bc, 0x4288, 0x4630,
                            0x4a60, 0x4c7c, 0x4f88, 0x5ccc}
        for route in dispatch["routes"]:
            handler = route["handler_blob_file_offset"]
            classification = ("compressed_input_open" if handler == 0x51c8 else
                              "version_metadata" if handler == 0x5ba4 else
                              "started_ack_only" if handler == 0x4bd4 else
                              "control" if handler in control_handlers else "ack_only")
            self.assertEqual(route["classification"], classification)
        self.assertEqual((dispatch["entry_blob_file_offset"], dispatch["domain_bits"],
                          dispatch["accepted_count"], dispatch["fallback_count"]),
                         (0x5f2c, 32, 29, (1 << 32) - 29))
        self.assertTrue(dispatch["complete_domain"])
        self.assertTrue(dispatch["disjoint_domains"])
        self.assertEqual((dispatch["table_blob_file_offset"], dispatch["table_entry_count"],
                          dispatch["zero_index_reachable"]), (0x6008, 7, False))
        # The full complement is independently built from literal expected
        # commands, not from production intervals or concrete sample coverage.
        expected, cursor = [], 0
        for command, _, _ in self.routes:
            if cursor < command:
                expected.append([cursor, command - 1])
            cursor = command + 1
        expected.append([cursor, 0xffffffff])
        self.assertEqual(dispatch["fallback_domains"], expected)
        segments = sorted([(low, high) for low, high in dispatch["fallback_domains"]] +
                          [(command, command) for command, _, _ in self.routes])
        self.assertEqual(sum(high - low + 1 for low, high in segments), 1 << 32)
        self.assertEqual((segments[0][0], segments[-1][1]), (0, 0xffffffff))
        self.assertTrue(all(left[1] + 1 == right[0] for left, right in zip(segments, segments[1:])))

    def test_concrete_nzcv_selector_boundary_signed_overflow_and_wrap_probes(self):
        dispatch = MAP._stock_host_command_closure(self.payload)["dispatch"]
        expected = {command: case for command, case, _ in self.routes}
        probes = {0, 1, 2, 3, 4, 0x7ffffffe, 0x7fffffff, 0x80000000,
                  0x80000001, 0xfffffffc, 0xfffffffd, 0xfffffffe, 0xffffffff}
        boundaries = {command for command, _, _ in self.routes} | {0x73763000, 0x73763108}
        for boundary in boundaries:
            for delta in (-9, -8, -7, -3, -2, -1, 0, 1, 2, 3, 6, 7, 8, 9):
                probes.add((boundary + delta) & 0xffffffff)
            # Signed compare overflow boundaries and modular arithmetic aliases.
            for delta in (-1, 0, 1):
                probes.add((boundary + 0x80000000 + delta) & 0xffffffff)
            for bit in range(32):
                probes.add(boundary ^ (1 << bit))
        # Dense low/high wraps supplement, rather than replace, the exhaustive
        # production interval-domain closure above.
        probes.update(range(256))
        probes.update(range(0xffffff00, 0x100000000))
        for command in sorted(probes):
            with self.subTest(command=hex(command)):
                case = self.execute_selector(command)
                self.assertEqual(case, expected.get(command, 0x60b8))
                domains = sum(low <= command <= high for low, high in dispatch["fallback_domains"])
                self.assertEqual(domains, int(case == 0x60b8))
        self.assertGreater(len(probes), 850)

    def test_private_selector_signed_rhs_negative_one_and_modular_base_boundaries(self):
        # The public contract rejects these literal mutations at its pin gate.
        # Direct evaluator tests exercise supported arithmetic independently,
        # especially CMP GT's rhs=-1 split at zero and signed wrap boundaries.
        for base in (0, 1, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff):
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, 0x6174, base)
            with self.subTest(base=hex(base)):
                dispatch = MAP._stock_host_dispatch_domains(changed)
                cases = {route["command"]: route["case_blob_file_offset"] for route in dispatch["routes"]}
                segments = sorted([(low, high) for low, high in dispatch["fallback_domains"]] +
                                  [(command, command) for command in cases])
                self.assertEqual((segments[0][0], segments[-1][1]), (0, 0xffffffff))
                self.assertEqual(sum(high - low + 1 for low, high in segments), 1 << 32)
                self.assertTrue(all(left[1] + 1 == right[0] for left, right in zip(segments, segments[1:])))
                probes = {0, 1, 2, 0x7ffffffe, 0x7fffffff, 0x80000000,
                          0x80000001, 0xfffffffd, 0xfffffffe, 0xffffffff}
                for boundary in set(cases) | {base, (base - 7) & 0xffffffff,
                                               (base - 264) & 0xffffffff}:
                    for delta in (-3, -2, -1, 0, 1, 2, 3):
                        probes.add((boundary + delta) & 0xffffffff)
                        probes.add((boundary + 0x80000000 + delta) & 0xffffffff)
                for command in probes:
                    with self.subTest(command=hex(command)):
                        case = self.execute_selector(command, changed)
                        self.assertEqual(case, cases.get(command, 0x60b8))
                        self.assertEqual(sum(low <= command <= high for low, high in dispatch["fallback_domains"]),
                                         int(case == 0x60b8))

    def test_all_direct_handler_footprints_have_independent_fixed_field_oracles(self):
        report = MAP._stock_host_command_closure(self.payload)
        handlers = {handler["entry_blob_file_offset"]: handler for handler in report["handlers"]}
        self.assertEqual(set(handlers), {handler for _, _, handler in self.routes})
        # Manually audited direct LDR/STR operands after request+20/reply+276
        # aliases. These are union footprints, not promises about called bodies.
        request = {
            0x3ca8: ((4, 4), (8, 4), (12, 4)),
            0x3e90: ((4, 4),), 0x3ed8: ((4, 4),), 0x3f20: ((4, 4),),
            0x3f68: ((4, 4),), 0x3fb0: ((4, 4), (8, 4), (12, 1)),
            0x4174: ((4, 4),), 0x41bc: ((4, 4), (8, 4), (12, 4)),
            0x4288: ((4, 4), (8, 4)), 0x4630: ((4, 4), (8, 4)),
            0x4a60: ((4, 4), (8, 4), (12, 4), (16, 1)),
            0x4b44: ((4, 4),), 0x4b8c: ((4, 4),),
            0x4bd4: ((4, 4), (8, 4)), 0x4c7c: ((4, 4), (8, 4), (12, 4)),
            0x4f40: ((4, 4),), 0x4f88: ((4, 4), (8, 4)),
            0x51c8: ((4, 4), (16, 1), (32, 4), (36, 1), (56, 4)),
            0x5a08: ((4, 4),), 0x5a50: ((4, 4),), 0x5ba4: ((4, 4),),
            0x5c3c: ((4, 4),), 0x5c84: ((4, 4),), 0x5ccc: ((4, 4),),
            0x6984: ((4, 4),),
        }
        for entry, handler in handlers.items():
            with self.subTest(entry=hex(entry)):
                reads = [(read["byte_offset"], read["width"]) for read in handler["request_reads"]]
                writes = [(write["byte_offset"], write["width"]) for write in handler["reply_writes"]]
                self.assertEqual(reads, list(request[entry]))
                expected = [(4, 4), (8, 4)]
                if entry == 0x51c8:
                    expected += [(12, 4), (44, 4)]
                elif entry == 0x5ba4:
                    expected += [(12, 4), (16, 4), (20, 4)]
                self.assertEqual(writes, expected)
                self.assertEqual([write["word_index"] for write in handler["reply_writes"]],
                                 [offset // 4 for offset, _ in expected])
                self.assertTrue(handler["direct_request_reply_only"])
                if entry in (0x3fb0, 0x41bc, 0x4a60):
                    self.assertEqual(handler["callee_targets"],
                                     [0x898, {0x3fb0: 0x16dc, 0x41bc: 0x168c, 0x4a60: 0x15d8}[entry]])
                else:
                    self.assertIn(0x203c4, handler["callee_targets"])
                self.assertEqual(handler["packet_header_reads"], [])
                header = [(16, 1)] if entry in (0x3fb0, 0x41bc, 0x4288, 0x4a60, 0x4bd4, 0x4c7c) else []
                self.assertEqual([(write["byte_offset"], write["width"])
                                  for write in handler["packet_header_writes"]], header)

    @staticmethod
    def independent_clear_ranges(alignment):
        # Thumb memset's independently decoded head alignment, two STM16
        # writes per32-byte iteration, then16/8/4/2/1 tails for fixed256 bytes.
        writes, cursor, remaining = [], 0, 256
        def append(width):
            nonlocal cursor, remaining
            writes.append([cursor, width])
            cursor, remaining = cursor + width, remaining - width
        if alignment:
            head = 4 - alignment
            if head != 2:
                append(1)
            if head >= 2:
                append(2)
        while remaining >= 32:
            append(16)
            append(16)
        for width in (16, 8, 4, 2, 1):
            if remaining >= width:
                append(width)
        return writes

    def test_actual_clear_helper_exactly_zeros256_bytes_at_all_four_alignments(self):
        clear = MAP._stock_host_command_closure(self.payload)["reply_initialization"]
        self.assertEqual((clear["request_record_offset"], clear["reply_record_offset"],
                          clear["byte_count"], clear["byte_value"]), (20, 276, 256, 0))
        self.assertEqual([alignment["destination_alignment"] for alignment in clear["alignments"]],
                         list(range(4)))
        for alignment in clear["alignments"]:
            start = alignment["destination_alignment"]
            with self.subTest(alignment=start):
                self.assertEqual(alignment["write_ranges"], self.independent_clear_ranges(start))
                self.assertEqual(alignment["written_bytes"], 256)
                self.assertTrue(alignment["complete"])
                self.assertFalse(alignment["overlap"])
                # Mocked memory guards ensure there is no prefix/suffix write,
                # each byte is covered once, and zero is the full direct output.
                memory, counts = bytearray(b"\xa5" * 272), [0] * 256
                for offset, width in alignment["write_ranges"]:
                    self.assertTrue(0 <= offset and offset + width <= 256)
                    self.assertIn(width, (1, 2, 4, 8, 16))
                    self.assertEqual((start + offset) % min(width, 4), 0)
                    for index in range(offset, offset + width):
                        counts[index] += 1
                    memory[8 + offset:8 + offset + width] = bytes(width)
                self.assertEqual(counts, [1] * 256)
                self.assertEqual(memory, b"\xa5" * 8 + bytes(256) + b"\xa5" * 8)

    def test_conditional_scope_does_not_promote_compressed_tx_or_runtime_proof(self):
        report = MAP._stock_host_command_closure(self.payload)
        self.assertEqual(report["validation_scope"], {
            "full_stock_selector": True, "direct_handler_footprints": True,
            "exact_reply_clear": True, "callee_bodies": False,
            "runtime_observed": False, "source_plane_ownership": False,
            "whole_firmware_absence": False})
        self.assertTrue(report["conclusion"]["conditional"])
        self.assertFalse(report["conclusion"]["explicit_raw_source_plane_lease"])
        self.assertFalse(report["conclusion"]["compressed_tx_metadata_is_raw_plane_lease"])
        context = report["source_context"]
        self.assertEqual(context["compressed_tx_metadata_reply_word"], 11)
        self.assertEqual(context["open_reply_stores_blob_file_offsets"], [0x5914, 0x591c])
        path, line = context["driver_open_postprocessing"].rsplit(":", 1)
        self.assertEqual(path, "driver/linux/crystalhd_fleafuncs.c")
        self.assertEqual((ROOT / path).read_text().splitlines()[int(line) - 1].strip(),
                         "hw->TxBuffInfoAddr = pRsp->transportStreamCaptureAddr;")
        self.assertEqual(context["getter"], {
            "entry_blob_file_offset": 0x898, "literal_blob_file_offset": 0x6fc,
            "fixed_context_value": 0xd3a00, "incoming_arguments_read": False})
        queue = context["queue_publication"]
        self.assertFalse(queue["reply_payload_written"])
        self.assertEqual(queue["node_write_byte_offsets"], [0, 4, 8, 12])
        self.assertEqual(queue["packet_header_overlay_bytes"], 16)
        self.assertTrue(queue["request_reply_payloads_disjoint"])
        self.assertFalse(queue["whole_packet_disjoint"])
        self.assertEqual(struct.unpack_from("<I", self.payload, 0x8bb0)[0], 0xe580000c)
        self.assertTrue(report["assumptions"])
        external = [text.lower() for text in report["assumptions"]
                    if "all modeled external-store destinations" in text.lower()]
        self.assertEqual(len(external), 1)
        for requirement in ("mmio", "compressed", "request", "reply", "do not alias"):
            self.assertIn(requirement, external[0])
        scope = json.dumps((report["assumptions"], report["conclusion"], report["source_context"])).lower()
        for requirement in ("calling convention", "disjoint", "raw", "compressed", "lifecycle", "silicon"):
            self.assertIn(requirement, scope)

    def test_region_identity_sizes_hashes_and_each_word_or_thumb_halfword_pin(self):
        report = MAP._stock_host_command_closure(self.payload)
        regions = (
            ("handlers", 0x3ca8, 8836), ("dispatcher", 0x5f2c, 2648),
            ("stream_ack", 0x6984, 52), ("clear_arm", 0x206e4, 36),
            ("clear_thumb_value", 0x2c688, 16), ("clear_thumb_fill", 0x2c73c, 142),
            ("caller", 0x9048, 496), ("caller_base", 0x92fc, 4),
            ("caller_queue", 0x8c28, 4), ("caller_publication", 0x8fcc, 4),
            ("stream_null", 0x6ab8, 20), ("queue_publish", 0x8afc, 236),
            ("context_getter", 0x898, 8), ("context_getter_literal", 0x6fc, 4),
            ("start_stack_output_prefix", 0x1bf04, 40),
            ("start_stack_output_literal", 0x1bbd4, 4),
        )
        self.assertEqual((report["validation"]["region_count"], report["validation"]["bytes"]),
                         (16, 12550))
        self.assertEqual([(region["role"], region["blob_file_offset"], region["size"])
                          for region in report["validation"]["regions"]], list(regions))
        self.assertEqual([(role, offset, size) for role, offset, size, _ in MAP._STOCK_HOST_COMMAND_REGIONS],
                         list(regions))
        for region in report["validation"]["regions"]:
            offset, size = region["blob_file_offset"], region["size"]
            self.assertEqual(region["sha256"], hashlib.sha256(self.payload[offset:offset + size]).hexdigest())
        mutations = 0
        with mock.patch.object(MAP, "_stock_host_dispatch_domains",
                               side_effect=AssertionError("selector interpreted before pins")), \
                mock.patch.object(MAP, "_stock_host_handler_footprints",
                                  side_effect=AssertionError("handlers interpreted before pins")), \
                mock.patch.object(MAP, "_stock_host_reply_clear",
                                  side_effect=AssertionError("clear interpreted before pins")):
            for role, offset, size in regions:
                width = 2 if role.startswith("clear_thumb") else 4
                self.assertEqual(size % width, 0)
                for position in range(offset, offset + size, width):
                    changed = bytearray(self.payload)
                    changed[position] ^= 1
                    mutations += 1
                    with self.subTest(role=role, offset=hex(position)), self.assertRaises(MAP.FormatError):
                        MAP._stock_host_command_closure(changed)
        self.assertEqual(mutations, 3177)

    def test_targeted_new_route_table_flag_store_and_clear_mutations_fail_before_interpretation(self):
        mutations = (
            (0x6054, "<I", 0xe3500014),  # Admit currently absent base+0x14 instead of+0x13.
            (0x6008, "<I", 0xe08ff102),  # Remove CC bound on computed selector table.
            (0x6010, "<I", 0xea00018d),  # Added route in the currently unreachable slotzero.
            (0x6014, "<I", 0xea000116),  # Swap two existing table handlers.
            (0x5f88, "<I", 0x8a000027),  # Unsigned HI does not implement signed NZCV GT.
            (0x5f80, "<I", 0xe0510002),  # SUBS incorrectly replaces preceding CMP flags.
            (0x5fb4, "<I", 0xe0620e42),  # ASR28 changes the stock sign-derived bias.
            (0x5fb8, "<I", 0xe0800001),  # ADD without S loses the Z update.
            (0x60b0, "<I", 0xe0800001),
            (0x6270, "<I", 0xebfffe5d),  # Retarget GET_VERSION's actual handler.
            (0x3f88, "<I", 0xe5840100),  # Direct reply store escapes256-byte record.
            (0x3f90, "<I", 0xe584000c),  # Change the sequence reply field.
            (0x591c, "<I", 0xe5841030),  # Change OPEN's published metadata field.
            (0x5f4c, "<I", 0xe3002104),  # Clear more than the reply record.
            (0x5f50, "<I", 0xe3a01001),  # Nonzero fill.
            (0x20700, "<I", 0xfa002fdf),  # Wrong A32-to-Thumb call target.
            (0x2c754, "<H", 0xf820),  # Prefix byte becomes halfword.
            (0x2c78c, "<H", 0xe890),  # Bulk write becomes load.
            (0x2c790, "<H", 0xe8a1),  # Second bulk store uses the length as base.
            (0x2c7b0, "<H", 0xf800),  # Tail word store becomes byte store.
            (0x898, "<I", 0xe5900000),  # Getter would dereference incoming packet.
            (0x89c, "<I", 0xe1a00001),  # Getter would no longer immediately return.
            (0x6fc, "<I", 0xd3a04),  # Wrong static context getter literal.
            (0x1bf0c, "<I", 0xe1a04000),  # Output prefix would save the wrong pointer.
            (0x1bf1c, "<I", 0xebfffebf),  # Wrong selected scalar-query target.
            (0x1bf24, "<I", 0xe2050fff),  # Wrong shifted mask would not prove low8 scalar.
            (0x1bf28, "<I", 0xe5c40000),  # Byte store leaves stale packet-pointer bytes.
            (0x1bbd4, "<I", 0x20b00c),  # Wrong fixed query-register literal.
        )
        with mock.patch.object(MAP, "_stock_host_dispatch_domains",
                               side_effect=AssertionError("unexpected selector interpretation")), \
                mock.patch.object(MAP, "_stock_host_handler_footprints",
                                  side_effect=AssertionError("unexpected handler interpretation")), \
                mock.patch.object(MAP, "_stock_host_reply_clear",
                                  side_effect=AssertionError("unexpected clear interpretation")):
            for offset, format_string, value in mutations:
                changed = bytearray(self.payload)
                self.assertNotEqual(struct.unpack_from(format_string, changed, offset)[0], value)
                struct.pack_into(format_string, changed, offset, value)
                with self.subTest(offset=hex(offset), value=hex(value)), self.assertRaises(MAP.FormatError):
                    MAP._stock_host_command_closure(changed)

    def test_exact_pin_budget_payload_bounds_and_no_partial_interpretation(self):
        self.assertEqual((MAP.MAX_STOCK_HOST_COMMAND_REGIONS, MAP.MAX_STOCK_HOST_COMMAND_BYTES),
                         (16, 16 * 1024))
        with mock.patch.object(MAP, "MAX_STOCK_HOST_COMMAND_REGIONS", 16), \
                mock.patch.object(MAP, "MAX_STOCK_HOST_COMMAND_BYTES", 12550):
            self.assertEqual(MAP._stock_host_command_closure(self.payload)["validation"]["bytes"], 12550)
        with mock.patch.object(MAP, "_stock_host_dispatch_domains",
                               side_effect=AssertionError("unexpected selector interpretation")), \
                mock.patch.object(MAP, "_stock_host_handler_footprints",
                                  side_effect=AssertionError("unexpected handler interpretation")), \
                mock.patch.object(MAP, "_stock_host_reply_clear",
                                  side_effect=AssertionError("unexpected clear interpretation")):
            for field, value in (("MAX_STOCK_HOST_COMMAND_REGIONS", 15),
                                 ("MAX_STOCK_HOST_COMMAND_BYTES", 12549)):
                with mock.patch.object(MAP, field, value), self.assertRaisesRegex(MAP.FormatError, "budget"):
                    MAP._stock_host_command_closure(self.payload)
            for payload in (b"", self.payload[:-1], self.payload[:-4], self.payload + bytes(4), self.data):
                with self.subTest(size=len(payload)), self.assertRaisesRegex(MAP.FormatError, "payload.*size"):
                    MAP._stock_host_command_closure(payload)
            for _, offset, size, _ in MAP._STOCK_HOST_COMMAND_REGIONS:
                with self.subTest(offset=hex(offset)), self.assertRaises(MAP.FormatError):
                    MAP._stock_host_command_closure(self.payload[:offset + size - 1])

    def test_private_models_fail_closed_on_unsupported_instruction_and_alias(self):
        # Direct model calls deliberately bypass the enclosing pin validator:
        # an unsupported model must raise, never quietly produce a full proof.
        for offset, word in ((0x5f78, 0xe3a02000), (0x5f7c, 0xffffffff),
                             (0x5f88, 0x8a000027), (0x5fb4, 0xe0620e42),
                             (0x5fc0, 0xe1500003), (0x6008, 0xe08ff102)):
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, offset, word)
            with self.subTest(model="selector", offset=hex(offset)), self.assertRaises(MAP.FormatError):
                MAP._stock_host_dispatch_domains(changed)
        for offset, word in ((0x3f84, 0xffffffff),
                             (0x3f84, 0x11a04005),  # Conditional request/reply alias merge.
                             (0x3f84, 0xe0844001),  # Opaque addition must not erase a packet alias.
                             (0x3f88, 0xe7840001),  # Variable direct reply address.
                             (0x3f88, 0xe5840100),  # Direct reply span overflow.
                             (0x3f88, 0xe5040008)):  # Direct reply negative offset.
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, offset, word)
            with self.subTest(model="handler", offset=hex(offset)), self.assertRaises(MAP.FormatError):
                MAP._stock_host_handler_footprints(changed, [0x3f68])
        for offset in (0x5f4c, 0x20700, 0x2c688, 0x2c73c, 0x2c78c, 0x2c7c8):
            changed = bytearray(self.payload)
            changed[offset] ^= 1
            with self.subTest(model="clear", offset=hex(offset)), self.assertRaises(MAP.FormatError):
                MAP._stock_host_reply_clear(changed)
        for offset, word in ((0x898, 0xe5900000), (0x89c, 0xe1a00001), (0x6fc, 0xd3a04)):
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, offset, word)
            with self.subTest(model="getter", offset=hex(offset)), self.assertRaises(MAP.FormatError):
                MAP._stock_host_handler_footprints(changed, [0x3fb0])
        for offset, word in ((0x1bf0c, 0xe1a04000), (0x1bf1c, 0xebfffebf),
                             (0x1bf24, 0xe2050fff), (0x1bf28, 0xe5c40000),
                             (0x1bbd4, 0x20b00c)):
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, offset, word)
            with self.subTest(model="stack-output-prefix", offset=hex(offset)), self.assertRaises(MAP.FormatError):
                MAP._stock_host_handler_footprints(changed, [0x4630])

    def test_independent_cfg_partition_and_clear_state_budgets_fail_closed(self):
        for field, value in (("MAX_STOCK_HOST_COMMAND_CFG_STATES", 0),
                             ("MAX_STOCK_HOST_COMMAND_CFG_STATES", 1),
                             ("MAX_STOCK_HOST_COMMAND_PARTITIONS", 0),
                             ("MAX_STOCK_HOST_COMMAND_PARTITIONS", 1)):
            with mock.patch.object(MAP, field, value), self.assertRaisesRegex(MAP.FormatError, "budget"):
                MAP._stock_host_dispatch_domains(self.payload)
        with mock.patch.object(MAP, "MAX_STOCK_HOST_COMMAND_CFG_STATES", 1), \
                self.assertRaisesRegex(MAP.FormatError, "budget"):
            MAP._stock_host_reply_clear(self.payload)
        with mock.patch.object(MAP, "MAX_STOCK_HOST_COMMAND_CFG_STATES", 1), \
                self.assertRaisesRegex(MAP.FormatError, "budget"):
            MAP._stock_host_handler_footprints(self.payload, [0x3f68])

    def test_packet_alias_spills_reload_push_movt_and_offsets_cannot_hide_ownership(self):
        # These direct model calls bypass pins to challenge the alias lattice.
        # Conditional external-memory separation does not permit a known packet
        # pointer to vanish through storage, reload, writeback or arithmetic.
        sequences = (
            ((0x3f84, 0xe58d4000),),  # STR packet-valued r4,[sp].
            ((0x3f84, 0xe58d4000), (0x3f88, 0xe59d4000)),  # Stack spill/reload into write base.
            ((0x3f88, 0xe58d4004),),  # Overwrite saved r5 with reply pointer.
            ((0x3f88, 0xe58d400c),),  # Overwrite saved LR/POP PC with reply pointer.
            ((0x3f84, 0xe5814000),),  # STR packet-valued r4,[opaque r1].
            ((0x3f84, 0xe5814000), (0x3f88, 0xe5914000)),  # External spill/reload.
            ((0x3f84, 0xe58d4000), (0x3f88, 0xe59d0000),
             (0x3f8c, 0xeb00710c)),  # Reload r0 packet then escape to opaque logger.
            ((0x3f84, 0xe92d0010),),  # PUSH packet-valued r4.
            ((0x3f68, 0xe92d4071),),  # PUSH additionally saves initial packet-valued r0.
            ((0x3f84, 0xe3404001),),  # MOVT r4,#1 must preserve may-packet taint.
            ((0x3f84, 0xe6840001),),  # STR r0,[r4],r1: offset0 then unknown writeback.
            ((0x3f84, 0xe2454014), (0x3f88, 0xe08410b2),
             (0x3f8c, 0xe1a04004)),  # Header STRH [r4],r2 unknown writeback, then reuse.
            ((0x3f84, 0xe1a04511),),  # MOV r4,r1,LSL r5: packet-valued shift amount.
        )
        for sequence in sequences:
            changed = bytearray(self.payload)
            for offset, word in sequence:
                struct.pack_into("<I", changed, offset, word)
            with self.subTest(sequence=[(hex(offset), hex(word)) for offset, word in sequence]), \
                    self.assertRaises(MAP.FormatError):
                MAP._stock_host_handler_footprints(changed, [0x3f68])

    def allocated_local_spill_payload(self):
        # A test-only A32 body, independently assembled, preserving ACK's
        # fields and caller-save restoration. Unlike overwriting a PUSH slot,
        # two explicit scratch words are reserved and balanced on every exit.
        words = (
            0xe92d4070,  # PUSH {r4,r5,r6,lr}.
            0xe24dd008,  # SUB sp,sp,#8: bounded local scratch.
            0xe2805014,  # ADD r5,r0,#20: request.
            0xe2804f45,  # ADD r4,r0,#276: reply.
            0xe58d4000,  # STR r4,[sp]: local reply-pointer spill, slot-24.
            0xe3a04000,  # MOV r4,#0: the reload must actually recover provenance.
            0xe59d4000,  # LDR r4,[sp].
            0xe3a00000,  # MOV r0,#0.
            0xe5840008,  # STR r0,[r4,#8]: status.
            0xe5950004,  # LDR r0,[r5,#4]: sequence.
            0xe5840004,  # STR r0,[r4,#4]: echoed sequence.
            0xe3a00000,  # MOV r0,#0: scalar return.
            0xe28dd008,  # ADD sp,sp,#8: release only local scratch.
            0xe8bd8070,  # POP {r4,r5,r6,pc}: exact original saved slots.
        )
        payload = bytearray(self.payload)
        struct.pack_into("<" + "I" * len(words), payload, 0x3f68, *words)
        return payload

    def test_bounded_local_packet_spill_reload_preserves_fields_and_return(self):
        changed = self.allocated_local_spill_payload()
        # The fixed stock entry still refuses this modified body before any
        # interpretation. Only the private model accepts the supported fixture.
        with self.assertRaises(MAP.FormatError):
            MAP._stock_host_command_closure(changed)
        handler, = MAP._stock_host_handler_footprints(changed, [0x3f68])
        self.assertEqual(handler["request_reads"], [{"byte_offset": 4, "width": 4}])
        self.assertEqual(handler["reply_writes"], [
            {"word_index": 1, "byte_offset": 4, "width": 4},
            {"word_index": 2, "byte_offset": 8, "width": 4}])
        self.assertEqual(handler["packet_header_reads"], [])
        self.assertEqual(handler["packet_header_writes"], [])
        self.assertEqual(handler["callee_targets"], [])
        self.assertEqual(handler["return_blob_file_offsets"], [0x3f9c])
        self.assertEqual(handler["stack_packet_spills"], [
            {"instruction_blob_file_offset": 0x3f68, "frame_byte_offset": -16,
             "packet_byte_offset": 0},
            {"instruction_blob_file_offset": 0x3f68, "frame_byte_offset": -8,
             "packet_byte_offset": 276},
            {"instruction_blob_file_offset": 0x3f78, "frame_byte_offset": -24,
             "packet_byte_offset": 276}])
        self.assertEqual(handler["validated_stack_output_prefix_calls"], [])
        self.assertTrue(handler["direct_request_reply_only"])

    def test_allocated_stack_alias_loss_escape_and_unbalanced_returns_fail_closed(self):
        # Start with a real allocated scratch slot so saved-slot protections
        # cannot accidentally satisfy these stack-provenance regressions.
        branch = lambda pc: 0xeb000000 | (((0x203c4 - pc - 8) // 4) & 0xffffff)
        sequences = (
            ((0x3f80, 0xe59d0000), (0x3f84, branch(0x3f84))),  # Reload packet into opaque BL argument.
            ((0x3f7c, 0xe08d1002), (0x3f80, 0xe5910000),
             (0x3f84, branch(0x3f84))),  # ADD r1,sp,unknown r2 must not erase stack alias.
            ((0x3f7c, 0xe1a0100d), (0x3f80, 0xe3401001),
             (0x3f84, 0xe5910000), (0x3f88, branch(0x3f88))),  # MOVT of copied stack pointer.
            ((0x3f7c, 0x11a0100d), (0x3f80, 0xe5910000),
             (0x3f84, branch(0x3f84))),  # Conditional stack/unknown join must not erase alias.
            ((0x3f7c, 0xe68d1002),),  # STR r1,[sp],r2: unbounded single-transfer writeback.
            ((0x3f7c, 0xe08d10b2),),  # STRH r1,[sp],r2: unbounded extra-transfer writeback.
            ((0x3f7c, 0xe58dd004), (0x3f80, 0xe59d1004),
             (0x3f84, 0xe5910000), (0x3f88, branch(0x3f88))),  # Spill SP and reload double indirection.
            ((0x3f7c, 0xe1a0121d),),  # MOV r1,sp,LSL r2 must not erase stack alias.
            ((0x3f98, 0xe28dd004),),  # Release only one of two scratch words.
            ((0x3f9c, 0xe8bd8071),),  # Extra restored register gives wrong return balance.
            ((0x3f9c, 0xe8bd8068),),  # Same POP width but saved-register mapping changes.
            ((0x3f9c, 0xe12fff1e),),  # BX LR with frame still allocated is not a leaf return.
        )
        for sequence in sequences:
            changed = self.allocated_local_spill_payload()
            for offset, word in sequence:
                struct.pack_into("<I", changed, offset, word)
            with self.subTest(sequence=[(hex(offset), hex(word)) for offset, word in sequence]), \
                    self.assertRaises(MAP.FormatError):
                MAP._stock_host_handler_footprints(changed, [0x3f68])

    def test_start_packet_stack_scratch_is_overwritten_by_exact_selected_prefix(self):
        # Independently decoded prefix: preserve output pointer across an
        # external-object/fixed-register query, AND the scalar result to8 bits,
        # and overwrite all4 bytes before the later local-stack consumer.
        prefix = (0xe92d40f0, 0xe1a06000, 0xe1a04001, 0xe3a07000,
                  0xe51f1348, 0xe1a00006, 0xebfffec0, 0xe1a05000,
                  0xe20500ff, 0xe5840000)
        self.assertEqual(struct.unpack_from("<10I", self.payload, 0x1bf04), prefix)
        self.assertEqual(struct.unpack_from("<I", self.payload, 0x1bbd4)[0], 0x20b008)
        report = MAP._stock_host_command_closure(self.payload)
        handler = next(item for item in report["handlers"] if item["entry_blob_file_offset"] == 0x4630)
        self.assertEqual(handler["stack_packet_spills"], [
            {"instruction_blob_file_offset": 0x4630, "frame_byte_offset": -48,
             "packet_byte_offset": 0},
            {"instruction_blob_file_offset": 0x4630, "frame_byte_offset": -32,
             "packet_byte_offset": 0},
            {"instruction_blob_file_offset": 0x4630, "frame_byte_offset": -24,
             "packet_byte_offset": 276}])
        self.assertEqual(handler["validated_stack_output_prefix_calls"], [
            {"call_blob_file_offset": 0x48d0, "frame_byte_offset": -48,
             "overwritten_bytes": 4, "value_mask": 255}])
        self.assertEqual(report["source_context"]["start_stack_output_prefix"], {
            "entry_blob_file_offset": 0x1bf04, "last_validated_blob_file_offset": 0x1bf28,
            "nested_callee_blob_file_offset": 0x1ba24,
            "literal_blob_file_offset": 0x1bbd4, "literal_value": 0x20b008,
            "stored_value_mask": 255, "post_prefix_body_validated": False})
        self.assertIn("post-prefix", " ".join(report["assumptions"]).lower())

    def test_leaf_return_preserves_original_lr_and_calls_cannot_fake_it(self):
        # A real scalar leaf can BX the untouched original LR with SP0. A BL
        # instead replaces LR with its local return PC, even for the explicitly
        # pinned zero-input getter; treating that as the caller return is wrong.
        leaf = bytearray(self.payload)
        struct.pack_into("<2I", leaf, 0x3f68, 0xe3a00000, 0xe12fff1e)
        handler, = MAP._stock_host_handler_footprints(leaf, [0x3f68])
        self.assertEqual(handler["return_blob_file_offsets"], [0x3f6c])
        self.assertEqual(handler["request_reads"], [])
        self.assertEqual(handler["reply_writes"], [])
        branch = lambda pc, target: 0xeb000000 | (((target - pc - 8) // 4) & 0xffffff)
        for words in ((branch(0x3f68, 0x898), 0xe12fff1e),
                      (0xe3a00000, branch(0x3f6c, 0x203c4), 0xe12fff1e)):
            changed = bytearray(self.payload)
            struct.pack_into("<" + "I" * len(words), changed, 0x3f68, *words)
            with self.subTest(words=[hex(word) for word in words]), self.assertRaises(MAP.FormatError):
                MAP._stock_host_handler_footprints(changed, [0x3f68])

    def test_contract_is_pure_offline_and_has_no_public_cli_option(self):
        mutable = bytearray(self.payload)
        before = bytes(mutable)
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file read")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected file/device open")), \
                mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected external command")), \
                mock.patch.object(subprocess, "Popen", side_effect=AssertionError("unexpected external process")):
            MAP._stock_host_command_closure(mutable)
        self.assertEqual(mutable, before)
        for option in ("--stock-host-command-closure", "--stock-host-command-closure=0x73763004",
                       "--stock-host-command-budget"):
            with mock.patch.object(MAP, "read_firmware", side_effect=AssertionError("unexpected file read")), \
                    mock.patch.object(sys, "stderr", new_callable=io.StringIO), \
                    self.assertRaises(SystemExit) as error:
                MAP.main(["offline.bin", option])
            self.assertEqual(error.exception.code, 2)

    def test_private_helper_does_not_change_any_of256_existing_public_reports(self):
        options = ("references", "all_symbols", "bootstrap", "picture_output",
                   "arc_metadata", "csc_command", "command_buffer_bridge", "inner_descriptor")
        aggregate = hashlib.sha256()
        with mock.patch.object(MAP, "_stock_host_command_closure",
                               side_effect=AssertionError("unexpected private contract evaluation")):
            for mask in range(256):
                flags = {name: bool(mask & (1 << bit)) for bit, name in enumerate(options)}
                wanted = ("ReadLine",) if mask & 1 else MAP.DEFAULT_SYMBOLS
                report = MAP.analyze(self.data, wanted, **flags)
                self.assertNotIn("stock_host_command_closure", report)
                prior_inner_descriptor_projection(report)
                stdout = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
                aggregate.update(bytes([mask]))
                aggregate.update(hashlib.sha256(stdout).digest())
        # Public-output snapshot excludes private helpers.
        self.assertEqual(aggregate.hexdigest(),
                         "33f775ebb6fc935321506ac22faeb48857f2e40b1ba85f114f1d77570b4348b6")


class FirmwarePpbBankContractTests(unittest.TestCase):
    # Independently checked against the outer ELF symbol/section tables and
    # GNU 2.23.2 disassembly. Addresses are ARC ELF VMAs, never ARM offsets.
    bodies = (
        ("Core_Run", 2, 0x4dc4, 0x51cc),
        ("Core_CircBuffer_Get", 2, 0x4d10, 0x4dc4),
        ("SystemCore_MonitorIL", 2, 0x41e8, 0x428c),
        ("VideoParameters", 4, 0x80dc, 0x8218),
        ("Core_OrderPIF_Release", 4, 0x903c, 0x907c),
        ("Core_DeallocatePPB", 4, 0x907c, 0x91c4),
        ("Core_OrderPIF_ReleaseOnLatest", 4, 0x91c4, 0x92a8),
        ("ChannelCore_MonitorIL", 4, 0x9850, 0x9ad8),
        ("Core_CopyDramToLsram", 4, 0x9e74, 0x9f90),
        ("System_Activate", 4, 0x9f90, 0xa0b4),
        ("Core_AttemptDecode", 4, 0xa258, 0xa844),
        ("AttemptRelease", 4, 0xab44, 0xac18),
        ("Core_AttemptIL", 4, 0xac18, 0xad90),
        ("AllocatePPB", 4, 0xad90, 0xb080),
        ("PPB_Video_Address", 4, 0xb080, 0xb0fc),
        ("Core_AttemptPPBAssignment", 4, 0xb0fc, 0xb38c),
        ("Core_CircBuffer_Put", 4, 0xb554, 0xb610),
        ("Core_AttemptDisplay", 4, 0xb610, 0xba98),
        ("Core_PPB_From_Address", 4, 0xba98, 0xbac8),
        ("_udivmod", 4, 0xbc04, 0xbd18),
        ("Platform_VideoStripeHeight", 4, 0xbd90, 0xbdc4),
        ("Platform_DeliverPicture", 4, 0xbdec, 0xbe38),
        ("Platform_UpdateReleaseQueue", 4, 0xbe38, 0xbe70),
        ("CmdInitialize", 16, 0x245ec, 0x24788),
        ("CmdChannelOpen", 16, 0x24788, 0x24bb4),
        ("CmdChannelStart", 16, 0x24dac, 0x254d4),
        ("Core_Command", 16, 0x25808, 0x25a0c),
        ("Core_SetPIF_NoDisplay", 16, 0x25f10, 0x26080),
        ("Core_ReleasePPB", 16, 0x260e4, 0x26144),
        ("Core_GetUndeliveredPPBs", 16, 0x26144, 0x26270),
        ("Core_Late_PPB_Release", 16, 0x26330, 0x263a8),
        ("PopulateEmptyPPB", 16, 0x263a8, 0x26444),
        ("Core_ChanInitialize", 16, 0x266f8, 0x26974),
        ("Core_StartChannel", 16, 0x26974, 0x26ba0),
        ("Core_StopChannel", 16, 0x26ba0, 0x26e10),
    )
    release_calls = (
        ("Core_Run", 2, 0x51a4, 0x3000, 0x519c, 0x51a8),
        ("Core_OrderPIF_ReleaseOnLatest", 4, 0x9280, 0x4000, 0x9278, 0x9284),
        ("AttemptRelease", 4, 0xabcc, 0x4000, 0xabc4, 0xabd0),
        ("Core_AttemptDisplay", 4, 0xb964, 0x3000, 0xb95c, 0xb968),
        ("Core_SetPIF_NoDisplay", 16, 0x25f90, 0x2000, 0x25f88, 0x25f94),
        ("Core_ReleasePPB", 16, 0x26130, 0x6000, 0x26124, 0x2612c),
        ("Core_GetUndeliveredPPBs", 16, 0x261d4, 0x6000, 0x261c8, 0x261d0),
        ("Core_GetUndeliveredPPBs", 16, 0x26264, 0x6000, 0x26258, 0x26260),
        ("Core_Late_PPB_Release", 16, 0x26390, 0x3000, 0x26384, 0x2638c),
    )

    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        # Independent ELF32 layout parser: no mapper parsing or body registry.
        cls.elf_base = 0x2ea60
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", cls.payload, cls.elf_base)
        cls.sections = [struct.unpack_from("<10I", cls.payload, cls.elf_base + header[6] + index * 40)
                        for index in range(header[12])]
        cls.elf_header = header

    def word(self, section, address):
        record = self.sections[section]
        self.assertTrue(record[3] <= address <= record[3] + record[5] - 4)
        offset = self.elf_base + record[4] + address - record[3]
        return struct.unpack_from("<I", self.payload, offset)[0]

    def test_independent_elf_symbols_and_per_section_body_mapping(self):
        self.assertEqual(self.elf_header[1:],
                         (2, 45, 1, 0x3a678, 52, 0x4aae0, 0, 52, 32, 18, 40, 55, 54))
        for index, vma, offset, size in ((2, 0x4000, 0x444, 0x23d4),
                                       (4, 0x7f8c, 0x43d0, 0x41cc),
                                       (16, 0x23d74, 0x17ea8, 0x17654)):
            section = self.sections[index]
            self.assertEqual((section[1], section[3], section[4], section[5]), (1, vma, offset, size))
        symbols, strings = self.sections[35], self.sections[34]
        names = self.payload[self.elf_base + strings[4]:self.elf_base + strings[4] + strings[5]]
        entries = {}
        for position in range(0, symbols[5], 16):
            name, value, size, info, _, section = struct.unpack_from(
                "<IIIBBH", self.payload, self.elf_base + symbols[4] + position)
            if info & 15 == 2:
                text = names[name:names.index(b"\0", name)].decode("ascii")
                entries.setdefault(text, []).append((value, size, section))
        for name, section, start, end in self.bodies:
            with self.subTest(function=name):
                # GNU records the assembly division entry with size zero;
                # its full selected body includes _divmod and _div0 suffixes.
                expected_size = 0 if name == "_udivmod" else end - start
                self.assertIn((start, expected_size, section), entries[name])
        self.assertEqual(sum(end - start for _, _, start, end in self.bodies), 15712)
        self.assertEqual(self.elf_base + self.sections[4][4] + 0xad90 - self.sections[4][3], 0x35c34)
        self.assertEqual(self.elf_base + self.sections[16][4] + 0x260e4 - self.sections[16][3], 0x48c78)

    def test_independent_full_function_instruction_transition_oracles(self):
        # Exact selected 32-bit ARC instruction words and long immediates. The
        # table is transcribed from disassembly, not from a production decoder.
        words = {
            4: {
                0xadbc: 0x087f0400, 0xadc0: 0x3fffceb2,  # Dynamic descriptor limit.
                0xadfc: 0x08410990, 0xae00: 0x1fe10d00,  # Sign-bit free test.
                0xae1c: 0x67e8a300, 0xae20: 0x67e79f01,
                0xae30: 0x1086fd90, 0xae34: 0xac00,  # Only BOTH dimensions zero.
                0xae64: 0x08a684cc, 0xae84: 0x08008044,
                0xae90: 0x60807c00, 0xae94: 0x7ff,
                0xae98: 0x88607e0b, 0xaea0: 0x7ff,
                0xaea4: 0x88007e16, 0xaea8: 0x60007e3f,
                0xaeb8: 0x57e07a20, 0xaec4: 0x57fe83ff,
                0xaedc: 0x801f8001, 0xaee0: 0x50007e01,
                0xaf18: 0x08208044, 0xaf20: 0x20000081,
                0xaf78: 0x080680c8, 0xaf7c: 0x0a7f0000, 0xaf80: 0x3fffd2c8,
                0xaf90: 0x2000038e, 0xafc8: 0x6029a600,
                0xafd0: 0x57e00300, 0xafd8: 0x6000820a,  # SIGNED GE clamp, not unsigned min.
                0xafd4: 0x81e7fe0b, 0xafdc: 0x6827a200,  # Unmasked geometry OR.
                0xafe0: 0x80007e16, 0xafe4: 0x68000200,
                0xaff0: 0x10018044, 0xaff8: 0x1001843c, 0xb000: 0x1001a640,
                0xb018: 0x803f8401, 0xb034: 0x80417e04, 0xb040: 0x1001803c,
                0xb048: 0x68477c00, 0xb04c: 0xe800, 0xb050: 0x10868590,
                0xb080: 0x40000000, 0xb090: 0x08200990,
                0xb094: 0x67e0fd00, 0xb098: 0xe000,  # ANY E000 bit, no special gate.
                0xb0a8: 0x6000fe0f, 0xb0b4: 0x6060fe1f,
                0xb0c0: 0x801f8601, 0xb0cc: 0x08010038, 0xb0d0: 0x08210040,
                0xb0d4: 0x679ffe20, 0xb0e0: 0x18618500,
                0xb0e4: 0x40410205, 0xb0e8: 0x40208302, 0xb0f4: 0x40010000,
                0x90bc: 0x67e87d00, 0x90c0: 0x6000, 0x90cc: 0x1fe80d00,
                0x90f8: 0x62487c00, 0x90fc: 0x400,
                0x9180: 0x801f8001, 0x9184: 0x70008100, 0x918c: 0x1001003c,
                0x9190: 0x10011e44, 0x9194: 0x10011e40, 0x9198: 0x10869f90,
                0xb14c: 0x67e77a01, 0xb158: 0x87e77a16,
                0xb1b0: 0x87e77a12, 0xb210: 0x87e77a15,
                0xb2f4: 0x87e77a11, 0xb314: 0x60007c00, 0xb318: 0xffffdfff,
                0xb678: 0x67e8fd00, 0xb67c: 0x600,
                0xb704: 0x87e8fa14, 0xb878: 0x87e87a17, 0xb87c: 0x200006a4,
                0xb944: 0x1fe88d00, 0xb99c: 0x68007c00, 0xb9a0: 0x1000,
                0xbc0c: 0x20000402, 0xbc10: 0x20001f00,
                0xbd0c: 0x181f85ff, 0xbd14: 0x603ffe00,  # div0 returns 7fffffff/remainder0.
                0xbd90: 0x807f8201, 0xbd98: 0x40010000, 0xbda8: 0x40018001,
                0xbdac: 0x57e07d00, 0xbdb0: 0x460,
                0xacb0: 0x2fffd220, 0xad18: 0x0800c0d0, 0xad40: 0x14000200,
                0x9a8c: 0x60007c00, 0x9a90: 0xdbff,  # Separate completion PIF clearing.
            },
            16: {
                0x260f4: 0x200001a4, 0x260fc: 0x57e0fa22, 0x26100: 0x2000020b,
                0x26124: 0x60007c00, 0x26128: 0xffff9fff,
                0x26890: 0x6001040d, 0x26898: 0x3000030d,
                0x2689c: 0x10002dfc, 0x268a0: 0x42cb2400,
                0x268a4: 0x10000600, 0x268a8: 0x10000604, 0x268ac: 0x10000608,
                0x268b4: 0x679ffe22, 0x268c0: 0x10820754,
                0x2487c: 0x57e07a09, 0x24880: 0x2000028e,
                0x246a0: 0x40007e05, 0x246a4: 0x1046801a,
                0x24674: 0x601f7c01, 0x24678: 0x7f, 0x2468c: 0x601ffe3f,
                0x24698: 0x1046841b,
                0x26a0c: 0x679ffe22, 0x26a18: 0x60007c00, 0x26a1c: 0xfffff7ff,
                0x26ca0: 0x1fe10d00, 0x26ca4: 0x87e17d04, 0x26ca8: 0x14,
                0x26cb0: 0x60217c00, 0x26cb4: 0xffffbfff,
                0x26cb8: 0x87e17a13, 0x26cc0: 0x2ffe8423,
                0x26420: 0x68007c00, 0x26424: 0x100,
            },
        }
        for section, expected in words.items():
            for address, word in expected.items():
                with self.subTest(section=section, address=hex(address)):
                    self.assertEqual(self.word(section, address), word)
        # The .jd taken-only error delay must not overwrite a valid returned ID.
        self.assertEqual(self.word(2, 0x5184), 0x2000054b)
        self.assertEqual(self.word(2, 0x5188), 0x601ffe13)

    def independent_regions(self):
        metadata = (
            ("elf_header", 0x2ea60, 52),
            ("section_headers", 0x79540, 2200),
            ("section_names", 0x79098, 1191),
            ("symbol_table", 0x69b70, 13392),
            ("symbol_names", 0x67a95, 8410),
            ("slice_relocations", 0x6d020, 1008),
            ("picture_relocations", 0x6da34, 3132),
            ("text_relocations", 0x72780, 26052),
            ("vendor_extension_declarations", 0x67a25, 112),
        )
        return metadata + tuple(
            (name, self.elf_base + self.sections[section][4] + start - self.sections[section][3], end - start)
            for name, section, start, end in self.bodies)

    def test_independent_all_region_manifest_sizes_hashes_and_relocation_headers(self):
        expected = self.independent_regions()
        self.assertEqual(len(expected), 44)
        self.assertEqual(sum(size for _, _, size in expected), 71261)
        self.assertEqual([(role, offset, size) for role, offset, size, _ in MAP._PPB_BANK_REGIONS],
                         list(expected))
        self.assertEqual([(name, section, start, end) for name, section, start, end, _, _
                          in MAP._PPB_BANK_BODIES], list(self.bodies))
        for role, offset, size, digest in MAP._PPB_BANK_REGIONS:
            with self.subTest(role=role):
                self.assertEqual(hashlib.sha256(self.payload[offset:offset + size]).hexdigest(), digest)
        # Original RELA tables contain 2516 entries. They are not applied to
        # the in-memory byte model, and cannot establish runtime relocation.
        for index, target, count in ((37, 2, 84), (39, 4, 261), (51, 16, 2171)):
            section = self.sections[index]
            self.assertEqual((section[1], section[5], section[6], section[7], section[9]),
                             (4, count * 12, 35, target, 12))
        self.assertEqual(sum(self.sections[index][5] // 12 for index in (37, 39, 51)), 2516)
        self.assertEqual((self.sections[33][4], self.sections[33][5]), (0x38fc5, 112))
        self.assertEqual((MAP.MAX_PPB_BANK_REGIONS, MAP.MAX_PPB_BANK_BYTES,
                          MAP.MAX_PPB_BANK_RELOCATIONS, MAP.MAX_PPB_BANK_MODEL_STEPS),
                         (64, 80 * 1024, 2516, 4096))

    def test_all_nine_reference_drop_call_sites_and_delay_slot_store_oracles(self):
        call_words = (0x2807daa0, 0x2fffbf20, 0x2ffc95a0, 0x2ffae2a0,
                      0x2fc61d20, 0x2fc5e920, 0x2fc5d4a0, 0x2fc5c2a0, 0x2fc59d20)
        for index, (name, section, call, clear, and_site, store) in enumerate(self.release_calls):
            with self.subTest(function=name, call=hex(call)):
                self.assertEqual(self.word(section, and_site), 0x6020fc00 if index < 5 else 0x60007c00)
                self.assertEqual(self.word(section, and_site + 4), ~clear & 0xffffffff)
                self.assertEqual(self.word(section, call), call_words[index])
                self.assertEqual(self.word(section, store),
                                 0x10810390 if index < 5 else 0x10808190 if index == 8 else 0x10810190)
                # Ordinary .d executes the field store before entering the
                # callee; the two explicit pre-call stores are also included.
                self.assertIn(store - call, (-4, 4))

    @staticmethod
    def raw_capacity(dividend, divisor):
        # Independent mathematical division followed by the disassembled
        # SIGNED GE conditional move. Not Python min(), and not silicon proof.
        quotient, remainder = divmod(dividend, divisor) if divisor else (0x7fffffff, 0)
        signed = quotient if quotient < 0x80000000 else quotient - 0x100000000
        return quotient, remainder, 32 if signed >= 32 else quotient

    @staticmethod
    def packed_geometry(width, height, capacity):
        return (width | (height << 11) | (capacity << 22)) & 0xffffffff

    def test_unsigned_division_signed_capacity_clamp_and_zero_divisor(self):
        for dividend in (0, 1, 31, 32, 33, 63, 64, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff):
            for divisor in (0, 1, 2, 3, 31, 32, 33, 0x7fffffff, 0x80000000, 0xffffffff):
                quotient, remainder, capacity = self.raw_capacity(dividend, divisor)
                with self.subTest(dividend=dividend, divisor=divisor):
                    self.assertEqual(MAP._ppb_bank_capacity(dividend, divisor), {
                        "quotient": quotient, "remainder": remainder, "capacity": capacity,
                        "division_by_zero": divisor == 0})
        self.assertEqual(MAP._ppb_bank_capacity(0x80000000, 1)["capacity"], 0x80000000)
        self.assertEqual(MAP._ppb_bank_capacity(0xffffffff, 1)["capacity"], 0xffffffff)
        self.assertEqual(MAP._ppb_bank_capacity(0, 0)["capacity"], 32)
        for value in (-1, 0x100000000, True, 1.0, None):
            with self.subTest(value=value), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_capacity(value, 1)

    def test_original_relocation_receipts_are_independently_resolved_not_runtime_applied(self):
        mappings, actual, examined = MAP._ppb_bank_elf_context(self.payload)
        self.assertEqual([(entry["section_index"], entry["blob_minus_elf_address"]) for entry in mappings],
                         [(2, 0x2aea4), (4, 0x2aea4), (16, 0x22b94)])
        names_section, symbols_section = self.sections[34], self.sections[35]
        names = self.payload[self.elf_base + names_section[4]:self.elf_base + names_section[4] + names_section[5]]
        symbols = [struct.unpack_from("<IIIBBH", self.payload, self.elf_base + symbols_section[4] + position)
                   for position in range(0, symbols_section[5], 16)]
        expected = []
        for relocation_index in (37, 39, 51):
            relocation_section = self.sections[relocation_index]
            owner_section = relocation_section[7]
            offset = self.elf_base + relocation_section[4]
            for position in range(offset, offset + relocation_section[5], 12):
                address, info, addend = struct.unpack_from("<IIi", self.payload, position)
                owners = [name for name, section, start, end in self.bodies
                          if section == owner_section and start <= address < end]
                if not owners:
                    continue
                self.assertEqual(len(owners), 1)
                name, value, _, _, _, target_section = symbols[info >> 8]
                text = names[name:names.index(b"\0", name)].decode("ascii")
                target = value + addend
                target_offset = None
                if 0 <= target_section < len(self.sections):
                    record = self.sections[target_section]
                    if record[1] != 8 and record[3] <= target < record[3] + record[5]:
                        target_offset = self.elf_base + record[4] + target - record[3]
                expected.append((owners[0], address, position, info & 255, info >> 8,
                                 text, value, addend, target & 0xffffffff, target_section, target_offset))
        self.assertEqual((examined, len(expected)), (2516, 315))
        self.assertEqual([(entry["owner"], entry["elf_virtual_address"],
                           entry["relocation_record_blob_file_offset"], entry["type"],
                           entry["symbol_index"], entry["symbol"], entry["symbol_elf_value"],
                           entry["addend"], entry["original_target_elf_value"],
                           entry["target_section_index"], entry["original_target_blob_file_offset"])
                          for entry in actual], expected)
        self.assertTrue(all(entry["runtime_application_validated"] is False for entry in actual))
        self.assertTrue(all(entry["original_target_blob_file_offset"] is None for entry in actual
                            if entry["target_section_index"] in (21, 31)))
        with mock.patch.object(MAP, "MAX_PPB_BANK_RELOCATIONS", 2515), \
                self.assertRaisesRegex(MAP.FormatError, "budget"):
            MAP._ppb_bank_elf_context(self.payload)
        mutations = ((0x2ea60 + 18, "<H", 93),  # Different ELF machine.
                     (0x79540 + 4 * 40 + 12, "<I", 0x4000),  # Pretend global section delta.
                     (0x79540 + 39 * 40 + 28, "<I", 2),  # Wrong relocation owner.
                     (0x6d020 + 4, "<I", 255),  # Unsupported relocation kind.
                     (0x6d020 + 4, "<I", (837 << 8) | 4))  # Symbol index outside 837 records.
        for offset, format_string, value in mutations:
            changed = bytearray(self.payload)
            struct.pack_into(format_string, changed, offset, value)
            with self.subTest(offset=hex(offset)), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_elf_context(changed)

    @staticmethod
    def geometry_oracle(width, height, exponent, mask, extra):
        # Independent ceiling-division formulation of the exact stripe code.
        # Wrap occurs at each recorded ADD/ASL, not only on the final result.
        unit = 2 ** exponent
        signed_height = height - 2 ** 32 if height >= 2 ** 31 else height
        def stripe(value):
            stripes = ((value + unit - 1) % (2 ** 32)) // unit
            if stripes % 2 == 0:
                stripes += 1
            return min((stripes * unit) % (2 ** 32), 1120)
        y_height, c_height = stripe(height), stripe((signed_height // 2) % (2 ** 32))
        pitch = ((width + mask) % (2 ** 32)) & (0xffffffff ^ mask)
        if pitch >= 32768:
            return None
        page = lambda value: ((value + 4095) % (2 ** 32)) // 4096 * 4096
        y_bytes, c_bytes = page(pitch * y_height), pitch * c_height
        total = page(y_bytes + c_bytes)
        offset = total if extra else 0
        signed_width = width - 2 ** 32 if width >= 2 ** 31 else width
        extra_bytes = (6 * (signed_width // 16) * (signed_height // 16)) % (2 ** 32) if extra else 0
        if extra:
            total = page(total + extra_bytes)
        return {"pitch": pitch, "y_stripe_height": y_height, "chroma_stripe_height": c_height,
                "y_bytes": y_bytes, "chroma_bytes": c_bytes, "extra_offset": offset,
                "extra_bytes": extra_bytes, "frame_bytes": total, "conditional_vendor_mul16": True}

    def test_independent_stripe_alignment_frame_and_metadata_extent_equations(self):
        for width in (0, 1, 16, 63, 64, 127, 128, 640, 2047, 2048, 32704, 32705, 0xfffffff0, 0xffffffff):
            for height in (0, 1, 16, 31, 32, 480, 1120, 1121, 2047, 2048, 0x80000000, 0xffffffff):
                for exponent in (0, 1, 5, 6, 7, 10, 11, 31):
                    for mask in (0, 63, 127, 255):
                        for extra in (False, True):
                            expected = self.geometry_oracle(width, height, exponent, mask, extra)
                            with self.subTest(width=width, height=height, exponent=exponent, mask=mask, extra=extra):
                                if expected is None:
                                    with self.assertRaises(MAP.FormatError):
                                        MAP._ppb_bank_geometry(width, height, exponent, mask, extra)
                                else:
                                    self.assertEqual(MAP._ppb_bank_geometry(width, height, exponent, mask, extra), expected)
        ordinary = MAP._ppb_bank_geometry(640, 480, 5, 63)
        self.assertEqual((ordinary["pitch"], ordinary["y_stripe_height"], ordinary["chroma_stripe_height"],
                          ordinary["frame_bytes"]), (640, 480, 288, 0x78000))
        self.assertEqual(MAP._ppb_bank_geometry(0, 480, 5, 63)["frame_bytes"], 0)
        self.assertEqual(MAP._ppb_bank_geometry(640, 0, 5, 63)["frame_bytes"], 0xa000)
        for exponent in (-1, 32, 255, True):
            with self.subTest(exponent=exponent), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_geometry(640, 480, exponent, 63)
        for mask, extra in ((-1, False), (256, False), (True, False), (63, 1)):
            with self.subTest(mask=mask, extra=extra), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_geometry(640, 480, 5, mask, extra)

    def test_constructor_projection_initializes_all_storage_without_implied_extent(self):
        state = MAP._ppb_bank_state([0x100000, 0x200000], 0x100000)
        self.assertEqual(state["flags"], [0] * 34)
        self.assertEqual(state["banks"], [
            {"base": 0x100000, "mask": 0, "stride": 0, "geometry": 0},
            {"base": 0x200000, "mask": 0, "stride": 0, "geometry": 0}])
        self.assertEqual((state["descriptor_limit"], state["stripe_exponent"], state["alignment_mask"],
                          state["metadata_extra"], state["recent_ppb"]), (34, 5, 63, False, 99))
        for key, value in (("frame_flags", [0] * 63), ("assigned", [99] * 63),
                           ("frame_word124", [0] * 63), ("ppb_frames", [0] * 34)):
            self.assertEqual(state[key], value)
        self.assertEqual(state["release_request"], {"head": 0, "tail": 0, "slots": [0] * 64})
        for key in ("delivery_ring", "return_ring"):
            self.assertEqual(state[key], {"read": 2, "write": 2, "slots": [0] * 64})
        copied = MAP._ppb_bank_copy_state(state)
        self.assertEqual(copied, state)
        copied["banks"][0]["mask"] = 1
        copied["flags"][0] = 0xe800
        copied["delivery_ring"]["slots"][2] = 0x1234
        self.assertEqual(state["banks"][0]["mask"], 0)
        self.assertEqual(state["flags"][0], 0)
        self.assertEqual(state["delivery_ring"]["slots"][2], 0)
        # Initialization itself does not establish nonzero/nonwrapping spans
        # or a physical address namespace. Admission checks mathematical span
        # premises only, not DRAM validity or device ownership.
        for bases, bank_bytes in (([], 0), ([0], 0), ([0xfffffff0], 0x20), ([0x4000000], 1)):
            self.assertEqual(MAP._ppb_bank_state(bases, bank_bytes)["bank_bytes"], bank_bytes)
        for count in (0, 1, 33, 34):
            self.assertEqual(MAP._ppb_bank_state([], 0, descriptor_limit=count)["descriptor_limit"], count)
        for kwargs in ({"bank_bases": [0] * 10}, {"bank_bases": [-1]}, {"bank_bases": [True]},
                       {"descriptor_limit": -1}, {"descriptor_limit": 35}, {"descriptor_limit": 255},
                       {"descriptor_limit": True}, {"bank_bytes": 0x100000000},
                       {"stripe_exponent": 256}, {"metadata_extra": 1}):
            arguments = {"bank_bases": [], "bank_bytes": 0}
            arguments.update(kwargs)
            with self.subTest(arguments=arguments), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_state(**arguments)
        mutations = (("flags", [0] * 33), ("flags", [0] * 33 + [65536]),
                     ("flags", [0] * 33 + [True]), ("assigned", [0] * 63 + [0]),
                     ("frame_flags", [0] * 62), ("frame_pool", -1),
                     ("descriptor_limit", 35), ("metadata_extra", 1))
        for key, value in mutations:
            changed = dict(state)
            changed[key] = value
            with self.subTest(key=key), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_copy_state(changed)
        changed = dict(state, unsupported_field=0)
        with self.assertRaises(MAP.FormatError):
            MAP._ppb_bank_copy_state(changed)

    def test_all65536_flag_predicates_preserve_distinct_native_gates(self):
        for flag in range(65536):
            live, references, started, delivered = bool(flag & 0x8000), bool(flag & 0x6000), \
                bool(flag & 0x0800), bool(flag & 0x1000)
            self.assertEqual(MAP._ppb_bank_flag_gates(flag), {
                "allocator_free": not live, "video_candidate": live or references,
                "deallocation_admitted": live and not references,
                "stop_eligible": live and started,
                "stop_force_release": live and started and not delivered}, hex(flag))
        for flag in (-1, 65536, True, None):
            with self.subTest(flag=flag), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_flag_gates(flag)

    def test_contract_receipts_initialization_release_dependencies_and_conditional_scope(self):
        contract = MAP._ppb_bank_contract(self.payload)
        self.assertEqual((contract["basis"]["complete_body_count"], contract["basis"]["code_bytes"],
                          contract["basis"]["region_count"], contract["basis"]["validated_bytes"]),
                         (35, 15712, 44, 71261))
        self.assertTrue(contract["basis"]["conditional"])
        self.assertEqual([(entry["role"], entry["blob_file_offset"], entry["size"])
                          for entry in contract["validated_regions"]], list(self.independent_regions()))
        self.assertEqual([(entry["owner"], entry["section_index"], entry["call_elf_virtual_address"],
                           entry["clear_mask"], entry["store_elf_virtual_address"])
                          for entry in contract["reference_edges"]],
                         [(name, section, call, clear, store)
                          for name, section, call, clear, _, store in self.release_calls])
        self.assertTrue(all(entry["store_before_callee"] for entry in contract["reference_edges"]))
        for edge in contract["reference_edges"]:
            section = self.sections[edge["section_index"]]
            delta = self.elf_base + section[4] - section[3]
            self.assertEqual(edge["call_blob_file_offset"], edge["call_elf_virtual_address"] + delta)
            self.assertEqual(edge["store_blob_file_offset"], edge["store_elf_virtual_address"] + delta)
        initialization = contract["context_initialization"]
        self.assertEqual((initialization["open_bank_count_maximum"], initialization["constructor_cleared_flag_count"],
                          initialization["constructor_saved_flags_offset"], initialization["metadata_record_bytes"]),
                         (9, 34, 0x354, 228))
        self.assertTrue(initialization["open_allows_zero_banks"])
        self.assertFalse(initialization["constructor_bank_count_guard"])
        self.assertFalse(initialization["pre_round_nonzero_implies_post_round_nonzero"])
        self.assertEqual(initialization["activation"]["snapshot_destination"], 0x3fffcdac)
        self.assertEqual(initialization["activation"]["common_header_bytes_not_copied"], 60)
        self.assertEqual(initialization["init_geometry"]["alignment_masks_for_request_word2"], [63, 127, 255])
        self.assertEqual(contract["bank_layout"]["entry_offsets_from_local_base"],
                         {"base": 56, "mask": 60, "stride": 64, "geometry": 68})
        self.assertFalse(contract["bank_layout"]["geometry_pack_masks_inputs"])
        self.assertTrue(contract["bank_layout"]["final_free_preserves_base"])
        self.assertEqual(contract["descriptor_layout"]["release_native_index_range"], [0, 33])
        self.assertFalse(contract["descriptor_layout"]["getter_native_index_guard"])
        self.assertFalse(contract["descriptor_layout"]["generation_field_validated"])
        stop = contract["operations"]["stop_selected"]
        self.assertEqual((stop["required_flag_mask"], stop["clear_flag_mask"]), (0x8800, 0x4000))
        self.assertTrue(stop["state_is_after_prelude"])
        self.assertIn("not modeled as no-ops", stop["prelude"])
        delivery = contract["delivery_edges"]
        self.assertFalse(delivery["drop4000_is_decode_completion"])
        self.assertEqual((delivery["attempt_release_call_elf_virtual_address"],
                          delivery["later_il_busy_read_elf_virtual_address"],
                          delivery["later_il_start_write_elf_virtual_address"]), (0xacb0, 0xad18, 0xad40))
        self.assertEqual(delivery["circ_buffer_data_word_index_range"], [2, 63])
        self.assertFalse(delivery["returned_or_delivered_metadata_address_is_host_plane_lease"])
        self.assertFalse(contract["empty_picture_path"]["all_both_zero_codec_outputs_tagged"])
        scope = contract["validation_scope"]
        for key in ("complete_selected_body_pins", "selected_state_projection",
                    "all_nine_direct_deallocation_edges", "original_section_mapping"):
            self.assertIs(scope[key], True)
        for key in ("whole_body_execution", "vendor_ISA", "runtime_relocation", "runtime_context_identity",
                    "source_plane_host_ownership", "completion_or_cache_coherence", "generation_safe_reuse",
                    "minimum_inner_ABI", "standalone_raw_feed", "silicon_incapability"):
            self.assertIs(scope[key], False)
        assumptions = " ".join(contract["assumptions"]).lower()
        for phrase in ("delay", "mul16", "unsupported model", "disjoint", "coherently", "opaque",
                       "after its monitor/ring prelude", "not universally proven"):
            self.assertIn(phrase, assumptions)
        self.assertEqual(json.loads(json.dumps(contract)), contract)

    def test_every_pinned_byte_mutation_refuses_before_context_interpretation(self):
        changed = bytearray(self.payload)
        mutations = 0
        with mock.patch.object(MAP, "_ppb_bank_elf_context",
                               side_effect=AssertionError("context interpreted before all pins")):
            for role, offset, size in self.independent_regions():
                # Includes every word's four bytes and non-word string tails.
                for position in range(offset, offset + size):
                    changed[position] ^= 1
                    try:
                        with self.assertRaises(MAP.FormatError, msg=f"{role}@{position:#x}"):
                            MAP._ppb_bank_contract(changed)
                    finally:
                        changed[position] ^= 1
                    mutations += 1
        self.assertEqual(mutations, 71261)
        self.assertEqual(changed, self.payload)

    def test_contract_budget_truncation_and_identity_fail_closed(self):
        with mock.patch.object(MAP, "MAX_PPB_BANK_REGIONS", 44), \
                mock.patch.object(MAP, "MAX_PPB_BANK_BYTES", 71261):
            self.assertEqual(MAP._ppb_bank_contract(self.payload)["basis"]["validated_bytes"], 71261)
        with mock.patch.object(MAP, "_ppb_bank_elf_context",
                               side_effect=AssertionError("context interpreted after validation refusal")):
            for field, maximum in (("MAX_PPB_BANK_REGIONS", 43), ("MAX_PPB_BANK_BYTES", 71260),
                                   ("MAX_PPB_BANK_RELOCATIONS", 0), ("MAX_PPB_BANK_MODEL_STEPS", 0)):
                with mock.patch.object(MAP, field, maximum), self.assertRaisesRegex(MAP.FormatError, "budget"):
                    MAP._ppb_bank_contract(self.payload)
            for payload in (b"", self.payload[:-1], self.payload + bytes(1), self.data):
                with self.subTest(size=len(payload)), self.assertRaisesRegex(MAP.FormatError, "size"):
                    MAP._ppb_bank_contract(payload)
            for _, offset, size in self.independent_regions():
                with self.subTest(offset=hex(offset)), self.assertRaises(MAP.FormatError):
                    MAP._ppb_bank_contract(self.payload[:offset + size - 1])

    def test_all_encoded_bank_subslots_and_wrapped_raw_getter_are_not_ownership(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000 + bank * 0x100000 for bank in range(9)], 0x100000)
        for bank in state["banks"]:
            bank.update(mask=0xffffffff, stride=0x1000, geometry=self.packed_geometry(64, 32, 32))
        for bank_index in range(16):
            for slot in range(32):
                state["flags"][0] = 0xe800 | slot << 4 | bank_index
                with self.subTest(bank=bank_index, slot=slot):
                    if bank_index >= 9:
                        with self.assertRaisesRegex(MAP.FormatError, "unmodeled bank"):
                            MAP._ppb_bank_step(contract, state, "video_address", index=0)
                    else:
                        result = MAP._ppb_bank_step(contract, state, "video_address", index=0)
                        self.assertEqual(result["result"], 0x100000 + bank_index * 0x100000 + slot * 0x1000)
                        self.assertEqual(result["state"], state)
                        state["banks"][bank_index]["mask"] ^= 1 << slot
                        self.assertEqual(MAP._ppb_bank_step(contract, state, "video_address", index=0)["result"], 0)
                        state["banks"][bank_index]["mask"] ^= 1 << slot
        state = MAP._ppb_bank_state([0x123400], 0x100000)
        state["banks"][0].update(mask=1, stride=0x1000, geometry=self.packed_geometry(64, 32, 32))
        for flag in (0x2000, 0x4000, 0x6000, 0x8000, 0xac00, 0xe800):
            state["flags"][0] = flag
            self.assertEqual(MAP._ppb_bank_step(contract, state, "video_address", index=0)["result"], 0x123400)
        for flag in (0, 0x0400, 0x0800, 0x1000, 0x1fff):
            state["flags"][0] = flag
            self.assertEqual(MAP._ppb_bank_step(contract, state, "video_address", index=0)["result"], 0)
        # AC00 and 2000/4000/6000 are genuinely getter-valid, not invented
        # special->zero cases. Ordinary admission must reject their invariants.
        for flag in (0xac00, 0x2000, 0x4000, 0x6000):
            state["flags"][0] = flag
            self.assertFalse(MAP._ppb_bank_admission(contract, state, 64, 32)["admitted"])
        state["banks"][0].update(base=0xfffffff0, stride=16, mask=2)
        state["flags"][0] = 0xe810
        self.assertEqual(MAP._ppb_bank_step(contract, state, "video_address", index=0)["result"], 0)
        state["banks"][0]["stride"] = 0xffffffff
        self.assertEqual(MAP._ppb_bank_step(contract, state, "video_address", index=0)["result"], 0xffffffef)
        for index in (-1, 34, 255, 0x80000000, 0xffffffff, True):
            with self.subTest(index=index), self.assertRaisesRegex(MAP.FormatError, "bounded model storage"):
                MAP._ppb_bank_step(contract, state, "video_address", index=index)

    def test_allocate_all_slots_banks_descriptor_limits_and_exhaustion(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        for slot in range(32):
            result = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
            self.assertEqual(result["result"], slot)
            state = result["state"]
            self.assertEqual(state["flags"][slot], 0xe800 | slot << 4)
            self.assertEqual(state["banks"][0], {"base": 0x100000, "mask": (1 << (slot + 1)) - 1,
                                                "stride": 0x2000, "geometry": 0x08010040})
        failed = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
        self.assertEqual((failed["result"], failed["state"], failed["events"]), (-1, state, []))
        state["banks"].append({"base": 0x200000, "mask": 0, "stride": 0, "geometry": 0})
        for slot in (32, 33):
            result = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
            self.assertEqual(result["result"], slot)
            state = result["state"]
            self.assertEqual(state["flags"][slot], 0xe801 | (slot - 32) << 4)
        self.assertEqual(MAP._ppb_bank_step(contract, state, "allocate", width=0, height=0)["result"], -1)
        for limit in (0, 1, 33, 34):
            state = MAP._ppb_bank_state([], 0, descriptor_limit=limit)
            state["flags"] = [0x8000] * 34
            if limit:
                state["flags"][limit - 1] = 0x6000  # No8000 remains allocator-free, despite getter gate.
            result = MAP._ppb_bank_step(contract, state, "allocate", width=0, height=0)
            self.assertEqual(result["result"], limit - 1 if limit else -1)
            if limit:
                self.assertEqual(result["state"]["flags"][limit - 1], 0xac00)
            self.assertEqual(result["state"]["banks"], [])
        # Existing available geometry wins over an earlier empty bank.
        state = MAP._ppb_bank_state([0x100000, 0x200000], 0x40000)
        state["banks"][1].update(mask=1, stride=0x2000, geometry=self.packed_geometry(64, 32, 4))
        result = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
        self.assertEqual(result["state"]["flags"][0], 0xe811)
        self.assertEqual(result["state"]["banks"][0], state["banks"][0])
        # Last free bit at every position is chosen, including bit31.
        for slot in range(32):
            state = MAP._ppb_bank_state([0x100000], 0x40000)
            state["banks"][0].update(mask=0xffffffff ^ (1 << slot), stride=0x2000,
                                      geometry=self.packed_geometry(64, 32, 32))
            result = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
            self.assertEqual(result["state"]["flags"][0], 0xe800 | slot << 4)
            self.assertEqual(result["state"]["banks"][0]["mask"], 0xffffffff)

    def test_capacity_zero_full_and_unsupported_shifts_and_unmasked_geometry(self):
        contract = MAP._ppb_bank_contract(self.payload)
        for capacity in (0, 1, 31, 32, 33, 63):
            state = MAP._ppb_bank_state([0x100000], 0x40000)
            mask = 0xffffffff if capacity == 32 else (1 << capacity) - 1 if capacity < 32 else 0
            state["banks"][0].update(mask=mask, stride=0x2000,
                                      geometry=self.packed_geometry(64, 32, capacity))
            if capacity > 32:
                with self.assertRaisesRegex(MAP.FormatError, "unvalidated vendor"):
                    MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
            else:
                failed = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
                self.assertEqual((failed["result"], failed["state"]), (-1, state))
        # One zero dimension is not the special path. Zero pitch yields real
        # division-by-zero quotient, cap32, stride0 and identical slot bases.
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        first = MAP._ppb_bank_step(contract, state, "allocate", width=0, height=480)
        second = MAP._ppb_bank_step(contract, first["state"], "allocate", width=0, height=480)
        self.assertEqual(first["state"]["banks"][0]["geometry"], self.packed_geometry(0, 480, 32))
        self.assertEqual(first["state"]["banks"][0]["stride"], 0)
        for result, index in ((first, 0), (second, 1)):
            self.assertEqual(MAP._ppb_bank_step(contract, result["state"], "video_address", index=index)["result"], 0x100000)
        # Unmasked oversized H contaminates the six-bit capacity field.
        oversized = MAP._ppb_bank_step(contract, state, "allocate", width=0, height=2048)
        packed = oversized["state"]["banks"][0]["geometry"]
        self.assertEqual(packed, self.packed_geometry(0, 2048, 32))
        self.assertEqual((packed >> 22) & 63, 33)
        # Oversized W contaminates the extracted H field, too.
        oversized = MAP._ppb_bank_step(contract, state, "allocate", width=2048, height=32)
        packed = oversized["state"]["banks"][0]["geometry"]
        self.assertEqual((packed & 2047, (packed >> 11) & 2047), (0, 33))
        # Too-large stride leaves flags/banks unchanged, but reports the
        # conditional failure event; no native raw bounds promise follows.
        state = MAP._ppb_bank_state([0x100000], 1)
        result = MAP._ppb_bank_step(contract, state, "allocate", width=640, height=480)
        expected = dict(state, core_error_flags=0x0400)
        self.assertEqual((result["result"], result["state"]), (-1, expected))
        self.assertEqual(result["events"], [
            {"kind": "core_error_flags", "value": 0x0400},
            {"kind": "frame_exceeds_bank", "frame_bytes": 0x78000, "bank_bytes": 1}])

    def test_deallocator_reference_gates_special_skip_and_final_bank_cleanup(self):
        contract = MAP._ppb_bank_contract(self.payload)
        for flag in (0, 0x2000, 0x4000, 0x6000, 0x8000, 0x8800, 0x9000, 0x9800,
                     0xa800, 0xc800, 0xe800, 0xf800, 0x8400, 0x8c00, 0xac00):
            state = MAP._ppb_bank_state([0x100000], 0x40000)
            state["flags"][0] = flag
            state["banks"][0].update(mask=1, stride=0x2000, geometry=self.packed_geometry(64, 32, 1))
            result = MAP._ppb_bank_step(contract, state, "deallocate", index=0)
            admitted = bool(flag & 0x8000) and not bool(flag & 0x6000)
            self.assertEqual(result["result"], admitted, hex(flag))
            if not admitted:
                self.assertEqual((result["state"], result["events"]), (state, []))
            else:
                self.assertEqual(result["state"]["flags"][0], 0)
                expected_bank = state["banks"][0] if flag & 0x0400 else {
                    "base": 0x100000, "mask": 0, "stride": 0, "geometry": 0}
                self.assertEqual(result["state"]["banks"][0], expected_bank)
                self.assertEqual(result["events"][0]["conditional_nonzero_fields"], [68, 224])
                self.assertFalse(result["events"][0]["complete_callee_effects_modeled"])
        for slot in range(32):
            state = MAP._ppb_bank_state([0x100000], 0x40000)
            state["flags"][0] = 0x8800 | slot << 4
            state["banks"][0].update(mask=0xffffffff, stride=0x2000,
                                      geometry=self.packed_geometry(64, 32, 32))
            result = MAP._ppb_bank_step(contract, state, "deallocate", index=0)
            self.assertEqual(result["state"]["banks"][0]["mask"], 0xffffffff ^ (1 << slot))
            self.assertEqual(result["state"]["banks"][0]["geometry"], state["banks"][0]["geometry"])
            state["banks"][0]["mask"] = 1 << slot
            result = MAP._ppb_bank_step(contract, state, "deallocate", index=0)
            self.assertEqual(result["state"]["banks"][0],
                             {"base": 0x100000, "mask": 0, "stride": 0, "geometry": 0})

    def test_all_nine_reference_transitions_preserve_remaining_holds(self):
        contract = MAP._ppb_bank_contract(self.payload)
        roles = ("returned", "latest", "attempt", "discard", "no_display", "release",
                 "undelivered_display", "undelivered_return", "late")
        for role, (_, _, _, clear, _, _) in zip(roles, self.release_calls):
            for original in (0xe800, 0x8800 | clear):
                state = MAP._ppb_bank_state([0x100000], 0x40000)
                state["flags"][0] = original
                state["banks"][0].update(mask=1, stride=0x2000, geometry=self.packed_geometry(64, 32, 1))
                result = MAP._ppb_bank_step(contract, state, "reference_drop", index=0, caller=role)
                dropped = original & ~clear
                freed = bool(dropped & 0x8000) and not bool(dropped & 0x6000)
                self.assertEqual(result["result"], freed, role)
                self.assertEqual(result["events"][0], {"kind": "reference_drop", "index": 0,
                                                       "caller": role, "clear_mask": clear, "value": dropped})
                self.assertEqual(result["state"]["flags"][0], 0 if freed else dropped)
                self.assertEqual(result["state"]["banks"][0]["mask"], 0 if freed else 1)
        with self.assertRaises(MAP.FormatError):
            MAP._ppb_bank_step(contract, state, "reference_drop", index=0, caller="decoder_completed")

    def test_release_repeat_and_allocate_release_reuse_stale_release_aba(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        first = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
        second = MAP._ppb_bank_step(contract, first["state"], "allocate", width=64, height=32)
        self.assertEqual((first["result"], second["result"]), (0, 1))
        released = MAP._ppb_bank_step(contract, second["state"], "release", index=0)
        self.assertTrue(released["result"])
        self.assertEqual(released["state"]["banks"][0]["mask"], 2)
        self.assertEqual(released["state"]["banks"][0]["stride"], 0x2000)
        repeated = MAP._ppb_bank_step(contract, released["state"], "release", index=0)
        self.assertFalse(repeated["result"])
        self.assertEqual(repeated["state"], released["state"])
        reused = MAP._ppb_bank_step(contract, released["state"], "allocate", width=64, height=32)
        self.assertEqual(reused["result"], 0)
        self.assertEqual(reused["state"]["flags"][0], first["state"]["flags"][0])
        stale = MAP._ppb_bank_step(contract, reused["state"], "release", index=first["result"])
        self.assertTrue(stale["result"])  # Old integer ID frees the new occupant: no generations.
        self.assertEqual(stale["state"]["flags"][0], 0)
        final = MAP._ppb_bank_step(contract, stale["state"], "release", index=1)
        self.assertEqual(final["state"]["banks"][0],
                         {"base": 0x100000, "mask": 0, "stride": 0, "geometry": 0})
        state = MAP._ppb_bank_state([], 0, descriptor_limit=1)
        state["flags"][33] = 0x8400
        self.assertTrue(MAP._ppb_bank_step(contract, state, "release", index=33)["result"])
        for index in (34, 255, 0x7fffffff, 0x80000000, 0xffffffff):
            result = MAP._ppb_bank_step(contract, state, "release", index=index)
            self.assertEqual((result["result"], result["state"]), (False, state))

    def test_start_preserves_banks_and_stop_post_prelude_is_not_free_all(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        state["flags"] = [0xffff - index for index in range(34)]
        state["frame_flags"], state["frame_word124"] = [0xffff] * 63, [0xffffffff] * 63
        state["core_error_flags"] = 0xabffffff
        result = MAP._ppb_bank_step(contract, state, "start")
        self.assertEqual(result["state"]["flags"], [flag & ~0x0800 for flag in state["flags"]])
        self.assertEqual(result["state"]["banks"], state["banks"])
        self.assertEqual(result["state"]["core_error_flags"], 0xab000100)
        self.assertEqual(result["state"]["frame_flags"], [0] * 63)
        self.assertEqual(result["state"]["frame_word124"], [0] * 63)
        self.assertFalse(result["events"][0]["opaque_picture_scan_and_other_state_effects_modeled"])
        for original in (0, 0x0800, 0x8000, 0x8800, 0xe000, 0xe800, 0xf800,
                         0xc800, 0xd800, 0xa800, 0xb800, 0x9800, 0x8c00, 0xac00, 0xbc00):
            state = MAP._ppb_bank_state([0x100000], 0x40000)
            state["flags"][0] = original
            state["banks"][0].update(mask=1, stride=0x2000, geometry=self.packed_geometry(64, 32, 1))
            result = MAP._ppb_bank_step(contract, state, "stop_selected")
            eligible = bool(original & 0x8000) and bool(original & 0x0800)
            forced = eligible and not bool(original & 0x1000)
            expected_flag = 0 if forced else original & ~0x4000 if eligible else original
            self.assertEqual(result["state"]["flags"][0], expected_flag, hex(original))
            self.assertEqual(result["result"], [0] if forced else [])
            expected_mask = 0 if forced and not original & 0x0400 else 1
            self.assertEqual(result["state"]["banks"][0]["mask"], expected_mask)
        # Old allocations without0800, and delivered allocations with1000,
        # explicitly survive this selected loop. Prelude completion is input.
        self.assertEqual(MAP._ppb_bank_contract(self.payload)["operations"]["stop_selected"]["required_flag_mask"], 0x8800)

    def test_new_bank_mixed_core_error_word_and_constructor_wrap_projection(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        state["core_error_flags"] = 0xabcdffff
        fitted = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
        self.assertEqual(fitted["state"]["core_error_flags"], 0xabcdfbff)
        reused_state = fitted["state"]
        reused_state["core_error_flags"] = 0xabcdffff
        reused = MAP._ppb_bank_step(contract, reused_state, "allocate", width=64, height=32)
        self.assertEqual(reused["state"]["core_error_flags"], 0xabcdffff)
        state["flags"] = [0x8000] * 34
        failed = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
        self.assertEqual(failed["state"]["core_error_flags"], 0xabcdffff)
        constructed = MAP._ppb_bank_step(contract, state, "constructor", base=0xfffff000, count=3, bank_bytes=0x1000)
        self.assertEqual([bank["base"] for bank in constructed["state"]["banks"]], [0xfffff000, 0, 0x1000])
        self.assertEqual(constructed["state"]["flags"], [0] * 34)
        self.assertEqual(constructed["state"]["core_error_flags"], 0)
        self.assertFalse(MAP._ppb_bank_admission(contract, constructed["state"], 64, 32)["admitted"])
        for count in (-1, 10, 255, True):
            with self.subTest(count=count), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_step(contract, state, "constructor", base=0x100000, count=count, bank_bytes=0x1000)

    def test_admission_requires_exact_initialized_ownership_geometry_and_nonwrapping_spans(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        self.assertTrue(MAP._ppb_bank_admission(contract, state, 64, 32)["admitted"])
        state = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)["state"]
        admitted = MAP._ppb_bank_admission(contract, state, 64, 32)
        self.assertTrue(admitted["admitted"])
        self.assertEqual(admitted["reasons"], [])
        self.assertTrue(admitted["conditional_vendor_ISA"])
        for key in ("runtime_ownership_established", "host_plane_lease", "generation_safe_reuse"):
            self.assertIs(admitted[key], False)
        # The test starts each refusal from a genuinely admitted state, so an
        # unrelated failed premise cannot mask a missing ownership/extent gate.
        mutations = (
            ("banks", [{"base": 0, "mask": 1, "stride": 0x2000, "geometry": 0x08010040}]),
            ("banks", [{"base": 0xfffff000, "mask": 1, "stride": 0x2000, "geometry": 0x08010040}]),
            ("banks", [{"base": 0x100001, "mask": 1, "stride": 0x2000, "geometry": 0x08010040}]),
            ("bank_bytes", 0x40001), ("bank_bytes", 0), ("descriptor_limit", 0),
            ("stripe_exponent", 32), ("alignment_mask", 0),
            ("flags", [0x6000] + [0] * 33), ("flags", [0xac00] + [0] * 33),
            ("flags", [0xe808] + [0] * 33), ("flags", [0xe810] + [0] * 33),
            ("flags", [0xe800, 0xe800] + [0] * 32),
            ("flags", [0] * 34),
            ("banks", [{"base": 0x100000, "mask": 0, "stride": 0x2000, "geometry": 0x08010040}]),
            ("banks", [{"base": 0x100000, "mask": 1, "stride": 0, "geometry": 0x08010040}]),
            ("banks", [{"base": 0x100000, "mask": 1, "stride": 0x3000, "geometry": 0x08010040}]),
            ("banks", [{"base": 0x100000, "mask": 1, "stride": 0x2000, "geometry": 0}]),
            ("banks", [{"base": 0x100000, "mask": 1, "stride": 0x2000,
                        "geometry": self.packed_geometry(64, 32, 31)}]),
            ("banks", [{"base": 0x100000, "mask": 1, "stride": 0x2000, "geometry": 0x18010040}]),
            ("banks", [{"base": 0x100000, "mask": 0x80000001, "stride": 0x2000,
                        "geometry": self.packed_geometry(64, 32, 1)}]),
        )
        for key, value in mutations:
            changed = dict(state)
            changed[key] = value
            refusal = MAP._ppb_bank_admission(contract, changed, 64, 32)
            with self.subTest(key=key, value=value):
                self.assertFalse(refusal["admitted"])
                self.assertTrue(refusal["reasons"])
                self.assertFalse(refusal["host_plane_lease"])
        for bases in ([0x100000, 0x100000], [0x100000, 0x120000], [0x100000, 0x200000]):
            changed = MAP._ppb_bank_state(bases, 0x40000)
            self.assertFalse(MAP._ppb_bank_admission(contract, changed, 64, 32)["admitted"])
        self.assertTrue(MAP._ppb_bank_admission(contract, MAP._ppb_bank_state([0x100000, 0x140000], 0x40000), 64, 32)["admitted"])
        for width, height in ((0, 0), (0, 480), (640, 0), (2048, 32), (64, 1121), (64, 2047)):
            refusal = MAP._ppb_bank_admission(contract, MAP._ppb_bank_state([0x100000], 0x400000), width, height)
            self.assertFalse(refusal["admitted"], (width, height))
        # Raw truncated-row geometry remains modeled; admission alone excludes it.
        raw = MAP._ppb_bank_step(contract, MAP._ppb_bank_state([0x100000], 0x400000),
                                 "allocate", width=64, height=1121)
        self.assertEqual(raw["result"], 0)
        self.assertEqual(MAP._ppb_bank_geometry(64, 1121, 5, 63)["y_stripe_height"], 1120)

    def test_assignment_reference_request_and_completion_are_distinct_selected_effects(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        state = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)["state"]
        for flags in (0, 0x2000, 0x4000, 0xffff):
            result = MAP._ppb_bank_step(contract, state, "assignment_reference", index=0, frame_flags=flags)
            self.assertEqual(result["result"], 0xc800 if flags & 0x4000 else 0xe800)
            self.assertEqual(result["state"]["banks"], state["banks"])
        requested = MAP._ppb_bank_step(contract, state, "order_release", frame=62, reason=255)
        self.assertEqual(requested["state"]["frame_flags"][62], 0x10)
        self.assertEqual(requested["state"]["release_request"]["slots"][0], 0xff3e)
        self.assertEqual(requested["state"]["release_request"]["head"], 1)
        self.assertEqual(requested["state"]["flags"], state["flags"])
        self.assertEqual(requested["state"]["banks"], state["banks"])
        self.assertTrue(requested["events"][0]["no_immediate_bank_release"])
        state["release_request"]["head"] = 63
        wrapped = MAP._ppb_bank_step(contract, state, "order_release", frame=0, reason=1)
        self.assertEqual(wrapped["state"]["release_request"]["head"], 0)
        self.assertEqual(wrapped["state"]["release_request"]["slots"][63], 0x100)
        state["frame_flags"][62] = 0xffff
        completed = MAP._ppb_bank_step(contract, state, "monitor_complete", frame=62)
        self.assertEqual(completed["state"]["frame_flags"][62], 0xdbff)
        self.assertEqual(completed["state"]["flags"], state["flags"])
        self.assertEqual(completed["state"]["banks"], state["banks"])
        self.assertTrue(completed["events"][0]["completed_dma_metadata_is_input"])
        self.assertFalse(completed["events"][0]["metadata_word0_effects_modeled"])
        for frame in (-1, 63, 255, True):
            with self.subTest(frame=frame), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_step(contract, state, "monitor_complete", frame=frame)

    def test_display_publication_offsets_tagged_empty_and_discard_reference(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        state["metadata_pool"] = 0x300000
        state = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)["state"]
        for optional in (0, 1, 0xffffffff):
            record = {"flags": 0, "y_offset": 0x1234, "chroma_offset": 0xffffffff, "optional_offset": optional}
            result = MAP._ppb_bank_step(contract, state, "display_publish", index=0, record=record)
            self.assertEqual(result["result"], {"flags": 0, "y_offset": 0x100000,
                                                "chroma_offset": 0x0fffff,
                                                "optional_offset": (optional + 0x100000) & 0xffffffff if optional else 0})
            self.assertEqual(record["y_offset"], 0x1234)  # Caller record is not rewritten.
            self.assertEqual(result["state"]["flags"][0], 0xf800)
            self.assertEqual(result["state"]["delivery_ring"]["slots"][2], 0x300000)
            self.assertEqual(result["state"]["delivery_ring"]["write"], 3)
            publication = result["events"][0]
            self.assertEqual((publication["record_address"], publication["record_bytes"]), (0x300000, 228))
            self.assertTrue(publication["dma_and_sync_assumed"])
            self.assertFalse(next(event for event in result["events"] if event["kind"] == "deliver_picture")["is_host_plane_lease"])
        empty = MAP._ppb_bank_step(contract, state, "empty_picture")
        self.assertEqual(empty["result"], {"width": 0, "height": 0, "picture_flags": 0x100,
                                           "record_bytes": 228, "selected_producer_only": True})
        no_video = MAP._ppb_bank_state([], 0)
        no_video = MAP._ppb_bank_step(contract, no_video, "allocate", width=0, height=0)["state"]
        tagged = {"flags": 0x100, "y_offset": 11, "chroma_offset": 22, "optional_offset": 33}
        result = MAP._ppb_bank_step(contract, no_video, "display_publish", index=0, record=tagged)
        self.assertEqual(result["result"], tagged)
        self.assertEqual(result["state"]["flags"][0], 0xbc00)
        # Without the producer's tag, AC00 really aliases occupied bank0/slot0.
        state["flags"][0] = 0xac00
        untagged = MAP._ppb_bank_step(contract, state, "display_publish", index=0,
                                     record={"flags": 0, "y_offset": 0, "chroma_offset": 0, "optional_offset": 0})
        self.assertEqual(untagged["result"]["y_offset"], 0x100000)
        state["flags"][0] = 0xa800  # No4000 hold; discarded clears3000 and actually frees.
        discarded = MAP._ppb_bank_step(contract, state, "display_publish", index=0,
                                       record={"flags": 0, "y_offset": 0, "chroma_offset": 0, "optional_offset": 0}, discarded=True)
        self.assertEqual(discarded["state"]["flags"][0], 0)
        self.assertEqual(discarded["state"]["banks"][0]["mask"], 0)
        self.assertEqual(discarded["state"]["delivery_ring"], state["delivery_ring"])

    def test_circular_data_positions_wrap_full_alias_and_returned_metadata_lookup(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([], 0)
        for position in range(2, 64):
            result = MAP._ppb_bank_step(contract, state, "ring_put", ring="delivery_ring", value=position)
            self.assertEqual(result["state"]["delivery_ring"]["slots"][position], position)
            self.assertFalse(result["events"][0]["native_full_guard"])
            state = result["state"]
        self.assertEqual((state["delivery_ring"]["read"], state["delivery_ring"]["write"]), (2, 2))
        # No put fullness guard: a complete62-slot cycle is indistinguishable
        # from empty to this getter. No invented flow-control guarantee.
        self.assertEqual(MAP._ppb_bank_step(contract, state, "ring_get", ring="delivery_ring")["result"], 0)
        state["delivery_ring"].update(read=63, write=2)
        result = MAP._ppb_bank_step(contract, state, "ring_get", ring="delivery_ring")
        self.assertEqual((result["result"], result["state"]["delivery_ring"]["read"]), (63, 2))
        for read, write in ((0, 2), (1, 2), (2, 0), (2, 1)):
            state["delivery_ring"].update(read=read, write=write)
            self.assertEqual(MAP._ppb_bank_step(contract, state, "ring_get", ring="delivery_ring")["result"], 0)
            with self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_step(contract, state, "ring_put", ring="delivery_ring", value=0x1234)
        for base in (0, 0x300000, 0xffffff80):
            state["metadata_pool"] = base
            for index in range(34):
                address = (base + index * 228) & 0xffffffff
                self.assertEqual(MAP._ppb_bank_step(contract, state, "ppb_from_address", address=address)["result"], index)
                self.assertEqual(MAP._ppb_bank_step(contract, state, "ppb_from_address", address=(address + 1) & 0xffffffff)["result"], -1)

    def test_explicit_undelivered_metadata_inputs_release_without_completion_claim(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        state["metadata_pool"] = 0x300000
        for _ in range(2):
            state = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)["state"]
        result = MAP._ppb_bank_step(contract, state, "undelivered",
                                    delivery_addresses=[0x300000, 0xdeadbeef], return_addresses=[0x3000e4, 0x300000])
        self.assertEqual(result["result"], [0, 1])
        self.assertEqual(result["state"]["flags"], [0] * 34)
        self.assertEqual(result["state"]["banks"][0], {"base": 0x100000, "mask": 0, "stride": 0, "geometry": 0})
        selected = [event for event in result["events"] if event["kind"] == "selected_undelivered_ring_result"]
        self.assertEqual([event["index"] for event in selected], [0, -1, 1, 0])
        self.assertEqual([event["caller"] for event in selected],
                         ["undelivered_display", "undelivered_display", "undelivered_return", "undelivered_return"])
        for values in ([0], [-1], [0x100000000], [True], None):
            with self.subTest(values=values), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_step(contract, state, "undelivered", delivery_addresses=values, return_addresses=[])

    def test_unsupported_models_argument_shapes_storage_and_step_budgets_refuse_purely(self):
        contract = MAP._ppb_bank_contract(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        original = json.dumps(state, sort_keys=True)
        for unsupported in (None, {}, {"basis": None}, {"basis": {"model": "whole-firmware"}},
                            {"basis": {"model": "selected-ppb-bank-v1"}, "validation_scope": None},
                            {"basis": {"model": "selected-ppb-bank-v1"}, "validation_scope": {}}):
            with self.subTest(contract=unsupported), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_step(unsupported, state, "start")
        for operation, arguments in ((None, {}), ([], {}), ({}, {}), ("stop", {}), ("allocate", {}),
                                     ("allocate", {"width": 64, "height": 32, "physical_base": 0x100000}),
                                     ("start", {"index": 0}), ("reference_drop", {"index": 0}),
                                     ("display_publish", {"index": 0, "record": {}}),
                                     ("display_publish", {"index": 0, "record": {
                                         "flags": 0, "y_offset": 0, "chroma_offset": 0, "optional_offset": 0}, "discarded": 1}),
                                     ("ring_put", {"ring": "release_request", "value": 1}),
                                     ("ring_get", {"ring": "return_ring", "completion": True}),
                                     ("order_release", {"frame": 0, "reason": 256}),
                                     ("assignment_reference", {"index": 0, "frame_flags": 65536}),
                                     ("release", {"index": True})):
            with self.subTest(operation=operation, arguments=arguments), self.assertRaises(MAP.FormatError):
                MAP._ppb_bank_step(contract, state, operation, **arguments)
        for maximum in (0, 1, 2, 3):
            with mock.patch.object(MAP, "MAX_PPB_BANK_MODEL_STEPS", maximum), \
                    self.assertRaisesRegex(MAP.FormatError, "budget"):
                MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
        self.assertEqual(json.dumps(state, sort_keys=True), original)
        with mock.patch.object(MAP, "MAX_PPB_BANK_MODEL_STEPS", 1), self.assertRaisesRegex(MAP.FormatError, "budget"):
            MAP._ppb_bank_step(contract, state, "undelivered", delivery_addresses=[1, 2], return_addresses=[])
        self.assertEqual(json.dumps(state, sort_keys=True), original)
        state["stripe_exponent"] = 255  # INIT byte wrapping is not a native shift validity guard.
        with self.assertRaisesRegex(MAP.FormatError, "supported ISA"):
            MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
        self.assertFalse(MAP._ppb_bank_admission(contract, state, 64, 32)["admitted"])

    def test_private_contract_models_are_offline_pure_and_no_public_cli_route_exists(self):
        mutable = bytearray(self.payload)
        state = MAP._ppb_bank_state([0x100000], 0x40000)
        state_before = json.dumps(state, sort_keys=True)
        with mock.patch("builtins.open", side_effect=AssertionError("unexpected file read")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("unexpected file/device open")), \
                mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected external command")), \
                mock.patch.object(subprocess, "Popen", side_effect=AssertionError("unexpected external process")):
            contract = MAP._ppb_bank_contract(mutable)
            contract_before = json.dumps(contract, sort_keys=True)
            result = MAP._ppb_bank_step(contract, state, "allocate", width=64, height=32)
            MAP._ppb_bank_step(contract, result["state"], "release", index=0)
            MAP._ppb_bank_admission(contract, state, 64, 32)
        self.assertEqual(mutable, self.payload)
        self.assertEqual(json.dumps(state, sort_keys=True), state_before)
        self.assertEqual(json.dumps(contract, sort_keys=True), contract_before)
        for option in ("--ppb-bank-contract", "--ppb-bank-step=allocate", "--ppb-bank-budget"):
            with mock.patch.object(MAP, "read_firmware", side_effect=AssertionError("unexpected file read")), \
                    mock.patch.object(sys, "stderr", new_callable=io.StringIO), self.assertRaises(SystemExit) as error:
                MAP.main(["offline.bin", option])
            self.assertEqual(error.exception.code, 2)

    def test_private_ppb_helpers_leave_all256_existing_public_reports_unchanged(self):
        options = ("references", "all_symbols", "bootstrap", "picture_output",
                   "arc_metadata", "csc_command", "command_buffer_bridge", "inner_descriptor")
        aggregate = hashlib.sha256()
        with mock.patch.object(MAP, "_ppb_bank_contract", side_effect=AssertionError("unexpected private contract")), \
                mock.patch.object(MAP, "_ppb_bank_step", side_effect=AssertionError("unexpected private transition")), \
                mock.patch.object(MAP, "_ppb_bank_admission", side_effect=AssertionError("unexpected private admission")):
            for mask in range(256):
                flags = {name: bool(mask & (1 << bit)) for bit, name in enumerate(options)}
                wanted = ("ReadLine",) if mask & 1 else MAP.DEFAULT_SYMBOLS
                report = MAP.analyze(self.data, wanted, **flags)
                self.assertNotIn("ppb_bank_contract", report)
                prior_inner_descriptor_projection(report)
                stdout = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
                aggregate.update(bytes([mask]))
                aggregate.update(hashlib.sha256(stdout).digest())
        # Shared public-output snapshot for private-helper isolation.
        self.assertEqual(aggregate.hexdigest(),
                         "33f775ebb6fc935321506ac22faeb48857f2e40b1ba85f114f1d77570b4348b6")


class FirmwareFreshInitCausalTests(unittest.TestCase):
    # Independent GNU ARM/ARC disassembly oracles. Offsets here are ARM blob
    # offsets; ARC addresses below are resolved through their own ELF section.
    ARM_WORDS = {
        0x5cd8: 0xe2805014, 0x5cdc: 0xe2804f45, 0x5cec: 0xe3a08000,
        0x5cf0: 0xe5970000, 0x5cf4: 0xe3500000,
        0x5d04: 0xe5848008, 0x5d08: 0xe5950004, 0x5d0c: 0xe5840004,
        0x5d10: 0xe3a00000, 0x5d14: 0xe8bd81f0,
        0x5d50: 0xe1a06000, 0x5d54: 0xe3560000,
        0x5d64: 0xe3e00000, 0x5d68: 0xe5840008, 0x5d74: 0xe1a00006,
        0x5f10: 0xe3a00001, 0x5f14: 0xe5870000,
        0x5f20: 0xe5970004, 0x5f24: 0xe5808744,
        0x7dc: 0xe2840008, 0x7e4: 0xe1a05000, 0x7e8: 0xe3550000,
        0x850: 0xe3a00000, 0x854: 0xe5c40740, 0x860: 0xe1a03005,
        0x86c: 0xe28dd05c, 0x870: 0xe8bd80f0, 0x880: 0xe5874000,
        0x884: 0xe3a00002, 0x888: 0xe5c40740, 0x890: 0xe3a00000,
        0xe8f4: 0xe28dd01c, 0xe8f8: 0xe8bd8ff0,
        0xec2c: 0xe1a00004, 0xec34: 0xe1a05000, 0xec38: 0xe3550000,
        0xec40: 0xe1a00004, 0xec48: 0xe1a00005,
        0xec50: 0xe1a00004, 0xec58: 0xe1a05000, 0xec5c: 0xe3550000,
        0xec64: 0xe1a00004, 0xec6c: 0xe1a00005,
        0xec7c: 0xe1a05000, 0xec80: 0xe3550000, 0xec90: 0xe1a00005,
        0xeca4: 0xe1a05000, 0xeca8: 0xe3550000, 0xecb8: 0xe1a00005,
        0xecc8: 0xe1a05000, 0xeccc: 0xe3550000, 0xecdc: 0xe1a00005,
        0xecf0: 0xe5884000, 0xecf4: 0xe3a00000,
        0x2669c: 0xe5951064, 0x266a0: 0xe1a00005,
        0x266a8: 0xe1a04000, 0x266ac: 0xe3540000, 0x266b4: 0xe1a00004,
        0x2667c: 0xe8bd8070, 0x266e8: 0xe3a00000,
        0x263bc: 0xe5d42030, 0x263c0: 0xe1a01006, 0x263c4: 0xe1a00004,
        0x263cc: 0xe1a05000, 0x263d0: 0xe3550000, 0x263e0: 0xe1a00005,
        0x26388: 0xe8bd81f0, 0x263f0: 0xe3a00000,
        0x271d0: 0xe1a04000, 0x271d4: 0xe1a0b001, 0x271d8: 0xe1a09002,
        0x271e0: 0xe28d5c01, 0x271e4: 0xe28d8004,
        0x271e8: 0xe3a020fc, 0x271ec: 0xe3a01000, 0x271f0: 0xe1a00005,
        0x271f8: 0xe3a020fc, 0x271fc: 0xe3a01000, 0x27200: 0xe1a00008,
        0x27214: 0xe5860000, 0x27218: 0xe5869004,
        0x2721c: 0xe594020c, 0x27220: 0xe5860008,
        0x27224: 0xe5940210, 0x27228: 0xe586000c,
        0x2722c: 0xe59400b4, 0x27230: 0xe5860010,
        0x27234: 0xe59400a8, 0x27238: 0xe5860014,
        0x2723c: 0xe3043e20, 0x27240: 0xe58d3000,
        0x27244: 0xe28d3004, 0x27248: 0xe28d2c01,
        0x2724c: 0xe1a0100b, 0x27250: 0xe1a00004,
        0x27258: 0xe1a0a000, 0x27288: 0xe35a0000,
        0x27298: 0xe1a0000a, 0x2729c: 0xe28ddf7f, 0x272a0: 0xe8bd8ff0,
        0x273e4: 0xe5970008, 0x273e8: 0xe584018c, 0x273f4: 0xe1a0000a,
        0x27060: 0xe1a04000, 0x27064: 0xe1a08001, 0x27068: 0xe1a06002,
        0x2706c: 0xe1a05003, 0x27070: 0xe59db028,
        0x27080: 0xe5d4008c, 0x27084: 0xe3500000,
        0x27094: 0xe8bd9ff0, 0x27098: 0xe3a00001, 0x2709c: 0xe5c4008c,
        0x270a4: 0xe5940088, 0x270ac: 0xe3a020fc, 0x270b0: 0xe1a01006,
        0x270b4: 0xe5940094, 0x270bc: 0xe5941118, 0x270c0: 0xe59421cc,
        0x270c4: 0xe1a00004, 0x270d8: 0xe1a0100b, 0x270dc: 0xe5940088,
        0x270e4: 0xe1a09000, 0x270e8: 0xe3590005,
        0x270f0: 0xe3a00000, 0x270f4: 0xe5c4008c, 0x270f8: 0xe1a00009,
        0x27108: 0xe5941114, 0x2710c: 0xe1a00004,
        0x27114: 0xe1a0a000, 0x27118: 0xe35a0000,
        0x27120: 0xe3a020fc, 0x27124: 0xe1a00005, 0x27128: 0xe5941094,
        0x27130: 0xe5960000, 0x27134: 0xe5951000, 0x27138: 0xe1500001,
        0x27140: 0xe3a00000, 0x27144: 0xe5c4008c, 0x27150: 0xe3a00002,
        0x27160: 0xe5970004, 0x27164: 0xe3500000,
        0x2716c: 0xe3a00000, 0x27170: 0xe5c4008c, 0x27198: 0xe3a00002,
        0x271a8: 0xe3a00009, 0x271b0: 0xe3a00000,
        0x271b4: 0xe5c4008c, 0x271b8: 0xe5940088, 0x271c0: 0xe1a00009,
        0x20690: 0xe1a01000, 0x20694: 0xe3a02000, 0x20698: 0xe5c12000,
        0x20614: 0xe1a01000, 0x20618: 0xe3a02001, 0x2061c: 0xe5c12000,
        0x205a4: 0xe3a06000, 0x205ac: 0xe3a08000,
        0x205c4: 0xe2888001, 0x205c8: 0xe3a00064, 0x205e4: 0xe3a06005,
        0x205ec: 0xe5d70000, 0x205f0: 0xe3500000, 0x205fc: 0xe3560000,
        0x20604: 0xe3a00000, 0x20608: 0xe5c70000, 0x2060c: 0xe1a00006,
        0x2c170: 0xe1a04000, 0x2c178: 0xe1a05004, 0x2c184: 0xe5950020,
        0x26df8: 0xe3a020ec, 0x26e00: 0xe28400a0, 0x26e78: 0xcfc04,
        0x22dc8: 0xe1a04003, 0x22dec: 0xe1a00004, 0x7580: 0xe3a00201,
        0x28204: 0xe3a03009, 0x28208: 0xe2842068, 0x28210: 0xe1a00003,
        0x6ea4: 0xe3500020, 0x6eb8: 0xe0800080, 0x6ebc: 0xe7841100,
        0x6ec0: 0xe0840100, 0x6ec4: 0xe5802004, 0x6ec8: 0xe5803008,
        0x6f00: 0xe3170c02, 0x6f8c: 0xe117041a,
        0x6f94: 0xe0848084, 0x6f98: 0xe0896108,
        0x6f9c: 0xe5961008, 0x6fa0: 0xe3510000, 0x6fa8: 0xe7992108,
        0x6fac: 0xe5960004, 0x6fb0: 0xe12fff32,
        0x6fd8: 0xe7992108, 0x6fdc: 0xe5960004, 0x6fe0: 0xe12fff32,
    }
    ARM_BRANCHES = (
        (0x5cf8, 0x5d2c, False, 0), (0x5d4c, 0x54c, True, 14),
        (0x5d58, 0x5f08, False, 0), (0x5d78, 0x5d14, False, 14),
        (0x5f28, 0x5d04, False, 14), (0x7e0, 0xe8b0, True, 14),
        (0x7ec, 0x814, False, 0), (0x810, 0x850, False, 14),
        (0x894, 0x86c, False, 14), (0xec30, 0x28310, True, 14),
        (0xec3c, 0xec50, False, 0), (0xec44, 0xe5bc, True, 14),
        (0xec4c, 0xe8f4, False, 14), (0xec54, 0x26658, True, 14),
        (0xec60, 0xec74, False, 0), (0xec68, 0xe5bc, True, 14),
        (0xec70, 0xe8f4, False, 14), (0x266a4, 0x26358, True, 14),
        (0xec78, 0x250bc, True, 14), (0xec84, 0xec98, False, 0),
        (0xec8c, 0xe5bc, True, 14), (0xec94, 0xe8f4, False, 14),
        (0xeca0, 0x24d98, True, 14), (0xecac, 0xecc0, False, 0),
        (0xecb4, 0xe5bc, True, 14), (0xecbc, 0xe8f4, False, 14),
        (0xecc4, 0x266f0, True, 14), (0xecd0, 0xece4, False, 0),
        (0xecd8, 0xe5bc, True, 14), (0xece0, 0xe8f4, False, 14),
        (0xecec, 0xe598, True, 14), (0xecf8, 0xe8f4, False, 14),
        (0x266b0, 0x266bc, False, 0), (0x266b8, 0x2667c, False, 14),
        (0x263c8, 0x271c8, True, 14), (0x263d4, 0x263e8, False, 0),
        (0x263e4, 0x26388, False, 14), (0x271f4, 0x206e4, True, 14),
        (0x27204, 0x206e4, True, 14), (0x27254, 0x2705c, True, 14),
        (0x2728c, 0x273b4, False, 0), (0x273f8, 0x2729c, False, 14),
        (0x27088, 0x27098, False, 0), (0x270a8, 0x20690, True, 14),
        (0x270b8, 0x20708, True, 14), (0x270c8, 0x25024, True, 14),
        (0x270e0, 0x20598, True, 14), (0x270ec, 0x27100, False, 1),
        (0x270fc, 0x27094, False, 14), (0x27110, 0x25010, True, 14),
        (0x2711c, 0x271a0, False, 0), (0x2712c, 0x20708, True, 14),
        (0x2713c, 0x27158, False, 0), (0x27154, 0x27094, False, 14),
        (0x27168, 0x271b0, False, 0), (0x2719c, 0x27094, False, 14),
        (0x271ac, 0x27094, False, 14), (0x271bc, 0x20690, True, 14),
        (0x271c4, 0x27094, False, 14), (0x205b8, 0x205ec, False, 0),
        (0x205c0, 0x205dc, False, 13), (0x205f4, 0x205b4, False, 0),
        (0x20600, 0x2060c, False, 1), (0x2c188, 0x20614, True, 14),
        (0x26e04, 0x2c624, True, 14), (0x28214, 0x6ea0, True, 14),
        (0x6ea8, 0x6eb4, False, 3), (0x6f04, 0x6f1c, False, 0),
        (0x6f90, 0x6fb4, False, 0), (0x6fa4, 0x6fc8, False, 0),
        (0x6fc0, 0x6f8c, False, 3), (0x6fec, 0x6fb4, False, 14),
        (0xf0, 0x6ef0, True, 14), (0xeb88, 0x2052c, True, 14),
        (0x868, 0x22db8, True, 14),
    )
    ARM_LITERALS = (
        (0x5ce8, 0x5128, 7, 0xd1ff4), (0x5d34, 0x5ea4, 0, 0xd4164),
        (0x5d44, 0x5ed4, 1, 0xd1ff8), (0x27090, 0x272a4, 0, 0x220009),
        (0x27210, 0x2734c, 0, 0x73760001),
        (0x281d0, 0x28694, 3, 0x2c16c), (0x2820c, 0x28694, 1, 0x2c16c),
        (0x2821c, 0x28698, 1, 0x10900000), (0x6eb4, 0x7128, 4, 0xd2000),
        (0x6f20, 0x7128, 9, 0xd2000),
        (0x18, 0x34, 15, 0xdc), (0x26dfc, 0x26e78, 1, 0xcfc04),
    )
    FRESH_REGIONS = (
        ("host_init", 0x5ccc, 0x260), ("init_context", 0x54c, 0x354),
        ("controller_factory", 0xe8b0, 0x44c), ("image_initialize", 0x26658, 0x98),
        ("image_load_call", 0x26358, 0xa0), ("init_packet_builder", 0x271c8, 0x234),
        ("transport", 0x2705c, 0x16c), ("register_access", 0x25010, 0x24),
        ("event_helpers", 0x2052c, 0x17c), ("response_callback", 0x2c16c, 0x24),
        ("response_registration", 0x28194, 0xb8), ("irq_slot_registration", 0x6ea0, 0x34),
        ("irq_dispatch", 0x6ef0, 0x100), ("irq_vector", 0, 0x3c),
        ("irq_entry", 0xdc, 0x1c), ("irq_enable", 0xad44, 0xc),
        ("registration_literals", 0x28694, 8), ("irq_dispatch_literals", 0x7128, 12),
        ("base_initializer", 0x7534, 0x70), ("base_constructor", 0x1e87c, 0x28),
        ("base_constructor_arguments", 0x264, 0x10), ("register_table", 0xcfc04, 0xec),
        ("register_table_copy", 0x26dd8, 0xa4),
        ("shortcut_global_literal", 0x5128, 4), ("return_logging", 0x22db8, 0x3c),
        ("outer_init", 0x47180, 0x19c), ("outer_loop", 0x491e0, 0xac),
        ("outer_local_clear", 0x30160, 0x24), ("outer_dma_write", 0x30220, 0x4c),
        ("outer_flush", 0x32f34, 0x18), ("outer_enable_interface", 0x5d4f4, 0xa8),
        ("outer_deliver_response", 0x36c68, 0x28), ("init_symbol", 0x69e50, 0x10),
        ("command_flush_symbols", 0x6be10, 0x30), ("loop_dma_symbols", 0x6c260, 0x180),
        ("platform_symbols", 0x6cd70, 0x20), ("init_name", 0x67b07, 14),
        ("command_flush_names", 0x688a1, 0x25), ("loop_dma_names", 0x68dd9, 0x154),
        ("platform_names", 0x698a5, 0x32), ("outer_section_names", 0x79098, 0x4a7),
    )
    ARC_SYMBOLS = (
        ("CmdInitialize", 46, 16, 0x245ec, 412),
        ("Core_Command", 554, 16, 0x25808, 516),
        ("Arc_FlushWrites", 556, 4, 0x8090, 24),
        ("Core_Loop", 623, 16, 0x2664c, 172),
        ("Core_LocalClear", 640, 2, 0x52bc, 36),
        ("Dma_Sync", 644, 2, 0x5364, 24),
        ("Dma_Write", 645, 2, 0x537c, 76),
        ("Dma_Read", 646, 2, 0x53c8, 68),
        ("Platform_EnableInterface", 800, 16, 0x3a960, 168),
        ("Platform_DeliverResponse", 801, 4, 0xbdc4, 40),
    )
    ARC_WORDS = {
        2: {0x52bc: 0x9020fe02, 0x52c0: 0x5040fe01, 0x52c4: 0x57e17bff,
            0x52c8: 0x67808202, 0x52cc: 0x50410402, 0x52d0: 0x30000102,
            0x52d4: 0x10000400, 0x52d8: 0x40007e04, 0x52dc: 0x380f8000,
            0x5364: 0x603f7c00, 0x5368: 0x30051800,
            0x536c: 0x0800c040, 0x5370: 0x67e07a0f,
            0x5374: 0x380f8001, 0x5378: 0x27fffe00,
            0x537c: 0x67e10500, 0x5380: 0x380f8001,
            0x5384: 0x609f7c00, 0x5388: 0x30051800,
            0x5390: 0x6061fe03, 0x5394: 0x57e1fa03, 0x5398: 0x27fffe01,
            0x53a0: 0x67e1fa01, 0x53a4: 0x20000202,
            0x53a8: 0x14020200, 0x53ac: 0x14020004, 0x53b0: 0x14020408,
            0x53b8: 0x14020210, 0x53bc: 0x14020014, 0x53c0: 0x14020418,
            0x53c8: 0x609f7c00, 0x53cc: 0x30051800,
            0x53d4: 0x6061fe0c, 0x53d8: 0x57e1fa0c, 0x53dc: 0x27fffe01,
            0x53e4: 0x67e1fa04, 0x53e8: 0x20000202,
            0x53ec: 0x14020020, 0x53f0: 0x14020224, 0x53f4: 0x14020428,
            0x53fc: 0x14020030, 0x5400: 0x14020234, 0x5404: 0x14020438},
        4: {0x8090: 0x603f7c00, 0x8094: 0x30000f00, 0x8098: 0x0800c0d0,
            0x809c: 0x67e07a01, 0x80a0: 0x27fffe82, 0x80a4: 0x380f8000,
            0xbdc4: 0x081f0000, 0xbdc8: 0x3fffd588,
            0xbdcc: 0x60007c00, 0xbdd0: 0x03ffffff,
            0xbdd4: 0x68207c00, 0xbdd8: 0x34000000,
            0xbddc: 0x601f7c00, 0xbde0: 0x20000,
            0xbde4: 0x14008000, 0xbde8: 0x380f8000},
        16: {
            0x25824: 0x61ff7c00, 0x25828: 0x30051d00,
            0x2582c: 0x61df7c00, 0x25830: 0x70100,
            0x25834: 0x41a77f00, 0x25838: 0x60069a00, 0x2583c: 0x60279e00,
            0x25840: 0x2fbf70a0, 0x25844: 0x405ffe80, 0x25848: 0x2fbf6320,
            0x25850: 0x08078000, 0x2586c: 0x50207c00, 0x25870: 0x73760001,
            0x25874: 0x57e0fa08, 0x25878: 0x2000278d,
            0x2587c: 0x081fa000, 0x25880: 0x40007e03,
            0x25884: 0x40000200, 0x25888: 0x38000000,
            0x2588c: 0x20000400, 0x258b0: 0x2ffda720,
            0x258b4: 0x60079e00, 0x258b8: 0x20002080,
            0x24640: 0x08070004, 0x24644: 0x1fe00f00,
            0x246a8: 0x08070010, 0x246ac: 0x282c5620, 0x246b0: 0x08270014,
            0x246d0: 0x50410400, 0x246e0: 0x10070404,
            0x24780: 0x380f8020, 0x24784: 0x0b6e1018,
            0x259c0: 0x601f7c00, 0x259c4: 0x30051d00,
            0x259c8: 0x40277f00, 0x259cc: 0x2fbf35a0,
            0x259d0: 0x405ffe80, 0x259d4: 0x2fbf3180,
            0x259d8: 0x2fc4d680, 0x259dc: 0x601f7c00, 0x259e0: 0x30000f00,
            0x259e4: 0x08204080, 0x259e8: 0x14001a84, 0x259ec: 0x2fcc7a80,
            0x25a04: 0x380f8020, 0x25a08: 0x0b6e1020,
            0x26668: 0x621f7c00, 0x2666c: 0x30000f00,
            0x266c4: 0x08084088, 0x266c8: 0x67e07a01, 0x266cc: 0x20000182,
            0x266d0: 0x0806851d, 0x266d4: 0x67e00100, 0x266d8: 0x20000181,
            0x266dc: 0x50000000, 0x266e0: 0x2ffe24a0, 0x266e4: 0x1046811d,
            0x3a97c: 0x60400100, 0x3a980: 0x61e08200,
            0x3a984: 0x61df7c00, 0x3a988: 0x3fffd688,
            0x3a98c: 0x20000141, 0x3a990: 0x08470104, 0x3a994: 0x10070504,
            0x3a9ac: 0x67e79f00, 0x3a9b0: 0x20000141,
            0x3a9b4: 0x09e70100, 0x3a9b8: 0x10071f00,
            0x3aa00: 0x380f8020, 0x3aa04: 0x0b6e101c,
        },
    }
    # Nine section16 RELA call records: includes the two bridge prerequisites
    # and new request/response/control-flow edges. PC bias is4, not ARM's8.
    ARC_CALLS = (
        (0x72930, 0x2461c, 640, "Core_LocalClear", 2, 0x52bc, 0x2fc193a0),
        (0x7293c, 0x246ac, 800, "Platform_EnableInterface", 16, 0x3a960, 0x282c5620),
        (0x72f0c, 0x25840, 646, "Dma_Read", 2, 0x53c8, 0x2fbf70a0),
        (0x72f18, 0x25848, 644, "Dma_Sync", 2, 0x5364, 0x2fbf6320),
        (0x72f90, 0x259cc, 645, "Dma_Write", 2, 0x537c, 0x2fbf35a0),
        (0x72f9c, 0x259d4, 644, "Dma_Sync", 2, 0x5364, 0x2fbf3180),
        (0x72fa8, 0x259d8, 556, "Arc_FlushWrites", 4, 0x8090, 0x2fc4d680),
        (0x72fb4, 0x259ec, 801, "Platform_DeliverResponse", 4, 0xbdc4, 0x2fcc7a80),
        (0x733a4, 0x266e0, 554, "Core_Command", 16, 0x25808, 0x2ffe24a0),
    )

    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]
        cls.elf_base = 0x2ea60
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", cls.payload, cls.elf_base)
        cls.sections = [struct.unpack_from("<10I", cls.payload, cls.elf_base + header[6] + i * 40)
                        for i in range(header[12])]

    def word(self, offset):
        return struct.unpack_from("<I", self.payload, offset)[0]

    def arc_offset(self, section, address):
        record = self.sections[section]
        self.assertTrue(record[3] <= address <= record[3] + record[5] - 4)
        return self.elf_base + record[4] + address - record[3]

    def mapping(self, payload=None, images=None):
        return MAP._fresh_init_causal_contract(self.payload if payload is None else payload,
                                             self.images if images is None else images)

    @staticmethod
    def immediate(word):
        byte, shift = word & 255, ((word >> 8) & 15) * 2
        return ((byte >> shift) | (byte << (32 - shift))) & 0xffffffff

    def test_independent_arm_predicate_argument_status_and_return_words(self):
        for offset, word in self.ARM_WORDS.items():
            with self.subTest(offset=hex(offset)):
                self.assertEqual(self.word(offset), word)
        for offset, target, link, condition in self.ARM_BRANCHES:
            word = self.word(offset)
            displacement = word & 0xffffff
            if displacement >= 0x800000:
                displacement -= 0x1000000
            with self.subTest(branch=hex(offset)):
                self.assertEqual(word >> 25 & 7, 5)
                self.assertEqual((bool(word >> 24 & 1), word >> 28), (link, condition))
                self.assertEqual(offset + 8 + 4 * displacement, target)
        # Operand receipts, not just a hash of surrounding code: both packet
        # buffers are zeroed/copied252 bytes, command/status are words0/1,
        # event and busy state are separate byte operations at C+88/C+8c.
        self.assertEqual([self.immediate(self.word(p)) for p in (0x271e8, 0x271f8, 0x270ac, 0x27120)],
                         [252] * 4)
        self.assertEqual([self.word(p) & 0xfff for p in (0x27080, 0x2709c, 0x270f4, 0x27144, 0x27170, 0x271b4)],
                         [0x8c] * 6)
        self.assertTrue(all(self.word(p) >> 22 & 1 for p in (0x27080, 0x2709c, 0x20698, 0x2061c)))
        self.assertEqual([self.word(p) & 0xfff for p in (0x270a4, 0x270dc, 0x271b8)], [0x88] * 3)
        self.assertEqual((self.word(0x2723c) & 0xfff) | (self.word(0x2723c) >> 4 & 0xf000), 20000)
        self.assertEqual((self.word(0x2734c), self.word(0x272a4)), (0x73760001, 0x220009))
        self.assertEqual(self.word(0x263bc) & 0xfff, 0x30)
        self.assertEqual(self.word(0x27218) & 0xfff, 4)
        self.assertEqual(self.word(0x27218) >> 12 & 15, 9)  # Saved byte argument, not a transaction ID.
        self.assertEqual(self.word(0x27160) & 0xfff, 4)
        self.assertEqual(self.word(0x271c0) & 15, 9)  # Preserve original wait return.
        for instruction, literal, register, value in self.ARM_LITERALS:
            word = self.word(instruction)
            with self.subTest(literal_instruction=hex(instruction)):
                self.assertEqual(word >> 16 & 15, 15)
                self.assertEqual(word >> 12 & 15, register)
                displacement = word & 0xfff
                self.assertEqual(instruction + 8 + (displacement if word >> 23 & 1 else -displacement), literal)
                self.assertEqual(self.word(literal), value)

    def test_independent_manifest_hashes_sizes_and_every_oracle_site_coverage(self):
        self.assertEqual([(role, offset, size) for role, offset, size, _ in MAP._FRESH_INIT_REGIONS],
                         list(self.FRESH_REGIONS))
        self.assertEqual((len(self.FRESH_REGIONS), sum(size for _, _, size in self.FRESH_REGIONS)), (41, 8536))
        for role, offset, size, digest in MAP._FRESH_INIT_REGIONS:
            with self.subTest(role=role):
                self.assertEqual(hashlib.sha256(self.payload[offset:offset + size]).hexdigest(), digest)
        pins = [(offset, size) for _, offset, size in self.FRESH_REGIONS]
        pins += [(offset, len(encoded) // 2) for _, offset, encoded in MAP._COMMAND_BUFFER_BRIDGE_REGIONS]
        pins.append((0x72780, 26052))
        sites = list(self.ARM_WORDS)
        sites += [offset for offset, _, _, _ in self.ARM_BRANCHES]
        sites += [p for instruction, literal, _, _ in self.ARM_LITERALS for p in (instruction, literal)]
        sites += [self.arc_offset(section, address) for section, words in self.ARC_WORDS.items() for address in words]
        sites += [position for position, *_ in self.ARC_CALLS]
        for offset in sites:
            with self.subTest(oracle_offset=hex(offset)):
                self.assertTrue(any(start <= offset and offset + 4 <= start + size for start, size in pins))
        self.assertEqual((MAP.MAX_FRESH_INIT_REGIONS, MAP.MAX_FRESH_INIT_BYTES,
                          MAP.MAX_FRESH_INIT_AGGREGATE_BYTES, MAP.MAX_FRESH_INIT_ANCHORS,
                          MAP.MAX_FRESH_INIT_EVENTS), (48, 16 * 1024, 80 * 1024, 160, 64))
        self.assertEqual((MAP.MAX_COMMAND_BUFFER_BRIDGE_REGIONS, MAP.MAX_COMMAND_BUFFER_BRIDGE_BYTES,
                          MAP.MAX_STOCK_HOST_COMMAND_REGIONS, MAP.MAX_STOCK_HOST_COMMAND_BYTES),
                         (80, 40 * 1024, 16, 16 * 1024))

    def test_independent_arc_symbols_section_mappings_and_delayed_arguments(self):
        strings = self.sections[34]
        names = self.payload[self.elf_base + strings[4]:self.elf_base + strings[4] + strings[5]]
        for name, index, section, address, size in self.ARC_SYMBOLS:
            position = self.elf_base + self.sections[35][4] + 16 * index
            entry = struct.unpack_from("<IIIBBH", self.payload, position)
            end = names.index(0, entry[0])
            with self.subTest(symbol=name):
                self.assertEqual(names[entry[0]:end].decode(), name)
                self.assertEqual((entry[1], entry[2], entry[3] & 15, entry[5]), (address, size, 2, section))
                self.assertEqual(self.arc_offset(section, address),
                                 address + {2: 0x2aea4, 4: 0x2aea4, 16: 0x22b94}[section])
        for section, words in self.ARC_WORDS.items():
            for address, word in words.items():
                with self.subTest(section=section, address=hex(address)):
                    self.assertEqual(self.word(self.arc_offset(section, address)), word)
        # .d argument slots execute on the call; .jd slots execute only on
        # the taken branch. Zero Platform_EnableInterface args load defaults,
        # while nonzero args store the supplied registration values instead.
        self.assertEqual(self.word(self.arc_offset(16, 0x258b4)), 0x60079e00)
        self.assertEqual(self.word(self.arc_offset(16, 0x246b0)), 0x08270014)
        self.assertEqual(self.word(self.arc_offset(16, 0x259d0)), 0x405ffe80)
        self.assertEqual(self.word(self.arc_offset(16, 0x266e4)), 0x1046811d)
        for branch, target in ((0x3a98c, 0x3a998), (0x3a9b0, 0x3a9bc)):
            word = self.word(self.arc_offset(16, branch))
            self.assertEqual(word & 63, 1)
            self.assertEqual(word >> 5 & 3, 2)  # Taken-only delay, not ordinary.d.
            self.assertEqual(branch + 4 + ((word >> 7) & 0xfffff) * 4, target)

    def test_independent_arc_relocation_symbol_addend_pc_bias_and_call_delay(self):
        for position, source, index, name, section, target, original in self.ARC_CALLS:
            with self.subTest(target=name, source=hex(source)):
                self.assertEqual(struct.unpack_from("<IIi", self.payload, position),
                                 (source, index * 256 + 6, 0))
                entry = struct.unpack_from("<IIIBBH", self.payload,
                                           self.elf_base + self.sections[35][4] + 16 * index)
                self.assertEqual((entry[1], entry[5]), (target, section))
                self.assertEqual(self.word(self.arc_offset(16, source)), original)
                delta = target - source - 4
                self.assertEqual(delta % 4, 0)
                self.assertTrue(-(1 << 21) <= delta < (1 << 21))
                patched = (original & 0xf800007f) | ((delta * 32) & 0x07ffff80)
                self.assertEqual(patched, original)
                displacement = original >> 7 & 0xfffff
                if displacement >= 0x80000:
                    displacement -= 0x100000
                self.assertEqual(source + 4 + displacement * 4, target)
                # Ordinary.d only when low delay field=1. Dma_Sync/flush/
                # delivery return edges have no manufactured argument slot.
                self.assertEqual(original >> 5 & 3,
                                 1 if source in (0x2461c, 0x246ac, 0x25840, 0x25848, 0x259cc, 0x266e0) else 0)

    @staticmethod
    def arc_branch_target(address, word):
        displacement = word >> 7 & 0xfffff
        if displacement >= 0x80000:
            displacement -= 0x100000
        return address + 4 + displacement * 4

    def test_irq_slot_trigger_dispatch_and_native_argument_operand_semantics(self):
        # Native request word1 is a byte widened through R2 -> saved R9.
        self.assertEqual((self.word(0x263bc) >> 16 & 15, self.word(0x263bc) >> 12 & 15,
                          bool(self.word(0x263bc) >> 22 & 1), self.word(0x263bc) & 4095), (4, 2, True, 0x30))
        self.assertEqual((self.word(0x271d8) >> 12 & 15, self.word(0x271d8) & 15), (9, 2))
        self.assertEqual((self.word(0x27218) >> 12 & 15, bool(self.word(0x27218) >> 22 & 1),
                          self.word(0x27218) & 4095), (9, False, 4))
        # Register callback literal and userdata; slot address is12*IRQ, not
        # a byte index or the generic callback table selector at C+cc.
        self.assertEqual(self.immediate(self.word(0x28204)), 9)
        self.assertEqual((self.word(0x28210) >> 12 & 15, self.word(0x28210) & 15), (0, 3))
        self.assertEqual((self.word(0x28208) >> 16 & 15, self.word(0x28208) >> 12 & 15,
                          self.immediate(self.word(0x28208))), (4, 2, 0x68))
        self.assertEqual((self.word(0x6eb8) >> 16 & 15, self.word(0x6eb8) >> 12 & 15,
                          self.word(0x6eb8) & 15, self.word(0x6eb8) >> 7 & 31), (0, 0, 0, 1))
        for site, base, data, offset, shift in ((0x6ebc, 4, 1, 0, 2), (0x6fa8, 9, 2, 8, 2)):
            word = self.word(site)
            self.assertEqual((word >> 16 & 15, word >> 12 & 15, word & 15, word >> 7 & 31),
                             (base, data, offset, shift))
            self.assertEqual(word >> 5 & 3, 0)
        self.assertEqual((self.word(0x6ec0) >> 16 & 15, self.word(0x6ec0) >> 12 & 15,
                          self.word(0x6ec0) & 15, self.word(0x6ec0) >> 7 & 31), (4, 0, 0, 2))
        for site, rd, offset in ((0x6ec4, 2, 4), (0x6ec8, 3, 8), (0x6fac, 0, 4)):
            self.assertEqual((self.word(site) >> 12 & 15, self.word(site) & 4095), (rd, offset))
        self.assertEqual((self.word(0x6f94) >> 12 & 15, self.word(0x6f94) >> 16 & 15,
                          self.word(0x6f94) & 15, self.word(0x6f94) >> 7 & 31), (8, 4, 4, 1))
        self.assertEqual((self.word(0x6f98) >> 12 & 15, self.word(0x6f98) >> 16 & 15,
                          self.word(0x6f98) & 15, self.word(0x6f98) >> 7 & 31), (6, 9, 8, 2))
        self.assertEqual(self.word(0x6fb0), 0xe12fff32)  # Same callback R2 loaded above.
        self.assertEqual((self.word(0x2c170) >> 12 & 15, self.word(0x2c170) & 15,
                          self.word(0x2c178) >> 12 & 15, self.word(0x2c178) & 15), (4, 0, 5, 4))
        self.assertEqual(0x68 + (self.word(0x2c184) & 4095), 0x88)
        # Selected bit-set trigger and the INIT jump-table entry. Legacy
        # STATUS-PC word-address semantics remain an explicit assumption.
        branch = self.word(self.arc_offset(16, 0x266cc))
        self.assertEqual((branch & 31, branch >> 5 & 3, self.arc_branch_target(0x266cc, branch)), (2, 0, 0x266dc))
        self.assertEqual(self.word(self.arc_offset(16, 0x25870)), 0x73760001)
        self.assertEqual(self.word(self.arc_offset(16, 0x25874)) & 511, 8)
        outside = self.word(self.arc_offset(16, 0x25878))
        self.assertEqual((outside & 31, self.arc_branch_target(0x25878, outside)), (13, 0x259b8))
        self.assertEqual(self.word(self.arc_offset(16, 0x25880)) & 511, 3)
        self.assertEqual(self.arc_branch_target(0x2588c, self.word(self.arc_offset(16, 0x2588c))), 0x258b0)
        self.assertEqual((0x73760001 - self.word(self.arc_offset(16, 0x25870))) & 0xffffffff, 0)
        # Nonzero response argument R1 is saved inR15 and skips the taken-only
        # default-load slot. The following store uses signed9=-256.
        word = self.word(self.arc_offset(16, 0x3a980))
        self.assertEqual((word >> 21 & 63, word >> 9 & 63), (15, 1))
        word = self.word(self.arc_offset(16, 0x3a9b8))
        signed9 = (word & 511) - 512 if word & 256 else word & 511
        self.assertEqual((word >> 15 & 63, word >> 9 & 63, signed9), (14, 15, -256))
        self.assertEqual(self.word(self.arc_offset(16, 0x3a988)) + signed9, 0x3fffd588)

    def test_decoded_receipts_bind_every_anchor_to_independent_instruction_operands(self):
        result = self.mapping()
        branches = {p: (target, link, condition) for p, target, link, condition in self.ARM_BRANCHES}
        literals = {p: (literal, rd, value) for p, literal, rd, value in self.ARM_LITERALS}
        seen = set()
        for receipt in result["instruction_anchors"]:
            if receipt["architecture"] != "ARM":
                continue
            p, word = receipt["blob_file_offset"], receipt["word"]
            self.assertNotIn(p, seen)
            seen.add(p)
            with self.subTest(arm=hex(p)):
                if p in branches:
                    target, link, condition = branches[p]
                    self.assertEqual((receipt["target_blob_file_offset"], receipt["operation"], receipt["condition"]),
                                     (target, "BL" if link else "B", condition))
                elif p in literals:
                    literal, rd, value = literals[p]
                    self.assertEqual((receipt["literal_blob_file_offset"], receipt["destination_register"],
                                      receipt["literal_value"]), (literal, rd, value))
                else:
                    self.assertEqual(word, self.ARM_WORDS[p])
                    self.assertEqual(receipt["condition"], word >> 28)
                    if p == 0x6fb0:
                        self.assertEqual((receipt["operation"], receipt["operand_register"]), ("BLX register", 2))
                    elif word >> 26 & 3 == 1:
                        self.assertEqual((receipt["operation"], receipt["base_register"], receipt["data_register"],
                                          receipt["byte_width"]),
                                         ("LDR" if word >> 20 & 1 else "STR", word >> 16 & 15,
                                          word >> 12 & 15, 1 if word >> 22 & 1 else 4))
                        if word >> 25 & 1:
                            self.assertEqual((receipt["offset_register"], receipt["shift_kind"], receipt["shift_amount"]),
                                             (word & 15, "LSL", word >> 7 & 31))
                        else:
                            self.assertEqual(receipt["byte_offset"], (word & 4095) * (1 if word >> 23 & 1 else -1))
                    elif p == 0x2723c:
                        self.assertEqual((receipt["operation"], receipt["destination_register"], receipt["immediate"]),
                                         ("MOVW", 3, 20000))
                    else:
                        self.assertEqual(receipt["source_register"], word >> 16 & 15)
                        self.assertEqual(receipt["destination_register"], word >> 12 & 15)
                        self.assertEqual(receipt["operation"], {4: "ADD", 10: "CMP", 13: "MOV", 15: "MVN"}[word >> 21 & 15])
                        if word >> 25 & 1:
                            self.assertEqual(receipt["immediate"], self.immediate(word))
                        else:
                            self.assertEqual(receipt["operand_register"], word & 15)
                            self.assertEqual((receipt["shift_kind"], receipt["shift_amount"]), ("LSL", word >> 7 & 31))
        for receipt in result["outer_path"]["operands"]:
            section, address = receipt["section_index"], receipt["elf_virtual_address"]
            word = self.ARC_WORDS[section][address]
            with self.subTest(arc=hex(address)):
                self.assertEqual((receipt["blob_file_offset"], receipt["word"], receipt["destination_register"],
                                  receipt["source_register"], receipt["operand_register"], receipt["low9"]),
                                 (self.arc_offset(section, address), word, word >> 21 & 63,
                                  word >> 15 & 63, word >> 9 & 63, word & 511))
                self.assertTrue(receipt["decode_conditional"])
                self.assertEqual(receipt["signed_low9"], (word & 511) - 512 if word & 256 else word & 511)
                if "literal_value" in receipt:
                    self.assertEqual(receipt["literal_value"], self.ARC_WORDS[section][address + 4])
                if "target_elf_virtual_address" in receipt:
                    self.assertEqual(receipt["target_elf_virtual_address"], self.arc_branch_target(address, word))
                    self.assertEqual(receipt["condition"], word & 31)
                    self.assertEqual(receipt["delay_slot_semantics"],
                                     "taken only" if word >> 5 & 3 == 2 else "always executed" if word >> 5 & 3 == 1 else "none")
        # The selected same-section INIT branch has no invented RELA record.
        expected = {source: (position, index, name, section, target, word)
                    for position, source, index, name, section, target, word in self.ARC_CALLS}
        expected[0x258b0] = (None, 46, "CmdInitialize", 16, 0x245ec, 0x2ffda720)
        self.assertEqual(set(r["source_elf_virtual_address"] for r in result["relocation_receipts"]), set(expected))
        for receipt in result["relocation_receipts"]:
            source = receipt["source_elf_virtual_address"]
            position, index, name, section, target, word = expected[source]
            with self.subTest(edge=hex(source)):
                self.assertEqual((receipt["relocation_record_blob_file_offset"], receipt["symbol_index"],
                                  receipt["callee"], receipt["target_section_index"],
                                  receipt["target_elf_virtual_address"], receipt["original_word"]),
                                 (position, index, name, section, target, word))
                self.assertEqual((receipt["source_section_index"], receipt["pc_bias_bytes"],
                                  receipt["addend"], receipt["vendor_type"], receipt["patched_word"],
                                  receipt["decoded_target_elf_virtual_address"]),
                                 (16, 4, 0, 6 if position else None, word, target))
                self.assertEqual(receipt["signed_byte_displacement"], target - source - 4)
                self.assertEqual((receipt["preserved_mask"], receipt["displacement_mask"]), (0xf800007f, 0x07ffff80))
                self.assertEqual(receipt["instruction_blob_file_offset"], self.arc_offset(16, source))
                delayed = bool(word & 32)
                self.assertEqual(receipt["delay_slot"], delayed)
                self.assertEqual(receipt["delay_slot_elf_virtual_address"], source + 4 if delayed else None)
                self.assertEqual(receipt["delay_slot_word"], self.word(self.arc_offset(16, source + 4)) if delayed else None)
                self.assertEqual(receipt["delay_slot_semantics"], "always executed" if delayed else "none")
                self.assertFalse(receipt["runtime_observed"])
        self.assertEqual(result["validation"]["causal_anchor_count"], len(result["instruction_anchors"]))
        self.assertEqual(len(result["instruction_anchors"]), 159)
        self.assertEqual((len(seen), len(result["outer_path"]["operands"]), len(result["relocation_receipts"])), (120, 29, 10))

    def test_transport_event_host_and_outer_contract_fields_have_independent_oracles(self):
        result = self.mapping()
        t, h, b, e, o = (result[key] for key in ("transport", "host_init", "arm_builder", "event_path", "outer_path"))
        expected = {"busy_context_offset": 0x8c, "busy_status": 0x220009, "event_context_offset": 0x88,
                    "copy_bytes": 252, "packet_virtual_context_offset": 0x94, "packet_physical_context_offset": 0x1cc,
                    "publication_register_context_offset": 0x118, "completion_register_context_offset": 0x114,
                    "timeout_argument": 20000, "timeout_status": 5, "zero_completion_status": 9,
                    "command_mismatch_status": 2, "backend_error_status": 2,
                    "command_word_offset": 0, "backend_status_word_offset": 4}
        self.assertEqual({key: t[key] for key in expected}, expected)
        self.assertEqual(t["native_wait_statuses"], [0, 5])
        self.assertTrue(t["success_returns_wait_status"])
        self.assertTrue(t["zero_completion_preserves_busy"])
        self.assertTrue(t["native_wait_success_consumes_event"])
        self.assertEqual(t["busy_acquire_value"], 1)
        self.assertEqual(t["busy_free_value"], 0)
        self.assertEqual(t["busy_acquire_offset"], 0x8c)
        self.assertEqual(t["busy_clear_offsets"], dict.fromkeys(("timeout", "command_mismatch", "backend_status", "success"), 0x8c))
        self.assertEqual(t["predicates"], {"busy_equals": 0, "timeout_equals": 5, "completion_not_equals": 0,
                                           "command_compare_registers": [0, 1], "backend_status_equals": 0})
        self.assertEqual(t["order"], ["busy_check", "event_reset", "request_copy", "request_publication", "event_wait",
                                       "completion_read", "reply_copy", "command_check", "backend_status_check", "success_reset"])
        self.assertEqual((h["command"], h["internal_command"], h["shortcut_global_address"]),
                         (0x73763001, 0x73760001, 0xd1ff4))
        self.assertEqual((h["failure_reply_status"], h["success_reply_status"], h["success_return"],
                          h["sequence_request_offset"], h["sequence_reply_offset"], h["status_reply_offset"]),
                         (0xffffffff, 0, 0, 4, 4, 8))
        self.assertEqual((b["request_word1_offset"], b["request_word1_register"],
                          b["response_target_context_offset"], b["response_target_word_offset"], b["timeout_argument"]),
                         (4, 9, 0xa8, 20, 20000))
        self.assertEqual((b["request_word1_source_width"], b["request_word1_source_context_offset"],
                          b["request_word1_native_max"]), (1, 0x30, 255))
        self.assertEqual((e["event_context_offset"], e["callback_userdata_context_offset"],
                          e["callback_event_offset_from_userdata"], e["irq_slot"], e["event_set_value"],
                          e["event_reset_value"], e["event_byte_width"], e["irq_vector_entry"], e["irq_dispatch_entry"]),
                         (0x88, 0x68, 32, 9, 1, 0, 1, 0xdc, 0x6ef0))
        self.assertFalse(e["event_generation_check"])
        self.assertEqual(e["arm_mmio_base"], 0x10000000)
        table = e["register_table"]
        self.assertEqual({name: (row["context_offset"], row["table_blob_file_offset"], row["value"])
                          for name, row in table.items()},
                         {"init_response_target": (0xa8, 0xcfc0c, 0x900004),
                          "generic_callback_selector": (0xcc, 0xcfc30, 0x4800002),
                          "completion": (0x114, 0xcfc78, 0x800f84),
                          "publication": (0x118, 0xcfc7c, 0x800f80)})
        self.assertEqual((o["internal_command"], o["local_packet_address"], o["local_mailbox_base"],
                          o["trigger_offset"], o["trigger_bit_mask"], o["reply_mailbox_offset"],
                          o["init_response_word_offset"], o["reply_backend_status_offset"], o["reply_backend_status"]),
                         (0x73760001, 0x30051d00, 0x30000f00, 136, 1, 132, 20, 4, 0))
        self.assertEqual((o["response_target_storage"], o["response_address_mask"],
                          o["response_address_prefix"], o["response_irq_bits"]),
                         (0x3fffd588, 0x03ffffff, 0x34000000, 0x20000))
        self.assertEqual(o["transport_edges"], ["Dma_Read", "Dma_Sync", "CmdInitialize", "Dma_Write", "Dma_Sync",
                                                "Arc_FlushWrites", "Platform_DeliverResponse"])
        self.assertTrue(o["ordinary_call_delay_slots_always_execute"])
        self.assertTrue(o["conditional_BZ_jd_slot_executes_only_when_taken"])
        self.assertTrue(o["dma_completion_and_visibility_assumed"])
        wanted = [("host", 0x5d4c, 6, 0x5d50, 0x5d54, 0x5d58, 0x5f08, 0x5d74),
                  ("context", 0x7e0, 5, 0x7e4, 0x7e8, 0x7ec, 0x814, None),
                  ("factory", 0xec54, 5, 0xec58, 0xec5c, 0xec60, 0xec74, 0xec6c),
                  ("image", 0x266a4, 4, 0x266a8, 0x266ac, 0x266b0, 0x266bc, 0x266b4),
                  ("loader", 0x263c8, 5, 0x263cc, 0x263d0, 0x263d4, 0x263e8, 0x263e0),
                  ("builder", 0x27254, 10, 0x27258, 0x27288, 0x2728c, 0x273b4, 0x27298)]
        fields = ("caller", "call_blob_file_offset", "saved_register", "save_blob_file_offset", "compare_blob_file_offset",
                  "success_branch_blob_file_offset", "success_blob_file_offset", "error_return_blob_file_offset")
        self.assertEqual([tuple(row[field] for field in fields) for row in result["checked_return_chain"]], wanted)
        self.assertTrue(all(row["success_compare_value"] == 0 and row["error_preserves_result"]
                            for row in result["checked_return_chain"]))

    def test_decoded_trigger_dispatch_interface_and_callback_linkage_receipts(self):
        result = self.mapping()
        outer, event = result["outer_path"], result["event_path"]
        self.assertEqual(outer["trigger"], {"base_address": 0x30000f00, "register_offset": 136, "bit_mask": 1,
                                            "condition": 2, "branch_target_elf_virtual_address": 0x266dc,
                                            "call_elf_virtual_address": 0x266e0,
                                            "pending_byte_alternative_validated": False})
        dispatch = outer["init_dispatch"]
        expected = {"command_base": 0x73760001, "command_index": 0, "maximum_index": 8, "range_condition": 13,
                    "default_target_elf_virtual_address": 0x259b8, "status_auxiliary_register": 0,
                    "status_pc_next_elf_virtual_address": 0x25880, "table_bias_words": 3,
                    "index_register": 1, "jump_register": 0, "selected_entry_elf_virtual_address": 0x2588c,
                    "selected_target_elf_virtual_address": 0x258b0, "status_pc_word_address_semantics_assumed": True}
        self.assertEqual(dispatch, expected)
        self.assertEqual(dispatch["status_pc_next_elf_virtual_address"] +
                         4 * (dispatch["table_bias_words"] + dispatch["command_index"]), dispatch["selected_entry_elf_virtual_address"])
        interface = outer["enable_interface"]
        self.assertEqual(interface, {"argument_register": 1, "saved_register": 15, "argument_value": 0x900004,
                                     "zero_branch_condition": 1, "zero_branch_target_elf_virtual_address": 0x3a9bc,
                                     "delay_slot_semantics": "taken only", "old_value_loaded_only_for_zero_argument": True,
                                     "store_signed_offset": -256, "storage_address": 0x3fffd588,
                                     "nonzero_argument_stored": True, "first_argument_branch_validated": False})
        self.assertEqual(event["registration"], {"callback_address": 0x2c16c, "table_address": 0xd2000,
                                                 "slot": 9, "slot_stride": 12, "slot_address": 0xd206c,
                                                 "callback_word_offset": 0, "userdata_word_offset": 4,
                                                 "flag_word_offset": 8, "flag_value": 9,
                                                 "flag_nonzero_selects_direct_callback": True,
                                                 "callback_load_register": 2, "callback_branch_register": 2,
                                                 "userdata_context_offset": 0x68, "event_offset_from_userdata": 0x20,
                                                 "event_context_offset": 0x88,
                                                 "requires_irq_slot_pending_and_table_preserved": True})
        shortcut = result["host_init"]["shortcut_branch_receipt"]
        self.assertEqual((shortcut["blob_file_offset"], shortcut["condition"], shortcut["target_blob_file_offset"]),
                         (0x5cf8, 0, 0x5d2c))
        self.assertEqual(result["host_init"]["shortcut_branch_receipt_scope"], "independently bounded bridge")
        self.assertIn("STATUS exposing the next PC in word-address units", " ".join(result["assumptions"]))

    def test_exact_and_one_over_preflight_dependency_budgets(self):
        # Tighten each cap to actual use, then one below. Do not raise any
        # existing bridge/stock budget merely to make the new receipt fit.
        for name, use in (("MAX_FRESH_INIT_REGIONS", 41), ("MAX_FRESH_INIT_BYTES", 8536),
                          ("MAX_FRESH_INIT_AGGREGATE_BYTES", 45228), ("MAX_FRESH_INIT_ANCHORS", 159),
                          ("MAX_COMMAND_BUFFER_BRIDGE_REGIONS", 80), ("MAX_COMMAND_BUFFER_BRIDGE_BYTES", 36692),
                          ("MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS", 2171),
                          ("MAX_STOCK_HOST_COMMAND_CFG_STATES", 53), ("MAX_FRESH_INIT_EVENTS", 35)):
            with self.subTest(exact=name), mock.patch.object(MAP, name, use):
                self.assertEqual(self.mapping()["host_init"]["handler_count"], 1)
            with self.subTest(one_over=name), mock.patch.object(MAP, name, use - 1), \
                    mock.patch.object(MAP, "_command_buffer_bridge_map", side_effect=AssertionError("preflight too late")), \
                    mock.patch.object(MAP, "_stock_host_handler_footprints", side_effect=AssertionError("INIT decode before preflight")), \
                    self.assertRaisesRegex(MAP.FormatError, "budget"):
                self.mapping()
        for name in ("MAX_FRESH_INIT_EVENTS", "MAX_STOCK_HOST_COMMAND_CFG_STATES"):
            with self.subTest(name=name), mock.patch.object(MAP, name, 0), \
                    mock.patch.object(MAP, "_command_buffer_bridge_map", side_effect=AssertionError("preflight too late")), \
                    self.assertRaisesRegex(MAP.FormatError, "budget"):
                self.mapping()

    def test_init_only_receipt_purity_json_roundtrip_and_scope_assumptions(self):
        before = bytes(self.payload), json.dumps(self.images, sort_keys=True)
        with mock.patch.object(MAP, "_stock_host_command_closure", side_effect=AssertionError("not INIT-only")), \
                mock.patch.object(MAP, "_stock_host_dispatch_domains", side_effect=AssertionError("selector invoked")), \
                mock.patch.object(MAP, "read_firmware", side_effect=AssertionError("external read")), \
                mock.patch("subprocess.run", side_effect=AssertionError("external decoder")):
            result = self.mapping()
        self.assertEqual(before, (bytes(self.payload), json.dumps(self.images, sort_keys=True)))
        self.assertEqual(result, json.loads(json.dumps(result, sort_keys=True)))
        self.assertEqual((result["validation"]["additional_region_count"], result["validation"]["additional_byte_count"],
                          result["validation"]["aggregate_byte_count"], result["validation"]["bridge_region_count"],
                          result["validation"]["bridge_byte_count"], result["validation"]["bridge_anchor_count"]),
                         (41, 8536, 45228, 80, 36692, 52))
        self.assertEqual([(r["role"], r["blob_file_offset"], r["size"])
                          for r in result["validation"]["validated_regions"]], list(self.FRESH_REGIONS))
        h = result["host_init"]
        self.assertEqual(h["handler_count"], 1)
        self.assertFalse(h["full_stock_selector_invoked"])
        footprint = h["stock_init_receipt"]
        self.assertEqual(footprint["request_reads"], [{"byte_offset": 4, "width": 4}])
        self.assertEqual(footprint["reply_writes"], [{"word_index": 1, "byte_offset": 4, "width": 4},
                                                    {"word_index": 2, "byte_offset": 8, "width": 4}])
        self.assertEqual(footprint["packet_header_reads"], [])
        self.assertEqual(footprint["packet_header_writes"], [])
        self.assertTrue(result["basis"]["conditional"])
        self.assertEqual(result["basis"]["baseline_firmware_sha256"], hashlib.sha256(self.data).hexdigest())
        self.assertTrue(result["basis"]["selected_regions_validated"])
        self.assertFalse(result["basis"]["entire_payload_rehashed"])
        self.assertNotIn("firmware_sha256", result["basis"])
        self.assertFalse(result["basis"]["device_observed"])
        self.assertFalse(result["basis"]["public_route"])
        scope = result["validation_scope"]
        self.assertTrue(scope["fresh_init_only"])
        self.assertTrue(scope["conditional_software_chain"])
        for name in ("full_stock_selector", "hardware_aliasing_proven", "runtime_transaction_acknowledged", "source_plane_lease",
                     "active_decode_context", "standalone_execution", "whole_firmware_relocation_closure"):
            self.assertFalse(scope[name])
        assumptions = " ".join(result["assumptions"]).lower()
        for phrase in ("serialized", "opaque callees", "saved registers", "vendor isa", "hardware routing",
                       "dma visibility", "cache coherence", "delayed old callback", "freshness assumption"):
            self.assertIn(phrase, assumptions)
        self.assertIn("logging/yield", assumptions)
        self.assertIn("not all setup helper return values are checked", assumptions)
        self.assertFalse(scope["opaque_setup_return_values_all_checked"])
        self.assertTrue(scope["transport_zero_requires_successful_post_transport_setup"])

    @staticmethod
    def raw_transport_return(busy, wait, mailbox, command, backend_status):
        # Independent predicates from 27080/270e8/27118/27138/27164 and
        # return MOVs. This is the transport boundary, not an emulator or a
        # claim that later opaque initialization/cleanup preserves live state.
        if busy:
            return 0x220009, True
        if wait == 5:
            return 5, False
        if mailbox == 0:
            return 9, True
        if command != 0x73760001 or backend_status != 0:
            return 2, False
        return wait, False

    def test_checked_return_chain_all_predicate_paths_and_explicit_fault_relaxations(self):
        contract = self.mapping()
        frozen = json.dumps(contract, sort_keys=True)
        for busy in (False, True):
            for wait in (0, 5, 2, 0xffffffff):
                for mailbox in (0, 1, 0x12345678, 0xffffffff):
                    for command in (0x73760001, 0, 0xffffffff):
                        for status in (0, 1, 0xffffffff):
                            scenario = {"busy": busy, "wait_status": wait, "completion_mailbox": mailbox,
                                        "reply_command": command, "reply_status": status}
                            result = MAP._fresh_init_projection(contract, scenario)
                            expected, busy_after = self.raw_transport_return(busy, wait, mailbox, command, status)
                            with self.subTest(**scenario):
                                self.assertEqual((result["transport_status"], result["host_return"], result["host_reply_status"]),
                                                 (expected, expected, 0 if expected == 0 else 0xffffffff))
                                self.assertEqual(result["busy_after"], busy_after)
                                self.assertEqual(result["busy_after_scope"], "transport_return_boundary")
                                self.assertTrue(result["post_transport_opaque_setup_success_assumed"])
                                self.assertEqual(result["native_wait_output"], wait in (0, 5))
                                self.assertEqual(result["opaque_wait_output_relaxed"], wait not in (0, 5))
                                self.assertEqual(result["event_consumed"], not busy and wait == 0)
                                coherent = mailbox != 0 and command == 0x73760001 and status == 0
                                self.assertEqual(result["current_transaction_acknowledged"], not busy and wait == 0 and coherent)
                                self.assertEqual(result["projected_current_path"], not busy and wait != 5)
                                self.assertEqual(result["observation_coherence_relaxed"], not busy and wait != 5 and not coherent)
                                self.assertFalse(result["runtime_observed"])
                                self.assertFalse(result["timeout_proves_backend_nonexecution"])
                                self.assertLessEqual(len(result["events"]), 64)
                                returns = [event for event in result["events"] if event["event"] == "checked_return"]
                                self.assertEqual([event["caller"] for event in returns],
                                                 ["builder", "loader", "image", "factory", "context", "host"])
                                self.assertTrue(all(event["value"] == expected and event["success"] == (expected == 0)
                                                    for event in returns))
                                names = [event["event"] for event in result["events"]]
                                self.assertEqual("event_consumed" in names, not busy and wait == 0)
                                self.assertEqual("completion_read" in names, not busy and wait != 5)
                                self.assertEqual("reply_copy" in names, not busy and wait != 5 and mailbox != 0)
                                self.assertEqual("backend_status_check" in names,
                                                 not busy and wait != 5 and mailbox != 0 and command == 0x73760001)
                                self.assertEqual("success_reset" in names, not busy and wait != 5 and coherent)
                                self.assertEqual("observation_coherence_relaxed" in names,
                                                 not busy and wait != 5 and not coherent)
        self.assertEqual(json.dumps(contract, sort_keys=True), frozen)

    def test_current_outer_order_irq_consumption_non_P_mailbox_and_shortcut(self):
        contract = self.mapping()
        result = MAP._fresh_init_projection(contract, {"completion_mailbox": 0x12345678, "request_word1": 255})
        self.assertEqual((result["host_return"], result["host_reply_status"]), (0, 0))
        self.assertTrue(result["current_transaction_acknowledged"])
        self.assertTrue(result["event_consumed"])
        events = result["events"]
        operations = [(event["event"], event.get("function")) for event in events]
        expected = [("outer_call", "Core_Command"), ("outer_call", "Dma_Read"), ("outer_call", "Dma_Sync"),
                    ("outer_call", "CmdInitialize"), ("outer_call", "Core_LocalClear"),
                    ("outer_call", "Platform_EnableInterface"), ("backend_status_store", None),
                    ("outer_call", "Dma_Write"), ("outer_call", "Dma_Sync"), ("outer_call", "Arc_FlushWrites"),
                    ("reply_publication", None), ("outer_call", "Platform_DeliverResponse"),
                    ("response_irq", None), ("arm_callback", None)]
        self.assertEqual([operation for operation in operations if operation[0] in
                          ("outer_call", "backend_status_store", "reply_publication", "response_irq", "arm_callback")], expected)
        names = [event["event"] for event in events]
        self.assertLess(names.index("request_copy"), names.index("request_publication"))
        self.assertLess(names.index("request_publication"), names.index("outer_trigger"))
        self.assertLess(names.index("arm_callback"), names.index("event_wait"))
        self.assertLess(names.index("event_wait"), names.index("event_consumed"))
        self.assertLess(names.index("event_consumed"), names.index("completion_read"))
        self.assertLess(names.index("completion_read"), names.index("reply_copy"))
        self.assertLess(names.index("command_check"), names.index("backend_status_check"))
        self.assertLess(names.index("backend_status_check"), names.index("success_reset"))
        self.assertLess(names.index("success_reset"), names.index("checked_return"))
        self.assertEqual(next(event for event in events if event["event"] == "request_copy")["word1"], 255)
        self.assertEqual(next(event for event in events if event["event"] == "reply_copy")["word1"], 0)
        self.assertEqual(next(event for event in events if event["event"] == "completion_read")["value"], 0x12345678)
        timeout = MAP._fresh_init_projection(contract, {"wait_status": 5})
        self.assertFalse(timeout["event_consumed"])
        self.assertFalse(timeout["busy_after"])
        self.assertFalse(timeout["current_transaction_acknowledged"])
        self.assertFalse(timeout["timeout_proves_backend_nonexecution"])
        zero = MAP._fresh_init_projection(contract, {"completion_mailbox": 0})
        self.assertEqual(zero["host_return"], 9)
        self.assertTrue(zero["busy_after"])
        self.assertTrue(zero["event_consumed"])
        # The shortcut can return0 with no outer command and no event/mailbox
        # access, even if the irrelevant fresh transport input says busy.
        shortcut = MAP._fresh_init_projection(contract, {"already_initialized": True, "busy": True})
        self.assertEqual((shortcut["transport_status"], shortcut["host_return"], shortcut["host_reply_status"]), (None, 0, 0))
        self.assertEqual([event["event"] for event in shortcut["events"]],
                         ["host_init", "initialized_shortcut", "host_reply_status_store"])
        self.assertFalse(shortcut["current_transaction_acknowledged"])
        self.assertFalse(shortcut["event_consumed"])

    def test_delayed_old_callback_countermodel_drops_freshness_and_preserves_new_request(self):
        contract = self.mapping()
        with self.assertRaisesRegex(MAP.FormatError, "freshness"):
            MAP._fresh_init_projection(contract, {"event_origin": "delayed_old"})
        for word1 in (0, 1, 255):
            scenario = {"event_origin": "delayed_old", "freshness_assumed": False,
                        "completion_mailbox": 0x12345678, "request_word1": word1,
                        "reply_command": 0xffffffff, "reply_status": 0xffffffff}
            result = MAP._fresh_init_projection(contract, scenario)
            expected = 0 if word1 == 0 else 2
            with self.subTest(word1=word1):
                self.assertEqual((result["transport_status"], result["host_return"], result["host_reply_status"]),
                                 (expected, expected, 0 if expected == 0 else 0xffffffff))
                self.assertFalse(result["freshness_assumed"])
                self.assertFalse(result["current_transaction_acknowledged"])
                self.assertFalse(result["projected_current_path"])
                self.assertFalse(result["runtime_observed"])
                self.assertTrue(result["event_consumed"])
                names = [event["event"] for event in result["events"]]
                self.assertLess(names.index("event_reset"), names.index("request_copy"))
                self.assertLess(names.index("request_copy"), names.index("delayed_old_callback"))
                self.assertLess(names.index("delayed_old_callback"), names.index("event_wait"))
                for forbidden in ("outer_trigger", "outer_call", "backend_status_store", "reply_publication", "response_irq", "arm_callback"):
                    self.assertNotIn(forbidden, names)
                reply = next(event for event in result["events"] if event["event"] == "reply_copy")
                self.assertEqual((reply["command"], reply["word1"]), (0x73760001, word1))

    def test_projection_event_budget_type_byte_domain_and_unsupported_operand_refusals(self):
        contract = self.mapping()
        result = MAP._fresh_init_projection(contract, {})
        used = len(result["events"])
        with mock.patch.object(MAP, "MAX_FRESH_INIT_EVENTS", used):
            self.assertEqual(MAP._fresh_init_projection(contract, {}), result)
        with mock.patch.object(MAP, "MAX_FRESH_INIT_EVENTS", used - 1), self.assertRaisesRegex(MAP.FormatError, "budget"):
            MAP._fresh_init_projection(contract, {})
        for cap in (0, used - 1):
            changed = json.loads(json.dumps(contract))
            changed["validation"]["projected_event_cap"] = cap
            with self.subTest(cap=cap), self.assertRaisesRegex(MAP.FormatError, "budget"):
                MAP._fresh_init_projection(changed, {})
        for scenario in (None, [], {"unknown": 0}, {"busy": 1}, {"already_initialized": 0},
                         {"freshness_assumed": "false"}, {"event_origin": "unknown"},
                         {"event_origin": 1}, {"wait_status": True}, {"wait_status": -1},
                         {"completion_mailbox": 0x100000000}, {"reply_command": None},
                         {"reply_status": 1.0}, {"request_word1": 256}, {"request_word1": 0xffffffff}):
            with self.subTest(scenario=scenario), self.assertRaises(MAP.FormatError):
                MAP._fresh_init_projection(contract, scenario)
        # Call the narrow operand decoder directly, beyond the outer pin
        # validator, so unsupported modeled instructions cannot pass silently.
        for word in (0xe5940004 | 0x00200000, 0xe4940004, 0xe92d4010,
                     0xe3400001, 0xe7e70050, 0xe1a00110):
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, 0x27080, word)
            with self.subTest(word=hex(word)), self.assertRaises(MAP.FormatError):
                MAP._fresh_init_arm_operand(changed, 0x27080, word)

    def test_every_additional_pin_word_name_and_tail_rejects_before_decode(self):
        for role, offset, size in self.FRESH_REGIONS:
            deltas = range(size) if "name" in role else sorted(set(range(0, size, 4)) | {size - 1})
            for delta in deltas:
                changed = bytearray(self.payload)
                changed[offset + delta] ^= 1
                with self.subTest(role=role, offset=hex(offset + delta)), \
                        mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("decode before validation")), \
                        mock.patch.object(MAP, "_a32_literal", side_effect=AssertionError("decode before validation")), \
                        self.assertRaises(MAP.FormatError):
                    self.mapping(changed)

    def test_every_bridge_dependency_pin_and_original_relocation_record_rejects(self):
        for role, offset, encoded in MAP._COMMAND_BUFFER_BRIDGE_REGIONS:
            size = len(encoded) // 2
            deltas = range(size) if "name" in role else sorted(set(range(0, size, 4)) | {size - 1})
            for delta in deltas:
                changed = bytearray(self.payload)
                changed[offset + delta] ^= 1
                with self.subTest(role=role, offset=hex(offset + delta)), \
                        mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("decode before dependency validation")), \
                        self.assertRaises(MAP.FormatError):
                    self.mapping(changed)
        for offset in list(range(0x72780, 0x78d44, 12)) + [0x78d43]:
            changed = bytearray(self.payload)
            changed[offset] ^= 1
            with self.subTest(relocation=hex(offset)), \
                    mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("decode before RELA validation")), \
                    self.assertRaises(MAP.FormatError):
                self.mapping(changed)

    def test_selected_arguments_predicates_event_and_return_edge_mutations_reject(self):
        for offset, replacement in (
                (0x27088, 0x1a000002),  # Invert the busy gate.
                (0x270b4, 0xe5940098),  # Change copied packet alias.
                (0x270c0, 0xe59421b0),  # Publish image base instead of physical packet P.
                (0x27118, 0xe15a0002),  # Invent comparison with P, not nonzero.
                (0x27138, 0xe1500002),  # Command equality uses wrong saved register.
                (0x27160, 0xe5970008),  # Status is word1, not word2.
                (0x271c0, 0xe3a00000),  # Erase nonzero original wait result.
                (0x27218, 0xe5868004),  # Word1 byte argument comes from saved R9.
                (0x2723c, 0xe3043e21),  # Exact20000 argument, not measured time.
                (0x20618, 0xe3a02000),  # Setter must publish1.
                (0x20608, 0xe320f000),  # Successful wait consumes the byte.
                (0x2c184, 0xe5950024),  # Callback slot+32 is the event.
                (0x6fac, 0xe5960008),  # Callback argument and status are distinct slots.
                (0x5d68, 0xe5840004),  # Failed INIT reply status at+8, not sequence+4.
                (0x22dec, 0xe3a00000)):  # Error logger must preserve incoming status.
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, offset, replacement)
            with self.subTest(offset=hex(offset)), self.assertRaises(MAP.FormatError):
                self.mapping(changed)
        for section, address, replacement in (
                (16, 0x258b4, 0x60279e00),  # Delayed INIT argument must be R0.
                (16, 0x246e0, 0x10070408),  # Reply status store must be word1.
                (16, 0x259d0, 0x405ffe40),  # Full128-halfword transfer argument.
                (16, 0x259e8, 0x14001a80),  # Completion writes mailbox132, not128.
                (16, 0x3a9b0, 0x20000121),  # Taken-only.jd must not become ordinary.d.
                (16, 0x266e4, 0x1046811c)):
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, self.arc_offset(section, address), replacement)
            with self.subTest(section=section, address=hex(address)), self.assertRaises(MAP.FormatError):
                self.mapping(changed)
        # New original RELA receipts bind every field, signed addend included.
        for position, *_ in self.ARC_CALLS:
            for delta in range(12):
                changed = bytearray(self.payload)
                changed[position + delta] ^= 1
                with self.subTest(record=hex(position), byte=delta), self.assertRaises(MAP.FormatError):
                    self.mapping(changed)

    def test_truncation_and_malformed_image_identities_fail_closed(self):
        for length in (0, 0x5ccc, 0x271c8, 0x491e0, 0x79098, len(self.payload) - 1):
            with self.subTest(length=length), self.assertRaises(MAP.FormatError):
                self.mapping(self.payload[:length])
        for images in ([], self.images[:1], self.images[::-1], self.images + self.images):
            with self.subTest(images=len(images)), self.assertRaises(MAP.FormatError):
                self.mapping(images=images)
        for key, value in (("flags", 1), ("machine", 93), ("section_count", 54),
                           ("endianness", "big"), ("blob_file_offset", 0)):
            images = [dict(image) for image in self.images]
            images[0][key] = value
            with self.subTest(key=key), self.assertRaises(MAP.FormatError):
                self.mapping(images=images)

    def test_private_receipt_and_projection_do_not_route_from_any256_public_reports(self):
        options = ("references", "all_symbols", "bootstrap", "picture_output",
                   "arc_metadata", "csc_command", "command_buffer_bridge", "inner_descriptor")
        aggregate = hashlib.sha256()
        with mock.patch.object(MAP, "_fresh_init_causal_contract", side_effect=AssertionError("public causal route")), \
                mock.patch.object(MAP, "_fresh_init_projection", side_effect=AssertionError("public projection route")), \
                mock.patch.object(MAP, "_init_reply_metadata_linkage", side_effect=AssertionError("public reply metadata route")), \
                mock.patch.object(MAP, "_init_reply_translation_projection", side_effect=AssertionError("public translation route")), \
                mock.patch.object(MAP, "_open_reply_metadata_linkage", side_effect=AssertionError("public OPEN metadata route")):
            for mask in range(256):
                flags = {name: bool(mask & (1 << bit)) for bit, name in enumerate(options)}
                wanted = ("ReadLine",) if mask & 1 else MAP.DEFAULT_SYMBOLS
                result = MAP.analyze(self.data, wanted, **flags)
                self.assertNotIn("fresh_init_causal_contract", result)
                self.assertNotIn("fresh_init_projection", result)
                prior_inner_descriptor_projection(result)
                stdout = (json.dumps(result, indent=2, sort_keys=True) + "\n").encode()
                aggregate.update(bytes([mask]))
                aggregate.update(hashlib.sha256(stdout).digest())
        self.assertEqual(aggregate.hexdigest(),
                         "33f775ebb6fc935321506ac22faeb48857f2e40b1ba85f114f1d77570b4348b6")
        with mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO), \
                self.assertRaises(SystemExit) as error:
            MAP.main([str(BLOB), "--fresh-init-causal-contract"])
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(output.getvalue(), "")


class FirmwareInitReplyMetadataTests(unittest.TestCase):
    ARM_WORDS = {
        0x28058: 0xe1a04000, 0x280a4: 0xe59421b0, 0x280ac: 0xe28d301c,
        0x27bd0: 0xe92d4fff, 0x27bd4: 0xe24dd01c, 0x27be0: 0xe1a0a003,
        0x27c7c: 0xe59d000c,
        0x27c80: 0xe590000c, 0x27c84: 0xe58d0008, 0x27d48: 0xe59d0024,
        0x27d4c: 0xe59d1008, 0x27d50: 0xe0800001, 0x27d54: 0xe58a0000,
        0x27d5c: 0xe59a2000, 0x27d60: 0xe1cd20f0, 0x27d64: 0xe3a03000,
        0x2af2c: 0xe92d4fff, 0x2af30: 0xe24dd014, 0x2af3c: 0xe1a09003,
        0x2afac: 0xe5849004, 0x2afb0: 0xe59d0048, 0x2afb4: 0xe5840008,
        0x2a48c: 0xe0813101, 0x2a490: 0xe2804040, 0x2a494: 0xe0843183,
        0x271d0: 0xe1a04000, 0x271e4: 0xe28d8004, 0x2720c: 0xe1a07008,
        0x27244: 0xe28d3004,
        0x273bc: 0xe597100c, 0x273c0: 0xe594000c, 0x273c4: 0xe2842e25,
        0x273cc: 0xe28f0f81, 0x273d4: 0xe5971010, 0x273d8: 0xe594000c,
        0x273dc: 0xe2842f95, 0x273e4: 0xe5970008, 0x273e8: 0xe584018c,
        0x273f4: 0xe1a0000a,
        0x1fdac: 0xe92d4030, 0x1fdb0: 0xe1a03000, 0x1fdb4: 0xe1a04001, 0x1fdb8: 0xe1a01003,
        0x1fdbc: 0xe5910028, 0x1fdc0: 0xe0800004, 0x1fdc4: 0xe5915030,
        0x1fdc8: 0xe0400005, 0x1fdcc: 0xe5820000, 0x1fdd0: 0xe5920000,
        0x1fdd4: 0xe5915018, 0x1fdd8: 0xe1500005, 0x1fde0: 0xe5920000,
        0x1fde4: 0xe591501c, 0x1fde8: 0xe1500005,
        0x1fdf4: 0xe5930004, 0x1fdf8: 0xe3500000, 0x1fe58: 0xe3a00002, 0x1fe5c: 0xe8bd8030,
        0x1fe64: 0xe3a00000,
        0x2a498: 0xe593200c, 0x2a49c: 0xe3520000, 0x2a4b0: 0xe5933004,
        0x2a4b4: 0xe3530001, 0x2a4c0: 0xe3520203, 0x2a4cc: 0xe5903004,
        0x2a4d0: 0xe1530002, 0x2a4dc: 0xe0813101, 0x2a4e0: 0xe2804040,
        0x2a4e4: 0xe0843183, 0x2a4e8: 0xe5933008, 0x2a4ec: 0xe3130004,
        0x2a4f4: 0xe5903004, 0x2a4f8: 0xe0422003, 0x2a500: 0xe5903004,
        0x2a504: 0xe0423003, 0x2a508: 0xe5904008, 0x2a50c: 0xe0832004,
        0x2a510: 0xe2803d61, 0x2a514: 0xe7832101,
        0x2a3fc: 0xe1d050be, 0x2a408: 0xe1d050be, 0x2a40c: 0xe3550080,
        0x2a418: 0xe5905004, 0x2a41c: 0xe3550203, 0x2a428: 0xe1d050be,
        0x2a42c: 0xe0855105, 0x2a430: 0xe2816040, 0x2a434: 0xe0864185,
        0x2a438: 0xe5905004,
        0x2a43c: 0xe594600c, 0x2a440: 0xe0453006,
        0x2a444: 0xe1d050be, 0x2a448: 0xe2816d61, 0x2a44c: 0xe7965105,
        0x2a450: 0xe0855003, 0x2a454: 0xe5805004,
    }
    ARM_BRANCHES = (
        (0x273c8, 0x1fdac, True, 14), (0x273d0, 0x203c4, True, 14),
        (0x273e0, 0x1fdac, True, 14), (0x273f0, 0x203c4, True, 14),
        (0x273f8, 0x2729c, False, 14),
        (0x1fddc, 0x1fdf4, False, 3), (0x1fdec, 0x1fdf4, False, 8),
        (0x1fdf0, 0x1fe60, False, 14), (0x1fdfc, 0x1fe58, False, 0),
        (0x1fe68, 0x1fe5c, False, 14),
        (0x2a4a0, 0x2a4c0, False, 1), (0x2a4b8, 0x2a4c0, False, 0),
        (0x2a4c4, 0x2a4cc, False, 3), (0x2a4d4, 0x2a4dc, False, 9),
        (0x2a4f0, 0x2a500, False, 0), (0x2a4fc, 0x2a510, False, 14),
        (0x2a404, 0x2a414, False, 0), (0x2a410, 0x2a418, False, 11),
        (0x2a420, 0x2a428, False, 3),
    )
    ARC_WORDS = {0x258b4: 0x60079e00, 0x24604: 0x61c00000,
                 0x246b4: 0x601f7c00, 0x246b8: 0x78608, 0x246bc: 0x1007000c,
                 0x246c0: 0x40007e0c, 0x246c4: 0x10070010}

    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]
        cls.elf_base = 0x2ea60
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", cls.payload, cls.elf_base)
        cls.sections = [struct.unpack_from("<10I", cls.payload, cls.elf_base + header[6] + i * 40)
                        for i in range(header[12])]

    word = FirmwareFreshInitCausalTests.word
    arc_offset = FirmwareFreshInitCausalTests.arc_offset
    immediate = staticmethod(FirmwareFreshInitCausalTests.immediate)

    def mapping(self, payload=None, images=None):
        return MAP._init_reply_metadata_linkage(self.payload if payload is None else payload,
                                               self.images if images is None else images)

    def test_independent_object_relocation_section21_and_NOBITS_extent(self):
        self.assertEqual(struct.unpack_from("<IIi", self.payload, 0x72948), (0x246b8, 0x31904, 0))
        self.assertEqual(0x31904 >> 8, 793)
        self.assertEqual(self.elf_base + self.sections[35][4] + 793 * 16, 0x6cd00)
        symbol = struct.unpack_from("<IIIBBH", self.payload, 0x6cd00)
        self.assertEqual(symbol, (0x1d8f, 0x78608, 24, 0x11, 0, 21))
        self.assertEqual(self.elf_base + self.sections[34][4] + symbol[0], 0x69824)
        self.assertEqual(self.payload[0x69824:0x69835], b"dms_deliver_info\0")
        self.assertEqual(self.sections[21], (0x260, 8, 3, 0x77138, 0x35734, 0x1654, 0, 0, 4, 1))
        self.assertEqual(0x79540 + 21 * 40, 0x79888)
        self.assertTrue(self.sections[21][3] <= symbol[1] < symbol[1] + symbol[2] <=
                        self.sections[21][3] + self.sections[21][5])
        # NOBITS has no object bytes to interpret from its sh_offset.
        self.assertEqual(self.sections[21][1], 8)
        self.assertEqual(self.arc_offset(16, 0x246b8), 0x4724c)
        self.assertEqual(self.word(0x4724c), symbol[1])

    def test_independent_reply_indices_translation_arguments_and_ignored_returns(self):
        for offset, word in self.ARM_WORDS.items():
            with self.subTest(arm=hex(offset)):
                self.assertEqual(self.word(offset), word)
        for offset, target, link, condition in self.ARM_BRANCHES:
            word = self.word(offset)
            signed = (word & 0xffffff) - (0x1000000 if word & 0x800000 else 0)
            with self.subTest(branch=hex(offset)):
                self.assertEqual((word >> 25 & 7, bool(word >> 24 & 1), word >> 28), (5, link, condition))
                self.assertEqual(offset + 8 + signed * 4, target)
        for address, word in self.ARC_WORDS.items():
            self.assertEqual(self.word(self.arc_offset(16, address)), word)
        self.assertEqual([self.ARC_WORDS[p] & 511 for p in (0x246bc, 0x246c0, 0x246c4)], [12, 12, 16])
        self.assertEqual([self.word(p) & 4095 for p in (0x273bc, 0x273d4, 0x273c0, 0x273d8)], [12, 16, 12, 12])
        self.assertEqual([self.immediate(self.word(p)) for p in (0x273c4, 0x273dc)], [0x250, 0x254])
        self.assertEqual(self.word(0x273cc) >> 21 & 15, 4)  # ADD overwrites first returned R0.
        self.assertTrue(self.word(0x273e4) >> 20 & 1)  # LDR overwrites second returned R0.
        self.assertEqual(self.word(0x273f4) & 15, 10)  # Return saved transport status, not either translation status.
        self.assertLess(0x1fdcc, 0x1fdd8)
        self.assertLess(0x1fdcc, 0x1fde8)
        self.assertEqual(self.word(0x1fdf4) & 4095, 4)  # Fallback chain head; null takes explicit MOV2 edge.
        self.assertEqual(self.word(self.arc_offset(16, 0x25824)), 0x61ff7c00)
        self.assertEqual(self.word(self.arc_offset(16, 0x25828)), 0x30051d00)
        self.assertEqual(self.word(self.arc_offset(16, 0x25824)) >> 21 & 63, 15)
        call = self.word(self.arc_offset(16, 0x258b0))
        self.assertEqual(call, 0x2ffda720)
        displacement = (call >> 7 & 0xfffff) - 0x100000
        self.assertEqual(0x258b0 + 4 + displacement * 4, 0x245ec)
        self.assertEqual(call & 0xf800007f, 0x28000020)  # Ordinary BL.d: delay always executes.
        self.assertEqual(self.ARC_WORDS[0x258b4] >> 9 & 63, 15)
        self.assertEqual((self.ARC_WORDS[0x258b4] >> 21 & 63,
                          self.ARC_WORDS[0x24604] >> 9 & 63), (0, 0))
        self.assertEqual(self.ARC_WORDS[0x24604] >> 21 & 63, 14)
        self.assertEqual([self.ARC_WORDS[p] >> 15 & 63 for p in (0x246bc, 0x246c4)], [14, 14])
        self.assertEqual(struct.unpack_from("<IIIBBH", self.payload, 0x6be10)[1:],
                         (0x25808, 516, 0x12, 0, 16))
        relas = [struct.unpack_from("<IIi", self.payload, p) for p in range(0x72780, 0x78d44, 12)]
        self.assertFalse(any(record[0] in (0x258b4, 0x24604) for record in relas))

    def test_independent_nonzero_NOBITS_placement_and_symbol_rebase_equation(self):
        section, symbol = self.sections[21], struct.unpack_from("<IIIBBH", self.payload, 0x6cd00)
        self.assertNotEqual(section[3], 0)
        self.assertEqual(section[2] & 4, 0)
        self.assertEqual(self.immediate(self.word(0x2a4c0)), 0x30000000)
        self.assertEqual(self.immediate(self.word(0x2a4ec)), 4)
        self.assertEqual(self.immediate(self.word(0x2a510)), 0x1840)
        self.assertEqual(self.immediate(self.word(0x2a448)), 0x1840)
        # Selected loader origin0 and data baseB. This comes from section21,
        # not section17's unrelated command-block extent or file offset.
        destination_offset = section[3] - 0 + 0
        rebased_symbol_offset = destination_offset + symbol[1] - section[3]
        self.assertEqual((destination_offset, rebased_symbol_offset, rebased_symbol_offset + 12),
                         (0x77138, 0x78608, 0x78614))
        self.assertNotEqual(section[3], self.sections[17][3])
        # PUSH52+local28 places incoming R2 at SP+36; the nested loader's
        # PUSH52+local20 reaches the physical data-base stack argument at+72.
        self.assertEqual((self.word(0x27bd0) & 65535).bit_count() * 4, 52)
        self.assertEqual(self.immediate(self.word(0x27bd4)), 28)
        self.assertEqual(self.word(0x27d48) & 4095, 28 + 2 * 4)
        self.assertEqual((self.word(0x2af2c) & 65535).bit_count() * 4, 52)
        self.assertEqual(self.immediate(self.word(0x2af30)), 20)
        self.assertEqual(self.word(0x2afb0) & 4095, 52 + 20)
        self.assertEqual(self.immediate(self.word(0x27d64)), 0)
        self.assertEqual((self.word(0x2af3c) >> 12 & 15, self.word(0x2af3c) & 15), (9, 3))
        self.assertEqual((self.word(0x2afac) >> 12 & 15, self.word(0x2afac) & 4095), (9, 4))

    def test_returned_section_symbol_relocation_reply_and_translation_contract(self):
        result = self.mapping()
        self.assertEqual(result["basis"], result["fresh_init"]["basis"])
        self.assertTrue(result["basis"]["conditional"])
        self.assertTrue(result["basis"]["selected_regions_validated"])
        for key in ("entire_payload_rehashed", "device_observed", "public_route"):
            self.assertFalse(result["basis"][key])
        self.assertEqual({key: value for key, value in result["validation"].items()
                          if key != "validated_regions"},
                         {"additional_region_count": 4, "additional_byte_count": 85,
                          "additional_anchor_count": 104, "dependency_byte_count": 45228,
                          "aggregate_byte_count": 45313})
        self.assertEqual(result["section_placement"], {
            "section_index": 21, "section_type": 8, "flags": 3,
            "virtual_address": 0x77138, "byte_extent": 0x1654,
            "original_virtual_base": 0, "initialized_contents_proven": False,
            "physical_base_offset_from_B": 0,
            "destination_offset_from_B": 0x77138, "nonzero_va_bypasses_progbits_gate": True,
            "destination_table_context_offset": 0x1840, "section_header_stride": 40,
            "fallback_frame_bytes": 80, "loader_frame_bytes": 72,
            "catalog_callbacks_are_inherited_successful_baseline_premise": True,
            "catalog_metadata_word_blob_file_offset": 0xcfbcc,
            "constructed_descriptor_stack_offset": 12, "caller_output_stack_offset": 28,
            "selected_entry_requires_valid_loader_and_section_iteration": True,
            "placement_precedes_symbol_rebase": True})
        self.assertEqual(result["symbol_rebase"], {
            "name": "dms_deliver_info", "symbol_index": 793,
            "symbol_record_blob_file_offset": 0x6cd00,
            "value": 0x78608, "size": 24, "section_index": 21,
            "section_relative_offset": 0x14d0, "destination_offset_from_B": 0x77138,
            "rebased_offset_from_B": 0x78608,
            "selected_entry_requires_valid_loader_and_symbol_iteration": True})
        self.assertEqual(struct.unpack_from("<IIIBBH", self.payload, 0x69e50)[1:],
                         (0x245ec, 412, 2, 0, 16))
        self.assertEqual(result["relocation_receipt"], {
            "architecture": "ELF", "operation": "selected type4 S+A",
            "relocation_record_blob_file_offset": 0x72948, "relocation_section_index": 51,
            "vendor_type": 4, "symbol_index": 793, "addend": 0,
            "source_section_index": 16, "source_function": "CmdInitialize",
            "source_function_symbol_index": 46, "source_function_elf_virtual_address": 0x245ec,
            "source_function_bytes": 412, "symbol_section_index": 21,
            "literal_blob_file_offset": 0x4724c, "literal_elf_virtual_address": 0x246b8,
            "original_literal": 0x78608, "patched_literal_offset_from_B": 0x78608,
            "selected_relocation_is_unique": True,
            "byte_store_blob_file_offsets": [0x29c9c, 0x29ca4, 0x29cb0, 0x29cbc],
            "applied_before_section_copy": True})
        self.assertEqual(result["outer_reply"], {
            "word0_command": 0x73760001, "word1_status": 0,
            "word2_interpretation": "raw_uninterpreted",
            "conditional_on_selected_fresh_arc_execution": True,
            "transport_checks_metadata_words": False,
            "local_packet_address": 0x30051d00, "caller_buffer_register": 15,
            "callee_buffer_register": 14,
            "buffer_argument_receipt_addresses": [0x258b4, 0x24604],
            "word3_offset_from_B": 0x78608, "word4_offset_from_B": 0x78614,
            "word4_delta_bytes": 12})
        self.assertEqual(result["arm_translations"], {
            "reply_base_stack_offset": 4, "response_alias_register": 7, "context_register": 4,
            "map_context_offset": 12, "output_context_offsets": [0x250, 0x254],
            "reply_byte_offsets": [12, 16], "helper_entry": 0x1fdac,
            "helper_store_precedes_bounds_check": True, "helper_status_checked": False,
            "stored_outputs_validated": False,
            "ignored_return_overwrite_blob_file_offsets": [0x273cc, 0x273e4],
            "saved_transport_status_preserved": True,
            "raw_version_reply_byte_offset": 8, "raw_version_context_offset": 0x18c,
            "translation": {"virtual_base_offset": 40, "physical_base_offset": 48,
                            "inclusive_low_offset": 24, "inclusive_high_offset": 28,
                            "chain_head_offset": 4, "error_status": 2, "success_status": 0,
                            "arithmetic_bits": 32, "unsigned_inclusive_checks": True,
                            "output_store_blob_file_offset": 0x1fdcc,
                            "bounds_check_blob_file_offsets": [0x1fdd8, 0x1fde8],
                            "fallback_chain_evaluated": False,
                            "error_projection_requires_empty_chain": True}})
        self.assertEqual(result["validation_scope"], {
            "conditional_reply_metadata": True, "catalog_callback_closure": False,
            "fallback_translation_chain_evaluated": False, "stored_queue_pointers_validated": False,
            "hardware_aliasing_proven": False, "runtime_observed": False, "freshness_proven": False,
            "operational_coherence_proven": False, "source_plane_lease": False,
            "active_decode_context": False, "standalone_execution": False, "public_route": False})
        self.assertEqual(result["assumptions"][:len(result["fresh_init"]["assumptions"])],
                         result["fresh_init"]["assumptions"])
        added = " ".join(result["assumptions"][len(result["fresh_init"]["assumptions"]):])
        for phrase in ("catalog callbacks", "does not decode arbitrary", "section iteration r0=loader/r1=21",
                       "symbol iteration r0=symbol793/r1=loader", "callee-saved ABI preservation",
                       "preserve CmdInitialize's saved r14 reply-buffer register",
                       "map storage, C+0x250/C+0x254 output slots and protected stack storage are disjoint",
                       "map fields remain stable",
                       "NOBITS address placement proves no initialized object contents", "unselected vendor",
                       "calling-convention and freshness premises",
                       "do not validate a live allocation or queue"):
            self.assertIn(phrase, added)

    def test_all104_returned_anchors_match_independent_words_and_operands(self):
        result = self.mapping()
        anchors = result["instruction_anchors"]
        arm = [record for record in anchors if record["architecture"] == "ARM"]
        arc = [record for record in anchors if record["architecture"] == "ARC"]
        elf = [record for record in anchors if record["architecture"] == "ELF"]
        self.assertEqual((len(anchors), len(arm), len(arc), len(elf)), (104, 97, 6, 1))
        branch_specs = {offset: (target, link, condition) for offset, target, link, condition in self.ARM_BRANCHES}
        # Additional static guards and inherited return words are not new receipts.
        static_only = {0x273d0, 0x273f0, 0x273f4, 0x273f8, 0x2a3fc, 0x2a404, 0x2a408,
                       0x2a40c, 0x2a410, 0x2a418, 0x2a41c, 0x2a420, 0x2a4b0, 0x2a4b4,
                       0x2a4b8, 0x2a4f4, 0x2a4f8, 0x2a4fc}
        selected = (set(self.ARM_WORDS) | set(branch_specs)) - static_only
        self.assertEqual(len(selected), 97)
        self.assertEqual({record["blob_file_offset"] for record in arm}, selected)
        self.assertEqual(len({record["blob_file_offset"] for record in arm}), len(arm))
        for record in arm:
            offset, word = record["blob_file_offset"], self.word(record["blob_file_offset"])
            expected = {"architecture": "ARM", "blob_file_offset": offset, "word": word, "condition": word >> 28}
            if offset in branch_specs:
                target, link, condition = branch_specs[offset]
                expected.update(operation="BL" if link else "B", target_blob_file_offset=target)
                self.assertEqual(word >> 28, condition)
            else:
                self.assertEqual(word, self.ARM_WORDS[offset])
                if offset in (0x27bd0, 0x2af2c, 0x1fdac, 0x1fe5c):
                    expected.update(operation="POP" if offset == 0x1fe5c else "PUSH", base_register=13,
                                    register_mask=word & 65535, byte_count=(word & 65535).bit_count() * 4)
                elif offset in (0x2a3fc, 0x2a408, 0x2a428, 0x2a444, 0x27d60):
                    expected.update(operation="STRD" if offset == 0x27d60 else "LDRH",
                                    base_register=word >> 16 & 15, data_register=word >> 12 & 15,
                                    byte_width=8 if offset == 0x27d60 else 2,
                                    byte_offset=(word >> 4 & 0xf0) + (word & 15))
                    if offset == 0x27d60:
                        expected["second_data_register"] = 3
                elif word >> 26 & 3 == 1:
                    expected.update(operation="LDR" if word >> 20 & 1 else "STR",
                                    base_register=word >> 16 & 15, data_register=word >> 12 & 15,
                                    byte_width=1 if word >> 22 & 1 else 4)
                    if word >> 25 & 1:
                        expected.update(offset_register=word & 15, shift_kind="LSL", shift_amount=word >> 7 & 31)
                    else:
                        expected["byte_offset"] = (word & 4095) * (1 if word >> 23 & 1 else -1)
                else:
                    self.assertEqual(word >> 26 & 3, 0)
                    opcode = word >> 21 & 15
                    expected.update(operation={2: "SUB", 4: "ADD", 8: "TST", 10: "CMP", 13: "MOV"}[opcode],
                                    source_register=word >> 16 & 15, destination_register=word >> 12 & 15)
                    if word >> 25 & 1:
                        expected["immediate"] = self.immediate(word)
                    else:
                        expected.update(operand_register=word & 15, shift_kind="LSL", shift_amount=word >> 7 & 31)
            with self.subTest(arm=hex(offset)):
                self.assertEqual(record, expected)
        self.assertEqual({record["elf_virtual_address"] for record in arc},
                         {0x258b4, 0x24604, 0x246b4, 0x246bc, 0x246c0, 0x246c4})
        for record in arc:
            address = record["elf_virtual_address"]
            word = self.ARC_WORDS[address]
            expected = {"architecture": "ARC", "section_index": 16, "elf_virtual_address": address,
                        "blob_file_offset": self.arc_offset(16, address), "word": word, "decode_conditional": True,
                        "destination_register": word >> 21 & 63, "source_register": word >> 15 & 63,
                        "operand_register": word >> 9 & 63, "signed_low9": (word & 511) - (512 if word & 256 else 0),
                        "operation": {0x246b4: "MOV LIMM", 0x246c0: "ADD immediate"}.get(address, "STR")}
            if address == 0x246b4:
                expected["literal_value"] = 0x78608
            if address in (0x258b4, 0x24604):
                expected.update(operation="MOV register", source_register=15 if address == 0x258b4 else 0,
                                source_function="Core_Command" if address == 0x258b4 else "CmdInitialize")
            with self.subTest(arc=hex(address)):
                self.assertEqual(record, expected)
        self.assertEqual(elf, [result["relocation_receipt"]])

    def test_fixed_metadata_regions_hashes_and_exact_one_over_preflight_budgets(self):
        self.assertEqual({(offset, size) for _, offset, size, _ in MAP._INIT_REPLY_REGIONS},
                         {(0x79888, 40), (0x6cd00, 16), (0x69824, 17), (0x72948, 12)})
        self.assertEqual((len(MAP._INIT_REPLY_REGIONS), sum(size for _, _, size, _ in MAP._INIT_REPLY_REGIONS)), (4, 85))
        for role, offset, size, digest in MAP._INIT_REPLY_REGIONS:
            with self.subTest(role=role):
                self.assertEqual(hashlib.sha256(self.payload[offset:offset + size]).hexdigest(), digest)
        self.assertEqual((MAP.MAX_INIT_REPLY_REGIONS, MAP.MAX_INIT_REPLY_BYTES, MAP.MAX_INIT_REPLY_ANCHORS,
                          MAP.MAX_INIT_REPLY_AGGREGATE_BYTES), (12, 1024, 104, 80 * 1024))
        self.assertEqual((MAP.MAX_FRESH_INIT_REGIONS, MAP.MAX_FRESH_INIT_BYTES, MAP.MAX_FRESH_INIT_ANCHORS,
                          MAP.MAX_FRESH_INIT_EVENTS, MAP.MAX_COMMAND_BUFFER_BRIDGE_REGIONS,
                          MAP.MAX_COMMAND_BUFFER_BRIDGE_BYTES), (48, 16 * 1024, 160, 64, 80, 40 * 1024))
        for name, actual in (("MAX_INIT_REPLY_REGIONS", 4), ("MAX_INIT_REPLY_BYTES", 85),
                             ("MAX_INIT_REPLY_ANCHORS", 104), ("MAX_INIT_REPLY_AGGREGATE_BYTES", 45313)):
            with self.subTest(exact=name), mock.patch.object(MAP, name, actual):
                self.assertEqual(self.mapping()["validation"]["additional_anchor_count"], 104)
            with self.subTest(one_over=name), mock.patch.object(MAP, name, actual - 1), \
                    mock.patch.object(MAP, "_fresh_init_causal_contract", side_effect=AssertionError("late preflight")), \
                    self.assertRaisesRegex(MAP.FormatError, "budget"):
                self.mapping()
        for name, actual in (("MAX_FRESH_INIT_ANCHORS", 159), ("MAX_FRESH_INIT_BYTES", 8536),
                             ("MAX_COMMAND_BUFFER_BRIDGE_BYTES", 36692)):
            with self.subTest(dependency=name), mock.patch.object(MAP, name, actual - 1), \
                    mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("decode before dependency bound")), \
                    self.assertRaisesRegex(MAP.FormatError, "budget"):
                self.mapping()

    def test_every_metadata_pin_and_targeted_relocation_object_placement_mutation_rejects(self):
        for role, offset, size, _ in MAP._INIT_REPLY_REGIONS:
            for delta in range(size):
                changed = bytearray(self.payload)
                changed[offset + delta] ^= 1
                with self.subTest(role=role, byte=delta), \
                        mock.patch.object(MAP, "_fresh_init_causal_contract", side_effect=AssertionError("metadata decoded before pins")), \
                        self.assertRaises(MAP.FormatError):
                    self.mapping(changed)
        mutations = ((0x72948 + 4, "<I", 0x31906), (0x72948 + 4, "<I", 0x31804),
                     (0x72948 + 8, "<i", 12), (0x72948 + 8, "<i", -12),
                     (0x6cd00 + 12, "<B", 0x12), (0x6cd00 + 14, "<H", 17),
                     (0x6cd00 + 8, "<I", 25), (0x6cd00 + 4, "<I", 0x78784),
                     (0x79888 + 4, "<I", 1), (0x79888 + 8, "<I", 7),
                     (0x79888 + 12, "<I", 0), (0x79888 + 12, "<I", 0x70000),
                     (0x79888 + 20, "<I", 0x14d0))
        for offset, format_string, value in mutations:
            changed = bytearray(self.payload)
            struct.pack_into(format_string, changed, offset, value)
            with self.subTest(offset=hex(offset), value=value), self.assertRaises(MAP.FormatError):
                self.mapping(changed)
        changed = bytearray(self.payload)
        struct.pack_into("<IIi", changed, 0x72954, 0x246b8, 0x31904, 0)
        with mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("duplicate decoded before RELA pin")), \
                self.assertRaises(MAP.FormatError):
            self.mapping(changed)
        for address in (0x258b4, 0x24604):
            changed = bytearray(self.payload)
            struct.pack_into("<IIi", changed, 0x72954, address, 0x31904, 0)
            with self.subTest(unexpected_relocation=hex(address)), \
                    mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("MOV relocation decoded before pin")), \
                    self.assertRaises(MAP.FormatError):
                self.mapping(changed)

    def test_reply_index_delta_destination_range_and_ignored_return_mutations_reject(self):
        for offset, replacement in (
                (0x47250, 0x10070008), (0x47254, 0x40007e08), (0x47258, 0x10070014),
                (0x273bc, 0xe5971008), (0x273d4, 0xe5971014), (0x273c4, 0xe2842e26),
                (0x273dc, 0xe2842f94), (0x273cc, 0xe3500000), (0x273e4, 0xe3500000),
                (0x273f4, 0xe3a00000), (0x1fdcc, 0xe320f000), (0x1fddc, 0x9a000004),
                (0x1fdec, 0x2a000000), (0x1fdfc, 0x1a000015),
                (0x2a4a0, 0x0a000006), (0x2a4ec, 0xe3130002),
                (0x2a504, 0xe0823003), (0x2a50c, 0xe0832003), (0x2a450, 0xe0855005)):
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, offset, replacement)
            with self.subTest(offset=hex(offset)), self.assertRaises(MAP.FormatError):
                self.mapping(changed)

    def test_every_new_instruction_pin_rejects_before_decode_and_unsupported_forms_refuse(self):
        contract = self.mapping()
        offsets = [record["blob_file_offset"] for record in contract["instruction_anchors"]
                   if record["architecture"] in ("ARM", "ARC")]
        self.assertEqual(len(offsets), 103)
        for offset in offsets:
            for delta in range(4):
                changed = bytearray(self.payload)
                changed[offset + delta] ^= 1
                with self.subTest(offset=hex(offset), byte=delta), \
                        mock.patch.object(MAP, "_a32_branch", side_effect=AssertionError("instruction decoded before pin")), \
                        mock.patch.object(MAP, "_stock_host_handler_footprints", side_effect=AssertionError("handler decoded before pin")), \
                        self.assertRaises(MAP.FormatError):
                    self.mapping(changed)
        for word in (0xe3400000, 0xe92da000, 0xe92d0000, 0xe8bd4000, 0xe8bda000,
                     0xe1cd30f0, 0xe4920000, 0xe7920013, 0xe0400015, 0xe3131004,
                     0xe0500005, 0xe3030004, 0x3b000000):
            changed = bytearray(self.payload)
            struct.pack_into("<I", changed, 0x1fdac, word)
            with self.subTest(unsupported=hex(word)), self.assertRaises(MAP.FormatError):
                MAP._init_reply_arm_operand(changed, 0x1fdac, word)

    def test_raw_u32_translation_inclusive_boundaries_wrap_and_absent_fallback(self):
        contract = self.mapping()
        for virtual, physical in ((0, 0), (0x1000, 0x2000), (0xffffffff, 0), (1, 0xffffffff)):
            for value in (0, 1, 0xfff, 0x1000, 0x1fff, 0x2000, 0xffffffff):
                stored = (virtual + value - physical) & 0xffffffff
                for low, high in ((0, 0xffffffff), (0x1000, 0x1fff), (stored, stored),
                                  (0xffffffff, 0), (1, 0xffffffff)):
                    scenario = {"physical_input": value, "virtual_base": virtual, "physical_base": physical,
                                "inclusive_low": low, "inclusive_high": high, "chain_head": 0}
                    result = MAP._init_reply_translation_projection(contract, scenario)
                    expected_status = 0 if low <= stored <= high else 2
                    with self.subTest(**scenario):
                        self.assertEqual(result["stored_output"], stored)
                        self.assertEqual(result["helper_status"], expected_status)
                        self.assertEqual(result["within_initial_map"], expected_status == 0)
                        self.assertTrue(result["store_precedes_bounds_check"])
                        self.assertFalse(result["caller_checks_helper_status"])
                        self.assertFalse(result["stored_output_validated"])
                        self.assertFalse(result["fallback_chain_evaluated"])
                        self.assertFalse(result["runtime_observed"])
                        self.assertEqual(result["chain_head"], 0)
        scenario = {"physical_input": 0x100, "virtual_base": 0, "physical_base": 0,
                    "inclusive_low": 0x1000, "inclusive_high": 0x1fff, "chain_head": 0}
        self.assertEqual(MAP._init_reply_translation_projection(contract, scenario)["helper_status"], 2)
        for bad in (None, [], dict(scenario, unknown=0), dict(scenario, chain_head=1),
                    dict(scenario, chain_head=0xffffffff), dict(scenario, physical_input=-1),
                    dict(scenario, virtual_base=0x100000000), dict(scenario, physical_base=True),
                    dict(scenario, inclusive_low=1.0), dict(scenario, inclusive_high="0")):
            with self.subTest(scenario=bad), self.assertRaises(MAP.FormatError):
                MAP._init_reply_translation_projection(contract, bad)

    def test_successful_transport_can_retain_both_failed_translation_outputs(self):
        contract = self.mapping()
        fresh = MAP._fresh_init_projection(contract["fresh_init"], {})
        self.assertEqual(fresh["transport_status"], 0)
        self.assertEqual(fresh["host_return"], 0)
        # Relax the initialized-map premise, retaining the checked transport
        # return and an explicitly absent fallback chain; this is not a live observation.
        map_fields = {"virtual_base": 0x1000, "physical_base": 0x2000,
                      "inclusive_low": 0x1000, "inclusive_high": 0x1fff, "chain_head": 0}
        for inputs, statuses in (((0x1fff, 0x3000), (2, 2)), ((0x2000, 0x3000), (0, 2)),
                                 ((0x1fff, 0x2fff), (2, 0)), ((0x2000, 0x2fff), (0, 0))):
            observations = [MAP._init_reply_translation_projection(contract, dict(map_fields, physical_input=value))
                            for value in inputs]
            with self.subTest(inputs=inputs):
                self.assertEqual(tuple(result["helper_status"] for result in observations), statuses)
                self.assertEqual([result["stored_output"] for result in observations],
                                 [(0x1000 + value - 0x2000) & 0xffffffff for value in inputs])
                self.assertTrue(all(not result["caller_checks_helper_status"] and not result["stored_output_validated"]
                                    for result in observations))
                self.assertTrue(contract["arm_translations"]["saved_transport_status_preserved"])
        delayed = MAP._fresh_init_projection(contract["fresh_init"],
                                             {"event_origin": "delayed_old", "freshness_assumed": False,
                                              "request_word1": 0, "completion_mailbox": 1})
        self.assertEqual((delayed["transport_status"], delayed["host_return"]), (0, 0))
        self.assertFalse(delayed["current_transaction_acknowledged"])
        self.assertFalse(delayed["runtime_observed"])
        self.assertNotIn("outer_call", [event["event"] for event in delayed["events"]])

    def test_translation_projection_fixed_model_corruption_refuses_without_certifying_provenance(self):
        contract = self.mapping()
        fixed = {"arithmetic_bits": 32, "success_status": 0, "error_status": 2,
                 "virtual_base_offset": 40, "physical_base_offset": 48,
                 "inclusive_low_offset": 24, "inclusive_high_offset": 28, "chain_head_offset": 4}
        for key, value in fixed.items():
            for replacement in (None, True, False, str(value), float(value), value + 1, -1):
                changed = json.loads(json.dumps(contract))
                changed["arm_translations"]["translation"][key] = replacement
                with self.subTest(field=key, replacement=replacement), self.assertRaises(MAP.FormatError):
                    MAP._init_reply_translation_projection(changed, {})
            changed = json.loads(json.dumps(contract))
            del changed["arm_translations"]["translation"][key]
            with self.subTest(missing=key), self.assertRaises(MAP.FormatError):
                MAP._init_reply_translation_projection(changed, {})
        for key, parent in (("unsigned_inclusive_checks", "translation"),
                            ("helper_store_precedes_bounds_check", "arm"),
                            ("helper_status_checked", "arm"), ("stored_outputs_validated", "arm")):
            for replacement in (None, 0, 1, "false", []):
                changed = json.loads(json.dumps(contract))
                target = changed["arm_translations"] if parent == "arm" else changed["arm_translations"]["translation"]
                target[key] = replacement
                with self.subTest(field=key, replacement=replacement), self.assertRaises(MAP.FormatError):
                    MAP._init_reply_translation_projection(changed, {})
            changed = json.loads(json.dumps(contract))
            target = changed["arm_translations"] if parent == "arm" else changed["arm_translations"]["translation"]
            del target[key]
            with self.subTest(missing=key), self.assertRaises(MAP.FormatError):
                MAP._init_reply_translation_projection(changed, {})
        for path in (("arm_translations",), ("arm_translations", "translation"),
                     ("fresh_init",), ("fresh_init", "bridge"),
                     ("fresh_init", "bridge", "initialized_heap")):
            for replacement in (None, [], "invalid", 0):
                changed = json.loads(json.dumps(contract))
                parent = changed
                for key in path[:-1]:
                    parent = parent[key]
                parent[path[-1]] = replacement
                with self.subTest(path=path, replacement=replacement), self.assertRaises(MAP.FormatError):
                    MAP._init_reply_translation_projection(changed, {})
            changed = json.loads(json.dumps(contract))
            parent = changed
            for key in path[:-1]:
                parent = parent[key]
            del parent[path[-1]]
            with self.subTest(missing_path=path), self.assertRaises(MAP.FormatError):
                MAP._init_reply_translation_projection(changed, {})
        for bad in (None, [], "invalid", 0, True):
            with self.subTest(contract=bad), self.assertRaises(MAP.FormatError):
                MAP._init_reply_translation_projection(bad, {})
        self.assertIn("trusted result", MAP._init_reply_translation_projection.__doc__)
        self.assertIn("assumed", MAP._init_reply_translation_projection.__doc__)
        self.assertIn("not established at runtime", MAP._init_reply_translation_projection.__doc__)

    def test_truncation_invalid_image_identity_purity_and_nonpublic_route(self):
        for length in (0, 0x273bc, 0x4724c, 0x6cd00, 0x79888, len(self.payload) - 1):
            with self.subTest(length=length), self.assertRaises(MAP.FormatError):
                self.mapping(self.payload[:length])
        for images in ([], self.images[:1], self.images[::-1], self.images * 2):
            with self.subTest(image_count=len(images)), self.assertRaises(MAP.FormatError):
                self.mapping(images=images)
        for key, value in (("class", 64), ("endianness", "big"), ("machine", 93),
                           ("elf_type", 3), ("flags", 1), ("section_count", 54)):
            images = [dict(image) for image in self.images]
            images[0][key] = value
            with self.subTest(key=key), self.assertRaises(MAP.FormatError):
                self.mapping(images=images)
        before = bytes(self.payload), json.dumps(self.images, sort_keys=True)
        with mock.patch.object(MAP, "read_firmware", side_effect=AssertionError("external read")), \
                mock.patch("subprocess.run", side_effect=AssertionError("external decoder")), \
                mock.patch.object(MAP, "_fresh_init_causal_contract", wraps=MAP._fresh_init_causal_contract) as dependency:
            result = self.mapping()
        self.assertEqual(dependency.call_count, 1)
        self.assertEqual(before, (bytes(self.payload), json.dumps(self.images, sort_keys=True)))
        self.assertEqual(result, json.loads(json.dumps(result, sort_keys=True)))
        with mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                mock.patch.object(sys, "stderr", new_callable=io.StringIO), self.assertRaises(SystemExit) as error:
            MAP.main([str(BLOB), "--init-reply-metadata-linkage"])
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(output.getvalue(), "")


class FirmwareOpenReplyMetadataTests(unittest.TestCase):
    # Independently inventoried original ELF/ARM/legacy-ARC sites. Ranges are
    # inclusive DWORD addresses, not an instruction scan or runtime emulator.
    ARM_GROUPS = (
        "898 89c 62d8",
        "51c8..51cc 51d8..51f8 570c..573c 5758 575c 5218 521c",
        "8a0..8d8 908..930 970..98c ac0..ae4 b0c..b24 b40 b44 b48 b4c b58 b60 b64 b68 b90 b94",
        "a2a4..a2d4 a2f0 a2f4 a2f8 a2fc..a324 a340 a344 a348 a354 a35c a360 a364 a370..a37c a3ac a3b0",
        "f7e4..f810 f830 f834 f838..f85c f87c f880 f884..f898 f8cc..f8e4 f980 f984 f9a4 f9a8 fb08..fb90 fba8..fbe8",
        "27480..275c0",
    )
    ARC_GROUPS = (
        (16, "25890 258bc 258c0 258c4 247c4"),
        (16, "24788..24794 247c8 247d0 247d4 247d8 247e0 247e4 247e8 247ec 247f0 247f4 247f8 247fc 24800 24810 24814 24818..24880 24890 24894"),
        (16, "248bc 248c4 248c8 248d4 248e0 248e4 248e8..24924 24934 24938"),
        (16, "2493c 24940 24948 2494c 24950 2496c"),
        (16, "24a6c..24aa8"),
        (16, "24a3c 24a40 24a4c 24a50 24a54 24aac 24ab0 24abc 24adc 24b08..24b20 24b24 24b2c 24b30..24b70 !24b6c 24bac 24bb0"),
        (16, "266f8 266fc 26700 26704 2672c 26734 26748 26760 26764 26768 2676c 2677c 26780 26788 26790 26794 26798 267a8 267b0 267b8 268c8 268e0 268e8 26918 26924 2692c 26938 26958 2696c 26970"),
        (16, "3b304 3b308"),
        (4, "9fa4 9fa8 9fb4 9fbc 9fc0 9fc8 9fd0 9fd4"),
        (4, "9ea4 9ea8 9eac 9efc 9f00 9f18 9f1c 9f48 9f5c 9f60"),
        (4, "b570..b5bc !b57c b5c8 b5cc b5d0 b5d4..b5ec !b5dc b5f8 b608 b60c"),
        (2, "4d10 4d14 4d1c 4d24..4db4 4dbc 4dc0"),
        (4, "b650 b658 b688 b818 b880 b8d8 b8dc b8e0 b8e4..b904 b944 b948 b970 b974 b978 b97c b980 b984 b988"),
        (16, "26158 26170 26174 26178 2617c 26180 26188 2618c..261ac 261d4 261d8 261e0 261e4 261e8 261ec 261f0 261f8 261fc 26200 26204 26208 2620c 26210 26214 26218 26264 26268"),
        (4, "be38 be3c be44 be48 be4c be50 be54 be58 be60 be68 be6c"),
    )
    PIN_SHAPES = (
        (0x51c8, 2112), (0x8a0, 760), (0xa2a4, 272), (0xf7e4, 1032),
        (0x27480, 324), (0x4731c, 1068), (0x4928c, 636), (0x5de98, 8),
        (0x34e34, 292), (0x34d18, 284), (0x363f8, 188), (0x2fbb4, 180),
        (0x364b4, 1160), (0x48cd8, 300), (0x36cdc, 56),
        (0x6da34, 3132), (0x6d020, 1008), (0x27610, 4), (0x62d8, 4),
        (0x79b08, 40), (0x79b58, 40),
        (0x69e60, 16), (0x67b15, 15), (0x69fd0, 16), (0x67c7c, 20),
        (0x6c030, 16), (0x68b0f, 24), (0x6c100, 16), (0x68c06, 21),
        (0x6c110, 16), (0x68c1b, 16), (0x6c1a0, 16), (0x68ccd, 20),
        (0x6c270, 16), (0x68de3, 20), (0x6c340, 16), (0x68ea0, 20),
        (0x6cd10, 16), (0x69835, 15), (0x6cda0, 16), (0x698ef, 28),
        (0x6cef0, 16), (0x69aaa, 24),
    )
    # (owning function, source section, source VA, record blob offset,
    # vendor type, symbol index, signed DECIMAL addend).
    RELA_GROUPS = (
        ("CmdChannelOpen", 16, (
            (0x24808, 0x729b4, 4, 19, 500), (0x2480c, 0x729c0, 6, 627, 0),
            (0x24888, 0x729cc, 4, 19, 520), (0x2488c, 0x729d8, 6, 537, 0),
            (0x248b4, 0x729e4, 4, 19, 552), (0x248c0, 0x729f0, 4, 622, 0),
            (0x248d0, 0x729fc, 4, 19, 816), (0x248d8, 0x72a08, 6, 627, 0),
            (0x24918, 0x72a14, 6, 627, 0), (0x2492c, 0x72a20, 6, 627, 0),
            (0x24944, 0x72a2c, 4, 794, 0), (0x24974, 0x72a38, 6, 627, 0),
            (0x249dc, 0x72a44, 6, 627, 0), (0x24a14, 0x72a50, 6, 824, 0),
            (0x24a20, 0x72a5c, 6, 824, 0), (0x24a44, 0x72a68, 6, 627, 0),
            (0x24aa4, 0x72a74, 6, 624, 0), (0x24b3c, 0x72a80, 6, 645, 0),
            (0x24b44, 0x72a8c, 6, 644, 0), (0x24b5c, 0x72a98, 6, 645, 0),
            (0x24b64, 0x72aa4, 6, 644, 0), (0x24b6c, 0x72ab0, 4, 794, 0))),
        ("Core_ChanInitialize", 16, (
            (0x26758, 0x733bc, 6, 604, 0), (0x26760, 0x733c8, 6, 824, 0),
            (0x26768, 0x733d4, 6, 824, 0), (0x26774, 0x733e0, 6, 604, 0),
            (0x26784, 0x733ec, 4, 27, 1792))),
        ("System_Activate", 4, (
            (0x9fb8, 0x6e028, 4, 622, 0), (0x9fcc, 0x6e034, 4, 621, 0),
            (0xa02c, 0x6e040, 6, 669, 0), (0xa038, 0x6e04c, 6, 687, 0),
            (0xa044, 0x6e058, 6, 704, 0), (0xa050, 0x6e064, 6, 727, 0),
            (0xa05c, 0x6e070, 6, 741, 0), (0xa068, 0x6e07c, 6, 773, 0),
            (0xa074, 0x6e088, 6, 748, 0), (0xa07c, 0x6e094, 6, 823, 0))),
        ("Core_CopyDramToLsram", 4, (
            (0x9eec, 0x6dfec, 6, 644, 0), (0x9efc, 0x6dff8, 6, 646, 0),
            (0x9f18, 0x6e004, 6, 641, 0), (0x9f48, 0x6e010, 6, 644, 0),
            (0x9f5c, 0x6e01c, 6, 641, 0))),
        ("Core_CircBuffer_Put", 4, (
            (0xb584, 0x6e478, 6, 646, 0), (0xb58c, 0x6e484, 6, 644, 0),
            (0xb5c4, 0x6e490, 4, 19, 1884), (0xb5c8, 0x6e49c, 6, 627, 0),
            (0xb5d0, 0x6e4a8, 6, 629, 0))),
        ("Core_AttemptDisplay", 4, (
            (0xb648, 0x6e4b4, 4, 621, 512), (0xb65c, 0x6e4c0, 4, 621, 0),
            (0xb68c, 0x6e4cc, 4, 621, 1536), (0xb6b8, 0x6e4d8, 6, 646, 0),
            (0xb6c0, 0x6e4e4, 6, 644, 0), (0xb6e8, 0x6e4f0, 6, 646, 0),
            (0xb6f0, 0x6e4fc, 6, 644, 0), (0xb724, 0x6e508, 4, 621, 1024),
            (0xb81c, 0x6e514, 4, 621, 1024), (0xb8fc, 0x6e520, 6, 645, 0),
            (0xb904, 0x6e52c, 6, 644, 0), (0xb938, 0x6e538, 6, 645, 0),
            (0xb940, 0x6e544, 6, 644, 0), (0xb988, 0x6e550, 6, 802, 0),
            (0xba4c, 0x6e55c, 4, 19, 1920), (0xba50, 0x6e568, 6, 627, 0),
            (0xba58, 0x6e574, 6, 626, 0))),
        ("Core_GetUndeliveredPPBs", 16, (
            (0x2615c, 0x73164, 4, 621, 1536), (0x26168, 0x73170, 4, 19, 1840),
            (0x2616c, 0x7317c, 6, 627, 0), (0x26170, 0x73188, 6, 637, 0),
            (0x26184, 0x73194, 4, 621, 1024), (0x261d4, 0x731a0, 6, 578, 0),
            (0x261e0, 0x731ac, 6, 637, 0), (0x261f4, 0x731b8, 4, 621, 1024),
            (0x26230, 0x731c4, 4, 19, 1840), (0x26234, 0x731d0, 6, 627, 0),
            (0x26264, 0x731dc, 6, 578, 0))),
        ("Platform_UpdateReleaseQueue", 4, (
            (0xbe40, 0x6e5d4, 4, 622, 0), (0xbe5c, 0x6e5e0, 4, 794, 0))),
    )
    SYMBOL_SHAPES = (
        ("CmdChannelOpen", 47, 16, 0x24788, 1068),
        ("Core_CircBuffer_Put", 70, 4, 0xb554, 188),
        ("Core_GetUndeliveredPPBs", 588, 16, 0x26144, 300),
        ("Core_CopyDramToLsram", 601, 4, 0x9e74, 284),
        ("System_Activate", 602, 4, 0x9f90, 292),
        ("Core_AttemptDisplay", 611, 4, 0xb610, 1160),
        ("Core_ChanInitialize", 624, 16, 0x266f8, 636),
        ("Core_CircBuffer_Get", 637, 2, 0x4d10, 180),
        ("dm_return_info", 794, 21, 0x78620, 128),
        ("Platform_UpdateReleaseQueue", 803, 4, 0xbe38, 56),
        ("Platform_DrvContextSize", 824, 16, 0x3b304, 8),
    )
    INHERITED_SYMBOL_INDICES = (46, 554, 556, 623, 640, 644, 645, 646, 800, 801)

    @staticmethod
    def sites(specification):
        result = []
        for item in specification.split():
            if item.startswith("!"):
                result.remove(int(item[1:], 16))
            elif ".." in item:
                start, end = (int(part, 16) for part in item.split(".."))
                result.extend(range(start, end + 4, 4))
            else:
                result.append(int(item, 16))
        return result

    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]
        cls.elf_base = 0x2ea60
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", cls.payload, cls.elf_base)
        cls.sections = [struct.unpack_from("<10I", cls.payload, cls.elf_base + header[6] + i * 40)
                        for i in range(header[12])]
        cls.arm_sites = [address for group in cls.ARM_GROUPS for address in cls.sites(group)]
        cls.arc_sites = [(section, address) for section, group in cls.ARC_GROUPS for address in cls.sites(group)]

    word = FirmwareFreshInitCausalTests.word
    arc_offset = FirmwareFreshInitCausalTests.arc_offset
    immediate = staticmethod(FirmwareFreshInitCausalTests.immediate)

    def mapping(self, payload=None, images=None):
        return MAP._open_reply_metadata_linkage(self.payload if payload is None else payload,
                                               self.images if images is None else images)

    def symbol(self, index):
        position = self.elf_base + self.sections[35][4] + index * 16
        record = struct.unpack_from("<IIIBBH", self.payload, position)
        names = self.elf_base + self.sections[34][4]
        end = self.payload.index(b"\0", names + record[0])
        return position, record, self.payload[names + record[0]:end].decode("ascii")

    def arc_function_owner(self, section, address):
        # Independent selected definitions, including the inherited dispatch
        # owner, never a lookup in the mapper's returned ownership narrative.
        definitions = self.SYMBOL_SHAPES + (("Core_Command", 554, 16, 0x25808, 516),)
        owners = [name for name, _, owner, start, size in definitions
                  if owner == section and start <= address < start + size]
        self.assertEqual(len(owners), 1, (section, hex(address)))
        return owners[0]

    def arc_value(self, section, address):
        word = self.word(self.arc_offset(section, address))
        source, operand = word >> 15 & 63, word >> 9 & 63
        if 62 in (source, operand):
            return self.word(self.arc_offset(section, address + 4))
        return (word & 511) - (512 if word & 256 else 0)

    def arc_displacement(self, section, address):
        word = self.word(self.arc_offset(section, address))
        return (word & 511) - (512 if word & 256 else 0)

    def union_pins(self):
        pins = [(offset, size) for _, offset, size, _ in MAP._OPEN_REPLY_REGIONS]
        pins += [(offset, size) for _, offset, size, _ in MAP._INIT_REPLY_REGIONS]
        pins += [(offset, size) for _, offset, size, _ in MAP._FRESH_INIT_REGIONS]
        pins += [(offset, len(raw) // 2) for _, offset, raw in MAP._COMMAND_BUFFER_BRIDGE_REGIONS]
        pins.append((0x72780, 0x65c4))
        pins += [(offset, len(raw) // 2) for _, offset, raw in MAP._INNER_DESCRIPTOR_HEADERS]
        for _, _, offset, raw, name_offset, name in MAP._INNER_DESCRIPTOR_SECTIONS:
            pins.append((offset, len(raw) // 2))
            if name != ".shstrtab":
                pins.append((name_offset, len(name.encode("ascii")) + 1))
        pins += [(offset, len(raw) // 2) for _, _, _, _, offset, raw in MAP._INNER_DESCRIPTOR_WINDOWS]
        return pins

    @staticmethod
    def stop_interpretation(stack):
        # Bounds/hash operations may run; no dependency, opcode, or ELF field
        # interpretation may start while any member of the union is unpinned.
        for name in ("_init_reply_metadata_linkage", "_inner_descriptor_map",
                     "_fresh_init_causal_contract", "_command_buffer_bridge_map",
                     "_stock_host_handler_footprints", "_open_reply_arm_operand",
                     "_open_reply_arc_operand", "_bootstrap_word", "_a32_branch", "_a32_literal"):
            stack.enter_context(mock.patch.object(MAP, name,
                                side_effect=AssertionError("interpretation before complete OPEN union: " + name)))
        for name in ("unpack", "unpack_from"):
            stack.enter_context(mock.patch.object(MAP.struct, name,
                                side_effect=AssertionError("ELF interpretation before complete OPEN union")))

    def test_every_new_pin_byte_and_all_dependency_words_tails_precede_interpretation(self):
        self.assertEqual({(offset, size) for _, offset, size, _ in MAP._OPEN_REPLY_REGIONS},
                         set(self.PIN_SHAPES))
        pins = self.union_pins()
        self.assertEqual((len(pins), sum(size for _, size in pins)), (216, 61651))
        changed = bytearray(self.payload)
        with ExitStack() as stack:
            self.stop_interpretation(stack)
            for offset, size in self.PIN_SHAPES:
                for delta in range(size):
                    changed[offset + delta] ^= 1
                    try:
                        with self.subTest(new_pin=hex(offset), byte=delta), self.assertRaises(MAP.FormatError):
                            self.mapping(changed)
                    finally:
                        changed[offset + delta] ^= 1
            for offset, size in pins[len(self.PIN_SHAPES):]:
                for delta in sorted(set(range(0, size, 4)) | {size - 1}):
                    changed[offset + delta] ^= 1
                    try:
                        with self.subTest(dependency_pin=hex(offset), byte=delta), self.assertRaises(MAP.FormatError):
                            self.mapping(changed)
                    finally:
                        changed[offset + delta] ^= 1
        self.assertEqual(changed, self.payload)
        # The final inner-descriptor window is particularly easy to pre-pin
        # too late when INIT and OPEN dependencies are interpreted separately.
        self.assertEqual(pins[-1], (0xb7e41, 56))

    def test_exact_one_less_new_and_every_inherited_preflight_budget(self):
        defaults = {
            "MAX_OPEN_REPLY_REGIONS": 48, "MAX_OPEN_REPLY_BYTES": 14 * 1024,
            "MAX_OPEN_REPLY_AGGREGATE_BYTES": 64 * 1024, "MAX_OPEN_REPLY_INSTRUCTIONS": 640,
            "MAX_OPEN_REPLY_ELF_RECORDS": 16, "MAX_OPEN_REPLY_OWNED_RELOCATIONS": 80,
            "MAX_OPEN_REPLY_SEMANTIC_RECEIPTS": 736, "MAX_OPEN_REPLY_NEW_TABLE_RECORDS": 345,
            "MAX_OPEN_REPLY_RELOCATION_RECORDS": 2516,
            "MAX_INIT_REPLY_REGIONS": 12, "MAX_INIT_REPLY_BYTES": 1024,
            "MAX_INIT_REPLY_AGGREGATE_BYTES": 80 * 1024, "MAX_INIT_REPLY_ANCHORS": 104,
            "MAX_FRESH_INIT_REGIONS": 48, "MAX_FRESH_INIT_BYTES": 16 * 1024,
            "MAX_FRESH_INIT_AGGREGATE_BYTES": 80 * 1024, "MAX_FRESH_INIT_ANCHORS": 160,
            "MAX_FRESH_INIT_EVENTS": 64, "MAX_COMMAND_BUFFER_BRIDGE_REGIONS": 80,
            "MAX_COMMAND_BUFFER_BRIDGE_BYTES": 40 * 1024, "MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS": 2171,
            "MAX_INNER_DESCRIPTOR_REGIONS": 48, "MAX_INNER_DESCRIPTOR_BYTES": 4096,
        }
        for name, value in defaults.items():
            self.assertEqual(getattr(MAP, name), value, name)
        uses = {
            "MAX_OPEN_REPLY_REGIONS": 43, "MAX_OPEN_REPLY_BYTES": 13299,
            "MAX_OPEN_REPLY_AGGREGATE_BYTES": 61651, "MAX_OPEN_REPLY_INSTRUCTIONS": 637,
            "MAX_OPEN_REPLY_ELF_RECORDS": 13, "MAX_OPEN_REPLY_OWNED_RELOCATIONS": 77,
            "MAX_OPEN_REPLY_SEMANTIC_RECEIPTS": 727, "MAX_OPEN_REPLY_NEW_TABLE_RECORDS": 345,
            "MAX_OPEN_REPLY_RELOCATION_RECORDS": 2516,
            "MAX_INIT_REPLY_REGIONS": 4, "MAX_INIT_REPLY_BYTES": 85,
            "MAX_INIT_REPLY_AGGREGATE_BYTES": 45313, "MAX_INIT_REPLY_ANCHORS": 104,
            "MAX_FRESH_INIT_REGIONS": 41, "MAX_FRESH_INIT_BYTES": 8536,
            "MAX_FRESH_INIT_AGGREGATE_BYTES": 45228, "MAX_FRESH_INIT_ANCHORS": 159,
            "MAX_FRESH_INIT_EVENTS": 35, "MAX_STOCK_HOST_COMMAND_CFG_STATES": 53,
            "MAX_COMMAND_BUFFER_BRIDGE_REGIONS": 80, "MAX_COMMAND_BUFFER_BRIDGE_BYTES": 36692,
            "MAX_COMMAND_BUFFER_BRIDGE_RELOCATIONS": 2171,
            "MAX_INNER_DESCRIPTOR_REGIONS": 48, "MAX_INNER_DESCRIPTOR_BYTES": 3039,
        }
        for name, use in uses.items():
            with self.subTest(exact=name), mock.patch.object(MAP, name, use):
                self.assertEqual(self.mapping()["validation"]["additional_semantic_count"], 727)
            with self.subTest(one_less=name), ExitStack() as stack:
                stack.enter_context(mock.patch.object(MAP, name, use - 1))
                self.stop_interpretation(stack)
                with self.assertRaisesRegex(MAP.FormatError, "budget"):
                    self.mapping()

    def test_truncated_and_malformed_identity_inputs_fail_before_dependencies(self):
        with ExitStack() as stack:
            self.stop_interpretation(stack)
            for length in (0, 0x275c0, 0x47700, 0x515ac, 0x6cd10, 0x79b58, len(self.payload) - 1):
                with self.subTest(length=length), self.assertRaises(MAP.FormatError):
                    self.mapping(self.payload[:length])
            for images in ([], self.images[:1], self.images[::-1], self.images * 2):
                with self.subTest(image_count=len(images)), self.assertRaises(MAP.FormatError):
                    self.mapping(images=images)
            for key, value in (("class", 64), ("endianness", "big"), ("machine", 93),
                               ("elf_type", 3), ("flags", 1), ("section_count", 54),
                               ("blob_file_offset", 0), ("blob_file_end", 0x79ddc)):
                images = [dict(image) for image in self.images]
                images[0][key] = value
                with self.subTest(identity=key), self.assertRaises(MAP.FormatError):
                    self.mapping(images=images)

    def test_independent_site_inventory_and_open_return_object(self):
        self.assertEqual((len(self.arm_sites), len(self.arc_sites)), (308, 329))
        self.assertEqual(len(set(self.arm_sites)), 308)
        self.assertEqual(len(set(self.arc_sites)), 329)
        self.assertEqual((len(self.PIN_SHAPES), sum(size for _, size in self.PIN_SHAPES)), (43, 13299))
        self.assertEqual(sum(len(records) for _, _, records in self.RELA_GROUPS), 77)
        # Captured directly from the bundled ELF/raw ARM bytes, separately
        # from the mapper's site words and SHA manifest.
        aggregate = hashlib.sha256()
        sites = [(0, address, address) for address in self.arm_sites]
        sites += [(section, address, self.arc_offset(section, address)) for section, address in self.arc_sites]
        for section, address, offset in sorted(sites):
            aggregate.update(struct.pack("<3I", section, address, self.word(offset)))
        self.assertEqual(aggregate.hexdigest(), "34899bf3a17e1944c6d3e5d5e454ded33be4af3379bd2200649aaecc313a3955")
        aggregate = hashlib.sha256()
        for offset, size in sorted(self.PIN_SHAPES):
            aggregate.update(struct.pack("<2I", offset, size))
            aggregate.update(hashlib.sha256(self.payload[offset:offset + size]).digest())
        self.assertEqual(aggregate.hexdigest(), "6cba127dbb34b912e17df31f99fcc54f36d3f04ec9e31e1af6e361807053624c")
        position, record, name = self.symbol(794)
        self.assertEqual((position, record, name),
                         (0x6cd10, (0x1da0, 0x78620, 128, 0x11, 0, 21), "dm_return_info"))
        self.assertEqual(self.sections[21], (0x260, 8, 3, 0x77138, 0x35734, 0x1654, 0, 0, 4, 1))
        self.assertTrue(self.sections[21][3] <= record[1] < record[1] + record[2] <=
                        self.sections[21][3] + self.sections[21][5])
        for address, offset, record_position in ((0x24944, 0x474d8, 0x72a2c),
                                                  (0x24b6c, 0x47700, 0x72ab0)):
            self.assertEqual(self.arc_offset(16, address), offset)
            self.assertEqual(self.word(offset), 0x78620)
            self.assertEqual(struct.unpack_from("<IIi", self.payload, record_position),
                             (address, 0x31a04, 0))
        # A separate data use exists: uniqueness is per owned selected field.
        self.assertEqual(struct.unpack_from("<IIi", self.payload, 0x6e5e0), (0xbe5c, 0x31a04, 0))

    def arm_oracle(self, address):
        word = self.word(address)
        out = {"architecture": "ARM", "blob_file_offset": address, "word": word,
               "condition": word >> 28}
        rn, rd, rm = word >> 16 & 15, word >> 12 & 15, word & 15
        if word >> 25 & 7 == 5:
            displacement = word & 0xffffff
            if displacement >= 0x800000:
                displacement -= 0x1000000
            out.update(operation="BL" if word >> 24 & 1 else "B",
                       target_blob_file_offset=address + 8 + displacement * 4)
        elif word & 0x0ffffff0 == 0x012fff10:
            out.update(operation="BX register", operand_register=rm)
        elif word & 0x0fc000f0 == 0x00000090:
            out.update(operation="MUL", destination_register=rn, source_register=rm,
                       operand_register=word >> 8 & 15, sets_flags=bool(word >> 20 & 1))
        elif word & 0xffff0000 in (0xe92d0000, 0xe8bd0000):
            out.update(operation="PUSH" if word >> 16 == 0xe92d else "POP", base_register=13,
                       register_mask=word & 65535, byte_count=(word & 65535).bit_count() * 4)
        elif word >> 25 & 7 == 4:
            out.update(operation="LDM" if word >> 20 & 1 else "STM", base_register=rn,
                       register_mask=word & 65535, byte_count=(word & 65535).bit_count() * 4,
                       addressing="increment before" if word >> 24 & 1 else "increment after",
                       writeback=bool(word >> 21 & 1))
        elif word & 0x0ff00000 == 0x03000000:
            out.update(operation="MOVW", destination_register=rd,
                       immediate=(word >> 4 & 0xf000) | (word & 4095))
        elif word >> 26 & 3 == 1:
            if rn == 15 and word >> 20 & 1:
                literal = address + 8 + (word & 4095) * (1 if word >> 23 & 1 else -1)
                out.update(operation="LDR literal", destination_register=rd,
                           literal_blob_file_offset=literal, literal_value=self.word(literal))
            else:
                out.update(operation="LDR" if word >> 20 & 1 else "STR", base_register=rn,
                           data_register=rd, byte_width=1 if word >> 22 & 1 else 4)
                if word >> 25 & 1:
                    out.update(offset_register=rm, shift_kind="LSL", shift_amount=word >> 7 & 31)
                else:
                    out["byte_offset"] = (word & 4095) * (1 if word >> 23 & 1 else -1)
        elif word & 0x0e000090 == 0x00000090:
            operation = {0xb0: "LDRH", 0xd0: "LDRD", 0xf0: "STRD"}[word & 0xf0]
            out.update(operation=operation, base_register=rn, data_register=rd,
                       byte_width=2 if operation == "LDRH" else 8,
                       byte_offset=(word >> 4 & 0xf0) | (word & 15))
            if operation != "LDRH":
                out["second_data_register"] = rd + 1
        else:
            opcode = word >> 21 & 15
            out.update(operation={2: "SUB", 4: "ADD", 8: "TST", 10: "CMP", 13: "MOV", 15: "MVN"}[opcode],
                       source_register=rn, destination_register=rd)
            if word >> 25 & 1:
                out["immediate"] = self.immediate(word)
            else:
                out.update(operand_register=rm, shift_kind="LSL", shift_amount=word >> 7 & 31)
        return out

    def test_all637_returned_instructions_match_independent_operand_oracles(self):
        records = self.mapping()["instruction_anchors"]
        arm = [r for r in records if r["architecture"] == "ARM"]
        arc = [r for r in records if r["architecture"] == "ARC"]
        self.assertEqual((len(records), len(arm), len(arc)), (637, 308, 329))
        self.assertEqual({r["blob_file_offset"] for r in arm}, set(self.arm_sites))
        self.assertEqual({(r["section_index"], r["elf_virtual_address"]) for r in arc}, set(self.arc_sites))
        for record in arm:
            with self.subTest(arm=hex(record["blob_file_offset"])):
                expected = self.arm_oracle(record["blob_file_offset"])
                actual = {key: value for key, value in record.items() if key != "source_function"}
                self.assertEqual(actual, expected)
                address = record["blob_file_offset"]
                owners = [role for role, start, size in (("host_open", 0x51c8, 2112), ("channel_wrapper", 0x8a0, 760),
                          ("smp_open", 0xa2a4, 272), ("ordinary_decoder_open", 0xf7e4, 1032),
                          ("open_packet_builder", 0x27480, 324)) if start <= address < start + size]
                expected_owner = owners[0] if owners else ("fixed_context_getter" if address in (0x898, 0x89c)
                                                          else "selected_dispatch_call_site_only")
                self.assertEqual(record["source_function"], expected_owner)
        majors = {0: "LD indexed", 1: "LD", 2: "ST", 3: "EXT", 4: "B", 5: "BL", 6: "LP",
                  7: "J", 8: "ADD", 9: "ADC", 10: "SUB", 11: "SBC", 12: "AND", 13: "OR",
                  14: "BIC", 15: "XOR", 16: "ASL"}
        for record in arc:
            section, address = record["section_index"], record["elf_virtual_address"]
            offset = self.arc_offset(section, address)
            word = self.word(offset)
            major = word >> 27
            expected = {"architecture": "ARC", "section_index": section, "elf_virtual_address": address,
                        "blob_file_offset": offset, "word": word, "decode_conditional": True,
                        "opcode_major": major, "destination_register": word >> 21 & 63,
                        "source_register": word >> 15 & 63, "operand_register": word >> 9 & 63,
                        "low9": word & 511, "signed_low9": (word & 511) - (512 if word & 256 else 0)}
            operation = majors[major]
            if major == 12 and expected["source_register"] == expected["operand_register"]:
                operation = "MOV"
            expected["operation"] = operation
            expected["source_function"] = self.arc_function_owner(section, address)
            if major in (4, 5, 6):
                displacement = word >> 7 & 0xfffff
                if displacement >= 0x80000:
                    displacement -= 0x100000
                expected.update(condition=word & 31, pc_bias_bytes=4,
                                target_elf_virtual_address=address + 4 + displacement * 4)
            if major in (4, 5, 7):
                delay = word >> 5 & 3
                self.assertIn(delay, (0, 1, 2))
                expected["delay_slot_semantics"] = {0: "none", 1: "always executed", 2: "taken only"}[delay]
                if delay:
                    expected.update(delay_slot_elf_virtual_address=address + 4,
                                    delay_slot_word=self.word(self.arc_offset(section, address + 4)))
            literal_required = (major == 1 and expected["source_register"] == 62 or
                                major == 2 and expected["operand_register"] == 62 or
                                major in (0, 8, 9, 10, 11, 12, 13, 14, 15, 16) and
                                62 in (expected["source_register"], expected["operand_register"]))
            if literal_required:
                expected.update(literal_blob_file_offset=offset + 4, literal_value=self.word(offset + 4))
            if major == 16 and expected["operand_register"] == 63:
                expected["shift_amount"] = expected["signed_low9"]
            # No selected receipt may claim an extension's actual ISA semantics.
            if address == 0x24b48:
                expected["vendor_ISA_validated"] = False
            with self.subTest(arc=hex(address)):
                self.assertEqual(record, expected)

    def test_independent_arc_argument_delay_pool_ring_and_release_words(self):
        words = {
            (16, 0x258bc): 0x2ffdd920, (16, 0x258c0): 0x60079e00,
            (16, 0x247c4): 0x62400000, (16, 0x2487c): 0x57e07a09,
            (16, 0x24880): 0x2000028e, (16, 0x248ec): 0x57e6fa10,
            (16, 0x2493c): 0x8006fe03, (16, 0x24940): 0x40207c00,
            (16, 0x2494c): 0x10008004, (16, 0x24950): 0x10009e00,
            (16, 0x24aa4): 0x28038a20, (16, 0x24aa8): 0x08cd81f4,
            (16, 0x24b0c): 0x10090004, (16, 0x24b14): 0x10090208,
            (16, 0x24b1c): 0x1009000c, (16, 0x24b2c): 0x10090014,
            (16, 0x24b68): 0x41a6fc00, (16, 0x24b70): 0x10091a10,
            (16, 0x2676c): 0x41a02800, (16, 0x267a8): 0x4029fc00,
            (16, 0x267b0): 0x10009b30, (16, 0x268e8): 0x1001013c,
            (16, 0x2692c): 0x10008524, (16, 0x26958): 0x10008528,
            (16, 0x3b304): 0x380f8020, (16, 0x3b308): 0x401ffeec,
            (4, 0x9fa8): 0x80007e05, (4, 0x9fb4): 0x41e07c00,
            (4, 0xb598): 0x57e0fa02, (4, 0xb5a8): 0x57e0fa3f,
            (4, 0xb5b0): 0x57e6fa02, (4, 0xb5b8): 0x57e6fa3f,
            (4, 0xb5d4): 0x57e77a40, (4, 0xb5e0): 0x8006fe02,
            (4, 0xb5e8): 0x10001e00, (4, 0xb5ec): 0x10081c04,
            (4, 0xb8e4): 0x828b7e03, (4, 0xb8e8): 0x528a2c00,
            (4, 0xb8ec): 0x828a7e03, (4, 0xb8f0): 0x428a2c00,
            (4, 0xb8f4): 0x828a7e02, (4, 0xb8f8): 0x40292800,
            (4, 0xbe44): 0x0821040f, (4, 0xbe48): 0x57e0faff,
            (4, 0xbe50): 0x08210010, (4, 0xbe54): 0x80007e03,
            (4, 0xbe6c): 0x10008004,
        }
        for (section, address), word in words.items():
            with self.subTest(section=section, address=hex(address)):
                self.assertEqual(self.word(self.arc_offset(section, address)), word)
        call = words[16, 0x258bc]
        self.assertEqual((call >> 27, call & 31, call >> 5 & 3), (5, 0, 1))
        displacement = (call >> 7 & 0xfffff) - 0x100000
        self.assertEqual(0x258bc + 4 + displacement * 4, 0x24788)
        # BL.d's R15 -> R0 argument, then callee R0 -> R18 reply identity.
        self.assertEqual((words[16, 0x258c0] >> 9 & 63, words[16, 0x258c0] >> 21 & 63), (15, 0))
        self.assertEqual((words[16, 0x247c4] >> 9 & 63, words[16, 0x247c4] >> 21 & 63), (0, 18))
        self.assertEqual((words[16, 0x2487c] & 511, words[16, 0x248ec] & 511), (9, 16))
        self.assertEqual([words[16, p] & 511 for p in (0x24b0c, 0x24b14, 0x24b1c, 0x24b70, 0x24b2c)],
                         [4, 8, 12, 16, 20])
        self.assertEqual(words[16, 0x2493c] & 511, 3)  # Channel *8, not *4 or ARC context size.
        self.assertEqual([words[4, p] & 511 for p in (0xb598, 0xb5a8, 0xb5b0, 0xb5b8, 0xb5d4)],
                         [2, 63, 2, 63, 64])
        self.assertEqual([words[4, p] & 511 for p in (0xbe44, 0xbe48, 0xbe50, 0xbe6c)], [15, 255, 16, 4])
        # A source-derived scalar address, never an operational Y/UV surface.
        for index in (0, 1, 33):
            stride_from_opcodes = ((index << 3) - index) << 3
            stride_from_opcodes = (stride_from_opcodes + index) << 2
            self.assertEqual(stride_from_opcodes, 228 * index)

    def test_validation_receipt_counts_pins_and_inherited_dependency_scope(self):
        result = self.mapping()
        expected = {
            "additional_region_count": 43, "additional_byte_count": 13299,
            "dependency_region_count": 173, "dependency_byte_count": 48352,
            "aggregate_region_count": 216, "aggregate_byte_count": 61651,
            "arm_instruction_count": 308, "arc_instruction_count": 329, "instruction_count": 637,
            "fixed_elf_record_count": 13, "owned_relocation_count": 77,
            "scanned_relocation_count": 2516, "additional_semantic_count": 727,
        }
        self.assertEqual({key: value for key, value in result["validation"].items()
                          if key not in ("validated_regions", "dependency_regions")}, expected)
        pins = result["validation"]["validated_regions"]
        self.assertEqual({(r["blob_file_offset"], r["size"]) for r in pins}, set(self.PIN_SHAPES))
        for record in pins + result["validation"]["dependency_regions"]:
            offset, size = record["blob_file_offset"], record["size"]
            self.assertEqual(record["sha256"], hashlib.sha256(self.payload[offset:offset + size]).hexdigest())
        self.assertEqual(result["init_reply"], MAP._init_reply_metadata_linkage(self.payload, self.images))
        self.assertEqual(result["basis"], result["init_reply"]["basis"])
        for key in ("entire_payload_rehashed", "device_observed", "public_route"):
            self.assertIs(result["basis"][key], False)

    def test_all13_elf_and77_owned_relocation_receipts_match_raw_source_fields(self):
        result = self.mapping()
        expected_elf = []
        definitions = {}
        for name, index, section, address, size in self.SYMBOL_SHAPES:
            position, raw, raw_name = self.symbol(index)
            self.assertEqual((raw_name, raw[1], raw[2], raw[5]), (name, address, size, section))
            self.assertEqual((raw[3], raw[4]), (0x11 if index == 794 else 2 if index in (47, 70) else 0x12, 0))
            owner = self.sections[section]
            record = {"name": name, "symbol_index": index, "symbol_record_blob_file_offset": position,
                      "name_blob_file_offset": self.elf_base + self.sections[34][4] + raw[0],
                      "section_index": section, "elf_virtual_address": address, "size": size,
                      "info": raw[3], "other": raw[4], "section_relative_offset": address - owner[3]}
            if index != 794:
                record["blob_file_offset"] = self.arc_offset(section, address)
            expected_elf.append(record)
            definitions[name] = index, section, address, size
        for index, owner, count, table_offset in ((37, 2, 84, 0x6d020), (39, 4, 261, 0x6da34)):
            expected_elf.append({"operation": "RELA section header", "section_index": index,
                                 "source_section_index": owner, "linked_symbol_table_index": 35,
                                 "blob_file_offset": self.elf_base + 0x4aae0 + index * 40,
                                 "record_blob_file_offset": table_offset, "record_count": count, "entry_size": 12})
        self.assertEqual(result["elf_receipts"], expected_elf)
        expected_relocations = []
        for name, section, rows in self.RELA_GROUPS:
            for address, position, kind, target, addend in rows:
                actual = struct.unpack_from("<IIi", self.payload, position)
                self.assertEqual(actual, (address, target * 256 + kind, addend))
                row = {"source_function": name, "source_function_symbol_index": definitions[name][0],
                       "source_section_index": section, "source_elf_virtual_address": address,
                       "source_blob_file_offset": self.arc_offset(section, address),
                       "relocation_section_index": {2: 37, 4: 39, 16: 51}[section],
                       "relocation_record_blob_file_offset": position, "vendor_type": kind,
                       "symbol_index": target, "addend": addend,
                       "unpinned_target_definition_interpreted": False, "runtime_application_proven": False}
                if address in (0x24944, 0x24b6c, 0xbe5c):
                    row.update(selected_target_name="dm_return_info", patched_literal_offset_from_B=0x78620)
                expected_relocations.append(row)
        key = lambda row: row["relocation_record_blob_file_offset"]
        self.assertEqual(sorted(result["relocation_receipts"], key=key), sorted(expected_relocations, key=key))
        self.assertEqual((len(expected_elf), len(expected_relocations)), (13, 77))
        # Type4 addresses are raw data fields owned by their containing ARC
        # instructions. They must not inflate the 637 instruction receipts.
        addresses = {r["elf_virtual_address"] for r in result["instruction_anchors"] if r["architecture"] == "ARC"}
        self.assertTrue({0x24940, 0x24b68, 0xbe58} <= addresses)
        self.assertTrue({0x24944, 0x24b6c, 0xbe5c}.isdisjoint(addresses))

    def test_selected_arc_edges_bind_pc_bias_delay_owner_and_conditional_relocations(self):
        result = self.mapping()
        definitions = {}
        for index in self.INHERITED_SYMBOL_INDICES + tuple(s[1] for s in self.SYMBOL_SHAPES if s[1] != 794):
            _, raw, name = self.symbol(index)
            definitions[raw[1]] = name, index, raw[5]
        tables = {}
        for table_index, owner in ((37, 2), (39, 4), (51, 16)):
            section = self.sections[table_index]
            table_start = self.elf_base + section[4]
            for i in range(section[5] // 12):
                position = table_start + i * 12
                address, info, addend = struct.unpack_from("<IIi", self.payload, position)
                tables.setdefault((owner, address), []).append((position, info, addend))
        expected = []
        for section, address in self.arc_sites:
            word = self.word(self.arc_offset(section, address))
            if word >> 27 != 5:
                continue
            displacement = word >> 7 & 0xfffff
            if displacement >= 0x80000:
                displacement -= 0x100000
            target = address + 4 + displacement * 4
            delay = word >> 5 & 3
            row = {"source_function": self.arc_function_owner(section, address),
                   "source_section_index": section, "source_elf_virtual_address": address,
                   "original_target_elf_virtual_address": target, "pc_bias_bytes": 4,
                   "delay_slot_semantics": {0: "none", 1: "always executed", 2: "taken only"}[delay],
                   "runtime_edge_proven": False, "target_definition_validated": target in definitions}
            if delay:
                row["delay_slot_word"] = self.word(self.arc_offset(section, address + 4))
            relocations = tables.get((section, address), [])
            self.assertLessEqual(len(relocations), 1)
            if target in definitions:
                name, index, target_section = definitions[target]
                row["target_function"] = name
                if not relocations:
                    self.assertEqual(section, target_section)
                else:
                    position, info, addend = relocations[0]
                    self.assertEqual((info & 255, info >> 8), (6, index))
                    signed_delta = target + addend - address - 4
                    self.assertEqual(signed_delta % 4, 0)
                    self.assertEqual((word & 0xf800007f) | ((signed_delta << 5) & 0x07ffff80), word)
                    row["original_encoding_preserved_under_selected_rebase"] = True
            if relocations:
                position, info, addend = relocations[0]
                row.update(relocation_record_blob_file_offset=position, vendor_type=info & 255,
                           symbol_index=info >> 8, addend=addend)
            expected.append(row)
        key = lambda row: (row["source_section_index"], row["source_elf_virtual_address"])
        self.assertEqual(sorted(result["selected_arc_edges"], key=key), sorted(expected, key=key))
        by_source = {row["source_elf_virtual_address"]: row for row in result["selected_arc_edges"]}
        for source, target in ((0x258bc, 0x24788), (0x9fd0, 0x9e74), (0xb980, 0xb554)):
            self.assertEqual(by_source[source]["original_target_elf_virtual_address"], target)
            self.assertEqual(by_source[source]["delay_slot_semantics"], "always executed")
            self.assertNotIn("relocation_record_blob_file_offset", by_source[source])
        self.assertEqual(by_source[0x24aa4]["relocation_record_blob_file_offset"], 0x72a74)
        self.assertEqual(by_source[0x24aa4]["target_function"], "Core_ChanInitialize")

    def test_arm_output_before_status_and_distinct_open_translation_publication_contract(self):
        result = self.mapping()
        arm = result["arm_path"]
        self.assertEqual(arm["identities"], {"C": "ARM controller", "K": "ARM API host-channel record",
                         "H": "allocated ARM decoder/channel object", "D": "ARC channel context",
                         "F": "ARC record-pool source", "host_command_record": "valid incoming host request"})
        self.assertEqual({k: v for k, v in arm.items() if k not in ("identities", "builder", "publication")},
                         {"selected_dispatch_call_site": 0x62d8, "dispatch_selector_and_argument_continuity_validated": False,
                          "controller_root_global_offset": 8, "host_channel_record_stride_bytes": 0x1cc,
                          "host_channel_record_base_offset": 0x18, "ordinary_builder_requires_zero_selected_flag": True,
                          "special_channel_bypass_validated": False})
        builder = arm["builder"]
        self.assertEqual(builder, {
            "entry": 0x27480, "frame_bytes": 552, "request_stack_offset": 264, "response_stack_offset": 12,
            "packet_bytes": 252, "command": 0x73760002, "channel_reply_request_byte_offset": 4,
            "bank_count_request_byte_offset": 32, "bank_count_H_offset": 32, "timeout_argument": 20000,
            "transport_entry": 0x2705c, "saved_transport_status_stack_offset": 8,
            "raw_reply_to_H": [{"reply_byte_offset": 8, "H_offset": 0x44},
                               {"reply_byte_offset": 12, "H_offset": 0x48},
                               {"reply_byte_offset": 16, "H_offset": 0x50}],
            "writes_raw_outputs_before_transport_status_check": True,
            "translation": {"reply_byte_offset": 20, "map_C_offset": 8, "output_H_offset": 0x58,
                            "helper_entry": 0x1fdac, "store_precedes_bounds_check": True,
                            "helper_status_checked": False, "stored_output_validated": False,
                            "same_map_as_INIT_validated": False},
            "unconditional_marker_H_offset": 0x220, "marker_value": 1, "marker_proves_accepted_object": False,
            "returns_saved_transport_status": True})
        publication = arm["publication"]
        self.assertEqual(publication, {
            "transport_status_branch": 0xfb80, "zero_status_success_target": 0xfb94,
            "nonzero_status_cleanup_is_fallthrough": True, "failure_cleanup_call": 0xfb88,
            "cleanup_callee": 0xf6e4, "deallocation_semantics_validated": False,
            "normal_outptr_store": 0xfbb0, "controller_table_store": 0xfbcc, "controller_table_C_offset": 0x19c,
            "failure_suppresses_normal_publication": True, "postpublication_opaque_return_statuses_checked": False,
            "forced_success_value": 0})
        raw = {address: self.arm_oracle(address) for address in
               (0x274cc, 0x2757c, 0x27580, 0x27584, 0x275a0, 0x275a4, 0x275a8, 0x275ac,
                0x275b0, 0x275b4, 0x275b8, 0xfb08, 0xfb0c, 0xfb7c, 0xfb80, 0xfb84,
                0xfb88, 0xfb8c, 0xfb90, 0xfbb0, 0xfbcc, 0xfbd8, 0xfbe0, 0xfbe4)}
        self.assertEqual((raw[0x274cc]["destination_register"], raw[0x27580]["base_register"]), (8, 8))
        self.assertEqual(raw[0x2757c]["data_register"], 0)
        self.assertEqual((raw[0x2757c]["byte_offset"], raw[0x275b8]["byte_offset"]), (8, 8))
        self.assertEqual((raw[0x275a4]["base_register"], raw[0x275a4]["data_register"], raw[0x275a4]["byte_offset"]), (9, 0, 8))
        self.assertEqual((raw[0x275a0]["source_register"], raw[0x275a0]["destination_register"]), (6, 2))
        self.assertEqual(raw[0x275b8]["data_register"], 0)
        # The helper's return r0 is overwritten by marker1, then by the saved
        # transport status; no conditional branch checks helper status here.
        self.assertEqual((raw[0x275b0]["operation"], raw[0x275b0]["destination_register"], raw[0x275b0]["immediate"]), ("MOV", 0, 1))
        self.assertEqual((raw[0xfb7c]["source_register"], raw[0xfb7c]["immediate"], raw[0xfb80]["condition"]), (8, 0, 0))
        self.assertEqual(raw[0xfb80]["target_blob_file_offset"], 0xfb94)
        self.assertEqual((raw[0xfb84]["destination_register"], raw[0xfb84]["operand_register"]), (0, 4))
        self.assertEqual((raw[0xfb8c]["destination_register"], raw[0xfb8c]["operand_register"]), (0, 8))
        self.assertEqual(raw[0xfb90]["target_blob_file_offset"], 0xf830)
        self.assertEqual(raw[0xfbb0]["data_register"], 4)
        self.assertEqual((raw[0xfbe4]["destination_register"], raw[0xfbe4]["immediate"]), (0, 0))
        self.assertEqual(result["init_reply"]["arm_translations"]["map_context_offset"], 12)
        self.assertNotEqual(builder["translation"]["map_C_offset"], 12)
        # The inherited helper writes output before bounds checking and may
        # return2; its INIT-specific identity-map defaults cannot prove OPEN's
        # C+8 map valid. No new OPEN translation/queue emulator is introduced.
        translation = result["init_reply"]["arm_translations"]["translation"]
        self.assertEqual(translation["error_status"], 2)
        self.assertIs(builder["translation"]["stored_output_validated"], False)

    def test_arc_reply_pool_ring_and_snapshot_equations_remain_conditional_metadata(self):
        result = self.mapping()
        outer = result["outer_reply"]
        self.assertEqual(outer, {
            "conditional_on_selected_fresh_arc_execution": True, "command": 0x73760002,
            "status": 0, "reply_buffer_register": 18,
            "word2": {"interpretation": "delivery_metadata_ring", "D_field_offset": 0x524,
                      "offset_from_F": 0x14ee4, "offset_from_D": 0x15678},
            "word3": {"interpretation": "return_metadata_ring", "D_field_offset": 0x528,
                      "offset_from_F": 0x14fe4, "offset_from_D": 0x15778},
            "word4": {"interpretation": "dm_return_info channel entry", "offset_from_B": 0x78620,
                      "channel_stride_bytes": 8, "channel_count": 16, "word_count_per_channel": 2,
                      "channel_scale_vendor_ISA_validated": False},
            "bank_count_inclusive_max": 9, "zero_bank_count_rejected": False,
            "constructor_status_checked": False, "transport_checks_metadata_words": False,
            "return_metadata_initialized_words": [2, 0], "NOBITS_initialized_contents_proven": False})
        pool = result["record_pool"]
        self.assertEqual({k: v for k, v in pool.items() if k != "inherited_constructor_preservation"}, {
            "context_name": "D", "pool_name": "F", "F_offset_from_D": 0x794, "source_D_field_offset": 0x530,
            "selected_bank_count_discharges_nonclobber_bound": True, "metadata_pool_D_field_offset": 0x33c,
            "metadata_pool_offset_from_F": 0x150e4, "metadata_pool_offset_from_D": 0x15878,
            "allocation_extent_validated": False, "runtime_pool_identity_validated": False})
        inner = MAP._inner_descriptor_map(self.payload, self.images)
        self.assertEqual(pool["inherited_constructor_preservation"],
                         inner["paths"]["record_pointer_and_boundary"]["record_pool_context_snapshot"]["conditional_constructor_return"])
        self.assertLessEqual(outer["bank_count_inclusive_max"],
                             pool["inherited_constructor_preservation"]["source_initialization"]["earlier_variable_loop"]["maximum_nonclobbering_count"])
        self.assertEqual(result["rings"], {
            "header_word_count": 2, "header_bytes": 8, "index_unit_bytes": 4,
            "index_inclusive_min": 2, "index_inclusive_max": 63, "data_entry_count": 62,
            "declared_index_extent_bytes": 256, "ring_base_gap_bytes": 256,
            "allocation_extent_validated": False, "full_ring_protection_validated": False,
            "invalid_put_can_fall_through_if_opaque_callees_return": True,
            "get_empty_and_invalid_both_return_zero": True, "get_MMIO_completion_bounded": False,
            "selected_delivery_payload": {"interpretation": "PPB metadata record address", "record_stride_bytes": 228,
                                          "copy_bytes": 228, "record_count": 34,
                                          "index_range_runtime_validated": False, "raw_source_plane": False},
            "release_queue": {"slot_byte_gate_offset": 15, "gate_skip_value": 255, "published_return_info_word": 0,
                              "destination_ring_D_field_offset": 0x528, "destination_header_byte_offset": 4,
                              "stored_index_validated": False}})
        snapshot = result["activation_snapshot"]
        self.assertEqual(snapshot, {
            "channel_table_local_base": 0x3fffd378, "channel_stride_bytes": 32, "copy_bytes": 0x5bc,
            "local_destination": 0x3fffcdac, "delivery_ring_field_local_address": 0x3fffd2d0,
            "return_ring_field_local_address": 0x3fffd2d4, "metadata_pool_field_local_address": 0x3fffd0e8,
            "successful_byte_preserving_copy_assumed": True, "same_channel_and_unchanged_entry_assumed": True,
            "runtime_active_context_validated": False})
        # Bind the equations independently to original immediate/LIMM values;
        # equal addresses under this conditional copy are not live identity.
        self.assertEqual(self.arc_value(16, 0x267a8) + self.arc_displacement(16, 0x267b0), 0x530)
        self.assertEqual(self.arc_value(16, 0x267b8) + self.arc_displacement(16, 0x268e8), 0x33c)
        self.assertEqual(self.arc_value(16, 0x26924), 0x14ee4)
        self.assertEqual(self.arc_value(16, 0x26938), 0x14fe4)
        self.assertEqual(self.arc_value(16, 0x268e0), 0x150e4)
        self.assertEqual(self.arc_value(4, 0x9fc8) + self.arc_displacement(4, 0x9fd4), snapshot["local_destination"])
        for word, field in (("word2", "delivery_ring_field_local_address"), ("word3", "return_ring_field_local_address")):
            self.assertEqual(pool["F_offset_from_D"] + outer[word]["offset_from_F"], outer[word]["offset_from_D"])
            self.assertEqual(snapshot["local_destination"] + outer[word]["D_field_offset"], snapshot[field])
        self.assertEqual(snapshot["local_destination"] + pool["metadata_pool_D_field_offset"], snapshot["metadata_pool_field_local_address"])
        self.assertEqual(self.arc_value(16, 0x2487c), 9)
        self.assertEqual(self.arc_value(16, 0x248ec), 16)
        self.assertEqual(self.arc_value(16, 0x247f0), outer["return_metadata_initialized_words"][0])
        self.assertEqual((self.word(self.arc_offset(16, 0x2493c)) & 511,
                          self.word(self.arc_offset(16, 0x24b48)) & 511), (3, 3))
        for channel in (0, 1, 15):
            self.assertLessEqual(8 * channel + 8, self.symbol(794)[1][2])
        self.assertGreater(8 * 16 + 8, self.symbol(794)[1][2])
        # Index64 wraps to2, not0; 62 payload DWORDs do not prove a full-ring
        # protocol or any allocation's operational capacity.
        self.assertEqual(self.arc_value(4, 0xb5d4), 64)
        self.assertEqual(self.arc_value(4, 0xb5d8), 2)
        self.assertEqual(self.arc_displacement(4, 0xbe44), 15)
        self.assertEqual(self.arc_value(4, 0xbe48), 255)
        self.assertEqual(self.arc_value(4, 0xbe60), 0x528)
        self.assertEqual(self.arc_displacement(4, 0xbe6c), 4)

    def test_cabac_saved_state_constructor_reset_and_copy_provenance(self):
        # Static stock-source regression, not an ARC interpreter or a device
        # observation. No native sample values or local-memory reads are used.
        functions = (
            (624, "Core_ChanInitialize", 16, 0x266f8, 636, "11aeb52ef9f2a3c8ab99cf099c75eade7a69abe991e3561a05f423e54b1b4d71"),
            (625, "Core_StartChannel", 16, 0x26974, 556, "8fe220a7487341e1975615f11a7553681f25ef618556f6f03838a87327d8be4b"),
            (602, "System_Activate", 4, 0x9f90, 292, "9530baaafd18492433c0f9fdca77cfcd8d0ad1aa982e221303d6cd54de17521e"),
            (601, "Core_CopyDramToLsram", 4, 0x9e74, 284, "a78e06a80f25a603981e888fa35ccf9e811dca10acdc4d71b919bd8902f88a1e"),
            (600, "System_Deactivate", 4, 0x9d48, 300, "f185b940f905ee5c4e28f097434d8757b3e5e9a5ccba6fe47422caf25e892e20"),
            (599, "Core_CopyLsramToDram", 4, 0x9c3c, 268, "32f163266b8f614b9e46b94bc4a9b20863606b0cbad567b3b6a496f5d7866c64"),
            (570, "Core_GetCabacWorklist", 4, 0x8614, 324, "02b7f103c04f61afb2f0bcc74caab124a8ee1916d9cf87758054b1dc129f1472"),
        )
        for index, name, section, start, size, digest in functions:
            with self.subTest(function=name):
                _, symbol, actual_name = self.symbol(index)
                self.assertEqual((actual_name, symbol[1:]), (name, (start, size, 0x12, 0, section)))
                owner = self.sections[section]
                self.assertEqual((owner[1], owner[2] & 6), (1, 6))
                self.assertTrue(owner[3] <= start < start + size <= owner[3] + owner[5])
                offset = self.arc_offset(section, start)
                self.assertEqual(hashlib.sha256(self.payload[offset:offset + size]).hexdigest(), digest)

        # Pin opcodes AND register operands before using signed displacements.
        # Constructor: r19=D, r17=incoming R; [fp+16]=N, r1=D+0x600.
        words = {
            16: {0x2672c: 0x62600000, 0x2675c: 0x62238e00, 0x267a8: 0x4029fc00,
                 0x26820: 0x080d8010, 0x26824: 0x1000a344, 0x2682c: 0x1000a34c,
                 0x26830: 0x40002200, 0x26834: 0x10008148, 0x26838: 0x1000a350,
                 0x26998: 0x61df7c00, 0x269a0: 0x40277f08, 0x269a4: 0x80007e05,
                 0x269a8: 0x40208000, 0x269ac: 0x0a008000, 0x269bc: 0x41e87c00,
                 0x26a84: 0x08078144, 0x26a94: 0x1007814c, 0x26a98: 0x10078150},
            4: {0x9fa8: 0x80007e05, 0x9fb4: 0x41e07c00, 0x9fbc: 0x08078010,
                0x9fc0: 0x605f7c00, 0x9fc8: 0x61df7c00, 0x9fd0: 0x2fffd420,
                0x9fd4: 0x40277e3c, 0x9d60: 0x607f7c00, 0x9d80: 0x80007e05,
                0x9d84: 0x41a07c00, 0x9d98: 0x08268010, 0x9d9c: 0x605f7c00,
                0x9da4: 0x2fffd2a0, 0x9da8: 0x4001fe3c,
                0x8614: 0x607f7c00, 0x861c: 0x0821818c, 0x8620: 0x08a18188,
                0x8634: 0x08818184, 0x8654: 0x08018180,
                0x873c: 0x10018188, 0x8744: 0x10018188},
        }
        for section, sites in words.items():
            for address, expected in sites.items():
                with self.subTest(section=section, instruction=hex(address)):
                    self.assertEqual(self.word(self.arc_offset(section, address)), expected)
        # Original relocation records, including local literals and cross-
        # section copy callees. Same-section copy calls have no owned RELA.
        for offset, source, index, kind, addend in (
                (0x72a74, 0x24aa4, 624, 6, 0), (0x72bf4, 0x24eb4, 625, 6, 0),
                (0x733f8, 0x2699c, 27, 4, 0x700), (0x6db78, 0x8618, 621, 4, 0x600),
                (0x6db84, 0x8734, 621, 4, 0x200), (0x6df2c, 0x9cb0, 645, 6, 0),
                (0x6df50, 0x9d14, 645, 6, 0), (0x6dff8, 0x9efc, 646, 6, 0),
                (0x6e004, 0x9f18, 641, 6, 0)):
            self.assertEqual(struct.unpack_from("<IIi", self.payload, offset),
                             (source, index << 8 | kind, addend))
        relocations = self.sections[39]
        self.assertEqual((relocations[1], relocations[5:8], relocations[9]), (4, (3132, 35, 4), 12))
        relocated = [self.word(self.elf_base + relocations[4] + offset)
                     for offset in range(0, relocations[5], 12)]
        self.assertNotIn(0x9da4, relocated)
        self.assertNotIn(0x9fd0, relocated)
        initialized = [self.arc_value(16, 0x267a8) + self.arc_displacement(16, address)
                       for address in (0x26824, 0x26834, 0x2682c, 0x26838)]
        self.assertEqual(initialized, [0x544, 0x548, 0x54c, 0x550])  # R, R+N, R, R.
        self.assertEqual(self.arc_value(16, 0x269bc) + self.arc_displacement(16, 0x26a84), initialized[0])
        self.assertEqual([self.arc_value(16, 0x269bc) + self.arc_displacement(16, p)
                          for p in (0x26a94, 0x26a98)], initialized[2:])  # START resets both to R.

        # Both selected calls pass the same channel-table D and 0x5bc bytes,
        # but in opposite directions. Their delay slots supply local base+60.
        table = self.arc_value(16, 0x26998) + self.arc_displacement(16, 0x269a0)
        for base, load in ((0x9fb4, 0x9fbc), (0x9d84, 0x9d98)):
            self.assertEqual(self.arc_value(4, base) + self.arc_displacement(4, load), table)
        for call, target in ((0x9fd0, 0x9e74), (0x9da4, 0x9c3c)):
            opcode = words[4][call]
            displacement = (opcode >> 7 & 0xfffff) - 0x100000
            self.assertEqual(call + 4 + displacement * 4, target)
        local = self.arc_value(4, 0x9fc8) + self.arc_displacement(4, 0x9fd4)
        self.assertEqual(local, self.arc_value(4, 0x9d60) + self.arc_displacement(4, 0x9da8))
        self.assertEqual((local, self.arc_value(4, 0x9fc0), self.arc_value(4, 0x9d9c)),
                         (0x3fffcdac, 0x5bc, 0x5bc))
        _, core, name = self.symbol(621)
        self.assertEqual((name, core[1:]), ("core_ls", (0x3fffcd70, 1528, 0x11, 0, 29)))
        self.assertEqual(local + 0x5bc, core[1] + core[2])
        working = self.arc_value(4, 0x8614)
        self.assertEqual([working + self.arc_displacement(4, p)
                          for p in (0x8654, 0x8634, 0x8620, 0x861c)],
                         [local + offset for offset in initialized])
        for address in (0x873c, 0x8744):
            self.assertEqual(working + self.arc_displacement(4, address), local + 0x54c)
        # Selected GetCabac stores target local +54c, not the saved DRAM D.
        # Successful visible same-channel copy and callee preservation remain
        # premises. Unchanged saved words do not rule out unsaved local work.

    def test_scope_and_named_assumptions_exclude_runtime_queue_or_plane_proofs(self):
        result = self.mapping()
        self.assertEqual(result["validation_scope"], {
            "conditional_reply_metadata": True, "raw_source_plane_contract": False,
            "full_PPB_contract_invoked": False, "operational_queue_validity": False, "runtime_observed": False,
            "freshness_proven": False, "operational_coherence_proven": False, "allocation_lifetime_proven": False,
            "source_plane_lease": False, "active_decode_context": False, "hardware_aliasing_proven": False,
            "standalone_execution": False, "public_route": False})
        self.assertEqual(result["assumptions"][:len(result["init_reply"]["assumptions"])], result["init_reply"]["assumptions"])
        assumptions = " ".join(result["assumptions"])
        for phrase in ("valid incoming command record", "neither selector nor argument continuity", "C/K/H", "r9=0",
                       "special-channel bypass", "channel 0..15", "empty channel slot", "valid normalized D",
                       "F=D+0x794", "D+0x530", "nonaliasing", "no-wrap", "callee-preservation", "delay slots",
                       "STATUS-PC", "vendor channel-shift", "saved reply buffer", "DMA/copy completion", "visibility",
                       "same-channel activation", "unchanged table/fields", "C+8", "INIT's C+12", "disjoint from H+0x58",
                       "stores before bounds checking", "ignores status", "34-record pool", "not proof of a current OPEN acknowledgment"):
            with self.subTest(premise=phrase):
                self.assertIn(phrase, assumptions)
        for phrase in ("raw-plane layout", "ownership", "lease", "operational completion", "unresolved relocation"):
            self.assertIn(phrase, assumptions)
        # The stored H marker, snapshot addresses and metadata declarations are
        # intentionally not advertised as observed queue/ownership/freshness.
        self.assertIs(result["arm_path"]["builder"]["marker_proves_accepted_object"], False)
        self.assertIs(result["record_pool"]["runtime_pool_identity_validated"], False)
        self.assertIs(result["activation_snapshot"]["runtime_active_context_validated"], False)

    @staticmethod
    def repin_fixture(stack, changed):
        # Deliberately bypass byte identity fuses, NOT semantic checks. This
        # demonstrates that an accepted hash alone cannot authorize a corrupt
        # section, owned relocation, or selected symbol interpretation.
        for name in ("_OPEN_REPLY_REGIONS", "_INIT_REPLY_REGIONS", "_FRESH_INIT_REGIONS"):
            regions = tuple((role, offset, size, hashlib.sha256(changed[offset:offset + size]).hexdigest())
                            for role, offset, size, _ in getattr(MAP, name))
            stack.enter_context(mock.patch.object(MAP, name, regions))
        regions = tuple((role, offset, bytes(changed[offset:offset + len(raw) // 2]).hex())
                        for role, offset, raw in MAP._COMMAND_BUFFER_BRIDGE_REGIONS)
        stack.enter_context(mock.patch.object(MAP, "_COMMAND_BUFFER_BRIDGE_REGIONS", regions))
        stack.enter_context(mock.patch.object(MAP, "_COMMAND_BUFFER_BRIDGE_RELA_SHA256",
                            hashlib.sha256(changed[0x72780:0x78d44]).hexdigest()))
        headers = tuple((slot, offset, bytes(changed[offset:offset + len(raw) // 2]).hex())
                        for slot, offset, raw in MAP._INNER_DESCRIPTOR_HEADERS)
        stack.enter_context(mock.patch.object(MAP, "_INNER_DESCRIPTOR_HEADERS", headers))
        sections = tuple((slot, index, offset, bytes(changed[offset:offset + len(raw) // 2]).hex(), name_offset, name)
                         for slot, index, offset, raw, name_offset, name in MAP._INNER_DESCRIPTOR_SECTIONS)
        stack.enter_context(mock.patch.object(MAP, "_INNER_DESCRIPTOR_SECTIONS", sections))
        windows = tuple((slot, role, section, address, offset, bytes(changed[offset:offset + len(raw) // 2]).hex())
                        for slot, role, section, address, offset, raw in MAP._INNER_DESCRIPTOR_WINDOWS)
        stack.enter_context(mock.patch.object(MAP, "_INNER_DESCRIPTOR_WINDOWS", windows))

    def test_re_pinned_semantically_incoherent_sections_symbols_and_relas_still_refuse(self):
        mutations = []
        for header, wrong_owner in ((0x79b08, 4), (0x79b58, 2)):
            mutations += [(header + 4, "<I", 9), (header + 24, "<I", 34),
                          (header + 28, "<I", wrong_owner), (header + 36, "<I", 16),
                          (header + 20, "<I", 13)]
        mutations += [(0x6cd10 + 12, "<B", 0x12), (0x6cd10 + 14, "<H", 17),
                      (0x6cd10 + 8, "<I", 129), (0x69e60 + 14, "<H", 4),
                      (0x72a2c + 4, "<I", 0x31a06), (0x72a2c + 4, "<I", 0x31904),
                      (0x72a2c + 8, "<i", -4), (0x72ab0 + 8, "<i", 4),
                      (0x729b4 + 8, "<i", 0x500), (0x733ec + 8, "<i", 0x1792),
                      (0x72a2c, "<I", 0x24948)]
        for offset, form, value in mutations:
            changed = bytearray(self.payload)
            struct.pack_into(form, changed, offset, value)
            with self.subTest(offset=hex(offset), value=value), ExitStack() as stack:
                self.repin_fixture(stack, changed)
                with self.assertRaises(MAP.FormatError):
                    self.mapping(changed)
        for position, record in ((0x72abc, (0x24944, 0x31a04, 0)),
                                  (0x72ab0, (0x24944, 0x31a04, 0)),
                                  (0x72abc, (0x258c0, 0x31a04, 0))):
            changed = bytearray(self.payload)
            struct.pack_into("<IIi", changed, position, *record)
            with self.subTest(duplicate_or_unowned=hex(position), source=hex(record[0])), ExitStack() as stack:
                self.repin_fixture(stack, changed)
                with self.assertRaises(MAP.FormatError):
                    self.mapping(changed)

    def test_complete_pin_site_byte_branch_delay_and_argument_mutations_refuse(self):
        # Every new pin byte is tested separately above; these named mutations
        # identify causal boundaries instead of calling a narrative a proof.
        mutations = (
            (0x5730, 0xebffec5b), (0xb18, 0xeb0025e0),
            (0xfb74, 0xeb005e40), (0xfb80, 0x1a000003),
            (0x27578, 0xebfffeb6), (0x275ac, 0xebffe1ff),
            (0x27584, 0xe5860048), (0x2758c, 0xe5860044),
            (0x27594, 0xe5860058), (0x275a0, 0xe286205c),
            (0x275b4, 0xe5c60230), (0x275b8, 0xe3a00000),
            (0xfbb0, 0xe58a5000), (0xfbcc, 0xe7810105),
            (0x27610, 0x73760001),
            (0x48450, 0x2ffdd900), (0x48454, 0x60279e00),
            (0x47358, 0x62679e00), (0x47704, 0x10091a14),
            (0x474d8, 0x78608), (0x47700, 0x78628),
        )
        with ExitStack() as stack:
            self.stop_interpretation(stack)
            for offset, replacement in mutations:
                changed = bytearray(self.payload)
                struct.pack_into("<I", changed, offset, replacement)
                with self.subTest(offset=hex(offset)), self.assertRaises(MAP.FormatError):
                    self.mapping(changed)

    def test_new_interpretation_never_reads_unpinned_opaque_targets_or_whole_symbol_tables(self):
        init = MAP._init_reply_metadata_linkage(self.payload, self.images)
        inner = MAP._inner_descriptor_map(self.payload, self.images)
        pins = self.union_pins()
        original = MAP.bounded
        requests = []

        def bounded_read(payload, offset, size, role):
            requests.append((offset, size, role))
            self.assertTrue(any(start <= offset and offset + size <= start + extent for start, extent in pins),
                            (hex(offset), size, role))
            return original(payload, offset, size, role)

        with mock.patch.object(MAP, "_init_reply_metadata_linkage", return_value=init), \
                mock.patch.object(MAP, "_inner_descriptor_map", return_value=inner), \
                mock.patch.object(MAP, "bounded", side_effect=bounded_read), \
                mock.patch.object(MAP, "_ppb_bank_contract", side_effect=AssertionError("unrelated full PPB closure")):
            self.mapping()
        self.assertTrue(requests)
        self.assertNotIn((0x69b70, 13392), {(offset, size) for offset, size, _ in requests})
        self.assertNotIn((0x67a95, 8410), {(offset, size) for offset, size, _ in requests})
        # Free/config/log targets remain opaque; source BL words are enough
        # to state conditional edges without reading these unpinned bodies.
        for opaque in (0xf6e4, 0xecfc, 0x128d8, 0x13100, 0x22d60, 0x232e8):
            self.assertNotIn(opaque, {offset for offset, size, _ in requests if size == 4})

    def test_private_helper_purity_no_public_cli_and_identity_scopes(self):
        before = bytes(self.payload), json.dumps(self.images, sort_keys=True)
        with mock.patch.object(MAP, "read_firmware", side_effect=AssertionError("external firmware read")), \
                mock.patch.object(MAP.os, "open", side_effect=AssertionError("external file or device open")), \
                mock.patch("subprocess.run", side_effect=AssertionError("external decoder")), \
                mock.patch.object(MAP, "_ppb_bank_contract", side_effect=AssertionError("full unrelated PPB closure")):
            result = self.mapping()
        self.assertEqual(before, (bytes(self.payload), json.dumps(self.images, sort_keys=True)))
        self.assertEqual(result, json.loads(json.dumps(result, sort_keys=True)))
        self.assertEqual(set(result), {"basis", "validation", "init_reply", "instruction_anchors", "elf_receipts",
                                       "relocation_receipts", "selected_arc_edges", "arm_path", "outer_reply",
                                       "record_pool", "rings", "activation_snapshot", "assumptions", "validation_scope"})
        self.assertIs(result["basis"]["conditional"], True)
        for flag in ("device_observed", "entire_payload_rehashed", "public_route"):
            self.assertIs(result["basis"][flag], False)
        for option in ("--open-reply-metadata-linkage", "--open-reply-ring-linkage"):
            with self.subTest(option=option), mock.patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                    mock.patch.object(sys, "stderr", new_callable=io.StringIO), self.assertRaises(SystemExit) as error:
                MAP.main([str(BLOB), option])
            self.assertEqual(error.exception.code, 2)
            self.assertEqual(output.getvalue(), "")

    def test_inherited_delayed_old_init_countermodel_is_not_an_open_acknowledgment(self):
        result = self.mapping()
        init = result["init_reply"]["fresh_init"]
        before = json.dumps(result, sort_keys=True)
        scenario = {"event_origin": "delayed_old", "freshness_assumed": False,
                    "completion_mailbox": 0x1234, "request_word1": 0,
                    "reply_command": 0xffffffff, "reply_status": 0xffffffff}
        projected = MAP._fresh_init_projection(init, scenario)
        self.assertEqual((projected["transport_status"], projected["host_return"]), (0, 0))
        self.assertIs(projected["current_transaction_acknowledged"], False)
        self.assertIs(projected["runtime_observed"], False)
        self.assertIs(projected["projected_current_path"], False)
        events = projected["events"]
        names = [event["event"] for event in events]
        self.assertLess(names.index("event_reset"), names.index("delayed_old_callback"))
        self.assertLess(names.index("delayed_old_callback"), names.index("event_wait"))
        reply = next(event for event in events if event["event"] == "reply_copy")
        self.assertEqual((reply["command"], reply["word1"]), (0x73760001, 0))
        for name in ("outer_call", "outer_trigger", "reply_publication", "response_irq"):
            self.assertNotIn(name, names)
        # This unchanged dependency model is INIT-specific, not an OPEN
        # execution projection, observed firmware bug, or freshness theorem.
        self.assertEqual(json.dumps(result, sort_keys=True), before)

    def test_independent_arm_frames_raw_reply_stores_and_publication_boundaries(self):
        expected = {
            0x62d8: 0xebfffbba, 0x5730: 0xebffec5a,
            0x27480: 0xe92d4ff0, 0x27484: 0xe24ddf81,
            0x274a0: 0xe28d4f42, 0x274a4: 0xe28d700c,
            0x274a8: 0xe3a020fc, 0x274b8: 0xe3a020fc,
            0x274c8: 0xe1a05004, 0x274cc: 0xe1a08007,
            0x27578: 0xebfffeb7, 0x2757c: 0xe58d0008,
            0x27580: 0xe5980008, 0x27584: 0xe5860044,
            0x27588: 0xe598000c, 0x2758c: 0xe5860048,
            0x27590: 0xe5980010, 0x27594: 0xe5860050,
            0x27598: 0xe5980014, 0x2759c: 0xe58d0004,
            0x275a0: 0xe2862058, 0x275a4: 0xe5990008,
            0x275a8: 0xe59d1004, 0x275ac: 0xebffe1fe,
            0x275b0: 0xe3a00001, 0x275b4: 0xe5c60220,
            0x275b8: 0xe59d0008, 0x275bc: 0xe28ddf81, 0x275c0: 0xe8bd8ff0,
            0xfb08: 0xe3590000, 0xfb0c: 0x0a000002,
            0xfb10: 0xe5d502b4, 0xfb14: 0xe3500000, 0xfb18: 0x1a00001d,
            0xfb74: 0xeb005e41, 0xfb7c: 0xe3580000,
            0xfb80: 0x0a000003, 0xfb84: 0xe1a00004, 0xfb88: 0xebfffed5,
            0xfb8c: 0xe1a00008, 0xfb90: 0xeaffff26,
            0xfbb0: 0xe58a4000, 0xfbcc: 0xe7810106,
            0xfbd8: 0xeb00549b, 0xfbe0: 0xebfffa59, 0xfbe4: 0xe3a00000,
        }
        for address, word in expected.items():
            with self.subTest(address=hex(address)):
                self.assertEqual(self.word(address), word)
        self.assertEqual((self.word(0x27480) & 65535).bit_count() * 4, 36)
        self.assertEqual(self.immediate(self.word(0x27484)), 516)
        self.assertEqual(self.immediate(self.word(0x274a0)), 264)
        self.assertEqual(self.immediate(self.word(0x274a4)), 12)
        self.assertEqual(self.word(0x27610), 0x73760002)
        # H is the ARM allocated channel, NOT ARC D/F, the ARM API host-channel
        # record K, or the separately named activated ARC-local snapshot.
        self.assertEqual([self.word(p) & 4095 for p in (0x27584, 0x2758c, 0x27594)], [0x44, 0x48, 0x50])
        self.assertEqual(self.immediate(self.word(0x275a0)), 0x58)
        self.assertEqual(self.word(0x275b4) & 4095, 0x220)
        self.assertLess(0x2757c, 0x27580)
        self.assertLess(0x275ac, 0x275b8)
        self.assertLess(0xfb88, 0xfbb0)

    def test_independent_signed_relas_sections_literals_and_source_owners(self):
        tables = {}
        for index, owner, count, offset in ((37, 2, 84, 0x6d020), (39, 4, 261, 0x6da34),
                                           (51, 16, 2171, 0x72780)):
            section = self.sections[index]
            self.assertEqual((section[1], section[6], section[7], section[9]), (4, 35, owner, 12))
            self.assertEqual((self.elf_base + section[4], section[5] // 12), (offset, count))
            tables[owner] = [(offset + i * 12, struct.unpack_from("<IIi", self.payload, offset + i * 12))
                             for i in range(count)]
        definitions = {}
        for index in (47, 70, 588, 601, 602, 611, 624, 637, 794, 803, 824):
            _, record, name = self.symbol(index)
            definitions[name] = record
        for name, owner, records in self.RELA_GROUPS:
            function = definitions[name]
            self.assertEqual(function[5], owner)
            actual_owned = [(position, raw) for position, raw in tables[owner]
                            if function[1] <= raw[0] < function[1] + function[2]]
            expected_owned = [(position, (address, (symbol << 8) | kind, addend))
                              for address, position, kind, symbol, addend in records]
            self.assertEqual(actual_owned, expected_owned)
            for address, position, kind, symbol, addend in records:
                with self.subTest(function=name, source=hex(address)):
                    self.assertIs(type(addend), int)
                    self.assertEqual(struct.unpack_from("<IIi", self.payload, position),
                                     (address, (symbol << 8) | kind, addend))
                    self.assertTrue(function[1] <= address <= function[1] + function[2] - 4)
        self.assertEqual(self.RELA_GROUPS[0][2][0][-1], 500)
        self.assertEqual(self.RELA_GROUPS[1][2][-1][-1], 1792)


class FirmwareOpenAllocationTests(unittest.TestCase):
    """Concrete stock A32 partitioning, with explicitly synthetic callee contracts."""

    START, END = 0x25c18, 0x26158
    DIGEST = "b413d045432e9927c877cbc76c507d9d2868cd41632c54f92ca782d35236d376"
    OUTPUTS = (8, 0x10, 0x18, 0x24, 0x2c, 0x34, 0x3c)
    K, H, SP, RETURN = 0x100000, 0x200000, 0x300100, 0xfffffff0
    POISON = 0xa5a5a5a5

    @classmethod
    def setUpClass(cls):
        cls.payload = MAP.read_firmware(BLOB)[:-MAP.TRAILER_SIZE]

    def execute(self, picture="common", split="common", generic="private",
                sizes=None, base=0x800000, fail=None, translation="write",
                aliases=None, payload=None, budget=512):
        # No device access, arbitrary code, or payload-derived reads.
        # The stock body is interpreted; only four external callees are stubbed.
        data = self.payload if payload is None else payload
        if hashlib.sha256(data[self.START:self.END]).hexdigest() != self.DIGEST:
            raise ValueError("stock allocation body changed")
        if type(budget) is not int or not 1 <= budget <= 512:
            raise ValueError("invalid allocation instruction budget")
        self.assertIn(translation, ("write", "error", "zero", "omit"))
        memory = {address: self.POISON for low, high in
                  ((self.K, self.K + 0x1b0), (self.H, self.H + 0x240),
                   (self.SP - 0x100, self.SP + 0x34))
                  for address in range(low, high, 4)}

        def read(address):
            if address not in memory:
                raise ValueError("unmapped synthetic read")
            return memory[address]

        writes = []
        def store(address, value, pc=None):
            if address not in memory:
                raise ValueError("unmapped synthetic write")
            memory[address] = value & 0xffffffff
            writes.append((pc, address, memory[address]))

        fields = {0x0c: 0x100, 0x14: 0x80, 0x1c: 3, 0x20: 0x40,
                  0x28: 0x200, 0x30: 0x300, 0x38: 0x60, 0x40: 0x70}
        fields.update(sizes or {})
        for offset, value in fields.items():
            store(self.H + offset, value)
        for field, offset, mode in ((0xd4, 0x1a8, picture), (0xd0, 0x1a4, split)):
            self.assertIn(mode, ("common", "private", "factory"))
            store(self.H + field, field if mode == "private" else 0)
            store(self.K + offset, offset if mode == "factory" else 0)
        self.assertIn(generic, ("private", "factory"))
        store(self.H + 0xcc, 0xcc if generic == "private" else 0)
        for offset in (8, 0x10, 0x14, 0x1a0):
            store(self.K + offset, offset + 0x1000)
        destinations = {offset: self.H + offset for offset in self.OUTPUTS}
        destinations.update(aliases or {})
        # Ordinary OPEN's FA8C..FAE8 caller ABI, not semantic buffer labels.
        stack = [fields[0x1c], fields[0x20], fields[0x28], fields[0x30],
                 fields[0x38], fields[0x40]] + [destinations[offset] for offset in
                                               (8, 0x10, 0x18, 0x24, 0x2c, 0x34, 0x3c)]
        for index, value in enumerate(stack):
            store(self.SP + index * 4, value)
        regs = [0xabc00000 + index for index in range(16)]
        regs[0:4] = [self.K, self.H, fields[0x0c], fields[0x14]]
        regs[13:16] = [self.SP, self.RETURN, self.START]
        preserved = regs[4:12]
        calls, allocations, zero = [], {}, False
        writes.clear()
        for steps in range(budget):
            pc = regs[15]
            if pc == self.RETURN:
                self.assertEqual((regs[4:12], regs[13]), (preserved, self.SP))
                return {"status": regs[0], "outputs": {offset: read(self.H + offset)
                        for offset in self.OUTPUTS}, "calls": calls, "writes": writes,
                        "steps": steps}
            if not (self.START <= pc < 0x26040 or 0x26134 <= pc < self.END) or pc & 3:
                raise ValueError("outside stock executable slices")
            word, = struct.unpack_from("<I", data, pc)
            condition = word >> 28
            self.assertIn(condition, (0, 1, 14))
            regs[15] = pc + 4
            if (condition == 0 and not zero) or (condition == 1 and zero):
                continue
            reg = lambda index: pc + 8 if index == 15 else regs[index]
            if word & 0x0e000000 == 0x0a000000:
                displacement = word & 0xffffff
                if displacement & 0x800000:
                    displacement -= 1 << 24
                target = pc + 8 + displacement * 4
                if not word & (1 << 24):
                    regs[15] = target
                    continue
                self.assertIn(target, (0x1f5d4, 0x2b628, 0x1fe6c, 0x203c4))
                args = tuple(regs[:4])
                calls.append((pc, target, args))
                result = 0
                if target in (0x1f5d4, 0x2b628):
                    slot = len(allocations)
                    pointer = 0x900000 + slot * 0x1000
                    allocations[pointer] = (base + slot * 0x10000) & 0xffffffff
                    result = 0 if slot == fail else pointer
                elif target == 0x1fe6c:
                    self.assertIn(args[1], allocations)
                    if translation != "omit":
                        store(args[2], 0 if translation == "zero" else allocations[args[1]], pc)
                    result = 4 if translation in ("error", "omit") else 0
                # The caller cannot rely on caller-saved registers or flags.
                regs[:4] = [result, 0xd1d1d1d1, 0xd2d2d2d2, 0xd3d3d3d3]
                regs[12], regs[14], zero = 0xdcdcdcdc, pc + 4, False
            elif word in (0xe1cd05d0, 0xe1cd20f0):
                if word == 0xe1cd05d0:
                    regs[0:2] = [read(regs[13] + 80), read(regs[13] + 84)]
                else:
                    store(regs[13], regs[2], pc)
                    store(regs[13] + 4, regs[3], pc)
            elif word & 0x0ff00000 == 0x03000000:
                regs[(word >> 12) & 15] = ((word >> 4) & 0xf000) | (word & 0xfff)
            elif word & 0x0fc000f0 == 0x00000090:
                self.assertEqual(word, 0xe0000190)
                regs[0] = regs[0] * regs[1] & 0xffffffff
            elif word in (0xe92d4fff, 0xe8bd8ff0):
                selected = [index for index in range(16) if word & (1 << index)]
                base_sp = regs[13]
                address = base_sp - 4 * len(selected) if word == 0xe92d4fff else base_sp
                for index in selected:
                    if word == 0xe92d4fff:
                        store(address, reg(index), pc)
                    else:
                        regs[index] = read(address)
                    address += 4
                regs[13] = base_sp + (4 if word == 0xe8bd8ff0 else -4) * len(selected)
            elif word & 0x0c000000 == 0x04000000:
                self.assertFalse(word & ((1 << 25) | (1 << 22) | (1 << 21)))
                self.assertTrue(word & (1 << 24))
                displacement = word & 0xfff
                address = (reg((word >> 16) & 15) +
                           (displacement if word & (1 << 23) else -displacement)) & 0xffffffff
                destination = (word >> 12) & 15
                if word & (1 << 20):
                    regs[destination] = read(address)
                else:
                    store(address, reg(destination), pc)
            else:
                self.assertEqual(word & 0x0c000000, 0)
                opcode, destination = (word >> 21) & 15, (word >> 12) & 15
                self.assertIn(opcode, (2, 4, 10, 13))
                left = reg((word >> 16) & 15)
                if word & (1 << 25):
                    rotation, immediate = ((word >> 8) & 15) * 2, word & 255
                    right = ((immediate >> rotation) | (immediate << ((32 - rotation) % 32))) & 0xffffffff
                else:
                    self.assertEqual(word & 0xff0, 0)
                    right = reg(word & 15)
                value = ({2: left - right, 4: left + right, 10: left - right,
                          13: right}[opcode]) & 0xffffffff
                if word & (1 << 20):
                    self.assertEqual(opcode, 10)
                    zero = value == 0
                if opcode != 10:
                    regs[destination] = value
        raise ValueError("stock allocation instruction budget exceeded")

    def test_exact_partition_and_refuted_final_segment_ring_candidate(self):
        for picture in ("common", "private", "factory"):
            for split in ("common", "private", "factory"):
                for generic in ("private", "factory"):
                    with self.subTest(picture=picture, split=split, generic=generic):
                        result = self.execute(picture, split, generic)
                        out = result["outputs"]
                        slot = int(picture != "common") + int(split != "common")
                        cursor = 0x800000 + slot * 0x10000
                        p = 0xc0 if picture == "common" else 0
                        x = 0x80 if split == "common" else 0
                        r = 0x300 if split == "common" else 0
                        self.assertEqual(result["status"], 0)
                        self.assertEqual(out[8], cursor + p + x + r)
                        self.assertEqual(out[0x24], out[8] + 0x100 + 0x70 + 0x60)
                        expected_ring = cursor + p + x if split == "common" else (
                            0x800000 + int(picture != "common") * 0x10000 + 0x80)
                        self.assertEqual(out[0x2c], expected_ring)
                        self.assertNotEqual(out[0x2c], out[0x24])
                        if split == "common":
                            self.assertEqual(out[0x2c], out[8] - 0x300)
                        allocations = [call for call in result["calls"] if call[1] in (0x1f5d4, 0x2b628)]
                        self.assertEqual(allocations[-1][2][1:3],
                                         (0x100 + 0x200 + 0x70 + 0x60 + p + x + r, 12))

    def test_complete_caller_builder_pins_and_distinct_output_publication(self):
        for start, end, digest in (
                (0xf7e4, 0xfbec, "723911a2a94585093da0b4caebd2ba2a20b2c8bc87c4e91dd42646ae95a3a092"),
                (0x27480, 0x275c4, "c356d9c46eee35ea6dafb60e7e1ef49cf37c77726e4ba14e5e192f9cc2c50f91")):
            self.assertEqual(hashlib.sha256(self.payload[start:end]).hexdigest(), digest)
        word = lambda offset: struct.unpack_from("<I", self.payload, offset)[0]
        # Every instruction of the ordinary caller setup, including paired
        # output destinations and STMIB's +4 start, is pinned independently.
        self.assertEqual([word(offset) for offset in range(0xfa8c, 0xfaec, 4)], [
            0xe284303c, 0xe2842034, 0xe284102c, 0xe2840024,
            0xe1cd02f4, 0xe1cd22fc, 0xe2843018, 0xe2842010,
            0xe2841008, 0xe5940040, 0xe1cd01f4, 0xe1cd21fc,
            0xe5940020, 0xe5941028, 0xe5942030, 0xe5943038,
            0xe98d000f, 0xe594301c, 0xe58d3000, 0xe594200c,
            0xe5943014, 0xe1a01004, 0xe1a00005, 0xeb00584a])
        self.assertEqual([word(offset) for offset in (0xfb1c, 0xfb20, 0xfb24, 0xfb28)],
                         [0xe1c402d4, 0xe1c422dc, 0xe1cd02fc, 0xe1cd23f4])
        # Builder pushes nine registers and reserves 0x204 bytes: +0x25c
        # therefore means caller +0x34 (H2C), not +0x2c (H24).
        self.assertEqual((word(0x27480), word(0x27484)), (0xe92d4ff0, 0xe24ddf81))
        self.assertEqual([word(offset) for offset in (0x27520, 0x27524, 0x27530, 0x27534)],
                         [0xe59d0254, 0xe585002c, 0xe59d025c, 0xe5850038])
        self.assertEqual((0x254 - (9 * 4 + 0x204), 0x25c - (9 * 4 + 0x204)),
                         (0x2c, 0x34))

    def test_zero_spans_leave_only_uninitialized_optional_outputs_stale(self):
        spans = (0x0c, 0x14, 0x28, 0x30, 0x38, 0x40)
        normal = dict(zip(spans, (0x100, 0x80, 0x200, 0x300, 0x60, 0x70)))
        for mask in range(64):
            sizes = {offset: value if mask & (1 << index) else 0
                     for index, (offset, value) in enumerate(normal.items())}
            result = self.execute(sizes=sizes)
            out, cursor = result["outputs"], 0x8000c0
            for size_offset, output in ((0x14, 0x10), (0x30, 0x2c), (0x0c, 8),
                                        (0x40, 0x3c), (0x38, 0x34), (0x28, 0x24)):
                expected = cursor if sizes[size_offset] else (self.POISON if output in (0x34, 0x3c) else 0)
                self.assertEqual(out[output], expected, (mask, output))
                cursor += sizes[size_offset]
            self.assertEqual(result["status"], 0)

    def test_failed_allocations_return_four_before_partitioning(self):
        for mode in ("private", "factory"):
            for fail in range(3):
                result = self.execute(mode, mode, mode, fail=fail)
                self.assertEqual(result["status"], 4)
                allocations = [c for c in result["calls"] if c[1] in (0x1f5d4, 0x2b628)]
                self.assertEqual(len(allocations), fail + 1)
                self.assertEqual(result["outputs"][8], 0)
                self.assertEqual(result["outputs"][0x24], 0)
                self.assertEqual(result["outputs"][0x34], self.POISON)

    def test_translation_status_is_ignored_and_not_a_success_certificate(self):
        valid = self.execute("private", "private")
        error = self.execute("private", "private", translation="error")
        self.assertEqual(error["outputs"], valid["outputs"])
        self.assertEqual(error["status"], 0)
        zero = self.execute("private", "private", translation="zero")
        self.assertEqual((zero["status"], zero["outputs"][8]), (0, 0x140))
        omitted = self.execute(translation="omit")
        self.assertEqual((omitted["status"], omitted["outputs"][0x18]), (0, self.POISON))
        self.assertNotEqual(omitted["outputs"][8], valid["outputs"][8])

    def test_aliases_zero_driver_and_wrap_refute_unqualified_ring_equation(self):
        alias = self.execute(aliases={0x2c: self.H + 8})
        self.assertEqual(alias["outputs"][8], 0x800140)
        self.assertEqual(alias["outputs"][0x2c], self.POISON)
        no_driver = self.execute(sizes={0x0c: 0})["outputs"]
        self.assertEqual(no_driver[8], 0)
        self.assertNotEqual(no_driver[0x2c], no_driver[8] - 0x300)
        wrapped = self.execute(base=0xffffff00)["outputs"]
        self.assertEqual((wrapped[0x2c], wrapped[8]), (0x40, 0x340))
        self.assertEqual(wrapped[0x2c], (wrapped[8] - 0x300) & 0xffffffff)
        self.assertLess(wrapped[0x2c], 0xffffff00)
        multiplied = self.execute(sizes={0x1c: 0x10000000, 0x20: 0x10})["outputs"]
        self.assertEqual((multiplied[0x18], multiplied[0x2c]), (0, 0x800080))

    def test_changed_body_unmapped_memory_and_budget_refuse(self):
        for offset in range(self.START, self.END):
            changed = bytearray(self.payload)
            changed[offset] ^= 1
            with self.assertRaisesRegex(ValueError, "body changed"):
                self.execute(payload=changed)
        with self.assertRaisesRegex(ValueError, "unmapped synthetic write"):
            self.execute(aliases={8: 0x400000})
        with self.assertRaisesRegex(ValueError, "budget exceeded"):
            self.execute(budget=1)
        for budget in (0, 513, True):
            with self.assertRaisesRegex(ValueError, "invalid allocation instruction budget"):
                self.execute(budget=budget)


class BlockAverageGateTests(unittest.TestCase):
    """Pinned stock gate path, not ARC execution or block-average capability."""

    @classmethod
    def setUpClass(cls):
        cls.payload = MAP.read_firmware(BLOB)[:-MAP.TRAILER_SIZE]
        cls.sections = {}
        for base in (0x2ea60, 0x79dd8):
            header = struct.unpack_from("<16sHHIIIIIHHHHHH", cls.payload, base)
            cls.sections[base] = [struct.unpack_from("<10I", cls.payload,
                                                    base + header[6] + index * 40)
                                  for index in range(header[12])]

    def word(self, base, section, address):
        record = self.sections[base][section]
        self.assertTrue(record[3] <= address <= record[3] + record[5] - 4)
        return struct.unpack_from("<I", self.payload,
                                  base + record[4] + address - record[3])[0]

    def test_complete_selected_bodies_and_original_symbol_ownership(self):
        # Independent ELF32/symbol mapping, including the duplicate local
        # FinalILSetup name and unaligned inner .text file position.
        bodies = (
            (0x2ea60, 4, "VideoParameters", 0x80dc, 316, 0x32f80,
             "e4525894db6bffca8c049ff0607d87d17f98cfea965b3f2af785de31b7b82a74"),
            (0x2ea60, 4, "Core_PopulatePPB", 0x9344, 516, 0x341e8,
             "2b3cde5b8b4eef00eedd2c7fa4829324f428f7cb89d01eee685db5a5f6de6805"),
            (0x2ea60, 5, "FinalILSetup", 0xfbac, 504, 0x3aa50,
             "2a8e46e1910f2bf84ca6020492e83b33de9064e7b5e5cbae07c7808d39a5e265"),
            (0x79dd8, 4, "H264_DecodePictureInner", 0x2e3c, 1100, 0x7b078,
             "f28130ced34ec1735975e82d96cf954ad20935e83e4f35c40e53146f1f1444af"),
            (0x79dd8, 47, "H264P_DecodePicture", 0x43834, 600, 0xbb035,
             "c8d4cdc1d0130a68a3c1c6d97618d8a1166b9ce08fbd457656b5c6e111b82e51"),
        )
        symbols = {}
        for base, sections in self.sections.items():
            symbols[base] = set()
            for table in sections:
                if table[1] != 2:
                    continue
                strings = sections[table[6]]
                names = self.payload[base + strings[4]:base + strings[4] + strings[5]]
                for position in range(0, table[5], 16):
                    name, value, size, info, _, section = struct.unpack_from(
                        "<IIIBBH", self.payload, base + table[4] + position)
                    if info & 15 == 2:
                        text = names[name:names.index(b"\0", name)].decode("ascii")
                        symbols[base].add((text, value, size, section))
        for base, section, name, address, size, offset, digest in bodies:
            with self.subTest(name=name):
                record = self.sections[base][section]
                self.assertEqual(base + record[4] + address - record[3], offset)
                self.assertLessEqual(address + size, record[3] + record[5])
                self.assertIn((name, address, size, section), symbols[base])
                self.assertEqual(hashlib.sha256(self.payload[offset:offset + size]).hexdigest(), digest)

    def test_saved_offset_is_hex82_not_decimal82(self):
        contract = MAP._ppb_bank_contract(self.payload)
        geometry = contract["context_initialization"]["init_geometry"]
        load = self.word(0x2ea60, 16, 0x24ac8)
        store = self.word(0x2ea60, 16, 0x24ad4)
        self.assertEqual((load, store), (0x08090034, 0x10408082))
        self.assertEqual(geometry["metadata_extra_open_request_byte_offset"], load & 511)
        self.assertEqual(geometry["metadata_extra_saved_context_byte_offset"], store & 511)
        self.assertEqual(store & 511, 130)
        activation = contract["context_initialization"]["activation"]
        flag_address = activation["snapshot_destination"] + (store & 511)
        self.assertEqual(flag_address, 0x3fffce2e)
        self.assertEqual(self.word(0x2ea60, 4, 0x8194), flag_address)

    def test_ordinary_arm_open_builder_omits_average_flag_word(self):
        body = self.payload[0x27480:0x275c4]
        self.assertEqual(hashlib.sha256(body).hexdigest(),
                         "c356d9c46eee35ea6dafb60e7e1ef49cf37c77726e4ba14e5e192f9cc2c50f91")
        words = dict(zip(range(0x27480, 0x275c4, 4), struct.unpack("<81I", body)))
        self.assertEqual((words[0x274a8], words[0x274ac], words[0x274b0]),
                         (0xe3a020fc, 0xe3a01000, 0xe1a00004))  # size252, zero, request pointer.
        self.assertEqual((words[0x274cc], words[0x2756c], words[0x27578]),
                         (0xe1a08007, 0xe1a02004, 0xebfffeb7))
        stores = [word & 4095 for address, word in words.items()
                  if 0x274d4 <= address < 0x27578 and word & 0xffff0000 == 0xe5850000]
        self.assertEqual(stores, [0, 4, 12, 8, 16, 20, 24, 28, 32, 36,
                                  40, 44, 48, 56, 60, 64, 68, 72, 76])
        self.assertNotIn(52, stores)
        # Only this selected builder: its zero-fill callee and opaque
        # transport must preserve the unwritten word. Not whole-firmware absence.

    def test_zero_flag_clears_average_offset_before_header_publication(self):
        # Conditional original legacy-ARC operands; successful same-context
        # activation/DMA and preservation by opaque callees are prerequisites.
        expected = ((4, 0x8190, 0x081f0400), (4, 0x8198, 0x67e00100),
                    (4, 0x819c, 0x20000a01), (4, 0x81f0, 0x50000000),
                    (4, 0x81f4, 0x1001814c), (4, 0x9514, 0x08070150),
                    (4, 0x951c, 0x0807014c), (4, 0x9524, 0x10068104),
                    (5, 0xfd28, 0x083f0000), (5, 0xfd2c, 0x3fffd074),
                    (5, 0xfd34, 0x10000224), (5, 0xfd8c, 0x2feabda0),
                    (5, 0xfd90, 0x605ffe30))
        for section, address, word in expected:
            self.assertEqual(self.word(0x2ea60, section, address), word)
        branch = self.word(0x2ea60, 4, 0x819c)
        self.assertEqual(0x819c + 4 + ((branch >> 7) & 0xfffff) * 4, 0x81f0)
        self.assertEqual(0x3fffd370 - 180, 0x3fffd2bc)
        self.assertEqual(0x3fffd170 - 252, 0x3fffd074)
        self.assertEqual(self.payload[0x6f570:0x6f57c].hex(),
                         "8cfd00000685020000000000")  # Original type6/add0, not applied relocation.
        sections = self.sections[0x2ea60]
        relocation, symbols = sections[40], sections[35]
        self.assertEqual((relocation[1], relocation[6], relocation[7], relocation[9]),
                         (4, 35, 5, 12))
        self.assertTrue(0x2ea60 + relocation[4] <= 0x6f570 <
                        0x2ea60 + relocation[4] + relocation[5])
        name, address, size, info, _, section = struct.unpack_from(
            "<IIIBBH", self.payload, 0x2ea60 + symbols[4] + 645 * 16)
        self.assertEqual((address, size, info & 15, section), (0x537c, 76, 2, 2))
        strings = sections[symbols[6]]
        names = self.payload[0x2ea60 + strings[4]:0x2ea60 + strings[4] + strings[5]]
        self.assertEqual(names[name:names.index(b"\0", name)], b"Dma_Write")

    def test_both_inner_setup_paths_require_nonzero_descriptor_word36(self):
        for section, gate, branch, skip, literal, base_write, image_write, ctl_write in (
                (4, 0x2ef4, 0x2efc, 0x2fa8, 0x2f04, 0x2f68, 0x2f80, 0x2f88),
                (47, 0x438f0, 0x438f8, 0x43988, 0x43900, 0x43964, 0x4397c, 0x43984)):
            with self.subTest(section=section):
                load = self.word(0x79dd8, section, gate)
                self.assertEqual(load & 511, 36)
                opcode = self.word(0x79dd8, section, branch)
                self.assertEqual(opcode & 31, 1)  # Zero branch, no delay slot.
                self.assertEqual(opcode & 96, 0)
                self.assertEqual(branch + 4 + ((opcode >> 7) & 0xfffff) * 4, skip)
                self.assertEqual(self.word(0x79dd8, section, literal), 0x30061000)
                self.assertEqual(self.word(0x79dd8, section, base_write), 0x14010000)
                self.assertEqual(self.word(0x79dd8, section, image_write), 0x1401000c)
                self.assertEqual(self.word(0x79dd8, section, ctl_write - 4), 0x601ffe44)
                self.assertEqual(self.word(0x79dd8, section, ctl_write), 0x14010008)
        # Firmware read/write operands do not establish host access safety,
        # read-clear behavior, a source-surface lease or actual completion.


if __name__ == "__main__":
    unittest.main()
