# History

## 1.2.0 — in development

- Reject missing VA subpicture-format count outputs instead of dereferencing
  them; retain the empty format list and unsupported subpicture functionality.
- Honor full-range NV12 in VA VPP/Bob ARGB conversion and black padding;
  preserve default-range output and reject unsupported range conversions.
- Store first-field Bob's private NV12 view at half height, preserving public
  decode surfaces, field/chroma samples and pending-picture ownership.
- Keep vectorized ARGB conversion tails private and restore output borders
  after conversion, including unaligned widths and offset output rectangles.
- Reject unsupported decode-integrated VA processing requests; preserve
  ordinary decoding and separate VPP/Bob contexts.
- Wait directly for BCM70015 firmware-command completion instead of adding
  a fixed 50 ms delay; retain IRQ fencing, reply ownership and timeout recovery.
- Reject device opens when TX buffers or the worker cannot be created;
  allocate worker scratch before startup and release it only after joining.
- Stop context initialization on mutex setup failure without publishing an
  unusable handle; release temporary mutex attributes on every initialized path.
- Correct hardware-initialization status: HWINIT no longer skips firmware
  upload/INIT, failed setup remains retryable, and in-progress setup is not
  reported as complete. Preserve existing successful playback and teardown.
- Advance stalled LIVE H.264 MBAFF Bob output with a private non-reference
  intra picture, preserving original frames and references without inferring
  EOS from an input gap. Drain the private output separately at explicit close.
- Avoid allocating/copying duplicate H.264 replay-field pixels while retaining
  paired-field validation, output release and completed-picture ownership.
- Honor negotiated VA render formats in queries and output bindings, reject
  unsupported VPP/display requests, and release obsolete Render captures.
- Avoid full-device reopen during fully drained finite H.264 reference replay
  when the library explicitly supports fresh-channel reuse; retain legacy fallback.
- Retry unsent H.264 prefeed after TX backpressure without requiring another
  input picture; cancel obsolete requests without claiming timestamp ownership.
- Complete first-temporal-field Bob from a released, operation-private H.264
  field without marking the decoded frame ready or changing EOS/replay policy.
- Allow aligned BCM70015 H.264 field scaling through the existing GStreamer
  width option, preserving paired-field identity and rejecting scaled whole Weave.
- Send library diagnostics to stderr so binary media written to stdout is not
  corrupted; preserve existing messages, log levels and caller-selected files.
- Reduce resized/padded NV12 Bob CPU work without changing sampled pixels,
  chroma pairing, CPU-access ownership or the no-SSE path.
- Avoid a redundant DMA-BUF CPU-write acquisition when VPP fencing is
  unavailable; retain synchronous completion and failure cleanup guarantees.
- Count a validated, released H.264 first field as transport progress so an
  active field pair is not prematurely treated as an idle decode batch.
- Implement CPU VA-VPP Bob for origin-aligned woven NV12 regions, with NV12/ARGB
  output, independent Y/UV field sampling and Render-time filter snapshots.
  Preserve zero-filter Weave/scaling and legacy ABI; single-field input,
  nonzero-origin cropping and hardware Bob remain unsupported. Bounded TFF/BFF
  pixel and headless FHD throughput controls do not establish displayed cadence,
  Atom performance, endurance or physical A/V acceptance.
- Preserve completed decoder pictures for delayed Bob/VPP after successful
  context close, without reviving canceled work or stale/rejected source bindings.
- Keep VPP queue allocation failures transactional and prevent an older failed
  request from invalidating a newer writer's output.
- Complete exposed, decode-pending VPP synchronously when fencing is unavailable,
  retaining cancellation and CPU ownership checks rather than exposing an
  unfenced asynchronous write.
- Preserve imported PRIME plane layouts on export and correct legacy batch
  imports. Validate import spans and CSC strides before access, and initialize
  complete chroma pairs in odd-width NV12 backgrounds.
- Add an extended-H.264 de-escaped admission guard for
  [#468](https://github.com/ahnhy1324/crystalhd/issues/468), rejecting over-budget
  transactions before submission without rewriting input. Selected native
  controls qualify bounded rejection/fresh reopen, including intentionally
  rejecting some formerly working input. Separate installed-v1.1.0 observations
  establish a single-NAL input-retention limitation; exact firmware reclamation
  and general client recovery remain open.
- Recycle unobservable, superseded progressive H.264 CPU backing after a
  successful surface rebind. Preserve pending identities, captured pictures
  and eager allocation-failure behavior.

## 1.1.0

Published on 2026-10-19: bundle `1.1.0` / module-DKMS package `3.10.9`,
source `85f426d5545a53cc63dd512b355ab6f6c9668d61`.
[Final-package qualification](HARDWARE-2026-09-13.md#110-final-package-qualification--2026-10-19)
and [#430](https://github.com/ahnhy1324/crystalhd/issues/430#issuecomment-6256652744)
record fresh final checks and scoped RC5 evidence reuse. RC5 and earlier
releases remain immutable.

- Incorporate the reviewed i686/SSE2 and scalar-build compatibility fixes,
  bounded large-H.264 transport, failed-picture read/lifetime protection and
  finite-input EOS/reopen corrections.
- Include selected H.264 MBAFF Weave decoding and explicit hardware/software
  player selection, without advertising a hardware deinterlacer or general
  full-rate interlaced presentation.
- Improve VA processing interoperability and setup/copy efficiency, plus
  GStreamer MPEG-2 admission, timestamps and drain handling.
- Add bounded HTTP(S) player input and read-only configuration inspection,
  presets and redacted diagnostics. Configuration files do not apply settings;
  HTTPS/network playback retains its documented unverified scope.
- Preserve `libcrystalhd.so.3`, legacy ioctl and firmware identities. The final
  package revision is distinct from RC5; it is not a new decoding implementation.

## 1.1.0-rc.5 — candidate

Published on 2026-10-18: bundle `1.1.0-rc.5` / module-DKMS package `3.10.8`,
source `7f78d01e8103082bd08a62f459c8e20f77a1ec74`.
[Fresh qualification](HARDWARE-2026-09-13.md#110-rc5-qualification--2026-10-19)
continues under [#430](https://github.com/ahnhy1324/crystalhd/issues/430).
Earlier published RCs, stable 1.0.0 and the Phase 1 baseline remain unchanged.
Publication of this candidate is not stable v1.1.0 GO.

- Admit qualified larger progressive H.264 units for identified BCM70015/DIL
  transport combinations, retaining conservative fallback limits. Preserve
  accepted private pictures while rejecting reads of a failed public decode
  transaction across all supported codecs.
- Preserve independent pending H.264 decode when obsolete completed surfaces
  are retired; avoid unnecessary decoder resets.
- Extend existing automatic finite-input prefeed to eligible progressive
  H.264 frame pictures. Use the existing bounded one-second inactivity grace
  while a fully submitted large H.264 unit remains outstanding, avoiding
  premature EOS/replay without delaying already-ready output or treating a
  player pause as decoder waiting.
- Avoid redundant decoder-control wakeups when tearing down unused VPP
  contexts, without disabling scaling/CSC or advertising deinterlacing.
- Correct read-only configuration inspection: explicit prefeed is independent
  of LIVE; automatic eligibility is not inferred from the inspector's boolean.
  No new option, legacy ioctl or `libcrystalhd.so.3` ABI change is introduced.

## 1.1.0-rc.4 — candidate

Candidate bundle `1.1.0-rc.4` / module-DKMS package `3.10.7`; publication and
exact-payload qualification are tracked in
[#430](https://github.com/ahnhy1324/crystalhd/issues/430). Published RC.3 and the
Phase 1 baseline remain unchanged. The legacy ioctl and `libcrystalhd.so.3`
ABI are retained.

- Reject competing VA decoder contexts early. Bound assembled H.264 hardware
  submissions to 2 MiB and preserve pending output/next-IDR recovery when
  large input is rejected, including with older libcrystalhd.
- Correct VA VPP capability metadata, derived uploads and private queued
  synchronization. Cache DRM discovery metadata to reduce setup overhead
  without disabling scaling/CSC or weakening exposed-buffer fences.
- Reuse bounded retired private CPU backing without reusing live pictures;
  reduce scalar packed-pixel conversion overhead while retaining field-local
  chroma and the no-SSE build path.
- Improve damaged MPEG-2 admission, EOS diagnostics and healthy-picture PTS;
  reject unsupported interlaced scaling explicitly rather than showing audio
  alone.
- Add bounded HTTP(S) input to `crystalhd-play` and correct module/runtime
  discovery diagnostics. Selected loopback HTTP decoding is checked; HTTPS
  and physical network-playback acceptance remain unverified.
- Keep selected interlaced Weave playback separate from deinterlacing.
  [Known Kodi/interlaced limits and planned v1.2 integration work](USAGE.md#known-issues-and-deinterlacing-roadmap)
  remain open; this candidate adds neither hardware Bob nor a general
  full-rate FHD/A/V qualification.

## 1.1.0-rc.3 — release candidate

Pre-release scope is tracked in
[#430](https://github.com/ahnhy1324/crystalhd/issues/430). Long-duration and
final stable-release qualification follow the immutable RC; this is not a
stable v1.1.0 support declaration. Earlier rc.1/rc.2 identities were frozen
development candidates, not published v1.1 releases.

- Compatibility reports [#412](https://github.com/ahnhy1324/crystalhd/issues/412)–[#415](https://github.com/ahnhy1324/crystalhd/issues/415)
  from [@sjq42](https://github.com/sjq42) led to normal i686 VA SSE2 selection,
  preservation of required VA build flags with external `CXXFLAGS`, bounded
  large H.264 access units with old-library fallback, selected H.264 MBAFF
  field pairs, and 32-bit player bus-message handling. These do not imply
  general interlaced VA, Atom or browser support.
- Selected H.264 MBAFF pairs materialize as complete interlaced NV12 Weave
  frames. Earlier unpublished RC.2 payload `f4d99d5` completed the 750-picture
  fixture with identical luma to software decode, and standalone FFmpeg BWDIF produced
  1,500 progressive outputs. Whole-picture storage is also accepted by VA and
  GStreamer without weakening compact-field ownership checks. Finite MBAFF
  prefeed fixes the last-field/EOS wait while explicit live gaps remain
  nonterminal and explicit context close drains the tail.
- `crystalhd-play --hardware --weave` and `--software --weave` select card or
  CPU decoding without automatic deinterlacing; ordinary playback defaults
  are unchanged. Stock Kodi progressive lifecycle and selected interlaced
  playback are verified within the recorded scope. Deinterlacing quality,
  A/V synchronization tuning and full-rate FHD presentation remain follow-up
  work in [#436](https://github.com/ahnhy1324/crystalhd/issues/436); no VA
  hardware-deinterlacing capability is advertised.
- The read-only configuration inspector, reviewed presets and redacted bug
  report are additive tooling; configuration files do not apply runtime
  settings.
- Driver and library work hardens decoder stop, suspend/resume, Flea power and
  receive ownership. Default-off firmware investigations remain research, not
  new supported hardware functions.
- VA teardown now retries busy hardware closure without losing ownership, and
  allocation failures release hardware output while failing the affected
  surface cleanly.
- The candidate keeps the legacy ioctl version and the `libcrystalhd.so.3`
  ABI. Its bundle/package mapping is `1.1.0-rc.3` / `3.10.6`; v1.0
  tags and qualification evidence remain immutable historical baselines.

## 1.0.0 — October 13, 2026

First numbered bundle release of this maintained fork, following the immutable
`phase1-baseline-2026-09-29`. [Published release](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.0.0);
qualification and publication are recorded in
[#369](https://github.com/ahnhy1324/crystalhd/issues/369).

- Local playback: accumulated DMA-stop, queued-seek, EOS and reopen fixes;
  broader selected GStreamer/VA-API codec and interlace coverage, with visible
  hardware selection in `crystalhd-play`.
- Receive efficiency: negotiated event-driven output readiness with fallback
  for older drivers/libraries, plus bounded default H.264 transport history.
- Optional features: session-scoped BCM70015 L0s workaround, progressive
  hardware output scaling and explicit software enlargement. Native-size
  output remains the default.
- Streaming: documented Moonlight frame-matching playback and opt-in
  previous-frame mode for higher cadence, with measured timing/CPU tradeoffs.
- Compatibility: retain `libcrystalhd.so.3`, legacy ioctl behavior, x86-64/i686
  builds and old-GStreamer support; separate bundle and component versions.
- Installation: keep public module/runtime paths and module indexes readable
  when the invoking shell uses a restrictive file-creation mask.
- Limits: BCM70015 local playback is the primary path. BCM70012, additional
  hosts and browser hardware coverage remain unvalidated; standalone video
  processing and V4L2 research are not advertised as working frontends.

See [README.md](README.md) for first use and [RELEASING.md](RELEASING.md) for
qualification and publication policy.

## Project lineage

There are various versions of the [Broadcom
CrystalHD](https://en.wikipedia.org/wiki/Broadcom_Crystal_HD) (BCM70012 and
BCM70015) drivers floating around the web.

The main public lineages, roughly from oldest to newest, are:

1. Staging driver from [Linux kernel v3.16](https://git.kernel.org/pub/scm/linux/kernel/git/gregkh/staging.git/tree/drivers/staging/crystalhd?h=v3.16)
  — this version was [removed in 2014 from v3.17](https://lkml.iu.edu/hypermail/linux/kernel/1408.0/01475.html) due to
  the fact that it was unmaintained and obsolete; it only supported the
  BCM70012 chip, for example.

  * See the [2013 LKML discussion](https://lkml.org/lkml/2013/10/27/103)
    between Steven Newbury and Greg Kroah-Hartman.

2. The historical Debian packaging tree,
   which appears to be based on a ~2010 version of the code from the
   mainline kernel, and like it only supports the BCM70012 chip.

3. Jarod Wilson's LinuxTV tree, last updated in 2012.

4. [@yeradis](https://github.com/yeradis)'s
   [tree](https://github.com/yeradis/crystalhd), forked from Jarod's tree.

5. [@dbason](https://github.com/dbason)'s
   [tree](https://github.com/dbason/crystalhd), forked from Yeradis's tree
   and updated for the kernels available in 2016.

The dbason tree was the practical modern option when this history was first
written and was reported working with Linux 4.4 and BCM70015. That statement
is historical; it does not describe current kernels or this repository.

## April 2025 maintenance milestone

The fork added Ubuntu CI for the driver and userspace components, updated the
kernel build for newer APIs, and refreshed the example and build documentation.
CI compilation does not load the PCI device or replace hardware testing.

## August 2026 maintenance milestone

The userspace integrations in this update are experimental. See
[README.md](README.md) and the component guides before deployment.

- Set Linux 6.1 as the then-current minimum supported kernel and added CI
  compilation against 6.1, 6.6, 6.12, and 6.18 LTS releases, stable, and
  mainline. Linux 5.15 support followed in September 2026.
- Added a GStreamer 1.x decoder and an experimental VA-API backend, including
  the buffer and video-processing operations needed by the documented clients.
- Added DKMS support and optional browser integration tooling.
- Hardened the kernel device lifetime for safe rejection of a second playback
  client. CrystalHD still supports only one active hardware session.
- Hardware-tested H.264 Constrained Baseline, Main, and High decoding on a
  BCM70015 with Ubuntu kernel 6.17.0-41-generic.

## October 2026 library addition

Added an opt-in raw-frame uploader, two-reference tile compositor and bounded
quarter-pixel translator using native H.264, including bounded non-reference
composition and translation. Existing library ABI and default decode paths
are preserved; this does not expose standalone MFD processing.

## October 2026 playback scaling

Added opt-in progressive BCM70015 decoder-output width selection to GStreamer
and `crystalhd-play`, plus an explicit bounded software output-size stage.
Native output remains the default and the legacy library ABI is unchanged.

See [README.md](README.md) for current build, installation, playback, and test
instructions. This file is only the project history.
