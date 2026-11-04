# Original-provider no-clock 900-picture diagnostic

This OWN client uses unchanged original App FFmpeg and a maintained VA backend.
The default mode tests source population without Kodi's renderer, media clock,
BWDIF or NONREF feedback. Optional `--bwdif` adds the original software graph
with separate unflushed and explicitly reference-flushed results.
Neither mode qualifies stock real-time playback, presentation,
independent pixel correctness or a release. Existing source-population FAILs
remain unchanged.

The fixed input contract is the retained 1920×1080 H.264 MPEG-TS fixture with
900 independently established pictures. The client discovers its video stream,
admits every selected nonempty packet unchanged, requires all three discard
fields to remain DEFAULT and rejects software fallback, corrupt frames and
nonzero decoder error flags. A successful report requires 900 video admissions,
900 VAAPI frame returns, 900 successful retained
frame readiness/downloads, input EOF, accepted explicit decoder drain and
decoder EOF. Accepted packet PTS and returned best-effort labels must each be
900 distinct raw integers with equal sets; bounded scalar sets check this
without a clock conversion or a retained pixel map. The external native trace must independently establish original
submission/materialization/ownership counts; a tool return is not that proof.
This client passes demux packets with their original stream time base. Kodi's
private timestamp conversion and per-packet option choices are not reproduced;
numeric packet/frame labels cannot be equated to prior Kodi labels by this run.

The public feed/drain and ownership pattern follows `vaapi-seek.cpp`. Eight
later returned frames precede synchronizing the oldest, with at most nine
retained frames and one reusable software NV12 download. After decoder EOF the
client explicitly frees the decoder before downloading its retained tail;
the AVFrames retain the surfaces/device. It changes no DIL or driver ownership
rule. Readiness has a finite 100ms budget. A readiness failure, 150s overall
deadline or tool-specific limit is retained as such, not an impossibility proof.
At most 4,096 demux packets and 900 selected frames are accepted. JSONL metadata
is streamed to a fresh mode0600 file; no 900-frame pixel collection is retained.

Only the root executor builds/runs this during the serialized hardware task.
Build the isolated binary outside the source worktree, against the audited
public-header ABI also used by the existing post-filter reference. The SDK
comes from unchanged `jc-kynesim/rpi-ffmpeg@12cc1f42`, not the App's build source:

```sh
SDK=/home/mache/.cache/crystalhd-v13-distribution-20261102/prefix-a/include
SOURCE=/home/mache/.cache/crystalhd-native900-client-20261104
BIN=/home/mache/.cache/crystalhd-native900-run-20261104/original-decode900
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$SDK" \
  "$SOURCE/tests/vaapi-original-decode900.cpp" -o "$BIN" -ldl -pthread
```

Like `vaapi-postfilter-reference.c`, this binary dynamically resolves the exact
original versioned providers; it does not link host FFmpeg. Run it in the
existing original-App/Platform namespace, with the original App read-only at
`/app`, the matching Platform at `/usr`, the private maintained VA/DIL paths,
the existing card/render-node bindings and a fresh writable payload directory.
The caller preserves original material/provider pins and records mapped images.
The source fixture may be mounted at `/fixture.mp4`; its suffix does not change
the detected MPEG-TS input contract. Inside that namespace:

```sh
/reference /fixture.mp4 /dev/dri/renderD128 /payload/decode900.jsonl
# Separate optional experiment, with a new report:
/reference /fixture.mp4 /dev/dri/renderD128 /payload/decode900-bwdif.jsonl --bwdif
```

Arguments are `INPUT VA_DEVICE FRESH_REPORT [--bwdif]`. Providers are fixed to
`/app/lib/libavformat.so.63.1.102`, `libavcodec.so.63.1.102`,
`libavutil.so.61.1.102`, and the namespace's
`/usr/lib/x86_64-linux-gnu/libva.so.2`. Each called API is resolved to its exact
provider; numeric runtime versions must match the audited header ABI. The
supplied App manifest instead declares canonical FFmpeg `n9.0.2` (commit
`946fcce07b6dcd0331c8cc609192aeff5e1924f8`) with its supplier's postproc/PGS
patches. SDK provenance and App provenance are separate: the 41-header closure
audit found no changed used structures, signatures or earlier enum values, but
unused fork API/inline helpers and appended RPI pixel formats are present in
the SDK. Its generated `avconfig.h` is not an independently obtained App build
header. Numeric ABI selection does not prove an identical build commit or
internal implementation. No external library is rebuilt, patched or replaced.

Every execution records the original provider's public `av_version_info()`.
BWDIF mode also records `avfilter_configuration()`; without that mode the
filter remains unloaded and configuration is null. These are measured build
metadata, distinct from the documented SDK source. The optional graph is dumped
once after configuration, before its first filter input. That is the actual
OWN graph on the selected original provider, not private Kodi graph access.

The optional graph also resolves exact `/app/lib/libavfilter.so.12.1.102`
versioned APIs. Its source is NV12 visible1920×1080, graph time_base1/1 and the
public source SAR (fallback1/1 if unspecified); its sink is NV12 and its filter
is exactly `bwdif=1:-1`. Graph threading remains its original auto default0,
with the effective configured thread count recorded. It is independent of
the decoder's thread_count1. The supplied 9.0.2 graph actually negotiates
`NV12 → auto_scale_1(yuv420p) → bwdif → auto_scale_0(NV12)`. The SDK fork's BWDIF
NV12 support is a different implementation; it does not establish direct NV12
support in the original App. Read the recorded graph rather than inferring it
from matching ABI numbers. This automatic conversion is not a claimed external
bug or an external replacement fix.

`av_hwframe_transfer_data` does not copy all properties. The client explicitly
uses `av_frame_copy_props` for the software download and filter input, preserving
public flags, SAR, colours, crop and error metadata. It copies active visible
Y/UV rows into a64-aligned input frame; coded1088 padding is not a pixel claim.
For this OWN graph, PTS/best-effort/DTS and duration are explicitly rescaled from
the public stream time base into AV_TIME_BASE and frame.time_base is1/1000000.
Graph time_base remains1/1 to match the original configuration. These are raw
diagnostic labels, not a physical graph clock or Kodi's private offset/rounding
recipe. This metadata join does not establish stock/private-frame equivalence.

After900 inputs, the unflushed checkpoint must have1798 outputs and no sinkEOF.
Only then does the OWN client call `av_buffersrc_close` with NOPTS, recording
reference-only sourceEOF separately;1800 outputs, sinkEOF and exactly two
best-effort labels per input are then required. Outputs must be progressive
(interlace flag cleared), valid NV12 and free of corruption/error flags.
Output metadata is streamed,
not retained as1800 images. No synthetic901st input is added. This reference
flush does not repair or qualify the original Kodi caller's absent filterEOF.
Original900 shortage and two held filter-tail outputs are separate questions.

The executor sends SIGTERM to its identity-pinned owned process on cancellation;
the handler requests normal decoder/frame/context cleanup. There is no SIGKILL
or implicit timeout subprocess. A blocked cleanup must retain the process and
resources under the existing handoff rule. Root verifies exit, native ledger,
kernel and full restored boundary separately. Whole-process CPU (all threads),
calling-thread CPU and wall time are separate raw metrics. Bounded cumulative
call counts/wall/main-CPU rows cover send, receive, finite VA readiness,
download, filter add, filter sink and explicit reference close. These are
inclusive public-call intervals; concurrent worker CPU and nested maintained
work prevent interpreting them as pure component CPU or proof of the stock
900-picture loss cause. Copies, source I/O, initialization and report writing
also contribute to total execution outside those call intervals. No CPU<=wall
clamp or inferred negative off-CPU metric is used. The unchanged frozen
no-filter0277883 result remains separate; this option requires its own execution.

Each `call_metric.rusage_thread` adds cumulative user/system CPU microseconds
and minor/major fault counts from Linux `getrusage(RUSAGE_THREAD)`. These
snapshots surround the existing clock measurements, leaving the clock endpoints
unchanged. Their wider window includes clock/query overhead and the public
call; it is not pure component CPU or allocator-only fault attribution.
The two `resource_usage` rows preserve start/end/delta for `RUSAGE_SELF` and
`RUSAGE_THREAD`, beginning before Providers and ending after client cleanup.
SELF covers all threads in this process, including filter/native workers;
THREAD covers the main calling thread. Neither includes launcher/controller
processes or child-process usage. API-specific THREAD measurements exclude
other worker-thread CPU even when their work occurs during the call.

`getrusage` CPU fields have microsecond representation/accounting granularity,
independent of the nanosecond CPU clocks; summed user/system deltas need not
equal clock CPU or the whole-window totals. Faults are cumulative counter
differences, not a byte count or proof of a particular copy/zeroing mechanism.
Instrumentation adds overhead, so paired comparisons use this same client
revision in both arms. Old0277883/a162 executions and earlier failures/source
attribution corrections retain their original identities and are not relabeled.
