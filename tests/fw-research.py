#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Hardware-free firmware mapping, malformed-input and file-ownership tests."""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
