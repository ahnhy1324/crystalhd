# Optional Xv output

Development `crystalhdxvsink` is a separately selected X11 output for
GStreamer. It uses a three-request SHM completion ring and a six-buffer
negotiated pool. It does not replace GStreamer's installed plugins or change
`crystalhd-play`'s default output. Hardware decoding still uses `crystalhddec`;
the output plugin itself works without a CrystalHD card.

Build with the GStreamer video development package and `xcb`, `xcb-shm`,
`xcb-xv` development packages available to `pkg-config`. From the source root:

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
A caller can supply a window through `GstVideoOverlay` **before** starting.
Exposure events, resize and `GstVideoOverlay.expose()` redraw the retained
last picture while paused, including after EOS. This preserves its pixels
and PTS and does not count as a new video frame. Replacing a live window is
not implemented.

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

```sh
# Device-free checks. The second command opens temporary X11 windows.
make -C filters/gst/xv-output check
make -C filters/gst/xv-output check-x11
```

The [current qualification](../../../HARDWARE-2026-09-13.md#development-per-instance-xv-output--2026-10-26)
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

The planned ten-minute run remains incomplete after a normal 85°C CPU-guard
stop at 359 seconds. Stock Xv also reaches the same guard; this does not
establish a new-output-only thermal defect or an endurance PASS. Scanout,
calibrated offset,
E2E and native i386 acceptance remain unverified. Short CPU measurements do
not establish a speedup. [#513](https://github.com/ahnhy1324/crystalhd/issues/513)
remains open and this component is experimental.
