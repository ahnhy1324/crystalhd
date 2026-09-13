#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 OUTPUT.mp4" >&2
    exit 2
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
    -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 360 \
    -vf "$filter" -an -c:v libx264 -threads 2 -preset medium -crf 18 \
    -profile:v high -pix_fmt yuv420p -g 30 -keyint_min 30 \
    -sc_threshold 0 -movflags +faststart "$1"
sha256sum "$1"
