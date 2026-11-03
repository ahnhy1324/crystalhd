#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Join bounded receive-frame scalars to complete OWN VA trace lifetimes.

Explicit trusted files only. Reusable pointers are descriptive. The join needs
one actual surface binding throughout the snapshot; no nearest-time inference,
device access, GPU completion or stock/release qualification is performed.
"""
import argparse
from collections import defaultdict
from fractions import Fraction
import importlib.util
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location(
    'frame_pts_payload_primitives', Path(__file__).with_name('vaapi-centered-payload-reader.py'))
P = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(P)
require = P.require
SCHEMA = 'crystalhd-frame-pts-v1'
CODEC_VERSION, UTIL_VERSION = (63 << 16) | (1 << 8) | 102, (61 << 16) | (1 << 8) | 102
NOPTS = -(1 << 63)
MAX_REPORT_BYTES, MAX_RECORDS = 16 * 1024 * 1024, 8192
SCOPE = ('Instrumented receive-frame metadata -> unique active OWN VA binding; '
         'GPU-reader return, scanout and stock/release compatibility NOT_OBSERVED.')


def signed(value, label, bits=64):
    require(type(value) is int and -(1 << (bits - 1)) <= value < (1 << (bits - 1)),
            label + ' must be a signed integer in range')
    return value


def rational(value, label):
    require(type(value) is list and len(value) == 2, label + ' must have two integers')
    n, d = (signed(item, label, 32) for item in value)
    return Fraction(n, d) if n > 0 and d > 0 else None


def parse_report(raw):
    require(raw.endswith(b'\n'), 'unterminated frame report')
    lines = raw.decode('utf-8', errors='strict').splitlines()
    require(2 <= len(lines) <= MAX_RECORDS + 2, 'frame report count exceeds bound')
    rows = [P.object_value(json.loads(line, object_pairs_hook=P.strict_object), 'frame report row')
            for line in lines]
    header, footer = rows[0], rows[-1]
    require(header.get('type') == 'header' and header.get('schema') == SCHEMA,
            'unsupported frame report header')
    require(header.get('provider_ok') is True and
            type(header.get('codec_version')) is int and header['codec_version'] == CODEC_VERSION and
            type(header.get('util_version')) is int and header['util_version'] == UTIL_VERSION,
            'frame provider/header ABI was not qualified')
    for key in ('codec_provider', 'util_provider'):
        require(type(header.get(key)) is str and header[key].startswith('/') and '\x00' not in header[key],
                'frame provider path is missing')
    pid = P.integer(header.get('pid'), 'frame pid', 1)
    capacity = P.integer(header.get('capacity'), 'frame capacity', 1)
    require(capacity <= MAX_RECORDS and len(rows) - 2 <= capacity, 'frame capacity differs')
    vaapi_format = signed(header.get('vaapi_format'), 'VAAPI format', 32)
    require(vaapi_format >= 0, 'VAAPI format must be concrete')
    require(footer.get('type') == 'footer' and footer.get('complete') is True and
            footer.get('provider_ok') is True and type(footer.get('dropped')) is int and
            footer['dropped'] == 0 and footer.get('errors') == [] and
            type(footer.get('records')) is int and footer['records'] == len(rows) - 2,
            'frame report is incomplete, dropped observations or failed provider checks')
    frames = rows[1:-1]
    require(frames, 'no returned VAAPI frames observed')
    for seq, row in enumerate(frames, 1):
        require(row.get('type') == 'frame' and type(row.get('seq')) is int and row['seq'] == seq,
                'frame sequence is incomplete or duplicated')
        require(row.get('pid') == pid and type(row.get('pid')) is int, 'frame PID differs')
        P.integer(row.get('tid'), 'frame tid', 1)
        begin = P.integer(row.get('snapshot_begin_ns'), 'snapshot begin', 1)
        end = P.integer(row.get('snapshot_end_ns'), 'snapshot end', 1)
        require(begin <= end, 'frame snapshot interval regressed')
        # Concurrent snapshots need not finish in sequence order.
        for key in ('codec', 'frame', 'pool'):
            require(type(row.get(key)) is str and re.fullmatch(r'0x[0-9a-fA-F]+', row[key]),
                    'invalid descriptive pointer: ' + key)
        require(row.get('display') is None, 'display unavailable in scalar-only protocol')
        require(type(row.get('format')) is int and row['format'] == vaapi_format,
                'observed frame is not the declared VAAPI format')
        require(P.integer(row.get('surface'), 'frame surface') < (1 << 32) - 1, 'invalid VA surface')
        for key in ('pts', 'best_effort_timestamp'):
            signed(row.get(key), 'frame ' + key)
        for key in ('frame_time_base', 'packet_time_base'):
            rational(row.get(key), key)
    return header, frames


def trace_lifetimes(trace_rows, pid):
    """Retain every same-PID instance, including surface namespace collisions."""
    instances = set()
    for row in trace_rows:
        P.object_value(row, 'trace row')
        if row.get('pid') == pid:
            instances.add(P.integer(row.get('driver_instance'), 'trace instance', 1))
    require(instances, 'observed PID has no OWN trace')
    spans = []
    for instance in sorted(instances):
        rows = P.trace_index(trace_rows, pid, instance)
        require(rows[-1]['event'] == 'session_destroy' and rows[-1]['outcome'] == 0,
                'frame join requires a complete retired trace for every PID instance')
        seen, identities = set(), set()
        for submit in (row for row in rows if row['event'] == 'submit_bind'):
            require(submit['outcome'] == 0, 'failed submit_bind cannot supply a frame identity')
            key = P.binding_key(submit)
            require(key not in seen, 'duplicate full OWN binding')
            seen.add(key)
            for name in P.BINDING_KEYS:
                P.integer(submit[name], 'submitted ' + name, 0 if name == 'surface' else 1)
            for name in ('token', 'decode_identity', 'submission_ordinal'):
                identity = (submit['context'], submit['generation'], name, submit[name])
                require(identity not in identities, 'duplicated original ' + name + ' in decoder lifetime')
                identities.add(identity)
            boundary = None
            for row in rows:
                if row['seq'] <= submit['seq']:
                    continue
                event = row['event']
                same_context = row['context'] == submit['context']
                same_resource = row['surface'] == submit['surface'] or row['owner'] == submit['owner']
                if (event == 'session_destroy' or
                    (event in ('context_destroy', 'context_reset') and same_context) or
                    (event == 'surface_destroy' and row['surface'] == submit['surface']) or
                    (event in ('binding_end', 'submit_bind') and same_resource)):
                    boundary = row
                    break
            require(boundary is not None, 'OWN binding has no retirement boundary')
            spans.append({'driver_instance': instance,
                          'binding': {name: submit[name] for name in P.BINDING_KEYS},
                          'submit_seq': submit['seq'], 'submit_ns': submit['ts_monotonic_ns'],
                          'boundary_seq': boundary['seq'], 'boundary_ns': boundary['ts_monotonic_ns'],
                          'boundary_event': boundary['event']})
    return spans


def frame_clocks(frame):
    frame_tb = rational(frame['frame_time_base'], 'frame_time_base')
    packet_tb = rational(frame['packet_time_base'], 'packet_time_base')
    # pkt_timebase explicitly declares decoding packet PTS/DTS units. The
    # pinned decoder propagates those to returned timestamps. Preserve this
    # basis when frame.time_base is unspecified; never infer fixture units.
    tb = frame_tb or packet_tb
    basis = 'frame_time_base' if frame_tb else 'packet_time_base' if packet_tb else None
    clocks = {}
    for name in ('pts', 'best_effort_timestamp'):
        value = frame[name]
        seconds = value * tb if value != NOPTS and tb is not None else None
        clocks[name] = {'raw': value, 'observed': value != NOPTS,
                        'time_base_source': basis, 'time_base': str(tb) if tb is not None else None,
                        'seconds': str(seconds) if seconds is not None else None}
    return clocks


def check_document(report_rows, trace_rows, selected_bindings=None):
    """Audit decoded report rows; also used by device-free controls."""
    raw = ''.join(json.dumps(row) + '\n' for row in report_rows).encode('utf-8')
    header, frames = parse_report(raw)
    spans = trace_lifetimes(trace_rows, header['pid'])
    joins, counts = [], defaultdict(list)
    for frame in frames:
        begin, end = frame['snapshot_begin_ns'], frame['snapshot_end_ns']
        overlapping = [span for span in spans if span['binding']['surface'] == frame['surface'] and
                       span['submit_ns'] <= end and span['boundary_ns'] >= begin]
        require(len(overlapping) == 1, 'frame has no unique active surface binding across PID instances')
        span = overlapping[0]
        require(span['submit_ns'] < begin <= end < span['boundary_ns'],
                'frame snapshot crosses or touches a binding/lifetime boundary')
        joined = dict(span, frame_seq=frame['seq'], pid=header['pid'], tid=frame['tid'],
                      snapshot_begin_ns=begin, snapshot_end_ns=end,
                      clocks=frame_clocks(frame),
                      descriptive_pointers={name: frame[name] for name in ('codec', 'frame', 'pool', 'display')})
        joins.append(joined)
        b = span['binding']
        counts[(span['driver_instance'], b['context'], b['generation'])].append(joined)
    selected_joins = _join_selected(selected_bindings, joins, header['pid'])
    lifetime_counts = []
    for (instance, context, generation), items in sorted(counts.items()):
        lifetime_counts.append({'driver_instance': instance, 'context': context, 'generation': generation,
                                'frame_returns': len(items),
                                'unique_bindings': len({P.binding_key(item['binding']) for item in items}),
                                'raw_pts_observed': sum(item['clocks']['pts']['observed'] for item in items),
                                'clock_observed': sum(item['clocks']['pts']['seconds'] is not None for item in items),
                                'first_frame_seq': min(item['frame_seq'] for item in items),
                                'last_frame_seq': max(item['frame_seq'] for item in items)})
    return {'status': 'PASS', 'schema': SCHEMA, 'pid': header['pid'],
            'frames_observed': len(frames),
            'driver_instances_considered': sorted({row['driver_instance'] for row in trace_rows
                                                   if row['pid'] == header['pid']}),
            'joins': joins, 'selected_joins': selected_joins, 'lifetime_counts': lifetime_counts,
            'selected_clock_status': ('NOT_OBSERVED_UNDECLARED_TIME_BASE' if any(
                comparison['status'] != 'PASS' for item in selected_joins
                for comparison in item['source_comparisons'].values()) else
                'PASS' if selected_joins else 'NOT_REQUESTED'),
            'selected_clock_qualified_bindings': sum(all(comparison['status'] == 'PASS'
                for comparison in item['source_comparisons'].values()) for item in selected_joins),
            'providers': {name: header[name] for name in
                          ('codec_version', 'util_version', 'codec_provider', 'util_provider')},
            'scope': SCOPE}


def _join_selected(selected_bindings, joins, pid):
    selected_joins = []
    if selected_bindings is None:
        return selected_joins
    require(type(selected_bindings) is list and selected_bindings,
            'selected bindings must be a nonempty independently audited list')
    selected_keys = set()
    for source in selected_bindings:
        P.object_value(source, 'selected source')
        require(source.get('pid', pid) == pid, 'selected source PID differs')
        instance = P.integer(source.get('driver_instance'), 'selected instance', 1)
        binding = P.object_value(source.get('binding'), 'selected binding')
        for name in P.BINDING_KEYS:
            P.integer(binding.get(name), 'selected ' + name, 0 if name == 'surface' else 1)
        key = (instance, P.binding_key(binding))
        require(key not in selected_keys, 'selected source binding is duplicated')
        selected_keys.add(key)
        matches = [row for row in joins if (row['driver_instance'], P.binding_key(row['binding'])) == key]
        require(len(matches) == 1, 'selected payload needs exactly one actual returned frame')
        joined = matches[0]
        ticks = signed(source.get('source_pts_ticks'), 'selected fixture PTS')
        require(type(source.get('source_time_base')) is str, 'selected fixture timebase must be explicit')
        source_tb = Fraction(source['source_time_base'])
        require(source_tb > 0, 'selected fixture timebase must be positive')
        expected, comparisons = ticks * source_tb, {}
        for name, clock in joined['clocks'].items():
            require(clock['observed'], 'selected actual timestamp is unobserved')
            if clock['seconds'] is None:
                comparisons[name] = {'status': 'NOT_OBSERVED_UNDECLARED_TIME_BASE',
                                     'raw': clock['raw'], 'source_seconds': str(expected),
                                     'actual_seconds': None, 'delta_seconds': None,
                                     'tolerance_seconds': None}
                continue
            delta, tick = Fraction(clock['seconds']) - expected, Fraction(clock['time_base'])
            require(abs(delta) <= tick, 'selected actual PTS disagrees with independently decoded source content')
            comparisons[name] = {'status': 'PASS', 'actual_seconds': clock['seconds'], 'source_seconds': str(expected),
                                 'delta_seconds': str(delta), 'tolerance_seconds': str(tick),
                                 'tolerance_basis': 'one explicitly observed runtime timebase tick'}
        selected_joins.append(dict(joined, phase=source.get('phase'), source_ordinal=source.get('source_ordinal'),
                                   source_pts_ticks=ticks, source_time_base=source['source_time_base'],
                                   source_comparisons=comparisons))
    return selected_joins


def audit_frames(report_path, trace_prefix, selected_bindings=None):
    """Read a complete post-exit report and full stdout trace, never a live log."""
    try:
        raw = P.read_regular(report_path, MAX_REPORT_BYTES)
        header, frames = parse_report(raw)
        footer = json.loads(raw.decode('utf-8').splitlines()[-1], object_pairs_hook=P.strict_object)
        trace = P.parse_trace(P.read_regular(trace_prefix, P.MAX_TRACE_BYTES))
        return check_document([header, *frames, footer], trace, selected_bindings)
    except (ValueError, OSError, KeyError, TypeError, OverflowError, ZeroDivisionError) as error:
        return {'status': 'FAIL', 'error': str(error), 'scope': SCOPE}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--trace', required=True, type=Path)
    parser.add_argument('--selected', type=Path, help='explicit JSON list from passing payload audits')
    args = parser.parse_args(argv)
    selected = None
    if args.selected is not None:
        selected = json.loads(P.read_regular(args.selected, P.MAX_MANIFEST_BYTES), object_pairs_hook=P.strict_object)
    result = audit_frames(args.report, args.trace, selected)
    print(json.dumps(result, sort_keys=True))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
