<a id="crystalhd-completion-checklist"></a>

# Open work

Open tasks are grouped by playback blockers and validation coverage. Detailed
test evidence is in the [hardware report](HARDWARE-2026-09-13.md) and linked issues.
See [README.md](README.md) for usage, [BRINGUP.md](BRINGUP.md) for diagnosis,
and [DMA.md](DMA.md) for DMA ownership and failure boundaries.

<a id="playback-validation"></a>

## Playback blockers

- [ ] [#8: Full HD and hardware matrix](https://github.com/ahnhy1324/crystalhd/issues/8):
  verify real-time display and physical speaker/display sync; exercise Full HD
  seeking, pause/resume and rate changes, including 2x. Decode/download and
  clocked-sink results do not establish these presentation guarantees.
- [ ] [#12: YouTube](https://github.com/ahnhy1324/crystalhd/issues/12):
  diagnose reported A/V desynchronization at 1x; lead/lag and cause remain
  unknown. Validate live seeks, pause/resume, quality selection and rates
  without bypassing service verification. Local-file checks are not live
  YouTube validation; keep Chrome's software-decoding default.
<a id="driver-and-abi"></a>
<a id="requires-additional-hardware-or-a-separate-test-session"></a>

## Coverage and lifecycle

- [ ] Test BCM70012 on a current LTS and recent stable kernel.
- [ ] Broaden H.264, MPEG-2, VC-1 and WMV3 samples, interlaced layouts and
  mid-stream resolution/profile/format transitions beyond the recorded cases.
  Include sustained playback and controls, not only short complete-file drain.
- [ ] [#47: MPEG-4 Part 2 APIs](https://github.com/ahnhy1324/crystalhd/issues/47):
  broaden native BCM70015 Simple/Advanced Simple coverage, including DIVX311,
  sprites/GMC, interlace and more resolutions, then add the missing
  GStreamer/VA-API paths. Repeated complete drains do not establish
  pixel-exact conformance or frontend support.
- [ ] Expand VA-API stream coverage and improve synchronous-client performance;
  one-picture-at-a-time clients can repeatedly reopen and replay references.
- [ ] Validate actual VLC/EGL/GBM compositor presentation and synchronization;
  image-handle and exported-pixel tests alone do not establish display behavior.
- [ ] Establish Chromium hardware frame identity after seeking and GPU sandbox
  compatibility, including asynchronous VPP fence/timeline access. Keep hardware
  decode experimental until those end-to-end checks pass.
- [ ] Test idle and active/recent-session suspend/resume, including the optional
  L0s workaround and failure recovery. Schedule separately: this interrupts the
  desktop and is not part of unattended `make check`. The command-layer idle
  resume defect and its hardware-free regression are tracked in
  [#46](https://github.com/ahnhy1324/crystalhd/issues/46).
- [ ] Extend hardware failure coverage for PCI stop/probe failures and physical
  removal during DMA, preserving the ownership and cleanup rules in [DMA.md](DMA.md).
- [ ] Cover remaining legacy library conversion helpers and broader device-API
  behavior; targeted ABI, raw YUY2 and cancellation tests are not conformance.

Do not close the hardware matrix on compilation or a BCM70015 H.264-only run.
Record exact commands, fixtures/profiles, checksums, source revision, loaded
module source version and new kernel findings in the existing hardware report.
