# Open work

Release status **2026-10-23**: [stable 1.2.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0)
with module-DKMS identity `3.10.13` and prereleases
[`v1.2.0-rc.1`](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0-rc.1)
(`3.10.11`) and
[`v1.2.0-rc.2`](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0-rc.2)
(`3.10.12`) are published and immutable. RC2 completed its bounded G0–G3
qualification in [#473](https://github.com/ahnhy1324/crystalhd/issues/473).
Fresh final-package checks, explicit maintainer GO and public-asset verification
are complete. Master is now unreleased `1.3.0-dev.1` / `3.10.14`.
Previous stable releases and RCs remain unchanged.
The [qualification summary](HARDWARE-2026-09-13.md#120-final-package-qualification--2026-10-23)
distinguishes fresh final-package checks from scoped RC2 evidence reuse.
Selected v1.2 CPU Bob, card scaling and reliability work is published;
broader performance and research remain in
[#472](https://github.com/ahnhy1324/crystalhd/issues/472) and do not expand the
scope to general full-rate FHD or hardware deinterlacing.

- [x] [#473: v1.2.0 bounded release scope and gates](https://github.com/ahnhy1324/crystalhd/issues/473)
  — RC2 completed G0/G1, selected Kodi/720p50/endurance G2 and direct/DKMS
  package/rollback G3. Its retained strict Kodi count failures are bounded to
  client adaptive field omission, not hidden as lossless 50p support. The
  distinct final `1.2.0` / `3.10.13` package, maintainer GO and downloaded
  public archive are qualified and published; original evidence and limits
  remain unchanged.
- [ ] [#472: v1.2 best-effort experiments and stop rules](https://github.com/ahnhy1324/crystalhd/issues/472)
  — retain optional Kick, memory, FHD/Atom, Moonlight and hardware-research
  follow-up without making it a release blocker or support claim.

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
  The matched output-size screen rules out deterministic 900p/990p failure:
  low-iowait repeats are zero-drop, while both FHD repeats retain small losses at low
  iowait. Next: attribute the remaining explicit-FHD QoS/deadline loss after
  native DI-QoS integration (30-minute run: 174/90,000 lost outputs); keep the
  intermittent firmware/input-space stall separate. See the
  [presentation results](HARDWARE-2026-09-13.md).
- [ ] [#400: Managed settings and desktop integration](https://github.com/ahnhy1324/crystalhd/issues/400)
  — shared storage, native readers, named profiles, Player defaults and the
  optional settings/tray editor are integrated through #498/#499/#502–#504.
  Installed-reader declarations complement bounded ordinary installed-client
  activation checks; neither a saved value nor a file declaration alone proves
  a running app applied it. Broader desktop/application acceptance stays open.
- [ ] [#470: Output-aware RX allocation](https://github.com/ahnhy1324/crystalhd/issues/470)
  — checked allocation/registration and reduced-output sizing are integrated
  through #479/#480; #507 adds retired-page reclamation and selected native
  EOS/reopen qualification. Physical paced A/B, BCM70012 and broader PM/format
  acceptance remain open; the fixed default and DMA quarantine stay intact.
  See the [retirement results](HARDWARE-2026-09-13.md#rx-pool-retirement-and-reopen-2026-10-24).
  The former fixed-size pool alone was not evidence of a DMA overrun.
- [ ] [#471: Decoded-output traffic and conversion](https://github.com/ahnhy1324/crystalhd/issues/471)
  — #477 records native/centered YUY2 completion spans, distinct from wire
  accounting and RX allocation. Next locate packed-4:2:2 decode-format,
  export/import and renderer negotiation boundaries in unmodified clients.
  #508 reuses repeated CPU Bob resampling and accepts standard VA FAST with
  unchanged nearest pixels; [scaler comparisons and native checks](HARDWARE-2026-09-13.md#output-scaling-cost-2026-10-25)
  retain their stage/batch limits. NV12 decode/VPP format negotiation is
  measured; packed export/EGL/rendering and displayed cadence remain open.
  #509 additionally reduces ordinary NV12 VPP clearing/resampling; native
  pixel/PTS checks pass. Whole-Player nearest/bilinear comparisons retain
  presentation deficits, so lower CPU does not promote a zero-loss profile.
  Direct 4:2:2 VA/Kodi negotiation is not part of the v1.2 candidate.
- [ ] [#477: Centered card output for ordinary VA clients](https://github.com/ahnhy1324/crystalhd/issues/477)
  — centered output and async completion are integrated through #494;
  retain small Bob, full original readback and exact lifetime semantics.
  New matched L0s/native/centered measurements qualify the selected progressive 30p
  five-minute centered path in both link states; native-ON falls short. New
  interlace/60p misses and physical-versus-software observations remain explicit
  in the [results](HARDWARE-2026-09-13.md#stock-kodi-centered-output-with-l0s-2026-10-24).
  Broader codec/host and zero-loss presentation acceptance remain open. See the
  [development usage](filters/vaapi/README.md#development-experiment-centered-card-output).
- [ ] [#92: BCM70015 capability research](https://github.com/ahnhy1324/crystalhd/issues/92)
  — pinned firmware mapping and live OPEN/scaling evidence; standalone
  processing and backend reuse remain unvalidated.
- [ ] [#204: BCM70015 picture ownership and engine lifecycle](https://github.com/ahnhy1324/crystalhd/issues/204)
  — #92 follow-up for runtime picture identity, PPB lifetime, and MFD/BVN
  completion.

Acceptance checklists live in those issues. Current usage and support are in
[README.md](README.md); the frozen hardware reference is in
[HARDWARE-2026-09-13.md](HARDWARE-2026-09-13.md). The stable release is bundle
`1.2.0`, module/DKMS `3.10.13`; stable 1.0.0, RC.1 (`1.0.0-rc.1` / `3.10.2`) and the
Phase 1 baseline remain unchanged. Mandatory missing tests remain
NOT RUN/HOLD; only optional work may be deferred. Historical Phase 1 and
bounded RC evidence do not establish new LTS/full-duration passes.

## Interlaced cadence and reduced-output follow-up (#436)

Decision, 2026-10-16: investigate maintaining every field's output cadence with
bounded additional latency, rather than improving the displayed fps by skipping
or repeating content. The selected CPU Bob and H.264 card-scaling subset is now
a conditional v1.2 claim governed by #473; its exact-candidate checks are release
gates. Broader READY, FHD/Atom and hardware-deinterlacing experiments remain
nonblocking under #472. Do not interrupt serial release qualification or change
its checkout, installed payload, L0s settings, thresholds or preserved failures
to run optional experiments.

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

Development order: complete VA scaling integration before expanding codec
admission. Keep card-side transfer savings separate from host VPP corrections.

Target: **1080i25 decode → field-preserving card 720i25 output → CPU BWDIF
720p50 → display-side enlargement to 1080p50**, preferably using the display
GPU rather than adding a second FHD CPU filter. At the same pixel format,
1280x720 has 44.4% of FHD's active pixels, so it could reduce PCIe payload and
filter work; decode remains FHD and enlargement cannot restore lost detail.

GStreamer's `scale-width` now accepts the validated aligned BCM70015 H.264
separate-field cases, with native capture, field assembly and host scalerbob
results in the [existing receipt](tests/fixtures/issue436/card-scaled-bob.json).
The separate [BWDIF qualification](HARDWARE-2026-09-13.md#card-scaling-with-stock-bwdif--2026-10-20)
now covers bounded TFF/BFF reopen, complete pixels/PTS and CPU enlargement.
This does not qualify other interlaced codecs or physical presentation.
VA now exposes opt-in H.264 decode-integrated card scaling, qualified
below. Separate VA VPP resizing still happens on the host after readback and
does not reduce PCIe traffic. Ordinary clients must explicitly select the
integrated API to obtain smaller card output; stock FFmpeg/Kodi do not do so.

- [x] Capture the existing uniform-TFF 2-second / 50-picture fixture with
  `tests/fw-single-field-decode.cpp --scale-width 1280`: actual 1280x360
  fields, 100 outputs, both parities per token, EOS and cleanup pass in the
  [receipt](tests/fixtures/issue436/card-scaled-bob.json). This is not a
  new measurement or a scaled-reopen qualification.
- [x] Pair the captured fields into 50 scaled Weave pictures; all active YUY2
  bytes match independent same-token assembly in that receipt.
- [x] Verify scaled native reopen and BWDIF's 100 progressive outputs. Uniform
  FHD TFF and BFF each pass two same-process scaled opens. Five two-open filter
  conditions cover TFF native/720/enlarged and BFF 720/enlarged: all input and
  progressive-output bytes/PTS match their respective field-derived references.
  Different resolutions are not pixel-identical; the filter reference uses the
  same stock BWDIF implementation, not an independent algorithm.
- [x] Compare paced 25-input/50-output and unpaced native FHD+BWDIF,
  card-720+BWDIF, and card-720+BWDIF+CPU enlargement: three rounds per cell,
  all 27,000 outputs, PTS, EOS and clean close pass. Preserve CPU/RSS, latency,
  burst gaps and the slower enlargement round in the existing receipt.
- [x] Qualify the actual Player display path with `--scale-width 1280` and no
  explicit software output size. One five-minute Xv/Pulse run renders all
  15,000 fields, with 14,999/14,999 native DI-QoS inspect/forward events and
  no failure, 37.02% of one CPU core, natural EOS and a clean boundary. Xv
  receives 1280x720 UYVY and owns display fitting; this is not physical
  scanout, A/V or interpolation-quality acceptance.
- [ ] Qualify physical A/V/display cadence and genuinely changing decoded
  field order separately. Mixed SEI labels are not a decoded-parity oracle:
  the existing 30-second fixture produces 750 BFF pictures in both software
  and hardware despite its stream-level TFF label. Batch wall time is not
  per-frame latency; these headless passes do not establish smooth presentation.
- [x] Measure coded-height 1088 separately from visible-height 1080. Both a
  crop-only input and the actual VA `BuildSps` output return 1280x724 / 362-row
  fields at width 1280, with all 50 token pairs and clean EOS. Native/scaled
  pixels match between these inputs by token/parity; their return order differs.
  The [receipt](tests/fixtures/issue436/card-scaled-bob.json) preserves the
  original collector/software HOLDs and separates these observations from VA
  adapter or presentation acceptance.
- [x] Qualify finite H.264 VA decode-integrated card scaling with explicit visible geometry
  and separate processed output. Trimming 724 to 720 is not
  pixel-equivalent: a same-DIL comparison of retained captures rejects all three
  possible field-row crops (see the existing receipt). The opt-in implementation
  applies SPS cropping before scaling, keeps original reconstruction in the
  firmware DPB and exposes only the small additional output as host pixels.
  Original reference handles reject image/export/VPP access. At `87e0306`,
  native coded-1088/visible-1080 → 720 output, all 100 NV12 pictures across
  two opens, EOS and clean reopen pass; first/last-picture CPU VA Bob also passes
  both parities across two opens. See the existing receipt's
  `va_decode_processing` section. Ordinary FFmpeg/Kodi do not request this
  integrated API; no automatic stock-player or physical-presentation claim.
- [x] Extend integrated scaling to explicit LIVE/prefeed while retaining exact
  small-output identity and rejecting READY remapping. At `e636182`, finite and
  LIVE each pass two opens / 100 exact pictures. Four LIVE input gaps complete
  Bob before explicit close, with unchanged non-IDR continuation and separately
  retired auxiliary output. This is bounded correctness, not steady cadence.
- [x] Return card720 through CPU Bob directly to FHD or through Bob720 then FHD.
  Correct completed-VPP source ownership and field-preserving Weave resizing;
  all 56 selected native FHD outputs and 16 intermediate720 outputs match their
  pixel references. One-pass Bob→FHD avoids the slower second host scaler.
  [Usage and measured limits](filters/vaapi/README.md#returning-the-small-output-as-progressive-video-or-fhd).
- [x] Remove per-pixel division from the scalar host NV12 enlargement path.
  `9affe1f` preserves exact crop/field/chroma pixels and reduces the controlled
  720→FHD kernel from 30.00 to 7.41 ms; affected native return/reopen and legacy
  no-SSE checks pass. This does not change card decode or LIVE completion waits.
- [x] Obtain the selected GStreamer card720→Bob→FHD operator A/V observation:
  speech/lip sync aligned, no persistent problem. Clocked no-drop acceptance
  remains open; the trace locates drops after complete hardware decoding.
  [Scope and limits](HARDWARE-2026-09-13.md#card720--bob--fhd-physical-playback--2026-10-21).
- [ ] Qualify additional stream geometries/parities, client selection and
  sustained LIVE/display cadence separately; four measured LIVE bottom-field
  completion waits still take 153–158 ms. READY remapping is not enabled for
  integrated scaling. Reuse completed benchmarks instead of unrelated gates.
- [x] Reduce the first-field Bob private NV12 view to half height without
  changing the public decode surface. Both-parity pixel/lifetime tests,
  no-SSE controls, CPU cost comparison and a bounded native regression pass
  ([results](HARDWARE-2026-09-13.md#compact-first-field-bob-and-argb-borders--2026-10-20)).
  This host-memory optimization does not resolve the VA card-scaling contract.

## VA-API compatibility backlog (#445 / #436)

Prioritize correctness within the advertised decode/VPP subset, using the
[existing compatibility investigation](https://github.com/ahnhy1324/crystalhd/issues/445#issuecomment-6257714172).
Missing optional VA operations are not all release requirements.

- [x] Extend VA MPEG-2 Main to uniform frame-coded TFF/BFF. At `cf3449f`,
  stock FFmpeg Weave/Bob each pass two fresh processes per field order:
  600 outputs match the unchanged GStreamer field-layout reference, with
  correct PTS steps, EOS and clean shutdown. See the
  [bounded result](HARDWARE-2026-09-13.md#mpeg-2-main-frame-coded-interlace--2026-10-21).
- [x] Extend selected MPEG-2 acceptance to real-capture FHD TFF/BFF intervals.
  1920×1088i25 and 1920×1080i29.97 Weave/Bob produce 1,074 exact outputs across
  eight fresh processes, with EOS and clean close. No codec code change needed;
  this does not qualify damaged whole captures or real-time presentation.
- [x] Admit bounded progressive Main MPEG-2 TFF/RFF without guessing cadence.
  `fc39d09` preserves original FFmpeg repeat/PTS/duration metadata; selected
  720×480 soft-RFF and whole-frame-repeat cases pass six native opens, exact
  pixels and EOS. [Scope](HARDWARE-2026-09-13.md#mpeg-2-progressive-repeat-flags--2026-10-21).
- [x] Remove repeated EOS/reopen from eligible progressive Main MPEG-2 sync.
  `dee7182` preserves exact originals and retires private auxiliary output;
  selected 24-picture cases use one EOS/no restart. The existing 180-picture
  fixture passes synchronous and pipelined clients with measured CPU/throughput.
  [Results and fallback limits](HARDWARE-2026-09-13.md#mpeg-2-synchronous-completion--2026-10-21).
- [x] Admit bounded mixed MPEG-2 Main frame flags in a fixed interlaced sequence.
  Initial interlaced input, equal macroblock-row grids, progressive frame prediction
  and no RFF are required. Selected TFF/BFF full-pixel downloads and fresh-process
  reopen pass; [scope and synchronous-path cost](HARDWARE-2026-09-13.md#mpeg-2-bounded-mixed-frame-pictures--2026-10-24).
- [x] Extend owned MPEG-2 completion to supported interlaced/mixed Main frames.
  `782a534` retires compact auxiliary output only after both complementary
  fields release; lookahead depth follows sequence mode, output layout follows
  each picture. Selected SD/FHD, mixed and progressive downloads preserve
  pixels/timing with one EOS and no internal reopen. A matched 24-picture pair
  reduces wall time 20.65→2.34 s and CPU time 3.06→0.65 s; these are process/download metrics,
  not displayed throughput. [Evidence and limits](HARDWARE-2026-09-13.md#mpeg-2-interlaced-synchronous-completion--2026-10-24).
- [ ] Complete broader MPEG-2 RFF/telecine presentation acceptance and contracts for
  field-coded input, progressive-first transitions and ambiguous mixed row grids
  before advertising them. The bounded mixed synchronous path is not real-time qualified.
  VC-1 interlace and additional MPEG-4 syntax remain separate unsupported
  subsets, not capabilities unlocked by the H.264 scaling change.
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
- [x] P0 — Complete #473's selected exact-installed-RC Kodi Auto/Off, finite
  seek/EOF/reopen and 30-minute card720→Bob presentation acceptance, as recorded
  in the closed release qualification. Broader
  native i386/Atom real-time, FHD cadence and LIVE inputs remain nonblocking
  #472/#429 follow-up. The bounded 10.100-second seek avoids the earlier long-GOP
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
  The proper BGRA output/image path now supports stock-client RGB download;
  the historical ARGB storage alias remains a separate compatibility limitation.
- [ ] P2 — Complete color conversion beyond range handling. `18b626f` fixes
  declared full-range NV12→ARGB and NV12 black padding; default/limited pixels
  are preserved, and unsupported range conversions reject before capture.
  Independent neutral-pixel, cache-transition, queued ownership and both-parity
  Bob controls pass. Direct packed-output `StoreArgb` still uses fixed limited
  BT.601; its range/matrix selection remains separate. Selectable chroma sites,
  primaries and transfer remain unimplemented.
  Ordinary VPP now accepts matching BT.601/BT.709 declarations and applies the
  input matrix for ARGB; same-standard NV12 preserves samples. Independent
  colored-pixel, cached-context and queued/canceled-operation checks cover this
  subset. Standard changes, explicit properties and primaries/transfer/chroma-site
  conversion remain rejected; add them only with independent pixel oracles and
  matching advertised capabilities.
- [x] P2 — Unsupported display attributes return an error; valid empty requests
  remain no-ops. Invalid arguments do not access or change caller attributes.
- [x] P2 — Evaluate `vaQuerySurfaceError`: retain `UNIMPLEMENTED` and a null
  detail pointer. The current generic failure state cannot identify macroblock
  damage or a hardware reset. Ready/failed/destroyed/removed-ID and null-output
  controls preserve this boundary without accessing or changing surface state.

Intentional unsupported scope includes legacy `vaPutSurface` presentation,
subpictures/palettes/display controls, temporal deinterlacing and other unexposed
VPP transforms, encoding, protected playback and codecs outside advertised
profiles. Do not advertise these merely to make an application probe succeed.
Use the [libva API contract](https://github.com/intel/libva/blob/2.23.0/va/va.h)
and [VPP contract](https://github.com/intel/libva/blob/2.23.0/va/va_vpp.h) for
feature-specific acceptance, not a blanket claim of full VA-API conformance.
