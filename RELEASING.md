# Releasing the maintained CrystalHD stack

This guide records published stable 1.3, RC4 and RC3 scope
alongside completed release campaigns. Installation belongs in [USAGE](USAGE.md)
and the [DKMS guide](README.dkms); the current native companion has its own
[delivery guide](NATIVE-RUNTIME-DELIVERY.md). See [TODO](TODO.md) for the live
work queue and [documentation map](DOCUMENTATION.md) for guide ownership.

- [Published stable 1.3 qualification](#published-stable-13-qualification)
- [Published RC4 qualification record](#published-rc4-qualification-record)
- [Current 1.3 quality test procedure](#current-13-quality-test-procedure)
- [RC3 complete scope and candidate preparation](#rc3-complete-scope-and-candidate-preparation)
- [Frozen RC2 baseline](#frozen-rc2-baseline)
- [Version identities and compatibility policy](#versions-and-compatibility)
- [Completed v1.2 scope and campaign reference](#historical-v12-release-scope-and-publication)
- [Branch and issue maintenance](#branch-and-issue-maintenance)
- [Useful bug reports](#useful-bug-reports)

<a id="stable-13-promotion-preparation"></a>

## Published stable 1.3 qualification

[Stable `1.3.0`](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0),
frozen source `24a5182b988b` / module-DKMS `3.10.20`, is published with
12 assets after protected [#655](https://github.com/ahnhy1324/crystalhd/pull/655)
integration, current-base CI (15 successful jobs/two intended skips), final
independent actual-evidence review and download/checksum verification.
The [qualification receipt](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0/release-qualification.json)
and [complete claim map](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0/complete-claim-map.json)
are authoritative for the exact assets, actual outcomes and scoped reuse.
Use the [ordinary installation guide](README.md#quick-start), matching settings
and optional [native companion instructions](NATIVE-RUNTIME-DELIVERY.md).
Published 1.2/module13 remains the immutable fallback.

All 17 integrated feature groups and defaults remain, including default-off
Centered, optional Xv/field/MPEG-1 and the native V4L2/unmodified FFmpeg DRM_PRIME
companion. Final qualification freshly binds the exact Git-free archive/build,
new README-bearing settings ZIP, installed module20 DKMS7/direct7 lifecycle
(including frozen RC4/module19 rollback), native5 API/ownership/original
720p/FHD EOS/reopen checks, two reproducible outer exports and 13 file-tool
operations. Original module13/runtime36, module/DKMS inventory, idle owners/pins
and kernel state restore. Add-on code/manifest0.1.0/UI is unchanged; README-only
ZIP bytes have a separately qualified artifact identity and scoped functional
UI reuse. Metadata-only promotion adds no runtime feature or default change.

The source600/frozen-kernel18 ordinary and own-FIFO 30-minute results, actual
RC4/module19 witnesses, original RC3 codec/native/stock-Kodi/Centered/field/UI
and other unchanged components retain their original identities. Three-hour
Bob reuse is unchanged normal VA/DIL on actual module16 with 540,000 ordered
full-NV12 outputs, not a fresh whole-kernel20 soak. No original result is
renamed final20. Original failures, the separate FIFO cooling continuation,
accepted renderer/QoS policy, exact-route no-drift-log acceptance and
unavailable-target/physical-presentation limits remain explicit. Reused FFmpeg,
providers and source/build materials remain unmodified with original pins and
notices; final own-core/module20 and tools have separate bindings.

The [bounded GTK review](tests/fixtures/issue400/gtk-bounded-review-20261104.json)
found no demonstrated own-code cause or repair for the original optional-editor
assertion/SIGSEGV. #623 stays open and unfixed; clean runs are not a repair.
Known hardware-GL LSan and stock native RGB-range failures remain. The agreed
release criteria are met with this explicit risk disposition; the 13 remaining
research/support issues do not become new promotion gates. Frozen final1.3,
RC4/RC3/RC2 artifacts and registered source20/19/18/17 remain immutable.

<a id="rc4-stabilization-candidate-preparation"></a>
<a id="frozen-rc4-preparation-record"></a>

## Published RC4 qualification record

[RC4](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.4) is
published from `1c8b0451c631`, bundle `1.3.0-rc.4` / module-DKMS `3.10.19`,
with 12 downloaded-and-verified assets. Its exact archive/build, native/API and
original-client, installed recovery, companion/tool lifecycle, current-base CI
and independent review pass through [#654](https://github.com/ahnhy1324/crystalhd/pull/654).
Its [actual receipt](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0-rc.4/release-qualification.json)
preserves source19 and scoped earlier reuse; none is relabeled final20.
The frozen source's preparation checkpoint stays historical and its assets and
registered sources remain unchanged after final promotion. The quality
procedure below remains the maintained procedure for future affected changes;
historical candidate sections retain their original source-specific scope.

## Current 1.3 quality test procedure

Use this procedure to plan affected candidate checks and report their actual
outcomes. The [published stable qualification](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0/release-qualification.json)
and [claim map](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0/complete-claim-map.json)
identify the existing fresh/reused evidence. The [retained G2 definitions](HARDWARE-2026-09-13.md#120-rc2-qualification--2026-10-22)
and owning issues [#91](https://github.com/ahnhy1324/crystalhd/issues/91) and
[#510](https://github.com/ahnhy1324/crystalhd/issues/510) retain earlier failures
and acceptance limits. This procedure is not a result or authorization to publish.

1. **Freeze and build.** Identify the intended source and actual archive/runtime;
   finish local compilation, offline regressions and private staging before media.
   Run one card/display campaign at a time on the test host, with no parallel
   compiler or soak. Independent hosted CI may run alongside it.
   Preserve the known working module, runtime, source, options and user settings.
2. **Reproduce installation and recovery.** Exercise first installation,
   update/reinstall, rollback, removal/repeated removal and reinstall through
   the applicable [direct](BRINGUP.md#direct-install-update-and-rollback) and
   [DKMS](README.dkms) routes. Check the selected/loaded module and completed
   runtime receipt separately, preserve user settings, and verify idle ownership,
   pin/refcount, PM/AER/kernel and original-file restoration after recovery.
   Never replace an immutable registered source with a moving checkout.
3. **Check affected decoding and frontends.** Keep the existing source-picture,
   PTS/pixel, ABI and ownership oracles, complete-file EOS and clean reopen.
   Cover affected settings/UI and stock Kodi Auto/Off/progressive controls.
   For changed Centered code, use the normal installed backend: counted NV12
   readback/export/retained seek, short two-EOF controls and the selected
   five-minute natural play. Independent ROI pixels remain a separate oracle.
   Changed native producer/range/lifetime code needs its actual native module
   and [pinned original Kodi/FFmpeg companion](NATIVE-RUNTIME-DELIVERY.md#rc3-companion-layout-and-use),
   including selected 720p/FHD 180-picture EOS/reopen twice and normal retirement.
   Private observers or modified clients cannot qualify stock compatibility.
4. **Run affected application endurance.** The selected ordinary Player route
   is continuous 30-minute TFF25 input, card width 1280, CPU LINEAR Bob at 50 fields/s,
   Xv output and the actual Pulse audio clock: **45,000 source originals /
   90,000 expected fields**. Preserve natural EOS, nonempty output, route/clock
   observations, source-qualified no-drift logs and the resource checks below.
   If the own Xv sink changes, run its explicit FIFO route separately from
   ordinary output; do not substitute latest/drop mode or a headless sink.
5. **Retain the distinct VA-Bob obligation.** The selected three-hour path is
   **270,000 originals / 540,000 ordered full-NV12 Bob outputs**, with the
   existing pixel/PTS oracle, natural EOS and resource windows; retain separate
   reopen coverage. This headless oracle does not establish Xv/presentation.
   The retained 1.3 campaign used actual module16: reuse covers only its unchanged
   normal VA/DIL Bob implementation, not the whole core or kernel. Changed native,
   kernel-boundary or application paths need their own affected checks.

Keep the existing AC and **below 75°C** operating boundary; the selected
30-minute Player run enters at **55°C or below**. Preserve the existing
20-second startup/15-second Player stall bounds and the VA soak's 30-second
no-progress bound. On a stop or failed retirement, preserve the evidence and
live resources for handoff; do not force an unload or kill a surviving owner.

The existing absolute resource bounds are RSS at most 1GiB, at most 256 file
descriptors and 128 threads; Player also requires at least 1GiB MemAvailable.
Keep the established first/last 60-sample trend windows: Player medians after
its first minute allow 32MiB RSS growth, VA means after ten minutes allow 64MiB;
both allow 8 descriptors and 4 threads. Record the actual samples and clocks,
not just a terminal status.

The 45,000 count describes the frozen input, not an assertion that sink renders
or decoded-output notifications are source-picture identities. Record actual
decoder, post-filter, rendered/dropped and QoS counts separately. Preserve
accepted route-specific renderer/QoS losses and historical deficits; do not add
a universal zero-loss gate or weaken a full-pixel/PTS oracle. Source-qualified
logs without detected drift suffice for exact-route A/V; physical offset and
scanout remain separate.

Post-RC3 source `6004636a339e0f0ea79614a1855953596e886da4` changes Player/Xv.
Its affected ordinary and own-FIFO 30-minute runs are complete with actual
timing, clocks, resources and retirement results, on separately frozen RC3
kernel18/source36ef. This scoped campaign is not an RC4/module19 or final20 result.
Reuse unchanged VA/DIL/other-path evidence only with explicit source and
implementation scope. Published final20 and RC4/RC3 assets and results stay
unchanged. Record each failure and any bounded continuation separately; later
publication never repairs or relabels an earlier failure.

<a id="experimental-playback-acceptance"></a>

### Experimental playback acceptance

The maintainer's current instruction is to test at least **900 source originals**
per relevant experimental playback path and integrate at or below the previously
accepted target. Preserve source-picture, pixel/PTS, ownership, ABI and retirement
requirements; report fields, renderer selections and QoS drops separately.

The recovered Centered/VA-Bob reference is the five-minute unmodified Kodi21.3
case at source `2fb823b88eaf`: **9,000 complete source originals / 18,000 Bob
outputs**, **18,000 accepted renderer slot epochs / 17,998 unique TRUE-Render
selections**. The maintainer accepted the two missing selections (**0.0111%**)
as a known issue and directed the remaining work to continue. See the
[contemporaneous decision and scope](https://github.com/ahnhy1324/crystalhd/issues/477#issuecomment-6361152819).

Use that reference within its renderer-stage/cadence scope. It is not permission
to lose decoder originals or join source tokens to renderer epochs without
evidence. A shorter >=900-original check remains a bounded observation, not a
repeat of that five-minute case or proof of a stable loss rate. Other accepted
route-specific QoS observations retain their own scope; do not manufacture a
universal zero-loss gate or a new percentage ceiling for every output.

<a id="private-v13-candidate-preparation"></a>
<a id="complete-rc2-and-future-candidate-preparation"></a>

## RC3 complete scope and candidate preparation

**Published RC3:** [bundle `1.3.0-rc.3`](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.3)
/ module-DKMS `3.10.18`, frozen source `36ef4c681f49`, was published on
2026-11-04 with 12 assets. The [qualification receipt](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0-rc.3/release-qualification.json)
and [complete claim map](https://github.com/ahnhy1324/crystalhd/releases/download/v1.3.0-rc.3/complete-claim-map.json)
record completed archive/build, actual legacy/native/output, installed
DKMS/direct lifecycle, stock Kodi/normal Centered, 30-minute Player and
companion checks, with original failures and reuse scopes preserved.
The preparation steps below document the packaging and qualification workflow;
RC3 publication is complete. Source documentation and version strings alone
do not qualify a candidate. Published RC3 and the [RC2 baseline](#frozen-rc2-baseline)
remain immutable. Stable 1.3 promotion is a separate decision.

RC3 retains every integrated feature and its existing opt-in/default boundary:

| Feature group | RC3 scope |
| --- | --- |
| Ordinary playback stack | Maintained driver/firmware interface, legacy DIL, GStreamer decoder/Player, VA subset, codec/framing/admission, source-picture/PTS and lifecycle repairs; ordinary local playback remains the recommended route. |
| Shared settings | Managed defaults, profiles/presets, headless CLI, optional translated GTK window/tray and stock-Kodi settings ZIP. These were already supplied in RC2. |
| Centered card output | Include the existing default-off BCM70015 H.264/NV12 experiment and its stock-client usage. Smaller card output is centered in a full readable canvas; Kodi zoom is a separate display choice. It is already present in RC2, not merely a new observer. No default promotion, native-FHD-detail or general BWDIF claim. |
| Native V4L2 + DRM_PRIME | Include the optional own native producer/EXPBUF path and the prebuilt **unmodified** FFmpeg companion, complete source/notices/build materials and pinned target instructions. RC3 adds token-bound source range, picture-preserving same-size range transitions and bounded progressive POC0/POC2 multi-slice association. |
| Optional Xv output | Retain explicit FIFO output and packed enlargement; include same-layout port handoff, caps/aspect border repaint and bounded local X11 setup. Latest/drop mode and local ACK/residence timing remain separate, default-off selections without a general speedup claim. |
| Optional field-GPU output | Retain explicit YUY2 output and add qualified direct NV12 input, live window replacement and bounded setup/atom/geometry/resize/handoff response waits. These limits do not bound every EGL/X11 call or establish physical scanout. |
| Other integrated experiments | Retain optional MPEG-1, RX-allocation controls, Moonlight/VA integrations and existing diagnostics under their documented opt-ins and limits. No feature is dropped because broader research remains open. |
| Diagnostic coverage | Preserve centered payload/raw-frame/CPU-copy observers, source-unit controls, emulation and GTK crash capture. Private observer or modified-client results do not qualify the normal backend, repair the unreproduced GTK crash or establish release acceptance. |

See [usage](USAGE.md), [Centered](filters/vaapi/README.md#development-experiment-centered-card-output)
and [native delivery](NATIVE-RUNTIME-DELIVERY.md) for user instructions.
Accepted renderer/QoS losses remain nonblocking within
[#91's recorded scope](https://github.com/ahnhy1324/crystalhd/issues/91#issuecomment-6392485987).
Source-qualified logs without detected A/V drift suffice for the tested route.
These policies retain source-picture, lifetime and ABI correctness. Current
equipment is the target; unavailable Atom/i386/BCM70012 or independently booted
OS coverage is not invented as a new prerequisite.

### Candidate evidence and packaging

Qualify the complete intended candidate under the existing release rules.
Current source checks, module13 observations and older component receipts keep
their actual attribution; they do not by themselves pass the new candidate.
Reuse evidence only for unchanged implementation and state that scope in the
candidate receipt. Changed native range/lifecycle code needs the candidate's
actual native module result. Keep broader BWDIF/GPU/performance research open
without turning all open research into release gates.

Commit the intended source, then run affected checks and package a new output:

```sh
make configuration-check kodi-settings-check candidate-package-check native-delivery-check
make package-candidate PACKAGE_DIR=/absolute/new-private-candidate-directory
```

The core packager requires Python3.8+ and Git and emits an exact-commit Git-free
source tar.gz, Kodi settings ZIP, `candidate-manifest.json` and `SHA256SUMS`.
Fixed archive ordering/times and `SOURCE_REVISION export-subst` preserve source
identity without `.git`. Untracked/ignored files are excluded and existing
outputs are never replaced. Its initial `NOT_RUN_FOR_THESE_ARTIFACTS` is not a
runtime result. Record qualification separately against the actual artifacts;
do not rewrite an already qualified manifest or package.

Extract that source into a fresh directory for its archive build and affected
checks. Validate installed component provenance, direct/DKMS update and
rollback/removal as applicable, ordinary playback, optional settings/stock-Kodi
UI and the changed native route with the matching candidate identity. Headless
`install-runtime` has no GTK dependency; `install-settings` needs GTK/PyGObject
and gettext. The Kodi ZIP needs the matching `editor-state` v1 and its existing
stock-Kodi qualification scope. Report actual reused and new evidence separately.

The frozen RC3 native outer delivery reused only unchanged pinned FFmpeg/provider/build
materials. It binds `materials/crystalhd-core-1.3.0-rc.3-source.tar.gz` to the
new own source/module in `evidence/rc3-core-binding.json`, records fresh native
qualification in `evidence/rc3-native-qualification.json`, and declares reuse
in `evidence/reuse-scope.json`. Historical inner `PUBLIC_NOT_READY` metadata
and RC2 costs/results stay unchanged. Assembly alone is not qualification.

Do not replace registered17 source, RC2 assets or historical RC1 files. A new
module/package identity accompanies changed payloads. Publication uses the
reviewed task branch/PR and current-base CI gate; stable promotion remains
separate. External Kodi/FFmpeg/GStreamer/Mesa/GTK/kernel-core product patches
are prohibited. Confirmed external limits remain known issues. #538 stays
open; only its explicitly excluded heap-abort follow-up remains ignored.

### Frozen RC2 baseline

[1.3.0-rc.2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.3.0-rc.2)
was published with 13 assets, core source `940cf357` and module-DKMS `3.10.17`.
Its current-equipment qualification and companion delivery are complete and
frozen. RC1 remains an incomplete historical cut whose release entry is a
draft. RC2 used a distinct identity to preserve package16 and its receipts.

RC2's combined-source archive/build, final17 native and installed DKMS/direct
lifecycle checks are complete. Selected30minute Player and3hour VA-Bob evidence
was reused only for unchanged runtime implementation from package16. Its
native companion includes two unchanged-source reproductions, complete
source/notice materials, measured fresh acquisition/build costs and acquired
target native EOS/reopen. Its independent booted-OS disposition remains
NOT_RUN/unavailable. #607 closed for that bounded cohort, not broader support.

The frozen core source940/module17 and separately identified delivery-tool
source retain their own attribution; use core source for DKMS, never the
supplemental tool source. PTS-epoch diagnostics, native colour/format/EXPBUF
repairs, optional field-GPU output, producer/lifecycle and host/materials tools,
MPEG-1, maintenance/Xv and optional Kodi UI remain supplied. The historical
private source/UI checkpoint below retains only its original qualified scope.
Do not infer hardware deinterlacing, selector5, general MPEG-1 conformance,
full-rate FHD or acceptance on unavailable equipment.

### Historical private development checkpoint — 2026-11-01

The earlier qualified private checkpoint is source
`c48fe3a12d4f0443dc444df27c0b6c35be2b3ced`, including the own VA sync-binding
and bounded H.264 admission repairs in #613/#615. Experimental merge
`1b17fd9ec1128c56bb55fef1dd79cf1865de3d10` has the same tree
`32c5b89104fd7b9641c0039760894baa3b926ead`.
[CI41259036128](https://github.com/ahnhy1324/crystalhd/actions/runs/41259036128)
passes 15 jobs with the intended standalone packed-scaler job skipped. The
[scoped package and runtime receipt](tests/fixtures/issue510/v13-private-admission-qualification-20261101.json)
records the exact artifact identities and subsequent qualification:

| Private artifact | SHA-256 |
| --- | --- |
| `crystalhd-1.3.0-dev.1-c48fe3a12d4f.tar.gz` | `26ce5a6ec4fafa34b95ece71a9f2cc219c10c1cbb1c99e1d1d16f0ff9d9906cb` |
| `script.crystalhd.settings-0.1.0.zip` | `1712f28d006a3e22a6c3601200a00b6912f720c262847605bc18e06f0be9001a` |

All four generated files reproduce byte-for-byte. The fresh Git-free source
archive builds VA/DIL binaries identical to the hardware-tested checkout and
passes full replay, real DRM import, provenance and user-configuration-preserving
staged install/remove/repeated-remove/reinstall checks. Qualification is pinned to this
source and these bytes; subsequent documentation does not rewrite the frozen
packages or qualify another archive. Existing T8100/BCM70015/Intel965 x86_64
evidence does not supply independently clean OS, physical Atom/i386 or BCM70012
acceptance. Earlier ce4cab1/c488f01 artifacts and their original results remain
frozen.

## Versions and compatibility

The fork's **bundle version** follows [Semantic Versioning](https://semver.org/).
It is separate from Broadcom API numbers, firmware versions and the library
SONAME. `VERSION` is tracked and included in source archives. It identifies
the source bundle; published tags and assets are listed in
[Releases](https://github.com/ahnhy1324/crystalhd/releases). A version in a
checkout alone does not announce a published release.

| Identity | Current value | Purpose |
| --- | --- | --- |
| Published stable / module package | `1.3.0` / `3.10.20` | Frozen source24, 12 verified public assets; actual final archive/install/native/delivery/CI/review and scoped reuse recorded separately |
| Published RC4 / module package | `1.3.0-rc.4` / `3.10.19` | Frozen source1c8, 12 verified public assets; immutable candidate qualification, not final20 attribution |
| Published complete RC3 / module package | `1.3.0-rc.3` / `3.10.18` | Immutable source36ef and 12 assets; actual qualification in its separate receipt/release entry |
| Published complete RC2 / module package | `1.3.0-rc.2` / `3.10.17` | Current-cohort runtime/install/delivery complete; immutable registered payloads |
| Historical partial RC1 / module package | `1.3.0-rc.1` / `3.10.15` | Retained draft release entry and unchanged artifacts; incomplete 1.3 scope |
| Earlier development bundle / module package | `1.3.0-dev.1` / `3.10.14` | Historical private checkpoints |
| Previous stable bundle / module package | `1.2.0` / `3.10.13` | Immutable source `8132043`; qualified public source asset |
| Published RC2 bundle / package | `1.2.0-rc.2` / `3.10.12` | Immutable qualified rollback source |
| Published RC1 bundle / package | `1.2.0-rc.1` / `3.10.11` | Immutable earlier candidate |
| Previous stable bundle | `1.1.0` | Immutable source `85f426d` |
| Previous stable DKMS / module package | `3.10.9` | RC.5 retains `3.10.8`, RC.4 `3.10.7`, stable 1.0.0 `3.10.3` |
| Driver ioctl version | `3.10.0` | Historical public API value, retained |
| Library API / pkg-config | `3.22.0` | Historical library component identity, retained |
| Library SONAME / file | `libcrystalhd.so.3` / `libcrystalhd.so.3.6` | Existing application linkage, retained |
| GStreamer plugin / VA vendor string | `3.10.0` / `0.1` | Historical component labels; not the bundle identity |
| Firmware | Existing BCM70012/BCM70015 images | Retain images, identities and license notices |

Published [v1.2.0-rc.2](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0-rc.2)
pins `b32a94d55bc8cfec781063aca6e1a2012b31f6f3`; its merge
`7f8ce592b0792466d34b33c7538ac392e2a1c091` has the same tree. The public
Git-free source asset has SHA-256
`556e13202fdaa22ef56cd9c795e65716b8472a30598ea609c2e364ed2d521294`.
Its selected Kodi, 30-minute 720p50, three-hour Bob and G3 package/rollback
results remain attributed to RC2. They do not qualify the new final package
identity or authorize stable publication.

Published [v1.1.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0)
pins `85f426d5545a53cc63dd512b355ab6f6c9668d61` after explicit maintainer GO
and [final-package qualification](HARDWARE-2026-09-13.md#110-final-package-qualification--2026-10-19).
The public source archive matches the archive actually built and tested.
[The final execution record](https://github.com/ahnhy1324/crystalhd/issues/430#issuecomment-6256652744)
separates fresh final checks from unchanged-code RC5 reuse and retained limits.
Later documentation updates do not change that source/tag/asset/runtime;
final CI belongs to `85f426d`, not a subsequent document commit.
Retain the RC5 rollback source/cache and all published tags unchanged; never
install the final payload under registered revision `3.10.8`.

Published [RC.5](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0-rc.5)
pins `7f78d01e8103082bd08a62f459c8e20f77a1ec74` and includes the
[post-RC4 changes](HISTORY.md#110-rc5--candidate). The tested public source
archive and fork merge `5a05ac1` preserve the candidate payload.
[Fresh RC5 results](HARDWARE-2026-09-13.md#110-rc5-qualification--2026-10-19)
and [the execution record](https://github.com/ahnhy1324/crystalhd/issues/430#issuecomment-6247676972)
separate completed Gate A/core/VA/final installed checks from the preserved
HTTP observation and blocked Moonlight source assessment.
Earlier development and RC4 results retain their original identities, not new
RC5 passes. Do not recreate or move its published tag; stable GO is separate.
The earlier six-guide follow-up `8cb8eb3` and selected A/V note `e53dd50`
changed no runtime or package bytes; RC5 CI remains attached to `7f78d01`.
The final package above received its own fresh qualification, not a pass
inferred from those earlier documentation commits.

Published [RC.4](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.1.0-rc.4)
pins `a1492c0824fa38748804e92de435da22d99d85de` and includes the reviewed
post-RC.3 fixes and optional HTTP player input. Its exact installed payload
passed [fresh Gate A qualification](https://github.com/ahnhy1324/crystalhd/issues/430#issuecomment-6230790018).
[Gate B results](https://github.com/ahnhy1324/crystalhd/issues/430#issuecomment-6231208771)
and the [hardware summary](HARDWARE-2026-09-13.md#110-rc4-qualification--2026-10-18)
distinguish fresh affected checks from explicitly justified unchanged-code RC.3
reuse; neither an old aggregate nor publication itself qualifies changed code.
The RC4 G0 exception is explicit, not independent restore/boot PASS. That
campaign's remaining work is historical; current candidate obligations are
recorded in #473.

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
The development RX-NOWAIT candidate uses module package `3.10.21`: a new
negotiated FETCH command avoids picture-arrival waits for Flea wait0 callers.
Published1.3/source24a/module20 and its installed baseline remain unchanged;
candidate21 is not yet qualified or published. Legacy FETCH, LINK output/EOS
and positive-timeout waits retain their existing semantics. NOWAIT can still
sleep on normal session/semaphore locks and is not a fix for all timeout values.
The runtime installation record identifies the last completed runtime install;
component checks remain separate. Use the loaded and selected module metadata
reported by `crystalhd-check` to identify the driver.

The public compatibility boundary covers `libcrystalhd.so.3`, legacy ioctl
layouts, documented buffer ownership/completion, and the documented
non-experimental player/plugin behavior. Native output remains default,
explicit hardware playback must fail visibly if unavailable, and one decoder
owns the device at a time. A change to any frozen or published candidate
uses the next RC and a fresh package revision, with affected requalification;
published tags are never moved. After a stable release, compatible fixes use
the next patch version and compatible features use the next minor version. A
development phase or internal rewrite alone does not require a new major
version.

The VA-API frontend remains experimental outside the selected v1.2 decode,
Bob and scaling contract. The following remain explicitly experimental: its
live H.264 and low-latency controls,
`CRYSTALHD_VAAPI_EXPERIMENTAL_READY_*`,
`CRYSTALHD_VAAPI_EXPERIMENTAL_I915_WC`,
`CRYSTALHD_EXPERIMENTAL_RX_POOL_COUNT`, the raw-frame producer API, and the
browser launcher. Their exact scope is in the [option index](USAGE.md#advanced-option-index)
and component guides. The optional V4L2/firmware research builds do not add a
supported frontend. A regression in an advertised opt-in path still requires
a fix or explicit withdrawal from the candidate.

## Historical v1.2 release scope and publication

The completed v1.2 release tracker is
[#473](https://github.com/ahnhy1324/crystalhd/issues/473). It defines the
bounded v1.2.0 scope and G0–G3 GO/HOLD rules. Optional performance and research
work belongs to [#472](https://github.com/ahnhy1324/crystalhd/issues/472),
#91, #92 or #204 and is not silently promoted into a release requirement.

Use the [issue triage policy](CONTRIBUTING.md#issue-triage-and-known-issue-policy)
for scheduling and scoped known limitations. These release gates and frozen
oracles take precedence; a deferred issue does not turn missing or failed
mandatory evidence into PASS.

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

For v1.2, follow **scope/diff audit and exact identity → same-SHA device-free
gates and short native smoke (G0/G1) → maintainer RC GO → publish an
immutable RC → exact-RC client, sustained and package qualification (G2/G3)
→ maintainer stable GO**. A version string or release branch is preparation,
not permission to publish an RC, tag or stable release. Published v1.1 and its
RCs remain immutable; their qualification is historical evidence, not a v1.2
PASS.

Published `v1.2.0-rc.2` completed the bounded G0–G3 qualification in #473.
Stable [v1.2.0](https://github.com/ahnhy1324/crystalhd/releases/tag/v1.2.0)
was published on 2026-10-23 at `8132043`, whose tree equals tested `f45dbdc`.
[Final package qualification and publication](https://github.com/ahnhy1324/crystalhd/issues/473#issuecomment-6355980181)
are complete. Fresh final CI/build, archive/staging, live DKMS upgrade/RC2
rollback/reinstall and installed application checks retain their actual source
attribution; RC2 direct-live, physical and endurance evidence was reused only
for unchanged implementation. These results do not qualify later development.

The selected v1.2 claims are bounded CPU VA-VPP Bob, explicit H.264 card-output
scaling and reviewed reliability fixes. Selected Kodi Auto, sustained 720p50
and RC2 package acceptance passed within the linked matrix. Hardware deinterlacing, general FHD50/60,
Atom real-time and the #470/#471 experiments are not candidate claims. Exact
boundaries and current status are in
[USAGE.md](USAGE.md#known-issues-and-deinterlacing-roadmap) and the
[hardware report](HARDWARE-2026-09-13.md#120-rc2-qualification--2026-10-22).

The freeze, qualification and publication procedures below are the retained
completed v1.2/#473 campaign. They preserve its gates and evidence; they are
not unfinished 1.3 work. Current RC2 status is above and the active queue is in
[TODO.md](TODO.md#current-priorities--2026-11-03). Future candidates need their
own scope, package identity and affected qualification.

<a id="freeze-and-prepare"></a>

## Freeze and prepare a release candidate

The following numbered steps record the completed v1.2/#473 freeze.

1. Agree on the v1.2 scope in #473. Audit the complete
   `v1.1.0...candidate` diff, link #429's compatibility results and separate
   supported production behavior from default-off research and explicitly unsupported paths.
   After freezing, accept only release fixes, documentation and qualification
   work.
2. Merge preparation changes. Check this version table, installation and
   rollback commands, notices and the curated [change summary](HISTORY.md).
   Use a short-lived `release/…` branch while unrelated development continues,
   then record one fixed candidate commit. After immutable-RC qualification,
   prepare the distinct final identity on `release/1.2.0-final` according to
   [Publish and maintain](#publish-and-maintain).
3. Run the existing device-free gates with the
   [developer dependencies](README.md#developer-checks) installed:

   ```sh
   NODE_PATH=/usr/share/nodejs make -j2 W=1 check
   make userspace32-check
   make legacy-cpu-check
   make vaapi-cpu-check
   ```

   Require completed CI success, including the older GStreamer build. Test
   a source archive without `.git`, staged install/uninstall and plugin
   discovery, and review and pin the documented direct and DKMS
   upgrade/rollback paths. Execute those live upgrade/rollback paths only in G3
   against the immutable RC. A staged layout check does not establish a live
   module installation or hardware pass.
4. Complete #473 G0/G1, including the declared short native smoke. Record the
   exact commit, source diff, selected claim matrix, limits, acknowledgements,
   release text and results, then obtain explicit maintainer RC GO before
   publishing the candidate RC. Never move published tags, including
   `v1.2.0-rc.1`, `v1.1.0`,
   `v1.1.0-rc.5`, `v1.1.0-rc.4`, `v1.1.0-rc.3`,
   `v1.0.0-rc.1`, `v1.0.0` and `phase1-baseline-2026-09-29`.

## Qualify the candidate

This matrix records #473's v1.2 qualification criteria and results.

Run G0/G1 against one exact v1.2 candidate commit before asking to publish an
RC. Begin G2/G3 only against that immutable RC after maintainer RC GO. Earlier
results remain useful baselines, but do not count as new candidate PASS results.
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
sh "$gate" evidence-check "$evidence_root" --manifest "$manifest" --maintenance-3h
```

The retained maintenance regression uses the explicit
`soak --rc --maintenance-3h` profile for 720p30 High/AAC: exactly
`--seconds 10800 --frames 324000`, with a matching complete 3-hour fixture.
Pass the normal fixture identity, runtime and new external results path as
for any soak. All other RC timing, correctness, resource, ownership and
identity checks remain in force. Select `--maintenance-3h` again for the
aggregate, as above; its distinct acceptance label is
`maintenance-soak-720p30-10800`. Without that flag, the historical Phase 1
720p soak and aggregate still require 8 hours / 864,000 pictures. A 3-hour
result is not 8-hour evidence. Native/scaled Full-HD and the other gates
retain their existing durations and counts.

RC/oracle pin-balance snapshots read `/proc/sys/vm/stat_refresh` through the
existing root/noninteractive-sudo path before sampling `/proc/vmstat`, as
[documented by Linux](https://docs.kernel.org/admin-guide/sysctl/vm.html#stat-refresh).
This flushes per-CPU accounting, not a persistent setting. Snapshots retain
the raw balance and observation method; refresh failure is unavailable
evidence, not a raw-counter fallback. Exact before/after equality is still
required, with no retry, grace period or wait for zero.

The table below records the completed v1.1 G0–G6 campaign mechanics. Reuse its
scripts and unchanged thresholds where #473 calls for them, but do not mistake
its historical results, Moonlight assessment or numbering for current v1.2
evidence. Current v1.2 GO/HOLD is governed only by #473 G0–G3; missing, stale,
failed or inconclusive mandatory evidence means **HOLD**.

### Historical v1.1 G0–G6 reference

| Gate | Declared run and acceptance |
| --- | --- |
| G0 — Recoverable reference | Before destructive candidate installation or testing: current known-good whole-system image on independent storage, verified integrity and rescue-media access, preserved configuration/source/fixtures and a concrete restore procedure. Prefer spare-disk restore-and-boot; without a spare, record `restore boot NOT TESTED` and obtain explicit operator acceptance. Integrity and recovery-media checks remain mandatory. |
| G1 — LTS admission | Actual X61/BCM70015 on the selected supported Ubuntu LTS; record exact OS/kernel/userspace/graphics. Rebuild the preserved RC first, verify loaded/installed identities and real native/scaled/VA decode, then freeze any necessary corrected candidate. Preserve comparable LXQt/X11 settings and record differences. |
| G2 — Correctness and compatibility | All 11 maintenance-oracle entries: defined counts, geometry/fields, PTS and pixels, valid EOS/drain and clean reopen. Candidate CI, native/i686/no-SSE and older supported kernel/GStreamer builds pass. Cover event wait/close/cancellation, mixed-version fallback, B-frame tail/replay, L0s restoration/force precedence and monitor-only close; distinguish mock from physical evidence. |
| G3 — Core endurance and lifecycle | Clocked H.264+AAC 720p30: **3 h / 324,000 pictures** with the explicit maintenance profile above; qualified native H.264 High 1080p30: **2 h / 216,000 pictures**; **1,000** complete sessions; **100** complete admitted-codec cycles; **10** control cycles and final idle reload/reopen. Retain full timing/resource limits, pause/resume, seek, flush/replay and reopen; exclude unsupported native FHD 2x. No recovery retries/reloads may mask failures. |
| G4 — Retained new paths | Progressive Full-HD input → 1280-wide output: **2 h**, correct geometry/PAR, count/EOS, pixel checks and seek/native-reopen controls. Actual admitted VA event-RX worker: **at least 2 h**, with output progress, bounded resources and no lost wake/stuck retirement. GStreamer-only soak cannot establish VA-worker coverage. |
| G5 — Installation and rollback | Final archive without `.git`, first install, RC/previous-package update, removal and rollback on direct/DKMS routes without mixed ownership. Fail before removal if prerequisites/builds fail; use idle-only unload and verify `crystalhd-check`, known decode/EOS and reopen. Distinguish staging from live installation. |
| G6 — Documentation and final artifact | Follow #430 and the established #369 fresh-user, update, slow-FHD, Moonlight and failed-run journeys. Check links, quoted placeholders, prerequisites, success/errors, experimental scope, component identity and notices. Stable-tag instructions become available only after publication. Final metadata/payload changes need CI/build/archive/install checks and affected runtime rechecks. |

The v1.1 G0 required a verified independent offline known-good image, integrity and
rescue access, and a defined restore path before destructive candidate
installation or testing. A live copy on the SSD being upgraded does not meet
it. Any exception requires a new explicit maintainer decision recorded in
#473 for the v1.2 campaign; earlier exceptions are not inherited.

The following Moonlight assessment was part of the historical v1.1 campaign,
not a v1.2 RC requirement. It required **60 minutes** exact 720p30
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

This is the completed v1.2/#473 publication sequence.

After #473 records G2/G3 evidence and dispositions for the immutable RC,
freeze final v1.2.0 metadata and a package revision distinct from any changed
registered RC payload. Account for bundle VERSION data compiled into the
module; verify the actual final CI/build/archive/install and upgrade/rollback
payload, and rerun affected runtime gates. Present the exact final commit,
results, release text, remaining limits and GO/HOLD row for maintainer
approval. Create an annotated `v1.2.0` tag and a normal GitHub release only
after explicit stable GO. Follow
[GitHub's release procedure](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).

The release body should contain highlights, install/update/rollback,
LTS validated scope, experimental options, known limits, evidence and
acknowledgements, in that order. Use `HISTORY.md` as the starting summary. Offer source tag
archives first; retain required notices. If optional binary assets are added,
publish their checksums. Smoke-test the public archive and its installation
commands, then link the result in #473. External announcements need separate
approval. Keep #91/#92/#204 open for their own remaining objectives.

Retain the pre-LTS recovery evidence and a separately verified LTS known-good
image. Keep images and private inventories off GitHub. After v1.2 stable
publication, compatible fixes inform 1.2.1; added compatible features inform
the next minor version. Evidence alone need not trigger a new release. The
published v1.0/v1.1 tags, release notes and qualification records remain
unchanged.

## Branch and issue maintenance

Integrate through task branches and PRs. Require the existing `CI gate` on the
current base plus affected validation and independent review. Master protection
applies to administrators and rejects direct pushes, force pushes and deletion;
keep it enabled while preparing releases. Automatic branch deletion stays off.

Use an issue for a distinct unresolved objective or reproduced defect; use a
PR directly for a small self-contained fix. Name short-lived branches
`fix/…`, `feature/…`, `docs/…` or `research/…` and record their issue/owner in
the PR. Before any authorized cleanup, check current worktrees and PRs in this
fork and upstream repositories. Integration into our master does not end an
upstream submission. Preserve all `upstream/` submission branches, including
`upstream/01-linux-6.1`, `upstream/02-userspace` and `upstream/03-chromium`.
Do not delete, rewrite or reuse them without an explicit maintainer instruction
for the specific branch; a backup does not replace a live PR head. Follow
[AGENTS.md](AGENTS.md#upstream-pr-branches) and [CONTRIBUTING.md](CONTRIBUTING.md#pull-requests).
Preserve unique research fixtures and failures before removing any branch;
retain unmerged V4L2 PR #115 and its stateful-contract failure evidence.
Keep usage in the guides, results in the hardware report, and current work in
issues. Hardware support claims require hardware evidence.

## Useful bug reports

Include the release tag, sanitized `crystalhd-check` output, card/chipset,
kernel/userspace, player/API and options, reproducible codec/profile/geometry,
relevant error logs, and whether rollback or default frame matching changes
the result. Credentials, pairing secrets and private media are unnecessary.
