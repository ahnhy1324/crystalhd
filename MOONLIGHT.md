# Moonlight / Sunshine with CrystalHD

Use unmodified Moonlight Qt 6.2.0 with BCM70015 to receive H.264 video from
Sunshine through CrystalHD hardware decoding. This integration is experimental;
the tested receiver and display route are listed below.

Start with the **default frame-matching mode** at 720p30: each requested frame
keeps its own pixels and timestamp. An optional **experimental previous-frame
mode** reached about 59 renderer submissions/s at 720p60 and 29 at 1080p30
with V-sync and frame pacing off, by allowing an earlier completed frame to
fill the current request. Graceful close/reopen is confirmed;
physical audio/video offset and forced connection-loss recovery remain unverified.

## Current verdict

| Target | What happened on the reference system | Recommendation |
| --- | --- | --- |
| 720p30 H.264 | On Ubuntu 26.04.1 LTS, default matching received/decoded 29.98 fps and submitted 29.97 frames/s over one hour, on a 50 Hz display. | **Recommended first hardware trial** |
| 720p60 H.264 | On LTS, optional `previous,0` received/decoded 59.98 fps and submitted 59.71 frames/s over one hour, on a 60 Hz display. | Try the optional mode after 720p30 works; it changes frame/timestamp matching |
| 1080p30 H.264 | On LTS, optional `previous,0` received/decoded 29.98 fps and submitted 28.91 frames/s over one hour, on a 50 Hz display. | Same optional mode and timing tradeoff; submissions remain below input rate |
| 1080p60 H.264, native card output | Card output stayed below 60 frames/s and live input accumulated backlog. | Not practical on this Moonlight path |
| Orderly session lifecycle | The LTS hour-long sessions retained hardware ownership, passed resource checks and exited normally; a follow-up exact-mode reopen also passed. | Confirmed for graceful close/reopen; forced connection-loss recovery untested |

“Handed to the renderer” is a Moonlight software event, not proof that the
frame reached physical scanout. Local GStreamer playback has a separate
[Full-HD-input/reduced-output option](USAGE.md#optional-bcm70015-output-scaling);
that result does not establish 1080p60 Moonlight streaming.
All three LTS target runs used live+prefeed with V-sync and frame pacing off.
The 60 fps source combined 30 fps video with a moving browser overlay; it was
not a native 60 fps video fixture. See the
[dated LTS results and limitations](HARDWARE-2026-09-13.md#moonlight-lts-sessions--2026-10-13).
These are hardware/lifecycle/resource passes, not measured physical A/V or
input-to-photon acceptance. They are not a matched comparison of default and
previous-frame matching at the same rate. The older ON/ON and three-run
OFF/OFF comparisons below remain separate historical measurements, not
performance limits for this recipe.

## Tested route and compatibility

The tested path is:

```text
Sunshine H.264 -> Moonlight FFmpeg VA-API -> CrystalHD BCM70015
    -> NV12 DRM PRIME -> EGL/GLES indirect presentation -> X11/i915
```

| Component | Tested reference |
| --- | --- |
| Receiver | ThinkPad X61, Core 2 Duo T8100, BCM70015, GMA X3100/i915 |
| OS | Current qualification: Ubuntu 26.04.1 LTS, Linux 7.0.0-38, X11 at 50 or 60 Hz as stated above. Historical comparisons: Ubuntu 25.10, Linux 6.17, 60 Hz |
| Client | Unmodified Moonlight Qt 6.2.0 x86-64 AppImage with bundled FFmpeg |
| Stream | H.264, 8-bit 4:2:0 SDR; 720p/1080p and 30/60 fps test matrix |
| Presentation | Indirect VA-API, composed DRM PRIME layers and EGLImage |

Moonlight's direct X11 route requires `vaPutSurface`, which this VA driver does
not implement. `VAAPI_FORCE_INDIRECT=1` selects the tested alternative.

CrystalHD does not support HEVC or AV1, and the Moonlight integration is
restricted to H.264 8-bit 4:2:0 SDR. BCM70012, Wayland, other GPUs, other
Moonlight versions and Steam Remote Play are unverified. No Moonlight source
modification is included or installed by this repository.

The linked AppImage requires x86-64 userspace; it cannot run on a 32-bit i686
installation. CrystalHD's i686/scalar-library support does not certify a
compatible Moonlight binary or its dependencies. The Core 2 Duo results,
including the software comparison, are not an Atom performance certification.

## Safe first trial

Start with default frame matching at 1280x720, 30 fps. The technical name
used in the evidence is *exact identity*.

1. For the reproduced setup, download
   [`Moonlight-6.2.0-x86_64.AppImage`](https://github.com/moonlight-stream/moonlight-qt/releases/download/v6.2.0/Moonlight-6.2.0-x86_64.AppImage)
   from the official Moonlight Qt 6.2.0 release on an x86-64 installation
   (`uname -m` reports `x86_64`, `getconf LONG_BIT` reports `64`). Other
   architectures need a suitable client build and are outside this tested
   recipe. Newer versions may be tried, but are not covered by the results below.
2. Install and verify CrystalHD using the [README quick start](README.md#quick-start).
3. Close every other CrystalHD client; `sudo fuser -v /dev/crystalhd` should
   report no owner.
4. Configure Sunshine for H.264, 8-bit SDR and stereo. Disable HEVC, AV1, HDR
   and 4:4:4 for the first trial.
5. Launch the installed stack through the indirect path:

```sh
moonlight_app="/path/to/Moonlight.AppImage"
chmod +x "$moonlight_app"
env -u CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP \
  -u CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY \
  -u CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US \
  LIBVA_DRIVER_NAME=crystalhd \
  VAAPI_FORCE_INDIRECT=1 \
  CRYSTALHD_VAAPI_LIVE_H264=1 \
  CRYSTALHD_VAAPI_LOW_LATENCY_H264=1 \
  "$moonlight_app"
```

6. In Moonlight Settings, select hardware decoding, H.264, 1280x720 and
   30 fps. Note your V-sync and frame-pacing choices before any later comparison.
7. Choose **Add PC**, enter the Sunshine host name or IP address, and enter
   Moonlight's displayed PIN in Sunshine's web UI.
8. After pairing completes, select the Sunshine **Desktop** application.

Replace the AppImage path before running the command. It clears previous-frame
options for this invocation, so it is also the way back after that experiment.
For an installed stack, do not add checkout-specific `LIBVA_DRIVERS_PATH` or
`LD_LIBRARY_PATH`; those can mix stale source and installed components.

Do not add the READY-remap variables to this first trial. Do not enable the
PCIe L0s workaround merely because Moonlight is being used. Test the default
link first, and use the [safe L0s procedure](USAGE.md#full-hd-pcie-workaround)
only for a repeatable, qualifying throughput shortfall.

## Confirm that the card is used

Before connecting:

```sh
crystalhd-check
```

While streaming:

```sh
sudo fuser -v /dev/crystalhd
```

The Moonlight process must own `/dev/crystalhd`, its diagnostics must select
VA-API rather than a software decoder, and moving video must continue beyond
the initial decoder probe. Device ownership alone proves that the card was
opened, not that every frame was decoded or displayed correctly.

After a normal exit:

```sh
sudo fuser -v /dev/crystalhd
cat /sys/module/crystalhd/refcnt
```

There should be no owner and the module reference count should be zero. A
second session should be able to acquire the device again.

## What the live options do

| Control | Meaning |
| --- | --- |
| `VAAPI_FORCE_INDIRECT=1` | Moonlight option selecting the DRM PRIME/EGL route instead of unsupported direct `vaPutSurface` presentation |
| `LIBVA_DRIVER_NAME=crystalhd` | Selects the installed CrystalHD VA driver for this process |
| `CRYSTALHD_VAAPI_LIVE_H264=1` | BCM70015 live-stream mode: avoids treating ordinary input gaps as file EOS/replay boundaries; final frames still require later input or context drain |
| `CRYSTALHD_VAAPI_LOW_LATENCY_H264=1` | Prefeeds the next timestamped H.264 prefix; this streaming setup combines it with explicit live mode. Finite input can enable prefeed independently without adopting live-gap/EOS policy. The tested 720p30 stream reduced decode latency by about one frame interval while preserving requested surface identity. |
| Event-driven RX | Negotiated automatically by a matching current driver/library; there is no user switch |

The live and low-latency controls are process-local and experimental. They do
not change `libcrystalhd.so.3`, the kernel ioctl ABI or BCM70012 behavior.
The live+prefeed path has produced about 60 decoded frames/s; decoder output
and Moonlight renderer submissions are different measurements. The near-60
renderer-submission result below also uses the separate READY experiment.

### Experimental previous-frame mode

After the 720p30 first trial works, this mode can improve the handoff rate at
720p60 or 1080p30. It trades normal frame/timestamp matching for the earlier
completed picture, so audio/video timing must be judged on the actual setup.
The measured candidate is `previous,0`; `0` means no additional timed wait.

Close Moonlight before changing modes. Use the following launch command and
these Moonlight GUI settings together:

| Setting | Measured previous-frame configuration |
| --- | --- |
| Video decoder / codec | Hardware / H.264, 8-bit SDR |
| Stream size and rate | 1280x720 at 60 fps, or 1920x1080 at 30 fps |
| V-sync | Off |
| Frame pacing | Off |
| Presentation | Indirect VA-API, selected by the launch command |

The LTS 720p60 run and historical finalists used the reference X61 at
1024x768 and 60 Hz; the LTS 1080p30 run used 50 Hz. All retained the
already-active `force_l0s_off=Y` host workaround. These conditions qualify the
recorded rates; the environment variables alone do not reproduce them. Use the
[session-scoped L0s procedure](USAGE.md#full-hd-pcie-workaround) only after a
qualifying throughput comparison. Disabling V-sync can expose tearing, and
changing GUI settings changes the comparison.

```sh
moonlight_app="/path/to/Moonlight.AppImage"
env LIBVA_DRIVER_NAME=crystalhd \
  VAAPI_FORCE_INDIRECT=1 \
  CRYSTALHD_VAAPI_LIVE_H264=1 \
  CRYSTALHD_VAAPI_LOW_LATENCY_H264=1 \
  CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP=1 \
  CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY=previous \
  CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US=0 \
  "$moonlight_app"
```

Confirm VA-API selection and card ownership as above, then check continuing
motion and Moonlight's statistics at the selected rate. Successful decoding
alone does not prove the recorded cadence on a different setup. Stop if there
is a persistent black picture, a sync error or accumulating delay; use the
[troubleshooting table](#troubleshooting).

To return to default frame matching, exit Moonlight and run the
[first-trial command](#safe-first-trial) again, select 720p30 and restore your
previous V-sync/frame-pacing choices. The command clears all three READY
variables even if they were exported in the shell. Do not put experimental
variables into a login profile.

A bounded READY pool keeps completed pictures available so presentation can
use the preceding picture instead of waiting for the newest requested surface
to finish decoding. If the exact requested
surface finishes first, the exact picture still takes precedence.

The sweep tested additional eligibility waits of 0, 16.67, 33.33 and 50 ms;
the nonzero waits showed no consistent benefit. Under the retained
`previous,0` policy, all 209 observed remap commits used the immediately
preceding payload (`submission_drift=1`). `0` therefore means zero *additional
time-based wait*, not zero payload lag. It reached 58.63 renderer submissions/s
at 720p60 and 28.58 at 1080p30 on the reference system. These are historical
average handoff rates from three repeated 60-second runs. Identity traces found
no duplicate, reversal or stale resurrection; physical scanout timing and
cadence variance were not separately instrumented. One submission of observed
drift is not a worst-case millisecond bound when input or decoding stalls.

The mode remains experimental because it copies that older picture into the
newly requested surface, violating normal VA surface identity and the caller's
PTS-to-surface association. The alternative `latest` policy was less
predictable and sometimes selected pictures 2-17 submissions old. Neither
policy establishes physical A/V synchronization or input-to-photon latency.
Both remain optional experiments, with default frame matching available through
the rollback above.

## Measured limits

The following historical comparisons used Ubuntu 25.10, not the new LTS
campaign summarized above. They combine only the named reference-host series;
they are not general performance promises. These runs retained the
host-specific `force_l0s_off=Y` setting and used an X11 desktop at
1024x768 and 60 Hz. Those conditions qualify the results; test the default PCIe
link first rather than enabling the workaround from these figures.

<a id="default-frame-matching-measurements"></a>

### Historical default matching: V-sync ON / pacing ON

These 720p60 results are from the earlier three-run ON/ON series, not the
OFF/OFF launch configuration above. They are not a matched control for the
previous-frame finalist results and do not establish the default path's
maximum rate. Resource sampling used a 10-second warmup and 60-second window;
Moonlight's FPS counters covered the full approximately 75-second sessions.

| Historical 720p60 path, V-sync ON / pacing ON | Decoder result | Renderer submissions/s, median (range) |
| --- | --- | --- |
| Software | 59.66 decoded fps | 41.33 (41.06-41.43) |
| CrystalHD live-only | 58.86 decoded fps | 19.14 (18.94-19.15) |
| CrystalHD live+prefeed | 59.13 decoded fps | 33.42 (32.05-35.49) |

Software also lost renderer submissions in this ON/ON series. That observation
does not establish a settings-independent client/display ceiling: the later
OFF/OFF software finalist reached 59.81 submissions/s. Prefeed improved the
ON/ON hardware result; this is not a comparison against the OFF/OFF remap mode.
The separate AU-to-output trace and older 720p30/1080p30 baselines remain in
the [hardware report](HARDWARE-2026-09-13.md#repeated-60-hz-presentation-follow-up).

<a id="one-submission-lag-cadence-comparison"></a>

### Previous-frame finalists: V-sync OFF / pacing OFF

Three repeated 60-second finalist runs used V-sync and pacing disabled:

| Stream | `previous,0` renderer submissions/s | Software renderer submissions/s |
| --- | ---: | ---: |
| 720p60 | 58.63 | 59.81 |
| 1080p30 | 28.58 | 29.71 |

These are means, not the medians reported for the older ON/ON series.
The repeated finalists were `previous,0`, `latest,0` and software, not
exact-identity hardware. The earlier screening matrix included remap-off
controls, but its 20-second samples are not the three-run finalist control.
Do not use 33.42 or the earlier 15-16 submissions/s as the OFF/OFF baseline.
This is the measured outcome of intentionally keeping the presented payload one
submission behind, not merely a READY-availability diagnostic. The figures are
not a normal-path support target because the payload is associated with the
newer requested surface and therefore with the caller's newer PTS. Hardware
used less Moonlight process CPU than software in that comparison, but
whole-system CPU was similar and hardware used more resident memory.

The experiment did not improve every target: at 720p30 the exact path was
already near 30 submissions/s and remapping added CPU without useful
throughput, while 1080p60 `previous,0` produced only about 0.27 submissions/s
in its bounded confirmation and remained unusable.

The later event-driven receive implementation reduced paced VA task time and
context switches. A 129-second Moonlight run also closed cleanly without a VA
status error, but its source/network cadence was too low to establish
presentation performance. This is stability evidence; it does not demonstrate
60 fps presentation.

### Input-response proxy, not physical latency

A separate test injected a local key, changed a marker in the remote desktop,
and stopped timing when the returned frame reached the receiver's X Present
notification. Each result is the median of three runs with 100 valid marker
increments:

| Stream and path | p50 | p95 |
| --- | ---: | ---: |
| 720p60 one-submission-lag `previous,0` | 59.05 ms | 73.57 ms |
| 720p60 software | 47.50 ms | 83.11 ms |
| 1080p30 one-submission-lag `previous,0` | 72.60 ms | 100.76 ms |
| 1080p30 software | 51.77 ms | 67.56 ms |

This proxy includes remote repaint, Sunshine capture/encode, network, decode,
renderer and X11. It stops before physical panel scanout, and the hardware
rows measure the cadence-stabilizing READY experiment, which deliberately
violates normal VA surface identity. No equivalent exact-identity hardware
input-response control was recorded, and no path was measured through physical
panel scanout.

## What is not established

- Moonlight's reported decode/queue/render fields are component timings, not
  camera-measured input-to-photon latency.
- Render submissions are not proof of physical scanout.
- The exact path preserves the requested surface and the caller's
  PTS-to-surface association. The newer LTS workload includes A/V cues, and the
  user reported no obvious problem early in the 720p30 run, but no complete
  start/end observation set or speaker-versus-panel capture was recorded for
  all conditions. Physical A/V offset therefore has no numerical bound.
- LTS payload age/progression was not instrumented. Historical one-submission
  identity traces do not certify the later hour-long sessions; the optional
  mode retains a limited/unverified disposition, not full daily-use support.
- Normal exits and later sessions passed. Automatic recovery after deliberately
  interrupting the network or Sunshine was not tested.
- Power consumption was not measured.
- Some hardware matrix runs logged decode-unit queue overflow, and bounded
  runs encountered VA status 23. These cannot be reclassified as network loss.
- The results do not imply Steam Remote Play, browser streaming, BCM70012,
  Wayland or other-GPU compatibility.

## Troubleshooting

| Symptom | First check |
| --- | --- |
| Moonlight uses software | Confirm `LIBVA_DRIVER_NAME=crystalhd`, hardware decoding in Moonlight, and `/dev/crystalhd` ownership |
| VA initializes but no useful display | Confirm `VAAPI_FORCE_INDIRECT=1`; direct `vaPutSurface` is unsupported |
| 720p30 is far below cadence | Confirm H.264 8-bit SDR, close other clients, remove unrelated experimental variables and inspect queue/error logs |
| Full-HD card output is below real time | Reproduce locally first, then evaluate the host-specific L0s procedure; do not change global ASPM |
| `Video decode unit queue overflow` | Treat it as receiver backpressure/presentation evidence, not automatically as packet loss |
| VA status 23 or a persistent black picture | Stop the run, retain the exact mode and logs, and return to the 720p30 exact-identity recipe |
| Device remains busy after exit | Check `sudo fuser -v /dev/crystalhd`, module refcount and current-boot kernel messages before retrying |

## Evidence and history

The new LTS qualification is recorded in
[#371](https://github.com/ahnhy1324/crystalhd/issues/371#issuecomment-6115915038)
and the [dated hardware results](HARDWARE-2026-09-13.md#moonlight-lts-sessions--2026-10-13).
The historical measurement narrative is in the
[VA-API decode and game-stream baseline](HARDWARE-2026-09-13.md#va-api-decode-and-game-stream-baseline-2026-10-10).
The main bounded investigations are:

- [#307 — initial Moonlight feasibility and live transport](https://github.com/ahnhy1324/crystalhd/issues/307)
- [#315 — synchronous surface-wait investigation](https://github.com/ahnhy1324/crystalhd/issues/315)
- [#337 — repeated 60 Hz presentation ceiling](https://github.com/ahnhy1324/crystalhd/issues/337)
- [#351 — nonstandard READY-remap stop-gate](https://github.com/ahnhy1324/crystalhd/issues/351)
- [#362 — event-driven receive experiment](https://github.com/ahnhy1324/crystalhd/issues/362)
  and [#363 — merged implementation](https://github.com/ahnhy1324/crystalhd/pull/363)

These issues are closed with measured conclusions. They do not represent an
open claim that daily-use Moonlight support is complete.
