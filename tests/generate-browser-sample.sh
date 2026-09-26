#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 OUTPUT.mp4 [--av-360p|--av-720p]" >&2
    exit 2
fi
sample_size=640x360
sample_audio=false
case "${2:-}" in
    '') ;;
    --av-360p) sample_audio=true ;;
    --av-720p) sample_size=1280x720; sample_audio=true ;;
    *) echo "unsupported fixture option: $2" >&2; exit 2 ;;
esac
output_file=$1
# Keep the original browser fixture unchanged. The optional local-player
# fixture uses the same pixel identity cells with quiet AAC-LC audio.
if [ "$sample_audio" = true ]; then
    set -- -f lavfi -i sine=frequency=440:sample_rate=48000:duration=12 \
        -map 0:v:0 -map 1:a:0 -af volume=0.02 -c:a aac -b:a 96k -t 12
else
    set -- -an
fi
# Nine binary cells encode the actual frame number, independent of container
# timestamps. White/black reference cells detect blank or corrupt readbacks.
filter='drawbox=x=0:y=0:w=288:h=32:color=black:t=fill'
bit=0
while [ "$bit" -lt 9 ]; do
    divisor=$((1 << bit))
    position=$((8 + 24 * bit))
    filter="$filter,drawbox=x=$position:y=8:w=16:h=16:color=white:t=fill:enable='eq(mod(floor(n/$divisor),2),1)'"
    bit=$((bit + 1))
done
filter="$filter,drawbox=x=240:y=8:w=16:h=16:color=white:t=fill"
ffmpeg -nostdin -hide_banner -loglevel error -n \
    -f lavfi -i "testsrc2=size=$sample_size:rate=30" "$@" -frames:v 360 \
    -vf "$filter" -c:v libx264 -threads 2 -preset medium -crf 18 \
    -profile:v high -pix_fmt yuv420p -g 30 -keyint_min 30 \
    -sc_threshold 0 -movflags +faststart "$output_file"
sha256sum "$output_file"
