#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline source/caller/returned PTS comparison for one pinned Kodi model.

The 28ea2eac1eb7af8fdcbd2672d933ce87f594be79 model uses these binary64
operations, in this order (BASE is 1000000.0):
    timestamp = (double) packet_pts * time_base.num / time_base.den
    demux_pts = timestamp * BASE
    caller_pts = (int64_t) (demux_pts / BASE * BASE)
Expected values come exclusively from ffprobe packet ticks/time_base. This is
not an execution probe, a tolerant rescale, or hardware/A-V qualification.
"""
import argparse
from decimal import Decimal, InvalidOperation
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import re
import sys

KODI_SOURCE = '28ea2eac1eb7af8fdcbd2672d933ce87f594be79'
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_VIDEO_PACKETS = 10000
MAX_CYCLES = 16
BASE = 1000000.0
RENDERER_GATE_SCOPE = 'OFFLINE_RENDERER_RETURN_COUNTS_ONLY_NO_RUNTIME_OR_AV_PROMOTION'
INT64_MIN, INT64_MAX = -(1 << 63), (1 << 63) - 1
SHA256 = re.compile(r'[0-9a-f]{64}\Z')
INTEGER = re.compile(r'-?(?:0|[1-9][0-9]*)\Z')
CLOCK = re.compile(r'-?[0-9]+(?:\.[0-9]+)?\Z')


class OracleError(ValueError):
    """Unsupported input or an exact comparison failure."""


def require(condition, message):
    if not condition:
        raise OracleError(message)


def integer(value, label, minimum=INT64_MIN, maximum=INT64_MAX, text=False):
    if text and isinstance(value, str) and INTEGER.fullmatch(value):
        require(len(value) <= 21, label + ': integer is too large')
        value = int(value)
    require(type(value) is int and minimum <= value <= maximum,
            label + ': bounded integer required')
    return value


def sha256(value, label):
    require(isinstance(value, str) and SHA256.fullmatch(value),
            label + ': lowercase SHA-256 required')
    return value


def zero_clock(value, label):
    require(type(value) is int or isinstance(value, str), label + ': finite decimal clock required')
    value = str(value)
    require(len(value) <= 64 and CLOCK.fullmatch(value), label + ': finite decimal clock required')
    try:
        clock = Decimal(value)
    except InvalidOperation as exc:
        raise OracleError(label + ': invalid clock') from exc
    require(clock.is_finite() and clock == 0, label + ': only zero-origin media is supported')


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate JSON key: ' + key[:96])
        result[key] = value
    return result


def _constant(value):
    raise OracleError('nonfinite JSON constant: ' + value)


def _json_values(value, depth=0, budget=None):
    if budget is None:
        budget = [250000]
    budget[0] -= 1
    require(budget[0] >= 0 and depth <= 32, 'JSON structure limit exceeded')
    if isinstance(value, dict):
        for key, child in value.items():
            require(isinstance(key, str), 'JSON object keys must be strings')
            _json_values(child, depth + 1, budget)
    elif isinstance(value, list):
        for child in value:
            _json_values(child, depth + 1, budget)
    elif isinstance(value, float):
        require(math.isfinite(value), 'nonfinite JSON number')
    elif isinstance(value, str):
        require(len(value) <= MAX_JSON_BYTES, 'JSON string limit exceeded')
    else:
        require(value is None or type(value) in (bool, int), 'unsupported JSON value')


def document(value, label):
    """Accept a bounded JSON object or JSON bytes/text; raw inputs reject duplicates."""
    try:
        if isinstance(value, (str, bytes, bytearray)):
            raw = value.encode('utf-8') if isinstance(value, str) else bytes(value)
            require(len(raw) <= MAX_JSON_BYTES, label + ': JSON byte limit exceeded')
            value = json.loads(raw, object_pairs_hook=_pairs, parse_constant=_constant)
        require(isinstance(value, dict), label + ': JSON object required')
        _json_values(value)
        require(len(json.dumps(value, allow_nan=False).encode('utf-8')) <= MAX_JSON_BYTES,
                label + ': JSON byte limit exceeded')
        return value
    except OracleError:
        raise
    except (UnicodeError, ValueError, RecursionError, OverflowError) as exc:
        raise OracleError(label + ': malformed JSON') from exc


def caller_pts(packet_pts, numerator, denominator):
    """Supported nonnegative ticks: literal ordering then C++ truncation toward zero.

    Negative timestamp/clamping paths are outside this deliberately bounded model.
    """
    packet_pts = integer(packet_pts, 'packet pts', 0)
    numerator = integer(numerator, 'time_base numerator', 1, (1 << 31) - 1)
    denominator = integer(denominator, 'time_base denominator', 1, (1 << 31) - 1)
    timestamp = float(packet_pts) * numerator
    timestamp = timestamp / denominator
    demux_pts = timestamp * BASE
    codec_pts = demux_pts / BASE
    codec_pts = codec_pts * BASE
    require(math.isfinite(codec_pts) and INT64_MIN <= codec_pts < (1 << 63),
            'caller PTS conversion is outside int64')
    return int(codec_pts)


def _source_expected(source):
    streams = source.get('streams')
    require(isinstance(streams, list) and 1 <= len(streams) <= 64, 'source streams required')
    indices = {}
    for stream in streams:
        require(isinstance(stream, dict), 'source stream must be an object')
        index = integer(stream.get('index'), 'stream index', 0, 65535)
        require(index not in indices, 'duplicate stream index')
        require(isinstance(stream.get('codec_type'), str), 'stream codec_type required')
        indices[index] = stream
    videos = [v for v in streams if v['codec_type'] == 'video']
    require(len(videos) == 1 and videos[0].get('codec_name') == 'h264',
            'exactly one H.264 video stream is supported')
    video = videos[0]
    require(isinstance(source.get('format'), dict), 'source format required')
    zero_clock(source['format'].get('start_time'), 'format.start_time')
    zero_clock(video.get('start_time'), 'video.start_time')
    require(integer(video.get('start_pts'), 'video.start_pts', text=True) == 0,
            'video.start_pts must be zero')
    time_base = video.get('time_base')
    require(isinstance(time_base, str) and re.fullmatch(r'[1-9][0-9]*/[1-9][0-9]*', time_base)
            and len(time_base) <= 21, 'positive rational video.time_base required')
    numerator, denominator = [integer(v, 'time_base', 1, (1 << 31) - 1, text=True)
                              for v in time_base.split('/')]
    packets = source.get('packets')
    require(isinstance(packets, list) and len(packets) <= MAX_VIDEO_PACKETS * 8,
            'bounded source packet list required')
    ticks = []
    for packet in packets:
        require(isinstance(packet, dict), 'packet must be an object')
        index = integer(packet.get('stream_index'), 'packet.stream_index', 0, 65535)
        require(index in indices, 'packet references an unknown stream')
        if 'codec_type' in packet:
            require(packet['codec_type'] == indices[index]['codec_type'], 'packet codec_type mismatch')
        if index == video['index']:
            ticks.append(integer(packet.get('pts'), 'video packet.pts', 0, INT64_MAX, text=True))
    require(1 <= len(ticks) <= MAX_VIDEO_PACKETS, 'bounded nonempty H.264 packet set required')
    require(min(ticks) == 0, 'source packet set does not include its declared zero origin')
    require(len(ticks) == len(set(ticks)), 'duplicate source packet PTS')
    for name in ('nb_frames', 'nb_read_packets', 'nb_read_frames'):
        if name in video and video[name] != 'N/A':
            require(integer(video[name], 'video.' + name, 1, MAX_VIDEO_PACKETS, text=True) == len(ticks),
                    'video.' + name + ' differs from video packet count')
    expected = [caller_pts(pts, numerator, denominator) for pts in ticks]
    require(len(expected) == len(set(expected)), 'source PTS collide after caller conversion')
    rational = [int(Fraction(pts * numerator * 1000000, denominator)) for pts in ticks]
    deltas = [actual - ideal for actual, ideal in zip(expected, rational)]
    diagnostic = {'reference': 'exact rational conversion truncated toward zero; diagnostic only',
                  'nonzero_delta_packets': sum(v != 0 for v in deltas),
                  'minimum_delta_us': min(deltas), 'maximum_delta_us': max(deltas),
                  'examples': [{'packet_index': n, 'raw_pts': ticks[n], 'rational_pts_us': rational[n],
                                'caller_pts_us': expected[n], 'delta_us': delta}
                               for n, delta in enumerate(deltas) if delta != 0][:16]}
    return video, expected, diagnostic


def _sequence(actual, expected, label):
    require(isinstance(actual, list) and len(actual) == len(expected), label + ': complete count mismatch')
    for n, value in enumerate(actual):
        integer(value, label + '[' + str(n) + ']')
    require(len(actual) == len(set(actual)), label + ': duplicate PTS')
    for n, (value, wanted) in enumerate(zip(actual, expected)):
        require(value == wanted, label + '[' + str(n) + ']: expected ' + str(wanted) + ', observed ' + str(value))


def _existing_counts(report, cycles, count):
    total = count * len(cycles)
    trace_bounds = any('trace_start_byte' in cycle or 'trace_end_byte' in cycle for cycle in cycles)
    previous_end = 0
    for n, cycle in enumerate(cycles):
        if trace_bounds:
            start = integer(cycle.get('trace_start_byte'), 'trace_start_byte', 0)
            end = integer(cycle.get('trace_end_byte'), 'trace_end_byte', 1)
            require(start >= previous_end and end > start, 'stale/overlapping cycle trace byte window')
            previous_end = end
        for name in ('drm_frame_count', 'accepted_count', 'returned_count', 'capture_nonempty', 'source_frame_count'):
            if name in cycle:
                require(integer(cycle[name], name, 0) == count, 'cycle ' + str(n) + ' ' + name + ': inconsistent count')
        if 'capture_LAST_bytes' in cycle:
            last = cycle['capture_LAST_bytes']
            require(isinstance(last, list) and len(last) == 1, 'cycle LAST count mismatch')
            integer(last[0], 'capture_LAST_bytes', 0)
    probe = report.get('native_probe', {})
    require(isinstance(probe, dict), 'native_probe must be an object')
    for name, wanted in (('@capture_nonempty', total), ('@capture_LAST', len(cycles))):
        if name in probe:
            require(integer(probe[name], name, 0) == wanted, name + ': inconsistent total')
    if '@h264_frames' in probe:
        frames = probe['@h264_frames']
        require(isinstance(frames, dict), '@h264_frames must be an object')
        native_count = 0
        for key, value in frames.items():
            require(re.fullmatch(r'[0-9]+,[0-9]+,[0-9]+', key), 'malformed @h264_frames key')
            amount = integer(value, '@h264_frames count', 0)
            if key.rsplit(',', 1)[1] == '178':
                native_count += amount
        require(native_count == total, '@h264_frames: inconsistent native total')
    if '@drm_layout' in probe:
        layouts = probe['@drm_layout']
        require(isinstance(layouts, dict), '@drm_layout must be an object')
        amount = 0
        for key, value in layouts.items():
            require(re.fullmatch(r'[0-9]+(?:,[0-9]+){4}', key), 'malformed @drm_layout key')
            amount += integer(value, '@drm_layout count', 0)
        require(amount == total, '@drm_layout: inconsistent total')


def _renderer_census(cycles, expected_count):
    """Separate return-count diagnostic; it does not qualify frame presentation."""
    rows = []
    for n, cycle in enumerate(cycles):
        observed = cycle.get('renderer_acceptances')
        valid = type(observed) is int and 0 <= observed <= INT64_MAX
        if not valid:
            rows.append({'cycle': n, 'expected': expected_count, 'renderer_acceptances': None,
                         'status': 'UNKNOWN', 'classification': 'UNKNOWN',
                         'deficit': None, 'excess': None,
                         'reason': 'renderer_acceptances is missing or is not a bounded nonnegative integer'})
            continue
        deficit, excess = max(0, expected_count - observed), max(0, observed - expected_count)
        rows.append({'cycle': n, 'expected': expected_count, 'renderer_acceptances': observed,
                     'status': 'FAIL' if deficit or excess else 'PASS',
                     'classification': 'DEFICIT' if deficit else 'EXCESS' if excess else 'FULL',
                     'deficit': deficit, 'excess': excess})
    unknown = sum(row['status'] == 'UNKNOWN' for row in rows)
    failed = any(row['status'] == 'FAIL' for row in rows)
    known_deficit = sum(row['deficit'] for row in rows if row['deficit'] is not None)
    known_excess = sum(row['excess'] for row in rows if row['excess'] is not None)
    return {'status': 'FAIL' if failed else 'UNKNOWN' if unknown else 'PASS',
            'scope': 'Renderer return counts only; no PTS-bound presentation, scanout, runtime or A/V promotion.',
            'expected_total': expected_count * len(cycles),
            'renderer_acceptances_total': None if unknown else sum(row['renderer_acceptances'] for row in rows),
            'deficit': None if unknown else known_deficit, 'excess': None if unknown else known_excess,
            'known_deficit': known_deficit, 'known_excess': known_excess,
            'unknown_cycles': unknown, 'cycles': rows}


def analyze(source_packets, native_report, media_sha256, kodi_source):
    """Return a concise exact-match summary; raise OracleError on unsupported/mismatch.

    Accept decoded dicts or bounded raw JSON. Duplicate-key detection requires raw
    JSON; the CLI always uses pinned raw bytes. The explicit Kodi source selects
    this model, and does not authenticate a Kodi binary or FFprobe execution.
    """
    require(kodi_source == KODI_SOURCE, 'unsupported Kodi source/model')
    media_sha256 = sha256(media_sha256, 'media_sha256')
    source = document(source_packets, 'source packets')
    report = document(native_report, 'native report')
    fixture = report.get('fixture')
    require(isinstance(fixture, dict) and sha256(fixture.get('sha256'), 'native fixture sha256') == media_sha256,
            'media SHA does not match native fixture')
    if 'kodi_source' in report:
        require(report['kodi_source'] == kodi_source, 'native report Kodi source mismatch')
    require(report.get('status') in ('PASS', 'FAIL'), 'a completed native harness report is required')
    video, expected, diagnostic = _source_expected(source)
    count = len(expected)
    require(integer(report.get('expected_frames_per_cycle'), 'expected_frames_per_cycle', 1, MAX_VIDEO_PACKETS) == count,
            'native expected frame count differs from source packet count')
    requested = integer(report.get('requested_cycles'), 'requested_cycles', 1, MAX_CYCLES)
    cycles = report.get('cycles')
    require(isinstance(cycles, list) and len(cycles) == requested, 'requested/completed cycle count mismatch')
    for name in ('width', 'height'):
        if name in video and name in report:
            require(integer(video[name], 'video.' + name, 1) == integer(report[name], name, 1),
                    'source/report ' + name + ' mismatch')
    summary = []
    for n, cycle in enumerate(cycles):
        require(isinstance(cycle, dict) and integer(cycle.get('cycle'), 'cycle index', 0, MAX_CYCLES - 1) == n,
                'cycle indices must be consecutive')
        label = 'cycle ' + str(n) + ' '
        _sequence(cycle.get('accepted_native_PTS'), expected, label + 'accepted_native_PTS')
        _sequence(cycle.get('returned_native_PTS'), sorted(expected), label + 'returned_native_PTS')
        summary.append({'cycle': n, 'accepted': count, 'returned': count})
    _existing_counts(report, cycles, count)
    return {'schema': 1, 'status': 'PASS_OFFLINE_SOURCE_CALLER_RETURNED_PTS_MATCH',
            'scope': 'OFFLINE_PTS_ONLY_NO_HARDWARE_OR_AV_PROMOTION', 'kodi_source': kodi_source,
            'media_sha256': media_sha256, 'video_time_base': video['time_base'],
            'source_video_packets': count, 'completed_cycles': requested, 'cycles': summary,
            'tolerance_us': 0, 'native_report_status_observed': report['status'],
            'renderer_census': _renderer_census(cycles, count),
            'rational_vs_binary64_diagnostic': diagnostic}


def read_pinned(path, expected_sha256):
    expected_sha256 = sha256(expected_sha256, 'JSON input SHA-256')
    try:
        with Path(path).open('rb') as stream:
            raw = stream.read(MAX_JSON_BYTES + 1)
    except OSError as exc:
        raise OracleError('cannot read JSON input: ' + str(exc)) from exc
    require(len(raw) <= MAX_JSON_BYTES, 'JSON input byte limit exceeded')
    require(hashlib.sha256(raw).hexdigest() == expected_sha256, 'JSON input SHA-256 mismatch')
    return raw


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source-packets', 'source-packets-sha256', 'native-report', 'native-report-sha256', 'media-sha256', 'kodi-source'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--require-renderer-census', action='store_true',
                        help='Exit 3 unless the separate renderer count census is PASS.')
    args = parser.parse_args(argv)
    try:
        result = analyze(read_pinned(args.source_packets, args.source_packets_sha256),
                         read_pinned(args.native_report, args.native_report_sha256),
                         args.media_sha256, args.kodi_source)
        result['input_sha256'] = {'source_packets': args.source_packets_sha256,
                                  'native_report': args.native_report_sha256}
    except (OracleError, ValueError, TypeError, KeyError) as exc:
        result = {'schema': 1, 'status': 'FAIL_OFFLINE_SOURCE_CALLER_RETURNED_PTS_MATCH',
                  'scope': 'OFFLINE_PTS_ONLY_NO_HARDWARE_OR_AV_PROMOTION', 'error': str(exc)[:512]}
        if args.require_renderer_census:
            result['requested_renderer_gate'] = {'requested': True, 'status': 'NOT_EVALUATED',
                                                 'scope': RENDERER_GATE_SCOPE,
                                                 'reason': 'PTS_OR_INPUT_VALIDATION_FAILED'}
        print(json.dumps(result, sort_keys=True))
        return 2
    exit_code = 0
    if args.require_renderer_census:
        census_status = result['renderer_census']['status']
        passed = census_status == 'PASS'
        result['requested_renderer_gate'] = {'requested': True, 'status': 'PASS' if passed else 'FAIL',
                                             'scope': RENDERER_GATE_SCOPE,
                                             'renderer_census_status': census_status}
        if not passed:
            exit_code = 3
    print(json.dumps(result, sort_keys=True, allow_nan=False))
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
