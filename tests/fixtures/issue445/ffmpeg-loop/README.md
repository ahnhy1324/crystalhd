# Retained FFmpeg field-rate loop boundary

`make ffmpeg-compat-check` runs offline with Python and a C11 compiler. The
fixtures retain exact FFmpeg n8.0.1 excerpts, each source's original leading
license notice, full-source and excerpt hashes, and commit-based primary links
in `source.json`. To additionally verify retained full sources:

```sh
python3 -B tests/ffmpeg-vaapi-loop.py --source-dir /path/to/ffmpeg-n8.0.1
```

The test executes the actual zero-reference Bob parameter setup, frame queue,
EOF request and pipeline-uninit functions, graph cleanup, and hardware-context
identity comparison. Frame IDs, requested parities, normalized PTS and every
input/output reference release have separate ledgers. VA calls, graph allocation
and downstream delivery are shims; no pixels, full FFmpeg, device or GPU run.
The unchanged upstream signed/unsigned interfaces need two scoped warning
exemptions; the surrounding scaffold still uses `-Wall -Wextra -Werror`.

One 150-picture queue plus final EOF produces 300 fields. Three 50-picture queues
with two teardowns before EOF produce 296, omitting fields 98/99 and 198/199.
Explicit EOF before every teardown is a separate counterfactual 300-field
control, not a proposed application patch. Frame-rate output, irregular PTS,
temporary upstream absence, upstream error and repeated EOF are controls.
Five semantic negatives must fail their exact expected oracle: missing EOF
drain, missing timestamp lookahead, incorrect next-PTS interpolation, missing
queued-reference release, and comparing AVBufferRef wrappers instead of their
backing identity.

`observation-20261029.json` independently reconciles the retained 2026-10-19
native artifacts. The original loop result stays **FAIL, 296/300, exit 0**;
its subsequent Weave-reopen stage was not run. The separately measured concat
control retains its 300-field PASS and fresh 50-picture reopen. All retained
fields match the same-run Weave-derived CPU oracle in full NV12 hashes,
durations and PTS; this is not a software-decoder colour golden.

The two native hardware-context reconfiguration messages, exact output gaps,
successful decode/VPP call counts and extracted-function behavior identify a
matching mechanism. They do not directly observe the native internal
`send_frame → configure_filtergraph → pipeline_uninit` call order or prove the
old binary equals a full rebuild of this upstream source. The test deliberately
applies the observed graph boundaries; it does not execute the CLI scheduler.
No CrystalHD production repair, current-master playback qualification, physical
presentation or general looping support follows. Do not fabricate a temporal
reference, retain an invalid hardware context, or synthesize an unrequested
field to hide the compatibility failure.
