#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
checker=$repo_dir/scripts/crystalhd-check
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/crystalhd-check-test.XXXXXX")
mock_dir=$test_dir/bin
mock_log=$test_dir/mock.log
case_no=0

unset GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0
unset GST_PLUGIN_SYSTEM_PATH GST_PLUGIN_SYSTEM_PATH_1_0
unset LD_LIBRARY_PATH LIBVA_DRIVERS_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR

cleanup()
{
	chmod -R u+w "$test_dir" 2>/dev/null || true
	rm -rf -- "$test_dir"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
	printf 'crystalhd-check test failed: %s\n' "$*" >&2
	exit 1
}

assert_status()
{
	[ "$check_status" -eq "$1" ] || {
		printf '%s\n' "$check_output" >&2
		fail "expected exit status $1, got $check_status"
	}
}

assert_contains()
{
	case "$check_output" in
		*"$1"*) ;;
		*)
			printf '%s\n' "$check_output" >&2
			fail "missing output: $1"
			;;
	esac
}

make_mocks()
{
	mkdir -p "$mock_dir"
	cat >"$mock_dir/mock-command" <<'EOF'
#!/bin/sh
set -u

name=${0##*/}
printf '%s %s\n' "$name" "$*" >>"$MOCK_LOG"
case "$name" in
	uname)
		case "${1:-}" in
			-r) printf '%s\n' 6.6-test ;;
			-m) printf '%s\n' x86_64 ;;
			*) exit 2 ;;
		esac
		;;
	getconf)
		[ "${1:-}" = LONG_BIT ] || exit 2
		printf '%s\n' 64
		;;
	modinfo)
		[ "${1:-}" = -F ] || exit 2
		[ -z "${MOCK_INVALID_MODULE:-}" ] || [ "${3:-}" != "$MOCK_INVALID_MODULE" ] || exit 2
		case "${2:-}" in
			name) printf '%s\n' crystalhd ;;
			srcversion)
				if [ -n "${MOCK_BAD_MODULE:-}" ] && [ "${3:-}" = "$MOCK_BAD_MODULE" ]; then
					printf '%s\n' BAD000
				else
					printf '%s\n' GOOD000
				fi
				;;
			*) exit 2 ;;
		esac
		;;
	dkms)
		[ -z "${MOCK_DKMS_STATUS:-}" ] || printf '%s\n' "$MOCK_DKMS_STATUS"
		;;
	gst-inspect-1.0)
		[ "${1:-}" = crystalhddec ] || exit 2
		[ -z "${GST_PLUGIN_PATH+x}${GST_PLUGIN_PATH_1_0+x}" ] || exit 77
		[ -z "${GST_PLUGIN_SYSTEM_PATH+x}${GST_PLUGIN_SYSTEM_PATH_1_0+x}" ] || exit 77
		[ -z "${LD_LIBRARY_PATH+x}" ] || exit 77
		if [ -n "${MOCK_GST_PLUGIN:-}" ]; then
			printf 'Plugin Details:\n  Filename                 %s\n' "$MOCK_GST_PLUGIN"
		else
			printf 'Plugin Details:\n  Filename                 %s/usr/lib/gstreamer-1.0/libgstcrystalhd.so\n' "$MOCK_ROOT"
		fi
		;;
	pkg-config)
		[ -z "${PKG_CONFIG_PATH+x}${PKG_CONFIG_LIBDIR+x}" ] || exit 77
		case "$*" in
			'--exists gstreamer-1.0'|'--exists libva') exit 0 ;;
			'--variable=pluginsdir gstreamer-1.0') printf '%s\n' /usr/lib/gstreamer-1.0 ;;
			'--variable=libdir libva') printf '%s\n' /usr/lib ;;
			*) exit 1 ;;
		esac
		;;
	ldd)
		[ -z "${LD_LIBRARY_PATH+x}" ] || exit 77
		if [ "${MOCK_LDD_NOT_FOUND:-0}" -eq 1 ]; then
			printf '%s\n' 'libcrystalhd.so.3 => not found'
		else
			printf 'libcrystalhd.so.3 => %s/usr/lib/libcrystalhd.so.3 (0x0000)\n' "$MOCK_ROOT"
		fi
		;;
	vainfo)
		[ -z "${LIBVA_DRIVERS_PATH+x}${LD_LIBRARY_PATH+x}" ] || exit 77
		if [ -n "${MOCK_VA_OPENED:-}" ]; then
			printf 'libva info: Trying to open %s\n' "$MOCK_VA_OPENED"
		else
			printf 'libva info: Trying to open %s/usr/lib/dri/crystalhd_drv_video.so\n' "$MOCK_ROOT"
		fi
		;;
	fuser)
		[ -z "${MOCK_FUSER_OUTPUT:-}" ] || printf '%s\n' "$MOCK_FUSER_OUTPUT"
		exit "${MOCK_FUSER_STATUS:-1}"
		;;
	ps)
		printf '%s\n' "${MOCK_PS_COMMAND:-player}"
		;;
	file)
		printf '%s\n' 'ELF 64-bit LSB shared object, x86-64'
		;;
	stat)
		case "${2:-}" in
			%a) printf '%s\n' 660 ;;
			%U:%G) printf '%s\n' root:video ;;
			*) exit 2 ;;
		esac
		;;
	modprobe|insmod|rmmod|depmod|sysctl|tee)
		exit 99
		;;
	*) exit 2 ;;
esac
EOF
	chmod 0755 "$mock_dir/mock-command"
		for command_name in uname getconf modinfo dkms gst-inspect-1.0 pkg-config \
			ldd vainfo fuser ps file stat modprobe insmod rmmod depmod sysctl tee; do
		ln -s mock-command "$mock_dir/$command_name"
	done
}

make_root()
{
	case_no=$((case_no + 1))
	root=$test_dir/root-$case_no
	pci=$root/sys/bus/pci/devices/0000:03:00.0
	mkdir -p "$pci" "$root/sys/bus/pci/drivers/crystalhd" \
		"$root/sys/module/crystalhd/parameters" \
			"$root/lib/modules/6.6-test/updates" \
			"$root/lib/modules/6.6-test/weak-updates" "$root/lib/firmware" \
		"$root/dev/dri" "$root/usr/lib/gstreamer-1.0" \
		"$root/usr/lib/dri"
	printf '%s\n' 0x14e4 >"$pci/vendor"
	printf '%s\n' 0x1615 >"$pci/device"
	ln -s "$root/sys/bus/pci/drivers/crystalhd" "$pci/driver"
	printf '%s\n' GOOD000 >"$root/sys/module/crystalhd/srcversion"
	printf '%s\n' N >"$root/sys/module/crystalhd/parameters/force_l0s_off"
		: >"$root/lib/modules/6.6-test/updates/crystalhd.ko"
		ln -s ../updates/crystalhd.ko \
			"$root/lib/modules/6.6-test/weak-updates/crystalhd.ko"
	: >"$root/lib/firmware/bcm70012fw.bin"
	: >"$root/lib/firmware/bcm70015fw.bin"
	cp /bin/true "$root/usr/lib/libcrystalhd.so.3"
	cp /bin/true "$root/usr/lib/gstreamer-1.0/libgstcrystalhd.so"
	: >"$root/usr/lib/dri/crystalhd_drv_video.so"
	ln -s /dev/null "$root/dev/crystalhd"
	ln -s /dev/null "$root/dev/dri/renderD128"
}

run_check()
{
	check_root=$1
	shift
	set +e
	check_output=$(
		CRYSTALHD_CHECK_ROOT=$check_root \
		MOCK_ROOT=$check_root \
		MOCK_LOG=$mock_log \
		MOCK_BAD_MODULE=${MOCK_BAD_MODULE:-} \
		MOCK_INVALID_MODULE=${MOCK_INVALID_MODULE:-} \
		MOCK_DKMS_STATUS=${MOCK_DKMS_STATUS:-} \
		MOCK_GST_PLUGIN=${MOCK_GST_PLUGIN:-} \
		MOCK_LDD_NOT_FOUND=${MOCK_LDD_NOT_FOUND:-0} \
		MOCK_VA_OPENED=${MOCK_VA_OPENED:-} \
		MOCK_FUSER_OUTPUT=${MOCK_FUSER_OUTPUT:-} \
		MOCK_FUSER_STATUS=${MOCK_FUSER_STATUS:-1} \
		MOCK_PS_COMMAND=${MOCK_PS_COMMAND:-player} \
		PATH=$mock_dir:$PATH \
			sh "$checker" "$@" 2>&1
	)
	check_status=$?
	set -e
}

tree_snapshot()
{
	(
		cd "$1"
		find . -mindepth 1 -printf 'entry %P %y %m %l\n'
		find . -type f -exec sha256sum {} \;
	) | LC_ALL=C sort
}

[ -r "$checker" ] || fail "checker is not readable: $checker"
make_mocks

# A complete fake installation must be discovered without touching its tree.
make_root
chmod -R a-w "$root"
before=$(tree_snapshot "$root")
run_check "$root"
after=$(tree_snapshot "$root")
assert_status 0
assert_contains 'OK: PCI 0000:03:00.0: Broadcom BCM70015'
assert_contains 'OK: Module identity: loaded and selected srcversions match'
assert_contains 'OK: Library: staged ABI-compatible libcrystalhd.so.3 found'
assert_contains 'OK: GStreamer: crystalhddec discovered'
assert_contains 'OK: VA-API: libva initialized CrystalHD on /dev/dri/renderD128'
assert_contains 'SUMMARY:'
assert_contains '0 WARN, 0 FAIL'
[ "$before" = "$after" ] || fail 'the checker changed the inspected tree'
chmod -R u+w "$root"
if grep -E '^(modprobe|insmod|rmmod|depmod|sysctl|tee) ' "$mock_log" >/dev/null; then
	fail 'the checker invoked a mutating system command'
fi

# Development overrides must be reported and excluded from installed discovery.
make_root
GST_PLUGIN_PATH=/source/plugin
GST_PLUGIN_PATH_1_0=/source/plugin-1.0
GST_PLUGIN_SYSTEM_PATH=/unrelated/source-tree
GST_PLUGIN_SYSTEM_PATH_1_0=/another/source-tree
LD_LIBRARY_PATH=/source/library
LIBVA_DRIVERS_PATH=/source/va
PKG_CONFIG_PATH=/source/pkgconfig
PKG_CONFIG_LIBDIR=/source/pkgconfig-libdir
export GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0
export GST_PLUGIN_SYSTEM_PATH GST_PLUGIN_SYSTEM_PATH_1_0
export LD_LIBRARY_PATH LIBVA_DRIVERS_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR
run_check "$root"
assert_status 0
assert_contains 'WARN: GST_PLUGIN_PATH is set'
assert_contains 'WARN: GST_PLUGIN_SYSTEM_PATH is set'
assert_contains 'WARN: GST_PLUGIN_SYSTEM_PATH_1_0 is set'
assert_contains 'WARN: LD_LIBRARY_PATH is set'
assert_contains 'WARN: LIBVA_DRIVERS_PATH is set'
assert_contains 'WARN: PKG_CONFIG_PATH is set'
assert_contains 'WARN: PKG_CONFIG_LIBDIR is set'
unset GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0
unset GST_PLUGIN_SYSTEM_PATH GST_PLUGIN_SYSTEM_PATH_1_0
unset LD_LIBRARY_PATH LIBVA_DRIVERS_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR

# Conflicting direct/DKMS candidates and a selected-module mismatch must be loud.
make_root
bad_module=$root/lib/modules/6.6-test/extra/crystalhd.ko.gz
mkdir -p "${bad_module%/*}"
: >"$bad_module"
MOCK_BAD_MODULE=$bad_module
MOCK_DKMS_STATUS='crystalhd/1.0, 6.6-test, x86_64: installed; Original modules exist'
run_check "$root" --module "$bad_module"
assert_status 1
assert_contains 'WARN: Module: 2 installed candidates can conflict'
assert_contains 'WARN: DKMS: a direct/original module shadows or coexists with DKMS'
assert_contains 'FAIL: Module identity: loaded GOOD000 differs from selected BAD000'
assert_contains 'SUMMARY:'
unset MOCK_BAD_MODULE MOCK_DKMS_STATUS

# An explicit readable non-module must not pass merely because modinfo failed.
make_root
invalid_module=$root/usr/lib/not-a-module
: >"$invalid_module"
MOCK_INVALID_MODULE=$invalid_module
run_check "$root" --module "$invalid_module"
assert_status 1
assert_contains "FAIL: Selected module: $invalid_module is not a valid crystalhd kernel module"
unset MOCK_INVALID_MODULE

# A built source module that modinfo cannot validate must not be called valid.
make_root
source_tree=$test_dir/source-$case_no
source_module=$source_tree/driver/linux/crystalhd.ko
mkdir -p "${source_module%/*}"
: >"$source_module"
MOCK_INVALID_MODULE=$source_module
run_check "$root" --source-tree "$source_tree"
assert_status 0
assert_contains "WARN: Source module exists, but modinfo could not validate it: $source_module"
unset MOCK_INVALID_MODULE

# Discovery from another prefix is reported even when the expected file is absent.
make_root
rm -f -- "$root/usr/lib/gstreamer-1.0/libgstcrystalhd.so"
MOCK_GST_PLUGIN=/opt/stale/libgstcrystalhd.so
run_check "$root"
assert_status 0
assert_contains 'WARN: GStreamer: expected installed plugin is missing'
assert_contains 'discovered /opt/stale/libgstcrystalhd.so'
unset MOCK_GST_PLUGIN

# A missing plugin dependency is a required failure, not a path named "not".
make_root
MOCK_LDD_NOT_FOUND=1
run_check "$root"
assert_status 1
assert_contains 'FAIL: GStreamer: libcrystalhd.so.3 is not resolved'
unset MOCK_LDD_NOT_FOUND

# An obvious owner and an inconclusive fuser error are reported distinctly.
make_root
MOCK_FUSER_OUTPUT=4321
MOCK_FUSER_STATUS=0
MOCK_PS_COMMAND=player
run_check "$root"
assert_status 0
assert_contains 'WARN: Session: /dev/crystalhd owner(s): 4321(player)'
unset MOCK_FUSER_OUTPUT MOCK_FUSER_STATUS MOCK_PS_COMMAND
make_root
MOCK_FUSER_STATUS=2
run_check "$root"
assert_status 0
assert_contains 'INFO: Session: fuser could not confirm an owner (status 2)'
unset MOCK_FUSER_STATUS

# A VA driver opened from another prefix must be reported.
make_root
MOCK_VA_OPENED=/opt/stale/crystalhd_drv_video.so
run_check "$root"
assert_status 0
assert_contains 'WARN: VA-API: libva opened /opt/stale/crystalhd_drv_video.so instead of installed'
unset MOCK_VA_OPENED

# A model-required firmware blob is a required-component failure.
make_root
rm -f -- "$root/lib/firmware/bcm70015fw.bin"
run_check "$root"
assert_status 1
assert_contains 'FAIL: Firmware: required /lib/firmware/bcm70015fw.bin is missing'
assert_contains 'SUMMARY:'
assert_contains '1 FAIL'

# Multiple DRM nodes require an explicit choice; a selected node is then used.
make_root
ln -s /dev/null "$root/dev/dri/renderD129"
run_check "$root"
assert_status 0
assert_contains 'WARN: VA-API: multiple render nodes found; rerun with --drm-device PATH'
assert_contains 'SUMMARY:'
run_check "$root" --drm-device /dev/dri/renderD129
assert_status 0
assert_contains 'OK: VA-API: libva initialized CrystalHD on /dev/dri/renderD129'

if grep -E '^(modprobe|insmod|rmmod|depmod|sysctl|tee) ' "$mock_log" >/dev/null; then
	fail 'the checker invoked a mutating system command'
fi

printf '%s\n' 'crystalhd-check tests: PASS'
