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
