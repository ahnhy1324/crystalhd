#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline, hash-pinned BCM70015 firmware, ELF32 symbols and retained references."""

import argparse
from bisect import bisect_right
import hashlib
import json
import os
import re
import stat
import struct
import sys


BUNDLED_SHA256 = "8bf3a68f5c64686358a52274e40911a88c7f8c67ecbf6cf1557a49b4d7bc67c9"
BUNDLED_SIZE = 0xd3014
MAX_FIRMWARE_SIZE = 4 * 1024 * 1024  # include/crystalhd_ioctl_limits.h
TRAILER_SIZE = 20  # driver/linux/FleaDefs.h: length slot plus 16-byte CMAC
MAX_SYMBOL_RECORDS = 65536  # Aggregate across all tables/images, including duplicates.
MAX_STRING_TABLE_BYTES = MAX_FIRMWARE_SIZE
MAX_RELOCATION_RECORDS = 65536  # Includes no-ops and repeated tables/images.
MAX_OWNER_LOOKUP_STEPS = 1000000  # Bounds overlapping/aliased function intervals.
MAX_METADATA_OUTPUT_BYTES = 16 * 1024 * 1024  # Retained symbols and section names.
MAX_REFERENCE_OUTPUT_BYTES = 32 * 1024 * 1024  # Conservative JSON size accounting.
MAX_BOOTSTRAP_ANCHORS = 256  # Fixed, audited ARM instructions; never a general scan.
MAX_PICTURE_OUTPUT_ANCHORS = 192  # Separate fixed picture-path inventory, not a scan.
MAX_ARC_COMMENT_BYTES = 4096
MAX_ARC_COMMENT_RECORDS = 128
MAX_ARC_METADATA_STRING_BYTES = 128
MAX_ARC_EXTENSION_BYTES = 112
MAX_ARC_EXTENSION_RECORDS = 10
MAX_ARC_METADATA_BYTES = 2 * (MAX_ARC_COMMENT_BYTES + MAX_ARC_EXTENSION_BYTES)
DEFAULT_SYMBOLS = (
    "Arc_UartInit", "Arc_UartPoll", "ArcGetc", "ArcPutc", "ArcCommandBuffer",
    "ReadLine", "MatchKeyword", "Core_Command", "CmdPeek", "CmdCore",
    "CmdState", "CmdTrace", "CmdCabac",
)


class FormatError(ValueError):
    pass


def read_firmware(path):
    # O_PATH does not invoke a device's open handler. Pin and check the inode
    # before reopening it for reading, so a pathname race cannot open hardware.
    reference = os.open(path, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        metadata = os.fstat(reference)
        if not stat.S_ISREG(metadata.st_mode):
            raise FormatError("firmware must be a regular file, not a device or symlink")
        if not 24 <= metadata.st_size <= MAX_FIRMWARE_SIZE:
            raise FormatError("firmware size is outside the BCM70015 download bounds")
        descriptor = os.open(f"/proc/self/fd/{reference}", os.O_RDONLY | os.O_CLOEXEC)
        try:
            source = os.fdopen(descriptor, "rb")
        except BaseException:
            os.close(descriptor)
            raise
        with source:
            data = source.read(MAX_FIRMWARE_SIZE + 1)
        if len(data) != metadata.st_size:
            raise FormatError("firmware changed size while reading")
        return data
    finally:
        os.close(reference)


def bounded(data, offset, size, description):
    if offset < 0 or size < 0 or offset > len(data) - size:
        raise FormatError(f"{description} extends outside the firmware payload")
    return data[offset:offset + size]


def _arc_comment(data, base, expected_count):
    """Validate stored compiler hints, not the meaning of vendor core options."""
    if not data or len(data) > MAX_ARC_COMMENT_BYTES or data[-1] != 0:
        raise FormatError("invalid ARC comment size or termination")
    if data.count(b"\0") > MAX_ARC_COMMENT_RECORDS:
        raise FormatError("ARC comment record budget exceeded")
    records = data[:-1].split(b"\0")
    if any(not record or len(record) > MAX_ARC_METADATA_STRING_BYTES or
           any(byte < 32 or byte > 126 for byte in record.replace(b"\n", b""))
           for record in records):
        raise FormatError("invalid ARC comment record")
    linker = b"MetaWare Linker v8.4.6\n"
    compiler = b"hc8.4.18 -a4 -core8 -O -Xbs "
    if not records[0].startswith(linker):
        raise FormatError("ARC linker hint does not match the baseline")
    records[0] = records[0][len(linker):]
    if len(records) != expected_count * 2:
        raise FormatError("ARC compiler record count does not match the baseline")
    for index in range(0, len(records), 2):
        if (not records[index].startswith(compiler) or
                not re.fullmatch(rb"[A-Za-z_][A-Za-z_0-9]*\.c", records[index][len(compiler):]) or
                records[index + 1] != b"11202009.075353"):
            raise FormatError("ARC compiler hint record does not match the baseline")
    return {"linker_hint": linker[:-1].decode("ascii"),
            "compiler_hint": compiler.rstrip().decode("ascii"),
            "compiler_record_count": expected_count, "nul_record_count": len(records),
            "first_compiler_record_blob_file_offset": base + len(linker)}


def _arc_extensions(data, base):
    """Strict declarations only; GNU 2.23.2 arc-ext.c:159-172 defines the layout."""
    if not data or len(data) > MAX_ARC_EXTENSION_BYTES:
        raise FormatError("ARC extension byte budget exceeded")
    records, identities, names = [], set(), set()
    offset = 0
    while offset < len(data):
        if len(records) >= MAX_ARC_EXTENSION_RECORDS:
            raise FormatError("ARC extension record budget exceeded")
        if len(data) - offset < 2:
            raise FormatError("truncated ARC extension record header")
        length, kind = data[offset:offset + 2]
        if kind not in (0, 2):
            raise FormatError("unsupported ARC extension record type")
        prefix = 5 if kind == 0 else 6
        if length < prefix + 2 or length > len(data) - offset:
            raise FormatError("invalid ARC extension record length")
        record = data[offset:offset + length]
        name = record[prefix:]
        if (name[-1] != 0 or b"\0" in name[:-1] or
                len(name) - 1 > MAX_ARC_METADATA_STRING_BYTES or
                any(byte < 32 or byte > 126 for byte in name[:-1])):
            raise FormatError("invalid ARC extension name")
        name = name[:-1].decode("ascii")
        item = {"record_blob_file_offset": base + offset, "length": length,
                "type": kind, "name": name}
        if kind == 0:
            opcode, minor, flags = record[2:5]
            if not 0x10 <= opcode <= 0x1f or minor or flags:
                raise FormatError("ARC extension instruction fields do not match the baseline")
            item.update(opcode=opcode, minor_opcode=minor, flags=flags)
            identity = (kind, opcode, minor)
        else:
            address = int.from_bytes(record[2:6], "big")
            item["auxiliary_address"] = address
            identity = (kind, address)
        if identity in identities or name in names:
            raise FormatError("duplicate ARC extension declaration")
        identities.add(identity)
        names.add(name)
        records.append(item)
        offset += length
    expected = [(2, 0x21, "t0_count"), (2, 0x22, "t0_control"), (2, 0x23, "t0_limit")]
    expected += [(0, opcode, name) for opcode, name in
                 ((0x10, "asl"), (0x11, "lsr"), (0x12, "asr"), (0x13, "ror"),
                  (0x16, "mul16"), (0x1e, "max"), (0x1f, "min"))]
    if [(r["type"], r.get("opcode", r.get("auxiliary_address")), r["name"])
            for r in records] != expected:
        raise FormatError("ARC extension declarations do not match the baseline")
    return records


def _arc_metadata_map(payload, images, image_sections):
    """Private pure validator; public entry requires the exact bundled SHA/size."""
    extents = [(0x2ea60, 0x79dd8), (0x79dd8, 0xcfbb0)]
    if (len(payload) != BUNDLED_SIZE - TRAILER_SIZE or len(image_sections) != 2 or
            [(i["blob_file_offset"], i["blob_file_end"]) for i in images] != extents):
        raise FormatError("ARC metadata image identities do not match the baseline")
    expected = ((32, 0x6711c, 2313, 0x79a40, 0x67a25, 0x79a68, 43),
                (63, 0xc1ced, 1289, 0xcf408, 0xc21f6, 0xcf430, 24))
    result, extensions, budget = [], [], MAX_ARC_METADATA_BYTES
    for image, sections, values in zip(images, image_sections, expected):
        if (image["class"], image["endianness"], image["machine"],
                image["elf_type"], image["flags"]) != (32, "little", 45, 2, 0):
            raise FormatError("ARC ELF header does not match the baseline")
        if len(sections) != 2 or {s["name"] for s in sections} != {".comment", ".arcextmap"}:
            raise FormatError("missing, duplicate or unexpected ARC metadata section")
        index, comment_base, comment_size, comment_header, ext_base, ext_header, count = values
        contents, sources = {}, {}
        for name, position, size, section_index, header in (
                (".comment", comment_base, comment_size, index, comment_header),
                (".arcextmap", ext_base, 112, index + 1, ext_header)):
            section = next(s for s in sections if s["name"] == name)
            if (section["size"] > budget or section["size"] >
                    (MAX_ARC_COMMENT_BYTES if name == ".comment" else MAX_ARC_EXTENSION_BYTES)):
                raise FormatError("ARC metadata byte budget exceeded")
            budget -= section["size"]
            if (section["blob_file_offset"] is None or section["size"] < 0 or
                    not image["blob_file_offset"] <= section["blob_file_offset"] <=
                    image["blob_file_end"] - section["size"]):
                raise FormatError("ARC metadata section crosses its ELF image bounds")
            if (section["section_index"], section["type"], section["flags"],
                    section["elf_virtual_address"], section["blob_file_offset"], section["size"],
                    section["link"], section["info"], section["align"], section["entry_size"],
                    section["section_header_blob_file_offset"]) != (
                        section_index, 1, 0, 0, position, size, 0, 0, 1, 1, header):
                raise FormatError("ARC metadata section does not match the baseline")
            contents[name] = bounded(payload, position, size, "ARC metadata section")
            sources[name] = {"blob_file_offset": position, "size": size,
                             "section_header_blob_file_offset": header}
        declarations = _arc_extensions(contents[".arcextmap"], ext_base)
        extensions.append(contents[".arcextmap"])
        result.append({"image_blob_file_offset": image["blob_file_offset"],
                       "elf_flags": image["flags"], "gnu_2_23_2_flag_machine": "ARC5",
                       "arc_attributes_present": False, "sections": sources,
                       "comment": _arc_comment(contents[".comment"], comment_base, count),
                       "extension_declarations": declarations})
    if extensions[0] != extensions[1]:
        raise FormatError("ARC extension maps differ between the baseline images")
    return {"gnu_binutils_version": "2.23.2", "architecture_selection": "unresolved",
            "extension_record_layout_source": "opcodes/arc-ext.c:159-172; gas/config/tc-arc.c:609-623,824-837",
            "elf_flags_source": "include/elf/arc.h:42-49; bfd/elf32-arc.c:187-205",
            "auxiliary_address_byte_order": "big", "extension_maps_identical": True,
            "images": result, "limitations": [
                "MetaWare -core8 and GNU ELF flag interpretations use different namespaces; their relationship is unresolved.",
                "Extension records declare ASCII names and fields, not instruction semantics or a complete ISA.",
                "Stored metadata does not establish silicon architecture, a call graph or codec capabilities."]}


def _bootstrap_word(payload, offset):
    if offset % 4:
        raise FormatError("bootstrap word is not aligned")
    return struct.unpack("<I", bounded(payload, offset, 4, "bootstrap word"))[0]


def _a32_branch(payload, offset, link=False, condition=14):
    """Decode only audited A32 B/BL, not Thumb, BLX or arbitrary conditions."""
    word = _bootstrap_word(payload, offset)
    if condition not in (0, 1, 11, 12, 14) or word >> 24 != (condition << 4) | 10 | int(link):
        raise FormatError("bootstrap instruction is not the required A32 branch")
    displacement = word & 0xffffff
    if displacement & 0x800000:
        displacement -= 1 << 24
    target = offset + 8 + displacement * 4
    _bootstrap_word(payload, target)
    return {"blob_file_offset": offset, "word": word,
            "operation": "BL" if link else "B", "condition": condition,
            "target_blob_file_offset": target}


def _a32_literal(payload, offset):
    """Decode AL LDR word [PC, +/-imm12], with no writeback or register offset."""
    word = _bootstrap_word(payload, offset)
    if word & 0xff7f0000 != 0xe51f0000:
        raise FormatError("bootstrap instruction is not an AL A32 LDR literal")
    displacement = word & 0xfff
    literal = offset + 8 + (displacement if word & (1 << 23) else -displacement)
    value = _bootstrap_word(payload, literal)
    return {"blob_file_offset": offset, "word": word, "operation": "LDR literal",
            "destination_register": (word >> 12) & 15,
            "literal_blob_file_offset": literal, "literal_value": value}


def _bootstrap_map(payload, images):
    """Private pure validator; public callers must pin SHA/size before entering."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("bootstrap payload size does not match the bundled baseline")
    extents = [(0x2ea60, 0x79dd8), (0x79dd8, 0xcfbb0)]
    if [(image["blob_file_offset"], image["blob_file_end"]) for image in images] != extents:
        raise FormatError("bootstrap catalog does not match the parsed ELF identities")
    anchors = []

    def word(offset, expected):
        actual = _bootstrap_word(payload, offset)
        if actual != expected:
            raise FormatError(f"bootstrap word at {offset:#x} does not match the baseline")
        anchors.append({"blob_file_offset": offset, "word": actual,
                        "operation": "validated word"})
        return actual

    def branch(offset, target, link=False, condition=14):
        record = _a32_branch(payload, offset, link, condition)
        if record["target_blob_file_offset"] != target:
            raise FormatError("bootstrap branch target does not match the baseline")
        anchors.append(record)

    def literal(offset, position, value, register):
        record = _a32_literal(payload, offset)
        if (record["literal_blob_file_offset"], record["literal_value"],
                record["destination_register"]) != (position, value, register):
            raise FormatError("bootstrap literal does not match the baseline")
        anchors.append(record)

    # Flat ARM vectors + coherent reset/main branches corroborate ARMCR4 in
    # crystalhd_fleafuncs.c:1307-1309. Do not decode either embedded ARC image.
    for offset, position, target in ((0, 0x20, 0x2ca00), (4, 0x24, 0x3c),
                                     (8, 0x28, 0x5c), (12, 0x2c, 0x7c),
                                     (16, 0x30, 0x9c), (24, 0x34, 0xdc),
                                     (28, 0x38, 0x104)):
        literal(offset, position, target, 15)
        _bootstrap_word(payload, target)
    word(0x2ca00, 0xee100f31)
    branch(0x2cc0c, 0x74bc, link=True)
    word(0x74bc, 0xe92d4010)
    branch(0x7530, 0x8d98)
    word(0x8d98, 0xe92d4ff8)
    # Host ARM1 mailbox read, queued receive and actual dispatcher call.
    word(0x8d48, 0xe92d4010)
    literal(0x8d54, 0x8f74, 0x100e0000, 0)
    word(0x8d58, 0xe590401c)
    word(0x8d70, 0xe1a00004)
    branch(0x8d74, 0x8cf4, link=True)
    word(0x8cf4, 0xe92d4038)
    word(0x8d04, 0xe3001100)
    branch(0x8d14, 0x8c30, link=True)
    word(0x8c30, 0xe92d47f0)
    branch(0x8e40, 0x9048, link=True)
    word(0x9048, 0xe92d4070)
    branch(0x9204, 0x5f2c, link=True)
    # Compare-tree arithmetic: 0x73763108 - 7 - 0xfd = GET_VERSION.
    literal(0x5f78, 0x6174, 0x73763108, 2)
    word(0x5f8c, 0xe2420007)
    word(0x5fa0, 0xe24020fd)
    word(0x5fa4, 0xe1510002)
    branch(0x5fac, 0x6264, condition=0)
    branch(0x6270, 0x5ba4, link=True)
    word(0x5f5c, 0xe5950000)
    word(0x5f60, 0xe5860000)
    for offset, expected in ((0x5c10, 0xe3a00000), (0x5c14, 0xe5840008),
                             (0x5c18, 0xe5950004), (0x5c1c, 0xe5840004)):
        word(offset, expected)
    # Function entry/common-return words, not claimed complete function maps.
    functions = []
    for name, entry, prologue, exit_offset, epilogue in (
            ("host_command_dispatch", 0x5f2c, 0xe92d4070, 0x60c4, 0xe8bd8070),
            ("get_version", 0x5ba4, 0xe92d4070, 0x5c24, 0xe8bd8070),
            ("image_container_open", 0x26e7c, 0xe92d41f0, 0x26ebc, 0xe8bd81f0),
            ("image_container_read", 0x26f74, 0xe92d41f0, 0x26fb8, 0xe8bd81f0),
            ("image_container_close", 0x26fdc, 0xe92d4010, 0x26ff4, 0xe8bd8010)):
        word(entry, prologue)
        word(exit_offset, epilogue)
        functions.append({"name": name, "entry_blob_file_offset": entry,
                          "common_exit_blob_file_offset": exit_offset})
    # OPEN selection in the same dispatcher: command - GET_VERSION == 0xfc.
    # Record buffers are 256 bytes; this is a host selector policy, not codec
    # capability or proof that an admitted selector can decode a bitstream.
    for offset, expected in ((0x5f44, 0xe2845014), (0x5f48, 0xe2846f45),
                             (0x5f4c, 0xe3002100), (0x5f50, 0xe3a01000),
                             (0x5f54, 0xe1a00006), (0x5f70, 0xe5951000),
                             (0x5f7c, 0xe1510002), (0x5f80, 0xe0410002),
                             (0x5f90, 0xe1510000), (0x5f94, 0xe0412000),
                             (0x5fa8, 0xe0410002), (0x5fe8, 0xe3500001),
                             (0x5ff0, 0xe3500002), (0x5ff8, 0xe35000fc),
                             (0x62d4, 0xe1a00004)):
        word(offset, expected)
    for offset, target, condition in ((0x5f84, 0x662c, 0), (0x5f88, 0x602c, 12),
                                      (0x5f98, 0x6428, 0), (0x5f9c, 0x6004, 12),
                                      (0x5fb0, 0x5fe8, 12), (0x5fec, 0x628c, 0),
                                      (0x5ff4, 0x62ac, 0), (0x5ffc, 0x60b8, 1)):
        branch(offset, target, condition=condition)
    branch(0x5f58, 0x206e4, link=True)
    branch(0x6000, 0x62cc)
    branch(0x62d8, 0x51c8, link=True)
    # Initial-state and free-slot checks precede the low-byte selector ladder.
    for offset, expected in ((0x51c8, 0xe92d4ff0), (0x51cc, 0xe24dd01c),
                             (0x51d0, 0xe3e07000), (0x51d4, 0xe3a05000),
                             (0x51d8, 0xe3500000), (0x51e0, 0xe2806014),
                             (0x51e4, 0xe2804f45), (0x51ec, 0xe3e08000),
                             (0x51f0, 0xe59b0000), (0x51f4, 0xe3500001),
                             (0x523c, 0xe59b0004), (0x5240, 0xe3a01073),
                             (0x5244, 0xe0010195), (0x5248, 0xe0801101),
                             (0x524c, 0xe5d110c4), (0x5250, 0xe3510000),
                             (0x5258, 0xe2855001), (0x525c, 0xe3550004),
                             (0x5264, 0xe3570000), (0x5300, 0xe1a07005),
                             (0x5218, 0xe28dd01c), (0x521c, 0xe8bd8ff0)):
        word(offset, expected)
    literal(0x51e8, 0x5128, 0xd1ff4, 11)
    branch(0x51dc, 0x5220, condition=0)
    branch(0x51f8, 0x5234, condition=0)
    branch(0x5254, 0x52f4, condition=0)
    branch(0x5260, 0x5240, condition=11)
    branch(0x5268, 0x59f0, condition=11)
    branch(0x5304, 0x5264)
    # LDRB deliberately establishes no validation of algorithm word high bits.
    word(0x5294, 0xe5d60024)
    word(0x52c4, 0xe3a01008)
    routes = []
    for selector, compare, jump, target in ((1, 0x52a0, 0x52a4, 0x5408),
                                            (0, 0x52a8, 0x52ac, 0x5420),
                                            (4, 0x52b0, 0x52b4, 0x5438),
                                            (7, 0x52b8, 0x52bc, 0x5454),
                                            (6, 0x52c0, 0x52c8, 0x547c),
                                            (8, 0x52cc, 0x52d0, 0x55b0)):
        word(compare, 0xe3500000 | selector)
        branch(jump, target, condition=0)
        routes.append({"low_byte_selector": selector, "compare_blob_file_offset": compare,
                       "branch_blob_file_offset": jump, "target_blob_file_offset": target})
    for offset, expected in ((0x52d4, 0xe28f00b8), (0x52dc, 0xe584800c),
                             (0x52e0, 0xe5960004), (0x52e4, 0xe5840004),
                             (0x52e8, 0xe5848008), (0x52ec, 0xe3a00002)):
        word(offset, expected)
    branch(0x52d8, 0x203c4, link=True)
    branch(0x52f0, 0x5218)
    open_policy = {
        "command": 0x73763100, "dispatcher_call_blob_file_offset": 0x62d8,
        "entry_blob_file_offset": 0x51c8, "record_buffer_bytes": 256,
        "request_record_offset": 0x14, "reply_record_offset": 0x114,
        "algorithm_word_index": 9, "algorithm_load_blob_file_offset": 0x5294,
        "algorithm_bits_compared": 8, "algorithm_upper_bits_checked": False,
        "comparison_routes": routes,
        "named_rejected_selectors": [
            {"name": name, "value": value, "source": f"include/7411d.h:{line}"}
            for name, value, line in (("H261", 2, 390), ("H263", 3, 391), ("MPEG1", 5, 393))],
        "fallback": {"entry_blob_file_offset": 0x52d4, "channel_id_word_index": 3,
                     "channel_id": 0xffffffff, "status_word_index": 2, "status": 0xffffffff,
                     "sequence_copy_blob_file_offsets": [0x52e0, 0x52e4],
                     "internal_return": 2, "common_exit_blob_file_offset": 0x5218},
        "preconditions": {"state_word_equals": 1, "state_check_blob_file_offset": 0x51f4,
                          "free_channel_slot_required": True, "slot_scan_limit": 4,
                          "slot_check_blob_file_offset": 0x5250, "device_observed": False},
        "device_observed": False,
        "scope": "Static host OPEN selector policy, not channel-open or codec capability proof."}
    # DeviceStart passes host-interface +4 as an out-pointer to API init. Init
    # writes the same static context that the START getter returns: matching
    # slot offsets alone would not establish the OPEN -> START cache handoff.
    for offset, expected in ((0x54c, 0xe92d40f0), (0x558, 0xe1a07001),
                             (0x880, 0xe5874000), (0x89c, 0xe12fff1e),
                             (0x526c, 0xe3a00073), (0x5278, 0xe0050097),
                             (0x5298, 0xe3a0a001), (0x529c, 0xe3a09000)):
        word(offset, expected)
    literal(0x5ce8, 0x5128, 0xd1ff4, 7)
    literal(0x5d44, 0x5ed4, 0xd1ff8, 1)
    branch(0x5d4c, 0x54c, link=True)
    literal(0x56c, 0x6fc, 0xd3a00, 4)
    literal(0x898, 0x6fc, 0xd3a00, 0)
    # Each admitted OPEN branch writes a byte into context + slot*0x1cc +0xd0.
    for offset, expected in ((0x5408, 0xe59b0004), (0x540c, 0xe0800105),
                             (0x5410, 0xe5c0a0d0), (0x5420, 0xe59b0004),
                             (0x5424, 0xe0800105), (0x5428, 0xe5c090d0),
                             (0x5438, 0xe59b1004), (0x543c, 0xe3a00004),
                             (0x5440, 0xe0811105), (0x5444, 0xe5c100d0),
                             (0x5454, 0xe59b1004), (0x5458, 0xe3a00007),
                             (0x545c, 0xe0811105), (0x5460, 0xe5c100d0),
                             (0x547c, 0xe59b0004), (0x5480, 0xe0800105),
                             (0x5484, 0xe5c010d0), (0x55b0, 0xe59b0004),
                             (0x55b4, 0xe0800105), (0x55b8, 0xe5c010d0)):
        word(offset, expected)
    for offset in (0x541c, 0x5434, 0x5450, 0x5478, 0x549c):
        branch(offset, 0x55d4)
    # START command - dispatcher base == 0x12. The handler indexes the supplied
    # channel before its opened-byte check; this does not prove range safety.
    for offset, expected in ((0x602c, 0xe350001c), (0x6038, 0xe3500015),
                             (0x604c, 0xe3500012), (0x6684, 0xe1a00004),
                             (0x4630, 0xe92d47ff), (0x463c, 0xe2805014),
                             (0x4640, 0xe2804f45), (0x4644, 0xe5957008),
                             (0x4660, 0xe3a00073), (0x4668, 0xe0060097),
                             (0x466c, 0xe59a0004), (0x4670, 0xe0802106),
                             (0x4674, 0xe5d200c4), (0x4678, 0xe3500001),
                             (0x4830, 0xe1a00007)):
        word(offset, expected)
    branch(0x6034, 0x607c, condition=12)
    branch(0x6040, 0x6060, condition=12)
    branch(0x6050, 0x667c, condition=0)
    branch(0x6688, 0x4630, link=True)
    literal(0x465c, 0x3de8, 0xd1ff4, 10)
    branch(0x467c, 0x4808, condition=0)
    branch(0x4834, 0xdf0, link=True)
    # API START obtains that context and replaces the first configuration byte
    # from the cached OPEN byte. The two configuration copies are 56 bytes.
    for offset, expected in ((0xdf8, 0xe1a01000), (0xe08, 0xe3a02073),
                             (0xe0c, 0xe0010291), (0xe10, 0xe0807101),
                             (0xe14, 0xe2874018), (0xff0, 0xe5940008),
                             (0xff4, 0xe28d1004), (0x1054, 0xe5d720d0),
                             (0x1058, 0xe5cd2004), (0x10ac, 0xe5940008),
                             (0x10b0, 0xe28d1004), (0xee9c, 0xe1a04001),
                             (0xeecc, 0xe3a02038), (0xeed4, 0xe1a00004),
                             (0xf0cc, 0xe92d4ffe), (0xf0d0, 0xe1a06000),
                             (0xf0d4, 0xe1a0b001), (0xf104, 0xe1a04006),
                             (0xf13c, 0xe3a02038), (0xf140, 0xe1a0100b),
                             (0xf144, 0xe2840068), (0xf1d4, 0xe5d40068),
                             (0xf1d8, 0xe3500007), (0xf1e0, 0xe3a00004),
                             (0xf1e4, 0xe5c40068), (0xf390, 0xe5d42068),
                             (0xf394, 0xe1a03005), (0xf398, 0xe1a00007),
                             (0xf39c, 0xe5961000)):
        word(offset, expected)
    branch(0xdfc, 0x898, link=True)
    branch(0xff8, 0xee94, link=True)
    literal(0xeed0, 0xf6c8, 0x2dd88, 1)
    bounded(payload, 0x2dd88, 56, "START default configuration")
    branch(0xeed8, 0x20708, link=True)
    branch(0x10b4, 0xf0cc, link=True)
    branch(0xf148, 0x20708, link=True)
    branch(0xf1dc, 0xf1e8, condition=1)
    branch(0xf3a0, 0x276b0, link=True)
    branch(0xf3a4, 0x206a0, link=True)
    # The packet helper preserves r2 as word1 and returns the transport result.
    # Its caller immediately makes another BL without checking that result.
    for offset, expected in ((0x276b0, 0xe92d4ff0), (0x276c0, 0xe1a09002),
                             (0x276d4, 0xe28d4f42), (0x276fc, 0xe1a05004),
                             (0x27708, 0xe5850000), (0x2770c, 0xe5859004),
                             (0x27730, 0xe1a02004), (0x27740, 0xe58d0008),
                             (0x27744, 0xe59d0008), (0x27748, 0xe28ddf81),
                             (0x2774c, 0xe8bd8ff0), (0x27068, 0xe1a06002),
                             (0x27080, 0xe5d4008c), (0x27084, 0xe3500000),
                             (0x270ac, 0xe3a020fc), (0x270b0, 0xe1a01006),
                             (0x270b4, 0xe5940094), (0x270bc, 0xe5941118),
                             (0x270c0, 0xe59421cc), (0x270c4, 0xe1a00004),
                             (0x25024, 0xe5903004), (0x25028, 0xe5933000),
                             (0x2502c, 0xe7832001), (0x270d8, 0xe1a0100b),
                             (0x270dc, 0xe5940088)):
        word(offset, expected)
    literal(0x27704, 0x27aac, 0x73760005, 0)
    branch(0x2773c, 0x2705c, link=True)
    branch(0x27088, 0x27098, condition=0)
    branch(0x270b8, 0x20708, link=True)
    branch(0x270c8, 0x25024, link=True)
    branch(0x270e0, 0x20598, link=True)
    decoder_start = {
        "command": 0x7376311a, "command_source": "include/7411d.h:173",
        "dispatcher_call_blob_file_offset": 0x6688, "entry_blob_file_offset": 0x4630,
        "request_channel_word_index": 2, "request_channel_load_blob_file_offset": 0x4644,
        "context_link": {"host_interface_global_address": 0xd1ff4,
                         "init_output_pointer_address": 0xd1ff8,
                         "context_address": 0xd3a00, "slot_stride_bytes": 0x1cc,
                         "cached_algorithm_byte_offset": 0xd0,
                         "instruction_blob_file_offsets": [0x5ce8, 0x5d44, 0x5d4c,
                                                           0x558, 0x56c, 0x880, 0x898]},
        "preconditions": {"opened_byte_equals": 1, "opened_byte_offset": 0xc4,
                          "check_blob_file_offset": 0x4678,
                          "channel_range_validation_established": False,
                          "device_observed": False},
        "selector_mapping": [
            {"host_open_low_byte": selector, "cached_algorithm_byte": cached,
             "inner_start_algorithm_byte": inner, "cache_store_blob_file_offset": store}
            for selector, cached, inner, store in ((0, 0, 0, 0x5428), (1, 1, 1, 0x5410),
                                                   (4, 4, 4, 0x5444), (6, 8, 8, 0x5484),
                                                   (7, 7, 4, 0x5460), (8, 8, 8, 0x55b8))],
        "configuration": {"bytes": 56, "cache_load_blob_file_offset": 0x1054,
                          "first_byte_store_blob_file_offset": 0x1058,
                          "copy_call_blob_file_offsets": [0xeed8, 0xf148],
                          "channel_configuration_offset": 0x68,
                          "normalize_compare_blob_file_offset": 0xf1d8,
                          "normalize_store_blob_file_offset": 0xf1e4},
        "inner_packet": {"command": 0x73760005, "command_word_index": 0,
                         "algorithm_word_index": 1, "entry_blob_file_offset": 0x276b0,
                         "algorithm_store_blob_file_offset": 0x2770c,
                         "transport_call_blob_file_offset": 0x2773c,
                         "copy_bytes": 252, "copy_call_blob_file_offset": 0x270b8,
                         "register_write_call_blob_file_offset": 0x270c8,
                         "wait_call_blob_file_offset": 0x270e0,
                         "publication_is_path_dependent": True,
                         "return_checked_by_caller": False,
                         "unchecked_continuation_blob_file_offset": 0xf3a4},
        "device_observed": False,
        "scope": "Static cached-selector path, not inner decoder acceptance or codec capability proof.",
        "limitations": ["The specific packet helper return is unchecked on this continuation; earlier START state/configuration failures can still be returned.",
                        "Numeric inner algorithm bytes are not RAVE parser protocol enums or decoded ARC calls.",
                        "AVS and MVC reachability through the host API is not established."]}
    # ARM initialized catalog data is outside both ELF files, inside payload.
    root = _bootstrap_word(payload, 0xcfc00)
    if root != 0xcfbe8:
        raise FormatError("bootstrap catalog root does not match the baseline")
    callbacks = [_bootstrap_word(payload, 0xcfcf0 + index * 4) for index in range(3)]
    if callbacks != [0x26e7c, 0x26f74, 0x26fdc]:
        raise FormatError("bootstrap catalog callbacks do not match the baseline")
    descriptors = []
    slots = [_bootstrap_word(payload, root + index * 4) for index in range(6)]
    if slots != [0xcfbd0, 0xcfbdc, 0, 0, 0, 0]:
        raise FormatError("bootstrap catalog slots do not match the baseline")
    for index, (start, end) in enumerate(extents):
        descriptor = slots[index]
        size_pointer, blob_pointer, metadata_pointer = (
            _bootstrap_word(payload, descriptor + offset) for offset in (0, 4, 8))
        size = _bootstrap_word(payload, size_pointer)
        metadata = _bootstrap_word(payload, metadata_pointer)
        expected_pointers = ((0xcfbb0, start, 0xcfbcc), (0xcfbb4, start, 0xcfbb8))[index]
        if (size_pointer, blob_pointer, metadata_pointer) != expected_pointers:
            raise FormatError("bootstrap image descriptor pointers do not match the baseline")
        if size != end - start:
            raise FormatError("bootstrap declared image size differs from its ELF extent")
        if metadata != (0, 0x90000)[index]:
            raise FormatError("bootstrap catalog metadata word does not match the baseline")
        bounded(payload, blob_pointer, size, "bootstrap catalog image")
        descriptors.append({"slot": index, "descriptor_blob_file_offset": descriptor,
                            "size_pointer_blob_file_offset": size_pointer,
                            "image_blob_file_offset": start, "image_blob_file_end": end,
                            "declared_image_size": size,
                            "metadata_pointer_blob_file_offset": metadata_pointer,
                            "metadata_word": metadata})
    # Joint static derivation, not a hardware observation: driver programs
    # BORCH_END = payload size - 1; firmware reply uses that register + 0x201.
    literal(0x9298, 0x8be8, 0x100f6000, 0)
    word(0x929c, 0xe5900004)
    word(0x92a0, 0xe3001201)
    word(0x92a4, 0xe0804001)
    if len(anchors) > MAX_BOOTSTRAP_ANCHORS:
        raise FormatError("bootstrap instruction-anchor budget exceeded")
    scrub_end = len(payload) - 1
    return {"schema_version": 1, "isa": "A32", "endianness": "little",
            "regions": [{"blob_file_offset": start, "blob_file_end": end, "kind": kind}
                        for start, end, kind in (
                            (0, extents[0][0], "ARM bootstrap code, literals and data"),
                            (*extents[0], "embedded ARC ELF image 0"),
                            (*extents[1], "embedded ARC ELF image 1"),
                            (extents[1][1], len(payload), "ARM initialized data and padding"))],
            "cmac": {"length_slot_blob_file_offset": len(payload), "length": 16,
                     "blob_file_offset": len(payload) + 4, "authentication_verified": False},
            "image_catalog": {"root_blob_file_offset": 0xcfc00,
                              "table_blob_file_offset": root, "slot_count": 6,
                              "descriptor_blob_file_offsets": slots,
                              "callback_table_blob_file_offset": 0xcfcf0,
                              "callback_entry_blob_file_offsets": callbacks,
                              "images": descriptors},
            "instruction_anchors": anchors, "function_anchors": functions,
            "host_channel_open_policy": open_policy,
            "host_decoder_start": decoder_start,
            "host_mailbox_dispatch": {"arm_mailbox_address": 0x100e001c,
                                      "receive_entry_blob_file_offset": 0x8d48,
                                      "queue_entry_blob_file_offset": 0x8cf4,
                                      "dispatch_entry_blob_file_offset": 0x5f2c,
                                      "get_version_command": 0x73763004,
                                      "get_version_entry_blob_file_offset": 0x5ba4},
            "static_derived_layout": {
                "device_observed": False,
                "scrub_end": {"address": scrub_end, "source_kind": "driver",
                              "source": "driver/linux/FleaDefs.h:42"},
                "host_command": {"address": scrub_end + 1 + 0x100, "source_kind": "driver",
                                 "source": "driver/linux/FleaDefs.h:51; driver/linux/crystalhd_fleafuncs.c:1242"},
                "reply": {"address": scrub_end + 0x201, "source_kind": "driver and ARM anchors",
                          "instruction_blob_file_offsets": [0x9298, 0x929c, 0x92a0, 0x92a4]}},
            "limitations": ["Only fixed baseline instruction anchors are decoded; this is not a complete ARM call graph.",
                            "Region boundaries do not classify every byte as code or data; ARM also uses Thumb helpers.",
                            "Catalog metadata words and OL/IL physical destinations are not established.",
                            "SHA-256 identity is not CMAC authentication or runtime capability proof."]}


def _picture_output_map(payload, images):
    """Pure fixed A32 evidence; callers must pin the exact bundled SHA/size."""
    if len(payload) != BUNDLED_SIZE - TRAILER_SIZE:
        raise FormatError("picture-output payload size does not match the bundled baseline")
    identities = [(0x2ea60, 0x79dd8, 32, "little", 45, 2, 0x3a678),
                  (0x79dd8, 0xcfbb0, 32, "little", 45, 2, 0x49f68)]
    fields = ("blob_file_offset", "blob_file_end", "class", "endianness",
              "machine", "elf_type", "entry_virtual_address")
    if [tuple(i.get(field) for field in fields) for i in images] != identities:
        raise FormatError("picture-output ELF identities do not match the bundled baseline")

    # Only these independently decoded A32 sites are instructions. Neither the
    # whole prefix nor either embedded ARC image is treated as executable A32.
    groups = {
        "delivery": (
            (0x6ff4, 0xe92d4070), (0x6ff8, 0xe1a04001), (0x6ffc, 0xe59f5130),
            (0x7014, 0xe3140c02), (0x7018, 0x0a000002), (0x701c, 0xeb0001b9),
            (0x7020, 0xe3000200), (0x7024, 0xe5850008), (0x7708, 0xe92d4070),
            (0x7714, 0xebffe45f), (0x7720, 0xe51f1034), (0x7724, 0xe5911004),
            (0x7728, 0xe3002401), (0x772c, 0xe0811002), (0x7730, 0xe51f4088),
            (0x7734, 0xe5943024), (0x7768, 0xe3a02073), (0x776c, 0xe0020293),
            (0x7770, 0xe0804102), (0x7774, 0xe3a02020), (0x7778, 0xe2840f62),
            (0x777c, 0xeb009386), (0x7780, 0xe3a00001), (0x7784, 0xe5c401a8),
            (0x7788, 0xe8bd8070), (0x898, 0xe51f01a4), (0x89c, 0xe12fff1e),
        ),
        "pending_main": (
            (0x8e78, 0xe3a05000), (0x8e7c, 0xe1a00005), (0x8e80, 0xebfffa41),
            (0x8e84, 0xe3500000), (0x8e88, 0x0a000001), (0x8e8c, 0xe1a00005),
            (0x8e90, 0xebfffd2d), (0x8e94, 0xe2855001), (0x8e98, 0xe3550004),
            (0x8e9c, 0xbafffff6), (0x77ac, 0xe5d011a8), (0x77b0, 0xe3510000),
            (0x77b4, 0x0a000007), (0x77b8, 0xe5d000c4), (0x77bc, 0xe3500000),
            (0x77c0, 0x0a000004), (0x77c4, 0xe3a00001), (0x77c8, 0xe8bd8010),
            (0x77d8, 0xe3a00000), (0x77dc, 0xeafffff9), (0x77a0, 0xe3a01073),
            (0x77a4, 0xe0010194), (0x77a8, 0xe0800101), (0x778c, 0xe92d4010),
            (0x7790, 0xe1a04000), (0x7794, 0xebffe43f), (0x7798, 0xe3500000),
            (0x779c, 0x0a00000a),
        ),
        "picture_handler": (
            (0x834c, 0xe92d43f0), (0x8350, 0xe24dd0ac), (0x8394, 0xe5d400d2),
            (0x8398, 0xe3500001), (0x839c, 0x0a000005), (0x8354, 0xe1a09000),
            (0x8698, 0xe1a01009), (0x869c, 0xe3a00001), (0x86a0, 0xebfffc7c),
            (0x8714, 0xe1a01009), (0x8718, 0xe3a00001), (0x871c, 0xebfffc5d),
            (0x87a8, 0xe1a01009), (0x87ac, 0xe3a00000), (0x87b0, 0xebfffc38),
            (0x8820, 0xe5c471a8), (0x8358, 0xe3a07000), (0x83b4, 0xe8bd83f0),
        ),
        "bop": (
            (0x7898, 0xe59f3188), (0x789c, 0xe5932000), (0x78a0, 0xe3500000),
            (0x78a4, 0x1a000003), (0x78a8, 0xe3820001), (0x78ac, 0xe5830000),
            (0x78b0, 0xe51f0220), (0x78b4, 0xe5801008), (0x78b8, 0x0a000001),
            (0x78bc, 0xe3c20001), (0x78c0, 0xe5830000), (0x78c4, 0xe59f1160),
            (0x78c8, 0xe3a00001), (0x78cc, 0xe5810030), (0x78d0, 0xe3a0002b),
            (0x78d4, 0xea000d8f),
        ),
        "dnr": (
            (0x82d0, 0xe92d4010), (0x82d4, 0xe3a03000), (0x82d8, 0xe51f28b4),
            (0x82dc, 0xe3a04001), (0x82e0, 0xe5824404), (0x82e4, 0xe3500e2d),
            (0x82e8, 0x9a000000), (0x82ec, 0xe3a03801), (0x82f0, 0xe5823408),
            (0x82f4, 0xe7df159f), (0x82f8, 0xe1810800), (0x82fc, 0xe582040c),
            (0x8348, 0xe8bd8010), (0x8610, 0xe1cd03d4), (0x8614, 0xebffff2d),
        ),
        "mfd": (
            (0x1bfc, 0xe92d41f0), (0x1c00, 0xe1a06000), (0x1c08, 0xe1a04003),
            (0x1c34, 0xe5945014), (0x1c3c, 0xe5960000), (0x1c40, 0xe5906004),
            (0x1c9c, 0xe1a02005), (0x1ca0, 0xe59f125c), (0x1ca4, 0xe1a00006),
            (0x1ca8, 0xeb00730e), (0x84cc, 0xe28d3020), (0x84d0, 0xe1a01009),
            (0x84d4, 0xe28d2014), (0x84d8, 0xe1a00008), (0x84dc, 0xebffe5c6),
            (0x1e8e8, 0xe5903000), (0x1e8ec, 0xe7832001), (0x1e8f0, 0xe12fff1e),
        ),
        "scl": (
            (0x1cf4, 0xe92d4070), (0x1cf8, 0xe1a06001), (0x1cfc, 0xe1a05002),
            (0x1d00, 0xe5900000), (0x1d04, 0xe5904004), (0x1d08, 0xe3a0200c),
            (0x1d0c, 0xe59f1224), (0x1d10, 0xe1a00004), (0x1d14, 0xeb0072f3),
            (0x1d38, 0xe1855806), (0x1d3c, 0xe59f1200), (0x1d40, 0xe1a02005),
            (0x1d44, 0xe1a00004), (0x1d48, 0xeb0072e6), (0x1d6c, 0xe59f11dc),
            (0x1d70, 0xe1a02005), (0x1d74, 0xe1a00004), (0x1d78, 0xeb0072da),
            (0x1e4c, 0xe1a00004), (0x1e50, 0xe59f1130), (0x1e54, 0xe8bd4070),
            (0x1e58, 0xe3a02001), (0x1e5c, 0xea0072a1), (0x2034, 0xe5941014),
            (0x2038, 0xe1a00008), (0x203c, 0xe5942018), (0x2040, 0xebffff2b),
        ),
        "key_stubs": (
            (0x3ed8, 0xe92d4070), (0x3edc, 0xe3500000), (0x3ee0, 0x0a000009),
            (0x3ee4, 0xe2805014), (0x3ee8, 0xe2804f45), (0x3eec, 0xe28f0f7b),
            (0x3ef0, 0xeb007133), (0x3ef4, 0xe3a00000), (0x3ef8, 0xe5840008),
            (0x3efc, 0xe5950004), (0x3f00, 0xe5840004), (0x3f04, 0xe3a00000),
            (0x3f08, 0xe8bd8070), (0x3f0c, 0xe1a01000), (0x3f10, 0xe59f01fc),
            (0x3f14, 0xeb00712a), (0x3f18, 0xe3a00002), (0x3f1c, 0xeafffff9),
            (0x3f20, 0xe92d4070), (0x3f24, 0xe3500000), (0x3f28, 0x0a000009),
            (0x3f2c, 0xe2805014), (0x3f30, 0xe2804f45), (0x3f34, 0xe28f0f77),
            (0x3f38, 0xeb007121), (0x3f3c, 0xe3a00000), (0x3f40, 0xe5840008),
            (0x3f44, 0xe5950004), (0x3f48, 0xe5840004), (0x3f4c, 0xe3a00000),
            (0x3f50, 0xe8bd8070), (0x3f54, 0xe1a01000), (0x3f58, 0xe59f01ec),
            (0x3f5c, 0xeb007118), (0x3f60, 0xe3a00002), (0x3f64, 0xeafffff9),
        ),
        "key_callers": ((0x6908, 0xebfff584), (0x6928, 0xebfff56a)),
    }
    literals = {
        0x6ffc: (0x7134, 0x100f2000, 5), 0x7720: (0x76f4, 0x100f6000, 1),
        0x7730: (0x76b0, 0x100e0000, 4), 0x898: (0x6fc, 0xd3a00, 0),
        0x7898: (0x7a28, 0x10510000, 3), 0x78b0: (0x7698, 0xd2210, 0),
        0x78c4: (0x7a2c, 0x10540000, 1), 0x82d8: (0x7a2c, 0x10540000, 2),
        0x1ca0: (0x1f04, 0x00540014, 1), 0x1d0c: (0x1f38, 0x00540804, 1),
        0x1d3c: (0x1f44, 0x00540810, 1), 0x1d6c: (0x1f50, 0x0054081c, 1),
        0x1e50: (0x1f88, 0x00540854, 1), 0x3f10: (0x4114, 0x2cf84, 0),
        0x3f58: (0x414c, 0x2cfdc, 0),
    }
    anchors = []
    for group, sites in groups.items():
        for offset, expected in sites:
            if len(anchors) >= MAX_PICTURE_OUTPUT_ANCHORS:
                raise FormatError("picture-output instruction-anchor budget exceeded")
            actual = _bootstrap_word(payload, offset)
            if actual != expected:
                raise FormatError(f"picture-output word at {offset:#x} does not match the baseline")
            record = {"blob_file_offset": offset, "word": actual,
                      "operation": "validated word", "group": group}
            if offset in literals:
                record.update(_a32_literal(payload, offset))
                if (record["literal_blob_file_offset"], record["literal_value"],
                        record["destination_register"]) != literals[offset]:
                    raise FormatError("picture-output literal does not match the baseline")
            elif actual & 0x0e000000 == 0x0a000000:
                # Decode a branch only after matching its fixed audited word.
                # This includes the one fixed BLS at 0x82e8, not a general scan.
                displacement = actual & 0xffffff
                if displacement & 0x800000:
                    displacement -= 1 << 24
                target = offset + 8 + displacement * 4
                _bootstrap_word(payload, target)
                record.update(operation="BL" if actual & (1 << 24) else "B",
                              condition=actual >> 28, target_blob_file_offset=target)
            anchors.append(record)

    diagnostics = []
    for offset, expected in (
            (0x40e0, b"[fw] SMP_CmdIf_SetSessionKey(): NOT Implemented\n"),
            (0x4118, b"[fw] SMP_CmdIf_SetContentKey(): NOT Implemented\n"),
            (0x2cf84, b"[fw] SMP_CmdIf_SetSessionKey(): Invalid Parameter with Command Header Address = 0x%x\n"),
            (0x2cfdc, b"[fw] SMP_CmdIf_SetContentKey(): Invalid Parameter with Command Header Address = 0x%x\n")):
        if bounded(payload, offset, len(expected) + 1, "picture-output diagnostic") != expected + b"\0":
            raise FormatError("picture-output diagnostic does not match the baseline")
        diagnostics.append({"blob_file_offset": offset, "text": expected.decode("ascii")})
    # ADR r0,PC+imm for the two non-null diagnostics; rotated-immediate A32.
    for offset, target in ((0x3eec, 0x40e0), (0x3f34, 0x4118)):
        instruction = _bootstrap_word(payload, offset)
        rotation = ((instruction >> 8) & 15) * 2
        value = instruction & 255
        immediate = ((value >> rotation) | (value << ((32 - rotation) % 32))) & 0xffffffff
        if offset + 8 + immediate != target:
            raise FormatError("picture-output diagnostic ADR does not match the baseline")

    rdb = "include/flea/70015/magnum/basemodules/chp/70015/rdb/a0/"
    # Context-relative writes do not establish that context's physical base.
    # Direct ARM physical addresses and RDB offsets are separate namespaces.
    operations = [
        ("BOP_AES_CTRL", 0x10510000, 0x00510000, [0x78ac, 0x78c0], 1,
         "START_ENCRYPTION_SCRAMBLE: set when r0==0, clear otherwise", "bchp_bop_aes.h:97"),
        ("MFD_PIC_FEED_CMD", 0x10540030, 0x00540030, [0x78cc], 1,
         "START_FEED written as 1", "bchp_mfd.h:323"),
        ("MFD_DISP_HSIZE", None, 0x00540014, [0x1ca8], 0x1fff,
         "descriptor word at offset 0x14 passed to register-write helper", "bchp_mfd.h:232"),
        ("DNR_DNR_TOP_CTRL", 0x10540404, 0x00540404, [0x82e0], 1,
         "DNR_ENABLE written as 1", "bchp_dnr.h:108"),
        ("DNR_LINE_STORE_CONFIG", 0x10540408, 0x00540408, [0x82f0], 0x10000,
         "LS_MODE HD bit set only when input width > 720", "bchp_dnr.h:121"),
        ("DNR_SRC_PIC_SIZE", 0x1054040c, 0x0054040c, [0x82fc], 0x07ff07ff,
         "width<<16 | (height & 0x7ff); no width masking is established", "bchp_dnr.h:138"),
        ("SCL_HD_TOP_CONTROL", None, 0x00540804, [0x1d14], 12,
         "ENABLE_CTRL and UPDATE_SEL picture-controlled bits written as 12", "bchp_scl_hd.h:299"),
        ("SCL_HD_BVB_IN_SIZE", None, 0x00540810, [0x1d48], 0x07ff07ff,
         "width<<16 | height passed to register-write helper", "bchp_scl_hd.h:410"),
        ("SCL_HD_DEST_PIC_SIZE", None, 0x0054081c, [0x1d78], 0x07ff07ff,
         "same packed value as BVB_IN_SIZE in this selected path", "bchp_scl_hd.h:467"),
        ("SCL_HD_ENABLE", None, 0x00540854, [0x1e5c], 1,
         "enable value 1 passed to register-write helper", "bchp_scl_hd.h:657"),
    ]
    return {
        "schema_version": 1, "isa": "A32", "endianness": "little", "device_observed": False,
        "instruction_anchors": anchors, "diagnostics": diagnostics,
        "descriptor_delivery": {
            "reader_entry_blob_file_offset": 0x7708, "arm2_callback_call_blob_file_offset": 0x701c,
            "arm_mailbox_physical_address": 0x100e0024, "arm_mailbox_rdb_address": 0x000e0024,
            "source_expression": "BORCH_END + 0x401", "slot_stride_bytes": 0x1cc,
            "destination_slot_offset": 0x188, "copy_argument_bytes": 32,
            "copy_helper_entry_blob_file_offset": 0x2c59c, "copy_helper_body_validated": False,
            "pending_slot_offset": 0x1a8, "active_slot_offset": 0xc4,
            "main_slot_range": [0, 3], "main_call_blob_file_offset": 0x8e90,
            "picture_handler_entry_blob_file_offset": 0x834c,
            "started_slot_offset": 0xd2, "pending_clear_blob_file_offset": 0x8820,
            "host_record_source": "include/flea/DriverFwShare.h:22",
            "host_submit_source": "driver/linux/crystalhd_fleafuncs.c:2199",
            "complete_dma_ownership_verified": False},
        "picture_feed": {
            "entry_blob_file_offset": 0x7898,
            "callers": [{"blob_file_offset": offset, "r0": value, "r1": "slot"}
                        for offset, value in ((0x86a0, 1), (0x871c, 1), (0x87b0, 0))],
            "scope": "Picture-feed trigger and output encryption/scramble control, not generic AES or input decryption."},
        "register_operations": [
            {"name": name, "arm_physical_address": physical, "rdb_address": address,
             "instruction_blob_file_offsets": sites, "field_mask": mask,
             "selected_operation": operation, "source": rdb + source,
             "register_write_helper_entry_blob_file_offset": 0x1e8e8 if physical is None else None}
            for name, physical, address, sites, mask, operation, source in operations],
        "key_handler_stubs": [
            {"name": name, "entry_blob_file_offset": entry, "last_instruction_blob_file_offset": end,
             "caller_blob_file_offset": caller, "diagnostic_blob_file_offset": diagnostic,
             "nonnull_reply_status": 0, "request_record_offset": 0x14, "reply_record_offset": 0x114,
             "sequence_word_index": 1, "status_word_index": 2,
             "key_payload_reads_in_bounded_body": False, "key_provisioning_verified": False,
             "scope": "Non-null record ACK stub with NOT Implemented diagnostic; status zero is not key provisioning."}
            for name, entry, end, caller, diagnostic in (
                ("SetSessionKey", 0x3ed8, 0x3f1c, 0x6928, 0x40e0),
                ("SetContentKey", 0x3f20, 0x3f64, 0x6908, 0x4118))],
        "deferred": ["CSC dispatcher routing and coefficient programming are not validated here.",
                     "Full scaler/filter configuration and arbitrary raw-frame operations are not validated here."],
        "limitations": ["Only fixed A32 anchors and bounded diagnostic data are validated, not a complete call graph.",
                        "Descriptor submission is not complete DMA ownership, lifetime or completion proof.",
                        "ARM physical address/RDB correspondence does not establish host GISB access, clock/reset readiness or safe reads.",
                        "Context-relative register writes do not establish their physical base.",
                        "The log routine and copy-helper implementations, ARC paths and Thumb helpers are not decoded.",
                        "No hardware execution, key provisioning, cipher transformation or standalone processing capability is verified."]}


def string_at(table, offset):
    if offset < 0 or offset >= len(table):
        raise FormatError("string index is outside its ELF string table")
    # Keep malformed string tables bounded even with many distinct indexes.
    end = table.find(b"\0", offset, min(len(table), offset + 4096))
    if end < 0:
        raise FormatError("unterminated or oversized ELF string")
    return table[offset:end].decode("ascii", errors="backslashreplace")


def function_intervals(symbol_tables, sections):
    """Index sized functions by section, without collapsing aliases or tables."""
    by_section = {}
    for symbols in symbol_tables.values():
        for symbol in symbols:
            index = symbol["section_index"]
            if (symbol["type"] == 2 and symbol["size"] and 0 < index < len(sections)
                    and sections[index]["flags"] & 4):
                by_section.setdefault(index, []).append(symbol)
    result = {}
    for index, symbols in by_section.items():
        symbols.sort(key=lambda symbol: (symbol["elf_virtual_address"],
                                         symbol["symbol_table_section_index"],
                                         symbol["symbol_index"]))
        starts = []
        prefix_ends = []
        end = 0
        for symbol in symbols:
            start = symbol["elf_virtual_address"]
            end = max(end, start + symbol["size"])
            starts.append(start)
            prefix_ends.append(end)
        result[index] = (symbols, starts, prefix_ends)
    return result


def parse_references(image, base, sections, symbol_tables, wanted, all_symbols,
                     relocation_budget, owner_budget, output_budget):
    # ET_EXEC r_offset is a VA in sh_info's section, not a file offset. Keep
    # numeric types: old ARC ABI revisions disagree on their names/semantics.
    # https://gabi.xinuos.com/elf/06-reloc.html
    tables = [(index, section) for index, section in enumerate(sections)
              if section["type"] in (4, 9)]  # SHT_RELA / SHT_REL
    if any(table["type"] == 9 for _, table in tables):
        raise FormatError("SHT_REL references without explicit addends are unsupported")
    count = sum(table["size"] // 12 for _, table in tables)
    if count > relocation_budget:
        raise FormatError("ELF relocation-record budget exceeded")
    intervals = function_intervals(symbol_tables, sections)
    references = []
    type_counts = {}
    noop_count = unowned_count = owner_steps = output_bytes = 0
    encoded_symbol_sizes = {}
    for table_index, table in tables:
        if table["entry_size"] != 12 or table["size"] % 12:
            raise FormatError("invalid ELF32 RELA table size")
        if table["link"] not in symbol_tables:
            raise FormatError("ELF relocations do not link to a symbol table")
        if not 0 < table["info"] < len(sections) or sections[table["info"]]["type"] == 0:
            raise FormatError("ELF relocation target section index is invalid")
        source_section = sections[table["info"]]
        symbols = symbol_tables[table["link"]]
        for offset in range(table["offset"], table["offset"] + table["size"], 12):
            address, info, addend = struct.unpack_from("<IIi", image, offset)
            symbol_index, kind = info >> 8, info & 0xff
            if symbol_index >= len(symbols):
                raise FormatError("ELF relocation symbol index is outside its symbol table")
            type_counts[str(kind)] = type_counts.get(str(kind), 0) + 1
            # The blob retains no-ops at the exclusive end of .text. They
            # represent no source byte and cannot be used to infer an edge.
            if kind == 0:
                noop_count += 1
                continue
            delta = address - source_section["address"]
            if not 0 <= delta < source_section["size"]:
                raise FormatError("ELF relocation source extends outside its target section")
            source_file_offset = (None if source_section["type"] == 8 else
                                  base + source_section["offset"] + delta)
            owners = []
            if table["info"] in intervals:
                functions, starts, prefix_ends = intervals[table["info"]]
                index = bisect_right(starts, address) - 1
                while index >= 0 and prefix_ends[index] > address:
                    if owner_steps >= owner_budget:
                        raise FormatError("ELF function-owner lookup budget exceeded")
                    owner_steps += 1
                    function = functions[index]
                    if address < function["elf_virtual_address"] + function["size"]:
                        owners.append(function)
                    index -= 1
                owners.sort(key=lambda symbol: (symbol["symbol_table_section_index"],
                                                symbol["symbol_index"]))
            if not owners and source_section["flags"] & 4:
                unowned_count += 1
            target = symbols[symbol_index]
            if not (all_symbols or target["name"] in wanted or
                    any(owner["name"] in wanted for owner in owners)):
                continue
            # S+A is only an arithmetic candidate, NOT an applied relocation.
            # Never resolve via a global VA search: overlay sections share VAs.
            candidate = target["elf_virtual_address"] + addend
            candidate_in_section = False
            candidate_file_offset = None
            target_index = target["section_index"]
            if 0 < target_index < len(sections) and 0 <= candidate < 1 << 32:
                target_section = sections[target_index]
                candidate_delta = candidate - target_section["address"]
                candidate_in_section = 0 <= candidate_delta < target_section["size"]
                if (candidate_in_section and target_section["type"] not in (0, 8)
                        and target_section["flags"] & 2):
                    candidate_file_offset = base + target_section["offset"] + candidate_delta
            # Repeated long names/aliases must not expand a small input into
            # unbounded JSON. Cache symbol accounting, not per-use output.
            cost = 1024 + 6 * len(source_section["name"])
            for symbol in [target] + owners:
                identity = (symbol["symbol_table_section_index"], symbol["symbol_index"])
                if identity not in encoded_symbol_sizes:
                    encoded_symbol_sizes[identity] = len(json.dumps(symbol, ensure_ascii=True)) + 768
                cost += encoded_symbol_sizes[identity]
            if output_bytes + cost > output_budget:
                raise FormatError("ELF reference-output byte budget exceeded")
            output_bytes += cost
            references.append({"relocation_record_offset": base + offset,
                               "relocation_section_index": table_index,
                               "relocation_type": kind, "addend": addend,
                               "source": {"section_index": table["info"],
                                          "section": source_section["name"],
                                          "elf_virtual_address": address,
                                          "blob_file_offset": source_file_offset,
                                          "function_owners": owners},
                               "target": {"symbol": target,
                                          "addend_candidate_virtual_address": candidate,
                                          "addend_candidate_blob_file_offset": candidate_file_offset,
                                          "addend_candidate_in_section": candidate_in_section}})
    return {"relocation_count": count, "relocation_type_counts": type_counts,
            "relocation_noop_count": noop_count,
            "unowned_executable_reference_count": unowned_count,
            "owner_lookup_steps": owner_steps, "reference_output_budget_used": output_bytes,
            "references": references}


def parse_elf(payload, base, wanted, symbol_budget, string_budget,
              references=False, all_symbols=False, relocation_budget=0,
              owner_budget=0, output_budget=0, metadata_budget=MAX_METADATA_OUTPUT_BYTES,
              arc_metadata=False):
    # ELF32 Ehdr/Phdr/Shdr/Sym layouts follow https://gabi.xinuos.com/elf/.
    image = memoryview(payload)[base:]
    header = bounded(image, 0, 52, "ELF header")
    if bytes(header[:7]) != b"\x7fELF\x01\x01\x01":
        raise FormatError("only ELF32 little-endian version 1 is supported")
    fields = struct.unpack_from("<HHIIIIIHHHHHH", header, 16)
    kind, machine, version, entry, phoff, shoff, flags = fields[:7]
    ehsize, phsize, phcount, shsize, shcount, names_index = fields[7:]
    if kind != 2 or machine != 45 or version != 1:
        raise FormatError("embedded image is not an ELF32 ARC executable")
    if ehsize != 52 or phsize != 32 or shsize != 40:
        raise FormatError("unsupported ELF header entry size")
    if (not phcount or phcount == 0xffff or not 0 < shcount < 0xff00
            or names_index >= shcount):
        raise FormatError("missing or unsupported extended ELF header table")
    bounded(image, phoff, phsize * phcount, "ELF program table")
    bounded(image, shoff, shsize * shcount, "ELF section table")
    extent = max(52, phoff + phsize * phcount, shoff + shsize * shcount)
    segments = []
    for index in range(phcount):
        ptype, offset, address, physical, filesz, memsz, pflags, align = (
            struct.unpack_from("<8I", image, phoff + index * phsize))
        if ptype == 1 and filesz > memsz:
            raise FormatError("ELF load segment has more file bytes than memory bytes")
        if ptype == 1 and address + memsz > 1 << 32:
            raise FormatError("ELF load segment overflows its 32-bit virtual address range")
        if filesz:
            bounded(image, offset, filesz, "ELF segment")
            extent = max(extent, offset + filesz)
        if ptype == 1:
            segments.append({"elf_virtual_address": address,
                             "blob_file_offset": base + offset if filesz else None,
                             "file_size": filesz, "memory_size": memsz,
                             "flags": pflags})

    sections = []
    for index in range(shcount):
        fields = struct.unpack_from("<10I", image, shoff + index * shsize)
        section = dict(zip(("name_index", "type", "flags", "address", "offset",
                            "size", "link", "info", "align", "entry_size"), fields))
        if section["flags"] & 2 and section["address"] + section["size"] > 1 << 32:
            raise FormatError("ELF section overflows its 32-bit virtual address range")
        # SHT_NULL and SHT_NOBITS do not occupy file-backed bytes.
        if section["type"] not in (0, 8) and section["size"]:
            bounded(image, section["offset"], section["size"], "ELF section")
            extent = max(extent, section["offset"] + section["size"])
        sections.append(section)
    names = sections[names_index]
    if names["type"] != 3:
        raise FormatError("ELF section names do not link to a string table")
    if names["size"] > string_budget:
        raise FormatError("ELF string-table byte budget exceeded")
    name_table = bytes(bounded(image, names["offset"], names["size"], "section names"))
    string_table_bytes = names["size"]
    string_cache = {(names["offset"], names["size"]): name_table}
    metadata_bytes = 0
    for section in sections:
        name = string_at(name_table, section["name_index"])
        # Bound decoded names before retaining them, including repeated names
        # and metadata-only --all-symbols runs that never parse references.
        metadata_bytes += 1024 + 6 * len(name)
        if metadata_bytes > metadata_budget:
            raise FormatError("ELF retained-metadata byte budget exceeded")
        section["name"] = name

    symbols = []
    symbol_count = 0
    # SHT_SYMTAB is retained linker metadata, not merely matching strings.
    symbol_tables = [(index, table) for index, table in enumerate(sections) if table["type"] == 2]
    if sum(table["size"] // 16 for _, table in symbol_tables) > symbol_budget:
        raise FormatError("ELF symbol-record budget exceeded")
    indexed_symbols = {}
    for table_index, table in symbol_tables:
        if references:
            indexed_symbols[table_index] = []
        if table["entry_size"] != 16 or table["size"] % 16:
            raise FormatError("invalid ELF32 symbol table size")
        if table["link"] >= shcount or sections[table["link"]]["type"] != 3:
            raise FormatError("ELF symbols do not link to a string table")
        if not table["size"]:
            continue
        strings = sections[table["link"]]
        key = (strings["offset"], strings["size"])
        if key not in string_cache:
            if string_table_bytes + strings["size"] > string_budget:
                raise FormatError("ELF string-table byte budget exceeded")
            string_cache[key] = bytes(bounded(image, *key, "symbol names"))
            string_table_bytes += strings["size"]
        string_table = string_cache[key]
        for offset in range(table["offset"], table["offset"] + table["size"], 16):
            name_index, value, size, info, other, section_index = (
                struct.unpack_from("<IIIBBH", image, offset))
            symbol_count += 1
            name = string_at(string_table, name_index)
            file_offset = None
            section_name = None
            if 0 < section_index < shcount:
                section = sections[section_index]
                section_name = section["name"]
                delta = value - section["address"]
                if delta < 0 or delta > section["size"] - size:
                    raise FormatError(f"symbol {name} extends outside its section")
                if (section["type"] not in (0, 8) and section["flags"] & 2
                        and delta < section["size"]):
                    file_offset = base + section["offset"] + delta
            elif section_index < 0xff00 and section_index != 0:
                raise FormatError("symbol section index is outside the ELF section table")
            elif section_index == 0xffff:
                raise FormatError("extended symbol section indexes are unsupported")
            if not (references or all_symbols or name in wanted):
                continue
            metadata_bytes += 1024 + 6 * (len(name) + len(section_name or ""))
            if metadata_bytes > metadata_budget:
                raise FormatError("ELF retained-metadata byte budget exceeded")
            symbol = {"name": name, "elf_virtual_address": value,
                      "blob_file_offset": file_offset, "size": size,
                      "type": info & 15, "binding": info >> 4,
                      "visibility": other & 3, "section": section_name,
                      "section_index": section_index,
                      "symbol_record_offset": base + offset}
            if references or all_symbols:
                symbol["symbol_table_section_index"] = table_index
                symbol["symbol_index"] = (offset - table["offset"]) // 16
            if references:
                indexed_symbols[table_index].append(symbol)
            if all_symbols or name in wanted:
                symbols.append(symbol)

    role_hints = [section["name"] for section in sections
                  if "outerloop" in section["name"] or "innerloop" in section["name"]]
    result = {"blob_file_offset": base, "blob_file_end": base + extent,
            "class": 32, "endianness": "little", "machine": machine,
            "elf_type": kind, "flags": flags, "entry_virtual_address": entry,
            "program_header_count": phcount, "section_count": shcount,
            "symbol_count": symbol_count, "string_table_bytes": string_table_bytes,
            "load_segments": segments,
            "role_hint_sections": role_hints,
            "symbols": symbols,
            "missing_symbols": sorted(wanted - {symbol["name"] for symbol in symbols}),
            "_metadata_budget_used": metadata_bytes}
    if all_symbols or arc_metadata:
        section_metadata = [{"section_index": index, "name": section["name"],
                               "type": section["type"], "flags": section["flags"],
                               "elf_virtual_address": section["address"],
                               "blob_file_offset": (base + section["offset"]
                                                    if section["type"] not in (0, 8)
                                                    and section["size"] else None),
                               "size": section["size"], "link": section["link"],
                               "info": section["info"], "align": section["align"],
                               "entry_size": section["entry_size"],
                               "section_header_blob_file_offset": base + shoff + index * shsize}
                              for index, section in enumerate(sections)]
        if all_symbols:
            result["sections"] = section_metadata
        if arc_metadata:
            result["_arc_sections"] = [s for s in section_metadata if s["name"] in
                                       (".comment", ".arcextmap", ".ARC.attributes")]
    if references:
        result.update(parse_references(image, base, sections, indexed_symbols, wanted,
                                       all_symbols, relocation_budget, owner_budget, output_budget))
    return result


def analyze(data, wanted=DEFAULT_SYMBOLS, expected_sha256=BUNDLED_SHA256,
            references=False, all_symbols=False, bootstrap=False, picture_output=False,
            arc_metadata=False):
    if len(data) < 24 or len(data) > MAX_FIRMWARE_SIZE or len(data) % 4:
        raise FormatError("invalid BCM70015 firmware size")
    sha256 = hashlib.sha256(data).hexdigest()
    if sha256 != expected_sha256:
        raise FormatError("firmware SHA-256 does not match --expect-sha256")
    if bootstrap and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--bootstrap requires the exact bundled firmware SHA-256 and size")
    if picture_output and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--picture-output requires the exact bundled firmware SHA-256 and size")
    if arc_metadata and (sha256 != BUNDLED_SHA256 or len(data) != BUNDLED_SIZE):
        raise FormatError("--arc-metadata requires the exact bundled firmware SHA-256 and size")
    payload = data[:-TRAILER_SIZE]
    length_slot = struct.unpack_from("<I", data, len(payload))[0]
    if length_slot != 16:
        raise FormatError("firmware trailer does not match the BCM70015 signature layout")
    images = []
    arc_sections = []
    symbol_budget = MAX_SYMBOL_RECORDS
    string_budget = MAX_STRING_TABLE_BYTES
    relocation_budget = MAX_RELOCATION_RECORDS
    owner_budget = MAX_OWNER_LOOKUP_STEPS
    output_budget = MAX_REFERENCE_OUTPUT_BYTES
    metadata_budget = MAX_METADATA_OUTPUT_BYTES
    wanted = set(wanted)
    offset = payload.find(b"\x7fELF")
    while offset >= 0:
        if len(images) == 16:
            raise FormatError("too many embedded ELF images")
        image = parse_elf(payload, offset, wanted, symbol_budget, string_budget,
                          references, all_symbols, relocation_budget, owner_budget,
                          output_budget, metadata_budget, arc_metadata)
        if arc_metadata:
            arc_sections.append(image.pop("_arc_sections"))
        symbol_budget -= image["symbol_count"]
        string_budget -= image["string_table_bytes"]
        metadata_budget -= image.pop("_metadata_budget_used")
        if references:
            relocation_budget -= image["relocation_count"]
            owner_budget -= image["owner_lookup_steps"]
            output_budget -= image["reference_output_budget_used"]
        if images and offset < images[-1]["blob_file_end"]:
            raise FormatError("embedded ELF file extents overlap")
        images.append(image)
        offset = payload.find(b"\x7fELF", offset + 4)
    if not images:
        raise FormatError("no embedded ELF32 ARC executables found")
    vectors = []
    for offset in (0, 4, 8, 12, 16, 24, 28):
        instruction = struct.unpack_from("<I", payload, offset)[0]
        if instruction & 0xfffff000 == 0xe59ff000:
            literal = offset + 8 + (instruction & 0xfff)
            target = struct.unpack("<I", bounded(payload, literal, 4, "ARM vector literal"))[0]
            vectors.append({"blob_file_offset": offset,
                            "literal_blob_file_offset": literal, "target_value": target})
    revisions = [{"blob_file_offset": match.start(),
                  "text": match.group().decode("ascii")}
                 for match in re.finditer(rb"\$Media_PC_FW_Rev: [0-9.]+ \$", payload)]
    result = {"schema_version": 1, "sha256": sha256,
            "git_blob_sha1": hashlib.sha1(b"blob " + str(len(data)).encode("ascii")
                                         + b"\0" + data).hexdigest(),
            "bundled_baseline": sha256 == BUNDLED_SHA256, "size": len(data),
            "payload_end": len(payload), "trailer_length_slot": length_slot,
            "signature_file_offset": len(data) - 16,
            "signature_verified": False, "firmware_revisions": revisions,
            "arm_vector_candidates": vectors, "images": images,
            "limitations": ["ELF virtual addresses are not host/device DRAM addresses.",
                            "Role hints do not identify an exact ARC core or usable codecs.",
                            "Symbols do not prove UART or host-mailbox accessibility.",
                            "ARM image extent and command call graph are not established."]}
    if references:
        result["limitations"].extend([
            "Retained references are incomplete and are not a proven instruction call graph.",
            "Relocation types are numeric; their ARC encoding semantics are not applied.",
            "S+A arithmetic candidates are not resolved targets; file mappings require containment in the referenced section.",
            "No-op relocations have no reference edge; unowned sites are not assigned to nearby functions."])
    if bootstrap:
        result["bootstrap"] = _bootstrap_map(payload, images)
    if picture_output:
        result["picture_output"] = _picture_output_map(payload, images)
    if arc_metadata:
        result["arc_metadata"] = _arc_metadata_map(payload, images, arc_sections)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Linux only; reads a regular firmware file and emits JSON to stdout. "
        "No device ioctls, firmware execution, extraction or patching. "
        "Supply an ordinary offline file, not a sysfs/debugfs attribute. "
        "The expected SHA-256 defaults to the bundled BCM70015 blob; "
        "all offsets refer to that exact input file."))
    parser.add_argument("firmware", help="regular firmware file (not a symlink)")
    parser.add_argument("--expect-sha256", default=BUNDLED_SHA256,
                        help="explicitly select another SHA-256-pinned blob")
    parser.add_argument("--symbol", action="append", help="exact ELF symbol name; repeatable")
    parser.add_argument("--references", action="store_true", help=(
        "inventory retained RELA references whose target or sized source function matches --symbol; "
        "not disassembly or a complete call graph"))
    parser.add_argument("--all-symbols", action="store_true", help=(
        "include all symbol records and section metadata; with --references include all retained references"))
    parser.add_argument("--bootstrap", action="store_true", help=(
        "validate fixed ARM bootstrap/mailbox anchors and image catalog; bundled firmware only"))
    parser.add_argument("--picture-output", action="store_true", help=(
        "validate fixed picture-output and key ACK-stub anchors; bundled firmware only, not capability proof"))
    parser.add_argument("--arc-metadata", action="store_true", help=(
        "validate stored ARC compiler hints and extension declarations; bundled firmware only, not ISA proof"))
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.expect_sha256):
        parser.error("--expect-sha256 must be 64 hexadecimal digits")
    try:
        report = analyze(read_firmware(args.firmware), args.symbol or DEFAULT_SYMBOLS,
                         args.expect_sha256.lower(), args.references, args.all_symbols, args.bootstrap,
                         args.picture_output, args.arc_metadata)
    except (OSError, FormatError) as error:
        print(f"flea_fw_map: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
