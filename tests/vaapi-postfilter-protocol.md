# OWN post-filter observer v1

This is isolated diagnostic evidence; it does not qualify stock compatibility or
release acceptance. Build against unchanged FFmpeg12.1.102/util61.1.102 headers.
Set `CRYSTALHD_TEST_POSTFILTER=v1`, absolute `CRYSTALHD_TEST_POSTFILTER_PREFIX`, and
optionally absolute `CRYSTALHD_TEST_POSTFILTER_FILTER_PROVIDER` and
`CRYSTALHD_TEST_POSTFILTER_UTIL_PROVIDER` (defaults `/app/lib/libavfilter.so.12.1.102`
and `/app/lib/libavutil.so.61.1.102`). Output is `PREFIX-PID.jsonl`, exclusive0600.

The first JSONL record has `type:"header"`, `schema:"crystalhd-postfilter-v1"`,
`pid`, `fork_policy:"PARENT_PID_ONLY_CHILD_FORWARD_ONLY"`, `capacity:16384`, `snapshot_capacity:16`, `max_width:1920`,
`max_height:1088`, `nv12_format` (the compile-header enum),
`sink_constraint_basis:"PINNED_ORIGINAL_SOURCE_PLUS_ACTUAL_NV12_SINK_OUTPUT"`,
`filter_provider`, `util_provider`, `filter_version:786790`,
`util_version:3998054` and boolean `provider_ok`. Actual capacities may be
reduced at compile time only by OWN controls. No foreign frame/context is read
when provider checking fails.

Each call record has `type:"event", seq` (zero-based call-entry reservation),
`pid, tid, begin_ns, end_ns, api, graph_id` (monotonic lifetime ID starting1),
`graph, context` (hex pointer strings, descriptive only), `ret` (integer, or null
for graph_free), `errno_before, errno_after`, `flags` (integer), `input_null`
(boolean), `close_pts` (raw integer, only meaningful for buffersrc_close),
`input_index, output_index` (one-based per-graph nonnull attempts/successes;
otherwise0), `name, args` (bounded original create name/filter arguments), and
`frame` (null or object). API names are full public C names:
`avfilter_graph_create_filter`, `avfilter_graph_parse_ptr`,
`av_buffersrc_add_frame`, `av_buffersrc_add_frame_flags`, `av_buffersrc_close`,
`av_buffersink_get_frame`, `av_buffersink_get_frame_flags`, `avfilter_graph_free`.
Internal nested forwarding wrappers do not make duplicate records. Source
metadata/bytes are copied BEFORE the original call can consume them. Sink data
is copied only AFTER a nonnegative original return. NULL, EAGAIN(-11),
AVERROR_EOF(-541478725), errors and graph teardown are never synthesized.
Graph-free records the old graph pointer/ID before forwarding, then retires its
lifetime. Reused addresses receive new IDs. Create uses `name` for filter type,
`args` for arguments; parse has empty name and its exact filter string in args.

Frame objects have `pts, best_effort_timestamp, duration, time_base:[num,den],
format, width, height, linesize:[Y,UV], flags, sample_aspect_ratio:[num,den],
color_range, color_primaries, color_trc, colorspace, chroma_location,
crop:[top,bottom,left,right]`. Sink successes also have `sink_time_base` and
`sink_frame_rate` rational arrays on the event. Raw clock domains remain separate.

`frame.snapshot` is null unless selected, otherwise `{slot,file,bytes}`. File is
report-directory basename `PREFIX_BASENAME-PID-sNN.nv12`, slot zero-based.
Selection is the first3 nonnull NV12 source attempts and first4 NV12 successful
sink outputs per graph; metadata indices count ALL attempts/successes. NV12
snapshots contain tight active Y(width*height), then interleaved UV(width*height/2),
no pitch padding; width/height must be positive/even, <=1920x1088, each pitch >=
width. Storage is fixed16slots; selected-copy failure/slot exhaustion invalidates
evidence. No references or pointers are retained for delayed frame copying.

Last record: `type:"footer", events, attempted_events, dropped, snapshots,
snapshot_failures, lifecycle_failures, string_truncations, clock_failure,
fork_seen, inflight, unpublished, open_graphs, complete`.
`complete` requires exact provider, no dropped/unpublished/inflight/clock/
lifecycle/truncation/snapshot failure and all observed graph lifetimes freed.
For the explicit fork policy, `fork_seen` is descriptive: the ledger and its
completeness cover only the header PID and exclude descendants. A pre-exec child
only forwards and never records or publishes inherited data. Exec starts a fresh
owner PID, with its own independent report; it cannot append to the parent.
Old reports without the explicit policy retain their original strict fork gate;
no existing footer or captured failure is rewritten or upgraded.
`snapshot_failures` counts failed selected attempts once (including failed
publication), rather than counting a rejected copy again at shutdown.
Snapshot files are exclusive0600; write/publication failure leaves incomplete
output (or no valid footer). Readers must require a complete footer, exact PID
and canonical original provider identities, not only successful client exit.

Sink pixel-format option mutation is intentionally not interposed. Its NV12
constraint is pinned original Kodi source plus actual observed sink NV12 output,
not an observed av_opt_set_array call. No stronger configuration claim follows.
