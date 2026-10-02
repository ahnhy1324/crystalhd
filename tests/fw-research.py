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
            "cd056fbfcfb795f8ffaa169d313bfb5c5535e67f16affd924dedd209f80fb939",
            "87ded59ecc94e3b4fa3a3cd9da6f2d978bcd5839bd196fd6cc72f717feaaa453",
            "f470be31c70241e5053aa2a9c4c2388ba8bfb920cc9feb657689b14f8e3a3f38",
            "804cca53d5c7358d37d06707eee797426a11d6c59d2a7f241198477ce2716d86",
            "0e99c9c380bd73676fb1288b4e6095213b25b2d3555478e394a851f1e9922462",
            "bcbc14bcfe15a2b97e355e9614fa21b79ded210e2cff948d64431fa5d1e160dc",
            "05dd432d0f3202229831a1e9d8832e49061da9b2a583bbb2e9bf676d975f25d9",
            "309046019c1e09222dfb96cf767a8736b2599e60b0269cab6f5870a48137c24f",
            "22e2f752cf00154034af7ad929b69d45ba240936b7b1a8f1d484beb895cda937",
            "b4fa8538c99ad02ee6a81ca517076b45009bf3848e766ea4762a5aa361100803",
            "2c7f1db68e27d91bb52ca3d4c03b39e164a19ade3a1842f6642da7aab213e149",
            "865d6503265abdc69c7b4e385766f8cb1d4f17d73be0f9b494c4487b1b090324")
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
            "cd056fbfcfb795f8ffaa169d313bfb5c5535e67f16affd924dedd209f80fb939",
            "87ded59ecc94e3b4fa3a3cd9da6f2d978bcd5839bd196fd6cc72f717feaaa453",
            "f470be31c70241e5053aa2a9c4c2388ba8bfb920cc9feb657689b14f8e3a3f38",
            "804cca53d5c7358d37d06707eee797426a11d6c59d2a7f241198477ce2716d86",
            "0e99c9c380bd73676fb1288b4e6095213b25b2d3555478e394a851f1e9922462",
            "bcbc14bcfe15a2b97e355e9614fa21b79ded210e2cff948d64431fa5d1e160dc",
            "05dd432d0f3202229831a1e9d8832e49061da9b2a583bbb2e9bf676d975f25d9",
            "309046019c1e09222dfb96cf767a8736b2599e60b0269cab6f5870a48137c24f",
            "22e2f752cf00154034af7ad929b69d45ba240936b7b1a8f1d484beb895cda937",
            "b4fa8538c99ad02ee6a81ca517076b45009bf3848e766ea4762a5aa361100803",
            "2c7f1db68e27d91bb52ca3d4c03b39e164a19ade3a1842f6642da7aab213e149",
            "865d6503265abdc69c7b4e385766f8cb1d4f17d73be0f9b494c4487b1b090324",
            "3553b947d6948d11fc48b2994ca29599caa8a70ff7b79d7ffc2639901c9aedfe",
            "6da05d4dca3424ef76e9359ed7ab3228d5c2622dcd1573b62bc88d2b0c3f2e7b",
            "6946e167d1dfbb01632025d014ebd76284aafcf58f79099881552c6fc80a4964",
            "839f141d887e74b8e5d9da871b2160ba15ab5ce5ad6acc77a87a7def68ef4ce6",
            "8083817bbe282d0727a2aeae18e162e3c8634d2805891a3641f8516fa610c16a",
            "10732aabc68b8e6a019b714212e726dc82400b139755429283c80192f2a9a3ab",
            "14c6de19078879dad7c645441cad2a4cc76e1f584946f8d6a1477edbc42a7902",
            "75bce6cbfd367c8af1cc2837cff10ce356f95064a45469a1a207f89ade347a45",
            "ffb4ed56d28f7f2262ec19d578c08f3ad6187f100986a763667aa0e0c276d864",
            "4303bf603a1070e397d8d4c6e48d874c78cd2e82c43aca09942d57e9f82c930a",
            "edfb10be5e6af600a62a8d4f679a5099e7439d987f4ddc215d22007cdb959d65",
            "5f873b9d8f5ae9e12d5a95c79fb7768a4d15454aabe58cd2fd2fe88371a26374",
            "dbbf1ef286423a1c3c7e0288fc7f0147fd21467ae91e2fc9078db2a752efa58c",
            "81114e5b7d4fb2227e3091d72eedb056bf34467ae1f7d2a81bcde1230b29be3e",
            "e8fec88375cf243161e3f282df0a2757d91ea7de59519b6d5fc96d099b5a0c20",
            "05cad2965807a274801889efe62b7a0ce903a22f10ac616e165abfb265039884")
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
        self.assertEqual(aggregate.hexdigest(), "252ec7e66b0b6a5c1c76c807640b2a2ba1542db1aad83fc03f0922796f661565")
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
        self.assertEqual(aggregate.hexdigest(), "37e06168817763cd85bf8702c313e3599d6821ed841d626e9bca109655208606")

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
    def record(selector=0, mode=0, form=2, field=0, horizontal=0, vertical=0,
               y=0x1000, c=0x8000, yn=40, cn=20):
        result = bytearray(116)
        result[8], result[0x27], result[0x28], result[0x5c] = mode, form, field, selector
        for offset, value in ((0x34, y), (0x38, c), (0x54, yn), (0x58, cn),
                              (0x6c, horizontal), (0x70, vertical)):
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
        for example in report["addressing"]["model_examples"]:
            record = self.record(selector=example["selector"], horizontal=3, vertical=5)
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

    def test_instruction_model_modes_formats_odd_coordinates_and_wrap(self):
        pairs = ((0, 0), (1, 1), (3, 5), (0x04000002, 0xfffffffd),
                 (0xffffffff, 0xffffffff), (0x80000001, 0x10003))
        total = 0
        for selector in range(3):
            for mode in (0, 1, 2, 255):
                for form in (1, 2, 3):
                    for field in (0, 1, 2):
                        for horizontal, vertical in pairs:
                            record = self.record(selector, mode, form, field, horizontal, vertical,
                                                 0xfffffffc, 0xfffffff8, 0xffffffff, 0x80000001)
                            with self.subTest(selector=selector, mode=mode, form=form, field=field,
                                              horizontal=horizontal, vertical=vertical):
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
            stdout = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
            aggregate.update(bytes([mask]))
            aggregate.update(hashlib.sha256(stdout).digest())
        # Recomputed from the unmodified b42ff87 mapper, not from this model.
        self.assertEqual(aggregate.hexdigest(),
                         "e40601809c6f2c5b1ddb1e76b185c0b46b2486b86e36d333ee200148f7cc90b1")

    def test_new_map_is_default_off_and_public_pin_precedes_parsing(self):
        with mock.patch.object(MAP, "_mfd_source_map", side_effect=AssertionError("unexpected source map")):
            self.assertNotIn("picture_output", MAP.analyze(MAP.read_firmware(BLOB)))
        altered = bytearray(MAP.read_firmware(BLOB))
        altered[0x1918] ^= 1
        with mock.patch.object(MAP, "parse_elf", side_effect=AssertionError("unexpected parse")), \
                mock.patch.object(MAP, "_mfd_source_map", side_effect=AssertionError("unexpected source map")):
            with self.assertRaisesRegex(MAP.FormatError, "exact bundled"):
                MAP.analyze(altered, expected_sha256=hashlib.sha256(altered).hexdigest(), picture_output=True)


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
                stdout = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
                aggregate.update(bytes([mask]))
                aggregate.update(hashlib.sha256(stdout).digest())
        # Computed from git9354041 before the private helper was added.
        self.assertEqual(aggregate.hexdigest(),
                         "bf116a4627c71956042f25a98f63fde401b58140569c22cdc5ebf8298bbc85dd")


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
                stdout = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
                aggregate.update(bytes([mask]))
                aggregate.update(hashlib.sha256(stdout).digest())
        # Independent public-output baseline predates these private helpers;
        # the same snapshot also survives clean parent091387c unchanged.
        self.assertEqual(aggregate.hexdigest(),
                         "bf116a4627c71956042f25a98f63fde401b58140569c22cdc5ebf8298bbc85dd")


if __name__ == "__main__":
    unittest.main()
