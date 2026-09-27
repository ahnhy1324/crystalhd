# Broadcom Crystal HD for current Linux systems

This fork maintains the BCM70012/BCM70015 kernel driver, firmware and legacy
`libcrystalhd` API for Linux 6.1 and newer. GStreamer on BCM70015 is the
recommended local-playback path. BCM70012 support is retained but has not been
recently tested on hardware.

The validated codec subset includes H.264, MPEG-2, MPEG-4 Part 2, VC-1 and
WMV3. Exact profiles, fixtures and remaining limits are in the
[hardware report](HARDWARE-2026-09-13.md) and [open work](TODO.md). Successful
compilation or driver discovery is not proof that a particular stream decodes.

## Quick install

On Ubuntu, install the ordinary build and playback dependencies:

```sh
sudo apt install build-essential pkg-config linux-headers-$(uname -r) \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav \
  python3-gi gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 \
  libva-dev libdrm-dev libgbm-dev libswscale-dev vainfo
```

Build and install for the running kernel and native userspace:

```sh
make -j"$(nproc)"
sudo make install
sudo modprobe crystalhd
```

`make install` installs the current-kernel module, udev rule, firmware,
`libcrystalhd`, headers, GStreamer plugin/player, VA-API driver and
`crystalhd-check`. It does not configure a browser, enable the optional PCIe
workaround, replace an already loaded module, or install a DKMS registration.

## Verify

Run the read-only preflight first:

```sh
crystalhd-check
gst-inspect-1.0 crystalhddec
```

The preflight reports the PCI device, loaded and installed module identities,
direct/DKMS conflicts, firmware, device access and owner, loader-selected
library, actual GStreamer plugin filename, VA driver and all DRM render nodes.
`OK`, `WARN`, `FAIL` and `INFO` lines can be copied into a bug report. It does
not load or unload a module, open a decoder session, alter PCIe settings or
require raw-register access.

After rebuilding, compare the checkout as well:

```sh
crystalhd-check --source-tree "$PWD"
```

`modprobe` does not replace a module already in memory. If the loaded and
selected source versions differ, close every CrystalHD client, verify that
`/dev/crystalhd` is idle, and reload normally. Never force-unload an active
device; reboot if a normal unload reports that it is busy.

The card permits one playback session at a time. The installed udev rule gives
the active desktop user access and otherwise uses `root:video` mode `0660`. On
a headless system, add the playback account to `video` and log in again.

## Play a local file

The installed controller selects CrystalHD explicitly and does not hide a
decoder failure behind software fallback:

```sh
crystalhd-play video.mp4
```

Space pauses; Left/Right or `j`/`l` seek ten seconds; `1`, `2` and `3` select
0.5x, 1x and 2x; `q` quits. Use `crystalhd-play --software video.mp4` only when
you intentionally want a software reference. Unsupported codec features, a
busy card or insufficient device access fail visibly.

The [GStreamer guide](filters/gst/gst-plugin-1.0/README.md) documents accepted
framing and counted hardware tests. Available controls do not certify every
resolution, rate, display path or physical A/V synchronization.

## Optional VA-API

`crystalhd-check` lists the available `/dev/dri/renderD*` nodes. If there is
more than one, choose the display GPU's node explicitly; do not assume that it
is always `renderD128`. Then verify installed-driver discovery:

```sh
drm_node=/dev/dri/renderDXXX
LIBVA_DRIVER_NAME=crystalhd \
  vainfo --display drm --device "$drm_node"
```

To decode and download a supported file with FFmpeg:

```sh
LIBVA_DRIVER_NAME=crystalhd \
ffmpeg -hwaccel vaapi -hwaccel_device "$drm_node" \
  -hwaccel_output_format vaapi -i video.mp4 \
  -vf hwdownload,format=nv12 -f null -
```

`vainfo` proves that libva found and initialized the installed driver; it does
not submit a compressed stream or prove hardware decode. The VA backend is
experimental and can be slow for synchronous one-picture-at-a-time clients.
See its [supported profiles and replay limits](filters/vaapi/README.md).

## 32-bit and legacy CPUs

On a native 32-bit i686 system with SSE2, use the ordinary build after
installing matching 32-bit dependencies. On a 64-bit multilib system, the
compiler, pkg-config paths, dependencies and installation directory must all
select the same 32-bit ABI. A 32-bit `libcrystalhd` cannot load into a 64-bit
player, or vice versa; `crystalhd-check` reports the selected library class.

`LEGACY_CPU=1` is only for 32-bit i686 processors without SSE/SSE2 or MMX. It
selects a scalar x87 build and requires matching 32-bit dependencies:

```sh
make LEGACY_CPU=1
```

Normal builds keep the accelerated SSE2 baseline. Changing `LEGACY_CPU`
rebuilds affected outputs automatically. `make userspace32-check` and
`make legacy-cpu-check` exercise isolated native-32 and no-SSE builds; they do
not make mixed-ABI installation automatic or certify performance on a physical
legacy processor.

## DKMS

DKMS owns only the kernel module. Firmware, the udev rule, libraries and
plugins remain a separate installation:

```sh
sudo apt install dkms linux-headers-$(uname -r)
# Register/build/install the module as described in README.dkms, then:
sudo make install-runtime
```

Do not run the direct `install-module` target over a DKMS-managed kernel. After
an update, use `crystalhd-check` to confirm which module file will load and
whether its source version matches the loaded module. Registration, safe
same-version rebuilds and removal are in [README.dkms](README.dkms).

## Troubleshooting

Start with `crystalhd-check`; then follow the milestone-based
[bring-up guide](BRINGUP.md). It separates PCI binding, firmware startup,
decoder setup, input framing, output and frontend synchronization instead of
inferring a driver failure from a late playback symptom.

On the tested BCM70015/ICH8 link, disabling PCIe L0s removed a measured Full HD
throughput bottleneck. This is an opt-in diagnostic, not the default:

```sh
sudo fuser -v /dev/crystalhd
sudo modprobe -r crystalhd
sudo modprobe crystalhd force_l0s_off=1
```

Proceed only when `fuser` shows no owner; never force the unload. The option
applies only to a
BCM70015 on a dedicated root-port link, can increase power use, and does not
certify display timing or audible lip-sync. Do not use global
`pcie_aspm=force` or change link speed/payload settings. See the
[recorded measurements](HARDWARE-2026-09-13.md#pcie-l0s-isolation-2026-09-27).

Chrome hardware decoding and PowerVLC integration remain experimental and are
tracked separately in issues
[#12](https://github.com/ahnhy1324/crystalhd/issues/12) and
[#18](https://github.com/ahnhy1324/crystalhd/issues/18). The core install does
not change browser/player defaults. Optional browser launcher assets use
`sudo make install-browser`; the checkout-only default-browser setup is not
installed as a system command.

## Uninstall

Remove only the files from the ordinary core installation:

```sh
sudo make uninstall
```

This removes the configured direct module file and runtime files, then updates
the module and dynamic-loader caches. It never unloads the running module and
does not touch a DKMS module. If CrystalHD is loaded, it remains in memory until
a safe normal unload or reboot.

Browser launcher assets are separately scoped:

```sh
sudo make uninstall-browser
```

That target removes only the launcher, bundled extension assets and desktop
file installed by `make install-browser`. It does not delete browser profiles,
unrelated policies or defaults. Remove a DKMS registration separately with the
procedure in [README.dkms](README.dkms).
State created by the checkout-only `setup-crystalhd-chrome-default` script
(profile/default and managed Chrome registration) is outside this target and
must be reviewed separately before removing it.

## Developer and source-tree testing

Optional regression dependencies include `gcc-multilib`, `g++-multilib`,
`qemu-user`, `ffmpeg`, its development libraries, `nodejs` and `node-ws`.
Run the device-free suite with:

```sh
make check
make userspace32-check
make legacy-cpu-check
```

The kernel and userspace builds use `-Werror`. CI also compiles the driver
against maintained LTS, stable and mainline kernel APIs. Device-free tests do
not establish hardware playback.

`tests/staged-install.sh` installs into a temporary `DESTDIR`, verifies the
complete layout and SONAME links, then exercises idempotent removal without
touching the host. Source-tree tests may deliberately use `LD_LIBRARY_PATH`,
`GST_PLUGIN_PATH`, a private GStreamer registry or `LIBVA_DRIVERS_PATH`; an
installed-path test must not. The component guides keep those development
commands separate from normal installed discovery.

## Licensing

Existing file notices remain authoritative in this mixed-license codebase;
see [LICENSES.md](LICENSES.md). [HISTORY.md](HISTORY.md) records original driver
releases, not the current task list.
