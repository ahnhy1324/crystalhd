# CrystalHD GStreamer 1.x decoder

For ordinary file playback, use the [`crystalhd-play` guide](../../../USAGE.md#play-a-local-file).
This page is for building GStreamer pipelines, selecting decoder properties
and running component diagnostics. [Install the runtime](../../../README.md#quick-start)
before using the installed-plugin examples.

`crystalhddec` exposes BCM70012/BCM70015 decoding to current GStreamer
pipelines. It accepts parsed H.264, MPEG-2 and MPEG-4 Part 2, plus the
VC-1/WMV3 framing described below, and outputs YUY2 video frames.

This is the primary local-playback integration. Its validated BCM70015 subset
and known limits are recorded in the [hardware report](../../../HARDWARE-2026-09-13.md).
Device-free CI checks discovery, framing/lifecycle helpers, synthetic YUY2
playback and software audio/video controls; it does not decode through
CrystalHD or establish BCM70012 behavior.

Start with [build/basic use](#build-and-basic-use), then choose
[output scaling](#optional-bcm70015-output-scaling), [MPEG-4](#mpeg-4-part-2-input)
or [VC-1/WMV3](#vc-1-and-wmv3-input) only when needed. [Counted playback](#counted-h264-playback-and-replay)
and the later trace/PTS/finish-frame sections are separate validation tools.

## Build and basic use

For an installed build, inspect the system plugin first:

```sh
gst-inspect-1.0 crystalhddec
```

Check `Filename` under `Plugin Details`; it must name the system plugin rather
than a checkout. `crystalhd-check` reports that filename and the resolved
`libcrystalhd.so.3` together.

For a source-tree test, run from the repository root:

```sh
make -C linux_lib/libcrystalhd
make -C filters/gst/gst-plugin-1.0
make -C filters/gst/gst-plugin-1.0 check
```

Verify that the source plugin resolves the source library:

```sh
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
ldd filters/gst/gst-plugin-1.0/libgstcrystalhd.so
```

`libcrystalhd.so.3` should resolve into `linux_lib/libcrystalhd`. For installed
discovery, run `gst-inspect-1.0` without `GST_PLUGIN_PATH` or `LD_LIBRARY_PATH`
overrides, and check `ldd` on the installed plugin's reported filename.

Decode an H.264 file in an MP4 container from the source tree:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! videoconvert ! autovideosink
```

The plugin requests access units in Annex-B/byte-stream format from
`h264parse`, so MP4 AVC length prefixes are converted automatically during
caps negotiation.

## Optional BCM70015 output scaling

`scale-width` selects the decoder-coupled hardware output width. Zero is the
native-size default; an explicit value must be even, from 128 through 1918,
and no wider than the coded input. Progressive BCM70015 sessions are supported;
the 1.2 plugin also accepts H.264 interlaced input when the width
is a multiple of 16 and the card returns separate, complementary fields.
Scaled whole-Weave payloads and other interlaced codecs remain rejected.
The property may be set in NULL or READY and is reapplied
after a flush/reopen, but it cannot change while the decoder is PAUSED or
PLAYING. The plugin checks the geometry returned by the card and preserves the
source display aspect ratio in output caps, including widths whose derived
height has to be rounded.

For example, return 1280x720 from progressive 1920x1080 H.264:

```sh
gst-launch-1.0 -v filesrc location="/path/to/video.mp4" ! qtdemux ! h264parse ! \
  crystalhddec scale-width=1280 ! videoconvert ! autovideosink
```

For this input, the verbose output should show `crystalhddec` source caps with
`width=(int)1280`, `height=(int)720` and `pixel-aspect-ratio=(fraction)1/1`,
followed by continuing playback and EOS. Replace the path with your file.

For an explicitly CPU-enlarged buffer, stock GStreamer also offers
`videoscale method=nearest-neighbour` before the requested raw-video size.
In a synthetic 720→1080 UYVY stage comparison it used about 44% less CPU
than bilinear2, with harder/jagged edges; Catmull-Rom and Lanczos cost more.
This is a filter-stage result, not a playback-rate guarantee. The Player's
`--software-output-size` remains bilinear2; it has no filter-selection flag.
A separate 30s Xv/Pulse Player comparison also lowers whole-process CPU with
nearest, but retains frame/count deficits in both methods; it does not qualify
zero-loss 50p/60p or change the Player default. See the
[whole-path comparison](../../../HARDWARE-2026-09-13.md#ordinary-nv12-vpp-2026-10-25).
Omitting that option and letting the sink fit the smaller picture can avoid
CPU enlargement altogether. See the
[comparison and exact scope](../../../HARDWARE-2026-09-13.md#output-scaling-cost-2026-10-25).

For interlaced FHD H.264 with the matching 1.2 plugin, reduce the
card-to-host pixels first, then use GStreamer's CPU Bob at the returned size:

```sh
gst-launch-1.0 -v filesrc location="/path/to/video-1080i.mkv" ! matroskademux ! h264parse ! \
  crystalhddec scale-width=1280 ! deinterlace method=scalerbob fields=all ! \
  videoconvert ! autovideosink
```

The measured 1080i25 input becomes 720-line interlaced pictures before host
processing, then 50-PTS/s progressive Bob output. This is not hardware Bob;
[bounded headless measurements](../../../HARDWARE-2026-09-13.md#card-scaled-interlaced-h264-and-host-bob-2026-10-20)
do not establish displayed cadence or physical A/V synchronization.

To compare an explicit host-side resize back to a square-pixel Full-HD buffer:

```sh
gst-launch-1.0 -v filesrc location="/path/to/video.mp4" ! qtdemux ! h264parse ! \
  crystalhddec scale-width=1280 ! \
  videoscale method=bilinear2 n-threads=2 add-borders=true ! \
  'video/x-raw,format=YUY2,width=1920,height=1080,pixel-aspect-ratio=1/1' ! \
  videoconvert ! autovideosink
```

The first scale occurs on CrystalHD and reduces returned YUY2 traffic. The
second is ordinary GStreamer software processing and consumes host CPU; it
does not recover source detail. Width 1280 is a measured example rather than a
plugin default. Remove `scale-width` and the explicit `videoscale`/caps stage
in a new pipeline to return to native output. See the
[user guide](../../../USAGE.md#optional-bcm70015-output-scaling)
and [hardware measurements](../../../HARDWARE-2026-09-13.md#full-hd-input-with-decoder-coupled-scaling-2026-10-11).

## MPEG-4 Part 2 input

BCM70015 accepts parsed Simple and Advanced Simple levels 3/5. The validated
subset is progressive rectangular 8-bit 4:2:0 with H.263 quantization and no
sprites/GMC, quarter-pixel, data partitioning, RVLC or interlace. Advanced
Simple additionally requires disabled resynchronization markers. Codec data or
an in-band VOS/VOL must agree with caps and picture dimensions; unsupported or
ambiguous streams fail negotiation instead of being submitted as another
codec. BCM70012 is rejected for MPEG-4.

For MPEG-4 in an MP4 container:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 -q filesrc location=video.mp4 ! qtdemux ! \
  mpeg4videoparse ! crystalhddec ! videoconvert ! autovideosink
```

The parser must provide one complete VOP per buffer with `parsed=true` and
Simple/Advanced Simple profile and level fields. A successful pipeline only
proves that particular file; use the counted hardware probe for frame-count,
timestamp and EOS validation.

## VC-1 and WMV3 input

The decoder distinguishes WMV3 Simple/Main from VC-1 Advanced (`WVC1`). These
are the accepted `video/x-wmv,wmvversion=3` framing combinations:

| Format | Stream/header format | Input contract |
| --- | --- | --- |
| WMV3 | `asf` / `asf` | One complete picture per buffer; original four-byte STRUCT_C metadata in `codec_data`. Trailing encoder bytes, including six-byte AVI metadata, are ignored without changing the header. |
| WVC1 | `asf` / `asf` | Complete ASF picture packets; sequence/entry-point startcodes in `codec_data`, with or without the ASF binding byte. |
| WMV3 | `frame-layer` / `asf` | One complete Annex-L frame layer per buffer; its eight-byte wrapper is validated and stripped. |
| WVC1 | `bdu` or `bdu-frame` / `none` | Startcoded elementary stream with sequence/entry-point headers in-band; fields and slices are grouped with their picture. |

Ordinary `asfdemux` output omits `stream-format` and `header-format`; these
missing fields mean ASF packets. Missing `format` retains the legacy WMV3
interpretation. Explicit unsupported framing is rejected. Raw BDU assembly
has a 16 MiB buffered-input guard; do not push an entire larger file in one
buffer. All codecs additionally require each complete compressed picture,
injected metadata and conservative packet-header allowance to fit the
library's 1 MiB transmit ring. Oversized pictures are rejected before
submission, so the usable payload limit is slightly below 1 MiB and depends
on framing/metadata.
Sequence-layer/RCV container framing is not accepted directly. The optional
FFmpeg-demuxed probe below can read RCV and supply its complete picture packets.

For WMV3 in an ASF/WMV container, no additional parser is required:

```sh
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --signal=TERM 35s gst-launch-1.0 -q \
  filesrc location=wmv3.wmv ! asfdemux ! crystalhddec ! fakesink sync=false
```

For WMV3 in AVI, replace `asfdemux` with `avidemux`. `asfdemux` is supplied by
GStreamer's Ugly plugins.

For raw VC-1 Advanced, set the caps to the stream's actual dimensions and
frame rate:

```sh
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --signal=TERM 35s gst-launch-1.0 -q \
  filesrc location=input.vc1 ! \
  'video/x-wmv,wmvversion=3,format=WVC1,stream-format=bdu,header-format=none,width=176,height=144,framerate=25/1' ! \
  crystalhddec ! fakesink sync=false
```

Replace `width`, `height` and `framerate` with the values for the input. The
decoder groups raw BDUs, so `vc1parse` is not required in this pipeline.

An optional counted probe uses FFmpeg demuxing and GStreamer `appsrc`, checks
exact frame count, YUY2 dimensions, valid timestamp ordering, and EOS, and
prints a hash of the visible pixels:

```sh
make -C filters/gst/gst-plugin-1.0 codec-playback-test
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
timeout --signal=TERM 35s \
  filters/gst/gst-plugin-1.0/gstreamer-codec-playback-test \
  /path/to/input.wmv EXPECTED_FRAMES
```

Building this optional probe needs the `libavformat`, `libavcodec`, `libavutil`
and GStreamer app development packages; they are not dependencies of the
plugin build. When pkg-config finds them, `make check` runs its hardware-free
`--self-test`. Feeding and EOS share a 25-second deadline and a bounded queue;
retain the outer timeout because userspace cannot bound a blocked driver
close. If TERM does not retire the device owner, preserve its PID, logs and
resources, stop new media and follow normal recovery; do not use SIGKILL or
unload underneath it. The printed hash is not a software-decoder comparison.

## Counted H.264 playback and replay

Generate reproducible Baseline/Main/High MP4 fixtures with FFmpeg and libx264:

```sh
sh tests/generate-h264-samples.sh /tmp/crystalhd-samples
# Optional Full HD set: 1920x1080, 30 fps, 180 frames per profile.
sh tests/generate-h264-samples.sh /tmp/crystalhd-fhd-samples 1920x1080
```

The generator refuses to overwrite existing samples and prints each profile,
frame count and SHA-256. Run the harness separately for each generated file.

The harness compares YUY2 output with FFprobe's decoded frame count, checks
dimensions and timestamp order, requires EOS and scans new kernel messages. It
also verifies module/source identity and the selected source plugin:

```sh
./tests/gstreamer-hardware.sh /path/to/video.mp4 2
```

The command also accepts Annex-B `.h264` streams. MP4 timestamps must be
present and ordered; elementary-stream timestamps are checked when available.
The default timeout is 120 seconds per run and can be changed with
`CRYSTALHD_TEST_TIMEOUT`. `ffprobe`, `h264parse` and `qtdemux` are required.
Other codecs/profiles and known interlaced input are rejected by this harness.
It loads and later unloads the source-built module only when no matching module
was already loaded; see [module identity](../../../BRINGUP.md#device-access-and-module-identity).

For one nondisplay decode without the frame-count harness:

```sh
GST_PLUGIN_PATH=$PWD/filters/gst/gst-plugin-1.0 \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
gst-launch-1.0 -q filesrc location=video.mp4 ! qtdemux ! h264parse ! \
  crystalhddec ! fakesink sync=false
```

A successful launch alone does not prove that every frame drained; use the
counted harness for that claim. Final drain has a 10-second no-progress
watchdog, renewed by successful delivery and suspended while stably paused.
See [the bring-up guide](../../../BRINGUP.md) for failure diagnosis.

The output worker uses negotiated kernel RX readiness and a private control wake
when available. Older libraries/drivers, or an unavailable control descriptor,
retain timed status checks. EOS/recovery and a READY indication without output
still receive bounded state probes. Flush and close wake the worker and retire
its descriptor borrower before replacing the session. BCM70012 retains its
metadata peek. This requires no new option and does not change QoS or deadlines;
[bounded measurements](../../../HARDWARE-2026-09-13.md#gst-output-readiness-2026-10-25)
separate fewer status queries from display latency or throughput claims.

MPEG-2 pictures with positively identified missing-reference prediction or
missing coded frame rows are skipped before hardware submission with a warning.
Inconclusive syntax keeps its existing decoding path. Skips are not decoded
output: EOS still requires every submitted picture to finish.

To exercise GStreamer's flushing seek in the same playback session:

```sh
CRYSTALHD_TEST_SEEK=1 ./tests/gstreamer-hardware.sh /path/to/video.mp4 2
```

Each iteration drains the file, seeks to zero with `GST_SEEK_FLAG_FLUSH`, and
requires the same frame count and visible-YUY2 SHA-256 on replay. This covers
replay after EOS, not arbitrary mid-playback seeks or resolution changes. The
hash compares two hardware runs, not hardware against a software decoder.

## Retained Player scaling observations

These source/host-specific measurements support the
[user scaling recipe](../../../USAGE.md#optional-bcm70015-output-scaling);
they are not settings required for first playback.

With the reference host's qualified L0s workaround active, the H.264 High
stream rose from about 46 to 65 unpaced frames/s with 1280-wide card output.
Software enlargement back to Full HD retained
about 64 frames/s and used about 84% of one CPU core. This is useful headroom
for the measured input, not native 1080p60 detail or a displayed-frame-rate
guarantee. Other codecs and hosts have different limits; exact throughput and
quality results are in the
[hardware report](../../../HARDWARE-2026-09-13.md#full-hd-input-with-decoder-coupled-scaling-2026-10-11).

That display-fitting path is the lower-cost choice when an exact Full-HD raw
buffer is not required. A five-minute H.264 1080i25 development baseline used
the source-pinned development player with `--scale-width 1280` alone: the card
returned
1280x720, Xv accepted 1280x720 UYVY at 50 fields/s, and all 15,000 fields were
rendered without a QoS drop. The player used 37.02% of one CPU core. Matched
explicit 1920x1080 enlargement runs used about 84.17% and lost 13 and 10
fields. Xv's display interpolation is adaptor-dependent and is not claimed to
be pixel-equivalent to the player's explicit method-4 software enlargement.
The separate installed-RC2 [30-minute application run](../../../HARDWARE-2026-09-13.md#120-rc2-qualification--2026-10-22)
completed 45,000 originals and 90,000 rendered fields with zero drop/QoS,
natural EOS and mean CPU 37.16% of one core. Neither run measures physical
scanout cadence.

With GStreamer **1.28.2**, the hardware-scaled Player path, with or without an
explicit host output size, works around a
confirmed default LINEAR deinterlacer error for YUY2 25-picture/s input and
50-field/s output. It bypasses that element's entire local QoS cache while
forwarding unchanged feedback upstream; decoder, resize/conversion and sink
QoS remain active. Other versions, rates and methods retain stock behavior.
This is not a guarantee of lossless FHD50 presentation; see the
[measured limits](../../../HARDWARE-2026-09-13.md#card720--bob--fhd-physical-playback--2026-10-21).

## Player video-stage trace

This optional 1.3 Player diagnostic is separate from ordinary playback. To investigate a missing frame or field, request a new JSON output file:

```sh
crystalhd-play --trace-frames /tmp/playback-trace.json "/path/to/video.mp4"
crystalhd-play --software --trace-frames /tmp/software-trace.json "/path/to/video.mp4"
```

This needs the matching native GStreamer helper even in software mode. The
file must not exist; it is created with mode `0600`. The normal Player does
not enable these probes. While tracing, native callbacks record the decoder
source, deinterlacer source when present, and leaf video-sink input. With
`--software-output-size`, it also records the Player's `output-scale` source
and, when prepared, its `output-pack` source. These identify the Player's
own scaler and early packing converter; other intervening transforms are
not individually observed. There is no additional per-buffer Python callback,
pixel readback or file writing in
the streaming thread. The JSON is written after playback teardown.

The default capacity is 4,096 records across all observed stages, events and
controls. `--trace-limit 16384` raises it to the maximum. Retention keeps the
first records; `dropped` declares truncation while attachment counters continue.
Every observed pad also records propagated QoS events. A 30-second 25i/50p
five-stage trace can use about 14,250 records; use the maximum and check that
`truncated` is false before comparing complete PTS sets. This is an example,
not a guarantee that every 30-second input fits.
This top-level `dropped` counts omitted trace records; it is separate from
the nested video-sink statistics' dropped-buffer count.
Attachment history is also limited to 64 entries. Rejected attachments and
unobserved stages mean incomplete coverage. Truncation, missing PTS, drops and
an absent deinterlacer do not become new playback failure gates. An unavailable
helper or output-file error fails the explicitly requested trace operation.

Add `--trace-window` to keep recent records in the same fixed capacity, then
freeze after the first timed video QoS bus message with positive jitter and a
known positive buffer-drop counter, then a quarter-capacity of following records
(at least one).
Propagated QoS events, untimed decoder headers and audio/time/unknown counters
do not trigger capture. Timestamp and running-time zero remain valid.
Window records include the emitting Linux `thread_id` in the caller's PID
namespace (`null` when unavailable), not internal scaler-worker identity.
First-N mode performs no additional thread-ID lookup.
Without a trigger, final recent records are exported. Playback and cumulative
counters continue after the window freezes. An older helper without window
support fails this explicit request before creating a report.

`retention: "window"` declares this mode. `window.complete` means the following
record budget was collected, not a complete picture census. EOS/stop before
that budget leaves it false. `window_context` preserves known CAPS/segment/flush
state before the earliest retained record; unknown context stays explicit.
Original order, ordinals and generations are preserved. Global `dropped` and
`truncated` include intentionally overwritten/omitted history, not lost video.
Do not count records outside the retained window as missing frames or subtract
window counts from cumulative attachment/sink counters. `player.loss_trigger`
retains the original bus counter snapshot and sequence number, not a unique
lost-picture identity. Rejected triggers declare incomplete capture.

Each attachment has independent buffer ordinals, raw video-buffer flags,
PTS/DTS/duration and stream/segment/flush generations. Timestamp zero is valid;
unknown times are JSON `null`. Running time is recorded only for a valid TIME
segment. Pause/resume and seek requests are annotations; observed native
events determine media generations. Compare the separate stage records without
assuming PTS uniquely identifies a source picture or a deinterlacer's fields.

`buffers` counts individual BUFFER callbacks. Buffer lists have separate
`buffer_lists`, `list_items` and `list_missing_pts` counters, with member records
named `buffer-list-item`. GStreamer's list fallback can expose both a list and
individual buffers at the same pad. `buffer_list_ambiguity` marks possible
overlap whenever any list is observed, not proof that a
particular list was duplicated. Do not add the two counters into a frame
census. List positions use `list_ordinal` and `list_index`; ordinary `ordinal`
continues to count individual BUFFER callbacks. The trace observes stream
start, CAPS, segment, flush, EOS and QoS events as well as attach/detach/stop
administration. All retained record types share the capacity limit.

Sink input records mean delivery to that pad. Sink statistics remain snapshots
since their last reset. Neither establishes actual screen presentation,
cadence, unique displayed pictures or physical A/V synchronization. Attached
candidates without buffers are not confirmed active renderers. The report
does not contain the media path or URL and does not select a different decoder.

## Optional whole-run pad PTS summary

The player's opt-in `--trace-frames FILE` helper includes a per-attachment
`pts_summary` version 1 in its schema 1 JSON. The constant-space summary covers
every observed ordinary buffer, including after first-N truncation, rolling
overwrite or post-trigger detail freeze. CAPS and segment state also continue
updating. Qualification covers fixed 25/50 Hz PTS metadata in one TIME segment
with rate/applied-rate 1, known canonical durations, complete timing and EOS.
Unknown/VFR timing, buffer-list overlap, changed CAPS, further segments, flush,
helper-annotated seek requests, cadence violations and arithmetic/counter
overflow prevent qualification;
identical CAPS repeats are idempotent. The Player annotates its own seek
requests; raw upstream SEEK events from an unrelated client are not a separate
seek observer. Gap counters describe observed timestamp
discontinuities and do not establish physical frame loss.

Audit a completed Player receipt against independently frozen source counts and
PTS anchors with `python3 -B tests/stage-pts-audit.py trace.json expected.json`
from the repository root. The expectation has `schema: 1` and an `attachments`
array containing every observed pad's `stage`, `token`, expected `buffers`,
`first_pts` and `period_ns`. The reader requires matching attachment coverage,
EOS, successful Player completion and all qualification fields; it does not
derive expectations from the retained detail ring or summary. `--native-only`
is for CPU controls without Player completion evidence. Older receipts without
the optional summary remain readable JSON and cannot establish this whole-run
claim. The summary and auditor measure no pixels, physical presentation or A/V
timing, and do not prove unique picture identity or source provenance.

### Bounded segment and flush epochs

Each attachment also contains an additive `pts_epochs` version 1 ledger. The
trace's schema 1 and the original `pts_summary` version 1 remain unchanged;
the original single-segment audit still rejects seek/flush observations.
The new ledger keeps at most 16 segment rows per attachment, independently of
the detail ring. Each row freezes its stream/segment/flush generations, CAPS
and TIME segment, first/last ordinary-buffer ordinals, PTS and running times,
cadence counters and closing boundary. No video payload references are kept.

Use expectation `schema: 2` with the existing `stage-pts-audit.py` command to
audit all rows. Each expected attachment supplies `stage`, `token`,
`seek_requests`, `seek_failures` and an ordered `epochs` array. Every expected
epoch supplies its generations, source `buffers`, `first_pts`, `period_ns`,
`end_reason`, `segment` and `caps`. Segment fields are `format`, `rate`,
`applied_rate`, `start`, `stop`, `time`, `base`, `offset`; CAPS signature fields
are `format`, `width`, `height`, `fps_n`, `fps_d`, `interlace_mode`, `field_order`,
`raw_video`, `video_info_valid`. An expected segment `seqnum` is optional.
Freeze these expectations from the producer/source and intended controls,
before inspecting the observed ledger. The reader never constructs expected
epochs or counts from trace rows or retained detail records.

Qualification is deliberately bounded to unchanged fixed raw-video CAPS at
25/50 Hz and forward TIME segments with rate/applied-rate 1. Positive cases
include new segments, paired flushes and repeated EOS followed by a flush and
new segment. The first DISCONT buffer in a fresh segment is permitted. A
Player seek annotation alone does not establish a boundary: a successful
flushing seek needs an observed flush pair and subsequent segment; a matching
rejected seek cancels the pending annotation. This is boundary observation,
not proof that a requested seek position was reached. Unrelated clients' raw
upstream SEEK events are not a separate seek observer.

The [GStreamer event model](https://gstreamer.freedesktop.org/documentation/additional/design/events.html)
defines flush and segment boundaries. This ledger qualifies a narrower
profile: after EOS, another row requires a flush pair even if a new stream
starts. One stream change and one flush may occur together between segments;
unmatched/repeated barriers and unresolved seeks fail qualification. Empty
rows, buffers without context, mid-row DISCONT, GAP, unknown timing, cadence
defects, changed CAPS/rates, arithmetic/counter overflow and more than 16 rows
also fail. Earlier rows remain visible on overflow and cannot certify the
whole attachment. A final EOS and complete Player completion are required;
`--native-only` remains a CPU-control exception to the Player requirement.

Counts and PTS endpoints are checked independently for every expected epoch,
including backward seeks that repeat PTS from an earlier epoch. Whole-epoch
omission, changed segment identity and compensating count errors cannot pass
through an equal aggregate total. Helper seek/failure broadcasts are counted
once, using the independent attachment expectations rather than summing pads.
The auditor also checks that reserved boundaries and STOP leave enough global
event slots for the required buffers and context events; this is necessary
metadata consistency, not reconstruction of every unretained event. These are
pad metadata observations; rendered
pixels, physical presentation, A/V drift and buffer ownership need their own
evidence.

The [bounded CPU receipt](../../../tests/fixtures/issue513/pts-epochs-20261101.json)
records 21 native groups/113 snapshots in ordinary and ASan/UBSan/leak runs,
37 independent reader controls, unchanged projections of the older 65
snapshots and the preserved initial harness failure.

## Optional finish-frame diagnostic

Set `crystalhddec`'s `finish-trace-limit` before PLAYING, then request its
`finish-trace-snapshot` action after EOS or normal Stop. For example, with an
existing Python GObject decoder instance:

```python
decoder.set_property("finish-trace-limit", 8192)  # NULL or READY only
# Run playback, then reach EOS or stop normally.
snapshot = json.loads(decoder.emit("finish-trace-snapshot"))
```

The default limit `0` leaves the per-frame path with a pointer branch and no
added diagnostic clock, TID, mutex or allocation calls. A nonzero limit
preallocates at most 8192 spans. Each retained span samples CLOCK_MONOTONIC
and the caller-namespace Linux TID twice and uses bounded collector locking.
The return timestamp is sampled before the collector lock and the decoder's
outer stream-lock reacquisition. This observation has overhead; no runtime
timing qualification is implied by the CPU tests.

Schema 1 retains the first admitted spans, with `observed`, `retained`,
`omitted`, `inflight` and counter-saturation metadata. `inflight_scope` is
`retained_records_only`: an omitted call can still be blocked when `inflight`
is zero. The collector does not guarantee
pretrigger or late-run coverage. The configured collector survives normal
Stop; replace the property in NULL/READY to start a fresh collector. Snapshots
allocate a capacity-sized copy before locking, copy records under the
collector mutex and serialize after unlocking. A
snapshot during playback can add contention; incomplete calls have null
return/flow fields, and unavailable clocks, TIDs or PTS remain null.

Records preserve call ID, process-local decoder instance, generation,
recovery epoch, system frame number, PTS/duration passed to the real
`gst_video_decoder_finish_frame()` call and its actual flow return. The frame
is never read after that transfer-full call. PTS zero, repeated PTS and
unknown PTS remain distinct. These values do not independently prove original
source PTS, process/TID birth, GPU ownership or presentation. `GST_FLOW_OK`
can include QoS, clipping or decode-only suppression and does not prove a
decoder-src buffer. Pair timing with independently qualified stage/wait
coverage; elapsed wall time does not identify exclusive CPU, I/O or a cause.
Own noinline `gst_crystalhd_finish_trace_enter/leave` symbols are available
for optional probes without changing external libraries.

## Local player and in-flight controls

`scripts/crystalhd-play` provides explicit hardware/software local-file
playback; see the [user guide](../../../USAGE.md#play-a-local-file). Hardware
mode never silently falls back to software.

The 1.3.0-rc.2 source and current development builds can select the separate
[completion-controlled Xv output](../xv-output/README.md) with
`--video-output xv-async`. Its plugin is built separately; ordinary output
selection and published 1.2 remain unchanged.

The separate controls probe verifies barcode pixels and timestamps while
pausing, resuming, seeking forward/backward and changing rate to 0.5x/2x/1x:

```sh
make -C filters/gst/gst-plugin-1.0 gstreamer-controls-test
sh tests/generate-browser-sample.sh /tmp/crystalhd-controls.mp4 --av-360p
timeout --kill-after=10 90 \
  filters/gst/gst-plugin-1.0/gstreamer-controls-test \
  /tmp/crystalhd-controls.mp4 --software --audio --timeout 75
```

For the numbered 720p hardware check, first verify the loaded module and stop
other CrystalHD clients. This needs FFmpeg with libx264/AAC encoding and
GStreamer's H.264/AAC plugins:

```sh
crystalhd_sample_dir=$(mktemp -d)
sh tests/generate-browser-sample.sh "$crystalhd_sample_dir/av720.mp4" --av-720p
GST_PLUGIN_PATH="$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
GST_REGISTRY="$crystalhd_sample_dir/registry.bin" \
timeout --signal=TERM 120s \
  filters/gst/gst-plugin-1.0/gstreamer-controls-test \
  "$crystalhd_sample_dir/av720.mp4" --audio --timeout 90
```

The check includes a complete 360-picture replay and rejects missing or
out-of-order identities, incorrect seek/rate progress, video lateness above
250 ms or sampled A/V interval skew above 100 ms. Flushing seeks recreate the
device. Retain the external timeout because a library or driver call can block
beyond the in-process watchdog.

`--expect-geometry WIDTHxHEIGHT` requires that exact decoded size on every
picture. `--skip-2x` runs pause/resume, both seeks, 0.5x and restored 1x, then
prints `2x=skipped`; it is useful for recording those controls separately but
is not a 2x or complete all-rate pass. The generated fixture also carries a
white marker through its top rows, so the probe rejects a full-width black-row
decode defect in rows 0 through 7.

`--sustain SECONDS` selects continuous 1x playback. It requires a fixture whose
duration is a multiple of 12 seconds, exact 30-fps timestamps, the repeated
360-frame barcode sequence and complete EOS. The optional audio check verifies
clock bounds and overlap with the final video interval, not sample-exact audio
duration. These tests use clocked test sinks and do not prove visible or
audible presentation; see the hardware report for measured runs.
