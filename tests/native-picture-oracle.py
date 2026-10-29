#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check source-invariant colour samples against one native BGRA capture.

The reference is an independently decoded, packed YUV420p sequence, in display
order. Candidate points come only from that sequence: their Y/U/V samples and
3x3 neighbourhoods must be flat and identical in EVERY reference frame. This
avoids selecting a source frame by matching the capture. It verifies sampled
colour/spatial invariants, not decoded YUYV, crop completeness, source-bound
capture PTS, GPU completion, physical scanout, controls or A/V.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys


ROUNDING_ERROR = 2
MAX_FRAMES = 240


def require(condition, message):
    if not condition:
        raise ValueError(message)


def geometry(width, height):
    require(isinstance(width, int) and isinstance(height, int) and
            4 <= width <= 1920 and 4 <= height <= 1088 and not width % 2 and not height % 2,
            'requires bounded, even, native YUV420p geometry')


def rgb(y, u, v, matrix='bt709'):
    """Independent real-valued limited-range Y'CbCr equation, clipped last."""
    require(matrix in ('bt709', 'bt601'), 'unsupported matrix')
    require(all(isinstance(c, int) and 0 <= c <= 255 for c in (y, u, v)),
            'YUV components must be bytes')
    kr, kb = (.2126, .0722) if matrix == 'bt709' else (.299, .114)
    luminance = (y - 16) * 255 / 219
    cb, cr = (u - 128) * 255 / 224, (v - 128) * 255 / 224
    values = (luminance + 2 * (1 - kr) * cr,
              luminance - 2 * kb * (1 - kb) / (1 - kr - kb) * cb -
              2 * kr * (1 - kr) / (1 - kr - kb) * cr,
              luminance + 2 * (1 - kb) * cb)
    return tuple(max(0, min(255, round(value))) for value in values)


def flat_sample(frame, width, height, x, y):
    plane = width * height
    chroma_width, chroma_height = width // 2, height // 2
    coordinates = ((x, y, width, height, 0),
                   (x // 2, y // 2, chroma_width, chroma_height, plane),
                   (x // 2, y // 2, chroma_width, chroma_height, plane * 5 // 4))
    result = []
    for px, py, pitch, rows, offset in coordinates:
        value = frame[offset + py * pitch + px]
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                qx, qy = max(0, min(pitch - 1, px + dx)), max(0, min(rows - 1, py + dy))
                if frame[offset + qy * pitch + qx] != value:
                    return None
        result.append(value)
    return tuple(result)


def reference_samples(frames, width, height, expected_frames, step=32):
    geometry(width, height)
    require(isinstance(expected_frames, int) and 1 <= expected_frames <= MAX_FRAMES,
            'reference frame count must be between 1 and 240')
    require(isinstance(step, int) and 1 <= step <= 128, 'invalid sample-grid step')
    xs = sorted(set(range(step // 2, width, step)) | {0, width - 1})
    ys = sorted(set(range(step // 2, height, step)) | {0, height - 1})
    candidates = None
    digest = hashlib.sha256()
    count = 0
    for count, frame in enumerate(frames, 1):
        require(count <= expected_frames, 'reference has extra frames')
        require(len(frame) == width * height * 3 // 2, 'short/oversized YUV420p frame')
        digest.update(frame)
        if candidates is None:
            candidates = {(x, y): value for y in ys for x in xs
                          if (value := flat_sample(frame, width, height, x, y)) is not None}
        else:
            candidates = {point: value for point, value in candidates.items()
                          if flat_sample(frame, width, height, *point) == value}
    require(count == expected_frames, 'reference frame count mismatch')
    return candidates, digest.hexdigest()


def compare(samples, captured, width, height, *, stride=None, matrix='bt709', minimum_samples=16):
    geometry(width, height)
    require(matrix in ('bt709', 'bt601'), 'unsupported declared matrix')
    stride = width * 4 if stride is None else stride
    require(isinstance(stride, int) and width * 4 <= stride <= width * 4 + 4096,
            'invalid BGRA stride')
    require(len(captured) == stride * height, 'capture byte extent/stride mismatch')
    require(isinstance(minimum_samples, int) and minimum_samples >= 1, 'invalid sample minimum')
    for point, yuv in samples.items():
        require(len(point) == 2 and all(isinstance(v, int) for v in point) and
                0 <= point[0] < width and 0 <= point[1] < height,
                'sample lies outside the source/capture geometry')
        require(len(yuv) == 3, 'invalid YUV sample')
        rgb(*yuv, matrix)
    nonopaque = sum(captured[y * stride + x * 4 + 3] != 255
                    for y in range(height) for x in range(width))
    maximum = total = over = 0
    differences = []
    # BT601 is a fixed diagnostic control when the declaration is BT709. Its
    # result NEVER selects the expected matrix or changes the primary verdict.
    alternate = 'bt601' if matrix == 'bt709' else 'bt709'
    alternate_max = alternate_over = 0
    for (x, y), yuv in sorted(samples.items(), key=lambda item: (item[0][1], item[0][0])):
        offset = y * stride + x * 4
        actual = tuple(reversed(captured[offset:offset + 3]))
        expected = rgb(*yuv, matrix)
        errors = tuple(abs(a - b) for a, b in zip(actual, expected))
        maximum = max(maximum, *errors)
        total += sum(errors)
        over += sum(error > ROUNDING_ERROR for error in errors)
        other_errors = tuple(abs(a - b) for a, b in zip(actual, rgb(*yuv, alternate)))
        alternate_max = max(alternate_max, *other_errors)
        alternate_over += sum(error > ROUNDING_ERROR for error in other_errors)
        if len(differences) < 12 and max(errors) > ROUNDING_ERROR:
            differences.append(dict(x=x, y=y, source_yuv=list(yuv), expected_rgb=list(expected),
                                    captured_rgb=list(actual), errors=list(errors)))
    adequate = len(samples) >= minimum_samples
    result = 'FAIL' if over or nonopaque else ('PASS' if adequate else 'NOT_VERIFIED')
    return dict(schema=1, result=result, scope='sampled source-invariant colour/spatial points only',
                width=width, height=height, capture_stride=stride, declared_matrix=matrix,
                declared_range='limited', permitted_channel_error=ROUNDING_ERROR,
                invariant_samples=len(samples), minimum_samples=minimum_samples,
                reference_coverage='SUFFICIENT' if adequate else 'INSUFFICIENT',
                maximum_channel_error=maximum, channels_over_tolerance=over,
                mean_absolute_channel_error=total / (len(samples) * 3) if samples else None,
                nonopaque_active_pixels=nonopaque, differences=differences,
                diagnostic_alternate_matrix=dict(matrix=alternate, maximum_channel_error=alternate_max,
                                                 channels_over_tolerance=alternate_over,
                                                 changes_primary_verdict=False),
                capture_sha256=hashlib.sha256(captured).hexdigest(),
                decoded_yuyv='NOT_OBSERVED', capture_source_pts='NOT_VERIFIED',
                complete_crop_and_pixels='NOT_VERIFIED', physical_presentation='NOT_VERIFIED')


def read_frames(stream, frame_bytes):
    while True:
        frame = bytearray()
        while len(frame) < frame_bytes:
            chunk = stream.read(frame_bytes - len(frame))
            if not chunk:
                break
            frame.extend(chunk)
        if not frame:
            return
        require(len(frame) == frame_bytes, 'truncated reference frame')
        yield frame


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', help='packed independent YUV420p sequence, or - for stdin')
    parser.add_argument('capture', type=Path, help='unmodified native BGRA capture')
    parser.add_argument('--width', type=int, required=True)
    parser.add_argument('--height', type=int, required=True)
    parser.add_argument('--frames', type=int, required=True)
    parser.add_argument('--step', type=int, default=32)
    parser.add_argument('--minimum-samples', type=int, default=16)
    parser.add_argument('--stride', type=int)
    parser.add_argument('--matrix', choices=('bt709', 'bt601'), required=True)
    parser.add_argument('--reference-sha256')
    parser.add_argument('--capture-sha256')
    args = parser.parse_args()
    geometry(args.width, args.height)
    stream = sys.stdin.buffer if args.reference == '-' else open(args.reference, 'rb')
    try:
        samples, digest = reference_samples(read_frames(stream, args.width * args.height * 3 // 2),
                                            args.width, args.height, args.frames, args.step)
    finally:
        if args.reference != '-':
            stream.close()
    require(args.reference_sha256 in (None, digest), 'reference SHA256 mismatch')
    stride = args.width * 4 if args.stride is None else args.stride
    require(isinstance(stride, int) and args.width * 4 <= stride <= args.width * 4 + 4096,
            'invalid BGRA stride')
    require(args.capture.stat().st_size == stride * args.height, 'capture byte extent/stride mismatch')
    captured = args.capture.read_bytes()
    require(args.capture_sha256 in (None, hashlib.sha256(captured).hexdigest()),
            'capture SHA256 mismatch')
    report = compare(samples, captured, args.width, args.height, stride=args.stride,
                     matrix=args.matrix, minimum_samples=args.minimum_samples)
    report.update(reference_frames=args.frames, reference_sha256=digest, sample_grid_step=args.step,
                  sample_selection='flat 3x3 Y/U/V neighbourhoods identical in EVERY source frame')
    print(json.dumps(report, indent=2))
    return 0 if report['result'] == 'PASS' else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
