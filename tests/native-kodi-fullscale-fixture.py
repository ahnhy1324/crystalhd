#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare full-scale Kodi input and freeze independent source-only samples."""
import argparse
import hashlib
import importlib.util
import json
from fractions import Fraction
from pathlib import Path
import re
import subprocess


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--width', type=int, choices=(1280, 1920), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    width, height = args.width, 720 if args.width == 1280 else 1080
    frames, source_case = 30, 17 if width == 1280 else 18
    barcode = load('fullscale_barcode', 'native-picture-fixture.py')
    oracle = load('fullscale_oracle', 'native-picture-oracle.py')
    levels = [0, 1, 8, 15, 16, 17, 31, 64, 96, 128, 160, 192, 219, 220, 234, 235, 236, 247, 254, 255]
    chroma = [(128, 128), (64, 128), (192, 128), (128, 64), (128, 192),
              (64, 192), (192, 64), (0, 255), (255, 0), (96, 160)]
    plane = width * height
    row = b''.join(bytes([value]) * (width // 20) for value in levels)
    base = bytearray(row * height + bytes([128]) * (plane // 2))
    base[:width * 128] = bytes([16]) * (width * 128)
    # Neutral luma region above the coloured lower half; barcode outside mask.
    for component, offset in ((0, plane), (1, plane * 5 // 4)):
        color_row = b''.join(bytes([chroma[i % 10][component]]) * (width // 40) for i in range(20))
        for y in range(height // 4, height // 2):
            start = offset + y * (width // 2)
            base[start:start + width // 2] = color_row
    with (root / 'input.yuv').open('xb') as output:
        for ordinal in range(frames):
            output.write(barcode.frame(width, height, ordinal, base, source_case=source_case))
    media, decoded = root / 'fullscale-aac.mp4', root / 'decoded.yuv'
    command = ['ffmpeg', '-hide_banner', '-nostdin', '-f', 'rawvideo', '-pixel_format', 'yuv420p',
               '-video_size', f'{width}x{height}', '-framerate', '30', '-color_range', 'pc',
               '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709',
               '-i', str(root / 'input.yuv'), '-f', 'lavfi', '-i', 'anullsrc=r=48000:cl=stereo',
               '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'libx264', '-profile:v', 'high',
               '-preset', 'veryfast', '-qp', '1', '-bf', '0', '-g', '30', '-threads:v', '1',
               '-x264-params', 'slices=1:repeat-headers=1:scenecut=0', '-pix_fmt', 'yuv420p',
               '-color_range', 'pc', '-colorspace', 'bt709', '-color_primaries', 'bt709',
               '-color_trc', 'bt709', '-c:a', 'aac', '-b:a', '96k', '-t', '1', str(media)]
    with (root / 'encode.log').open('x') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (root / 'decode.log').open('x') as log:
        subprocess.run(['ffmpeg', '-hide_banner', '-nostdin', '-i', str(media), '-map', '0:v:0',
                        '-an', '-c:v', 'rawvideo', '-pix_fmt', 'yuvj420p', '-f', 'rawvideo', str(decoded)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    with (root / 'trace.log').open('x') as log:
        subprocess.run(['ffmpeg', '-hide_banner', '-nostdin', '-i', str(media), '-map', '0:v:0',
                        '-c:v', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    low, high, below, above, count = 255, 0, 0, 0, 0
    with decoded.open('rb') as source:
        for ordinal, picture in enumerate(oracle.read_frames(source, plane * 3 // 2)):
            assert barcode.decode(width, height, picture, 'yuv420p', source_case=source_case) == ordinal
            y = picture[:plane]
            low, high = min(low, min(y)), max(high, max(y))
            below += sum(y.count(value) for value in range(16))
            above += sum(y.count(value) for value in range(236, 256))
            count += 1
    assert count == frames and low == 0 and high == 255 and below and above
    with decoded.open('rb') as source:
        points, reference_hash = oracle.reference_samples(oracle.read_frames(source, plane * 3 // 2),
                                                         width, height, frames, 32)
    points = {point: value for point, value in points.items() if point[1] >= 128}
    assert len(points) >= 100
    grays = {y for y, u, v in points.values() if u == v == 128}
    assert {0, 255} <= grays and any(16 <= y <= 32 for y in grays) and any(64 <= y <= 192 for y in grays)
    trace = (root / 'trace.log').read_text()
    first = re.findall(r'first_mb_in_slice\s+[01]+ = (\d+)', trace)
    assert len(first) == frames and set(first) == {'0'}
    for field in ('video_full_range_flag', 'colour_primaries', 'transfer_characteristics', 'matrix_coefficients'):
        assert set(re.findall(field + r'\s+[01]+ = (\d+)', trace)) == {'1'}
    probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                      '-show_packets', '-show_streams', '-show_entries',
                      'packet=pts:stream=time_base,color_range', '-of', 'json', str(media)], text=True))
    assert probe['streams'][0]['color_range'] == 'pc'
    time_base = probe['streams'][0]['time_base']
    timestamps = sorted(int(packet['pts']) for packet in probe['packets'])
    assert len(timestamps) == len(set(timestamps)) == frames
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = dict(id=f'fullscale-{width}', width=width, height=height, frames=frames, fps=30,
                    media=str(media), media_sha256=digest(media), reference_sha256=reference_hash,
                    color_range='full', matrix='bt709', expected_colorspace=3, expected_ycbcr_enc=2,
                    expected_xfer_func=1, expected_quantization=1, barcode_case_id=source_case, minimum_points=100,
                    source_luma_min=low, source_luma_max=high, source_below16=below, source_above235=above,
                    gray_sample_levels=sorted(grays), source_mask_y=128, sample_grid_step=32,
                    invariant_points=[dict(x=x, y=y, yuv=list(value)) for (x, y), value in sorted(points.items())],
                    frame_pts=[dict(ordinal=i, pts=pts, time_base=time_base,
                                    pts_time=str(float(pts * Fraction(time_base)))) for i, pts in enumerate(timestamps)],
                    encode_command=command, preparation_script_sha256=digest(Path(__file__)),
                    trace_sha256=digest(root / 'trace.log'),
                    scope='Actual full-scale source and source-only sampled reference; no hardware or presentation claim.')
    (root / 'fixture.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({key: manifest[key] for key in ('id', 'frames', 'source_luma_min', 'source_luma_max',
                      'source_below16', 'source_above235')}, indent=2))
    print(f'Source-qualified invariant points: {len(points)}')


if __name__ == '__main__':
    main()
