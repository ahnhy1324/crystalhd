# Kodi route probe

Issue [#531](https://github.com/ahnhy1324/crystalhd/issues/531).
The default Gate A build demuxes a selected local H.264 MP4 and emits one
**synthetic YUY2 picture** through an InputStream-hosted VideoCodec instance.
It neither decodes the file nor accesses CrystalHD. It is not a playback
decoder, performance test or physical display acceptance.

Target: x86_64 Kodi 22 RC1 `28ea2ea`, InputStream ABI 3.4.0 and VideoCodec
2.2.1. The installed Ubuntu Kodi 21.3 / VideoCodec 2.1.0 has no packed-format
enum and cannot run this add-on. Build against the matching official SDK:

```sh
make -C tools/kodi-route-probe KODI_SDK_INCLUDE="/absolute/path/to/kodi-dev-kit/include"
```

The build produces `build/inputstream.crystalhd.routeprobe/`; it has no install
target. Use only a separate Kodi test profile, explicitly selecting this
InputStream in a ListItem. With the add-on enabled in that profile, invoke
`RunScript(/absolute/path/to/launch.py,/absolute/path/high.mp4)` inside Kodi;
[launch.py](launch.py) sets the required property. Ordinary local-file opening does not select it.
System Kodi, its profile and the VA/NV12 defaults are not changed.

RC1 advertises only YV12 to the add-on despite exposing packed formats in its
buffer API. This is an explicit, experimental format-admission probe, not a
claim of negotiated support. Reject unsafe allocation/layout; do not replace
formats with private enum values or retain decoder DMA leases. Native decode,
pixels/PTS, seek/drain/reopen, CPU and A/V remain separate gates.

## Native decoder prototype

`make -C tools/kodi-route-probe native KODI_SDK_INCLUDE="/absolute/path/to/kodi-dev-kit/include"`
builds a separate `build-native/inputstream.crystalhd.routeprobe/` directory.
It uses `appsrc → h264parse → crystalhddec → appsink` with explicit YUY2
output and no autoplugged video decoder or NV12 conversion. It needs the
project's GStreamer plugin and matching `libcrystalhd.so.3` at runtime, selected
through the usual `GST_PLUGIN_PATH_1_0` / `LD_LIBRARY_PATH` environment.
Use that directory instead of the synthetic build in a separate test profile;
the explicit ListItem launcher stays the same. A selected mono/stereo AAC
soundtrack is forwarded to Kodi's audio decoder using its own packet timebase;
another soundtrack format is rejected. MPEG-2 and interlaced field flags are
not implemented in this prototype.
Missing/invalid timestamps, interlaced output and unexpected geometry fail.

The existing decoder owns hardware NoCopy leases, CPB admission and recovery.
There are **two host copies** (RX to GstBuffer, then checked rows to Kodi's
buffer), not zero-copy. The source queue is bounded at 8 packets/4 MiB and the
output queue at 4 pictures. Kodi DRAIN can mean temporary input starvation, so
it never by itself ends the hardware stream. Final EOS requires the parent's
demux EOF, every demuxed video packet submitted, real decoder EOS and queued
pictures. A temporary squeeze phase can finish while preserving later input.
Seek Reset closes the old decoder and creates a fresh parser
and decoder on the next packet. This avoids H264Parse's NULL/restart timestamp
policy change. An inactive/error session also terminates the host's drain loop;
an error cancellation is logged separately and is not a natural EOS pass.
Bounded native qualification is recorded below; it does not establish
performance or physical A/V acceptance. Kodi may choose its own fallback when an add-on fails;
verify the native session markers, actual device ownership and returned frames
before calling playback a CrystalHD success.

`make -C tools/kodi-route-probe check` runs device-free row/timestamp and bounded
appsrc/appsink retry/drain/cancel/reopen tests. `native-trace` is a separate
diagnostic build that logs per-picture PTS and an active-row pixel digest at
EOS; do not use that build as a performance measurement. Neither target
installs anything or changes the normal Kodi/VA-API path.
With the matching SDK, `check-codec` also runs the real add-on callbacks against
a minimal host, including repeated Reset and terminal error drain, without
loading a decoder plugin or opening a device.

Source `f9c8b2f` passes the explicit route/buffer probe on published Flathub
22 RC1 in a private Xvfb/software-GLES environment: a companion codec instance
opens and returns one Kodi-owned 640×360 YUY2 buffer (stride 1280, 460,800
bytes), then exits normally. This is **not a renderer or native decode pass**.
The [receipt](../../tests/fixtures/issue531/kodi-route-gate-a-20261026.json)
preserves the initial packet-size defect and an excluded virtual-display
startup failure. The system Kodi/profile and device stack remain unchanged.


## Native qualification

At `cd0ddd2`, a single isolated Kodi process completes three video-only runs,
each **180 ordered pictures with exact PTS and frozen YUY2 pixels**, returning
its device resources between files. At `f8b0bcb`, H.264/AAC full playback and
reopen also return **180/180**; pause/resume, forward/backward seek and an
explicit mid-file Stop complete with clean card retirement. Kodi selects its
ordinary **YUY2 GLES shader**. A separately routed null-audio run verifies the
Kodi client-to-null-sink links; none of these are physical A/V acceptance.

The [native receipt](../../tests/fixtures/issue531/kodi-native-yuy2-20261026.json)
separates tested add-on and decoder-engine sources, full-file counts from
cancelled seek/Stop epochs, earlier FAILs and observer corrections. It includes
the fixed premature-EOS counterexample: the first AAC arm ended at 122/180
because temporary DRAIN was treated as final input. The repaired callback test
proves later input survives a temporary drain, including parent EOF with a
packet still queued in Kodi.

These bounded runs use a **software GLES/Xvfb** display, not a matched GPU
performance comparison. Full RGB/asymmetric shader pixel checks, broader
source color metadata, physical A/V, endurance, i386 and a matched VA/NV12 A/B
remain open. No throughput, CPU saving or zero-copy claim is made. Installed
Kodi 21.3 and the existing VA/NV12 path remain the ordinary playback route.
