# Seek begun during Flea runtime sleep

The 2026-11-28 paired 720p30 diagnostic for [#689](https://github.com/ahnhy1324/crystalhd/issues/689)
reproduced an input stall after restart. The OFF arm completed both same-handle
seeks and returned all 900 original pictures with exact source PTS, native YUY2
and reference luma. In the ON arm, the first mode2 call started during a
completed low-power interval and succeeded, but the next decoder generation
stalled after one host transfer. Full official 1.3 restoration and both
180-picture/EOS controls passed.

The new profile skips acquiring application outputs before each seek. The first
ON prefix reached 14 ready buffers with 18 accepted inputs. After successful
mode2, all 18 were explicitly cancelled. The first call timestamp is between
the recorded completed down transition and the subsequent wake. A second
physical down/wake also occurs inside that same call. This consumes both finite
attempts and supplies only one observed asleep seek. These are host-published
power-state brackets around the userspace call.

The next ON prefix accepted 24 inputs but issued one actual host transfer,
with a successful descriptor, DMA completion and firmware notification.
The ready count stayed zero. No new picture-queue notification or later TX
start was observed. The experiment stopped at its packet cap before the
second seek or final 900-picture generation. All 608 copied observer records
were retained, and normal cleanup left no owned process or buffer.

The old capture's `hw_pause_issued` was still set after the channel reopened.
During the first new TX, CMD_COMP selected low-power pending. The maintained
setter writes zero FLL before checking the physical-down budget, so exhaustion
of that budget did not prevent the logical transition.
Ordinary TX then remained gated. RX-ready backpressure had outlived the queue
that owned it. The earlier RX alternating-list correction addresses a different
failure.

[PR #718](https://github.com/ahnhy1324/crystalhd/pull/718) clears that request
after successful destructive capture retirement while the existing Flea
cleanup hold is active. The ordinary command/start path still controls wake
and FLL publication. The new actual-source checks fail on the old driver and
pass on the corrected driver, with Link, default-disabled PWM, discard-only
and failed-stop paths covered.

## Corrected same-workload run

With the corrected module and unchanged client, both OFF and ON finish the two
mode2 seeks. Each cancels 18 and 19 accepted prefix inputs after the corresponding
successful seek, then delivers and releases the final 900 original pictures
with exact PTS, full native YUY2 and reference luma, and firmware EOS. Ready
and pending counts return to zero. The corrected ON restart has 18 completed
host TX chains and 14 RX transfers in the next prefix, where the original ON
had one TX and no RX picture-path progress.

The first ON seek again starts during published low power. Both physical
down/wake cycles occur within that call, so the second asleep seek remains
unexercised. Both official 180-picture/EOS controls and all six strict kernel
checks pass, with complete official 1.3 restoration. The OFF source/pixel/host
join passes. The first ON analysis saves successful source and pixel checks
before its separate TX/PES reader reports a mismatch. That reader expected
all 937 accepted inputs to appear in TX, including prefixes cancelled by seek.
The separate host analysis matches 935 actually transmitted original inputs,
including all 900 final originals, against exact bytes and PTS. Source inputs
17 and 498 had been queued and were cancelled after successful mode2. The
terminal 235-byte EOS payload and kernel TX/RX joins pass. This follow-up
reuses the completed checkpoints without reopening any pixel file.

The original paired failure is preserved. The first aggregate reader refused
the successful OFF arm before reading any pixels. A separately reviewed reader
admitted only the independently completed OFF arm after exact official recovery
and compared each of its 900 pictures once. Raw runs and the isolated diagnostic
artifacts remain with the kernel research work. No display, power-saving or
endurance result follows from this comparison.
