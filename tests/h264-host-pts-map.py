#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Experimental #533 host metadata mapping; no device or presentation API.

A unique prefed native identity may be reserved before source PTS is known,
then bound to the actual next accepted original VCL. Output lookup uses that
identity, never image hashes, fixture POC rank, display ordinal or FIFO guesses.
Raw firmware tokens remain unchanged. Zero/colliding identities fail closed.
This bounded progressive prototype is not a production library policy.
"""
import argparse
import json
from pathlib import Path
import subprocess
from typing import NamedTuple, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]


class Unsupported(ValueError):
    pass


class Source(NamedTuple):
    au_id: int
    canonical_token: int
    pts: Optional[int]
    dts: Optional[int]
    poc: int
    frame_num: int
    reference: int
    idr: bool
    time_base: Tuple[int, int]
    gop_id: int = 0
    source_copy: int = 0


class HostMap:
    """One generation, one outstanding early marker; bounded finite experiment."""
    def __init__(self, generation, capacity=64):
        if not generation or not 1 <= capacity <= 4096:
            raise Unsupported('invalid generation/capacity')
        self.generation, self.capacity = generation, capacity
        self.records, self.used, self.pending_marker = {}, set(), None
        self.original_ids, self.published = set(), []

    def check(self, generation):
        if generation != self.generation:
            raise Unsupported('stale decoder generation')

    def room(self):
        if len(self.used) >= self.capacity:
            raise Unsupported('bounded experiment identity capacity')

    def reserve(self, token, generation):
        self.check(generation)
        if not token or token % 20000 or token in self.used or self.pending_marker is not None:
            raise Unsupported('zero, unaligned, colliding or overlapping early marker')
        self.room(); self.used.add(token)
        self.records[token] = None
        self.pending_marker = token

    def original(self, source, generation):
        self.check(generation)
        if source.au_id <= 0 or source.au_id in self.original_ids or not source.canonical_token or source.canonical_token % 20000 or source.time_base[0] <= 0 or source.time_base[1] <= 0:
            raise Unsupported('duplicate original owner or invalid timebase')
        key = self.pending_marker if self.pending_marker is not None else source.canonical_token
        if source.canonical_token in self.used and source.canonical_token != self.pending_marker:
            raise Unsupported('source token collides with an existing native owner')
        if self.pending_marker is None:
            self.room(); self.used.add(key)
        elif source.canonical_token != key:
            self.room(); self.used.add(source.canonical_token)
        self.records[key] = source
        self.original_ids.add(source.au_id)
        self.pending_marker = None
        return key

    def private(self, token, generation):
        self.check(generation)
        if not token or token % 20000 or token in self.used or self.pending_marker is not None:
            raise Unsupported('ambiguous private owner/intervening VCL')
        self.room(); self.used.add(token); self.records[token] = 'private'

    def output(self, raw_token, generation):
        self.check(generation)
        if raw_token not in self.records:
            raise Unsupported('unknown or already retired native owner')
        owner = self.records[raw_token]
        if owner is None:
            raise Unsupported('early marker returned before its original VCL was accepted')
        del self.records[raw_token]
        if owner == 'private':
            return {'kind': 'private', 'raw_native_token': raw_token, 'display': False}
        result = {'kind': 'original', 'au_id': owner.au_id,
            'raw_native_token': raw_token, 'canonical_token': owner.canonical_token,
            'effective_pts': owner.pts, 'source_dts': owner.dts,
            'source_poc': owner.poc, 'time_base': list(owner.time_base),
            'source_gop': owner.gop_id, 'source_copy': owner.source_copy,
            'generation': generation, 'source_pts_present': owner.pts is not None}
        self.published.append(result)
        return result


def source_record(header, time_base=(1, 15360)):
    # Called only at the actual accepted-input event, not when reserving an
    # early marker. PTS/DTS/POC come from that compressed input's own metadata.
    return Source(header['index']+1, (header['index']+1)*100000,
        header.get('pts'), header.get('dts'), header['poc'], header['frame_num'],
        header['reference'], header['idr'], time_base)


class TraceMapper:
    """Live/offline adapter. Any ambiguity stops metadata publication.

    The caller still owns all native buffers and must drain/close them even
    after this adapter rejects a mapping. This class never releases a lease.
    """
    def __init__(self, source_provider, generation='frozen-channel', capacity=64):
        self.provider, self.generation = source_provider, generation
        self.mapper = HostMap(generation, capacity)
        self.private_prefixes, self.outputs, self.bindings, self.rejected = {}, [], [], []
        self.failed = None

    def consume(self, event, observed_index=None):
        if self.failed is not None:
            return None
        try:
            if event['type'] == 'input-call':
                if event['status'] != 0:
                    raise Unsupported('input was not accepted')
                kind, index = event['kind'], event['association']
                if kind == 'original':
                    if not event['vcl_present'] or self.private_prefixes:
                        raise Unsupported('original VCL missing or unfinished private input')
                    source = self.provider(index)
                    key = self.mapper.original(source, self.generation)
                    self.bindings.append({'au_id': source.au_id, 'expected_native_owner': key,
                        'canonical_token': source.canonical_token, 'source_pts': source.pts,
                        'bound_only_after_actual_input': True})
                elif kind == 'private-prefix':
                    if self.mapper.pending_marker is not None or event['vcl_present'] or index in self.private_prefixes:
                        raise Unsupported('private prefix overwrites an unbound owner')
                    self.private_prefixes[index] = event['token']
                elif kind == 'private-vcl':
                    if not event['vcl_present'] or index not in self.private_prefixes:
                        raise Unsupported('private VCL lacks its own accepted prefix')
                    self.mapper.private(self.private_prefixes.pop(index), self.generation)
                elif kind == 'early-prefix':
                    if event['vcl_present'] or self.private_prefixes:
                        raise Unsupported('early original marker contains VCL or unfinished private input')
                    if event['token']:
                        self.mapper.reserve(event['token'], self.generation)
            elif event['type'] == 'pts-attribution':
                # This study is progressive. Field-pair identity and unknown
                # source types require their own validated adapter.
                if event['flags'] & (0x18 | 0x20 | 0x40):
                    raise Unsupported('field/unknown-source mapping is not qualified')
                result = self.mapper.output(event['actual_token'], self.generation)
                result['observed_index'] = observed_index
                self.outputs.append(result)
                return result
        except Unsupported as error:
            self.failed = str(error)
            self.rejected.append({'reason': self.failed, 'event': event})
        return None

    def snapshot(self):
        return {'outputs': self.outputs, 'rejected': self.rejected, 'binding_error': self.failed,
            'remaining_owners': len(self.mapper.records), 'pending_marker': self.mapper.pending_marker,
            'bindings': self.bindings}


def self_test():
    count = 0
    def reject(fn):
        nonlocal count
        try:
            fn()
        except Unsupported:
            count += 1
        else:
            raise AssertionError('unsafe mapping accepted')
    a = Source(19, 1900000, 21504, 17408, 42, 12, 2, False, (1, 15360))
    m = HostMap('g'); m.reserve(2120000, 'g')
    assert not m.original_ids and not m.published  # no next-AU metadata yet
    reject(lambda: m.output(2120000, 'g'))
    reject(lambda: m.private(2100000, 'g'))
    reject(lambda: m.reserve(2140000, 'g'))
    reject(lambda: m.output(2120000, 'old-generation'))
    m.original(a, 'g'); result = m.output(2120000, 'g')
    assert result['raw_native_token'] == 2120000 and result['effective_pts'] == a.pts
    reject(lambda: m.output(2120000, 'g'))
    reject(lambda: m.output(0, 'g'))
    reject(lambda: m.original(a, 'g'))
    reject(lambda: m.reserve(2120000, 'g'))
    for marker in (0, 2120001):
        reject(lambda marker=marker: HostMap('g').reserve(marker, 'g'))
    m = HostMap('g'); m.original(a, 'g')
    reject(lambda: m.reserve(1900000, 'g'))
    # Zero still cannot identify a frame when precisely one original remains.
    reject(lambda: m.output(0, 'g'))
    # Missing, duplicate, negative and wrap-sized PTS remain source metadata,
    # never an ownership key or a frame-rate-derived replacement.
    for pts in (None, 0, -1024, 2**33-1, 1024, 1024):
        m = HostMap('g'); m.reserve(2120000, 'g')
        s = Source(19, 1900000, pts, None, 42, 12, 2, False, (1, 90000))
        m.original(s, 'g'); out = m.output(2120000, 'g')
        assert out['effective_pts'] == pts and out['time_base'] == [1, 90000]
        count += 1
    m = HostMap('g', capacity=1); m.private(2020000, 'g')
    reject(lambda: m.reserve(2120000, 'g'))
    reject(lambda: HostMap('g', capacity=4097))
    m = HostMap('g'); m.private(2020000, 'g')
    assert not m.output(2020000, 'g')['display']
    reject(lambda: m.output(2020000, 'g'))
    # The live adapter must stop publication after a rejected unknown, even
    # if a later buffer has a valid native identity. Cleanup is independent.
    adapter = TraceMapper(lambda index: a)
    adapter.consume({'type': 'input-call', 'status': 0, 'kind': 'original',
        'association': 19, 'token': 1900000, 'vcl_present': True})
    adapter.consume({'type': 'pts-attribution', 'actual_token': 0, 'flags': 0})
    assert adapter.failed and not adapter.outputs
    adapter.consume({'type': 'pts-attribution', 'actual_token': 1900000, 'flags': 0})
    assert not adapter.outputs
    count += 1
    for event in (
        {'type': 'input-call', 'status': 0, 'kind': 'early-prefix', 'association': 19, 'token': 2120000, 'vcl_present': True},
        {'type': 'input-call', 'status': 4, 'kind': 'original', 'association': 19, 'token': 1900000, 'vcl_present': True},
        {'type': 'pts-attribution', 'actual_token': 1900000, 'flags': 0x20},
    ):
        adapter = TraceMapper(lambda index: a)
        adapter.consume(event)
        assert adapter.failed and not adapter.outputs
        count += 1
    return count


def replay(case, headers):
    """Consumes only accepted input descriptors and actual output tokens.

    Ground-truth image association is deliberately absent from this function.
    A separate caller validates outputs against the existing native oracle.
    """
    provided = []
    def provider(index):
        if index != len(provided)+1:
            raise Unsupported('source metadata was requested out of accepted-input order')
        provided.append(index)
        return source_record(headers[index-1])
    adapter = TraceMapper(provider)
    # No future source metadata is registered before its accepted input event.
    events = []
    for e in case['inputs']:
        row = dict(zip(case['input_columns'], e))
        events.append((row['elapsed_ms'], len(events), 'input', row))
    for index, e in enumerate(case['rx']):
        row = dict(zip(case['rx_columns'], e))
        events.append((row['acquired_ms'], len(events), 'output',
            {'raw_token': row['actual_token'], 'observed_index': index}))
    events.sort(key=lambda e: (e[0], e[1]))
    for first, second in zip(events, events[1:]):
        if first[0] == second[0] and first[2] != second[2]:
            raise Unsupported('trace time precision cannot establish input/output ordering')
    for _, _, kind, event in events:
        if kind == 'input':
            adapter.consume(dict(event, type='input-call', token=event['native_token']))
        else:
            # Only actual returned token and ordinary picture flags enter the
            # mapper; oracle image identity fields are never passed to it.
            adapter.consume({'type': 'pts-attribution', 'actual_token': event['raw_token'],
                'flags': case['outputs'][event['observed_index']]['flags']}, event['observed_index'])
        if adapter.failed is not None:
            break
    return dict(adapter.snapshot(), source_records_requested_only_at_input=provided)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    fixture = ROOT/'tests/fixtures/issue533/early-token-20261027.json'
    evidence = json.loads(fixture.read_text())
    report = {'schema': 1, 'authority': 'https://github.com/ahnhy1324/crystalhd/issues/533#issuecomment-6436305327',
        'offline_source': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'native_evidence_source': evidence['probe_source'],
        'scope': 'H0/H1 offline replay only. No native retagging/presentation or hardware run. Runtime mapper receives only actual accepted input descriptors and raw returned tokens; source-image oracle is used separately for validation.',
        'self_test_checks': self_test(), 'cases': []}
    for case in evidence['cases']:
        mapped = replay(case, evidence['original_aus'])
        originals = [o for o in mapped['outputs'] if o['kind'] == 'original']
        private = [o for o in mapped['outputs'] if o['kind'] == 'private']
        valid = True
        for out in originals:
            # Independent ground truth is consumed only after mapping has
            # finished. It cannot select or repair the mapper's AU identity.
            truth = case['outputs'][out['observed_index']]
            valid = valid and truth['diagnostic_original_token'] == out['au_id']*100000
            valid = valid and evidence['original_aus'][out['au_id']-1]['pts'] == out['effective_pts']
        original_order = [o['canonical_token'] for o in originals]
        expected_order = [i['diagnostic_original_token'] for i in case['outputs'] if i['diagnostic_original_token']]
        complete = len(originals) == 20 and len(private) == 5 and valid and original_order == expected_order and not mapped['rejected'] and not mapped['remaining_owners']
        expected = case['name'].startswith(('next-', 'reserved-near-', 'reserved-far-'))
        assert complete == expected, (case['name'], mapped)
        report['cases'].append({'name': case['name'],
            'raw_native_identity': case['grades']['strict_original_native_identity'],
            'source_image_oracle': case['grades']['original_full_pixels_flags_order'],
            'host_effective_PTS': 'PASS20/20 offline' if complete else 'UNSUPPORTED; fail closed',
            'published_originals': len(originals), 'hidden_private_outputs': len(private),
            'independent_original_identity_validation': valid,
            'mapping': mapped})
    report['decision'] = 'H0/H1 qualify the collision-free early-marker-to-actual-next-VCL binding for a narrow native H2 trial. They do not recover token0 or a previous-real collision. Raw native identity FAIL stays unchanged for generic markers. H2/H3 NOT RUN by this offline script.'
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'self_test_checks': report['self_test_checks'],
        'cases': [(c['name'], c['host_effective_PTS']) for c in report['cases']]}, indent=2))


if __name__ == '__main__':
    main()
