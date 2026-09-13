# CrystalHD VA-API driver

This backend exposes BCM70012/BCM70015 H.264 decoding through the standard
VA-API VLD interface. It accepts VA-allocated NV12 surfaces and imported
linear or GBM-mappable DRM PRIME NV12 surfaces for FFmpeg-style clients. It
also allocates and exports ARGB DRM PRIME surfaces for Chromium-class clients
whose compositor cannot render NV12 directly.

This is an experimental, client-oriented VA-API subset rather than a complete
VA-API implementation. Complete progressive H.264 decode to NV12 through
FFmpeg on BCM70015 currently fails validation: a 180-frame Baseline sample
produced only 178 frames because firmware retained the final two pictures.
An infinite `vaSyncSurface` request now reports a decode error after 10 seconds
without that picture, instead of hanging or substituting another frame.
Use the [GStreamer path](../gst/gst-plugin-1.0/README.md) for validated complete
playback. The DRM PRIME import/export and VPP smoke test does not itself
exercise CrystalHD hardware, and no libva conformance suite is run.

Chromium may export a VA surface and then reimport the same DMA-BUF under a
different surface ID for video processing. The driver links those aliases to
the original decode or display owner so input completion and output readiness
remain synchronized before the compositor presents a frame.

BCM70015 hardware output is YUY2, which the driver converts to the requested
NV12 client surface. A minimal CPU video-processing path uses libswscale's
SIMD conversion for NV12-to-ARGB output when Chromium needs a
compositor-compatible export. CrystalHD needs later compressed pictures before
it emits some reordered output. Each submission therefore gets immutable,
timestamped NV12 storage independent of Chromium's reusable VA surface. A VPP
worker waits for hardware pictures that are still reordered, allowing Chromium
to keep submitting input while output is pending. Pictures already emitted by
CrystalHD are converted directly before `vaEndPicture` returns. Reused display
targets carry a generation number that is checked again after every hardware
wait, so an older queued conversion cannot overwrite a newer frame. ARGB is
first rendered into private memory and only then copied into the shared DMA-BUF,
preventing Chromium from sampling a half-converted scanline image. Superseded
asynchronous targets receive the latest complete frame (or neutral black after
a discontinuity) before their DMA-BUF fence is signaled; unchanged pool memory
is never exposed. Destroyed surface objects are permanently made non-writable
before their VA IDs are released.

Current limitations:

- complete end-of-stream drain and VA-API seek/flush pixel identity are not
  validated; VA-API supplies no explicit end-of-stream callback, and sending
  the library's H.264 end-of-sequence marker during ordinary surface sync
  would invalidate ongoing reference-picture decoding
- only the decode, image, DRM PRIME, and minimal video-processing operations
  needed by the documented clients are implemented; `vaPutSurface`,
  subpictures, palettes, and detailed surface-error reporting are unavailable
- progressive H.264 Constrained Baseline, Main, and High profiles only
- maximum coded size 1920x1088
- one CrystalHD playback session at a time; another client receives hardware
  busy until the active decoder closes
- video processing is limited to unfiltered NV12 scaling and NV12-to-ARGB
  conversion; there is no deinterlacing or rotation
- no VP8, VP9, AV1, or protected content
- direct rendering requires writable NV12 DRM PRIME buffers
- the VA-API backend is not a general display driver; it uses an existing DRM
  render node for surface allocation while CrystalHD performs H.264 decoding
- current Chrome's asynchronous output-pool reuse does not preserve frame
  identity across CrystalHD's reordered output after timeline seeks; the
  `crystalhd-chromium` launcher therefore defaults to `FFmpegVideoDecoder` and
  requires `CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` for browser VA-API

Build and inspect the driver:

```sh
make -C linux_lib/libcrystalhd
make -C filters/vaapi check
LIBVA_DRIVER_NAME=crystalhd \
LIBVA_DRIVERS_PATH=$PWD/filters/vaapi \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
vainfo --display drm --device /dev/dri/renderD128
```

Hardware-decode a file with FFmpeg:

```sh
LIBVA_DRIVER_NAME=crystalhd \
LIBVA_DRIVERS_PATH=$PWD/filters/vaapi \
LD_LIBRARY_PATH=$PWD/linux_lib/libcrystalhd \
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i video.mp4 \
  -vf hwdownload,format=nv12 -f null -
```

After `sudo make install`, libva discovers the driver from the system DRI
directory, so only the driver selection and render device are required:

```sh
LIBVA_DRIVER_NAME=crystalhd \
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i video.mp4 \
  -vf hwdownload,format=nv12 -f null -
```

The DRM render node normally belongs to the machine's display GPU. That GPU
allocates and displays surfaces; the BCM70012/BCM70015 still performs the H.264
decode. For persistent Chrome integration, including codec preference and
desktop default-browser registration, see the **Chrome and YouTube** section
of the top-level [README](../../README.md).
