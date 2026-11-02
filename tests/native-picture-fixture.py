#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded colour fixture and replicated content barcode. Imported without side effects.

An ordinal identifies source content, not a DRM object or physical display time."""
FRAMES = 180
FPS = 30
MAGIC = 0xC53D


def geometry(width, height):
    if (width, height) not in ((1280, 720), (1920, 1080)):
        raise ValueError('unsupported fixture geometry')

def crc8(values):
    value = 0
    for byte in values:
        value ^= byte
        for _ in range(8):
            value = (value << 1 ^ (7 if value & 128 else 0)) & 255
    return value

def case_id(width, source_case=None):
    if not width in (1280, 1920):
        raise ValueError('invalid fixture input')
    if source_case is not None:
        if not isinstance(source_case, int) or not 1 <= source_case <= 255:
            raise ValueError('invalid declared source case')
        return source_case
    return 1 if width == 1280 else 2

def code_bytes(width, ordinal, *, source_case=None):
    if not 0 <= ordinal < FRAMES:
        raise ValueError('invalid fixture input')
    data = [MAGIC >> 8, MAGIC & 255, case_id(width, source_case), ordinal]
    return bytes(data + [ordinal ^ 255, crc8(data)])

def cells(width):
    size = width // 64
    left = width // 8
    return [(left + i * size, top, size, 32) for top in (16, 64) for i in range(48)]

def rgb_to_yuv(red, green, blue):
    y = (0.2126 * red + 0.7152 * green + 0.0722 * blue) / 255
    return (round(16 + 219 * y), round(128 + 224 * (blue / 255 - y) / (2 * (1 - 0.0722))), round(128 + 224 * (red / 255 - y) / (2 * (1 - 0.2126))))

def base_frame(width, height):
    geometry(width, height)
    palette = [rgb_to_yuv(*color) for color in ((192, 64, 64), (64, 192, 64), (64, 64, 192), (192, 192, 64), (192, 64, 192), (64, 192, 192), (128, 128, 128), (32, 32, 32))]
    result = bytearray()
    for plane, w, h in ((0, width, height), (1, width // 2, height // 2), (2, width // 2, height // 2)):
        row = b''.join((bytes([p[plane]]) * (w // 8) for p in palette))
        result.extend(row * h)
    result[:width * 128] = bytes([16]) * (width * 128)
    p = width * height
    for offset in (p, p * 5 // 4):
        result[offset:offset + width // 2 * 64] = bytes([128]) * (width // 2 * 64)
    for x, value in ((0, 235), (width - 32, 16)):
        for y in range(height - 32, height):
            result[y * width + x:y * width + x + 32] = bytes([value]) * 32
        for offset in (p, p * 5 // 4):
            for y in range(height // 2 - 16, height // 2):
                q = offset + y * (width // 2) + x // 2
                result[q:q + 16] = bytes([128]) * 16
    return result

def frame(width, height, ordinal, base=None, *, source_case=None):
    geometry(width, height)
    result = bytearray(base if base is not None else base_frame(width, height))
    bits = [value >> shift & 1 for value in code_bytes(width, ordinal, source_case=source_case)
            for shift in range(7, -1, -1)]
    for (x, y, w, h), bit in zip(cells(width), bits * 2):
        for row in range(y, y + h):
            result[row * width + x:row * width + x + w] = bytes([235 if bit else 16]) * w
    return result

def decode(width, height, data, kind, *, source_case=None):
    geometry(width, height)
    if not kind in ('yuv420p', 'yuyv', 'bgra'):
        raise ValueError('invalid fixture input')
    expected = width * height * (4 if kind == 'bgra' else 2) if kind != 'yuv420p' else width * height * 3 // 2
    if not len(data) == expected:
        raise ValueError('barcode extent mismatch')
    bits = []
    for x, y, w, h in cells(width):
        values = []
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                px, py = (x + w // 2 + dx, y + h // 2 + dy)
                if kind == 'bgra':
                    q = (py * width + px) * 4
                    channels = data[q:q + 3]
                    if not (max(channels) - min(channels) <= 4 and data[q + 3] == 255):
                        raise ValueError('non-neutral barcode cell')
                    values.append(sum(channels) / 3)
                else:
                    values.append(data[(py * width + px) * (2 if kind == 'yuyv' else 1)])
        if not max(values) - min(values) <= 8:
            raise ValueError('nonuniform barcode cell')
        value = sum(values) / len(values)
        if not (value <= 64 or value >= 192):
            raise ValueError('ambiguous barcode threshold')
        bits.append(int(value >= 192))
    if not bits[:48] == bits[48:]:
        raise ValueError('barcode replicas differ')
    decoded = bytes((sum((bits[i + j] << 7 - j for j in range(8))) for i in range(0, 48, 8)))
    if not int.from_bytes(decoded[:2], 'big') == MAGIC:
        raise ValueError('barcode magic mismatch')
    if not decoded[2] == case_id(width, source_case):
        raise ValueError('barcode source-case mismatch')
    if not decoded[3] ^ decoded[4] == 255:
        raise ValueError('barcode complement mismatch')
    if not crc8(decoded[:4]) == decoded[5]:
        raise ValueError('barcode CRC mismatch')
    if not decoded[3] < FRAMES:
        raise ValueError('barcode ordinal out of range')
    return decoded[3]


def main():
    import argparse
    import json
    from pathlib import Path
    import sys

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--width', type=int, choices=(1280, 1920), required=True)
    parser.add_argument('--source-case', type=int,
                        help='source identity declared before capture; default is the original width case')
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--generate-yuv420p', help='new file, or - for stdout; all180 frames')
    action.add_argument('--decode', type=Path, help='unmodified tightly packed capture')
    parser.add_argument('--format', choices=('yuv420p', 'yuyv', 'bgra'), default='bgra')
    args = parser.parse_args()
    height = 720 if args.width == 1280 else 1080
    case_id(args.width, args.source_case)
    if args.decode:
        extent = args.width * height * {'yuv420p': 3, 'yuyv': 4, 'bgra': 8}[args.format] // 2
        if args.decode.stat().st_size != extent:
            raise ValueError('barcode extent mismatch')
        ordinal = decode(args.width, height, args.decode.read_bytes(), args.format,
                         source_case=args.source_case)
        print(json.dumps(dict(width=args.width, height=height, source_ordinal=ordinal,
                              source_case=case_id(args.width, args.source_case),
                              scope='source content only; no DRM-object or display timestamp')))
    else:
        output = sys.stdout.buffer if args.generate_yuv420p == '-' else open(args.generate_yuv420p, 'xb')
        try:
            base = base_frame(args.width, height)
            for ordinal in range(FRAMES):
                output.write(frame(args.width, height, ordinal, base, source_case=args.source_case))
        finally:
            if args.generate_yuv420p != '-':
                output.close()
    return 0


if __name__ == '__main__':
    import sys
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print('native-picture-fixture: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
