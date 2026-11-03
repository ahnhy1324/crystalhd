# Optional Xv output

Development `crystalhdxvsink` is a separately selected X11 output for
GStreamer. It uses a three-request SHM completion ring and a six-buffer
negotiated pool. It does not replace GStreamer's installed plugins or change
`crystalhd-play`'s default output. Hardware decoding still uses `crystalhddec`;
the output plugin itself works without a CrystalHD card.

Use this page to evaluate the optional output in a matching source build.
For first playback with the normal installed output, use the
[Player guide](../../../USAGE.md#play-a-local-file).

The optional FIFO output is included in the 1.3.0-rc.2 source; current master
adds the later latest-frame and timing experiments described below. Frozen RC2
assets are not updated by those later results. Read [build/use](#build-and-input-contract),
[latest-frame policy](#latest-frame-policy), [timing diagnostics](#timing-diagnostics)
and [validation](#validation-and-retained-qualification) separately. Dated
qualification and test counts retain the source scope of each receipt.

## Build and input contract

Build with the GStreamer video development package and `xcb`, `xcb-shm`,
`xcb-xv`, `xau` and `xdmcp` development packages available to `pkg-config`.
From the source root:

```sh
make -C filters/gst/xv-output
repo_root=$(pwd -P)
GST_PLUGIN_PATH="$repo_root/filters/gst/xv-output${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}" \
  python3 "$repo_root/scripts/crystalhd-player.py" \
    --hardware --video-output xv-async "/absolute/path/video.mkv"
```

This needs a working decoder installation (see the
[decoder build instructions](../gst-plugin-1.0/README.md)) and a local Unix
`DISPLAY` with MIT-SHM, a free UYVY Xv port and the relevant X11 authorization.
The Xorg display's normal session supplies `DISPLAY` and, when needed,
`XAUTHORITY`. The optional
plugin is built separately and is not installed by the normal runtime target.
Use the development Player script above; published 1.2 does not have this flag.
`--video-output auto` restores ordinary sink selection. A missing or unsupported
explicit output reports an error rather than silently choosing another sink.

The current input is progressive, limited-range BT.601/BT.709 UYVY. Playback
uses the ordinary GStreamer deinterlacer when necessary; this is not hardware
deinterlacing. `--weave` requires ordinary output. BT.709 additionally requires
the adaptor's supported color-space attribute.
Unspecified colorimetry keeps GStreamer's ordinary interpretation rather than
forcing a BT.601 conversion during format negotiation. Explicit unsupported
matrix/range requests fail during caps admission, before image allocation.
A caller can supply a window through `GstVideoOverlay` before starting.
Exposure events, resize and `GstVideoOverlay.expose()` redraw the retained
last picture while paused, including after EOS. This preserves its pixels
and PTS and does not count as a new video frame. A live handle change accepts
a mapped InputOutput window with the current window's screen, depth and visual. It
validates the target, waits for old-window SHM completion within the existing
deadline, then retains the pool, GC, source epoch and monotonic image
generations while repainting the retained source on the new window. The caller
continues owning its external windows. An invalid target posts a GStreamer
warning and keeps the current window; changing an external live window to
handle zero is rejected. A repeated current handle is idempotent.

The optional writable `port` property selects an exact UYVY Xv port. Before
startup, zero (the default) selects the first free compatible port; a nonzero
request fails if that exact port is unavailable. In PAUSED/PLAYING, a nonzero
request acquires the candidate first, checks its visual/color selector and
exact UYVY layouts for current caps, every image and every proposed pool
(including pools with no allocated images), then drains the old SHM requests
before restoring and releasing the old port. It keeps the window, GC, pool,
source epoch and retained picture and repaints on the new port. A rejected
busy/missing/incompatible candidate posts a warning and preserves the active
port and preferred selection. Transport/deadline failures retain the ordinary
failure/retirement behavior. Selecting the current port is idempotent; setting
zero live clears the next-start preference while keeping the active port.
For example, an embedding application can use `g_object_set(sink, "port",
port_id, NULL)`. The Player's default output selection is unchanged.

## Latest-frame policy

The experimental `latest-frame` Boolean defaults to `false` (FIFO). Set it
before starting the sink, for example `g_object_set(sink, "latest-frame",
TRUE, NULL)`; an active sink rejects changes with a warning. When the ring
is busy, this mode retains at most one unsent source and replaces that
pending source with newer input. Submitted images remain protected until
their matching completion or proven terminal retirement. The completion
reader submits the pending source with its original pixels and PTS, without
waiting for ring space or allocating images on that reader. Ordinary copied
input prepares compatible storage on the producer when needed.

EOS, CAPS and SEGMENT drain the pending source as well as submitted requests.
With an unsent source, the aggregate drain permits two one-second stages
(old requests, then the latest request); each individual request still has
its original one-second completion limit. Temporary PAUSED unlock preserves
the pending source; a real flush, output failure or stop cancels it. A live
window/port handoff drains old requests before dispatching that source to the
new target. This arm deliberately replaces unsent video and remains separate
from FIFO. The Player selects it with `--video-output xv-async
--xv-latest-frame`; it has no managed configuration key. An older plugin
without this property reports an error when the flag is selected. Ordinary
FIFO playback remains compatible with that plugin. The Player's before-close
summary reports intentional replacements separately from BaseSink QoS drops.

## Ownership and statistics

The renderer holds one last source buffer independently of BaseSink's
`enable-last-sample` setting. For negotiated pool memory, both SHM completion
and the final Gst owner must retire before that storage is writable again.
Copied input keeps its original source and repaints through a free SHM copy.
Redraw requests coalesce
and never block the completion reader waiting for ring space. A real flush,
new caps/segment, output error or stop discards the old picture. Temporary
BaseSink unlock during an ordinary pause preserves it. Resize/exposure also
repaints the black bars around the video.

The negotiated pool avoids an extra sink row copy when upstream uses its
actual memory layout. Other ordinary buffers take a bounded row-copy path.
Prior conversion, deinterlacing, scaling and X-server work still cost CPU/GPU
time. This does not establish zero-copy display, better cadence or physical
A/V synchronization.

Each sink owns its connection and image registry. Reuse waits for both the
matching SHM completion and final Gst ownership return. EOS drains submitted
requests. Flush/CAPS changes cannot reassign pending requests to a new picture.
Output errors return through GStreamer; disconnect retirement is distinct from
successful completion. Unproved detach retains the mappings for a reader-owned
reaper, with at most four live/quarantined contexts and bounded image storage.
It neither aborts the application nor guesses an ACK. The ordinary decoder
close path remains the application's normal pipeline teardown.

The read-only `xv-stats` structure reports submissions, ACKs, terminal
retirement, copied bytes and image ownership. `video-submitted` and
`redraw-submitted` separate normal video calls from repaint requests;
`submitted` counts both, including requests whose transport flush failed.
`retained-picture` reports whether the last source is held. Video calls include preroll when
the inherited `show-preroll-frame` property is enabled: twelve video frames
can generate thirteen requests. ACKs measure X-server SHM retirement, **not
unique video frames or physical scanout**. After stop the property retains a
stop-time snapshot; later quarantine cleanup is not a live snapshot.
`active-port` reports the acquired port ID; `port-switches` counts committed
live handoffs and `port-rejections` counts rejected live nonzero requests.
The readable `port` property reports the preferred selection rather than an
automatically acquired ID.
`latest-frame` and `latest-pending` report this arm's state. `latest-queued`
counts entries into the pending slot; `latest-replaced` counts intentional
replacement of that slot; `latest-submitted` counts requests created from it;
`latest-cancelled` counts unsent cancellation. The ledger is
`queued = replaced + submitted + cancelled + pending` (pending is zero or one).
Request creation includes a subsequent transport-flush failure, so these
counters do not prove visible or unique frames. Replacements are separate
from completion errors and BaseSink QoS drops.

## Timing diagnostics

The optional `observe-timing` Boolean defaults to `false`. Set it in NULL/READY
before output starts; active changes warn and are ignored. With observation
enabled, `xv-stats` additionally carries these local monotonic observations:

| Fields | Sample boundary |
| --- | --- |
| `ack-receipt-count`, `ack-receipt-us-sum`, `ack-receipt-us-max` | Flight request creation after pixel preparation and before flush, through the reader processing its matching ACK. Includes preroll and redraw requests; late/error ACKs and terminal retirement are excluded. |
| `latest-residence-count`, `latest-residence-us-sum`, `latest-residence-us-max` | Publication of the final pending original through creation of its actual request, counted only after a successful flush. Replacing a pending source starts a fresh timestamp; replaced/cancelled sources and failed flushes produce no residence sample. |
| `latest-pending-age-us` | Age of the currently pending original at the statistics query; zero when no source is pending or timing is disabled. |

The `observe-timing` statistics field records the context's selected mode.
Timing disabled skips additional clock reads and aggregate updates; its timing
counters remain zero. Aggregates accumulate for one context and survive in the
stop snapshot; a new context resets them. Count and sum saturate rather than
wrap, and sticky `timing-overflow` declares that the aggregate is incomplete.
Do not compute a reliable mean after overflow. Count/sum/max support only a
mean and maximum, not p95/p99 or a latency distribution.

ACK receipt includes local scheduling and reader delay. It does not identify
the server's read interval, GPU completion or physical scanout. Earlier decoder
work, producer ring-space wait and preparation before flight creation are
outside that interval. Pending residence includes any PAUSED or handoff wait
preserved with that source; it
is not PTS lateness. Because `latest-submitted` includes request creation even
when its flush fails, the successful residence count can be smaller. Interpret
these observations with failures, pending requests, terminal retirement and
intentional replacement counters. There is no ordinary Player CLI option or
saved setting for this diagnostic.

## Validation and retained qualification

```sh
# Device-free checks. The second command opens temporary X11 windows.
make -C filters/gst/xv-output check
make -C filters/gst/xv-output check-x11
```

The twelve `/xv/x11/latest-*` controls are included in `check-x11`. They use
the same eight distinct source IDs/PTS with held real ACKs for FIFO and latest
arms, verify complete active source rasters and final RGB pixels, and cover
copied/pooled/mixed memory, window/port/CAPS changes, actual pipeline EOS,
PAUSED/flush-seek, submit failure, pooled stop and missing ACK. An explicit
duplicate final reference request is counted as a reference, not a new
unique picture. The [current-host receipt](../../../tests/fixtures/issue513/latest-frame-20261103.json)
records the executed paths and original failed attempts. This bounded synthetic
qualification establishes no latency/CPU improvement, card-decoder/A/V result,
physical scanout or endurance acceptance.

The separate [current-card comparison](../../../tests/fixtures/issue513/latest-card-abba-20261103.json)
records six30s H264/PCM sessions on BCM70015/module13 and Intel965GM: the
initial FIFO/latest pair followed by FIFO/latest/latest/FIFO. Every session
decodes750 original pictures, renders1500 output buffers, reaches natural
EOS with actual Pulse audio/clock and returns all image/module/PM ownership.
The matched ABBA process CPU observations are40.16% FIFO and40.23% latest
(one-core units, two samples each). No intentional replacement occurs;
replacement behavior and a CPU/latency benefit are not demonstrated on this
noncongested route. FIFO remains the default. The logs detect no A/V drift,
accepted for this exact route; sampled clock identity is not a measured offset.

The explicit30s harness uses a fixed640x360 mapped area and the current
Player's scale1280/native output path. Build the own decoder/DIL and Xv plugin,
select their process-local paths as above, and supply a known750-picture
H264/PCM fixture in the current X11/Pulse session:

```sh
python3 -B tests/gst-xv-output-bench.py /absolute/dialogue-30s.mkv \
  --mode fifo --expected-pictures 750 \
  --expected-plugin "$PWD/filters/gst/xv-output/libgstcrystalhdxv.so" \
  --results /absolute/new-fifo.json
```

Use `--mode latest` and a different new result file for the other arm.
It records CPU, before/after-NULL Xv ownership, intentional replacements,
QoS and sampled Pulse-clock identity with45s/75C guards. Pre-NULL pending is
diagnostic; safe terminal retirement counts alongside ACKs after NULL.
It leaves timing observation off by default. To record the local aggregates
above, use `--observe-timing` with a rebuilt plugin and a new results file:

```sh
python3 -B tests/gst-xv-output-bench.py /absolute/dialogue-30s.mkv \
  --mode latest --observe-timing --expected-pictures 750 \
  --expected-plugin "$PWD/filters/gst/xv-output/libgstcrystalhdxv.so" \
  --results /absolute/new-latest-timing.json
```

The benchmark checks the Boolean property's default and applies the requested
mode before startup, then preserves the timing fields in its before/after-NULL
snapshots. Requested observations must have coherent sample counts and sums,
ACK samples matching acknowledged requests, and no aggregate overflow; an
invalid measurement fails the benchmark without changing playback policy.
An older plugin without the property is compatible when observation
is omitted and fails explicitly when it is requested. No source pixels, unique
displayed pictures, physical scanout or endurance are measured. Timing-enabled
runs must keep their exact implementation and observation mode separate from
the retained six-session comparison, which contains no timing observations.
Those older records remain unchanged. The retained raw logs include an
unrelated dbind accessibility-bus warning at GUI startup.

The [November3 timing record](../../../tests/fixtures/issue513/xv-timing-20261103.json)
qualifies5 device-free and32 actual-X11 groups in each native and
ASan/UBSan/leak variant. It preserves existing pixel/PTS/ownership checks,
holds real ACK delivery to distinguish0/3/4 samples, verifies the final
source's exact residence, and excludes cancellation, failed flush, missing
and timestamp-expired matching ACK controls. The timestamp-expiry control is
diagnostic, not an observed server stall. Saturation and stop snapshots pass.
An initial stale native test selection (31 instead of32) is retained; final
completed-source builds execute all32 without skips.

On the unchanged current BCM70015/module13 route, FIFO/latest timing-on and
FIFO timing-off each complete750-picture/1500-rendered30s playback with actual
Pulse audio/clock and clean resource/module/PM return. The two timing-on runs
record1501 ACKs each: mean3.570/3.558ms and maximum11.041/7.853ms. The off run
records zero samples. No pending source or intentional replacement occurs,
so no current-card residence under congestion or latency benefit is qualified.
These single-run means/maxima are descriptive local receipt observations,
not percentiles or a physical-display measurement. Observed maximum57C and
related kernel errors0 add no broader target/endurance/release acceptance.

### Window, port and setup controls

`/xv/x11/live-window-replacement` owns two mapped windows and delays a real
SHM completion while replacement waits. It checks the old request identity,
source IDs/PTS/pixels, new-window pixels, later pool submissions and balanced
image/context ownership, including rejection of missing and unmapped targets.
This decoder-free window control requires at least a 640x240 local screen.
The decoder-free `/xv/x11/port-contention-reopen` control temporarily grabs
only currently free, compatible UYVY ports through its own local XCB connection
(bounded64 candidates/8s). It verifies the exact busy-port error, settles NULL
and records the same failed attempt's additional BaseSink state error before
reopening. Unexpected errors fail; messages are not silently flushed.
Releasing one owned port permits two same-pipeline12-frame natural-EOS cycles
on that exact port. Each terminal context has12 submissions/ACKs,6 allocated/
freed images, no pending/retained/quarantined resources and closed XCB/event
FDs; exact-port re-grab and final own-context/SHM checks prove retirement.
One live Intel965 host exercised16 free ports; already busy external ports
were not present in that run. Other clients' grabs are never released.
Without explicit X11 opt-in/local Xv availability the case truthfully skips.
The [receipt](../../../tests/fixtures/issue513/xv-port-contention-20261031.json)
retains the two original harness FAILs, diagnostic and corrected outcome.
System X-server interruption and broader stress remain separate acceptance.

Local socket connection and XCB protocol setup now share a one-second
deadline. Previously, an accepted connection with no setup reply could block
inside `xcb_connect()` before the existing reply deadlines applied. The own
connector uses nonblocking Unix connection attempts and the normal local
Xauthority selection, including MIT cookie and XDM authorization. A joined
watchdog shuts down a duplicate of only that connection's socket; no blocked
setup worker is abandoned. Caller signal masks and any already-pending
SIGPIPE are preserved across a deadline/write race. Xauthority filesystem I/O
is ordinary local file lookup and is not interrupted by this network deadline.

The [startup receipt](../../../tests/fixtures/issue513/xv-setup-deadline-20261103.json)
retains a direct production-code before/after control: the old connection
still waits after three seconds, while the corrected call returns a setup
timeout after about one second, before the owned peer closes. Peer credentials
identify the exact caller process. The initial gst-launch attempt instead
blocked before the own plugin was loaded and is excluded from attribution.

`make -C filters/gst/xv-output check` also runs six isolated setup exchanges:
no authority/MIT/XDM, each with an absent or partial reply, normal and `unix/`
display spelling and explicit screen selection. Private known test credentials,
FD/context retirement and caller signal state are checked without contacting
the user's display. The explicit X11 suite adds timeout then normal display/
EOS/restart in one process; all18 groups pass ordinary and ASan/UBSan/leaks
on the current965GM host. This does not qualify restarting the system X server,
card decoding, A/V or physical presentation. Original failures remain retained.

The decoder-free `/xv/x11/live-port-handoff` control requires two currently
free compatible ports and owns its pixel-test window. It covers busy/missing
requests, a deliberately injected candidate-only dormant-pool layout mismatch,
a held real SHM completion, direct-pool source IDs/PTS/pixels, retained and
next-frame RGB output, exact-port reacquisition, preference reset and exact
startup selection. A stop during a second held-ACK handoff cancels it: that
request has zero ACKs and one detach-proven terminal retirement, not successful
presentation. A separate actual appsrc pipeline switches ports in PAUSED and
PLAYING, preserving source memory/PTS through the copy path and reaching
natural EOS with five requests/ACKs (three video calls including preroll, two
repaints). Its two source pictures are not five unique video frames.
The [current-host receipt](../../../tests/fixtures/issue513/xv-live-port-20261102.json)
also retains the initial harness failures. PAUSED pixel regression fixtures
explicitly map their own windows; an SHM ACK/EOS does not prove a window manager
has made a managed window viewable. No user desktop policy is changed.

### Earlier playback and endurance observations

The [October26 qualification](../../../HARDWARE-2026-09-13.md#development-per-instance-xv-output--2026-10-26)
records complete bounded Player controls and a timestamped MP4 error/reopen
test. A reused GStreamer 1.28.2 H.264 parser can infer incorrect raw-stream
PTS after NULL; the same failure occurs with software decoding. A test-local
restoration of its original parser policy passes 100/100 fields on three
replays, while unmodified-parser failures remain recorded. This output does
not change parser policy in applications.

The earlier `db391ad` 30-second speech check confirms audible A/V and normal speed
with card720/Bob50. Pause/resume and forward/backward seek also complete;
the control run has four BaseSink drops in its final post-seek snapshot, so
it is not a zero-loss census. A decorated test-local GTK embedding window
also passes F11/resize, EOS and resource retirement. This does not add a GUI
or fullscreen shortcut to the installed Player.

The [`7aaec83` paused-redraw follow-up](../../../tests/fixtures/issue513/xv-paused-redraw-20261027.json)
passes 18 device-free/X11 ASan/UBSan/leak checks, including actual paused
window pixels for both pool and copied input. Three native MP4 error/reopens
return 180 originals each. Baseline and updated 30-second speech runs each
decode 750 originals and render 1,500 with zero drops; the single-pair CPU
difference is about 1%, not evidence of a speedup. A separate native control
run repaints twice while paused without new video calls, then resumes/seeks
and closes cleanly. Its seek/reset counters are not a unique-frame census.
This is automated output/lifetime qualification, not new physical A/V acceptance.

The [bounded paced-HTTP test](../../../tests/fixtures/issue513/xv-paced-http-20261027.json)
also passes at the same runtime. Endless chunked H.264/AAC input pauses for
10.01 seconds, then resumes with the same decoder and audio clock. After
buffered input drains, 142 decoded pictures and 281 video requests stay
unchanged while resize/exposure adds two acknowledged repaints. Resuming
reaches 245 decoded pictures; explicit quit closes in 0.296 s. A new HTTP
connection/pipeline in the same process decodes 106 pictures and closes in
0.270 s. Device FDs, owned SHM and retained pictures retire; kernel and
module/pin/PM/link/AER boundaries pass. Counts include preroll/in-flight calls,
not a full-input EOS or scanout census. Initial buffering takes about eight
seconds; source-loop boundaries, physical A/V and network latency are not
qualified. The earlier invalid 4 Mbps transport control is retained separately.

The planned ten-minute run remains incomplete after a normal 85°C CPU-guard
stop at 359 seconds. Stock Xv also reaches the same guard; this does not
establish a new-output-only thermal defect or an endurance PASS. Scanout,
calibrated offset,
E2E and native i386 acceptance remain unverified. Short CPU measurements do
not establish a speedup. [#513](https://github.com/ahnhy1324/crystalhd/issues/513)
remains open and this component is experimental.

<a id="current-player-integration-2026-10-30"></a>

### Historical Player integration, 2026-10-30

At this revision, the optional output is integrated with the rolling stage trace
and paired decoder finish collector. Player checks pass 125 tests; configuration
checks classify 39 surfaces with 21 managed keys. The four device-free and
14 real-X11 component groups pass. These are historical counts, not the current
suite inventory. Default output selection remains `auto`.

One pre-frozen original-profile PCM/FHD invocation reuses the exact current
decoder/DIL images and selects `--video-output xv-async`. Actual negotiation
confirms early 1280x720 packing, method-4/two-thread 1920x1080 scaling and the
original QoS, 5ms lateness and 15ms processing deadline. The first counted
sink drop is at PTS 147.36s. The 75°C guard stops playback normally after
247.619s wall time, before the planned 300s natural EOS. FHD acceptance fails;
this run does not establish a repair or a matched performance improvement.

After NULL, all 12,236 own Xv requests are acknowledged, pending requests and
images are zero, and all six allocations are freed without quarantine. The
sink row-copy counter is zero and all requests use its negotiated pool;
these are local ownership/copy observations, not scanout or end-to-end
zero-copy qualification. The exact driver/resource boundary and kernel
checks pass. See the [source-pinned result](../../../tests/fixtures/issue513/xv-current-integration-20261030.json).
