#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""CPU-only H264 reference lifecycle regression; never accesses a device.

Distinct non-reference I_PCM pictures challenge a coded long-term reference.
Passing pixels or reference syntax does not establish a physical surface lease.
Only the three valid streams and their fixed-profile pixel oracles can be emitted.
"""

import sys

if not __debug__:
    raise SystemExit("assertion-enabled Python is required")

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import xml.etree.ElementTree as ET
import zlib


WIDTH, HEIGHT, FRAMES = 256, 96, 12
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2
TIMEOUT = 30
FRAME_NUM = (0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6)
NAL_HEADERS = (0x65, 0x01, 0x21, 0x01, 0x21, 0x01,
               0x21, 0x01, 0x21, 0x01, 0x21, 0x01)
SOURCE_FRAMES = (None, None, 0, None, 0, None,
                 None, None, 6, None, None, 10)
PICTURE_TYPES = ("I", "I", "P", "I", "P", "I",
                 "I", "I", "P", "I", "I", "P")
VARIANTS = (
    ("explicit-replace-retire", True, True, 296890,
     "222f72a842ca73eaa4d4b0da73aae2ba55c80b7b704aad7066c305039de8c7c5"),
    ("implicit-replace-retire", False, True, 296889,
     "9afb972c4a7a9108a7dcfe2e1f49eaa0aaa0f4cd0525a7b5afae027c7ea076bc"),
    ("implicit-replace-keep", False, False, 296888,
     "2ee4aa7ffbf289ec58e4a7b45b4695feb187b5817ebe1b00bd217fc71c25eccd"),
)
ORACLE_SHA256 = "778b253f7847a7ea194761ee149b6c049e1958fe7f286248c6284b3a0c61f88e"
PACKED_SHA256 = "cce639ad821e1dcafc998582d9d172cca628b9f8295672432ccb30e92fe8b4a5"


class Checks:
    def __init__(self):
        self.count = 0

    def require(self, condition, message):
        self.count += 1
        if not condition:
            raise AssertionError(message)


class Bits:
    def __init__(self):
        self.data, self.used = bytearray(), 0

    def u(self, value, width):
        assert 0 <= width <= 32 and 0 <= value < 1 << width
        for shift in range(width - 1, -1, -1):
            if not self.used:
                self.data.append(0)
            self.data[-1] |= ((value >> shift) & 1) << (7 - self.used)
            self.used = (self.used + 1) % 8

    def ue(self, value):
        assert 0 <= value < (1 << 31) - 1
        code = value + 1
        self.u(0, code.bit_length() - 1)
        self.u(code, code.bit_length())

    def se(self, value):
        self.ue(2 * abs(value) - (value > 0))

    def align(self):
        if self.used:
            self.u(0, 8 - self.used)

    def bytes(self, payload):
        assert self.used == 0
        self.data.extend(payload)

    def finish(self):
        self.u(1, 1)
        self.align()
        return bytes(self.data)


def annexb(header, rbsp):
    escaped, zeros = bytearray(), 0
    for value in rbsp:
        if zeros == 2 and value <= 3:
            escaped.append(3)
            zeros = 0
        escaped.append(value)
        zeros = zeros + 1 if value == 0 else 0
    return b"\x00\x00\x00\x01" + bytes((header,)) + escaped


def sps():
    b = Bits()
    b.u(66, 8); b.u(0x80, 8); b.u(30, 8)  # Baseline, Level 3.0.
    b.ue(0); b.ue(0); b.ue(2); b.ue(3)  # Four-bit frame_num, POC2, three refs.
    b.u(0, 1); b.ue(15); b.ue(5)  # No gaps, progressive 256x96.
    b.u(1, 1); b.u(1, 1); b.u(0, 1); b.u(1, 1)
    b.u(1, 1); b.u(1, 8); b.u(0, 1)  # Square samples, no overscan.
    b.u(1, 1); b.u(5, 3); b.u(0, 1); b.u(0, 1); b.u(0, 1)
    b.u(1, 1); b.u(1, 32); b.u(60, 32); b.u(1, 1)  # 30 fps.
    b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(0, 1)
    return annexb(0x67, b.finish())


def pps():
    b = Bits()
    b.ue(0); b.ue(0); b.u(0, 1); b.u(0, 1)
    b.ue(0); b.ue(0); b.ue(0); b.u(0, 1); b.u(0, 2)
    b.se(0); b.se(0); b.se(0)
    b.u(1, 1); b.u(0, 1); b.u(0, 1)  # CAVLC; deblocking control present.
    return annexb(0x68, b.finish())


def sample(k, x, y, plane):
    if plane == 0:
        return 16 + (17*x + 29*y + 11*k + (x ^ (y + 7*k))) % 220
    if plane == 1:
        return 16 + (19*x + 31*y + 23*k + (x ^ (y + 3*k))) % 225
    return 16 + (47*x + 13*y + 19*k + ((3*x) ^ (5*y + k))) % 225


def marking(b, operations):
    b.u(bool(operations), 1)
    if operations:
        for operation, argument in operations:
            assert operation in (2, 6) and argument == 0
            b.ue(operation); b.ue(argument)
        b.ue(0)


def intra(frame_num, pattern, reference=True, idr=False, operations=()):
    assert 0 <= frame_num <= 15 and 0 <= pattern <= 7
    b = Bits()
    b.ue(0); b.ue(2); b.ue(0); b.u(frame_num, 4)
    if idr:
        assert reference and frame_num == 0
        b.ue(0); b.u(0, 1); b.u(1, 1)  # Initial picture becomes LT0.
    elif reference:
        marking(b, operations)
    else:
        assert not operations
    b.se(0); b.ue(1)  # Deblocking disabled, including all I_PCM boundaries.
    for my in range(6):
        for mx in range(16):
            b.ue(25); b.align()
            for plane in range(3):
                side = 16 if plane == 0 else 8
                b.bytes(bytes(sample(pattern, side*mx+c, side*my+r, plane)
                              for r in range(side) for c in range(side)))
    return annexb(0x65 if idr else 0x21 if reference else 0x01, b.finish())


def predict(frame_num, reference=True, dx=0, long_term=True):
    assert 0 <= frame_num <= 15 and dx in (-16, 0, 16)
    b = Bits()
    b.ue(0); b.ue(0); b.ue(0); b.u(frame_num, 4)
    b.u(0, 1); b.u(long_term, 1)
    if long_term:
        b.ue(2); b.ue(0); b.ue(3)  # Explicit L0 long_term_pic_num0.
    if reference:
        marking(b, ())
    b.se(0); b.ue(1)
    for macroblock in range(96):
        b.ue(0); b.ue(0)  # Explicit P_L0_16x16, one reference, no residual.
        b.se(4*dx if macroblock == 0 else 0); b.se(0); b.ue(0)
    return annexb(0x21 if reference else 0x01, b.finish())


def make_trial(explicit_replace, retire, missing_reference_probe=False):
    replacement = ((2, 0), (6, 0)) if explicit_replace else ((6, 0),)
    retirement = ((2, 0),) if retire else ()
    slices = [intra(0, 0, idr=True), intra(1, 1, False), predict(1, dx=16),
              intra(2, 3, False), predict(2, dx=-16), intra(3, 4, False),
              intra(3, 2, operations=replacement), intra(4, 5, False),
              predict(4, dx=16), intra(5, 6, False),
              intra(5, 7, operations=retirement),
              predict(6, False, long_term=missing_reference_probe)]
    units = []
    for index, nal in enumerate(slices):
        units.append(annexb(9, bytes((0x10 if index in (0, 1, 3, 5, 6, 7, 9, 10)
                                     else 0x30,))))
        if index == 0:
            units.extend((sps(), pps()))
        units.append(nal)
    payload = b"".join(units)
    assert len(payload) <= 300000 and len(slices) == FRAMES
    return payload, slices


class Reader:
    def __init__(self, payload):
        self.payload, self.bit = payload, 0

    def u(self, width):
        assert 0 <= width <= 32 and self.bit + width <= len(self.payload)*8
        value = 0
        for _ in range(width):
            value = (value << 1) | ((self.payload[self.bit//8] >> (7-self.bit % 8)) & 1)
            self.bit += 1
        return value

    def ue(self):
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
            assert zeros <= 31
        return (1 << zeros) - 1 + self.u(zeros)


def parse_header(nal):
    assert nal[:4] == b"\x00\x00\x00\x01" and nal[4] in (0x65, 0x21, 0x01)
    r = Reader(nal[5:].replace(b"\0\0\3", b"\0\0"))
    assert r.ue() == 0
    slice_type = r.ue()
    assert slice_type in (0, 2) and r.ue() == 0
    frame_num, operations, selected = r.u(4), [], None
    if slice_type == 0:
        assert r.u(1) == 0
        if r.u(1):
            assert r.ue() == 2
            selected = ("LT", r.ue())
            assert r.ue() == 3
        else:
            selected = ("ST", "newest")
    if nal[4] == 0x65:
        assert r.ue() == 0 and r.u(1) == 0 and r.u(1) == 1
    elif nal[4] >> 5 and r.u(1):
        for _ in range(4):
            operation = r.ue()
            if operation == 0:
                break
            assert operation in (2, 6)
            operations.append((operation, r.ue()))
        else:
            raise AssertionError("MMCO budget exceeded")
    assert r.ue() == 0 and r.ue() == 1  # Zero QP delta, disabled deblocking.
    reference = bool(nal[4] >> 5)
    return {"nal": nal[4], "frame_num": frame_num, "reference": reference,
            "poc": 0 if nal[4] == 0x65 else 2*frame_num - int(not reference),
            "mmco": operations, "l0": selected}


def reference_model(headers):
    long_term, short_term, selected, snapshots = {}, [], [], []
    valid = True
    for index, header in enumerate(headers):
        source = None
        if header["l0"] is not None:
            source = (long_term.get(header["l0"][1]) if header["l0"][0] == "LT"
                      else short_term[-1] if short_term else None)
            valid = valid and source is not None
        selected.append(source)
        if header["reference"]:
            if header["nal"] == 0x65:
                long_term, short_term = {0: index}, []
            else:
                current_long_term = None
                for operation, argument in header["mmco"]:
                    long_term.pop(argument, None)
                    if operation == 6:
                        current_long_term = argument
                if current_long_term is None:
                    if not header["mmco"] and len(long_term) + len(short_term) >= 3:
                        assert short_term
                        short_term.pop(0)
                    short_term.append(index)
                else:
                    long_term[current_long_term] = index
            assert len(long_term) + len(short_term) <= 3
        snapshots.append({"lt": dict(long_term), "st": list(short_term)})
    return valid, selected, snapshots


def oracle_source(k):
    # Independent row-major oracle: never calls sample() or uses decoded pixels.
    y = bytes(16 + (29*r + 17*c + 11*k + (c ^ (7*k+r))) % 220
              for r in range(96) for c in range(256))
    cb = bytes(16 + (31*r + 19*c + 23*k + (c ^ (3*k+r))) % 225
               for r in range(48) for c in range(128))
    cr = bytes(16 + (13*r + 47*c + 19*k + ((5*r+k) ^ (3*c))) % 225
               for r in range(48) for c in range(128))
    return y + cb + cr


def oracle_view(pixels, dx):
    return b"".join(bytes(pixels[offset+r*width+max(0, min(width-1, c+shift))]
                          for r in range(height) for c in range(width))
                    for offset, width, height, shift in
                    ((0, 256, 96, dx), (24576, 128, 48, dx//2), (30720, 128, 48, dx//2)))


def packed_yuy2(planar):
    """Empirically calibrated 256x96 unscaled stock MFD packing, not universal SCL."""
    # Whole prior 180-frame stock capture matched this half-up/clamped 3:1 rule:
    # planar e68e26f2a6cf0cd8219858411cc45fcc13ea702042822940e205104db42ae327
    # packed 1ba4af890ad878a5472777c873f1f86f070264ebfc33994ba719b22ea5df9068
    # The new output pin predates capture: never refit it to future observations.
    if type(planar) not in (bytes, bytearray) or len(planar) != FRAMES*FRAME_BYTES:
        raise ValueError("packing requires exactly twelve fixed-profile planar frames")
    output = bytearray(FRAMES*WIDTH*HEIGHT*2)
    for frame in range(FRAMES):
        source_base, output_base = frame*FRAME_BYTES, frame*WIDTH*HEIGHT*2
        for y in range(HEIGHT):
            row = output_base + y*WIDTH*2
            output[row:row+WIDTH*2:2] = planar[source_base+y*WIDTH:source_base+(y+1)*WIDTH]
            q = y//2
            adjacent = max(0, q-1) if y % 2 == 0 else min(HEIGHT//2-1, q+1)
            for x in range(WIDTH//2):
                for offset, lane in ((24576, 1), (30720, 3)):
                    current = planar[source_base+offset+q*(WIDTH//2)+x]
                    neighbor = planar[source_base+offset+adjacent*(WIDTH//2)+x]
                    output[row+4*x+lane] = (3*current + neighbor + 2)//4
    return bytes(output)


def check_packing(checks, planar, packed):
    checks.require(len(packed) == 589824, "bounded packed oracle")
    checks.require(hashlib.sha256(packed).hexdigest() == PACKED_SHA256, "preselected packed pin")
    checks.require(packed[::2] == b"".join(planar[i*FRAME_BYTES:i*FRAME_BYTES+WIDTH*HEIGHT]
                                         for i in range(FRAMES)), "every packed Y lane preserved")
    fixture = bytearray(FRAME_BYTES)
    for q in range(HEIGHT//2):
        cb = {0: 10, 1: 12, 2: 11, 46: 252, 47: 254}.get(q, 100)
        cr = {0: 250, 1: 252, 2: 253, 46: 2, 47: 0}.get(q, 140)
        fixture[24576+q*128:24576+(q+1)*128] = bytes((cb,))*128
        fixture[30720+q*128:30720+(q+1)*128] = bytes((cr,))*128
    fixture *= FRAMES
    original = bytes(fixture)
    result = packed_yuy2(fixture)
    for y, pair in ((0, (10, 250)), (1, (11, 251)), (2, (12, 252)),
                    (94, (254, 1)), (95, (254, 0))):
        checks.require((result[y*512+1], result[y*512+3]) == pair,
                       "chroma row parity, half-up tie and edge clamp")
    checks.require(bytes(fixture) == original and type(result) is bytes, "detached immutable packing")
    refused = []
    for invalid in (b"", original[:-1], original+b"\0", memoryview(original), True):
        try:
            packed_yuy2(invalid)
        except ValueError:
            refused.append(True)
        else:
            refused.append(False)
    checks.require(all(refused), "strict packing type and exact length refusal")


def decode(payload):
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
               "-threads", "1", "-hwaccel", "none", "-err_detect", "explode", "-xerror",
               "-f", "h264", "-i", "pipe:0", "-map", "0:v:0", "-an", "-sn", "-dn",
               "-fps_mode", "passthrough", "-threads", "1", "-pix_fmt", "yuv420p",
               "-f", "rawvideo", "pipe:1"]
    return subprocess.run(command, input=payload, capture_output=True, timeout=TIMEOUT)


def check_native_fixture(checks, outputs, planar):
    """Validate saved finite results and native luma thumbnails without a device."""
    folder = Path(__file__).resolve().parent / "fixtures/issue92"
    raw = (folder / "native-coded-reference.json").read_bytes()
    svg = (folder / "native-coded-reference.svg").read_bytes()
    checks.require(hashlib.sha256(raw).hexdigest() ==
                   "e232ef1b0f3fa68cfa32abe700715534f3e8b2900446df4c039342f968d0362e", "native receipt pin")
    checks.require(hashlib.sha256(svg).hexdigest() ==
                   "af11d8eef735be03bbc0a995f5c0977d470b0c781bb61d54964b73f214d2a595", "native visual pin")
    data = json.loads(raw)
    checks.require((data["schema_version"], data["kind"]) ==
                   (1, "native-coded-reference-lifecycle"), "native receipt schema")
    checks.require(data["frame_num"] == list(FRAME_NUM) and data["poc"] == list(range(FRAMES)) and
                   data["nal_headers_hex"] == [format(n, "02x") for n in NAL_HEADERS] and
                   data["reference"] == [i % 2 == 0 for i in range(FRAMES)], "native input syntax")
    for variant, (name, payload), (label, explicit, retire, size, digest) in zip(
            data["variants"], outputs, VARIANTS):
        checks.require((variant["name"], name, variant["input_bytes"], variant["input_sha256"]) ==
                       (label, "lifecycle-" + label + ".h264", len(payload), digest), "native stimulus receipt")
        checks.require(variant["frame6_mmco"] == ([2, 6] if explicit else [6]) and
                       variant["frame10_mmco"] == ([2] if retire else []), "native MMCO receipt")
    checks.require(len(data["variants"]) == len(outputs) == 3, "three native variants")
    oracle = data["oracle"]
    checks.require((oracle["planar_bytes"], oracle["planar_sha256"], oracle["packed_bytes"],
                    oracle["packed_sha256"]) == (len(planar), ORACLE_SHA256, 589824, PACKED_SHA256), "native oracle receipt")
    checks.require(oracle["preselected_before_native_capture"] is True and
                   oracle["same_pixels_for_all_valid_variants"] is True, "positive pixel inference boundary")
    controls = (("before", 32, 14745600, "021b6736caed04600c4801ca1b0e30dc4a48b60985e2dff02d38bea7c2aa9244"),
                ("a", 12, 589824, PACKED_SHA256), ("b", 12, 589824, PACKED_SHA256),
                ("c", 12, 589824, PACKED_SHA256),
                ("after", 180, 4423680, "d72c16b7eb12d844fb6a5805c2d33a120874237ac5f1cf3608d4bebe186ee7cd"))
    checks.require(len(data["sessions"]) == 5, "five normal native sessions")
    for session, expected in zip(data["sessions"], controls):
        checks.require(tuple(session[k] for k in ("id", "frames", "capture_bytes", "capture_sha256")) ==
                       expected, "whole native capture pins")
        checks.require(all(session[k] is True for k in ("whole_capture_equal", "firmware_eos", "cleanup")) and
                       all(session[k] == 0 for k in ("exit_code", "pending", "ready", "stderr_bytes", "new_kernel_errors")),
                       "native output EOS and cleanup")
        checks.require((session["fd_before_after"], session["threads_before_after"], session["kernel_normal_lines"]) ==
                       ([3, 3], [1, 1], 3), "native resource and kernel receipts")
    checks.require((sum(s["frames"] for s in data["sessions"]), sum(s["capture_bytes"] for s in data["sessions"])) ==
                   (248, 20938752), "native total frames and bytes")
    checks.require(all(data["summary"][k] is False for k in
                       ("added_ppb_or_dram_observer", "diagnostic_target_writes", "invalid_reference_stream_sent")),
                   "normal native input boundary")
    tree = ET.fromstring(svg)
    ns = {"svg": "http://www.w3.org/2000/svg"}
    groups = tree.findall("svg:g", ns)
    pictures = [g for g in groups if "data-frame" in g.attrib]
    checks.require(len(pictures) == len(data["thumbnails"]) == 4, "four native luma pictures")
    for group, thumbnail in zip(pictures, data["thumbnails"]):
        index = int(group.attrib["data-frame"])
        y = planar[index*FRAME_BYTES:index*FRAME_BYTES+WIDTH*HEIGHT]
        checks.require(index == thumbnail["frame"] and hashlib.sha256(y).hexdigest() ==
                       group.attrib["data-luma-sha256"] == thumbnail["luma_sha256"], "native whole luma provenance")
        uri = group.find("svg:image", ns).attrib["href"]
        checks.require(uri.startswith("data:image/png;base64,"), "embedded native PNG only")
        png = base64.b64decode(uri.split(",", 1)[1], validate=True)
        checks.require(len(png) == thumbnail["png_bytes"] and hashlib.sha256(png).hexdigest() ==
                       group.attrib["data-png-sha256"] == thumbnail["png_sha256"], "native thumbnail byte pin")
        checks.require(png[:8] == b"\x89PNG\r\n\x1a\n", "PNG signature")
        offset, chunks = 8, []
        while offset < len(png):
            size = struct.unpack_from(">I", png, offset)[0]
            kind, body = png[offset+4:offset+8], png[offset+8:offset+8+size]
            checks.require(zlib.crc32(kind+body) & 0xffffffff == struct.unpack_from(">I", png, offset+8+size)[0],
                           "PNG chunk CRC")
            chunks.append((kind, body))
            offset += size+12
        checks.require(offset == len(png) and [k for k, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"] and
                       chunks[0][1] == struct.pack(">IIBBBBB", 128, 48, 8, 0, 0, 0, 0) and chunks[-1][1] == b"", "bounded gray PNG")
        expected = b"".join(b"\0" + y[row*WIDTH:row*WIDTH+WIDTH:2] for row in range(0, HEIGHT, 2))
        checks.require(zlib.decompress(chunks[1][1]) == expected, "every displayed native luma sample")
    timeline = [g for g in groups if "data-au" in g.attrib]
    checks.require([(int(g.attrib["data-au"]), int(g.attrib["data-frame-num"]), int(g.attrib["data-poc"]),
                     g.attrib["data-reference"]) for g in timeline] ==
                   [(i, FRAME_NUM[i], i, str(i % 2 == 0).lower()) for i in range(FRAMES)], "visual input timeline")
    text = " ".join(tree.itertext())
    checks.require("do not prove retirement/freeing" in text and "physical/raw-source lease" in text and
                   any("identical pixels" in s for s in data["limits"]), "native visual inference limits")
    checks.require(not any(p in raw.decode()+svg.decode() for p in ("/home/", "/tmp/", "file://")), "public-safe receipts")


def validate():
    checks = Checks()
    a, b, c = oracle_source(0), oracle_source(2), oracle_source(7)
    expected_frames = [a, oracle_source(1), oracle_view(a, 16), oracle_source(3),
                       oracle_view(a, -16), oracle_source(4), b, oracle_source(5),
                       oracle_view(b, 16), oracle_source(6), c, c]
    expected = b"".join(expected_frames)
    checks.require(len(expected) == FRAMES*FRAME_BYTES, "bounded planar oracle")
    checks.require(hashlib.sha256(expected).hexdigest() == ORACLE_SHA256, "oracle pin")
    challenges = (a, b, c, *(expected_frames[i] for i in (1, 3, 5, 7, 9)))
    checks.require(len({hashlib.sha256(p).digest() for p in challenges}) == 8,
                   "all non-reference overwrite patterns differ from A/B/C")
    packed = packed_yuy2(expected)
    check_packing(checks, expected, packed)
    outputs, receipts = [], []
    for name, explicit_replace, retire, size, digest in VARIANTS:
        payload, slices = make_trial(explicit_replace, retire)
        checks.require(len(payload) == size, "source byte pin")
        checks.require(hashlib.sha256(payload).hexdigest() == digest, "source SHA256 pin")
        headers = [parse_header(nal) for nal in slices]
        for index, header in enumerate(headers):
            checks.require(header["nal"] == NAL_HEADERS[index], "NAL header")
            checks.require(header["frame_num"] == FRAME_NUM[index], "reference frame_num")
            checks.require(header["reference"] == (index % 2 == 0), "reference bit")
            checks.require(header["poc"] == index, "monotonic POC2")
            operations = ([(2, 0), (6, 0)] if explicit_replace else [(6, 0)]) if index == 6 else (
                [(2, 0)] if index == 10 and retire else [])
            checks.require(header["mmco"] == operations, "exact MMCO sequence")
            l0 = ("LT", 0) if index in (2, 4, 8) else ("ST", "newest") if index == 11 else None
            checks.require(header["l0"] == l0, "exact L0 selector")
        valid, selected, snapshots = reference_model(headers)
        checks.require(valid and tuple(selected) == SOURCE_FRAMES, "reference source IDs")
        checks.require(snapshots[10]["lt"] == ({} if retire else {0: 6}), "LT retirement model")
        decoded = decode(payload)
        checks.require(decoded.returncode == 0 and decoded.stderr == b"", "valid FFmpeg decode")
        checks.require(decoded.stdout == expected, "every planar pixel byte")
        probe = subprocess.run(["ffprobe", "-v", "error", "-f", "h264", "-select_streams", "v:0",
                                "-show_entries", "frame=key_frame,width,height,pict_type",
                                "-of", "json", "pipe:0"], input=payload,
                               capture_output=True, timeout=TIMEOUT)
        checks.require(probe.returncode == 0 and probe.stderr == b"", "valid FFprobe decode")
        frames = json.loads(probe.stdout)["frames"]
        checks.require(len(frames) == FRAMES, "all twelve frames")
        checks.require(all((f["width"], f["height"]) == (WIDTH, HEIGHT) for f in frames), "dimensions")
        checks.require(tuple(f["key_frame"] for f in frames) == (1,) + (0,)*11, "only initial IDR")
        checks.require(tuple(f["pict_type"] for f in frames) == PICTURE_TYPES, "picture types")
        outputs.append(("lifecycle-" + name + ".h264", payload))
        receipts.append({"variant": name, "bytes": size, "sha256": digest, "frames": len(frames),
                         "ffmpeg_exit": decoded.returncode, "ffprobe_exit": probe.returncode,
                         "stderr_bytes": [len(decoded.stderr), len(probe.stderr)]})
    negative_receipts = []
    for retire in (True, False):
        # The retired LT0 selector is intentionally invalid and is NEVER emitted.
        payload, slices = make_trial(False, retire, missing_reference_probe=True)
        valid, selected, _ = reference_model([parse_header(nal) for nal in slices])
        decoded = decode(payload)
        checks.require(valid == (not retire), "missing-reference model distinction")
        checks.require(len(decoded.stdout) == FRAMES*FRAME_BYTES, "CPU probe frame bytes")
        checks.require(decoded.stdout[:11*FRAME_BYTES] == expected[:11*FRAME_BYTES], "CPU probe prefix pixels")
        if retire:
            checks.require(b"reference picture missing during reorder" in decoded.stderr and
                           b"Missing reference picture, default is 10" in decoded.stderr,
                           "CPU-only missing-reference diagnostics, not exit rejection")
            checks.require(decoded.stdout[-FRAME_BYTES:] == c, "CPU concealment uses C")
        else:
            checks.require(decoded.returncode == 0 and decoded.stderr == b"", "retained LT0 control")
            checks.require(selected[-1] == 6 and decoded.stdout[-FRAME_BYTES:] == b, "retained LT0 uses B")
        negative_receipts.append({"retire": retire, "ffmpeg_exit": decoded.returncode,
                                  "missing_reference_diagnostic": retire, "last_pattern": "C" if retire else "B",
                                  "hardware_candidate": False})
    check_native_fixture(checks, outputs, expected)
    summary = {"self_checks": checks.count, "valid_cpu_frames": 3*FRAMES, "variants": receipts,
               "nal_headers": [format(n, "02x") for n in NAL_HEADERS], "frame_num": FRAME_NUM,
               "poc": list(range(FRAMES)), "reference": [i % 2 == 0 for i in range(FRAMES)],
               "selected_source_frames": SOURCE_FRAMES,
               "pts_100ns_proposal": [i*10000000//30 for i in range(FRAMES)],
               "planar_oracle_bytes": len(expected), "planar_oracle_sha256": ORACLE_SHA256,
               "packed_oracle_bytes": len(packed), "packed_oracle_sha256": PACKED_SHA256,
               "packing_scope": "empirical prior stock progressive 256x96 unscaled MFD; not universal SCL",
               "cpu_only_reference_probes": negative_receipts,
               "valid_pixels_alone_certify_retirement": False, "physical_lease_certified": False,
               "packed_oracle_provided": True, "hardware_access": False}
    return outputs, expected, packed, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--self-test", action="store_true", help="CPU-only validation (the default)")
    mode.add_argument("--emit", metavar="NEWDIR", type=Path,
                      help="after validation, create a new directory with valid streams and fixed-profile oracles")
    args = parser.parse_args()
    try:
        if args.emit is not None and os.path.lexists(args.emit):
            raise ValueError("emit target already exists; nothing overwritten")
        outputs, oracle, packed, summary = validate()
        if args.emit is not None:
            args.emit.mkdir(mode=0o700)  # Atomic refusal; do not create parent directories.
            oracles = (("expected-lifecycle.yuv420p", oracle), ("expected-lifecycle.yuy2", packed))
            for name, payload in (*outputs, *oracles):
                with (args.emit / name).open("xb") as destination:
                    destination.write(payload)
            summary["emitted_files"] = [name for name, _ in (*outputs, *oracles)]
        else:
            summary["emitted_files"] = []
        print(json.dumps(summary, sort_keys=True))
        return 0
    except (AssertionError, ValueError, OSError, subprocess.TimeoutExpired, KeyError) as error:
        print("reference lifecycle validation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
