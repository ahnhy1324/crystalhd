# Kodi route probe

Issue [#531](https://github.com/ahnhy1324/crystalhd/issues/531), Gate A only.
This optional binary add-on demuxes a selected local H.264 MP4 and emits one
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

Source `f9c8b2f` passes the explicit route/buffer probe on published Flathub
22 RC1 in a private Xvfb/software-GLES environment: a companion codec instance
opens and returns one Kodi-owned 640×360 YUY2 buffer (stride 1280, 460,800
bytes), then exits normally. This is **not a renderer or native decode pass**.
The [receipt](../../tests/fixtures/issue531/kodi-route-gate-a-20261026.json)
preserves the initial packet-size defect and an excluded virtual-display
startup failure. The system Kodi/profile and device stack remain unchanged.
