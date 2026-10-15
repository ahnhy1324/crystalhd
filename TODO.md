# Open work

Release status **2026-10-15**: [stable 1.0.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.0.0)
is published and unchanged. The hardware-tested v1.1.0-rc.2 runtime payload is
`f4d99d5` in [PR #435](https://github.com/ahnhy1324/crystalhd/pull/435); its code,
CI, native hardware and Kodi video-lifecycle checks passed. The current
approval head adds documentation only and leaves runtime files unchanged.
Physical Kodi flash/click A/V observation and explicit maintainer RC GO remain;
the documentation successor is not relabelled as a newly hardware-tested
payload.

- [ ] [#430: v1.1.0 RC qualification and GO/HOLD](https://github.com/ahnhy1324/crystalhd/issues/430)
  — retain the exact tested runtime identity and recorded limitations while
  awaiting physical Kodi A/V observation and the explicit RC decision.
- [ ] [#429: External i386/BCM70015 acceptance](https://github.com/ahnhy1324/crystalhd/issues/429)
  — primary-host results do not replace the reporter's Atom N280/i386/Kodi
  combinations; keep unavailable external configurations marked NOT RUN.
- [ ] [#436: proposed v1.2.0 Kodi interlace integration](https://github.com/ahnhy1324/crystalhd/issues/436)
  — selected H.264 field pairs already produce a complete deterministic Weave
  frame, and standalone FFmpeg BWDIF produces 50 progressive frames/s from 25
  complete interlaced pictures/s. Resolve the Kodi/CrystalHD VA decode,
  readback and post-processing scheduling integration without decode-error
  recovery, preserve field order/cadence and EOS, and recheck progressive
  playback and clean close/reopen. The exact owning component remains
  unresolved; hardware deinterlacing is separate research, not a prerequisite.

- [x] [#369: CrystalHD 1.0.0 final gates and release](https://github.com/ahnhy1324/crystalhd/issues/369)
  — G0–G6 and experimental dispositions are complete. The exact commit and
  release text were approved; stable 1.0.0 and its public archive are verified.
- [x] [#371: Pre-1.0 recovery, LTS admission and full qualification](https://github.com/ahnhy1324/crystalhd/issues/371)
  — G0–G4 complete: same-RC LTS admission, core endurance/lifecycle, scaled
  output and VA event-worker endurance. Moonlight's three one-hour targets
  and corrected reopen pass hardware/lifecycle/resources; physical A/V and
  payload-age behavior retain an explicit limited/unverified disposition.
  Final package `f41ca40` passes direct/DKMS installation, RC rollback and
  installed-path checks; G6 documentation/decision handoff is complete.
  Preserve the existing backup under the G0 exception.
  The release and public-archive verification are complete in #369.
- [ ] [#91: Deferred hardware and application acceptance](https://github.com/ahnhy1324/crystalhd/issues/91)
  — alternate hardware, the LTS item executed through #371, bounded BCM70015
  deep-S3 coverage completed through #395/#397, remaining fault/removal work,
  broader streams, and physical presentation/A/V synchronization.
- [ ] [#92: BCM70015 capability research](https://github.com/ahnhy1324/crystalhd/issues/92)
  — pinned firmware mapping and live OPEN/scaling evidence; standalone
  processing and backend reuse remain unvalidated.
- [ ] [#204: BCM70015 picture ownership and engine lifecycle](https://github.com/ahnhy1324/crystalhd/issues/204)
  — #92 follow-up for runtime picture identity, PPB lifetime, and MFD/BVN
  completion.

Acceptance checklists live in those issues. Current usage and support are in
[README.md](README.md); the frozen hardware reference is in
[HARDWARE-2026-09-13.md](HARDWARE-2026-09-13.md). The stable release is bundle
`1.0.0`, module/DKMS `3.10.3`; RC.1 (`1.0.0-rc.1` / `3.10.2`) and the
Phase 1 baseline remain unchanged. Mandatory missing tests remain
NOT RUN/HOLD; only optional work may be deferred. Historical Phase 1 and
bounded RC evidence do not establish new LTS/full-duration passes.
