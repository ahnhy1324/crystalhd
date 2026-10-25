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

## Optional output readiness and RX pool

`DtsDupOutputPollFd()` is an additive API that returns a caller-owned,
close-on-exec duplicate descriptor when the loaded driver advertises RX
polling. `POLLIN` is level-triggered while the active decoder owner has queued
output; close, removal and session-lifetime changes wake a waiter with
`POLLERR` or `POLLHUP`. Older drivers return `BC_STS_NOT_IMPL`. The VA backend
weak-links this symbol and retains its one-millisecond fallback with an older
library or driver. The `libcrystalhd.so.3` ABI and legacy ioctl behavior are
unchanged.
The GStreamer worker also negotiates this optional interface, retaining its
five-millisecond compatibility checks and bounded library EOS/recovery probes.

`CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT` accepts exactly `8` or `16`; unset or
invalid values use the default of 16. Eight buffers reduced average peak RSS
by 11.7% in the paced BCM70015 720p60 screening without a repeatable task-time
or context-switch change, so it remains an explicit opt-in. See the
[hardware report](../../HARDWARE-2026-09-13.md) for the bounded comparison.

For isolated source-build qualification, `CRYSTALHD_EXPERIMENTAL_SCALED_RX`
is a **compile-time** experiment, separate from the pool-count setting:

```sh
make library CXXFLAGS='-O2 -DCRYSTALHD_EXPERIMENTAL_SCALED_RX'
```

It sizes BCM70015 playback/MOVE YUY2 backing from a successfully admitted
H.264 scaling command with matching progressive/interlaced width fields
(even widths 128–1918), retaining the conservative 1090-row bound.
Other layouts use the full-size fallback. With this build,
all playback/MOVE payload allocation, including fallback, moves from device
open to first map/start; allocation failure can therefore occur there.
DIAG/non-MOVE allocation remains eager. Replacement requires retired DMA
ownership; uncertain unmap/stop retains backing and fails closed.
After confirmed retirement, this experimental build also returns complete
pages inside the RX payload to the kernel before freeing it. This reduces
resident pages retained by the allocator without trimming the application's
other allocations. Quarantined or active backing is never discarded; the
ordinary fixed build is unchanged. See the
[reopen and memory observations](../../HARDWARE-2026-09-13.md#rx-pool-retirement-and-reopen-2026-10-24).
Use only a private build, not a system-library replacement. This is not a
new playback option or default: BCM70012 hardware, suspend/resume and
mid-stream resizing are not qualified. Scope and measured results remain in
[#470](https://github.com/ahnhy1324/crystalhd/issues/470).

## Direct-library drain validation

This optional probe bypasses GStreamer and VA-API. It checks complete picture
delivery and the codec's drain-completion contract, not pixel quality or
displayed cadence.
Install the FFmpeg `libavformat`, `libavcodec`, `libavutil` and GLib
development packages. Use a progressive fixture with an independently known
decoded-frame count:

```sh
make library-drain-test
ffprobe -v error -select_streams v:0 -count_frames \
  -show_entries stream=codec_name,profile,width,height,field_order,nb_read_frames \
  "/path/to/video.h264"
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
  tests/library-drain-test --preflight "/path/to/video.h264" 30
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
  timeout --kill-after=10s 60s \
  tests/library-drain-test --hardware "/path/to/video.h264" 30 35
```

Replace `30` in both commands with the fixture's expected picture count.
`--preflight` never opens the card: it validates supported framing and the
demuxed packet count, assuming one complete progressive picture per packet.
It is not itself a software decode or proof of error-free media.
`--hardware` requires an idle BCM70015 and a matching loaded module; see
[device access and module identity](../../BRINGUP.md#device-access-and-module-identity).
It verifies that EOS was clear immediately before `DtsFlushInput(0)`, then
requires every expected timestamped picture, `DtsIsEndOfStream`, empty
pending/ready queues and successful stop/close. For H.264, MPEG-2, VC-1 and
WMV3, EOS derives from the firmware timing marker, which need not be returned
as a separate client-owned output buffer. MPEG-4 Part 2 additionally permits
the library's idle-fence completion: the explicit drain TX must retire, and
all output buffers and queues must remain free and idle for one second. The
probe labels this `library-EOS` rather than claiming a firmware marker.
Input inactivity alone never counts as EOS.

Supported input is raw Annex-B H.264, MPEG-2 elementary stream, raw VC-1
Advanced, WMV3 in ASF with four-byte sequence metadata (a fifth trailing byte
is tolerated), or progressive MPEG-4 Part 2 Simple/Advanced Simple levels 3/5
in MP4. MPEG-4 admission reuses the production GStreamer parser and requires
unchanged global headers and one supported coded picture per packet; its
[profile restrictions](../../filters/gst/gst-plugin-1.0/README.md#mpeg-4-part-2-input)
also apply here. MP4 H.264, raw/AVI MPEG-4, RCV containers and known interlaced
input are rejected.
Fixtures are limited to 64 MiB, 10,000 pictures and 1920x1088; each packet plus
framing must fit the transmit ring.
The in-process timeout defaults to 30 seconds and accepts at most 300. Keep an
external timeout because blocked ioctls or device close cannot be interrupted
by the probe's deadline. The probe does not load or replace the module and is
not part of `make check`. For hardware results, use the
[hardware report](../../HARDWARE-2026-09-13.md).

## Raw YUY2 copy contract

For 32-bit i686 CPUs without SSE/SSE2, build with `LEGACY_CPU=1` (see
[other installation modes](../../README.md#other-installation-modes)). This
keeps the library ABI but selects the i686 scalar/x87 build with SSE, SSE2 and
MMX disabled; it does not bypass the player's own CPU or dependency
requirements. The dedicated legacy check runs both production sections and
the built shared library under a verified no-SSE CPU model, without opening
hardware.

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

## Experimental raw-frame producer

The opt-in
[`libcrystalhd_raw_frame.h`](libcrystalhd_raw_frame.h) API produces a fixed
256x96, 30 fps, 8-bit YUV420P H.264 stream. It can upload two retained source
frames, compose 16x16 tiles from them and apply bounded quarter-pixel
translation. Non-reference composition and translation leave both source slots
unchanged; the fixed POC type 2 profile permits at most one non-reference access
unit between committed reference access units.

This is a standard-H.264 producer, not standalone MFD/scaler processing, a raw
device-surface API or an on-card surface lease. It requires a fresh,
noninterleaved decoder session, and the caller still owns device setup, pacing,
timestamps, output buffers, EOS, drain, stop and close. Hardware qualification
covers sparse uploads plus composition, not sustained 30 fps full-plane
uploads.

Run `make raw-frame-check` for the device-free producer contract tests. The
public header is authoritative for prepare/commit/abort semantics and argument
limits.
