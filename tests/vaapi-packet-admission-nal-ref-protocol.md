# Isolated packet NAL-reference extension

`vaapi-packet-admission-nal-ref-probe.c` is an own diagnostic derivative of
`vaapi-packet-admission-probe.c` at maintained source43da405. The original probe,
v1 reader/protocol, product and external applications remain unchanged. Build
the variant to a separately named output in this checkout, against the same
unchanged original format63.1.102/codec63.1.102/util61.1.102 public headers.
It is excluded from normal build/install/release qualification.

The existing `CRYSTALHD_TEST_PACKET_ADMISSION=v1` activation, provider checks,
event sequence, public metadata, forwarding/result/errno, fork/lifetime rules,
fixed ring capacities, paired clocks and complete footer are preserved.
The variant additionally resolves public `av_opt_get_int` at LIBAVUTIL_61 and
requires its actual canonical provider path and DSO base to equal the attested
util provider. It reads `is_avc` and `nal_length_size` with flags0 on the tracked
H.264 context's public `priv_data` before forwarding. There is no private offset,
option write or decoder call. Original entry errno is restored before the real
send and its actual result/errno are preserved; query statuses/values stay in
the diagnostic event.
These getter reads additionally require actual entry `thread_count:1` and
`active_thread_type:0`, with the already open, nonconcurrent codec lifetime.
Other threading states record UNKNOWN without calling the getter; no worker's
private option state is read under an assumed software frame-thread boundary.
The variant adds no references, retries, packet mutation, payload copy/hash/log,
per-event I/O or sidecar. Its bounded reads add diagnostic overhead; elapsed
spans include that work and are not pure external-call cost or stock timing.

The same v1 header has this explicit additive object:

```json
"extensions": {"nal_ref": {
  "schema": "crystalhd-packet-nal-ref-v1",
  "capture": "H264_VIDEO_NONNULL_SEND_ENTRY_BEFORE_REAL_CALL",
  "max_packet_bytes": 4194304,
  "max_nals": 256,
  "max_extradata_bytes": 65536,
  "max_side_data_elems": 64,
  "framing_policy": "SUCCESSFUL_OPEN_AND_CURRENT_ANNEX_B_EXTRADATA_OPTIONS_ZERO_NO_OBSERVED_TRANSITION",
  "public_option_getter_provider_ok": true,
  "vcl_nal_types": [1, 5],
  "classification_scope": "NAL_HEADER_ONLY_NOT_SLICE_AU_OR_DECODER_VALIDITY"
}}
```

An optional event `nal_ref` appears on **every tracked, open, nonconcurrent
H.264 video nonnull `avcodec_send_packet` entry**, including size0, null data,
negative sizes and rejected sends. It is absent on NULL sends and all other
APIs/codecs or unqualified lifetimes. Capture occurs after the same entry
metadata copy and before the one real call; it stays attached to that event's
sequence, codec instance/epoch, raw PTS, entry discard values and eventual return.
It neither identifies a source picture nor proves whether FFmpeg skipped it.

Its exact keys are `classification`, `reason`, `framing_origin`,
`is_avc_query_status`, `is_avc`, `nal_length_size_query_status`,
`nal_length_size`, `framing_complete` (boolean),
`bytes_scanned`, `nal_count`, `vcl_count`, `vcl_ref_idc_counts` (four entries0..3)
and `nal_type_counts` (32 entries). The counters describe only examined headers;
`vcl_count` counts supported ordinary slice types1/5 and equals their type sum
and the reference histogram sum. All header type counts sum to `nal_count`.
On UNKNOWN they may be partial and supply no all-packet reference conclusion.
`bytes_scanned` is the actual read high-water mark, bounded by packet size and
the byte cap; precheck failures read zero bytes.

Packet bytes **never select Annex B mode**. A valid AVCC four-byte length257
starts `00 00 01 01` and can masquerade as a prefix plus a nonref header while
its actual NAL header is reference-coded. The gate requires bounded SPS+PPS
Annex B extradata at a genuinely successful new codec open and again at send
entry; avcC configurationVersion1, absent or ambiguous extradata stays unknown.
Both attested public option queries must succeed with actual values0/0, ruling
out a retained or overridden `nal_length_size` and AVCC autodetection.

Any `AV_PKT_DATA_NEW_EXTRADATA` entry or malformed side-data array invalidates
framing authority for the **entire codec lifetime**, including later packets
and flush epochs; a fresh successful lifetime alone resets the latch. A failed
send does not clear that conservative transition. This avoids trusting current
public extradata after a packet changed the decoder's private framing state.
No AVCC length parser, payload guess or hash is used.

Only all those proofs produce
`framing_origin:CONTEXT_OPEN_AND_CURRENT_ANNEX_B_OPTIONS_ZERO`; otherwise it is
`UNKNOWN`, classification is UNKNOWN, framing is false and packet counters/read
high-water are zero. Query statuses are null if unavailable/unattempted; values
are null on failed/unattempted queries, otherwise the actual signed values.
Known authority can still produce packet-level UNKNOWN with partial counters.

`ALL_VCL_ZERO` means every supported VCL header has nal_ref_idc0;
`ALL_VCL_REF` means all have nonzero nal_ref_idc; `MIXED` contains both.
Only NAL types1/5 enter that histogram; SPS/SEI/AUD header ref bits do not.
A B-picture label is never a non-reference oracle. Successful classifications
require `reason:NONE` and `framing_complete:true`.

UNKNOWN reasons are `NO_DATA`, `INVALID_SIZE`, `BYTE_CAP`, `NOT_ANNEX_B`,
`TRUNCATED_NAL`, `FORBIDDEN_BIT`, `UNSUPPORTED_TYPE`, `NAL_CAP`, `NO_VCL`,
`BAD_ESCAPE` and `IDR_REF_ZERO`. `NO_VCL` alone may have complete framing;
all other UNKNOWN reasons have `framing_complete:false`. The parser accepts
three/four-byte Annex B prefixes and permitted leading/trailing zero bytes.
It rejects detectable empty/header-only/trailing-prefix or escape truncation,
forbidden headers, IDR ref0, unsupported partitions/extensions/reserved types,
missing VCL, and either cap. It never guesses AVCC length prefixes or a slice
picture type. Plain non-VCL types6..12 are counted without semantic decoding.
Authority UNKNOWN reasons are `CONTEXT_AVCC`, `CONTEXT_MISSING`,
`CONTEXT_AMBIGUOUS`, `NEW_EXTRADATA`, `CONTEXT_TRANSITION`, `SIDE_DATA_INVALID`,
`OPTIONS_UNAVAILABLE`, `OPTIONS_QUERY_FAILED` and `OPTIONS_NONZERO`.

This is a **NAL header/framing census**, not entropy/slice/AU validation.
Arbitrary payload truncation or corruption beyond these structural checks
cannot be diagnosed by header inspection; no complete access unit or valid
H.264 decoding is claimed. It does not parse slice_type or independently prove
reference-picture/DPB effects. Any downstream closed-window skip attribution
must keep buffering, EAGAIN/retry, flush, concurrency, discard changes, mixed
or UNKNOWN data and packet/native clock identity limitations explicit.

The original v1 reader accepts additive keys and retains all existing gates;
its passing result does not qualify this extension. The separate extension
reader must validate its header, eligibility, fields and UNKNOWN boundaries.
`vaapi-packet-admission-nal-ref.test.c` independently covers header/reference,
prefix/zero, mixed/reference-B counterexamples, unsupported/invalid/truncated,
escape and exact-cap controls, plus AVCC257/353 prefix collisions and rejected
context/transition/option authority; it runs no media, FFmpeg, device or decoder.
ROOT alone compiles/runs these controls and any original-App observation.
