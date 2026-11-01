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
# Strip trailing slashes before -L checks so they cannot hide a dangling link.
# Preserve / so the fresh-directory check rejects it.
while [ "$build_dir" != / ] && [ "${build_dir%/}" != "$build_dir" ]; do
    build_dir=${build_dir%/}
done
while [ "$prefix_dir" != / ] && [ "${prefix_dir%/}" != "$prefix_dir" ]; do
    prefix_dir=${prefix_dir%/}
done

revision=12cc1f42f06ff0e85633c2602c96786fbd280b70
test "$(git -C "$source_dir" rev-parse HEAD)" = "$revision"
git -C "$source_dir" diff --quiet HEAD --
test ! -e "$build_dir"
test ! -L "$build_dir"
test ! -e "$prefix_dir"
test ! -L "$prefix_dir"
# Resolve missing components as well as existing parent symlinks before
# comparing paths. Recheck freshness because a literal missing/../existing
# path can hide a directory that already exists at its canonical location.
build_dir=$(realpath -m -- "$build_dir")
prefix_dir=$(realpath -m -- "$prefix_dir")
test ! -e "$build_dir"
test ! -L "$build_dir"
test ! -e "$prefix_dir"
test ! -L "$prefix_dir"
case "$build_dir/" in
    "$prefix_dir/"*)
        echo 'Build and private-prefix output directories must be disjoint.' >&2
        exit 2 ;;
esac
case "$prefix_dir/" in
    "$build_dir/"*)
        echo 'Build and private-prefix output directories must be disjoint.' >&2
        exit 2 ;;
esac
pkg-config --exists libdrm libva zlib
mkdir -p "$build_dir"
cd "$build_dir"
stage_dir=$build_dir/runtime-stage

# Keep personal output paths out of av*_configuration()/ffmpeg -buildconf.
# DESTDIR supplies the temporary installation root; /usr is metadata only.
"$source_dir/configure" \
    --prefix=/usr \
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
make install DESTDIR="$stage_dir"
test -d "$stage_dir/usr"
# Publish the existing private-prefix layout only after install succeeds.
# -T prevents nesting; -n also preserves a destination created after our check.
# GNU mv reports a skipped no-clobber move as success, so check its source too.
test ! -e "$prefix_dir"
test ! -L "$prefix_dir"
mkdir -p "$(dirname -- "$prefix_dir")"
mv -T -n -- "$stage_dir/usr" "$prefix_dir"
test ! -e "$stage_dir/usr"
