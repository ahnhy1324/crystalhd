#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Opt-in local-file probe. Never download an AppImage or use system VLC.
set -eu

die() { printf 'powervlc-playback: %s\n' "$*" >&2; exit 2; }
if [ "$#" -lt 3 ]; then
    die "usage: $0 EXTRACTED_APPIMAGE_ROOT LIBCRYSTALHD_DIR AV360.mp4 [--software] [--controls | --half | --double]"
fi
probe_source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
[ -d "$1" ] || die "extracted AppImage root is not a directory"
probe_app_dir=$(CDPATH= cd -- "$1" && pwd -P)
[ -d "$2" ] || die "libcrystalhd directory is not a directory"
probe_library_dir=$(CDPATH= cd -- "$2" && pwd -P)
[ -f "$3" ] && [ -r "$3" ] || die "fixture must be a readable local file"
probe_video_dir=$(CDPATH= cd -- "$(dirname -- "$3")" && pwd -P)
probe_video="$probe_video_dir/$(basename -- "$3")"
shift 3
# The ELF loader splits these environment variables on colons/spaces.
case "$probe_app_dir:$probe_library_dir" in
    *' '*|*'	'*|*'
'*) die "runtime directories must not contain whitespace" ;;
esac
case "$probe_app_dir" in *:*) die "AppImage path must not contain a colon" ;; esac
case "$probe_library_dir" in *:*) die "library path must not contain a colon" ;; esac
for probe_required in \
    "$probe_source_dir/powervlc-playback.c" \
    "$probe_app_dir/usr/include/vlc/vlc.h" \
    "$probe_app_dir/usr/lib/libpowervlc.so" \
    "$probe_app_dir/usr/lib/libpowervlccore.so.9" \
    "$probe_app_dir/usr/lib/vlc/plugins/codec/libcrystalhd_plugin.so" \
    "$probe_app_dir/usr/lib/vlc/plugins/video_output/libvmem_plugin.so" \
    "$probe_library_dir/libcrystalhd.so.3"; do
    [ -f "$probe_required" ] && [ -r "$probe_required" ] || die "required local SDK/library/plugin is missing: $probe_required"
done
command -v "${CC:-cc}" >/dev/null 2>&1 || die "C compiler is unavailable"
command -v timeout >/dev/null 2>&1 || die "GNU timeout is required"
probe_software=0
probe_scenario=
for probe_option in "$@"; do
    case "$probe_option" in
        --software) [ "$probe_software" -eq 0 ] || die "duplicate --software"; probe_software=1 ;;
        --controls|--half|--double)
            [ -z "$probe_scenario" ] || die "scenario options are mutually exclusive"
            probe_scenario=$probe_option ;;
        *) die "unknown option: $probe_option" ;;
    esac
done

umask 077
probe_temp_dir=$(mktemp -d -t crystalhd-powervlc-probe.XXXXXX)
probe_temp_dir=$(CDPATH= cd -- "$probe_temp_dir" && pwd -P)
cleanup() {
    # Only this invocation's validated mktemp directory, never an input path.
    unset LD_PRELOAD LD_LIBRARY_PATH VLC_PLUGIN_PATH
    case "$probe_temp_dir" in
        */crystalhd-powervlc-probe.??????) rm -rf -- "$probe_temp_dir" ;;
        *) printf 'Refusing unexpected cleanup target\n' >&2 ;;
    esac
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
mkdir "$probe_temp_dir/config" "$probe_temp_dir/cache" "$probe_temp_dir/data" "$probe_temp_dir/runtime"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
    -I"$probe_app_dir/usr/include" "$probe_source_dir/powervlc-playback.c" \
    -L"$probe_app_dir/usr/lib" -Xlinker --enable-new-dtags \
    -Xlinker -rpath -Xlinker "$probe_app_dir/usr/lib" \
    -Xlinker -rpath-link -Xlinker "$probe_app_dir/usr/lib" \
    -lpowervlc -pthread -lm -ldl -o "$probe_temp_dir/probe"

# Explicit preload wins even if this particular PowerVLC build uses RPATH.
# Do not inherit another application's injection or runtime/plugin paths.
LD_LIBRARY_PATH="$probe_library_dir:$probe_app_dir/usr/lib:$probe_app_dir/usr/lib/vlc"
LD_PRELOAD="$probe_library_dir/libcrystalhd.so.3"
CRYSTALHD_PROBE_LIBRARY="$probe_library_dir/libcrystalhd.so.3"
VLC_PLUGIN_PATH="$probe_app_dir/usr/lib/vlc/plugins"
XDG_CONFIG_HOME="$probe_temp_dir/config"
XDG_CACHE_HOME="$probe_temp_dir/cache"
XDG_DATA_HOME="$probe_temp_dir/data"
XDG_RUNTIME_DIR="$probe_temp_dir/runtime"
export LD_LIBRARY_PATH LD_PRELOAD VLC_PLUGIN_PATH CRYSTALHD_PROBE_LIBRARY
export XDG_CONFIG_HOME XDG_CACHE_HOME XDG_DATA_HOME XDG_RUNTIME_DIR
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY SESSION_MANAGER DBUS_SESSION_BUS_ADDRESS
printf 'Opt-in PowerVLC probe: generated 640x360/30fps/360-frame barcode fixture required.\n'
printf 'Offscreen callbacks only; no audible/display or exact cadence claim.\n'
set +e
timeout --signal=TERM --kill-after=5s 145s "$probe_temp_dir/probe" "$probe_video" "$@"
probe_status=$?
set -e
exit "$probe_status"
