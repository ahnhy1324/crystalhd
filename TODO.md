# CrystalHD completion checklist

This checklist follows the open issues in
[ahnhy1324/crystalhd](https://github.com/ahnhy1324/crystalhd/issues).
Build results and hardware results are separate. See [BRINGUP.md](BRINGUP.md)
for the bring-up milestones and [DMA.md](DMA.md) for DMA ownership details.
Measured results are recorded in [HARDWARE-2026-09-13.md](HARDWARE-2026-09-13.md).

## Driver and ABI

- [x] [#5: ioctl safety and device permissions](https://github.com/ahnhy1324/crystalhd/issues/5):
  validate native and compat request headers, transfer limits, PCI alignment,
  permissions, legacy identification, and failed copies; preserve playback.
- [x] [#6: 32-bit compatibility](https://github.com/ahnhy1324/crystalhd/issues/6):
  freeze both ABIs, compile library/examples and native i386 module in CI,
  and execute both runtime probes on the x86-64 driver.
- [x] [#9: DMA pins](https://github.com/ahnhy1324/crystalhd/issues/9):
  pair long-term DMA pins with unpin helpers, unmap before release, exercise
  partial failures and merged SG descriptors, and validate sustained playback
  and unload/reload without outstanding pins.

## Playback validation

- [ ] [#16: everyday GStreamer playback](https://github.com/ahnhy1324/crystalhd/issues/16):
  provide explicit hardware/software local playback, validate in-flight
  controls and audio/video timing, and measure sustained 720p operation.
  Keep clocked test sinks distinct from visible/audible presentation and
  diagnose input/output starvation before claiming real-time performance.
  Current blocker: BCM70015 can emit only one new picture after an in-flight
  seek, then fail drain with pending inputs. Recheck full-device versus
  decoder-only reset before sustained hardware playback; startup with audio
  is also timing-dependent. Software controls are not hardware proof.
- [ ] [#12: YouTube integration](https://github.com/ahnhy1324/crystalhd/issues/12):
  replace the legacy quality/seek overrides with codec-only preference,
  reject late player failures in the probe, and validate a normal YouTube
  session and playback controls without bypassing service verification.
  The user confirms normal software playback beyond one minute with 1.6.0;
  live seeking, pause/resume, quality selection, and playback rates remain open.
- [x] Local Chrome software playback controls: exact sampled pixel identities
  through pause/resume, 0.5x/1.5x/2x/restored 1x, four forward/backward seeks,
  and final frame 359. This does not establish live YouTube or hardware controls.
- [x] [#7: reproducible playback paths](https://github.com/ahnhy1324/crystalhd/issues/7):
  verify source and staged/installed discovery, complete drain, exact frame
  counts, and repeated GStreamer and secondary VA-API playback.
- [ ] [#8: hardware matrix](https://github.com/ahnhy1324/crystalhd/issues/8):
  finish the remaining card/codec/lifecycle coverage below.
- [x] Record BCM70015 H.264 Constrained Baseline, Main, and High samples
  separately with exact commands, frame counts, and sample/pixel checksums.
- [x] VA-API complete-file drain: all three H.264 profiles pass 180/180 with
  independent software pixel comparisons. Timestamped access units begin
  with their delimiter; finite batches use bounded actual-IDR replay after
  full device reopen. Missing output is never a success/blank frame.
- [x] VA-API High forward/backward seek and flush: a complete 180-frame
  reference plus four seeks with 12 exact PTS/pixel matches each, using
  `tests/vaapi-seek.cpp` in its default synchronous mode.
- [x] VA-API Baseline/Main/High pipelined seek/flush: the same exact checks
  with `--lookahead 8`, leaving eight client pictures undownloaded before each seek.
- [ ] Broader VA-API stream coverage and synchronous-client performance;
  one-at-a-time clients can repeatedly reopen the device and replay references.
- [x] BCM70015 Full HD H.264 Baseline/Main/High complete-file VA-API decode:
  1920×1080, 30 fps, 180/180 frames per fixture, independent pixel comparisons.
- [x] Full HD GStreamer drain and exact EOS replay for all three profiles;
  Full HD High VA-API four-seek lookahead pixel comparisons.
- [x] Retain original output frames across flush/new input and decoder-context
  destruction; exact old-frame PTS/pixels checked after each transition.
- [x] Asynchronous FFmpeg input looping: High at 640×360 and 1920×1080
  passes five complete loops (900 frames); all 900 smaller-frame pixel hashes
  match the reference. Held-frame regressions also pass all three profiles
  at 640×360 and High at 1920×1080.
- [ ] Full HD real-time display playback; measured decode/download throughput
  and correctness are separate from compositor/display integration.
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
- [x] BCM70015 VC-1 Advanced: 15/15 frames through raw BDU and demuxed
  packet paths, identical hardware pixel hashes.
- [x] BCM70015 WMV3 Main: 25/25 frames through ASF and demuxed packet
  paths, identical hardware pixel hashes.
- [ ] Broader VC-1/WMV3 samples, interlaced output, and mid-stream resolution changes.
- [ ] Suspend/resume with an idle device and around an active/recent session.
  This interrupts the desktop and is not part of unattended `make check`.
- [ ] Chromium hardware frame identity after seeking, and GPU sandbox support.
  Browser hardware decoding remains opt-in; the default is software decoding.
- [x] [#14: VA-API image and synchronization errors](https://github.com/ahnhy1324/crystalhd/issues/14):
  validate image bounds/layout, CPU reads, `vaPutImage`, and fence-signal failures.
  Hardware-free failure/alias regressions, sanitizers, real DRM image readback,
  and the High retained-frame hardware regression pass; this is not general
  GBM/compositor synchronization or Chrome hardware validation.

Do not close the hardware-matrix issue on the strength of compilation or a
BCM70015 H.264-only run. Record exact commands, profiles, checksums, source
revision, loaded module source version, and new kernel log findings.
