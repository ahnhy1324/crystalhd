# CrystalHD bring-up and troubleshooting notes

This document records the shortest known-good path from PCI detection to a
decoded frame. It is intended to keep kernel, firmware, library, and frontend
failures separate during hardware bring-up.

The current hardware-tested baseline is a BCM70015 (`14e4:1615`) decoding
progressive H.264 on Ubuntu kernel `6.17.0-41-generic`. The module is compiled
in CI against Linux 6.1, 6.6, 6.12, and 6.18 LTS releases plus stable and
mainline, but compilation is not a substitute for a hardware test. BCM70012
support is retained and is not recently hardware-tested.

## Bring-up order

Establish one invariant at a time:

1. The PCI function is visible with the expected device ID.
2. The module binds and creates `/dev/crystalhd`.
3. `DtsDeviceOpen()` loads the correct firmware and observes its heartbeat.
4. The decoder accepts an explicitly described elementary stream.
5. The application selects an output format advertised by the device.
6. `DtsProcInput()` accepts data and the firmware consumes it.
7. A picture reaches the host RX path with valid picture information.
8. The client releases the output buffer before requesting more work.
9. The frontend preserves frame identity across drain, flush, and seek.

Do not infer a lower-layer failure from a later missing milestone. For
example, a live firmware heartbeat does not prove that the input stream is
valid, and accepted input does not prove that the requested output format is
supported.

## Known-good BCM70015 userspace sequence

The maintained GStreamer and VA-API frontends use this initialization order:

```c
DtsDeviceOpen(&device, playback_mode);
DtsSetInputFormat(device, &input_format);
DtsOpenDecoder(device, BC_STREAM_TYPE_ES);
DtsSetColorSpace(device, OUTPUT_MODE422_YUY2);
DtsStartDecoder(device);
DtsStartCapture(device);
```

For progressive H.264 elementary streams, describe the input accurately and
preserve Annex-B start codes. Feed the described stream with `DtsProcInput()`,
receive frames with `DtsProcOutputNoCopy()`, and call
`DtsReleaseOutputBuffs()` after every successful output before reusing or
destroying the associated state.

The card and firmware support one playback session. A second process is not a
valid concurrency test; close the first client before opening another.

## BCM70015 output-format trap

BCM70015 is called FLEA in the source. `DtsGetCapabilities()` advertises only
`OUTPUT_MODE422_YUY2` for this device, while the legacy library context starts
with `b422Mode` set to `OUTPUT_MODE420`. Applications must therefore select
YUY2 explicitly before starting decode and capture.

This mismatch is easy to miss because device open, firmware commands, and
input submission can all succeed first. A test that omits
`DtsSetColorSpace()` or assumes YV12/4:2:0 may then report output DMA timeouts,
partial Y activity, or an idle UV path. Check the device capability and color
selection before investigating firmware command layout or RX descriptors.

The current frontends and diagnostics make the selection explicitly:

- [GStreamer](filters/gst/gst-plugin-1.0/gstcrystalhd.c)
- [VA-API](filters/vaapi/crystalhd_drv_video.cpp)
- Legacy diagnostics: [hellobcm](examples/hellobcm.cpp) and
  [mpeg2test](examples/mpeg2test.cpp)

BCM70012 has different advertised output capabilities. Do not copy the FLEA
assumption to that device without querying its capabilities.

## Milestones and failure boundaries

| Milestone | Positive evidence | Investigate first when it fails |
| --- | --- | --- |
| PCI discovery | `14e4:1612` or `14e4:1615` is present | Slot, power, BIOS, PCI enumeration |
| Driver bind | Module is loaded and `/dev/crystalhd` exists | Kernel log, PCI probe, udev rule |
| Firmware start | `DtsDeviceOpen()` succeeds without a heartbeat failure | Firmware file, permissions, device ownership, existing client |
| Decoder setup | Format, open, color, start, and capture calls succeed | Call order, input subtype, supported color format |
| Input acceptance | `DtsProcInput()` returns success and progress continues | Annex-B framing, metadata, start-code size, buffer alignment |
| Picture ready | Output succeeds with valid picture information | YUY2 selection first, then bitstream validity and RX path |
| Continued output | Multiple frames drain without stalling | `DtsReleaseOutputBuffs()`, single-session ownership, drain loop |
| Correct display | Frames remain ordered through seek and flush | Timestamps, reorder state, surface lifetime, frontend synchronization |

## Diagnostic shortcuts

- Module compilation proves kernel API compatibility only.
- A successful firmware heartbeat proves that the device processor is alive,
  not that a decode channel is correctly configured.
- A successful `DtsProcInput()` proves API acceptance, not necessarily correct
  elementary-stream framing.
- On BCM70015, repeated output timeouts should trigger a YUY2 and call-sequence
  check before firmware reverse engineering.
- One frame followed by a stall commonly points to an unreleased output buffer
  or an incomplete drain loop.
- Green, black, duplicated, or stale frames after a seek are normally a
  userspace surface-identity or synchronization problem once the hardware
  continues to produce valid pictures.

## Minimum useful test report

Record the following when reporting a new card or failure:

```text
uname -r
lspci -nn -d 14e4:
module source commit
BCM70012 or BCM70015
firmware filename and version, when known
libcrystalhd source commit
frontend and exact command line
input codec, profile, resolution, frame rate, and framing
last successful milestone from the table above
new kernel log lines from the test
```

Also state whether the result is a compile test, module-load test, firmware
open test, single-frame decode, or sustained playback. These are different
levels of validation.
