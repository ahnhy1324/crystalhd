# MPEG1 planar-to-packed reference isolation

This is offline, default-off research for [#529](https://github.com/ahnhy1324/crystalhd/issues/529),
using retained native captures from [#92](https://github.com/ahnhy1324/crystalhd/issues/92#issuecomment-5968507185)
and [PR217](https://github.com/ahnhy1324/crystalhd/pull/217). It changes no production
codec advertising, firmware, release oracle or quality threshold.

The [retained report](pristine-reference-20261031.json) freezes exact ES, planar420,
native YUYV, runtime-library and owned-source identities. The two inputs are
352x288/25 horizontal chroma with 30 I pictures and a changing checker with 90 I/P/B
pictures. Every conversion uses the same retained planar bytes and original
file order. There is no new hardware capture here.

The source pair is official FFmpeg
[`0a41fef0`](https://github.com/FFmpeg/FFmpeg/commit/0a41fef0fa5a16c4b9830f7ae87f94cfe52e8b4e)
and [`6507dc36`](https://github.com/FFmpeg/FFmpeg/commit/6507dc36e542df5fb1c0b8b394f56297ef50385e).
The latter contains the
[`a532a838` unscaled-position guard](https://github.com/FFmpeg/FFmpeg/commit/a532a8383f971af24d803ce59cb918103cefa920)
and its direct-child cascade-position propagation change. Both private builds
use identical options including `--disable-x86asm --cpu=core2`, report
libswscale 10.2.100/libavutil 61.10.100, and retain unchanged source/tree checks.
Standalone x86 assembly is disabled; compiler intrinsics/inline assembly may
remain. This is not retrospective identity of optimized distribution binaries.

Seven hand-computable reader controls and eight byte-exact I420/packed identity
controls passed before the 36-case Bicubic matrix. Each runtime processes all nine
auto/left/center source/destination siting pairs with `SWS_BICUBIC=4`, threads 1,
limited-to-limited BT601, unchanged geometry and no RGB conversion. Active
component bytes exclude aligned-buffer padding. All 18 corresponding pre/post
outputs are byte-identical, and every native comparison has exact Y over the
whole 30/90-frame population.

These declared Bicubic rows apply equally to both source revisions:

| Fixture | Source → destination siting | Whole-stream max Cb/Cr |
| --- | --- | --- |
| Horizontal30 | auto → auto | 13 / 14 |
| Horizontal30 | center → left | 2 / 2 |
| Checker90 | auto → auto | 57 / 57 |
| Checker90 | center → left | 22 / 22 |

Across all nine prescribed siting cases, the maxima are 20/21 for horizontal and
73/73 for checker. These are coverage extrema; the report retains every case,
MAE, changed fraction and error/edge distributions. No row is selected to
replace the frozen matrix or turn residual mismatch into a quality PASS.
The earlier installed 7.1.1/8.0.1 matrix also retained 18 byte-identical same-case
conversions; its evidence remains separate and unchanged.

The completed [filter report](filter-reference-20261031.json) adds seven reader
hand controls, eight exact identities, 108 native comparisons and 54 pre/post
comparisons for POINT 16, BILINEAR 2 and LANCZOS 512. All 54 pre/post pairs are
byte-identical, all native Y bytes are exact, and frozen input/provider pins remain
unchanged. It also rehashes and confirms byte identity of all 18 retained
old-7.1.1/pristine-post Bicubic pairs. There were no program or control errors.

Fixed center-to-left cases apply equally to both source revisions:

| Filter | Horizontal30 whole-stream max Cb/Cr | Checker90 whole-stream max Cb/Cr |
| --- | --- | --- |
| POINT 16 | 13 / 14 | 68 / 68 |
| BILINEAR 2 | 6 / 7 | 58 / 58 |
| BICUBIC 4 | 2 / 2 | 22 / 22 |
| LANCZOS 512 | 1 / 1 | 22 / 22 |

Across all nine siting cases, the POINT maxima are 13/14 (horizontal) and 68/68
(checker), BILINEAR 22/23 and 68/68, and LANCZOS 20/21 and 73/73. The reported
per-case MAE, changed fractions and distributions remain part of the fixed matrix;
no filter or rounding flags were searched to obtain a quality target.

The [pre source selection condition](https://github.com/FFmpeg/FFmpeg/blob/0a41fef0fa5a16c4b9830f7ae87f94cfe52e8b4e/libswscale/swscale_unscaled.c)
for the special planar420→YUYV wrapper requires FAST_BILINEAR or POINT flags.
Bicubic4 does not satisfy that condition. Equality of this pair therefore does
not show that the cited fix has no effect on every conversion path. The separate
fixed POINT/BILINEAR/LANCZOS matrix broadens this discriminator.
Nearest-neighbor can legitimately keep the same samples after a quarter-chroma
sample displacement, so equal POINT output alone is not proof of ignored siting.

The [fresh current decoder receipt](current-planar-decode-20261031.json) records
unmodified `/usr/bin/ffmpeg` 8.0.1-3ubuntu2 decoding each original ES with named
software MPEG1, hardware disabled, one decode thread and native `yuv420p` rawvideo
passthrough. Every retained planar Y/Cb/Cr byte matches over all 30/90 pictures:
4,561,920 and 13,685,760 bytes. It preserves full option sequences, ES/output and
provider hashes, and log hashes; only private input/output paths are normalized.

The [fresh old decoder receipt](old711-planar-decode-20261031.json) records the same
finite 30/90-picture replay with unmodified official Ubuntu 7:7.1.1-1ubuntu4.2
amd64. Its [official package](https://security.ubuntu.com/ubuntu/pool/universe/f/ffmpeg/ffmpeg_7.1.1-1ubuntu4.2_amd64.deb)
is 1,973,474 bytes with [official SHA256](https://packages.ubuntu.com/questing/amd64/ffmpeg/download)
`222f58432561f343ecd536cd69ff53d7714405aaa1e2605ba8fa7977a3ba204f`.
The exact package was verified and privately extracted; version/loader providers
were qualified before execution, with no installation or external binary/source
edits. Both old and current decodes reproduce every retained I420 byte. Thus all
four finite decodes (two fixtures per version, 240 pictures total) agree exactly
at the planar decoder-output stage. Exact provider/CLI hashes and option order
are retained; private CLI/input/output paths are normalized in the old receipt.
The pristine pre/post source pair above is a swscale-only comparison, not a fresh
decoder build containing those fixes. Native PTS and historical flags remain
unknown.

The separate [historical-reference byte replay](historical-reference-byte-replay-20261031.json)
finds exact identity for the horizontal neighbor first frame. Historical
horizontal Bicubic and Lanczos first frames and the whole 90-frame checker
Lanczos file differ from newly declared same-named cases. The fixed new Lanczos
center-to-left case meets the historical numerical bounds over all 30/90 frames
(horizontal 1/1, checker 22/22), while those original files remain different.
Matching names or bounds do not reconstruct the command.

Historical reference commands/flags remain **UNKNOWN**. The old first-horizontal
13/14, explicitly resited Lanczos first-frame 1 and checker 22 findings keep their
original scopes. A new Bicubic result does not establish the identical original
Lanczos recipe. The encoded/native frame tokens are packet identifiers, not PTS;
ordinal/25 timestamps in the reader are explicitly synthetic. No firmware cause,
general FFmpeg effect, broad MPEG1 equivalence or production support follows.

## Manual replay

The optional research reader/matrix needs Python 3 and NumPy. The converter needs
a C11 compiler, public FFmpeg development headers and libdl; it dynamically opens
the caller's exact libraries and links no FFmpeg implementation at build time.
The optional conversion/matrix replay does not install dependencies or run in
core CI. The separate synthetic arithmetic checks below run on both CI hosts.

```sh
python3 -B tests/mpeg1-chroma-pixels.py --self-test
cc -std=c11 -O2 -Wall -Wextra -Werror tests/mpeg1-chroma-convert.c \
  -ldl -o NEW_PRIVATE_CONVERTER
python3 -B tests/mpeg1-chroma-reference.py \
  --manifest LOCAL_FROZEN_MANIFEST.json --converter NEW_PRIVATE_CONVERTER \
  --output NEW_PRIVATE_RESULTS
```

The converter can also be invoked directly after matching I420/packed identity
controls for each provider/input. This example is the fixed center-to-left
Lanczos horizontal case; all output names must be fresh:

```sh
env -u LD_PRELOAD -u LD_AUDIT -u LD_LIBRARY_PATH NEW_PRIVATE_CONVERTER \
  post6507 /ABS/libswscale.so.10 /ABS/libavutil.so.61 10.2.100 61.10.100 \
  512 i420-yuyv center left 30 inputs/horizontal-sw.yuv420 \
  NEW_HORIZONTAL.yuyv NEW_CONVERSION.json
python3 -B tests/mpeg1-chroma-pixels.py \
  --candidate inputs/horizontal-card.yuy2 --reference NEW_HORIZONTAL.yuyv \
  --frames 30 --planar-luma inputs/horizontal-sw.yuv420 \
  --output NEW_METRICS.json
```

Identity modes are `i420-i420` and `yuyv-yuyv`, with equal source/destination
siting; independently replay their outputs with the reader's `--require-identical`
and corresponding `--format`. These controls compare every active input byte.

Start from [filter-manifest.example.json](filter-manifest.example.json). The
manifest supplies local file locations, immutable hashes, exact source/build
proof and paired library paths/full versions. This optional driver deliberately
requires the exact qualified pre0a41/post6507 source-build/adoption proof schema
used in the retained run; arbitrary providers do not substitute for that proof.
The converter/reader can be used independently for other declared providers.
Relative locations resolve against the manifest directory. The converter accepts
only flags 4/16/2/512, fixed 30/90
populations and I420/YUYV modes. Provider/dependency `dladdr` checks, full API
versions, identity checks and exclusive output creation remain mandatory.
The matrix requires every prescribed case, retains a first failure without
retry/overwrite, and rechecks all input/code/provider/output pins at completion.
It performs no fetch, upstream build, decoder invocation or new device capture.

The recorded measured-source hashes identify the exact tools that produced each
receipt. Packaged helpers and later filter results retain their own source hashes;
they do not rewrite the earlier executed-source identities. Raw media, external
DSOs and full per-frame logs are retained separately rather than committed here.
NumPy may be provided through a private `PYTHONPATH`; it is not a mandatory global
dependency. The separately source-qualified [fresh MPEG2 spatial control](https://github.com/ahnhy1324/crystalhd/issues/529#issuecomment-6540677844)
is now complete:30 pictures, exactY and correctly sited Cb/Cr maxima1/1.
Missing original recipes/nativePTS and actual hardware arithmetic remain open.

Repository preparation removes trailing blank lines from the converter, pixel
reader and example manifest. No program logic changes: the final compiled
converter is byte-identical to the measured filter-matrix executable
(SHA256 `53ee827c3e90bc105c0e78c114b7a4543ecdfb71ec124b5902e5253a1d6742ee`).
The final repository reader independently passes all7 hand-computable controls;
its AST equals the measured reader. Original run source hashes stay preserved
in their receipts.

## Fixed separable reference follow-up

The [additional observation](separable/observation-20261031.json) uses the same
retained planar/native bytes and raw order, without a decoder, converter or
new hardware run. Fixed masks built only from decoded chroma gradients first
partition the checker into interior, horizontal-edge, vertical-edge and crossing
support. Their immutable radius3 is a descriptive neighbourhood, not a claim
about hardware support. Against the earlier full-axis Lanczos reference, the
checker Cb/Cr maxima are respectively0,2,20,22 in those four classes.

The previously calibrated clamped half-up vertical rule
`(3*current + adjacent + 2)//4`, with the previous row for even output rows and
the next row for odd rows, matches every no-horizontal-edge sample exactly.
For each component this includes1,105,920 vertical-edge and1,382,400 interior
checker samples. The horizontal30 interior control also matches exactly.
Independent scalar oracles and literal border/parity controls precede reads.

Before any full-model result, two complete separable hypotheses were frozen:
normalized float64 horizontal Lanczos3 at source coordinate `j-0.25`, clamped
tap indices and half-up clipped8-bit output, combined with that fixed vertical
rule in **both** quantized orders. Neither order is selected or discarded.

| Complete raw population | H8 then V8 max Cb/Cr | V8 then H8 max Cb/Cr |
| --- | --- | --- |
| Horizontal30 |1 /1 |1 /1 |
| Checker90 |2 /2 |2 /2 |

All native Y bytes still match the same raw ordinal's planar Y. The minimum
horizontal distance from a rounding half-boundary is0.02779 for horizontal30
and0.08337 for checker90; no sample reaches the fixed1e-10 warning margin.
The two orders have different signed/count distributions, preserved in the
receipt, despite equal maxima. These observations demonstrate that a declared
reference with a different vertical kernel accounts for the large numerical
residual in these retained fixtures. They do not identify actual firmware
coefficients/order, recover original PTS/commands or prove broad codec quality.
The original full-axis Lanczos22 FAIL, local target3 and release oracles remain
unchanged; this is no production MPEG1 PASS or external-code repair.

Run all53 synthetic controls, without media or a frozen manifest:

```sh
make mpeg1-reference-model-check
```

NumPy is needed only for this optional developer check/research replay; CI
installs its own test dependency. Four private semantic mutations of mask
radius, vertical weights, horizontal phase and aliased operation orders are
rejected by literal controls. Product dependencies are unchanged.

The measured helpers are published byte-for-byte as
`tests/mpeg1-axis-topology.py`, `tests/mpeg1-vertical31-reference.py` and
`tests/mpeg1-separable-reference.py`. The three `separable/*inputs.json` files
preserve original private paths as evidence. For a local replay, make new
manifest copies and change input/helper paths to the locally retained exact
bytes; preserve sizes, hashes, geometry, rules and both orders. Helper identity
objects use an absolute resolved path, byte length and SHA256. A moved helper
keeps the published/measured byte hash. New manifest hashes are recorded
separately; historical manifests and raw captures must not be rewritten.

```sh
python3 -B tests/mpeg1-axis-topology.py --manifest LOCAL_AXIS.json \
  --output /ABS/NEW_AXIS_RESULTS
python3 -B tests/mpeg1-vertical31-reference.py --manifest LOCAL_VERTICAL.json \
  --output /ABS/NEW_VERTICAL_RESULTS
python3 -B tests/mpeg1-separable-reference.py --manifest LOCAL_SEPARABLE.json \
  --output /ABS/NEW_BOTH_ORDER_RESULTS
```

These readers require all30/90 pictures, regular read-only input descriptors,
pre/post hashes and metadata, exact Y, complete component census and exclusive
result directories. Full per-picture reports remain privately retained; their
hashes and complete component summaries are in the observation receipt.
