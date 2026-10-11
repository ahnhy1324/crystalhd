# Releasing the maintained CrystalHD stack

The launch tracker is [#369](https://github.com/ahnhy1324/crystalhd/issues/369).
This procedure prepares the maintained legacy stack for users; optional
firmware research has its own acceptance criteria in #92 and #204.

## Versions and compatibility

The fork's **bundle version** follows [Semantic Versioning](https://semver.org/).
It is separate from Broadcom API numbers, firmware versions and the library
SONAME. `VERSION` is tracked and included in source archives. An RC version in
that file identifies preparation sources; only a published tag and release
make it an available release candidate.

| Identity | 1.0.0-rc.1 preparation | Purpose |
| --- | --- | --- |
| Bundle | `1.0.0-rc.1`; no tag published yet | Distribution identity from `VERSION` |
| DKMS / module package | `3.10.1` | Maintained package revision; distinct from existing `3.10.0` installs |
| Driver ioctl version | `3.10.0` | Historical public API value, retained |
| Library API / pkg-config | `3.22.0` | Historical library component identity, retained |
| Library SONAME / file | `libcrystalhd.so.3` / `libcrystalhd.so.3.6` | Existing application linkage, retained |
| GStreamer plugin / VA vendor string | `3.10.0` / `0.1` | Historical component labels; not the bundle identity |
| Firmware | Existing BCM70012/BCM70015 images | Retain images, identities and license notices |

Increase the numeric package revision for a changed published candidate
payload; never replace an existing `/usr/src/crystalhd-VERSION` tree with a
different payload. DKMS compares module versions using its own ordering, so
do not rely on SemVer prerelease suffix ordering for module upgrades.
For example, a later candidate or final package can use `3.10.2` while the
public ioctl remains `3.10.0`. Record the mapping here before qualification.
The runtime installation record identifies the last completed runtime install;
component checks remain separate. Use the loaded and selected module metadata
reported by `crystalhd-check` to identify the driver.

The public compatibility boundary covers `libcrystalhd.so.3`, legacy ioctl
layouts, documented buffer ownership/completion, and the documented
non-experimental player/plugin behavior. Native output remains default,
explicit hardware playback must fail visibly if unavailable, and one decoder
owns the device at a time. Additive, compatible fixes use `1.0.x`; compatible
features use `1.x.0`. A development phase or internal rewrite alone does not
require a new major version.

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
4. Record the exact commit, release text and pre-RC results in #369. Obtain
   the maintainer's go/no-go before publishing `v1.0.0-rc.1` as a prerelease.
   Later candidates use a new RC number; never move published tags, including
   `phase1-baseline-2026-09-29`.

## Qualify the candidate

Use [tests/phase1-release-gate.sh](tests/phase1-release-gate.sh) and the frozen
[codec oracle](tests/phase1-oracle.tsv). Keep their thresholds and fixtures.
Run hardware tests from a clean Git checkout of the candidate, serially on the
qualified BCM70015 host. The existing hardware gate records Git identity and
requires its tracked oracle even with `--runtime installed`; the archive
build/install check above is separate. Before starting,
record candidate commit/tag, loaded/selected module and userspace identities,
host versions, options, fixture IDs, commands and result location once per
run set. Keep logs and private media out of the source tree.

| Gate | Declared run and acceptance |
| --- | --- |
| Frozen behavior | All 11 oracle entries; exact counts, metadata and pixels where defined, EOS and clean reopen. |
| Sustained playback | Existing `--rc` High/AAC 720p30 soak: 8 hours; qualified 1080p30 soak: 2 hours. Exact counts/EOS, existing resource/error limits. Older soaks remain historical. |
| Lifecycle | Existing `--rc` 1,000-session churn, 100 five-codec round-robin cycles, ten 720p control cycles and reload sanity. No retained owner, leaked resources or new kernel faults. |
| Event readiness | Existing mock coverage for wait/wake/close, cancellation and mixed-version fallback; native H.264 B-frame tail/seek/reopen. Identify which results are mock versus hardware. |
| Scaling | Existing FHD fixture at native, 1280 and non-round 130 widths: exact count/EOS, actual geometry/PAR and pixel check; flushing seek and separate native reopen. Explicit SW enlargement is measured separately. |
| L0s lifecycle | Existing mock tests plus bounded native opt-out, session opt-in, force precedence and repeated close/error restoration checks. Record the qualified link policy; no live register sweeps. |
| Integrations | Documented VLC combination and three bounded 60-second Moonlight runs: exact 720p30, `previous,0` 720p60/1080p30, each followed by graceful reopen. These do not measure physical streaming A/V or forced-loss recovery. |
| User journeys | Follow the #369 fresh-user, upgrade, slow-FHD, Moonlight and failed-first-run walkthroughs. Verify links, quoted placeholders, action preconditions and useful success/error messages. |

In the hardware report, add one dated candidate qualification table using
**PASS / FAIL / NOT RUN / DEFERRED**, with evidence links and affected scope.
Do not copy historical runs into the candidate's PASS column. Require no
unexpected DMA/PCIe/AER errors, stuck teardown or unexplained resource growth.
Record perceptual display/speaker feedback separately from headless results;
do not call a walkthrough novice-tested without an unfamiliar reader.

If a change affects runtime, ownership, packaging or defaults, rerun the
affected gates on the new candidate. Text-only corrections require document
checks, not every hardware experiment. Core regressions, install/build
failures and misleading support claims block release. Additional hardware,
standalone processing and physical Moonlight A/V certification remain tracked
in #91/#92/#204 without becoming prerequisites for this declared scope.

## Publish and maintain

After qualification, present the exact final commit, release text and remaining
limits for maintainer approval. Update final version metadata before the final
archive/install checks; rerun any affected gates. Create an annotated
`v1.0.0` tag and a normal GitHub release only after approval. Follow
[GitHub's release procedure](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).

The release body should contain highlights, installation/upgrade links,
validated scope, experimental options, known limits/rollback, evidence and
acknowledgements. Use `HISTORY.md` as the starting summary. Offer source tag
archives first; retain required notices. If optional binary assets are added,
publish their checksums. Smoke-test the public archive and its installation
commands, then link the result in #369. External announcements need separate
approval. Keep #91/#92/#204 open for their own remaining objectives.

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
