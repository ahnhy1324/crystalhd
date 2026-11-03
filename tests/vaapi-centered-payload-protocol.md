# Private centered NV12 payload observation

This test-only backend observes maintained CrystalHD code under an unmodified
stock client. It does not replace the ordinary backend or qualify a release.
The existing noncentered Bob observer and its scope remain unchanged.

`CRYSTALHD_VAAPI_TEST_CENTERED_PAYLOAD` is enabled only by a separate wrapper.
Its private output variable is `CRYSTALHD_TEST_CENTERED_PAYLOAD_DIR`. Ordinary
build, install and runtime have no new state, copies or environment reader.

## Observation and publication

One driver reserves two decoder `(context,generation)` lifetimes, with the
first two distinct eligible source bindings per lifetime. Selection is fixed:
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
slot is eligible. Only successful consuming decoder-context retirement seals
a complete two-slot stream. A guard declared before the original Driver lock
publishes independent arrays after normal lock release. Each sealed stream
publishes once, so one natural EOF can be inspected before another play in
the same process. A live Driver destructor is not required. No Surface,
allocation, device or client ownership is retained by the independent arrays.
Rejected/incomplete retirement produces a bounded status record, not a
qualifying manifest. Exclusive file creation preserves old/partial outputs.

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
This protocol does not yet exercise a seek or requested Stop.

The private profile explicitly selects method0 and prefers VA rendering. The
runner requires the observed `ff-h264-vaapi`/`none` labels, mapped original
App/FFmpeg and selected OWN backend, hardware FD, natural EOF and exact source
input/output binding census. A successful public materialization count remains
separate from renderer delivery. At each EOF it requires ordinary decoder
retirement before independently reading that context's two-slot manifest.
It preserves a result for failed cases and a live-resource handoff if bounded
ordinary Quit/verified TERM cannot retire media. It never kills media, changes
the loaded module, resets the card or changes scheduling priority.
