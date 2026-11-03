# Private centered NV12 payload observation

This test-only backend observes maintained CrystalHD code under an unmodified
stock client. It does not replace the ordinary backend or qualify a release.
The existing noncentered Bob observer and its scope remain unchanged.

`CRYSTALHD_VAAPI_TEST_CENTERED_PAYLOAD` is enabled only by a separate wrapper.
Its private output variable is `CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR`. Ordinary
build, install and runtime have no new state, copies or environment reader.

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
`--own-revision` and an independent `--source-probe` packet/frame JSON. Execute
that plan explicitly with `--execute`. Both modes require `--plan` and matching
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
