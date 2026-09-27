#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_base=$(CDPATH= cd -- "${TMPDIR:-/tmp}" && pwd)
stage_dir=$(mktemp -d "$tmp_base/crystalhd-install.XXXXXX")
registry_file=$(mktemp "$tmp_base/crystalhd-registry.XXXXXX")
KVER=${KVER:-$(uname -r)}
KDIR=${KDIR:-/lib/modules/$KVER/build}

cleanup()
{
    case "$registry_file" in
        "$tmp_base"/crystalhd-registry.*) rm -f -- "$registry_file" ;;
        *) echo "Refusing to remove unexpected path: $registry_file" >&2 ;;
    esac
    case "$stage_dir" in
        "$tmp_base"/crystalhd-install.*) rm -rf -- "$stage_dir" ;;
        *) echo "Refusing to remove unexpected path: $stage_dir" >&2 ;;
    esac
}
trap cleanup EXIT HUP INT TERM

make -C "$repo_dir" KVER="$KVER" KDIR="$KDIR" \
    DESTDIR="$stage_dir" install

test -f "$stage_dir/lib/modules/$KVER/updates/crystalhd.ko"
test -f "$stage_dir/lib/udev/rules.d/20-crystalhd.rules"
test -f "$stage_dir/lib/firmware/bcm70012fw.bin"
test -f "$stage_dir/lib/firmware/bcm70015fw.bin"
test -f "$stage_dir/usr/lib/libcrystalhd.so.3.6"
test -L "$stage_dir/usr/lib/libcrystalhd.so.3"
test -L "$stage_dir/usr/lib/libcrystalhd.so"
test "$(readlink "$stage_dir/usr/lib/libcrystalhd.so.3")" = \
    libcrystalhd.so.3.6
test "$(readlink "$stage_dir/usr/lib/libcrystalhd.so")" = \
    libcrystalhd.so.3
test -f "$stage_dir/usr/lib/pkgconfig/libcrystalhd.pc"
test -f "$stage_dir/usr/include/libcrystalhd/libcrystalhd_if.h"
test -f "$stage_dir/usr/include/libcrystalhd/bc_dts_defs.h"
test -f "$stage_dir/usr/include/libcrystalhd/bc_dts_types.h"
test -f "$stage_dir/usr/include/libcrystalhd/libcrystalhd_version.h"

plugin_dir=$(pkg-config --variable=pluginsdir gstreamer-1.0)
test -f "$stage_dir$plugin_dir/libgstcrystalhd.so"
plugin_details=$(GST_REGISTRY="$registry_file" \
    GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_PATH="$stage_dir$plugin_dir" \
    LD_LIBRARY_PATH="$stage_dir/usr/lib" gst-inspect-1.0 crystalhddec)
discovered_plugin=$(printf '%s\n' "$plugin_details" | awk '$1 == "Filename" { print $2 }')
test "$discovered_plugin" = "$stage_dir$plugin_dir/libgstcrystalhd.so"
discovered_library=$(LD_LIBRARY_PATH="$stage_dir/usr/lib" \
    ldd "$discovered_plugin" | awk '$1 == "libcrystalhd.so.3" { print $3 }')
test "$discovered_library" = "$stage_dir/usr/lib/libcrystalhd.so.3"
echo "staged GStreamer plugin and libcrystalhd discovery passed"
test -x "$stage_dir/usr/bin/crystalhd-play"
test -x "$stage_dir/usr/bin/crystalhd-check"
test -f "$stage_dir/usr/share/crystalhd/player.py"
env -u GST_PLUGIN_PATH -u GST_PLUGIN_PATH_1_0 -u LD_LIBRARY_PATH \
    "$stage_dir/usr/bin/crystalhd-play" --help >/dev/null
"$stage_dir/usr/bin/crystalhd-check" --help >/dev/null
va_driver_dir=$(pkg-config --variable=libdir libva)/dri
test -f "$stage_dir$va_driver_dir/crystalhd_drv_video.so"
test ! -e "$stage_dir/usr/bin/crystalhd-chromium"
test ! -e "$stage_dir/usr/bin/setup-crystalhd-chrome-default"

for installed_helper in \
    "$stage_dir/usr/bin/crystalhd-play" \
    "$stage_dir/usr/bin/crystalhd-check"; do
    if grep -F "$repo_dir" "$installed_helper" >/dev/null; then
        echo "Source-tree path leaked into $installed_helper" >&2
        exit 1
    fi
done

make -C "$repo_dir" DESTDIR="$stage_dir" install-browser
test -x "$stage_dir/usr/bin/crystalhd-chromium"
grep -q -- '--disable-accelerated-video-decode' \
    "$stage_dir/usr/bin/crystalhd-chromium"
test ! -e "$stage_dir/usr/bin/setup-crystalhd-chrome-default"
test -f "$stage_dir/usr/share/crystalhd/h264-only/manifest.json"
test -f "$stage_dir/usr/share/crystalhd/h264-only/prefer-h264.js"
test -f "$stage_dir/usr/share/applications/crystalhd-chromium.desktop"
if grep -F "$repo_dir" "$stage_dir/usr/bin/crystalhd-chromium" >/dev/null; then
    echo "Source-tree path leaked into crystalhd-chromium" >&2
    exit 1
fi

PKG_CONFIG_PATH="$stage_dir/usr/lib/pkgconfig" \
    pkg-config --validate libcrystalhd

if make -C "$repo_dir/filters/gst/gst-plugin-1.0" \
    PKG_CONFIG=false DESTDIR="$stage_dir" uninstall >/dev/null 2>&1; then
    echo "GStreamer uninstall accepted an empty plugin directory" >&2
    exit 1
fi
if make -C "$repo_dir/filters/vaapi" \
    PKG_CONFIG=false DESTDIR="$stage_dir" uninstall >/dev/null 2>&1; then
    echo "VA-API uninstall accepted an empty driver directory" >&2
    exit 1
fi

sentinel="$stage_dir/usr/share/unrelated.keep"
: >"$sentinel"
dkms_sentinel="$stage_dir/lib/modules/$KVER/updates/dkms/crystalhd.ko"
mkdir -p "${dkms_sentinel%/*}"
: >"$dkms_sentinel"

make -C "$repo_dir" DESTDIR="$stage_dir" uninstall-browser
make -C "$repo_dir" DESTDIR="$stage_dir" uninstall-browser
test ! -e "$stage_dir/usr/bin/crystalhd-chromium"
test ! -e "$stage_dir/usr/share/crystalhd/h264-only/manifest.json"
test ! -e "$stage_dir/usr/share/crystalhd/h264-only/prefer-h264.js"
test ! -e "$stage_dir/usr/share/applications/crystalhd-chromium.desktop"
test -f "$sentinel"
test -f "$dkms_sentinel"

make -C "$repo_dir" KVER="$KVER" KDIR="$KDIR" \
    DESTDIR="$stage_dir" uninstall
make -C "$repo_dir" KVER="$KVER" KDIR="$KDIR" \
    DESTDIR="$stage_dir" uninstall
test -f "$sentinel"
test -f "$dkms_sentinel"
rm -f -- "$sentinel"
rm -f -- "$dkms_sentinel"

remaining=$(find "$stage_dir" \( -type f -o -type l \) -print)
if [ -n "$remaining" ]; then
    echo "Unexpected files remain after staged uninstall:" >&2
    printf '%s\n' "$remaining" >&2
    exit 1
fi

echo "staged install and uninstall layout passed"
