#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 VIDEO.mp4" >&2
    exit 2
fi

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
video=$1
loaded_here=false

test -r "$video"
make -C "$repo_dir" all

cleanup()
{
    if [ "$loaded_here" = true ]; then
        sudo rmmod crystalhd
    fi
}
trap cleanup EXIT HUP INT TERM

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

GST_PLUGIN_PATH="$repo_dir/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$repo_dir/linux_lib/libcrystalhd" \
gst-launch-1.0 -q \
    filesrc location="$video" ! qtdemux name=demux \
    demux.video_0 ! queue ! h264parse ! crystalhddec ! fakesink sync=false

echo "CrystalHD GStreamer hardware decode passed"
