# CrystalHD VA-API driver

This backend exposes BCM70012/BCM70015 H.264 decoding through the standard
VA-API VLD interface. It accepts VA-allocated NV12 surfaces and imported
linear or GBM-mappable DRM PRIME NV12 surfaces for FFmpeg-style clients. It
also allocates and exports ARGB DRM PRIME surfaces for Chromium-class clients
whose compositor cannot render NV12 directly.

This is an experimental, client-oriented VA-API subset rather than a complete
VA-API implementation. Complete progressive H.264 Baseline/Main/High decode
to NV12 through FFmpeg on BCM70015 passes the tested 180-frame fixtures,
including independent software pixel comparisons. High also passes a complete
reference decode and four forward/backward seek/flush pixel comparisons in
synchronous mode; all three profiles pass the separate eight-frame lookahead
variant with eight queued client pictures left undownloaded before each flush.
The separate `--retain-old-frames` variant keeps those frame owners across
flush/new input or context destruction and then verifies their original pixels.
An infinite `vaSyncSurface` request reports a decode error after 10 seconds
without its picture, instead of hanging or substituting another frame.
The [GStreamer path](../gst/gst-plugin-1.0/README.md) remains the primary
playback baseline. DRM PRIME import/export and VPP smoke tests do not themselves
exercise CrystalHD hardware, and no libva conformance suite is run.

BCM70015 firmware retains output until later compressed pictures or a real
end-of-sequence marker arrive. After 100 ms of synchronization grace, the
backend may seal the exact submitted batch with EOS. New input is queued until
all real output timestamps and the firmware EOS marker are received. If input
continues, the complete device is reopened and original access units from the
last retained actual IDR rebuild reference state. Replayed completed pictures
are discarded before touching immutable client pixels. Access-unit delimiters
are first in their own timestamped packet; trailing delimiters shifted firmware
timestamp association. Decoder-only resets were tested and rejected after a
firmware stall; full device reopen passed the validated sequence.

The replay cache allows at most 512 KiB per access unit, 32 MiB / 512 access
units total, and 8192 replayed units before a completed older IDR prefix is
pruned. Missing IDR history or exceeded limits produces an error. This is not
free: a client that downloads each picture before submitting the next may
reopen and replay for nearly every picture. The 100 ms grace helps clients
with concurrent input; it cannot manufacture lookahead for synchronous ones.
BCM70012 does not use the unvalidated sealed-batch path.

Decoder-context destruction stops new submissions and drains already-accepted
live pictures before closing the device, so queued FFmpeg output frames retain
their actual pixels. The drain has a ten-second polling budget; individual
firmware calls and cleanup can extend wall time, so hardware tests also use
an external timeout. Completed plain decode surfaces survive context removal;
destroyed/reused surfaces and canceled VPP epochs still report errors.

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
CrystalHD are converted directly before `vaEndPicture` returns. VPP sources
are captured when `vaRenderPicture` supplies the parameters, and remain
available for repeated VPP until the decode surface is reused or destroyed.
Decoder generations are checked after every hardware wait, so retired pictures
cannot become successful new-epoch output. A target with an outstanding writer
rejects reuse as busy. ARGB is first rendered into private memory and only then
copied into the shared DMA-BUF under the write fence. Missing or
canceled pictures report errors; they are never replaced by unrelated fallback
pixels. Destroyed surface objects are permanently made non-writable before
their VA IDs are released. Timeline increments cannot encode arbitrary operation
errors; closing an unsignaled timeline releases its fences with `-ENOENT`.
Clients must check VA surface status, not treat fence signaling alone as proof
of valid pixels. Hardware-free production-state tests
cover repeated VPP, source reuse between parameter submission and completion,
decoder retirement, busy targets, and cancellation. These fix and verify driver
lifecycle bugs, not end-to-end browser playback: the FFmpeg drain and seek
results above do not establish browser hardware seek correctness.

Current limitations:

- VA-API supplies no explicit end-of-stream callback; bounded batch replay
  covers the tested BCM70015 files, not arbitrary streams or BCM70012
- only the decode, image, DRM PRIME, and minimal video-processing operations
  needed by the documented clients are implemented; `vaPutSurface`,
  subpictures, palettes, and detailed surface-error reporting are unavailable
- progressive H.264 Constrained Baseline, Main, and High profiles only
- maximum coded size 1920x1088
- NV12 images are limited to 1920x1088; odd dimensions retain complete UV pairs.
  Image copies reject busy surfaces and invalid rectangles. `vaPutImage` also
  rejects retained decode pictures and their aliases instead of invalidating
  decoder reference identity. `vaDeriveImage` remains a readback snapshot, not a
  writable direct alias; use `vaCreateImage`/`vaPutImage` for ordinary uploads.
- imports sharing any known backing object must describe identical format,
  dimensions, and plane views. Nonidentical views are rejected; surviving aliases
  of a destroyed owner are invalid, and reimport is busy until pending writes end.
  H.264 decode targets must be canonical surfaces, not imported aliases; aliases
  remain usable for the supported image and VPP operations.
- one CrystalHD playback session at a time; another client receives hardware
  busy until the active decoder closes
- video processing is limited to unfiltered NV12 scaling and NV12-to-ARGB
  conversion; there is no deinterlacing or rotation
- no VP8, VP9, AV1, or protected content
- direct rendering requires writable NV12 DRM PRIME buffers
- the VA-API backend is not a general display driver; it uses an existing DRM
  render node for surface allocation while CrystalHD performs H.264 decoding
- earlier browser hardware tests showed incorrect post-seek pixels; driver
  lifecycle fixes now pass regressions, but browser hardware seek correctness
  still needs validation. The `crystalhd-chromium` launcher therefore defaults
  to `FFmpegVideoDecoder` and
  requires `CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE=1` for browser VA-API
- browser hardware access has two distinct unresolved integration barriers:
  Chromium's GPU sandbox does not broker the CrystalHD device and firmware
  files, while asynchronous VPP additionally needs the debugfs `sw_sync`
  timeline to fence compositor reads. Disabling the GPU sandbox alone does
  not grant that timeline's filesystem permissions. The driver reports an
  error if it cannot fence pending VPP; it does not change permissions or
  substitute a synchronous decode wait inside `vaEndPicture`. Default browser
  software decoding keeps the GPU sandbox enabled.

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
