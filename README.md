# Broadcom Crystal HD for current Linux systems

This fork maintains the BCM70012/BCM70015 kernel driver, firmware and legacy
`libcrystalhd` API for Linux 6.1 and newer. Start with **GStreamer on BCM70015**
for local playback. This is a hardware-revival project, not a production-ready
multimedia stack.

[Install](#install) · [Local playback](#gstreamer-playback) ·
[Full HD option](#optional-bcm70015-full-hd-workaround) ·
[Remaining work](TODO.md) · [Troubleshooting](BRINGUP.md)

## Project status

| Component | Supported path and limits |
| --- | --- |
| Kernel / library | BCM70015 hardware-tested; BCM70012 retained but not recently tested. Native and 32-bit compatibility checks are not comprehensive device-API conformance. |
| GStreamer | Primary playback path. Selected H.264, MPEG-2, VC-1 and WMV3 fixtures pass; broader streams and physical display/audio validation remain open. |
| VA-API / FFmpeg | Experimental progressive H.264, plus BCM70015 MPEG-2 Simple/Main and standard WMV3 Simple/Main / VC-1 Advanced. See the [codec and replay limits](filters/vaapi/README.md), including older WMV3 variants; synchronous clients can incur substantial restart/replay overhead. |
| PowerVLC | Experimental native-plugin integration. Codec, end-of-stream, playback-rate and A/V limitations remain. |
| Chrome / YouTube | Software decoding with the GPU sandbox enabled is the default. Hardware decoding and live YouTube A/V synchronization remain unresolved. |

Exact fixtures, measurements and historical failures are in the
[hardware report](HARDWARE-2026-09-13.md); open acceptance criteria are in
[TODO.md](TODO.md). Clocked test sinks do not certify physical lip-sync.

## Userspace components

The card supports **one playback session at a time**. Close other CrystalHD
players before a hardware test or module unload. GStreamer produces YUY2;
VA-API provides NV12 downloads and experimental DRM PRIME/VPP integration.
Neither adds VP8, VP9 or AV1 support to the card. Distribution VLC builds may
omit their native CrystalHD plugin; installing this driver does not enable it.
See the [GStreamer](filters/gst/gst-plugin-1.0/README.md),
[VA-API](filters/vaapi/README.md) and
[library](linux_lib/libcrystalhd/README.md) guides for format/API contracts.

## Dependencies

On Ubuntu:

```sh
sudo apt install build-essential autoconf dkms pkg-config \
  gcc-multilib g++-multilib linux-headers-$(uname -r) \
  curl desktop-file-utils xdg-utils \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  python3-gi gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 \
  libva-dev libdrm-dev libgbm-dev libswscale-dev vainfo
```

Optional hardware/browser probes need `ffmpeg`, `nodejs` and `node-ws`.
Direct-library and VA-API seek probes also need `libavcodec-dev`,
`libavformat-dev` and `libavutil-dev`. Software playback may need
`gstreamer1.0-libav`.

## Build and test

```sh
make -j$(nproc)
make check
make userspace32-check
```

The kernel build treats warnings as errors. `make check` builds the components
and runs ABI, library, frontend, browser and staged-install checks, plus DRM
smoke tests when a render node is available. It does not decode video on the
CrystalHD card. The 32-bit check uses an isolated build directory; a 32-bit
process on a 64-bit kernel needs `CONFIG_COMPAT`.
CI also compiles Linux LTS, stable and mainline kernel APIs.

### Legacy CPUs without SSE

`make -C linux_lib/libcrystalhd LEGACY_CPU=1` builds a scalar **32-bit i686**
library without SSE/SSE2 or MMX. The same option applies to userspace plugins
and examples; they need matching 32-bit dependencies, pkg-config settings and
installation directories. It does not make a 32-bit library loadable by a
64-bit player, change the kernel's CPU requirements, or guarantee old-machine
playback speed. `LEGACY_CPU=0` restores the ordinary accelerated build; mode
changes automatically rebuild affected outputs. Contradictory `-m` flags are
rejected in legacy mode.

With `qemu-user` installed, `make legacy-cpu-check` executes production tests
and the actual shared library under a no-SSE Pentium II model. `QEMU_I386` may
select a privately extracted emulator instead. Legacy VA-API builds retain
synchronous CPU access but reject asynchronous VPP before publishing a fence;
that path requires supported cache-flush and barrier instructions.

Validation uses Linux i386 userspace (GCC 15.2/glibc 2.42) under emulation,
plus 180/180 FHD H.264 frames on BCM70015 with the scalar 32-bit library on an
SSE2-capable host. Physical no-SSE machines and full 32-bit player/plugin
stacks remain unverified; use an OS and dependencies built for the target CPU.

Hardware validation requires an idle card and matching loaded module. Follow
[module/ABI checks](BRINGUP.md#device-access-and-module-identity),
[GStreamer counts/replay](filters/gst/gst-plugin-1.0/README.md#counted-h264-playback-and-replay),
[VA-API stress/seeks](filters/vaapi/README.md#hardware-stress), or
[direct-library drain](linux_lib/libcrystalhd/README.md#direct-library-drain-validation).
Legacy [examples](examples/README.md) are diagnostic programs, not playback
validation.

## Install

```sh
sudo make install
sudo modprobe crystalhd
gst-inspect-1.0 crystalhddec
```

This installs the module for the selected kernel, firmware, device rule,
library/headers, GStreamer/VA-API plugins and launcher files. It does **not**
replace an already-loaded module or make Chrome the default browser. Verify
[loaded module identity](BRINGUP.md#device-access-and-module-identity) before
testing a new build; never force-unload an active card.

For automatic rebuilding after kernel upgrades, follow [DKMS](README.dkms).
To inspect an installation without changing the host:

```sh
crystalhd_stage=$(mktemp -d)
make DESTDIR="$crystalhd_stage" install
```

### Device access and diagnostics

The udev rule uses `root:video`, mode `0660`, with an ACL for the active desktop
user. On a headless system, add the playback account to `video` and log in again:

```sh
sudo usermod -aG video "$USER"
```

Playback needs device access, not root. Raw register/DRAM/PCI diagnostics also
require `CAP_SYS_RAWIO`; legacy exceptions and local access overrides are in
[bring-up](BRINGUP.md#device-access-and-module-identity).

### Optional BCM70015 Full HD workaround

On the tested BCM70015/ICH8 link, disabling PCIe **L0s only** removes the
observed FHD throughput bottleneck. The default remains unchanged. For an
affected system, close all CrystalHD players and test a local build with:

```sh
make driver
sudo modprobe -r crystalhd
sudo insmod ./driver/linux/crystalhd.ko force_l0s_off=1
```

This option is limited to BCM70015 on a dedicated root-port link. It preserves
L1, link speed/width, payload sizes and other devices, and can increase power
consumption. It first requests PCI-core control; an ownership denial or
compiled-out ASPM support permits the checked, bit-preserving fallback.
No firmware or application rebuild is needed.

Raw-owned original L0s bits are restored and checked on driver removal.
**PCI-core-managed policy can remain disabled after unload**, until separately
reset or rebooted: reloading without the option is not a universal reset.
Restoration failures are logged; a failed power transition leaves the adapter
unavailable until reload. Do not use global `pcie_aspm=force` or change link
speed/MPS. Physical lip-sync, FHD 2x controls and actual suspend/resume are not
certified by this result. See the
[measurements and reproduction](HARDWARE-2026-09-13.md#pcie-l0s-isolation-2026-09-27).

## GStreamer playback

From a build tree, the local-file controller provides video, audio and optional
external subtitles:

```sh
./scripts/crystalhd-play --hardware video.mp4
./scripts/crystalhd-play --software --subtitles captions.srt video.mp4
```

After installation, use `crystalhd-play` without the source-tree prefix. Keep
the launching terminal open: Space pauses/resumes; Left/Right or `j`/`l` seek
ten seconds; `1`/`2`/`3` select 0.5x/1x/2x; `q` quits. Hardware mode is the
default and confirms actual decoded output. Unsupported input, a busy card
or decoder failure does not silently select software; retry with `--software`.
Ranks apply only to this process, not system defaults.

See the [GStreamer guide](filters/gst/gst-plugin-1.0/README.md) for pipelines,
codec framing and control tests. Available controls do not guarantee real-time
playback at every resolution/rate. Tests need external timeouts because a
blocked library call can outlive the cooperative player watchdog.

## FFmpeg through VA-API

For an installed build and supported H.264 file:

```sh
LIBVA_DRIVER_NAME=crystalhd \
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i video.mp4 \
  -vf hwdownload,format=nv12 -f null -
```

This decodes/downloads frames, not visible playback. The render node belongs
to the display GPU; CrystalHD performs H.264 decoding. Select your system's
node. Uninstalled-build paths, export requirements and tests are in the
[VA-API guide](filters/vaapi/README.md).

## PowerVLC native playback (experimental)

The [PowerVLC 2.1.0 Linux x86_64 release](https://github.com/Olsro/powervlc/releases/tag/powervlc-2.1.0)
includes a native CrystalHD plugin and uses our ABI-compatible library without
rebuilding. Supported file/container combinations depend on the player frontend;
a playable file may use software decoding. End-of-stream picture loss,
playback-rate and A/V limitations are tracked in
[#18](https://github.com/ahnhy1324/crystalhd/issues/18). Use GStreamer as the primary
playback path. No PowerVLC application patches are supplied here.

Extract the downloaded AppImage in its own directory with
`./PowerVLC-2.1.0-x86_64.AppImage --appimage-extract`. From this repository root:

```sh
make library
LD_PRELOAD="$PWD/linux_lib/libcrystalhd/libcrystalhd.so.3" \
  /path/to/squashfs-root/AppRun \
  --ignore-config --no-one-instance --no-qt-privacy-ask \
  --no-metadata-network-access --no-disable-screensaver \
  --crystalhd --codec=crystalhd,avcodec,none --avcodec-hw=none \
  --video-cache-mb=0 --no-spu --text-renderer=tdummy --vout=xcb_x11 \
  /path/to/video.mp4
```

`LD_PRELOAD` selects our library; `LD_LIBRARY_PATH` alone loses to AppRun's
bundled paths. These diagnostic options disable look-ahead caching/subtitles
and avoid an observed bundled font-renderer stall. They do not replace system
VLC. Add `VLC_CHD_TRACE=1` and `-vvv` for diagnostics; require the **main video**
decoder to remain `crystalhd` with actual output. Audio/thumbnail `avcodec`
activity is not main-video fallback, and decode logs are not presentation proof.

The strict offscreen pixel regression requires the extracted SDK, a C compiler
and GNU `timeout`:

```sh
crystalhd_sample_dir=$(mktemp -d)
sh tests/generate-browser-sample.sh "$crystalhd_sample_dir/av360.mp4" --av-360p
sh tests/powervlc-playback.sh /path/to/squashfs-root \
  "$PWD/linux_lib/libcrystalhd" "$crystalhd_sample_dir/av360.mp4"
```

The default requires every picture through EOF and currently fails natively.
`--software` selects the reference decoder; `--controls` tests pause/seek/rate
progress; `--half` / `--double` isolate rate checks after settling. These use
temporary settings and do not test audible sync or a real window.

## Chrome and YouTube

Chrome defaults to **software decoding**, not the CrystalHD card. The codec
preference extension no longer forces 480p or masks seeks; live YouTube A/V
desynchronization remains open in [#12](https://github.com/ahnhy1324/crystalhd/issues/12).
Local-file tests do not establish live-session acceptance.

### Persistent one-time setup

This optional setup changes system/browser defaults. It is not needed for
GStreamer or FFmpeg, nor should it be used just to update an extension.
Re-running it overwrites launcher configuration. Run from a normal desktop
login, not a root shell:

```sh
./scripts/setup-crystalhd-chrome-default
```

The idempotent script installs non-Snap Google Chrome if absent (its package
may configure Google's update APT repository), builds/installs this stack,
and performs the following:

- Rewrites `~/.config/crystalhd/chromium.conf` and uses a dedicated
  `~/.config/crystalhd/chrome-profile` to prevent an existing Chrome process
  from silently absorbing launch settings.
- Uses the **basic password store**, without desktop-keyring protection.
- Persists `CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1`. Software decoding still
  keeps the sandbox; later enabling experimental hardware disables it without
  another prompt.
- Force-installs third-party [h264ify](https://chromewebstore.google.com/detail/h264ify/aleakchihdccplidncghkekgioiakgal)
  through machine-wide policy, affecting **all Chrome profiles** and marking
  Chrome as managed.
- Packages/registers the bundled codec-preference extension with a persistent
  signing key/ID. Linux external registration can affect fresh profiles even
  when Chrome ignores `--load-extension`.
- Registers the launcher for HTTP/HTTPS/HTML and shadows the normal Chrome
  desktop entry for the current user.
- Sets a browser-wide policy hiding command-line security warnings. This does
  **not** restore any disabled sandbox and also hides warnings for other flags.
  The managed policy also sets `DefaultBrowserSettingEnabled=false`.

The bundled 1.6.0 extension filters codec capabilities only; update older 1.5.0
packages that changed quality/visibility. Unsupported codecs and rates above
30 fps remain filtered. An installed CRX update needs both a newly signed
package and updated external-registration version; `make install` alone does
not replace it. Chromium snap is unsupported: its confinement denies card access.

### Decoder, display GPU, and compositor

The launcher selects `FFmpegVideoDecoder` by default. Hardware decoding remains
experimental despite driver-level fixes. For developer tests,
`CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` selects VA-API, subject to the
sandbox acknowledgement below. Asynchronous VPP also needs a usable write
fence source; disabling the sandbox alone does not provide it.

The display GPU's render node allocates/presents surfaces. In `chromium.conf`,
use `CRYSTALHD_DRM_DEVICE` for a node other than `/dev/dri/renderD128`, or
`CRYSTALHD_CHROMIUM_NATIVE_GL=1` only when the display GPU supports current
Chrome. Composition defaults to Mesa llvmpipe. See [VA-API limitations](filters/vaapi/README.md).

### Security boundary

Software decoding keeps the GPU sandbox enabled even if an old configuration
contains the opt-out acknowledgement. Experimental hardware decoding requires
`CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1` and runs graphics/video parsing
without the GPU-process sandbox. Other browser sandboxes remain enabled.
The warning-hiding policy does not make this safe.

Avoid saving passwords in the basic-store profile. Set
`CRYSTALHD_CHROMIUM_PASSWORD_STORE=gnome-libsecret` in `chromium.conf` for
keyring protection and accept its unlock prompt. The informational XNNPACK
message may come from Chrome's phishing protection, not hardware decoding;
the launcher does not disable that security feature.

### Verify browser playback

YouTube's Stats for nerds should show `avc1`; `chrome://media-internals` should
report `FFmpegVideoDecoder`, platform decoding `false`, for the default path.
For numbered-pixel/seek checks in a temporary profile with the GPU sandbox:

```sh
crystalhd_sample_dir=$(mktemp -d)
sh tests/generate-browser-sample.sh "$crystalhd_sample_dir/browser.mp4"
node tests/chromium-local-playback.js "$crystalhd_sample_dir/browser.mp4"
node tests/chromium-local-playback.js "$crystalhd_sample_dir/browser.mp4" --controls
```

`--controls` adds pause/resume and 0.5x/1.5x/2x/restored 1x checks. This tests
sampled pixel identity/timestamps, not audible sync or full-rate display.
`--expect-hardware` rejects software fallback and requires explicit hardware
and sandbox opt-in.

After persistent setup, a software-default live probe uses two terminals:

```sh
crystalhd-chromium --remote-debugging-port=9223 \
  --remote-allow-origins=http://localhost about:blank
```

```sh
node tests/chromium-youtube.js --port 9223 \
  'https://www.youtube.com/watch?v=aqz-KE-bpKQ'
```

For an acknowledged hardware experiment, set
`CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` on the launcher and append
`--expect-hardware` to the probe. It requires `VaapiVideoDecoder`, platform
`true`, and output without fallback. Device ownership (`sudo fuser -v /dev/crystalhd`)
alone does not prove valid pictures. The default window is 90 seconds;
`--seek-at 20 --seek-to 120` checks a jump and subsequent frame timestamps,
not independent pixels. `--force-h264` is a diagnostic fallback without the
managed extension.

Service/player rejection (`ump.spsrejectfailure` /
`HTML5_SPS_UMP_STATUS_REJECTED`) has occurred with extensions disabled and
software decoding. HTTP 200 is not a playback pass. Do not hide automation
indicators or bypass service verification to make a test pass.

### Persistent files and removal

Setup state includes:

- `~/.config/crystalhd/`: launcher config, dedicated profile and extension key.
- `~/.local/share/applications/crystalhd-chromium.desktop` and
  `~/.local/share/applications/google-chrome.desktop`.
- `/etc/opt/chrome/policies/managed/crystalhd-h264.json`: managed extension,
  default-browser and command-line-warning policy.
- `/usr/share/crystalhd/crystalhd-seek-gate.crx` and
  `/opt/google/chrome/extensions/<extension-id>.json`: bundled package and
  registration; historical names preserve the extension identity.

Select another desktop-default browser and remove the two per-user desktop
entries to stop redirecting launches. Remove this setup's managed policy to
stop its browser policies, and its external registration to stop
installing the bundled extension in new profiles. Leave unrelated policies
and registrations alone. Deleting the dedicated profile deletes its browsing
data; retain the signing key for same-ID updates. System driver/library
installation is separate. Re-running setup reinstalls its files and defaults.

## DKMS

Follow [README.dkms](README.dkms) for the maintained procedure, including
existing-version handling, selected-kernel installation, source-version checks
and safe reload. DKMS handles the module, not firmware or userspace plugins.

## Licensing

Existing file notices remain authoritative in this mixed-license codebase;
see [LICENSES.md](LICENSES.md). [HISTORY.md](HISTORY.md) records original driver
releases, not the current task list.
