# Kodi accounting and lifecycle source tests

`make kodi-compat-check` runs without Kodi, libva, a GPU, a decoder device or
network access. The pinned revision and each complete source/header and excerpt
hash are recorded in `source.json`. Every excerpt retains Kodi's
LGPL-2.1-or-later notice; the test scaffolds use GPL-2.0-or-later.

The existing publication test applies the repository's actual optional Kodi
patch in a temporary tree. Its original branch fails the controlled last-picture
drain assertion; the patched branch passes. That discriminator is unchanged.

The lifecycle test executes the extracted configured FLUSH branch and parent
dispatch, `COutput::Flush()` and processed-picture return methods,
`CDecoder::Reset()`, the actual PICTURE receive/initial drain statements, and
the complete `CVaapiBufferStats` class. It covers:

- Queued NEWFRAME, RETURNPIC and PICTURE messages, invalid payloads, decoded and
  processed queues, and exactly-once message/surface/reference retirement.
- Present-picture release before Reset's synchronous FLUSH; output cleanup
  before ACK; statistics reset only after the decoder receives successful ACK.
- Injected Actor error reply, undispatched timeout and dispatched-but-lost-reply
  modes exercise the actual Reset failure branches: statistics/error counter
  survive and the decoder enters its error state. These are shim inputs, not
  reproduced production failures. Unconfigured Reset releases its present picture.
- GPU-pending returns and an externally renderer-held old picture surviving
  successful Reset. The old picture may return after a fresh picture is queued;
  its return must not change the new picture's render count or identity.
- A fresh epoch's actual PICTURE consumer/drain accounting, repeated empty
  Reset, and configured FLUSH routing from all six configured states.

Both original and patched publication branches pass these lifecycle cases.
This adds coverage; it does not identify a new Flush/Reset fault. Six temporary
semantic mutations must fail the runtime oracle: skipped picture/message
release, premature ACK/statistics reset, reset on error, and omitted consumer
accounting. The original fixtures and external source files are never mutated.

Actor delivery, reference-count callbacks, backing storage and GPU readiness
are explicit scaffold boundaries. A PICTURE release can enqueue a RETURNPIC
after Flush has already drained outbound messages; the test preserves and later
processes it. `Flush()` itself does not reset statistics, and zero statistics
after successful Reset do not prove all externally held resources retired.
Full Actor concurrency, PRECLEANUP/buffer-pool destruction, AVFrame/VA/GPU work,
physical playback, Stop/seek acceptance and the separate FFmpeg stream-loop
296/300 failure are not tested here.

To check a retained unmodified full source/header against every tested excerpt:

```sh
python3 -B tests/kodi-vaapi-publish-order.py \
  --source /path/to/VAAPI.cpp --header /path/to/VAAPI.h
```

The full C++ file may differ elsewhere from the recorded upstream file, but all
tested excerpts must match. The optional patch must apply forward/reverse and
must leave every lifecycle excerpt unchanged. The header's actual statistics
class must match; no other header declarations are compiled by this test.
