# CrystalHD GStreamer 1.x decoder

`crystalhddec` exposes BCM70012/BCM70015 decoding to current GStreamer
pipelines. It accepts parsed H.264 byte-stream and MPEG-2, plus the VC-1/WMV3
framing described below,
and outputs YUY2 video frames.

This plugin is experimental. The hardware harness targets progressive H.264
Baseline, Main and High on BCM70015. It counts YUY2 output buffers against
FFprobe's decoded frame count, requires EOS, checks output buffer sizes and
monotonic timestamps, and opens a fresh playback session for each repetition.
Separate BCM70015 tests also cover one progressive MPEG-2 fixture, two small
VC-1/WMV3 fixtures, and H.264 flushing replay after EOS. BCM70012, interlaced
output, arbitrary in-flight seeks, and mid-stream format changes remain
unvalidated. CI checks plugin discovery, caps/framing helpers, and synthetic
YUY2 playback; it does not decode through CrystalHD. See the
[hardware report](../../../HARDWARE-2026-09-13.md) for fixture counts, hashes,
software-output comparisons, and precise coverage boundaries.

Build the driver and `libcrystalhd` first, then build and inspect the plugin:

```sh
make -C linux_lib/libcrystalhd
make -C filters/gst/gst-plugin-1.0
make -C filters/gst/gst-plugin-1.0 check
```

After `sudo make install`, inspect the system plugin with:

```sh
gst-inspect-1.0 crystalhddec
```

Check the `Filename` under `Plugin Details`: an installed test should show the
system plugin directory; a source-tree test should show this directory. To
verify the source-tree library as well:

```sh
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
ldd filters/gst/gst-plugin-1.0/libgstcrystalhd.so
```

`libcrystalhd.so.3` should resolve into `linux_lib/libcrystalhd`. For installed
discovery, run `gst-inspect-1.0` without `GST_PLUGIN_PATH` or `LD_LIBRARY_PATH`
overrides, and check `ldd` on the installed plugin's reported filename.

For an H.264 file in an MP4 container:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! videoconvert ! autovideosink
```

The plugin requests access units in Annex-B/byte-stream format from
`h264parse`, so MP4 AVC length prefixes are converted automatically during
caps negotiation.

## VC-1 and WMV3 input

The decoder distinguishes WMV3 Simple/Main from VC-1 Advanced (`WVC1`). These
are the accepted `video/x-wmv,wmvversion=3` framing combinations:

| Format | Stream/header format | Input contract |
| --- | --- | --- |
| WMV3 | `asf` / `asf` | One complete ASF picture per buffer; four-byte sequence metadata in `codec_data` (a fifth trailing byte is tolerated). |
| WVC1 | `asf` / `asf` | Complete ASF picture packets; sequence/entry-point startcodes in `codec_data`, with or without the ASF binding byte. |
| WMV3 | `frame-layer` / `asf` | One complete Annex-L frame layer per buffer; its eight-byte wrapper is validated and stripped. |
| WVC1 | `bdu` or `bdu-frame` / `none` | Startcoded elementary stream with sequence/entry-point headers in-band; fields and slices are grouped with their picture. |

Ordinary `asfdemux` output omits `stream-format` and `header-format`; these
missing fields mean ASF packets. Missing `format` retains the legacy WMV3
interpretation. Explicit unsupported framing is rejected. Each VC-1/WMV3
compressed picture is limited to 16 MiB. Raw BDU input also has a 16 MiB
buffered-input guard; do not push an entire larger file in one buffer.
Sequence-layer/RCV container framing is not accepted directly. The optional
FFmpeg-demuxed probe below can read RCV and supply its complete picture packets.

For WMV3 in an ASF/WMV container, no additional parser is required:

```sh
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=5 35 gst-launch-1.0 -q \
  filesrc location=wmv3.wmv ! asfdemux ! crystalhddec ! fakesink sync=false
```

`asfdemux` is supplied by GStreamer's Ugly plugins. The tested WMV3 Main
fixture was FFmpeg FATE's `SMM0015.rcv`, remuxed without transcoding using
`ffmpeg -nostdin -n -i SMM0015.rcv -map 0:v:0 -c copy -f asf wmv3.wmv`.
It produced all 25 frames at 720×576, 25 fps.

For the tested raw VC-1 Advanced fixture, use its actual dimensions and rate:

```sh
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=5 35 gst-launch-1.0 -q \
  filesrc location=SA00040.vc1 ! \
  'video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=none,width=176,height=144,framerate=25/1' ! \
  crystalhddec ! fakesink sync=false
```

This fixture produced all 15 frames. The decoder itself groups raw BDUs;
`vc1parse` is not needed in this pipeline. The installed GStreamer 1.26.5
`vc1parse` did not successfully negotiate/parse the two supplied elementary
fixtures, so it was not part of the passing paths. Do not generalize these
short progressive results to interlaced streams, arbitrary seeks, format
changes, or codec conformance. H.264/MPEG-2 parser requirements are unchanged.

An optional counted probe uses FFmpeg demuxing and GStreamer `appsrc`, checks
exact frame count, YUY2 dimensions, valid timestamp ordering, and EOS, and
prints a hash of the visible pixels:

```sh
make -C filters/gst/gst-plugin-1.0 codec-playback-test
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=5 35 \
  filters/gst/gst-plugin-1.0/gstreamer-codec-playback-test SMM0015.rcv 25
```

Use `SA00040.vc1 15` for the other fixture. Building this optional probe needs
the `libavformat`, `libavcodec`, `libavutil`, and GStreamer app development
packages; they are not dependencies of the plugin build. When pkg-config
finds them, `make check` also runs the probe's hardware-free `--self-test`.
Feeding and EOS share a 25-second deadline and a bounded queue; retain the
outer timeout because userspace cannot bound a blocked driver close. A hash
alone is not a software-decoder comparison; see the hardware report for the
separate pixel checks performed on these fixtures.

## Counted H.264 playback and replay

CrystalHD provides one playback session. Stop any VA-API, browser, or other
GStreamer hardware decode before starting another `crystalhddec` pipeline.
The hardware test helper verifies module/source-version consistency and two
complete decodes by default. It builds the source-tree frontend, uses a fresh
GStreamer registry, and reports the selected plugin filename:

```sh
./tests/gstreamer-hardware.sh /path/to/video.mp4 2
```

The same command accepts `.h264` Annex-B elementary streams. MP4 timestamps
must be present and ordered; elementary streams are also checked for ordering
when timestamps are available. Set `CRYSTALHD_TEST_TIMEOUT=300` for a long clip;
the default timeout is 120 seconds per run. `ffprobe` (from the `ffmpeg` package),
`h264parse` and `qtdemux` are required. The shell harness explicitly rejects
other codecs/profiles and known interlaced input so a baseline result cannot
be mistaken for validation of those paths.

For one nondisplay decode without the frame-count harness:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 -q filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! fakesink sync=false
```

Plugin errors identify the failing library call and suggest checks for device
ownership, permissions, firmware, framing and output selection. A successful
launch alone does not prove that every frame drained: use the counted harness
for that claim. Final drain uses a 10-second monotonic budget, not a fixed
number of output polls; missing pictures still produce an error. See
[the bring-up guide](../../../BRINGUP.md) for the canonical
device/firmware/input/output diagnosis order and required test-report details.

To exercise GStreamer's flushing seek in the same playback session:

```sh
CRYSTALHD_TEST_SEEK=1 ./tests/gstreamer-hardware.sh /path/to/video.mp4 2
```

Each iteration first drains the complete file, seeks accurately to zero with
`GST_SEEK_FLAG_FLUSH`, and requires the same frame count and SHA256 of the
visible YUY2 pixels on replay. This covers replay after EOS, not arbitrary
mid-playback seeks or a change of resolution. Pixel hashes validate replay
identity against the first hardware decode; they are not a comparison against
a software decoder. Record this result separately from ordinary playback.
