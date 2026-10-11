# Using CrystalHD

This guide answers three questions: which frontend to use, how to prove that
the card is doing the decode, and which options are safe to change. It covers
the installed stack. Source-tree test commands belong in the component guides
and hardware measurements belong in the
[hardware report](HARDWARE-2026-09-13.md).

Start with every tuning option unset. Change one only for the measured problem
described in its section below.

## Choose a path

| Goal | Path | Current status |
| --- | --- | --- |
| Play a local file with audio, subtitles and controls | `crystalhd-play` | Recommended |
| Build a media pipeline | GStreamer `crystalhddec` | Primary integration path |
| Use an application that requires libva | CrystalHD VA-API driver | Experimental subset |
| Keep an application written for the Broadcom API working | `libcrystalhd.so.3` | Compatibility API |
| Use VLC | VA-API, selected H.264/X11 setup only | Focused compatibility result |
| Accelerate Chromium, YouTube or Moonlight | VA-API research path | Not recommended for daily use |

The card decodes video. Audio decoding, subtitles, synchronization and display
are provided by the frontend and the rest of the Linux media stack. Only one
CrystalHD hardware playback session can be open at a time.

## Install and verify

For a new direct installation, install the dependencies listed in the
[quick-install section](README.md#quick-install), then run:

```sh
make -j"$(nproc)"
sudo make install
sudo modprobe crystalhd
crystalhd-check
gst-inspect-1.0 crystalhddec
```

If DKMS already owns the module, do not install a direct module over it. Follow
[README.dkms](README.dkms), install the runtime separately, and run
`crystalhd-check` after every update.

These checks have deliberately different meanings:

- `crystalhd-check` checks device access and the installed/loaded module,
  library and plugin identities. It does not decode a frame.
- `gst-inspect-1.0 crystalhddec` proves that GStreamer can find the plugin. It
  does not open the card.
- `vainfo` proves that libva can initialize a driver. It does not prove
  compressed-video decoding.
- `crystalhd-play --hardware FILE` requires real CrystalHD video output and
  fails visibly instead of silently selecting a software video decoder.

For targeted preflight checks, `crystalhd-check --drm-device PATH` selects one
render node, `--module PATH` compares a particular module file with the loaded
driver, and `--source-tree DIR` compares an already built checkout with the
installed stack. All three remain read-only.

## Play a local file

Hardware mode is the default:

```sh
crystalhd-play video.mp4
crystalhd-play --hardware video.mp4
crystalhd-play --subtitles subtitles.srt video.mkv
```

A successful hardware run reports
`Decoder: crystalhddec (hardware; video output confirmed)` and a nonzero
decoded-video-frame count. Use software mode only for an intentional
comparison; it is not an automatic fallback:

```sh
crystalhd-play --software video.mp4
```

Controls are:

| Key | Action |
| --- | --- |
| Space | Pause or resume |
| Left / `j` | Seek backward 10 seconds |
| Right / `l` | Seek forward 10 seconds |
| `1`, `2`, `3` | Select 0.5x, 1x or 2x playback |
| `q` | Quit |

Playback-rate controls exercise the whole player. They do not mean that every
resolution can be decoded at 2x in real time; in particular, 1080p60 and
1080p30 at 2x are beyond the measured card/output limit.

## GStreamer pipelines

These installed-system examples explicitly name `crystalhddec`, so GStreamer
cannot replace it with a software video decoder. They are video-only examples;
use `crystalhd-play` when normal container audio and subtitles are wanted.

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
is not accepted directly. The [GStreamer guide](filters/gst/gst-plugin-1.0/README.md)
defines exact framing rules and source-tree validation commands.

## Codec boundaries

The following table describes the recently tested BCM70015 subset, not every
bitstream allowed by a codec standard.

| Codec | Validated subset | Important exclusions or limits |
| --- | --- | --- |
| H.264/AVC | Selected Constrained Baseline, Main and High streams through Full HD; selected MBAFF TFF/BFF fixtures | Maximum coded size 1920x1088; no PAFF, pulldown or arbitrary-stream claim |
| MPEG-2 | Progressive Simple through 720x576, progressive Main through 1920x1088, and selected TFF/BFF/hard-telecine Main fixtures | No broad profile, chroma, DVD-navigation or interlace conformance claim |
| MPEG-4 Part 2 | Progressive Simple and Advanced Simple levels 3/5 through 1920x1088 | No GMC/sprites, quarter-pixel, interlace, data partitioning or RVLC |
| VC-1 | Advanced progressive whole-frame/BDU and documented ASF framing | No interlaced/field-picture, pan-scan or range-mapping claim |
| WMV3 | Standard Simple/Main frame packets | Nonstandard metadata and multi-resolution variants are outside the subset |

HEVC, VP8, VP9 and AV1 are not supported. BCM70012 retains its legacy H.264
path and ABI, but it has not been recently retested on physical hardware. On
BCM70012, the VA backend rejects the other codecs; non-H.264 GStreamer behavior
is unvalidated and MPEG-4 is explicitly rejected. For the full contracts, see
the [GStreamer](filters/gst/gst-plugin-1.0/README.md) and
[VA-API](filters/vaapi/README.md) component guides.

## VA-API and external clients

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
add checkout-specific `LIBVA_DRIVERS_PATH` or `LD_LIBRARY_PATH` values, because
that can mix stale and installed components.

One VLC 3.0.21 H.264 path was validated on the reference X11/i915 system:

```sh
LIBVA_DRIVER_NAME=crystalhd \
vlc --avcodec-hw=vaapi --vout=gl --gl=egl_x11 \
  --glconv=glconv_vaapi_drm video.mp4
```

VLC can choose another decoder, so this command alone is not proof of
CrystalHD use. While it is playing, `sudo fuser -v /dev/crystalhd` should show
the client. This result does not certify other VLC versions, codecs, Wayland
paths or GPUs.

## Expected performance

The reference host is a BCM70015 on a dedicated Intel ICH8 PCIe Gen1 x1 link,
Ubuntu 25.10 and Linux 6.17. Results on another host can differ. The two
decode/download capacity rows below were measured with the host-specific L0s
workaround already active; their ranges span strict VA replay and the
experimental live plus low-latency mode. They are not ordinary default-setting
results or display-rate claims. The capacity fixture was synthetic progressive
H.264 with no B pictures and one reference picture.

| Workload | Reference result | Meaning |
| --- | --- | --- |
| H.264 720p30 playback | 8 hours, 864,000/864,000 pictures, EOS and controls passed | Validated ordinary playback target |
| H.264 1080p30 playback | A 2-hour complete clocked run; a separate 900-picture throughput run reached 41.20 fps with the host L0s workaround | Conditional target; test the default link first |
| Synthetic 1280x720 decode/download | About 82-83 fps | Capacity result, not proof of 60 Hz presentation |
| Synthetic 1920x1080 decode/download | About 41-42 fps | 1080p60 and 1080p30 at 2x are not practical |

The 8-hour 720p30 result and its counted controls used clocked/headless sinks;
they do not by themselves establish physical display or speaker behavior. In
the recorded synthetic comparison, the decoder process used roughly
61-84% CPU with CrystalHD and 161-181% with software, where 100% means one
logical CPU. CrystalHD used more process memory and software completed the
unpaced batch faster. These numbers do not measure total-system power, and no
power-consumption claim is made. On the primary host, physical 1080p30
display/speaker playback also passed pause/resume, seeking and 0.5x-to-1x
controls with perceptually acceptable synchronization; no numerical A/V-sync
tolerance was measured. See the
[current result matrix](HARDWARE-2026-09-13.md#current-result-matrix) for exact
fixtures and measurement boundaries.

## Options

### Normal-use options

| Option | Default and scope | Use |
| --- | --- | --- |
| `crystalhd-play --hardware` | Default; one player run | Require CrystalHD output; no silent software fallback |
| `crystalhd-play --software` | Off; one player run | Deliberate software comparison |
| `crystalhd-play --subtitles FILE` | None; one player run | Load an external subtitle file |
| `LIBVA_DRIVER_NAME=crystalhd` | Unset; one process | Select CrystalHD explicitly for a VA test |
| `LEGACY_CPU=1` | Off; build and install time | Build 32-bit scalar userspace for i686 CPUs without SSE2; pass it to both `make` and `make install` |

The GStreamer decoder has no CrystalHD-specific element properties. The VA
backend negotiates event-driven library/driver output readiness automatically
when available and retains its compatibility fallback; there is no event-RX
tuning switch.

### Full-HD PCIe workaround

`force_l0s_off=1` is an opt-in, host-specific BCM70015 workaround. The default
is off. Try it only when measured 1080p30 decoding is below real time on a
direct, dedicated root-port link. It can increase power use, and an unsupported
PCIe topology causes probe to fail rather than changing a shared link.

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
requesting the workaround for a temporary test, close clients and safely
reload without the parameter:

```sh
sudo modprobe -r crystalhd
sudo modprobe crystalhd
```

The reference host's raw-fallback path restores its saved L0s bits during a
normal unload. A PCI-core-owned disable is intentionally sticky and may remain
after module removal, so a parameter value of `N` does not by itself prove
that the link returned to its prior state. Reboot without the option to return
to the machine's boot-time PCIe policy, and inspect both endpoint and root-port
`LnkCtl` values with `lspci -vv` when an exact before/after comparison matters.

Make the option persistent only after a repeatable benefit is established, by
placing `options crystalhd force_l0s_off=1` in
`/etc/modprobe.d/crystalhd.conf`. Remove that entry and reboot to undo the
persistent request. Do not use global `pcie_aspm=force`, link-speed changes or
payload-size tweaks as substitutes.

### Experimental library and VA options

Leave these unset for ordinary local files.

| Variable | Accepted value and default | Scope and measured purpose |
| --- | --- | --- |
| `CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT` | `8` or `16`; default 16 | All library clients. Eight buffers reduced measured peak RSS by 11.7%, with no repeatable speed gain and less buffering margin. |
| `CRYSTALHD_VAAPI_LIVE_H264` | Exact `1` enables; unset is the bounded default; any other set value selects strict replay | BCM70015 H.264 VA contexts; BCM70012 is unchanged. Experimental long-running/gap-tolerant transport; the tail needs later input or explicit context drain. |
| `CRYSTALHD_VAAPI_LOW_LATENCY_H264` | Exact `1`; requires live mode | BCM70015 H.264 VA contexts. Reduced one tested low-delay 720p30 input-to-output interval by about one frame cadence; it is not an A/V-sync guarantee. |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC` | Exact `1`; default off | Reference-host i915 VA-owned surfaces only. A narrow readback test improved by 3.9%; this is not a general latency option. |

Reconstructed H.264 frame-rate signaling remains fixed at 30 fps, so the
live/low-latency pair must not be presented as a supported 60-fps mode.

The following controls are diagnostic experiments, not application settings:

| Variable | Meaning |
| --- | --- |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP=1` | Returns an older completed picture through the requested surface/timestamp. This deliberately violates standard VA surface identity. |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY=previous|latest` | Unset/`previous` is the default; `latest` selects the newest eligible older picture. Invalid values disable remap. |
| `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US=0|16667|33333|50000` | Unset/`0` is the default. Nonzero values add remap eligibility delay; invalid values disable remap. |
| `CRYSTALHD_VAAPI_DEBUG` | Any presence, including `0`, enables verbose stderr and may dump the first bytes of access units. |
| `CRYSTALHD_VAAPI_TRACE=1` | Emits JSONL timing/identity traces to stderr and can perturb timing. |

Ready remap additionally requires live mode, low-latency mode, H.264 and
BCM70015. It is not appropriate for normal playback because frame identity is
part of the VA contract. Remove experimental settings with `unset VARIABLE`;
do not place them in a login profile unless deliberately running a test.

### Chromium launcher

`crystalhd-chromium` defaults to software video decoding. Experimental
hardware mode requires both
`CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` and
`CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1`; the latter disables a browser
security boundary. It is therefore a test-only path, not a daily-use recipe.
`CRYSTALHD_DRM_DEVICE` selects its display render node, and
`CRYSTALHD_CHROMIUM_NATIVE_GL=1` stops the launcher from forcing software GL.
`CRYSTALHD_CHROMIUM_H264_POLICY=1` only acknowledges/suppresses a launcher
warning; it does not install or enforce a browser policy.

For a temporary test profile/workload only:

```sh
test_profile=$(mktemp -d /tmp/crystalhd-browser.XXXXXX)
CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1 \
CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1 \
CRYSTALHD_DRM_DEVICE=/dev/dri/renderDXXX \
CRYSTALHD_CHROMIUM_PROFILE_DIR="$test_profile" \
  crystalhd-chromium
rm -r -- "$test_profile"
```

Do not use this sandbox-disabled mode for ordinary or untrusted browsing.

## Troubleshooting and proof of hardware use

Use this order rather than changing several options at once:

1. Run `crystalhd-check` and resolve any direct/DKMS, loaded/installed identity
   or device-permission warning.
2. Run `sudo fuser -v /dev/crystalhd`. Close the existing owner; one card
   supports one hardware session.
3. Check the plugin path printed by `gst-inspect-1.0 crystalhddec` and remove
   stale source-tree `GST_PLUGIN_PATH` or `LD_LIBRARY_PATH` overrides.
4. Use `crystalhd-play --hardware FILE` for the clearest no-fallback result.
5. Confirm that the input is inside the codec/profile/framing table above.
6. Only for a repeatable Full-HD throughput shortfall, test
   `force_l0s_off=1` by itself.
7. Inspect current-boot kernel messages for CrystalHD faults or firmware
   timeouts, then follow the [bring-up guide](BRINGUP.md).

After closing a player, `sudo fuser -v /dev/crystalhd` should report no owner
and `cat /sys/module/crystalhd/refcnt` should report zero. Discovery alone is
not a decode result; a complete file, EOS and a clean reopen provide the useful
lifecycle check.

## Further documentation

- [README](README.md): adoption summary, installation and project scope
- [DKMS installation](README.dkms): kernel-package ownership and updates
- [Bring-up guide](BRINGUP.md): staged diagnosis and safe module identity checks
- [GStreamer guide](filters/gst/gst-plugin-1.0/README.md): accepted framing and counted tests
- [VA-API guide](filters/vaapi/README.md): implemented API subset and replay contract
- [libcrystalhd guide](linux_lib/libcrystalhd/README.md): legacy ABI and direct-library behavior
- [Hardware report](HARDWARE-2026-09-13.md): fixtures, measured evidence and limits
- [TODO](TODO.md): current work rather than historical milestones
