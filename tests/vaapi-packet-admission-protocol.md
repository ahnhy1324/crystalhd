# OWN packet admission observer v1

This is metadata-only isolated diagnosis, excluded from ordinary installation,
stock compatibility and release qualification. It forwards each outer public
call exactly once, preserves arguments/result/errno and ownership, takes no
packet/frame reference, copies no payload and never injects EOF/retries.
Build against unchanged original format63.1.102/codec63.1.102/util61.1.102 headers.

Enable `CRYSTALHD_TEST_PACKET_ADMISSION=v1` and absolute
`CRYSTALHD_TEST_PACKET_ADMISSION_PREFIX`. Optional absolute provider variables
end `_FORMAT_PROVIDER`, `_CODEC_PROVIDER`, `_UTIL_PROVIDER`; defaults are
`/app/lib/libavformat.so.63.1.102`, `/app/lib/libavcodec.so.63.1.102` and
`/app/lib/libavutil.so.61.1.102`. Exclusive0600 output is `PREFIX-PID.jsonl`.

Header: `type:"header",schema:"crystalhd-packet-admission-v1",pid,
fork_policy:"PARENT_PID_ONLY_CHILD_FORWARD_ONLY",
observation_scope:"OUTERMOST_PUBLIC_CALLS_PARENT_PID",capacity:32768,
lifecycle_capacity:256,max_streams:1024,format_provider,codec_provider,
util_provider,format_version:4129126,codec_version:4129126,
util_version:3998054,provider_ok,h264_codec_id,video_media_type,vaapi_format,
discard_none,discard_default,discard_nonref`. Enum values come from the actual
compile headers, not a reader's guessed ABI. Every original hook/version symbol
must resolve to its family's exact canonical path/base/version before any public
object or URL string is read. Unversioned fallbacks only forward, never observe.

Events: `type:"event",seq` (zero-based call-entry reservation), `pid,tid,
begin_ns,end_ns,api,ret` (integer or null for void APIs), `errno_before,
errno_after,format_instance,codec_instance,codec_epoch,format_context,
codec_context,caller` (three descriptive hexadecimal pointers), `concurrent`
(boolean), `opened_before` (boolean), `unopened_free` (boolean),
`flush_old_epoch,flush_new_epoch` (otherwise0), `packet_null` (boolean),
`input_url,format_url` (null or copied bounded strings; open_input only),
`format,codec,stream,packet,frame` (objects below, otherwise null).
API strings are full names: `avformat_open_input`, `avformat_find_stream_info`,
`av_read_frame`, `avformat_close_input`, `avcodec_open2`, `avcodec_send_packet`,
`avcodec_receive_frame`, `avcodec_flush_buffers`, `avcodec_free_context`.

Successful opens establish monotonic family lifetime IDs starting1; repeated
open on an already-open address retains its ID and `opened_before:true`.
Failed opens have ID0 unless an already-open lifetime existed. Unknown data
calls invalidate evidence. NULL/unopened close/free record ID0 and
`unopened_free:true`, never invent a successful lifetime. Pointer reuse after
retirement receives a new ID. The lifecycle capacity bounds unique address
slots per family, not historical ID values; IDs can reach the event capacity.
Codec epoch starts1; flush records old epoch and
its incremented new epoch (event codec_epoch is the old epoch). Close/free
capture context metadata BEFORE destruction. Codec public state is copied at
serialized caller entry, not retrospectively. A second overlapping public call
on the same context marks `concurrent:true`, increments lifecycle_failures and
invalidates the receipt; original calls are never serialized or locked.

`format:{start_time,nb_streams,info_complete}` is public metadata at entry,
except successful open/read capture it after return. Successful read refreshes
format metadata with its packet/selected stream, including streams newly
discovered during that read. The lifetime must still be open with the same ID
and no overlapping call; otherwise no post-call context is dereferenced. `info_complete` becomes true
only after successful outer find_stream_info; its event shows the prior state.
Nested discovery decoder/read calls are intentionally excluded. An outer
format-open/read/close lifetime may legitimately skip find_stream_info (the
pinned original Kodi MPEG-TS path does); info_complete:false is descriptive and
must not classify its outer reads as discovery or remove them from the census. `input_url` is the open argument copied before return, `format_url` the
successful context URL copied after return; each max1023 bytes, nullable, never
an invented source identity. Missing/ambiguous URL prevents exact-source joins.

`codec:{codec_type,codec_id,width,height,pix_fmt,skip_frame,skip_idct,
skip_loop_filter,pkt_timebase:[num,den],thread_count,thread_type,
active_thread_type,has_b_frames}`. State is never changed by the observer.

`packet:{pts,dts,duration,pos,size,flags,stream_index,time_base:[num,den],
data_null,side_data_elems}` is
copied BEFORE send (also on failure), or AFTER nonnegative read. `packet_null`
is true only for an actual NULL send. `data_null` records only pointer nullness;
size0/non-null-data and side-data-only packets remain distinct from an
empty-drain-shaped nonnull packet. No size-only EOF inference is made.
No packet pointer/data/address/reference is an identity. A successful read's
`stream:{index,id,codec_type,codec_id,width,height,time_base:[num,den],
start_time,duration}` is read only after validating nonnegative packet index,
nb_streams<=1024, index<nb_streams, stream array/entry and codecpar nonnull.
Invalid stream association increments lifecycle_failures; packet metadata stays
an observation, never a repaired index or source-frame mapping.

`frame:{pts,best_effort_timestamp,duration,time_base:[num,den],format,width,
height,flags,va_surface}` appears only AFTER nonnegative receive. `va_surface`
is an integer copied from data[3]'s descriptive value only for actual VAAPI
format, otherwise null; no plane, pool or surface object is dereferenced.
Packet/frame/context/source clocks remain raw separate domains.

Footer: `type:"footer",events,attempted_events,dropped,lifecycle_failures,
string_truncations,clock_failure,fork_seen,inflight,unpublished,open_formats,
open_codecs,complete`. Complete requires exact providers, all successful
lifetimes retired, no overflow/truncation/lifecycle/clock/inflight/unpublished
failure and complete publication. `fork_seen` is descriptive: the owner-PID
ledger excludes children. A pre-exec child only forwards and never publishes
inherited data; exec creates a fresh PID-owned report. Shutdown closes the
observation-start gate before sampling active calls. Readers must require exact
owner PID/policy/providers and all complete gates, not merely client exit0.
Whole900 acceptance and exact per-packet/skip/VA attribution are separate from
valid metadata observation; aggregate counts alone prove neither.
