#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare complete POC1 multi-slice sources; qualify them with stock FFmpeg.

This owned I_PCM producer is separate from the maintained range reader. No
device is opened. Only complete positive streams are emitted as media; the
deliberately mismatched picture keys are labelled prefix-only CPU controls.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess as sp
import sys
import zlib


HELPER = Path(__file__).with_name('h264-reference-lifecycle.py')
SPEC = importlib.util.spec_from_file_location('poc1_pcm_bits', HELPER)
PCM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PCM)
Bits, annexb = PCM.Bits, PCM.annexb
WIDTH, HEIGHT, FRAME_BITS, IDR_INTERVAL = 256, 96, 4, 48
MACROBLOCKS = WIDTH * HEIGHT // 256
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2
MODES = ('zero', 'top', 'both')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def mode_flags(mode):
    require(mode in MODES, 'unknown delta mode')
    return mode == 'zero', mode != 'top'


def tag(ordinal):
    require(0 <= ordinal < 65536, 'ordinal outside the source tag')
    payload = b'P1' + ordinal.to_bytes(2, 'big')
    return payload + zlib.crc32(payload).to_bytes(4, 'big')


def chroma(ordinal):
    return ((128, 128), (64, 192), (192, 64), (96, 160))[ordinal % 4]


def coded_luma(ordinal, x, y, code):
    # Two spatially separate tags survive arbitrary slice boundaries.
    local_y = y if y < 32 else y - 64 if y >= 64 else -1
    if 8 <= x < 136 and 0 <= local_y < 32:
        bit = (local_y // 8) * 16 + (x - 8) // 8
        return 235 if code[bit // 8] & (1 << (7 - bit % 8)) else 16
    return 16 + (3 * x + 5 * y + 7 * ordinal + (x ^ (y + ordinal))) % 220


def oracle_frame(ordinal):
    # Independent row-major construction, not a decode of our coded samples.
    y = bytearray(16 + (5 * r + 3 * c + 7 * ordinal + (c ^ (ordinal + r))) % 220
                  for r in range(HEIGHT) for c in range(WIDTH))
    code = tag(ordinal)
    for top in (0, 64):
        for bit in range(64):
            value = 235 if code[bit // 8] & (1 << (7 - bit % 8)) else 16
            left, row = 8 + (bit % 16) * 8, top + (bit // 16) * 8
            for dy in range(8):
                y[(row + dy) * WIDTH + left:(row + dy) * WIDTH + left + 8] = bytes([value]) * 8
    u, v = chroma(ordinal)
    return bytes(y) + bytes([u]) * (WIDTH * HEIGHT // 4) + bytes([v]) * (WIDTH * HEIGHT // 4)


def packed_frame(planar):
    require(len(planar) == FRAME_BYTES, 'exact planar oracle extent')
    plane = WIDTH * HEIGHT
    require(len(set(planar[plane:plane * 5 // 4])) == 1 and
            len(set(planar[plane * 5 // 4:])) == 1, 'whole-picture constant chroma')
    packed = bytearray(plane * 2)
    packed[0::2] = planar[:plane]
    packed[1::4] = planar[plane:plane + 1] * (plane // 2)
    packed[3::4] = planar[-1:] * (plane // 2)
    return bytes(packed)


def picture_plan(ordinal, mode, *, equal_field_poc=False):
    always_zero, bottom_present = mode_flags(mode)
    local = ordinal % IDR_INTERVAL
    delta0 = 0 if always_zero or local == 0 else (0, 1, -1, 2, -2)[local % 5]
    delta1 = 0 if equal_field_poc or not bottom_present or always_zero or local == 0 else (0, -1, 1)[local % 3]
    return dict(ordinal=ordinal, idr=local == 0, idr_pic_id=ordinal // IDR_INTERVAL,
                frame_num=local % (1 << FRAME_BITS), frame_num_offset=local // (1 << FRAME_BITS) * (1 << FRAME_BITS),
                delta0=delta0, delta1=delta1,
                expected_top_poc=8 * local + delta0,
                expected_bottom_poc=8 * local + delta0 + delta1)


def build_sps(mode, full):
    always_zero, _ = mode_flags(mode)
    b = Bits()
    b.u(77, 8); b.u(0, 8); b.u(30, 8)  # Main, CAVLC, 8-bit 4:2:0.
    b.ue(0); b.ue(FRAME_BITS - 4); b.ue(1)
    b.u(always_zero, 1); b.se(-1); b.se(0); b.ue(1); b.se(8)
    b.ue(1); b.u(0, 1)  # One sliding reference, no frame_num gaps.
    b.ue(WIDTH // 16 - 1); b.ue(HEIGHT // 16 - 1)
    b.u(1, 1); b.u(1, 1); b.u(0, 1); b.u(1, 1)
    b.u(1, 1); b.u(1, 8); b.u(0, 1)  # Square SAR, no overscan.
    b.u(1, 1); b.u(5, 3); b.u(full, 1); b.u(1, 1)
    b.u(1, 8); b.u(1, 8); b.u(1, 8)  # Declared BT.709, not an RGB oracle.
    b.u(0, 1); b.u(1, 1); b.u(1, 32); b.u(60, 32); b.u(1, 1)
    b.u(0, 1); b.u(0, 1); b.u(0, 1); b.u(1, 1)
    b.u(1, 1); b.ue(0); b.ue(0); b.ue(11); b.ue(11); b.ue(0); b.ue(1)
    return annexb(0x67, b.finish())


def build_pps(mode):
    _, bottom_present = mode_flags(mode)
    b = Bits()
    b.ue(0); b.ue(0); b.u(0, 1); b.u(bottom_present, 1)
    b.ue(0); b.ue(0); b.ue(0); b.u(0, 1); b.u(0, 2)
    b.se(0); b.se(0); b.se(0)
    b.u(1, 1); b.u(0, 1); b.u(0, 1)
    return annexb(0x68, b.finish())


def build_slice(plan, mode, slices, index, *, delta0=None, delta1=None):
    always_zero, bottom_present = mode_flags(mode)
    require(slices in (2, 4) and 0 <= index < slices, 'two/four ordered slices')
    count = MACROBLOCKS // slices
    first = index * count
    b = Bits()
    b.ue(first); b.ue(2); b.ue(0); b.u(plan['frame_num'], FRAME_BITS)
    if plan['idr']:
        b.ue(plan['idr_pic_id'])
    if not always_zero:
        b.se(plan['delta0'] if delta0 is None else delta0)
        if bottom_present:
            b.se(plan['delta1'] if delta1 is None else delta1)
    if plan['idr']:
        b.u(0, 1); b.u(0, 1)
    else:
        b.u(0, 1)  # Sliding-window reference marking.
    b.se(0); b.ue(1)  # Zero QP delta; deblocking disabled at every edge.
    ordinal, code = plan['ordinal'], tag(plan['ordinal'])
    u, v = chroma(ordinal)
    for mb in range(first, first + count):
        mx, my = mb % (WIDTH // 16), mb // (WIDTH // 16)
        b.ue(25); b.align()
        b.bytes(bytes(coded_luma(ordinal, mx * 16 + x, my * 16 + y, code)
                      for y in range(16) for x in range(16)))
        b.bytes(bytes([u]) * 64); b.bytes(bytes([v]) * 64)
    return annexb(0x65 if plan['idr'] else 0x21, b.finish())


def access_unit(ordinal, mode, slices, full, *, equal_field_poc=False):
    plan = picture_plan(ordinal, mode, equal_field_poc=equal_field_poc)
    nals = [annexb(9, b'\x10')]
    if plan['idr']:
        nals.extend((build_sps(mode, full), build_pps(mode)))
    vcl = [build_slice(plan, mode, slices, index) for index in range(slices)]
    return b''.join(nals + vcl), vcl, plan


def prefix_controls(mode, slices, full, *, equal_field_poc=False):
    plan = picture_plan(2, mode, equal_field_poc=equal_field_poc)
    vcl = [build_slice(plan, mode, slices, index) for index in range(slices)]
    prefix = lambda nal: nal[4:132].hex()  # NAL header + bounded EBSP prefix.
    controls = []
    if mode != 'zero':
        changed = list(vcl)
        changed[1] = build_slice(plan, mode, slices, 1, delta0=plan['delta0'] + 1)
        controls.append(dict(name='delta0-only-mismatch', vcl=[prefix(n) for n in changed],
                             changed_slice=1, expected_associated=False))
    if mode == 'both':
        changed = list(vcl)
        changed[1] = build_slice(plan, mode, slices, 1, delta1=plan['delta1'] + 1)
        controls.append(dict(name='delta1-only-mismatch', vcl=[prefix(n) for n in changed],
                             changed_slice=1, expected_associated=False))
    return dict(scope='PREFIX_ONLY_INVALID_AU_CONTROLS_NEVER_SENT_TO_HARDWARE',
                sps=build_sps(mode, full)[4:].hex(), pps=build_pps(mode)[4:].hex(),
                positive=dict(vcl=[prefix(n) for n in vcl], expected_associated=True,
                              picture=plan), negatives=controls)


FIELD = re.compile(r'\]\s+\d+\s+(\S+)\s+[01]+ = (-?\d+)')


def read_trace(text):
    values, rows, current, reference = {}, [], None, None
    for line in text.splitlines():
        match = FIELD.search(line)
        if not match:
            continue
        key, value = match[1], int(match[2])
        values.setdefault(key, []).append(value)
        if key == 'nal_ref_idc':
            reference = value
            continue  # This belongs to the next NAL, not the previous VCL.
        if key == 'forbidden_zero_bit':
            continue
        if key == 'nal_unit_type':
            if current is not None:
                rows.append(current)
            current = dict(nal_unit_type=value, nal_ref_idc=reference) if value in (1, 5) else None
        elif current is not None:
            current[key] = value
    if current is not None:
        rows.append(current)
    return values, rows


def validate_trace(text, mode, slices, full, frames, *, equal_field_poc=False):
    values, rows = read_trace(text)
    always_zero, bottom_present = mode_flags(mode)
    expected_fields = dict(profile_idc=77, pic_order_cnt_type=1,
        delta_pic_order_always_zero_flag=int(always_zero), frame_mbs_only_flag=1,
        num_slice_groups_minus1=0, redundant_pic_cnt_present_flag=0,
        bottom_field_pic_order_in_frame_present_flag=int(bottom_present),
        video_full_range_flag=int(full), num_ref_frames_in_pic_order_cnt_cycle=1,
        offset_for_non_ref_pic=-1, offset_for_top_to_bottom_field=0,
        colour_primaries=1, transfer_characteristics=1, matrix_coefficients=1)
    for key, expected in expected_fields.items():
        require(values.get(key) and set(values[key]) == {expected}, 'independent SPS/PPS trace: ' + key)
    require(len(rows) == frames * slices, 'independent complete VCL census')
    for n, row in enumerate(rows):
        plan = picture_plan(n // slices, mode, equal_field_poc=equal_field_poc)
        expected = dict(nal_unit_type=5 if plan['idr'] else 1,
                        first_mb_in_slice=n % slices * (MACROBLOCKS // slices),
                        slice_type=2, pic_parameter_set_id=0, frame_num=plan['frame_num'])
        if plan['idr']:
            expected['idr_pic_id'] = plan['idr_pic_id']
        for key, value in expected.items():
            require(row.get(key) == value, 'independent slice header: ' + key)
        for key, present, value in (
                ('delta_pic_order_cnt[0]', not always_zero, plan['delta0']),
                ('delta_pic_order_cnt[1]', not always_zero and bottom_present, plan['delta1'])):
            require((key in row) == present and (not present or row[key] == value),
                    'independent conditional signed slice delta: ' + key)
        require(row.get('nal_ref_idc', 0) > 0, 'every generated picture is a reference')


def file_digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def compare_decoded(decoded, expected, frames):
    with decoded.open('rb') as actual, expected.open('rb') as wanted:
        for ordinal in range(frames):
            frame = actual.read(FRAME_BYTES)
            require(len(frame) == FRAME_BYTES and frame == wanted.read(FRAME_BYTES),
                    'independent full planar pixels/source order at ordinal ' + str(ordinal))
        require(not actual.read(1) and not wanted.read(1), 'no missing/extra decoded bytes')


def run_logged(command, log, timeout):
    with log.open('xb') as error:
        sp.run(command, stdout=sp.DEVNULL, stderr=error, check=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new output directory')
    parser.add_argument('--frames', type=int, default=96, help='1..3600; 900+ is supported')
    parser.add_argument('--slices', type=int, nargs='+', choices=(2, 4), default=[2, 4])
    parser.add_argument('--delta-mode', nargs='+', choices=MODES, default=list(MODES))
    parser.add_argument('--equal-field-poc', action='store_true',
                        help='keep delta1 syntax in both mode but emit zero; original variant is unchanged by default')
    parser.add_argument('--range', choices=('full', 'limited', 'both'), default='both')
    parser.add_argument('--ffmpeg', default='ffmpeg')
    parser.add_argument('--ffprobe', default='ffprobe')
    parser.add_argument('--timeout', type=int, default=180, help='per software subprocess, seconds')
    args = parser.parse_args()
    require(1 <= args.frames <= 3600 and 1 <= args.timeout <= 600, 'bounded frame/time options')
    require(len(set(args.slices)) == len(args.slices) and len(set(args.delta_mode)) == len(args.delta_mode),
            'duplicate case options')
    root = args.output.resolve()
    root.mkdir(mode=0o700, exist_ok=False)
    expected, packed = root / 'expected.yuv420p', root / 'expected.yuyv'
    pictures = []
    with expected.open('xb') as planar, packed.open('xb') as yuyv:
        for ordinal in range(args.frames):
            frame = oracle_frame(ordinal)
            planar.write(frame); yuyv.write(packed_frame(frame))
            pictures.append(dict(ordinal=ordinal, tag_hex=tag(ordinal).hex(),
                                 planar_sha256=hashlib.sha256(frame).hexdigest()))
    manifest = dict(kind='native-poc1-multislice-source', width=WIDTH, height=HEIGHT,
        frames=args.frames, source_order='ordinal; no reordered pictures',
        equal_field_poc=args.equal_field_poc,
        frame_num_bits=FRAME_BITS, idr_interval=IDR_INTERVAL, pictures=pictures, cases=[],
        expected_planar_sha256=file_digest(expected), expected_yuyv_sha256=file_digest(packed),
        producer_sha256=file_digest(Path(__file__)), bit_helper_sha256=file_digest(HELPER),
        scope='Owned complete media plus stock software validation; no hardware/VA/release qualification')
    controls = {}
    ranges = (False, True) if args.range == 'both' else (args.range == 'full',)
    for mode in args.delta_mode:
        for slices in args.slices:
            for full in ranges:
                variant = '-equal-field-poc' if args.equal_field_poc else ''
                stem = f'poc1-{mode}{variant}-slices{slices}-' + ('full' if full else 'limited')
                raw, media = root / (stem + '.h264'), root / (stem + '.mp4')
                plans = []
                with raw.open('xb') as stream:
                    for ordinal in range(args.frames):
                        au, _, plan = access_unit(ordinal, mode, slices, full,
                                                 equal_field_poc=args.equal_field_poc)
                        plan.update(offset=stream.tell(), bytes=len(au),
                                    first_mb=[i * (MACROBLOCKS // slices) for i in range(slices)])
                        stream.write(au); plans.append(plan)
                mux = [args.ffmpeg, '-nostdin', '-hide_banner', '-v', 'error', '-fflags', '+genpts',
                    '-r', '30', '-f', 'h264', '-i', str(raw), '-map', '0:v:0', '-c:v', 'copy',
                    '-video_track_timescale', '30000', str(media)]
                run_logged(mux, root / (stem + '.mux.log'), args.timeout)
                trace = [args.ffmpeg, '-nostdin', '-hide_banner', '-i', str(media), '-map', '0:v:0',
                    '-c:v', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-']
                trace_log = root / (stem + '.trace.log')
                run_logged(trace, trace_log, args.timeout)
                validate_trace(trace_log.read_text(), mode, slices, full, args.frames,
                               equal_field_poc=args.equal_field_poc)
                decoded = root / (stem + '.decoded.yuv')
                decode = [args.ffmpeg, '-nostdin', '-hide_banner', '-v', 'error', '-threads', '1',
                    '-hwaccel', 'none', '-err_detect', 'explode', '-xerror', '-i', str(media),
                    '-map', '0:v:0', '-an', '-sn', '-dn', '-fps_mode', 'passthrough', '-threads', '1',
                    '-pix_fmt', 'yuvj420p' if full else 'yuv420p', '-f', 'rawvideo', str(decoded)]
                decode_log = root / (stem + '.decode.log')
                run_logged(decode, decode_log, args.timeout)
                require(decode_log.stat().st_size == 0, 'no software decode error diagnostics')
                compare_decoded(decoded, expected, args.frames)
                probe = [args.ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_frames',
                    '-show_streams', '-show_entries',
                    'frame=pts,pict_type,key_frame,width,height:stream=time_base,has_b_frames,color_range',
                    '-of', 'json', str(media)]
                inspected = sp.run(probe, capture_output=True, text=True, check=True, timeout=args.timeout)
                require(not inspected.stderr, 'no independent probe error diagnostics')
                (root / (stem + '.probe.json')).write_text(inspected.stdout)
                data = json.loads(inspected.stdout)
                shown, streams = data['frames'], data['streams']
                require(len(streams) == 1 and streams[0]['has_b_frames'] == 0 and
                        streams[0]['time_base'] == '1/30000', 'software container/timebase contract')
                require(len(shown) == args.frames and
                        [f['pts'] for f in shown] == [i * 1000 for i in range(args.frames)],
                        'independent exact finite source PTS/order')
                require(all((f['width'], f['height'], f['pict_type']) == (WIDTH, HEIGHT, 'I') and
                            f['key_frame'] == int(i % IDR_INTERVAL == 0) for i, f in enumerate(shown)),
                        'independent geometry/I-picture/IDR census')
                controls[stem] = prefix_controls(mode, slices, full,
                                                equal_field_poc=args.equal_field_poc)
                manifest['cases'].append(dict(stem=stem, slices=slices, delta_mode=mode,
                    full_range=full, equal_field_poc=args.equal_field_poc,
                    access_units=plans, frame_pts=[f['pts'] for f in shown],
                    time_base=streams[0]['time_base'], annexb_sha256=file_digest(raw),
                    media_sha256=file_digest(media), decoded_sha256=file_digest(decoded),
                    trace_sha256=file_digest(trace_log), commands=[mux, trace, decode, probe]))
    (root / 'prefix-goldens.json').write_text(json.dumps(controls, indent=2) + '\n')
    manifest['status'] = 'SOFTWARE_SOURCE_QUALIFICATION_PASS'
    (root / 'fixture.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(dict(status=manifest['status'], frames=args.frames, cases=len(manifest['cases']))))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, sp.SubprocessError) as error:
        print('POC1 fixture qualification failed: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
