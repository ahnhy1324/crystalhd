# Isolated sustained-admission profile

This research branch adds `CRYSTALHD_PM_SUSTAINED_RESEARCH=1` alongside the
existing `CRYSTALHD_PM_RESEARCH=1`. It does not change either the default
production module or the original finite2 build. It includes the separately
reviewed RX list-phase correction from PR #705.

The explicit sustained1 profile permits at most 64 physical down admissions
within at most 60,000 ms from the first actual admission. Both runtime arguments
are mandatory to enable it; the default 0/0 remains OFF. The module identity is
`3.10.29-pmsustain1`, and both debugfs headers identify `sustained1`, so frozen
finite2 guards cannot silently accept this different experiment.

The profile only changes finite limits and reserves 416 static receipt slots.
An admission already requires six spare slots plus terminal headroom; the
compile-time bound retains six slots per permitted cycle plus four terminal
slots. No ring overwriting, dynamic growth, register access, retry, transition
or ownership change is added. The original actual selector/down/up/FLL paths,
once-only module claim, failed-wake retention and checked terminal cleanup
remain. Expiry stops new down admissions; it neither forces wake nor prevents
an outstanding ordinary wake/FLL from completing.

The first planned workload is one original-PTS 900-picture / 37.5-second 720p24
stream with 19 bounded ready-pressure windows distributed at input ordinals
0, 48, ..., 864. There is one device/session/clock epoch and no explicit pause,
seek, reopen or budget reset. Actual physical cycles and their distribution
must come from the native ledger; requested windows are not completed cycles.
Source pictures, full pixels, release, EOS, current-process cleanup and complete
official 1.3 restoration keep their existing requirements. The caller explicitly
retains default YUY2 without invoking the colour setter.

This extends gating observation across playback. It is not five-minute or
hour-scale endurance, calibrated thermal benefit, electrical power measurement,
smooth physical display or production promotion. No native result is claimed
by this source description.
