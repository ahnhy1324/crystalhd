# libcrystalhd

`libcrystalhd` is the legacy userspace API used to control the CrystalHD
kernel driver and firmware. This fork preserves its public headers and ABI
while updating the build and installation layout for current Linux systems.

CI freezes the native/compat ioctl layouts and exercises the TX ring,
flush/cancellation, EOS, AVC1 framing and raw-copy error paths without
hardware. These checks are not comprehensive device-API conformance. New
clients should treat this API as a compatibility layer rather than a complete
media framework.

Build and stage the library from the repository root:

```sh
make library
make -C linux_lib/libcrystalhd DESTDIR=/tmp/crystalhd-library install
```

The install target provides the shared library, public headers, and
`libcrystalhd.pc` for `pkg-config`.

## Direct-library drain validation

This optional probe bypasses GStreamer and VA-API. It checks complete picture
delivery and genuine firmware EOS, not pixel quality or displayed cadence.
Install the FFmpeg `libavformat`, `libavcodec`, `libavutil` and GLib
development packages. Use a progressive fixture with an independently known
decoded-frame count:

```sh
make library-drain-test
ffprobe -v error -select_streams v:0 -count_frames \
  -show_entries stream=codec_name,profile,width,height,field_order,nb_read_frames \
  /path/to/video.h264
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
  tests/library-drain-test --preflight /path/to/video.h264 30
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
  timeout --kill-after=10s 60s \
  tests/library-drain-test --hardware /path/to/video.h264 30 35
```

Replace `30` in both commands with the fixture's expected picture count.
`--preflight` never opens the card: it validates supported framing and the
demuxed packet count, assuming one complete progressive picture per packet.
It is not itself a software decode or proof of error-free media.
`--hardware` requires an idle BCM70015 and a matching loaded module; see
[device access and module identity](../../BRINGUP.md#device-access-and-module-identity).
It verifies that EOS was clear immediately before `DtsFlushInput(0)`, then
requires every expected timestamped picture, firmware-derived
`DtsIsEndOfStream`, an empty ready queue and successful stop/close. The firmware
timing marker need not be returned as a separate client-owned output buffer;
input inactivity alone never counts as EOS.

The probe does not accept MPEG-4. A library client draining MPEG-4 may complete
`DtsIsEndOfStream` without a firmware marker only after the explicit drain TX
retires and every output buffer and queue remains free and idle for one second.
Input inactivity by itself still does not count as EOS.

Supported input is raw Annex-B H.264, MPEG-2 elementary stream, raw VC-1
Advanced, or WMV3 in ASF with four-byte sequence metadata (a fifth trailing byte
is tolerated). MP4 H.264, RCV containers and known interlaced input are rejected.
Fixtures are limited to 64 MiB, 10,000 pictures and 1920x1088; each packet plus
framing must fit the transmit ring.
The in-process timeout defaults to 30 seconds and accepts at most 300. Keep an
external timeout because blocked ioctls or device close cannot be interrupted
by the probe's deadline. The probe does not load or replace the module and is
not part of `make check`. For hardware results, use the
[hardware report](../../HARDWARE-2026-09-13.md).

## Raw YUY2 copy contract

For 32-bit i686 CPUs without SSE/SSE2, build with `LEGACY_CPU=1` (see
[32-bit and legacy CPUs](../../README.md#32-bit-and-legacy-cpus)). This keeps the
library ABI but selects scalar conversions; it does not bypass the player's
own CPU or dependency requirements. The dedicated legacy check runs both
production sections and the built shared library under a verified no-SSE CPU
model, without opening hardware.

For the ordinary packed-YUY2 `DtsProcOutput` copy path (without
`BC_POUT_FLAGS_MODE`), `YbuffSz` and `YBuffDoneSz` are counts of four-byte
units, not bytes. `YbuffSz` describes available storage starting at the supplied
`Ybuff` pointer, including when that pointer is offset into a larger picture.
`YBuffDoneSz` retains the source transfer count; it is not a count of destination
bytes written after cropping or adding padding.

`BC_POUT_FLAGS_SIZE` selects the application's `PicInfo.width/height` crop;
without it, the current decoded picture supplies those dimensions. The source
row pitch always comes from the hardware. With `BC_POUT_FLAGS_STRIDE`,
`StrideSz` is extra destination pixels per copied row, so packed-YUY2 pitch is
`2 * (width + StrideSz)` bytes. It is not the full pitch or a byte count.
Current interlaced fields copy half the picture height; callers weave them by
choosing the appropriate starting row and padding over the other field's row.

The copy checks dimensions, source transfer extent and destination capacity
before writing, including stride gaps but not unused padding after the last
row. Undersized or overflowing layouts fail rather than return a partial
successful picture. From the repository root, `make library-check` includes
[tests/library-copy.cpp](../../tests/library-copy.cpp): exact row/crop/field
identity and rejected-layout canaries against the real implementation.
Separate planar and MODE regressions cover NV12/YV12 row layout, field
weaving, independent chroma padding and bounded format conversions. These
device-free checks do not establish hardware playback or frontend timing.

## AVC1 parameter sets

AVC1 input uses the configured NAL-length width before conversion to Annex B.
The library recognizes an in-band SPS in that framing and avoids sending a
redundant untimestamped copy of stored parameter sets. When the input lacks an
SPS, the stored metadata is still sent. This detection preserves converter
state, picture bytes and caller timestamps; it does not repair timestamps
already changed by a demuxer.

The production input/PES/ring regression in `make library-check` verifies
length widths 1/2/4, retained metadata injection, existing Annex-B compatibility
and bounded detection, including i386 execution. See the
[hardware report](../../HARDWARE-2026-09-13.md) for frontend results.
