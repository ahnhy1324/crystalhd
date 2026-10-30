#!/bin/sh
# Finite CPU contract only. Limit discovery to the plugins used by this test.
set -eu
component=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
plugins=$(mktemp -d)
trap 'rm -rf -- "$plugins"' EXIT HUP INT TERM
system_plugins=$(pkg-config --variable=pluginsdir gstreamer-1.0)
for name in libgstapp.so libgstcoreelements.so libgstvideoconvertscale.so; do
    test -f "$system_plugins/$name"
    ln -s "$system_plugins/$name" "$plugins/$name"
done
ln -s "$component/libgstcrystalhdpackedscale.so" "$plugins/libgstcrystalhdpackedscale.so"
GST_PLUGIN_PATH="$plugins" GST_PLUGIN_PATH_1_0="$plugins" \
GST_PLUGIN_SYSTEM_PATH= GST_PLUGIN_SYSTEM_PATH_1_0= \
GST_REGISTRY="$plugins/registry.bin" GST_REGISTRY_1_0="$plugins/registry.bin" \
GST_DEBUG=0 "$component/packed-scale-test" --execute
