#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded content checks for the prepared centered FHD barcode fixture.

Mapped observed patches are passed to the unchanged public barcode/CRC reader.
The sparse proxy is not a reconstructed image. Content ordinal selects fixture
PTS; packet/AVFrame PTS at the VA handoff, GPU return and scanout are unobserved.
"""
import argparse
import importlib.util
import json
from pathlib import Path


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


DIRECTORY = Path(__file__).resolve().parent
B = load('centered_public_content_barcode', DIRECTORY / 'native-picture-fixture.py')
P = load('centered_payload_content_reader', DIRECTORY / 'vaapi-centered-payload-reader.py')
SOURCE_CASE, SOURCE_FRAMES = 2, 180
WIDTH, HEIGHT = 1920, 1080
TARGET_SELECTION = 'case2-90-91'
TARGETS = (90, 91)
SCOPE = ('Observed barcode content -> prepared fixture ordinal/PTS only; '
         'actual packet/AVFrame PTS-to-VA binding, GPU-reader return and scanout NOT_OBSERVED.')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def decode_patches(data, width, height, kind, centered):
    """Keep each real 3x3 patch; public decoder checks replicas/CRC/thresholds."""
    require(kind in ('bgra', 'nv12'), 'unsupported observed pixel format')
    expected = width * height * (4 if kind == 'bgra' else 3) // (1 if kind == 'bgra' else 2)
    require(len(data) == expected, 'observed pixel extent differs')
    require(((width, height) == (WIDTH, HEIGHT) and centered) if kind == 'bgra' else
            ((width == 1280 and height == 720 and not centered) or
             (width == WIDTH and height in (1080, 1088) and centered)),
            'observed geometry differs from the frozen coordinate contract')
    factor = 4 if kind == 'bgra' else 1
    proxy = bytearray(WIDTH * HEIGHT * (4 if kind == 'bgra' else 3) //
                      (1 if kind == 'bgra' else 2))
    for index, (x, y, cell_width, cell_height) in enumerate(B.cells(WIDTH)):
        sx = 170 + 20 * (index % 48) + (320 if centered else 0)
        sy = (21 if index < 48 else 53) + (180 if centered else 0)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                source = ((sy + dy) * width + sx + dx) * factor
                target = ((y + cell_height // 2 + dy) * WIDTH + x + cell_width // 2 + dx) * factor
                proxy[target:target + factor] = data[source:source + factor]
    return B.decode(WIDTH, HEIGHT, proxy, 'bgra' if kind == 'bgra' else 'yuv420p',
                    source_case=SOURCE_CASE)


def content(ordinal):
    require(type(ordinal) is int and 0 <= ordinal < SOURCE_FRAMES, 'source ordinal outside fixture')
    return {'source_ordinal': ordinal, 'source_pts_ticks': ordinal * 512,
            'source_time_base': '1/15360', 'source_case': SOURCE_CASE,
            'actual_media_PTS_to_VA_binding': 'NOT_OBSERVED', 'scope': SCOPE}


def audit_capture(path):
    try:
        data = P.read_regular(Path(path), WIDTH * HEIGHT * 4)
        ordinal = decode_patches(data, WIDTH, HEIGHT, 'bgra', True)
        return dict(status='PASS', **content(ordinal))
    except (ValueError, OSError, KeyError, TypeError) as error:
        return {'status': 'FAIL', 'error': str(error), 'scope': SCOPE}


def audit_payload(manifest_path, trace_path, required_targets=None):
    """Join selected OWN bindings to content, preserving the full payload gate."""
    base = P.audit(manifest_path, trace_path)
    if base.get('status') != 'PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD':
        return {'status': 'FAIL', 'payload_audit': base, 'scope': SCOPE}
    try:
        manifest_path = Path(manifest_path)
        doc = json.loads(P.read_regular(manifest_path, P.MAX_MANIFEST_BYTES), object_pairs_hook=P.strict_object)
        if required_targets is not None:
            require(type(required_targets) in (tuple, list) and
                    tuple(required_targets) == TARGETS and
                    all(type(value) is int for value in required_targets),
                    'only the frozen case2 targets (90,91) are supported')
            require(doc.get('selection') == TARGET_SELECTION,
                    'manifest must explicitly name case2-90-91 selection')
        selected, seen = [], set()
        for slot in doc['slots']:
            ordinals = {}
            for stage, geometry in (('small', 'small'), ('composed', 'composed'), ('public', 'public_canvas')):
                g = slot[geometry]
                pixels = P.read_regular(manifest_path.parent / P.filename(slot[stage + '_file']), P.MAX_CANVAS_BYTES)
                ordinals[stage] = decode_patches(pixels, g['width'], g['height'], 'nv12', stage != 'small')
                if stage == 'small':
                    require(pixels[g['width'] * g['height']:] == bytes([128]) * (g['width'] * g['height'] // 2),
                            'selected neutral source fixture has non-neutral chroma')
            require(len(set(ordinals.values())) == 1, 'selected small/composed/public source ordinals differ')
            ordinal = ordinals['small']
            require(ordinal not in seen, 'distinct selected OWN bindings repeat source content')
            seen.add(ordinal)
            selected.append(dict(binding=slot['binding'], stage_ordinals=ordinals, **content(ordinal)))
        result = {'status': 'PASS', 'payload_audit': base,
                  'selected_source_bindings': selected, 'scope': SCOPE}
        if required_targets is not None:
            require(tuple(row['source_ordinal'] for row in selected) == TARGETS,
                    'selected decoded source contents must be exactly (90,91)')
            result.update(selection=TARGET_SELECTION, required_targets=list(TARGETS))
        return result
    except (ValueError, OSError, KeyError, TypeError) as error:
        return {'status': 'FAIL', 'error': str(error), 'payload_audit': base, 'scope': SCOPE}


def audit_target_window(result, trace_path, seek_request_ns, stop_request_ns):
    """Join fixed source content to exact OWN events within a control window.

    Bounds are monotonic request times, not media PTS. The full source binding
    must submit, materialize and successfully sync after the seek request and
    before requested Stop. Nearby timestamps or VA submission ordinals never
    stand in for decoded barcode identity.
    """
    try:
        require(type(result) is dict and result.get('status') == 'PASS' and
                result.get('selection') == TARGET_SELECTION and
                result.get('required_targets') == list(TARGETS),
                'target window requires a passing explicit case2-90-91 content audit')
        base = P.object_value(result.get('payload_audit'), 'payload_audit')
        require(base.get('status') == 'PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD' and
                base.get('schema') in P.SCHEMAS,
                'target window requires the passing full payload gate')
        begin = P.integer(seek_request_ns, 'seek_request_ns', 1)
        end = P.integer(stop_request_ns, 'stop_request_ns', 1)
        require(begin < end, 'seek request must precede requested Stop')
        pid = P.integer(base.get('pid'), 'pid', 1)
        instance = P.integer(base.get('driver_instance'), 'driver_instance', 1)
        publication = P.object_value(base.get('publication'), 'publication')
        require(publication.get('after_driver_unlock') is True and
                publication.get('all_slots_frozen') is True,
                'target payload publication is not unlocked/frozen')
        rows = P.trace_index(P.parse_trace(P.read_regular(trace_path, P.MAX_TRACE_BYTES)), pid, instance)
        boundary = P.publication_boundary(base['schema'], publication, rows)
        selected = result.get('selected_source_bindings')
        observations = base.get('observations')
        require(type(selected) is list and len(selected) == 2 and
                type(observations) is list and len(observations) == 2 and
                all(type(row) is dict for row in selected) and
                tuple(row.get('source_ordinal') for row in selected) == TARGETS,
                'target window needs exactly the two decoded source targets')
        joined = []
        for number, (source, ordinal) in enumerate(zip(selected, TARGETS)):
            source = P.object_value(source, 'selected source')
            require(source.get('stage_ordinals') ==
                    {'small': ordinal, 'composed': ordinal, 'public': ordinal} and
                    source.get('source_case') == SOURCE_CASE and
                    source.get('source_pts_ticks') == ordinal * 512 and
                    source.get('source_time_base') == '1/15360',
                    'target window source-content evidence differs')
            binding = P.object_value(source.get('binding'), 'binding')
            for name in P.BINDING_KEYS:
                P.integer(binding.get(name), 'binding.' + name, 1)
            require((binding['context'], binding['generation']) ==
                    (publication['context'], publication['generation']),
                    'target binding differs from the published decoder lifetime')
            key = P.binding_key(binding)
            prior = P.object_value(observations[number], 'payload observation')
            require(P.binding_key(P.object_value(prior.get('binding'), 'payload binding')) == key,
                    'target source binding was swapped after its payload audit')
            current = P.join_binding(rows, binding, boundary)
            require(all(current[name] == prior.get(name)
                        for name in ('submit_seq', 'materialized_seq')),
                    'target trace differs from its full payload binding receipt')
            submit = P.matching(rows, 'submit_bind', key)[0]
            materialized = P.matching(rows, 'materialized', key)[0]
            for label, row in (('submit_bind', submit), ('materialized', materialized)):
                require(begin <= row['ts_monotonic_ns'] < end,
                        'target ' + label + ' is outside [seek request, requested Stop)')
            syncs = [row for row in P.matching(rows, 'sync_exit', key)
                     if row['outcome'] == 0 and begin <= row['ts_monotonic_ns'] < end]
            require(syncs, 'target has no successful matching sync inside the control window')
            joined.append({'source_ordinal': ordinal, 'binding': dict(binding),
                           'submit_seq': submit['seq'], 'submit_ns': submit['ts_monotonic_ns'],
                           'materialized_seq': materialized['seq'],
                           'materialized_ns': materialized['ts_monotonic_ns'],
                           'successful_syncs_in_window': [
                               {'seq': row['seq'], 'operation': row['operation'],
                                'ts_monotonic_ns': row['ts_monotonic_ns']} for row in syncs]})
        return {'status': 'PASS', 'selection': TARGET_SELECTION, 'required_targets': list(TARGETS),
                'pid': pid, 'driver_instance': instance, 'seek_request_ns': begin,
                'stop_request_ns': end, 'selected_source_bindings': joined,
                'actual_media_PTS_to_VA_binding': 'NOT_OBSERVED',
                'gpu_reader_return': 'NOT_OBSERVED', 'scanout': 'NOT_OBSERVED', 'scope': SCOPE}
    except (ValueError, OSError, KeyError, TypeError) as error:
        return {'status': 'FAIL', 'error': str(error), 'scope': SCOPE}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    result = audit_capture(args.capture)
    print(json.dumps(result, sort_keys=True))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
