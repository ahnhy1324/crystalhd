# Releasing the maintained CrystalHD stack

The current release tracker is
[#430](https://github.com/ahnhy1324/crystalhd/issues/430). This procedure
prepares the maintained legacy stack for users; optional firmware research has
its own acceptance criteria in #92 and #204.

The completed 1.0 campaign is historical evidence.
The published `v1.0.0-rc.1` at
`79c7cc963f69a6cbe22852c7fa30cee71a0c1737` and its bounded results remain
unchanged.
After qualification and explicit maintainer GO,
[stable `v1.0.0`](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.0.0)
was published on 2026-10-13 from approved commit
`2e91a4a612efe236e566362022bf986c1d81c67b`. Its tag is immutable.
The maintainer's G0 exception and the G1/G2 dispositions recorded in
[#369](https://github.com/ahnhy1324/crystalhd/issues/369) and
[#371](https://github.com/ahnhy1324/crystalhd/issues/371) apply only to that
campaign; they are not v1.1 qualification passes.

For v1.1, follow **scope and diff audit → device-free gates and short native
smoke → freeze exact candidate → maintainer RC GO → publish immutable RC →
qualify that exact RC → final package and rollback checks → maintainer stable
GO**. A version string in a checkout is preparation, not permission to publish
`v1.1.0-rc.3` or `v1.1.0`.

The RC.3 interlaced scope is bounded playback with explicit hardware/software
Weave selection. Further deinterlacing and A/V synchronization tuning are
deferred; qualitative playback observations do not establish universal
frame-rate or physical timing guarantees.

## Versions and compatibility

The fork's **bundle version** follows [Semantic Versioning](https://semver.org/).
It is separate from Broadcom API numbers, firmware versions and the library
SONAME. `VERSION` is tracked and included in source archives. It identifies
the source bundle; published tags and assets are listed in
[Releases](https://github.com/ahnhy1324/crystalhd/releases). A version in a
checkout alone does not announce a published release.

| Identity | Current source | Purpose |
| --- | --- | --- |
| Bundle | `1.1.0-rc.3` | v1.1 release-candidate identity from `VERSION` |
| DKMS / module package | `3.10.6` | Candidate package revision; distinct from frozen `3.10.5` and stable `3.10.3` |
| Driver ioctl version | `3.10.0` | Historical public API value, retained |
| Library API / pkg-config | `3.22.0` | Historical library component identity, retained |
| Library SONAME / file | `libcrystalhd.so.3` / `libcrystalhd.so.3.6` | Existing application linkage, retained |
| GStreamer plugin / VA vendor string | `3.10.0` / `0.1` | Historical component labels; not the bundle identity |
| Firmware | Existing BCM70012/BCM70015 images | Retain images, identities and license notices |

Published [v1.1.0-rc.3](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0-rc.3)
pins `51e5cdfe4c720da92dec54c72fc4f47c8382e43a`, bundle `1.1.0-rc.3` and
package `3.10.6`. Its Gate A results and maintainer RC GO are recorded in
#430; Gate B and stable GO remain separate. The earlier `1.1.0-rc.2` / `3.10.5` candidate
was frozen but never published; retain its registered source/build unchanged
for rollback rather than replacing it with RC.3 bytes.
The published `v1.0.0-rc.1` retains bundle
`1.0.0-rc.1` and package `3.10.2`; stable `v1.0.0` retains bundle `1.0.0` and
package `3.10.3`.

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
owns the device at a time. A change to a frozen or published v1.1 candidate
uses the next RC and a fresh package revision, with affected requalification;
published tags are never moved. After a stable release, compatible fixes use
the next patch version and compatible features use the next minor version. A
development phase or internal rewrite alone does not require a new major
version.

The following remain explicitly experimental: the VA-API frontend, its live
H.264 and low-latency controls, `CRYSTALHD_VAAPI_EXPERIMENTAL_READY_*`,
`CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC`,
`CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT`, the raw-frame producer API, and the
browser launcher. Their exact scope is in the [option index](USAGE.md#advanced-option-index)
and component guides. The optional V4L2/firmware research builds do not add a
supported frontend. A regression in an advertised opt-in path still requires
a fix or explicit withdrawal from the candidate.

## Freeze and prepare

1. Agree on the v1.1 scope in #430. Audit the complete `v1.0.0...candidate`
   diff, link #429's compatibility results and separate supported production
   behavior from default-off research and explicitly unsupported paths.
   After freezing, accept only release fixes, documentation and qualification
   work.
2. Merge preparation changes. Check this version table, installation and
   rollback commands, notices and the curated [change summary](HISTORY.md).
   Use the short-lived `release/1.1` branch while unrelated development
   continues, then record one fixed candidate commit.
3. Run the existing device-free gates with the
   [developer dependencies](README.md#developer-checks) installed:

   ```sh
   NODE_PATH=/usr/share/nodejs make -j2 W=1 check
   make userspace32-check
   make legacy-cpu-check
   make vaapi-cpu-check
   ```

   Require completed CI success, including the older GStreamer build. Test
   a source archive without `.git`, staged install/uninstall, plugin discovery,
   and the documented direct and DKMS upgrade/rollback paths. A staged layout
   check does not establish a live module installation or hardware pass.
4. Complete #430 Gate A, including the declared short native smoke. Record the
   exact commit, source diff, feature list, limits, acknowledgements, release
   text and results, then obtain explicit maintainer RC GO before publishing
   `v1.1.0-rc.3`. Never move published tags, including `v1.0.0-rc.1`,
   `v1.0.0` and `phase1-baseline-2026-09-29`.

## Qualify the candidate

Begin the full campaign only after the immutable v1.1 RC is published. The
v1.0 results remain useful baselines, but do not count as new v1.1 PASS results.
Use [tests/phase1-release-gate.sh](tests/phase1-release-gate.sh) with the tracked
[maintenance codec oracle](tests/maintenance-oracle.tsv) as `--manifest`.
Its 11 fixtures retain exact matching; only the WMV3 HD missing-duration metadata
reflects the intentional [#341 correction](https://github.com/ahnhy1324/crystalhd/pull/341).
The [Phase 1 oracle](tests/phase1-oracle.tsv) stays frozen as historical evidence.
For the qualified **GStreamer 1.28.2** environment, explicitly select
[its maintenance variant](tests/maintenance-oracle-gst-1.28.2.tsv) instead.
Its version condition is exactly `1.28.2`, not `1.28.2` or later. It records
the documented H.264 duration-metadata difference, without relaxing frame
counts, PTS, pixels, test duration or resource limits. Other environments follow
their validated manifest rules; do not automatically switch to this variant
to avoid a failure or bypass the version check.
Keep the full `--rc` thresholds and fixtures unchanged. Pass the same manifest
explicitly to oracle, round-robin and aggregate evidence validation.
`evidence-check` without `--manifest` defaults to historical
`tests/phase1-oracle.tsv`, not the maintenance oracle. Individual runs do not
establish an aggregate pass; preserve historical Phase 1 validation separately.
Run hardware tests from a clean Git checkout of the candidate, serially on the
qualified BCM70015 host. The existing hardware gate records Git identity and
requires its tracked oracle even with `--runtime installed`; the archive
build/staging/install check above is separate. Path construction with `pwd -P`
and ordinary build/install instructions do not require `.git`; the hardware
release gates do. Before starting,
record candidate commit/tag, loaded/selected module and userspace identities,
host versions, options, fixture IDs, commands and result location once per
run set. Keep logs and private media out of the source tree.

Manifest-related examples for that exact environment, from the source root:
replace the fixture and evidence placeholders with your absolute paths.
Each hardware result directory must be new and outside the source tree.
Create only the campaign parent below; leave its per-gate directories absent.
Run commands separately and stop on failure. `manifest-check` checks structure;
the hardware oracle and evidence validation enforce the exact version match.

```sh
repo_root=$(pwd -P)
gate="$repo_root/tests/phase1-release-gate.sh"
manifest="$repo_root/tests/maintenance-oracle-gst-1.28.2.tsv"
fixture_root="/absolute/path/to/fixtures"
evidence_root="/absolute/path/to/new-campaign"

mkdir -m 0750 -- "$evidence_root"
sh "$gate" manifest-check "$manifest"
sh "$gate" oracle --runtime source --results "$evidence_root/oracle" \
  --manifest "$manifest" --fixture-root "$fixture_root"
sh "$gate" round-robin --rc --runtime source \
  --results "$evidence_root/round-robin" \
  --manifest "$manifest" --fixture-root "$fixture_root"
```

These are excerpts, not the full campaign. Only after all required stages
(including soak-720p, soak-1080p, churn, controls and reload-sanity) are complete
with the same candidate/runtime identity, validate the aggregate:

```sh
sh "$gate" evidence-check "$evidence_root" --manifest "$manifest"
```

G0–G6 must all PASS for the final qualifying candidate, with an explicit
experimental disposition and maintainer approval. Missing, stale, failed or
inconclusive mandatory evidence means **HOLD**. Follow #430's v1.1 acceptance
criteria and the proven mechanics in #369/#371; these are the finite stable
gates:

| Gate | Declared run and acceptance |
| --- | --- |
| G0 — Recoverable reference | Before destructive candidate installation or testing: current known-good whole-system image on independent storage, verified integrity and rescue-media access, preserved configuration/source/fixtures and a concrete restore procedure. Prefer spare-disk restore-and-boot; without a spare, record `restore boot NOT TESTED` and obtain explicit operator acceptance. Integrity and recovery-media checks remain mandatory. |
| G1 — LTS admission | Actual X61/BCM70015 on the selected supported Ubuntu LTS; record exact OS/kernel/userspace/graphics. Rebuild the preserved RC first, verify loaded/installed identities and real native/scaled/VA decode, then freeze any necessary corrected candidate. Preserve comparable LXQt/X11 settings and record differences. |
| G2 — Correctness and compatibility | All 11 maintenance-oracle entries: defined counts, geometry/fields, PTS and pixels, valid EOS/drain and clean reopen. Candidate CI, native/i686/no-SSE and older supported kernel/GStreamer builds pass. Cover event wait/close/cancellation, mixed-version fallback, B-frame tail/replay, L0s restoration/force precedence and monitor-only close; distinguish mock from physical evidence. |
| G3 — Core endurance and lifecycle | Clocked H.264+AAC 720p30: **8 h / 864,000 pictures**; qualified native H.264 High 1080p30: **2 h / 216,000 pictures**; **1,000** complete sessions; **100** complete admitted-codec cycles; **10** control cycles and final idle reload/reopen. Retain full timing/resource limits, pause/resume, seek, flush/replay and reopen; exclude unsupported native FHD 2x. No recovery retries/reloads may mask failures. |
| G4 — Retained new paths | Progressive Full-HD input → 1280-wide output: **2 h**, correct geometry/PAR, count/EOS, pixel checks and seek/native-reopen controls. Actual admitted VA event-RX worker: **at least 2 h**, with output progress, bounded resources and no lost wake/stuck retirement. GStreamer-only soak cannot establish VA-worker coverage. |
| G5 — Installation and rollback | Final archive without `.git`, first install, RC/previous-package update, removal and rollback on direct/DKMS routes without mixed ownership. Fail before removal if prerequisites/builds fail; use idle-only unload and verify `crystalhd-check`, known decode/EOS and reopen. Distinguish staging from live installation. |
| G6 — Documentation and final artifact | Follow #430 and the established #369 fresh-user, update, slow-FHD, Moonlight and failed-run journeys. Check links, quoted placeholders, prerequisites, success/errors, experimental scope, component identity and notices. Stable-tag instructions become available only after publication. Final metadata/payload changes need CI/build/archive/install checks and affected runtime rechecks. |

G0 requires a verified independent offline known-good image, integrity and
rescue access, and a defined restore path before destructive candidate
installation or testing. A live copy on the SSD being upgraded does not meet
it. Any exception requires a new explicit maintainer decision recorded in
#430; the v1.0 exception is not inherited.

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

After #430 records G0–G6 evidence and experimental dispositions, freeze final
version metadata and a package revision distinct from any changed registered
RC payload. Account for bundle VERSION data compiled into the module; verify
the actual final CI/build/archive/install and upgrade/rollback payload, and
rerun affected runtime gates. Present the exact final commit, results, release
text, remaining limits and GO/HOLD row for maintainer approval. Create an
annotated `v1.1.0` tag and a normal GitHub release only after explicit stable
GO. Follow
[GitHub's release procedure](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).

The release body should contain highlights, install/update/rollback,
LTS validated scope, experimental options, known limits, evidence and
acknowledgements, in that order. Use `HISTORY.md` as the starting summary. Offer source tag
archives first; retain required notices. If optional binary assets are added,
publish their checksums. Smoke-test the public archive and its installation
commands, then link the result in #430. External announcements need separate
approval. Keep #91/#92/#204 open for their own remaining objectives.

Retain the pre-LTS recovery evidence and a separately verified LTS known-good
image. Keep images and private inventories off GitHub. After v1.1 stable
publication, compatible fixes inform 1.1.1; added compatible features inform
1.2.0. Evidence alone need not trigger a new release. The published v1.0 tags,
release notes and qualification record remain unchanged.

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
