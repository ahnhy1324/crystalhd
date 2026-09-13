# CrystalHD completion checklist

This checklist follows the open issues in
[ahnhy1324/crystalhd](https://github.com/ahnhy1324/crystalhd/issues).
Build results and hardware results are separate. See [BRINGUP.md](BRINGUP.md)
for the bring-up milestones and [DMA.md](DMA.md) for DMA ownership details.
Measured results are recorded in [HARDWARE-2026-09-13.md](HARDWARE-2026-09-13.md).

## Driver and ABI

- [ ] [#5: ioctl safety and device permissions](https://github.com/ahnhy1324/crystalhd/issues/5):
  validate native and compat request headers, transfer limits, PCI alignment,
  permissions, legacy identification, and failed copies; preserve playback.
- [ ] [#6: 32-bit compatibility](https://github.com/ahnhy1324/crystalhd/issues/6):
  freeze both ABIs, compile library/examples and native i386 module in CI,
  and execute both runtime probes on the x86-64 driver.
- [ ] [#9: DMA pins](https://github.com/ahnhy1324/crystalhd/issues/9):
  pair long-term DMA pins with unpin helpers, unmap before release, exercise
  partial failures and merged SG descriptors, and validate sustained playback
  and unload/reload without outstanding pins.

## Playback validation

- [ ] [#7: reproducible playback paths](https://github.com/ahnhy1324/crystalhd/issues/7):
  verify source and staged/installed discovery, complete drain, exact frame
  counts, and repeated GStreamer and secondary VA-API playback.
- [ ] [#8: hardware matrix](https://github.com/ahnhy1324/crystalhd/issues/8):
  finish the remaining card/codec/lifecycle coverage below.
- [x] Record BCM70015 H.264 Constrained Baseline, Main, and High samples
  separately with exact commands, frame counts, and sample/pixel checksums.
- [ ] VA-API complete-file drain: strict validation still misses the final
  frames. Access-unit delimiters and firmware decode-order requests did not
  resolve it; do not replace missing pixels with a success/blank frame.
- [ ] VA-API forward/backward seek and flush: compare downloaded pixels with
  an uninterrupted reference decode using `tests/vaapi-seek.cpp`.
- [x] GStreamer flushing seek to zero after EOS: all three H.264 profiles,
  complete frame counts and identical replay pixels. Arbitrary in-flight
  seeks still need separate coverage.
- [x] Module unload/reload followed by complete decode and seek replay on
  all three H.264 profiles, with balanced pins and zero final module references.
- [x] Reject a second playback client safely while the first keeps decoding;
  both the first and subsequent standalone run still drain all 180 frames.

## Requires additional hardware or a separate test session

- [ ] BCM70012 on a current LTS and recent stable kernel.
- [x] BCM70015 progressive MPEG-2 Main: 180/180 YUY2 frames and complete drain.
- [ ] VC-1, WMV3, interlaced output, and mid-stream resolution changes.
- [ ] Suspend/resume with an idle device and around an active/recent session.
  This interrupts the desktop and is not part of unattended `make check`.
- [ ] Chromium hardware frame identity after seeking, and GPU sandbox support.
  Browser hardware decoding remains opt-in; the default is software decoding.

Do not close the hardware-matrix issue on the strength of compilation or a
BCM70015 H.264-only run. Record exact commands, profiles, checksums, source
revision, loaded module source version, and new kernel log findings.
