#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline #533 complete-sequence screen for repeated input-only pauses.

Original compressed AUs and source PTS stay intact. Repeated source copies
have separate diagnostic API identities; no playback timestamps are inferred.
No device is opened. This is not V4L2 STOP/START or a production Kick policy.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('matrix', ROOT/'tests/h264-dummy-matrix.py')
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)
G = M.G0


def software_hashes(path):
    run = subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error',
        '-xerror', '-err_detect', 'explode', '-threads', '1', '-i', str(path),
        '-map', '0:v:0', '-fps_mode', 'passthrough', '-pix_fmt', 'yuv420p',
        '-f', 'framemd5', '-'], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0 and not run.stderr, run.stderr
    rows = [line.split(',') for line in run.stdout.splitlines() if not line.startswith('#')]
    assert rows and all(len(row) == 6 and int(row[4]) == 640*360*3//2 for row in rows)
    return [row[-1].strip() for row in rows]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('frozen_g0', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    frozen = json.loads((args.frozen_g0/'g0.json').read_text())
    assert hashlib.sha256(args.fixture.read_bytes()).hexdigest() == frozen['fixture_sha256']
    run = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
        '-show_streams', '-show_packets', '-show_data', '-show_entries',
        'stream=width,height,extradata:packet=pts,dts,data,size', '-of', 'json',
        str(args.fixture)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0 and not run.stderr
    probe = json.loads(run.stdout)
    stream = probe['streams'][0]
    assert (stream['width'], stream['height']) == (640, 360)
    extra = G.dump_bytes(stream['extradata'])
    assert extra[0] == 1
    length_size, cursor = (extra[4] & 3)+1, 6
    sequences, parameters = [], []
    for _ in range(extra[5] & 31):
        size = int.from_bytes(extra[cursor:cursor+2], 'big'); cursor += 2
        sequences.append(extra[cursor:cursor+size]); cursor += size
    count = extra[cursor]; cursor += 1
    for _ in range(count):
        size = int.from_bytes(extra[cursor:cursor+2], 'big'); cursor += 2
        parameters.append(extra[cursor:cursor+size]); cursor += size
    ss = {v['id']: v for v in map(G.sps, sequences)}
    pp = {v['id']: v for v in map(G.pps, parameters)}
    assert len(ss) == len(pp) == 1 and 254 not in pp
    sequence = next(iter(ss.values()))
    assert sequence == frozen['sps'] and sequence['gaps'] == 0
    initial = b''.join(b'\0\0\0\1'+n for n in sequences+parameters)
    units, headers = [], []
    for index, packet in enumerate(probe['packets']):
        data = G.dump_bytes(packet['data']); assert len(data) == int(packet['size'])
        cursor, ns = 0, []
        while cursor < len(data):
            size = int.from_bytes(data[cursor:cursor+length_size], 'big'); cursor += length_size
            assert 0 < size <= len(data)-cursor
            ns.append(data[cursor:cursor+size]); cursor += size
        hs = [G.slice_header(n, ss, pp) for n in ns if n[0] & 31 in (1, 5)]
        assert hs and hs[0]['first_mb'] == 0
        assert all({k: v for k, v in h.items() if k != 'first_mb'} ==
                   {k: v for k, v in hs[0].items() if k != 'first_mb'} for h in hs)
        units.append((initial if index == 0 else b'')+b''.join(b'\0\0\0\1'+n for n in ns))
        headers.append(dict(hs[0], pts=packet['pts'], dts=packet['dts'], source_au=index+1))
    assert len(units) == 180
    # The first20 AU payloads are precisely the existing P1 source, including
    # the original first-AU parameter sets. No regenerated source VCL.
    assert units[:20] == M.packets(args.frozen_g0)
    keys = [i for i, h in enumerate(headers) if h['idr']]
    assert keys == list(range(0, 180, 30))
    plans, rejected = [], []
    for generation, start in enumerate(keys):
        hs = headers[start:start+30]
        # This fixture's closed30-AU GOPs have no POC wrap or MMCO5.
        assert hs[0]['idr'] and not any(h['idr'] for h in hs[1:])
        assert all(h['poc_lsb'] < (1 << (sequence['poc_bits']-1))*2 for h in hs)
        pocs = [h['poc_lsb'] for h in hs]
        assert len(set(pocs)) == 30 and min(pocs) == 0 and max(pocs) == 58
        eligible = []
        for candidate in range(6, 31):
            ref = next(h for h in reversed(hs[:candidate]) if h['reference'])
            order_safe = sorted(pocs[:candidate])+sorted(pocs[candidate:]) == sorted(pocs)
            for first_poc in range(1, (1 << sequence['poc_bits'])-8, 2):
                ps = [first_poc+2*i for i in range(5)]
                if any(abs(p-ref['poc_lsb']) >= (1 << (sequence['poc_bits']-1)) for p in ps):
                    continue
                required = M.reorder_bound(pocs[:candidate]+ps+pocs[candidate:])
                if required <= sequence['max_num_reorder_frames'] and order_safe:
                    eligible.append((candidate, first_poc))
                else:
                    rejected.append(dict(gop=generation, boundary=candidate,
                        first_private_poc=first_poc, required_reorder_bound=required,
                        original_order_boundary_safe=order_safe,
                        native='NOT RUN: outside unchanged syntax/order envelope'))
        if not eligible:
            print(json.dumps(dict(type='offline-rejection', gop=generation,
                source_pocs=pocs, candidates=[r for r in rejected if r['gop']==generation]), indent=2), flush=True)
        assert eligible, 'no independently valid five-private insertion in this GOP'
        boundary, first_poc = eligible[generation % len(eligible)]
        last_reference = next(h for h in reversed(hs[:boundary]) if h['reference'])
        frame_num = (last_reference['frame_num']+1) % (1 << sequence['frame_bits'])
        private_pocs = [first_poc+2*i for i in range(5)]
        assert not set(private_pocs).intersection(pocs)
        assert len(set(private_pocs)) == 5 and all(0 <= p < (1 << sequence['poc_bits']) for p in private_pocs)
        assert all(abs(p-last_reference['poc_lsb']) < (1 << (sequence['poc_bits']-1)) for p in private_pocs)
        required = M.reorder_bound(pocs[:boundary]+private_pocs+pocs[boundary:])
        assert required <= sequence['max_num_reorder_frames']
        assert sorted(pocs[:boundary])+sorted(pocs[boundary:]) == sorted(pocs)
        private = b''.join(G.private_picture(sequence, frame_num, poc) for poc in private_pocs)
        ns = M.nals(private)
        assert all(n[0] & 31 in (1, 8, 9) for n in ns)
        private_pps = {p['id']: p for p in (G.pps(n) for n in ns if n[0] & 31 == 8)}
        assert set(private_pps) == {254} and private_pps[254]['sps'] == sequence['id']
        private_headers = [G.slice_header(n, ss, private_pps) for n in ns if n[0] & 31 == 1]
        assert len(private_headers) == 5
        assert all(not h['reference'] and not h['idr'] and not h['mmco'] and h['kind'] == 2 and
                   h['frame_num'] == frame_num and h['poc_lsb'] == poc
                   for h, poc in zip(private_headers, private_pocs))
        plans.append(dict(boundary=start+boundary, gop=generation, gop_boundary=boundary,
            private_frame_num=frame_num, private_pocs=private_pocs,
            required_reorder_bound=required, private_bytes=len(private), eligible_boundaries=eligible))
    args.output.mkdir(mode=0o700, exist_ok=False)
    (args.output/'original180.h264').write_bytes(b''.join(units))
    container = bytearray(b'CHDCYCL1'+struct.pack('<I', 180))
    for i, (unit, h) in enumerate(zip(units, headers)):
        container.extend(struct.pack('<IQq', len(unit), (i+1)*100000, h['pts']))
        container.extend(unit)
    (args.output/'original180.packets').write_bytes(container)
    (args.output/'prefix.h264').write_bytes((args.frozen_g0/'prefix.h264').read_bytes())
    for row in plans:
        (args.output/('private-%d.h264' % row['boundary'])).write_bytes(b''.join(
            G.private_picture(sequence, row['private_frame_num'], poc) for poc in row['private_pocs']))
    (args.output/'cycles.tsv').write_text(''.join('%d\t5\n' % p['boundary'] for p in plans))
    baseline = software_hashes(args.output/'original180.h264')
    neutral = hashlib.md5(bytes([128])*(640*360*3//2)).hexdigest()
    assert len(baseline) == 180 and neutral not in baseline
    report = dict(scope='Offline complete-sequence syntax-envelope and software pixel/order screen; not native scheduling, physical PPB ownership or STOP/START',
        source_fixture_sha256=frozen['fixture_sha256'], sps=sequence,
        original_aus=headers, original180_pixels=baseline, insertion_plans=plans,
        rejected_boundaries=rejected,
        source_au_bytes_and_pts_unchanged=True, copies='Repeat unchanged original180 AUs; each copy starts with its original IDR/SPS/PPS. Source media PTS preserved per-copy. Native API identity generations are separate diagnostic tokens.',
        cases=[], native='NOT RUN')
    # The stages are independently screened before hardware. Each selected
    # GOP has one insertion, so no private POC is blindly reused within a GOP.
    for cycles in (2, 5, 10, 100):
        repeats = (cycles+5)//6
        path = args.output/('cycles-%d.h264' % cycles)
        data, transport, inserted = bytearray(), bytearray(), 0
        by_boundary = {p['boundary']: p for p in plans}
        for repeat in range(repeats):
            for i, unit in enumerate(units, 1):
                data.extend(unit)
                transport.extend(unit)
                if i in by_boundary and inserted < cycles:
                    data.extend((args.output/('private-%d.h264' % i)).read_bytes())
                    row = by_boundary[i]
                    for poc in row['private_pocs']:
                        transport.extend((args.frozen_g0/'prefix.h264').read_bytes())
                        transport.extend(G.private_picture(sequence, row['private_frame_num'], poc))
                    transport.extend((args.frozen_g0/'prefix.h264').read_bytes())
                    inserted += 1
        assert inserted == cycles
        path.write_bytes(data)
        actual = software_hashes(path)
        transport_path = args.output/('cycles-%d-transport.h264' % cycles)
        transport_path.write_bytes(transport)
        assert software_hashes(transport_path) == actual
        originals = [h for h in actual if h != neutral]
        assert originals == baseline*repeats and actual.count(neutral) == 5*cycles
        report['cases'].append(dict(cycles=cycles, repeats=repeats,
            originals=180*repeats, private=5*cycles, total=len(actual),
            original_pixels_and_order=True, private_count=True,
            actual_prefix_transport_same_pixels_and_order=True,
            whole_sequence_sha256=hashlib.sha256(data).hexdigest(),
            prefix_transport_sha256=hashlib.sha256(transport).hexdigest(),
            required_reorder_bound=max(p['required_reorder_bound'] for p in plans),
            native='NOT RUN'))
        (args.output/'offline.json').write_text(json.dumps(report, indent=2)+'\n')
        print('OFFLINE PASS', cycles, 'pauses', len(actual), 'pictures', flush=True)


if __name__ == '__main__':
    main()
