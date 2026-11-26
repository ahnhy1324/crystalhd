# Isolated five-minute admission profile

This branch extends the separately frozen sustained1 profile (`7632ab6`) for
one five-minute, 7,200-picture 720p24 experiment. The original default module
and finite2 build remain unchanged. The earlier sustained1 source and its
64-attempt / 60,000-ms / 416-record bounds remain preserved on their own branch;
its initial native preflight was refused before playback because host temperature
exceeded the existing 55-degree Celsius start condition.

Building requires both `CRYSTALHD_PM_RESEARCH=1` and
`CRYSTALHD_PM_SUSTAINED_RESEARCH=1`. The new module is
`3.10.30-pmendurance1`; both debugfs headers identify `endurance1`. Its explicit
runtime limits are at most 256 physical down admissions within 360,000 ms from
the first actual admission. Zero/zero remains OFF. The fixed array contains
1,600 records, exceeding six reserved slots per possible admission plus four
terminal slots (1,540). The actual selector/down/up/FLL, once-only module claim,
fault/cleanup ownership and retirement code are unchanged. Expiry stops new
down admissions without forcing wake or preventing an outstanding ordinary wake.

The planned single-HANDLE workload has 7,200 original packet timestamps over
300 seconds and 150 ready-pressure windows at input ordinals 0, 48, ..., 7152.
It stream-copies the exact earlier 900-picture source eight times; visual content
repeats every 37.5 seconds. New source PTS/DTS are continuous. There is no seek,
reopen, explicit pause, claim reset or forced physical transition. Requested
windows are separate from the actual cycle count. The client retains default
YUY2 without invoking the colour setter.

The new client shares one 330-second deadline from before source loading; the
owned process controller has a distinct 345-second limit for endurance arms,
while ordinary recovery controls retain 75 seconds. Original monitors remain
unchanged. Initial host temperature at most 55 degrees Celsius, runtime below
75 degrees, AC supply, process/memory bounds, once-release/EOS, current-process
cleanup and full official 1.3 restoration retain their existing requirements.

These are source limits and planned stimulus, not a native result. Source-frame
and pixel comparisons must cover all 7,200 pictures in each arm. Five-minute
operation cannot establish hour-scale reliability, calibrated thermal benefit,
electrical power consumption, smooth display or production promotion.
