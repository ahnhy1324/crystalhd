# Broadcom Crystal HD for current Linux systems

This fork maintains the BCM70012/BCM70015 kernel driver, firmware and legacy
`libcrystalhd` API for Linux 5.15 and newer. On BCM70015, `crystalhd-play`
(which uses the GStreamer `crystalhddec` plugin) is the recommended local-file
entry point; explicit GStreamer pipelines are the integration path. BCM70012
support is retained but has not been recently tested on hardware.

Measured capabilities and adoption limits are summarized below. Exact profiles,
fixtures and remaining work are in the
[hardware report](HARDWARE-2026-09-13.md) and [open work](TODO.md). Successful
compilation or driver discovery is not proof that a particular stream decodes.

## Current status

BCM70015 local-file playback is the usable path today. Start with every tuning
option unset. CrystalHD decodes video only, while the normal Linux audio,
subtitle and display components handle the rest.

| Target | Status | What is established |
| --- | --- | --- |
| BCM70015 720p30 local playback | **Validated** | An 8-hour clocked H.264 run and counted control, EOS and clean-reopen tests passed on the reference system. |
| BCM70015 1080p30 local playback | **Conditionally validated** | With its host-specific PCIe L0s workaround, the reference system completed a 2-hour 1080p30 run; a separate throughput run reached 41.20 fps. Test the default first. |
| 720p60 | **Experimental** | A synthetic no-B H.264 decode/download test exceeded 60 fps with the reference host's L0s workaround active, but a complete 60 Hz presentation path is not validated. |
| 1080p60 | **Not practical** | Under the same synthetic capacity configuration, output remained about 42 fps and live input accumulated backlog. |
| VA-API and VLC | **Experimental** | Selected FFmpeg and one VLC H.264/X11 path passed; this is not general client or codec certification. |
| Chromium/YouTube and BCM70012 | **Unverified** | Browser hardware playback is not a supported daily-use path; BCM70012 has no recent hardware retest. |

The validated BCM70015 subset includes H.264, MPEG-2, MPEG-4 Part 2, VC-1
Advanced and WMV3 within the documented profile and framing limits. Maximum
coded size is 1920x1088, only one hardware playback session may be active, and
HEVC, VP8, VP9 and AV1 are not supported.

See [usage and runtime options](USAGE.md) for choosing a frontend, proving
that hardware decoding is active, codec-specific examples and safe option
rollback. Exact fixtures, measurements and qualifications remain in the
[hardware report](HARDWARE-2026-09-13.md); current work is tracked in
[TODO](TODO.md).

## Quick install

On Ubuntu, install the ordinary build and playback dependencies:

```sh
sudo apt install build-essential pkg-config linux-headers-$(uname -r) \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav ffmpeg \
  python3-gi gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 \
  libva-dev libdrm-dev libgbm-dev libswscale-dev vainfo \
  pciutils psmisc
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
Optional browser launcher assets use `sudo make install-browser`; hardware
browser decoding remains experimental and opt-in.

## Verify

Run the read-only preflight first:

```sh
crystalhd-check
gst-inspect-1.0 crystalhddec
```

The preflight reports the PCI device, loaded and installed module identities,
direct/DKMS conflicts, firmware, device access and owner, loader-selected
library, GStreamer plugin, VA driver and DRM render nodes. It does not load or
unload a module, open a decoder session, alter PCIe settings or require raw
register access.

After rebuilding, compare the checkout as well:

```sh
crystalhd-check --source-tree "$PWD"
```

The card permits one playback session at a time. The installed udev rule gives
the active desktop user access and otherwise uses `root:video` mode `0660`. On
a headless system, add the playback account to `video` and log in again. For
module identity checks and safe reload instructions, see the
[bring-up guide](BRINGUP.md#device-access-and-module-identity). Never
force-unload an active device.

## Play a local file

The installed controller selects CrystalHD explicitly and does not hide a
decoder failure behind software fallback:

```sh
crystalhd-play video.mp4
```

Unsupported codec features, a busy card or insufficient device access fail
visibly. The [usage guide](USAGE.md#play-a-local-file) documents controls,
subtitles, deliberate software comparison and codec-specific GStreamer
pipelines.

## Optional VA-API

The optional VA-API backend is experimental and can be slow for synchronous
one-picture-at-a-time clients. See the [installed FFmpeg/VLC recipes and
option boundaries](USAGE.md#va-api-and-external-clients) and the component's
[implemented subset](filters/vaapi/README.md). Driver discovery alone is not
proof of compressed-video decoding.

## 32-bit and legacy CPUs

On a native 32-bit i686 system with SSE2, use the ordinary build with matching
32-bit dependencies. A multilib compiler, dependencies and installation path
must all select the same ABI; a 32-bit library cannot load into a 64-bit player.

`LEGACY_CPU=1` is for 32-bit i686 processors that lack SSE2. It also disables
SSE and MMX, selects a scalar x87 build and requires matching 32-bit
dependencies:

```sh
make LEGACY_CPU=1
sudo make LEGACY_CPU=1 install
```

Normal builds keep the SSE2 baseline. `make userspace32-check` and
`make legacy-cpu-check` exercise the two build modes, but do not configure a
mixed-ABI installation or certify performance on a physical legacy processor.

## DKMS

DKMS owns only the kernel module. Register and install it with the procedure in
[README.dkms](README.dkms), then install the separate runtime:

```sh
sudo apt install dkms linux-headers-$(uname -r)
sudo make install-runtime
```

Do not run the direct `install-module` target over a DKMS-managed kernel. Use
`crystalhd-check` after updates to detect direct/DKMS conflicts and stale loaded
modules.

## Troubleshooting

Start with `crystalhd-check`; then follow the milestone-based
[bring-up guide](BRINGUP.md). It separates PCI binding, firmware startup,
decoder setup, input framing, output and frontend synchronization instead of
inferring a driver failure from a late playback symptom.

On the tested BCM70015/ICH8 link, disabling PCIe L0s removed a measured Full HD
throughput bottleneck. This is an opt-in, host-specific diagnostic that can
increase power use, not a default. Follow the [safe test and rollback
procedure](USAGE.md#full-hd-pcie-workaround); never force-unload an active
device or apply a global PCIe workaround.

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

That target removes only the installed launcher, extension assets and desktop
file; it does not delete profiles, policies or defaults. Remove a DKMS
registration separately with the procedure in [README.dkms](README.dkms).

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
against maintained LTS, stable and mainline kernel APIs. Linux 5.15 coverage
is an API compilation check, not BCM70012 or BCM70015 hardware certification;
device-free tests do not establish hardware playback.

The opt-in [raw-frame library API](linux_lib/libcrystalhd/libcrystalhd_raw_frame.h)
uploads 256x96 YUV420P images, composes tiles from two retained references,
and applies bounded quarter-pixel translation through native H.264. It requires
a fresh, correctly configured decoder session and caller-managed stream rate;
it is not standalone MFD processing or a raw device-surface API.
Non-reference composition and translation leave both source slots unchanged;
the fixed POC2 profile permits at most one non-reference AU between committed
reference AUs.
`make raw-frame-check` tests the host API.

## Licensing

Existing file notices remain authoritative in this mixed-license codebase;
see [LICENSES.md](LICENSES.md). [HISTORY.md](HISTORY.md) records project lineage
and maintenance milestones, not the current task list.
