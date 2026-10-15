# Open work

Release status **2026-10-16**: [stable 1.0.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.0.0)
is unchanged. Published [v1.1.0-rc.3](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0-rc.3)
adds selected interlaced
Weave playback, finite MBAFF tail fixes and explicit HW/SW Weave selection.
The maintainer approved RC publication with deinterlacing and sync tuning
deferred; exact candidate checks and publication state are recorded in #430.
This is not stable v1.1.0 GO or a full-rate FHD/deinterlacing support claim.

- [ ] [#430: v1.1.0 RC qualification and GO/HOLD](https://github.com/ahnhy1324/crystalhd/issues/430)
  — qualify the published immutable RC before a separate stable-release GO.
  The 11-codec oracle passes; the 8-hour run reaches 864,000 pictures/EOS but
  fails its final audio-interval criterion. Later gates remain NOT RUN;
  preserve the failure and investigate without weakening acceptance.
- [ ] [#429: External i386/BCM70015 acceptance](https://github.com/ahnhy1324/crystalhd/issues/429)
  — primary-host results do not replace the reporter's Atom N280/i386/Kodi
  combinations; keep unavailable external configurations marked NOT RUN.
- [ ] [#436: deinterlacing and presentation follow-up](https://github.com/ahnhy1324/crystalhd/issues/436)
  — selected Weave playback and finite tail handling are included in the RC.
  Continue deinterlacing quality, physical sync/cadence tuning and FHD dropped-
  picture/resync investigation without claiming full-rate presentation.
  Hardware Bob remains separate research, not a playback-release prerequisite.

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
