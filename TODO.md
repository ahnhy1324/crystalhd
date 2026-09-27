# Active core work

This is a compact index of unfinished driver, library and supported frontend
work. Detailed acceptance criteria belong in the linked issues; test evidence
belongs in the [hardware report](HARDWARE-2026-09-13.md).
See [README.md](README.md) for usage, [BRINGUP.md](BRINGUP.md) for diagnosis,
and [DMA.md](DMA.md) for DMA ownership and failure boundaries.

## Tracked issues

- [ ] [#8: Full HD and hardware matrix][issue-8]:
  finish physical display/audio, device and lifecycle coverage.
- [ ] [#12: YouTube][issue-12]:
  diagnose live 1x A/V desynchronization and validate playback controls.
- [ ] [#52: V4L2 stateful M2M roadmap][issue-52]:
  freeze the legacy behavioral baseline, then add the V4L2 frontend without
  breaking the existing ABI or legacy hardware support.

## Coverage and lifecycle

- [ ] Test BCM70012 on a current LTS and recent stable kernel.
- [ ] Broaden H.264, MPEG-2, MPEG-4, VC-1 and WMV3 samples, interlaced layouts
  and mid-stream resolution/profile/format transitions beyond the recorded
  cases. MPEG-4 still excludes DIVX311, sprites/GMC, quarter-pixel, data
  partitioning and interlace. Include sustained playback and controls, not
  only short complete-file drain.
- [ ] Expand VA-API stream coverage and improve synchronous-client performance;
  one-picture-at-a-time clients can repeatedly reopen and replay references.
- [ ] Validate actual VLC/EGL/GBM compositor presentation and synchronization;
  image-handle and exported-pixel tests alone do not establish display behavior.
- [ ] Establish Chromium hardware frame identity after seeking and GPU sandbox
  compatibility, including asynchronous VPP fence/timeline access. Keep hardware
  decode experimental until those end-to-end checks pass.
- [ ] Test idle and active/recent-session suspend/resume, including the optional
  L0s workaround and failure recovery. Schedule separately: this interrupts the
  desktop and is not part of unattended `make check`.
- [ ] Extend hardware failure coverage for PCI stop/probe failures and physical
  removal during DMA, preserving the ownership and cleanup rules in [DMA.md](DMA.md).
- [ ] Cover remaining legacy library conversion helpers and broader device-API
  behavior; targeted ABI, raw YUY2 and cancellation tests are not conformance.

Do not close the hardware matrix on compilation or a BCM70015 H.264-only run.
Keep the report's result matrix and reproducible evidence concise; put full
logs and investigation history in the relevant issue or CI artifact.

[issue-8]: https://github.com/ahnhy1324/crystalhd/issues/8
[issue-12]: https://github.com/ahnhy1324/crystalhd/issues/12
[issue-52]: https://github.com/ahnhy1324/crystalhd/issues/52
