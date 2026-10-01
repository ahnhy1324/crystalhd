#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline, hash-pinned BCM70015 firmware and embedded ELF32 symbol inventory."""

import argparse
import hashlib
import json
import os
import re
import stat
import struct
import sys


BUNDLED_SHA256 = "8bf3a68f5c64686358a52274e40911a88c7f8c67ecbf6cf1557a49b4d7bc67c9"
MAX_FIRMWARE_SIZE = 4 * 1024 * 1024  # include/crystalhd_ioctl_limits.h
TRAILER_SIZE = 20  # driver/linux/FleaDefs.h: length slot plus 16-byte CMAC
MAX_SYMBOL_RECORDS = 65536  # Aggregate across all tables/images, including duplicates.
MAX_STRING_TABLE_BYTES = MAX_FIRMWARE_SIZE
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


def string_at(table, offset):
    if offset < 0 or offset >= len(table):
        raise FormatError("string index is outside its ELF string table")
    # Keep malformed string tables bounded even with many distinct indexes.
    end = table.find(b"\0", offset, min(len(table), offset + 4096))
    if end < 0:
        raise FormatError("unterminated or oversized ELF string")
    return table[offset:end].decode("ascii", errors="backslashreplace")


def parse_elf(payload, base, wanted, symbol_budget, string_budget):
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
    if not phcount or phcount == 0xffff or not shcount or names_index >= shcount:
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
    for section in sections:
        section["name"] = string_at(name_table, section["name_index"])

    symbols = []
    symbol_count = 0
    # SHT_SYMTAB is retained linker metadata, not merely matching strings.
    symbol_tables = [table for table in sections if table["type"] == 2]
    if sum(table["size"] // 16 for table in symbol_tables) > symbol_budget:
        raise FormatError("ELF symbol-record budget exceeded")
    for table in symbol_tables:
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
            if name not in wanted:
                continue
            symbols.append({"name": name, "elf_virtual_address": value,
                            "blob_file_offset": file_offset, "size": size,
                            "type": info & 15, "binding": info >> 4,
                            "visibility": other & 3, "section": section_name,
                            "section_index": section_index,
                            "symbol_record_offset": base + offset})

    role_hints = [section["name"] for section in sections
                  if "outerloop" in section["name"] or "innerloop" in section["name"]]
    return {"blob_file_offset": base, "blob_file_end": base + extent,
            "class": 32, "endianness": "little", "machine": machine,
            "elf_type": kind, "flags": flags, "entry_virtual_address": entry,
            "program_header_count": phcount, "section_count": shcount,
            "symbol_count": symbol_count, "string_table_bytes": string_table_bytes,
            "load_segments": segments,
            "role_hint_sections": role_hints,
            "symbols": symbols,
            "missing_symbols": sorted(wanted - {symbol["name"] for symbol in symbols})}


def analyze(data, wanted=DEFAULT_SYMBOLS, expected_sha256=BUNDLED_SHA256):
    if len(data) < 24 or len(data) > MAX_FIRMWARE_SIZE or len(data) % 4:
        raise FormatError("invalid BCM70015 firmware size")
    sha256 = hashlib.sha256(data).hexdigest()
    if sha256 != expected_sha256:
        raise FormatError("firmware SHA-256 does not match --expect-sha256")
    payload = data[:-TRAILER_SIZE]
    length_slot = struct.unpack_from("<I", data, len(payload))[0]
    if length_slot != 16:
        raise FormatError("firmware trailer does not match the BCM70015 signature layout")
    images = []
    symbol_budget = MAX_SYMBOL_RECORDS
    string_budget = MAX_STRING_TABLE_BYTES
    wanted = set(wanted)
    offset = payload.find(b"\x7fELF")
    while offset >= 0:
        if len(images) == 16:
            raise FormatError("too many embedded ELF images")
        image = parse_elf(payload, offset, wanted, symbol_budget, string_budget)
        symbol_budget -= image["symbol_count"]
        string_budget -= image["string_table_bytes"]
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
    return {"schema_version": 1, "sha256": sha256,
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
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.expect_sha256):
        parser.error("--expect-sha256 must be 64 hexadecimal digits")
    try:
        report = analyze(read_firmware(args.firmware), args.symbol or DEFAULT_SYMBOLS,
                         args.expect_sha256.lower())
    except (OSError, FormatError) as error:
        print(f"flea_fw_map: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
