#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 VIDEO.mp4 [ITERATIONS]" >&2
    exit 2
fi

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
video=$1
iterations=${2:-10}
loaded_here=false
timeout_seconds=${CRYSTALHD_TEST_TIMEOUT:-120}
drm_device=${CRYSTALHD_DRM_DEVICE:-/dev/dri/renderD128}

case "$timeout_seconds" in
    ''|*[!0-9]*|0) echo "CRYSTALHD_TEST_TIMEOUT must be a positive integer" >&2; exit 2 ;;
esac
if [ "$timeout_seconds" -eq 0 ]; then
    echo "CRYSTALHD_TEST_TIMEOUT must be a positive integer" >&2
    exit 2
fi

case "$iterations" in
    ''|*[!0-9]*)
        echo "iterations must be a positive integer" >&2
        exit 2
        ;;
esac
if [ "$iterations" -eq 0 ]; then
    echo "iterations must be a positive integer" >&2
    exit 2
fi

progress_file=$(mktemp /tmp/crystalhd-vaapi-progress.XXXXXX)

cleanup()
{
    rm -f -- "$progress_file"
    if [ "$loaded_here" = true ]; then
        sudo rmmod crystalhd
    fi
}
trap cleanup EXIT HUP INT TERM

test -r "$video"
command -v ffmpeg >/dev/null
command -v ffprobe >/dev/null
command -v timeout >/dev/null
codec=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
    -of default=nw=1:nk=1 "$video")
if [ "$codec" != h264 ]; then
    echo "CrystalHD VA-API validation requires H.264 input (found: $codec)" >&2
    exit 2
fi
make -C "$repo_dir" all

if lsmod | grep -q '^crystalhd '; then
    loaded_srcversion=$(cat /sys/module/crystalhd/srcversion 2>/dev/null || true)
    built_srcversion=$(modinfo -F srcversion \
        "$repo_dir/driver/linux/crystalhd.ko" 2>/dev/null || true)
    if [ -n "$loaded_srcversion" ] && [ -n "$built_srcversion" ] && \
       [ "$loaded_srcversion" != "$built_srcversion" ]; then
        echo "loaded crystalhd module does not match this build" >&2
        echo "unload it or reboot before running the stress test" >&2
        exit 1
    fi
else
    sudo insmod "$repo_dir/driver/linux/crystalhd.ko"
    loaded_here=true
    udevadm settle --timeout=10
fi
test -r /dev/crystalhd
test -r "$drm_device"

expected_frames=$(ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$video")
case "$expected_frames" in
    ''|N/A|*[!0-9]*|0)
        echo "could not establish a positive reference frame count" >&2
        exit 1
        ;;
esac

started_at=$(date '+%Y-%m-%d %H:%M:%S.%6N')
iteration=1
while [ "$iteration" -le "$iterations" ]; do
    : > "$progress_file"
    LIBVA_DRIVER_NAME=crystalhd \
    LIBVA_DRIVERS_PATH="$repo_dir/filters/vaapi" \
    LD_LIBRARY_PATH="$repo_dir/linux_lib/libcrystalhd" \
    timeout --foreground --kill-after=10 "$timeout_seconds" \
    ffmpeg -nostdin -hide_banner -loglevel error -xerror \
        -hwaccel vaapi -hwaccel_device "$drm_device" \
        -hwaccel_output_format vaapi -i "$video" \
        -map 0:v:0 -vf hwdownload,format=nv12 -fps_mode passthrough \
        -an -progress "$progress_file" \
        -f null -

    decoded_frames=$(awk -F= '$1 == "frame" { value=$2 } END { print value }' \
        "$progress_file")
    if [ -n "$expected_frames" ] && [ "$decoded_frames" != "$expected_frames" ]; then
        echo "iteration $iteration decoded $decoded_frames/$expected_frames frames" >&2
        exit 1
    fi
    echo "iteration $iteration/$iterations decoded ${decoded_frames:-unknown} frames"
    iteration=$((iteration + 1))
done

sh "$repo_dir/tests/kernel-log-check.sh" "$started_at"

echo "CrystalHD VA-API stress test passed: $iterations iterations"
