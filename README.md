# Broadcom Crystal HD for current Linux systems

Use **[stable 1.2.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0)
with `crystalhd-play`** for local video playback. Start with a BCM70015 card
and a progressive H.264 file at 720p30 or below. The player handles audio,
subtitles and playback controls; the card decodes video.

This fork maintains the BCM70012/BCM70015 kernel driver, firmware and legacy
`libcrystalhd.so.3` API for Linux 5.15 and newer. Selected codec and hardware
limits are in [Compatibility](#compatibility).

| I want to… | Start here |
| --- | --- |
| Install CrystalHD | [Quick start](#quick-start), or [DKMS installation](README.dkms) for automatic kernel-module rebuilds |
| Play a video | [First playback](#quick-start), then [controls and subtitles](#common-use) |
| Change playback settings | [Player options](#common-use); [desktop/Kodi settings](#settings) require 1.3 RC2 |
| Fix an installation or playback error | [Troubleshooting by symptom](BRINGUP.md#first-checks) |
| Use an existing player | [Kodi/VLC and other paths](USAGE.md#choose-a-path), or [Moonlight streaming](MOONLIGHT.md) |
| Contribute or investigate the source | [Development and project records](#development-and-project-records) |

**Need the settings window, Kodi settings add-on or experimental native Kodi
route?** Use the published [1.3.0-rc.2 prerelease](#versions-and-optional-features)
and its matching instructions. Stable 1.3 has not been released.

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

sudo apt install build-essential pkg-config gettext linux-headers-$(uname -r) \
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
`cd` steps; building and installing do not require `.git`.

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

Space pauses; Left/Right or `j`/`l` seek ten seconds; `1`, `2` and `3` select
0.5x, 1x and 2x; `q` quits.

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

See [everyday usage](USAGE.md) for codec examples,
scaling choices and tradeoffs, VA/VLC commands and troubleshooting. The width
above is a measured example, not a hardcoded or automatic default.

Start with tuning variables unset. For a repeatable Full-HD throughput
shortfall, see the [qualified PCIe workaround](USAGE.md#full-hd-pcie-workaround).
It is an optional choice for suitable BCM70015 links.

## Settings

For stable 1.2, use the Player's [command-line options](#common-use) and leave
tuning variables unset for the first run. Stable 1.2's `crystalhd-config` is a
read-only inspector; it does not save or apply managed defaults.

The published **1.3.0-rc.2** adds managed defaults and optional settings UIs:

- [Install the matching RC2 components](#install-rc2-for-managed-settings) before
  adding either editor.
- [Desktop settings window and tray](CONFIGURATION.md#optional-desktop-settings):
  install the matching RC2 tools and optional GTK frontend, then run
  `crystalhd-settings`.
- [Kodi settings add-on](addons/script.crystalhd.settings/README.md): install
  the published ZIP and matching RC2 configuration tools, then open it from
  Kodi's Program add-ons. It edits defaults; it does not install a decoder.

Restart the application after saving. Settings inspection does not certify
which decoder a running application uses. Use the
[configuration guide](CONFIGURATION.md) for installation, profiles and details.

### Install RC2 for managed settings

Keep your stable source directory for rollback. Select the published RC2 core
source in a **new directory**, rather than building current master. From the
directory where you keep source checkouts:

```sh
sudo apt install git
git clone --branch v1.3.0-rc.2 --single-branch \
  https://github.com/ahnhy1324/crystalhd.git crystalhd-1.3.0-rc.2
cd crystalhd-1.3.0-rc.2
```

Alternatively, extract the published core source
[`crystalhd-1.3.0-rc.2-940cf357ff0b.tar.gz`](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0-rc.2/crystalhd-1.3.0-rc.2-940cf357ff0b.tar.gz)
into a new directory and enter it. Use the core source `940cf357` / module
`3.10.17`; the separate native-delivery tool source is for its companion,
not this core installation.

Install the [quick-start dependencies](#quick-start), then use the existing
[direct install/update procedure](BRINGUP.md#direct-install-update-and-rollback)
or [DKMS update procedure](README.dkms#update-an-existing-installation), according
to how your current module is managed. A fresh DKMS installation uses
[Install a new revision](README.dkms#install-a-new-revision). Follow that route
from this RC2 source directory so the driver, library, GStreamer, VA and
configuration tools match. Close players before updating and verify with
`crystalhd-check` afterward. Installing only an editor over stable 1.2 does not
update its readers.

For the optional desktop window, stay in the RC2 directory and run:

```sh
sudo apt install gettext python3-gi gir1.2-gtk-3.0
sudo make install-settings
crystalhd-settings
```

The [desktop settings guide](CONFIGURATION.md#optional-desktop-settings) covers
the optional tray and profiles. For Kodi instead, continue with the
[published add-on ZIP](addons/script.crystalhd.settings/README.md#install).
This settings route uses the ordinary playback stack; the experimental native
Kodi runtime has its own [delivery guide](NATIVE-RUNTIME-DELIVERY.md).

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

## Compatibility

| What you have or want | What to expect |
| --- | --- |
| BCM70015 local files | Recommended: `crystalhd-play` / GStreamer with audio, subtitles and controls. 720p30 is well tested; native 1080p30 works on the reference host with its qualified PCIe workaround. |
| H.264, MPEG-2, MPEG-4 Part 2, VC-1 / WMV3 | Selected profiles work; see the [codec boundaries](USAGE.md#codec-boundaries). HEVC, VP8, VP9 and AV1 are unsupported. |
| Optional development/RC2 MPEG-1 experiment | Experimental MPEG-1 decoding via the MPEG-2 algorithm-1 path, natural-content hardware qualified on BCM70015 at progressive 352x288/25. Select the separately built rank-NONE `crystalhdmpeg1experimentaldec` explicitly. This is outside stable 1.2 and distinct from official/vendor support, generalized conformance and the ordinary decoder. See [setup and limitations](filters/gst/gst-plugin-1.0/MPEG1-EXPERIMENTAL.md). |
| Full-HD input with reduced card output | Optional hardware scaling reduces transfer traffic. Selected H.264 input reached about 65 pictures/s with the qualified L0s workaround active; software enlargement costs CPU and cannot recover detail. This is not native 1080p60 or a displayed-rate guarantee. |
| Sunshine / Moonlight | Default frame-matching mode works at 720p30; with V-sync/pacing off, an experimental previous-frame mode achieved about 59 renderer submissions/s at 720p60 and 29 at 1080p30. These are measured rates, not a matched speedup over default matching. Physical streaming A/V offset and forced-loss recovery remain unverified. |
| BCM70012 / older CPUs | Legacy BCM70012 compatibility and x86-64/i686 builds are retained. Recent BCM70012 and physical no-SSE hardware tests remain outstanding. |
| Kodi | Stock Kodi 21.3 progressive playback and selected 720i25 CPU Bob/Weave pass lifecycle and qualitative A/V checks on the reference host. Auto may omit second-field requests; uniform 50p, general FHD Auto and hardware Bob are not claimed. See [usage and limits](USAGE.md#kodi). |
| VA-API, VLC, browsers | VA-API is experimental. VLC 3.0.23 selects CrystalHD VA/EGL and closes cleanly on the LTS reference host, but logs late pictures. Chromium/YouTube hardware support is unverified. |

Recorded BCM70015 qualification uses Ubuntu 26.04.1 LTS / Linux 7.0.0-38.
Earlier Ubuntu 25.10 / Linux 6.17 results, including VLC 3.0.21, remain
historical; see the [dated hardware report](HARDWARE-2026-09-13.md#new-lts-qualification--2026-10-13).
The driver builds against Linux 5.15 and newer; other hosts still need testing.
One hardware session can run at a time, with coded size up to 1920x1088.

## Documentation

The [documentation index](DOCUMENTATION.md) starts with installation and everyday
use, then lists component references and development records.

| Need | Document |
| --- | --- |
| Local playback, controls, codecs, scaling, Kodi/VLC | [USAGE.md](USAGE.md) |
| Installation recovery, update and rollback | [BRINGUP.md](BRINGUP.md) |
| DKMS ownership and kernel updates | [README.dkms](README.dkms) |
| Optional RC2 desktop settings and profiles | [CONFIGURATION.md](CONFIGURATION.md#optional-desktop-settings) |
| Optional RC2 Kodi settings add-on | [Add-on guide](addons/script.crystalhd.settings/README.md) |
| Sunshine/Moonlight streaming | [MOONLIGHT.md](MOONLIGHT.md) |

Component references, qualification evidence, the work queue and contributor
rules are listed under [Development and project records](#development-and-project-records).

## Versions and optional features

| Release | Status |
| --- | --- |
| [Stable 1.2.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0), module-DKMS `3.10.13` | Recommended for the quick start above. [Final-package qualification](HARDWARE-2026-09-13.md#120-final-package-qualification--2026-10-23) and [change summary](HISTORY.md#120). |
| [Complete 1.3.0-rc.2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.2), module-DKMS `3.10.17` | Published prerelease: current-equipment qualification and companion delivery are complete; its tag and 13 assets are frozen. **Stable 1.3 remains deferred.** |
| Earlier 1.2 candidates | Immutable [RC1](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0-rc.1) / `3.10.11` and [RC2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0-rc.2) / `3.10.12`; RC2 completed bounded G0–G3 under [#473](https://github.com/ahnhy1324/crystalhd/issues/473). |

Current master includes later research and is not a replacement for frozen RC2
assets. See [current work and completed subsets](TODO.md#current-priorities).
For [v1.1.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0) or the
[Phase 1 baseline](https://github.com/ahnhy1324/crystalhd/releases/tag/phase1-baseline-2026-09-29),
use the instructions shipped with that tag. Preserve 1.2.0-rc.2 and stable 1.1.0
source/cached modules for [rollback](BRINGUP.md#direct-install-update-and-rollback).
The 1.2 scope is selected CPU VA-VPP Bob and explicit H.264 card-output scaling;
Kodi, sustained playback and package results are tracked in the
[qualification matrix](HARDWARE-2026-09-13.md#120-rc2-qualification--2026-10-22).
Native hardware deinterlacing, general FHD50/60 and Atom real-time playback are
not claimed. [Current limits and usage](USAGE.md#known-issues-and-deinterlacing-roadmap)
separate Weave, application-side filtering and unqualified paths.

For the older fixed baseline, clone with `--branch phase1-baseline-2026-09-29`
and use the guide shipped with that version.

The RC2 [native runtime companion](NATIVE-RUNTIME-DELIVERY.md) is an optional,
experimental Kodi route for the measured Core2 T8100 / BCM70015 / Intel965
cohort. Its frozen core source is `940cf357`; use its pinned package and
prerequisites. The stable quick start above uses the ordinary GStreamer/VA
stack and does not install that companion.

## Development and project records

**Project rule: do not implement or ship CrystalHD fixes by patching external
applications or dependencies.** Isolated debugging and test probes are allowed.
Read the [external code policy](EXTERNAL-CODE-POLICY.md) before work.

Read [CONTRIBUTING.md](CONTRIBUTING.md) before development; it covers task
branches, review, checks, hardware sessions and issue handling. Current work is
in [TODO.md](TODO.md#current-priorities), and [HISTORY.md](HISTORY.md) records
changes and project lineage.

| Reference | Document |
| --- | --- |
| GStreamer options and framing | [Component guide](filters/gst/gst-plugin-1.0/README.md) |
| VA-API subset and experimental controls | [Component guide](filters/vaapi/README.md) |
| Explicit experimental field-GPU output | [Build, selection and retained limits](tests/fixtures/issue530/README.md#experimental-window-consumer) |
| Legacy API and library options | [libcrystalhd guide](linux_lib/libcrystalhd/README.md) |
| Complete configuration inventory and schema | [CONFIGURATION.md](CONFIGURATION.md), [CONFIGURATION-SCHEMA.md](CONFIGURATION-SCHEMA.md) |
| Hardware fixtures, measurements and coverage | [Dated hardware report](HARDWARE-2026-09-13.md) |
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

The explicit field-GPU output additionally needs `libxcb1-dev`, `libegl-dev`,
`libgles-dev`, `libgbm-dev` and `libdrm-dev` with the GStreamer video/app
development headers. Build and run its device-free contracts with
`make -C filters/gst/field-output check`; actual window tests require X11/EGL.

Device-free checks and CI kernel API builds complement physical playback
tests. See [RELEASING.md](RELEASING.md) for the release gates and
[HISTORY.md](HISTORY.md) for changes and project lineage.

## Licensing

Existing file notices remain authoritative in this mixed-license codebase; see
[LICENSES.md](LICENSES.md).
