# GPU field-view probe

[#530](https://github.com/ahnhy1324/crystalhd/issues/530) first-stage results:
[source-qualified measurements and failed controls](egl-field-views-20261027.json).
The first stage is a synthetic experiment on Intel 965GM. The subsequent
native integration below uses real CrystalHD output. Neither stage qualifies
a supported frontend, VA Bob replacement or physical presentation.

One immutable linear YUYV allocation supplies top/bottom EGLImages with
offsets 0/S, pitch 2S and height H/2. Both field uses complete before source
retirement. Two differently patterned allocations alternate by picture;
both parities and TFF/BFF order are checked. CPU producer cache writeback is
specific to this heap test, not a requirement inferred for direct card RX.

The independent per-pixel BT.709 RGB oracle checks physical nearest Bob:
preserve original field-line positions, choose the following line on a
missing-line tie and clamp edges. It differs from the origin-aligned VA
row-duplication helper. At 720→FHD, neighboring output pixels can use different
source chroma pairs, so the oracle does not repack expected output as YUYV.
All 32 final preflight comparisons pass with maximum error 1/255, tolerance 2.
Nearest sampling uses an interior bias derived for each scale ratio;
linear filtering remains unqualified. The intermediate FHD shader rounding
failure at `32db394` and its correction at `c4af844` are retained.

| Source → GPU output | Fields/s | Field-view completions | Late completions | Mean draw + wait | Mean client CPU/field |
| --- | ---: | ---: | ---: | ---: | ---: |
| 720 → 720 (`77d42a7`) | 50 | 400 | 0 | 7.093 ms | 0.410 ms |
| 720 → 720 (`77d42a7`) | 60 | 400 | 0 | 7.040 ms | 0.400 ms |
| FHD → FHD (`77d42a7`) | 50 | 400 | 1 | 15.605 ms | 0.413 ms |
| FHD → FHD (`77d42a7`) | 60 | 400 | 240 | 16.116 ms | 0.376 ms |
| 720 → FHD (`c4af844`) | 50 | 400 | 0 | 14.254 ms | 0.405 ms |
| 720 → FHD (`c4af844`) | 60 | 400 | 67 | 14.695 ms | 0.386 ms |

Each row comprises four bounded arms of 100 fields, two per field order.
Every field is submitted and completed: **late is not dropped**. Late means
completion after the next field's due time. At 720→FHD60, maximum age is
35.743 ms; this is not a lossless 60-field result. Full-image parity shaders
are slower (about 22.9 ms at scaled FHD, all 400 late at each rate).

Earlier same-size CPU row-copy + packed GL upload comparisons and the slower
per-fragment coordinate shader remain in the JSON. They are synthetic
controls, not measurements of the complete production VA/GStreamer path.
There is no CPU-scaler comparison for the scaled case. Source generation,
cache writeback, shader setup and RGB readback are outside the clocked arms.
No frame PTS, decoder readiness, scanout, physical A/V or energy is measured.

From the source root, with EGL/GLES2/GBM/libdrm development packages:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/egl-field-views.cpp \
    $(pkg-config --cflags --libs egl glesv2 gbm libdrm) -o /tmp/chd-field-views
sudo /tmp/chd-field-views
sudo /tmp/chd-field-views --fields 100 --scaled-only
```

The first command run checks pixels at all four geometries. Omit
`--scaled-only` for all clocked geometries; counts must be even and at most
180 per arm. The probe needs Intel `/dev/dri/renderD128`, DMA-heap access,
x86 CLFLUSH/MFENCE, start temperature below 75°C and runtime below 85°C.
Run without concurrent playback or benchmarks.

Native source generation/lifetime, field PTS/media clock, decode-progress
policy, EOS/seek/reopen, NV12 and physical A/V need separate proof. Removing
CPU Bob does not inherit the VA-only auxiliary Kick trigger. No card, module,
firmware, default or ABI was changed by these runs.

On this recorded source, GStreamer's `gst_crystalhd_handle_frame` submits the
real AU through `DtsProcInput`; its finite drain uses `DtsFlushInput(0)`.
It has no VA-style Prefix/Auxiliary policy. In contrast, VA's
`TryH264Auxiliary` requires Bob demand, 100 ms idle and the existing syntax,
generation and ownership predicates. Native integration must qualify its
actual producer/readiness owner rather than transplanting or assuming that
trigger. This is source inspection, not a new runtime or Kick measurement.

## Native finite-file integration

[`gst-field-views.cpp`](../../gst-field-views.cpp) connects the existing
GStreamer decoder to the shared field renderer: BCM70015 → assembled YUY2 →
one owned GBM transfer → two field EGLImages → offscreen GPU output. The
card-to-Gst public copy remains. Each source sample and BO generation stay
owned through both GPU completions; images retire before the next write.
The first actual source is prepared during PAUSED preroll, with one extra
GPU use counted separately from unique timed fields. Output PTS are unchanged.

The final GBM path runs as an ordinary user with card/render-node access.
GBM map/unmap supplies its coherency boundary, including any internal staging
cost, without the heap producer's explicit CLFLUSH. The linear four-byte-pair
storage is imported as YUYV; this is byte layout, not CPU color conversion.
The final BFF/720→FHD proof checks all 50 distinct pictures/100 fields at
maximum RGB error 1/255. Earlier CPU/TFF/BFF proofs remain source-qualified
in the [native receipt](native-field-views-20261027.json).

![Matched native CPU measurements](native-field-views-20261027.svg)

Final timing source `05492a8`, two 50-picture trials per path in ABBA order:

| Card720 → output; fields/s | Renderer-thread CPU reduction | Playback-process CPU reduction | Late fields, CPU / GBM (each /200) |
| --- | ---: | ---: | ---: |
| 720; 50 | 52.8% | 23.1% | 0 / 0 |
| FHD; 50 | 52.7% | 22.7% | 3 / 1 |
| FHD; 60 cadence stress | 58.3% | 33.0% | 113 / 68 |

Every run completes 50 pictures/100 distinct field uses and natural EOS;
late completions are retained, not dropped. FHD60 has no lossless-deadline
qualification. The 60-field input is a separate VUI-cadence fixture: all
50 VCL payloads/order remain identical, while GStreamer negotiates 30
pictures/s. It is not representative newly encoded 30fps motion content.
Peak RSS in the final matrix is about 154–156 MiB; no power result is claimed.

Both paths use the same physical-nearest field reference and output size.
The CPU control materializes source-size rows and uploads a packed texture; GBM imports
field views. Renderer-thread CPU includes preparation/copy/map/cache/import
and drawing. Playback-process CPU includes decoder/assembly and observers,
but excludes initialization/preroll. Full main lifetime through NULL is
reported separately. These are short headless completion measurements;
physical A/V, scanout, E2E and steady-state endurance remain unqualified.

Full-image RGB checks run separately from timing. The timed observer samples
three YUYV pairs on every active row and compares 50 unique ordered signatures
across arms; it is explicitly sparse, not a full-image hash. Earlier dual-SHA
timed runs, cold first-field work, per-picture thermal-read overhead, and the
rejected GBM bo-write creation flag remain in the receipt. The 85°C stop
threshold is unchanged; thermal sampling is bounded at 250 ms.

Build from the source root with GStreamer app/video development packages:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror tests/gst-field-views.cpp \
    $(pkg-config --cflags --libs gstreamer-app-1.0 gstreamer-video-1.0 \
      egl glesv2 gbm libdrm) -ldl -o /tmp/chd-gst-field-views
/tmp/chd-gst-field-views /absolute/path/to/1080i25-50.h264 \
    50 1280 1080 field verify
/tmp/chd-gst-field-views /absolute/path/to/1080i25-50.h264 \
    50 1280 1080 field clocked
```

Use an independently counted, supported Annex-B H.264 MBAFF fixture and an
idle card. `cpu` selects the matched materialization control. The helper
prints actual loaded library/plugin identities and checks live-process
device FD retirement after NULL. It registers no player output or installed
plugin. Native i386, seek/flush/reopen, network/live input, physical playback
and broader geometry/color support require separate qualification.
