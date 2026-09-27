# CrystalHD GStreamer 1.x decoder

`crystalhddec` exposes BCM70012/BCM70015 decoding to current GStreamer
pipelines. It accepts parsed H.264, MPEG-2 and MPEG-4 Part 2, plus the
VC-1/WMV3 framing described below, and outputs YUY2 video frames.

This plugin is experimental. The validated BCM70015 subset and known failures
are recorded in the [hardware report](../../../HARDWARE-2026-09-13.md).
Device-free CI checks discovery, framing/lifecycle helpers, synthetic YUY2
playback and software audio/video controls; it does not decode through
CrystalHD or establish BCM70012 behavior.

## Build and basic use

For an installed build, inspect the system plugin first:

```sh
gst-inspect-1.0 crystalhddec
```

Check `Filename` under `Plugin Details`; it must name the system plugin rather
than a checkout. `crystalhd-check` reports that filename and the resolved
`libcrystalhd.so.3` together.

For a source-tree test, run from the repository root:

```sh
make -C linux_lib/libcrystalhd
make -C filters/gst/gst-plugin-1.0
make -C filters/gst/gst-plugin-1.0 check
```

Verify that the source plugin resolves the source library:

```sh
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
ldd filters/gst/gst-plugin-1.0/libgstcrystalhd.so
```

`libcrystalhd.so.3` should resolve into `linux_lib/libcrystalhd`. For installed
discovery, run `gst-inspect-1.0` without `GST_PLUGIN_PATH` or `LD_LIBRARY_PATH`
overrides, and check `ldd` on the installed plugin's reported filename.

Decode an H.264 file in an MP4 container from the source tree:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! videoconvert ! autovideosink
```

The plugin requests access units in Annex-B/byte-stream format from
`h264parse`, so MP4 AVC length prefixes are converted automatically during
caps negotiation.

## MPEG-4 Part 2 input

BCM70015 accepts parsed Simple and Advanced Simple levels 3/5. The validated
subset is progressive rectangular 8-bit 4:2:0 with H.263 quantization and no
sprites/GMC, quarter-pixel, data partitioning, RVLC or interlace. Advanced
Simple additionally requires disabled resynchronization markers. Codec data or
an in-band VOS/VOL must agree with caps and picture dimensions; unsupported or
ambiguous streams fail negotiation instead of being submitted as another
codec. BCM70012 is rejected for MPEG-4.

For MPEG-4 in an MP4 container:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 -q filesrc location=video.mp4 ! qtdemux ! \
  mpeg4videoparse ! crystalhddec ! videoconvert ! autovideosink
```

The parser must provide one complete VOP per buffer with `parsed=true` and
Simple/Advanced Simple profile and level fields. A successful pipeline only
proves that particular file; use the counted hardware probe for frame-count,
timestamp and EOS validation.

## VC-1 and WMV3 input

The decoder distinguishes WMV3 Simple/Main from VC-1 Advanced (`WVC1`). These
are the accepted `video/x-wmv,wmvversion=3` framing combinations:

| Format | Stream/header format | Input contract |
| --- | --- | --- |
| WMV3 | `asf` / `asf` | One complete picture per buffer; original four-byte STRUCT_C metadata in `codec_data`. Trailing encoder bytes, including six-byte AVI metadata, are ignored without changing the header. |
| WVC1 | `asf` / `asf` | Complete ASF picture packets; sequence/entry-point startcodes in `codec_data`, with or without the ASF binding byte. |
| WMV3 | `frame-layer` / `asf` | One complete Annex-L frame layer per buffer; its eight-byte wrapper is validated and stripped. |
| WVC1 | `bdu` or `bdu-frame` / `none` | Startcoded elementary stream with sequence/entry-point headers in-band; fields and slices are grouped with their picture. |

Ordinary `asfdemux` output omits `stream-format` and `header-format`; these
missing fields mean ASF packets. Missing `format` retains the legacy WMV3
interpretation. Explicit unsupported framing is rejected. Raw BDU assembly
has a 16 MiB buffered-input guard; do not push an entire larger file in one
buffer. All codecs additionally require each complete compressed picture,
injected metadata and conservative packet-header allowance to fit the
library's 1 MiB transmit ring. Oversized pictures are rejected before
submission, so the usable payload limit is slightly below 1 MiB and depends
on framing/metadata.
Sequence-layer/RCV container framing is not accepted directly. The optional
FFmpeg-demuxed probe below can read RCV and supply its complete picture packets.

For WMV3 in an ASF/WMV container, no additional parser is required:

```sh
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=5 35 gst-launch-1.0 -q \
  filesrc location=wmv3.wmv ! asfdemux ! crystalhddec ! fakesink sync=false
```

For WMV3 in AVI, replace `asfdemux` with `avidemux`. `asfdemux` is supplied by
GStreamer's Ugly plugins.

For raw VC-1 Advanced, set the caps to the stream's actual dimensions and
frame rate:

```sh
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=5 35 gst-launch-1.0 -q \
  filesrc location=input.vc1 ! \
  'video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=none,width=176,height=144,framerate=25/1' ! \
  crystalhddec ! fakesink sync=false
```

Replace `width`, `height` and `framerate` with the values for the input. The
decoder groups raw BDUs, so `vc1parse` is not required in this pipeline.

An optional counted probe uses FFmpeg demuxing and GStreamer `appsrc`, checks
exact frame count, YUY2 dimensions, valid timestamp ordering, and EOS, and
prints a hash of the visible pixels:

```sh
make -C filters/gst/gst-plugin-1.0 codec-playback-test
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=5 35 \
  filters/gst/gst-plugin-1.0/gstreamer-codec-playback-test \
  /path/to/input.wmv EXPECTED_FRAMES
```

Building this optional probe needs the `libavformat`, `libavcodec`, `libavutil`
and GStreamer app development packages; they are not dependencies of the
plugin build. When pkg-config finds them, `make check` runs its hardware-free
`--self-test`. Feeding and EOS share a 25-second deadline and a bounded queue;
retain the outer timeout because userspace cannot bound a blocked driver
close. The printed hash is not a software-decoder comparison.

## Counted H.264 playback and replay

Generate reproducible Baseline/Main/High MP4 fixtures with FFmpeg and libx264:

```sh
sh tests/generate-h264-samples.sh /tmp/crystalhd-samples
# Optional Full HD set: 1920x1080, 30 fps, 180 frames per profile.
sh tests/generate-h264-samples.sh /tmp/crystalhd-fhd-samples 1920x1080
```

The generator refuses to overwrite existing samples and prints each profile,
frame count and SHA-256. Run the harness separately for each generated file.

The harness compares YUY2 output with FFprobe's decoded frame count, checks
dimensions and timestamp order, requires EOS and scans new kernel messages. It
also verifies module/source identity and the selected source plugin:

```sh
./tests/gstreamer-hardware.sh /path/to/video.mp4 2
```

The command also accepts Annex-B `.h264` streams. MP4 timestamps must be
present and ordered; elementary-stream timestamps are checked when available.
The default timeout is 120 seconds per run and can be changed with
`CRYSTALHD_TEST_TIMEOUT`. `ffprobe`, `h264parse` and `qtdemux` are required.
Other codecs/profiles and known interlaced input are rejected by this harness.
It loads and later unloads the source-built module only when no matching module
was already loaded; see [module identity](../../../BRINGUP.md#device-access-and-module-identity).

For one nondisplay decode without the frame-count harness:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 -q filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! fakesink sync=false
```

A successful launch alone does not prove that every frame drained; use the
counted harness for that claim. Final drain has a 10-second no-progress
watchdog, renewed by successful delivery and suspended while stably paused.
See [the bring-up guide](../../../BRINGUP.md) for failure diagnosis.

To exercise GStreamer's flushing seek in the same playback session:

```sh
CRYSTALHD_TEST_SEEK=1 ./tests/gstreamer-hardware.sh /path/to/video.mp4 2
```

Each iteration drains the file, seeks to zero with `GST_SEEK_FLAG_FLUSH`, and
requires the same frame count and visible-YUY2 SHA-256 on replay. This covers
replay after EOS, not arbitrary mid-playback seeks or resolution changes. The
hash compares two hardware runs, not hardware against a software decoder.

## Local player and in-flight controls

`scripts/crystalhd-play` provides explicit hardware/software local-file
playback; see the [top-level usage](../../../README.md#play-a-local-file).
Hardware mode never silently falls back to software.

The separate controls probe verifies barcode pixels and timestamps while
pausing, resuming, seeking forward/backward and changing rate to 0.5x/2x/1x:

```sh
make -C filters/gst/gst-plugin-1.0 gstreamer-controls-test
sh tests/generate-browser-sample.sh /tmp/crystalhd-controls.mp4 --av-360p
timeout --kill-after=10 90 \
  filters/gst/gst-plugin-1.0/gstreamer-controls-test \
  /tmp/crystalhd-controls.mp4 --software --audio --timeout 75
```

For the numbered 720p hardware check, first verify the loaded module and stop
other CrystalHD clients. This needs FFmpeg with libx264/AAC encoding and
GStreamer's H.264/AAC plugins:

```sh
crystalhd_sample_dir=$(mktemp -d)
sh tests/generate-browser-sample.sh "$crystalhd_sample_dir/av720.mp4" --av-720p
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
GST_REGISTRY="$crystalhd_sample_dir/registry.bin" \
timeout --kill-after=10 120 \
  filters/gst/gst-plugin-1.0/gstreamer-controls-test \
  "$crystalhd_sample_dir/av720.mp4" --audio --timeout 90
```

The check includes a complete 360-picture replay and rejects missing or
out-of-order identities, incorrect seek/rate progress, video lateness above
250 ms or sampled A/V interval skew above 100 ms. Flushing seeks recreate the
device. Retain the external timeout because a library or driver call can block
beyond the in-process watchdog.

`--sustain SECONDS` selects continuous 1x playback. It requires a fixture whose
duration is a multiple of 12 seconds, exact 30-fps timestamps, the repeated
360-frame barcode sequence and complete EOS. The optional audio check verifies
clock bounds and overlap with the final video interval, not sample-exact audio
duration. These tests use clocked test sinks and do not prove visible or
audible presentation; see the hardware report for measured runs.
