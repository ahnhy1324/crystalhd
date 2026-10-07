#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Hardware-free execution of pinned stock A32 allocation and metadata bodies.

Guest execution is restricted to exact bodies; literals are pinned separately.
Allocation/OPEN callees use synthetic contracts; handoff logging is synthetic.
Allocation uses a Python reference; OPEN and handoff use complete page oracles.
Metadata handoff executes the stock physical-to-virtual helper in bounded RAM.
Cached-picture replay uses synthetic critical-region events, not privileged
IRQ/FIQ execution, DMA/barrier visibility, hardware completion or a source lease.
This does not emulate the card, ARC, firmware boot, DMA or payload ownership.
"""
import hashlib
import importlib.util
from contextlib import ExitStack
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("fw_tests", ROOT / "tests/fw-research.py")
FW = importlib.util.module_from_spec(spec)
spec.loader.exec_module(FW)
Model = FW.FirmwareOpenAllocationTests
RETURN = 0x10000
CALLEES = (0x1f5d4, 0x2b628, 0x1fe6c, 0x203c4)
OPEN_START, OPEN_END, OPEN_SP = 0x27480, 0x275c4, 0x300300
OPEN_DIGEST = "c356d9c46eee35ea6dafb60e7e1ef49cf37c77726e4ba14e5e192f9cc2c50f91"
OPEN_LITERAL = (0x27610, bytes.fromhex("02007673"))
METADATA_BODIES = (
    ("release", 0xd5a4, 0xd624, "8276e18c409aa706a5b3e92b880887c4157256253d7be40312e28a7464b9c812"),
    ("acquire", 0xd624, 0xd6d4, "e6ff28c676a32fe6219c53e6f40f2f23589e6f7f1f7235e4a46c95baf619907a"),
    ("peek", 0xd718, 0xd780, "013bcc90b5e7d904f03f9787cc7e820978a711eba2fb37c95a63a7b8324c374d"),
    ("translate", 0x1fdac, 0x1fe6c, "08d03815210fc1e069068765847cc4c5d847bd5df744f223f9851ee985e05d28"),
)
METADATA_LITERALS = (
    (0xd780, b"BXVD_PMQM_P_ReleasePicture: PictureRelease Q is corrupted %d\0"),
    (0xd7c0, b"BXVD_PMQM_P_ReleasePicture: PictureRelease Q is full %d\0"),
    (0xd7f8, b"\nrelease ptr offset:%d, addr 0x%x\0"),
    (0xd81c, b"get PPB :pstDispElem->pPPBPhysical:0x%x,pstDispElem->pPPB:0x%x \0"),
)
METADATA_CALLS = {
    0xd5d4: 0x203c4, 0xd5f8: 0x203c4, 0xd618: 0x203c4,
    0xd69c: 0x1fdac, 0xd6ac: 0x203c4, 0xd774: 0x1fdac,
}
META_DEL, META_REL, META_RECORD = 0x400100, 0x401100, 0x500100
META_MAP, META_HEAD, META_SP = 0x600100, 0x600400, 0x300800
META_PAGES = (Model.H, 0x300000, 0x400000, 0x401000, 0x500000, 0x600000)
PICTURE_BODIES = (
    ("picture", 0x834c, 0x8838, "e2e7a336ede34b0f3f170cc26476debb81ad4cd0eb3862dd1ab3968db7ee07a3"),
    ("format", 0x78dc, 0x79a8, "2b48db9af14d92ab297a738cae94fbb693c6af39b6a2929a47097b7ac079fa91"),
    ("irq", 0x888c, 0x8948, "110a0aa7e8d7845c6ba5854114808d7cc9e067dd2bcac1a746e4a159d15d6ba5"),
)
# Complete source guards are pinned but never executed as privileged guest code.
PICTURE_GUARDS = (
    ("root", 0x898, 0x8a0, "fed8d9727528a2abbf8b8b5c258b5d558e3192d5fc3232bc4a7f0a1c91cd6e38"),
    ("register", 0x703c, 0x70e4, "dea84781aa0e00184e96726f8cdc0a49537ca4c0672651a8556a11b03c10b1a9"),
    ("callback", 0x6ea0, 0x6ed4, "9e6b69c108f5f1af4178bd069f394176b9675f22e5720791fe13c24e126e6a8f"),
    ("dispatch", 0x6ef0, 0x6ff0, "fd586631b4c4ff1f065091fc0f186c347b8ec40153b707fb222b0b08abc69bd8"),
    ("vector", 0xdc, 0x104, "df4d1d05de497d003188ba999b398feac2c881623b2001d1eac3a90ca125f5ca"),
    ("fiq", 0xaca8, 0xacc0, "33d7831d5e75d2f71c0d2a606324a3c671d9b92f411f88ef37f7f1f998d20cd3"),
    ("connect", 0xacc0, 0xacf4, "ed00d52610e5f39c9326dd13d1979bd4c26ee12d33efcd1d551535e03be9af1f"),
    ("enter", 0x70f0, 0x710c, "634493d235864f16a147dbab7f542737ceb548b4de4b5c9b3061374ad5e39c16"),
    ("leave", 0x710c, 0x7128, "283d962a1f5d6f22c247aacc0d8d4160aef713663d6d8e3d250420ad4b64dc4f"),
    ("mask", 0x2c9d0, 0x2c9e0, "e4d1a1a0d51cd562d39febd6431be6cce67332c2a9880ed1d2ba840d67b5f490"),
    ("unmask", 0x2c9c0, 0x2c9d0, "5c0ec99d0b92780b3ec42f42d11b203cdbe8779d35eec0b4027390642343b00b"),
    ("kick", 0x7898, 0x78dc, "a052564dac821c66bd2dc31e7869422086e7a4d93020b8a29833e5390dff748e"),
    ("producer", 0xe110, 0xe248, "7d82037ed0f6b4d0ab23aace8b9247f7073ff243e6ad8cde6ee53326895e93d2"),
    ("mode", 0xded4, 0xdff4, "a754d48cdd63ac3cc688d6ade94948f174d43525af4cd28b751d4f15dbbe8456"),
    ("choose", 0xdff4, 0xe110, "a326dea03d2672e92d1f5bc959f75dc6bf639ed3da003c8cfa96ad9f1ab5625b"),
)
PICTURE_WORDS = {
    0x6fc: 0xd3a00, 0x7128: 0xd2000, 0x7144: 0x888c, 0x7174: 0xd1ffc, 0xadc8: 0xd2248,
    0x79b0: 0x10502000, 0x7a2c: 0x10540000, 0x8964: 0x10541000, 0x8990: 0xd2210,
    0x7060: 0xe3a00001, 0x7074: 0xe3a00013, 0x6ebc: 0xe7841100,
    0x6fb0: 0xe12fff32, 0x2c9d0: 0xf10c0080, 0x2c9c0: 0xf1080080,
    0x2c9d4: 0xf57ff06f, 0x2c9d8: 0xf57ff04f, 0x2c9dc: 0xe12fff1e,
    0x8778: 0xe5801100, 0x88b8: 0xe5d400d2, 0x88c4: 0xe5d400c4,
    0x88d4: 0xe5d40180, 0x8938: 0xe5950008, 0x893c: 0xe28410e0,
}
PICTURE_CALLS = dict(METADATA_CALLS)
PICTURE_CALLS.update({
    0x8368: 0x70f0, 0x836c: 0x898, 0x83a4: 0x710c, 0x83ac: 0xaf18,
    0x83d8: 0x203c4, 0x83e8: 0x20708, 0x8424: 0x20708, 0x8434: 0x203c4,
    0x8440: 0x203c4, 0x8450: 0x206e4, 0x845c: 0xd624, 0x8470: 0x206e4,
    0x8480: 0xe110, 0x84c8: 0x20708, 0x84dc: 0x1bfc, 0x84f8: 0x82d0,
    0x8518: 0x1f8c, 0x85e8: 0x206e4, 0x85f8: 0xe110, 0x8608: 0x203c4,
    0x8614: 0x82d0, 0x8634: 0x1f8c, 0x8650: 0x78dc, 0x8678: 0xaf18,
    0x8694: 0x7bd4, 0x86a0: 0x7898, 0x86bc: 0x20708, 0x86c8: 0xaf18, 0x86d8: 0x20708,
    0x8710: 0x7bd4, 0x871c: 0x7898, 0x8734: 0x20708, 0x8750: 0x20708,
    0x8768: 0x7bd4, 0x8798: 0xaf18, 0x87a4: 0xaf18, 0x87b0: 0x7898,
    0x87c4: 0x20708, 0x881c: 0x77e0, 0x882c: 0xaf18, 0x8830: 0x710c,
    0x7980: 0x203c4, 0x7990: 0x203c4, 0x8898: 0xaf18, 0x88a0: 0xaf18,
    0x88a4: 0x898, 0x88b4: 0x203c4, 0x88e4: 0xaf18, 0x890c: 0x203c4,
    0x892c: 0x203c4, 0x8934: 0xaf18, 0x8940: 0xd5a4,
})
PICTURE_TAILS = {0x7970: 0x203c4, 0x79a4: 0x203c4}
PICTURE_ROOT, PICTURE_TOKEN = 0xd3a00, 0x710100
PICTURE_PAGES = META_PAGES + (0xd3000, 0xd2000, 0x10502000, 0x10540000, 0x10541000)
MFD_SOURCE_BODIES = (
    (0x1918, 0x1ad8, "78f4221d3656a86e50dce4fcd833856b9b941db1054b75778e760c18affe328f"),
    (0x1e8e8, 0x1e8f4, "127fb56c30f3496c824443348ca74b9236add85c1381d8d0fae5bf61c0a9d927"),
)
MFD_SOURCE_ROWS = ((0, 64, 6), (1, 128, 7), (2, 256, 8))
MFD_SOURCE_WORDS = {0x1b28: 0x2cccc, 0x1b2c: 0x2ce70, 0x1b48: 0x540010,
                    0x1b4c: 0x540028, 0x1b50: 0x54002c, 0x1b6c: 0x54001c, 0x1b70: 0x540020}
MFD_SOURCE_CALLS = {0x1960: 0x203c4, 0x1990: 0x203c4, 0x1a7c: 0x203c4,
                    0x19a0: 0x1e8e8, 0x19b0: 0x1e8e8, 0x19c0: 0x1e8e8,
                    0x1abc: 0x1e8e8, 0x1acc: 0x1e8e8}
MFD_CONTEXT, MFD_RECORD, MFD_STACK = 0x400000, 0x400100, 0x300800
SOURCE_BODIES = (
    (0xe110, 0xe248, "7d82037ed0f6b4d0ab23aace8b9247f7073ff243e6ad8cde6ee53326895e93d2"),
    (0xd880, 0xd92c, "1268e39236294a083ef06179d884a898c15b72addc5aa96c776b60af51de40e9"),
    (0x1fdac, 0x1fe6c, "08d03815210fc1e069068765847cc4c5d847bd5df744f223f9851ee985e05d28"),
)
SOURCE_CALLS = {0xe144: 0xd880, 0xe154: 0xd92c, 0xe168: 0xd9c4,
                0xe178: 0xdcc8, 0xe188: 0xdd4c, 0xe198: 0xdff4,
                0xd8b8: 0x1fdac, 0xd8c8: 0x1fdac}
SOURCE_HELPERS = (0xd92c, 0xd9c4, 0xdcc8, 0xdd4c, 0xdff4)
SOURCE_H, SOURCE_C, SOURCE_META = 0x400000, 0x400600, 0x401100
SOURCE_PICTURE, SOURCE_MAP, SOURCE_STACK = 0x500100, 0x600100, 0x300800
SOURCE_MODE_BODIES = (
    (0xded4, 0xdff4, "a754d48cdd63ac3cc688d6ade94948f174d43525af4cd28b751d4f15dbbe8456"),
    (0xdff4, 0xe110, "a326dea03d2672e92d1f5bc959f75dc6bf639ed3da003c8cfa96ad9f1ab5625b"),
)
SOURCE_MODE_CALLS = {0xe03c: 0xded4, 0xe0ac: 0xded4, 0xe0f0: 0xded4,
                     0xdf7c: 0x203c4, 0xdf98: 0x203c4,
                     0xdfc0: 0x203c4, 0xdfdc: 0x203c4}
SOURCE_MODE_LATCH = 0xd247a


class MetadataRAM(tuple):
    """Immutable trusted CPU page copy, not an authenticated owner/generation."""
    def __new__(cls, pages, nodes):
        if set(pages) != set(META_PAGES) or any(
                type(page) not in (bytes, bytearray) or len(page) != 4096
                for page in pages.values()):
            raise ValueError("invalid metadata replay pages")
        return tuple.__new__(cls, (tuple((base, bytes(pages[base])) for base in META_PAGES),
                                  tuple(tuple(node) for node in nodes)))


def data_member(address):
    return address & 3 == 0 and any(low <= address < high for low, high in (
        (Model.K, Model.K + 0x1b0), (Model.H, Model.H + 0x240),
        (Model.SP - 0x100, Model.SP + 0x34)))


class RSP:
    def __init__(self, connection, deadline):
        self.connection, self.deadline = connection, deadline

    def set_timeout(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("QEMU wall deadline exceeded")
        self.connection.settimeout(min(2, remaining))

    def send(self, data):
        self.set_timeout()
        self.connection.sendall(data)

    def receive(self, count):
        result = bytearray()
        while len(result) < count:
            self.set_timeout()
            part = self.connection.recv(count - len(result))
            if not part:
                raise ValueError("QEMU RSP closed")
            result.extend(part)
        return bytes(result)

    def packet(self, command):
        body = command.encode("ascii")
        self.send(b"$" + body + b"#" + f"{sum(body) & 255:02x}".encode())
        if self.receive(1) != b"+" or self.receive(1) != b"$":
            raise ValueError("unexpected RSP acknowledgement")
        reply = bytearray()
        while True:
            char = self.receive(1)
            if char == b"#":
                break
            if len(reply) >= 16384:
                raise ValueError("oversized RSP reply")
            reply.extend(char)
        checksum = self.receive(2)
        if checksum != f"{sum(reply) & 255:02x}".encode():
            raise ValueError("bad RSP checksum")
        self.send(b"+")
        decoded, escaped = bytearray(), False
        for char in reply:
            if escaped:
                decoded.append(char ^ 0x20)
                escaped = False
            elif char == 0x7d:
                escaped = True
            else:
                decoded.append(char)
        if escaped:
            raise ValueError("truncated RSP escape")
        return decoded.decode("ascii")

    def xml(self, name):
        result = ""
        while len(result) < 16384:
            reply = self.packet(f"qXfer:features:read:{name}:{len(result):x},1000")
            if not reply or reply[0] not in "lm" or (reply[0] == "m" and len(reply) == 1):
                raise ValueError("missing QEMU register XML")
            result += reply[1:]
            if reply[0] == "l":
                # GDB's target.dtd defines xi:include without an XML namespace.
                return ET.fromstring(result.replace("xi:include", "include"))
        raise ValueError("oversized QEMU register XML")

    def register(self, index, value=None):
        if value is None:
            reply = self.packet(f"p{index:x}")
            if len(reply) != 8:
                raise ValueError("invalid QEMU register reply")
            return struct.unpack("<I", bytes.fromhex(reply))[0]
        encoded = struct.pack("<I", value & 0xffffffff).hex()
        if self.packet(f"P{index:x}={encoded}") != "OK":
            raise ValueError("QEMU register write refused")

    def registers(self):
        reply = self.packet("g")
        if len(reply) < 128:
            raise ValueError("short QEMU core register reply")
        return list(struct.unpack("<16I", bytes.fromhex(reply[:128])))

    def memory(self, address, data=None, size=4):
        if data is None:
            if size > 512:
                return b"".join(self.memory(address + offset, size=min(512, size - offset))
                                for offset in range(0, size, 512))
            reply = self.packet(f"m{address:x},{size:x}")
            if len(reply) != size * 2:
                raise ValueError(f"QEMU memory read refused: size={size} reply={reply[:80]!r} length={len(reply)}")
            return bytes.fromhex(reply)
        if self.packet(f"M{address:x},{len(data):x}:{data.hex()}") != "OK":
            raise ValueError("QEMU memory write refused")


def initial(picture="common", split="common", generic="private", sizes=None,
            aliases=None, base=0x800000, fail=None, translation="write"):
    pages = {base: bytearray(b"\xa5" * 4096) for base in (Model.K, Model.H, 0x300000)}
    def store(address, value):
        base = address & ~4095
        struct.pack_into("<I", pages[base], address - base, value & 0xffffffff)
    fields = {0x0c: 0x100, 0x14: 0x80, 0x1c: 3, 0x20: 0x40,
              0x28: 0x200, 0x30: 0x300, 0x38: 0x60, 0x40: 0x70}
    fields.update(sizes or {})
    if set(fields) != {0x0c, 0x14, 0x1c, 0x20, 0x28, 0x30, 0x38, 0x40} or any(
            type(value) is not int or not 0 <= value <= 0xffffffff for value in fields.values()):
        raise ValueError("invalid synthetic spans")
    if type(base) is not int or not 0 <= base <= 0xffffffff:
        raise ValueError("invalid synthetic allocation base")
    if fail is not None and (type(fail) is not int or not 0 <= fail < 3):
        raise ValueError("invalid synthetic allocation failure")
    if translation not in ("write", "error", "zero", "omit"):
        raise ValueError("invalid synthetic translation")
    for offset, value in fields.items():
        store(Model.H + offset, value)
    for field, offset, mode in ((0xd4, 0x1a8, picture), (0xd0, 0x1a4, split)):
        if mode not in ("common", "private", "factory"):
            raise ValueError("invalid provider mode")
        store(Model.H + field, field if mode == "private" else 0)
        store(Model.K + offset, offset if mode == "factory" else 0)
    if generic not in ("private", "factory"):
        raise ValueError("invalid generic provider")
    store(Model.H + 0xcc, 0xcc if generic == "private" else 0)
    for offset in (8, 0x10, 0x14, 0x1a0):
        store(Model.K + offset, offset + 0x1000)
    destinations = {offset: Model.H + offset for offset in Model.OUTPUTS}
    destinations.update(aliases or {})
    if set(destinations) != set(Model.OUTPUTS) or any(
            type(address) is not int or (not data_member(address) and address != 0x400000)
            for address in destinations.values()):
        raise ValueError("invalid synthetic output destination")
    stack = [fields[offset] for offset in (0x1c, 0x20, 0x28, 0x30, 0x38, 0x40)]
    stack += [destinations[offset] for offset in Model.OUTPUTS]
    for index, value in enumerate(stack):
        store(Model.SP + index * 4, value)
    registers = [0xabc00000 + index for index in range(16)]
    registers[:4] = [Model.K, Model.H, fields[0x0c], fields[0x14]]
    registers[13:16] = [Model.SP, RETURN, Model.START]
    return pages, registers


def elf(payload, pages, start=Model.START, end=Model.END,
        digest=Model.DIGEST, literals=()):
    body = payload[start:end]
    if hashlib.sha256(body).hexdigest() != digest:
        raise ValueError("stock allocation body changed" if start == Model.START else
                         "stock OPEN body changed")
    code = bytearray(struct.pack("<I", 0xe7f000f0) * (0x20000 // 4))
    code[start - RETURN:end - RETURN] = body
    for address, expected in literals:
        if payload[address:address + len(expected)] != expected:
            raise ValueError("stock OPEN literal changed")
        code[address - RETURN:address - RETURN + len(expected)] = expected
    segments = [(RETURN, bytes(code), 5)] + [(base, bytes(page), 6) for base, page in pages.items()]
    return segment_elf(segments, start)


def segment_elf(segments, start):
    image = bytearray(4096)
    image[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", image, 16, 2, 40, 1, start, 52,
                     0, 0x05000000, 52, 32, len(segments), 0, 0, 0)
    for index, (address, data, flags) in enumerate(segments):
        offset = len(image)
        struct.pack_into("<8I", image, 52 + index * 32, 1, offset, address,
                         address, len(data), len(data), flags, 4096)
        image.extend(data)
    return image


def execute(options=None, budget=512, expected_signal=None):
    options = options or {}
    if type(budget) is not int or not 1 <= budget <= 512:
        raise ValueError("invalid instruction budget")
    pages, registers = initial(**options)
    image = elf(Model.payload, pages)
    allocations = {}
    def callee(rsp, pc, args, stack):
        result = 0
        if pc in CALLEES[:2]:
            slot = len(allocations)
            pointer = 0x900000 + slot * 0x1000
            allocations[pointer] = (options.get("base", 0x800000) + slot * 0x10000) & 0xffffffff
            result = 0 if slot == options.get("fail") else pointer
        elif pc == 0x1fe6c:
            if args[1] not in allocations:
                raise ValueError("translation of unknown synthetic allocation")
            if not data_member(args[2]):
                raise ValueError("translation outside admitted synthetic data")
            translation = options.get("translation", "write")
            if translation != "omit":
                value = 0 if translation == "zero" else allocations[args[1]]
                rsp.memory(args[2], struct.pack("<I", value))
            result = 4 if translation in ("error", "omit") else 0
        return result
    actual = emulate(image, pages, registers, ((Model.START, 0x26040), (0x26134, Model.END)),
                     CALLEES, callee, budget, expected_signal)
    if "pages" in actual:
        actual["outputs"] = {offset: struct.unpack_from("<I", actual["pages"][Model.H], offset)[0]
                             for offset in Model.OUTPUTS}
    return actual


def emulate(image, pages, registers, slices, callees, callee, budget,
            expected_signal=None, clobber_flags=0, real_callees=(), call_edges=None,
            tail_edges=None, instruction=None):
    """Single-step only admitted stock slices; synthetic callees never execute."""
    if tail_edges is not None and (type(tail_edges) is not dict or
            any(type(site) is not int or type(target) is not int or
                (site, target) not in ((0x7970, 0x203c4), (0x79a4, 0x203c4)) or target not in callees
                for site, target in tail_edges.items()) or 0x78dc not in real_callees):
        raise ValueError("invalid pinned tail edges")
    qemu = shutil.which(os.environ.get("QEMU_ARM", "qemu-arm"))
    if not qemu:
        raise ValueError("fw-qemu-check requires qemu-arm (or QEMU_ARM=/path/to/qemu-arm)")
    with tempfile.TemporaryDirectory(prefix="chd-qemu-", dir="/tmp") as temporary:
        directory = Path(temporary)
        program, endpoint = directory / "stock.elf", directory / "gdb.sock"
        program.write_bytes(image)
        program.chmod(0o700)
        process = subprocess.Popen([qemu, "-cpu", "cortex-a8", "-g", str(endpoint), str(program)],
                                   env={"PATH": os.defpath, "LC_ALL": "C"},
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.PIPE, close_fds=True)
        deadline = time.monotonic() + 15
        def stop_process():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)
        try:
            with ExitStack() as cleanup:
                connection = cleanup.enter_context(socket.socket(socket.AF_UNIX))
                # A disconnected QEMU debugger resumes the guest: reap it first.
                cleanup.callback(stop_process)
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("QEMU debugger startup deadline exceeded")
                    connection.settimeout(min(2, remaining))
                    try:
                        connection.connect(str(endpoint))
                        break
                    except (FileNotFoundError, ConnectionRefusedError):
                        if process.poll() is not None:
                            raise ValueError("QEMU did not start its private debugger: " +
                                             process.stderr.read(2048).decode("ascii", errors="replace"))
                        if time.monotonic() >= deadline:
                            raise ValueError("QEMU debugger startup deadline exceeded")
                        time.sleep(0.01)
                rsp = RSP(connection, deadline)
                supported = rsp.packet("qSupported:xmlRegisters=arm")
                if "qXfer:features:read+" not in supported:
                    raise ValueError("QEMU lacks register XML")
                target = rsp.xml("target.xml")
                if not any(node.attrib.get("href") == "arm-core.xml" for node in target):
                    raise ValueError("unexpected QEMU core XML")
                core = rsp.xml("arm-core.xml")
                core_regs = [node for node in core if node.tag == "reg"]
                if [node.attrib.get("name") for node in core_regs[:16]] != (
                        [f"r{index}" for index in range(13)] + ["sp", "lr", "pc"]) or any(
                        node.attrib.get("bitsize") != "32" or
                        int(node.attrib.get("regnum", index)) != index
                        for index, node in enumerate(core_regs[:16])):
                    raise ValueError("unexpected QEMU core register order")
                cpsr = next(node for node in core if node.attrib.get("name") == "cpsr")
                if cpsr.attrib.get("regnum") != "25" or cpsr.attrib.get("bitsize") != "32":
                    raise ValueError("unexpected QEMU CPSR register")
                # Preserve the user-mode control bits; all execution stays in A32.
                flags = rsp.register(25) & 0x0fffffff & ~0x20
                if flags & 0x1f != 0x10:
                    raise ValueError("QEMU is not in ARM user mode")
                rsp.register(25, flags)
                for index, value in enumerate(registers):
                    rsp.register(index, value)
                preserved, entry_stack = registers[4:12], registers[13]
                calls, stub_status, real_status, real_returns = [], [], [], []
                steps, last_pc = 0, None
                while True:
                    registers = rsp.registers()
                    pc = registers[15]
                    if real_returns and pc == real_returns[-1][0]:
                        _, target = real_returns.pop()
                        real_status.append((target, registers[0]))
                    if pc == RETURN:
                        if expected_signal is not None:
                            raise ValueError("expected guest signal did not occur")
                        if real_returns or registers[4:12] != preserved or registers[13] != entry_stack:
                            raise ValueError("stock callee-saved ABI changed")
                        observed = {base: rsp.memory(base, size=4096) for base in pages}
                        return dict(status=registers[0], calls=calls,
                                    pages=observed, initial=pages, steps=steps,
                                    stub_status=stub_status, real_status=real_status)
                    if pc in callees or pc in real_callees:
                        tail = tail_edges is not None and tail_edges.get(last_pc) == pc
                        if tail and (not real_returns or real_returns[-1][1] != 0x78dc or
                                     registers[14] != real_returns[-1][0]):
                            raise ValueError("unexpected pinned tail LR")
                        if last_pc is None or (not tail and registers[14] != last_pc + 4):
                            raise ValueError("unexpected pinned call ABI")
                        if call_edges is not None and not tail and call_edges.get(last_pc) != pc:
                            raise ValueError("outside pinned call-target allowlist")
                        args = tuple(registers[:4])
                        calls.append((last_pc, pc, args))
                        if pc in real_callees:
                            real_returns.append((registers[14], pc))
                        else:
                            result = callee(rsp, pc, args, registers[13])
                            stub_status.append((pc, result))
                            for index, value in ((0, result), (1, 0xd1d1d1d1), (2, 0xd2d2d2d2),
                                                 (3, 0xd3d3d3d3), (12, 0xdcdcdcdc), (15, registers[14])):
                                rsp.register(index, value)
                            rsp.register(25, flags | clobber_flags)
                            last_pc = None
                            continue
                    if pc & 3 or not any(low <= pc < high for low, high in slices):
                        raise ValueError("outside pinned executable slices")
                    if steps >= budget:
                        raise ValueError("stock instruction budget exceeded")
                    if instruction is not None:
                        instruction(rsp, pc, registers)
                    stop = rsp.packet("s")
                    steps += 1
                    if not stop.startswith(("S05", "T05")):
                        if expected_signal is not None and stop.startswith(f"T{expected_signal:02x}"):
                            return dict(signal=expected_signal, pc=pc, steps=steps)
                        raise ValueError(f"unexpected guest stop: {stop}")
                    last_pc = pc
        finally:
            stop_process()
            process.stderr.close()


def execute_open(mode=1, algorithm=10, arguments=None, transport=0,
                 translation="write", budget=128, payload=None):
    """Exact OPEN builder; clear, transport and translation are synthetic only."""
    arguments = tuple(arguments) if arguments is not None else tuple(
        0x41000000 + index * 0x10101 for index in range(15))
    if len(arguments) != 15 or any(type(value) is not int or not 0 <= value <= 0xffffffff
                                  for value in (*arguments, mode, algorithm, transport)):
        raise ValueError("invalid synthetic OPEN arguments")
    if translation not in ("write", "error", "omit"):
        raise ValueError("invalid synthetic OPEN translation")
    if type(budget) is not int or not 1 <= budget <= 512:
        raise ValueError("invalid instruction budget")
    pages = {base: bytearray(b"\xa5" * 4096) for base in (Model.K, Model.H, 0x300000)}
    def store(address, value):
        struct.pack_into("<I", pages[address & ~4095], address & 4095, value)
    store(Model.K + 8, 0x11223344)
    store(Model.K + 0x64, 0x55667788)
    store(Model.H, 0x12345678)
    for index, value in enumerate(arguments):
        store(OPEN_SP + index * 4, value)
    registers = [0xabc00000 + index for index in range(16)]
    registers[:4] = [Model.K, Model.H, mode, algorithm]
    registers[13:16] = [OPEN_SP, RETURN, OPEN_START]
    image = elf(Model.payload if payload is None else payload, pages,
                OPEN_START, OPEN_END, OPEN_DIGEST, (OPEN_LITERAL,))
    # The 552-byte frame and incoming 60 bytes fit this mapped stack page.
    frame, request, response = OPEN_SP - 552, OPEN_SP - 288, OPEN_SP - 540
    reply = struct.pack("<63I", *(0x82000000 + index * 0x101 for index in range(63)))
    captured, sequence = {}, []
    def callee(rsp, pc, args, stack):
        ordinal = len(sequence)
        sequence.append(pc)
        if stack != frame:
            raise ValueError("unexpected synthetic OPEN stack")
        if ordinal < 2:
            destination = request if ordinal == 0 else response
            if pc != 0x206e4 or args[:3] != (destination, 0, 252):
                raise ValueError("unexpected synthetic OPEN clear")
            rsp.memory(destination, bytes(252))
            return destination
        if ordinal == 2:
            if pc != 0x2705c or args != (Model.K, 0x55667788, request, response) or \
                    rsp.memory(frame) != struct.pack("<I", 20000):
                raise ValueError("unexpected synthetic OPEN transport")
            # Capture the complete packet before any opaque transport writes.
            captured["packet"] = rsp.memory(request, size=252)
            captured["response_before"] = rsp.memory(response, size=252)
            rsp.memory(response, reply)
            return transport
        if ordinal != 3 or pc != 0x1fdac or args[:3] != (
                0x11223344, struct.unpack_from("<I", reply, 20)[0], Model.H + 0x58):
            raise ValueError("unexpected synthetic OPEN translation")
        if translation != "omit":
            rsp.memory(Model.H + 0x58, struct.pack("<I", 0x33445566))
        return 4 if translation != "write" else 0
    actual = emulate(image, pages, registers, ((OPEN_START, OPEN_END),),
                     (0x206e4, 0x2705c, 0x1fdac), callee, budget,
                     clobber_flags=0xf0000000)
    actual.update(captured, reply=reply, arguments=arguments, registers=registers,
                  frame=frame, request=request, response=response)
    return actual


def metadata_elf(payload, pages, start):
    """Only four complete stock bodies and four data strings enter the guest."""
    code_base = 0x8000
    code = bytearray(struct.pack("<I", 0xe7f000f0) * (0x20000 // 4))
    for name, low, high, digest in METADATA_BODIES:
        body = payload[low:high]
        if hashlib.sha256(body).hexdigest() != digest:
            raise ValueError(f"stock metadata {name} body changed")
        code[low - code_base:high - code_base] = body
    for address, expected in METADATA_LITERALS:
        if payload[address:address + len(expected)] != expected:
            raise ValueError("stock metadata literal changed")
        code[address - code_base:address - code_base + len(expected)] = expected
    segments = [(code_base, bytes(code), 5)]
    segments.extend((base, bytes(page), 6) for base, page in pages.items())
    return segment_elf(segments, start)


def execute_metadata(kind, read=None, write=None, null_ring=False, null_output=False,
                     translation="direct", token=0x710030, logger_status=0,
                     budget=256, payload=None, expected_signal=None, previous=None):
    """Stock metadata tokens only: no record/plane payload is dereferenced.

    Queue, map nodes, output pair and stack are disjoint bounded host RAM.
    Invalid indices have deliberately mapped header/adjacent-word targets.
    Only logging is synthetic; the actual stock translation helper executes.
    """
    if previous is not None and (type(previous) is not MetadataRAM or kind != "release" or
                                 read is not None or write is not None or null_ring or null_output or
                                 translation != "direct" or token != 0x710030):
        raise ValueError("invalid metadata replay arguments")
    read, write = 2 if read is None else read, 3 if write is None else write
    indices = set(range(2, 64)) | {0, 1, 64, 0xffffffff}
    if kind not in {name for name, _, _, _ in METADATA_BODIES} or \
            type(read) is not int or read not in indices or \
            type(write) is not int or write not in indices or \
            type(null_ring) is not bool or type(null_output) is not bool or \
            type(token) is not int or not 0 <= token <= 0xffffffff or \
            type(logger_status) is not int or not 0 <= logger_status <= 0xffffffff or \
            translation not in ("direct", "linked", "linked_failure", "wrap"):
        raise ValueError("invalid synthetic metadata arguments")
    if type(budget) is not int or not 1 <= budget <= 512:
        raise ValueError("invalid instruction budget")
    pages = {base: bytearray(b"\xa5" * 4096) for base in META_PAGES}
    def word(address, value):
        struct.pack_into("<I", pages[address & ~4095], address & 4095, value & 0xffffffff)
    word(Model.H + 0x224, META_MAP)
    word(Model.H + 0x250, 0 if null_ring and kind != "release" else META_DEL)
    word(Model.H + 0x254, 0 if null_ring and kind == "release" else META_REL)
    for ring in (META_DEL, META_REL):
        word(ring, read)
        word(ring + 4, write)
        for index in range(2, 64):
            word(ring + index * 4, 0x730000 + index * 0x101)
    # A malformed index0/1 really reads a header, not a fabricated data slot.
    if read not in (0, 1):
        word((META_DEL + read * 4) & 0xffffffff, token)
    word(META_RECORD, 0x44440000)
    word(META_RECORD + 4, token)
    nodes = [(META_MAP, 0x810000, 0x710000, 0x810000, 0x8100ff)]
    if translation.startswith("linked"):
        nodes = [(META_MAP, 0x820000, 0x710000, 0x820100, 0x8201ff),
                 (META_MAP + 0x100, 0x830000, 0x710000,
                  0x830100 if translation == "linked_failure" else 0x830000, 0x8301ff)]
        if translation == "linked_failure":
            nodes.append((META_MAP + 0x200, 0x840000, 0x710000, 0x840100, 0x8401ff))
    elif translation == "wrap":
        nodes = [(META_MAP, 0xfffffff0, 0, 0, 0xff)]
    for index, (address, virtual, physical, low, high) in enumerate(nodes):
        word(address, nodes[index + 1][0] if index + 1 < len(nodes) else 0)
        word(address + 4, META_HEAD if len(nodes) > 1 else 0)
        for offset, value in ((0x18, low), (0x1c, high), (0x28, virtual), (0x30, physical)):
            word(address + offset, value)
    word(META_HEAD, META_MAP)
    if previous is not None:
        # Do not reinitialize a single byte of the previous observed context,
        # ring, mapping, output pair or stack. Only caller registers are fresh.
        pages = {base: bytearray(page) for base, page in previous[0]}
        if set(pages) != set(META_PAGES) or any(len(page) != 4096 for page in pages.values()):
            raise ValueError("invalid metadata replay pages")
        def load(address):
            return struct.unpack_from("<I", pages[address & ~4095], address & 4095)[0]
        if any(load(Model.H + offset) != value
               for offset, value in ((0x224, META_MAP), (0x250, META_DEL), (0x254, META_REL))) or \
                any(not 2 <= load(ring + offset) < 64
                    for ring in (META_DEL, META_REL) for offset in (0, 4)):
            raise ValueError("invalid metadata replay context")
        read, write, token = load(META_REL), load(META_REL + 4), load(META_RECORD + 4)
        nodes = list(previous[1])
    registers = [0xabc00000 + index for index in range(16)]
    registers[:2] = [Model.H, 0 if null_output else META_RECORD]
    if kind == "translate":
        registers[:3] = [META_MAP, token, 0 if null_output else META_RECORD]
    start = next(low for name, low, _, _ in METADATA_BODIES if name == kind)
    registers[13:16] = [META_SP, RETURN, start]
    image = metadata_elf(Model.payload if payload is None else payload, pages, start)
    publication = []
    def log(rsp, pc, args, stack):
        if pc != 0x203c4:
            raise ValueError("unexpected metadata logging target")
        publication.append(dict(args=args, stack=stack,
                                delivery=rsp.memory(META_DEL, size=256),
                                release=rsp.memory(META_REL, size=256),
                                output=rsp.memory(META_RECORD, size=8)))
        return logger_status
    actual = emulate(image, pages, registers,
                     tuple((low, high) for _, low, high, _ in METADATA_BODIES),
                     (0x203c4,), log, budget, expected_signal,
                     clobber_flags=0xf0000000,
                     real_callees=(0x1fdac,) if kind != "translate" else (),
                     call_edges=METADATA_CALLS)
    actual.update(kind=kind, read=read, write=write, null_ring=null_ring,
                  null_output=null_output, token=token, nodes=nodes,
                  registers=registers, publication=publication)
    if "pages" in actual:
        actual["snapshot"] = MetadataRAM(actual["pages"], nodes)
    return actual


class PictureRAM(tuple):
    """Detached synthetic slot-0 RAM, not native ownership or a generation."""
    def __new__(cls, pages, picture):
        if set(pages) != set(PICTURE_PAGES) or any(
                type(page) not in (bytes, bytearray) or len(page) != 4096
                for page in pages.values()) or type(picture) not in (bytes, bytearray) or len(picture) != 140:
            raise ValueError("invalid picture replay pages")
        return tuple.__new__(cls, (tuple((base, bytes(pages[base])) for base in PICTURE_PAGES), bytes(picture)))


def picture_elf(payload, pages, start):
    """Complete pinned scheduler/IRQ/format and metadata bodies only execute."""
    code_base = 0x7000
    code = bytearray(struct.pack("<I", 0xe7f000f0) * (0x21000 // 4))
    for name, low, high, digest in PICTURE_BODIES + METADATA_BODIES + PICTURE_GUARDS:
        body = payload[low:high]
        if hashlib.sha256(body).hexdigest() != digest:
            raise ValueError(f"stock picture {name} body changed")
        if (name, low, high, digest) not in PICTURE_GUARDS:
            code[low - code_base:high - code_base] = body
    for address, value in PICTURE_WORDS.items():
        if payload[address:address + 4] != struct.pack("<I", value):
            raise ValueError("stock picture control/literal changed")
        if code_base <= address < code_base + len(code):
            code[address - code_base:address - code_base + 4] = struct.pack("<I", value)
    for site, target in {**PICTURE_CALLS, **PICTURE_TAILS, 0xf0: 0x6ef0,
                         0x7064: 0xacc0, 0x7078: 0x6ea0, 0x70f4: 0x2c9d0, 0x7120: 0x2c9c0}.items():
        word = struct.unpack_from("<I", payload, site)[0]
        displacement = (word & 0xffffff) << 2
        if displacement & 0x2000000:
            displacement -= 0x4000000
        if word & 0xff000000 != (0xea000000 if site in (*PICTURE_TAILS, 0x7120) else 0xeb000000) or \
                (site + 8 + displacement) & 0xffffffff != target:
            raise ValueError("stock picture call target changed")
    for address, expected in METADATA_LITERALS:
        if payload[address:address + len(expected)] != expected:
            raise ValueError("stock picture metadata literal changed")
        code[address - code_base:address - code_base + len(expected)] = expected
    return segment_elf([(code_base, bytes(code), 5)] +
                       [(base, bytes(page), 6) for base, page in pages.items()], start)


def execute_picture(kind="picture", mode=2, single_field=False, active=1, started=1,
                    configured=False, cached_physical=True, metadata_flags=0,
                    refresh=False, phase=0, route=0, empty=False, format_change=False,
                    previous=None, budget=512, payload=None):
    """Actual slot-0 instructions with bounded, explicitly opaque CPU contracts.

    E110 supplies a synthetic 140-byte picture; MFD setup/build/kick and RX-list
    publication are contracts, not engines. CPSID/CPSIE are separately pinned:
    enter/leave events do not emulate IRQ/FIQ exclusion or barrier visibility.
    IRQ is invoked only after a scheduler return, never asynchronously. Fixed
    MMIO-numbered pages are ordinary guest RAM. No plane scalar is dereferenced.
    Strict input/window admission is a replay guard, not a native validation.
    """
    defaults = (2, False, 1, 1, False, True, 0, False, 0, 0, False, False)
    options = (mode, single_field, active, started, configured, cached_physical,
               metadata_flags, refresh, phase, route, empty, format_change)
    if type(kind) is not str or kind not in ("picture", "irq") or type(mode) is not int or mode not in (0, 1, 2) or \
            any(type(value) is not bool for value in
                (single_field, configured, cached_physical, refresh, empty, format_change)) or \
            any(type(value) is not int or not 0 <= value <= 255 for value in (active, started)) or \
            any(type(value) is not int or value not in (0, 1) for value in (phase, route)) or \
            type(metadata_flags) is not int or metadata_flags not in (0, 0x100) or \
            (previous is not None and (type(previous) is not PictureRAM or options != defaults)):
        raise ValueError("invalid synthetic picture arguments")
    if type(budget) is not int or not 1 <= budget <= 512:
        raise ValueError("invalid instruction budget")
    payload = Model.payload if payload is None else payload
    if type(payload) not in (bytes, bytearray):
        raise ValueError("invalid picture payload")
    pages = {base: bytearray(b"\xa5" * 4096) for base in PICTURE_PAGES}
    def put(address, data, destination=pages):
        base, offset = address & ~4095, address & 4095
        if base not in destination or offset + len(data) > 4096:
            raise ValueError("outside picture RAM")
        destination[base][offset:offset + len(data)] = data
    def word(address, value, destination=pages):
        put(address, struct.pack("<I", value), destination)
    def get(address, size=4, source=pages):
        base, offset = address & ~4095, address & 4095
        if base not in source or offset + size > 4096:
            raise ValueError("outside picture RAM")
        return bytes(source[base][offset:offset + size])
    def load(address, source=pages):
        return struct.unpack("<I", get(address, source=source))[0]
    picture = bytearray((index * 17 + 3) & 255 for index in range(140))
    picture[8], picture[0x1d] = mode, 1
    for offset, value in ((0x14, 256), (0x18, 96), (0x34, 0x920000), (0x38, 0x930000)):
        struct.pack_into("<I", picture, offset, value)
    for offset, value in ((0x224, META_MAP), (0x250, META_DEL), (0x254, META_REL)):
        word(Model.H + offset, value)
    for ring in (META_DEL, META_REL):
        word(ring, 2)
        word(ring + 4, 2 if ring == META_REL or empty else 3)
        word(ring + 8, PICTURE_TOKEN)
    word(META_MAP, 0)
    for offset, value in ((0x18, 0x500000), (0x1c, 0x500fff), (0x28, 0x500000), (0x30, 0x710000)):
        word(META_MAP + offset, value)
    word(META_RECORD, metadata_flags)
    word(PICTURE_ROOT + 0x20, Model.H)
    for offset, value in ((0xc4, active), (0xd2, started), (0xc5, int(configured)),
                          (0x180, route), (0x181, phase), (0x178, int(refresh)),
                          (0x1cc, 0), (0x1cd, int(single_field))):
        put(PICTURE_ROOT + offset, bytes([value]))
    put(PICTURE_ROOT + 0xe0, struct.pack("<3I", META_RECORD,
                                      PICTURE_TOKEN if cached_physical else 0, 0xdec0adde))
    put(PICTURE_ROOT + 0xec, picture)
    if format_change:
        word(PICTURE_ROOT + 0x100, 128)
    if previous is not None:
        pages.clear()
        pages.update((base, bytearray(page)) for base, page in previous[0])
        picture = bytearray(previous[1])
    if any(load(Model.H + offset) != value for offset, value in
           ((0x224, META_MAP), (0x250, META_DEL), (0x254, META_REL))) or \
            load(PICTURE_ROOT + 0x20) != Model.H or \
            any(not 2 <= load(ring + offset) < 64 for ring in (META_DEL, META_REL) for offset in (0, 4)) or \
            (load(META_DEL) != load(META_DEL + 4) and
             load(META_DEL + load(META_DEL) * 4) != PICTURE_TOKEN) or \
            any(load(META_MAP + offset) != value for offset, value in
                ((0, 0), (0x18, 0x500000), (0x1c, 0x500fff), (0x28, 0x500000), (0x30, 0x710000))) or \
            load(META_RECORD) not in (0, 0x100) or \
            load(PICTURE_ROOT + 0xe0) != META_RECORD or \
            load(PICTURE_ROOT + 0xe4) not in (0, PICTURE_TOKEN) or \
            any(get(PICTURE_ROOT + offset, 1)[0] not in (0, 1)
                for offset in (0xc5, 0x178, 0x180, 0x181, 0x1cd)) or \
            picture[8] not in (0, 1, 2) or len(picture) != 140 or picture[0x1d] != 1 or \
            struct.unpack_from("<2I", picture, 0x14) != (256, 96) or \
            struct.unpack_from("<2I", picture, 0x34) != (0x920000, 0x930000):
        raise ValueError("invalid picture replay context")
    start = 0x834c if kind == "picture" else 0x888c
    image = picture_elf(payload, pages, start)
    expected = {base: bytearray(page) for base, page in pages.items()}
    registers = [0xabc00000 + index for index in range(16)]
    registers[:2] = [0, 0x100]
    registers[13:16] = [META_SP, RETURN, start]
    frame = META_SP - 200
    events, publication, depth = [], [], 0
    pushes = {0x834c: (4, 5, 6, 7, 8, 9, 14), 0x78dc: (4, 5, 6, 14),
              0xd624: (4, 5, 6, 7, 8, 14), 0x1fdac: (4, 5, 14),
              0x888c: (4, 5, 6, 14), 0xd5a4: (4, 5, 6, 7, 8, 9, 10, 14)}
    def instruction(rsp, pc, regs):
        if pc in pushes:
            values = [regs[index] for index in pushes[pc]]
            put(regs[13] - len(values) * 4, struct.pack("<" + "I" * len(values), *values), expected)
        if pc in (0x835c, 0x8360):
            word(frame + (0x10 if pc == 0x835c else 0x0c), 0, expected)
        if pc in (0x850c, 0x8628):
            put(frame, struct.pack("<2I", frame + 0x10, frame + 0x0c), expected)
        if pc in (0x8688, 0x8708, 0x8764):
            word(frame, 1 if pc == 0x8688 else 0, expected)
        if pc == 0xd624:
            if tuple(regs[:2]) != (Model.H, frame + 0x14):
                raise ValueError("unexpected picture acquire")
            if load(META_DEL) != load(META_DEL + 4):
                put(frame + 0x14, struct.pack("<2I", META_RECORD, PICTURE_TOKEN), expected)
                read = load(META_DEL)
                word(META_DEL, 2 if read == 63 else read + 1, expected)
        if pc == 0xd5a4:
            if tuple(regs[:2]) != (Model.H, PICTURE_ROOT + 0xe0):
                raise ValueError("unexpected picture release")
            write = load(META_REL + 4)
            word(META_REL + write * 4, load(PICTURE_ROOT + 0xe4), expected)
            word(META_REL + 4, 2 if write == 63 else write + 1, expected)
    def contract(rsp, pc, args, stack):
        nonlocal depth
        if pc == 0x70f0:
            if depth != 0 or kind != "picture":
                raise ValueError("unexpected critical enter")
            depth = 1
            events.append(("enter",))
        elif pc == 0x710c:
            if depth != 1:
                raise ValueError("unexpected critical leave")
            depth = 0
            events.append(("leave", rsp.memory(PICTURE_ROOT + 0xe0, size=12),
                           rsp.memory(PICTURE_ROOT + 0x180, size=2)))
        elif pc == 0x898:
            return PICTURE_ROOT
        elif pc in (0x206e4, 0x20708):
            destination, source, size = args[:3]
            pairs = {(frame + 0x14, PICTURE_ROOT + 0xe0, 12),
                     (frame + 0x20, PICTURE_ROOT + 0xec, 140),
                     (PICTURE_ROOT + 0xec, frame + 0x20, 140),
                     (PICTURE_ROOT + 0xe0, frame + 0x14, 12)}
            if pc == 0x206e4:
                if source != 0 or (destination, size) not in ((frame + 0x14, 12), (frame + 0x20, 140)):
                    raise ValueError("unexpected picture clear")
                data = bytes(size)
            else:
                if (destination, source, size) not in pairs:
                    raise ValueError("unexpected picture copy")
                data = get(source, size, expected)
                if rsp.memory(source, size=size) != data:
                    raise ValueError("unexpected picture copy source")
            rsp.memory(destination, data)
            put(destination, data, expected)
            return destination
        elif pc == 0xe110:
            if args[:3] not in ((Model.H, META_RECORD, frame + 0x20), (Model.H, 0, frame + 0x20)):
                raise ValueError("unexpected picture producer")
            rsp.memory(frame + 0x20, picture)
            put(frame + 0x20, picture, expected)
            if args[1] == 0:
                put(frame + 0x28, b"\x02", expected)
                word(frame + 0x34, 1920, expected)
                word(frame + 0x38, 1080, expected)
        elif pc == 0x1bfc:
            if args != (PICTURE_ROOT, 0, frame + 0x14, frame + 0x20):
                raise ValueError("unexpected picture setup")
        elif pc == 0x82d0:
            p = get(frame + 0x20, 140, expected)
            width, height = struct.unpack_from("<2I", p, 0x14)
            if args[:2] != (width, height if p[8] == 2 else height // 2):
                raise ValueError("unexpected picture dimensions")
        elif pc == 0x1f8c:
            if args != (PICTURE_ROOT, frame + 0x14, frame + 0x20, 0) or \
                    rsp.memory(stack, size=8) != struct.pack("<2I", frame + 0x10, frame + 0x0c):
                raise ValueError("unexpected picture pipeline")
        elif pc == 0x7bd4:
            if args[:3] != (frame + 0x14, frame + 0x20, 0) or args[3] not in (0, 1):
                raise ValueError("unexpected picture builder")
            events.append(("build", args[3], load(frame, expected), get(frame + 0x14, 12, expected),
                           get(frame + 0x20, 140, expected)))
        elif pc == 0x7898:
            if depth != 1 or args[:2] not in ((0, 0), (1, 0)):
                raise ValueError("kick outside synthetic critical region")
            events.append(("kick", args[0], rsp.memory(PICTURE_ROOT + 0xe0, size=12),
                           rsp.memory(PICTURE_ROOT + 0x180, size=2)))
        elif pc == 0x77e0:
            if args[0] != PICTURE_ROOT + 0x188 or depth != 1:
                raise ValueError("unexpected picture RX publication")
            events.append(("rx-publish",))
        elif pc == 0x203c4:
            if args[0] == 0xd7f8:
                write = load(META_REL + 4)
                if rsp.memory(META_REL + 4) != struct.pack("<I", write) or \
                        rsp.memory(META_REL + write * 4) != get(PICTURE_ROOT + 0xe4):
                    raise ValueError("release payload/index ordering changed")
            publication.append((args, rsp.memory(META_DEL, size=256), rsp.memory(META_REL, size=256)))
        elif pc != 0xaf18:
            raise ValueError("unexpected picture opaque helper")
        return 0
    active, started = (get(PICTURE_ROOT + offset, 1)[0] for offset in (0xc4, 0xd2))
    configured, refresh, route, phase, single_field = (
        get(PICTURE_ROOT + offset, 1)[0] for offset in (0xc5, 0x178, 0x180, 0x181, 0x1cd))
    usable = active != 0 and started == 1
    fresh = route == 1 or not configured
    physical = (PICTURE_TOKEN if load(META_DEL) != load(META_DEL + 4) else 0) if fresh else load(PICTURE_ROOT + 0xe4)
    if kind == "picture" and usable and physical:
        tag = load(META_RECORD) == 0x100
        cached = configured and (tag or not refresh)
        p = get(PICTURE_ROOT + 0xec, 140) if cached else bytes(picture)
        if tag and not configured:
            p = bytearray(p)
            p[8] = 2
            struct.pack_into("<2I", p, 0x14, 1920, 1080)
        same_format = (p[0x14:0x1c] == get(PICTURE_ROOT + 0x100, 8) and
                       p[0x1d] == get(PICTURE_ROOT + 0x109, 1)[0] and
                       (p[8] == 2) == (get(PICTURE_ROOT + 0xf4, 1)[0] == 2))
        configured = configured if cached else (1 if tag else (configured if same_format else 0))
        put(PICTURE_ROOT + 0xc5, b"\x01", expected)
        tuple_bytes = struct.pack("<3I", META_RECORD, physical, 0) if fresh else get(PICTURE_ROOT + 0xe0, 12)
        put(PICTURE_ROOT + 0xe0, tuple_bytes, expected)
        if not configured:
            put(PICTURE_ROOT + 0xec, p, expected)
            put(PICTURE_ROOT + 0xc6, b"\0", expected)
            put(PICTURE_ROOT + 0x178, b"\0", expected)
            put(PICTURE_ROOT + 0x180, b"\0", expected)
        else:
            put(PICTURE_ROOT + 0xc6, b"\x01", expected)
            put(PICTURE_ROOT + 0x178, b"\x01", expected)
            if tag:
                put(PICTURE_ROOT + 0x180, b"\x01", expected)
            else:
                put(PICTURE_ROOT + 0xec, p, expected)
                word(0x10502100, load(0x10502100) | 2, expected)
                put(PICTURE_ROOT + 0x180, bytes([1 if p[8] == 2 or single_field or phase else 0]), expected)
                if p[8] != 2:
                    put(PICTURE_ROOT + 0x181, bytes([0 if single_field or phase else 1]), expected)
        put(PICTURE_ROOT + 0x1a8, b"\0", expected)
    if kind == "irq":
        word(0x10541208, 0xc3, expected)
        word(0xd2214, 0, expected)
    actual = emulate(image, pages, registers,
                     ((0x834c, 0x8520), (0x85dc, 0x8838), (0x78dc, 0x79a8), (0x888c, 0x8948)) +
                     tuple((low, high) for _, low, high, _ in METADATA_BODIES),
                     (0x70f0, 0x710c, 0x898, 0x203c4, 0x20708, 0x206e4, 0xe110,
                      0x1bfc, 0x82d0, 0x1f8c, 0x7bd4, 0x7898, 0x77e0, 0xaf18),
                     contract, budget, clobber_flags=0xf0000000,
                     real_callees=(0x78dc, 0xd624, 0xd5a4, 0x1fdac),
                     call_edges=PICTURE_CALLS, tail_edges=PICTURE_TAILS, instruction=instruction)
    if depth != 0 or actual["pages"] != {base: bytes(page) for base, page in expected.items()}:
        differences = [hex(base + offset) for base, page in expected.items()
                       for offset, (wanted, got) in enumerate(zip(page, actual["pages"][base]))
                       if wanted != got][:8]
        raise ValueError("picture full-page/critical oracle changed: " + ",".join(differences))
    actual.update(kind=kind, events=events, publication=publication,
                  expected={base: bytes(page) for base, page in expected.items()},
                  snapshot=PictureRAM(actual["pages"], picture))
    return actual


def execute_mfd_source(record, budget=256, payload=None):
    """Stock address arithmetic and HAL stores in three synthetic RAM pages.

    Selector/format/mode/field admission is replay-only, not native legality.
    Context[0]=0 makes fixed HAL offsets guest-RAM addresses. Line-address
    scalars, including zero/wrapped values, are never followed. The sole
    synthetic callee is logging; no MFD engine, completion or lease is modeled.
    """
    if type(record) not in (bytes, bytearray) or len(record) != 116 or \
            record[0x5c] > 2 or record[0x27] not in (1, 2, 3) or \
            record[8] not in (0, 1, 2) or record[0x28] not in (0, 1):
        raise ValueError("invalid synthetic MFD record")
    if type(budget) is not int or not 1 <= budget <= 256:
        raise ValueError("invalid instruction budget")
    record = bytes(record)
    payload = Model.payload if payload is None else payload
    if type(payload) not in (bytes, bytearray):
        raise ValueError("invalid MFD source payload")
    code = {0x1000: bytearray(struct.pack("<I", 0xe7f000f0) * 1024),
            0x1e000: bytearray(struct.pack("<I", 0xe7f000f0) * 1024),
            0x2c000: bytearray(b"\xa5" * 4096)}
    for low, high, digest in MFD_SOURCE_BODIES:
        if hashlib.sha256(payload[low:high]).hexdigest() != digest:
            raise ValueError("stock MFD source body changed")
        code[low & ~4095][low & 4095:high & 4095] = payload[low:high]
    table = struct.pack("<9I", *(value for row in MFD_SOURCE_ROWS for value in row))
    if payload[0x2cccc:0x2ccf0] != table:
        raise ValueError("stock MFD source table changed")
    code[0x2c000][0xccc:0xcf0] = table
    for address, value in MFD_SOURCE_WORDS.items():
        if payload[address:address + 4] != struct.pack("<I", value):
            raise ValueError("stock MFD source literal changed")
        struct.pack_into("<I", code[0x1000], address & 4095, value)
    for site, target in MFD_SOURCE_CALLS.items():
        instruction = struct.unpack_from("<I", payload, site)[0]
        displacement = (instruction & 0xffffff) << 2
        if displacement & 0x2000000:
            displacement -= 0x4000000
        if instruction & 0xff000000 != 0xeb000000 or site + 8 + displacement != target:
            raise ValueError("stock MFD source call target changed")
    pages = {base: bytearray(b"\xa5" * 4096) for base in (0x300000, 0x400000, 0x540000)}
    struct.pack_into("<I", pages[0x400000], 0, 0)
    pages[0x400000][0x100:0x174] = record
    registers = [0xabc00000 + index for index in range(16)]
    registers[:2] = [MFD_CONTEXT, MFD_RECORD]
    registers[13:16] = [MFD_STACK, RETURN, 0x1918]
    image = segment_elf([(base, bytes(page), 4 if base == 0x2c000 else 5)
                         for base, page in code.items()] +
                        [(base, bytes(page), 6) for base, page in pages.items()], 0x1918)
    # Existing independently tested arithmetic oracle, not a second ISA loop.
    oracle = FW.MAP._mfd_source_model(record, MFD_SOURCE_ROWS)
    expected = {base: bytearray(page) for base, page in pages.items()}
    struct.pack_into("<5I", expected[0x300000], (MFD_STACK - 20) & 4095,
                     *registers[4:8], RETURN)
    row = MFD_SOURCE_ROWS[record[0x5c]]
    struct.pack_into("<5I", expected[0x300000], (MFD_STACK - 40) & 4095,
                     row[2], record[0x28], *row)
    for address, value in oracle["writes"]:
        struct.pack_into("<I", expected[0x540000], address - 0x540000, value)
    writes = []
    def instruction(rsp, pc, regs):
        if pc == 0x1e8ec:
            position = len(writes)
            if regs[0] != MFD_CONTEXT or regs[3] != 0 or position >= len(oracle["writes"]) or \
                    [regs[1], regs[2]] != oracle["writes"][position]:
                raise ValueError("outside ordered MFD register-write contract")
            writes.append([regs[1], regs[2]])
    def log(rsp, pc, args, stack):
        if pc != 0x203c4 or stack != MFD_STACK - 40:
            raise ValueError("unexpected MFD logging contract")
        return 0
    actual = emulate(image, pages, registers, ((0x1918, 0x1ad8), (0x1e8e8, 0x1e8f4)),
                     (0x203c4,), log, budget, clobber_flags=0xf0000000,
                     real_callees=(0x1e8e8,), call_edges=MFD_SOURCE_CALLS, instruction=instruction)
    expected = {base: bytes(page) for base, page in expected.items()}
    if actual["pages"] != expected or actual["status"] != oracle["return_value"] or writes != oracle["writes"]:
        raise ValueError("MFD full-page/arithmetic oracle changed")
    actual.update(expected=expected, writes=writes, record=record)
    return actual


def execute_source_producer(null=False, cleared=True, budget=256, payload=None):
    """Real E110/D880/translation; geometry/format helpers are CPU contracts.

    Physical and translated Y/C scalars are deliberately unmapped. This proves
    their selected copy ordering, not planes, a complete native PIB or a lease.
    The returned register is a scalar, not a completion/status acknowledgement.
    """
    if type(null) is not bool or type(cleared) is not bool:
        raise ValueError("invalid synthetic source arguments")
    if type(budget) is not int or not 1 <= budget <= 256:
        raise ValueError("invalid instruction budget")
    payload = Model.payload if payload is None else payload
    if type(payload) not in (bytes, bytearray):
        raise ValueError("invalid source payload")
    code = {base: bytearray(struct.pack("<I", 0xe7f000f0) * 1024)
            for base in (0xd000, 0xe000, 0x1f000)}
    for low, high, digest in SOURCE_BODIES:
        if hashlib.sha256(payload[low:high]).hexdigest() != digest:
            raise ValueError("stock source producer body changed")
        code[low & ~4095][low & 4095:high & 4095] = payload[low:high]
    table = bytes(range(10)) + bytes(6)
    if payload[0xe2e0:0xe2e4] != struct.pack("<I", 0x2dcf6) or \
            payload[0x2dcf6:0x2dd06] != table:
        raise ValueError("stock source producer table/literal changed")
    code[0xe000][0x2e0:0x2e4] = struct.pack("<I", 0x2dcf6)
    table_page = bytearray(b"\xa5" * 4096)
    table_page[0xcf6:0xd06] = table
    for site, target in SOURCE_CALLS.items():
        word = struct.unpack_from("<I", payload, site)[0]
        displacement = (word & 0xffffff) << 2
        if displacement & 0x2000000:
            displacement -= 0x4000000
        if word & 0xff000000 != 0xeb000000 or site + 8 + displacement != target:
            raise ValueError("stock source producer call target changed")
    pages = {base: bytearray(b"\xa5" * 4096)
             for base in (0x300000, 0x400000, 0x401000, 0x500000, 0x600000)}
    def put(address, data, destination=pages):
        base, offset = address & ~4095, address & 4095
        if base not in destination or offset + len(data) > 4096:
            raise ValueError("outside source producer RAM")
        destination[base][offset:offset + len(data)] = data
    def word(address, value, destination=pages):
        put(address, struct.pack("<I", value), destination)
    for address, value in ((SOURCE_H + 0x64, SOURCE_C), (SOURCE_H + 0x228, SOURCE_MAP),
            (SOURCE_C + 0x20c, 10), (SOURCE_META + 4, 0x920040), (SOURCE_META + 8, 0x920100),
            (SOURCE_META + 0x0c, 0x11112222), (SOURCE_META + 0x10, 0x33334444),
            (SOURCE_META + 0x14, 256), (SOURCE_META + 0x18, 96),
            (SOURCE_META + 0x20, 2), (SOURCE_META + 0x24, 0x31),
            (SOURCE_MAP + 4, 0), (SOURCE_MAP + 0x18, 0x940000),
            (SOURCE_MAP + 0x1c, 0x940fff), (SOURCE_MAP + 0x28, 0x940000),
            (SOURCE_MAP + 0x30, 0x920000)):
        word(address, value)
    fill = 0 if cleared else 0xa5
    put(SOURCE_PICTURE, bytes([fill]) * 140)
    expected = {base: bytearray(page) for base, page in pages.items()}
    if null:
        for offset, value in ((0, 0), (1, 1), (8, 0), (0x1c, 2), (0x1d, 0), (0x1e, 0),
                (0x24, 0), (0x25, 0), (0x26, 0), (0x27, 2), (0x28, 0), (0x29, 2),
                (0x2a, 2), (0x40, 0), (0x41, 0), (0x50, 0), (0x5c, 0)):
            put(SOURCE_PICTURE + offset, bytes([value]), expected)
        for offset, value in ((4, 0), (0x20, 0), (0x34, 0), (0x38, 0), (0x44, 0),
                              (0x74, 8), (0x78, 8), (0x7c, 0), (0x80, 0)):
            word(SOURCE_PICTURE + offset, value, expected)
    else:
        for offset, value in ((1, 0), (8, 2), (0x1e, 0x31), (0x26, 0),
                              (0x27, 1), (0x28, 0), (0x5c, 0)):
            put(SOURCE_PICTURE + offset, bytes([value]), expected)
        for offset, value in ((4, SOURCE_MAP), (0x14, 256), (0x18, 96),
                (0x2c, 0x11112222), (0x30, 0x33334444), (0x34, 0x920040),
                (0x38, 0x920100), (0x54, 16), (0x58, 6), (0x6c, 0), (0x70, 0)):
            word(SOURCE_PICTURE + offset, value, expected)
    registers = [0xabc00000 + index for index in range(16)]
    registers[:3] = [SOURCE_H, 0 if null else SOURCE_META, SOURCE_PICTURE]
    registers[13:16] = [SOURCE_STACK, RETURN, 0xe110]
    image = segment_elf([(base, bytes(page), 5) for base, page in code.items()] +
                        [(0x2d000, bytes(table_page), 4)] +
                        [(base, bytes(page), 6) for base, page in pages.items()], 0xe110)
    translations, before_copy = [], []
    pushes = {0xe110: (4, 5, 6, 14), 0xd880: (4, 5, 6, 7, 8, 14), 0x1fdac: (4, 5, 14)}
    def instruction(rsp, pc, regs):
        if pc in pushes:
            saved = [regs[index] for index in pushes[pc]]
            put(regs[13] - 4 * len(saved), struct.pack("<" + "I" * len(saved), *saved), expected)
        if pc == 0x1fdcc:
            wanted = ((SOURCE_PICTURE + 0x34, 0x940040), (SOURCE_PICTURE + 0x38, 0x940100))
            if len(translations) >= 2 or (regs[2], regs[0]) != wanted[len(translations)]:
                raise ValueError("outside ordered source translation contract")
            translations.append((regs[2], regs[0]))
        if pc == 0xe19c:
            before_copy.extend(struct.unpack("<2I", rsp.memory(SOURCE_PICTURE + 0x34, size=8)))
    def contract(rsp, pc, args, stack):
        if pc not in SOURCE_HELPERS or args[:2] != (SOURCE_H, SOURCE_META) or \
                args[2 if pc != 0xd9c4 else 3] != SOURCE_PICTURE or \
                (pc == 0xd9c4 and args[2] != 0) or stack != SOURCE_STACK - 16:
            raise ValueError("unexpected source geometry/format contract")
        if pc == 0xd92c:
            rsp.memory(SOURCE_PICTURE + 0x14, struct.pack("<2I", 256, 96))
        if pc == 0xdff4:
            rsp.memory(SOURCE_PICTURE + 8, b"\x02")
            rsp.memory(SOURCE_PICTURE + 0x28, b"\0")
            rsp.memory(SOURCE_PICTURE + 0x6c, bytes(8))
        return 0
    actual = emulate(image, pages, registers, tuple((low, high) for low, high, _ in SOURCE_BODIES),
                     SOURCE_HELPERS, contract, budget, clobber_flags=0xf0000000,
                     real_callees=(0xd880, 0x1fdac), call_edges=SOURCE_CALLS, instruction=instruction)
    expected = {base: bytes(page) for base, page in expected.items()}
    if actual["pages"] != expected or actual["status"] != (0 if null else 0x920100) or \
            translations != ([] if null else [(SOURCE_PICTURE + 0x34, 0x940040),
                                               (SOURCE_PICTURE + 0x38, 0x940100)]) or \
            before_copy != ([] if null else [0x940040, 0x940100]):
        raise ValueError("source producer full-page/copy oracle changed")
    actual.update(expected=expected, picture=actual["pages"][0x500000][0x100:0x18c],
                  before_copy=before_copy, translations=translations)
    return actual


def execute_source_mode(flags=0, span=256, kind=0, latch=0xff, budget=256, payload=None):
    """Execute stock DFF4/DED4 selection and its shared alternating byte.

    The metadata words are synthetic CPU inputs, not authenticated geometry.
    Only logging is substituted. No field timing, IRQ, planes or lease is
    inferred from the selected bytes or the scalar return register.
    """
    if any(type(value) is not int or not 0 <= value <= 0xffffffff
           for value in (flags, span, kind)) or type(latch) is not int or not 0 <= latch <= 255:
        raise ValueError("invalid synthetic source mode inputs")
    if type(budget) is not int or not 1 <= budget <= 256:
        raise ValueError("invalid instruction budget")
    payload = Model.payload if payload is None else payload
    if type(payload) not in (bytes, bytearray):
        raise ValueError("invalid source mode payload")
    code = {base: bytearray(struct.pack("<I", 0xe7f000f0) * 1024)
            for base in (0xd000, 0xe000)}
    for low, high, digest in SOURCE_MODE_BODIES:
        if hashlib.sha256(payload[low:high]).hexdigest() != digest:
            raise ValueError("stock source mode body changed")
        cursor = low
        while cursor < high:
            end = min(high, (cursor & ~4095) + 4096)
            code[cursor & ~4095][cursor & 4095:(cursor & 4095) + end - cursor] = payload[cursor:end]
            cursor = end
    if payload[0xe25c:0xe260] != struct.pack("<I", SOURCE_MODE_LATCH):
        raise ValueError("stock source mode literal changed")
    struct.pack_into("<I", code[0xe000], 0x25c, SOURCE_MODE_LATCH)
    for site, target in SOURCE_MODE_CALLS.items():
        word = struct.unpack_from("<I", payload, site)[0]
        displacement = (word & 0xffffff) << 2
        if displacement & 0x2000000:
            displacement -= 0x4000000
        if word & 0xff000000 != 0xeb000000 or site + 8 + displacement != target:
            raise ValueError("stock source mode call target changed")
    pages = {base: bytearray(b"\xa5" * 4096)
             for base in (0x300000, 0x401000, 0x500000, 0xd2000)}
    struct.pack_into("<I", pages[0x401000], 0x100, flags)
    struct.pack_into("<I", pages[0x401000], 0x10c, span)
    struct.pack_into("<I", pages[0x401000], 0x124, kind)
    pages[0xd2000][SOURCE_MODE_LATCH & 4095] = latch
    expected = {base: bytearray(page) for base, page in pages.items()}
    # Independent branch oracle over metadata words, not another ISA executor.
    selected = bool(flags & 4) if kind == 0 else \
        not 720 < span <= 1280 and (bool(flags & 12) or kind == 4)
    mode, field, status = 2, 1, 1
    expected_latch_writes = []
    if selected:
        low = flags & 3
        if low == 2:
            mode, field, status = 0, 0, 0
        elif low == 3:
            mode, field, status = 1, 0, 0
        elif low == 1:
            if latch == 255:
                latch = 0 if flags & 16 else 1
                expected_latch_writes.append(latch)
            mode, field, status = (1 if latch == 0 else 0), 0, 0
            latch = 1 if latch == 0 else 0
            expected_latch_writes.append(latch)
    expected[0x500000][0x108], expected[0x500000][0x128] = mode, field
    expected[0xd2000][SOURCE_MODE_LATCH & 4095] = latch
    registers = [0xabc00000 + index for index in range(16)]
    registers[:3] = [SOURCE_H, SOURCE_META, SOURCE_PICTURE]
    registers[13:16] = [SOURCE_STACK, RETURN, 0xdff4]
    image = segment_elf([(base, bytes(page), 5) for base, page in code.items()] +
                        [(base, bytes(page), 6) for base, page in pages.items()], 0xdff4)
    pushes = {0xdff4: (4, 5, 6, 7, 8, 14), 0xded4: (4, 5, 6, 14)}
    latch_writes = []
    def instruction(rsp, pc, regs):
        if pc in pushes:
            saved = [regs[index] for index in pushes[pc]]
            struct.pack_into("<" + "I" * len(saved), expected[0x300000],
                             (regs[13] - len(saved) * 4) & 4095, *saved)
        if pc in (0xdf70, 0xdf8c, 0xdfb8, 0xdfd4):
            position = len(latch_writes)
            if regs[1] != SOURCE_MODE_LATCH or position >= len(expected_latch_writes) or \
                    regs[0] != expected_latch_writes[position]:
                raise ValueError("outside ordered source mode latch contract")
            latch_writes.append(regs[0])
    def log(rsp, pc, args, stack):
        if pc != 0x203c4 or stack != SOURCE_STACK - 40:
            raise ValueError("unexpected source mode logging contract")
        return 0
    actual = emulate(image, pages, registers,
                     tuple((low, high) for low, high, _ in SOURCE_MODE_BODIES),
                     (0x203c4,), log, budget, clobber_flags=0xf0000000,
                     real_callees=(0xded4,), call_edges=SOURCE_MODE_CALLS, instruction=instruction)
    expected = {base: bytes(page) for base, page in expected.items()}
    if actual["pages"] != expected or actual["status"] != status or \
            latch_writes != expected_latch_writes:
        raise ValueError("source mode full-page/selection oracle changed")
    actual.update(expected=expected, mode=mode, field=field, latch=latch,
                  latch_writes=latch_writes, selected=selected)
    return actual


class FirmwareQemuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        raw = FW.MAP.read_firmware(FW.BLOB)
        if len(raw) != FW.MAP.BUNDLED_SIZE or hashlib.sha256(raw).hexdigest() != FW.MAP.BUNDLED_SHA256:
            raise ValueError("bundled firmware changed")
        Model.payload = raw[:-FW.MAP.TRAILER_SIZE]

    def test_96_stock_allocation_vectors(self):
        model = Model()
        # Both engines must save the same mapped return word on the stack.
        model.RETURN = RETURN
        vectors = [dict(picture=picture, split=split, generic=generic)
                   for picture in ("common", "private", "factory")
                   for split in ("common", "private", "factory")
                   for generic in ("private", "factory")]
        normal = {0x0c: 0x100, 0x14: 0x80, 0x28: 0x200, 0x30: 0x300, 0x38: 0x60, 0x40: 0x70}
        vectors += [dict(sizes={offset: value if mask & (1 << index) else 0
                               for index, (offset, value) in enumerate(normal.items())}) for mask in range(64)]
        vectors += [dict(picture=mode, split=mode, generic=mode, fail=fail)
                    for mode in ("private", "factory") for fail in range(3)]
        vectors += [dict(picture="private", split="private", translation=mode)
                    for mode in ("write", "error", "zero", "omit")]
        vectors += [dict(aliases={0x2c: Model.H + 8}), dict(sizes={0x0c: 0}),
                    dict(base=0xffffff00), dict(sizes={0x1c: 0x10000000, 0x20: 0x10})]
        self.assertEqual(len(vectors), 96)
        results = []
        for index, options in enumerate(vectors):
            with self.subTest(index=index, options=options):
                reference, actual = model.execute(**options), execute(options)
                for key in ("status", "outputs", "calls", "steps"):
                    self.assertEqual(actual[key], reference[key], key)
                expected = {base: bytearray(page) for base, page in actual["initial"].items()}
                for pc, address, value in reference["writes"]:
                    struct.pack_into("<I", expected[address & ~4095], address & 4095, value)
                # Includes all page padding: QEMU's mapping is wider than the
                # interpreter's word-membership bounds. No unexpected writes.
                self.assertEqual(actual["pages"], {base: bytes(page) for base, page in expected.items()})
                results.append(actual)
        self.assertEqual(len(results), 96)
        valid, error = results[88:90]
        self.assertEqual(valid["outputs"], error["outputs"])
        self.assertEqual(error["status"], 0)
        self.assertIn((0x1fe6c, 4), error["stub_status"])
        alias = results[92]
        self.assertEqual((alias["status"], alias["outputs"][0x2c]), (0, Model.POISON))
        self.assertNotEqual(alias["outputs"][0x2c], (alias["outputs"][8] - 0x300) & 0xffffffff)

    def test_guest_unmapped_output_fault(self):
        fault = execute(dict(aliases={8: 0x400000}), expected_signal=11)
        self.assertEqual((fault["signal"], fault["pc"]), (11, 0x25c3c))

    def test_instruction_budget(self):
        with self.assertRaisesRegex(ValueError, "instruction budget exceeded"):
            execute(budget=1)
        for budget in (0, 513, True):
            with self.subTest(budget=budget), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid instruction budget"):
                    execute(budget=budget)
                spawn.assert_not_called()

    def test_changed_body_and_unsafe_inputs_refused_before_spawn(self):
        for offset in (Model.START, 0x26040, Model.END - 1):
            changed = bytearray(Model.payload)
            changed[offset] ^= 1
            with self.subTest(offset=offset), mock.patch.object(Model, "payload", changed), \
                    mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "stock allocation body changed"):
                    execute()
                spawn.assert_not_called()
        for options in (dict(translation="bad"), dict(base=-1), dict(fail=True),
                        dict(aliases={8: RETURN}), dict(aliases={8: Model.H + 1}),
                        dict(sizes={0x0c: -1}), dict(sizes={0x44: 0})):
            with self.subTest(options=options), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid synthetic"):
                    execute(options)
                spawn.assert_not_called()

    def test_stock_open_packets_abi_and_ignored_translation_status(self):
        # Independent packet oracle, not another A32 interpreter. Nonzero
        # argument tags distinguish all 15 stack sources, including caller
        # +0x34 -> packet+0x38; packet+52 stays zero under the clear contract.
        vectors = [(1, 10, status, translation) for status in (0, 4)
                   for translation in ("write", "error", "omit")]
        vectors += [(0, 10, 0, "write"), (0xffffffff, 0xffffffff, 0xffffffff, "omit")]
        for mode, algorithm, transport, translation in vectors:
            with self.subTest(mode=mode, algorithm=algorithm, transport=transport,
                              translation=translation):
                actual = execute_open(mode=mode, algorithm=algorithm, transport=transport,
                                      translation=translation)
                packet = bytearray(252)
                fields = {0: 0x73760002, 4: 0x12345678, 8: algorithm, 12: mode}
                destinations = (16, 20, 64, 68, 72, 76, 24, 28, 32, 36, 40, 44, 48, 56, 60)
                fields.update(zip(destinations, actual["arguments"]))
                for offset, value in fields.items():
                    struct.pack_into("<I", packet, offset, value)
                self.assertEqual(actual["packet"], packet)
                self.assertEqual(actual["response_before"], bytes(252))
                self.assertEqual(struct.unpack_from("<I", packet, 52)[0], 0)
                self.assertEqual((actual["status"], actual["steps"]), (transport, 81))
                self.assertEqual([(site, target) for site, target, _ in actual["calls"]],
                                 [(0x274b4, 0x206e4), (0x274c4, 0x206e4),
                                  (0x27578, 0x2705c), (0x275ac, 0x1fdac)])
                self.assertEqual(actual["stub_status"][-1],
                                 (0x1fdac, 0 if translation == "write" else 4))
                expected = {base: bytearray(page) for base, page in actual["initial"].items()}
                def put(address, data):
                    base = address & ~4095
                    expected[base][address - base:address - base + len(data)] = data
                def word(address, value):
                    put(address, struct.pack("<I", value))
                saved = actual["registers"][4:12] + [RETURN]
                put(OPEN_SP - 36, struct.pack("<9I", *saved))
                put(actual["request"], packet)
                put(actual["response"], actual["reply"])
                word(actual["frame"], 20000)
                word(actual["frame"] + 4, struct.unpack_from("<I", actual["reply"], 20)[0])
                word(actual["frame"] + 8, transport)
                for destination, source in ((0x44, 8), (0x48, 12), (0x50, 16)):
                    word(Model.H + destination, struct.unpack_from("<I", actual["reply"], source)[0])
                if translation != "omit":
                    word(Model.H + 0x58, 0x33445566)
                put(Model.H + 0x220, b"\x01")
                # Request/response abut: whole-page poison detects other
                # writes, rather than pretending there is an in-between gap.
                self.assertEqual(actual["pages"], {base: bytes(page) for base, page in expected.items()})
                # Error transport still publishes reply/marker; an omitted
                # translation leaves poison, not a successful address proof.
                self.assertEqual(actual["pages"][Model.H][0x220:0x224], b"\x01\xa5\xa5\xa5")
                if translation == "omit":
                    self.assertEqual(actual["pages"][Model.H][0x58:0x5c], b"\xa5" * 4)

    def test_open_pin_input_and_budget_refusals(self):
        for offset in (OPEN_START, OPEN_END - 1, OPEN_LITERAL[0], OPEN_LITERAL[0] + 3):
            changed = bytearray(Model.payload)
            changed[offset] ^= 1
            with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "stock OPEN (body|literal) changed"):
                    execute_open(payload=changed)
                spawn.assert_not_called()
        for options in (dict(mode=-1), dict(algorithm=True), dict(transport=1 << 32),
                        dict(arguments=[1] * 14), dict(arguments=[1] * 16),
                        dict(arguments=[-1] * 15), dict(arguments=[1 << 32] * 15),
                        dict(translation="bad"), dict(budget=0), dict(budget=513), dict(budget=True)):
            with self.subTest(options=options), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid"):
                    execute_open(**options)
                spawn.assert_not_called()
        with self.assertRaisesRegex(ValueError, "instruction budget exceeded"):
            execute_open(budget=1)

    def check_metadata_pages(self, actual):
        """Independent byte-footprint oracle, including all RAM padding."""
        expected = {base: bytearray(page) for base, page in actual["initial"].items()}
        def word(address, value):
            struct.pack_into("<I", expected[address & ~4095], address & 4095, value & 0xffffffff)
        def load(address):
            return struct.unpack_from("<I", actual["initial"][address & ~4095], address & 4095)[0]
        def save(address, values):
            for index, value in enumerate(values):
                word(address + index * 4, value)
        kind, registers = actual["kind"], actual["registers"]
        if kind == "release":
            save(META_SP - 32, registers[4:11] + [RETURN])
            next_index = (actual["write"] + 1) & 0xffffffff
            if next_index == 64:
                next_index = 2
            word((META_REL + actual["write"] * 4) & 0xffffffff, actual["token"])
            word(META_REL + 4, next_index)
        else:
            count = 3 if kind == "translate" else 6
            saved = registers[4:6] if kind == "translate" else registers[4:9]
            save(META_SP - count * 4, saved + [RETURN])
            entered = kind == "translate" or (not actual["null_ring"] and
                                               actual["read"] != actual["write"])
            if entered and not actual["null_output"]:
                physical = actual["token"] if kind == "translate" else load(
                    (META_DEL + actual["read"] * 4) & 0xffffffff)
                if kind != "translate":
                    word(META_RECORD + 4, physical)
                    save(META_SP - 36, [META_RECORD, actual["read"],
                                        0xd6a0 if kind == "acquire" else 0xd778])
                # The helper writes each candidate before checking bounds.
                # Failure retains the last translated token, never a null.
                for _, virtual, origin, low, high in actual["nodes"]:
                    translated = (virtual + physical - origin) & 0xffffffff
                    if low <= translated <= high:
                        break
                word(META_RECORD, translated)
            if kind == "acquire" and entered:
                next_index = (actual["read"] + 1) & 0xffffffff
                word(META_DEL, 2 if next_index == 64 else next_index)
        self.assertEqual(actual["pages"], {base: bytes(page) for base, page in expected.items()})
        return actual

    def test_metadata_acquire_peek_complete_pages_and_call_edges(self):
        for kind in ("acquire", "peek"):
            for read in (2, 17, 63):
                with self.subTest(kind=kind, read=read):
                    actual = self.check_metadata_pages(execute_metadata(kind, read=read, write=23))
                    helper_site = 0xd69c if kind == "acquire" else 0xd774
                    expected_calls = [(helper_site, 0x1fdac)]
                    if kind == "acquire":
                        expected_calls.append((0xd6ac, 0x203c4))
                    self.assertEqual([(site, target) for site, target, _ in actual["calls"]], expected_calls)
                    self.assertEqual(actual["calls"][0][2][:3], (META_MAP, 0x710030, META_RECORD))
                    self.assertEqual(actual["real_status"], [(0x1fdac, 0)])
                    self.assertEqual(struct.unpack_from("<2I", actual["pages"][0x500000], 0x100),
                                     (0x810030, 0x710030))
                    if kind == "acquire":
                        log = actual["publication"][0]
                        self.assertEqual(log["args"][:3], (0xd81c, 0x710030, 0x810030))
                        self.assertEqual(struct.unpack_from("<I", log["delivery"])[0], read)
                        self.assertEqual(log["stack"], META_SP - 24)
                    else:
                        self.assertEqual(actual["publication"], [])

    def test_metadata_empty_null_and_null_output_counterexamples(self):
        # Early-return r0 values are incidental, not a success-status ABI.
        for kind in ("acquire", "peek"):
            with self.subTest(kind=kind, case="empty"):
                actual = self.check_metadata_pages(execute_metadata(kind, read=9, write=9))
                self.assertEqual((actual["status"], actual["calls"]), (Model.H, []))
            with self.subTest(kind=kind, case="null-output"):
                actual = self.check_metadata_pages(execute_metadata(kind, null_output=True))
                self.assertEqual(actual["calls"], [])
                # NULL output consumes a nonempty acquire, but not a peek.
                self.assertEqual(struct.unpack_from("<I", actual["pages"][0x400000], 0x100)[0],
                                 3 if kind == "acquire" else 2)
        actual = self.check_metadata_pages(execute_metadata("acquire", null_ring=True))
        self.assertEqual((actual["status"], actual["calls"]), (Model.H, []))
        for kind, pc, options in (("peek", 0xd728, dict(null_ring=True)),
                                  ("release", 0xd5b4, dict(null_ring=True)),
                                  ("release", 0xd604, dict(null_output=True)),
                                  ("translate", 0x1fdcc, dict(null_output=True))):
            with self.subTest(kind=kind, options=options):
                actual = execute_metadata(kind, expected_signal=11, **options)
                self.assertEqual((actual["signal"], actual["pc"]), (11, pc))

    def test_stock_metadata_translation_inclusive_linked_failure_and_u32(self):
        cases = (("direct", 0x710000, 0x810000, 0),
                 ("direct", 0x7100ff, 0x8100ff, 0),
                 ("direct", 0x70ffff, 0x80ffff, 2),
                 ("direct", 0x710100, 0x810100, 2),
                 ("linked", 0x710030, 0x830030, 0),
                 ("linked_failure", 0x710030, 0x840030, 2),
                 ("wrap", 0x30, 0x20, 0))
        for translation, token, expected, status in cases:
            for kind in ("translate", "acquire", "peek"):
                with self.subTest(kind=kind, translation=translation, token=token):
                    actual = self.check_metadata_pages(execute_metadata(
                        kind, translation=translation, token=token, logger_status=7))
                    output = struct.unpack_from("<2I", actual["pages"][0x500000], 0x100)
                    self.assertEqual(output, (expected, token))
                    if kind == "translate":
                        self.assertEqual((actual["status"], actual["calls"]), (status, []))
                    else:
                        self.assertEqual(actual["real_status"], [(0x1fdac, status)])
                        self.assertEqual(actual["status"], 7 if kind == "acquire" else status)
                        # Translation rejection does not veto acquire's consume.
                        self.assertEqual(struct.unpack_from("<I", actual["pages"][0x400000], 0x100)[0],
                                         3 if kind == "acquire" else 2)

    def test_metadata_invalid_acquire_peek_indices_are_not_hard_guards(self):
        for kind in ("acquire", "peek"):
            for read in (0, 1, 64, 0xffffffff):
                with self.subTest(kind=kind, read=read):
                    actual = self.check_metadata_pages(execute_metadata(kind, read=read, write=9))
                    self.assertEqual(actual["calls"][0][1], 0x1fdac)
                    self.assertEqual(len(actual["real_status"]), 1)
                    self.assertEqual(len(actual["publication"]), 1 if kind == "acquire" else 0)

    def test_metadata_release_publish_order_wrap_full_invalid_and_repeat(self):
        cases = ((2, 3, ()), (23, 63, ()), (2, 63, (0xd5f8,)),
                 (3, 2, (0xd5f8,)), (9, 0, (0xd5d4,)),
                 (9, 1, (0xd5d4,)), (9, 64, (0xd5d4,)),
                 (9, 0xffffffff, (0xd5d4,)))
        for read, write, warnings in cases:
            with self.subTest(read=read, write=write):
                actual = self.check_metadata_pages(execute_metadata("release", read=read, write=write,
                                                                   logger_status=0xabcdef01))
                self.assertEqual([(site, target) for site, target, _ in actual["calls"]],
                                 [(site, 0x203c4) for site in (*warnings, 0xd618)])
                self.assertEqual(actual["real_status"], [])
                if warnings == (0xd5d4,):
                    self.assertEqual(actual["calls"][0][2][:2], (0xd780, write))
                if warnings == (0xd5f8,):
                    self.assertEqual(actual["calls"][0][2][:2], (0xd7c0, read))
                self.assertEqual(actual["calls"][-1][2][:3],
                                 (0xd7f8, (write - 2) & 0xffffffff, 0x710030))
                last = actual["publication"][-1]
                if 2 <= write < 64:
                    self.assertEqual(struct.unpack_from("<I", last["release"], write * 4)[0], 0x710030)
                    self.assertEqual(struct.unpack_from("<I", last["release"], 4)[0], write)
                # Logger return values are not a release acknowledgement.
                self.assertEqual(actual["status"], 0xabcdef01)

    def test_metadata_joined_acquire_and_duplicate_release_preserve_ram(self):
        acquired = self.check_metadata_pages(execute_metadata("acquire"))
        self.assertEqual(struct.unpack_from("<2I", acquired["pages"][0x500000], 0x100),
                         (0x810030, 0x710030))
        before = acquired
        for slot in (3, 4):
            actual = self.check_metadata_pages(execute_metadata("release", previous=before["snapshot"]))
            self.assertEqual({base: bytes(page) for base, page in actual["initial"].items()}, before["pages"])
            self.assertEqual(actual["calls"][-1][2][:3], (0xd7f8, slot - 2, 0x710030))
            self.assertEqual(struct.unpack_from("<2I", actual["pages"][0x401000], 0x100),
                             (2, slot + 1))
            for published in range(3, slot + 1):
                self.assertEqual(struct.unpack_from("<I", actual["pages"][0x401000],
                                                   0x100 + published * 4)[0], 0x710030)
            self.assertEqual(actual["pages"][Model.H], acquired["pages"][Model.H])
            self.assertEqual(actual["pages"][0x600000], acquired["pages"][0x600000])
            self.assertEqual(actual["pages"][0x400000], acquired["pages"][0x400000])
            self.assertEqual(actual["pages"][0x500000], acquired["pages"][0x500000])
            before = actual
        # Same RAM context and token was returned twice; no one-shot guard.
        self.assertEqual(struct.unpack_from("<2I", before["pages"][0x500000], 0x100),
                         (0x810030, 0x710030))
        with self.assertRaises(TypeError):
            before["snapshot"][0][0][1][0] = 0
        for prior in (dict(before["pages"]), tuple(before["snapshot"]), None):
            args = dict(previous=prior)
            if prior is None:
                args = dict(previous=before["snapshot"], write=4)
            with self.subTest(args=args.keys()), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "metadata replay arguments"):
                    execute_metadata("release", **args)
                spawn.assert_not_called()
        changed = {base: bytearray(page) for base, page in before["pages"].items()}
        struct.pack_into("<I", changed[Model.H], 0x250, 0)
        with mock.patch.object(subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(ValueError, "metadata replay context"):
                execute_metadata("release", previous=MetadataRAM(changed, before["nodes"]))
            spawn.assert_not_called()

    def test_metadata_fuses_inputs_budget_and_target_allowlist(self):
        for _, start, end, _ in METADATA_BODIES:
            for offset in (start, start + (end - start) // 2, end - 1):
                changed = bytearray(Model.payload)
                changed[offset] ^= 1
                with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                    with self.assertRaisesRegex(ValueError, "stock metadata .* body changed"):
                        execute_metadata("acquire", payload=changed)
                    spawn.assert_not_called()
        for address, data in METADATA_LITERALS:
            for offset in (address, address + len(data) - 1):
                changed = bytearray(Model.payload)
                changed[offset] ^= 1
                with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                    with self.assertRaisesRegex(ValueError, "stock metadata literal changed"):
                        execute_metadata("peek", payload=changed)
                    spawn.assert_not_called()
        for options in (dict(kind="bad"), dict(read=True), dict(read=65), dict(write=-1),
                        dict(write=128), dict(null_ring=1), dict(null_output=1),
                        dict(token=-1), dict(token=1 << 32), dict(logger_status=True),
                        dict(translation="omit"), dict(budget=0), dict(budget=513), dict(budget=True)):
            args = dict(kind="acquire")
            args.update(options)
            with self.subTest(options=options), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid"):
                    execute_metadata(**args)
                spawn.assert_not_called()
        with self.assertRaisesRegex(ValueError, "instruction budget exceeded"):
            execute_metadata("acquire", budget=1)
        with mock.patch.dict(METADATA_CALLS, {0xd69c: 0x203c4}):
            with self.assertRaisesRegex(ValueError, "call-target allowlist"):
                execute_metadata("acquire")


class FirmwarePictureQemuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        FirmwareQemuTests.setUpClass()

    def check(self, actual):
        self.assertEqual(actual["pages"], actual["expected"])
        for base in (Model.H, 0x400000, 0x500000, 0x600000, 0x10540000):
            if base != 0x400000 or actual["kind"] == "irq":
                self.assertEqual(actual["pages"][base], bytes(actual["initial"][base]))
        return actual

    def state(self, actual):
        page = actual["pages"][0xd3000]
        return tuple(page[0xa00 + offset] for offset in (0xc5, 0x178, 0x180, 0x181))

    def test_progressive_preroll_cached_kick_then_irq_return(self):
        first = self.check(execute_picture())
        idle_irq = self.check(execute_picture("irq", previous=first["snapshot"]))
        cached = self.check(execute_picture(previous=idle_irq["snapshot"]))
        returned = self.check(execute_picture("irq", previous=cached["snapshot"]))
        self.assertEqual([first["steps"], idle_irq["steps"], cached["steps"], returned["steps"]],
                         [223, 39, 121, 70])
        self.assertEqual(self.state(first), (1, 0, 0, 0))
        self.assertEqual(self.state(cached), (1, 1, 1, 0))
        self.assertEqual([(site, target) for site, target, _ in first["calls"]], [
            (0x8368, 0x70f0), (0x836c, 0x898), (0x8440, 0x203c4), (0x8450, 0x206e4),
            (0x845c, 0xd624), (0xd69c, 0x1fdac), (0xd6ac, 0x203c4), (0x85e8, 0x206e4),
            (0x85f8, 0xe110), (0x8608, 0x203c4), (0x84dc, 0x1bfc), (0x8614, 0x82d0),
            (0x8634, 0x1f8c), (0x8650, 0x78dc), (0x7970, 0x203c4), (0x86c8, 0xaf18),
            (0x86d8, 0x20708), (0x8710, 0x7bd4), (0x871c, 0x7898), (0x8734, 0x20708),
            (0x881c, 0x77e0), (0x8830, 0x710c)])
        self.assertEqual([(site, target) for site, target, _ in cached["calls"]], [
            (0x8368, 0x70f0), (0x836c, 0x898), (0x83d8, 0x203c4), (0x83e8, 0x20708),
            (0x84c8, 0x20708), (0x84dc, 0x1bfc), (0x8614, 0x82d0), (0x8634, 0x1f8c),
            (0x8750, 0x20708), (0x8768, 0x7bd4), (0x87b0, 0x7898), (0x87c4, 0x20708),
            (0x881c, 0x77e0), (0x8830, 0x710c)])
        for actual, kick_mode in ((first, 1), (cached, 0)):
            self.assertEqual([event[0] for event in actual["events"]],
                             ["enter", "build", "kick", "rx-publish", "leave"])
            self.assertEqual(actual["events"][2][1], kick_mode)
            self.assertEqual(actual["events"][1][3][:8], struct.pack("<2I", META_RECORD, PICTURE_TOKEN))
        # The real first kick precedes cache publication. Delivery of this
        # synthetic IRQ is deferred until leave, not simulated during the kick.
        self.assertEqual(first["events"][2][2][-4:], struct.pack("<I", 0xdec0adde))
        self.assertEqual(first["events"][-1][1][-4:], bytes(4))
        self.assertEqual(cached["events"][2][3], b"\0\0")
        self.assertEqual(cached["events"][-1][2], b"\x01\0")
        self.assertEqual(idle_irq["real_status"], [])
        self.assertEqual(returned["real_status"], [(0xd5a4, 0)])
        self.assertEqual(struct.unpack_from("<3I", returned["pages"][0x401000], 0x100),
                         (2, 3, PICTURE_TOKEN))
        self.assertEqual(struct.unpack_from("<2I", returned["pages"][0x400000], 0x100), (3, 3))
        self.assertEqual(first["real_status"], [(0x1fdac, 0), (0xd624, 0), (0x78dc, 0)])

    def test_field_modes_phase_and_single_field_routes(self):
        for mode in (0, 1):
            for single in (False, True):
                with self.subTest(mode=mode, single=single):
                    before = None
                    gates = (0, 1) if single else (0, 0, 1)
                    phases = (0, 0) if single else (0, 1, 0)
                    for index, (gate, phase) in enumerate(zip(gates, phases)):
                        actual = self.check(execute_picture(mode=mode, single_field=single) if before is None else
                                            execute_picture(previous=before["snapshot"]))
                        self.assertEqual(self.state(actual), (1, int(index != 0), gate, phase))
                        self.assertEqual(actual["events"][2][1], 1 if index == 0 else 0)
                        builder = actual["events"][1]
                        self.assertEqual((builder[1:3], builder[4][8]), ((1 if index == 0 else 0, 0), mode))
                        irq = self.check(execute_picture("irq", previous=actual["snapshot"]))
                        self.assertEqual(irq["real_status"], [(0xd5a4, 0)] if gate else [])
                        self.assertEqual(struct.unpack_from("<I", irq["pages"][0x401000], 0x104)[0],
                                         3 if gate else 2)
                        before = irq
                    self.assertEqual(struct.unpack_from("<I", before["pages"][0x400000], 0x100)[0], 3)

    def test_active_started_empty_and_zero_cached_physical_guards(self):
        for options in (dict(active=0), dict(started=0), dict(started=2),
                        dict(empty=True), dict(configured=True, cached_physical=False)):
            with self.subTest(options=options):
                actual = self.check(execute_picture(**options))
                self.assertEqual([event[0] for event in actual["events"]], ["enter", "leave"])
                self.assertNotIn(0xd5a4, [target for _, target, _ in actual["calls"]])
                self.assertEqual(actual["pages"][0xd3000], bytes(actual["initial"][0xd3000]))
                irq = self.check(execute_picture("irq", previous=actual["snapshot"]))
                self.assertEqual(irq["real_status"], [])
        # Nonzero active is the native byte guard, not a model-only ==1 guard.
        actual = self.check(execute_picture(active=2))
        self.assertEqual(self.state(actual), (1, 0, 0, 0))

    def test_tagged_empty_picture_branch_is_not_a_lease(self):
        for configured in (False, True):
            with self.subTest(configured=configured):
                actual = self.check(execute_picture(metadata_flags=0x100, configured=configured))
                builder = actual["events"][1]
                self.assertEqual(builder[1:3], (0, 1))
                self.assertEqual(self.state(actual), (1, 1, 1, 0))
                self.assertEqual(actual["events"][2][1], 1)
                if not configured:
                    self.assertEqual(builder[4][8], 2)
                    self.assertEqual(struct.unpack_from("<2I", builder[4], 0x14), (1920, 1080))
                    self.assertIn((0x8480, 0xe110, (Model.H, 0, META_SP - 168, 0xd3d3d3d3)), actual["calls"])
                irq = self.check(execute_picture("irq", previous=actual["snapshot"]))
                self.assertEqual(irq["real_status"], [(0xd5a4, 0)])
                self.assertEqual(irq["pages"][0x500000], actual["pages"][0x500000])

    def test_real_format_gate_and_pinned_tail_return(self):
        for changed in (False, True):
            with self.subTest(changed=changed):
                actual = self.check(execute_picture(configured=True, route=1, refresh=True, format_change=changed))
                sites = [site for site, _, _ in actual["calls"]]
                self.assertIn(0x8650, sites)
                self.assertIn(0x79a4 if changed else 0x7970, sites)
                self.assertEqual(self.state(actual), (1, 0, 0, 0) if changed else (1, 1, 1, 0))
                self.assertEqual(actual["events"][2][1], 1 if changed else 0)
        original = RSP.registers
        def wrong_lr(rsp):
            regs = original(rsp)
            if regs[15] == 0x203c4 and regs[14] == 0x8654:
                regs[14] = RETURN
            return regs
        with mock.patch.object(RSP, "registers", wrong_lr):
            with self.assertRaisesRegex(ValueError, "pinned tail LR"):
                execute_picture(configured=True, route=1, refresh=True)
        for register, value, message in ((13, 0x900000, "outside picture RAM"),
                                         (15, 0x8520, "outside pinned executable slices")):
            def outside(rsp):
                regs = original(rsp)
                if regs[15] == 0x834c:
                    regs[register] = value
                return regs
            with self.subTest(register=register), mock.patch.object(RSP, "registers", outside):
                with self.assertRaisesRegex(ValueError, message):
                    execute_picture()
        for tails in ({0x7970: 0xaf18}, {0x796c: 0x203c4}, {True: 0x203c4}, []):
            with self.subTest(tails=tails), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid pinned tail"):
                    emulate(b"", {}, [], (), (0x203c4,), lambda *args: 0, 1,
                            real_callees=(0x78dc,), tail_edges=tails)
                spawn.assert_not_called()

    def test_picture_source_pins_and_refusals_before_spawn(self):
        for _, low, high, _ in PICTURE_BODIES + PICTURE_GUARDS + METADATA_BODIES:
            for offset in (low, high - 1):
                changed = bytearray(Model.payload)
                changed[offset] ^= 1
                with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                    with self.assertRaisesRegex(ValueError, "stock picture .* body changed"):
                        execute_picture(payload=changed)
                    spawn.assert_not_called()
        for offset in (0x6fc, 0x7128, 0x7144, 0x7174, 0xadc8, 0x79b0, 0x7a2c, 0x8964, 0x8990):
            changed = bytearray(Model.payload)
            changed[offset] ^= 1
            with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "stock picture control/literal"):
                    execute_picture(payload=changed)
                spawn.assert_not_called()
        for options in (dict(kind="bad"), dict(mode=True), dict(mode=3), dict(active=True),
                        dict(started=-1), dict(single_field=1), dict(configured=1), dict(route=True),
                        dict(phase=2), dict(refresh=1), dict(format_change=1), dict(empty=1),
                        dict(cached_physical=1), dict(metadata_flags=True), dict(metadata_flags=1),
                        dict(budget=0), dict(budget=True), dict(budget=513), dict(payload="bad")):
            with self.subTest(options=options), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid"):
                    execute_picture(**options)
                spawn.assert_not_called()
        with self.assertRaisesRegex(ValueError, "instruction budget exceeded"):
            execute_picture(budget=1)
        with mock.patch.dict(PICTURE_CALLS, {0x8650: 0x203c4}), mock.patch.object(subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(ValueError, "stock picture call target"):
                execute_picture()
            spawn.assert_not_called()

    def test_snapshot_detached_bounds_and_no_owner_generation_guard(self):
        actual = self.check(execute_picture())
        snapshot = actual["snapshot"]
        with self.assertRaises(TypeError):
            snapshot[0][0][1][0] = 0
        for previous in (dict(actual["pages"]), tuple(snapshot)):
            with self.subTest(previous=type(previous)), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid synthetic picture arguments"):
                    execute_picture(previous=previous)
                spawn.assert_not_called()
        for address in (Model.H + 0x224, Model.H + 0x254, META_MAP, PICTURE_ROOT + 0xe0):
            pages = {base: bytearray(page) for base, page in actual["pages"].items()}
            struct.pack_into("<I", pages[address & ~4095], address & 4095, 0x900000)
            unsafe = PictureRAM(pages, snapshot[1])
            with self.subTest(address=address), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "picture replay context"):
                    execute_picture(previous=unsafe)
                spawn.assert_not_called()
        pages = {base: bytearray(page) for base, page in actual["pages"].items()}
        detached = PictureRAM(pages, bytearray(snapshot[1]))
        pages[0xd3000][0xa00 + 0xc4] = 0
        self.assertEqual(detached, snapshot)
        # The IRQ can return the same scalar again; no one-shot/generation
        # guard is invented by this replay or by the stock release helper.
        cached = self.check(execute_picture(previous=detached))
        returned = self.check(execute_picture("irq", previous=cached["snapshot"]))
        repeated = self.check(execute_picture("irq", previous=returned["snapshot"]))
        self.assertEqual(struct.unpack_from("<4I", repeated["pages"][0x401000], 0x100),
                         (2, 4, PICTURE_TOKEN, PICTURE_TOKEN))


class FirmwareMfdSourceQemuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        FirmwareQemuTests.setUpClass()

    def check(self, actual):
        self.assertEqual(actual["pages"], actual["expected"])
        self.assertEqual(actual["pages"][0x400000], bytes(actual["initial"][0x400000]))
        self.assertEqual(actual["real_status"], [(0x1e8e8, MFD_CONTEXT)] * len(actual["writes"]))
        self.assertLessEqual(actual["steps"], 116)
        self.assertEqual([target for _, target, _ in actual["calls"] if target != 0x203c4],
                         [0x1e8e8] * len(actual["writes"]))
        return actual

    def test_actual_mfd_selector_mode_format_field_matrix(self):
        count = 0
        for selector in range(3):
            for mode in range(3):
                for form in (1, 2, 3):
                    for field in (0, 1):
                        with self.subTest(selector=selector, mode=mode, form=form, field=field):
                            record = FW.FirmwareMfdSourceTests.record(
                                selector, mode, form, field, offset_6c=3, offset_70=5)
                            before = bytes(record)
                            actual = self.check(execute_mfd_source(record))
                            self.assertEqual(record, before)
                            oracle = FW.MAP._mfd_source_model(before, ((0, 64, 6), (1, 128, 7), (2, 256, 8)))
                            self.assertEqual((actual["writes"], actual["status"]),
                                             (oracle["writes"], oracle["return_value"]))
                            self.assertEqual([site for site, target, _ in actual["calls"] if target == 0x1e8e8],
                                             [0x19a0, 0x19b0, 0x19c0] + ([] if form == 3 else [0x1abc, 0x1acc]))
                            count += 1
        self.assertEqual(count, 54)

    def test_actual_mfd_zero_wrap_and_partial_format(self):
        record = FW.FirmwareMfdSourceTests.record
        zero = self.check(execute_mfd_source(bytes(record(mode=2, y=0, c=0, yn=0, cn=0))))
        self.assertEqual((zero["steps"], zero["status"], zero["writes"]), (106, 0, [
            [0x540010, 0x00400040], [0x540028, 0], [0x54002c, 0], [0x54001c, 0], [0x540020, 0]]))
        field = self.check(execute_mfd_source(record(1, 1, 1, 1, 3, 5)))
        self.assertEqual((field["steps"], field["status"], field["writes"]), (116, 0, [
            [0x540010, 0x00800100], [0x540028, 40], [0x54002c, 20],
            [0x54001c, 0x1184], [0x540020, 0x8104]]))
        wrapped = self.check(execute_mfd_source(record(2, 0, 2, 0, 0xffffffff, 0xfffffffe)))
        self.assertEqual((wrapped["steps"], wrapped["status"]), (106, 0))
        self.assertEqual(wrapped["writes"][-2:], [[0x54001c, 0xfffd8ffe], [0x540020, 0xffff3ffe]])
        partial = self.check(execute_mfd_source(record(form=3)))
        self.assertEqual((partial["steps"], partial["status"], len(partial["writes"])), (78, 8, 3))
        self.assertEqual(partial["pages"][0x540000][0x1c:0x24], b"\xa5" * 8)
        # Form3's partial-register effect is not a valid native input claim.
        for form in (1, 2):
            for offset in (0x7fffffff, 0x80000000, 0xfffffffd, 0xffffffff):
                with self.subTest(form=form, offset=offset):
                    self.check(execute_mfd_source(record(2, 1, form, 1, offset, offset,
                                                       y=0xfffffff0, c=0xfffffffc, yn=0xffffffff, cn=0x80000001)))

    def test_mfd_source_pins_admission_budget_and_store_bounds(self):
        valid = FW.FirmwareMfdSourceTests.record()
        for low, high, _ in MFD_SOURCE_BODIES:
            for offset in (low, high - 1):
                changed = bytearray(Model.payload)
                changed[offset] ^= 1
                with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                    with self.assertRaisesRegex(ValueError, "stock MFD source body"):
                        execute_mfd_source(valid, payload=changed)
                    spawn.assert_not_called()
        for offset in (*MFD_SOURCE_WORDS, 0x2cccc, 0x2ccef):
            changed = bytearray(Model.payload)
            changed[offset] ^= 1
            with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "stock MFD source (literal|table)"):
                    execute_mfd_source(valid, payload=changed)
                spawn.assert_not_called()
        class BytesSubclass(bytes):
            pass
        malformed = [b"", bytes(115), bytes(117), list(valid), BytesSubclass(valid)]
        for offset, value in ((0x5c, 3), (0x27, 0), (0x27, 4), (8, 3), (0x28, 2)):
            changed = bytearray(valid)
            changed[offset] = value
            malformed.append(changed)
        for record in malformed:
            with self.subTest(record=type(record)), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid synthetic MFD record"):
                    execute_mfd_source(record)
                spawn.assert_not_called()
        for budget in (0, True, 257):
            with self.subTest(budget=budget), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid instruction budget"):
                    execute_mfd_source(valid, budget=budget)
                spawn.assert_not_called()
        with self.assertRaisesRegex(ValueError, "instruction budget exceeded"):
            execute_mfd_source(valid, budget=1)
        with mock.patch.dict(MFD_SOURCE_CALLS, {0x19a0: 0x203c4}), mock.patch.object(subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(ValueError, "stock MFD source call target"):
                execute_mfd_source(valid)
            spawn.assert_not_called()
        original = RSP.registers
        def outside(rsp):
            regs = original(rsp)
            if regs[15] == 0x1e8ec:
                regs[3] = 0x900000
            return regs
        with mock.patch.object(RSP, "registers", outside):
            with self.assertRaisesRegex(ValueError, "outside ordered MFD register-write"):
                execute_mfd_source(valid)


class FirmwareSourceProducerQemuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        FirmwareQemuTests.setUpClass()

    def test_actual_producer_restores_physical_words_before_mfd(self):
        for cleared in (False, True):
            with self.subTest(cleared=cleared):
                actual = execute_source_producer(cleared=cleared)
                self.assertEqual(actual["pages"], actual["expected"])
                self.assertEqual(actual["before_copy"], [0x940040, 0x940100])
                self.assertEqual(struct.unpack_from("<2I", actual["picture"], 0x34), (0x920040, 0x920100))
                self.assertEqual(actual["status"], 0x920100)
                self.assertEqual(actual["real_status"], [(0x1fdac, 0), (0x1fdac, 0), (0xd880, 1)])
                for base in (0x400000, 0x401000, 0x600000):
                    self.assertEqual(actual["pages"][base], bytes(actual["initial"][base]))
                mfd = execute_mfd_source(actual["picture"][:116])
                self.assertEqual(mfd["writes"], [[0x540010, 0x00800040], [0x540028, 16],
                                                [0x54002c, 6], [0x54001c, 0x920040], [0x540020, 0x920100]])

    def test_actual_null_producer_preserves_unwritten_caller_fields(self):
        for cleared in (False, True):
            with self.subTest(cleared=cleared):
                actual = execute_source_producer(null=True, cleared=cleared)
                self.assertEqual((actual["steps"], actual["status"], actual["calls"]), (44, 0, []))
                self.assertEqual(actual["picture"][0x34:0x3c], bytes(8))
                fill = 0 if cleared else 0xa5
                for low in (0x14, 0x54, 0x6c):
                    self.assertEqual(actual["picture"][low:low + 8], bytes([fill]) * 8)
                if cleared:
                    self.assertEqual(execute_mfd_source(actual["picture"][:116])["writes"][-2:],
                                     [[0x54001c, 0], [0x540020, 0]])

    def test_source_producer_pins_admission_and_translation_bounds(self):
        for low, high, _ in SOURCE_BODIES:
            for offset in (low, high - 1):
                changed = bytearray(Model.payload)
                changed[offset] ^= 1
                with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                    with self.assertRaisesRegex(ValueError, "stock source producer body"):
                        execute_source_producer(payload=changed)
                    spawn.assert_not_called()
        for offset in (0xe2e0, 0x2dcf6, 0x2dd05):
            changed = bytearray(Model.payload)
            changed[offset] ^= 1
            with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "stock source producer table/literal"):
                    execute_source_producer(payload=changed)
                spawn.assert_not_called()
        for options in (dict(null=0), dict(cleared=1), dict(payload="bad"),
                        dict(budget=True), dict(budget=0), dict(budget=257)):
            with self.subTest(options=options), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(ValueError, "invalid"):
                    execute_source_producer(**options)
                spawn.assert_not_called()
        with self.assertRaisesRegex(ValueError, "instruction budget exceeded"):
            execute_source_producer(budget=1)
        with mock.patch.dict(SOURCE_CALLS, {0xe144: 0xd92c}), mock.patch.object(subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(ValueError, "stock source producer call target"):
                execute_source_producer()
            spawn.assert_not_called()
        original = RSP.registers
        def outside(rsp):
            regs = original(rsp)
            if regs[15] == 0x1fdcc:
                regs[2] = 0x900000
            return regs
        with mock.patch.object(RSP, "registers", outside):
            with self.assertRaisesRegex(ValueError, "outside ordered source translation"):
                execute_source_producer()
        def wrong_argument(rsp):
            regs = original(rsp)
            if regs[15] == 0xd9c4:
                regs[2] = 1
            return regs
        with mock.patch.object(RSP, "registers", wrong_argument):
            with self.assertRaisesRegex(ValueError, "source geometry/format contract"):
                execute_source_producer()


class ProtocolTests(unittest.TestCase):
    class Connection:
        def __init__(self, response=b""):
            self.response = response
            self.sent = []

        def recv(self, count):
            result, self.response = self.response[:count], self.response[count:]
            return result

        def sendall(self, data):
            self.sent.append(data)

        def settimeout(self, timeout):
            pass

    def test_packet_refusals(self):
        for data in (b"-", b"+$hello#00", b"+$}#7d", b"+$" + b"a" * 16385):
            with self.subTest(data=data[:16]), self.assertRaises(ValueError):
                RSP(self.Connection(data), time.monotonic() + 2).packet("g")

    def test_wall_deadline_covers_partial_reads_and_sends(self):
        connection = self.Connection(b"ab")
        rsp = RSP(connection, 1)
        # Expiry between two partial reads must prevent the second recv.
        with mock.patch.object(time, "monotonic", side_effect=[0.5, 1.1]):
            with mock.patch.object(connection, "recv", return_value=b"a") as receive:
                with self.assertRaisesRegex(TimeoutError, "wall deadline"):
                    rsp.receive(2)
                self.assertEqual(receive.call_count, 1)
        with mock.patch.object(time, "monotonic", return_value=1.1):
            with self.assertRaisesRegex(TimeoutError, "wall deadline"):
                rsp.send(b"+")
        self.assertEqual(connection.sent, [])


class FirmwareSourceModeQemuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        FirmwareQemuTests.setUpClass()

    def check(self, actual):
        self.assertEqual(actual["pages"], actual["expected"])
        self.assertEqual(actual["pages"][0x401000], bytes(actual["initial"][0x401000]))
        self.assertEqual(actual["real_status"],
                         [(0xded4, actual["status"])] if actual["selected"] else [])
        self.assertLessEqual(actual["steps"], 112)
        return actual

    def test_actual_mode_thresholds_and_metadata_flags(self):
        # The +0c word's dimensions/units are not authenticated by this replay.
        for span in (720, 721, 1280, 1281):
            for kind in (0, 1, 4):
                for flags in (0, 4, 8, 12):
                    with self.subTest(span=span, kind=kind, flags=flags):
                        actual = self.check(execute_source_mode(flags, span, kind))
                        selected = bool(flags & 4) if kind == 0 else \
                            (span in (720, 1281) and (bool(flags & 12) or kind == 4))
                        self.assertEqual(actual["selected"], selected)
                        self.assertEqual((actual["mode"], actual["field"], actual["latch"]), (2, 1, 255))
        for flags, mode in ((6, 0), (7, 1)):
            actual = self.check(execute_source_mode(flags))
            self.assertEqual((actual["mode"], actual["field"], actual["latch"]), (mode, 0, 255))

    def test_actual_shared_latch_initialization_and_alternation(self):
        for flags, first in ((5, 0), (21, 1)):
            latch = 255
            for position in range(4):
                with self.subTest(flags=flags, position=position):
                    actual = self.check(execute_source_mode(flags, latch=latch))
                    self.assertEqual((actual["mode"], actual["field"]), (first ^ (position & 1), 0))
                    self.assertEqual(len(actual["latch_writes"]), 2 if position == 0 else 1)
                    latch = actual["latch"]
        # Preserve a pre-existing latch through the non-selection branch.
        for latch in (0, 1, 2, 254):
            actual = self.check(execute_source_mode(5, 721, 1, latch))
            self.assertEqual((actual["mode"], actual["field"], actual["latch_writes"], actual["latch"]),
                             (2, 1, [], latch))
        actual = self.check(execute_source_mode(5, latch=2))
        self.assertEqual((actual["mode"], actual["field"], actual["latch_writes"]), (0, 0, [0]))

    def test_mode_pins_inputs_budget_and_latch_target(self):
        for low, high, _ in SOURCE_MODE_BODIES:
            for offset in (low, high - 1):
                changed = bytearray(Model.payload)
                changed[offset] ^= 1
                with self.subTest(offset=offset), mock.patch.object(subprocess, "Popen") as spawn:
                    with self.assertRaisesRegex(ValueError, "stock source mode body"):
                        execute_source_mode(payload=changed)
                    spawn.assert_not_called()
        changed = bytearray(Model.payload)
        changed[0xe25c] ^= 1
        with mock.patch.object(subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(ValueError, "stock source mode literal"):
                execute_source_mode(payload=changed)
            spawn.assert_not_called()
        with mock.patch.dict(SOURCE_MODE_CALLS, {0xe03c: 0x203c4}), \
                mock.patch.object(subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(ValueError, "stock source mode call target"):
                execute_source_mode()
            spawn.assert_not_called()
        for options in ({"flags": True}, {"flags": -1}, {"span": 1 << 32}, {"kind": 0.0},
                        {"latch": True}, {"latch": 256}, {"budget": True}, {"budget": 0},
                        {"budget": 257}, {"payload": []}):
            with self.subTest(options=options), mock.patch.object(subprocess, "Popen") as spawn:
                with self.assertRaises(ValueError):
                    execute_source_mode(**options)
                spawn.assert_not_called()
        with self.assertRaisesRegex(ValueError, "instruction budget exceeded"):
            execute_source_mode(budget=1)
        original = RSP.registers
        def outside(rsp):
            regs = original(rsp)
            if regs[15] == 0xdf8c:
                regs[1] = SOURCE_MODE_LATCH + 1
            return regs
        with mock.patch.object(RSP, "registers", outside):
            with self.assertRaisesRegex(ValueError, "outside ordered source mode latch"):
                execute_source_mode(5)


if __name__ == "__main__":
    unittest.main(defaultTest=("FirmwareQemuTests", "FirmwarePictureQemuTests",
                               "FirmwareMfdSourceQemuTests", "FirmwareSourceProducerQemuTests",
                               "FirmwareSourceModeQemuTests", "ProtocolTests"))
