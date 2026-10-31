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
