# Sustained 720p24 clock gating: 900 pictures and five minutes

Both isolated OFF/ON comparisons pass source pictures, timestamps, pixels,
ownership, EOS and complete official 1.3 restoration. The longer run keeps one
HANDLE and one source clock for 7,200 pictures over 300 seconds per arm. ON
completes 150 actual automatic down/wake/FLL cycles across that stream.
Normal PWM remains disabled. These are diagnostic-client results for
[#689](https://github.com/ahnhy1324/crystalhd/issues/689).

| Workload per arm | OFF / ON originals | Actual ON cycles | Published sleep | Source duration |
| --- | ---: | ---: | ---: | ---: |
| Sustained 900 | 900 / 900 | 19 | 14.549114 s | 37.5 s |
| Five-minute 7200 | 7,200 / 7,200 | 150 | 123.918211 s | 300 s |

Every OFF arm records zero physical cycles. All ON transitions restore FLL12;
no physical failure, refusal, lost record or remaining power snapshot occurs.
The 900 ON entries span 0.678–36.973 seconds, counting through final FLL.
The five-minute span is 0.678–298.975 seconds, with 38/37/38/37 entries in the
four source-time quarters. Transitions are not confined to startup.

## Source and picture evidence

The original 1280×720 H.264 source uses 24/1 cadence, time base 1/24000 and
1000-tick display cadence and decode-timestamp spacing, preserving original
B-frame PTS order. The longer MP4 is a stock stream-copy of eight exact
copies of the 900-picture content; visuals repeat every 37.5 seconds while
all 7,200 source timestamps remain continuous. The analysis checks the actual
AnnexB bytes and every packet's PTS/DTS, duration, size and flags before using
the existing 900-picture pixel reference modulo 900.

Separate first analyses read all 1,800 and 14,400 newly captured pictures once.
Every visible software-reference luma plane matches. Full native YUY2 matches
ON→new OFF→the retained original native 900 reference; OFF self-equality is not
independent evidence. All source admissions, ordered outputs, once-releases,
capture parts and original EOS are accounted for. No uncancelled source picture
is missing, and no renderer-loss allowance substitutes for source correctness.

The five-minute stream has 50 parts per arm: 49×145+95 pictures and exactly
13,271,040,000 YUY2 bytes. Peak owned slots are OFF 14 / ON 16 within the existing
16-slot total. Both workers join; the explicit final census returns to
FD 55 / Threads 1, including the intentionally open capture files. The shorter
run uses seven parts and returns to FD 12 / Threads 1. Each current process has
its own normal cleanup and whole candidate-boundary proof before unloading.
Both acquisitions restore the complete official source `24a5182b988b` / module `3.10.20`
installation and pass ordinary 180-picture/EOS controls before and after.

## Stimulus, timing and temperature scope

The clients retain original DTS-paced input and PTS targets with the explicit
333.333334-ms owned-presentation lead. Each requested pressure window begins
48 AUs apart, admits at most 24 AUs within 1500 ms, and permits a 50-ms host dwell
only after observing ready≥14. No explicit pause, seek, reopen, forced wake,
epoch reset or module-claim reset occurs. These deliberate native-fetch gaps
are not smooth physical-display qualification.

Requested windows and physical cycles are independently counted. Five-minute
ON has two READY/dwell windows and 148 PACKET_LIMIT windows; all 150 contain one
observed physical entry. The shorter ON has two READY and 17 PACKET_LIMIT
windows. Neither count was forced as a playback acceptance requirement.

Published DOWN_END→UP_BEGIN intervals total 38.7976% of the 37.5-second source
window and 41.3061% of the 300-second window. Using actual first-input→final-owned-
capture durations instead gives 38.4085% and 41.2540%, respectively. These are
clock-interval fractions, not measured energy savings.

The five-minute acquisition starts at host 50°C; sampled host temperatures are
OFF 50–53°C and ON 51–57°C. Sampled RSS peaks are 508,876 / 513,084 KiB, threads 4 and
FDs 57. Initial ≤55°C, runtime <75°C, AC and resource limits remain unchanged.
These sequential host observations do not establish a matched card-temperature
or electrical-power benefit. No fan or cooling configuration was altered.

## Separate research profile and retained evidence

The earlier sustained1 source `7632ab6` / module `3.10.29-pmsustain1` remains frozen. The new
endurance1 source `189d9fc` / module `3.10.30-pmendurance1` requires both build flags
`CRYSTALHD_PM_RESEARCH=1` and `CRYSTALHD_PM_SUSTAINED_RESEARCH=1`; neither is a
production toggle. Both explicit runtime parameters must be supplied, with
at most 256 new-down attempts within 360000 ms of the first admission. Zero/zero
remains OFF. The fixed 1600-record array retains complete transition and
terminal history. Expiry prevents a new down admission but never forces wake
or prevents an outstanding normal wake/FLL.

Normal builds and the existing finite2 limits remain unchanged. Actual power
transitions, owner/admission fences, fault retention and cleanup are unchanged.
The maintained test target exercises both finite2 and endurance1 under normal
and sanitized watchdog variants. Affected checks, W=1 module builds and
independent source/lifecycle reviews pass. The five-minute client uses one 330 s
deadline from before input loading and an owned 345 s outer bound; ordinary
controls retain 75 s. The real colour setter is explicitly omitted at the caller,
with no fabricated successful API return.

The immutable [900-picture evidence](https://github.com/ahnhy1324/crystalhd-firmware-research/blob/c24252cb6ef8c2215f2d287a59d53e62f76bf5b9/results/native-pm-sustained-2026-11-27/README.md)
and [five-minute evidence](https://github.com/ahnhy1324/crystalhd-firmware-research/blob/c24252cb6ef8c2215f2d287a59d53e62f76bf5b9/results/native-pm-endurance-2026-11-27/README.md)
record both primary executions, original receipts, source/build identities,
physical ledgers and independent reviews.
Full captures remain locally intact; their archive reconstruction references
immutable native 900 payloads only after the complete full-byte equality chain
passes. New source clocks, events, offsets and results retain their own identity.
The earlier temperature refusal and preparation-only snapshot remain unchanged.

Next are additional resolutions/codecs, matched card-temperature sampling and
longer operation. This bounded five-minute result does not qualify stock
applications, display, hour-scale reliability or default-enabled operation.
