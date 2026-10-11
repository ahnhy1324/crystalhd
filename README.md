# Broadcom Crystal HD for current Linux systems

This fork maintains the BCM70012/BCM70015 kernel driver, firmware and legacy
`libcrystalhd` API for Linux 5.15 and newer. GStreamer on BCM70015 is the
recommended local-playback path. BCM70012 support is retained but has not been
recently tested on hardware.

Measured capabilities and adoption limits are summarized below. Exact profiles,
fixtures and remaining work are in the
[hardware report](HARDWARE-2026-09-13.md) and [open work](TODO.md). Successful
compilation or driver discovery is not proof that a particular stream decodes.

## Is it useful today?

For a BCM70015, yes for local playback of the tested codec subset, especially
30-fps material. `crystalhd-play` and GStreamer are the recommended paths. This
is not a general replacement for a modern GPU decoder: VA-API remains a limited
client backend, browser hardware decoding is not validated, and BCM70012 has
not been recently retested on hardware. The stack permits one active hardware
session, has a maximum coded size of 1920x1088 and does not decode HEVC, VP8,
VP9 or AV1.

The following results come from the reference BCM70015 on an Intel ICH8 PCIe
Gen1 x1 link, running Ubuntu 25.10 and Linux 6.17. They describe measured
behavior on that host rather than a performance guarantee for every card,
mainboard or input file.

| Intended use | Measured result | Current judgement |
| --- | --- | --- |
| Local 720p30 playback | H.264/AAC completed an 8-hour clocked run with 864,000/864,000 pictures and EOS. Ten control cycles passed pause/resume, forward/backward seek, 0.5x, 1x and 2x, followed by a clean reopen. | Usable on BCM70015 within the tested codec/profile boundaries. |
| Local 1080p30 playback | The default PCIe link state was below 30 fps on the reference host. With the opt-in `force_l0s_off=1` workaround, the same card reached 41.20 fps and completed a 2-hour 216,000-picture H.264 run with EOS. | Usable on the tested host after confirming that the [documented L0s workaround](#troubleshooting) is needed; Full HD 2x is not supported. |
| Codec compatibility | Selected H.264 Constrained Baseline/Main/High, MPEG-2, MPEG-4 Part 2 Simple/Advanced Simple, VC-1 Advanced and WMV3 Simple/Main fixtures from SD through FHD, depending on the codec, complete through the paths listed in the hardware report. | Suitable for known-compatible files, not arbitrary-stream conformance. |
| Stock VLC | VLC 3.0.21 completed one 120-second H.264 DRM/EGL/X11 playback with perceptual dialogue sync, pause/resume, seeks, EOS and clean release. | Useful as a focused compatibility path, not general VLC or multi-codec certification. |
| Decoder CPU and memory trade-off | Across strict and live+prefeed synthetic 360p-to-1080p measurements, CrystalHD used about 61-84% process CPU and 159-230 MiB peak RSS; software used about 161-181% CPU and 58-76 MiB. Here 100% CPU is one logical processor, and software was substantially faster. | CrystalHD provides decoder-process CPU offload for legacy hosts, at the cost of speed and memory; lower whole-system use is not proven. |
| VA-API and game streaming | With the reference host's L0s workaround active, local H.264 decode/download measured 82.25 fps at 1280x720 and 41.54 fps at 1920x1080. In Moonlight the card produced about 60 fps at 720p, but the live+prefeed exact-identity client path submitted about 33 fps; a faster non-standard remap remains a diagnostic opt-in rather than a supported presentation path. | Experimental: useful for integration, but not a 720p60 presentation claim. |
| 1080p60 | Full-HD local output capacity remained about 42 fps and a live 1080p60 stream accumulated substantial backlog. | Not a practical target for the current PCIe output path. |
| Chromium/YouTube | Local Chrome software playback controls pass. Hardware decode, live YouTube A/V sync and post-seek frame identity are not validated. | Do not choose this project for browser hardware acceleration today. |
| BCM70012 | The legacy H.264 path and ABI are retained, but current hardware results are for BCM70015. | Treat as unverified until tested on actual BCM70012 hardware. |

The frozen Phase 1 acceptance also completed 1,000 open/decode/EOS/close
sessions and a 500-session, five-codec round robin without a stale frame or
hash change. The current event-driven receive path subsequently reduced paced
task time by 6.3% at 720p60 and 10.3% at 1080p30, and voluntary context switches
by 52.4% and 61.7%, compared with the old 1 ms polling path. It passed repeated
B-frame/EOS exactness, one-core/frequency-cap and clean-release checks; the
change improves transport overhead rather than codec or resolution support.

The 8-hour and 2-hour figures remain frozen Phase 1 results: later changes are
regression-tested against that behavior but do not repeat every multi-hour run
after each commit. Reported Moonlight component timing is not camera-measured
input-to-photon latency. Physical power consumption and a numerical A/V-sync
tolerance have not been measured. See the
[result matrix](HARDWARE-2026-09-13.md#current-result-matrix),
[lifecycle evidence](HARDWARE-2026-09-13.md#frontend-and-lifecycle-evidence)
and [current performance screening](HARDWARE-2026-09-13.md#event-driven-receive-and-bounded-memorywrite-screening-2026-10-11)
before deciding whether the documented limits fit a particular system.

## Quick install

On Ubuntu, install the ordinary build and playback dependencies:

```sh
sudo apt install build-essential pkg-config linux-headers-$(uname -r) \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav ffmpeg \
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
See its [supported profiles and replay limits](filters/vaapi/README.md). The
[reference-host performance baseline](HARDWARE-2026-09-13.md#va-api-decode-and-game-stream-baseline-2026-10-10)
separates card output capacity from client presentation; it is not a general
real-time playback claim.

## 32-bit and legacy CPUs

On a native 32-bit i686 system with SSE2, use the ordinary build with matching
32-bit dependencies. A multilib compiler, dependencies and installation path
must all select the same ABI; a 32-bit library cannot load into a 64-bit player.

`LEGACY_CPU=1` is for 32-bit i686 processors that lack SSE2. It also disables
SSE and MMX, selects a scalar x87 build and requires matching 32-bit
dependencies:

```sh
make LEGACY_CPU=1
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
throughput bottleneck. This is an opt-in diagnostic, not the default:

```sh
sudo fuser -v /dev/crystalhd
sudo modprobe -r crystalhd
sudo modprobe crystalhd force_l0s_off=1
```

Proceed only when `fuser` shows no owner; never force the unload. The option is
for BCM70015 on a dedicated root-port link, can increase power use, and does
not certify display timing or audible lip-sync. Do not use global
`pcie_aspm=force` or change link speed or payload settings. See the
[recorded measurements](HARDWARE-2026-09-13.md#pcie-l0s-isolation-2026-09-27).

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
