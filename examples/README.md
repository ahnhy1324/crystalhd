# Legacy examples

`hellobcm` and `mpeg2test` are retained as low-level diagnostics for the
historical `libcrystalhd` API. They are not general media players: both use
hard-coded elementary-stream paths under `/tmp`, assume particular stream
formats, and contain legacy format-change handling.

CI verifies only that the examples compile. Start with the top-level
[README](../README.md) and use the maintained GStreamer or VA-API frontend for
playback and hardware validation.
