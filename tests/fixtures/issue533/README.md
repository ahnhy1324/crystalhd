# V4L2 buffer sharing experiment

This opt-in BCM70015 progressive H.264 prototype tests card CAPTURE → EXPBUF →
EGL/GPU rendering against two host copies plus GL upload, following
[#531](https://github.com/ahnhy1324/crystalhd/issues/531). It reuses unmerged
[#115](https://github.com/ahnhy1324/crystalhd/pull/115). Nonempty drained START
still returns `EOPNOTSUPP` for the
[#114 reference-state limitation](https://github.com/ahnhy1324/crystalhd/issues/114);
the Kick/continuation gates in #533 remain separate.

[Memory/render proof](yuy2-interop-20261026.json) and
[native CAPTURE results](native-capture-20261026.json) retain measurements,
source identities, unsuccessful controls and subsequent corrections. The heap
CPU-write proof needed cache writeback; the real CAPTURE GPU consumer performs
neither CPU writes nor cache flushes. The two producers must be distinguished.

## Results

Kernel 7.0.0-38, Mesa 26.0.8, Intel 965GM; six-second 30p H.264 High/B2 fixtures.
Mean render CPU from two arms per mode, in upload/direct/direct/upload order:

| Output | Two copies + GL upload | Direct CAPTURE import | Render CPU reduction |
| --- | ---: | ---: | ---: |
| 640×360 | 0.873 ms/frame | 1.133 ms/frame | **29.8% worse** |
| 1280×720 | 3.675 ms/frame | 1.149 ms/frame | 68.7% |
| 1920×1080 | 8.640 ms/frame | 1.147 ms/frame | 86.7% |

These are render-stage savings. Whole-process CPU in the probe with three SHA
observers decreased only 4.95% at 720p and 6.47% at FHD; 640×360 increased 2.24%.
All 15 cases completed 2,700 pictures with ordered PTS, frozen YUYV and genuine
LAST/EPIPE. The 540 full-frame GPU RGB checks passed at maximum error 1/255
(fixed tolerance 2). Peak temperature was 79°C; starts required less than 75°C.

Performance results retain runtime `4667d31`. Later fixes were qualified
functionally on `830170b`, preserving earlier failures:

- File-owned ioctl handles replace the NULL reserved callback argument used
  by modern kernels; the original port fault and reboot recovery are retained.
- Bounded TX retries use 2 ms instead of a fixed 100 ms host stall. Measured
  median retry time fell from 100.804 to 2.898 ms; absolute deadlines,
  cancellation and legacy 100 ms behavior remain intact.
- Client CAPTURE capacity and index tracking cover up to 32 buffers, separately
  from hardware admission. During drain, one existing queued credit remains
  available until real pictures retire, then receives the genuine EOS marker.
  The capacity-only and index-only intermediate failures remain recorded.

On the final code, unmodified FFmpeg 8.0.1 decoded all 180 pictures with matching
YUYV at each size using default `h264_v4l2m2m` decoder options. Earlier 720p/FHD
runs returned exit 0 with only 79/69 pictures and remain **FAIL**. The final code
also passed 360 pictures with a two-buffer pool and explicit reset recovery,
180 GPU-checked pictures with 32 buffers, legacy/native ownership exclusion,
and 141 real ioctl/MMAP/EXPBUF checks in each ILP32 and LP64 process.
All successful matrix and final cases restored the original 3.10.13 module
and idle/PM/AER state; the first failed port required the recorded reboot.

The benchmark uses 12 buffers with immediate requeue; its results were not
relabeled after the larger-pool and final-credit fixes. Physical A/V, energy,
maximum decoder FPS, native i686/non-SSE playback, other codecs, arbitrary color
metadata and unmodified Kodi integration remain unqualified. This remains an
experimental backend with the nonempty STOP/START limitation above.

## Reproduce

Work from the repository root in a dedicated test session, following the
[idle/recovery prerequisites](../../../BRINGUP.md). Build the experimental
module separately; this does not install or replace the loaded module:

```sh
make -C "/lib/modules/$(uname -r)/build" \
    M="$PWD/driver/linux" CRYSTALHD_V4L2=1 W=1 -j2 modules
```

With that module temporarily loaded under the controlled recovery procedure,
select its QUERYCAP-identified video node. The native probe needs FFmpeg
development libraries; GPU mode additionally needs EGL, GLES2, GBM and libdrm
development libraries and an Intel render node at `/dev/dri/renderD128`.
Use a local progressive H.264 fixture interpreted as limited-range BT.709:

```sh
CRYSTALHD_TEST_GPU=1 sh tests/v4l2-decode.sh \
    --device /dev/video0 --input /absolute/path/to/fixture.mp4 \
    --gpu-check --timeout 90
```

`--gpu-check` checks every image against an independent RGB conversion.
`--gpu-render` draws exported buffers directly; `--gpu-upload` performs two host
copies and packed-texture upload. All retain count, ordered PTS, LAST/EPIPE and
pixel-hash observers. Use `--capture-buffers 2` or `32` for pool boundaries
(default 12). `--repeat 2` checks rejected nonempty START and explicit OUTPUT
STREAMOFF recovery. DMA-BUF export of MMAP buffers works; client DMA-BUF import
through REQBUFS remains unsupported.

Compare upload/direct/direct/upload on the same file without concurrent decode
or tracing. Render timing includes export/import or copies, upload, draw and
`glFinish`; it excludes RGB readback/reference work and shader allocation.
Whole-process time includes initialization, demux, three SHA observers and
drain. Completed draws do not measure physical display or E2E latency.

The stock-FFmpeg full-file check used:

```sh
ffmpeg -nostdin -hide_banner -loglevel verbose \
    -c:v h264_v4l2m2m -i /absolute/path/to/fixture.mp4 -an \
    -c:v rawvideo -pix_fmt yuyv422 -fps_mode passthrough \
    -f hash -hash sha256 -
```

Require the expected picture count and matching decoded pixels, not just exit
0. This checks hardware decode into CPU-visible YUYV; GPU sharing by FFmpeg or
a player is a separate integration question. Application sources and the
installed playback stack were unchanged throughout these experiments.

The standalone heap/render proof requires no card or module:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror \
    $(pkg-config --cflags egl glesv2 gbm libdrm) \
    tests/v4l2-yuy2-interop.cpp \
    $(pkg-config --libs egl glesv2 gbm libdrm) -o /tmp/chd-yuy2-interop
sudo /tmp/chd-yuy2-interop --frames 120
```

It needs `/dev/dma_heap/system` access and x86 CLFLUSH/MFENCE capabilities.
These are probe prerequisites; the driver and `libcrystalhd.so.3` ABI retain
their existing requirements.
