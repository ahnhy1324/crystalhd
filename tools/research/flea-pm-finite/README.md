# Finite PM playback adapter

This isolated #689 diagnostic runs the current maintained `library-drain.cpp`
input, decoder lifecycle, source-token/count/EOS checks and output leases.
It imports no old drain implementation and makes no installed-runtime changes.

The retained synthetic MPEG-2 fixture is `mpeg2.m2v`, 640×360 progressive,
180 pictures, 1,777,511 submitted bytes, SHA-256
`32d8ca6ddf8ad529ae3875ee9b7e904630c633bfef6d74ba40d7b1040d20820b`.
This is the existing fixture recorded in
[`native-mfd-address.json`](../../../tests/fixtures/issue92/native-mfd-address.json)
and the earlier #376 experiment at
`/home/mache/crystalhd-issue376-shadow.pY8ZL5/`.
The two historical 82,944,000-byte captures remain historical evidence.

## Two explicit modes

Both modes submit the same clear input and issue exactly one existing
`DtsFWPauseVideo(handle, 1)` / `DtsFWPauseVideo(handle, 0)` pair immediately
after the first valid picture's successful actual release, before writing
its owned pixel copy. OFF is attempted once even if ON failed. The command
order and failure handling follow `e38a2f8:tests/flea-power-observer.h`,
`HostPauseAndResume`; no register observer is imported.

- `reference` forwards the ordinary `DtsSetColorSpace(YUY2)` call.
- `host` skips that setter, retaining the DIL's initialized YUY2 cache.
  Actual output must still satisfy the existing YUY2/payload/geometry checks.

The adapter adds no raw register or memory accesses in either mode. The
reference's existing public setter retains its own implementation. Neither
mode selects `capture_path` in the original drain, so `PackingState` is not
called. Pixel capture is a separate validated owned copy. The original
`OutputLease` releases every successful fetch once, including invalid output;
failed releases are never retried by the adapter. Unknown, duplicate, encrypted,
non-PIB or invalid geometry outputs cannot trigger the pause pair.

## Build and invocation

```sh
python3 -B tools/research/flea-pm-finite/build.py /absolute/new/build-directory
/absolute/new/build-directory/probe --preflight /path/to/mpeg2.m2v
```

The build compiles the current DIL and adapter without installing either,
then runs focused normal and ASan/UBSan observer controls and the pinned
device-free preflight. Each invocation preserves its sources, commands and
first failure in a new directory. It never opens a decoder or loads a module.

Only the separately reviewed native coordinator invokes:

```text
probe --hardware INPUT reference|host ABSOLUTE_NEW_OUTPUT_DIRECTORY
```

There is one 180-picture run with the maintained 60-second deadline. The
coordinator must bound process lifetime, own the device exclusively, preserve
module/driver/firmware identity, collect the separate finite-PM ledger and
restore the complete official 1.3 baseline. This probe does not load/unload
modules, read debugfs, restart a faulted device or decide safe restoration.

## Records and scope

`events.tsv` records exact input tokens/lengths/status, returned output
metadata and flags, release status, pause/resume status and monotonic host
timestamps, color-setter selection, capture counts and process resources.
`input.bin` contains the exact ordered submitted access units. `output.yuy2`
contains 180 tightly packed 640×360 pictures (82,944,000 bytes) on completion.
Pixels are copied while leased and written only after successful release.
The tight layout is the returned DIL contract, not a new physical pitch reading.

The unchanged `Library drain:` result remains in stdout. A complete adapter
adds the exact footer:

```text
PM_EXERCISE result=PASS frames=180 inputs=180 pause=0 resume=0 mode=host
```

The reference footer ends in `mode=reference`. Failure preserves the actual
statuses and partial files. PASS is a transport/count/EOS/cleanup/capture result,
not proof of a physical down/up cycle, thermal benefit or product qualification.
Actual transitions must be established independently from the finite-PM ledger;
a two-attempt mechanism ceiling does not establish two transitions.
