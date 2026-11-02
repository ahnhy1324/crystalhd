#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare progressive POC2 2/4-slice range sources and independent references."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess as sp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    root = parser.parse_args().output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    width, height, frames = 640, 360, 30
    levels = [0, 1, 8, 15, 16, 17, 31, 64, 96, 128, 160, 192, 219, 220, 234, 235, 236, 247, 254, 255]
    chroma = [(128, 128), (64, 128), (192, 128), (128, 64), (128, 192),
              (64, 192), (192, 64), (0, 255), (255, 0), (96, 160)]
    raw = root / 'input.yuv'
    with raw.open('xb') as stream:
        for ordinal in range(frames):
            row = bytes(levels[(x // 32 + ordinal) % len(levels)] for x in range(width))
            u, v = chroma[ordinal % len(chroma)]
            stream.write(row * height + bytes([u]) * (width * height // 4) +
                         bytes([v]) * (width * height // 4))
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = dict(width=width, height=height, frames=frames, cases=[],
                    source_sha256=digest(raw), preparation_script_sha256=digest(Path(__file__)))
    frozen = {}
    for slices in (2, 4):
        full = root / f'slices{slices}-full.mp4'
        command = ['ffmpeg', '-nostdin', '-v', 'error', '-f', 'rawvideo', '-pixel_format', 'yuv420p',
                   '-video_size', f'{width}x{height}', '-framerate', '30', '-color_range', 'pc',
                   '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709',
                   '-i', str(raw), '-an', '-c:v', 'libx264', '-profile:v', 'high', '-preset', 'veryfast',
                   '-qp', '1', '-bf', '0', '-g', '30', '-threads:v', '1', '-x264-params',
                   f'slices={slices}:repeat-headers=1:scenecut=0', '-pix_fmt', 'yuv420p',
                   '-color_range', 'pc', '-colorspace', 'bt709', '-color_primaries', 'bt709',
                   '-color_trc', 'bt709', str(full)]
        sp.run(command, check=True)
        limited = root / f'slices{slices}-limited.mp4'
        sp.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(full), '-an', '-c:v', 'copy',
                '-bsf:v', 'h264_metadata=video_full_range_flag=0', '-color_range', 'tv', str(limited)],
               check=True)
        for label, media, pixel_format, flag in [('full', full, 'yuvj420p', 1),
                                                 ('limited', limited, 'yuv420p', 0)]:
            stem = media.stem
            trace = sp.run(['ffmpeg', '-hide_banner', '-nostdin', '-i', str(media), '-an',
                            '-c:v', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'],
                           stdout=sp.PIPE, stderr=sp.STDOUT, text=True, check=True).stdout
            (root / (stem + '.trace.log')).write_text(trace)
            values = lambda key: [int(n) for n in re.findall(key + r'\s+[01]+ = (\d+)', trace)]
            for key, expected in [('profile_idc', 100), ('pic_order_cnt_type', 2),
                                  ('frame_mbs_only_flag', 1), ('num_slice_groups_minus1', 0),
                                  ('redundant_pic_cnt_present_flag', 0),
                                  ('colour_primaries', 1), ('transfer_characteristics', 1),
                                  ('matrix_coefficients', 1), ('video_full_range_flag', flag)]:
                assert values(key) and set(values(key)) == {expected}, (stem, key, values(key))
            first = values('first_mb_in_slice')
            assert len(first) == frames * slices and first[:slices] == sorted(set(first[:slices]))
            assert first[:slices][0] == 0
            assert all(first[i * slices:(i + 1) * slices] == first[:slices] for i in range(frames))
            fn = values('frame_num')
            assert len(fn) == frames * slices
            assert all(len(set(fn[i * slices:(i + 1) * slices])) == 1 for i in range(frames))
            decoded = root / (stem + '.yuv')
            sp.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(media), '-an', '-c:v', 'rawvideo',
                    '-pix_fmt', pixel_format, '-f', 'rawvideo', str(decoded)], check=True)
            data = decoded.read_bytes()
            plane = width * height
            assert len(data) == frames * plane * 3 // 2
            packed = bytearray()
            for ordinal in range(frames):
                picture = data[ordinal * plane * 3 // 2:(ordinal + 1) * plane * 3 // 2]
                y, u, v = picture[:plane], picture[plane:plane * 5 // 4], picture[plane * 5 // 4:]
                assert len(set(u)) == len(set(v)) == 1, (stem, ordinal)
                image = bytearray(plane * 2)
                image[0::2] = y
                image[1::4] = bytes([u[0]]) * (plane // 2)
                image[3::4] = bytes([v[0]]) * (plane // 2)
                packed.extend(image)
            reference = root / (stem + '.yuyv')
            reference.write_bytes(packed)
            annexb = root / (stem + '.h264')
            sp.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(media), '-an', '-c:v', 'copy',
                    '-bsf:v', 'h264_mp4toannexb', '-f', 'h264', str(annexb)], check=True)
            raw_nals = annexb.read_bytes()
            starts = list(re.finditer(b'\x00\x00(?:\x00)?\x01', raw_nals))
            nals = [raw_nals[m.end():starts[i + 1].start() if i + 1 < len(starts) else len(raw_nals)]
                    .rstrip(b'\x00') for i, m in enumerate(starts)]
            vcl = [n for n in nals if n[0] & 31 in (1, 5)]
            assert len(vcl) == frames * slices
            frozen[stem] = dict(sps=next(n.hex() for n in nals if n[0] & 31 == 7),
                                pps=next(n.hex() for n in nals if n[0] & 31 == 8),
                                vcl=[n.hex() for n in vcl[:slices * 3]])
            probe = json.loads(sp.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                '-show_packets', '-show_streams', '-show_entries',
                'packet=pts:stream=time_base,color_range,has_b_frames', '-of', 'json', str(media)], text=True))
            pts = [int(p['pts']) for p in probe['packets']]
            assert len(pts) == len(set(pts)) == frames and pts == sorted(pts)
            assert probe['streams'][0]['has_b_frames'] == 0
            manifest['cases'].append(dict(stem=stem, slices=slices, range=label,
                first_mb=first[:slices], frame_num_bits=values('log2_max_frame_num_minus4')[0] + 4,
                pic_width_in_mbs=values('pic_width_in_mbs_minus1')[0] + 1,
                pic_height_in_map_units=values('pic_height_in_map_units_minus1')[0] + 1,
                media_sha256=digest(media), decoded_sha256=digest(decoded),
                reference_sha256=digest(reference), annexb_sha256=digest(annexb),
                trace_sha256=digest(root / (stem + '.trace.log')), frame_pts=pts,
                time_base=probe['streams'][0]['time_base'], encode_command=command if flag else None))
        assert (root / f'slices{slices}-full.yuv').read_bytes() == (root / f'slices{slices}-limited.yuv').read_bytes()
    (root / 'fixture.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (root / 'prefix-goldens.json').write_text(json.dumps(frozen, indent=2) + '\n')
    print(json.dumps(dict(status='SOURCE_ONLY_PASS', cases=len(manifest['cases']),
                         frames=frames, slices=[2, 4]), indent=2))


if __name__ == '__main__':
    main()
