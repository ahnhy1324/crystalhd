# libcrystalhd

`libcrystalhd` is the legacy userspace API used to control the CrystalHD
kernel driver and firmware. This fork preserves its public headers and ABI
while updating the build and installation layout for current Linux systems.

CI freezes the native/compat ioctl layouts and exercises targeted production
TX-ring, flush/cancellation, EOS, AVC1 input framing and raw-copy error paths
without hardware.
GStreamer, VA-API and an optional direct-library drain probe also exercise
BCM70015 hardware. These checks are not comprehensive device-API conformance;
BCM70012 has not been tested recently. New clients should treat the API as a
compatibility layer rather than a complete modern media framework.

Build and stage the library with:

```sh
make
make DESTDIR=/tmp/crystalhd-library install
```

The install target provides the shared library, public headers, and
`libcrystalhd.pc` for `pkg-config`.

## Raw YUY2 copy contract

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
identity and rejected-layout canaries against the real implementation. These
67 checks also pass native and i386 execution and ASan/UBSan with leak
detection. They cover raw YUY2 copying, not the separate legacy NV12, YV12 or
`BC_POUT_FLAGS_MODE` conversion helpers, hardware playback, or PowerVLC's
playback timing.

## AVC1 parameter sets

AVC1 input uses the configured NAL-length width before conversion to Annex B.
The library recognizes an in-band SPS in that framing and avoids sending a
redundant untimestamped copy of stored parameter sets. When the input lacks an
SPS, the stored metadata is still sent. This detection preserves converter
state, picture bytes and caller timestamps; it does not repair timestamps
already changed by a demuxer.

The production input/PES/ring regression in `make library-check` verifies
length widths 1/2/4, retained metadata injection, existing Annex-B compatibility
and bounded detection. It also executes on i386. On BCM70015, this restores
the first numbered picture in unmodified PowerVLC 2.1.0; the final four
pictures remain missing because full-file native drain is unresolved.
