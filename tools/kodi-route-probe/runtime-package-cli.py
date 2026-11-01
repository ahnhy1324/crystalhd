#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute the private CLIs in an already device-free namespace, software only."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


KINDS = ('avutil', 'avcodec', 'avformat', 'swresample', 'swscale', 'avfilter', 'avdevice')


def require(okay, reason):
    if not okay:
        raise RuntimeError(reason)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(data)
    return value.hexdigest()


def mapped_libraries(path):
    values = set()
    for line in path.read_text().splitlines():
        fields = line.split(None, 5)
        if len(fields) == 6 and fields[5].startswith('/') and any(
                re.fullmatch(r'lib' + kind + r'\.so(?:\.\d+)+', Path(fields[5]).name)
                for kind in KINDS):
            values.add(fields[5])
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    args = parser.parse_args()
    root = args.package.resolve(strict=True)
    reference = json.loads(Path('/reference/expected.json').read_text())
    require(digest(Path('/media/input.ts')) == reference['media']['sha256'], 'input hash mismatch')
    require(digest(Path('/reference/software.nv12')) == reference['retained_software_reference']['sha256'],
            'retained independent software reference hash mismatch')
    require(digest(Path('/reference/independent-ffprobe8.json')) ==
            reference['independent_ffprobe']['clock_and_geometry_raw']['sha256'],
            'independent source-clock reference hash mismatch')
    preflight = subprocess.run(['/usr/bin/python3', '/proof/metadata.py', '--package', str(root)],
                               text=True, capture_output=True, timeout=20)
    require(preflight.returncode == 0 and not preflight.stderr,
            'device-free/compilerless package preflight failed: ' + preflight.stderr)
    metadata = json.loads(preflight.stdout)
    require(metadata['status'] == 'PASS_COMPILERLESS_RELOCATED_METADATA_ONLY', 'preflight incomplete')
    dependencies = json.loads((root / 'metadata/dependencies.json').read_text())
    expected = {(root / 'lib' / soname).resolve().as_posix()
                for soname in dependencies['sonames'].values()}
    reports = []

    def run(label, argv, require_maps=False):
        stdout_path, stderr_path = Path('/work') / (label + '.stdout'), Path('/work') / (label + '.stderr')
        environment = os.environ.copy()
        environment['LD_DEBUG'] = 'libs,files'
        observed, snapshots, complete_snapshots = set(), 0, 0
        started = time.monotonic()
        with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
            child = subprocess.Popen(argv, stdout=stdout, stderr=stderr, env=environment)
            try:
                while child.poll() is None:
                    require(time.monotonic() - started < 25, label + ': deadline exceeded')
                    require(stdout_path.stat().st_size < 1024 * 1024 and
                            stderr_path.stat().st_size < 2 * 1024 * 1024, label + ': log bound exceeded')
                    output = Path('/work/decoded.nv12')
                    require(not output.exists() or output.stat().st_size <= 80 * 1024 * 1024,
                            'raw output bound exceeded')
                    try:
                        values = mapped_libraries(Path('/proc') / str(child.pid) / 'maps')
                        if values:
                            observed |= values
                            snapshots += 1
                            if values == expected:
                                complete_snapshots += 1
                    except (FileNotFoundError, ProcessLookupError):
                        pass
                    time.sleep(0.002)
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
        require(child.returncode == 0, label + ': exit ' + str(child.returncode))
        stderr = stderr_path.read_text()
        initialized = {Path(value.strip()).resolve().as_posix()
                       for value in re.findall(r'calling init: (.+)', stderr)
                       if any(re.fullmatch(r'lib' + kind + r'\.so(?:\.\d+)+', Path(value.strip()).name)
                              for kind in KINDS)}
        require(initialized == expected, label + ': loader used an unexpected FFmpeg set')
        require(observed <= expected, label + ': process mappings differ from package')
        if require_maps:
            require(observed == expected and complete_snapshots > 0, label + ': no complete live CLI DSO census')
        non_loader = [line for line in stderr.splitlines() if line.strip()
                      and not re.match(r'^\s*\d+:', line)]
        require(not non_loader, label + ': unexpected CLI diagnostic: ' + '\n'.join(non_loader)[:500])
        reports.append({'case': label, 'argv': argv, 'exit': child.returncode,
                        'loader_FFmpeg_files': sorted(initialized),
                        'live_process_FFmpeg_files': sorted(observed),
                        'live_map_snapshots': snapshots, 'complete_live_map_snapshots': complete_snapshots,
                        'stdout_sha256': digest(stdout_path), 'stderr_sha256': digest(stderr_path)})
        return stdout_path.read_text()

    ffmpeg, ffprobe = str(root / 'bin/ffmpeg'), str(root / 'bin/ffprobe')
    versions = {'ffmpeg': run('ffmpeg-version', [ffmpeg, '-version']).splitlines()[0],
                'ffprobe': run('ffprobe-version', [ffprobe, '-version']).splitlines()[0]}
    decoders = run('decoders', [ffmpeg, '-hide_banner', '-decoders'])
    require(re.search(r'^ V\S*\s+h264\s', decoders, re.M) is not None, 'software h264 decoder absent')
    output_options = ['-map', '0:v:0', '-an', '-sn', '-dn', '-vf', 'format=nv12',
                      '-c:v', 'rawvideo', '-threads:v', '1', '-pix_fmt', 'nv12',
                      '-fps_mode', 'passthrough', '-enc_time_base', reference['stream']['time_base']]
    run('software-decode', [ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-xerror', '-n',
                           '-copyts', '-readrate', '1', '-readrate_initial_burst', '0',
                           '-hwaccel', 'none', '-threads', '1', '-c:v', 'h264', '-i', '/media/input.ts',
                           *output_options, '-f', 'rawvideo', '/work/decoded.nv12',
                           *output_options, '-f', 'framemd5', '/work/decoded.framemd5'], require_maps=True)
    got = []
    framehash = Path('/work/decoded.framemd5').read_text()
    require(framehash.count('#tb 0: ' + reference['stream']['time_base'] + '\n') == 1,
            'decoded output time base differs from source reference')
    for line in framehash.splitlines():
        if line and not line.startswith('#'):
            values = [value.strip() for value in line.split(',')]
            require(len(values) == 6 and int(values[0]) == 0 and int(values[1]) == int(values[2]),
                    'unexpected decoded frame/clock record')
            got.append({'pts': int(values[2]), 'duration': int(values[3]),
                        'bytes': int(values[4]), 'md5': values[5]})
    require(got == reference['frames'], 'software decoded full-frame hashes/count/PTS differ from independent reference')
    y_bytes = reference['stream']['width'] * reference['stream']['height']
    with Path('/work/decoded.nv12').open('rb') as actual, Path('/reference/software.nv12').open('rb') as expected_file:
        for frame in reference['frames']:
            decoded, expected_frame = actual.read(frame['bytes']), expected_file.read(frame['bytes'])
            require(len(decoded) == len(expected_frame) == frame['bytes'], 'short raw Y/UV frame')
            require(decoded[:y_bytes] == expected_frame[:y_bytes] and
                    decoded[y_bytes:] == expected_frame[y_bytes:], 'software decoded Y/UV bytes differ')
        require(not actual.read(1) and not expected_file.read(1), 'raw output/reference has extra bytes')
    probe = json.loads(run('software-ffprobe', [ffprobe, '-v', 'error', '-threads', '1',
                   '-codec_whitelist', 'h264', '-select_streams', 'v:0', '-show_frames', '-show_streams',
                   '-show_entries', 'frame=pts,duration,width,height,pix_fmt,interlaced_frame,top_field_first:stream=codec_name,width,height,pix_fmt,time_base',
                   '-of', 'json', '/media/input.ts'], require_maps=True))
    stream_fields = ('codec_name', 'width', 'height', 'pix_fmt', 'time_base')
    require(len(probe['streams']) == 1 and
            {name: probe['streams'][0][name] for name in stream_fields} ==
            {name: reference['stream'][name] for name in stream_fields},
            'ffprobe stream metadata differs from independent reference')
    fields = ('pts', 'duration', 'width', 'height', 'pix_fmt', 'interlaced_frame', 'top_field_first')
    actual_frames = [{name: value[name] for name in fields} for value in probe['frames']]
    independent = json.loads(Path('/reference/independent-ffprobe8.json').read_text())
    expected_frames = [{name: value[name] for name in fields} for value in independent['frames']]
    require(actual_frames == expected_frames, 'ffprobe decoded source PTS/geometry/field metadata differs')
    value = {'status': 'PASS_COMPILERLESS_RELOCATED_SOFTWARE_CLI', 'metadata_preflight': metadata,
             'versions': versions, 'cases': reports, 'software_decoder': 'h264, -hwaccel none',
             'frames': len(got), 'time_base': reference['stream']['time_base'],
             'pts_first_last': [got[0]['pts'], got[-1]['pts']],
             'full_NV12_Y_and_UV_bytes_compared': Path('/work/decoded.nv12').stat().st_size,
             'all_raw_bytes_equal_retained_independent_software_reference': True,
             'all_frame_hashes_counts_and_PTS_match': True,
             'ffprobe_all_decoded_clock_geometry_and_field_metadata_match': True,
             'reference_manifest_sha256': digest(Path('/reference/expected.json')),
             'raw_output_sha256': digest(Path('/work/decoded.nv12')),
             'framehash_output_sha256': digest(Path('/work/decoded.framemd5')),
             'scope': 'Actual packaged CLIs, software H264 only, no devices or Kodi; '
                      'existing host kernel and retained Platform, not independent clean OS or native playback.'}
    Path('/work/result.json').write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps(value, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print('runtime-package-cli: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
