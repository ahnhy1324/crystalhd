#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build an unchanged, pinned external implementation into a private prefix.
set -eu

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 SOURCE_CHECKOUT NEW_BUILD_DIR NEW_PRIVATE_PREFIX" >&2
    exit 2
fi

source_dir=$1
build_dir=$2
prefix_dir=$3
for directory in "$source_dir" "$build_dir" "$prefix_dir"; do
    case "$directory" in
        /*) ;;
        *) echo 'All three paths must be absolute.' >&2; exit 2 ;;
    esac
done

revision=12cc1f42f06ff0e85633c2602c96786fbd280b70
test "$(git -C "$source_dir" rev-parse HEAD)" = "$revision"
git -C "$source_dir" diff --quiet HEAD --
test ! -e "$build_dir"
test ! -e "$prefix_dir"
pkg-config --exists libdrm libva zlib
mkdir -p "$build_dir" "$prefix_dir"
cd "$build_dir"

"$source_dir/configure" \
    --prefix="$prefix_dir" \
    --enable-shared --disable-static --disable-doc --disable-debug \
    --disable-autodetect --disable-everything \
    --enable-ffmpeg --enable-ffprobe --disable-ffplay \
    --enable-libdrm --enable-zlib --enable-vaapi --enable-v4l2-m2m \
    --enable-runtime-cpudetect --cpu="${FFMPEG_CPU:-generic}" \
    --enable-decoder=h264,h264_v4l2m2m,aac,pcm_s16le,pcm_s16be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_u8,png,mjpeg \
    --enable-hwaccel=h264_vaapi \
    --enable-parser=h264,aac,aac_latm,png,mjpeg \
    --enable-bsf=h264_mp4toannexb,extract_extradata \
    --enable-demuxer=mov,matroska,mpegts,h264,rawvideo,wav,image2,image2pipe,image_png_pipe,image_jpeg_pipe \
    --enable-muxer=null,rawvideo,framecrc,framemd5 \
    --enable-protocol=file,pipe \
    --enable-encoder=rawvideo,wrapped_avframe,pcm_s16le \
    --enable-filter=scale,format,aformat,aresample,hwdownload,hwupload,null,anull
make -j1
make install
