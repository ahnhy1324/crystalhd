<a id="open-work"></a>

# Current work

Updated **2026-11-06**, after stable 1.3 publication. This is the current work queue.
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
| Historical comparison/rollback | Published [1.2.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0) / module-DKMS `3.10.13` and its source remain immutable for explicitly selected comparison/rollback; they are not the current default host baseline. |
| Published RC4 candidate | [1.3.0-rc.4](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.4), frozen source `1c8b0451c631` / module-DKMS `3.10.19`, is published with 12 downloaded-and-verified assets after #654 qualification/integration. Its actual outcomes and scoped reuse remain candidate19 evidence, not final20 results. Preserve its artifacts and registered payload. |
| Published RC3 candidate | [1.3.0-rc.3](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.3), core `36ef4c681f49` / module-DKMS `3.10.18`, is published with 12 assets: complete core, settings ZIP and native V4L2/unmodified FFmpeg companion, including default-off Centered and every integrated opt-in. Current-equipment qualification is complete; see the [actual results](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0-rc.3/release-qualification.json) and [complete scope](RELEASING.md#rc3-complete-scope-and-candidate-preparation). RC4 and final20 promotion retain separate identities and qualification. |
| Frozen published candidate | [1.3.0-rc.2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.2), frozen core 940/module 17 and 13 public assets. Current-equipment qualification and the separately identified [native companion](NATIVE-RUNTIME-DELIVERY.md) are complete. Frozen RC2 does not qualify a later package identity or stable promotion. |
| Development master | [#674](https://github.com/ahnhy1324/crystalhd/pull/674) is merged at `fe0e21f`: the coupled CPU/COW/field/mirror/worker stack passed independent review and the existing current-base CI gate. Earlier post-release [#663](https://github.com/ahnhy1324/crystalhd/pull/663)/[#664](https://github.com/ahnhy1324/crystalhd/pull/664) readback/audit work, [#665](https://github.com/ahnhy1324/crystalhd/pull/665) TX/parser/private-small changes, [#666](https://github.com/ahnhy1324/crystalhd/pull/666) zero-wait/module21, [#667](https://github.com/ahnhy1324/crystalhd/pull/667)/[#668](https://github.com/ahnhy1324/crystalhd/pull/668) exact-Sync lookahead, [#669](https://github.com/ahnhy1324/crystalhd/pull/669) retained host-RX/module22 and [#672](https://github.com/ahnhy1324/crystalhd/pull/672) auxiliary control notification remain integrated. The separate finite-Sync correction `06f6da9` has distinguishing CPU checks on that base and still requires its own normal PR/CI integration. Pre-first `1b7166e` and adaptive pacing `48ca434` remain unpromoted. Published source `24a5182b988b` stays immutable; no new stable release is claimed. |
| Installed host baseline | **Verified official1.3/source24a/module20**, retained after normal installation and installed Player/controls checks. DKMS20 is installed for kernels7.0.0-38 and7.0.0-42; actual playback uses38. VERSION1.3.0 and the complete installed runtime receipt agree, full before/after boundary is equal and kernel checking passes. The [post-release receipt](tests/fixtures/issue477/postrelease-source900-20261105.json) records the new baseline; the former13/dev.1 host and temporary20/research3fd comparison remain historical. |

The [published stable claim map](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0/complete-claim-map.json)
binds all 17 feature groups to actual final20 checks or explicitly scoped reuse.
RC4/RC3 receipts preserve their original source/module and failures. The [earlier integration map](tests/fixtures/issue510/v13-complete-claim-map-20261102.json)
retains its historical attribution. Actual RC3 archive, installed/native,
normal Centered and application checks are complete; older source or module13
observations retain only their original scope.
Do not replace published final1.3 or RC4/RC3/RC2 payloads and registered module20/19/18/17 sources with master.

Use the [current-source/baseline rule](AGENTS.md#current-source-and-installed-baseline):
new fixes start from latest maintained master, and persistent installation uses
matching current official stable components. Preserve dated module13 restores
as historical facts. The verified official installation now has its own full
baseline receipt; future experiments restore that baseline, never implicitly13.

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
| 3 | [#477 centered output](https://github.com/ahnhy1324/crystalhd/issues/477), [#436 BWDIF](https://github.com/ahnhy1324/crystalhd/issues/436) | The full CPU-canvas stack is merged through #674. Its latest source-qualified physical evidence remains `16bbc04`, **893/895: Source900 FAIL**, with four scoped audits, controls, safety and full official20 restoration PASS. The [finite-Sync correction](tests/fixtures/issue477/sync-deadline-20261106.json) fixes a separately reproduced CPU wait bug; it adds no fresh hardware900 claim. Frozen adaptive-pacing `48ca434` returns 891/896 and stays unpromoted without demonstrated benefit. Preserve the earlier research6a 900/900 case and the [full-port receipt](tests/fixtures/issue477/centered-cpu-canvas-20261106.json); continue existing-data shortage/selection attribution and broader pixels, clocks/cadence, GPU return, lifetime/endurance and matched-benefit work. Both umbrella issues stay open. |
| 4 | [#538 native picture association](https://github.com/ahnhy1324/crystalhd/issues/538), [#445 colour input](https://github.com/ahnhy1324/crystalhd/issues/445) | Establish a valid independent POC1 multi-slice input before extending association, and an authoritative source-colour contract for direct packed RGB. POC0/2 and selected range transitions are already qualified; crop/chroma, broader syntax, object/GPU-return census and endurance remain open. |
| 5 | [#470 RX allocation](https://github.com/ahnhy1324/crystalhd/issues/470) | Checked output-aware allocation and its compile-time opt-in already exist. The current matched-baseline SIZED16 arm completes864/854 originals and still fails900; sequential counts do not prove a performance gain or justify changing the default. Remaining paced16/8 A/B, native-planar, PM and format-change acceptance keep their original scopes; do not duplicate the allocator. Preserve DMA quarantine and the coupled defer-allocation/DONTNEED scope. |
| 6 | [#400 desktop integration](https://github.com/ahnhy1324/crystalhd/issues/400) | Actual Orca/accessibility interaction and specifically named frontend adapters on the available desktop. Shared settings, GTK/tray, queried AT-SPI state and reviewed Korean are complete within their recorded scopes. |
| 7 | [#530 field-GPU output](https://github.com/ahnhy1324/crystalhd/issues/530) | Remaining XID refill, congested writes/X11 calls, EGL/whole-window restart, absolute chroma reconstruction and live/endurance work. Existing setup/resize/handoff deadlines and selected NV12 field/pixel controls are complete. The hardware-GL LSan failure remains recorded. |
| 8, parallel research | [#92 capabilities](https://github.com/ahnhy1324/crystalhd/issues/92), [#204 picture ownership](https://github.com/ahnhy1324/crystalhd/issues/204) | Establish a coherent firmware-owned acquire/hold/publish/complete/release boundary. Standalone scaler/CSC/DNR and hardware deinterlacing follow that contract; headers or host-register guesses are not proof. |
| Follow-up queue | [#91 application acceptance](https://github.com/ahnhy1324/crystalhd/issues/91), [#510 reliability](https://github.com/ahnhy1324/crystalhd/issues/510), [#445 VA compatibility](https://github.com/ahnhy1324/crystalhd/issues/445) | HTTP(S), network seek/recovery, broader streams/frontends and specific remaining reliability contracts. MPEG-2 RFF/telecine, field-coded/progressive-first transitions, additional MPEG-4 syntax and unsupported VPP transformations keep their own acceptance limits. Add narrower CI lanes only when a concrete change needs them. |

<a id="next-centered-bwdif-case"></a>

<a id="centeredbwdif-observation-and-next-attribution"></a>

<a id="packet-admission-and-next-decoder-attribution"></a>

### Source 900 investigation and remaining real-time boundary

**The full CPU-canvas stack is merged; Source900 still fails.**
[#674](https://github.com/ahnhy1324/crystalhd/pull/674) integrated the qualified research
CPU storage/COW image, fused field conversion, border reuse, mirrored/deferred
Small, source-prefetch and bounded worker changes into the maintained branch,
alongside the private field/relative-POC/10ms pacing fix. It preserves the two
additional current field-mask and complementary-picture mismatch oracles.
The [current receipt](tests/fixtures/issue477/centered-cpu-canvas-20261106.json)
binds physical results to `16bbc04`; the merged source reuses that qualification
and the later portable-offset check rather than claiming a new physical run.

The separate [finite-Sync correction](tests/fixtures/issue477/sync-deadline-20261106.json)
is implemented as `06f6da9` on merged master `fe0e21f`. A genuine complement queued
after a real 510ms wait used to be consumed by an already-expired 500ms caller,
which returned success. The fixed caller returns TIMEOUT without extra RX,
input or public materialization; a fresh Sync completes that same original and
checks its pixels. Literal-old O0 fails after finite cleanup; fixed O0 focused/full
and fresh strict O2 ordinary/private builds/full replay pass. The unchanged
scaling/VPP/buffer/COW/observer suites are reused explicitly. This is a bounded
own wait-correctness fix awaiting its normal PR/CI integration, with no adaptive
pacing, infinite-wait change or fresh Source900 result.

At source `16bbc04`, fresh strict O2 ordinary/private builds, full replay, VPP, buffer handles and
all four observer contract checks pass. Full scaling passes **52 groups /
21,094,879 checks**, with zero COW groups unexercised; real-kernel COW controls
pass **172,102 checks**. A separate native32 process passes its COW controls on
the current 64-bit host; this does not qualify unavailable i386 playback hardware.
The DIL was rebuilt as a normal private-build dependency, with its log retained;
no fresh DIL test suite is claimed.

Fresh prefix75 playback preserves every visible NV12 byte and raw frame
metadata against the reused source2d baseline. Two actual private fields release
successfully with masks `1→3` and pending debt `1→0`; the complete bounded FMT18
census is zero, versus two in that retained baseline. The 63 following originals
include P/B pictures, IDRs at packet ordinals26/51 and actual EOS. This is a
cross-source regression, not an independent decoder golden, timing comparison
or general zero-format-change promise.

| Latest separate natural-play observation, default fixed DIL / temporary22 | First play | Reopen | Disposition |
| --- | ---: | ---: | --- |
| Full maintained task candidate `16bbc04` | 893 | 895 | **Source900 FAIL**; 1,786/1,790 genuine original fields/releases; strict ledgers/four scoped audits/controls PASS; zero replay; App0/kernel0; errors empty; maximum68C; AC throughout; full boundary equality; no handoff; full official20 restore PASS |

Product code matches the earlier research `6a293ff` passing case below, so its
900/900 observation has **not become repeatable900 qualification**. The new run
keeps the original Kodi/App, 900-picture input, discard policy and own diagnostic
observers. Serial populations establish neither causal speedup nor the shortage's
cause. [#674](https://github.com/ahnhy1324/crystalhd/pull/674) merged after independent
review and all 17 current-base CI jobs completed: 15 success and two intended
skips, with the existing gate passing. The portable `off_t` correction has fresh
strict native32 syntax and full native64 VPP checks; it adds no playback claim.
This evidence does not qualify a new stable release.
Centered remains default-off; no external
product patch or default change is included. Published 1.3 remains frozen at
source24a/module20; temporary22 experiments restore that complete installation.

[#477](https://github.com/ahnhy1324/crystalhd/issues/477) and
[#436](https://github.com/ahnhy1324/crystalhd/issues/436) stay **open** for their
broader acceptance: independent full-field/chroma and post-copy/filter pixels,
source clocks/cadence, matched CPU/QoS benefit, GPU return and lifetime/endurance,
broader inputs, and the original filter-EOF tail. Source-qualified no-drift logs
satisfy A/V for their recorded route; accepted renderer/QoS loss is not a new
universal zero-loss release gate. Continue the main-only lateness/selection
attribution without inventing an external defect or global impossibility.
All 13 owners retain their genuine remaining scopes.

<details>
<summary>Unpromoted pre-first and adaptive-pacing experiments</summary>

Pre-first `1b7166e` remains unpromoted: its physical component run did not
exercise accepted pre-first auxiliary input. The later held-first adaptive
experiment `48ca434` did exercise its route in a fresh prefix75 comparison;
all 75 visible NV12 pictures and raw metadata match. That adoption/pixel result
does not establish a latency or throughput benefit.

Its separate original-Kodi case returns **891/896: Source900 FAIL**, with
1,782/1,792 original fields/releases, four scoped audits/controls and safety
PASS, zero replay and full official20 restoration. The candidate is frozen
without demonstrated benefit. Its 40ms/1ms pacing, retry hints and extra
qualification markers are excluded from the finite-Sync correction above.
Retain the original component comparison and natural-play failures; do not
repeat identical runs or infer an external defect from these serial counts.

</details>

<details>
<summary>Earlier narrow maintained247 and same-product research6a observations</summary>

The [narrow field/pacing receipt](tests/fixtures/issue477/auxiliary-field-pacing-20261106.json)
is immutable. Candidate `247bfc8` contained only the private field/relative-POC/
10ms correction, before the CPU/COW/worker port. Its literal-old O0 control fails
the intended first-private-field oracle after actual release and finite cleanup;
fixed O0/full replay, strict O2 builds/full replay and fresh prefix75 pass,
including 13 faults, signed POC bounds and unchanged legacy bytes.

| Earlier natural-play observation, default fixed DIL / temporary22 | First play | Reopen | Disposition |
| --- | ---: | ---: | --- |
| Narrow maintained `247bfc8` | 832 | 834 | Source900 FAIL; 1,664/1,668 validated original fields/releases; four scoped audits/controls and safety PASS; zero replay; full official20 restore PASS |
| Research `6a293ff`, same product code as the later full port | 900 | 900 | Scoped Source900 PASS for this case; 1,800 fields/releases each; 130/128 private auxiliary pairs; four scoped audits/controls and safety PASS; zero replay; maximum66C; full official20 restore PASS |

The source6a subtask passed for those two recorded natural plays. The later
same-product full-port result above prevents a repeatability claim; neither
case establishes causal speedup, stock compatibility or a release. Preserve
both passing and failing raw records. Broader umbrella acceptance stays open.

</details>

<details>
<summary>Earlier source2d renderer and source1fa scheduler/wake checkpoints</summary>

These frozen observations precede the field/POC/pacing work above. They retain
their original failures and limits; no ordinary source2d result is claimed.
The latest separate ordinary predecessor remains
[source0331](tests/fixtures/issue477/source900-deferred-20261105.json), 894/893
with native-only scope.

| Earlier natural-play observation, default fixed DIL / temporary22 | First play | Reopen | Disposition |
| --- | ---: | ---: | --- |
| Source2d, research stack plus pending-auxiliary control wake | 892 | 891 | Source900 FAIL; 1,784/1,782 validated original fields/releases; strict ledgers/four audits/controls PASS; zero replay; App0/kernel0; maximum68C; AC present throughout; full boundary equality; no handoff; full official20 restore PASS |

The **separate renderer diagnostic** on source2d returns **893/888**, with
four scoped audits, controls, App0/kernel0, full boundary equality, no handoff
and full official20 restoration PASS. Each play has 900 unique
Calc/GetStats/return → non-DRAIN control → accepted public-send execution-order
links, plus 11 independent drain controls. Original VERYLATE decisions are
38/25 and DROP_ANY requests are 34/24. Every linked DROP_ANY send enters with
runtime `skip_frame=8` (NONREF); other linked sends enter default0.
These are observed policy requests, **not 34/24 actual discarded pictures**.
Gain, allowDrop, message-queue state and other private state remain unobserved;
renderer feedback does not identify the cause or prove an external bug.
No media-PTS/native-picture identity or release/performance qualification is
inferred from this diagnostic.

The maintained **#672 control-wake correction is merged**. A real worker with
empty eventfds reproduces the old CV-only notification failure when an accepted
H.264 auxiliary leaves its delimiter pending on BUSY/low TX space. Conditional
control-event notification passes the four TFF/BFF × BUSY/space O0 controls,
strict O2 production/replay builds and full replay, followed by independent
review and the current-base CI gate: 17 completed jobs, 15 success and two
intended skips. The accepted-delimiter quiet path and existing ownership/EOS
rules remain intact. Fresh scaling/VPP/library reruns are not claimed.
This proves a bounded own notification fix; it does not establish the source900
cause or qualify a main-only playback result from the combined research stack.

The [retained research and CPU wake-fix record](tests/fixtures/issue477/auxiliary-control-wake-20261106.json)
records source1fa and the distinguishing maintained-code controls. It remains
that checkpoint; the source2d natural-play and renderer observations above are
separate evidence in the owning issues. Unobserved EndPicture bodies and other
threads remain part of the causal boundary.

| Earlier natural-play observation, default fixed DIL / temporary22 | First play | Reopen | Disposition |
| --- | ---: | ---: | --- |
| Source1fa, bounded worker pairing after actual auxiliary output release | 889 | 898 | Source900 FAIL; 1,778/1,796 validated original fields/releases; strict ledgers/four audits/controls PASS; zero replay; App0/kernel0; maximum65C; no handoff; full official20 restore PASS |

The separate source1fa scheduler/syscall diagnostic returns **865/881**, with
four scoped audits, controls, retirement and full official20 restoration passing.
Its capture overhead prevents comparison with natural-play counts. In the
first play, 819 exact same-worker field gaps total 13.351s; 786 have fully known
scheduler partitions and 33 retain unknown segments. Disjoint intersections
contain 10.8374s blocked inside matched poll attempts, 0.7472s blocked inside
futex attempts, 0.7426s runnable and 0.8441s unknown. Successful wakeup and actual
switch-in bound runnable time; a waking attempt alone does not. These observations
do not establish that TX backpressure caused the waits or the 900 shortage.

</details>

<details>
<summary>Source0331 predecessor: deferred Small and source-prefetch qualification</summary>

The [source0331 checkpoint](tests/fixtures/issue477/source900-deferred-20261105.json)
records the earlier 896/890 private and 894/893 ordinary observations, with
maintained master3e as their original base. It remains valid within that scope;
the ordinary native-only result does not qualify a source1fa or source2d
ordinary run.

The deferred representation omits duplicate Small stores only while actual
public ROI bytes, field parity, allocation epoch and original ownership are
proved. Genuine independent Small pixels are required before consumers,
writers, exports or detachment need them. Source67c passes 52 scaling groups /
21,094,879 checks, full replay/VPP, buffer handles, observer controls and
independent review. Source0331 adds only seven bounded source-prefetch lines;
its ordinary/private builds and 800 same-function pair controls run fresh,
while the unchanged structural/ownership checks above are explicitly reused.
The full 52-group suite was not rerun on source0331. All 800 pairs preserve every
Small/public oracle byte. Independent selected pixel captures remain eager;
deferred-compose adoption does not qualify all unselected pixels.

The source0331 private natural-play case has 1,792/1,780 validated original fields
and successful releases. The separate ordinary case has 1,788/1,786 native
dequeue records and both strict native ledgers pass, with no private probes.
These ordinary records precede field validation and do not qualify parity,
distinct field pixels or individual DIL releases.
Both complete controls, App0/kernel0, full boundary equality, no handoff and
normal full official20 restoration, with a maximum of 68C and AC present
throughout.
Ordinary pixel, packet, cost, replay-disposition and postfilter audits remain
UNOBSERVED; successful native census does not turn them into passing audits.

For exact materialized keys, first-field staging including its converter,
the complementary converter and final Compose have mean combined thread CPU
of 1.580/1.565ms per original on source0331 versus the earlier source67c
1.779/1.753ms. These are disjoint measured work spans from different original
populations, not a paired speedup or elapsed playback budget. In the separate
eight-arm / 800-pair same-function comparison, warm thread CPU is
1.201→1.048ms (about 12.7% lower); CPUID-guarded source-only CLFLUSH
controls are 1.127→1.006ms (about 10.7% lower). Setup and private
materialization demand are outside the timer, and the controls do not model
natural DMA cache state or the interval between physical fields. Neither
comparison establishes net lifetime savings, end-to-end benefit or the skip
cause; the actual 900 requirement still fails.

The separate 49Hz source67c profile returns 887/858 and remains diagnostic only:
3,725 samples, 31 empty stacks and zero reported lost samples, with userspace stacks but
no per-play time window or kernel attribution. It is excluded from the natural
case table. Sampled own converter loads motivated the bounded source0331
prefetch comparison; these samples alone do not establish cache misses.
The earlier worker change offers only 3 immediate gaps below 0.5ms among 1,699
same-worker pairs. Read-only frequency observation finds about 93.5–94.8%
upper-Pstate residency and median 2.095GHz; it supplies no basis to change the
governor merely to obtain a passing count.

These predecessor cases do not establish the 900 shortage's underlying cause
or global impossibility. The later scheduler observation and maintained wakeup
correction are scoped separately above. The returned-
frame probe remains NOT_EXERCISED with its original wrapper failure preserved.
For this predecessor, full source900, unselected post-copy/filter pixels,
source clocks/cadence, GPU return, presentation and broader lifetime/endurance
remain open. The original
client's missing filter EOF is a scoped known limitation, not proof of the
900 shortage's cause or global impossibility. These research scopes do not add
release gates or change defaults; all 13 owning issues retain their remaining
work. Earlier failures and ordinary/private qualification boundaries remain
in the records below.

</details>

<details>
<summary>Earlier post-release cases and maintained component checkpoints</summary>

The [post-release receipt](tests/fixtures/issue477/postrelease-source900-20261105.json)
and [earlier receipt](tests/fixtures/issue477/native900-20261104.json) preserve
their original source, failures and no-clock limits. These older observations
do not replace the latest separate cases above.

| Earlier matched-baseline arm | First play | Reopen | Disposition |
| --- | ---: | ---: | --- |
| Source67c, deferred private Small | 887 | 899 | Source900 FAIL; strict ledgers/four audits/controls PASS; zero replay; maximum69C; full official20 restore PASS |
| Source8fa, bounded released-first-field worker pair | 891 | 893 | Source900 FAIL; strict ledgers/four audits/controls PASS; zero replay; maximum66C; full official20 restore PASS |
| Same8fa, additional read-only 1Hz frequency observation | 894 | 888 | Source900 FAIL; same scoped checks PASS; zero replay; maximum64C; no governor/bounds/priority changes; full official20 restore PASS |
| Research e4d full candidate | 837 | 830 | Source900 FAIL |
| Same e4d with existing SIZED16 DIL opt-in | 864 | 854 | Source900 FAIL; second play also has10 classified replay inputs, not extra originals |
| Same original App/e4d/SIZED16 under49Hz all-thread perf | 876 | 878 | Diagnostic profile only; Source900 FAIL, no speedup comparison |
| Research9d with bounded TX-before-RX, actual fixed DIL | 881 | 877 | Source900 FAIL; replay not exercised |
| Research8fac with parser/private-small reuse, actual fixed DIL | 897 | 894 | Source900 FAIL; replay not exercised |
| Same8fac ordinary backend, native trace only | Unqualified; raw submits876 | Unqualified; raw submits886 | Both strict ledgers FAIL; no replay/unknown-row waiver |
| Minimal maintained #665, ordinary/default DIL | 828 | 819 | Balanced native ledgers; Source900 FAIL |
| Fullc6b, confirmed SIZED16 + temporary reviewed module21 | 870 | 875 | Strict ledgers/four audits PASS; Source900 FAIL; normal official20 restoration PASS |
| Samec6b/module21, separately frozen default/fixed DIL | 892 | 860 | Source900 FAIL; second play has20 completed replay inputs, not extra originals |
| Full research + narrow full-Sync lookahead, default DIL/module21 | 879 | 861 | Source900 FAIL; strict ledgers/four audits PASS; no replay; maximum73C and normal official20 restoration PASS |
| Full research + early exact-Sync empty-READY lookahead, default DIL/module21 | 869 | 888 | Source900 FAIL; strict ledgers/four audits PASS; no replay; sampled74C, separate post-retirement75C; official20 restore PASS |
| Full research + retained host RX registrations, default DIL/module22 | 870 | 890 | Source900 FAIL; strict ledgers/four audits/controls PASS; no replay; maximum71C; normal full official20 restore PASS |
| Same full50c/default DIL/module22, ordinary native-only backend | 891 | 881 | Both strict native ledgers PASS; Source900 FAIL; no private observers; maximum70C; normal full official20 restore PASS |

The source67c private case has 1,774/1,798 validated original fields/releases and
885/897 deferred-compose observations. Its two selected independent
Small/composed/public captures per play remain eager, so adoption does not
qualify those unselected pixels. Its earlier warm parent-to67c ABBA comparison
has about 1.709→1.189ms timed pair CPU with private demand excluded. This is a
different comparison from the final source67c-to0331 eight-arm control above; neither
is an end-to-end playback speedup.

Private cases retain their scoped packet/postfilter/cost/replay audits, controls,
App0/kernel0 and full official-baseline equality. The retained8fac ordinary arm has no private
audit or pixel proof: first-play input/dequeue multiplicity and second-play
incomplete identities fail its ledgers despite normal EOF/control cleanup.

The minimal maintained branch contains only a bounded H264/Flea/Centered input
pump before the unchanged RX64 harvest, equivalent bounded Annex-B prefix search,
and clear-elision on exact-size retired tight privateSmall bytes after device
confirmation. Fresh, padded, generic/public/image and compact-Bob initialization
remain unchanged. Its tests use maintained packed first fields; COW, NV12
first-field staging, border reuse and mirror stores from full8fac are excluded.
Default-DIL/ordinary-VA strict builds, main packed-path scaling39groups/190,061
checks, full replay and private-backing controls pass. The unchanged parser's
complete library check and26,003-case native/scalar32/PentiumII results are
explicitly reused; no unchanged-library rerun is inferred. Task-PR/current-base
CI passes and #665 is merged; **these results do not establish a900 fix**.
The separate #666 repair uses negotiated ready-only RX only for Flea wait0.
Actual empty-input controls return in38µs/14µs instead of the legacy approximately2s;
positive waits and LINK EOS keep legacy behavior. The deliberate positive-timeout
control retains its expected kernel-check finding; it is not clean-playback proof.
Full library/UAPI/compat/ownership checks and independent review pass; PR #666
is merged. The full research21/SIZED16 playback still
fails870/875 despite four scoped audits/control/kernel0/cleanup passing;4/10
completed replay inputs are separate. The exact official20 installation is restored
with the original full guard. The actual shortage, decoder skip/latency,
full post-copy pixels/clocks, GPU return and endurance remain open. Preserve the
original missing-filter-EOF and no-clock limits without claiming impossibility.

The merged **full-Sync lookahead (#667)** reuses the existing non-reference
intra auxiliary for a genuine missing complementary field on the exact live
public Centered owner after the unchanged 100 ms inactivity grace; it does not
fabricate Bob demand, publish auxiliary pixels or remove actual-EOS debt.
An OWN/default-discard diagnostic actually exercises one auxiliary, continues
63 originals on the same channel including P/B pictures before IDR, and matches
all 75 visible1920x1080 NV12 images/public metadata against its no-aux parent.
That bounded firmware differential excludes coded1088-only padding and is not
a normative golden, stock cadence or a900 fix. The corrected CPU controls keep
the real coded1088 config while admitting public1080/1088; the earlier manually
mutated cfg1080 failure is retained as synthetic evidence, not a stock-activation
cause. The new original Kodi two-EOF case still fails879/861, despite controls,
four audits, App0/kernel0, no replay and full official20 restoration passing;
its peak is73C and the caller still sends no filter EOF. PR667 merged after
current-base CI41972643790 passed15 jobs/two intended skips and the CI gate.

The merged early exact-Sync repair (#668) uses only an existing successful empty
READY-queue sample with no acquired activity; first-progress, acquired/FMT/
duplicate, error or zero-attempt turns cannot authorize early auxiliary, and
Bob/EOS retain100ms.
Its separate75-AU diagnostic actually exercises auxiliary at10.13ms, completes
the selected Sync in89.68ms, and preserves all75 visible NV12 images/metadata
and subsequent P/B continuation against the retained no-aux parent. Baseline
reuse establishes bounded pixel safety, not speed causality. The original Kodi
two-play result still fails869/888 with no replay despite controls, strict
ledgers/four audits, App0/kernel0, full boundary equality and no handoff.
Controller samples peak at74C; root separately observed75C after retirement,
outside that sampling window, then passed the normal guard and restored exact
official20. The old100ms mutation builds and fails the expected early-path
assertion. Continue the shortage/skip investigation; preserve the returned-frame
NOT_EXERCISED/wrapper failure, all original900 failures and the remaining
filter-EOF, pixel/clock, GPU-return and endurance boundaries.

The isolated kernel22/DIL development feature retains host RX registrations
across fetch/repost to avoid repeated pinning and DMA mapping. Its
[development API contract](linux_lib/libcrystalhd/README.md#development-registration-reuse)
preserves copy/NoCopy leases, legacy fallback when capability is absent, exact
opaque identities and positive retirement before backing release. Host buffer
registration is separate from #470 sizing and #204 firmware picture ownership.

The bounded actual client registers16 buffers, performs152 total exact
borrow/reposts and positively retires all16 owner-wide, with no legacy RX ioctl.
All75 original visible NV12 images/raw metadata and subsequent P/B/two-IDR
continuation match the retained no-aux parent. Its different source/DIL/module21
cohort provides pixel regression safety, not a paired performance estimate.

Original Kodi source900 remains FAIL: the full50c private case returns870/890
with no replay and all four scoped audits PASS; its ordinary native-only case
returns891/881 originals with1782/1762 dequeued field records. Both pass
controls, App0/kernel0/full boundary/no-handoff and normal full official20
restoration, with maxima71C/70C respectively. The ordinary case has no private
preload or pixel/packet/cost/replay/postfilter probe; its pixels, replay
disposition and clock/cadence remain unqualified. The shortage persists without
private observers, while serial counts do not estimate causal overhead. The
compact portable receipt above is the retained private22 checkpoint; the later
ordinary result/restoration are indexed in the [owning issue](https://github.com/ahnhy1324/crystalhd/issues/477).
Development #669 uses the required independent review and existing current-base
CI gate. These results do not establish a900 fix, complete #470 acceptance or
prove global impossibility. Published1.3/module20 and original failures remain
unchanged.

</details>

<details>
<summary>Retained pre-current field/COW/mirror checkpoints and historical host comparisons</summary>

The following source-qualified records retain their original checkpoint state;
the current matched-baseline observations and integration queue above supersede
their then-pending plans. Their coupled CPU/COW/field/mirror implementation is
now integrated through #674; these individual historical results retain their
original scope. The separate pre-first and adaptive-pacing candidates above
remain unpromoted.

| Separate source/arm | First play | Reopen | Disposition |
| --- | ---: | ---: | --- |
| Merged #663 readback/retired-image reuse | 804 | 805 | Source900 FAIL |
| Ordinary #663, native trace only | 798 | 821 | Source900 FAIL; no private preloads |
| CPU-primary full canvas | 803 | 806 | Source900 FAIL |
| CPU-primary + immutable-small readback | 778 | 817 | Source900 FAIL |
| Private NV12 first-field staging | 774 | 803 | Source900 FAIL |
| Memfd COW, without field staging | 824 | 853 | Source900 FAIL |
| COW + field staging + black-border reuse | 840 | 860 | Source900 FAIL |
| Dual-store fields + final ROI-copy omission, private3fd | 882 | 844 | Source900 FAIL |
| Same3fd ordinary, module13 | Unqualified; raw submits826 | 883 | First ledger incomplete; second FAIL900 |
| Same3fd ordinary, temporary official legacy20 | 881 | Unqualified; raw submits852 | First FAIL900; second ledger multiplicity FAIL |

The dual-store case joins881/882 and843/844 actual native materializations to
both successful field stores and validated final ROI-copy omission; one first
original per play uses the original staged-copy fallback. Existing payload
captures independently match full composed/public NV12 for two actually
mirrored originals, one per play. The two COW API samples instead cover the
**same first fallback original**; they do not qualify mirrored-alias bytes or
all frames. Preserve that gap for a future justified case, without resampling
solely to obtain different samples. Private Small, early Bob, CPU/DMA ownership,
COW and complete real pixels before READY/M remain required.

All four private audits, ordinary controls, App0/kernel0 and the existing
terminal ownership checks pass in that private case, maximum73C. Its cost-v2
ledger is complete at78,176/131,072 events, dropped0; retained v1 stays capped
65,536. Physical replay is observed separately:20 completed replay inputs and
40 Duplicate fields in the second play are not additional originals.

Ordinary module13 has full raw SubmitBind/M sets826/883, but four partial
first-play dequeue records leave that entire strict ledger unqualified.
Temporary20 has raw sets881/852; eight second-play bindings have duplicated
input/dequeue multiplicity. Neither unknown rows nor duplicates are guessed
to be replay or waived. The completed temporary20 comparison restored13;
that journal is a historical result, **not the future default baseline**.
Its unchanged3fd userspace was a research mount, not the official24a runtime.

- [x] Merged readback/cache and isolated field/COW/fusion implementations,
  meaningful ownership/byte/failure controls and bounded route observations.
- [x] Real COW semantics on native64/native32; unsupported emulator advice
  falls back rather than receiving a false PASS. No physical Atom claim.
- [x] Complete the latest maintained-source and separate prototype audits,
  reviewed source repairs and targeted checks, preserving defaults and original
  failures. Candidate scaling/buffer/VPP/replay suites pass; scalar32 native
  checks pass and PentiumII emulation retains explicit unsupported-COW fallback.
- [x] Establish and retain the matched official1.3/source24a/module20 installation:
  installed Player/controls pass with full boundary equality, ref/pin0 and
  kernel0. Kernel42 has build/install coverage only. The previous13/dev.1 host
  remains historical and the new SIZED setup remains held/unexecuted.
- [ ] Whole900, underlying NONREF/latency cause, all post-copy/filter pixels,
  source clocks/cadence, GPU return, presentation and broader endurance.

The earlier first-field attempt that hit75C stays FAIL with observed810 and
an interrupted second play; the later successful control flow did not fix it.
READ-prefault had no useful copy-batch benefit and is not adopted. Original
App FFmpeg is canonical9.0.2 plus supplier patches; the12cc SDK is a separate
compatible-header fork. The fixed caller's absent filter EOF is a confirmed
narrow tail boundary, separate from source900 and no proof of global
impossibility. Stable1.3 payloads and every17-feature/default contract remain
unchanged; this research does not replace their qualification.

</details>

<details>
<summary>Retained804/805 and earlier comparison details, with original sources and limits</summary>

These recorded next-step statements belong to their earlier checkpoints.
The completed implementation and current queue above supersede those plans;
original measurements and failures below are preserved.

The [current receipt](tests/fixtures/issue477/native900-20261104.json) separates
actual candidate failures from the no-clock counterfactual. All use the retained
900-picture input, original App providers and maintained VA/DIL. Stock
diagnostic arms preserve the external applications, discard policy and 900
criterion. The separate no-clock OWN client explicitly uses DEFAULT discard.

| Current case | First play | Reopen | Disposition |
| --- | ---: | ---: | --- |
| Default stock diagnostic | 690 | 698 | Source 900 FAIL |
| Existing eligible i915 WC requested | 680 | 598 | Source 900 FAIL; default remains off |
| H264 frontend harvest 64 → 1 candidate | 580 | 679 | Source 900 FAIL; candidate withdrawn from integration, branch retained |
| Immutable centered CPU-image readback | 779 | 778 | Readback cost reduced; source 900 FAIL |
| CPU-image readback plus existing WC option | 734 | 720 | Source 900 FAIL; WC stays off |
| CPU-image readback plus Derive-only retired-buffer reuse | 804 | 805 | Further bounded improvement; source 900 FAIL |
| Separate OWN no-clock/default-discard client | 900 | — | 900 actual VA returns/downloads and 900 native originals; not stock playback |

WC activation is proved by every matched canvas's flags80 acquisition/completion
and successful WC/GTT domain children, with no WRITE fallback and READ leases
retained. Canvas WRITE-END median calling CPU falls from 1.927/1.932ms to
0.021/0.021ms; the enclosing direct-public CPU also falls. This neither repairs
source 900 nor measures total readback/BWDIF/presentation cost. DeriveImage's
independent snapshot allocation/copy is outside that enclosing scope.

The first default and WC plays each naturally exercise 10 completed replay
inputs / 20 Duplicate fields and 20 successful releases, including two fields
after old bindings retire. Their native ledgers balance without rematerializing
replay or inventing live tuples. This completes that physical observation;
earlier [#662 evidence](tests/fixtures/issue477/replay-disposition-20261104.json)
keeps its original 690/694, 11,072-event ledger and NOT_EXERCISED result.

The no-clock client uses original format 63 / codec 63 / util 61, DEFAULT discard,
900 distinct equal accepted/returned raw-label sets and explicit decoder drain.
Its independent native ledger records 900 submissions/inputs/materializations,
1,800 validated/released fields and 900 binding ends. Its 22.010 s wall and 17.257 s
main-thread CPU do not qualify stock real-time playback or total-process CPU.
A post-exit wrapper error lost the saved numeric exit and individual mapping
records: retain nulls and live stdout confirmation, with the separate follow-up
kernel 0 / equal-boundary result. No successful rerun replaces that original record.

The pinned, unmodified original Kodi BWDIF caller never supplies filter source EOF before
freeing its graph. Its held final picture requires that EOF, so correct VA
pixel output alone cannot repair the missing two filter outputs. This narrow
[known limitation](USAGE.md#known-issues-and-deinterlacing-roadmap) is separate
from source 900 loss under NONREF. It does not prove all OWN latency fixes
impossible. Next distinguish readback/filter critical paths with the original
providers; keep full post-copy pixels, GPU-reader return, presentation and
broader lifetime/cadence qualification open.

The no-clock BWDIF extension accepts all 900 originals, produces 1,798 outputs
before filter EOF and 1,800 after an explicitly **OWN reference-only** EOF.
This confirms the separate tail boundary; it does not fix stock playback.
The isolated derived-only snapshot cache also fails stock source 900 (699/583); a
generic zero-preserving cache and row-fenced SSE readback show no useful
no-clock improvement. These candidates remain isolated and are not promoted.

Source attribution is corrected: the App manifest supplies canonical FFmpeg
9.0.2 (`946fcce`) plus two distribution patches; the retained `12cc1f4` source
is an RPi SDK fork. The used public ABI/header closure is compatible, so actual
binary observations remain valid. The original-provider, same-configuration
CPU graph reports NV12 → YUV420P → BWDIF → NV12 with automatic conversions.
This withdraws the earlier source inference of native NV12 BWDIF; it is not a
live Kodi graph dump or proof that these conversions cause source loss.
Actual stock sampling confirms `vaDeriveImage` canvas readback and hardware
Intel 965GM rendering. The maintained implementation now reconstructs an
independent full NV12 image from proven immutable centered pixels. It retains
public READ leases, validates the current binding/geometry/write epochs and
falls back after writes, retirement or export. Derive-only reuse retains one
fully retired byte vector (size and capacity at most 8,355,840 bytes); generic
CreateImage keeps fresh zero initialization and all snapshot bytes are written
before publication. Source review, 39 scaling groups / 188,686 checks, buffer
handles and 115 VPP regressions pass for the combined source.

In the first readback case, the interval between positive public READ methods
falls from 6.95/6.98ms to 1.68/1.68ms median calling CPU; the methods themselves
remain about 0.019ms. This includes copy/dispatch/bookkeeping, not pure copy or
whole AddPicture time. Both first API snapshots match all 3,133,440 public
NV12 bytes but represent the **same** original, not two distinct pictures or
all images. The combined case returns 804/805 originals and has clean native,
selected-pixel and shutdown audits. Missing NONREF labels continue into the
last 300 packets; they are not merely startup loss. A separate 49Hz whole-App
profile is diagnostic, not a speed comparison or an impossibility proof.
Continue the specific worker-notification handoff review without changing
discard policy, surface semantics or the 900 criterion.


</details>

<a id="maintained-code-costs-and-next-replay-attribution"></a>

#### Retained maintained-cost result and direction review

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
11 unjoined promotions per play. No global no-extra-copy claim follows.
Independent-clock subtraction can make off-CPU unavailable; raw values are preserved.

The actual `maintained2997-01` result and controls remain **FAIL**, despite both
natural end callbacks, application exit0 and clean module13/runtime36/kernel
restoration. Native materializations are 689/681 of 900. The second play records
691 input sends and 1,382 dequeue events: 10 AU reinputs produce 20 replayed
fields, including four after old full bindings retired. That capture's
original one-input/two-field census could not describe that replay history;
new observation must not silently accept unknown bindings or reclassify the
failed record. Raw post-filter
inputs/outputs are689→1,376 and681→1,360, with no filter EOF; fresh selected-
prefix reference was **NOT_RUN** after the census failure. Earlier source-population
and clock-accounting FAILs retain their original outcomes.

A separate offline parent/direction review of this same `8cdbc62` ledger
locates median canvas WRITE CPU-access END thread CPU at 1.971/1.928 ms for its
689/681-key subset. It narrows the observed branch, not pure GPU waiting or the
underlying NONREF cause. These are retained measurements, not new `42ab9ac`
performance results. First-field staging still needs its lease/ownership proof;
no product defect, harmless replay, normal-backend speedup or release result is
established. The new implementation does not relabel this original FAIL.

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
| Maintained-cost diagnostic implementation | [CPU/wall observation](tests/fixtures/issue477/maintained-cost-20261104.json): private backend/collector/reader and actual pixel/CPU-access/DIL/Sync scopes are complete; 42,766 scalar events and the materialized 689/681-key timing subset pass offline validation. Its original actual case/controls, strict native census and 900-original criteria remain FAIL; later replay-aware implementation does not rewrite that receipt. |
| Replay-disposition diagnostic implementation | [Actual observation and CPU path controls](tests/fixtures/issue477/replay-disposition-20261104.json): exact native sequence links, completed-input/restart state, actual Inspect/pair/Observe/release and offline historical provenance are implemented. Two-EOF diagnostic controls and fresh selected-prefix references pass; physical replay is NOT_EXERCISED and original populations690/694 of900 remain FAIL. |
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
features. Separately rejected copy/performance controls remain withdrawn; preserve an
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

The verified official1.3 baseline is tracked in [release and integration](#release-and-integration).
The [previous established baseline](docs/history/TODO-2026-11-03.md#established-baseline)
is an immutable historical record, not a restoration instruction for new work.

<a id="ready--asynchronous-delivery-candidate"></a>

- [READY / asynchronous delivery candidate](docs/history/TODO-2026-11-03.md#ready--asynchronous-delivery-candidate)

<a id="card-720-output-deinterlace-then-enlarge"></a>

- [Card 720 output, deinterlace, then enlarge](docs/history/TODO-2026-11-03.md#card-720-output-deinterlace-then-enlarge)

<a id="va-api-compatibility-backlog-445--436"></a>

- [VA-API compatibility backlog (#445 / #436)](docs/history/TODO-2026-11-03.md#va-api-compatibility-backlog-445--436)
