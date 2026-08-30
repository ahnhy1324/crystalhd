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
test -r /dev/dri/renderD128

expected_frames=$(ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$video")
case "$expected_frames" in
    ''|N/A|*[!0-9]*) expected_frames= ;;
esac

started_at=$(date --iso-8601=seconds)
iteration=1
while [ "$iteration" -le "$iterations" ]; do
    : > "$progress_file"
    LIBVA_DRIVER_NAME=crystalhd \
    LIBVA_DRIVERS_PATH="$repo_dir/filters/vaapi" \
    LD_LIBRARY_PATH="$repo_dir/linux_lib/libcrystalhd" \
    ffmpeg -nostdin -hide_banner -loglevel error \
        -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
        -hwaccel_output_format vaapi -i "$video" \
        -vf hwdownload,format=nv12 -an -progress "$progress_file" \
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

if command -v journalctl >/dev/null 2>&1; then
    kernel_findings=$(journalctl -k --since "$started_at" --no-pager 2>/dev/null | \
        grep -Ei 'BUG:|Oops:|general protection|Call Trace|hung task|crystalhd.*(error|fail|timeout|invalid arguments)' || true)
    if [ -n "$kernel_findings" ]; then
        echo "$kernel_findings" >&2
        echo "CrystalHD kernel errors occurred during the stress test" >&2
        exit 1
    fi
fi

echo "CrystalHD VA-API stress test passed: $iterations iterations"
