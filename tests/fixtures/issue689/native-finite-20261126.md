# Finite Flea PM on current source

**Two real down/wake cycles now complete in one maintained-source session, with all 180
MPEG-2 pictures byte-identical to the native reference and full official 1.3
restoration.** This is bounded research for [#689](https://github.com/ahnhy1324/crystalhd/issues/689),
not thermal/endurance qualification or production enablement.

## Results and cause

| Attempt | Module / client | Native comparison | Physical down / wake |
| --- | --- | --- | --- |
| Run1 | finite1 / immediate pause-resume | Reference, OFF, ON, OFF: each 180/180, full YUY2 equal | 0 / 0 |
| Run2 | finite2 / same immediate pause-resume | OFF, ON, OFF: each 180/180, full YUY2 equal | 0 / 0 |
| Run3 | same finite2 / one explicit 50 ms paused interval | OFF, ON, OFF: each 180/180, full YUY2 equal | ON: 1 / 1; both OFF: 0 / 0 |
| Run4 | same finite2 / two explicit 50 ms paused intervals | OFF, ON, OFF: each 180/180, full YUY2 equal | ON: 2 / 2; both OFF: 0 / 0 |

Run2 identifies the zero-entry condition without changing the power policy.
The actual decoder context is armed. At pause, its RX0 picture-delivery request
and the firmware transaction still own work. Releasing the transaction removes
that blocker, but RX0 still waits for its Y completion interrupt. The next resume
selection arrives 34,852 ns later, before that RX owner retires. There is no
attempt, claim or started admission window. All eight selector observations
fit without overflow; the original zero-entry results remain unchanged.

Run3 changes only the explicit diagnostic client's pause interval. With the
same module and DIL, RX0 clears and the existing completion path finds all
down-owner conditions clear about 1.05 ms after the RX-only observation.
The existing physical helpers enter low power, later wake, and publish the
current FLL successfully. This demonstrates that the posted RX can retire
while the current pause path holds FLL at zero in this workload. No missing
completion callback or product delay change is needed to explain these runs.

The 50 ms client interval is an experimental stimulus, not a driver fix.
It follows the pause reply and zero-FLL transition. The historical #376 driver's
50 ms wait followed command posting before that transition, so the two delays
are not equivalent. These runs neither establish a minimum working interval
nor prove that 50 ms is required for other workloads.

Run4 repeats the stimulus after released pictures 1 and 90 in the same session.
Both posted RX owners retire before their respective physical down; the second
pause initially waits for RX1. Both cycles complete the existing down, wake,
saved-state restoration and current-FLL publication. The ledger records two
attempts, two entries, two completed wakes and two successful FLL restores,
with no refusal, failure, dropped record or retained snapshot. Its final state
is IDLE/STOPPED and retired. The selector's 16 calls fit in 13 distinct rows
without overflow. There is no third requested cycle or native limit-refusal
claim.

| Run4 cycle | Down helper bracket | Published low-power interval | Up helper bracket | Restored FLL |
| --- | --- | --- | --- | --- |
| After picture 1 | 96.244 µs | 49.183712 ms | 162.943 µs | 15 |
| After picture 90 | 114.262 µs | 48.660381 ms | 166.435 µs | 15 |

The low-power interval is the host ledger's `DOWN_END`/`LP_COMPLETE` through
`UP_BEGIN`, not an independently measured clock edge. Run3's corresponding
single interval was 49.002199 ms. These are bounded observations, not a latency
distribution or a measured power saving.

## What each native comparison checks

The retained synthetic MPEG-2 input has 180 pictures at 640×360 and 1,777,511
submitted bytes. Every arm returns the 180 unique submitted source tokens,
releases each successful output once, reaches firmware EOS, and completes
STOP/CLOSE with no outstanding process resources. Each entire 82,944,000-byte
tight YUY2 capture equals the fresh Run1 reference and the retained #376 native
reference. This is native byte comparison, not a new software-color or physical
display qualification. The tight DIL output layout is not an independent
physical RX pitch observation.

Each arm uses a fresh research module load. OFF is 0 attempts / 0 ms; ON is
limited to 2 attempts / a 5,000 ms new-down admission window. The window starts
at the first actual down admission. It is not a timer that forces wake.
The original source-token, owner, firmware-command, IRQ, RX/TX, raw-access and
cleanup guards remain in force. No external application, firmware or installed
component is modified. The diagnostic client performs no additional raw
register reads; its normal command path remains identified in the records.

Full per-arm candidate snapshots match before/after. Each experiment restores
the matching official 1.3 runtime/module 3.10.20, with the complete baseline
equal before, restored and after. Ordinary independent 180-picture/EOS controls
pass before and after each matrix. Kernel/PCIe/AER checks remain clean, module
references and pins return to zero, and no forced shutdown/unload or live-owner
handoff occurs.

## Source and evidence map

| Component | Frozen source / artifact |
| --- | --- |
| Maintained product base | `5e9dd72` |
| finite1 mechanism and original client | `83f1e9c`; module `3.10.25-pmfinite1`, probe build02 |
| finite2 selector observer | `9cf8adc`; module `3.10.25-pmfinite2` reused unchanged in Run2–Run4 |
| Single-dwell client | `6104afb`; probe dwell build01 |
| Repeated-dwell client | `53e6a5e`; probe repeat build01 |
| Native DIL throughout | Original `83f1e9c` build; its maintained DIL source is unchanged in later experiment commits |

The [frozen private evidence archive](https://github.com/ahnhy1324/crystalhd-firmware-research/blob/4a5ecb476cd0dfab0bcd32fda4aebb9170f6c297/results/native-pm-finite-2026-11-26/README.md)
preserves all four experiments, original failures, exact sources/builds and
independent readers; see [private PR #168](https://github.com/ahnhy1324/crystalhd-firmware-research/pull/168).
Original coordinator receipts keep `CAPTURED_OFFICIAL_RESTORED_PENDING_ANALYSIS`.
Separate later readers classify playback, pixels, physical transitions and
restoration; they do not rewrite the first receipt. Zero-entry attempts and
development failures remain alongside successful runs.

Read the [mechanism and limits](finite-mechanism-20261126.md),
[selector observation contract](finite2-selector-20261126.md), and
[current-source client instructions](../../../tools/research/flea-pm-finite/README.md)
for the respective implementation scopes. Raw experiments are retained under
`/home/mache/.cache/crystalhd-pm-finite-run{1,2,3,4}-20261126/`; the private evidence
archive supplies their original files and independent reconstruction.

The actual-source mechanism fixtures pass normal/sanitized and watchdog-on/off
configurations, including mocked count/deadline/refusal/fault recovery. W=1
default/research module builds and existing diagnostic patch application pass.
The final repeated client passes 714 normal and 714 ASan/UBSan/leak checks plus
the exact input preflight. Independent reviews cover source, actual native
ledgers, input/output/lease reconstruction and complete restoration. Device-free
controls remain distinct from these actual hardware results.

The initial integration documentation check failed with `undocumented module:
pm_research_attempts, pm_research_ms`: its broad source scan counted the isolated
research object as a production configuration surface. The inventory now asks
the actual Kbuild for its default object list, without building a module, and
scans those sources. Temporary source controls confirm that a newly linked
production parameter is included, and linking the research object into the
default would expose its parameters to the check again. There is no parameter
name exclusion. The corrected check retains 44 production surfaces; all 16
existing schema checks pass. Research controls remain documented separately in
[CONFIGURATION.md](../../../CONFIGURATION.md).

## Remaining scope

This establishes two finite current-source transitions, not sustained automatic
gating. More cycles and pacing/codecs, a bounded 720p24 H.264
case, identified card-temperature measurements, long-idle/endurance and any
future experimental playback promotion still require their own evidence.
Host thermal-zone maxima are not identified BCM70015 sensor or wattage readings.
These 180-picture initial controls do not satisfy or replace the separate
900-source-original experimental playback criterion. Runtime PWM remains
disabled by default, and #689 remains open.
