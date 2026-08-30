# Legacy examples

`hellobcm` and `mpeg2test` are retained as low-level diagnostics for the
historical `libcrystalhd` API. They are not general media players: both use
hard-coded elementary-stream paths under `/tmp`, assume particular stream
formats, and contain legacy format-change handling.

CI verifies only that the examples compile. Prefer the GStreamer or VA-API
hardware tests documented in the top-level [README](../README.md) for current
end-to-end validation.
