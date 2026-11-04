# Private centered NV12 payload observation

This test-only backend observes maintained CrystalHD code under an unmodified
stock client. It does not replace the ordinary backend or qualify a release.
The existing noncentered Bob observer and its scope remain unchanged.

`CRYSTALHD_VAAPI_TEST_CENTERED_PAYLOAD` is enabled only by a separate wrapper.
Its private output variable is `CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR`. Ordinary
build, install and runtime have no new state, copies or environment reader.

Read [payload publication](#observation-and-publication) and
[manifest formats](#manifest-v1) for the byte/binding contract. The separate
[returned-frame PTS observer](#actual-returned-avframe-pts-isolated-observation)
and [progressive Off CPU-copy route](#progressive-off-cpu-copy-route) add
diagnostic observations; they do not expand the base payload protocol into
post-copy pixel, BWDIF or GPU/scanout qualification. Current receipts are
indexed by the [frontend acceptance targets](../FRONTEND-ACCEPTANCE.md).
The separate [method7/BWDIF case](#method7bwdif-post-filter-observation)
adds measured post-copy/filter metadata and a bounded same-input pixel replay.

## Observation and publication

One driver reserves three decoder `(context,generation)` lifetimes. The default
mode takes the first two distinct eligible source bindings per lifetime. Selection is fixed:
failed selected snapshots are not replaced by later samples. Scope is limited
NV12, 1280x720 small picture at `(320,180)` in a 1920-wide canvas, explicit
1920x1080 visible geometry, and physical canvas heights 1080 or 1088. Compact
field canvases, noncentered/full-range and unrelated bindings are rejected.

Composition copies small and composed active rows while the original small
read and canvas write leases are held, after production writes and before END.
They stay tentative until both existing ENDs succeed. A direct public
composition also captures its public bytes at that point. A private composed
picture must subsequently match the source storage serial/epoch and binding
of the ordinary decoded-public copy. That copy captures the final public
canvas under its existing read/write leases; both ENDs must succeed too.
No additional DMA access acquisition, wait, release, pixel write, worker I/O
or scheduling policy is introduced by observation.

The original successful public `materialized` binding must match before a
slot is eligible. Successful consuming decoder-context retirement or a
successful decoder-generation reset can qualify a complete two-slot stream.
A guard declared before the original Driver lock
publishes independent arrays after normal lock release. Each sealed stream
publishes once, so one natural EOF can be inspected before another play in
the same process. A live Driver destructor is not required. No Surface,
allocation, device or client ownership is retained by the independent arrays.
Rejected/incomplete retirement produces a bounded status record, not a
qualifying manifest. Exclusive file creation preserves old/partial outputs.

### Fixed seek-target observation

The explicit private value
`CRYSTALHD_TEST_CENTERED_PAYLOAD_SELECT=case2-90-91` selects the prepared fixture's
content90 and91 instead. State reads this value once at initialization; unset
keeps the default selection and ordinary v1 record shapes. Target-only manifest
and status records identify `selection: "case2-90-91"`.

The target selector runs after production composition while the original
small-read/canvas-write leases are still held. It checks only the two fixed
case2 codewords, including CRC, complement and both replicas, at the declared
3x3 luma patches. Slot0 belongs to content90 and slot1 to91; unrelated content
cannot fill a missing target. Once selected, a failed END or mismatched public
copy permanently fails that slot. No worker allocation/I/O, new DMA lease,
dynamic target range or application priority change is introduced.

Target recognition cannot identify a failed acquisition that returned before
the content could be read. This mode observes its eligible target snapshots,
not every decode/access attempt. It does not expand normal-backend or release
qualification. Default pre-BEGIN selection retains its original failed-slot
semantics.

The independent content reader must decode exactly90/91 from each actual
small/composed/public payload and retain all existing full-NV12 gates. For the
interrupted play, each target's matching submission, materialization and
successful sync must fall after the recorded seek request and before requested
Stop. VA submission ordinals are synthetic identities, not content90/91.

The reset hook records the old generation before `DecodeContext::Reset()`
advances it. Its private receipt carries that decoder's actual close return,
whether its device is closed, and the derived per-decoder VA result. It does
not use the aggregate multi-decoder call result or infer success from the
ordinary `context_reset` event's default outcome. The ordinary event continues
to name the new generation. Failed or malformed first reset attempts seal a
rejected stream; later cleanup cannot relabel the original attempt as PASS.
Fixed guards publish any selected old streams after the existing lock releases,
without worker I/O, extra CPU leases or retained Surface/device ownership.

## Manifest v1

The writer emits `centered-payload-PID-INSTANCE-CONTEXT-GENERATION.json` and
six independently packed NV12 blobs, three for each selected binding.

```json
{
  "schema": "crystalhd-centered-payload-v1",
  "pid": 100,
  "driver_instance": 200,
  "slots_required": 2,
  "media_pts_observed": false,
  "publication": {
    "kind": "selected-decoder-context-retirement",
    "context": 3,
    "generation": 1,
    "status": 0,
    "after_driver_unlock": true,
    "all_slots_frozen": true
  },
  "slots": []
}
```

Ordinary retirement retains this v1 format. A reset uses
`crystalhd-centered-payload-v2` with the same payload fields and this publication:

```json
{
  "kind": "selected-decoder-generation-reset",
  "context": 3,
  "generation": 1,
  "next_generation": 2,
  "status": 0,
  "close_status": 0,
  "device_closed": true,
  "after_driver_unlock": true,
  "all_slots_frozen": true
}
```

The reader requires exactly one ordinary `context_reset` for this context and
new generation, the explicit successful private close receipt, and old binding
completion/sync before its termination. New-generation work can follow in the
same frozen trace prefix. No `context_destroy` is invented for a reset.

Each slot carries:

- `binding`: nonzero `context`, `generation`, synthetic `token`,
  `decode_identity`, `submission_ordinal`, public `surface` and `owner`.
- `roi: [320,180,1280,720]`, `visible: [1920,1080]`,
  `limited_range: true`, `direct_public` Boolean.
- True `compose_copied`, `compose_committed`, `public_copied`,
  `public_committed`, and `materialized` flags.
- `small`, `composed`, `copy_source`, `public_canvas` geometry objects.
  Geometry uses the existing diagnostic names: `surface_serial`,
  `storage_epoch`, `width`, `height`, `pitch`, `plane_address`, `fourcc`.
  Existing extra allocation metadata is allowed. `copy_source` must identify
  the composed storage view; on the direct route all three canvas descriptors
  describe the same view. Public and private canvas pitches/addresses can differ.
- `small_file`, `composed_file`, `public_file` basenames and respective
  `small_bytes`, `composed_bytes`, `public_bytes` lengths.

Packed Y rows precede UV rows; each plane independently omits pitch tails.
Small length is 1280*720*3/2; canvas length is 1920*physical_height*3/2.
The independent reader compares every active Y and UV component of each
canvas with the small source at the independently computed ROI. All other
active canvas bytes must be limited black Y16/UV128, including actual coded
bottom rows when 1088 are present. It does not invent a pitch-padding oracle.

The reader joins unique `submit_bind`, successful public `materialized` and
`sync_exit` observations, and the successful matching `context_destroy`, in
trace-v4 for the same pid/driver instance. Bindings may not cross decoder
lifetimes or a `binding_end`. It reports matching exports separately;
neither an export nor these private bytes proves GPU-reader return or scanout.

`token` is the existing VA driver's synthetic identity, not actual media PTS.
Source ffprobe PTS/counts are only a separate source census. Actual
packet/AVFrame PTS-to-VA binding is NOT_OBSERVED by this protocol. The pinned
stock case must independently record real hardware decoding and actual
deinterlace `none`; no uniform physical parity is inferred from stream TFF.

## Explicit current-host execution

`make -C filters/vaapi check-centered-observer` runs device-free production
composition/copy controls and the independent reader's negative controls.
`make -C filters/vaapi centered-observer` builds only the isolated backend
and its ordinary OWN library dependency. Neither target installs components.

`vaapi-centered-payload-stock.py` reuses the existing pinned original Kodi22
RC1 namespace/shutdown helper on the current host. Prepare a fresh plan from a
reviewed, committed clean OWN checkout with `--prepare-plan`, `--own-source`,
`--own-revision`, an independent `--source-probe` packet/frame JSON and
`--runtime-baseline` pointing to the approved absolute regular baseline JSON.
The current schema1 baseline has status
`PASS_VERIFIED_OFFICIAL_STABLE_BASELINE`, bundle1.3.0/source24a, module20,
the full idle host snapshot and the matching installed runtime receipt.
The current host's verified file is
`/home/mache/.cache/crystalhd-postrelease-audit-run-20261105/official-stable-baseline.json`;
the convenience discovery symlink is not accepted. Preparation freezes the
file and its DATA; execution rejects missing or changed baseline data and
does not allow a baseline override. The current guard verifies the full
approved host boundary and installed component cohort before and after media;
it does not invoke the older helper's fixed13 guard or reinstall an old stack.
Historical module13/runtime36 results below retain their original baselines.

Execute the prepared plan explicitly with `--execute`. Both modes require `--plan` and matching
fresh `--case`/`--case-root`; `--cycles 2` at preparation selects same-process
natural EOF/reopen. The original 30-second H264 fixture has 750 source pictures.
That EOF runner does not request a seek or Stop.

The separate `vaapi-centered-controls-stock.py` scenario uses the prepared
six-second progressive FHD barcode input for one forward seek, requested Stop
and same-process natural reopen. It reuses the pinned original stock namespace
and owned shutdown helpers. `vaapi-centered-controls-reader.py` maps actual
observed 3x3 patches to the unchanged public barcode/CRC decoder. The sparse
proxy is not a reconstructed image or a full-frame BGRA pixel comparison.
Its decoded ordinal selects the prepared fixture PTS table; it does not observe
packet/AVFrame PTS at the VA handoff. Small/composed/public payload samples
retain the full NV12 gate and are then joined to that content ordinal.

If a seek keeps the same decoder generation, it need not produce new payload
slots. Its post-seek BGRA barcode checkpoint proves selected content separately
from the retained full-NV12 sample scope. Interrupted first playback does not
have a whole-population completion requirement. The natural progressive reopen
has180 source originals and one dequeue record per original, in contrast to the
MBAFF fixture's two field records below. Successful public materialization and
renderer delivery remain distinct. Runtime observations can establish support
at the selected card-output patch positions; because both proposed height
models share those centers, that result does not disambiguate the full scaler
mapping. Prior software scaler controls alone establish no card behavior.

The private profile explicitly selects method0 and prefers VA rendering. The
runner requires the observed `ff-h264-vaapi`/`none` labels, mapped original
App/FFmpeg and selected OWN backend, hardware FD, natural EOF and exact source
input/output binding census. A successful public materialization count remains
separate from renderer delivery. At each EOF it requires ordinary decoder
retirement before independently reading that context's two-slot manifest.
It preserves a result for failed cases and a live-resource handoff if bounded
ordinary Quit/verified TERM cannot retire media. It never kills media, changes
the loaded module, resets the card or changes scheduling priority.

For this pinned MBAFF fixture, a source picture produces two
`output_dequeued` records. The census requires 750 distinct originals, one
submit/input each and exactly two output records each, in that order; it does
not classify the second field as a duplicate picture. The dequeue event occurs
before field validation, so its default outcome alone is not proof of a valid
field pair. Matching later public materializations are recorded separately,
and the payload reader requires successful materialization for its selected
bindings. These records do not independently measure physical field parity.

## Method7/BWDIF post-filter observation

Select `--scenario bwdif2997` only while preparing a fresh
`vaapi-centered-payload-stock.py` plan. The default remains `off750`.
This separate case freezes the original short `uniform-18.ts`, its reviewed
900-packet/900-software-frame metadata, original App/Platform providers,
method7, CPU-copy preference and the isolated post-filter artifacts. Execution
requires actual `ff-h264-vaapi`/`bwdif`, saved renderer preference false,
original SSE4/filter mappings and two same-PID natural EOFs with decoder
retirement. Its native census requires900 original bindings and1800 field
records per play. Those are distinct from measured post-filter inputs/outputs.
A balanced measured subset keeps the complete-source criterion FAIL. The
BWDIF scenario may finish both diagnostic plays without lowering that count;
it reports `OBSERVED_SOURCE_QUALIFICATION_FAIL` and exits nonzero rather than
promoting a subset into a whole-source PASS. Native ownership/field structure
must still pass before the second play can begin.

Build only the explicit diagnostic targets using the original unchanged
filter12.1.102/util61.1.102 public headers:

```sh
make -C filters/vaapi centered-observer postfilter-observer FFMPEG63_INCLUDE=/absolute/original/include
make -C filters/vaapi check-postfilter-observer
```

On the retained original-App/Platform cohort with the pinned namespace helpers
available, run from the committed clean OWN source checkout. Use new plan/case
paths and the reviewed **short** packet/frame JSON; execution reads its scenario
from the frozen plan, so `--scenario` belongs only to preparation:

```sh
python3 -B tests/vaapi-centered-payload-stock.py --prepare-plan \
  --scenario bwdif2997 --cycles 2 \
  --own-source "$PWD" --own-revision "$(git rev-parse HEAD)" \
  --source-probe /absolute/reviewed-short-packets-frames.json \
  --runtime-baseline /absolute/verified-official-stable-baseline.json \
  --plan /absolute/new-bwdif2997.plan.json \
  --case-root /absolute/existing-case-parent --case bwdif2997-new
python3 -B tests/vaapi-centered-payload-stock.py --execute \
  --plan /absolute/new-bwdif2997.plan.json \
  --case-root /absolute/existing-case-parent --case bwdif2997-new
```

For a separate packet-admission investigation, build its explicit artifacts:

```sh
make -C filters/vaapi packet-admission-observer FFMPEG63_INCLUDE=/absolute/original/include
make -C filters/vaapi check-packet-admission-observer
```

Add `--observe-packets` to **preparation only**, with fresh plan/case paths;
execution reads the frozen boolean from its new plan. It composes the
metadata probe with the post-filter observer in a single preload value and
verifies both probes and the unchanged original format63/codec63/util61 images
at each held identity. Ordinary Off and BWDIF plans leave this mode disabled.

The [packet observation protocol](vaapi-packet-admission-protocol.md) separates
public demux reads, accepted codec sends, retries, entry skip state, successful
frame returns and original drain/EOF. Select the source by its observed public
open URL and stream metadata; never select a population because its count is
near 900. The TS playback format may legitimately omit find_stream_info;
`info_complete:false` does not exclude its outer reads. Nested discovery calls
and empty/NULL packets remain separate. Packet, frame,
context and source clocks are raw distinct domains; this does not observe
private Kodi start offsets or a packet-to-VA binding. Aggregate counts under a
skip policy alone do not identify every discarded packet or its underlying
latency cause. A separate exact raw-label set comparison may establish label/state
correspondence inside a codec/epoch without supplying payload or source identity.
The exact 900-original and native ownership requirements remain intact.

For maintained-code timing, build the separate private backend and its CPU
collector/reader/pixel controls:

```sh
make -C filters/vaapi maintained-cost-observer
make -C filters/vaapi check-maintained-cost-observer
```

Add **both** `--observe-packets --observe-cost` to preparation of a fresh
`bwdif2997` plan. `--observe-cost` requires packet observation; neither option is
an execution-time override. A cost-only plan selects
`maintained-cost-observer/crystalhd_drv_video.so`, with existing original App
packet/post-filter probes and provider guards. Off, ordinary BWDIF and
packet-only plans retain their existing backend. This artifact, its timing
state and its runtime marker are excluded from ordinary/default/package paths.
The [maintained-cost protocol](vaapi-maintained-cost-protocol.md) specifies
bounded same-thread parent IDs, paired raw wall/thread-CPU endpoints, actual
DIL/pixel/CPU-access branches and late-resolved scalar identity. Publication is
after normal retirement; no locks, leases, copies, scheduling or EOF are changed.
Wall-minus-CPU includes waits/preemption, and independent-clock subtraction may
be unavailable; raw values are not clamped. A Sync body may release its mutex,
so it is not an uninterrupted mutex-holder measurement. These diagnostic costs
do not by themselves establish the cause of Kodi's skip policy or a performance
fix; exact native ownership and the unchanged 900-original criterion remain.

For replay-disposition diagnosis, use the combined private backend and its
separate collector/reader/actual-field-path CPU controls:

```sh
make -C filters/vaapi replay-disposition-observer
make -C filters/vaapi check-replay-disposition-observer
```

Add **all three** `--observe-packets --observe-cost --observe-replay` to
preparation of a fresh `bwdif2997` plan. Replay requires the packet, maintained
cost and existing method7/BWDIF observations. Execution reads the frozen mode
from that plan; it cannot add the flag later. Only this plan selects
`replay-disposition-observer/crystalhd_drv_video.so`. The combined backend,
scalar ledger and runtime marker remain excluded from normal/default/package
paths; cost-only, packet-only and old scenarios retain their existing artifacts.

The [replay-disposition protocol](vaapi-replay-disposition-protocol.md) links
actual retained-prefix restart phases, completed-before-input state, each
original dequeue, existing Inspect/pair/Observe decisions and its actual DIL
release to exact native sequence IDs. During playback the replay census is
`PENDING_REPLAY_CLASSIFICATION`, never PASS. Its complete parent-PID ledger is
published at retirement; final classification runs after normal Quit and media
retirement, with overflow, missing receipts, malformed pairs and failed releases
remaining failures. No additional media run is requested merely to obtain replay.
A run without completed replay is recorded as NOT_EXERCISED.

Historical provenance is built offline from original submit_bind/binding_end
records. It does not grant a current public VA lease or fill zero live fields.
The wire's live_* tuple is a raw native observation, and a pending original can
retain its full immutable decoded-picture tuple after public backing reuse;
that tuple alone does not prove current public ownership. Observed materialized
rows retain the existing subset/max1 rule, original New-pair provenance and
sequence checks. Duplicate replay cannot cause another materialization. Missing
materialization stays visible without a new all-M gate or pixel/public claim.
The unchanged 900-original submission/input/New-field requirement is separate
from successfully classified extra replay input/field records. A balanced
smaller diagnostic population remains source-unqualified.

The ordinary build/install/package does not include these artifacts. The
[observer wire protocol](vaapi-postfilter-protocol.md) defines the bounded
per-PID graph lifetime/API ledger and first3-input/first4-output active NV12
snapshots. Original source arguments and `bwdif` parsing are observed; the
NV12 sink constraint is qualified by pinned original source and actual NV12
returns, not by an intercepted option-setting call. Old receive-frame and
progressive Off observers keep their formats and evidence scopes.

After original media and compositor retirement, the controller reads the
complete observer report and replays each selected temporal prefix through
an OWN client using the original App filter/util in a CPU-only Platform
namespace. It compares the four active NV12 outputs and captured raw
properties. Prefix replay preserves the original API order and supplies no
source EOF. An explicitly requested `flushed-reference` run is a separate
reference-only control; it never repairs or drains Kodi. The reference namespace
has no card/GPU bind, and an unresolved owned reference process retains its
HANDOFF record and output directory.

Source90k ticks, numeric Kodi microseconds, AVFrame clocks, declared graph1/1
and observed sink clocks remain separate. Missing client EOF or a short output
tail remains an observation. Neither a nominal900-to1800 expectation nor a
successful same-input prefix establishes whole-tail cadence, independent
codec/scaler correctness, physical presentation, normal-backend compatibility
or release qualification. The system codec62/filter11 input census and native
field-weave reference cannot substitute for this original-App filter12 replay.

### Current replay-disposition observation

The [replay2997-01 receipt](fixtures/issue477/replay-disposition-20261104.json)
uses diagnostic source `42ab9ac` and original Kodi/App/providers. Its two
same-PID natural EOFs, normal Quit, final parent-PID analysis and both selected
prefix references complete. Controls and native replay, packet, cost and
post-filter audits pass; the application exits0, kernel checking is0, the
module13/runtime36 boundary is equal, no handoff remains and maximum temperature is 67C.

| Actual stage | First natural play | Reopen natural play |
| --- | --- | --- |
| Native original bindings / logical inputs / public M | 690 / 690 / 690 | 694 / 694 / 694 |
| Original field dequeues / validated released pairs | 1,380 / 690 | 1,388 / 694 |
| Successful restarts / completed replay inputs / replay fields | 0 / 0 / 0 | 0 / 0 / 0 |
| Accepted BWDIF inputs / successful outputs | 690 / 1,378 | 694 / 1,386 |
| Original filter source NULL/close; sink EOF | 0; 0 | 0; 0 |

All 11,072 replay events publish completely and classify the observed original
records. Physical replay remains **NOT_EXERCISED** in both plays. Separate
actual maintained-path CPU controls cover completed replay after backing reuse,
raw zero tuples, valid pairs/releases and rejected parity/release failures;
those controls do not qualify physical replay. Applied to the same retained-
backing CPU records, the old census rejects incomplete live identity while
the corrected census classifies two originals plus one completed replay, with
raw zero tuples preserved. The unchanged 900-original requirement still fails
with two observed.
Do not repeat a physical case merely to obtain the event.

The first four filter outputs in each play match their same-input original-App
prefix reference in all 24,883,200 active NV12 bytes and captured raw metadata;
no EOF is injected. This is fresh selected-prefix evidence, not full-tail,
independent codec/scaler, source-clock or presentation qualification. The whole
case remains **OBSERVED_SOURCE_QUALIFICATION_FAIL** because 690/694 is below 900.
Successful diagnostic control flow and the existing public-M subset/max1 rule
do not waive the original-picture requirement or establish a performance fix.

The fresh selected codec/epoch sets contain 900 unique accepted nonempty-packet
PTS labels per play and 690/694 unique returned labels, exact integer subsets:

| Accepted-call entry state | Play 1: labels / returned / absent | Play 2: labels / returned / absent |
| --- | --- | --- |
| DEFAULT `0/0/0` | 340 / 340 / 0 | 356 / 356 / 0 |
| NONREF `8/8/8` | 560 / 350 / 210 | 544 / 338 / 206 |

All absent labels associate with entry NONREF state. In observed accepted-call
order, the first NONREF call's label is absent in play1 but returns in play2;
do not generalize the older
packet-only case's first-call finding. Raw timebases remain unset `0/1`.
This is label/state correspondence, not payload/source-clock/VA identity or
proof why the caller selects the policy.

Next distinguish the underlying NONREF/latency policy from measured OWN pixel
and CPU-access/release costs before a bounded own-code candidate comparison.
Keep original inputs and leases, ordinary defaults and all source criteria;
external applications/dependencies are not patched.

<a id="current-maintained-cost-observation"></a>

### Retained maintained-cost observation

The [maintained2997-01 receipt](fixtures/issue477/maintained-cost-20261104.json)
uses diagnostic source `8cdbc62` and the original App/providers, leaving the
host module13/runtime36 baseline unchanged. Its 42,766-event cost ledger validates offline;
correlation by actual materialized six-scalar native keys covers 689/681
originals, with field-conversion/canvas pixel coverage for every such key.
Unjoined/partial observations remain visible separately. Matched execution-
parent timing can include still-bound replay work; it is not a replay-free or
whole-transport budget. This subset validation is not a strict native-ledger or
source-population PASS.

| Correlated pixel stage | Play 1 CPU / wall p50 (ms) | Play 2 CPU / wall p50 (ms) |
| --- | --- | --- |
| First-field staging | 0.932 / 1.084 | 0.952 / 1.149 |
| Field-conversion pixels | 1.668 / 2.472 | 1.697 / 2.500 |
| Canvas pixels | 1.788 / 2.325 | 1.731 / 2.213 |

These scopes exclude selected payload capture and separate CPU-access begin/
end from pixel loops. They are not summed medians or overlapping playback
budgets. CPU-access-end p95 thread CPU is2.869/2.706ms in the correlated
population; interpret that aggregate with the retained parent/direction split
below rather than as pure waiting. Wall-minus-thread-CPU mixes waits and preemption, with negative
independent-clock differences retained as unavailable derived off-CPU rather
than clamped. The initial active-pixel clock-accounting FAIL is preserved.

The correlated subset records 689/681 direct-public events, with public copy/
promotion unobserved there. All-context observations still contain 11 public
copies and 11 promotions per play outside that subset; no global zero-copy
claim follows. Within the correlated subset, diagnostic callback events
are1,378/1,362, but selected-capture flags occur only twice per play: callbacks
are not a pixel-snapshot count.
Sync body spans may condition-wait/unlock and do not measure uninterrupted
mutex ownership.

The original actual case and controls remain **FAIL**. Both natural end callbacks
occur, application exit is0, kernel checking is0, the original runtime/boundary
is retained, no handoff remains and maximum temperature is73C. Materializations
are 689/681 against the unchanged 900 criterion. In the second play, 691 input
sends and1,382 dequeue events include10 replayed AUs/20 fields; four dequeue
records arrive after their old full bindings retired. Source review explains
how completed transport AUs can be resent and duplicate outputs return before
copy/materialization, but that capture's original strict census lacked explicit replay/
historical-binding disposition. That is a concrete own diagnostic coverage gap,
not proof of a product defect or harmlessness. The retired raw post-filter
report records689→1,376 and681→1,360 inputs/outputs, with no source NULL/close
or sink EOF. No fresh original-App selected-prefix reference ran after the
native-census failure; previous prefix PASS
receipts retain their earlier source/case identities. Separate offline packet
metadata still records900 video demux returns and900 accepted codec data sends
per play; it does not repair the native ledger.

The retained parent/direction review of this same `8cdbc62` record locates
median thread CPU for canvas WRITE CPU-access END at 1.971/1.928 ms across the
689/681-key subset. This is a source/branch-qualified measurement, not pure
GPU waiting, a causal NONREF explanation or current `42ab9ac` performance.
Preserve first-field staging and every original lease/copy unless an ownership
proof supports an own-code change. The new replay-aware implementation and its
separate receipt do not relabel this original actual-case/control FAIL or
establish an external code fix, normal-backend speedup or release result.

<a id="current-packet-admission-observation"></a>

### Retained packet-admission observation

The [packet2997-01 receipt](fixtures/issue477/packet-admission-20261104.json)
records two same-PID natural EOFs with the metadata probe enabled:

| Selected public stage | First natural play | Reopen natural play |
| --- | --- | --- |
| H264/video outer demux returns | 900 | 900 |
| Codec data send attempts / accepted | 901 / 900 | 900 / 900 |
| Successful VAAPI frame returns | 677 | 684 |
| Native submit/input/materialized originals; accepted BWDIF inputs | 677 | 684 |
| Native field dequeue records | 1,354 | 1,368 |
| Successful filter outputs before graph free | 1,352 | 1,366 |
| Nonnull empty-drain attempts / accepted at codec | 9 / 1 | 7 / 1 |
| Codec receive EOF returns | 7 | 5 |
| Post-filter source NULL/close; sink EOF | 0; 0 | 0; 0 |

One data send returns EAGAIN in the first play before the observed 900 accepted
data sends. Codec drain is an actual nonnull data-NULL/size0 packet, with one
accepted per play; no NULL send is observed. This codec-level EOF does not
supply EOF to the separate BWDIF graph. NONREF skip_frame/skip_idct/
skip_loop_filter entry states are observed. The read-only raw-label review
finds 900 unique accepted nonempty-packet PTS values in each selected codec/epoch;
all successful frame PTS equal best-effort and form exact subsets of those values.
The skip-state partition is:

| Accepted-call entry state | Play 1: labels / returned / absent | Play 2: labels / returned / absent |
| --- | --- | --- |
| DEFAULT `0/0/0` | 332 / 332 / 0 | 330 / 330 / 0 |
| NONREF `8/8/8` | 568 / 345 / 223 | 570 / 354 / 216 |

In observed accepted-call order, each play's first absent label belongs to the
first accepted NONREF call.
This is exact integer-label/state correspondence in the selected codec/epoch,
with EAGAIN attempts and empty drain excluded; it is not an observed demux-to-
codec payload identity or packet-to-VA binding. Packet/frame/codec timebases
remain `0/1`. Private start offsets, source90k conversion, source/pixel identity
and the underlying CPU/latency cause of selecting the skip policy remain unproved.

The 900-native-original criterion still fails (677/684 observed), so the
controller retains `OBSERVED_SOURCE_QUALIFICATION_FAIL` and exit1 despite
scoped packet/post-filter observation and selected-prefix reference PASS.
The application exits0 after both natural EOFs, kernel checking succeeds,
no handoff remains and maximum temperature is67C. Those facts do not turn the
223/216-native deficit, full-tail cadence or stock/release support into PASS.

### Retained post-filter-only observation

The earlier [bwdif2997-02 receipt](fixtures/issue477/bwdif-postfilter-20261104.json)
keeps its original populations:


| Measured stage | First natural play | Reopen natural play |
| --- | --- | --- |
| Short source packets/software-decoded originals | 900 | 900 |
| Submitted/input/materialized originals and accepted BWDIF inputs | 687 | 690 |
| Native field dequeue records | 1,374 | 1,380 |
| Successful filter outputs before graph free | 1,372 | 1,378 |
| Source NULL/close; sink EOF | 0; 0 | 0; 0 |

The native subset ledgers are balanced, but the complete-source criterion is
**FAIL**: case02 ends `OBSERVED_SOURCE_QUALIFICATION_FAIL` with nonzero exit.
Two same-PID natural EOFs and the first four outputs per play were observed;
the eight outputs match the original-App same-input reference in all 24,883,200
active NV12 bytes and captured raw properties. A separate explicit flush of the
three captured inputs yields six reference outputs rather than four, with EOF
injected only into that reference. It does not change the original client's
two-output shortfall or explain the 213/210 source originals absent before
VA submission. Existing decoder-drop log rows are approximate, not that exact
packet/skip-state attribution. This original-App EOF-boundary diagnostic is a
scoped known limitation, not a general stock compatibility or release result.

The initial case01 FAIL remains unchanged: the first-play strict900 census
aborted reopening, and its legacy observer footer rejected a parent fork.
The corrected observer explicitly records only its owner PID, with child
forwarding excluded; it does not rewrite that old capture. Case02 exits the
application normally, retains the original module13/runtime boundary and has
no kernel-check error. Those cleanup results do not upgrade source population,
whole-tail cadence or unselected pixels to PASS.

## Actual returned AVFrame PTS (isolated observation)

`make -C filters/vaapi frame-pts-observer FFMPEG63_INCLUDE=/path/to/headers`
builds a separate OWN `frame-pts-observer/frame-pts-probe.so`. It requires the
unchanged codec63.1.102/util61.1.102 public headers used by the original App;
host codec62/util60 headers are unsuitable. No FFmpeg library is linked into
the probe. The artifact is excluded from ordinary build/install/package paths.
`check-frame-pts-observer` uses synthetic public-structure/provider controls;
those controls do not establish the original runtime ABI.

The optional prepare-only `--observe-frame-pts` mode of
`vaapi-centered-controls-stock.py` freezes this explicit LD_PRELOAD observer
and its reader alongside the existing original-App and fixture inputs. This
is an instrumented diagnostic run. It does not replace the previous
source-content-only receipts or qualify normal stock compatibility, release,
GPU-reader completion or scanout. No external product source or binary is
modified, and the rejected private FFmpeg candidate libraries are never used.

The probe forwards `avcodec_receive_frame@LIBAVCODEC_63` unchanged. Only after
a successful VAAPI return and verified original codec/util providers does it
copy scalar PTS, best-effort timestamp, declared frame/packet time bases,
`data[3]` surface identity and descriptive codec/frame/pool pointer identities.
It does not retain frames, alter references or access picture bytes. A fixed
bounded memory buffer replaces per-frame file output/allocation. The report
is written at ordinary process shutdown; overflow, ABI/provider failure and
incomplete output cannot qualify an observation. Return values and errno are
preserved. Metadata capture still adds work, so timing/performance is not
qualified by this probe.

The JSONL report uses `crystalhd-frame-pts-v1`: one header, sequenced frame
records and one complete footer. Each record brackets its metadata snapshot
with CLOCK_MONOTONIC nanoseconds. PTS values retain the signed AV_NOPTS_VALUE
sentinel. Unknown time bases remain unknown; a valid codec packet time base
is explicitly identified when a frame time base is unset. Pointer identities
may be reused and are descriptive only; display identity is unobserved.
The output environment value is an absolute prefix: the probe appends
`-<pid>.jsonl`. The runner reads only the identified Kodi PID and requires that
PID in the report. Inherited observers in child processes cannot occupy the
parent's report path; their reports do not qualify the parent's frames.

`vaapi-frame-pts-reader.py` joins the actual surface over the whole snapshot
interval to exactly one unchanged OWN binding, across every driver instance
in the same PID. Complete trace sequences and retirement boundaries are
required. A gap, collision or binding change during capture fails; nearest
timestamps and synthetic VA tokens are not media clocks. Submitted decoder
lifetimes require explicit epoch retirement and a successful terminal
`context_destroy`; `binding_end` alone is insufficient. Display-wide
`session_destroy` is reported separately. Its absence after all submitted
decoder contexts retire cannot invalidate an earlier fully bounded snapshot.
Existing independent
full-NV12 content audits supply selected source90/91 identities. Their actual
returned timestamps are compared to the fixture clock with the explicitly
declared runtime tick precision, without inferring source content from PTS.
When both declared time bases are unset, a successful raw-PTS/binding join
remains an observation while the source-clock comparison is explicitly
`NOT_OBSERVED_UNDECLARED_TIME_BASE`. The natural reopen requires180 distinct
returned bindings with actual raw PTS; declared-clock coverage is reported
separately. The fixture clock is never substituted for missing runtime units.

## Ordinary Kodi flush and the defensive generation reset

For the pinned Kodi22RC1 source, `DVDVideoCodecFFmpeg::Reset()` flushes FFmpeg
and calls the hardware Reset; `VAAPI::CDecoder::Reset()` flushes the output
queue. Neither operation destroys the live decoder surface pool. Dispose
frees the codec context before releasing the hardware object. FFmpeg's VAAPI
uninit destroys its VA context at that point. Sources:
[codec Reset/Dispose](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecFFmpeg.cpp#L955),
[hardware Reset](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoPlayer/DVDCodecs/Video/VAAPI.cpp#L1106).

The OWN `context_reset` event instead comes from `DestroySurfacesWithTimeout`
when a nonclosing decoder still owns a nonobsolete destroyed target. The
libva `vaDestroySurfaces` contract requires destroying contexts using those
surfaces first (`va/va.h`, installed libva1.23.0, lines1906–1910). This makes
live-context pool teardown a separate defensive case. Zero resets on these
ordinary Kodi seek/Stop/reopen runs are expected from the source paths and
are not a reason to repeat the same seek or force a device reset. Reset-v2
close/device coverage remains CPU-controlled; no actual defensive reset is
claimed. Other clients and failure/recovery routes remain separate.

## Progressive Off CPU-copy route

The controls runner's prepare-only `--render-route cpu-copy` selects the
original Kodi `videoplayer.prefervaapirender=false` setting. Its ordinary
default remains `va`. This is the original VA hardware decoder followed by
Kodi's `CFFmpegPostproc` CPU-copy/readback path with deinterlace Off; it does
not exercise BWDIF or the OWN VPP sidecar-mismatch promotion branch.

The CPU-copy plan enables the published video logging category and requires
the original `Initializing ffmpeg postproc` selection in each play. A method
label of `none`, a previous play's message, or an attempted initialization
followed by preinit failure/skip fallback cannot qualify this route. The
same worker must also log the completed Off/NONE initialization
(`VAAPI::CFFmpegPostproc::Init  - skip deinterlacing`). Fresh owned Kodi-log
identity and separate byte windows bind these observations to each play;
the `T:` field on postproc messages identifies the worker, not the process.
Original `CFFmpegPostproc::Flush()` calls `Close()` then `Init(m_diMethod)`
on the retained object. Repeated Off completion on a worker already selected
in this play is recorded as reinitialization; it cannot establish a new route.
The
original App's `lib/kodi/system/libsse4-x86_64-linux.so` is frozen and its mapped
provider is checked while the owned application is paused.

The existing180-picture progressive fixture, fixed source90/91 targets,
full NV12/source/seek-window audits and same-PID natural reopen are reused.
Full NV12 bytes are observed at the OWN public-surface boundary. Downstream
BGRA barcodes check selected CPU-copy output content; they do not compare all
post-copy CPU-frame YUV bytes or every rendered pixel.
The optional receive-frame observer continues to report raw PTS separately
from undeclared runtime clocks. CPU-copy source-content observations remain
isolated OWN diagnostics, with GPU return/scanout and normal-backend/release
qualification outside their scope. Actual route selection and successful
readback must be observed before recording a completed CPU-copy case.
Pinned source: [route choice](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoPlayer/DVDCodecs/Video/VAAPI.cpp#L2199),
[preinit, Off initialization and sync/derive/map/copy](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoPlayer/DVDCodecs/Video/VAAPI.cpp#L2975).
