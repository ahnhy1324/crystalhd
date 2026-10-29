# Native picture verification

`make native-picture-check` runs standard-library Python oracle tests and a
small C11 metadata source model. It does not open a device, start Kodi, attach a
tracer or change a module, installed file or external application.

The [retained observation](native-picture-observation-20261029.json) is an
offline analysis of existing 2026-10-27 native Kodi 22 captures. It preserves the
original route qualification separately from this new colour check:

| Capture | Strict source-invariant points | New sampled-colour result |
| --- | ---: | --- |
| 1280×720 | 0 | NOT_VERIFIED |
| 1920×1080 | 3 | FAIL: each has green-channel error 15 with fixed tolerance 2 |

Both have insufficient broad reference coverage. A valid counterexample can
fail the sampled check even when too few points exist to pass it. The three
FHD points have exactly the same Y/U/V and flat 3×3 neighbourhoods in every one
of 180 software-decoded source frames. No capture matching, first-frame guess
or tolerance adjustment selects them. All source/capture fingerprints, the
software decode command and the independent real-valued BT.709 model are
recorded. BT.601 is a diagnostic calculation; it never selects the expected
matrix or changes the primary verdict. Matched VA capture bytes are unavailable
in this native batch; the earlier 360p copied-shader proof is a different path.
The observation uses minimum 100 samples (the generic CLI defaults to 16).
Even a `PASS` means only the sampled oracle passed, not native playback acceptance.

The capture script paused at `onAVStarted` and reported clock 0.0 on either side
of RenderCapture. It did not bind the image to a particular source PTS or DRM
object. Raw native YUYV for those captures was not retained. Decoded YUYV,
source-bound GPU PTS, complete crop/pixels, controls, physical A/V/scanout and
performance remain unverified. The colour mismatch does not identify its stage
or prove a driver defect. CI tests use synthetic images and frozen-point
arithmetic; the full retained pixel replay requires the external source/capture
files and is not repeated or claimed by CI.

To replay an independently decoded packed YUV420p sequence and unmodified BGRA
capture, using the reference command/fingerprints in the receipt:

```sh
python3 -B tests/native-picture-oracle.py SOURCE.yuv CAPTURE.bgra \
  --width 1920 --height 1080 --frames 180 --step 32 --minimum-samples 100 \
  --matrix bt709 --reference-sha256 REFERENCE_SHA --capture-sha256 CAPTURE_SHA
```

The tool also accepts `-` for a streamed raw reference. It never decodes media
or infers a matrix itself; the caller must establish the raw reference's source
and declared limited-range SDR metadata. Nonpassing results exit 1; malformed
extent, hash or reference input exits 2.

`native-color-negotiation.py` executes eight licensed/hash-pinned excerpts from
the actual research node 830170b, FFmpeg 12cc1f42 and kernel 7.0 validation helpers.
It checks state-pure queries, identical TRY_FMT/S_FMT normalization, explicit
colour tuple preservation, busy isolation, and a default-negotiation mechanism:
direct DEFAULT commit can keep automatic HD selection, while committing the
returned normalized SMPTE170M value is indistinguishable from explicit BT.601.
Three source-semantic mutations must fail those guards. Ioctl routing, queue
busy state, locks, source dimensions and buffer sizing are explicit model
boundaries; no runtime negotiation or DMA behavior is replayed.

The [V4L2 format contract](https://docs.kernel.org/userspace-api/media/v4l/vidioc-g-fmt.html)
requires pure TRY_FMT and recommends G_FMT→modify→S_FMT. Query-history or equality
heuristics cannot safely distinguish normalized defaults from deliberate caller
values. No driver or third-party fix is applied. The historical ioctl colour
tuple and raw YUYV are unavailable, so this source mechanism remains separate
from historical attribution. #538 stays open; the heap-only follow-up is excluded.
