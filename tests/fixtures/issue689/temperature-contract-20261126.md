# Card-temperature observation prerequisite

The BCM70015 temperature-data field is identified in the retained chip register
definitions. This source-level prerequisite for [#689](https://github.com/ahnhy1324/crystalhd/issues/689)
does not establish measured card temperature, sensor calibration, sample
freshness or a thermal benefit from power management.

## Register and API boundary

The original `bchp_clk.h` defines `TEMP_MON_STATUS` at `0x00070044`:

| Bits | Meaning |
| --- | --- |
| 8:0 | Temperature DATA, mask `0x000001ff` |
| 15:9 | Reserved |
| 23:16 | STROBE_COUNT |
| 30:24 | Reserved |
| 31 | CLK25K |

See the [retained register definition](../../../include/flea/70015/magnum/basemodules/chp/70015/rdb/a0/bchp_clk.h)
and the maintained [DtsGetCoreTemperature implementation](../../../linux_lib/libcrystalhd/libcrystalhd_int_if.cpp).
The previous `0x0000ffff` mask admitted reserved bits 15:9 into the conversion.
Correct extraction must use only DATA; the existing formula
`267.2 - 0.7 * DATA` remains unchanged. This is a demonstrated source-contract
error under controlled inputs, not evidence that the current card returned
nonzero reserved bits or that an earlier native temperature was wrong.

The output pointer must be valid before it is written. Read failure must return
the original status with zero output and no retry. Existing non-Flea behavior
is success with zero output and no register read; that compatibility result
must not be interpreted as a measured 0°C temperature.

## Regression evidence

[The device-free regression](../../library-temperature.cpp) links the actual
library implementation, context and pooled register-read path. Only the final
register ioctl is intercepted; no device is opened. It checks DATA endpoints,
each non-DATA bit, failed driver/syscall reads, pool exhaustion, invalid and
foreign-process handles, non-Flea compatibility and NULL output rejection.
Independent literal temperature examples distinguish the field mask from a
copy of the implementation.

The same test against the retained original implementation reports **153
checks, 9 failures**: seven reserved bits 9 through 15, their combined effect
and the NULL output. The fixed implementation passes **153 checks normally
and 153 under ASan/UBSan with leak detection**. Existing selected sanitizer coverage passes
all six cases/eight invocations; its seven runner controls also pass. The
ordinary DIL build succeeds. The test is wired into the existing
`make library-check` and `make library-check-sanitized` targets.
Independent source and test review finds no blocker. These are library API
and extraction checks, not hardware temperature measurements.

## Native measurement still required

The existing device start enables the monitor. A future observer should reuse
that initialization and the admitted read path, preserve raw values, statuses
and timestamps, and distinguish data from the separate status fields. A raw
read under runtime PWM is admitted only while ACTIVE. Refused or missing reads
must remain visible; they do not justify forcing a wake or treating the cached
`TemperatureRegVal` as a fresh asleep sample.

The [completed 900-picture comparison](native900-20261126.md) used its original
frozen module and DIL. It did not exercise this temperature-function correction
or collect identified card-temperature samples. Host thermal-zone observations
remain host measurements. Ambient/cooling conditions, actual sensor sampling,
matched thermal A/B and independently instrumented electrical power retain
their separate pending scope.
