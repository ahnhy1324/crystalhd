#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Metadata-only child for the compilerless private-runtime namespace proof."""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys


KINDS = ('avutil', 'swresample', 'swscale', 'avcodec', 'avformat', 'avfilter', 'avdevice')
TOOLS = ('cc', 'gcc', 'g++', 'c++', 'clang', 'clang++', 'make', 'nasm', 'yasm', 'meson', 'ninja')


def require(okay, message):
    if not okay:
        raise RuntimeError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    args = parser.parse_args()
    root = args.package.resolve(strict=True)
    tools = {name: shutil.which(name) for name in TOOLS}
    require(not any(tools.values()), 'compiler/build tool is present in the target namespace')
    require(not Path('/dev/crystalhd').exists() and not Path('/dev/dri').exists()
            and not list(Path('/dev').glob('video*')) and not Path('/sys').exists(),
            'device or sysfs mount entered metadata namespace')
    require(not Path('/app').exists() and not Path('/gl').exists()
            and not Path('/probe/deps').exists() and not Path('/native-lib').exists(),
            'old application/GL/helper/library overlay entered namespace')
    files = json.loads((root / 'metadata/files.json').read_bytes())
    for entry in files:
        path = root / entry['path']
        if entry['kind'] == 'file':
            require(path.is_file() and sha(path.read_bytes()) == entry['sha256'],
                    'target file does not match package manifest: ' + entry['path'])
        else:
            require(path.is_symlink() and path.readlink() == Path(entry['target']),
                    'target SONAME link changed')
    dependencies = json.loads((root / 'metadata/dependencies.json').read_bytes())
    loaded, expected, versions, configurations = {}, {}, {}, {}
    embedded_prefixes = set()
    for kind in KINDS:
        soname = dependencies['sonames'][kind]
        path = (root / 'lib' / soname).resolve(strict=True)
        require(path.parent == root / 'lib', 'library leaves relocated package')
        expected[kind] = str(path)
        lib = ctypes.CDLL(str(path), mode=ctypes.RTLD_GLOBAL)
        function = getattr(lib, kind + '_version')
        function.argtypes = []
        function.restype = ctypes.c_uint
        value = function()
        require(value >> 16 == int(soname.rsplit('.', 1)[1]), 'loaded library ABI major mismatch')
        versions[soname] = [value >> 16, (value >> 8) & 255, value & 255]
        configuration = getattr(lib, kind + '_configuration')
        configuration.argtypes = []
        configuration.restype = ctypes.c_char_p
        raw = configuration()
        require(raw is not None, 'missing library build configuration')
        configurations[kind] = sha(raw)
        for prefix in re.findall(rb'--prefix=([^\x00\s]+)', raw):
            require(not Path(prefix.decode()).exists(), 'original build prefix is visible')
            embedded_prefixes.add(sha(prefix))
        loaded[kind] = lib
    provenance = json.loads((root / 'metadata/provenance.json').read_bytes())
    require(sorted(embedded_prefixes) == provenance['embedded_configure_prefix_sha256'],
            'loaded configure prefixes disagree with provenance inventory')
    mapped = {kind: set() for kind in KINDS}
    for line in Path('/proc/self/maps').read_text().splitlines():
        fields = line.split(None, 5)
        if len(fields) < 6 or not fields[5].startswith('/'):
            continue
        path = fields[5]
        require(path.startswith('/usr/') or path.startswith(str(root) + '/'),
                'mapped library outside pinned Platform/package')
        for kind in KINDS:
            if re.fullmatch(r'lib' + kind + r'\.so(?:\.\d+)+', Path(path).name):
                mapped[kind].add(path)
    require(all(mapped[kind] == {expected[kind]} for kind in KINDS),
            'a FFmpeg library fell back outside the exact relocated set')

    class HWConfig(ctypes.Structure):
        _fields_ = [('pix_fmt', ctypes.c_int), ('methods', ctypes.c_int), ('device_type', ctypes.c_int)]

    codec, util = loaded['avcodec'], loaded['avutil']
    codec.avcodec_find_decoder_by_name.argtypes = [ctypes.c_char_p]
    codec.avcodec_find_decoder_by_name.restype = ctypes.c_void_p
    codec.avcodec_get_hw_config.argtypes = [ctypes.c_void_p, ctypes.c_int]
    codec.avcodec_get_hw_config.restype = ctypes.POINTER(HWConfig)
    util.av_get_pix_fmt_name.argtypes = [ctypes.c_int]
    util.av_get_pix_fmt_name.restype = ctypes.c_char_p
    decoder = codec.avcodec_find_decoder_by_name(b'h264_v4l2m2m')
    require(decoder is not None, 'native decoder metadata is absent')
    configs = []
    for index in range(32):
        value = codec.avcodec_get_hw_config(decoder, index)
        if not value:
            break
        value = value.contents
        name = util.av_get_pix_fmt_name(value.pix_fmt)
        configs.append({'format': name.decode() if name else None,
                        'methods': value.methods, 'device_type': value.device_type})
    else:
        raise RuntimeError('hardware metadata list did not terminate')
    require(any(value['format'] == 'drm_prime' and value['methods'] & 5 for value in configs),
            'no Kodi-shaped DRM PRIME hardware metadata candidate')
    print(json.dumps({'status': 'PASS_COMPILERLESS_RELOCATED_METADATA_ONLY',
                      'package_prefix': str(root), 'compiler_tools': tools,
                      'no_decode_GPU_or_sysfs_devices': True,
                      'no_app_GL_DIL_Gst_probe_dependency_overlays': True,
                      'all_seven_FFmpeg_mappings_match_exact_package': True,
                      'original_build_prefix_visible': False,
                      'versions': versions, 'configuration_sha256': configurations,
                      'h264_v4l2m2m_HW_configs': configs,
                      'scope': 'Pinned Platform on the existing host kernel; not an independent '
                      'clean OS, Kodi launch, decode, graphics or playback proof.'}, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, AttributeError) as error:
        print('runtime-package-metadata: ' + str(error), file=sys.stderr)
        raise SystemExit(2)
