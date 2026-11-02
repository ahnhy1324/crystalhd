# Optional packed enlargement

`crystalhdpackedscale` fuses YUY2 packing and method4 enlargement for the explicit
progressive1280x720/PAR1/50Hz to UYVY1920x1080/PAR1/50Hz path. It is rank NONE
and requires GStreamer1.28.2. The normal Player path remains independently
selectable. Unsupported formats, rates, geometry, color conversion, multiview,
interlacing and spatial metadata are rejected; there is no automatic fallback.
Input and output must have the same resolved colorimetry and chroma site.

Build and check from the repository root:

```sh
make -C filters/gst/packed-upscale check
make -C filters/gst/xv-output
```

After building the normal decoder/library, explicitly select both components:

```sh
GST_PLUGIN_PATH="$PWD/filters/gst/packed-upscale:$PWD/filters/gst/xv-output:$PWD/filters/gst/gst-plugin-1.0" \
LD_LIBRARY_PATH="$PWD/linux_lib/libcrystalhd" \
  scripts/crystalhd-play --hardware --video-output xv-async --packed-upscale \
  --scale-width 1280 --software-output-size 1920x1080 "/path/to/video.mkv"
```

The switch is invocation-only. It does not alter saved settings, PTS, audio
clock, QoS, lateness, processing deadline, priorities, queues, Xv ring depth or
pool size. Method4 and the two compute threads are fixed: the calling thread
and one persistent worker compute disjoint output stripes. Other GStreamer
versions pass only the CPU check of version rejection; they do not receive a
pixel or runtime qualification.

The engine uses public GstVideoScaler coefficients and the same Q6 horizontal
rounding as the qualified stock path. Setup rejects coefficients that cannot
prove exact integer normalization. A table-verified3:2 periodic kernel uses
SSSE3 only after runtime CPU admission; other CPUs use the same scalar math.
Vertical interpolation uses the public scaler. Each stripe caches two
horizontal rows, so no full720p packed intermediate is written or read.
No private GStreamer structures or third-party implementation edits are used.

The normal GstVideoFilter/BaseTransform path allocates the output from the
negotiated downstream pool. The engine writes only active bytes through actual
mapped plane offsets/positive strides; it retains no buffer references after
return. The wrapper serializes run/reconfiguration/free and joins its worker
before releasing its scalers/cache. Memory-specific input metadata is not
copied onto the output pool layout. Unqualified spatial metadata is an error;
ordinary nonspatial metadata uses its normal COPY transform.

The Player records four actual stages: decoder, deinterlace, output-scale and
sink. The output-scale factory is crystalhdpackedscale. There is no separate
output-pack buffer or fabricated measurement. Read-only properties `method`,
`n-threads`, `simd-active` and `frames` describe the fixed method, current or
last negotiated dispatch and successful lifetime transforms. These are not
physical display counters.

CPU checks compare36 stock/candidate frames over three restart cycles, exact
pixels/timing/offsets/flags, flush/new segment/EOS, padded offsets/strides with
sentinels and unsupported-caps/crop rejection. Discovery is limited to the
plugins required by the test. The separate X11 allocation control can be
built with `make -C filters/gst/packed-upscale packed-scale-x11-contract` and
explicitly invoked with `--execute-x11`; it opens one temporary window. Its
sync=false control proves only pool/ACK/teardown behavior, never clocked media
performance. The natural300s qualification keeps sync/QoS and original5ms
lateness/15ms deadline unchanged, requires the75C guard, original source grid,
15000 field counts, EOS, zero counted loss and complete own image retirement.

Finite CPU/ownership checks do not establish physical scanout, calibrated A/V,
repeatability, arbitrary inputs or sustained thermal endurance.


The selected CPU contract also has an isolated sanitizer target:

```sh
make -C filters/gst/packed-upscale check-sanitized
```

It recompiles both the plugin and test executable with ASan/UBSan, enables leak
checking without suppressions and halts on sanitizer errors. The executable
loads the instrumented plugin from a fresh required-plugin-only registry with
in-process discovery; an uninstrumented external plugin scanner is not used.
The temporary outputs are removed on success, compiler/test failure or normal
interruption. Ordinary O2 outputs and their CPU-mode stamp are untouched;
a private sanitizer config fingerprint records the effective toolchain, CPU,
sanitizer, app-link and plugin-directory choices in the temporary build.
`cpu.mk` still validates and supplies the selected CPU flags to both builds.
Only the instrumented test executable uses non-PIE linking for a stable
sanitizer address space. Make passes the plugin directory resolved by its
selected `PKG_CONFIG` to the shared loader, preserving the selected metadata
rather than discovering unrelated default plugins.

Both Ubuntu CI userspaces run this explicit target after the ordinary component
check. GStreamer1.28.2 executes the36 stock/candidate pixel, metadata, stride,
flush/EOS/restart and rejection comparisons. Other versions load the plugin and
verify explicit runtime-version rejection only; they receive no pixel or media
qualification. The sanitizer target is CPU-only and adds no X11, decoder-device,
media, physical-output or thermal test.

The shared loader has separate filesystem-only contracts in
`tests/packed-sanitizer-loader.test.py`. They check exact selected file identity
for absolute/relative paths with spaces, reject ambient fallback, propagate a
failing test status and remove temporary registries. Those stub-based checks
do not replace the native pixel/sanitizer comparisons above.
