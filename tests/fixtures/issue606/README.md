# Natural MPEG-1 holdout, 2026-10-31

The receipt records two physical BCM70015 sessions of one independently
licensed natural clip. All 1,250 pictures and 253,440,000 active YUYV bytes
repeat exactly. Real container packet/frame PTS is joined through identical
ES access units and returned input identities; numeric token order is never
used as presentation time. Both unchanged separable references have Cb/Cr
maximum error2. Direct Y has maximum3 (five samples), so this is not bit-exact
luma and does not identify the chip's IDCT or rounding arithmetic.

`metrics-all-frames.json` contains the complete absolute and signed histograms,
percentiles, exact/change counts and frozen topology/parity partitions for
both orders and sessions. The observation receipt pins the retained per-frame
hashes, picture/PTS proof, error maps, captures, separate logs, qualifications,
operator and eight same-conversion visual comparisons. These large artifacts
remain privately retained under the named cache evidence directory; they are
not downloadable fixtures in this repository. The receipt is evidence, not a
portable absolute-path manifest or public codec-support claim.

The film is **Tears of Steel (2012)**, attributed to **(CC) Blender Foundation |
mango.blender.org**, [CC BY3.0 film license](https://mango.blender.org/about/),
from the [official720p source](https://download.blender.org/demo/movies/ToS/tears_of_steel_720p.mov).
The download directory's `copyright.txt` concerns the OST and is not used as
the film license. No third-party video is included here. The encoded clip is
the original120..170s interval, authored as50s/25fps/352x288 with an active
352x146 film and black letterbox. This length preserves the existing256MiB
capture cap. The full source identity and exact FFmpeg8 producer arguments,
provider identities and three retained coefficient-clipping warnings are in
the receipt. The original movie matrix tags are unknown: the declared
709-to601 producer transform is an explicit fixture assumption, not recovered
metadata. Software and hardware compare the very same MPEG-1 bytes.

Run the media-free checks with NumPy available:

```sh
make mpeg1-reference-model-check
# Existing53 model controls plus38 mapping and10 numeric controls.
python3 -B tests/mpeg1-natural-audit.py --self-test
```

For a retained-data audit, construct an actual `crystalhd.issue606.natural-inputs.v1`
manifest using the schema consumed by `tests/mpeg1-natural-audit.py`: population
352x288/1250/25, both orders, pinned unchanged three model helpers, exact
collector source/binary/generation0, actual ES/container/software-I420,
selected video packet/decoded-frame and stream tables, and two distinct
captures with separate stdout/stderr and normalized completed root-operator
qualification receipts. Every pin is `{path, bytes, sha256}`. Include the ES
decoded-frame table for the independent ES/container display-order crosscheck.
Provider/source and completed safety evidence must describe the real files;
do not populate booleans from desired outcomes.

```sh
python3 -B tests/mpeg1-natural-audit.py \
  --manifest /absolute/frozen-actual-inputs.json \
  --output /absolute/new-exclusive-result-directory
```

Acquire captures only through the existing explicit test route:
`library-drain-test --hardware INPUT.m1v 1250 15 1 --scaler-test 0
--mpeg1-via-mpeg2 --capture-yuy2 NEW_PATH`. Reserve the device/host, keep AC and
temperature below75C, observe the actual direct child and device owner, retain
normal EOS/queue/FD/thread cleanup, and compare stable module/firmware/pin/AER
boundaries plus ordinary MPEG-2/H.264 recovery. No build, other media or large
offline audit runs concurrently with actual media. Never silently retry,
overwrite the first failure, raise the capture cap, change playback policy,
reset/unload hardware or use selector5. The existing ordinary collector's
device-free preflight is separate from hardware execution.

The original offline predicates incorrectly treated guessed raw-ES nominal
rate as authoritative and accepted only absent pulldown information. The
retained failures are preserved: the corrected source proof requires average25,
sequence-header rate code3 and all actual40ms container PTS; own public enum9
is `vdecFrame_X1`. Wholeframe0/9 is accepted and field/repetition patterns are
rejected. The corrected controls preserve raw metadata and every quality
threshold/model. The separate H.264 observer exit race and unsupported-MP4
preflight are also retained, without relabeling their operator FAILs.

Normal frontend playback, audio-clock/no-drift logs and any conditional codec
admission have their own gates in #606. Calibrated physical lip-sync is not
required under the current acceptance policy. A still comparison or headless
capture alone does not qualify cadence, scanout, A/V or a public frontend.

The subsequent [optional-plugin receipt](experimental-codec-20261031.json) records scoped experimental support, staged default-policy playback, eight actual sink-buffer identities and removal/recovery. The original observation remains the historical pre-frontend HOLD, with every numerical residual unchanged.
