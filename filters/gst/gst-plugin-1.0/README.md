# CrystalHD GStreamer 1.x decoder

`crystalhddec` exposes BCM70012/BCM70015 decoding to current GStreamer
pipelines. It accepts parsed H.264 byte-stream, MPEG-2, VC-1, and WMV3 input
and outputs YUY2 video frames.

This plugin is experimental. The current hardware test covers progressive
H.264 on BCM70015 and verifies that a complete file drains successfully.
MPEG-2, VC-1, WMV3, BCM70012, interlaced output, seeks, and mid-stream format
changes are exposed by the implementation but have not been validated by the
current test suite. CI without hardware checks only that the plugin builds and
can be discovered by `gst-inspect-1.0`.

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
The hardware test helper verifies module/source-version consistency and a
complete decode:

```sh
./tests/gstreamer-hardware.sh /path/to/video.mp4
```
