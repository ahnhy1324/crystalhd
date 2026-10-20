# Open work

Release status **2026-10-19**: [stable 1.1.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0)
/ package `3.10.9` is published. Previous stable releases and RCs remain unchanged.
The [qualification summary](HARDWARE-2026-09-13.md#110-final-package-qualification--2026-10-19)
distinguishes fresh final-package checks from scoped RC5 evidence reuse.
Selected Weave playback remains the scope, with the
[known deinterlacing limits and planned v1.2 work](USAGE.md#known-issues-and-deinterlacing-roadmap)
kept separate from current functionality.
Development now turns to the existing v1.2 integration work below, without
expanding the released scope to general full-rate FHD/deinterlacing support.

- [x] [#430: v1.1.0 release qualification and publication](https://github.com/ahnhy1324/crystalhd/issues/430)
  — final-source CI/build/archive, direct/DKMS installation/RC5 rollback and
  affected installed native/VA/player/Kodi checks pass. Publication and public
  archive verification are complete under the maintainer's GO. See the
  [final execution record](https://github.com/ahnhy1324/crystalhd/issues/430#issuecomment-6256652744).
  RC5 endurance retains its original identity. HTTP's raw FAIL/OBSERVED_NOT_PASS
  and Moonlight's three 60-minute modes/reopen NOT RUN/source BLOCKED remain
  explicit limitations, not converted passes. This checkbox records completed
  release work, not acceptance of those limitations.
  Preserve older RC results and FAILs with their original identities; no
  earlier result substitutes for a new candidate gate. Keep the existing
  maintenance-only 3-hour profile and all other thresholds unchanged.
  Selected local-player dialogue A/V has
  subjective acceptance; quantitative timing, other frontends and external
  final-package acceptance are not inferred from automated tests.
- [ ] [#429: External i386/BCM70015 acceptance](https://github.com/ahnhy1324/crystalhd/issues/429)
  — retain the RC3 failures and scoped RC4 results. The
  [new RC5 reporter run](https://github.com/ahnhy1324/crystalhd/issues/429#issuecomment-6257883927)
  adds three-version Kodi long-playback/EOS/reopen, native scalar VA playback
  and short progressive/Off physical A/V measurements. Exact Kodi output
  identity, interlaced Bob A/V, hour-long measured A/V drift and out-of-policy
  B-pyramid recovery remain open; these are not new 1.2 Bob acceptance.
- [x] [#468: admitted real B-pyramid input can stop output](https://github.com/ahnhy1324/crystalhd/issues/468)
  — the real-content failure is reproduced and the development extended-input
  guard passes bounded native rejection/fresh-reopen controls, intentionally
  rejecting some formerly working input. A single-NAL input-retention limitation
  is now [measured](HARDWARE-2026-09-13.md#h264-input-retention-investigation);
  disposition is a signed-firmware/configuration compatibility limitation,
  not full decoding repair. Affected continuous playback uses software decode;
  hardware failure permits a clean reopen of supported input. Internal reclamation
  remains research, not a closure prerequisite. Ordinary assembled inputs
  at/below 2 MiB are unchanged.
- [ ] [#436: planned v1.2 deinterlacing and presentation work](https://github.com/ahnhy1324/crystalhd/issues/436)
  — qualify the development CPU VA-VPP Bob path for
  the existing Kodi Auto problem in [#445](https://github.com/ahnhy1324/crystalhd/issues/445):
  actual filter execution, field order/timing, ownership, seek/EOS and
  i686/scalar behavior. The 1.2 branch has bounded native TFF/BFF pixel/clock and
  short Kodi Auto/Off controls as well as device-free coverage. One headless
  FHD Bob batch reaches 49.759 outputs/s, not demonstrated real-time 50p headroom.
  A separate three-hour 720i25 headless concat run passes 540,000 exact Bob outputs,
  EOF and fresh Weave reopen; default FFmpeg stream-loop remains 296/300 FAIL
  ([scope and retained failures](https://github.com/ahnhy1324/crystalhd/issues/445#issuecomment-6257714172)).
  Kodi21.3 directly observed Bob metadata still duplicates each field-pair PTS;
  one seek takes its 5-second cache-startup fallback. Uniform presentation is not
  qualified. Selected 720i25 dialogue A/V and controls have qualitative maintainer
  confirmation; calibrated/FHD timing and displayed endurance remain open. The
  [native Atom report](https://github.com/ahnhy1324/crystalhd/pull/467#issuecomment-6272584392)
  adds passing tests/pixel controls but failing Kodi Bob timing and LIVE-EOS
  on its earlier source. A [current-userspace stock Kodi control](https://github.com/ahnhy1324/crystalhd/issues/445#issuecomment-6289514942)
  completes all 750 originals / 1,500 Bob operations and natural EOF under
  LIVE+LL; presentation and current Atom acceptance remain open.
  Keep existing Weave playback intact. Hardware Bob remains separate research,
  not a prerequisite for this software integration.
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
`1.1.0`, module/DKMS `3.10.9`; stable 1.0.0, RC.1 (`1.0.0-rc.1` / `3.10.2`) and the
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

GStreamer's `scale-width` now accepts the validated aligned BCM70015 H.264
separate-field cases, with native capture, field assembly and host scalerbob
results in the [existing receipt](tests/fixtures/issue436/card-scaled-bob.json).
This does not qualify other interlaced codecs or BWDIF plus enlargement.
VA still has no card-scaling control and expects native stream geometry. Existing VA
VPP resizing happens on the host after readback, so it does not reduce PCIe
traffic or establish field-preserving vertical scaling. Keep these boundaries
until the adapters are implemented and validated, rather than just removing
their guards.

- [x] Capture the existing uniform-TFF 2-second / 50-picture fixture with
  `tests/fw-single-field-decode.cpp --scale-width 1280`: actual 1280x360
  fields, 100 outputs, both parities per token, EOS and cleanup pass in the
  [receipt](tests/fixtures/issue436/card-scaled-bob.json). This is not a
  new measurement or a scaled-reopen qualification.
- [x] Pair the captured fields into 50 scaled Weave pictures; all active YUY2
  bytes match independent same-token assembly in that receipt.
- [ ] Verify scaled native reopen and compare BWDIF's 100 progressive outputs
  against the unscaled FHD path using the same input. Keep uniform TFF, BFF
  and mixed per-picture parity as distinct cases.
- [ ] If correctness passes, compare paced 25-input/50-output playback and
  unpaced throughput, CPU/RSS, unique outputs, gaps/drops, EOS and reopen for
  native FHD+BWDIF versus card-720+BWDIF+enlargement. Measure physical A/V
  separately when observable; batch wall time is not per-frame latency.
- [ ] Check visible-height 1080 separately from VA coded-height 1088 before
  frontend integration. CPU Bob already tests visible 1080 inside 1088 backing
  at 720 output for both parities; this is not a card-capture result. VA rebuilds
  an uncropped SPS, unlike the tested GStreamer input. Measure its actual scaled
  capture geometry before designing integration that preserves public decoded
  surfaces. Only then evaluate READY on an eligible input, keeping
  B-frame limitations and original payload timestamps explicit. Reuse the
  baseline and completed benchmarks instead of repeating unrelated gates.

## VA-API compatibility backlog (#445 / #436)

Prioritize correctness within the advertised decode/VPP subset, using the
[existing compatibility investigation](https://github.com/ahnhy1324/crystalhd/issues/445#issuecomment-6257714172).
Missing optional VA operations are not all release requirements.

- [x] P0 — Implement nonterminal completion for eligible BCM70015 MBAFF LIVE Bob.
  `2c79f1e` supplies a separately owned non-reference intra picture, retaining
  original references and public pixels without inferring EOS from an input gap.
  Exact-source mid-gap/EOF Bob and source synchronization complete before close;
  all 50 original NV12/PTS comparisons pass afterward. Actual close drains private
  output/EOS separately. The [current-userspace stock Kodi control](https://github.com/ahnhy1324/crystalhd/issues/445#issuecomment-6289514942)
  independently completes 750 originals / 1,500 Bob operations and natural EOF
  with the stable kernel. This replaces neither the earlier private-prototype
  record nor general LIVE/presentation acceptance. See [the pixel/continuation result](https://github.com/ahnhy1324/crystalhd/issues/445#issuecomment-6286955966).
  A pinned drain/replay checkpoint prototype passed a short own-client
  continuation and pixel control, but is not enabled: stock Kodi required
  243-picture reconstruction (5.66 s) and exceeded its 2 s output wait.
  Cache/work bounds do not bound client latency. Preserve that failure and the
  negative filler-only trials; do not widen timeouts or synthesize missing fields.
- [x] P0 — Attribute the stock Kodi 21.3 duplicate field-PTS boundary. Its
  second-field timestamp calculation requires prior-picture metadata that the
  zero-reference Bob path discards. Source and the existing 1,500-output trace
  agree. VA VPP cannot return output PTS; retain truthful zero temporal references
  and immutable decode tokens. The existing FFmpeg VA field-rate control passes
  100 outputs at 20 ms PTS intervals without an application patch.
- [ ] P0 — Complete remaining presentation/reporter acceptance: current-source
  native i386/Atom, FHD displayed cadence and physical A/V, broader LIVE inputs
  and seek recovery. The bounded 10.100-second seek avoids the earlier long-GOP
  preroll and 5-second startup fallback, but video acknowledgement still arrives
  about 1.47 seconds after the demuxer seek log (1.466 seconds with current
  userspace/stable kernel). This is not E2E, a same-version A/B, qualification
  of the new kernel wait path in Kodi, or warning-free acceptance.
  Keep existing tail/API/pixel successes separate from these qualifications.
- [x] P1 — Validate requested RT formats and slice modes; retain selected output
  formats through queries, context lifetime and target binding without restricting
  VPP input. Zero-filter VPP and Bob reject unsupported meaningful requests while
  retaining valid defaults/no-ops. Device-free regressions verify ID/pixel/lease
  safety and release obsolete unsubmitted captures after rejected replacements.
- [ ] P2 — Define color conversion explicitly. Direct packed-output `StoreArgb`
  uses fixed limited-range BT.601 coefficients; VPP ARGB uses swscale without
  explicit matrix/range negotiation. Neither establishes selectable BT.709 or
  full-range conversion support. Explicit color standards are rejected; existing
  range/chroma metadata handling is retained for ordinary client pipelines, not
  qualified as selectable conversion. Add conversions only with independent
  pixel oracles and matching advertised capabilities.
- [x] P2 — Unsupported display attributes return an error; valid empty requests
  remain no-ops. Invalid arguments do not access or change caller attributes.
- [ ] P2 — Evaluate `vaQuerySurfaceError` using only information
  actually available for the exact live surface; do not invent macroblock or
  hardware-reset details. Cover invalid IDs, lifetime and unsupported requests.

Intentional unsupported scope includes legacy `vaPutSurface` presentation,
subpictures/palettes/display controls, temporal deinterlacing and other unexposed
VPP transforms, encoding, protected playback and codecs outside advertised
profiles. Do not advertise these merely to make an application probe succeed.
Use the [libva API contract](https://github.com/intel/libva/blob/2.23.0/va/va.h)
and [VPP contract](https://github.com/intel/libva/blob/2.23.0/va/va_vpp.h) for
feature-specific acceptance, not a blanket claim of full VA-API conformance.
