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
The [GStreamer path](../gst/gst-plugin-1.0/README.md) remains the primary
playback baseline. DRM PRIME import/export and VPP smoke tests do not themselves
exercise CrystalHD hardware, and no libva conformance suite is run.

## Supported decode subset

The backend supports these progressive BCM70015 inputs:

- H.264 Constrained Baseline, Main and High. Reconstruction preserves each
  picture's effective 4x4 and active 8x8 scaling lists, including JVT and
  custom matrices. Omitted IQ buffers use flat-16 defaults for that picture;
  malformed buffers and zero active coefficients are rejected.
- MPEG-2 Simple I/P pictures through 720x576, and Main I/P/B pictures through
  1920x1088 coded size. Input must be 4:2:0 frame pictures with frame-predicted
  DCT, whole slices and no repeat-first-field or top-field-first flags. Picture
  parameters, inherited quantization matrices and start-coded slice bytes are
  preserved. Visible dimensions cannot change within a context.
- MPEG-4 Part 2 Simple/Advanced Simple levels 3/5, rectangular 8-bit 4:2:0,
  H.263 quantization and one whole slice per picture through 1920x1088.
  Simple-profile I/P video-packet resynchronization is accepted only when the
  policy remains fixed within a timing epoch and each interior packet header
  has a nonzero in-range, strictly increasing macroblock number, a nonzero
  quantizer and no header extension code. Packet payload bit alignment must
  match the reconstructed VOP. Advanced Simple must disable resynchronization
  markers. Sprites/GMC, quarter-pixel, MPEG quantization matrices, partitioning,
  RVLC and interlace are rejected. I/P/B references and timing survive bounded
  replay; a random-access discontinuity drains older transport epochs before
  starting the new one.
- VC-1 Advanced whole frames, headerless continuation slices and identical
  repeated picture headers. Changed per-slice headers, interlaced/field
  pictures, pan-scan, range mapping and separate in-loop output are rejected.
  A fixed-geometry I picture may introduce a new sequence/entry-point
  configuration.
- Standard WMV3 Simple/Main frame packets. Reconstructed STRUCT_C metadata
  uses X8 off with FASTTX and RTM set. VA does not expose the original reserved
  bits, so older RTM0 and other nonstandard variants cannot be identified
  reliably. Multi-resolution and sprite variants are outside this subset.

BCM70012 retains its H.264 path but is not recently hardware-validated. Other
codecs are rejected on that device. VA decode buffers omit some original timing
metadata: MPEG-2 reconstruction uses a 30-fps fallback and stable zero temporal
references, and VC-1 reconstruction cannot establish presentation cadence or
A/V synchronization. Client timestamps remain associated with their submitted
pictures.

## Replay and end of stream

BCM70015 firmware can retain output until later compressed pictures or a real
end-of-sequence marker arrive. After 100 ms without real input/output progress
during synchronization, the backend may seal the exact submitted batch with
EOS. New input waits until all
real output timestamps retire and drain completes through the firmware EOS
marker or, for MPEG-4, the library EOS state; continued input then reopens the
device and replays retained original access units.

H.264 replay starts at the last retained actual IDR. MPEG-2 retains the causal
I-picture history needed by the last two I/P anchors and pending or outstanding
pictures; an I picture is not assumed to close an open GOP. VC-1 retains BI and
skipped-P state because BI can affect later WMV3 rounding. References are
immutable accepted-picture tokens rather than reusable surface IDs. Unknown,
stale or out-of-window references are rejected, and completed replay pictures
are discarded without changing immutable client pixels. H.264 access-unit
delimiters are placed first in their timestamped packet so they cannot shift
firmware timestamp association.

The replay cache permits at most 512 KiB per access unit, 32 MiB or 512 access
units total, and 8192 replayed units before pruning an obsolete reference
prefix. Missing IDR/I-root history or an exceeded limit is an error. Completed,
non-outstanding ordinary B pictures may be omitted from MPEG-2 and VC-1 replay;
stateful BI and reference history required by an open GOP remain. Synchronous
one-picture-at-a-time clients can be slow because they may trigger frequent
reopen and replay. BCM70012 does not use this sealed-batch path.

For experimental long-running H.264 input, `CRYSTALHD_VAAPI_LIVE_H264=1`
allows completed transport history to retire within the same memory limits.
Queued input and outstanding pictures are never discarded. If the original
IDR prefix is no longer retained, continuation after EOS requires an actual
IDR; dependent input is rejected rather than decoded with missing references.
In that state synchronization does not infer EOS from an input-delivery gap:
the final picture may require subsequent input or explicit context draining.
The existing absolute synchronization deadline still applies.
The default remains strict replay. This opt-in is not a real-time streaming
support claim and does not affect other codecs or the library ABI.

Destroying a decoder context stops new submissions and drains accepted live
pictures before closing the device. The drain has a ten-second polling budget,
but individual firmware calls and cleanup can extend wall time. An infinite
`vaSyncSurface` request reports a decode error after ten seconds without its
picture. Completed decode surfaces survive context removal; destroyed or reused
surfaces and canceled VPP epochs report errors.

## Surface and export contracts

Decode targets may be VA-allocated NV12 surfaces or imported linear or
GBM-mappable DRM PRIME NV12 surfaces. CrystalHD YUY2 output is converted into
immutable, timestamped NV12 storage for the requested picture.

NV12 image buffers support `vaAcquireBufferHandle` and
`vaReleaseBufferHandle` with DRM PRIME DMA-BUF, also when no memory-type hint
is supplied. The handle is an independent
linear snapshot with the image's original pitches and offsets, not a zero-copy
alias of a decoded surface. Mapping, resizing, copying or destroying the image
or buffer is rejected while it is externally borrowed. Release waits for
external DMA-BUF work and copies external writes back into the image; a sync
failure invalidates that image's contents. Export allocation requires a
compatible linear GBM byte buffer, while CPU-only image access remains usable
without one.

NV12 PRIME2 exports default to separate R8 and GR88 layers. Request
`VA_EXPORT_SURFACE_COMPOSED_LAYERS` for one two-plane NV12 layer. Read/default
exports and derived snapshots wait for picture completion; cancellation or an
identity change reports an error. Write-only exports remain nonblocking for
target allocation.

Aliases imported from a known DMA-BUF share their decode or display owner so
completion remains synchronized. Every view of the same backing object must
use identical format, dimensions and planes. Decode targets must be canonical
surfaces, not imported aliases. A surviving alias of a destroyed owner is
invalid, and reimport remains busy until pending writes finish.

The minimal VPP path provides unfiltered NV12 scaling and NV12-to-ARGB
conversion. A worker can wait for reordered hardware output while later input
is submitted. VPP sources remain available until the decode surface is reused
or destroyed; a target with an outstanding writer is busy. Missing, canceled
or retired pictures report errors rather than substitute other pixels. Clients
must check VA surface status and must not treat fence signaling alone as proof
of valid pixels. ARGB is rendered privately before it is copied under the write
fence. A canceled or failed pending write closes its unsignaled timeline so its
fences fail with `-ENOENT`; successful writes flush pixel caches before the
timeline advances.

## Limitations

- VA-API supplies no explicit end-of-stream callback; bounded batch replay
  covers the tested BCM70015 files, not arbitrary streams or BCM70012
- only the decode, image, DRM PRIME, and minimal video-processing operations
  needed by the documented clients are implemented; `vaPutSurface`,
  subpictures, palettes, and detailed surface-error reporting are unavailable
- maximum coded size 1920x1088; MPEG-2 Simple is limited to 720x576
- NV12 images are limited to 1920x1088; odd dimensions retain complete UV pairs.
  Image copies reject busy surfaces and invalid rectangles. `vaPutImage` also
  rejects retained decode pictures and their aliases instead of invalidating
  decoder reference identity. `vaDeriveImage` remains a readback snapshot, not a
  writable direct alias; use `vaCreateImage`/`vaPutImage` for ordinary uploads.
- one CrystalHD playback session at a time; another client receives hardware
  busy until the active decoder closes
- video processing is limited to unfiltered NV12 scaling and NV12-to-ARGB
  conversion; there is no deinterlacing or rotation
- no VP8, VP9, AV1, or protected content
- direct rendering requires writable NV12 DRM PRIME buffers
- the VA-API backend is not a general display driver; it uses an existing DRM
  render node for surface allocation while CrystalHD performs supported decoding
- browser hardware decode remains experimental: the GPU sandbox does not
  broker the CrystalHD device or firmware, and asynchronous VPP needs the
  debugfs `sw_sync` timeline for compositor fences. The driver fails rather
  than change permissions or substitute unrelated pixels. The
  `crystalhd-chromium` launcher therefore defaults to software decoding;
  `CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` enables the experimental path.

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
and distinct picture timestamps. Open-GOP seeking requires the client to supply
earlier reference pictures: a demuxer seek to a later I picture can skip leading
B pictures even in software decoding. The probe does not certify arbitrary
files' seek indices or preroll.

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
This checks the backend's export-read contract, not GUI presentation, audio,
subtitles or playback-rate handling.
