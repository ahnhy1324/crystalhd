#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare actual B-picture progressive POC0 2/4-slice range sources and independent references."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess as sp
import zlib


def tag(ordinal):
    payload = b'CH' + ordinal.to_bytes(2, 'big')
    return payload + zlib.crc32(payload).to_bytes(4, 'big')


def read_tag(y, width, top):
    result = bytearray(8)
    for bit in range(64):
        x, row = 16 + (bit % 16) * 16, top + (bit // 16) * 16
        values = [y[(row + dy) * width + x + dx] for dy in range(4, 12) for dx in range(4, 12)]
        assert max(values) - min(values) < 8, (bit, min(values), max(values))
        result[bit // 8] |= (sum(values) / len(values) > 128) << (7 - bit % 8)
    return bytes(result)


def trace_pictures(trace, packets, slices):
    # Independent unchanged FFmpeg trace: no production CrystalHD parsing.
    aus, current, last_ref = [], None, None
    for line in trace.splitlines():
        packet = re.search(r'Packet: .*pts (-?\d+), dts (-?\d+),', line)
        if packet:
            aus.append(dict(decode_index=len(aus), pts=int(packet[1]), dts=int(packet[2]), slices=[]))
            current = None
        field = re.search(r'\]\s+\d+\s+(\w+)\s+[01]+ = (-?\d+)', line)
        if not field or not aus:
            continue
        key, value = field[1], int(field[2])
        if key == 'nal_unit_type':
            current = {} if value in (1, 5) else None
            if current is not None:
                current.update(nal_unit_type=value, nal_ref_idc=last_ref)
                aus[-1]['slices'].append(current)
        if key == 'nal_ref_idc':
            last_ref = value
        if current is not None and key in ('first_mb_in_slice', 'slice_type', 'pic_parameter_set_id',
                'frame_num', 'idr_pic_id', 'pic_order_cnt_lsb', 'delta_pic_order_cnt_bottom'):
            current[key] = value
    assert len(aus) == len(packets)
    for au, packet in zip(aus, packets):
        assert (au['pts'], au['dts']) == (int(packet['pts']), int(packet['dts']))
        au['flags'] = packet['flags']
        assert len(au['slices']) == slices
        first = [s['first_mb_in_slice'] for s in au['slices']]
        assert first[0] == 0 and first == sorted(set(first))
        keys = [{k: v for k, v in s.items() if k != 'first_mb_in_slice'} for s in au['slices']]
        assert all(key == keys[0] for key in keys)
    b = [au for au in aus if au['slices'][0]['slice_type'] % 5 == 1]
    assert b and all(au['slices'][0]['nal_ref_idc'] == 0 for au in b)
    pair = next((a['decode_index'], c['decode_index']) for a, c in zip(b, b[1:])
                if a['slices'][0]['frame_num'] == c['slices'][0]['frame_num'] and
                a['slices'][0]['pic_order_cnt_lsb'] != c['slices'][0]['pic_order_cnt_lsb'])
    return aus, pair


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ordinal-base', type=int, default=0)
    parser.add_argument('--slices', type=int, choices=(2, 4), nargs='+', default=[2, 4])
    args = parser.parse_args()
    assert 0 <= args.ordinal_base <= 65506
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    width, height, frames = 640, 360, 30
    levels = [0, 1, 8, 15, 16, 17, 31, 64, 96, 128, 160, 192, 219, 220, 234, 235, 236, 247, 254, 255]
    chroma = [(128, 128), (64, 128), (192, 128), (128, 64), (128, 192),
              (64, 192), (192, 64), (0, 255), (255, 0), (96, 160)]
    raw = root / 'input.yuv'
    with raw.open('xb') as stream:
        for ordinal in range(frames):
            row = bytes(levels[x // 32] for x in range(width))
            luma = bytearray(row * height)
            for top in (16, 272):
                for bit in range(64):
                    value = 235 if tag(ordinal + args.ordinal_base)[bit // 8] & (1 << (7 - bit % 8)) else 16
                    x, y = 16 + (bit % 16) * 16, top + (bit // 16) * 16
                    for dy in range(16):
                        luma[(y + dy) * width + x:(y + dy) * width + x + 16] = bytes([value]) * 16
            x = 16 + ordinal * 16
            for y in range(160, 192):
                luma[y * width + x:y * width + x + 16] = bytes([235]) * 16
            u, v = chroma[ordinal % len(chroma)]
            stream.write(luma + bytes([u]) * (width * height // 4) +
                         bytes([v]) * (width * height // 4))
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    manifest = dict(width=width, height=height, frames=frames, ordinal_base=args.ordinal_base, cases=[],
                    source_sha256=digest(raw), preparation_script_sha256=digest(Path(__file__)))
    frozen = {}
    for slices in args.slices:
        full = root / f'slices{slices}-full.mp4'
        command = ['ffmpeg', '-nostdin', '-v', 'error', '-f', 'rawvideo', '-pixel_format', 'yuv420p',
                   '-video_size', f'{width}x{height}', '-framerate', '30', '-color_range', 'pc',
                   '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709',
                   '-i', str(raw), '-an', '-c:v', 'libx264', '-profile:v', 'high', '-preset', 'veryfast',
                   '-qp', '1', '-bf', '2', '-g', '30', '-threads:v', '1', '-x264-params',
                   f'slices={slices}:repeat-headers=1:scenecut=0:b-adapt=0:b-pyramid=none:open-gop=0:keyint=30:min-keyint=30', '-pix_fmt', 'yuv420p',
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
            for key, expected in [('profile_idc', 100), ('pic_order_cnt_type', 0),
                                  ('frame_mbs_only_flag', 1), ('num_slice_groups_minus1', 0),
                                  ('redundant_pic_cnt_present_flag', 0),
                                  ('bottom_field_pic_order_in_frame_present_flag', 0),
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
            sp.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(media), '-an', '-fps_mode', 'passthrough', '-c:v', 'rawvideo',
                    '-pix_fmt', pixel_format, '-f', 'rawvideo', str(decoded)], check=True)
            data = decoded.read_bytes()
            plane = width * height
            assert len(data) == frames * plane * 3 // 2
            shown = json.loads(sp.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                '-show_frames', '-show_entries', 'frame=pts,pict_type', '-of', 'json', str(media)], text=True))['frames']
            pictures = []
            packed = bytearray()
            for ordinal in range(frames):
                picture = data[ordinal * plane * 3 // 2:(ordinal + 1) * plane * 3 // 2]
                y, u, v = picture[:plane], picture[plane:plane * 5 // 4], picture[plane * 5 // 4:]
                assert read_tag(y, width, 16) == read_tag(y, width, 272) == tag(ordinal + args.ordinal_base)
                assert len(set(u)) == len(set(v)) == 1, (stem, ordinal)
                image = bytearray(plane * 2)
                image[0::2] = y
                image[1::4] = bytes([u[0]]) * (plane // 2)
                image[3::4] = bytes([v[0]]) * (plane // 2)
                pictures.append(dict(ordinal=ordinal + args.ordinal_base,
                    tag_hex=tag(ordinal + args.ordinal_base).hex(), pts=int(shown[ordinal]['pts']),
                    pict_type=shown[ordinal]['pict_type'], yuyv_sha256=hashlib.sha256(image).hexdigest()))
                packed.extend(image)
            assert len({p['yuyv_sha256'] for p in pictures}) == frames
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
                                vcl=[n[:16].hex() for n in vcl[:slices * 4]])
            probe = json.loads(sp.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                '-show_packets', '-show_streams', '-show_entries',
                'packet=pts,dts,flags:stream=time_base,color_range,has_b_frames', '-of', 'json', str(media)], text=True))
            pts = [int(p['pts']) for p in probe['packets']]
            dts = [int(p['dts']) for p in probe['packets']]
            assert len(pts) == len(set(pts)) == frames and min(pts) >= 0 and pts != sorted(pts)
            assert len(dts) == len(set(dts)) == frames and dts == sorted(dts)
            assert probe['streams'][0]['has_b_frames'] > 0
            assert [p['pts'] for p in pictures] == sorted(pts)
            aus, b_pair = trace_pictures(trace, probe['packets'], slices)
            actual_b = sum(p['pict_type'] == 'B' for p in pictures)
            assert actual_b == sum(a['slices'][0]['slice_type'] % 5 == 1 for a in aus) > 0
            manifest['cases'].append(dict(stem=stem, slices=slices, range=label,
                actual_b_pictures=actual_b, same_frame_num_b_pair=b_pair, aus=aus, pictures=pictures,
                poc_lsb_bits=values('log2_max_pic_order_cnt_lsb_minus4')[0] + 4,
                first_mb=first[:slices], frame_num_bits=values('log2_max_frame_num_minus4')[0] + 4,
                pic_width_in_mbs=values('pic_width_in_mbs_minus1')[0] + 1,
                pic_height_in_map_units=values('pic_height_in_map_units_minus1')[0] + 1,
                media_sha256=digest(media), decoded_sha256=digest(decoded),
                reference_sha256=digest(reference), annexb_sha256=digest(annexb),
                trace_sha256=digest(root / (stem + '.trace.log')), packet_pts=pts,
                time_base=probe['streams'][0]['time_base'], encode_command=command if flag else None))
        assert (root / f'slices{slices}-full.yuv').read_bytes() == (root / f'slices{slices}-limited.yuv').read_bytes()
    (root / 'fixture.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (root / 'prefix-goldens.json').write_text(json.dumps(frozen, indent=2) + '\n')
    print(json.dumps(dict(status='SOURCE_ONLY_PASS', cases=len(manifest['cases']),
                         frames=frames, actual_b_pictures=[c['actual_b_pictures'] for c in manifest['cases']]), indent=2))


if __name__ == '__main__':
    main()
