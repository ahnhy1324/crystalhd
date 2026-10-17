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
For the separately characterized live-stream integration, see the top-level
[Moonlight/Sunshine guide](../../MOONLIGHT.md).

## Supported decode subset

The backend supports these progressive BCM70015 inputs, plus the selected
H.264 MBAFF case described below:

- H.264 Constrained Baseline, Main and High. Reconstruction preserves each
  picture's effective 4x4 and active 8x8 scaling lists, including JVT and
  custom matrices. Omitted IQ buffers use flat-16 defaults for that picture;
  malformed buffers and zero active coefficients are rejected. Selected MBAFF
  field pairs emitted from one submitted VA frame/access unit are materialized
  as a complete interlaced NV12 Weave frame; separately submitted PAFF fields,
  VA VPP deinterlacing and pulldown are not supported.
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

## Interlaced H.264, Weave and client deinterlacing

For a supported MBAFF pair, the hardware returns one packed 4:2:2 buffer per
field. The VA driver writes field luma into alternating display rows and
downsamples chroma between adjacent rows of the same field before interleaving
the field chroma rows into NV12. Averaging chroma across temporally different
top and bottom fields would instead create coloured comb artifacts. This
operation is Weave output, not Bob or motion-adaptive deinterlacing.

Earlier unpublished RC.2 payload `f4d99d5` fully materialized the selected
750-picture 1920x1080i25 fixture twice; every luma plane matched the software-decoded
reference after common normalization, while deterministic chroma differences
remained from this field-aware conversion. The exact fixture, pixel, timing,
resource and teardown results are in the
[hardware report](../../HARDWARE-2026-09-13.md#h264).

Standalone FFmpeg CPU BWDIF after `hwdownload` completed the same fixture, but
the driver still advertises no `VAProcFilterDeinterlacing` capability. See the
[runnable command and performance boundary](../../USAGE.md#interlaced-h264-weave-and-ffmpeg-bwdif).
Stock Kodi 21.3 progressive hardware-video and lifecycle checks passed;
quantified physical A/V remains unqualified. Earlier RC.2 payload `f4d99d5` failed
interlaced BWDIF/Render Bob playback. The `ec77b71` finite MBAFF input/tail fix
is included in published RC.3, with complete decoder-oracle results, but Kodi
source picture accounting and timing/output warnings still need qualification. See
[Kodi qualification and remaining limits](../../USAGE.md#kodi). The driver
does not advertise VA VPP deinterlacing; no general Kodi interlace or
hardware-Bob support is claimed.

## Process VPP opt-out

The development backend recognizes `CRYSTALHD_VAAPI_DISABLE_VPP=1`, snapshotted
once at VA driver initialization. Only exactly `1` disables VPP; unset and
other values preserve the default. An opted-out instance omits the VPP profile
and entrypoint and rejects its config/attribute requests before allocating
surfaces or a context. Decode capabilities remain available. This can reduce
the cost of repeated unsupported deinterlacing probes in clients such as Kodi;
it cannot stop a client's retries or supply deinterlacing.

The restriction also disables legitimate unfiltered NV12 scaling and
NV12-to-ARGB conversion for that process. Keep it unset for applications that
need those operations. It is an explicit runtime-only invocation, not a
persisted `crystalhd-config` key. Use Kodi Off/NONE for intended Weave playback;
an initial capability probe may still occur. See the
[optional invocation and qualification boundary](../../USAGE.md#kodi).
This change is absent from published `v1.1.0-rc.3`; actual Kodi 22 performance
with the opt-out remains unverified.

## Replay and end of stream

BCM70015 firmware can retain output until later compressed pictures or a real
end-of-sequence marker arrive. For strict H.264 and the other codecs, the
backend may seal the exact submitted batch after 100 ms without real
input/output progress during synchronization. New input waits until all real
output timestamps retire and drain completes through the firmware EOS marker
or, for MPEG-4, the library EOS state; continued input then reopens the device
and replays retained original access units.

H.264 replay starts at the last retained actual IDR. MPEG-2 retains the causal
I-picture history needed by the last two I/P anchors and pending or outstanding
pictures; an I picture is not assumed to close an open GOP. VC-1 retains BI and
skipped-P state because BI can affect later WMV3 rounding. References are
immutable accepted-picture tokens rather than reusable surface IDs. Unknown,
stale or out-of-window references are rejected, and completed replay pictures
are discarded without changing immutable client pixels. H.264 access-unit
delimiters are placed first in their timestamped packet so they cannot shift
firmware timestamp association.

The replay cache permits at most 8 MiB per H.264 access unit and 512 KiB per
MPEG-2, MPEG-4 or VC-1 access unit, with 32 MiB or 512 access units total and
8192 replayed units before pruning an obsolete reference prefix. Large H.264
units are submitted automatically through bounded 512 KiB libcrystalhd
fragments; applications must not split them. They require a matching
libcrystalhd that exports `DtsProcInputFragment`; with an older library, the VA
driver rejects the large unit before submitting a partial picture. Missing
IDR/I-root history or an exceeded limit is an error. Completed, non-outstanding
ordinary B pictures may be omitted from MPEG-2 and VC-1 replay; stateful BI and
reference history required by an open GOP remain. Synchronous
one-picture-at-a-time clients can be slow because they may trigger frequent
reopen and replay. BCM70012 does not use this sealed-batch path.

With `CRYSTALHD_VAAPI_LIVE_H264` unset, H.264 uses bounded transport history:
completed history may retire within the limits above, while queued input and
outstanding pictures are never discarded. While the retained prefix remains
replayable the ordinary 100 ms rule applies. After that prefix has retired on
BCM70015, only time actually spent waiting without progress inside VA sync
calls is accumulated, and EOS is deferred to one second. Time between calls,
including a player pause, is not counted. If this bounded grace eventually
seals an unreplayable stream, continuation requires an actual IDR; dependent
input is rejected rather than decoded with missing references.

For experimental long-running H.264 input,
`CRYSTALHD_VAAPI_LIVE_H264=1` also enables the direct decode-output path and,
after a second original picture is accepted, does not infer EOS from an
input-delivery gap. Final pictures then require subsequent input or explicit
context drain; the absolute ten-second synchronization deadline still applies.
For finite files with stock FFmpeg, leave this variable unset:
`vaSyncSurface()` waits for one surface and is not an EOF or drain signal. In
explicit live mode the last reordered pictures can therefore remain held until
subsequent input, or until a client retains the output surfaces and calls
`vaDestroyContext()` to perform the explicit drain before downloading them.
Setting the variable to `0`, or to any value other than exactly `1`, selects
strict replay.
An initial one-picture decoder probe remains eligible for inactivity-based
EOS. These policies do not affect other codecs, BCM70012's existing path or
the library ABI, and the explicit live mode is not a general real-time
streaming support claim.

Finite-policy BCM70015 MBAFF frame pictures automatically send the next
timestamped AUD/filler prefix, avoiding a wait for the next complete input AU.
This does not predict slice data or complete a missing field. The last picture
still uses the ordinary real-EOS drain above. Progressive pictures, separately
coded fields/PAFF, other codecs, BCM70012 and explicit live mode do not gain
automatic prefeed. Set `CRYSTALHD_VAAPI_LOW_LATENCY_H264=0` to disable it.

On BCM70015, `CRYSTALHD_VAAPI_LOW_LATENCY_H264=1` explicitly enables H.264
input prefeeding independently of the LIVE policy. With LIVE unset, finite
sealing/replay remains available; with `CRYSTALHD_VAAPI_LIVE_H264=1`, input gaps
remain nonterminal and the client must explicitly close/drain at stream end.
The explicit live+prefeed combination reduced decode latency by one input
cadence in a low-delay 720p30 hardware test. VA-API frame-rate signaling remains
fixed at 30 fps; prefeed alone does not establish real-time playback or physical
A/V synchronization support.

`CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP=1` enables a further BCM70015-only
presentation experiment only with explicit `LIVE_H264=1` and
`LOW_LATENCY_H264=1`. Selection is
`previous` by default; `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY=latest`
selects the newest eligible older picture. The optional
`CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US` accepts exactly `0`, `16667`,
`33333`, or `50000` (default `0`). Any other policy or delay value disables
remapping. This path deliberately exports an older completed picture through
the requested surface and timestamp, so it does not preserve standard VA
surface identity. It is a working, default-off streaming experiment: see the
[previous-frame setup and rollback](../../MOONLIGHT.md#experimental-previous-frame-mode)
for matched GUI settings and timing tradeoffs. The default frame-matching
path and BCM70012 behavior are unchanged.

### Measured reference-host limits

With both H.264 options enabled and the documented host-specific L0s workaround
already active, low-delay decode plus NV12 download reached about 83 fps at
1280x720, 57 fps at 1600x900 and 42 fps at 1920x1080 on the BCM70015 reference
host. The observed 60 fps boundary lay between 1536x864 and 1568x882. Input
rates from 5 to 20 Mbit/s did not materially change the 720p result.

In the historical three-run unmodified-Moonlight 720p60 series with **V-sync
and frame pacing both on**, software submitted
41.06–41.43 frames/s, CrystalHD live-only submitted 18.94–19.15, and
live+prefeed submitted 32.05–35.49. A separate paired steady trace measured
about 60.02 and 60.00 CrystalHD outputs/s respectively while prefeed reduced
real access-unit-to-matching-output p50/p95 from 36.05/45.12 ms to
21.25/25.13 ms. Render rate is submission rate, not physical scanout, and
Moonlight's component sum is not measured input-to-photon latency. An earlier
native-output 1080p60 run accumulated a multi-second decoder backlog. These
figures describe the default frame-matching/live-prefeed path. The separate
previous-frame experiment reached near-60 renderer handoff at 720p60 with
**V-sync and pacing both off**. Its repeated finalists did not include an
exact-identity hardware control: these two series are not a matched A/B and
do not establish a settings-independent default-mode ceiling. Neither series
measured physical scanout.
Methods, resource use and measurement boundaries are in
the [hardware report](../../HARDWARE-2026-09-13.md#va-api-decode-and-game-stream-baseline-2026-10-10).

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

`CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC=1` is an optional reference-host
optimization for CPU writes into VA-allocated i915 dumb surfaces. It uses the
i915 WC/GTT ownership transitions only for the synchronous YUY2-to-NV12 copy;
imported surfaces, VPP and other DRM drivers retain the normal DMA-BUF sync
path. The default is off. It improved CPU time only when pixels were read back
in the measured test and is not a general display-latency setting.

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
  conversion; there is no VA VPP deinterlacing or rotation. Selected H.264
  field pairs use the decode-time Weave path described above
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

## Diagnostic controls

These process-local controls are for debugging, not ordinary playback:

| Variable | Effect |
| --- | --- |
| `CRYSTALHD_VAAPI_DEBUG` | Any presence, including `0`, enables verbose stderr and can dump the first 64 bytes of an access unit. Use `unset CRYSTALHD_VAAPI_DEBUG` to disable it. |
| `CRYSTALHD_VAAPI_TRACE=1` | Emits JSONL identity and timing events to stderr. Other values leave tracing off. Trace probes can perturb timing. |

Do not publish trace or debug output without checking it for media bytes,
paths and application metadata.

## Chromium launcher experiment

Chromium hardware decoding is less mature than the documented Moonlight path.
The launcher defaults to software video decoding and should remain that way for
ordinary browsing. Hardware mode requires disabling the GPU sandbox, which is
a material security reduction, and YouTube hardware decode/A/V synchronization
has not been accepted.

From the repository root, install the launcher separately from the core
runtime:

```sh
sudo make install-browser
```

For a temporary, trusted test only:

```sh
test_profile=$(mktemp -d /tmp/crystalhd-browser.XXXXXX)
CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1 \
CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1 \
CRYSTALHD_DRM_DEVICE=/dev/dri/renderDXXX \
CRYSTALHD_CHROMIUM_PROFILE_DIR="$test_profile" \
  crystalhd-chromium
rm -r -- "$test_profile"
```

Do not use the sandbox-disabled mode for ordinary or untrusted browsing.

| Launcher variable | Meaning |
| --- | --- |
| `CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` | Enables the experimental CrystalHD VA path; otherwise accelerated video decode is disabled |
| `CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1` | Required acknowledgement for hardware mode; causes `--disable-gpu-sandbox` |
| `CRYSTALHD_DRM_DEVICE=PATH` | Selects the display render node; default is `/dev/dri/renderD128`, which may be wrong on multi-GPU systems |
| `CRYSTALHD_CHROMIUM_PROFILE_DIR=DIR` | Uses a separate browser profile |
| `CRYSTALHD_CHROMIUM_NATIVE_GL=1` | Stops the launcher from forcing software GL |
| `CRYSTALHD_CHROMIUM=PATH` | Selects a non-Snap Chrome/Chromium executable |
| `CRYSTALHD_CHROMIUM_H264_POLICY=1` | Suppresses the Google Chrome policy warning only; it does not install or enforce a codec policy |
| `CRYSTALHD_CHROMIUM_CONFIG=PATH` | Selects a trusted per-user shell configuration file; the file is sourced as shell code |

From the repository root, remove only the separately installed browser assets
with:

```sh
sudo make uninstall-browser
```

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
Simple and VC-1/WMV3 in seekable containers with distinct picture timestamps.
It normally requires a reliable declared frame count. Open-GOP seeking requires
the client to supply earlier reference pictures: a demuxer seek to a later I
picture can skip leading B pictures even in software decoding. The probe does
not certify arbitrary files' seek indices or preroll.

Install the `libavcodec`, `libavformat`, `libavutil` and `libva` development
packages, then build the optional probe:

```sh
make -C filters/vaapi seek-test
LIBVA_DRIVER_NAME=crystalhd LIBVA_DRIVERS_PATH="$PWD/filters/vaapi" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --kill-after=10 120s filters/vaapi/vaapi-seek-test \
  /tmp/crystalhd-samples/high.mp4
```

Some ASF/WMV containers omit `nb_frames`. Only when the fixture's exact decoded
boundary was established independently, supply it explicitly, for example
`--expected-frames 480`. The reference pass must still reach EOF with exactly
that many pictures. A conflicting declared count is rejected; do not estimate
this value from duration and frame rate or use it to excuse a short drain.

The frozen 60-picture Full HD VC-1 Advanced elementary fixture needs a
timestamped index before this probe can seek it. Build a new external MOV and
verify its unchanged compressed bytes, packet boundaries, declared stream and
frozen full-resolution PTS/software-pixel reference without opening CrystalHD:

```sh
python3 tests/vc1-seek-oracle.py build \
  /absolute/path/vc1-fhd-scene60.vc1 /absolute/new/path/vc1-fhd60.mov \
  --seek-probe "$PWD/filters/vaapi/vaapi-seek-test"
```

The operation is stream-copy only and refuses a different source hash or an
existing output. `--seek-probe` runs only the software synchronous and
eight-frame-lookahead checks; a later hardware invocation remains separate.

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
