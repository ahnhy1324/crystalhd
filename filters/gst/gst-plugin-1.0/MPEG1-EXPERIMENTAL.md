# Optional experimental MPEG-1

This unreleased research-branch feature is **experimental MPEG-1 decoding via
the MPEG-2 algorithm-1 path, natural-content hardware qualified on BCM70015**.
It is separate from official/vendor documented support and generalized MPEG-1
conformance. The qualified scope is progressive352x288/25, not BCM70012,
other sizes/rates, interlacing or arbitrary damaged streams.

The separately built plugin registers `crystalhdmpeg1experimentaldec` at rank
NONE. Select it explicitly. The ordinary `crystalhddec` retains its existing
caps and MPEG-1 rejection; automatic playback/backend defaults do not change.
Original `video/mpeg,mpegversion=1` identity, bytes and PTS are preserved.
Only the internal DIL transport uses MPEG2VIDEO/algorithm1; there is no
selector5, invented VA MPEG-1 profile or Kodi-support claim. The DIL, driver,
firmware and external applications are unchanged.

From a configured development checkout with the normal DIL already built:

```sh
make -C filters/gst/gst-plugin-1.0 experimental-mpeg1
make -C filters/gst/gst-plugin-1.0 check-experimental-mpeg1
sudo make -C filters/gst/gst-plugin-1.0 install-experimental-mpeg1
gst-inspect-1.0 crystalhdmpeg1experimentaldec
```

Normal `all` and `install` do not install this optional plugin. An isolated
`DESTDIR` plus a private `GST_PLUGIN_PATH_1_0`/registry can test it without
system installation. The optional build accepts `MPEG1_DIL`, `MPEG1_PLUGIN`,
`MPEG1_TEST` and `MPEG1_PLAYBACK` overrides for an exact existing DIL and private outputs;
do not mix unrelated library versions. No local FFmpeg rebuild is needed.

For the qualified Matroska MPEG-1/AAC clip:

```sh
gst-launch-1.0 -e filesrc location=clip.mkv ! matroskademux name=d \
  d.video_0 ! queue ! mpegvideoparse ! crystalhdmpeg1experimentaldec ! \
    videoconvert ! xvimagesink \
  d.audio_0 ! queue ! aacparse ! avdec_aac ! audioconvert ! audioresample ! pulsesink
```

Use the original MPEG-1 bytes and parser/caps. Relabeling another codec as
MPEG-1 or MPEG-2 is not a compatibility method. The scoped sequence/header,
rate/geometry, picture and device checks reject unsupported input. One decoder
session owns the card; follow the project's normal thermal/PCIe requirements.
No queue, QoS, lateness, deadline, thread or priority adjustment is part of
this qualification.

Disable by selecting an ordinary supported-codec path and removing only the
optional plugin:

```sh
sudo make -C filters/gst/gst-plugin-1.0 uninstall-experimental-mpeg1
```

Use the same `PLUGINDIR`/`DESTDIR` used for installation. The ordinary plugin
and default player remain available. Existing releases do not gain this
feature merely because this research branch contains it.

The [natural holdout receipt](../../../tests/fixtures/issue606/observation-20261031.json)
records two identical1250-picture captures, true AU/picture/PTS association,
normal MPEG-2/H.264 recovery and both unchanged chroma models. Cb/Cr maxima
are2/2; direct Y is not bit-exact (max3, five samples reach3;1,319,874 of
126,720,000 samples differ overall). No filter fitting, reference replacement
or actual chip arithmetic is claimed. Same-conversion stills and clocked
Xv/Pulse playback are separate evidence. A/V sync follows source-qualified
logs with no detected drift; the [installed/staged-plugin receipt](../../../tests/fixtures/issue606/experimental-codec-20261031.json)
records actual playback, selected sink pixels, safe teardown and removal.
This is not a calibrated physical lip-sync or scanout measurement. Keep historical full-axis Lanczos22/22 FAIL and all
negative/operator outcomes when interpreting this scoped result (#606).
