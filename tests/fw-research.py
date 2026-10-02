#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Hardware-free firmware mapping, malformed-input and file-ownership tests."""

import hashlib
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


class FirmwarePictureOutputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = MAP.read_firmware(BLOB)
        cls.payload = cls.data[:-MAP.TRAILER_SIZE]
        cls.images = MAP.analyze(cls.data)["images"]

    def mapping(self, payload=None, images=None):
        return MAP._picture_output_map(self.payload if payload is None else payload,
                                       self.images if images is None else images)

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
            "04b13afcddc28b5b26849309839932fc2ae7a1c2f48ef351222d61b1c4fee279",
            "a299b545da28d928da63228d61835891b05750b85a7e10425ddda224f51d43d0",
            "9818336b301d58ee708b243fa0e4371dc797b4794fa4388f869bd7ca92fa51bf",
            "4d997f643b9c40bec1464e2d1f739bbc5459cc1af21d04dc0ee863121d372643",
            "71f4f6553e74bacbcc5827212061478b2870228324b44d2985849f4bbc264720",
            "edcd2e5e20756b35177df5d9c92dc913d72b1a1ee4d0f93a2d4f1d029be4c5da",
            "b81221b60ec232afb8c0bf809cca91a9b73a014cd740d53c0b5619fead782cac",
            "3cc03d72d623d18563ec5bb6ebf0588b8db595828440a578e888265d2bec5fe2",
            "7d9b9fdd4e9383c5cacbd9c8949330c44683a659efb2457869bc4bfe2dff446f",
            "104420747e6da2a06da0c545abb39a7dbbcc3b330170582f1794cbd8d24889f2",
            "b699f03e6e2ba7a6e34ee9c834abffebad17d97939e0ac58221403dbf90cbdbf",
            "fa8d9d60ab3f2b9d661e7279b3e97fbb2a2ffd5dd0f5267804c130ca4e8f9e22")
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
            "04b13afcddc28b5b26849309839932fc2ae7a1c2f48ef351222d61b1c4fee279",
            "a299b545da28d928da63228d61835891b05750b85a7e10425ddda224f51d43d0",
            "9818336b301d58ee708b243fa0e4371dc797b4794fa4388f869bd7ca92fa51bf",
            "4d997f643b9c40bec1464e2d1f739bbc5459cc1af21d04dc0ee863121d372643",
            "71f4f6553e74bacbcc5827212061478b2870228324b44d2985849f4bbc264720",
            "edcd2e5e20756b35177df5d9c92dc913d72b1a1ee4d0f93a2d4f1d029be4c5da",
            "b81221b60ec232afb8c0bf809cca91a9b73a014cd740d53c0b5619fead782cac",
            "3cc03d72d623d18563ec5bb6ebf0588b8db595828440a578e888265d2bec5fe2",
            "7d9b9fdd4e9383c5cacbd9c8949330c44683a659efb2457869bc4bfe2dff446f",
            "104420747e6da2a06da0c545abb39a7dbbcc3b330170582f1794cbd8d24889f2",
            "b699f03e6e2ba7a6e34ee9c834abffebad17d97939e0ac58221403dbf90cbdbf",
            "fa8d9d60ab3f2b9d661e7279b3e97fbb2a2ffd5dd0f5267804c130ca4e8f9e22",
            "3553b947d6948d11fc48b2994ca29599caa8a70ff7b79d7ffc2639901c9aedfe",
            "6da05d4dca3424ef76e9359ed7ab3228d5c2622dcd1573b62bc88d2b0c3f2e7b",
            "6946e167d1dfbb01632025d014ebd76284aafcf58f79099881552c6fc80a4964",
            "839f141d887e74b8e5d9da871b2160ba15ab5ce5ad6acc77a87a7def68ef4ce6",
            "3052c6a0be9280ce7dc64cd63cc1cb0f5a6952a772597a5140fc9e7fe4a569c8",
            "69083d2540bf15ff74a718cad540e15abeaf3fae894176114d2cea9cd42c963d",
            "be7edbd1a31c609eaf33c94577d98f2e599a2a92cde71ba36db2085487300ae9",
            "ed0d60bcf68c0187dc3082a908d74411ce161a637aa8f14921588eaca5571f0f",
            "ed8d637d4c50f41888a5eab84b7f2b80223753a911dfdae27b382dbec1e9539d",
            "d50be9b85465c563a80aa3dddefe84bd163df964795bb3960110aecf70c11eaa",
            "ecf785961c0f202cc685df6b23b32dcfaf2ac3f8d21ec933dc26d661be2561dd",
            "7974a762e99d8fe42d7eaee953ffbd0b5fc528adda0ba2731b82efe310958ae7",
            "0d0ca12ef03882fb20d272f453f19dc448e0013a86662278196518adf574b15c",
            "87af0e7b0031d0fb32ddbbdda1d2d2699c3f74bd441b97e13d61e6189ad5b07a",
            "c5e1be475f6073a208c6bc2a5a0c76989ab9791111561d4cf54f46927b2af6b3",
            "47ef9dcca7270991a5e623a5e535f86309956faed0f62825189f64641324ed8f")
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

    def test_bridge_all_64_old_outputs_are_byte_identical_and_new_report_deterministic(self):
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
        self.assertEqual(aggregate.hexdigest(), "1e6b80ad8fa76d0a37a959a955c61e323cb8c4f4606fc553985736027cd2edd8")
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
        return MAP._inner_descriptor_map(self.payload if payload is None else payload,
                                         self.images if images is None else images)

    def word(self, slot, address):
        # Independent ELF VM/file conversions for the selected sections.
        layouts = ((0, 0x7f8c, 0xc158, 0x32e30), (0, 0x23d74, 0x3b3c8, 0x46908),
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

    def test_inner_descriptor_private_identity_mapping_and_budget_limits(self):
        result = self.mapping()
        self.assertEqual(len(result["validated_regions"]), 40)
        self.assertEqual(sum(r["size"] for r in result["validated_regions"]), 2067)
        self.assertEqual(len(result["instruction_windows"]), 24)
        with mock.patch.object(MAP, "MAX_INNER_DESCRIPTOR_REGIONS", 40), \
                mock.patch.object(MAP, "MAX_INNER_DESCRIPTOR_BYTES", 2067):
            self.assertEqual(self.mapping(), result)
        for name, limit in (("MAX_INNER_DESCRIPTOR_REGIONS", 39), ("MAX_INNER_DESCRIPTOR_BYTES", 2066)):
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

    def test_inner_descriptor_preserves_128_old_outputs_and_composes_exactly(self):
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
        # Golden includes the verified RX metadata source-location update.
        self.assertEqual(aggregate.hexdigest(), "588588f1c823e22c94f917da0f9be143e97bb399a3e1d02f507706d1d9803060")

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


if __name__ == "__main__":
    unittest.main()
