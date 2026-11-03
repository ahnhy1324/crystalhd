<a id="open-work"></a>

# Current work

Updated **2026-11-04**. This is the live work queue and completion summary.
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
| Stable release | [1.2.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0), module/DKMS `3.10.13`, is published. Earlier releases and RCs remain immutable; see [release history](HISTORY.md). |
| Complete integration candidate | [1.3.0-rc.2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.2), frozen core 940/module 17 and 13 public assets. Current-equipment qualification and the separately identified [native companion](NATIVE-RUNTIME-DELIVERY.md) are complete. **Stable 1.3 remains deferred.** |
| Development master | Later research and diagnostics are integrated, including [#647 raw-frame observation](https://github.com/ahnhy1324/crystalhd/pull/647) and [#648 progressive Off CPU-copy](https://github.com/ahnhy1324/crystalhd/pull/648). Master is not a replacement RC2 payload or a new release. |

The [complete 1.3 claim map](tests/fixtures/issue510/v13-complete-claim-map-20261102.json)
binds the integrated feature groups to their qualification and evidence reuse.
When release work resumes, identify a new candidate/module revision for changed
payloads, package the complete intended scope and qualify the affected paths.
Do not replace frozen RC2 assets or registered module 17 source with master.

<a id="current-priorities--2026-11-03"></a>
<a id="tracked-work"></a>

## Current priorities

There are 13 open owning issues. The table identifies the next actions;
completed subsets do not close their broader acceptance/research issues.
Their full checklists remain in the linked owners and the archived technical
plans. Nonblocking research continues alongside release work.

| Order | Owner | Remaining work and existing boundary |
| --- | --- | --- |
| 1, parallel diagnosis | [#623 GTK crash](https://github.com/ahnhy1324/crystalhd/issues/623) | Capture a useful failing native/Python stack and establish the original CSS assertion/SIGSEGV cause. The capture helper is implemented; the original crash is still unreproduced and unfixed. Continue other work instead of looping clean tests. |
| 2 | [#513 Xv delivery](https://github.com/ahnhy1324/crystalhd/issues/513) | Exercise representative congestion/pending residence and attribute latency, then broader formats, controls, interruption and endurance. Timing/latest-frame support is implemented; current-card runs showed no pending replacement or demonstrated gain. FIFO remains the default. |
| 3 | [#477 centered output](https://github.com/ahnhy1324/crystalhd/issues/477), [#436 BWDIF](https://github.com/ahnhy1324/crystalhd/issues/436) | Add the separate 29.97 fps/method 7 original-Kodi case and post-filter observation/reference. Off VA observations and progressive CPU-copy controls are complete within their private diagnostic scopes. Broader geometry/lifetime, all post-copy pixels, GPU-reader return and physical presentation remain separate. |
| 4 | [#538 native picture association](https://github.com/ahnhy1324/crystalhd/issues/538), [#445 colour input](https://github.com/ahnhy1324/crystalhd/issues/445) | Establish a valid independent POC1 multi-slice input before extending association, and an authoritative source-colour contract for direct packed RGB. POC0/2 and selected range transitions are already qualified; crop/chroma, broader syntax, object/GPU-return census and endurance remain open. |
| 5 | [#470 RX allocation](https://github.com/ahnhy1324/crystalhd/issues/470) | Compare sized versus fixed allocation with 16/8 buffers on the current BCM70015 with paced input, then required native-planar, PM and format-change cases. Output-aware allocation is implemented. Preserve the conservative default and DMA quarantine. |
| 6 | [#400 desktop integration](https://github.com/ahnhy1324/crystalhd/issues/400) | Actual Orca/accessibility interaction and specifically named frontend adapters on the available desktop. Shared settings, GTK/tray, queried AT-SPI state and reviewed Korean are complete within their recorded scopes. |
| 7 | [#530 field-GPU output](https://github.com/ahnhy1324/crystalhd/issues/530) | Remaining XID refill, congested writes/X11 calls, EGL/whole-window restart, absolute chroma reconstruction and live/endurance work. Existing setup/resize/handoff deadlines and selected NV12 field/pixel controls are complete. The hardware-GL LSan failure remains recorded. |
| 8, parallel research | [#92 capabilities](https://github.com/ahnhy1324/crystalhd/issues/92), [#204 picture ownership](https://github.com/ahnhy1324/crystalhd/issues/204) | Establish a coherent firmware-owned acquire/hold/publish/complete/release boundary. Standalone scaler/CSC/DNR and hardware deinterlacing follow that contract; headers or host-register guesses are not proof. |
| Follow-up queue | [#91 application acceptance](https://github.com/ahnhy1324/crystalhd/issues/91), [#510 reliability](https://github.com/ahnhy1324/crystalhd/issues/510), [#445 VA compatibility](https://github.com/ahnhy1324/crystalhd/issues/445) | HTTP(S), network seek/recovery, broader streams/frontends and specific remaining reliability contracts. MPEG-2 RFF/telecine, field-coded/progressive-first transitions, additional MPEG-4 syntax and unsupported VPP transformations keep their own acceptance limits. Add narrower CI lanes only when a concrete change needs them. |

### Next centered/BWDIF case

The retained short 29.97 fps input has 900 H.264 access units/video packets; its
existing packet preflight is not a full decoded-frame vector. Preserve a
separate short-input packet/frame metadata record, add an original method 7 /
actual `bwdif` EOF/reopen preset, and observe post-filter pixels/cadence before
claiming that output. The existing receive-frame probe observes decoder
returns, not BWDIF outputs.

Do not adopt the long `uniform-180.ts` / 9,000-picture `preparation.json` as the
short `uniform-18.ts` manifest. Native 1,800 field-dequeue records, 900 AVCodec
returns and 1,800 BWDIF outputs are different stages. The retained field-weave
reference is not a software BWDIF-output reference; unrelated 720-line source references
cannot substitute. See [#477's preparation record](https://github.com/ahnhy1324/crystalhd/issues/477#issuecomment-6606058827).

<a id="completed-integration-baseline--2026-11-02"></a>
<a id="completed-milestones-and-remaining-acceptance"></a>

## Completed baselines

| Work | Completion and retained evidence |
| --- | --- |
| Complete core/native/runtime integration | #619–#622 and the [master integration receipt](tests/fixtures/issue510/master-integration-20261102.json); [complete 1.3 claim map](tests/fixtures/issue510/v13-complete-claim-map-20261102.json). The partial historical RC1 is not the complete candidate. |
| Centered Off and seek targets | [Off EOF/reopen](tests/fixtures/issue477/centered-off-20261103.json), [initial controls](tests/fixtures/issue477/centered-controls-20261103.json) and [fresh seek-target 90/91 NV12](tests/fixtures/issue477/centered-seek-target-20261103.json). The initial case retains zero seek-target samples; later samples do not relabel it. |
| Actual returned raw PTS | [#647 receipt](tests/fixtures/issue477/centered-frame-pts-20261103.json): 212 frame/binding joins and four source 90/91 targets, with the two original controller FAILs preserved and separate corrected offline reanalysis. Both declared time bases are unset; source-clock comparison remains unobserved. |
| Progressive Off CPU-copy | [#648 receipt](tests/fixtures/issue477/centered-cpu-copy-20261103.json): 211 raw-frame joins (31 interrupted + 180 natural), four pre-copy NV12 targets and downstream BGRA 12→90; original per-play selection/SSE4 provider, Stop/reopen and normal retirement. The earlier flush-oracle FAIL remains unchanged. This is not all post-copy YUV or BWDIF/GPU/release qualification. |
| Field-window handoff | [#530 receipt](tests/fixtures/issue530/field-window-handoff-20261103.json): shared response deadline and checked geometry/ancestry/completion; scoped actual reopen and retained NV12 regressions. Broader field-GPU work stays in the queue. |
| Xv latest-frame/timing | [Current-card comparison](tests/fixtures/issue513/latest-card-abba-20261103.json) and [timing receipt](tests/fixtures/issue513/xv-timing-20261103.json). These are local request/ACK observations, not proof of congestion benefit or server/GPU/scanout latency. |
| Native source range/association | [POC0](tests/fixtures/issue538/poc0-multislice-range-20261102.json), [POC2](tests/fixtures/issue538/progressive-multislice-range-20261102.json) and [same-size range staging](tests/fixtures/issue538/same-size-range-staging-20261102.json) retain their exact source/pixel/PTS/ownership scopes and earlier failures. |
| Closed bounded investigations | #607/#539 current-cohort delivery, #429 unavailable-target disposition, #471/#472 bounded measurement/stop decisions and #614 own admission repair are complete. Closure does not turn their historical FAIL/NOT_RUN or broader acceptance into PASS. Details remain in the [archive](docs/history/TODO-2026-11-03.md#tracked-work) and owning issues. |

The last integrated source checkpoint [#648 CI](https://github.com/ahnhy1324/crystalhd/actions/runs/41649574478)
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
