# CrystalHD bring-up and troubleshooting

This document records the shortest known-good path from PCI detection to a
decoded frame. It is intended to keep kernel, firmware, library, and frontend
failures separate during hardware bring-up.

Start with the installed, read-only preflight:

```sh
crystalhd-check
```

It checks the common PCI, module identity, DKMS conflict, firmware, device,
library and frontend failures without raw-register access. The detailed
[hardware report](HARDWARE-2026-09-13.md) records tested kernels, fixtures and
limits. CI kernel compilation is not a substitute for a hardware test, and
BCM70012 has not been recently hardware-tested.

## Bring-up order

Establish one invariant at a time:

| Milestone | Positive evidence | Investigate first when it fails |
| --- | --- | --- |
| PCI discovery | `14e4:1612` or `14e4:1615` is present | Slot, power, BIOS, PCI enumeration |
| Driver bind | Module is loaded and `/dev/crystalhd` exists | Kernel log, PCI probe, udev rule |
| Firmware start | `DtsDeviceOpen()` succeeds without a heartbeat failure | Firmware file, permissions, device ownership, existing client |
| Decoder setup | Format, open, color, start and capture calls succeed | Call order, input subtype, supported color format |
| Input acceptance | `DtsProcInput()` succeeds and progress continues | Annex-B framing, metadata, start-code size, buffer alignment |
| Picture ready | Output has valid picture information | YUY2 selection, bitstream validity, RX path |
| Continued output | Multiple frames drain without stalling | Released output buffers, single-session ownership, drain loop |
| Correct display | Frames stay ordered through seek and flush | Timestamps, reorder state, surface lifetime, frontend synchronization |

Do not infer a lower-layer failure from a later missing milestone. For
example, a live firmware heartbeat does not prove that the input stream is
valid, and accepted input does not prove that the requested output format is
supported.

## Device access and module identity

The installed udev rule creates `/dev/crystalhd` as `root:video` with mode
`0660` and asks systemd-logind to grant the active desktop user an ACL. On a
headless system, add the playback account to `video` and log in again:

```sh
sudo usermod -aG video "$USER"
```

`crystalhd-check --source-tree "$PWD"` compares the installed components with
a built checkout and reports obvious device owners. For a deeper manual module
check, run from the repository root:

```sh
modinfo -F filename crystalhd
modinfo -F srcversion crystalhd
modinfo -F srcversion ./driver/linux/crystalhd.ko
cat /sys/module/crystalhd/srcversion
sudo fuser -v /dev/crystalhd
```

The sysfs file exists only while the module is loaded. Matching source versions
are a useful consistency check, not proof of hardware correctness; record the
source commit and build as well. `modinfo crystalhd` describes the installed
file, not necessarily the running module. `modprobe crystalhd` does not replace
an already-loaded module. If versions differ, close all players, confirm the
device is idle and unload normally before explicitly loading the intended
build. Never force-unload a module with active users or DMA; reboot if a normal
unload reports that it is busy. Hardware harnesses reject an already-loaded
module whose source version differs from their build.

With the intended module loaded and idle, `sh tests/ioctl-smoke.sh` checks
native/compat ioctl validation. `sh tests/userspace32.sh --hardware` builds
isolated 32/64-bit libraries and verifies firmware open, capabilities, version
and close through both ABIs. These are explicit hardware tests, not part of
`make check`; 32-bit clients on a 64-bit kernel require `CONFIG_COMPAT`.

### Diagnostic permissions and legacy compatibility

Normal firmware loading and playback use the device permission above. Direct
register, FPGA, device-DRAM and PCI configuration diagnostics additionally
require `CAP_SYS_RAWIO`; run legacy tools as root only when those diagnostics
are needed. The default rule does not expose raw hardware access to every user.

Limited device-identification and playback color-selection operations remain
available to legacy libraries without raw access. Other register access stays
privileged, and BCM70012 initialization under this policy still needs hardware
verification.

For a legacy installation that deliberately permits **all local accounts** to
open the device, an optional `/etc/udev/rules.d/99-crystalhd-local.rules` can
contain:

```udev
KERNEL=="crystalhd", MODE="0666"
```

This broadens device access and is not needed for ordinary desktop playback.
Reload rules with `sudo udevadm control --reload-rules`; the override takes
effect when the device is recreated. Removing that local file restores the
default policy on the next rule reload and device creation. It does not grant
the capability required by raw diagnostic ioctls.

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

The card and firmware support one playback session. A second playback client
must be rejected without disturbing the first; this checks safe rejection,
not concurrent decode support. Close the first client before testing another
complete decode.

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
