# Original-provider no-clock 900-picture diagnostic

This OWN client uses unchanged original App FFmpeg and a maintained VA backend.
It tests source population without Kodi's renderer, media clock, BWDIF or
NONREF feedback. It does not qualify stock real-time playback, presentation,
independent pixel correctness or a release. Existing source-population FAILs
remain unchanged.

The fixed input contract is the retained 1920×1080 H.264 MPEG-TS fixture with
900 independently established pictures. The client discovers its video stream,
admits every selected nonempty packet unchanged, requires all three discard
fields to remain DEFAULT and rejects software fallback, corrupt frames and
nonzero decoder error flags. A successful report requires 900 video admissions,
900 VAAPI frame returns, 900 successful retained
frame readiness/downloads, input EOF, accepted explicit decoder drain and
decoder EOF. Accepted packet PTS and returned best-effort labels must each be
900 distinct raw integers with equal sets; bounded scalar sets check this
without a clock conversion or a retained pixel map. The external native trace must independently establish original
submission/materialization/ownership counts; a tool return is not that proof.
This client passes demux packets with their original stream time base. Kodi's
private timestamp conversion and per-packet option choices are not reproduced;
numeric packet/frame labels cannot be equated to prior Kodi labels by this run.

The public feed/drain and ownership pattern follows `vaapi-seek.cpp`. Eight
later returned frames precede synchronizing the oldest, with at most nine
retained frames and one reusable software NV12 download. After decoder EOF the
client explicitly frees the decoder before downloading its retained tail;
the AVFrames retain the surfaces/device. It changes no DIL or driver ownership
rule. Readiness has a finite 100ms budget. A readiness failure, 150s overall
deadline or tool-specific limit is retained as such, not an impossibility proof.
At most 4,096 demux packets and 900 selected frames are accepted. JSONL metadata
is streamed to a fresh mode0600 file; no 900-frame pixel collection is retained.

Only the root executor builds/runs this during the serialized hardware task.
Build the isolated binary outside the source worktree, against the unchanged
12cc public headers used by the existing post-filter reference:

```sh
SDK=/home/mache/.cache/crystalhd-v13-distribution-20261102/prefix-a/include
SOURCE=/home/mache/.cache/crystalhd-native900-client-20261104
BIN=/home/mache/.cache/crystalhd-native900-run-20261104/original-decode900
c++ -std=c++17 -O2 -Wall -Wextra -Werror -I"$SDK" \
  "$SOURCE/tests/vaapi-original-decode900.cpp" -o "$BIN" -ldl -pthread
```

Like `vaapi-postfilter-reference.c`, this binary dynamically resolves the exact
original versioned providers; it does not link host FFmpeg. Run it in the
existing original-App/Platform namespace, with the original App read-only at
`/app`, the matching Platform at `/usr`, the private maintained VA/DIL paths,
the existing card/render-node bindings and a fresh writable payload directory.
The caller preserves original material/provider pins and records mapped images.
The source fixture may be mounted at `/fixture.mp4`; its suffix does not change
the detected MPEG-TS input contract. Inside that namespace:

```sh
/reference /fixture.mp4 /dev/dri/renderD128 /payload/decode900.jsonl
```

Arguments are exactly `INPUT VA_DEVICE FRESH_REPORT`. Providers are fixed to
`/app/lib/libavformat.so.63.1.102`, `libavcodec.so.63.1.102`,
`libavutil.so.61.1.102`, and the namespace's
`/usr/lib/x86_64-linux-gnu/libva.so.2`. Each called API is resolved to its exact
provider; FFmpeg runtime versions must match the original headers. No external
library is rebuilt or replaced.

The executor sends SIGTERM to its identity-pinned owned process on cancellation;
the handler requests normal decoder/frame/context cleanup. There is no SIGKILL
or implicit timeout subprocess. A blocked cleanup must retain the process and
resources under the existing handoff rule. Root verifies exit, native ledger,
kernel and full restored boundary separately. Wall time and the calling
thread's CPU time are separate raw metrics; the latter excludes other threads
and is not total application CPU or a real-time throughput qualification.
