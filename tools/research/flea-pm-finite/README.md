# Finite PM playback adapter

This isolated #689 diagnostic runs the current maintained `library-drain.cpp`
input, decoder lifecycle, source-token/count/EOS checks and output leases.
It imports no old drain implementation and makes no installed-runtime changes.

The [native comparison and remaining scope](../../../tests/fixtures/issue689/native-finite-20261126.md)
records two actual down/wake/current-FLL cycles in one 180-picture session
using `host-dwell50-twice`, with matched OFF controls and official restoration.
Use that result to distinguish the immediate, single-dwell and repeated-dwell
stimuli below; none qualifies thermal benefit or normal product enablement.

The retained synthetic MPEG-2 fixture is `mpeg2.m2v`, 640×360 progressive,
180 pictures, 1,777,511 submitted bytes, SHA-256
`32d8ca6ddf8ad529ae3875ee9b7e904630c633bfef6d74ba40d7b1040d20820b`.
This is the existing fixture recorded in
[`native-mfd-address.json`](../../../tests/fixtures/issue92/native-mfd-address.json)
and the earlier #376 experiment at
`/home/mache/crystalhd-issue376-shadow.pY8ZL5/`.
The two historical 82,944,000-byte captures remain historical evidence.

## Explicit modes

All modes submit the same clear input. Each existing
`DtsFWPauseVideo(handle, 1)` / `DtsFWPauseVideo(handle, 0)` pair follows a
selected valid picture's successful actual release, before writing its owned
pixel copy. OFF is attempted once even if ON failed. The command order and
failure handling follow `e38a2f8:tests/flea-power-observer.h`,
`HostPauseAndResume`; no register observer is imported. The first three modes
issue one pair after output 1; the explicit repeat mode issues two pairs.

- `reference` forwards the ordinary `DtsSetColorSpace(YUY2)` call.
- `host` skips that setter, retaining the DIL's initialized YUY2 cache.
  Actual output must still satisfy the existing YUY2/payload/geometry checks.
- `host-dwell50` uses the same host path and requests one 50,000-microsecond
  host wait after a successful pause return, before the ordinary resume.
  A failed pause skips this wait and still attempts resume once. A signal
  during the wait does not skip resume; the original drain observes the
  interruption and retains its normal cleanup/result handling.
- `host-dwell50-twice` uses that same pair and wait after released outputs
  **1 and 90** within the same 180-picture decoder session. Each successful
  pause requests its own 50,000-microsecond wait. A pause or resume failure
  stops the ordinary drain; the pair already attempted keeps its actual
  statuses, and the current pair still attempts OFF exactly once. There is
  no third pair, module reload, session restart or physical-budget reset.

The repeat mode tests whether the unchanged finite2 mechanism can admit and
complete two physical transitions in one session with its existing
`attempts=2, window_ms=5000` ceiling. Four successful API calls and two waits
alone cannot establish two transitions: only the separate physical ledger
can do that. This 180-picture research stimulus is neither a 900-picture
integration qualification nor thermal/endurance acceptance.

The additional mode is an explicit idle stimulus for the finite2 observation.
The retained finite2 run completed its pause with RX0 still awaiting its Y
completion, then resumed about 35 microseconds after transaction retirement.
The driver posts that RX list only after a picture-available indication;
the waiting bit establishes outstanding ownership, not whether the bus was
transferring bytes at the sampled instant. While PENDING, normal IRQ handling
can retire that RX list and re-evaluate the unchanged physical-down conditions.
The wait tests whether that retirement occurs before the client resumes.

This wait follows the pause acknowledgement and host zero-FLL transition.
The historical `e38a2f8` driver waited after command posting, before that host
transition. These are different schedules. The new mode restores no product
delay, changes no driver or clock policy, and does not guarantee RX retirement
or entry. The previous `reference` and `host` modes retain their immediate
resume sequence. All compared OFF/ON/OFF arms must use the same selected mode.

The adapter adds no raw register or memory accesses in any mode. The
reference's existing public setter retains its own implementation. No
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
probe --hardware INPUT reference|host|host-dwell50|host-dwell50-twice ABSOLUTE_NEW_OUTPUT_DIRECTORY
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
The dwell modes add a `DWELL` row between each successful pause and resume,
with `requested_us=50000`, actual monotonic begin/end times and the released
picture count. The repeat mode requires two rows with
`after_released_outputs=1` and `90`, four `PAUSE` rows in ON/OFF/ON/OFF order,
and `pause_calls=4` in the final `END` row. The requested wait is not a hard scheduling-latency bound;
the existing process and playback deadlines still apply.
`input.bin` contains the exact ordered submitted access units. `output.yuy2`
contains 180 tightly packed 640×360 pictures (82,944,000 bytes) on completion.
Pixels are copied while leased and written only after successful release.
The tight layout is the returned DIL contract, not a new physical pitch reading.

The unchanged `Library drain:` result remains in stdout. A complete adapter
adds the exact footer:

```text
PM_EXERCISE result=PASS frames=180 inputs=180 pause=0 resume=0 mode=host
```

The other footers name their selected mode, including `mode=host-dwell50-twice`.
Pause/resume footer statuses are the last pair's statuses; every pair's actual
statuses remain in `events.tsv`, and any pair's failure prevents PASS.
Failure preserves the actual
statuses and partial files. PASS is a transport/count/EOS/cleanup/capture result,
not proof of a physical down/up cycle, thermal benefit or product qualification.
Actual transitions must be established independently from the finite-PM ledger;
a two-attempt mechanism ceiling does not establish two transitions.
