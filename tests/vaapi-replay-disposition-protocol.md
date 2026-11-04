# OWN replay disposition observer v1

Private scalar diagnosis only. The explicit backend enables centered payload,
maintained cost and replay disposition; ordinary build/install/package has no
new state, runtime knob or hooks. No source picture/lease/copy/replay/error/EOF
behavior is changed. History comes offline from committed original submit_bind
rows; no Surface/pixel/FD/reference or runtime historical ownership is retained.

The explicit artifact is built with `make -C filters/vaapi
replay-disposition-observer`; it is written only below
`filters/vaapi/replay-disposition-observer/`. The affected CPU check is
`make -C filters/vaapi check-replay-disposition-observer`. Its collector controls
and actual Pump/ReceiveOne field-pair scaffolds use no real device; positive and
expected-rejection paths have separate PID reports. Two software originals in
those controls do not replace the actual900-original criterion.

The existing [frozen payload controller](vaapi-centered-payload-protocol.md)
uses prepare-only `--scenario bwdif2997 --observe-packets --observe-cost
--observe-replay`. Replay requires the preceding packet/cost/postfilter setup;
only the explicit later frozen execution runs the diagnostic case. It does not
change ordinary defaults or authorize a different installed backend.

Enable `CRYSTALHD_TEST_REPLAY_DISPOSITION=v1` and absolute
`CRYSTALHD_TEST_REPLAY_DISPOSITION_PREFIX`. Exclusive0600 output is
`PREFIX-PID.jsonl`, published at process/private-state retirement.

Header: `type:"header",schema:"crystalhd-replay-disposition-v1",pid,
fork_policy:"PARENT_PID_ONLY_CHILD_FORWARD_ONLY",
scope:"OWN_REPLAY_DISPOSITION_ONLY",capacity:32768,bc_success:0,va_success:0,
field_constants:{topfield:16,bottomfield:24,interlaced_src:32,bottom_first:128}`.
CPU controls may reduce capacity at compile time. Footer:
`type:"footer",events,attempted_events,dropped,inflight,unpublished,
clock_failures,lifecycle_failures,fork_seen,complete`. Completeness requires no
overflow/inflight/unpublished/clock/lifecycle/publication failure. Fork is
explicitly descriptive: pre-exec children neither observe nor publish inherited
state; owner PID and atfork handling are initialized at private DSO/TU load,
before the first hook. Exec starts its own PID ledger. Constructor, record,
finish and publication preserve errno. No per-call allocation or IO is added.

Every event has `type:"event",seq,id` (zero-based reservation; id=seq+1),
`pid,tid,monotonic_ns,event,driver_instance,context,generation,token,
restart_ordinal,receive_id,native_seq,live_decode_identity,
live_submission_ordinal,live_surface,live_owner,result` (nullable signed integer),
`completed_before` (nullable boolean), `fragmented,consume_prefeed` (booleans),
`input_offset,input_bytes,unit_bytes,cached_units` (unsigned counts),
`restart_phase` (null or string), `inspect,observe` (null or `new`, `duplicate`,
`unknown`, `invalid`), `field_role` (`none`, `first`, `complement`, `whole`),
`field_valid,pair_valid` (nullable booleans), `replay_duplicate` (nullable boolean),
`pair_first_receive_id` (0 if unavailable), `picture_number,sess_num,
picture_meta_payload,width,height,field_flags` (unsigned original metadata).
Unused scalar fields are0 and optional values null, not inferred success.
Output/disposition/release retain the same live tuple and raw field metadata;
only later decision fields/results are added. Input commit repeats the final
successful actual attempt lengths/fragment/prefeed values. Every actual
PumpDecodeInput logical-unit DIL call, including consumed prefeed, has an
input_attempt row. Speculative
TryH264Prefeed prefix/auxiliary calls retain their existing native prefeed_sent
scope and are not new logical replay debts.

Exact event names and meanings:

- `restart`: only actual restart attempts. restart_phase is `close`, `open`,
  `transition` or `committed`; result is actual BC close / VA open / bool
  Restarted return respectively. Committed successful transition increments the
  ordinal, distinct from client generation. Failed phases do not increment it.
  cached_units is captured once at restart entry for every phase. Close, open
  and transition use the old ordinal; committed carries the incremented ordinal
  and result1. native_seq/receive_id0.
- `input_attempt`: actual DIL compressed-input call result (BC_STATUS), actual
  unit token/completed state and fragment offset/transfer/total lengths BEFORE
  InputSent advances. Retry/failed/nonfinal fragments are not logical commits.
- `input_commit`: result1 only after existing successful InputSent. The actual
  unit.completed value was copied BEFORE that call changes its cursor; true
  identifies completed-history replay input. native_seq links the exact original
  input_sent. consume_prefeed does not invent another original packet/input.
- `output`: one actual public-picture dequeue. receive_id is a monotonic private
  process-global lease-operation ID (never reset at restart or generation),
  and native_seq is the exact output_dequeued receipt. Live
  scalar fields copy that emitted native row, including every original zero.
  Despite their wire names, live_* fields are raw TraceDecodeTokenFields copies,
  not proof of the current public VA owner: a still-pending original can retain
  its full immutable decoded-picture tuple after binding_end/backing reuse.
  Accepted New debt plus exact original provenance is separate from current
  public ownership and observed materialization. Unknown/zero nonreplay output
  remains unqualified. Original field metadata is copied, not pixel bytes.
  No nearest-time join.
- `disposition`: one ProcessDecodedOutput return per observed output, before
  release; result is actual VAStatus. inspect/observe reuse the actual existing
  evaluations. First valid field has field_role:first, field_valid:true,
  pair_valid:null and no Observe. Complement records the actual held first
  receive ID; pair_valid:true only after every existing identity/parity/geometry
  check succeeds. A duplicate pair must have replay_duplicate:true and actual
  observe:duplicate. Unknown/invalid remain explicit. Whole-picture paths use
  field_role:whole and no invented pair result. native_seq refers to that output.
- `release`: actual original DIL output-release BC_STATUS for the same receive_id
  and output native_seq. One release is required even for a first or rejected
  field; failure never satisfies a completed replay lease.

The native link is `(pid,driver_instance,native_seq)`. Internal Trace::Emit now
returns its exact local sequence ONLY after the original complete write, or0
when disabled/truncated/failed/partial; existing callers may ignore the result.
There is no later CurrentSequence getter/race. New private records must not
rewrite native live identity or manufacture a missing native receipt.

Restart ordinals and first-field receive IDs are private scalar context/field
metadata, reset with actual client generation/field reset. Receive scopes are
same-TID actual call scopes, not timing proximity. Auxiliary/EOS-only DIL output
without a public-picture output_dequeued is outside this field-pair protocol.
Process-only software scaffolds are not a hardware release qualification.

Reader uses original native submit_bind/binding_end/context reset/destroy to
construct unique offline historical provenance, keyed driver/context/generation/
token. Historical source tuple/surface/owner never restore a live lease. A
classified replay needs accepted completed input after a successful restart,
actual valid complementary duplicate pair and both successful releases; exact
native IDs account for still-bound and retired replay fields. Ordinary new
originals retain the existing materialization-subset policy: each observed M
row requires its unique original binding, accepted New pair and valid sequence,
with at most one M per original and no M caused by Duplicate. Missing M remains
visible and does not qualify pixel/public scope; it is not a new all-M gate.
Missing/ambiguous history, unknown output, wrong generation, forged debt,
malformed pair or incomplete release remain FAIL. Existing untagged FAILs cannot be upgraded retroactively.
The separate900-original source requirement never becomes a transport-send
count or a diagnostic-subset PASS. No causal CPU/latency/performance, harmless
replay, external product fix or stock/release claim follows from classification.
