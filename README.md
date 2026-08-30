# Broadcom Crystal HD for current Linux systems

This maintenance fork brings the Broadcom BCM70012 and BCM70015 Crystal HD
video decoders forward to current Linux kernels. It contains the kernel
module, firmware, the legacy `libcrystalhd` API, and experimental GStreamer
1.x, VA-API, and Chromium integrations.

The kernel module supports Linux 6.1 and newer. Older compatibility branches
were removed because they were not covered by build or hardware tests.

## Project status

This is a hardware-revival project, not a production-ready multimedia stack.
The kernel and userspace pieces have different levels of validation:

| Component | Current status |
| --- | --- |
| Kernel module | Maintained for Linux 6.1 and newer; hardware-tested on BCM70015 with Ubuntu 6.17.0-41-generic. BCM70012 support is retained but has not been tested recently. |
| `libcrystalhd` | Legacy compatibility API. CI freezes its 32-bit and 64-bit ioctl layouts, builds the complete library as 32-bit code, and exercises it through the tested frontends; it still has no comprehensive device-API test suite. |
| GStreamer 1.x | Experimental. H.264 decode has been exercised on BCM70015. MPEG-2, VC-1, WMV3, interlaced output, seeking, and mid-stream format changes are not covered by current hardware tests. |
| VA-API | Experimental client-oriented subset, not a general or conformance-tested VA-API driver. Progressive H.264 decode through FFmpeg has been exercised on BCM70015. |
| Chromium | Developer experiment only. The safe default uses Chrome's software decoder; hardware decode is opt-in and has a known post-seek frame-identity failure. It also requires disabling the GPU-process sandbox. |
| Examples | Legacy diagnostic programs. CI verifies that they compile, not that their hard-coded sample streams decode correctly. |

CI compiles the module against the latest 6.1, 6.6, 6.12, and 6.18 long-term
kernels plus upstream stable and mainline. It also builds the userspace
components, checks both x86 userspace ABIs, and runs discovery, H.264
parameter-set, and installation smoke tests. Those jobs validate build and
API compatibility but do not replace hardware testing.

For the known-good BCM70015 initialization sequence, validation milestones,
and failure isolation order, see [BRINGUP.md](BRINGUP.md).

## Userspace components

The interfaces below describe what each frontend currently exposes. Unless a
path is identified as hardware-tested in the status table, it should be
treated as unverified.

The `crystalhddec` GStreamer 1.x element advertises parsed H.264 Annex-B,
MPEG-2, VC-1, and WMV3 input and produces standard YUY2 raw video. Current
hardware validation covers only progressive H.264.

The `crystalhd_drv_video.so` VA-API backend exposes progressive H.264
Constrained Baseline, Main, and High decoding. It supports NV12 output for
FFmpeg/GStreamer and exported ARGB surfaces for Chromium's compositor, and
accepts imported DRM PRIME NV12 surfaces.

The card and firmware support one playback session at a time. A second
simultaneous VA-API client receives `VA_STATUS_ERROR_HW_BUSY`; close the first
player before starting another hardware decode.

FFmpeg deprecated its CrystalHD decoders in version 6.0, and current packaged
FFmpeg and VLC builds no longer expose CrystalHD decoding. Installing this
kernel module alone therefore does not make current VLC use the card. Programs
can instead use the GStreamer element or the standard VA-API backend.

CrystalHD does not decode VP8, VP9, or AV1. YouTube normally prefers those
newer codecs. The optional Chrome setup installs an H.264 preference policy,
but browser hardware decode remains disabled by default because the
experimental VA-API path is not seek-correct.

## Dependencies

On Ubuntu:

```sh
sudo apt install build-essential autoconf dkms pkg-config \
  gcc-multilib g++-multilib \
  linux-headers-$(uname -r) \
  curl desktop-file-utils xdg-utils \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  libva-dev libdrm-dev libgbm-dev libswscale-dev vainfo
```

The optional hardware-stress and browser probes also use:

```sh
sudo apt install ffmpeg nodejs node-ws
```

## Build and test

Build the kernel module, userspace library, examples, and experimental
GStreamer and VA-API frontends:

```sh
make -j$(nproc)
make check
```

`make check` builds every component, freezes the 32-bit and 64-bit public ioctl
layouts, validates GStreamer and VA-API discovery, checks VA-API H.264 SPS/PPS
generation, checks the browser scripts and assets, tests DRM PRIME NV12 surface
import when a render node is available, and checks a staged installation
without changing the host system. It does not decode a stream on CrystalHD
hardware.

The CI-only `make userspace32-check` target builds and links the complete
`libcrystalhd` library with `-m32`, then removes those temporary 32-bit build
products. A 32-bit process on a 64-bit kernel requires `CONFIG_COMPAT`; the
driver translates the pointer-bearing playback ioctls rather than treating a
32-bit request as a native structure.

To exercise the actual decoder hardware with an H.264 MP4:

```sh
./tests/gstreamer-hardware.sh /path/to/video.mp4
```

To exercise the same hardware through VA-API and FFmpeg:

```sh
LIBVA_DRIVER_NAME=crystalhd \
LIBVA_DRIVERS_PATH=$PWD/filters/vaapi \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i /path/to/video.mp4 \
  -vf hwdownload,format=nv12 -f null -
```

To repeat the decode, verify that every frame reaches an NV12 surface, and
scan the new kernel log entries for driver failures:

```sh
./tests/vaapi-hardware-stress.sh /path/to/video.mp4 10
```

The hardware tests load the locally built module only when necessary and
unload it afterward if the script loaded it. They refuse to run when an
already-loaded module has a different source version, preventing an old DKMS
build from being mistaken for the code under test.

## Install

```sh
sudo make install
sudo modprobe crystalhd
gst-inspect-1.0 crystalhddec
```

Installation places:

- `crystalhd.ko` under `/lib/modules/$(uname -r)/updates`
- firmware under `/lib/firmware`
- the udev rule under `/lib/udev/rules.d`
- `libcrystalhd`, public headers, and `libcrystalhd.pc` under `/usr`
- `libgstcrystalhd.so` in GStreamer's detected plugin directory
- `crystalhd_drv_video.so` in libva's detected driver directory
- `crystalhd-chromium`, `setup-crystalhd-chrome-default`, the bundled H.264
  preference extension, and a desktop launcher

### Device access and diagnostics

The installed udev rule creates `/dev/crystalhd` as `root:video` with mode
`0660` and asks systemd-logind to grant the active desktop user an ACL. On a
headless system, add the playback account to the `video` group and log in
again:

```sh
sudo usermod -aG video "$USER"
```

Ordinary firmware loading and decode remain available through that device
permission. Direct register, FPGA, device-DRAM, and PCI configuration ioctls
are diagnostic interfaces and additionally require `CAP_SYS_RAWIO`; run legacy
diagnostic tools as root when those commands are needed. The rule no longer
makes the raw hardware interface world-writable.

To stage a package instead of changing the host:

```sh
make DESTDIR=/tmp/crystalhd-package install
```

## GStreamer playback

For an H.264 MP4 file:

```sh
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! videoconvert ! autovideosink
```

For a build that has not been installed:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! fakesink sync=false
```

## Chrome and YouTube

### Persistent one-time setup

Run the setup from a normal desktop login, not a root shell:

```sh
./scripts/setup-crystalhd-chrome-default
```

The script is idempotent and performs the complete desktop setup:

- installs the current non-Snap Google Chrome package when it is absent; the
  package can also configure Google's APT repository for browser updates
- builds and installs the CrystalHD kernel/userspace stack and launcher
- saves launcher settings in `~/.config/crystalhd/chromium.conf`
- creates the persistent profile `~/.config/crystalhd/chrome-profile`
- uses Chrome's local basic password store for that profile, avoiding desktop
  keyring unlock prompts
- writes the system-wide
  [`ExtensionInstallForcelist`](https://chromeenterprise.google/policies/extension-install-forcelist/)
  policy `/etc/opt/chrome/policies/managed/crystalhd-h264.json`, which
  force-installs the third-party Chrome Web Store
  [`h264ify`](https://chromewebstore.google.com/detail/h264ify/aleakchihdccplidncghkekgioiakgal)
  extension so YouTube selects H.264 rather than VP9 or AV1
- registers `crystalhd-chromium.desktop` for HTTP, HTTPS, and HTML
- shadows the ordinary Google Chrome application entry for the current user,
  so the normal Chrome icon also starts the CrystalHD launcher

After setup, open the normal **Google Chrome with CrystalHD** application or
click any web link. No environment variables are required. The launcher uses
the dedicated profile so an already-running standard Chrome profile cannot
silently absorb the launch and discard the CrystalHD settings.

The managed extension policy applies to every Google Chrome profile on the
machine, not only the dedicated CrystalHD profile. Chrome shows the browser as
managed while this policy is installed.

Canonical's Chromium snap is not supported. Snap confinement denies access to
`/dev/crystalhd`; copying the VA-API driver into the snap does not grant that
device access. The setup therefore installs Google's non-Snap Debian package.

### Decoder, display GPU, and compositor

The launcher defaults to Chrome's `FFmpegVideoDecoder`. Repeated X11 pixel
captures showed that current Chrome recycles VA-API output buffers before the
legacy CrystalHD firmware resolves reordered pictures; after a seek this can
present pre-seek frames under new media timestamps. DMA-BUF fencing prevents
partial writes but cannot repair that frame-identity mismatch. Correct seeks
therefore take priority over browser hardware decoding.

Set `CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` only to test the unresolved
VA-API browser path. CrystalHD remains available to FFmpeg, GStreamer, and
direct VA-API clients. `/dev/dri/renderD128` belongs to the display GPU and is
used for allocating/displaying surfaces. On the tested Intel 965GM system,
the launcher uses Mesa llvmpipe for composition.

Set `CRYSTALHD_CHROMIUM_NATIVE_GL=1` in
`~/.config/crystalhd/chromium.conf` only when the display GPU supports current
Chrome. Set `CRYSTALHD_DRM_DEVICE` there if the active render node is not
`/dev/dri/renderD128`.

### Security boundary

Chrome's GPU sandbox does not broker `/dev/crystalhd` or this out-of-tree
VA-API driver. The launcher therefore uses `--disable-gpu-sandbox`. Renderer,
network, and other browser-process sandboxes remain enabled, but graphics and
video parsing in the GPU process are unsandboxed. Because the persistent setup
makes this browser the desktop default, use it only for sites you trust.

The setup disables Chrome's command-line security-warning banner through the
managed `CommandLineFlagSecurityWarningsEnabled` policy. This only hides the
repeated `--disable-gpu-sandbox` warning; it does not restore the GPU sandbox.
The policy is browser-wide, so Chrome also hides warnings for other dangerous
command-line flags while the policy remains installed.

The basic password backend does not protect saved passwords with the desktop
keyring. Avoid saving passwords in the dedicated profile, or set
`CRYSTALHD_CHROMIUM_PASSWORD_STORE=gnome-libsecret` in `chromium.conf` to use
the keyring and accept its unlock prompt.

Chrome may print `Created TensorFlow Lite XNNPACK delegate for CPU` when its
Safe Browsing client-side phishing model starts. This is an informational
message unrelated to CrystalHD, VA-API, or GPU acceleration; the launcher does
not disable that browser security feature.

Without the persistent configuration, the launcher refuses to disable the GPU
sandbox until `CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1` is explicitly set.

### Verify browser playback

YouTube's **Stats for nerds** should show an `avc1` codec. In
`chrome://media-internals`, the active player should report
`FFmpegVideoDecoder` and platform decoding `false`. This is the safe browser
default.

To inspect the experimental hardware path, add
`CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` to `chromium.conf`. While it is
active, the GPU process should own the device:

```sh
sudo fuser -v /dev/crystalhd
```

For an automated check after persistent setup, start the installed launcher:

```sh
CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1 \
  crystalhd-chromium --remote-debugging-port=9223 \
  --remote-allow-origins=http://localhost about:blank
```

Then run the probe in a second terminal. The managed H.264 policy means no
diagnostic codec injection is necessary:

```sh
./tests/chromium-youtube.js --port 9223 \
  --expect-hardware 'https://www.youtube.com/watch?v=aqz-KE-bpKQ'
```

The experimental check expects an `avc1` codec and
`Chrome decoder: VaapiVideoDecoder (platform=true)`. The probe exits
unsuccessfully unless playback advances through the hardware path without a
media-timeline regression or Chrome media error. Use `--seconds 60` for a
sustained check that covers YouTube's adaptive quality changes.
Add `--seek-at 20 --seek-to 120` to perform a real timeline jump and fail if a
pre-seek frame is presented again after the new timeline has settled.
Use `--force-h264` only as a diagnostic fallback on a profile where the
managed extension is not installed.

Omit both the experimental environment variable and `--expect-hardware` to
verify the safe `FFmpegVideoDecoder` path.

### Persistent files and removal

The setup changes these persistent locations:

- `~/.config/crystalhd/chromium.conf`
- `~/.config/crystalhd/chrome-profile`
- `~/.local/share/applications/crystalhd-chromium.desktop`
- `~/.local/share/applications/google-chrome.desktop`
- `/etc/opt/chrome/policies/managed/crystalhd-h264.json`

To stop using CrystalHD as the desktop default, select another browser in the
desktop settings and remove the two per-user desktop entries. Removing the
managed policy stops force-installing `h264ify`; removing the dedicated
profile deletes only this launcher's browsing data. Re-run the setup after a
launcher update to reinstall the latest files.

See [`filters/vaapi/README.md`](filters/vaapi/README.md) for backend details and
limitations.

## DKMS

The regular install above places a module only under the currently selected
kernel. To rebuild it automatically after kernel upgrades, register the source
with DKMS:

```sh
sudo ln -sfn "$PWD" /usr/src/crystalhd-3.10.0
sudo dkms add -m crystalhd -v 3.10.0
sudo dkms build -m crystalhd -v 3.10.0
sudo dkms install -m crystalhd -v 3.10.0
```

Verify that the module is installed for the running kernel and that its PCI
alias can be loaded automatically at boot:

```sh
dkms status
modinfo crystalhd
sudo modprobe crystalhd
```

See [HISTORY.md](HISTORY.md) for the history of the original driver releases.

## Licensing

This is a mixed-license codebase. Existing file notices remain authoritative;
new maintenance-fork material is identified in [LICENSES.md](LICENSES.md).
