# CrystalHD VA-API driver

This backend exposes H.264 decoding through the standard VA-API VLD interface,
with bounded MPEG-2 Simple/Main, progressive MPEG-4 Part 2 Simple/Advanced Simple,
standard WMV3 Simple/Main and VC-1 Advanced support on BCM70015. BCM70012
retains its H.264 path but is not recently hardware-validated; the other
codecs are rejected on that device.
The backend accepts VA-allocated NV12 surfaces and imported
linear or GBM-mappable DRM PRIME NV12 surfaces for FFmpeg-style clients. It
also allocates packed BGRA output and exports DRM ARGB8888 surfaces for clients
whose compositor cannot render NV12 directly. New VA clients should select
`VA_FOURCC_BGRA`, which describes the actual B/G/R/A byte order. The historical
`VA_FOURCC_ARGB` storage alias and default selection remain for compatibility;
that alias is not a claim of standard A/R/G/B byte order.

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
H.264 MBAFF and selected MPEG-2 frame-coded interlaced cases described below:

- H.264 Constrained Baseline, Main and High, with 8-bit 4:2:0 input.
  Other chroma formats, separate colour planes and higher bit depths are
  rejected rather than reconstructed as 8-bit 4:2:0. Reconstruction preserves each
  picture's effective 4x4 and active 8x8 scaling lists, including JVT and
  custom matrices. Omitted IQ buffers use flat-16 defaults for that picture;
  malformed buffers and zero active coefficients are rejected. Selected MBAFF
  field pairs emitted from one submitted VA frame/access unit are materialized
  as a complete interlaced NV12 Weave frame; separately submitted PAFF fields
  and pulldown are not supported. CPU Bob is separate post-processing below.
- MPEG-2 Simple I/P pictures through 720x576, and Main I/P/B pictures through
  1920x1088 coded size. Input uses 4:2:0 frame pictures and whole slices.
  Progressive pictures require `frame_pred_frame_dct=1`; ordinary input has
  clear TFF/RFF flags, with the bounded Main-profile exception below. Caller picture
  parameters, inherited quantization matrices and start-coded slice bytes are
  preserved. The bounded **1.2 path** also accepts Main frame-coded TFF/BFF
  interlace with either value of `frame_pred_frame_dct`, preserving macroblock
  motion/DCT syntax. Complete field pairs are assembled into NV12 Weave for
  ordinary CPU Bob/VPP. The first accepted picture fixes the
  reconstructed sequence mode; VA does not supply the original sequence flag.
  A sequence starting with an interlaced frame may later contain progressive
  frame-predicted pictures without RFF when the two macroblock-row grids agree
  (`ceil(height/16) == 2*ceil(height/32)`), preserving that fixed interlaced
  sequence. Selected 720x480 mixed TFF/BFF downloads pass. A sequence starting
  progressive cannot later switch to interlaced; ambiguous row grids (including
  360/720-line mixed input), separately submitted field pictures, mixed RFF and
  Simple-profile interlace remain unsupported.
  Interlaced height must be
  divisible by four. Visible dimensions and sequence mode cannot change within
  a context. [Mixed-frame evidence](../../HARDWARE-2026-09-13.md#mpeg-2-bounded-mixed-frame-pictures--2026-10-24)
  is separate from the earlier uniform cases. Stock FFmpeg passes the bounded
  [SD and selected FHD TFF/BFF Weave/Bob checks](../../HARDWARE-2026-09-13.md#mpeg-2-main-frame-coded-interlace--2026-10-21),
  including clean real-capture intervals at 1920×1088i25 and 1920×1080i29.97.
  These establish exact output, EOS and fresh-process reopen, not real-time
  FHD playback, broad interlace conformance or physical A/V qualification.
  **Progressive Main TFF/RFF:** the internal headers may normalize display flags
  when progressive and interlaced macroblock-row counts agree
  (`ceil(height/16) == 2*ceil(height/32)`). This excludes flagged 360/720-line
  input and Simple-profile repeats. One stored picture is returned per input;
  the client retains its original repetition, PTS and duration rather than the
  backend guessing the omitted sequence flag. The selected
  [720×480 soft-RFF and whole-frame-repeat checks](../../HARDWARE-2026-09-13.md#mpeg-2-progressive-repeat-flags--2026-10-21)
  pass exact pixels, original FFmpeg metadata, EOS and reopen. No extra option
  is needed. Eligible Main frame-picture synchronization uses separately owned
  non-reference B pictures to advance output without repeated EOS/reopen;
  original pictures and client timing remain unchanged. Interlaced sequences
  need two lookahead pictures even when their current B picture is progressive.
  Complete output and compact field pairs have separate ownership records;
  neither a first field nor failed lease release retires a pair. See the
  [interlaced/mixed completion results](../../HARDWARE-2026-09-13.md#mpeg-2-interlaced-synchronous-completion--2026-10-24).
  The earlier
  [measured completion improvement](../../HARDWARE-2026-09-13.md#mpeg-2-synchronous-completion--2026-10-21)
  includes canonical and repeat-flag inputs. Newest-anchor-only demands and
  post-replay channels retain the existing EOS fallback. This does not qualify
  general telecine presentation, real-time FHD or physical A/V.
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
published 1.1.0 advertises no `VAProcFilterDeinterlacing` capability. See the
[runnable command and performance boundary](../../USAGE.md#interlaced-h264-weave-and-ffmpeg-bwdif).
Published RC.3 passed stock Kodi 21.3 progressive hardware-video and lifecycle checks;
quantified physical A/V remains unqualified. Earlier RC.2 payload `f4d99d5` failed
interlaced BWDIF/Render Bob playback. The `ec77b71` finite MBAFF input/tail fix
is included in published RC.3, with complete decoder-oracle results, but Kodi
source picture accounting and timing/output warnings still need qualification. See
[Kodi qualification and remaining limits](../../USAGE.md#kodi). Published 1.1.0
does not advertise VA VPP deinterlacing; no general Kodi interlace or
hardware-Bob support is claimed. The RC4 candidate reduces repeated VPP setup
and private-allocation overhead without disabling scaling/CSC. The RC4 stock
Kodi 22 report confirms bounded Weave playback, not Bob or physical A/V
qualification; Auto still retries unsupported DI on that release. See the
[known issues and planned v1.2 integration work](../../USAGE.md#known-issues-and-deinterlacing-roadmap).

<a id="development-cpu-va-vpp-bob"></a>

### v1.2: CPU VA-VPP Bob

The 1.2 implementation provides `VAProcFilterDeinterlacing` / Bob on the
CPU, after decode. It duplicates the selected field's rows independently in
Y and UV; it is not hardware Bob, BWDIF or motion-adaptive filtering. Existing
zero-filter Weave, scaling and CSC remain available.

In published 1.2, Bob requires separate source/target backing, both interleaved NV12
input fields, origin-zero processing regions, even region widths and region
heights divisible by four. NV12 or ARGB output may retain neutral coded padding
outside that region. Nonzero-origin cropping, single-field input (`ONE_FIELD`), FMD/SCD, temporal references,
additional outputs, rotation, mirroring and blending are rejected. Each request
produces one progressive output. `BOTTOM_FIELD` selects physical parity;
`BOTTOM_FIELD_FIRST` describes temporal order, not that selection. Clients own
field-rate scheduling and output timestamps, normally submitting both fields.
Filter settings are captured at Render and retained through queued work.
For BCM70015 H.264, a validated first temporal field can satisfy its Bob request through
a private view before the full decoded frame is ready. Opposite-field Bob,
Weave and ordinary surface synchronization still require the genuine pair.

Device-free pixel, API, ownership/cancellation and i686/no-SSE checks do not
establish real-time FHD, stock Kodi/Atom compatibility or physical A/V sync.
Those remain tracked in [#445](https://github.com/ahnhy1324/crystalhd/issues/445)
and [#436](https://github.com/ahnhy1324/crystalhd/issues/436). Source-only
development builds do not modify published packages or installed drivers.

From an already built 1.2 source root with a compatible module running, a
headless FFmpeg check for the selected H.264 MBAFF path is:

```sh
drm_node=/dev/dri/renderDXXX
LIBVA_DRIVER_NAME=crystalhd LIBVA_DRIVERS_PATH="$PWD/filters/vaapi" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
ffmpeg -hide_banner -hwaccel vaapi -hwaccel_device "$drm_node" \
  -hwaccel_output_format vaapi -i "/path/to/mbaff-video.ts" \
  -vf 'deinterlace_vaapi=mode=bob:rate=field,hwdownload,format=nv12' \
  -fps_mode passthrough -an -sn -f null -
```

This produces no display or sound. The
[bounded native comparison](../../HARDWARE-2026-09-13.md#12-development-cpu-va-bob--2026-10-19)
verified 50 input pictures→100 field outputs against an independent same-run
Weave-derived reference for both TFF and BFF, plus frame-rate Bob and Weave
reopen. A separate startup-inclusive headless FHD batch reaches 49.759 Bob
outputs/s: this does not demonstrate real-time 50p headroom, displayed cadence
or physical A/V. Exact source identities, CPU/RSS and method are in that report;
the short pixel controls are not themselves throughput or Kodi acceptance.

Field-rate Bob with FFmpeg 8.0.1 `-stream_loop` is not lossless: hardware-context
reconfiguration can discard one buffered picture at each boundary. Continuous
concat-demuxer input passes a separate bounded control; this does not qualify
that looping mode. See [the compatibility result in #445](https://github.com/ahnhy1324/crystalhd/issues/445#issuecomment-6257714172).

For supported interlaced input with this VA driver selected, Kodi's
per-video deinterlace method **Auto** requests CPU Bob; **Off** retains Weave.
The [installed-RC2 matrix](../../HARDWARE-2026-09-13.md#120-rc2-qualification--2026-10-22)
passes selected progressive and 720i25 Auto/Off lifecycle and qualitative A/V
checks. Its strict Auto count remains FAIL: Kodi requested 1,469/1,500 fields
at 60 Hz and 1,475/1,500 at 50 Hz because adaptive `DROP_ANY`/`SKIPDEINT`
omits some second-field VPP calls. This does not qualify uniform displayed 50p
or general FHD Auto playback.

The following source-pinned development results retain their original scope;
RC1's separate installed Kodi result remains HOLD. The selected 30-second FHD
development control has Auto commit
1,500 Bob outputs for 750 decoded pictures; Off uses no Bob. Both reach natural
EOF and return resources cleanly; see the source-pinned hardware report above.
In those source-pinned builds, previously exported targets awaiting decode
complete synchronously within the existing bounded wait when safe fence setup
is unavailable, rather than publishing an unfenced asynchronous write.
In unreleased builds, strictly driver-owned NV12 Bob targets exported only
read-only can remain VA-pending; external readers must call `vaSyncSurface`
before reading. Imported aliases and writable exports retain the existing
fence or synchronous-completion path. No root/debugfs permission change
or new option is needed. CPU-access/cleanup and partial-import failures still
reject. A passive Kodi 21.3 observation of all 1,500 successful post-processing
outputs confirms equal PTS within each field pair, with approximately 40 ms
between source pictures, not uniformly timed 50p. Its second-field adjustment
uses a previous temporal reference, while Bob correctly requires none; all 750
second-field adjustments were skipped. The pinned stock Kodi 21.3 source confirms
this client-side timestamp dependency. VA VPP has no output-PTS return field;
advertising false temporal references would not be a valid driver fix. These are
output metadata, not measured screen presentation times. The existing FFmpeg
VA field-rate Bob control independently passes 100 ordered outputs at 20 ms PTS
intervals; this does not establish Kodi presentation cadence.

The earlier 10.000-second seek landed at 0.021 seconds, before the fixture's next
keyframe at 10.021 seconds, and selected Kodi's 5-second cache startup fallback.
A bounded `2c79f1e` control seeking to 10.100 seconds lands at 10.021 seconds and
receives video-start acknowledgement 1.474 seconds after the demuxer seek log,
without that fallback.
The source versions differ, so this is not a controlled latency improvement or
complete attribution of the old delay. Seek/Stop buffer warnings remain; a prior
branch trace attributed its two warnings to cancellation, not deadline expiry.
Neither the new warnings nor physical presentation are classified by that old
trace. See
[#445's compatibility evidence](https://github.com/ahnhy1324/crystalhd/issues/445#issuecomment-6257714172).
Selected 720i25 dialogue now has qualitative maintainer A/V/motion confirmation,
including pause/resume and both seeks; calibrated timing and actual displayed
cadence remain unmeasured. The [native Atom report](https://github.com/ahnhy1324/crystalhd/pull/467#issuecomment-6272584392)
passes tests and TFF/BFF pixel controls, but Kodi Bob timing and LIVE-EOS fail.
For finite Bob playback, leave `CRYSTALHD_VAAPI_LIVE_H264` and
`CRYSTALHD_VAAPI_LOW_LATENCY_H264` unset. The explicit LIVE+LL path also has a
reproduced pre-EOF stall with `d1b3788`. With `9a3b08b`, the selected stock Kodi
control admits all 750 pictures without that midstream timeout, but final-field
drain still exceeds Kodi's EOF wait; close-time completion is not a playback PASS.
`2c79f1e` implements a private non-reference intra picture for eligible BCM70015
MBAFF LIVE Bob completion. Its exact-source 50-picture control completes both
mid-input-gap and EOF Bob/source synchronization before close; subsequent full
NV12/PTS comparisons match all 50 originals. The auxiliary is never presented,
and an input gap is not treated as EOS. The earlier private precursor passed a
bounded stock Kodi LIVE+LL control: all 750 originals and 1,500 Bob outputs
completed before close-time EOS without decoder restart or application changes.
That precursor run is not a stock-Kodi qualification of the final commit.
The separate PTS/cadence and physical A/V limits, prior failures and unrelated
Kodi environment errors remain recorded in [#445](https://github.com/ahnhy1324/crystalhd/issues/445).
General FHD/Atom Bob playback is not qualified. **Off** retains Weave; the separate
[FFmpeg BWDIF path](../../USAGE.md#interlaced-h264-weave-and-ffmpeg-bwdif)
remains available.
An automatic LIVE drain/replay checkpoint prototype is not enabled: a short
continuation/pixel control passed, but stock Kodi's 243-picture reconstruction
took 5.66 s and exceeded its 2 s output wait. It did not qualify LIVE playback.

## Replay and end of stream

BCM70015 firmware can retain output until later compressed pictures or a real
end-of-sequence marker arrive. For strict H.264 and the other codecs, the
backend may seal the exact submitted batch after 100 ms without real
input/output progress during synchronization, subject to the H.264 grace below.
New input waits until all real output timestamps retire and drain completes
through the firmware EOS marker
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

The published RC4 candidate includes the following admission/recovery guards; the
immutable `v1.1.0-rc.3` predates them.
Its VA backend submits at most **2 MiB per assembled H.264 access unit**,
including its reconstructed headers. This is a conservative submission policy,
not the physical chip maximum: [#442](https://github.com/ahnhy1324/crystalhd/issues/442)
reported session-wide output loss at 3,112,960 bytes, while primary BCM70015
pixel readback qualified 2 MiB and all pictures through the following IDR.
Larger units are rejected before replay or picture identity is committed;
earlier pending pictures remain valid. If a rejected picture is a reference,
dependent input is rejected until an actual IDR commits, preserving the
remaining reference chain. Rejecting a non-reference B picture does not require
an IDR or block otherwise valid continuation.

The [RC5 changes](https://github.com/ahnhy1324/crystalhd/pull/459)
retain that conservative baseline, but admit up to **3,014,656 assembled bytes**
for public-version-identified BCM70015/DIL 3.22.0 progressive frame-coded input.
Unknown/BCM70012/interlaced paths retain 2 MiB. Input bytes are not rewritten;
this qualified headroom and failed-public-read protection do not establish
generic out-of-policy recovery or the chip's physical maximum. Previously
accepted private pictures remain independent of a rejected public transaction.

A [new RC5 reporter result](https://github.com/ahnhy1324/crystalhd/issues/468)
finds some real B-pyramid input failing below that admitted byte limit, although
the synthetic boundary fixture passes. The limit is not a guarantee for every
stream. Installed-v1.1.0 observations identify an input-retention limitation:
the failing single NAL exceeds the measured 2 MiB compressed-input ring, and
its verified header disappears before the following AU supplies its delimiter.
The [hardware report](../../HARDWARE-2026-09-13.md#h264-input-retention-investigation)
separates this observation from the untraced internal reclamation path.
The 1.2 implementation adds a second admission check only for units
above the ordinary 2 MiB assembled baseline: at most **2,093,056 estimated
de-escaped bytes** across the assembled Annex B unit, including start codes and
NAL headers. The existing 3,014,656-byte assembled cap remains. Only unambiguous,
nonterminal emulation-prevention bytes reduce that estimate; no input bytes are
removed or rewritten. Over-budget transactions are rejected before input,
replay or accepted-picture identity is committed, using the existing reference
picture/next-IDR recovery rules.

This is bounded containment for the documented
[#468 compatibility limitation](https://github.com/ahnhy1324/crystalhd/issues/468),
not full decoding repair. The 2 MiB minus 4 KiB
cutoff is conservative policy, not a measured firmware reserve or proven chip
limit. Ordinary units at or below 2 MiB assembled bytes are unchanged, so this
does not establish protection for every near-2-MiB de-escaped input. Selected
native controls verify rejection before the offending AU's DMA and fresh
reopen. The formerly working 2,958,968-byte
IDR250 control is intentionally rejected. Exact firmware reclamation and
broader recovery remain research; no new option, timeout, firmware or legacy
ioctl change is introduced. This describes the measured signed-firmware and
configuration behavior, not a universal BCM70015 silicon limit.

For uninterrupted playback of affected input, select software decoding.
Published v1.1.0 remains affected; the 1.2 implementation rejects selected over-budget
input rather than decoding it. After a hardware error, close that decode context
and reopen supported input. A separate deliberate-client-discard test reaches
all 192 pictures from the next actual IDR on one context; ordinary players are
not thereby given automatic recovery. Longer timeouts or disabling prefeed
did not resolve the retained failure.

RC5 applies this failed-`vaEndPicture()` public-read protection
to every supported decode codec. New synchronization, image/export and VPP
source reads report the rejected transaction instead of an earlier completed
frame. Previously captured private pictures remain readable; a valid new
picture transaction can reuse the surface. This is not stream-error recovery
or a change to the published RC4. RC5 additionally preserves independent
pending H.264 decoding when an obsolete completed surface is retired; that
retirement must not reset unrelated accepted pictures.

The independent software assembly/replay allocation bound remains 8 MiB per
H.264 unit; it is not an 8 MiB hardware-decode claim. MPEG-2, MPEG-4 and VC-1
retain 512 KiB per unit, with 32 MiB or 512 access units total and 8192 replayed
units before pruning an obsolete reference prefix. Admitted H.264 units above
512 KiB normally use `DtsProcInputFragment`, with at most 512 KiB per call;
applications must not split them. RC5 additionally allows the
identified older Flea PES library to send a complete AU up to **768 KiB** in
one fully reserved legacy call. Unidentified older libraries retain 512 KiB;
larger units require the fragment API and are otherwise rejected before replay
commit or partial input, never continued through a legacy tail call. Missing
IDR/I-root history or an exceeded limit is an error. Completed, non-outstanding
ordinary B pictures may be omitted from MPEG-2 and VC-1 replay; stateful BI and
reference history required by an open GOP remain. Synchronous
one-picture-at-a-time clients can be slow because they may trigger frequent
reopen and replay. BCM70012 does not use this sealed-batch path.

With `CRYSTALHD_VAAPI_LIVE_H264` unset, H.264 uses bounded transport history:
completed history may retire within the limits above, while queued input and
outstanding pictures are never discarded. On BCM70015, EOS is deferred to one
second when that prefix is no longer replayable or while a fully submitted
H.264 AU larger than 512 KiB remains outstanding. Queued/partial AUs and
completed cached history alone do not extend the grace. The large-AU exception is independent
of the LIVE setting; it neither changes the live-gap policy below nor delays
already-ready output. Only time actually spent waiting without
progress inside VA sync calls is accumulated. Time between calls,
including a player pause, is not counted. If this bounded grace eventually
seals an unreplayable stream, continuation requires an actual IDR; dependent
input is rejected rather than decoded with missing references.

For experimental long-running H.264 input,
`CRYSTALHD_VAAPI_LIVE_H264=1` also enables the direct decode-output path and,
after a second original picture is accepted, does not infer EOS from an
input-delivery gap. Final pictures generally require subsequent input or explicit
context drain; the absolute ten-second synchronization deadline still applies.
For BCM70015 8-bit 4:2:0 MBAFF frame pictures with POC type 0, an opposite-field
Bob request stalled for 100 ms may instead send a small non-reference intra
picture and the next AUD/filler prefix. Its syntax comes from the last fully
submitted original picture. The original complementary field must still arrive
before Bob completes; the auxiliary output owns no public surface or PTS and is
discarded only after validation and hardware-buffer release. This advances the
selected retained tail without sealing live input or changing reference pictures.
Only one auxiliary can be outstanding, and explicit context close still drains
its firmware EOS obligation. Other syntax and non-Bob requests retain the existing
wait/drain policy; this is not a general EOF detector or a new user option.
In unreleased development builds, eligible ordinary H.264 Bob requests also use
this lookahead with LIVE unset and automatic or explicit prefeed enabled.
This avoids repeatedly sealing and replaying the stream to complete a field.
Explicit `CRYSTALHD_VAAPI_LIVE_H264=0`, or disabled prefeed with LIVE unset,
retains the ordinary finite fallback. Explicit LIVE keeps its existing behavior.
Finite replay after auxiliary use requires the actual EOS marker and retired
output/prefix debt, including during close.
For finite files with stock FFmpeg, leave this variable unset:
`vaSyncSurface()` waits for one surface and is not an EOF or drain signal. In
explicit live mode outside that Bob path, the last reordered pictures can remain
held until subsequent input, or until a client retains the output surfaces and calls
`vaDestroyContext()` to perform the explicit drain before downloading them.
Setting the variable to `0`, or to any value other than exactly `1`, selects
strict replay.
An initial one-picture decoder probe remains eligible for inactivity-based
EOS. These policies do not affect other codecs, BCM70012's existing path or
the library ABI, and the explicit live mode is not a general real-time
streaming support claim.

Finite-policy BCM70015 progressive and MBAFF frame pictures automatically send
the next timestamped AUD/filler prefix, avoiding a wait for the next complete
input AU. This does not predict slice data, complete a missing field or remove
the ordinary EOS/drain fallback: a one-picture probe or reordered tail can still
require EOS. Separately coded fields/PAFF, other codecs, BCM70012 and explicit
live mode do not gain automatic prefeed. Set
`CRYSTALHD_VAAPI_LOW_LATENCY_H264=0` to disable it.
Automatic progressive prefeed and the large-AU grace above are RC5 changes;
earlier RC4 only automatically prefed the eligible finite MBAFF subset.
They do not implement general out-of-policy recovery, physical A/V timing or
deinterlacing. Exact RC5 package qualification is tracked in
[#430](https://github.com/ahnhy1324/crystalhd/issues/430); earlier development
measurements retain their original source identities.

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

In the unreleased 1.3 backend, `vaSyncSurface2(..., 0)` is a passive query:
mutex contention returns `VA_STATUS_ERROR_TIMEDOUT` without inspecting or
changing surface state. Even an invalid surface ID can therefore time out
until admitted. Decoder-context destruction also waits for the worker's
borrowed output-poll descriptors to retire. `VA_STATUS_ERROR_HW_BUSY` retains
ownership and permits a subsequent, non-overlapping destruction retry.

## Surface and export contracts

Decode targets may be VA-allocated NV12 surfaces or imported linear or
GBM-mappable DRM PRIME NV12 surfaces. CrystalHD YUY2 output is converted into
immutable, timestamped NV12 storage for the requested picture.

`CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC=1` is an optional reference-host
optimization for CPU writes into VA-allocated i915 dumb surfaces. It uses the
i915 WC/GTT ownership transitions for synchronous decoded-pixel writes,
including finite decode's private-to-public NV12 copy. Eligibility requires
MMAP_GTT interface version 4 or newer and an explicit WC offset matching the
existing mapping; an unconfirmed mapping keeps normal DMA-BUF synchronization.
Imported surfaces, generic copies, VPP and other DRM drivers retain that normal
path. The default is off. Bounded stock-Kodi tests reduced the NV12 copy-stage
elapsed time, but this is not a claim of lower physical display latency or
general playback performance. Earlier readback measurements likewise do not
establish a display-latency improvement.

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

Images derived from cold, unowned upload surfaces commit CPU writes on final
unmap, or exported-image writes on handle release. The source is reserved
while writable access is active; stale sources and failed commits report errors.
Images derived from retained decode pictures remain independent readback
snapshots and cannot overwrite decoder references.

NV12 PRIME2 exports default to separate R8 and GR88 layers. Request
`VA_EXPORT_SURFACE_COMPOSED_LAYERS` for one two-plane NV12 layer. Read/default
exports and derived snapshots wait for picture completion; cancellation or an
identity change reports an error. Write-only exports remain nonblocking for
target allocation, but are busy during an unfenced internal VPP write.

Aliases imported from a known DMA-BUF share their decode or display owner so
completion remains synchronized. Every view of the same backing object must
use identical format, dimensions and planes. Decode targets must be canonical
surfaces, not imported aliases. A surviving alias of a destroyed owner is
invalid, and reimport remains busy until pending writes finish.

The minimal VPP path provides unfiltered NV12 scaling and NV12-to-ARGB
conversion in a separate video-processing context. Decode-integrated processing
is a separate, opt-in H.264 path described below; omission or
`VA_DEC_PROCESSING_NONE` retains ordinary decode. Separate VPP/Bob does not
implicitly enable the card scaler.

Unreleased 1.3 VPP accepts standard `VA_FILTER_SCALING_FAST` as well as
DEFAULT; both select the existing nearest-neighbor kernel. For a built
1.3 driver selected through `LIBVA_DRIVERS_PATH`, stock FFmpeg can use
`scale_vaapi=w=1280:h=720:mode=fast` with NV12 output. This is host VPP,
not card scaling. HQ, bilinear and advanced interpolation remain unsupported.
When NV12 Bob resizing repeats a physical field row, its horizontal resample
is now reused from private CPU memory, without reading the output surface or
changing field selection, pixels or ownership. See the
[bounded cost and negotiation results](../../HARDWARE-2026-09-13.md#output-scaling-cost-2026-10-25);
these do not establish streaming or displayed-frame rates.

Ordinary NV12 VPP also avoids clearing pixels that its ROI will overwrite,
copies matching-width rows and reuses sampled rows independently for each
interleaved field. Odd source-crop chroma phase and destination borders/pitch
padding retain their previous values. The [native comparison](../../HARDWARE-2026-09-13.md#ordinary-nv12-vpp-2026-10-25)
preserves all selected pixel/PTS references; it is a finite headless result,
not new real-time or display acceptance.

On the 1.2 implementation, VPP/Bob honors declared full-range NV12 for
ARGB conversion; UNKNOWN input retains the existing limited-range interpretation.
NV12 output preserves the input range, with range-correct black padding.
Explicit NV12 range changes and reduced-range ARGB output are rejected.
Unreleased 1.3 ordinary VPP additionally accepts BT.601 or BT.709 input with
the same output standard, or `None` output to inherit it. NV12→ARGB uses the
declared matrix; NV12→NV12 preserves samples. `None` input retains the existing
BT.601 interpretation. Changing standards, explicit color properties,
primaries/transfer conversion and selectable chroma-site conversion remain
unsupported; this is not general color management. Decode-integrated scaling
retains its existing `None`-only color-standard contract.
RGB output requires a VPP config permitting `VA_RT_FORMAT_RGB32` and a compatible
pixel format. The default config permits RGB. Unreleased 1.3 also exposes
`VA_FOURCC_BGRA` output plus packed BGRA images for `vaGetImage`, `vaPutImage`
and `vaDeriveImage`, retaining the usual synchronization and write leases.
Image transfers copy matching NV12 or BGRA formats; use VPP for conversion.
Reduced-range RGB remains unsupported: for example, stock FFmpeg VPP needs
`scale_vaapi=format=bgra:mode=default:out_range=full` before
`hwdownload,format=bgra`. The legacy ARGB alias's image restrictions remain.

Unreleased 1.3 Bob also accepts an NV12 destination rectangle at even X and
Y divisible by four, keeping the source origin at zero. For example, a
1280×720 source can fill `{320, 180, 1280, 720}` on a 1920×1080 target without
enlargement; the remaining visible canvas is range-correct black. This does
not implicitly enable card scaling or alter stock Kodi's full-frame requests.
Nonzero source crops and nonzero ARGB Bob destinations remain unsupported;
the full centered-playback experiment remains [#477](https://github.com/ahnhy1324/crystalhd/issues/477).

Pipeline capability format lists are borrowed, read-only driver
metadata, valid until the VA display terminates. Read the returned pointers
and counts; do not free them or assume supplied scratch arrays were filled.
A worker can wait for reordered hardware output while later input
is submitted. VPP sources remain available until the decode surface is reused
or destroyed; a target with an outstanding writer is busy. Missing pictures and
canceled or reset epochs report errors rather than substitute other pixels.
On the 1.2 implementation, exact completed sources may remain readable after
successful decoder-context close; this does not revive canceled work or
rejected, reused or destroyed bindings. Clients must check VA surface status
and must not treat fence signaling alone as proof
of valid pixels. Never-exported internal targets use VA synchronization and
ordinary CPU cache synchronization, without requiring a `sw_sync` timeline.
Unreleased builds also allow strictly driver-owned, read-only-exported NV12
Bob targets to remain VA-pending until `vaSyncSurface` completes. Imported or
aliased targets and writable exports retain their existing fence or synchronous
path; ARGB is rendered privately before the final copy. A canceled or failed
fenced write closes its
unsignaled timeline so its
fences fail with `-ENOENT`; successful writes flush pixel caches before the
timeline advances.

### Development experiment: centered card output

The default-off `CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12` mode lets an
ordinary BCM70015 H.264 VLD client retain a readable coded-size NV12 canvas
while the card returns a smaller picture. Compatible full-canvas NV12 Bob
requests process that small picture and place it at 1:1 size inside black
borders. This reduces visible picture size, **not** the negotiated surface
size; it does not enlarge 720 back to FHD or provide hardware deinterlacing.
It is a development experiment, absent from stable v1.2.0.

With the development driver selected, an example for a known 1920×1080
visible source, including a coded 1920×1088 source, is:

```sh
CRYSTALHD_VAAPI_EXPERIMENTAL_CENTERED_NV12=1280x720@1920x1080:limited \
  LIBVA_DRIVER_NAME=crystalhd kodi /absolute/path/to/1080i25.mkv
```

For display enlargement, Kodi's Video settings → Zoom can apply `1.5` to this
720-in-1080 picture on the GPU. Source-encoded black bars remain; return Zoom
to `1.0` for the native centered size. Selected playback results are in [#477](https://github.com/ahnhy1324/crystalhd/issues/477).

On the reference T8100/BCM70015 host, a new L0s-ON progressive 30p comparison
selects all 9,001 pictures over five minutes with centered 720, while native FHD
selects 8,506. Centered Bob at 50 outputs/s also completes the selected five-minute BFF case;
the TFF case skips 3/15,000 outputs. The selected no-B 60p OFF case skips
3/18,000 over five minutes; B-reference and ON controls retain further misses.
These are source/VA-canvas results on a 60-Hz 1024×768 panel,
not native FHD detail or a power measurement. See the
[matched measurements and remaining cadence limits](../../HARDWARE-2026-09-13.md#stock-kodi-centered-output-with-l0s-2026-10-24).
Keep L0s policy changes separate from normal playback; the existing default
workaround remains unchanged. The selected centered Bob/zoom 1.5 dialogue also
has qualitative maintainer A/V and control confirmation in this new ON condition.

The value specifies `capture-width×height@visible-width×height:limited|full`
using ASCII `x`. Crop and black range must be supplied explicitly; coded1088
does not imply visible1080. The current experiment requires the full coded
width and fewer than16 cropped bottom rows, aligned to the sequence's crop
unit. Capture width must be at least128, smaller than the source and divisible
by16; capture height must be divisible by4, with an exact aspect-preserving
reduction. Placement uses even X and rounds Y toward the top to a multiple
of four. Remove the variable or set it to `0` to
disable it. Keep ordinary LIVE/prefeed defaults when comparing playback;
explicit `CRYSTALHD_VAAPI_LOW_LATENCY_H264=0` disables automatic prefeed and
is not equivalent to leaving it unset. Explicit decode-processing/additional-output
selection, READY remapping and BCM70012 cannot be combined with this mode.

Original GetImage, DeriveImage and export still expose actual full-canvas
pixels. Requests incompatible with the small Bob path use that real canvas.
Full-size allocation, composition and presentation costs remain, so smaller
card transfers do not establish a playback speedup. Stock-client performance,
field delivery and presentation acceptance remain tracked in [#477](https://github.com/ahnhy1324/crystalhd/issues/477).

<a id="development-decode-integrated-card-scaling"></a>

### v1.2: decode-integrated card scaling

An H.264 VLD client can explicitly create its configuration with
`VAConfigAttribDecProcessing = VA_DEC_PROCESSING`. For each decoded picture,
submit a `VAProcPipelineParameterBufferType` in the **decode context**, alongside
the usual picture and slice buffers:

- `surface` is the original coded-size decode target; `additional_outputs`
  contains one different, smaller NV12 surface.
- `surface_region` explicitly supplies the origin-zero visible rectangle,
  using the full coded width. `output_region` is the entire smaller target.
  For example, coded 1920×1088, visible 1920×1080 and output 1280×720.
- Only an exact aspect-preserving downscale is accepted. Output width is even
  and at least 128; MBAFF additionally requires width divisible by 16 and height
  divisible by four. Visible crop and output geometry stay fixed per context.
- The card scales before readback. Original reconstruction stays in the
  firmware DPB: the original VA handle supports decode completion/reference
  identity, **not** image readback, export, upload or separate VPP input.
  Only the small additional output has host pixels and can feed CPU Bob/VPP.
- Supply internally allocated, previously unexposed surfaces. A processed pool
  can be reused after completion; `vaCreateImage`/`vaGetImage` takes a readback
  snapshot without making the target externally writable.

This BCM70015 path defaults to finite replay. Explicit
`CRYSTALHD_VAAPI_LIVE_H264=1` enables the existing live-gap policy;
`CRYSTALHD_VAAPI_LOW_LATENCY_H264=1` independently enables prefeed. Both
preserve exact-picture identity. Once streaming has started, explicit LIVE
retains references across input gaps; prefeed alone does not change finite
drain policy. READY remapping, arbitrary crop/height, extra decode-time filters and
BCM70012 are rejected. The default VLD path and published 1.1 are unchanged. Stock
FFmpeg/Kodi do not request integrated processing, so this does not automatically
accelerate their separate `scale_vaapi`/VPP calls.

At `87e0306`, the finite FHD TFF test completed two opens with 50 pictures each:
only 1280×720 picture geometry / 360-row fields were received, all 100 assembled NV12 pictures matched the
independently joined card-output reference, and EOS/reopen/cleanup passed.
Ordinary CPU VA Bob then matched both field parities of the first and last
small pictures across two opens (eight exact outputs). This qualifies the
explicit API connection, not stock-player selection, live cadence or physical
A/V. See the [measurement receipt](../../tests/fixtures/issue436/card-scaled-bob.json)
and [remaining scope](../../TODO.md#card-720-output-deinterlace-then-enlarge).

#### Returning the small output as progressive video or FHD

Use the **small additional output**, not the opaque original decode handle,
as the source of a separate `VAEntrypointVideoProc` context:

| Requested result | Host-side processing after card readback |
| --- | --- |
| 720p | CPU VA Bob into a 1280×720 target |
| 1080p | CPU VA Bob directly into a 1920×1080 target (one VPP operation) |
| Bob 720p, then 1080p | Bob into 1280×720, synchronize it, then zero-filter progressive VPP into 1920×1080 |
| 1080 interleaved Weave | Zero-filter field-preserving enlargement; this does **not** deinterlace |

For Bob, use a `VAProcFilterParameterBufferDeinterlacing` with
`algorithm = VAProcDeinterlacingBob`, select each field's parity, and submit its
buffer through the pipeline's `filters`/`num_filters`. Set `surface_region`
to `{0, 0, 1280, 720}` and `output_region` to the full selected target; declare
the source's TFF/BFF ordering and `output_surface_flag = VA_FRAME_PICTURE`.
Filter `flags = VA_DEINTERLACING_BOTTOM_FIELD` selects bottom; clear that bit
for top. Ordering flags alone do not select the requested field.
For the second stage after Bob, use no filters and FRAME→FRAME flags. Field-rate
output requires two Bob requests and caller-assigned field timestamps; VPP
does not schedule playback or generate the application's PTS.

For Weave enlargement, use matching `VA_TOP_FIELD_FIRST` or
`VA_BOTTOM_FIELD_FIRST` input/output flags. The scaler keeps the two Y and UV
field lattices separate. Source-region X/width and output width must be even;
source-region Y/height and destination height must be multiples of four;
NV12 destination X must be even and Y a multiple of four; ARGB destination Y
must be even. Prefer origin-zero
full-picture regions. Do not label woven pixels progressive to request
deinterlacing.

A completed internal NV12 VPP result can feed another VPP stage after
synchronization. The next Render captures its exact pixels and original identity
before reuse; this adds one private copy only for chained VPP. Rejected, pending
or failed timestamped output is not a valid replacement. Writable DMA-BUF export
or externally supplied writable backing cannot provide this immutable-result
contract; use internal intermediate targets. Read-only aliases of our backing
retain the canonical owner's rules.

All enlargement and Bob above run on the **CPU**, not the card or display GPU.
The card still transfers only the small picture, so returning FHD host pixels
does not undo the PCIe reduction; it does add CPU/memory work and cannot restore
detail removed by downscaling. Prefer **one-pass Bob→FHD** when no separate
720p intermediate is needed. At `e636182`, bounded native finite and LIVE+prefeed
tests each passed two opens, 100 exact small pictures and 16 exact FHD returns;
LIVE also passed 24 FHD Bob outputs across four input gaps before explicit close.
The scalar enlargement optimization at `9affe1f` reduces the isolated
720→FHD kernel from 30.00 to 7.41 ms on the T8100, with identical pixels and
no SSE requirement. In the follow-up finite card test, second-stage API+sync
samples are 11.78–16.37 ms; direct Bob→FHD samples are 6.35–23.70 ms.
These are not sustained fps or display-latency measurements: four LIVE
bottom-field completion waits still took 153–158 ms. Full scope is in the
[existing receipt](../../tests/fixtures/issue436/card-scaled-bob.json)
(`va_scaled_return_extension` and `host_resize_optimization`).
The separate
[GStreamer BWDIF recipe](../../USAGE.md#interlaced-h264-weave-and-ffmpeg-bwdif)
uses a different filter. Its throughput is not a VA Bob performance claim.
An additional [GStreamer scalerbob physical check](../../HARDWARE-2026-09-13.md#card720--bob--fhd-physical-playback--2026-10-21)
confirmed speech/lip sync and no persistent problem, but still dropped late
frames during clocked output. It does not qualify this VA path's presentation.

## Limitations

- VA-API supplies no explicit end-of-stream callback; bounded batch replay
  covers the tested BCM70015 files, not arbitrary streams or BCM70012
- selected Kodi 21.3 finite sessions can log an output-message timeout at EOF
  after the application's queue counters reach zero. A guarded trace identifies
  one empty-drain wait lasting two seconds; it is a finite-playback compatibility
  limit, separate from CrystalHD DMA retirement. Other timeouts are not thereby
  classified as harmless. Retain picture counts, EOS and resource checks; see
  the [observations](../../HARDWARE-2026-09-13.md#rx-pool-retirement-and-reopen-2026-10-24)
  and [#445](https://github.com/ahnhy1324/crystalhd/issues/445).
- only the decode, image, DRM PRIME, and minimal video-processing operations
  needed by the documented clients are implemented; `vaPutSurface`,
  subpictures, palettes, and detailed surface-error reporting are unavailable
- maximum coded size 1920x1088; MPEG-2 Simple is limited to 720x576
- NV12 images are limited to 1920x1088; odd dimensions retain complete UV pairs.
  Image copies reject busy surfaces and invalid rectangles. `vaPutImage` also
  rejects retained decode pictures and their aliases instead of invalidating
  decoder reference identity. For retained decode pictures, `vaDeriveImage`
  remains a readback snapshot, not a
  writable direct alias; use `vaCreateImage`/`vaPutImage` for ordinary uploads.
- one CrystalHD playback session at a time; another client receives hardware
  busy at decode-context creation if the device is already owned. Configuration
  queries and VPP-only contexts do not reserve playback. The first codec start
  reuses the admitted handle; later finite replay/reset still performs the
  existing full-device close/reopen and reports a failed reacquisition normally
- video processing supports NV12 scaling and NV12-to-ARGB conversion, plus the
  bounded CPU Bob path above; rotation is unsupported. Selected H.264
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

Trace schema `crystalhd-vaapi-trace-v4` separates admitted Sync/Export calls'
`admission_ns` (entry through acquiring the driver mutex) from `duration_ns`
(post-admission through the exit sample). Add them for that call's sampled
elapsed time. `call_entry_ns` uses the same steady monotonic clock as event
`ts_monotonic_ns`; enter records are emitted after admission and identify the
binding observed then. Zero-timeout calls rejected before admission and early
argument/surface rejections do not produce paired timing records. Timeout and
readiness behavior are unchanged.

For queued VPP, join `vpp_queued`, `vpp_start`, `vpp_run` and
`vpp_commit`/`vpp_cancel` by session and operation. Queue-to-start includes
scheduling; start-to-run includes waiting for the selected source; run-to-commit
includes conversion and retirement, not pure CPU time. Immediate ready-source
VPP has run/terminal records without a queue. Canceled work need not reach run.
These VPP records retain the captured source identity and name the destination
in surface/owner, including after reset or target reuse. Times from tracing are
diagnostic observations, not physical display or end-to-end latency.

### Bounded H.264 admission diagnostic

`make h264-au-admission-check` validates the fixture/readback checker without
hardware or FFmpeg. For a deliberate physical BCM70015 check, first create a
new offline fixture directory with FFmpeg/libx264:

```sh
python3 -B tests/h264-au-admission.py prepare NEW_DIRECTORY
```

The separate `native` command requires explicit source-built VA/library
directories and a supervising hardware owner with AC, temperature, scoped
kernel-log, installed-module/PM identity and device-ownership guards. It never
installs components or loads/reloads the module. Run only while the card is idle:

```sh
python3 -B tests/h264-au-admission.py native NEW_DIRECTORY \
  --driver-directory VA_DIRECTORY --library-directory LIBRARY_DIRECTORY
```

It calibrates the driver's assembled header size, checks exact 512 KiB −1/0/+1,
1 MiB, 1.5 MiB and 2 MiB units using 75 consumed luma hashes and the following
IDR, then checks reopen. It stops at the first mismatch/error and never probes
the known-failing larger units from #442. Routine seal messages with outstanding
timestamps are normal progress; an EOS-before-completion failure is not. A
native timeout reports the child PID without signaling it; the supervising
owner controls recovery. This checks a conservative policy, not a physical
maximum or a substitute for exact candidate/host identity records.

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
