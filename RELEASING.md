# Releasing the maintained CrystalHD stack

The launch tracker is [#369](https://github.com/ahnhy1324/crystalhd/issues/369).
This procedure prepares the maintained legacy stack for users; optional
firmware research has its own acceptance criteria in #92 and #204.

**Policy clarification — 2026-10-12:** stable 1.0.0 requires
**published RC → verified independent recovery image → same-RC LTS admission
→ full fixed-candidate qualification → final artifact checks → maintainer GO
→ stable release**. This supersedes the 2026-10-11 stable-first/soak-after
sequence. [#371](https://github.com/ahnhy1324/crystalhd/issues/371) owns execution
and evidence; #369 owns the final decision and publication.
The published `v1.0.0-rc.1` at
`79c7cc963f69a6cbe22852c7fa30cee71a0c1737` and its bounded results remain
unchanged. They do not establish the new LTS/full-duration passes, and stable
`v1.0.0` is not yet approved or published.

For this campaign, the maintainer subsequently accepted **G0 by exception**
and authorized narrowly reviewed G1/G2 compatibility dispositions; see the
dated decision in #369/#371. This does not claim unperformed recovery tests
passed or waive the remaining endurance and final-package gates.

## Versions and compatibility

The fork's **bundle version** follows [Semantic Versioning](https://semver.org/).
It is separate from Broadcom API numbers, firmware versions and the library
SONAME. `VERSION` is tracked and included in source archives. It identifies
the source bundle; published tags and assets are listed in
[Releases](https://github.com/ahnhy1324/crystalhd/releases). A version in a
checkout alone does not announce a published release.

| Identity | Current source | Purpose |
| --- | --- | --- |
| Bundle | `1.0.0-rc.1` | Distribution identity from `VERSION` |
| DKMS / module package | `3.10.2` | Maintained package revision; upgrades earlier `3.10.0` / `3.10.1` installs |
| Driver ioctl version | `3.10.0` | Historical public API value, retained |
| Library API / pkg-config | `3.22.0` | Historical library component identity, retained |
| Library SONAME / file | `libcrystalhd.so.3` / `libcrystalhd.so.3.6` | Existing application linkage, retained |
| GStreamer plugin / VA vendor string | `3.10.0` / `0.1` | Historical component labels; not the bundle identity |
| Firmware | Existing BCM70012/BCM70015 images | Retain images, identities and license notices |

Increase the numeric package revision when changing an already registered or
published candidate payload; never replace an existing `/usr/src/crystalhd-VERSION` tree with a
different payload. DKMS compares module versions using its own ordering, so
do not rely on SemVer prerelease suffix ordering for module upgrades.
The public ioctl remains `3.10.0` when the package revision changes.
Record the mapping here before qualification.
The runtime installation record identifies the last completed runtime install;
component checks remain separate. Use the loaded and selected module metadata
reported by `crystalhd-check` to identify the driver.

The public compatibility boundary covers `libcrystalhd.so.3`, legacy ioctl
layouts, documented buffer ownership/completion, and the documented
non-experimental player/plugin behavior. Native output remains default,
explicit hardware playback must fail visibly if unavailable, and one decoder
owns the device at a time. Pre-stable fixes use further `1.0.0` RCs and require
affected requalification. After stable publication, compatible fixes use
`1.0.x`; compatible features use `1.x.0`. A development phase or internal
rewrite alone does not require a new major version.

The following remain explicitly experimental: the VA-API frontend, its live
H.264 and low-latency controls, `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_*`,
`CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC`,
`CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT`, the raw-frame producer API, and the
browser launcher. Their exact scope is in the [option index](USAGE.md#advanced-option-index)
and component guides. The optional V4L2/firmware research builds do not add a
supported frontend. A regression in an advertised opt-in path still requires
a fix or explicit withdrawal from the candidate.

## Freeze and prepare

1. Agree on the launch scope in #369. Include playback/ownership fixes,
   negotiated event readiness with old-component fallback, session L0s,
   progressive decoder scaling and the documented Moonlight experiment.
   After freezing, accept release fixes, documentation and qualification work.
2. Merge preparation changes. Check this version table, installation and
   rollback commands, notices and the curated [change summary](HISTORY.md).
   Use a short-lived `release/1.0` branch only if unrelated development needs
   to continue; otherwise select a fixed master commit.
3. Run the existing device-free gates with the
   [developer dependencies](README.md#developer-checks) installed:

   ```sh
   NODE_PATH=/usr/share/nodejs make -j2 W=1 check
   make userspace32-check
   make legacy-cpu-check
   ```

   Require completed CI success, including the older GStreamer build. Test
   a source archive without `.git`, staged install/uninstall, plugin discovery,
   and the documented direct and DKMS upgrade/rollback paths. A staged layout
   check does not establish a live module installation or hardware pass.
4. RC.1 is already published. Record any corrected candidate's exact commit,
   diff, release text and results in #369 and obtain the maintainer's approval
   before publishing a new RC number. Never move published tags, including
   `v1.0.0-rc.1` and `phase1-baseline-2026-09-29`.

## Qualify the candidate

Use [tests/phase1-release-gate.sh](tests/phase1-release-gate.sh) with the tracked
[maintenance codec oracle](tests/maintenance-oracle.tsv) as `--manifest`.
Its 11 fixtures retain exact matching; only the WMV3 HD missing-duration metadata
reflects the intentional [#341 correction](https://github.com/ahnhy1324/crystalhd/pull/341).
The [Phase 1 oracle](tests/phase1-oracle.tsv) stays frozen as historical evidence.
Keep the full `--rc` thresholds and fixtures unchanged. Aggregate evidence
validation must explicitly select and exactly verify the maintenance manifest;
individual runs do not establish an aggregate pass. Preserve historical
Phase 1 evidence validation as a separate path.
Run hardware tests from a clean Git checkout of the candidate, serially on the
qualified BCM70015 host. The existing hardware gate records Git identity and
requires its tracked oracle even with `--runtime installed`; the archive
build/install check above is separate. Before starting,
record candidate commit/tag, loaded/selected module and userspace identities,
host versions, options, fixture IDs, commands and result location once per
run set. Keep logs and private media out of the source tree.

G0–G6 must all PASS for the final qualifying candidate, with an explicit
experimental disposition and maintainer approval. Missing, stale, failed or
inconclusive mandatory evidence means **HOLD**. Follow the detailed #371
procedure and #369 acceptance criteria; these are the finite stable gates:

| Gate | Declared run and acceptance |
| --- | --- |
| G0 — Recoverable reference | Before OS changes: offline whole-system image on independent storage, verified integrity and rescue-media access, preserved configuration/source/fixtures and a concrete restore procedure. Prefer spare-disk restore-and-boot; without a spare, record `restore boot NOT TESTED` and obtain explicit operator acceptance before migration. Integrity and recovery-media checks remain mandatory. |
| G1 — LTS admission | Actual X61/BCM70015 on the selected supported Ubuntu LTS; record exact OS/kernel/userspace/graphics. Rebuild the preserved RC first, verify loaded/installed identities and real native/scaled/VA decode, then freeze any necessary corrected candidate. Preserve comparable LXQt/X11 settings and record differences. |
| G2 — Correctness and compatibility | All 11 maintenance-oracle entries: defined counts, geometry/fields, PTS and pixels, valid EOS/drain and clean reopen. Candidate CI, native/i686/no-SSE and older supported kernel/GStreamer builds pass. Cover event wait/close/cancellation, mixed-version fallback, B-frame tail/replay, L0s restoration/force precedence and monitor-only close; distinguish mock from physical evidence. |
| G3 — Core endurance and lifecycle | Clocked H.264+AAC 720p30: **8 h / 864,000 pictures**; qualified native H.264 High 1080p30: **2 h / 216,000 pictures**; **1,000** complete sessions; **100** complete admitted-codec cycles; **10** control cycles and final idle reload/reopen. Retain full timing/resource limits, pause/resume, seek, flush/replay and reopen; exclude unsupported native FHD 2x. No recovery retries/reloads may mask failures. |
| G4 — Retained new paths | Progressive Full-HD input → 1280-wide output: **2 h**, correct geometry/PAR, count/EOS, pixel checks and seek/native-reopen controls. Actual admitted VA event-RX worker: **at least 2 h**, with output progress, bounded resources and no lost wake/stuck retirement. GStreamer-only soak cannot establish VA-worker coverage. |
| G5 — Installation and rollback | Final archive without `.git`, first install, RC/previous-package update, removal and rollback on direct/DKMS routes without mixed ownership. Fail before removal if prerequisites/builds fail; use idle-only unload and verify `crystalhd-check`, known decode/EOS and reopen. Distinguish staging from live installation. |
| G6 — Documentation and final artifact | Follow #369's fresh-user, update, slow-FHD, Moonlight and failed-run journeys. Check links, quoted placeholders, prerequisites, success/errors, experimental scope, component identity and notices. Stable-tag instructions become available only after publication. Final metadata/payload changes need CI/build/archive/install checks and affected runtime rechecks. |

A live copy on the SSD being upgraded does not meet G0. If the OS was already
changed without the required independent offline image, retain that gap as
HOLD in #369/#371; a later LTS image cannot establish a pre-migration recovery
pass. Any exception requires an explicit maintainer decision recorded in the
release issues; the current G0 decision is identified above.

Complete the planned experimental assessment: **60 minutes** exact 720p30
Moonlight and **60 minutes each** `previous,0` 720p60/1080p30, followed by
graceful disconnect/reopen, plus the documented VLC check. Record hardware use,
source/receive/output rates, refresh/settings, payload progression/age where
observable, resources and beginning/end A/V-cue observations. Give each retained
mode an explicit tested experimental PASS, limited/unverified LTS disposition,
or actual withdrawal of a defective path with affected rechecks. Intentional
previous-frame behavior is assessed under its documented contract; no universal
60 fps or physical A/V certification is implied. Unsafe shared DMA/session
behavior blocks release even when an experimental mode triggers it.

In the hardware report, add one dated candidate qualification table using
**PASS / FAIL / NOT RUN**, with source/OS identities, actual duration/count,
evidence links and affected scope. Only optional work may be **DEFERRED**.
Do not copy historical runs into the candidate's PASS column. Require no
unexpected DMA/PCIe/AER errors, stuck teardown or unexplained resource growth.
Record perceptual display/speaker feedback separately from headless results;
do not call a walkthrough novice-tested without an unfamiliar reader.

If a change affects runtime, ownership, packaging or defaults, rerun the
affected gates on the new fixed candidate; affected endurance tests restart
from zero. Explain reuse of unaffected results with the diff/impact rationale.
Text-only corrections require document
checks, not every hardware experiment. Core regressions, install/build
failures and misleading support claims block release. Additional hardware,
standalone processing and physical Moonlight A/V certification remain tracked
in #91/#92/#204 without becoming prerequisites for this declared scope.

## Publish and maintain

After #371 hands off G0–G4 evidence and experimental dispositions, freeze final
version metadata and the module-package mapping. Account for bundle VERSION
data compiled into the module; verify the actual final CI/build/archive/install,
upgrade/rollback payload and rerun affected runtime gates. Present the exact
final commit, G0–G6 results, release text, remaining limits and GO/HOLD row for
maintainer approval. #371 can finish its handoff before the stable tag exists.
Create an annotated
`v1.0.0` tag and a normal GitHub release only after approval. Follow
[GitHub's release procedure](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).

The release body should contain highlights, install/update/rollback,
LTS validated scope, experimental options, known limits, evidence and
acknowledgements, in that order. Use `HISTORY.md` as the starting summary. Offer source tag
archives first; retain required notices. If optional binary assets are added,
publish their checksums. Smoke-test the public archive and its installation
commands, then link the result in #369. External announcements need separate
approval. Keep #91/#92/#204 open for their own remaining objectives.

Retain the pre-LTS recovery evidence and a separately verified LTS known-good
image. Keep images and private inventories off GitHub. After stable publication,
compatible fixes inform 1.0.1; added compatible features inform 1.1.0. Evidence
alone need not trigger a new release.

Use an issue for a distinct unresolved objective or reproduced defect; use a
PR directly for a small self-contained fix. Name short-lived branches
`fix/…`, `feature/…`, `docs/…` or `research/…` and record their issue/owner in
the PR. Delete after verified integration and checking for post-merge work.
Preserve unique research fixtures and failures before removing their branch;
retain unmerged V4L2 PR #115 and its stateful-contract failure evidence.
Keep usage in the guides, results in the hardware report, and current work in
issues. Hardware support claims require hardware evidence.

## Useful bug reports

Include the release tag, sanitized `crystalhd-check` output, card/chipset,
kernel/userspace, player/API and options, reproducible codec/profile/geometry,
relevant error logs, and whether rollback or default frame matching changes
the result. Credentials, pairing secrets and private media are unnecessary.
