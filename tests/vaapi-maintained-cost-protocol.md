# OWN maintained-backend cost observer v2

Private instrumentation only: no normal backend knob/state/clocks, external
hooks, product fix, stock compatibility or release qualification. The explicit
private backend also retains the centered payload observer. Set
`CRYSTALHD_TEST_MAINTAINED_COST=v2` and absolute
`CRYSTALHD_TEST_MAINTAINED_COST_PREFIX`. Output `PREFIX-PID.jsonl` is exclusive0600.

Header: `type:"header",schema:"crystalhd-maintained-cost-v2",pid,
fork_policy:"PARENT_PID_ONLY_CHILD_FORWARD_ONLY",
scope:"OWN_MAINTAINED_BACKEND_ONLY",capacity:131072,
clock_names:{wall:"CLOCK_MONOTONIC",cpu:"CLOCK_THREAD_CPUTIME_ID"},stages:[...],
flags:{compact:1,decoded_public:2,direct_public:4,selected_capture:8,
i915_wc:16,read:32,write:64}`. CPU controls may reduce capacity at compile time.

Version2 only increases the fixed event capacity for additional first-field
scope trees. It preserves every event, clock, identity, stage and completeness
rule below. The current producer requires the v2 activation marker; a v1 marker
does not enable it. Ordinary backends still contain no observer state.

The offline reader also accepts retained `crystalhd-maintained-cost-v1` reports,
whose declared capacity and event count remain bounded by65536. Version2 is
bounded by131072; reduced CPU-control capacities remain explicit in the header.
JSONL is limited to the selected version's event bound plus header/footer and
128MiB. A v1 header cannot borrow the v2 quota. Overflow, dropped rows or any
other incomplete gate still rejects either version. Historical v1 captures and
their original PASS/FAIL dispositions are unchanged; extra diagnostic capacity
does not qualify source900, product performance or release acceptance.

Exact stage strings: `receive_one`, `dil_acquire`, `dil_release`, `process_output`,
`first_field_stage`, `second_field_process`, `field_convert`,
`field_convert_pixels`, `canvas_compose`, `canvas_pixels`, `cpu_access_begin`,
`cpu_access_end`, `decoded_cpu_begin`, `decoded_cpu_end`, `i915_domain`,
`public_copy`, `public_copy_pixels`, `public_promote`, `direct_public`,
`sync_admission`, `sync_body`, `diagnostic_capture`.

Events: `type:"event",seq` zero-based entry reservation, `id:seq+1`,
`parent_id` (actual active same-thread scope ID, or0), `pid,tid,end_tid,stage`,
`wall_begin_ns,wall_end_ns,cpu_begin_ns,cpu_end_ns,wall_ns,cpu_ns`,
`driver_instance,context,generation,token,decode_identity,submission_ordinal,
surface,owner,binding_kind,binding_phase,detail,flags,result`.
`result` is null unless an actual existing result was captured, otherwise a
signed integer (bool0/1, VAStatus or BC_STATUS as originally returned).
No media PTS, pointers, pixel views, objects or FDs are retained.

`binding_kind` is `none`, `decode`, `surface` or `full`:

- `none`: no source binding claim; driver_instance may identify the immutable
  driver session while remaining fields are0.
- `decode`: driver_instance/context/generation nonzero; remaining fields may
  be0 if unavailable. An actual output token can be recorded without pretending
  every binding component is resolved.
- `surface`: copied available surface trace scalars; driver session may be
  inherited from the actual same-thread parent. No full-source claim follows.
- `full`: driver_instance/context/generation/token/decode_identity/
  submission_ordinal/surface/owner all nonzero and not invalid-surface values.

`binding_phase` is `entry`, `inherited`, `resolved_after_output` or
`resolved_after_admission`. It describes when recorded identity became known,
not an inferred identity at interval entry. Child scopes may inherit scalar
identity only from their actual active same-thread parent; explicit valid
surface fields replace available components, never invent missing values.
`detail` is an unsigned stage-local scalar: CPU access uses the original DMA
read/write direction; field conversion uses its field mask; diagnostic_capture
uses1 compose/2 public/3 materialized; otherwise0 unless documented by the hook.
The field_convert function scope also covers input rejection: its original
unsigned field mask may lie outside1..3 only when result is0 (the original call
rejected it). Successful field_convert and every field_convert_pixels span
require1..3; an invalid mask never qualifies converted pixels.
`flags` is the header bitmask, with bits set only on actual branches.

The collector takes wall then threadCPU at entry, threadCPU then wall at finish.
Both endpoint TIDs must match. Records publish in entry-sequence order after
normal retirement; readiness release/acquire excludes partially written rows.
RAII early return/exception keeps scopes closed without changing errno.
No per-call allocation/IO, original-call serialization, lock movement, lease
extension, extra pixel copy or fabricated EOF is introduced. Same-thread active
scopes form a strict LIFO parent chain; Finish is idempotent and closes a scope
before an existing later operation when required (e.g. pixels before capture).

Footer: `type:"footer",events,attempted_events,dropped,inflight,unpublished,
clock_failures,lifecycle_failures,fork_seen,complete`. Complete requires no
capacity/publication/clock/lifecycle/inflight failure. Fork is descriptive for
the parent-PID-only ledger: inherited children neither observe nor publish it;
exec starts a new process scope. Reader requires every complete gate and exact
PID/policy/stage/flag contract. Missing/zero-count stage means NOT_OBSERVED,
not measured zero cost. CPU controls use explicit owned clock providers to
exercise failure boundaries; these do not qualify actual clocks/hardware.

Inclusive wall/CPU contain nested work and diagnostic capture. Pixel-only spans
finish before payload snapshots and before CPU access release. Sync admission
ends after the original lock acquisition (including failed try_lock); identity
is resolved afterward. Sync body may condition-wait/unlock and is not a mutex-
holder metric. DIL acquisition/release wrap original calls without relocating
them. First-field staging includes its actual allocation/copy; no borrowed
output lease is deleted or stretched. Direct-public/promotion/public-copy
counts describe only branches that actually execute.

Only fully contained same-TID, nonoverlapping siblings support exact child
subtraction. Each clock's nonnegative endpoint delta and nonnegative exclusive wall/CPU
are strict. The clocks use independent accounting: on a short span measured
thread CPU can exceed measured wall despite valid endpoint order/TID/nesting.
Preserve both exact raw deltas; a negative inclusive or exclusive wall-minus-CPU
remainder makes derived off-CPU unavailable, never clamped, tolerated away or
classified as a lifecycle failure. The precise accounting mechanism is not
established by that discrepancy. Wall-minus-threadCPU mixes waits and preemption, not pure IO/lock time.
Cross-thread sums and overlapping picture sums are not playback budgets.
Bookkeeping perturbs diagnostics; no bottleneck/CPU/skip-policy cause or effect
size follows without a discriminating comparison. Existing900 FAILs remain.
