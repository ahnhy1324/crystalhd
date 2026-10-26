# Kodi route probe

Issue [#531](https://github.com/ahnhy1324/crystalhd/issues/531), Gate A only.
This optional binary add-on demuxes a selected local H.264 file and emits one
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
InputStream in a ListItem. Ordinary local-file opening does not select it.
System Kodi, its profile and the VA/NV12 defaults are not changed.

RC1 advertises only YV12 to the add-on despite exposing packed formats in its
buffer API. This is an explicit, experimental format-admission probe, not a
claim of negotiated support. Reject unsafe allocation/layout; do not replace
formats with private enum values or retain decoder DMA leases. Native decode,
pixels/PTS, seek/drain/reopen, CPU and A/V remain separate gates.
