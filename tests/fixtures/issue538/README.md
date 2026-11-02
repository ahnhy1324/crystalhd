# Native picture verification

`make native-picture-check` runs standard-library Python oracle/barcode tests and a
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

## Fresh controlled captures on 2026-10-29

The [fresh hardware receipt](native-picture-capture-20261029.json) records new
clips, actual unmodified Kodi DRM/EXPBUF captures and stable-module restoration.
Historical evidence above is unchanged.

| Fresh capture | Invariant points | Points above tolerance2 | BT709 result |
| --- | ---: | ---: | --- |
| 1280×720 | 794 | 585 | FAIL; maximum channel error25 |
| 1920×1080 | 1,798 | 1,329 | FAIL; maximum channel error25 |

Both exceed the frozen minimum100 points. A source-only mask excludes y<128
before capture, where two48-bit barcode replicas change with each frame. Magic,
source case, ordinal, complement and CRC8 bind720p content to ordinal0/PTS0 and
FHD content to ordinal1/PTS512, at1/15360. These identify content, not a DRM
object or physical display time. BT601 is diagnostic (error0) and changes no FAIL.

Each actual colour trace has10 matched PRE/POST pairs and END0/0/10/0. Initial
DEFAULT0x0 becomes16x16/SMPTE170M; final HD CAPTURE still reports SMPTE170M.
Source-address warnings and15,688/14,314 null-map diagnostics remain recorded.
The pixel/barcode verdict reads independent captured files. Each route returns
180 originals and true LAST, exits0, and restores stable3.10.13 identity/ref,
PM/link/AER with kernel-check0.

The inert public fixture helper generates bytes matching every frozen producer
frame at both resolutions. Twelve corruption controls also run under `python -O`.
Source tables retain all source PTS and eligible YUV points. Replays require
the exact media/reference/capture hashes; source encoding metadata is recorded.

```sh
python3 -B tests/native-picture-fixture.py --width 1280 --decode CAPTURE.bgra
python3 -B tests/native-picture-fixture.py --width 1280 --generate-yuv420p -
```

The generation command writes all180 packed YUV420p frames to stdout for a
separate software encoder. Add `--minimum-source-y 128` to the oracle replay
with180 frames, step32, minimum100, declared BT709 and exact hashes. This mask
is frozen from source properties. Full pixels/crop and physical presentation
remain separate requirements.

A separate normal FFmpeg YUYV/MMAP epoch retains all180 frames per resolution
(331,776,000/746,496,000 bytes). Independent offline checks verify every barcode,
frame hash and rational source PTS;142,920/323,640 pre-frozen point-frame YUV
triplets match with component error0. The first720p collector remains FAIL due
to an exited-child `/proc/fd` race, although its complete corpus is valid.
The later FHD collector is PASS. Both batches restore stable/kernel boundaries.
These are separate decoder opens, not the Kodi EXPBUF buffers; complete raw
retention and sampled YUV equality do not claim full-frame reference equality.

The native decoder committed the initial 16×16/SMPTE170M format default as a
caller hint and kept reporting it after discovering an explicitly BT709 HD
stream. The FORMAT_CHANGE path did not consume the firmware colour description.
The original compact PIB field name, `colour_primaries`, concealed a complete
packed matrix/primaries/transfer tuple on the ordinary BCM70015 H.264 path.

`flea-color-word-20261029.json` qualifies the original firmware producer and
publisher by complete image, function and instruction-region hashes. The
publisher copies the word unchanged to compact PIB offset40:

```text
word = (matrix_coefficients << 24) | (colour_primaries << 16)
       | (transfer_characteristics << 8)
```

`flea-color-word-hardware-20261029.json` independently checks the interpretation
using the original module and original FFmpeg. All six controls preserve the
same compressed picture data and decoded YUV. Actual metadata words match the
predictions; all180 returned frames pass replicated barcode/CRC, framemd5,
rational source-PTS and142,920 source-invariant YUV point comparisons. Kernel,
module, power/link/AER and complete dynamic-debug restoration checks pass.

| Primaries | Transfer | Matrix | Full range | Observed word |
| --- | --- | --- | --- | --- |
| 1 | 1 | 1 | 0 | `01010100` |
| 6 | 6 | 6 | 0 | `06060600` |
| 6 | 1 | 1 | 0 | `01060100` |
| 1 | 1 | 6 | 0 | `06010100` |
| 1 | 7 | 1 | 0 | `01010700` |
| 1 | 1 | 1 | 1 | `01010100` |

The fix applies a complete supported source tuple to CAPTURE before publishing
SOURCE_CHANGE and advertises extended pixel-format fields. OUTPUT remains a
hint. Absent, partial, reserved, unsupported or nonzero-low-byte descriptions
retain the existing normalized fallback. Every format announcement starts
from that stream's hint, preventing a previous parsed tuple from leaking into
an unqualified result. G_FMT and TRY_FMT remain pure; CAPTURE S_FMT does not
request a colour conversion.

The firmware discards `video_full_range_flag`; this word cannot establish range
or the VUI presence flags. An explicit caller quantization remains a
compatibility hint; DEFAULT retains the V4L2 YUV default and does not mean
unknown. This change does not establish generic full-range correctness, correct
rendering of mixed tuples by every original client, or delivery of every
intra-stream colour-only notification. The qualification applies to the exact
original BCM70015 ordinary H.264 path, not other firmware, codecs or silicon.

Run the source-compiled node regressions with `make v4l2-node-check`. They cover
the normalized-default round trip, parsed-source priority, independent tuple
components, atomic rejection, query purity, CAPTURE negotiation, range-hint
preservation and new OUTPUT reset. Fourteen executions cover seven kernel API
versions with and without ASan/UBSan. The barcode helper additionally supports
an explicitly declared control-case identity and rejects a mismatched case:

```sh
python3 -B tests/native-picture-fixture.test.py
python3 -O -B tests/native-picture-fixture.test.py
```

The node regression now also executes the extracted production `chd_format`
and `crystalhd_rx_pause_format` functions together. Its 22 additional scenarios
cover initial discovery and matching-allocation reuse, same-size BT709→BT601
and absent/unspecified fallback announcements, changed geometry, boundary
buffers in slots0/16/31, and invalid geometry, stale epoch, failed stop, DMA
fault and still-owned retirement. Independent expected tuples are checked at
SOURCE_CHANGE publication; LAST, payload, event ordering and preservation of
OUTPUT hints are observed separately. Eight compiled mutations of temporary
extracted code must each fail a runtime assertion. These include omitted or
late colour publication, suppressed dynamic events, missing retirement/ownership
guards/LAST, ignored pause failures and omitted general buffer return.

This is direct format-handler coverage with one modeled active registration
and modeled VB2/event, capture-retirement and `chd_return_active` edges. The
latter only verifies that the separately handled boundary is absent before
general return; this test does not execute its remaining-buffer loop.
`chd_receive` does not supply the format flag in these tests. They do not
establish that firmware emits all intra-stream colour-only notifications,
real DMA/GPU retirement, or new hardware playback acceptance. Production code
and the earlier runtime receipt are unchanged.

The BT601 source table in `native-color-control601-20261029.json` was frozen
from an independent software decode before native capture. Source selection
uses only flat3×3 Y/U/V neighborhoods unchanged in every source frame, with the
declared source-only top mask, minimum100 points and fixed channel tolerance2.
The original BT709 fixtures and independent RGB oracle are published at
master commit `04fb4654924717617cccba367ee3bab49114f30a` under this fixture
directory and `tests/native-picture-oracle.py`. Earlier colour failures and
the excluded shutdown heap investigation remain historical evidence. Issue538
stays open; this research fix does not promote the native backend to master or
establish complete playback or physical presentation acceptance.

## One actual CAPTURE/EXPBUF active image — 2026-10-30

[`native-expbuf-picture-20261030.json`](native-expbuf-picture-20261030.json)
qualifies one complete active image from the actual original native Kodi queue.
It is evidence on the separate #579 → #581 research lineage, preserving frozen
`2399430`; it does not complete K2b, close #538 or promote a native backend.
The evidence parent is `f759aae`; runtime uses own module `fca6d9b`, image SHA256
`35fcda6c642342a779f0c2e6c81ef4007c06f392fd817e40e8b82ea8f192e0a2`,
version `3.10.14`/srcversion `DF1416C32049D4DADE81EB2` on `7.0.0-38-generic`.
This receipt changes no production code or workflow; its new head requires CI.

The client is the unchanged published Kodi22RC1 executable and unchanged published
native FFmpeg `12cc1f42` in the existing private prefix. Actual native mappings
are pinned, including libavcodec `c92a041d…075c8c6`. This differs from the ordinary
stock VA/App-bundled-FFmpeg control result; those library routes are not mixed.
All external sources/binaries and the frozen research baseline remain unchanged.

The new six-second, 180-picture 720p30 input has spatially distinct luma and
neutral U/V=128. Before hardware, the software preparation verifies every source
PTS/barcode, all neutral chroma and a second direct Y/chroma reference pass.
V1 fails missing encoded colour metadata; V2 fails non-neutral software output.
Both failures remain. A frozen two-frame input-declaration A/B locates an
unintended conversion through RGB in our invocation. V3 explicitly declares the
raw input's TV/BT709 properties before `-i`, verifies the actual SPS and retains
zero tolerance. No external code or post-capture tolerance/PTS adjustment is used.
The final encoded input is `abf31871…0071513`; the complete software YUYV oracle
is `ca930e9b…dc92403`. Full hashes and preparation manifests are in the receipt.
This neutral oracle does not qualify arbitrary colourful chroma interpolation.

Two bounded metadata observers are armed before media. During published Kodi
pause/RenderCapture, all 31 owned threads become T. A is stopped and fully drained;
its successful DQBUF/EXPBUF replay identifies a held CAPTURE allocation, not an
active producer buffer. B continues through copying, resume, EOF and descriptor
closure. Root duplicates only the selected actual exported fd through normal
`pidfd_getfd`, uses DMA-BUF START/READ → read-only shared mapping → bounded RAM
copy → END/READ, closes that mapping/duplicate, then unconditionally sends
SIGCONT before cancelling an independent watchdog. Reference comparison and file
output follow CONT; helper hashing/base64 stays bounded RAM during the stop.

| Observation | Recorded result |
| --- | --- |
| Actual object | video fd33, EXPBUF fd35, CAPTURE index1/plane0, allocation1/stream1 |
| Successful calls | EXPBUF TID1083551/call38; DQBUF same TID/call106, sequence1 |
| Internal tracking | 2 microseconds; not original source PTS |
| Declared format | 1280×720 YUYV, actual stride2560, query/image length1,846,052 |
| Copied image | all1,843,200 active bytes, crop0,0,1280,720; mismatch0/tolerance0 |
| Raw and RGB barcode | both ordinal1; independent sourcePTS512 at TB1/15360 =1/30s |
| Raw SHA256 | `930b38df951f591e5e81599b19c60d72391e083ec4a8e46329de02dca4cacb56` |
| Observed stop/copy | STOP→CONT724.713014ms; copy span10.233606ms |
| Continuous ownership | same export/DQBUF syscall IDs and epochs; no requeue/reset/close/fd replacement through copy end; selected QBUF and fd close after copy |
| Natural tail | 180 nonempty input QBUFs /180 nonempty capture DQBUFs, one empty LAST; zero observed EPIPE returns (NOT_OBSERVED) |

The earlier prose mixed an expected/synthetic EPIPE branch with actual capture.
All833 actual observer-B POSTs return0; EPIPE is NOT_OBSERVED. Independent replay
passes72 checks/93 artifact hashes, including full bytes/CRC/lease/restoration;
its first wrong one-EPIPE assertion remains retained without a runtime retry.

The declared 2,852 extra bytes and page-allocation padding are outside the active
image comparison. The copied allocation is the actual EXPBUF object from this
queue; matching raw/RGB content alone does not establish an exact GPU import or
render fd. Internal tracking1…180 is an API census, not independent source-PTS
proof for all180 pictures. No physical scanout/A/V, cadence/performance, interlaced
Bob, general colour interpolation or complete client/GPU return graph is claimed.

The actual controller exits0, passes its kernel check and restores its experimental
boundary; maximum temperature is60°C. The outer root-only activation wrapper
records no errors and exact restoration of stable `3.10.13`, all dynamic-debug
flags, dependency presence, absent video nodes, firmware, PCI links, AER/resource
boundary and kernel interval. The controller itself performs no module operations.
CPU observer01's signed-format FAIL, observer02/03 qualifications, four watchdog
cases (including actual1.8s continuation), 12 fake-copy API checks and 23 synthetic
reader controls remain separate from this actual DMA-BUF image result. The old
0.3s collector bound is explicitly unqualified; the observed621.099ms CPU drain
justifies the pre-hardware0.9s bound while keeping the1.8s watchdog, absolute2s
CONT limit and media QoS/deadlines unchanged. All earlier failures and the scoped
ignored heap disposition remain; no heap investigation was performed.

With the preserved files named by the receipt, independently replay content
without opening a device. Raw images and the large oracle are not committed:

```sh
python3 -B tests/native-picture-fixture.py --width 1280 --source-case 1 \
  --format yuyv --decode /path/to/same-expbuf-active.yuyv
python3 -B tests/native-picture-fixture.py --width 1280 --source-case 1 \
  --format bgra --decode /path/to/native-pause.bgra
python3 - /path/to/same-expbuf-active.yuyv /path/to/software-reference.yuyv <<'PY'
import hashlib, pathlib, sys
raw = pathlib.Path(sys.argv[1]).read_bytes()
with open(sys.argv[2], 'rb') as reference:
    reference.seek(1 * 1843200)  # Independent oracle fd; never a shared EXPBUF fd.
    expected = reference.read(1843200)
assert len(raw) == len(expected) == 1843200
assert hashlib.sha256(expected).hexdigest() == (
    '930b38df951f591e5e81599b19c60d72391e083ec4a8e46329de02dca4cacb56')
assert raw == expected  # Entire active image, tolerance0.
print('ordinal1 full active YUYV equality; no device or display claim')
PY
```

First verify whole input/oracle/raw hashes against the receipt. The full metadata
replay also uses the pinned private observer ABI/reader and continuous A/B traces;
CRC/image replay alone does not reconstruct the lease proof or a new hardware run.

Actual full-scale Kodi720p/FHD and same-size range transitions (2026-11-02)
are recorded in [the Kodi receipt](kodi-fullscale-20261102.json) and
[the transition receipt](same-size-range-staging-20261102.json). The former
contains both original/corrected-module batches, source extrema and sampled
RGB results; the latter preserves pre-fix diagnostic and strict failures
separately from the corrected control, both range directions, final-picture
DRAINING and EXPBUF/GPU passes. Neither completes broader #538 acceptance.

Generate independent source-only fixtures without opening a device:

```sh
python3 -B tests/native-kodi-fullscale-fixture.py --width 1280 --output /tmp/fullscale720
python3 -B tests/native-kodi-fullscale-fixture.py --width 1920 --output /tmp/fullscale1080
```

Use a new output directory. Encoding uses the installed unmodified FFmpeg and
libx264; exact media hashes can vary with encoder builds. Replay a retained
capture against its corresponding frozen software source with
`tests/native-picture-oracle.py`, `--frames 30 --minimum-source-y 128
--minimum-samples 100 --matrix bt709 --range full` and the receipt's exact
geometry, reference/capture hashes. The two new TSV files preserve every fixed
YUV/expected-RGB/captured-RGB row. They do not substitute for all-pixel or
physical presentation measurements.

The [EGL range-rebind receipt](egl-range-rebind-20261102.json) keeps the later
single-file Kodi limited→full PASS separate from its round-trip RGB FAIL.
The same failure is independently reproducible without opening CrystalHD:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror -Itests \
  $(pkg-config --cflags egl glesv2 gbm libdrm) tests/yuy2-range-rebind.cpp \
  $(pkg-config --libs egl glesv2 gbm libdrm) -o /tmp/yuy2-range-rebind
/tmp/yuy2-range-rebind
```

The process needs access to `/dev/dri/renderD128` and `/dev/dma_heap/system`.
The tested host's heap permission requires an explicitly privileged invocation;
the first unprivileged attempt stops before allocation and is not a pixel test.
The probe rejects a start temperature at/above75C, uses16KiB per DMA-BUF allocation,
uses128×64 neutral bars and checks all pixels after each completed GPU draw.
It changes no module, firmware, renderer binary or installed preference.

Output labels encode reuse of the DMA-BUF, input external texture and Picture
bundle. That last bundle includes programs, output texture and FBO; it is not
a shader-program-only experiment. On the retained965GM/Mesa26.0.8 stack, all
four reused-input-texture return-to-NARROW checks fail at max20; fresh input
textures return at error0. The strict exit1 is the expected reproduction of
this measured failure, never a PASS. Other implementations can return0 if
every RGB check passes. Kodi's actual NARROW import logs and the independent
native90-picture point/raw/GPU controls retain their separate attribution.
