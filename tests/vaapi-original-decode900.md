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
Build the isolated binary outside the source worktree, against the unchanged
12cc public headers used by the existing post-filter reference:

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
provider; FFmpeg runtime versions must match the original headers. No external
library is rebuilt or replaced.

The optional graph also resolves exact `/app/lib/libavfilter.so.12.1.102`
versioned APIs. Its source is NV12 visible1920×1080, graph time_base1/1 and the
public source SAR (fallback1/1 if unspecified); its sink is NV12 and its filter
is exactly `bwdif=1:-1`. Graph threading remains its original auto default0,
with the effective configured thread count recorded. It is independent of
the decoder's thread_count1. Original FFmpeg supports NV12 directly.

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
