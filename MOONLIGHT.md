# Moonlight / Sunshine with CrystalHD

Moonlight is a first-class experimental integration in this project. Stock
Moonlight Qt 6.2.0 has opened BCM70015 through the CrystalHD VA-API driver,
decoded a live H.264 stream in hardware and displayed it through an indirect
EGL/DRM PRIME path. This is real hardware integration, not a `vainfo` result or
software fallback.

It is not yet a daily-use or 60 fps support claim. Use 720p30 for the first
hardware trial; 720p60 and 1080p30 remain performance experiments. Orderly
close/reopen is confirmed. Physical A/V offset has no controlled measurement;
the input-response proxy below uses a nonstandard path and stops before panel
scanout. Automatic recovery from a forced network or Sunshine interruption has
not been tested. Use software decoding when dependable game streaming matters
more than experimental offload.

## Current verdict

| Target | What happened on the reference system | Recommendation |
| --- | --- | --- |
| 720p30 H.264 | Hardware decode and indirect display worked; bounded runs handed roughly 28-29 frames/s to Moonlight's renderer. | **Only recommended starting point for a hardware trial** |
| 720p60 H.264 | The card decoded near 60 frames/s, but Moonlight handed only about 33 frames/s to its renderer. | Experimental; not smooth 60 fps |
| 1080p30 H.264 | The card decoded about 30 frames/s, but Moonlight handed only about 15-16 frames/s to its renderer. | Experimental; not smooth 30 fps |
| 1080p60 H.264 | Card output stayed below 60 frames/s and live input accumulated backlog. | Not practical |
| Orderly session lifecycle | A five-minute hardware session exited normally; an immediate 40-second session reacquired the device, and later bounded sessions also released it cleanly. | Confirmed for graceful close/reopen; forced connection-loss recovery untested |
| Nonstandard READY remap | Selected runs approached the requested handoff rate by substituting an older picture under a newer surface/timestamp. | Diagnostic only; invalid for normal VA playback |

“Handed to the renderer” is a Moonlight software event, not proof that the
frame reached physical scanout.

## Tested route and compatibility

The tested path is:

```text
Sunshine H.264 -> Moonlight FFmpeg VA-API -> CrystalHD BCM70015
    -> NV12 DRM PRIME -> EGL/GLES indirect presentation -> X11/i915
```

| Component | Tested reference |
| --- | --- |
| Receiver | ThinkPad X61, Core 2 Duo T8100, BCM70015, GMA X3100/i915 |
| OS | Ubuntu 25.10, Linux 6.17, X11 at 60 Hz |
| Client | Unmodified Moonlight Qt 6.2.0 AppImage with bundled FFmpeg |
| Stream | H.264, 8-bit 4:2:0 SDR; 720p/1080p and 30/60 fps test matrix |
| Presentation | Indirect VA-API, composed DRM PRIME layers and EGLImage |

Moonlight's direct X11 route requires `vaPutSurface`, which this VA driver does
not implement. `VAAPI_FORCE_INDIRECT=1` selects the tested alternative.

CrystalHD does not support HEVC or AV1, and the Moonlight integration is
restricted to H.264 8-bit 4:2:0 SDR. BCM70012, Wayland, other GPUs, other
Moonlight versions and Steam Remote Play are unverified. No Moonlight source
modification is included or installed by this repository.

## Safe first trial

Start at 1280x720, 30 fps. Do not begin with a 60 fps or Full-HD target.

1. For the reproduced setup, download
   [`Moonlight-6.2.0-x86_64.AppImage`](https://github.com/moonlight-stream/moonlight-qt/releases/download/v6.2.0/Moonlight-6.2.0-x86_64.AppImage)
   from the official Moonlight Qt 6.2.0 release. Newer versions may be tried,
   but are not covered by the results below.
2. Install and verify CrystalHD using the [README quick start](README.md#quick-start).
3. Close every other CrystalHD client; `sudo fuser -v /dev/crystalhd` should
   report no owner.
4. Configure Sunshine for H.264, 8-bit SDR and stereo. Disable HEVC, AV1, HDR
   and 4:4:4 for the first trial.
5. Launch the installed stack through the indirect path:

```sh
moonlight_app=/path/to/Moonlight.AppImage
chmod +x "$moonlight_app"
env LIBVA_DRIVER_NAME=crystalhd \
  VAAPI_FORCE_INDIRECT=1 \
  CRYSTALHD_VAAPI_LIVE_H264=1 \
  CRYSTALHD_VAAPI_LOW_LATENCY_H264=1 \
  "$moonlight_app"
```

6. In Moonlight Settings, select hardware decoding, H.264, 1280x720 and
   30 fps.
7. Choose **Add PC**, enter the Sunshine host name or IP address, and enter
   Moonlight's displayed PIN in Sunshine's web UI.
8. After pairing completes, select the Sunshine **Desktop** application.

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
| `CRYSTALHD_VAAPI_LOW_LATENCY_H264=1` | Requires live mode; prefeeds the next timestamped H.264 prefix and reduced decode latency by about one frame interval in a tested 720p30 stream while preserving requested surface identity |
| Event-driven RX | Negotiated automatically by a matching current driver/library; there is no user switch |

The live and low-latency controls are process-local and experimental. They do
not change `libcrystalhd.so.3`, the kernel ioctl ABI or BCM70012 behavior.
Reconstructed H.264 frame-rate signaling remains fixed at 30 fps, so these
options are not a supported 60 fps mode.

### READY remap is diagnostic only

Do not set these for ordinary streaming:

```sh
CRYSTALHD_VAAPI_EXPERIMENTAL_READY_REMAP
CRYSTALHD_VAAPI_EXPERIMENTAL_READY_POLICY
CRYSTALHD_VAAPI_EXPERIMENTAL_READY_DELAY_US
```

The retained `previous,0` diagnostic can copy an immediately older completed
picture into the newly requested surface/timestamp. It improved selected
submission rates, but deliberately violates standard VA surface identity.
`latest` sometimes selected pictures 2-17 submissions old. Neither is a fix
for exact pixels, PTS, A/V synchronization or physical latency, so neither is
recommended to users or enabled by default.

## Measured limits

The following are decision-oriented summaries. They combine only the named
reference-host series; they are not general performance promises. These runs
retained the host-specific `force_l0s_off=Y` setting and used an X11 desktop at
1024x768 and 60 Hz. Those conditions qualify the results; test the default PCIe
link first rather than enabling the workaround from these figures.

### Exact-identity paths

| Reference condition | Decoder result | Moonlight presentation result |
| --- | --- | --- |
| 720p30 live+prefeed | Hardware output held the requested cadence | Bounded runs reached roughly 28-29 render submissions/s |
| 720p60 live-only | 58.86 decoded fps | 19.14 render submissions/s |
| 720p60 live+prefeed | 59.13 decoded fps; matched AU-to-output p50/p95 21.25/25.13 ms | 33.42 render submissions/s (three-run median) |
| 1080p30 exact identity | About 30 hardware outputs/s | About 15-16 render submissions/s in repeated baselines |
| 1080p60 | Below requested hardware cadence with backlog | Not usable |

The paired 720p60 software control decoded 59.66 fps and submitted 41.33 fps,
so this host also has a client/display presentation ceiling independent of
CrystalHD. Prefeed materially improved the hardware path, but did not remove
that presentation bottleneck.

### Nonstandard diagnostic comparison

With V-sync and pacing disabled, three repeated finalist runs measured 58.63
render submissions/s at 720p60 and 28.58 at 1080p30 with `previous,0`. These
figures are not a support target because the presented payload identity is one
submission older. Hardware used less Moonlight process CPU than software in
that comparison, but whole-system CPU was similar and hardware used more
resident memory.

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
| 720p60 nonstandard `previous,0` | 59.05 ms | 73.57 ms |
| 720p60 software | 47.50 ms | 83.11 ms |
| 1080p30 nonstandard `previous,0` | 72.60 ms | 100.76 ms |
| 1080p30 software | 51.77 ms | 67.56 ms |

This proxy includes remote repaint, Sunshine capture/encode, network, decode,
renderer and X11. It stops before physical panel scanout, and the hardware
rows use READY remapping that deliberately violates normal VA surface
identity. No equivalent exact-identity 720p30 input-response proxy was
recorded, and no path was measured through physical panel scanout.

## What is not established

- Moonlight's reported decode/queue/render fields are component timings, not
  camera-measured input-to-photon latency.
- Render submissions are not proof of physical scanout.
- The exact path preserves requested surface/PTS identity and the live stream
  carried audio, but the workload had no known simultaneous audio/video cue
  and no speaker-versus-panel capture. Physical A/V offset therefore has no
  numerical bound.
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

The maintained measurement narrative is in the
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
