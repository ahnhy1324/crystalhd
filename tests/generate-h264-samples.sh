#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 OUTPUT_DIRECTORY [640x360|1920x1080]" >&2
    exit 2
fi
sample_size=${2:-640x360}
case "$sample_size" in
    640x360|1920x1080) ;;
    *) echo "unsupported sample size: $sample_size" >&2; exit 2 ;;
esac
mkdir -p -- "$1"
output_dir=$(CDPATH= cd -- "$1" && pwd)

# Six seconds, with a one-second closed GOP, exercise reorder and seek while
# defaulting to a small fixture for the reference BCM70015 laptop. The optional
# Full HD size exercises coded-height padding (1088) and higher throughput.
# Never overwrite
# existing samples: their checksums may already appear in a hardware report.
for profile in baseline main high; do
    ffmpeg -nostdin -hide_banner -loglevel error -n \
        -f lavfi -i "testsrc2=size=$sample_size:rate=30" -frames:v 180 \
        -an -c:v libx264 -threads 2 -preset medium -crf 20 \
        -profile:v "$profile" -pix_fmt yuv420p \
        -g 30 -keyint_min 30 -sc_threshold 0 \
        "$output_dir/$profile.mp4"
    ffprobe -v error -select_streams v:0 -count_frames \
        -show_entries stream=codec_name,profile,width,height,field_order,r_frame_rate,nb_read_frames \
        -of default=nw=1 "$output_dir/$profile.mp4"
    sha256sum "$output_dir/$profile.mp4"
done
