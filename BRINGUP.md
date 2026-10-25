# CrystalHD bring-up and troubleshooting

Start here when installation or playback fails. For a first installation, use
the [Quick start](README.md#quick-start), or [DKMS](README.dkms) if you want
automatic module rebuilds after kernel updates. Run this read-only check first:

```sh
crystalhd-check
```

If it is not installed yet, run `./scripts/crystalhd-check` from the source
directory. Resolve any identity or access failure before starting playback.
Discovery checks do not decode a video; follow [first playback](USAGE.md) once
they pass.

## First checks

| Symptom | First check | Action |
| --- | --- | --- |
| No card found | `lspci -nn -d 14e4:` should show `14e4:1612` or `14e4:1615` | Check that the card is fitted and enabled in the BIOS. Reinstalling the library cannot repair missing PCI discovery. |
| Card found, no `/dev/crystalhd` | `modinfo crystalhd`; `sudo journalctl -k -b --no-pager` | Install the module for `uname -r` using your chosen route. If loading reports a signature/key error, resolve your distribution's module-signing policy. |
| Permission denied | `ls -l /dev/crystalhd`; `id` | Follow [device access](#device-access-and-module-identity); log in again after a group change. |
| Device busy | `sudo fuser -v /dev/crystalhd` | Close the listed player normally. The card supports one decoding session at a time. |
| Loaded/installed modules differ | `crystalhd-check` | Follow [idle reload](#reload-an-idle-device), or reboot after installing the intended module. |
| Multiple module candidates / DKMS conflict | `crystalhd-check`; `dkms status -m crystalhd` if DKMS is installed | Choose one module owner. Follow the [DKMS migration instructions](README.dkms#switch-from-a-direct-install) instead of installing another copy over it. |
| Unsupported input | Check codec/profile, not just the file extension | Choose an input from [supported formats](USAGE.md); hardware mode reports failure instead of silently selecting software. |
| Slow, black or stalled output | Check the player's error and recent kernel messages | Use [playback troubleshooting](USAGE.md#troubleshooting-and-proof-of-hardware-use). The scaling and L0s choices there have separate prerequisites and costs. |

## Direct install, update and rollback

Direct installation builds for the selected kernel. Use [README.dkms](README.dkms)
instead if DKMS already manages CrystalHD. Install the [Quick start
dependencies](README.md#quick-start) and work from the root of the chosen source
tree. A downloaded source archive works without `.git`.

For an update, keep the previous source directory/archive and note its release
or commit before replacing anything. Also keep any local
`/etc/modprobe.d/` CrystalHD option files; these settings survive reinstallation.
Check ownership and options before changing installed files:

```sh
./scripts/crystalhd-check
modprobe -c | grep -E '^options[[:space:]]+crystalhd([[:space:]]|$)'
```

No matching option line is normal. If the check reports a DKMS registration,
stop this route and follow the DKMS guide. For a fresh install, missing module
or runtime files are expected. For an update, close all players and meet the
[idle preconditions](#reload-an-idle-device) before installation.

Build and install from this directory. Paste the complete parenthesized block;
it stops on failure without closing your terminal:

```sh
(
    set -eu
    make -j2 driver library gstreamer vaapi
    sudo make install
)
```

Use `make -j1 driver library gstreamer vaapi` if parallel builds exhaust memory.
These targets build the playback stack; the separate development tests need
additional dependencies. The installer adds the module, firmware, permissions,
library and frontends, but does not replace a module already in memory. Load a
first install with `sudo modprobe crystalhd`; use the idle reload procedure for
an update. Then run `crystalhd-check` and the [hardware playback command](USAGE.md).

After booting a different kernel, install its matching headers and repeat the
build/install steps. To roll back, close players and meet the idle preconditions.
From the outgoing version's source root, replace the quoted placeholder with
your retained older source directory. Verify its release/commit before running
the complete block:

```sh
(
    set -eu
    previous_source="/path/to/retained-previous-source"
    test -f "$previous_source/Makefile" || {
        printf '%s\n' 'STOP: select the retained previous source directory.' >&2
        exit 1
    }
    kernel_release=$(uname -r)
    sudo make install-preflight KVER="$kernel_release"
    make -C "$previous_source" -j2 driver library gstreamer vaapi KVER="$kernel_release"
    sudo make uninstall-runtime
    sudo make -C "$previous_source" install KVER="$kernel_release"
)
```

A failed prerequisite or build stops before removing the current runtime.
Removing the outgoing runtime first also removes release metadata or helpers
absent from an older version. Restore any option
files changed with the update before reloading or rebooting; remove options
the older module does not support. Run `crystalhd-check` afterward. A Phase 1
rollback also reverts newer optional features; use its own playback instructions.

<a id="interrupted-runtime-installation"></a>

Runtime installation and removal record their current stage in
`/var/lib/crystalhd/runtime-install.pending`. If interrupted, run
`sh scripts/crystalhd-check` from the intended source to inspect it; an older
installed checker may not understand the marker, or removal may have deleted it.
The updated checker reports failure even when an older VERSION record remains. Resolve the reported
error, then rerun `sudo make install-runtime` from the intended source or release;
to finish removal, rerun `sudo make uninstall-runtime` from the outgoing source.
A successful complete sequence clears the marker. An overlapping install/removal
is refused while another process or its surviving child still writes files.
This supports diagnosis and full-sequence retry; multi-file atomic replacement,
automatic restoration of previous files and power-loss recovery are separate
work. Existing user configuration and direct/DKMS module ownership are unchanged.

To uninstall this direct installation, close players and satisfy the idle
preconditions first. From the source directory used to install it:

```sh
(
    set -eu
    if test -d /sys/module/crystalhd; then
        test "$(cat /sys/module/crystalhd/refcnt)" = 0
        sudo modprobe -r crystalhd
    fi
    sudo make uninstall KVER="$(uname -r)"
)
```

If the module is already absent, skip its unload. Stop if unloading fails;
use a normal reboot and retry before playback.
Uninstall removes the configured direct module and shared runtime files, not
DKMS registrations, browser profiles or your own `/etc/modprobe.d/` files.
If you installed the optional browser launcher, its separate removal command
is `sudo make uninstall-browser`. For modules installed for other kernels,
repeat `sudo make uninstall-module KVER="the-installed-kernel-release"`, replacing the
quoted value with an actual installed kernel release. Do not remove runtime
files while another CrystalHD installation still needs them.

## Device access and module identity

The installed udev rule creates `/dev/crystalhd` as `root:video` with mode
`0660` and asks systemd-logind to grant the active desktop user an ACL. On a
headless system, add the playback account to `video` and log in again:

```sh
sudo usermod -aG video "$USER"
```

`modinfo crystalhd` describes the installed file; it can differ from the module
currently in memory. `modprobe crystalhd` leaves an already-loaded module in
place. Use `crystalhd-check` to compare them, or add `--source-tree "$PWD"`
from a built source directory to compare the intended build as well.

## Reload an idle device

Close all players and diagnostic programs. Inspect first:

```sh
modinfo -F filename crystalhd
modinfo -F srcversion crystalhd
sudo fuser -v /dev/crystalhd
cat /sys/module/crystalhd/refcnt
modprobe -c | grep -E '^options[[:space:]]+crystalhd([[:space:]]|$)'
```

Continue only when `fuser` lists no owner and the loaded module's reference
count is `0`. If the module is absent, the sysfs file will also be absent and
there is nothing to unload. Keep the intended saved module options: a plain
`modprobe` reads configuration files again, while options supplied only on an
earlier command line are not remembered.

With the intended module installed and those checks satisfied:

```sh
(
    set -eu
    if test -d /sys/module/crystalhd; then
        test "$(cat /sys/module/crystalhd/refcnt)" = 0
        sudo modprobe -r crystalhd
    fi
    sudo modprobe crystalhd
    crystalhd-check
)
```

Stop if either module command fails. Never use forced module unloading. If
normal unload reports a busy device after clients have closed, reboot rather
than forcing it; do not kill a stuck owner to force recovery. For an option
experiment or rollback, restore the saved option
configuration first and use the explicit values documented in [USAGE](USAGE.md).

## Report a remaining problem

Include the release/tag or source commit, `crystalhd-check` output, card model,
`uname -r`, player command, codec/profile, resolution/frame rate, and the symptom.
Add relevant new `sudo journalctl -k -b --no-pager` messages and whether the
previous version or default options work. Remove private paths and unrelated
log entries; private media and pairing details are not needed.

The [hardware report](HARDWARE-2026-09-13.md) records tested hosts and limits.
BCM70012 has not been recently hardware-tested. The material below is for
programmers and hardware diagnosis; ordinary playback recovery does not require
these tools.

## Advanced: decoder bring-up

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

A firmware heartbeat does not prove valid input, and accepted input does not
prove supported output. Hardware harnesses reject a loaded module whose source
version differs from their build; compilation alone does not test hardware.

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

### Known-good BCM70015 userspace sequence

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

### BCM70015 output-format trap

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

### Additional detail for decoder test reports

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
