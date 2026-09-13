# CrystalHD GStreamer 1.x decoder

`crystalhddec` exposes BCM70012/BCM70015 decoding to current GStreamer
pipelines. It accepts parsed H.264 byte-stream, MPEG-2, VC-1, and WMV3 input
and outputs YUY2 video frames.

This plugin is experimental. The hardware harness targets progressive H.264
Baseline, Main and High on BCM70015. It counts YUY2 output buffers against
FFprobe's decoded frame count, requires EOS, checks output buffer sizes and
monotonic timestamps, and opens a fresh playback session for each repetition.
MPEG-2, VC-1, WMV3, BCM70012, interlaced output, seeks, and mid-stream format
changes are exposed by the implementation but are outside this harness's
coverage. CI checks plugin discovery and uses a synthetic YUY2 source to verify
that the harness rejects incomplete output and missing EOS; it does not decode
through CrystalHD. Record each hardware profile separately in the test report.

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
for that claim. See [the bring-up guide](../../../BRINGUP.md) for the canonical
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
