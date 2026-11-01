#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline #533 discriminator; never opens hardware or qualifies STOP/START.

Reuse the frozen G0 packets and private non-reference I16x16/DC builder.
Neighbor frame_num cases are invalid progressive/no-gap negative controls;
permissive software decoding does not prove bitstream conformance.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('stop_kick', ROOT / 'tests/h264-stop-kick.py')
G0 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(G0)


def nals(data):
    starts = list(re.finditer(b'\x00{2,}\x01', data))
    assert starts and starts[0].start() == 0
    return [data[m.end():starts[i + 1].start() if i + 1 < len(starts) else len(data)]
            for i, m in enumerate(starts)]


def packets(directory):
    data = (directory / 'original20.packets').read_bytes()
    assert data[:8] == b'CHDKICK1' and struct.unpack_from('<I', data, 8)[0] == 20
    cursor, result = 12, []
    for index in range(20):
        size, token, pts = struct.unpack_from('<IQq', data, cursor)
        cursor += 20
        assert token == (index + 1) * 100000
        assert 0 < size <= 1024 * 1024 and cursor + size <= len(data)
        result.append(data[cursor:cursor + size])
        cursor += size
    assert cursor == len(data)
    assert b''.join(result) == (directory / 'original20.h264').read_bytes()
    return result


def reorder_bound(pocs):
    return max(sum(previous > current for previous in pocs[:i])
               for i, current in enumerate(pocs))


def software_decode(path, frame_bytes):
    result = subprocess.run(
        ['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-xerror',
         '-err_detect', 'explode', '-threads', '1', '-i', str(path),
         '-map', '0:v:0', '-fps_mode', 'passthrough', '-pix_fmt', 'yuv420p',
         '-f', 'rawvideo', '-'], capture_output=True, timeout=30)
    error = result.stderr.decode(errors='replace')
    if result.returncode or error or len(result.stdout) % frame_bytes:
        return [], {'exit': result.returncode, 'stderr': error[:2000],
                    'partial_bytes': len(result.stdout)}
    return [result.stdout[i:i + frame_bytes] for i in range(0, len(result.stdout), frame_bytes)], None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('g0_directory', type=Path)
    parser.add_argument('output_directory', type=Path)
    args = parser.parse_args()
    args.output_directory.mkdir(mode=0o700, exist_ok=False)
    frozen = json.loads((args.g0_directory / 'g0.json').read_text())
    oracle = next(line.split('\t') for line in (ROOT / 'tests/phase1-oracle.tsv').read_text().splitlines()
                  if line.startswith('round\th264-high-ipb\t'))
    assert frozen['fixture_sha256'] == oracle[4]
    units = packets(args.g0_directory)
    first = nals(units[0])
    sequences = {v['id']: v for v in (G0.sps(n) for n in first if n[0] & 31 == 7)}
    parameters = {v['id']: v for v in (G0.pps(n) for n in first if n[0] & 31 == 8)}
    assert len(sequences) == len(parameters) == 1 and 254 not in parameters
    sequence = next(iter(sequences.values()))
    assert sequence == frozen['sps'] and sequence['gaps'] == 0
    headers = []
    for index, unit in enumerate(units):
        slices = [G0.slice_header(n, sequences, parameters)
                  for n in nals(unit) if n[0] & 31 in (1, 5)]
        assert slices and slices[0]['first_mb'] == 0
        header = slices[0]
        assert all({k: v for k, v in h.items() if k != 'first_mb'} ==
                   {k: v for k, v in header.items() if k != 'first_mb'} for h in slices)
        assert all(header[k] == frozen['original_aus'][index][k] for k in header)
        headers.append(dict(header, poc=frozen['original_aus'][index]['poc']))
    # This fixture has no POC wrap. Original MMCO1 evictions remain untouched;
    # the imported header parser rejects MMCO5's different POC derivation.
    assert all(h['poc'] == h['poc_lsb'] for h in headers)
    width, height = 640, 360
    frame_bytes = width * height * 3 // 2
    baseline, error = software_decode(args.g0_directory / 'original20.h264', frame_bytes)
    assert not error and len(baseline) == len(set(baseline)) == 20
    neutral = bytes([128]) * frame_bytes
    assert neutral not in baseline
    original_pocs = [h['poc'] for h in headers]
    report = {'schema': 1, 'issue': 533,
              'scope': 'OFFLINE ONLY: original pixel/order and syntax-envelope screening; no native progress, PPB lifetime or STOP/START proof',
              'frozen_fixture_sha256': frozen['fixture_sha256'],
              'original_packets_equal_frozen_annexb': True,
              'sps': sequence, 'software_baseline_pictures': 20,
              'ffmpeg': subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0],
              'frame_num_rule': 'H.264 (01/2012) 7.4.3: this progressive, non-IDR, gaps=0 picture uses (PrevRefFrameNum+1)%MaxFrameNum. Neighbor values are invalid controls even if software accepts them.',
              'syntax_reference': 'https://www.itu.int/rec/dologin_pub.asp?id=T-REC-H.264-201201-S%21%21PDF-E&lang=e&type=items',
              'cases': [], 'native': 'NOT RUN'}
    for boundary in (12, 15, 17, 18):
        prefix = headers[:boundary]
        last_reference = next(h for h in reversed(prefix) if h['reference'])
        expected_frame = (last_reference['frame_num'] + 1) % (1 << sequence['frame_bits'])
        first_pocs = sorted({last_reference['poc'] + offset for offset in (1, 3)} |
                            {max(original_pocs[:boundary]) + offset for offset in (1, 3)})
        combinations = [(expected_frame, poc, count, False)
                        for poc in first_pocs for count in (1, 2)]
        combinations += [((expected_frame + offset) % (1 << sequence['frame_bits']),
                          first_pocs[0], 1, True) for offset in (-1, 1)]
        order_safe = sorted(original_pocs[:boundary]) + sorted(original_pocs[boundary:]) == sorted(original_pocs)
        # Larger candidates must first satisfy both the original reorder
        # restriction and the independently ordered continuation boundary.
        # In particular, do not rerun the known-invalid frozen third dummy.
        if order_safe:
            combinations += [(expected_frame, poc, count, False)
                             for poc in first_pocs for count in (3, 4, 5)
                             if reorder_bound(original_pocs[:boundary] +
                                              [poc + 2 * i for i in range(count)] +
                                              original_pocs[boundary:]) <= sequence['max_num_reorder_frames']]
        for frame_num, first_poc, count, negative in combinations:
            pocs = [first_poc + 2 * i for i in range(count)]
            assert len(set(pocs)) == count and not set(pocs).intersection(original_pocs)
            assert all(0 <= p < (1 << sequence['poc_bits']) and
                       abs(p - last_reference['poc']) < (1 << (sequence['poc_bits'] - 1)) for p in pocs)
            private = b''.join(G0.private_picture(sequence, frame_num, poc) for poc in pocs)
            private_nals = nals(private)
            assert all(n[0] & 31 in (1, 8, 9) for n in private_nals)
            private_pps = {p['id']: p for p in (G0.pps(n) for n in private_nals if n[0] & 31 == 8)}
            assert set(private_pps) == {254} and private_pps[254]['sps'] == sequence['id']
            private_headers = [G0.slice_header(n, sequences, private_pps)
                               for n in private_nals if n[0] & 31 == 1]
            assert len(private_headers) == count
            assert all(not h['reference'] and not h['idr'] and not h['mmco'] and
                       h['kind'] == 2 and h['frame_num'] == frame_num and h['poc_lsb'] == poc
                       for h, poc in zip(private_headers, pocs))
            name = 'b%d-f%d-p%d-n%d' % (boundary, frame_num, first_poc, count)
            path = args.output_directory / (name + '.h264')
            path.write_bytes(b''.join(units[:boundary]) + private + b''.join(units[boundary:]))
            decoded, error = software_decode(path, frame_bytes)
            real = [f for f in decoded if f != neutral]
            actual_private = len(decoded) - len(real)
            bound = reorder_bound(original_pocs[:boundary] + pocs + original_pocs[boundary:])
            software_ok = not error and real == baseline and actual_private == count
            syntax_envelope = not negative and bound <= sequence['max_num_reorder_frames']
            report['cases'].append({
                'name': name, 'boundary': boundary, 'private_frame_num': frame_num,
                'private_pocs': pocs, 'negative_frame_num_control': negative,
                'required_reorder_bound': bound, 'within_original_reorder_limit': bound <= sequence['max_num_reorder_frames'],
                'segment_order_equals_uninterrupted': order_safe,
                # A rejected decoder invocation has partial raw bytes in the
                # error receipt, not a trustworthy complete picture census.
                'software_pictures': len(decoded) if not error else None,
                'software_private_outputs': actual_private if not error else None,
                'original_pixels_and_order_unchanged': real == baseline if not error else None,
                'missing_original_display_indices': [i for i, f in enumerate(baseline) if f not in real] if not error else None,
                'software_originals_preserved': software_ok, 'decode_error': error,
                'screened_progress_candidate': software_ok and syntax_envelope,
                'screened_order_safe_candidate': software_ok and syntax_envelope and order_safe,
                'native': 'NOT RUN'})
            (args.output_directory / 'matrix.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'cases': len(report['cases']),
                      'originals_preserved': sum(c['software_originals_preserved'] for c in report['cases']),
                      'progress_candidates': sum(c['screened_progress_candidate'] for c in report['cases']),
                      'order_safe_candidates': sum(c['screened_order_safe_candidate'] for c in report['cases']),
                      'native': 'NOT RUN'}, indent=2))


if __name__ == '__main__':
    main()
