#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Hardware-free differential execution of a pinned stock A32 allocation body.

Only the exact body and its literals are guest code. Four opaque callees use
synthetic contracts shared with the Python reference, not real allocators.
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


def elf(payload, pages):
    body = payload[Model.START:Model.END]
    if hashlib.sha256(body).hexdigest() != Model.DIGEST:
        raise ValueError("stock allocation body changed")
    code = bytearray(struct.pack("<I", 0xe7f000f0) * (0x20000 // 4))
    code[Model.START - RETURN:Model.END - RETURN] = body
    segments = [(RETURN, bytes(code), 5)] + [(base, bytes(page), 6) for base, page in pages.items()]
    image = bytearray(4096)
    image[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into("<HHIIIIIHHHHHH", image, 16, 2, 40, 1, Model.START, 52,
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
                preserved = registers[4:12]
                calls, allocations, stub_status = [], {}, []
                steps, last_pc = 0, None
                while True:
                    registers = rsp.registers()
                    pc = registers[15]
                    if pc == RETURN:
                        if expected_signal is not None:
                            raise ValueError("expected guest signal did not occur")
                        if registers[4:12] != preserved or registers[13] != Model.SP:
                            raise ValueError("stock callee-saved ABI changed")
                        observed = {base: rsp.memory(base, size=4096) for base in pages}
                        outputs = {offset: struct.unpack_from("<I", observed[Model.H], offset)[0]
                                   for offset in Model.OUTPUTS}
                        return dict(status=registers[0], outputs=outputs, calls=calls,
                                    pages=observed, initial=pages, steps=steps, stub_status=stub_status)
                    if pc in CALLEES:
                        if last_pc is None or registers[14] != last_pc + 4:
                            raise ValueError("unexpected synthetic call ABI")
                        args = tuple(registers[:4])
                        calls.append((last_pc, pc, args))
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
                        stub_status.append((pc, result))
                        for index, value in ((0, result), (1, 0xd1d1d1d1), (2, 0xd2d2d2d2),
                                             (3, 0xd3d3d3d3), (12, 0xdcdcdcdc), (15, registers[14])):
                            rsp.register(index, value)
                        rsp.register(25, flags)
                        last_pc = None
                        continue
                    if pc & 3 or not (Model.START <= pc < 0x26040 or 0x26134 <= pc < Model.END):
                        raise ValueError("outside pinned executable slices")
                    if steps >= budget:
                        raise ValueError("stock instruction budget exceeded")
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


if __name__ == "__main__":
    unittest.main(defaultTest=("FirmwareQemuTests", "ProtocolTests"))
