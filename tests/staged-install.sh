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

# The independently exposed configuration-tool target must also create public
# parent directories under a restrictive caller umask.
config_stage="$stage_dir/config-only"
(
    umask 077
    make -C "$repo_dir" DESTDIR="$config_stage" install-config
)
test "$(stat -c '%a' "$config_stage/usr")" = 755
test "$(stat -c '%a' "$config_stage/usr/bin")" = 755
test "$(stat -c '%a' "$config_stage/usr/bin/crystalhd-config")" = 755
test "$(stat -c '%a' \
    "$config_stage/usr/share/crystalhd/config-presets-v1.json")" = 644
if grep -F '@PRESET_MANIFEST_RELATIVE@' \
    "$config_stage/usr/bin/crystalhd-config" >/dev/null; then
    echo 'Installed configuration tool retained its source-tree manifest token' >&2
    exit 1
fi
"$config_stage/usr/bin/crystalhd-config" validate \
    --no-system-config --no-user-config --ignore-environment >/dev/null
"$config_stage/usr/bin/crystalhd-config" list-presets --format json >/dev/null
"$config_stage/usr/bin/crystalhd-config" bug-report \
    --no-system-config --no-user-config --ignore-environment >/dev/null
test ! -e "$config_stage/etc/crystalhd/config.ini"
make -C "$repo_dir" DESTDIR="$config_stage" uninstall-config
make -C "$repo_dir" DESTDIR="$config_stage" uninstall-config
test ! -e "$config_stage/usr/bin/crystalhd-config"
test ! -e "$config_stage/usr/share/crystalhd/config-presets-v1.json"
test -f "$config_stage/usr/share/crystalhd/provenance.py"
make -C "$repo_dir" DESTDIR="$config_stage" uninstall-provenance
test ! -e "$config_stage/usr/share/crystalhd/provenance.py"

# Nondefault binary/data directories retain a relocation-safe relationship;
# the generated tool does not search the host installation, cwd or HOME.
custom_stage="$stage_dir/config-custom"
custom_bindir=/opt/crystalhd-tools/bin
custom_datadir=/srv/crystalhd-data/share
make -C "$repo_dir" DESTDIR="$custom_stage" \
    BINDIR="$custom_bindir" DATADIR="$custom_datadir" install-config
test -x "$custom_stage$custom_bindir/crystalhd-config"
test -f "$custom_stage$custom_datadir/crystalhd/config-presets-v1.json"
(cd / && HOME="$stage_dir/unrelated-home" \
    "$custom_stage$custom_bindir/crystalhd-config" \
    list-presets --format json >/dev/null)
(cd / && HOME="$stage_dir/unrelated-home" \
    "$custom_stage$custom_bindir/crystalhd-config" \
    bug-report --no-system-config --no-user-config \
    --ignore-environment >/dev/null)
test ! -e "$custom_stage/etc/crystalhd/config.ini"
make -C "$repo_dir" DESTDIR="$custom_stage" \
    BINDIR="$custom_bindir" DATADIR="$custom_datadir" uninstall-config
test ! -e "$custom_stage$custom_bindir/crystalhd-config"
test ! -e "$custom_stage$custom_datadir/crystalhd/config-presets-v1.json"
test -f "$custom_stage$custom_datadir/crystalhd/provenance.py"
make -C "$repo_dir" DESTDIR="$custom_stage" DATADIR="$custom_datadir" \
    uninstall-provenance

# The generated Python literal must preserve a supplementary Unicode code
# point as UTF-8, rather than splitting it into two surrogate code units.
unicode_stage="$stage_dir/config-unicode"
unicode_datadir='/srv/crystalhd-🧪/share'
make -C "$repo_dir" DESTDIR="$unicode_stage" \
    BINDIR="$custom_bindir" DATADIR="$unicode_datadir" install-config
(cd / && HOME="$stage_dir/unrelated-home" \
    "$unicode_stage$custom_bindir/crystalhd-config" \
    list-presets --format json >/dev/null)
make -C "$repo_dir" DESTDIR="$unicode_stage" \
    BINDIR="$custom_bindir" DATADIR="$unicode_datadir" uninstall-config
test ! -e "$unicode_stage$custom_bindir/crystalhd-config"
test ! -e "$unicode_stage$unicode_datadir/crystalhd/config-presets-v1.json"
test -f "$unicode_stage$unicode_datadir/crystalhd/provenance.py"
make -C "$repo_dir" DESTDIR="$unicode_stage" DATADIR="$unicode_datadir" \
    uninstall-provenance

# The shared verifier survives either consumer's partial uninstall. Full
# runtime uninstall owns its final removal through uninstall-provenance.
helper_stage="$stage_dir/helper-lifecycle"
make -C "$repo_dir" DESTDIR="$helper_stage" install-check install-config
make -C "$repo_dir" DESTDIR="$helper_stage" uninstall-check
test -x "$helper_stage/usr/bin/crystalhd-config"
test -f "$helper_stage/usr/share/crystalhd/provenance.py"
make -C "$repo_dir" DESTDIR="$helper_stage" install-check uninstall-config
test -x "$helper_stage/usr/bin/crystalhd-check"
test -f "$helper_stage/usr/share/crystalhd/provenance.py"
make -C "$repo_dir" DESTDIR="$helper_stage" uninstall-check uninstall-provenance

# A restrictive caller umask must not hide installed components from ordinary
# users. Owner-only checks would miss inaccessible directories in a root install.
(
    umask 077
    make -C "$repo_dir" KVER="$KVER" KDIR="$KDIR" \
        DESTDIR="$stage_dir" install
)
# The private staging root remains private; all newly installed public parents,
# including intermediate directories, must permit unprivileged traversal.
find "$stage_dir" -mindepth 1 -type d -print | while IFS= read -r public_directory; do
    if [ "$(stat -c '%a' "$public_directory")" != 755 ]; then
        echo "Installation directory is not public/traversable: $public_directory" >&2
        exit 1
    fi
done
test "$(stat -c '%a' "$stage_dir")" = 700
test "$(stat -c '%a' "$stage_dir/lib/modules/$KVER/updates/crystalhd.ko")" = 644

# Recursive make recipes execute even with -n; grouping them must not make
# their non-recursive install commands write files during a dry run.
make -n -C "$repo_dir" DESTDIR="$stage_dir/dry-run" install-runtime >/dev/null
test ! -e "$stage_dir/dry-run"

# Exercise the real non-DESTDIR depmod branches without changing host modules.
# Only this isolated check skips ownership preflight, uses a private MODULEDIR,
# and substitutes depmod; normal installation above retains the real guard.
permission_test="$stage_dir/permission-test"
mkdir "$permission_test"
cat >"$permission_test/depmod" <<'EOF'
#!/bin/sh
set -eu
test "$*" = '-a crystalhd-permission-test'
case $(umask) in 0022|022) ;; *) echo 'depmod inherited a restrictive umask' >&2; exit 1 ;; esac
printf '%s\n' "$*" >>"${TEST_DEPMOD_OUTPUT:?}"
: >"${TEST_DEPMOD_INDEX:?}"
EOF
chmod 0755 "$permission_test/depmod"
(
    umask 077
    PATH="$permission_test:$PATH"
    TEST_DEPMOD_OUTPUT="$permission_test/calls"
    TEST_DEPMOD_INDEX="$permission_test/modules.dep"
    export PATH TEST_DEPMOD_OUTPUT TEST_DEPMOD_INDEX
    test "$(command -v depmod)" = "$permission_test/depmod"
    make -C "$repo_dir/driver/linux" -f Makefile.in -o install-preflight \
        KVER=crystalhd-permission-test DESTDIR= \
        MODULEDIR="$permission_test/modules" install-module
    test "$(stat -c '%a' "$TEST_DEPMOD_INDEX")" = 644
    make -C "$repo_dir/driver/linux" -f Makefile.in \
        KVER=crystalhd-permission-test DESTDIR= \
        MODULEDIR="$permission_test/modules" uninstall-module
    test "$(stat -c '%a' "$TEST_DEPMOD_INDEX")" = 644
    test "$(wc -l <"$TEST_DEPMOD_OUTPUT")" -eq 2
)
rm -f -- "$permission_test/depmod" "$permission_test/calls" "$permission_test/modules.dep"
echo "restrictive-umask module paths and install/uninstall depmod passed"

test -f "$stage_dir/lib/modules/$KVER/updates/crystalhd.ko"
package_version=$(sed -n 's/^PACKAGE_VERSION="\([^"]*\)"$/\1/p' "$repo_dir/dkms.conf")
bundle_version=$(cat "$repo_dir/VERSION")
test "$(modinfo -F version "$stage_dir/lib/modules/$KVER/updates/crystalhd.ko")" = "$package_version"
test "$(modinfo -F crystalhd_bundle "$stage_dir/lib/modules/$KVER/updates/crystalhd.ko")" = "$bundle_version"
cmp "$repo_dir/VERSION" "$stage_dir/usr/share/crystalhd/VERSION"
runtime_receipt="$stage_dir/usr/share/crystalhd/runtime-components-v1.json"
provenance_helper="$stage_dir/usr/share/crystalhd/provenance.py"
test -f "$runtime_receipt"
test -f "$provenance_helper"
test "$(stat -c '%a' "$runtime_receipt")" = 644
test "$(stat -c '%a' "$provenance_helper")" = 644
if grep -F "$stage_dir" "$runtime_receipt" >/dev/null; then
    echo 'DESTDIR leaked into the runtime component receipt' >&2
    exit 1
fi
test ! -e "$stage_dir/var/lib/crystalhd/runtime-install.pending"
test -f "$stage_dir/lib/udev/rules.d/20-crystalhd.rules"
test -f "$stage_dir/lib/firmware/bcm70012fw.bin"
test -f "$stage_dir/lib/firmware/bcm70015fw.bin"
module_firmware=$(modinfo -F firmware \
    "$stage_dir/lib/modules/$KVER/updates/crystalhd.ko" | LC_ALL=C sort)
expected_firmware=$(printf '%s\n' bcm70012fw.bin bcm70015fw.bin)
if [ "$module_firmware" != "$expected_firmware" ]; then
    echo "Unexpected module firmware metadata:" >&2
    printf '%s\n' "$module_firmware" >&2
    exit 1
fi
while IFS= read -r firmware_name; do
    test -f "$stage_dir/lib/firmware/$firmware_name"
done <<EOF
$module_firmware
EOF
test -f "$stage_dir/usr/lib/libcrystalhd.so.3.6"
test -L "$stage_dir/usr/lib/libcrystalhd.so.3"
test -L "$stage_dir/usr/lib/libcrystalhd.so"
test "$(readlink "$stage_dir/usr/lib/libcrystalhd.so.3")" = \
    libcrystalhd.so.3.6
test "$(readlink "$stage_dir/usr/lib/libcrystalhd.so")" = \
    libcrystalhd.so.3
test -f "$stage_dir/usr/lib/pkgconfig/libcrystalhd.pc"
test -f "$stage_dir/usr/include/libcrystalhd/libcrystalhd_if.h"
test -f "$stage_dir/usr/include/libcrystalhd/libcrystalhd_raw_frame.h"
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
test -x "$stage_dir/usr/bin/crystalhd-config"
test -f "$stage_dir/usr/share/crystalhd/player.py"
test -f "$stage_dir/usr/share/crystalhd/config-presets-v1.json"
env -u GST_PLUGIN_PATH -u GST_PLUGIN_PATH_1_0 -u LD_LIBRARY_PATH \
    "$stage_dir/usr/bin/crystalhd-play" --help >/dev/null
"$stage_dir/usr/bin/crystalhd-check" --help >/dev/null
"$stage_dir/usr/bin/crystalhd-config" show-effective \
    --no-system-config --no-user-config --ignore-environment >/dev/null
"$stage_dir/usr/bin/crystalhd-config" list-presets --format text >/dev/null
"$stage_dir/usr/bin/crystalhd-config" bug-report \
    --no-system-config --no-user-config --ignore-environment >/dev/null
test ! -e "$stage_dir/etc/crystalhd/config.ini"
va_driver_dir=$(pkg-config --variable=libdir libva)/dri
test -f "$stage_dir$va_driver_dir/crystalhd_drv_video.so"
provenance_output=$(python3 -B "$provenance_helper" verify \
    --receipt "$runtime_receipt" --root "$stage_dir" \
    --runtime-version "$bundle_version" \
    --observed "dil=$stage_dir/usr/lib/libcrystalhd.so.3" \
    --observed "gstreamer=$discovered_plugin" \
    --observed "vaapi=$stage_dir$va_driver_dir/crystalhd_drv_video.so")
printf '%s\n' "$provenance_output" | grep -F 'OK' >/dev/null
if printf '%s\n' "$provenance_output" | grep -F 'FAIL' >/dev/null; then
    printf '%s\n' "$provenance_output" >&2
    exit 1
fi
test ! -e "$stage_dir/usr/bin/crystalhd-chromium"
test ! -e "$stage_dir/usr/bin/setup-crystalhd-chrome-default"

for installed_helper in \
    "$stage_dir/usr/bin/crystalhd-play" \
    "$stage_dir/usr/bin/crystalhd-check" \
    "$stage_dir/usr/bin/crystalhd-config"; do
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
user_settings="$stage_dir/etc/crystalhd/config.ini"
mkdir -p "${user_settings%/*}"
printf '%s\n' 'saved user defaults' >"$user_settings"
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
test "$(cat "$user_settings")" = 'saved user defaults'
test ! -e "$stage_dir/var/lib/crystalhd/runtime-install.pending"
test ! -e "$runtime_receipt"
test ! -e "$provenance_helper"
rm -f -- "$sentinel"
rm -f -- "$dkms_sentinel"
rm -f -- "$user_settings"

remaining=$(find "$stage_dir" \( -type f -o -type l \) -print)
if [ -n "$remaining" ]; then
    echo "Unexpected files remain after staged uninstall:" >&2
    printf '%s\n' "$remaining" >&2
    exit 1
fi

echo "staged install and uninstall layout passed"
