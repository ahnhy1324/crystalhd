# Broadcom Crystal HD for current Linux systems

This fork maintains the BCM70012/BCM70015 kernel driver, firmware and legacy
`libcrystalhd.so.3` API for Linux 5.15 and newer. Its best-established use is
hardware video decoding for local files on BCM70015. CrystalHD handles video;
the normal Linux media stack handles audio, subtitles, synchronization and
display.

**[CrystalHD 1.2.0 is available](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0)**
with module-DKMS `3.10.13`. The quick start installs that stable release. See the
[final-package qualification](HARDWARE-2026-09-13.md#120-final-package-qualification--2026-10-23)
and [change summary](HISTORY.md#120).
Current master is development `1.3.0-dev.1` / `3.10.14`, not a new qualified
release; its follow-up experiments do not extend stable compatibility claims.
For published [v1.1.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0),
use the instructions shipped with that tag. The
[Phase 1 baseline](https://github.com/ahnhy1324/crystalhd/releases/tag/phase1-baseline-2026-09-29)
remains available and unchanged.

The immutable [v1.2.0-rc.1 prerelease](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0-rc.1)
retains module-DKMS `3.10.11`; the published
[v1.2.0-rc.2 prerelease](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0-rc.2)
retains `3.10.12`. RC2 completed the bounded G0–G3 qualification tracked in
[#473](https://github.com/ahnhy1324/crystalhd/issues/473). Keep RC2 and stable
1.1.0 source/cached modules for [rollback](BRINGUP.md#direct-install-update-and-rollback).
The 1.2 scope is selected CPU VA-VPP Bob and explicit H.264 card-output scaling;
Kodi, sustained playback and package results are tracked in the
[qualification matrix](HARDWARE-2026-09-13.md#120-rc2-qualification--2026-10-22).
Native hardware deinterlacing, general FHD50/60 and Atom real-time playback are
not claimed. [Current limits and usage](USAGE.md#known-issues-and-deinterlacing-roadmap)
separate Weave, application-side filtering and unqualified paths.

## Compatibility

| What you have or want | What to expect |
| --- | --- |
| BCM70015 local files | Recommended: `crystalhd-play` / GStreamer with audio, subtitles and controls. 720p30 is well tested; native 1080p30 works on the reference host with its qualified PCIe workaround. |
| H.264, MPEG-2, MPEG-4 Part 2, VC-1 / WMV3 | Selected profiles work; see the [codec boundaries](USAGE.md#codec-boundaries). HEVC, VP8, VP9 and AV1 are unsupported. |
| Optional development MPEG-1 experiment | Experimental MPEG-1 decoding via the MPEG-2 algorithm-1 path, natural-content hardware qualified on BCM70015 at progressive352x288/25. Select the separately built rank-NONE `crystalhdmpeg1experimentaldec` explicitly. This unreleased experiment is distinct from official/vendor support, generalized conformance and the ordinary decoder. See [setup and limitations](filters/gst/gst-plugin-1.0/MPEG1-EXPERIMENTAL.md). |
| Full-HD input with reduced card output | Optional hardware scaling reduces transfer traffic. Selected H.264 input reached about 65 pictures/s with the qualified L0s workaround active; software enlargement costs CPU and cannot recover detail. This is not native 1080p60 or a displayed-rate guarantee. |
| Sunshine / Moonlight | Default frame-matching mode works at 720p30; with V-sync/pacing off, an experimental previous-frame mode achieved about 59 renderer submissions/s at 720p60 and 29 at 1080p30. These are measured rates, not a matched speedup over default matching. Physical streaming A/V offset and forced-loss recovery remain unverified. |
| BCM70012 / older CPUs | Legacy BCM70012 compatibility and x86-64/i686 builds are retained. Recent BCM70012 and physical no-SSE hardware tests remain outstanding. |
| Kodi | Stock Kodi 21.3 progressive playback and selected 720i25 CPU Bob/Weave pass lifecycle and qualitative A/V checks on the reference host. Auto may omit second-field requests; uniform 50p, general FHD Auto and hardware Bob are not claimed. See [usage and limits](USAGE.md#kodi). |
| VA-API, VLC, browsers | VA-API is experimental. VLC 3.0.23 selects CrystalHD VA/EGL and closes cleanly on the LTS reference host, but logs late pictures. Chromium/YouTube hardware support is unverified. |

Fresh BCM70015 qualification uses Ubuntu 26.04.1 LTS / Linux 7.0.0-38.
Earlier Ubuntu 25.10 / Linux 6.17 results, including VLC 3.0.21, remain
historical; see the [dated hardware report](HARDWARE-2026-09-13.md#new-lts-qualification--2026-10-13).
The driver builds against Linux 5.15 and newer; other hosts still need testing.
One hardware session can run at a time, with coded size up to 1920x1088.

Start here: [play a local file](#quick-start), [try Moonlight](MOONLIGHT.md), or
[update/troubleshoot an existing installation](BRINGUP.md).

## Quick start

The direct route installs for the running kernel and requires rebuilding after
a kernel update. [DKMS](README.dkms) handles those later module rebuilds
automatically. Choose one route; if DKMS already manages CrystalHD, follow its
guide instead of this direct-install procedure.

For a fresh stable 1.2.0 checkout on Ubuntu:

```sh
sudo apt install git
git clone --branch v1.2.0 https://github.com/ahnhy1324/crystalhd.git
cd crystalhd

sudo apt install build-essential pkg-config linux-headers-$(uname -r) \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav ffmpeg \
  python3-gi gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 \
  libva-dev libdrm-dev libgbm-dev libswscale-dev vainfo \
  pciutils psmisc

make -j2
sudo make install
sudo modprobe crystalhd
crystalhd-check
```

Use `make -j1` if a parallel build runs out of memory. For a published source
archive, extract it and enter its directory in place of the `git clone` and
`cd` steps; building and installing do not require `.git`. To try the older
fixed baseline, clone with `--branch phase1-baseline-2026-09-29` and use the
guide shipped with that version.

The direct target installs the current-kernel module, firmware, udev rule,
library, GStreamer plugin/player, VA-API driver and `crystalhd-check`.
After booting a new kernel, install its matching headers and repeat `make`
and `sudo make install`. Returning users should follow
[update and rollback](BRINGUP.md#direct-install-update-and-rollback) before
replacing an installation. If the check reports an old loaded module, follow
[device access and module identity](BRINGUP.md#device-access-and-module-identity).

After `crystalhd-check` passes without an identity or device-access error,
play a supported file. Start with a progressive H.264 8-bit 4:2:0 video at
1280x720 and 30 fps or less, with AAC audio in MP4. Replace the quoted path
below with your file; the `.mp4` suffix alone does not identify its codec.

```sh
crystalhd-play --hardware "/path/to/video.mp4"
```

Success reports
`Decoder: crystalhddec (hardware; video output confirmed)` and a nonzero
decoded-video-frame count. Unsupported input, a busy device or missing access
fails visibly instead of falling back to software. If the check or playback
fails, start with the [symptom table](BRINGUP.md#first-checks).

## Common use

Replace the quoted example paths with your files.

```sh
# External subtitles
crystalhd-play --subtitles "/path/to/subtitles.srt" "/path/to/video.mkv"

# Deliberate software comparison
crystalhd-play --software "/path/to/video.mp4"

# Reduce card-to-host traffic for progressive BCM70015 input
crystalhd-play --hardware --scale-width 1280 "/path/to/video-1080p.mp4"

# Optional software enlargement of that reduced output to Full HD
crystalhd-play --hardware --scale-width 1280 \
  --software-output-size 1920x1080 "/path/to/video-1080p.mp4"
```

For ordinary display playback, prefer `--scale-width` by itself: the video
sink fits the smaller decoded image to its window. Add
`--software-output-size` only when the caller needs an exact-size raw buffer;
host enlargement costs CPU and cannot restore detail discarded by the card.

Space pauses; Left/Right or `j`/`l` seek ten seconds; `1`, `2` and `3` select
0.5x, 1x and 2x; `q` quits. See [everyday usage](USAGE.md) for codec examples,
scaling choices and tradeoffs, VA/VLC commands and troubleshooting. The width
above is a measured example, not a hardcoded or automatic default.

Start with tuning variables unset. For a repeatable Full-HD throughput
shortfall, see the [qualified PCIe workaround](USAGE.md#full-hd-pcie-workaround).
It is an optional choice for suitable BCM70015 links.

## Sunshine / Moonlight streaming

The [Moonlight guide](MOONLIGHT.md) walks through the tested x86-64 client,
pairing, H.264 720p30 playback, and the optional `previous,0`
mode. It keeps a completed picture in reserve and uses the previous frame
when needed; that changes frame-to-timestamp matching and adds delay.
All tested GUI settings, commands, measured limits and the way back to
default frame matching are together in that guide. Native 1080p60 streaming
is beyond the demonstrated path; the separate local-file scaling result
does not establish Moonlight support at that rate.

## Other installation modes

- DKMS owns the kernel module; follow [README.dkms](README.dkms) and use
  `sudo make install-runtime` for userspace.
- Native 32-bit i686 needs a matching compiler, dependencies and player. A
  32-bit library cannot load into a 64-bit player. For CPUs without SSE2, pass
  `LEGACY_CPU=1` to both build and install: `make LEGACY_CPU=1` then
  `sudo make LEGACY_CPU=1 install`. Physical no-SSE performance is unverified;
  this flag cannot remove other applications' CPU requirements.
- [Uninstall and rollback](BRINGUP.md#direct-install-update-and-rollback)
  cover the direct route; the [DKMS guide](README.dkms) covers its own route.

## Documentation

| Need | Document |
| --- | --- |
| Local playback, controls, codecs, scaling, FFmpeg/Kodi/VLC | [USAGE.md](USAGE.md) |
| Sunshine/Moonlight hardware streaming | [MOONLIGHT.md](MOONLIGHT.md) |
| Installation recovery, update and rollback | [BRINGUP.md](BRINGUP.md) |
| DKMS ownership and updates | [README.dkms](README.dkms) |
| Hardware fixtures, measurements and coverage | [HARDWARE-2026-09-13.md](HARDWARE-2026-09-13.md) |
| Current work | [TODO.md](TODO.md) |
| GStreamer options and framing | [GStreamer component guide](filters/gst/gst-plugin-1.0/README.md) |
| VA-API subset and experimental controls | [VA-API component guide](filters/vaapi/README.md) |
| Legacy API and library options | [libcrystalhd guide](linux_lib/libcrystalhd/README.md) |
| Configuration options, optional settings window/tray and schema | [CONFIGURATION.md](CONFIGURATION.md) |
| Contributing and bug reports | [CONTRIBUTING.md](CONTRIBUTING.md) |
| Maintainer release policy and qualification | [RELEASING.md](RELEASING.md) |

## Developer checks

The maintained driver, library, GStreamer and VA-API targets use `-Werror`;
the historical example programs are outside that claim. The full Ubuntu
regression suite needs these additional development packages beyond the
quick start:

```sh
sudo apt install autoconf gcc-multilib g++-multilib qemu-user \
  libavcodec-dev libavformat-dev libavutil-dev nodejs node-ws
NODE_PATH=/usr/share/nodejs make -j2 W=1 check
make userspace32-check
make legacy-cpu-check
```

Device-free checks and CI kernel API builds complement physical playback
tests. See [RELEASING.md](RELEASING.md) for the release gates and
[HISTORY.md](HISTORY.md) for changes and project lineage.

## Licensing

Existing file notices remain authoritative in this mixed-license codebase; see
[LICENSES.md](LICENSES.md).
