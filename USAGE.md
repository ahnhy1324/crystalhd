# Everyday CrystalHD usage

This guide covers installed local playback and common application paths. Start
with every tuning option unset. Check the [README compatibility tables](README.md#compatibility-at-a-glance)
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

Hardware mode is the default:

```sh
crystalhd-play video.mp4
crystalhd-play --hardware video.mp4
crystalhd-play --subtitles subtitles.srt video.mkv
```

A successful hardware run reports
`Decoder: crystalhddec (hardware; video output confirmed)` and a nonzero
decoded-video-frame count. Software mode is an intentional comparison, not a
fallback:

```sh
crystalhd-play --software video.mp4
```

| Key | Action |
| --- | --- |
| Space | Pause or resume |
| Left / `j` | Seek backward 10 seconds |
| Right / `l` | Seek forward 10 seconds |
| `1`, `2`, `3` | Select 0.5x, 1x or 2x playback |
| `q` | Quit |

The controls exercise the whole player; they do not imply that every
resolution can be decoded at every playback rate. In particular, 1080p60 and
1080p30 at 2x are beyond the measured output limit.

## GStreamer pipelines

These installed-system examples explicitly name `crystalhddec`, so GStreamer
cannot replace it with a software video decoder. They are video-only examples;
use `crystalhd-play` for ordinary container audio and subtitles.

H.264 in MP4:

```sh
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! videoconvert ! autovideosink
```

MPEG-2 elementary video:

```sh
gst-launch-1.0 filesrc location=video.m2v ! mpegvideoparse ! \
  crystalhddec ! videoconvert ! autovideosink
```

MPEG-4 Part 2 in MP4:

```sh
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! mpeg4videoparse ! \
  crystalhddec ! videoconvert ! autovideosink
```

WMV3 in ASF/WMV:

```sh
gst-launch-1.0 filesrc location=video.wmv ! asfdemux ! \
  crystalhddec ! videoconvert ! autovideosink
```

Raw VC-1 Advanced requires caps matching the stream:

```sh
gst-launch-1.0 filesrc location=video.vc1 ! \
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
  -hwaccel_output_format vaapi -i video.mp4 \
  -vf hwdownload,format=nv12 -an -sn -f null -
```

The FFmpeg command submits and downloads decoded frames; it does not test
speaker output or physical display cadence. For an installed driver, do not
add checkout-specific `LIBVA_DRIVERS_PATH` or `LD_LIBRARY_PATH` values.

One VLC 3.0.21 H.264 path was validated on the reference X11/i915 system:

```sh
LIBVA_DRIVER_NAME=crystalhd \
vlc --avcodec-hw=vaapi --vout=gl --gl=egl_x11 \
  --glconv=glconv_vaapi_drm video.mp4
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
- 1080p60 and 1080p30 at 2x are not practical on the current output path.
- CrystalHD reduced decoder-process CPU in the reference comparison, while
  software completed unpaced work faster and used less memory. Whole-system
  power was not measured.

Exact fixtures, rates, resource use and A/V qualifications remain in the
[hardware report](HARDWARE-2026-09-13.md#current-result-matrix).

## Full-HD PCIe workaround

`force_l0s_off=1` is an opt-in, host-specific BCM70015 workaround. The default
is off. Try it only when measured 1080p30 decoding is below real time, or when
a measured 720p60 decode lacks sufficient headroom, on a direct, dedicated
root-port link. The post-event/DMA cross-check still measured 27.33 versus
43.04 fps at 1080p30 and 57.5 versus 87.0 fps in an unpaced 720p60 control with
L0s enabled versus disabled. It can increase power use, and an unsupported PCIe
topology causes probe to fail rather than changing a shared link.

Close every client first, then perform a temporary test:

```sh
sudo fuser -v /dev/crystalhd
cat /sys/module/crystalhd/refcnt
sudo modprobe -r crystalhd
sudo modprobe crystalhd force_l0s_off=1
cat /sys/module/crystalhd/parameters/force_l0s_off
crystalhd-check
```

Continue only if `fuser` reports no owner and the reference count is zero;
never force-unload the module. The parameter should report `Y`. To stop
requesting the workaround, close clients and safely reload without it:

```sh
sudo modprobe -r crystalhd
sudo modprobe crystalhd
```

The reference host's raw-fallback path restores saved L0s bits during a normal
unload. A PCI-core-owned disable is intentionally sticky and may remain after
module removal, so `force_l0s_off=N` does not prove the link returned to its
prior state. Reboot without the option to return to the machine's boot-time
PCIe policy; compare endpoint and root-port `LnkCtl` values with `lspci -vv`
when an exact before/after check matters.

Make the request persistent only after a repeatable benefit is established, by
placing `options crystalhd force_l0s_off=1` in
`/etc/modprobe.d/crystalhd.conf`. Remove the entry and reboot to undo it. Do not
use global `pcie_aspm=force`, link-speed changes or payload-size tweaks.

## Advanced option index

These are not required for ordinary local playback. Their owning component
documents the exact behavior and limitations.

| Need | Control | Where to read first |
| --- | --- | --- |
| Reduce library RX-buffer memory | `CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT=8` | [libcrystalhd optional RX pool](linux_lib/libcrystalhd/README.md#optional-output-readiness-and-rx-pool) |
| Long-running live H.264 input | `CRYSTALHD_VAAPI_LIVE_H264=1` | [Moonlight guide](MOONLIGHT.md) and [VA replay contract](filters/vaapi/README.md#replay-and-end-of-stream) |
| Reduce decode latency by about one frame interval in a tested 720p30 stream | `CRYSTALHD_VAAPI_LOW_LATENCY_H264=1` with live mode | [Moonlight guide](MOONLIGHT.md) |
| Test nonstandard older-ready-frame remapping | `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP=1` plus policy/delay controls | Diagnostic only; [VA guide](filters/vaapi/README.md#replay-and-end-of-stream) |
| Test reference-host i915 write combining | `CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC=1` | [VA surface contract](filters/vaapi/README.md#surface-and-export-contracts) |
| Collect VA debug or JSONL trace output | `CRYSTALHD_VAAPI_DEBUG`, `CRYSTALHD_VAAPI_TRACE=1` | [VA diagnostic controls](filters/vaapi/README.md#diagnostic-controls) |
| Exercise the Chromium launcher | `CRYSTALHD_CHROMIUM_*` | [Chromium experiment](filters/vaapi/README.md#chromium-launcher-experiment) |

Unset experimental variables after a test; do not put them in a login profile.
The negotiated event-driven VA receive path is automatic and has no user
tuning switch.

## Troubleshooting and proof of hardware use

Use this order instead of changing several options at once:

1. Run `crystalhd-check` and resolve direct/DKMS, loaded/installed identity or
   device-permission warnings.
2. Run `sudo fuser -v /dev/crystalhd`. Close the existing owner; one card
   supports one hardware session.
3. Check the filename printed by `gst-inspect-1.0 crystalhddec` and remove stale
   source-tree `GST_PLUGIN_PATH` or `LD_LIBRARY_PATH` overrides.
4. Use `crystalhd-play --hardware FILE` for the clearest no-fallback result.
5. Confirm that the input fits the codec/profile/framing table above.
6. Only for a repeatable Full-HD throughput shortfall, test
   `force_l0s_off=1` by itself.
7. Inspect current-boot kernel messages for CrystalHD faults or firmware
   timeouts, then follow the [bring-up guide](BRINGUP.md).

After closing a player, `sudo fuser -v /dev/crystalhd` should report no owner
and `cat /sys/module/crystalhd/refcnt` should report zero. Discovery alone is
not a decode result; a complete file, EOS and a clean reopen provide the useful
lifecycle check.
