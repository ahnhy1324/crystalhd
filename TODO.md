# Open work

Release status **2026-10-19**: [stable 1.0.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.0.0)
and previous RCs remain unchanged. Published
[v1.1.0-rc.5](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0-rc.5)
/ package `3.10.8` adds VA decode/lifetime fixes; its
[qualification summary](HARDWARE-2026-09-13.md#110-rc5-qualification--2026-10-19)
distinguishes fresh candidate checks from remaining work.
Selected Weave playback remains the scope, with the
[known deinterlacing limits and planned v1.2 work](USAGE.md#known-issues-and-deinterlacing-roadmap)
kept separate from current functionality.
The current source prepares final `1.1.0` / package `3.10.9`. The maintainer
authorized publication after the [required final-package checks](HARDWARE-2026-09-13.md#110-final-package-qualification--2026-10-19)
pass; actual results and publication state are recorded in #430. This does not expand the tested scope to general
full-rate FHD/deinterlacing support.

- [ ] [#430: v1.1.0 RC qualification and GO/HOLD](https://github.com/ahnhy1324/crystalhd/issues/430)
  — RC5 is published; fresh Gate A, the 11-fixture oracle, 3-hour 720p,
  2-hour native/scaled FHD and core lifecycle/aggregate checks pass.
  VA event-worker endurance/reopen and affected large-AU replay/seek checks
  also pass. Long-HTTP output/EOS/reopen completed, but its shutdown-warning
  raw FAIL/OBSERVED_NOT_PASS result is retained, not an unconditional gate PASS.
  Fresh Moonlight source preparation fails with Sunshine 503 capture/encoding
  initialization; three 60-minute modes/reopen remain NOT RUN/source BLOCKED.
  RC5's final installed 360+360-picture handoff also passes. Fresh final-package
  CI/build/archive/install/RC5 rollback and affected smoke checks must pass
  before the authorized stable publication; their current state is in #430. See the
  [RC5 execution record](https://github.com/ahnhy1324/crystalhd/issues/430#issuecomment-6247676972).
  Preserve older RC results and FAILs with their original identities; no
  earlier result substitutes for a new candidate gate. Keep the existing
  maintenance-only 3-hour profile and all other thresholds unchanged.
  All existing issues stay open. Selected local-player dialogue A/V has
  subjective acceptance; quantitative timing, other frontends and external
  final-package acceptance are not inferred from automated tests.
- [ ] [#429: External i386/BCM70015 acceptance](https://github.com/ahnhy1324/crystalhd/issues/429)
  — installed RC4 Atom N280/i386 results cover selected codec, player and
  stock-Kodi paths; retain the original RC3 failures. Ordered B-pyramid
  rejection recovery, exact Kodi output/physical A/V and the other open
  checklist rows remain incomplete. Later primary-host long playback and
  ELF32 scalar-build hardware controls are recorded in
  [the focused investigation](https://github.com/ahnhy1324/crystalhd/issues/429#issuecomment-6245398250);
  they do not replace reporter-host or physical A/V acceptance.
- [ ] [#436: planned v1.2 deinterlacing and presentation work](https://github.com/ahnhy1324/crystalhd/issues/436)
  — after final publication, prioritize a truthful CPU VA-VPP Bob path for
  the existing Kodi Auto problem in [#445](https://github.com/ahnhy1324/crystalhd/issues/445):
  actual filter execution, field order/timing, ownership, seek/EOS and
  i686/scalar behavior before advertising capability. Query-only Weave or
  returning unchanged Weave for Bob is not an implementation. Keep existing
  Weave playback intact; physical cadence/sync and FHD performance require
  their own measurements. Hardware Bob remains separate research, not a
  prerequisite for this software integration.
- [ ] Player HTTP(S) acceptance — implementation is included in RC4, with
  bounded errors and explicit hardware/software selection. Selected native
  loopback HTTP decoding/EOS/reopen passed; HTTPS, physical A/V, network
  seeking and Internet recovery remain unverified.

- [x] [#369: CrystalHD 1.0.0 final gates and release](https://github.com/ahnhy1324/crystalhd/issues/369)
  — G0–G6 and experimental dispositions are complete. The exact commit and
  release text were approved; stable 1.0.0 and its public archive are verified.
- [x] [#371: Pre-1.0 recovery, LTS admission and full qualification](https://github.com/ahnhy1324/crystalhd/issues/371)
  — G0–G4 complete: same-RC LTS admission, core endurance/lifecycle, scaled
  output and VA event-worker endurance. Moonlight's three one-hour targets
  and corrected reopen pass hardware/lifecycle/resources; physical A/V and
  payload-age behavior retain an explicit limited/unverified disposition.
  Final package `f41ca40` passes direct/DKMS installation, RC rollback and
  installed-path checks; G6 documentation/decision handoff is complete.
  Preserve the existing backup under the G0 exception.
  The release and public-archive verification are complete in #369.
- [ ] [#91: Deferred hardware and application acceptance](https://github.com/ahnhy1324/crystalhd/issues/91)
  — alternate hardware, the LTS item executed through #371, bounded BCM70015
  deep-S3 coverage completed through #395/#397, remaining fault/removal work,
  broader streams, and physical presentation/A/V synchronization.
- [ ] [#92: BCM70015 capability research](https://github.com/ahnhy1324/crystalhd/issues/92)
  — pinned firmware mapping and live OPEN/scaling evidence; standalone
  processing and backend reuse remain unvalidated.
- [ ] [#204: BCM70015 picture ownership and engine lifecycle](https://github.com/ahnhy1324/crystalhd/issues/204)
  — #92 follow-up for runtime picture identity, PPB lifetime, and MFD/BVN
  completion.

Acceptance checklists live in those issues. Current usage and support are in
[README.md](README.md); the frozen hardware reference is in
[HARDWARE-2026-09-13.md](HARDWARE-2026-09-13.md). The stable release is bundle
`1.0.0`, module/DKMS `3.10.3`; RC.1 (`1.0.0-rc.1` / `3.10.2`) and the
Phase 1 baseline remain unchanged. Mandatory missing tests remain
NOT RUN/HOLD; only optional work may be deferred. Historical Phase 1 and
bounded RC evidence do not establish new LTS/full-duration passes.

## Interlaced cadence and reduced-output follow-up (#436)

Decision, 2026-10-16: investigate maintaining every field's output cadence with
bounded additional latency, rather than improving the displayed fps by skipping
or repeating content. These are post-RC3 experiments, not new support claims or
prerequisites for the already frozen playback scope. Do not interrupt the
serial release qualification or change its checkout, installed payload, L0s
settings, thresholds or preserved failures to run them.

### Established baseline

- 1080i50 / 1080i25 here means 25 complete interlaced pictures and 50 fields per
  second. BWDIF `send_field` produces 50 progressive outputs from those 25
  decoded pictures; it does not require decoding 50 complete FHD pictures/s.
  `send_frame` instead retains 25 outputs/s and loses field-rate motion cadence.
- RC3's three-repeat FHD benchmark reached 39.29 pictures/s for NV12 Weave and
  28.30 input pictures/s / 56.60 output frames/s with CPU BWDIF `send_field`.
  The latter ranged 55.74–56.99 outputs/s, giving about 11.5–14% throughput
  headroom over 50p, **without audio or display**. This is not a deadline,
  physical A/V, pure-decoder or hardware-deinterlacing result.
- The 720i benchmark's 122.05 BWDIF outputs/s used an original 720i input. It
  cannot be assigned to FHD input followed by card downscaling. Methods, CPU,
  memory and separate RAM-filter costs remain in the existing
  [benchmark section](HARDWARE-2026-09-13.md#rc3-interlaced-null-output-throughput-2026-10-16).
- [Native scaling evidence](tests/fixtures/issue436/native-single-field.json)
  already includes uniform-TFF FHD input returning 960x540 interlaced output,
  packed as 960x270 fields: 50 input pictures produced 100 fields normally,
  50 with field omission, then 100 after normal reopen. EOS/recovery and
  selected-field byte comparisons passed. This is not a scaling-quality or
  throughput result. Use normal `single_field=0` for the full-cadence proposal;
  omitting alternating fields is not Bob or 50p preservation.

### READY / asynchronous delivery candidate

- Overlapping decode, CPU filtering and presentation may reduce waiting and
  cadence stalls. READY alone does not remove Weave combing or supply a
  deinterlacer, and cannot accelerate the underlying FHD decode work.
- Existing `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP=1` requires explicit
  `CRYSTALHD_VAAPI_LIVE_H264=1` and
  `CRYSTALHD_VAAPI_LOW_LATENCY_H264=1`. It returns a completed older payload
  through a newer requested surface/timestamp; it is not merely ordinary
  asynchronous completion. B-slices are rejected. Selection checks context,
  generation, dimensions and older submission order, not field parity.
- There is no measured interlaced READY+BWDIF result. Any comparison must
  preserve/trace original payload identity, PTS and field order, handle temporal
  neighbors, and distinguish `previous` from potentially skipping `latest`.
  Render submissions alone are not proof of unique 50p outputs or correct A/V.
  Prefer a bounded, fixed-latency queue with content continuity; its required
  depth/latency and benefit remain measurements, not confirmed capabilities.

### Card 720 output, deinterlace, then enlarge

Target: **1080i25 decode → field-preserving card 720i25 output → CPU BWDIF
720p50 → display-side enlargement to 1080p50**, preferably using the display
GPU rather than adding a second FHD CPU filter. At the same pixel format,
1280x720 has 44.4% of FHD's active pixels, so it could reduce PCIe payload and
filter work; decode remains FHD and enlargement cannot restore lost detail.

GStreamer's production `scale-width` currently rejects interlaced input/output;
VA has no card-scaling control and expects native stream geometry. Existing VA
VPP resizing happens on the host after readback, so it does not reduce PCIe
traffic or establish field-preserving vertical scaling. Keep these boundaries
until the adapters are implemented and validated, rather than just removing
their guards.

- [ ] Reuse the existing uniform-TFF 2-second / 50-picture fixture and
  `tests/fw-single-field-decode.cpp --scale-width 1280`, with normal field
  output and no omission. Verify actual geometry/extent (expected 1280x360
  per field, not assumed from the requested width), 100 fields, 50 unique
  input identities, both parities, EOS, cleanup and native reopen.
- [ ] Pair fields from the same input into 50 Weave pictures; verify retained
  field order/content before filtering. Compare BWDIF's 100 progressive outputs
  against the unscaled FHD path using the same input. Keep uniform TFF, BFF
  and mixed per-picture parity as distinct cases.
- [ ] If correctness passes, compare paced 25-input/50-output playback and
  unpaced throughput, CPU/RSS, unique outputs, gaps/drops, EOS and reopen for
  native FHD+BWDIF versus card-720+BWDIF+enlargement. Measure physical A/V
  separately when observable; batch wall time is not per-frame latency.
- [ ] Check visible-height 1080 separately from VA coded-height 1088 before
  frontend integration. Only then evaluate READY on an eligible input, keeping
  B-frame limitations and original payload timestamps explicit. Reuse the
  baseline and completed benchmarks instead of repeating unrelated gates.
