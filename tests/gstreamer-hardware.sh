#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 VIDEO.mp4|VIDEO.h264 [ITERATIONS]" >&2
    exit 2
fi

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
video=$1
iterations=${2:-2}
timeout_seconds=${CRYSTALHD_TEST_TIMEOUT:-120}
loaded_here=false

case "$iterations" in
    ''|*[!0-9]*) echo "iterations must be a positive integer" >&2; exit 2 ;;
esac
if [ "$iterations" -eq 0 ]; then
    echo "iterations must be a positive integer" >&2
    exit 2
fi
case "$timeout_seconds" in
    ''|*[!0-9]*) echo "CRYSTALHD_TEST_TIMEOUT must be a positive integer" >&2; exit 2 ;;
esac
if [ "$timeout_seconds" -eq 0 ]; then
    echo "CRYSTALHD_TEST_TIMEOUT must be a positive integer" >&2
    exit 2
fi
wall_timeout=$((timeout_seconds + 10))
case "${CRYSTALHD_TEST_SEEK:-0}" in
    0) set -- ;;
    1) set -- --seek; wall_timeout=$((timeout_seconds * 3 + 10)) ;;
    *) echo "CRYSTALHD_TEST_SEEK must be 0 or 1" >&2; exit 2 ;;
esac
if [ ! -f "$video" ] || [ ! -r "$video" ]; then
    echo "input video is not a readable file: $video" >&2
    exit 2
fi
for command in ffprobe gst-inspect-1.0 timeout; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "required command is missing: $command" >&2
        exit 2
    fi
done
codec=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
    -of default=nw=1:nk=1 "$video")
if [ "$codec" != h264 ]; then
    echo "this hardware test requires progressive H.264; found: ${codec:-no video}" >&2
    exit 2
fi
profile=$(ffprobe -v error -select_streams v:0 -show_entries stream=profile \
    -of default=nw=1:nk=1 "$video")
case "$profile" in
    'Constrained Baseline'|Baseline|Main|High) ;;
    *) echo "this test supports H.264 Baseline, Main or High; found profile: $profile" >&2; exit 2 ;;
esac
field_order=$(ffprobe -v error -select_streams v:0 -show_entries stream=field_order \
    -of default=nw=1:nk=1 "$video")
case "$field_order" in
    progressive|unknown|'') ;;
    *) echo "interlaced H.264 is outside this test's validated coverage" >&2; exit 2 ;;
esac
format=$(ffprobe -v error -show_entries format=format_name \
    -of default=nw=1:nk=1 "$video")
case "$format" in
    *mp4*) format=mp4 ;;
    h264) ;;
    *) echo "this test accepts MP4 or H.264 Annex-B; found container: $format" >&2; exit 2 ;;
esac
expected_frames=$(ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$video")
case "$expected_frames" in
    ''|N/A|*[!0-9]*)
        echo "ffprobe could not determine an exact decoded frame count" >&2
        exit 2
        ;;
esac
if [ "$expected_frames" -eq 0 ]; then
    echo "input contains no decodable video frames" >&2
    exit 2
fi
ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name,profile,width,height,r_frame_rate,field_order \
    -of default=nw=1 "$video"
make -C "$repo_dir" all
make -C "$repo_dir/filters/gst/gst-plugin-1.0" gstreamer-playback-test

export GST_PLUGIN_PATH="$repo_dir/filters/gst/gst-plugin-1.0"
export LD_LIBRARY_PATH="$repo_dir/linux_lib/libcrystalhd${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
registry=$(mktemp /tmp/crystalhd-gstreamer-registry.XXXXXX)
export GST_REGISTRY="$registry"

cleanup()
{
    rm -f -- "$registry"
    if [ "$loaded_here" = true ]; then
        sudo rmmod crystalhd
    fi
}
trap cleanup EXIT HUP INT TERM

for element in crystalhddec h264parse fakesink; do
    if ! gst-inspect-1.0 "$element" >/dev/null; then
        echo "GStreamer element is unavailable: $element (check plugin packages and library path)" >&2
        exit 1
    fi
done
gst-inspect-1.0 crystalhddec | sed -n '/  Filename /p'
if [ "$format" = mp4 ]; then
    gst-inspect-1.0 qtdemux >/dev/null
fi

started_at=$(date '+%Y-%m-%d %H:%M:%S.%6N')
if lsmod | grep -q '^crystalhd '; then
    loaded_srcversion=$(cat /sys/module/crystalhd/srcversion 2>/dev/null || true)
    built_srcversion=$(modinfo -F srcversion \
        "$repo_dir/driver/linux/crystalhd.ko" 2>/dev/null || true)
    if [ -n "$loaded_srcversion" ] && [ -n "$built_srcversion" ] && \
       [ "$loaded_srcversion" != "$built_srcversion" ]; then
        echo "loaded crystalhd module does not match this build" >&2
        echo "unload it or reboot before running the hardware test" >&2
        exit 1
    fi
else
    sudo insmod "$repo_dir/driver/linux/crystalhd.ko"
    loaded_here=true
    udevadm settle --timeout=10
fi

if [ ! -r /dev/crystalhd ] || [ ! -w /dev/crystalhd ]; then
    echo "/dev/crystalhd is missing or inaccessible; check module binding and video-group/udev permissions" >&2
    exit 1
fi

iteration=1
while [ "$iteration" -le "$iterations" ]; do
    timeout --foreground --kill-after=10 "$wall_timeout" \
        "$repo_dir/filters/gst/gst-plugin-1.0/gstreamer-playback-test" \
        "$video" "$expected_frames" "$format" "$timeout_seconds" "$@"
    echo "GStreamer iteration $iteration/$iterations passed"
    iteration=$((iteration + 1))
done

sh "$repo_dir/tests/kernel-log-check.sh" "$started_at"

echo "CrystalHD GStreamer hardware decode passed: $iterations complete runs"
