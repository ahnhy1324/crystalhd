# Finite Flea PM research mechanism

This is the isolated mechanism for [#689](https://github.com/ahnhy1324/crystalhd/issues/689),
based on maintained master `5e9dd72`. Production runtime PWM remains disabled.
No module was installed or loaded and no hardware trial was performed here.

## Build and controls

The default build excludes the mechanism and its parameters. Build the separate
research module with:

```sh
make -C driver/linux -f Makefile.in W=1 CRYSTALHD_PM_RESEARCH=1
make flea-pm-research-check
```

Its module identity has the suffix `-pmfinite1`. Research builds require debugfs
and reject simultaneous `CRYSTALHD_FW_RESEARCH` or `CRYSTALHD_V4L2`. The same
research binary defaults to the OFF control: `pm_research_attempts=0` and
`pm_research_ms=0`. Both immutable, read-only parameters must be explicitly
nonzero to arm it. The hard limits are 1–8 physical down attempts and a 1–5000 ms
window. These are mechanism ceilings, not a selected native workload.

Arming at hardware initialization enables the existing host ownership fences;
it does not claim the module's session or start its clock. The temporary
`crystalhd_setup_cmd_context` open/close therefore consumes no experiment budget.
The first actual down admission, after successful zero FLL and the setter's
final locked owner check, claims the sole session and starts its monotonic
window. Startup and firmware loading do not consume that window. A later
context/reopen cannot reset the module-wide claim.

This is a **new-down admission deadline**, not a bound on sleep residency or
process lifetime. The implementation adds no timer, forced wake, worker or
unattended playback loop. An already entered down can complete its ordinary
wake and current-FLL publication after either admission limit. A native runner
must separately bound its workload and retain ownership if cleanup is unknown.

## Preserved behavior and new boundaries

The original physical down/up routines and register helpers are unchanged from
the current base. The mechanism reuses the claim/result/saved-state ideas from
the frozen #376 `e38a2f8` one-cycle prototype; it does not restore its old module,
replace current source files or reimplement the integrated cleanup guards.
The current locked free-RX count, transition reservation, IRQ/command/raw owners,
nested cleanup holds and terminal PCI retirement remain authoritative.

Ordinary busy selections consume no attempt or record. A physical attempt is
claimed only immediately before the existing down helper. Its first refusal
stops further physical attempts while retaining the ordinary accepted pause;
a refusal is not a hardware fault or a completed sleep. A helper fault or
failed zero/restored FLL stops the experiment and retains fault state. A failure
before first physical admission binds a failure-only session with start/deadline
zero and attempts zero. It cannot be hidden by reopening the device.

The shared fault publisher also records DMA/FW faults. It does not acquire
`hw->lock`, retry hardware or clear a fault. Ledger locking never acquires the
hardware lock, so the normal hardware-lock → ledger-lock order is preserved.
Fault-path owner fields are observations, not an atomic multi-owner snapshot.
Repeated fault publication records the first fault only.

## Readout and interpretation

The research module exposes `/sys/kernel/debug/crystalhd-pm-research/trace` as
read-only numeric records. The module owns a fixed 64-record array. Each new
attempt requires room for its down/up/FLL receipts plus stop/fault/retirement;
records are never overwritten. No hardware pointer is retained in a record.
The reader copies the ledger under its lock and formats the copy after release.
An open reader pins the module and must be closed by the eventual native runner.

Events are defined in `driver/linux/crystalhd_pm_research.h`: session, down begin,
down end, up begin, up end, FLL, admission stop, fault and retirement. Counters
separate down attempts/entered/refused/failed, up attempted/completed/failed and
FLL completed/failed. The `state`, `phase` and `snapshot` fields remain distinct.
Saved register values are meaningful as a live restoration baseline only when
`snapshot=1`; the copied values are retained after invalidation for diagnosis.

`DOWN_END` with `LP_COMPLETE` and `UP_BEGIN` delimit the host-published low-power
interval. They do not measure an independent physical clock edge. A successful
up result means the existing PLL/CKE/register checks completed; ACTIVE is recorded
only after successful current-FLL publication. No new target reads are introduced.
Sampled owner values do not establish whole-run IRQ or owner balance.

`retired=1` records the existing caller's decision immediately before freeing its
context, including an explicitly terminal PCI path. It is not an unload permit,
proof of normal wake, or proof that module/file references are zero. A retained
snapshot, sleeping/uncertain phase or fault stays visible; none authorizes forced
unload or a recovery retry. Native recovery must use the complete matched official
1.3/source `24a5182b988b`/module `3.10.20` baseline and existing ordinary control plus
180-frame/EOS recovery. The old #376 module 3.10.3 restore is historical only.

## Device-free checks

`tests/flea-pm-research.c` reuses the existing Flea MMIO/fault environment and
source-extracts the actual ledger, selector, setter, physical down/up and FLL
paths. It covers OFF/invalid limits, throwaway startup versus first stream,
startup delay, no reopen budget reset, eight cycles, exact deadline refusal,
post-deadline wake, bounded record capacity, busy-owner nonconsumption, cleanup
exclusion, MEMC refusal, zero/restored-FLL failure, PLL wake failure and shared
fault publication. The physical accesses are mocked, not executed on a card.

`tests/flea-pm-lifecycle.c` executes the actual module entry/exit bodies with
registration stubs: initialization failure, failed PCI registration and cleanup
after unregister. Normal and ASan/UBSan modes run; physical transition fixtures
also run with watchdog handling compiled both ways. Existing default Flea power,
command PM and device lifetime tests remain connected to their current sources.

The 2026-11-26 device-free result is PASS: all 13 mechanism fixtures in the four
normal/sanitized × watchdog configurations, plus all four module-lifecycle cases
in normal and sanitized modes. The affected default Flea power, 84 command-PM
groups, 72 lifetime scenarios and 221 device-access checks also passed. W=1
builds passed for default `3.10.25` and research `3.10.25-pmfinite1`; the default
artifact exposes no PM-research parameters. The original physical helper/register
block is byte-identical to the maintained base. All 22 existing diagnostic
patches check and apply in order; their full staging suite was not rerun here.
Local raw logs and preserved development failures are indexed by
`/home/mache/.cache/crystalhd-pm-finite-final-20261126.json`.

Temperature collection, repeated native playback, recovery orchestration,
thermal benefit, long-idle/endurance and production enablement are outside this
mechanism. The #376 180-frame one-cycle receipt is not a result of this source.
