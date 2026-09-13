#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stage_dir=$(mktemp -d -t crystalhd-install.XXXXXX)
KVER=${KVER:-$(uname -r)}
KDIR=${KDIR:-/lib/modules/$KVER/build}

cleanup()
{
    case "$stage_dir" in
        /tmp/crystalhd-install.*) rm -rf -- "$stage_dir" ;;
        *) echo "Refusing to remove unexpected path: $stage_dir" >&2 ;;
    esac
}
trap cleanup EXIT HUP INT TERM

make -C "$repo_dir/driver/linux" -f Makefile.in \
    KVER="$KVER" KDIR="$KDIR" DESTDIR="$stage_dir" install
make -C "$repo_dir/linux_lib/libcrystalhd" \
    DESTDIR="$stage_dir" install
make -C "$repo_dir/filters/gst/gst-plugin-1.0" \
    DESTDIR="$stage_dir" install
make -C "$repo_dir/filters/vaapi" DESTDIR="$stage_dir" install
make -C "$repo_dir/browser" DESTDIR="$stage_dir" install

test -f "$stage_dir/lib/modules/$KVER/updates/crystalhd.ko"
test -f "$stage_dir/lib/udev/rules.d/20-crystalhd.rules"
test -f "$stage_dir/lib/firmware/bcm70012fw.bin"
test -f "$stage_dir/lib/firmware/bcm70015fw.bin"
test -f "$stage_dir/usr/lib/libcrystalhd.so.3.6"
test -L "$stage_dir/usr/lib/libcrystalhd.so.3"
test -L "$stage_dir/usr/lib/libcrystalhd.so"
test -f "$stage_dir/usr/lib/pkgconfig/libcrystalhd.pc"
test -f "$stage_dir/usr/include/libcrystalhd/libcrystalhd_if.h"

plugin_dir=$(pkg-config --variable=pluginsdir gstreamer-1.0)
test -f "$stage_dir$plugin_dir/libgstcrystalhd.so"
plugin_details=$(GST_REGISTRY="$stage_dir/gstreamer-registry.bin" \
    GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_PATH="$stage_dir$plugin_dir" \
    LD_LIBRARY_PATH="$stage_dir/usr/lib" gst-inspect-1.0 crystalhddec)
discovered_plugin=$(printf '%s\n' "$plugin_details" | awk '$1 == "Filename" { print $2 }')
test "$discovered_plugin" = "$stage_dir$plugin_dir/libgstcrystalhd.so"
discovered_library=$(LD_LIBRARY_PATH="$stage_dir/usr/lib" \
    ldd "$discovered_plugin" | awk '$1 == "libcrystalhd.so.3" { print $3 }')
test "$discovered_library" = "$stage_dir/usr/lib/libcrystalhd.so.3"
echo "staged GStreamer plugin and libcrystalhd discovery passed"
va_driver_dir=$(pkg-config --variable=libdir libva)/dri
test -f "$stage_dir$va_driver_dir/crystalhd_drv_video.so"
test -x "$stage_dir/usr/bin/crystalhd-chromium"
grep -q -- '--disable-accelerated-video-decode' \
    "$stage_dir/usr/bin/crystalhd-chromium"
test -x "$stage_dir/usr/bin/setup-crystalhd-chrome-default"
test -f "$stage_dir/usr/share/crystalhd/h264-only/manifest.json"
test -f "$stage_dir/usr/share/crystalhd/h264-only/prefer-h264.js"
test -f "$stage_dir/usr/share/applications/crystalhd-chromium.desktop"

PKG_CONFIG_PATH="$stage_dir/usr/lib/pkgconfig" \
    pkg-config --validate libcrystalhd

echo "staged install layout passed"
