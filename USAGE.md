# Everyday CrystalHD usage

This guide covers installed local playback and common application paths. Start
with every tuning option unset. Check [README compatibility](README.md#compatibility)
before choosing the card for a workload; Sunshine/Moonlight has a separate
[integration guide](MOONLIGHT.md).

## Choose a path

| Goal | Path | Current status |
| --- | --- | --- |
| Play a local file with audio, subtitles and controls | `crystalhd-play` | Recommended |
| Play an HTTP(S) media URL | [HTTP(S) player input](#https-input) | Native loopback HTTP checked; HTTPS and physical playback pending |
| Build a media pipeline | GStreamer `crystalhddec` | Primary integration path |
| Use an application that requires libva | CrystalHD VA-API driver | Experimental subset |
| Keep a Broadcom-API application working | `libcrystalhd.so.3` | Compatibility API |
| Use VLC | VA-API, selected H.264/X11 setup only | Focused tested combination |
| Use Kodi | VA-API, progressive H.264 on the reference X11 host | Interlaced post-processing is not yet support-grade |
| Stream from Sunshine with Moonlight | [Moonlight guide](MOONLIGHT.md) | First-class experimental integration |
| Accelerate Chromium or YouTube | VA-API browser experiment | Hardware path unverified; not daily use |

The card decodes video. Audio decoding, subtitles, synchronization and display
are provided by the frontend and the rest of the Linux media stack. Only one
CrystalHD hardware decode session can be open at a time.

## Known issues and deinterlacing roadmap

- **Weave is not deinterlacing.** Hardware and software Weave retain both
  fields, so moving edges can show combing or shaking. Published 1.1.0 does not
  provide native hardware Bob or a VA deinterlacing filter. Applications can
  apply [CPU deinterlacing after Weave](#application-deinterlacing-after-weave),
  with additional CPU cost; successful decode does not guarantee smooth FHD
  presentation or calibrated A/V sync.
- **Kodi 22 interlaced playback remains a compatibility limitation.** Auto can
  repeatedly probe unavailable VA deinterlacing, with stutter, A/V drift and
  increased CPU/memory use. RC4 reduces driver-side setup/allocation overhead
  and fixes VA processing interoperability. The RC4 Atom/i386 report finds
  that explicit Off avoids Auto's repeated probes and allows selected playback
  through EOF/reopen; exact displayed-picture identity and physical A/V remain
  unmeasured ([#445](https://github.com/ahnhy1324/crystalhd/issues/445),
  [#429](https://github.com/ahnhy1324/crystalhd/issues/429)). Off/NONE selects
  Weave, not deinterlacing or a guarantee for every timing problem. Explicit
  LIVE/LOW_LATENCY finite playback still has a known tail-timeout limitation.
- **Oversized H.264 rejection is not general stream recovery.** RC4 passes
  selected synthetic next-IDR controls, but a real B-pyramid rejection case
  returns two stale pictures despite the expected total count. The
  [RC5 changes](https://github.com/ahnhy1324/crystalhd/pull/459) add
  qualified transport-specific headroom and decode that padded case fully;
  generic out-of-policy rejection recovery remains open in
  [#429](https://github.com/ahnhy1324/crystalhd/issues/429). These changes are
  not part of the published RC4. RC5 qualification is separate from those
  development checks; see the [admission limits](#codec-boundaries).
- **Legacy client CPU requirements still apply.** On the reporter's Atom N280
  / i386 / Kodi 22 build, Render Bob (6) crashed with SIGILL in
  `libsse4-i486-linux.so`. Deinterlace (7) was not separately run on that host;
  its compatibility is not established. The driver's `LEGACY_CPU=1` build
  cannot remove ISA requirements from applications or their dependencies.

The planned **v1.2 deinterlacing work** in
[#436](https://github.com/ahnhy1324/crystalhd/issues/436) targets decode/readback/
filter integration, field order, cadence and A/V validation. These are not
capabilities added by published 1.1.0 or a promise that every interlaced input will be
fixed. The limitations above can remain until the relevant implementation and
host-specific validation are complete; hardware deinterlacing remains separate
research.

The separate 1.2 development branch implements a bounded
[CPU VA-VPP Bob path](filters/vaapi/README.md#development-cpu-va-vpp-bob),
not card-side deinterlacing or a newly qualified Kodi/Atom playback path.

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

### HTTP(S) input

HTTP(S) input is included in 1.1.0 and RC4/RC5; the earlier 1.0.0 and
v1.1.0-rc.3 do not contain it. From the selected source checkout, use
its wrapper (or the matching installed `crystalhd-play`). Replace the example URL
with your direct media URL, and close other CrystalHD sessions first:

```sh
./scripts/crystalhd-play --hardware 'https://media.example.test/video.mp4'
./scripts/crystalhd-play --software 'http://media.example.test/video.mp4'
```

The source wrapper prefers the checkout's plugin/library when its built
GStreamer plugin is present; it is not a check of the installed payload.

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

Automated network coverage uses loopback HTTP with software video and fake
audio/video sinks. Native BCM70015 loopback HTTP playback also passed selected
360p and 720p30 H.264 video, EOS and local-file reopen with fake sinks.
Real HTTPS/TLS, native network seeking, physical A/V and Internet
recovery remain unverified. Inclusion in the source is not a release-gate PASS for
those paths or a reason to replace a qualified installation without testing.

### Explicit Weave playback

`--weave` is included in stable 1.1.0 and published RC5, not v1.0.0. Select
the immutable stable release with:

```sh
git clone --branch v1.1.0 --single-branch \
  https://github.com/ahnhy1324/crystalhd.git crystalhd
cd crystalhd
```

Use `v1.1.0-rc.5` instead to select the earlier immutable candidate.

Install the [dependencies and build](README.md#quick-start) from this directory,
then follow the [direct install/update](BRINGUP.md#direct-install-update-and-rollback)
or [DKMS route](README.dkms), according to the existing module owner. Do not
mix the two routes. Keep published tags and the previous rollback source
unchanged.

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

The hardware option is rejected for BCM70012. Published 1.1 supports progressive
scaling only. With the matching **1.2 development plugin and player**, H.264
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

For this enlargement path, the development Player can pack pixels at the
smaller size when its selected Xv output accepts UYVY but not YUY2. This avoids
repacking a full-size picture on the display thread; no extra option is needed.
Deinterlacing still receives YUY2. Custom video sinks, Weave and uncertain
output negotiation retain the previous format choice.

With GStreamer **1.28.2**, this hardware-scaled Player path works around a
confirmed default LINEAR deinterlacer error for YUY2 25-picture/s input and
50-field/s output. It bypasses that element's entire local QoS cache while
forwarding unchanged feedback upstream; decoder, resize/conversion and sink
QoS remain active. Other versions, rates and methods retain stock behavior.
This is not a guarantee of lossless FHD50 presentation; see the
[measured limits](HARDWARE-2026-09-13.md#card720--bob--fhd-physical-playback--2026-10-21).

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

The RC4 candidate includes the following admission and recovery guards; the
immutable `v1.1.0-rc.3` predates them.
The VA backend has a conservative **2 MiB assembled H.264 access-unit submission
limit**, including reconstructed headers. Larger units are rejected before
hardware/replay admission. Rejected reference pictures require a later actual
IDR before dependent input resumes; rejecting a non-reference B picture does
not block valid continuation. This policy is not the chip's
physical maximum, and the separate 8 MiB software allocation bound does not
establish 8 MiB hardware decoding. Units above 512 KiB require a matching
libcrystalhd with `DtsProcInputFragment`.

RC5 includes the [bounded admission changes](https://github.com/ahnhy1324/crystalhd/pull/459),
not present in published RC4. Development controls qualified **3,014,656 bytes**
for identified BCM70015/DIL 3.22.0 progressive frame-coded input. Unknown/BCM70012/interlaced
paths retain 2 MiB. With the identified older Flea PES library, complete AUs up
to **768 KiB** can use a fully reserved legacy call; larger units still need
the fragment API. This restores the tested original/padded Sintel paths, not
arbitrary oversized-input recovery or a silicon maximum. Exact tested code
and headless results are in the [qualification summary](HARDWARE-2026-09-13.md#110-rc4-qualification--2026-10-18);
new RC5 package results are tracked in [#430](https://github.com/ahnhy1324/crystalhd/issues/430).

For finite H.264 files, leave LIVE and LOW_LATENCY unset. RC5 automatically
prefeeds eligible progressive/MBAFF frame pictures and avoids premature EOS
while a fully submitted large AU is still outstanding. This is bounded
finite-input handling, not an automatic streaming mode or deinterlacer. A
genuine large-AU tail may take up to one second of inactivity inside Sync calls
before EOS; ready output is not deliberately delayed. Explicit LIVE still
requires subsequent input or an actual context drain for its reordered tail.
See the [VA replay/EOS contract](filters/vaapi/README.md#replay-and-end-of-stream).

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
does not advertise a VA VPP deinterlacing filter. The bounded 1.2 development
implementation is described separately in the
[VA component contract](filters/vaapi/README.md#development-cpu-va-vpp-bob).
For one progressive BWDIF output per input picture instead, use `mode=send_frame`;
this reduces output cadence, not the underlying decode work, and has different
motion quality. The [BWDIF reference](https://ffmpeg.org/ffmpeg-filters.html#bwdif)
defines its mode/parity options. These commands filter on the CPU after
hardware readback, not inside the card.

### Kodi

Published RC.3 and the fresh installed RC4 confirmation passed stock Kodi 21.3
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
hardware decoding. No physical flash/click A/V PASS was recorded. Other
Kodi releases and display backends are not generally qualified; stock Kodi 22
has the [interlaced compatibility limits above](#known-issues-and-deinterlacing-roadmap).

For the selected interlaced subset in Kodi 21.3, keep **VAAPI hardware
acceleration enabled** for CrystalHD decoding. While playing, open the cog
icon → **Video settings → Deinterlace method**:

- **Off** leaves the decoded Weave frame unfiltered.
- **Deinterlace** explicitly selects Kodi's existing CPU BWDIF path after VA
  readback; this is not driver-side Bob or VPP deinterlacing.
- **Auto select** is not a way to force BWDIF and may probe unsupported VAAPI
  Bob. Prefer the explicit method for this comparison.

Use **Off/NONE** for Weave. Kodi may still perform an initial VPP capability
probe even when deinterlacing is off. With Auto on interlaced pictures, some
Kodi versions repeat unsupported deinterlacing probes once per picture
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
preserved in the hardware report. During RC3 preparation, the later finite
MBAFF input/tail fix completed all 750 decoder-oracle pictures with verified
pixels/PTS and EOS, and passed live input-gap/explicit-close/reopen checks.
Kodi traversed the selected full file but could discard pictures or resync
under load; some FHD runs retained timing/render warnings and a bounded
post-EOS empty-output wait. These are historical results, not fresh RC4
qualification. No incomplete surface is returned to hide a wait.
For an explicit HW/SW Weave selection use the
[player commands above](#explicit-weave-playback). Kodi's VA-API acceleration
setting selects hardware versus CPU decode; its deinterlacing method is a
separate setting. Published 1.1.0 advertises no VA driver
deinterlacing capability; deinterlacing and sync tuning remain
tracked in [#436](https://github.com/ahnhy1324/crystalhd/issues/436).

During RC3 preparation (recorded in `a3fd609`), a selected 30-second 720i25 dialogue clip had
subjectively aligned speech/lip motion in both no-deinterlace and explicit
`Deinterlace` (BWDIF) comparisons; BWDIF reduced the visible shaking. Both
ended normally. This is a useful playback check, not measured millisecond
sync, a complete displayed-frame census or FHD support qualification.
The CPU/no-deinterlace control showed similar shaking without using CrystalHD.
These observations are not new RC4 playback or physical-sync measurements.

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
Fresh RC4 installation also passed two complete 12-second VLC 3.0.23
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
