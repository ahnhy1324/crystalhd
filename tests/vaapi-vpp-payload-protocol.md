# Private completed VPP payload protocol

This explicit test backend observes OWN CPU Bob NV12 payloads while the pinned
original Kodi/FFmpeg images remain unchanged. The ordinary driver has no
diagnostic environment variable, state, payload reference, allocation or copy:
`CRYSTALHD_VAAPI_TEST_VPP_PAYLOAD` is defined only by the separate wrapper.
`all`, `check`, and `install` do not build or install that wrapper.

The implementation and protocol have source review only until the root operator
records actual build/control/media results. A historical PASS for operation
counts is not a PASS for these new payload captures.

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
ordinary backend lacks `CRYSTALHD_TEST_VPP_PAYLOAD_DIR` and the observer schema.

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
decision. Geometry is deliberately limited to origin-aligned 1280x720 NV12 Bob
with equal input/output sizes and physical immutable canonical source planes.
Borrowed direct pictures, centered sidecars, scaling, cropping, BGRA and other
cases produce no qualifying checkpoint.

Eight preallocated slots hold two source parents, both physical parities, in
each of two `(context,generation)` decoder lifetimes. Their source/target byte
arrays reserve 22,118,400 bytes in total per diagnostic VA driver instance.
Selection takes the first two supported canonical source bindings per lifetime;
a failed selection is retained and is never replaced by a later retry. Source
and target rows are packed independently; source pitches, physical dimensions,
logical rectangles and original plane addresses remain in metadata. Pitch tails
are excluded. Compact first fields retain half-height physical storage and the
validated `H264FirstField` hardware flag provenance, separately from the original
logical source binding. Their samples are additionally compared with the same
parent's later completed full source by the independent reader.

Copies stay tentative through both CPU ENDs. Queued publication additionally
requires the existing final fence release, successful normal completion, current
decoder lifetime/source token, live objects, current destination operation and
nonstopping driver. The immediate path applies the corresponding current
lifetime/operation checks. Canceled, superseded, failed or incomplete captures
write no payload files. Their selected metadata remains a strict failure rather
than disappearing from the census. `Driver` flushes only independent arrays
after `StopWorkers` joins both workers and outside its mutex. Output creation is
exclusive in a preopened fresh directory; partial/old files are never overwritten.

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
  --plan /home/mache/.cache/crystalhd-all-work-20261031/frozen-stock-bob-payload-plan.json \
  --case stock-bob-payload-01 --own-source /ABSOLUTE/REVIEWED/OWN/TREE \
  --own-revision FULL_40_HEX_OWN_COMMIT \
  --tff-probe /ABSOLUTE/FROZEN/TFF-FFPROBE.json \
  --bff-probe /ABSOLUTE/FROZEN/BFF-FFPROBE.json
python3 -B tests/vaapi-vpp-payload-stock.py --execute \
  --plan /home/mache/.cache/crystalhd-all-work-20261031/frozen-stock-bob-payload-plan.json \
  --case stock-bob-payload-01
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
complete selected trace sequences and one final `session_destroy`.

Keep the existing boundary/AC/temperature guards and finite deadlines. If media
survives normal Quit and verified pidfd TERM, retain compositor, private runtime,
profile and payload directory and honor `live-resource-handoff.json`. Do not use
SIGKILL, `--die-with-parent`, new playback, cleanup or teardown under a survivor.
If only the compositor survives its normal TERM, retain its runtime and handoff.
The diagnostic arrays flush only on orderly backend destruction; an unflushed
survivor is incomplete evidence.

## Source anchors and proof limits

The hook locations are `ProcessVpp`, `VppPixelView::Capture`,
`PrepareFirstFieldBob`, `RunVppWorker`, `ReleasePendingVpp`, the immediate and
synchronous VPP branches in `EndPicture`, and `Driver::~Driver` in
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
nor their chronological presentation. Source/output PTS, external fd/import
release, GPU fence/scanout lifetime and displayed pixels remain NOT_RUN.
Instrumentation overhead makes this unsuitable for a performance claim. Existing
public `binding_end` and export call records retain their narrower meanings.
No heap observer or investigation is included.
