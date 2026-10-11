# Broadcom Crystal HD for current Linux systems

This fork maintains the BCM70012/BCM70015 kernel driver, firmware and legacy
`libcrystalhd.so.3` API for Linux 5.15 and newer. Its best-established use is
hardware video decoding for local files on BCM70015. CrystalHD handles video;
the normal Linux media stack handles audio, subtitles, synchronization and
display.

## Compatibility at a glance

“Tested” below means the named reference configuration and documented stream
subset, not every card, host or file.

### Hardware and platform

| Platform | Current status |
| --- | --- |
| BCM70015 | Primary maintained device. Playback, codec and lifecycle tests are recorded on Ubuntu 25.10 / Linux 6.17. |
| BCM70012 | Legacy H.264 and `libcrystalhd.so.3` compatibility are retained, but no recent physical-card retest has been completed. |
| Linux kernel | Driver API builds are maintained for Linux 5.15 and newer. Compile coverage is not hardware certification. |
| Userspace | Native x86-64 and i686 paths are maintained. `LEGACY_CPU=1` builds scalar i686 userspace for CPUs without SSE2; physical no-SSE performance is unverified. |

One CrystalHD hardware decode session can be active at a time. Maximum coded
size is 1920x1088.

### Video formats and rates

| Media on BCM70015 | Tested scope |
| --- | --- |
| H.264/AVC | Selected Constrained Baseline, Main and High streams, including selected MBAFF streams |
| MPEG-2 | Selected progressive Simple/Main and field-coded Main streams |
| MPEG-4 Part 2 | Selected progressive Simple and Advanced Simple streams |
| VC-1 / WMV3 | Selected progressive VC-1 Advanced and standard WMV3 Simple/Main streams |
| HEVC, VP8, VP9, AV1 | Not supported |

| Playback target | Practical status |
| --- | --- |
| 720p30 local files | Best-established ordinary playback target |
| 1080p30 local files | Works on the reference host with a host-specific PCIe workaround; test the default first |
| 720p60 | Decoder capacity has been demonstrated, but general 60 Hz presentation has not |
| 1080p60 | Not practical on the current output path |

Exact profiles, framing restrictions, fixtures and measurements are in the
[usage guide](USAGE.md) and [hardware report](HARDWARE-2026-09-13.md).

### Applications and APIs

| Application or API | Status | Practical meaning |
| --- | --- | --- |
| `crystalhd-play` / GStreamer `crystalhddec` | **Recommended** | Local files with audio, subtitles and controls; explicit hardware selection and no silent software fallback |
| `libcrystalhd.so.3` | **Compatibility API** | Existing Broadcom-API applications; the ABI and legacy ioctl behavior remain preserved |
| FFmpeg through VA-API | **Experimental** | Selected decode/download paths work; libva discovery alone is not proof of decode or display |
| VLC 3.0.21 | **Tested combination** | H.264 on the reference X11/i915 system only, not general VLC or codec certification |
| [Moonlight Qt 6.2.0](MOONLIGHT.md) | **Experimental integration** | Stock Moonlight has performed real BCM70015 H.264 decode and indirect EGL/DRM PRIME display; presentation cadence and complete A/V/input/reconnect acceptance remain below support grade |
| Chromium / YouTube | **Hardware path unverified** | The launcher defaults to software; hardware mode disables the GPU sandbox and is not a daily-use path |

## Quick start

Before choosing a direct install, check whether DKMS already owns CrystalHD:

```sh
dkms status 2>/dev/null | grep -i crystalhd || true
```

If that prints a CrystalHD registration, follow the [DKMS procedure](README.dkms)
and do not run the direct `sudo make install` below. Otherwise, from a fresh
checkout on Ubuntu:

```sh
sudo apt install git
git clone https://github.com/ahnhy1324/crystalhd.git
cd crystalhd

sudo apt install build-essential pkg-config linux-headers-$(uname -r) \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav ffmpeg \
  python3-gi gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 \
  libva-dev libdrm-dev libgbm-dev libswscale-dev vainfo \
  pciutils psmisc

make -j"$(nproc)"
sudo make install
sudo modprobe crystalhd
crystalhd-check
crystalhd-play --hardware video.mp4
```

When updating an existing direct install, stop before playback if
`crystalhd-check` reports that the loaded and installed modules differ. Follow
the [idle reload or reboot procedure](BRINGUP.md#device-access-and-module-identity)
so the check and player use the same driver image.

A successful hardware run reports
`Decoder: crystalhddec (hardware; video output confirmed)` and a nonzero
decoded-video-frame count. Unsupported input, a busy device or missing access
fails visibly instead of falling back to software.

## Common use

```sh
# External subtitles
crystalhd-play --subtitles subtitles.srt video.mkv

# Deliberate software comparison; never an automatic fallback
crystalhd-play --software video.mp4

# Explicit installed GStreamer H.264 pipeline
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! videoconvert ! autovideosink
```

Space pauses; Left/Right or `j`/`l` seek ten seconds; `1`, `2` and `3` select
0.5x, 1x and 2x; `q` quits. See [everyday usage](USAGE.md) for all supported
file examples, codec boundaries, VA/VLC commands, proof of hardware use and
troubleshooting.

Start with every tuning variable unset. The Full-HD PCIe workaround is only
for a repeatable BCM70015 throughput shortfall on a suitable dedicated link;
follow the [safe test and rollback procedure](USAGE.md#full-hd-pcie-workaround)
instead of applying global PCIe settings.

## Sunshine / Moonlight streaming

720p30 is the first hardware-offload trial. The reference system performed
real CrystalHD decoding and indirect display, but smooth 60 fps presentation,
measured end-to-end latency, physical A/V synchronization and reconnect have
not passed a user-support gate. Use software decoding when dependable game
streaming is the priority.

After installing the stack, launch the Moonlight AppImage with the tested
process-local controls:

```sh
moonlight_app=/path/to/Moonlight.AppImage
chmod +x "$moonlight_app"
env LIBVA_DRIVER_NAME=crystalhd \
  VAAPI_FORCE_INDIRECT=1 \
  CRYSTALHD_VAAPI_LIVE_H264=1 \
  CRYSTALHD_VAAPI_LOW_LATENCY_H264=1 \
  "$moonlight_app"
```

Use H.264, 1280x720 and 30 fps for the first connection. The
[Moonlight/Sunshine guide](MOONLIGHT.md) covers download, pairing, proof of
hardware use, measured limits and rollback-safe troubleshooting.

## Other installation modes

- DKMS owns only the kernel module; follow [README.dkms](README.dkms) and use
  `sudo make install-runtime` for userspace.
- Native 32-bit i686 needs a matching 32-bit compiler, dependencies and
  installation path; a 32-bit library cannot load into a 64-bit player. On a
  CPU without SSE2, additionally pass `LEGACY_CPU=1` to both the build and
  install: `make LEGACY_CPU=1` then `sudo make LEGACY_CPU=1 install`.
- `sudo make uninstall` removes the direct installation without force-unloading
  a running module or deleting a DKMS registration.

## Documentation

| Need | Document |
| --- | --- |
| Local playback, controls, codec examples, VA/VLC and common troubleshooting | [USAGE.md](USAGE.md) |
| Sunshine/Moonlight hardware streaming | [MOONLIGHT.md](MOONLIGHT.md) |
| PCI binding, firmware startup, identity and staged diagnosis | [BRINGUP.md](BRINGUP.md) |
| DKMS ownership and updates | [README.dkms](README.dkms) |
| Exact hardware fixtures, benchmarks and evidence boundaries | [HARDWARE-2026-09-13.md](HARDWARE-2026-09-13.md) |
| Current work | [TODO.md](TODO.md) |
| GStreamer framing and counted tests | [GStreamer component guide](filters/gst/gst-plugin-1.0/README.md) |
| VA-API subset, replay and experimental controls | [VA-API component guide](filters/vaapi/README.md) |
| Legacy API and library options | [libcrystalhd guide](linux_lib/libcrystalhd/README.md) |

## Developer checks

The kernel and userspace builds use `-Werror`. Device-free checks do not prove
hardware playback:

```sh
make check
make userspace32-check
make legacy-cpu-check
```

CI also compiles the driver against maintained LTS, stable and mainline kernel
APIs. [HISTORY.md](HISTORY.md) records project lineage and completed
maintenance milestones rather than the current task list.

## Licensing

Existing file notices remain authoritative in this mixed-license codebase; see
[LICENSES.md](LICENSES.md).
