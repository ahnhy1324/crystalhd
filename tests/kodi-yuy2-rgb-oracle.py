#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare exact handed-off YUY2 rows with Kodi 22 GLES RenderCapture BGRA.

Independent floating-point model of the pinned SDR shader: limited-range
BT.709/601 matrix; when required, gamma 2.2 -> xy-primary XYZ conversion ->
BT.709 -> inverse gamma 2.2. This is not physical scanout or HDR acceptance.
The permitted rounding error is fixed at 2/255 per RGB channel.
"""
import argparse
import json
from pathlib import Path


def multiply(a, b):
    return [[sum(x * y for x, y in zip(row, col)) for col in zip(*b)] for row in a]


def inverse(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[v / det for v in row] for row in
            ((e*i-f*h, c*h-b*i, b*f-c*e),
             (f*g-d*i, a*i-c*g, c*d-a*f),
             (d*h-e*g, b*g-a*h, a*e-b*d))]


def to_xyz(primaries):
    m = [[x/y for x, y in primaries], [1, 1, 1],
         [(1-x-y)/y for x, y in primaries]]
    scale = multiply(inverse(m), [[.3127/.3290], [1], [(1-.3127-.3290)/.3290]])
    return [[value * scale[column][0] for column, value in enumerate(row)] for row in m]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('yuy2', type=Path)
    p.add_argument('bgra', type=Path)
    p.add_argument('--width', type=int, required=True)
    p.add_argument('--height', type=int, required=True)
    p.add_argument('--matrix', type=int, choices=(1, 5, 6), required=True)
    p.add_argument('--primaries', type=int, choices=(1, 5), required=True)
    args = p.parse_args()
    w, h = args.width, args.height
    if not (0 < w <= 1920 and w % 2 == 0 and 0 < h <= 1088):
        p.error('requires bounded even-width native geometry')
    source, captured = args.yuy2.read_bytes(), args.bgra.read_bytes()
    if len(source) != w*h*2 or len(captured) != w*h*4:
        p.error('requires exact active-row files, without padding/cropping')
    dst = to_xyz(((.640, .330), (.300, .600), (.150, .060)))
    src = to_xyz(((.640, .330), (.290, .600), (.150, .060)))
    gamut = multiply(inverse(dst), src) if args.primaries == 5 else None
    kr, kb = (.2126, .0722) if args.matrix == 1 else (.299, .114)
    maximum = total = over = alpha = 0
    cache = {}
    for y in range(h):
        for x in range(w):
            q = (y*w + (x//2)*2)*2
            yy, u, v = source[q + (x % 2)*2], source[q+1]-128, source[q+3]-128
            key = yy, u, v
            rgb = cache.get(key)
            if rgb is None:
                c = (yy-16)*255/219
                rgb = [c+2*(1-kr)*255/224*v,
                       c-2*kb*(1-kb)/(1-kr-kb)*255/224*u-2*kr*(1-kr)/(1-kr-kb)*255/224*v,
                       c+2*(1-kb)*255/224*u]
                if gamut:
                    linear = [[max(0, value/255)**2.2] for value in rgb]
                    rgb = [max(0, value[0])**(1/2.2)*255 for value in multiply(gamut, linear)]
                rgb = tuple(max(0, min(255, round(value))) for value in reversed(rgb))
                cache[key] = rgb
            offset = (y*w+x)*4
            for channel, value in enumerate(rgb):
                error = abs(captured[offset+channel]-value)
                total += error; maximum = max(maximum, error); over += error > 2
            alpha += captured[offset+3] != 255
    result = dict(status='PASS' if not over and not alpha else 'FAIL', width=w, height=h,
                  matrix_iso=args.matrix, primaries_iso=args.primaries,
                  max_rgb_error=maximum, mean_rgb_error=total/(w*h*3),
                  channels_over_2=over, nonopaque_pixels=alpha,
                  scope='exact source rows to GLES capture; no physical scanout or AV claim')
    print(json.dumps(result, indent=2))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
