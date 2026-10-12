# Everyday CrystalHD usage

This guide covers installed local playback and common application paths. Start
with every tuning option unset. Check [README compatibility](README.md#compatibility)
before choosing the card for a workload; Sunshine/Moonlight has a separate
[integration guide](MOONLIGHT.md).

## Choose a path

| Goal | Path | Current status |
| --- | --- | --- |
| Play a local file with audio, subtitles and controls | `crystalhd-play` | Recommended |
| Build a media pipeline | GStreamer `crystalhddec` | Primary integration path |
| Use an application that requires libva | CrystalHD VA-API driver | Experimental subset |
| Keep a Broadcom-API application working | `libcrystalhd.so.3` | Compatibility API |
| Use VLC | VA-API, selected H.264/X11 setup only | Focused tested combination |
| Stream from Sunshine with Moonlight | [Moonlight guide](MOONLIGHT.md) | First-class experimental integration |
| Accelerate Chromium or YouTube | VA-API browser experiment | Hardware path unverified; not daily use |

The card decodes video. Audio decoding, subtitles, synchronization and display
are provided by the frontend and the rest of the Linux media stack. Only one
CrystalHD hardware decode session can be open at a time.

## Verify the installed stack

Install the stack with the [README quick start](README.md#quick-start), or use
[README.dkms](README.dkms) when DKMS owns the module. Then run:

```sh
crystalhd-check
gst-inspect-1.0 crystalhddec
```

These checks have different meanings:

- `crystalhd-check` checks the PCI device, access, firmware and the
  installed/loaded module, library and plugin identities. It does not decode a
  frame.
- `gst-inspect-1.0 crystalhddec` proves that GStreamer can find the plugin. It
  does not open the card.
- `vainfo` proves that libva can initialize a driver. It does not prove
  compressed-video decoding.
- `crystalhd-play --hardware FILE` requires real CrystalHD video output and
  fails visibly instead of silently selecting a software video decoder.

For targeted read-only checks:

| Preflight option | Purpose |
| --- | --- |
| `crystalhd-check --drm-device PATH` | Select one DRM render node for VA discovery |
| `crystalhd-check --module PATH` | Compare a module file with the loaded driver |
| `crystalhd-check --source-tree DIR` | Compare an already built checkout with the installed stack |

## Play a local file

For a first run, choose a progressive H.264 8-bit 4:2:0 MP4 at 720p30 or
smaller. The `.mp4` extension alone does not identify the codec. Replace the
example paths with your own files; hardware mode is the default:

```sh
crystalhd-play "/path/to/video.mp4"
crystalhd-play --hardware "/path/to/video.mp4"
crystalhd-play --subtitles "/path/to/subtitles.srt" "/path/to/video.mkv"
```

A successful hardware run reports
`Decoder: crystalhddec (hardware; video output confirmed)` and a nonzero
decoded-video-frame count. Software mode is an intentional comparison, not a
fallback:

```sh
crystalhd-play --software "/path/to/video.mp4"
```

If hardware mode fails, start with the
[symptom table](#troubleshooting-and-proof-of-hardware-use).

| Key | Action |
| --- | --- |
| Space | Pause or resume |
| Left / `j` | Seek backward 10 seconds |
| Right / `l` | Seek forward 10 seconds |
| `1`, `2`, `3` | Select 0.5x, 1x or 2x playback |
| `q` | Quit |

The controls exercise the whole player; they do not imply that every
resolution can be decoded at every playback rate. Native Full-HD output at
60 fps, including 1080p30 at 2x, is beyond the measured output limit.
[Smaller card output](#optional-bcm70015-output-scaling) can improve throughput
for Full-HD input at the cost of detail.

## Optional BCM70015 output scaling

Use this option when Full-HD output is bandwidth-limited and a smaller image
is acceptable. On BCM70015, the card can decode a progressive Full-HD input
and return fewer pixels over PCIe. Detail is lost during that reduction;
enlarging the image afterward does not recover it. Native-size output remains
the default.

These examples keep the input at 1920x1080, reduce the card output to
1280x720, and optionally resize it in software to a 1920x1080 display buffer:

```sh
# Return 1280-wide YUY2 from a progressive Full-HD input.
crystalhd-play --hardware --scale-width 1280 "/path/to/video-1080p.mp4"

# Then use GStreamer videoscale to produce an exact 1920x1080 display buffer.
crystalhd-play --hardware --scale-width 1280 \
  --software-output-size 1920x1080 "/path/to/video-1080p.mp4"
```

Expect `Decoder: crystalhddec (hardware; video output confirmed)`, a nonzero
decoded-video-frame count and playback through the end of the file. Compare
motion and detail with the native run. For negotiated dimensions, use the
verbose [GStreamer scaling example](filters/gst/gst-plugin-1.0/README.md#optional-bcm70015-output-scaling).
To return to native output, close the player and launch it without either
scaling option; the choices are not saved between invocations.

With the reference host's qualified L0s workaround active, the H.264 High
stream rose from about 46 to 65 unpaced frames/s with 1280-wide card output.
Software enlargement back to Full HD retained
about 64 frames/s and used about 84% of one CPU core. This is useful headroom
for the measured input, not native 1080p60 detail or a displayed-frame-rate
guarantee. Other codecs and hosts have different limits; exact throughput and
quality results are in the
[hardware report](HARDWARE-2026-09-13.md#full-hd-input-with-decoder-coupled-scaling-2026-10-11).

`--scale-width` accepts `0` for native output or any even width from 128 through
1918 that does not exceed the coded width. The card derives an even output
height from the source geometry; the plugin reports the actual geometry and a
pixel aspect ratio that preserves the source display aspect ratio. The width is
selected for each playback session and survives flush/reopen. It is not fixed
at 1280, and it cannot be changed while a pipeline is playing.

The hardware option is rejected for BCM70012 and interlaced input. The optional
`--software-output-size WIDTHxHEIGHT` stage requires a nonzero hardware scale
width and performs an explicit bounded GStreamer resize with aspect-preserving
borders. Both dimensions must be positive and even, neither may exceed 4096,
and their product may not exceed 4096x2160 pixels. It can resize either up or
down; resizing up does not restore detail discarded by the card. Omitting it
lets the normal video sink fit the smaller decoded image to its window or
display.

## GStreamer pipelines

These installed-system examples explicitly name `crystalhddec`, so GStreamer
cannot replace it with a software video decoder. They are video-only examples;
use `crystalhd-play` for ordinary container audio and subtitles.

H.264 in MP4:

```sh
gst-launch-1.0 filesrc location="/path/to/video.mp4" ! qtdemux ! h264parse ! \
  crystalhddec ! videoconvert ! autovideosink
```

MPEG-2 elementary video:

```sh
gst-launch-1.0 filesrc location="/path/to/video.m2v" ! mpegvideoparse ! \
  crystalhddec ! videoconvert ! autovideosink
```

MPEG-4 Part 2 in MP4:

```sh
gst-launch-1.0 filesrc location="/path/to/video.mp4" ! qtdemux ! mpeg4videoparse ! \
  crystalhddec ! videoconvert ! autovideosink
```

WMV3 in ASF/WMV:

```sh
gst-launch-1.0 filesrc location="/path/to/video.wmv" ! asfdemux ! \
  crystalhddec ! videoconvert ! autovideosink
```

Raw VC-1 Advanced requires caps matching the stream:

```sh
gst-launch-1.0 filesrc location="/path/to/video.vc1" ! \
  'video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=none,width=176,height=144,framerate=25/1' ! \
  crystalhddec ! videoconvert ! autovideosink
```

Replace the dimensions and frame rate with facts from the input. Raw WMV3 RCV
is not accepted directly. The [GStreamer component guide](filters/gst/gst-plugin-1.0/README.md)
defines exact framing rules and counted test procedures.

## Codec boundaries

This is the tested BCM70015 subset, not every bitstream allowed by each codec
standard.

| Codec | Validated subset | Important exclusions or limits |
| --- | --- | --- |
| H.264/AVC | Selected Constrained Baseline, Main and High through Full HD; selected MBAFF TFF/BFF fixtures | Maximum coded size 1920x1088; no PAFF, pulldown or arbitrary-stream claim |
| MPEG-2 | Progressive Simple through 720x576, progressive Main through 1920x1088, and selected TFF/BFF/hard-telecine Main fixtures | No broad profile, chroma, DVD-navigation or interlace conformance claim |
| MPEG-4 Part 2 | Progressive Simple and Advanced Simple levels 3/5 through 1920x1088 | No GMC/sprites, quarter-pixel, interlace, data partitioning or RVLC |
| VC-1 | Advanced progressive whole-frame/BDU and documented ASF framing | No interlaced/field-picture, pan-scan or range-mapping claim |
| WMV3 | Standard Simple/Main frame packets | Nonstandard metadata and multi-resolution variants are outside the subset |

HEVC, VP8, VP9 and AV1 are not supported. BCM70012 has no recent physical
retest: its legacy H.264 path remains, its VA backend rejects the other codecs,
and MPEG-4 is explicitly rejected by GStreamer. See the GStreamer and
[VA-API](filters/vaapi/README.md) guides for the full contracts.

## VA-API, FFmpeg and VLC

VA-API is an experimental client-integration subset, not the preferred local
player. Choose the display GPU render node reported by `crystalhd-check`; do
not assume it is always `renderD128`.

```sh
drm_node=/dev/dri/renderDXXX
LIBVA_DRIVER_NAME=crystalhd \
  vainfo --display drm --device "$drm_node"

LIBVA_DRIVER_NAME=crystalhd \
ffmpeg -hide_banner -hwaccel vaapi -hwaccel_device "$drm_node" \
  -hwaccel_output_format vaapi -i "/path/to/video.mp4" \
  -vf hwdownload,format=nv12 -an -sn -f null -
```

The FFmpeg command submits and downloads decoded frames; it does not test
speaker output or physical display cadence. For an installed driver, do not
add checkout-specific `LIBVA_DRIVERS_PATH` or `LD_LIBRARY_PATH` values.

One VLC 3.0.21 H.264 path was validated on the reference X11/i915 system:

```sh
LIBVA_DRIVER_NAME=crystalhd \
vlc --avcodec-hw=vaapi --vout=gl --gl=egl_x11 \
  --glconv=glconv_vaapi_drm "/path/to/video.mp4"
```

VLC can choose another decoder, so this command alone is not proof of
CrystalHD use. While it plays, `sudo fuser -v /dev/crystalhd` should show the
client. This result does not certify other VLC versions, codecs, Wayland paths
or GPUs.

## Realistic expectations

- 720p30 local playback is the best-established target. Its long clocked and
  control tests do not by themselves certify every display/speaker path.
- 1080p30 completed sustained playback on the reference host only after its
  documented PCIe L0s workaround was enabled.
- Synthetic 720p decode/download exceeded 60 fps with that host workaround,
  but this is not a general 60 Hz display claim.
- Native Full-HD output at 60 fps, including 1080p30 at 2x, is not practical
  on the current path. The optional reduced-output mode above exceeded
  60 frames/s on selected Full-HD H.264 input, with reduced detail.
- CrystalHD had lower average decoder-process CPU utilization in the reference
  unpaced comparison, while software decoded faster and used less memory.
  Different output rates do not establish a per-frame CPU saving; whole-system
  power was not measured.

Exact fixtures, rates, resource use and A/V qualifications remain in the
[hardware report](HARDWARE-2026-09-13.md#current-result-matrix).

## Full-HD PCIe workaround

Consider this only for a repeatable native 1080p30 throughput shortfall or
insufficient 720p60 decode headroom. Disabling PCIe L0s saved enough transfer
time to restore real-time Full-HD playback on the reference host. It may
increase power use during playback; whole-system wattage was not measured.
The [hardware report](HARDWARE-2026-09-13.md#pcie-l0s-isolation-2026-09-27)
retains the comparisons.

Both driver options default to off and require BCM70015 on a direct,
dedicated root-port link. The driver rejects unsupported topology rather than
changing a shared link.

- `session_l0s_off=1` is the preferred first test. It disables L0s while a
  decoder session owns the card and restores both link ends to their captured
  idle values after clean close. Monitor-only opens do not change the link.
  Suspend restores that baseline; resume captures the new baseline before
  reapplying the session override. Existing internal card low-power handling
  remains active.
- `force_l0s_off=1` keeps the existing module-lifetime behavior. Depending on
  PCI-core ownership, its disable policy may remain after module removal. If
  both options are supplied, `force_l0s_off=1` takes precedence.

Close every client, then inspect the loaded and saved options:

```sh
sudo fuser -v /dev/crystalhd
cat /sys/module/crystalhd/refcnt
cat /sys/module/crystalhd/parameters/session_l0s_off
cat /sys/module/crystalhd/parameters/force_l0s_off
modprobe --showconfig | grep -E '^options[[:space:]]+crystalhd[[:space:]]'
cat /proc/cmdline
```

Continue only if `fuser` reports no owner and the reference count is zero.
Never force-unload the module. If `session_l0s_off` is missing, resolve the
[loaded/installed module mismatch](BRINGUP.md#device-access-and-module-identity)
before trying the option. Saved `options crystalhd` lines and
`crystalhd.*` kernel-command-line settings can reapply options on future
loads. If force mode was already enabled, first remove its saved setting and
reboot as described below to obtain a clean baseline.

For a temporary session-mode test, explicitly disable the force option:

```sh
sudo modprobe -r crystalhd
sudo modprobe crystalhd force_l0s_off=0 session_l0s_off=1
cat /sys/module/crystalhd/parameters/session_l0s_off
cat /sys/module/crystalhd/parameters/force_l0s_off
crystalhd-check
```

The parameters should report `Y` then `N`. Repeat the same complete file and
compare playback/throughput; after close, confirm no owner and refcount zero.
For an exact link check, compare endpoint and root-port `LnkCtl` in
`lspci -vv`: L0s is disabled during decode and returns to its saved idle state
after close. Do not change ASPM policy through another tool or service during
the comparison.

To undo the temporary request, close clients and repeat the idle checks above
before reloading with both options off:

```sh
sudo modprobe -r crystalhd
sudo modprobe crystalhd force_l0s_off=0 session_l0s_off=0
```

The session-scoped path uses no PCI-core ASPM-disable call and takes a fresh
snapshot for each decoder session. The force mode's raw-fallback path restores
saved L0s bits during a normal unload, but a PCI-core-owned disable is
intentionally sticky and may remain after module removal. Therefore
`force_l0s_off=N` alone does not prove that the link returned to its prior
state. Reboot without the force option to return to the machine's boot-time
PCIe policy.

Make the request persistent only after a repeatable benefit is established, by
placing `options crystalhd session_l0s_off=1` in
`/etc/modprobe.d/crystalhd.conf`. Keep `force_l0s_off=1` only when its
module-lifetime policy is deliberately required. To undo a persistent choice,
remove only the relevant option from its modprobe configuration or bootloader
configuration, preserving unrelated settings. On Ubuntu, refresh the initramfs
with `sudo update-initramfs -u` after changing saved module options; update the
bootloader if its command line was changed, then reboot. A reload with explicit
zeros affects only that load and does not remove saved options. Do not use
global `pcie_aspm=force`, link-speed changes or payload-size tweaks.

## Advanced option index

These are not required for ordinary local playback. Their owning component
documents the exact behavior and limitations.

| Need | Control | Where to read first |
| --- | --- | --- |
| Reduce library RX-buffer memory | `CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT=8` | [libcrystalhd optional RX pool](linux_lib/libcrystalhd/README.md#optional-output-readiness-and-rx-pool) |
| Long-running live H.264 input | `CRYSTALHD_VAAPI_LIVE_H264=1` | [Moonlight guide](MOONLIGHT.md) and [VA replay contract](filters/vaapi/README.md#replay-and-end-of-stream) |
| Reduce decode latency by about one frame interval in a tested 720p30 stream | `CRYSTALHD_VAAPI_LOW_LATENCY_H264=1` with live mode | [Moonlight guide](MOONLIGHT.md) |
| Try Moonlight's experimental previous-frame mode | `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP=1` plus policy/delay controls | [Moonlight setup and rollback](MOONLIGHT.md#experimental-previous-frame-mode) |
| Test reference-host i915 write combining | `CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC=1` | [VA surface contract](filters/vaapi/README.md#surface-and-export-contracts) |
| Collect VA debug or JSONL trace output | `CRYSTALHD_VAAPI_DEBUG`, `CRYSTALHD_VAAPI_TRACE=1` | [VA diagnostic controls](filters/vaapi/README.md#diagnostic-controls) |
| Exercise the Chromium launcher | `CRYSTALHD_CHROMIUM_*` | [Chromium experiment](filters/vaapi/README.md#chromium-launcher-experiment) |

Unset experimental variables after a test; do not put them in a login profile.
The negotiated event-driven VA receive path is automatic and has no user
tuning switch.

## Troubleshooting and proof of hardware use

Start with `crystalhd-check`, then follow the matching symptom. Change one
setting at a time.

| Symptom | First check and next action |
| --- | --- |
| No card or `/dev/crystalhd` | Check device/module/firmware results in `crystalhd-check`; follow [device recovery](BRINGUP.md#device-access-and-module-identity). |
| Permission denied | Check the access result in `crystalhd-check`; use the documented `video` group/udev setup and sign in again after group changes. See [device access](BRINGUP.md#device-access-and-module-identity). |
| Device busy | Run `sudo fuser -v /dev/crystalhd` and close the existing owner normally; one card supports one hardware session. Follow [bring-up recovery](BRINGUP.md) if ownership remains after exit. |
| Loaded and installed versions differ | Follow the [idle reload or reboot procedure](BRINGUP.md#reload-an-idle-device) for the installation method that owns your module. |
| Plugin missing or from an old checkout | Check `Filename` in `gst-inspect-1.0 crystalhddec`; remove stale `GST_PLUGIN_PATH`/`LD_LIBRARY_PATH` overrides and follow [installation verification](#verify-the-installed-stack). |
| Unsupported input | Compare the stream's actual codec/profile with [codec boundaries](#codec-boundaries); try the known supported H.264 first-test file or explicit software playback. |
| Slow native Full-HD output | Choose [smaller card output](#optional-bcm70015-output-scaling) if loss of detail is acceptable, or evaluate the [session-scoped L0s procedure](#full-hd-pcie-workaround) for a repeatable qualifying shortfall. |
| Black output or a firmware timeout | Stop playback, inspect current-boot kernel messages, and follow [bring-up recovery](BRINGUP.md). For Moonlight, use its [troubleshooting guide](MOONLIGHT.md#troubleshooting). |

After closing a player, `sudo fuser -v /dev/crystalhd` should report no owner
and `cat /sys/module/crystalhd/refcnt` should report zero. Discovery alone is
not a decode result; a complete file, EOS and a clean reopen provide the useful
lifecycle check.
