# Same-handle forward/backward seek with finite clock gating

The OFF and explicit default-YUY2 ON diagnostics complete two midstream seeks
on one device HANDLE, then decode all 900 original pictures through natural
EOS. ON records two automatic down/wake/FLL cycles. Normal PWM remains disabled.

| Acquisition | Colour setup | Seek / final pictures | Result |
| --- | --- | --- | --- |
| run1 OFF | Actual `DtsSetColorSpace` | 2 / 900 | PASS, including pixels |
| run2 ON | Actual `DtsSetColorSpace` | 0 / 0 | Preserved startup FAIL: raw write returns BUSY |
| run3 ON | Explicitly retain default YUY2; setter is not called | 2 / 900 | PASS, including all 926 captured pictures |

The unchanged 1280×720p24 High/B2 input starts at AU0, moves to the original
IDR at AU480 (20 seconds), then returns to AU0. Both interrupted prefixes
accept 18 source access units, return 13 pictures and cancel the remaining five only after actual
successful mode2 flush. The final generation independently returns all 900;
the 26 prefix pictures do not contribute to that requirement. Original source
PTS/DTS and disjoint generation tags expose stale or misassociated output.

The first ON failure is a setup difference, not a failed seek: the DIL colour
setter performs a raw-register write, which experimental PWM intentionally
refuses. Earlier PM clients retained the DIL's default YUY2 without that call.
The corrected diagnostic explicitly selects this existing research profile
and records zero setter calls in each generation. The real-setter path and
its failure remain preserved. No fabricated API return, raw-write exception,
external application change or firmware modification is used.

Both ON wakes finish before their corresponding flush calls. This establishes
seek after automatic gating, not a flush begun while asleep. The device stays
open; successful mode2 closes the decoder, which is reconfigured and restarted
on that same HANDLE. There is no full-device-reopen fallback. The old GStreamer
in-flight stall was not reproduced in the qualified profiles, but its cause
was not fixed here and the maintained workaround remains unchanged.

Every acquisition, including the failed setup, restores the complete official
1.3 runtime and passes ordinary 180-picture playback before and after. Final
resources return to 14 FDs / one thread in this probe, with no native output
lease or kernel/PCIe error. Both successful runs additionally finish with natural
firmware EOS, no pending input and an empty ready queue.

[Complete sources, original failure, captures and analysis](https://github.com/ahnhy1324/crystalhd-firmware-research/blob/6e359ac9b5c1e7879e2b99f4c4ed23cde5dc26c9/results/native-pm-seek-2026-11-26/README.md)
retain the separate client revisions and native receipts. New default-profile
controls pass 76 checks normally and under ASan/UBSan; the original profile
passes 69 in each mode. Source, lifecycle and data reviews are independent.

This private, bounded diagnostic does not qualify ordinary setter support
under PWM, stock-player display, sustained gating, temperature benefit,
electrical power or endurance. Its generation clocks are explicitly rebased
at each selected source position; raw timestamp tags are reversible identifiers,
not real-time 100 ns timestamps. Serial owned capture follows native release
and makes no smooth-display claim.
