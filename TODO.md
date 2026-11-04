<a id="open-work"></a>

# Current work

Updated **2026-11-04**, after stable 1.3 publication. This is the current work queue.
A packaged copy is a snapshot; subsequent completion belongs in the current
repository queue and candidate qualification receipts.
Detailed dated investigations, original failures and superseded branch states
are retained in the [planning archive](docs/history/TODO-2026-11-03.md).
Use the [documentation index](DOCUMENTATION.md) to find usage, contracts and
evidence; update this queue in place rather than appending another status log.

Before work, read the [maintainer rules](AGENTS.md) and
[external-code policy](EXTERNAL-CODE-POLICY.md). Product fixes belong in our
maintained components; isolated diagnostic probes retain their own scope.

## Release and integration

| Scope | Current state |
| --- | --- |
| Stable release | [1.3.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0), frozen source `24a5182b988b` / module-DKMS `3.10.20`, is published with 12 downloaded-and-verified assets. All 17 feature groups/defaults remain; fresh final archive/settings/install/native/delivery, current-base CI and independent review pass with explicitly scoped reuse. See [actual qualification](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0/release-qualification.json) and [release record](RELEASING.md#published-stable-13-qualification). |
| Retained fallback | Published [1.2.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0) / module-DKMS `3.10.13` and its source remain immutable for fallback/rollback. |
| Published RC4 candidate | [1.3.0-rc.4](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.4), frozen source `1c8b0451c631` / module-DKMS `3.10.19`, is published with 12 downloaded-and-verified assets after #654 qualification/integration. Its actual outcomes and scoped reuse remain candidate19 evidence, not final20 results. Preserve its artifacts and registered payload. |
| Published RC3 candidate | [1.3.0-rc.3](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.3), core `36ef4c681f49` / module-DKMS `3.10.18`, is published with 12 assets: complete core, settings ZIP and native V4L2/unmodified FFmpeg companion, including default-off Centered and every integrated opt-in. Current-equipment qualification is complete; see the [actual results](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0-rc.3/release-qualification.json) and [complete scope](RELEASING.md#rc3-complete-scope-and-candidate-preparation). RC4 and final20 promotion retain separate identities and qualification. |
| Frozen published candidate | [1.3.0-rc.2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.2), frozen core 940/module 17 and 13 public assets. Current-equipment qualification and the separately identified [native companion](NATIVE-RUNTIME-DELIVERY.md) are complete. Frozen RC2 does not qualify a later package identity or stable promotion. |
| Development master | Protected [#655 final1.3 integration](https://github.com/ahnhy1324/crystalhd/pull/655) follows RC4/#654. Published final source `24a5182b988b` stays immutable; later documentation/research does not replace its payload. The earlier #652/#653 source600/kernel18 quality results retain scoped reuse rather than new module20 attribution. |

The [published stable claim map](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0/complete-claim-map.json)
binds all 17 feature groups to actual final20 checks or explicitly scoped reuse.
RC4/RC3 receipts preserve their original source/module and failures. The [earlier integration map](tests/fixtures/issue510/v13-complete-claim-map-20261102.json)
retains its historical attribution. Actual RC3 archive, installed/native,
normal Centered and application checks are complete; older source or module13
observations retain only their original scope.
Do not replace published final1.3 or RC4/RC3/RC2 payloads and registered module20/19/18/17 sources with master.

<a id="current-priorities--2026-11-03"></a>
<a id="tracked-work"></a>

## Current priorities

There are 13 open owning issues. The table identifies the next actions;
completed subsets do not close their broader acceptance/research issues.
Their full checklists remain in the linked owners and the archived technical
plans. The agreed release criteria are complete; these remaining research and
support scopes are not additional promotion gates. Nonblocking research continues.

| Order | Owner | Remaining work and existing boundary |
| --- | --- | --- |
| 1, retained risk | [#623 GTK crash](https://github.com/ahnhy1324/crystalhd/issues/623) | The [bounded review](tests/fixtures/issue400/gtk-bounded-review-20261104.json) is complete: retained incident evidence and maintained callback/worker/lifetime boundaries establish no demonstrated own fault or causal repair. Keep #623 open and the original CSS assertion/SIGSEGV unreproduced/unfixed. Existing native/Python capture can supply decisive recurrence evidence; continue the remaining research with the optional GTK risk explicit and no new recurrence gate. |
| 2 | [#513 Xv delivery](https://github.com/ahnhy1324/crystalhd/issues/513) | The [natural FHD50 FIFO/latest pair](tests/fixtures/issue513/natural-fhd-20261104.json) completes the next bounded load point: each decodes 750 originals/renders 1,500 fields with natural EOS and clean resources, but queued/replaced/pending-residence counts remain zero. Representative congestion/latency attribution and broader formats, controls, interruption and endurance stay open; FIFO remains default and no gain is demonstrated. Do not repeat the identical route next; continue the separate #477/#436 decoder skip/latency and EOF attribution below after the completed bounded observations. |
| 3 | [#477 centered output](https://github.com/ahnhy1324/crystalhd/issues/477), [#436 BWDIF](https://github.com/ahnhy1324/crystalhd/issues/436) | The [maintained-cost observer/analysis](tests/fixtures/issue477/maintained-cost-20261104.json) records complete paired CPU/wall scopes, while the actual case and strict native census remain FAIL (689/681 materializations of 900; second-play replay/history gaps). Next add explicit replay-aware historical-binding/disposition observation and ownership controls, then attribute measured pixel/CPU-sync costs without claiming the cause of NONREF policy. Full-population/tail cadence, broader geometry/lifetime, all post-copy pixels, GPU-reader return and physical presentation remain open. |
| 4 | [#538 native picture association](https://github.com/ahnhy1324/crystalhd/issues/538), [#445 colour input](https://github.com/ahnhy1324/crystalhd/issues/445) | Establish a valid independent POC1 multi-slice input before extending association, and an authoritative source-colour contract for direct packed RGB. POC0/2 and selected range transitions are already qualified; crop/chroma, broader syntax, object/GPU-return census and endurance remain open. |
| 5 | [#470 RX allocation](https://github.com/ahnhy1324/crystalhd/issues/470) | Compare sized versus fixed allocation with 16/8 buffers on the current BCM70015 with paced input, then required native-planar, PM and format-change cases. Output-aware allocation is implemented. Preserve the conservative default and DMA quarantine. |
| 6 | [#400 desktop integration](https://github.com/ahnhy1324/crystalhd/issues/400) | Actual Orca/accessibility interaction and specifically named frontend adapters on the available desktop. Shared settings, GTK/tray, queried AT-SPI state and reviewed Korean are complete within their recorded scopes. |
| 7 | [#530 field-GPU output](https://github.com/ahnhy1324/crystalhd/issues/530) | Remaining XID refill, congested writes/X11 calls, EGL/whole-window restart, absolute chroma reconstruction and live/endurance work. Existing setup/resize/handoff deadlines and selected NV12 field/pixel controls are complete. The hardware-GL LSan failure remains recorded. |
| 8, parallel research | [#92 capabilities](https://github.com/ahnhy1324/crystalhd/issues/92), [#204 picture ownership](https://github.com/ahnhy1324/crystalhd/issues/204) | Establish a coherent firmware-owned acquire/hold/publish/complete/release boundary. Standalone scaler/CSC/DNR and hardware deinterlacing follow that contract; headers or host-register guesses are not proof. |
| Follow-up queue | [#91 application acceptance](https://github.com/ahnhy1324/crystalhd/issues/91), [#510 reliability](https://github.com/ahnhy1324/crystalhd/issues/510), [#445 VA compatibility](https://github.com/ahnhy1324/crystalhd/issues/445) | HTTP(S), network seek/recovery, broader streams/frontends and specific remaining reliability contracts. MPEG-2 RFF/telecine, field-coded/progressive-first transitions, additional MPEG-4 syntax and unsupported VPP transformations keep their own acceptance limits. Add narrower CI lanes only when a concrete change needs them. |

<a id="next-centered-bwdif-case"></a>

<a id="centeredbwdif-observation-and-next-attribution"></a>

<a id="packet-admission-and-next-decoder-attribution"></a>

### Maintained-code costs and next replay attribution

The [maintained-cost case](tests/fixtures/issue477/maintained-cost-20261104.json)
adds a private OWN backend, paired wall/thread-CPU scopes and an offline reader.
Its 42,766-event timing ledger is complete. Exact materialized-key correlation
covers 689/681 originals and one field-conversion/canvas pixel span for each;
that bounded subset does not qualify the whole native ledger. Matched parent
scopes can include still-bound replay work and are not a transport budget.

| Native-correlated stage: p50 thread CPU (ms) | Play 1 | Play 2 |
| --- | --- | --- |
| First-field staging | 0.932 | 0.952 |
| Field-conversion pixels | 1.668 | 1.697 |
| Canvas pixels | 1.788 | 1.731 |

These individual scope medians are not an additive playback budget or a proof
of the skip-policy cause. Direct-public execution is observed for the 689/681
correlated originals; public copy/promotion are unobserved in that subset,
while the all-context ledger separately contains 11 unjoined copies and
11 unjoined promotions per play. No global no-extra-copy claim follows. Independent-clock subtraction can make off-CPU unavailable; raw values
are preserved.

The actual `maintained2997-01` result and controls remain **FAIL**, despite both
natural end callbacks, application exit0 and clean module13/runtime36/kernel
restoration. Native materializations are 689/681 of 900. The second play records
691 input sends and 1,382 dequeue events: 10 AU reinputs produce 20 replayed
fields, including four after old full bindings retired. Existing one-input/
two-field census cannot describe that replay history; it must not silently
accept unknown bindings or reclassify the failed record. Raw post-filter
inputs/outputs are689→1,376 and681→1,360, with no filter EOF; fresh selected-
prefix reference was **NOT_RUN** after the census failure. Earlier source-population
and clock-accounting FAILs retain their original outcomes.

Next implement explicit replay/history disposition and ownership controls in
our diagnostic observer/census, preserving the 900-original requirement.
Then use the measured parent/direction scopes to distinguish pixel work from
CPU-access/release costs before proposing an own-code optimization; first-field
staging still needs its lease/ownership proof. These measurements establish no
product defect, harmless replay, normal-backend performance benefit or release
qualification.

#### Retained packet-admission result

The [packet-admission case](tests/fixtures/issue477/packet-admission-20261104.json)
completes the separate metadata observer/reader census alongside the
[method7 post-filter observer/reference](tests/fixtures/issue477/bwdif-postfilter-20261104.json).
Two natural EOFs occur in one original Kodi/App PID; the original providers and
parent-PID/outer-public-call scope stay explicit.

| Earlier packet-only stage | Play 1 | Play 2 |
| --- | --- | --- |
| Reviewed short source packets/software TFF frames | 900 | 900 |
| Selected outer H264/video demux returns | 900 | 900 |
| Codec data send attempts / accepted | 901 / 900 | 900 / 900 |
| VAAPI frame returns; native originals; accepted filter inputs | 677 | 684 |
| Native field dequeues (two per submitted original) | 1,354 | 1,368 |
| Post-filter outputs before graph free | 1,352 | 1,366 |
| Codec nonnull empty-drain attempts / accepted | 9 / 1 | 7 / 1 |
| Codec receive EOF returns | 7 | 5 |
| Post-filter source NULL/close; sink EOF | 0; 0 | 0; 0 |

These are stage populations, not packet-to-frame/VA identity joins. Both
900-original native criteria remain FAIL (223/216 fewer native originals), and
`packet2997-01` exits nonzero as `OBSERVED_SOURCE_QUALIFICATION_FAIL` despite
successful diagnostic observation. Codec drain is observed as nonnull,
data-NULL/size0 packets; it must not be confused with absent post-filter EOF.
Each selected codec/epoch has 900 unique accepted nonempty-packet PTS labels;
returned frame PTS labels form exact integer subsets. All 223/216 absent labels
were accepted with NONREF skip_frame/skip_idct/skip_loop_filter `8/8/8`;
all 332/330 default-state labels appear. This establishes raw label/state
correspondence, not payload/source/VA identity. That packet-only result does not
establish why the original caller selects the skip/latency policy.
Whole-population, full-tail cadence and unselected pixels remain open.

Each play's first four post-filter outputs match its same-input original-App
reference in active NV12 bytes and raw metadata. This selected prefix does not
supply missing client EOF. The earlier `bwdif2997-02` record remains 687/690
originals, 1,374/1,380 native fields and 1,372/1,378 filter outputs, with its
whole-source FAIL preserved. Its separate three-input reference-only flush
produces six outputs versus four unflushed; it never drains Kodi. The initial
case01's 695-original subset, legacy fork gate and first-EOF census-abort FAIL
are unchanged.

The retained [`uniform-18.ts` census](https://github.com/ahnhy1324/crystalhd/issues/477#issuecomment-6606058827)
uses system codec62/filter11: 900 TFF/no-repeat frames, source timebase
`1/90000` and 3003-tick spacing, coded `1920x1088` versus visible `1920x1080`.
Raw original-App packet/frame/sink clocks and private Kodi start offsets remain
separate; no source-clock, pixel or CPU-cause claim follows from these counts.
The long `uniform-180.ts` / 9,000-picture preparation, native field-weave and
unrelated 720-line references remain different inputs/scopes.

<a id="completed-integration-baseline--2026-11-02"></a>
<a id="completed-milestones-and-remaining-acceptance"></a>

## Completed baselines

| Work | Completion and retained evidence |
| --- | --- |
| Stable1.3 and RC4 publication | [#655](https://github.com/ahnhy1324/crystalhd/pull/655) final source24/module20 and [#654](https://github.com/ahnhy1324/crystalhd/pull/654) RC4/source1c8/module19 are separately qualified and published, each with 12 assets downloaded and verified. Final archive/settings binding, DKMS7/direct7/native5, companion2exports/13fileoperations, current-base CI/review and original host restoration pass. [Final qualification](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0/release-qualification.json) retains every feature, fresh-versus-reused scope and original failure/known risk. |
| Post-RC3 affected quality and documented flows | Userspace `6004636` with frozen RC3 kernel18/source36ef: ordinary and explicit own-FIFO Player each pass 30 minutes, 45,000 originals / 90,000 fields, natural EOS and 1,800/1,800 audio-clock matches. Settled RSS grows 1,122/1,168KiB; FD/thread growth is zero. Actual DKMS7/direct7 cases and staged/optional file journeys pass; original module13/runtime36/inventory/kernel state is restored. The initial pre-FIFO cooling-wait FAIL remains preserved and only the missing FIFO arm continued with unchanged media/thermal/resource rules. All 17 RC3 feature groups are covered by fresh evidence or scoped unchanged-component reuse; the three-hour VA/DIL Bob result remains original module16 evidence. [Current #510 summary](https://github.com/ahnhy1324/crystalhd/issues/510) retains the actual scope; these are not RC4/module19 or final20 results. |
| Post-RC3 stability fixes | Player seek/resume grace survives delayed frame observations; Xv rejects replies observed after the original response deadline. Focused regressions retain the original failures and timeout/ownership rules. Strict private-CA HTTPS success and independent CA/hostname rejection are covered by software tests. See [RC4 changes](HISTORY.md#unreleased-stabilization-after-130-rc3); broader transport, X11 and GTK issues remain open. |
| RC3 qualification and publication | [#650](https://github.com/ahnhy1324/crystalhd/pull/650) and the [published qualification](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0-rc.3/release-qualification.json): exact archive/build, actual legacy/native/output, DKMS/direct install and rollback, stock Kodi/normal Centered, 30-minute Player and companion delivery pass. All 12 uploaded assets were downloaded and matched before publication. Original failures, reuse limits and deferred stable promotion remain explicit. |
| Complete core/native/runtime integration | #619–#622 and the [master integration receipt](tests/fixtures/issue510/master-integration-20261102.json); [complete 1.3 claim map](tests/fixtures/issue510/v13-complete-claim-map-20261102.json). The partial historical RC1 is not the complete candidate. |
| Centered Off and seek targets | [Off EOF/reopen](tests/fixtures/issue477/centered-off-20261103.json), [initial controls](tests/fixtures/issue477/centered-controls-20261103.json) and [fresh seek-target 90/91 NV12](tests/fixtures/issue477/centered-seek-target-20261103.json). The initial case retains zero seek-target samples; later samples do not relabel it. |
| Actual returned raw PTS | [#647 receipt](tests/fixtures/issue477/centered-frame-pts-20261103.json): 212 frame/binding joins and four source 90/91 targets, with the two original controller FAILs preserved and separate corrected offline reanalysis. Both declared time bases are unset; source-clock comparison remains unobserved. |
| Progressive Off CPU-copy | [#648 receipt](tests/fixtures/issue477/centered-cpu-copy-20261103.json): 211 raw-frame joins (31 interrupted + 180 natural), four pre-copy NV12 targets and downstream BGRA 12→90; original per-play selection/SSE4 provider, Stop/reopen and normal retirement. The earlier flush-oracle FAIL remains unchanged. This is not all post-copy YUV or BWDIF/GPU/release qualification. |
| Method7/BWDIF diagnostic implementation | [Bounded observation/reference](tests/fixtures/issue477/bwdif-postfilter-20261104.json): separate transparent observer/reader, same-input filter12 reference and two original-App natural EOFs are complete; selected prefixes match. Original case01 FAIL and case02 whole-source `OBSERVED_SOURCE_QUALIFICATION_FAIL` remain failures, not release or lossless-cadence qualification. |
| Packet-admission diagnostic implementation | [Actual two-EOF census](tests/fixtures/issue477/packet-admission-20261104.json): metadata-only public format/codec observer and reader are complete; 900 video demux returns and 900 accepted codec data sends per play, NONREF states and codec drain/EOF are measured. Native originals remain 677/684 of 900; absent raw-PTS labels associate with NONREF state, while its underlying selection cause, payload/VA identity and full-source qualification remain open. |
| Maintained-cost diagnostic implementation | [CPU/wall observation](tests/fixtures/issue477/maintained-cost-20261104.json): private backend/collector/reader and actual pixel/CPU-access/DIL/Sync scopes are complete; 42,766 scalar events and the materialized 689/681-key timing subset pass offline validation. The actual case/controls, strict native census and 900-original criteria remain FAIL; replay-aware history/disposition is the next bounded diagnostic work. |
| Field-window handoff | [#530 receipt](tests/fixtures/issue530/field-window-handoff-20261103.json): shared response deadline and checked geometry/ancestry/completion; scoped actual reopen and retained NV12 regressions. Broader field-GPU work stays in the queue. |
| Xv latest-frame/timing | [Current-card comparison](tests/fixtures/issue513/latest-card-abba-20261103.json), [timing receipt](tests/fixtures/issue513/xv-timing-20261103.json) and the [natural FHD50 load point](tests/fixtures/issue513/natural-fhd-20261104.json). FIFO/latest each complete 750 decoded originals/1,500 rendered fields, natural EOS and clean resources; no source is queued/replaced and no pending residence is observed. The module13/runtime36/kernel boundary is preserved without installation or module changes. This bounded negative result leaves representative congestion and benefit unproved; FIFO stays default. Local request/ACK observations do not measure server/GPU/scanout latency. |
| Native source range/association | [POC0](tests/fixtures/issue538/poc0-multislice-range-20261102.json), [POC2](tests/fixtures/issue538/progressive-multislice-range-20261102.json) and [same-size range staging](tests/fixtures/issue538/same-size-range-staging-20261102.json) retain their exact source/pixel/PTS/ownership scopes and earlier failures. |
| Closed bounded investigations | #607/#539 current-cohort delivery, #429 unavailable-target disposition, #471/#472 bounded measurement/stop decisions and #614 own admission repair are complete. Closure does not turn their historical FAIL/NOT_RUN or broader acceptance into PASS. Details remain in the [archive](docs/history/TODO-2026-11-03.md#tracked-work) and owning issues. |

The retained integrated source checkpoint [#648 CI](https://github.com/ahnhy1324/crystalhd/actions/runs/41649574478)
passes 15 jobs with two intended component skips. Source/ABI CI is separate
from actual-device or release qualification. A later successful run does not
establish the cause of an earlier crash or repair an old failed case.

<a id="mandatory-development-boundary"></a>

## Working boundaries

Read [AGENTS.md](AGENTS.md) and the [external code policy](EXTERNAL-CODE-POLICY.md).
Fix maintained CrystalHD components; do not implement or ship fixes by changing
external applications/dependencies. Isolated probes are allowed and retained,
but modified-client results cannot qualify stock compatibility or a release.
All master changes use task PRs, independent review and the current-base
`CI gate`. Preserve upstream submission branches, particularly
`upstream/01-linux-6.1` for [yeradis/crystalhd#9](https://github.com/yeradis/crystalhd/pull/9);
a local merge does not authorize their deletion or reuse.

Use the current equipment only. Physical Atom/i386, BCM70012, another GPU and
an independently booted OS remain unavailable/NOT_RUN outside this execution
queue. Do not reopen closed #429/#607 for those targets.

Source-qualified no-drift logs satisfy A/V acceptance for the exact route.
Accepted renderer/QoS losses do not create a universal zero-loss gate;
source-picture correctness, ownership and ABI requirements still apply.
Physical cadence/scanout and broader endurance remain separate research.

Only the original #538 shutdown heap-abort investigation is excluded.
**Maintainer-reported reason: agent cybersecurity filtering prevents the work
from proceeding.** Preserve its historical aborts and diagnostics; this is not
proof that it was invalid, fixed or disproved. Other #538 work remains open.
[Scoped disposition](https://github.com/ahnhy1324/crystalhd/issues/538#issuecomment-6486735751).

Choose checks that distinguish the changed behavior and reuse evidence for
unchanged components. Do not add per-action hashes, repeated full suites or
generic manifests merely as proof of progress. Preserve meaningful pixel/
integrity controls and frozen release requirements. Dirty research worktrees
remain probes or historical attempts, not automatically missing product
features. Withdrawn copy/performance prototypes remain withdrawn; preserve an
undisposed old scale-only packing candidate without claiming a measured gain
or reviving it as an active fix.

## Archived planning and technical detail

The [complete pre-audit planning snapshot](docs/history/TODO-2026-11-03.md)
retains the earlier completed checklists, source/package identities, exact
measurements, rejected prototypes and technical acceptance plans. The dated
[capability snapshot](STATUS-2026-10-31.md) is historical; use this file for the
live queue. Older section URLs below remain entry points to their archived
records.

<a id="active-research-status--2026-11-03"></a>

- [Active research status — 2026-11-03](docs/history/TODO-2026-11-03.md#active-research-status--2026-11-03)

<a id="historical-private-runtimepackage-checkpoints--2026-11-01"></a>

- [Historical private runtime/package checkpoints — 2026-11-01](docs/history/TODO-2026-11-03.md#historical-private-runtimepackage-checkpoints--2026-11-01)

<a id="private-native-runtime-follow-up--2026-11-01"></a>

- [Private native-runtime follow-up — 2026-11-01](docs/history/TODO-2026-11-03.md#private-native-runtime-follow-up--2026-11-01)

<a id="stability-follow-up--2026-11-01"></a>

- [Stability follow-up — 2026-11-01](docs/history/TODO-2026-11-03.md#stability-follow-up--2026-11-01)

<a id="interlaced-cadence-and-reduced-output-follow-up-436"></a>

- [Interlaced cadence and reduced-output follow-up (#436)](docs/history/TODO-2026-11-03.md#interlaced-cadence-and-reduced-output-follow-up-436)

<a id="established-baseline"></a>

- [Established baseline](docs/history/TODO-2026-11-03.md#established-baseline)

<a id="ready--asynchronous-delivery-candidate"></a>

- [READY / asynchronous delivery candidate](docs/history/TODO-2026-11-03.md#ready--asynchronous-delivery-candidate)

<a id="card-720-output-deinterlace-then-enlarge"></a>

- [Card 720 output, deinterlace, then enlarge](docs/history/TODO-2026-11-03.md#card-720-output-deinterlace-then-enlarge)

<a id="va-api-compatibility-backlog-445--436"></a>

- [VA-API compatibility backlog (#445 / #436)](docs/history/TODO-2026-11-03.md#va-api-compatibility-backlog-445--436)
