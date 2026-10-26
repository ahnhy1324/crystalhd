# GPU field-view probe

[#530](https://github.com/ahnhy1324/crystalhd/issues/530) first-stage results:
[source-qualified measurements and failed controls](egl-field-views-20261027.json).
This is a synthetic display experiment on Intel 965GM, not a decoder, VA Bob
replacement, supported frontend or physical presentation measurement.

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
