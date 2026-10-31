#!/bin/sh
# Finite CPU contract only. Limit discovery to the plugins used by this test.
set -eu
component=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" -gt 2 ]; then
    printf '%s\n' 'Usage: check.sh [build-directory [system-plugins-directory]]' >&2
    exit 2
fi
packed_build="$component"
if [ "$#" -ge 1 ]; then
    packed_build=$(CDPATH= cd -- "$1" && pwd)
fi
if [ "$#" -eq 2 ]; then
    system_plugins=$2
else
    system_plugins=$(pkg-config --variable=pluginsdir gstreamer-1.0)
fi
test -n "$system_plugins"
system_plugins=$(CDPATH= cd -- "$system_plugins" && pwd)
test -f "$packed_build/libgstcrystalhdpackedscale.so"
test -x "$packed_build/packed-scale-test"
plugins=$(mktemp -d)
trap 'rm -rf -- "$plugins"' EXIT
trap 'exit 1' HUP INT TERM
for name in libgstapp.so libgstcoreelements.so libgstvideoconvertscale.so; do
    test -f "$system_plugins/$name"
    ln -s "$system_plugins/$name" "$plugins/$name"
done
ln -s "$packed_build/libgstcrystalhdpackedscale.so" "$plugins/libgstcrystalhdpackedscale.so"
GST_PLUGIN_PATH="$plugins" GST_PLUGIN_PATH_1_0="$plugins" \
GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0= \
GST_REGISTRY="$plugins/registry.bin" GST_REGISTRY_1_0="$plugins/registry.bin" \
GST_DEBUG=0 "$packed_build/packed-scale-test" --execute
