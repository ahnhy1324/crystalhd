# Everyday CrystalHD usage

For ordinary local playback, use **1.3.0 with `crystalhd-play` after its official
release is qualified and published**. This guide targets final 1.3/module20;
select the exact released core and actual qualification receipt using
[source selection](README.md#quick-start). A source version is not a publication
announcement; use the immutable 1.2/module13 fallback if the release is unavailable.
Leave tuning options at their defaults for the first run.

Optional features below retain their existing opt-in defaults. Prior RC4/module19,
published RC3/module18 and RC2/module17 keep separate frozen artifacts/results.

1. [Install and verify the stack](#verify-the-installed-stack).
2. [Play a local file](#play-a-local-file), then use the [controls](#player-controls)
   and [audio/subtitle instructions](#audio-and-subtitles).
3. If playback fails, use the [symptom table](#troubleshooting-and-proof-of-hardware-use).

For another application, see [Choose a path](#choose-a-path). The
[documentation index](DOCUMENTATION.md) covers installation, settings and
advanced guides. Sunshine/Moonlight has its own [integration guide](MOONLIGHT.md).

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
example path with your own file:

```sh
crystalhd-play "/path/to/video.mp4"
```

Hardware mode is the default; adding `--hardware` requests it explicitly.
A successful run reports
`Decoder: crystalhddec (hardware; video output confirmed)` and a nonzero
decoded-video-frame count. Software mode is an intentional comparison, not a
fallback:

```sh
crystalhd-play --software "/path/to/video.mp4"
```

## Player controls

Run `crystalhd-play` in a terminal and keep that terminal focused when using
these keys. The ordinary video window does not handle the Player's terminal
shortcuts. Redirected/non-terminal input disables them and prints a notice.

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

## Audio and subtitles

The Player uses GStreamer's normal audio output; choose the output device and
volume in your desktop mixer. CrystalHD accelerates video decoding, while the
host decodes audio and mixes subtitles. If video plays without sound, check the
mixer and the required audio codec plugin before changing decoder options.

For an external local subtitle file:

```sh
crystalhd-play --subtitles "/path/to/subtitles.srt" "/path/to/video.mkv"
```

The same option works with explicit `--hardware` or `--software` playback.

## Saved settings (1.3 tools)

Stable 1.2.0 does not include the new configuration editors. With matching
1.3 tools and updated components installed, use the optional desktop window
or inspect defaults from the terminal. For final 1.3/module20 use the matching
release core/settings artifacts after publication, or its exact private binding
manifest during preparation. [Matching installation](README.md#install-matching-13-tools)
keeps final and previous candidate versions separate:

```sh
crystalhd-settings
crystalhd-config show-effective
```

If the window is not installed, follow [desktop settings installation](CONFIGURATION.md#optional-desktop-settings).
Choose **Player** for decoder/output-size defaults, then **Save changes**.
Restart the Player after saving. Explicit command-line flags override their
corresponding saved values. A smaller card output reduces detail; leave native
size selected unless you need that tradeoff.

Kodi users can use the [optional Kodi settings add-on](addons/script.crystalhd.settings/README.md).
After saving there, restart Kodi and keep its own VA-API decoder selection
enabled. These editors change shared CrystalHD defaults; a saved setting alone
does not establish active hardware decoding. See [configuration](CONFIGURATION.md)
for profiles, reset and exact precedence. Follow
[matching settings installation](README.md#install-matching-13-tools) when
installing 1.3 components. The optional GTK editor's intermittent Ubuntu24
assertion/SIGSEGV remains [unresolved](https://github.com/ahnhy1324/crystalhd/issues/623);
clean tests and diagnostic capture do not establish a repair. The terminal
editor does not require GTK.

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
| No sound | Check the desktop mixer/output and the audio codec's GStreamer plugin; compare the same file with `--software`. CrystalHD decodes video only. |
| Keyboard controls do nothing | Run the player in a terminal and focus that terminal; see [controls](#player-controls). |
| Slow native Full-HD output | Choose [smaller card output](#optional-bcm70015-output-scaling) if loss of detail is acceptable, or evaluate the [session-scoped L0s procedure](#full-hd-pcie-workaround) for a repeatable qualifying shortfall. |
| Black output or a firmware timeout | Stop playback, inspect current-boot kernel messages, and follow [bring-up recovery](BRINGUP.md). For Moonlight, use its [troubleshooting guide](MOONLIGHT.md#troubleshooting). |

After closing a player, `sudo fuser -v /dev/crystalhd` should report no owner
and `cat /sys/module/crystalhd/refcnt` should report zero. Discovery alone is
not a decode result; a complete file, EOS and a clean reopen provide the useful
lifecycle check.

## Choose a path

| Goal | Path | Current status |
| --- | --- | --- |
| Play a local file with audio, subtitles and controls | `crystalhd-play` | Recommended |
| Play an HTTP(S) media URL | [HTTP(S) player input](#https-input) | Selected loopback HTTP/private-CA HTTPS checked; broader network and physical playback remain unverified |
| Build a media pipeline | GStreamer `crystalhddec` | Primary integration path |
| Use an application that requires libva | CrystalHD VA-API driver | Experimental subset |
| Keep a Broadcom-API application working | `libcrystalhd.so.3` | Compatibility API |
| Use VLC | VA-API, selected H.264/X11 setup only | Focused tested combination |
| Use Kodi | VA-API on the reference X11 host | Progressive playback; selected 1.2 CPU Bob/Weave on Kodi 21.3, with cadence limits below |
| Stream from Sunshine with Moonlight | [Moonlight guide](MOONLIGHT.md) | First-class experimental integration |
| Accelerate Chromium or YouTube | VA-API browser experiment | Hardware path unverified; not daily use |

The card decodes video. Audio decoding, subtitles, synchronization and display
are provided by the frontend and the rest of the Linux media stack. Only one
CrystalHD hardware decode session can be open at a time.

## HTTP(S) input

HTTP(S) input is included in 1.1.0 and its RC4/RC5 candidates; the earlier
1.0.0 and v1.1.0-rc.3 do not contain it. With the matching Player installed,
replace the example URL with your direct media URL and close other CrystalHD
sessions first:

```sh
crystalhd-play --hardware 'https://media.example.test/video.mp4'
crystalhd-play --software 'http://media.example.test/video.mp4'
```

For source-tree testing, run `./scripts/crystalhd-play` from that checkout's
root. That wrapper prefers its built plugin/library when present; it is not
a check of the installed payload.

The URL is passed to GStreamer without rewriting its escaping or query string.
HTTP transport needs `souphttpsrc` (Ubuntu: `gstreamer1.0-plugins-good`); HTTPS
keeps the transport's normal certificate verification. This is not a web-page
or YouTube URL resolver, and HLS/DASH have not been qualified. Codec limits and
the hardware-output requirement remain unchanged: a URL does not enable
software fallback. `--subtitles` still takes a local file.

The controller handles [GStreamer network buffering](https://gstreamer.freedesktop.org/documentation/playback/playbin.html#buffering)
without overriding a user pause. Seek and speed changes require a seekable
stream/container and server support; a rejected operation leaves the accepted
playback settings unchanged. Existing no-output limits remain 20 seconds at
startup and 15 seconds after decoded output stalls, including network buffering
(a deliberate user pause is exempt). Errors exit nonzero; there is no automatic
reconnect. Close and restart the player to reconnect.

Automated network coverage uses loopback HTTP and private-CA HTTPS with software
video and fake audio/video sinks. HTTPS keeps strict verification enabled:
the selected trusted CA permits playback/EOS, while an unrelated CA or a wrong
hostname is rejected before decoded frames. These tests select trust through
the transport API and do not change system trust. BCM70015 GStreamer hardware
loopback HTTP playback also passed selected 360p and 720p30 H.264 video, EOS and
local-file reopen with fake sinks.

The [post-RC3 current-host checks](https://github.com/ahnhy1324/crystalhd/pull/652)
also passed private-CA HTTPS through the legacy GStreamer hardware decoder:
two 180-picture/EOS plays on module13 with unchanged RC3 decoder/DIL artifacts,
followed by untrusted-CA and wrong-hostname rejection with zero frames in
separate fresh processes. This does not qualify the optional native
V4L2/FFmpeg route. The original mixed-trust same-process attempt remains FAIL:
a previously trusted TLS session resumed, so it was not a fresh certificate
verification control. The tests do not establish changing custom trust policy
inside a long-running embedding application.

Default-CA/Internet HTTPS, hardware network seeking, physical A/V and Internet
recovery remain unverified. These scoped checks do not replace the qualification
of a particular published package or justify updating a working installation
without testing.

## Interlaced video

### Explicit Weave playback

`--weave` was introduced in 1.1.0/RC5 and remains in stable 1.2.0.
Use the [stable installation guide](README.md#quick-start) if needed. The earlier `v1.1.0-rc.5` tag remains available for historical reproduction; use the guide shipped with that tag and retain its rollback source.

After installation, select the decoder and pass a complete interlaced
frame through without GStreamer's automatic deinterlacing:

```sh
crystalhd-play --hardware --weave "/path/to/interlaced-video.mkv"
crystalhd-play --software --weave "/path/to/interlaced-video.mkv"
```

Hardware means CrystalHD decoding; software means CPU decoding with hardware
video decoders disabled. Both retain audio and the normal player controls.
Hardware mode never silently falls back to software. Weave keeps the two
fields in alternating rows; it is not hardware Bob or a deinterlacing filter,
so motion can show combing/shaking. Without `--weave`, the player's existing
deinterlacing policy is unchanged. Selected H.264 MBAFF playback is the bounded
RC scope; deinterlacing quality and A/V synchronization tuning are deferred,
not certified by successful playback. See [codec boundaries](#codec-boundaries).

If hardware mode fails, start with the
[symptom table](#troubleshooting-and-proof-of-hardware-use).

### Application deinterlacing after Weave

Weave is the complete interlaced frame supplied by the decoder. An application
can then deinterlace that frame in software; this does not require a CrystalHD
hardware deinterlacer. In `crystalhd-play`, **omit `--weave`** to allow
GStreamer's existing automatic deinterlacing policy:

```sh
crystalhd-play --hardware "/path/to/interlaced-video.mkv"
crystalhd-play --software "/path/to/interlaced-video.mkv"
```

Audio, subtitles and player controls stay available. The player has no CLI
option selecting a particular filter such as BWDIF. For explicit selection,
use [GStreamer's filter](#gstreamer-deinterlacing),
[FFmpeg BWDIF](#interlaced-h264-weave-and-ffmpeg-bwdif), or
[Kodi's Deinterlace method](#kodi). These filters use CPU time and may increase
output cadence; successful decoding alone does not certify smooth display or
A/V synchronization on every host.

## Optional BCM70015 output scaling

Use this option when Full-HD output is bandwidth-limited and a smaller image
is acceptable. On BCM70015, the card can decode a progressive Full-HD input
and return fewer pixels over PCIe. Detail is lost during that reduction;
enlarging the image afterward does not recover it. Native-size output remains
the default when no saved player override exists.

These examples keep the input at 1920x1080, reduce the card output to
1280x720, and optionally resize it in software to a 1920x1080 display buffer:

```sh
# Return 1280-wide YUY2 from a progressive Full-HD input.
crystalhd-play --hardware --scale-width 1280 "/path/to/video-1080p.mp4"

# Then use GStreamer videoscale to produce an exact 1920x1080 display buffer.
crystalhd-play --hardware --scale-width 1280 \
  --software-output-size 1920x1080 "/path/to/video-1080p.mp4"
```

**1.3 tools only:** updated players also accept
[saved player defaults](CONFIGURATION.md#player-defaults).
Explicit flags override only their corresponding saved setting. Use
`--scale-width 0 --software-output-size native` to clear saved scale/resize values
for one invocation; add `--software` for software decoding. The saved file stays intact.

Expect `Decoder: crystalhddec (hardware; video output confirmed)`, a nonzero
decoded-video-frame count and playback through the end of the file. Compare
motion and detail with the native run. For negotiated dimensions, use the
verbose [GStreamer scaling example](filters/gst/gst-plugin-1.0/README.md#optional-bcm70015-output-scaling).
To return to native output on stable 1.2.0, omit both scaling options in a new
invocation. With 1.3 saved defaults, use
`--scale-width 0 --software-output-size native`; the `native` reset value is
1.3-only. Without saved scaling defaults, omit both options.

Measured examples and their exact source/host boundaries are in the
[component scaling observations](filters/gst/gst-plugin-1.0/README.md#retained-player-scaling-observations).

`--scale-width` accepts `0` for native output or any even width from 128 through
1918 that does not exceed the coded width. The card derives an even output
height from the source geometry; the plugin reports the actual geometry and a
pixel aspect ratio that preserves the source display aspect ratio. The width is
selected for each playback session and survives flush/reopen. It is not fixed
at 1280, and it cannot be changed while a pipeline is playing.

The hardware option is rejected for BCM70012. Published 1.1 supports progressive
scaling only. With the matching **1.2 plugin and player**, selected H.264 MBAFF
interlaced input can also use a multiple-of-16 width:

```sh
crystalhd-play --hardware --scale-width 1280 "/path/to/video-1080i.mkv"
```

The card returns the smaller fields; the host still performs deinterlacing.
Add `--weave` to preserve their interlaced lines instead. The player's selected
deinterlacer may differ from the explicit GStreamer `scalerbob` used in the
[bounded FHD-to-720 comparison](HARDWARE-2026-09-13.md#card-scaled-interlaced-h264-and-host-bob-2026-10-20).
Other interlaced codecs, nonaligned widths and scaled whole-Weave payloads are
not supported by this new path. This does not add card-side Bob.
VA clients have a separate, explicit
[decode-integrated scaling API](filters/vaapi/README.md#development-decode-integrated-card-scaling);
ordinary FFmpeg/Kodi VPP calls do not select it automatically.

The optional
`--software-output-size WIDTHxHEIGHT` stage requires a nonzero hardware scale
width and performs an explicit bounded GStreamer resize with aspect-preserving
borders. Both dimensions must be positive and even, neither may exceed 4096,
and their product may not exceed 4096x2160 pixels. It can resize either up or
down; resizing up does not restore detail discarded by the card. Omitting it
lets the normal video sink fit the smaller decoded image to its window or
display.

Letting the video sink fit the smaller picture avoids the explicit CPU
enlargement stage. Its interpolation depends on the display adaptor; use
`--software-output-size` only when you need that exact raw-buffer size.

For this enlargement path, the 1.2 Player can pack pixels at the
smaller size when its selected Xv output accepts UYVY but not YUY2. This avoids
repacking a full-size picture on the display thread; no extra option is needed.
Deinterlacing still receives YUY2. Custom video sinks, Weave and uncertain
output negotiation retain the previous format choice.

The matching 1.2 Player automatically handles the documented GStreamer
1.28.2 YUY2 deinterlacer/QoS case; no extra user flag is needed. Exact
conditions and limits are in the component observations linked above.


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

Framing and compressed-picture admission limits depend on the frontend. See
[GStreamer input contracts](filters/gst/gst-plugin-1.0/README.md) or the
[VA H.264 admission notes](#va-api-h264-admission) when supported-size input
is rejected. An unsupported file can be played explicitly with `--software`.

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

### GStreamer deinterlacing

For a known interlaced H.264 MP4, explicitly place GStreamer's built-in CPU
`deinterlace` element after CrystalHD's complete Weave output:

```sh
gst-launch-1.0 -e \
  filesrc location="/path/to/interlaced-video.mp4" ! qtdemux ! h264parse ! \
  crystalhddec ! deinterlace mode=interlaced method=linear fields=all tff=auto ! \
  videoconvert ! autovideosink
```

`method=linear` selects linear interpolation; `tff=auto` follows the decoder's
field-order metadata. `fields=all` ordinarily produces 50 progressive outputs/s
from 25 complete interlaced pictures/s. This is a video-only example, not a
newly measured playback/performance result. Check locally available methods
with `gst-inspect-1.0 deinterlace`; the
[element reference](https://gstreamer.freedesktop.org/documentation/deinterlace/index.html)
describes the choices. Remove the `deinterlace ... !` stage for plain Weave;
do not select `method=weave` as an interpolation filter. For normal playback
with audio, use the player above or Kodi instead.

## VA-API, FFmpeg, Kodi and VLC

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

### Interlaced H.264: Weave and FFmpeg BWDIF

For the selected H.264 MBAFF field-pair subset, CrystalHD returns two hardware
fields and the VA driver combines their alternating lines into one complete
interlaced NV12 **Weave** frame. Weave preserves both fields; it is not
deinterlacing, so motion can show normal combing.

The earlier, unpublished RC.2 payload `f4d99d5` was checked with a 30-second,
1920x1080i25 TFF fixture. Direct VA readback completed all 750 input pictures
twice. Every luma sample matched the
software decoder; the deterministic same-field chroma conversion measured
`U 40.96 dB` and `V 38.85 dB` PSNR. This is a selected-fixture result, not a
claim for PAFF, arbitrary interlaced streams or physical picture quality.

FFmpeg can deinterlace after downloading that Weave frame. This is the exact
shape of the validated BWDIF path; replace the quoted input and render node:

```sh
drm_node=/dev/dri/renderDXXX
LIBVA_DRIVER_NAME=crystalhd \
ffmpeg -hide_banner -hwaccel vaapi -hwaccel_device "$drm_node" \
  -hwaccel_output_format vaapi -i "/path/to/interlaced-video.ts" \
  -map 0:v:0 -filter_threads 2 \
  -vf 'hwdownload,format=nv12,format=yuv420p,bwdif=mode=send_field:parity=auto:deint=all' \
  -fps_mode passthrough -an -sn -f null -
```

This null-output command verifies decode, readback and filtering; it does not
play audio or display the result. `send_field` ordinarily turns 25 complete
interlaced pictures/s into 50 progressive frames/s.

Installed **RC.3** on the reference Core 2 Duo T8100 / FFmpeg 8.0.1 was timed
on 2026-10-16 without input pacing, audio, display or checksums, using two
filter threads. Existing `force_l0s_off=1`, `session_l0s_off=0` settings kept
L0s disabled; no PCIe power settings were changed for this measurement.
Each 30-second H.264 MBAFF fixture contains 750 pictures;
all three repetitions returned the expected 750 or 1,500 outputs:

| Path | 1920×1080i25 output fps | 1280×720i25 output fps |
| --- | ---: | ---: |
| Weave, NV12 readback | 39.29 | 80.39 |
| Weave, converted to YUV420p | 38.23 | 79.96 |
| CPU BWDIF `send_frame` | 32.27 | 70.22 |
| CPU BWDIF `send_field` | 56.60 | 122.05 |

These are achieved rates from median complete-process wall time, not the
media's nominal 25/50 fps. Field-rate BWDIF processed 28.30 FHD input
pictures/s, about 13% above this clip's 25-picture/s requirement before
audio/display work. This is useful headroom, not a guarantee of smooth Kodi
presentation or a pure hardware-decoder maximum. Decode, readback, format
conversion, CPU filtering and process overhead are included; no per-frame
filter latency or physical A/V offset was measured. See the
[hardware results](HARDWARE-2026-09-13.md#rc3-interlaced-null-output-throughput-2026-10-16)
for elapsed time, CPU, memory and measurement limits.
That section also separates a RAM-backed CPU-filter cost comparison from
hardware throughput; its whole-batch times are not live per-frame latency.

The earlier unpublished RC.2 checksum run separately produced 1,500/1,500
frames plus EOS in 33.95 seconds, at 157% process CPU and 325,296 KiB maximum
RSS. It used `framemd5` and the older runtime; it is not the current null-output
benchmark or a directly comparable filter-cost measurement.
On published 1.1.0, `deinterlace_vaapi=mode=bob` is not a substitute: that release
does not advertise a VA VPP deinterlacing filter. The bounded stable 1.2
implementation is described separately in the
[VA component contract](filters/vaapi/README.md#development-cpu-va-vpp-bob).
For one progressive BWDIF output per input picture instead, use `mode=send_frame`;
this reduces output cadence, not the underlying decode work, and has different
motion quality. The [BWDIF reference](https://ffmpeg.org/ffmpeg-filters.html#bwdif)
defines its mode/parity options. These commands filter on the CPU after
hardware readback, not inside the card.

### Kodi

The historical 1.1.0-rc.3 and fresh installed 1.1.0-rc.4 checks passed stock Kodi 21.3
progressive H.264 hardware-video and application-lifecycle checks on the
reference X11 host ([RC4 results](HARDWARE-2026-09-13.md#110-rc4-qualification--2026-10-18)). Kodi selected
`ff-h264-vaapi` through the installed CrystalHD VA driver and played a
120-second 1080p30 file to natural EOS; separate control runs passed seek,
pause/resume, stop/reopen and quit. The qualified strict/default run had all
live, low-latency, READY-remap and diagnostic experiments unset. Start an
installed driver without checkout-specific library paths:

```sh
unset CRYSTALHD_VAAPI_LIVE_H264 CRYSTALHD_VAAPI_LOW_LATENCY_H264
unset CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP
unset CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY
unset CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US
unset CRYSTALHD_VAAPI_DEBUG CRYSTALHD_VAAPI_TRACE
LIBVA_DRIVER_NAME=crystalhd kodi
```

Enable VA-API hardware acceleration in Kodi's video settings. During playback,
verify both CrystalHD/`ff-h264-vaapi` in Kodi's debug log and an open hardware
owner with `sudo fuser -v /dev/crystalhd`; the UI setting alone does not prove
hardware decoding. The installed 1.2.0-rc.2 matrix passed qualitative dialogue
A/V and controls for 1080p30 progressive and 720i25 Auto/Off; calibrated
flash/click offset was not measured. Other
Kodi releases and display backends are not generally qualified; stock Kodi 22
has the [interlaced compatibility limits above](#known-issues-and-deinterlacing-roadmap).

Retained source-scoped centered Off diagnostics under original Kodi22RC1 add selected
full NV12 seek-target pixels and Stop/natural reopen. The separate progressive
CPU-copy case confirms that route's selection. A separate observer joins raw
returned AVFrame PTS, with runtime time bases still unset. These isolated OWN diagnostic builds do
not qualify all post-copy bytes, interlaced BWDIF, GPU return or release
compatibility. See the [current observation scope](filters/vaapi/README.md#development-experiment-centered-card-output)
and [remaining frontend targets](FRONTEND-ACCEPTANCE.md).

For the selected interlaced subset in Kodi 21.3, keep **VAAPI hardware
acceleration enabled** for CrystalHD decoding. While playing, open the cog
icon → **Video settings → Deinterlace method**:

- **Off** leaves the decoded Weave frame unfiltered.
- **Deinterlace** explicitly selects Kodi's existing CPU BWDIF path after VA
  readback; this is not driver-side Bob or VPP deinterlacing.
- **Auto select** requests CrystalHD's CPU VA Bob on the selected 1.2 path.
  Kodi's adaptive field omission means this does not guarantee uniform 50p.
  On published 1.1.0, Bob is unavailable and Auto may repeat unsupported probes.

Use **Off/NONE** for Weave. Kodi may still perform an initial VPP capability
probe even when deinterlacing is off. With the 1.1 driver and Auto on interlaced
pictures, some Kodi versions repeat unsupported probes once per picture
([#445](https://github.com/ahnhy1324/crystalhd/issues/445)).

These explicit-filter observations are for Kodi 21.3 on the reference host,
not a recommendation to select BWDIF/Render Bob on the reporter's Atom/Kodi 22
build. Method availability and labels depend on the Kodi version/backend; see the
[playback settings](https://kodi.wiki/view/Video_playback#Video_Settings) and
[Kodi 21.3 implementation](https://github.com/xbmc/xbmc/blob/21.3-Omega/xbmc/cores/VideoPlayer/DVDCodecs/Video/VAAPI.cpp).
Decoder selection is separate from filter selection: for a CPU-only control,
disable VAAPI (and VDPAU if offered), then close and reopen the file. Changing
only the deinterlace method does not switch decoding to software. These are
per-player choices, not settings applied by `crystalhd-config`.

The v1.1 RC includes selected interlaced Weave playback, not general
deinterlacing or full-rate FHD support. The earlier `f4d99d5` Kodi stalls are
preserved in the hardware report. During 1.1.0-rc.3 preparation, the later finite
MBAFF input/tail fix completed all 750 decoder-oracle pictures with verified
pixels/PTS and EOS, and passed live input-gap/explicit-close/reopen checks.
Kodi traversed the selected full file but could discard pictures or resync
under load; some FHD runs retained timing/render warnings and a bounded
post-EOS empty-output wait. These are historical results, not fresh 1.3 RC4
qualification. No incomplete surface is returned to hide a wait.
For an explicit HW/SW Weave selection use the
[player commands above](#explicit-weave-playback). Kodi's VA-API acceleration
setting selects hardware versus CPU decode; its deinterlacing method is a
separate setting. Published 1.1.0 advertises no VA driver
deinterlacing capability; deinterlacing and sync tuning remain
tracked in [#436](https://github.com/ahnhy1324/crystalhd/issues/436).

During 1.1.0-rc.3 preparation (recorded in `a3fd609`), a selected 30-second 720i25 dialogue clip had
subjectively aligned speech/lip motion in both no-deinterlace and explicit
`Deinterlace` (BWDIF) comparisons; BWDIF reduced the visible shaking. Both
ended normally. This is a useful playback check, not measured millisecond
sync, a complete displayed-frame census or FHD support qualification.
The CPU/no-deinterlace control showed similar shaking without using CrystalHD.
These observations are not new 1.3 RC4 playback or physical-sync measurements.

### VLC

The historical Ubuntu 25.10 / VLC 3.0.21 H.264 check used this X11/i915 path:

```sh
LIBVA_DRIVER_NAME=crystalhd \
vlc --avcodec-hw=vaapi --vout=gl --gl=egl_x11 \
  --glconv=glconv_vaapi_drm "/path/to/video.mp4"
```

VLC can choose another decoder, so this command alone is not proof of
CrystalHD use. While it plays, `sudo fuser -v /dev/crystalhd` should show the
client. This result does not certify other VLC versions, codecs, Wayland paths
or GPUs. On the newer LTS host, VLC 3.0.23 selected CrystalHD VA/EGL and closed
cleanly but logged late pictures; see the
[dated LTS result](HARDWARE-2026-09-13.md#new-lts-qualification--2026-10-13).
Neither progressive result establishes VLC interlaced/deinterlacing support.
The fresh 1.1.0-rc.4 installation also passed two complete 12-second VLC 3.0.23
hardware plays with EOS, process reopen and clean teardown; this bounded
lifecycle check does not certify cadence or physical A/V.

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

## Known issues and deinterlacing roadmap

- **Weave is not deinterlacing.** Hardware and software Weave retain both
  fields, so moving edges can show combing or shaking. Stable 1.2 provides
  the bounded CPU VA-VPP Bob path; native hardware Bob is not supported.
  Applications can also apply
  [CPU deinterlacing after Weave](#application-deinterlacing-after-weave),
  with additional CPU cost; successful decode does not guarantee smooth FHD
  presentation or calibrated A/V sync.
- **Kodi interlaced playback is narrowly bounded in stable 1.2.** Off selects
  Weave; Auto can select the bounded CPU VA-VPP Bob path. Earlier 1.1.0 lacked
  that filter and could repeatedly probe for it. The selected installed 1.2 RC2
  matrix with Kodi 21.3 Auto/Off completed decode, recorded A/V controls, seek,
  natural EOF and reopen; stable reuse applies only to unchanged implementation.
  Kodi's adaptive `SKIPDEINT` requests omit some second
  fields, so this is not an uninterrupted 50p or general Kodi 21–23 claim.
  Atom real-time playback also remains unsupported
  ([#445](https://github.com/ahnhy1324/crystalhd/issues/445),
  [#429](https://github.com/ahnhy1324/crystalhd/issues/429)). LIVE/LOW_LATENCY
  remains a separate experimental path.
- **Known issue: Kodi VAAPI finite-EOF accounting.** In the observed Kodi 21.3
  output path, a picture can be published before its processed counter is
  retired. Diagnostic receiver-priority scheduling exposed an approximately
  two-second drain timeout at natural EOF; the normal-scheduling control
  reached EOF. Other builds and ordinary-workload frequency remain unverified.
  This is a scoped Kodi publication/accounting issue; a general VA-API or
  CrystalHD-driver defect is not established. See
  [#445](https://github.com/ahnhy1324/crystalhd/issues/445) and the
  [stock observation](tests/fixtures/issue445/publish-order-observation-20261029.json).
  The external patch has been removed under
  [project policy](EXTERNAL-CODE-POLICY.md); historical modified-Kodi results
  cannot qualify stock compatibility or a release.
- **Known issue: Kodi 22 RC1 BWDIF filter tail.** The observed original method 7
  route frees its software filter without sending the filter source EOF.
  Consequently its last input picture's two progressive outputs remain held,
  separately from any pictures skipped earlier under decoder load. Correct
  CrystalHD pixels cannot flush an application-owned filter graph. The
  [source and actual-call evidence](tests/fixtures/issue477/native900-20261104.json)
  records this scoped limitation; a separate OWN client sending explicit EOF
  does not qualify lossless stock Kodi EOF. The full 900-source investigation
  remains open in [#477](https://github.com/ahnhy1324/crystalhd/issues/477) and
  [#436](https://github.com/ahnhy1324/crystalhd/issues/436).
- **Oversized H.264 rejection is containment, not general stream recovery.**
  Stable 1.2 includes the transport-specific headroom introduced in 1.1 RC5
  and rejects selected over-budget extended input
  before submission and preserves a clean reopen; it intentionally rejects
  some input that previously worked. Exact firmware reclamation remains a
  measured signed-firmware/configuration limitation; use software decode for
  rejected content. See the [admission limits](#codec-boundaries).
- **Legacy client CPU requirements still apply.** On the reporter's Atom N280
  / i386 / Kodi 22 build, Render Bob (6) crashed with SIGILL in
  `libsse4-i486-linux.so`. Deinterlace (7) was not separately run on that host;
  its compatibility is not established. The driver's `LEGACY_CPU=1` build
  cannot remove ISA requirements from applications or their dependencies.

The remaining work in [#436](https://github.com/ahnhy1324/crystalhd/issues/436)
targets broader readback/filter integration, field order, cadence and client
acceptance beyond the selected stable 1.2 scope. Source-qualified no-drift logs
satisfy A/V acceptance for the exact route; physical cadence/scanout is separate
research. The existing renderer/QoS policy does not require universal zero loss.
Hardware deinterlacing remains separate from the published CPU implementation.

Stable `1.2.0` preserves the bounded path qualified for its RC2:
[CPU VA-VPP Bob path](filters/vaapi/README.md#development-cpu-va-vpp-bob),
plus the focused completed-source lifetime correction. It is not card-side
deinterlacing or a general Kodi/Atom playback path.

### VA-API H.264 admission

Stable 1.2 and the 1.3 source inherit the admission/recovery guards introduced
in `v1.1.0-rc.4`; immutable `v1.1.0-rc.3` predates them.
For VA-API, the conservative baseline is a **2 MiB assembled H.264 access-unit submission
limit**, including reconstructed headers. Units beyond the applicable limit
are rejected before hardware/replay admission. Rejected reference pictures require a later actual
IDR before dependent input resumes; rejecting a non-reference B picture does
not block valid continuation. This policy is not the chip's
physical maximum, and the separate 8 MiB software allocation bound does not
establish 8 MiB hardware decoding. Units above 512 KiB require a matching
libcrystalhd with `DtsProcInputFragment`.

Historical 1.1.0-rc.5 includes the [bounded admission changes](https://github.com/ahnhy1324/crystalhd/pull/459),
not present in published 1.1.0-rc.4. Development controls qualified **3,014,656 bytes**
for identified BCM70015/DIL 3.22.0 progressive frame-coded input. Unknown/BCM70012/interlaced
paths retain 2 MiB. With the identified older Flea PES library, complete AUs up
to **768 KiB** can use a fully reserved legacy call; larger units still need
the fragment API. This restores the tested original/padded Sintel paths, not
arbitrary oversized-input recovery or a silicon maximum. Exact tested code
and headless results are in the [qualification summary](HARDWARE-2026-09-13.md#110-rc4-qualification--2026-10-18);
the completed RC5 package results are retained in [#430](https://github.com/ahnhy1324/crystalhd/issues/430).

For finite H.264 files, leave LIVE and LOW_LATENCY unset. Since RC5, the backend
automatically prefeeds eligible progressive/MBAFF frame pictures and avoids premature EOS
while a fully submitted large AU is still outstanding. This is bounded
finite-input handling, not an automatic streaming mode or deinterlacer. A
genuine large-AU tail may take up to one second of inactivity inside Sync calls
before EOS; ready output is not deliberately delayed. Explicit LIVE still
requires subsequent input or an actual context drain for its reordered tail.
See the [VA replay/EOS contract](filters/vaapi/README.md#replay-and-end-of-stream).

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

## Experimental outputs and diagnostics (1.3)

These are optional source-build features. Ordinary stable 1.2.0 playback uses
the earlier commands without these output selectors. Published 1.3.0-rc.3
includes the optional outputs and additions below. Final1.3/module20 retains
them and RC4's Player/Xv stability fixes, with unchanged opt-in defaults. Use
the final release's own qualification receipt for actual results. Prior RC4/module19
and frozen RC3/RC2 payloads are not replaced by later source changes; their actual
qualification and later affected checks retain separate attribution.

### Experimental Xv output and packed enlargement

`--video-output xv-async` selects the separately built
[completion-controlled X11 sink](filters/gst/xv-output/README.md). Keep `auto`
for ordinary output selection. RC3 also offers
`--video-output xv-async --xv-latest-frame`, which intentionally replaces
unsent pictures under congestion. FIFO remains the default; the retained
comparison demonstrates no CPU/latency benefit on the selected noncongested route.

`--packed-upscale` selects a separate experimental packing/enlargement stage
for its exact supported geometry and GStreamer version. Follow the
[packed-upscale recipe](filters/gst/packed-upscale/README.md) for required flags,
plugin build and qualification. Neither option is a saved setting or a general
performance recommendation.

### Experimental GPU field output

The optional 1.3 research Player accepts `--video-output field-gpu` to
select the explicitly built `crystalhdfieldsink`. It performs GPU nearest Bob
and display fitting for limited-range YUY2 with BT.601/709 colorimetry, and
disables playbin's automatic deinterlacing. This requires the research plugin
and its XCB/EGL/GLES2/GBM/DRM dependencies in an X11 session. It is not part
of an ordinary install.
See the [field-output build, invocation and qualification limits](tests/fixtures/issue530/README.md#experimental-window-consumer).

Frozen RC2 provides the original YUY2 field output; set an embedding window
before READY. RC3 adds local-display admission, setup/response deadlines,
direct NV12 input and live window replacement. The additions below are
included in RC3, beyond the frozen RC2 implementation.

The default `--video-output auto` keeps the normal output path. The selector is
per invocation and is not saved by the configuration manager. With `field-gpu`,
omit `--weave` and use `--software-output-size native` to clear any saved
software resize; either conflicting request is rejected before playback.

<a id="current-master-additions"></a>
<a id="rc3-additions"></a>

#### Field-output additions included in RC3

Direct raw-video input additionally accepts explicitly declared limited-range
NV12 within its [plane/field qualification](tests/fixtures/issue530/README.md#nv12-input--2026-11-03).
The existing CrystalHD hardware Player route continues negotiating YUY2.

Remote/TCP DISPLAY is rejected. Local X11 setup and explicit atom/geometry/
window-handoff response waits have bounded deadlines. A rejected read-only
handoff keeps the current attachment; an uncertain mutation instead reports an
error and closes the worker. These guards do not bound all EGL, startup, resize
or shutdown work. The [component deadline contract](tests/fixtures/issue530/README.md#x11-response-deadlines-current-implementation)
records the exact budgets and remaining scopes.

The experimental sink accepts `GstVideoOverlay.set_window_handle()` during
PLAYING or PAUSED. Keep the new X11 target and its ancestors alive while it is
selected. After successful synchronous replacement, a previous parent outside
that ancestry may be destroyed.
Handle0 restores an owned standalone window. Invalid targets preserve the
current output; `field-stats` reports `window-handle`, `window-replacements`
and `window-rejections`. The GL context and retained field imports survive the
change. This does not broaden format, live-input or physical display claims.

### Optional video-stage trace (development)

For a focused frame-flow investigation with the matching 1.3 native helper,
request a new JSON file:

```sh
crystalhd-play --trace-frames /tmp/playback-trace.json "/path/to/video.mp4"
crystalhd-play --software --trace-frames /tmp/software-trace.json "/path/to/video.mp4"
```

The output file must not already exist. Tracing is opt-in and can be incomplete;
records describe pipeline handoffs rather than screen presentation. The
[component trace contract](filters/gst/gst-plugin-1.0/README.md#player-video-stage-trace)
explains bounded retention, `--trace-limit`, `--trace-window`, stages, counters
and interpretation.

### Reading Player output counts

The development Player also reports available video-sink rendered/dropped
buffer counts before closing. These counters are separate from decoded
pictures: deinterlacing can produce two output buffers per picture, and seeks
or state changes can reset sink counters. They are a snapshot before teardown,
not physical display counts or a new playback pass criterion. A renderer
without these statistics reports `Video sink statistics: unavailable`.

The development `crystalhd-play` warns once if GStreamer reports dropped video
buffers. Its decoded-frame counter measures decoder-pad output, not displayed
frames;
natural EOS alone does not prove complete presentation. For lower-cost display
fitting, keep `--scale-width` and omit `--software-output-size`. The
[measured frame-flow investigation](HARDWARE-2026-09-13.md#player-frame-flow-2026-10-25)
separates decoder, transform and sink QoS loss from card/firmware failure.

## Advanced option index

These are not required for ordinary local playback. Their owning component
documents the exact behavior and limitations. The
[complete configuration inventory](CONFIGURATION.md) records every current
setting's owner, default, dependencies, lifetime and safety level.

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
