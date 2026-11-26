# Finite PM: identify the selector that prevented entry

The first finite1 comparison completed four 180-picture runs and restored the
official 1.3 environment. Its ON(2 attempts, 5000 ms) ledger recorded **zero
claims and zero physical attempts**. That is successful transport and cleanup,
not a sleep/wake qualification. The original run is preserved at
`/home/mache/.cache/crystalhd-pm-finite-run1-20261126/` and its source at `83f1e9c`.

The current source has no later assignment that disables `FleaEnablePWM`.
The compiled initializer calls the research arming decision before START.
Zero attempts therefore cannot be explained by the admission timer expiring:
the timer starts only with the first admitted physical down.

There is a relevant historical timing difference. The successful #376
`e38a2f8` path waited 50 ms after posting a firmware command; the maintained
path uses IRQ completion directly, following the earlier `0085c02` change.
The retained old pause/resume calls took 50.870/50.926 ms, while finite1 ON
took 1.328/2.094 ms. The old down followed transaction release near the end of
that pause call. These observations identify a scheduling difference; they
do not identify which current RX, IRQ or other owner prevented entry.
No delay or physical predicate is changed to reproduce the old timing.

## What finite2 adds

`3.10.25-pmfinite2` preserves finite1's physical attempt ledger, limits, default
OFF mode and cleanup rules. It adds a separate, bounded observation of the
current selector and its final setter check. The physical register helpers,
ownership conditions, target reads/writes and transition timing policy are
unchanged. Observation itself adds some host execution time.

Each initialization receives a numeric context sequence and increments an
arming counter. This distinguishes the temporary setup context from the real
decoder without claiming an experiment session. Owner fields are copied only
where the existing hardware lock is already held; a lock assertion accompanies
the observer. This is not proof that every independently published device field
forms an atomic snapshot, or proof of firmware quiescence.

The observation table has two reserved banks of 32 entries each:

- Routine selection records.
- Pause-intent records: explicit PAUSE/RX_PAUSE, the final down setter check,
  current LP_PENDING state, or current RX pause intent.

Routine startup cannot consume the pause-intent bank. The first raw observation
for each context/reason/event/state/target/status/owner-mask and guard-value key
is immutable. Repeats update only its saturating count and last timestamp.
New keys beyond the relevant bank capacity increment that bank's overflow;
they never overwrite old observations or affect physical admission. Overflow
limits conclusions about omitted decisions. A retained first sample is not a
final-owner snapshot.

## Readout

The same read-only debugfs file now contains these distinct sections:

```text
pm-research=finite2 ...
record=0 ...
observations=finite2 capacity=64 count=... calls=... overflow=... saturated=...
selection=0 context=... bank=0 count=... first_ns=... last_ns=... reason=...
```

Physical `record=` rows retain their original schema. The observation summary
also includes initialization/enabled counts and each bank's capacity, count,
calls and overflow. `bank=0` is routine; `bank=1` is pause intent. Without
saturation, row repeat counts plus overflow equal total calls, including
separately within each bank. `selection=` rows must not be parsed as physical
attempts or retirement records.

Reasons are defined in `driver/linux/crystalhd_pm_research.h`:

| Value | Reason |
| --- | --- |
| 0–6 | Fault, cleanup hold, transition in progress, STOP, START, unavailable device, invalid state |
| 7 | Other selected target, including resume |
| 8 | Final setter check before the existing down-owner predicate |
| 9 | Completion has no current pause intent |
| 10 | Active producer still owns a temporary wake |
| 11 | Already asleep |
| 12 | Pause selected PENDING with busy down-owner terms |
| 13 | Selector found the down-owner terms clear |

The blocker mask records the existing down predicate, not a new gate: bits
0–3 are TX0/TX1/RX0/RX1 lists, 4–6 are TX/IRQ/raw owners, 7–10 are firmware
transaction/active/count/poison, and 11–12 are empty-credit/single-thread credit.
For a resume or another event, these bits do not independently imply refusal.
The row also preserves the first raw owner counts and list states, current and
target power state, enabled flag, cleanup/transition/snapshot/pause flags,
device presence/start and fault observations.

## Verification and limits

The existing actual-source fixtures retain all finite1 coverage. New controls
exercise initialization identity, repeated no-intent completion, producer and
RX blockers, cleanup rejection, final-setter owner movement, immutable first
records, saturation and independent banks. A full startup bank followed by an
actual mocked down/up cycle proves the observer cannot consume the physical
budget or hide the first pause behind routine observations. Target accesses
remain mocked; these controls do not establish a native transition.

Final device-free results: 16 PM fixtures in all four normal/sanitized ×
watchdog configurations, four module-lifecycle cases in normal/sanitized modes,
the unchanged default Flea power matrix, and the research W=1 module build all
passed. All 22 existing diagnostic patches still check and apply. The first
host compile's signed-enum comparison warning and its correction are preserved
with the final logs in
`/home/mache/.cache/crystalhd-pm-finite2-final-20261126.json`.

The [subsequent native comparison](native-finite-20261126.md) uses fresh finite2
loads, the same clear input, owned-pixel comparison, ordinary cleanup and full
official 1.3 restoration. Run2 reuses the original `83f1e9c` client/DIL and
identifies RX0 waiting for completion before the immediate resume. Run3 keeps
that exact module/DIL and changes only the diagnostic client's pause interval;
it records an actual down/wake/current-FLL cycle. Native timing and remaining
temperature/endurance limits belong to that separate evidence.
