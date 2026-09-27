# CrystalHD VA-API driver

This backend exposes H.264 decoding through the standard VA-API VLD interface,
with progressive MPEG-2 Simple/Main, MPEG-4 Part 2 Simple/Advanced Simple,
standard WMV3 Simple/Main and VC-1 Advanced support on BCM70015. BCM70012
retains its H.264 path but is not recently hardware-validated; the other
codecs are rejected on that device.
The backend accepts VA-allocated NV12 surfaces and imported
linear or GBM-mappable DRM PRIME NV12 surfaces for FFmpeg-style clients. It
also allocates and exports ARGB DRM PRIME surfaces for Chromium-class clients
whose compositor cannot render NV12 directly.

This is an experimental, client-oriented VA-API subset rather than a complete
VA-API implementation. See the [hardware report](../../HARDWARE-2026-09-13.md)
for tested BCM70015 profiles, resolutions, throughput, pixel comparisons and
seek/lifetime results; these are not general codec or client conformance claims.
An infinite `vaSyncSurface` request reports a decode error after 10 seconds
without its picture, instead of hanging or substituting another frame.
The [GStreamer path](../gst/gst-plugin-1.0/README.md) remains the primary
playback baseline. DRM PRIME import/export and VPP smoke tests do not themselves
exercise CrystalHD hardware, and no libva conformance suite is run.

## Decode and surface contracts

H.264 High reconstruction preserves each picture's effective 4×4 and active
8×8 IQ scaling lists, including JVT and custom matrices. Omitted IQ buffers
start from flat-16 defaults for that picture, never a previous picture's
matrix; malformed buffers and zero active coefficients are rejected before
decoder submission. See [the scaling-matrix validation](https://github.com/ahnhy1324/crystalhd/issues/34)
for actual pixel comparisons and remaining codec limits.

MPEG-2 reconstruction preserves picture parameters, effective quantization
matrices and exact start-coded slice bytes. Matrix updates follow MPEG-2
inheritance rules rather than H.264's per-picture flat defaults. Simple supports
I/P pictures up to 720×576; Main adds B pictures up to 1920×1088 coded size,
including 1920×1080 visible pictures in aligned allocations. The initial subset
requires progressive 4:2:0 frame pictures with frame-predicted DCT, whole slices,
and no repeat-first-field or top-field-first presentation flags. A context's
visible dimensions cannot change after its first accepted picture.
VA's MPEG-2 decode buffers do not supply the original frame rate or temporal
reference. Reconstruction uses a 30 fps fallback and stable zero temporal
references; submitted timestamps identify actual pictures across replay, while
the client retains its presentation timestamps. This is not an A/V-clock claim.

Selected BCM70015 Simple/Main, open/closed-GOP, FHD, custom-matrix and longer
streams produced complete NV12 output byte-identical to an independent scalar
conversion of the original streams' GStreamer YUY2 output. This validates those
decode fixtures, not general MPEG-2 conformance, physical display/audio timing,
or parity with the GStreamer path. Selected seek/reopen and retained-frame
tests also pass; see
[issue #36](https://github.com/ahnhy1324/crystalhd/issues/36) for the exact matrix
and remaining acceptance checks.

MPEG-4 reconstruction consumes the standard VA picture and slice buffers and
builds the VOS/VOL/GOV/VOP syntax required by CrystalHD. It supports progressive
rectangular 8-bit 4:2:0 Simple/Advanced Simple levels 3/5, H.263 quantization,
one whole slice per picture and coded sizes through 1920x1088. Resynchronization
markers, sprites/GMC, quarter-pixel, MPEG quantization matrices, partitioning,
RVLC and interlace are rejected. Advanced Simple I/P/B references and timing
are retained across bounded replay; random-access timing discontinuities drain
older transport epochs in order without blocking `vaEndPicture`.

VC-1 Advanced reconstruction preserves compressed macroblock bits and rebuilds
complete progressive headers and bitplanes. It accepts whole frames,
headerless continuation slices and identical repeated picture headers, not
changed per-slice headers. Fixed-geometry I pictures can introduce a new
sequence/entry-point configuration; their own headers remain in replay.
Interlaced/field pictures, pan-scan, range mapping and separate in-loop output
are unsupported. VA omits original display timing; reconstruction does not
establish presentation cadence or A/V synchronization.

WMV3 preserves complete frame packets and reconstructs standard Simple/Main
STRUCT_C metadata (X8 off, FASTTX and RTM set). VA omits these original reserved
bits, so older variants cannot reliably be identified or reconstructed here.
An early RTM0 WMV9 sample demonstrably differs with standard RTM1 metadata.
GStreamer retains the original metadata; older variants still need per-file
hardware validation.
Multi-resolution and nonstandard/sprite variants are outside this VA subset.
Selected standard Simple/Main and Advanced SD/FHD, I/P/B/BI/skipped-P and
multi-slice fixtures match original-stream hardware pixels with complete output
and firmware EOS. Selected Main/Advanced seek and retained-frame tests pass;
see [issue #40](https://github.com/ahnhy1324/crystalhd/issues/40) for coverage and
remaining limits, not a general conformance or daily-use claim.

BCM70015 firmware retains output until later compressed pictures or a real
end-of-sequence marker arrive. After 100 ms of synchronization grace, the
backend may seal the exact submitted batch with EOS. New input is queued until
all real output timestamps and the firmware EOS marker are received. If input
continues, the complete device is reopened and retained original access units
rebuild reference state. H.264 starts from the last retained actual IDR. MPEG-2
retains the causal I-picture history needed by the last two I/P anchors and
pending or outstanding pictures; an I picture is not assumed to close an open
GOP. VC-1 also retains BI and skipped-P state; BI is not a reference anchor but
can change subsequent WMV3 rounding. References are captured as immutable
accepted-picture tokens, not
re-resolved from reusable surface IDs during replay. Unknown, stale or
out-of-window references are rejected. Replayed completed pictures are discarded
before touching immutable client pixels. H.264 access-unit delimiters are first
in their own timestamped packet; trailing delimiters shifted firmware timestamp
association. Decoder-only resets were tested and rejected after a firmware
stall; full device reopen passed the validated H.264 sequence.

The replay cache allows at most 512 KiB per access unit, 32 MiB / 512 access
units total, and 8192 replayed units before an obsolete reference prefix is
pruned. Missing IDR/I-root history or exceeded limits produces an error. MPEG-2
and VC-1 can omit completed, non-outstanding ordinary B pictures from replay,
but retain stateful BI and reference history still needed by an open GOP.
This never drops a picture's initial decode or requested output. Replay is not
free: a client that downloads each picture before submitting the next may
reopen and replay for nearly every picture. The 100 ms grace helps clients
with concurrent input; it cannot manufacture lookahead for synchronous ones.
BCM70012 does not use the unvalidated sealed-batch path.

Decoder-context destruction stops new submissions and drains already-accepted
live pictures before closing the device, so queued FFmpeg output frames retain
their actual pixels. The drain has a ten-second polling budget; individual
firmware calls and cleanup can extend wall time, so hardware tests also use
an external timeout. Completed plain decode surfaces survive context removal;
destroyed/reused surfaces and canceled VPP epochs still report errors.

NV12 image buffers support `vaAcquireBufferHandle`/`vaReleaseBufferHandle`
with a DRM PRIME DMA-BUF (also selected when no memory-type hint is supplied).
This is an independent, linear snapshot with the image's original pitches and
offsets, not a zero-copy alias of the decoded surface. Mapping, resizing,
copying or destroying the image/buffer is rejected while externally borrowed.
Release waits for external DMA-BUF work and copies external writes back into
the image only; synchronization failure invalidates that image's contents.
Allocation requires a compatible linear GBM byte buffer. CPU-only image access
continues to work when such an export cannot be allocated.

NV12 PRIME2 surface exports default to separate R8/GR88 layers; explicitly
request `VA_EXPORT_SURFACE_COMPOSED_LAYERS` for one two-plane NV12 layer.
Read/default exports and derived snapshots wait for the selected picture's
completion. A concurrent cancellation or identity change reports an error;
write-only exports remain nonblocking for target allocation. These operations
cover specific VLC compatibility requirements, not a complete VLC/Chromium
playback or display-conformance claim.

Chromium may export a VA surface and then reimport the same DMA-BUF under a
different surface ID for video processing. The driver links those aliases to
the original decode or display owner so input completion and output readiness
remain synchronized before the compositor presents a frame.

BCM70015 hardware output is YUY2, which the driver converts to the requested
NV12 client surface. A minimal CPU video-processing path uses libswscale's
SIMD conversion for NV12-to-ARGB output when Chromium needs a
compositor-compatible export. CrystalHD needs later compressed pictures before
it emits some reordered output. Each submission therefore gets immutable,
timestamped NV12 storage independent of Chromium's reusable VA surface. A VPP
worker waits for hardware pictures that are still reordered, allowing Chromium
to keep submitting input while output is pending. Pictures already emitted by
CrystalHD are converted directly before `vaEndPicture` returns. VPP sources
are captured when `vaRenderPicture` supplies the parameters, and remain
available for repeated VPP until the decode surface is reused or destroyed.
Decoder generations are checked after every hardware wait, so retired pictures
cannot become successful new-epoch output. A target with an outstanding writer
rejects reuse as busy. ARGB is first rendered into private memory and only then
copied into the shared DMA-BUF under the write fence. Missing or
canceled pictures report errors; they are never replaced by unrelated fallback
pixels. Destroyed surface objects are permanently made non-writable before
their VA IDs are released. Canceled or failed pending writes close their
unsignaled timeline, releasing its fences with `-ENOENT` instead of falsely
signaling success. Successful writes flush pixel caches before incrementing the
timeline; a later CPU-access cleanup error is reported through VA status but
cannot undo an already-signaled fence. Clients must therefore check VA surface
status, not treat fence signaling alone as proof of valid pixels.
Hardware-free production-state tests
cover repeated VPP, source reuse between parameter submission and completion,
decoder retirement, busy targets, and cancellation. These fix and verify driver
lifecycle bugs, not end-to-end browser playback: published FFmpeg drain and
seek results do not establish browser hardware seek correctness.

## Limitations

- VA-API supplies no explicit end-of-stream callback; bounded batch replay
  covers the tested BCM70015 files, not arbitrary streams or BCM70012
- only the decode, image, DRM PRIME, and minimal video-processing operations
  needed by the documented clients are implemented; `vaPutSurface`,
  subpictures, palettes, and detailed surface-error reporting are unavailable
- progressive H.264 Constrained Baseline, Main, and High; MPEG-2 Simple/Main,
  MPEG-4 Simple/Advanced Simple, and standard WMV3 Simple/Main / VC-1 Advanced
  on BCM70015 only, with the picture and legacy-variant restrictions above
- maximum coded size 1920x1088; MPEG-2 Simple is limited to 720x576
- NV12 images are limited to 1920x1088; odd dimensions retain complete UV pairs.
  Image copies reject busy surfaces and invalid rectangles. `vaPutImage` also
  rejects retained decode pictures and their aliases instead of invalidating
  decoder reference identity. `vaDeriveImage` remains a readback snapshot, not a
  writable direct alias; use `vaCreateImage`/`vaPutImage` for ordinary uploads.
- imports sharing any known backing object must describe identical format,
  dimensions, and plane views. Nonidentical views are rejected; surviving aliases
  of a destroyed owner are invalid, and reimport is busy until pending writes end.
  Decode targets must be canonical surfaces, not imported aliases; aliases
  remain usable for the supported image and VPP operations.
- one CrystalHD playback session at a time; another client receives hardware
  busy until the active decoder closes
- video processing is limited to unfiltered NV12 scaling and NV12-to-ARGB
  conversion; there is no deinterlacing or rotation
- no VP8, VP9, AV1, or protected content
- direct rendering requires writable NV12 DRM PRIME buffers
- the VA-API backend is not a general display driver; it uses an existing DRM
  render node for surface allocation while CrystalHD performs supported decoding
- earlier browser hardware tests showed incorrect post-seek pixels; driver
  lifecycle fixes now pass regressions, but browser hardware seek correctness
  still needs validation. The `crystalhd-chromium` launcher therefore defaults
  to `FFmpegVideoDecoder` and
  requires `CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` for browser VA-API
- browser hardware access has two distinct unresolved integration barriers:
  Chromium's GPU sandbox does not broker the CrystalHD device and firmware
  files, while asynchronous VPP additionally needs the debugfs `sw_sync`
  timeline to fence compositor reads. Disabling the GPU sandbox alone does
  not grant that timeline's filesystem permissions. The driver reports an
  error if it cannot fence pending VPP; it does not change permissions or
  substitute a synchronous decode wait inside `vaEndPicture`. Default browser
  software decoding keeps the GPU sandbox enabled.

## Build and basic decode

For an installed build, choose one of the render nodes reported by
`crystalhd-check`, then verify driver discovery without source-tree paths:

```sh
drm_node=/dev/dri/renderDXXX
LIBVA_DRIVER_NAME=crystalhd \
vainfo --display drm --device "$drm_node"
```

This proves libva discovery and initialization, not compressed-stream decode.
If the machine has multiple render nodes, select the display GPU explicitly.

For a source-tree test, build and inspect the driver from the repository root:

```sh
drm_node=/dev/dri/renderDXXX
make -C linux_lib/libcrystalhd
make -C filters/vaapi check
LIBVA_DRIVER_NAME=crystalhd \
LIBVA_DRIVERS_PATH=$PWD/filters/vaapi \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
vainfo --display drm --device "$drm_node"
```

Hardware-decode a file with FFmpeg:

```sh
LIBVA_DRIVER_NAME=crystalhd \
LIBVA_DRIVERS_PATH=$PWD/filters/vaapi \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
ffmpeg -hwaccel vaapi -hwaccel_device "$drm_node" \
  -hwaccel_output_format vaapi -i video.mp4 \
  -vf hwdownload,format=nv12 -f null -
```

## Hardware stress

Stop all other CrystalHD playback clients first. Generate the three H.264
profiles using the [fixture recipe](../gst/gst-plugin-1.0/README.md#counted-h264-playback-and-replay),
then run each file through the hardware harness:

```sh
./tests/vaapi-hardware-stress.sh /tmp/crystalhd-samples/high.mp4 10
# Exercise decoder teardown within one FFmpeg process (five input loops).
CRYSTALHD_TEST_INPUT_LOOPS=5 \
  ./tests/vaapi-hardware-stress.sh /tmp/crystalhd-samples/high.mp4 1
```

Each run requires the complete frame count on NV12 hardware surfaces and scans
new kernel messages. `CRYSTALHD_TEST_TIMEOUT` defaults to 120 seconds per run;
a timeout or missing frame fails validation. The script checks the source-built
module's identity, loads it only if needed, and unloads it only if it loaded it.
It refuses a different already-loaded version. See
[module identity](../../BRINGUP.md#device-access-and-module-identity) rather than
assuming an installed module is the one in use.

## Seek, flush and retained frames

The probe also accepts progressive MPEG-2 Simple/Main, MPEG-4 Simple/Advanced
Simple and VC-1/WMV3 in seekable containers with reliable declared frame counts
and distinct picture timestamps.
The MPEG-2 checks are tracked in
[issue #36](https://github.com/ahnhy1324/crystalhd/issues/36). Open-GOP seeking
requires the client to supply earlier reference pictures: a demuxer seek to a
later I picture can skip leading B pictures even in software decoding. These
selected tests do not certify arbitrary files' seek indices or preroll.

Install the `libavcodec`, `libavformat`, `libavutil` and `libva` development
packages, then build the optional probe:

```sh
make -C filters/vaapi seek-test
LIBVA_DRIVER_NAME=crystalhd LIBVA_DRIVERS_PATH="$PWD/filters/vaapi" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=10 120s filters/vaapi/vaapi-seek-test \
  /tmp/crystalhd-samples/high.mp4
```

The default synchronous test requires hardware frames, drains a complete
reference decode, then compares NV12 pixel SHA-256 and PTS after forward and
backward seeks and decoder flushes, including a flush with reordered pictures
pending. `--software` is an explicitly labelled self-check of the probe, not
CrystalHD validation. Keep the external timeout because firmware calls and
cleanup can exceed an in-process polling deadline.

Append `--lookahead 8` for a pipelined-client check: it retains eight future
output frames before downloading the oldest, leaving an undownloaded suffix at
each seek. Report this separately from synchronous mode, which can be much
slower because it feeds no future input while waiting for a picture.

Use `--retain-old-frames` to retain eight original frame owners across flush
and new input, or actual decoder-context destruction, before checking their
original PTS/pixel hashes. This implies eight-frame lookahead and tests lifetime
after teardown, unlike `--lookahead 8` alone.

To check the exported-pixel consumer path without opening a GUI, use a generated
H.264 MP4 with a declared frame count:

```sh
make -C filters/vaapi seek-test
LIBVA_DRIVER_NAME=crystalhd LIBVA_DRIVERS_PATH="$PWD/filters/vaapi" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=5s 210s filters/vaapi/vaapi-seek-test video.mp4 \
  --export-prime --retain-old-frames
```

The probe first records synchronized downloads, then compares complete-file
and sought/retained exported pixels and PTS without a prior `vaSyncSurface`.
It requires genuine linear DMA-BUFs and never accepts software fallback.
This checks the backend's export-read contract, not VLC's GUI, EGL compositor,
audio clock, subtitles or playback-rate handling.

After `sudo make install`, libva discovers the driver from the system DRI
directory. Keep `LIBVA_DRIVER_NAME=crystalhd` and the render device selection;
the source-tree `LIBVA_DRIVERS_PATH` and `LD_LIBRARY_PATH` overrides are no
longer needed.

The DRM render node normally belongs to the machine's display GPU. That GPU
allocates and displays surfaces; CrystalHD performs the supported video
decode. For persistent Chrome integration, including codec preference and
desktop default-browser registration, see the **Chrome and YouTube** section
of the top-level [README](../../README.md).
