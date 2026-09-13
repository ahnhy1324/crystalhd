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
| GStreamer 1.x | Primary validation path on BCM70015: progressive H.264 Baseline/Main/High, complete-file drain, and flushing replay with identical pixels. MPEG-2, VC-1 Advanced, and WMV3 Main each have a small complete-drain hardware fixture. Interlaced output, arbitrary in-flight seeks, and mid-stream format changes remain unverified. |
| VA-API | Experimental client-oriented subset. BCM70015 H.264 Baseline/Main/High complete-file decode and pipelined seek checks pass with verified pixels; High also passes the synchronous seek probe. Bounded IDR replay handles firmware drain but can be expensive for synchronous clients. Not a general or conformance-tested VA-API driver. |
| Chromium | Developer experiment only. The safe default uses Chrome's software decoder with the GPU sandbox enabled. Hardware decode is opt-in, has unresolved post-seek correctness, and requires disabling the GPU-process sandbox. |
| Examples | Legacy diagnostic programs. CI verifies that they compile, not that their hard-coded sample streams decode correctly. |

CI compiles the module against the latest 6.1, 6.6, 6.12, and 6.18 long-term
kernels plus upstream stable and mainline. It also builds the userspace
components, checks both x86 userspace ABIs, and runs discovery, H.264
parameter-set, and installation smoke tests. Those jobs validate build and
API compatibility but do not replace hardware testing.

For the known-good BCM70015 initialization sequence, validation milestones,
and failure isolation order, see [BRINGUP.md](BRINGUP.md).
See the [2026-09-13 hardware report](HARDWARE-2026-09-13.md) for exact samples,
commands, successful tests, and failures still under investigation.

## Userspace components

The interfaces below describe what each frontend currently exposes. Unless a
path is identified as hardware-tested in the status table, it should be
treated as unverified.

For the reproducible playback baseline, use **GStreamer 1.x on BCM70015**
with progressive H.264 Annex-B input and YUY2 output. The secondary,
**experimental** path is **FFmpeg through VA-API**, decoding progressive
H.264 to NV12. The tested BCM70015 files now drain completely; clients that
synchronize every frame without feeding ahead can incur substantial decoder
restart/replay overhead. GStreamer remains the primary playback path.
Broader codec coverage, BCM70012, and
Chromium hardware decoding remain experimental; see [TODO.md](TODO.md) for
the open validation work and linked GitHub issues.

The `crystalhddec` GStreamer 1.x element advertises parsed H.264 Annex-B,
MPEG-2, VC-1, and WMV3 input and produces standard YUY2 raw video. Current
hardware validation covers progressive H.264 plus small MPEG-2, VC-1 Advanced,
and WMV3 Main fixtures. VC-1 and WMV3 use distinct firmware subtypes and
framing; ASF demuxer output is accepted directly, while raw VC-1 BDUs are
assembled into pictures. See the hardware report for exact caps and commands.

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

The `make userspace32-check` target builds and links the complete
`libcrystalhd` library, examples, and API probe with `-m32` in an isolated
temporary directory, preserving existing native build products. CI also
compiles the kernel module for native i386. A 32-bit process on a 64-bit kernel requires `CONFIG_COMPAT`; the
driver translates the pointer-bearing playback ioctls rather than treating a
32-bit request as a native structure.

With the freshly built driver loaded and the device idle, run
`sh tests/ioctl-smoke.sh` to exercise native/compat ioctl validation, then
`sh tests/userspace32.sh --hardware` to verify firmware open, capabilities,
version, and close through both 32-bit and 64-bit libraries. The latter is an
explicit hardware test and does not run as part of `make check`.

To exercise the actual decoder hardware with an H.264 MP4:

```sh
./tests/gstreamer-hardware.sh /path/to/video.mp4 2
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
# Exercise decoder teardown within one FFmpeg process (five input loops):
CRYSTALHD_TEST_INPUT_LOOPS=5 ./tests/vaapi-hardware-stress.sh /path/to/video.mp4 1
```

Generate three small, reproducible profile samples for a hardware report:

```sh
sh tests/generate-h264-samples.sh /tmp/crystalhd-samples
# Separate optional Full HD fixtures (1920x1080, 30 fps, 180 frames each):
sh tests/generate-h264-samples.sh /tmp/crystalhd-fhd-samples 1920x1080
```

The generator refuses to overwrite existing samples and prints each profile,
frame count, and SHA-256 checksum. Run both hardware scripts for each of the
three MP4 files. `CRYSTALHD_TEST_TIMEOUT` bounds each decode (120 seconds by
default); a timeout or missing frame fails validation.

Set `CRYSTALHD_TEST_SEEK=1` on the GStreamer hardware command to replay the
file after a flushing seek to zero in the same pipeline. Both passes must
drain every frame and produce the same pixel SHA-256. This checks replay
after end-of-stream; arbitrary seeks during playback need separate coverage.

For VA-API seek/flush validation outside Chromium, install `libavcodec-dev`,
`libavformat-dev`, and `libavutil-dev`, then run:

```sh
make -C filters/vaapi seek-test
LIBVA_DRIVER_NAME=crystalhd LIBVA_DRIVERS_PATH=$PWD/filters/vaapi \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
timeout --kill-after=10 120s ./filters/vaapi/vaapi-seek-test /tmp/crystalhd-samples/high.mp4
```

This probe requires hardware frames, drains a complete reference decode, then
compares the SHA-256 hashes of downloaded NV12 pixels after forward and
backward seeks and decoder flushes. It also flushes with reordered frames
still pending. `--software` is an explicitly labelled self-check of the
probe; it does not validate CrystalHD.

Append `--lookahead 8` for a bounded pipelined-client test. It retains eight
future output frames before downloading the oldest and leaves an undownloaded
suffix at each seek. This is separately reported coverage; the default remains
synchronous and can be much slower on firmware that requires future input.

Use `--retain-old-frames` to retain eight original frame owners across flush
and new input, or actual decoder-context destruction, before downloading and
checking their original PTS/pixel hashes. This implies eight-frame lookahead;
unlike `--lookahead 8` alone, it tests old-frame lifetime after teardown.

For a local browser pixel/seek audit, run these from a graphical session:

```sh
sh tests/generate-browser-sample.sh /tmp/crystalhd-browser-barcode.mp4
node tests/chromium-local-playback.js /tmp/crystalhd-browser-barcode.mp4
```

This uses a temporary profile and Chrome's software default with its GPU
sandbox enabled. It checks numbered pixels against retained video-frame
timestamps through four seeks and requires the actual final frame at EOS.
It does not measure full-rate presentation or establish hardware decoding.
The explicit `--expect-hardware` mode rejects any software fallback and
requires the launcher's experimental GPU-sandbox opt-out acknowledgement.

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

The read-only PCI vendor/device-ID DWORD (offset 0, size 4) remains available
for older libraries that use it to identify the card before opening firmware.
New builds use the unprivileged hardware-type query instead.

BCM70015 playback sessions also retain the legacy color-register operation:
the kernel permits YUY2/UYVY selection while preserving unrelated register
bits. Adjacent registers and access outside playback remain privileged.
BCM70012's legacy reset, clock, and FPGA initialization need separate hardware
verification under this permission policy.

For a legacy installation that deliberately needs all local accounts to open
the device, create `/etc/udev/rules.d/99-crystalhd-local.rules` containing:

```udev
KERNEL=="crystalhd", MODE="0666"
```

Reload the rules with `sudo udevadm control --reload-rules`; they take effect
when the device is recreated. Removing that local file restores the default
access policy on the next rule reload and device creation. This override
does not grant the capability required by raw diagnostic ioctls.

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
- packages and registers the bundled codec-preference extension as a local
  Chrome extension, preserving its signing key and extension ID on updates
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

The bundled extension is also installed through Chrome's
[Linux external-extension mechanism](https://developer.chrome.com/docs/extensions/how-to/distribute/install-extensions#linux),
which can load it in fresh profiles even when `--load-extension` is ignored.
Version 1.6.0 filters codec capability queries only: it does not force 480p,
change player quality, hide posters, or mask video during seeks. Version
1.5.0 had those presentation overrides and should be updated. Unsupported
video codecs and frame rates above 30 fps are still reported as unsupported;
that can affect which representations the site offers without overriding the
user's selection among compatible ones.
For a local CRX update, both the signed package and the external registration's
version must change; updating repository files or running `make install` alone
does not replace an already packaged extension. The full desktop setup above
also changes system/browser defaults, so do not use it merely to update an
extension unless those broader changes are intended.

Canonical's Chromium snap is not supported. Snap confinement denies access to
`/dev/crystalhd`; copying the VA-API driver into the snap does not grant that
device access. The setup therefore installs Google's non-Snap Debian package.

### Decoder, display GPU, and compositor

The launcher defaults to Chrome's `FFmpegVideoDecoder`. Earlier X11 pixel
captures showed incorrect post-seek pictures in the hardware path. Two driver
lifecycle bugs have since been fixed and regression-tested: repeated video
processing retains the correct decoded picture, and surface reuse between
parameter submission and completion cannot select a different picture.
Missing or retired pictures now report errors instead of unrelated fallback
pixels. These tests do not establish complete hardware drain or actual browser
seek correctness; both still require end-to-end validation. Software decoding
remains the browser default until that validation passes.

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

The default software-decoding path keeps Chrome's GPU sandbox enabled and does
not force the CrystalHD VA-API driver, even if an older persistent configuration
contains the sandbox acknowledgement. Chrome's GPU sandbox does not broker
`/dev/crystalhd` or this out-of-tree VA-API driver. Only experimental hardware
decoding uses `--disable-gpu-sandbox`, and it requires
`CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1`. Renderer, network, and other
browser-process sandboxes remain enabled, but graphics and video parsing in
the experimental GPU process are unsandboxed.

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

Software playback needs no sandbox acknowledgement. Experimental hardware
playback is refused until `CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1` is set.

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
media-timeline regression, player error, or Chrome media error. The default
observation window is 90 seconds; success also requires healthy video and
recent frame/time progress at the end, not just a few seconds of earlier output.
Add `--seek-at 20 --seek-to 120` to perform a real timeline jump and fail if a
pre-seek frame timestamp is reported again after the new timeline has settled.
This callback-timestamp check does not independently establish visible pixel
identity; use the local barcode probe for exact pixels and their own timestamps.
Use `--force-h264` only as a diagnostic fallback on a profile where the
managed extension is not installed.

Omit both the experimental environment variable and `--expect-hardware` to
verify the safe `FFmpegVideoDecoder` path.

The live YouTube failure tracked in
[#12](https://github.com/ahnhy1324/crystalhd/issues/12) reports
`ump.spsrejectfailure` / `HTML5_SPS_UMP_STATUS_REJECTED`, including with
extensions disabled and software decoding. HTTP 200 segment responses and a
clean decoder error log do not make this service/player rejection a pass.
Do not hide automation indicators or bypass service verification to make the
probe green. A normal browser-session check and service-side troubleshooting
are separate from validating the CrystalHD decoder.

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
