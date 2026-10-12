# Open work

Release order revised **2026-10-12**: published RC → verified independent
recovery image → same-RC LTS admission → mandatory full qualification → final
artifact checks → maintainer GO → stable 1.0.0. This supersedes the earlier
stable-first/soak-after plan; historical RC results are unchanged.

- [ ] [#369: CrystalHD 1.0.0 final gates and release](https://github.com/ahnhy1324/crystalhd/issues/369)
  — G0–G6 must PASS, experimental modes need explicit dispositions, and final
  artifact/installation/documentation checks precede approval of the exact
  commit and release text. Stable 1.0.0 is not yet approved or published.
- [ ] [#371: Pre-1.0 recovery, LTS admission and full qualification](https://github.com/ahnhy1324/crystalhd/issues/371)
  — independent offline recovery image, same-RC LTS baseline, full-duration
  soak/lifecycle/new-path gates and experimental assessment before stable.
  Missing recovery evidence remains HOLD even if migration already occurred.
  Pre-stable fixes use further RCs and affected requalification; handoff to
  #369 does not require an existing final tag.
- [ ] [#91: Deferred hardware and application acceptance](https://github.com/ahnhy1324/crystalhd/issues/91)
  — alternate hardware, the LTS item executed through #371, power management,
  broader streams, and physical
  presentation/A/V synchronization.
- [ ] [#92: BCM70015 capability research](https://github.com/ahnhy1324/crystalhd/issues/92)
  — pinned firmware mapping and live OPEN/scaling evidence; standalone
  processing and backend reuse remain unvalidated.
- [ ] [#204: BCM70015 picture ownership and engine lifecycle](https://github.com/ahnhy1324/crystalhd/issues/204)
  — #92 follow-up for runtime picture identity, PPB lifetime, and MFD/BVN
  completion.

Acceptance checklists live in those issues. Current usage and support are in
[README.md](README.md); the frozen hardware reference is in
[HARDWARE-2026-09-13.md](HARDWARE-2026-09-13.md). The published baseline remains
bundle `1.0.0-rc.1`, module/DKMS `3.10.2`. Mandatory missing tests remain
NOT RUN/HOLD; only optional work may be deferred. Historical Phase 1 and
bounded RC evidence do not establish new LTS/full-duration passes.
