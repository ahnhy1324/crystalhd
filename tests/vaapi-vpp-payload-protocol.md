# Private completed VPP payload protocol

This explicit test backend observes OWN CPU Bob NV12 payloads while the pinned
original Kodi/FFmpeg images remain unchanged. The ordinary driver has no
diagnostic environment variable, state, payload reference, allocation or copy:
`CRYSTALHD_VAAPI_TEST_VPP_PAYLOAD` is defined only by the separate wrapper.
`all`, `check`, and `install` do not build or install that wrapper.

Case `stock-bob-payload-01` remains FAIL: its decoder stream supplied valid
operation counts and clean normal exit/boundaries, but the destructor-only
publisher never ran for its active driver instance. A probe instance wrote only
an empty manifest. That case is preserved unchanged. Root's first CPU controls
passed 57 checks and 21 independent-reader methods. These revised retirement
publication controls and a new case require fresh root execution; earlier PASS
counts do not establish the new payload evidence.

Case `stock-bob-payload-02` also remains FAIL and its plan/results are preserved:
normal EOF, operation counts, exit and boundaries passed, but no payload file
was published. The OWN observer incorrectly required both physical surfaces and
requested rectangles to be exactly 720 rows. Production supports coded/VA
padding with a smaller active prefix. The existing fixture receipt explicitly
documents coded736/visible720; cached original Kodi21.3 `VAAPI.cpp` also derives
surface and equal VPP rectangle heights from `coded_height`. That cached client
version is supporting source evidence, not proof of case02's actual dimensions;
the frozen raw trace has no geometry fields. This revision admits only bounded
equal rectangles and adds caller-side rejection receipts, so missing publication
can be diagnosed without another unchanged media retry.

## Build and controls (root executor only)

From the assigned source tree, with the qualified compiler/dependency
environment used for the ordinary backend:

```sh
make -C filters/vaapi check-payload-observer
make -C filters/vaapi payload-observer
```

The first target executes production `ProcessVpp` with private CPU planes and
the existing device/DIL-aborting Bob harness. It checks both physical parities,
compact input, selected-slot nonreplacement, storage epoch advancement, canceled
publication, and failed existing target CPU END. The independent Python controls
exercise all Y/U/V components, incorrect UV row mapping, compact/full confusion,
changed epochs/bindings, cancellation/sync failure, duplicated operations,
malformed traces and bounded artifact reading. No hardware is used by this
target. It neither builds nor links the OWN DIL. The second target explicitly
runs the ordinary `linux_lib/libcrystalhd` library submake before linking, so it
also works from a clean source tree and creates the normal library/SONAME links.
Command-line compiler and CPU settings propagate normally; no new library flags,
library behavior or install action is introduced. Root should also run the
relevant ordinary VA checks and verify that the
ordinary backend lacks `CRYSTALHD_TEST_VPP_PAYLOAD_DIR` and the observer schema,
and remains byte-identical to the ordinary artifact at OWN revision `147c142e`.

The only diagnostic driver is
`filters/vaapi/payload-observer/crystalhd_drv_video.so`. Never copy it over the
ordinary target or install it. Pin its hash and the current OWN DIL hash before
execution. The private target is always freshly compiled; it does not depend on
or rebuild the normal backend artifact. It uses the selected CPU flags and the
same source/dependency ABI as the normal backend.

## Exact observation boundary

`ProcessVpp` copies active NV12 rows after all NV12 writes and before its existing
`EndCpuWrite`/`EndCpuRead`. The target is under the already acquired write/fence
lease and the source under its already acquired read lease. The hook adds no
access acquisition, wait, firmware/API call, pixel mutation or new scheduling
decision. Geometry is deliberately limited to NV12 Bob with width1280, physical
canonical/target heights720..768 divisible by four, and equal origin-aligned
source/output rectangles within those physical bounds. Their requested height
is retained exactly and must also be720..768 divisible by four. The observation
is only the top active1280x720 output prefix and its source field rows. Borrowed
direct pictures, centered sidecars, unequal rectangles, nonzero offsets,
horizontal scaling, BGRA and other cases produce no qualifying checkpoint.

Eight preallocated slots hold two source parents, both physical parities, in
each of two `(context,generation)` decoder lifetimes. Their source/target byte
arrays reserve 22,118,400 bytes in total per diagnostic VA driver instance.
Selection takes the first two supported canonical source bindings per lifetime;
a failed selection is retained and is never replaced by a later retry. Source
and target rows are packed independently; source pitches, physical dimensions,
logical rectangles and original plane addresses remain in metadata. New
`source_payload`/`target_payload` fields describe the separately packed active
dimensions:1280x720 source/full and target,1280x360 source/compact. Coded bottom
padding and pitch tails are excluded. At equal requested heights each plane's
Bob mapping is `floor(row/2)`, so the first720 output Y rows and360 UV rows depend
only on that captured active source prefix even when the requested canvas is736
rows. Compact first fields retain half-height physical storage and the
validated `H264FirstField` hardware flag provenance, separately from the original
logical source binding. Their samples are additionally compared with the same
parent's later completed full source by the independent reader.

Copies stay tentative through both CPU ENDs. Queued publication additionally
requires the existing final fence release, successful normal completion, current
decoder lifetime/source token, live objects, current destination operation and
nonstopping driver. The immediate path applies the corresponding current
lifetime/operation checks. Canceled, superseded, failed or incomplete captures
prevent publication. The root case retains a strict failure with the raw
receipts rather than substituting a later operation or source payload.

The existing consuming `DestroyContextWithTimeout` path emits `context_destroy`
after the selected decoder's ordinary drain and terminal close consume its
device, with its final status. A diagnostic guard declared before the existing
`unique_lock` claims publication only after both selected `(context,generation)`
lifetimes retire successfully and all eight checkpoints have passed. Claiming
seals all arrays under that mutex. Normal destroy-call cleanup and lock release
then run before the guard writes independent arrays on the existing caller.
No worker I/O, additional wait, thread, drain, close or scheduling decision is
introduced. A shared diagnostic owner pins only independent bytes across this
callback; the Driver may remain alive. Unrelated, first-only, partial or failed
retirement cannot publish. Sealing blocks later selection/bookkeeping writes,
and an atomic once flag plus exclusive file creation prevents repeated output.
The destructor performs no publication. Partial/old files are never overwritten.

Each ordinary decoder retirement also snapshots a bounded
`crystalhd-vpp-payload-status-v1` record under the existing mutex, then emits it
from the same after-unlock caller guard. It records observer/directory status,
the first candidate's actual physical dimensions and rectangles, selection
rejections, and at most eight slot statuses. Worker-written fields are read only
after the mutex-owned slot retirement flag; unknown fields appear as null.
This slot flag describes OWN conversion bookkeeping, not external ownership.
The operator preserves these records and includes their summary in a missing
manifest failure. No status record authorizes a retry or substitutes for bytes.

Selection rejection bits are:1 directory unavailable,2 non-Bob,4 unavailable
surface,8 binding mismatch,16 format,32 borrowed/centered source,64 physical
geometry,128 rectangles,256 storage identity/planes,512 parity,1024 fixed quota,
2048 already selected. Quota rejection after the selected parents is expected;
the first-candidate mask and failed slot statuses distinguish actual failure.
Copy rejection reasons are1 geometry/target identity,2 compact provenance,
3 full-source allocation identity,4 plane/pitch validation;0 means no recorded
copy rejection only when the slot has retired. No worker performs diagnostic I/O.

Diagnostic `Surface` serials and advancing storage epochs identify OWN storage
views, including CPU/GBM/dumb/import installation, moved-storage adoption and
revocation. Borrowed `VppPixelView` metadata carries the pinned allocation's
identity. These identifiers and virtual addresses do not identify global
DMA-BUF aliases or external client lifetime.

## Finite original-stock operator

The new own helper is `tests/vaapi-vpp-payload-stock.py`. It preserves the frozen
ordinary Bob operator's normal Quit, verified pidfd TERM and surviving-resource
handoff protocol. It pins the previous own helper and operation reader, uses the
same original App, Platform, GL and Weston pins, and changes only the OWN VA
artifact and a fresh writable `/payload` bind. The backend's private-only
`CRYSTALHD_TEST_VPP_PAYLOAD_DIR=/payload` setting is added inside the namespace.
External source and binary images are neither edited nor replaced.

Root supplies the already independently generated frozen FFprobe JSON for the
two original fixture files. After root commits the reviewed OWN changes, freeze
a new plan and use a new case name; do not alter an old plan/result. Preparation
and execution are mutually exclusive modes, source/revision/probe arguments are
accepted only during preparation, and an existing plan path is rejected:

```sh
python3 -B tests/vaapi-vpp-payload-stock.py --prepare-plan \
  --plan /home/mache/.cache/crystalhd-all-work-20261031/frozen-stock-bob-payload-plan-03.json \
  --case stock-bob-payload-03 --own-source /ABSOLUTE/REVIEWED/OWN/TREE \
  --own-revision FULL_40_HEX_OWN_COMMIT \
  --tff-probe /ABSOLUTE/FROZEN/TFF-FFPROBE.json \
  --bff-probe /ABSOLUTE/FROZEN/BFF-FFPROBE.json
python3 -B tests/vaapi-vpp-payload-stock.py --execute \
  --plan /home/mache/.cache/crystalhd-all-work-20261031/frozen-stock-bob-payload-plan-03.json \
  --case stock-bob-payload-03
```

This is one play per frozen TFF/BFF source, with identity-only initial pause,
then normal-speed natural EOF and reopen in the same original Kodi process.
It requires actual `ff-h264-vaapi` and `vaapi-bob`, fifty original source bindings
and one hundred unique successful VPP operations per source. Exact checkpoint
operations must fall within that source's frozen raw-log interval. Eight selected
payload slots must independently pass full Y/U/V comparison and binding joins.
Missing manifests/slots/observers, stale status, fallback, incomplete EOF,
nonzero exit or boundary/kernel failure retain FAIL. No unchanged retry is part
of this protocol. An unselected zero-slot initialization session is not evidence.

The expected original H.264 fixtures remain
`/home/mache/.cache/crystalhd-single-field-20261015.boremY/`:

| Fixture | SHA256 |
| --- | --- |
| `h264-tff-1280x720-2s.mp4` | `847249796f3ff572a82421b0573aa3c5aaa4856b2a6901bb874204466b1409f3` |
| `h264-bff-1280x720-2s.mp4` | `ea902de5e02fc530f18fc8597bd476e185fa94f84c4a8ff3d4f94a0789afc779` |

After normal application termination, independently replay the actual manifest
against the frozen raw log (substitute the single nonempty manifest filename):

```sh
python3 -B tests/vaapi-vpp-payload-reader.py \
  --manifest /ABSOLUTE/NEW/CASE/payload/vpp-payload-PID-INSTANCE.json \
  --trace /ABSOLUTE/NEW/CASE/kodi.stdout
```

Freeze the plan, result, full raw log, every manifest and packed file, executable
and library pins, and OWN source/reader/controller hashes before independent
replay. This is a consistency audit of trusted frozen inputs, not authentication
of arbitrary evidence. The reader independently computes plane-local field-row
selection and compares every active output Y/U/V byte to actual captured source
bytes; it does not call the production row helper or compare to guessed FFmpeg
chroma. It requires source `submit_bind` and target successful `vpp_commit` joins,
contiguous selected-session sequences through the supplied frozen raw prefix,
and one successful `context_destroy` after the selected commits for each of the
two decoder lifetimes. The manifest's explicit publication receipt must match
those context/generation/status joins. This closes the selected context scope;
it does not require or infer `session_destroy` or a complete Driver lifetime.

Keep the existing boundary/AC/temperature guards and finite deadlines. If media
survives normal Quit and verified pidfd TERM, retain compositor, private runtime,
profile and payload directory and honor `live-resource-handoff.json`. Do not use
SIGKILL, `--die-with-parent`, new playback, cleanup or teardown under a survivor.
If only the compositor survives its normal TERM, retain its runtime and handoff.
The diagnostic arrays publish at the successful selected decoder retirement
boundary even while their Driver remains alive. Missing retirement/publication
is incomplete evidence and does not justify forcing backend destruction.

## Source anchors and proof limits

The hook locations are `ProcessVpp`, `VppPixelView::Capture`,
`PrepareFirstFieldBob`, `RunVppWorker`, `ReleasePendingVpp`, the immediate and
synchronous VPP branches in `EndPicture`, and `DestroyContextWithTimeout` in
`filters/vaapi/crystalhd_drv_video.cpp`. The production independent-plane row
mapping remains `crystalhd-vpp-bob.h::SourcePlaneRow`; the new reader independently
implements `floor(output_row * (logical_plane_rows / 2) / output_plane_rows)`.
It selects `2*field_row+physical_parity` for a full source, `field_row` for compact
first-field storage, separately for Y and UV. Compact/full equality follows
`CopyH264InterlacedToSurface`, including the existing same-field UV average.

The original App source is pinned to Kodi
[`28ea2eac1eb7af8fdcbd2672d933ce87f594be79`](https://github.com/xbmc/xbmc/tree/28ea2eac1eb7af8fdcbd2672d933ce87f594be79).
[`VideoSettings.h`](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoSettings.h),
[`MediaSettings.cpp`](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/settings/MediaSettings.cpp), and
[`VAAPI.cpp`](https://github.com/xbmc/xbmc/blob/28ea2eac1eb7af8fdcbd2672d933ce87f594be79/xbmc/cores/VideoPlayer/DVDCodecs/Video/VAAPI.cpp)
bind `<defaultvideosettings><interlacemethod>22` to the actual `vaapi-bob` /
`VAProcDeinterlacingBob` route. OWN VA implements CPU Bob behind that interface.
Enum7 uses the external FFmpeg `bwdif` postprocessor; enum6 uses renderer Bob and
a CPU download. This protocol requires enum22 and its actual reported route.

A PASS establishes the selected OWN completed CPU payloads, both physical
parities and full active chroma, plus natural EOF/count evidence in this finite
diagnostic run. It establishes neither all one hundred output fields' pixels
nor their chronological presentation, coded bottom padding or pitch tails.
Source/output PTS, Driver destruction,
external fd/import release, GPU fence/scanout lifetime and displayed pixels
remain NOT_RUN.
Instrumentation overhead makes this unsuitable for a performance claim. Existing
public `binding_end` and export call records retain their narrower meanings.
No heap observer or investigation is included.
