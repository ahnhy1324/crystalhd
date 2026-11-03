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


def audit_payload(manifest_path, trace_path):
    """Join selected OWN bindings to content, preserving the full payload gate."""
    base = P.audit(manifest_path, trace_path)
    if base.get('status') != 'PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD':
        return {'status': 'FAIL', 'payload_audit': base, 'scope': SCOPE}
    try:
        manifest_path = Path(manifest_path)
        doc = json.loads(P.read_regular(manifest_path, P.MAX_MANIFEST_BYTES), object_pairs_hook=P.strict_object)
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
        return {'status': 'PASS', 'payload_audit': base, 'selected_source_bindings': selected, 'scope': SCOPE}
    except (ValueError, OSError, KeyError, TypeError) as error:
        return {'status': 'FAIL', 'error': str(error), 'payload_audit': base, 'scope': SCOPE}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    result = audit_capture(args.capture)
    print(json.dumps(result, sort_keys=True))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
