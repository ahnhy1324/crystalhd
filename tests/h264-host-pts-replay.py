#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replay actual cycle-probe JSON logs with synthetic source-PTS metadata.

No hardware or renderer. The original native run/stream is never rewritten.
Missing/duplicate/nonmonotonic/wrapped PTS are caller-metadata simulations,
not claims that those source files or audio-clock cases were run on a card.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('hostmap', ROOT/'tests/h264-host-pts-map.py')
H = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(H)


def replay(events, metadata, variant):
    provided, expected, pictures = [], {}, []
    gop = -1
    def provider(ordinal):
        nonlocal gop
        assert ordinal == len(provided)+1
        provided.append(ordinal)
        h = metadata['original_aus'][(ordinal-1) % 180]
        if h['idr']:
            gop += 1
        time_base = tuple(map(int, metadata['source_time_base'].split('/')))
        pts, dts = h['pts'], h['dts']
        if variant == 'duplicate':
            pts = 777
        elif variant == 'missing':
            pts = None if ordinal % 7 == 0 else pts
            dts = None if ordinal % 5 == 0 else dts
        elif variant == 'nonmonotonic':
            pts = 100000-ordinal*37 if ordinal % 2 else -ordinal*11
        elif variant == 'wrap':
            pts = ((1 << 33)-9009+ordinal*3003) % (1 << 33)
            dts = None; time_base = (1, 90000)
        source = H.Source(ordinal, ordinal*100000, pts, dts, h['poc_lsb'],
            h['frame_num'], h['reference'], h['idr'], time_base, gop, (ordinal-1)//180+1)
        expected[ordinal] = source
        return source
    adapter = H.TraceMapper(provider, generation='replay-channel', capacity=4096)
    for event in events:
        if event['type'] == 'cycle-input':
            kind = 'early-prefix' if event['kind'] == 'reserved-original-prefix' else event['kind']
            adapter.consume(dict(event, type='input-call', kind=kind, status=0))
        elif event['type'] == 'cycle-picture':
            index = len(pictures); pictures.append(event)
            adapter.consume({'type': 'pts-attribution', 'actual_token': event['token'],
                'flags': event['flags']}, index)
    mapped = adapter.snapshot()
    assert not mapped['rejected'] and not mapped['remaining_owners'] and not mapped['pending_marker']
    originals = [r for r in mapped['outputs'] if r['kind'] == 'original']
    privates = [r for r in mapped['outputs'] if r['kind'] == 'private']
    for row in originals:
        # Validation only, after the independent input/token mapping finished.
        picture = pictures[row['observed_index']]
        source = expected[row['au_id']]
        assert picture['full_native_reference_match']
        assert row['canonical_token'] == picture['validator_input_owner']
        assert row['effective_pts'] == source.pts and row['source_dts'] == source.dts
        assert row['time_base'] == list(source.time_base) and row['source_gop'] == source.gop_id
        assert row['source_pts_present'] == (source.pts is not None)
    result = next(e for e in events if e['type'] == 'cycle-result')
    assert len(originals) == result['originals'] and len(privates) == result['private']
    return {'variant': variant, 'scope': 'OFFLINE caller-metadata replay only; no new native/audio-clock test',
        'original_owners': len(originals), 'private_outputs_hidden': len(privates),
        'source_pts_present': sum(r['source_pts_present'] for r in originals),
        'alias_outputs': sum(r['raw_native_token'] != r['canonical_token'] for r in originals),
        'exact_metadata_identity_order': True, 'inferred_timestamps': 0,
        'playback_clock_for_missing_or_wrapped_timestamps': 'NOT RUN'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native_log', type=Path)
    parser.add_argument('input_metadata', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    events = [json.loads(line) for line in args.native_log.read_text().splitlines() if line.startswith('{')]
    metadata = json.loads(args.input_metadata.read_text())
    report = {'schema': 1, 'replay_source': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'source_time_base': metadata['source_time_base'],
        'variants': [replay(events, metadata, name) for name in
            ('original', 'duplicate', 'missing', 'nonmonotonic', 'wrap')]}
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
